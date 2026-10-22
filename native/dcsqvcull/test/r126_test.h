// R126: "optimise our optimisations" (docs/research/R126_overhead_optimisation.md). Lossless speed-ups of our own
// render-thread code; each one is checked here against the code it replaced, which is kept verbatim as a reference.
//  S1 cockpit ipd's cost scopes count TSC ticks (one __rdtsc per edge instead of a QueryPerformanceCounter): the
//     logged microseconds stay right (TscPerSec), nothing a hook decides depends on them.
//  S3 fixation_depth.h FD_DRAW_BODY times its draw only while cockpit ipd is on (CostScopeIf).
//  S2 ovl_cpu.h MatchWorld (the overlay learner's world-matrix search): the affine test on the four floats it reads
//     before the 16-float load, and no second product when vpA and vpB are one object (an unmoved replay).
#pragma once

#include <random>

namespace r126test {

namespace ck = ::fixdepth::ckipd;
namespace oc = ::fixdepth::ckipd::ocpu;
using ck::M4;
using ck::LoadM;
using ck::MulM;
using ck::StoreM;
using ck::kMaxSrc;

// MatchWorld as it was before R126 (R125 main), verbatim.
int MatchWorldRef(const uint8_t* data, UINT len, const M4& mvp, UINT mvpOff, const M4& vpA, const M4& vpB, UINT* off,
               int* order, bool cached) {
  double sc = 0;
  for (int r = 0; r < 4; ++r)
    for (int c = 0; c < 4; ++c) sc = std::max(sc, std::fabs(mvp.a[r][c]));
  if (!(sc > 0)) return 0;
  auto test = [&](UINT o, int ord) -> int {
    if (o + 64 > len || (o < mvpOff + 64 && mvpOff < o + 64)) return 0;
    M4 w = LoadM(data, o);
    if (ord) {
      M4 tr;
      for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) tr.a[r][c] = w.a[c][r];
      w = tr;
    }
    if (std::fabs(w.a[0][3]) + std::fabs(w.a[1][3]) + std::fabs(w.a[2][3]) + std::fabs(w.a[3][3] - 1) > 1e-4) return 0;
    for (int k = 1; k <= 2; ++k) {
      const M4 p = MulM(w, k == 1 ? vpA : vpB);
      double dev = 0;
      for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) dev = std::max(dev, std::fabs(p.a[r][c] - mvp.a[r][c]));
      if (dev <= 1e-4 * sc) return k;
    }
    return 0;
  };
  if (cached) {
    const int k = test(*off, *order);
    if (k) return k;
  }
  for (UINT o = 0; o + 64 <= len && o + 64 <= kMaxSrc; o += 16)
    for (int ord = 0; ord < 2; ++ord) {
      const int k = test(o, ord);
      if (k) {
        *off = o;
        *order = ord;
        return k;
      }
    }
  return 0;
}

M4 RndM(std::mt19937& g, double s) {
  std::uniform_real_distribution<double> u(-s, s);
  M4 m;
  for (auto& r : m.a)
    for (double& v : r) v = u(g);
  return m;
}

// S2: the same result, offset and order as the reference on random buffers with planted world matrices (either
// order, near the 1e-4 tolerance, NaNs, zero MVPs, the matrix overlapping the MVP), cached and uncached, with vpA
// and vpB one object or two (equal or different).
void TestMatchWorld() {
  std::mt19937 g(126);
  uint64_t cases = 0, diff = 0, found = 0, foundB = 0, foundAlias = 0;
  for (int it = 0; it < 40000; ++it) {
    const UINT len = 64 + 16 * (g() % 160);
    std::vector<uint8_t> buf(len);
    std::uniform_real_distribution<float> uf(-2, 2);
    for (UINT i = 0; i + 4 <= len; i += 4) {
      const float v = (g() % 7 == 0) ? 0.0f : (g() % 11 == 0 ? 1.0f : uf(g));
      memcpy(&buf[i], &v, 4);
    }
    if (g() % 50 == 0) {
      const float n = NAN;
      memcpy(&buf[(g() % (len / 4)) * 4], &n, 4);
    }
    const M4 vpA = RndM(g, 3);
    const int bKind = g() % 3;  // 0: the same object, 1: an equal copy, 2: another matrix
    const M4 vpCopy = vpA, vpOther = RndM(g, 3);
    const M4& vpB = bKind == 0 ? vpA : bKind == 1 ? vpCopy : vpOther;
    M4 w = RndM(g, 5);
    w.a[0][3] = w.a[1][3] = w.a[2][3] = 0;
    w.a[3][3] = 1;
    if (g() % 3 == 0) w.a[1][3] = (g() & 1) ? 2e-5 : 9.9e-5;  // inside the tolerance
    if (g() % 9 == 0) w.a[2][3] = 1.2e-4;                       // just outside
    const UINT slots = len / 16;
    const UINT wOff = 16 * (slots > 3 ? g() % (slots - 3) : 0);
    const int wOrd = g() & 1;
    if (wOff + 64 <= len && g() % 4) {
      M4 st = w;
      if (wOrd)
        for (int r = 0; r < 4; ++r)
          for (int c = 0; c < 4; ++c) st.a[r][c] = w.a[c][r];
      StoreM(buf.data(), wOff, st);
    }
    const int which = g() % 3;
    M4 mvp = which == 0 ? MulM(w, vpA) : which == 1 ? MulM(w, vpB) : RndM(g, 4);
    if (g() % 9 == 0) mvp = M4{};
    const UINT mvpOff = 16 * (g() % slots);
    for (int cached = 0; cached < 2; ++cached) {
      UINT o1 = cached ? wOff : 0, o2 = o1;
      int r1 = cached ? wOrd : 0, r2 = r1;
      const int k1 = MatchWorldRef(buf.data(), len, mvp, mvpOff, vpA, vpB, &o1, &r1, cached != 0);
      const int k2 = oc::MatchWorld(buf.data(), len, mvp, mvpOff, vpA, vpB, &o2, &r2, cached != 0);
      ++cases;
      if (k1) ++found;
      if (k1 == 2) ++foundB;
      if (k1 && bKind == 0) ++foundAlias;
      if (k1 != k2 || o1 != o2 || r1 != r2) ++diff;
    }
  }
  printf("  (MatchWorld: %llu cases, %llu matched, %llu by vpB, %llu with one VP object; %llu differ)\n",
         static_cast<unsigned long long>(cases), static_cast<unsigned long long>(found),
         static_cast<unsigned long long>(foundB), static_cast<unsigned long long>(foundAlias),
         static_cast<unsigned long long>(diff));
  Check(diff == 0, "R126 S2: MatchWorld gives the reference's result, offset and order on every case");
  Check(found > 10000 && foundB > 1000 && foundAlias > 1000, "R126 S2: the cases cover matches by vpA, by vpB and with one VP object");
}

// S1: a scope adds TSC ticks that LogStats' rate turns back into the wall time (a 2 ms spin reads 2 ms, within the
// scheduling noise of a test machine); the rate is a plausible clock.
void TestCostScope() {
  const double tps = ck::TscPerSec();
  Check(tps > 1e8 && tps < 2e10, "R126 S1: TscPerSec is a plausible TSC rate");
  Check(ck::TscPerSec() == tps, "R126 S1: TscPerSec is measured once");
  const uint64_t before = ck::g_cost[ck::kCostDraw].exchange(0);
  LARGE_INTEGER f, a, b;
  QueryPerformanceFrequency(&f);
  {
    ck::CostScope s(ck::kCostDraw);
    QueryPerformanceCounter(&a);
    do {
      QueryPerformanceCounter(&b);
    } while (b.QuadPart - a.QuadPart < f.QuadPart / 500);
  }
  const double us = ck::g_cost[ck::kCostDraw].exchange(before) * 1e6 / tps;
  printf("  (cost scope over a 2 ms spin: %.1f us; TSC %.3f GHz)\n", us, tps / 1e9);
  Check(us >= 1900 && us < 20000, "R126 S1: a cost scope measures its interval in TSC ticks (2 ms spin -> 2 ms)");
  // S3: the draw hooks' conditional scope adds nothing while cockpit ipd is off and times like CostScope when on.
  const uint64_t keep = ck::g_cost[ck::kCostDraw].exchange(0);
  { ck::CostScopeIf off(ck::kCostDraw, false); }
  Check(ck::g_cost[ck::kCostDraw].load() == 0, "R126 S3: a draw hook's cost scope adds nothing while cockpit ipd is off");
  {
    ck::CostScopeIf on(ck::kCostDraw, true);
    QueryPerformanceCounter(&a);
    do {
      QueryPerformanceCounter(&b);
    } while (b.QuadPart - a.QuadPart < f.QuadPart / 1000);
  }
  const double us3 = ck::g_cost[ck::kCostDraw].exchange(keep) * 1e6 / tps;
  Check(us3 >= 950 && us3 < 20000, "R126 S3: a draw hook's cost scope times its draw while cockpit ipd is on (1 ms -> 1 ms)");
}

// FocusTwin = 0 (the default) costs nothing: not installed, so no observer around the G-buffer executions, no collect
// tap, no worker pool; what remains is the two null checks the shared hooks always made (R124: while ON it costs
// 1.0-1.9 ms of render thread and never matched live).
void TestFocusTwinOff() {
  Check(::ftwin::g_state.load() != 1 && ::gbpass::g_obs2.load() == nullptr && ::g_collectTap.load() == nullptr,
        "R126: with FocusTwin off nothing of the focus twin is installed (no observer, no collect tap)");
}

// R126 (log only, Dev's requests after R124): (1) with DCS Control's OpenXR layer hosting the driver preset filter
// (DCSQV_DLSS_DRIVER_PRESET_HIDDEN=1 in the process), the late IgnoreDriverPreset line says so instead of "not
// possible"; nothing is installed either way. (2) The create line ends with the caller (module + offset) and the
// handle DLSS returned, after the unchanged fields.
void TestDlssLines() {
  using namespace dlssv;
  using namespace r122test;
  uint8_t* fakeCreate = MakeFakeCreate();
  uint8_t* lateGs = MakeFakeGetSetting();
  if (!fakeCreate || !lateGs) {
    QvSkip("R126 dlss views lines: no executable memory\n");
    return;
  }
  wchar_t saved[8] = {};
  const DWORD hadEnv = GetEnvironmentVariableW(kHiddenEnv, saved, 8);
  NewGeneration();  // (a new process: clears the variable)
  SetEnvironmentVariableW(kHiddenEnv, L"1");  // DCS Control's layer hosts the filter
  g_drsState = 1;
  g_drs.state = 1, g_drs.appPresetSet = true, g_drs.appPreset = 13;
  g_testGetSettingTarget = lateGs;
  g_testNgxUp = true;
  g_testCreateTarget = fakeCreate;
  g_testQvfrLog = WriteQvfrLog(kQvfrText);
  StartReportCapture();
  Tick(5, 0, true, false);
  const std::string late = StopReportCapture();
  Check(g_getSettingState.load() == -2 && memcmp(lateGs, kGetSettingPrologue, 15) == 0 &&
            late.find("IgnoreDriverPreset: driver preset filter hosted by the OpenXR layer") != std::string::npos &&
            late.find("not possible in this DCS session") == std::string::npos,
        "R126: with the OpenXR layer hosting the driver preset filter the late line says so; nothing installed");
  int ctxDummy = 0;
  void* h = nullptr;
  FakeParam p;
  StartReportCapture();
  DcsFeature(p, 604, 596, 1040, 1028);
  reinterpret_cast<CreateFn>(fakeCreate)(&ctxDummy, kFeatureSuperSampling, &p, &h);
  const std::string line = StopReportCapture();
  Check(line.find("dlss views: create #1 periphery 604x596->1040x1028: preset E requested") != std::string::npos &&
            line.find("; caller test_main.exe+0x") != std::string::npos && line.find("; handle 0x") != std::string::npos,
        "R126: the create line keeps its prefix and adds the caller (module + offset) and the handle");
  Stop();
  if (hadEnv)
    SetEnvironmentVariableW(kHiddenEnv, saved);
  else
    SetEnvironmentVariableW(kHiddenEnv, nullptr);
  NewGeneration();
}

void Run() {
  printf("-- R126 overhead optimisation (lossless) --\n");
  TestMatchWorld();
  TestCostScope();
  TestFocusTwinOff();
  TestDlssLines();
}

}  // namespace r126test
