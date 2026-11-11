// Audit B6 periphery instance census (periph_census.h, [Suite] PeriphCensus): the pure parts (per-bucket triangle
// weight, a run into the totals, the log line, the key), the kernels compile, and one census run on WARP over a
// synthetic DCS-like scene (R38's scene, several argument entries per bucket with different index counts): the
// zero-coverage instance and triangle counts against an independent CPU reference, the triangle totals against the
// arguments, DCS's buffers untouched, an inconsistent argument entry left out of the shares.
#pragma once

namespace pctest {
using namespace pcensus;

struct D3 {
  double x, y, z;
};
inline D3 operator+(D3 a, D3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline D3 operator*(D3 a, double s) { return {a.x * s, a.y * s, a.z * s}; }
inline double Dot(D3 a, D3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline double Len(D3 a) { return std::sqrt(Dot(a, a)); }
inline D3 Cross(D3 a, D3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
inline D3 Norm(D3 a) {
  const double l = Len(a);
  return l > 0 ? a * (1.0 / l) : D3{0, 0, 0};
}
inline D3 Rot(D3 v, D3 a, double t) {
  const double c = std::cos(t), s = std::sin(t);
  return v * c + Cross(a, v) * s + a * (Dot(a, v) * (1 - c));
}
struct Rng {
  uint64_t s;
  explicit Rng(uint64_t seed) : s(seed * 0x9E3779B97F4A7C15ull + 1) {}
  uint32_t U() {
    s = s * 6364136223846793005ull + 1442695040888963407ull;
    return static_cast<uint32_t>(s >> 33);
  }
  double F() { return (U() & 0xffffff) / double(0x1000000); }
  double S() { return F() * 2 - 1; }
};

// ---------------------------------------------------------------------------
// 1. Pure parts
// ---------------------------------------------------------------------------
void PureChecks() {
  {
    const Config d{};
    bool retired = false;
    for (const RetiredKey& r : kRetiredKeys)
      if (!_wcsicmp(r.sec, L"Suite") && !_wcsicmp(r.key, L"PeriphCensus")) retired = true;
    Check(d.suitePeriphCensus == 0 && !retired && !g_installed.load() && !ptiming::g_pcBoundary.load(),
          "B6 census: [Suite] PeriphCensus off by default, no longer retired, nothing installed at load");
  }
  {
    // entries: (IndexCount, bucket): bucket 1 drawn by two entries, bucket 3 by none, bucket 9 out of range
    const std::vector<uint32_t> args = {60, 5, 0, 0, 0, 0, 0, 1,  300, 2, 0, 0, 0, 0, 0, 0,
                                        31, 5, 0, 0, 0, 0, 0, 1,  90,  1, 0, 0, 0, 0, 0, 9};
    const std::vector<uint32_t> w = BucketTriCpu(args, 4);
    Check(w.size() == 4 && w[0] == 100 && w[1] == 20 + 10 && w[2] == 0 && w[3] == 0,
          "B6 census: bucket triangle weight = sum of IndexCount / 3 over the bucket's argument entries");
  }
  {
    uint32_t s[kSCount] = {};
    s[kSInst] = 1000;
    s[kSTri] = 50000;
    s[kSZeroI] = 30;
    s[kSZeroT] = 900;
    s[kSZeroNI] = 40;
    s[kSZeroNT] = 1200;
    s[kSCoreI] = 100;
    s[kSCoreT] = 7000;
    s[kSCoreOn] = 1;
    s[kSArgTriK] = 50000;
    Census c;
    Accumulate(c, s);  // clean
    uint32_t e1[kSCount];
    memcpy(e1, s, sizeof(s));
    e1[kSErr] = kErrVp;  // another camera in the G-buffer: left out entirely
    Accumulate(c, e1);
    uint32_t e2[kSCount];
    memcpy(e2, s, sizeof(s));
    e2[kSErr] = kErrEye;  // focus check failed: zero share kept, core share left out
    e2[kSCoreI] = 900;
    Accumulate(c, e2);
    Check(c.runs == 3 && c.okRuns == 2 && c.coreRuns == 1 && c.inst == 2000 && c.zeroI == 60 && c.zeroT == 1800 &&
              c.coreI == 100 && c.coreInst == 1000 && c.bitRuns[8] == 1 && c.bitRuns[11] == 1 &&
              c.errBits == (kErrVp | kErrEye),
          "B6 census: run totals (an error run counts its bits only; a failed focus check only drops the core share)");
    // 64-bit triangle words
    uint32_t big[kSCount] = {};
    big[kSInst] = 1;
    big[kSTri] = 5;
    big[kSTri + 1] = 2;  // 2^33 + 5
    Census b;
    Accumulate(b, big);
    Check(b.tri == (uint64_t(2) << 32) + 5, "B6 census: triangle totals read as 64-bit words");
    const std::string line = Line(c, Census{}, 20.0);
    printf("     %s\n", line.c_str());
    Check(line.rfind("periph census: 20 s, 3 runs (2 used", 0) == 0 &&
              line.find("zero coverage 3.00 % of instances, 1.80 % of triangles") != std::string::npos &&
              line.find("focus core 10.00 / 14.00 %") != std::string::npos &&
              line.find("0x100 1, 0x800 1") != std::string::npos && line.find("below, close B6") != std::string::npos,
          "B6 census: the greppable line (instance and triangle shares, core share, error runs, gate)");
    Census hi;
    s[kSZeroT] = 3000;  // 6 % of the triangles
    Accumulate(hi, s);
    Check(Line(hi, Census{}, 20.0).find("reached, reopen B6") != std::string::npos,
          "B6 census: a triangle share of 5 % or more reopens B6 even when the instance share is lower");
    Census bad;
    Accumulate(bad, e1);
    Check(Line(bad, Census{}, 20.0) == "periph census: 20 s, no usable run (1 runs, error bits 0x100)",
          "B6 census: no usable run -> says so (no shares)");
  }
}

// ---------------------------------------------------------------------------
// 2. One census run on a device over a synthetic scene (R38 rank 1's test scene)
// ---------------------------------------------------------------------------
struct Inst64 {
  uint32_t w[16];
};
struct Scene {
  uint32_t nb = 300, nObj = 60, nLod = 0, nEntries = 400, idxCap = 32768, mainCap = 32768, N = 0;
  std::vector<uint32_t> objs, lods;
  std::vector<Inst64> main;
  std::vector<uint32_t> counts, offsets, indices, args;
  float tcChain[136 * 4] = {}, tcGb[136 * 4] = {};
};
void SetRow(float* tc, int reg, double a, double b, double c, double d) {
  tc[reg * 4 + 0] = float(a);
  tc[reg * 4 + 1] = float(b);
  tc[reg * 4 + 2] = float(c);
  tc[reg * 4 + 3] = float(d);
}
void MakeCamera(Scene& s, double yaw, double pitch) {
  const double P[4][4] = {{0.7807443, 0, 0.193703, 0}, {0, 0.7906425, -0.0013454, 0}, {0, 0, -1.33333e-07, 0.02},
                          {0, 0, 1, 0}};
  const D3 f = Rot(Rot(D3{0, 0, 1}, D3{1, 0, 0}, -pitch), D3{0, 1, 0}, yaw);
  const D3 rgt = Norm(Cross(D3{0, 1, 0}, f));
  const D3 up = Cross(f, rgt);
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
  s.tcChain[44 * 4 + 2] = 816;  // the chain's output size
  s.tcChain[44 * 4 + 3] = 804;
  s.tcGb[44 * 4 + 2] = 476;  // the G-buffer's render size
  s.tcGb[44 * 4 + 3] = 468;
}

void BuildScene(Scene& s, uint64_t seed, int nInst) {
  Rng r(seed);
  MakeCamera(s, 0.35, 0.05);
  s.objs.assign(size_t(s.nObj) * 16, 0);
  uint32_t lod = 0;
  std::vector<uint32_t> lodObj;
  for (uint32_t o = 0; o < s.nObj; ++o) {
    const uint32_t cnt = 1 + r.U() % 3;
    float* f = reinterpret_cast<float*>(&s.objs[o * 16]);
    const float h = 3 + 20 * float(r.F()), rad = 1 + 12 * float(r.F());
    f[3] = float(r.S());
    f[4] = h * 0.5f;
    f[5] = float(r.S());
    f[6] = rad;
    f[7] = h;
    s.objs[o * 16 + 8] = cnt;
    s.objs[o * 16 + 9] = lod;
    for (uint32_t k = 0; k < cnt; ++k) lodObj.push_back(o);
    lod += cnt;
  }
  s.nLod = lod + 20;  // 20 unreferenced LODs (unmapped)
  s.lods.assign(size_t(s.nLod) * 12, 0);
  for (uint32_t l = 0; l < s.nLod; ++l) {
    s.lods[l * 12 + 4] = l % s.nb;
    s.lods[l * 12 + 7] = 0xffffffffu;  // gPrimCount as live 2.9.30: not a triangle count (unused now)
  }
  const D3 fwd = Norm(D3{s.tcChain[23 * 4 + 0], s.tcChain[23 * 4 + 1], s.tcChain[23 * 4 + 2]});
  const D3 rgt = Norm(Cross(D3{0, 1, 0}, fwd)), up = Cross(fwd, rgt);
  for (int i = 0; i < nInst; ++i) {
    const uint32_t l = (i % 50 == 7) ? lod + (r.U() % 20) : r.U() % lod;
    const uint32_t b = s.lods[l * 12 + 4];
    double dist, ax, ay;
    const int kind = i % 10;  // far (sub-pixel), beyond the side edges, centre, near, behind
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
    f[12] = (i % 13 == 0) ? -float(r.F()) : 1.0f;
    f[13] = float(r.F());
    s.main.push_back(in);
  }
  s.N = uint32_t(s.main.size());
  s.counts.assign(s.nb, 0);
  for (auto& in : s.main) ++s.counts[in.w[0] & 0xffffff];
  s.offsets.assign(s.nb, 0);
  for (uint32_t b = 1; b < s.nb; ++b) s.offsets[b] = s.offsets[b - 1] + s.counts[b - 1];
  s.indices.assign(s.idxCap, 0xdeadbeefu);
  std::vector<std::vector<uint32_t>> per(s.nb);
  for (uint32_t i = 0; i < s.N; ++i) per[s.main[i].w[0] & 0xffffff].push_back(i);
  for (uint32_t b = 0; b < s.nb; ++b) {
    auto& v = per[b];
    for (size_t k = v.size(); k > 1; --k) std::swap(v[k - 1], v[r.U() % k]);
    for (size_t k = 0; k < v.size(); ++k) s.indices[s.offsets[b] + k] = v[k];
  }
  // argument entries: buckets 0..74 drawn by two entries, some entries past the buckets; index counts vary
  s.args.assign(size_t(s.nEntries) * 8, 0);
  for (uint32_t e = 0; e < s.nEntries; ++e) {
    const uint32_t b = e % (s.nb + 25);
    uint32_t* a = &s.args[e * 8];
    a[0] = 3 * (20 + (e * 7) % 50) + (e % 3);  // not always a multiple of 3 (the weight floors it, as the draw does)
    a[1] = b < s.nb ? s.counts[b] : 0;
    a[2] = r.U();
    a[3] = r.U();
    a[7] = b;
  }
}

// CPU reference of zero coverage (double precision); 1 zero, 0 not, -1 borderline (two perturbations disagree).
int RefZero(const Scene& s, uint32_t i, double W, double H) {
  const Inst64& in = s.main[i];
  const uint32_t l = in.w[1] & 0xffffff;
  uint32_t o = 0xffffffffu;
  for (uint32_t k = 0; k < s.nObj; ++k) {
    const uint32_t cnt = s.objs[k * 16 + 8], st = s.objs[k * 16 + 9];
    if (l >= st && l < st + cnt) o = k;
  }
  if (o == 0xffffffffu) return 0;
  const float* f = reinterpret_cast<const float*>(in.w);
  const float* of = reinterpret_cast<const float*>(&s.objs[o * 16]);
  // rigorous radius about the pivot (the kernel's Bounds, in double)
  const D3 X{f[5], f[6], f[7]}, Z{f[8], f[9], f[10]}, c{of[3], of[4], of[5]};
  const double rad = std::fabs(of[6]), lx = Len(X), lz = Len(Z);
  const D3 yd = Cross(Z, X);
  const D3 Y = Len(yd) > 0 ? yd * (lx / Len(yd)) : D3{0, 0, 0};
  const double cosXZ = std::fabs(Dot(X, Z)) / (lx * lz);
  const double sn = cosXZ < 1e-3 ? std::max(lx, lz) * 1.002 : std::sqrt(2 * lx * lx + lz * lz) * 1.002;
  const D3 C = X * c.x + Y * c.y + Z * c.z;
  const double tree = std::fabs(C.y) + 3 * std::sqrt(C.x * C.x + C.z * C.z) + 4 * sn * rad;
  const double flat = 3.1623 * sn * (Len(c) + rad);
  const double rr = f[12] < 0 ? std::max(tree, flat) : tree;
  int res[2];
  for (int pass = 0; pass < 2; ++pass) {
    const double tweak = pass ? 1.002 : 0.998, eps = (pass ? 1.2 : 0.8) / 16.0;
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
    res[pass] = (!has(X0 - eps, X1 + eps, W) || !has(Y0 - eps, Y1 + eps, H)) ? 1 : 0;
  }
  return res[0] == res[1] ? res[0] : -1;
}

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
std::vector<uint32_t> Read(ID3D11Device* d, ID3D11DeviceContext* c, ID3D11Buffer* b) {
  std::vector<uint32_t> v;
  chainfast::ReadBuf(d, c, b, BufBytes(b), v);
  return v;
}

void GpuRun(ID3D11Device* d, ID3D11DeviceContext* c, const char* tag) {
  char what[400];
  Scene s;
  BuildScene(s, 21, 20000);
  const UINT sr = D3D11_BIND_SHADER_RESOURCE, ua = D3D11_BIND_UNORDERED_ACCESS;
  std::vector<Inst64> mainCap(s.mainCap);
  std::copy(s.main.begin(), s.main.end(), mainCap.begin());
  ID3D11Buffer* bufs[8] = {
      MkBuf(d, s.mainCap * 64, sr | ua, D3D11_RESOURCE_MISC_BUFFER_STRUCTURED, 64, mainCap.data()),
      MkBuf(d, s.idxCap * 4, sr | ua, 0, 0, s.indices.data()),
      MkBuf(d, s.nb * 4, sr | ua, 0, 0, s.offsets.data()),
      MkBuf(d, s.nb * 4, sr | ua, 0, 0, s.counts.data()),
      MkBuf(d, s.nEntries * 32, sr | ua, D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS, 0, s.args.data()),
      MkBuf(d, s.nObj * 64, sr, D3D11_RESOURCE_MISC_BUFFER_STRUCTURED, 64, s.objs.data()),
      MkBuf(d, s.nLod * 48, sr, D3D11_RESOURCE_MISC_BUFFER_STRUCTURED, 48, s.lods.data()),
      MkBuf(d, 136 * 16, D3D11_BIND_CONSTANT_BUFFER, 0, 0, s.tcChain)};
  bool made = true;
  for (ID3D11Buffer* b : bufs) made = made && b;
  Check(made, "B6 census: synthetic DCS buffers");
  // the module's view of "DCS" (as the identification sets it)
  {
    std::lock_guard<std::mutex> lock(g_mu);
    ReleaseOurs();
    ReleaseDcs();
  }
  g_resReady = false;
  g_resFail = 0;
  g_rt = Rt{};
  SetDeviceForTest(d);
  Check(CompileAll(), (std::string("B6 census: kernels compile (") + g_codeErr + ")").c_str());
  if (!made || !g_codeReady.load()) {
    for (ID3D11Buffer* b : bufs) Rel(b);
    SetDeviceForTest(nullptr);
    return;
  }
  ID3D11Buffer** dst[8] = {&g_d.mainB, &g_d.idx, &g_d.offs, &g_d.cnts, &g_d.args, &g_d.objs, &g_d.lods, &g_d.tc};
  for (int i = 0; i < 8; ++i) {
    bufs[i]->AddRef();
    *dst[i] = bufs[i];
  }
  g_d.nArgs = s.nEntries;
  g_d.mainCap = s.mainCap;
  g_d.idxCap = s.idxCap;
  g_d.nb = s.nb;
  g_d.nObj = s.nObj;
  g_d.nLod = s.nLod;
  g_d.tcBytes = 136 * 16;
  g_drawLearned = g_dispatchLearned = true;
  g_rt.W[0] = 476;
  g_rt.H[0] = 468;
  g_rt.zwKnown[0] = true;
  g_rt.zw[0][0] = 816;
  g_rt.zw[0][1] = 804;
  g_rt.samples[0] = 1;
  auto run = [&](std::vector<uint32_t>& stats) {
    PeriphBeginAt(0, g_d.tc);
    c->UpdateSubresource(g_d.tc, 0, nullptr, s.tcGb, 0, 0);  // DCS's G-buffer upload inside the pass
    RunCheck(kKCheckRun, 0, g_d.tc);
    stats = Read(d, c, g_o.stats[0].b);
    c->UpdateSubresource(g_d.tc, 0, nullptr, s.tcChain, 0, 0);
  };
  std::vector<uint32_t> st;
  run(st);
  Check(g_resReady.load() && st.size() == kSCount && st[kSClassified] == 1 && st[kSErr] == 0 && st[kSCheckRun] == 1,
        "B6 census: a census run classified with no error bits and the after-run check ran");
  if (st.size() == kSCount) {
    auto q = [&](int at) { return uint64_t(st[at]) | (uint64_t(st[at + 1]) << 32); };
    const std::vector<uint32_t> w = BucketTriCpu(s.args, s.nb);
    uint64_t tri = 0, argTri = 0, refZT = 0, borderT = 0;
    uint32_t refZ = 0, border = 0;
    for (uint32_t i = 0; i < s.N; ++i) tri += w[s.main[i].w[0] & 0xffffff];
    for (uint32_t e = 0; e < s.nEntries; ++e) argTri += uint64_t(s.args[e * 8] / 3) * s.args[e * 8 + 1];
    for (uint32_t j = 0; j < s.N; ++j) {
      const uint32_t i = s.indices[j];
      const int z = RefZero(s, i, 476, 468);
      const uint32_t wi = w[s.main[i].w[0] & 0xffffff];
      if (z < 0) {
        ++border;
        borderT += wi;
      } else if (z) {
        ++refZ;
        refZT += wi;
      }
    }
    const uint64_t gZT = q(kSZeroT);
    sprintf_s(what,
              "B6 census %s: zero coverage %u instances / %llu triangles = CPU %u / %llu (+ %u borderline, %llu "
              "triangles); totals %u instances / %llu triangles = CPU %u / %llu = argument entries %llu",
              tag, st[kSZeroI], (unsigned long long)gZT, refZ, (unsigned long long)refZT, border,
              (unsigned long long)borderT, st[kSInst], (unsigned long long)q(kSTri), s.N, (unsigned long long)tri,
              (unsigned long long)q(kSArgTriK));
    Check(st[kSZeroI] >= refZ && st[kSZeroI] <= refZ + border && gZT >= refZT && gZT <= refZT + borderT &&
              refZ > 1000 && st[kSInst] == s.N && q(kSTri) == tri && q(kSArgTriK) == argTri && tri == argTri,
          what);
    Census cen;
    Accumulate(cen, st.data());
    printf("     %s\n", Line(cen, Census{}, 1).c_str());
  }
  Check(Read(d, c, g_d.idx) == s.indices && Read(d, c, g_d.args) == s.args,
        "B6 census: measure only (DCS's indices and arguments unchanged)");
  {  // an argument entry whose InstanceCount is not counts[bucket]: error 0x10, the run is left out
    std::vector<uint32_t> badArgs = s.args;
    badArgs[5 * 8 + 1] += 1;
    c->UpdateSubresource(g_d.args, 0, nullptr, badArgs.data(), 0, 0);
    std::vector<uint32_t> sb;
    run(sb);
    Census cb;
    if (sb.size() == kSCount) Accumulate(cb, sb.data());
    Check(sb.size() == kSCount && (sb[kSErr] & kErrArgs) && cb.runs == 1 && cb.okRuns == 0,
          "B6 census: arguments inconsistent with counts -> error 0x10, run left out of the shares");
    c->UpdateSubresource(g_d.args, 0, nullptr, s.args.data(), 0, 0);
  }
  {
    std::lock_guard<std::mutex> lock(g_mu);
    ReleaseOurs();
    ReleaseDcs();
  }
  g_resReady = false;
  g_drawLearned = g_dispatchLearned = false;
  g_rt = Rt{};
  SetDeviceForTest(nullptr);
  for (ID3D11Buffer* b : bufs) Rel(b);
}

void Run() {
  printf("-- B6 periphery instance census (periph_census.h) --\n");
  PureChecks();
  ID3D11Device* d = nullptr;
  ID3D11DeviceContext* c = nullptr;
  D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
  if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, &fl, 1, D3D11_SDK_VERSION, &d, nullptr, &c))) {
    QvSkip("B6 census: no WARP device\n");
    return;
  }
  if (!chainfast::LoadCompiler()) {
    QvSkip("B6 census: no d3dcompiler_47\n");
  } else {
    GpuRun(d, c, "WARP");
  }
  c->Release();
  d->Release();
}

}  // namespace pctest
