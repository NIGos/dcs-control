// 0.7 three views ([Views] ThreeViews, default 0): DCS renders the right periphery of Quad Views only as its outer
// strip (NDC x >= [Views] ThreeViewsStripNdc, default 0.55) plus the cockpit; the bundled Quad Views layer fills the
// rest of that view at xrEndFrame from the left periphery's colour, reprojected with its depth. The focus views are
// unchanged. A first, rough version to judge in the headset: measured 2026-11-19 (16 ON/OFF pairs, ATMOSX, culling
// only) -1.54 ms GPU frame in the F-16 cockpit in low flight, about 0 in the cockpit on the ground.
//
// Parts (included from features.inc after fixation_depth.h, chain_fast.h and shader_fast.h):
//   - views.inc (tvc, TvPatch): at collect time every collection of the right periphery but the cockpit's (shading
//     model 4) culls with a volume whose main frustum is the strip: objects and the CPU-culled terrain patches;
//   - vstrip below: the GPU-culled terrain / forest chain of the right periphery's passes (DCS's TerrainContext constant
//     buffer, 2176 bytes, culling planes at rows 28..33) gets its x >= -w plane moved to x >= s w, on the GPU, at each
//     CS bind of that buffer in those passes (DCS uploads it after the first compute bind of a pass);
//   - fixation_depth.h (tvexp): the periphery views' depth + stencil pushed to Quad Views (DcsVr_QuadViewsDepth, hmdView
//     0 / 1), the right one flagged DCSVR_DEPTH_THREE_VIEWS while engaged (and a few frames after);
//   - this file's Tick (worker): engages only when every condition holds and logs each change with its reason:
//     the key on, the hotkey state on, the engine on (kill switch), Quad Views asking for both periphery views (a layer
//     that cannot fill never sees a culled view), fixation depth installed. Frame generation no longer gates it: the
//     cockpit is still drawn in the right periphery (mask and cockpit depth stay right), and the motion companion
//     publishes that view without view_flags::valid while engaged (cockpit_mask.h TvRightMvUnproven): its MVs outside
//     the strip are the culled view's, so the reader uses flow for that eye.
// [Views] ThreeViewsKey (vk:mods, default 0:0 = none): toggles the mode in flight (a high beep on, a low one off); an
// ini change of ThreeViews resets the toggle to the ini value. [Suite] ThreeViewsTiming=<pairs>: OFF / ON blocks with
// the GPU frame period (the layer's fill included: it follows the flag).
#pragma once

namespace tviews {

// ---------------------------------------------------------------------------
// The terrain chain's culling planes (render thread, DCS's immediate context)
// ---------------------------------------------------------------------------
namespace vstrip {
std::atomic<bool> g_arm{false};  // the current top-level pass is the right periphery's and the mode is engaged
uint64_t g_passSeq = 0;
float g_s = 0.55f;               // NDC x of the narrowed plane (render thread copy of tvc::g_stripNdc)
bool g_inOwn = false;
ID3D11Buffer *g_copy = nullptr, *g_out = nullptr, *g_diag = nullptr, *g_stDiag = nullptr;
ID3D11ShaderResourceView* g_copySrv = nullptr;
ID3D11UnorderedAccessView *g_outUav = nullptr, *g_diagUav = nullptr;
ID3D11ComputeShader* g_cs = nullptr;
float g_csS = -100.0f;
bool g_failed = false, g_stPending = false;
std::atomic<uint64_t> g_patches{0}, g_binds{0}, g_noTc{0}, g_skipIdent{0}, g_ok{0}, g_notOk{0}, g_otherView{0};
ID3D11Buffer* g_lastBuf = nullptr;
uint64_t g_lastSeq = ~0ull, g_sinceRead = 0;
// [Views] ThreeViewsIsolate bit 16 (crash bisect): the narrowed planes go into a constant buffer of ours, bound at the
// TerrainContext's slot in DCS's place, instead of being copied back into DCS's own buffer; DCS's buffer is bound again
// at the pass end. g_ownFor / g_ownSlot: what g_own stands in for (render thread).
ID3D11Buffer* g_own = nullptr;
ID3D11Buffer* g_ownFor = nullptr;
UINT g_ownSlot = 0;
std::atomic<uint64_t> g_ownBinds{0}, g_ownRestores{0}, g_cbForeign{0};
bool OwnMode() { return (tvc::g_isolate.load(std::memory_order_relaxed) & 16u) != 0; }
constexpr UINT kTcBytes = 2176;

// One thread: copies DCS's TerrainContext, finds among the culling planes (rows 28..33) the two matching the clip
// planes x >= -w and x <= w of the view-projection in rows 20..23, and replaces the first by x >= s w written as a
// combination of DCS's own two (DCS's frame, units and sign convention kept). Only a right-periphery buffer
// (projection row 16 = (sx, 0, cx, 0), sx < 1.5, cx < -0.02) whose plane is still DCS's own: a repeat is a no-op.
const char kSrc[] = R"(
ByteAddressBuffer tcIn : register(t0);
RWByteAddressBuffer tcOut : register(u0);
RWByteAddressBuffer dg : register(u1);
float4 R(uint r) { return asfloat(tcIn.Load4(r * 16)); }
void Match(float4 x, float4 w, out int il, out int ir, out float ql, out float qr) {
  float3 nl = normalize((w + x).xyz), nr = normalize((w - x).xyz);
  il = -1; ir = -1; ql = 0; qr = 0;
  for (int k = 0; k < 6; ++k) {
    float4 p = R(28 + k);
    float n = length(p.xyz);
    if (!(n > 0)) continue;
    float a = dot(p.xyz / n, nl), b = dot(p.xyz / n, nr);
    if (abs(a) > abs(ql)) { ql = a; il = k; }
    if (abs(b) > abs(qr)) { qr = b; ir = k; }
  }
}
[numthreads(1, 1, 1)] void main(uint3 gid : SV_GroupID) {
  if (gid.x != 0) return;  // dispatched as 2 groups: chain_fast.h's identification probes only 1x1x1 dispatches
  for (uint r = 0; r < 136; ++r) tcOut.Store4(r * 16, tcIn.Load4(r * 16));
  float4 x0 = R(20), w0 = R(23);
  float4 x1 = float4(R(20).x, R(21).x, R(22).x, R(23).x), w1 = float4(R(20).w, R(21).w, R(22).w, R(23).w);
  int il0, ir0, il1, ir1; float ql0, qr0, ql1, qr1;
  Match(x0, w0, il0, ir0, ql0, qr0);
  Match(x1, w1, il1, ir1, ql1, qr1);
  bool lay1 = min(abs(ql1), abs(qr1)) > min(abs(ql0), abs(qr0));
  float4 x = lay1 ? x1 : x0, w = lay1 ? w1 : w0;
  int il = lay1 ? il1 : il0, ir = lay1 ? ir1 : ir0;
  float ql = lay1 ? ql1 : ql0, qr = lay1 ? qr1 : qr0;
  uint ok = 0;
  float4 L = w + x, Rr = w - x;
  bool right = R(16).x < 1.5 && R(16).z < -0.02;
  if (!right) ok = 2;
  if (right && il >= 0 && ir >= 0 && il != ir && abs(ql) > 0.995 && abs(qr) > 0.995) {
    float4 Ld = R(28 + il), Rd = R(28 + ir);
    float4 L2 = sign(ql) * length(L.xyz) / length(Ld.xyz) * Ld;    // x >= -w in DCS's frame
    float4 R2 = sign(qr) * length(Rr.xyz) / length(Rd.xyz) * Rd;   // x <= w
    float4 S = (1 - S_NDC) * L2 - (1 + S_NDC) * R2;               // 2 (x - s w)
    float4 Sd = sign(ql) * S * (length(Ld.xyz) / length(S.xyz));
    tcOut.Store4((28 + il) * 16, asuint(Sd));
    ok = 1;
  }
  dg.Store4(0, uint4(ok, (uint)(il + 1), (uint)(ir + 1), lay1 ? 1u : 0u));
}
)";

bool Ensure(ID3D11DeviceContext* c) {
  if (g_failed) return false;
  if (g_cs && g_csS == g_s) return true;
  ID3D11Device* dev = nullptr;
  c->GetDevice(&dev);
  if (!dev) return g_failed = true, false;
  bool ok = true;
  if (!g_copy) {
    D3D11_BUFFER_DESC d{kTcBytes, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, 0,
                        D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS, 0};
    ok = ok && SUCCEEDED(dev->CreateBuffer(&d, nullptr, &g_copy));
    d.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
    ok = ok && SUCCEEDED(dev->CreateBuffer(&d, nullptr, &g_out));
    D3D11_BUFFER_DESC cd{kTcBytes, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0};
    ok = ok && SUCCEEDED(dev->CreateBuffer(&cd, nullptr, &g_own));
    D3D11_BUFFER_DESC dd{256, D3D11_USAGE_DEFAULT, D3D11_BIND_UNORDERED_ACCESS, 0,
                         D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS, 0};
    ok = ok && SUCCEEDED(dev->CreateBuffer(&dd, nullptr, &g_diag));
    D3D11_BUFFER_DESC sd{256, D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ, 0, 0};
    ok = ok && SUCCEEDED(dev->CreateBuffer(&sd, nullptr, &g_stDiag));
    if (ok) {
      D3D11_SHADER_RESOURCE_VIEW_DESC sv{};
      sv.Format = DXGI_FORMAT_R32_TYPELESS;
      sv.ViewDimension = D3D11_SRV_DIMENSION_BUFFEREX;
      sv.BufferEx.NumElements = kTcBytes / 4;
      sv.BufferEx.Flags = D3D11_BUFFEREX_SRV_FLAG_RAW;
      ok = SUCCEEDED(dev->CreateShaderResourceView(g_copy, &sv, &g_copySrv));
      D3D11_UNORDERED_ACCESS_VIEW_DESC uv{};
      uv.Format = DXGI_FORMAT_R32_TYPELESS;
      uv.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
      uv.Buffer.NumElements = kTcBytes / 4;
      uv.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
      ok = ok && SUCCEEDED(dev->CreateUnorderedAccessView(g_out, &uv, &g_outUav));
      uv.Buffer.NumElements = 64;
      ok = ok && SUCCEEDED(dev->CreateUnorderedAccessView(g_diag, &uv, &g_diagUav));
    }
  }
  if (ok) {
    char src[sizeof(kSrc) + 64];
    snprintf(src, sizeof(src), "#define S_NDC %.6f\n%s", g_s, kSrc);
    std::vector<uint8_t> code;
    std::string err;
    if (chainfast::Compile(src, "three_views_strip", code, &err)) {
      if (g_cs) g_cs->Release(), g_cs = nullptr;
      ok = SUCCEEDED(dev->CreateComputeShader(code.data(), code.size(), nullptr, &g_cs));
      g_csS = g_s;
      Log("three views: terrain chain kernel compiled, culling plane at NDC x %.3f (%s)", g_s, ok ? "ok" : "create failed");
    } else {
      Log("three views: terrain chain kernel did not compile: %s", err.c_str());
      ok = false;
    }
  }
  dev->Release();
  if (!ok) {
    g_failed = true;
    Log("three views: terrain chain resources failed; the right periphery's terrain chain is not narrowed");
  }
  return ok;
}

void ReadDiag(ID3D11DeviceContext* c) {
  if (!g_stPending) return;
  D3D11_MAPPED_SUBRESOURCE m{};
  if (c->Map(g_stDiag, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m) != S_OK) return;
  const uint32_t ok = static_cast<const uint32_t*>(m.pData)[0];
  if (ok == 1) g_ok.fetch_add(1, std::memory_order_relaxed);
  else if (ok == 2) g_otherView.fetch_add(1, std::memory_order_relaxed);
  else g_notOk.fetch_add(1, std::memory_order_relaxed);
  c->Unmap(g_stDiag, 0);
  g_stPending = false;
}

bool IsTc(ID3D11Buffer* b) {
  if (!b) return false;
  D3D11_BUFFER_DESC d{};
  b->GetDesc(&d);
  return d.ByteWidth == kTcBytes && (d.BindFlags & D3D11_BIND_CONSTANT_BUFFER) && d.Usage == D3D11_USAGE_DEFAULT;
}
using CbFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, ID3D11Buffer* const*);
CbFn CbOrig(ID3D11DeviceContext* c);
// Binds b at CS slot `slot` past our own CSSetConstantBuffers entry (DCS's d3d11 function, or the table's when ours
// is not on it).
void BindCb(ID3D11DeviceContext* c, UINT slot, ID3D11Buffer* b) {
  const bool was = g_inOwn;
  g_inOwn = true;
  if (const CbFn o = CbOrig(c)) o(c, slot, 1, &b);
  else c->CSSetConstantBuffers(slot, 1, &b);
  g_inOwn = was;
}
// Copy, our kernel, copy back (or, own mode, into our buffer bound in DCS's place); DCS's compute state put back.
void PatchBuf(ID3D11DeviceContext* c, ID3D11Buffer* tc, UINT slot) {
  if (chainfast::g_fpLive.load(std::memory_order_relaxed)) {  // a fingerprint job owns the render thread's CS state
    g_skipIdent.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  if (!Ensure(c)) return;
  g_inOwn = true;
  c->CopyResource(g_copy, tc);
  ID3D11ComputeShader* pcs = nullptr;
  ID3D11ClassInstance* pci[8] = {};
  UINT nci = 8;
  c->CSGetShader(&pcs, pci, &nci);
  ID3D11ShaderResourceView* psrv = nullptr;
  c->CSGetShaderResources(0, 1, &psrv);
  ID3D11UnorderedAccessView* puav[2] = {};
  c->CSGetUnorderedAccessViews(0, 2, puav);
  chainfast::g_setOrig(c, g_cs, nullptr, 0);
  c->CSSetShaderResources(0, 1, &g_copySrv);
  ID3D11UnorderedAccessView* u[2] = {g_outUav, g_diagUav};
  c->CSSetUnorderedAccessViews(0, 2, u, nullptr);
  sfast::t_inside = true;  // shader_fast.h's Dispatch hook leaves this one alone
  c->Dispatch(2, 1, 1);
  sfast::t_inside = false;
  const UINT keep[2] = {~0u, ~0u};
  c->CSSetUnorderedAccessViews(0, 2, puav, keep);
  c->CSSetShaderResources(0, 1, &psrv);
  chainfast::g_setOrig(c, pcs, nci ? pci : nullptr, nci);
  const bool own = OwnMode() && g_own;
  if (own) {
    c->CopyResource(g_own, g_out);
    BindCb(c, slot, g_own);
    g_ownFor = tc, g_ownSlot = slot;
    g_ownBinds.fetch_add(1, std::memory_order_relaxed);
  } else {
    c->CopyResource(tc, g_out);
  }
  if (!g_stPending && ++g_sinceRead >= 31) c->CopyResource(g_stDiag, g_diag), g_stPending = true, g_sinceRead = 0;
  ReadDiag(c);
  for (ID3D11UnorderedAccessView* v : puav)
    if (v) v->Release();
  if (psrv) psrv->Release();
  for (UINT i = 0; i < nci && i < 8; ++i)
    if (pci[i]) pci[i]->Release();
  if (pcs) pcs->Release();
  g_inOwn = false;
  g_lastBuf = tc, g_lastSeq = g_passSeq;
  g_patches.fetch_add(1, std::memory_order_relaxed);
}

// chain_fast.h's CSSetShader hook, before DCS's bind: the TerrainContext bound at CS b11, once per pass and buffer.
void OnCsSet(ID3D11DeviceContext* c, ID3D11ComputeShader*) {
  if (g_inOwn || !g_arm.load(std::memory_order_relaxed)) return;
  g_binds.fetch_add(1, std::memory_order_relaxed);
  ID3D11Buffer* b = nullptr;
  c->CSGetConstantBuffers(11, 1, &b);
  if (b && b == g_own) {  // own mode: ours already stands in for DCS's
    b->Release();
    return;
  }
  if (!IsTc(b)) {
    g_noTc.fetch_add(1, std::memory_order_relaxed);
    if (b) b->Release();
    return;
  }
  if (b != g_lastBuf || g_lastSeq != g_passSeq) PatchBuf(c, b, 11);
  b->Release();
}

// Our CSSetConstantBuffers entry on DCS's immediate context's table (armed at each right-periphery pass begin: d3d11
// rewrites the slot): a TerrainContext DCS binds there is patched after its bind, before its dispatch.
void STDMETHODCALLTYPE H_CSSetConstantBuffers(ID3D11DeviceContext* c, UINT s, UINT n, ID3D11Buffer* const* b);
struct CbT { void** vt; void* orig; };
CbT g_cbt[8];
std::atomic<int> g_ncbt{0};
int g_cbSlot = -1;
std::atomic<uint64_t> g_cbRefused{0}, g_cbBinds{0};
CbFn CbOrig(ID3D11DeviceContext* c) {
  void** vt = *reinterpret_cast<void***>(c);
  const int n = g_ncbt.load(std::memory_order_acquire);
  for (int i = 0; i < n; ++i)
    if (g_cbt[i].vt == vt) return reinterpret_cast<CbFn>(g_cbt[i].orig);
  // A table we never hooked carries our entry (d3d11 copied it): another table's d3d11 function stands in. Counted:
  // a non-zero count in the status line is a lead for the switch-on crash.
  for (int i = 0; i < n; ++i)
    if (g_cbt[i].orig != reinterpret_cast<void*>(&H_CSSetConstantBuffers)) {
      g_cbForeign.fetch_add(1, std::memory_order_relaxed);
      return reinterpret_cast<CbFn>(g_cbt[i].orig);
    }
  return nullptr;
}
void STDMETHODCALLTYPE H_CSSetConstantBuffers(ID3D11DeviceContext* c, UINT s, UINT n, ID3D11Buffer* const* b) {
  const CbFn o = CbOrig(c);
  if (o) o(c, s, n, b);
  if (g_inOwn || !g_arm.load(std::memory_order_relaxed) || c != chainfast::g_ctx || !b) return;
  g_cbBinds.fetch_add(1, std::memory_order_relaxed);
  for (UINT i = 0; i < n && i < 16; ++i)
    if (b[i] != g_own && IsTc(b[i])) PatchBuf(c, b[i], s + i);
}
bool EnsureCb(ID3D11DeviceContext* c) {
  if (!c) return false;
  if (g_cbSlot < 0) g_cbSlot = DcsQv_ContextSlot("CSSetConstantBuffers");
  if (g_cbSlot < 0) return false;
  void** vt = *reinterpret_cast<void***>(c);
  void* cur = vt[g_cbSlot];
  if (cur == reinterpret_cast<void*>(&H_CSSetConstantBuffers)) return true;
  if (d3ds::InPayload(cur) || !fixdepth::InD3d11(cur)) {
    g_cbRefused.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  const int n = g_ncbt.load();
  int i = 0;
  for (; i < n; ++i)
    if (g_cbt[i].vt == vt) break;
  if (i == n) {
    if (n >= 8) return false;
    g_cbt[n].vt = vt;
    g_ncbt.store(n + 1, std::memory_order_release);
  }
  g_cbt[i].orig = cur;
  d3ds::WriteTablePointer(&vt[g_cbSlot], reinterpret_cast<void*>(&H_CSSetConstantBuffers));
  return true;
}
// Render thread or after the render thread stopped calling us (g_arm false first).
// Own mode, render thread at the pass end (and before a disengage): DCS's buffer back at its slot if ours is there.
void RestoreOwn(ID3D11DeviceContext* c) {
  if (!g_ownFor || !c) return;
  ID3D11Buffer* cur = nullptr;
  c->CSGetConstantBuffers(g_ownSlot, 1, &cur);
  if (cur == g_own) {
    BindCb(c, g_ownSlot, g_ownFor);
    g_ownRestores.fetch_add(1, std::memory_order_relaxed);
  }
  if (cur) cur->Release();
  g_ownFor = nullptr;
}
void UninstallCb() {
  for (int i = 0; i < g_ncbt.load(); ++i)
    if (g_cbSlot >= 0 && g_cbt[i].vt[g_cbSlot] == reinterpret_cast<void*>(&H_CSSetConstantBuffers))
      d3ds::WriteTablePointer(&g_cbt[i].vt[g_cbSlot], g_cbt[i].orig);
}
// Our entry is on one of the tables we hooked.
bool CbOnTable() {
  for (int i = 0; i < g_ncbt.load(); ++i)
    if (g_cbSlot >= 0 && g_cbt[i].vt[g_cbSlot] == reinterpret_cast<void*>(&H_CSSetConstantBuffers)) return true;
  return false;
}
}  // namespace vstrip

// ---------------------------------------------------------------------------
// Fixation depth's pass boundary (render thread)
// ---------------------------------------------------------------------------
std::atomic<bool> g_cbWanted{false};  // worker: the CSSetConstantBuffers entry should be on the table
void OnPass(bool begin, bool inRight) {
  ++vstrip::g_passSeq;
  const bool arm = begin && inRight && tvc::g_engaged.load(std::memory_order_relaxed) &&
                   !(tvc::g_isolate.load(std::memory_order_relaxed) & 4u);  // [Views] ThreeViewsIsolate bit 4
  if (arm) {
    vstrip::g_s = static_cast<float>(tvc::g_stripNdc.load(std::memory_order_relaxed));
    vstrip::EnsureCb(chainfast::g_ctx);
  }
  if (!begin || !arm) vstrip::RestoreOwn(chainfast::g_ctx);  // own mode: DCS's TerrainContext bound again
  vstrip::g_arm.store(arm, std::memory_order_relaxed);
  if (!begin && !g_cbWanted.load(std::memory_order_relaxed) && vstrip::g_ncbt.load(std::memory_order_relaxed))
    vstrip::UninstallCb();  // disengaged: our entry off the table on the thread that uses it
}

// ---------------------------------------------------------------------------
// Worker: configuration, hotkey, engage / disengage
// ---------------------------------------------------------------------------
std::atomic<int> g_suiteForce{-1};  // [Suite] ThreeViewsTiming: -1 none, 0 off, 1 on
bool g_userOn = false;              // the hotkey's state (reset to the ini value when the key changes)
int g_lastCfg = -1;
std::string g_lastWhy;

// Worker, every loop: decides tvc::g_engaged and installs / removes the render-thread parts.
void Tick(bool cfgOn, double stripNdc, int keyVk, int keyMods, bool engineOff, bool fixInstalled,
          bool keyDown) {
  (void)keyMods;  // the caller matched the modifiers (keyDown)
  tvc::g_cfgOn = cfgOn;
  tvc::g_stripNdc = Clamp(stripNdc, 0.2, 0.9);
  tvc::g_onPass = cfgOn ? &OnPass : nullptr;
  if (static_cast<int>(cfgOn) != g_lastCfg) {
    g_lastCfg = cfgOn ? 1 : 0;
    g_userOn = cfgOn;
  }
  static bool kPrev = false;
  if (cfgOn && keyVk > 0 && keyDown && !kPrev) {
    g_userOn = !g_userOn;
    Log("hotkey: three views %s", g_userOn ? "ON" : "OFF");
    if (g_userOn)
      Chime(1400, 90);
    else
      Chime(450, 120);
  }
  kPrev = keyDown;
  const uint32_t mask = tvc::g_layerMask.load(std::memory_order_relaxed);
  const int force = g_suiteForce.load();
  const bool want = force >= 0 ? force == 1 : g_userOn;
  std::string why;
  if (!cfgOn) why = "[Views] ThreeViews=0";
  else if (!want) why = force >= 0 ? "timing OFF block" : "hotkey OFF";
  else if (engineOff) why = "engine optimizations off (kill switch)";
  else if (!fixInstalled) why = "fixation depth's pass hooks not installed";
  else if ((mask & 3u) != 3u) why = "the Quad Views layer does not ask for the periphery views (not a three-views build)";
  const bool on = why.empty();
  if (on != tvc::g_engaged.load()) {
    // W1's per-view gate learnt the right periphery while it drew only its strip (or the whole view): it tests again.
    w1::g_rearms.fetch_add(1, std::memory_order_relaxed);
    if (on) {
      if (chainfast::g_onSetCensus.load() == nullptr) chainfast::g_onSetCensus = &vstrip::OnCsSet;
      tvc::g_logLeft = 2;
      g_cbWanted = true;
      tvc::g_engaged = true;
      Log("three views: engaged (right periphery = strip NDC x >= %.3f + cockpit, the rest filled by Quad Views from "
          "the left periphery; terrain chain narrowing: %s)",
          tvc::g_stripNdc.load(),
          !chainfast::g_ctx ? "OFF, ChainFast's context hook absent (only objects and terrain patches are culled)"
          : chainfast::g_onSetCensus.load() == &vstrip::OnCsSet ? "on"
                                                                : "constant-buffer binds only (the CS bind callback is another feature's)");
    } else {
      tvc::g_engaged = false;
      g_cbWanted = false;
      vstrip::g_arm = false;
      auto me = &vstrip::OnCsSet;
      chainfast::g_onSetCensus.compare_exchange_strong(me, nullptr);
      Log("three views: disengaged (%s)", why.c_str());
    }
    g_lastWhy = why;
  } else if (!on && why != g_lastWhy && force < 0) {
    Log("three views: not engaged (%s)", why.c_str());
    g_lastWhy = why;
  }
  // [Views] ThreeViews set to 0: the pass callback is out from this tick, so the render thread never takes our
  // CSSetConstantBuffers entry off the table (OnPass does that at a pass end only while the key is on). It goes here
  // one tick later (the worker loop sleeps 50-100 ms: a pass boundary that loaded the callback and saw the mode
  // engaged has long finished); the caller keeps calling while the entry is on (CbOnTable).
  static bool offSeen = false;
  if (!cfgOn) {
    if (offSeen && vstrip::CbOnTable()) {
      vstrip::UninstallCb();
      Log("three views: constant-buffer entry off DCS's context table ([Views] ThreeViews=0)");
    }
    offSeen = true;
  } else {
    offSeen = false;
  }
}

// Payload stop (before fixdepth::Shutdown and chainfast::Shutdown): disengaged, our table entry and callback out,
// the kernel's objects released.
void Shutdown() {
  tvc::g_engaged = false;
  g_cbWanted = false;
  tvc::g_onPass = nullptr;
  vstrip::g_arm = false;
  auto me = &vstrip::OnCsSet;
  chainfast::g_onSetCensus.compare_exchange_strong(me, nullptr);
  Sleep(50);  // a bind that loaded the pointers before
  vstrip::UninstallCb();
  for (IUnknown** p : {reinterpret_cast<IUnknown**>(&vstrip::g_copySrv), reinterpret_cast<IUnknown**>(&vstrip::g_outUav),
                       reinterpret_cast<IUnknown**>(&vstrip::g_diagUav), reinterpret_cast<IUnknown**>(&vstrip::g_copy),
                       reinterpret_cast<IUnknown**>(&vstrip::g_out), reinterpret_cast<IUnknown**>(&vstrip::g_diag),
                       reinterpret_cast<IUnknown**>(&vstrip::g_stDiag), reinterpret_cast<IUnknown**>(&vstrip::g_cs), reinterpret_cast<IUnknown**>(&vstrip::g_own)})
    if (*p) (*p)->Release(), *p = nullptr;
}

// The status line (worker, every few seconds while configured).
void LogStatus() {
  Log("three views: %s; collect: strip volumes %llu, refused %llu, cockpit collections kept %llu; terrain chain: "
      "patches %llu (binds %llu, CB binds %llu, no TerrainContext %llu, during identification %llu, diag ok %llu / "
      "not matched %llu / other view %llu, kernel %s, own buffer binds %llu restores %llu, foreign table %llu); periphery depth: copies %llu, pushes %llu, accepted %llu, "
      "rejected %llu (last result %u), flagged %llu, after water %llu, skipped arm %llu identity %llu format %llu proj "
      "%llu; Quad Views wants 0x%x",
      tvc::g_engaged.load() ? "ENGAGED" : "off", (unsigned long long)tvc::g_patched.load(),
      (unsigned long long)tvc::g_refused.load(), (unsigned long long)tvc::g_ckKept.load(),
      (unsigned long long)vstrip::g_patches.load(), (unsigned long long)vstrip::g_binds.load(),
      (unsigned long long)vstrip::g_cbBinds.load(), (unsigned long long)vstrip::g_noTc.load(),
      (unsigned long long)vstrip::g_skipIdent.load(), (unsigned long long)vstrip::g_ok.load(),
      (unsigned long long)vstrip::g_notOk.load(), (unsigned long long)vstrip::g_otherView.load(),
      vstrip::g_failed ? "FAILED" : (vstrip::g_cs ? "ok" : "not built"), (unsigned long long)vstrip::g_ownBinds.load(),
      (unsigned long long)vstrip::g_ownRestores.load(), (unsigned long long)vstrip::g_cbForeign.load(),
      (unsigned long long)fixdepth::tvexp::g_copies.load(), (unsigned long long)fixdepth::tvexp::g_pushes.load(),
      (unsigned long long)fixdepth::tvexp::g_accepted.load(), (unsigned long long)fixdepth::tvexp::g_rejected.load(),
      fixdepth::tvexp::g_lastResult.load(), (unsigned long long)fixdepth::tvexp::g_flagged.load(),
      (unsigned long long)fixdepth::tvexp::g_afterWater.load(), (unsigned long long)fixdepth::tvexp::g_skipArm.load(),
      (unsigned long long)fixdepth::tvexp::g_skipIdentity.load(), (unsigned long long)fixdepth::tvexp::g_skipFormat.load(),
      (unsigned long long)fixdepth::tvexp::g_skipProj.load(), tvc::g_layerMask.load());
  // The fill inside the engine (option C): per periphery TransparentCockpitPass, left copies and right fills.
  Log("three views: engine fill: left copies %llu, right fills %llu, not run %llu, no left copy %llu, target not float %llu, "
      "left without motion %llu; right view not culled (DCS's own, not flagged) %llu; pushes deferred after a copy texture "
      "was made %llu, idle layer releases %llu, post-transparent copies left out %llu, after the glass %llu / later %llu; "
      "copy textures made %llu, released %llu; constant-buffer entry refused %llu (isolate 0x%x)",
      (unsigned long long)fixdepth::tvexp::g_leftCopies.load(), (unsigned long long)fixdepth::tvexp::g_fills.load(),
      (unsigned long long)fixdepth::tvexp::g_fillSkips.load(), (unsigned long long)fixdepth::tvexp::g_noLeft.load(),
      (unsigned long long)fixdepth::tvexp::g_badTarget.load(), (unsigned long long)fixdepth::tvexp::g_noMv.load(),
      (unsigned long long)fixdepth::tvexp::g_notCulled.load(), (unsigned long long)fixdepth::tvexp::g_deferredPushes.load(),
      (unsigned long long)fixdepth::tvexp::g_idleReleases.load(), (unsigned long long)fixdepth::tvexp::g_postSkipped.load(),
      (unsigned long long)fixdepth::tvexp::g_postCopies.load(), (unsigned long long)fixdepth::tvexp::g_postFallback.load(),
      (unsigned long long)fixdepth::tvexp::g_texCreates.load(), (unsigned long long)fixdepth::tvexp::g_releases.load(),
      (unsigned long long)vstrip::g_cbRefused.load(), tvc::g_isolate.load());
}

// ---------------------------------------------------------------------------
// [Suite] ThreeViewsTiming: OFF / ON blocks (suite thread)
// ---------------------------------------------------------------------------
struct Block {
  bool on = false, engaged = false;
  uint64_t frames = 0;
  double periodMs = 0, busyMs = 0, rtCpuMs = 0;
  uint64_t flagged = 0;
};
std::string NoName(void*) { return std::string(); }

Block MeasureBlock(bool on, int frames, std::atomic<uint64_t>& fc, double tscHz) {
  Block b;
  b.on = on;
  gpt::TakeDone();
  const DWORD tid = ptiming::g_topTid.load();
  const uint64_t fl0 = fixdepth::tvexp::g_flagged.load();
  const uint64_t c0 = w1::RenderThreadCycles(tid), f0 = fc.load();
  w1::WaitFrames(fc, static_cast<uint64_t>(frames), 20000);
  const uint64_t c1 = w1::RenderThreadCycles(tid), f1 = fc.load();
  b.engaged = tvc::g_engaged.load();
  b.flagged = fixdepth::tvexp::g_flagged.load() - fl0;
  Sleep(5);
  b.frames = f1 - f0;
  const double f = b.frames ? static_cast<double>(b.frames) : 1.0;
  b.rtCpuMs = tscHz > 0 && c1 > c0 ? (c1 - c0) / tscHz * 1000.0 / f : -1;
  Sleep(60);
  std::vector<gpt::Done> done = gpt::TakeDone();
  gpt::Result r = gpt::Analyze(done, &NoName);
  double busy = 0;
  int nb = 0;
  for (const gpt::Done& d : done) {
    double span = 0, bz = 0, xr = 0;
    if (gpt::FrameGpu(d, &span, &bz, &xr)) busy += bz, ++nb;
  }
  b.periodMs = r.period.empty() ? -1 : gpt::Mean(r.period);
  b.busyMs = nb ? busy / nb : -1;
  return b;
}

void TimingPhase(int pairs, int frames, std::atomic<uint64_t>& fc, double tscHz, const std::wstring& csvDir) {
  if (!tvc::g_cfgOn.load()) {
    Log("  three views timing: [Views] ThreeViews=0; phase skipped");
    return;
  }
  if (!gpt::Begin(fc, true)) {
    gpt::End();
    Log("  three views timing: GPU timing did not start; phase skipped");
    return;
  }
  std::vector<Block> blocks;
  const int settle = 20;
  for (int i = 0; i < 2 * pairs + 1; ++i) {
    const bool on = (i & 1) != 0;
    g_suiteForce = on ? 1 : 0;
    if (!w1::WaitFrames(fc, settle, 10000)) break;
    blocks.push_back(MeasureBlock(on, frames, fc, tscHz));
  }
  g_suiteForce = -1;
  gpt::End();
  struct D {
    std::vector<double> v;
    double Mean() const { double s = 0; for (double x : v) s += x; return v.empty() ? 0 : s / v.size(); }
    double Se() const {
      if (v.size() < 2) return 0;
      const double m = Mean();
      double s = 0;
      for (double x : v) s += (x - m) * (x - m);
      return std::sqrt(s / (v.size() - 1) / v.size());
    }
  } dP, dB, dC;
  double offP = 0, offC = 0;
  int nOff = 0, onEngaged = 0, nOn = 0, offEngaged = 0;
  for (size_t i = 1; i + 1 < blocks.size(); i += 2) {
    const Block &a = blocks[i - 1], &b = blocks[i], &c = blocks[i + 1];
    dP.v.push_back(b.periodMs - (a.periodMs + c.periodMs) / 2);
    dB.v.push_back(b.busyMs - (a.busyMs + c.busyMs) / 2);
    dC.v.push_back(b.rtCpuMs - (a.rtCpuMs + c.rtCpuMs) / 2);
  }
  for (const Block& b : blocks) {
    if (b.on) {
      ++nOn, onEngaged += b.engaged ? 1 : 0;
      continue;
    }
    offP += b.periodMs, offC += b.rtCpuMs, ++nOff, offEngaged += b.engaged ? 1 : 0;
  }
  if (nOff) offP /= nOff, offC /= nOff;
  SYSTEMTIME st;
  GetLocalTime(&st);
  wchar_t name[96];
  swprintf_s(name, L"three_views_timing_%04d%02d%02d_%02d%02d%02d.csv", st.wYear, st.wMonth, st.wDay, st.wHour,
             st.wMinute, st.wSecond);
  FILE* csv = nullptr;
  _wfopen_s(&csv, (csvDir + name).c_str(), L"w");
  if (csv) {
    fprintf(csv, "block,on,engaged,frames,gpu_period_ms,gpu_busy_ms,rt_cpu_ms,flagged_pushes\n");
    for (size_t i = 0; i < blocks.size(); ++i) {
      const Block& b = blocks[i];
      fprintf(csv, "%zu,%d,%d,%llu,%.4f,%.4f,%.4f,%llu\n", i, b.on ? 1 : 0, b.engaged ? 1 : 0,
              static_cast<unsigned long long>(b.frames), b.periodMs, b.busyMs, b.rtCpuMs,
              static_cast<unsigned long long>(b.flagged));
    }
    fclose(csv);
  }
  Log("  three views timing: %zu pairs, %d frames per block, ON blocks engaged %d of %d (OFF blocks engaged %d); ON minus "
      "neighbouring OFF (+- standard error): GPU frame period %+.3f +- %.3f ms (OFF %.3f), GPU busy in passes %+.3f +- "
      "%.3f ms; render thread CPU %+.3f +- %.3f ms/frame (OFF %.3f); blocks in %ls",
      dP.v.size(), frames, onEngaged, nOn, offEngaged, dP.Mean(), dP.Se(), offP, dB.Mean(), dB.Se(), dC.Mean(), dC.Se(),
      offC, name);
}

}  // namespace tviews
