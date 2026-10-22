// Offline tests for cockpit_ipd.h ([Perception] CockpitTrueIpd, R31 option
// 3: the cockpit at the true eye distance while Depth boost renders the world
// at the boosted one). No DCS, no external load process; D3D on our own WARP
// device.
//  T1 the patch math (rigid views, jittered reverse-Z projections, Dw / Dp,
//     motion vectors, the tail frame, untouched bytes bit-identical);
//  T2 the upload filter (null / full / partial box, sub, unknown buffer, a
//     struct at a *SetConstantBuffers1 offset, fallbacks forward DCS's pointer);
//  T3 BoostOut: layout, a byte compare of the verbatim copy (when
//     QV_PERCEPTION_SHM_H names the peer header), the seqlock reader against a
//     writer thread, the suffix variable with a test-owned mapping;
//  T4 the frame matcher (lag 0/1/2 ramps, Stepped jumps, constant scale,
//     stale, scale 1, pairFault, k rejection, the FOV fingerprint);
//  T6 the eye-axis learner (+-3 degree cant, pupil noise);
//  T5/T7 the device pipeline through fixation_depth.h's boundary callback and
//     gbpass::Hook: an MFD view, four HMD views x (sm 4 tag 600, sm 0, sm 3,
//     GBufferLightingPass, TransparentCockpit*), CopyToHMD, cascades; the GPU
//     sees the patched bytes in the cockpit passes and DCS's bytes after them;
//     signs, one h for all views, the gPrev closed form across frames, tail,
//     scale 1, observe, foreign hooks, h > 40 mm, a second cockpit pass, stale,
//     the test shift, a mid-pass table swap repaired by UsRearm;
//  T8 off is bit-identical (no observer, no install, no mapping, the hooks
//     forward the identical pointer, the context table untouched by uploads);
//  T9 phase 2: the stencil split on D24S8 and D32S8 (every pixel written once,
//     the right b7 per region), the hooked GBufferLightingPass, the MSAA skip,
//     the debug layer.
#pragma once

namespace cktest {

using namespace fixdepth;
namespace ck = fixdepth::ckipd;
using fdtest::Rel;
using psh_BoostOut = ::dcsvr_perception::BoostOut;
constexpr uint32_t psh_enabled() { return ::dcsvr_perception::boost_flags::enabled; }
constexpr uint32_t psh_applied() { return ::dcsvr_perception::boost_flags::applied; }
const char* psh_suffixVar() { return ::dcsvr_perception::name_suffix_variable; }
// The tests written before R32 exercise the d3d11 overlay path (CockpitTrueIpdOverlayPath=d3d11, A/B only since
// R32); the cpu path has its own tests (ovl_cpu_test.h).
template <typename... A>
void ConfigureD3d11(A... a) {
  ck::Configure(a...);
  ck::g_cfgOvlPath = ck::kOvlPathD3d11;
}

// ---------------------------------------------------------------------------
// Matrices (logical [r][c], row vectors) and DCS-like cPerView structs
// ---------------------------------------------------------------------------

struct Mat {
  double a[4][4] = {};
};
Mat Ident() {
  Mat m;
  for (int i = 0; i < 4; ++i) m.a[i][i] = 1;
  return m;
}
Mat Mul(const Mat& x, const Mat& y) {
  Mat m;
  for (int r = 0; r < 4; ++r)
    for (int c = 0; c < 4; ++c)
      for (int k = 0; k < 4; ++k) m.a[r][c] += x.a[r][k] * y.a[k][c];
  return m;
}
Mat Inv(const Mat& in) {
  double a[4][8];
  for (int r = 0; r < 4; ++r)
    for (int c = 0; c < 8; ++c) a[r][c] = c < 4 ? in.a[r][c] : (c - 4 == r ? 1.0 : 0.0);
  for (int c = 0; c < 4; ++c) {
    int p = c;
    for (int r = c + 1; r < 4; ++r)
      if (std::fabs(a[r][c]) > std::fabs(a[p][c])) p = r;
    for (int k = 0; k < 8; ++k) std::swap(a[c][k], a[p][k]);
    const double d = a[c][c];
    for (int k = 0; k < 8; ++k) a[c][k] /= d;
    for (int r = 0; r < 4; ++r)
      if (r != c) {
        const double f = a[r][c];
        for (int k = 0; k < 8; ++k) a[r][k] -= f * a[c][k];
      }
  }
  Mat m;
  for (int r = 0; r < 4; ++r)
    for (int c = 0; c < 4; ++c) m.a[r][c] = a[r][c + 4];
  return m;
}
bool NonZero3(const double v[3]) { return v[0] != 0 || v[1] != 0 || v[2] != 0; }
Mat Trans(const double v[3]) {
  Mat m = Ident();
  for (int i = 0; i < 3; ++i) m.a[3][i] = v[i];
  return m;
}
void Put(uint8_t* s, uint32_t off, const Mat& m) {
  float f[16];
  for (int r = 0; r < 4; ++r)
    for (int c = 0; c < 4; ++c) f[c * 4 + r] = static_cast<float>(m.a[r][c]);
  memcpy(s + off, f, sizeof(f));
}
Mat Get(const uint8_t* s, uint32_t off) {
  float f[16];
  memcpy(f, s + off, sizeof(f));
  Mat m;
  for (int r = 0; r < 4; ++r)
    for (int c = 0; c < 4; ++c) m.a[r][c] = f[c * 4 + r];
  return m;
}
double MaxDiff(const Mat& x, const Mat& y) {
  double d = 0;
  for (int r = 0; r < 4; ++r)
    for (int c = 0; c < 4; ++c) d = std::max(d, std::fabs(x.a[r][c] - y.a[r][c]));
  return d;
}
double MaxAbs(const Mat& x) {
  double d = 0;
  for (int r = 0; r < 4; ++r)
    for (int c = 0; c < 4; ++c) d = std::max(d, std::fabs(x.a[r][c]));
  return d;
}
// Row vector p (w = 1) through m, then the perspective divide.
void Project(const Mat& m, const double p[3], double out[3]) {
  double c[4];
  for (int k = 0; k < 4; ++k) c[k] = p[0] * m.a[0][k] + p[1] * m.a[1][k] + p[2] * m.a[2][k] + m.a[3][k];
  for (int k = 0; k < 3; ++k) out[k] = c[k] / c[3];
}
// A clip-space row vector through m, then the divide.
void ClipThrough(const double clip[4], const Mat& m, double out[3]) {
  double c[4];
  for (int k = 0; k < 4; ++k)
    c[k] = clip[0] * m.a[0][k] + clip[1] * m.a[1][k] + clip[2] * m.a[2][k] + clip[3] * m.a[3][k];
  for (int k = 0; k < 3; ++k) out[k] = c[k] / c[3];
}
// Rotation rows (right, up, forward) from yaw / pitch / roll in degrees.
void Rot(double yaw, double pitch, double roll, double R[3][3]) {
  const double y = yaw * 0.017453292519943295, p = pitch * 0.017453292519943295, r = roll * 0.017453292519943295;
  // forward from yaw/pitch, then right/up, rolled.
  const double f[3] = {std::sin(y) * std::cos(p), std::sin(p), std::cos(y) * std::cos(p)};
  double rt[3] = {std::cos(y), 0, -std::sin(y)};
  double up[3] = {f[1] * rt[2] - f[2] * rt[1], f[2] * rt[0] - f[0] * rt[2], f[0] * rt[1] - f[1] * rt[0]};
  // up = f x right gives a left-handed up; flip to point +y for zero pitch
  if (up[1] < 0)
    for (double& v : up) v = -v;
  for (int i = 0; i < 3; ++i) {
    R[0][i] = std::cos(r) * rt[i] + std::sin(r) * up[i];
    R[1][i] = -std::sin(r) * rt[i] + std::cos(r) * up[i];
    R[2][i] = f[i];
  }
}
// Rotates the rows of R about R's own up axis by deg (a canted display).
void Cant(const double R[3][3], double deg, double out[3][3]) {
  const double a = deg * 0.017453292519943295;
  for (int i = 0; i < 3; ++i) {
    out[0][i] = std::cos(a) * R[0][i] - std::sin(a) * R[2][i];
    out[1][i] = R[1][i];
    out[2][i] = std::sin(a) * R[0][i] + std::cos(a) * R[2][i];
  }
}
Mat ViewInvOf(const double R[3][3], const double C[3]) {
  Mat m = Ident();
  for (int i = 0; i < 3; ++i)
    for (int j = 0; j < 3; ++j) m.a[i][j] = R[i][j];
  for (int j = 0; j < 3; ++j) m.a[3][j] = C[j];
  return m;
}
Mat ProjOf(const double fov[4], double jx, double jy) {
  float P[16];
  fdtest::MakeProj(P, fov[0], fov[1], fov[2], fov[3], 0.02, 0, true);
  P[8] += static_cast<float>(jx);
  P[9] += static_cast<float>(jy);
  Mat m;
  for (int r = 0; r < 4; ++r)
    for (int c = 0; c < 4; ++c) m.a[r][c] = P[r * 4 + c];
  return m;
}

// A DCS-like cPerView struct: the camera fields from (VI, P, prevVP), every
// other byte a deterministic pattern (finite floats).
void BuildStruct(uint8_t* s, const Mat& VI, const Mat& P, const Mat& prevVP, uint32_t seed) {
  uint32_t x = seed * 2654435761u + 12345u;
  for (uint32_t i = 0; i < ck::kCbBytes; i += 4) {
    x = x * 1664525u + 1013904223u;
    const float f = static_cast<float>(static_cast<int32_t>(x >> 8) % 20000) * 0.001f - 10.0f;
    memcpy(s + i, &f, 4);
  }
  const Mat V = Inv(VI);
  Mat Pf;  // P as DCS stores it (floats)
  Put(s, ck::kOffProj, P);
  Pf = Get(s, ck::kOffProj);
  Put(s, ck::kOffView, V);
  Put(s, ck::kOffViewInv, VI);
  Put(s, ck::kOffProjInv, Inv(Pf));
  const Mat VP = Mul(Get(s, ck::kOffView), Pf);
  Put(s, ck::kOffViewProj, VP);
  const Mat VPI = Inv(Get(s, ck::kOffViewProj));
  Put(s, ck::kOffViewProjInv, VPI);
  Put(s, ck::kOffPrevVP, prevVP);
  Put(s, ck::kOffPrevXf, Mul(Get(s, ck::kOffViewProjInv), Get(s, ck::kOffPrevVP)));
  const float cp[4] = {static_cast<float>(VI.a[3][0]), static_cast<float>(VI.a[3][1]), static_cast<float>(VI.a[3][2]),
                       1.0f};
  memcpy(s + ck::kOffCamPos, cp, sizeof(cp));
}

// The bytes the patch may change.
bool InPatchedField(uint32_t i) {
  const uint32_t f[][2] = {{ck::kOffView, 64},     {ck::kOffViewInv, 64}, {ck::kOffViewProj, 64},
                           {ck::kOffViewProjInv, 64}, {ck::kOffPrevXf, 64}, {ck::kOffPrevVP, 64},
                           {ck::kOffCamPos, 12}};
  for (auto& r : f)
    if (i >= r[0] && i < r[0] + r[1]) return true;
  return false;
}
bool OthersIdentical(const uint8_t* a, const uint8_t* b) {
  for (uint32_t i = 0; i < ck::kCbBytes; ++i)
    if (!InPatchedField(i) && a[i] != b[i]) return false;
  return true;
}
bool FieldIdentical(const uint8_t* a, const uint8_t* b, uint32_t off, uint32_t n = 64) { return !memcmp(a + off, b + off, n); }
void CamOf(const uint8_t* s, double C[3]) {
  const Mat vi = Get(s, ck::kOffViewInv);
  for (int i = 0; i < 3; ++i) C[i] = vi.a[3][i];
}

uint32_t g_rng = 1;
double Rnd(double lo, double hi) {
  g_rng = g_rng * 1664525u + 1013904223u;
  return lo + (hi - lo) * ((g_rng >> 8) / 16777216.0);
}

// ---------------------------------------------------------------------------
// T1 the patch math
// ---------------------------------------------------------------------------

void TestPatchMath() {
  bool vOk = true, viOk = true, vpOk = true, vpiOk = true, projOk = true, motionOk = true, bitsOk = true,
       projBits = true, camOk = true;
  double worstMotion = 0;
  for (int it = 0; it < 200; ++it) {
    double R[3][3], C[3], Rp[3][3], Cp[3];
    const double yaw = Rnd(-180, 180), pitch = Rnd(-40, 40), roll = Rnd(-10, 10);
    Rot(yaw, pitch, roll, R);
    Rot(yaw + Rnd(-2, 2), pitch + Rnd(-2, 2), roll + Rnd(-1, 1), Rp);  // the previous frame: the head turned a little
    for (int i = 0; i < 3; ++i) C[i] = Rnd(-3, 3), Cp[i] = C[i] + Rnd(-0.05, 0.05);
    const double fov[4] = {Rnd(-1.4, -0.5), Rnd(0.5, 1.4), Rnd(0.5, 1.3), Rnd(-1.3, -0.5)};
    const Mat P = ProjOf(fov, Rnd(-0.004, 0.004), Rnd(-0.004, 0.004));
    const Mat prevVP = Mul(Inv(ViewInvOf(Rp, Cp)), ProjOf(fov, Rnd(-0.004, 0.004), Rnd(-0.004, 0.004)));
    alignas(16) uint8_t s[ck::kCbBytes], o[ck::kCbBytes];
    BuildStruct(s, ViewInvOf(R, C), P, prevVP, it);
    memcpy(o, s, sizeof(s));
    double dw[3], dp[3];
    for (int i = 0; i < 3; ++i) dw[i] = Rnd(-0.02, 0.02), dp[i] = it % 3 == 0 ? 0.0 : Rnd(-0.02, 0.02);
    ck::PatchStruct(s, dw, dp);
    const Mat V = Get(s, ck::kOffView), VI = Get(s, ck::kOffViewInv), VP = Get(s, ck::kOffViewProj),
              VPI = Get(s, ck::kOffViewProjInv), PFT = Get(s, ck::kOffPrevXf), PVP = Get(s, ck::kOffPrevVP);
    const Mat Pd = Get(o, ck::kOffProj), VPd = Get(o, ck::kOffViewProj), PVPd = Get(o, ck::kOffPrevVP);
    if (MaxDiff(Mul(VI, V), Ident()) > 2e-5) viOk = false;
    if (MaxDiff(Mul(Get(o, ck::kOffViewInv), Trans(dw)), VI) > 1e-5) vOk = false;
    if (MaxDiff(Mul(V, Pd), VP) > 2e-5 * std::max(1.0, MaxAbs(VP))) vpOk = false;
    if (MaxDiff(Mul(VPI, VP), Ident()) > 2e-4) vpiOk = false;
    float cp[3];
    memcpy(cp, s + ck::kOffCamPos, sizeof(cp));
    for (int i = 0; i < 3; ++i)
      if (std::fabs(cp[i] - (C[i] + dw[i])) > 2e-6 * std::max(1.0, std::fabs(C[i]))) camOk = false;
    for (int k = 0; k < 5; ++k) {
      // A point in front of the camera.
      double p[3];
      const double tx = Rnd(-0.4, 0.4), ty = Rnd(-0.4, 0.4), dist = Rnd(0.3, 50);
      for (int i = 0; i < 3; ++i) p[i] = C[i] + dw[i] + dist * (R[2][i] + tx * R[0][i] + ty * R[1][i]);
      double a[3], b[3], pm[3] = {p[0] - dw[0], p[1] - dw[1], p[2] - dw[2]};
      Project(VP, p, a);
      Project(VPd, pm, b);
      for (int i = 0; i < 3; ++i)
        if (std::fabs(a[i] - b[i]) > 2e-4) projOk = false;
      // Motion: current clip through gPrevFrameTransform = the point through the true previous camera.
      double clip[4];
      for (int c = 0; c < 4; ++c) clip[c] = p[0] * VP.a[0][c] + p[1] * VP.a[1][c] + p[2] * VP.a[2][c] + VP.a[3][c];
      double m1[3], m2[3];
      ClipThrough(clip, PFT, m1);
      const double ndp[3] = {-dp[0], -dp[1], -dp[2]};
      Project(Mul(Trans(ndp), PVPd), p, m2);
      double m3[3];
      Project(PVP, p, m3);
      for (int i = 0; i < 2; ++i) {
        worstMotion = std::max(worstMotion, std::max(std::fabs(m1[i] - m2[i]), std::fabs(m3[i] - m2[i])));
        if (std::fabs(m1[i] - m2[i]) > 5e-4 || std::fabs(m3[i] - m2[i]) > 2e-4) motionOk = false;
      }
    }
    if (!OthersIdentical(s, o)) bitsOk = false;
    if (!FieldIdentical(s, o, ck::kOffProj) || !FieldIdentical(s, o, ck::kOffProjInv)) projBits = false;
  }
  Check(vOk && viOk, "cockpit ipd T1: gViewInv' = gViewInv * T(Dw) (camera moved by Dw), gViewInv' * gView' = I");
  Check(vpOk && vpiOk, "cockpit ipd T1: gViewProj' = gView' * gProj (jittered), gViewProjInv' * gViewProj' = I");
  Check(projOk, "cockpit ipd T1: p through gViewProj' = (p - Dw) through DCS's gViewProj (200 random rigid views)");
  printf("     (motion vectors: worst NDC difference %.2e)\n", worstMotion);
  Check(motionOk, "cockpit ipd T1: gPrevFrameTransform' and gPrevFrameViewProj' = the true previous camera (Dp)");
  Check(camOk, "cockpit ipd T1: gCameraPos xyz += Dw");
  Check(bitsOk && projBits, "cockpit ipd T1: gProj, gProjInv and every byte outside the patched fields bit-identical");
  // Zero: no change at all.
  {
    double R[3][3], C[3] = {0.4, 1.1, -0.3};
    Rot(20, 5, 0, R);
    const double fov[4] = {-1, 1, 1, -1};
    alignas(16) uint8_t s[ck::kCbBytes], o[ck::kCbBytes];
    BuildStruct(s, ViewInvOf(R, C), ProjOf(fov, 0.001, -0.002), Mul(Inv(ViewInvOf(R, C)), ProjOf(fov, 0, 0)), 7);
    memcpy(o, s, sizeof(s));
    const double z[3] = {0, 0, 0};
    const uint32_t ch = ck::PatchStruct(s, z, z);
    Check(ch == 0 && !memcmp(s, o, sizeof(s)), "cockpit ipd T1: Dw = Dp = 0 is no patch (bytes identical)");
    // Tail: the cockpit was rendered at C (true) last frame, DCS's previous
    // camera was C - Dp (boosted); this frame DCS renders at C itself (no boost).
    const double dp[3] = {0.0131 * R[0][0], 0.0131 * R[0][1], 0.0131 * R[0][2]};
    const double Cb[3] = {C[0] - dp[0], C[1] - dp[1], C[2] - dp[2]};
    const Mat P = ProjOf(fov, 0.001, -0.002);
    BuildStruct(s, ViewInvOf(R, C), P, Mul(Inv(ViewInvOf(R, Cb)), P), 8);
    memcpy(o, s, sizeof(s));
    const uint32_t tch = ck::PatchStruct(s, z, dp);
    bool zeroMotion = true;
    const Mat VP = Get(s, ck::kOffViewProj), PFT = Get(s, ck::kOffPrevXf);
    for (int k = 0; k < 20; ++k) {
      double p[3];
      const double dist = Rnd(0.3, 3);
      for (int i = 0; i < 3; ++i) p[i] = C[i] + dist * (R[2][i] + Rnd(-0.3, 0.3) * R[0][i] + Rnd(-0.3, 0.3) * R[1][i]);
      double clip[4], a[3], b[3];
      for (int c = 0; c < 4; ++c) clip[c] = p[0] * VP.a[0][c] + p[1] * VP.a[1][c] + p[2] * VP.a[2][c] + VP.a[3][c];
      ClipThrough(clip, PFT, a);
      Project(VP, p, b);
      if (std::fabs(a[0] - b[0]) > 2e-4 || std::fabs(a[1] - b[1]) > 2e-4) zeroMotion = false;
    }
    Check(tch == (ck::kFPrevXf | ck::kFPrevVP) && FieldIdentical(s, o, ck::kOffView) &&
              FieldIdentical(s, o, ck::kOffViewInv) && FieldIdentical(s, o, ck::kOffViewProj) &&
              FieldIdentical(s, o, ck::kOffViewProjInv) && FieldIdentical(s, o, ck::kOffCamPos, 12) && zeroMotion,
          "cockpit ipd T1: tail frame (Dw 0, Dp != 0) patches only the gPrev fields; a static point has zero motion");
    // A -0.0 cell keeps its sign when the patch adds nothing to it.
    BuildStruct(s, ViewInvOf(R, C), P, Mul(Inv(ViewInvOf(R, C)), P), 9);
    const float negz = -0.0f;
    memcpy(s + ck::kOffViewInv + 3 * 4, &negz, 4);  // logical VI[3][0] at f[0*4+3]: row 3 col 0 -> changes
    memcpy(s + ck::kOffViewInv + 4 * 4 + 3 * 4, &negz, 4);  // f[7] = VI[3][1]
    memcpy(s + ck::kOffViewInv + 12, &negz, 4);
    float before[16];
    memcpy(before, s + ck::kOffViewInv, 64);
    const double dx[3] = {0.01, 0, 0};
    ck::PatchStruct(s, dx, z);
    float after[16];
    memcpy(after, s + ck::kOffViewInv, 64);
    bool keep = true;
    for (int i = 0; i < 16; ++i) {
      const int r = i % 4, c = i / 4;  // f[c*4+r]
      if (!(r == 3 && c == 0) && memcmp(&before[i], &after[i], 4)) keep = false;
    }
    Check(keep, "cockpit ipd T1: cells the patch does not move keep their exact bits (incl. -0.0)");
  }
  // Self-checks.
  {
    double R[3][3], C[3] = {0.4, 1.1, -0.3};
    Rot(20, 5, 0, R);
    const double fov[4] = {-1, 1, 1, -1};
    const Mat P = ProjOf(fov, 0.001, -0.002);
    alignas(16) uint8_t s[ck::kCbBytes];
    BuildStruct(s, ViewInvOf(R, C), P, P, 3);
    const uint32_t ok = ck::SelfCheck(s);
    uint8_t b1[ck::kCbBytes];
    memcpy(b1, s, sizeof(b1));
    float f = 1.3f;
    memcpy(b1 + ck::kOffViewInv, &f, 4);  // right axis no longer unit
    uint8_t b2[ck::kCbBytes];
    memcpy(b2, s, sizeof(b2));
    memcpy(&f, b2 + ck::kOffViewProj + 4 * 3, 4);
    f += 0.5f;
    memcpy(b2 + ck::kOffViewProj + 4 * 3, &f, 4);  // VP != V * P
    uint8_t b3[ck::kCbBytes];
    memcpy(b3, s, sizeof(b3));
    f = 5.0f;
    memcpy(b3 + ck::kOffCamPos, &f, 4);  // camera position 4.6 m away
    uint8_t b4[ck::kCbBytes];
    memcpy(b4, s, sizeof(b4));
    f = std::numeric_limits<float>::quiet_NaN();
    memcpy(b4 + ck::kOffPrevXf + 8, &f, 4);
    Check(ok == 0 && (ck::SelfCheck(b1) & ck::kChkAxis) && (ck::SelfCheck(b2) & ck::kChkVp) &&
              ck::SelfCheck(b3) == ck::kChkCam && (ck::SelfCheck(b4) & ck::kChkFinite),
          "cockpit ipd T1: self-checks (unit right axis, VP == V*P, finite; camera position only logged)");
  }
}

// ---------------------------------------------------------------------------
// R6: gPrevFrameViewProj (the camera centre, the store's translation, the
// gPrevFrameTransform map when DCS built it with another inverse)
// ---------------------------------------------------------------------------

ck::M4 ToM4(const Mat& m) {
  ck::M4 r;
  for (int i = 0; i < 4; ++i)
    for (int j = 0; j < 4; ++j) r.a[i][j] = m.a[i][j];
  return r;
}

void TestPrevMath() {
  // The centre: jitter, an unjittered or other projection (near) and a world translation.
  bool centreOk = true, shiftOk = true, singular = true;
  double worst = 0;
  for (int it = 0; it < 100; ++it) {
    double R[3][3], C[3];
    Rot(Rnd(-180, 180), Rnd(-40, 40), Rnd(-10, 10), R);
    for (double& c : C) c = Rnd(-50, 50);
    const double fov[4] = {Rnd(-1.4, -0.5), Rnd(0.5, 1.4), Rnd(0.5, 1.3), Rnd(-1.3, -0.5)};
    const Mat V = Inv(ViewInvOf(R, C));
    Mat Palt = ProjOf(fov, 0, 0);
    Palt.a[3][2] *= 3.0;  // another depth mapping (near)
    const Mat Ms[3] = {Mul(V, ProjOf(fov, Rnd(-0.004, 0.004), Rnd(-0.004, 0.004))), Mul(V, ProjOf(fov, 0, 0)),
                       Mul(V, Palt)};
    for (const Mat& m : Ms) {
      double c[3];
      if (!ck::Centre(ToM4(m), c)) {
        centreOk = false;
        continue;
      }
      for (int i = 0; i < 3; ++i) {
        worst = std::max(worst, std::fabs(c[i] - C[i]));
        if (std::fabs(c[i] - C[i]) > 1e-7 * (1 + std::fabs(C[i]))) centreOk = false;
      }
      const double d[3] = {Rnd(-1, 1), Rnd(-1, 1), Rnd(-1, 1)}, nd[3] = {-d[0], -d[1], -d[2]};
      double c2[3];
      if (!ck::Centre(ToM4(Mul(Trans(nd), m)), c2)) shiftOk = false;
      for (int i = 0; i < 3; ++i)
        if (std::fabs(c2[i] - (C[i] + d[i])) > 1e-7 * (1 + std::fabs(C[i]))) shiftOk = false;
    }
  }
  {
    double c[3];
    singular = !ck::Centre(ck::M4{}, c);
  }
  printf("     (camera centre of gViewProj variants: worst error %.2e)\n", worst);
  Check(centreOk && shiftOk && singular,
        "cockpit ipd R6: the camera centre of a VP is the camera, with or without DLSS jitter and for another near "
        "plane; T(-d) * VP moves it by +d; a degenerate matrix has none");

  // The store: sent = T(-Dw) * DCS's gViewProj (as PatchStruct writes it) gives back Dw.
  bool storeOk = true;
  for (int it = 0; it < 50; ++it) {
    double R[3][3], C[3];
    Rot(Rnd(-180, 180), Rnd(-30, 30), 0, R);
    for (double& c : C) c = Rnd(-20, 20);
    const double fov[4] = {-1.2, 1.0, 1.1, -1.1};
    const Mat P = ProjOf(fov, Rnd(-0.004, 0.004), Rnd(-0.004, 0.004));
    alignas(16) uint8_t s[ck::kCbBytes], o[ck::kCbBytes];
    BuildStruct(s, ViewInvOf(R, C), P, Mul(Inv(ViewInvOf(R, C)), P), 30 + it);
    memcpy(o, s, sizeof(s));
    const double dw[3] = {Rnd(-0.03, 0.03), Rnd(-0.03, 0.03), Rnd(-0.03, 0.03)}, z[3] = {};
    ck::PatchStruct(s, dw, z);
    ck::PrevStore st;
    st.valid = st.patched = true;
    st.vp = ck::LoadM(o, ck::kOffViewProj);
    st.sent = ck::LoadM(s, ck::kOffViewProj);
    double dp[3];
    if (!ck::StoreShift(st, dp)) storeOk = false;
    for (int i = 0; i < 3; ++i)
      if (std::fabs(dp[i] - dw[i]) > 2e-6 * (1 + std::fabs(C[i]))) storeOk = false;
  }
  {
    ck::PrevStore st;  // not patched: 0; sent rotated against DCS's: not a translation
    double dp[3] = {1, 1, 1};
    const bool plain = ck::StoreShift(st, dp) && dp[0] == 0 && dp[1] == 0 && dp[2] == 0;
    double R[3][3], R2[3][3], C[3] = {1, 2, 3};
    Rot(10, 0, 0, R);
    Rot(12, 0, 0, R2);
    const double fov[4] = {-1, 1, 1, -1};
    st.patched = true;
    st.vp = ToM4(Mul(Inv(ViewInvOf(R, C)), ProjOf(fov, 0, 0)));
    st.sent = ToM4(Mul(Inv(ViewInvOf(R2, C)), ProjOf(fov, 0, 0)));
    Check(storeOk && plain && !ck::StoreShift(st, dp),
          "cockpit ipd R6: the store's translation (our sent gViewProj against DCS's) gives back the applied Dw; "
          "0 when not patched; a rotation is refused");
  }

  // gPrevFrameTransform built by DCS with another clip -> world map (unjittered, other near):
  // the patch uses that map (kFXfAlt); with gViewProjInv it keeps the standard path.
  bool altOk = true, stdOk = true;
  double worstAlt = 0;
  for (int it = 0; it < 50; ++it) {
    double R[3][3], C[3], Rp[3][3], Cp[3];
    const double yw = Rnd(-180, 180), pt = Rnd(-30, 30);
    Rot(yw, pt, 0, R);
    Rot(yw + Rnd(-2, 2), pt + Rnd(-2, 2), 0, Rp);
    for (int i = 0; i < 3; ++i) C[i] = Rnd(-3, 3), Cp[i] = C[i] + Rnd(-0.05, 0.05);
    const double fov[4] = {-1.2, 1.0, 1.1, -1.1};
    const Mat P = ProjOf(fov, Rnd(-0.004, 0.004), Rnd(-0.004, 0.004));
    Mat Palt = ProjOf(fov, 0, 0);
    Palt.a[3][2] *= 2.5;
    const Mat PVPd = Mul(Inv(ViewInvOf(Rp, Cp)), ProjOf(fov, Rnd(-0.004, 0.004), Rnd(-0.004, 0.004)));
    alignas(16) uint8_t s[ck::kCbBytes], o[ck::kCbBytes];
    BuildStruct(s, ViewInvOf(R, C), P, PVPd, 60 + it);
    memcpy(o, s, sizeof(s));
    const double dw[3] = {Rnd(-0.02, 0.02), Rnd(-0.02, 0.02), Rnd(-0.02, 0.02)};
    const double dp[3] = {Rnd(-0.02, 0.02), Rnd(-0.02, 0.02), Rnd(-0.02, 0.02)};
    if (ck::PatchStruct(s, dw, dp) & ck::kFXfAlt) stdOk = false;
    // DCS's X = inv(V * Palt) * PVP.
    const Mat V = Get(o, ck::kOffView), Pv = Get(o, ck::kOffPrevVP);
    memcpy(s, o, sizeof(s));
    Put(s, ck::kOffPrevXf, Mul(Inv(Mul(V, Palt)), Pv));
    if (!(ck::PatchStruct(s, dw, dp) & ck::kFXfAlt)) altOk = false;
    const Mat X = Get(s, ck::kOffPrevXf), PVPt = Get(s, ck::kOffPrevVP);
    const double ndw[3] = {-dw[0], -dw[1], -dw[2]}, ndp[3] = {-dp[0], -dp[1], -dp[2]};
    const Mat Mtrue = Mul(Mul(Trans(ndw), V), Palt);  // the true camera with DCS's other projection
    const Mat Ptrue = Mul(Trans(ndp), Pv);
    if (MaxDiff(PVPt, Ptrue) > 2e-5 * std::max(1.0, MaxAbs(Ptrue))) altOk = false;
    for (int k = 0; k < 5; ++k) {
      double p[3];
      const double dist = Rnd(0.3, 20);
      for (int i = 0; i < 3; ++i) p[i] = C[i] + dw[i] + dist * (R[2][i] + Rnd(-0.4, 0.4) * R[0][i] + Rnd(-0.4, 0.4) * R[1][i]);
      double clip[4], a[3], b[3];
      for (int c = 0; c < 4; ++c) clip[c] = p[0] * Mtrue.a[0][c] + p[1] * Mtrue.a[1][c] + p[2] * Mtrue.a[2][c] + Mtrue.a[3][c];
      ClipThrough(clip, X, a);
      Project(Ptrue, p, b);
      for (int i = 0; i < 2; ++i) {
        worstAlt = std::max(worstAlt, std::fabs(a[i] - b[i]));
        if (std::fabs(a[i] - b[i]) > 5e-4) altOk = false;
      }
    }
  }
  printf("     (gPrevFrameTransform with DCS's own clip->world map: worst NDC difference %.2e)\n", worstAlt);
  Check(stdOk, "cockpit ipd R6: gPrevFrameTransform = gViewProjInv * PVP: the standard column (no alternative map)");
  Check(altOk, "cockpit ipd R6: gPrevFrameTransform built with an unjittered / other-near inverse: patched with that "
               "map, static motion vectors = the true cameras'");
}

// ---------------------------------------------------------------------------
// T6 the eye-axis learner
// ---------------------------------------------------------------------------

void TestAxis() {
  const double h = 0.0131;  // x1.4 of 65.5 mm
  double worst = 0, worstFallback = 0;
  for (int cant = -3; cant <= 3; cant += 6) {
    ck::Axis ax;
    for (int f = 0; f < 200; ++f) {
      double H[3][3], L[3][3];
      Rot(Rnd(-180, 180), Rnd(-30, 30), Rnd(-15, 15), H);  // the head turns every frame
      Cant(H, cant, L);                                   // the left eye's canted display
      double d[3];
      for (int i = 0; i < 3; ++i) d[i] = 0.0917 * H[0][i] + Rnd(-0.0005, 0.0005);  // boosted eyes + pupil noise
      ax.Add(L, d);
      if (f >= ck::Axis::kLearn) {
        double u[3];
        ax.World(L, u);
        double e = 0, ef = 0;
        for (int i = 0; i < 3; ++i) {
          e += (u[i] - H[0][i]) * (u[i] - H[0][i]);
          ef += (L[0][i] - H[0][i]) * (L[0][i] - H[0][i]);
        }
        worst = std::max(worst, std::sqrt(e) * h);
        worstFallback = std::max(worstFallback, std::sqrt(ef) * h);
      }
    }
    Check(ax.Learned() && std::fabs(ax.AngleToRightDeg() - 3.0) < 0.3,
          cant > 0 ? "cockpit ipd T6: +3 degree cant learned (axis 3 degrees from the camera's right axis)"
                   : "cockpit ipd T6: -3 degree cant learned (axis 3 degrees from the camera's right axis)");
  }
  printf("     (eye axis: worst error after learning %.3f mm, right-axis fallback %.3f mm, at h = 13.1 mm)\n",
         worst * 1e3, worstFallback * 1e3);
  Check(worst < 0.2e-3 && worstFallback > 0.5e-3,
        "cockpit ipd T6: learned axis error < 0.2 mm at x1.4 with pupil noise (the right axis alone: > 0.5 mm)");
  ck::Axis empty;
  double u[3];
  double R[3][3];
  Rot(0, 0, 0, R);
  Check(!empty.Learned() && !empty.World(R, u), "cockpit ipd T6: nothing learned -> no axis (the right axis is used)");
}

// ---------------------------------------------------------------------------
// T4 the frame matcher
// ---------------------------------------------------------------------------

ck::Rec MakeRec(uint32_t frame, double scale, double base = 0.0655, bool enabled = true) {
  ck::Rec r;
  r.frame = frame;
  r.displayTime = 100000 + frame * 11111;
  r.flags = (enabled ? psh_enabled() : 0) | (enabled && std::fabs(scale - 1) >= 1e-3 ? psh_applied() : 0);
  r.scale = static_cast<float>(scale);
  r.base = static_cast<float>(base);
  r.rendered = static_cast<float>(base * scale);
  return r;
}

void TestMatcher() {
  // Lag 0, 1, 2 with a ramp: the learned lag, then exact picks.
  for (int trueLag = 0; trueLag <= 2; ++trueLag) {
    ck::Matcher m;
    std::vector<ck::Rec> sent;
    int exactAfter = -1, wrongLate = 0;
    for (uint32_t n = 1; n <= 120; ++n) {
      const double scale = n < 20 ? 1.0 : n < 60 ? 1.0 + (n - 20) * 0.01 : 1.4;
      const ck::Rec r = MakeRec(n, scale);
      sent.push_back(r);
      m.Push(r, n);
      const auto lo = m.Latch(n, nullptr, nullptr);
      const ck::Rec& real = sent[sent.size() - 1 - std::min<size_t>(trueLag, sent.size() - 1)];
      const bool exact = std::fabs(lo.h - ck::RecH(real)) < 1e-7;
      if (n >= 20 && n < 60) {
        if (exact && exactAfter < 0) exactAfter = static_cast<int>(n);
        if (!exact && n >= 30) ++wrongLate;
      }
      m.Verify(ck::RecIpd(real), lo);  // DCS rendered the real record's views
    }
    char what[160];
    snprintf(what, sizeof(what),
             "cockpit ipd T4: lag %d learned during a ramp (lag %d, %llu switch(es)), exact picks from frame %d on",
             trueLag, m.lag, static_cast<unsigned long long>(m.switches), exactAfter);
    Check(m.lag == trueLag && wrongLate == 0 && !m.fault && (trueLag == 0 ? m.switches == 0 : m.switches == 1), what);
  }
  // Constant scale: always "same", h exact; scale 1 / not applied: 0.
  {
    ck::Matcher m;
    bool same = true;
    for (uint32_t n = 1; n <= 10; ++n) {
      m.Push(MakeRec(n, 1.4), n);
      const auto lo = m.Latch(n, nullptr, nullptr);
      if (lo.path != ck::kPathSame || std::fabs(lo.h - 0.0655 * 0.4 / 2) > 1e-7) same = false;
    }
    m.Push(MakeRec(11, 1.0), 11);
    m.Push(MakeRec(12, 1.0), 12);
    m.Push(MakeRec(13, 1.0), 13);
    const auto lo1 = m.Latch(13, nullptr, nullptr);
    ck::Rec off = MakeRec(14, 1.4);
    off.flags = psh_enabled();  // enabled, not applied
    m.Push(off, 14);
    const auto lo2 = m.Latch(14, nullptr, nullptr);
    Check(same && lo1.h == 0 && lo1.path == ck::kPathSame && lo2.h == 0,
          "cockpit ipd T4: constant scale -> path same, h exact; scale 1 or applied clear -> h 0");
  }
  // Stepped jump with lag 1 learned: the frame of the jump takes the old record.
  {
    ck::Matcher m;
    std::vector<ck::Rec> sent;
    for (uint32_t n = 1; n <= 60; ++n) {
      const double scale = n < 10 ? 1.0 : n < 40 ? 1.0 + (n - 10) * 0.01 : 1.3;
      sent.push_back(MakeRec(n, scale));
      m.Push(sent.back(), n);
      const auto lo = m.Latch(n, nullptr, nullptr);
      m.Verify(ck::RecIpd(sent[sent.size() - 2 < sent.size() ? sent.size() - 2 : 0]), lo);
    }
    bool ok = m.lag == 1;
    // A jump 1.3 -> 1.0 at frame 61: DCS renders frame 61 with record 60 (1.3).
    sent.push_back(MakeRec(61, 1.0));
    m.Push(sent.back(), 61);
    const auto lo = m.Latch(61, nullptr, nullptr);
    // The lag's record and its neighbour differ by the whole step (9.8 mm): ambiguous. View 0 gets the smallest
    // plausible shift (0: the cockpit at DCS's camera), view 1's |C1 - C0| picks the record for views 1-3.
    double hv = -1, hNew = -1, hMid = -1;
    const bool vOld = m.VerifiedH(ck::RecIpd(sent[59]), lo, &hv);
    const bool vNew = m.VerifiedH(ck::RecIpd(sent[60]), lo, &hNew);
    const bool vMid = m.VerifiedH((ck::RecIpd(sent[59]) + ck::RecIpd(sent[60])) / 2, lo, &hMid);
    ok = ok && lo.path == ck::kPathLag && lo.ambiguous && lo.h == 0 && lo.hLow == 0 &&
         std::fabs(lo.hSpread - ck::RecH(sent[59])) < 1e-7 && std::fabs(ck::RecH(lo.rec) - ck::RecH(sent[59])) < 1e-7 &&
         m.ambs == 1 && vOld && std::fabs(hv - ck::RecH(sent[59])) < 1e-7 && vNew && hNew == 0 && !vMid;
    Check(ok, "cockpit ipd T4: stepped jump 1.3 -> 1.0 with lag 1: ambiguous (records 9.8 mm apart in h), view 0 not "
              "patched with the pick (smallest plausible shift 0); |C1 - C0| verifies the old or the new record, "
              "a separation between them verifies none");
  }
  // R77 ckipd-amb-window-capped: with lag 2 the pick is back index 2; a step between it and its older neighbour
  // (back index 3) is ambiguous (the window was capped at index 2), and view 1 can verify that neighbour.
  {
    ck::Matcher m;
    std::vector<ck::Rec> sent;
    auto frame = [&](uint32_t n, double scale) {
      sent.push_back(MakeRec(n, scale));
      m.Push(sent.back(), n);
      const auto lo = m.Latch(n, nullptr, nullptr);
      m.Verify(ck::RecIpd(sent[sent.size() >= 3 ? sent.size() - 3 : 0]), lo);
      return lo;
    };
    for (uint32_t n = 1; n <= 60; ++n) frame(n, n < 10 ? 1.0 : n < 40 ? 1.0 + (n - 10) * 0.01 : 1.3);
    const bool lag2 = m.lag == 2;
    frame(61, 1.0);
    frame(62, 1.4);
    frame(63, 1.4);
    sent.push_back(MakeRec(64, 1.42));
    m.Push(sent.back(), 64);
    const auto lo = m.Latch(64, nullptr, nullptr);  // pick = back 2 (record 62, x1.4); its older neighbour: 61, x1
    double hOld = -1, hPick = -1;
    const bool vOld = m.VerifiedH(ck::RecIpd(sent[60]), lo, &hOld);
    const bool vPick = m.VerifiedH(ck::RecIpd(sent[61]), lo, &hPick);
    char what[220];
    snprintf(what, sizeof(what),
             "R77: cockpit ipd lag %d: a step at the pick's older neighbour is ambiguous (path %d, pick %d, spread %.1f mm, "
             "h %.1f mm); view 1 verifies either record",
             m.lag, lo.path, lo.pick, lo.hSpread * 1e3, lo.h * 1e3);
    Check(lag2 && lo.path == ck::kPathLag && lo.pick == 2 && lo.ambiguous && lo.h == 0 && vOld && hOld == 0 && vPick &&
              std::fabs(hPick - ck::RecH(sent[61])) < 1e-7,
          what);
  }
  // Ambiguity needs a step > 2 mm in h between plausible records: a saccade step (< 4 mm IPD) and a ramp are
  // not ambiguous; a step down while boosted (1.4 -> 1.25) gives view 0 the smaller (new) shift, never
  // more than either record; a step up likewise keeps the old smaller shift.
  {
    auto run = [](double s0, double s1, ck::Matcher::LatchOut* out) {
      ck::Matcher m;
      for (uint32_t n = 1; n <= 5; ++n) {
        m.Push(MakeRec(n, s0), n);
        m.Latch(n, nullptr, nullptr);
      }
      m.Push(MakeRec(6, s1), 6);
      *out = m.Latch(6, nullptr, nullptr);
    };
    ck::Matcher::LatchOut sacc, down, up;
    run(1.30, 1.30 + 0.0038 / 0.0655, &sacc);  // a saccade step up (+3.8 mm IPD: h +1.9 mm)
    run(1.40, 1.25, &down);
    run(1.10, 1.40, &up);
    const double h125 = 0.0655 * 0.25 / 2, h110 = 0.0655 * 0.10 / 2;
    Check(!sacc.ambiguous && sacc.path == ck::kPathLag && std::fabs(sacc.h - ck::RecH(sacc.rec)) < 1e-9 &&
              down.ambiguous && std::fabs(down.h - h125) < 1e-6 && up.ambiguous && std::fabs(up.h - h110) < 1e-6,
          "cockpit ipd T4: a 2 mm step in h is not ambiguous (the lag's pick); larger steps down or up: view 0 "
          "gets the smaller plausible shift");
  }
  // Stale: no new record for 30 DCS frames -> stale (h 0).
  {
    ck::Matcher m;
    m.Push(MakeRec(1, 1.4), 1);
    const auto a = m.Latch(29, nullptr, nullptr);
    const auto b = m.Latch(31, nullptr, nullptr);
    ck::Matcher e;
    const auto c = e.Latch(1, nullptr, nullptr);
    Check(!a.stale && a.h > 0 && b.stale && b.h == 0 && c.stale && c.h == 0,
          "cockpit ipd T4: a record unchanged for 30 frames is stale (h 0); no record at all: h 0");
  }
  // pairFault: DCS renders 5 mm off at every lag for 10 frames -> h 0; 60 agreeing frames clear it.
  {
    ck::Matcher m;
    int faultAt = -1, clearAt = -1;
    for (uint32_t n = 1; n <= 120; ++n) {
      const ck::Rec r = MakeRec(n, 1.4);
      m.Push(r, n);
      const auto lo = m.Latch(n, nullptr, nullptr);
      const bool wrong = n <= 40;
      m.Verify(ck::RecIpd(r) + (wrong ? 0.005 : 0.0), lo);
      if (m.fault && faultAt < 0) faultAt = static_cast<int>(n);
      if (faultAt > 0 && !m.fault && clearAt < 0) clearAt = static_cast<int>(n);
    }
    ck::Matcher q;
    q.fault = true;
    q.Push(MakeRec(1, 1.4), 1);
    const auto lo = q.Latch(1, nullptr, nullptr);
    char what[160];
    snprintf(what, sizeof(what), "cockpit ipd T4: pairFault after 10 bad frames (frame %d), cleared after 60 good (frame %d); "
             "faulted latch h 0", faultAt, clearAt);
    Check(faultAt == 10 && clearAt == 100 && lo.h == 0, what);
  }
  // k: plain records with DCS at 1.1 x the eye distance -> k rejected; 1.003 -> fine, k 1 used.
  {
    ck::Matcher m, n2;
    for (uint32_t n = 1; n <= 40; ++n) {
      const ck::Rec r = MakeRec(n, 1.0);
      m.Push(r, n);
      n2.Push(r, n);
      const auto lo = m.Latch(n, nullptr, nullptr);
      m.Verify(r.base * 1.1, lo);
      const auto lo2 = n2.Latch(n, nullptr, nullptr);
      n2.Verify(r.base * 1.003, lo2);
    }
    Check(m.KBad() && std::fabs(m.kEst - 1.1) < 0.01 && !n2.KBad() && n2.K() == 1.0 && std::fabs(n2.bias - 0.0655 * 0.003) < 2e-5,
          "cockpit ipd T4: k 1.1 rejected (inert); k 1.003 accepted, k 1 used, the bias absorbs 0.2 mm");
  }
  // Early records (live 2026-10-13: boost reads == frames, dry 143): DCS locates frame k + 1 on another thread
  // while frame k renders, so the layer's record k + 1 sometimes lands before latch k. That latch sees two new
  // records, the next one none. Before the fix the empty latch went dry: h 0 for the whole frame while DCS
  // rendered it at the boosted eye (the cockpit, HUD and MFD jumped by h for one frame, about once per second).
  // Now every latch keeps the frame's own record, exactly, during a ramp too; nothing goes dry.
  {
    ck::Matcher m;
    std::vector<ck::Rec> sent(1);
    int wrong = 0, dry = 0, carried = 0, earlies = 0;
    uint32_t pushed = 0;
    for (uint32_t n = 1; n <= 240; ++n) {  // DCS frame n renders record n
      auto rec = [&](uint32_t k) {
        while (sent.size() <= k) sent.push_back(MakeRec(static_cast<uint32_t>(sent.size()), 1.0 + 0.002 * sent.size()));
        return sent[k];
      };
      while (pushed < n) m.Push(rec(++pushed), n);
      if (n % 7 == 3 && n < 230) {  // record n + 1 written before this frame's latch
        m.Push(rec(++pushed), n);
        ++earlies;
      }
      const auto lo = m.Latch(n, nullptr, nullptr);
      if (lo.stale || lo.dry) ++dry;
      if (lo.carried) ++carried;
      if (!(std::fabs(lo.h - ck::RecH(rec(n))) < 1e-9)) ++wrong;
      m.Verify(ck::RecIpd(rec(n)), lo);
    }
    char what[200];
    snprintf(what, sizeof(what),
             "cockpit ipd T4: early records (%d): the double latch takes the frame's own record, the empty one "
             "after it the banked one: every frame exact (wrong %d), none dry (%d), carried %d, lag %d",
             earlies, wrong, dry, carried, m.lag);
    Check(wrong == 0 && dry == 0 && carried == earlies && m.early == static_cast<uint64_t>(earlies) && m.lag == 0 &&
              !m.fault && m.switches == 0,
          what);
  }
  // A late record (the mirror case) is served by the newest one (one ramp step behind, no dry frame); the
  // layer stopping for good goes dry after kCarryMax + 1 latches (h 0).
  {
    ck::Matcher m;
    for (uint32_t n = 1; n <= 5; ++n) {
      m.Push(MakeRec(n, 1.30 + 0.002 * n), n);
      m.Latch(n, nullptr, nullptr);
    }
    const auto late = m.Latch(6, nullptr, nullptr);  // record 6 not there yet
    m.Push(MakeRec(6, 1.312), 7);
    m.Push(MakeRec(7, 1.314), 7);
    const auto catchUp = m.Latch(7, nullptr, nullptr);  // records 6 and 7: frame 7 renders 7
    const auto c1 = m.Latch(8, nullptr, nullptr);
    const auto c2 = m.Latch(9, nullptr, nullptr);
    const auto d3 = m.Latch(10, nullptr, nullptr);
    Check(late.carried && late.late && !late.stale && std::fabs(late.h - ck::RecH(MakeRec(5, 1.310))) < 1e-9 &&
              !catchUp.carried && std::fabs(catchUp.h - ck::RecH(MakeRec(7, 1.314))) < 1e-9 && c1.carried &&
              !c1.stale && c2.carried && !c2.stale && d3.dry && d3.stale && d3.h == 0,
          "cockpit ipd T4: a late record: the newest stands in (no dry frame), exact again with the next; the layer "
          "stopped: carried for kCarryMax latches, then dry (h 0)");
  }
  // The FOV fingerprint (pupil shift on): a ramp with lag 2 but the view's FOV names its record.
  {
    ck::Matcher m;
    ck::GazeRing g;
    std::vector<ck::Rec> sent;
    int fovPicks = 0, exact = 0, ramp = 0;
    for (uint32_t n = 1; n <= 50; ++n) {
      const double scale = n < 10 ? 1.0 : 1.0 + (n - 10) * 0.01;
      sent.push_back(MakeRec(n, scale));
      m.Push(sent.back(), n);
      ck::GazeRec gr;
      gr.displayTime = sent.back().displayTime;
      const float shift = 0.004f * static_cast<float>(n % 29);  // the pupil moves the FOV
      gr.fov[0] = -1.0f + shift, gr.fov[1] = 1.0f + shift, gr.fov[2] = 1.0f, gr.fov[3] = -1.0f;
      g.Push(gr);
      // DCS renders record n-2: its FOV.
      const ck::Rec& real = sent[sent.size() >= 3 ? sent.size() - 3 : 0];
      const double s2 = static_cast<double>(0.004f * static_cast<float>(real.frame % 29));
      const double fov[4] = {-1.0 + s2, 1.0 + s2, 1.0, -1.0};
      const auto lo = m.Latch(n, &g, fov);
      if (n > 12) {
        ++ramp;
        if (lo.path == ck::kPathFov) ++fovPicks;
        if (std::fabs(lo.h - ck::RecH(real)) < 1e-7) ++exact;
      }
    }
    // Pupil off: every gaze record has the same FOV -> not decisive -> the lag.
    ck::Matcher m2;
    ck::GazeRing g2;
    for (uint32_t n = 1; n <= 20; ++n) {
      m2.Push(MakeRec(n, 1.0 + n * 0.01), n);
      ck::GazeRec gr;
      gr.displayTime = 100000 + n * 11111;
      gr.fov[0] = -1, gr.fov[1] = 1, gr.fov[2] = 1, gr.fov[3] = -1;
      g2.Push(gr);
    }
    const double fov[4] = {-1, 1, 1, -1};
    const auto lo2 = m2.Latch(20, &g2, fov);
    Check(fovPicks == ramp && exact == ramp && lo2.path == ck::kPathLag,
          "cockpit ipd T4: FOV fingerprint (pupil shift on) picks the rendered record at lag 2 without learning; "
          "pupil still -> not decisive -> the learned lag");
  }
}

// ---------------------------------------------------------------------------
// T2 the upload filter (white box, CPU)
// ---------------------------------------------------------------------------

void ResetCk() {
  ck::ResetRender();
  ck::g_f = ck::Frame{};
}

void TestUploadFilter() {
  ResetCk();
  const bool oldOn = ck::g_on.load();
  ck::g_on = true;
  ck::g_cfgMode = 1;
  ID3D11Resource* const rA = reinterpret_cast<ID3D11Resource*>(static_cast<uintptr_t>(0x1000));
  ID3D11Resource* const rB = reinterpret_cast<ID3D11Resource*>(static_cast<uintptr_t>(0x2000));
  ID3D11Resource* const rU = reinterpret_cast<ID3D11Resource*>(static_cast<uintptr_t>(0x3000));
  auto setup = [&](double h) {
    ck::g_f = ck::Frame{};
    ck::g_f.latched = true;
    ck::g_f.h = h;
    ck::g_known[0] = ck::Known{rA, 0, ck::kCbBytes, 1};
    ck::g_known[1] = ck::Known{rB, 256, 0xB00, 1};
    ck::t_cls = ck::kClsGb;
    ck::t_view = 0;
    ck::g_restN = 0;
  };
  double R[3][3], C[3] = {0.5, 1.2, -0.2};
  Rot(10, 0, 0, R);
  const double fov[4] = {-1, 1, 1, -1};
  const Mat P = ProjOf(fov, 0.001, 0);
  std::vector<uint8_t> big(0xB00);
  uint8_t* s = big.data() + 256;
  BuildStruct(s, ViewInvOf(R, C), P, Mul(Inv(ViewInvOf(R, C)), P), 5);
  for (int i = 0; i < 256; ++i) big[i] = static_cast<uint8_t>(i);
  const double h = 0.0131;
  auto movedBy = [&](const uint8_t* p) {
    double c[3];
    CamOf(p, c);
    return (c[0] - C[0]) * R[0][0] + (c[1] - C[1]) * R[0][1] + (c[2] - C[2]) * R[0][2];
  };
  setup(h);
  const void* f1 = ck::OnUpload(rA, 0, nullptr, s, false);
  const bool nullOk = f1 == ck::g_scratch && std::fabs(movedBy(static_cast<const uint8_t*>(f1)) - h) < 1e-6 &&
                      ck::g_restN == 1 && !memcmp(ck::g_rest[0].bytes, s, ck::kCbBytes) && !ck::g_rest[0].hasBox;
  setup(h);
  const D3D11_BOX full{0, 0, 0, ck::kCbBytes, 1, 1};
  const void* f2 = ck::OnUpload(rA, 0, &full, s, false);
  const bool fullOk = f2 == ck::g_scratch && std::fabs(movedBy(static_cast<const uint8_t*>(f2)) - h) < 1e-6 &&
                      ck::g_rest[0].hasBox;
  setup(h);
  const D3D11_BOX part{0, 0, 0, 0x500, 1, 1};
  const uint64_t pb = ck::g_st.partialBox.load();
  const bool partOk = ck::OnUpload(rA, 0, &part, s, false) == s && ck::g_st.partialBox.load() == pb + 1;
  setup(h);
  const bool subOk = ck::OnUpload(rA, 1, nullptr, s, false) == s;
  setup(h);
  const bool unkOk = ck::OnUpload(rU, 0, nullptr, s, false) == s && ck::g_restN == 0;
  setup(h);
  const void* f3 = ck::OnUpload(rB, 0, nullptr, big.data(), true);
  const bool offOk = f3 == ck::g_scratch && !memcmp(ck::g_scratch, big.data(), 256) &&
                     std::fabs(movedBy(ck::g_scratch + 256) - h) < 1e-6 && ck::g_rest[0].us1 &&
                     ck::g_rest[0].len == 0xB00;
  setup(h);
  const D3D11_BOX at{256, 0, 0, 256 + ck::kCbBytes, 1, 1};
  const void* f4 = ck::OnUpload(rB, 0, &at, s, false);
  const bool boxOffOk = f4 == ck::g_scratch && std::fabs(movedBy(ck::g_scratch) - h) < 1e-6;
  Check(nullOk && fullOk, "cockpit ipd T2: null box and full box of a known cPerView -> patched private copy, "
                          "DCS's bytes kept for the restore");
  Check(partOk && subOk && unkOk, "cockpit ipd T2: partial box, sub != 0, unknown buffer -> DCS's pointer");
  Check(offOk && boxOffOk, "cockpit ipd T2: struct at a *SetConstantBuffers1 offset (null box and a box at the offset)");
  // Fallbacks forward the original pointer.
  setup(h);
  ck::t_cls = ck::kClsNone;
  const bool noneOk = ck::OnUpload(rA, 0, nullptr, s, false) == s;
  setup(h);
  ck::g_f.off = true;
  const bool offFrame = ck::OnUpload(rA, 0, nullptr, s, false) == s;
  setup(0);
  const bool zeroH = ck::OnUpload(rA, 0, nullptr, s, false) == s && ck::g_restN == 0;
  setup(h);
  uint8_t badS[ck::kCbBytes];
  memcpy(badS, s, sizeof(badS));
  float f = 2.0f;
  memcpy(badS + ck::kOffViewInv, &f, 4);
  const uint64_t sf = ck::g_st.selfFail.load();
  const bool selfOk = ck::OnUpload(rA, 0, nullptr, badS, false) == badS && ck::g_st.selfFail.load() == sf + 1 &&
                      ck::g_f.v[0].failed;
  setup(h);
  ck::g_cfgMode = 2;
  ck::g_f.observe = true;  // the frame latched in observe mode
  const uint64_t ob = ck::g_st.observed.load();
  const bool obsOk = ck::OnUpload(rA, 0, nullptr, s, false) == s && ck::g_st.observed.load() == ob + 1 &&
                     ck::g_f.v[0].patched && ck::g_restN == 0;
  ck::g_cfgMode = 1;
  setup(h);
  ck::t_cls = ck::kClsWatch;
  const bool watchOk = ck::OnUpload(rA, 0, nullptr, s, false) == s;
  Check(noneOk && offFrame && zeroH && selfOk && obsOk && watchOk,
        "cockpit ipd T2: no class, frame off, h 0 without Dp, self-check failure, observe mode, watched pass -> "
        "DCS's pointer");
  ResetCk();
  ck::g_restN = 0;
  ck::g_on = oldOn;
  ck::g_cfgMode = 0;
}

// ---------------------------------------------------------------------------
// T3 BoostOut
// ---------------------------------------------------------------------------

std::wstring ExeDir() {
  wchar_t p[MAX_PATH] = {};
  GetModuleFileNameW(nullptr, p, MAX_PATH);
  std::wstring s = p;
  return s.substr(0, s.find_last_of(L"\\/") + 1);
}
bool ReadAll(const wchar_t* path, std::string& out) {
  FILE* f = nullptr;
  if (_wfopen_s(&f, path, L"rb") || !f) return false;
  char buf[4096];
  size_t n;
  out.clear();
  while ((n = fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
  fclose(f);
  return true;
}

void TestBoostOut() {
  static_assert(sizeof(psh_BoostOut) == 56, "BoostOut 56 bytes");
  Check(offsetof(psh_BoostOut, displayTime) == 8 && offsetof(psh_BoostOut, flags) == 16 &&
            offsetof(psh_BoostOut, frame) == 20 && offsetof(psh_BoostOut, scale) == 24 &&
            offsetof(psh_BoostOut, baseIpdM) == 28 && offsetof(psh_BoostOut, renderedIpdM) == 32 &&
            psh_enabled() == 1 && psh_applied() == 2,
        "cockpit ipd T3: BoostOut layout (56 bytes, pack 8) and flags as the contract");
  const char* peer = getenv("QV_PERCEPTION_SHM_H");
  if (peer && *peer) {
    std::string a, b;
    wchar_t wp[MAX_PATH] = {};
    MultiByteToWideChar(CP_UTF8, 0, peer, -1, wp, MAX_PATH);
    // The module's own copy: DCSQV_MODULE_SHM_H when set (build layouts other
    // than test\ next to src\), else ..\src\ from the test exe.
    wchar_t own[MAX_PATH] = {};
    const std::wstring ours = GetEnvironmentVariableW(L"DCSQV_MODULE_SHM_H", own, MAX_PATH)
                                  ? std::wstring(own)
                                  : ExeDir() + L"..\\src\\dcsvr_perception_shm.h";
    // R107: identity as in the contract manifest (Dev's scripts\contracts.json): the content without CR bytes and a
    // leading UTF-8 BOM (each tree keeps its own line endings). A difference FAILS; unset, this is a listed SKIP.
    auto norm = [](std::string s) {
      if (s.size() >= 3 && static_cast<uint8_t>(s[0]) == 0xEF && static_cast<uint8_t>(s[1]) == 0xBB &&
          static_cast<uint8_t>(s[2]) == 0xBF)
        s.erase(0, 3);
      s.erase(std::remove(s.begin(), s.end(), '\r'), s.end());
      return s;
    };
    const bool readA = ReadAll(wp, a), readB = ReadAll(ours.c_str(), b);
    Check(readA && readB && norm(a) == norm(b),
          "cockpit ipd T3: src/dcsvr_perception_shm.h is the peer perception_shm.h (CR / BOM-normalised, as the manifest)");
  } else {
    QvSkip("cockpit ipd T3 byte compare: QV_PERCEPTION_SHM_H not set (path of the peer perception_shm.h)\n");
  }
  // The suffix variable names our mapping; the reader opens it read-only.
  char suffix[48];
  snprintf(suffix, sizeof(suffix), "_cktest%lu", GetCurrentProcessId());
  SetEnvironmentVariableA(psh_suffixVar(), suffix);
  std::wstring name = ck::BoostName();
  wchar_t want[96];
  swprintf_s(want, L"Local\\DcsVrDepthBoost_v1_cktest%lu", GetCurrentProcessId());
  HANDLE m = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, 4096, want);
  void* wv = m ? MapViewOfFile(m, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(psh_BoostOut)) : nullptr;
  ck::CloseBoost();
  const uint64_t opens = ck::g_boostOpens.load();
  ck::OpenBoost(true);
  Check(name == want && ck::g_boostView.load() && ck::g_boostOpens.load() == opens + 1,
        "cockpit ipd T3: DCSVR_PERCEPTION_SHM_SUFFIX honoured; the block opened read-only");
  // A writer thread against the render-thread reader (Poll): no torn record.
  ResetCk();
  std::atomic<bool> stop{false};
  std::thread w([&] {
    uint32_t n = 1;
    while (!stop.load()) {
      psh_BoostOut b{};
      b.displayTime = static_cast<int64_t>(n) * 7;
      b.flags = 3;
      b.frame = n;
      b.scale = 1.0f + static_cast<float>(n % 1000) * 1e-4f;
      b.baseIpdM = static_cast<float>(n % 997);
      b.renderedIpdM = static_cast<float>(n % 991);
      ::dcsvr_perception::seqlock_write(static_cast<psh_BoostOut*>(wv), b);
      ++n;
    }
  });
  int reads = 0, torn = 0;
  const ULONGLONG t0 = GetTickCount64();
  uint32_t lastFrame = 0;
  // At least 300 ms and until more than 50 records were read (R60: a deadline, not a fixed window: on a loaded
  // machine the writer and this reader got few overlapping slices in 300 ms, reads <= 50 with nothing torn).
  while (GetTickCount64() - t0 < 300 || (reads <= 50 && GetTickCount64() - t0 < 30000)) {
    ck::Poll();
    if (ck::g_match.count) {
      const ck::Rec& r = ck::g_match.At(0);
      if (r.frame != lastFrame) {
        ++reads;
        lastFrame = r.frame;
        if (r.displayTime != static_cast<int64_t>(r.frame) * 7 || r.base != static_cast<float>(r.frame % 997) ||
            r.rendered != static_cast<float>(r.frame % 991) ||
            r.scale != 1.0f + static_cast<float>(r.frame % 1000) * 1e-4f)
          ++torn;
      }
    }
  }
  stop = true;
  w.join();
  char what[160];
  snprintf(what, sizeof(what), "cockpit ipd T3: seqlock reader against a writer thread: %d records, %d torn", reads, torn);
  Check(reads > 50 && torn == 0, what);
  ck::CloseBoost();
  ResetCk();
  SetEnvironmentVariableA(psh_suffixVar(), nullptr);
  if (wv) UnmapViewOfFile(wv);
  if (m) CloseHandle(m);
}

// ---------------------------------------------------------------------------
// Overlay passes: the collimated HUD under the patch, the MVP probe (pure)
// ---------------------------------------------------------------------------

void TestOverlayMath() {
  bool dirOk = true, relOk = true, rotOk = true, finiteMoves = true;
  double worstRel = 0;
  for (int it = 0; it < 100; ++it) {
    double R[3][3], C[3];
    Rot(Rnd(-180, 180), Rnd(-40, 40), Rnd(-10, 10), R);
    for (double& x : C) x = Rnd(-500, 500);
    const double fov[4] = {-1.1, 1.0, 1.0, -1.1};
    alignas(16) uint8_t s[ck::kCbBytes], p[ck::kCbBytes];
    const Mat VI = ViewInvOf(R, C), P = ProjOf(fov, 0.001, -0.001);
    BuildStruct(s, VI, P, Mul(Inv(VI), P), 7 + it);
    memcpy(p, s, sizeof(s));
    const double h = Rnd(3e-3, 15e-3);
    const double dw[3] = {h * R[0][0], h * R[0][1], h * R[0][2]}, dp[3] = {0, 0, 0};
    ck::PatchStruct(p, dw, dp);
    const Mat VP = Get(s, ck::kOffViewProj), VP2 = Get(p, ck::kOffViewProj);
    const Mat V = Get(s, ck::kOffView), V2 = Get(p, ck::kOffView);
    double d[3];
    const double ox = Rnd(-0.3, 0.3), oy = Rnd(-0.3, 0.3);
    for (int i = 0; i < 3; ++i) d[i] = R[2][i] + ox * R[0][i] + oy * R[1][i];
    // A symbol at infinity (w = 0): rows 0..2 only, bit-identical after the patch.
    for (int c = 0; c < 4; ++c) {
      const double a = d[0] * VP.a[0][c] + d[1] * VP.a[1][c] + d[2] * VP.a[2][c];
      const double b = d[0] * VP2.a[0][c] + d[1] * VP2.a[1][c] + d[2] * VP2.a[2][c];
      if (a != b) dirOk = false;
    }
    // A rotation-only view (gView's 3x3): unchanged.
    for (int r = 0; r < 3; ++r)
      for (int c = 0; c < 3; ++c)
        if (V.a[r][c] != V2.a[r][c]) rotOk = false;
    // Camera-relative at a far distance F: gCameraPos + d * F through gViewProj.
    float cp[3], cp2[3];
    memcpy(cp, s + ck::kOffCamPos, sizeof(cp));
    memcpy(cp2, p + ck::kOffCamPos, sizeof(cp2));
    const double F = 1000;
    double q1[3], q2[3], n1[3], n2[3];
    for (int i = 0; i < 3; ++i) q1[i] = cp[i] + d[i] * F, q2[i] = cp2[i] + d[i] * F;
    Project(VP, q1, n1);
    Project(VP2, q2, n2);
    const double e = std::max(std::fabs(n1[0] - n2[0]), std::fabs(n1[1] - n2[1]));
    worstRel = std::max(worstRel, e);
    if (!(e < 2e-5)) relOk = false;
    // A symbol at a finite cockpit position (0.6 m ahead) moves with the cockpit (about h / 0.6 rad).
    double f1[3], f2[3], pf[3];
    for (int i = 0; i < 3; ++i) pf[i] = C[i] + 0.6 * d[i];
    Project(VP, pf, f1);
    Project(VP2, pf, f2);
    if (!(std::fabs(f1[0] - f2[0]) > 2e-3)) finiteMoves = false;
  }
  printf("     (overlay HUD: camera-relative symbol at 1 km, worst NDC change %.2g)\n", worstRel);
  Check(dirOk && rotOk && relOk && finiteMoves,
        "cockpit ipd overlay HUD: the patch leaves a collimated HUD where it is (w = 0 symbols bit-identical, a "
        "rotation-only view unchanged, gCameraPos + d * 1 km within 2e-5 NDC); a finite cockpit symbol moves");

  // The probe: an MVP of the view's camera in an effect buffer.
  bool mvp = true, vpSelf = true, none = true, transposed = true, jitter = true;
  for (int it = 0; it < 50; ++it) {
    double R[3][3], C[3], Rm[3][3], Cm[3];
    Rot(Rnd(-180, 180), Rnd(-40, 40), Rnd(-10, 10), R);
    Rot(Rnd(-180, 180), Rnd(-40, 40), Rnd(-10, 10), Rm);
    for (int i = 0; i < 3; ++i) C[i] = Rnd(-500, 500), Cm[i] = C[i] + Rnd(-1, 1);
    const double fov[4] = {-1.1, 1.0, 1.0, -1.1};
    const Mat V = Inv(ViewInvOf(R, C)), Pj = ProjOf(fov, 0.0013, -0.0007), P0 = ProjOf(fov, 0, 0);
    ck::M4 vp;
    const Mat VPj = Mul(V, Pj);
    for (int r = 0; r < 4; ++r)
      for (int c = 0; c < 4; ++c) vp.a[r][c] = static_cast<float>(VPj.a[r][c]);
    Mat Model = ViewInvOf(Rm, Cm);
    const double sc = Rnd(0.001, 2);
    for (int r = 0; r < 3; ++r)
      for (int c = 0; c < 3; ++c) Model.a[r][c] *= sc;
    alignas(16) uint8_t buf[512], rnd[512];
    for (UINT i = 0; i < sizeof(rnd); i += 4) {  // other effect constants: finite values
      const float v = static_cast<float>(Rnd(-10, 10));
      memcpy(rnd + i, &v, 4);
    }
    memcpy(buf, rnd, sizeof(buf));
    const UINT at = 16 * static_cast<UINT>(1 + it % 20);
    Put(buf, at, Mul(Model, VPj));
    mvp = mvp && ck::MvpOfCamera(buf, sizeof(buf), vp) == 1;
    Put(buf, at, VPj);
    vpSelf = vpSelf && ck::MvpOfCamera(buf, sizeof(buf), vp) == 2;
    none = none && ck::MvpOfCamera(rnd, sizeof(rnd), vp) == 0;
    // Row-major storage.
    const Mat M = Mul(Model, VPj);
    float f[16];
    for (int r = 0; r < 4; ++r)
      for (int c = 0; c < 4; ++c) f[r * 4 + c] = static_cast<float>(M.a[r][c]);
    memcpy(buf, rnd, sizeof(buf));
    memcpy(buf + at, f, sizeof(f));
    transposed = transposed && ck::MvpOfCamera(buf, sizeof(buf), vp) == 1;
    // The symbology's MVP built with the unjittered projection, cPerView's VP jittered.
    memcpy(buf, rnd, sizeof(buf));
    Put(buf, at, Mul(Model, Mul(V, P0)));
    jitter = jitter && ck::MvpOfCamera(buf, sizeof(buf), vp) == 1;
  }
  Check(mvp && vpSelf && none && transposed && jitter,
        "cockpit ipd overlay probe: a model * view * projection of the view's camera found in an effect buffer "
        "(either storage order, unjittered against the jittered cPerView), the VP itself told apart, none in a "
        "pattern");
}

// Overlay MVPs (CockpitTrueIpdOverlayMvp): the effect buffers' CPU-built MVP
// of the view's boosted camera rewritten for the true eye (pure).
void TestOverlayMvp() {
  bool roundTrip = true, rowsBits = true, othersSame = true, infOk = true, transOk = true, countOk = true,
       projOk = true;
  double worstRt = 0, worstNdc = 0;
  for (int it = 0; it < 60; ++it) {
    double R[3][3], C[3], Rm[3][3], Cm[3];
    Rot(Rnd(-180, 180), Rnd(-40, 40), Rnd(-10, 10), R);
    Rot(Rnd(-180, 180), Rnd(-60, 60), Rnd(-30, 30), Rm);
    for (int i = 0; i < 3; ++i) C[i] = Rnd(-300, 300), Cm[i] = C[i] + Rnd(-1, 1);
    const double fov[4] = {-1.1, 1.0, 1.0, -1.1};
    const Mat V = Inv(ViewInvOf(R, C)), Pj = ProjOf(fov, 0.0011, -0.0009), P0 = ProjOf(fov, 0, 0);
    // DCS's (boosted) gViewProj as cPerView holds it (floats).
    alignas(16) uint8_t vpb[64];
    Put(vpb, 0, Mul(V, Pj));
    const Mat VPf = Get(vpb, 0);
    ck::M4 vp;
    for (int r = 0; r < 4; ++r)
      for (int c = 0; c < 4; ++c) vp.a[r][c] = VPf.a[r][c];
    // A rigid model with a per-axis scale (an MFD page: glyph units to metres), near the camera.
    Mat Model = ViewInvOf(Rm, Cm);
    for (int r = 0; r < 3; ++r) {
      const double sc = it % 3 == 0 ? Rnd(0.0005, 0.01) : Rnd(0.05, 2);
      for (int c = 0; c < 3; ++c) Model.a[r][c] *= sc;
    }
    // Every other symbol uses the unjittered projection (the probe found both).
    const Mat MVP = Mul(Model, it % 2 ? Mul(V, P0) : VPf);
    alignas(16) uint8_t buf[512], orig[512];
    for (UINT i = 0; i < sizeof(buf); i += 4) {
      const float v = static_cast<float>(Rnd(-10, 10));
      memcpy(buf + i, &v, 4);
    }
    const UINT at = 16 * static_cast<UINT>(1 + it % 20);
    const bool rowMajor = it % 4 == 3;  // the other storage order
    if (rowMajor) {
      float f[16];
      for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) f[r * 4 + c] = static_cast<float>(MVP.a[r][c]);
      memcpy(buf + at, f, sizeof(f));
    } else {
      Put(buf, at, MVP);
    }
    memcpy(orig, buf, sizeof(buf));
    auto getAt = [&](const uint8_t* b) {
      Mat m = Get(b, at);
      if (rowMajor) {
        Mat t;
        for (int r = 0; r < 4; ++r)
          for (int c = 0; c < 4; ++c) t.a[r][c] = m.a[c][r];
        m = t;
      }
      return m;
    };
    const double h = Rnd(3e-3, 15e-3) * (it % 2 ? -1 : 1);
    const double dw[3] = {h * R[0][0], h * R[0][1], h * R[0][2]};
    const ck::MvpScan sc = ck::PatchMvps(buf, sizeof(buf), vp, dw, C);
    countOk = countOk && sc.patched == 1 && sc.implausible == 0;
    // The true eye's camera: VP' = T(-Dw) * VP (cPerView's gViewProj patch). MVP' must be the model through it.
    double mdw[3] = {-dw[0], -dw[1], -dw[2]};
    const Mat VPp = Mul(Trans(mdw), VPf);
    const Mat got0 = getAt(orig), got = getAt(buf);
    const Mat X = Mul(got0, Inv(VPf));  // DCS's model as the patch recovers it (with the jitter shear)
    const Mat want = Mul(X, VPp);
    const double e = MaxDiff(got, want) / std::max(1.0, MaxAbs(want));
    worstRt = std::max(worstRt, e);
    if (!(e < 2e-6)) roundTrip = false;
    // Model-space points (of world points 0.3-2 m ahead of the camera) through MVP' land where the model
    // through the true eye's camera puts them.
    const Mat Mi = Inv(Model);
    for (int k = 0; k < 4; ++k) {
      const double dist = Rnd(0.3, 2), lx = Rnd(-0.3, 0.3), ly = Rnd(-0.3, 0.3);
      double q[3], p[3];
      for (int i = 0; i < 3; ++i) q[i] = C[i] + dist * (R[2][i] + lx * R[0][i] + ly * R[1][i]);
      for (int i = 0; i < 3; ++i) p[i] = q[0] * Mi.a[0][i] + q[1] * Mi.a[1][i] + q[2] * Mi.a[2][i] + Mi.a[3][i];
      double n1[3], n2[3];
      Project(got, p, n1);
      Project(Mul(Model, Mul(Trans(mdw), it % 2 ? Mul(V, P0) : VPf)), p, n2);
      const double d = std::max(std::fabs(n1[0] - n2[0]), std::fabs(n1[1] - n2[1]));
      worstNdc = std::max(worstNdc, d);
      if (!(d < 2e-4)) projOk = false;
    }
    // Rows 0..2 keep their exact bits; the rest of the buffer is untouched.
    for (int r = 0; r < 3; ++r)
      for (int c = 0; c < 4; ++c)
        if (got.a[r][c] != got0.a[r][c]) rowsBits = false;
    if (memcmp(buf, orig, at) || memcmp(buf + at + 64, orig + at + 64, sizeof(buf) - at - 64)) othersSame = false;
    // A symbol at infinity (w = 0): bit-identical clip coordinates (the collimated HUD / JHMCS stay put).
    const double d[3] = {Rnd(-1, 1), Rnd(-1, 1), 1};
    for (int c = 0; c < 4; ++c) {
      const double a = d[0] * got0.a[0][c] + d[1] * got0.a[1][c] + d[2] * got0.a[2][c];
      const double b = d[0] * got.a[0][c] + d[1] * got.a[1][c] + d[2] * got.a[2][c];
      if (a != b) infOk = false;
    }
    if (rowMajor && !(got.a[3][0] != got0.a[3][0] || got.a[3][1] != got0.a[3][1])) transOk = false;
  }
  printf("     (overlay MVP: worst round trip %.2g relative, worst NDC %.2g)\n", worstRt, worstNdc);
  Check(countOk && roundTrip && projOk,
        "cockpit ipd overlay MVP: an MVP of the boosted camera (model with per-axis scale, jittered or unjittered "
        "projection, either storage order) becomes model * T(-Dw) * VP: round trip against VP' within 2e-6, "
        "model points within 2e-4 NDC of the true eye's projection");
  Check(rowsBits && othersSame && infOk && transOk,
        "cockpit ipd overlay MVP: only row 3 changes (rows 0..2 bit-identical, the row-major copy patched in its own "
        "order); symbols at infinity (w = 0, collimated HUD) bit-identical; every other byte untouched");

  // The gate (2026-10-13): every affine model is patched (the row-3 delta is exact for it): a model scaled 5 km per
  // unit, a sheared one, one 300 km away, a symbol rotated 45 deg in a 0.75-aspect indicator (R * S(1, 0.75): its
  // axes 74 deg apart flipped between patched and DCS's camera with the angle), world-space symbology (model =
  // identity, the camera 900 km from the world origin); only a non-transform (scale beyond 1e7) is refused.
  {
    double R[3][3], C[3] = {12, 3, -40};
    Rot(30, 5, 0, R);
    const double fov[4] = {-1.1, 1.0, 1.0, -1.1};
    alignas(16) uint8_t vpb[64];
    Put(vpb, 0, Mul(Inv(ViewInvOf(R, C)), ProjOf(fov, 0.001, 0.001)));
    const Mat VPf = Get(vpb, 0);
    ck::M4 vp;
    for (int r = 0; r < 4; ++r)
      for (int c = 0; c < 4; ++c) vp.a[r][c] = VPf.a[r][c];
    const double dw[3] = {0.01 * R[0][0], 0.01 * R[0][1], 0.01 * R[0][2]};
    auto run = [&](const Mat& model, ck::MvpScan* out) {
      alignas(16) uint8_t buf[256], orig[256];
      for (UINT i = 0; i < sizeof(buf); i += 4) {
        const float v = 0.37f + 0.11f * static_cast<float>(i / 4);
        memcpy(buf + i, &v, 4);
      }
      Put(buf, 64, Mul(model, VPf));
      memcpy(orig, buf, sizeof(buf));
      *out = ck::PatchMvps(buf, sizeof(buf), vp, dw, C);
      return !memcmp(buf, orig, sizeof(buf));
    };
    // The patched matrix must be X * T(-dw) * VP for the model X the patch recovers.
    double mdw[3] = {-dw[0], -dw[1], -dw[2]};
    auto exact = [&](const Mat& model) {
      alignas(16) uint8_t buf[256];
      for (UINT i = 0; i < sizeof(buf); i += 4) {
        const float v = 0.37f + 0.11f * static_cast<float>(i / 4);
        memcpy(buf + i, &v, 4);
      }
      Put(buf, 64, Mul(model, VPf));
      const Mat got0 = Get(buf, 64);
      ck::PatchMvps(buf, sizeof(buf), vp, dw, C);
      const Mat want = Mul(Mul(got0, Inv(VPf)), Mul(Trans(mdw), VPf)), got = Get(buf, 64);
      return MaxDiff(got, want) <= 2e-6 * std::max(1.0, MaxAbs(want));
    };
    double Rm[3][3];
    Rot(10, 20, 30, Rm);
    double Cm[3] = {C[0] + 0.5, C[1], C[2] + 0.4};
    Mat big = ViewInvOf(Rm, Cm), shear = ViewInvOf(Rm, Cm), distant = ViewInvOf(Rm, Cm), good = ViewInvOf(Rm, Cm),
        huge = ViewInvOf(Rm, Cm);
    for (int r = 0; r < 3; ++r)
      for (int c = 0; c < 3; ++c) big.a[r][c] *= 5e3, huge.a[r][c] *= 5e8;  // 5 km / 500 000 km per unit
    for (int c = 0; c < 3; ++c) shear.a[1][c] = 0.8 * Rm[0][c] + 0.6 * Rm[1][c];  // 37 degrees off the x axis
    distant.a[3][0] += 3e5;                                                          // 300 km from the camera
    // R(45 deg) * S(1, 0.75) in the indicator plane (glyph units of 2 mm), then the indicator's placement.
    Mat aspect = Ident();
    {
      const double cs = std::cos(0.7853981633974483), sn = std::sin(0.7853981633974483);
      Mat rs = Ident();
      rs.a[0][0] = cs, rs.a[0][1] = 0.75 * sn, rs.a[1][0] = -sn, rs.a[1][1] = 0.75 * cs;
      Mat place = ViewInvOf(Rm, Cm);
      for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c) place.a[r][c] *= 0.002;
      aspect = Mul(rs, place);
    }
    ck::MvpScan sBig, sShear, sFar, sOk, sNone, sAsp, sHuge;
    const bool bigChanged = !run(big, &sBig), shearChanged = !run(shear, &sShear), farChanged = !run(distant, &sFar),
               aspChanged = !run(aspect, &sAsp), hugeSame = run(huge, &sHuge);
    const bool okChanged = !run(good, &sOk);
    alignas(16) uint8_t pat[256], pat0[256];
    for (UINT i = 0; i < sizeof(pat); i += 4) {
      const float v = static_cast<float>(Rnd(-10, 10));
      memcpy(pat + i, &v, 4);
    }
    memcpy(pat0, pat, sizeof(pat));
    sNone = ck::PatchMvps(pat, sizeof(pat), vp, dw, C);
    const bool exactAll = exact(big) && exact(shear) && exact(distant) && exact(aspect) && exact(good);
    Check(bigChanged && shearChanged && farChanged && aspChanged && okChanged && sBig.patched == 1 &&
              sShear.patched == 1 && sFar.patched == 1 && sFar.nFar == 1 && sAsp.patched == 1 && sOk.patched == 1 &&
              sBig.implausible + sShear.implausible + sFar.implausible + sAsp.implausible + sOk.implausible == 0 &&
              exactAll && hugeSame && sHuge.implausible == 1 && sHuge.patched == 0 && sNone.patched == 0 &&
              sNone.implausible == 0 && !memcmp(pat, pat0, sizeof(pat)),
          "cockpit ipd overlay MVP gate: models scaled 5 km per unit, sheared, 300 km away (counted far), rotated "
          "45 deg in a 0.75-aspect indicator: patched exactly (X * T(-Dw) * VP); a 5e8 scale refused and counted; "
          "constants without an MVP untouched");
    // World-space symbology: the effect's own (unjittered) view-projection, model = identity, the camera 2 km
    // from the render origin (whatever the old gate made of it, it is a camera matrix of the view: patched).
    {
      double Rw[3][3], Cw[3] = {1.6e3, 40, -1.2e3};
      Rot(40, 5, 0, Rw);
      alignas(16) uint8_t vb[64];
      Put(vb, 0, Mul(Inv(ViewInvOf(Rw, Cw)), ProjOf(fov, 0.0012, -0.0008)));
      const Mat VPw = Get(vb, 0);
      ck::M4 vpw;
      for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) vpw.a[r][c] = VPw.a[r][c];
      const double dww[3] = {0.0131 * Rw[0][0], 0.0131 * Rw[0][1], 0.0131 * Rw[0][2]};
      alignas(16) uint8_t buf[256] = {};
      Put(buf, 0, Mul(Inv(ViewInvOf(Rw, Cw)), ProjOf(fov, 0, 0)));  // unjittered: X ~ identity plus the jitter shear
      const Mat w0 = Get(buf, 0);
      const ck::MvpScan sw = ck::PatchMvps(buf, sizeof(buf), vpw, dww, Cw);
      double nd[3] = {-dww[0], -dww[1], -dww[2]};
      const Mat want = Mul(Mul(w0, Inv(VPw)), Mul(Trans(nd), VPw)), got = Get(buf, 0);
      Check(sw.patched == 1 && sw.implausible == 0 &&
                MaxDiff(got, want) <= 2e-6 * std::max(1.0, MaxAbs(want)),
            "cockpit ipd overlay MVP gate: world-space symbology (the effect's own unjittered VP, camera 2 km "
            "from the origin) patched to the true eye like the MVPs");
    }
    // font.fx: cv0..cv5, charArray[128], worldSpace, then the MVP at byte 2160 (scanned: the whole buffer).
    {
      std::vector<uint8_t> font(2240);
      for (size_t i = 0; i + 4 <= font.size(); i += 4) {
        const float v = 0.25f + 0.01f * static_cast<float>(i / 4 % 97);
        memcpy(font.data() + i, &v, 4);
      }
      Put(font.data(), 2160, Mul(good, VPf));
      const std::vector<uint8_t> f0 = font;
      const ck::MvpScan sf = ck::PatchMvps(font.data(), static_cast<UINT>(font.size()), vp, dw, C);
      Check(sf.patched == 1 && memcmp(font.data(), f0.data(), 2160) == 0 &&
                memcmp(font.data() + 2160, f0.data() + 2160, 64) != 0,
            "cockpit ipd overlay MVP: font.fx's layout (MVP at byte 2160) found and patched, the rest untouched");
    }
    Check(ck::PlausibleModel(ck::M4{{{0.002, 0, 0, 0}, {0, 0.002, 0, 0}, {0, 0, 0, 0}, {C[0], C[1], C[2] + 0.6, 1}}}, C),
          "cockpit ipd overlay MVP gate: a flat (2D) model with a zero z axis is plausible");
  }
}

// A d3d11 variant of the Draw entry (as a Flush / ExecuteCommandList switches
// it to): forwards to the real draw.
using DrawT = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT);
DrawT g_realDraw = nullptr;
int g_variantDraws = 0;
void STDMETHODCALLTYPE VariantDraw(ID3D11DeviceContext* c, UINT n, UINT sv) {
  ++g_variantDraws;
  g_realDraw(c, n, sv);
}

// ---------------------------------------------------------------------------
// T5/T7/T8 the device pipeline
// ---------------------------------------------------------------------------

// A GBufferPass object as gbpass::ItemVector and plod::KindRaw read it: name
// length at +8, name at +0x10, the resource node at +0x88 (word +0x38 = the
// collection index).
struct GbPass {
  void* vt = nullptr;
  uint64_t len = 0;
  char name[0x78] = {};
  uint8_t* node = nullptr;
  uint8_t pad[0x40] = {};
};
static_assert(offsetof(GbPass, node) == 0x88, "GbPass layout");

// The render graph: item vectors at [rg+0x438], descriptors at [rg+0x408].
struct Graph {
  std::vector<uint8_t> rg = std::vector<uint8_t>(0x1000, 0);
  std::vector<uint8_t> vecs = std::vector<uint8_t>(64 * 24, 0);
  std::vector<uint8_t> descs;
  std::vector<std::vector<uint8_t>> nodes;
  void* ctxBlock[2] = {};
  int Add(uint16_t sm, uint32_t tag) {
    const size_t k = descs.size() / gbrec::kCollStride;
    descs.resize(descs.size() + gbrec::kCollStride, 0);
    uint8_t* d = descs.data() + k * gbrec::kCollStride;
    memcpy(d, &sm, 2);
    memcpy(d + gbrec::kCollTag, &tag, 4);
    return static_cast<int>(k);
  }
  void Finish() {
    uint8_t* r = rg.data();
    *reinterpret_cast<uint8_t**>(r + 0x438) = vecs.data();
    *reinterpret_cast<uint8_t**>(r + gbrec::kCollDescs) = descs.data();
    *reinterpret_cast<uint8_t**>(r + gbrec::kCollDescs + 8) = descs.data() + descs.size();
    ctxBlock[0] = r;
  }
  GbPass Pass(int idx) {
    nodes.emplace_back(static_cast<size_t>(0x40), static_cast<uint8_t>(0));
    const int16_t i = static_cast<int16_t>(idx);
    memcpy(nodes.back().data() + 0x38, &i, 2);
    GbPass p;
    p.len = 11;
    strcpy_s(p.name, "GBufferPass");
    p.node = nodes.back().data();
    return p;
  }
};

ID3D11DeviceContext1* g_c = nullptr;
// What the fake slot-19 body does (DCS's binder: bind the frame buffer, set
// the viewport, upload cPerView, bind it at VS/PS b7; then the GPU's view of
// the buffer is captured as a draw would see it).
struct Exec {
  ID3D11Buffer* cb = nullptr;
  UINT first = 0, num = 0;
  const uint8_t* data = nullptr;
  ID3D11DepthStencilView* dsv = nullptr;
  ID3D11DepthStencilState* dss = nullptr;
  UINT ref = 0;
  std::vector<uint8_t>* inPass = nullptr;
  bool swapSlot = false;
  int calls = 0;
};
Exec g_x;
ID3D11Buffer* g_stage[3] = {};  // staging per buffer size class

std::vector<uint8_t> Snapshot(ID3D11Buffer* cb) {
  D3D11_BUFFER_DESC d{};
  cb->GetDesc(&d);
  ID3D11Device* dev = nullptr;
  cb->GetDevice(&dev);
  D3D11_BUFFER_DESC sd{};
  sd.ByteWidth = d.ByteWidth;
  sd.Usage = D3D11_USAGE_STAGING;
  sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  ID3D11Buffer* st = nullptr;
  dev->CreateBuffer(&sd, nullptr, &st);
  dev->Release();
  std::vector<uint8_t> out;
  if (!st) return out;
  g_c->CopyResource(st, cb);
  D3D11_MAPPED_SUBRESOURCE m{};
  if (SUCCEEDED(g_c->Map(st, 0, D3D11_MAP_READ, 0, &m))) {
    out.assign(static_cast<const uint8_t*>(m.pData), static_cast<const uint8_t*>(m.pData) + d.ByteWidth);
    g_c->Unmap(st, 0);
  }
  st->Release();
  return out;
}

void BindB7(ID3D11Buffer* cb, UINT first, UINT num) {
  if (first) {
    g_c->VSSetConstantBuffers1(7, 1, &cb, &first, &num);
    g_c->PSSetConstantBuffers1(7, 1, &cb, &first, &num);
  } else {
    g_c->VSSetConstantBuffers(7, 1, &cb);
    g_c->PSSetConstantBuffers(7, 1, &cb);
  }
}

// Helmet models (cockpit_ipd.h's helmet section): fake renderables of DCS's item loops. Each records, when DCS's
// loop "draws" it, the struct bound at VS b7 then (its buffer read back at the bound offset), per view.
struct HName {
  uint64_t pad = 0;
  char s[64] = {};  // the model path at +8 (material +0x20 points here)
};
struct HMat {
  uint8_t b[0x40] = {};
};
struct HItem {
  uint8_t b[0x100] = {};
};
struct HRend {
  void** vt = nullptr;
  uint64_t pad = 0;
  uint8_t* item = nullptr;  // +0x10
  uint8_t rest[0x60] = {};
};
constexpr int kHItems = 6;
HRend* g_hRend = nullptr;
void* g_hVt[8] = {};
std::vector<uint8_t> g_hSeen[2][4][kHItems];  // [G-buffer / forward][view][item]
bool g_hPsSame[2][4][kHItems] = {};          // PS b7 bound to the same buffer as VS b7 at the draw
int g_hPassKind = 0;                           // 0 the cockpit G-buffer pass, 1 the forward pass
uint64_t __fastcall HDraw(void* self, void* data) {
  (void)data;
  int i = -1;
  for (int k = 0; k < kHItems && g_hRend; ++k)
    if (self == &g_hRend[k]) i = k;
  const int t = fixdepth::g_s.hmdOrd;
  if (i < 0 || t < 0 || t > 3) return 0;
  ID3D11Buffer *vb = nullptr, *pb = nullptr;
  UINT vf = 0, vn = 0, pf = 0, pn = 0;
  g_c->VSGetConstantBuffers1(7, 1, &vb, &vf, &vn);
  g_c->PSGetConstantBuffers1(7, 1, &pb, &pf, &pn);
  std::vector<uint8_t> v = vb ? Snapshot(vb) : std::vector<uint8_t>();
  g_hSeen[g_hPassKind][t][i].assign(v.size() >= vf * 16 + ck::kCbBytes ? v.begin() + vf * 16 : v.end(),
                                    v.size() >= vf * 16 + ck::kCbBytes ? v.begin() + vf * 16 + ck::kCbBytes : v.end());
  g_hPsSame[g_hPassKind][t][i] = pb == vb && pf == vf;
  if (vb) vb->Release();
  if (pb) pb->Release();
  return 0;
}
// The ownship test (CockpitTrueIpdOwnship): world items whose draws record VS b7 per pass kind (g_oK: 1 the world
// G-buffer execution, 2 the decal execution, 3 the world's TransparentPass), view and item, and whether the
// recorder was told to step aside at that draw.
constexpr int kOItems = 4;
HRend* g_oRend = nullptr;
void* g_oVt[8] = {};
int g_oK = 0;
bool g_noUploadWorld = false;  // the world G-buffer / decal executions upload no cPerView (they draw with the buffer as
                               // the view's cockpit pass left it: DCS's bytes, put back by its restore)
std::vector<uint8_t> g_oSeen[4][4][kOItems];
bool g_oAside[4][4][kOItems] = {};
// The recorder's step aside (gbrec::AsideRun): DCS's own execute, counted.
int g_asideCalls = 0;
void* g_asidePass = nullptr;
void __fastcall AsideExec(void* pass, void* ctx) {
  (void)ctx;
  ++g_asideCalls;
  g_asidePass = pass;
}
uint64_t __fastcall ODraw(void* self, void* data) {
  (void)data;
  int i = -1;
  for (int k = 0; k < kOItems && g_oRend; ++k)
    if (self == &g_oRend[k]) i = k;
  const int t = fixdepth::g_s.hmdOrd;
  if (i < 0 || t < 0 || t > 3 || g_oK < 1 || g_oK > 3) return 0;
  ID3D11Buffer* vb = nullptr;
  UINT vf = 0, vn = 0;
  g_c->VSGetConstantBuffers1(7, 1, &vb, &vf, &vn);
  std::vector<uint8_t> v = vb ? Snapshot(vb) : std::vector<uint8_t>();
  g_oSeen[g_oK][t][i].assign(v.size() >= vf * 16 + ck::kCbBytes ? v.begin() + vf * 16 : v.end(),
                             v.size() >= vf * 16 + ck::kCbBytes ? v.begin() + vf * 16 + ck::kCbBytes : v.end());
  g_oAside[g_oK][t][i] = gbrec::t_ownAside;
  if (vb) vb->Release();
  return 0;
}
// DCS's item loop: the vector read once, (*p)->vt[1](p, data) per entry [V GraphicsCore 0x896e1-0x89713].
void RunItems(void** vec) {
  if (!vec || !vec[0]) return;
  auto** b = static_cast<uint8_t**>(vec[0]);
  auto** e = static_cast<uint8_t**>(vec[1]);
  for (auto** p = b; p < e; ++p) {
    void** vt = *reinterpret_cast<void***>(*p);
    reinterpret_cast<uint64_t(__fastcall*)(void*, void*)>(vt[1])(*p, nullptr);
  }
}

void __fastcall FakeExec(void* pass, void* ctx) {
  ++g_x.calls;
  ID3D11RenderTargetView* none = nullptr;
  g_c->OMSetRenderTargets(1, &none, g_x.dsv);
  if (g_x.swapSlot) {  // d3d11 switches the update entry to another variant mid-pass
    void** vt = *reinterpret_cast<void***>(g_c);
    const int slot = DcsQv_ContextSlot("UpdateSubresource");
    fartest::g_realUs = reinterpret_cast<fartest::UsFnT>(g_usOrig);
    d3ds::WriteTablePointer(&vt[slot], reinterpret_cast<void*>(&fartest::VariantUs));
  }
  D3D11_VIEWPORT vp{0, 0, 64, 64, 0, 1};
  g_c->RSSetViewports(1, &vp);  // the OM / viewport hooks re-arm the upload entries
  // DCS's struct sits at the binding offset: the upload covers the whole buffer.
  D3D11_BUFFER_DESC d{};
  g_x.cb->GetDesc(&d);
  std::vector<uint8_t> full(d.ByteWidth, 0x5a);
  memcpy(full.data() + g_x.first * 16, g_x.data, ck::kCbBytes);
  if (!(g_noUploadWorld && g_oK >= 1)) g_c->UpdateSubresource(g_x.cb, 0, nullptr, full.data(), 0, 0);
  BindB7(g_x.cb, g_x.first, g_x.num);
  if (g_x.dss) g_c->OMSetDepthStencilState(g_x.dss, g_x.ref);
  if (g_x.inPass) *g_x.inPass = Snapshot(g_x.cb);
  g_hPassKind = 0;
  RunItems(gbpass::ItemVector(pass, ctx));  // (empty vectors in every scenario but the helmet one)
}

// The struct inside a buffer snapshot.
const uint8_t* At(const std::vector<uint8_t>& v, UINT first) { return v.size() >= first * 16 + ck::kCbBytes ? v.data() + first * 16 : nullptr; }

// Root cause 4 (live 2026-10-13): d3d11 submits its command buffer inside a state call of an overlay pass
// (IASetVertexBuffers, IASetIndexBuffer, PSSetShaderResources, ...), which toggles the table like a Flush. WARP
// does not, so the test's IASetVertexBuffers is replaced under our toggle-point hook by one that calls d3d11's
// and then d3d11's own Flush (not through our hook).
void* g_vbReal = nullptr;
void* g_flushReal = nullptr;
void STDMETHODCALLTYPE VbThenSubmit(ID3D11DeviceContext* c, UINT s0, UINT n, ID3D11Buffer* const* b, const UINT* st,
                                    const UINT* of) {
  reinterpret_cast<decltype(&VbThenSubmit)>(g_vbReal)(c, s0, n, b, st, of);
  reinterpret_cast<void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*)>(g_flushReal)(c);
}

// R49: the split filter's hook emulated in the PSSetShader slot (a pointer outside d3d11.dll). The real one puts
// itself back whenever another pointer sits in its slot, taking that pointer as its original: a toggle point of ours
// over it means mutual forwarding (live R47: stack overflow). It counts the calls that found its slot not its own.
void* g_sfEmuOrig = nullptr;
int g_sfEmuSlot = -1;
int g_sfEmuCalls = 0, g_sfEmuOver = 0, g_sfEmuExtSeen = 0;
void STDMETHODCALLTYPE SfEmuPSSetShader(ID3D11DeviceContext* c, ID3D11PixelShader* s, ID3D11ClassInstance* const* ci,
                                        UINT n) {
  ++g_sfEmuCalls;
  void** vt = *reinterpret_cast<void***>(c);
  if (vt[g_sfEmuSlot] != reinterpret_cast<void*>(&SfEmuPSSetShader)) ++g_sfEmuOver;
  if (fixdepth::g_tpArmed[fixdepth::kTpPSSetSRV]) ++g_sfEmuExtSeen;  // the arm's d3d11-own points are in
  reinterpret_cast<decltype(&SfEmuPSSetShader)>(g_sfEmuOrig)(c, s, ci, n);
}

void TestPipeline() {
  ID3D11Device* dev = nullptr;
  ID3D11DeviceContext* imm = nullptr;
  const D3D_FEATURE_LEVEL want[] = {D3D_FEATURE_LEVEL_11_1};
  if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, want, 1, D3D11_SDK_VERSION, &dev, nullptr,
                               &imm))) {
    QvSkip("cockpit ipd pipeline: no WARP 11.1 device\n");
    return;
  }
  imm->QueryInterface(__uuidof(ID3D11DeviceContext1), reinterpret_cast<void**>(&g_c));
  if (!g_c || !fixdepth::CompileShaders()) {
    Rel(g_c), Rel(imm), Rel(dev);
    Check(false, "cockpit ipd pipeline: device context 1 and fixation depth shaders");
    return;
  }
  // cPerView buffers: A and B (0x970, plain b7), C (struct at constant 16, *1 offsets).
  auto mkCb = [&](UINT bytes) {
    D3D11_BUFFER_DESC d{};
    d.ByteWidth = bytes;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    ID3D11Buffer* b = nullptr;
    dev->CreateBuffer(&d, nullptr, &b);
    return b;
  };
  ID3D11Buffer* cbA = mkCb(ck::kCbBytes);
  ID3D11Buffer* cbB = mkCb(ck::kCbBytes);
  ID3D11Buffer* cbC = mkCb(176 * 16);
  ID3D11Buffer* fxCb = mkCb(256);  // an overlay effect's own constant buffer (font.fx / Lines.fx: a CPU-built MVP)
  // Per view depth (D24S8, single-sample) with a stencil 0x28 cockpit region.
  ID3D11Texture2D* dtex[5] = {};
  ID3D11DepthStencilView* dsv[5] = {};
  ID3D11ShaderResourceView* dsrv[5] = {};
  for (int i = 0; i < 5; ++i) {
    D3D11_TEXTURE2D_DESC td{};
    td.Width = td.Height = 64;
    td.MipLevels = td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R24G8_TYPELESS;
    td.SampleDesc.Count = 1;
    td.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
    dev->CreateTexture2D(&td, nullptr, &dtex[i]);
    std::vector<uint32_t> px(64 * 64);
    for (int y = 0; y < 64; ++y)
      for (int x = 0; x < 64; ++x) px[y * 64 + x] = (x < 32 ? 0x28u : 0x10u) << 24 | 0x400000u;
    D3D11_TEXTURE2D_DESC sd = td;
    sd.BindFlags = 0;
    D3D11_SUBRESOURCE_DATA init{px.data(), 64 * 4, 0};
    ID3D11Texture2D* src = nullptr;
    dev->CreateTexture2D(&sd, &init, &src);
    if (src && dtex[i]) imm->CopyResource(dtex[i], src);
    Rel(src);
    D3D11_DEPTH_STENCIL_VIEW_DESC dd{};
    dd.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    dd.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
    if (dtex[i]) dev->CreateDepthStencilView(dtex[i], &dd, &dsv[i]);
    D3D11_SHADER_RESOURCE_VIEW_DESC rd{};
    rd.Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    rd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    rd.Texture2D.MipLevels = 1;
    if (dtex[i]) dev->CreateShaderResourceView(dtex[i], &rd, &dsrv[i]);
  }
  ID3D11DepthStencilView* defDsv = nullptr;  // DCS's default frame buffer between passes
  ID3D11Texture2D* defTex = nullptr;
  {
    D3D11_TEXTURE2D_DESC td{};
    td.Width = td.Height = 64;
    td.MipLevels = td.ArraySize = 1;
    td.Format = DXGI_FORMAT_D32_FLOAT;
    td.SampleDesc.Count = 1;
    td.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    dev->CreateTexture2D(&td, nullptr, &defTex);
    if (defTex) dev->CreateDepthStencilView(defTex, nullptr, &defDsv);
  }
  ID3D11DepthStencilState* ckDss = nullptr;  // the cockpit pass writes stencil ref 40
  {
    D3D11_DEPTH_STENCIL_DESC d = CD3D11_DEPTH_STENCIL_DESC(D3D11_DEFAULT);
    d.StencilEnable = TRUE;
    d.FrontFace.StencilPassOp = D3D11_STENCIL_OP_REPLACE;
    d.BackFace = d.FrontFace;
    dev->CreateDepthStencilState(&d, &ckDss);
  }
  // Phase 2 resources: the lighting target (R32G32B32A32, additive count in .y),
  // a full-screen VS and a PS writing gCameraPos.x of b7 (reading the depth SRV).
  ID3D11Texture2D* litTex = nullptr;
  ID3D11RenderTargetView* litRtv = nullptr;
  ID3D11VertexShader* litVs = nullptr;
  ID3D11PixelShader* litPs = nullptr;
  ID3D11BlendState* addBlend = nullptr;
  ID3D11BlendState* maxBlend = nullptr;  // a blend the repair cannot invert (MAX)
  ID3D11DepthStencilState* noDepth = nullptr;
  {
    D3D11_TEXTURE2D_DESC td{};
    td.Width = td.Height = 64;
    td.MipLevels = td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    td.SampleDesc.Count = 1;
    td.BindFlags = D3D11_BIND_RENDER_TARGET;
    dev->CreateTexture2D(&td, nullptr, &litTex);
    if (litTex) dev->CreateRenderTargetView(litTex, nullptr, &litRtv);
    HMODULE dll = LoadLibraryW(L"d3dcompiler_47.dll");
    auto compile = dll ? reinterpret_cast<decltype(&D3DCompile)>(GetProcAddress(dll, "D3DCompile")) : nullptr;
    static const char kSrc[] =
        "cbuffer cPerView : register(b7) { float4 pad[98]; float4 camPos; };\n"
        "Texture2D<float> depthTex : register(t3);\n"
        "float4 vs(uint id : SV_VertexID) : SV_Position {\n"
        "  float2 uv = float2((id << 1) & 2, id & 2);\n"
        "  return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);\n"
        "}\n"
        "float4 ps(float4 pos : SV_Position) : SV_Target {\n"
        "  return float4(camPos.x + 0 * depthTex.Load(int3(pos.xy, 0)), 1, 0, 0);\n"
        "}\n";
    ID3DBlob* b[2] = {};
    if (compile) {
      compile(kSrc, sizeof(kSrc) - 1, "lit", nullptr, nullptr, "vs", "vs_5_0", 0, 0, &b[0], nullptr);
      compile(kSrc, sizeof(kSrc) - 1, "lit", nullptr, nullptr, "ps", "ps_5_0", 0, 0, &b[1], nullptr);
    }
    if (b[0]) dev->CreateVertexShader(b[0]->GetBufferPointer(), b[0]->GetBufferSize(), nullptr, &litVs);
    if (b[1]) dev->CreatePixelShader(b[1]->GetBufferPointer(), b[1]->GetBufferSize(), nullptr, &litPs);
    for (ID3DBlob* x : b)
      if (x) x->Release();
    D3D11_BLEND_DESC bd{};
    bd.RenderTarget[0].BlendEnable = TRUE;
    bd.RenderTarget[0].SrcBlend = bd.RenderTarget[0].DestBlend = D3D11_BLEND_ONE;
    bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].SrcBlendAlpha = bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
    bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    dev->CreateBlendState(&bd, &addBlend);
    bd.RenderTarget[0].BlendOp = bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_MAX;
    dev->CreateBlendState(&bd, &maxBlend);
    D3D11_DEPTH_STENCIL_DESC nd = CD3D11_DEPTH_STENCIL_DESC(D3D11_DEFAULT);
    nd.DepthEnable = FALSE;
    nd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    dev->CreateDepthStencilState(&nd, &noDepth);
  }
  // The focus views' final LDR image at CopyToHMDPass (the frame dump reads it).
  ID3D11Texture2D* ldrTex = nullptr;
  ID3D11RenderTargetView* ldrRtv = nullptr;
  {
    D3D11_TEXTURE2D_DESC td{};
    td.Width = 80;
    td.Height = 64;
    td.MipLevels = td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.BindFlags = D3D11_BIND_RENDER_TARGET;
    dev->CreateTexture2D(&td, nullptr, &ldrTex);
    if (ldrTex) dev->CreateRenderTargetView(ldrTex, nullptr, &ldrRtv);
  }
  const bool made = cbA && cbB && cbC && fxCb && dsv[4] && dsrv[4] && defDsv && ckDss && litRtv && litVs && litPs &&
                    addBlend && maxBlend && noDepth && ldrRtv;
  Check(made, "cockpit ipd pipeline: WARP device, cPerView buffers (plain and at an offset), D24S8 depth targets, "
              "lighting shaders");
  if (!made) return;

  // The render graph: per HMD view sm 4 (tag 600), sm 0, sm 3; an MFD view's sm 0.
  Graph gr;
  int idx[4][3];
  for (int t = 0; t < 4; ++t) {
    idx[t][0] = gr.Add(gbrec::kSmCockpit, 600);
    idx[t][1] = gr.Add(gbrec::kSmGbuffer, 700 + t);
    idx[t][2] = gr.Add(gbrec::kSmDecal, 700 + t);
  }
  const int mfdIdx = gr.Add(gbrec::kSmGbuffer, 99);
  const int fwdIdx = gr.Add(gbrec::kSmGbuffer, 98);  // the helmet test's forward cockpit pass (its own vector)
  const int owfIdx = gr.Add(gbrec::kSmGbuffer, 97);  // the ownship test's world TransparentPass (its own vector)
  gr.Finish();
  GbPass gbp[4][3], mfdPass = gr.Pass(mfdIdx), extraCk = gr.Pass(idx[0][0]);
  for (int t = 0; t < 4; ++t)
    for (int k = 0; k < 3; ++k) gbp[t][k] = gr.Pass(idx[t][k]);
  fdtest::FakePass casc = fdtest::MakePass("CascadeShadowPass0"), lights = fdtest::MakePass("LightsBuffersPass"),
                   lighting = fdtest::MakePass("GBufferLightingPass"), tc = fdtest::MakePass("TransparentCockpitPass"),
                   tcd = fdtest::MakePass("TransparentCockpitDecalPass"), hmd = fdtest::MakePass("CopyToHMDPass"),
                   glass = fdtest::MakePass("CockpitGlassEffects"), ovlHdr = fdtest::MakePass("OverlayHDRPass"),
                   ovlLdr = fdtest::MakePass("OverlayLDRPass"), gui = fdtest::MakePass("GUIPass"),
                   mfdOvl = fdtest::MakePass("MFDOverlayPass");
  // A 2D GUI camera uploaded into the view's cPerView inside its GUIPass: never patched.
  alignas(16) uint8_t guiCam[ck::kCbBytes];
  {
    double R[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}, C[3] = {0, 0, 0};
    const double fv[4] = {-1, 1, 1, -1};
    BuildStruct(guiCam, ViewInvOf(R, C), ProjOf(fv, 0, 0), ProjOf(fv, 0, 0), 55);
  }

  // The module: only the cockpit ipd (no cone, grid or far depth).
  const int oldGbState = gbpass::g_state;
  const gbpass::ExecFn oldOrig = gbpass::g_orig;
  const gbpass::WrapFn oldWrap = gbpass::g_wrap.load();
  void* const oldLearnRg = shadowbatch::g_qLastLearnRg;
  gbpass::g_wrap = nullptr;  // DCS's body directly (no recorder wrapper from earlier tests)
  ID3D11Device* oldDev = shadowinst::g_device;
  const std::wstring oldGaze = g_gazeName;
  wchar_t gname[64], bname[64];
  swprintf_s(gname, L"Local\\CkTestGaze_%lu", GetCurrentProcessId());
  swprintf_s(bname, L"Local\\CkTestBoost_%lu", GetCurrentProcessId());
  g_gazeName = gname;  // never a running DCS's blocks
  ck::g_boostNameOverride = bname;
  HANDLE bm = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, 4096, bname);
  auto* bview = bm ? static_cast<psh_BoostOut*>(MapViewOfFile(bm, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(psh_BoostOut)))
                   : nullptr;
  gbpass::g_state = 1;  // "hooked" (no GraphicsCore here): the fake slot-19 body
  gbpass::g_orig = &FakeExec;
  shadowinst::g_device = dev;
  ptiming::g_topTid = GetCurrentThreadId();
  g_coneOn = false;
  g_gridOn = false;
  fdexport::g_on = false;
  ConfigureD3d11(1, false, true, 0, 0, false);
  const uint64_t installs0 = ck::g_gbInstallCalls.load(), disp0 = g_dispatches.load(), pub0 = g_published.load();
  g_fault = false;
  fdexport::g_stopping = false;
  fdexport::ResetFrameState();
  fixdepth::OpenMappings();
  fixdepth::EnsureDevice(dev);
  g_installed = true;
  g_ckWith = true;
  const bool started = ck::Start();
  Check(started && ck::g_on.load() && gbpass::g_obs.load() == &ck::Observer &&
            ck::g_gbInstallCalls.load() == installs0 && ck::g_boostView.load(),
        "cockpit ipd pipeline: started (gbpass observer in, already-hooked gbpass not reinstalled, BoostOut opened)");

  // The scene: head at H, yaw 25 deg; eyes on the head's right axis; DCS gets the boosted poses.
  double HR[3][3];
  double yaw = 25;
  Rot(yaw, 3, 0, HR);
  double H[3] = {1.25, 0.8, -0.6};
  const double base = 0.0655;
  const double fovs[4][4] = {{-1.3, 1.0, 1.1, -1.2}, {-1.0, 1.3, 1.1, -1.2}, {-0.6, 0.5, 0.55, -0.6},
                             {-0.5, 0.6, 0.55, -0.6}};
  Mat prevVP[5];
  for (Mat& m : prevVP) m = Ident();
  bool havePrev[5] = {};
  // gPrevFrameViewProj scenarios (R6): what DCS's PVP is, camera-origin rebases, head motion, a wrong eye.
  int pvpMode = 0;     // 0 DCS's previous gViewProj; 1 the previous camera unjittered; 2 with this frame's jitter
  bool rebase = false;  // gOrigin = the head position snapped to 1 m each frame (positions in origin space)
  double O[3] = {}, Olast[3] = {};  // this frame's / the previous frame's gOrigin
  double headStep[3] = {}, yawStep = 0;
  int wrongEye = -1;  // this view's PVP is the opposite eye's previous camera
  Mat prevV[4], prevPj[4], prevP0[4], prevTrueVP[4], prevTrueV[4];
  double scale = 1.4;
  bool writeBoost = true, mfdOk = true;
  bool cullFeed = true;  // main.cpp's culling hook: the frame's eye separation before its passes (NoteCullSep)
  uint32_t layerFrame = 0;
  int frameNo = 0;
  // Per frame results.
  struct ViewRes {
    std::vector<uint8_t> dcs;                   // DCS's struct this frame
    std::vector<uint8_t> in[3], after[3];       // GPU view in / after the sm 4, 0, 3 passes
    std::vector<uint8_t> tcIn, tcAfter, tcdIn, lightIn;
    std::vector<uint8_t> ovlIn[2], ovlAfter[2], guiIn;  // OverlayHDRPass / OverlayLDRPass; GUIPass (2D camera)
    // The overlay passes' effect buffer: DCS's bytes (an MFD page's MVP at byte 64, an implausible one at 128),
    // the GPU's view in / after each of the two passes; the page's model.
    std::vector<uint8_t> fxDcs, fxIn[2], fxAfter[2];
    Mat fxModel, fxModelB;
    float lit[2] = {}, litN[2] = {};  // phase 2 (lightAll): the lighting target left (stencil 0x28) / right; counts
    bool litOnce = false;
    Mat pvpTrue;  // the true eye's previous camera as DCS would give it (same mode, origin, jitter)
    bool haveTrue = false;
  };
  ViewRes res[4];
  std::vector<uint8_t> prevPatchedVP[4];
  bool secondCk = false, swap0 = false, lightOn = false, fwdSwap = false;
  bool ovlOuter = false;     // the overlay passes inside far depth's overlay arm (ArmOm(true), no upload entries)
  bool sfEmu = false;        // R49: a PSSetShader call (through the emulated split filter hook) inside each overlay pass
  bool lightAll = false;     // the lighting draw in every view (else view 0 with lightOn only)
  bool lightBlend = true;    // the lighting draw blends (additive) or writes
  bool maxBlendOn = false;   // the lighting draw blends with MAX (not invertible: no repair)
  bool lightPop = false;     // DCS pops the pass's frame buffer (default one bound) inside the pass, after the draw
  bool omLost = false;       // d3d11 also took the OM entries before that pop (the pop goes past the OM hooks)
  int drawToggle = 0;        // d3d11 switches the Draw entry in the lighting pass: 1 before the binder's upload, 2 after
  bool mfdOvlOk = true;
  int outerMode = 0;        // forward passes inside another feature's in-pass arm (see fwd below)
  bool foreignTook = false, foreignDraws = false;  // outerMode 4
  double extraScale = 0;    // > 0: the layer writes one more (newer) record DCS does not render this frame
  bool earlyRec = false;    // the layer writes the next frame's record (same scale) before this frame's latch
  bool skipWriteOnce = false;  // ... and then none at the next frame (its record is already there)
  int ovlFlush = 0;  // 3: a state call that submits (IASetVertexBuffers) + a clear inside each overlay pass
  uint64_t vbSubmits = 0;         // 1: Flush + a clear inside each overlay pass before its effect upload; 2: Flush before
                            //    the overlay pass begins (the arm lands on the after-flush table), a clear inside
  bool fxSkipLdr = false;   // OverlayLDRPass draws with the effect buffer without uploading it (the draw probe)
  bool dumpTarget = false;  // the focus views' LDR target bound at their CopyToHMDPass begin
  bool dumpBindInPass = true;  // ... and bound inside their OverlayLDRPass (false: only the mirror at CopyToHMDPass)
  float litRead[2] = {};  // phase 2: the lighting target's value left / right (view 0)
  float litCount[2] = {};
  ID3D11Buffer* bufOf[4] = {cbA, cbB, cbA, cbC};
  const UINT firstOf[4] = {0, 0, 0, 16}, numOf[4] = {0, 0, 0, 160};
  auto ob = [&](void* p, bool begin) { fixdepth::OnBoundary(p, begin, 0); };
  auto bindDefault = [&] {
    ID3D11RenderTargetView* none = nullptr;
    g_c->OMSetRenderTargets(1, &none, defDsv);
  };
  auto eyePos = [&](int t, double ipd, double out[3]) {
    const double sg = (t & 1) ? 1.0 : -1.0;  // left eye at -axis
    for (int i = 0; i < 3; ++i) out[i] = H[i] + sg * ipd / 2 * HR[0][i];
  };
  auto runGb = [&](GbPass& p, const uint8_t* data, ID3D11Buffer* cb, UINT first, UINT num, ID3D11DepthStencilView* d,
                   bool ckState, std::vector<uint8_t>* in, std::vector<uint8_t>* after, bool swap) {
    ob(&p, true);
    g_x = Exec{};
    g_x.cb = cb;
    g_x.first = first;
    g_x.num = num;
    g_x.data = data;
    g_x.dsv = d;
    g_x.dss = ckState ? ckDss : nullptr;
    g_x.ref = 40;
    g_x.inPass = in;
    g_x.swapSlot = swap;
    gbpass::Hook(&p, gr.ctxBlock);
    if (after) *after = Snapshot(cb);
    ob(&p, false);
    if (ckState) g_c->OMSetDepthStencilState(nullptr, 0);
  };
  // Helmet models (2026-10-14, AH-64D): a cockpit item, the pilot's helmet anchored to DCS's right eye (its origin
  // 25 mm left of that eye, 81 mm behind it, as measured live), a second cockpit item, a helmet model on the
  // eye midpoint (head-centred), one 0.5 m ahead (not on the head) and a node of the first helmet 17 cm away; the models' paths at material +0x20 -> +8,
  // their sbPositions elements in a fake page (gbrec::g_env.globals -> +0x80 -> page 0: +4 stride, +0x14 used,
  // +0x18 data). helmetOn fills every view's cockpit vector; helmetFwd also runs a forward cockpit pass
  // (TransparentCockpitPass, its vector at +0x68 -> +0x38) over the same items after the view's other passes.
  bool helmetOn = false, helmetFwd = false;
  bool ownOn = false;      // the ownship test's items in every HMD view's world G-buffer / decal / transparent vector
  bool ownDecoy = false;   // ... plus a decoy instance nearer the eyes than the ownship
  bool ownAbsent = false;  // ... the ownship's items missing (another view of the scene)
  HName hNames[2];
  strcpy_s(hNames[0].s, "//models/pilot_ah64_helmet.edm");
  strcpy_s(hNames[1].s, "//models/cockpit_test.edm");
  HMat hMats[2];
  for (int k = 0; k < 2; ++k) *reinterpret_cast<HName**>(hMats[k].b + 0x20) = &hNames[k];
  static HRend hRends[kHItems];
  static HItem hItems[kHItems];
  g_hRend = hRends;
  for (auto*& v : g_hVt) v = reinterpret_cast<void*>(&HDraw);
  const int hKind[kHItems] = {1, 0, 1, 0, 0, 0};  // the material: 0 helmet, 1 cockpit
  // The model instance ([item+0x18]): item 5 is a node of item 1's helmet (its glass, its origin 17 cm from the eyes,
  // beside the head: it follows its instance's eye); items 3 and 4 are other helmets.
  static uint8_t hInst[4][8];
  const int hInstOf[kHItems] = {0, 1, 0, 2, 3, 1};
  void* hList[kHItems];
  for (int k = 0; k < kHItems; ++k) {
    hRends[k].vt = g_hVt;
    hRends[k].item = hItems[k].b;
    *reinterpret_cast<HMat**>(hItems[k].b + 0x10) = &hMats[hKind[k]];
    *reinterpret_cast<uint32_t*>(hItems[k].b + 0xd0) = 0;  // page 0
    *reinterpret_cast<uint32_t*>(hItems[k].b + 0xd4) = static_cast<uint32_t>(k);  // element k
    *reinterpret_cast<uint8_t**>(hItems[k].b + 0x18) = hInst[hInstOf[k]];
    hList[k] = &hRends[k];
  }
  static float hElems[(kHItems + kOItems) * 24];
  alignas(16) static uint8_t hPage[0x30];
  alignas(16) static uint8_t hGlobals[0x100];
  static uint8_t* hPages = hPage;
  static uint8_t* hGlobalsPtr = hGlobals;
  *reinterpret_cast<uint32_t*>(hPage + 4) = 96;
  *reinterpret_cast<uint32_t*>(hPage + 0x14) = (kHItems + kOItems) * 96;
  *reinterpret_cast<float**>(hPage + 0x18) = hElems;
  *reinterpret_cast<uint8_t**>(hGlobals + 0x80) = hPages;
  uint8_t* const* const oldGlobals = gbrec::g_env.globals;
  struct FwdPass {  // a SimplePassData pass: name length +8, name +0x10, its resource node +0x68
    void* vt = nullptr;
    uint64_t len = 0;
    char name[0x58] = {};
    uint8_t* node = nullptr;
  };
  static_assert(offsetof(FwdPass, node) == 0x68, "FwdPass layout");
  std::vector<uint8_t> fwdNode(0x40, 0);
  {
    const int16_t i = static_cast<int16_t>(fwdIdx);
    memcpy(fwdNode.data() + 0x38, &i, 2);
  }
  FwdPass hFwd;
  hFwd.len = 22;
  strcpy_s(hFwd.name, "TransparentCockpitPass");
  hFwd.node = fwdNode.data();
  std::vector<uint8_t> hFwdIn[4];  // the forward pass's struct as bound before its items (the true eye)
  // The ownship: items 0 and 1 the own aircraft's exterior instance (1.2 m ahead, 0.5 m low: the CPG; 4 m: a wing),
  // item 2 another aircraft's (9 m), item 3 a decoy instance 0.8 m ahead (ownDecoy only).
  HName oNames[2];
  strcpy_s(oNames[0].s, "//models/ah-64d_bl2.edm");
  strcpy_s(oNames[1].s, "//models/f-16c_bl50.edm");
  HMat oMats[2];
  for (int k = 0; k < 2; ++k) *reinterpret_cast<HName**>(oMats[k].b + 0x20) = &oNames[k];
  static HRend oRends[kOItems];
  static HItem oItems[kOItems];
  static uint8_t oInst[3][8];
  const int oInstOf[kOItems] = {0, 0, 1, 2}, oMatOf[kOItems] = {0, 0, 1, 1};
  g_oRend = oRends;
  for (auto*& v : g_oVt) v = reinterpret_cast<void*>(&ODraw);
  for (int k = 0; k < kOItems; ++k) {
    oRends[k].vt = g_oVt;
    oRends[k].item = oItems[k].b;
    *reinterpret_cast<HMat**>(oItems[k].b + 0x10) = &oMats[oMatOf[k]];
    *reinterpret_cast<uint32_t*>(oItems[k].b + 0xd0) = 0;
    *reinterpret_cast<uint32_t*>(oItems[k].b + 0xd4) = static_cast<uint32_t>(kHItems + k);
    *reinterpret_cast<uint8_t**>(oItems[k].b + 0x18) = oInst[oInstOf[k]];
  }
  void* oList[kOItems];
  int oListN = 0;
  ck::g_ownSrVtOverride = g_oVt;
  FwdPass oFwd;
  oFwd.len = 15;
  strcpy_s(oFwd.name, "TransparentPass");
  std::vector<uint8_t> owfNode(0x40, 0);
  {
    const int16_t i = static_cast<int16_t>(owfIdx);
    memcpy(owfNode.data() + 0x38, &i, 2);
  }
  oFwd.node = owfNode.data();
  std::vector<uint8_t> oFwdIn[4];
  auto helmetSetup = [&] {
    // The page: every element a unit rotation (rows of the head's axes) and its origin, in origin space.
    double eR[3], eL[3], mid[3];
    eyePos(1, base * scale, eR);
    eyePos(0, base * scale, eL);
    for (int i = 0; i < 3; ++i) eR[i] -= O[i], eL[i] -= O[i], mid[i] = 0.5 * (eR[i] + eL[i]);
    double org[kHItems][3];
    for (int i = 0; i < 3; ++i) {
      org[0][i] = mid[i] + 0.6 * HR[2][i];
      org[1][i] = eR[i] - 0.025 * HR[0][i] - 0.081 * HR[2][i];
      org[2][i] = mid[i] + 0.45 * HR[2][i] - 0.2 * HR[1][i];
      org[3][i] = mid[i] - 0.081 * HR[2][i];
      org[4][i] = mid[i] + 0.5 * HR[2][i];
      org[5][i] = eR[i] + 0.087 * HR[0][i] - 0.098 * HR[1][i] - 0.06 * HR[2][i];
    }
    for (int k = 0; k < kHItems; ++k) {
      float* e = hElems + k * 24;
      for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c) e[r * 3 + c] = static_cast<float>(HR[r][c]);
      for (int c = 0; c < 3; ++c) e[9 + c] = static_cast<float>(org[k][c]);
    }
    for (int t = 0; t < 4; ++t) {
      void** v = reinterpret_cast<void**>(gr.vecs.data() + static_cast<size_t>(idx[t][0]) * 24);
      v[0] = helmetOn ? hList : nullptr;
      v[1] = helmetOn ? hList + kHItems : nullptr;
      v[2] = v[1];
    }
    void** fv = reinterpret_cast<void**>(gr.vecs.data() + static_cast<size_t>(fwdIdx) * 24);
    fv[0] = helmetFwd ? hList : nullptr;
    fv[1] = fv[2] = helmetFwd ? hList + kHItems : nullptr;
    {
      const double oo[kOItems][3] = {{0, -0.5, 1.2}, {2.5, -0.6, 3.2}, {4.0, 0.0, 8.0}, {0.0, -0.1, 0.8}};  // right, up, fwd
      for (int k = 0; k < kOItems; ++k) {
        float* e = hElems + (kHItems + k) * 24;
        for (int r = 0; r < 3; ++r)
          for (int c = 0; c < 3; ++c) e[r * 3 + c] = static_cast<float>(HR[r][c]);
        for (int c = 0; c < 3; ++c)
          e[9 + c] = static_cast<float>(mid[c] + oo[k][0] * HR[0][c] + oo[k][1] * HR[1][c] + oo[k][2] * HR[2][c]);
      }
      oListN = 0;
      for (int k = 0; k < kOItems; ++k)
        if ((k != 3 || ownDecoy) && (k > 1 || !ownAbsent)) oList[oListN++] = &oRends[k];
      for (int t = 0; t < 4; ++t)
        for (int k = 1; k < 3; ++k) {
          void** v = reinterpret_cast<void**>(gr.vecs.data() + static_cast<size_t>(idx[t][k]) * 24);
          v[0] = ownOn ? oList : nullptr;
          v[1] = v[2] = ownOn ? oList + oListN : nullptr;
        }
      void** ov = reinterpret_cast<void**>(gr.vecs.data() + static_cast<size_t>(owfIdx) * 24);
      ov[0] = ownOn ? oList : nullptr;
      ov[1] = ov[2] = ownOn ? oList + oListN : nullptr;
    }
    gbrec::g_env.globals = helmetOn || ownOn ? &hGlobalsPtr : oldGlobals;
  };
  auto frame = [&] {
    ++frameNo;
    for (int i = 0; i < 3; ++i) H[i] += headStep[i];
    if (yawStep != 0) {
      yaw += yawStep;
      Rot(yaw, 3, 0, HR);
    }
    if (rebase)
      for (int i = 0; i < 3; ++i) O[i] = std::floor(H[i]);
    helmetSetup();
    if (writeBoost && bview && !skipWriteOnce) {
      ++layerFrame;
      psh_BoostOut b{};
      b.displayTime = 1000 + layerFrame;
      b.frame = layerFrame;
      b.scale = static_cast<float>(scale);
      b.baseIpdM = static_cast<float>(base);
      b.renderedIpdM = static_cast<float>(base * scale);
      b.flags = psh_enabled() | (std::fabs(scale - 1) >= 1e-3 ? psh_applied() : 0);
      ::dcsvr_perception::seqlock_write(bview, b);
    }
    skipWriteOnce = false;
    if (cullFeed) ck::NoteCullSep(base * scale);  // the culling call: this frame's eyes (DCS's), before its passes
    ob(&casc, true);
    if (earlyRec && bview) {  // DCS located the next frame already: its record lands before this frame's latch
      ++layerFrame;
      psh_BoostOut b{};
      b.displayTime = 1000 + layerFrame;
      b.frame = layerFrame;
      b.scale = static_cast<float>(scale);
      b.baseIpdM = static_cast<float>(base);
      b.renderedIpdM = static_cast<float>(base * scale);
      b.flags = psh_enabled() | (std::fabs(scale - 1) >= 1e-3 ? psh_applied() : 0);
      ::dcsvr_perception::seqlock_write(bview, b);
      earlyRec = false;
      skipWriteOnce = true;
    }
    if (extraScale > 0 && bview) {  // read at the cascade's boundary: a second record in the same frame
      ++layerFrame;
      psh_BoostOut b{};
      b.displayTime = 1000 + layerFrame;
      b.frame = layerFrame;
      b.scale = static_cast<float>(extraScale);
      b.baseIpdM = static_cast<float>(base);
      b.renderedIpdM = static_cast<float>(base * extraScale);
      b.flags = psh_enabled() | (std::fabs(extraScale - 1) >= 1e-3 ? psh_applied() : 0);
      ::dcsvr_perception::seqlock_write(bview, b);
    }
    ob(&casc, false);
    // The MFD view first (hmdOrd 0, no cockpit pass): never patched.
    {
      alignas(16) uint8_t ms[ck::kCbBytes];
      double R[3][3], C[3] = {2, 1, 3};
      Rot(90, -30, 0, R);
      const double fv[4] = {-0.3, 0.3, 0.3, -0.3};
      BuildStruct(ms, ViewInvOf(R, C), ProjOf(fv, 0, 0), Mul(Inv(ViewInvOf(R, C)), ProjOf(fv, 0, 0)), 77);
      std::vector<uint8_t> in;
      bindDefault();
      runGb(mfdPass, ms, cbA, 0, 0, dsv[4], false, &in, nullptr, false);
      if (in.size() < ck::kCbBytes || memcmp(in.data(), ms, ck::kCbBytes)) mfdOk = false;
      // The MFD render's own overlay passes (MFDOverlayPass, and an OverlayHDRPass of its view setup) at hmdOrd 0,
      // before view 0's cockpit pass: never patched.
      for (fdtest::FakePass* op : {&mfdOvl, &ovlHdr}) {
        ob(op, true);
        g_c->UpdateSubresource(cbA, 0, nullptr, ms, 0, 0);
        BindB7(cbA, 0, 0);
        const std::vector<uint8_t> got = Snapshot(cbA);
        ob(op, false);
        if (got.size() < ck::kCbBytes || memcmp(got.data(), ms, ck::kCbBytes)) mfdOvlOk = false;
      }
    }
    for (int t = 0; t < 4; ++t) {
      ViewRes& r = res[t];
      r = ViewRes{};
      double C[3], Ct[3];
      eyePos(t, base * scale, C);  // DCS's (boosted) eye and the true one, in origin space
      eyePos(t, base, Ct);
      for (int i = 0; i < 3; ++i) C[i] -= O[i], Ct[i] -= O[i];
      const double jx = ((frameNo * 7 + t) % 16) / 16.0 * 0.004 - 0.002;
      const Mat P = ProjOf(fovs[t], jx, -jx), P0 = ProjOf(fovs[t], 0, 0);
      const Mat VI = ViewInvOf(HR, C);
      // DCS's gPrevFrameViewProj: the previous camera (of view u) in this frame's origin space.
      const int u = t == wrongEye ? (t ^ 1) : t;
      const double dO[3] = {O[0] - Olast[0], O[1] - Olast[1], O[2] - Olast[2]};
      Mat pvp = Mul(Inv(VI), P);
      if (havePrev[u]) {
        const Mat b = pvpMode == 1 ? Mul(prevV[u], prevP0[u]) : pvpMode == 2 ? Mul(prevV[u], P) : prevVP[u];
        pvp = NonZero3(dO) ? Mul(Trans(dO), b) : b;
        const Mat bt = pvpMode == 1 ? Mul(prevTrueV[u], prevP0[u]) : pvpMode == 2 ? Mul(prevTrueV[u], P) : prevTrueVP[u];
        r.pvpTrue = Mul(Trans(dO), bt);
        r.haveTrue = true;
      }
      r.dcs.resize(ck::kCbBytes);
      BuildStruct(r.dcs.data(), VI, P, pvp, 100 + t);
      const float of[4] = {static_cast<float>(O[0]), static_cast<float>(O[1]), static_cast<float>(O[2]), 0.0f};
      memcpy(r.dcs.data() + ck::kOffOrigin, of, sizeof(of));
      prevVP[t] = Get(r.dcs.data(), ck::kOffViewProj);
      prevV[t] = Get(r.dcs.data(), ck::kOffView);
      prevPj[t] = Get(r.dcs.data(), ck::kOffProj);
      prevP0[t] = P0;
      prevTrueV[t] = Inv(ViewInvOf(HR, Ct));
      prevTrueVP[t] = Mul(prevTrueV[t], prevPj[t]);
      havePrev[t] = true;
      ob(&lights, true);
      ob(&lights, false);
      bindDefault();
      for (int k = 0; k < 3; ++k) {
        g_oK = k;  // (the ownship recorder: 1 world, 2 decal)
        runGb(gbp[t][k], r.dcs.data(), bufOf[t], firstOf[t], numOf[t], dsv[t], k == 0, &r.in[k], &r.after[k],
              k == 0 && t == 0 && swap0);
        g_oK = 0;
        if (k == 0 && t == 0 && secondCk) runGb(extraCk, r.dcs.data(), bufOf[t], firstOf[t], numOf[t], dsv[t], true,
                                                nullptr, nullptr, false);
      }
      // GBufferLightingPass: DCS's binder uploads cPerView, one full-screen draw reading the depth. The state
      // stays bound to the pass end (DCS's binder does not unbind); it is unbound after it.
      ob(&lighting, true);
      const bool drawHere = (lightOn && t == 0) || lightAll;
      void** dvt = *reinterpret_cast<void***>(g_c);
      const int dslot = DcsQv_ContextSlot(fdexport::kDrawNames[fdexport::kDkDraw]);
      bool toggled = false;
      auto toggle = [&] {  // d3d11 switches the Draw entry to another variant (a Flush / ExecuteCommandList)
        void* cur = dvt[dslot];
        g_realDraw = reinterpret_cast<DrawT>(cur == reinterpret_cast<void*>(&DhDraw) ? g_dOrig[fdexport::kDkDraw] : cur);
        d3ds::WriteTablePointer(&dvt[dslot], reinterpret_cast<void*>(&VariantDraw));
        toggled = true;
      };
      // The frame buffer and the effect state first (the OM / viewport calls are re-arm points), then the
      // binder's cPerView upload and the draw. (No Map / readback inside the pass: WARP's Map flushes, and the
      // flush itself switches the draw entries, which the next OM call repairs.)
      if (drawHere) {
        const float zero[4] = {0, 0, 0, 0};
        g_c->ClearRenderTargetView(litRtv, zero);
        g_c->OMSetRenderTargets(1, &litRtv, nullptr);
        D3D11_VIEWPORT vp{0, 0, 64, 64, 0, 1};
        g_c->RSSetViewports(1, &vp);
        g_c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        g_c->IASetInputLayout(nullptr);
        g_c->VSSetShader(litVs, nullptr, 0);
        g_c->PSSetShader(litPs, nullptr, 0);
        g_c->PSSetShaderResources(3, 1, &dsrv[t]);
        g_c->OMSetBlendState(maxBlendOn ? maxBlend : lightBlend ? addBlend : nullptr, nullptr, 0xffffffff);
        g_c->OMSetDepthStencilState(noDepth, 0);
      }
      if (drawHere && drawToggle == 1) toggle();  // after the last OM / viewport call, before the binder's upload
      {
        D3D11_BUFFER_DESC d{};
        bufOf[t]->GetDesc(&d);
        std::vector<uint8_t> full(d.ByteWidth, 0x5a);
        memcpy(full.data() + firstOf[t] * 16, r.dcs.data(), ck::kCbBytes);
        g_c->UpdateSubresource(bufOf[t], 0, nullptr, full.data(), 0, 0);
        BindB7(bufOf[t], firstOf[t], numOf[t]);
      }
      if (drawHere && drawToggle == 2) toggle();  // after the upload: nothing of ours runs before the draw
      if (drawHere) g_c->Draw(3, 0);
      if (drawHere && omLost) {  // a full table rewrite took the OM entries too: the pop is not seen
        void** vt = *reinterpret_cast<void***>(g_c);
        if (vt[g_omSlot] == reinterpret_cast<void*>(&OmHook))
          d3ds::WriteTablePointer(&vt[g_omSlot], reinterpret_cast<void*>(g_omOrig));
        if (vt[g_omUavSlot] == reinterpret_cast<void*>(&OmUavHook))
          d3ds::WriteTablePointer(&vt[g_omUavSlot], reinterpret_cast<void*>(g_omUavOrig));
      }
      if (drawHere && lightPop) {  // live: DCS's default frame buffer and its viewport back before the pass end
        bindDefault();
        D3D11_VIEWPORT dvp{0, 0, 32, 32, 0, 1};
        g_c->RSSetViewports(1, &dvp);
      }
      ob(&lighting, false);
      if (toggled && dvt[dslot] == reinterpret_cast<void*>(&VariantDraw))  // d3d11's entry back
        d3ds::WriteTablePointer(&dvt[dslot], reinterpret_cast<void*>(g_realDraw));
      r.lightIn = Snapshot(bufOf[t]);  // DCS's lighting upload as the buffer holds it (nothing of ours wrote it)
      if (drawHere) {
        ID3D11ShaderResourceView* nullSrv = nullptr;
        g_c->PSSetShaderResources(3, 1, &nullSrv);
        g_c->OMSetBlendState(nullptr, nullptr, 0xffffffff);
        g_c->OMSetDepthStencilState(nullptr, 0);
        // Read pixels (8, 8) (cockpit stencil) and (40, 8) (world).
        D3D11_TEXTURE2D_DESC sd{};
        litTex->GetDesc(&sd);
        sd.Usage = D3D11_USAGE_STAGING;
        sd.BindFlags = 0;
        sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ID3D11Texture2D* st = nullptr;
        dev->CreateTexture2D(&sd, nullptr, &st);
        if (st) {
          g_c->CopyResource(st, litTex);
          D3D11_MAPPED_SUBRESOURCE m{};
          if (SUCCEEDED(g_c->Map(st, 0, D3D11_MAP_READ, 0, &m))) {
            const float* row = reinterpret_cast<const float*>(static_cast<const uint8_t*>(m.pData) + 8 * m.RowPitch);
            r.lit[0] = row[8 * 4];
            r.litN[0] = row[8 * 4 + 1];
            r.lit[1] = row[40 * 4];
            r.litN[1] = row[40 * 4 + 1];
            bool once = true;
            for (int y = 0; y < 64; ++y) {
              const float* rr = reinterpret_cast<const float*>(static_cast<const uint8_t*>(m.pData) + y * m.RowPitch);
              for (int x = 0; x < 64; ++x)
                if (rr[x * 4 + 1] != 1.0f) once = false;
            }
            r.litOnce = once;
            g_c->Unmap(st, 0);
          }
          st->Release();
        }
        if (t == 0) {
          litRead[0] = r.lit[0], litRead[1] = r.lit[1];
          litCount[0] = r.litOnce ? r.litN[0] : -1, litCount[1] = r.litN[1];
        }
      }
      // Forward cockpit passes: their own binder upload.
      auto fwd = [&](fdtest::FakePass& p, std::vector<uint8_t>* in, std::vector<uint8_t>* after) {
        // Another feature's in-pass arm around the forward pass (far depth's overlay-class arm: no upload
        // entries; or a full arm with them, as the verify trace's): 1 / 3 it ends after the pass, 2 inside it.
        if (outerMode == 1 || outerMode == 2) fixdepth::ArmOm(true);
        if (outerMode == 3) fixdepth::ArmOm(false);
        ob(&p, true);
        D3D11_BUFFER_DESC d{};
        bufOf[t]->GetDesc(&d);
        std::vector<uint8_t> full(d.ByteWidth, 0x5a);
        memcpy(full.data() + firstOf[t] * 16, r.dcs.data(), ck::kCbBytes);
        const bool swapHere = fwdSwap && &p != &glass;  // the forward passes (armed)
        if (swapHere) {
          // d3d11 switches the update entry right after the pass begins (a Flush / ExecuteCommandList); DCS
          // binds the pass's frame buffer and viewport before the binder's upload.
          void** vt = *reinterpret_cast<void***>(g_c);
          const int slot = DcsQv_ContextSlot("UpdateSubresource");
          fartest::g_realUs = reinterpret_cast<fartest::UsFnT>(
              vt[slot] == reinterpret_cast<void*>(&UsHook) ? reinterpret_cast<void*>(g_usOrig) : vt[slot]);
          d3ds::WriteTablePointer(&vt[slot], reinterpret_cast<void*>(&fartest::VariantUs));
          ID3D11RenderTargetView* none = nullptr;
          g_c->OMSetRenderTargets(1, &none, defDsv);
          D3D11_VIEWPORT vp{0, 0, 64, 64, 0, 1};
          g_c->RSSetViewports(1, &vp);
        }
        if (outerMode == 4 && &p != &glass) {  // another feature tries to arm / disarm inside the forward pass
          foreignTook = foreignTook || fixdepth::ArmOm(true);
          fixdepth::ArmDraws();
          foreignDraws = foreignDraws || g_drawArmed;
          fixdepth::DisarmOm();
        }
        g_c->UpdateSubresource(bufOf[t], 0, nullptr, full.data(), 0, 0);
        BindB7(bufOf[t], firstOf[t], numOf[t]);
        if (in) *in = Snapshot(bufOf[t]);
        if (outerMode == 2) fixdepth::DisarmOm();  // the outer feature's pass ends first: handover
        ob(&p, false);
        if (outerMode == 1 || outerMode == 3) fixdepth::DisarmOm();
        if (after) *after = Snapshot(bufOf[t]);
        if (swapHere) {  // d3d11's variant stays in the table after the pass: back to the real entry
          void** vt = *reinterpret_cast<void***>(g_c);
          const int slot = DcsQv_ContextSlot("UpdateSubresource");
          if (vt[slot] == reinterpret_cast<void*>(&fartest::VariantUs))
            d3ds::WriteTablePointer(&vt[slot], reinterpret_cast<void*>(fartest::g_realUs));
        }
      };
      fwd(tc, &r.tcIn, &r.tcAfter);
      fwd(tcd, &r.tcdIn, nullptr);
      fwd(glass, nullptr, nullptr);
      if (ownOn) {  // the world's TransparentPass: its binder upload, then DCS's item loop
        ob(&oFwd, true);
        D3D11_BUFFER_DESC d{};
        bufOf[t]->GetDesc(&d);
        std::vector<uint8_t> full(d.ByteWidth, 0x5a);
        memcpy(full.data() + firstOf[t] * 16, r.dcs.data(), ck::kCbBytes);
        g_c->UpdateSubresource(bufOf[t], 0, nullptr, full.data(), 0, 0);
        BindB7(bufOf[t], firstOf[t], numOf[t]);
        oFwdIn[t] = Snapshot(bufOf[t]);
        g_oK = 3;
        RunItems(reinterpret_cast<void**>(gr.vecs.data() + static_cast<size_t>(owfIdx) * 24));
        g_oK = 0;
        ob(&oFwd, false);
      }
      if (helmetFwd) {  // the helmet test's forward cockpit pass: its binder upload, then DCS's item loop
        ob(&hFwd, true);
        D3D11_BUFFER_DESC d{};
        bufOf[t]->GetDesc(&d);
        std::vector<uint8_t> full(d.ByteWidth, 0x5a);
        memcpy(full.data() + firstOf[t] * 16, r.dcs.data(), ck::kCbBytes);
        g_c->UpdateSubresource(bufOf[t], 0, nullptr, full.data(), 0, 0);
        BindB7(bufOf[t], firstOf[t], numOf[t]);
        hFwdIn[t] = Snapshot(bufOf[t]);
        g_hPassKind = 1;
        RunItems(reinterpret_cast<void**>(gr.vecs.data() + static_cast<size_t>(fwdIdx) * 24));
        g_hPassKind = 0;
        ob(&hFwd, false);
      }
      // The overlay passes after DLSS (HUD / MFD symbology): their binder uploads cPerView; GUIPass uploads a 2D
      // camera. ovlOuter: far depth's overlay arm (no upload entries) is held around the pass.
      // DCS's effect constants for the symbology (font.fx / Lines.fx): an MFD page 0.6 m ahead of the eye,
      // 2 mm per glyph unit, its MVP built on the CPU with DCS's boosted gViewProj; a second matrix whose model
      // is implausible (5 km per unit); other constants.
      {
        r.fxDcs.assign(256, 0);
        for (UINT i = 0; i < 256; i += 4) {
          const float v = 0.37f + 0.11f * static_cast<float>(i / 4);
          memcpy(r.fxDcs.data() + i, &v, 4);
        }
        double Cp[3];
        for (int i = 0; i < 3; ++i) Cp[i] = C[i] + 0.6 * HR[2][i] - 0.15 * HR[1][i];
        Mat Mo = ViewInvOf(HR, Cp), Mb = ViewInvOf(HR, Cp);
        for (int a = 0; a < 3; ++a)
          for (int b = 0; b < 3; ++b) Mo.a[a][b] *= 0.002, Mb.a[a][b] *= 5e3;
        r.fxModel = Mo;
        r.fxModelB = Mb;
        const Mat VPd = Get(r.dcs.data(), ck::kOffViewProj);
        Put(r.fxDcs.data(), 64, Mul(Mo, VPd));
        Put(r.fxDcs.data(), 128, Mul(Mb, VPd));
      }
      auto ovl = [&](fdtest::FakePass& p, const uint8_t* data, std::vector<uint8_t>* in, std::vector<uint8_t>* after,
                     int fx) {
        const float clearC[4] = {0, 0, 0, 0};
        if (ovlOuter) fixdepth::ArmOm(true);
        if (ovlFlush == 2 && fx >= 0) g_c->Flush();  // a Flush between passes (DLSS, the runtime): table toggled
        ob(&p, true);
        if (sfEmu && fx >= 0) g_c->PSSetShader(nullptr, nullptr, 0);
        // The pass's first toggled-entry call (a clear) before any upload: on an arm taken over the after-flush
        // table it would write d3d11's normal entries over our hooks.
        if (ovlFlush == 2 && fx >= 0) g_c->ClearRenderTargetView(litRtv, clearC);
        D3D11_BUFFER_DESC d{};
        bufOf[t]->GetDesc(&d);
        std::vector<uint8_t> full(d.ByteWidth, 0x5a);
        memcpy(full.data() + firstOf[t] * 16, data, ck::kCbBytes);
        g_c->UpdateSubresource(bufOf[t], 0, nullptr, full.data(), 0, 0);
        BindB7(bufOf[t], firstOf[t], numOf[t]);
        // d3d11 toggles its draw / copy / update / clear entries on a Flush; the next call through any of them
        // (here a clear) writes the normal ones back: without the repair our upload hook is gone for the rest of
        // the pass and the effect upload below goes out at the boosted eye.
        if (ovlFlush == 1 && fx >= 0) g_c->Flush();
        if (ovlFlush == 1 && fx >= 0) g_c->ClearRenderTargetView(litRtv, clearC);
        if (ovlFlush == 3 && fx >= 0) {
          void** vt = *reinterpret_cast<void***>(g_c);
          const bool hooked = g_tpArmed[kTpIASetVB];
          if (hooked) {
            g_vbReal = g_tpOrig[kTpIASetVB];
            g_flushReal = g_tpArmed[kTpFlush] ? g_tpOrig[kTpFlush] : vt[g_tpSlot[kTpFlush]];
            g_tpOrig[kTpIASetVB] = reinterpret_cast<void*>(&VbThenSubmit);
            ++vbSubmits;
          }
          ID3D11Buffer* nb = nullptr;
          const UINT z = 0;
          g_c->IASetVertexBuffers(0, 1, &nb, &z, &z);
          if (hooked) {
            g_tpOrig[kTpIASetVB] = g_vbReal;
          } else {  // no toggle point there: d3d11's submit all the same (the table toggled behind our back)
            reinterpret_cast<void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*)>(
                g_tpArmed[kTpFlush] ? g_tpOrig[kTpFlush] : vt[g_tpSlot[kTpFlush]])(g_c);
            ++vbSubmits;
          }
          g_c->ClearRenderTargetView(litRtv, clearC);
        }
        if (ovlFlush == 4 && fx >= 0) {
          // R32 4.5: d3d11 submits where no hook of ours sees it (its own Flush entry), then the first call
          // through a toggled entry (a clear) writes the NORMAL entries over our hooks: the table looks normal
          // (UsToggled false). The next state call (a toggle point) must still repair the arm before the upload.
          void** vt = *reinterpret_cast<void***>(g_c);
          reinterpret_cast<void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*)>(
              g_tpArmed[kTpFlush] ? g_tpOrig[kTpFlush] : vt[g_tpSlot[kTpFlush]])(g_c);
          g_c->ClearRenderTargetView(litRtv, clearC);
          ID3D11Buffer* nb = nullptr;
          const UINT z = 0;
          g_c->IASetVertexBuffers(0, 1, &nb, &z, &z);
        }
        if (fx == 0 && fxSkipLdr) {  // the HDR pass draws with the effect buffer after uploading it
          g_c->VSSetConstantBuffers(0, 1, &fxCb);
        }
        // The symbology's own effect buffer, uploaded before its draw (before the readbacks below: WARP's Map
        // flushes, which switches d3d11's update entry; DCS reads nothing back inside a pass).
        if (dumpTarget && dumpBindInPass && fx == 1 && (t == 2 || t == 3)) {  // OverlayLDRPass: the view's LDR target
          const float col[4] = {0.1f * t, 0.5f, static_cast<float>(frameNo % 10) / 10.0f, 1.0f};
          g_c->ClearRenderTargetView(ldrRtv, col);
          g_c->OMSetRenderTargets(1, &ldrRtv, nullptr);
          D3D11_VIEWPORT vp{8, 0, 64, 64, 0, 1};
          g_c->RSSetViewports(1, &vp);
        }
        const bool upFx = fx >= 0 && !(fx == 1 && fxSkipLdr);
        if (upFx) g_c->UpdateSubresource(fxCb, 0, nullptr, r.fxDcs.data(), 0, 0);
        if (fxSkipLdr && fx >= 0) {  // DCS's draw with the effect buffer bound at VS b0
          g_c->VSSetConstantBuffers(0, 1, &fxCb);
          g_c->Draw(0, 0);
          ID3D11Buffer* none = nullptr;
          g_c->VSSetConstantBuffers(0, 1, &none);
        }
        if (in) *in = Snapshot(bufOf[t]);
        if (fx >= 0) r.fxIn[fx] = Snapshot(fxCb);
        ob(&p, false);
        if (ovlOuter) fixdepth::DisarmOm();
        if (after) *after = Snapshot(bufOf[t]);
        if (fx >= 0) r.fxAfter[fx] = Snapshot(fxCb);
      };
      ovl(ovlHdr, r.dcs.data(), &r.ovlIn[0], &r.ovlAfter[0], 0);
      ovl(ovlLdr, r.dcs.data(), &r.ovlIn[1], &r.ovlAfter[1], 1);
      ovl(gui, guiCam, &r.guiIn, nullptr, -1);
      if (dumpTarget && (t == 2 || t == 3)) {  // DCS's window mirror bound at CopyToHMDPass begin (not the view's)
        const float col[4] = {1, 0, 0, 1};
        g_c->ClearRenderTargetView(litRtv, col);
        g_c->OMSetRenderTargets(1, &litRtv, nullptr);
      }
      ob(&hmd, true);
      ob(&hmd, false);
      if (dumpTarget) bindDefault();
    }
    for (int i = 0; i < 3; ++i) Olast[i] = O[i];
  };
  // Expected shift of view t's cockpit: sigma * h along the head's right axis.
  auto shiftOf = [&](const std::vector<uint8_t>& in, const std::vector<uint8_t>& dcs, UINT first, double* along,
                     double* off) {
    const uint8_t* a = At(in, first);
    if (!a) return false;
    double c1[3], c0[3];
    CamOf(a, c1);
    CamOf(dcs.data(), c0);
    const double d[3] = {c1[0] - c0[0], c1[1] - c0[1], c1[2] - c0[2]};
    *along = d[0] * HR[0][0] + d[1] * HR[0][1] + d[2] * HR[0][2];
    double o = 0;
    for (int i = 0; i < 3; ++i) o += (d[i] - *along * HR[0][i]) * (d[i] - *along * HR[0][i]);
    *off = std::sqrt(o);
    return true;
  };
  auto same = [&](const std::vector<uint8_t>& v, const std::vector<uint8_t>& dcs, UINT first) {
    const uint8_t* a = At(v, first);
    return a && !memcmp(a, dcs.data(), ck::kCbBytes);
  };
  auto allDcs = [&]() {  // every capture this frame equals DCS's bytes
    bool ok = true;
    for (int t = 0; t < 4; ++t) {
      for (int k = 0; k < 3; ++k) ok = ok && same(res[t].in[k], res[t].dcs, firstOf[t]);
      ok = ok && same(res[t].tcIn, res[t].dcs, firstOf[t]) && same(res[t].tcdIn, res[t].dcs, firstOf[t]);
      ok = ok && same(res[t].ovlIn[0], res[t].dcs, firstOf[t]) && same(res[t].ovlIn[1], res[t].dcs, firstOf[t]);
    }
    return ok;
  };
  // The view's overlay passes carry exactly its G-buffer pass's camera (the same Dw), restored after each pass.
  auto ovlAsGb = [&](int t) {
    const uint8_t* g = At(res[t].in[0], firstOf[t]);
    if (!g) return false;
    for (int k = 0; k < 2; ++k) {
      const uint8_t* o = At(res[t].ovlIn[k], firstOf[t]);
      if (!o || memcmp(o, g, ck::kCbBytes) || !same(res[t].ovlAfter[k], res[t].dcs, firstOf[t])) return false;
    }
    return true;
  };
  // The view's effect-buffer matrices in both overlay passes: each model through the camera the view's G-buffer
  // pass was drawn with (MVP' = M * T(-Dw) * VP, round trip against that VP'; the page at byte 64 and the 5 km
  // per unit one at 128, which the 100 km / orthogonality gate refused until 2026-10-13); rows 0..2
  // bit-identical; the other constants untouched; DCS's bytes again after each pass.
  auto fxAsGb = [&](int t) {
    const uint8_t* g = At(res[t].in[0], firstOf[t]);
    if (!g || res[t].fxDcs.size() != 256) return false;
    for (int k = 0; k < 2; ++k) {
      const std::vector<uint8_t>& in = res[t].fxIn[k];
      // No restore (default since 2026-10-13: DCS skips re-uploading an unchanged effect buffer, a restored one
      // drew its next pass at the boosted eye): the patched bytes stay; with CockpitTrueIpdOverlayRestore=1, DCS's.
      if (in.size() != 256 || res[t].fxAfter[k] != (ck::g_cfgOvlRestore ? res[t].fxDcs : in)) return false;
      for (int j = 0; j < 2; ++j) {
        const UINT off = j ? 128 : 64;
        const Mat want = Mul(j ? res[t].fxModelB : res[t].fxModel, Get(g, ck::kOffViewProj)),
                  d = Get(res[t].fxDcs.data(), off), got = Get(in.data(), off);
        if (MaxDiff(got, want) > 1e-5 * std::max(1.0, MaxAbs(want)) || MaxDiff(got, d) < 1e-9) return false;
        for (int r = 0; r < 3; ++r)
          for (int c = 0; c < 4; ++c)
            if (got.a[r][c] != d.a[r][c]) return false;
      }
      if (memcmp(in.data(), res[t].fxDcs.data(), 64) || memcmp(in.data() + 192, res[t].fxDcs.data() + 192, 64))
        return false;
    }
    return true;
  };
  auto fxDcsOnly = [&](int t) {  // the effect buffer exactly as DCS uploaded it in both passes
    return res[t].fxIn[0] == res[t].fxDcs && res[t].fxIn[1] == res[t].fxDcs && res[t].fxAfter[1] == res[t].fxDcs;
  };
  auto guiSame = [&]() {  // the 2D GUI camera is never patched
    bool ok = true;
    for (int t = 0; t < 4; ++t) {
      const uint8_t* a = At(res[t].guiIn, firstOf[t]);
      ok = ok && a && !memcmp(a, guiCam, ck::kCbBytes);
    }
    return ok;
  };

  // Frame 1: no HMD count yet, buffers learned. Frame 2: patched.
  frame();
  Check(allDcs() && mfdOk && ck::g_st.learned.load() >= 2,
        "cockpit ipd pipeline: frame 1 (views not known yet): DCS's bytes everywhere, cPerView buffers learned");
  frame();
  const double hExp = base * (scale - 1) / 2;
  bool signs = true, rigid = true, worldSame = true, restored = true, fwdOk = true, lightSame = true;
  double alongs[4] = {};
  for (int t = 0; t < 4; ++t) {
    double along = 0, off = 1;
    if (!shiftOf(res[t].in[0], res[t].dcs, firstOf[t], &along, &off)) signs = false;
    alongs[t] = along;
    const double wantShift = (t & 1) ? -hExp : hExp;
    if (std::fabs(along - wantShift) > 2e-6 || off > 2e-6) signs = false;
    // The patched struct is rigid and consistent.
    const uint8_t* a = At(res[t].in[0], firstOf[t]);
    if (!a || ck::SelfCheck(a) != 0 || !OthersIdentical(a, res[t].dcs.data()) ||
        !FieldIdentical(a, res[t].dcs.data(), ck::kOffProj))
      rigid = false;
    worldSame = worldSame && same(res[t].in[1], res[t].dcs, firstOf[t]) && same(res[t].in[2], res[t].dcs, firstOf[t]);
    restored = restored && same(res[t].after[0], res[t].dcs, firstOf[t]) && same(res[t].tcAfter, res[t].dcs, firstOf[t]);
    double fa = 0, fo = 1, da = 0, dofs = 1;
    fwdOk = fwdOk && shiftOf(res[t].tcIn, res[t].dcs, firstOf[t], &fa, &fo) && std::fabs(fa - along) < 1e-7 &&
            shiftOf(res[t].tcdIn, res[t].dcs, firstOf[t], &da, &dofs) && std::fabs(da - along) < 1e-7;
    lightSame = lightSame && same(res[t].lightIn, res[t].dcs, firstOf[t]);
    prevPatchedVP[t].assign(a ? a + ck::kOffViewProj : nullptr, a ? a + ck::kOffViewProj + 64 : nullptr);
  }
  printf("     (frame 2 cockpit shifts along the eye axis: %+.3f %+.3f %+.3f %+.3f mm, expected +-%.3f)\n",
         alongs[0] * 1e3, alongs[1] * 1e3, alongs[2] * 1e3, alongs[3] * 1e3, hExp * 1e3);
  Check(signs, "cockpit ipd T5: sm 4 passes of HMD views 0-3 moved back to the true eye: +h, -h, +h, -h (one h, "
               "x1.4 -> 13.1 mm)");
  Check(rigid, "cockpit ipd T5: the patched struct passes the self-checks; gProj and the other bytes are DCS's");
  Check(worldSame && mfdOk && lightSame,
        "cockpit ipd T5: sm 0 / sm 3 world passes, GBufferLightingPass (phase 1) and the MFD view see DCS's bytes");
  Check(restored, "cockpit ipd T7: after each patched pass the buffer holds DCS's bytes again (restore upload)");
  Check(fwdOk, "cockpit ipd T5: TransparentCockpitPass and ...DecalPass get the same Dw as the view's sm 4 pass");
  Check(ck::g_st.stencilOk.load() >= 4 && ck::g_st.stencilMismatch.load() == 0,
        "cockpit ipd T5: stencil cross-check: the cockpit pass ends with stencil ref 40");
  {
    bool ovlOk = true;
    for (int t = 0; t < 4; ++t) ovlOk = ovlOk && ovlAsGb(t);
    Check(ovlOk && ck::g_st.ovlPatched.load() >= 8 && ck::g_st.ovlFail.load() == 0,
          "cockpit ipd overlays: OverlayHDRPass / OverlayLDRPass of HMD views 0-3 get exactly the view's G-buffer "
          "camera (same Dw, +h -h +h -h), DCS's bytes again after each pass");
    bool fxOk = true;
    for (int t = 0; t < 4; ++t) fxOk = fxOk && fxAsGb(t);
    if (!fxOk) {
      const uint8_t* g = At(res[0].in[0], firstOf[0]);
      const Mat want = g ? Mul(res[0].fxModel, Get(g, ck::kOffViewProj)) : Ident();
      printf("     (overlay MVP view 0: in %zu bytes, diff %.3g, mvp patched %llu none %llu refused only %llu)\n",
             res[0].fxIn[0].size(), res[0].fxIn[0].size() == 256 ? MaxDiff(Get(res[0].fxIn[0].data(), 64), want) : -1.0,
             static_cast<unsigned long long>(ck::g_st.mvpPatched.load()),
             static_cast<unsigned long long>(ck::g_st.mvpNone.load()),
             static_cast<unsigned long long>(ck::g_st.mvpImplOnly.load()));
    }
    Check(fxOk && ck::g_st.mvpPatched.load() >= 8 && ck::g_st.mvpMatrices.load() >= 16 &&
              ck::g_st.mvpRejMatrices.load() == 0 && ck::g_st.mvpMixed.load() == 0 && ck::g_st.mvpRestFull.load() == 0,
          "cockpit ipd overlay MVP: both camera matrices of the overlay passes' effect buffer (the MFD page's MVP and "
          "a 5 km per unit one) follow the view's G-buffer camera (round trip against its VP', rows 0..2 "
          "bit-identical), the other constants untouched, the patched bytes kept after each pass, nothing refused");
    Check(guiSame() && ck::g_st.ovlOtherCam.load() >= 4 && mfdOvlOk && mfdOk,
          "cockpit ipd overlays: a 2D GUI camera in GUIPass (another camera) and the MFD render's MFDOverlayPass / "
          "OverlayHDRPass (before view 0's cockpit pass) keep DCS's bytes; no failure counted");
  }

  // Frame 3: the gPrev closed form (DCS's previous gViewProj -> our previous patched one) and a mid-pass swap.
  swap0 = true;
  const int variant0 = fartest::g_variantCalls.load();
  frame();
  swap0 = false;
  bool prevOk = true;
  for (int t = 0; t < 4; ++t) {
    const uint8_t* a = At(res[t].in[0], firstOf[t]);
    if (!a || prevPatchedVP[t].size() != 64) {
      prevOk = false;
      continue;
    }
    alignas(16) uint8_t tmp[ck::kCbBytes] = {};
    memcpy(tmp + ck::kOffViewProj, prevPatchedVP[t].data(), 64);
    const Mat wantVP = Get(tmp, ck::kOffViewProj), got = Get(a, ck::kOffPrevVP);
    if (MaxDiff(wantVP, got) > 2e-5 * std::max(1.0, MaxAbs(wantVP))) prevOk = false;
  }
  double a0 = 0, o0 = 1;
  Check(prevOk, "cockpit ipd T5: frame 3 gPrevFrameViewProj = the patched gViewProj of frame 2 (closed form, Dp)");
  Check(fartest::g_variantCalls.load() > variant0 && shiftOf(res[0].in[0], res[0].dcs, 0, &a0, &o0) &&
            std::fabs(a0 - hExp) < 2e-6,
        "cockpit ipd T7: d3d11 switched the update entry mid-pass; UsRearm put the hook back, the upload was patched");
  {  // d3d11's variant stays in the table after the pass: back to the real entry.
    void** vt = *reinterpret_cast<void***>(g_c);
    const int slot = DcsQv_ContextSlot("UpdateSubresource");
    if (vt[slot] == reinterpret_cast<void*>(&fartest::VariantUs))
      d3ds::WriteTablePointer(&vt[slot], reinterpret_cast<void*>(fartest::g_realUs));
  }

  // Frame 4: scale -> 1 (Depth boost back to the true distance): the tail frame.
  scale = 1.0;
  frame();
  bool tailOk = true;
  for (int t = 0; t < 4; ++t) {
    const uint8_t* a = At(res[t].in[0], firstOf[t]);
    const uint8_t* d = res[t].dcs.data();
    tailOk = tailOk && a && FieldIdentical(a, d, ck::kOffView) && FieldIdentical(a, d, ck::kOffViewInv) &&
             FieldIdentical(a, d, ck::kOffViewProj) && FieldIdentical(a, d, ck::kOffCamPos, 12) &&
             !FieldIdentical(a, d, ck::kOffPrevVP) && !FieldIdentical(a, d, ck::kOffPrevXf) && OthersIdentical(a, d);
  }
  Check(tailOk && ck::g_st.tail.load() >= 4,
        "cockpit ipd T5: scale back to 1: one tail frame patches only gPrevFrameViewProj / gPrevFrameTransform");
  {
    bool ovlTail = true;
    for (int t = 0; t < 4; ++t)
      ovlTail = ovlTail && same(res[t].ovlIn[0], res[t].dcs, firstOf[t]) && same(res[t].ovlIn[1], res[t].dcs, firstOf[t]) &&
                fxDcsOnly(t);
    Check(ovlTail, "cockpit ipd overlays: tail frame (Dw 0): the overlay passes keep DCS's bytes (no eye shift; "
                   "their symbology has no motion vectors)");
  }
  frame();
  Check(allDcs(), "cockpit ipd T5: scale 1 (no tail left): no patch, DCS's bytes everywhere");

  // Observe mode: everything runs, nothing is modified.
  scale = 1.4;
  ConfigureD3d11(2, false, true, 0, 0, false);
  const uint64_t obs0 = ck::g_st.observed.load();
  frame();
  frame();
  {
    bool fxObs = true;
    for (int t = 0; t < 4; ++t) fxObs = fxObs && fxDcsOnly(t);
    Check(allDcs() && fxObs && ck::g_st.observed.load() >= obs0 + 8 && ck::g_st.mvpObserved.load() >= 8,
          "cockpit ipd T5: CockpitTrueIpd=2 (observe): would-patch counted (cPerView and overlay MVPs), DCS's bytes "
          "everywhere");
  }
  ConfigureD3d11(1, false, true, 0, 0, false);
  frame();  // first real frame after observe: Dp 0 (nothing was really applied)
  double a1 = 0, o1 = 1;
  Check(shiftOf(res[0].in[0], res[0].dcs, 0, &a1, &o1) && std::fabs(a1 - hExp) < 2e-6 &&
            FieldIdentical(At(res[0].in[0], 0), res[0].dcs.data(), ck::kOffPrevVP),
        "cockpit ipd T5: on again after observe: patched, gPrevFrameViewProj untouched (no previous patch: Dp 0)");

  // Foreign table hooks (GPU pass timing): uploads not armed, the frame is DCS's.
  gpt::g_opsHooked = true;
  frame();
  gpt::g_opsHooked = false;
  Check(allDcs(), "cockpit ipd T5: another module's table hooks: the upload hook stays out, DCS's bytes");
  frame();  // tail of nothing: Dp was cleared by the unpatched frame
  // h > 50 mm (R77: 40 -> 50 mm, above the layer's maximum of 45 mm).
  scale = 2.8;
  ck::g_tooBigAt = -1;
  const uint64_t big0 = ck::g_st.tooBig.load();
  frame();
  bool bigTailOnly = true;  // the frame before was patched: only its tail may run
  for (int t = 0; t < 4; ++t) {
    const uint8_t* a = At(res[t].in[0], firstOf[t]);
    bigTailOnly = bigTailOnly && a && FieldIdentical(a, res[t].dcs.data(), ck::kOffViewInv);
  }
  Check(bigTailOnly && ck::g_st.tooBig.load() > big0 && ck::g_tooBigAt >= 0,
        "cockpit ipd T5: per-eye shift > 50 mm ignored (the camera stays DCS's)");
  Check(std::fabs(ck::g_lastBoostH - base * (scale - 1) / 2) < 1e-5 && ck::g_lastH == 0,
        "R77: cockpit ipd T5: a frame left unmoved by a gate keeps the record's boost (boostH) apart from the applied h 0");
  ck::g_tooBigAt = -1;
  scale = 1.4;
  frame();
  frame();
  // A second cockpit pass in view 0: the rest of the frame keeps DCS's camera; the next frame is forced off.
  // A view whose previous frame drew its cockpit at T(-Dw) still gets the gPrev tail in such a frame (its motion
  // vectors match DLSS's history): gViewProj, gView and gCameraPos DCS's, gPrevFrameViewProj moved.
  auto tailOnly = [&](const std::vector<uint8_t>& v, int t) {
    const uint8_t* a = At(v, firstOf[t]);
    const uint8_t* d = res[t].dcs.data();
    return a && FieldIdentical(a, d, ck::kOffView) && FieldIdentical(a, d, ck::kOffViewInv) &&
           FieldIdentical(a, d, ck::kOffViewProj) && FieldIdentical(a, d, ck::kOffViewProjInv) &&
           FieldIdentical(a, d, ck::kOffCamPos, 12) && !FieldIdentical(a, d, ck::kOffPrevVP) && OthersIdentical(a, d);
  };
  secondCk = true;
  const uint64_t multi0 = ck::g_st.offMulti.load(), next0 = ck::g_st.offNext.load(), tail0 = ck::g_st.offTail.load();
  frame();
  secondCk = false;
  bool restTail = true;
  for (int t = 1; t < 4; ++t) restTail = restTail && tailOnly(res[t].in[0], t) && tailOnly(res[t].tcIn, t);
  frame();
  // The forced-off frame: view 0 (patched in full before) gets its tail, views 1-3 (tail only before: Dw 0) DCS's.
  bool nextOff = tailOnly(res[0].in[0], 0) && tailOnly(res[0].tcIn, 0);
  for (int t = 1; t < 4; ++t) nextOff = nextOff && same(res[t].in[0], res[t].dcs, firstOf[t]);
  frame();
  double a2 = 0, o2 = 1;
  Check(restTail && ck::g_st.offMulti.load() > multi0 && nextOff && ck::g_st.offNext.load() > next0 &&
            ck::g_st.offTail.load() >= tail0 + 4 && shiftOf(res[0].in[0], res[0].dcs, 0, &a2, &o2) &&
            std::fabs(a2 - hExp) < 2e-6 && FieldIdentical(At(res[0].in[0], 0), res[0].dcs.data(), ck::kOffPrevVP),
        "cockpit ipd T5: a second cockpit pass in view 0: views 1-3 and their forward passes at DCS's camera with "
        "only the gPrev tail (their previous frame was moved), the forced-off next frame only view 0's tail, then "
        "patched again (Dp 0)");
  // Forward passes off ([Perception] CockpitTrueIpdForward=0).
  ConfigureD3d11(1, false, false, 0, 0, false);
  frame();
  double a3 = 0, o3 = 1;
  Check(shiftOf(res[2].in[0], res[2].dcs, 0, &a3, &o3) && same(res[2].tcIn, res[2].dcs, 0),
        "cockpit ipd T5: CockpitTrueIpdForward=0: the sm 4 pass patched, TransparentCockpitPass DCS's");
  ConfigureD3d11(1, false, true, 0, 0, false);
  frame();
  frame();
  // Overlays off ([Perception] CockpitTrueIpdOverlays=0): today's behaviour.
  {
    const uint64_t ovp0 = ck::g_st.ovlPasses.load();
    void** vto = *reinterpret_cast<void***>(g_c);
    ConfigureD3d11(1, false, true, 0, 0, false, 0, false);
    frame();
    bool offOk = true;
    for (int t = 0; t < 4; ++t) {
      double a = 0, o = 1;
      offOk = offOk && shiftOf(res[t].in[0], res[t].dcs, firstOf[t], &a, &o) &&
              std::fabs(std::fabs(a) - hExp) < 2e-6 && same(res[t].ovlIn[0], res[t].dcs, firstOf[t]) &&
              same(res[t].ovlIn[1], res[t].dcs, firstOf[t]) && fxDcsOnly(t);
    }
    const bool noArm = ck::g_st.ovlPasses.load() == ovp0 && !g_armTable && !g_usArmed;
    ConfigureD3d11(1, false, true, 0, 0, false, 0, true);
    frame();
    bool onOk = true;
    for (int t = 0; t < 4; ++t) onOk = onOk && ovlAsGb(t);
    // Inside far depth's overlay arm (no upload entries): nested, the upload entries added for the pass and
    // removed at its end; the context table as before.
    const std::vector<void*> before(vto, vto + 160);
    const uint64_t nest0 = ck::g_st.ovlNested.load();
    ovlOuter = true;
    frame();
    ovlOuter = false;
    bool nestOk = ck::g_st.ovlNested.load() >= nest0 + 8 && !g_armTable && !g_usArmed && !g_fwdNested;
    for (int t = 0; t < 4; ++t) nestOk = nestOk && ovlAsGb(t);
    // No entry of ours left in the table (d3d11 itself toggles draw / copy slots on WARP's flushes, so the
    // table is not compared bit for bit here).
    bool tableSame = vto[g_usSlot] != reinterpret_cast<void*>(&UsHook) &&
                     vto[g_us1Slot] != reinterpret_cast<void*>(&Us1Hook) &&
                     vto[g_omSlot] != reinterpret_cast<void*>(&OmHook) &&
                     vto[g_omUavSlot] != reinterpret_cast<void*>(&OmUavHook) &&
                     vto[g_vpSlot] != reinterpret_cast<void*>(&VpHook);
    for (int k = 0; k < fdexport::kDkCount; ++k)
      tableSame = tableSame && (g_dSlot[k] < 0 || vto[g_dSlot[k]] != DrawHookOf(k));
    if (!(onOk && nestOk && tableSame && ck::g_st.ovlFail.load() == 0)) {
      int diff = -1;
      for (int i = 0; i < 160 && diff < 0; ++i)
        if (before[i] != vto[i]) diff = i;
      printf("     (overlays on again %d, nested %d (+%llu, arm %p us %d fwdNested %d), table same %d (first diff slot "
             "%d), failed %llu)\n",
             onOk, nestOk, static_cast<unsigned long long>(ck::g_st.ovlNested.load() - nest0),
             static_cast<void*>(g_armTable), static_cast<int>(g_usArmed), static_cast<int>(g_fwdNested), tableSame,
             diff, static_cast<unsigned long long>(ck::g_st.ovlFail.load()));
    }
    Check(offOk && noArm, "cockpit ipd overlays: CockpitTrueIpdOverlays=0: the cockpit patched, the overlay passes "
                          "not armed and DCS's bytes (today's behaviour)");
    Check(onOk && nestOk && tableSame && ck::g_st.ovlFail.load() == 0,
          "cockpit ipd overlays: on again: patched with the view's Dw; inside far depth's overlay arm nested and "
          "patched, the arm's upload entries removed at the pass end, no entry of ours left in the context table");
    // R49: the split filter owns PSSetShader (emulated): the overlay passes' state-call toggle points (own arm and
    // nested in far depth's) never go over its hook; d3d11's own points (PSSetShaderResources) still go in.
    {
      using namespace fixdepth;
      g_sfEmuSlot = g_tpSlot[kTpPSSetShader];
      if (g_sfEmuSlot >= 0) {
        g_sfEmuOrig = vto[g_sfEmuSlot];
        FdWriteSlot(&vto[g_sfEmuSlot], reinterpret_cast<void*>(&SfEmuPSSetShader));
        g_sfEmuCalls = g_sfEmuOver = g_sfEmuExtSeen = 0;
        sfEmu = true;
        frame();  // own arms
        const int ownCalls = g_sfEmuCalls, ownExt = g_sfEmuExtSeen;
        ovlOuter = true;
        frame();  // nested in far depth's arm
        ovlOuter = false;
        sfEmu = false;
        const bool slotKept = vto[g_sfEmuSlot] == reinterpret_cast<void*>(&SfEmuPSSetShader);
        FdWriteSlot(&vto[g_sfEmuSlot], g_sfEmuOrig);
        bool imgOk = true;
        for (int t = 0; t < 4; ++t) imgOk = imgOk && ovlAsGb(t);
        if (!(ownCalls >= 8 && g_sfEmuCalls >= 16 && g_sfEmuOver == 0 && ownExt >= 8 && g_sfEmuExtSeen >= 16 &&
              slotKept && imgOk))
          printf("     (split filter emu: calls own %d all %d, over ours %d, d3d11 points armed own %d all %d, slot kept "
                 "%d, overlays %d)\n",
                 ownCalls, g_sfEmuCalls, g_sfEmuOver, ownExt, g_sfEmuExtSeen, slotKept, imgOk);
        Check(ownCalls >= 8 && g_sfEmuCalls >= 16 && g_sfEmuOver == 0 && ownExt >= 8 && g_sfEmuExtSeen >= 16 &&
                  slotKept && imgOk,
              "cockpit ipd overlays R49: the d3d11 path's state-call toggle points (own arm and nested) never over a "
              "foreign hook (the split filter's slot left alone), d3d11's own points still armed, overlays patched");
      } else {
        Check(false, "cockpit ipd overlays R49: PSSetShader slot known");
      }
    }
    // [Perception] CockpitTrueIpdOverlayMvp=0: the overlay passes' cPerView as before, the effect buffers DCS's.
    const uint64_t mvp0 = ck::g_st.mvpPatched.load();
    ConfigureD3d11(1, false, true, 0, 0, false, 0, true, false);
    frame();
    bool gateOff = ck::g_st.mvpPatched.load() == mvp0;
    for (int t = 0; t < 4; ++t) gateOff = gateOff && ovlAsGb(t) && fxDcsOnly(t);
    ConfigureD3d11(1, false, true, 0, 0, false, 0, true, true);
    frame();
    bool gateOn = ck::g_st.mvpPatched.load() >= mvp0 + 8;
    for (int t = 0; t < 4; ++t) gateOn = gateOn && ovlAsGb(t) && fxAsGb(t);
    Check(gateOff && gateOn, "cockpit ipd overlay MVP: CockpitTrueIpdOverlayMvp=0: cPerView of the overlay passes "
                             "patched as before, the effect buffers' MVPs DCS's (today); =1 again: patched");
  }

  // ---- The flashing of 2026-10-13 (root cause 1): an early BoostOut record. The layer writes the next frame's
  // record before this frame's latch (DCS locates it on another thread); this latch sees two records, the next
  // frame's none. Before the fix that frame went dry (h 0): cockpit, HUD and MFD at the boosted eye for one frame,
  // about once per second. Every view and class must stay at the true eye in both frames. ----
  {
    const uint64_t dry0 = ck::g_st.offDry.load(), car0 = ck::g_st.carried.load();
    bool both = true;
    for (int rep = 0; rep < 3; ++rep) {
      earlyRec = true;
      frame();  // two records at this frame's latch
      bool a = true;
      for (int t = 0; t < 4; ++t) {
        double s1 = 0, o1 = 1;
        a = a && shiftOf(res[t].in[0], res[t].dcs, firstOf[t], &s1, &o1) &&
            std::fabs(s1 - ((t & 1) ? -hExp : hExp)) < 2e-6 && ovlAsGb(t) && fxAsGb(t);
      }
      frame();  // no new record at this frame's latch
      bool b = true;
      for (int t = 0; t < 4; ++t) {
        double s1 = 0, o1 = 1;
        b = b && shiftOf(res[t].in[0], res[t].dcs, firstOf[t], &s1, &o1) &&
            std::fabs(s1 - ((t & 1) ? -hExp : hExp)) < 2e-6 && ovlAsGb(t) && fxAsGb(t);
      }
      if (!(a && b)) printf("     (early record %d: frame with two records %d, frame without %d)\n", rep, a, b);
      both = both && a && b;
    }
    frame();
    Check(both && ck::g_st.offDry.load() == dry0 && ck::g_st.carried.load() == car0 + 3,
          "cockpit ipd flashing (root cause 1): an early BoostOut record: the frame with two records and the next one "
          "with none both keep every view's cockpit, overlay cPerView and effect MVPs at the true eye (no dry "
          "frame; the empty latch carried)");
  }
  // ---- Depth boost switched off in the layer's ini mid-flight ([DepthBoost] Enabled=0): from that display time the
  // layer renders the true eyes (scale 1 at once) and writes no BoostOut record at all (no final enabled = 0 one).
  // The latches without a record are carried (the last h kept for up to lag + kCarryMax frames): before the fix the
  // cockpit went past the true eye by h for those frames. The frame's eye separation measured by the culling call
  // (unboosted) now gives h 0 from the first such frame. Controls: without the measurement the old carry (the
  // defect, reproduced); a merely missing record while DCS still renders boosted keeps h. ----
  {
    auto cockpitAt = [&](double hWant) {  // every view's cockpit G-buffer camera moved by hWant along the eye axis
      bool ok = true;
      for (int t = 0; t < 4; ++t) {
        double a = 0, o = 1;
        ok = ok && shiftOf(res[t].in[0], res[t].dcs, firstOf[t], &a, &o) &&
             std::fabs(a - ((t & 1) ? -hWant : hWant)) < 2e-6 && o < 2e-6;
      }
      return ok;
    };
    auto steady = [&](int n) {
      writeBoost = true;
      scale = 1.4;
      for (int i = 0; i < n; ++i) frame();
    };
    const double hOn = base * (1.4 - 1) / 2;
    // The defect, reproduced without the measurement: the first unboosted frame still moved by the carried h.
    steady(6);
    cullFeed = false;
    writeBoost = false;
    scale = 1.0;
    frame();
    const bool oldCarry = cockpitAt(hOn);
    for (int i = 0; i < 4; ++i) frame();
    cullFeed = true;
    steady(8);
    // With the measurement: every frame after the stop at the true eye (h 0), none moved.
    const uint64_t ub0 = ck::g_carriedUnboosted.load();
    writeBoost = false;
    scale = 1.0;
    bool stopOk = true;
    for (int i = 0; i < 5; ++i) {
      frame();
      const bool z = cockpitAt(0.0);
      if (!z) printf("     (layer stopped, frame %d after the stop: cockpit not at DCS's true eye)\n", i + 1);
      stopOk = stopOk && z;
    }
    const bool counted = ck::g_carriedUnboosted.load() >= ub0 + 1;
    // Control: a record missing for one frame while DCS still renders boosted (the measurement says boosted):
    // carried, h kept.
    steady(8);
    const uint64_t ub1 = ck::g_carriedUnboosted.load(), car1 = ck::g_st.carried.load() + ck::g_st.carriedLate.load();
    skipWriteOnce = true;
    frame();
    const bool lateKept = cockpitAt(hOn) && ck::g_st.carried.load() + ck::g_st.carriedLate.load() == car1 + 1;
    frame();
    frame();
    const bool lateOk = lateKept && ck::g_carriedUnboosted.load() == ub1 && cockpitAt(hOn);
    // The pure rule.
    const ck::Rec r14 = MakeRec(1, 1.4);
    const bool pure = ck::CarriedUnboosted(0.0655, 1.0, 0.0, r14) && !ck::CarriedUnboosted(0.0917, 1.0, 0.0, r14) &&
                      !ck::CarriedUnboosted(0.0655, 1.0, 0.0, MakeRec(1, 1.0)) &&
                      !ck::CarriedUnboosted(0.0, 1.0, 0.0, r14) &&
                      ck::CarriedUnboosted(0.0657, 1.0, 0.0002, r14) &&           // a pupil-shift bias
                      !ck::CarriedUnboosted(0.0660, 1.0, 0.0, MakeRec(1, 1.01));  // under 1 mm of boost: kept
    Check(oldCarry, "cockpit ipd Depth boost off in the ini (replay): without the measured separation the first frame "
                    "after the stop is moved by the carried h (the defect reproduced)");
    Check(stopOk && counted && lateOk && pure,
          "cockpit ipd Depth boost off in the ini: from the first frame the layer renders the true eyes without a "
          "record, every view's cockpit stays at the true eye (h 0 by the measured separation); a record merely "
          "missing while DCS renders boosted keeps h; the pure rule");
    steady(6);
  }
  // ---- Root cause 2: d3d11's table toggle inside an overlay pass. A Flush writes d3d11's "after flush" upload /
  // draw / clear entries over our hooks, the next call through any of them writes the normal ones back: every
  // later effect upload of the pass went out at the boosted eye (MFD / HUD symbols flashing). Repaired at the
  // Flush (hooked: not a toggled entry): the table normalized by our private upload, our hooks back. ----
  {
    const uint64_t tog0 = ck::g_st.ovlToggles.load(), norm0 = ck::g_st.tblNormalized.load(),
                   lost0 = ck::g_st.ovlArmLost.load(), fail0 = ck::g_st.ovlFail.load();
    ovlFlush = 1;
    frame();
    ovlFlush = 0;
    bool inPass = true;
    for (int t = 0; t < 4; ++t) inPass = inPass && ovlAsGb(t) && fxAsGb(t);
    const uint64_t tog1 = ck::g_st.ovlToggles.load(), norm1 = ck::g_st.tblNormalized.load();
    // A Flush between passes (the arm lands on the after-flush table): normalized at the arm.
    const uint64_t arm0 = ck::g_st.tblNormArm.load();
    ovlFlush = 2;
    frame();
    ovlFlush = 0;
    bool between = true;
    for (int t = 0; t < 4; ++t) between = between && ovlAsGb(t) && fxAsGb(t);
    frame();
    printf("     (table toggles: in overlay passes %llu, normalized %llu, at an arm %llu, arm lost %llu)\n",
           static_cast<unsigned long long>(tog1 - tog0), static_cast<unsigned long long>(norm1 - norm0),
           static_cast<unsigned long long>(ck::g_st.tblNormArm.load() - arm0),
           static_cast<unsigned long long>(ck::g_st.ovlArmLost.load() - lost0));
    Check(inPass && tog1 >= tog0 + 8 && norm1 >= norm0 + 8 && ck::g_st.ovlArmLost.load() == lost0 &&
              ck::g_st.ovlFail.load() == fail0,
          "cockpit ipd flashing (root cause 2): a Flush and a clear inside each overlay pass before its effect upload: "
          "the toggle repaired at the Flush, every effect MVP patched, no arm lost");
    Check(between && ck::g_st.tblNormArm.load() >= arm0 + 8 && ck::g_st.ovlArmLost.load() == lost0,
          "cockpit ipd flashing (root cause 2): a Flush before the overlay pass: the arm normalizes the table first, "
          "the clear inside no longer takes our upload hook, every effect MVP patched");
    // Root cause 4: the submit inside IASetVertexBuffers, then a clear (which would write d3d11's normal entries
    // over our upload hook), then the effect upload.
    const uint64_t vb0 = g_tpHits[kTpIASetVB].load(), lost4 = ck::g_st.ovlArmLost.load(), sub0 = vbSubmits;
    ovlFlush = 3;
    frame();
    ovlFlush = 0;
    bool stateCall = true;
    for (int t = 0; t < 4; ++t) stateCall = stateCall && ovlAsGb(t) && fxAsGb(t);
    frame();
    printf("     (submits inside IASetVertexBuffers %llu, toggles found after it %llu)\n",
           static_cast<unsigned long long>(vbSubmits - sub0),
           static_cast<unsigned long long>(g_tpHits[kTpIASetVB].load() - vb0));
    Check(stateCall && vbSubmits >= sub0 + 8 && g_tpHits[kTpIASetVB].load() >= vb0 + 8 &&
              ck::g_st.ovlArmLost.load() == lost4,
          "cockpit ipd flashing (root cause 4): d3d11 submitting inside IASetVertexBuffers of each overlay pass "
          "(table toggled), then a clear: repaired at the state call's toggle point, every effect MVP patched");
    // R32 4.5: the submit no hook sees, then the first call through a toggled entry (normal entries over our
    // hooks, the table looks normal), then a state call: before the fix the toggle point saw "not toggled" and the
    // effect upload went past the hook; now the arm is found "normal, not ours" and repaired there.
    const uint64_t nr0 = g_tpNormalRepair[kTpIASetVB].load(), lost5 = ck::g_st.ovlArmLost.load(),
                   nrAll0 = ck::g_st.tpNormalRepair.load();
    ovlFlush = 4;
    frame();
    ovlFlush = 0;
    bool normalRepair = true;
    for (int t = 0; t < 4; ++t) normalRepair = normalRepair && ovlAsGb(t) && fxAsGb(t);
    frame();
    printf("     (normal-not-ours repairs at IASetVertexBuffers %llu)\n",
           static_cast<unsigned long long>(g_tpNormalRepair[kTpIASetVB].load() - nr0));
    Check(normalRepair && g_tpNormalRepair[kTpIASetVB].load() >= nr0 + 8 &&
              ck::g_st.tpNormalRepair.load() >= nrAll0 + 8 && ck::g_st.ovlArmLost.load() == lost5,
          "cockpit ipd toggle repair (R32 4.5): a submit no hook sees, the first call through a toggled entry "
          "(d3d11's normal entries over our hooks), then a state call: the arm repaired there, every effect MVP "
          "patched (missed before: UsToggled saw a normal table)");
    // The G-buffer arms do not take the state-call toggle points (thousands of such calls per frame).
    bool noExt = true;
    for (int k = kTpBasic; k < kTpCount; ++k) noExt = noExt && !g_tpArmed[k];
    Check(noExt, "cockpit ipd toggle points: no state-call toggle point left armed outside the overlay passes");
  }
  // ---- The overlay draw probe and the flashing's root cause 3 (live 2026-10-13): OverlayLDRPass draws with the
  // effect buffer the HDR pass uploaded, without uploading it again (DCS skips an unchanged effect upload). With
  // the pass-end restore (CockpitTrueIpdOverlayRestore=1, the behaviour before) that draw used DCS's boosted-eye
  // bytes (counted by the probe); by default the patched bytes stay and the LDR draw is at the true eye. ----
  {
    auto ldrKeepsPatch = [&]() {
      bool ok = true;
      for (int t = 0; t < 4; ++t)
        ok = ok && res[t].fxIn[1].size() == 256 && res[t].fxIn[1] == res[t].fxIn[0] && res[t].fxIn[1] != res[t].fxDcs;
      return ok;
    };
    auto ldrBoosted = [&]() {
      bool ok = true;
      for (int t = 0; t < 4; ++t) ok = ok && res[t].fxIn[1] == res[t].fxDcs;
      return ok;
    };
    uint64_t st0 = ck::g_st.ovlStaleDraws.load(), sp0 = ck::g_st.ovlStalePasses.load(),
             pd0 = ck::g_st.ovlProbeDraws.load();
    fxSkipLdr = true;
    frame();
    const bool keep = ldrKeepsPatch();
    fxSkipLdr = false;
    frame();
    Check(keep && ck::g_st.ovlStaleDraws.load() == st0 && ck::g_st.ovlStalePasses.load() == sp0 &&
              ck::g_st.ovlProbeDraws.load() >= pd0 + 8,
          "cockpit ipd flashing (root cause 3): OverlayLDRPass draws with the effect buffer the HDR pass patched, "
          "not uploaded again: no restore at the pass end, the draw sees the true eye's MVP (none stale)");
    ck::g_cfgOvlRestore = true;
    st0 = ck::g_st.ovlStaleDraws.load(), sp0 = ck::g_st.ovlStalePasses.load(), pd0 = ck::g_st.ovlProbeDraws.load();
    fxSkipLdr = true;
    frame();
    const bool boosted = ldrBoosted();
    fxSkipLdr = false;
    frame();
    ck::g_cfgOvlRestore = false;
    Check(boosted && ck::g_st.ovlStaleDraws.load() == st0 + 4 && ck::g_st.ovlStalePasses.load() == sp0 + 4 &&
              ck::g_st.ovlProbeDraws.load() >= pd0 + 8,
          "cockpit ipd overlay probe: CockpitTrueIpdOverlayRestore=1: a draw with an effect buffer restored at the "
          "previous overlay pass's end and not uploaded again draws DCS's boosted bytes and is counted (one per "
          "view); a draw after its upload is not");
  }
  // ---- [Perception] CockpitTrueIpdFrameDump: N frames of HMD views 2 and 3 at their CopyToHMDPass begin. ----
  // Each view's image is the render target its OverlayLDRPass bound (the OM hook), never the target bound at the
  // CopyToHMDPass begin (DCS's default frame buffer, the mirror: here litRtv cleared red). Live 2026-10-14: with
  // the overlays inert no overlay arm recorded the target and every view was dumped as the mirror.
  {
    struct DumpRun {
      int bmps = 0, rows = 0;
      bool sizeOk = true, pixOk = true, srcOk = true, distinct = true, notBlack = true;
      uint64_t written = 0, noTarget = 0;
    };
    auto runDump = [&](bool overlays, bool bindInPass) {
      DumpRun out;
      wchar_t tmp[MAX_PATH];
      GetTempPathW(MAX_PATH, tmp);
      wchar_t dir[MAX_PATH];
      swprintf_s(dir, L"%lsckdump_%lu\\", tmp, GetCurrentProcessId());
      CreateDirectoryW(dir, nullptr);
      const uint64_t w0 = ck::g_dumpWritten.load(), nt0 = ck::g_dumpNoTarget.load();
      dumpTarget = true;
      dumpBindInPass = bindInPass;
      ConfigureD3d11(1, false, true, 0, 0, false, 0, overlays, true, 3, 0, dir);
      for (int i = 0; i < 6; ++i) frame();
      // R60: the ring's slots are read back (DO_NOT_WAIT) at later frame ends; on a loaded CPU WARP had not
      // finished the last captures within the 6 frames and they were never written. More frames, each after the
      // GPU finished the previous one, until the dump has drained its ring (a deadline, not a frame count).
      for (const ULONGLONG until = GetTickCount64() + 30000; ck::g_dumpActive && GetTickCount64() < until;) {
        gpttest::WaitGpuIdle(dev, imm);
        frame();
      }
      dumpTarget = false;
      dumpBindInPass = true;
      for (int i = 0; i < 200 && bindInPass && ck::g_dumpWritten.load() < w0 + 6; ++i) Sleep(10);
      Sleep(50);
      ConfigureD3d11(1, false, true, 0, 0, false, 0, true, true, 0, 0);
      out.written = ck::g_dumpWritten.load() - w0;
      out.noTarget = ck::g_dumpNoTarget.load() - nt0;
      std::wstring sub;
      WIN32_FIND_DATAW fd{};
      HANDLE h = FindFirstFileW((std::wstring(dir) + L"cockpit_frames\\*").c_str(), &fd);
      if (h != INVALID_HANDLE_VALUE) {
        do {
          if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && fd.cFileName[0] != L'.')
            sub = std::wstring(dir) + L"cockpit_frames\\" + fd.cFileName + L"\\";
        } while (FindNextFileW(h, &fd));
        FindClose(h);
      }
      if (!sub.empty()) {
        h = FindFirstFileW((sub + L"*.bmp").c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE) {
          do {
            ++out.bmps;
            std::string bytes;
            if (ReadAll((sub + fd.cFileName).c_str(), bytes) && bytes.size() >= 54) {
              int32_t bw = 0, bh = 0;
              memcpy(&bw, bytes.data() + 18, 4);
              memcpy(&bh, bytes.data() + 22, 4);
              if (bw != 64 || bh != 64 || bytes.size() != 54 + 64 * 64 * 3) out.sizeOk = false;
              // Green 0.5 everywhere (the LDR clear colour's G; the mirror is pure red).
              const uint8_t g = static_cast<uint8_t>(bytes[54 + 1]);
              if (g < 126 || g > 129) out.pixOk = false;
            } else {
              out.sizeOk = false;
            }
            DeleteFileW((sub + fd.cFileName).c_str());
          } while (FindNextFileW(h, &fd));
          FindClose(h);
        }
        std::string csv;
        if (ReadAll((sub + L"frames.csv").c_str(), csv)) {
          // Columns by the header's names: src ldr_bind, black 0, the hashes of views 2 and 3 of a frame different.
          auto split = [](const std::string& l) {
            std::vector<std::string> f;
            size_t a = 0;
            for (size_t b; (b = l.find(',', a)) != std::string::npos; a = b + 1) f.push_back(l.substr(a, b - a));
            f.push_back(l.substr(a));
            return f;
          };
          std::vector<std::string> lines;
          for (size_t a = 0, b; (b = csv.find('\n', a)) != std::string::npos; a = b + 1)
            lines.push_back(csv.substr(a, b - a));
          out.rows = static_cast<int>(lines.size());
          if (!lines.empty()) {
            const std::vector<std::string> hd = split(lines[0]);
            auto col = [&](const char* n) {
              for (size_t i = 0; i < hd.size(); ++i)
                if (hd[i] == n) return static_cast<int>(i);
              return -1;
            };
            const int cF = col("frame"), cS = col("src"), cH = col("hash"), cB = col("black");
            std::vector<std::pair<std::string, std::string>> hashOf;  // (frame, hash) of the rows so far
            if (cF < 0 || cS < 0 || cH < 0 || cB < 0) out.srcOk = false;
            for (size_t i = 1; i < lines.size() && out.srcOk; ++i) {
              const std::vector<std::string> f = split(lines[i]);
              if (f.size() != hd.size()) {
                out.srcOk = false;
                break;
              }
              if (f[cS] != "ldr_bind") out.srcOk = false;
              if (f[cB] != "0") out.notBlack = false;
              for (const auto& fh : hashOf)
                if (fh.first == f[cF] && fh.second == f[cH]) out.distinct = false;
              hashOf.emplace_back(f[cF], f[cH]);
            }
          }
        }
        DeleteFileW((sub + L"frames.csv").c_str());
        RemoveDirectoryW(sub.c_str());
      }
      RemoveDirectoryW((std::wstring(dir) + L"cockpit_frames").c_str());
      RemoveDirectoryW(dir);
      printf("     (frame dump: %d images, %d CSV lines, written %llu, no target %llu)\n", out.bmps, out.rows,
             static_cast<unsigned long long>(out.written), static_cast<unsigned long long>(out.noTarget));
      return out;
    };
    const DumpRun on = runDump(true, true);
    Check(on.bmps == 6 && on.rows == 7 && on.sizeOk && on.pixOk && on.written == 6 && on.srcOk && on.distinct &&
              on.notBlack && on.noTarget == 0,
          "cockpit ipd CockpitTrueIpdFrameDump=3: 3 frames of HMD views 2 and 3 written as 64x64 BMPs of the target "
          "their OverlayLDRPass bound (not the mirror bound at CopyToHMDPass), frames.csv with a header and 6 rows "
          "(src ldr_bind, not black, views 2 / 3 of a frame with different hashes)");
    const DumpRun inert = runDump(false, true);
    Check(inert.bmps == 6 && inert.rows == 7 && inert.sizeOk && inert.pixOk && inert.srcOk && inert.distinct &&
              inert.notBlack && inert.noTarget == 0,
          "cockpit ipd CockpitTrueIpdFrameDump with the overlays off (as inert, live 2026-10-14): each view's own "
          "OverlayLDRPass target all the same (the dump's own OM arm), never the mirror");
    const DumpRun none = runDump(true, false);
    Check(none.bmps == 0 && none.written == 0 && none.noTarget == 6,
          "cockpit ipd CockpitTrueIpdFrameDump: a view whose overlay pass binds no target of its own is not written "
          "(counted no target), never dumped as the target bound at its CopyToHMDPass begin");
  }
  // ---- The forward cockpit passes' arm (live 2026-10-13: 17 forward passes failed, flicker). ----
  {
    auto fwdAllPatched = [&]() {  // every view's forward passes on the same Dw as its sm 4 pass
      bool ok = true;
      for (int t = 0; t < 4; ++t) {
        double ga = 0, go = 1, fa = 0, fo = 1, da = 0, dd = 1;
        ok = ok && shiftOf(res[t].in[0], res[t].dcs, firstOf[t], &ga, &go) && std::fabs(std::fabs(ga) - hExp) < 2e-6 &&
             shiftOf(res[t].tcIn, res[t].dcs, firstOf[t], &fa, &fo) && std::fabs(fa - ga) < 1e-7 &&
             shiftOf(res[t].tcdIn, res[t].dcs, firstOf[t], &da, &dd) && std::fabs(da - ga) < 1e-7 &&
             same(res[t].tcAfter, res[t].dcs, firstOf[t]);
      }
      return ok;
    };
    void** vtc = *reinterpret_cast<void***>(g_c);
    const std::vector<void*> tableBefore(vtc, vtc + 160);
    const uint64_t ff0 = ck::g_st.fwdFail.load(), noArm0 = ck::g_st.fwdNoArm.load(), next0b = ck::g_st.offNext.load();
    // (a) d3d11 switches the update entry between the pass begin and the binder's upload: the OM / viewport
    // entries of the forward pass's own arm put the upload hook back before the upload.
    fwdSwap = true;
    const int var0 = fartest::g_variantCalls.load();
    frame();
    fwdSwap = false;
    const bool swapOk = fwdAllPatched() && fartest::g_variantCalls.load() > var0 && ck::g_st.fwdFail.load() == ff0;
    frame();
    const bool swapNext = fwdAllPatched() && ck::g_st.offNext.load() == next0b;
    Check(swapOk && swapNext, "cockpit ipd fwd arm: d3d11 switches the upload entry right after the forward pass "
                              "begins; the OM / viewport hooks re-arm it, the transparent cockpit is patched, no "
                              "failure, next frame on");
    // (b) far depth's overlay-class arm (no upload entries) held around each forward pass: nested, the upload
    // entries added for the pass and removed at its end; then the same with the far depth arm ending first.
    const uint64_t nest0 = ck::g_st.fwdNested.load(), hand0 = ck::g_st.fwdHandover.load();
    outerMode = 1;
    frame();
    const bool nestOk = fwdAllPatched() && ck::g_st.fwdNested.load() >= nest0 + 8 && !g_armTable && !g_usArmed;
    outerMode = 2;
    frame();
    const bool handOk = fwdAllPatched() && ck::g_st.fwdHandover.load() >= hand0 + 8 && !g_armTable && !g_usOnlyArm;
    outerMode = 3;  // a full arm with its own upload entries (the verify trace's): shared
    frame();
    const bool shareOk = fwdAllPatched() && !g_armTable && !g_usArmed && !g_fwdNested;
    outerMode = 4;  // far depth's HDR / overlay paths arm or disarm inside the forward pass: refused, left alone
    const uint64_t hand1 = ck::g_st.fwdHandover.load();
    frame();
    const bool foreignOk = fwdAllPatched() && !foreignTook && !foreignDraws && ck::g_st.fwdHandover.load() == hand1 &&
                           !g_armTable;
    outerMode = 0;
    frame();
    const bool tableSame = std::equal(tableBefore.begin(), tableBefore.end(), vtc);
    Check(nestOk && handOk && shareOk && foreignOk && tableSame && ck::g_st.fwdFail.load() == ff0 &&
              ck::g_st.fwdNoArm.load() == noArm0 && fwdAllPatched(),
          "cockpit ipd fwd arm: inside far depth's arm (ending after or inside the forward pass) or a full arm: the "
          "forward passes armed (nested / handed over), patched, no failure; another feature's arm / disarm inside "
          "the forward pass refused / ignored; the context table bit-identical after");
  }
  frame();
  frame();
  // ---- An ambiguous step: the layer writes a newer record (cockpit return, scale 1) that DCS does not
  // render yet (it renders x1.4). Lag 0 would pick the new one; the frame is ambiguous: view 0 at DCS's
  // camera (smallest plausible shift 0), views 1-3 on the record |C1 - C0| verifies (x1.4). ----
  {
    cullFeed = false;  // (without the culling call's measurement: the latch alone, view 0 at the smallest shift)
    const uint64_t amb0 = ck::g_st.pairAmb.load(), ver0 = ck::g_st.pairAmbVerified.load();
    extraScale = 1.0;
    frame();
    extraScale = 0;
    double sa0 = 1, so0 = 1;
    bool step = shiftOf(res[0].in[0], res[0].dcs, 0, &sa0, &so0) && std::fabs(sa0) < 2e-6 && so0 < 2e-6;
    for (int t = 1; t < 4; ++t) {
      double a = 0, o = 1;
      step = step && shiftOf(res[t].in[0], res[t].dcs, firstOf[t], &a, &o) &&
             std::fabs(a - ((t & 1) ? -hExp : hExp)) < 2e-6 && o < 2e-6;
    }
    const bool counted = ck::g_st.pairAmb.load() == amb0 + 1 && ck::g_st.pairAmbVerified.load() == ver0 + 1;
    // Each view's overlay passes follow its own G-buffer shift: view 0 (Dw 0, only its gPrev tail) DCS's, views 1-3
    // the verified record's.
    bool ovlStep = same(res[0].ovlIn[0], res[0].dcs, 0) && same(res[0].ovlIn[1], res[0].dcs, 0) && fxDcsOnly(0);
    for (int t = 1; t < 4; ++t) ovlStep = ovlStep && ovlAsGb(t) && fxAsGb(t);
    Check(ovlStep, "cockpit ipd overlays: ambiguous step: view 0's overlays at DCS's camera like its G-buffer (Dw 0), "
                   "views 1-3's on their verified G-buffer shift (one h per view)");
    frame();  // the new record is now the lag's neighbour: ambiguous once more, same split
    double sa1 = 1, so1 = 1, sa2 = 0, so2 = 1;
    const bool after = shiftOf(res[0].in[0], res[0].dcs, 0, &sa1, &so1) && std::fabs(sa1) < 2e-6 &&
                       shiftOf(res[2].in[0], res[2].dcs, 0, &sa2, &so2) && std::fabs(sa2 - hExp) < 2e-6;
    frame();
    frame();
    bool back = true;
    for (int t = 0; t < 4; ++t) {
      double a = 0, o = 1;
      back = back && shiftOf(res[t].in[0], res[t].dcs, firstOf[t], &a, &o) &&
             std::fabs(a - ((t & 1) ? -hExp : hExp)) < 2e-6;
    }
    printf("     (ambiguous step: view 0 shift %.3f mm, ambiguous frames %llu, verified %llu)\n", sa0 * 1e3,
           static_cast<unsigned long long>(ck::g_st.pairAmb.load() - amb0),
           static_cast<unsigned long long>(ck::g_st.pairAmbVerified.load() - ver0));
    Check(step && counted && after && back,
          "cockpit ipd ambiguous step: a newer scale-1 record DCS does not render yet: view 0 not shifted (cockpit "
          "at DCS's camera, no guess), views 1-3 on the |C1 - C0|-verified x1.4 record; then all views again");
    cullFeed = true;
  }
  // ---- The same ambiguous step with the frame's eye separation measured before it renders (the culling call):
  // view 0 takes the record that separation verifies too, so all four views are on it (live 2026-10-14, AH-64D,
  // Depth boost switched off: view 0's cockpit had been at the boosted eye for that frame). ----
  {
    const uint64_t amb0 = ck::g_st.pairAmb.load(), bc0 = ck::g_ambByCull.load();
    extraScale = 1.0;
    frame();
    extraScale = 0;
    bool all = true;
    for (int t = 0; t < 4; ++t) {
      double a = 0, o = 1;
      all = all && shiftOf(res[t].in[0], res[t].dcs, firstOf[t], &a, &o) &&
            std::fabs(a - ((t & 1) ? -hExp : hExp)) < 2e-6 && o < 2e-6 && ovlAsGb(t) && fxAsGb(t);
    }
    frame();  // ambiguous once more (the new record is the lag's neighbour): decided the same way
    bool all2 = true;
    for (int t = 0; t < 4; ++t) {
      double a = 0, o = 1;
      all2 = all2 && shiftOf(res[t].in[0], res[t].dcs, firstOf[t], &a, &o) &&
             std::fabs(a - ((t & 1) ? -hExp : hExp)) < 2e-6;
    }
    frame();
    frame();
    const uint64_t byCull = ck::g_ambByCull.load() - bc0;
    if (!(all && all2 && byCull >= 1)) printf("     (ambiguous step with the measured separation: %d %d, decided %llu)\n", all,
                                               all2, static_cast<unsigned long long>(byCull));
    (void)amb0;  // (the frame is decided by the measurement: no longer counted as an ambiguous lag pick)
    Check(all && all2 && byCull + (ck::g_pickMeas.load() > 0 ? 1 : 0) >= 1,
          "cockpit ipd ambiguous step with the measured eye separation: view 0 on the record the separation verifies "
          "(DCS still renders x1.4), like views 1-3: every view's cockpit, overlays and effect MVPs at the true eye "
          "in both ambiguous frames");
  }
  // ---- A ramp whose newest record is always one ahead of the one DCS renders (the layer located the next frame
  // already): the lag path takes the newest, the cockpit is one ramp step off the true eye (live 2026-10-14,
  // F/A-18: 0.1 mm every other frame, edge shimmer). The frame's measured separation picks the rendered record:
  // every view exact in every frame. ----
  {
    auto exactAt = [&](double h) {
      bool ok = true;
      for (int t = 0; t < 4; ++t) {
        double a = 0, o = 1;
        ok = ok && shiftOf(res[t].in[0], res[t].dcs, firstOf[t], &a, &o) && std::fabs(a - ((t & 1) ? -h : h)) < 2e-6;
      }
      return ok;
    };
    // Without the measurement: the defect (the first frame of the pattern takes the record one ahead).
    cullFeed = false;
    scale = 1.30;
    frame();
    frame();
    scale = 1.31;
    extraScale = 1.32;
    writeBoost = false;  // (only the record one ahead: the frame's own was written a frame early)
    ck::g_match.lag = 0;  // (the lag the timing gave live: the newest record)
    ck::g_gaze = ck::GazeRing{};  // (no FOV fingerprint: the pupil shift is off live)
    frame();
    double a0 = 0, o0 = 1;
    shiftOf(res[0].in[0], res[0].dcs, firstOf[0], &a0, &o0);
    const bool defect = std::fabs(a0 - base * (1.32 - 1) / 2) < 2e-6;
    if (!defect) printf("     (replay without the measurement: view 0 shift %.4f mm, path %d, lag %d, h %.4f mm)\n", a0 * 1e3, ck::g_lastPath, ck::g_match.lag, ck::g_lastH * 1e3);
    extraScale = 0;
    writeBoost = true;
    cullFeed = true;
    scale = 1.30;
    for (int i = 0; i < 3; ++i) frame();
    // With it: 8 frames of the ramp, each with a newer record DCS does not render.
    const uint64_t pm0 = ck::g_pickMeas.load();
    bool rampOk = true;
    for (int i = 0; i < 8; ++i) {
      scale = 1.30 + 0.01 * i;
      extraScale = scale + 0.01;
      writeBoost = i == 0;  // (then only the record one ahead each frame)
      ck::g_match.lag = 0;
      frame();
      const bool ok = exactAt(base * (scale - 1) / 2);
      if (!ok) {
        double a = 0, o = 1;
        shiftOf(res[0].in[0], res[0].dcs, firstOf[0], &a, &o);
        printf("     (ramp one record ahead, frame %d: view 0 shift %.4f mm, want %.4f)\n", i, a * 1e3,
               base * (scale - 1) / 2 * 1e3);
      }
      rampOk = rampOk && ok;
    }
    extraScale = 0;
    writeBoost = true;
    scale = 1.4;
    frame();
    frame();
    Check(defect, "cockpit ipd measured pick (replay): without the measured separation a newer record DCS does not "
                  "render yet is taken (the cockpit one ramp step off the true eye)");
    Check(rampOk && ck::g_pickMeas.load() >= pm0 + 8 && ck::CullPrecise(),
          "cockpit ipd measured pick: on a ramp with every newest record one ahead of the rendered one, the frame's "
          "measured eye separation picks the rendered record: every view at exactly the true eye in all 8 frames");
  }

  // ---- R6: gPrevFrameViewProj. The reference is DCS rendering the true eye
  // with the same kind of PVP (jitter, origin, motion): the patched PVP must
  // be that camera, whatever h did in between. ----
  auto viewPatched = [&](int t, double h) {
    double a = 0, o = 1;
    return shiftOf(res[t].in[0], res[t].dcs, firstOf[t], &a, &o) && std::fabs(a - ((t & 1) ? -h : h)) < 2e-6 && o < 2e-6;
  };
  auto prevTrue = [&](int t) {
    const uint8_t* a = At(res[t].in[0], firstOf[t]);
    if (!a || !res[t].haveTrue) return false;
    return MaxDiff(Get(a, ck::kOffPrevVP), res[t].pvpTrue) <= 2e-5 * std::max(1.0, MaxAbs(res[t].pvpTrue));
  };
  // Static points: the current clip through gPrevFrameTransform' lands where the true previous camera saw them.
  auto mvTrue = [&](int t) {
    const uint8_t* a = At(res[t].in[0], firstOf[t]);
    if (!a || !res[t].haveTrue) return false;
    const Mat VP = Get(a, ck::kOffViewProj), X = Get(a, ck::kOffPrevXf);
    double c0[3];
    CamOf(a, c0);
    for (int k = 0; k < 6; ++k) {
      double p[3];
      const double dist = 0.3 + 0.5 * k;
      for (int i = 0; i < 3; ++i) p[i] = c0[i] + dist * (HR[2][i] + (0.1 * k - 0.25) * HR[0][i] + 0.1 * HR[1][i]);
      double clip[4], m1[3], m2[3];
      for (int c = 0; c < 4; ++c) clip[c] = p[0] * VP.a[0][c] + p[1] * VP.a[1][c] + p[2] * VP.a[2][c] + VP.a[3][c];
      ClipThrough(clip, X, m1);
      Project(res[t].pvpTrue, p, m2);
      if (std::fabs(m1[0] - m2[0]) > 5e-5 || std::fabs(m1[1] - m2[1]) > 5e-5) return false;
    }
    return true;
  };
  // Head still, h constant: the motion vectors of a static cockpit point equal DCS's own.
  auto mvAsDcs = [&](int t) {
    const uint8_t* a = At(res[t].in[0], firstOf[t]);
    if (!a) return false;
    const uint8_t* d = res[t].dcs.data();
    const Mat VP = Get(a, ck::kOffViewProj), PVP = Get(a, ck::kOffPrevVP), VPd = Get(d, ck::kOffViewProj),
              PVPd = Get(d, ck::kOffPrevVP);
    double c0[3];
    CamOf(d, c0);
    for (int k = 0; k < 6; ++k) {
      double p[3];
      const double dist = 0.4 + 0.3 * k;
      for (int i = 0; i < 3; ++i) p[i] = c0[i] + dist * (HR[2][i] + (0.08 * k - 0.2) * HR[0][i] - 0.1 * HR[1][i]);
      double a1[3], a2[3], b1[3], b2[3];
      Project(VP, p, a1);
      Project(PVP, p, a2);
      Project(VPd, p, b1);
      Project(PVPd, p, b2);
      for (int i = 0; i < 2; ++i)
        if (std::fabs((a1[i] - a2[i]) - (b1[i] - b2[i])) > 2e-5) return false;
    }
    return true;
  };
  {
    const uint64_t gate0 = ck::g_st.prevGate.load(), bad0 = ck::g_st.relBad.load(), ok0 = ck::g_st.relOk.load();
    // S1: DCS's PVP is the previous camera without the DLSS jitter; h constant, head still.
    pvpMode = 1;
    scale = 1.4;
    frame();
    frame();
    frame();
    bool s1 = true;
    for (int t = 0; t < 4; ++t) s1 = s1 && viewPatched(t, hExp) && prevTrue(t) && mvTrue(t) && mvAsDcs(t);
    Check(s1 && ck::g_st.relBad.load() == bad0 && ck::g_st.prevGate.load() == gate0 && ck::g_st.relOk.load() > ok0,
          "cockpit ipd R6: PVP = the previous camera unjittered, h constant, head still: patched; gPrevFrameViewProj "
          "= the true eye's previous camera; static cockpit motion vectors = DCS's own");
    // S2: PVP with this frame's jitter; gOrigin rebased every frame (head moving 2.2 m per frame), head turning.
    pvpMode = 2;
    rebase = true;
    headStep[0] = 2.2, headStep[1] = 0.05, headStep[2] = -1.3;
    yawStep = 1.5;
    const uint64_t k1 = ck::g_st.rebase[1].load();
    frame();
    frame();
    frame();
    bool s2 = true;
    for (int t = 0; t < 4; ++t) s2 = s2 && viewPatched(t, hExp) && prevTrue(t) && mvTrue(t);
    Check(s2 && ck::g_st.relBad.load() == bad0 && ck::g_st.prevGate.load() == gate0 && ck::g_st.rebase[1].load() >= k1 + 3,
          "cockpit ipd R6: PVP with the current jitter, a camera-origin rebase every frame, head moving and turning: "
          "patched; gPrevFrameViewProj and gPrevFrameTransform = the true eye's previous camera; rebase seen in gOrigin");
    // S3: h changing every frame (the true eye does not move with it); unjittered PVP, rebases, motion.
    // A step > 2 mm in h between this frame's record and the previous one is
    // ambiguous for the lag path: view 0 takes the smaller of the two shifts,
    // views 1-3 the record |C1 - C0| verifies (here the rendered one).
    pvpMode = 1;
    cullFeed = false;  // (the latch alone: view 0 at the smaller shift, its store relations)
    bool s3 = true;
    const double scales[4] = {1.46, 1.33, 1.42, 1.38};
    double hPrev = hExp;
    bool under0 = false;  // view 0 was left at the smaller shift in the previous frame
    int ambFrames = 0;
    const uint64_t amb0 = ck::g_st.pairAmb.load(), ambV0 = ck::g_st.pairAmbVerified.load();
    for (double sc : scales) {
      scale = sc;
      frame();
      const double h = base * (sc - 1) / 2;
      const bool amb = std::fabs(h - hPrev) > 2e-3;
      ambFrames += amb;
      const double h0 = amb ? std::min(h, hPrev) : h;
      for (int t = 0; t < 4; ++t) {
        // After a frame whose view 0 was left at the smaller shift, view 0's gPrevFrameViewProj is the camera
        // really sent then (R6: Dp from the store), not the true eye's: the true-eye relations do not apply.
        const bool rel = !(t == 0 && under0);
        const bool vp = viewPatched(t, t == 0 ? h0 : h), pt = !rel || prevTrue(t), mt = !rel || mvTrue(t);
        if (!(vp && pt && mt)) {
          double a = 0, o = 0;
          shiftOf(res[t].in[0], res[t].dcs, firstOf[t], &a, &o);
          printf("     (S3 x%.2f view %d: patched %d prev %d mv %d (shift %.3f mm, want %.3f))\n", sc, t, vp, pt, mt,
                 a * 1e3, (t & 1 ? -1 : 1) * (t == 0 ? h0 : h) * 1e3);
        }
        s3 = s3 && vp && pt && mt;
      }
      under0 = std::fabs(h0 - h) > 1e-9;
      hPrev = h;
    }
    Check(s3 && ck::g_st.relBad.load() == bad0 && ck::g_st.prevGate.load() == gate0 && ambFrames == 2 &&
              ck::g_st.pairAmb.load() == amb0 + 2 && ck::g_st.pairAmbVerified.load() == ambV0 + 2,
          "cockpit ipd R6: h changing every frame (x1.46, 1.33, 1.42, 1.38): each frame's gPrevFrameViewProj = the "
          "true eye's previous camera (Dp from the store), motion vectors right; the 2 steps > 2 mm in h: view 0 "
          "at the smaller shift, views 1-3 on the verified record");
    cullFeed = true;
    // S4 the gate: view 1's PVP is the left eye's previous camera for one frame.
    scale = 1.4;
    pvpMode = 0;
    rebase = false;
    headStep[0] = headStep[1] = headStep[2] = 0;
    yawStep = 0;
    frame();
    wrongEye = 1;
    const uint64_t gate1 = ck::g_st.prevGate.load(), bad1 = ck::g_st.relBad.load(), offRel0 = ck::g_st.offPrevRel.load();
    frame();
    wrongEye = -1;
    const bool gated = viewPatched(0, hExp) && same(res[1].in[0], res[1].dcs, firstOf[1]) &&
                       same(res[1].tcIn, res[1].dcs, firstOf[1]) && ck::g_st.prevGate.load() == gate1 + 1 &&
                       ck::g_st.relBad.load() > bad1;
    bool offWhile = true;
    for (int i = 0; i < ck::kRelRecover; ++i) {
      frame();
      offWhile = offWhile && allDcs();
    }
    const bool offCounted = ck::g_st.offPrevRel.load() > offRel0;
    frame();  // agreed for kRelRecover frames: patched again, Dp 0 (nothing was sent shifted)
    const bool back = viewPatched(0, hExp) && viewPatched(1, hExp) &&
                      FieldIdentical(At(res[0].in[0], 0), res[0].dcs.data(), ck::kOffPrevVP);
    frame();
    bool back2 = true;
    for (int t = 0; t < 4; ++t) back2 = back2 && viewPatched(t, hExp) && prevTrue(t);
    Check(gated, "cockpit ipd R6 gate: view 1's gPrevFrameViewProj is the other eye's camera: view 1 (and its forward "
                 "passes) not patched, counted; view 0 patched");
    Check(offWhile && offCounted && back && back2,
          "cockpit ipd R6 gate: refuted -> every frame DCS's until 30 frames agree, then patched again (first frame "
          "Dp 0, then the closed relation)");
    // S5 the store disagrees with the recorded Dw (bookkeeping): that view is not patched.
    const uint64_t mis0 = ck::g_st.storeMis.load(), gate2 = ck::g_st.prevGate.load();
    // The frame end (at the next frame's first boundary) commits g_cur into the store: our "sent" matrix of
    // view 2 is 2 mm off the Dw that frame recorded.
    for (int c = 0; c < 4; ++c) ck::g_cur[2].sent.a[3][c] -= 0.002 * ck::g_cur[2].sent.a[0][c];
    frame();
    const bool misOk = viewPatched(0, hExp) && same(res[2].in[0], res[2].dcs, firstOf[2]) &&
                       ck::g_st.storeMis.load() > mis0 && ck::g_st.prevGate.load() == gate2 + 1;
    if (!misOk)
      printf("     (store mismatch: v0 %d v2 dcs %d storeMis %llu->%llu gate %llu->%llu)\n", viewPatched(0, hExp),
             same(res[2].in[0], res[2].dcs, firstOf[2]), static_cast<unsigned long long>(mis0),
             static_cast<unsigned long long>(ck::g_st.storeMis.load()), static_cast<unsigned long long>(gate2),
             static_cast<unsigned long long>(ck::g_st.prevGate.load()));
    for (int i = 0; i <= ck::kRelRecover; ++i) frame();
    frame();
    bool back3 = true;
    for (int t = 0; t < 4; ++t) back3 = back3 && viewPatched(t, hExp) && prevTrue(t);
    Check(misOk && back3, "cockpit ipd R6 gate: the stored gViewProj disagrees with the recorded Dw: that view not "
                          "patched, then recovered");
    // Back to the base scene (origin 0: one more rebase).
    O[0] = O[1] = O[2] = 0;
    H[0] = 1.25, H[1] = 0.8, H[2] = -0.6;
    yaw = 25;
    Rot(yaw, 3, 0, HR);
    const uint64_t bad2 = ck::g_st.relBad.load(), gate3 = ck::g_st.prevGate.load();
    frame();
    frame();
    bool home = true;
    for (int t = 0; t < 4; ++t) home = home && viewPatched(t, hExp) && prevTrue(t);
    Check(home && ck::g_st.relBad.load() == bad2 && ck::g_st.prevGate.load() == gate3,
          "cockpit ipd R6: a rebase back to origin 0 with a head jump: still the camera of its eye, patched");
  }

  // Phase 2: the stencil split in the hooked GBufferLightingPass of view 0.
  ConfigureD3d11(1, true, true, 0, 0, false);
  lightOn = true;
  const uint64_t split0 = ck::g_st.lightSplit.load();
  frame();
  lightOn = false;
  {
    double cDcs[3];
    CamOf(res[0].dcs.data(), cDcs);
    const double trueX = cDcs[0] + hExp * HR[0][0];
    printf("     (phase 2: lighting left (stencil 0x28) %.6f, right %.6f; DCS x %.6f, true x %.6f; counts %g/%g; "
           "arms %llu split %llu skip %llu no copy %llu draw arms %llu)\n",
           litRead[0], litRead[1], cDcs[0], trueX, litCount[0], litCount[1],
           static_cast<unsigned long long>(ck::g_st.lightArms.load()),
           static_cast<unsigned long long>(ck::g_st.lightSplit.load()),
           static_cast<unsigned long long>(ck::g_st.lightSkip.load()),
           static_cast<unsigned long long>(ck::g_st.lightNoCopy.load()),
           static_cast<unsigned long long>(fdexport::g_drawArms.load()));
    Check(ck::g_st.lightSplit.load() > split0 && std::fabs(litRead[0] - trueX) < 1e-5 &&
              std::fabs(litRead[1] - cDcs[0]) < 1e-5 && litCount[0] == 1.0f && litCount[1] == 1.0f,
          "cockpit ipd T9: CockpitTrueIpdLighting=1: GBufferLightingPass drawn twice, cockpit-stencil pixels with "
          "the true eye's b7, world pixels with DCS's, every pixel written once");
    Check(same(res[0].lightIn, res[0].dcs, 0), "cockpit ipd T9: DCS's own lighting b7 upload is not modified");
  }

  // ---- Lighting and G-buffer of a view always on the same camera (live 2026-10-13: shadows "flash"). ----
  {
    // The lighting of view t: cockpit pixels lit with the camera its G-buffer pass was drawn with, world pixels
    // with DCS's; every pixel written once.
    auto litAsGb = [&](int t) {
      const uint8_t* g = At(res[t].in[0], firstOf[t]);
      if (!g) return false;
      float gx = 0, dx = 0;
      memcpy(&gx, g + ck::kOffCamPos, 4);
      memcpy(&dx, res[t].dcs.data() + ck::kOffCamPos, 4);
      return std::fabs(res[t].lit[0] - gx) < 1e-5 && std::fabs(res[t].lit[1] - dx) < 1e-5 && res[t].litOnce;
    };
    auto allLitAsGb = [&]() {
      bool ok = true;
      for (int t = 0; t < 4; ++t) ok = ok && litAsGb(t);
      return ok;
    };
    auto shifted = [&](int t) {  // the view's G-buffer camera moved (Dw != 0)
      double a = 0, o = 1;
      return shiftOf(res[t].in[0], res[t].dcs, firstOf[t], &a, &o) && std::fabs(a) > 1e-4;
    };
    lightAll = true;
    lightBlend = true;
    frame();  // the first frame with the option: latched at its view 0 upload
    const uint64_t miss0 = ck::g_st.lightMiss.load(), split0b = ck::g_st.lightSplit.load();
    frame();
    frame();
    bool steady = allLitAsGb();
    for (int t = 0; t < 4; ++t) steady = steady && shifted(t);
    frame();  // (one more frame: the miss check of the previous one runs at this frame's start)
    Check(steady && ck::g_st.lightSplit.load() >= split0b + 8 && ck::g_st.lightMiss.load() == miss0,
          "cockpit ipd lighting: every view's lighting split with exactly its G-buffer camera (cockpit pixels) and "
          "DCS's (world pixels), no view-frame missed");
    // An ambiguous step: view 0's G-buffer at DCS's camera (Dw 0), views 1-3 on the verified record; each
    // view's lighting on its own G-buffer camera.
    cullFeed = false;  // (the latch alone)
    extraScale = 1.0;
    frame();
    extraScale = 0;
    bool amb = allLitAsGb() && !shifted(0);
    for (int t = 1; t < 4; ++t) amb = amb && shifted(t);
    frame();
    const bool amb2 = allLitAsGb();
    frame();
    frame();
    cullFeed = true;
    Check(amb && amb2 && ck::g_st.lightMiss.load() == miss0,
          "cockpit ipd lighting: ambiguous step: view 0 lit at DCS's camera like its G-buffer (no split), views 1-3 "
          "split with their verified G-buffer camera; no lighting / G-buffer disagreement");
    // d3d11 switches the Draw entry before the binder's upload: the upload re-arms the draw entries, the draw is
    // split.
    const uint64_t noDraw0 = ck::g_st.lightNoDraw.load(), true0 = ck::g_st.lightNoDrawTrue.load(),
                   fwd0 = ck::g_st.lightFwd.load();
    const int var0 = g_variantDraws;
    drawToggle = 1;
    frame();
    drawToggle = 0;
    const bool before = allLitAsGb() && g_variantDraws > var0 && ck::g_st.lightNoDraw.load() == noDraw0;
    if (!before)
      printf("     (switch before the upload: lit as G-buffer %d %d %d %d, variant draws %d, no draw %llu -> %llu, "
             "lit %.6f / %.6f once %d)\n",
             litAsGb(0), litAsGb(1), litAsGb(2), litAsGb(3), g_variantDraws - var0,
             static_cast<unsigned long long>(noDraw0), static_cast<unsigned long long>(ck::g_st.lightNoDraw.load()),
             res[0].lit[0], res[0].lit[1], res[0].litOnce);
    frame();
    Check(before && ck::g_st.lightMiss.load() == miss0 && ck::g_st.lightFwd.load() >= fwd0 + 8,
          "cockpit ipd lighting: the Draw entry switched before the binder's upload: the upload re-armed the draw "
          "hook, every view split (the lighting b7 forwarded at the true eye, restored after the pass)");
    // ... after the upload (the draw goes past every hook; live 2026-10-13: ~0.13 % of the view-frames, never
    // repaired: DCS unbinds the draw's SRVs before any later point). The lighting cPerView went out at the true
    // eye, so DCS's one draw lit the cockpit pixels from their G-buffer camera (world pixels from the true eye
    // too: ~13 mm, for that view and frame): no G-buffer / lighting disagreement on the cockpit, no miss.
    auto litTrue = [&](int t) {  // every pixel lit once with the view's G-buffer camera
      const uint8_t* g = At(res[t].in[0], firstOf[t]);
      if (!g) return false;
      float gx = 0;
      memcpy(&gx, g + ck::kOffCamPos, 4);
      return std::fabs(res[t].lit[0] - gx) < 1e-5 && std::fabs(res[t].lit[1] - gx) < 1e-5 && res[t].litOnce;
    };
    auto allLitTrue = [&]() {
      bool ok = true;
      for (int t = 0; t < 4; ++t) ok = ok && litTrue(t);
      return ok;
    };
    lightBlend = false;
    frame();
    drawToggle = 2;
    frame();
    drawToggle = 0;
    const bool missedTrue = allLitTrue() && ck::g_st.lightNoDraw.load() >= noDraw0 + 4 &&
                            ck::g_st.lightNoDrawTrue.load() == true0 + 4;
    bool lightSameAfter = true;  // DCS's bytes in the buffer again after the pass
    for (int t = 0; t < 4; ++t) lightSameAfter = lightSameAfter && same(res[t].lightIn, res[t].dcs, firstOf[t]);
    frame();
    printf("     (lighting: no draw %llu, with the true eye's b7 %llu, miss %llu)\n",
           static_cast<unsigned long long>(ck::g_st.lightNoDraw.load()),
           static_cast<unsigned long long>(ck::g_st.lightNoDrawTrue.load()),
           static_cast<unsigned long long>(ck::g_st.lightMiss.load()));
    Check(missedTrue && lightSameAfter && ck::g_st.lightMiss.load() == miss0,
          "cockpit ipd lighting: the draw went past the hooks (entry switched after the upload): DCS's own draw lit "
          "the cockpit pixels from their G-buffer camera (b7 at the true eye), every pixel once, DCS's bytes back "
          "after the pass; no miss");
    // The same with additive blending, with DCS's frame buffer popped inside the pass, with the OM entries lost
    // as well, and with a blend no repair could invert (MAX): nothing is drawn again, the cockpit is right.
    lightPop = true;
    {
      lightBlend = true;
      frame();  // a seen draw: split as before
      const bool seen = allLitAsGb();
      bool all = true;
      for (int mode = 0; mode < 4; ++mode) {
        lightBlend = mode != 1;
        omLost = mode == 2;
        maxBlendOn = mode == 3;
        drawToggle = 2;
        frame();
        drawToggle = 0;
        omLost = false;
        const bool ok = allLitTrue();
        if (!ok)
          printf("     (missed draw, mode %d: lit %.6f / %.6f once %d)\n", mode, res[0].lit[0], res[0].lit[1],
                 res[0].litOnce);
        all = all && ok;
        frame();
      }
      maxBlendOn = false;
      Check(seen && all && ck::g_st.lightMiss.load() == miss0,
            "cockpit ipd lighting (live shapes): a missed draw with additive blending, the frame buffer popped "
            "inside the pass, the OM entries lost too, or a MAX blend: the cockpit lit from its G-buffer camera, "
            "every pixel once, no miss; a seen draw split as before");
    }
    lightPop = false;
    lightBlend = true;
    // The end of the boost: the tail frame (Dw 0, gPrev only): G-buffer and lighting both at DCS's camera.
    scale = 1.0;
    const uint64_t miss1 = ck::g_st.lightMiss.load();
    frame();
    bool tail = allLitAsGb();
    for (int t = 0; t < 4; ++t) tail = tail && !shifted(t);
    frame();
    tail = tail && allLitAsGb();
    scale = 1.4;
    frame();
    frame();
    frame();  // (the step back to x1.4 is ambiguous while a scale-1 record is a plausible neighbour)
    const bool back = allLitAsGb() && shifted(0) && shifted(3);
    frame();
    Check(tail && back && ck::g_st.lightMiss.load() == miss1,
          "cockpit ipd lighting: tail frame and scale 1: lighting at DCS's camera like the G-buffer (no split, no "
          "miss); boost back: split again");
    lightAll = false;
  }
  ConfigureD3d11(1, false, true, 0, 0, false);

  // Stale: the layer stops writing; after 30 frames nothing is patched.
  writeBoost = false;
  const uint64_t stale0 = ck::g_st.offStale.load();
  for (int i = 0; i < 31; ++i) frame();
  frame();
  Check(allDcs() && ck::g_st.offStale.load() > stale0,
        "cockpit ipd T5: no new BoostOut record for 30 frames: stale, nothing patched");
  // The dev test shift without the layer: 10 mm per eye.
  ConfigureD3d11(1, false, true, 0, 10.0, false);
  frame();
  double a4 = 0, o4 = 1, a5 = 0, o5 = 1;
  Check(shiftOf(res[0].in[0], res[0].dcs, 0, &a4, &o4) && std::fabs(a4 - 0.010) < 2e-6 &&
            shiftOf(res[1].in[0], res[1].dcs, 0, &a5, &o5) && std::fabs(a5 + 0.010) < 2e-6,
        "cockpit ipd T5: CockpitTrueIpdTestShiftMm=10 without the layer: cockpit passes shifted 10 mm per eye");
  ConfigureD3d11(1, false, true, 0, 0, false);
  // Helmet models (2026-10-14, AH-64D: the IHADSS monocle 160 px beside its symbology at the 13.1 mm test shift).
  {
    ConfigureD3d11(1, false, true, 0, 13.1, false);
    helmetOn = helmetFwd = true;
    for (auto& a : g_hSeen)
      for (auto& b : a)
        for (auto& c : b) c.clear();
    const uint64_t sw0 = ck::g_helmetSwitches.load(), ref0 = ck::g_helmetRefused.load();
    frame();
    frame();
    const uint64_t sw1 = ck::g_helmetSwitches.load();
    auto trueOf = [&](const std::vector<uint8_t>& snap, int t) {
      const uint8_t* a = At(snap, firstOf[t]);
      return a ? std::vector<uint8_t>(a, a + ck::kCbBytes) : std::vector<uint8_t>();
    };
    // Per pass kind and view: the cockpit items and the two helmet models that are not anchored see the pass's
    // true-eye struct; the right-eye helmet sees DCS's own struct in the right eye's views and DCS's camera moved
    // by 2 Dw (twice the cockpit's shift, along the eye axis) in the left eye's.
    bool others = true, own = true, other = true, ps = true;
    double al[2][4] = {};
    for (int kind = 0; kind < 2; ++kind)
      for (int t = 0; t < 4; ++t) {
        const std::vector<uint8_t> tr = trueOf(kind ? hFwdIn[t] : res[t].in[0], t);
        for (int k : {0, 2, 3, 4}) others = others && !tr.empty() && g_hSeen[kind][t][k] == tr;
        // (item 5, the anchored helmet's far node, must see exactly what item 1 sees)
        own = own && g_hSeen[kind][t][5] == g_hSeen[kind][t][1];
        const std::vector<uint8_t>& h = g_hSeen[kind][t][1];
        double a = 0, o = 1, ah = 0, oh = 1;
        std::vector<uint8_t> hb(h);
        hb.insert(hb.begin(), static_cast<size_t>(firstOf[t]) * 16, 0);  // (shiftOf reads at the binding offset)
        const bool okTrue = shiftOf(kind ? hFwdIn[t] : res[t].in[0], res[t].dcs, firstOf[t], &a, &o);
        if (t & 1) {
          own = own && h.size() == ck::kCbBytes && !memcmp(h.data(), res[t].dcs.data(), ck::kCbBytes);
        } else {
          other = other && okTrue && h.size() == ck::kCbBytes && shiftOf(hb, res[t].dcs, firstOf[t], &ah, &oh) &&
                  std::fabs(ah - 2 * a) < 2e-6 && oh < 2e-6 && std::fabs(a - 0.0131) < 2e-6;
          const uint8_t* hp = h.size() == ck::kCbBytes ? h.data() : nullptr;
          other = other && hp && ck::SelfCheck(hp) == 0 && FieldIdentical(hp, res[t].dcs.data(), ck::kOffProj);
        }
        al[kind][t] = ah;
        for (int k = 0; k < kHItems; ++k) ps = ps && g_hPsSame[kind][t][k];
      }
    void** v0 = reinterpret_cast<void**>(gr.vecs.data() + static_cast<size_t>(idx[0][0]) * 24);
    bool back = v0[0] == hList && v0[1] == hList + kHItems && !ck::t_helmetBind.active && !ck::t_helmetSwap.vec;
    for (int t = 0; t < 4; ++t) back = back && same(res[t].after[0], res[t].dcs, firstOf[t]);  // DCS's bytes after
    printf("     (helmet: switches %llu in 2 frames, left-eye helmet shift %+.2f / %+.2f mm (G-buffer / forward, view "
           "0), refused %llu)\n",
           static_cast<unsigned long long>(sw1 - sw0), al[0][0] * 1e3, al[1][0] * 1e3,
           static_cast<unsigned long long>(ck::g_helmetRefused.load() - ref0));
    Check(others && own && other && ps && back && sw1 - sw0 == 32 && ck::g_helmetRefused.load() == ref0,
          "cockpit ipd helmet: a helmet model anchored to DCS's right eye (every item of its instance, a node 17 cm "
          "away too) is drawn with DCS's own camera in the right eye's views and with DCS's camera moved by 2 Dw in "
          "the left eye's (G-buffer and forward cockpit pass); the cockpit, a head-centred helmet and one off the head "
          "keep the true eye; PS b7 follows VS b7; DCS's item vector and DCS's bytes back after each pass");
    // [Perception] CockpitTrueIpdHelmet=0: every item at the true eye (as before).
    ck::g_cfgHelmet = false;
    frame();
    bool offTrue = true;
    for (int t = 0; t < 4; ++t)
      offTrue = offTrue && g_hSeen[0][t][1] == trueOf(res[t].in[0], t) && g_hSeen[1][t][1] == trueOf(hFwdIn[t], t);
    ck::g_cfgHelmet = true;
    // No shift (the cockpit at DCS's camera; the first such frame is the gPrev tail): the helmet as the cockpit,
    // nothing switched.
    ConfigureD3d11(1, false, true, 0, 0, false);
    const uint64_t sw2 = ck::g_helmetSwitches.load();
    frame();
    frame();
    bool unshifted = true;
    for (int t = 0; t < 4; ++t)
      for (int k = 0; k < kHItems; ++k)
        unshifted = unshifted && g_hSeen[0][t][k].size() == ck::kCbBytes &&
                    !memcmp(g_hSeen[0][t][k].data(), res[t].dcs.data(), ck::kCbBytes);
    printf("     (helmet off: at the true eye %d; no shift: DCS's bytes %d, switches %llu)\n", offTrue, unshifted,
           static_cast<unsigned long long>(ck::g_helmetSwitches.load() - sw2));
    Check(offTrue && unshifted && ck::g_helmetSwitches.load() == sw2,
          "cockpit ipd helmet: CockpitTrueIpdHelmet=0 draws the helmet at the true eye like the cockpit; with no "
          "shift every item, the helmet included, is drawn with DCS's bytes and nothing is switched");
    helmetOn = helmetFwd = false;
    frame();  // (the vectors emptied, DCS's page globals back)
  }
  // ---- The ownship (CockpitTrueIpdOwnship, stage 1): the own aircraft's exterior model instance locked once and
  // all its items drawn from the true eye in the world G-buffer, decal and transparent passes; the recorder told
  // to step aside for exactly those executions. ----
  {
    auto clearSeen = [&] {
      for (auto& a : g_oSeen)
        for (auto& b : a)
          for (auto& c : b) c.clear();
      memset(g_oAside, 0, sizeof(g_oAside));
    };
    auto trueOf = [&](const std::vector<uint8_t>& snap, int t) {
      const uint8_t* a = At(snap, firstOf[t]);
      return a ? std::vector<uint8_t>(a, a + ck::kCbBytes) : std::vector<uint8_t>();
    };
    // Every recorded draw of item i in this frame: the true eye (the cockpit pass's struct) or DCS's bytes.
    auto itemsAt = [&](std::initializer_list<int> items, bool trueEye, bool aside) {
      bool ok = true;
      for (int k = 1; k <= 3; ++k)
        for (int t = 0; t < 4; ++t)
          for (int i : items) {
            const std::vector<uint8_t>& v = g_oSeen[k][t][i];
            const std::vector<uint8_t> want = trueEye ? trueOf(res[t].in[0], t) : res[t].dcs;
            ok = ok && v.size() == ck::kCbBytes && v == want && (k == 3 || g_oAside[k][t][i] == aside);
          }
      return ok;
    };
    auto vectorsBack = [&] {
      bool ok = !ck::t_ownSwap.vec && !ck::t_ownSrc.bound && !gbrec::t_ownAside;
      for (int t = 0; t < 4; ++t)
        for (int k = 1; k < 3; ++k) {
          void** v = reinterpret_cast<void**>(gr.vecs.data() + static_cast<size_t>(idx[t][k]) * 24);
          ok = ok && v[0] == oList && v[1] == oList + oListN;
        }
      for (int t = 0; t < 4; ++t) ok = ok && same(res[t].after[1], res[t].dcs, firstOf[t]) &&
                                       same(res[t].after[2], res[t].dcs, firstOf[t]);
      return ok;
    };
    ConfigureD3d11(1, false, true, 0, 13.1, false);
    ownOn = true;
    // Off (default): today's frame bit for bit: no lock, nothing swapped or switched, DCS's bytes everywhere.
    ck::SetOwnship(false);
    const uint64_t sw0 = ck::g_ownSwitches.load(), ps0 = ck::g_ownPasses.load();
    bool offOk = true;
    for (int i = 0; i < 70; ++i) {
      clearSeen();
      frame();
      offOk = offOk && itemsAt({0, 1, 2}, false, false) && vectorsBack();
    }
    offOk = offOk && !ck::g_ownLocked.load() && ck::g_ownSwitches.load() == sw0 && ck::g_ownPasses.load() == ps0 &&
            !gbrec::g_ownAsideHint.load();
    Check(offOk, "cockpit ipd ownship: CockpitTrueIpdOwnship=0: no lock, no vector swapped, no switch, the recorder "
                 "never told to step aside, every world item drawn with DCS's bytes (today's frame)");
    // On: locked after kOwnLockFrames frames of the same nearest instance; until then today's frame.
    ck::SetOwnship(true);
    int lockedAt = -1;
    bool beforeOk = true;
    for (int i = 0; i < ck::kOwnLockFrames + 5 && lockedAt < 0; ++i) {
      clearSeen();
      frame();
      if (ck::g_ownLocked.load())
        lockedAt = i;
      else
        beforeOk = beforeOk && itemsAt({0, 1, 2}, false, false);
    }
    if (lockedAt != ck::kOwnLockFrames - 1)
      printf("     (ownship lock: at %d, candidate %p n %d, lock frame %llu / frame %llu, srvt %p ng %p, globals %p)\n",
             lockedAt, ck::g_own.cand, ck::g_own.candN, static_cast<unsigned long long>(ck::g_ownLockFrame),
             static_cast<unsigned long long>(ck::g_frame), ck::SrVtbl(), static_cast<void*>(GetModuleHandleW(L"NGModel.dll")),
             static_cast<const void*>(gbrec::g_env.globals));
    Check(lockedAt == ck::kOwnLockFrames - 1 && beforeOk && strstr(ck::g_own.path, "ah-64d_bl2") != nullptr,
          "cockpit ipd ownship: the exterior instance nearest the eyes (1.2 m; the next instance 9 m) is locked "
          "after kOwnLockFrames frames; nothing is moved before the lock");
    // Locked: every frame the ownship's items (both, every view and pass) at the true eye, the other aircraft's at
    // DCS's camera, the recorder told to step aside for those executions only while they run; no flicker.
    bool lockedOk = true;
    const uint64_t sw1 = ck::g_ownSwitches.load();
    for (int i = 0; i < 20; ++i) {
      clearSeen();
      frame();
      lockedOk = lockedOk && itemsAt({0, 1}, true, true) && itemsAt({2}, false, true) && vectorsBack();
    }
    const uint64_t perFrame = (ck::g_ownSwitches.load() - sw1) / 20;
    if (!lockedOk || perFrame != 12) {
      for (int k = 1; k <= 3; ++k)
        for (int t = 0; t < 4; ++t) {
          const std::vector<uint8_t> tr = trueOf(res[t].in[0], t);
          printf("     (ownship k %d view %d: items 0/1/2 sizes %zu %zu %zu, true %d %d, dcs %d, aside %d %d %d)\n", k, t,
                 g_oSeen[k][t][0].size(), g_oSeen[k][t][1].size(), g_oSeen[k][t][2].size(), g_oSeen[k][t][0] == tr,
                 g_oSeen[k][t][1] == tr, g_oSeen[k][t][2] == res[t].dcs, g_oAside[k][t][0], g_oAside[k][t][1],
                 g_oAside[k][t][2]);
        }
      printf("     (ownship: switches per frame %llu, no source %llu, passes %llu fwd %llu, vectors back %d, fault %d %d, frames patched %llu, off next %llu fail %llu)\n",
             static_cast<unsigned long long>(perFrame), static_cast<unsigned long long>(ck::g_ownNoSrc.load()),
             static_cast<unsigned long long>(ck::g_ownPasses.load()), static_cast<unsigned long long>(ck::g_ownFwdPasses.load()),
             vectorsBack(), (int)ck::g_fault.load(), (int)fixdepth::g_fault.load(), (unsigned long long)ck::g_st.framesPatched.load(), (unsigned long long)ck::g_st.offNext.load(), (unsigned long long)ck::g_st.failNoUpload.load());
    }
    Check(lockedOk && perFrame == 12 && gbrec::g_ownAsideHint.load() && ck::g_ownNoSrc.load() == 0,
          "cockpit ipd ownship: locked: 20 frames in a row the ownship's items at the true eye in every view's world "
          "G-buffer, decal and transparent pass (one switch each), the other aircraft at DCS's camera, the recorder "
          "stepping aside for exactly those executions; DCS's vectors and bytes back after each pass");
    // A nearer decoy instance for 10 frames, then the ownship absent for 20 (< kOwnLostFrames): the selection
    // never changes, the ownship never drawn at DCS's camera while present.
    const void* key = ck::g_own.key;
    bool fixedOk = true;
    ownDecoy = true;
    for (int i = 0; i < 10; ++i) {
      clearSeen();
      frame();
      fixedOk = fixedOk && ck::g_own.key == key && itemsAt({0, 1}, true, true) && itemsAt({2, 3}, false, true);
    }
    ownDecoy = false;
    ownAbsent = true;
    for (int i = 0; i < 20; ++i) {
      clearSeen();
      frame();
      fixedOk = fixedOk && ck::g_own.key == key && ck::g_ownLocked.load() && itemsAt({2}, false, false);
    }
    ownAbsent = false;
    for (int i = 0; i < 5; ++i) {
      clearSeen();
      frame();
      fixedOk = fixedOk && ck::g_own.key == key && itemsAt({0, 1}, true, true);
    }
    Check(fixedOk, "cockpit ipd ownship: a fixed selection: a nearer decoy instance (10 frames) and the ownship absent "
                   "(20 frames) never change the lock; the ownship at the true eye in every frame it is drawn");
    // World passes without an upload of their own: the switch starts from the view's cockpit struct (the buffer
    // bound at b7 is the one the cockpit pass uploaded to); the ownship still at the true eye, nothing else moved.
    {
      g_noUploadWorld = true;
      const uint64_t fc0 = ck::g_ownFromCockpit.load(), ns0 = ck::g_ownNoSrc.load();
      bool nuOk = true;
      for (int i = 0; i < 5; ++i) {
        clearSeen();
        frame();
        nuOk = nuOk && itemsAt({0, 1}, true, true) && itemsAt({2}, false, true) && vectorsBack();
      }
      g_noUploadWorld = false;
      const uint64_t fromCk = ck::g_ownFromCockpit.load() - fc0;
      if (!nuOk || fromCk != 40) printf("     (ownship without world uploads: ok %d, from the cockpit struct %llu)\n", nuOk,
                                        static_cast<unsigned long long>(fromCk));
      Check(nuOk && fromCk == 40 && ck::g_ownNoSrc.load() == ns0,
            "cockpit ipd ownship: world passes that upload no cPerView of their own: the switch starts from the view's "
            "cockpit struct (same buffer at b7), the ownship at the true eye, the rest at DCS's camera");
    }
    // No shift (the cockpit at DCS's camera): after the tail frame nothing is switched, DCS's bytes everywhere.
    ConfigureD3d11(1, false, true, 0, 0, false);
    frame();
    frame();
    const uint64_t sw2 = ck::g_ownSwitches.load();
    clearSeen();
    frame();
    const bool unshifted = itemsAt({0, 1, 2}, false, false) && ck::g_ownSwitches.load() == sw2;
    // Off again: unlocked at the frame end, the recorder's hint gone within kOwnAsideHold frames, today's frame.
    ConfigureD3d11(1, false, true, 0, 13.1, false);
    ck::SetOwnship(false);
    bool backOk = true;
    int hintFrames = 0;
    for (int i = 0; i < ck::kOwnAsideHold + 2; ++i) {
      clearSeen();
      frame();
      backOk = backOk && itemsAt({0, 1, 2}, false, false) && vectorsBack();
      if (gbrec::g_ownAsideHint.load()) ++hintFrames;
    }
    Check(unshifted && backOk && !ck::g_ownLocked.load() && !gbrec::g_ownAsideHint.load() && hintFrames == 0,
          "cockpit ipd ownship: no shift: nothing switched; option off again: unlocked, the recorder's job hint off, "
          "today's frame (DCS's bytes, DCS's vectors)");
    ownOn = false;
    ConfigureD3d11(1, false, true, 0, 0, false);
    frame();
    // The recorder's side (gb_rec.h AsideRun, called by its wrapper once the execution's slot is known): with the
    // flag the execution is DCS's own (stock), its slot counted as having run and its armed job left untouched
    // (dropped as stale at the next render entry); without the flag nothing happens (the recorder runs as before).
    {
      gbrec::Slot& sl = gbrec::g_slot[2];
      const bool armed0 = sl.armed;
      gbrec::Job* job0 = sl.job;
      const int64_t q0 = sl.lastPassQpc;
      const uint64_t n0 = gbrec::g_ownAsidePasses.load();
      int dummyPass = 0, dummyCtx = 0;
      gbrec::t_ownAside = false;
      g_asideCalls = 0;
      const bool r0 = gbrec::AsideRun(2, &dummyPass, &dummyCtx, &AsideExec);
      const bool noEffect = !r0 && g_asideCalls == 0 && gbrec::g_ownAsidePasses.load() == n0 && sl.lastPassQpc == q0;
      gbrec::t_ownAside = true;
      const bool r1 = gbrec::AsideRun(2, &dummyPass, &dummyCtx, &AsideExec);
      const bool stepped = r1 && g_asideCalls == 1 && g_asidePass == &dummyPass &&
                           gbrec::g_ownAsidePasses.load() == n0 + 1 && sl.lastPassQpc > q0 && sl.armed == armed0 &&
                           sl.job == job0;
      const bool noSlot = !gbrec::AsideRun(-1, &dummyPass, &dummyCtx, &AsideExec) && g_asideCalls == 1;
      gbrec::t_ownAside = false;
      sl.lastPassQpc = q0;
      Check(noEffect && stepped && noSlot,
            "cockpit ipd ownship: the recorder steps aside only when told (an execution whose vector the ownship "
            "swapped): DCS's own execute runs once, the slot counted as run, its job untouched; untold, nothing "
            "changes");
    }
    // Stage 2 (CockpitTrueIpdOwnship=2): the locked instance published to the recorder's jobs (gbrec::g_ownKey), no
    // step-aside hint; with the recorder off (as here) every execution falls back to stage 1: the same image. A
    // switch entry met in a pass that moves no ownship (a stage-2 job's list at scale 1) switches nothing.
    {
      ConfigureD3d11(1, false, true, 0, 13.1, false);
      ck::SetOwnship(2);
      ownOn = true;
      for (int i = 0; i < ck::kOwnLockFrames + 2 && !ck::g_ownLocked.load(); ++i) frame();
      bool s2Ok = ck::g_ownLocked.load();
      for (int i = 0; i < 10 && s2Ok; ++i) {
        clearSeen();
        frame();
        s2Ok = itemsAt({0, 1}, true, true) && itemsAt({2}, false, true) && vectorsBack();
      }
      const bool keyOk = gbrec::g_ownKey.load() == ck::g_own.key && ck::g_own.key && !gbrec::g_ownAsideHint.load() &&
                         !gbrec::t_own2;
      const uint64_t idle0 = ck::g_ownSwitchIdle.load(), sw0 = ck::g_ownSwitches.load();
      ck::t_ownArmed = false;
      ck::OwnSwitch(true);
      const bool idleOk = ck::g_ownSwitchIdle.load() == idle0 + 1 && ck::g_ownSwitches.load() == sw0 &&
                          !ck::t_ownSrc.bound;
      ck::SetOwnship(0);
      for (int i = 0; i < 2; ++i) frame();
      const bool offOk = !ck::g_ownLocked.load() && gbrec::g_ownKey.load() == nullptr;
      ownOn = false;
      ConfigureD3d11(1, false, true, 0, 0, false);
      frame();
      Check(s2Ok && keyOk && idleOk && offOk,
            "cockpit ipd ownship stage 2: the locked instance published to the recorder's jobs, no step-aside hint; "
            "the recorder off: stage 1 for every execution (the ownship at the true eye, the other aircraft at DCS's "
            "camera); a switch entry in a pass with nothing to move switches nothing; off: key withdrawn");
    }
    // Mission / slot events and scene cuts (live 2026-10-14, 0.5.9: the previous mission's instance stayed locked
    // for 900 frames after each load). An event unlocks at the next lock step (nothing moved in that frame, the
    // recorder's key withdrawn), the next aircraft locks kOwnLockFrames later. After a gap / jump cut the instance
    // must be seen again within kOwnRecheckFrames lock steps; seen once, the cut is forgotten; a turn cut (a head
    // turn over a hitch) changes nothing.
    {
      ConfigureD3d11(1, false, true, 0, 13.1, false);
      ck::SetOwnship(true);
      ownOn = true;
      auto lockNow = [&] {
        for (int i = 0; i < ck::kOwnLockFrames + 2 && !ck::g_ownLocked.load(); ++i) frame();
        return ck::g_ownLocked.load();
      };
      bool evOk = lockNow();
      const uint64_t ev0 = ck::g_ownEventUnlocks.load(), un0 = ck::g_ownUnlocks.load();
      ck::OwnOnMissionEvent();
      clearSeen();
      frame();
      evOk = evOk && !ck::g_ownLocked.load() && !ck::g_own.key && ck::g_ownEventUnlocks.load() == ev0 + 1 &&
             ck::g_ownUnlocks.load() == un0 + 1 && gbrec::g_ownKey.load() == nullptr && itemsAt({0, 1, 2}, false, false);
      int relockAt = -1;
      for (int i = 0; i < ck::kOwnLockFrames + 5 && relockAt < 0; ++i) {
        frame();
        if (ck::g_ownLocked.load()) relockAt = i;
      }
      evOk = evOk && relockAt == ck::kOwnLockFrames - 2;  // (the event frame was the first candidate frame)
      // An event while unlocked: nothing to count, nothing breaks.
      ck::SetOwnship(false);
      frame();
      ck::OwnOnMissionEvent();
      frame();
      evOk = evOk && ck::g_ownEventUnlocks.load() == ev0 + 1;
      ck::SetOwnship(true);
      if (!evOk) printf("     (ownship events: unlocks %llu, relock at %d)\n",
                        static_cast<unsigned long long>(ck::g_ownEventUnlocks.load() - ev0), relockAt);
      Check(evOk, "cockpit ipd ownship: a mission / slot event unlocks at the next lock step (nothing moved in that "
                  "frame, the recorder's key withdrawn); the aircraft is locked again kOwnLockFrames later");
      // Jump cut, the instance gone: unlocked after kOwnRecheckFrames lock steps.
      bool cutOk = lockNow();
      const uint64_t cu0 = ck::g_ownCutUnlocks.load();
      scenecut::g_cutsByReason[scenecut::kJump]++;
      ownAbsent = true;
      int unlockedAt = -1;
      for (int i = 0; i < ck::kOwnRecheckFrames + 5 && unlockedAt < 0; ++i) {
        frame();
        if (!ck::g_ownLocked.load()) unlockedAt = i;
      }
      ownAbsent = false;
      cutOk = cutOk && unlockedAt == ck::kOwnRecheckFrames - 1 && ck::g_ownCutUnlocks.load() == cu0 + 1;
      // Jump cut, the instance seen again at once: the cut forgotten (absent for 20 frames afterwards: still locked).
      cutOk = cutOk && lockNow();
      const void* key2 = ck::g_own.key;
      scenecut::g_cutsByReason[scenecut::kGap]++;
      frame();
      ownAbsent = true;
      for (int i = 0; i < 20; ++i) frame();
      ownAbsent = false;
      cutOk = cutOk && ck::g_ownLocked.load() && ck::g_own.key == key2;
      // A turn cut: nothing (absent for 20 frames: still locked).
      scenecut::g_cutsByReason[scenecut::kTurn]++;
      ownAbsent = true;
      for (int i = 0; i < 20; ++i) frame();
      ownAbsent = false;
      frame();
      cutOk = cutOk && ck::g_ownLocked.load() && ck::g_own.key == key2 && ck::g_ownCutUnlocks.load() == cu0 + 1;
      if (!cutOk) printf("     (ownship cuts: unlocked at %d, cut unlocks %llu, locked %d)\n", unlockedAt,
                         static_cast<unsigned long long>(ck::g_ownCutUnlocks.load() - cu0), (int)ck::g_ownLocked.load());
      Check(cutOk, "cockpit ipd ownship: after a gap / jump scene cut a missing instance is unlocked within "
                   "kOwnRecheckFrames lock steps; seen again at once, the cut is forgotten; a turn cut changes nothing");
      // Stage 2 with the recorder on: the passes move the instance only once its key is published to the recorder's
      // jobs (the frame that locks moves nothing anywhere: no job dropped for its key); stage 1 needs no key.
      {
        const bool on0 = gbrec::g_on.load();
        const int st0 = gbrec::g_state.load();
        const bool dis0 = gbrec::g_disabled.load();
        const void* pubKey0 = gbrec::g_ownKey.load();
        gbrec::g_on = true;
        gbrec::g_state = 1;
        gbrec::g_disabled = false;
        const bool active = gbrec::Active();
        ck::SetOwnship(2);
        gbrec::g_ownKey = nullptr;
        const bool before = ck::OwnMoves();
        gbrec::g_ownKey = ck::g_own.key;
        const bool after = ck::OwnMoves();
        static int otherKey = 0;
        gbrec::g_ownKey = &otherKey;
        const bool other = ck::OwnMoves();
        ck::SetOwnship(1);
        gbrec::g_ownKey = nullptr;
        const bool s1 = ck::OwnMoves();
        gbrec::g_on = on0;
        gbrec::g_state = st0;
        gbrec::g_disabled = dis0;
        gbrec::g_ownKey = pubKey0;
        if (!active) printf("     (ownship gate: the recorder could not be marked active here)\n");
        Check(active && ck::g_ownLocked.load() && !before && after && !other && s1,
              "cockpit ipd ownship stage 2: with the recorder on, the ownship moves only once the locked key is the "
              "one published to the recorder's jobs; stage 1 moves it as soon as it is locked");
      }
      ck::SetOwnship(false);
      frame();
      ownOn = false;
      ConfigureD3d11(1, false, true, 0, 0, false);
      frame();
    }
  }
  // Trace ([Perception] CockpitTrueIpdLog).
  ConfigureD3d11(1, false, true, 2, 0, false);
  frame();
  frame();
  frame();  // the second traced frame ends at this frame's start
  size_t traced = 0;
  bool glassTraced = false;
  {
    std::lock_guard<std::mutex> lock(ck::g_traceMutex);
    traced = ck::g_trace.size();
    for (const std::string& l : ck::g_trace)
      if (l.find("CockpitGlassEffects") != std::string::npos) glassTraced = true;
    ck::g_trace.clear();
  }
  Check(traced > 20 && glassTraced && ck::g_logLeft.load() == 0,
        "cockpit ipd: CockpitTrueIpdLog=2 traced 2 frames, incl. watched cockpit passes (CockpitGlassEffects)");
  ConfigureD3d11(1, false, true, 0, 0, false);
  // gPrev trace ([Perception] CockpitTrueIpdPrevLog).
  ConfigureD3d11(1, false, true, 0, 0, false, 2);
  frame();
  frame();
  frame();
  {
    size_t pv = 0;
    bool hasY = false, hasX = false, hasRel = false, hasP = false, hasO = false, hasPrev = false;
    std::lock_guard<std::mutex> lock(ck::g_traceMutex);
    for (const std::string& l : ck::g_trace) {
      if (l.rfind("pv ", 0) != 0) continue;
      ++pv;
      hasY = hasY || l.find("Y=prevVP^-1*PVP") != std::string::npos;
      hasX = hasX || l.find("X=PVP*prevVP^-1") != std::string::npos;
      hasRel = hasRel || (l.find(" rel: d ") != std::string::npos && l.find(" ok") != std::string::npos);
      hasP = hasP || l.find(" P ") != std::string::npos;
      hasO = hasO || (l.find(" origin ") != std::string::npos && l.find(" campos ") != std::string::npos);
      hasPrev = hasPrev || l.find(" prevVP ") != std::string::npos;
    }
    ck::g_trace.clear();
    printf("     (gPrev trace: %zu lines, Y %d X %d rel %d P %d origin %d prevVP %d)\n", pv, hasY, hasX, hasRel, hasP, hasO,
           hasPrev);
    Check(pv >= 2 * 4 * 8 && pv <= 2 * 4 * 9 && hasY && hasX && hasRel && hasP && hasO && hasPrev &&
              ck::g_prevLogLeft.load() == 0 && allDcs(),
          "cockpit ipd: CockpitTrueIpdPrevLog=2: per view and frame gViewProj, gPrevFrameViewProj, gProj, gOrigin / "
          "gCameraPos, the stored previous frame and the relations, for 2 frames");
  }
  ConfigureD3d11(1, false, true, 0, 0, false, 0);
  Check(g_dispatches.load() == disp0 && g_published.load() == pub0 && !g_depthView.load() && !g_gridView.load(),
        "cockpit ipd T8: with only the cockpit option on, fixation depth samples, maps and publishes nothing");

  // Off: the uninstall, then a frame with the module on for nothing else.
  ConfigureD3d11(0, false, true, 0, 0, false);
  fixdepth::Uninstall();
  Check(!ck::g_on.load() && !gbpass::g_obs.load() && !ck::g_boostView.load() && !g_installed.load(),
        "cockpit ipd T8: off: observer out, BoostOut unmapped, module uninstalled");
  {
    // gbpass::Hook with no observer: DCS's body exactly once.
    g_x = Exec{};
    alignas(16) uint8_t ms[ck::kCbBytes] = {};
    g_x.cb = cbA;
    g_x.data = ms;
    gbpass::Hook(&gbp[0][0], gr.ctxBlock);
    Check(g_x.calls == 1, "cockpit ipd T8: gbpass::Hook without the observer calls DCS's body once, as before");
  }

  // Teardown.
  gbpass::g_state = oldGbState;
  gbpass::g_orig = oldOrig;
  gbpass::g_wrap = oldWrap;
  shadowbatch::g_qLastLearnRg = oldLearnRg;
  shadowinst::g_device = oldDev;
  g_gazeName = oldGaze;
  ck::g_boostNameOverride.clear();
  if (bview) UnmapViewOfFile(bview);
  if (bm) CloseHandle(bm);
  g_c->ClearState();
  for (int i = 0; i < 5; ++i) Rel(dsrv[i]), Rel(dsv[i]), Rel(dtex[i]);
  Rel(defDsv), Rel(defTex), Rel(ckDss), Rel(cbA), Rel(cbB), Rel(cbC);
  Rel(litRtv), Rel(litTex), Rel(litVs), Rel(litPs), Rel(addBlend), Rel(maxBlend), Rel(noDepth), Rel(fxCb);
  Rel(ldrRtv), Rel(ldrTex);
  Rel(g_c), Rel(imm);
  const ULONG last = dev->Release();
  Check(last == 0, "cockpit ipd pipeline: no D3D reference leaked (device released to 0)");
}

// ---------------------------------------------------------------------------
// T8 off is bit-identical (no device needed for most of it)
// ---------------------------------------------------------------------------

const void* g_fwdSeen = nullptr;
void STDMETHODCALLTYPE CaptureUs(ID3D11DeviceContext*, ID3D11Resource*, UINT, const D3D11_BOX*, const void* src, UINT,
                                 UINT) {
  g_fwdSeen = src;
}
void STDMETHODCALLTYPE CaptureUs1(ID3D11DeviceContext1*, ID3D11Resource*, UINT, const D3D11_BOX*, const void* src,
                                  UINT, UINT, UINT) {
  g_fwdSeen = src;
}

void TestOff() {
  ConfigureD3d11(0, false, true, 0, 0, false);
  const uint64_t inst0 = ck::g_gbInstallCalls.load(), opens0 = ck::g_boostOpens.load(), starts0 = ck::g_starts.load();
  fixdepth::Tick(false, false, 0, L"", false, L"");  // the config thread's call with every Perception output off
  Check(!ck::Wanted() && !ck::WantUploads() && !ck::g_on.load() && !gbpass::g_obs.load() &&
            ck::g_gbInstallCalls.load() == inst0 && ck::g_boostOpens.load() == opens0 &&
            ck::g_starts.load() == starts0 && !g_installed.load(),
        "cockpit ipd T8: CockpitTrueIpd=0: nothing installed, no gbpass install, no mapping opened, no observer");
  // The upload hooks forward DCS's very pointer when the option is off.
  const UsFn oldUs = g_usOrig;
  const Us1Fn oldUs1 = g_us1Orig;
  const bool oldArmed = g_usArmed;
  g_usOrig = &CaptureUs;
  g_us1Orig = &CaptureUs1;
  g_usArmed = false;
  uint8_t bytes[64] = {1, 2, 3};
  g_fwdSeen = nullptr;
  UsHook(g_s.ctx, nullptr, 0, nullptr, bytes, 0, 0);
  const bool a = g_fwdSeen == bytes;
  g_fwdSeen = nullptr;
  Us1Hook(g_s.ctx1, nullptr, 0, nullptr, bytes, 0, 0, 0);
  const bool b = g_fwdSeen == bytes;
  g_usOrig = oldUs;
  g_us1Orig = oldUs1;
  g_usArmed = oldArmed;
  Check(a && b, "cockpit ipd T8: UpdateSubresource / UpdateSubresource1 hooks forward the identical pointer when off");
  // The context table: an in-pass arm with neither far depth nor the cockpit option leaves the upload entries alone.
  ID3D11Device* dev = nullptr;
  ID3D11DeviceContext* imm = nullptr;
  const D3D_FEATURE_LEVEL want[] = {D3D_FEATURE_LEVEL_11_1};
  if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, want, 1, D3D11_SDK_VERSION, &dev, nullptr,
                               &imm))) {
    QvSkip("cockpit ipd T8 table: no WARP device\n");
    return;
  }
  fdexport::g_on = false;
  const ID3D11Device* oldDev = g_s.dev;
  (void)oldDev;
  State saved = g_s;
  g_s = State{};
  g_s.ctx = imm;
  void** vt = *reinterpret_cast<void***>(imm);
  std::vector<void*> snap(vt, vt + 160);
  const int us = DcsQv_ContextSlot("UpdateSubresource"), us1 = DcsQv_ContextSlot("UpdateSubresource1");
  ArmOm();
  const bool usUntouched = vt[us] == snap[us] && vt[us1] == snap[us1] && !g_usArmed;
  DisarmOm();
  const bool same = std::equal(snap.begin(), snap.end(), vt);
  // The uninstall stops every re-arm before it disarms (g_rearmOk false, hooks drained): a render-thread re-arm
  // after that never writes our hook back over d3d11's entry.
  bool noRearm = false;
  if (ArmUsOnly(false)) {
    void* const real = vt[us] == reinterpret_cast<void*>(&UsHook) ? reinterpret_cast<void*>(g_usOrig) : vt[us];
    d3ds::WriteTablePointer(&vt[us], reinterpret_cast<void*>(&fartest::VariantUs));  // d3d11 switched it
    g_rearmOk = false;
    UsRearm();
    noRearm = vt[us] == reinterpret_cast<void*>(&fartest::VariantUs);
    g_rearmOk = true;
    d3ds::WriteTablePointer(&vt[us], real);
    g_usOrig = reinterpret_cast<UsFn>(real);
    DisarmUsOnly();
  }
  const bool same2 = std::equal(snap.begin(), snap.end(), vt);
  g_s = saved;
  Check(usUntouched && same,
        "cockpit ipd T8: off: the in-pass arm never takes the upload entries; the context table is bit-identical after");
  Check(noRearm && same2, "cockpit ipd T8: re-arms stopped (uninstall): a switched upload entry stays d3d11's; the "
                          "table bit-identical after the arm is gone");
  imm->Release();
  dev->Release();
}

// ---------------------------------------------------------------------------
// T9 the stencil split, white box (D24S8, D32S8, MSAA skip, debug layer)
// ---------------------------------------------------------------------------

// b7True: the lighting pass's cPerView went to the GPU at the true eye (the default since 2026-10-13): draw 1
// binds a private b7 with DCS's bytes, draw 2 DCS's bound one.
void TestSplit(DXGI_FORMAT texFmt, bool msaa, bool debug, bool b7True = false) {
  const char* fmtName = texFmt == DXGI_FORMAT_R24G8_TYPELESS ? (b7True ? "D24S8 (b7 at the true eye)" : "D24S8")
                                                             : "D32S8";
  ID3D11Device* dev = nullptr;
  ID3D11DeviceContext* imm = nullptr;
  const D3D_FEATURE_LEVEL want[] = {D3D_FEATURE_LEVEL_11_1};
  if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, debug ? D3D11_CREATE_DEVICE_DEBUG : 0, want, 1,
                               D3D11_SDK_VERSION, &dev, nullptr, &imm))) {
    QvSkip("cockpit ipd T9 %s%s: no WARP%s device\n", fmtName, msaa ? " MSAA" : "", debug ? " debug" : "");
    return;
  }
  ID3D11DeviceContext1* c = nullptr;
  imm->QueryInterface(__uuidof(ID3D11DeviceContext1), reinterpret_cast<void**>(&c));
  ID3D11InfoQueue* q = nullptr;
  if (debug) dev->QueryInterface(__uuidof(ID3D11InfoQueue), reinterpret_cast<void**>(&q));
  const UINT W = 32, Hh = 16;
  const UINT samples = msaa ? 4 : 1;
  // Depth+stencil: stencil 0x28 (cockpit), 0x2f (cockpit, low bits set), 0x10, 0x08 in four columns.
  D3D11_TEXTURE2D_DESC td{};
  td.Width = W;
  td.Height = Hh;
  td.MipLevels = td.ArraySize = 1;
  td.Format = texFmt;
  td.SampleDesc.Count = samples;
  td.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
  ID3D11Texture2D* tex = nullptr;
  dev->CreateTexture2D(&td, nullptr, &tex);
  const uint8_t stv[4] = {0x28, 0x2f, 0x10, 0x08};
  if (tex && !msaa) {
    const UINT bpp = texFmt == DXGI_FORMAT_R24G8_TYPELESS ? 4 : 8;
    std::vector<uint8_t> px(W * Hh * bpp, 0);
    for (UINT y = 0; y < Hh; ++y)
      for (UINT x = 0; x < W; ++x) {
        uint8_t* o = px.data() + (y * W + x) * bpp;
        const uint8_t st = stv[x / 8];
        if (bpp == 4) {
          const uint32_t u = 0x400000u | static_cast<uint32_t>(st) << 24;
          memcpy(o, &u, 4);
        } else {
          const float d = 0.25f;
          memcpy(o, &d, 4);
          o[4] = st;
        }
      }
    D3D11_TEXTURE2D_DESC sd = td;
    sd.BindFlags = 0;
    D3D11_SUBRESOURCE_DATA init{px.data(), W * bpp, 0};
    ID3D11Texture2D* src = nullptr;
    dev->CreateTexture2D(&sd, &init, &src);
    if (src) imm->CopyResource(tex, src);
    Rel(src);
  }
  ID3D11ShaderResourceView* srv = nullptr;
  {
    D3D11_SHADER_RESOURCE_VIEW_DESC rd{};
    rd.Format = texFmt == DXGI_FORMAT_R24G8_TYPELESS ? DXGI_FORMAT_R24_UNORM_X8_TYPELESS
                                                     : DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
    rd.ViewDimension = msaa ? D3D11_SRV_DIMENSION_TEXTURE2DMS : D3D11_SRV_DIMENSION_TEXTURE2D;
    rd.Texture2D.MipLevels = 1;
    if (tex) dev->CreateShaderResourceView(tex, &rd, &srv);
  }
  // RT, shaders, CB.
  D3D11_TEXTURE2D_DESC rtd{};
  rtd.Width = W;
  rtd.Height = Hh;
  rtd.MipLevels = rtd.ArraySize = 1;
  rtd.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
  rtd.SampleDesc.Count = 1;
  rtd.BindFlags = D3D11_BIND_RENDER_TARGET;
  ID3D11Texture2D* rt = nullptr;
  ID3D11RenderTargetView* rtv = nullptr;
  dev->CreateTexture2D(&rtd, nullptr, &rt);
  if (rt) dev->CreateRenderTargetView(rt, nullptr, &rtv);
  HMODULE dll = LoadLibraryW(L"d3dcompiler_47.dll");
  auto compile = dll ? reinterpret_cast<decltype(&D3DCompile)>(GetProcAddress(dll, "D3DCompile")) : nullptr;
  static const char kSrc[] =
      "cbuffer cPerView : register(b7) { float4 pad[98]; float4 camPos; };\n"
      "float4 vs(uint id : SV_VertexID) : SV_Position {\n"
      "  float2 uv = float2((id << 1) & 2, id & 2);\n"
      "  return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);\n"
      "}\n"
      "float4 ps(float4 pos : SV_Position) : SV_Target { return float4(camPos.x, 1, 0, 0); }\n";
  ID3DBlob* b[2] = {};
  if (compile) {
    compile(kSrc, sizeof(kSrc) - 1, "split", nullptr, nullptr, "vs", "vs_5_0", 0, 0, &b[0], nullptr);
    compile(kSrc, sizeof(kSrc) - 1, "split", nullptr, nullptr, "ps", "ps_5_0", 0, 0, &b[1], nullptr);
  }
  ID3D11VertexShader* vs = nullptr;
  ID3D11PixelShader* ps = nullptr;
  if (b[0]) dev->CreateVertexShader(b[0]->GetBufferPointer(), b[0]->GetBufferSize(), nullptr, &vs);
  if (b[1]) dev->CreatePixelShader(b[1]->GetBufferPointer(), b[1]->GetBufferSize(), nullptr, &ps);
  for (ID3DBlob* x : b)
    if (x) x->Release();
  D3D11_BUFFER_DESC cd{};
  cd.ByteWidth = ck::kCbBytes;
  cd.Usage = D3D11_USAGE_DEFAULT;
  cd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
  alignas(16) uint8_t dcs[ck::kCbBytes];
  double R[3][3], C[3] = {0.7, 1.0, 0.2};
  Rot(0, 0, 0, R);
  const double fov[4] = {-1, 1, 1, -1};
  BuildStruct(dcs, ViewInvOf(R, C), ProjOf(fov, 0, 0), Mul(Inv(ViewInvOf(R, C)), ProjOf(fov, 0, 0)), 1);
  alignas(16) uint8_t bound[ck::kCbBytes];  // what DCS's b7 holds on the GPU
  memcpy(bound, dcs, sizeof(bound));
  if (b7True) {
    const double dwT[3] = {0.0131, 0, 0}, zero[3] = {0, 0, 0};
    ck::PatchStruct(bound, dwT, zero);
  }
  D3D11_SUBRESOURCE_DATA ci{bound, 0, 0};
  ID3D11Buffer* cb = nullptr;
  dev->CreateBuffer(&cd, &ci, &cb);
  D3D11_BLEND_DESC bd{};
  bd.RenderTarget[0].BlendEnable = TRUE;
  bd.RenderTarget[0].SrcBlend = bd.RenderTarget[0].DestBlend = D3D11_BLEND_ONE;
  bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
  bd.RenderTarget[0].SrcBlendAlpha = bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
  bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
  bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
  ID3D11BlendState* add = nullptr;
  dev->CreateBlendState(&bd, &add);
  D3D11_DEPTH_STENCIL_DESC nd = CD3D11_DEPTH_STENCIL_DESC(D3D11_DEFAULT);
  nd.DepthEnable = FALSE;
  nd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
  ID3D11DepthStencilState* noDepth = nullptr;
  dev->CreateDepthStencilState(&nd, &noDepth);
  const bool made = tex && srv && rtv && vs && ps && cb && add && noDepth && c;
  if (!made) {
    Check(msaa ? true : made, "cockpit ipd T9: split resources created");
  } else {
    // The module's state for one lighting pass of view 0 (white box).
    State saved = g_s;
    g_s = State{};
    g_s.dev = dev;
    g_s.ctx = imm;
    g_s.ctx1 = c;
    ResetCk();
    const bool oldOn = ck::g_on.load();
    ck::g_on = true;
    ck::g_cfgMode = 1;
    ck::g_known[0] = ck::Known{cb, 0, ck::kCbBytes, 1};
    ck::t_cls = ck::kClsLight;
    ck::t_view = 0;
    ck::ViewFrame& vf = ck::g_f.v[0];
    vf.patched = true;
    vf.dw[0] = 0.0131;
    vf.depthTex = tex;
    ck::g_lightCopy[0].res = cb;
    ck::g_lightCopy[0].valid = true;
    ck::g_lightCopy[0].len = ck::kCbBytes;
    ck::g_lightCopy[0].fwdPatched = b7True;
    memcpy(ck::g_lightCopy[0].bytes, dcs, ck::kCbBytes);
    // DCS's lighting state.
    const float zero[4] = {0, 0, 0, 0};
    imm->ClearRenderTargetView(rtv, zero);
    imm->OMSetRenderTargets(1, &rtv, nullptr);
    D3D11_VIEWPORT vp{0, 0, static_cast<float>(W), static_cast<float>(Hh), 0, 1};
    imm->RSSetViewports(1, &vp);
    imm->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    imm->VSSetShader(vs, nullptr, 0);
    imm->PSSetShader(ps, nullptr, 0);
    imm->VSSetConstantBuffers(7, 1, &cb);
    imm->PSSetConstantBuffers(7, 1, &cb);
    imm->PSSetShaderResources(0, 1, &srv);
    imm->OMSetBlendState(add, nullptr, 0xffffffff);
    imm->OMSetDepthStencilState(noDepth, 3);
    if (q) q->ClearStoredMessages();
    const uint64_t skip0 = ck::g_st.lightSkip.load();
    int draws = 0;
    const bool split = ck::SplitDraw(imm, 3, 0, [&] {
      ++draws;
      imm->Draw(3, 0);
    });
    if (!split) imm->Draw(3, 0);
    // State back exactly.
    ID3D11RenderTargetView* r2 = nullptr;
    ID3D11DepthStencilView* d2 = nullptr;
    imm->OMGetRenderTargets(1, &r2, &d2);
    ID3D11DepthStencilState* s2 = nullptr;
    UINT ref2 = 0;
    imm->OMGetDepthStencilState(&s2, &ref2);
    ID3D11Buffer* p7 = nullptr;
    ID3D11Buffer* v7 = nullptr;
    imm->PSGetConstantBuffers(7, 1, &p7);
    imm->VSGetConstantBuffers(7, 1, &v7);
    const bool stateBack = r2 == rtv && !d2 && s2 == noDepth && ref2 == 3 && p7 == cb && v7 == cb;
    Rel(r2), Rel(d2), Rel(s2), Rel(p7), Rel(v7);
    UINT64 msgs = 0;  // warnings, errors, corruption (INFO lines such as object creation do not count)
    if (q)
      for (UINT64 i = 0, n = q->GetNumStoredMessages(); i < n; ++i) {
        SIZE_T len = 0;
        q->GetMessage(i, nullptr, &len);
        std::vector<char> mb(len);
        auto* msg = reinterpret_cast<D3D11_MESSAGE*>(mb.data());
        if (SUCCEEDED(q->GetMessage(i, msg, &len)) && msg->Severity <= D3D11_MESSAGE_SEVERITY_WARNING) {
          ++msgs;
          printf("     debug layer: %s\n", msg->pDescription);
        }
      }
    // Read back.
    D3D11_TEXTURE2D_DESC sd = rtd;
    sd.Usage = D3D11_USAGE_STAGING;
    sd.BindFlags = 0;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Texture2D* st = nullptr;
    dev->CreateTexture2D(&sd, nullptr, &st);
    bool once = true, values = true;
    if (st) {
      imm->CopyResource(st, rt);
      D3D11_MAPPED_SUBRESOURCE m{};
      if (SUCCEEDED(imm->Map(st, 0, D3D11_MAP_READ, 0, &m))) {
        for (UINT y = 0; y < Hh; ++y) {
          const float* row = reinterpret_cast<const float*>(static_cast<const uint8_t*>(m.pData) + y * m.RowPitch);
          for (UINT x = 0; x < W; ++x) {
            if (row[x * 4 + 1] != 1.0f) once = false;
            const bool cockpit = (stv[x / 8] & 0x38) == 0x28;
            const double wantX = msaa ? C[0] : (cockpit ? C[0] + 0.0131 : C[0]);
            if (std::fabs(row[x * 4] - wantX) > 1e-5) values = false;
          }
        }
        imm->Unmap(st, 0);
      }
      st->Release();
    }
    char what[200];
    if (msaa) {
      snprintf(what, sizeof(what), "cockpit ipd T9 %s MSAA: not split (skip counted), drawn once as DCS's", fmtName);
      Check(!split && draws == 0 && ck::g_st.lightSkip.load() == skip0 + 1 && once && values && stateBack, what);
    } else {
      snprintf(what, sizeof(what),
               "cockpit ipd T9 %s: split in two draws; stencil 0x28 / 0x2f (mask 0x38) lit with the patched b7, "
               "0x10 / 0x08 with DCS's; every pixel written once; DSV, state, ref and b7 restored%s",
               fmtName, debug ? (q ? ", debug layer clean" : " (no debug layer)") : "");
      Check(split && draws == 2 && once && values && stateBack && msgs == 0, what);
    }
    ck::ReleaseLight();
    ResetCk();
    ck::g_on = oldOn;
    ck::g_cfgMode = 0;
    g_s = saved;
  }
  imm->ClearState();
  Rel(q), Rel(srv), Rel(tex), Rel(rtv), Rel(rt), Rel(vs), Rel(ps), Rel(cb), Rel(add), Rel(noDepth), Rel(c);
  imm->Release();
  dev->Release();
}

// ---------------------------------------------------------------------------
// T10 review fixes: dry latches, restore merge, ambiguous offsets, forward
// pass failure, fault path (white box, CPU)
// ---------------------------------------------------------------------------

void TestReviewFixes() {
  // Dry: the layer stops writing at 1.4, the last record stays in the block: carried for kCarryMax latches
  // beyond the lag (one-off gaps are early / late records, live 2026-10-13), then h 0, never verified.
  {
    ck::Matcher m;
    for (uint32_t n = 1; n <= 5; ++n) {
      m.Push(MakeRec(n, 1.4), n);
      m.Latch(n, nullptr, nullptr);
    }
    const auto d1 = m.Latch(6, nullptr, nullptr);  // lag 0: no new record -> carried (the newest stands in)
    m.Verify(0.0655, d1);                            // a carried-late latch is never verified
    const auto d2 = m.Latch(7, nullptr, nullptr);
    const auto d3 = m.Latch(8, nullptr, nullptr);  // beyond kCarryMax: dry
    m.Verify(0.0655, d3);
    m.Push(MakeRec(9, 1.4), 9);  // records again
    const auto back = m.Latch(9, nullptr, nullptr);
    ck::Matcher l1;  // lag 1 learned: one more latch covered by the ring
    l1.lag = 1;
    for (uint32_t n = 1; n <= 3; ++n) {
      l1.Push(MakeRec(n, 1.4), n);
      l1.Latch(n, nullptr, nullptr);
    }
    const auto c1 = l1.Latch(4, nullptr, nullptr);
    const auto c2 = l1.Latch(5, nullptr, nullptr);
    const auto c3 = l1.Latch(6, nullptr, nullptr);
    const auto c4 = l1.Latch(7, nullptr, nullptr);
    Check(!d1.stale && d1.carried && d1.h > 0.013 && !d2.stale && d2.carried && d3.dry && d3.stale && d3.h == 0 &&
              !m.fault && m.verifies == 0 && !back.stale && back.h > 0.013 && !c1.stale && c1.h > 0 && !c3.stale &&
              c3.h > 0 && !c2.stale && c4.dry && c4.h == 0,
          "cockpit ipd T10: no new record since the last latch: carried (h kept, not verified) for kCarryMax "
          "latches beyond the lag, then h 0 (dry), no pairFault; records again -> h back");
  }
  ResetCk();
  const bool oldOn = ck::g_on.load();
  ck::g_on = true;
  ck::g_cfgMode = 1;
  // No context: the fake resources below never reach D3D (restore, LearnBound, BoundBase).
  ID3D11DeviceContext* const oldCtx = fixdepth::g_s.ctx;
  ID3D11DeviceContext1* const oldCtx1 = fixdepth::g_s.ctx1;
  fixdepth::g_s.ctx = nullptr;
  fixdepth::g_s.ctx1 = nullptr;
  const bool oldFdFault = fixdepth::g_fault.load();
  fixdepth::g_fault = false;
  ck::g_fault = false;
  ID3D11Resource* const rA = reinterpret_cast<ID3D11Resource*>(static_cast<uintptr_t>(0x1000));
  ID3D11Resource* const rC = reinterpret_cast<ID3D11Resource*>(static_cast<uintptr_t>(0x4000));
  double R[3][3], C[3] = {0.5, 1.2, -0.2};
  Rot(10, 0, 0, R);
  const double fov[4] = {-1, 1, 1, -1};
  const Mat P = ProjOf(fov, 0.001, 0);
  std::vector<uint8_t> a(ck::kCbBytes), b(ck::kCbBytes);
  BuildStruct(a.data(), ViewInvOf(R, C), P, Mul(Inv(ViewInvOf(R, C)), P), 5);
  memcpy(b.data(), a.data(), ck::kCbBytes);
  for (int i = 0; i < 64; ++i) b[0x800 + i] = static_cast<uint8_t>(0xA0 + i);  // DCS's newer bytes
  auto setup = [&](ck::Cls cls) {
    ck::g_f = ck::Frame{};
    ck::g_f.latched = true;
    ck::g_f.h = 0.0131;
    ck::g_known[0] = ck::Known{rA, 0, ck::kCbBytes, 1};
    ck::g_known[1] = ck::Known{rC, 0, 0x2000, 1};
    ck::g_known[2] = ck::Known{rC, 0x1000, 0x2000, 1};
    ck::t_cls = cls;
    ck::t_view = 0;
    ck::t_uploads = ck::t_patchedN = 0;
    ck::g_restN = 0;
  };
  // Restore merge: a patched upload (A), then DCS's partial upload (B) forwarded as is.
  setup(ck::kClsGb);
  const void* p1 = ck::OnUpload(rA, 0, nullptr, a.data(), false);
  const D3D11_BOX part{0x800, 0, 0, 0x840, 1, 1};
  const void* p2 = ck::OnUpload(rA, 0, &part, b.data() + 0x800, false);
  const bool mergeOk = p1 == ck::g_scratch && p2 == b.data() + 0x800 && ck::g_restN == 1 &&
                       !memcmp(ck::g_rest[0].bytes, b.data(), ck::kCbBytes);
  // A full unpatched upload after the patched one: the entry goes (the buffer holds DCS's bytes).
  setup(ck::kClsGb);
  ck::OnUpload(rA, 0, nullptr, a.data(), false);
  uint8_t badS[ck::kCbBytes];
  memcpy(badS, a.data(), sizeof(badS));
  float f2 = 2.0f;
  memcpy(badS + ck::kOffViewInv, &f2, 4);
  const bool dropOk = ck::OnUpload(rA, 0, nullptr, badS, false) == badS && ck::g_restN == 0;
  Check(mergeOk && dropOk, "cockpit ipd T10: a later upload forwarded as DCS's is merged into the restore (partial) "
                           "or drops it (whole): the restore never writes older bytes");
  // Two learned structs in one buffer: a null box is ambiguous without b7 (no context): DCS's pointer;
  // a box around one of them patches that one.
  setup(ck::kClsGb);
  std::vector<uint8_t> big(0x2000);
  memcpy(big.data() + 0x1000, a.data(), ck::kCbBytes);
  const uint64_t amb = ck::g_st.ambiguous.load();
  const bool ambOk = ck::OnUpload(rC, 0, nullptr, big.data(), false) == big.data() &&
                     ck::g_st.ambiguous.load() == amb + 1 && ck::g_restN == 0;
  setup(ck::kClsGb);
  const D3D11_BOX second{0x1000, 0, 0, 0x1000 + ck::kCbBytes, 1, 1};
  const void* p3 = ck::OnUpload(rC, 0, &second, big.data() + 0x1000, false);
  double c3[3];
  CamOf(static_cast<const uint8_t*>(p3), c3);
  const double moved = (c3[0] - C[0]) * R[0][0] + (c3[1] - C[1]) * R[0][1] + (c3[2] - C[2]) * R[0][2];
  Check(ambOk && p3 == ck::g_scratch && std::fabs(moved - 0.0131) < 1e-6 && ck::g_rest[0].lo == 0x1000,
        "cockpit ipd T10: several learned structs in one buffer -> the one the box covers; undecided -> DCS's pointer");
  // Forward cockpit pass: not armed / an upload forwarded unpatched -> the next frame off, counted.
  auto fwd = [&]() {
    setup(ck::kClsFwd);
    ck::ViewFrame& vf = ck::g_f.v[0];
    vf.patched = vf.dwReady = vf.done = true;
    vf.dw[0] = 0.0131;
    ck::g_f.anyPatched = true;
    ck::g_offNext = false;
    ck::t_pass = reinterpret_cast<void*>(static_cast<uintptr_t>(0x77));
  };
  fwd();
  ck::PassArmed(false);
  const uint64_t ff = ck::g_st.fwdFail.load(), rNo = ck::g_st.fwdFailNoArm.load(),
                 rLost = ck::g_st.fwdFailArmLost.load(), rUnp = ck::g_st.fwdFailUnpatched.load();
  ck::PassEnd(ck::t_pass, true);
  const bool noArm = ck::g_offNext && ck::g_f.anyFailed && ck::g_st.fwdFail.load() == ff + 1;
  fwd();
  ck::PassArmed(true);
  ck::PassEnd(ck::t_pass, false);  // a table switch took the arm before any upload
  const bool lost = ck::g_offNext && ck::g_st.fwdFail.load() == ff + 2;
  fwd();
  ck::PassArmed(true);
  ck::OnUpload(rA, 0, &part, a.data() + 0x800, false);  // known buffer, struct not covered: forwarded
  ck::PassEnd(ck::t_pass, true);
  const bool unp = ck::g_offNext && ck::g_st.fwdFail.load() == ff + 3;
  fwd();
  ck::PassArmed(true);
  const bool patchedFwd = ck::OnUpload(rA, 0, nullptr, a.data(), false) == ck::g_scratch;
  ck::PassEnd(ck::t_pass, false);  // d3d11 switched the entry after the patched upload: no failure
  const bool good = patchedFwd && !ck::g_offNext && ck::g_st.fwdFail.load() == ff + 3 && ck::t_cls == ck::kClsNone;
  Check(noArm && lost && unp && good, "cockpit ipd T10: forward cockpit pass not armed, arm lost or an upload "
                                      "unpatched -> next frame off (counted); all patched (arm lost after it) -> no failure");
  // Each failure under its own reason (the live log had all 17 under "not armed": the reason was read after
  // PassEnd cleared the begin's verdict).
  Check(ck::g_st.fwdFailNoArm.load() == rNo + 1 && ck::g_st.fwdFailArmLost.load() == rLost + 1 &&
            ck::g_st.fwdFailUnpatched.load() == rUnp + 1,
        "cockpit ipd T10: forward pass failure reasons counted apart: not armed 1, arm lost 1, unpatched 1");
  // Fault: the upload hook forwards DCS's bytes (merging into the open pass's restore), the pass still closes.
  setup(ck::kClsFwd);
  ck::t_pass = reinterpret_cast<void*>(static_cast<uintptr_t>(0x77));
  ck::OnUpload(rA, 0, nullptr, a.data(), false);  // patched before the fault
  ck::g_fault = true;
  const bool inactive = !ck::Active() && !ck::WantUploads() && ck::PassOpen();
  const void* pf = ck::OnUploadSeh(rA, 0, &part, b.data() + 0x800, false);
  const bool fwdDcs = pf == b.data() + 0x800 && ck::g_restN == 1 && !memcmp(ck::g_rest[0].bytes, b.data(), ck::kCbBytes);
  ck::PassEnd(ck::t_pass, true);  // no context in the test: the restore entries are consumed without an upload
  const bool closed = ck::t_cls == ck::kClsNone && ck::g_restN == 0 && !ck::PassOpen();
  ck::t_cls = ck::kClsLight;
  ck::t_view = 0;
  const bool noSplit = !ck::SplitDraw(nullptr, 3, 0, [] {});
  ck::g_fault = false;
  Check(inactive && fwdDcs && closed && noSplit,
        "cockpit ipd T10: after a fault nothing is patched or split, DCS's later bytes still reach the restore, the "
        "open pass is closed");
  // An upload on DCS's context from another thread: forwarded untouched (cockpit ipd's per-pass state belongs to the
  // render thread), counted.
  {
    setup(ck::kClsGb);
    const DWORD oldTid = ptiming::g_topTid.load();
    ptiming::g_topTid = GetCurrentThreadId() + 1;
    const uint64_t ot0 = ck::g_st.usOtherThread.load();
    const void* fw = fixdepth::CkUpload(rA, 0, nullptr, a.data(), false);
    ptiming::g_topTid = GetCurrentThreadId();
    const void* fr = fixdepth::CkUpload(rA, 0, nullptr, a.data(), false);
    ptiming::g_topTid = oldTid;
    Check(fw == a.data() && ck::g_st.usOtherThread.load() == ot0 + 1 && fr == ck::g_scratch,
          "cockpit ipd T10: an upload from another thread on DCS's context is forwarded untouched (counted); the "
          "render thread's is patched");
    ck::g_restN = 0;
  }
  ResetCk();
  ck::g_restN = 0;
  ck::g_on = oldOn;
  ck::g_cfgMode = 0;
  fixdepth::g_s.ctx = oldCtx;
  fixdepth::g_s.ctx1 = oldCtx1;
  fixdepth::g_fault = oldFdFault;
}

// StereoAck (Local\DcsVrCockpitStereo_v1, engine to layer): the header's
// layout, the flag composition, the writer through a real mapping read back
// with the seqlock as the layer does, flags 0 once at Stop, nothing created
// when CockpitTrueIpd=0; and the dual camera status word it feeds.
void TestStereoAck() {
  namespace psh = ::dcsvr_perception;
  static_assert(sizeof(psh::CockpitStereo) == 32, "CockpitStereo 32 bytes");
  Check(offsetof(psh::CockpitStereo, seq) == 0 && offsetof(psh::CockpitStereo, flags) == 8 &&
            offsetof(psh::CockpitStereo, reserved) == 12 && offsetof(psh::CockpitStereo, engineFrame) == 16 &&
            offsetof(psh::CockpitStereo, qpc) == 24 && ck::kStereoActive == 1u && ck::kStereoObserve == 2u &&
            wcscmp(psh::cockpit_stereo_block_name, L"Local\\DcsVrCockpitStereo_v1") == 0,
        "cockpit ipd StereoAck: CockpitStereo layout (32 bytes), flags active 1 / observing 2, name as the header");
  const auto a = ck::StereoAckCompose(true, false, false, 7, 99);
  const auto p = ck::StereoAckCompose(true, false, true, 8, 100);
  const auto o = ck::StereoAckCompose(true, true, false, 9, 101);
  const auto n = ck::StereoAckCompose(false, false, false, 10, 102);
  Check(a.flags == ck::kStereoActive && a.engineFrame == 7 && a.qpc == 99 && p.flags == 0 &&
            o.flags == ck::kStereoObserve && n.flags == 0,
        "cockpit ipd StereoAck: bit 0 only when active, not paused and not observing; bit 1 observe only");

  // The suffix variable names the block, as BoostOut.
  char suffix[48];
  snprintf(suffix, sizeof(suffix), "_cktestack%lu", GetCurrentProcessId());
  char oldSuf[64] = {};
  const DWORD hadSuf = GetEnvironmentVariableA(psh::name_suffix_variable, oldSuf, sizeof(oldSuf));
  SetEnvironmentVariableA(psh::name_suffix_variable, suffix);
  wchar_t want[96];
  swprintf_s(want, L"Local\\DcsVrCockpitStereo_v1_cktestack%lu", GetCurrentProcessId());
  ck::g_ackNameOverride.clear();
  Check(ck::AckName() == want, "cockpit ipd StereoAck: name = Local\\DcsVrCockpitStereo_v1 + the suffix variable");

  // Saved state.
  const bool oldOn = ck::g_on.load(), oldFault = ck::g_fault.load(), oldFdFault = fixdepth::g_fault.load(),
             oldUserOff = ck::g_userOff.load(), oldForeign = ck::g_cfgForeign.load();
  const int oldMode = ck::g_cfgMode.load();
  void* const oldBoost = ck::g_boostView.load();
  ResetCk();
  ck::CloseAck(0);
  ck::g_fault = false;
  fixdepth::g_fault = false;
  ck::g_userOff = false;
  ck::g_cfgForeign = false;
  const bool oldAssume = ck::g_ackAssumeDevice;
  ck::g_ackAssumeDevice = true;  // R63: the ack needs a learnt device; this test has no D3D11 context

  // Off (CockpitTrueIpd=0): Start refuses, nothing is created or written.
  ck::g_on = false;
  ConfigureD3d11(0, false, true, 0, 0, false);
  const bool offStart = ck::Start();
  HANDLE probe = OpenFileMappingW(FILE_MAP_READ, FALSE, want);
  const bool offNone = !offStart && !probe && ck::g_ackView.load() == nullptr;
  if (probe) CloseHandle(probe);
  ck::g_on = true;  // even a stray frame end with the option off writes nothing
  ck::AckFrame(ck::g_f);
  probe = OpenFileMappingW(FILE_MAP_READ, FALSE, want);
  Check(offNone && !probe && ck::DualCameraStatusNow() == 0,
        "cockpit ipd StereoAck: CockpitTrueIpd=0 creates no block, writes nothing, status word 0");
  if (probe) CloseHandle(probe);

  // On: the block opens (as Start does), the layer's reader maps it.
  ck::g_cfgMode = 1;
  const uint64_t opens0 = ck::g_ackOpens.load();
  ck::OpenAck(true);
  HANDLE rm = OpenFileMappingW(FILE_MAP_READ, FALSE, want);
  const auto* rv = rm ? static_cast<const psh::CockpitStereo*>(
                            MapViewOfFile(rm, FILE_MAP_READ, 0, 0, sizeof(psh::CockpitStereo)))
                      : nullptr;
  Check(ck::g_ackView.load() != nullptr && rv && ck::g_ackOpens.load() == opens0 + 1,
        "cockpit ipd StereoAck: CockpitTrueIpd=1 creates the block (the layer opens it by name)");

  // A fresh BoostOut record (x1.40 applied) at this frame.
  alignas(8) static unsigned char fakeBoost[64];
  ck::g_boostView = fakeBoost;  // only its presence is read here
  auto push = [&](float scale, uint32_t fr) {
    ck::Rec r;
    r.displayTime = 1000 + fr;
    r.frame = fr;
    r.flags = psh::boost_flags::enabled | psh::boost_flags::applied;
    r.scale = scale;
    r.base = 0.064f;
    r.rendered = 0.064f * scale;
    ck::g_match.Push(r, ck::g_frame);
  };
  psh::CockpitStereo got{};
  uint64_t lastSeq = 0;
  auto frameEnd = [&](bool patched) {
    ck::g_f = ck::Frame{};
    ck::g_f.anyPatched = patched;
    ck::g_f.observe = ck::Observe();
    const uint64_t fr = ck::g_frame;
    const int64_t q0 = ck::QpcNow();
    ck::EndFrame();
    const bool r = rv && psh::seqlock_read(rv, got);
    const bool ok = r && got.engineFrame == fr && got.qpc >= q0 && got.qpc <= ck::QpcNow() && got.reserved == 0 &&
                    (got.seq & 1) == 0 && got.seq > lastSeq;
    lastSeq = got.seq;
    return ok;
  };
  uint32_t recFrame = 1;
  push(1.4f, recFrame++);
  const uint64_t writes0 = ck::g_ackWrites.load();
  const bool rOn = frameEnd(true);
  const uint32_t fOn = got.flags;
  const uint64_t seqOn = got.seq;
  push(1.4f, recFrame++);
  const bool rOn2 = frameEnd(true);
  Check(rOn && rOn2 && fOn == ck::kStereoActive && got.flags == ck::kStereoActive && got.seq == seqOn + 2 &&
            ck::g_ackWrites.load() == writes0 + 2,
        "cockpit ipd StereoAck: on: active, one seqlock write per frame end (even seq, +2), engineFrame = our "
        "frame, qpc now");
  // The status word on: configured, ON, not waiting, scale 140, not observe.
  const uint64_t wOn = ck::DualCameraStatusNow();
  Check(qvstatus::DcConfigured(wOn) && qvstatus::DcOn(wOn) && !qvstatus::DcPaused(wOn) &&
            !qvstatus::DcWaiting(wOn) && !qvstatus::DcObserve(wOn) && qvstatus::DcScaleX100(wOn) == 140 &&
            (wOn >> 32) == 0,
        "cockpit ipd dual status: on: configured, ON, scale 140, not waiting");

  // Paused by the key: active cleared (still written each frame).
  ck::g_userOff = true;
  push(1.4f, recFrame++);
  const bool rP = frameEnd(false);
  const uint64_t wP = ck::DualCameraStatusNow();
  Check(rP && got.flags == 0 && qvstatus::DcPaused(wP) && !qvstatus::DcOn(wP) && qvstatus::DcConfigured(wP),
        "cockpit ipd StereoAck: paused (Alt+Shift+F7): active cleared; status paused, not ON");
  ck::g_userOff = false;

  // Scale 1.0 (boost not applied): still active (the cockpit is at the true eye whatever the scale); status waiting.
  push(1.0f, recFrame++);
  const bool r1 = frameEnd(false);
  const uint64_t w1 = ck::DualCameraStatusNow();
  Check(r1 && got.flags == ck::kStereoActive && qvstatus::DcWaiting(w1) && qvstatus::DcScaleX100(w1) == 100,
        "cockpit ipd StereoAck: scale 1: still active; status waiting, scale 100");

  // Stale: no new record for kStaleFrames frames -> active held for up to g_ackHoldMs (hysteresis), then cleared.
  push(1.4f, recFrame++);
  ck::g_frame += ck::Matcher::kStaleFrames;
  const uint64_t held0 = ck::g_st.ackHeld.load(), drops0 = ck::g_st.ackDrops.load(),
                 dropB0 = ck::g_st.ackDropBoost.load();
  const bool rSh = frameEnd(true);
  const uint32_t fSh = got.flags;
  const uint64_t wSh = ck::DualCameraStatusNow();
  const bool heldOk = rSh && fSh == ck::kStereoActive && ck::g_st.ackHeld.load() == held0 + 1 &&
                      ck::g_st.ackDrops.load() == drops0 && qvstatus::DcWaiting(wSh);
  Sleep(static_cast<DWORD>(ck::g_ackHoldMs.load() + 40));  // still stale, now for longer than the hold
  const bool rS = frameEnd(true);
  const uint64_t wS = ck::DualCameraStatusNow();
  Check(heldOk && rS && got.flags == 0 && ck::g_st.ackDrops.load() == drops0 + 1 &&
            ck::g_st.ackDropBoost.load() == dropB0 + 1 && qvstatus::DcWaiting(wS) && qvstatus::DcScaleX100(wS) == 0,
        "cockpit ipd StereoAck: BoostOut stale: active held within 150 ms (status already waiting), cleared once "
        "stale for longer (drop counted as BoostOut); status waiting, scale 0");
  // Dry: records stopped at the latches -> active cleared.
  push(1.4f, recFrame++);
  ck::g_match.dry = ck::g_match.lag + ck::Matcher::kCarryMax + 1;
  const bool rD = frameEnd(true);
  Check(rD && got.flags == 0, "cockpit ipd StereoAck: BoostOut dry: active cleared");
  ck::g_match.dry = 0;
  // No BoostOut mapped -> active cleared.
  ck::g_boostView = nullptr;
  push(1.4f, recFrame++);
  const bool rN = frameEnd(true);
  Check(rN && got.flags == 0 && qvstatus::DcWaiting(ck::DualCameraStatusNow()),
        "cockpit ipd StereoAck: no BoostOut: active cleared; status waiting");
  ck::g_boostView = fakeBoost;
  // Faulted or inert -> active cleared.
  ck::g_fault = true;
  push(1.4f, recFrame++);
  const bool rF = frameEnd(true);
  const uint32_t fF = got.flags;
  const uint64_t wF = ck::DualCameraStatusNow();
  ck::g_fault = false;
  ck::g_inertSelf = true;
  push(1.4f, recFrame++);
  const bool rI = frameEnd(true);
  ck::g_inertSelf = false;
  Check(rF && fF == 0 && !qvstatus::DcOn(wF) && rI && got.flags == 0,
        "cockpit ipd StereoAck: faulted or inert: active cleared (status not ON)");
  // R77 ckipd-toobig-ack-active: a frame past the shift limit (not patched) takes the ack down for the hold.
  {
    push(1.4f, recFrame++);
    const bool rA = frameEnd(true) && got.flags == ck::kStereoActive;
    ck::g_tooBigAt = static_cast<int64_t>(ck::g_frame);
    push(1.4f, recFrame++);
    const bool rB = frameEnd(true) && got.flags == 0;
    ck::g_frame += ck::kTooBigHoldFrames;
    push(1.4f, recFrame++);
    const bool rC = frameEnd(true) && got.flags == ck::kStereoActive;
    ck::g_tooBigAt = -1;
    Check(rA && rB && rC && ck::kMaxShiftM > (2.0 - 1.0) * 0.090 / 2,
          "R77: cockpit ipd StereoAck: a per-eye shift past the limit (above the layer's 45 mm maximum) clears active "
          "for the hold, then active again");
  }

  // Observe (CockpitTrueIpd=2): observing only.
  ck::g_cfgMode = 2;
  push(1.4f, recFrame++);
  const bool rO = frameEnd(false);
  const uint64_t wO = ck::DualCameraStatusNow();
  Check(rO && got.flags == ck::kStereoObserve && qvstatus::DcObserve(wO) && !qvstatus::DcOn(wO) &&
            qvstatus::DcConfigured(wO),
        "cockpit ipd StereoAck: observe: flags = observing only; status observe, not ON");
  ck::g_cfgMode = 1;
  push(1.4f, recFrame++);
  const bool rBack = frameEnd(true);
  Check(rBack && got.flags == ck::kStereoActive, "cockpit ipd StereoAck: back on: active again");

  // R62 e3: ON from the last second (a gated last frame no longer reads "no dual camera"); no cockpit pass drawn
  // (external view) is bit 5, not a failure; a cockpit drawn and never patched still shows as not ON.
  {
    auto end2 = [&](bool patched, bool cockpit) {
      ck::g_f = ck::Frame{};
      ck::g_f.anyPatched = patched;
      ck::g_f.latched = cockpit;
      ck::g_f.observe = ck::Observe();
      ck::EndFrame();
    };
    ck::DualCameraStatusNow();  // the per-second window starts here
    push(1.4f, recFrame++);
    end2(true, true);
    push(1.4f, recFrame++);
    end2(false, true);  // the sampled frame was gated (camera cut, gPrev check)
    // R107: 20 ms, not 5: the per-second rate divides by GetTickCount64's delta (15.6 ms steps), which a 5 ms sleep
    // left at 0 now and then (rate 0: "not ON"; one failure in R107's step runs). Still well inside the 200 ms window.
    Sleep(20);
    const uint64_t wGate = ck::DualCameraStatusNow();
    push(1.4f, recFrame++);
    end2(false, false);
    Sleep(5);
    ck::DualCameraStatusNow();  // a second with only external-view frames follows
    push(1.4f, recFrame++);
    end2(false, false);
    push(1.4f, recFrame++);
    end2(false, false);
    Sleep(5);
    const uint64_t wExt = ck::DualCameraStatusNow();
    push(1.4f, recFrame++);
    end2(false, true);
    Sleep(5);
    const uint64_t wFail = ck::DualCameraStatusNow();
    Check(qvstatus::DcOn(wGate) && !qvstatus::DcNoCockpit(wGate),
          "cockpit ipd dual status (R62): patched in the last second, last frame gated -> still ON");
    Check(!qvstatus::DcOn(wExt) && qvstatus::DcNoCockpit(wExt) && !qvstatus::DcWaiting(wExt) &&
              qvstatus::DcScaleX100(wExt) == 140,
          "cockpit ipd dual status (R62): external view (no cockpit pass) -> no-cockpit bit, not a failure");
    Check(!qvstatus::DcOn(wFail) && !qvstatus::DcNoCockpit(wFail),
          "cockpit ipd dual status (R62): cockpit drawn but not patched -> not ON, no no-cockpit bit");
    Check(qvstatus::PackDualCamera(true, true, false, false, false, 1.4, 90, true) ==
              qvstatus::PackDualCamera(true, true, false, false, false, 1.4, 90) &&
              qvstatus::PackDualCamera(true, false, false, false, false, 1.4, 0, true) ==
              (1ull | 32ull | (140ull << 8)),
          "dual status (R62): bit 5 no cockpit, never together with ON");
  }

  // Hysteresis (live 2026-10-13: "BoostOut 9" drops from transient staleness): a gap of a frame or two
  // (stale, then dry) keeps the ack active and is bridged when records come again; no drop.
  {
    const uint64_t br0 = ck::g_st.ackBridged.load(), dr0 = ck::g_st.ackDrops.load(), hd0 = ck::g_st.ackHeld.load();
    push(1.4f, recFrame++);
    ck::g_frame += ck::Matcher::kStaleFrames;  // stale for this frame end
    const bool g1 = frameEnd(true) && got.flags == ck::kStereoActive;
    push(1.4f, recFrame++);
    ck::g_match.dry = ck::g_match.lag + ck::Matcher::kCarryMax + 1;  // then dry for one
    const bool g2 = frameEnd(true) && got.flags == ck::kStereoActive;
    ck::g_match.dry = 0;
    push(1.4f, recFrame++);  // fresh again
    const bool g3 = frameEnd(true) && got.flags == ck::kStereoActive;
    Check(g1 && g2 && g3 && ck::g_st.ackHeld.load() == hd0 + 2 && ck::g_st.ackBridged.load() == br0 + 1 &&
              ck::g_st.ackDrops.load() == dr0,
          "cockpit ipd StereoAck hysteresis: a 2-frame BoostOut gap (stale, then dry) keeps the ack active (held 2, "
          "bridged 1, no drop)");
    // The key pause during a gap clears it at once (counted as paused).
    const uint64_t dp0 = ck::g_st.ackDropPause.load();
    push(1.4f, recFrame++);
    ck::g_frame += ck::Matcher::kStaleFrames;
    const bool p1 = frameEnd(true) && got.flags == ck::kStereoActive;
    ck::g_userOff = true;
    const bool p2 = frameEnd(false) && got.flags == 0;
    ck::g_userOff = false;
    // Released while BoostOut is still stale: stays inactive (nothing to hold), active again with a fresh record.
    const bool p3 = frameEnd(false) && got.flags == 0;
    push(1.4f, recFrame++);
    const bool p4 = frameEnd(true) && got.flags == ck::kStereoActive;
    // Health clears at once too (no hold).
    ck::g_inertSelf = true;
    push(1.4f, recFrame++);
    const bool h1 = frameEnd(true) && got.flags == 0;
    ck::g_inertSelf = false;
    push(1.4f, recFrame++);
    const bool h2 = frameEnd(true) && got.flags == ck::kStereoActive;
    Check(p1 && p2 && p3 && p4 && h1 && h2 && ck::g_st.ackDropPause.load() == dp0 + 1,
          "cockpit ipd StereoAck hysteresis: the in-flight key clears a held ack at once; an inactive ack is not "
          "held (inactive until BoostOut is fresh); inert clears at once");
  }
  // The hold itself (pure): 150 ticks.
  {
    ck::AckHold h;
    bool held = false, br = false;
    const bool a0 = ck::AckBoostUsable(h, true, true, 0, 150, &held, &br) && !held && !br;
    const bool a1 = ck::AckBoostUsable(h, false, true, 1000, 150, &held, &br) && held;          // gap starts
    const bool a2 = ck::AckBoostUsable(h, false, true, 1150, 150, &held, &br) && held;          // 150: still held
    const bool a3 = !ck::AckBoostUsable(h, false, true, 1151, 150, &held, &br) && !held;        // > 150: not
    const bool a4 = !ck::AckBoostUsable(h, false, false, 1200, 150, &held, &br) && !held;       // inactive: never
    const bool a5 = ck::AckBoostUsable(h, true, false, 1300, 150, &held, &br) && !br;           // fresh again
    const bool a6 = ck::AckBoostUsable(h, false, true, 2000, 150, &held, &br) && held;          // a new gap
    const bool a7 = ck::AckBoostUsable(h, true, true, 2100, 150, &held, &br) && br;             // bridged
    const bool a8 = ck::AckBoostUsable(h, false, true, 5000, 150, &held, &br) && held;          // restarts at 5000
    Check(a0 && a1 && a2 && a3 && a4 && a5 && a6 && a7 && a8,
          "cockpit ipd StereoAck hysteresis (pure): held while the gap lasts <= the hold, then not; an inactive "
          "ack is never held; a recovered gap is bridged and the next gap starts its own window");
  }

  // R63 stereo-ack-active-without-any-patch: active only with evidence that the engine can move the cockpit.
  {
    auto endWith = [&](bool latched, int ckTotal, bool patched) {
      ck::g_f = ck::Frame{};
      ck::g_f.latched = latched;
      ck::g_f.ckTotal = ckTotal;
      ck::g_f.anyPatched = patched;
      ck::g_f.observe = ck::Observe();
      ck::EndFrame();
      return rv && psh::seqlock_read(rv, got);
    };
    push(1.4f, recFrame++);
    const bool e0 = endWith(true, 1, true) && got.flags == ck::kStereoActive;
    // No D3D11 device (perception_device.h gave up on a wrapped context): never active, whatever else holds.
    ID3D11DeviceContext* const ctx0 = fixdepth::g_s.ctx;
    fixdepth::g_s.ctx = nullptr;
    ck::g_ackAssumeDevice = false;
    bool noDev = true;
    for (int i = 0; i < 5; ++i) {
      push(1.4f, recFrame++);
      noDev = noDev && endWith(false, 0, false) && got.flags == 0;
    }
    Check(e0 && noDev && ck::g_ackNoEvidence,
          "R63 StereoAck: no D3D11 device learnt -> flags 0 (the layer keeps its own cockpit steps), BoostOut fresh");
    ck::g_ackAssumeDevice = true;
    fixdepth::g_s.ctx = ctx0;
    push(1.4f, recFrame++);
    const bool back = endWith(true, 1, false) && got.flags == ck::kStereoActive;
    // Scale 1 (latched, nothing to patch): active (the layer raises the scale only on an active ack).
    bool s1 = true;
    for (int i = 0; i < 60; ++i) {
      push(1.0f, recFrame++);
      s1 = s1 && endWith(true, 1, false) && got.flags == ck::kStereoActive;
    }
    Check(back && s1, "R63 StereoAck: device back -> active; at scale 1 (latched, nothing patched) stays active");
    // External view (no cockpit pass, every execution identified): stays active.
    bool ext = true;
    for (int i = 0; i < 100; ++i) {
      push(1.4f, recFrame++);
      ck::g_st.gbExecs += 3;
      ext = ext && endWith(false, 0, false) && got.flags == ck::kStereoActive;
    }
    Check(ext, "R63 StereoAck: external view (no cockpit pass) for 100 frames: still active");
    // Cockpit drawn in every frame but view 0's upload never intercepted: inactive after kAckMissFrames.
    bool before = true, after = false;
    for (int i = 0; i < ck::kAckMissFrames; ++i) {
      push(1.4f, recFrame++);
      const bool r = endWith(false, 2, false);
      if (i < ck::kAckMissFrames - 1) before = before && r && got.flags == ck::kStereoActive;
      else after = r && got.flags == 0;
    }
    push(1.4f, recFrame++);
    const bool relatch = endWith(true, 2, false) && got.flags == ck::kStereoActive;
    Check(before && after && relatch,
          "R63 StereoAck: cockpit drawn, never intercepted: active for 44 frames, inactive at 45, active once a "
          "frame latches view 0 again");
    // No G-buffer execution identified at all (a DCS update broke the identity): inactive likewise.
    bool idf = true;
    for (int i = 0; i < ck::kAckMissFrames; ++i) {
      push(1.4f, recFrame++);
      ck::g_st.gbExecs += 4;
      ck::g_st.idFail += 4;
      idf = endWith(false, 0, false) && got.flags == (i < ck::kAckMissFrames - 1 ? ck::kStereoActive : 0u);
      if (!idf) break;
    }
    push(1.4f, recFrame++);
    const bool relatch2 = endWith(true, 1, true) && got.flags == ck::kStereoActive;
    Check(idf && relatch2, "R63 StereoAck: no G-buffer execution identified for 45 frames -> inactive; back on a latch");
  }

  // Stop: flags 0 once, then unmapped (our view; the reader's copy keeps the last write).
  const uint64_t seqBefore = got.seq;
  ck::g_boostView = nullptr;  // fake: not ours to unmap
  ck::Stop();
  const bool rStop = rv && psh::seqlock_read(rv, got);
  Check(rStop && got.flags == 0 && got.seq == seqBefore + 2 && ck::g_ackView.load() == nullptr,
        "cockpit ipd StereoAck: Stop writes flags 0 once, then unmaps");
  if (rv) UnmapViewOfFile(rv);
  if (rm) CloseHandle(rm);
  probe = OpenFileMappingW(FILE_MAP_READ, FALSE, want);
  Check(!probe, "cockpit ipd StereoAck: after Stop and the reader's close the block is gone");
  if (probe) CloseHandle(probe);

  // Restore.
  ck::g_on = oldOn;
  ck::g_fault = oldFault;
  fixdepth::g_fault = oldFdFault;
  ck::g_userOff = oldUserOff;
  ck::g_cfgForeign = oldForeign;
  ck::g_cfgMode = oldMode;
  ck::g_boostView = oldBoost;
  ck::g_ackAssumeDevice = oldAssume;
  SetEnvironmentVariableA(psh::name_suffix_variable, hadSuf ? oldSuf : nullptr);
  ResetCk();
}

// DcsQvCull_DualCameraStatus's bit composition (pure) and the loader-owned
// word's handover (0 with no payload attached, 0 again after its Stop).
void TestDualCameraStatus() {
  using namespace qvstatus;
  Check(PackDualCamera(false, true, true, true, true, 1.4, 90) == 0, "dual status: not configured packs to 0");
  const uint64_t w = PackDualCamera(true, true, false, false, false, 1.4, 90);
  Check(w == (1ull | 2ull | (140ull << 8) | (90ull << 16)), "dual status: configured, ON, scale 140, 90 frames");
  Check(PackDualCamera(true, false, true, false, false, 1.0, 0) == (1ull | 4ull | (100ull << 8)),
        "dual status: paused in bit 2");
  Check(PackDualCamera(true, false, false, true, false, 0.0, 0) == (1ull | 8ull),
        "dual status: waiting in bit 3, scale 0");
  Check(PackDualCamera(true, false, false, false, true, 1.25, 0) == (1ull | 16ull | (125ull << 8)),
        "dual status: observe in bit 4");
  const uint64_t big = PackDualCamera(true, true, true, true, true, 9.0, 1000000);
  Check(DcScaleX100(big) == 255 && DcFramesPerSecond(big) == 65535 && (big >> 32) == 0 && (big & 0xe0) == 0,
        "dual status: scale clamps to 255, frames to 65535, nothing above bit 31 or in bits 5-7");
  Check(DcScaleX100(PackDualCamera(true, false, false, false, false, std::nan(""), 0)) == 0 &&
            DcScaleX100(PackDualCamera(true, false, false, false, false, -1.0, 0)) == 0 &&
            DcScaleX100(PackDualCamera(true, false, false, false, false, 1.399, 0)) == 140,
        "dual status: NaN / negative scale 0, rounded x100");
  Check(RatePerSecond(45, 1000) == 45 && RatePerSecond(90, 2000) == 45 && RatePerSecond(10, 0) == 0 &&
            RatePerSecond(1000000, 1) == 65535,
        "dual status: frames per second from a delta (clamped, 0 without elapsed time)");
  // The loader's word: 0 until a payload attaches, its writes, 0 after Detach (Stop), no write after.
  std::atomic<uint64_t> word{0};
  Sink sink;
  sink.Publish(w);
  const bool zeroNoPayload = word.load() == 0;
  sink.Attach(&word);
  sink.Publish(w);
  const bool seen = word.load() == w;
  sink.Detach();
  const bool zeroStop = word.load() == 0;
  sink.Publish(w);
  Check(zeroNoPayload && seen && zeroStop && word.load() == 0,
        "dual status: loader word 0 with no payload, the payload's value while attached, 0 after its Stop");
}

#include "ovl_cpu_test.h"  // R32: the symbology camera set on the CPU (DCS replica on WARP)
#include "r128_test.h"     // R128: the dormant mode, whole-path exactness on that replica (WARP; HW variant)

void Run() {
  printf("-- cockpit_ipd.h --\n");
  TestStereoAck();
  TestDualCameraStatus();
  TestPatchMath();
  TestOverlayMath();
  TestOverlayMvp();
  TestPrevMath();
  TestAxis();
  TestMatcher();
  TestUploadFilter();
  TestReviewFixes();
  TestBoostOut();
  TestOff();
  TestSplit(DXGI_FORMAT_R24G8_TYPELESS, false, false);
  TestSplit(DXGI_FORMAT_R32G8X24_TYPELESS, false, false);
  TestSplit(DXGI_FORMAT_R24G8_TYPELESS, true, false);
  TestSplit(DXGI_FORMAT_R24G8_TYPELESS, false, true);
  TestSplit(DXGI_FORMAT_R24G8_TYPELESS, false, false, true);
  TestPipeline();
  ocpt::Run();
}

}  // namespace cktest
