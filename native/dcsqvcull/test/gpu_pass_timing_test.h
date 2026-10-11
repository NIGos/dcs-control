// Offline test for gpu_pass_timing.h: the analysis on synthetic ticks (nesting,
// exclusive time, gaps, xrEndFrame, period, call order), then a real D3D11
// device (hardware, WARP fallback) driven through the pass-boundary callback:
// frames are ring-buffered and read back without blocking, the context ops
// are counted per innermost pass and forwarded (the clear really happens),
// and stopping restores every context-table slot.
#pragma once

namespace gpttest {

void* const kA = reinterpret_cast<void*>(0xA000);
void* const kB = reinterpret_cast<void*>(0xB000);

std::string NameOf(void* p) { return p == kA ? "A" : p == kB ? "B" : "?"; }

// Flushes ctx and waits (event query, up to 30 s) until the GPU finished
// everything submitted so far. The frames' queries are then ready by the
// next boundary on a loaded GPU too: no frame is dropped from the ring or
// read without its statistics because the GPU was busy with other work
// (fixed sleeps were enough only on an idle GPU).
inline void WaitGpuIdle(ID3D11Device* dev, ID3D11DeviceContext* ctx) {
  D3D11_QUERY_DESC qd = {D3D11_QUERY_EVENT, 0};
  ID3D11Query* q = nullptr;
  if (FAILED(dev->CreateQuery(&qd, &q)) || !q) {
    Sleep(50);
    return;
  }
  ctx->End(q);
  const ULONGLONG end = GetTickCount64() + 30000;
  BOOL done = FALSE;
  while (ctx->GetData(q, &done, sizeof(done), 0) != S_OK && GetTickCount64() < end) Sleep(1);
  q->Release();
}

void Synthetic() {
  // Two consecutive frames, 1 MHz (1 tick = 1 us): A{ B } A, then xrEndFrame.
  std::vector<gpt::Done> done(2);
  for (int f = 0; f < 2; ++f) {
    gpt::Done& d = done[f];
    d.serial = 7 + f;
    d.freq = 1000000;
    const uint64_t o = 2000 * f;
    d.ev = {{kA, gpt::kBegin, 0}, {kB, gpt::kBegin, 1}, {kB, gpt::kEnd, 1}, {kA, gpt::kEnd, 0},
            {kA, gpt::kBegin, 0}, {kA, gpt::kEnd, 0},   {nullptr, gpt::kXrBegin, 0}, {nullptr, gpt::kXrEnd, 0}};
    d.ticks = {o + 100, o + 200, o + 500, o + 600, o + 700, o + 900, o + 950, o + 1000};
  }
  std::swap(done[0], done[1]);  // Analyze orders by serial
  gpt::Result r = gpt::Analyze(done, &NameOf);
  auto approx = [](double a, double b) { return std::fabs(a - b) < 1e-9; };
  Check(r.frames == 2 && r.unmatched == 0, "gpu pass timing: two frames analysed, every pass event matched");
  const gpt::KindStat& a = r.kinds["A"];
  const gpt::KindStat& b = r.kinds["B"];
  Check(approx(gpt::Mean(a.perFrame), 0.4) && approx(gpt::Mean(b.perFrame), 0.3),
        "gpu pass timing: exclusive GPU ms per frame (nested pass subtracted)");
  Check(approx(a.inclMs, 1.4) && a.calls == 4 && b.calls == 2, "gpu pass timing: inclusive ms and calls");
  Check(a.occMs.size() == 2 && approx(a.occMs[0], 0.4) && approx(a.occMs[1], 0.4) && a.occN[1] == 2,
        "gpu pass timing: exclusive ms by call order within the frame");
  Check(approx(gpt::Mean(r.span), 0.8) && approx(gpt::Mean(r.gaps), 0.1), "gpu pass timing: span and gaps");
  Check(approx(gpt::Mean(r.xr), 0.05) && approx(gpt::Mean(r.toXr), 0.05), "gpu pass timing: xrEndFrame and lead-in");
  Check(r.period.size() == 1 && approx(r.period[0], 2.0) && approx(r.afterXr[0], 1.1),
        "gpu pass timing: frame period and xrEndFrame -> next frame");
  // A frame that does not follow its predecessor gives no period; a stray end is unmatched.
  done[1].serial = 20;
  done[1].ev.push_back({kB, gpt::kEnd, 0});
  done[1].ticks.push_back(1990 + 2000);
  r = gpt::Analyze(done, &NameOf);
  Check(r.period.empty() && r.unmatched == 1, "gpu pass timing: gap in serials, unmatched end");
}

void Device() {
  ID3D11Device* dev = nullptr;
  ID3D11DeviceContext* ctx = nullptr;
  D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
  if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &fl, 1, D3D11_SDK_VERSION, &dev,
                               nullptr, &ctx)) &&
      FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, &fl, 1, D3D11_SDK_VERSION, &dev, nullptr,
                               &ctx))) {
    Check(false, "gpu pass timing: create a D3D11 device");
    return;
  }
  D3D11_TEXTURE2D_DESC td{};
  td.Width = td.Height = 64;
  td.MipLevels = td.ArraySize = 1;
  td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  td.SampleDesc.Count = 1;
  td.BindFlags = D3D11_BIND_RENDER_TARGET;
  ID3D11Texture2D *rt = nullptr, *copy = nullptr, *staging = nullptr;
  ID3D11RenderTargetView* rtv = nullptr;
  dev->CreateTexture2D(&td, nullptr, &rt);
  dev->CreateTexture2D(&td, nullptr, &copy);
  td.BindFlags = 0;
  td.Usage = D3D11_USAGE_STAGING;
  td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  dev->CreateTexture2D(&td, nullptr, &staging);
  if (rt) dev->CreateRenderTargetView(rt, nullptr, &rtv);
  if (!rt || !copy || !staging || !rtv) {
    Check(false, "gpu pass timing: test resources");
    return;
  }
  void** vt = *reinterpret_cast<void***>(ctx);
  void* before[gpt::kOpCount];
  for (int i = 0; i < gpt::kOpCount; ++i) before[i] = vt[DcsQv_ContextSlot(gpt::kSlotNames[i])];

  std::atomic<uint64_t> frames{0};
  Check(gpt::Prepare(dev, ctx, &frames, false), "gpu pass timing: prepare (slots, queries)");
  gpt::g_xrTid = 0;
  gpt::g_want = 1;
  const float red[4] = {1, 0, 0, 1}, green[4] = {0, 1, 0, 1};
  constexpr int kFrames = 40;
  for (int f = 0; f < kFrames; ++f) {
    ++frames;
    gpt::OnBoundary(kA, true, 0);  // activates on the first, opens a frame on each
    if (f == 0)
      Check(gpt::g_active.load() && vt[DcsQv_ContextSlot("ClearRenderTargetView")] == gpt::kHooks[gpt::kClearRTV],
            "gpu pass timing: active, context table hooked");
    ctx->ClearRenderTargetView(rtv, red);
    ctx->ClearRenderTargetView(rtv, red);
    gpt::OnBoundary(kB, true, 1);
    ctx->CopyResource(copy, rt);
    gpt::OnBoundary(kB, false, 1);
    gpt::OnBoundary(kA, false, 0);
    ctx->ClearRenderTargetView(rtv, f == kFrames - 1 ? green : red);  // outside passes
    WaitGpuIdle(dev, ctx);
  }
  // A d3d11-style table rewrite of a hooked slot is taken back at the next boundary.
  const int cs = DcsQv_ContextSlot("CopyResource");
  const uint64_t rehooks = gpt::g_s.rehooks;
  d3ds::WriteTablePointer(&vt[cs], before[gpt::kCopyResource]);
  gpt::OnBoundary(kB, true, 1);
  Check(vt[cs] == gpt::kHooks[gpt::kCopyResource] && gpt::g_s.rehooks > rehooks,
        "gpu pass timing: re-hook after a rewrite");
  gpt::OnBoundary(kB, false, 1);
  WaitGpuIdle(dev, ctx);
  gpt::g_want = 0;
  gpt::OnBoundary(kA, true, 0);  // deactivates
  // d3d11's entry: the one from before, or the variant it switched to meanwhile.
  bool restored = true;
  for (int i = 0; i < gpt::kOpCount; ++i) {
    void* now = vt[DcsQv_ContextSlot(gpt::kSlotNames[i])];
    restored &= now != gpt::kHooks[i] && (now == before[i] || now == gpt::g_orig[i]);
  }
  Check(!gpt::g_active.load() && restored && !gpt::g_queriesReady.load(),
        "gpu pass timing: stopped, every slot restored, queries released");
  const gpt::Counts& ca = gpt::g_passCounts[kA];
  const gpt::Counts& cb = gpt::g_passCounts[kB];
  printf("     counts: A clear %llu copy %llu, B copy %llu, outside clear %llu, rehooks %llu\n",
         static_cast<unsigned long long>(ca.n[gpt::kClearRTV]), static_cast<unsigned long long>(ca.n[gpt::kCopyResource]),
         static_cast<unsigned long long>(cb.n[gpt::kCopyResource]),
         static_cast<unsigned long long>(gpt::g_outside[gpt::kClearRTV].load()),
         static_cast<unsigned long long>(gpt::g_s.rehooks));
  Check(ca.n[gpt::kClearRTV] == 2 * kFrames && ca.n[gpt::kCopyResource] == 0 && cb.n[gpt::kCopyResource] == kFrames &&
            gpt::g_outside[gpt::kClearRTV].load() == kFrames,
        "gpu pass timing: ops counted for the innermost pass, outside passes separately");
  std::vector<gpt::Done> done;
  {
    std::lock_guard<std::mutex> lock(gpt::g_doneMutex);
    done.swap(gpt::g_done);
  }
  const uint64_t read = done.size();
  gpt::Result r = gpt::Analyze(done, &NameOf);
  printf("     %llu of %llu frames read (disjoint %llu, dropped %llu), A %.4f ms, B %.4f ms, period %.3f ms\n",
         static_cast<unsigned long long>(read), static_cast<unsigned long long>(gpt::g_s.framesOpened),
         static_cast<unsigned long long>(gpt::g_s.disjoint), static_cast<unsigned long long>(gpt::g_s.dropped),
         gpt::Mean(r.kinds["A"].perFrame), gpt::Mean(r.kinds["B"].perFrame), gpt::Mean(r.period));
  Check(gpt::g_s.framesOpened == kFrames && read + gpt::g_s.disjoint + gpt::g_s.dropped >= kFrames - 2 &&
            read >= kFrames / 2,
        "gpu pass timing: frames ring-buffered and read back without blocking");
  Check(r.unmatched == 0 && r.kinds["A"].calls == read && r.kinds["B"].calls == read &&
            r.kinds["A"].inclMs >= r.kinds["B"].inclMs,
        "gpu pass timing: read frames hold the A{B} nesting");
  // The forwarded clears reached d3d11: the last one (green) is in the texture.
  ctx->CopyResource(staging, rt);
  D3D11_MAPPED_SUBRESOURCE m{};
  bool green_ok = false;
  if (SUCCEEDED(ctx->Map(staging, 0, D3D11_MAP_READ, 0, &m))) {
    green_ok = *static_cast<const uint32_t*>(m.pData) == 0xff00ff00u;
    ctx->Unmap(staging, 0);
  }
  Check(green_ok, "gpu pass timing: hooked calls forwarded to d3d11");
  gpt::g_ctx = nullptr;
  gpt::g_dev = nullptr;
  rtv->Release();
  staging->Release();
  copy->Release();
  rt->Release();
  ctx->Release();
  dev->Release();
}

// Pipeline statistics: aggregation per kind and call (synthetic), then the
// query pairs on a real device (outermost passes only, read with the frame).
void Stats() {
  std::vector<gpt::Done> done(2);
  for (int f = 0; f < 2; ++f) {
    gpt::Done& d = done[f];
    d.serial = 3 + f;
    d.freq = 1000000;
    d.ev = {{kA, gpt::kBegin, 0}, {kA, gpt::kEnd, 0}, {kA, gpt::kBegin, 0}, {kA, gpt::kEnd, 0}};
    d.ticks = {10, 20, 30, 40};
    if (f == 0) d.stats = {{kA, 1, 2, 3, 4, 0, 5}, {kA, 10, 20, 30, 40, 0, 50}};  // frame 1: statistics late
  }
  gpt::Result r = gpt::Analyze(done, &NameOf);
  const gpt::KindPipe& kp = r.pipe["A"];
  Check(r.statFrames == 1 && kp.frames == 1 && kp.sum.ps == 44 && kp.sum.samples == 55 && kp.occ.size() == 2 &&
            kp.occ[1].cPrims == 30 && kp.occN[0] == 1,
        "gpu pass timing: pipeline statistics per kind and per call");

  ID3D11Device* dev = nullptr;
  ID3D11DeviceContext* ctx = nullptr;
  D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
  if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &fl, 1, D3D11_SDK_VERSION, &dev,
                               nullptr, &ctx)) &&
      FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, &fl, 1, D3D11_SDK_VERSION, &dev, nullptr,
                               &ctx))) {
    Check(false, "gpu pass timing: create a D3D11 device (statistics)");
    return;
  }
  std::atomic<uint64_t> frames{0};
  gpt::g_statsOn = true;
  gpt::g_light = true;  // timestamps only: no context-op hooks (statistics are off in light mode)
  Check(gpt::Prepare(dev, ctx, &frames, false), "gpu pass timing: prepare with statistics");
  gpt::g_light = false;
  Check(gpt::g_statsOn.load() && gpt::g_ring[0].pq[0] && gpt::g_ring[0].oq[0],
        "gpu pass timing: statistics queries created");
  gpt::g_xrTid = 0;
  gpt::g_want = 1;
  for (int f = 0; f < 20; ++f) {
    ++frames;
    gpt::OnBoundary(kA, true, 0);
    gpt::OnBoundary(kB, true, 1);
    gpt::OnBoundary(kB, false, 1);
    gpt::OnBoundary(kA, false, 0);
    WaitGpuIdle(dev, ctx);
  }
  gpt::g_want = 0;
  gpt::OnBoundary(kA, true, 0);  // deactivates (last reads, hooks out, queries released)
  std::vector<gpt::Done> got;
  {
    std::lock_guard<std::mutex> lock(gpt::g_doneMutex);
    got.swap(gpt::g_done);
  }
  r = gpt::Analyze(got, &NameOf);
  printf("     statistics: %d of %zu read frames (%llu late)\n", r.statFrames, got.size(),
         static_cast<unsigned long long>(gpt::g_s.statsLate));
  Check(r.statFrames > 0 && r.pipe.count("A") && !r.pipe.count("B") && r.pipe["A"].occ.size() == 1,
        "gpu pass timing: statistics for the outermost pass only, read with the frames");
  Check(!gpt::g_queriesReady.load() && !gpt::g_ring[0].pq[0], "gpu pass timing: statistics queries released");
  gpt::g_statsOn = false;
  gpt::g_ctx = nullptr;
  gpt::g_dev = nullptr;
  ctx->Release();
  dev->Release();
}

void Run() {
  Synthetic();
  Device();
  Stats();
}

}  // namespace gpttest
