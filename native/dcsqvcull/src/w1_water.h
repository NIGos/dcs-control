// W1: DCS's water mask, refraction, rivers and sea passes are not drawn by the GPU in a view that shows no water
// ([Water] SkipHiddenWater, default 0). The decision is made on the GPU, in the same frame, from the water DCS itself
// draws.
//
// Per view DCS runs, in this order and nothing in between (2.9.31, all four Quad Views views, measured 2026-11-18):
//   WaterWaveMaskPass (M: water.fx technique Wave: the shore wave texture and the water mask, ~0.46 ms for four views)
//   RefractionPass    (R: refraction of the bottom and its filter, ~0.08 ms)
//   RiversPass, SeaPass (the water surfaces into the G-buffer: every draw writes stencil WATER with REPLACE, depth
//                     test GREATER_EQUAL, KEEP on fail; no UAV, no clear, no copy)
// The outputs of M and R are read only by the sea's VS/DS/PS, by R itself and by the lighting compose at WATER and
// UNDERWATER stencil pixels (reader scan and compiled-shader audit, stock and ATMOSX). So when no Rivers/Sea sample
// passes, nothing M and R write is ever seen. R, the rivers or the sea may be missing (other water settings): the
// passes that come are handled in that order; anything else in between runs the held passes at once (stock).
//
// With the key on, a view's M runs its CPU side at its own slot with its GPU work under a predicate known to be FALSE
// (so the constant buffers it uploads, cbWaterParams with the view's matrices and wave time, are current for the
// test), and R is held back (its boundary callbacks fire around nothing). At RiversPass and SeaPass DCS's own executes
// run first as a TEST: same shaders, bindings and depth/stencil tests, but every depth-stencil state is swapped for a
// copy with depth and stencil writes off and every blend state for a copy that writes no colour, inside an occlusion
// predicate (and two counting occlusion queries, rivers and sea). Then, under that predicate, the real M, R, rivers and
// sea in DCS's order: the GPU skips all of them when the test saw no water sample.
// Uploads inside a predicated block (UpdateSubresource on a buffer) go through predicated and their bytes are kept;
// after the block the last upload of each (buffer, box) is issued again unpredicated, so every buffer ends as without
// W1 in both outcomes (DCS's effect framework caches what it uploaded). Other uploads are issued unpredicated in place.
//
// Falls back to stock (runs everything unpredicated, reason in the counters): a depth-stencil state during the test
// that is not a water-marking one ("unknown draw"), a context-table slot W1 needs held by another hook of ours ("hook
// taken"), a pass out of the expected order ("pass order"), the start guard ("start").
//
// Start guard: the first 3 frames after the key goes on, after a camera cut (scene_cut.h: F1/F2/F10, respawn, mission
// load) and after a pause of more than 0.5 s without frames run unpredicated (tested and counted, never skipped).
// Water seen: once a view had a water sample, every view runs stock (no test, real passes counted) until 90 frames in
// a row had no water sample in any view; then the views are tested again.
//
// Exactness. Rivers: the test is the stock draw (the river VS reads only terrain buffers; the PS discard reads
// constants). Sea: its DS reads M's two outputs (g_WaveTexture: the shore wave displacement, stock and ATMOSX;
// g_MaskTexture: ATMOSX's swell damping and the non-GEOTERRAIN shore damping). In the test those textures still hold
// the last M that the GPU really ran (a previous view or frame), so the sea's test geometry can differ from the real
// one by the shore wave / swell displacement: an undercount, never measured to reach zero outside the start guard.
//
// Modes ([Water] SkipHiddenWater): 0 off (nothing installed). 1 on. 2 verify: everything runs unpredicated (the stock
// picture), the test runs and is logged next to the real passes' own sample counts, per view and frame, in
// DcsQvCull_water.csv next to the log; the start guard and the water-seen switch run as in mode 1 and their decision
// is logged too. In verify mode the test is fed M's outputs as mode 1 would leave them (a private copy of M's targets,
// updated only when mode 1 would have run M on the GPU), so "test 0, real > 0" counts exactly the frames mode 1 would
// have drawn wrong. Other values: off.
// 3 no-submit (0.7): a view that the earlier frames found dry is not submitted at all (DCS's water mask, refraction,
// rivers and sea pass objects are not called), so the render thread saves their time as well. The decision comes from
// earlier frames, read back on the CPU, so it is NOT exact: the guard is temporal only. While no view has seen water
// (the gate's test state), each view is probed once every [Water] NoSubmitProbe frames (default 4, 1-16; staggered:
// view v probes when (frame + v) % N == 0): the probe is mode 1's test alone (M's CPU side under a FALSE predicate,
// the rivers / sea test with writes off, counted), the real passes are not drawn. In the other frames the view is not
// submitted. A probe that sees a sample turns the gate to stock (every view drawn) when its queries are read back
// (1-3 frames later); the start guard and the 90-dry-frame return work as in mode 1. Worst case after water enters
// the view: the view stays undrawn until a probe of any view sees it and is read back: N - 1 + read-back latency
// frames (N = 4, latency 2: up to 5 frames, ~55 ms at 90 Hz); there is no spatial margin, so the strip that enters
// during that time (head turn rate x that time, e.g. 100 deg/s x 55 ms = 5.5 deg) is missing for those frames.
// What the skipped passes write that later passes read: on the GPU nothing, the same as a predicated view without
// water in mode 1 (M's and R's targets keep older contents, no stencil WATER); their constant-buffer uploads are not
// made, so DCS's effect framework and the GPU buffers agree (both hold the last upload that did run).
// 4 verify of 3: everything is drawn (the stock picture); the views mode 3 would probe run the test (fed M's outputs
// as mode 3 leaves them), the views it would skip run their real passes counted; the line counts the view-frames
// with real water that mode 3 would not have drawn (skipped or probed), the frames with any such view, the episodes
// and the longest run of such frames.
// The "water skip:" line every 10 s while on: per view tested / skipped / kept / forced / stock frames, forced reasons,
// switches, start-guard arms, and in verify mode the frames with real water while the test saw none (rivers, sea).
// In modes 3 / 4 "tested" = probe and start-guard frames, "skipped" = frames not submitted (4: that mode 3 would not
// submit), "kept" = probes that saw water.
//
// Included once from main.cpp inside its anonymous namespace, after pass_timing.h, d3dstate.h, split_filter.h,
// shadow_inst.h, scene_cut.h and gpu_pass_timing.h.
#pragma once

namespace w1 {

using Ctx = ID3D11DeviceContext;
using ExecFn = ptiming::ExecFn;

// ---------------------------------------------------------------------------
// Decision logic (pure; w1_water_test.h)
// ---------------------------------------------------------------------------
constexpr int kStartFrames = 3;  // frames run unpredicated after the key goes on, a camera cut or a pause
constexpr int kDryFrames = 90;   // frames without water in any view before tested views come back
int g_gapMs = 500;               // no W1 frame for this long: the start guard again (tests raise it)

// The per-session state of one mode (1 or 2): start guard and the water-seen switch. Render thread.
struct Gate {
  bool stock = false;  // water was seen: views run stock
  int dry = 0;         // frames in a row without water while stock
  int warm = 0;        // start-guard frames left
  bool forced = false; // this frame runs under the start guard
  uint64_t toStock = 0, toTest = 0, arms = 0;
  void Start() {  // the key went on (or changed mode)
    stock = false, dry = 0, warm = kStartFrames, forced = true;  // a change mid-frame guards that frame's later views
  }
  void Arm() {  // a camera cut or a pause
    warm = kStartFrames;
    ++arms;
  }
  void BeginFrame() {
    forced = warm > 0;
    if (warm > 0) --warm;
  }
  // One frame read back: did any view have a water sample (the test's count in a tested view, the real passes'
  // count in a stock view)?
  void OnFrameRead(bool water) {
    if (water) {
      if (!stock) ++toStock;
      stock = true, dry = 0;
    } else if (stock && ++dry >= kDryFrames) {
      stock = false, dry = 0;
      ++toTest;
    }
  }
};

// What mode 1 does with one view in the current frame (modes 3 / 4: kPlanTest = a probe, kPlanSkip = not submitted).
enum ViewPlan : uint8_t { kPlanTest = 0, kPlanStock = 1, kPlanForced = 2, kPlanSkip = 3 };
inline ViewPlan PlanView(const Gate& g) { return g.stock ? kPlanStock : g.forced ? kPlanForced : kPlanTest; }

// Mode 1 skips a tested view when the test saw no sample: exact when that never happens while the real passes have one.
inline bool Skips(ViewPlan p, uint64_t testSamples) { return p == kPlanTest && testSamples == 0; }

inline int SanitizeMode(int m) { return m >= 1 && m <= 4 ? m : 0; }
inline bool IsVerify(int m) { return m == 2 || m == 4; }
inline bool NoSubmit(int m) { return m == 3 || m == 4; }
inline int SanitizeProbe(int n) { return n < 1 ? 1 : n > 16 ? 16 : n; }
// Modes 3 / 4: a view the gate would test is probed one frame in `every` (staggered by view), else not submitted.
inline ViewPlan PlanFor(const Gate& g, int mode, uint64_t frame, int view, int every) {
  const ViewPlan p = PlanView(g);
  if (p != kPlanTest || !NoSubmit(mode)) return p;
  return every <= 1 || (frame + static_cast<uint64_t>(view)) % static_cast<uint64_t>(every) == 0 ? kPlanTest : kPlanSkip;
}
// Mode 3 draws a view's water only in its stock and start-guard frames: a probe or a skipped view with real water is
// a frame drawn wrong.
inline bool NoSubmitWrong(ViewPlan p, uint64_t realSamples) {
  return (p == kPlanTest || p == kPlanSkip) && realSamples != 0;
}
// Runs of frames with at least one view drawn wrong (mode 4's account; also the offline replay).
struct BadRuns {
  uint64_t frames = 0, episodes = 0, longest = 0, cur = 0;
  void Frame(bool bad) {
    if (bad) {
      if (!cur) ++episodes;
      ++frames, ++cur;
      if (cur > longest) longest = cur;
    } else {
      cur = 0;
    }
  }
};

// ---------------------------------------------------------------------------
// Pass kinds (render thread)
// ---------------------------------------------------------------------------
std::atomic<int> g_mode{0};       // wanted ([Water] SkipHiddenWater, kill switch applied)
std::atomic<int> g_benchMode{-1};  // >= 0: the timing phase's arm (replaces g_mode while it runs)
int g_rMode = 0;                  // render thread: the mode in force
std::atomic<int> g_probeEvery{4};  // [Water] NoSubmitProbe (modes 3 / 4)
enum PK : uint8_t { kNone, kMask, kRefr, kRivers, kSea, kOther };

std::unordered_map<void*, PK> g_vtKind;  // vtable -> kind for the kinds with their own class
// The timing phase: the water pass objects of its blocks (both arms) and their kind, whose render-thread time it sums.
std::atomic<bool> g_timing{false};
std::mutex g_passMu;
std::unordered_map<void*, PK> g_passObjs;  // guarded by g_passMu

inline bool InlineName(void* pass, void** vt, char* name) {
  __try {
    *vt = *reinterpret_cast<void**>(pass);
    const uint8_t* b = static_cast<const uint8_t*>(pass);
    const uint64_t n = *reinterpret_cast<const uint64_t*>(b + 8);
    name[0] = 0;
    if (n > 0 && n <= 32 && b[0x10 + n] == 0) {
      for (uint64_t i = 0; i < n; ++i)
        if (b[0x10 + i] < 0x20 || b[0x10 + i] > 0x7e) return true;
      memcpy(name, b + 0x10, n);
      name[n] = 0;
    }
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}
inline PK KindOf(void* pass) {
  void* vt = nullptr;
  char nm[40];
  if (!InlineName(pass, &vt, nm)) return kOther;
  auto it = g_vtKind.find(vt);
  if (it != g_vtKind.end() && it->second != kNone) return it->second;
  if (it == g_vtKind.end()) {
    const std::string r = ptiming::RttiName(pass);
    const PK k = r == "RefractionPassData" ? kRefr : r == "RiversPassData" ? kRivers : r == "SeaPassData" ? kSea
               : (r == "PassData" || r == "SimplePassData") ? kNone : kOther;
    g_vtKind[vt] = k;
    if (k != kNone) return k;
  }
  return strcmp(nm, "WaterWaveMaskPass") == 0 ? kMask : kOther;  // PassData is shared: its inline name decides
}

// ---------------------------------------------------------------------------
// D3D objects (render thread; created on first use, released at Shutdown)
// ---------------------------------------------------------------------------
ID3D11Device* g_dev = nullptr;
Ctx* g_ctx = nullptr;
constexpr int kRing = 16, kViews = 4;
struct Rec {
  ID3D11Predicate* pred = nullptr;  // test: any water sample
  ID3D11Query* occR = nullptr;      // test samples, rivers
  ID3D11Query* occS = nullptr;      // test samples, sea
  ID3D11Query* gtR = nullptr;       // the real rivers pass's samples (verify, stock views)
  ID3D11Query* gtS = nullptr;       // the real sea pass's samples
};
// kStSkipped: mode 3, not submitted. kStProbe: mode 3, the test alone. kStVerifySkip: mode 4, a view mode 3 would not
// submit (its real passes run counted).
enum State : uint8_t {
  kStNone = 0, kStPredicated = 1, kStForced = 2, kStVerify = 3, kStStock = 4, kStSkipped = 5, kStProbe = 6,
  kStVerifySkip = 7
};
enum Reason : uint8_t { kRsNone = 0, kRsUnknownDraw = 1, kRsHookTaken = 2, kRsPassOrder = 3, kRsStart = 4, kRsCount = 5 };
struct FrameRec {
  uint64_t frame = 0;
  bool used = false;
  uint8_t state[kViews] = {};
  uint8_t reason[kViews] = {};
  uint8_t plan[kViews] = {};  // verify: what mode 1 would have done (ViewPlan)
  bool testOn[kViews] = {}, gtOn[kViews] = {};
  Rec r[kViews];
};
FrameRec g_ring[kRing];
int g_cur = -1;  // ring slot of the frame being recorded
ID3D11DepthStencilState* g_dssNever = nullptr;
ID3D11Predicate* g_false = nullptr;  // an occlusion predicate over nothing, resolved long ago: FALSE, no wait
std::unordered_map<ID3D11DepthStencilState*, ID3D11DepthStencilState*> g_dssMap;  // DCS's -> write-masked (nullptr: not marking)
std::unordered_map<ID3D11BlendState*, ID3D11BlendState*> g_bsMap;                // DCS's -> no colour writes
bool g_deviceLogged = false;

bool EnsureDevice() {
  if (g_ctx) return true;
  ID3D11Device* dev = shadowinst::g_device;
  if (dev) dev->AddRef();
  else if (sfilt::Ctx* c = sfilt::g_ctx.load()) c->GetDevice(&dev);
  if (!dev) {
    if (!g_deviceLogged) {
      g_deviceLogged = true;
      Log("water skip: DCS's device not known yet (it comes from [Model] ShadowInstancing=1 or [D3D] SplitFilter=1); "
          "views run stock");
    }
    return false;
  }
  g_dev = dev;
  dev->GetImmediateContext(&g_ctx);
  D3D11_DEPTH_STENCIL_DESC d{};
  d.DepthEnable = TRUE, d.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO, d.DepthFunc = D3D11_COMPARISON_NEVER;
  d.StencilEnable = FALSE;
  if (FAILED(dev->CreateDepthStencilState(&d, &g_dssNever))) g_dssNever = nullptr;
  {
    D3D11_QUERY_DESC q{D3D11_QUERY_OCCLUSION_PREDICATE, 0};
    if (SUCCEEDED(dev->CreatePredicate(&q, &g_false))) g_ctx->Begin(g_false), g_ctx->End(g_false);
  }
  for (FrameRec& f : g_ring)
    for (Rec& r : f.r) {
      D3D11_QUERY_DESC q{D3D11_QUERY_OCCLUSION_PREDICATE, 0};
      dev->CreatePredicate(&q, &r.pred);
      q.Query = D3D11_QUERY_OCCLUSION;
      dev->CreateQuery(&q, &r.occR);
      dev->CreateQuery(&q, &r.occS);
      dev->CreateQuery(&q, &r.gtR);
      dev->CreateQuery(&q, &r.gtS);
    }
  return g_ctx != nullptr;
}

// Water-marking: stencil REPLACE with WATER (0x10) or UNDERWATER (0x30) in the composition bits, KEEP on fail.
bool Marking(const D3D11_DEPTH_STENCIL_DESC& d) {
  auto face = [](const D3D11_DEPTH_STENCILOP_DESC& f) {
    return f.StencilPassOp == D3D11_STENCIL_OP_REPLACE && f.StencilFailOp == D3D11_STENCIL_OP_KEEP &&
           f.StencilDepthFailOp == D3D11_STENCIL_OP_KEEP;
  };
  return d.StencilEnable && (d.StencilWriteMask & 0x38) == 0x38 && face(d.FrontFace) && face(d.BackFace);
}
inline bool MarkingRef(UINT ref) {
  const UINT comp = ref & 0x38;
  return comp == 0x10 || comp == 0x30;
}
ID3D11DepthStencilState* MaskedDss(ID3D11DepthStencilState* s, UINT ref, bool* marking) {
  *marking = false;
  if (!s) return g_dssNever;  // default state: depth LESS writes, stencil off: not a water draw
  auto it = g_dssMap.find(s);
  ID3D11DepthStencilState* m = nullptr;
  if (it != g_dssMap.end()) {
    m = it->second;
  } else {
    D3D11_DEPTH_STENCIL_DESC d{};
    s->GetDesc(&d);
    if (Marking(d)) {  // the reference is checked per call below
      d.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
      d.StencilWriteMask = 0;
      if (FAILED(g_dev->CreateDepthStencilState(&d, &m))) m = nullptr;
    }
    s->AddRef();  // the key stays valid
    g_dssMap[s] = m;
  }
  if (!m || !MarkingRef(ref)) return g_dssNever;
  *marking = true;
  return m;
}
ID3D11BlendState* MaskedBs(ID3D11BlendState* s) {
  auto it = g_bsMap.find(s);
  if (it != g_bsMap.end()) return it->second;
  D3D11_BLEND_DESC d{};
  if (s) {
    s->GetDesc(&d);
  } else {
    d.RenderTarget[0].SrcBlend = d.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    d.RenderTarget[0].DestBlend = d.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
    d.RenderTarget[0].BlendOp = d.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
  }
  if (!d.IndependentBlendEnable)
    for (int i = 1; i < 8; ++i) d.RenderTarget[i] = d.RenderTarget[0];
  d.IndependentBlendEnable = TRUE;
  for (auto& rt : d.RenderTarget) rt.RenderTargetWriteMask = 0;
  ID3D11BlendState* m = nullptr;
  if (FAILED(g_dev->CreateBlendState(&d, &m))) m = nullptr;
  if (s) s->AddRef();
  g_bsMap[s] = m;
  return m;
}

// ---------------------------------------------------------------------------
// Test: DCS's depth-stencil and blend states are swapped while its RiversPass / SeaPass run as the test
// ---------------------------------------------------------------------------
int g_slotDss = -1, g_slotBs = -1, g_slotUpd = -1;
void* volatile g_origDss = nullptr;
void* volatile g_origBs = nullptr;
void* volatile g_origUpd = nullptr;
void** g_table = nullptr;
bool g_testing = false;
bool g_unknown = false;  // a non-marking depth-stencil state was set during the test
ID3D11DepthStencilState* g_wantDss = nullptr;  // DCS's last request during the test (restored after it)
UINT g_wantRef = 0;
ID3D11BlendState* g_wantBs = nullptr;
FLOAT g_wantBf[4] = {};
UINT g_wantMask = 0xffffffff;
using DssFn = void(STDMETHODCALLTYPE*)(Ctx*, ID3D11DepthStencilState*, UINT);
using BsFn = void(STDMETHODCALLTYPE*)(Ctx*, ID3D11BlendState*, const FLOAT*, UINT);
using UpdFn = void(STDMETHODCALLTYPE*)(Ctx*, ID3D11Resource*, UINT, const D3D11_BOX*, const void*, UINT, UINT);

void STDMETHODCALLTYPE H_Dss(Ctx* c, ID3D11DepthStencilState* s, UINT ref) {
  if (c == g_ctx && g_testing) {
    g_wantDss = s, g_wantRef = ref;
    bool marking = false;
    ID3D11DepthStencilState* m = MaskedDss(s, ref, &marking);
    if (!marking) g_unknown = true;
    return reinterpret_cast<DssFn>(g_origDss)(c, m, ref);
  }
  reinterpret_cast<DssFn>(g_origDss)(c, s, ref);
}
void STDMETHODCALLTYPE H_Bs(Ctx* c, ID3D11BlendState* s, const FLOAT f[4], UINT mask) {
  if (c == g_ctx && g_testing) {
    g_wantBs = s, g_wantMask = mask;
    if (f) memcpy(g_wantBf, f, sizeof(g_wantBf));
    return reinterpret_cast<BsFn>(g_origBs)(c, MaskedBs(s), f, mask);
  }
  reinterpret_cast<BsFn>(g_origBs)(c, s, f, mask);
}
// Installed only while a block is predicated and no gpu_pass_timing.h session holds the slot (that hook does the same).
void STDMETHODCALLTYPE H_Upd(Ctx* c, ID3D11Resource* d, UINT ds, const D3D11_BOX* b, const void* p, UINT rp, UINT dp) {
  ID3D11Predicate* unp = c == g_ctx ? ptiming::g_unpred.load(std::memory_order_relaxed) : nullptr;
  if (unp && !ptiming::UnpredUpload(d, ds, b, p, rp, dp)) unp = nullptr;  // kept, issued again after the block
  if (unp) c->SetPredication(nullptr, FALSE);
  reinterpret_cast<UpdFn>(g_origUpd)(c, d, ds, b, p, rp, dp);
  if (unp) c->SetPredication(unp, FALSE);
}

// Puts the two state hooks in DCS's context table; false (nothing changed) when a slot holds another hook of ours or
// the split filter calls d3d11's blend / depth entries directly.
bool BeginTest() {
  if (sfilt::g_opMask.load() & ((1u << sfilt::kBlend) | (1u << sfilt::kDepth))) return false;
  void** vt = *reinterpret_cast<void***>(g_ctx);
  if (g_slotDss < 0) g_slotDss = DcsQv_ContextSlot("OMSetDepthStencilState"), g_slotBs = DcsQv_ContextSlot("OMSetBlendState");
  if (g_slotDss < 0 || g_slotBs < 0) return false;
  void* d = vt[g_slotDss];
  void* b = vt[g_slotBs];
  if (d3ds::InPayload(d) || d3ds::InPayload(b)) return false;
  g_table = vt, g_origDss = d, g_origBs = b;
  // What is bound now (a draw before DCS's first set would use it): remembered, swapped.
  ID3D11DepthStencilState* cs = nullptr;
  UINT cref = 0;
  g_ctx->OMGetDepthStencilState(&cs, &cref);
  ID3D11BlendState* cb = nullptr;
  FLOAT bf[4] = {};
  UINT sm = 0;
  g_ctx->OMGetBlendState(&cb, bf, &sm);
  g_wantDss = cs, g_wantRef = cref, g_wantBs = cb, g_wantMask = sm;
  memcpy(g_wantBf, bf, sizeof(bf));
  if (cs) cs->Release();  // DCS keeps its own references; ours are only for the maps
  if (cb) cb->Release();
  g_testing = true;
  d3ds::WriteTablePointer(&vt[g_slotDss], reinterpret_cast<void*>(&H_Dss));
  d3ds::WriteTablePointer(&vt[g_slotBs], reinterpret_cast<void*>(&H_Bs));
  bool marking = false;
  reinterpret_cast<DssFn>(g_origDss)(g_ctx, MaskedDss(g_wantDss, g_wantRef, &marking), g_wantRef);
  reinterpret_cast<BsFn>(g_origBs)(g_ctx, MaskedBs(g_wantBs), g_wantBf, g_wantMask);
  return true;
}
void EndTest() {
  if (!g_testing) return;
  g_testing = false;
  void** vt = g_table;
  if (vt[g_slotDss] == reinterpret_cast<void*>(&H_Dss)) d3ds::WriteTablePointer(&vt[g_slotDss], g_origDss);
  if (vt[g_slotBs] == reinterpret_cast<void*>(&H_Bs)) d3ds::WriteTablePointer(&vt[g_slotBs], g_origBs);
  // DCS's own last requests, so the pipeline holds what DCS believes it holds.
  reinterpret_cast<DssFn>(g_origDss)(g_ctx, g_wantDss, g_wantRef);
  reinterpret_cast<BsFn>(g_origBs)(g_ctx, g_wantBs, g_wantBf, g_wantMask);
}

// Predicated block: every UpdateSubresource on DCS's context goes through gpu_pass_timing.h's hook (when its session
// holds the slot) or ours. False (nothing set) when the slot holds another hook of ours: the block then runs
// unpredicated ("hook taken").
bool g_updOurs = false;
bool BeginUnpred(ID3D11Predicate* p) {
  if (g_slotUpd < 0) g_slotUpd = DcsQv_ContextSlot("UpdateSubresource");
  if (g_slotUpd < 0) return false;
  void** vt = *reinterpret_cast<void***>(g_ctx);
  void* u = vt[g_slotUpd];
  g_updOurs = false;
  if (u == gpt::kHooks[gpt::kUpdateSubresource]) {
    // gpu_pass_timing.h's hook: it reads ptiming::g_unpred.
  } else if (d3ds::InPayload(u)) {
    return false;
  } else {
    g_origUpd = u;
    d3ds::WriteTablePointer(&vt[g_slotUpd], reinterpret_cast<void*>(&H_Upd));
    g_updOurs = true;
  }
  ptiming::g_upRec.clear();
  ptiming::g_unpred.store(p, std::memory_order_relaxed);
  g_ctx->SetPredication(p, FALSE);
  return true;
}
void EndUnpred() {
  g_ctx->SetPredication(nullptr, FALSE);
  ptiming::g_unpred.store(nullptr, std::memory_order_relaxed);
  // The last upload of each buffer again, unpredicated: the buffers hold what they hold without W1, water or not.
  for (ptiming::UpRec& r : ptiming::g_upRec) {
    if (g_updOurs) reinterpret_cast<UpdFn>(g_origUpd)(g_ctx, r.d, r.sub, r.hasBox ? &r.box : nullptr, r.bytes.data(), r.rp, r.dp);
    else g_ctx->UpdateSubresource(r.d, r.sub, r.hasBox ? &r.box : nullptr, r.bytes.data(), r.rp, r.dp);
    r.d->Release();
  }
  ptiming::g_upRec.clear();
  if (g_updOurs) {
    void** vt = *reinterpret_cast<void***>(g_ctx);
    if (vt[g_slotUpd] == reinterpret_cast<void*>(&H_Upd)) d3ds::WriteTablePointer(&vt[g_slotUpd], g_origUpd);
    g_updOurs = false;
  }
}

// ---------------------------------------------------------------------------
// Verify mode: M's targets as mode 1 would leave them
// ---------------------------------------------------------------------------
// M's render targets are found once (bound after M ran); each gets a private copy that is refreshed from DCS's
// texture only when mode 1 would have run M on the GPU (stock / forced views always, tested views under the test's
// predicate). Before a verify test the copy goes back into DCS's texture; the real M then draws it again (M's Wave
// technique is a full-screen pass per target).
struct MTarget {
  ID3D11Resource* dcs = nullptr;
  ID3D11Resource* copy = nullptr;
  bool valid = false;  // the copy holds an M output
};
constexpr int kMaxTargets = 4;
MTarget g_mt[kMaxTargets];
int g_nMt = -1;  // -1: not looked for yet
void FindMTargets() {
  if (g_nMt >= 0) return;
  g_nMt = 0;
  ID3D11RenderTargetView* rtv[8] = {};
  ID3D11DepthStencilView* dsv = nullptr;
  g_ctx->OMGetRenderTargets(8, rtv, &dsv);
  if (dsv) dsv->Release();
  char desc[256] = {};
  int n = 0;
  for (int i = 0; i < 8; ++i) {
    if (!rtv[i]) continue;
    ID3D11Resource* res = nullptr;
    rtv[i]->GetResource(&res);
    rtv[i]->Release();
    ID3D11Texture2D* t = nullptr;
    if (res && SUCCEEDED(res->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&t)))) {
      D3D11_TEXTURE2D_DESC td{};
      t->GetDesc(&td);
      t->Release();
      n += snprintf(desc + n, sizeof(desc) - n, " %ux%u x%u fmt %d;", td.Width, td.Height, td.ArraySize, td.Format);
      td.BindFlags = 0, td.CPUAccessFlags = 0, td.MiscFlags = 0, td.Usage = D3D11_USAGE_DEFAULT;
      ID3D11Texture2D* c = nullptr;
      if (g_nMt < kMaxTargets && td.SampleDesc.Count == 1 && SUCCEEDED(g_dev->CreateTexture2D(&td, nullptr, &c))) {
        g_mt[g_nMt].dcs = res, g_mt[g_nMt].copy = c, g_mt[g_nMt].valid = false;
        ++g_nMt;
        continue;
      }
    }
    if (res) res->Release();
  }
  Log("water skip: verify: the water mask pass's targets:%s %d kept for the mode-1 view of the test", n ? desc : " none",
      g_nMt);
}
void ReleaseMTargets() {
  for (int i = 0; i < kMaxTargets; ++i) {
    if (g_mt[i].dcs) g_mt[i].dcs->Release(), g_mt[i].dcs = nullptr;
    if (g_mt[i].copy) g_mt[i].copy->Release(), g_mt[i].copy = nullptr;
    g_mt[i].valid = false;
  }
  g_nMt = -1;
}
// After a real M; predicated: the caller set the test's predicate (mode 1 would have run M only with water). A copy
// that never held an M output is only filled unpredicated.
void SaveMTargets(bool predicated) {
  for (int i = 0; i < (g_nMt > 0 ? g_nMt : 0); ++i) {
    if (predicated && !g_mt[i].valid) continue;
    g_ctx->CopyResource(g_mt[i].copy, g_mt[i].dcs);
    g_mt[i].valid = true;
  }
}
void LoadMTargets() {  // before a verify test
  for (int i = 0; i < (g_nMt > 0 ? g_nMt : 0); ++i)
    if (g_mt[i].valid) g_ctx->CopyResource(g_mt[i].dcs, g_mt[i].copy);
}

// ---------------------------------------------------------------------------
// Per view, per frame (render thread)
// ---------------------------------------------------------------------------
uint64_t g_frame = ~0ull;
int g_view = -1;
enum St : uint8_t { kIdle, kWantRefr, kWantRivers, kWantSea, kStockRefr, kStockRivers, kStockSea };
St g_st = kIdle;
void* g_pM = nullptr;
void* g_pR = nullptr;
void* g_pRiv = nullptr;
uint64_t g_execArg = 0;
bool g_testOk = false;   // the test hooks went in for this view
bool g_testOpen = false; // the view's predicate is open
bool g_skipView = false; // mode 3: this view is not submitted (its passes are held, then dropped at the sea)
ViewPlan g_plan = kPlanTest;  // this view's plan, decided at its mask pass
Gate g_gate;
uint64_t g_lastCuts = 0;
int64_t g_lastFrameQpc = 0;
// Counters (read by the worker's log line)
std::atomic<uint64_t> g_cTested[kViews], g_cSkipped[kViews], g_cKept[kViews], g_cForced[kViews], g_cStock[kViews],
    g_cViolR[kViews], g_cViolS[kViews], g_cReasons[kRsCount], g_cSwitchStock{0}, g_cSwitchTest{0}, g_cArms{0};
// Mode 4: view-frames with real water that mode 3 would not have drawn (skipped / probed), and the frame runs.
std::atomic<uint64_t> g_cWrongSkip[kViews], g_cWrongProbe[kViews], g_cBadFrames{0}, g_cBadEpisodes{0}, g_cBadLongest{0};
BadRuns g_bad;  // render thread
FILE* g_csv = nullptr;
std::wstring g_csvPath;

void NoOp(void*, uint64_t) {}
// M's CPU side at its own slot, its GPU work under a predicate known FALSE: the constant buffers it uploads (the
// view's cbWaterParams: wave time, grid, matrices) are current for the test, which then sees what the real passes see.
void MPrepass(void* pass, uint64_t frame) {
  if (g_false && BeginUnpred(g_false)) {
    ptiming::g_orig(pass, frame);
    EndUnpred();
  } else {
    // No unpredicated uploads possible: M runs for real (a correct, slower view).
    ptiming::g_orig(pass, frame);
  }
}

inline FrameRec& Cur() { return g_ring[g_cur]; }

void OpenTest(FrameRec& f) {
  if (g_testOpen) return;
  g_ctx->Begin(f.r[g_view].pred);
  g_testOpen = true;
  f.testOn[g_view] = true;
  g_unknown = false;
}
// Runs one test pass (rivers or sea) between its occlusion query's Begin/End.
void RunTest(void* pass, uint64_t frame, ID3D11Query* q) {
  g_ctx->Begin(q);
  if (g_testOk && BeginTest()) {
    ptiming::g_orig(pass, frame);
    EndTest();
  } else {
    g_testOk = false;
  }
  g_ctx->End(q);
}

// Runs what is held back, unpredicated, before an unexpected pass (or at a mode change).
void Flush(uint8_t reason) {
  const bool view = g_cur >= 0 && g_view >= 0 && g_view < kViews;
  if (g_st == kIdle || g_st == kStockRefr || g_st == kStockRivers || g_st == kStockSea) {
    if (view && g_st == kStockSea && Cur().gtOn[g_view])  // rivers counted, no sea pass: its query counts nothing
      g_ctx->Begin(Cur().r[g_view].gtS), g_ctx->End(Cur().r[g_view].gtS);
    g_st = kIdle;
    return;
  }
  // A skipped view (mode 3) runs what it held, as any forced view: the view is drawn stock, nothing was tested.
  EndTest();
  if (view) {
    FrameRec& f = Cur();
    if (g_testOpen) {  // the rivers test ran, the sea's did not: its query counts nothing
      g_ctx->Begin(f.r[g_view].occS), g_ctx->End(f.r[g_view].occS);
      g_ctx->End(f.r[g_view].pred);
    }
    f.state[g_view] = kStForced, f.reason[g_view] = reason;
  }
  g_testOpen = false;
  g_skipView = false;
  g_cReasons[reason < kRsCount ? reason : 0].fetch_add(1, std::memory_order_relaxed);
  if (g_pM) ptiming::g_orig(g_pM, g_execArg);
  if (g_pR) ptiming::g_orig(g_pR, g_execArg);
  if (g_pRiv) ptiming::g_orig(g_pRiv, g_execArg);  // its test ran; the real rivers did not
  g_pM = g_pR = g_pRiv = nullptr;
  g_st = kIdle;
}

void TestRivers(void* pass, uint64_t frame) {
  FrameRec& f = Cur();
  OpenTest(f);
  if (IsVerify(g_rMode)) LoadMTargets();
  RunTest(pass, frame, f.r[g_view].occR);
  g_st = kWantSea;
}

void SeaBlock(void* pass, uint64_t frame) {
  FrameRec& f = Cur();
  Rec& r = f.r[g_view];
  const bool riversTested = g_testOpen;
  OpenTest(f);
  if (!riversTested) {  // no rivers pass: its query counts nothing
    g_ctx->Begin(r.occR), g_ctx->End(r.occR);
    if (IsVerify(g_rMode)) LoadMTargets();
  }
  RunTest(pass, frame, r.occS);
  g_ctx->End(r.pred);
  g_testOpen = false;
  const bool verify = IsVerify(g_rMode);
  const ViewPlan plan = g_plan;
  f.plan[g_view] = plan;
  uint8_t reason = !g_testOk ? kRsHookTaken : g_unknown ? kRsUnknownDraw : plan == kPlanForced ? kRsStart : kRsNone;
  if (g_rMode == 3 && reason == kRsNone) {
    // Mode 3's probe: the test alone; M ran its CPU side only, R and the real water passes are not drawn.
    f.reason[g_view] = reason;
    f.state[g_view] = kStProbe;
    g_pM = g_pR = g_pRiv = nullptr;
    g_st = kIdle;
    return;
  }
  bool predicated = !verify && reason == kRsNone;
  if (predicated && !BeginUnpred(r.pred)) predicated = false, reason = kRsHookTaken;
  if (!verify && reason != kRsNone) g_cReasons[reason].fetch_add(1, std::memory_order_relaxed);
  f.reason[g_view] = reason;
  f.state[g_view] = verify ? kStVerify : predicated ? kStPredicated : kStForced;
  if (g_pM) {
    ptiming::g_orig(g_pM, g_execArg);
    if (verify) {
      FindMTargets();
      if (g_rMode == 4) {
        // Mode 3 runs M on the GPU in its stock and forced views only (a probe runs M's CPU side alone).
        if (plan != kPlanTest || reason != kRsNone) SaveMTargets(false);
      } else {
        // Mode 1 runs M on the GPU in a forced view always, in a tested one only when its test saw water.
        const bool pred = plan == kPlanTest && reason == kRsNone;
        if (pred) g_ctx->SetPredication(r.pred, FALSE);
        SaveMTargets(pred);
        if (pred) g_ctx->SetPredication(nullptr, FALSE);
      }
    }
  }
  if (g_pR) ptiming::g_orig(g_pR, g_execArg);
  if (verify) g_ctx->Begin(r.gtR), f.gtOn[g_view] = true;
  if (g_pRiv) ptiming::g_orig(g_pRiv, g_execArg);
  if (verify) g_ctx->End(r.gtR), g_ctx->Begin(r.gtS);
  ptiming::g_orig(pass, frame);  // the real SeaPass (its end callbacks follow in ptiming::Hook)
  if (verify) g_ctx->End(r.gtS);
  if (predicated) EndUnpred();
  g_pM = g_pR = g_pRiv = nullptr;
  g_st = kIdle;
}

// Stock views: the real passes run with their sample counts (for the switch back to testing; in verify mode the
// gate's stock decision is only logged).
void StockRivers(void* pass, uint64_t frame) {
  FrameRec& f = Cur();
  if (!f.gtOn[g_view]) g_ctx->Begin(f.r[g_view].gtR), f.gtOn[g_view] = true;
  ptiming::g_orig(pass, frame);
  g_ctx->End(f.r[g_view].gtR);
  g_st = kStockSea;
}
void StockSea(void* pass, uint64_t frame) {
  FrameRec& f = Cur();
  if (!f.gtOn[g_view]) {  // no rivers pass: the rivers query counts nothing
    g_ctx->Begin(f.r[g_view].gtR), g_ctx->End(f.r[g_view].gtR);
    f.gtOn[g_view] = true;
  }
  g_ctx->Begin(f.r[g_view].gtS);
  ptiming::g_orig(pass, frame);
  g_ctx->End(f.r[g_view].gtS);
  g_st = kIdle;
}

// Reads the frames whose queries are done (never waits) and feeds the gate.
void Poll() {
  if (g_cur < 0) return;
  for (int k = kRing - 1; k >= 0; --k) {  // oldest first (k = 0: the frame that just ended): the gate sees frames in order
    FrameRec& f = g_ring[(g_cur - k + kRing) % kRing];
    if (!f.used) continue;
    bool ready = true, water = false;
    uint64_t oR[kViews] = {}, oS[kViews] = {}, gR[kViews] = {}, gS[kViews] = {};
    BOOL pr[kViews] = {};
    auto get = [&](ID3D11Asynchronous* q, void* out, UINT n) {
      return g_ctx->GetData(q, out, n, D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK;
    };
    for (int v = 0; v < kViews && ready; ++v) {
      if (!f.state[v]) continue;
      if (f.testOn[v])
        ready = get(f.r[v].occR, &oR[v], sizeof(uint64_t)) && get(f.r[v].occS, &oS[v], sizeof(uint64_t)) &&
                get(f.r[v].pred, &pr[v], sizeof(BOOL));
      if (ready && f.gtOn[v])
        ready = get(f.r[v].gtR, &gR[v], sizeof(uint64_t)) && get(f.r[v].gtS, &gS[v], sizeof(uint64_t));
    }
    if (!ready) {
      if (k < kRing - 2) break;  // newer frames wait for this one: the gate sees frames in order
      f.used = false;           // too old: dropped
      continue;
    }
    bool bad = false;  // mode 4: a view mode 3 would have drawn wrong
    for (int v = 0; v < kViews; ++v) {
      const uint8_t s = f.state[v];
      if (!s) continue;
      const uint64_t test = oR[v] + oS[v], real = gR[v] + gS[v];
      if (s == kStStock) {
        g_cStock[v].fetch_add(1, std::memory_order_relaxed);
        if (real) water = true;
      } else if (s == kStSkipped) {
        g_cSkipped[v].fetch_add(1, std::memory_order_relaxed);  // nothing known: nothing for the gate
      } else if (s == kStVerifySkip) {
        g_cSkipped[v].fetch_add(1, std::memory_order_relaxed);  // what mode 3 knows: nothing
        if (NoSubmitWrong(kPlanSkip, real)) g_cWrongSkip[v].fetch_add(1, std::memory_order_relaxed), bad = true;
      } else if (s == kStProbe) {
        g_cTested[v].fetch_add(1, std::memory_order_relaxed);
        if (test) water = true, g_cKept[v].fetch_add(1, std::memory_order_relaxed);
      } else {
        g_cTested[v].fetch_add(1, std::memory_order_relaxed);
        if (s == kStForced) g_cForced[v].fetch_add(1, std::memory_order_relaxed);
        else if (s == kStVerify || pr[v]) g_cKept[v].fetch_add(1, std::memory_order_relaxed);
        else g_cSkipped[v].fetch_add(1, std::memory_order_relaxed);
        // Verify: the gate follows what mode 1 would have counted (the real passes in its stock views).
        if (s == kStVerify && f.plan[v] == kPlanStock ? real != 0 : test != 0) water = true;
        if (s == kStVerify) {
          if (!oR[v] && gR[v] && !test) g_cViolR[v].fetch_add(1, std::memory_order_relaxed);
          if (!oS[v] && gS[v] && !test) g_cViolS[v].fetch_add(1, std::memory_order_relaxed);
          if (g_rMode == 4 && f.reason[v] == kRsNone && NoSubmitWrong(static_cast<ViewPlan>(f.plan[v]), real))
            g_cWrongProbe[v].fetch_add(1, std::memory_order_relaxed), bad = true;
        }
      }
      if (g_csv)
        fprintf(g_csv, "%llu,%d,%u,%u,%u,%llu,%llu,%llu,%llu,%d\n", static_cast<unsigned long long>(f.frame), v, s,
                f.reason[v], f.plan[v], static_cast<unsigned long long>(oR[v]), static_cast<unsigned long long>(oS[v]),
                static_cast<unsigned long long>(gR[v]), static_cast<unsigned long long>(gS[v]),
                f.testOn[v] ? (pr[v] ? 1 : 0) : -1);
    }
    if (g_rMode == 4) {
      g_bad.Frame(bad);
      g_cBadFrames.store(g_bad.frames, std::memory_order_relaxed);
      g_cBadEpisodes.store(g_bad.episodes, std::memory_order_relaxed);
      g_cBadLongest.store(g_bad.longest, std::memory_order_relaxed);
    }
    const bool wasStock = g_gate.stock;
    const uint64_t toTest = g_gate.toTest;
    g_gate.OnFrameRead(water);
    if (!wasStock && g_gate.stock) g_cSwitchStock.fetch_add(1, std::memory_order_relaxed);
    if (g_gate.toTest != toTest) g_cSwitchTest.fetch_add(1, std::memory_order_relaxed);
    f.used = false;
  }
}

void NewFrame(uint64_t frame) {
  if (g_st != kIdle) Flush(kRsPassOrder);
  g_frame = frame;
  g_view = -1;
  // Start guard again after a camera cut or a pause without frames.
  const uint64_t cuts = scenecut::g_cuts.load(std::memory_order_relaxed);
  LARGE_INTEGER q;
  QueryPerformanceCounter(&q);
  const bool gap = g_lastFrameQpc && (q.QuadPart - g_lastFrameQpc) * g_qpcToUs > g_gapMs * 1000.0;
  if (cuts != g_lastCuts || gap) {
    if (gap) ReleaseMTargets();  // a mission load may have made new ones
    g_gate.Arm();
    g_cArms.fetch_add(1, std::memory_order_relaxed);
  }
  g_lastCuts = cuts;
  g_lastFrameQpc = q.QuadPart;
  Poll();
  g_gate.BeginFrame();
  g_cur = (g_cur + 1) % kRing;
  FrameRec& f = g_ring[g_cur];
  f.used = true, f.frame = frame;
  memset(f.state, 0, sizeof(f.state)), memset(f.reason, 0, sizeof(f.reason)), memset(f.plan, 0, sizeof(f.plan));
  memset(f.testOn, 0, sizeof(f.testOn)), memset(f.gtOn, 0, sizeof(f.gtOn));
}

void OpenCsv() {
  if (g_csv || g_csvPath.empty()) return;
  _wfopen_s(&g_csv, g_csvPath.c_str(), L"a");
  if (!g_csv) return;
  setvbuf(g_csv, nullptr, _IOFBF, 1 << 20);
  fprintf(g_csv, "frame,view,state,reason,mode1_plan,test_rivers,test_sea,real_rivers,real_sea,predicate\n");
}

ExecFn Intercept(void* pass, uint64_t frame) {
  const int bm = g_benchMode.load(std::memory_order_relaxed);
  const int want = SanitizeMode(bm >= 0 ? bm : g_mode.load(std::memory_order_relaxed));
  if (want != g_rMode) {
    if (g_st != kIdle) Flush(kRsPassOrder);
    g_rMode = want;
    g_gate.Start();
    g_bad.cur = 0;
    if (g_csv && !IsVerify(want)) fclose(g_csv), g_csv = nullptr;
    if (IsVerify(want)) OpenCsv();
  }
  if (g_timing.load(std::memory_order_relaxed)) {  // the timing phase classifies in both arms
    const PK tk = KindOf(pass);
    if (tk != kOther) {
      std::lock_guard<std::mutex> lock(g_passMu);
      g_passObjs[pass] = tk;
    }
  }
  if (!g_rMode) return nullptr;
  const PK k = KindOf(pass);
  const uint64_t fr = ptiming::g_execFrame.load(std::memory_order_relaxed);
  if (fr != g_frame) {
    if (!EnsureDevice()) return nullptr;
    NewFrame(fr);
  }
  if (k == kOther) {
    if (g_st != kIdle) Flush(kRsPassOrder);  // anything else between a view's water passes: what is held runs now
    return nullptr;
  }
  // A water pass out of the expected order: what is held back runs now, unpredicated.
  const bool expected = k == kMask ? g_st == kIdle
                      : k == kRefr ? (g_st == kWantRefr || g_st == kStockRefr)
                      : k == kRivers ? (g_st == kWantRefr || g_st == kWantRivers || g_st == kStockRefr || g_st == kStockRivers)
                      : (g_st == kWantRefr || g_st == kWantRivers || g_st == kWantSea || g_st == kStockRefr ||
                         g_st == kStockRivers || g_st == kStockSea);
  if (!expected && g_st != kIdle) Flush(kRsPassOrder);
  switch (k) {
    case kMask: {
      if (++g_view >= kViews) return nullptr;
      g_execArg = frame;
      g_skipView = false;
      g_plan = PlanFor(g_gate, g_rMode, g_frame, g_view, g_probeEvery.load(std::memory_order_relaxed));
      if ((g_rMode == 1 || g_rMode == 3) && g_plan == kPlanStock) {
        Cur().state[g_view] = kStStock;
        g_st = kStockRefr;
        return nullptr;
      }
      if (g_rMode == 4 && g_plan == kPlanSkip) {  // verify: drawn, counted; mode 3 would not submit it
        Cur().state[g_view] = kStVerifySkip;
        g_st = kStockRefr;
        return nullptr;
      }
      g_pM = pass, g_pR = g_pRiv = nullptr, g_st = kWantRefr;
      if (g_rMode == 3 && g_plan == kPlanSkip) {  // held (a foreign pass before the sea runs them), then dropped
        g_skipView = true;
        return &NoOp;
      }
      g_testOk = true;
      return &MPrepass;
    }
    case kRefr:
      if (g_st == kStockRefr) {
        g_st = kStockRivers;
        return nullptr;
      }
      if (g_st != kWantRefr) return nullptr;
      g_pR = pass, g_st = kWantRivers;
      return &NoOp;
    case kRivers:
      if (g_st == kStockRefr || g_st == kStockRivers) return &StockRivers;
      if (g_st != kWantRefr && g_st != kWantRivers) return nullptr;
      g_pRiv = pass;
      if (g_skipView) {
        g_st = kWantSea;
        return &NoOp;
      }
      return &TestRivers;
    case kSea:
      if (g_st == kStockRefr || g_st == kStockRivers || g_st == kStockSea) return &StockSea;
      if (g_st != kWantRefr && g_st != kWantRivers && g_st != kWantSea) return nullptr;
      if (g_skipView) {  // mode 3: the view's water passes are not submitted
        Cur().state[g_view] = kStSkipped;
        g_pM = g_pR = g_pRiv = nullptr;
        g_skipView = false;
        g_st = kIdle;
        return &NoOp;
      }
      return &SeaBlock;
    default:
      return nullptr;
  }
}

// Worker thread: the key (kill switch applied by the caller). The intercept stays installed once set (one atomic load
// per top-level pass while the key is 0); Shutdown takes it out.
void Apply(int m, int probe, const std::wstring& csvPath) {
  m = SanitizeMode(m);
  probe = SanitizeProbe(probe);
  if (probe != g_probeEvery.load()) {
    g_probeEvery = probe;
    if (NoSubmit(m)) Log("water skip: [Water] NoSubmitProbe=%d (a dry view is probed one frame in %d)", probe, probe);
  }
  if (m == g_mode.load()) return;
  if (m && !ptiming::g_orig) ptiming::Install();
  static bool notedWait = false;
  if (m && !ptiming::g_orig) {  // GraphicsCore.dll not loaded yet: the worker retries every second
    if (!notedWait) Log("water skip: [Water] SkipHiddenWater=%d waits for the pass execute hook (GraphicsCore.dll)", m);
    notedWait = true;
    return;
  }
  notedWait = false;
  if (g_csvPath.empty()) g_csvPath = csvPath;
  g_mode = m;
  if (m) ptiming::g_intercept.store(&Intercept);
  char probeNote[48] = "";
  if (NoSubmit(m)) snprintf(probeNote, sizeof(probeNote), "; probe every %d frames", g_probeEvery.load());
  Log("water skip: [Water] SkipHiddenWater=%d (%s)%s", m,
      m == 0   ? "off"
      : m == 1 ? "on: a view's water mask, refraction and water passes are drawn only when its water test sees water"
      : m == 2 ? "verify: the test runs and is logged next to the real water passes, nothing is skipped"
      : m == 3 ? "no-submit: a view found dry is not submitted, probed one frame in NoSubmitProbe; not exact after water "
                 "enters the view"
               : "verify of no-submit: everything drawn; the view-frames mode 3 would have drawn without their water are "
                 "counted",
      probeNote);
}

// Worker thread, every 10 s while the intercept is in: per view tested / skipped / kept / forced / stock frames.
void LogCounters() {
  static uint64_t last = 0;
  uint64_t tot = 0;
  for (int v = 0; v < kViews; ++v) tot += g_cTested[v].load() + g_cStock[v].load();
  if (tot == last) return;
  last = tot;
  auto u = [](const std::atomic<uint64_t>& a) { return static_cast<unsigned long long>(a.load()); };
  char t[512];
  int n = snprintf(t, sizeof(t), "water skip: per view tested/skipped/kept/forced/stock");
  for (int v = 0; v < kViews; ++v)
    n += snprintf(t + n, sizeof(t) - n, " | v%d %llu/%llu/%llu/%llu/%llu", v, u(g_cTested[v]), u(g_cSkipped[v]),
                  u(g_cKept[v]), u(g_cForced[v]), u(g_cStock[v]));
  Log("%s; forced: unknown draw %llu, hook taken %llu, pass order %llu, start %llu; start guard armed %llu (cut or "
      "pause); switches to stock %llu (water seen), to test %llu (%d dry frames); verify: real water while the test "
      "saw none, rivers %llu/%llu/%llu/%llu, sea %llu/%llu/%llu/%llu",
      t, u(g_cReasons[kRsUnknownDraw]), u(g_cReasons[kRsHookTaken]), u(g_cReasons[kRsPassOrder]), u(g_cReasons[kRsStart]),
      u(g_cArms), u(g_cSwitchStock), u(g_cSwitchTest), kDryFrames, u(g_cViolR[0]), u(g_cViolR[1]), u(g_cViolR[2]),
      u(g_cViolR[3]), u(g_cViolS[0]), u(g_cViolS[1]), u(g_cViolS[2]), u(g_cViolS[3]));
  if (g_mode.load() == 4)
    Log("water skip: no-submit verify (probe every %d): view-frames with real water mode 3 would not draw: skipped "
        "%llu/%llu/%llu/%llu, probed %llu/%llu/%llu/%llu; frames with any: %llu in %llu episodes, longest %llu frames",
        g_probeEvery.load(), u(g_cWrongSkip[0]), u(g_cWrongSkip[1]), u(g_cWrongSkip[2]), u(g_cWrongSkip[3]),
        u(g_cWrongProbe[0]), u(g_cWrongProbe[1]), u(g_cWrongProbe[2]), u(g_cWrongProbe[3]), u(g_cBadFrames),
        u(g_cBadEpisodes), u(g_cBadLongest));
}

// ---------------------------------------------------------------------------
// [Suite] WaterSkipTiming: OFF / ON blocks (OFF = key 0, ON = the configured mode, 1 when it is 0), each with the GPU
// frame period (gpu_pass_timing.h, timestamps only), the render thread's CPU time per frame and the render-thread time
// inside the water passes; each ON block against the mean of its two OFF neighbours.
// ---------------------------------------------------------------------------
struct Block {
  bool on = false;
  uint64_t frames = 0;
  double periodMs = 0, busyMs = 0, rtCpuMs = 0, waterPassMs = 0, topPassMs = 0;
  double kindMs[4] = {};  // render-thread time per kind: mask, refraction, rivers, sea
  int periods = 0;
};

uint64_t RenderThreadCycles(DWORD tid) {
  ULONG64 c = 0;
  if (!tid) return 0;
  if (HANDLE h = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, tid)) {
    QueryThreadCycleTime(h, &c);
    CloseHandle(h);
  }
  return c;
}

bool WaitFrames(std::atomic<uint64_t>& frameCounter, uint64_t n, int timeoutMs) {
  const uint64_t f0 = frameCounter.load();
  const ULONGLONG t0 = GetTickCount64();
  while (frameCounter.load() - f0 < n) {
    if (GetTickCount64() - t0 > static_cast<ULONGLONG>(timeoutMs)) return false;
    Sleep(2);
  }
  return true;
}

Block MeasureBlock(bool on, int frames, std::atomic<uint64_t>& frameCounter, double tscHz) {
  Block b;
  b.on = on;
  gpt::TakeDone();
  const DWORD tid = ptiming::g_topTid.load();
  {
    std::lock_guard<std::mutex> lock(ptiming::g_mutex);
    ptiming::g_stats.clear();
    ptiming::g_topLevelUs = 0;
  }
  ptiming::g_recording = true;
  const uint64_t c0 = RenderThreadCycles(tid), f0 = frameCounter.load();
  WaitFrames(frameCounter, static_cast<uint64_t>(frames), 20000);
  const uint64_t c1 = RenderThreadCycles(tid), f1 = frameCounter.load();
  ptiming::g_recording = false;
  b.frames = f1 - f0;
  std::unordered_map<void*, ptiming::Stat> snap;
  double top = 0;
  {
    std::lock_guard<std::mutex> lock(ptiming::g_mutex);
    snap.swap(ptiming::g_stats);
    top = ptiming::g_topLevelUs;
  }
  double water = 0, kind[4] = {};
  {
    std::lock_guard<std::mutex> lock(g_passMu);
    for (const auto& kv : snap) {
      auto it = g_passObjs.find(kv.first);
      if (it == g_passObjs.end()) continue;
      water += kv.second.inclUs;
      if (it->second >= kMask && it->second <= kSea) kind[it->second - kMask] += kv.second.inclUs;
    }
  }
  const double f = b.frames ? static_cast<double>(b.frames) : 1.0;
  for (int i = 0; i < 4; ++i) b.kindMs[i] = kind[i] / 1000.0 / f;
  b.rtCpuMs = tscHz > 0 && c1 > c0 ? (c1 - c0) / tscHz * 1000.0 / f : -1;
  b.waterPassMs = water / 1000.0 / f;
  b.topPassMs = top / 1000.0 / f;
  Sleep(60);  // the block's last frames are read back
  std::vector<gpt::Done> done = gpt::TakeDone();
  gpt::Result r = gpt::Analyze(done, [](void*) -> std::string { return std::string(); });
  double busy = 0;
  int nb = 0;
  for (const gpt::Done& d : done) {
    double span = 0, bz = 0, xr = 0;
    if (gpt::FrameGpu(d, &span, &bz, &xr)) busy += bz, ++nb;
  }
  b.periods = static_cast<int>(r.period.size());
  b.periodMs = r.period.empty() ? -1 : gpt::Mean(r.period);
  b.busyMs = nb ? busy / nb : -1;
  return b;
}

void TimingPhase(int pairs, int cfgMode, int frames, std::atomic<uint64_t>& frameCounter, double tscHz,
                 const std::wstring& csvDir) {
  const int onMode = SanitizeMode(cfgMode) ? cfgMode : 1;
  if (!ptiming::g_orig) ptiming::Install();
  if (!ptiming::g_orig) {
    Log("  water skip timing: the pass execute hook is not installed; phase skipped");
    return;
  }
  if (!ptiming::g_intercept.load()) ptiming::g_intercept.store(&Intercept);
  if (!gpt::Begin(frameCounter, true)) {
    gpt::End();
    Log("  water skip timing: GPU timing did not start; phase skipped");
    return;
  }
  {
    std::lock_guard<std::mutex> lock(g_passMu);
    g_passObjs.clear();
  }
  g_timing = true;
  std::vector<Block> blocks;
  const int settle = 15;
  for (int i = 0; i < 2 * pairs + 1; ++i) {
    const bool on = (i & 1) != 0;
    g_benchMode = on ? onMode : 0;
    if (!WaitFrames(frameCounter, settle, 10000)) break;
    blocks.push_back(MeasureBlock(on, frames, frameCounter, tscHz));
  }
  g_benchMode = -1;
  g_timing = false;
  gpt::End();
  // Each ON block against the mean of its OFF neighbours.
  struct D {
    std::vector<double> v;
    void Add(double x) { v.push_back(x); }
    double Mean() const { double s = 0; for (double x : v) s += x; return v.empty() ? 0 : s / v.size(); }
    double Se() const {
      if (v.size() < 2) return 0;
      const double m = Mean();
      double s = 0;
      for (double x : v) s += (x - m) * (x - m);
      return std::sqrt(s / (v.size() - 1) / v.size());
    }
  } dPeriod, dBusy, dCpu, dWater, dTop;
  double offPeriod = 0, offCpu = 0, offWater = 0;
  int nOff = 0;
  for (size_t i = 1; i + 1 < blocks.size(); i += 2) {
    const Block &a = blocks[i - 1], &b = blocks[i], &c = blocks[i + 1];
    dPeriod.Add(b.periodMs - (a.periodMs + c.periodMs) / 2);
    dBusy.Add(b.busyMs - (a.busyMs + c.busyMs) / 2);
    dCpu.Add(b.rtCpuMs - (a.rtCpuMs + c.rtCpuMs) / 2);
    dWater.Add(b.waterPassMs - (a.waterPassMs + c.waterPassMs) / 2);
    dTop.Add(b.topPassMs - (a.topPassMs + c.topPassMs) / 2);
  }
  double kOff[4] = {}, kOn[4] = {};
  int nOn = 0;
  for (const Block& b : blocks) {
    if (!b.on) offPeriod += b.periodMs, offCpu += b.rtCpuMs, offWater += b.waterPassMs, ++nOff;
    else ++nOn;
    for (int i = 0; i < 4; ++i) (b.on ? kOn : kOff)[i] += b.kindMs[i];
  }
  for (int i = 0; i < 4; ++i) kOff[i] /= nOff ? nOff : 1, kOn[i] /= nOn ? nOn : 1;
  if (nOff) offPeriod /= nOff, offCpu /= nOff, offWater /= nOff;
  Log("  water skip timing: render-thread time per water pass kind, ms/frame, OFF -> ON: mask %.3f -> %.3f, refraction "
      "%.3f -> %.3f, rivers %.3f -> %.3f, sea %.3f -> %.3f",
      kOff[0], kOn[0], kOff[1], kOn[1], kOff[2], kOn[2], kOff[3], kOn[3]);
  SYSTEMTIME st;
  GetLocalTime(&st);
  wchar_t name[80];
  swprintf_s(name, L"water_timing_%04d%02d%02d_%02d%02d%02d.csv", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute,
             st.wSecond);
  FILE* csv = nullptr;
  _wfopen_s(&csv, (csvDir + name).c_str(), L"w");
  if (csv) {
    fprintf(csv, "block,on,mode,frames,gpu_period_ms,gpu_busy_ms,render_thread_cpu_ms,water_pass_ms,top_pass_ms,periods,"
                 "mask_ms,refraction_ms,rivers_ms,sea_ms\n");
    for (size_t i = 0; i < blocks.size(); ++i)
      fprintf(csv, "%zu,%d,%d,%llu,%.4f,%.4f,%.4f,%.4f,%.4f,%d,%.4f,%.4f,%.4f,%.4f\n", i, blocks[i].on ? 1 : 0,
              blocks[i].on ? onMode : 0, static_cast<unsigned long long>(blocks[i].frames), blocks[i].periodMs,
              blocks[i].busyMs, blocks[i].rtCpuMs, blocks[i].waterPassMs, blocks[i].topPassMs, blocks[i].periods,
              blocks[i].kindMs[0], blocks[i].kindMs[1], blocks[i].kindMs[2], blocks[i].kindMs[3]);
    fclose(csv);
  }
  Log("  water skip timing: %zu pairs (ON = mode %d), %d frames per block; ON minus neighbouring OFF (+- standard "
      "error): GPU frame period %+.3f +- %.3f ms (OFF %.3f), GPU busy in passes %+.3f +- %.3f ms; render thread CPU "
      "%+.3f +- %.3f ms/frame (OFF %.3f), render-thread time in the water passes %+.3f +- %.3f ms/frame (OFF %.3f), "
      "in all passes %+.3f +- %.3f ms/frame; blocks in %ls",
      dPeriod.v.size(), onMode, frames, dPeriod.Mean(), dPeriod.Se(), offPeriod, dBusy.Mean(), dBusy.Se(), dCpu.Mean(),
      dCpu.Se(), offCpu, dWater.Mean(), dWater.Se(), offWater, dTop.Mean(), dTop.Se(), name);
}

// Payload stop: the intercept out after the render thread flushed any view in flight, then our D3D objects.
void Shutdown() {
  if (!ptiming::g_intercept.load()) return;
  g_mode = 0;
  g_benchMode = -1;
  for (int i = 0; i < 20 && (g_rMode || g_st != kIdle); ++i) Sleep(10);  // the render thread flushes at its next pass
  ptiming::g_intercept.store(nullptr);
  Sleep(50);  // a render thread still inside Intercept finishes
  if (g_csv) fclose(g_csv), g_csv = nullptr;
  if (g_rMode || g_st != kIdle) return;  // the render thread never came back: its objects stay (never freed under it)
  auto rel = [](auto*& p) {
    if (p) p->Release(), p = nullptr;
  };
  for (FrameRec& f : g_ring) {
    f.used = false;
    for (Rec& r : f.r) rel(r.pred), rel(r.occR), rel(r.occS), rel(r.gtR), rel(r.gtS);
  }
  for (auto& kv : g_dssMap) {
    kv.first->Release();
    if (kv.second) kv.second->Release();
  }
  g_dssMap.clear();
  for (auto& kv : g_bsMap) {
    if (kv.first) kv.first->Release();
    if (kv.second) kv.second->Release();
  }
  g_bsMap.clear();
  ReleaseMTargets();
  rel(g_dssNever);
  rel(g_false);
  rel(g_ctx);
  rel(g_dev);
  g_frame = ~0ull;
  g_cur = -1;
}

}  // namespace w1
