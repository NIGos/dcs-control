// Scene-cut reset for the bundled Quad Views layer ([Perception]
// SceneCutReset, default 1). XR_APILAYER_MBUCCHIA_quad_views_foveated.dll
// exports `void DcsVr_QuadViewsTemporalReset() noexcept` (no arguments, any
// thread, bumps an atomic): it drops Periphery stability's history so a
// camera cut does not smear 1-3 frames of the old scene. The export is
// optional; without it (or without the layer) this is a no-op. Default on:
// the call is harmless, a false positive only costs the layer's history for
// a frame or two, a missed cut costs a visible smear.
//
// Detection (Prepare, the culling thread, once per quad frame): the left
// periphery view's camera (frustum apex and forward, decoded from DCS's own
// clipping volume every frame) against its recent motion:
//  - gap: more than 1 s since the last quad frame (mission load or restart:
//    the render graph is rebuilt and no quad frame is culled meanwhile);
//  - jump: the position leaves the prediction from the smoothed velocity by
//    more than 3 m + 35 % of the predicted step (frame-time jitter) +
//    1/2 * 150 m/s^2 * dt^2 (a 15 g jet). Head translation is well under
//    0.1 m per frame and a 600 m/s jet stays on its prediction, while an
//    F1/F2/F10 switch, an external-view change or a respawn moves the camera
//    by metres to kilometres at once. Before a velocity is known (first
//    frame after a start or a cut): more than 700 m/s * dt + 3 m;
//  - turn: the forward direction turns by more than max(40°, 1200°/s * dt)
//    in one frame (beyond any head rotation).
// After a cut the velocity estimate restarts.
// Log: each cut (at most one line per 2 s), and every 60 s the counts.
// Included once from main.cpp inside its anonymous namespace (Log, LoadConfig).
#pragma once

namespace scenecut {

enum Reason { kNone = 0, kGap = 1, kJump = 2, kTurn = 3 };
const char* const kReasonName[] = {"none", "gap", "jump", "turn"};

struct Detector {
  bool have = false, haveVel = false;
  double p[3] = {}, f[3] = {}, v[3] = {};
  double t = 0;
  // The last update, for the log: residual against the prediction and its
  // limit (0 for a gap), the raw move, its part along the previous view
  // direction, the turn between the two view directions, dt.
  double lastResid = 0, lastThr = 0, lastMove = 0, lastAhead = 0, lastAngleDeg = 0, lastDt = 0;

  static void Unit(const double in[3], double out[3]) {
    const double n = std::sqrt(in[0] * in[0] + in[1] * in[1] + in[2] * in[2]);
    for (int k = 0; k < 3; ++k) out[k] = n > 1e-12 ? in[k] / n : 0.0;
  }

  void Reset() {
    have = haveVel = false;
  }
  // pos (m), fwd (unit), tSec (monotonic seconds). Returns the cut reason.
  int Update(const double pos[3], const double fwdIn[3], double tSec) {
    double fwd[3];
    Unit(fwdIn, fwd);  // acos of a dot product needs unit vectors
    if (!have) {
      memcpy(p, pos, sizeof(p));
      memcpy(f, fwd, sizeof(f));
      t = tSec;
      have = true;
      haveVel = false;
      return kNone;
    }
    const double rawDt = tSec - t;
    lastDt = rawDt;
    int reason = kNone;
    if (rawDt > 1.0) reason = kGap;
    // The real interval up to the gap limit: a stall of 0.5-1 s moves the
    // camera v * rawDt, which the prediction must use (clamped to 0.5 s, a
    // 600 m/s jet read as a jump of up to 300 m).
    const double dt = std::min(1.0, std::max(0.002, rawDt));
    double d[3], step = 0;
    for (int k = 0; k < 3; ++k) d[k] = pos[k] - p[k];
    lastMove = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    lastAhead = d[0] * f[0] + d[1] * f[1] + d[2] * f[2];
    lastResid = lastMove;
    lastThr = 0;  // a gap has no motion limit
    if (reason == kNone) {
      double resid = 0, thr;
      if (haveVel) {
        double vn = 0;
        for (int k = 0; k < 3; ++k) {
          const double e = d[k] - v[k] * dt;
          resid += e * e;
          vn += v[k] * v[k];
        }
        resid = std::sqrt(resid);
        step = std::sqrt(vn) * dt;
        thr = 3.0 + 0.35 * step + 75.0 * dt * dt;
      } else {
        resid = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        thr = 700.0 * dt + 3.0;
      }
      lastResid = resid;
      lastThr = thr;
      if (resid > thr) reason = kJump;
    }
    const double c = std::max(-1.0, std::min(1.0, f[0] * fwd[0] + f[1] * fwd[1] + f[2] * fwd[2]));
    lastAngleDeg = std::acos(c) * 57.29577951308232;
    if (reason == kNone && lastAngleDeg > std::max(40.0, 1200.0 * dt)) reason = kTurn;
    if (reason == kNone) {
      for (int k = 0; k < 3; ++k) {
        const double vn = d[k] / dt;
        v[k] = haveVel ? 0.5 * v[k] + 0.5 * vn : vn;
      }
      haveVel = true;
    } else {
      haveVel = false;
    }
    memcpy(p, pos, sizeof(p));
    memcpy(f, fwd, sizeof(f));
    t = tSec;
    return reason;
  }
};

// The export (resolved by Resolve, retried until found). Only a note of what
// was found (Tick's log): every call resolves the export again from the
// loaded module (CallReset), since the OpenXR loader may unload the layer
// (xrDestroyInstance) and load it again at another address mid-session.
using ResetFn = void (*)();
std::atomic<ResetFn> g_reset{nullptr};
std::wstring g_moduleName = L"XR_APILAYER_MBUCCHIA_quad_views_foveated.dll";  // tests use their own
std::string g_exportName = "DcsVr_QuadViewsTemporalReset";  // tests use their own

std::atomic<bool> g_on{true};
std::atomic<uint64_t> g_cuts{0}, g_calls{0}, g_cutsByReason[4] = {};
Detector g_det;           // culling thread only
uint64_t g_lastLogMs = 0;  // culling thread only
std::atomic<int> g_resolveState{0};  // 0 not tried / pending, 1 resolved, -1 module loaded but no export

// Looks the export up while the layer is loaded (for the log and the counts).
// Cheap; called every second from the config loop (the layer may load after
// us, or unload and load again).
void Resolve() {
  HMODULE m = GetModuleHandleW(g_moduleName.c_str());
  if (!m) {  // not loaded (yet, or unloaded since)
    g_reset = nullptr;
    if (g_resolveState.load() == 1) g_resolveState = 0;
    return;
  }
  auto fn = reinterpret_cast<ResetFn>(GetProcAddress(m, g_exportName.c_str()));
  if (fn) {
    g_resolveState = 1;
    if (g_reset.exchange(fn) != fn) Log("scene cut: %s found in %ls", g_exportName.c_str(), g_moduleName.c_str());
  } else {
    g_reset = nullptr;
    if (g_resolveState.exchange(-1) != -1)
      Log("scene cut: %ls is loaded but does not export %s (older layer): cuts are only counted", g_moduleName.c_str(),
          g_exportName.c_str());
  }
}

// Calls the layer's export if the layer is loaded now: the module is looked
// up and held (a reference) for the duration of the call, so an unloaded
// layer is never called through a stale pointer and a reloaded one is called
// at its new address. Cuts are rare (a few per session): the lookup's cost
// does not matter. True when the export was called.
bool CallReset() {
  HMODULE m = nullptr;
  if (!GetModuleHandleExW(0, g_moduleName.c_str(), &m) || !m) {
    g_reset = nullptr;
    if (g_resolveState.load() == 1) g_resolveState = 0;  // layer not loaded (Resolve finds it again)
    return false;
  }
  const auto fn = reinterpret_cast<ResetFn>(GetProcAddress(m, g_exportName.c_str()));
  g_reset = fn;
  if (fn) fn();
  FreeLibrary(m);
  return fn != nullptr;
}

// One quad frame's camera (culling thread).
void OnCamera(const double pos[3], const double fwd[3]) {
  if (!g_on.load(std::memory_order_relaxed)) {
    g_det.Reset();
    return;
  }
  LARGE_INTEGER q, fq;
  QueryPerformanceCounter(&q);
  QueryPerformanceFrequency(&fq);
  const int r = g_det.Update(pos, fwd, static_cast<double>(q.QuadPart) / static_cast<double>(fq.QuadPart));
  if (r == kNone) return;
  g_cuts++;
  g_cutsByReason[r]++;
  if (CallReset()) g_calls++;
  const uint64_t now = GetTickCount64();
  if (now - g_lastLogMs >= 2000) {
    g_lastLogMs = now;
    char lim[48];
    if (r == kGap)
      snprintf(lim, sizeof(lim), "no frame for %.0f ms", g_det.lastDt * 1000.0);
    else
      snprintf(lim, sizeof(lim), "off the prediction by %.1f m, %.1f allowed", g_det.lastResid, g_det.lastThr);
    // The turn is between the view directions before and after the cut: an
    // F2 camera that faces the aircraft reads close to 180° against F1.
    Log("scene cut (%s): camera moved %.1f m (%+.1f m along the old view direction), %s; view direction turned "
        "%.1f deg; dt %.1f ms; Quad Views reset %s",
        kReasonName[r], g_det.lastMove, g_det.lastAhead, lim, g_det.lastAngleDeg, g_det.lastDt * 1000.0,
        g_reset.load() ? "called" : "not available");
  }
}

// Once per second from the config loop.
void Tick(bool on) {
  g_on = on;
  if (on) Resolve();
  static ULONGLONG last = 0;
  const ULONGLONG now = GetTickCount64();
  if (!last) last = now;
  if (now - last >= 60000) {
    last = now;
    Log("scene cut: %llu cuts (gap %llu, jump %llu, turn %llu), %llu Quad Views resets; export %s",
        static_cast<unsigned long long>(g_cuts.load()), static_cast<unsigned long long>(g_cutsByReason[kGap].load()),
        static_cast<unsigned long long>(g_cutsByReason[kJump].load()),
        static_cast<unsigned long long>(g_cutsByReason[kTurn].load()), static_cast<unsigned long long>(g_calls.load()),
        g_reset.load() ? "found" : g_resolveState.load() < 0 ? "missing in the layer" : "layer not loaded");
  }
}

}  // namespace scenecut
