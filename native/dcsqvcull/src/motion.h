// Motion profile: how the per-frame cost changes with head/camera motion.
// While recording, each quad frame is put in a bucket by the head's angular
// speed (from the peripheral view direction) and the camera's linear speed;
// per bucket it accumulates frame time, culling time, render-pass time,
// render-thread CPU, renderable counts, texture-streaming requests that reach
// dx11backend (not deduped) and model-allocator traffic.
// Start: create run_motion.flag (next to the DLL or the dev ini) or as a suite
// phase; it records [Suite] MotionSeconds, then writes motion_*.txt.
// Included once from main.cpp inside its anonymous namespace.
#pragma once

namespace motion {

constexpr int kBuckets = 5;
const char* const kBucketNames[kBuckets] = {"still (<5 deg/s)", "slow (5-30 deg/s)", "medium (30-90 deg/s)",
                                            "fast (90-180 deg/s)", "very fast (>180 deg/s)"};
const double kBucketLimits[kBuckets - 1] = {5, 30, 90, 180};

struct Bucket {
  uint64_t frames = 0;
  std::vector<float> frameMs;
  double collectMs = 0, passMs = 0, rtCpuMs = 0, periph = 0, focus = 0, shadow = 0, streamCalls = 0,
         streamOrig = 0, allocKB = 0, linSpeed = 0;
  double probeMs[mprobe::kProbes] = {}, probeCalls[mprobe::kProbes] = {};
};

std::atomic<bool> g_recording{false};
FILE* g_csv = nullptr;  // one line per quad frame while recording
std::mutex g_mutex;
Bucket g_b[kBuckets];
bool g_havePrev = false;
Vec3 g_prevFwd{}, g_prevPos{};
int64_t g_prevQpc = 0;
uint64_t g_prevRtCycles = 0;
double g_prevPassUs = 0;
texbind::Totals g_prevTex;
allocslab::Totals g_prevAlloc;
mprobe::Snap g_prevProbe{};
double g_tscHz = 1;
int64_t g_csvT0 = 0;  // first frame of the current recording (CSV time origin)

// [Suite] MotionCounters: 0 = record frame times only, without the texture,
// allocator, pass-timing and probe counters (for clean CPU profiles in motion).
bool g_counters = true;

void Start(double tscHz) {
  std::lock_guard<std::mutex> lock(g_mutex);
  for (auto& b : g_b) b = Bucket{};
  g_havePrev = false;
  g_csvT0 = 0;
  g_tscHz = tscHz;
  if (g_counters) {
    mprobe::Install();
    g_hookMeasure = true;  // texture/allocator counters on (same overhead in every bucket)
    texbind::SetAttached(true);
    ptiming::g_recording = true;
  }
  SYSTEMTIME st;
  GetLocalTime(&st);
  wchar_t name[64];
  swprintf_s(name, L"motion_%04d%02d%02d_%02d%02d%02d.csv", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute,
             st.wSecond);
  if (g_csv) fclose(g_csv);  // a recording that was never stopped
  g_csv = nullptr;
  _wfopen_s(&g_csv, (g_dir + name).c_str(), L"w");
  if (g_csv)
    fprintf(g_csv, "t_s,deg_per_s,frame_ms,culling_ms,passes_ms,rt_cpu_ms,periph,focus,shadow,tex_req,tex_orig,"
                   "alloc_kb,drain_ms,drain_calls,lin_m_per_s,terrain_spatial_ms,terrain_objects_ms\n");
  g_recording = true;
}

double PassTotalUs() {
  std::lock_guard<std::mutex> lock(ptiming::g_mutex);
  return ptiming::g_topLevelUs;
}

// Called by the collect hook after every quad frame (render thread).
void OnFrame(const std::vector<ViewInfo>& views, int64_t qpcNow, uint64_t collectUs, uint64_t periph,
             uint64_t focus, uint64_t shadow) {
  if (!g_recording.load(std::memory_order_relaxed)) return;
  const ViewInfo* p = nullptr;
  for (const ViewInfo& v : views)
    if (v.role == 1 && v.fr.valid) {
      p = &v;
      break;
    }
  if (!p) return;
  // The collect hook runs on a pool thread: query the render thread itself.
  static HANDLE rt = nullptr;
  static DWORD rtId = 0;
  DWORD want = ptiming::g_topTid.load(std::memory_order_relaxed);
  if (want && want != rtId) {
    if (rt) CloseHandle(rt);
    rt = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, want);
    rtId = want;
    g_havePrev = false;
  }
  ULONG64 cyc = 0;
  if (rt) QueryThreadCycleTime(rt, &cyc);
  double passUs = PassTotalUs();
  texbind::Totals tex = texbind::Snapshot();
  allocslab::Totals al = allocslab::Snapshot();
  mprobe::Snap pr = mprobe::Take();
  std::lock_guard<std::mutex> lock(g_mutex);
  if (g_havePrev && qpcNow > g_prevQpc) {
    double dt = (qpcNow - g_prevQpc) * g_qpcToUs / 1e6;
    double ang = std::acos(Clamp(Dot(p->fr.centerDir, g_prevFwd), -1.0, 1.0)) * 57.29577951308232 / dt;
    double lin = Len(p->fr.apex - g_prevPos) / dt;
    int k = 0;
    while (k < kBuckets - 1 && ang >= kBucketLimits[k]) ++k;
    Bucket& b = g_b[k];
    b.frames++;
    b.frameMs.push_back(static_cast<float>(dt * 1000.0));
    b.collectMs += collectUs / 1000.0;
    b.passMs += (passUs - g_prevPassUs) / 1000.0;
    b.rtCpuMs += (cyc - g_prevRtCycles) / g_tscHz * 1000.0;
    b.periph += static_cast<double>(periph);
    b.focus += static_cast<double>(focus);
    b.shadow += static_cast<double>(shadow);
    b.streamCalls += static_cast<double>(tex.calls - g_prevTex.calls);
    b.streamOrig += static_cast<double>((tex.calls - g_prevTex.calls) - (tex.skipped - g_prevTex.skipped));
    b.allocKB += (al.bytes - g_prevAlloc.bytes) / 1024.0;
    b.linSpeed += lin;
    if (g_csv) {
      if (!g_csvT0) g_csvT0 = qpcNow;
      fprintf(g_csv, "%.3f,%.1f,%.3f,%.3f,%.3f,%.3f,%llu,%llu,%llu,%llu,%llu,%.0f,%.3f,%llu,%.1f,%.3f,%.3f\n",
              (qpcNow - g_csvT0) * g_qpcToUs / 1e6, ang, dt * 1000.0, collectUs / 1000.0,
              (passUs - g_prevPassUs) / 1000.0, (cyc - g_prevRtCycles) / g_tscHz * 1000.0,
              static_cast<unsigned long long>(periph), static_cast<unsigned long long>(focus),
              static_cast<unsigned long long>(shadow), static_cast<unsigned long long>(tex.calls - g_prevTex.calls),
              static_cast<unsigned long long>((tex.calls - g_prevTex.calls) - (tex.skipped - g_prevTex.skipped)),
              (al.bytes - g_prevAlloc.bytes) / 1024.0,
              (pr.cycles[0] - g_prevProbe.cycles[0]) / g_tscHz * 1000.0,
              static_cast<unsigned long long>(pr.calls[0] - g_prevProbe.calls[0]), lin,
              (pr.cycles[2] - g_prevProbe.cycles[2]) / g_tscHz * 1000.0,
              (pr.cycles[3] - g_prevProbe.cycles[3]) / g_tscHz * 1000.0);
    }
    for (int i = 0; i < mprobe::kProbes; ++i) {
      b.probeCalls[i] += static_cast<double>(pr.calls[i] - g_prevProbe.calls[i]);
      b.probeMs[i] += (pr.cycles[i] - g_prevProbe.cycles[i]) / g_tscHz * 1000.0;
    }
  }
  g_havePrev = true;
  g_prevFwd = p->fr.centerDir;
  g_prevPos = p->fr.apex;
  g_prevQpc = qpcNow;
  g_prevRtCycles = cyc;
  g_prevPassUs = passUs;
  g_prevTex = tex;
  g_prevAlloc = al;
  g_prevProbe = pr;
}

void StopAndReport() {
  g_recording = false;
  {
    // Under the lock OnFrame writes with: it checks g_recording before taking
    // it, so a frame in flight could otherwise fprintf to the closed FILE.
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_csv) {
      fclose(g_csv);
      g_csv = nullptr;
    }
  }
  ptiming::g_recording = false;
  g_hookMeasure = false;
  ApplyTextureHook();
  std::lock_guard<std::mutex> lock(g_mutex);
  Log("-- motion profile (per quad frame, by head angular speed) --");
  Log("  %-24s %6s %8s %8s %8s %8s %8s %8s %8s %8s %9s %9s %8s", "bucket", "frames", "frame", "p95", "culling",
      "passes", "rt cpu", "periph", "focus", "shadow", "tex req", "tex orig", "allocKB");
  for (int k = 0; k < kBuckets; ++k) {
    Bucket& b = g_b[k];
    if (!b.frames) continue;
    double f = static_cast<double>(b.frames);
    std::vector<float> v = b.frameMs;
    double sum = 0;
    for (float x : v) sum += x;
    size_t i95 = std::min(v.size() - 1, static_cast<size_t>(v.size() * 0.95));
    std::nth_element(v.begin(), v.begin() + i95, v.end());
    Log("  %-24s %6llu %7.2fms %7.2fms %7.2fms %7.2fms %7.2fms %8.0f %8.0f %8.0f %9.0f %9.0f %8.0f",
        kBucketNames[k], static_cast<unsigned long long>(b.frames), sum / v.size(), v[i95], b.collectMs / f,
        b.passMs / f, b.rtCpuMs / f, b.periph / f, b.focus / f, b.shadow / f, b.streamCalls / f, b.streamOrig / f,
        b.allocKB / f);
  }
  for (int k = 0; k < kBuckets; ++k) {
    Bucket& b = g_b[k];
    if (!b.frames) continue;
    double f = static_cast<double>(b.frames);
    std::string line;
    char buf[160];
    for (int i = 0; i < mprobe::kProbes; ++i) {
      snprintf(buf, sizeof(buf), "  %s %.3f ms (%.1f calls)", mprobe::g_probes[i].name, b.probeMs[i] / f,
               b.probeCalls[i] / f);
      line += buf;
    }
    Log("  %-24s per frame:%s", kBucketNames[k], line.c_str());
  }
  Log("  (frame = interval between quad frames; passes = render-graph pass CPU on the render thread; "
      "rt cpu = render-thread CPU per frame; tex orig = streaming requests that reached dx11backend)");
}

}  // namespace motion
