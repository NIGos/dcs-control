// R32 (src/ovl_cpu.h): the cockpit symbology's camera set where DCS builds it.
// Included from cockpit_ipd_test.h inside namespace cktest (its matrix helpers,
// Check, the WARP device helpers), before cktest::Run.
//
// A replica of DCS's pipeline (no DCS needed):
//  - ContextBase (+0xb4 local, +0xf4 view, +0x134 proj, +0x1b4 viewProj,
//    +0x1f4 viewInv, +0x234 projInv) and the currentContext static; setCamera;
//    an updateBuffers clone (a cPerView-like b7 upload per pass).
//  - ccMTHandlerImpl as DCS lays it out: the record array at +0x20 (0xd0-byte
//    records: +0 / +8 the element's object pointers, both 0 for a state record,
//    +0xc8 / +0xc9 the filter bytes), the per-view packed ranges at +0x98 /
//    +0xa0, the extra filter at +0x4b0. The record replay with DCS's calling
//    convention (vt[3](this, view, ctx, std::function*)) walks the view's range
//    and calls the per-record filter through _Impl (+0x38) vt[+0x10] (&c8, &c9)
//    with copies of the record's bytes (the Visualizer bt-mask lambda;
//    CockpitBase's composite lambda {vt, handler, inner}, calling the inner
//    filter first), records drawn right after their filter call with MVP =
//    world * currentContext()->viewProj.
//  - FX11-style effects: Set (CPU copy, dirty bit), Apply (UpdateSubresource of
//    the whole buffer when dirty), Draw; variants: one buffer shared by every
//    record and view, an upload-skip cache (an unchanged buffer is not uploaded
//    again), Map(WRITE_DISCARD) dynamic buffers, several draws per record.
//  - A vertex shader float4(0, 0, 0, 1) * matWorldViewProj (dp4 with
//    cb0[0..3], as the ILS material) with stream output: every draw's clip
//    position as the GPU computed it from the bytes bound at that draw.
//  - Records: the collimated HUD built per eye (O = E_t + d * dir), HUD DED
//    repeater, HUD glyphs whose count changes per view and frame, DED, two MFD
//    LCD quads (their page content changing every other frame), EHSI, a
//    head-fixed JHMCS, an OverlayLDRPass panel, a focus-only display, a state
//    record without a draw, a noisy element; a world label (not a record) and
//    a 2D GUI draw. The glyphs, the repeater (B) and the DED (T) share one
//    (c9, c8) pair and one material, so a key by (c9, c8, ordinal) would alias
//    them; the MFDs and the focus-only display share another pair.
//  - Per view: its own record list (the focus views cull displays, glyph counts
//    differ per view), its own order (shuffled across elements).
//  - d3d11 table toggles behind our back (a submit that no hook sees, then the
//    first call through a toggled entry), missed observations, empty passes,
//    canted context views, a jittered G-buffer camera for the focus views.
// Every draw is classified per frame: DCS's camera, the true eye, or neither.
// Asserted: the HUD at zero disparity against a w = 0 point in every frame;
// cockpit displays on their true-eye bezels after the learner commits; world
// labels at DCS's camera; no A-B-A (no run shorter than the learner's commit
// streak between two runs of the same state); the context bytes restored bit
// for bit; the learned classes (views 2 / 3 measured too); the fail-safes (the
// pair rule, its recurrence, observer misses as failures, a context switched
// or rewritten mid-replay with DCS's camera put back, a record identity shared
// by a HUD and a display record, whole-replay scans without a filter point);
// the install order, its rollback, the uninstall during a replay; signatures
// against the DCS install's DLLs; off is bit-identical.
// R32 r3: one decision per frame for all four views (a stop blocks the rest of
// the frame and the next 30; views of one eye differ only in a frame whose
// later view failed after an earlier one was drawn; overlays off / on / inert
// latched per frame; a whole-frame scan without a filter point); the block
// checked before the camera (a 2-frame mismatch is one failure); per-key
// failures (cpu miss in every frame, a one-off wrong B) freeze the key with no
// A-B-A and no stop; learner resets on mission events and scene cuts (done on
// the render thread at a frame end; no stale class afterwards); frozen keys
// expire; the incremental learner (sweep, backward-shift deletion, counts); an
// uninstall whose replay outlives the drain keeps the hooks (pass-through) and
// a reinstall waits for it; C++ exceptions through DCS's filter; another
// thread's Unmap; far depth with CockpitTrueIpd=0 as before R32.
#pragma once

namespace ocpt {

namespace oc = ::fixdepth::ckipd::ocpu;
using cktest::Mat;
using cktest::Mul;
using cktest::Inv;
using cktest::Get;
using cktest::Put;
using cktest::Ident;

// ---------------------------------------------------------------------------
// DCS replica
// ---------------------------------------------------------------------------

alignas(16) uint8_t g_ctxA[0x300];
alignas(16) uint8_t g_ctxB[0x300];
uint8_t* g_curCtx = nullptr;  // ContextBase::currentContext

// The context's viewProj as DCS 2.9.30 keeps it: the row-vector VP row-major (the transpose of cPerView's layout,
// R32 r4); tests: g_ctxColMajor stores it in cPerView's layout instead (both are accepted per replay).
bool g_ctxColMajor = false;
Mat Tr(const Mat& m) {
  Mat t;
  for (int r = 0; r < 4; ++r)
    for (int c = 0; c < 4; ++c) t.a[r][c] = m.a[c][r];
  return t;
}
void PutVp(uint8_t* ctx, const Mat& m) { Put(ctx, 0x1b4, g_ctxColMajor ? m : Tr(m)); }
Mat GetVp(const uint8_t* ctx) {
  const Mat m = Get(ctx, 0x1b4);
  return g_ctxColMajor ? m : Tr(m);
}
ck::M4 CtxVp(const uint8_t* ctx) { return oc::LoadCtxVp(ctx + 0x1b4, !g_ctxColMajor); }

void SetCameraCtx(uint8_t* ctx, const Mat& V, const Mat& P) {
  Put(ctx, 0xb4, Ident());
  Put(ctx, 0x174, Ident());
  Put(ctx, 0xf4, V);
  Put(ctx, 0x134, P);
  PutVp(ctx, Mul(Get(ctx, 0xf4), Get(ctx, 0x134)));
  Put(ctx, 0x1f4, Inv(Get(ctx, 0xf4)));
  Put(ctx, 0x234, Inv(Get(ctx, 0x134)));
}

enum Kind : int {
  kHud = 0, kHudDed, kGlyph, kDed, kMfdL, kMfdR, kEhsi, kJhmcs, kPanel, kState, kNoisy, kFocus, kNewHud, kKinds
};
const char* const kKindNames[kKinds] = {"HUD", "HUD DED", "HUD glyph", "DED", "MFD L", "MFD R", "EHSI", "JHMCS",
                                        "LDR panel", "state", "noisy", "focus display", "new HUD element"};
inline bool KindT(int k) {
  return k == kDed || k == kMfdL || k == kMfdR || k == kEhsi || k == kJhmcs || k == kPanel || k == kFocus;
}
inline bool KindB(int k) { return k == kHud || k == kHudDed || k == kGlyph || k == kNewHud; }
// Elements (their object pointers: +0 per element, +8 per material) and materials.
enum Mat8 : int { kMatHud = 0, kMatMfd, kMatEhsi, kMatJhmcs, kMatPanel, kMatNoisy, kMatCount };
enum Elem : int {
  kEHud0 = 0, kEGlyph0 = 3, kERep = 7, kEDed = 8, kEMfdL = 9, kEMfdR = 10, kEEhsi = 11, kEJhmcs = 12, kEPanel = 13,
  kENoisy = 14, kEFocus = 15, kENewHud = 16, kElems = 17
};
uint64_t g_elemObj[kElems] = {};
uint64_t g_matObj[kMatCount] = {};
struct Rec {
  uint8_t c9 = 0, c8 = 0;
  int kind = 0, id = 0, sub = 0;  // id: the element (tracks); sub: glyph index / HUD element
  int elem = -1, mat = 0;         // the identity (elem -1: a state record)
};
// The key DCS's record of an element gets (ordinal 0: the element's first record in the replay).
uint64_t KeyOf(int elem, int mat, uint8_t c9, uint8_t c8, uint32_t ord = 0) {
  return oc::RecKey(oc::ElemBase(reinterpret_cast<uint64_t>(&g_elemObj[elem]), reinterpret_cast<uint64_t>(&g_matObj[mat]),
                                 c9, c8),
                    ord);
}

// MSVC std::function: the callable's _Impl pointer at +0x38.
struct FnObj {
  uint8_t storage[0x38] = {};
  void* impl = nullptr;
};
static_assert(offsetof(FnObj, impl) == 0x38, "std::function _Impl");
struct VisLambda {  // Visualizer 0x160710: mov eax, [rcx+8]; bt eax, *(u8*)r8
  void** vt = nullptr;
  uint32_t mask = 0;
};
static_assert(offsetof(VisLambda, mask) == 8, "the overlay lambda's mask");
struct Handler;
struct CompLambda {  // CockpitBase 0x3c3de0: [rcx+8] the handler, [rcx+0x10] the inner std::function
  void** vt = nullptr;
  Handler* handler = nullptr;
  FnObj* inner = nullptr;
};
using FilterFnT = bool (*)(void*, uint8_t*, uint8_t*);
using ReplayFnT = void (*)(void*, int, void*, void*);
void* g_handlerVt[8] = {};
void* g_visVt[8] = {};
void* g_compVt[8] = {};
void* g_unkVt[8] = {};  // a callable type DCS might pass that we do not hook

// ccMTHandlerImpl's fields the replay reads (CockpitBase 0x3c6570 / 0x3c6640).
struct Handler {
  void** vt = g_handlerVt;
  uint8_t pad0[0x18] = {};
  uint8_t* recs = nullptr;  // +0x20 the record array (0xd0 bytes each)
  uint8_t pad1[0x98 - 0x28] = {};
  int32_t* viewsB = nullptr;  // +0x98 / +0xa0 the per-view packed ranges (lo16 first, hi16 end)
  int32_t* viewsE = nullptr;
  uint8_t pad2[0x4b0 - 0xa8] = {};
  void* composite = nullptr;  // +0x4b0 DCS's extra filter (non-null: the composite lambda)
  int excludeC9 = -1;         // that filter: records of this type out (the composite returns its NOT)
};
static_assert(offsetof(Handler, recs) == 0x20 && offsetof(Handler, viewsB) == 0x98 &&
                  offsetof(Handler, viewsE) == 0xa0 && offsetof(Handler, composite) == 0x4b0,
              "ccMTHandlerImpl layout");
bool VisCall(void* self, uint8_t* c8, uint8_t* c9) {
  (void)c8;
  return (static_cast<VisLambda*>(self)->mask >> *c9 & 1u) != 0;
}
bool UnkCall(void* self, uint8_t* c8, uint8_t* c9) { return VisCall(self, c8, c9); }
// Tests: the composite filter throws a C++ exception at its n-th call from now (0: the next one; -1 never), as a
// DCS filter might; the replay either catches it per record (g_catchInReplay) or lets it leave the replay.
int g_throwIn = -1;
bool g_catchInReplay = false;
int g_caught = 0;
bool CompCall(void* self, uint8_t* c8, uint8_t* c9) {
  if (g_throwIn >= 0 && g_throwIn-- == 0) throw std::runtime_error("composite filter");
  CompLambda* c = static_cast<CompLambda*>(self);
  uint8_t a = *c8, b = *c9;  // DCS passes copies on its own stack
  if (c->inner && c->inner->impl) {
    void** vt = *static_cast<void***>(c->inner->impl);
    if (!reinterpret_cast<FilterFnT>(vt[2])(c->inner->impl, &a, &b)) return false;
  }
  return !(c->handler->excludeC9 >= 0 && b == c->handler->excludeC9);
}

// The frame's record lists per view (the cockpit thread's recording), packed into DCS's array.
std::vector<Rec> g_list[4];
std::vector<uint8_t> g_recArr;
int32_t g_ranges[4] = {};
void PackRecords(Handler& h) {
  size_t n = 0;
  for (int t = 0; t < 4; ++t) n += g_list[t].size();
  g_recArr.assign((n + 1) * oc::kRecSize, 0);
  uint32_t at = 0;
  for (int t = 0; t < 4; ++t) {
    const uint32_t lo = at;
    for (size_t i = 0; i < g_list[t].size(); ++i, ++at) {
      uint8_t* r = g_recArr.data() + static_cast<size_t>(at) * oc::kRecSize;
      const Rec& x = g_list[t][i];
      const uint64_t o0 = x.elem >= 0 ? reinterpret_cast<uint64_t>(&g_elemObj[x.elem]) : 0;
      const uint64_t o8 = x.elem >= 0 ? reinterpret_cast<uint64_t>(&g_matObj[x.mat]) : 0;
      memcpy(r, &o0, 8);
      memcpy(r + 8, &o8, 8);
      const int32_t idx = static_cast<int32_t>(i);
      memcpy(r + 0x50, &idx, 4);
      r[oc::kRecC8] = x.c8;
      r[oc::kRecC9] = x.c9;
    }
    g_ranges[t] = static_cast<int32_t>(lo | at << 16);
  }
  h.recs = g_recArr.data();
  h.viewsB = g_ranges;
  h.viewsE = g_ranges + 4;
}

void DrawRecord(int t, const Rec& r);  // below
// Tests: once, this kind's MVP is built with the camera the context held at the record drawn before it in the
// replay (an MVP builder caching the previous record's camera: our VP_true reaches a record it was not for).
int g_leakKind = -1;
Mat g_prevVp;
bool g_prevVpValid = false;
int g_ctxSwitchAt = -1;  // tests: currentContext pushed (a copy of the current one) after this many drawn records
int g_rewriteAt = -1;    // tests: DCS's setCamera on the context after this many drawn records
Mat g_rewriteV, g_rewriteP;
int g_drawnInReplay = 0;
// Tests: a callback after this many drawn records of this view's first replay (the uninstall during a replay).
int g_midView = -1, g_midAt = -1;
void (*g_midCb)() = nullptr;

// Graphics::RenderObject as CockpitBase's record loop fills it (0x3c6016) before it calls render through its import
// slot (0x3c6065): +0x60 / +0x68 the record's +8 / +0 object pointers, +0x78 the record + 0x40. The replica's own
// fields (the view and the record to draw) after them.
struct FakeRo {
  uint8_t pad[0x60] = {};
  uint64_t obj8 = 0, obj0 = 0;
  uint8_t pad2[0x78 - 0x70] = {};
  const uint8_t* recPlus40 = nullptr;
  int view = 0;
  const Rec* rec = nullptr;
};
static_assert(offsetof(FakeRo, obj8) == 0x60 && offsetof(FakeRo, obj0) == 0x68 && offsetof(FakeRo, recPlus40) == 0x78,
              "RenderObject fields the record loop sets");
using RenderFnT = void (*)(void*, uint32_t);
uint64_t g_renderImplCalls = 0;
// GraphicsVista's RenderObject::render: the record's material builds its MVP from the context and draws.
void RenderImpl(void* ro, uint32_t) {
  ++g_renderImplCalls;
  const FakeRo* r = static_cast<const FakeRo*>(ro);
  if (r->rec) DrawRecord(r->view, *r->rec);
}
void* g_renderIat[1] = {reinterpret_cast<void*>(&RenderImpl)};  // CockpitBase's import slot
int g_badRoAt = -1;  // tests: the RenderObject of the record drawn at this index names the next record (once)

// ccMTHandlerImpl vtable slot 3 (CockpitBase 0x3c6570 + 0x3c6640 + 0x3c5bd0).
void ReplayImpl(void* self, int view, void* ctx, void* filter) {
  (void)ctx;
  Handler* h = static_cast<Handler*>(self);
  if (view < 0 || view >= h->viewsE - h->viewsB) return;
  const uint32_t packed = static_cast<uint32_t>(h->viewsB[view]), lo = packed & 0xffffu, hi = packed >> 16;
  FnObj* f = static_cast<FnObj*>(filter);
  FnObj local;
  CompLambda comp;
  FnObj* use = f;
  if (h->composite) {
    comp.vt = g_compVt;
    comp.handler = h;
    comp.inner = f;
    local.impl = &comp;
    use = &local;
  }
  g_drawnInReplay = 0;
  g_prevVpValid = false;
  uint8_t* pushedFrom = nullptr;
  bool rewritten = false;
  for (uint32_t i = lo; i < hi; ++i) {
    const uint8_t* rec = h->recs + static_cast<size_t>(i) * oc::kRecSize;
    if (use && use->impl) {
      uint8_t c9 = rec[oc::kRecC9], c8 = rec[oc::kRecC8];
      void** vt = *static_cast<void***>(use->impl);
      bool pass = false;
      if (g_catchInReplay) {
        try {
          pass = reinterpret_cast<FilterFnT>(vt[2])(use->impl, &c8, &c9);
        } catch (const std::exception&) {
          ++g_caught;  // DCS's loop skips the record
          continue;
        }
      } else {
        pass = reinterpret_cast<FilterFnT>(vt[2])(use->impl, &c8, &c9);
      }
      if (!pass) continue;
    }
    int32_t idx = 0;
    memcpy(&idx, rec + 0x50, 4);
    const Rec& r = g_list[view][static_cast<size_t>(idx)];
    if (r.elem >= 0) {  // a drawing record: its RenderObject through the import slot (a state record: no render)
      FakeRo ro;
      memcpy(&ro.obj0, rec, 8);
      memcpy(&ro.obj8, rec + 8, 8);
      ro.recPlus40 = rec + 0x40;
      if (g_badRoAt == g_drawnInReplay) {
        ro.recPlus40 += oc::kRecSize;
        g_badRoAt = -1;
      }
      ro.view = view;
      ro.rec = &r;
      reinterpret_cast<RenderFnT>(g_renderIat[0])(&ro, 0);
    }
    if (r.kind != kState) ++g_drawnInReplay;
    if (view == g_midView && g_drawnInReplay == g_midAt && g_midCb) {
      void (*cb)() = g_midCb;
      g_midCb = nullptr;
      cb();
    }
    // Inside this record's render, after its draw (before the next record's filter call): DCS pushes another
    // context (a copy of the current one) or sets its camera again.
    if (g_drawnInReplay == g_ctxSwitchAt && !pushedFrom) {
      pushedFrom = g_curCtx;
      memcpy(g_ctxB, g_curCtx, sizeof(g_ctxB));
      g_curCtx = g_ctxB;
    }
    if (g_drawnInReplay == g_rewriteAt && !rewritten) {
      rewritten = true;
      SetCameraCtx(g_curCtx, g_rewriteV, g_rewriteP);
    }
  }
  if (pushedFrom) g_curCtx = pushedFrom;
}

// FX11-style effects.
enum FxMode : int { kFxDirty = 0, kFxShared, kFxSkip, kFxMap, kFxMulti, kFxModes };
const char* const kFxNames[kFxModes] = {"dirty bit, per indicator", "one buffer shared", "upload-skip cache",
                                        "Map WRITE_DISCARD", "several draws, shared, skip cache"};
struct Effect {
  ID3D11Buffer* cb = nullptr;
  bool dyn = false, dirty = false, everUp = false;
  uint8_t cpu[256] = {};
  uint8_t last[256] = {};
};
Effect g_fx[16];       // per indicator (default), [14] the shared one, [15] the shared dynamic one
Effect g_fxDyn[16];    // per indicator, dynamic (Map)
int g_fxMode = kFxDirty;
ID3D11DeviceContext1* g_c = nullptr;
ID3D11Buffer* g_so = nullptr;
ID3D11Buffer* g_soStage = nullptr;
ID3D11RenderTargetView* g_rtv = nullptr;
int g_missKind = -1;  // tests: this kind's MVP is built from DCS's camera, not from the context (an MVP builder
                      // that does not read currentContext+0x1b4)

// The table toggles behind our back: a submit no hook of ours sees (d3d11 inside a draw / state call), then the
// first call through a toggled entry (a clear), which writes the normal entries over our hooks.
int g_togglePlan = -1;  // toggle before the record with this draw index in the current replay (-1 none)
uint64_t g_toggles = 0;
void ToggleBehindOurBack() {
  void** vt = *reinterpret_cast<void***>(g_c);
  const int fs = DcsQv_ContextSlot("Flush");
  void* f = fixdepth::g_tpArmed[fixdepth::kTpFlush] ? fixdepth::g_tpOrig[fixdepth::kTpFlush] : vt[fs];
  reinterpret_cast<void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*)>(f)(g_c);
  const float z[4] = {};
  g_c->ClearRenderTargetView(g_rtv, z);
  ++g_toggles;
}

void Apply(Effect& e) {
  const bool skip = g_fxMode == kFxSkip || g_fxMode == kFxMulti;
  if (skip && e.everUp && !memcmp(e.cpu, e.last, 256)) e.dirty = false;  // the upload-skip cache
  if (e.dirty || (skip && (!e.everUp || memcmp(e.cpu, e.last, 256)))) {
    if (e.dyn) {
      D3D11_MAPPED_SUBRESOURCE m{};
      if (SUCCEEDED(g_c->Map(e.cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
        memcpy(m.pData, e.cpu, 256);
        g_c->Unmap(e.cb, 0);
      }
    } else {
      g_c->UpdateSubresource(e.cb, 0, nullptr, e.cpu, 0, 0);
    }
    memcpy(e.last, e.cpu, 256);
    e.everUp = true;
    e.dirty = false;
  }
  g_c->VSSetConstantBuffers(0, 1, &e.cb);
}
Effect& EffectOf(const Rec& r) {
  const int ind = r.kind % 12;
  switch (g_fxMode) {
    case kFxShared:
    case kFxMulti:
      return g_fx[14];
    case kFxMap:
      return g_fxDyn[ind];
    default:
      return g_fx[ind];
  }
}

// The scene of the frame (the test sets it per frame and view).
struct ViewScene {
  Mat V, P, VPf;   // DCS's (boosted) camera; VPf: the float VP in the context
  double E[3];     // the boosted eye
  double dw[3];    // true eye - boosted eye
  Mat VPtrueF;     // T(-dw) * VPf as ocpu writes it (floats)
};
ViewScene g_vs[4];
double g_H[3], g_HR[3][3], g_cockpit[3];
int g_frameNo = 0;
bool g_noisyEyeRel = false;
int g_mfdPage = 0;  // the MFD pages' content (bake at half rate: changes every other frame)

// The record's world matrix in view t (DCS's cockpit records).
Mat WorldOf(int t, const Rec& r) {
  double O[3];
  double R[3][3] = {{0.002, 0, 0}, {0, 0.002, 0}, {0, 0, 0.002}};  // glyph scale, aircraft axes
  auto cockpitPoint = [&](double x, double y, double z) {
    O[0] = g_cockpit[0] + x, O[1] = g_cockpit[1] + y, O[2] = g_cockpit[2] + z;
  };
  auto eyeRel = [&](double d, double ax, double ay) {  // collimated: per eye, at d along a fixed aircraft direction
    const double dir[3] = {ax, ay, 1.0};
    const double n = std::sqrt(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
    for (int i = 0; i < 3; ++i) O[i] = g_vs[t].E[i] + d * dir[i] / n;
  };
  switch (r.kind) {
    case kHud: eyeRel(0.75, -0.05 + 0.03 * r.sub, 0.02); break;
    case kHudDed: eyeRel(0.75, 0.0, -0.06); break;
    case kGlyph: eyeRel(0.75, 0.04, 0.05 - 0.01 * r.sub); break;
    case kNewHud: eyeRel(0.75, -0.08, 0.07); break;
    case kDed: cockpitPoint(0.05, -0.18, 0.55); break;
    case kMfdL: cockpitPoint(-0.17, -0.32, 0.6); break;
    case kMfdR: cockpitPoint(0.17, -0.32, 0.6); break;
    case kEhsi: cockpitPoint(0.0, -0.42, 0.58); break;
    case kPanel: cockpitPoint(0.25, -0.2, 0.5); break;
    case kFocus: cockpitPoint(0.0, -0.1, 0.45); break;
    case kJhmcs:  // fixed to the head, 0.4 m ahead of it, a little low
      for (int i = 0; i < 3; ++i) O[i] = g_H[i] + 0.4 * g_HR[2][i] - 0.05 * g_HR[1][i];
      break;
    case kNoisy:
      if (g_noisyEyeRel)
        eyeRel(0.9, 0.1, 0.1);
      else
        cockpitPoint(-0.3, -0.1, 0.7);
      break;
    default: cockpitPoint(0, 0, 0.5); break;
  }
  Mat W = Ident();
  for (int i = 0; i < 3; ++i)
    for (int j = 0; j < 3; ++j) W.a[i][j] = R[i][j];
  for (int i = 0; i < 3; ++i) W.a[3][i] = O[i];
  return W;
}

// One logged draw: what the GPU must have computed under DCS's camera / the true eye.
struct DrawLog {
  int frame = 0, t = 0, id = 0, kind = 0;
  double dcs[4] = {}, tru[4] = {}, ref[4] = {};  // clip row 3 of W * VP; the HUD's w = 0 point
};
std::vector<DrawLog> g_log;  // this frame's draws in order
bool g_logOn = true;

void Row3(const Mat& W, const Mat& VP, double out[4]) {
  for (int c = 0; c < 4; ++c)
    out[c] = W.a[3][0] * VP.a[0][c] + W.a[3][1] * VP.a[1][c] + W.a[3][2] * VP.a[2][c] + W.a[3][3] * VP.a[3][c];
}
void DrawOne(int t, int id, int kind, const Mat& W, const Mat* dirRef) {
  g_c->Draw(1, 0);
  if (!g_logOn) return;
  DrawLog d;
  d.frame = g_frameNo, d.t = t, d.id = id, d.kind = kind;
  Row3(W, g_vs[t].VPf, d.dcs);
  Row3(W, g_vs[t].VPtrueF, d.tru);
  if (dirRef) {  // the HUD's direction at infinity: (dir, 0) * VP
    for (int c = 0; c < 4; ++c)
      d.ref[c] = dirRef->a[3][0] * g_vs[t].VPf.a[0][c] + dirRef->a[3][1] * g_vs[t].VPf.a[1][c] +
                 dirRef->a[3][2] * g_vs[t].VPf.a[2][c];
  }
  g_log.push_back(d);
}

void DrawRecord(int t, const Rec& r) {
  if (r.kind == kState) return;  // a state record: no MVP, no draw
  if (g_togglePlan == g_drawnInReplay) ToggleBehindOurBack();
  const Mat W = WorldOf(t, r);
  // GraphicsVista's indicator material: getCurrentContext(), world * ctx+0x1b4, setMatrix x2, Apply, Draw.
  Mat VP = r.kind == g_missKind ? g_vs[t].VPf : GetVp(g_curCtx);
  if (r.kind == g_leakKind && g_prevVpValid) {
    VP = g_prevVp;
    g_leakKind = -1;
  }
  g_prevVp = GetVp(g_curCtx);
  g_prevVpValid = true;
  Effect& e = EffectOf(r);
  Put(e.cpu, 0, Mul(W, VP));
  Put(e.cpu, 64, W);
  {
    float col[4] = {0.2f * (r.kind % 5), 1.0f, (r.kind == kMfdL || r.kind == kMfdR) ? 0.1f * g_mfdPage : 0.5f, 1.0f};
    memcpy(e.cpu + 128, col, 16);
  }
  e.dirty = true;
  Apply(e);
  Mat dir;  // the HUD's direction (w = 0 reference): its origin minus the eye
  const bool hud = KindB(r.kind);
  if (hud)
    for (int i = 0; i < 3; ++i) dir.a[3][i] = W.a[3][i] - g_vs[t].E[i];
  const int n = g_fxMode == kFxMulti ? 2 + (r.id % 2) : 1;
  for (int k = 0; k < n; ++k) DrawOne(t, r.id, r.kind, W, hud ? &dir : nullptr);
}

// ---------------------------------------------------------------------------
// The harness
// ---------------------------------------------------------------------------

struct Harness {
  ID3D11Device* dev = nullptr;
  ID3D11DeviceContext* imm = nullptr;
  ID3D11VertexShader* vs = nullptr;
  ID3D11GeometryShader* gs = nullptr;
  ID3D11Texture2D* rtTex = nullptr;
  ID3D11Buffer* b7 = nullptr;
  Handler handler;
  VisLambda visHdr, visLdr, unk;
  FnObj fnHdr, fnLdr, fnNull, fnUnk;
  fdtest::FakePass hdr = fdtest::MakePass("OverlayHDRPass"),
                           ldr = fdtest::MakePass("OverlayLDRPass"), gui = fdtest::MakePass("GUIPass");
  bool ok = false;
};
Harness* g_h = nullptr;

// The view's cockpit G-buffer state as cockpit_ipd.h would have it after patching (the overlay path's inputs).
void SetViewState(int t, bool patched, const Mat& gbVp) {
  ck::ViewFrame& vf = ck::g_f.v[t];
  vf.patched = patched;
  vf.failed = false;
  vf.haveOrig = true;
  vf.dwReady = true;
  for (int i = 0; i < 3; ++i) vf.dw[i] = g_vs[t].dw[i];
  for (int i = 0; i < 3; ++i) vf.C[i] = g_vs[t].E[i];
  for (int i = 0; i < 3; ++i)
    for (int j = 0; j < 3; ++j) vf.R[i][j] = 0;
  ck::PrevStore& c = ck::g_cur[t];
  c.valid = true;
  c.tag = ck::g_frame + 1;
  memcpy(&c.vp, &gbVp, sizeof(c.vp));
}

// The frame's options.
struct FrameOpts {
  bool toggles = true;       // random toggles behind our back inside the passes
  bool shuffle = true;       // records reordered across elements, per view
  bool tFirst = false;       // (no shuffle) the cockpit displays first, the HUD after them
  bool nullFilter = false, unknownFilter = false;
  int camWrongView = -1;     // this view's G-buffer camera is another one (the camera identity fails)
  int ctxSwitchView = -1;    // DCS pushes another context mid-replay in this view's HDR replay
  int rewriteView = -1;      // DCS rewrites the context's camera mid-replay in this view's HDR replay
  bool rewriteSame = false;  // ... with its own same camera (the same bytes)
  bool patched = true;       // the cockpit G-buffer passes were moved this frame
  bool scale1 = false;       // Depth boost at scale 1: the frame's h 0, no view shifted or patched
  bool alias = false;        // the DED shares the HUD DED repeater's identity (pointers, c9, c8), placed first
  bool dropDed = false;      // the DED is not recorded this frame (any view)
  int missKind = -1;         // this kind's MVP builder does not read the context
  void (*edit)(int t, std::vector<Rec>& list) = nullptr;  // per-view list edit (the scan tests)
};
uint32_t g_rs = 7;
double g_yaw = 10, g_px = 0;
double R01() {
  g_rs = g_rs * 1664525u + 1013904223u;
  return (g_rs >> 8) / 16777216.0;
}
// Bit-exact restore of the context per replay.
uint64_t g_restoreBad = 0, g_replays = 0, g_rewriteKept = 0;

void Replay(int t, FnObj* f) {
  void** vt = g_h->handler.vt;
  alignas(16) uint8_t before[64];
  memcpy(before, g_curCtx + 0x1b4, 64);
  reinterpret_cast<ReplayFnT>(vt[3])(&g_h->handler, t, g_curCtx, f);
  ++g_replays;
  if (g_rewriteAt < 0 && memcmp(before, g_curCtx + 0x1b4, 64) != 0) ++g_restoreBad;
  if (g_rewriteAt >= 0) {  // DCS's own camera written inside the replay must still be there
    alignas(16) uint8_t dcsCam[0x300];
    SetCameraCtx(dcsCam, g_rewriteV, g_rewriteP);
    if (!memcmp(dcsCam + 0x1b4, g_curCtx + 0x1b4, 64)) ++g_rewriteKept;
  }
}

Rec MakeRec(uint8_t c9, uint8_t c8, int kind, int elem, int mat, int sub = 0) {
  Rec r;
  r.c9 = c9, r.c8 = c8, r.kind = kind, r.elem = elem, r.mat = mat, r.sub = sub, r.id = elem >= 0 ? elem : 99;
  return r;
}

// The view's record list (the cockpit thread's recording for view t).
std::vector<Rec> BuildList(int t, const FrameOpts& o) {
  std::vector<std::vector<Rec>> groups;  // one element (or one display's records) per group
  auto one = [&](const Rec& r) { groups.push_back({r}); };
  for (int i = 0; i < 3; ++i) one(MakeRec(1, 0, kHud, kEHud0 + i, kMatHud, i));
  // HUD glyphs, the repeater (B) and the DED (T): one (c9, c8) pair, one material; the glyph count per view.
  const int glyphs = 2 + static_cast<int>(R01() * 3);  // 2..4
  for (int i = 0; i < glyphs; ++i) one(MakeRec(1, 1, kGlyph, kEGlyph0 + i, kMatHud, i));
  const Rec rep = MakeRec(1, 1, kHudDed, kERep, kMatHud);
  Rec ded = MakeRec(1, 1, kDed, kEDed, kMatHud);
  if (o.alias) ded.elem = kERep;  // the same identity as the repeater (tracks keep the DED's id)
  if (o.alias)
    groups.push_back(o.dropDed ? std::vector<Rec>{rep} : std::vector<Rec>{ded, rep});
  else {
    one(rep);
    if (!o.dropDed) one(ded);
  }
  // The MFDs: one pair; the focus views cull the left one (and, every other frame, the EHSI in view 3).
  if (t < 2) one(MakeRec(2, 0, kMfdL, kEMfdL, kMatMfd));
  one(MakeRec(2, 0, kMfdR, kEMfdR, kMatMfd));
  if (t >= 2) one(MakeRec(2, 0, kFocus, kEFocus, kMatMfd));  // a display only the focus views draw
  if (!(t == 3 && (g_frameNo & 1))) one(MakeRec(2, 2, kEhsi, kEEhsi, kMatEhsi));
  one(MakeRec(1, 3, kJhmcs, kEJhmcs, kMatJhmcs));
  one(MakeRec(3, 0, kPanel, kEPanel, kMatPanel));
  one(MakeRec(2, 9, kState, -1, 0));
  one(MakeRec(2, 7, kNoisy, kENoisy, kMatNoisy));
  if (o.shuffle) {
    for (size_t i = groups.size(); i > 1; --i) std::swap(groups[i - 1], groups[static_cast<size_t>(R01() * i) % i]);
  } else if (o.tFirst) {
    std::stable_partition(groups.begin(), groups.end(), [](const std::vector<Rec>& g) { return KindT(g[0].kind); });
  }
  std::vector<Rec> list;
  for (auto& g : groups)
    for (Rec& r : g) list.push_back(r);
  if (o.edit) o.edit(t, list);
  return list;
}

void (*g_beforeView)(int t) = nullptr;  // tests: called before each view of a frame (mid-frame changes)
ck::ViewFrame g_cpuSnap[4];  // the last frame's per-view state before its end (the frame dump's cpu counters)
void RunFrame(const FrameOpts& o) {
  ++g_frameNo;
  g_log.clear();
  // The head: slow yaw / position motion, still for a few frames now and then (the skip cache's case).
  const bool still = (g_frameNo % 37) < 6;
  if (!still) {
    g_yaw += 0.35 * std::sin(g_frameNo * 0.05);
    g_px += 0.0004 * std::cos(g_frameNo * 0.03);
  }
  const double yaw = g_yaw;
  cktest::Rot(yaw, 2, 0, g_HR);
  g_H[0] = 0.1 + g_px, g_H[1] = 0.6, g_H[2] = -0.2;
  g_cockpit[0] = 0.1, g_cockpit[1] = 0.65, g_cockpit[2] = -0.15;
  g_noisyEyeRel = R01() < 0.5;
  if (g_frameNo % 2 == 0) ++g_mfdPage;
  g_missKind = o.missKind;
  const double base = 0.0655, scale = o.scale1 ? 1.0 : g_frameNo > 1000 ? 1.3 : 1.4, boosted = base * scale,
               h = (boosted - base) / 2;
  const double fovs[4][4] = {{-1.3, 1.0, 1.1, -1.2}, {-1.0, 1.3, 1.1, -1.2}, {-0.6, 0.5, 0.55, -0.6},
                             {-0.5, 0.6, 0.55, -0.6}};
  for (int t = 0; t < 4; ++t) {
    ViewScene& v = g_vs[t];
    const double sg = (t & 1) ? 1.0 : -1.0;
    for (int i = 0; i < 3; ++i) v.E[i] = g_H[i] + sg * boosted / 2 * g_HR[0][i];
    for (int i = 0; i < 3; ++i) v.dw[i] = -sg * h * g_HR[0][i];
    double R[3][3];
    if (t < 2)
      cktest::Cant(g_HR, sg * 4.0, R);  // canted context displays
    else
      memcpy(R, g_HR, sizeof(R));
    v.V = Inv(cktest::ViewInvOf(R, v.E));
    v.P = cktest::ProjOf(fovs[t], 0, 0);  // the overlays: unjittered
    alignas(16) uint8_t tmp[0x300];
    SetCameraCtx(tmp, v.V, v.P);
    v.VPf = GetVp(tmp);
    ck::M4 m, mt;
    memcpy(&m, &v.VPf, sizeof(m));
    mt = oc::TrueVp(m, v.dw);
    float f[16];
    oc::ToFloats(mt, f, false);
    uint8_t fb[64];
    memcpy(fb, f, 64);
    v.VPtrueF = Get(fb, 0);
  }
  // The frame's record lists (the cockpit thread), per view, packed into DCS's array.
  for (int t = 0; t < 4; ++t) g_list[t] = BuildList(t, o);
  PackRecords(g_h->handler);

  g_c->SOSetTargets(1, &g_so, std::array<UINT, 1>{0}.data());
  for (int t = 0; t < 4; ++t) {
    if (g_beforeView) g_beforeView(t);
    ViewScene& v = g_vs[t];
    fixdepth::g_s.hmdOrd = t;
    g_curCtx = g_ctxA;
    SetCameraCtx(g_ctxA, v.V, v.P);
    Mat gb = v.VPf;
    if (t >= 2) {  // the focus views' G-buffer camera: jittered (DLSS); the overlays draw unjittered
      alignas(16) uint8_t tmp[0x300];
      SetCameraCtx(tmp, v.V, cktest::ProjOf(t == 2 ? std::array<double, 4>{-0.6, 0.5, 0.55, -0.6}.data()
                                                   : std::array<double, 4>{-0.5, 0.6, 0.55, -0.6}.data(),
                                            0.0007, -0.0004));
      gb = GetVp(tmp);
    }
    if (t == o.camWrongView) {  // another camera: 2 degrees of yaw
      double R2[3][3];
      cktest::Rot(yaw + 2, 2, 0, R2);
      alignas(16) uint8_t tmp[0x300];
      SetCameraCtx(tmp, Inv(cktest::ViewInvOf(R2, v.E)), v.P);
      gb = GetVp(tmp);
    }
    SetViewState(t, o.patched && !o.scale1, gb);
    ck::g_f.h = h;
    for (int pass = 0; pass < 3; ++pass) {
      fdtest::FakePass& p = pass == 0 ? g_h->hdr : pass == 1 ? g_h->ldr : g_h->gui;
      fixdepth::CockpitIpdBoundary(&p, true, plod::kOther);
      fixdepth::CockpitIpdOverlayBegin(&p);
      // updateBuffers: the pass's cPerView (b7) from the context (one upload per pass).
      {
        alignas(16) uint8_t pv[ck::kCbBytes] = {};
        memcpy(pv + ck::kOffView, g_curCtx + 0xf4, 64);
        memcpy(pv + ck::kOffViewInv, g_curCtx + 0x1f4, 64);
        memcpy(pv + ck::kOffProj, g_curCtx + 0x134, 64);
        memcpy(pv + ck::kOffProjInv, g_curCtx + 0x234, 64);
        Put(pv, ck::kOffViewProj, GetVp(g_curCtx));  // (updateBuffers: cPerView's layout)
        g_c->UpdateSubresource(g_h->b7, 0, nullptr, pv, 0, 0);
        g_c->VSSetConstantBuffers(7, 1, &g_h->b7);
      }
      if (o.toggles && R01() < 0.15) ToggleBehindOurBack();  // between the binder and the first record
      if (pass < 2) {
        g_togglePlan = o.toggles && R01() < 0.3 ? static_cast<int>(R01() * 8) : -1;
        g_ctxSwitchAt = (pass == 0 && t == o.ctxSwitchView) ? 2 : -1;
        g_rewriteAt = (pass == 0 && t == o.rewriteView) ? 2 : -1;
        if (g_rewriteAt >= 0) {
          g_rewriteV = v.V;
          g_rewriteP = o.rewriteSame ? v.P : cktest::ProjOf(fovs[t], 0.001, 0.0);  // DCS's own camera
        }
        FnObj* f = pass == 0 ? &g_h->fnHdr : &g_h->fnLdr;
        if (o.nullFilter) f = &g_h->fnNull;
        if (o.unknownFilter) f = &g_h->fnUnk;
        const bool empty = pass == 1 && R01() < 0.05;  // an empty replay (nothing of this type drawn)
        const int32_t keep = g_ranges[t];
        if (empty) g_ranges[t] = static_cast<int32_t>((static_cast<uint32_t>(keep) & 0xffffu) * 0x10001u);
        Replay(t, f);
        g_ranges[t] = keep;
        g_togglePlan = g_ctxSwitchAt = g_rewriteAt = -1;
        if (pass == 0) {  // a world label (not a cockpit record): world-projected with the context's camera
          Mat W = Ident();
          W.a[3][0] = g_H[0] + 900, W.a[3][1] = g_H[1] + 50, W.a[3][2] = g_H[2] + 2500;
          Effect& e = g_fx[13];
          Put(e.cpu, 0, Mul(W, GetVp(g_curCtx)));
          Put(e.cpu, 64, W);
          e.dirty = true;
          const int keepMode = g_fxMode;
          g_fxMode = kFxDirty;
          Apply(e);
          g_fxMode = keepMode;
          DrawOne(t, 1000, -1, W, nullptr);
        }
      } else {  // GUIPass: 2D text, no camera
        Mat M = Ident();
        M.a[3][0] = 0.25, M.a[3][1] = -0.5;
        Effect& e = g_fx[12];
        Put(e.cpu, 0, M);
        e.dirty = true;
        const int keepMode = g_fxMode;
        g_fxMode = kFxDirty;
        Apply(e);
        g_fxMode = keepMode;
        g_c->Draw(1, 0);
        if (g_logOn) {
          DrawLog d;
          d.frame = g_frameNo, d.t = t, d.id = 2000, d.kind = -2;
          d.dcs[0] = d.tru[0] = 0.25, d.dcs[1] = d.tru[1] = -0.5, d.dcs[3] = d.tru[3] = 1;
          g_log.push_back(d);
        }
      }
      fixdepth::CockpitIpdBoundary(&p, false, plod::kOther);
    }
  }
  ID3D11Buffer* none = nullptr;
  g_c->SOSetTargets(1, &none, std::array<UINT, 1>{0}.data());
  g_missKind = -1;
  for (int t = 0; t < 4; ++t) g_cpuSnap[t] = ck::g_f.v[t];
  ck::EndFrame();
}

// The GPU's clip position of every draw of the frame (stream output, in draw order).
std::vector<std::array<float, 4>> ReadDraws() {
  std::vector<std::array<float, 4>> out;
  g_c->CopyResource(g_soStage, g_so);
  D3D11_MAPPED_SUBRESOURCE m{};
  if (SUCCEEDED(g_c->Map(g_soStage, 0, D3D11_MAP_READ, 0, &m))) {
    const float* f = static_cast<const float*>(m.pData);
    out.resize(g_log.size());
    for (size_t i = 0; i < g_log.size(); ++i) memcpy(out[i].data(), f + 4 * i, 16);
    g_c->Unmap(g_soStage, 0);
  }
  return out;
}

// Per draw: 0 DCS's camera, 1 the true eye, 2 neither. NDC within 1e-5 (a 6.5 mm eye move is ~1e-2).
int StateOf(const DrawLog& d, const std::array<float, 4>& g) {
  auto close = [&](const double e[4]) {
    if (!(std::fabs(g[3]) > 1e-9) || !(std::fabs(e[3]) > 1e-9)) return false;
    return std::fabs(g[0] / g[3] - e[0] / e[3]) <= 1e-5 && std::fabs(g[1] / g[3] - e[1] / e[3]) <= 1e-5;
  };
  if (close(d.dcs)) return 0;
  if (close(d.tru)) return 1;
  return 2;
}

// Per (record, view): the state per frame, for the A-B-A analysis.
struct Track {
  std::vector<int> st;  // state per frame (-1 not drawn)
};
std::map<int, Track> g_tracks;  // key id * 4 + t

// A-B-A: a run of one state shorter than the learner's commit streak with the same state on both sides.
int CountAba(const std::vector<int>& s, int minRun) {
  std::vector<std::pair<int, int>> runs;  // state, length (frames not drawn skipped)
  for (int v : s) {
    if (v < 0) continue;
    if (!runs.empty() && runs.back().first == v)
      ++runs.back().second;
    else
      runs.push_back({v, 1});
  }
  int n = 0;
  for (size_t i = 1; i + 1 < runs.size(); ++i)
    if (runs[i - 1].first == runs[i + 1].first && runs[i].second < minRun) ++n;
  return n;
}

bool g_verbose = false;  // debugging: every bad draw printed
// The frame's checks.
struct Tally {
  uint64_t draws = 0, hudBad = 0, bezelBad = 0, labelBad = 0, guiBad = 0, tBefore = 0, neither = 0, mismatch = 0;
  uint64_t hudBadFrames = 0;
  // Frames in which views of one eye differ (0 vs 2, 1 vs 3): one has a cockpit display at the true eye, the other
  // none (R32 r3: one decision per frame for all four views).
  uint64_t eyeDisagree = 0;
  double hudMaxPx = 0, bezelMaxPx = 0;
};
// *states (optional): per draw of the frame, its state (the special-case scenarios read it).
void Analyse(Tally& ta, bool tCommitted, bool expectMoved[4], int frameIdx, std::vector<int>* states = nullptr) {
  const auto g = ReadDraws();
  if (g.size() != g_log.size()) {
    ++ta.mismatch;
    return;
  }
  const uint64_t hud0 = ta.hudBad;
  if (states) states->assign(g.size(), -1);
  bool anyTrue[4] = {};
  for (size_t i = 0; i < g.size(); ++i) {
    const DrawLog& d = g_log[i];
    ++ta.draws;
    const int st = StateOf(d, g[i]);
    if (states) (*states)[i] = st;
    if (d.id < 1000 && frameIdx >= 0) {
      Track& tr = g_tracks[d.id * 4 + d.t];
      if (static_cast<int>(tr.st.size()) <= frameIdx) tr.st.resize(frameIdx + 1, -1);
      tr.st[frameIdx] = st;
    }
    if (st == 2) ++ta.neither;
    if (d.kind >= 0 && KindT(d.kind) && st == 1) anyTrue[d.t] = true;
    const double px = 500.0;  // NDC to pixels (a 1000-pixel-wide view)
    if (d.kind == -2) {
      if (!(std::fabs(g[i][0] - 0.25f) < 1e-6 && std::fabs(g[i][1] + 0.5f) < 1e-6)) ++ta.guiBad;
      continue;
    }
    if (d.kind == -1) {  // world label: DCS's camera
      if (st != 0) ++ta.labelBad;
      continue;
    }
    if (KindB(d.kind)) {  // HUD: zero disparity against its direction at infinity, at DCS's camera
      const double e = std::fabs(g[i][0] / g[i][3] - d.ref[0] / d.ref[3]) * px;
      ta.hudMaxPx = std::max(ta.hudMaxPx, e);
      if (st != 0 || e > 0.05) ++ta.hudBad;
    } else if (KindT(d.kind)) {
      if (tCommitted && expectMoved[d.t]) {
        // On the bezel: the same cockpit point through the true eye's camera (what the cockpit pass drew).
        const double e = std::fabs(g[i][0] / g[i][3] - d.tru[0] / d.tru[3]) * px;
        ta.bezelMaxPx = std::max(ta.bezelMaxPx, e);
        if (st != 1 || e > 0.05) {
          ++ta.bezelBad;
          if (g_verbose) printf("       bad: frame %d view %d %s state %d (want 1)\n", d.frame, d.t, kKindNames[d.kind], st);
        }
      } else if (!expectMoved[d.t] && st != 0) {
        ++ta.bezelBad;  // a view that must stay native
        if (g_verbose) printf("       bad: frame %d view %d %s state %d (want 0)\n", d.frame, d.t, kKindNames[d.kind], st);
      } else if (!tCommitted && st == 1) {
        ++ta.tBefore;
      }
    }
  }
  if (ta.hudBad > hud0) ++ta.hudBadFrames;
  if (anyTrue[0] != anyTrue[2] || anyTrue[1] != anyTrue[3]) {
    ++ta.eyeDisagree;
    if (g_verbose) printf("       eye disagreement: frame %d moved %d %d %d %d\n", g_frameNo, anyTrue[0], anyTrue[1],
                          anyTrue[2], anyTrue[3]);
  }
}

bool AllTCommitted() {
  const uint64_t keys[] = {KeyOf(kEDed, kMatHud, 1, 1),    KeyOf(kEMfdL, kMatMfd, 2, 0),
                           KeyOf(kEMfdR, kMatMfd, 2, 0),   KeyOf(kEEhsi, kMatEhsi, 2, 2),
                           KeyOf(kEJhmcs, kMatJhmcs, 1, 3), KeyOf(kEPanel, kMatPanel, 3, 0),
                           KeyOf(kEFocus, kMatMfd, 2, 0)};
  for (uint64_t k : keys)
    if (oc::g_learn.ClassOf(k) != oc::kClsT) return false;
  return true;
}
// A failure scenario's pair history cleared (each scenario tests the pair rule on its own; the recurrence rule
// has its own test).
void ClearFailHistory() {
  oc::g_lastStop = ~0ull;
}

// ---------------------------------------------------------------------------
// Pure tests
// ---------------------------------------------------------------------------

// A slot reader that takes a lock, as the loader's ApiOriginal does (g_regMutex).
std::mutex g_slotMx;
void* LockingSlotRead(void** slot) {
  std::lock_guard<std::mutex> lock(g_slotMx);
  return *slot;
}

void TestPure() {
  // VP_true: rows 0..2 bit-identical, row 3 by -(dw . rows 0..2), the camera centre moved by +dw.
  {
    double R[3][3];
    cktest::Rot(25, 3, 0, R);
    const double E[3] = {1.2, 0.7, -0.4}, fov[4] = {-1.1, 1.0, 1.0, -1.1}, dw[3] = {0.0101, -0.0022, 0.0043};
    alignas(16) uint8_t c[0x300];
    SetCameraCtx(c, Inv(cktest::ViewInvOf(R, E)), cktest::ProjOf(fov, 0, 0));
    const ck::M4 vp = CtxVp(c), vt = oc::TrueVp(vp, dw);
    bool rows = true;
    for (int r = 0; r < 3; ++r)
      for (int k = 0; k < 4; ++k) rows = rows && vt.a[r][k] == vp.a[r][k];
    double c0[3], c1[3];
    const bool cen = ck::Centre(vp, c0) && ck::Centre(vt, c1);
    double err = 0;
    for (int i = 0; i < 3; ++i) err = std::max(err, std::fabs(c1[i] - c0[i] - dw[i]));
    Check(rows && cen && err < 1e-6, "ovl cpu: VP_true = T(-dw) * VP: rows 0..2 bit-identical, camera centre +dw");
    // Camera identity: same bytes 1; jittered / other depth mapping 2; 6.5 mm sideways, 1 degree of yaw: 0.
    alignas(16) uint8_t j[0x300], s[0x300], y[0x300];
    SetCameraCtx(j, Inv(cktest::ViewInvOf(R, E)), cktest::ProjOf(fov, 0.0008, -0.0005));
    double E2[3];
    for (int i = 0; i < 3; ++i) E2[i] = E[i] + 0.0065 * R[0][i];
    SetCameraCtx(s, Inv(cktest::ViewInvOf(R, E2)), cktest::ProjOf(fov, 0, 0));
    double R2[3][3];
    cktest::Rot(26, 3, 0, R2);
    SetCameraCtx(y, Inv(cktest::ViewInvOf(R2, E)), cktest::ProjOf(fov, 0, 0));
    Check(oc::CamSame(vp, vp) == 1 && oc::CamSame(CtxVp(j), vp) == 2 &&
              oc::CamSame(CtxVp(s), vp) == 0 && oc::CamSame(CtxVp(y), vp) == 0 &&
              oc::CamSame(vt, vp) == 0,
          "ovl cpu: camera identity: same bytes / jittered projection accepted; 6.5 mm sideways, 1 degree of yaw, "
          "VP_true refused");
  }
  // Record keys: the element identity decides, not the record's position among others of its (c9, c8).
  {
    uint64_t a = 0, b = 0;
    const uint64_t pa = reinterpret_cast<uint64_t>(&a), pb = reinterpret_cast<uint64_t>(&b);
    const uint64_t k1 = oc::RecKey(oc::ElemBase(pa, pb, 1, 1), 0), k2 = oc::RecKey(oc::ElemBase(pb, pb, 1, 1), 0),
                   k3 = oc::RecKey(oc::ElemBase(pa, pa, 1, 1), 0), k4 = oc::RecKey(oc::ElemBase(pa, pb, 2, 1), 0),
                   k5 = oc::RecKey(oc::ElemBase(pa, pb, 1, 2), 0), k6 = oc::RecKey(oc::ElemBase(pa, pb, 1, 1), 1);
    oc::OrdTab* o = new oc::OrdTab();
    o->Begin();
    const uint64_t ba = oc::ElemBase(pa, pb, 1, 1), bb = oc::ElemBase(pb, pb, 1, 1);
    const uint32_t o1 = o->Next(ba), o2 = o->Next(bb), o3 = o->Next(ba), o4 = o->Next(ba);
    o->Begin();
    const uint32_t o5 = o->Next(ba);
    delete o;
    const uint64_t ks[] = {k1, k2, k3, k4, k5, k6};
    bool distinct = true;
    for (int i = 0; i < 6; ++i)
      for (int k = i + 1; k < 6; ++k) distinct = distinct && ks[i] != ks[k];
    Check(distinct && k1 && o1 == 0 && o2 == 0 && o3 == 1 && o4 == 2 && o5 == 0,
          "ovl cpu keys: each pointer, c9, c8 and the ordinal of the same identity make a different key; ordinals "
          "count per identity and restart per replay");
  }
  // The learner: commits after 16 agreeing frames; a noisy key freezes at B (mixed) before any commit; a committed
  // key contradicted once is frozen at once (a contradiction); views 2 / 3 measure as a second pair.
  {
    oc::Learner* L = new oc::Learner();
    const uint64_t kb = 0x101, kt = 0x203, kn = 0x305, kf = 0x407;
    uint32_t rs = 5;
    int commitB = -1, commitT = -1, commitF = -1;
    bool tBefore = false;
    const double E0[3] = {-0.045, 0, 0}, E1[3] = {0.045, 0, 0};
    const double hudOff[3] = {0.0, 0.0, 0.75}, mfd[3] = {0.17, -0.3, 0.6};
    double ow0[3], ow1[3], oe0[3], oe1[3], me0[3], me1[3];
    for (int i = 0; i < 3; ++i) ow0[i] = E0[i] + hudOff[i], ow1[i] = E1[i] + hudOff[i], oe0[i] = oe1[i] = hudOff[i];
    for (int i = 0; i < 3; ++i) me0[i] = mfd[i] - E0[i], me1[i] = mfd[i] - E1[i];
    int noisyFrozenAt = -1;
    for (uint64_t f = 0; f < 60; ++f) {
      L->Seen(kb, f), L->Seen(kt, f), L->Seen(kn, f), L->Seen(kf, f);
      L->Sample(0, 0, kb, f, ow0, oe0), L->Sample(0, 1, kb, f, ow1, oe1);
      L->Sample(0, 0, kt, f, mfd, me0), L->Sample(0, 1, kt, f, mfd, me1);
      L->Sample(1, 0, kf, f, mfd, me0), L->Sample(1, 1, kf, f, mfd, me1);  // measured by views 2 / 3 only
      rs = rs * 1664525u + 1013904223u;
      if ((f & 1) == 0) {
        L->Sample(0, 0, kn, f, ow0, oe0), L->Sample(0, 1, kn, f, ow1, oe1);
      } else {
        L->Sample(0, 0, kn, f, mfd, me0), L->Sample(0, 1, kn, f, mfd, me1);
      }
      if (f < 15 && L->ClassOf(kt) == oc::kClsT) tBefore = true;
      int contra = -1;
      L->EndFrame(f, nullptr, &contra);
      if (contra != 0) tBefore = true;
      if (commitB < 0 && L->ClassOf(kb) == oc::kClsB) commitB = static_cast<int>(f);
      if (commitT < 0 && L->ClassOf(kt) == oc::kClsT) commitT = static_cast<int>(f);
      if (commitF < 0 && L->ClassOf(kf) == oc::kClsT) commitF = static_cast<int>(f);
      const oc::KeyRec* n = L->FindC(kn);
      if (noisyFrozenAt < 0 && n && n->frozen) noisyFrozenAt = static_cast<int>(f);
    }
    Check(commitB == 15 && commitT == 15 && commitF == 15 && !tBefore && noisyFrozenAt == 3 &&
              L->ClassOf(kn) == oc::kClsB && L->mixed == 1 && L->contradictions == 0,
          "ovl cpu learner: HUD-like key B and MFD-like key T after 16 frames (also one measured by views 2 / 3 "
          "only); an alternating key frozen at B after both outcomes twice, before any commit");
    // A committed T key measured once as eye-relative: frozen at B in that frame, one contradiction.
    const uint64_t c0 = L->contradictions;
    L->Sample(0, 0, kt, 60, ow0, oe0), L->Sample(0, 1, kt, 60, ow1, oe1);
    L->Sample(0, 0, kb, 60, ow0, oe0), L->Sample(0, 1, kb, 60, ow1, oe1);
    int contra = 0;
    L->EndFrame(60, nullptr, &contra);
    const oc::KeyRec* t = L->FindC(kt);
    const bool frozenT = t && t->frozen && L->ClassOf(kt) == oc::kClsB && contra == 1 && L->contradictions == c0 + 1 &&
                         L->ClassOf(kb) == oc::kClsB;
    // Both pairs of one frame disagreeing on a committed B key: a contradiction too.
    L->Sample(0, 0, kb, 61, ow0, oe0), L->Sample(0, 1, kb, 61, ow1, oe1);
    L->Sample(1, 0, kb, 61, mfd, me0), L->Sample(1, 1, kb, 61, mfd, me1);
    L->EndFrame(61, nullptr, &contra);
    const bool frozenB = contra == 1 && L->FindC(kb)->frozen;
    // Coincident eyes give no class; keys not seen for 600 frames are forgotten, frozen ones kept.
    const double same[2][3] = {{0.1, 0.2, 0.3}, {0.1, 0.2, 0.3}};
    for (uint64_t f = 62; f < 62 + 2 * oc::kLearnExpire; ++f) {
      L->Seen(kf, f);
      L->EndFrame(f);
    }
    // R32 r3: frozen keys expire too (a key names heap objects that DCS frees and reuses).
    const bool forgot = L->FindC(kf) && !L->FindC(kn + 1000) && !L->FindC(kn) && !L->FindC(kt) && !L->FindC(kb) &&
                        L->expired >= 3 && L->used == 1;
    oc::Learner* L2 = new oc::Learner();
    L2->Seen(0x55, 0);
    for (uint64_t f = 1; f < 1 + 2 * oc::kLearnExpire; ++f) L2->EndFrame(f);
    const bool dropped = !L2->FindC(0x55) && L2->expired == 1 && L2->used == 0;
    delete L2;
    Check(frozenT && frozenB && forgot && dropped && oc::Learner::Outcome(same, same) == oc::kClsUnknown,
          "ovl cpu learner: a committed key contradicted once (in either pair) is frozen at B in that frame and "
          "counted; coincident eyes give no class; unseen keys are forgotten after 600 frames, frozen keys too");
    delete L;
  }
  // R32 r3: a failure that names a key freezes it at once (once: the same key again is not the key's failure); a
  // reset clears the table (every key unknown: DCS's eye) and keeps the counters.
  {
    oc::Learner* L = new oc::Learner();
    const double E0[3] = {-0.045, 0, 0}, E1[3] = {0.045, 0, 0}, mfd[3] = {0.17, -0.3, 0.6};
    double me0[3], me1[3];
    for (int i = 0; i < 3; ++i) me0[i] = mfd[i] - E0[i], me1[i] = mfd[i] - E1[i];
    const uint64_t kt = 0x777;
    for (uint64_t f = 0; f < 20; ++f) {
      L->Seen(kt, f);
      L->Sample(0, 0, kt, f, mfd, me0), L->Sample(0, 1, kt, f, mfd, me1);
      L->EndFrame(f);
    }
    const bool committed = L->ClassOf(kt) == oc::kClsT && L->keysT == 1 && L->keysB == 0;
    const bool first = L->FailKey(kt), second = L->FailKey(kt);
    const bool frozen = L->ClassOf(kt) == oc::kClsB && L->FindC(kt)->failFrozen && L->keysT == 0 && L->keysB == 1 &&
                        L->keyFails == 1;
    const uint64_t commits = L->commits;
    L->Reset();
    const bool cleared = L->ClassOf(kt) == oc::kClsUnknown && !L->FindC(kt) && L->used == 0 && L->keysT == 0 &&
                         L->keysB == 0 && L->commits == commits && L->resets == 1;
    delete L;
    Check(committed && first && !second && frozen && cleared,
          "ovl cpu learner (R32 r3): a failure naming a committed T key freezes it at B at once, a second one on the "
          "same key is refused (not the key's); a reset leaves every key unknown (DCS's eye), the counters kept");
  }
  // R32 r3 (cost): the frame end walks only the keys sampled in that frame and forgets through a sweep with
  // backward-shift deletion. 1400 keys in the 2048 slots (long probe chains), half of them no longer seen after
  // frame 1000: every live key still found, every stale one gone, the class counts equal a full recount.
  {
    oc::Learner* L = new oc::Learner();
    uint32_t rs = 11;
    auto rnd = [&]() {
      rs = rs * 1664525u + 1013904223u;
      return rs >> 8;
    };
    const int kN = 1400;
    std::vector<uint64_t> keys(kN), last(kN, 0);
    for (int i = 0; i < kN; ++i) keys[i] = (static_cast<uint64_t>(rnd()) << 32 | rnd()) * 2 + 1;
    const double E0[3] = {-0.045, 0, 0}, E1[3] = {0.045, 0, 0}, mfd[3] = {0.17, -0.3, 0.6};
    const double hud[3] = {0, 0, 0.75};
    double me0[3], me1[3], hw0[3], hw1[3];
    for (int i = 0; i < 3; ++i)
      me0[i] = mfd[i] - E0[i], me1[i] = mfd[i] - E1[i], hw0[i] = E0[i] + hud[i], hw1[i] = E1[i] + hud[i];
    const uint64_t kF = 3000;
    for (uint64_t f = 0; f < kF; ++f) {
      for (int j = 0; j < 60; ++j) {
        const int i = static_cast<int>(rnd() % (f < 1000 ? kN : kN / 2));
        L->Seen(keys[i], f);
        last[i] = f + 1;
        if (j < 20) {
          if (i & 1)
            L->Sample(0, 0, keys[i], f, mfd, me0), L->Sample(0, 1, keys[i], f, mfd, me1);
          else
            L->Sample(0, 0, keys[i], f, hw0, hud), L->Sample(0, 1, keys[i], f, hw1, hud);
        }
      }
      L->EndFrame(f);
    }
    bool found = true, gone = true;
    for (int i = 0; i < kN; ++i) {
      if (last[i] && last[i] + oc::kLearnExpire >= kF) found = found && L->FindC(keys[i]) != nullptr;
      if (last[i] + oc::kLearnExpire + oc::kLearnWindow + 2 < kF) gone = gone && L->FindC(keys[i]) == nullptr;
    }
    int live = 0, nT = 0, nB = 0;
    for (const oc::KeyRec& e : L->k) {
      if (!e.used) continue;
      ++live;
      const int c = oc::Learner::CountCls(e);
      nT += c == 2, nB += c == 1;
    }
    const int used = L->used, keysT = L->keysT, keysB = L->keysB;
    // A frame end with nothing sampled on that full table: a few slots of the sweep only.
    LARGE_INTEGER q0, q1, fq;
    QueryPerformanceFrequency(&fq);
    QueryPerformanceCounter(&q0);
    for (uint64_t f = kF; f < kF + 1000; ++f) L->EndFrame(f);
    QueryPerformanceCounter(&q1);
    const double us = (q1.QuadPart - q0.QuadPart) * 1e6 / fq.QuadPart / 1000.0;
    printf("     (learner: %d keys live, %llu expired, T %d B %d, idle frame end %.3f us)\n", live,
           static_cast<unsigned long long>(L->expired), nT, nB, us);
    Check(found && gone && live == used && nT == keysT && nB == keysB && nT > 100 && nB > 100 &&
              L->expired >= kN / 2 && us < 50,
          "ovl cpu learner (R32 r3 cost): the frame end walks only the sampled keys, the forgetting sweeps a few "
          "slots per frame (backward-shift deletion): every key seen within 600 frames found, every stale one gone, "
          "the T / B counts equal a recount, an idle frame end costs microseconds");
    delete L;
  }
  // R32 r3: a vtable slot is read only once it is readable, before the registry lookup that may take a lock (the
  // loader's g_regMutex): a fault can never happen while that lock is held.
  {
    void* out = nullptr;
    const bool bad = oc::ReadSlot(reinterpret_cast<void**>(static_cast<uintptr_t>(0x10)), &LockingSlotRead, &out);
    const bool unlocked = g_slotMx.try_lock();
    if (unlocked) g_slotMx.unlock();
    void* val = &out;
    const bool good = oc::ReadSlot(&val, &LockingSlotRead, &out) && out == &out;
    Check(!bad && unlocked && good,
          "ovl cpu signatures (R32 r3): an unreadable vtable slot is refused before the slot reader runs (no fault "
          "inside the loader registry's lock, which stays free); a readable one goes through it");
  }
}

// ---------------------------------------------------------------------------
// Signatures against the DCS install's own DLLs (read only, mapped as images)
// ---------------------------------------------------------------------------

void TestSignatures() {
  const std::wstring bin = dcsinstall::Bin();
  if (bin.empty()) {
    QvSkip("ovl cpu signatures: DCS install not found\n");
    return;
  }
  HMODULE h[oc::kModCount] = {};
  uint8_t* img[oc::kModCount] = {};
  uintptr_t pref[oc::kModCount] = {};
  uint32_t sizeOf[oc::kModCount] = {};
  bool mapped = true;
  for (int m = 0; m < oc::kModCount; ++m) {
    h[m] = LoadLibraryExW((bin + oc::kModNames[m]).c_str(), nullptr, LOAD_LIBRARY_AS_IMAGE_RESOURCE);
    if (!h[m]) {
      mapped = false;
      continue;
    }
    img[m] = reinterpret_cast<uint8_t*>(reinterpret_cast<uintptr_t>(h[m]) & ~static_cast<uintptr_t>(3));
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(img[m]);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(img[m] + dos->e_lfanew);
    pref[m] = static_cast<uintptr_t>(nt->OptionalHeader.ImageBase);  // the vtables are not relocated in this mapping
    sizeOf[m] = nt->OptionalHeader.SizeOfImage;
  }
  if (!mapped) {
    QvSkip("ovl cpu signatures: a DLL could not be mapped\n");
  } else {
    // R147: one table per DCS build (2.9.30, 2.9.31); the installed DLLs must be exactly one of them.
    int bi = -1;
    const char* why = oc::VerifySites(img, pref, &oc::PlainSlot, &bi);
    if (why) printf("     (signature: %s)\n", why);
    if (!why && bi >= 0) printf("     (signature: DCS %s sites)\n", oc::kBuilds[bi].name);
    Check(!why && bi >= 0 && bi < oc::kBuildCount,
          "ovl cpu signatures: every code range (with the record array walk and layout) and vtable target "
          "matches one known DCS build's DLLs (2.9.30 or 2.9.31, as installed)");
    const oc::Build& b = oc::kBuilds[bi >= 0 ? bi : oc::kBuildCount - 1];
    Check(bi >= 0 &&
              sigscan::ImportSlotRva(img[oc::kModCockpit], "GraphicsVista.dll", oc::kRenderImport) == b.renderIat &&
              sigscan::ExportRva(img[oc::kModGv], oc::kRenderImport) != 0,
          "ovl cpu signatures (R32 r4): CockpitBase imports RenderObject::render at the render point's slot, "
          "GraphicsVista exports it");
    // The other builds' tables refuse these DLLs (a build is never mistaken for another).
    bool othersRefused = true;
    for (int o = 0; o < oc::kBuildCount; ++o)
      if (o != bi && oc::VerifyBuild(oc::kBuilds[o], img, pref, &oc::PlainSlot) == nullptr) othersRefused = false;
    Check(bi >= 0 && othersRefused, "ovl cpu signatures (R147): the other known builds' sites refuse the installed DLLs");
    // A copy of CockpitBase's image with one flipped byte (the filter call; the record stride), then a moved
    // vtable target. The bytes: the call in the per-record range (code[1] + 0x2c), the 0xd0 of add rbx, 0xd0 (code[8]
    // + 3), in the build that matched.
    std::vector<uint8_t> copy(sizeOf[oc::kModCockpit], 0);
    for (uint32_t o = 0; o < sizeOf[oc::kModCockpit]; o += 4096)
      allocslab::ReadBytes(img[oc::kModCockpit] + o, copy.data() + o, std::min<uint32_t>(4096, sizeOf[oc::kModCockpit] - o));
    uint8_t* keep = img[oc::kModCockpit];
    img[oc::kModCockpit] = copy.data();
    const bool copyOk = bi >= 0 && oc::VerifySites(img, pref) == nullptr;
    const uint32_t filterCall = b.code[1].begin + 0x2c, stride = b.code[8].begin + 3;
    const bool strideIsD0 = copy[stride] == 0xd0;
    copy[filterCall] ^= 0x01;
    const bool flipRefused = oc::VerifySites(img, pref) != nullptr;
    copy[filterCall] ^= 0x01;
    copy[stride] ^= 0x01;  // add rbx, 0xd0
    const bool strideRefused = oc::VerifySites(img, pref) != nullptr;
    copy[stride] ^= 0x01;
    uint64_t* slot = reinterpret_cast<uint64_t*>(copy.data() + b.vt[oc::kVtReplay].vt + 8 * b.vt[oc::kVtReplay].slot);
    *slot += 16;
    const bool vtRefused = oc::VerifySites(img, pref) != nullptr;
    *slot -= 16;
    img[oc::kModCockpit] = keep;
    Check(copyOk && strideIsD0 && flipRefused && strideRefused && vtRefused,
          "ovl cpu signatures: one flipped code byte (filter call, record stride), one moved vtable target: install "
          "refused");
  }
  for (HMODULE m : h)
    if (m) FreeLibrary(m);
}

// ---------------------------------------------------------------------------
// The replica run
// ---------------------------------------------------------------------------

void ResetEffects() {
  for (Effect* arr : {g_fx, g_fxDyn})
    for (int i = 0; i < 16; ++i) {
      arr[i].dirty = arr[i].everUp = false;
      memset(arr[i].cpu, 0, 256);
      memset(arr[i].last, 0, 256);
    }
}
void ResetScene() {
  g_frameNo = 0;
  g_yaw = 10, g_px = 0;
  g_rs = 7;
  g_mfdPage = 0;
  ResetEffects();
}

// The install-order probe: when a slot points at our hook, its original must already be in place.
int g_orderOk = 0, g_orderBad = 0;
void AfterSlotWrite(void** slot) {
  bool ok = false;
  if (slot == &g_compVt[2])
    ok = *slot == reinterpret_cast<void*>(&oc::CompFilterHook) && oc::g_compOrig == &CompCall;
  else if (slot == &g_visVt[2])
    ok = *slot == reinterpret_cast<void*>(&oc::VisFilterHook) && oc::g_visOrig == &VisCall &&
         [] {  // a render-thread call through the slot at once (outside any replay: passes through)
           VisLambda l{g_visVt, 2};
           uint8_t c8 = 0, c9 = 1;
           return reinterpret_cast<FilterFnT>(g_visVt[2])(&l, &c8, &c9);
         }();
  else if (slot == &g_handlerVt[3])
    ok = *slot == reinterpret_cast<void*>(&oc::ReplayHook) && oc::g_replayOrig == &ReplayImpl;
  (ok ? g_orderOk : g_orderBad)++;
}

// The uninstall during a replay: the render thread (a worker here) is inside view 0's HDR replay; the config
// thread uninstalls; the replay must finish with our filters (they stay until it has drained).
struct MidUninstall {
  HANDLE reached = nullptr;
  DWORD tid = 0;
  FrameOpts o;
};
MidUninstall g_mid;
void MidCb() {
  SetEvent(g_mid.reached);
  // Wait until the replay slot is DCS's again (the uninstall has begun), then give it time to go on.
  for (int i = 0; i < 400 && g_handlerVt[3] == reinterpret_cast<void*>(&oc::ReplayHook); ++i) Sleep(5);
  Sleep(40);
}
// The replay held until the test releases it (past the uninstall's 1 s drain).
HANDLE g_holdRelease = nullptr;
void HoldCb() {
  SetEvent(g_mid.reached);
  WaitForSingleObject(g_holdRelease, 30000);
}
DWORD WINAPI MidWorker(void*) {
  g_mid.tid = GetCurrentThreadId();
  ptiming::g_topTid = g_mid.tid;
  RunFrame(g_mid.o);
  return 0;
}

void TestReplica() {
  Harness H;
  g_h = &H;
  const D3D_FEATURE_LEVEL want[] = {D3D_FEATURE_LEVEL_11_1};
  if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, want, 1, D3D11_SDK_VERSION, &H.dev, nullptr,
                               &H.imm))) {
    QvSkip("ovl cpu replica: no WARP 11.1 device\n");
    return;
  }
  H.imm->QueryInterface(__uuidof(ID3D11DeviceContext1), reinterpret_cast<void**>(&g_c));
  // The ILS-like vertex shader (float4(pos, 1) * matWorldViewProj, dp4 with cb0[0..3]) and its stream output.
  {
    HMODULE dll = LoadLibraryW(L"d3dcompiler_47.dll");
    auto compile = dll ? reinterpret_cast<decltype(&D3DCompile)>(GetProcAddress(dll, "D3DCompile")) : nullptr;
    static const char kSrc[] =
        "cbuffer cGlobals : register(b0) { float4x4 matWorldViewProj; float4x4 matWorld; float4 other[8]; };\n"
        "float4 vs(uint id : SV_VertexID) : SV_Position { return mul(float4(0, 0, 0, 1), matWorldViewProj); }\n";
    ID3DBlob* b = nullptr;
    if (compile) compile(kSrc, sizeof(kSrc) - 1, "ils", nullptr, nullptr, "vs", "vs_5_0", 0, 0, &b, nullptr);
    if (b) {
      H.dev->CreateVertexShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &H.vs);
      D3D11_SO_DECLARATION_ENTRY e = {0, "SV_Position", 0, 0, 4, 0};
      UINT stride = 16;
      H.dev->CreateGeometryShaderWithStreamOutput(b->GetBufferPointer(), b->GetBufferSize(), &e, 1, &stride, 1,
                                                  D3D11_SO_NO_RASTERIZED_STREAM, nullptr, &H.gs);
      b->Release();
    }
  }
  {
    D3D11_BUFFER_DESC d{};
    d.ByteWidth = 16 * 16384;
    d.BindFlags = D3D11_BIND_STREAM_OUTPUT;
    H.dev->CreateBuffer(&d, nullptr, &g_so);
    d.BindFlags = 0;
    d.Usage = D3D11_USAGE_STAGING;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    H.dev->CreateBuffer(&d, nullptr, &g_soStage);
    D3D11_BUFFER_DESC c{};
    c.ByteWidth = 256;
    c.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    for (int i = 0; i < 16; ++i) {
      H.dev->CreateBuffer(&c, nullptr, &g_fx[i].cb);
      D3D11_BUFFER_DESC dd = c;
      dd.Usage = D3D11_USAGE_DYNAMIC;
      dd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
      H.dev->CreateBuffer(&dd, nullptr, &g_fxDyn[i].cb);
      g_fxDyn[i].dyn = true;
    }
    c.ByteWidth = ck::kCbBytes;
    H.dev->CreateBuffer(&c, nullptr, &H.b7);
    D3D11_TEXTURE2D_DESC td{};
    td.Width = td.Height = 16;
    td.MipLevels = td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.BindFlags = D3D11_BIND_RENDER_TARGET;
    H.dev->CreateTexture2D(&td, nullptr, &H.rtTex);
    if (H.rtTex) H.dev->CreateRenderTargetView(H.rtTex, nullptr, &g_rtv);
  }
  bool made = g_c && H.vs && H.gs && g_so && g_soStage && H.b7 && g_rtv;
  for (int i = 0; i < 16; ++i) made = made && g_fx[i].cb && g_fxDyn[i].cb;
  Check(made, "ovl cpu replica: WARP device, ILS-like VS with stream output, effect buffers (default and dynamic)");
  if (!made) return;
  g_c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_POINTLIST);
  g_c->IASetInputLayout(nullptr);
  g_c->VSSetShader(H.vs, nullptr, 0);
  g_c->GSSetShader(H.gs, nullptr, 0);
  g_c->PSSetShader(nullptr, nullptr, 0);

  // The module: cockpit ipd on (its G-buffer state set by the test per view), the cpu path, our replica's slots.
  ID3D11DeviceContext* oldCtx = fixdepth::g_s.ctx;
  ID3D11DeviceContext1* oldCtx1 = fixdepth::g_s.ctx1;
  fixdepth::g_s.ctx = H.imm;
  fixdepth::g_s.ctx1 = g_c;
  const DWORD mainTid = GetCurrentThreadId();
  ptiming::g_topTid = mainTid;
  fixdepth::g_fault = false;
  ck::Configure(1, false, true, 0, 0, false);  // CockpitTrueIpdOverlayPath: cpu (the default)
  ck::ResetRender();
  ck::g_ovlProbeLeft = 0;
  ck::g_fault = false;
  ck::g_on = true;
  g_handlerVt[3] = reinterpret_cast<void*>(&ReplayImpl);
  g_visVt[2] = reinterpret_cast<void*>(&VisCall);
  g_compVt[2] = reinterpret_cast<void*>(&CompCall);
  g_unkVt[2] = reinterpret_cast<void*>(&UnkCall);

  // Install order: every original in place before its slot is written (a render-thread call may come at once).
  g_orderOk = g_orderBad = 0;
  oc::g_testAfterSlotWrite = &AfterSlotWrite;
  const bool inst = oc::InstallAt(&g_handlerVt[3], &g_visVt[2], &g_compVt[2], &g_curCtx, g_visVt, false);
  oc::g_testAfterSlotWrite = nullptr;
  Check(inst && ck::PathCpu() && oc::Installed() && g_handlerVt[3] == reinterpret_cast<void*>(&oc::ReplayHook) &&
            g_visVt[2] == reinterpret_cast<void*>(&oc::VisFilterHook) &&
            g_compVt[2] == reinterpret_cast<void*>(&oc::CompFilterHook) && g_orderOk == 3 && g_orderBad == 0,
        "ovl cpu replica: the replay and both filter slots hooked (cpu path is the default); each slot's original "
        "in place when the slot points at our hook (a call through it at once passes to DCS's)");
  H.visHdr.vt = g_visVt, H.visHdr.mask = 6;  // OverlayHDRPass: types 1 and 2 (HUD, MFD)
  H.visLdr.vt = g_visVt, H.visLdr.mask = 8;  // OverlayLDRPass: type 3
  H.unk.vt = g_unkVt, H.unk.mask = 6;
  H.fnHdr.impl = &H.visHdr;
  H.fnLdr.impl = &H.visLdr;
  H.fnUnk.impl = &H.unk;
  H.fnNull.impl = nullptr;

  // ---- Main run: 2000 frames x 4 views, every effect pattern, toggles, per-view lists, the composite filter. ----
  ResetScene();
  g_tracks.clear();
  g_restoreBad = g_replays = 0;
  Tally ta;
  int commitFrame = -1;
  uint64_t perMode[kFxModes] = {};
  const uint64_t tog0 = g_toggles;
  constexpr int kMain = 2000;
  for (int f = 0; f < kMain; ++f) {
    g_fxMode = f < 500 ? kFxDirty : f < 900 ? kFxShared : f < 1300 ? kFxSkip : f < 1700 ? kFxMap : kFxMulti;
    H.handler.composite = ((f >= 250 && f < 500) || (f >= 1500 && f < 1700)) ? reinterpret_cast<void*>(&H) : nullptr;
    const bool committed = AllTCommitted();
    if (committed && commitFrame < 0) commitFrame = f;
    FrameOpts o;
    RunFrame(o);
    bool moved[4] = {true, true, true, true};
    const uint64_t bad0 = ta.hudBad + ta.bezelBad + ta.labelBad + ta.guiBad + ta.neither;
    Analyse(ta, committed, moved, f);
    if (ta.hudBad + ta.bezelBad + ta.labelBad + ta.guiBad + ta.neither > bad0 && bad0 == 0)
      printf("     (first bad frame %d, mode %s)\n", f, kFxNames[g_fxMode]);
    ++perMode[g_fxMode];
  }
  int aba = 0, abaTracks = 0;
  for (auto& kv : g_tracks) {
    const int n = CountAba(kv.second.st, oc::kLearnCommit);
    aba += n;
    abaTracks += n > 0;
  }
  printf("     (main run: %llu draws, commit at frame %d, toggles behind our back %llu, HUD max %.4f px, bezel max %.4f "
         "px, replays %llu, observer uploads %llu, samples %llu, keys %llu (T %llu, B %llu), mixed %llu, jittered "
         "camera %llu)\n",
         static_cast<unsigned long long>(ta.draws), commitFrame,
         static_cast<unsigned long long>(g_toggles - tog0), ta.hudMaxPx, ta.bezelMaxPx,
         static_cast<unsigned long long>(g_replays), static_cast<unsigned long long>(oc::g_os.obsUploads.load()),
         static_cast<unsigned long long>(oc::g_os.lSamples.load()), static_cast<unsigned long long>(oc::g_os.lKeys.load()),
         static_cast<unsigned long long>(oc::g_os.lKeysT.load()), static_cast<unsigned long long>(oc::g_os.lKeysB.load()),
         static_cast<unsigned long long>(oc::g_os.lMixed.load()), static_cast<unsigned long long>(oc::g_os.camJitter.load()));
  Check(ta.mismatch == 0 && ta.draws > 100000 && g_toggles > tog0 + 500,
        "ovl cpu replica: 2000 frames x 4 views drawn (stream output read back per draw), toggles behind our back");
  Check(ta.hudBad == 0 && ta.hudMaxPx < 0.05,
        "ovl cpu replica: the collimated HUD (per-eye records; its glyphs, repeater and the DED in one (c9, c8) pair "
        "with a glyph count changing per view and frame) at DCS's camera, zero disparity against its direction at "
        "infinity, in every frame (also while learning)");
  printf("     (bezel: draws off their bezel after the commit %llu)\n", static_cast<unsigned long long>(ta.bezelBad));
  Check(commitFrame >= 16 && commitFrame <= 120 && ta.bezelBad == 0 && ta.bezelMaxPx < 0.05,
        "ovl cpu replica: DED, MFDs, EHSI, JHMCS, LDR panel and a focus-only display learned T (16 agreeing "
        "frames, within 120 under 45 % observer loss), then on their true-eye bezels in every frame, view (focus "
        "views culling displays) and effect pattern (dirty bit, shared buffer, upload-skip cache, Map, several "
        "draws)");
  Check(ta.labelBad == 0 && ta.guiBad == 0 && ta.neither == 0,
        "ovl cpu replica: world labels at DCS's camera, the 2D GUI untouched, no draw at any third camera");
  Check(aba == 0, "ovl cpu replica: no A-B-A for any record and view (no run shorter than the commit streak between "
                  "two runs of the same camera)");
  if (aba) printf("     (A-B-A in %d tracks, %d events)\n", abaTracks, aba);
  Check(g_restoreBad == 0 && oc::g_os.restoreMismatch.load() == 0 && oc::g_os.restored.load() >= g_replays * 3 / 4,
        "ovl cpu replica: currentContext's viewProj bit-equal to DCS's after every replay");
  Check(oc::g_os.failCam.load() == 0 && oc::g_os.failSplit.load() == 0 && oc::g_os.ctxForeign.load() == 0 &&
            oc::g_os.pairLate.load() == 0 && oc::g_os.cpuMiss.load() == 0 && oc::g_os.cpuWrongB.load() == 0 &&
            oc::g_os.recAlign.load() == 0 && oc::g_os.lContra.load() == 0 && oc::g_os.viewMapDiff.load() == 0 &&
            oc::g_os.recurInert.load() == 0 && !ck::g_ovlInert.load(),
        "ovl cpu replica: ovlCpuCam, ovlCpuSplit, ctxForeign, record alignment, ovlCpuPairLate, cpu miss, wrong B, "
        "contradicted keys, view map differences all 0; overlays not inert");
  {
    const oc::KeyRec* noisy = oc::g_learn.FindC(KeyOf(kENoisy, kMatNoisy, 2, 7));
    bool hudB = true;
    for (uint64_t k : {KeyOf(kEHud0, kMatHud, 1, 0), KeyOf(kEHud0 + 1, kMatHud, 1, 0), KeyOf(kEHud0 + 2, kMatHud, 1, 0),
                       KeyOf(kERep, kMatHud, 1, 1), KeyOf(kEGlyph0, kMatHud, 1, 1), KeyOf(kEGlyph0 + 1, kMatHud, 1, 1),
                       KeyOf(kEGlyph0 + 2, kMatHud, 1, 1)})
      hudB = hudB && oc::g_learn.ClassOf(k) == oc::kClsB;
    Check(hudB && AllTCommitted() && noisy && noisy->frozen &&
              oc::g_learn.ClassOf(KeyOf(kENoisy, kMatNoisy, 2, 7)) == oc::kClsB && oc::g_os.lMixed.load() >= 1 &&
              oc::g_os.lFlips.load() == 0,
          "ovl cpu replica learner: HUD / HUD DED / glyph keys B, cockpit display keys T (canted context views, "
          "jittered focus G-buffer camera; the focus-only display from views 2 / 3), the noisy key frozen at B "
          "(mixed), no class flipped");
  }
  Check(oc::g_os.camJitter.load() > 0 && oc::g_os.recSkip.load() > 0 && oc::g_os.recState.load() > 0 &&
            oc::g_os.obsWorld.load() > 0 && perMode[kFxMap] > 0 && oc::g_os.cpuOk.load() > 0,
        "ovl cpu replica: jittered G-buffer camera accepted, filters skip records, state records passed over, the "
        "observer found the world matrices (UpdateSubresource and Map / Unmap)");

  // ---- Fail-safes (classes committed). ----
  g_fxMode = kFxDirty;
  H.handler.composite = nullptr;
  Check(ck::g_cost[ck::kCostOvlCpu].load() > 0,
        "ovl cpu cost (R32 r3): the replay / filter hooks and the frame end are in the render-thread cost line");
  auto frameExpect = [&](const FrameOpts& o, bool m0, bool m1, bool m2, bool m3, Tally& t) {
    RunFrame(o);
    bool moved[4] = {m0, m1, m2, m3};
    Analyse(t, true, moved, -1);
  };
  auto allFrames = [&](const FrameOpts& o, int n, bool m, Tally& t) {
    for (int i = 0; i < n; ++i) frameExpect(o, m, m, m, m, t);
  };
  // The frames left of a stop's block (nothing checked), then one frame with the views moved again.
  auto waitBlock = [&]() {
    FrameOpts n;
    for (int i = 0; i < 2 * oc::kPairRecover && oc::g_frameBlock > 0; ++i) RunFrame(n);
    RunFrame(n);
  };
  // Frames of o while the last stop's block lasts (at most 30; no view moved), then n frames expecting m. (A
  // blocked frame checks nothing, so a rule that still holds stops the frame again only once the block is over.)
  auto afterBlock = [&](const FrameOpts& o, int n, bool m, Tally& tb, Tally& t) {
    int b = 0;
    for (; b < 2 * oc::kPairRecover && oc::g_frameBlock > 0; ++b) frameExpect(o, false, false, false, false, tb);
    allFrames(o, n, m, t);
    return b;
  };
  {
    // The frame stop: view 1's camera identity fails in frame N after view 0 was drawn moved (counted
    // ovlCpuPairLate: views 0 and 2 differ in that one frame, a later view's failure cannot be known earlier);
    // views 2 / 3 not moved in frame N, no view in N+1..N+30, every view in N+31. Later replays of a view decided
    // not moved count as latched.
    ClearFailHistory();
    const uint64_t late0 = oc::g_os.pairLate.load(), cam0 = oc::g_os.failCam.load(), np0 = oc::g_os.natPair.load(),
                   nl0 = oc::g_os.natLatched.load(), sf0 = oc::g_os.stopFail.load();
    Tally t0, t1, t2;
    FrameOpts o;
    o.camWrongView = 1;
    frameExpect(o, true, false, false, false, t0);
    FrameOpts n;
    allFrames(n, oc::kPairRecover, false, t1);
    allFrames(n, 1, true, t2);
    Check(t0.bezelBad == 0 && t1.bezelBad == 0 && t2.bezelBad == 0 && t0.neither + t1.neither + t2.neither == 0 &&
              oc::g_os.pairLate.load() == late0 + 1 && oc::g_os.failCam.load() == cam0 + 1 &&
              oc::g_os.stopFail.load() == sf0 + 1 && oc::g_os.natPair.load() == np0 + 2 + 4 * oc::kPairRecover &&
              oc::g_os.natLatched.load() >= nl0 + 4 * oc::kPairRecover && t0.eyeDisagree == 1 &&
              t1.eyeDisagree + t2.eyeDisagree == 0,
          "ovl cpu frame stop (R32 r3): a camera mismatch in view 1 stops the whole frame (views 2 / 3 not moved in "
          "it), no view moved for the next 30 frames, then all four again; one ovlCpuCam, one stop, ovlCpuPairLate "
          "once; views of one eye differ only in that frame (view 0 drawn before the failure)");
  }
  {
    // A stop again within 300 frames of the previous one: inert at once. The failure at view 0 (nothing drawn
    // moved yet): no view moved in that frame, no ovlCpuPairLate.
    const uint64_t r0 = oc::g_os.recurInert.load(), late0 = oc::g_os.pairLate.load();
    FrameOpts o;
    o.camWrongView = 0;
    Tally t0;
    frameExpect(o, false, false, false, false, t0);
    const bool inert = ck::g_ovlInert.load();
    Tally t1;
    FrameOpts n;
    frameExpect(n, false, false, false, false, t1);
    Check(inert && oc::g_os.recurInert.load() == r0 + 1 && oc::g_os.pairLate.load() == late0 && t0.bezelBad == 0 &&
              t1.bezelBad == 0 && t1.hudBad == 0 && t0.eyeDisagree + t1.eyeDisagree == 0,
          "ovl cpu frame stop: a failure 32 frames after the previous stop makes the overlays inert at once (every "
          "view at DCS's camera, counted recurInert); a failure at the frame's first view moves no view of it");
    ck::g_ovlInert = false;
    ClearFailHistory();
    waitBlock();
  }
  {
    // R32 r3 (4): the stop's block is checked before the camera identity: a 2-frame camera mismatch is one failure
    // (the second frame is blocked and checks nothing), not a recurrence; all moved again 31 frames after the first.
    ClearFailHistory();
    const uint64_t cam0 = oc::g_os.failCam.load(), sf0 = oc::g_os.stopFail.load(), r0 = oc::g_os.recurInert.load();
    Tally t0, t1, t2, t3;
    FrameOpts o;
    o.camWrongView = 1;
    frameExpect(o, true, false, false, false, t0);
    frameExpect(o, false, false, false, false, t1);
    FrameOpts n;
    allFrames(n, oc::kPairRecover - 1, false, t2);
    allFrames(n, 1, true, t3);
    Check(oc::g_os.failCam.load() == cam0 + 1 && oc::g_os.stopFail.load() == sf0 + 1 &&
              oc::g_os.recurInert.load() == r0 && !ck::g_ovlInert.load() &&
              t0.bezelBad + t1.bezelBad + t2.bezelBad + t3.bezelBad == 0 &&
              t1.eyeDisagree + t2.eyeDisagree + t3.eyeDisagree == 0,
          "ovl cpu frame stop (R32 r3): a camera mismatch lasting 2 frames is one failure (the blocked frame checks "
          "nothing before the block), no recurrence, no inert; every view moved again 31 frames after it");
  }
  {
    // DCS pushes another context mid-replay (view 0) after two cockpit displays were drawn at the true eye: the
    // pushed copy carries our bytes, so DCS's are put into it (and into the saved one); the HUD after it at DCS's
    // camera; counted ctxForeign; the context restored bit for bit; no view moved for the next 30 frames.
    ClearFailHistory();
    const uint64_t f0 = oc::g_os.ctxForeign.load(), r0 = g_restoreBad, cp0 = oc::g_os.copyPutBack.load();
    Tally t0, t1;
    FrameOpts o;
    o.ctxSwitchView = 0;
    o.shuffle = false;
    o.tFirst = true;
    RunFrame(o);
    std::vector<int> st;
    bool moved[4] = {false, false, false, false};
    Analyse(t0, true, moved, -1, &st);
    int tTrue0 = 0, tDcs0 = 0;
    for (size_t i = 0; i < g_log.size(); ++i)
      if (g_log[i].t == 0 && KindT(g_log[i].kind) && g_log[i].kind != kPanel) (st[i] == 1 ? tTrue0 : tDcs0)++;
    FrameOpts n;
    allFrames(n, oc::kPairRecover, false, t1);
    printf("     (push: hud %llu label %llu tTrue0 %d tDcs0 %d t1.bezel %llu foreign %llu copy %llu restoreBad %llu)\n",
           (unsigned long long)t0.hudBad, (unsigned long long)t0.labelBad, tTrue0, tDcs0, (unsigned long long)t1.bezelBad,
           (unsigned long long)(oc::g_os.ctxForeign.load() - f0), (unsigned long long)(oc::g_os.copyPutBack.load() - cp0),
           (unsigned long long)(g_restoreBad - r0));
    Check(t0.hudBad == 0 && t0.labelBad == 0 && tTrue0 == 2 && tDcs0 > 0 && t1.bezelBad == 0 &&
              oc::g_os.ctxForeign.load() == f0 + 1 && oc::g_os.copyPutBack.load() == cp0 + 1 && g_restoreBad == r0,
          "ovl cpu fail-safe: currentContext pushed (a copy of ours) inside a replay after two displays at the true "
          "eye: DCS's camera put back into the copy, every later record (the HUD) at DCS's camera, counted "
          "ctxForeign, the saved context restored bit for bit, no view moved the next 30 frames");
  }
  {
    // DCS writes its own camera into the context mid-replay (view 2): ours stop, DCS's bytes are left as written.
    ClearFailHistory();
    const uint64_t m0 = oc::g_os.restoreMismatch.load();
    FrameOpts o;
    o.rewriteView = 2;
    o.shuffle = false;
    o.tFirst = true;
    RunFrame(o);
    ReadDraws();
    Check(oc::g_os.restoreMismatch.load() == m0 + 1 && g_rewriteKept == 1,
          "ovl cpu fail-safe: DCS rewrites currentContext's viewProj inside a replay: our writes stop, DCS's bytes "
          "are not overwritten at the return (restore mismatch counted)");
    FrameOpts n;
    Tally t1;
    allFrames(n, oc::kPairRecover, false, t1);
    // DCS sets its own same camera again mid-replay: nothing of ours is left there, the replay goes on (no failure).
    ClearFailHistory();
    const uint64_t cr0 = oc::g_os.ctxReset.load(), cf0 = oc::g_os.ctxForeign.load(), m1 = oc::g_os.restoreMismatch.load();
    Tally t2;
    FrameOpts s;
    s.rewriteView = 2;
    s.rewriteSame = true;
    s.shuffle = false;
    s.tFirst = true;
    frameExpect(s, true, true, true, true, t2);
    printf("     (same: bezel %llu hud %llu reset %llu foreign %llu mismatch %llu)\n", (unsigned long long)t2.bezelBad,
           (unsigned long long)t2.hudBad, (unsigned long long)(oc::g_os.ctxReset.load() - cr0),
           (unsigned long long)(oc::g_os.ctxForeign.load() - cf0), (unsigned long long)(oc::g_os.restoreMismatch.load() - m1));
    Check(t1.bezelBad == 0 && t2.bezelBad == 0 && t2.hudBad == 0 && oc::g_os.ctxReset.load() == cr0 + 1 &&
              oc::g_os.ctxForeign.load() == cf0 && oc::g_os.restoreMismatch.load() == m1,
          "ovl cpu fail-safe: DCS sets its same camera again inside a replay: the replay stays moved, every record "
          "at its camera, no failure");
  }
  {
    // No per-record filter point (DLSS off: a null std::function; or a callable type we do not hook): the records
    // of every HMD view's range decide, once per frame (R32 r3). With the HUD in them every view stays native,
    // whole (a rule stop before any view moved: the next 30 frames blocked, no recurrence). Committed displays
    // only: moved whole. An element never measured among displays: native, also when only view 3 draws it (was:
    // views 0-2 moved, view 3 not). The composite lambda around a null filter is a per-record point.
    ClearFailHistory();
    g_verbose = true;
    const uint64_t fc0 = oc::g_os.failCam + oc::g_os.failSplit + oc::g_os.cpuMiss + oc::g_os.cpuWrongB +
                         oc::g_os.ctxForeign + oc::g_os.recAlign + oc::g_os.lContra;
    const uint64_t nf0 = oc::g_os.nullFilter.load(), uf0 = oc::g_os.unknownFilter.load(), sc0 = oc::g_os.scanned.load(),
                   late0 = oc::g_os.pairLate.load(), sr0 = oc::g_os.stopRule.load(), ri0 = oc::g_os.recurInert.load();
    Tally t0, t1, tb, t2, t3, tb2, t4, t5, t6, tb3, t7;
    FrameOpts o;
    o.nullFilter = true;
    allFrames(o, 10, false, t0);
    FrameOpts u;
    u.unknownFilter = true;
    allFrames(u, 10, false, t1);
    FrameOpts dOnly = o;
    dOnly.edit = [](int, std::vector<Rec>& l) {
      std::vector<Rec> k;
      for (const Rec& r : l)
        if (KindT(r.kind) || r.kind == kState) k.push_back(r);
      l = k;
    };
    const int b1 = afterBlock(dOnly, 3, true, tb, t2);  // the block of the last rule stop above
    FrameOpts dNew = o;
    dNew.edit = [](int, std::vector<Rec>& l) {
      std::vector<Rec> k;
      for (const Rec& r : l)
        if (KindT(r.kind)) k.push_back(r);
      k.push_back(MakeRec(1, 0, kNewHud, kENewHud, kMatHud));
      l = k;
    };
    allFrames(dNew, 3, false, t3);
    H.handler.composite = &H;
    const int b2 = afterBlock(o, 3, true, tb2, t4);
    H.handler.composite = nullptr;
    FrameOpts n;
    allFrames(n, 5, true, t5);
    FrameOpts dNew3 = o;
    dNew3.edit = [](int t, std::vector<Rec>& l) {
      std::vector<Rec> k;
      for (const Rec& r : l)
        if (KindT(r.kind)) k.push_back(r);
      if (t == 3) k.push_back(MakeRec(1, 0, kNewHud, kENewHud, kMatHud));
      l = k;
    };
    allFrames(dNew3, 3, false, t6);
    const int b3 = afterBlock(n, 1, true, tb3, t7);
    g_verbose = false;
    const uint64_t bezel = t0.bezelBad + t1.bezelBad + tb.bezelBad + t2.bezelBad + t3.bezelBad + tb2.bezelBad +
                           t4.bezelBad + t5.bezelBad + t6.bezelBad + tb3.bezelBad + t7.bezelBad;
    const uint64_t eye = t0.eyeDisagree + t1.eyeDisagree + tb.eyeDisagree + t2.eyeDisagree + t3.eyeDisagree +
                         tb2.eyeDisagree + t4.eyeDisagree + t5.eyeDisagree + t6.eyeDisagree + tb3.eyeDisagree +
                         t7.eyeDisagree;
    printf("     (scan: bezel %llu eye %llu hud %llu %llu %llu %llu nf %llu uf %llu frame scans %llu rule stops %llu "
           "late %llu fails %llu)\n",
           (unsigned long long)bezel, (unsigned long long)eye, (unsigned long long)t0.hudBad,
           (unsigned long long)t1.hudBad, (unsigned long long)t3.hudBad, (unsigned long long)t4.hudBad,
           (unsigned long long)(oc::g_os.nullFilter.load() - nf0), (unsigned long long)(oc::g_os.unknownFilter.load() - uf0),
           (unsigned long long)(oc::g_os.scanned.load() - sc0), (unsigned long long)(oc::g_os.stopRule.load() - sr0),
           (unsigned long long)(oc::g_os.pairLate.load() - late0),
           (unsigned long long)(oc::g_os.failCam + oc::g_os.failSplit + oc::g_os.cpuMiss + oc::g_os.cpuWrongB +
                                oc::g_os.ctxForeign + oc::g_os.recAlign + oc::g_os.lContra - fc0));
    printf("     (scan blocks %d %d %d frames)\n", b1, b2, b3);
    Check(bezel == 0 && eye == 0 && t0.hudBad + t1.hudBad + t3.hudBad + t4.hudBad + t6.hudBad == 0 &&
              b1 <= oc::kPairRecover && b2 == oc::kPairRecover - 2 && b3 == oc::kPairRecover - 2 &&
              oc::g_os.nullFilter.load() >= nf0 + 80 && oc::g_os.unknownFilter.load() >= uf0 + 80 &&
              oc::g_os.scanned.load() > sc0 && oc::g_os.pairLate.load() == late0 && oc::g_os.stopRule.load() > sr0 &&
              oc::g_os.recurInert.load() == ri0 && !ck::g_ovlInert.load() &&
              oc::g_os.failCam + oc::g_os.failSplit + oc::g_os.cpuMiss + oc::g_os.cpuWrongB + oc::g_os.ctxForeign +
                      oc::g_os.recAlign + oc::g_os.lContra ==
                  fc0,
          "ovl cpu fail-safe: a null or unknown filter: the records of the frame's views decide, alike for all four "
          "(with the HUD: native; displays only: moved whole; a never-measured element, even in view 3 only: "
          "native); a rule stop before any view moved blocks 30 frames, no recurrence; the composite around a null "
          "filter moves per record; views of one eye never differ");
  }
  {
    // The cockpit not moved in a frame (off / failed G-buffer): the overlays at DCS's camera too, whole frame.
    Tally t0;
    FrameOpts o;
    o.patched = false;
    frameExpect(o, false, false, false, false, t0);
    Check(t0.bezelBad == 0 && t0.hudBad == 0, "ovl cpu fail-safe: a view whose cockpit was not moved draws its "
                                               "symbology at DCS's camera");
    ClearFailHistory();
    waitBlock();
  }
  {
    // Depth boost at scale 1 (h 0, nothing patched, nothing to move) for 70 frames, then a ramp's first frames: no
    // stop at scale 1, and every view moved from the ramp's first frame (was: a rule stop every 31 frames at scale
    // 1; its 30-frame block kept the displays at DCS's boosted eye after the ramp began while the cockpit was at
    // the true eye: live 2026-10-14, F-16, 9 frames at 5 mm/s).
    ClearFailHistory();
    const uint64_t sf0 = oc::g_os.stopFail.load(), sr0 = oc::g_os.stopRule.load(), np0 = oc::g_os.natPair.load(),
                   nc0 = oc::g_os.natCockpit.load();
    Tally ts, tr;
    FrameOpts s;
    s.scale1 = true;
    allFrames(s, 70, false, ts);
    const bool noBlock = oc::g_frameBlock == 0;
    FrameOpts n;
    allFrames(n, 3, true, tr);
    Check(ts.bezelBad + ts.hudBad + ts.neither + ts.eyeDisagree == 0 &&
              tr.bezelBad + tr.hudBad + tr.neither + tr.eyeDisagree == 0 && noBlock &&
              oc::g_os.stopFail.load() == sf0 && oc::g_os.stopRule.load() == sr0 && oc::g_os.natPair.load() == np0 &&
              oc::g_os.natCockpit.load() >= nc0 + 70 * 4,
          "ovl cpu at scale 1 (h 0): nothing to move is no stop (no rule stop, no 30-frame block over 70 frames); the "
          "ramp's first frame moves every view at once (the displays on their bezels with the cockpit at the true "
          "eye)");
  }
  {
    // A mission load (the learner's table cleared), 200 frames at scale 1 (h 0; about 1 in 4 observed), then a ramp: the
    // learner observes at h 0 (each record compared between the two eyes needs no shift), so every display key is
    // committed T and every HUD key B before the first boost; at h 0 nothing is moved, and no cpu miss, wrong B, key
    // failure or class flip comes from the observation; the ramp's first frame draws every display on its bezel (was:
    // about 16 boosted frames at DCS's eye after each mission load while the learner committed).
    oc::g_learn.Reset();
    ClearFailHistory();
    const uint64_t miss0 = oc::g_os.cpuMiss.load(), wb0 = oc::g_os.cpuWrongB.load(), kf0 = oc::g_os.keyFail.load(),
                   kv0 = oc::g_os.keyFailView.load(), fl0 = oc::g_learn.flips, sf0 = oc::g_os.stopFail.load(),
                   sr0 = oc::g_os.stopRule.load(), lp0 = ck::g_st.ovlLearnPasses.load();
    Tally ts, tr;
    FrameOpts s;
    s.scale1 = true;
    allFrames(s, 200, false, ts);
    const bool ready = AllTCommitted() && oc::g_learn.ClassOf(KeyOf(kEHud0, kMatHud, 1, 0)) == oc::kClsB;
    const bool clean = oc::g_os.cpuMiss.load() == miss0 && oc::g_os.cpuWrongB.load() == wb0 &&
                       oc::g_os.keyFail.load() == kf0 && oc::g_os.keyFailView.load() == kv0 &&
                       oc::g_learn.flips == fl0 && oc::g_os.stopFail.load() == sf0 && oc::g_os.stopRule.load() == sr0;
    FrameOpts n;
    bool all[4] = {true, true, true, true};
    for (int i = 0; i < 3; ++i) {
      RunFrame(n);
      Analyse(tr, true, all, -1);
    }
    printf("     (learn at scale 1: ready %d, clean %d, learner passes %llu; scale 1 bezel %llu hud %llu neither %llu; "
           "ramp bezel %llu hud %llu eye %llu)\n",
           ready, clean, (unsigned long long)(ck::g_st.ovlLearnPasses.load() - lp0), (unsigned long long)ts.bezelBad,
           (unsigned long long)ts.hudBad, (unsigned long long)ts.neither, (unsigned long long)tr.bezelBad,
           (unsigned long long)tr.hudBad, (unsigned long long)tr.eyeDisagree);
    Check(ready && clean && ck::g_st.ovlLearnPasses.load() > lp0 && ts.bezelBad + ts.hudBad + ts.neither == 0 &&
              tr.bezelBad + tr.hudBad + tr.neither + tr.eyeDisagree == 0 && tr.draws > 0,
          "ovl cpu learner at scale 1: after a mission load, 200 frames at h 0 (about 1 in 4 observed) commit every "
          "display key T and the HUD "
          "B with nothing moved and no cpu miss / wrong B / key failure / flip / stop; the ramp's first frame draws "
          "every display on its bezel");
  }
  {
    // R32 r3 (2): the overlays turned off / on / inert between view 1 and view 2 of a frame: the frame keeps the
    // state latched at its first replay (all four views alike); the change applies from the next frame. No stop.
    ClearFailHistory();
    const uint64_t sf0 = oc::g_os.stopFail.load(), sr0 = oc::g_os.stopRule.load(), no0 = oc::g_os.natOff.load();
    FrameOpts n;
    Tally a[8];
    g_beforeView = [](int t) {
      if (t == 2) ck::g_cfgOverlays = false;
    };
    allFrames(n, 1, true, a[0]);
    g_beforeView = nullptr;
    allFrames(n, 1, false, a[1]);
    g_beforeView = [](int t) {
      if (t == 2) ck::g_cfgOverlays = true;
    };
    allFrames(n, 1, false, a[2]);
    g_beforeView = nullptr;
    allFrames(n, 1, true, a[3]);
    g_beforeView = [](int t) {
      if (t == 2) ck::g_ovlInert = true;
    };
    allFrames(n, 1, true, a[4]);
    g_beforeView = nullptr;
    allFrames(n, 1, false, a[5]);
    ck::g_ovlInert = false;
    allFrames(n, 1, true, a[6]);
    uint64_t bad = 0, eye = 0;
    for (int i = 0; i < 7; ++i) bad += a[i].bezelBad + a[i].hudBad + a[i].neither, eye += a[i].eyeDisagree;
    Check(bad == 0 && eye == 0 && oc::g_os.stopFail.load() == sf0 && oc::g_os.stopRule.load() == sr0 &&
              oc::g_os.natOff.load() >= no0 + 8 && ck::g_cfgOverlays.load(),
          "ovl cpu frame decision (R32 r3): overlays turned off, on, or inert between view 1 and view 2 of a frame: "
          "the frame keeps its state from its first replay for all four views, the change applies from the next "
          "frame, no stop");
  }
  {
    // R32 r3 (3): an MVP builder that does not read the context (the EHSI built from DCS's camera) in every frame:
    // the observer's cpu miss names the EHSI's key, which is frozen at DCS's eye at once. No view fails, no frame
    // stops, nothing counts toward inert: every other display stays at the true eye. No A-B-A for any record and
    // view (was: the view failed, the pair blocked 30 frames twice, then inert: two flashes of every display).
    ClearFailHistory();
    g_tracks.clear();
    const uint64_t miss0 = oc::g_os.cpuMiss.load(), kf0 = oc::g_os.keyFail.load(), kv0 = oc::g_os.keyFailView.load(),
                   sf0 = oc::g_os.stopFail.load(), sr0 = oc::g_os.stopRule.load(), np0 = oc::g_os.natPair.load(),
                   ri0 = oc::g_os.recurInert.load();
    const uint64_t ehsi = KeyOf(kEEhsi, kMatEhsi, 2, 2);
    Tally tm;
    std::vector<int> st;
    int ehsiTrue = 0, otherDcs = 0, otherN = 0;
    bool all[4] = {true, true, true, true};
    for (int i = 0; i < 60; ++i) {
      FrameOpts o;  // toggles and shuffled lists
      o.missKind = kEhsi;
      RunFrame(o);
      Analyse(tm, false, all, i, &st);
      for (size_t k = 0; k < g_log.size(); ++k) {
        if (g_log[k].kind == kEhsi) ehsiTrue += st[k] == 1;
        if (g_log[k].kind >= 0 && KindT(g_log[k].kind) && g_log[k].kind != kEhsi) ++otherN, otherDcs += st[k] == 0;
      }
    }
    int aba = 0;
    for (auto& kv : g_tracks) aba += CountAba(kv.second.st, oc::kLearnCommit);
    const oc::KeyRec* e = oc::g_learn.FindC(ehsi);
    printf("     (cpu miss: misses %llu, keys frozen %llu, EHSI at the true eye %d, other displays at DCS's %d of %d, "
           "A-B-A %d)\n",
           (unsigned long long)(oc::g_os.cpuMiss.load() - miss0), (unsigned long long)(oc::g_os.keyFail.load() - kf0),
           ehsiTrue, otherDcs, otherN, aba);
    Check(oc::g_os.cpuMiss.load() > miss0 && oc::g_os.keyFail.load() == kf0 + 1 && oc::g_os.keyFailView.load() == kv0 &&
              oc::g_os.stopFail.load() == sf0 && oc::g_os.stopRule.load() == sr0 && oc::g_os.natPair.load() == np0 &&
              oc::g_os.recurInert.load() == ri0 && !ck::g_ovlInert.load() && e && e->frozen && e->failFrozen &&
              ehsiTrue == 0 && otherDcs == 0 && otherN > 1000 && aba == 0 && tm.eyeDisagree == 0 && tm.hudBad == 0 &&
              tm.neither == 0,
          "ovl cpu per-key failure (R32 r3): a cockpit display whose MVP is not built from the context (cpu miss in "
          "every frame) freezes its key at DCS's eye at once; no view or frame fails, every other display stays at "
          "the true eye: no A-B-A for any record and view over 60 frames, views of one eye never differ, nothing "
          "toward inert");
  }
  {
    // R32 r3 (3): a HUD record built once with the camera of the display drawn before it (wrong B: our VP_true
    // reached a record it was not for) names the HUD's key: frozen at DCS's eye at once; the views and the frame
    // go on (that one HUD draw off in that one view), every display at the true eye in every frame. The same key
    // failing again would be the view's failure (not the key's).
    ClearFailHistory();
    const uint64_t wb0 = oc::g_os.cpuWrongB.load(), kf0 = oc::g_os.keyFail.load(), sf0 = oc::g_os.stopFail.load(),
                   sr0 = oc::g_os.stopRule.load();
    FrameOpts o;
    o.toggles = false;
    o.shuffle = false;
    o.tFirst = true;
    Tally tw;
    std::vector<int> st;
    int hudOff = 0, hudOffFrame = -1, dispOff = 0;
    bool all[4] = {true, true, true, true};
    g_leakKind = kHud;
    for (int i = 0; i < 20; ++i) {
      RunFrame(o);
      Analyse(tw, false, all, -1, &st);
      for (size_t k = 0; k < g_log.size(); ++k) {
        if (KindB(g_log[k].kind) && st[k] != 0) ++hudOff, hudOffFrame = i;
        if (g_log[k].kind >= 0 && KindT(g_log[k].kind) && g_log[k].kind != kEhsi && st[k] != 1) ++dispOff;
      }
    }
    g_leakKind = -1;
    const oc::KeyRec* e = oc::g_learn.FindC(KeyOf(kEHud0, kMatHud, 1, 0));
    Check(oc::g_os.cpuWrongB.load() == wb0 + 1 && oc::g_os.keyFail.load() == kf0 + 1 &&
              oc::g_os.stopFail.load() == sf0 && oc::g_os.stopRule.load() == sr0 && hudOff == 1 && hudOffFrame == 0 &&
              dispOff == 0 && e && e->failFrozen && tw.eyeDisagree == 0,
          "ovl cpu per-key failure (R32 r3): a HUD record built once with the previous display's camera (wrong B) "
          "freezes its key at DCS's eye; no stop, that one draw off in one view and frame, every display at the true "
          "eye in every frame");
  }
  {
    // R32 r3 (7): a C++ exception through DCS's composite filter. Caught by DCS's record loop: the hooks' busy
    // count given back, the nesting depth restored, the rest of the replay at DCS's camera (a failure: the frame
    // stops), the context restored bit for bit. Leaving the replay: the replay closed, every count back to 0.
    ClearFailHistory();
    H.handler.composite = &H;
    const uint64_t fu0 = oc::g_os.filterUnwind.load(), sf0 = oc::g_os.stopFail.load(), rb0 = g_restoreBad;
    const int c0 = g_caught;
    g_catchInReplay = true;
    g_throwIn = 3;  // view 0's HDR replay, its fourth record
    FrameOpts o;
    o.toggles = false;
    Tally te;
    bool none[4] = {false, false, false, false};
    RunFrame(o);
    Analyse(te, false, none, -1);
    g_catchInReplay = false;
    const bool caughtOk = g_caught == c0 + 1 && oc::g_os.filterUnwind.load() == fu0 + 1 &&
                          oc::g_os.stopFail.load() == sf0 + 1 && oc::g_hookBusy.load() == 0 &&
                          oc::g_replayBusy.load() == 0 && oc::g_replayOpen.load() == 0 && !oc::t_rp.open &&
                          te.hudBad == 0 && g_restoreBad == rb0;
    g_throwIn = 0;
    bool threw = false;
    try {
      reinterpret_cast<ReplayFnT>(g_handlerVt[3])(&H.handler, 0, g_curCtx, &H.fnHdr);
    } catch (const std::exception&) {
      threw = true;
    }
    g_throwIn = -1;
    const bool leftOk = threw && oc::g_hookBusy.load() == 0 && oc::g_replayBusy.load() == 0 &&
                        oc::g_replayOpen.load() == 0 && !oc::t_rp.open;
    H.handler.composite = nullptr;
    printf("     (exception: caught %d, unwinds %llu, busy %d / %d / %d)\n", g_caught - c0,
           (unsigned long long)(oc::g_os.filterUnwind.load() - fu0), oc::g_hookBusy.load(), oc::g_replayBusy.load(),
           oc::g_replayOpen.load());
    Check(caughtOk && leftOk,
          "ovl cpu (R32 r3): a C++ exception through DCS's composite filter, caught by DCS's record loop or leaving "
          "the replay: the hooks' busy counts given back, the rest of the replay at DCS's camera, the context "
          "restored, the replay closed");
    ClearFailHistory();
    waitBlock();
  }
  {
    // R32 r3 (1): a mission event (load, slot change, simulation stop) between frames: the next frame moves
    // nothing, the learned table is cleared at its end, and no class learned before applies to a record after it
    // (every key starts at DCS's eye: no display at the true eye for at least the 16-frame commit); then relearned.
    // A mission event arriving between view 1 and view 2 of a frame: that frame stays moved (latched), the next
    // one not. A scene cut (scene_cut.h) does the same.
    ClearFailHistory();
    ck::g_ovlInert = false;
    FrameOpts n;
    n.toggles = false;
    // (The EHSI and one HUD key stay frozen from the per-key tests above until this reset.)
    const bool ready = oc::g_learn.ClassOf(KeyOf(kEDed, kMatHud, 1, 1)) == oc::kClsT &&
                       oc::g_learn.ClassOf(KeyOf(kEEhsi, kMatEhsi, 2, 2)) == oc::kClsB;
    const uint64_t rm0 = oc::g_os.lResetMission.load(), rc0 = oc::g_os.lResetCut.load(), nr0 = oc::g_os.natReset.load(),
                   rs0 = oc::g_os.lResets.load();
    std::vector<int> st;
    bool all[4] = {true, true, true, true};
    auto trueDisplays = [&](Tally& t) {
      Analyse(t, false, all, -1, &st);
      int n1 = 0;
      for (size_t k = 0; k < g_log.size(); ++k)
        if (g_log[k].kind >= 0 && KindT(g_log[k].kind) && st[k] == 1) ++n1;
      return n1;
    };
    // Mid-frame: the frame keeps its decision (latched at its first replay); the table is cleared at its end.
    g_beforeView = [](int t) {
      if (t == 2) oc::OnMissionEvent();
    };
    Tally ta, tb, tc, td, te;
    RunFrame(n);
    const int midFrame = trueDisplays(ta);
    g_beforeView = nullptr;
    const bool cleared = oc::g_learn.used == 0 && oc::g_learn.ClassOf(KeyOf(kEDed, kMatHud, 1, 1)) == oc::kClsUnknown &&
                         oc::g_os.lResets.load() == rs0 + 1 && oc::g_os.natReset.load() == nr0;
    int after = 0;
    for (int i = 0; i < oc::kLearnCommit - 1; ++i) {
      RunFrame(n);
      after += trueDisplays(tc);
    }
    for (int i = 0; i < 120 && !AllTCommitted(); ++i) RunFrame(n);
    const bool relearned = AllTCommitted();
    RunFrame(n);
    const int moved = trueDisplays(td);
    // The frame dump's per-view cpu counters of that frame: both replays moved, records at the true eye (the
    // displays) and at DCS's (the HUD), every record that passed the filter one of them, no failure.
    bool dumpCsv = true;
    for (int t = 0; t < 4; ++t) {
      const ck::ViewFrame& c = g_cpuSnap[t];
      dumpCsv = dumpCsv && c.cpuWhy == oc::kMoved && c.cpuReplays == 2 && c.cpuMoved == 2 && c.cpuRecT > 0 &&
                c.cpuRecB > 0 && c.cpuRecs == c.cpuRecT + c.cpuRecB && c.cpuFails == 0 && c.cpuKeyFails == 0;
    }
    const int keysT = oc::g_learn.keysT, keysB = oc::g_learn.keysB;
    const ck::ViewFrame snap2 = g_cpuSnap[2];
    // A mission event between frames: the next frame moves nothing (the reset pending at its first replay), the
    // table cleared at its end.
    oc::OnMissionEvent();
    RunFrame(n);
    const int resetFrame = trueDisplays(tb);
    const bool cleared2 = oc::g_learn.used == 0 && oc::g_os.lResets.load() == rs0 + 2 && oc::g_os.natReset.load() == nr0 + 4;
    for (int i = 0; i < 120 && !AllTCommitted(); ++i) RunFrame(n);
    RunFrame(n);
    const int moved2 = trueDisplays(te);
    // A jump scene cut between frames (an F1 / F2 switch, or a false positive from a frame hitch: live 2026-10-14,
    // F-14A / A-10C, one every ~2 s): nothing is freed, the table is kept, the next frames stay moved (the old
    // behaviour put the displays at DCS's eye for the relearn: a jump of h against the bezels at every cut).
    const int used0 = oc::g_learn.used;
    scenecut::g_cuts++;
    scenecut::g_cutsByReason[scenecut::kJump]++;
    Tally tjump;
    int jumpMoved = 1 << 30;  // the fewest display records at the true eye in one of the 3 frames after the cut
    for (int i = 0; i < 3; ++i) {
      RunFrame(n);
      const int m = trueDisplays(tjump);
      if (m < jumpMoved) jumpMoved = m;
    }
    const bool jumpKept = oc::g_learn.used == used0 && oc::g_os.lResetCut.load() == rc0 && AllTCommitted();
    // A gap cut (a load: no quad frame for > 1 s) clears the table like a mission event.
    scenecut::g_cuts++;
    scenecut::g_cutsByReason[scenecut::kGap]++;
    Tally tcut;
    RunFrame(n);
    const int cutFrame = trueDisplays(tcut);
    const bool cutCleared = oc::g_learn.used == 0 && oc::g_os.lResetCut.load() == rc0 + 1 &&
                            oc::g_os.natReset.load() == nr0 + 8;
    printf("     (reset: mid-frame %d, after %d, relearned %d, moved %d, reset frame %d, moved %d, jump cut: least moved "
           "%d, kept %d; gap cut frame %d)\n",
           midFrame, after, relearned, moved, resetFrame, moved2, jumpMoved, jumpKept, cutFrame);
    Check(jumpKept && jumpMoved > 20 && tjump.hudBad == 0 && tjump.eyeDisagree == 0,
          "ovl cpu: a jump / turn scene cut keeps the learned table (nothing freed): the displays stay at the true eye "
          "in the frames after it, no reset counted");
    printf("     (frame dump cpu counters of the relearned frame, view 2: decision %d, replays %d moved %d, records %d "
           "= T %d + B %d, failures %d; keys T %d B %d)\n",
           snap2.cpuWhy, snap2.cpuReplays, snap2.cpuMoved, snap2.cpuRecs, snap2.cpuRecT, snap2.cpuRecB, snap2.cpuFails,
           keysT, keysB);
    Check(dumpCsv && keysT > 0 && keysB > 0,
          "ovl cpu: the frame dump's per-view counters (ViewFrame cpu*) of a moved frame: decision moved, both "
          "replays moved, records at the true eye and at DCS's camera adding up to the records drawn, no failure; "
          "committed keys T and B");
    Check(ready && midFrame > 15 && ta.eyeDisagree == 0 && cleared && after == 0 && relearned && moved > 20 &&
              resetFrame == 0 && cleared2 && moved2 > 20 && cutFrame == 0 && cutCleared &&
              oc::g_os.lResetMission.load() == rm0 + 2 &&
              ta.hudBad + tb.hudBad + tc.hudBad + td.hudBad + te.hudBad + tcut.hudBad == 0,
          "ovl cpu resets (R32 r3): a mission event or a scene cut clears the learned keys at the render thread's "
          "frame end (one arriving mid-frame: that frame keeps its decision, latched; one between frames: the next "
          "frame moves nothing); no class learned before applies afterwards (displays at DCS's eye until "
          "relearned), then moved again");
  }
  {
    // One identity (pointers, c9, c8) shared by the HUD DED repeater (B) and the DED (T), the DED first: the
    // ordinal keeps them apart until the DED is missing for a frame; then the repeater takes the DED's committed T
    // key and is drawn at the true eye in that one frame; the key is frozen at B in it (a contradiction, a
    // failure), so the repeater is right from the next frame on (the DED then stays at DCS's camera).
    oc::g_learn.Reset();
    ClearFailHistory();
    const uint64_t c0 = oc::g_os.lContra.load();
    Tally t0, t1, t2;
    FrameOpts a;
    a.alias = true;
    a.toggles = false;
    a.shuffle = false;
    bool none[4] = {true, true, true, true};
    for (int i = 0; i < 40; ++i) {
      RunFrame(a);
      Analyse(t0, false, none, -1);
    }
    const bool learned = oc::g_learn.ClassOf(KeyOf(kERep, kMatHud, 1, 1, 0)) == oc::kClsT &&
                         oc::g_learn.ClassOf(KeyOf(kERep, kMatHud, 1, 1, 1)) == oc::kClsB;
    FrameOpts d = a;
    d.dropDed = true;
    RunFrame(d);
    Analyse(t1, false, none, -1);
    const bool frozen = oc::g_learn.FindC(KeyOf(kERep, kMatHud, 1, 1, 0)) &&
                        oc::g_learn.FindC(KeyOf(kERep, kMatHud, 1, 1, 0))->frozen;
    std::vector<int> st;
    int dedDcs = 0, dedTrue = 0;
    for (int i = 0; i < 20; ++i) {
      RunFrame(a);
      Analyse(t2, false, none, -1, &st);
      for (size_t k = 0; k < g_log.size(); ++k)
        if (g_log[k].kind == kDed) (st[k] == 0 ? dedDcs : dedTrue)++;
    }
    printf("     (shared identity: HUD frames off in the alias frame %llu, after %llu)\n",
           static_cast<unsigned long long>(t1.hudBadFrames), static_cast<unsigned long long>(t2.hudBadFrames));
    Check(learned && t0.hudBad == 0 && t1.hudBadFrames == 1 && frozen && oc::g_os.lContra.load() == c0 + 1 &&
              t2.hudBad == 0 && dedTrue == 0 && dedDcs > 0,
          "ovl cpu fail-safe: an identity shared by a HUD and a display record costs one frame when the order "
          "shifts: the key frozen at B in that frame (a contradiction, counted), the HUD right from the next frame");
    ClearFailHistory();
    ck::g_ovlInert = false;
  }

  // ---- The uninstall while the render thread is inside a replay: the replay ends with our filters. ----
  {
    oc::g_learn.Reset();
    ClearFailHistory();
    FrameOpts n;
    n.toggles = false;
    bool all[4] = {true, true, true, true};
    Tally tl;
    for (int i = 0; i < 40; ++i) {
      RunFrame(n);
      Analyse(tl, false, all, -1);
    }
    const bool ready = AllTCommitted();
    g_mid = MidUninstall{};
    g_mid.reached = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_mid.o.toggles = false;
    g_mid.o.shuffle = false;
    g_mid.o.tFirst = true;
    g_midView = 0;
    g_midAt = 2;  // two displays drawn at the true eye, the HUD still to come
    g_midCb = &MidCb;
    HANDLE th = CreateThread(nullptr, 0, &MidWorker, nullptr, 0, nullptr);
    const bool reached = th && WaitForSingleObject(g_mid.reached, 10000) == WAIT_OBJECT_0;
    if (reached) oc::Uninstall();
    if (th) {
      WaitForSingleObject(th, 20000);
      CloseHandle(th);
    }
    CloseHandle(g_mid.reached);
    g_midView = g_midAt = -1;
    g_midCb = nullptr;
    ptiming::g_topTid = mainTid;
    std::vector<int> st;
    Tally tu;
    bool none[4] = {false, false, false, false};
    Analyse(tu, true, none, -1, &st);
    int v0T = 0, v0TTrue = 0;
    for (size_t i = 0; i < g_log.size(); ++i)
      if (g_log[i].t == 0 && KindT(g_log[i].kind) && g_log[i].kind != kPanel) {
        ++v0T;
        v0TTrue += st[i] == 1;
      }
    const bool back = g_handlerVt[3] == reinterpret_cast<void*>(&ReplayImpl) &&
                      g_visVt[2] == reinterpret_cast<void*>(&VisCall) && g_compVt[2] == reinterpret_cast<void*>(&CompCall);
    Check(ready && reached && back && !oc::Installed() && tu.hudBad == 0 && v0T > 2 && v0TTrue == v0T &&
              oc::g_os.busyAtStop.load() == 0,
          "ovl cpu: uninstall while view 0's replay is running: the replay slot back first, the replay finishes with "
          "our filters (its displays all at the true eye, the HUD after them at DCS's camera), then the filters back");
    oc::InstallAt(&g_handlerVt[3], &g_visVt[2], &g_compVt[2], &g_curCtx, g_visVt, false);
  }

  // ---- R32 r3 (5): an uninstall whose replay outlives the 1 s drain keeps every hook in its slot, passing
  // through; a reinstall is refused while that replay runs (nothing of the render thread's state is reset from the
  // config thread under it); once it has ended the hooks are taken back and the render thread resets its state at
  // its next frame end (that frame moves nothing). ----
  {
    ClearFailHistory();
    ck::g_ovlInert = false;
    FrameOpts n;
    n.toggles = false;
    RunFrame(n);  // the reset the reinstall above asked for (done at this frame's end)
    for (int i = 0; i < 120 && !AllTCommitted(); ++i) RunFrame(n);
    for (int i = 0; i < 3; ++i) RunFrame(n);
    const bool ready = AllTCommitted() && !ck::g_ovlInert.load() && oc::g_resetReq.load() == 0;
    const uint64_t bs0 = oc::g_os.busyAtStop.load(), ib0 = oc::g_os.installBusy.load(), np0 = oc::g_os.natPass.load(),
                   rb0 = g_restoreBad, nr0 = oc::g_os.natReset.load();
    g_mid = MidUninstall{};
    g_mid.reached = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_holdRelease = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_mid.o.toggles = false;
    g_mid.o.shuffle = false;
    g_mid.o.tFirst = true;
    g_midView = 0;
    g_midAt = 2;  // two displays drawn at the true eye, then the replay is held past the uninstall's drain
    g_midCb = &HoldCb;
    HANDLE th = CreateThread(nullptr, 0, &MidWorker, nullptr, 0, nullptr);
    const bool reached = th && WaitForSingleObject(g_mid.reached, 10000) == WAIT_OBJECT_0;
    bool kept = false, refused = false;
    if (reached) {
      oc::Uninstall();  // the drain times out (1 s): the hooks stay, passing through
      kept = oc::g_state.load() == oc::kStatePass && g_handlerVt[3] == reinterpret_cast<void*>(&oc::ReplayHook) &&
             g_visVt[2] == reinterpret_cast<void*>(&oc::VisFilterHook) &&
             g_compVt[2] == reinterpret_cast<void*>(&oc::CompFilterHook) && oc::g_os.busyAtStop.load() == bs0 + 1 &&
             oc::t_rp.open && oc::g_replayOpen.load() == 1;
      uint8_t* heldCtx = oc::t_rp.ctx;
      const bool r = oc::InstallAt(&g_handlerVt[3], &g_visVt[2], &g_compVt[2], &g_curCtx, g_visVt, false);
      refused = !r && oc::g_os.installBusy.load() == ib0 + 1 && oc::t_rp.open && oc::t_rp.ctx == heldCtx &&
                oc::g_state.load() == oc::kStatePass;
    }
    SetEvent(g_holdRelease);
    if (th) {
      WaitForSingleObject(th, 20000);
      CloseHandle(th);
    }
    CloseHandle(g_mid.reached);
    CloseHandle(g_holdRelease);
    g_holdRelease = nullptr;
    g_midView = g_midAt = -1;
    g_midCb = nullptr;
    ptiming::g_topTid = mainTid;
    // The worker's frame: view 0's held replay finished with our filters (its displays at the true eye, the HUD at
    // DCS's camera); views 1-3 passed through (DCS's camera); the context restored after every replay.
    std::vector<int> st;
    Tally tw;
    bool none[4] = {false, false, false, false};
    Analyse(tw, false, none, -1, &st);
    int v0T = 0, v0TTrue = 0, otherTrue = 0;
    for (size_t i = 0; i < g_log.size(); ++i) {
      if (!(g_log[i].kind >= 0 && KindT(g_log[i].kind) && g_log[i].kind != kPanel)) continue;
      if (g_log[i].t == 0) {
        ++v0T;
        v0TTrue += st[i] == 1;
      } else {
        otherTrue += st[i] == 1;
      }
    }
    const bool workerOk = v0T > 2 && v0TTrue == v0T && otherTrue == 0 && tw.hudBad == 0 && g_restoreBad == rb0 &&
                          oc::g_os.natPass.load() > np0 && !oc::t_rp.open && oc::g_replayOpen.load() == 0;
    // Taken back; the next frame moves nothing and clears the learner at its end; then moved again once relearned.
    const bool back = oc::InstallAt(&g_handlerVt[3], &g_visVt[2], &g_compVt[2], &g_curCtx, g_visVt, false) &&
                      oc::Installed();
    Tally tr;
    RunFrame(n);
    Analyse(tr, false, none, -1, &st);
    int resetTrue = 0;
    for (size_t i = 0; i < g_log.size(); ++i) resetTrue += g_log[i].kind >= 0 && KindT(g_log[i].kind) && st[i] == 1;
    const bool resetOk = resetTrue == 0 && oc::g_os.natReset.load() == nr0 + 4 && oc::g_learn.used == 0;
    for (int i = 0; i < 120 && !AllTCommitted(); ++i) RunFrame(n);
    Tally tm;
    bool all[4] = {true, true, true, true};
    RunFrame(n);
    Analyse(tm, true, all, -1);
    printf("     (busy uninstall: kept %d refused %d view0 %d/%d others at the true eye %d back %d reset frame %d)\n",
           kept, refused, v0TTrue, v0T, otherTrue, back, resetTrue);
    Check(ready && reached && kept && refused && workerOk && back && resetOk && AllTCommitted() && tm.bezelBad == 0,
          "ovl cpu (R32 r3): an uninstall whose replay outlives the drain keeps the hooks installed, passing through; "
          "a reinstall is refused while that replay runs (its record untouched); the replay ends with our filters, "
          "the other views pass through; then reinstalled, the render thread resets at its frame end (that frame "
          "moves nothing), moved again once relearned");
  }

  // ---- R32 r4: the render point. DCS 2.9.30's HMD overlay replays pass an empty std::function (Visualizer
  // 0x15d6ca, mask -1) and the handler has no extra filter: no filter point, so before r4 the whole-frame scan
  // decided, nothing was ever tagged for the observer, no key was learned, and the displays stayed at DCS's camera.
  // CockpitBase's record loop calls RenderObject::render through its import slot with the record's RenderObject
  // (+0x78 = record + 0x40); hooked, it is the per-record point: learned from scratch through it, the displays on
  // their true-eye bezels, the HUD at DCS's camera, no A-B-A, views of one eye alike, no whole-frame scan. ----
  {
    oc::Uninstall();
    const bool inst =
        oc::InstallAt(&g_handlerVt[3], &g_visVt[2], &g_compVt[2], &g_curCtx, g_visVt, false, g_renderIat, nullptr);
    const bool hooked = inst && g_renderIat[0] == reinterpret_cast<void*>(&oc::RenderHook) &&
                        oc::g_renderOrig == &RenderImpl && g_handlerVt[3] == reinterpret_cast<void*>(&oc::ReplayHook);
    ClearFailHistory();
    waitBlock();  // the install's learner reset consumed at a frame end: every key unknown again
    const bool fresh = !AllTCommitted();
    H.handler.composite = nullptr;
    g_tracks.clear();
    g_restoreBad = 0;
    const uint64_t g0 = g_replays;
    const uint64_t vr0 = oc::g_os.viaRender.load(), rr0 = oc::g_os.renderRecs.load(), mv0 = oc::g_os.moved.load(),
                   rt0 = oc::g_os.recTrue.load(), sc0 = oc::g_os.scanned.load(), nf0 = oc::g_os.nullFilter.load(),
                   uf0 = oc::g_os.unknownFilter.load(), ob0 = oc::g_os.obsUploads.load();
    auto failSum = [] {
      return oc::g_os.failCam + oc::g_os.failSplit + oc::g_os.cpuMiss + oc::g_os.cpuWrongB + oc::g_os.ctxForeign +
             oc::g_os.recAlign + oc::g_os.lContra + oc::g_os.stopFail + oc::g_os.stopRule;
    };
    const uint64_t fc0 = failSum();
    Tally ta;
    int commitFrame = -1;
    for (int f = 0; f < 600; ++f) {
      g_fxMode = f < 200 ? kFxDirty : f < 400 ? kFxSkip : f < 500 ? kFxMap : kFxMulti;
      const bool committed = AllTCommitted();
      if (committed && commitFrame < 0) commitFrame = f;
      FrameOpts o;
      if (f < 450)
        o.nullFilter = true;
      else
        o.unknownFilter = true;
      RunFrame(o);
      bool moved[4] = {true, true, true, true};
      Analyse(ta, committed, moved, f);
    }
    int aba = 0;
    for (auto& kv : g_tracks) aba += CountAba(kv.second.st, oc::kLearnCommit);
    const uint64_t fails = failSum() - fc0;
    printf("     (render point: commit at frame %d, draws %llu, hud %llu bezel %llu neither %llu eye %llu aba %d, "
           "replays through it %llu of %llu, records %llu, moved %llu, true-eye records %llu, scans %llu, observer "
           "uploads %llu, failures and stops %llu)\n",
           commitFrame, (unsigned long long)ta.draws, (unsigned long long)ta.hudBad, (unsigned long long)ta.bezelBad,
           (unsigned long long)ta.neither, (unsigned long long)ta.eyeDisagree, aba,
           (unsigned long long)(oc::g_os.viaRender.load() - vr0), (unsigned long long)(g_replays - g0),
           (unsigned long long)(oc::g_os.renderRecs.load() - rr0), (unsigned long long)(oc::g_os.moved.load() - mv0),
           (unsigned long long)(oc::g_os.recTrue.load() - rt0), (unsigned long long)(oc::g_os.scanned.load() - sc0),
           (unsigned long long)(oc::g_os.obsUploads.load() - ob0), (unsigned long long)fails);
    Check(hooked && fresh, "ovl cpu render point (R32 r4): the import slot hooked before the replay slot (its "
                           "original kept); the install reset the learner");
    Check(ta.mismatch == 0 && ta.hudBad == 0 && ta.hudMaxPx < 0.05 && ta.labelBad == 0 && ta.guiBad == 0 &&
              ta.neither == 0,
          "ovl cpu render point: with an empty filter in every replay the collimated HUD stays at DCS's camera (zero "
          "disparity against infinity) in every frame, world labels and the GUI untouched, no third camera");
    Check(commitFrame >= oc::kLearnCommit && commitFrame <= 200 && ta.bezelBad == 0 && ta.bezelMaxPx < 0.05 &&
              aba == 0 && ta.eyeDisagree == 0,
          "ovl cpu render point: the cockpit displays learned T through the render point (observer tagged per "
          "record) and then drawn on their true-eye bezels in every frame, view and effect pattern (null and "
          "unknown filters); no A-B-A; views of one eye never differ");
    Check(oc::g_os.viaRender.load() - vr0 >= (g_replays - g0) * 9 / 10 && oc::g_os.renderRecs.load() > rr0 &&
              oc::g_os.moved.load() - mv0 >= (g_replays - g0) * 3 / 4 && oc::g_os.recTrue.load() > rt0 &&
              oc::g_os.scanned.load() == sc0 && oc::g_os.nullFilter.load() > nf0 &&
              oc::g_os.unknownFilter.load() > uf0 && oc::g_os.obsUploads.load() > ob0 && fails == 0 &&
              g_restoreBad == 0 && !ck::g_ovlInert.load(),
          "ovl cpu render point: the replays without a filter point decided through it (no whole-frame scan), "
          "moved, no failure or frame stop, currentContext bit-equal to DCS's after every replay");
    // R94: [Perception] SymbologyTrace in traced frames through the render point: each replay's records (c9.c8 runs)
    // and the records it drew, per HMD view and overlay pass; the blend state of each type's first drawn record;
    // target copies at the drawn records' type changes (SymbologyTraceSegments); what DCS draws is unchanged.
    {
      ID3D11BlendState* bs = nullptr;
      D3D11_BLEND_DESC bd{};
      D3D11_RENDER_TARGET_BLEND_DESC& rb = bd.RenderTarget[0];
      rb.BlendEnable = TRUE;
      rb.SrcBlend = D3D11_BLEND_SRC_ALPHA, rb.DestBlend = D3D11_BLEND_INV_SRC_ALPHA, rb.BlendOp = D3D11_BLEND_OP_ADD;
      rb.SrcBlendAlpha = D3D11_BLEND_ZERO, rb.DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
      rb.BlendOpAlpha = D3D11_BLEND_OP_ADD, rb.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
      H.dev->CreateBlendState(&bd, &bs);
      g_c->OMSetBlendState(bs, nullptr, 0xffffffffu);
      g_c->OMSetRenderTargets(1, &g_rtv, nullptr);
      const int keepViews = symtr::g_views, keepSegs = symtr::g_segs;
      ID3D11Device* const keepDev = fixdepth::g_s.dev;
      fixdepth::g_s.dev = H.dev;  // (the copies' staging textures)
      symtr::g_views = 15;
      symtr::g_segs = 2;
      Tally tt;
      bool rleOk = true, drawnOk = true, blendOk = true;
      int rLines = 0, rLdr = 0;
      for (int f = 0; f < 6; ++f) {
        FrameOpts o;
        o.nullFilter = true;
        symtr::g_text.clear();
        for (int v = 0; v < 4; ++v) symtr::g_segN[v] = 0, symtr::g_blendLogged[v] = 0;
        symtr::g_on = true;
        RunFrame(o);
        symtr::g_on = false;
        bool moved[4] = {true, true, true, true};
        Analyse(tt, true, moved, 600 + f);
        const std::string& tx = symtr::g_text;
        for (int t = 0; t < 4; ++t) {
          std::string recs, drawn;
          int l1 = -1, n1 = 0, l2 = -1, n2 = 0;
          std::set<int> types;
          for (const Rec& r : g_list[t]) {
            symtr::RleAdd(recs, l1, n1, r.elem < 0 ? -2 : r.c9 * 256 + r.c8);
            if (r.elem >= 0) symtr::RleAdd(drawn, l2, n2, r.c9 * 256 + r.c8), types.insert(r.c9);
          }
          symtr::RleAdd(recs, l1, n1, -1);
          symtr::RleAdd(drawn, l2, n2, -1);
          char h[96];
          snprintf(h, sizeof(h), "R %d H hmd %d dcsview %d recs %zu: ", t, t, t, g_list[t].size());
          rleOk = rleOk && tx.find(std::string(h) + recs + "\n") != std::string::npos;
          snprintf(h, sizeof(h), "D %d drawn ", t);
          drawnOk = drawnOk && tx.find(std::string(h) + drawn + "\n") != std::string::npos;
          for (int c9 : types) {
            snprintf(h, sizeof(h), "B %d c9 %d blend en 1 src 5 dst 6 op 1 srcA 1 dstA 6 opA 1 write 0xf", t, c9);
            blendOk = blendOk && tx.find(h) != std::string::npos;
          }
        }
        rLines += r94::CountLines(tx, "R ");
        for (int t = 0; t < 4; ++t) {
          char h[16];
          snprintf(h, sizeof(h), "R %d L ", t);
          rLdr += r94::CountLines(tx, h);
        }
      }
      size_t segs = 0;
      bool segTarget = true;
      for (const symtr::Cap& cp : symtr::g_caps) {
        segs += cp.tag.rfind("seg", 0) == 0;
        segTarget = segTarget && cp.w == 16 && cp.h == 16 && cp.fmt == DXGI_FORMAT_R8G8B8A8_UNORM;
      }
      printf("     (R94 trace through the render point: %d replay lines (%d LDR), %zu segment copies; draws %llu, hud %llu "
             "bezel %llu neither %llu)\n",
             rLines, rLdr, segs, (unsigned long long)tt.draws, (unsigned long long)tt.hudBad,
             (unsigned long long)tt.bezelBad, (unsigned long long)tt.neither);
      if (!blendOk) {
        const size_t at = symtr::g_text.find("B 0 ");
        printf("     (R94 blend line: %s)\n", at == std::string::npos ? "none" : symtr::g_text.substr(at, 200).c_str());
      }
      symtr::Reset();
      fixdepth::g_s.dev = keepDev;
      symtr::g_views = keepViews;
      symtr::g_segs = keepSegs;
      symtr::g_text.clear();
      g_c->OMSetBlendState(nullptr, nullptr, 0xffffffffu);
      ID3D11RenderTargetView* none = nullptr;
      g_c->OMSetRenderTargets(1, &none, nullptr);
      if (bs) bs->Release();
      Check(rLines == 6 * 8 && rLdr == 6 * 4 && rleOk && drawnOk,
            "R94 SymbologyTrace (ovl cpu render point): one line per replay with its HMD view, pass and DCS's record "
            "runs (c9.c8, state records S), and the drawn records' runs through the render point");
      Check(blendOk && segs == 6 * 4 * 2 && segTarget && symtr::g_caps.empty(),
            "R94 SymbologyTrace (ovl cpu render point): each type's first drawn record logs the bound blend state "
            "(SRC_ALPHA / INV_SRC_ALPHA, alpha ZERO / INV_SRC_ALPHA); SymbologyTraceSegments=2 copies the bound target "
            "at the first 2 type changes per view; Reset releases them");
      Check(tt.mismatch == 0 && tt.hudBad == 0 && tt.bezelBad == 0 && tt.neither == 0 && tt.labelBad == 0 &&
                tt.guiBad == 0 && tt.eyeDisagree == 0,
            "R94 SymbologyTrace (ovl cpu render point): the traced frames draw exactly what untraced ones draw (HUD at "
            "DCS's camera, displays on their true-eye bezels)");
    }

    // The context's VP in cPerView's layout (not DCS 2.9.30's row-major): the layout that is the view's camera is
    // taken per replay and ours written in it.
    g_ctxColMajor = true;
    const uint64_t cm0 = oc::g_os.ctxColMajor.load(), mv1 = oc::g_os.moved.load(), fc1 = failSum();
    Tally tc;
    g_restoreBad = 0;
    for (int f = 0; f < 20; ++f) {
      FrameOpts o;
      o.nullFilter = true;
      frameExpect(o, true, true, true, true, tc);
    }
    g_ctxColMajor = false;
    Check(tc.bezelBad == 0 && tc.hudBad == 0 && tc.neither == 0 && oc::g_os.ctxColMajor.load() > cm0 &&
              oc::g_os.moved.load() >= mv1 + 80 && failSum() == fc1 && g_restoreBad == 0,
          "ovl cpu (R32 r4): the context's VP read in the layout that is the view's camera (DCS 2.9.30: row-major; "
          "cPerView's layout accepted too), VP_true written back in it");
  }
  {
    // A RenderObject that does not name DCS's next record (its +0x78 one record further): record alignment lost,
    // DCS's camera put back before that record draws, the view's failure (the frame stopped); no third camera, the
    // HUD untouched. A render call outside any replay passes through.
    ClearFailHistory();
    const uint64_t ra0 = oc::g_os.recAlign.load(), sf0 = oc::g_os.stopFail.load(), ro0 = oc::g_os.renderOther.load(),
                   ri0 = g_renderImplCalls;
    g_badRoAt = 3;
    Tally t0, t1;
    FrameOpts o;
    o.nullFilter = true;
    RunFrame(o);
    bool none[4] = {false, false, false, false};
    std::vector<int> st;
    Analyse(t0, true, none, -1, &st);  // (view 0's first records may be at the true eye: only third cameras count)
    const bool consumed = g_badRoAt < 0;
    g_badRoAt = -1;
    FakeRo outside;
    reinterpret_cast<RenderFnT>(g_renderIat[0])(&outside, 0);
    const bool passed = oc::g_os.renderOther.load() == ro0 + 1 && g_renderImplCalls > ri0;
    ClearFailHistory();
    waitBlock();
    FrameOpts n;
    n.nullFilter = true;
    frameExpect(n, true, true, true, true, t1);
    Check(consumed && oc::g_os.recAlign.load() == ra0 + 1 && oc::g_os.stopFail.load() == sf0 + 1 &&
              t0.neither == 0 && t0.hudBad == 0 && passed && t1.bezelBad == 0 && t1.hudBad == 0 && t1.neither == 0,
          "ovl cpu render point: a RenderObject naming another record loses the alignment (counted, DCS's camera "
          "for the rest of the replay, the frame stopped, no third camera); a render call outside a replay passes "
          "through; moved again after the block");
  }
  {
    // The replay slot failing to hook puts the render slot back too; uninstall restores it; the rest of the suite
    // runs without the render point (as before R32 r4).
    oc::Uninstall();
    const bool back0 = g_renderIat[0] == reinterpret_cast<void*>(&RenderImpl);
    const uint64_t rb0 = oc::g_os.installRollback.load();
    oc::g_testFailSlot = 4;
    const bool r = oc::InstallAt(&g_handlerVt[3], &g_visVt[2], &g_compVt[2], &g_curCtx, g_visVt, false, g_renderIat,
                                 nullptr);
    oc::g_testFailSlot = 0;
    const bool back = g_handlerVt[3] == reinterpret_cast<void*>(&ReplayImpl) &&
                      g_visVt[2] == reinterpret_cast<void*>(&VisCall) &&
                      g_compVt[2] == reinterpret_cast<void*>(&CompCall) &&
                      g_renderIat[0] == reinterpret_cast<void*>(&RenderImpl) && oc::g_slotRender == nullptr;
    Check(back0 && !r && back && oc::g_os.installRollback.load() == rb0 + 1,
          "ovl cpu render point: uninstall puts the import slot back; the replay slot failing to hook rolls back the "
          "filter slots and the render slot");
    oc::InstallAt(&g_handlerVt[3], &g_visVt[2], &g_compVt[2], &g_curCtx, g_visVt, false);
    ClearFailHistory();
    waitBlock();
  }

  // ---- A hook that cannot be written: the ones already written are put back. ----
  {
    oc::Uninstall();
    const uint64_t rb0 = oc::g_os.installRollback.load();
    oc::g_testFailSlot = 3;
    const bool r = oc::InstallAt(&g_handlerVt[3], &g_visVt[2], &g_compVt[2], &g_curCtx, g_visVt, false);
    oc::g_testFailSlot = 0;
    const bool back = g_handlerVt[3] == reinterpret_cast<void*>(&ReplayImpl) &&
                      g_visVt[2] == reinterpret_cast<void*>(&VisCall) && g_compVt[2] == reinterpret_cast<void*>(&CompCall);
    const bool again = oc::InstallAt(&g_handlerVt[3], &g_visVt[2], &g_compVt[2], &g_curCtx, g_visVt, false);
    Check(!r && back && oc::g_os.installRollback.load() == rb0 + 1 && again && oc::Installed(),
          "ovl cpu: the replay slot failing to hook puts both filter slots back (rolled back, counted); the next "
          "install succeeds");
  }

  // ---- Off is bit-identical: the replica's draws with no module at all, and with the hooks in but the option 0. ----
  {
    auto run = [&](std::vector<std::array<float, 4>>& out) {
      ResetScene();
      for (int f = 0; f < 12; ++f) {
        g_fxMode = f % kFxModes;
        RunFrame(FrameOpts{});
        const auto g = ReadDraws();
        out.insert(out.end(), g.begin(), g.end());
      }
    };
    oc::Uninstall();
    const bool back = g_handlerVt[3] == reinterpret_cast<void*>(&ReplayImpl) &&
                      g_visVt[2] == reinterpret_cast<void*>(&VisCall) && g_compVt[2] == reinterpret_cast<void*>(&CompCall);
    ck::Configure(0, false, true, 0, 0, false);
    ck::g_on = false;
    std::vector<std::array<float, 4>> a, b;
    run(a);
    oc::InstallAt(&g_handlerVt[3], &g_visVt[2], &g_compVt[2], &g_curCtx, g_visVt, false);
    const uint64_t moved0 = oc::g_os.moved.load();
    run(b);
    Check(back && a.size() == b.size() && !a.empty() && !memcmp(a.data(), b.data(), a.size() * 16) &&
              oc::g_os.moved.load() == moved0,
          "ovl cpu: uninstall puts DCS's slots back; CockpitTrueIpd=0 with the hooks in draws bit-identically to no "
          "module at all");
    oc::Uninstall();
  }

  // ---- R32 r3 (9): far depth with CockpitTrueIpd=0 behaves as before R32. Its in-pass arm (ArmOm) takes the
  // toggle points it took before (Flush, Map, GetData; not R32's Unmap), and an arm found "normal, not ours" at a
  // toggle point (d3d11's normal entry over one of our draw hooks, the table not showing the toggle) is left as
  // before R32: not repaired there. Cockpit ipd's own arm takes Unmap and is repaired there. ----
  {
    ID3D11DeviceContext* c = fixdepth::g_s.ctx;
    void** vt = *reinterpret_cast<void***>(c);
    const int mapSlot = DcsQv_ContextSlot("Map");
    void* const mapBefore = vt[mapSlot];
    ID3D11Buffer* dyn = g_fxDyn[0].cb;
    auto mapUnmap = [&]() {
      D3D11_MAPPED_SUBRESOURCE m{};
      if (SUCCEEDED(c->Map(dyn, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) c->Unmap(dyn, 0);
    };
    const bool ckOff = !ck::g_on.load() && ck::g_cfgMode.load() == 0;
    fixdepth::NormalizeTable(vt);  // (the replica's toggles may have left d3d11's after-flush entries)
    const bool normal = !fixdepth::UsToggled(vt);
    // Far depth's arm.
    const bool armed = fixdepth::ArmOm();
    const bool basic = armed && fixdepth::g_tpArmed[fixdepth::kTpFlush] && fixdepth::g_tpArmed[fixdepth::kTpMap] &&
                       fixdepth::g_tpArmed[fixdepth::kTpGetData] && !fixdepth::g_tpArmed[fixdepth::kTpUnmap] &&
                       !fixdepth::CkArm();
    fixdepth::ArmDraws();
    const bool draws = fixdepth::g_drawArmed;
    const int ds = fixdepth::g_dSlot[0];
    const uint64_t nr0 = ck::g_st.tpNormalRepair.load();
    bool farAsBefore = false;
    if (draws && ds >= 0) {
      fixdepth::FdWriteSlot(&vt[ds], fixdepth::g_dOrig[0]);  // what the first call through a toggled entry does
      mapUnmap();
      farAsBefore = ck::g_st.tpNormalRepair.load() == nr0 && vt[ds] == fixdepth::g_dOrig[0];
      fixdepth::FdWriteSlot(&vt[ds], fixdepth::DrawHookOf(0));
    }
    fixdepth::DisarmOm();
    // Cockpit ipd's own overlay arm (its d3d11 path takes the draw entries too).
    const bool ckArmed = fixdepth::ArmUsOnly(true);
    const bool ckUnmap = ckArmed && fixdepth::g_tpArmed[fixdepth::kTpUnmap] && fixdepth::CkArm();
    bool ckRepaired = false;
    if (ckArmed && fixdepth::g_drawArmed && ds >= 0) {
      fixdepth::FdWriteSlot(&vt[ds], fixdepth::g_dOrig[0]);
      mapUnmap();
      ckRepaired = ck::g_st.tpNormalRepair.load() == nr0 + 1 && vt[ds] == fixdepth::DrawHookOf(0);
    }
    fixdepth::DisarmUsOnly();
    const bool clean = !fixdepth::g_armTable && vt[mapSlot] == mapBefore && !fixdepth::g_tpArmed[fixdepth::kTpUnmap];
    Check(ckOff && normal && basic && draws && farAsBefore && ckUnmap && ckRepaired && clean,
          "far depth with CockpitTrueIpd=0 (R32 r3): its arm takes the toggle points of before R32 (Flush, Map, "
          "GetData; no Unmap) and an arm found \"normal, not ours\" at a toggle point is left as before R32; cockpit "
          "ipd's own arm takes Unmap and is repaired there");
  }

  // ---- R32 r3 (8): an Unmap of DCS's context from another thread while an overlay pass is open on the render
  // thread is neither observed nor consumes the pass's mapping; the render thread's own Unmap is. ----
  {
    ID3D11DeviceContext* c = fixdepth::g_s.ctx;
    void** vt = *reinterpret_cast<void***>(c);
    const int us = DcsQv_ContextSlot("Unmap");
    void* const keepOrig = fixdepth::g_tpOrig[fixdepth::kTpUnmap];
    fixdepth::g_tpOrig[fixdepth::kTpUnmap] = vt[us];  // (not armed here: the hook called directly)
    ID3D11Buffer* dyn = g_fxDyn[1].cb;
    const int keepCls = ck::t_cls;
    ck::t_cls = ck::kClsOvl;
    auto mapFor = [&]() {
      D3D11_MAPPED_SUBRESOURCE m{};
      const bool ok = SUCCEEDED(c->Map(dyn, 0, D3D11_MAP_WRITE_DISCARD, 0, &m));
      ck::t_mapRes = dyn;
      ck::t_mapSub = 0;
      ck::t_mapData = m.pData;
      ck::t_mapLen = 256;
      return ok;
    };
    const bool m1 = mapFor();
    std::thread other([&] { fixdepth::UnmapHook(c, dyn, 0); });
    other.join();
    const bool otherKept = ck::t_mapRes == dyn && ck::t_mapData != nullptr;
    const bool m2 = mapFor();
    fixdepth::UnmapHook(c, dyn, 0);
    const bool ownTaken = ck::t_mapRes == nullptr && ck::t_mapData == nullptr;
    ck::t_cls = keepCls;
    ck::t_mapRes = nullptr;
    ck::t_mapData = nullptr;
    fixdepth::g_tpOrig[fixdepth::kTpUnmap] = keepOrig;
    Check(m1 && m2 && otherKept && ownTaken,
          "ovl cpu (R32 r3): another thread's Unmap of DCS's context during an open overlay pass is not observed and "
          "leaves the pass's mapping alone; the render thread's own Unmap is observed");
  }

  // Teardown.
  ck::g_on = false;
  ck::ResetRender();
  fixdepth::g_s.ctx = oldCtx;
  fixdepth::g_s.ctx1 = oldCtx1;
  fixdepth::g_s.hmdOrd = 0;
  g_c->ClearState();
  if (fixdepth::g_tinyCb && fixdepth::g_tinyDev == H.dev) {  // the table normalizer's private buffer on our device
    fixdepth::g_tinyCb->Release();
    fixdepth::g_tinyCb = nullptr;
    fixdepth::g_tinyDev = nullptr;
  }
  for (int i = 0; i < 16; ++i) Rel(g_fx[i].cb), Rel(g_fxDyn[i].cb);
  Rel(g_so), Rel(g_soStage), Rel(g_rtv);
  Rel(H.rtTex), Rel(H.b7), Rel(H.vs), Rel(H.gs);
  Rel(g_c), Rel(H.imm);
  const ULONG last = H.dev->Release();
  Check(last == 0, "ovl cpu replica: no D3D reference leaked (device released to 0)");
  g_recArr.clear();
  g_recArr.shrink_to_fit();
  g_h = nullptr;
}

void Run() {
  printf("-- ovl_cpu.h (R32) --\n");
  TestPure();
  TestSignatures();
  TestReplica();
}

}  // namespace ocpt
