// Offline tests for scene_cut.h: the camera-cut detector on synthetic camera
// sequences (head motion in a parked cockpit, fast flight with a hard turn
// and frame-time jitter, an F1 -> F2 switch parked and at 300 m/s, a
// respawn, a mission-load gap, an instant 90° turn) and the export: resolve
// through the module/export seam, call on a cut, a missing export is a no-op.
#pragma once

namespace sctest {
std::atomic<int> g_fakeResets{0};  // calls of the stand-in export below
std::atomic<int> g_staleCalls{0};  // calls through a stale (cached) function pointer
}  // namespace sctest

// Stands in for the layer's DcsVr_QuadViewsTemporalReset: found by name in
// this executable's export table (scenecut::g_moduleName = test_main.exe).
extern "C" __declspec(dllexport) void DcsQvCullTest_SceneCutReset() { sctest::g_fakeResets++; }

namespace sctest {

using scenecut::Detector;

// The address a previous copy of the layer exported (unloaded since): must
// never be called.
void StaleReset() { g_staleCalls++; }

struct Cam {
  double p[3], f[3];
};

// Runs a sequence; returns the frame indices that were cuts.
std::vector<int> RunSeq(const std::vector<Cam>& seq, const std::vector<double>& t, std::vector<int>* reasons = nullptr) {
  Detector d;
  std::vector<int> cuts;
  for (size_t i = 0; i < seq.size(); ++i) {
    const int r = d.Update(seq[i].p, seq[i].f, t[i]);
    if (r != scenecut::kNone) {
      cuts.push_back(static_cast<int>(i));
      if (reasons) reasons->push_back(r);
    }
  }
  return cuts;
}

// Frame times at `fps` with +-`jitter` (fraction), deterministic.
std::vector<double> Times(int n, double fps, double jitter) {
  std::vector<double> t(n);
  double now = 100.0;
  uint32_t x = 12345;
  for (int i = 0; i < n; ++i) {
    t[i] = now;
    x = x * 1664525u + 1013904223u;
    const double u = (x >> 8) / 16777216.0 * 2.0 - 1.0;
    now += (1.0 / fps) * (1.0 + jitter * u);
  }
  return t;
}

void Run() {
  printf("-- scene_cut.h --\n");
  const double kPi = 3.14159265358979;
  // 1. Parked cockpit, head motion: 0.15 m sway at 1.5 Hz, yaw +-60° at 1 Hz
  //    (peak about 380°/s), 50 fps with 30 % jitter, 20 s.
  {
    const int n = 1000;
    std::vector<double> t = Times(n, 50, 0.3);
    std::vector<Cam> s(n);
    for (int i = 0; i < n; ++i) {
      const double a = 60.0 * kPi / 180.0 * std::sin(2 * kPi * 1.0 * t[i]);
      s[i] = {{-497832.0 + 0.15 * std::sin(2 * kPi * 1.5 * t[i]), 248.0, -933662.0 + 0.1 * std::cos(2 * kPi * t[i])},
              {std::cos(a), 0, std::sin(a)}};
    }
    Check(RunSeq(s, t).empty(), "scene cut: parked head motion (sway, 380°/s yaw, 30 % frame jitter) is no cut");
  }
  // 2. Fast flight: 600 m/s, a 9 g level turn (radius 4.1 km), 90 fps with
  //    25 % jitter and a 60 ms hitch, head yaw on top.
  {
    const int n = 2000;
    std::vector<double> t = Times(n, 90, 0.25);
    for (int i = 1000; i < n; ++i) t[i] += 0.06;  // one long frame
    std::vector<Cam> s(n);
    const double v = 600, r = v * v / (9 * 9.81), w = v / r;
    for (int i = 0; i < n; ++i) {
      const double a = w * (t[i] - t[0]);
      const double yaw = a + 0.5 * std::sin(3 * t[i]);
      s[i] = {{r * std::sin(a), 3000.0, r * (1 - std::cos(a))}, {std::cos(yaw), 0, std::sin(yaw)}};
    }
    Check(RunSeq(s, t).empty(), "scene cut: 600 m/s with a 9 g turn, frame jitter and a 60 ms hitch is no cut");
  }
  // 2b. A 0.6-0.95 s stall in level flight (a loading hitch, below the 1 s
  //     gap): the camera moved v * dt, which is what was predicted. The
  //     prediction used dt clamped to 0.5 s, so 600 m/s read as a jump of
  //     60-270 m.
  for (const double hitch : {0.6, 0.8, 0.95}) {
    const int n = 400;
    std::vector<double> t = Times(n, 90, 0.1);
    for (int i = 200; i < n; ++i) t[i] += hitch;
    std::vector<Cam> s(n);
    for (int i = 0; i < n; ++i) s[i] = {{600.0 * (t[i] - t[0]), 3000.0, 0}, {1, 0, 0}};
    char name[128];
    snprintf(name, sizeof(name), "scene cut: 600 m/s with a %.2f s stall (under the 1 s gap) is no cut", hitch);
    Check(RunSeq(s, t).empty(), name);
  }
  // 3. F1 -> F2 while parked: the camera moves 18 m back and 4 m up at once.
  {
    const int n = 200;
    std::vector<double> t = Times(n, 50, 0.1);
    std::vector<Cam> s(n);
    for (int i = 0; i < n; ++i) s[i] = {{i < 100 ? 0.0 : -18.0, i < 100 ? 1.0 : 5.0, 0}, {1, 0, 0}};
    std::vector<int> why;
    const std::vector<int> c = RunSeq(s, t, &why);
    Check(c.size() == 1 && c[0] == 100 && why[0] == scenecut::kJump, "scene cut: F1 -> F2 parked is one cut (jump)");
  }
  // 4. F1 -> F2 at 300 m/s: the same 18 m offset on top of the flight path.
  {
    const int n = 400;
    std::vector<double> t = Times(n, 60, 0.2);
    std::vector<Cam> s(n);
    for (int i = 0; i < n; ++i) {
      const double x = 300.0 * (t[i] - t[0]);
      s[i] = {{x - (i >= 200 ? 18.0 : 0.0), 500.0 + (i >= 200 ? 4.0 : 0.0), 0}, {1, 0, 0}};
    }
    const std::vector<int> c = RunSeq(s, t);
    Check(c.size() == 1 && c[0] == 200, "scene cut: F1 -> F2 at 300 m/s is one cut");
  }
  // 5. Respawn: 40 km away, then parked again; and a mission load (3 s gap).
  {
    const int n = 300;
    std::vector<double> t = Times(n, 50, 0.1);
    for (int i = 200; i < n; ++i) t[i] += 3.0;
    std::vector<Cam> s(n);
    for (int i = 0; i < n; ++i) s[i] = {{i < 100 ? 0.0 : 40000.0, 2.0, 0}, {1, 0, 0}};
    std::vector<int> why;
    const std::vector<int> c = RunSeq(s, t, &why);
    Check(c.size() == 2 && c[0] == 100 && why[0] == scenecut::kJump && c[1] == 200 && why[1] == scenecut::kGap,
          "scene cut: respawn (jump) and a 3 s mission-load gap are one cut each, nothing after");
  }
  // 6. An instant 90° turn without moving (external view snap).
  {
    const int n = 100;
    std::vector<double> t = Times(n, 90, 0.1);
    std::vector<Cam> s(n);
    for (int i = 0; i < n; ++i) s[i] = {{0, 2, 0}, {i < 50 ? 1.0 : 0.0, 0, i < 50 ? 0.0 : 1.0}};
    std::vector<int> why;
    const std::vector<int> c = RunSeq(s, t, &why);
    Check(c.size() == 1 && c[0] == 50 && why[0] == scenecut::kTurn, "scene cut: an instant 90° turn is one cut (turn)");
  }
  // The logged numbers: the turn is the angle between the view directions
  // before and after (live: F1 -> the bench's F2 camera, 41 m in front of the
  // cockpit facing back at the aircraft, read 179.3°), the move and its part
  // along the old view direction are real, also for a gap; directions need
  // not be unit length.
  {
    Detector d;
    const double t0 = 10.0;
    const double eye[3] = {0, 2, 0}, fwdF1[3] = {2, 0, 0};  // not unit length
    const double cam[3] = {41, 3, 0}, fwdF2[3] = {-0.9998, -0.02, 0};
    d.Update(eye, fwdF1, t0);
    d.Update(eye, fwdF1, t0 + 0.011);
    const int r = d.Update(cam, fwdF2, t0 + 0.022);
    Check(r == scenecut::kJump && fdtest::Near(d.lastMove, std::sqrt(41.0 * 41 + 1), 1e-6) && fdtest::Near(d.lastAhead, 41, 1e-6) &&
              fdtest::Near(d.lastAngleDeg, 180.0 - std::atan2(0.02, 0.9998) * 57.29577951308232, 1e-3),
          "scene cut log: F1 -> an F2 camera in front facing back = jump, 41 m ahead, about 179° turn");
    const double yaw30[3] = {std::cos(30 * kPi / 180), 0, std::sin(30 * kPi / 180)};
    Detector e;
    e.Update(eye, fwdF1, t0);
    e.Update(eye, yaw30, t0 + 0.011);
    Check(fdtest::Near(e.lastAngleDeg, 30, 1e-6) && fdtest::Near(e.lastMove, 0, 1e-12), "scene cut log: a 30° turn reads 30°, no move");
    Detector g;
    g.Update(eye, fwdF1, t0);
    const int rg = g.Update(cam, fwdF1, t0 + 4.66);
    Check(rg == scenecut::kGap && fdtest::Near(g.lastMove, std::sqrt(41.0 * 41 + 1), 1e-6) && g.lastThr == 0 &&
              fdtest::Near(g.lastAngleDeg, 0, 1e-9),
          "scene cut log: a gap reports the real move (not 0) and no turn for the same direction");
  }
  // Export: resolve through the seam (a real export of kernel32), a missing
  // export, and the call on a cut through a fake function.
  {
    const std::wstring oldMod = scenecut::g_moduleName;
    const std::string oldExp = scenecut::g_exportName;
    scenecut::g_reset = nullptr;
    scenecut::g_resolveState = 0;
    scenecut::g_moduleName = L"no_such_layer_dll.dll";
    scenecut::Resolve();
    Check(!scenecut::g_reset.load() && scenecut::g_resolveState.load() == 0, "scene cut: layer not loaded -> not resolved");
    scenecut::g_moduleName = L"kernel32.dll";
    scenecut::g_exportName = "DcsVr_NoSuchExport";
    scenecut::Resolve();
    Check(!scenecut::g_reset.load() && scenecut::g_resolveState.load() == -1, "scene cut: export missing -> no-op");
    scenecut::g_exportName = "GetCurrentProcessId";
    scenecut::Resolve();
    Check(scenecut::g_reset.load() == reinterpret_cast<scenecut::ResetFn>(
                                          GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetCurrentProcessId")) &&
              scenecut::g_resolveState.load() == 1,
          "scene cut: the export is resolved once the module is loaded");
    // Call on a cut (this executable's export stands in for the layer).
    scenecut::g_moduleName = L"test_main.exe";
    scenecut::g_exportName = "DcsQvCullTest_SceneCutReset";
    scenecut::g_reset = nullptr;
    scenecut::g_resolveState = 0;
    scenecut::Resolve();
    scenecut::g_on = true;
    scenecut::g_det.Reset();
    const uint64_t calls0 = scenecut::g_calls.load(), cuts0 = scenecut::g_cuts.load();
    const double a[3] = {0, 2, 0}, b[3] = {5000, 2, 0}, f[3] = {1, 0, 0};
    scenecut::OnCamera(a, f);
    Sleep(15);
    scenecut::OnCamera(a, f);
    Sleep(15);
    scenecut::OnCamera(b, f);  // respawn
    Check(scenecut::g_resolveState.load() == 1 && g_fakeResets.load() == 1 && scenecut::g_calls.load() == calls0 + 1 &&
              scenecut::g_cuts.load() == cuts0 + 1,
          "scene cut: a cut calls the export once");
    scenecut::g_exportName = "DcsVr_NoSuchExport";  // an older layer
    Sleep(15);
    scenecut::OnCamera(a, f);  // back: another cut, no export -> counted, no call
    Check(g_fakeResets.load() == 1 && scenecut::g_calls.load() == calls0 + 1 && scenecut::g_cuts.load() == cuts0 + 2,
          "scene cut: without the export a cut is only counted");
    // The layer unloaded mid-session (the OpenXR loader frees API layers at
    // xrDestroyInstance): the pointer resolved earlier is stale and must not
    // be called (it was: an access violation or a jump into whatever was
    // mapped there next).
    scenecut::g_moduleName = L"no_such_layer_dll.dll";
    scenecut::g_exportName = "DcsQvCullTest_SceneCutReset";
    scenecut::g_reset = &StaleReset;
    Sleep(15);
    scenecut::OnCamera(b, f);
    Check(g_staleCalls.load() == 0 && scenecut::g_calls.load() == calls0 + 1 && scenecut::g_cuts.load() == cuts0 + 3 &&
              !scenecut::g_reset.load(),
          "scene cut: a layer unloaded mid-session -> the stale pointer is not called (cut only counted)");
    // Reloaded (another copy, another address): the current export is called,
    // never the previous copy's.
    scenecut::g_moduleName = L"test_main.exe";
    scenecut::g_reset = &StaleReset;
    Sleep(15);
    scenecut::OnCamera(a, f);
    Check(g_staleCalls.load() == 0 && g_fakeResets.load() == 2 && scenecut::g_calls.load() == calls0 + 2 &&
              scenecut::g_reset.load() == reinterpret_cast<scenecut::ResetFn>(&DcsQvCullTest_SceneCutReset),
          "scene cut: a layer reloaded mid-session -> its current export is called, not the old pointer");
    scenecut::g_on = false;
    scenecut::OnCamera(b, f);
    Check(scenecut::g_cuts.load() == cuts0 + 4, "scene cut: [Perception] SceneCutReset=0 detects nothing");
    scenecut::g_on = true;
    scenecut::g_det.Reset();
    scenecut::g_moduleName = oldMod;
    scenecut::g_exportName = oldExp;
    scenecut::g_resolveState = 0;
  }
}

}  // namespace sctest
