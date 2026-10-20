// R62 vrfx-state-stale-when-both-off: VrFx G / DoF state across an all-off gap (color_pass.h FxIdle).
#pragma once

namespace r62fx {

void VrFxGap() {
  // vrfx-state-stale-when-both-off: GVision off with GazeDof off, back on later in the same mission -> no stale
  // blackout.
  using namespace vrfx;
  Params p;
  p.gv = 1;
  while (g_paramLock.test_and_set()) {
  }
  g_params = p;
  g_paramLock.clear();
  g_paramVer++;
  const bool wantDof0 = g_wantDof.load(), wantGv0 = g_wantGv.load();
  g_wantDof = false;
  g_wantGv = true;
  cpass::FxRt& f = cpass::g_fx;
  NoteG(0, 1, 0, 10.0);
  cpass::FxStep(false, 0);  // takes the parameters
  f.g.loss = 1.2, f.g.red = 0;  // a blackout at the moment of the switch
  NoteG(0, 9, 0, 11.0);
  cpass::FxStep(false, 0);
  const bool black = f.gvOn && !Neutral(f.shape);
  g_wantGv = false;  // both effects off: FxFrame no longer runs
  cpass::FxStep(false, 0);
  cpass::FxStep(false, 0);
  const bool idle = !f.gvOn && f.g.loss == 0 && !cpass::FxActive();
  Sleep(20);
  g_wantGv = true;  // back on, same mission, level 1 G
  NoteG(0, 1, 0, 12.0);
  cpass::FxStep(false, 0);
  const bool clean = f.gvOn && f.g.loss < 0.05 && Neutral(f.shape) && !cpass::FxActive();
  Check(black && idle && clean,
        "R62 vrfx: GVision switched off (GazeDof off) during a blackout and on again later: no stale blackout");
  g_wantGv = wantGv0;
  g_wantDof = wantDof0;
  while (g_paramLock.test_and_set()) {
  }
  g_params = Params{};
  g_paramLock.clear();
  g_paramVer++;
  cpass::FxStep(false, 0);
}

void Run() {
  VrFxGap();
}

}  // namespace r62fx
