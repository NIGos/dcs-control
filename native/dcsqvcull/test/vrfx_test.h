// R51 [VrFx] GazeDof / GVision: pure parts (keys, physiology, focus state machine with saccade release, G model and
// the visual-field sequence) and the pass on WARP (the real shaders in a stand-in of DCS's tone-map draw: GPU against
// the CPU reference, inert when neutral, slots restored, angular blur equal across resolutions).
// R57 (GazeDof cost): the stock-constant shader against the full one, the prep's tile maxima against the CPU's, and
// the fovea guard's band (per-tap guard path) against the CPU reference.
#pragma once

#include <array>

#include "../tools/vrfx_rig.h"

namespace vfxtest {

inline bool Near(double a, double b, double tol) { return std::fabs(a - b) <= tol; }

inline void PureTests() {
  using namespace vrfx;
  {  // keys: off by default, ranges clamped, the ini read back
    const Params d = Sanitize(Params{});
    Check(d.dof == 0 && d.gv == 0 && Same(d, Params{}) && !Wanted(),
          "vrfx: GazeDof and GVision off by default (nothing wanted, defaults already sane)");
    Params b;
    b.dofStrength = 7, b.dofMaxArcmin = 0, b.dofSettleMs = -5, b.dofGuardDeg = 99, b.dofPupilMm = 1.0, b.gvStrength = -1;
    b.gvTolerance = 50, b.gvMode = 9, b.dof = 5;
    const Params s = Sanitize(b);
    Check(s.dofStrength == 1 && s.dofMaxArcmin == 1 && s.dofSettleMs == 0 && s.dofGuardDeg == 5 && s.dofPupilMm == 2 &&
              s.gvStrength == 0 && s.gvTolerance == 10 && s.gvMode == 0 && s.dof == 1,
          "vrfx: out-of-range keys clamped to their documented ranges");
    wchar_t tmp[MAX_PATH], path[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    GetTempFileNameW(tmp, L"vfx", 0, path);
    WritePrivateProfileStringW(L"VrFx", L"GazeDof", L"1", path);
    WritePrivateProfileStringW(L"VrFx", L"GazeDofStrength", L"0.35", path);
    WritePrivateProfileStringW(L"VrFx", L"GVision", L"1", path);
    WritePrivateProfileStringW(L"VrFx", L"GVisionUseGaze", L"0", path);
    WritePrivateProfileStringW(L"VrFx", L"GVisionTolerance", L"8.5", path);
    const Params r = ReadIni(path);
    DeleteFileW(path);
    printf("     (ini: GazeDof %d strength %.3f GVision %d use gaze %d tolerance %.2f release %d)\n", r.dof, r.dofStrength,
           r.gv, r.gvUseGaze, r.gvTolerance, r.dofSaccadeRelease);
    const Params none = ReadIni(L"C:\\this\\ini\\does\\not\\exist.ini");
    Check(r.dof == 1 && Near(r.dofStrength, 0.35, 1e-9) && r.gv == 1 && r.gvUseGaze == 0 && Near(r.gvTolerance, 8.5, 1e-9) &&
              r.dofSaccadeRelease == 1 && Same(none, Params{}),
          "vrfx: [VrFx] keys read from the ini; a missing ini gives the defaults");
  }
  {  // DCS's G-EFFECT option
    const std::string real = "options = {\n\t[\"difficulty\"] = {\n\t\t[\"geffect\"] = \"realistic\",\n";
    const std::string none = "\t\t[\"geffect\"] = \"none\",";
    Check(ParseGEffect(real) == 2 && ParseGEffect(none) == 0 && ParseGEffect("[\"geffect\"] = \"reduced\"") == 1 &&
              ParseGEffect("nothing") == -1 && EffectiveGvMode(0, 2) == 1 && EffectiveGvMode(0, 0) == 2 &&
              EffectiveGvMode(0, -1) == 1 && EffectiveGvMode(2, 2) == 2 && EffectiveGvMode(1, 0) == 1,
          "vrfx: DCS's G-EFFECT parsed from options.lua; auto = complement unless DCS's is none");
  }
  {  // physiology
    const double day = PupilMm(3000), cockpit = PupilMm(50), night = PupilMm(0.03);
    Check(Near(day, 2.25, 0.05) && Near(cockpit, 3.13, 0.05) && Near(night, 6.53, 0.05) && day < cockpit && cockpit < night,
          "vrfx: pupil (Moon & Spencer) 2.25 mm sunlit, 3.1 mm at 50 cd/m2, 6.5 mm moonlit");
    Check(Near(SceneCdm2(true, 0.5, 0), 3000, 1) && Near(SceneCdm2(true, -0.3, 0), 0.03, 1e-6) &&
              Near(SceneCdm2(true, 0.5, 0.004), 300, 1) && Near(SceneCdm2(false, 0, 0.04), 3000, 1),
          "vrfx: scene luminance: sun sets the absolute level, DCS's adapted value moves it (x0.1 .. x10)");
    // panel at 0.7 m while focused at infinity, 3 mm pupil, 0.3 D depth of focus: (1.43 - 0.3) x 3 x 3.438 = 11.6'
    const double c = CocArcmin(1 / 0.7, 0, 3.0, 0.3, 1.0, 100);
    Check(Near(c, 11.64, 0.05) && CocArcmin(1 / 0.7, 1 / 0.7, 3, 0.3, 1, 100) == 0 && CocArcmin(0.2, 0, 3, 0.3, 1, 100) == 0 &&
              CocArcmin(1 / 0.7, 0, 3, 0.3, 0.5, 100) == c * 0.5 && CocArcmin(1 / 0.7, 0, 3, 0.3, 1, 6) == 6,
          "vrfx: circle of confusion from pupil x dioptre difference beyond the depth of focus, scaled and capped");
    // seam: the same angle is the same blur in a focus-like (2028 px over +-0.6) and a periphery-like view (1040 px
    // over +-1.2): pixels differ by the density ratio, arcmin equal
    FxCb a{}, b{};
    a.viewW = 2028, b.viewW = 1040;
    a.dofAmt = b.dofAmt = 1, a.pupilMm = b.pupilMm = 3, a.maxArcmin = b.maxArcmin = 20, a.tolD = b.tolD = 0.3f;
    CProj pa, pb;
    pa.p00 = 1 / 0.6f, pb.p00 = 1 / 1.2f;
    const float ra = CocRadiusPx(a, pa, 1.5f, {0, 0}), rb = CocRadiusPx(b, pb, 1.5f, {0, 0});
    const double arcA = 2 * ra / (0.5 * 2028 / 0.6 * 2.9088821e-4), arcB = 2 * rb / (0.5 * 1040 / 1.2 * 2.9088821e-4);
    Check(Near(arcA, arcB, 1e-3) && Near(arcA, CocArcmin(1.5, 0, 3, 0.3, 1, 20), 1e-3) && ra > 3 * rb,
          "vrfx: blur is angular: equal arcmin in focus and periphery views (no seam), pixels follow each view's density");
    a.guardDeg = 2, a.gazeX = 0.1f;
    const float g0 = CocRadiusPx(a, pa, 1.5f, {0.1f, 0}), g2 = CocRadiusPx(a, pa, 1.5f, {0.1345f, 0});
    const float g5 = CocRadiusPx(a, pa, 1.5f, {0.1f + 0.085f, 0});
    Check(g0 == 0 && g2 == 0 && g5 > 0.9f * ra, "vrfx: the fovea guard keeps the gazed content sharp (none within 2 deg, full from 4 deg)");
  }
  {  // the focus state machine
    Params p;
    p.dof = 1;
    DofState s;
    DofIn in;
    in.dt = 1 / 90.0, in.gazeValid = true, in.depthValid = true, in.medianM = 20000, in.conf = 0.9;
    StepDof(s, in, p);  // first plane: focus at infinity, released
    int frames = 0;
    while (s.gain < 1 && frames < 200) StepDof(s, in, p), ++frames;
    // settle 120 ms + fade-in 250 ms at 90 Hz = 33 frames
    Check(s.haveFocus && s.focusD < 0.001 && frames >= 32 && frames <= 35 && s.gain == 1,
          "vrfx focus: first fixation: sharp for the settle time, then the blur fades in (120 + 250 ms)");
    in.saccade = true;
    StepDof(s, in, p);
    Check(s.gain == 0 && s.saccades == 1, "vrfx focus: saccade onset releases the blur in the same frame");
    in.saccade = false;
    in.medianM = 0.7;  // landed on the panel
    StepDof(s, in, p);
    const bool sharpNow = s.gain == 0 && Near(s.focusD, 1 / 0.7, 1e-9) && s.newPlanes == 1;
    int back = 0;
    while (s.gain == 0 && back < 100) StepDof(s, in, p), ++back;
    Check(sharpNow && back >= 10 && back <= 12, "vrfx focus: new fixation plane re-targets at once and stays sharp for the settle time");
    // velocity backup: gaze jumps 10 deg in one frame without the layer's flag
    for (int i = 0; i < 40; ++i) StepDof(s, in, p);
    in.gazeX = std::tan(10 * kPi / 180);
    StepDof(s, in, p);
    Check(s.gain == 0, "vrfx focus: a 900 deg/s gaze jump without the saccade flag releases too (velocity backup)");
    // edge flicker: alternating panel / sky estimates never let the blur come back
    double gmax = 0;
    for (int i = 0; i < 300; ++i) {
      in.medianM = (i / 3) % 2 ? 0.7 : 20000;
      StepDof(s, in, p);
      gmax = std::max(gmax, s.gain);
    }
    Check(gmax == 0, "vrfx focus: an estimate flipping between two planes every 33 ms keeps everything sharp");
    // low confidence fades out within 0.1 s, no gaze too
    in.medianM = 0.7;
    for (int i = 0; i < 60; ++i) StepDof(s, in, p);
    const double g1 = s.gain;
    in.conf = 0.3;
    for (int i = 0; i < 9; ++i) StepDof(s, in, p);
    const double g2 = s.gain;
    in.conf = 0.9;
    for (int i = 0; i < 60; ++i) StepDof(s, in, p);
    in.gazeValid = false;
    for (int i = 0; i < 9; ++i) StepDof(s, in, p);
    Check(g1 == 1 && g2 == 0 && s.gain == 0, "vrfx focus: low confidence and lost gaze fade the blur out within 0.1 s");
    // release off: a saccade alone does not release
    p.dofSaccadeRelease = 0;
    in.gazeValid = true;
    in.gazeX = 0;
    for (int i = 0; i < 60; ++i) StepDof(s, in, p);
    in.saccade = true;
    StepDof(s, in, p);
    Check(s.gain > 0, "vrfx focus: GazeDofSaccadeRelease=0 keeps the blur through a saccade on the same plane");
    Check(Near(HistConf(std::array<uint16_t, 16>{0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 60, 40}.data()), 1.0, 1e-9) &&
              Near(HistConf(std::array<uint16_t, 16>{50, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 50}.data()), 0.5, 1e-9),
          "vrfx focus: confidence = the cone's share in its dominant distance band");
  }
  {  // G model and the visual-field sequence
    Params p;
    p.gv = 1;
    GState g;
    for (int i = 0; i < 90 * 10; ++i) StepG(g, true, 6.0, 1 / 90.0, p);
    const double below = g.loss;
    GState s;
    double t03 = -1, t1 = -1;
    for (int i = 1; i <= 90 * 12; ++i) {
      StepG(s, true, 9.0, 1 / 90.0, p);
      if (t03 < 0 && s.loss >= 0.3) t03 = i / 90.0;
      if (t1 < 0 && s.loss >= 1.0) t1 = i / 90.0;
    }
    Check(below == 0 && t03 > 0.6 && t03 < 1.0 && t1 > 3.5 && t1 < 5.5,
          "vrfx G: 6 G sustained (tolerance 7.5) no effect; 9 G: grey-out after ~0.8 s, blackout after ~4.5 s");
    double rec = -1;
    for (int i = 1; i <= 90 * 10; ++i) {
      StepG(s, true, 1.0, 1 / 90.0, p);
      if (rec < 0 && s.loss <= 0.1) rec = i / 90.0;
    }
    Check(rec > 3 && rec < 7, "vrfx G: recovery to light grey within ~5 s after unloading");
    GState st = g;
    st.loss = 0.8;
    for (int i = 0; i < 180; ++i) StepG(st, false, 0, 1 / 90.0, p);
    Check(st.loss < 0.4, "vrfx G: a stale G input (no Lua samples) lets the effect fade out");
    GState neg;
    for (int i = 0; i < 90 * 5; ++i) StepG(neg, true, -3.5, 1 / 90.0, p);
    Params pr = p;
    pr.gvRedout = 0;
    GState neg2;
    for (int i = 0; i < 90 * 5; ++i) StepG(neg2, true, -3.5, 1 / 90.0, pr);
    Check(neg.red > 0.9 && neg.loss == 0 && neg2.red == 0, "vrfx G: -3.5 G gives red-out (GVisionRedout=0: none)");
    // sequence: colour first, then the periphery's light, then the centre
    const Shape a = ShapeOf(0.25, 0, 1, 2), b = ShapeOf(0.6, 0, 1, 2), c = ShapeOf(1.1, 0, 1, 2), z = ShapeOf(0, 0, 1, 2);
    Check(Neutral(z) && a.desatP > 0.6 && a.outside > 0.8 && a.global == 1 && a.desatC == 0 && b.tunnel < 40 &&
              b.tunnel > 20 && b.outside < 0.35 && b.global == 1 && b.desatC > 0.3 && c.global == 0 && c.tunnel == 0,
          "vrfx G: colour lost first (periphery), then light from the periphery (tunnel), then the centre (blackout)");
    const Shape m = ShapeOf(1.1, 0, 1, 1), h = ShapeOf(0.6, 0, 0.5, 2), r2 = ShapeOf(0, 1, 1, 2), r1 = ShapeOf(0, 1, 1, 1);
    Check(m.global == 1 && m.outside >= 0.35f && Near(h.outside, 1 + (b.outside - 1) * 0.5, 1e-6) && r2.red > 0.5 &&
              r2.global < 0.6 && r1.red == 0,
          "vrfx G: complement mode leaves the darkening to DCS; strength scales; red-out only in full mode");
    // per pixel: at the gaze the light is kept at L 0.6, 40 deg out it is nearly gone; the tunnel follows the gaze
    FxCb cb{};
    cb.gvOn = 1, cb.viewW = 200, cb.viewH = 200;
    cb.tunnel = b.tunnel, cb.edge = b.edge, cb.outside = b.outside, cb.global = b.global;
    cb.desatC = b.desatC, cb.colorEdge = b.colorEdge, cb.desatP = b.desatP, cb.red = b.red;
    cb.gvX = 0.3f;
    CProj pp;
    pp.p00 = pp.p11 = 1 / 1.2f;
    auto at = [&](float tx) {
      float c3[3] = {0.4f, 0.2f, 0.1f};
      const unsigned x = static_cast<unsigned>((tx * pp.p00 + 1) * 0.5f * 200);
      GVision(cb, pp, x, 100, c3);
      return c3[0] * 0.2126f + c3[1] * 0.7152f + c3[2] * 0.0722f;
    };
    const float l0 = 0.4f * 0.2126f + 0.2f * 0.7152f + 0.1f * 0.0722f;
    const float plus30 = at(1.06f), minus30 = at(-0.236f), mirror = at(-1.06f);
    printf("     (light at the gaze %.4f of %.4f; 30 deg either side of the gaze %.4f / %.4f; mirrored about the view centre %.4f)\n",
           at(0.3f), l0, plus30, minus30, mirror);
    Check(Near(at(0.3f), l0, 1e-3 * l0) && at(-1.0f) < 0.35f * l0 && Near(plus30, minus30, 0.08 * plus30) && mirror < 0.8f * plus30,
          "vrfx G: the tunnel is centred on the gaze, not the view centre");
  }
}

// R57: the GPU's prep (read back after the last draw) against the CPU reference's Prep: the worst relative
// difference of the tile maxima (radii, px; below 0.1 px absolute) and of the half-resolution texels (the GPU stores
// those as half floats: 1 ulp is ~1e-3 relative; the device's float -> half rounding may differ from the CPU's float).
inline void PrepWorst(vfxrig::Rig& r, const vrfx::FxCb& cb, double* tiles, double* texels) {
  *tiles = *texels = 1e9;
  vrfx::CPrep g;
  if (!vfxrig::ReadPrep(r, cb, g)) return;
  vrfx::CView view;
  view.w = r.sc.w, view.h = r.sc.h, view.rgb = r.rgbGpu, view.depth = r.depthDev;
  const vrfx::CPrep c = vrfx::Prep(view, cb, vrfx::ProjFromStored(r.projStored));
  *tiles = *texels = 0;
  for (size_t i = 0; i < c.tiles.size(); ++i)
    *tiles = std::max(*tiles, std::fabs(static_cast<double>(g.tiles[i]) - c.tiles[i]) / std::max(static_cast<double>(c.tiles[i]), 0.1));
  for (size_t i = 0; i < c.px.size(); ++i)
    *texels = std::max(*texels, std::fabs(static_cast<double>(g.px[i]) - c.px[i]) / std::max(std::fabs(static_cast<double>(c.px[i])), 1e-2));
}

inline void GpuTests() {
  using namespace vfxrig;
  // A focus-like view: 120 x 80 px over +-0.06 (about 0.3 px per arcmin at the centre), panel at 0.7 m on the left
  // with fine detail, terrain at 2 km on the right, sky at the top right.
  Scene sc;
  sc.w = 120, sc.h = 80;
  sc.l = -0.06f, sc.r = 0.06f, sc.u = 0.04f, sc.d = -0.04f;
  sc.adapted = 0.05f, sc.ec = 0.2f;
  sc.rgb.assign(sc.w * sc.h * 3, 0);
  sc.rangeM.assign(sc.w * sc.h, 0);
  for (unsigned y = 0; y < sc.h; ++y)
    for (unsigned x = 0; x < sc.w; ++x) {
      float* c = &sc.rgb[(y * sc.w + x) * 3];
      float& rm = sc.rangeM[y * sc.w + x];
      if (x < 50) {
        const bool line = (x % 6 < 2) || (y % 9 == 0);
        c[0] = line ? 0.9f : 0.03f, c[1] = line ? 0.8f : 0.03f, c[2] = line ? 0.3f : 0.035f;
        rm = 0.7f;
      } else if (y < 25) {
        c[0] = 0.12f, c[1] = 0.2f, c[2] = 0.45f;
        rm = 0;  // sky
      } else {
        const bool f = ((x / 3 + y / 3) % 2) != 0;
        c[0] = f ? 0.08f : 0.03f, c[1] = f ? 0.12f : 0.05f, c[2] = 0.02f;
        rm = 2000;
      }
    }
  Rig r;
  std::string err;
  if (!Make(r, sc, err)) {
    printf("     %s\n", err.c_str());
    Check(false, "vrfx pass: WARP rig (device, real shaders, objects)");
    r.Release();
    Clear();
    return;
  }
  cpass::g_rt = cpass::Rt{};
  cpass::g_fault = false;
  cpass::g_mode = 0;
  auto compare = [&](const std::vector<uint8_t>& gpu, const std::vector<uint8_t>& cpu, int* worst, double* w1) {
    int w = 0, ok = 0, n = 0;
    for (size_t i = 0; i < gpu.size(); ++i) {
      if (i % 4 == 3) continue;
      const int d = std::abs(static_cast<int>(gpu[i]) - static_cast<int>(cpu[i]));
      w = std::max(w, d), ok += d <= 1, ++n;
    }
    *worst = w, *w1 = n ? static_cast<double>(ok) / n : 0;
    printf("     (GPU vs CPU: worst %d codes, %.4f within 1)\n", w, *w1);
  };
  // 1. Stock: Mode 0 and no effect: DCS's own draw, nothing swapped.
  Clear();
  Check(Draw(r, 2) == 0, "vrfx pass: Mode 0 with both effects off swaps nothing (DCS's own tone map)");
  const std::vector<uint8_t> magenta = Read(r);
  // 2. GazeDof enabled but released (gain 0): our stock emulation, bit-equal to the R42 shader's stock emulation.
  FxSet f;
  f.dof = true, f.gain = 0, f.focusD = 0, f.pupil = 3, f.strength = 1, f.maxArcmin = 20, f.guard = 0;
  Apply(f);
  int act = Draw(r, 2);
  const std::vector<uint8_t> inert = Read(r);
  int worst = 0;
  double w1 = 0;
  compare(inert, Reference(r, cpass::FxBuild(2, sc.w, sc.h, true)), &worst, &w1);
  Check(act == 1 && magenta[0] == 255 && worst <= 1 && w1 > 0.999,
        "vrfx pass: GazeDof on while released: DCS's tone map emulated (CPU replica within 1 code), no blur");
  // 3. Focused at infinity: the panel is blurred, the far terrain is not.
  f.gain = 1;
  Apply(f);
  ID3D11ShaderResourceView* sent[3] = {r.bloomSrv, r.lumSrv, r.hdrSrv};
  ID3D11Buffer* sentCb = r.b0;
  act = Draw(r, 2, sent, sentCb);
  const std::vector<uint8_t> farImg = Read(r);
  vrfx::FxCb cbFar = cpass::FxBuild(2, sc.w, sc.h, true);
  compare(farImg, Reference(r, cbFar), &worst, &w1);
  Check(act == 1 && cbFar.dofOn == 1 && worst <= 2 && w1 > 0.995,
        "vrfx pass: GazeDof on WARP = CPU reference (prep CS + gather; > 99.5 % within 1 code)");
  auto contrast = [&](const std::vector<uint8_t>& img, unsigned x0, unsigned x1, unsigned y0, unsigned y1) {
    double s = 0, s2 = 0;
    int n = 0;
    for (unsigned y = y0; y < y1; ++y)
      for (unsigned x = x0; x < x1; ++x) {
        const double v = img[(y * sc.w + x) * 4 + 1];
        s += v, s2 += v * v, ++n;
      }
    const double m = s / n;
    return std::sqrt(std::max(0.0, s2 / n - m * m));
  };
  const double panelSharp = contrast(inert, 10, 40, 20, 60), panelFar = contrast(farImg, 10, 40, 20, 60);
  const double terrSharp = contrast(inert, 70, 110, 35, 75), terrFar = contrast(farImg, 70, 110, 35, 75);
  printf("     (panel contrast %.1f -> %.1f, terrain %.1f -> %.1f)\n", panelSharp, panelFar, terrSharp, terrFar);
  Check(panelFar < 0.8 * panelSharp && std::fabs(terrFar - terrSharp) < 0.02 * terrSharp + 0.5,
        "vrfx pass: focused far: the 0.7 m panel loses detail, the 2 km terrain is untouched");
  // slots restored
  {
    ID3D11ShaderResourceView* now[3] = {};
    ID3D11Buffer* cb = nullptr;
    r.ctx->PSGetShaderResources(vrfx::kPsFxSrvSlot, 3, now);
    r.ctx->PSGetConstantBuffers(vrfx::kPsFxCbSlot, 1, &cb);
    ID3D11ComputeShader* cs = nullptr;
    r.ctx->CSGetShader(&cs, nullptr, nullptr);
    ID3D11UnorderedAccessView* u[2] = {};
    r.ctx->CSGetUnorderedAccessViews(0, 2, u);
    const bool ok = now[0] == sent[0] && now[1] == sent[1] && now[2] == sent[2] && cb == sentCb && !cs && !u[0] && !u[1];
    for (auto* s : now)
      if (s) s->Release();
    if (cb) cb->Release();
    if (cs) cs->Release();
    for (auto* x : u)
      if (x) x->Release();
    Check(ok, "vrfx pass: PS t22-t24 / b11 and the CS state are DCS's again after the draw");
  }
  // 4. Focused on the panel: the panel is sharp, the terrain blurred; GPU = CPU.
  f.focusD = 1 / 0.7;
  Apply(f);
  Draw(r, 2);
  const std::vector<uint8_t> nearImg = Read(r);
  compare(nearImg, Reference(r, cpass::FxBuild(2, sc.w, sc.h, true)), &worst, &w1);
  const double panelNear = contrast(nearImg, 10, 40, 20, 60), terrNear = contrast(nearImg, 70, 110, 35, 75);
  printf("     (focused on the panel: panel contrast %.1f, terrain %.1f)\n", panelNear, terrNear);
  Check(worst <= 2 && w1 > 0.995 && std::fabs(panelNear - panelSharp) < 0.02 * panelSharp + 0.5 && terrNear < 0.9 * terrSharp,
        "vrfx pass: focused on the panel: panel untouched, terrain softened (GPU = CPU reference)");
  // 5. The fovea guard: gaze on the panel at x 20 while focused far: a 2-4 deg guard is wider than this tiny view,
  // so the whole panel stays sharp.
  f.focusD = 0, f.guard = 5;
  f.gaze[0] = -0.06f + 20.5f / sc.w * 0.12f, f.gaze[1] = 0;
  Apply(f);
  Draw(r, 2);
  const std::vector<uint8_t> guarded = Read(r);
  Check(std::fabs(contrast(guarded, 10, 40, 20, 60) - panelSharp) < 0.02 * panelSharp + 0.5,
        "vrfx pass: content within the fovea guard of the gaze is never blurred");
  // R57: Mode 0 + effects runs on the stock-constant shader, with the full effects shader's image; the prep's tile
  // maxima and texels are the CPU reference's.
  {
    f.focusD = 0, f.guard = 0;
    Apply(f);
    cpass::g_fxStockShader = 1;
    Draw(r, 2);
    const bool stockUsed = cpass::g_rt.cbStock;
    const std::vector<uint8_t> a = Read(r);
    double tw = 0, pw = 0;
    PrepWorst(r, cpass::FxBuild(2, sc.w, sc.h, true), &tw, &pw);
    cpass::g_fxStockShader = 0;
    Draw(r, 2);
    const std::vector<uint8_t> b = Read(r);
    cpass::g_fxStockShader = 1;
    int d = 0;
    for (size_t i = 0; i < a.size(); ++i) d = std::max(d, std::abs(static_cast<int>(a[i]) - static_cast<int>(b[i])));
    printf("     (stock-constant vs full effects shader: worst %d codes; prep vs CPU: tile maxima %.2e, texels %.2e relative)\n",
           d, tw, pw);
    Check(stockUsed && d <= 1, "vrfx pass (R57): Mode 0 + effects on the stock-constant shader = the full effects shader");
    Check(tw < 1e-3 && pw < 2e-3,
          "vrfx pass (R57): the prep's tile maxima (one-atomic reduction, guard test) and texels = the CPU reference's");
  }
  // 6. GVision on WARP = CPU reference (Mode 0, loss 0.6, gaze off-centre).
  FxSet g;
  g.gv = true, g.loss = 0.6, g.gvMode = 2;
  g.gaze[0] = 0.02f, g.gaze[1] = -0.01f;
  Apply(g);
  // this narrow view lies inside the 32 deg tunnel: the check covers the central colour loss
  Draw(r, 1);
  compare(Read(r), Reference(r, cpass::FxBuild(1, sc.w, sc.h, true)), &worst, &w1);
  Check(worst <= 1 && w1 > 0.999, "vrfx pass: GVision on WARP = CPU reference (within 1 code)");
  // 7. GVision neutral (no G): DCS's own draw again (no swap in Mode 0).
  g.loss = 0;
  Apply(g);
  Check(Draw(r, 1) == 0, "vrfx pass: GVision on but no G: nothing swapped (DCS's own tone map)");
  // 8. Off: nothing.
  Clear();
  Check(Draw(r, 3) == 0 && cpass::FxBuild(3, sc.w, sc.h, true).dofOn == 0 && cpass::FxBuild(3, sc.w, sc.h, true).gvOn == 0,
        "vrfx pass: both keys 0: nothing armed, nothing swapped");
  cpass::g_obj.store(nullptr);
  cpass::g_rt = cpass::Rt{};
  for (auto& s : cpass::g_swaps) s = 0;
  for (auto& s : cpass::g_rejects) s = 0;
  r.Release();
  Clear();
}

// R57: the fovea guard's band crossing blurred content. A wider view (240 x 160 over +-0.2) with the gaze on the
// panel's top edge, exaggerated blur: the per-tap guard path (discs touching the 2-4 deg band), the fast path (discs
// beyond it) and the early return (discs inside the guard) all against the CPU reference, which evaluates the guard at
// every tap. The lamps make any tap that picks another texel visible.
inline void GuardBandTests() {
  using namespace vfxrig;
  Scene sc;
  sc.w = 240, sc.h = 160;
  sc.l = -0.2f, sc.r = 0.2f, sc.u = 0.1333f, sc.d = -0.1333f;
  sc.adapted = 0.05f, sc.ec = 0.2f;
  sc.rgb.assign(sc.w * sc.h * 3, 0);
  sc.rangeM.assign(sc.w * sc.h, 0);
  for (unsigned y = 0; y < sc.h; ++y)
    for (unsigned x = 0; x < sc.w; ++x) {
      float* c = &sc.rgb[(y * sc.w + x) * 3];
      float& rm = sc.rangeM[y * sc.w + x];
      if (y >= 80) {  // panel at 0.7 m with fine lines and a few bright lamps
        const bool line = (x % 5 < 2) || (y % 7 == 0);
        c[0] = line ? 0.9f : 0.03f, c[1] = line ? 0.8f : 0.03f, c[2] = line ? 0.3f : 0.035f;
        if (x % 37 == 3 && y % 23 == 90 % 23) c[0] = 20, c[1] = 6, c[2] = 1;
        rm = 0.7f;
      } else if (y < 30) {
        c[0] = 0.12f, c[1] = 0.2f, c[2] = 0.45f;
        rm = 0;  // sky
      } else {
        const bool f = ((x / 3 + y / 3) % 2) != 0;
        c[0] = f ? 0.08f : 0.03f, c[1] = f ? 0.12f : 0.05f, c[2] = 0.02f;
        rm = 2000;
      }
    }
  Rig r;
  std::string err;
  if (!Make(r, sc, err)) {
    printf("     %s\n", err.c_str());
    Check(false, "vrfx pass (R57): WARP rig for the guard band");
    r.Release();
    Clear();
    return;
  }
  cpass::g_rt = cpass::Rt{};
  cpass::g_fault = false;
  cpass::g_mode = 0;
  FxSet f;
  f.dof = true, f.gain = 1, f.focusD = 0, f.pupil = 6, f.strength = 1, f.maxArcmin = 20, f.guard = 2;
  f.gaze[0] = 0.01f, f.gaze[1] = 0.1333f - 80.5f / sc.h * 0.2667f;  // on the panel's top edge
  Apply(f);
  Draw(r, 2);
  const std::vector<uint8_t> img = Read(r);
  const vrfx::FxCb cb = cpass::FxBuild(2, sc.w, sc.h, true);
  // the prep against the CPU's (tile maxima through the guard test), then the gather against the CPU gather on the
  // GPU's own prep: the stored dioptres are half floats, and whether a same-plane sample counts as nearer than the
  // pixel follows the device's rounding of that store (WARP truncates), in R51 as now; the gather is compared free of it
  double tw = 0, pw = 0;
  PrepWorst(r, cb, &tw, &pw);
  vrfx::CPrep gpuPrep;
  const bool readOk = ReadPrep(r, cb, gpuPrep);
  const std::vector<uint8_t> ref = Reference(r, cb, &gpuPrep);
  vrfx::FxCb noGuard = cb;
  noGuard.guardDeg = 0;
  const std::vector<uint8_t> refNoGuard = Reference(r, noGuard, &gpuPrep);
  int worst = 0, ok = 0, n = 0, guardMatters = 0;
  for (size_t i = 0; i < img.size(); ++i) {
    if (i % 4 == 3) continue;
    const int d = std::abs(static_cast<int>(img[i]) - static_cast<int>(ref[i]));
    worst = std::max(worst, d), ok += d <= 1, ++n;
    guardMatters += std::abs(static_cast<int>(ref[i]) - static_cast<int>(refNoGuard[i])) > 2;
  }
  printf("     (guard band: GPU vs CPU gather worst %d codes, %.4f within 1; channels the guard changes by > 2 codes: %d; "
         "prep vs CPU: tile maxima %.2e, texels %.2e relative)\n",
         worst, n ? static_cast<double>(ok) / n : 0.0, guardMatters, tw, pw);
  Check(readOk && guardMatters > 200 && worst <= 2 && ok > 0.995 * n && tw < 1e-3 && pw < 2e-3,
        "vrfx pass (R57): the fovea guard's band across blurred content (per-tap guard path) = CPU reference");
  cpass::g_obj.store(nullptr);
  cpass::g_rt = cpass::Rt{};
  for (auto& s2 : cpass::g_swaps) s2 = 0;
  for (auto& s2 : cpass::g_rejects) s2 = 0;
  r.Release();
  Clear();
}

// R58: the DoF prep targets only grow. Views whose half sizes cross (periphery taller, focus wider) used to recreate
// the targets at every view switch (2-4 times per frame on the render thread).
inline void FxTargetsGrowOnly() {
  ID3D11Device* dev = nullptr;
  const D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
  if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, &fl, 1, D3D11_SDK_VERSION, &dev, nullptr,
                               nullptr))) {
    Check(false, "vrfx targets (R58): WARP device");
    return;
  }
  cpass::Objects o;
  o.dev = dev;
  auto tiles = [](UINT h) { return (h + 7) / 8; };
  const bool a = o.EnsureFxTargets(650, 650, tiles(650), tiles(650));  // periphery
  const bool b = o.EnsureFxTargets(800, 550, tiles(800), tiles(550));  // focus: wider, shorter
  ID3D11Texture2D* settled = o.prepTex;
  int recreated = 0;
  for (int frame = 0; frame < 4; ++frame) {
    o.EnsureFxTargets(650, 650, tiles(650), tiles(650));
    recreated += o.prepTex != settled ? 1 : 0;
    settled = o.prepTex;
    o.EnsureFxTargets(800, 550, tiles(800), tiles(550));
    recreated += o.prepTex != settled ? 1 : 0;
    settled = o.prepTex;
  }
  Check(a && b && recreated == 0 && o.prepW == 800 && o.prepH == 650 && o.tileW == tiles(800) && o.tileH == tiles(650),
        "vrfx targets (R58): crossing view sizes settle on the union (grow only, no per-frame recreation)");
  o.ReleaseFxTargets();
  o.dev = nullptr;
  dev->Release();
}

// R58 GVision: a NaN G does not latch the state; the field is off while the camera is external (state kept); a mission
// event drops the state of the previous aircraft or mission.
inline void GVisionR58() {
  using namespace vrfx;
  Params p;
  p.gv = 1;
  GState s;
  StepG(s, true, std::nan(""), 1 / 90.0, p);
  const bool nanIn = std::isfinite(s.loss) && s.loss == 0 && std::isfinite(s.red);
  s.loss = std::nan(""), s.red = std::nan("");
  StepG(s, false, 0, 1 / 90.0, p);
  Check(nanIn && std::isfinite(s.loss) && std::isfinite(s.red) && Neutral(ShapeOf(s.loss, s.red, 1, 2)),
        "vrfx G (R58): a NaN G sample or state does not latch the field (finite, recovers)");
  const uint64_t q0 = g_gSeq.load();
  NoteG(0, std::nanf(""), 0, 1.0);
  const bool dropped = g_gSeq.load() == q0;
  NoteG(0, 9, 0, -1.0);
  const bool ext = g_gExternal.load() && g_gSeq.load() == q0 + 1;
  NoteG(0, 1, 0, 2.0);
  Check(dropped && ext && !g_gExternal.load(), "vrfx G (R58): non-finite samples dropped; a negative model time = external camera");

  vfxrig::Clear();
  while (g_paramLock.test_and_set()) {
  }
  g_params = p;
  g_paramLock.clear();
  g_paramVer++;
  g_wantGv = true;
  cpass::FxRt& f = cpass::g_fx;
  cpass::FxFrame(false, 0);  // takes the parameters
  f.g.loss = 1.2;
  NoteG(0, 9, 0, 3.0);
  cpass::FxFrame(false, 0);
  const bool cockpitField = f.gvOn && !Neutral(f.shape) && cpass::FxActive();
  NoteG(0, 9, 0, -1.0);
  cpass::FxFrame(false, 0);
  const bool extOff = Neutral(f.shape) && !cpass::FxActive() && f.g.loss > 1.0;
  Check(cockpitField && extOff, "vrfx G (R58): an external camera draws no field, the physiology keeps its state");
  NoteG(0, 9, 0, 4.0);
  NoteMissionEvent();
  cpass::FxFrame(false, 0);
  Check(f.g.loss == 0 && Neutral(f.shape) && f.dof.gain == 0,
        "vrfx G (R58): a mission / slot event drops the previous blackout (pre-event samples not new)");
  g_gExternal = false;
  while (g_paramLock.test_and_set()) {
  }
  g_params = Params{};
  g_paramLock.clear();
  g_paramVer++;
  vfxrig::Clear();
}

// R77: a NaN texel in DCS's HDR counts as black in the DoF prep (it was clamped to 65504 by min(): a white disc
// over the defocused neighbours). The GPU prep and the CPU reference agree on it.
inline void DofNanR77() {
  using namespace vfxrig;
  Scene sc;
  sc.w = 120, sc.h = 80;
  sc.l = -0.06f, sc.r = 0.06f, sc.u = 0.04f, sc.d = -0.04f;
  sc.adapted = 0.05f, sc.ec = 0.2f;
  sc.rgb.assign(sc.w * sc.h * 3, 0);
  sc.rangeM.assign(sc.w * sc.h, 0);
  for (unsigned y = 0; y < sc.h; ++y)
    for (unsigned x = 0; x < sc.w; ++x) {
      float* c = &sc.rgb[(y * sc.w + x) * 3];
      const bool line = (x % 6 < 2) || (y % 9 == 0);
      c[0] = line ? 0.9f : 0.03f, c[1] = line ? 0.8f : 0.03f, c[2] = line ? 0.3f : 0.035f;
      sc.rangeM[y * sc.w + x] = 0.7f;  // a panel at 0.7 m: blurred when focused far
    }
  Rig r;
  std::string err;
  if (!Make(r, sc, err)) {
    printf("     %s\n", err.c_str());
    Check(false, "R77: vrfx DoF NaN rig");
    r.Release();
    Clear();
    return;
  }
  const unsigned nx = 21, ny = 40;  // the NaN texel (prep texel 10, 20)
  {
    const uint16_t nanHalf[4] = {0x7e00, 0x7e00, 0x7e00, 0x3c00};
    const D3D11_BOX box{nx, ny, 0, nx + 1, ny + 1, 1};
    r.ctx->UpdateSubresource(r.hdr, 0, &box, nanHalf, 8, 0);
    for (int k = 0; k < 3; ++k) r.rgbGpu[(ny * sc.w + nx) * 3 + k] = std::nanf("");
  }
  cpass::g_rt = cpass::Rt{};
  cpass::g_fault = false;
  cpass::g_mode = 0;
  FxSet f;
  f.dof = true, f.gain = 1, f.focusD = 0, f.pupil = 6, f.strength = 1, f.maxArcmin = 20, f.guard = 0;
  Apply(f);
  const int act = Draw(r, 2);
  const vrfx::FxCb cb = cpass::FxBuild(2, sc.w, sc.h, true);
  vrfx::CPrep g;
  const bool readOk = ReadPrep(r, cb, g);
  float gmax = 0;
  bool finite = true;
  for (size_t i = 0; i < g.px.size(); ++i)
    if (i % 4 != 3) finite = finite && std::isfinite(g.px[i]), gmax = std::max(gmax, g.px[i]);
  vrfx::CView view;
  view.w = sc.w, view.h = sc.h, view.rgb = r.rgbGpu, view.depth = r.depthDev;
  const vrfx::CPrep c = vrfx::Prep(view, cb, vrfx::ProjFromStored(r.projStored));
  bool cpuOk = c.px.size() == g.px.size();
  double worst = 0;
  for (size_t i = 0; cpuOk && i < c.px.size(); ++i) {
    if (i % 4 == 3) continue;
    cpuOk = std::isfinite(c.px[i]);
    worst = std::max(worst, std::fabs(static_cast<double>(g.px[i]) - c.px[i]) / std::max(std::fabs(static_cast<double>(c.px[i])), 1e-2));
  }
  const float* t = &g.px[(20 * g.hw + 10) * 4];
  printf("     (prep: max texel %.4g, the NaN's texel %.4g %.4g %.4g; GPU vs CPU %.2e relative; blur radius in its tile %.2f px)\n",
         gmax, t[0], t[1], t[2], worst, g.tiles.empty() ? 0.0f : g.tiles[(20 / 8) * g.tx + 10 / 8]);
  Check(act == 1 && readOk && cb.dofOn == 1 && finite && gmax < 1.0f && cpuOk && worst < 2e-3,
        "R77: vrfx DoF prep: a NaN HDR texel counts as black (no 65504 disc), GPU = CPU reference");
  cpass::g_obj.store(nullptr);
  cpass::g_rt = cpass::Rt{};
  for (auto& s2 : cpass::g_swaps) s2 = 0;
  for (auto& s2 : cpass::g_rejects) s2 = 0;
  r.Release();
  Clear();
}

inline void Run() {
  PureTests();
  GpuTests();
  GuardBandTests();
  FxTargetsGrowOnly();
  GVisionR58();
  DofNanR77();
}

}  // namespace vfxtest
