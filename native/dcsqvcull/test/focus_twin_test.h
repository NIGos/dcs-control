// R123 focus twin (src/focus_twin.h, focus_twin_math.h). docs/research/R123_focus_twin.md.
//
//  1. Pure, on DCS's own TerrainContext uploads (census fixture, focus_twin_fixture.h): view identification, the
//     shared / per-view row split, the side-plane formula, the pixel-equal jitter step, the prediction (collect-time
//     cameras both jittered and not, eye-tracked gaze steps) and its misses.
//  2. Pipeline on WARP with a DCS-like fake: TerrainObject vt[1] / MetaMaterial vt[8] vtables (hooked by the real
//     InstallOn), RenderItems laid out as DCS's (the real descriptor reader), a DX11StateManager-like redundant-bind
//     filter, per-view b5 / b7 (ping-pong) / b10 / b11 uploads, per-draw constant-buffer uploads, "instancer" items
//     between the patch runs, per-view patches (their own geometry in the focus, as R120b's census vb0), an MFD view
//     on odd frames, DLSS jitter per view, an eye-tracked gaze moving every frame. Periphery L, R, then focus L, R.
//     Each scenario runs twice from the same inputs: stock (not installed) and with the twin; every focus (and
//     periphery) target is compared byte for byte, every frame.
//     Scenarios: exact output with the twin drawing; prediction mismatch -> stock; F10 (no culling this frame) ->
//     stock; uninstall / reinstall (restart, hot reload) and a scene change; order violation; audit failure (latched
//     off); a fault in the item hook (latched off); shadow mode; key off is a no-op (nothing installed, slots
//     untouched).
//  3. R125 ([Model] FocusTwinInstancer): MetaMaterial_Instancer2 items as DCS's (vt[8] through the vtable, gated by
//     vt[6]): an InstanceManager2 with its type entries (bit index, per-draw int, argument offset), per-view CPU bits,
//     ONE shared instance buffer and ONE shared argument buffer that every view's "culling chain" rewrites before
//     its G-buffer pass (only the (type, sub-draw) pairs whose bit is set: the others keep another view's words),
//     DrawIndexedInstancedIndirect per announced sub-draw, a per-draw constant buffer with the per-draw int, one
//     material binding the TerrainContext buffer at another slot. Scenarios: exact with the instancer twin (and
//     without the per-view enable it would not be: the negative control), a type whose per-draw int differs per view,
//     the argument buffer recreated between the periphery and the focus, instancer items only (FocusTwinPatches=0),
//     no gate pass (not installed), an instancer audit failure, verify, key 0 (slot untouched), the build check.
// QV_R123_ONLY=1: only these tests.
#pragma once

#include "focus_twin_fixture.h"

namespace r123test {

using namespace ftwin;

// ---------------------------------------------------------------------------
// Pure: the census TerrainContexts
// ---------------------------------------------------------------------------
void Tc(int i, uint8_t* out) { memcpy(out, r123fix::kTc[i], kTcBytes); }
inline float RowF(const uint8_t* tc, int r, int k) { return TcRow(tc, r)[k]; }

// A collect-time view-projection (osg::Matrixf, clip = v * M, camera relative) from an upload's VP rows, minus a
// jitter (NDC) on x / y (0: the CI carries the same jitter as the upload).
void CiFromTc(const uint8_t* tc, double jx, double jy, float* m) {
  memset(m, 0, 64);
  for (int k = 0; k < 3; ++k) {
    const double w = RowF(tc, kRowVw, k);
    m[k * 4 + 0] = static_cast<float>(RowF(tc, kRowVx, k) - jx * w);
    m[k * 4 + 1] = static_cast<float>(RowF(tc, kRowVy, k) - jy * w);
    m[k * 4 + 2] = RowF(tc, 22, k);
    m[k * 4 + 3] = static_cast<float>(w);
  }
  m[15] = 1;
}
// Moves a focus TerrainContext's centre by (dx, dy) NDC (an eye-tracked gaze step): P, VP, inverse P, planes.
void ShiftFocus(uint8_t* tc, double dx, double dy) {
  float* px = reinterpret_cast<float*>(tc + 16 * kRowPx);
  float* py = reinterpret_cast<float*>(tc + 16 * kRowPy);
  px[2] = static_cast<float>(px[2] + dx), py[2] = static_cast<float>(py[2] + dy);
  float* vx = reinterpret_cast<float*>(tc + 16 * kRowVx);
  float* vy = reinterpret_cast<float*>(tc + 16 * kRowVy);
  const float* vw = TcRow(tc, kRowVw);
  for (int k = 0; k < 4; ++k) vx[k] = static_cast<float>(vx[k] + dx * vw[k]), vy[k] = static_cast<float>(vy[k] + dy * vw[k]);
  PutRow(tc, kRowIx, 1.0 / px[0], 0, 0, -static_cast<double>(px[2]) / px[0]);
  PutRow(tc, kRowIy, 0, 1.0 / py[1], 0, -static_cast<double>(py[2]) / py[1]);
  for (int i = 0; i < 4; ++i) {
    const float* a = (i < 2) ? vx : vy;
    const double sgn = (i & 1) ? -1.0 : 1.0;
    double p[4];
    for (int k = 0; k < 4; ++k) p[k] = static_cast<double>(vw[k]) + sgn * a[k];
    const double n = std::sqrt(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]);
    PutRow(tc, kRowPlane0 + i, p[0] / n, p[1] / n, p[2] / n, p[3] / n);
  }
}

void PureCensus() {
  uint8_t tc[8][kTcBytes];
  for (int i = 0; i < 8; ++i) Tc(i, tc[i]);
  bool idOk = true;
  for (int i = 0; i < 8; ++i) {
    const ViewId v = ClassifyTc(tc[i]);
    const int want = (i % 4) < 2 ? kPeriph : kFocus;
    idOk &= v.kind == want && v.eye == (want == kPeriph ? (i % 2) : -1) && (v.w == (want == kPeriph ? 476 : 1176));
    // a focus's eye: the periphery of its frame with the same shared rows
    if (want == kFocus) idOk &= SharedRowsEqual(tc[(i / 4) * 4 + (i % 2)], tc[i]) && !SharedRowsEqual(tc[(i / 4) * 4 + 1 - (i % 2)], tc[i]);
  }
  Check(idOk, "R123 census: TerrainContext identifies periphery (sx < 1.5, eye = sign of cx) and focus (eye = the "
              "periphery with its shared rows: a gaze-following centre may sit on either side) in all 8 uploads");
  uint8_t mfd[kTcBytes];
  memcpy(mfd, tc[4], kTcBytes);
  reinterpret_cast<float*>(mfd + 16 * kRowPx)[2] = 0.0f;
  Check(ClassifyTc(mfd).kind == kOther, "R123 census: a centred projection (an MFD sensor camera) is 'other', never paired");
  // The per-view rows are exactly the rows that differ inside an eye (both frames, both eyes).
  bool rowsOk = true, sharedOk = true;
  for (int f = 0; f < 2; ++f)
    for (int e = 0; e < 2; ++e) {
      const uint8_t* p = tc[f * 4 + e];
      const uint8_t* q = tc[f * 4 + 2 + e];
      for (int r = 0; r < kTcRows; ++r) rowsOk &= (memcmp(p + 16 * r, q + 16 * r, 16) != 0) == PerViewRow(r);
      sharedOk &= SharedRowsEqual(p, q);
    }
  Check(rowsOk, "R123 census: periphery and focus of one eye differ in exactly the 12 per-view rows (P, VP, inverse P, planes, size, 132)");
  Check(sharedOk && !SharedRowsEqual(tc[4], tc[7]) && !SharedRowsEqual(tc[5], tc[6]),
        "R123 census: the shared rows are byte-equal within an eye and differ across eyes (camera row 38)");
  // Side planes = normalised (w + x), (w - x), (w + y), (w - y) of the VP rows.
  double planeDev = 0;
  for (int i = 0; i < 8; ++i)
    for (int pl = 0; pl < 4; ++pl) {
      const int a = pl < 2 ? kRowVx : kRowVy;
      const double sgn = (pl & 1) ? -1.0 : 1.0;
      double p[4];
      for (int k = 0; k < 4; ++k) p[k] = static_cast<double>(RowF(tc[i], kRowVw, k)) + sgn * RowF(tc[i], a, k);
      const double n = std::sqrt(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]);
      for (int k = 0; k < 4; ++k)
        planeDev = std::max(planeDev, std::fabs(p[k] / n - RowF(tc[i], kRowPlane0 + pl, k)) / std::max(1.0, std::fabs(p[3] / n)));
    }
  Check(planeDev < 2e-6, "R123 census: rows 28-31 are the normalised (w +- x), (w +- y) side planes of the VP rows");
  printf("     (side planes: largest deviation %.2g)\n", planeDev);
  // R53: the jitter step is equal in pixels in both views of an eye.
  double jit = 0;
  for (int e = 0; e < 2; ++e) {
    const ViewId p0 = ClassifyTc(tc[e]), p1 = ClassifyTc(tc[4 + e]), f0 = ClassifyTc(tc[2 + e]), f1 = ClassifyTc(tc[6 + e]);
    jit = std::max(jit, std::fabs(f1.cx - (f0.cx + (p1.cx - p0.cx) * p0.w / f0.w)));
    jit = std::max(jit, std::fabs(f1.cy - (f0.cy + (p1.cy - p0.cy) * p0.h / f0.h)));
  }
  Check(jit < 2e-6, "R123 census: frame 0 -> 1 focus jitter step = periphery step x Wp / Wf (x and y, both eyes)");
  printf("     (jitter step: largest deviation %.2g NDC)\n", jit);

  // The prediction: collect-time cameras jittered like the upload (J) and jitter-free (U, any jitter).
  const double tol = 5e-5;
  for (int hyp = 0; hyp < 2; ++hyp) {
    bool ok = true;
    double worst = 0;
    for (int e = 0; e < 2; ++e) {
      const double jx[2] = {hyp ? 0.0011 : 0.0, hyp ? -0.0007 : 0.0}, jy[2] = {hyp ? -0.0004 : 0.0, hyp ? 0.0009 : 0.0};
      CiFrame ci[2];
      for (int f = 0; f < 2; ++f) {
        const ViewId vp = ClassifyTc(tc[f * 4 + e]);
        const ViewId vf = ClassifyTc(tc[f * 4 + 2 + e]);
        ci[f].seq = 10 + f;
        ci[f].pairs = 2;
        // the pair of the other eye first (the matcher picks by camera)
        CiFromTc(tc[f * 4 + (1 - e)], jx[f], jy[f], ci[f].p[0]);
        CiFromTc(tc[f * 4 + 2 + (1 - e)], jx[f] * vp.w / vf.w, jy[f] * vp.h / vf.h, ci[f].f[0]);
        CiFromTc(tc[f * 4 + e], jx[f], jy[f], ci[f].p[1]);
        CiFromTc(tc[f * 4 + 2 + e], jx[f] * vp.w / vf.w, jy[f] * vp.h / vf.h, ci[f].f[1]);
      }
      EyeHist h;
      ok &= LearnFocus(h, tc[e], tc[2 + e], ci[0]);
      const Pred pr = PredictFocus(tc[4 + e], ci[1], h);
      double dev = 1;
      ok &= pr.ok && ValidateFocus(pr, tc[6 + e], tol, &dev) == 0;
      worst = std::max(worst, dev);
      // The other eye's focus is not this eye's prediction.
      ok &= ValidateFocus(pr, tc[6 + (1 - e)], tol, nullptr) != 0;
    }
    char what[200];
    snprintf(what, sizeof(what), "R123 census: focus TerrainContext predicted at the periphery pass from this frame's "
             "cameras (%s), within %.0e, other eye rejected", hyp ? "jitter-free cameras" : "jittered cameras", tol);
    Check(ok, what);
    printf("     (%s: largest deviation %.2g)\n", hyp ? "U" : "J", worst);
  }
  // Eye tracking: the gaze moves between the frames; this frame's focus camera follows it, the previous-frame
  // predictor (R53) does not.
  {
    bool ok = true;
    for (int e = 0; e < 2; ++e) {
      uint8_t f1[kTcBytes];
      memcpy(f1, tc[6 + e], kTcBytes);
      ShiftFocus(f1, 0.031, -0.017);
      CiFrame ci0, ci1;
      ci0.seq = 1, ci0.pairs = 1, ci1.seq = 2, ci1.pairs = 1;
      CiFromTc(tc[e], 0, 0, ci0.p[0]);
      CiFromTc(tc[2 + e], 0, 0, ci0.f[0]);
      CiFromTc(tc[4 + e], 0, 0, ci1.p[0]);
      CiFromTc(f1, 0, 0, ci1.f[0]);
      EyeHist h;
      ok &= LearnFocus(h, tc[e], tc[2 + e], ci0);
      const Pred pr = PredictFocus(tc[4 + e], ci1, h);
      ok &= pr.ok && ValidateFocus(pr, f1, tol, nullptr) == 0;
      // R53's predictor: previous focus + the periphery's jitter step.
      const ViewId p0 = ClassifyTc(tc[e]), p1 = ClassifyTc(tc[4 + e]), fp = ClassifyTc(tc[2 + e]), fa = ClassifyTc(f1);
      const double r53cx = fp.cx + (p1.cx - p0.cx) * p0.w / fp.w;
      ok &= std::fabs(r53cx - fa.cx) > 1e-3;
      // The camera moved but the upload did not (a stale or foreign upload): a miss.
      const Pred pr2 = PredictFocus(tc[4 + e], ci1, h);
      ok &= ValidateFocus(pr2, tc[6 + e], tol, nullptr) != 0;
    }
    Check(ok, "R123 census: an eye-tracked gaze step is predicted from this frame's focus camera (R53's previous-frame "
              "predictor misses it); an upload that did not move is rejected");
  }
  {  // no history / no cameras
    EyeHist h;
    CiFrame none, ci;
    ci.seq = 3, ci.pairs = 1;
    CiFromTc(tc[4], 0, 0, ci.p[0]);
    CiFromTc(tc[6], 0, 0, ci.f[0]);
    const Pred a = PredictFocus(tc[4], none, h);
    const Pred b = PredictFocus(tc[4], ci, h);
    Check(!a.ok && a.why == kPwNoCi && !b.ok && b.why == kPwNoHist,
          "R123: no collect-time camera this frame (F10, map) -> no prediction; no previous focus -> no prediction");
  }
}

void PureMisc() {
  // Descriptor map: duplicates refused, lookups.
  DescMap* m = new DescMap();
  m->Clear();
  int32_t dup = -1;
  bool ok = m->Put(11, 0, &dup) && m->Put(22, 1, &dup) && !m->Put(11, 5, &dup) && dup == 0 && m->Get(22) == 1 &&
            m->Get(33) == -1;
  delete m;
  Check(ok, "R123: descriptor map (duplicates refused with the first index, misses -1)");
  // Island rule: runs of >= island descriptors survive into the next frame's good set.
  RunTracker* t = new RunTracker();
  GoodSet* g = new GoodSet();
  t->Begin();
  for (uint64_t d = 1; d <= 5; ++d) t->Add(d);
  t->Break(3);
  t->Add(10), t->Add(11);
  t->Break(3);
  for (uint64_t d = 20; d <= 22; ++d) t->Add(d);
  t->Break(3);
  g->From(*t);
  Check(g->Has(1) && g->Has(5) && !g->Has(10) && !g->Has(11) && g->Has(20) && g->Has(22),
        "R123: island rule (runs of 5 and 3 kept with island 3, a run of 2 dropped)");
  delete t;
  delete g;
  // Shadow hash: order independent, per-view slots excluded, equal states equal.
  Shadow a{}, b{};
  a.h = Shadow::HashAll(a.st), b.h = Shadow::HashAll(b.st);
  ID3D11Buffer* x = reinterpret_cast<ID3D11Buffer*>(0x1000);
  ID3D11Buffer* y = reinterpret_cast<ID3D11Buffer*>(0x2000);
  a.SetVS(reinterpret_cast<void*>(0x10)), a.SetCB(0, 0, x, 0, 0), a.SetCB(0, 11, x, 0, 0);
  b.SetCB(0, 11, y, 0, 0), b.SetCB(0, 0, x, 0, 0), b.SetVS(reinterpret_cast<void*>(0x10));
  const bool hOk = a.h == b.h && a.h == Shadow::HashAll(a.st) && b.h == Shadow::HashAll(b.st);
  a.SetCB(0, 1, y, 0, 0);
  Check(hOk && a.h != b.h && a.h == Shadow::HashAll(a.st),
        "R123: state hash (incremental = full, order free, per-view slot 11 excluded, a CB slot change seen)");
  // The split filter's shadow feeds the ops it filters (its sites bypass every table).
  {
    Win& w = g_win;
    const sfilt::Shadow saved = sfilt::g_sh;
    w.sh = Shadow{};
    w.sfLive = true;
    for (bool& s : w.sfOp) s = false;
    w.sfOp[sfilt::kVS] = w.sfOp[sfilt::kVSCB] = true;
    sfilt::g_sh.ForgetAll();
    sfilt::g_sh.vs = reinterpret_cast<void*>(0x77);
    sfilt::g_sh.vsCb[3] = y;
    sfilt::g_sh.ps = reinterpret_cast<void*>(0x88);  // not an op of this window: ignored
    SyncSfilt();
    const bool sOk = w.sh.st.vs == reinterpret_cast<void*>(0x77) && w.sh.st.cb[0][3] == y && w.sh.st.ps == nullptr &&
                     w.sh.st.cb[0][2] == nullptr && (w.sh.dirty.g & gVS) && w.sh.dirty.cb[0].Any();
    sfilt::g_sh = saved;
    w.sfLive = false;
    for (bool& s : w.sfOp) s = false;
    w.sh = Shadow{};
    Check(sOk, "R123: split filter's shadow supplies its filtered ops (known entries only; others untouched)");
  }
  // Statistics text: present and naming every part the live plan reads.
  ftwin::Stats s;
  s.focusPasses = 100, s.why[kWTwin] = 80, s.why[kWMismatch] = 3, s.twinItems = 8000, s.focusItems = 9000;
  s.focusStockItems = 1000, s.focusStockItemUs = 700, s.periphItems = 20000, s.periphDcsItemUs = 16000;
  s.periphOwnUs = 2000, s.focusOwnUs = 1500, s.gpuTwinN = 10, s.gpuTwinUs = 6000, s.gpuStockN = 5, s.gpuStockUs = 2900;
  const std::string txt = StatsText(s, 30, 50);
  Check(txt.find("focus twin: 30 s, 100 focus passes: twin 80 (80 %)") == 0 && txt.find("prediction miss 3") != std::string::npos &&
            txt.find("saved ~112 us") != std::string::npos && txt.find("net ") != std::string::npos &&
            txt.find("GPU focus terrain: twin 600 us, stock 580 us (+20 us)") != std::string::npos,
        "R123: the 30 s statistics line (outcomes, saved / added / net render-thread us, GPU twin vs stock)");
}

// ---------------------------------------------------------------------------
// WARP pipeline with a DCS-like fake
// ---------------------------------------------------------------------------
constexpr int kPerW = 64, kPerH = 64, kFocW = 96, kFocH = 96, kMfdW = 48, kMfdH = 48;
constexpr int kGridX = 14, kGridZ = 12;  // patches
constexpr int kPatches = kGridX * kGridZ;
constexpr float kPatchSize = 6.0f;

struct View {
  int w = 0, h = 0;
  ID3D11Texture2D* rt[2] = {};
  ID3D11RenderTargetView* rtv[2] = {};
  ID3D11Texture2D* depth = nullptr;
  ID3D11DepthStencilView* dsv = nullptr;
  ID3D11Texture2D* stage[3] = {};
};

struct Rig {
  ID3D11Device* dev = nullptr;
  ID3D11DeviceContext* ctx = nullptr;
  ID3D11VertexShader* vs[2] = {};
  ID3D11PixelShader* ps[2] = {};
  ID3D11InputLayout* il = nullptr;
  ID3D11VertexShader* vsInst = nullptr;
  ID3D11PixelShader* psInst = nullptr;
  ID3D11InputLayout* ilInst = nullptr;
  ID3D11Buffer* vbPos = nullptr;
  ID3D11Buffer* vbCol[3] = {};
  ID3D11Buffer* ib = nullptr;
  ID3D11Buffer* cbDraw[2] = {};  // per-draw (DCS's R34 / R1295)
  ID3D11Buffer* cbExtra = nullptr;  // b2: uploaded by some items only (inheritance)
  ID3D11Buffer* cbInst = nullptr;
  ID3D11Buffer* cbMat = nullptr;    // b1 pass level
  ID3D11Buffer* cbFrame = nullptr;  // b6
  ID3D11Buffer* b5 = nullptr;
  ID3D11Buffer* b7[2] = {};
  ID3D11Buffer* b10 = nullptr;
  ID3D11Buffer* b11 = nullptr;
  ID3D11ShaderResourceView* tex[2] = {};
  ID3D11SamplerState* smp = nullptr;
  ID3D11RasterizerState* rs = nullptr;
  ID3D11DepthStencilState* dss = nullptr;
  ID3D11BlendState* bs = nullptr;
  View view[5];  // periph L, periph R, focus L, focus R, MFD
  // R125 instancer
  ID3D11VertexShader* vsI2 = nullptr;
  ID3D11InputLayout* ilI2 = nullptr;
  ID3D11PixelShader* psI2[2] = {};
  ID3D11Buffer* cbI2 = nullptr;
  ID3D11Buffer* instBuf = nullptr;
  ID3D11ShaderResourceView* instSrv = nullptr;
  ID3D11Buffer* argsBuf[2] = {};  // DCS's IM2 argument buffer (the second: "recreated")
  bool ok = false;
};
Rig g_rig;

template <class T>
void Rel(T*& p) {
  if (p) p->Release();
  p = nullptr;
}

const char kShader[] =
    "cbuffer Draw : register(b0) { float4 gOff; float4 gTint; };\n"
    "cbuffer Mat : register(b1) { float4 gMat; };\n"
    "cbuffer Extra : register(b2) { float4 gExtra; };\n"
    "cbuffer B5 : register(b5) { float4 b5r[19]; };\n"
    "cbuffer PerFrame : register(b6) { float4 gFrame; };\n"
    "cbuffer PerView : register(b7) { float4 v7[151]; };\n"
    "cbuffer B10 : register(b10) { float4 b10r[80]; };\n"
    "cbuffer TC : register(b11) { float4 tc[136]; };\n"
    "Texture2D gTex : register(t0); SamplerState gSmp : register(s0);\n"
    "struct VI { float3 pos : POSITION; float4 col : COLOR; };\n"
    "struct VO { float4 sv : SV_Position; float4 col : COLOR; float2 uv : TEXCOORD; };\n"
    "VO vs_main(VI i) { float4 p = float4(i.pos * gOff.w + gOff.xyz, 1); VO o;\n"
    "  o.sv = float4(dot(p, tc[20]), dot(p, tc[21]), dot(p, tc[22]), dot(p, tc[23]));\n"
    "  o.col = i.col * gTint + gMat + gExtra; o.uv = i.pos.xz * 0.37 + gOff.xz * 0.01; return o; }\n"
    "VO vs_alt(VI i) { VO o = vs_main(i); o.col = o.col.bgra; return o; }\n"
    "struct PO { float4 c0 : SV_Target0; float4 c1 : SV_Target1; };\n"
    "PO ps_main(VO i) { PO o; o.c0 = saturate(i.col * gTex.Sample(gSmp, i.uv) + gFrame * 0.01);\n"
    "  o.c1 = float4(v7[16].z, b10r[4].x, b5r[0].x, i.sv.z); return o; }\n"
    "PO ps_alt(VO i) { PO o = ps_main(i); o.c0 = 1 - o.c0; return o; }\n"
    "cbuffer Inst : register(b0) { float4 gIOff; };\n"
    "struct VJ { float3 pos : POSITION; uint iid : SV_InstanceID; };\n"
    "struct VP2 { float4 sv : SV_Position; float4 col : COLOR; };\n"
    "VP2 vs_inst(VJ i) { float4 p = float4(i.pos * 1.5 + gIOff.xyz + float3(i.iid * 2.0, 0.6, 0), 1); VP2 o;\n"
    "  o.sv = float4(dot(p, tc[20]), dot(p, tc[21]), dot(p, tc[22]), dot(p, tc[23]));\n"
    "  o.col = float4(0.2, 0.9, 0.3, 1) * gIOff.w; return o; }\n"
    "PO ps_inst(VP2 i) { PO o; o.c0 = i.col; o.c1 = float4(v7[16].z, 0, 0, i.sv.z); return o; }\n"
    // R125: MetaMaterial_Instancer2-like draws: the instances from the shared, per view compacted instance buffer
    // (per-instance stream, offset by the argument words' StartInstanceLocation, as the chain's compaction wrote them)
    "cbuffer I2 : register(b0) { float4 gI2Tint; uint4 gI2Base; };\n"
    "cbuffer TC3 : register(b3) { float4 tc3[136]; };\n"
    "struct VI2 { float3 pos : POSITION; float4 ip : IPOS; };\n"
    "VP2 vs_i2(VI2 i) { float4 p = float4(i.pos * 1.2 * gI2Tint.w + i.ip.xyz, 1); VP2 o;\n"
    "  o.sv = float4(dot(p, tc[20]), dot(p, tc[21]), dot(p, tc[22]), dot(p, tc[23]));\n"
    "  o.col = gI2Tint * (0.5 + 0.5 * i.ip.w) * (1.0 + 1e-4 * gI2Base.x); return o; }\n"
    "PO ps_i2(VP2 i) { PO o; o.c0 = saturate(i.col * gTex.Load(int3(0, 0, 0))); o.c1 = float4(v7[16].z, b10r[4].x, 0, i.sv.z); return o; }\n"
    "PO ps_i2tc(VP2 i) { PO o = ps_i2(i); o.c1.z = tc3[44].z * 0.001; return o; }\n";

ID3DBlob* Compile(decltype(&D3DCompile) compile, const char* entry, const char* target) {
  ID3DBlob* blob = nullptr;
  ID3DBlob* err = nullptr;
  compile(kShader, sizeof(kShader) - 1, "r123", nullptr, nullptr, entry, target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
          &blob, &err);
  if (err) {
    if (!blob) printf("     (R123 shader %s: %s)\n", entry, static_cast<const char*>(err->GetBufferPointer()));
    err->Release();
  }
  return blob;
}
ID3D11Buffer* MakeBuf(UINT bytes, UINT bind, const void* data = nullptr) {
  D3D11_BUFFER_DESC d{};
  d.ByteWidth = bytes;
  d.Usage = D3D11_USAGE_DEFAULT;
  d.BindFlags = bind;
  D3D11_SUBRESOURCE_DATA sd{data, 0, 0};
  ID3D11Buffer* b = nullptr;
  g_rig.dev->CreateBuffer(&d, data ? &sd : nullptr, &b);
  return b;
}
bool MakeView(View& v, int w, int h) {
  v.w = w, v.h = h;
  D3D11_TEXTURE2D_DESC td{};
  td.Width = static_cast<UINT>(w), td.Height = static_cast<UINT>(h), td.MipLevels = 1, td.ArraySize = 1;
  td.SampleDesc.Count = 1;
  td.Usage = D3D11_USAGE_DEFAULT;
  const DXGI_FORMAT fmt[2] = {DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R32G32B32A32_FLOAT};
  for (int i = 0; i < 2; ++i) {
    td.Format = fmt[i];
    td.BindFlags = D3D11_BIND_RENDER_TARGET;
    if (FAILED(g_rig.dev->CreateTexture2D(&td, nullptr, &v.rt[i])) ||
        FAILED(g_rig.dev->CreateRenderTargetView(v.rt[i], nullptr, &v.rtv[i])))
      return false;
    D3D11_TEXTURE2D_DESC sd = td;
    sd.Usage = D3D11_USAGE_STAGING, sd.BindFlags = 0, sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (FAILED(g_rig.dev->CreateTexture2D(&sd, nullptr, &v.stage[i]))) return false;
  }
  td.Format = DXGI_FORMAT_R32_TYPELESS;
  td.BindFlags = D3D11_BIND_DEPTH_STENCIL;
  if (FAILED(g_rig.dev->CreateTexture2D(&td, nullptr, &v.depth))) return false;
  D3D11_DEPTH_STENCIL_VIEW_DESC dd{};
  dd.Format = DXGI_FORMAT_D32_FLOAT;
  dd.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
  if (FAILED(g_rig.dev->CreateDepthStencilView(v.depth, &dd, &v.dsv))) return false;
  D3D11_TEXTURE2D_DESC sd = td;
  sd.Usage = D3D11_USAGE_STAGING, sd.BindFlags = 0, sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  return SUCCEEDED(g_rig.dev->CreateTexture2D(&sd, nullptr, &v.stage[2]));
}
bool MakeRig() {
  if (g_rig.ok) return true;
  D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_1;
  if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, &fl, 1, D3D11_SDK_VERSION, &g_rig.dev, nullptr,
                               &g_rig.ctx)))
    return false;
  HMODULE dll = LoadLibraryW(L"d3dcompiler_47.dll");
  auto compile = dll ? reinterpret_cast<decltype(&D3DCompile)>(GetProcAddress(dll, "D3DCompile")) : nullptr;
  if (!compile) return false;
  ID3DBlob* v0 = Compile(compile, "vs_main", "vs_5_0");
  ID3DBlob* v1 = Compile(compile, "vs_alt", "vs_5_0");
  ID3DBlob* p0 = Compile(compile, "ps_main", "ps_5_0");
  ID3DBlob* p1 = Compile(compile, "ps_alt", "ps_5_0");
  ID3DBlob* vi = Compile(compile, "vs_inst", "vs_5_0");
  ID3DBlob* pi = Compile(compile, "ps_inst", "ps_5_0");
  ID3DBlob* vi2 = Compile(compile, "vs_i2", "vs_5_0");
  ID3DBlob* pi2 = Compile(compile, "ps_i2", "ps_5_0");
  ID3DBlob* pi2t = Compile(compile, "ps_i2tc", "ps_5_0");
  bool ok = v0 && v1 && p0 && p1 && vi && pi && vi2 && pi2 && pi2t;
  if (ok) {
    ID3D11Device* d = g_rig.dev;
    ok = SUCCEEDED(d->CreateVertexShader(v0->GetBufferPointer(), v0->GetBufferSize(), nullptr, &g_rig.vs[0])) &&
         SUCCEEDED(d->CreateVertexShader(v1->GetBufferPointer(), v1->GetBufferSize(), nullptr, &g_rig.vs[1])) &&
         SUCCEEDED(d->CreatePixelShader(p0->GetBufferPointer(), p0->GetBufferSize(), nullptr, &g_rig.ps[0])) &&
         SUCCEEDED(d->CreatePixelShader(p1->GetBufferPointer(), p1->GetBufferSize(), nullptr, &g_rig.ps[1])) &&
         SUCCEEDED(d->CreateVertexShader(vi->GetBufferPointer(), vi->GetBufferSize(), nullptr, &g_rig.vsInst)) &&
         SUCCEEDED(d->CreatePixelShader(pi->GetBufferPointer(), pi->GetBufferSize(), nullptr, &g_rig.psInst)) &&
         SUCCEEDED(d->CreateVertexShader(vi2->GetBufferPointer(), vi2->GetBufferSize(), nullptr, &g_rig.vsI2)) &&
         SUCCEEDED(d->CreatePixelShader(pi2->GetBufferPointer(), pi2->GetBufferSize(), nullptr, &g_rig.psI2[0])) &&
         SUCCEEDED(d->CreatePixelShader(pi2t->GetBufferPointer(), pi2t->GetBufferSize(), nullptr, &g_rig.psI2[1]));
    // vertex stream 0: colour (per patch / per view), stream 1: positions (shared) -- as DCS's vb0 / vb1
    const D3D11_INPUT_ELEMENT_DESC ie[2] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 1, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0}};
    ok = ok && SUCCEEDED(d->CreateInputLayout(ie, 2, v0->GetBufferPointer(), v0->GetBufferSize(), &g_rig.il));
    const D3D11_INPUT_ELEMENT_DESC ij{"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 1, 0, D3D11_INPUT_PER_VERTEX_DATA, 0};
    ok = ok && SUCCEEDED(d->CreateInputLayout(&ij, 1, vi->GetBufferPointer(), vi->GetBufferSize(), &g_rig.ilInst));
    const D3D11_INPUT_ELEMENT_DESC i2[2] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 1, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"IPOS", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 2, 0, D3D11_INPUT_PER_INSTANCE_DATA, 1}};
    ok = ok && SUCCEEDED(d->CreateInputLayout(i2, 2, vi2->GetBufferPointer(), vi2->GetBufferSize(), &g_rig.ilI2));
  }
  for (ID3DBlob* b : {v0, v1, p0, p1, vi, pi, vi2, pi2, pi2t})
    if (b) b->Release();
  if (!ok) return false;
  // A 4 x 4 quad grid patch (25 vertices, 96 indices).
  float pos[25][3];
  float col[3][25][4];
  for (int z = 0; z < 5; ++z)
    for (int x = 0; x < 5; ++x) {
      const int i = z * 5 + x;
      pos[i][0] = x / 4.0f, pos[i][1] = 0.05f * static_cast<float>((x * 7 + z * 3) % 5), pos[i][2] = z / 4.0f;
      for (int k = 0; k < 3; ++k) {
        col[k][i][0] = 0.3f + 0.1f * static_cast<float>(x) + 0.2f * static_cast<float>(k);
        col[k][i][1] = 0.2f + 0.15f * static_cast<float>(z);
        col[k][i][2] = 0.5f - 0.1f * static_cast<float>(k);
        col[k][i][3] = 1.0f;
      }
    }
  uint16_t idx[96];
  int n = 0;
  for (int z = 0; z < 4; ++z)
    for (int x = 0; x < 4; ++x) {
      const uint16_t a = static_cast<uint16_t>(z * 5 + x);
      const uint16_t b = static_cast<uint16_t>(a + 1), c = static_cast<uint16_t>(a + 5), d = static_cast<uint16_t>(a + 6);
      idx[n++] = a, idx[n++] = c, idx[n++] = b, idx[n++] = b, idx[n++] = c, idx[n++] = d;
    }
  g_rig.vbPos = MakeBuf(sizeof(pos), D3D11_BIND_VERTEX_BUFFER, pos);
  for (int k = 0; k < 3; ++k) g_rig.vbCol[k] = MakeBuf(sizeof(col[k]), D3D11_BIND_VERTEX_BUFFER, col[k]);
  g_rig.ib = MakeBuf(sizeof(idx), D3D11_BIND_INDEX_BUFFER, idx);
  for (auto*& b : g_rig.cbDraw) b = MakeBuf(176, D3D11_BIND_CONSTANT_BUFFER);
  g_rig.cbExtra = MakeBuf(64, D3D11_BIND_CONSTANT_BUFFER);
  g_rig.cbInst = MakeBuf(64, D3D11_BIND_CONSTANT_BUFFER);
  g_rig.cbMat = MakeBuf(64, D3D11_BIND_CONSTANT_BUFFER);
  g_rig.cbFrame = MakeBuf(608, D3D11_BIND_CONSTANT_BUFFER);
  g_rig.b5 = MakeBuf(304, D3D11_BIND_CONSTANT_BUFFER);
  for (auto*& b : g_rig.b7) b = MakeBuf(2416, D3D11_BIND_CONSTANT_BUFFER);
  g_rig.b10 = MakeBuf(1280, D3D11_BIND_CONSTANT_BUFFER);
  g_rig.b11 = MakeBuf(2176, D3D11_BIND_CONSTANT_BUFFER);
  // R125: the instancer's shared buffers (rewritten by every view's chain)
  g_rig.cbI2 = MakeBuf(32, D3D11_BIND_CONSTANT_BUFFER);
  g_rig.instBuf = MakeBuf(4096 * 16, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_VERTEX_BUFFER);
  {
    D3D11_SHADER_RESOURCE_VIEW_DESC vd{};
    vd.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    vd.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
    vd.Buffer.NumElements = 4096;
    if (!g_rig.instBuf || FAILED(g_rig.dev->CreateShaderResourceView(g_rig.instBuf, &vd, &g_rig.instSrv))) return false;
    for (auto*& b : g_rig.argsBuf) {
      D3D11_BUFFER_DESC d{};
      d.ByteWidth = 4096, d.Usage = D3D11_USAGE_DEFAULT, d.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
      d.MiscFlags = D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS;
      if (FAILED(g_rig.dev->CreateBuffer(&d, nullptr, &b))) return false;
    }
  }
  for (int k = 0; k < 2; ++k) {
    uint32_t px[8 * 8];
    for (int i = 0; i < 64; ++i) px[i] = 0xff000000u | (static_cast<uint32_t>(i * 37 + k * 90) & 0xffu) << 8 | static_cast<uint32_t>(200 - i * k);
    D3D11_TEXTURE2D_DESC td{};
    td.Width = td.Height = 8, td.MipLevels = 1, td.ArraySize = 1, td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1, td.Usage = D3D11_USAGE_DEFAULT, td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA sd{px, 32, 0};
    ID3D11Texture2D* t = nullptr;
    if (FAILED(g_rig.dev->CreateTexture2D(&td, &sd, &t)) || FAILED(g_rig.dev->CreateShaderResourceView(t, nullptr, &g_rig.tex[k])))
      return false;
    t->Release();
  }
  D3D11_SAMPLER_DESC sm{};
  sm.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
  sm.AddressU = sm.AddressV = sm.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
  sm.MaxLOD = D3D11_FLOAT32_MAX;
  g_rig.dev->CreateSamplerState(&sm, &g_rig.smp);
  D3D11_RASTERIZER_DESC rd{};
  rd.FillMode = D3D11_FILL_SOLID, rd.CullMode = D3D11_CULL_NONE, rd.DepthClipEnable = TRUE;
  g_rig.dev->CreateRasterizerState(&rd, &g_rig.rs);
  D3D11_DEPTH_STENCIL_DESC ds{};
  ds.DepthEnable = TRUE, ds.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL, ds.DepthFunc = D3D11_COMPARISON_GREATER_EQUAL;
  g_rig.dev->CreateDepthStencilState(&ds, &g_rig.dss);
  D3D11_BLEND_DESC bd{};
  bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
  bd.RenderTarget[1].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
  bd.IndependentBlendEnable = TRUE;
  g_rig.dev->CreateBlendState(&bd, &g_rig.bs);
  const int sizes[5][2] = {{kPerW, kPerH}, {kPerW, kPerH}, {kFocW, kFocH}, {kFocW, kFocH}, {kMfdW, kMfdH}};
  for (int v = 0; v < 5; ++v)
    if (!MakeView(g_rig.view[v], sizes[v][0], sizes[v][1])) return false;
  g_rig.ok = g_rig.vbPos && g_rig.ib && g_rig.b11 && g_rig.smp && g_rig.rs && g_rig.dss && g_rig.bs;
  return g_rig.ok;
}

// ---- the fake DCS objects ---------------------------------------------------
// RenderItem as DCS lays it out (ftwin::ReadDescRaw): +0x08 material, +0x24, +0x48, +0x50, +0x58, +0xa8 the
// MetaShaderRenderItem {p0 -> geometry, ints, params vector}, +0xd8. Test data from +0xe0 (outside the descriptor).
struct Geo {  // p0: +8 / +0x10 stream vector of {object, handle}; object + 8 = the vertex buffer
  uint8_t raw[0x60];
  uint8_t streams[2][16];
  uint8_t objs[2][16];
};
struct FakeItem {
  uint8_t raw[0x100];
  Geo geo;
  float param[2];
};
struct PatchData {  // at FakeItem +0xe0
  int id;
  int shader;   // 0 / 1
  int colSet;   // vb0 variant
  int extra;    // 0 none, 1 uploads b2, 2 binds b2 without uploading (inherits)
  int tex;
  int focusSrvSwap;  // the audit-failure case: draws another texture in a focus view
};
void InitItem(FakeItem& it, void* mat, const PatchData& pd) {
  memset(&it, 0, sizeof(it));
  *reinterpret_cast<void**>(it.raw + 0x08) = mat;
  *reinterpret_cast<int*>(it.raw + 0x24) = pd.id;
  it.param[0] = 1.5f, it.param[1] = static_cast<float>(pd.id);
  *reinterpret_cast<float**>(it.raw + 0x50) = it.param;
  *reinterpret_cast<int*>(it.raw + 0x58) = pd.shader;
  *reinterpret_cast<Geo**>(it.raw + 0xa8) = &it.geo;
  *reinterpret_cast<int*>(it.raw + 0xd8) = 1;
  memcpy(it.raw + 0xe0, &pd, sizeof(pd));
  for (int s = 0; s < 2; ++s) {
    *reinterpret_cast<ID3D11Buffer**>(it.geo.objs[s] + 8) = s == 0 ? g_rig.vbCol[pd.colSet] : g_rig.vbPos;
    *reinterpret_cast<void**>(it.geo.streams[s]) = it.geo.objs[s];
    *reinterpret_cast<uint64_t*>(it.geo.streams[s] + 8) = static_cast<uint64_t>(s + 1);
  }
  *reinterpret_cast<void**>(it.geo.raw + 8) = it.geo.streams[0];
  *reinterpret_cast<void**>(it.geo.raw + 0x10) = it.geo.streams[0] + 32;
}
inline const PatchData& PD(void* item) { return *reinterpret_cast<const PatchData*>(static_cast<uint8_t*>(item) + 0xe0); }

// DCS's DX11StateManager: a bind reaches d3d11 only when it differs from what the manager last set.
struct Cache {
  void* vs = nullptr;
  void* ps = nullptr;
  void* il = nullptr;
  ID3D11Buffer* vb0 = nullptr;
  ID3D11Buffer* vb1 = nullptr;
  ID3D11Buffer* ib = nullptr;
  ID3D11Buffer* vcb[14] = {};
  ID3D11Buffer* pcb[14] = {};
  ID3D11ShaderResourceView* srv = nullptr;
  int topo = -1;
  void Clear() { *this = Cache(); }
};
struct RCtx {  // DCS's render context (the ctx argument of both hooks); slot 5 writes per-entry data into it
  uint8_t dcs[0x400];  // +0x98 world matrix (camera relative), +0x358 height range, +0x378 entry+0x60
  ID3D11DeviceContext* c;
  int view;  // 0..3 quad views, 4 MFD
  Cache cache;
  int frame;
  double eye[3];
};
inline void SetCb(RCtx& r, UINT slot, ID3D11Buffer* b) {
  if (r.cache.vcb[slot] != b) r.c->VSSetConstantBuffers(slot, 1, &b), r.cache.vcb[slot] = b;
  if (r.cache.pcb[slot] != b) r.c->PSSetConstantBuffers(slot, 1, &b), r.cache.pcb[slot] = b;
}

uint64_t __fastcall FakeMatRender(void* mat, void* item, void* ctx, void* x);
uint64_t __fastcall FakeTerrRender(void* self, void* ctx);
uint64_t __fastcall FakeDtor(void*, void*) { return 0; }
void* g_matVt[12];
void* g_terrVt[4];
void* g_instVt[12];
struct FakeMat {
  void** vt;
  int shader;
};
FakeMat g_mat[2] = {{g_matVt, 0}, {g_matVt, 1}};
FakeMat g_instMat{g_instVt, 9};

// ---- R125: MetaMaterial_Instancer2 / InstanceManager2 as DCS lays them out -------------------------------
constexpr int kI2Items = 48;   // one type per item
constexpr int kI2PerSub = 12;  // world instances per (type, sub-draw)
constexpr int kI2PvType = 5;   // the type whose per-draw int differs per view (scenario)
struct FakeI2Mat {  // +0 vtable, +8 + 8 v the technique per view index (-1: nothing)
  void** vt;
  uint64_t tech[4];
  int variant;  // 1: binds the TerrainContext buffer at b3 too
};
void* g_i2Vt[12];
FakeI2Mat g_i2Mat[2] = {{g_i2Vt, {7, 7, 7, 7}, 0}, {g_i2Vt, {9, 9, 9, 9}, 1}};
struct I2Data {  // at FakeI2Item + 0x180 (outside the descriptor)
  int id;
  int type;
  int focusSrvSwap;  // the audit-failure case: another texture in a focus view
  float tint[4];
};
struct FakeI2Item {
  uint8_t raw[0x200];  // +0xa8 MetaShaderRenderItem {geometry, ints +0xb0.., params}, +0x108 type per view index
  Geo geo;
};
inline I2Data& ID2(void* item) { return *reinterpret_cast<I2Data*>(static_cast<uint8_t*>(item) + 0x180); }
struct FakeIm2 {
  uint8_t raw[0x400];  // +0 vtable, +0xe0 type entries, +0x210 -> -> bits, +0x2f0..+0x370 buffer members
};
FakeIm2 g_im2;
void* g_im2Vt[64];
uint8_t g_i2Types[kI2Items * 3 * 16];
uint32_t g_i2Bits[4];
uint32_t* g_i2BitsP = g_i2Bits;
uint8_t g_i2Members[8][16];  // the IM2 buffer objects ([6], [7]: the argument buffer's, before / after "recreation")
uint8_t g_i2X[0x100];        // G->vt[13]()'s object (+0xc8: sub-draw 1's buffer)
void* g_gVt[16];
struct FakeG {
  void** vt;
} g_fakeG{g_gVt};
void* g_fakeGlobal = &g_fakeG;  // DCS: edterrainGraphics41 + 0x1a7b48
void* g_ivVt[12];
struct FakeIv {
  void** vt;
} g_iv{g_ivVt};  // the render context's instancer view ([ctx + 0x370])
uint64_t __fastcall FakeGetIm2(void*) { return reinterpret_cast<uint64_t>(&g_im2); }
uint64_t __fastcall FakeGetX(void*) { return reinterpret_cast<uint64_t>(g_i2X); }
uint64_t __fastcall FakeIvIndex(void*) { return 0; }
uint64_t __fastcall FakeIvTech(void*) { return 5; }
uint64_t __fastcall FakeI2Render(void* mat, void* item, void* ctx, void* x);
int g_i2ArgsSel = 0;  // which argument buffer DCS's IM2 uses now
struct I2World {
  float pos[kI2Items][2][kI2PerSub][4];
};
I2World g_i2World;
std::vector<FakeI2Item*> g_i2Items;
bool g_withI2 = false;   // the scene has instancer items
bool g_i2PvInt = false;  // kI2PvType's per-draw int (its instance region) differs per view
int g_i2AuditBad = -1;   // an instancer item drawn with another texture in the focus views
inline int I2Entry(int type, int sub) { return 3 * type + sub; }
inline uint8_t* I2E(int type, int sub) { return g_i2Types + 16 * I2Entry(type, sub); }
inline UINT I2Base(int type, int sub, bool alt) { return static_cast<UINT>((type * 2 + sub) * kI2PerSub + (alt ? 1600 : 0)); }
void FreeI2() {
  for (FakeI2Item* p : g_i2Items) delete p;
  g_i2Items.clear();
}
// The IM2 / global / view-context objects and the items (deterministic: both runs of a scenario see the same).
void BuildI2() {
  FreeI2();
  for (int i = 0; i < 12; ++i) g_i2Vt[i] = reinterpret_cast<void*>(&FakeDtor), g_ivVt[i] = reinterpret_cast<void*>(&FakeDtor);
  g_i2Vt[8] = reinterpret_cast<void*>(&FakeI2Render);
  for (auto& v : g_im2Vt) v = reinterpret_cast<void*>(&FakeDtor);
  for (auto& v : g_gVt) v = reinterpret_cast<void*>(&FakeDtor);
  g_gVt[11] = reinterpret_cast<void*>(&FakeGetIm2);
  g_gVt[13] = reinterpret_cast<void*>(&FakeGetX);
  g_ivVt[0] = reinterpret_cast<void*>(&FakeIvIndex);
  g_ivVt[8] = reinterpret_cast<void*>(&FakeIvTech);
  memset(&g_im2, 0, sizeof(g_im2));
  memset(g_i2Types, 0, sizeof(g_i2Types));
  memset(g_i2X, 0, sizeof(g_i2X));
  *reinterpret_cast<void**>(g_im2.raw) = g_im2Vt;
  *reinterpret_cast<void**>(g_im2.raw + 0xe0) = g_i2Types;
  *reinterpret_cast<uint32_t***>(g_im2.raw + 0x210) = &g_i2BitsP;
  const uint32_t mem[6] = {0x2f0, 0x320, 0x330, 0x340, 0x360, 0x370};
  for (int k = 0; k < 6; ++k) *reinterpret_cast<void**>(g_im2.raw + mem[k]) = g_i2Members[k];
  *reinterpret_cast<void**>(g_i2X + 0xc8) = g_i2Members[7];
  g_i2ArgsSel = 0;
  for (int t = 0; t < kI2Items; ++t)
    for (int sub = 0; sub < 2; ++sub) {
      uint8_t* e = I2E(t, sub);
      *reinterpret_cast<int32_t*>(e) = t * 2 + sub;                               // bit index
      *reinterpret_cast<int32_t*>(e + 8) = static_cast<int32_t>(I2Base(t, sub, false));  // the per-draw int
      *reinterpret_cast<uint32_t*>(e + 0xc) = 64u + 20u * static_cast<uint32_t>(I2Entry(t, sub));  // argument offset
      // a cluster per (type, sub-draw) (a forest block): some are in a periphery view and not in its focus
      const uint32_t hc = static_cast<uint32_t>((t * 977 + sub * 131 + 17) * 2654435761u);
      const float cx = static_cast<float>(hc % 8000u) * 0.01f + 2.0f, cz = static_cast<float>((hc >> 13) % 6800u) * 0.01f + 2.0f;
      for (int k = 0; k < kI2PerSub; ++k) {
        float* p = g_i2World.pos[t][sub][k];
        const uint32_t hsh = static_cast<uint32_t>((t * 977 + sub * 131 + k * 37) * 2654435761u);
        p[0] = cx + static_cast<float>(hsh % 500u) * 0.01f - 2.5f;
        p[1] = 0.4f + 0.1f * static_cast<float>(sub);
        p[2] = cz + static_cast<float>((hsh >> 13) % 500u) * 0.01f - 2.5f;
        p[3] = static_cast<float>(k) / kI2PerSub;
      }
    }
  for (int i = 0; i < kI2Items; ++i) {
    FakeI2Item* it = new FakeI2Item();
    memset(it, 0, sizeof(*it));
    *reinterpret_cast<Geo**>(it->raw + 0xa8) = &it->geo;
    *reinterpret_cast<int32_t*>(it->raw + 0xb0) = 24;      // the index count per instance (DCS: +0xb0 to drawIndirect)
    *reinterpret_cast<int32_t*>(it->raw + 0xb4) = 100 + i;  // per item
    *reinterpret_cast<int32_t*>(it->raw + 0xb8) = 1;
    for (int v = 0; v < 4; ++v) *reinterpret_cast<int32_t*>(it->raw + 0x108 + 4 * v) = i;  // type per view index
    *reinterpret_cast<ID3D11Buffer**>(it->geo.objs[0] + 8) = g_rig.vbPos;
    *reinterpret_cast<void**>(it->geo.streams[0]) = it->geo.objs[0];
    *reinterpret_cast<uint64_t*>(it->geo.streams[0] + 8) = 1;
    *reinterpret_cast<void**>(it->geo.raw + 8) = it->geo.streams[0];
    *reinterpret_cast<void**>(it->geo.raw + 0x10) = it->geo.streams[0] + 16;
    I2Data& d = ID2(it);
    d.id = i, d.type = i, d.focusSrvSwap = (i == g_i2AuditBad) ? 1 : 0;
    d.tint[0] = 0.3f + 0.02f * static_cast<float>(i), d.tint[1] = 0.8f - 0.01f * static_cast<float>(i), d.tint[2] = 0.25f, d.tint[3] = 0.7f + 0.06f * static_cast<float>(i % 9);  // .w: the mesh scale per type
    g_i2Items.push_back(it);
  }
}
inline bool I2Bit(int type, int sub) {
  const int32_t i = *reinterpret_cast<const int32_t*>(I2E(type, sub));
  return (g_i2Bits[i >> 5] >> (i & 31)) & 1u;
}

// The scene: patches on the ground, a few "instancer" objects between the runs.
struct Entry {
  FakeMat* mat;
  FakeItem* item;  // null for an instancer entry
  int patch;
  float ix, iz;    // instancer position
  FakeI2Item* i2 = nullptr;  // R125: a MetaMaterial_Instancer2 item (mat unused)
  FakeI2Mat* i2mat = nullptr;
};
struct Scene {
  std::vector<FakeItem*> items;     // shared items (one per patch)
  std::vector<FakeItem*> copies;    // per-view copies of them (the same content and geometry) for the focus lists
  std::vector<FakeItem*> perView;   // the focus versions of the per-view patches (own geometry)
  std::vector<int> perViewOf;       // patch -> index in perView (-1)
  int generation = 0;
};
Scene g_scene;
bool g_focusCopies = false;  // the focus lists hold per-view copies of the RenderItems (content-equal)
void FreeScene() {
  for (FakeItem* p : g_scene.copies) delete p;
  g_scene.copies.clear();
  for (FakeItem* p : g_scene.items) delete p;
  for (FakeItem* p : g_scene.perView) delete p;
  g_scene.items.clear(), g_scene.perView.clear(), g_scene.perViewOf.clear();
}
// Patch kinds: every 7th per-view (its own vb0 in the focus), 5 / 6 mod 11 the b2 upload / inheritance pair, 1 in 9
// the second shader.
void BuildScene(int gen, int auditBadPatch) {
  FreeScene();
  g_scene.generation = gen;
  g_scene.perViewOf.assign(kPatches, -1);
  for (int p = 0; p < kPatches; ++p) {
    PatchData pd{};
    pd.id = p + 1000 * gen;
    pd.shader = (p % 9 == 4) ? 1 : 0;
    pd.colSet = 0;
    pd.extra = (p % 11 == 5) ? 1 : (p % 11 == 6) ? 2 : 0;
    pd.tex = (p % 3 == 0) ? 1 : 0;
    pd.focusSrvSwap = (auditBadPatch >= 0 && p % 5 == 1) ? 1 : 0;
    FakeItem* it = new FakeItem();
    InitItem(*it, &g_mat[pd.shader], pd);
    g_scene.items.push_back(it);
    if (p % 7 == 3) {
      PatchData pf = pd;
      pf.colSet = 2;
      FakeItem* f = new FakeItem();
      InitItem(*f, &g_mat[pd.shader], pf);
      g_scene.perViewOf[static_cast<size_t>(p)] = static_cast<int>(g_scene.perView.size());
      g_scene.perView.push_back(f);
      // the periphery's version uses its own stream 0 too
      PatchData pp = pd;
      pp.colSet = 1;
      InitItem(*it, &g_mat[pd.shader], pp);
    }
  }
}

// ---- per-view constants -----------------------------------------------------
struct Cam {
  double V[4][4];  // row vectors: v = p * V
  double eye[3];
};
struct ViewP {
  double sx, sy, cx, cy;
  int w, h;
  int type;  // 0 periphery, 1 focus, 2 MFD
};
Cam MakeCam(int frame, int eye, bool mfd) {
  Cam c{};
  const double yaw = 0.15 + 0.004 * frame + (mfd ? 0.9 : 0.0);
  const double pitch = mfd ? -0.6 : -0.35;
  // camera axes: right r, up u, forward f (world y up)
  const double f[3] = {std::sin(yaw) * std::cos(pitch), std::sin(pitch), std::cos(yaw) * std::cos(pitch)};
  const double r[3] = {std::cos(yaw), 0, -std::sin(yaw)};
  const double u[3] = {f[1] * r[2] - f[2] * r[1], f[2] * r[0] - f[0] * r[2], f[0] * r[1] - f[1] * r[0]};
  const double base[3] = {30.0 + 0.05 * frame, 9.0, -12.0};
  const double ipd = mfd ? 0 : (eye == 0 ? -0.032 : 0.032);
  for (int k = 0; k < 3; ++k) c.eye[k] = base[k] + ipd * r[k];
  for (int k = 0; k < 3; ++k) c.V[k][0] = r[k], c.V[k][1] = u[k], c.V[k][2] = f[k], c.V[k][3] = 0;
  for (int j = 0; j < 3; ++j) {
    const double* ax = j == 0 ? r : j == 1 ? u : f;
    c.V[3][j] = -(c.eye[0] * ax[0] + c.eye[1] * ax[1] + c.eye[2] * ax[2]);
  }
  c.V[3][3] = 1;
  return c;
}
// VP row c (coefficients of world x, y, z, 1): clip_c = sum_j v_j P_c[j].
void VpRows(const Cam& c, const double P[4][4], double out[4][4]) {
  for (int row = 0; row < 4; ++row)
    for (int k = 0; k < 4; ++k) {
      double s = 0;
      for (int j = 0; j < 4; ++j) s += c.V[k][j] * P[row][j];
      out[row][k] = s;
    }
}
void PRows(const ViewP& v, double P[4][4]) {
  const double z[4][4] = {{v.sx, 0, v.cx, 0}, {0, v.sy, v.cy, 0}, {0, 0, -1e-7, 0.02}, {0, 0, 1, 0}};
  memcpy(P, z, sizeof(z));
}
void Put(uint8_t* buf, int row, const double* v) { PutRow(buf, row, v[0], v[1], v[2], v[3]); }
void Pattern(uint8_t* buf, int rows, uint32_t seed) {
  for (int r = 0; r < rows; ++r)
    for (int k = 0; k < 4; ++k)
      reinterpret_cast<float*>(buf)[r * 4 + k] = static_cast<float>(((seed * 2654435761u) ^ static_cast<uint32_t>(r * 131 + k * 7)) % 1000) / 997.0f;
}
// The per-view buffers of one view (TerrainContext, b10, b7, b5).
struct ViewConsts {
  uint8_t tc[kTcBytes];
  uint8_t b10[1280];
  uint8_t b7[2416];
  uint8_t b5[304];
};
void MakeConsts(const Cam& cam, const ViewP& v, int eye, int frame, ViewConsts& out) {
  double P[4][4], VP[4][4];
  PRows(v, P);
  VpRows(cam, P, VP);
  Pattern(out.tc, kTcRows, static_cast<uint32_t>(frame * 10 + eye + (v.type == 2 ? 50 : 0)));
  for (int r = 0; r < 4; ++r) Put(out.tc, kRowPx + r, P[r]);
  for (int r = 0; r < 4; ++r) Put(out.tc, kRowVx + r, VP[r]);
  PutRow(out.tc, kRowIx, 1.0 / v.sx, 0, 0, -v.cx / v.sx);
  PutRow(out.tc, kRowIy, 0, 1.0 / v.sy, 0, -v.cy / v.sy);
  for (int i = 0; i < 4; ++i) {
    const double* a = i < 2 ? VP[0] : VP[1];
    const double sgn = (i & 1) ? -1.0 : 1.0;
    double p[4];
    for (int k = 0; k < 4; ++k) p[k] = VP[3][k] + sgn * a[k];
    const double n = std::sqrt(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]);
    PutRow(out.tc, kRowPlane0 + i, p[0] / n, p[1] / n, p[2] / n, p[3] / n);
  }
  PutRow(out.tc, kRowCam, cam.eye[0], cam.eye[1], cam.eye[2], 1);
  PutRow(out.tc, kRowSize, 0, 0, v.w, v.h);
  if (v.type == 1)
    PutRow(out.tc, kRow132, 2.0, 2.0370383, 0, 0);
  else
    PutRow(out.tc, kRow132, 3.6893488e19, 1.8201860, 0, 0);
  Pattern(out.b10, 80, static_cast<uint32_t>(frame * 31 + eye * 3 + v.type));
  Put(out.b10, 4, P[0]);
  Put(out.b10, 5, P[1]);
  Pattern(out.b7, 151, static_cast<uint32_t>(frame * 17 + eye));
  Put(out.b7, 16, P[0]);
  Put(out.b7, 17, P[1]);
  Pattern(out.b5, 19, static_cast<uint32_t>(frame * 5 + eye * 2 + v.type));
  reinterpret_cast<float*>(out.b5)[0] = static_cast<float>(v.type * 10 + eye) * 0.01f;
}
// Collect-time view-projection: jitter-free, camera relative (osg, clip = v * M).
void CiOf(const Cam& cam, const ViewP& v, double jx, double jy, float* m) {
  ViewP u = v;
  u.cx -= jx, u.cy -= jy;
  double P[4][4], VP[4][4];
  PRows(u, P);
  Cam c = cam;
  c.V[3][0] = c.V[3][1] = c.V[3][2] = 0;
  VpRows(c, P, VP);
  for (int k = 0; k < 4; ++k)
    for (int col = 0; col < 4; ++col) m[k * 4 + col] = static_cast<float>(VP[col][k]);
}

// One frame's inputs.
struct Frame {
  int f;
  Cam cam[2], mfdCam;
  ViewP vp[5];
  ViewConsts vc[5];
  CiFrame ci;
  bool publishCi = true;
  bool mfd = false;
  bool swapFocusOrder = false;  // eye 0's focus list: two items swapped
  float ciNudge = 0;            // the published focus camera off by this (prediction mismatch)
  bool argsSwap = false;        // R125: the instancer argument buffer recreated before focus L
  std::vector<Entry> list[5];
};
// Culling: a patch is in a view when its corners project inside (or straddle) the frustum.
bool Visible(const Cam& cam, const ViewP& v, float x0, float z0, float size) {
  double P[4][4], VP[4][4];
  PRows(v, P);
  VpRows(cam, P, VP);
  int out[5] = {0, 0, 0, 0, 0};
  for (int c = 0; c < 4; ++c) {
    const double p[4] = {x0 + ((c & 1) ? size : 0.0), 0.0, z0 + ((c & 2) ? size : 0.0), 1.0};
    double cl[4];
    for (int r = 0; r < 4; ++r) cl[r] = VP[r][0] * p[0] + VP[r][1] * p[1] + VP[r][2] * p[2] + VP[r][3];
    if (cl[3] <= 0.05) {
      ++out[4];
      continue;
    }
    out[0] += cl[0] > cl[3], out[1] += cl[0] < -cl[3], out[2] += cl[1] > cl[3], out[3] += cl[1] < -cl[3];
  }
  for (int i = 0; i < 5; ++i)
    if (out[i] == 4) return false;
  return true;
}
void MakeFrame(int f, Frame& fr) {
  fr.f = f;
  for (int e = 0; e < 2; ++e) fr.cam[e] = MakeCam(f, e, false);
  fr.mfdCam = MakeCam(f, 0, true);
  // DLSS jitter: the same pixel offset in both views of an eye (R53).
  const double jpx = 0.37 * std::sin(1.7 * f + 0.3), jpy = 0.41 * std::cos(2.3 * f + 0.1);
  const double gx = 0.06 * std::sin(0.9 * f), gy = -0.03 + 0.04 * std::cos(1.3 * f);  // eye-tracked gaze (periphery NDC)
  const double ax = 2.9412, ay = 2.9412;
  for (int e = 0; e < 2; ++e) {
    ViewP& p = fr.vp[e];
    p.sx = 0.7807443, p.sy = 0.7906425, p.cx = e == 0 ? 0.195 : -0.192, p.cy = 0.001;
    p.w = kPerW, p.h = kPerH, p.type = 0;
    ViewP& q = fr.vp[2 + e];
    q.sx = ax * p.sx, q.sy = ay * p.sy;
    q.cx = ax * (p.cx - gx - (e == 0 ? 0.16 : -0.16)), q.cy = ay * (p.cy - gy);
    q.w = kFocW, q.h = kFocH, q.type = 1;
    p.cx += 2 * jpx / kPerW, p.cy += 2 * jpy / kPerH;
    q.cx += 2 * jpx / kFocW, q.cy += 2 * jpy / kFocH;
  }
  ViewP& m = fr.vp[4];
  m.sx = 1.3, m.sy = 1.3, m.cx = 0, m.cy = 0, m.w = kMfdW, m.h = kMfdH, m.type = 2;
  for (int v = 0; v < 5; ++v) MakeConsts(v == 4 ? fr.mfdCam : fr.cam[v % 2], fr.vp[v], v == 4 ? 0 : v % 2, f, fr.vc[v]);
  fr.ci = CiFrame();
  fr.ci.pairs = 2;
  for (int e = 0; e < 2; ++e) {
    CiOf(fr.cam[e], fr.vp[e], 2 * jpx / kPerW, 2 * jpy / kPerH, fr.ci.p[e]);
    CiOf(fr.cam[e], fr.vp[2 + e], 2 * jpx / kFocW, 2 * jpy / kFocH, fr.ci.f[e]);
  }
  fr.mfd = (f % 2) == 1;
  // Lists: the same order in every view (DCS's sorted per-material groups), each view its own culled subset; an
  // instancer entry after every 13th patch slot (present in every view: its own per-view chain).
  for (int v = 0; v < 5; ++v) {
    fr.list[v].clear();
    const Cam& cam = v == 4 ? fr.mfdCam : fr.cam[v % 2];
    for (int p = 0; p < kPatches; ++p) {
      if (p % 13 == 12) fr.list[v].push_back(Entry{&g_instMat, nullptr, -1, 10.0f + p * 0.7f, 8.0f + (p % 5)});
      const float x0 = (p % kGridX) * kPatchSize, z0 = (p / kGridX) * kPatchSize;
      if (!Visible(cam, fr.vp[v], x0, z0, kPatchSize)) continue;
      FakeItem* it = g_scene.items[static_cast<size_t>(p)];
      if (v >= 2 && v < 4 && g_scene.perViewOf[static_cast<size_t>(p)] >= 0)
        it = g_scene.perView[static_cast<size_t>(g_scene.perViewOf[static_cast<size_t>(p)])];
      if (g_focusCopies && v >= 2 && v < 4 && it == g_scene.items[static_cast<size_t>(p)]) {
        // DCS's per-view list owning its own RenderItem copies: the same bytes (geometry pointer included)
        FakeItem* cp = new FakeItem(*it);
        g_scene.copies.push_back(cp);
        it = cp;
      }
      fr.list[v].push_back(Entry{static_cast<FakeMat*>(*reinterpret_cast<void**>(it->raw + 8)), it, p, 0, 0});
    }
    // R125: the instancer items, in every view's list (slot 5's vt[6] gate decides per view), after the patches
    if (g_withI2)
      for (int i = 0; i < kI2Items; ++i)
        fr.list[v].push_back(Entry{nullptr, nullptr, -1, 0, 0, g_i2Items[static_cast<size_t>(i)], &g_i2Mat[i % 3 == 1 ? 1 : 0]});
  }
}

// ---- the fake renderer ------------------------------------------------------
int g_curView = 0;
uint64_t __fastcall FakeMatRender(void* mat, void* item, void* ctx, void*) {
  RCtx& r = *static_cast<RCtx*>(ctx);
  const PatchData& pd = PD(item);
  ID3D11DeviceContext* c = r.c;
  const int sh = static_cast<FakeMat*>(mat)->shader;
  if (r.cache.vs != g_rig.vs[sh]) c->VSSetShader(g_rig.vs[sh], nullptr, 0), r.cache.vs = g_rig.vs[sh];
  if (r.cache.ps != g_rig.ps[sh]) c->PSSetShader(g_rig.ps[sh], nullptr, 0), r.cache.ps = g_rig.ps[sh];
  if (r.cache.il != g_rig.il) c->IASetInputLayout(g_rig.il), r.cache.il = g_rig.il;
  if (r.cache.topo != 4) c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST), r.cache.topo = 4;
  // vertex streams from the item's geometry (GeometryBinder)
  const Geo* geo = *reinterpret_cast<Geo* const*>(static_cast<uint8_t*>(item) + 0xa8);
  ID3D11Buffer* s0 = *reinterpret_cast<ID3D11Buffer* const*>(geo->objs[0] + 8);
  ID3D11Buffer* s1 = *reinterpret_cast<ID3D11Buffer* const*>(geo->objs[1] + 8);
  if (r.cache.vb0 != s0) {
    const UINT st = 16, of = 0;
    c->IASetVertexBuffers(0, 1, &s0, &st, &of), r.cache.vb0 = s0;
  }
  if (r.cache.vb1 != s1) {
    const UINT st = 12, of = 0;
    c->IASetVertexBuffers(1, 1, &s1, &st, &of), r.cache.vb1 = s1;
  }
  if (r.cache.ib != g_rig.ib) c->IASetIndexBuffer(g_rig.ib, DXGI_FORMAT_R16_UINT, 0), r.cache.ib = g_rig.ib;
  int tex = pd.tex;
  if (pd.focusSrvSwap && r.view >= 2 && r.view < 4) tex = 1 - tex;  // a per-view input the descriptor cannot see
  ID3D11ShaderResourceView* srv = g_rig.tex[tex];
  if (r.cache.srv != srv) c->PSSetShaderResources(0, 1, &srv), r.cache.srv = srv;
  // FX: the per-draw constant buffer is uploaded at every draw (binders always dirty it)
  const int p = pd.id % 1000;
  float cb[44] = {};
  cb[0] = (p % kGridX) * kPatchSize, cb[1] = 0, cb[2] = (p / kGridX) * kPatchSize, cb[3] = kPatchSize;
  cb[4] = 0.5f + 0.003f * static_cast<float>(p % 50), cb[5] = 0.7f, cb[6] = 0.4f + 0.002f * static_cast<float>(p % 70), cb[7] = 1;
  c->UpdateSubresource(g_rig.cbDraw[sh], 0, nullptr, cb, 0, 0);
  SetCb(r, 0, g_rig.cbDraw[sh]);
  if (pd.extra == 1) {
    const float e[16] = {0.01f * static_cast<float>(p % 9), 0.02f, 0.0f, 0.0f};
    c->UpdateSubresource(g_rig.cbExtra, 0, nullptr, e, 0, 0);
  }
  if (pd.extra) SetCb(r, 2, g_rig.cbExtra);
  c->DrawIndexed(96, 0, 0);
  if (pd.extra) {  // FX unbinds b2 after the technique (so the next patches do not read it)
    ID3D11Buffer* nb = nullptr;
    c->VSSetConstantBuffers(2, 1, &nb), c->PSSetConstantBuffers(2, 1, &nb);
    r.cache.vcb[2] = r.cache.pcb[2] = nullptr;
  }
  return 1;
}
void DrawInstancer(RCtx& r, const Entry& e) {
  ID3D11DeviceContext* c = r.c;
  if (r.cache.vs != g_rig.vsInst) c->VSSetShader(g_rig.vsInst, nullptr, 0), r.cache.vs = g_rig.vsInst;
  if (r.cache.ps != g_rig.psInst) c->PSSetShader(g_rig.psInst, nullptr, 0), r.cache.ps = g_rig.psInst;
  if (r.cache.il != g_rig.ilInst) c->IASetInputLayout(g_rig.ilInst), r.cache.il = g_rig.ilInst;
  if (r.cache.vb1 != g_rig.vbPos) {
    const UINT st = 12, of = 0;
    c->IASetVertexBuffers(1, 1, &g_rig.vbPos, &st, &of), r.cache.vb1 = g_rig.vbPos;
  }
  if (r.cache.ib != g_rig.ib) c->IASetIndexBuffer(g_rig.ib, DXGI_FORMAT_R16_UINT, 0), r.cache.ib = g_rig.ib;
  const float cb[16] = {e.ix, 0, e.iz, 1.0f};
  c->UpdateSubresource(g_rig.cbInst, 0, nullptr, cb, 0, 0);
  SetCb(r, 0, g_rig.cbInst);
  c->DrawIndexedInstanced(24, 3, 0, 0, 0);
}
// R125: MetaMaterial_Instancer2::render as DCS's (0xb4180): binders, IM2 buffer binds, then per sub-draw with the
// view's bit: the per-draw int (IM2 vt[32]) into the per-draw constant buffer, DrawIndexedInstancedIndirect from IM2's
// argument buffer at the entry's offset (vt[33]).
uint64_t __fastcall FakeI2Render(void* mat, void* item, void* ctx, void*) {
  RCtx& r = *static_cast<RCtx*>(ctx);
  FakeI2Mat& m = *static_cast<FakeI2Mat*>(mat);
  void* iv = *reinterpret_cast<void**>(r.dcs + 0x370);
  const int v = static_cast<int>(reinterpret_cast<uint64_t(__fastcall*)(void*)>((*static_cast<void***>(iv))[0])(iv));
  const int t = *reinterpret_cast<const int32_t*>(static_cast<uint8_t*>(item) + 0x108 + 4 * v);
  if (t < 0 || m.tech[v] == ~0ull) return 0;
  ID3D11DeviceContext* c = r.c;
  const I2Data& d = ID2(item);
  if (r.cache.vs != g_rig.vsI2) c->VSSetShader(g_rig.vsI2, nullptr, 0), r.cache.vs = g_rig.vsI2;
  if (r.cache.ps != g_rig.psI2[m.variant]) c->PSSetShader(g_rig.psI2[m.variant], nullptr, 0), r.cache.ps = g_rig.psI2[m.variant];
  if (r.cache.il != g_rig.ilI2) c->IASetInputLayout(g_rig.ilI2), r.cache.il = g_rig.ilI2;
  if (r.cache.topo != 4) c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST), r.cache.topo = 4;
  {  // IM2's instance stream (bound per item)
    const UINT st = 16, of = 0;
    c->IASetVertexBuffers(2, 1, &g_rig.instBuf, &st, &of);
  }
  const Geo* geo = *reinterpret_cast<Geo* const*>(static_cast<uint8_t*>(item) + 0xa8);
  ID3D11Buffer* s0 = *reinterpret_cast<ID3D11Buffer* const*>(geo->objs[0] + 8);
  if (r.cache.vb1 != s0) {
    const UINT st = 12, of = 0;
    c->IASetVertexBuffers(1, 1, &s0, &st, &of), r.cache.vb1 = s0;
  }
  if (r.cache.ib != g_rig.ib) c->IASetIndexBuffer(g_rig.ib, DXGI_FORMAT_R16_UINT, 0), r.cache.ib = g_rig.ib;
  c->VSSetShaderResources(1, 1, &g_rig.instSrv);  // IM2's instance buffer (bound per item)
  ID3D11ShaderResourceView* srv = g_rig.tex[(d.focusSrvSwap && r.view >= 2 && r.view < 4) ? 1 : 0];
  if (r.cache.srv != srv) c->PSSetShaderResources(0, 1, &srv), r.cache.srv = srv;
  if (m.variant) SetCb(r, 3, g_rig.b11);  // DCS's R6 at another slot (census): the TerrainContext buffer itself
  for (int sub = 0; sub < 2; ++sub) {
    if (!I2Bit(t, sub)) continue;
    const uint8_t* e = I2E(t, sub);
    struct {
      float tint[4];
      uint32_t base[4];
    } cb{{d.tint[0], d.tint[1], d.tint[2], d.tint[3]},
         {static_cast<uint32_t>(*reinterpret_cast<const int32_t*>(e + 8)), static_cast<uint32_t>(sub), 0, 0}};
    c->UpdateSubresource(g_rig.cbI2, 0, nullptr, &cb, 0, 0);
    SetCb(r, 0, g_rig.cbI2);
    c->DrawIndexedInstancedIndirect(g_rig.argsBuf[g_i2ArgsSel], *reinterpret_cast<const uint32_t*>(e + 0xc));
  }
  return 1;
}
// R125: one view's culling chain (LightsBuffersPass): the view's CPU bits, then for every (type, sub-draw) with its
// bit set the visible instances into the SHARED instance buffer at the entry's per-draw int and the argument words
// into the SHARED argument buffer; the others are left as an earlier view wrote them.
void RunChain(const Frame& fr, int v) {
  if (!g_withI2) return;
  const Cam& cam = v == 4 ? fr.mfdCam : fr.cam[v % 2];
  double P[4][4], VP[4][4];
  PRows(fr.vp[v], P);
  VpRows(cam, P, VP);
  const bool focus = v == 2 || v == 3;
  ID3D11DeviceContext* c = g_rig.ctx;
  ID3D11Buffer* ab = g_rig.argsBuf[g_i2ArgsSel];
  memset(g_i2Bits, 0, sizeof(g_i2Bits));
  UINT cur = 0;  // the compaction (DCS: instancemanager2.calcoffsets): each view packs ITS visible instances
  for (int t = 0; t < kI2Items; ++t)
    for (int sub = 0; sub < 2; ++sub) {
      uint8_t* e = I2E(t, sub);
      if (g_i2PvInt && t == kI2PvType)  // DCS's IM2 rewrites this type's entry per view (R125 census: 1-2 per pass)
        *reinterpret_cast<int32_t*>(e + 8) = static_cast<int32_t>(I2Base(t, sub, focus));
      float list[kI2PerSub][4];
      UINT n = 0;
      bool coarse = false;
      for (int k = 0; k < kI2PerSub; ++k) {
        const float* p = g_i2World.pos[t][sub][k];
        double cl[4];
        for (int row = 0; row < 4; ++row) cl[row] = VP[row][0] * p[0] + VP[row][1] * p[1] + VP[row][2] * p[2] + VP[row][3];
        if (cl[3] <= 0.05) continue;
        const double ax = std::fabs(cl[0]) / cl[3], ay = std::fabs(cl[1]) / cl[3];
        if (ax <= 1.3 && ay <= 1.3) coarse = true;  // the CPU's block test (coarse)
        if (ax <= 1.05 && ay <= 1.05) memcpy(list[n++], p, 16);  // the GPU's (fine)
      }
      if (!coarse) continue;
      const int32_t bi = *reinterpret_cast<const int32_t*>(e);
      g_i2Bits[bi >> 5] |= 1u << (bi & 31);
      if (n) {
        const D3D11_BOX box{cur * 16u, 0, 0, (cur + n) * 16u, 1, 1};
        c->UpdateSubresource(g_rig.instBuf, 0, &box, list, 0, 0);
      }
      const uint32_t args[5] = {24u, n, 0u, 0u, cur};
      cur += n;
      const UINT off = *reinterpret_cast<const uint32_t*>(e + 0xc);
      const D3D11_BOX ab0{off, 0, 0, off + 20u, 1, 1};
      c->UpdateSubresource(ab, 0, &ab0, args, 0, 0);
    }
}
struct FakeTerr {
  void** vt;
  const std::vector<Entry>* list;
  const ViewConsts* vc;
};
uint32_t g_secondTcViews = 0;  // views that upload a second, different TerrainContext halfway through the terrain
uint64_t __fastcall FakeTerrRender(void* self, void* ctx) {
  FakeTerr& t = *static_cast<FakeTerr*>(self);
  RCtx& r = *static_cast<RCtx*>(ctx);
  ID3D11DeviceContext* c = r.c;
  // Visualizer 0x102ae0 (b10), then edterrain 0x116b60 inside slot 5 (b11), before any item.
  c->UpdateSubresource(g_rig.b10, 0, nullptr, t.vc->b10, 0, 0);
  c->UpdateSubresource(g_rig.b11, 0, nullptr, t.vc->tc, 0, 0);
  SetCb(r, 10, g_rig.b10);
  SetCb(r, 11, g_rig.b11);
  size_t n = 0;
  for (const Entry& e : *t.list) {
    if (++n == t.list->size() / 2 && (g_secondTcViews >> r.view & 1)) {
      uint8_t tc2[kTcBytes];
      memcpy(tc2, t.vc->tc, kTcBytes);
      reinterpret_cast<float*>(tc2 + 16 * 60)[0] += 1.0f;  // a terrain parameter row changed mid-pass
      c->UpdateSubresource(g_rig.b11, 0, nullptr, tc2, 0, 0);
    }
    if (e.i2) {  // R125: slot 5 for an instancer entry: the context, then vt[6] (any sub-draw bit), then vt[8]
      const float m[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, static_cast<float>(-r.eye[0]), static_cast<float>(-r.eye[1]),
                           static_cast<float>(-r.eye[2]), 1};
      memcpy(r.dcs + 0x98, m, 64);
      const float hr[2] = {static_cast<float>(-r.eye[1]), static_cast<float>(3.0 - r.eye[1])};
      memcpy(r.dcs + 0x358, hr, 8);
      *reinterpret_cast<uint64_t*>(r.dcs + 0x378) = 5000u + static_cast<uint64_t>(ID2(e.i2).id);
      const int t = ID2(e.i2).type;
      if (I2Bit(t, 0) || I2Bit(t, 1)) {
        void** vt = *reinterpret_cast<void***>(e.i2mat);
        reinterpret_cast<uint64_t(__fastcall*)(void*, void*, void*, void*)>(vt[8])(e.i2mat, e.i2, &r, nullptr);
      }
      continue;
    }
    if (!e.item) {
      DrawInstancer(r, e);
      continue;
    }
    // TerrainRenderable slot 5: the entry's world matrix relative to the camera into the context, then vt[8]
    const float x0 = static_cast<float>((e.patch % kGridX) * kPatchSize - r.eye[0]);
    const float z0 = static_cast<float>((e.patch / kGridX) * kPatchSize - r.eye[2]);
    const float m[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, x0, static_cast<float>(-r.eye[1]), z0, 1};
    memcpy(r.dcs + 0x98, m, 64);
    const float hr[2] = {static_cast<float>(-r.eye[1]), static_cast<float>(0.2 - r.eye[1])};
    memcpy(r.dcs + 0x358, hr, 8);
    *reinterpret_cast<uint64_t*>(r.dcs + 0x378) = static_cast<uint64_t>(e.patch);
    void** vt = *reinterpret_cast<void***>(e.mat);
    reinterpret_cast<uint64_t(__fastcall*)(void*, void*, void*, void*)>(vt[8])(e.mat, e.item, &r, nullptr);
  }
  return 7;
}
FakeTerr g_terr{g_terrVt, nullptr, nullptr};

int g_passParity = 0;
// One view's main G-buffer pass.
void Pass(Frame& fr, int v) {
  View& vw = g_rig.view[v];
  ID3D11DeviceContext* c = g_rig.ctx;
  gbpass::ObsFn obs = gbpass::g_obs2.load();
  if (obs) obs(nullptr, nullptr, true);
  RCtx& r = *new RCtx();
  r.c = c, r.view = v, r.frame = fr.f;
  *reinterpret_cast<void**>(r.dcs + 0x370) = &g_iv;  // R125: [ctx + 0x370], the view's instancer context
  const Cam& cam = v == 4 ? fr.mfdCam : fr.cam[v % 2];
  for (int k = 0; k < 3; ++k) r.eye[k] = cam.eye[k];
  c->ClearState();
  c->OMSetRenderTargets(2, vw.rtv, vw.dsv);
  const D3D11_VIEWPORT vp{0, 0, static_cast<float>(vw.w), static_cast<float>(vw.h), 0, 1};
  c->RSSetViewports(1, &vp);
  const FLOAT clr[4] = {0.1f, 0.2f, 0.3f, 1.0f};
  c->ClearRenderTargetView(vw.rtv[0], clr);
  const FLOAT clr1[4] = {0, 0, 0, 0};
  c->ClearRenderTargetView(vw.rtv[1], clr1);
  c->ClearDepthStencilView(vw.dsv, D3D11_CLEAR_DEPTH, 0.0f, 0);
  c->RSSetState(g_rig.rs);
  c->OMSetDepthStencilState(g_rig.dss, 0);
  const FLOAT bf[4] = {1, 1, 1, 1};
  c->OMSetBlendState(g_rig.bs, bf, 0xffffffff);
  c->PSSetSamplers(0, 1, &g_rig.smp);
  // The pass binder: cPerView into one of two ping-pong buffers (parity of the pass), b5, b6, the material CB.
  ID3D11Buffer* b7 = g_rig.b7[g_passParity++ & 1];
  c->UpdateSubresource(b7, 0, nullptr, fr.vc[v].b7, 0, 0);
  c->UpdateSubresource(g_rig.b5, 0, nullptr, fr.vc[v].b5, 0, 0);
  float frame[152] = {static_cast<float>(fr.f) * 0.1f, 0.5f, 0.25f, 1};
  c->UpdateSubresource(g_rig.cbFrame, 0, nullptr, frame, 0, 0);
  const float mat[16] = {0.02f, 0.01f, 0.03f, 0};
  c->UpdateSubresource(g_rig.cbMat, 0, nullptr, mat, 0, 0);
  SetCb(r, 1, g_rig.cbMat);
  SetCb(r, 5, g_rig.b5);
  SetCb(r, 6, g_rig.cbFrame);
  SetCb(r, 7, b7);
  // the terrain item (TerrainObject vt[1] through the vtable, as the G-buffer item loop calls it)
  std::vector<Entry> list = fr.list[v];
  if (fr.swapFocusOrder && v == 2) {
    int a = -1, b = -1;
    // Inside the runs (the twinned part), not at the pass's start.
    for (int i = static_cast<int>(list.size()) / 4; i < static_cast<int>(list.size()) && a < 0; ++i)
      if (list[static_cast<size_t>(i)].item && list[static_cast<size_t>(i)].patch % 7 != 3) a = i;
    for (int i = static_cast<int>(list.size()) * 2 / 3; i < static_cast<int>(list.size()); ++i)
      if (list[static_cast<size_t>(i)].item && list[static_cast<size_t>(i)].patch % 7 != 3 && i != a) {
        b = i;
        break;
      }
    if (a >= 0 && b >= 0) std::swap(list[static_cast<size_t>(a)], list[static_cast<size_t>(b)]);
  }
  g_terr.list = &list;
  g_terr.vc = &fr.vc[v];
  reinterpret_cast<uint64_t(__fastcall*)(void*, void*)>(g_terrVt[1])(&g_terr, &r);
  // models after the terrain (a draw outside the terrain item)
  DrawInstancer(r, Entry{nullptr, nullptr, -1, 40.0f, 30.0f});
  if (obs) obs(nullptr, nullptr, false);
  delete &r;
}
// DCS runs several ms of other passes between an eye's periphery and focus G-buffer passes: the workers' lists are
// ready by then. The test waits for them at that point (a late list is a separate, counted outcome).
void WaitJobs() {
  for (int i = 0; i < 2000; ++i) {
    bool busy = false;
    for (const Eye& e : ftwin::g_eye)
      for (const Stream& s : e.s) busy |= s.job.load() == kJRunning;
    if (!busy) return;
    Sleep(1);
  }
}
void RunFrame(Frame& fr) {
  if (fr.publishCi) {
    CiFrame ci = fr.ci;
    if (fr.ciNudge != 0)  // the published focus camera's centre moved (a gaze step DCS's focus upload did not make)
      for (int e = 0; e < 2; ++e)
        for (int k = 0; k < 3; ++k) ci.f[e][k * 4 + 0] += fr.ciNudge * ci.f[e][k * 4 + 3];
    PublishCi(ci);
  }
  if (fr.mfd) RunChain(fr, 4), Pass(fr, 4);
  for (int v = 0; v < 4; ++v) {
    if (v == 2) WaitJobs();
    if (v == 2 && fr.argsSwap) {  // R125: IM2 recreated its argument buffer between the periphery and the focus
      g_i2ArgsSel = 1 - g_i2ArgsSel;
      *reinterpret_cast<void**>(g_im2.raw + 0x370) = g_i2Members[g_i2ArgsSel ? 6 : 5];
    }
    RunChain(fr, v);
    Pass(fr, v);
  }
}
// Bytes of the views' targets (RT0, RT1, depth) of one frame.
std::vector<uint8_t> Readback(int v) {
  std::vector<uint8_t> out;
  View& vw = g_rig.view[v];
  ID3D11Resource* src[3] = {vw.rt[0], vw.rt[1], vw.depth};
  for (int i = 0; i < 3; ++i) {
    g_rig.ctx->CopyResource(vw.stage[i], src[i]);
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(g_rig.ctx->Map(vw.stage[i], 0, D3D11_MAP_READ, 0, &m))) continue;
    const size_t row = static_cast<size_t>(vw.w) * (i == 1 ? 16 : 4);
    for (int y = 0; y < vw.h; ++y) {
      const uint8_t* p = static_cast<const uint8_t*>(m.pData) + static_cast<size_t>(y) * m.RowPitch;
      out.insert(out.end(), p, p + row);
    }
    g_rig.ctx->Unmap(vw.stage[i], 0);
  }
  return out;
}

// ---- running scenarios --------------------------------------------------------
struct Script {
  int frames = 12;
  int nudgeFrame = -1;      // prediction mismatch
  int noCiFrame = -1;       // F10: no culling
  int reinstallFrame = -1;  // uninstall + install
  int sceneFrame = -1;      // new items (mission restart)
  int orderFrame = -1;      // eye 0 focus order swapped
  int faultFrame = -1;      // the item hook faults (one descriptor read)
  int auditBadPatch = -1;   // a patch whose focus draw differs although its descriptor matches
  int offFrame = -1;        // g_active false from here (kill switch)
  int secondTcFrame = -1;   // periphery L and focus L upload a second TerrainContext mid-terrain
  int secondTcFocusFrame = -1;  // only focus R does
  bool shadow = false;
  int verify = 0;            // [Suite] FocusTwinVerify
  int trust = 2, auditEvery = 5;
  bool focusCopies = false;  // per-view RenderItem copies in the focus lists
  bool twin = true;         // false: stock reference (not installed)
  // R125
  bool i2 = false;          // the scene has MetaMaterial_Instancer2 items
  bool inst = false;        // [Model] FocusTwinInstancer
  bool patches = true;      // [Suite] FocusTwinPatches
  bool noGate = false;      // the gate pass cannot be built
  bool noMask = false;      // negative control: every recorded sub-draw enabled
  bool pvInt = false;       // one type's per-draw int differs per view
  int argsSwapFrame = -1;   // the argument buffer recreated before focus L of this frame
  int i2AuditBad = -1;      // this instancer item draws another texture in the focus views
};
struct Result {
  std::vector<std::vector<uint8_t>> img;  // per frame: 4 views
  std::vector<std::string> log;           // per frame: outcomes
  ftwin::Stats st;
  bool installed = false, slotsBack = false, instInstalled = false;
  uint64_t twinPasses = 0;
};
void ResetFt() {
  ftwin::Uninstall(nullptr);
  ftwin::g_st = ftwin::Stats();
  ftwin::g_faulted = false;
  ftwin::g_faultWhere = "";
  ftwin::g_active = false;
  ftwin::g_renderTid = 0;
  ftwin::g_ciCur = -1;
  ftwin::g_descFn = nullptr;
  ftwin::g_win.open = false;
  ftwin::g_win.mode = kWmNone;
  ftwin::g_instKey = 0;
  ftwin::g_patchesKey = true;
  ftwin::g_testNoGate = false;
  ftwin::g_testNoMask = false;
}
bool InstallFt() {
  ftwin::g_testDevice = true;
  Targets2 tg{&g_matVt[8], &g_terrVt[1]};
  if (g_withI2) tg.instSlot = &g_i2Vt[8], tg.instGlobal = &g_fakeGlobal, tg.im2Vtbl = g_im2Vt;  // R125
  const bool ok = ftwin::InstallOn(tg, g_rig.dev, g_rig.ctx);
  ftwin::g_active = ok;
  return ok;
}
int g_faultCountdown = -1;
bool FaultyDesc(void* mat, void* item, void* ctx, uint64_t* out, uint64_t* key) {
  if (g_faultCountdown == 0) {
    g_faultCountdown = -1;
    volatile int* p = nullptr;
    *p = 1;  // access violation inside the item hook
  }
  if (g_faultCountdown > 0) --g_faultCountdown;
  return ftwin::ReadDesc(mat, item, ctx, out, key);
}
Result Run(const Script& sc) {
  Result res;
  ResetFt();
  g_passParity = 0;
  ftwin::g_trustNeeded = sc.trust;
  ftwin::g_auditEvery = sc.auditEvery;
  ftwin::g_verifyEvery = sc.verify;
  ftwin::g_island = 3;
  ftwin::g_chunk = 16;
  ftwin::g_shadowKey = sc.shadow;
  ftwin::g_predTol = 5e-5;
  g_focusCopies = sc.focusCopies;
  // R125: the instancer scene (fresh IM2 objects and shared buffers: both runs of a scenario start alike)
  g_withI2 = sc.i2, g_i2PvInt = sc.pvInt, g_i2AuditBad = sc.i2AuditBad;
  if (sc.i2) {
    BuildI2();
    std::vector<uint8_t> zero(4096 * 16, 0);
    g_rig.ctx->UpdateSubresource(g_rig.instBuf, 0, nullptr, zero.data(), 0, 0);
    for (auto* b : g_rig.argsBuf) g_rig.ctx->UpdateSubresource(b, 0, nullptr, zero.data(), 0, 0);
  }
  ftwin::g_instKey = sc.inst ? 1 : 0;
  ftwin::g_patchesKey = sc.patches;
  ftwin::g_testNoGate = sc.noGate;
  ftwin::g_testNoMask = sc.noMask;
  BuildScene(0, sc.auditBadPatch);
  if (sc.twin) res.installed = InstallFt();
  for (int f = 0; f < sc.frames; ++f) {
    if (sc.twin && f == sc.reinstallFrame) {
      ftwin::Uninstall("test");
      res.slotsBack = g_matVt[8] == reinterpret_cast<void*>(&FakeMatRender) &&
                      g_terrVt[1] == reinterpret_cast<void*>(&FakeTerrRender) && !gbpass::g_obs2.load() &&
                      !g_collectTap.load();
      InstallFt();
    }
    if (f == sc.sceneFrame) BuildScene(f, sc.auditBadPatch);
    if (sc.twin && f == sc.offFrame) ftwin::g_active = false;
    if (sc.twin && f == sc.faultFrame) {
      g_faultCountdown = 5;
      ftwin::g_descFn = &FaultyDesc;
    }
    Frame fr;
    MakeFrame(f, fr);
    fr.publishCi = f != sc.noCiFrame;
    fr.ciNudge = f == sc.nudgeFrame ? 0.02f : 0.0f;
    fr.swapFocusOrder = f == sc.orderFrame;
    fr.argsSwap = f == sc.argsSwapFrame;
    g_secondTcViews = (f == sc.secondTcFrame ? (1u << 0) | (1u << 2) : 0u) | (f == sc.secondTcFocusFrame ? (1u << 3) : 0u);
    const uint64_t t0 = ftwin::g_st.why[kWTwin];
    const ftwin::Stats before = ftwin::g_st;
    RunFrame(fr);
    res.twinPasses += ftwin::g_st.why[kWTwin] - t0;
    {
      char b[400];
      int n = snprintf(b, sizeof(b), "f%d:", f);
      for (int w = 0; w < kWCount; ++w)
        if (ftwin::g_st.why[w] != before.why[w]) n += snprintf(b + n, sizeof(b) - n, " %s %llu", WhyName(w), (unsigned long long)(ftwin::g_st.why[w] - before.why[w]));
      n += snprintf(b + n, sizeof(b) - n, " | twinned %llu flushes %llu focusPasses %llu stale %llu", (unsigned long long)(ftwin::g_st.twinItems - before.twinItems),
                    (unsigned long long)(ftwin::g_st.flushes - before.flushes), (unsigned long long)(ftwin::g_st.focusPasses - before.focusPasses),
                    (unsigned long long)(ftwin::g_st.staleSkips - before.staleSkips));
      res.log.push_back(b);
    }
    WaitJobs();
    std::vector<uint8_t> all;
    for (int v = 0; v < 4; ++v) {
      std::vector<uint8_t> b = Readback(v);
      all.insert(all.end(), b.begin(), b.end());
    }
    res.img.push_back(std::move(all));
  }
  res.st = ftwin::g_st;
  res.instInstalled = ftwin::g_instInstalled;
  g_withI2 = false, g_i2PvInt = false, g_i2AuditBad = -1;
  ftwin::g_descFn = nullptr;
  ftwin::g_verifyEvery = 0;
  g_secondTcViews = 0;
  return res;
}
int FirstDiff(const Result& a, const Result& b) {
  if (a.img.size() != b.img.size()) return 0;
  for (size_t f = 0; f < a.img.size(); ++f)
    if (a.img[f] != b.img[f]) {
      if (getenv("QV_R123_DEBUG")) {
        // which view / target / pixel
        size_t off = 0;
        for (int v = 0; v < 4; ++v) {
          const View& vw = g_rig.view[v];
          const size_t sz[3] = {static_cast<size_t>(vw.w) * vw.h * 4, static_cast<size_t>(vw.w) * vw.h * 16, static_cast<size_t>(vw.w) * vw.h * 4};
          for (int t = 0; t < 3; ++t) {
            size_t nd = 0, first = SIZE_MAX;
            for (size_t i = 0; i < sz[t]; ++i)
              if (a.img[f][off + i] != b.img[f][off + i]) {
                ++nd;
                if (first == SIZE_MAX) first = i;
              }
            if (nd) {
              const size_t px = first / (t == 1 ? 16 : 4);
              printf("     (frame %zu view %d target %d: %zu bytes differ, first at pixel (%zu, %zu))\n", f, v, t, nd, px % vw.w, px / vw.w);
              if (t == 1) {
                const float* fa = reinterpret_cast<const float*>(&a.img[f][off + px * 16]);
                const float* fb = reinterpret_cast<const float*>(&b.img[f][off + px * 16]);
                printf("       stock (%g %g %g %g) twin (%g %g %g %g)\n", fa[0], fa[1], fa[2], fa[3], fb[0], fb[1], fb[2], fb[3]);
              }
              if (t == 0) {
                const uint8_t* ca = &a.img[f][off + px * 4];
                const uint8_t* cb = &b.img[f][off + px * 4];
                printf("       stock rgba %u %u %u %u twin %u %u %u %u\n", ca[0], ca[1], ca[2], ca[3], cb[0], cb[1], cb[2], cb[3]);
              }
            }
            off += sz[t];
          }
        }
        for (const std::string& s : b.log) printf("     %s\n", s.c_str());
      }
      return static_cast<int>(f);
    }
  return -1;
}
void PrintStats(const char* name, const Result& r) {
  const ftwin::Stats& s = r.st;
  printf("     (%s, our CPU on this PC: periphery %.2f us per teed item, focus %.2f us per item, windows %.1f us each "
         "(periphery baseline + end, focus classification + decision + end), flush %.1f us each on WARP)\n",
         name, s.periphItems ? s.periphItemOwnUs / static_cast<double>(s.periphItems) : 0.0,
         s.focusItems ? s.focusItemOwnUs / static_cast<double>(s.focusItems) : 0.0,
         (s.periphOwnUs - s.periphItemOwnUs + s.focusOwnUs - s.focusItemOwnUs - s.focusFlushUs) /
             std::max(1.0, static_cast<double>(s.periphPasses + s.focusPasses)),
         s.flushes ? s.focusFlushUs / static_cast<double>(s.flushes) : 0.0);
  printf("     (%s: record of a teed draw %.2f us, tapped calls %.1f per periphery item)\n", name,
         s.teeDraws ? s.teeDrawUs / static_cast<double>(s.teeDraws) : 0.0,
         s.periphItems ? static_cast<double>(s.tapped) / static_cast<double>(s.periphItems + s.focusItems) : 0.0);
  printf("     (%s: focus passes %llu, twin %llu, items twinned %llu of %llu, audits %llu: ok %llu fail %llu, periphery items %llu, "
         "not twinnable: work %llu inherit %llu; DCS items: unmatched %llu island %llu order %llu; flushes %llu, lists %llu, "
         "why: pred %llu mismatch %llu stale %llu eyebad %llu targets %llu build %llu late %llu trust %llu audit %llu)\n",
         name, (unsigned long long)s.focusPasses, (unsigned long long)s.why[kWTwin], (unsigned long long)s.twinItems,
         (unsigned long long)s.focusItems, (unsigned long long)s.audits, (unsigned long long)s.auditOk,
         (unsigned long long)s.auditFail, (unsigned long long)s.periphItems, (unsigned long long)s.perWhy[kPWork],
         (unsigned long long)s.perWhy[kPInherit], (unsigned long long)s.itemWhy[kIUnmatched], (unsigned long long)s.itemWhy[kIIsland],
         (unsigned long long)s.itemWhy[kIOrder], (unsigned long long)s.flushes, (unsigned long long)s.ecls,
         (unsigned long long)s.why[kWPred], (unsigned long long)s.why[kWMismatch], (unsigned long long)s.why[kWStale],
         (unsigned long long)s.why[kWEyeBad], (unsigned long long)s.why[kWTargets], (unsigned long long)s.why[kWBuild],
         (unsigned long long)s.why[kWLate], (unsigned long long)s.why[kWTrust], (unsigned long long)s.why[kWAudit]);
}

void Pipeline() {
  for (int i = 0; i < 12; ++i) g_matVt[i] = reinterpret_cast<void*>(&FakeDtor), g_instVt[i] = reinterpret_cast<void*>(&FakeDtor);
  for (int i = 0; i < 4; ++i) g_terrVt[i] = reinterpret_cast<void*>(&FakeDtor);
  g_matVt[8] = reinterpret_cast<void*>(&FakeMatRender);
  g_terrVt[1] = reinterpret_cast<void*>(&FakeTerrRender);
  if (!MakeRig()) {
    Check(false, "R123 pipeline: WARP rig (11.1 device, shaders, targets)");
    return;
  }
  const defrec::Caps caps = defrec::QueryCaps(g_rig.dev);
  if (!caps.cbOffsetting) {
    QvSkip("R123 pipeline: WARP without constant-buffer offsets\n");
    return;
  }
  // 1. Exact output while the twin draws (eye tracking, jitter, per-view patches, inheritance, MFD frames).
  Script base;
  base.frames = 12;
  Script stock = base;
  stock.twin = false;
  const Result ref = Run(stock);
  const Result tw = Run(base);
  PrintStats("twin", tw);
  const int d1 = FirstDiff(ref, tw);
  Check(tw.installed && d1 < 0, "R123 pipeline: focus and periphery targets byte-identical to stock, every frame, with the twin on");
  Check(tw.twinPasses >= 10 && tw.st.twinItems > 200,
        "R123 pipeline: the twin replaced the focus patch items (after the trust audits) in most focus passes");
  Check(tw.st.itemWhy[kIUnmatched] > 0 && tw.st.perWhy[kPInherit] > 0 && tw.st.auditOk >= 4 && tw.st.auditFail == 0,
        "R123 pipeline: per-view patches drawn by DCS (unmatched), inherited uploads not twinnable, audits passed");
  Check(tw.st.why[kWStale] == 0 && tw.st.why[kWMismatch] == 0 && tw.st.focusPasses == 24,
        "R123 pipeline: MFD views (centred projection, odd frames) ignored; every quad focus pass counted once");
  // 1b. The focus lists hold per-view copies of the RenderItems: matched by content (not by address), exact.
  Script cp = base;
  cp.focusCopies = true;
  Script cpS = cp;
  cpS.twin = false;
  const Result cpRef = Run(cpS);
  const Result cpTw = Run(cp);
  g_focusCopies = false;
  Check(FirstDiff(cpRef, cpTw) < 0 && cpTw.st.twinItems == tw.st.twinItems && cpTw.st.auditFail == 0,
        "R123 pipeline: per-view RenderItem copies in the focus lists are matched by content, the same items replaced, exact");
  // 2. Prediction mismatch -> that frame stock.
  Script mm = base;
  mm.nudgeFrame = 7;
  Script mmS = mm;
  mmS.twin = false;
  const Result mmRef = Run(mmS);
  const Result mmTw = Run(mm);
  if (getenv("QV_R123_DEBUG")) {
    PrintStats("mismatch", mmTw);
    for (const std::string& s : mmTw.log) printf("     %s\n", s.c_str());
  }
  Check(FirstDiff(mmRef, mmTw) < 0 && mmTw.st.why[kWMismatch] == 2 && mmTw.st.why[kWPred] == 4,
        "R123 pipeline: a focus camera that does not match DCS's focus upload -> both focus passes of that frame stock, "
        "the next frame relearns (no prediction), exact");
  // 3. F10: no culling (no collect-time camera) in one frame.
  Script f10 = base;
  f10.noCiFrame = 6;
  Script f10S = f10;
  f10S.twin = false;
  const Result f10Ref = Run(f10S);
  const Result f10Tw = Run(f10);
  Check(FirstDiff(f10Ref, f10Tw) < 0 && f10Tw.st.why[kWStale] + f10Tw.st.why[kWEyeBad] >= 2 && f10Tw.st.staleSkips >= 2,
        "R123 pipeline: a frame without culling (F10 / map) -> nothing teed or replaced in it, exact");
  // 4. Restart / hot reload (uninstall + install) and a scene change (new items).
  Script rs = base;
  rs.frames = 16;
  rs.reinstallFrame = 5;
  rs.sceneFrame = 10;
  Script rsS = rs;
  rsS.twin = false;
  const Result rsRef = Run(rsS);
  const Result rsTw = Run(rs);
  PrintStats("restart", rsTw);
  Check(FirstDiff(rsRef, rsTw) < 0 && rsTw.slotsBack && rsTw.twinPasses >= 8,
        "R123 pipeline: uninstall puts the slots and observers back; reinstall and a scene change resume the twin, exact");
  // 4b. A second, different TerrainContext inside a terrain pass: a periphery's stream is not used (its focus pass is
  // stock); a focus pass replaces nothing after it.
  Script tc2 = base;
  tc2.secondTcFrame = 6;
  tc2.secondTcFocusFrame = 8;
  Script tc2S = tc2;
  tc2S.twin = false;
  const Result tc2Ref = Run(tc2S);
  const Result tc2Tw = Run(tc2);
  Check(FirstDiff(tc2Ref, tc2Tw) < 0 && tc2Tw.st.why[kWEyeBad] >= 1 && tc2Tw.twinPasses >= 10,
        "R123 pipeline: a second TerrainContext mid-terrain (periphery: that eye's focus stock; focus: no replacement after it), exact");
  // 5. Order violation: the rest of that focus pass is DCS's.
  Script od = base;
  od.orderFrame = 8;
  Script odS = od;
  odS.twin = false;
  const Result odRef = Run(odS);
  const Result odTw = Run(od);
  Check(FirstDiff(odRef, odTw) < 0 && odTw.st.itemWhy[kIOrder] >= 1,
        "R123 pipeline: a focus item out of the periphery's order stops the replacement for that pass, exact");
  // 6. Audit failure: a matched patch drawn with another texture in the focus -> latched off.
  Script au = base;
  au.auditBadPatch = 2 * kGridX + 5;
  Script auS = au;
  auS.twin = false;
  const Result auRef = Run(auS);
  const Result auTw = Run(au);
  Check(FirstDiff(auRef, auTw) < 0 && auTw.st.auditFail == 1 && auTw.twinPasses == 0 && ftwin::g_faulted.load(),
        "R123 pipeline: an audit that finds a matched item drawn differently latches the twin off before it ever draws");
  // 7. A fault in the item hook: latched off, DCS draws the item, exact.
  Script ft = base;
  ft.faultFrame = 6;
  Script ftS = ft;
  ftS.twin = false;
  const Result ftRef = Run(ftS);
  const Result ftTw = Run(ft);
  Check(FirstDiff(ftRef, ftTw) < 0 && ftwin::g_faulted.load() && ftTw.twinPasses > 0,
        "R123 pipeline: an exception in the item hook latches the twin off for the session, output exact");
  // 8. Shadow mode: audits only.
  Script sh = base;
  sh.shadow = true;
  const Result shTw = Run(sh);
  Check(FirstDiff(ref, shTw) < 0 && shTw.twinPasses == 0 && shTw.st.auditOk >= 10 && shTw.st.twinItems == 0,
        "R123 pipeline: [Suite] FocusTwinShadow audits every focus pass and never replaces an item");
  // 8b. [Suite] FocusTwinVerify: the focus pass drawn stock, then again with the twin from the same target contents,
  // compared byte for byte: no difference; the frames stay exact (run 2's result is the one kept).
  Script vf = base;
  vf.verify = 3;
  const Result vfTw = Run(vf);
  PrintStats("verify", vfTw);
  printf("     (verify: %llu passes compared, %llu of %llu bytes differ, %llu skipped)\n", (unsigned long long)vfTw.st.verifies,
         (unsigned long long)vfTw.st.verifyDiffBytes, (unsigned long long)vfTw.st.verifyBytes, (unsigned long long)vfTw.st.verifySkipped);
  Check(FirstDiff(ref, vfTw) < 0 && vfTw.st.verifies >= 4 && vfTw.st.verifyFail == 0 && vfTw.st.verifyDiffBytes == 0 &&
            vfTw.st.verifyBytes > 0 && !ftwin::g_faulted.load(),
        "R123 pipeline: [Suite] FocusTwinVerify compares stock and twin of the same focus pass: 0 bytes differ, exact");
  // ... and finds a twin that would draw differently (audits off for the check): latched off.
  Script vb = base;
  vb.verify = 2;
  vb.trust = 0, vb.auditEvery = 0;
  vb.auditBadPatch = 2 * kGridX + 5;
  const Result vbTw = Run(vb);
  Check(vbTw.st.verifyFail == 1 && vbTw.st.verifyDiffBytes > 0 && ftwin::g_faulted.load(),
        "R123 pipeline: the verify finds a twin that draws differently from DCS and latches it off");
  // 9. Kill switch mid-run: pass-through from that frame.
  Script ks = base;
  ks.offFrame = 7;
  const Result ksTw = Run(ks);
  Check(FirstDiff(ref, ksTw) < 0 && ksTw.twinPasses > 0,
        "R123 pipeline: the twin off mid-run (kill switch) is pass-through from that frame, exact");
  // 10. Key off: Tick(0) installs nothing; no slot, observer or tap changes.
  ResetFt();
  void* m8 = g_matVt[8];
  void* t1 = g_terrVt[1];
  ftwin::Tick(0, false, false, 0);
  ftwin::Tick(0, true, false, 0);
  const bool off = ftwin::g_state.load() == 0 && g_matVt[8] == m8 && g_terrVt[1] == t1 && !gbpass::g_obs2.load() &&
                   !g_collectTap.load() && !ftwin::g_active.load();
  Script ko = base;
  ko.twin = false;
  const Result koRun = Run(ko);
  Check(off && FirstDiff(ref, koRun) < 0 && koRun.st.windows == 0 && koRun.st.focusPasses == 0,
        "R123 pipeline: key 0 is a no-op (nothing installed, vtable slots / pass observer / collect tap untouched, output stock)");
  // In this process DCS's modules are absent: key 1 installs nothing either (TryInstall retries quietly).
  if (!GetModuleHandleW(L"edterrainGraphics41.dll") && !GetModuleHandleW(L"Visualizer.dll")) {
    ftwin::Tick(1, false, false, 0);
    Check(ftwin::g_state.load() == 0 && g_matVt[8] == m8, "R123: key 1 without DCS's modules installs nothing (retried each second)");
  }
  ftwin::Tick(0, false, false, 0);
  ResetFt();
  FreeScene();
}

// R125: the instancer part on WARP (the R123 rig plus MetaMaterial_Instancer2 items and InstanceManager2).
void PrintInst(const char* name, const Result& r) {
  const ftwin::Stats& s = r.st;
  printf("     (%s: instancer items per focus pass twinned %.1f of %.1f, %llu sub-draws drawn by the twin, %llu recorded "
         "sub-draws not drawn in the focus (per-view bit), %llu gated lists of %llu, periphery %llu items %llu records; "
         "DCS items: unmatched %llu other sub-draws %llu island %llu; audits ok %llu fail %llu)\n",
         name, s.focusPasses ? static_cast<double>(s.twinInstItems) / static_cast<double>(s.focusPasses) : 0.0,
         s.focusPasses ? static_cast<double>(s.focusInstItems) / static_cast<double>(s.focusPasses) : 0.0,
         (unsigned long long)s.twinInstDraws, (unsigned long long)s.instMasked, (unsigned long long)s.gateLists,
         (unsigned long long)s.ecls, (unsigned long long)s.periphInstItems, (unsigned long long)s.instRecords,
         (unsigned long long)s.itemWhy[kIUnmatched], (unsigned long long)s.itemWhy[kISubs], (unsigned long long)s.itemWhy[kIIsland],
         (unsigned long long)s.auditOk, (unsigned long long)s.auditFail);
}
void PipelineInst() {
  if (!g_rig.ok) return;
  // I1. Exact with the instancer twin (patches too), every frame; the twin replaced instancer items and drew only the
  // focus view's sub-draws through the gate pass.
  Script base;
  base.frames = 12;
  base.i2 = true;
  base.inst = true;
  Script stock = base;
  stock.twin = false;
  const Result ref = Run(stock);
  const Result tw = Run(base);
  PrintStats("instancer", tw);
  PrintInst("instancer", tw);
  Check(tw.installed && tw.instInstalled && FirstDiff(ref, tw) < 0,
        "R125 pipeline: focus and periphery targets byte-identical to stock, every frame, with the instancer twin on");
  Check(tw.st.twinInstItems >= 100 && tw.st.twinInstDraws > 0 && tw.st.gateLists > 0 && tw.st.twinItems > tw.st.twinInstItems &&
            tw.st.auditFail == 0 && tw.st.auditOk >= 4,
        "R125 pipeline: instancer items (and patches) replaced after the trust audits, through gated command lists");
  {  // our render-thread cost per item on this PC (sampled; WARP's flushes are not representative and not checked)
    const double per = tw.st.periphItems ? tw.st.periphItemOwnUs / static_cast<double>(tw.st.periphItems) : 0.0;
    const double foc = tw.st.focusItems ? tw.st.focusItemOwnUs / static_cast<double>(tw.st.focusItems) : 0.0;
    const double rec = tw.st.teeDraws ? tw.st.teeDrawUs / static_cast<double>(tw.st.teeDraws) : 0.0;
    Check(per > 0 && per < 1.0 && foc > 0 && foc < 1.0 && rec < 1.0,
          "R125 pipeline: our cost per item with instancer items stays sub-microsecond on this PC (periphery tee, focus "
          "decision, record per draw)");
  }
  Check(tw.st.instMasked > 0,
        "R125 pipeline: recorded periphery sub-draws whose focus bit is off were not drawn (the per-view enable)");
  // Negative control: every recorded sub-draw enabled -> the replay draws argument words another view's chain left.
  Script nm = base;
  nm.noMask = true;
  const Result nmTw = Run(nm);
  {
    size_t dn = 0;
    for (size_t f = 0; f < ref.img.size() && f < nmTw.img.size(); ++f)
      for (size_t i = 0; i < ref.img[f].size() && i < nmTw.img[f].size(); ++i) dn += ref.img[f][i] != nmTw.img[f][i];
    printf("     (negative control: %zu target bytes differ from stock over the run)\n", dn);
  }
  Check(FirstDiff(ref, nmTw) >= 0,
        "R125 pipeline: negative control: without the per-view sub-draw enable the replay differs from stock (stale "
        "argument words of another view)");
  // I2. A type whose per-draw int (its instance region) differs per view: its focus item does not match, exact.
  Script pv = base;
  pv.pvInt = true;
  Script pvS = pv;
  pvS.twin = false;
  const Result pvRef = Run(pvS);
  const Result pvTw = Run(pv);
  Check(FirstDiff(pvRef, pvTw) < 0 && pvTw.st.twinInstItems > 0 && pvTw.st.itemWhy[kIUnmatched] > tw.st.itemWhy[kIUnmatched],
        "R125 pipeline: a type entry that differs per view (the per-draw int) -> that focus item is DCS's, exact");
  // I3. The argument buffer recreated between the periphery and the focus: nothing of that frame matches, exact.
  Script as = base;
  as.argsSwapFrame = 7;
  Script asS = as;
  asS.twin = false;
  const Result asRef = Run(asS);
  const Result asTw = Run(as);
  Check(FirstDiff(asRef, asTw) < 0 && asTw.st.twinInstItems > 0,
        "R125 pipeline: IM2's argument buffer recreated between the periphery and the focus pass -> stock for those "
        "items, then the twin again, exact");
  // I4. [Suite] FocusTwinPatches=0: instancer items only.
  Script io = base;
  io.patches = false;
  const Result ioTw = Run(io);
  PrintStats("instancer only", ioTw);
  Check(FirstDiff(ref, ioTw) < 0 && ioTw.st.twinInstItems > 0 && ioTw.st.twinItems == ioTw.st.twinInstItems,
        "R125 pipeline: [Suite] FocusTwinPatches=0 replaces instancer items only (patches DCS's), exact");
  // I5. No gate pass (compute shader unavailable): the instancer part is not installed; patches still twinned.
  Script ng = base;
  ng.noGate = true;
  const Result ngTw = Run(ng);
  Check(FirstDiff(ref, ngTw) < 0 && ngTw.installed && !ngTw.instInstalled && ngTw.st.twinInstItems == 0 && ngTw.st.twinItems > 0 &&
            g_i2Vt[8] == reinterpret_cast<void*>(&FakeI2Render),
        "R125 pipeline: without the gate pass the instancer slot is not hooked (trees and buildings DCS's), patches twinned, exact");
  // I6. An instancer item drawn with another texture in the focus (an input the descriptor cannot see): the audit
  // latches the twin off before it draws.
  Script au = base;
  au.i2AuditBad = 4;
  Script auS = au;
  auS.twin = false;
  const Result auRef = Run(auS);
  const Result auTw = Run(au);
  Check(FirstDiff(auRef, auTw) < 0 && auTw.st.auditFail == 1 && auTw.twinPasses == 0 && ftwin::g_faulted.load(),
        "R125 pipeline: an audit that finds a matched instancer item drawn differently latches the twin off");
  // I7. [Suite] FocusTwinVerify with the instancer part: stock and twin of the same focus pass, 0 bytes differ.
  Script vf = base;
  vf.verify = 3;
  const Result vfTw = Run(vf);
  printf("     (instancer verify: %llu passes compared, %llu of %llu bytes differ, %llu skipped)\n",
         (unsigned long long)vfTw.st.verifies, (unsigned long long)vfTw.st.verifyDiffBytes,
         (unsigned long long)vfTw.st.verifyBytes, (unsigned long long)vfTw.st.verifySkipped);
  Check(FirstDiff(ref, vfTw) < 0 && vfTw.st.verifies >= 4 && vfTw.st.verifyFail == 0 && vfTw.st.verifyDiffBytes == 0 &&
            vfTw.st.twinInstItems > 0 && !ftwin::g_faulted.load(),
        "R125 pipeline: [Suite] FocusTwinVerify with instancer items: 0 bytes differ, exact");
  // I8. FocusTwinInstancer=0 with the instancer scene: the instancer slot untouched (R123 as merged), exact.
  Script k0 = base;
  k0.inst = false;
  const Result k0Tw = Run(k0);
  Check(FirstDiff(ref, k0Tw) < 0 && k0Tw.installed && !k0Tw.instInstalled && k0Tw.st.twinInstItems == 0 &&
            k0Tw.st.twinItems > 0 && g_i2Vt[8] == reinterpret_cast<void*>(&FakeI2Render),
        "R125 pipeline: [Model] FocusTwinInstancer=0: the instancer slot is never hooked, patches as R123, exact");
  // Uninstall puts the instancer slot back.
  ResetFt();
  g_withI2 = true;
  BuildI2();
  ftwin::g_instKey = 1;
  const bool inst = InstallFt() && ftwin::g_instInstalled && g_i2Vt[8] != reinterpret_cast<void*>(&FakeI2Render);
  ftwin::Uninstall("test");
  Check(inst && g_i2Vt[8] == reinterpret_cast<void*>(&FakeI2Render) && !ftwin::g_instInstalled,
        "R125: uninstall puts MetaMaterial_Instancer2 vt[8] back");
  g_withI2 = false;
  ResetFt();
  FreeI2();
}

// The production hook targets against the installed DCS (images mapped without running them, freed after).
void BuildCheck() {
  if (!dcsinstall::Found()) {
    dcsinstall::Skip("R123 build checks");
    return;
  }
  HMODULE etg = dcsinstall::LoadBinary(L"edterrainGraphics41.dll");
  HMODULE vis = dcsinstall::LoadBinary(L"Visualizer.dll");
  void** ms = nullptr;
  void** ts = nullptr;
  const char* why = (etg && vis) ? CheckBuild(reinterpret_cast<uint8_t*>(etg), reinterpret_cast<uint8_t*>(vis), &ms, &ts)
                                 : "not loaded";
  if (why) printf("     (R123 build check: %s)\n", why);
  Check(!why && ms && ts,
        "R123 build: MetaMaterial vt[8] (0xb99e0) and TerrainObject vt[1] (0x103cf0) match the installed DCS (RTTI, "
        "slot entries, code hashes of the render functions and of the slot-5 item call)");
  bool neg = true;
  if (etg) neg &= !shadowtex::CodeIs(reinterpret_cast<uint8_t*>(etg), kEtgCode[0].begin, kEtgCode[0].end, kEtgCode[0].hash ^ 1);
  Check(neg, "R123 build: a code hash that differs is refused (not installed)");
  // R125: the instancer part's targets (MetaMaterial_Instancer2 vt[8], InstanceManager2 slots, code hashes).
  void** is = nullptr;
  void** gl = nullptr;
  void* iv = nullptr;
  const char* whyI = etg ? CheckInstBuild(reinterpret_cast<uint8_t*>(etg), &is, &gl, &iv) : "not loaded";
  if (whyI) printf("     (R125 build check: %s)\n", whyI);
  Check(!whyI && is && gl && iv,
        "R125 build: MetaMaterial_Instancer2 vt[8] (0xb4180), vt[6], InstanceManager2 slots 30/32/33/37-44 and the "
        "terrain global's getter match the installed DCS (RTTI, slot entries, code hashes)");
  bool negI = true;
  if (etg) negI &= !shadowtex::CodeIs(reinterpret_cast<uint8_t*>(etg), kInstCode[0].begin, kInstCode[0].end, kInstCode[0].hash ^ 1);
  Check(negI, "R125 build: an instancer code hash that differs is refused");
  if (etg) FreeLibrary(etg);
  if (vis) FreeLibrary(vis);
}

void Run() {
  PureCensus();
  PureMisc();
  Pipeline();
  PipelineInst();
  BuildCheck();
}

}  // namespace r123test
