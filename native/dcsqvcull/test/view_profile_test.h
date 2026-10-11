// Offline test for the synthetic pose (pose_sweep.h: the sweep rotates with a
// hold and with MotionTaxi, moves only with MotionTaxi) and the view profile
// (view_profile.h: bound classification, spikes and elevated quantities,
// window aggregation, texture-upload bytes, GPU per-frame busy time), then
// gpu_pass_timing's light mode and the CreateTexture2D counter on a real
// D3D11 device.
#pragma once

namespace vptest {

bool Near(double a, double b, double eps = 1e-6) { return std::fabs(a - b) < eps; }

void Pose() {
  using namespace posesweep;
  // The 2026-10-09 failure: MotionTaxi=1 (and any hold) froze the yaw.
  Check(Near(ComposeYaw(false, 0, true, 0, 10.0), SweepYaw(10.0)) && std::fabs(SweepYaw(10.0)) > 1.0,
        "pose sweep: the sine sweep rotates (t = 10 s)");
  Check(Near(ComposeYaw(true, 90, true, 0, 10.0), 90 + SweepYaw(10.0)),
        "pose sweep: a hold is the base the sweep rotates about (no longer freezes it)");
  Check(Near(ComposeYaw(true, 90, false, 0, 10.0), 90) && Near(ComposeYaw(false, 0, false, 60, 10.0), 0),
        "pose sweep: no sweep = the held yaw (or 0)");
  Check(Near(ComposeYaw(false, 0, true, 60, 2.5), 150) && Near(ComposeYaw(true, 30, true, 60, 7.0), 30 + 60),
        "pose sweep: constant-rate turn, wrapped to one revolution");
  Check(Near(ComposeForward(true, false, 80), 0) && Near(ComposeForward(false, true, 80), 0) &&
            Near(ComposeForward(true, true, 10), 75),
        "pose sweep: the pose slides forward only with MotionTaxi=1 during a sweep");
  // Taxi no longer zeroes the yaw: the yaw of a sweep with taxi is the sweep's.
  g_taxi = true;
  g_sweep = true;
  g_rateDps = 0;
  g_hold = -2000.0;
  const double y = YawAt(10.0), f = ForwardAt(10.0);
  g_taxi = false;
  g_sweep = false;
  Check(Near(y, SweepYaw(10.0)) && Near(f, 75), "pose sweep: MotionTaxi=1 sweeps and slides");
  Check(Near(ForwardAt(80.0), 0), "pose sweep: no forward slide without a running taxi sweep");
}

void Classify() {
  using namespace vprof;
  BoundIn in;
  in.frameMs = 20;
  Check(Classify(in) == kBCpu, "view profile: nothing waits -> CPU-bound (render thread)");
  in.xrEndMs = 2.5;
  in.gpuValid = true;
  in.gpuIdleMs = 1.0;
  Check(Classify(in) == kBGpu, "view profile: xrEndFrame wait 12% and GPU idle 5% -> GPU-bound");
  in.gpuIdleMs = 5.0;
  Check(Classify(in) == kBCpu, "view profile: xrEndFrame 12% but GPU idle 25% -> not GPU-bound");
  in.xrEndMs = 6.0;
  Check(Classify(in) == kBGpu, "view profile: xrEndFrame >= 25% alone -> GPU-bound");
  in.xrEndMs = 0.3;
  in.pacerMs = 3.0;
  Check(Classify(in) == kBPacing, "view profile: pacer wait 15% -> pacing-bound");
  in.pacerMs = 0;
  in.recLate = 0.5;
  Check(Classify(in) == kBWorker, "view profile: late recorder jobs 0.5/frame -> worker-bound");
  in.recLate = 0;
  in.recWaitMs = 1.2;
  Check(Classify(in) == kBWorker, "view profile: recorder wait 6% -> worker-bound");
}

void Spikes() {
  using namespace vprof;
  std::vector<float> ft = {10, 10, 11, 10, 25, 10, 9, 30, 10, 10};
  double med = 0;
  std::vector<size_t> sp = vprof::Spikes(ft, 1.5, &med);
  Check(Near(med, 10) && sp.size() == 2 && sp[0] == 4 && sp[1] == 7, "view profile: spikes > 1.5 x median");
  std::vector<Series> s = {{"cpu", true, {8, 8, 8, 8, 18, 8, 8, 20, 8, 8}},
                           {"flat", true, {2, 2, 2, 2, 2.1f, 2, 2, 2.1f, 2, 2}},
                           {"tex", false, {0, 0, 1, 0, 40, 0, 0, 30, 0, 0}},
                           {"gpu", true, {-1, -1, -1, -1, -1, -1, -1, -1, -1, -1}}};
  std::vector<Elev> el = Elevated(s, sp);
  Check(el.size() == 2 && strcmp(el[0].name, "cpu") == 0 && Near(el[0].spikeMean, 19) &&
            strcmp(el[1].name, "tex") == 0 && Near(el[1].spikeMean, 35),
        "view profile: elevated quantities in the spikes (times first), flat and unmeasured ones left out");
}

void Aggregate() {
  using namespace vprof;
  std::vector<Rec> v(4);
  for (int i = 0; i < 4; ++i) {
    v[i].frameMs = 10.0f + i;
    v[i].rtCpuMs = 8;
    v[i].pacerMs = i < 2 ? -1.0f : 0.5f;
    v[i].shCasters = 100;
    v[i].shRecorded = 90;
    v[i].gbItems = 200;
    v[i].gbDraws = 100;
    v[i].shPasses = 4;
    v[i].shExec = 3;
    v[i].shLate = i == 0 ? 1.0f : 0.0f;
  }
  Agg a = vprof::Aggregate(v, 0.046);
  Check(a.frames == 4 && Near(a.frameMs, 11.5) && Near(a.fps, 4 / 0.046) && Near(a.rtCpuMs, 8) &&
            Near(a.pacerMs, 0.5) && Near(a.shCov, 90) && Near(a.gbCov, 50) && Near(a.shExecPct, 75) &&
            Near(a.recLate, 0.25) && Near(a.gbExecPct, -1),
        "view profile: window aggregation (means, coverage, unmeasured pacer frames skipped)");
  Check(BoundOf(a) == kBWorker, "view profile: aggregate classified (late 0.25/frame)");
}

void Bytes() {
  D3D11_TEXTURE2D_DESC d{};
  d.Width = d.Height = 256;
  d.MipLevels = 2;
  d.ArraySize = 1;
  d.Format = DXGI_FORMAT_BC1_UNORM;
  D3D11_SUBRESOURCE_DATA init[2] = {{nullptr, 512, 0}, {nullptr, 256, 0}};
  Check(vprof::InitBytes(d, init) == 512ull * 64 + 256ull * 32 && vprof::InitBytes(d, nullptr) == 0,
        "view profile: BC texture upload bytes (4-pixel rows)");
  d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  d.MipLevels = 1;
  D3D11_SUBRESOURCE_DATA one = {nullptr, 1024, 0};
  Check(vprof::InitBytes(d, &one) == 1024ull * 256, "view profile: uncompressed texture upload bytes");
}

void FrameGpu() {
  gpt::Done d;
  d.freq = 1000000;
  void* A = reinterpret_cast<void*>(0xA0);
  void* B = reinterpret_cast<void*>(0xB0);
  d.ev = {{A, gpt::kBegin, 0}, {B, gpt::kBegin, 1}, {B, gpt::kEnd, 1}, {A, gpt::kEnd, 0},
          {B, gpt::kBegin, 0}, {B, gpt::kEnd, 0},   {nullptr, gpt::kXrBegin, 0}, {nullptr, gpt::kXrEnd, 0}};
  d.ticks = {100, 200, 300, 600, 1000, 1400, 1500, 1800};
  double span = 0, busy = 0, xr = 0;
  Check(gpt::FrameGpu(d, &span, &busy, &xr) && Near(span, 1.3) && Near(busy, 0.9) && Near(xr, 0.3),
        "view profile: GPU span, busy (top-level passes) and xrEndFrame per frame");
  d.ev.resize(1);
  d.ticks.resize(1);
  Check(!gpt::FrameGpu(d, &span, &busy, &xr), "view profile: a frame without a complete pass is skipped");
}

void Device() {
  ID3D11Device* dev = nullptr;
  ID3D11DeviceContext* ctx = nullptr;
  D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
  if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &fl, 1, D3D11_SDK_VERSION, &dev,
                               nullptr, &ctx)) &&
      FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, &fl, 1, D3D11_SDK_VERSION, &dev, nullptr,
                               &ctx))) {
    Check(false, "view profile: create a D3D11 device");
    return;
  }
  // CreateTexture2D counter: counts creations with initial data, restores the slot.
  void** dvt = *reinterpret_cast<void***>(dev);
  void* const ct2Before = dvt[5];
  const bool hooked = vprof::InstallTexCreate(dev);
  const uint64_t n0 = vprof::g_texNew.load(), b0 = vprof::g_texNewBytes.load();
  std::vector<uint32_t> pixels(64 * 64, 0xff336699u);
  D3D11_TEXTURE2D_DESC td{};
  td.Width = td.Height = 64;
  td.MipLevels = td.ArraySize = 1;
  td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  td.SampleDesc.Count = 1;
  td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  D3D11_SUBRESOURCE_DATA init{pixels.data(), 64 * 4, 0};
  ID3D11Texture2D *t1 = nullptr, *t2 = nullptr;
  dev->CreateTexture2D(&td, &init, &t1);
  dev->CreateTexture2D(&td, nullptr, &t2);  // no data: not an upload
  Check(hooked && t1 && t2 && vprof::g_texNew.load() - n0 == 1 && vprof::g_texNewBytes.load() - b0 == 64ull * 64 * 4,
        "view profile: texture creations with data counted (and created)");
  vprof::UninstallTexCreate();
  Check(dvt[5] == ct2Before, "view profile: CreateTexture2D slot restored");
  if (t1) t1->Release();
  if (t2) t2->Release();

  // gpu_pass_timing light mode: timestamps only, the context table untouched.
  void** vt = *reinterpret_cast<void***>(ctx);
  std::atomic<uint64_t> frames{0};
  Check(gpt::Prepare(dev, ctx, &frames, false), "view profile: gpu light mode prepare");
  gpt::g_light = true;
  gpt::g_xrTid = 0;
  gpt::g_want = 1;
  void* A = reinterpret_cast<void*>(0xA0);
  void* B = reinterpret_cast<void*>(0xB0);
  bool untouched = true;
  for (int f = 0; f < 30; ++f) {
    ++frames;
    gpt::OnBoundary(A, true, 0);
    gpt::OnBoundary(B, true, 1);
    gpt::OnBoundary(B, false, 1);
    gpt::OnBoundary(A, false, 0);
    // d3d11 itself switches some entries after a Flush: only check that none is ours.
    for (int i = 0; i < gpt::kOpCount; ++i) untouched &= vt[DcsQv_ContextSlot(gpt::kSlotNames[i])] != gpt::kHooks[i];
    gpttest::WaitGpuIdle(dev, ctx);  // queries ready by the next boundary, loaded GPU or not
  }
  gpt::g_want = 0;
  gpt::OnBoundary(A, true, 0);  // deactivates
  gpt::g_light = false;
  std::vector<gpt::Done> done = gpt::TakeDone();
  int ok = 0;
  for (const gpt::Done& d : done) {
    double span = 0, busy = 0;
    if (d.qpc > 0 && gpt::FrameGpu(d, &span, &busy, nullptr) && busy <= span + 1e-9) ++ok;
  }
  Check(untouched && !gpt::g_opsHooked.load() && gpt::g_passCounts.empty() && !gpt::g_active.load(),
        "view profile: gpu light mode leaves the context table alone and counts no ops");
  Check(ok >= 10 && ok == static_cast<int>(done.size()), "view profile: gpu light mode frames read with open times");
  gpt::g_ctx = nullptr;
  gpt::g_dev = nullptr;
  ctx->Release();
  dev->Release();
}

void Run() {
  Pose();
  Classify();
  Spikes();
  Aggregate();
  Bytes();
  FrameGpu();
  Device();
}

}  // namespace vptest
