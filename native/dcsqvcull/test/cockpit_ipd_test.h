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
    ok = ok && lo.path == ck::kPathLag && std::fabs(lo.h - ck::RecH(sent[59])) < 1e-7;
    Check(ok, "cockpit ipd T4: Stepped jump with lag 1: the jump frame takes the previous record (no mis-pair)");
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
    const std::wstring ours = ExeDir() + L"..\\src\\dcsvr_perception_shm.h";
    Check(ReadAll(wp, a) && ReadAll(ours.c_str(), b) && a == b,
          "cockpit ipd T3: src/dcsvr_perception_shm.h is a byte-for-byte copy of the peer perception_shm.h");
  } else {
    printf("SKIP cockpit ipd T3 byte compare: QV_PERCEPTION_SHM_H not set (path of the peer perception_shm.h)\n");
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
  while (GetTickCount64() - t0 < 300) {
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

void __fastcall FakeExec(void* pass, void* ctx) {
  (void)pass;
  (void)ctx;
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
  g_c->UpdateSubresource(g_x.cb, 0, nullptr, full.data(), 0, 0);
  BindB7(g_x.cb, g_x.first, g_x.num);
  if (g_x.dss) g_c->OMSetDepthStencilState(g_x.dss, g_x.ref);
  if (g_x.inPass) *g_x.inPass = Snapshot(g_x.cb);
}

// The struct inside a buffer snapshot.
const uint8_t* At(const std::vector<uint8_t>& v, UINT first) { return v.size() >= first * 16 + ck::kCbBytes ? v.data() + first * 16 : nullptr; }

void TestPipeline() {
  ID3D11Device* dev = nullptr;
  ID3D11DeviceContext* imm = nullptr;
  const D3D_FEATURE_LEVEL want[] = {D3D_FEATURE_LEVEL_11_1};
  if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, want, 1, D3D11_SDK_VERSION, &dev, nullptr,
                               &imm))) {
    printf("SKIP cockpit ipd pipeline: no WARP 11.1 device\n");
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
    D3D11_DEPTH_STENCIL_DESC nd = CD3D11_DEPTH_STENCIL_DESC(D3D11_DEFAULT);
    nd.DepthEnable = FALSE;
    nd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    dev->CreateDepthStencilState(&nd, &noDepth);
  }
  const bool made = cbA && cbB && cbC && dsv[4] && dsrv[4] && defDsv && ckDss && litRtv && litVs && litPs &&
                    addBlend && noDepth;
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
  gr.Finish();
  GbPass gbp[4][3], mfdPass = gr.Pass(mfdIdx), extraCk = gr.Pass(idx[0][0]);
  for (int t = 0; t < 4; ++t)
    for (int k = 0; k < 3; ++k) gbp[t][k] = gr.Pass(idx[t][k]);
  fdtest::FakePass casc = fdtest::MakePass("CascadeShadowPass0"), lights = fdtest::MakePass("LightsBuffersPass"),
                   lighting = fdtest::MakePass("GBufferLightingPass"), tc = fdtest::MakePass("TransparentCockpitPass"),
                   tcd = fdtest::MakePass("TransparentCockpitDecalPass"), hmd = fdtest::MakePass("CopyToHMDPass"),
                   glass = fdtest::MakePass("CockpitGlassEffects");

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
  ck::Configure(1, false, true, 0, 0, false);
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
  Rot(25, 3, 0, HR);
  const double H[3] = {1.25, 0.8, -0.6};
  const double base = 0.0655;
  const double fovs[4][4] = {{-1.3, 1.0, 1.1, -1.2}, {-1.0, 1.3, 1.1, -1.2}, {-0.6, 0.5, 0.55, -0.6},
                             {-0.5, 0.6, 0.55, -0.6}};
  Mat prevVP[5];
  for (Mat& m : prevVP) m = Ident();
  bool havePrev[5] = {};
  double scale = 1.4;
  bool writeBoost = true, mfdOk = true;
  uint32_t layerFrame = 0;
  int frameNo = 0;
  // Per frame results.
  struct ViewRes {
    std::vector<uint8_t> dcs;                   // DCS's struct this frame
    std::vector<uint8_t> in[3], after[3];       // GPU view in / after the sm 4, 0, 3 passes
    std::vector<uint8_t> tcIn, tcAfter, tcdIn, lightIn;
  };
  ViewRes res[4];
  std::vector<uint8_t> prevPatchedVP[4];
  bool secondCk = false, swap0 = false, lightOn = false;
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
  auto frame = [&] {
    ++frameNo;
    if (writeBoost && bview) {
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
    ob(&casc, true);
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
    }
    for (int t = 0; t < 4; ++t) {
      ViewRes& r = res[t];
      r = ViewRes{};
      double C[3];
      eyePos(t, base * scale, C);
      const double jx = ((frameNo * 7 + t) % 16) / 16.0 * 0.004 - 0.002;
      const Mat P = ProjOf(fovs[t], jx, -jx);
      const Mat VI = ViewInvOf(HR, C);
      r.dcs.resize(ck::kCbBytes);
      BuildStruct(r.dcs.data(), VI, P, havePrev[t] ? prevVP[t] : Mul(Inv(VI), P), 100 + t);
      prevVP[t] = Get(r.dcs.data(), ck::kOffViewProj);
      havePrev[t] = true;
      ob(&lights, true);
      ob(&lights, false);
      bindDefault();
      for (int k = 0; k < 3; ++k) {
        runGb(gbp[t][k], r.dcs.data(), bufOf[t], firstOf[t], numOf[t], dsv[t], k == 0, &r.in[k], &r.after[k],
              k == 0 && t == 0 && swap0);
        if (k == 0 && t == 0 && secondCk) runGb(extraCk, r.dcs.data(), bufOf[t], firstOf[t], numOf[t], dsv[t], true,
                                                nullptr, nullptr, false);
      }
      // GBufferLightingPass: DCS's binder uploads cPerView, one full-screen draw reading the depth.
      ob(&lighting, true);
      {
        D3D11_BUFFER_DESC d{};
        bufOf[t]->GetDesc(&d);
        std::vector<uint8_t> full(d.ByteWidth, 0x5a);
        memcpy(full.data() + firstOf[t] * 16, r.dcs.data(), ck::kCbBytes);
        g_c->UpdateSubresource(bufOf[t], 0, nullptr, full.data(), 0, 0);
        BindB7(bufOf[t], firstOf[t], numOf[t]);
        r.lightIn = Snapshot(bufOf[t]);
        if (lightOn && t == 0) {
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
          g_c->OMSetBlendState(addBlend, nullptr, 0xffffffff);
          g_c->OMSetDepthStencilState(noDepth, 0);
          g_c->Draw(3, 0);
          ID3D11ShaderResourceView* nullSrv = nullptr;
          g_c->PSSetShaderResources(3, 1, &nullSrv);
          g_c->OMSetBlendState(nullptr, nullptr, 0xffffffff);
          g_c->OMSetDepthStencilState(nullptr, 0);
          // Read pixels (8, 8) and (40, 8).
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
              litRead[0] = row[8 * 4];
              litCount[0] = row[8 * 4 + 1];
              litRead[1] = row[40 * 4];
              litCount[1] = row[40 * 4 + 1];
              bool once = true;
              for (int y = 0; y < 64; ++y) {
                const float* rr = reinterpret_cast<const float*>(static_cast<const uint8_t*>(m.pData) + y * m.RowPitch);
                for (int x = 0; x < 64; ++x)
                  if (rr[x * 4 + 1] != 1.0f) once = false;
              }
              if (!once) litCount[0] = -1;
              g_c->Unmap(st, 0);
            }
            st->Release();
          }
        }
      }
      ob(&lighting, false);
      // Forward cockpit passes: their own binder upload.
      auto fwd = [&](fdtest::FakePass& p, std::vector<uint8_t>* in, std::vector<uint8_t>* after) {
        ob(&p, true);
        D3D11_BUFFER_DESC d{};
        bufOf[t]->GetDesc(&d);
        std::vector<uint8_t> full(d.ByteWidth, 0x5a);
        memcpy(full.data() + firstOf[t] * 16, r.dcs.data(), ck::kCbBytes);
        g_c->UpdateSubresource(bufOf[t], 0, nullptr, full.data(), 0, 0);
        BindB7(bufOf[t], firstOf[t], numOf[t]);
        if (in) *in = Snapshot(bufOf[t]);
        ob(&p, false);
        if (after) *after = Snapshot(bufOf[t]);
      };
      fwd(tc, &r.tcIn, &r.tcAfter);
      fwd(tcd, &r.tcdIn, nullptr);
      fwd(glass, nullptr, nullptr);
      ob(&hmd, true);
      ob(&hmd, false);
    }
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
  frame();
  Check(allDcs(), "cockpit ipd T5: scale 1 (no tail left): no patch, DCS's bytes everywhere");

  // Observe mode: everything runs, nothing is modified.
  scale = 1.4;
  ck::Configure(2, false, true, 0, 0, false);
  const uint64_t obs0 = ck::g_st.observed.load();
  frame();
  frame();
  Check(allDcs() && ck::g_st.observed.load() >= obs0 + 8,
        "cockpit ipd T5: CockpitTrueIpd=2 (observe): would-patch counted, DCS's bytes everywhere");
  ck::Configure(1, false, true, 0, 0, false);
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
  // h > 40 mm.
  scale = 2.4;
  const uint64_t big0 = ck::g_st.tooBig.load();
  frame();
  bool bigTailOnly = true;  // the frame before was patched: only its tail may run
  for (int t = 0; t < 4; ++t) {
    const uint8_t* a = At(res[t].in[0], firstOf[t]);
    bigTailOnly = bigTailOnly && a && FieldIdentical(a, res[t].dcs.data(), ck::kOffViewInv);
  }
  Check(bigTailOnly && ck::g_st.tooBig.load() > big0,
        "cockpit ipd T5: per-eye shift > 40 mm ignored (the camera stays DCS's)");
  scale = 1.4;
  frame();
  frame();
  // A second cockpit pass in view 0: the rest of the frame is DCS's; the next frame is forced off.
  secondCk = true;
  const uint64_t multi0 = ck::g_st.offMulti.load(), next0 = ck::g_st.offNext.load();
  frame();
  secondCk = false;
  bool restDcs = true;
  for (int t = 1; t < 4; ++t)
    restDcs = restDcs && same(res[t].in[0], res[t].dcs, firstOf[t]) && same(res[t].tcIn, res[t].dcs, firstOf[t]);
  frame();
  const bool nextOff = allDcs();
  frame();
  double a2 = 0, o2 = 1;
  Check(restDcs && ck::g_st.offMulti.load() > multi0 && nextOff && ck::g_st.offNext.load() > next0 &&
            shiftOf(res[0].in[0], res[0].dcs, 0, &a2, &o2) && std::fabs(a2 - hExp) < 2e-6,
        "cockpit ipd T5: a second cockpit pass in view 0: views 1-3 and their forward passes DCS's, next frame off, "
        "then patched again");
  // Forward passes off ([Perception] CockpitTrueIpdForward=0).
  ck::Configure(1, false, false, 0, 0, false);
  frame();
  double a3 = 0, o3 = 1;
  Check(shiftOf(res[2].in[0], res[2].dcs, 0, &a3, &o3) && same(res[2].tcIn, res[2].dcs, 0),
        "cockpit ipd T5: CockpitTrueIpdForward=0: the sm 4 pass patched, TransparentCockpitPass DCS's");
  ck::Configure(1, false, true, 0, 0, false);

  // Phase 2: the stencil split in the hooked GBufferLightingPass of view 0.
  ck::Configure(1, true, true, 0, 0, false);
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
  ck::Configure(1, false, true, 0, 0, false);

  // Stale: the layer stops writing; after 30 frames nothing is patched.
  writeBoost = false;
  const uint64_t stale0 = ck::g_st.offStale.load();
  for (int i = 0; i < 31; ++i) frame();
  frame();
  Check(allDcs() && ck::g_st.offStale.load() > stale0,
        "cockpit ipd T5: no new BoostOut record for 30 frames: stale, nothing patched");
  // The dev test shift without the layer: 10 mm per eye.
  ck::Configure(1, false, true, 0, 10.0, false);
  frame();
  double a4 = 0, o4 = 1, a5 = 0, o5 = 1;
  Check(shiftOf(res[0].in[0], res[0].dcs, 0, &a4, &o4) && std::fabs(a4 - 0.010) < 2e-6 &&
            shiftOf(res[1].in[0], res[1].dcs, 0, &a5, &o5) && std::fabs(a5 + 0.010) < 2e-6,
        "cockpit ipd T5: CockpitTrueIpdTestShiftMm=10 without the layer: cockpit passes shifted 10 mm per eye");
  ck::Configure(1, false, true, 0, 0, false);
  // Trace ([Perception] CockpitTrueIpdLog).
  ck::Configure(1, false, true, 2, 0, false);
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
  ck::Configure(1, false, true, 0, 0, false);
  Check(g_dispatches.load() == disp0 && g_published.load() == pub0 && !g_depthView.load() && !g_gridView.load(),
        "cockpit ipd T8: with only the cockpit option on, fixation depth samples, maps and publishes nothing");

  // Off: the uninstall, then a frame with the module on for nothing else.
  ck::Configure(0, false, true, 0, 0, false);
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
  Rel(litRtv), Rel(litTex), Rel(litVs), Rel(litPs), Rel(addBlend), Rel(noDepth);
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
  ck::Configure(0, false, true, 0, 0, false);
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
    printf("SKIP cockpit ipd T8 table: no WARP device\n");
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
  g_s = saved;
  Check(usUntouched && same,
        "cockpit ipd T8: off: the in-pass arm never takes the upload entries; the context table is bit-identical after");
  imm->Release();
  dev->Release();
}

// ---------------------------------------------------------------------------
// T9 the stencil split, white box (D24S8, D32S8, MSAA skip, debug layer)
// ---------------------------------------------------------------------------

void TestSplit(DXGI_FORMAT texFmt, bool msaa, bool debug) {
  const char* fmtName = texFmt == DXGI_FORMAT_R24G8_TYPELESS ? "D24S8" : "D32S8";
  ID3D11Device* dev = nullptr;
  ID3D11DeviceContext* imm = nullptr;
  const D3D_FEATURE_LEVEL want[] = {D3D_FEATURE_LEVEL_11_1};
  if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, debug ? D3D11_CREATE_DEVICE_DEBUG : 0, want, 1,
                               D3D11_SDK_VERSION, &dev, nullptr, &imm))) {
    printf("SKIP cockpit ipd T9 %s%s: no WARP%s device\n", fmtName, msaa ? " MSAA" : "", debug ? " debug" : "");
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
  D3D11_SUBRESOURCE_DATA ci{dcs, 0, 0};
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
  // Dry: Depth boost switched off at 1.4, the last record stays in the block.
  {
    ck::Matcher m;
    for (uint32_t n = 1; n <= 5; ++n) {
      m.Push(MakeRec(n, 1.4), n);
      m.Latch(n, nullptr, nullptr);
    }
    const auto d1 = m.Latch(6, nullptr, nullptr);  // lag 0: no new record -> h 0, not verified
    m.Verify(0.0655, d1);                            // DCS renders unboosted (would be 2h off)
    const auto d2 = m.Latch(7, nullptr, nullptr);
    m.Push(MakeRec(8, 1.4), 8);  // Depth boost back on
    const auto back = m.Latch(8, nullptr, nullptr);
    ck::Matcher l1;  // lag 1 learned: the first dry latch is still covered by the ring
    l1.lag = 1;
    for (uint32_t n = 1; n <= 3; ++n) {
      l1.Push(MakeRec(n, 1.4), n);
      l1.Latch(n, nullptr, nullptr);
    }
    const auto c1 = l1.Latch(4, nullptr, nullptr);
    const auto c2 = l1.Latch(5, nullptr, nullptr);
    Check(d1.stale && d1.dry && d1.h == 0 && d2.dry && !m.fault && m.verifies == 0 && !back.stale && back.h > 0.013 &&
              !c1.stale && c1.h > 0 && c2.dry && c2.h == 0,
          "cockpit ipd T10: no new record since the last latch (beyond the learned lag) -> h 0 at once, no pairFault; "
          "records again -> h back");
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
  const uint64_t ff = ck::g_st.fwdFail.load();
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
  ResetCk();
  ck::g_restN = 0;
  ck::g_on = oldOn;
  ck::g_cfgMode = 0;
  fixdepth::g_s.ctx = oldCtx;
  fixdepth::g_s.ctx1 = oldCtx1;
  fixdepth::g_fault = oldFdFault;
}

void Run() {
  printf("-- cockpit_ipd.h --\n");
  TestPatchMath();
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
  TestPipeline();
}

}  // namespace cktest
