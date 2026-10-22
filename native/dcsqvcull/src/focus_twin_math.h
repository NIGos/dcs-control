// R123 focus twin: the pure parts (no hook, no device, no DCS memory). docs/research/R123_focus_twin.md.
//
//  - TerrainContext (VS/PS b11, 2176 bytes, edterrainGraphics41+0x118116's upload) rows and view identification
//    (R53 / R120b: P rows 16 / 17 = (sx, 0, cx, 0), (0, sy, cy, 0), row 19 = (0, 0, 1, 0); sx < 1.5 = periphery,
//    else focus; the eye is the sign of cx; row 44 = (0, 0, render w, render h); row 38 = the eye position).
//  - The focus prediction: this frame's focus TerrainContext predicted at the periphery pass from the periphery's
//    actual upload, this frame's collect-time cameras of both views (CollectionInfo view-projections, published by
//    the collect hook before the render graph runs: the eye-tracked gaze is in them) and the previous frame's
//    relation between the focus camera and the focus upload (the DLSS jitter and any culling margin), with the
//    jitter step equal in pixels in both views of an eye (R53). Validated against DCS's actual focus upload.
//  - Descriptor map, chunk arithmetic, the island rule, the statistics text.
// Included by focus_twin.h; header-only, C++17, <cmath> / <cstring> / <cstdint> / <cstdio> only.
#pragma once

namespace ftwin {

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------
constexpr int kTcBytes = 2176;  // TerrainContext (b11)
constexpr int kTcRows = kTcBytes / 16;
constexpr int kB10Bytes = 1280;  // Visualizer view block (b10)
constexpr int kPerView = 4;
constexpr unsigned kPerViewSlot[kPerView] = {5, 7, 10, 11};  // b5, b7 cPerView, b10, b11 TerrainContext (R120b 1.2)
// TerrainContext rows that differ between the periphery and the focus of one eye (census R26 11, frames 0-1, both
// eyes: R123 scratch tcrows.py): P x / y, VP x / y, inverse P x / y, the four side planes, the render size, and
// row 132 (a per-view-type pair: periphery (2^65, 1.820), focus (2.0, 2.037)). Every other row is byte-equal.
constexpr int kRowPx = 16, kRowPy = 17, kRowPw = 19, kRowVx = 20, kRowVy = 21, kRowVw = 23, kRowIx = 24,
              kRowIy = 25, kRowPlane0 = 28, kRowCam = 38, kRowSize = 44, kRow132 = 132;
inline bool PerViewRow(int r) {
  return r == kRowPx || r == kRowPy || r == kRowVx || r == kRowVy || r == kRowIx || r == kRowIy ||
         (r >= kRowPlane0 && r < kRowPlane0 + 4) || r == kRowSize || r == kRow132;
}

inline uint64_t Fnv1a(const void* p, size_t n, uint64_t h = 0xcbf29ce484222325ull) {
  const uint8_t* b = static_cast<const uint8_t*>(p);
  for (size_t i = 0; i < n; ++i) {
    h ^= b[i];
    h *= 0x100000001b3ull;
  }
  return h;
}
inline uint64_t Mix64(uint64_t h, uint64_t v) { return Fnv1a(&v, sizeof(v), h); }

inline const float* TcRow(const uint8_t* tc, int r) { return reinterpret_cast<const float*>(tc + 16 * r); }

// ---------------------------------------------------------------------------
// View identification by the TerrainContext
// ---------------------------------------------------------------------------
enum Kind : int { kOther = 0, kPeriph = 1, kFocus = 2 };
struct ViewId {
  int kind = kOther;
  int eye = -1;  // periphery: 0 = cx > 0, 1 = cx < 0. Focus: -1 (its centre follows the gaze and can sit on either
                 // side; the eye is the periphery of this frame whose shared rows it has, EyeOfFocus)
  double sx = 0, sy = 0, cx = 0, cy = 0, w = 0, h = 0;
};
// A perspective QV view: P rows of the R53 shape, a render size. Periphery: sx < 1.5 and an off-centre x (the eyes'
// asymmetric frusta, |cx| ~0.19; a centred wide camera, e.g. an MFD sensor view, is "other"). Focus: sx >= 1.5 (any
// centre); a narrow camera that is no quad view (an MFD zoom) matches no periphery's shared rows and is dropped there.
inline ViewId ClassifyTc(const uint8_t* tc) {
  ViewId v;
  if (!tc) return v;
  const float* x = TcRow(tc, kRowPx);
  const float* y = TcRow(tc, kRowPy);
  const float* w = TcRow(tc, kRowPw);
  const float* s = TcRow(tc, kRowSize);
  if (!(x[0] > 0 && y[1] > 0 && x[1] == 0 && x[3] == 0 && y[0] == 0 && y[3] == 0 && w[0] == 0 && w[1] == 0 &&
        w[2] == 1 && w[3] == 0))
    return v;
  if (!(s[2] >= 16 && s[3] >= 16 && s[2] <= 16384 && s[3] <= 16384)) return v;
  v.sx = x[0], v.cx = x[2], v.sy = y[1], v.cy = y[2], v.w = s[2], v.h = s[3];
  if (!std::isfinite(v.sx) || !std::isfinite(v.cx) || !std::isfinite(v.sy) || !std::isfinite(v.cy)) return v;
  if (v.sx >= 1.5) {
    v.kind = kFocus;
    return v;
  }
  if (std::fabs(v.cx) < 0.02) return v;
  v.eye = v.cx > 0 ? 0 : 1;
  v.kind = kPeriph;
  return v;
}
// The rows every view of one eye shares (camera, z / w, terrain parameters) are byte-equal.
inline bool SharedRowsEqual(const uint8_t* p, const uint8_t* f, int* firstDiff = nullptr) {
  for (int r = 0; r < kTcRows; ++r) {
    if (PerViewRow(r)) continue;
    if (memcmp(p + 16 * r, f + 16 * r, 16) != 0) {
      if (firstDiff) *firstDiff = r;
      return false;
    }
  }
  return true;
}

// ---------------------------------------------------------------------------
// Affine relations between clip rows (first three components: rotation x projection; the fourth carries the
// origin offset, which differs between the collect-time camera-relative matrix and the uploaded one)
// ---------------------------------------------------------------------------
struct Ab {
  double a = 1, b = 0, res = 1e30;
};
// target ~ a * row + b * wrow (least squares over the three components).
inline Ab SolveAb(const double* row, const double* wrow, const double* target) {
  double s_rr = 0, s_rw = 0, s_ww = 0, s_rt = 0, s_wt = 0, n_t = 0;
  for (int k = 0; k < 3; ++k) {
    s_rr += row[k] * row[k], s_rw += row[k] * wrow[k], s_ww += wrow[k] * wrow[k];
    s_rt += row[k] * target[k], s_wt += wrow[k] * target[k], n_t += target[k] * target[k];
  }
  Ab o;
  const double det = s_rr * s_ww - s_rw * s_rw;
  if (!(std::fabs(det) > 1e-18)) return o;
  o.a = (s_rt * s_ww - s_wt * s_rw) / det;
  o.b = (s_wt * s_rr - s_rt * s_rw) / det;
  double r = 0;
  for (int k = 0; k < 3; ++k) r = std::max(r, std::fabs(o.a * row[k] + o.b * wrow[k] - target[k]));
  o.res = r / std::max(1e-12, std::sqrt(n_t));
  return o;
}
// Clip rows (x, y, w) of a view as 3-vectors.
struct Clip3 {
  double x[3], y[3], w[3];
  bool ok = false;
};
// The uploaded TerrainContext's VP rows 20, 21, 23.
inline Clip3 TcClip(const uint8_t* tc) {
  Clip3 c;
  for (int k = 0; k < 3; ++k) {
    c.x[k] = TcRow(tc, kRowVx)[k], c.y[k] = TcRow(tc, kRowVy)[k], c.w[k] = TcRow(tc, kRowVw)[k];
  }
  c.ok = true;
  return c;
}
// A CollectionInfo view-projection (osg::Matrixf, 16 floats, ci+0x198). colMajor = false: clip = v * M (the clip x
// coefficients are column 0: m[0], m[4], m[8]); true: the transpose (rows). Which one DCS stores is learned (the
// one that relates to the upload with the smaller residual) and kept for the session.
inline Clip3 CiClip(const float* m, bool rows) {
  Clip3 c;
  for (int k = 0; k < 3; ++k) {
    if (!rows) {
      c.x[k] = m[k * 4 + 0], c.y[k] = m[k * 4 + 1], c.w[k] = m[k * 4 + 3];
    } else {
      c.x[k] = m[0 * 4 + k], c.y[k] = m[1 * 4 + k], c.w[k] = m[3 * 4 + k];
    }
  }
  c.ok = true;
  for (int k = 0; k < 3; ++k)
    if (!std::isfinite(c.x[k]) || !std::isfinite(c.y[k]) || !std::isfinite(c.w[k])) c.ok = false;
  return c;
}
// NDC relation "to = A(from)" per axis: to_x = ax from_x + bx w (both views share the w row up to scale).
struct Rel {
  Ab x, y;
  double res = 1e30;
  bool ok = false;
};
inline Rel Relate(const Clip3& from, const Clip3& to) {
  Rel r;
  if (!from.ok || !to.ok) return r;
  // The w rows must agree (same camera orientation): scale-free check.
  double nf = 0, nt = 0, dot = 0;
  for (int k = 0; k < 3; ++k) nf += from.w[k] * from.w[k], nt += to.w[k] * to.w[k], dot += from.w[k] * to.w[k];
  if (!(nf > 0 && nt > 0)) return r;
  const double cosw = dot / std::sqrt(nf * nt);
  // Scale the source to the target's w (clip w = view z in both, normally equal).
  const double sc = std::sqrt(nt / nf);
  double fx[3], fy[3], fw[3];
  for (int k = 0; k < 3; ++k) fx[k] = from.x[k] * sc, fy[k] = from.y[k] * sc, fw[k] = from.w[k] * sc;
  r.x = SolveAb(fx, fw, to.x);
  r.y = SolveAb(fy, fw, to.y);
  r.res = std::max(std::max(r.x.res, r.y.res), 1.0 - cosw);
  r.ok = r.res < 1e-3 && r.x.a > 0 && r.y.a > 0;
  return r;
}

// Collect-time cameras of one frame (published by the collect hook).
struct CiFrame {
  uint64_t seq = 0;  // publication counter (0 = none)
  int pairs = 0;
  float p[2][16] = {}, f[2][16] = {};  // periphery / focus view-projection per pair
};

// Per eye, what the previous frame taught: the focus camera's relation to the focus upload (x: a, b; y: a, b), the
// periphery's (b only, for the jitter step), the render sizes and the focus rows the prediction carries over.
struct EyeHist {
  bool have = false;
  uint64_t ciSeq = 0;          // the publication it was learned from
  double fa[2] = {1, 1}, fb[2] = {0, 0};  // focus: actual = fa * ci + fb * w
  double pa[2] = {1, 1}, pb[2] = {0, 0};  // periphery: actual = pa * ci + pb * w (at that frame)
  double wp = 0, hp = 0, wf = 0, hf = 0;
  uint8_t row44[16] = {}, row132[16] = {};
  bool rows = false;           // CiClip layout
};

// The predicted focus TerrainContext: every row, and how it is compared.
enum RowMode : uint8_t { kRowExact = 1, kRowNear = 2 };
struct Pred {
  bool ok = false;
  int why = 0;  // PredWhy when !ok
  uint8_t tc[kTcBytes] = {};
  uint8_t mode[kTcRows] = {};
  double ax = 0, bx = 0, ay = 0, by = 0;  // focus = a * periphery + b * w (actual uploads)
};
enum PredWhy : int {
  kPwOk = 0,
  kPwNoCi,       // no collect-time camera for this frame (F10 / map / loading: no culling ran)
  kPwNoPair,     // no published pair relates to this periphery upload
  kPwNoHist,     // the eye's focus was not seen in the previous frame (first frame, a gap)
  kPwSize,       // the render sizes changed
  kPwBadAffine,  // the camera relation is not an affine of the expected form
  kPwCount
};
inline const char* PredWhyName(int w) {
  static const char* const n[kPwCount] = {"ok", "no camera this frame", "no camera pair", "no history",
                                          "render size", "camera relation"};
  return w >= 0 && w < kPwCount ? n[w] : "?";
}

inline void PutRow(uint8_t* tc, int r, double a, double b, double c, double d) {
  float* o = reinterpret_cast<float*>(tc + 16 * r);
  o[0] = static_cast<float>(a), o[1] = static_cast<float>(b), o[2] = static_cast<float>(c), o[3] = static_cast<float>(d);
}

// A collect-time camera projects like the upload up to the jitter and a culling margin (scale within 25 %).
constexpr double kCiScaleTol = 0.25;
// Which published pair belongs to this periphery upload: the one whose periphery camera relates to it with a ~1
// (same projection up to jitter and margin) and the smallest offset. Both eyes' views are parallel, so the OTHER
// eye's camera relates exactly too, offset by the difference of the asymmetric centres (~0.39 NDC); the jitter and a
// margin about the centre leave the own eye's offset at a few 1e-3.
constexpr double kCiOffsetMax = 0.05;
inline int FindPair(const CiFrame& ci, const uint8_t* tcP, bool rows, Rel* relOut) {
  const Clip3 t = TcClip(tcP);
  int best = -1;
  double bestOff = 1e30;
  for (int i = 0; i < ci.pairs && i < 2; ++i) {
    const Rel r = Relate(CiClip(ci.p[i], rows), t);
    if (!r.ok || std::fabs(r.x.a - 1) > kCiScaleTol || std::fabs(r.y.a - 1) > kCiScaleTol) continue;
    const double off = std::fabs(r.x.b) + std::fabs(r.y.b);
    if (off > kCiOffsetMax) continue;
    if (off < bestOff) bestOff = off, best = i, *relOut = r;
  }
  return best;
}

// Predicts this frame's focus TerrainContext of the eye whose periphery uploaded tcP.
inline Pred PredictFocus(const uint8_t* tcP, const CiFrame& ci, const EyeHist& h) {
  Pred pr;
  if (!ci.seq || ci.pairs < 1) return pr.why = kPwNoCi, pr;
  if (!h.have) return pr.why = kPwNoHist, pr;
  Rel tp;
  const int pi = FindPair(ci, tcP, h.rows, &tp);
  if (pi < 0) return pr.why = kPwNoPair, pr;
  const ViewId vp = ClassifyTc(tcP);
  if (vp.w != h.wp || vp.h != h.hp) return pr.why = kPwSize, pr;
  const Rel cf = Relate(CiClip(ci.p[pi], h.rows), CiClip(ci.f[pi], h.rows));  // focus camera from periphery camera
  if (!cf.ok) return pr.why = kPwBadAffine, pr;
  // This frame's focus-camera-to-upload relation: last frame's scale, offset moved by the periphery's jitter step
  // in focus NDC (same pixels: x Wp / Wf).
  const double fbx = h.fb[0] + (tp.x.b - h.pb[0]) * h.wp / h.wf;
  const double fby = h.fb[1] + (tp.y.b - h.pb[1]) * h.hp / h.hf;
  // focus = fa * (ca * ci_p + cb * w) + fb * w, ci_p = (actual_p - pb * w) / pa
  pr.ax = h.fa[0] * cf.x.a / tp.x.a;
  pr.bx = h.fa[0] * cf.x.b + fbx - h.fa[0] * cf.x.a * tp.x.b / tp.x.a;
  pr.ay = h.fa[1] * cf.y.a / tp.y.a;
  pr.by = h.fa[1] * cf.y.b + fby - h.fa[1] * cf.y.a * tp.y.b / tp.y.a;
  if (!(pr.ax > 1 && pr.ay > 1 && std::isfinite(pr.bx) && std::isfinite(pr.by))) return pr.why = kPwBadAffine, pr;
  memcpy(pr.tc, tcP, kTcBytes);
  for (int r = 0; r < kTcRows; ++r) pr.mode[r] = kRowExact;
  const float* px = TcRow(tcP, kRowPx);
  const float* py = TcRow(tcP, kRowPy);
  const double sx = pr.ax * px[0], cx = pr.ax * px[2] + pr.bx;
  const double sy = pr.ay * py[1], cy = pr.ay * py[2] + pr.by;
  PutRow(pr.tc, kRowPx, sx, 0, cx, 0);
  PutRow(pr.tc, kRowPy, 0, sy, cy, 0);
  PutRow(pr.tc, kRowIx, 1.0 / sx, 0, 0, -cx / sx);
  PutRow(pr.tc, kRowIy, 0, 1.0 / sy, 0, -cy / sy);
  const float* vx = TcRow(tcP, kRowVx);
  const float* vy = TcRow(tcP, kRowVy);
  const float* vw = TcRow(tcP, kRowVw);
  double X[4], Y[4], W[4];
  for (int k = 0; k < 4; ++k) {
    X[k] = pr.ax * vx[k] + pr.bx * vw[k];
    Y[k] = pr.ay * vy[k] + pr.by * vw[k];
    W[k] = vw[k];
  }
  PutRow(pr.tc, kRowVx, X[0], X[1], X[2], X[3]);
  PutRow(pr.tc, kRowVy, Y[0], Y[1], Y[2], Y[3]);
  // Side planes: normalised (w + x), (w - x), (w + y), (w - y) (checked against the census: row 28 = w + x).
  const double* sides[4][2] = {{W, X}, {W, X}, {W, Y}, {W, Y}};
  for (int i = 0; i < 4; ++i) {
    const double sgn = (i & 1) ? -1.0 : 1.0;
    double p[4];
    for (int k = 0; k < 4; ++k) p[k] = sides[i][0][k] + sgn * sides[i][1][k];
    const double n = std::sqrt(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]);
    if (!(n > 0)) return pr.why = kPwBadAffine, pr;
    PutRow(pr.tc, kRowPlane0 + i, p[0] / n, p[1] / n, p[2] / n, p[3] / n);
  }
  memcpy(pr.tc + 16 * kRowSize, h.row44, 16);
  memcpy(pr.tc + 16 * kRow132, h.row132, 16);
  for (int r : {kRowPx, kRowPy, kRowIx, kRowIy, kRowVx, kRowVy, kRowPlane0, kRowPlane0 + 1, kRowPlane0 + 2,
                kRowPlane0 + 3})
    pr.mode[r] = kRowNear;
  pr.ok = true;
  return pr;
}

// The prediction against DCS's actual focus upload: exact rows byte-equal, near rows within tol relative to the
// row's largest component (P / VP / planes; the prediction runs through float camera matrices). 0 = match, else
// 1 + the first row that misses; *dev = the largest relative deviation of the near rows.
inline int ValidateFocus(const Pred& pr, const uint8_t* actual, double tol, double* dev) {
  double worst = 0;
  int miss = 0;
  for (int r = 0; r < kTcRows; ++r) {
    if (pr.mode[r] == kRowExact) {
      if (memcmp(pr.tc + 16 * r, actual + 16 * r, 16) != 0 && !miss) miss = 1 + r;
      continue;
    }
    const float* a = TcRow(pr.tc, r);
    const float* b = TcRow(actual, r);
    // Scale: the row's largest direction component (x, y, z); the fourth (origin offset / plane distance) is
    // judged against itself when larger.
    double sc = 1e-6;
    for (int k = 0; k < 3; ++k) sc = std::max(sc, std::fabs(static_cast<double>(b[k])));
    for (int k = 0; k < 4; ++k) {
      const double s = k < 3 ? sc : std::max(sc, std::fabs(static_cast<double>(b[k])));
      const double d = std::fabs(static_cast<double>(a[k]) - b[k]) / s;
      if (!(d <= 1e30)) worst = 1e30;
      worst = std::max(worst, d);
      if (!(d <= tol) && !miss) miss = 1 + r;
    }
  }
  if (dev) *dev = worst;
  return miss;
}

// After the focus pass of an eye: what the next frame's prediction needs. False when the focus upload does not
// relate to this frame's cameras (then the history is dropped: the next frame of this eye is stock).
inline bool LearnFocus(EyeHist& h, const uint8_t* tcP, const uint8_t* tcF, const CiFrame& ci) {
  h.have = false;
  if (!ci.seq || ci.pairs < 1) return false;
  for (int lay = 0; lay < 2; ++lay) {
    const bool rows = lay == 1;
    Rel tp;
    const int pi = FindPair(ci, tcP, rows, &tp);
    if (pi < 0) continue;
    const Rel tf = Relate(CiClip(ci.f[pi], rows), TcClip(tcF));
    if (!tf.ok || std::fabs(tf.x.a - 1) > kCiScaleTol || std::fabs(tf.y.a - 1) > kCiScaleTol) continue;
    const ViewId vp = ClassifyTc(tcP), vf = ClassifyTc(tcF);
    if (vp.kind != kPeriph || vf.kind != kFocus || !SharedRowsEqual(tcP, tcF)) return false;
    h.rows = rows;
    h.fa[0] = tf.x.a, h.fb[0] = tf.x.b, h.fa[1] = tf.y.a, h.fb[1] = tf.y.b;
    h.pa[0] = tp.x.a, h.pb[0] = tp.x.b, h.pa[1] = tp.y.a, h.pb[1] = tp.y.b;
    h.wp = vp.w, h.hp = vp.h, h.wf = vf.w, h.hf = vf.h;
    memcpy(h.row44, tcF + 16 * kRowSize, 16);
    memcpy(h.row132, tcF + 16 * kRow132, 16);
    h.ciSeq = ci.seq;
    h.have = true;
    return true;
  }
  return false;
}

// ---------------------------------------------------------------------------
// Descriptor map (open addressing, power-of-two capacity; value = item index, -1 = empty)
// ---------------------------------------------------------------------------
struct DescMap {
  static constexpr uint32_t kCap = 1u << 13;  // items per periphery pass: 8192 / 2 load
  uint64_t key[kCap];
  int32_t val[kCap];
  uint32_t n = 0;
  void Clear() {
    for (uint32_t i = 0; i < kCap; ++i) val[i] = -1;
    n = 0;
  }
  // False when full or the key is already there (a duplicate descriptor: both items become ambiguous).
  bool Put(uint64_t k, int32_t v, int32_t* dupOf) {
    if (n >= kCap / 2) return false;
    uint32_t i = static_cast<uint32_t>(k * 0x9e3779b97f4a7c15ull >> 51) & (kCap - 1);
    for (;;) {
      if (val[i] < 0) {
        key[i] = k, val[i] = v, ++n;
        return true;
      }
      if (key[i] == k) {
        if (dupOf) *dupOf = val[i];
        return false;
      }
      i = (i + 1) & (kCap - 1);
    }
  }
  int32_t Get(uint64_t k) const {
    uint32_t i = static_cast<uint32_t>(k * 0x9e3779b97f4a7c15ull >> 51) & (kCap - 1);
    for (;;) {
      if (val[i] < 0) return -1;
      if (key[i] == k) return val[i];
      i = (i + 1) & (kCap - 1);
    }
  }
};

// ---------------------------------------------------------------------------
// Island rule: a focus item is replaced by the twin only when, in the previous frame of that eye, it was part of a
// run of at least `island` consecutive matched, twinnable items with no other work in between (so one command list
// execution, ~10 us, covers enough DCS item work, ~0.5-1 us each). Runs are measured every frame, twin or stock.
// ---------------------------------------------------------------------------
struct RunTracker {
  static constexpr uint32_t kMax = 4096;
  uint64_t cur[kMax];
  uint32_t curN = 0;
  uint64_t good[kMax];  // the descriptors of runs >= island, this frame so far
  uint32_t goodN = 0;
  void Begin() { curN = goodN = 0; }
  void Add(uint64_t d) {
    if (curN < kMax) cur[curN++] = d;
  }
  void Break(uint32_t island) {
    if (curN >= island)
      for (uint32_t i = 0; i < curN && goodN < kMax; ++i) good[goodN++] = cur[i];
    curN = 0;
  }
};
struct GoodSet {  // last frame's good descriptors (open addressing)
  static constexpr uint32_t kCap = 1u << 13;
  uint64_t key[kCap];
  uint8_t used[kCap];
  uint32_t n = 0;
  void Clear() {
    memset(used, 0, sizeof(used));
    n = 0;
  }
  void Put(uint64_t k) {
    if (n >= kCap / 2) return;
    uint32_t i = static_cast<uint32_t>(k * 0x9e3779b97f4a7c15ull >> 51) & (kCap - 1);
    while (used[i]) {
      if (key[i] == k) return;
      i = (i + 1) & (kCap - 1);
    }
    used[i] = 1, key[i] = k, ++n;
  }
  bool Has(uint64_t k) const {
    uint32_t i = static_cast<uint32_t>(k * 0x9e3779b97f4a7c15ull >> 51) & (kCap - 1);
    while (used[i]) {
      if (key[i] == k) return true;
      i = (i + 1) & (kCap - 1);
    }
    return false;
  }
  void From(const RunTracker& t) {
    Clear();
    for (uint32_t i = 0; i < t.goodN; ++i) Put(t.good[i]);
  }
};

// Records per command list (chunk) of a periphery stream.
inline int ChunkOf(int rec, int chunk) { return rec / chunk; }

// ---------------------------------------------------------------------------
// Focus-view outcome reasons (one per focus pass of an eye; the 30 s line counts them)
// ---------------------------------------------------------------------------
enum Why : int {
  kWTwin = 0,       // the twin drew this focus pass's terrain patches
  kWOff,            // feature off / not trusted yet (counted separately) -- placeholder, unused in the line
  kWNoPeriph,       // no periphery record of this eye in this frame
  kWStale,          // the periphery record or the cameras are from another frame (F10, map, loading)
  kWPred,           // prediction unavailable (PredWhy)
  kWMismatch,       // prediction != DCS's focus upload
  kWShared,         // shared rows differ (another camera / eye)
  kWNoTc,           // no TerrainContext upload seen before the first patch
  kWTargets,        // the focus targets / viewports differ from the command lists'
  kWLate,           // the command lists were not ready
  kWBuild,          // the command lists failed to build (device, no records)
  kWTrust,          // fewer passed audits than required
  kWAudit,          // this pass was an audit (stock, compared)
  kWEyeBad,         // the periphery pass of this eye was not usable (shared write, conflict, fault)
  kWShadow,         // [Suite] FocusTwinShadow: measure only
  kWNoItems,        // nothing matched
  kWVerify,         // [Suite] FocusTwinVerify: this pass ran twice (stock, then the twin) and was compared
  kWCount
};
inline const char* WhyName(int w) {
  static const char* const n[kWCount] = {"twin",    "off",          "no periphery", "stale frame",
                                         "no prediction", "prediction miss", "other camera", "no TC upload",
                                         "targets", "late",         "build",        "untrusted",
                                         "audit",   "periphery unusable", "shadow", "nothing matched", "verify"};
  return w >= 0 && w < kWCount ? n[w] : "?";
}

// Item-level reasons a focus patch item is drawn by DCS although the twin is on for the pass.
enum ItemWhy : int {
  kIUnmatched = 0,  // no periphery item with this descriptor (a per-view item, e.g. its own geometry)
  kINotTwinnable,   // the periphery item did something the twin cannot replay (see the periphery reasons)
  kIOrder,          // out of the periphery's order: the rest of the pass is stock
  kIIsland,         // in a run shorter than the island last frame
  kIDescFault,      // its descriptor could not be read
  kIAmbiguous,      // two periphery items share its descriptor
  kISubs,           // R125: an instancer item whose focus draws a sub-draw the periphery did not record
  kICount
};
inline const char* ItemWhyName(int w) {
  static const char* const n[kICount] = {"unmatched", "not twinnable", "order", "island", "descriptor", "ambiguous",
                                         "other sub-draws"};
  return w >= 0 && w < kICount ? n[w] : "?";
}
// Periphery-item reasons (recorded at the tee).
enum PerWhy : int {
  kPOk = 0,
  kPWork,       // a call other than binds, constant-buffer uploads and DrawIndexed
  kPInherit,    // a bound constant buffer was written by an earlier patch item (not self-contained)
  kPPerView,    // a per-view slot rebound
  kPStages,     // GS / HS / DS bound, or class instances
  kPPartial,    // a partial constant-buffer upload
  kPNoDraw,     // no draw
  kPFull,       // stream full
  kPNotCand,    // not recorded: not in the eye's last-frame runs (the island rule would not replace it anyway)
  kPSubs,       // R125: an instancer item whose indirect draws are not one per sub-draw with instances
  kPArgs,       // R125: an instancer item drawing from another argument buffer than the stream's first
  kPCount
};
inline const char* PerWhyName(int w) {
  static const char* const n[kPCount] = {"ok",      "other work", "inherited upload", "per-view slot",
                                         "stages",  "partial upload", "no draw",       "stream full",
                                         "not a candidate", "sub-draws", "argument buffer"};
  return w >= 0 && w < kPCount ? n[w] : "?";
}

}  // namespace ftwin
