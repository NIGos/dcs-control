// R132: OPT-IN release of DCS's idle DLSS feature sets ([DLSS] ReleaseIdleSets=1, src\dlss_views.h, "R132" section).
// The R130 fake NGX core (fake CreateFeature / EvaluateFeature / ReleaseFeature with the real prologues of
// _nvngx.dll 32.0.16.1742, patched by the real detour code), the R130 test clock and memory readings. Checks:
//   - the release key alone installs the three hooks (tracking without summaries);
//   - two sets, only the second evaluated: after N evaluations of each newest handle the first set is released, once;
//   - no release while the load is open, nor before N evaluations since a mission event;
//   - the guard before: an old handle evaluated after the newer set exists -> that set is refused, never released;
//   - the guard after: an evaluate of a released handle is refused (FeatureNotFound) and never reaches NGX;
//   - a DCS ReleaseFeature on a released handle is swallowed (success, NGX not called);
//   - a second load: the previous load's evaluated set and the new idle set go, one set per frame, oldest first;
//   - the set created while DCS closes is never touched; nothing after the shutdown signal;
//   - kill switch, key 0 (nothing changes), a fault in the release step latches off;
//   - payload stop: the persistent filters guard the released handles; a later payload adopts the guard block;
//     NGX reusing a released handle's address for a new feature un-guards it.
#pragma once

namespace r132test {

using namespace r130test;

int EvalOne(const Rig& rig, int i) {
  r122test::FakeParam p;
  return reinterpret_cast<dlssv::EvalFn>(rig.eval)(nullptr, &g_h[i], &p, nullptr);
}
// Frames of evaluations of handles [first, first+count) without moving the clock.
void EvalFrozen(const Rig& rig, int first, int count, int times) {
  for (int t = 0; t < times; ++t)
    for (int i = 0; i < count; ++i) EvalOne(rig, first + i);
}
int CountOf(const std::string& s, const char* what) {
  int n = 0;
  for (size_t at = s.find(what); at != std::string::npos; at = s.find(what, at + 1)) ++n;
  return n;
}
void TickRel(bool instrument, bool killed, bool release, int n = 300) {
  dlssv::Tick(0, 0, false, killed, instrument, release, n);
}

// Instrument=1, ReleaseIdleSets=0: the R130 scenario past N changes nothing (no release, no guard, no new line).
void KeyZero(const Rig& rig) {
  using namespace dlssv::life;
  Fresh(rig);
  StartReportCapture();
  TickRel(true, false, false);
  CreateSet(rig, 0);
  CreateSet(rig, 4);
  g_relCalls = 0;
  EvalHandles(rig, 4, 4, 400);
  Advance(10.0);
  TickRel(true, false, false);
  EvalHandles(rig, 4, 4, 400);
  const std::string log = StopReportCapture();
  Check(g_relCalls == 0 && g_ourSets.load() == 0 && !GuardAny() && !g_f[0].released.load() &&
            !Has(log, "dlss release") && Has(log, "release rule (observation only, N=300; newest set 2 min 400") &&
            Has(log, "set 1 eligible;"),
        "R132 ReleaseIdleSets=0: an eligible set is only reported (R130), nothing released, no guard, no release line");
}

// ReleaseIdleSets=1 alone: hooks, release of the never-evaluated first set, guards.
void Basic(const Rig& rig) {
  using namespace dlssv;
  using namespace dlssv::life;
  Fresh(rig);
  StartReportCapture();
  TickRel(false, false, true);
  const std::string inst = StopReportCapture();
  Check(g_createState.load() == 1 && g_evalState.load() == 1 && g_releaseState.load() == 1 && g_on.load() &&
            g_relOn.load() && GuardEnsure() && Has(inst, "dlss release: on ([DLSS] ReleaseIdleSets=1, ReleaseAfterEvals=300)") &&
            Has(inst, "NVSDK_NGX_D3D11_EvaluateFeature hooked (earlier payload's patch reused; lifetime tracking for [DLSS] ReleaseIdleSets)") &&
            !Has(inst, "dlss life: on ([DLSS] Instrument=1"),
        "R132 ReleaseIdleSets=1 alone: CreateFeature, EvaluateFeature, ReleaseFeature hooked; tracking on without Instrument");

  StartReportCapture();
  CreateSet(rig, 0);
  CreateSet(rig, 4);
  g_relCalls = 0;
  g_evalCalls = 0;
  // A load is open (2 s since the last creation): 400 frames on a frozen clock release nothing.
  EvalFrozen(rig, 4, 4, 400);
  const bool noneInLoad = g_relCalls == 0 && g_ourSets.load() == 0;
  // A mission event: the proof starts again; 299 frames after it are not enough.
  Advance(2.5);
  NoteMissionEvent();
  EvalHandles(rig, 4, 4, 299);
  const bool noneBeforeN = g_relCalls == 0 && g_ourSets.load() == 0;
  EvalHandles(rig, 4, 4, 1);
  const bool releasedAtN = g_relCalls == 4 && g_ourSets.load() == 1 && g_ourFeats.load() == 4;
  EvalHandles(rig, 4, 4, 1000);
  Advance(10.0);
  TickRel(false, false, true);
  EvalHandles(rig, 4, 4, 100);
  const std::string log = StopReportCapture();
  Check(noneInLoad, "R132 no release while the load is open (newest set evaluated N times within 2 s of its creation)");
  Check(noneBeforeN, "R132 no release before N evaluations of each newest handle since the last mission event");
  bool flags = true;
  for (int i = 0; i < 8; ++i) flags = flags && g_f[i].released.load() == (i < 4) && g_f[i].ours.load() == (i < 4);
  Check(releasedAtN && flags && g_relCalls == 4 && g_ourSets.load() == 1 && CountOf(log, "dlss release: released set") == 1,
        "R132 two sets, only the second evaluated: after N the first set is released, once (NGX called 4 times)");
  Check(Has(log, "dlss release: released set 1 (load 1; ids 1-4; P 0x") && Has(log, "never evaluated)") &&
            Has(log, "newest set 2 has >= 300 evaluations per handle since the last mission event (min 300)") &&
            Has(log, "private +0.0 MB, DLSS stats +0.0 MB; 1 set(s) / 4 feature(s) released this session"),
        "R132 release line: set, handles, why (idle, never evaluated, newest set's proof) and the memory deltas");
  Check(!Has(log, "dlss life: summary") && !Has(log, "release rule ("), "R132 ReleaseIdleSets alone: no R130 summaries");
  Check(GuardHas(&g_h[0]) && GuardHas(&g_h[3]) && !GuardHas(&g_h[4]), "R132 the released handles are in the guard block");

  // Guard after: DCS evaluates a released handle -> refused before NGX, releasing off for the session.
  const int calls0 = g_evalCalls;
  StartReportCapture();
  const int r = EvalOne(rig, 1);
  const std::string ga = StopReportCapture();
  Check(r == kNgxFeatureNotFound && g_evalCalls == calls0 && g_refusedEvals.load() == 1 && g_relLatched.load() &&
            !g_relOn.load() && Has(ga, "dlss release: DCS evaluated handle 0x") && Has(ga, "(id 2, set 1)") &&
            Has(ga, "refused before NGX (0xbad00004 FeatureNotFound)") &&
            Has(ga, "dlss release: OFF for this session: DCS evaluated a released handle (guard after)"),
        "R132 guard after: an evaluate of a released handle is refused (FeatureNotFound), not forwarded; releasing latched off");
  const int r2 = EvalOne(rig, 5);
  Check(r2 == kNgxSuccess && g_evalCalls == calls0 + 1, "R132 guard after: live handles still evaluate normally");

  // DCS releases a handle we released: swallowed.
  const int rel0 = g_relCalls;
  StartReportCapture();
  const int rr = reinterpret_cast<ReleaseFn>(rig.rel)(&g_h[2]);
  const std::string sw = StopReportCapture();
  Check(rr == kNgxSuccess && g_relCalls == rel0 && g_swallowed.load() == 1 && g_releases.load() == 0 &&
            Has(sw, "DCS released handle 0x") && Has(sw, "(id 3, set 1) that DcsQvCull had released: swallowed"),
        "R132 a DCS ReleaseFeature on a handle we released is swallowed (success, NGX not called: no double free)");
  const int rr2 = reinterpret_cast<ReleaseFn>(rig.rel)(&g_h[6]);
  Check(rr2 == kNgxSuccess && g_relCalls == rel0 + 1 && g_f[6].released.load() && !g_f[6].ours.load(),
        "R132 a DCS ReleaseFeature on a live handle still reaches NGX (R130 bookkeeping)");
}

// Guard before: an old handle evaluated after the newer set exists -> that set is refused for the session.
void GuardBefore(const Rig& rig) {
  using namespace dlssv;
  using namespace dlssv::life;
  Fresh(rig);
  TickRel(false, false, true);
  StartReportCapture();
  CreateSet(rig, 0);
  CreateSet(rig, 4);
  g_relCalls = 0;
  EvalHandles(rig, 4, 4, 10);
  EvalHandles(rig, 2, 1, 1);  // DCS still uses id 3 (set 1)
  EvalHandles(rig, 4, 4, 600);
  const std::string log = StopReportCapture();
  Check(g_relCalls == 0 && g_ourSets.load() == 0 && g_refusedSets.load() == 1 && !g_f[0].released.load() &&
            g_f[0].refused && !GuardAny() &&
            Has(log, "dlss release: set 1 (load 1; ids 1-4) REFUSED for this session (guard before): 1 evaluation(s) after set 2") &&
            CountOf(log, "REFUSED") == 1 && !g_relLatched.load(),
        "R132 guard before: an old handle evaluated after the newer set exists -> that set is refused (logged once), no release");
}

// A second load, the close set, shutdown, kill switch, ReleaseAfterEvals.
void Loads(const Rig& rig) {
  using namespace dlssv;
  using namespace dlssv::life;
  Fresh(rig);
  StartReportCapture();
  TickRel(false, false, true, 100);
  CreateSet(rig, 0);
  CreateSet(rig, 4);
  g_relCalls = 0;
  EvalHandles(rig, 4, 4, 300);
  const bool load1 = g_ourSets.load() == 1 && g_f[0].ours.load() && !g_f[4].ours.load();
  // Restart: the Lua hook's onMissionLoadBegin, then DCS creates sets 3 and 4 (set 2 was evaluated in load 1).
  Advance(2.0);
  NoteMissionEvent();
  Advance(3.0);
  CreateSet(rig, 8);
  EvalFrozen(rig, 8, 4, 50);  // (DCS never does this; the proof counts only the newest set)
  CreateSet(rig, 12);
  const bool noneInLoad2 = g_ourSets.load() == 1;
  Advance(2.5);  // the load closes
  EvalHandles(rig, 12, 4, 99);
  const bool noneBeforeN = g_ourSets.load() == 1;
  Check(load1 && noneInLoad2 && noneBeforeN, "R132 ReleaseAfterEvals=100: set 1 after 100; nothing during load 2 nor before N");
  // N reached: set 2 (the previous load's evaluated set) goes first, set 3 at the next frame; one per frame.
  EvalHandles(rig, 12, 4, 1);
  const uint64_t afterFirst = g_ourSets.load();
  EvalHandles(rig, 12, 1, 3);  // three more evaluates: not yet a frame
  const uint64_t notAFrame = g_ourSets.load();
  EvalHandles(rig, 12, 4, 2);
  const std::string log = StopReportCapture();
  Check(afterFirst == 2 && notAFrame == 2 && g_ourSets.load() == 3 && g_f[4].ours.load() && g_f[8].ours.load() &&
            !g_f[12].ours.load() && g_relCalls == 12,
        "R132 second load: the previous load's set and the new idle set are released, one set per frame");
  const size_t a2 = log.find("released set 2 (load 1; ids 5-8"), a3 = log.find("released set 3 (load 2; ids 9-12");
  Check(a2 != std::string::npos && a3 != std::string::npos && a2 < a3 && Has(log, "1200 evaluations before (4 handles), last") &&
            Has(log, "newest set 4 has >= 100 evaluations per handle") && Has(log, "dlss release: status at a mission load: 1 set(s)"),
        "R132 second load: oldest first; the evaluated set's line gives its evaluations before; a status line at the load");

  // DCS closes: a set of four created, never evaluated. Nothing older goes after it (no proof), nor the set itself.
  Advance(30.0);
  StartReportCapture();
  NoteMissionEvent();  // onSimulationStop
  CreateSet(rig, 16);
  Advance(10.0);
  TickRel(false, false, true, 100);
  EvalHandles(rig, 12, 4, 400);  // even if set 4 were still evaluated (DCS does not): not the newest set
  const bool closeKept = g_ourSets.load() == 3 && !g_f[16].ours.load() && !g_f[12].ours.load();
  NoteNoMore("at DCS's shutdown");
  EvalHandles(rig, 16, 4, 400);  // pretend: the close set evaluated after the shutdown signal
  TickRel(false, false, true, 100);
  const std::string close = StopReportCapture();
  Check(closeKept && g_ourSets.load() == 3 && !g_f[12].ours.load() && !g_f[16].ours.load() && g_relCalls == 12 &&
            !g_relOn.load() && Has(close, "dlss release: status at DCS's shutdown: 3 set(s) / 12 feature(s) released") &&
            Has(close, "0 evaluation(s) of a released handle refused") && Has(close, "stopped"),
        "R132 the close set is never touched; nothing is released after the shutdown signal (status line)");

  // Kill switch: no release; the guard stays.
  Fresh(rig);
  TickRel(false, false, true);
  CreateSet(rig, 0);
  CreateSet(rig, 4);
  EvalHandles(rig, 4, 4, 300);
  const bool rel1 = g_ourSets.load() == 1;
  Advance(5.0);
  CreateSet(rig, 8);
  CreateSet(rig, 12);
  Advance(2.5);
  StartReportCapture();
  TickRel(false, true, true);  // kill switch
  EvalHandles(rig, 12, 4, 600);
  const bool killNone = g_ourSets.load() == 1;
  TickRel(false, false, true);
  TickRel(false, false, false);  // key 0
  EvalHandles(rig, 12, 4, 600);
  const std::string kl = StopReportCapture();
  Check(rel1 && killNone && g_ourSets.load() == 1 && Has(kl, "dlss release: paused (kill switch)") &&
            Has(kl, "dlss release: resumed") && Has(kl, "dlss release: paused ([DLSS] ReleaseIdleSets=0)") && !g_on.load(),
        "R132 kill switch and ReleaseIdleSets=0: no release (newest set evaluated 600 frames each time)");
  TickRel(false, false, true);
  EvalHandles(rig, 12, 4, 400);
  Check(g_ourSets.load() == 3 && g_f[4].ours.load() && g_f[8].ours.load(), "R132 key back to 1: releasing resumes (sets 2 and 3)");
  TickRel(false, true, true);
  const int refused = EvalOne(rig, 0);
  Check(refused == kNgxFeatureNotFound && g_refusedEvals.load() == 1, "R132 kill switch: the released handles stay guarded");
}

// A DCS creation inside NGX on another thread (or an evaluate on another thread) at the release moment: no release
// beside it (retried at the next evaluate); the handles are un-guarded again meanwhile.
void Busy(const Rig& rig) {
  using namespace dlssv;
  using namespace dlssv::life;
  Fresh(rig);
  TickRel(false, false, true);
  CreateSet(rig, 0);
  CreateSet(rig, 4);
  g_relCalls = 0;
  g_creating = 1;  // stands for DCS's loading thread inside CreateFeature
  EvalHandles(rig, 4, 4, 400);
  const bool waitC = g_ourSets.load() == 0 && g_relCalls == 0 && g_relBusy.load() > 0 && !GuardHas(&g_h[0]);
  g_creating = 0;
  g_inFlight = 1;  // another thread inside EvaluateFeature
  EvalHandles(rig, 4, 4, 2);
  const bool waitE = g_ourSets.load() == 0 && g_relCalls == 0;
  g_inFlight = 0;
  EvalHandles(rig, 4, 4, 1);
  Check(waitC && waitE && g_ourSets.load() == 1 && g_relCalls == 4 && !g_relLatched.load() && g_releasing.load() == 0,
        "R132 no release beside a creation or an evaluate inside NGX on another thread; it follows once they are out");
}

// A fault in the release step latches off; the handles it guarded stay guarded.
void Fault(const Rig& rig) {
  using namespace dlssv;
  using namespace dlssv::life;
  Fresh(rig);
  TickRel(false, false, true);
  CreateSet(rig, 0);
  CreateSet(rig, 4);
  g_relCalls = 0;
  g_testFaultOn = &g_h[2];
  StartReportCapture();
  EvalHandles(rig, 4, 4, 400);
  const std::string log = StopReportCapture();
  g_testFaultOn = nullptr;
  Check(g_relFaults.load() == 1 && g_relLatched.load() && !g_relOn.load() && g_ourSets.load() == 0 && g_relCalls == 2 &&
            GuardHas(&g_h[2]) && EvalOne(rig, 2) == kNgxFeatureNotFound &&
            Has(log, "dlss release: OFF for this session: a fault in the release step (SEH)"),
        "R132 a fault in the release step is contained, latches releasing off, the set's handles stay guarded");
  TickRel(false, false, true);
  Check(!g_relOn.load(), "R132 latched: the worker does not turn releasing back on");
}

// Payload stop: the persistent filters; a later payload adopts the block; NGX reusing an address un-guards it.
void StopAndAdopt(const Rig& rig) {
  using namespace dlssv;
  using namespace dlssv::life;
  Fresh(rig);
  TickRel(false, false, true);
  CreateSet(rig, 0);
  CreateSet(rig, 4);
  EvalHandles(rig, 4, 4, 300);
  const bool rel = g_ourSets.load() == 1;
  uint8_t* const block = g_guardBase;
  Stop();
  void* evalSlot = *g_eval.slot;
  const int e0 = g_evalCalls, r0 = g_relCalls;
  const int refused = EvalOne(rig, 0);
  const int live = EvalOne(rig, 5);
  const int swallowed = reinterpret_cast<ReleaseFn>(rig.rel)(&g_h[1]);
  const int passed = reinterpret_cast<ReleaseFn>(rig.rel)(&g_h[7]);
  Check(rel && evalSlot == block + kEvalFilterOff && refused == kNgxFeatureNotFound && live == kNgxSuccess &&
            g_evalCalls == e0 + 1 && swallowed == kNgxSuccess && passed == kNgxSuccess && g_relCalls == r0 + 1,
        "R132 payload stop: the persistent filters refuse / swallow the released handles and pass the others to NGX");

  // "Next payload": fresh globals, the environment still names the block.
  wchar_t env[32] = {};
  GetEnvironmentVariableW(kGuardEnv, env, 32);
  Fresh(rig);
  SetEnvironmentVariableW(kGuardEnv, env);
  StartReportCapture();
  TickRel(true, false, false);  // the later payload has only Instrument=1 (no release key)
  const int refused2 = EvalOne(rig, 0);
  const std::string adopt = StopReportCapture();
  Check(g_guardBase == block && *g_eval.slot == reinterpret_cast<void*>(&EvalHook) && refused2 == kNgxFeatureNotFound &&
            g_refusedEvals.load() == 1 && Has(adopt, "guard block of an earlier payload adopted (4 released handle(s) still guarded)") &&
            Has(adopt, "EvaluateFeature hooked (earlier payload's patch reused"),
        "R132 a later payload (Instrument only) adopts the guard block: the released handles stay refused");
  // NGX gives a released handle's address to a new feature (the fake core returns ctx = &g_h[i] as the handle).
  StartReportCapture();
  Advance(5.0);
  CreateSet(rig, 0);
  const std::string reuse = StopReportCapture();
  const int e1 = g_evalCalls;
  const int now = EvalOne(rig, 0);
  Check(!GuardHas(&g_h[0]) && !GuardHas(&g_h[3]) && now == kNgxSuccess && g_evalCalls == e1 + 1 &&
            CountOf(reuse, "NGX reused the address") == 4,
        "R132 a new creation at a released handle's address un-guards it (DCS's new feature evaluates normally)");
  Stop();
  SetEnvironmentVariableW(kGuardEnv, nullptr);
}

// The persistent filter's machine code alone (both registers, hit / miss / cleared slot / empty list).
void FilterCode(const Rig& rig) {
  using namespace dlssv;
  using namespace dlssv::life;
  Fresh(rig);
  TickRel(false, false, true);  // hooks installed, block created
  GuardData* d = Gd();
  const auto ef = reinterpret_cast<EvalFn>(g_guardBase + kEvalFilterOff);
  const auto rf = reinterpret_cast<ReleaseFn>(g_guardBase + kRelFilterOff);
  r122test::FakeParam p;
  const int e0 = g_evalCalls, r0 = g_relCalls;
  const int a = ef(nullptr, &g_h[9], &p, nullptr);  // empty list: through to NGX (the trampoline)
  d->h[0] = &g_h[9];
  d->h[1] = &g_h[10];
  d->count = 2;
  const int b = ef(nullptr, &g_h[10], &p, nullptr), c = ef(nullptr, &g_h[11], &p, nullptr);
  const int x = rf(&g_h[9]), y = rf(&g_h[11]);
  d->h[1] = kTomb;
  const int z = ef(nullptr, &g_h[10], &p, nullptr);
  d->count = 0;
  Check(a == kNgxSuccess && b == kNgxFeatureNotFound && c == kNgxSuccess && x == kNgxSuccess && y == kNgxSuccess &&
            z == kNgxSuccess && g_evalCalls == e0 + 3 && g_relCalls == r0 + 1,
        "R132 persistent filter code: listed handle refused / swallowed, others and cleared slots pass to the trampoline");
}

void Run() {
  printf("-- R132 DLSS idle-set release (opt-in) --\n");
  const Rig rig = MakeRig();
  if (!rig.ok()) {
    QvSkip("R132: no executable memory\n");
    return;
  }
  KeyZero(rig);
  Basic(rig);
  GuardBefore(rig);
  Loads(rig);
  Busy(rig);
  Fault(rig);
  FilterCode(rig);
  StopAndAdopt(rig);
  dlssv::life::ResetForTest();
  r122test::NewGeneration();
}

}  // namespace r132test
