// R77 uncovered-bugfix round (native side): tests for the fixes that have no better home.
#pragma once

namespace r77 {

// rec-tex-table-pins-dcs-textures-while-inactive: the recorders' texture tables are wiped at the first render entry
// after a mission event and after the recorder goes inactive (never while a job reads them).
inline void TexIdle() {
  shrec::TexTable t;
  SRWLOCK lock = SRWLOCK_INIT;
  shrec::TexIdle s;
  const bool alloc = shrec::AllocTex(t, 64);
  alignas(16) static uint8_t tex[8][16];
  auto fill = [&] {
    for (int i = 0; i < 8; ++i) {
      bool fresh = false;
      shrec::TexInsert(t, tex[i], -1, 0, &fresh);
    }
  };
  fill();
  const uint32_t live0 = t.live;
  const bool keepActive = !shrec::TexIdleStep(t, lock, s, true) && t.live == live0;
  const bool offWipes = shrec::TexIdleStep(t, lock, s, false) && t.live == 0;
  fill();
  const bool staysOff = !shrec::TexIdleStep(t, lock, s, false) && t.live == live0;  // once per transition
  s.eventGen.fetch_add(1);
  AcquireSRWLockShared(&lock);  // a job reads the table: retried at the next entry
  const bool busyWaits = !shrec::TexIdleStep(t, lock, s, true) && t.live == live0;
  ReleaseSRWLockShared(&lock);
  const bool eventWipes = shrec::TexIdleStep(t, lock, s, true) && t.live == 0;
  fill();
  const bool thenKeeps = !shrec::TexIdleStep(t, lock, s, true) && t.live == live0;
  Check(alloc && live0 == 8 && keepActive && offWipes && staysOff && busyWaits && eventWipes && thenKeeps && s.wipes == 2,
        "R77: recorder texture tables released after the recorder goes inactive and at a mission event (lock-safe)");
  shrec::FreeTex(t);
  // The modules' own triggers are the ones main.cpp's OnMissionEvent bumps.
  const uint32_t g0 = gbrec::g_texIdle.eventGen.load(), s0 = shrec::g_texIdle.eventGen.load();
  gbrec::NoteMissionEvent();
  shrec::NoteMissionEvent();
  Check(gbrec::g_texIdle.eventGen.load() == g0 + 1 && shrec::g_texIdle.eventGen.load() == s0 + 1,
        "R77: gbrec / shrec NoteMissionEvent bump their texture-table wipe generation");
}

// suite-engine-restore-ignores-killswitch: the all-optimizations A/B (bench mode 11) leaves the kill switch as the
// user had it (also when aborted).
inline void BenchKillSwitch() {
  const bool saved = g_engineOff.load();
  const int blocks0 = g_cfg.benchBlocks, sec0 = g_cfg.benchBlockSec;
  const auto settle0 = g_cfg.benchSettleSec;
  g_cfg.benchBlocks = 4, g_cfg.benchBlockSec = 2, g_cfg.benchSettleSec = 0;
  auto run = [](bool offBefore) {
    SetEngineOff(offBefore);
    std::thread ab([] {
      Sleep(150);
      g_benchAbort = true;
    });
    RunBenchmark(11, false);
    ab.join();
    return g_engineOff.load();
  };
  const bool keptOff = run(true), keptOn = !run(false);
  Check(keptOff && keptOn, "R77: bench mode 11 (all optimizations A/B) restores the kill switch as it was (OFF stays OFF)");
  g_cfg.benchBlocks = blocks0, g_cfg.benchBlockSec = sec0, g_cfg.benchSettleSec = settle0;
  g_benchAbort = false;
  SetEngineOff(saved);
}

// cmask-boostshift-from-applied-h: the depth companion's boostShiftM is the boost record DCS renders with
// (Frame::boostH), also in a frame the dual camera left unmoved (h 0: paused, inert, limit, off tail); NaN with the
// boost flag clear when the frame decided no record.
inline void DepthBoostShift() {
  namespace ck = fixdepth::ckipd;
  ck::Frame paused;
  paused.latched = true, paused.h = 0, paused.boostH = 0.0131;
  float s0 = NAN, s1 = NAN, su = NAN, sn = NAN;
  uint32_t f0 = 0, f1 = 0, fu = 0, fn = 0;
  fixdepth::cmask::FrameBoost(paused, 0, &s0, &f0);
  fixdepth::cmask::FrameBoost(paused, 1, &s1, &f1);
  ck::Frame unknown;
  unknown.latched = true;  // e.g. an off frame (offNext / foreign hooks / gPrev refuted): no record latched
  fixdepth::cmask::FrameBoost(unknown, 2, &su, &fu);
  ck::Frame none;  // no cockpit latch (external view)
  none.boostH = 0.0131;
  fixdepth::cmask::FrameBoost(none, 3, &sn, &fn);
  namespace cdp = ::dcsvr_cockpit_depth;
  Check(std::fabs(s0 + 0.0131f) < 1e-7f && std::fabs(s1 - 0.0131f) < 1e-7f && (f0 & cdp::view_flags::boost) &&
            (f1 & cdp::view_flags::boost) && std::isnan(su) && !(fu & cdp::view_flags::boost) && std::isnan(sn) &&
            !(fn & cdp::view_flags::boost),
        "R77: cockpit depth boostShiftM = the frame's boost record (-h left, +h right) even with h 0 applied; unknown: "
        "NaN, boost flag clear");
}

// GitHub #32: on a stereo runtime (no focus / peripheral pairs, so BenchOnFrame never counts a frame) the A/B bench
// compares whole frames instead of stopping "nothing to compare", and says so in its report.
inline void BenchStereo() {
  const int blocks0 = g_cfg.benchBlocks, sec0 = g_cfg.benchBlockSec;
  const auto settle0 = g_cfg.benchSettleSec;
  const bool enabled0 = g_enabled.load(), focused0 = g_xrFocused.load();
  g_xrFocused = true;
  g_cfg.benchBlocks = 4, g_cfg.benchBlockSec = 2, g_cfg.benchSettleSec = 0;
  g_benchAbort = false;
  std::atomic<bool> stop{false};
  std::thread frames([&] {  // the collect hook's main call of a stereo frame, about 200 fps
    while (!stop.load()) {
      LARGE_INTEGER q;
      QueryPerformanceCounter(&q);
      BenchOnWholeFrame(q.QuadPart, 300);
      Sleep(5);
    }
  });
  {
    std::lock_guard<std::mutex> lock(g_logMutex);
    g_reportCapture = true;
    g_reportThread = GetCurrentThreadId();
    g_reportBuf.clear();
  }
  const bool completed = RunBenchmark(0, false);
  std::string captured;
  {
    std::lock_guard<std::mutex> lock(g_logMutex);
    g_reportCapture = false;
    captured.swap(g_reportBuf);
  }
  stop = true;
  frames.join();
  const bool note = captured.find("whole-frame comparison") != std::string::npos &&
                    captured.find("comparing whole frames") != std::string::npos;
  const bool noStop = captured.find("nothing to compare") == std::string::npos;
  const bool fps = captured.find("  fps ") != std::string::npos &&
                   captured.find("process_cpu_ms/frame") != std::string::npos;
  const bool noQuadFields = captured.find("periph_renderables") == std::string::npos;
  const bool blocksCounted = captured.find("frames=0") == std::string::npos;
  Check(completed && note && noStop && fps && noQuadFields && blocksCounted,
        "bench on a stereo runtime: whole-frame fallback (fps, frame time, process CPU per block), the report says so, "
        "no 'nothing to compare' stop");
  g_cfg.benchBlocks = blocks0, g_cfg.benchBlockSec = sec0, g_cfg.benchSettleSec = settle0;
  g_enabled = enabled0;
  g_xrFocused = focused0;
  g_benchAbort = false;
}

inline void Run() {
  TexIdle();
  DepthBoostShift();
  BenchKillSwitch();
  BenchStereo();
}

}  // namespace r77
