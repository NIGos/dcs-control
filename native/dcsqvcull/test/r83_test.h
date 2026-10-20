// R83 (R82 D1): no false "MISSED HMD tone-map swaps" at VrFx transitions in Mode 0 (color_pass.h TonePassBegin /
// ToneDrawPre decided "due" and "swap" from different effect states: the frame's FxStep runs inside the first view's
// ToneDrawPre, after TonePassBegin). Real misses are still counted, and logged with the frame's mode.
#pragma once

namespace r83 {

using vfxrig::Rig;

struct Counts {
  uint64_t miss = 0, swaps = 0;
};
inline Counts Now() {
  Counts c;
  c.miss = cpass::g_missTotal.load();
  for (auto& s : cpass::g_swaps) c.swaps += s.load();
  return c;
}

// One HMD frame as the glue drives it (fixation_depth.h ColorBoundary): per view TonePassBegin, the armed tone-map
// draw through ToneDrawPre / ToneDrawPost, TonePassEnd. beforeDraw(v) runs between a view's pass begin and its draw
// (the config thread publishing an ini change mid-frame). 30 fps as in R82 (FG 90/3 cap), 1 G parked.
template <class F>
inline void Frame(Rig& r, double& modelT, F beforeDraw) {
  modelT += 1 / 30.0;
  vrfx::NoteG(0, 1, 0, modelT);
  for (int v = 0; v < 4; ++v) {
    cpass::TonePassBegin(v);
    cpass::TonePassArmed(true);
    beforeDraw(v);
    vfxrig::Draw(r, v);
    cpass::TonePassEnd();
  }
  Sleep(33);
}
inline void Frame(Rig& r, double& modelT) {
  Frame(r, modelT, [](int) {});
}
inline void Frames(Rig& r, double& modelT, int n) {
  for (int i = 0; i < n; ++i) Frame(r, modelT);
}

inline void SetParams(bool dof, bool gv) {
  vrfx::Params p;
  p.dof = dof, p.gv = gv;
  while (vrfx::g_paramLock.test_and_set()) {
  }
  vrfx::g_params = p;
  vrfx::g_paramLock.clear();
  vrfx::g_paramVer++;
}

inline void ToggleStormD1() {
  vfxrig::Scene sc;
  sc.w = 64, sc.h = 48;
  sc.l = -0.06f, sc.r = 0.06f, sc.u = 0.045f, sc.d = -0.045f;
  sc.adapted = 0.05f, sc.ec = 0.2f;
  sc.rgb.assign(sc.w * sc.h * 3, 0.2f);
  sc.rangeM.assign(sc.w * sc.h, 2.0f);
  Rig r;
  std::string err;
  if (!vfxrig::Make(r, sc, err)) {
    printf("     %s\n", err.c_str());
    Check(false, "R83 D1: WARP rig");
    r.Release();
    vfxrig::Clear();
    return;
  }
  vfxrig::Clear();  // g_fxHold off: the real FxStep / FxFrame / FxIdle path
  cpass::g_rt = cpass::Rt{};
  cpass::g_tp = cpass::TonePass{};
  cpass::g_fault = false;
  cpass::g_mode = 0;
  for (auto& m : cpass::g_miss) m = 0;
  for (auto& w : cpass::g_missWhy) w = 0;
  cpass::g_missTotal = 0, cpass::g_missPending = 0, cpass::g_missMode = -1;
  SetParams(true, true);  // [VrFx] GazeDof / GVision enabled in the ini; the R82 toggles flip what is wanted
  double t = 100.0;

  // The D1 sequence of R82 seed 8221 (steps 58, 155, 375-378; toggles held >= 0.3 s, the storm's shortest gap).
  // 1. Mode 0, VrFx off: DCS's tone map, nothing due.
  Frames(r, t, 3);
  const Counts c0 = Now();
  // 2. VrFx on (GazeDof 1, GVision 1) in Mode 0: our shader with DCS's tone map from the first frame.
  vrfx::g_wantDof = true, vrfx::g_wantGv = true;
  Frames(r, t, 9);
  const Counts c1 = Now();
  const bool fxSwapped = c1.swaps - c0.swaps == 36 && cpass::FxActive();
  // 3. Step 58: GazeDof 0 with GVision 1 at 1 G (a neutral shape). View 0's pass began with the previous frame's
  //    effects (active); its draw steps this frame's (inactive): DCS's tone map by design. The old code counted a miss.
  vrfx::g_wantDof = false;
  Frames(r, t, 9);
  const Counts c2 = Now();
  const bool offNoMiss = c2.miss == c1.miss && c2.swaps == c1.swaps && !cpass::FxActive();
  // 4. Steps 375-376: GazeDof 1, GVision 0, then GazeDof 0 published between view 2's pass begin and its draw.
  vrfx::g_wantDof = true, vrfx::g_wantGv = false;
  Frames(r, t, 9);
  const Counts c3 = Now();
  Frame(r, t, [](int v) {
    if (v == 2) vrfx::g_wantDof = false;
  });
  Frames(r, t, 2);
  const Counts c4 = Now();
  const bool midFrameNoMiss = c3.swaps > c2.swaps && c4.miss == c3.miss && c4.swaps - c3.swaps == 2;
  // 5. Step 378: Mode 2 (Enhanced) after the off: every view swapped, no miss.
  cpass::g_mode = 2;
  Frames(r, t, 3);
  const Counts c5 = Now();
  const bool modeNoMiss = c5.miss == c4.miss && c5.swaps - c4.swaps == 12;
  printf("     (D1 replay: misses %llu/%llu/%llu/%llu/%llu, swaps per phase %llu/%llu/%llu/%llu/%llu)\n",
         static_cast<unsigned long long>(c0.miss), static_cast<unsigned long long>(c1.miss),
         static_cast<unsigned long long>(c2.miss), static_cast<unsigned long long>(c4.miss),
         static_cast<unsigned long long>(c5.miss), static_cast<unsigned long long>(c0.swaps),
         static_cast<unsigned long long>(c1.swaps - c0.swaps), static_cast<unsigned long long>(c2.swaps - c1.swaps),
         static_cast<unsigned long long>(c4.swaps - c2.swaps), static_cast<unsigned long long>(c5.swaps - c4.swaps));
  Check(c0.miss == 0 && c0.swaps == 0 && fxSwapped && offNoMiss && midFrameNoMiss && modeNoMiss && c5.miss == 0,
        "R83 D1: Mode 0 -> VrFx on -> VrFx off (and a mid-frame flip) -> Mode 2: no false MISSED tone-map swaps");

  // Real misses still count, labelled with the frame's mode, not the mode at log time.
  cpass::g_mode = 0;
  vrfx::g_wantDof = true;
  Frames(r, t, 2);
  const uint64_t m0 = cpass::g_missTotal.load(), rj0 = cpass::g_missWhy[cpass::kMissReject].load(),
                 ar0 = cpass::g_missWhy[cpass::kMissArm].load();
  // Effects due but our effect shader unavailable at the draw: DCS's tone map without the effect = a miss.
  ID3D11PixelShader* keepFx = r.obj.psFx;
  r.obj.psFx = nullptr;
  Frame(r, t, [](int) {});
  r.obj.psFx = keepFx;
  const bool fxMiss = cpass::g_missTotal.load() == m0 + 4 && cpass::g_missWhy[cpass::kMissReject].load() == rj0 + 4;
  cpass::g_mode = 2;  // the config thread switches to Enhanced before it logs the miss
  const bool label0 = cpass::g_missMode.load() == 0;
  // Arm refused in Mode 2: no draw at all.
  cpass::TonePassBegin(1);
  cpass::TonePassArmed(false);
  cpass::TonePassEnd();
  const bool armMiss = cpass::g_missTotal.load() == m0 + 5 && cpass::g_missWhy[cpass::kMissArm].load() == ar0 + 1 &&
                       cpass::g_missMode.load() == 2;
  Check(fxMiss && label0 && armMiss,
        "R83 D1: real misses still counted (Mode 0 + VrFx effect not drawn, Mode 2 arm refused), labelled with the frame's mode");

  cpass::g_mode = 0;
  cpass::g_obj.store(nullptr);
  cpass::g_rt = cpass::Rt{};
  cpass::g_tp = cpass::TonePass{};
  for (auto& s : cpass::g_swaps) s = 0;
  for (auto& s : cpass::g_rejects) s = 0;
  for (auto& m : cpass::g_miss) m = 0;
  for (auto& w : cpass::g_missWhy) w = 0;
  cpass::g_missTotal = 0, cpass::g_missPending = 0, cpass::g_missMode = 0, cpass::g_swapUnknown = 0;
  SetParams(false, false);
  r.Release();
  vfxrig::Clear();
}

inline void Run() {
  ToggleStormD1();
}

}  // namespace r83
