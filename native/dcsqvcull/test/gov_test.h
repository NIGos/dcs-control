// Offline tests for R30: the frame sampler, the CPU/GPU-bound detector on
// synthetic timings, the governor's hysteresis and stage transitions, the
// stage apply path (pause/resume through the kill-switch path, with the
// kill switch, the benchmark and ini changes), the G-buffer recorder's
// stale-start rule across a pause, the status word's R30 bits and the new
// log lines.
#pragma once

namespace govtest {

using namespace perfgov;

// ---- Frame sampler ----
void Sampler() {
  Init();
  g_frames = 0;
  g_tid = 0;
  g_maxCount = 0;
  g_mainCount = 0;
  g_otherThread = 0;
  g_lastQpc = 0;
  LARGE_INTEGER f;
  QueryPerformanceFrequency(&f);
  const int64_t ms = f.QuadPart / 1000;
  int64_t q = 1000 * ms;
  // 20 frames at 11.1 ms; a small call (MFD-sized) in between never counts.
  for (int i = 0; i < 20; ++i) {
    OnCollect(62, 4, 2, q);
    OnCollect(3, -1, -1, q + ms);  // below 4 collections
    q += static_cast<int64_t>(11.1 * ms);
  }
  std::vector<float> iv;
  uint64_t at = ReadIntervals(0, iv);
  bool ok = at == 20 && iv.size() == 19;
  for (float v : iv) ok &= std::fabs(v - 11.1) < 0.05;
  Check(ok && g_views.load() == 4 && g_pairs.load() == 2,
        "perf sampler: one frame per main collect call, 19 intervals of 11.1 ms, view layout kept");
  // A new window: calls under half the main size are not frames.
  RollWindow();
  Check(g_mainCount.load() == 62, "perf sampler: the window's largest call becomes the main-call size");
  OnCollect(20, 2, 0, q);
  Check(g_frames.load() == 20, "perf sampler: a call under half the main size is not a frame");
  // Live 2026-10-11: DCS's collect call runs on pool threads, a different one from frame to frame. Every
  // main call is a frame, whichever thread makes it (counting one thread's calls gave 9-12 fps at 62 fps).
  {
    const uint64_t f0 = g_frames.load();
    std::vector<float> iv2;
    ReadIntervals(f0, iv2);
    int64_t qq = q;
    for (int i = 0; i < 60; ++i) {
      qq += static_cast<int64_t>(16.1 * ms);
      std::thread t([&] { OnCollect(62, 4, 2, qq); });  // a new thread every frame
      t.join();
    }
    iv2.clear();
    ReadIntervals(f0, iv2);
    bool ok2 = g_frames.load() == f0 + 60 && iv2.size() == 60;
    for (size_t i = 1; i < iv2.size(); ++i) ok2 &= std::fabs(iv2[i] - 16.1) < 0.05;  // [0]: from the earlier frames
    double sum = 0;
    for (size_t i = 1; i < iv2.size(); ++i) sum += iv2[i];
    Check(ok2 && std::fabs(sum / (iv2.size() - 1) - 16.1) < 0.05 && g_otherThread.load() >= 59,
          "perf sampler: main calls from changing pool threads all count (62 fps stays 62 fps)");
    q = qq;
    at = g_frames.load();
  }
  // Stereo: 2 views, 0 pairs, still frames.
  const uint64_t fs = g_frames.load();
  OnCollect(40, 2, 0, q + 16 * ms);
  at = ReadIntervals(at, iv);
  Check(g_frames.load() == fs + 1 && g_views.load() == 2 && g_pairs.load() == 0,
        "perf sampler: stereo frames count (2 views, no pairs)");
  // Ring overrun: the reader keeps only the newest kRing - 64.
  for (uint32_t i = 0; i < kRing + 100; ++i) {
    q += 10 * ms;
    OnCollect(62, 4, 2, q);
  }
  iv.clear();
  ReadIntervals(at, iv);
  Check(iv.size() == kRing - 64, "perf sampler: a slow reader gets the newest kRing - 64 intervals");
  g_tid = 0;
  g_frames = 0;
  g_mainCount = 0;
  g_maxCount = 0;
}

// ---- Detector ----
void Detector() {
  Thresholds t;
  auto v = [&](uint32_t fr, double rt, double gpu) {
    Second s;
    s.frames = fr;
    s.renderPct = rt;
    s.gpuPct = gpu;
    return Classify(s, t);
  };
  // GPU-bound VR at 45 fps: GPU full, render thread waits most of the frame.
  Check(v(45, 55, 98) == kGpu, "detector: GPU full, render thread 55% busy -> GPU");
  // CPU-bound airfield: render thread busy all frame, GPU at 70%.
  Check(v(40, 97, 70) == kCpu, "detector: render thread 97%, GPU 70% -> CPU");
  Check(v(40, 95, 88) == kCpu, "detector: render thread 95%, GPU 88% -> CPU");
  // Both full: no clear limit.
  Check(v(40, 96, 97) == kMixed, "detector: both near full (GPU 97%) -> mixed");
  Check(v(67, 93, 99) == kGpu && v(67, 99, 98) == kGpu,
        "detector: GPU at 98% or more is GPU-bound even with a busy (spin-waiting) render thread (live airfield case)");
  // Frame cap / half-rate quantisation: neither full.
  Check(v(45, 50, 60) == kCapped, "detector: render thread 50%, GPU 60% -> capped");
  Check(v(90, 88, 60) == kCpu, "detector: GPU with headroom, render thread 88% -> CPU");
  Check(v(45, 88, 95) == kMixed, "detector: GPU full but the render thread 88% busy -> mixed");
  Check(v(45, 70, 85) == kMixed, "detector: between the thresholds -> mixed");
  Check(v(2, 50, 99) == kNone, "detector: under 3 frames (loading, menu) -> none");
  Check(v(45, -1, 99) == kNone, "detector: no render-thread reading -> none");
  Check(v(45, 95, -1) == kCpu && v(45, 60, -1) == kNone, "detector: no GPU reading: only a full render thread is CPU");
  // Thresholds are honoured.
  t.gpuBusyPct = 98;
  Check(v(45, 55, 97) == kMixed, "detector: GpuBusyPct=98 makes 97% not GPU-bound");
  t.gpuFullPct = 101;
  Check(v(67, 93, 99) == kMixed, "detector: GpuFullPct=101 turns the GPU-full rule off");
}

// ---- Stages ----
void Stages() {
  StageSettings s0 = ForStage(0, 0xf, 2), s1 = ForStage(1, 0xf, 2), s2 = ForStage(2, 0xf, 2), s3 = ForStage(3, 0xf, 2);
  Check(s0.shadowSplit == 0xf && s0.gbHelpers == 2 && !s0.pause, "stages: 0 = as configured");
  Check(s1.shadowSplit == 0x8 && s1.gbHelpers == 2 && !s1.pause, "stages: 1 = shadow split 0xf -> 0x8");
  Check(s2.shadowSplit == 0 && s2.gbHelpers == 0 && !s2.pause, "stages: 2 = no helper lists (split 0, G-buffer 0)");
  Check(s3.shadowSplit == 0 && s3.gbHelpers == 0 && s3.pause, "stages: 3 = recorders paused");
  Check(ForStage(1, 0x3, 2).shadowSplit == 0, "stages: a configured split without cascade 3 has no helpers in stage 1");
  Check(UsableStages(true, true, 0xf, 2) == 0xf, "stages: both recorders, split 0xf, helpers 2: all 4 stages");
  Check(UsableStages(true, false, 0x8, 2) == (1u | 4u | 8u),
        "stages: split already 0x8: stage 1 skipped (it would change nothing)");
  Check(UsableStages(false, true, 0xf, 2) == (1u | 4u | 8u), "stages: G-buffer recorder alone: 0, 2, 3");
  Check(UsableStages(false, true, 0xf, 0) == (1u | 8u), "stages: G-buffer recorder without helpers: 0, 3");
  Check(UsableStages(false, false, 0xf, 2) == 1u, "stages: no recorder: only stage 0");
}

// ---- Governor hysteresis and transitions ----
void Hysteresis() {
  GovConfig c;  // window 8, need 6, dwell 10
  Governor g;
  double t = 0;
  auto feed = [&](Verdict v, int n) {
    int changes = 0;
    for (int i = 0; i < n; ++i) {
      t += 1;
      changes += g.Tick(v, t, c, 0xf) != 0;
    }
    return changes;
  };
  g.Reset(0);
  // 5 GPU seconds of 8: not enough.
  feed(kGpu, 5);
  const int noise = feed(kCpu, 1) + feed(kGpu, 0) + feed(kMixed, 2);
  Check(noise == 0 && g.stage == 0, "governor: 5 of 8 s GPU-bound is not enough (noise ignored)");
  // Sustained GPU-bound: one step after 6 s, never before the dwell.
  g.Reset(t);
  int steps = feed(kGpu, 9);
  Check(steps == 0 && g.stage == 0, "governor: no step inside the minimum dwell (10 s) even when GPU-bound");
  steps = feed(kGpu, 1);
  Check(steps == 1 && g.stage == 1, "governor: GPU-bound for the window and the dwell -> stage 1");
  // The window restarts in the new stage: the next step needs 10 s more.
  steps = feed(kGpu, 9);
  Check(steps == 0 && g.stage == 1, "governor: stage 1 held for its dwell");
  steps = feed(kGpu, 1);
  Check(g.stage == 2, "governor: still GPU-bound -> stage 2");
  feed(kGpu, 10);
  Check(g.stage == 3, "governor: still GPU-bound -> stage 3 (paused)");
  steps = feed(kGpu, 30);
  Check(steps == 0 && g.stage == 3, "governor: stage 3 is the last (no further step)");
  // Mixed holds the stage.
  steps = feed(kMixed, 30);
  Check(steps == 0 && g.stage == 3, "governor: mixed verdicts hold the stage");
  // CPU-bound: step back up, one stage per dwell.
  steps = feed(kCpu, 10);
  Check(steps == 1 && g.stage == 2, "governor: CPU-bound -> back up to stage 2");
  steps = feed(kCapped, 10);
  Check(steps == 1 && g.stage == 1, "governor: capped (GPU headroom) also steps up -> stage 1");
  // Bounce: GPU-bound again right after stepping up from stage 2 -> back down; the next up-step from 2 waits longer.
  steps = feed(kGpu, 10);
  Check(steps == 1 && g.stage == 2 && g.bounces == 1 && g.UpDwell(c, 2) == 2.0 * c.dwellSec,
        "governor: a step down soon after a step up from the same stage is a bounce (its up-dwell doubles)");
  steps = feed(kCpu, 10);
  Check(steps == 0 && g.stage == 2, "governor: after a bounce the step up waits longer (no thrash)");
  steps = feed(kCpu, 10);
  Check(steps == 1 && g.stage == 1, "governor: ... and comes after the doubled dwell");
  // Repeated bounces are capped.
  for (int k = 0; k < 10; ++k) {
    feed(kGpu, 10);
    feed(kCpu, static_cast<int>(g.UpDwell(c, 2)) + 1);
  }
  Check(g.UpDwell(c, 2) <= c.maxDwellSec, "governor: the up-dwell is capped (MaxDwell)");
  // Decay: a step up that held for the bounce time halves the penalty.
  const double before = g.UpDwell(c, 2);
  feed(kMixed, c.bounceSec + 2);
  Check(g.UpDwell(c, 2) < before || before <= c.dwellSec, "governor: a step up that held relaxes the up-dwell");
  // Count stage changes over a long alternating run: at most one per dwell.
  Governor h;
  h.Reset(0);
  double tt = 0;
  int changes = 0;
  for (int i = 0; i < 600; ++i) {
    tt += 1;
    changes += h.Tick((i / 3) % 2 ? kGpu : kCpu, tt, c, 0xf) != 0;  // flips every 3 s
  }
  Check(changes == 0, "governor: verdicts flipping every 3 s never reach 6 of 8: no change in 10 min");
  Governor h2;
  h2.Reset(0);
  tt = 0;
  changes = 0;
  for (int i = 0; i < 1200; ++i) {
    tt += 1;
    changes += h2.Tick((i / 12) % 2 ? kGpu : kCpu, tt, c, 0xf) != 0;  // flips every 12 s
  }
  Check(changes <= 1200 / c.dwellSec, "governor: slow flips change at most once per dwell");
  // Disabled: never steps.
  GovConfig off = c;
  off.enabled = false;
  Governor d;
  d.Reset(0);
  int dc = 0;
  for (int i = 1; i < 100; ++i) dc += d.Tick(kGpu, i, off, 0xf) != 0;
  Check(dc == 0 && d.stage == 0, "governor: Enabled=0 never steps");
  // Usable stages: skipped stages and a stage that stops applying.
  Governor u;
  u.Reset(0);
  double tu = 0;
  for (int i = 0; i < 10; ++i) u.Tick(kGpu, ++tu, c, 1u | 4u | 8u);
  Check(u.stage == 2, "governor: an unusable stage is skipped (0 -> 2)");
  const int r = u.Tick(kMixed, ++tu, c, 1u);
  Check(r == -1 && u.stage == 0, "governor: no recorder left -> back to stage 0 at once");
}

// ---- fps proof: each step must show its effect on fps ----
// Model scene (the F-4 F2 case): GPU-bound with the recorders; the helper
// stages gain a little, but pausing the recorders moves their work to the
// render thread, which becomes the limit (verdict CPU) and fps drops.
struct Scene {
  double fps[kStages];
  Verdict verdict[kStages];
};
struct RunStats {
  int changes = 0, attempts3 = 0, reverts = 0, kept = 0, secondsAt3 = 0, finalStage = 0;
};
RunStats RunScene(const Scene& sc, int seconds, int startStage = 0) {
  GovConfig c;
  Governor g;
  g.stage = startStage;
  g.Reset(0);
  RunStats r;
  for (int t = 1; t <= seconds; ++t) {
    const double noise = ((t * 7919) % 11 - 5) * 0.01;  // +-0.05 fps, deterministic
    const int st = g.stage;
    const int d = g.Tick(sc.verdict[st], sc.fps[st] + noise, t, c, 0xf);
    r.changes += d != 0;
    if (g.lastEvent == kEvDown && g.stage == 3) r.attempts3++;
    r.reverts += g.lastEvent == kEvRevertDown || g.lastEvent == kEvRevertUp;
    r.kept += g.lastEvent == kEvKept;
    r.secondsAt3 += g.stage == 3;
  }
  r.finalStage = g.stage;
  return r;
}

void FpsProof() {
  // 1. Pausing lowers fps (the render thread becomes the limit): stages 1 and 2 kept (+3%), stage 3 reverted and
  //    blocked with a doubling back-off.
  const Scene f2{{45.0, 46.5, 48.0, 43.0}, {kGpu, kGpu, kGpu, kCpu}};
  const RunStats a = RunScene(f2, 3600);
  Check(a.kept == 2 && a.finalStage == 2, "governor fps proof: helper stages that raise fps (+3%) are kept");
  Check(a.attempts3 >= 1 && a.attempts3 <= 4 && a.reverts == a.attempts3,
        "governor fps proof: pausing that lowers fps is reverted every time, retried at most 4 times in an hour "
        "(300, 600, 1200, 2400 s back-off)");
  Check(a.secondsAt3 <= a.attempts3 * 23 && a.changes <= 6 + 2 * a.attempts3,
        "governor fps proof: the paused stage lasts only its trials (no thrash)");
  printf("     F2 model: %d stage changes, %d pause trials, %d s paused in an hour\n", a.changes, a.attempts3,
         a.secondsAt3);
  // 2. Without the fps proof (verdict only) the same scene would bounce between stage 2 and 3.
  {
    GovConfig c;
    Governor g;
    g.Reset(0);
    int changes = 0;
    for (int t = 1; t <= 3600; ++t) changes += g.Tick(f2.verdict[g.stage], t, c, 0xf) != 0;
    Check(changes > 2 * a.changes, "governor fps proof: (verdict-only governor on that scene: many more changes)");
  }
  // 3. A step down with a gain under MinGainPct (2%) is reverted.
  const Scene flat{{45.0, 45.5, 45.5, 45.5}, {kGpu, kGpu, kGpu, kGpu}};
  const RunStats b = RunScene(flat, 600);
  Check(b.finalStage == 0 && b.reverts >= 1 && b.kept == 0,
        "governor fps proof: a step down gaining +1.1% (under MinGainPct 2%) is reverted; the governor stays at 0");
  // 4. A real GPU-bound gain is kept down to the pause.
  const Scene gpu{{45.0, 46.5, 48.0, 50.0}, {kGpu, kGpu, kGpu, kGpu}};
  const RunStats c4 = RunScene(gpu, 600);
  Check(c4.finalStage == 3 && c4.reverts == 0 && c4.kept == 3, "governor fps proof: steps that pay off are kept (stage 3)");
  // 5. A step up that lowers fps is reverted (start paused, verdict capped, the full recorders cost fps here).
  const Scene up{{44.0, 44.0, 44.0, 46.0}, {kCapped, kCapped, kCapped, kCapped}};
  const RunStats d = RunScene(up, 600, 3);
  Check(d.finalStage == 3 && d.reverts >= 1 && d.changes <= 2 * d.reverts,
        "governor fps proof: a step up that lowers fps is reverted and stays reverted (blocked)");
  // 6. Inconclusive trial (no frames after the step): the step stays.
  {
    GovConfig c;
    Governor g;
    g.Reset(0);
    for (int t = 1; t <= 10; ++t) g.Tick(kGpu, 45.0, t, c, 0xf);
    const bool down = g.stage == 1 && g.trial;
    int ev = 0;
    for (int t = 11; t <= 40; ++t) {
      g.Tick(kNone, -1.0, t, c, 0xf);
      if (g.lastEvent != kEvNone) ev = g.lastEvent;
    }
    Check(down && ev == kEvInconclusive && g.stage == 1,
          "governor fps proof: a trial without frames (menu, loading) keeps the step");
  }
  // 7. Drift: fps rises 0.1 fps/s on its own, the step does nothing. Before/after alone sees +5% and keeps it;
  //    the A-B-A reference (the old stage rechecked) cancels the drift and reverts.
  {
    auto run = [](int recheck) {
      GovConfig c;
      c.recheckSec = recheck;
      Governor g;
      g.Reset(0);
      int kept = 0, reverted = 0;
      for (int t = 1; t <= 60; ++t) {
        g.Tick(kGpu, 45.0 + 0.1 * t, t, c, 0xf);
        kept += g.lastEvent == kEvKept;
        reverted += g.lastEvent == kEvRevertDown || g.lastEvent == kEvUnsteady;
      }
      return std::make_pair(kept, reverted);
    };
    const auto plain = run(0), paired = run(10);
    Check(plain.first == 1 && paired.first == 0 && paired.second == 1,
          "governor fps proof: a drifting scene fools before/after (kept); the paired A-B-A trial does not keep it");
  }
  // 7b. A step change instead of a drift (live 2026-10-11): the before window caught a 48 fps dip, the scene is
  //     66 fps afterwards whatever the stage. A and A' disagree: A' alone is the reference, and the step goes.
  {
    GovConfig c;
    Governor g;
    g.Reset(0);
    int kept = 0, reverted = 0, unsteady = 0;
    for (int t = 1; t <= 60; ++t) {
      g.Tick(kGpu, t <= 10 ? 48.0 : 66.0 + (g.stage == 1 ? 0.4 : 0.0), t, c, 0xf);
      kept += g.lastEvent == kEvKept;
      reverted += g.lastEvent == kEvRevertDown;
      unsteady += g.lastEvent == kEvUnsteady;
    }
    Check(kept == 0 && reverted == 0 && unsteady == 1 && g.stage == 0,
          "governor fps proof: a dip in the before window gives no verdict: the step down is undone, not kept");
  }
  // 7c. A step up during a scene change: no verdict, the step toward the configuration stays.
  {
    GovConfig c;
    Governor g;
    g.stage = 1;
    g.Reset(0);
    int unsteady = 0;
    for (int t = 1; t <= 60; ++t) {
      g.Tick(kCpu, t <= 10 ? 63.0 : 68.5, t, c, 0xf);
      unsteady += g.lastEvent == kEvUnsteady;
    }
    Check(unsteady == 1 && g.stage == 0 && g.Blocked(1, 50),
          "governor fps proof: a step up during a scene change has no verdict and stays (stage 1 waits 60 s)");
  }
  // 7d. A scene that keeps changing (every trial unsteady): the waits double, so trials get rare.
  {
    GovConfig c;
    Governor g;
    g.Reset(0);
    int trials = 0;
    for (int t = 1; t <= 3600; ++t) {
      const double fps = (t / 25) % 2 ? 60.0 : 68.0;  // the scene flips every 25 s, whatever the stage
      g.Tick(kGpu, fps, t, c, 0xf);
      trials += g.lastEvent == kEvDown;
    }
    Check(trials <= 8 && g.stage == 0,
          "governor fps proof: a scene that keeps changing is retried ever more rarely (waits 60, 120, 240 s...)");
    printf("     changing scene: %d trials in an hour\n", trials);
  }
  // 8. A static GPU-bound scene with +-3% per-second noise and no real effect of any stage: mostly stage 0, rare
  //    retries, never a kept step on noise.
  {
    GovConfig c;
    Governor g;
    g.Reset(0);
    uint32_t rng = 12345;
    int kept = 0, trials = 0, secondsDown = 0, changes = 0;
    for (int t = 1; t <= 3600; ++t) {
      rng = rng * 1664525u + 1013904223u;
      const double noise = ((rng >> 8) % 6001) / 1000.0 - 3.0;  // -3..+3 %
      const int d = g.Tick(kGpu, 60.0 * (1.0 + noise / 100.0), t, c, 0xf);
      changes += d != 0;
      kept += g.lastEvent == kEvKept;
      trials += g.lastEvent == kEvDown;
      secondsDown += g.stage > 0;
    }
    Check(kept == 0 && trials <= 5 && secondsDown <= trials * 24 && g.stage == 0,
          "governor fps proof: +-3% noise on a static scene: no step kept, at most 5 trials an hour, stage 0");
    printf("     noisy static scene: %d trials, %d s off stage 0, %d stage changes in an hour\n", trials, secondsDown,
           changes);
  }
}

// ---- Apply path: pause/resume through the kill-switch path ----
struct RecSave {
  Config cfg = g_cfg;
  int shState = shrec::g_state.load(), gbState = gbrec::g_state.load();
  bool shOn = shrec::g_on.load(), gbOn = gbrec::g_on.load();
  uint32_t split = shrec::g_split.load(), gbHelpers = gbrec::g_helpers.load();
  bool onGb = shadowinst::g_onGb.load(), gbInst = shadowinst::g_gbInstalled.load();
  bool collectGb = shadowinst::g_collectGb.load();
  int stage = g_govStage.load();
  bool engineOff = g_engineOff.load();
  int heapState = frameheap::g_state.load();
  ~RecSave() {
    frameheap::g_state = heapState;
    g_cfg = cfg;
    shrec::g_state = shState;
    gbrec::g_state = gbState;
    shrec::g_on = shOn;
    gbrec::g_on = gbOn;
    shrec::g_split = split;
    gbrec::g_helpers = gbHelpers;
    shadowinst::g_onGb = onGb;
    shadowinst::g_gbInstalled = gbInst;
    shadowinst::g_collectGb = collectGb;
    g_govStage = stage;
    g_engineOff = engineOff;
  }
};

void ApplyPath() {
  RecSave save;
  // Both recorders "ready" (Install returns at once) with the app's configuration.
  g_cfg.shadowRecorder = true;
  g_cfg.gbufferRecorder = true;
  g_cfg.shadowInst = true;
  g_cfg.gbufferBatch = false;
  g_cfg.shadowRecorderSplit = 0xf;
  g_cfg.gbufferRecorderHelpers = 2;
  shrec::g_state = 1;
  gbrec::g_state = 1;
  frameheap::g_state = 1;  // the reset hooks (the recorders' recycled-heap guard) installed
  shadowinst::g_onGb = true;
  shadowinst::g_gbInstalled = true;
  g_engineOff = false;
  struct Want {
    int stage;
    bool on;
    uint32_t split, helpers;
  };
  const Want want[] = {{0, true, 0xf, 2}, {1, true, 0x8, 2}, {2, true, 0, 0}, {3, false, 0, 0},
                       {2, true, 0, 0},   {1, true, 0x8, 2}, {0, true, 0xf, 2}};
  bool ok = true;
  for (const Want& w : want) {
    perfmon::SetStage(w.stage);
    ok &= shrec::g_on.load() == w.on && gbrec::g_on.load() == w.on && shrec::g_split.load() == w.split &&
          gbrec::g_helpers.load() == w.helpers && g_govStage.load() == w.stage;
  }
  Check(ok, "governor apply: stages 0-3-0 set the split, the G-buffer helpers and pause/resume both recorders");
  // Kill switch (SetEngineOff's recorder part): off wins over every stage; back on restores the governor's stage.
  auto engine = [](bool off) {
    g_engineOff = off;
    ApplyShadowRecorder();
    ApplyGBufferRecorder();
  };
  perfmon::SetStage(3);
  engine(true);
  const bool offOk = !shrec::g_on.load() && !gbrec::g_on.load();
  engine(false);
  const bool stillPaused = !shrec::g_on.load() && !gbrec::g_on.load() && g_govStage.load() == 3;
  perfmon::SetStage(1);
  engine(true);
  engine(false);
  const bool backOn = shrec::g_on.load() && gbrec::g_on.load() && shrec::g_split.load() == 0x8;
  Check(offOk && stillPaused && backOn,
        "governor apply: the kill switch turns both off; back on keeps the governor's stage (3 paused, 1 on with 0x8)");
  // Benchmark start: back to stage 0 (the A/B measures the configured features).
  perfmon::SetStage(3);
  perfmon::GovForBench();
  Check(g_govStage.load() == 0 && shrec::g_on.load() && gbrec::g_on.load() && shrec::g_split.load() == 0xf &&
            gbrec::g_helpers.load() == 2,
        "governor apply: a benchmark start puts stage 0 back (configured split and helpers, recorders on)");
  // An ini re-read (LoadConfig's Apply calls) keeps the stage.
  perfmon::SetStage(2);
  ApplyShadowRecorder();
  ApplyGBufferRecorder();
  Check(shrec::g_split.load() == 0 && gbrec::g_helpers.load() == 0 && shrec::g_on.load(),
        "governor apply: re-applying the ini keeps the governor's stage");
  // A latched recorder is not governed (UsableStages from what can run).
  shrec::g_disabled = true;
  const bool shGov = perfmon::ShadowRecGovernable();
  shrec::g_disabled = false;
  Check(!shGov && perfmon::GBufferRecGovernable(), "governor apply: a latched recorder is left out of the stages");
  perfmon::SetStage(0);
  // Concurrency: a render-thread stand-in arming jobs (reads of on/split/helpers) while the worker steps.
  std::atomic<bool> stop{false};
  std::atomic<uint64_t> bad{0}, reads{0};
  std::thread reader([&] {
    while (!stop.load()) {
      const bool on = shrec::g_on.load();
      const uint32_t sp = shrec::g_split.load(), hp = gbrec::g_helpers.load();
      if (sp != 0xf && sp != 0x8 && sp != 0) bad++;
      if (hp != 2 && hp != 0) bad++;
      (void)on;
      reads++;
    }
  });
  // The reader must really overlap the steps (a busy machine may start it late): wait for it, then step until it
  // has read many times (bounded).
  for (int i = 0; i < 5000 && reads.load() == 0; ++i) Sleep(1);
  const uint64_t r0 = reads.load();
  const ULONGLONG until = GetTickCount64() + 10000;
  for (int i = 0; (i < 4000 || reads.load() < r0 + 1000) && GetTickCount64() < until; ++i) perfmon::SetStage(i % 4);
  stop = true;
  reader.join();
  perfmon::SetStage(0);
  printf("     concurrent arming: %llu reads, %llu outside a stage's values%c", static_cast<unsigned long long>(reads.load()),
         static_cast<unsigned long long>(bad.load()), 10);
  Check(bad.load() == 0 && reads.load() > 0,
        "governor apply: concurrent arming reads only ever see a stage's split/helpers values");
}

// ---- G-buffer stale start across a pause ----
void StaleAcrossPause() {
  using namespace gbrec;
  // Timeline (QPC ticks): frame entries every 100. A job started by the sort at 250 (frame 2, before its entry
  // at 260); the recorder pauses at 255. Paused entries at 260, 360, 460; resume before the entry at 560.
  int64_t mark = 160;           // the last active entry (frame 1)
  const int64_t tStart = 250;
  int64_t markOld = mark;       // before the fix: not advanced while paused
  for (int64_t e : {260, 360, 460}) mark = e;  // the fix: advanced at paused entries
  const int64_t lastPass = 170;
  const int64_t prevNew = lastPass > mark ? lastPass : mark;
  const int64_t prevOld = lastPass > markOld ? lastPass : markOld;
  Check(StaleStart(kStartRun, 0, tStart, prevNew),
        "gbuffer rec pause: a job started just before a pause is stale at the first entry after the resume");
  Check(!StaleStart(kStartRun, 0, tStart, prevOld),
        "gbuffer rec pause: (without the advancing mark it was not: only the vector check at the pass stopped it)");
  Check(!StaleStart(kStartRun, 0, 470, prevNew) && !StaleStart(kStartArmed, 0, 100, prevNew) &&
            !StaleStart(kStartRun, 7, 100, prevNew),
        "gbuffer rec pause: a start after the last entry, an unstarted job or one given to an entry is not stale");
}

// ---- Issue #3 (0.5.8): Alt+Shift+F11 off and back on in flight ----
// Replays the field log's sequence (dcsqvcull-1.log 20:34:38.190 OFF, 5 s of frames off, 20:34:43.49 ON in the
// middle of a frame, the recorder's first frames back on) through the code that decides it: the kill switch's
// generation step, the frame-heap reset hook with FrameHeap=off(killswitch), the G-buffer recorder's render
// entry and the pass's arming rule, and a re-record of a job from before the switch.
void __fastcall NoHeapReset(void*) {}

void KillSwitchReplay() {
  using namespace gbrec;
  RecSave save;
  const DWORD tid0 = g_renderTid;
  void* const rg0 = g_gbRg;
  const bool dis0 = g_disabled.load(), sd0 = g_shutdown.load(), batch0 = gbbatch::g_on.load(),
             chained0 = gbreccount::g_chained.load(), heapOn0 = g_frameHeapOn.load();
  const int64_t scene0 = g_sceneChangeQpc.load();
  const auto reset0 = frameheap::g_origReset;
  static int rg;
  g_renderTid = GetCurrentThreadId();
  g_gbRg = &rg;
  g_state = 1;
  g_disabled = false;
  g_shutdown = false;
  gbbatch::g_on = false;
  gbreccount::g_chained = false;
  frameheap::g_origReset = &NoHeapReset;
  g_frameHeapOn = true;
  Job* j = NewJob();
  uint8_t* bad = static_cast<uint8_t*>(VirtualAlloc(nullptr, 1 << 16, MEM_RESERVE, PAGE_NOACCESS));
  if (!j || !bad) {
    QvSkip("kill switch replay: no job memory\n");
  } else {
    // Engine ON: a frame the recorder sees from its render entry; a job of that frame (its pass queued re-records).
    g_on = true;
    frameheap::HookReset<0>(nullptr);
    OnRenderBody(&rg, nullptr);
    const bool armOn = PassMayArm(false) && !PassMayArm(true);
    j->n = 4;
    for (uint32_t i = 0; i < 4; ++i) {
      j->snap[i] = bad;  // reading a renderable faults: the test sees any read
      j->cands[i] = Cand();
      j->cands[i].item = i;
      j->cands[i].alive = 1;
    }
    j->heapGen = g_frameHeapGen.load();
    j->tStart = defrec::Qpc();
    j->redoStop = 0;
    // 20:34:38.190 OFF: the switch's generation step, then the recorders off (SetEngineOff's order).
    OnEngineSwitch();
    g_on = false;
    g_frameHeapOn = false;  // FrameHeap=off(killswitch): the slabs stop, the reset hooks stay
    const bool staleAtOff = HeapRecycled(*j) && shrec::SceneChanging(j->tStart);
    // 5 s off: DCS keeps resetting its frame heap every frame; the hooks keep counting.
    const uint64_t g1 = g_frameHeapGen.load();
    for (int f = 0; f < 3; ++f) {
      frameheap::HookReset<0>(nullptr);
      OnRenderBody(&rg, nullptr);
    }
    const bool genMoved = g_frameHeapGen.load() == g1 + 3;
    const bool noArmOff = !PassMayArm(false);
    // 20:34:43.49 ON between this frame's render entry and its G-buffer passes: those passes do not arm.
    g_on = true;
    g_frameHeapOn = true;
    OnEngineSwitch();
    const bool noArmMid = !PassMayArm(false);
    // The next frame: its entry is seen active, its passes arm again.
    frameheap::HookReset<0>(nullptr);
    OnRenderBody(&rg, nullptr);
    const bool armNext = PassMayArm(false);
    // A re-record of the job from before the switch, run late (the field fault's path, RecordRange <- RedoMain):
    // stopped by the generation before its first read of a renderable.
    defrec::Worker w;
    Piece p = Piece();
    p.candFirst = 0;
    p.candEnd = 4;
    p.redo = 1;
    j->recycled = 0;
    const bool stoppedGen = !RecordRange(w, *j, p, 0) && j->recycled.load() == 1;
    // A re-record of this frame's job queued past its pass: stopped by redoStop, still before any read.
    j->heapGen = g_frameHeapGen.load();
    j->recycled = 0;
    j->redoStop = 1;
    const bool stoppedPass = !HeapRecycled(*j) && !RecordRange(w, *j, p, 0) && j->recycled.load() == 0;
    // A fault that still slips in within 2 s of the switch is contained per job, even on the current generation.
    const bool contained = RecycledFilter(EXCEPTION_ACCESS_VIOLATION, *j) == EXCEPTION_EXECUTE_HANDLER;
    printf("     kill switch replay: generation %llu -> %llu over 3 frames off; arm on %d, off %d, mid-frame %d, next %d\n",
           static_cast<unsigned long long>(g1), static_cast<unsigned long long>(g_frameHeapGen.load()), armOn,
           !noArmOff, !noArmMid, armNext);
    Check(armOn && staleAtOff && genMoved,
          "kill switch replay: the switch makes the jobs in flight stale; the frame-heap generation keeps moving while "
          "FrameHeap is off (the reset hooks stay; only the slabs stop)");
    Check(noArmOff && noArmMid && armNext,
          "kill switch replay: no pass arms a job while off, nor in the frame the switch came back on in the middle "
          "of; the next frame's passes do");
    Check(stoppedGen && stoppedPass && contained,
          "kill switch replay: a late re-record stops before reading a renderable (heap generation, its pass over); a "
          "fault near the switch is contained per job");
    g_recycledFaults = 0;
  }
  if (bad) VirtualFree(bad, 0, MEM_RELEASE);
  if (j) FreeJob(j);
  frameheap::g_origReset = reset0;
  g_frameHeapOn = heapOn0;
  g_sceneChangeQpc = scene0;
  gbreccount::g_chained = chained0;
  gbbatch::g_on = batch0;
  g_shutdown = sd0;
  g_disabled = dis0;
  g_gbRg = rg0;
  g_renderTid = tid0;
}

// ---- gb_rec live crash 2026-10-11 20:09:55: workers reading a recycled frame heap ----
int FaultUnder(gbrec::Job& j, volatile int* bad, bool* handled) {
  __try {
    return *bad;
  } __except ((*handled = gbrec::RecycledFilter(GetExceptionCode(), j) == EXCEPTION_EXECUTE_HANDLER) ? 1 : 0) {
    return -1;
  }
}

void* g_unwound[8];
int g_unwoundN = 0;
int UnwindProbeFilter(EXCEPTION_POINTERS* ep) {
  g_unwoundN = defrec::Pool::UnwindFrames(*ep->ContextRecord, g_unwound, 8);
  return EXCEPTION_EXECUTE_HANDLER;
}
__declspec(noinline) int UnwindProbe(volatile int* bad) {
  __try {
    return *bad;
  } __except (UnwindProbeFilter(GetExceptionInformation())) {
    return -1;
  }
}

void RecycledHeap() {
  using namespace gbrec;
  Job* j = NewJob();
  if (!j) {
    QvSkip("gbuffer rec recycled heap: no job memory\n");
    return;
  }
  // The job's renderables: unmapped memory, as recycled frame-heap bytes give a garbage item pointer.
  uint8_t* bad = static_cast<uint8_t*>(VirtualAlloc(nullptr, 1 << 16, MEM_RESERVE, PAGE_NOACCESS));
  j->n = 100;
  for (size_t i = 0; i < 100 && i < kMaxItems; ++i) j->snap[i] = bad;
  j->heapGen = g_frameHeapGen.load();
  j->recycled = 0;
  j->preNext = 0;
  j->preDone = 0;
  Check(!HeapRecycled(*j), "gbuffer rec heap: a job started in this frame is not recycled");
  g_frameHeapGen.fetch_add(1);  // DCS resets the frame heap: the next frame began
  PreChunks(*j, 0);
  Check(HeapRecycled(*j) && j->recycled.load() == 1 && j->preDone.load() == 100,
        "gbuffer rec heap: stage A after the heap reset counts its chunks without reading a renderable (no fault, "
        "the primary is not left waiting)");
  bool handled = false;
  const int r = FaultUnder(*j, reinterpret_cast<volatile int*>(bad), &handled);
  const uint64_t faults = g_recycledFaults.load();
  Check(r == -1 && handled && faults >= 1,
        "gbuffer rec heap: a fault on a recycled heap is contained per job (the recorder is not latched off)");
  j->heapGen = g_frameHeapGen.load();
  Check(RecycledFilter(EXCEPTION_ACCESS_VIOLATION, *j) == EXCEPTION_CONTINUE_SEARCH &&
            RecycledFilter(EXCEPTION_INT_DIVIDE_BY_ZERO, *j) == EXCEPTION_CONTINUE_SEARCH,
        "gbuffer rec heap: a fault on a live heap is not swallowed (still latches: a real bug)");
  g_recycledFaults = 0;
  VirtualFree(bad, 0, MEM_RELEASE);
  FreeJob(j);
  // The shadow recorder's jobs: the same guard (stage A, commit, recording; per-job containment).
  {
    shrec::Job* sj = shrec::NewJob();
    if (!sj) {
      QvSkip("shadow rec recycled heap: no job memory\n");
    } else {
      uint8_t* bad3 = static_cast<uint8_t*>(VirtualAlloc(nullptr, 1 << 16, MEM_RESERVE, PAGE_NOACCESS));
      sj->n = 100;
      for (size_t i = 0; i < 100; ++i) sj->snap[i] = bad3;
      sj->heapGen = g_frameHeapGen.load();
      sj->recycled = 0;
      sj->preNext = 0;
      sj->preDone = 0;
      const bool live = !shrec::HeapRecycled(*sj);
      g_frameHeapGen.fetch_add(1);
      shrec::PreChunks(*sj, 0);
      Check(live && shrec::HeapRecycled(*sj) && sj->recycled.load() == 1 && sj->preDone.load() == 100,
            "shadow rec heap: stage A after the heap reset reads no caster and counts its chunks");
      bool handled2 = false;
      int r2 = 0;
      __try {
        r2 = *reinterpret_cast<volatile int*>(bad3);
      } __except ((handled2 = shrec::RecycledFilter(GetExceptionCode(), *sj) == EXCEPTION_EXECUTE_HANDLER) ? 1 : 0) {
        r2 = -1;
      }
      sj->heapGen = g_frameHeapGen.load();
      Check(r2 == -1 && handled2 && shrec::RecycledFilter(EXCEPTION_ACCESS_VIOLATION, *sj) == EXCEPTION_CONTINUE_SEARCH,
            "shadow rec heap: a fault on a recycled heap is contained per job, one on a live heap is not");
      sj->heapGen = 0;
      Check(!shrec::HeapRecycled(*sj), "shadow rec heap: a job not started by StartJob (tests) is never recycled");
      shrec::g_recycledFaults = 0;
      VirtualFree(bad3, 0, MEM_RELEASE);
      shrec::FreeJob(sj);
    }
  }
  // The pool's fault line: module+offset of the fault and the frames below it.
  uint8_t* bad2 = static_cast<uint8_t*>(VirtualAlloc(nullptr, 4096, MEM_RESERVE, PAGE_NOACCESS));
  UnwindProbe(reinterpret_cast<volatile int*>(bad2));
  VirtualFree(bad2, 0, MEM_RELEASE);
  char at[160] = {};
  if (g_unwoundN) defrec::Pool::ModOff(g_unwound[0], at, sizeof(at));
  Check(g_unwoundN >= 3 && strstr(at, ".exe+0x") != nullptr,
        "pool fault line: the faulting address as module+offset and an unwound stack");
  printf("     fault at %s, %d frames\n", at, g_unwoundN);
}

// ---- Status word R30 bits ----
void StatusBits() {
  using namespace qvstatus;
  const uint64_t w = Pack(true, true, 12, 0, 18);
  const uint64_t x = WithExtras(w, true, 3, kGpu, true);
  Check(Warming(x) && GovernorStage(x) == 3 && qvstatus::Verdict(x) == 2 && GovernorOn(x) && ActiveCount(x) == 12 &&
            ConfiguredCount(x) == 18 && (x & 0xffffffffull) == (w | kWarming),
        "status word R30: warm-up bit 3, stage bits 32-33, verdict bits 34-36, governor bit 37; counts unchanged");
  Check(WithExtras(0, true, 2, 1, true) == 0, "status word R30: not running stays 0");
  Check(GovernorStage(WithExtras(w, false, 9, 4, false)) == 3 && qvstatus::Verdict(WithExtras(w, false, 0, 4, false)) == 4 &&
            !GovernorOn(WithExtras(w, false, 0, 0, false)),
        "status word R30: stage saturates at 3, capped = 4, governor off");
  const uint64_t real = ComputeStatus();
  Check(Running(real) && GovernorOn(real) == g_cfg.govEnabled, "status word R30: ComputeStatus carries the governor bit");
}

// ---- Log lines ----
void Lines() {
  // Features: 18 names, states from ComputeStatus.
  ComputeStatus();
  const std::string f = perfmon::FeatureText();
  bool names = true;
  for (const char* n : perfmon::kFeatName) names &= f.find(std::string(" ") + n + "=") != std::string::npos;
  // The app's warning filter (EngineOptimizations.cs) reads lines by these lower-case prefixes: never in this line.
  bool clean = f.find("shadow recorder") == std::string::npos && f.find("gbuffer recorder") == std::string::npos &&
               f.find("split filter") == std::string::npos && f.find("shadow inst:") == std::string::npos;
  Check(names && clean && f.rfind("features: ", 0) == 0,
        "features line: all 18 by name with a state, none of the app's warning prefixes");
  // Aggregation of synthetic seconds: fps, busy, majority verdict, engine-state filter.
  perfmon::g_secCount = 0;
  for (int i = 1; i <= 20; ++i) {
    perfmon::Sec s;
    s.t = i;
    s.frames = i <= 10 ? 45 : 60;
    s.renderPct = i <= 10 ? 55 : 95;
    s.gpuPct = i <= 10 ? 98 : 70;
    s.engineOff = i > 10;
    s.verdict = i <= 10 ? kGpu : kCpu;
    perfmon::g_sec[perfmon::g_secCount++ % perfmon::kSecs] = s;
  }
  const perfmon::Agg on = perfmon::Aggregate(0, 20, 0), off = perfmon::Aggregate(0, 20, 1);
  Check(on.n == 10 && std::fabs(on.Fps() - 45) < 1e-9 && std::fabs(on.Gpu() - 98) < 1e-9 &&
            perfmon::Majority(on) == kGpu && off.n == 10 && std::fabs(off.Fps() - 60) < 1e-9 &&
            perfmon::Majority(off) == kCpu,
        "perf aggregation: per engine state fps, busy and the majority verdict over the seconds");
  perfmon::g_secCount = 0;
  // The recorders' compact lines run without a recorder (no frames): one line each.
  shrec::LogSummary("test", 0);
  gbrec::LogSummary("test", 0);
  Check(true, "recorder summaries: one line each without counters");
  // Environment helpers: strings, never empty.
  const std::string rt = envinfo::XrRuntime(), layers = envinfo::XrLayers(), st = envinfo::Streamers();
  Check(!rt.empty() && !layers.empty() && !st.empty() && !envinfo::CpuBrand().empty(),
        "env: OpenXR runtime, layers, streamers and CPU are named (or 'none')");
  printf("     env: runtime %s; layers %s; streamers %s\n", rt.c_str(), layers.c_str(), st.c_str());
}

// ---- D3DKMT with a fake driver: RunningTime negative (RTX 5090 node 0), the process query unanswered ----
LONGLONG g_fakeRun = -3028944060LL;  // 100 ns units, counting up
bool g_fakeProcAnswers = false;
NTSTATUS APIENTRY FakeQs(const D3DKMT_QUERYSTATISTICS* q) {
  auto* w = const_cast<D3DKMT_QUERYSTATISTICS*>(q);
  if (q->Type == D3DKMT_QUERYSTATISTICS_NODE) {
    w->QueryResult.NodeInformation.GlobalInformation.RunningTime.QuadPart = g_fakeRun;
    return 0;
  }
  if (q->Type == D3DKMT_QUERYSTATISTICS_PROCESS_NODE) {
    if (!g_fakeProcAnswers) return static_cast<NTSTATUS>(0xC000000DL);
    w->QueryResult.ProcessNodeInformation.RunningTime.QuadPart = g_fakeRun / 4;
    return 0;
  }
  return static_cast<NTSTATUS>(0xC000000DL);
}
void KmtFake() {
  perfmon::g_kmt = perfmon::Kmt();
  perfmon::g_kmt.qs = &FakeQs;
  perfmon::g_kmt.ok = true;
  perfmon::g_kmt.nodes = {0};
  perfmon::g_kmt.lastAll.assign(1, 0), perfmon::g_kmt.lastProc.assign(1, 0);
  perfmon::g_kmt.haveAll.assign(1, 0), perfmon::g_kmt.haveProc.assign(1, 0);
  g_fakeProcAnswers = false;
  double a = -1, p = -1;
  // A negative counter that counts up: the first sample primes, the next ones are percentages (was: never valid,
  // -1 doubled as "no previous sample").
  const bool first = perfmon::KmtSample(&a, &p);
  bool later = true;
  int na = 0;
  for (int i = 0; i < 2 * perfmon::kKmtRetry + 2; ++i) {
    Sleep(15);
    g_fakeRun += 20000;  // 2 ms busy per sample
    double a2 = -1, p2 = -1;
    const bool ok = perfmon::KmtSample(&a2, &p2);
    later = later && ok && a2 > 0 && a2 <= 100;
    na += p2 < 0;
  }
  // The process query never answered with either handle kind: one switch, then n/a without more switching.
  const uint32_t sw = perfmon::g_kmt.switches;
  const bool unsupported = perfmon::g_kmt.procUnsupported;
  for (int i = 0; i < 3 * perfmon::kKmtRetry; ++i) {
    g_fakeRun += 20000;
    perfmon::KmtSample(&a, &p);
  }
  const bool noMoreSwitches = perfmon::g_kmt.switches == sw;
  // The driver answers again: picked up at the next probe (once per kKmtProbe samples), then a share.
  g_fakeProcAnswers = true;
  bool back = false;
  for (uint32_t i = 0; i < perfmon::kKmtProbe + 3 && !back; ++i) {
    Sleep(1);
    g_fakeRun += 20000;
    back = perfmon::KmtSample(&a, &p) && p >= 0;
  }
  Check(!first && later && na == 2 * perfmon::kKmtRetry + 2 && sw == 1 && unsupported && noMoreSwitches && back &&
            !perfmon::g_kmt.procUnsupported,
        "gpu busy (D3DKMT, fake driver): a negative RunningTime that counts up gives a busy percentage (a valid "
        "flag, not a -1 sentinel); a process query that never answers (0xC000000D, both handles) degrades to n/a "
        "without more switching and recovers when the driver answers");
  perfmon::g_kmt = perfmon::Kmt();
}

// ---- D3DKMT GPU busy on this machine's first adapter (read-only statistics) ----
void GpuBusy() {
  if (!perfmon::OpenFirstAdapter() || !perfmon::g_ad.haveDesc) {
    QvSkip("gpu busy: no DXGI adapter\n");
    return;
  }
  if (!perfmon::KmtOpen(perfmon::g_ad.desc.AdapterLuid)) {
    QvSkip("gpu busy: D3DKMT statistics not available\n");
    return;
  }
  double a = -1, p = -1;
  const bool first = perfmon::KmtSample(&a, &p);
  Sleep(250);
  const bool second = perfmon::KmtSample(&a, &p);
  Check(!first && second && a >= 0 && a <= 100 && (p < 0 || (p >= 0 && p <= 100)),
        "gpu busy (D3DKMT): the first sample primes, the second is a percentage");
  printf("     gpu busy: %s 3D engine %.0f%% (this process %.0f%%) on %zu node(s)\n",
         envinfo::Utf8(perfmon::g_ad.desc.Description).c_str(), a, p, perfmon::g_kmt.nodes.size());
  perfmon::Shutdown();
  perfmon::g_kmt = perfmon::Kmt();
  perfmon::g_ad = perfmon::Adapter();
}

// ---- Frame-rate cap (field 2026-10-11: 5080 + VD locked at 30.0 fps = 90/3) ----
std::vector<float> Steady(double ms, int n, double jitterMs = 0, uint32_t seed = 1, double spikeEvery = 0) {
  std::vector<float> v;
  uint32_t r = seed;
  for (int i = 0; i < n; ++i) {
    r = r * 1664525u + 1013904223u;
    const double j = jitterMs * (((r >> 8) % 2001) / 1000.0 - 1.0);
    double x = ms + j;
    if (spikeEvery > 0 && i % static_cast<int>(spikeEvery) == static_cast<int>(spikeEvery) - 1) x = 2 * ms;  // a missed slot
    v.push_back(static_cast<float>(x));
  }
  return v;
}

void CapTests() {
  struct Want {
    double ms;
    int hz, div;
  };
  const Want exact[] = {{1000.0 / 30, 90, 3}, {1000.0 / 45, 90, 2}, {1000.0 / 72, 72, 1}, {1000.0 / 90, 90, 1},
                        {1000.0 / 40, 80, 2},  {1000.0 / 120, 120, 1}, {1000.0 / 144, 144, 1}, {1000.0 / 36, 72, 2},
                        {1000.0 / 60, 120, 2}};
  bool ok = true;
  for (const Want& w : exact) {
    const Cap c = DetectCap(Steady(w.ms, 300));
    ok &= c.hz == w.hz && c.div == w.div && std::fabs(c.fps - static_cast<double>(w.hz) / w.div) < 1e-9;
  }
  Check(ok, "cap: exact caps found as refresh/N (30 = 90/3, 45 = 90/2, 72, 90, 40 = 80/2, 120, 144, 36 = 72/2, 60 = "
            "120/2)");
  const Cap noisy = DetectCap(Steady(1000.0 / 30, 300, 1.5, 7, 50));
  Check(noisy.hz == 90 && noisy.div == 3, "cap: +-1.5 ms jitter and a missed slot every 50 frames: still 30 = 90/3");
  const Cap var = DetectCap(Steady(16.0, 600, 4.0, 3));
  const Cap steady68 = DetectCap(Steady(1000.0 / 68.5, 600, 0.3, 5));
  const Cap near72 = DetectCap(Steady(1000.0 / 69.0, 600, 0.2, 9));
  Check(var.fps == 0 && steady68.fps == 0 && near72.fps == 0,
        "cap: variable 12-20 ms and steady uncapped 68.5 / 69 fps (live airfield, GPU-bound) are not caps");
  Check(DetectCap(Steady(1000.0 / 30, 10)).fps == 0, "cap: too few frames is no cap");
  // A long tail (median at 30 fps, mean far below): not a cap.
  std::vector<float> tail = Steady(1000.0 / 30, 200);
  for (int i = 0; i < 40; ++i) tail[i * 5] = 120.0f;
  Check(DetectCap(tail).fps == 0, "cap: a median on 33.3 ms with a long tail of 120 ms frames is not a cap");
  // Cap change mid-run (SSW 90/3 -> 90/2): the 5-s tracker follows within the window, never reports a wrong cap.
  CapTracker t;
  bool before = false, wrong = false;
  int switchedAt = -1;
  for (int sec = 0; sec < 20; ++sec) {
    t.Add(sec < 10 ? Steady(1000.0 / 30, 30, 0.5, sec + 1) : Steady(1000.0 / 45, 45, 0.5, sec + 1));
    if (sec == 9) before = t.last.hz == 90 && t.last.div == 3;
    if (sec >= 10 && t.last.fps > 0 && !(t.last.hz == 90 && t.last.div == 2) && sec > 10 + t.seconds) wrong = true;
    if (sec >= 10 && switchedAt < 0 && t.last.hz == 90 && t.last.div == 2) switchedAt = sec;
  }
  Check(before && !wrong && switchedAt >= 10 && switchedAt <= 10 + t.seconds && t.last.div == 2,
        "cap: a change from 30 to 45 fps (90/3 -> 90/2) is followed within the 5-s window");
  printf("     cap change seen %d s after it happened\n", switchedAt - 10);
  // Governor: capped seconds hold the stage (no trial), once logged; uncapped again logged.
  RecSave save;
  g_cfg.shadowRecorder = true;
  g_cfg.gbufferRecorder = true;
  g_cfg.shadowInst = true;
  g_cfg.gbufferBatch = false;
  g_cfg.govEnabled = true;
  shrec::g_state = 1;
  gbrec::g_state = 1;
  shadowinst::g_onGb = true;
  shadowinst::g_gbInstalled = true;
  g_engineOff = false;
  perfmon::g_gov = Governor();
  perfmon::g_govAnnounced = true;
  perfmon::g_capHoldLogged = false;
  perfmon::SetStage(0);
  StartReportCapture();
  double tt = 5000;
  perfmon::g_gov.Reset(tt);
  for (int i = 0; i < 60; ++i) {
    perfmon::Sec s;
    s.t = ++tt;
    s.frames = 30;
    s.fps = 30.0;
    s.renderPct = 50;
    s.gpuPct = 99;  // the thresholds alone would say GPU-bound
    s.verdict = kGpu;
    s.cap.fps = 30.0;
    s.cap.hz = 90;
    s.cap.div = 3;
    perfmon::GovTick(s);
  }
  const int heldStage = g_govStage.load();
  perfmon::Sec u;
  u.t = ++tt;
  u.frames = 60;
  u.fps = 60;
  u.verdict = kMixed;
  perfmon::GovTick(u);
  const std::string text = StopReportCapture();
  perfmon::SetStage(0);
  size_t holds = 0;
  for (size_t p = text.find("governor holding"); p != std::string::npos; p = text.find("governor holding", p + 1))
    ++holds;
  Check(heldStage == 0 && holds == 1 && text.find("capped at 30.0 fps (90 Hz / 3): governor holding") != std::string::npos &&
            text.find("frame rate no longer capped") != std::string::npos &&
            text.find("GPU-bound in") == std::string::npos,
        "cap: the governor holds while capped (no step, no trial, logged once) and says when the cap ends");
  // [Governor] Enabled=0 in the middle of a step's fps trial: stage 0 at once,
  // and the trial is dropped. It stayed armed: re-enabled, its old B seconds
  // and new seconds at stage 0 decided the step (recheck, then kept or
  // reverted) on data from before the pause.
  {
    perfmon::g_gov = Governor();
    perfmon::SetStage(0);
    double tg = 9000;
    perfmon::g_gov.Reset(tg);
    auto sec = [&](double fps, int verdict) {
      perfmon::Sec s;
      s.t = ++tg;
      s.frames = static_cast<uint32_t>(fps);
      s.fps = fps;
      s.renderPct = 50;
      s.gpuPct = verdict == kGpu ? 99 : 70;
      s.verdict = verdict;
      perfmon::GovTick(s);
    };
    bool stepped = false;
    for (int i = 0; i < 120 && !stepped; ++i) {
      sec(60, kGpu);
      stepped = perfmon::g_gov.trial && g_govStage.load() == 1;
    }
    for (int i = 0; i < 15; ++i) sec(75, kGpu);  // B: 15 s at stage 1
    const bool midTrial = perfmon::g_gov.trial && perfmon::g_gov.trialPhase == 1;
    g_cfg.govEnabled = false;
    sec(75, kGpu);
    const bool off = g_govStage.load() == 0 && !perfmon::g_gov.trial;
    g_cfg.govEnabled = true;
    bool trialEvent = false;
    for (int i = 0; i < 40; ++i) {
      sec(60, kMixed);
      const int ev = perfmon::g_gov.lastEvent;
      trialEvent |= ev == perfgov::kEvRecheck || ev == perfgov::kEvKept || ev == perfgov::kEvInconclusive ||
                    ev == perfgov::kEvRevertDown || ev == perfgov::kEvUnsteady;
    }
    Check(stepped && midTrial && off && !trialEvent && g_govStage.load() == 0,
          "governor: Enabled=0 during a trial -> stage 0 and the trial dropped (re-enabled, no stale verdict)");
    perfmon::SetStage(0);
    perfmon::g_gov = Governor();
  }
  // Bench: both states capped -> a note at the top of the result.
  {
    std::vector<BenchBlock> blocks;
    for (int i = 0; i < 4; ++i) {
      BenchBlock b;
      b.on = i % 2 == 1;
      b.seconds = 5;
      b.frames = 150;
      b.intervalsMs = Steady(1000.0 / 30, 150, 0.4, i + 3);
      b.procCycles = 1000000;
      blocks.push_back(b);
    }
    StartReportCapture();
    Summarize(blocks, 3e9, true, "cap test");
    const std::string r = StopReportCapture();
    const size_t head = r.find("==== BENCH RESULT"), note = r.find("NOTE: frame rate was locked at 30.0 fps (refresh 90 Hz / 3)");
    Check(head != std::string::npos && note != std::string::npos && note > head &&
              r.find("process_cpu_ms/frame") != std::string::npos,
          "bench: both states capped -> the note right under BENCH RESULT (compare process CPU per frame instead)");
  }
}

// Every new log line runs once with plausible data (a format/argument
// mismatch would show here, not in DCS); the captured text is checked.
void LinesRun() {
  RecSave save;
  Init();
  g_frames = 0;
  g_tid = 0;
  g_lastQpc = 0;
  LARGE_INTEGER f;
  QueryPerformanceFrequency(&f);
  int64_t q = f.QuadPart;
  for (int i = 0; i < 450; ++i) {
    OnCollect(62, 4, 2, q);
    q += f.QuadPart / 45;
  }
  perfmon::g_secCount = 0;
  perfmon::g_framesPerfAt = 0;
  perfmon::g_lastPerfT = 0;
  for (int i = 1; i <= 10; ++i) {
    perfmon::Sec s;
    s.t = i;
    s.frames = 45;
    s.renderPct = 58;
    s.gpuPct = 97;
    s.dcsPct = 90;
    s.verdict = i == 3 ? kMixed : kGpu;
    perfmon::g_sec[perfmon::g_secCount++ % perfmon::kSecs] = s;
  }
  StartReportCapture();
  perfmon::PerfLine(10.0);
  perfmon::LogViews(4, 2);
  perfmon::LogViews(2, 0);
  perfmon::LogEnvStart();
  const bool ad = perfmon::OpenFirstAdapter();
  if (ad) {
    perfmon::LogGpuEnv();
    perfmon::LogVram();
  }
  perfmon::OnToggle(true);
  perfmon::ToggleTick(perfmon::Now() + 13);
  // The governor stepping down and up through GovTick (both recorders "ready").
  g_cfg.shadowRecorder = true;
  g_cfg.gbufferRecorder = true;
  g_cfg.shadowInst = true;
  g_cfg.gbufferBatch = false;
  g_cfg.shadowRecorderSplit = 0xf;
  g_cfg.gbufferRecorderHelpers = 2;
  g_cfg.govEnabled = true;
  shrec::g_state = 1;
  gbrec::g_state = 1;
  shadowinst::g_onGb = true;
  shadowinst::g_gbInstalled = true;
  g_engineOff = false;
  perfmon::g_gov = Governor();
  perfmon::g_govAnnounced = false;
  perfmon::SetStage(0);
  double t = 1000;
  perfmon::g_gov.Reset(t);
  for (int i = 0; i < 12; ++i) {
    perfmon::Sec s;
    s.t = ++t;
    s.renderPct = 55;
    s.gpuPct = 98;
    s.verdict = kGpu;
    perfmon::GovTick(s);
  }
  const int down = g_govStage.load();
  for (int i = 0; i < 12; ++i) {
    perfmon::Sec s;
    s.t = ++t;
    s.renderPct = 96;
    s.gpuPct = 70;
    s.verdict = kCpu;
    perfmon::GovTick(s);
  }
  const int up = g_govStage.load();
  // With fps: a step down that loses fps is reverted and logged.
  perfmon::g_gov = Governor();
  perfmon::g_gov.Reset(t);
  for (int i = 0; i < 50; ++i) {
    perfmon::Sec s;
    s.t = ++t;
    s.frames = 45;
    s.fps = g_govStage.load() >= 1 ? 44.0 : 45.0;
    s.renderPct = 55;
    s.gpuPct = 98;
    s.verdict = kGpu;
    perfmon::GovTick(s);
  }
  const int reverted = g_govStage.load();
  g_cfg.govEnabled = false;
  perfmon::SetStage(2);
  perfmon::Sec s;
  s.t = ++t;
  perfmon::GovTick(s);
  const int offStage = g_govStage.load();
  const std::string text = StopReportCapture();
  perfmon::SetStage(0);
  perfmon::Shutdown();
  perfmon::g_kmt = perfmon::Kmt();
  perfmon::g_ad = perfmon::Adapter();
  perfmon::g_secCount = 0;
  g_tid = 0;
  g_frames = 0;
  auto has = [&](const char* k) { return text.find(k) != std::string::npos; };
  Check(has("perf: fps=45.0 ft p50/p95=22.2/22.2 ms render-thread busy=58% gpu util=97% (dcs 90%) verdict=capped 45.0 fps = 90/2 (9 s GPU") &&
            has("views=4 pairs=2"),
        "perf line: fps, frame p50/p95, busy, gpu, verdict with its seconds, views (exactly 45 fps: a 90/2 cap wins over GPU)");
  Check(has("quad views recognised yes") && has("quad views recognised NO (stereo"), "env views line: quad and stereo");
  Check(has("env: DCS ") && has("OpenXR runtime ") && has("streamers/runtimes running: "), "env start line");
  Check(!ad || (has("env: GPU ") && has("driver ") && has("VRAM budget")), "env GPU line (first adapter here)");
  Check(has("toggle: engine OFF; the 10 s before (engine ON)") && has("toggle: the 10 s after (engine OFF"),
        "toggle lines: before and after");
  Check(down == 1 && up == 0 && has("governor: on (") && has("governor: GPU-bound in") && has("-> stage 1") &&
            has("governor: not GPU-bound in") && has("-> stage 0"),
        "governor lines: announced, stepped down to 1 and back up to 0 through GovTick");
  Check(offStage == 0 && has("governor: off ([Governor] Enabled=0)"), "governor: Enabled=0 returns to stage 0");
  Check(reverted == 0 && has("trial: kept only if fps rises") && has("governor: step 1 reverted: fps 45.0 -> 44.0") &&
            has("stage 1 blocked for 300 s") && has("rechecking stage 0 for 10 s"),
        "governor lines: a step down that lowers fps is logged as reverted with both fps values");
}

void Run() {
  Sampler();
  Detector();
  Stages();
  Hysteresis();
  FpsProof();
  ApplyPath();
  StaleAcrossPause();
  KillSwitchReplay();
  RecycledHeap();
  StatusBits();
  Lines();
  LinesRun();
  CapTests();
  KmtFake();
  GpuBusy();
}

}  // namespace govtest
