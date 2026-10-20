// R99 (docs/research/R99_pose_displaytime_cost.md):
//  1. the test pose hook (pose_sweep.h) as a function of each locate's displayTime: one pose per display time for every
//     caller and every view, the mid display time's yaw in the middle, a monotonic turn; [Suite] PoseWallClock=1 keeps
//     the old clock;
//  2. the symbology export's replay stage: RunSym (the lean dispatch) puts back every binding it touches exactly (WARP);
//     the per-record path and the per-transition calls timed (QV_R95_HW=1: the hardware driver; the microbenchmark).
#pragma once

namespace r99test {

using namespace fixdepth;
using fdtest::Rel;
namespace sy = fixdepth::cmask::sym;

// ---- 1. pose hook ----
// A QV locate: 4 views (left / right periphery, left / right focus), each with its own orientation; the head can be
// moved between calls (g_head: a yaw of the runtime's head, degrees) to check the HoldPose anchor.
double g_head = 0;
int g_locates = 0;
int32_t __fastcall FakeLocate4(void*, const void*, void*, uint32_t cap, uint32_t* count, uint8_t* views) {
  ++g_locates;
  *count = 4;
  const double h = g_head * 3.14159265358979 / 180;
  for (uint32_t i = 0; i < cap && i < 4; ++i) {
    float* q = reinterpret_cast<float*>(views + i * 64 + 16);
    // the view's own orientation: a small per-view pitch (the focus views look elsewhere), then the head's yaw
    const double a = (0.02 + 0.03 * i) / 2;
    const double qv[4] = {std::sin(a), 0, 0, std::cos(a)}, qy[4] = {0, std::sin(h / 2), 0, std::cos(h / 2)};
    double o[4];
    posesweep::QMul(qy, qv, o);
    for (int k = 0; k < 4; ++k) q[k] = static_cast<float>(o[k]);
    const double off[3] = {(i & 1) ? 0.032 : -0.032, 0, 0};  // the eye offset, turned with the head
    double ow[3];
    posesweep::QRot(qy, off, ow);
    q[4] = static_cast<float>(ow[0]), q[5] = static_cast<float>(1.2 + ow[1]), q[6] = static_cast<float>(0.05 + ow[2]);
  }
  return 0;
}
struct Located {
  uint8_t v[4 * 64];
};
Located Locate(int64_t displayTime, bool withInfo = true) {
  alignas(16) uint8_t info[40] = {};
  memcpy(info + posesweep::kInfoDisplayTime, &displayTime, 8);
  Located l{};
  uint32_t n = 0;
  posesweep::Hook(nullptr, withInfo ? info : nullptr, nullptr, 4, &n, l.v);
  return l;
}
// The yaw (degrees) the hook applied to view i: q_out * conj(q_in), against the runtime's pose of that view.
double YawOf(const Located& l, int i) {
  alignas(16) uint8_t raw[4 * 64] = {};
  uint32_t n = 0;
  FakeLocate4(nullptr, nullptr, nullptr, 4, &n, raw);
  const float* qo = reinterpret_cast<const float*>(l.v + i * 64 + 16);
  const float* qi = reinterpret_cast<const float*>(raw + i * 64 + 16);
  const double a[4] = {qo[0], qo[1], qo[2], qo[3]}, bc[4] = {-qi[0], -qi[1], -qi[2], qi[3]};
  double r[4];
  posesweep::QMul(a, bc, r);
  return 2 * std::atan2(r[1], r[3]) * 180 / 3.14159265358979;
}
double Wrap(double d) { return std::remainder(d, 360.0); }

void TestPoseDisplayTime() {
  namespace ps = posesweep;
  const auto keepTramp = ps::g_tramp;
  const bool keepOn = ps::g_on, keepSweep = ps::g_sweep, keepHoldPose = ps::g_holdPose, keepTaxi = ps::g_taxi;
  const double keepRate = ps::g_rateDps, keepHold = ps::g_hold, keepPitch = ps::g_pitchDeg;
  LARGE_INTEGER f;
  QueryPerformanceFrequency(&f);
  g_qpcToUs = 1e6 / static_cast<double>(f.QuadPart);
  ps::g_tramp = &FakeLocate4;
  ps::g_hold = -2000.0, ps::g_pitchDeg = 0, ps::g_holdPose = false, ps::g_taxi = false, ps::g_wallClock = false;
  ps::g_rateDps = 30, ps::g_sweep = true, ps::g_on = true;  // what Start(30) sets (without the install)
  ps::g_t0Xr = 0;
  const int64_t D0 = 1'000'000'000'000;  // an XrTime (ns)
  const int64_t kFrame = 22'222'222;     // 45 fps
  // The first locate latches t0: yaw 0 there.
  const Located l0 = Locate(D0);
  const bool latched = ps::g_t0Xr.load() == D0 && std::fabs(Wrap(YawOf(l0, 0))) < 1e-3;
  // Same displayTime, two callers (DCS for the real frame, a layer after it), the wall clock moving in between and
  // another display time located in between: the same bytes.
  const Located a1 = Locate(D0 + 2 * kFrame);
  Sleep(30);
  Locate(D0 + 7 * kFrame);
  const Located a2 = Locate(D0 + 2 * kFrame);
  const bool same = memcmp(a1.v, a2.v, sizeof(a1.v)) == 0;
  // Every view of one locate rotated alike (the yaw about the head's centre).
  double spread = 0;
  for (int i = 1; i < 4; ++i) spread = std::max(spread, std::fabs(Wrap(YawOf(a1, i) - YawOf(a1, 0))));
  const double yawA = YawOf(a1, 0);
  // A synthetic display time halfway between two real frames: the yaw halfway.
  const double y1 = YawOf(Locate(D0 + 10 * kFrame), 0), y2 = YawOf(Locate(D0 + 11 * kFrame), 0);
  const double ym = YawOf(Locate(D0 + 10 * kFrame + kFrame / 2), 0);
  const double midErr = std::fabs(Wrap(ym - (y1 + Wrap(y2 - y1) / 2)));
  // yaw = rate x (displayTime - t0): 2 frames at 30 deg/s
  const double expectA = 30.0 * 2 * kFrame * 1e-9;
  // Monotonic over 20 s at 30 deg/s (one wrap of 360 included), every display time 1/90 s apart.
  int nonMono = 0;
  double prev = YawOf(Locate(D0), 0), minStep = 1e9, maxStep = 0;
  for (int k = 1; k <= 1800; ++k) {
    const double y = YawOf(Locate(D0 + k * (kFrame / 2)), 0);
    const double step = Wrap(y - prev);
    nonMono += step <= 0;
    minStep = std::min(minStep, step), maxStep = std::max(maxStep, step);
    prev = y;
  }
  char what[600];
  sprintf_s(what,
            "R99 pose hook: the yaw is a function of the locate's displayTime: t0 latched at the first locate (%d); the "
            "same displayTime located twice (30 ms and another display time in between) gives the same bytes for all 4 "
            "views (%d); every view of a locate rotated alike (spread %.1e deg); 2 frames in: %.4f deg (rate x dt = "
            "%.4f); a display time halfway between two frames' gets the yaw halfway (error %.1e deg); monotonic over "
            "20 s with a wrap (%d non-increasing steps of 1800, step %.4f..%.4f deg)",
            latched, same, spread, yawA, expectA, midErr, nonMono, minStep, maxStep);
  Check(latched && same && spread < 1e-4 && std::fabs(yawA - expectA) < 1e-4 && midErr < 1e-4 && nonMono == 0 &&
            maxStep - minStep < 1e-3,
        what);
  // HoldPose: the runtime's head moves between two locates of one display time; the held pose is the same.
  ps::g_holdPose = true;
  ps::g_anchorState = 0;
  g_head = 0;
  const Located h1 = Locate(D0 + 30 * kFrame);
  g_head = 7.5;
  const Located h2 = Locate(D0 + 30 * kFrame);
  g_head = 0;
  ps::g_holdPose = false;
  bool holdSame = true;
  for (int i = 0; i < 4; ++i)
    for (int k = 0; k < 7; ++k)
      holdSame = holdSame && std::fabs(reinterpret_cast<const float*>(h1.v + i * 64 + 16)[k] -
                                       reinterpret_cast<const float*>(h2.v + i * 64 + 16)[k]) < 2e-6f;
  Check(holdSame, "R99 pose hook with HoldPose: the runtime's head turned 7.5 deg between two locates of one display "
                  "time: the same poses (the anchor, latched once)");
  // [Suite] PoseWallClock=1: the old clock (QPC at the call): one displayTime, two wall-clock times, two yaws; a
  // locate without its info also falls back to the clock.
  ps::g_wallClock = true;
  LARGE_INTEGER now;
  QueryPerformanceCounter(&now);
  ps::g_t0 = now.QuadPart - 2 * f.QuadPart;  // 2 s ago: ~60 deg
  const double w1 = YawOf(Locate(D0), 0);
  ps::g_t0 = now.QuadPart - 4 * f.QuadPart;  // 4 s ago: ~120 deg
  const double w2 = YawOf(Locate(D0), 0);
  ps::g_wallClock = false;
  ps::g_t0 = now.QuadPart - 1 * f.QuadPart;
  const double wn = YawOf(Locate(D0, false), 0);
  sprintf_s(what, "R99 pose hook: [Suite] PoseWallClock=1 keeps the wall clock (one displayTime, 2 s and 4 s after t0: "
                  "%.1f / %.1f deg); a locate without its info falls back to it (%.1f deg at 1 s)",
            w1, w2, wn);
  Check(std::fabs(w1 - 60) < 2 && std::fabs(w2 - 120) < 2 && std::fabs(wn - 30) < 2, what);
  ps::g_tramp = keepTramp, ps::g_on = keepOn, ps::g_sweep = keepSweep, ps::g_holdPose = keepHoldPose;
  ps::g_taxi = keepTaxi, ps::g_rateDps = keepRate, ps::g_hold = keepHold, ps::g_pitchDeg = keepPitch;
  ps::g_t0Xr = 0, ps::g_anchorState = 0;
}

// ---- 2. the replay stage ----
// RunSym against a state as DCS leaves it: its own CS shader, b0 / b1 (b0 with an offset range when the device has
// ID3D11DeviceContext1), t0-t1, u0-u2, and its target bound as RTV 0 plus RTV 1 and a DSV. Afterwards every one of
// them is bound again, exactly.
void TestRunSymState() {
  ID3D11Device* dev = nullptr;
  ID3D11DeviceContext* c = nullptr;
  const D3D_FEATURE_LEVEL want[] = {D3D_FEATURE_LEVEL_11_1};
  if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, want, 1, D3D11_SDK_VERSION, &dev, nullptr, &c))) {
    Check(false, "R99 RunSym: WARP device");
    return;
  }
  ID3D11DeviceContext1* c1 = nullptr;
  c->QueryInterface(__uuidof(ID3D11DeviceContext1), reinterpret_cast<void**>(&c1));
  auto tex = [&](DXGI_FORMAT fmt, UINT bind) {
    D3D11_TEXTURE2D_DESC td{};
    td.Width = td.Height = 64, td.MipLevels = td.ArraySize = 1, td.Format = fmt, td.SampleDesc.Count = 1, td.BindFlags = bind;
    ID3D11Texture2D* t = nullptr;
    dev->CreateTexture2D(&td, nullptr, &t);
    return t;
  };
  ID3D11Texture2D* hdr = tex(DXGI_FORMAT_R16G16B16A16_FLOAT, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE);
  ID3D11Texture2D* rt1 = tex(DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_RENDER_TARGET);
  ID3D11Texture2D* ds = tex(DXGI_FORMAT_D32_FLOAT, D3D11_BIND_DEPTH_STENCIL);
  ID3D11Texture2D* s0 = tex(DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_SHADER_RESOURCE);
  ID3D11Texture2D* s1 = tex(DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_SHADER_RESOURCE);
  ID3D11Texture2D* uT[3] = {tex(DXGI_FORMAT_R32_UINT, D3D11_BIND_UNORDERED_ACCESS), tex(DXGI_FORMAT_R32_UINT, D3D11_BIND_UNORDERED_ACCESS),
                            tex(DXGI_FORMAT_R32_UINT, D3D11_BIND_UNORDERED_ACCESS)};
  ID3D11Texture2D* sig = tex(DXGI_FORMAT_R32_UINT, D3D11_BIND_UNORDERED_ACCESS);
  ID3D11RenderTargetView *rtv0 = nullptr, *rtv1 = nullptr;
  ID3D11DepthStencilView* dsv = nullptr;
  ID3D11ShaderResourceView *hdrSrv = nullptr, *srv0 = nullptr, *srv1 = nullptr;
  ID3D11UnorderedAccessView *uav[3] = {}, *sigUav = nullptr;
  dev->CreateRenderTargetView(hdr, nullptr, &rtv0);
  dev->CreateRenderTargetView(rt1, nullptr, &rtv1);
  dev->CreateDepthStencilView(ds, nullptr, &dsv);
  dev->CreateShaderResourceView(hdr, nullptr, &hdrSrv);
  dev->CreateShaderResourceView(s0, nullptr, &srv0);
  dev->CreateShaderResourceView(s1, nullptr, &srv1);
  for (int i = 0; i < 3; ++i) dev->CreateUnorderedAccessView(uT[i], nullptr, &uav[i]);
  dev->CreateUnorderedAccessView(sig, nullptr, &sigUav);
  D3D11_BUFFER_DESC bd{};
  bd.ByteWidth = 4096 * 16, bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
  ID3D11Buffer *cb0 = nullptr, *cb1 = nullptr;
  dev->CreateBuffer(&bd, nullptr, &cb0);
  dev->CreateBuffer(&bd, nullptr, &cb1);
  const bool ok = cmask::Compile() && sy::MakeBase(dev) && hdr && rtv0 && rtv1 && dsv && uav[2] && cb1;
  bool exact = false, both = false;
  if (ok) {
    ID3D11DeviceContext* keepC = fixdepth::g_s.ctx;
    ID3D11DeviceContext1* keepC1 = fixdepth::g_s.ctx1;
    fixdepth::g_s.ctx = c, fixdepth::g_s.ctx1 = c1;
    ID3D11ComputeShader* dcsCs = sy::g_b.cs[5];  // "DCS's" compute shader: any other one
    auto bindDcs = [&] {
      c->CSSetShader(dcsCs, nullptr, 0);
      ID3D11Buffer* cbs[2] = {cb0, cb1};
      if (c1) {
        const UINT first[2] = {256, 0}, num[2] = {16, 4096};  // b0 with an offset range
        c1->CSSetConstantBuffers1(0, 2, cbs, first, num);
      } else {
        c->CSSetConstantBuffers(0, 2, cbs);
      }
      ID3D11ShaderResourceView* ss[2] = {srv0, srv1};
      c->CSSetShaderResources(0, 2, ss);
      c->CSSetUnorderedAccessViews(0, 3, uav, nullptr);
      ID3D11RenderTargetView* rs[2] = {rtv0, rtv1};
      c->OMSetRenderTargets(2, rs, dsv);
    };
    auto check = [&] {
      ID3D11ComputeShader* cs = nullptr;
      c->CSGetShader(&cs, nullptr, nullptr);
      ID3D11Buffer* cbs[2] = {};
      UINT first[2] = {}, num[2] = {};
      if (c1) c1->CSGetConstantBuffers1(0, 2, cbs, first, num);
      else c->CSGetConstantBuffers(0, 2, cbs);
      ID3D11ShaderResourceView* ss[2] = {};
      c->CSGetShaderResources(0, 2, ss);
      ID3D11UnorderedAccessView* us[3] = {};
      c->CSGetUnorderedAccessViews(0, 3, us);
      ID3D11RenderTargetView* rs[8] = {};
      ID3D11DepthStencilView* d = nullptr;
      c->OMGetRenderTargets(8, rs, &d);
      bool e = cs == dcsCs && cbs[0] == cb0 && cbs[1] == cb1 && ss[0] == srv0 && ss[1] == srv1 && us[0] == uav[0] &&
               us[1] == uav[1] && us[2] == uav[2] && rs[0] == rtv0 && rs[1] == rtv1 && !rs[2] && d == dsv;
      if (c1) e = e && first[0] == 256 && num[0] == 16 && first[1] == 0 && num[1] == 4096;
      if (cs) cs->Release();
      for (auto* p : cbs) if (p) p->Release();
      for (auto* p : ss) if (p) p->Release();
      for (auto* p : us) if (p) p->Release();
      for (auto* p : rs) if (p) p->Release();
      if (d) d->Release();
      return e;
    };
    bindDcs();
    // The scan shader over DCS's target (t0 = the bound RTV 0's resource): it must see it (signatures written).
    const UINT zero[4] = {};
    c->ClearUnorderedAccessViewUint(sigUav, zero);
    const float red[4] = {3, 1, 0.5f, 1};
    c->ClearRenderTargetView(rtv0, red);
    sy::RunSym(sy::g_b.cs[4], sy::g_b.cb[0], hdrSrv, nullptr, sigUav, sy::g_b.statUav, nullptr, 1, 1, true);
    exact = check();
    sy::RunSym(sy::g_b.cs[4], sy::g_b.cb[0], hdrSrv, nullptr, sigUav, sy::g_b.statUav, nullptr, 1, 1, false);
    both = check();
    fixdepth::g_s.ctx = keepC, fixdepth::g_s.ctx1 = keepC1;
  }
  Check(ok && exact && both,
        "R99 RunSym (the replay's lean dispatch): DCS's CS shader, b0 (with its offset range) and b1, t0-t1, u0-u2, its "
        "RTV 0 / RTV 1 / DSV all bound again exactly after the dispatch, with the OM targets taken off and without");
  sy::ReleaseBase();
  for (auto* p : uav) Rel(p);
  Rel(sigUav), Rel(cb0), Rel(cb1), Rel(rtv0), Rel(rtv1), Rel(dsv), Rel(hdrSrv), Rel(srv0), Rel(srv1);
  for (auto* p : uT) Rel(p);
  Rel(sig), Rel(hdr), Rel(rt1), Rel(ds), Rel(s0), Rel(s1), Rel(c1), Rel(c), Rel(dev);
}

// The per-record path (ovl_cpu.h RenderHook -> SymRecordSeh -> sym::Record): ns per record of a run (the type
// unchanged: no QPC, no D3D11), against the same hook with the export's views off. CPU only.
void NoRender(void*, uint32_t) {}
void BenchPerRecord() {
  namespace oc = ckipd::ocpu;
  const auto keepRender = oc::g_renderOrig;
  const void* keepRet = oc::g_renderRet;
  const auto keepRec = oc::g_symRec;
  const DWORD keepTid = ptiming::g_topTid.load();
  oc::g_renderOrig = &NoRender;
  oc::g_renderRet = nullptr;
  oc::g_symRec = &sy::Record;
  ptiming::g_topTid = GetCurrentThreadId();
  alignas(16) uint8_t record[0xd0] = {};
  record[oc::kRecC9] = 1;
  alignas(16) uint8_t ro[0x100] = {};
  const uint8_t* recp = record + oc::kRecFromRo;
  memcpy(ro + oc::kRoRec, &recp, sizeof(recp));
  oc::t_ovlOpen = true, oc::t_ovlLdr = false, oc::t_ovlView = 2;
  sy::g_fv[2] = sy::FrameView{};
  sy::g_fv[2].armed = sy::g_fv[2].tgtChecked = true;
  sy::g_fv[2].cur = 1;  // inside the HUD run
  LARGE_INTEGER f0, f1, fr;
  QueryPerformanceFrequency(&fr);
  constexpr int N = 200000;
  auto ns = [&](uint32_t views) {
    oc::g_symViews = views;
    for (int i = 0; i < 2000; ++i) oc::RenderHook(ro, 0);
    QueryPerformanceCounter(&f0);
    for (int i = 0; i < N; ++i) oc::RenderHook(ro, 0);
    QueryPerformanceCounter(&f1);
    return (f1.QuadPart - f0.QuadPart) * 1e9 / fr.QuadPart / N;
  };
  double on = 1e9, off = 1e9;
  for (int k = 0; k < 3; ++k) on = std::min(on, ns(1u << 2)), off = std::min(off, ns(0));
  QueryPerformanceCounter(&f0);
  for (int i = 0; i < N; ++i) sy::Record(2, 1);
  QueryPerformanceCounter(&f1);
  const double rec = (f1.QuadPart - f0.QuadPart) * 1e9 / fr.QuadPart / N;
  const bool stayed = sy::g_fv[2].cur == 1 && sy::g_fv[2].runs == 0;
  printf("     (R99 per-record microbenchmark, this CPU: the render hook %.1f ns per record with the export's view on, "
         "%.1f ns off (the export's share %.1f ns; x 672 records of R94's F-16 frame = %.2f us); sym::Record alone "
         "%.1f ns)\n",
         on, off, on - off, (on - off) * 672 / 1000, rec);
  Check(stayed && on - off < 15, "R99 per-record path: a record of an open run costs the export a few ns (no QPC, no "
                                 "D3D11 call; the run stays open)");
  sy::g_fv[2] = sy::FrameView{};
  oc::t_ovlOpen = false, oc::t_ovlView = -1;
  oc::g_symViews = 0;
  oc::g_renderOrig = keepRender, oc::g_renderRet = keepRet, oc::g_symRec = keepRec;
  ptiming::g_topTid = keepTid;
}

// QV_R95_HW=1: the replay stage's calls on the hardware driver after a draw into DCS's target (as live: the target was
// just drawn to): RunOv (R97) against RunSym (R99), the run-start copy, and CheckTarget's look-up against its R99 cache.
void HwTransitionCost() {
  ID3D11Device* dev = nullptr;
  ID3D11DeviceContext* c = nullptr;
  const D3D_FEATURE_LEVEL want[] = {D3D_FEATURE_LEVEL_11_1};
  if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, want, 1, D3D11_SDK_VERSION, &dev, nullptr, &c)))
    return;
  ID3D11DeviceContext1* c1 = nullptr;
  c->QueryInterface(__uuidof(ID3D11DeviceContext1), reinterpret_cast<void**>(&c1));
  D3D11_TEXTURE2D_DESC td{};
  td.Width = 2028, td.Height = 2004, td.MipLevels = td.ArraySize = 1, td.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
  td.SampleDesc.Count = 1, td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
  ID3D11Texture2D *hdr = nullptr, *pre = nullptr, *acc = nullptr;
  ID3D11RenderTargetView* rtv = nullptr;
  ID3D11ShaderResourceView *srv = nullptr, *preSrv = nullptr;
  ID3D11UnorderedAccessView* accUav = nullptr;
  dev->CreateTexture2D(&td, nullptr, &hdr);
  dev->CreateRenderTargetView(hdr, nullptr, &rtv);
  dev->CreateShaderResourceView(hdr, nullptr, &srv);
  td.Width = td.Height = 1024, td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  dev->CreateTexture2D(&td, nullptr, &pre);
  dev->CreateShaderResourceView(pre, nullptr, &preSrv);
  td.Format = DXGI_FORMAT_R32G32B32A32_FLOAT, td.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
  dev->CreateTexture2D(&td, nullptr, &acc);
  dev->CreateUnorderedAccessView(acc, nullptr, &accUav);
  const bool ok = hdr && rtv && srv && pre && accUav && cmask::Compile() && sy::MakeBase(dev);
  if (ok) {
    ID3D11DeviceContext* keepC = fixdepth::g_s.ctx;
    ID3D11DeviceContext1* keepC1 = fixdepth::g_s.ctx1;
    fixdepth::g_s.ctx = c, fixdepth::g_s.ctx1 = c1;
    c->OMSetRenderTargets(1, &rtv, nullptr);
    const D3D11_VIEWPORT vp{0, 0, 2028, 2004, 0, 1};
    c->RSSetViewports(1, &vp);
    LARGE_INTEGER f0, f1, fr;
    QueryPerformanceFrequency(&fr);
    // A record's draw before each call: a triangle over a 960 px HUD box of DCS's target with its own VS / PS.
    const char* kV = "float4 main(uint id : SV_VertexID) : SV_Position { float2 p = float2((id << 1) & 2, id & 2); "
                     "return float4(p * float2(2, -2) + float2(-1, 1), 0, 1); }";
    const char* kP = "float4 main() : SV_Target { return float4(0.1, 2.0, 0.2, 0.5); }";
    std::vector<uint8_t> vb, pb;
    std::string err;
    ID3D11VertexShader* vs = nullptr;
    ID3D11PixelShader* psh = nullptr;
    if (vfxrig::CompileTo(kV, "vs_5_0", vb, err)) dev->CreateVertexShader(vb.data(), vb.size(), nullptr, &vs);
    if (vfxrig::CompileTo(kP, "ps_5_0", pb, err)) dev->CreatePixelShader(pb.data(), pb.size(), nullptr, &psh);
    c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    c->IASetInputLayout(nullptr);
    c->VSSetShader(vs, nullptr, 0);
    c->PSSetShader(psh, nullptr, 0);
    const D3D11_VIEWPORT box{500, 500, 960, 960, 0, 1};
    auto us = [&](auto fn) {  // per call, after a record's draw into DCS's target
      constexpr int N = 300;
      double best = 1e9;
      for (int rep = 0; rep < 3; ++rep) {
        double sum = 0;
        for (int i = 0; i < N; ++i) {
          c->RSSetViewports(1, &box);
          c->Draw(3, 0);
          c->RSSetViewports(1, &vp);
          QueryPerformanceCounter(&f0);
          fn();
          QueryPerformanceCounter(&f1);
          sum += (f1.QuadPart - f0.QuadPart) * 1e6 / fr.QuadPart;
          if (i % 32 == 31) c->Flush();
        }
        best = std::min(best, sum / N);
      }
      return best;
    };
    const UINT gx = 120, gy = 120;  // a focus HUD window of ~960 px
    const double ov = us([&] {
      cmask::covl::RunOv(sy::g_b.cs[0], sy::g_b.cb[0], srv, preSrv, accUav, sy::g_b.statUav, nullptr, gx, gy, hdr);
    });
    const double rs = us([&] {
      sy::RunSym(sy::g_b.cs[0], sy::g_b.cb[0], srv, preSrv, accUav, sy::g_b.statUav, nullptr, gx, gy, true);
    });
    const D3D11_BOX b{500, 500, 0, 1460, 1460, 1};
    const double copy = us([&] { c->CopySubresourceRegion(pre, 0, 0, 0, 0, hdr, 0, &b); });
    const double look = us([&] {  // R97's CheckTarget look-up of an unchanged target
      ID3D11RenderTargetView* r = nullptr;
      c->OMGetRenderTargets(1, &r, nullptr);
      D3D11_VIEWPORT v{};
      UINT n = 1;
      c->RSGetViewports(&n, &v);
      ID3D11Resource* res = nullptr;
      D3D11_RENDER_TARGET_VIEW_DESC rd{};
      if (r) r->GetResource(&res), r->GetDesc(&rd), r->Release();
      if (res) res->Release();
    });
    const double look99 = us([&] {  // R99's: the RTV pointer and the box
      ID3D11RenderTargetView* r = nullptr;
      c->OMGetRenderTargets(1, &r, nullptr);
      D3D11_VIEWPORT v{};
      UINT n = 1;
      c->RSGetViewports(&n, &v);
      if (r) r->Release();
    });
    c->Flush();
    printf("     (R99 replay-stage calls on the hardware driver after a draw into DCS's target, us per call: the "
           "accumulation dispatch RunOv (R97) %.2f -> RunSym (R99) %.2f; the run-start copy %.2f; CheckTarget's "
           "look-up %.2f -> %.2f)\n",
           ov, rs, copy, look, look99);
    Check(rs < ov, "R99 HW: RunSym costs the render thread less than RunOv for the same dispatch");
    c->VSSetShader(nullptr, nullptr, 0);
    c->PSSetShader(nullptr, nullptr, 0);
    Rel(vs), Rel(psh);
    fixdepth::g_s.ctx = keepC, fixdepth::g_s.ctx1 = keepC1;
  }
  sy::ReleaseBase();
  Rel(hdr), Rel(pre), Rel(acc), Rel(rtv), Rel(srv), Rel(preSrv), Rel(accUav), Rel(c1), Rel(c), Rel(dev);
}

inline void Run() {
  printf("-- R99 pose displayTime, replay-stage cost --\n");
  TestPoseDisplayTime();
  TestRunSymState();
  BenchPerRecord();
  if (const char* e = getenv("QV_R95_HW"); e && atoi(e) != 0) HwTransitionCost();
}

}  // namespace r99test
