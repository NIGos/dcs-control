// R74b: cockpit mask v1.1, value 128 = overlay symbology over the world ([Perception] CockpitMaskOverlay, default 0;
// contract: dcsvr_cockpit_mask.h "Overlay symbology"; design and live check: docs/research/R74_content_time_and_mask_v11.md).
//
// DCS draws the HUD and the other cockpit indicators after DLSS, per HMD view, in OverlayHDRPass (HUD_MFD_after_DLSS;
// R32 §2.1, R41 §3) into the view's HDR target at output resolution. Per configured view whose mask this frame was
// computed (its sample point came before), on DCS's render thread:
//  1. Learn (first frame, and whenever the target or its viewport changes). While the view's OverlayHDRPass runs, the
//     in-pass OM arm (fixation_depth.h ArmOm, as far depth's overlay arm; cockpit ipd's overlay arm nests in it) notes
//     the pass's first render target 0 (AddRef'd) and the first viewport after it (fixation_depth.h CmOvlBind /
//     CmOvlViewport). At the pass end they are kept as the view's target (an SRV on it) and box, when single-sample,
//     shader-readable, and with the aspect of the mask view's source (DLSS: output = render x scale).
//  2. Pre (the pass begin, before any arm of this boundary): one thread per mask texel of the view's rect writes a
//     30-bit signature (FNV-1a over the raw bits of every HDR texel of the texel's footprint in the output box) and
//     whether the mask texel is cockpit (bit 31) into our R32_UINT scratch atlas; the view's change counter is reset.
//  3. Post (the pass end, the same target and box bound in this pass): the signature again; a change sets bit 30 and
//     counts world texels. Then Apply: world texels with a change get 128 in the mask slot (cockpit stays 255), unless
//     changed world texels exceed 40 % of the view's rect (a full-screen pass or a switched target: the view gets no 128
//     this frame). The view gets view_flags::overlay; the slot slot_flags::overlay.
//     R76: also unless they exceed 40 % of the view's WORLD texels (counted in Post): with the cockpit covering 60 % or
//     more of the rect (F1, head down) a full-screen change touched at most 40 % of the rect and was never rejected.
// Our dispatches save and put back every CS binding they touch (shader, CB 0-1 with *1 ranges, SRV 0-1, UAV 0-2) and,
// when the HDR target is bound as a render target, the OM targets (unbound for the dispatch, rebound after).
// Cost: two reads of the view's HDR box (~8.5 MB at 1040 x 1028 RGBA16F) and three small dispatches per view; GPU
// timestamps per dispatch in the 30 s line, the change counts read back (never waiting) for the coverage figures.
// Included from cockpit_mask.h inside namespace fixdepth::cmask (after the frame state).
#pragma once

namespace covl {

std::atomic<bool> g_on{false};  // [Perception] CockpitMaskOverlay (opt-in until a lit-HUD live check)
constexpr float kMaxFrac = 0.4f;
constexpr int kTs = 4;  // timing / count frames in flight

const char kShader[] = R"(
cbuffer P : register(b0) {
  uint4 rect;  // the view's rect in the mask atlas: x, y, w, h
  uint4 src;   // srcW, srcH, divisor, view
  uint4 box;   // the HDR box: x, y, w, h (output texels)
  uint4 misc;  // x: limit (world texels that may change, of the rect); z: percent of the view's world texels (R76)
};
#if OV_STAGE == 0
Texture2D<float> maskT : register(t0);
#endif
#if OV_STAGE <= 1
#if OV_ARRAY
Texture2DArray<float4> hdr : register(t1);
#else
Texture2D<float4> hdr : register(t1);
#endif
#endif
RWTexture2D<uint> scratch : register(u0);
RWBuffer<uint> cnt : register(u1);
#if OV_STAGE == 2
RWTexture2D<unorm float> maskU : register(u2);
#endif

#if OV_STAGE <= 1
uint Sig(uint2 m) {
  uint x0 = box.x + (m.x * src.z * box.z) / src.x;
  uint x1 = box.x + (min((m.x + 1) * src.z, src.x) * box.z) / src.x;
  uint y0 = box.y + (m.y * src.z * box.w) / src.y;
  uint y1 = box.y + (min((m.y + 1) * src.z, src.y) * box.w) / src.y;
  x1 = max(x1, x0 + 1);
  y1 = max(y1, y0 + 1);
  uint h = 2166136261u;
  for (uint y = y0; y < y1; ++y)
    for (uint x = x0; x < x1; ++x) {
#if OV_ARRAY
      uint4 u = asuint(hdr.Load(int4(x, y, 0, 0)));  // the SRV starts at the target's slice
#else
      uint4 u = asuint(hdr.Load(int3(x, y, 0)));
#endif
      h = (h ^ u.x) * 16777619u;
      h = (h ^ u.y) * 16777619u;
      h = (h ^ u.z) * 16777619u;
      h = (h ^ u.w) * 16777619u;
    }
  return h & 0x3fffffffu;
}
#endif

#if OV_STAGE == 1
groupshared uint gWorld;
#endif

// cnt: [v] changed world texels of view v, [4 + v] its world texels (R76; both reset in stage 0).
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID, uint gi : SV_GroupIndex) {
#if OV_STAGE == 1
  // R76: the view's world texels counted too (one atomic per group): the bail is relative to them.
  if (gi == 0) gWorld = 0;
  GroupMemoryBarrierWithGroupSync();
  if (id.x < rect.z && id.y < rect.w) {
    uint2 a = rect.xy + id.xy;
    uint s = scratch[a];
    if (!(s & 0x80000000u)) {  // cockpit: stays 255, not read
      InterlockedAdd(gWorld, 1u);
      if ((s & 0x3fffffffu) != Sig(id.xy)) {
        scratch[a] = s | 0x40000000u;
        InterlockedAdd(cnt[src.w], 1u);
      }
    }
  }
  GroupMemoryBarrierWithGroupSync();
  if (gi == 0 && gWorld) InterlockedAdd(cnt[4u + src.w], gWorld);
#else
#if OV_STAGE == 0
  if (id.x == 0 && id.y == 0) {
    cnt[src.w] = 0;
    cnt[4u + src.w] = 0;
  }
#endif
  if (id.x >= rect.z || id.y >= rect.w) return;
  uint2 a = rect.xy + id.xy;
#if OV_STAGE == 0
  // Cockpit texels never become 128: no signature (R74 live: about 45 % of a periphery rect, that share of the reads).
  scratch[a] = maskT.Load(int3(a, 0)) > 0.25 ? 0x80000000u : Sig(id.xy);
#else
  uint s = scratch[a];
  uint changed = cnt[src.w];
  if ((s & 0xc0000000u) == 0x40000000u && changed <= misc.x && changed * 100u <= cnt[4u + src.w] * misc.z)
    maskU[a] = 128.0 / 255.0;
#endif
#endif
}
)";

struct Learnt {
  ID3D11Resource* tex = nullptr;          // DCS's HDR target (reference)
  ID3D11ShaderResourceView* srv = nullptr;
  bool array = false;
  UINT slice = 0;
  D3D11_VIEWPORT vp{};
  uint32_t srcW = 0, srcH = 0;
  bool ok = false;
};
struct TsFrame {
  ID3D11Query* dj = nullptr;
  ID3D11Query* ts[4][4] = {};  // per view: pre begin / end, post begin / end (post includes apply)
  bool used[4][2] = {};
  bool open = false, pending = false;
  ID3D11Buffer* cnt = nullptr;  // staging copy of the counters (4 uint)
  bool cntPending = false;
  uint32_t views = 0, area[4] = {}, limit[4] = {}, pct = 40;  // pct: R76, of the world texels
};
struct ResOv {
  ID3D11ComputeShader* cs[5] = {};  // pre, pre array, post, post array, apply
  ID3D11Buffer* cb[4] = {};
  ID3D11Texture2D* scratch = nullptr;
  ID3D11UnorderedAccessView* scratchUav = nullptr;
  UINT sW = 0, sH = 0;
  ID3D11Buffer* cnt = nullptr;
  ID3D11UnorderedAccessView* cntUav = nullptr;
  ID3D11ShaderResourceView* maskSrv[kSlots] = {};
  ID3D11Texture2D* maskSrvOf[kSlots] = {};  // identity: the slot texture the SRV was made for
  TsFrame tf[kTs];
  int tfCur = -1, tfNext = 0;
  bool failed = false;
};
ResOv g_o;
Learnt g_l[4];
std::vector<uint8_t> g_blob[5];
std::atomic<bool> g_blobsOk{false};

std::atomic<uint64_t> g_pre{0}, g_post{0}, g_applied{0}, g_learns{0}, g_noLearn{0}, g_aspect{0}, g_noArm{0},
    g_mismatch{0}, g_rejected{0}, g_viewsCovered{0}, g_noMask{0}, g_frames{0};
double g_covSum[4] = {}, g_gpuSum = 0;
uint64_t g_covN[4] = {}, g_gpuN = 0;
double g_lastCov[4] = {};
std::mutex g_statMu;

bool Wanted() { return g_on.load(std::memory_order_relaxed) && Active() && !g_idle; }

bool CompileOv() {
  if (g_blobsOk.load()) return true;
  wchar_t sys[MAX_PATH];
  const UINT n = GetSystemDirectoryW(sys, MAX_PATH);
  if (!n || n >= MAX_PATH) return false;
  HMODULE m = LoadLibraryW((std::wstring(sys) + L"\\d3dcompiler_47.dll").c_str());
  if (!m) return false;
  auto compile = reinterpret_cast<pD3DCompile>(GetProcAddress(m, "D3DCompile"));
  if (!compile) return false;
  const char* stage[5] = {"0", "0", "1", "1", "2"};
  const char* arr[5] = {"0", "1", "0", "1", "0"};
  for (int i = 0; i < 5; ++i) {
    const D3D_SHADER_MACRO defs[] = {{"OV_STAGE", stage[i]}, {"OV_ARRAY", arr[i]}, {nullptr, nullptr}};
    ID3DBlob* code = nullptr;
    ID3DBlob* err = nullptr;
    const HRESULT hr = compile(kShader, sizeof(kShader) - 1, "cockpit_overlay", defs, nullptr, "main", "cs_5_0",
                               D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &err);
    if (FAILED(hr) || !code) {
      Log("cockpit mask overlay: shader compile failed: %s",
          err ? static_cast<const char*>(err->GetBufferPointer()) : "D3DCompile failed");
      if (err) err->Release();
      if (code) code->Release();
      return false;
    }
    const uint8_t* b = static_cast<const uint8_t*>(code->GetBufferPointer());
    g_blob[i].assign(b, b + code->GetBufferSize());
    code->Release();
    if (err) err->Release();
  }
  g_blobsOk = true;
  return true;
}

// The mask slot textures' SRVs and the scratch atlas follow the mask atlas (cockpit_mask.h ReleaseAtlas).
void ReleaseAtlasRes() {
  for (int i = 0; i < kSlots; ++i) {
    RelP(g_o.maskSrv[i]);
    g_o.maskSrvOf[i] = nullptr;
  }
  RelP(g_o.scratchUav);
  RelP(g_o.scratch);
  g_o.sW = g_o.sH = 0;
}

void ReleaseLearnt(int v) {
  RelP(g_l[v].srv);
  RelP(g_l[v].tex);
  g_l[v] = Learnt{};
}

// Everything, DCS's targets included (cockpit_mask.h ReleaseAll: the uninstall, a device change). Also an arm the
// pass end never closed.
void ReleaseAllRes() {
  ReleaseAtlasRes();
  for (int v = 0; v < 4; ++v) ReleaseLearnt(v);
  for (auto*& c : g_o.cs) RelP(c);
  for (auto*& b : g_o.cb) RelP(b);
  RelP(g_o.cntUav);
  RelP(g_o.cnt);
  for (TsFrame& t : g_o.tf) {
    RelP(t.dj);
    for (auto& v : t.ts)
      for (auto*& q : v) RelP(q);
    RelP(t.cnt);
  }
  g_o = ResOv{};
  if (g_cmOvlRtv) {
    g_cmOvlRtv->Release();
    g_cmOvlRtv = nullptr;
  }
  g_cmOvlArm = false;
  g_cmOvlWantVp = false;
}

// Our objects on DCS's device (render thread, lazily).
bool EnsureRes() {
  if (g_o.failed || !g_r.dev || !g_r.haveLayout) return false;
  ID3D11Device* dev = g_r.dev;
  if (!g_o.cs[0]) {
    if (!CompileOv()) {
      g_o.failed = true;
      return false;
    }
    for (int i = 0; i < 5; ++i)
      if (FAILED(dev->CreateComputeShader(g_blob[i].data(), g_blob[i].size(), nullptr, &g_o.cs[i]))) {
        g_o.failed = true;
        return false;
      }
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = 64;
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    for (auto*& b : g_o.cb)
      if (FAILED(dev->CreateBuffer(&bd, nullptr, &b))) {
        g_o.failed = true;
        return false;
      }
    D3D11_BUFFER_DESC cd{};
    cd.ByteWidth = 32;  // R76: 8 counters (changed, world per view)
    cd.Usage = D3D11_USAGE_DEFAULT;
    cd.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
    D3D11_UNORDERED_ACCESS_VIEW_DESC ud{};
    ud.Format = DXGI_FORMAT_R32_UINT;
    ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    ud.Buffer.NumElements = 8;
    if (FAILED(dev->CreateBuffer(&cd, nullptr, &g_o.cnt)) || FAILED(dev->CreateUnorderedAccessView(g_o.cnt, &ud, &g_o.cntUav))) {
      g_o.failed = true;
      return false;
    }
    D3D11_QUERY_DESC qd{D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
    D3D11_QUERY_DESC qt{D3D11_QUERY_TIMESTAMP, 0};
    D3D11_BUFFER_DESC sd{};
    sd.ByteWidth = 32;
    sd.Usage = D3D11_USAGE_STAGING;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    for (TsFrame& t : g_o.tf) {
      dev->CreateQuery(&qd, &t.dj);
      for (auto& v : t.ts)
        for (auto*& q : v) dev->CreateQuery(&qt, &q);
      dev->CreateBuffer(&sd, nullptr, &t.cnt);
    }
  }
  const Layout& L = g_r.layout;
  if (!g_o.scratch || g_o.sW != L.atlasW || g_o.sH != L.atlasH) {
    RelP(g_o.scratchUav);
    RelP(g_o.scratch);
    D3D11_TEXTURE2D_DESC td{};
    td.Width = L.atlasW;
    td.Height = L.atlasH;
    td.MipLevels = td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R32_UINT;
    td.SampleDesc.Count = 1;
    td.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
    if (FAILED(dev->CreateTexture2D(&td, nullptr, &g_o.scratch)) ||
        FAILED(dev->CreateUnorderedAccessView(g_o.scratch, nullptr, &g_o.scratchUav)))
      return false;
    g_o.sW = L.atlasW;
    g_o.sH = L.atlasH;
  }
  return true;
}

ID3D11ShaderResourceView* MaskSrv(int slot) {
  if (slot < 0 || slot >= kSlots || !g_r.tex[slot]) return nullptr;
  if (g_o.maskSrvOf[slot] != g_r.tex[slot]) {
    RelP(g_o.maskSrv[slot]);
    g_o.maskSrvOf[slot] = nullptr;
    if (FAILED(g_r.dev->CreateShaderResourceView(g_r.tex[slot], nullptr, &g_o.maskSrv[slot]))) return nullptr;
    g_o.maskSrvOf[slot] = g_r.tex[slot];
  }
  return g_o.maskSrv[slot];
}

// One dispatch on DCS's context; every CS binding touched is put back, and the OM targets when one of them is `hdr`.
void RunOv(ID3D11ComputeShader* cs, ID3D11Buffer* cb, ID3D11ShaderResourceView* t0, ID3D11ShaderResourceView* t1,
           ID3D11UnorderedAccessView* u0, ID3D11UnorderedAccessView* u1, ID3D11UnorderedAccessView* u2, UINT gx, UINT gy,
           ID3D11Resource* hdr) {
  State& s = g_s;
  ID3D11DeviceContext* c = s.ctx;
  Saved sv;
  SaveState(c, s.ctx1, sv);
  ID3D11UnorderedAccessView* savedU[2] = {};
  c->CSGetUnorderedAccessViews(1, 2, savedU);
  if (hdr) {  // DCS's HDR target bound as a render target would null our SRV: unbind the OM targets for the dispatch
    ID3D11DepthStencilView* d = nullptr;
    ID3D11RenderTargetView* rtv[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
    c->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtv, &d);
    bool conflict = false;
    for (auto* r : rtv)
      if (r) {
        ID3D11Resource* res = nullptr;
        r->GetResource(&res);
        conflict = conflict || res == hdr;
        if (res) res->Release();
      }
    if (conflict) {
      sv.om = true;
      memcpy(sv.rtv, rtv, sizeof(rtv));
      sv.dsv = d;
      c->OMSetRenderTargetsAndUnorderedAccessViews(0, nullptr, nullptr, 0, D3D11_KEEP_UNORDERED_ACCESS_VIEWS, nullptr,
                                                   nullptr);
      g_omUnbinds++;
    } else {
      for (auto* r : rtv)
        if (r) r->Release();
      if (d) d->Release();
    }
  }
  c->CSSetShader(cs, nullptr, 0);
  ID3D11Buffer* cbs[2] = {cb, nullptr};
  c->CSSetConstantBuffers(0, 2, cbs);
  const UINT keep[3] = {static_cast<UINT>(-1), static_cast<UINT>(-1), static_cast<UINT>(-1)};
  ID3D11UnorderedAccessView* us[3] = {u0, u1, u2};
  c->CSSetUnorderedAccessViews(0, 3, us, keep);  // before the SRVs (a target of DCS's left at a CS UAV is displaced)
  ID3D11ShaderResourceView* ts[2] = {t0, t1};
  c->CSSetShaderResources(0, 2, ts);
  c->Dispatch(gx, gy, 1);
  ID3D11ShaderResourceView* nullSrv[2] = {};
  c->CSSetShaderResources(0, 2, nullSrv);
  c->CSSetUnorderedAccessViews(1, 2, savedU, keep);
  for (auto* u : savedU)
    if (u) u->Release();
  RestoreState(c, s.ctx1, sv);
}

TsFrame* CurTs() {
  if (g_o.tfCur >= 0) return &g_o.tf[g_o.tfCur];
  TsFrame& t = g_o.tf[g_o.tfNext];
  if (t.pending || t.cntPending || !t.dj) return nullptr;  // still in flight: no timing this frame
  g_o.tfCur = g_o.tfNext;
  g_o.tfNext = (g_o.tfNext + 1) % kTs;
  t.open = true;
  t.views = 0;
  for (auto& u : t.used) u[0] = u[1] = false;
  g_s.ctx->Begin(t.dj);
  return &t;
}

// The view's mask this frame (sampled before its overlay pass) and its learnt target with the same source size.
bool ViewReady(int v) {
  return v >= 0 && v < 4 && g_f.begun && !g_f.skipped && g_f.slot >= 0 && (g_f.viewMask & (1u << v)) &&
         g_r.haveLayout && g_r.tex[g_f.slot];
}

void Pre(int v) {
  const cm::MaskView& mv = g_f.view[v];
  Learnt& l = g_l[v];
  if (!l.ok || l.srcW != mv.srcW || l.srcH != mv.srcH) return;
  if (!EnsureRes()) return;
  ID3D11ShaderResourceView* msrv = MaskSrv(g_f.slot);
  if (!msrv) return;
  uint32_t p[16] = {mv.x, mv.y, mv.w, mv.h, mv.srcW, mv.srcH, mv.divisor, static_cast<uint32_t>(v),
                    static_cast<uint32_t>(l.vp.TopLeftX), static_cast<uint32_t>(l.vp.TopLeftY),
                    static_cast<uint32_t>(l.vp.Width), static_cast<uint32_t>(l.vp.Height), 0, l.slice, 0, 0};
  // World texels that may change: kMaxFrac of the rect, and (R76) kMaxFrac of the view's world texels, which the GPU
  // counts in Post (the cockpit share is unknown on the CPU: the rect alone never rejected a mostly-cockpit view).
  p[12] = static_cast<uint32_t>(kMaxFrac * float(mv.w) * float(mv.h));
  p[14] = static_cast<uint32_t>(kMaxFrac * 100.0f + 0.5f);
  g_s.ctx->UpdateSubresource(g_o.cb[v], 0, nullptr, p, 0, 0);
  TsFrame* t = CurTs();
  if (t) g_s.ctx->End(t->ts[v][0]);
  RunOv(g_o.cs[l.array ? 1 : 0], g_o.cb[v], msrv, l.srv, g_o.scratchUav, g_o.cntUav, nullptr, (mv.w + 7) / 8,
        (mv.h + 7) / 8, l.tex);
  if (t) {
    g_s.ctx->End(t->ts[v][1]);
    t->used[v][0] = true;
    t->area[v] = mv.w * mv.h;
    t->limit[v] = p[12];
    t->pct = p[14];
  }
  g_f.ovlPre |= 1u << v;
  g_pre++;
}

void Post(int v) {
  const cm::MaskView& mv = g_f.view[v];
  Learnt& l = g_l[v];
  TsFrame* t = CurTs();
  if (t) g_s.ctx->End(t->ts[v][2]);
  RunOv(g_o.cs[l.array ? 3 : 2], g_o.cb[v], nullptr, l.srv, g_o.scratchUav, g_o.cntUav, nullptr, (mv.w + 7) / 8,
        (mv.h + 7) / 8, l.tex);
  RunOv(g_o.cs[4], g_o.cb[v], nullptr, nullptr, g_o.scratchUav, g_o.cntUav, g_r.uav[g_f.slot], (mv.w + 7) / 8,
        (mv.h + 7) / 8, nullptr);
  if (t) {
    g_s.ctx->End(t->ts[v][3]);
    t->used[v][1] = true;
    t->views |= 1u << v;
  }
  g_f.view[v].flags |= cm::view_flags::overlay;
  g_f.ovlMask |= 1u << v;
  g_post++;
}

// The pass end: what the pass bound (fixation_depth.h's notes) against the learnt target; learn when it differs.
void EndPass(int v) {
  ID3D11RenderTargetView* rtv = g_cmOvlRtv;
  g_cmOvlRtv = nullptr;
  const D3D11_VIEWPORT vp = g_cmOvlVp;
  const bool haveVp = !g_cmOvlWantVp && rtv;
  g_cmOvlWantVp = false;
  if (!rtv) {
    if (g_f.ovlPre & (1u << v)) g_mismatch++;
    return;
  }
  ID3D11Resource* res = nullptr;
  rtv->GetResource(&res);
  D3D11_RENDER_TARGET_VIEW_DESC rd{};
  rtv->GetDesc(&rd);
  rtv->Release();
  const bool array = rd.ViewDimension == D3D11_RTV_DIMENSION_TEXTURE2DARRAY;
  const UINT slice = array ? rd.Texture2DArray.FirstArraySlice : 0;
  Learnt& l = g_l[v];
  const bool same = res && l.ok && res == l.tex && haveVp && l.array == array && l.slice == slice &&
                    std::fabs(vp.TopLeftX - l.vp.TopLeftX) < 0.5f && std::fabs(vp.TopLeftY - l.vp.TopLeftY) < 0.5f &&
                    std::fabs(vp.Width - l.vp.Width) < 0.5f && std::fabs(vp.Height - l.vp.Height) < 0.5f;
  if (same && (g_f.ovlPre & (1u << v))) {
    Post(v);
    res->Release();
    return;
  }
  if (g_f.ovlPre & (1u << v)) g_mismatch++;
  // Learn: single-sample 2D (array) texture, shader-readable, the mask view's aspect.
  bool ok = res && haveVp && vp.Width >= 1 && vp.Height >= 1 &&
            (rd.ViewDimension == D3D11_RTV_DIMENSION_TEXTURE2D ? rd.Texture2D.MipSlice == 0
                                                                : array && rd.Texture2DArray.MipSlice == 0);
  const cm::MaskView& mv = g_f.view[v];
  if (ok) {
    ID3D11Texture2D* t2 = nullptr;
    D3D11_TEXTURE2D_DESC td{};
    if (SUCCEEDED(res->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&t2))) && t2) {
      t2->GetDesc(&td);
      t2->Release();
      ok = td.SampleDesc.Count == 1 && (td.BindFlags & D3D11_BIND_SHADER_RESOURCE);
    } else {
      ok = false;
    }
    if (!ok) g_noLearn++;
  }
  if (ok && (!mv.srcW || !mv.srcH ||
             std::fabs(vp.Width / vp.Height - float(mv.srcW) / float(mv.srcH)) > 0.02f * float(mv.srcW) / float(mv.srcH) ||
             vp.Width + 0.5f < float(mv.srcW) * 0.9f)) {
    ok = false;
    g_aspect++;
  }
  ReleaseLearnt(v);
  if (ok) {
    D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
    sd.Format = rd.Format;
    if (array) {
      sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
      sd.Texture2DArray.MostDetailedMip = 0;
      sd.Texture2DArray.MipLevels = 1;
      sd.Texture2DArray.FirstArraySlice = slice;
      sd.Texture2DArray.ArraySize = 1;
    } else {
      sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
      sd.Texture2D.MostDetailedMip = 0;
      sd.Texture2D.MipLevels = 1;
    }
    if (SUCCEEDED(g_r.dev->CreateShaderResourceView(res, &sd, &l.srv))) {
      l.tex = res;
      res = nullptr;  // the reference moves into l.tex
      l.array = array;
      l.slice = slice;
      l.vp = vp;
      l.srcW = mv.srcW;
      l.srcH = mv.srcH;
      l.ok = true;
      g_learns++;
      Log("cockpit mask overlay: view %d learnt its HDR target (%s, box %ux%u at %.0f,%.0f, format %d) for source %ux%u",
          v, array ? "array slice" : "2D", static_cast<unsigned>(vp.Width), static_cast<unsigned>(vp.Height),
          vp.TopLeftX, vp.TopLeftY, static_cast<int>(rd.Format), mv.srcW, mv.srcH);
    } else {
      g_noLearn++;
    }
  }
  if (res) res->Release();
}

int g_passView = -1;  // the view whose OverlayHDRPass is open (render thread)
bool g_ownArm = false;

bool IsOverlayHdr(void* pass) {
  char n[48];
  PassName(pass, n, sizeof(n));
  return strcmp(n, "OverlayHDRPass") == 0;
}

// fixation_depth.h's boundary callback, kOther passes: begin first in the boundary (before any arm), end after cockpit
// ipd's and far depth's end work.
void Boundary(void* pass, bool begin, int hmdOrd) {
  if (begin) {
    if (g_passView >= 0 || !IsOverlayHdr(pass)) return;
    const int v = hmdOrd;
    if (v < 0 || v > 3 || !(g_views.load(std::memory_order_relaxed) & (1u << v)) || (g_f.ovlDone & (1u << v))) return;
    g_f.ovlDone |= 1u << v;
    if (!ViewReady(v)) {
      g_noMask++;
      return;
    }
    Pre(v);
    // The arm: ours when none is held (far depth's overlay arm and cockpit ipd's nest in it); a held capture arm is
    // shared (its OM hook notes our bind too).
    g_ownArm = ArmOm(true);
    if (!g_armTable || g_usOnlyArm) {
      g_noArm++;
      return;
    }
    g_passView = v;
    g_cmOvlRtv = nullptr;
    g_cmOvlWantVp = false;
    g_cmOvlArm = true;
    return;
  }
  if (g_passView < 0 || !IsOverlayHdr(pass)) return;
  const int v = g_passView;
  g_passView = -1;
  g_cmOvlArm = false;
  if (g_ownArm && !g_overlayArm && !g_trOpen) DisarmOm();
  g_ownArm = false;
  EndPass(v);
}

// A GBufferPass began while our pass was open (its end never came): close.
void Abort() {
  if (g_passView < 0) return;
  g_passView = -1;
  g_cmOvlArm = false;
  if (g_ownArm && !g_overlayArm && !g_trOpen) DisarmOm();
  g_ownArm = false;
  if (g_cmOvlRtv) {
    g_cmOvlRtv->Release();
    g_cmOvlRtv = nullptr;
  }
  g_cmOvlWantVp = false;
}

// The commit (render thread): the frame's timing and counters closed, older ones read (never waiting).
void FrameEnd(const Frame& f) {
  ID3D11DeviceContext* c = g_s.ctx;
  if (!c) return;
  if (f.ovlMask) g_frames++;
  if (g_o.tfCur >= 0) {
    TsFrame& t = g_o.tf[g_o.tfCur];
    c->End(t.dj);
    t.open = false;
    t.pending = true;
    if (t.views && t.cnt && g_o.cnt) {
      c->CopyResource(t.cnt, g_o.cnt);
      t.cntPending = true;
    }
    g_o.tfCur = -1;
  }
  for (TsFrame& t : g_o.tf) {
    if (t.open) continue;
    if (t.pending) {
      D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj{};
      if (c->GetData(t.dj, &dj, sizeof(dj), D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK) {
        double us = 0;
        bool ok = !dj.Disjoint && dj.Frequency > 0;
        for (int v = 0; v < 4 && ok; ++v)
          for (int k = 0; k < 2 && ok; ++k) {
            if (!t.used[v][k]) continue;
            UINT64 a = 0, b = 0;
            if (c->GetData(t.ts[v][2 * k], &a, sizeof(a), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK ||
                c->GetData(t.ts[v][2 * k + 1], &b, sizeof(b), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) {
              ok = false;
              break;
            }
            us += (b >= a ? b - a : 0) * 1e6 / static_cast<double>(dj.Frequency);
          }
        t.pending = false;
        if (ok && (t.used[0][0] || t.used[1][0] || t.used[2][0] || t.used[3][0])) {
          std::lock_guard<std::mutex> lock(g_statMu);
          g_gpuSum += us;
          ++g_gpuN;
        }
      }
    }
    if (t.cntPending && !t.pending) {
      D3D11_MAPPED_SUBRESOURCE m{};
      if (SUCCEEDED(c->Map(t.cnt, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m))) {
        uint32_t n[8];  // R76: [4 + v] the view's world texels
        memcpy(n, m.pData, sizeof(n));
        c->Unmap(t.cnt, 0);
        t.cntPending = false;
        std::lock_guard<std::mutex> lock(g_statMu);
        for (int v = 0; v < 4; ++v) {
          if (!(t.views & (1u << v)) || !t.area[v]) continue;
          if (n[v] > t.limit[v] || static_cast<uint64_t>(n[v]) * 100u > static_cast<uint64_t>(n[4 + v]) * t.pct) {
            g_rejected++;
            continue;
          }
          g_applied++;
          if (n[v]) g_viewsCovered++;
          g_lastCov[v] = double(n[v]) / double(t.area[v]);
          g_covSum[v] += g_lastCov[v];
          ++g_covN[v];
        }
      }
    }
  }
}

std::string StatsText() {
  auto u = [](const std::atomic<uint64_t>& a) { return static_cast<unsigned long long>(a.load()); };
  double gpu = 0, cov[4] = {};
  {
    std::lock_guard<std::mutex> lock(g_statMu);
    gpu = g_gpuN ? g_gpuSum / double(g_gpuN) : 0.0;
    for (int v = 0; v < 4; ++v) cov[v] = g_covN[v] ? 100.0 * g_covSum[v] / double(g_covN[v]) : 0.0;
    g_gpuSum = 0;
    g_gpuN = 0;
    for (int v = 0; v < 4; ++v) g_covSum[v] = 0, g_covN[v] = 0;
  }
  char b[600];
  sprintf_s(b,
            "overlay symbology (v1.1) %s: %llu frames computed, views pre %llu / post %llu (applied %llu, with symbols "
            "%llu, rejected > 40 %% %llu); coverage view 0 %.2f %% / 1 %.2f %% / 2 %.2f %% / 3 %.2f %% of the rect; "
            "learnt %llu, not learnt %llu (aspect %llu), target or box mismatch %llu, no mask yet %llu, no arm %llu; "
            "GPU %.1f us/frame",
            g_on.load() ? "on" : "off", u(g_frames), u(g_pre), u(g_post), u(g_applied), u(g_viewsCovered),
            u(g_rejected), cov[0], cov[1], cov[2], cov[3], u(g_learns), u(g_noLearn), u(g_aspect), u(g_mismatch),
            u(g_noMask), u(g_noArm), gpu);
  return b;
}

}  // namespace covl

// CockpitMaskLog: the slot's overlay views (appended to the frame line).
std::string OverlayLogText(const cm::MaskSlot& sl) {
  if (!(sl.flags & cm::slot_flags::overlay)) return std::string();
  char b[160];
  uint32_t m = 0;
  for (int v = 0; v < 4; ++v)
    if (sl.view[v].flags & cm::view_flags::overlay) m |= 1u << v;
  sprintf_s(b, " | overlay views 0x%x (last coverage %.2f %% / %.2f %%)", m, 100.0 * covl::g_lastCov[0],
            100.0 * covl::g_lastCov[1]);
  return b;
}
