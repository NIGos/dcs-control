// ---------------------------------------------------------------------------
// Geometry
// ---------------------------------------------------------------------------

struct Vec3 {
  double x, y, z;
};
inline Vec3 operator+(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline Vec3 operator-(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline Vec3 operator*(Vec3 a, double s) { return {a.x * s, a.y * s, a.z * s}; }
inline double Dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline double Len(Vec3 a) { return std::sqrt(Dot(a, a)); }
inline Vec3 Cross(Vec3 a, Vec3 b) {
  return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
inline Vec3 Normalize(Vec3 a) { return a * (1.0 / Len(a)); }

struct Plane {
  Vec3 n;  // unit, pointing inside
  double d;
};

double Clamp(double v, double lo, double hi) { return v < lo ? lo : (v > hi ? hi : v); }

// A view frustum decoded from a ClippingVolume plane block.
struct Frustum {
  bool valid = false;
  Plane nearP{}, farP{};
  bool hasFar = false;
  Plane sides[4]{};
  Vec3 apex{}, forward{};
  double halfAngle[4]{};  // radians, angle between the image centre and each corner
  double meanHalfAngle = 0;
  // Image-plane rectangle at distance 1 along `forward`, relative to the apex,
  // in winding order. Quad-Views-Foveated's texture coordinates are linear in it.
  Vec3 corners[4]{};
  Vec3 center{};     // centre of that rectangle (relative to apex)
  Vec3 centerDir{};  // unit direction through the image centre
};

bool ReadPlanes(const uint8_t* block, std::vector<Plane>& out) {
  int count = *reinterpret_cast<const int32_t*>(block + kPlaneCount);
  if (count < 5 || count > kMaxPlanes) return false;
  out.clear();
  for (int i = 0; i < count; ++i) {
    const double* p = reinterpret_cast<const double*>(block + i * kPlaneStride);
    Vec3 n{p[0], p[1], p[2]};
    double len = Len(n);
    if (!(len > 1e-9) || !std::isfinite(p[3])) return false;
    out.push_back({n * (1.0 / len), p[3] / len});
  }
  return true;
}

bool SolveApex(const Plane* sides, int n, Vec3& apex) {
  // Least squares point closest to all side planes: (sum n n^T) x = -sum d n.
  double a[3][3] = {};
  double b[3] = {};
  for (int i = 0; i < n; ++i) {
    const double v[3] = {sides[i].n.x, sides[i].n.y, sides[i].n.z};
    for (int r = 0; r < 3; ++r) {
      for (int c = 0; c < 3; ++c) a[r][c] += v[r] * v[c];
      b[r] -= sides[i].d * v[r];
    }
  }
  double det = a[0][0] * (a[1][1] * a[2][2] - a[1][2] * a[2][1]) -
               a[0][1] * (a[1][0] * a[2][2] - a[1][2] * a[2][0]) +
               a[0][2] * (a[1][0] * a[2][1] - a[1][1] * a[2][0]);
  if (std::fabs(det) < 1e-9) return false;
  auto detWith = [&](int col) {
    double m[3][3];
    memcpy(m, a, sizeof(m));
    for (int r = 0; r < 3; ++r) m[r][col] = b[r];
    return m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) -
           m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) +
           m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
  };
  apex = {detWith(0) / det, detWith(1) / det, detWith(2) / det};
  return std::isfinite(apex.x) && std::isfinite(apex.y) && std::isfinite(apex.z);
}

Frustum DecodeFrustum(const uint8_t* block) {
  Frustum f;
  std::vector<Plane> planes;
  if (!ReadPlanes(block, planes)) return f;
  const int count = static_cast<int>(planes.size());

  // Near and far planes face each other; with no far plane, the near plane is
  // the one whose normal is closest to the average of all normals.
  int ni = -1, fi = -1;
  double best = 0.0;
  for (int i = 0; i < count; ++i)
    for (int j = i + 1; j < count; ++j) {
      double dd = Dot(planes[i].n, planes[j].n);
      if (dd < best) {
        best = dd;
        ni = i;
        fi = j;
      }
    }
  if (best > -0.99) return f;  // no opposing pair: not a perspective frustum we understand

  int sideIdx[kMaxPlanes];
  int ns = 0;
  for (int i = 0; i < count; ++i)
    if (i != ni && i != fi) sideIdx[ns++] = i;
  if (ns != 4) return f;
  for (int i = 0; i < 4; ++i) f.sides[i] = planes[sideIdx[i]];
  if (!SolveApex(f.sides, 4, f.apex)) return f;

  // The near plane has the apex behind it (negative side).
  Plane a = planes[ni], b = planes[fi];
  if (Dot(a.n, f.apex) + a.d > Dot(b.n, f.apex) + b.d) std::swap(a, b);
  f.nearP = a;
  f.farP = b;
  f.hasFar = true;
  f.forward = a.n;

  // Side planes must pass through the apex. They may face away from the
  // forward axis: an off-centre focus view (gaze far from the eye axis) has
  // an asymmetric frustum entirely on one side of it.
  for (int i = 0; i < 4; ++i)
    if (std::fabs(Dot(f.sides[i].n, f.apex) + f.sides[i].d) > 0.05) return f;

  // Frustum edges: intersection lines of adjacent side planes, i.e. those that
  // point forward and lie inside the two remaining side planes.
  int nc = 0;
  for (int i = 0; i < 4; ++i)
    for (int j = i + 1; j < 4; ++j) {
      Vec3 d = Cross(f.sides[i].n, f.sides[j].n);
      double l = Len(d);
      if (l < 1e-9) continue;
      d = d * (1.0 / l);
      if (Dot(d, f.forward) < 0) d = d * -1.0;
      if (Dot(d, f.forward) < 1e-3) continue;
      bool inside = true;
      for (int k = 0; k < 4; ++k)
        if (k != i && k != j && Dot(f.sides[k].n, d) < -1e-6) inside = false;
      if (!inside) continue;
      if (nc == 4) return f;
      f.corners[nc++] = d * (1.0 / Dot(d, f.forward));
    }
  if (nc != 4) return f;
  f.center = (f.corners[0] + f.corners[1] + f.corners[2] + f.corners[3]) * 0.25;
  // Sort corners by angle around the centre so consecutive ones share an edge.
  Vec3 e1 = Normalize(f.corners[0] - f.center);
  Vec3 e2 = Cross(f.forward, e1);
  double ang[4];
  for (int i = 0; i < 4; ++i) {
    Vec3 v = f.corners[i] - f.center;
    ang[i] = std::atan2(Dot(v, e2), Dot(v, e1));
  }
  for (int i = 0; i < 4; ++i)
    for (int j = i + 1; j < 4; ++j)
      if (ang[j] < ang[i]) {
        std::swap(ang[i], ang[j]);
        std::swap(f.corners[i], f.corners[j]);
      }
  f.centerDir = Normalize(f.center);
  double sum = 0;
  for (int i = 0; i < 4; ++i) {
    f.halfAngle[i] = std::acos(Clamp(Dot(Normalize(f.corners[i]), f.centerDir), -1.0, 1.0));
    sum += f.halfAngle[i];
  }
  f.meanHalfAngle = sum / 4.0;  // mean angle from the image centre to the corners
  f.valid = true;
  return f;
}

void WritePlane(uint8_t* block, int idx, const Plane& p) {
  uint8_t* dst = block + idx * kPlaneStride;
  double* d = reinterpret_cast<double*>(dst);
  d[0] = p.n.x;
  d[1] = p.n.y;
  d[2] = p.n.z;
  d[3] = p.d;
  int pmask = (p.n.x >= 0 ? 1 : 0) | (p.n.y >= 0 ? 2 : 0) | (p.n.z >= 0 ? 4 : 0);
  *reinterpret_cast<int32_t*>(dst + kPlaneMaskP) = pmask;
  *reinterpret_cast<int32_t*>(dst + kPlaneMaskN) = (~pmask) & 7;
}

// Builds a plane block for frustum `f` with its image rectangle scaled by
// `keep` about its centre (keep = 1: the full frustum).
void BuildExclusionBlock(uint8_t* block, const Frustum& f, double keep) {
  memset(block, 0, kPlaneBlockSize);
  int idx = 0;
  WritePlane(block, idx++, f.nearP);
  if (f.hasFar) WritePlane(block, idx++, f.farP);
  Vec3 p[4];
  for (int i = 0; i < 4; ++i) p[i] = f.center + (f.corners[i] - f.center) * keep;
  for (int i = 0; i < 4; ++i) {
    Vec3 n = Normalize(Cross(p[i], p[(i + 1) & 3]));
    if (Dot(n, f.center) < 0) n = n * -1.0;
    WritePlane(block, idx++, Plane{n, -Dot(n, f.apex)});
  }
  *reinterpret_cast<int32_t*>(block + kPlaneCount) = idx;
  *reinterpret_cast<int32_t*>(block + kPlaneExtraCount) = 0;
}

// ---------------------------------------------------------------------------
// Persistent replacement volumes
// ---------------------------------------------------------------------------

// Each patched view gets its own long-lived ClippingVolume copies. They are
// never freed: a renderer thread may still hold a pointer from an earlier
// frame, so the memory must stay valid for the life of the process.
struct VolumeSlot {
  uint8_t* buf[2] = {};
  uint32_t flip = 0;
};

std::mutex g_slotMutex;
std::unordered_map<uint64_t, VolumeSlot> g_slots;

uint8_t* AcquireVolume(uint64_t key) {
  std::lock_guard<std::mutex> lock(g_slotMutex);
  VolumeSlot& s = g_slots[key];
  if (!s.buf[0] || !s.buf[1]) {
    // Checked: a failed allocation used to fault in memset with g_slotMutex
    // held; SehPrepare's __except does not unwind the lock_guard, so the next
    // collect call deadlocked the culling thread. Null = view not patched.
    for (auto& b : s.buf) {
      if (!b) b = static_cast<uint8_t*>(_aligned_malloc(0x700, 64));
      if (!b) return nullptr;
      memset(b, 0, 0x700);
    }
  }
  s.flip ^= 1;
  return s.buf[s.flip];
}

// Copies `src` into `dst` and appends `extra` to its exclusion list. Returns
// false when the source already has a full inline exclusion list.
bool MakeVolumeWithExclusion(uint8_t* dst, const uint8_t* src, const uint8_t* extra) {
  uint64_t srcSize = *reinterpret_cast<const uint64_t*>(src + kVolExclSize);
  const uint8_t* srcData = *reinterpret_cast<uint8_t* const*>(src + kVolExclPtr);
  if (srcSize >= static_cast<uint64_t>(kVolExclInlineCap)) return false;
  if (srcSize > 0 && !srcData) return false;

  memcpy(dst, src, kVolExclPtr);
  uint8_t* inl = dst + kVolExclInline;
  for (uint64_t i = 0; i < srcSize; ++i)
    memcpy(inl + i * kPlaneBlockSize, srcData + i * kPlaneBlockSize, kPlaneBlockSize);
  memcpy(inl + srcSize * kPlaneBlockSize, extra, kPlaneBlockSize);
  *reinterpret_cast<uint8_t**>(dst + kVolExclPtr) = inl;
  *reinterpret_cast<uint64_t*>(dst + kVolExclCap) = kVolExclInlineCap;
  *reinterpret_cast<uint64_t*>(dst + kVolExclSize) = srcSize + 1;
  return true;
}

// ---------------------------------------------------------------------------
// Statistics
// ---------------------------------------------------------------------------

struct Stats {
  std::atomic<uint64_t> calls{0};
  std::atomic<uint64_t> quadCalls{0};     // calls with >= 2 focus/peripheral pairs
  std::atomic<uint64_t> patchedViews{0};
  std::atomic<uint64_t> usQuad{0};        // time spent in quad calls
  std::atomic<uint64_t> periphRenderables{0};
  std::atomic<uint64_t> focusRenderables{0};
  std::atomic<uint64_t> usAll{0};
  std::atomic<uint64_t> maxViews{0};
};
Stats g_stats;
std::atomic<int> g_dumpsLeft{0};
double g_qpcToUs = 0;
std::atomic<int64_t> g_firstQuadQpc{0};

