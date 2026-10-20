// R38 rank 2 (focus_bcast.h): the focus-of-periphery affine on the census matrices, the broadcast viewport,
// the motion-vector fixup (static and moving gaze; the HLSL compiles), the FastGS passthrough generated from VS
// output signatures (layout reproduced, unreproducible signatures refused), NVAPI FastGS creation on this PC's
// GPU when it is NVIDIA's, both keys off by default, and the FocusGeomProbe viewport arm on a WARP context
// (alone and chained with the depth publisher's RSSetViewports hook).
#pragma once

namespace fbctest {

struct FakePass {
  void* vt = nullptr;
  uint64_t len = 0;
  char name[40] = {};
};

fbc::Rows Mul(const fbc::Rows& p, const double v[4][4]) {  // rows of P * V (clip = P (V x))
  fbc::Rows r{};
  for (int i = 0; i < 4; ++i)
    for (int j = 0; j < 4; ++j)
      for (int k = 0; k < 4; ++k) r.r[i][j] += p.r[i][k] * v[k][j];
  return r;
}
void Clip(const fbc::Rows& m, const double x[4], double c[4]) {
  for (int i = 0; i < 4; ++i) c[i] = m.r[i][0] * x[0] + m.r[i][1] * x[1] + m.r[i][2] * x[2] + m.r[i][3] * x[3];
}

void Run() {
  using namespace fbc;
  // Census frame 1 (R38 3.1): periphery L and focus L rows x / y; z, w rows reverse-Z infinite (near 0.02).
  const Rows p{{{0.78074, 0, 0.1958, 0}, {0, 0.79064, 0.0010288, 0}, {0, 0, 0, 0.02}, {0, 0, 1, 0}}};
  const Rows f{{{2.2963, 0, 0.097157, 0}, {0, 2.3254, 0.00041458, 0}, {0, 0, 0, 0.02}, {0, 0, 1, 0}}};
  Affine a;
  const bool ok = SolveAffine(p, f, a);
  Check(ok && std::fabs(a.ax - 2.9412) < 2e-4 && std::fabs(a.bx + 0.4787) < 2e-4 && std::fabs(a.ay - 2.9412) < 2e-4 &&
            std::fabs(a.by + 0.0026) < 1e-4,
        "fbc: census focus = affine of periphery (x 2.9412 x_p - 0.4787 w, y 2.9412 y_p - 0.0026 w)");
  // The same affine through a view matrix (camera rotated and moved): view-projection rows.
  const double c = std::cos(0.3), s = std::sin(0.3);
  const double view[4][4] = {{c, 0, -s, 1.5}, {0, 1, 0, -2.0}, {s, 0, c, 4.0}, {0, 0, 0, 1}};
  Affine a2;
  Check(SolveAffine(Mul(p, view), Mul(f, view), a2) && std::fabs(a2.ax - a.ax) < 1e-9 && std::fabs(a2.bx - a.bx) < 1e-9,
        "fbc: the affine is the same on view-projection rows");
  Rows bad = f;
  bad.r[2][3] = 0.021;
  Check(!SolveAffine(p, bad, a2), "fbc: different z rows refused");
  bad = f;
  bad.r[0][1] = 0.01;
  Check(!SolveAffine(p, bad, a2), "fbc: a cross term refused");

  // Broadcast viewport (focus 1176 x 1164): R38's Width 3,459, TopLeftX -1,423; it maps periphery NDC to the same
  // focus pixel as the focus projection.
  D3D11_VIEWPORT v{};
  Check(BroadcastViewport(a, 0, 0, 1176, 1164, v) && std::fabs(v.Width - 3459) < 1.5 && std::fabs(v.TopLeftX + 1423) < 1.5,
        "fbc: broadcast viewport 3459 wide at x -1423");
  double worst = 0;
  for (int i = 0; i < 50; ++i) {
    const double x[4] = {-3 + 0.13 * i, 2 - 0.07 * i, 10 + 3.0 * i, 1};
    double cp[4], cf[4];
    Clip(p, x, cp), Clip(f, x, cf);
    const double viaVpX = v.TopLeftX + (cp[0] / cp[3] + 1) * 0.5 * v.Width;
    const double viaVpY = v.TopLeftY + (1 - cp[1] / cp[3]) * 0.5 * v.Height;
    const double ownX = (cf[0] / cf[3] + 1) * 0.5 * 1176, ownY = (1 - cf[1] / cf[3]) * 0.5 * 1164;
    worst = std::max(worst, std::max(std::fabs(viaVpX - ownX), std::fabs(viaVpY - ownY)));
  }
  Check(worst < 0.05, "fbc: viewport mapping = focus projection within 0.05 px (census rounding)");
  {
    Affine huge = a;
    huge.ax = 40;
    Check(!BroadcastViewport(huge, 0, 0, 1176, 1164, v), "fbc: viewport beyond D3D11 bounds refused");
  }

  // Motion vectors: a world point, current and previous view-projections (camera moved), focus = affine of the
  // periphery with the gaze moved between frames (prev affine differs).
  {
    const double vPrev[4][4] = {{c, 0, -s, 1.45}, {0, 1, 0, -2.01}, {s, 0, c, 3.9}, {0, 0, 0, 1}};
    const Rows curP = Mul(p, view), prevP = Mul(p, vPrev);
    Affine cur = a, prev = a;
    prev.bx += 0.05, prev.by -= 0.03, prev.ax *= 1.01;
    auto focusOf = [](const Rows& r, const Affine& af) {
      Rows o = r;
      for (int k = 0; k < 4; ++k) {
        o.r[0][k] = af.ax * r.r[0][k] + af.bx * r.r[3][k];
        o.r[1][k] = af.ay * r.r[1][k] + af.by * r.r[3][k];
      }
      return o;
    };
    const Rows curF = focusOf(curP, cur), prevF = focusOf(prevP, prev);
    const double dP[2] = {476, 468}, dF[2] = {1176, 1164};
    double err = 0, mag = 0;
    for (int i = 0; i < 40; ++i) {
      const double x[4] = {-0.6 + 0.03 * i, 1.2 - 0.05 * i, 30 + 2.0 * i, 1};
      double cc[4], pc[4], cf[4], pf[4];
      Clip(curP, x, cc), Clip(prevP, x, pc), Clip(curF, x, cf), Clip(prevF, x, pf);
      const Mv mvP{0.5 * dP[0] * (pc[0] / pc[3] - cc[0] / cc[3]), 0.5 * dP[1] * (pc[1] / pc[3] - cc[1] / cc[3])};
      const Mv want{0.5 * dF[0] * (pf[0] / pf[3] - cf[0] / cf[3]), 0.5 * dF[1] * (pf[1] / pf[3] - cf[1] / cf[3])};
      // The focus pixel whose centre is the point (continuous coordinates).
      const double px = (cf[0] / cf[3] + 1) * 0.5 * dF[0] - 0.5, py = (1 - cf[1] / cf[3]) * 0.5 * dF[1] - 0.5;
      const Mv got = FixMv(px, py, mvP, cur, prev, dP[0], dP[1], dF[0], dF[1]);
      err = std::max(err, std::max(std::fabs(got.x - want.x), std::fabs(got.y - want.y)));
      mag = std::max(mag, std::max(std::fabs(want.x), std::fabs(want.y)));
    }
    Check(err < 1e-6 * std::max(1.0, mag), "fbc: MV fixup = the focus PS's own motion vector (gaze moved, camera moved)");
    std::string e;
    ID3DBlob* b = CompileHlsl(kMvFixHlsl, "main", "ps_5_0", &e);
    Check(b != nullptr, "fbc: MV fixup pixel shader compiles");
    if (b) b->Release();
  }

  // FastGS passthrough from VS output signatures.
  const char* vsSrc = R"(
struct O { float4 pos : SV_Position; float2 uv : TEXCOORD0; float3 n : TEXCOORD1; float4 cur : TEXCOORD2;
           float4 prev : TEXCOORD3; float tint : TEXCOORD4; nointerpolation uint id : BLENDINDICES0; };
O main(float3 p : POSITION, uint i : SV_VertexID) {
  O o; o.pos = float4(p, 1); o.uv = p.xy; o.n = p; o.cur = o.pos; o.prev = o.pos * 2; o.tint = p.x; o.id = i;
  return o;
})";
  const char* vsPacked = R"(
struct O { float2 a : TEXCOORD0; float2 b : TEXCOORD1; float c : TEXCOORD2; float3 d : TEXCOORD3;
           float4 pos : SV_Position; float e : COLOR0; };
O main(float3 p : POSITION) { O o; o.a = p.xy; o.b = p.yz; o.c = p.z; o.d = p; o.pos = float4(p, 1); o.e = 1; return o; }
)";
  const char* vsClip = R"(
struct O { float4 pos : SV_Position; float clip : SV_ClipDistance0; };
O main(float3 p : POSITION) { O o; o.pos = float4(p, 1); o.clip = p.x; return o; })";
  int gsOk = 0;
  for (const char* src : {vsSrc, vsPacked}) {
    std::string e;
    ID3DBlob* vb = CompileHlsl(src, "main", "vs_5_0", &e);
    std::vector<SigElem> sig;
    const bool parsed = vb && ParseOutputSignature(vb->GetBufferPointer(), vb->GetBufferSize(), sig);
    for (bool rt : {false, true}) {
      const std::string h = parsed ? FastGsHlsl(sig, 3, rt) : std::string();
      ID3DBlob* gb = h.empty() ? nullptr : CompileHlsl(h, "main", "gs_5_0", &e);
      if (gb && CheckGsMatches(sig, gb->GetBufferPointer(), gb->GetBufferSize(), rt)) ++gsOk;
      if (gb) gb->Release();
    }
    if (vb) vb->Release();
  }
  Check(gsOk == 4, "fbc: FastGS passthrough reproduces the VS output layout (plain, packed; with/without RT index)");
  {
    std::string e;
    ID3DBlob* vb = CompileHlsl(vsClip, "main", "vs_5_0", &e);
    std::vector<SigElem> sig;
    Check(vb && ParseOutputSignature(vb->GetBufferPointer(), vb->GetBufferSize(), sig) && FastGsHlsl(sig, 3, false).empty(),
          "fbc: a signature with clip distances is refused");
    if (vb) vb->Release();
    const uint8_t junk[40] = {'D', 'X', 'B', 'C'};
    Check(!ParseOutputSignature(junk, sizeof(junk), sig) && !ParseOutputSignature(junk, 8, sig),
          "fbc: malformed DXBC refused");
  }
  {
    // The readiness check's built-in signature compiles to a matching GS too.
    std::string e;
    const std::string h = FastGsHlsl(ProbeSignature(), 3, false);
    ID3DBlob* gb = h.empty() ? nullptr : CompileHlsl(h, "main", "gs_5_0", &e);
    const bool match = gb && CheckGsMatches(ProbeSignature(), gb->GetBufferPointer(), gb->GetBufferSize(), false);
    if (!match) printf("     (fbc: generated GS:\n%s\n compiler: %s)\n", h.c_str(), e.c_str());
    Check(match, "fbc: readiness signature compiles to a matching FastGS");
    if (gb) gb->Release();
  }
  // NVAPI FastGS on this PC's GPU (skipped when it is not NVIDIA's).
  {
    ID3D11Device* hw = nullptr;
    D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
    nvfgs::Api api;
    if (nvfgs::Load(api) && SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &fl, 1,
                                                        D3D11_SDK_VERSION, &hw, nullptr, nullptr))) {
      int st = -1, st2 = -1;
      ID3D11GeometryShader* g1 = CreateFastGs(hw, ProbeSignature(), 3, false, &st);
      ID3D11GeometryShader* g2 = CreateFastGs(hw, ProbeSignature(), 3, true, &st2);
      Check(g1 && g2 && st == 0 && st2 == 0, "fbc: NvAPI FastGS (viewport mask, RT offset) created on this GPU");
      if (g1) g1->Release();
      if (g2) g2->Release();
      hw->Release();
    } else {
      printf("     (fbc: no NVIDIA driver / hardware device: NVAPI FastGS creation not tested)\n");
    }
  }

  // Keys off by default.
  {
    Config def;
    Check(def.suiteFocusGeomProbe == 0 && def.focusBroadcast == 0 && fbc::g_mask.load() == 0,
          "fbc: FocusGeomProbe and FocusBroadcast off by default");
    Check(GetPrivateProfileIntW(L"Suite", L"FocusGeomProbe", 0, L"Z:\\no\\such\\none.ini") == 0,
          "fbc: a missing key reads as 0");
  }

  // FocusGeomProbe arm on WARP.
  ID3D11Device* dev = nullptr;
  ID3D11DeviceContext* ctx = nullptr;
  D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
  if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, &fl, 1, D3D11_SDK_VERSION, &dev, nullptr,
                               &ctx))) {
    Check(false, "fbc: WARP device");
    return;
  }
  ID3D11Device* savedDev = plod::g_dev;
  ID3D11DeviceContext* savedCtx = plod::g_ctx;
  const DWORD savedTid = ptiming::g_topTid.load();
  plod::g_dev = dev, plod::g_ctx = ctx;
  ptiming::g_topTid = GetCurrentThreadId();
  plod::g_kind.clear();
  static FakePass hm, gb, hmd;
  hm.len = strlen("HeightMapPass"), strcpy_s(hm.name, "HeightMapPass");
  gb.len = strlen("GBufferPass"), strcpy_s(gb.name, "GBufferPass");
  hmd.len = strlen("CopyToHMDPass"), strcpy_s(hmd.name, "CopyToHMDPass");
  void** vt = *reinterpret_cast<void***>(ctx);
  const int slot = DcsQv_ContextSlot("RSSetViewports");
  void* d3d = vt[slot];
  const D3D11_VIEWPORT dcsVp[2] = {{0, 0, 1176, 1164, 0, 1}, {0, 0, 476, 468, 0, 1}};
  auto bound = [&] {
    D3D11_VIEWPORT b[2]{};
    UINT n = 2;
    ctx->RSGetViewports(&n, b);
    return n ? b[0] : D3D11_VIEWPORT{};
  };
  // One frame: per view a main and a second GBufferPass, then CopyToHMDPass; DCS sets its viewport inside.
  int probed = 0, stock = 0, restored = 0, slotBack = 0;
  auto frame = [&](bool fdOnTop) {
    fbc::OnBoundary(&hm, true, 0);
    fbc::OnBoundary(&hm, false, 0);
    for (int view = 0; view < 4; ++view) {
      for (int pass = 0; pass < 2; ++pass) {
        fbc::OnBoundary(&gb, true, 0);
        if (fdOnTop && vt[slot] != reinterpret_cast<void*>(&fixdepth::VpHook)) {  // the depth publisher arms after us
          fixdepth::g_vpOrig = reinterpret_cast<fixdepth::VpFn>(vt[slot]);
          d3ds::WriteTablePointer(&vt[slot], reinterpret_cast<void*>(&fixdepth::VpHook));
        }
        ctx->RSSetViewports(1, &dcsVp[view < 2 ? 1 : 0]);  // through the table, as DCS
        const D3D11_VIEWPORT b = bound();
        const bool want = pass == 0 && (fbc::g_mask.load() >> view & 1);
        if (want && b.TopLeftX == -64 && b.Width == 16) ++probed;
        if (!want && b.Width == dcsVp[view < 2 ? 1 : 0].Width) ++stock;
        fbc::OnBoundary(&gb, false, 0);
        if (fdOnTop && vt[slot] == reinterpret_cast<void*>(&fixdepth::VpHook))  // ... and disarms after us
          d3ds::WriteTablePointer(&vt[slot], reinterpret_cast<void*>(fixdepth::g_vpOrig));
        if (bound().Width == dcsVp[view < 2 ? 1 : 0].Width) ++restored;
      }
      fbc::OnBoundary(&hmd, true, 0);
      fbc::OnBoundary(&hmd, false, 0);
    }
  };
  fbc::g_mask = 0;
  frame(false);
  Check(probed == 0 && stock == 8 && vt[slot] == d3d, "fbc probe: mask 0 changes nothing, table untouched");
  probed = stock = restored = 0;
  fbc::g_mask = 12;  // focus L and R
  frame(false);
  slotBack += vt[slot] == d3d;
  Check(probed == 2 && stock == 6 && restored == 8, "fbc probe: focus main passes off-target, the rest stock, "
                                                    "DCS's viewport back at every pass end");
  Check(slotBack == 1, "fbc probe: RSSetViewports entry back to d3d11's after the pass");
  // Chained with the depth publisher's hook (it arms after us at the pass begin and disarms after us).
  probed = stock = restored = 0;
  const uint64_t buried0 = fbc::g_buried.load();
  frame(true);
  frame(true);
  Check(probed == 4 && stock == 12 && restored == 16 && fbc::g_buried.load() > buried0 && fbc::g_reentry.load() == 0,
        "fbc probe: under the depth publisher's hook: probed, stock elsewhere, no re-entry");
  Check(fbc::MainOrdinal(0) == 0 && fbc::MainOrdinal(2) == 0 && fbc::MainOrdinal(3) == 1 && fbc::MainOrdinal(4) == 2,
        "fbc probe: main GBufferPass = second-to-last of the view (R44: 3 per view live, the second is the main)");
  fbc::g_mask = 0;
  fbc::Unwrap();
  Check(vt[slot] == d3d && reinterpret_cast<void*>(fixdepth::g_vpOrig) != reinterpret_cast<void*>(&fbc::VpHook),
        "fbc probe: unwrapped from the table and from the publisher's chain");
  fixdepth::g_vpOrig = nullptr;
  plod::g_kind.clear();
  plod::g_dev = savedDev, plod::g_ctx = savedCtx;
  ptiming::g_topTid = savedTid;
  ctx->Release();
  dev->Release();
}

}  // namespace fbctest
