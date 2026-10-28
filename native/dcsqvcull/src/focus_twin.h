// R123 (R120b H2): the "focus twin". [Model] FocusTwin = 0 (default: nothing installed, nothing changes) | 1.
// Design, measurements, live plan: docs/research/R123_focus_twin.md.
//
// What it does, per frame and eye (HMD views render periphery L, periphery R, focus L, focus R; R120b 1.1):
//  - Periphery main G-buffer pass of the eye: while DCS draws the view's terrain (TerrainObject vt[1], Visualizer
//    0x103cf0, inside a GBufferPass execution), DCS's immediate context runs on a COPY of its own vtable (the
//    context's table pointer swapped for the call, every entry forwarding to the live table, a few dozen entries
//    observing first). The terrain patch items (edterrainGraphics41 MetaMaterial vt[8] 0xb99e0: binders, FX Apply,
//    DrawIndexed) are TEED: each DrawIndexed becomes a record (its arguments, the state deltas since the previous
//    record, the bytes of every constant buffer the item uploaded), each item a descriptor (its RenderItem, material,
//    MetaShaderRenderItem and geometry streams read from DCS memory). split_filter.h's site-filtered setters are read
//    from its shadow (they bypass every table).
//  - A worker (deferred context) records the records into command lists (chunks of [Suite] FocusTwinChunk records):
//    the focus targets of the previous frame (checked at use), the four per-view constant buffers (b5, b7 cPerView,
//    b10, b11 TerrainContext) bound to OUR copies, the uploaded constant buffers from a ring with offsets, and every
//    draw as DrawIndexedInstancedIndirect from an argument buffer (instance count 0 or 1, set at execution).
//  - Focus main G-buffer pass of the same eye: at its first patch item the focus TerrainContext DCS uploaded is
//    checked against the one PREDICTED at the periphery pass (this frame's collect-time focus camera: eye tracking
//    follows; the jitter step equal in pixels in both views, R53): every shared row byte-equal, the projection rows
//    within [Suite] FocusTwinPredTol. Any miss: the whole focus pass is stock (counted by reason). Else DCS's four
//    per-view buffers are copied into ours on the GPU (DCS's own focus bytes: exact by construction) and every focus
//    patch item whose descriptor matches a twinnable periphery item, in the periphery's order, is NOT run: its
//    records are executed from the command lists instead (lazily, right before the next other work, so the stock
//    order holds). Any other item (no match: a per-view item such as the R120b-census patches whose vertex stream 0
//    differs per view; not twinnable; out of order) is DCS's own, in place.
//  - Audits: the focus runs stock with the same tee and every matched item's records are compared with the
//    periphery's (state hash per draw: every bound object, the uploaded bytes, the draw arguments; per-view slots
//    excepted). The twin draws only after [Suite] FocusTwinTrust passed audits; then one audit every
//    [Suite] FocusTwinAuditEvery focus passes. A failed audit latches the feature off for the session.
//    [Suite] FocusTwinShadow=1: audits only (measure, never skip).
//  - Island rule ([Suite] FocusTwinIsland, default 16): an item is replaced only if, in the previous frame, it was in
//    a run of at least that many consecutive matched, twinnable items (one command-list execution ~10 us must buy
//    enough DCS item work, ~0.5-1 us each).
//
// R125 ([Model] FocusTwinInstancer = 1, needs FocusTwin = 1; docs/research/R125_focus_twin_instancer.md): the same for
// the GPU-driven trees and buildings, edterrainGraphics41 MetaMaterial_Instancer2 vt[8] 0xb4180. Its draws are
// DrawIndexedInstancedIndirect from InstanceManager2's fixed argument buffer (IM2 + 0x370) at a per (type, sub-draw)
// offset, reading IM2's fixed instance buffers: every view's GPU culling chain (LightsBuffersPass) rewrites those
// same buffers before its G-buffer pass, so a replay at the focus pass's own position draws the FOCUS's culled
// instances, exactly as DCS would (not the periphery's). Per item DCS draws sub-draw 0 / 1 only where the view's CPU
// bit (IM2 vt[30]) is set; the twin records the periphery's sub-draws, reads the focus bits at the focus item and
// enables only those (a focus sub-draw the periphery did not record: DCS's item). The enable is applied on the GPU:
// each command list holding instancer records starts with a small compute pass that copies DCS's argument words of
// its enabled records into the list's own argument buffer (zero for the others). The descriptor adds what vt[8] reads
// besides the MetaShaderRenderItem: the view index and technique argument of the context's instancer view, the item's
// type for that view, the two IM2 type entries (bit index, per-draw int, argument offset; per view for some types),
// IM2's buffer members and the sub-draw-1 buffer.
// [Suite] FocusTwinPatches = 0: only the instancer items are teed / replaced (patches stay DCS's).
//
// Safety: every hook body runs under SEH; a fault latches the feature off for the session (pass-through). The kill
// switch / key 0 / payload stop / device loss: hooks out, workers stopped. F10 / map / loading: no fresh
// collect-time camera (no culling ran) or gb_rec's graph-wide sort guard (R73): nothing is teed or skipped. MFD
// sensor views: identified by their TerrainContext (centred: "other"), never paired with a quad view.
//
// Included once from main.cpp (features.inc), last: uses gbpass (gb_batch.h), sfilt (split_filter.h), gbrec
// (gb_rec.h), defrec (deferred_rec.h), allocslab, shadowtex::CodeIs, perfmon::DcsDevice, HookSlot, Log,
// g_collectTap / ViewInfo (views.inc).
#pragma once

#include "focus_twin_math.h"

extern "C" int DcsQv_ContextSlotCount(void);

namespace ftwin {

// ---------------------------------------------------------------------------
// Keys and global state
// ---------------------------------------------------------------------------
std::atomic<int> g_key{0};                // [Model] FocusTwin
std::atomic<bool> g_shadowKey{false};     // [Suite] FocusTwinShadow
std::atomic<int> g_auditEvery{64};        // [Suite] FocusTwinAuditEvery (focus passes per eye between audits)
std::atomic<int> g_trustNeeded{4};        // [Suite] FocusTwinTrust (passed audits per eye before the twin draws)
std::atomic<int> g_island{16};            // [Suite] FocusTwinIsland
std::atomic<int> g_chunk{64};             // [Suite] FocusTwinChunk (records per command list)
std::atomic<double> g_predTol{5e-5};      // [Suite] FocusTwinPredTol (relative)
std::atomic<int> g_verifyEvery{0};        // [Suite] FocusTwinVerify: one same-frame compare per N focus passes per eye
std::atomic<int> g_instKey{0};            // R125 [Model] FocusTwinInstancer (read at install)
std::atomic<bool> g_patchesKey{true};     // R125 [Suite] FocusTwinPatches (0: instancer items only)

std::atomic<int> g_state{0};      // 0 not installed, 1 installed, -1 unavailable on this build (not retried)
std::atomic<bool> g_active{false};
std::atomic<bool> g_faulted{false};
std::atomic<int> g_inside{0};     // hook bodies in flight
const char* g_faultWhere = "";
DWORD g_faultCode = 0;

// Production RVAs (DCS 2.9.30; edterrainGraphics41 0x6aae1e36, Visualizer 0x6ac12454), RTTI- and hash-checked.
// R147: edterrainGraphics41 is byte-identical in DCS 2.9.31 (every check below passes); Visualizer has a 2.9.31 entry.
constexpr uint32_t kMetaMatVtbl = 0x151850, kMetaMatRender = 0xb99e0;  // MetaMaterial vt[8]
constexpr int kMatSlot = 8, kTerrSlot = 1;
struct CodeRange {
  uint32_t begin, end;
  uint64_t hash;
};
constexpr CodeRange kEtgCode[] = {
    {0xb99e0, 0xb9bdb, 0xf830d5293a6f4423ull},   // MetaMaterial::render (vt[8]): pass index, technique, MetaShader::render
    {0xb9bdb, 0xb9c2a, 0xa62fa0c6fa21247cull},
    {0x12cd7e, 0x12cd8a, 0xc1c9fbcd909d824eull},  // TerrainRenderable slot 5: item = [entry + 0x58]
    {0x12cf15, 0x12cf3f, 0x1117740ad883386cull},  // ... mat = [item + 8]; vt[6](mat, item, ctx, x), vt[8](same)
};
// TerrainObject vt[1] per analysed Visualizer build: vtable, render, render's prologue (self, ctx). DCS 2.9.31
// (R147): the same prologue at .text +0x2c790; only its security-cookie displacement differs (hence the hash).
struct VisTerr {
  uint32_t vtbl, render;
  CodeRange code;
};
constexpr VisTerr kVisTerr[] = {
    {0x20ec08, 0x103cf0, {0x103cf0, 0x103d39, 0x0163045434e4ef39ull}},  // DCS 2.9.30 (0x6ac12454)
    {0x23d318, 0x130480, {0x130480, 0x1304c9, 0xae688f8a3a9ab874ull}},  // DCS 2.9.31 (0x6ad87745)
};

// R125: MetaMaterial_Instancer2 (RTTI `.?AVMetaMaterial_Instancer2@edtg41@@`) vt[8] and what it reads.
constexpr uint32_t kInstVtbl = 0x1513f8, kInstRender = 0xb4180;  // MetaMaterial_Instancer2 vt[8]
constexpr uint32_t kIm2Vtbl = 0x14f4f8;                          // InstanceManager2 (RTTI-checked)
constexpr uint32_t kTerrGlobal = 0x1a7b48;                       // 0x1389d0 returns this variable
constexpr int kIm2Slots[] = {30, 32, 33, 37, 38, 39, 42, 44};
constexpr uint32_t kIm2SlotRva[] = {0xa1f30, 0xa09c0, 0xa0980, 0xa0380, 0xa01e0, 0xa07f0, 0xa02c0, 0xa01d0};
constexpr CodeRange kInstCode[] = {
    {0xb4180, 0xb4380, 0x55cee3997b53b23eull},   // MetaMaterial_Instancer2::render (vt[8]), first part
    {0xb4380, 0xb4552, 0xe861e1ca0e57eac3ull},   // ... the sub-draw loop (bit, technique, ints, drawIndirect)
    {0xb40d0, 0xb4180, 0x530a14764f32fc25ull},   // vt[6]: the slot-5 gate (any sub-draw bit of the view)
    {0xa1f30, 0xa1f79, 0x26e1536e92d076a6ull},   // IM2 vt[30] hasInstances(type, sub): bit [[IM2+0x210]][e.+0]
    {0xa09c0, 0xa09e6, 0x44a504bf53107bbaull},   // IM2 vt[32]: the per-draw int, e.+8
    {0xa0980, 0xa09bc, 0x6020fd9be833a346ull},   // IM2 vt[33]: the argument buffer IM2+0x370, offset e.+0xc
    {0xa0380, 0xa0388, 0x5c641548dac2e70cull},   // IM2 vt[37] -> +0x2f0
    {0xa01e0, 0xa01e8, 0x0a8201c65513a51dull},   // IM2 vt[38] -> +0x320
    {0xa07f0, 0xa07f8, 0x3f93cf1e801cfeedull},   // IM2 vt[39] -> +0x330
    {0xa02c0, 0xa02c8, 0x74a59c76ab2658bdull},   // IM2 vt[42] -> +0x340
    {0xa01d0, 0xa01d8, 0xdec7b72701367fddull},   // IM2 vt[44] -> +0x360
    {0x1389d0, 0x1389d8, 0x5e38a3cfdef1ee08ull}, // the terrain global's getter
};
constexpr uint32_t kIm2Members[] = {0x2f0, 0x320, 0x330, 0x340, 0x360, 0x370};  // buffer objects the item binds

using MatFn = uint64_t(__fastcall*)(void* mat, void* item, void* ctx, void* x);
using TerrFn = uint64_t(__fastcall*)(void* self, void* ctx);
void** g_matSlot = nullptr;
void** g_terrSlot = nullptr;
MatFn g_matOrig = nullptr;
TerrFn g_terrOrig = nullptr;
void** g_instSlot = nullptr;        // R125: MetaMaterial_Instancer2 vt[8] (null: the instancer part not installed)
MatFn g_instOrig = nullptr;
void** g_instGlobal = nullptr;      // DCS's terrain global (edterrainGraphics41 + kTerrGlobal; tests: their own)
void* g_im2Vtbl = nullptr;          // InstanceManager2's vtable (null: not checked, tests)
bool g_instInstalled = false;
bool g_instRefused = false;         // the instancer part was asked for but not installed (build, gate)

thread_local int t_gbDepth = 0;  // inside gbpass::Hook (a GBufferPass execution)
DWORD g_renderTid = 0;

// ---------------------------------------------------------------------------
// Collect-time cameras (views.inc's tap; the render thread, before the render graph)
// ---------------------------------------------------------------------------
CiFrame g_ci[2];
std::atomic<int> g_ciCur{-1};
std::atomic<uint64_t> g_ciSeq{0};
inline CiFrame CurrentCi() {
  const int i = g_ciCur.load(std::memory_order_acquire);
  return i < 0 ? CiFrame() : g_ci[i];
}
void CollectTap(uint32_t count, const uint8_t* infos, const std::vector<ViewInfo>& views) {
  if (!g_active.load(std::memory_order_relaxed)) return;
  __try {
    const int next = g_ciCur.load(std::memory_order_relaxed) == 0 ? 1 : 0;
    CiFrame& f = g_ci[next];
    f.pairs = 0;
    for (size_t i = 0; i < views.size() && f.pairs < 2; ++i) {
      const ViewInfo& v = views[i];
      if (v.role != 1 || v.partner < 0 || static_cast<size_t>(v.partner) >= views.size()) continue;
      const ViewInfo& fo = views[static_cast<size_t>(v.partner)];
      if (v.index < 0 || fo.index < 0 || static_cast<uint32_t>(v.index) >= count ||
          static_cast<uint32_t>(fo.index) >= count)
        continue;
      memcpy(f.p[f.pairs], infos + static_cast<size_t>(v.index) * kCollectionInfoStride + kCiViewProj, 64);
      memcpy(f.f[f.pairs], infos + static_cast<size_t>(fo.index) * kCollectionInfoStride + kCiViewProj, 64);
      ++f.pairs;
    }
    if (f.pairs < 1) return;
    f.seq = g_ciSeq.fetch_add(1, std::memory_order_relaxed) + 1;
    g_ciCur.store(next, std::memory_order_release);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }
}
// Tests: publish a frame's cameras directly.
void PublishCi(const CiFrame& in) {
  const int next = g_ciCur.load(std::memory_order_relaxed) == 0 ? 1 : 0;
  g_ci[next] = in;
  g_ci[next].seq = g_ciSeq.fetch_add(1, std::memory_order_relaxed) + 1;
  g_ciCur.store(next, std::memory_order_release);
}

// ---------------------------------------------------------------------------
// Tracked pipeline state, dirty ranges, state hash
// ---------------------------------------------------------------------------
constexpr UINT kCb = 14, kSrv = 128, kSmp = 16, kVb = 16;
struct St {
  void* vs;
  void* ps;
  void* il;
  UINT topo;
  void* gs;
  void* hs;
  void* ds;
  ID3D11Buffer* vb[kVb];
  UINT vbStride[kVb];
  UINT vbOff[kVb];
  ID3D11Buffer* ib;
  UINT ibFmt;
  UINT ibOff;
  ID3D11Buffer* cb[2][kCb];
  UINT cbFirst[2][kCb];
  UINT cbNum[2][kCb];
  ID3D11ShaderResourceView* srv[2][kSrv];
  ID3D11SamplerState* smp[2][kSmp];
  void* rs;
  void* dss;
  UINT sref;
  void* bs;
  float bf[4];
  UINT smask;
};
inline bool IsPerViewSlot(UINT k) { return k == 5 || k == 7 || k == 10 || k == 11; }
inline int PerViewIndex(UINT k) { return k == 5 ? 0 : k == 7 ? 1 : k == 10 ? 2 : k == 11 ? 3 : -1; }

enum Grp : uint32_t {
  gVS = 1u << 0, gPS = 1u << 1, gIL = 1u << 2, gTopo = 1u << 3, gIB = 1u << 4, gRS = 1u << 5, gDSS = 1u << 6,
  gBS = 1u << 7, gStages = 1u << 8
};
struct Range {
  uint16_t lo = 0xffff, hi = 0;
  void Add(UINT a, UINT b) {
    if (a < lo) lo = static_cast<uint16_t>(a);
    if (b > hi) hi = static_cast<uint16_t>(b);
  }
  bool Any() const { return lo < hi; }
  void Clear() { lo = 0xffff, hi = 0; }
};
struct Dirty {
  uint32_t g = 0;
  Range vb, cb[2], srv[2], smp[2];
  void Clear() {
    g = 0;
    vb.Clear();
    for (int s = 0; s < 2; ++s) cb[s].Clear(), srv[s].Clear(), smp[s].Clear();
  }
  void All() {
    g = gVS | gPS | gIL | gTopo | gIB | gRS | gDSS | gBS | gStages;
    vb.Add(0, kVb);
    for (int s = 0; s < 2; ++s) cb[s].Add(0, kCb), srv[s].Add(0, kSrv), smp[s].Add(0, kSmp);
  }
};
// Slot ids for the hash (unique per field and slot).
enum HSlot : uint32_t { hVS = 1, hPS, hIL, hTopo, hGS, hHS, hDS, hIB, hRS, hDSS, hBS, hVB = 0x100, hCB = 0x200, hSRV = 0x400, hSMP = 0x800 };
inline uint64_t HS(uint32_t id, uint64_t a, uint64_t b = 0) {
  uint64_t x = (static_cast<uint64_t>(id) << 32) ^ a ^ (b * 0x9e3779b97f4a7c15ull);
  x ^= x >> 30, x *= 0xbf58476d1ce4e5b9ull, x ^= x >> 27, x *= 0x94d049bb133111ebull, x ^= x >> 31;
  return x;
}
inline uint64_t U(const void* p) { return reinterpret_cast<uint64_t>(p); }

struct Shadow {
  St st;
  Dirty dirty;
  uint64_t h = 0;  // XOR of HS over every tracked field except the per-view constant-buffer slots
  void SetVS(void* v) { if (st.vs != v) h ^= HS(hVS, U(st.vs)) ^ HS(hVS, U(v)), st.vs = v, dirty.g |= gVS; }
  void SetPS(void* v) { if (st.ps != v) h ^= HS(hPS, U(st.ps)) ^ HS(hPS, U(v)), st.ps = v, dirty.g |= gPS; }
  void SetIL(void* v) { if (st.il != v) h ^= HS(hIL, U(st.il)) ^ HS(hIL, U(v)), st.il = v, dirty.g |= gIL; }
  void SetTopo(UINT v) { if (st.topo != v) h ^= HS(hTopo, st.topo) ^ HS(hTopo, v), st.topo = v, dirty.g |= gTopo; }
  void SetStage(int which, void* v) {
    void** f = which == 0 ? &st.gs : which == 1 ? &st.hs : &st.ds;
    const uint32_t id = which == 0 ? hGS : which == 1 ? hHS : hDS;
    if (*f != v) h ^= HS(id, U(*f)) ^ HS(id, U(v)), *f = v, dirty.g |= gStages;
  }
  void SetIB(ID3D11Buffer* b, UINT fmt, UINT off) {
    if (st.ib == b && st.ibFmt == fmt && st.ibOff == off) return;
    h ^= HS(hIB, U(st.ib), (static_cast<uint64_t>(st.ibFmt) << 32) | st.ibOff) ^ HS(hIB, U(b), (static_cast<uint64_t>(fmt) << 32) | off);
    st.ib = b, st.ibFmt = fmt, st.ibOff = off, dirty.g |= gIB;
  }
  void SetVB(UINT i, ID3D11Buffer* b, UINT stride, UINT off) {
    if (i >= kVb) return;
    if (st.vb[i] == b && st.vbStride[i] == stride && st.vbOff[i] == off) return;
    h ^= HS(hVB + i, U(st.vb[i]), (static_cast<uint64_t>(st.vbStride[i]) << 32) | st.vbOff[i]) ^
         HS(hVB + i, U(b), (static_cast<uint64_t>(stride) << 32) | off);
    st.vb[i] = b, st.vbStride[i] = stride, st.vbOff[i] = off;
    dirty.vb.Add(i, i + 1);
  }
  void SetCB(int s, UINT i, ID3D11Buffer* b, UINT first, UINT num) {
    if (i >= kCb) return;
    if (st.cb[s][i] == b && st.cbFirst[s][i] == first && st.cbNum[s][i] == num) return;
    if (!IsPerViewSlot(i)) {
      const uint32_t id = hCB + static_cast<uint32_t>(s) * 0x20 + i;
      h ^= HS(id, U(st.cb[s][i]), (static_cast<uint64_t>(st.cbFirst[s][i]) << 32) | st.cbNum[s][i]) ^
           HS(id, U(b), (static_cast<uint64_t>(first) << 32) | num);
    }
    st.cb[s][i] = b, st.cbFirst[s][i] = first, st.cbNum[s][i] = num;
    dirty.cb[s].Add(i, i + 1);
  }
  void SetSRV(int s, UINT i, ID3D11ShaderResourceView* v) {
    if (i >= kSrv || st.srv[s][i] == v) return;
    const uint32_t id = hSRV + static_cast<uint32_t>(s) * 0x100 + i;
    h ^= HS(id, U(st.srv[s][i])) ^ HS(id, U(v));
    st.srv[s][i] = v;
    dirty.srv[s].Add(i, i + 1);
  }
  void SetSMP(int s, UINT i, ID3D11SamplerState* v) {
    if (i >= kSmp || st.smp[s][i] == v) return;
    const uint32_t id = hSMP + static_cast<uint32_t>(s) * 0x20 + i;
    h ^= HS(id, U(st.smp[s][i])) ^ HS(id, U(v));
    st.smp[s][i] = v;
    dirty.smp[s].Add(i, i + 1);
  }
  void SetRS(void* v) { if (st.rs != v) h ^= HS(hRS, U(st.rs)) ^ HS(hRS, U(v)), st.rs = v, dirty.g |= gRS; }
  void SetDSS(void* v, UINT ref) {
    if (st.dss == v && st.sref == ref) return;
    h ^= HS(hDSS, U(st.dss), st.sref) ^ HS(hDSS, U(v), ref);
    st.dss = v, st.sref = ref, dirty.g |= gDSS;
  }
  void SetBS(void* v, const FLOAT* f, UINT mask) {
    float nf[4] = {1, 1, 1, 1};
    if (f) memcpy(nf, f, sizeof(nf));
    if (st.bs == v && memcmp(st.bf, nf, sizeof(nf)) == 0 && st.smask == mask) return;
    uint64_t a = 0, b = 0;
    memcpy(&a, st.bf, 8), memcpy(&b, st.bf + 2, 8);
    h ^= HS(hBS, U(st.bs), a ^ (b * 31) ^ st.smask);
    memcpy(&a, nf, 8), memcpy(&b, nf + 2, 8);
    h ^= HS(hBS, U(v), a ^ (b * 31) ^ mask);
    st.bs = v, memcpy(st.bf, nf, sizeof(nf)), st.smask = mask, dirty.g |= gBS;
  }
  // The hash of a whole state (baseline).
  static uint64_t HashAll(const St& t) {
    uint64_t x = HS(hVS, U(t.vs)) ^ HS(hPS, U(t.ps)) ^ HS(hIL, U(t.il)) ^ HS(hTopo, t.topo) ^ HS(hGS, U(t.gs)) ^
                 HS(hHS, U(t.hs)) ^ HS(hDS, U(t.ds)) ^
                 HS(hIB, U(t.ib), (static_cast<uint64_t>(t.ibFmt) << 32) | t.ibOff) ^ HS(hRS, U(t.rs)) ^
                 HS(hDSS, U(t.dss), t.sref);
    uint64_t a = 0, b = 0;
    memcpy(&a, t.bf, 8), memcpy(&b, t.bf + 2, 8);
    x ^= HS(hBS, U(t.bs), a ^ (b * 31) ^ t.smask);
    for (UINT i = 0; i < kVb; ++i)
      x ^= HS(hVB + i, U(t.vb[i]), (static_cast<uint64_t>(t.vbStride[i]) << 32) | t.vbOff[i]);
    for (int s = 0; s < 2; ++s) {
      for (UINT i = 0; i < kCb; ++i)
        if (!IsPerViewSlot(i))
          x ^= HS(hCB + static_cast<uint32_t>(s) * 0x20 + i, U(t.cb[s][i]),
                  (static_cast<uint64_t>(t.cbFirst[s][i]) << 32) | t.cbNum[s][i]);
      for (UINT i = 0; i < kSrv; ++i) x ^= HS(hSRV + static_cast<uint32_t>(s) * 0x100 + i, U(t.srv[s][i]));
      for (UINT i = 0; i < kSmp; ++i) x ^= HS(hSMP + static_cast<uint32_t>(s) * 0x20 + i, U(t.smp[s][i]));
    }
    return x;
  }
};

// ---------------------------------------------------------------------------
// Records (one per teed DrawIndexed), items, streams
// ---------------------------------------------------------------------------
// Delta stream words: header = op | stage << 8 | lo << 16 | hi << 32, then the values.
enum DOp : uint32_t { dVS = 1, dPS, dIL, dTopo, dIB, dVB, dCB, dSRV, dSMP, dRS, dDSS, dBS, dStages };
struct Rec {
  UINT idxCount, startIdx;
  INT baseVtx;
  int item;
  uint32_t dOff, dEnd;   // deltas since the previous record
  uint32_t upFirst, upCount;
  uint64_t h;            // state hash at the draw (uploads by bytes, args mixed in)
  // R125: an instancer record (DrawIndexedInstancedIndirect from DCS's argument buffer): its buffer and offset, and
  // the sub-draw (0 / 1) it is; -1 for a patch record (DrawIndexed).
  ID3D11Buffer* argBuf;
  UINT argOff;
  int sub;
};
struct Up {  // a constant buffer slot bound to bytes this item uploaded
  uint8_t stage, slot;
  uint32_t off, len;  // in the stream's blob
};
struct Item {
  uint64_t desc;
  int first, count;
  int why;  // PerWhy
  bool inst;        // R125: a MetaMaterial_Instancer2 item
  uint8_t subMask;  // R125: its recorded sub-draws (bit per sub-draw; records in sub-draw order)
};
// R125: a focus item the twin replaces, with the sub-draws the focus view draws (instancer items; patches: all).
struct Pend {
  int item;
  uint8_t mask;
};
struct Targets {
  ID3D11RenderTargetView* rtv[8];
  UINT nrtv;
  ID3D11DepthStencilView* dsv;
  D3D11_VIEWPORT vp[16];
  UINT nvp;
  D3D11_RECT sc[16];
  UINT nsc;
  bool valid;
  bool uav;  // UAVs bound with the targets (not reproduced: never matches)
};
void ReleaseTargets(Targets& t) {
  for (auto*& r : t.rtv)
    if (r) r->Release(), r = nullptr;
  if (t.dsv) t.dsv->Release();
  memset(&t, 0, sizeof(t));
}
void CopyTargets(Targets& to, const Targets& from) {
  ReleaseTargets(to);
  to = from;
  for (auto* r : to.rtv)
    if (r) r->AddRef();
  if (to.dsv) to.dsv->AddRef();
}
bool SameTargets(const Targets& a, const Targets& b) {
  return a.valid && b.valid && !a.uav && !b.uav && a.nrtv == b.nrtv && memcmp(a.rtv, b.rtv, sizeof(a.rtv)) == 0 &&
         a.dsv == b.dsv && a.nvp == b.nvp && memcmp(a.vp, b.vp, sizeof(D3D11_VIEWPORT) * a.nvp) == 0 &&
         a.nsc == b.nsc && memcmp(a.sc, b.sc, sizeof(D3D11_RECT) * a.nsc) == 0;
}

struct Chunk {
  ID3D11CommandList* cl = nullptr;
  ID3D11Buffer* args = nullptr;
  UINT argsCap = 0;  // records the buffer holds
  int first = 0, count = 0;
  bool any = false;  // at least one twinnable record
  // R125: the argument buffer as a UAV (the gate pass writes the instancer records' words), and the gate's map (per
  // record of the chunk: the first argument word in the stream's stage copy, or ~0u).
  bool argsUavable = false;
  ID3D11UnorderedAccessView* argsUav = nullptr;
  ID3D11Buffer* map = nullptr;
  ID3D11ShaderResourceView* mapSrv = nullptr;
  UINT mapCap = 0;
  bool ind = false;  // the list runs the gate pass (it holds twinnable instancer records)
};

enum JobSt : int { kJIdle = 0, kJRunning, kJOk, kJFailed };
struct Stream {
  int eye = -1;
  uint64_t ciSeq = 0;
  uint64_t frame = 0;
  St base;
  uint64_t baseHash = 0;
  std::vector<uint64_t> d;
  std::vector<Rec> rec;
  std::vector<Up> up;
  std::vector<uint8_t> blob;
  std::vector<Item> item;
  std::vector<IUnknown*> refs;  // AddRef'd: everything the worker may bind
  ID3D11Buffer* pv[kPerView] = {};
  UINT pvFirst[kPerView] = {}, pvNum[kPerView] = {};
  bool pvSet[kPerView] = {};
  uint8_t tc[kTcBytes] = {};
  ID3D11Buffer* tcBuf = nullptr;
  bool usable = false;
  int unusable = 0;  // 0 ok, 1 shared write, 2 second TerrainContext, 3 fault, 4 full, 5 predication, 6 stale
  Pred pred;
  DescMap* map = nullptr;
  std::vector<uint64_t> dupDesc;  // descriptors two items shared
  // Constant buffers patch items wrote (item, buffer), and those a later item read without writing (inherited): every
  // writer of an inherited buffer stays DCS's in the focus, so whatever reads it there reads what stock wrote.
  std::vector<std::pair<int, ID3D11Buffer*>> writes;
  std::vector<ID3D11Buffer*> inherited;
  Targets targets{};              // the focus targets the lists draw into (the previous frame's focus pass)
  // worker output
  std::vector<Chunk> chunks;
  std::vector<uint32_t> args;  // 5 words per record (instance count 0)
  // One small constant buffer per upload the lists replay, filled by UpdateSubresource inside the list (no
  // constant-buffer offsets: runtimes that emulate command lists misplace offset windows of a dynamic buffer, WARP
  // R123), kept across frames.
  std::vector<ID3D11Buffer*> upCb;
  std::vector<UINT> upCbBytes;
  // R125: DCS's argument buffer (the first instancer record's; every other one is not twinnable) and the stage copy
  // of its words the gate pass reads (kept across frames).
  ID3D11Buffer* argBuf = nullptr;
  ID3D11Buffer* stage = nullptr;
  ID3D11ShaderResourceView* stageSrv = nullptr;
  UINT stageCap = 0;
  std::atomic<int> job{kJIdle};
  double buildUs = 0;
  int jobWorker = -1;
};

struct Eye {
  Stream s[2];
  int cur = -1;  // this frame's stream (index), -1 none
  EyeHist hist;
  Targets targets{};  // last focus pass's
  ID3D11Buffer* pvCopy[kPerView] = {};
  UINT pvBytes[kPerView] = {};
  GoodSet* good = nullptr;
  RunTracker* runs = nullptr;
  uint64_t lastPeriphCi = 0;
  int trust = 0;
  uint64_t focusPasses = 0;
  uint64_t sinceAudit = 0;
  uint64_t sinceVerify = 0;
};
Eye g_eye[2];

// ---------------------------------------------------------------------------
// Statistics (render thread, read by the worker tick)
// ---------------------------------------------------------------------------
struct Stats {
  uint64_t focusPasses = 0, why[kWCount] = {}, predWhy[kPwCount] = {};
  uint64_t periphPasses = 0, periphItems = 0, perWhy[kPCount] = {}, records = 0;
  uint64_t focusItems = 0, twinItems = 0, itemWhy[kICount] = {};
  uint64_t flushes = 0, ecls = 0, emptyDraws = 0;
  uint64_t audits = 0, auditOk = 0, auditFail = 0, auditCompared = 0;
  uint64_t windows = 0, windowsIgnored = 0, staleSkips = 0, swapRefused = 0, copyWrites = 0;
  uint64_t tapped = 0;
  uint64_t gpuTwinN = 0, gpuStockN = 0;
  double gpuTwinUs = 0, gpuStockUs = 0;
  // Render thread: our own sections (periphery: baseline, item bookkeeping, window end; focus: classification,
  // decision, item bookkeeping, flushes, window end) and DCS's patch items (periphery: under the tee taps; focus:
  // the ones DCS drew, under the light focus taps).
  double periphOwnUs = 0, periphDcsItemUs = 0;
  double focusOwnUs = 0, focusFlushUs = 0;
  double periphItemOwnUs = 0, focusItemOwnUs = 0;  // the per-item parts of the two above
  double teeDrawUs = 0;  // the record of a teed draw (inside DCS's item, so inside periphDcsItemUs; sampled)
  uint64_t teeDraws = 0;
  double focusStockItemUs = 0;
  uint64_t focusStockItems = 0;
  double buildUs = 0;
  uint64_t builds = 0, buildFails = 0, late = 0;
  double predDevMax = 0;
  uint64_t predChecks = 0;
  uint64_t verifies = 0, verifyFail = 0, verifyBytes = 0, verifyDiffBytes = 0, verifySkipped = 0;
  uint64_t timed = 0;  // items whose times were sampled (1 in 8; the time sums are scaled)
  // R125 instancer items (also counted in the totals above, except the DCS item times, which are per kind)
  uint64_t periphInstItems = 0, instRecords = 0, focusInstItems = 0, twinInstItems = 0, twinInstDraws = 0;
  uint64_t focusStockInstItems = 0, gateLists = 0, instNoDraw = 0, instMasked = 0;
  double focusStockInstUs = 0, periphDcsInstUs = 0;
};
Stats g_st;
std::mutex g_statsMutex;  // the tick copies / resets under it; the render thread updates plain fields (torn reads are ok)

inline double QpcUs() { return defrec::QpcToUs(); }

// ---------------------------------------------------------------------------
// The context table copy
// ---------------------------------------------------------------------------
enum Sl : int {
  sVSSetShader, sPSSetShader, sGSSetShader, sHSSetShader, sDSSetShader,
  sVSSetCB, sPSSetCB, sVSSetCB1, sPSSetCB1, sVSSetSRV, sPSSetSRV, sVSSetSmp, sPSSetSmp,
  sIASetIL, sIASetVB, sIASetIB, sIASetTopo, sOMSetBlend, sOMSetDSS, sRSSetState,
  sRSSetViewports, sRSSetScissor, sOMSetRT, sOMSetRTUAV,
  // work
  sDraw, sDrawIndexed, sDrawInstanced, sDrawIndexedInstanced, sDrawIndexedInstancedIndirect, sDrawInstancedIndirect,
  sDrawAuto, sDispatch, sDispatchIndirect, sUpdate, sUpdate1, sMap, sUnmap, sCopyResource, sCopySubresourceRegion,
  sCopySubresourceRegion1, sCopyStructureCount, sClearRTV, sClearDSV, sClearUAVUint, sClearUAVFloat, sClearView,
  sGenerateMips, sResolve, sECL, sClearState, sFlush, sSwap, sDiscardResource, sDiscardView, sDiscardView1,
  // binds outside the tracked state (inside a patch item: not twinnable)
  sGSSetCB, sGSSetSRV, sGSSetSmp, sHSSetCB, sHSSetSRV, sHSSetSmp, sDSSetCB, sDSSetSRV, sDSSetSmp, sCSSetShader,
  sCSSetCB, sCSSetSRV, sCSSetSmp, sCSSetUAV, sSOSetTargets, sSetPredication,
  sCount
};
const char* const kSlName[sCount] = {
    "VSSetShader", "PSSetShader", "GSSetShader", "HSSetShader", "DSSetShader", "VSSetConstantBuffers",
    "PSSetConstantBuffers", "VSSetConstantBuffers1", "PSSetConstantBuffers1", "VSSetShaderResources",
    "PSSetShaderResources", "VSSetSamplers", "PSSetSamplers", "IASetInputLayout", "IASetVertexBuffers",
    "IASetIndexBuffer", "IASetPrimitiveTopology", "OMSetBlendState", "OMSetDepthStencilState", "RSSetState",
    "RSSetViewports", "RSSetScissorRects", "OMSetRenderTargets", "OMSetRenderTargetsAndUnorderedAccessViews",
    "Draw", "DrawIndexed", "DrawInstanced", "DrawIndexedInstanced", "DrawIndexedInstancedIndirect",
    "DrawInstancedIndirect", "DrawAuto", "Dispatch", "DispatchIndirect", "UpdateSubresource", "UpdateSubresource1",
    "Map", "Unmap", "CopyResource", "CopySubresourceRegion", "CopySubresourceRegion1", "CopyStructureCount",
    "ClearRenderTargetView", "ClearDepthStencilView", "ClearUnorderedAccessViewUint", "ClearUnorderedAccessViewFloat",
    "ClearView", "GenerateMips", "ResolveSubresource", "ExecuteCommandList", "ClearState", "Flush",
    "SwapDeviceContextState", "DiscardResource", "DiscardView", "DiscardView1", "GSSetConstantBuffers",
    "GSSetShaderResources", "GSSetSamplers", "HSSetConstantBuffers", "HSSetShaderResources", "HSSetSamplers",
    "DSSetConstantBuffers", "DSSetShaderResources", "DSSetSamplers", "CSSetShader", "CSSetConstantBuffers",
    "CSSetShaderResources", "CSSetSamplers", "CSSetUnorderedAccessViews", "SOSetTargets", "SetPredication"};

struct Tbl {
  ID3D11DeviceContext* ctx = nullptr;  // DCS's immediate context (AddRef'd)
  void** orig = nullptr;               // its own table (inside the context object)
  void** copy = nullptr;               // ours (session lifetime, never freed)
  void** expect = nullptr;             // what each entry of the copy should hold
  void** snap = nullptr;               // entries taken from the live table at the swap (split filter's slots)
  uint8_t* page = nullptr;             // [data page: the live table pointer | code page: forwarders]
  void** origVar = nullptr;
  int n = 0;
  int slot[sCount] = {};
  bool fromLive[256] = {};             // this swap: the entry is the live table's (split filter's hook)
  bool tapped[256] = {};
  uintptr_t d3dLo = 0, d3dHi = 0;      // d3d11.dll's image
  bool swapped = false;
};
Tbl g_tbl;

template <class F>
inline F O(int s) {
  return reinterpret_cast<F>(g_tbl.orig[g_tbl.slot[s]]);
}

// ---------------------------------------------------------------------------
// The window: one TerrainObject::render call inside a G-buffer pass
// ---------------------------------------------------------------------------
enum WMode : int { kWmNone = 0, kWmPending, kWmPeriph, kWmFocus, kWmAudit, kWmIgnore };
struct Writer {  // constant buffer -> its last writer in this window
  ID3D11Buffer* buf;
  int item;       // -1: outside the patch items
  uint32_t upOff, upLen;
  uint64_t bytesHash;  // of the upload (the audit's state hash takes the bytes)
};
struct Win {
  bool open = false;
  int mode = kWmNone;
  int eye = -1;
  int64_t t0 = 0;
  uint64_t ciSeq = 0;
  CiFrame ci;
  uint8_t tc[kTcBytes];
  bool haveTc = false;
  int tcUploads = 0;
  ID3D11Buffer* tcBuf = nullptr;
  ID3D11Buffer* b10Buf = nullptr;  // the window's b10 upload (Visualizer 0x102ae0; 2.9.31: 0x12f270)
  bool inItem = false;
  int item = -1;
  int itemWhy = kPOk;
  bool itemInst = false;  // R125: the current item is an instancer item (its draws: DrawIndexedInstancedIndirect)
  int itemSub[2] = {};    // ... the sub-draws its view's bits announce, in order
  int itemSubN = 0, itemSubNext = 0;
  bool itemRec = true;   // the current item's draws are recorded (a candidate)
  bool recAll = true;    // this tee records every item (audit, trust, shadow, no runs known yet)
  Stream* s = nullptr;  // periphery / audit stream being built
  Shadow sh;
  bool sfOp[sfilt::kOpCount] = {};  // ops read from split_filter.h's shadow this window
  bool sfLive = false;
  Writer wr[64];
  int nwr = 0;
  // focus
  bool decided = false, twin = false, stopTwin = false;
  int why = kWTwin;
  Targets tg{};
  ID3D11Buffer* pvSrc[kPerView] = {};
  int lastIdx = -1;
  std::vector<Pend> pending;  // periphery item indices (and the focus's sub-draws), in order
  bool sawWork = false;      // other work since the last item (breaks runs)
  // [Suite] FocusTwinVerify: run 1 (stock) with the targets captured before any terrain draw, run 2 (the twin) from
  // the same target contents, the two results compared.
  bool verifyCap = false, verifyGo = false, verifyRun2 = false;
  ID3D11Query* q[3] = {};    // GPU timestamps of this focus window (disjoint, begin, end)
  int qSlot = -1;
};
Win g_win;
Stream g_audit;  // the focus stream an audit records

// GPU timing ring (focus windows)
struct GpuQ {
  ID3D11Query* dj = nullptr;
  ID3D11Query* b = nullptr;
  ID3D11Query* e = nullptr;
  bool pending = false;
  bool twin = false;
};
constexpr int kGpuRing = 8;
GpuQ g_gq[kGpuRing];
int g_gqNext = 0;

ID3D11Device* g_dev = nullptr;
ID3D11DeviceContext1* g_ctx1 = nullptr;
bool g_testDevice = false;  // tests: installed on a device of their own (no DCS device to compare with)
defrec::Pool g_pool;
bool g_poolOk = false;
// R125: the gate pass (cs_5_0): per record of a list, its five argument words from the stage copy of DCS's argument
// buffer when the record is enabled (word 1 set by the flush), else zero.
ID3D11ComputeShader* g_gateCs = nullptr;
bool g_testNoGate = false;  // tests: the gate cannot be built (the instancer part is not installed)
bool g_testNoMask = false;  // tests: every recorded sub-draw enabled (shows what the per-view enable prevents)
const char kGateHlsl[] =
    "Buffer<uint> gSrc : register(t0);\n"
    "Buffer<uint> gMap : register(t1);\n"
    "RWBuffer<uint> gArgs : register(u0);\n"
    "[numthreads(64, 1, 1)] void main(uint3 id : SV_DispatchThreadID) {\n"
    "  uint n; gMap.GetDimensions(n);\n"
    "  if (id.x >= n) return;\n"
    "  uint o = gMap[id.x];\n"
    "  if (o == 0xffffffffu) return;\n"
    "  uint b = id.x * 5u;\n"
    "  bool en = gArgs[b + 1u] != 0u;\n"
    "  [unroll] for (uint k = 0u; k < 5u; ++k) gArgs[b + k] = en ? gSrc[o + k] : 0u;\n"
    "}\n";

// ---------------------------------------------------------------------------
// Faults
// ---------------------------------------------------------------------------
void Latch(const char* where, DWORD code) {
  if (!g_faulted.exchange(true)) {
    g_faultWhere = where;
    g_faultCode = code;
    Log("focus twin: exception 0x%08lx in %s; off for this session (DCS draws everything)",
        static_cast<unsigned long>(code), where);
  }
  g_active = false;
}
inline int Filt(unsigned long code) {
  g_faultCode = code;
  return EXCEPTION_EXECUTE_HANDLER;
}

// ---------------------------------------------------------------------------
// Copy-table maintenance
// ---------------------------------------------------------------------------
inline bool InD3d11(const void* p) {
  const uintptr_t a = reinterpret_cast<uintptr_t>(p);
  return a >= g_tbl.d3dLo && a < g_tbl.d3dHi;
}
// Swaps DCS's context to our copy for the window. False: refused (another table in place).
bool SwapIn() {
  Tbl& t = g_tbl;
  if (!t.copy || !t.ctx) return false;
  void** cur = *reinterpret_cast<void***>(t.ctx);
  if (cur != t.orig) {
    g_st.swapRefused++;
    return false;
  }
  *t.origVar = t.orig;
  // split_filter.h checks, through the context's table pointer, that its op and ClearState entries are its own: those
  // entries of the copy are the live table's (its hooks), and their ops are read from its shadow.
  const bool sfOn = sfilt::g_hooked.load(std::memory_order_acquire) &&
                    static_cast<void*>(sfilt::g_ctx.load(std::memory_order_acquire)) == static_cast<void*>(t.ctx);
  for (int i = 0; i < t.n; ++i) t.fromLive[i] = false;
  for (bool& b : g_win.sfOp) b = false;
  g_win.sfLive = false;
  if (sfOn) {
    for (int h = 0; h < sfilt::kHookCount; ++h) {
      const int sl = sfilt::g_slot[h];
      if (sl < 0 || sl >= t.n) continue;
      if (t.orig[sl] == sfilt::kHooks[h]) {
        t.fromLive[sl] = true;
        if (h < sfilt::kOpCount) g_win.sfOp[h] = true;
      }
    }
    g_win.sfLive = true;
  }
  for (int i = 0; i < t.n; ++i) {
    t.snap[i] = t.fromLive[i] ? t.orig[i] : t.expect[i];
    t.copy[i] = t.snap[i];
  }
  *reinterpret_cast<void***>(t.ctx) = t.copy;
  t.swapped = true;
  return true;
}
// Entries someone wrote into the copy during the window: d3d11's own (a table rewrite that went through the table
// pointer) are carried into the live table; anything else is dropped. The copy is reset either way.
void SyncCopy() {
  Tbl& t = g_tbl;
  for (int i = 0; i < t.n; ++i) {
    void* c = t.copy[i];
    if (c == t.snap[i]) continue;
    g_st.copyWrites++;
    if (InD3d11(c)) InterlockedExchangePointer(&t.orig[i], c);
    t.copy[i] = t.snap[i];
  }
}
void SwapOut() {
  Tbl& t = g_tbl;
  if (!t.swapped) return;
  SyncCopy();
  if (*reinterpret_cast<void***>(t.ctx) == t.copy) *reinterpret_cast<void***>(t.ctx) = t.orig;
  t.swapped = false;
}
// Our own calls on DCS's context inside a window (flushes, the per-view copies) run on DCS's own table: a runtime
// that replays a command list through the context's table (emulated command lists) or calls itself virtually
// must not re-enter our taps. Afterwards the split filter's entries are taken again (its ExecuteCommandList hook
// may have re-hooked).
void Pause() {
  Tbl& t = g_tbl;
  if (t.swapped && *reinterpret_cast<void***>(t.ctx) == t.copy) {
    SyncCopy();
    *reinterpret_cast<void***>(t.ctx) = t.orig;
  }
}
void Resume() {
  Tbl& t = g_tbl;
  if (!t.swapped || *reinterpret_cast<void***>(t.ctx) != t.orig) return;
  for (int i = 0; i < t.n; ++i)
    if (t.fromLive[i]) t.snap[i] = t.copy[i] = t.orig[i];
  *reinterpret_cast<void***>(t.ctx) = t.copy;
}

// ---------------------------------------------------------------------------
// Window state helpers (render thread)
// ---------------------------------------------------------------------------
inline Writer* FindWriter(ID3D11Buffer* b) {
  for (int i = 0; i < g_win.nwr; ++i)
    if (g_win.wr[i].buf == b) return &g_win.wr[i];
  return nullptr;
}
inline Writer* AddWriter(ID3D11Buffer* b) {
  if (Writer* w = FindWriter(b)) return w;
  if (g_win.nwr >= 64) return nullptr;
  Writer& w = g_win.wr[g_win.nwr++];
  w.buf = b, w.item = -1, w.upOff = w.upLen = 0, w.bytesHash = 0;
  return &w;
}
inline void MarkItem(int why) {
  if (g_win.inItem && g_win.itemWhy == kPOk) g_win.itemWhy = why;
}

// Full state snapshot from the device (the window's baseline). References are taken into the stream.
void GetState(ID3D11DeviceContext* c, St& t, std::vector<IUnknown*>* refs) {
  memset(&t, 0, sizeof(t));
  auto keep = [&](IUnknown* p) {
    if (!p) return;
    if (refs) refs->push_back(p); else p->Release();
  };
  ID3D11VertexShader* vs = nullptr;
  ID3D11PixelShader* ps = nullptr;
  ID3D11GeometryShader* gs = nullptr;
  ID3D11HullShader* hs = nullptr;
  ID3D11DomainShader* ds = nullptr;
  ID3D11ClassInstance* ci[256] = {};
  UINT nci = 256;
  c->VSGetShader(&vs, ci, &nci);
  for (UINT i = 0; i < nci && i < 256; ++i)
    if (ci[i]) ci[i]->Release();
  nci = 256;
  c->PSGetShader(&ps, ci, &nci);
  for (UINT i = 0; i < nci && i < 256; ++i)
    if (ci[i]) ci[i]->Release();
  nci = 0;
  c->GSGetShader(&gs, nullptr, &nci);
  nci = 0;
  c->HSGetShader(&hs, nullptr, &nci);
  nci = 0;
  c->DSGetShader(&ds, nullptr, &nci);
  t.vs = vs, t.ps = ps, t.gs = gs, t.hs = hs, t.ds = ds;
  keep(vs), keep(ps), keep(gs), keep(hs), keep(ds);
  ID3D11InputLayout* il = nullptr;
  c->IAGetInputLayout(&il);
  t.il = il, keep(il);
  D3D11_PRIMITIVE_TOPOLOGY topo = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
  c->IAGetPrimitiveTopology(&topo);
  t.topo = static_cast<UINT>(topo);
  c->IAGetVertexBuffers(0, kVb, t.vb, t.vbStride, t.vbOff);
  for (auto* b : t.vb) keep(b);
  DXGI_FORMAT f = DXGI_FORMAT_UNKNOWN;
  c->IAGetIndexBuffer(&t.ib, &f, &t.ibOff);
  t.ibFmt = static_cast<UINT>(f);
  keep(t.ib);
  if (g_ctx1) {
    g_ctx1->VSGetConstantBuffers1(0, kCb, t.cb[0], t.cbFirst[0], t.cbNum[0]);
    g_ctx1->PSGetConstantBuffers1(0, kCb, t.cb[1], t.cbFirst[1], t.cbNum[1]);
  } else {
    c->VSGetConstantBuffers(0, kCb, t.cb[0]);
    c->PSGetConstantBuffers(0, kCb, t.cb[1]);
  }
  for (int s = 0; s < 2; ++s)
    for (UINT i = 0; i < kCb; ++i) {
      ID3D11Buffer* b = t.cb[s][i];
      if (!b) {
        t.cbFirst[s][i] = t.cbNum[s][i] = 0;
        continue;
      }
      // A plain binding reads back as offset 0 with a window covering the buffer (0, 4096 or its size, depending on
      // the runtime): normalised to 0 / 0, as the setters record it (defrec::detail::FinishCbs's rule).
      D3D11_BUFFER_DESC bd{};
      b->GetDesc(&bd);
      if (t.cbFirst[s][i] == 0 && (t.cbNum[s][i] == 0 || t.cbNum[s][i] * 16u >= bd.ByteWidth)) t.cbNum[s][i] = 0;
      keep(b);
    }
  c->VSGetShaderResources(0, kSrv, t.srv[0]);
  c->PSGetShaderResources(0, kSrv, t.srv[1]);
  for (int s = 0; s < 2; ++s)
    for (auto* v : t.srv[s]) keep(v);
  c->VSGetSamplers(0, kSmp, t.smp[0]);
  c->PSGetSamplers(0, kSmp, t.smp[1]);
  for (int s = 0; s < 2; ++s)
    for (auto* v : t.smp[s]) keep(v);
  ID3D11RasterizerState* rs = nullptr;
  c->RSGetState(&rs);
  t.rs = rs, keep(rs);
  ID3D11DepthStencilState* dss = nullptr;
  c->OMGetDepthStencilState(&dss, &t.sref);
  t.dss = dss, keep(dss);
  ID3D11BlendState* bs = nullptr;
  c->OMGetBlendState(&bs, t.bf, &t.smask);
  t.bs = bs, keep(bs);
}
void GetTargets(ID3D11DeviceContext* c, Targets& t) {
  ReleaseTargets(t);
  ID3D11UnorderedAccessView* uav[8] = {};
  c->OMGetRenderTargetsAndUnorderedAccessViews(8, t.rtv, &t.dsv, 0, 8, uav);
  for (auto*& u : uav)
    if (u) t.uav = true, u->Release();
  t.nrtv = 8;
  while (t.nrtv > 0 && !t.rtv[t.nrtv - 1]) --t.nrtv;
  t.nvp = 16;
  c->RSGetViewports(&t.nvp, t.vp);
  t.nsc = 16;
  c->RSGetScissorRects(&t.nsc, t.sc);
  t.valid = true;
}

// ---------------------------------------------------------------------------
// Split filter's shadow -> ours (its ops bypass every table)
// ---------------------------------------------------------------------------
inline bool Known(const void* p) { return p != sfilt::kUnknown; }
void SyncSfilt() {
  if (!g_win.sfLive) return;
  Shadow& s = g_win.sh;
  const sfilt::Shadow& f = sfilt::g_sh;
  const bool* on = g_win.sfOp;
  if (on[sfilt::kVS] && Known(f.vs)) s.SetVS(f.vs);
  if (on[sfilt::kPS] && Known(f.ps)) s.SetPS(f.ps);
  if (on[sfilt::kIL] && Known(f.il)) s.SetIL(f.il);
  if (on[sfilt::kTopo] && f.topo != ~0u) s.SetTopo(f.topo);
  if (on[sfilt::kRS] && Known(f.rs)) s.SetRS(f.rs);
  if (on[sfilt::kDepth] && Known(f.depth)) s.SetDSS(f.depth, f.stencilRef);
  if (on[sfilt::kBlend] && Known(f.blend)) s.SetBS(f.blend, f.blendFactor, f.sampleMask);
  for (int st = 0; st < 2; ++st) {
    const int opCb = st == 0 ? sfilt::kVSCB : sfilt::kPSCB;
    const int opSm = st == 0 ? sfilt::kVSSamp : sfilt::kPSSamp;
    if (on[opCb]) {
      void* const* a = st == 0 ? f.vsCb : f.psCb;
      for (UINT i = 0; i < kCb; ++i)
        if (Known(a[i])) s.SetCB(st, i, static_cast<ID3D11Buffer*>(a[i]), 0, 0);
    }
    if (on[opSm]) {
      void* const* a = st == 0 ? f.vsSamp : f.psSamp;
      for (UINT i = 0; i < kSmp; ++i)
        if (Known(a[i])) s.SetSMP(st, i, static_cast<ID3D11SamplerState*>(a[i]));
    }
  }
}

// ---------------------------------------------------------------------------
// Delta emission (render thread, at each teed draw)
// ---------------------------------------------------------------------------
inline void Ref(Stream& s, void* p) {
  if (!p) return;
  static_cast<IUnknown*>(p)->AddRef();
  s.refs.push_back(static_cast<IUnknown*>(p));
}
inline uint64_t Hdr(uint32_t op, int stage, UINT lo, UINT hi) {
  return op | (static_cast<uint64_t>(stage) << 8) | (static_cast<uint64_t>(lo) << 16) | (static_cast<uint64_t>(hi) << 32);
}
void EmitDeltas(Stream& s, Shadow& sh) {
  const St& t = sh.st;
  Dirty& d = sh.dirty;
  auto one = [&](uint32_t op, void* v) {
    s.d.push_back(Hdr(op, 0, 0, 0));
    s.d.push_back(U(v));
    Ref(s, v);
  };
  if (d.g & gVS) one(dVS, t.vs);
  if (d.g & gPS) one(dPS, t.ps);
  if (d.g & gIL) one(dIL, t.il);
  if (d.g & gRS) one(dRS, t.rs);
  if (d.g & gTopo) s.d.push_back(Hdr(dTopo, 0, 0, 0)), s.d.push_back(t.topo);
  if (d.g & gIB) {
    s.d.push_back(Hdr(dIB, 0, 0, 0));
    s.d.push_back(U(t.ib));
    s.d.push_back((static_cast<uint64_t>(t.ibFmt) << 32) | t.ibOff);
    Ref(s, t.ib);
  }
  if (d.g & gDSS) {
    s.d.push_back(Hdr(dDSS, 0, 0, 0));
    s.d.push_back(U(t.dss));
    s.d.push_back(t.sref);
    Ref(s, t.dss);
  }
  if (d.g & gBS) {
    uint64_t a = 0, b = 0;
    memcpy(&a, t.bf, 8), memcpy(&b, t.bf + 2, 8);
    s.d.push_back(Hdr(dBS, 0, 0, 0));
    s.d.push_back(U(t.bs));
    s.d.push_back(a);
    s.d.push_back(b);
    s.d.push_back(t.smask);
    Ref(s, t.bs);
  }
  if (d.vb.Any()) {
    s.d.push_back(Hdr(dVB, 0, d.vb.lo, d.vb.hi));
    for (UINT i = d.vb.lo; i < d.vb.hi; ++i) {
      s.d.push_back(U(t.vb[i]));
      s.d.push_back((static_cast<uint64_t>(t.vbStride[i]) << 32) | t.vbOff[i]);
      Ref(s, t.vb[i]);
    }
  }
  for (int st = 0; st < 2; ++st) {
    if (d.cb[st].Any()) {
      s.d.push_back(Hdr(dCB, st, d.cb[st].lo, d.cb[st].hi));
      for (UINT i = d.cb[st].lo; i < d.cb[st].hi; ++i) {
        s.d.push_back(U(t.cb[st][i]));
        s.d.push_back((static_cast<uint64_t>(t.cbFirst[st][i]) << 32) | t.cbNum[st][i]);
        Ref(s, t.cb[st][i]);
      }
    }
    if (d.srv[st].Any()) {
      s.d.push_back(Hdr(dSRV, st, d.srv[st].lo, d.srv[st].hi));
      for (UINT i = d.srv[st].lo; i < d.srv[st].hi; ++i) s.d.push_back(U(t.srv[st][i])), Ref(s, t.srv[st][i]);
    }
    if (d.smp[st].Any()) {
      s.d.push_back(Hdr(dSMP, st, d.smp[st].lo, d.smp[st].hi));
      for (UINT i = d.smp[st].lo; i < d.smp[st].hi; ++i) s.d.push_back(U(t.smp[st][i])), Ref(s, t.smp[st][i]);
    }
  }
  d.Clear();
}

// One teed DrawIndexed (inside a patch item of a periphery / audit window), or (R125, argBuf set) one
// DrawIndexedInstancedIndirect inside an instancer item: the item's next announced sub-draw.
void TeeDraw(UINT idxCount, UINT startIdx, INT baseVtx, ID3D11Buffer* argBuf = nullptr, UINT argOff = 0) {
  Win& w = g_win;
  Stream& s = *w.s;
  int sub = -1;
  if (argBuf) {
    if (w.itemSubNext >= w.itemSubN) {
      MarkItem(kPSubs);
      return;
    }
    sub = w.itemSub[w.itemSubNext++];
  }
  if (s.rec.size() >= 65536) {
    MarkItem(kPFull);
    return;
  }
  if (!w.itemRec) {  // not recorded: only what it reads that an earlier item wrote (its writers stay DCS's)
    const St& t = w.sh.st;
    for (int st = 0; st < 2; ++st)
      for (UINT i = 0; i < kCb; ++i) {
        ID3D11Buffer* b = t.cb[st][i];
        if (!b || IsPerViewSlot(i)) continue;
        Writer* wr = FindWriter(b);
        if (wr && wr->item >= 0 && wr->item != w.item &&
            std::find(s.inherited.begin(), s.inherited.end(), b) == s.inherited.end())
          s.inherited.push_back(b);
      }
    return;
  }
  SyncSfilt();
  const St& t = w.sh.st;
  if (t.gs || t.hs || t.ds) MarkItem(kPStages);
  // Per-view slots: one buffer per slot for the whole stream (the same object in both stages).
  for (int k = 0; k < kPerView; ++k) {
    const UINT slot = kPerViewSlot[k];
    for (int st = 0; st < 2; ++st) {
      ID3D11Buffer* b = t.cb[st][slot];
      if (!b) continue;
      if (!s.pvSet[k]) {
        s.pv[k] = b, s.pvFirst[k] = t.cbFirst[st][slot], s.pvNum[k] = t.cbNum[st][slot], s.pvSet[k] = true;
        Ref(s, b);
      } else if (s.pv[k] != b || s.pvFirst[k] != t.cbFirst[st][slot] || s.pvNum[k] != t.cbNum[st][slot]) {
        MarkItem(kPPerView);
      }
      if (Writer* wr = FindWriter(b))
        if (wr->item >= 0) MarkItem(kPPerView);  // a patch item wrote a per-view buffer
    }
  }
  Rec r{};
  r.idxCount = idxCount, r.startIdx = startIdx, r.baseVtx = baseVtx, r.item = w.item;
  r.sub = sub;
  if (argBuf) {
    // One argument buffer per stream (the gate copies from it): DCS's IM2 has one.
    if (!s.argBuf) s.argBuf = argBuf;
    if (s.argBuf != argBuf || (argOff & 3u)) MarkItem(kPArgs);
    r.argBuf = argBuf, r.argOff = argOff;
    Ref(s, argBuf);
  }
  r.upFirst = static_cast<uint32_t>(s.up.size());
  uint64_t h = w.sh.h;
  for (int st = 0; st < 2; ++st)
    for (UINT i = 0; i < kCb; ++i) {
      ID3D11Buffer* b = t.cb[st][i];
      if (!b || IsPerViewSlot(i)) continue;
      Writer* wr = FindWriter(b);
      if (!wr || wr->item < 0) continue;  // pass-level content: DCS's buffer
      if (wr->item != w.item) {
        MarkItem(kPInherit);
        if (std::find(s.inherited.begin(), s.inherited.end(), b) == s.inherited.end()) s.inherited.push_back(b);
        continue;
      }
      Up u{static_cast<uint8_t>(st), static_cast<uint8_t>(i), wr->upOff, wr->upLen};
      s.up.push_back(u);
      // The hash takes the bytes, not the (per-draw) buffer object.
      const uint32_t id = hCB + static_cast<uint32_t>(st) * 0x20 + i;
      h ^= HS(id, U(b), (static_cast<uint64_t>(t.cbFirst[st][i]) << 32) | t.cbNum[st][i]);
      h ^= HS(id, wr->bytesHash, 1);
    }
  r.upCount = static_cast<uint32_t>(s.up.size()) - r.upFirst;
  r.dOff = static_cast<uint32_t>(s.d.size());
  EmitDeltas(s, w.sh);
  r.dEnd = static_cast<uint32_t>(s.d.size());
  r.h = h ^ HS(0x7777, (static_cast<uint64_t>(idxCount) << 32) | startIdx, static_cast<uint64_t>(static_cast<uint32_t>(baseVtx)));
  if (argBuf) r.h ^= HS(0x7778, U(argBuf), (static_cast<uint64_t>(argOff) << 8) | static_cast<uint64_t>(sub));
  s.rec.push_back(r);
  g_st.records++;
  if (argBuf && w.mode == kWmPeriph) g_st.instRecords++;
}

// A constant-buffer upload seen in a tee window.
void TeeUpload(ID3D11Resource* r, const D3D11_BOX* box, const void* src) {
  Win& w = g_win;
  D3D11_RESOURCE_DIMENSION dim = D3D11_RESOURCE_DIMENSION_UNKNOWN;
  r->GetType(&dim);
  if (dim != D3D11_RESOURCE_DIMENSION_BUFFER) {
    MarkItem(kPWork);
    return;
  }
  ID3D11Buffer* b = static_cast<ID3D11Buffer*>(r);
  D3D11_BUFFER_DESC bd{};
  b->GetDesc(&bd);
  if (!(bd.BindFlags & D3D11_BIND_CONSTANT_BUFFER)) {
    MarkItem(kPWork);
    return;
  }
  Writer* wr = AddWriter(b);
  if (!w.inItem) {
    if (wr) wr->item = -1;
    return;
  }
  if (box || !src) {
    MarkItem(kPPartial);
    if (wr) wr->item = -2;  // unknown content
    return;
  }
  if (!wr) {
    MarkItem(kPFull);
    return;
  }
  Stream& s = *w.s;
  if (wr->item != w.item) s.writes.emplace_back(w.item, b);
  wr->item = w.item;
  if (!w.itemRec) {  // its bytes are never replayed
    wr->upOff = wr->upLen = 0, wr->bytesHash = 0;
    return;
  }
  wr->upOff = static_cast<uint32_t>(s.blob.size());
  wr->upLen = bd.ByteWidth;
  const uint8_t* p = static_cast<const uint8_t*>(src);
  s.blob.insert(s.blob.end(), p, p + bd.ByteWidth);
  uint64_t bh = 0x9e3779b97f4a7c15ull ^ bd.ByteWidth;
  const size_t q = bd.ByteWidth / 8;
  for (size_t k = 0; k < q; ++k) {
    uint64_t v;
    memcpy(&v, p + 8 * k, 8);
    bh = (bh ^ (v + 0x9e3779b97f4a7c15ull + (bh << 6) + (bh >> 2))) * 0xff51afd7ed558ccdull;
  }
  for (size_t k = q * 8; k < bd.ByteWidth; ++k) bh = (bh ^ p[k]) * 0x100000001b3ull;
  wr->bytesHash = bh;
}

// ---------------------------------------------------------------------------
// Taps (only reached while DCS's context runs on the copy: one window on the render thread)
// ---------------------------------------------------------------------------
using Ctx = ID3D11DeviceContext;
void Flush();
void OnTcUpload(ID3D11Resource* r, const D3D11_BOX* box, const void* src);

inline bool Teeing() { return g_win.mode == kWmPeriph || g_win.mode == kWmAudit; }
// Other work on the GPU timeline: in a tee window inside an item it makes the item not twinnable; outside items a
// draw is checked against the item-written buffers (the ledger); in a focus window the pending twin draws go first.
__declspec(noinline) void WorkBody(int sl) {
  Win& w = g_win;
  if (w.mode == kWmFocus) {
    if (!w.pending.empty()) Flush();
    if (!w.inItem) w.sawWork = true;
    return;
  }
  if (!Teeing()) return;
  if (w.inItem) {  // the item's own draw and constant-buffer uploads are recorded; anything else is not replayable
    const int draw = w.itemInst ? sDrawIndexedInstancedIndirect : sDrawIndexed;
    if (sl != draw && sl != sUpdate && sl != sUpdate1) MarkItem(kPWork);
    return;
  }
  w.sawWork = true;
  if (sl >= sDraw && sl <= sDispatchIndirect && w.nwr) {
    SyncSfilt();
    for (int i = 0; i < w.nwr; ++i) {
      if (w.wr[i].item < 0) continue;
      for (int st = 0; st < 2; ++st)
        for (UINT k = 0; k < kCb; ++k)
          if (w.sh.st.cb[st][k] == w.wr[i].buf && w.s && w.s->usable) {
            w.s->usable = false;
            w.s->unusable = 1;  // shared write
          }
    }
  }
}
inline void Work(int sl) {
  g_st.tapped++;
  __try {
    WorkBody(sl);
  } __except (Filt(GetExceptionCode())) {
    Latch("a focus twin tap", g_faultCode);
  }
}
// Binds outside the tracked state: inside a teed item they make it not twinnable.
inline void OtherBind() {
  g_st.tapped++;
  if (g_win.inItem && Teeing()) MarkItem(kPWork);
}
#define FT_GUARD(body)                          \
  g_st.tapped++;                                \
  if (Teeing()) {                               \
    __try {                                     \
      body;                                     \
    } __except (Filt(GetExceptionCode())) {     \
      Latch("a focus twin tap", g_faultCode);   \
    }                                           \
  }

void STDMETHODCALLTYPE T_VSSetShader(Ctx* c, ID3D11VertexShader* s, ID3D11ClassInstance* const* ci, UINT n) {
  FT_GUARD(if (n) MarkItem(kPStages); g_win.sh.SetVS(s));
  O<decltype(&T_VSSetShader)>(sVSSetShader)(c, s, ci, n);
}
void STDMETHODCALLTYPE T_PSSetShader(Ctx* c, ID3D11PixelShader* s, ID3D11ClassInstance* const* ci, UINT n) {
  FT_GUARD(if (n) MarkItem(kPStages); g_win.sh.SetPS(s));
  O<decltype(&T_PSSetShader)>(sPSSetShader)(c, s, ci, n);
}
void STDMETHODCALLTYPE T_GSSetShader(Ctx* c, ID3D11GeometryShader* s, ID3D11ClassInstance* const* ci, UINT n) {
  FT_GUARD(g_win.sh.SetStage(0, s));
  O<decltype(&T_GSSetShader)>(sGSSetShader)(c, s, ci, n);
}
void STDMETHODCALLTYPE T_HSSetShader(Ctx* c, ID3D11HullShader* s, ID3D11ClassInstance* const* ci, UINT n) {
  FT_GUARD(g_win.sh.SetStage(1, s));
  O<decltype(&T_HSSetShader)>(sHSSetShader)(c, s, ci, n);
}
void STDMETHODCALLTYPE T_DSSetShader(Ctx* c, ID3D11DomainShader* s, ID3D11ClassInstance* const* ci, UINT n) {
  FT_GUARD(g_win.sh.SetStage(2, s));
  O<decltype(&T_DSSetShader)>(sDSSetShader)(c, s, ci, n);
}
inline void CbRange(int st, UINT a, UINT n, ID3D11Buffer* const* b, const UINT* f, const UINT* k) {
  if (!b) {
    for (UINT i = 0; i < n && a + i < kCb; ++i) g_win.sh.SetCB(st, a + i, nullptr, 0, 0);
    return;
  }
  for (UINT i = 0; i < n && a + i < kCb; ++i) g_win.sh.SetCB(st, a + i, b[i], f ? f[i] : 0, k ? k[i] : 0);
}
void STDMETHODCALLTYPE T_VSSetCB(Ctx* c, UINT a, UINT n, ID3D11Buffer* const* b) {
  FT_GUARD(CbRange(0, a, n, b, nullptr, nullptr));
  O<decltype(&T_VSSetCB)>(sVSSetCB)(c, a, n, b);
}
void STDMETHODCALLTYPE T_PSSetCB(Ctx* c, UINT a, UINT n, ID3D11Buffer* const* b) {
  FT_GUARD(CbRange(1, a, n, b, nullptr, nullptr));
  O<decltype(&T_PSSetCB)>(sPSSetCB)(c, a, n, b);
}
void STDMETHODCALLTYPE T_VSSetCB1(ID3D11DeviceContext1* c, UINT a, UINT n, ID3D11Buffer* const* b, const UINT* f,
                                  const UINT* k) {
  FT_GUARD(CbRange(0, a, n, b, f, k));
  O<decltype(&T_VSSetCB1)>(sVSSetCB1)(c, a, n, b, f, k);
}
void STDMETHODCALLTYPE T_PSSetCB1(ID3D11DeviceContext1* c, UINT a, UINT n, ID3D11Buffer* const* b, const UINT* f,
                                  const UINT* k) {
  FT_GUARD(CbRange(1, a, n, b, f, k));
  O<decltype(&T_PSSetCB1)>(sPSSetCB1)(c, a, n, b, f, k);
}
inline void SrvRange(int st, UINT a, UINT n, ID3D11ShaderResourceView* const* v) {
  for (UINT i = 0; i < n && a + i < kSrv; ++i) g_win.sh.SetSRV(st, a + i, v ? v[i] : nullptr);
}
void STDMETHODCALLTYPE T_VSSetSRV(Ctx* c, UINT a, UINT n, ID3D11ShaderResourceView* const* v) {
  FT_GUARD(SrvRange(0, a, n, v));
  O<decltype(&T_VSSetSRV)>(sVSSetSRV)(c, a, n, v);
}
void STDMETHODCALLTYPE T_PSSetSRV(Ctx* c, UINT a, UINT n, ID3D11ShaderResourceView* const* v) {
  FT_GUARD(SrvRange(1, a, n, v));
  O<decltype(&T_PSSetSRV)>(sPSSetSRV)(c, a, n, v);
}
inline void SmpRange(int st, UINT a, UINT n, ID3D11SamplerState* const* v) {
  for (UINT i = 0; i < n && a + i < kSmp; ++i) g_win.sh.SetSMP(st, a + i, v ? v[i] : nullptr);
}
void STDMETHODCALLTYPE T_VSSetSmp(Ctx* c, UINT a, UINT n, ID3D11SamplerState* const* v) {
  FT_GUARD(SmpRange(0, a, n, v));
  O<decltype(&T_VSSetSmp)>(sVSSetSmp)(c, a, n, v);
}
void STDMETHODCALLTYPE T_PSSetSmp(Ctx* c, UINT a, UINT n, ID3D11SamplerState* const* v) {
  FT_GUARD(SmpRange(1, a, n, v));
  O<decltype(&T_PSSetSmp)>(sPSSetSmp)(c, a, n, v);
}
void STDMETHODCALLTYPE T_IASetIL(Ctx* c, ID3D11InputLayout* l) {
  FT_GUARD(g_win.sh.SetIL(l));
  O<decltype(&T_IASetIL)>(sIASetIL)(c, l);
}
void STDMETHODCALLTYPE T_IASetVB(Ctx* c, UINT a, UINT n, ID3D11Buffer* const* b, const UINT* s, const UINT* o) {
  FT_GUARD(for (UINT i = 0; i < n && a + i < kVb; ++i) g_win.sh.SetVB(a + i, b ? b[i] : nullptr, s ? s[i] : 0, o ? o[i] : 0));
  O<decltype(&T_IASetVB)>(sIASetVB)(c, a, n, b, s, o);
}
void STDMETHODCALLTYPE T_IASetIB(Ctx* c, ID3D11Buffer* b, DXGI_FORMAT f, UINT o) {
  FT_GUARD(g_win.sh.SetIB(b, static_cast<UINT>(f), o));
  O<decltype(&T_IASetIB)>(sIASetIB)(c, b, f, o);
}
void STDMETHODCALLTYPE T_IASetTopo(Ctx* c, D3D11_PRIMITIVE_TOPOLOGY t) {
  FT_GUARD(g_win.sh.SetTopo(static_cast<UINT>(t)));
  O<decltype(&T_IASetTopo)>(sIASetTopo)(c, t);
}
void STDMETHODCALLTYPE T_OMSetBlend(Ctx* c, ID3D11BlendState* b, const FLOAT f[4], UINT m) {
  FT_GUARD(g_win.sh.SetBS(b, f, m));
  O<decltype(&T_OMSetBlend)>(sOMSetBlend)(c, b, f, m);
}
void STDMETHODCALLTYPE T_OMSetDSS(Ctx* c, ID3D11DepthStencilState* d, UINT r) {
  FT_GUARD(g_win.sh.SetDSS(d, r));
  O<decltype(&T_OMSetDSS)>(sOMSetDSS)(c, d, r);
}
void STDMETHODCALLTYPE T_RSSetState(Ctx* c, ID3D11RasterizerState* r) {
  FT_GUARD(g_win.sh.SetRS(r));
  O<decltype(&T_RSSetState)>(sRSSetState)(c, r);
}
// Viewports, scissors, targets: never changed by a twinnable item. In a focus window the pending twin draws go first
// and nothing more is replaced (the lists draw into the pass's targets). Outside items in a tee window the SRV
// shadow is read back (d3d11 unbinds inputs that become outputs).
__declspec(noinline) void TargetsChange() {
  Win& w = g_win;
  if (w.mode == kWmFocus) {
    if (!w.pending.empty()) Flush();
    w.stopTwin = true;
    return;
  }
  MarkItem(kPWork);
}
inline void TargetsChangeGuarded() {
  g_st.tapped++;
  __try {
    TargetsChange();
  } __except (Filt(GetExceptionCode())) {
    Latch("a focus twin tap", g_faultCode);
  }
}
void ResyncSrv(Ctx* c);
inline void ResyncSrvGuarded(Ctx* c) {
  if (!Teeing()) return;
  __try {
    ResyncSrv(c);
  } __except (Filt(GetExceptionCode())) {
    Latch("a focus twin tap", g_faultCode);
  }
}
void STDMETHODCALLTYPE T_RSSetViewports(Ctx* c, UINT n, const D3D11_VIEWPORT* v) {
  TargetsChangeGuarded();
  O<decltype(&T_RSSetViewports)>(sRSSetViewports)(c, n, v);
}
void STDMETHODCALLTYPE T_RSSetScissor(Ctx* c, UINT n, const D3D11_RECT* r) {
  TargetsChangeGuarded();
  O<decltype(&T_RSSetScissor)>(sRSSetScissor)(c, n, r);
}
void STDMETHODCALLTYPE T_OMSetRT(Ctx* c, UINT n, ID3D11RenderTargetView* const* r, ID3D11DepthStencilView* d) {
  TargetsChangeGuarded();
  O<decltype(&T_OMSetRT)>(sOMSetRT)(c, n, r, d);
  ResyncSrvGuarded(c);
}
void STDMETHODCALLTYPE T_OMSetRTUAV(Ctx* c, UINT n, ID3D11RenderTargetView* const* r, ID3D11DepthStencilView* d,
                                    UINT us, UINT un, ID3D11UnorderedAccessView* const* u, const UINT* ic) {
  TargetsChangeGuarded();
  O<decltype(&T_OMSetRTUAV)>(sOMSetRTUAV)(c, n, r, d, us, un, u, ic);
  ResyncSrvGuarded(c);
}
// d3d11 may have unbound inputs: the SRV shadow from the device (both stages).
void ResyncSrv(Ctx* c) {
  ID3D11ShaderResourceView* v[kSrv];
  for (int st = 0; st < 2; ++st) {
    memset(v, 0, sizeof(v));
    if (st == 0)
      c->VSGetShaderResources(0, kSrv, v);
    else
      c->PSGetShaderResources(0, kSrv, v);
    for (UINT i = 0; i < kSrv; ++i) {
      g_win.sh.SetSRV(st, i, v[i]);
      if (v[i]) v[i]->Release();
    }
  }
}

// Work taps
void STDMETHODCALLTYPE T_Draw(Ctx* c, UINT a, UINT b) {
  Work(sDraw);
  O<decltype(&T_Draw)>(sDraw)(c, a, b);
}
void STDMETHODCALLTYPE T_DrawIndexed(Ctx* c, UINT n, UINT s, INT b) {
  Work(sDrawIndexed);
  if (g_win.inItem && !g_win.itemInst && Teeing()) {
    static uint32_t sampled = 0;
    const bool timed = (++sampled & 7u) == 0;
    const int64_t t0 = timed ? defrec::Qpc() : 0;
    __try {
      TeeDraw(n, s, b);
    } __except (Filt(GetExceptionCode())) {
      Latch("the focus twin tee", g_faultCode);
    }
    if (timed) g_st.teeDrawUs += (defrec::Qpc() - t0) * QpcUs() * 8.0, g_st.teeDraws += 8;
  }
  O<decltype(&T_DrawIndexed)>(sDrawIndexed)(c, n, s, b);
}
void STDMETHODCALLTYPE T_DrawInstanced(Ctx* c, UINT a, UINT b, UINT d, UINT e) {
  Work(sDrawInstanced);
  O<decltype(&T_DrawInstanced)>(sDrawInstanced)(c, a, b, d, e);
}
void STDMETHODCALLTYPE T_DrawIndexedInstanced(Ctx* c, UINT a, UINT b, UINT d, INT e, UINT f) {
  Work(sDrawIndexedInstanced);
  O<decltype(&T_DrawIndexedInstanced)>(sDrawIndexedInstanced)(c, a, b, d, e, f);
}
void STDMETHODCALLTYPE T_DrawIndexedInstancedIndirect(Ctx* c, ID3D11Buffer* b, UINT o) {
  Work(sDrawIndexedInstancedIndirect);
  if (g_win.inItem && g_win.itemInst && Teeing()) {  // R125: an instancer item's sub-draw
    static uint32_t sampled = 0;
    const bool timed = (++sampled & 7u) == 0;
    const int64_t t0 = timed ? defrec::Qpc() : 0;
    __try {
      if (b)
        TeeDraw(0, 0, 0, b, o);
      else
        MarkItem(kPWork);
    } __except (Filt(GetExceptionCode())) {
      Latch("the focus twin tee", g_faultCode);
    }
    if (timed) g_st.teeDrawUs += (defrec::Qpc() - t0) * QpcUs() * 8.0, g_st.teeDraws += 8;
  }
  O<decltype(&T_DrawIndexedInstancedIndirect)>(sDrawIndexedInstancedIndirect)(c, b, o);
}
void STDMETHODCALLTYPE T_DrawInstancedIndirect(Ctx* c, ID3D11Buffer* b, UINT o) {
  Work(sDrawInstancedIndirect);
  O<decltype(&T_DrawInstancedIndirect)>(sDrawInstancedIndirect)(c, b, o);
}
void STDMETHODCALLTYPE T_DrawAuto(Ctx* c) {
  Work(sDrawAuto);
  O<decltype(&T_DrawAuto)>(sDrawAuto)(c);
}
void STDMETHODCALLTYPE T_Dispatch(Ctx* c, UINT x, UINT y, UINT z) {
  Work(sDispatch);
  O<decltype(&T_Dispatch)>(sDispatch)(c, x, y, z);
}
void STDMETHODCALLTYPE T_DispatchIndirect(Ctx* c, ID3D11Buffer* b, UINT o) {
  Work(sDispatchIndirect);
  O<decltype(&T_DispatchIndirect)>(sDispatchIndirect)(c, b, o);
}
__declspec(noinline) void UploadBody(ID3D11Resource* r, const D3D11_BOX* box, const void* src) {
  OnTcUpload(r, box, src);
  if (Teeing()) TeeUpload(r, box, src);
}
void STDMETHODCALLTYPE T_Update(Ctx* c, ID3D11Resource* r, UINT sub, const D3D11_BOX* box, const void* src, UINT rp,
                                UINT dp) {
  Work(sUpdate);
  __try {
    UploadBody(r, box, src);
  } __except (Filt(GetExceptionCode())) {
    Latch("the focus twin upload tap", g_faultCode);
  }
  O<decltype(&T_Update)>(sUpdate)(c, r, sub, box, src, rp, dp);
}
void STDMETHODCALLTYPE T_Update1(ID3D11DeviceContext1* c, ID3D11Resource* r, UINT sub, const D3D11_BOX* box,
                                 const void* src, UINT rp, UINT dp, UINT fl) {
  Work(sUpdate1);
  __try {
    UploadBody(r, box, src);
  } __except (Filt(GetExceptionCode())) {
    Latch("the focus twin upload tap", g_faultCode);
  }
  O<decltype(&T_Update1)>(sUpdate1)(c, r, sub, box, src, rp, dp, fl);
}
HRESULT STDMETHODCALLTYPE T_Map(Ctx* c, ID3D11Resource* r, UINT s, D3D11_MAP t, UINT f, D3D11_MAPPED_SUBRESOURCE* m) {
  Work(sMap);
  return O<decltype(&T_Map)>(sMap)(c, r, s, t, f, m);
}
void STDMETHODCALLTYPE T_Unmap(Ctx* c, ID3D11Resource* r, UINT s) {
  Work(sUnmap);
  O<decltype(&T_Unmap)>(sUnmap)(c, r, s);
}
void STDMETHODCALLTYPE T_CopyResource(Ctx* c, ID3D11Resource* d, ID3D11Resource* s) {
  Work(sCopyResource);
  O<decltype(&T_CopyResource)>(sCopyResource)(c, d, s);
}
void STDMETHODCALLTYPE T_CopySubresourceRegion(Ctx* c, ID3D11Resource* d, UINT ds, UINT x, UINT y, UINT z,
                                               ID3D11Resource* s, UINT ss, const D3D11_BOX* b) {
  Work(sCopySubresourceRegion);
  O<decltype(&T_CopySubresourceRegion)>(sCopySubresourceRegion)(c, d, ds, x, y, z, s, ss, b);
}
void STDMETHODCALLTYPE T_CopySubresourceRegion1(ID3D11DeviceContext1* c, ID3D11Resource* d, UINT ds, UINT x, UINT y,
                                                UINT z, ID3D11Resource* s, UINT ss, const D3D11_BOX* b, UINT f) {
  Work(sCopySubresourceRegion1);
  O<decltype(&T_CopySubresourceRegion1)>(sCopySubresourceRegion1)(c, d, ds, x, y, z, s, ss, b, f);
}
void STDMETHODCALLTYPE T_CopyStructureCount(Ctx* c, ID3D11Buffer* d, UINT o, ID3D11UnorderedAccessView* s) {
  Work(sCopyStructureCount);
  O<decltype(&T_CopyStructureCount)>(sCopyStructureCount)(c, d, o, s);
}
void STDMETHODCALLTYPE T_ClearRTV(Ctx* c, ID3D11RenderTargetView* v, const FLOAT col[4]) {
  Work(sClearRTV);
  O<decltype(&T_ClearRTV)>(sClearRTV)(c, v, col);
}
void STDMETHODCALLTYPE T_ClearDSV(Ctx* c, ID3D11DepthStencilView* v, UINT f, FLOAT d, UINT8 s) {
  Work(sClearDSV);
  O<decltype(&T_ClearDSV)>(sClearDSV)(c, v, f, d, s);
}
void STDMETHODCALLTYPE T_ClearUAVUint(Ctx* c, ID3D11UnorderedAccessView* v, const UINT x[4]) {
  Work(sClearUAVUint);
  O<decltype(&T_ClearUAVUint)>(sClearUAVUint)(c, v, x);
}
void STDMETHODCALLTYPE T_ClearUAVFloat(Ctx* c, ID3D11UnorderedAccessView* v, const FLOAT x[4]) {
  Work(sClearUAVFloat);
  O<decltype(&T_ClearUAVFloat)>(sClearUAVFloat)(c, v, x);
}
void STDMETHODCALLTYPE T_ClearView(ID3D11DeviceContext1* c, ID3D11View* v, const FLOAT col[4], const D3D11_RECT* r,
                                   UINT n) {
  Work(sClearView);
  O<decltype(&T_ClearView)>(sClearView)(c, v, col, r, n);
}
void STDMETHODCALLTYPE T_GenerateMips(Ctx* c, ID3D11ShaderResourceView* v) {
  Work(sGenerateMips);
  O<decltype(&T_GenerateMips)>(sGenerateMips)(c, v);
}
void STDMETHODCALLTYPE T_Resolve(Ctx* c, ID3D11Resource* d, UINT ds, ID3D11Resource* s, UINT ss, DXGI_FORMAT f) {
  Work(sResolve);
  O<decltype(&T_Resolve)>(sResolve)(c, d, ds, s, ss, f);
}
__declspec(noinline) void Resync(Ctx* c);
void STDMETHODCALLTYPE T_ECL(Ctx* c, ID3D11CommandList* l, BOOL restore) {
  Work(sECL);
  O<decltype(&T_ECL)>(sECL)(c, l, restore);
  SyncCopy();
  if (!restore) FT_GUARD(Resync(c));
}
void STDMETHODCALLTYPE T_ClearState(Ctx* c) {
  Work(sClearState);
  O<decltype(&T_ClearState)>(sClearState)(c);
  FT_GUARD(Resync(c));
}
void STDMETHODCALLTYPE T_Flush(Ctx* c) {
  Work(sFlush);
  O<decltype(&T_Flush)>(sFlush)(c);
  SyncCopy();
}
void STDMETHODCALLTYPE T_Swap(ID3D11DeviceContext1* c, ID3DDeviceContextState* s, ID3DDeviceContextState** p) {
  Work(sSwap);
  O<decltype(&T_Swap)>(sSwap)(c, s, p);
  SyncCopy();
  FT_GUARD(Resync(c));
}
void STDMETHODCALLTYPE T_DiscardResource(ID3D11DeviceContext1* c, ID3D11Resource* r) {
  Work(sDiscardResource);
  O<decltype(&T_DiscardResource)>(sDiscardResource)(c, r);
}
void STDMETHODCALLTYPE T_DiscardView(ID3D11DeviceContext1* c, ID3D11View* v) {
  Work(sDiscardView);
  O<decltype(&T_DiscardView)>(sDiscardView)(c, v);
}
void STDMETHODCALLTYPE T_DiscardView1(ID3D11DeviceContext1* c, ID3D11View* v, const D3D11_RECT* r, UINT n) {
  Work(sDiscardView1);
  O<decltype(&T_DiscardView1)>(sDiscardView1)(c, v, r, n);
}
#define FT_OTHER_RANGE(NAME, SL, T)                                    \
  void STDMETHODCALLTYPE NAME(Ctx* c, UINT a, UINT n, T* const* v) {   \
    OtherBind();                                                       \
    O<decltype(&NAME)>(SL)(c, a, n, v);                                \
  }
FT_OTHER_RANGE(T_GSSetCB, sGSSetCB, ID3D11Buffer)
FT_OTHER_RANGE(T_GSSetSRV, sGSSetSRV, ID3D11ShaderResourceView)
FT_OTHER_RANGE(T_GSSetSmp, sGSSetSmp, ID3D11SamplerState)
FT_OTHER_RANGE(T_HSSetCB, sHSSetCB, ID3D11Buffer)
FT_OTHER_RANGE(T_HSSetSRV, sHSSetSRV, ID3D11ShaderResourceView)
FT_OTHER_RANGE(T_HSSetSmp, sHSSetSmp, ID3D11SamplerState)
FT_OTHER_RANGE(T_DSSetCB, sDSSetCB, ID3D11Buffer)
FT_OTHER_RANGE(T_DSSetSRV, sDSSetSRV, ID3D11ShaderResourceView)
FT_OTHER_RANGE(T_DSSetSmp, sDSSetSmp, ID3D11SamplerState)
FT_OTHER_RANGE(T_CSSetCB, sCSSetCB, ID3D11Buffer)
FT_OTHER_RANGE(T_CSSetSRV, sCSSetSRV, ID3D11ShaderResourceView)
FT_OTHER_RANGE(T_CSSetSmp, sCSSetSmp, ID3D11SamplerState)
#undef FT_OTHER_RANGE
void STDMETHODCALLTYPE T_CSSetShader(Ctx* c, ID3D11ComputeShader* s, ID3D11ClassInstance* const* ci, UINT n) {
  OtherBind();
  O<decltype(&T_CSSetShader)>(sCSSetShader)(c, s, ci, n);
}
void STDMETHODCALLTYPE T_CSSetUAV(Ctx* c, UINT a, UINT n, ID3D11UnorderedAccessView* const* u, const UINT* ic) {
  O<decltype(&T_CSSetUAV)>(sCSSetUAV)(c, a, n, u, ic);
  OtherBind();
  FT_GUARD(ResyncSrv(c));  // UAVs unbind conflicting inputs
}
void STDMETHODCALLTYPE T_SOSetTargets(Ctx* c, UINT n, ID3D11Buffer* const* b, const UINT* o) {
  O<decltype(&T_SOSetTargets)>(sSOSetTargets)(c, n, b, o);
  OtherBind();
  FT_GUARD(Resync(c));  // may unbind vertex / index buffers
}
void STDMETHODCALLTYPE T_SetPredication(Ctx* c, ID3D11Predicate* p, BOOL v) {
  OtherBind();
  if (g_win.mode == kWmFocus) TargetsChangeGuarded();  // predication applies to the draws after it
  if (p && Teeing() && g_win.s) g_win.s->usable = false, g_win.s->unusable = 5;
  O<decltype(&T_SetPredication)>(sSetPredication)(c, p, v);
}
#undef FT_GUARD

void* const kTap[sCount] = {
    reinterpret_cast<void*>(&T_VSSetShader), reinterpret_cast<void*>(&T_PSSetShader),
    reinterpret_cast<void*>(&T_GSSetShader), reinterpret_cast<void*>(&T_HSSetShader),
    reinterpret_cast<void*>(&T_DSSetShader), reinterpret_cast<void*>(&T_VSSetCB),
    reinterpret_cast<void*>(&T_PSSetCB), reinterpret_cast<void*>(&T_VSSetCB1),
    reinterpret_cast<void*>(&T_PSSetCB1), reinterpret_cast<void*>(&T_VSSetSRV),
    reinterpret_cast<void*>(&T_PSSetSRV), reinterpret_cast<void*>(&T_VSSetSmp),
    reinterpret_cast<void*>(&T_PSSetSmp), reinterpret_cast<void*>(&T_IASetIL),
    reinterpret_cast<void*>(&T_IASetVB), reinterpret_cast<void*>(&T_IASetIB),
    reinterpret_cast<void*>(&T_IASetTopo), reinterpret_cast<void*>(&T_OMSetBlend),
    reinterpret_cast<void*>(&T_OMSetDSS), reinterpret_cast<void*>(&T_RSSetState),
    reinterpret_cast<void*>(&T_RSSetViewports), reinterpret_cast<void*>(&T_RSSetScissor),
    reinterpret_cast<void*>(&T_OMSetRT), reinterpret_cast<void*>(&T_OMSetRTUAV),
    reinterpret_cast<void*>(&T_Draw), reinterpret_cast<void*>(&T_DrawIndexed),
    reinterpret_cast<void*>(&T_DrawInstanced), reinterpret_cast<void*>(&T_DrawIndexedInstanced),
    reinterpret_cast<void*>(&T_DrawIndexedInstancedIndirect), reinterpret_cast<void*>(&T_DrawInstancedIndirect),
    reinterpret_cast<void*>(&T_DrawAuto), reinterpret_cast<void*>(&T_Dispatch),
    reinterpret_cast<void*>(&T_DispatchIndirect), reinterpret_cast<void*>(&T_Update),
    reinterpret_cast<void*>(&T_Update1), reinterpret_cast<void*>(&T_Map),
    reinterpret_cast<void*>(&T_Unmap), reinterpret_cast<void*>(&T_CopyResource),
    reinterpret_cast<void*>(&T_CopySubresourceRegion), reinterpret_cast<void*>(&T_CopySubresourceRegion1),
    reinterpret_cast<void*>(&T_CopyStructureCount), reinterpret_cast<void*>(&T_ClearRTV),
    reinterpret_cast<void*>(&T_ClearDSV), reinterpret_cast<void*>(&T_ClearUAVUint),
    reinterpret_cast<void*>(&T_ClearUAVFloat), reinterpret_cast<void*>(&T_ClearView),
    reinterpret_cast<void*>(&T_GenerateMips), reinterpret_cast<void*>(&T_Resolve),
    reinterpret_cast<void*>(&T_ECL), reinterpret_cast<void*>(&T_ClearState),
    reinterpret_cast<void*>(&T_Flush), reinterpret_cast<void*>(&T_Swap),
    reinterpret_cast<void*>(&T_DiscardResource), reinterpret_cast<void*>(&T_DiscardView),
    reinterpret_cast<void*>(&T_DiscardView1), reinterpret_cast<void*>(&T_GSSetCB),
    reinterpret_cast<void*>(&T_GSSetSRV), reinterpret_cast<void*>(&T_GSSetSmp),
    reinterpret_cast<void*>(&T_HSSetCB), reinterpret_cast<void*>(&T_HSSetSRV),
    reinterpret_cast<void*>(&T_HSSetSmp), reinterpret_cast<void*>(&T_DSSetCB),
    reinterpret_cast<void*>(&T_DSSetSRV), reinterpret_cast<void*>(&T_DSSetSmp),
    reinterpret_cast<void*>(&T_CSSetShader), reinterpret_cast<void*>(&T_CSSetCB),
    reinterpret_cast<void*>(&T_CSSetSRV), reinterpret_cast<void*>(&T_CSSetSmp),
    reinterpret_cast<void*>(&T_CSSetUAV), reinterpret_cast<void*>(&T_SOSetTargets),
    reinterpret_cast<void*>(&T_SetPredication)};

// The whole tracked state again from the device (ClearState, Swap, ECL(FALSE), SO targets).
void Resync(Ctx* c) {
  St t;
  GetState(c, t, nullptr);  // references released at once (the shadow holds none)
  Shadow& sh = g_win.sh;
  sh.st = t;
  sh.h = Shadow::HashAll(t);
  sh.dirty.All();
}

// ---------------------------------------------------------------------------
// Item descriptors (DCS memory, R123 2.3): everything MetaMaterial::render's result depends on besides the per-view
// constants, by content (a per-view copy of the same RenderItem matches; the item's address is not part of it).
// RenderItem: +0x08 material, +0x24 / +0x58 ints, +0x48 / +0x50 parameter pointers (render 0xb9b12-0xb9ba9), +0xa8
// the MetaShaderRenderItem {p0, four ints, params vector of 16-byte entries} (metaShader 0xe2a0), +0xd8 its pass int.
// p0's stream vector [p0+8, p0+0x10) of 16-byte {object, handle}, object+8 the vertex buffer (GeometryBinder
// 0xc3400). The render context (vt[8]'s third argument): what TerrainRenderable slot 5 wrote for this entry before
// the call (0x12cdac-0x12ceb4): the world matrix relative to the camera (+0x98, 64 bytes; the same camera in both
// views of an eye), the height range (+0x358, 8 bytes) and the entry's +0x60 (+0x378). A per-view item (another
// geometry, other streams) gets another descriptor.
// A fast 64-bit mix per qword (the descriptor is an equality key, ~60 qwords per item, read twice per frame).
inline uint64_t Q(uint64_t h, uint64_t v) {
  h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
  return h * 0xff51afd7ed558ccdull;
}
inline uint64_t QSpan(uint64_t h, const uint8_t* p, size_t qwords) {
  const uint64_t* q = reinterpret_cast<const uint64_t*>(p);
  for (size_t i = 0; i < qwords; ++i) h = Q(h, q[i]);
  return h;
}
// *out: the matching key (with the context block, per frame); *key: the item alone (stable across frames: the
// island rule's key).
bool ReadDescRaw(void* mat, void* item, void* ctx, uint64_t* out, uint64_t* key) {
  const uint8_t* it = static_cast<const uint8_t*>(item);
  uint64_t h = Q(0xcbf29ce484222325ull, U(mat));
  h = Q(h, *reinterpret_cast<const uint32_t*>(it + 0x24));
  h = Q(h, *reinterpret_cast<const uint64_t*>(it + 0x48));
  h = Q(h, *reinterpret_cast<const uint32_t*>(it + 0x58));
  h = Q(h, *reinterpret_cast<const uint32_t*>(it + 0xd8));
  const uint8_t* p50 = *reinterpret_cast<const uint8_t* const*>(it + 0x50);
  if (p50) h = QSpan(h, p50, 1);
  const uint8_t* msri = it + 0xa8;
  h = QSpan(h, msri + 8, 2);  // its four ints
  const uint8_t* pb = *reinterpret_cast<const uint8_t* const*>(msri + 0x18);
  const uint8_t* pe = *reinterpret_cast<const uint8_t* const*>(msri + 0x20);
  if (pb || pe) {
    if (pe < pb || pe - pb > 64 * 16 || (pe - pb) % 16) return false;
    h = QSpan(h, pb, static_cast<size_t>(pe - pb) / 8);
  }
  const uint8_t* geo = *reinterpret_cast<const uint8_t* const*>(msri);
  h = Q(h, U(geo));
  if (geo) {
    const uint8_t* b = *reinterpret_cast<const uint8_t* const*>(geo + 8);
    const uint8_t* e = *reinterpret_cast<const uint8_t* const*>(geo + 0x10);
    if (b || e) {
      if (e < b || e - b > 32 * 16 || (e - b) % 16) return false;
      for (const uint8_t* p = b; p < e; p += 16) {
        const uint8_t* obj = *reinterpret_cast<const uint8_t* const*>(p);
        h = Q(Q(h, U(obj)), *reinterpret_cast<const uint64_t*>(p + 8));
        if (obj) h = QSpan(h, obj + 8, 1);
      }
    }
  }
  if (!ctx) return false;
  *key = h | 1;
  const uint8_t* c = static_cast<const uint8_t*>(ctx);
  h = QSpan(h, c + 0x98, 8);
  h = Q(Q(h, *reinterpret_cast<const uint64_t*>(c + 0x358)), *reinterpret_cast<const uint64_t*>(c + 0x378));
  *out = h | 1;
  return true;
}
bool ReadDesc(void* mat, void* item, void* ctx, uint64_t* out, uint64_t* key) {
  __try {
    return ReadDescRaw(mat, item, ctx, out, key);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}
// Tests: another descriptor reader (nullptr = DCS's layout).
bool (*g_descFn)(void* mat, void* item, void* ctx, uint64_t* out, uint64_t* key) = nullptr;
inline bool Desc(void* mat, void* item, void* ctx, uint64_t* out, uint64_t* key) {
  return g_descFn ? g_descFn(mat, item, ctx, out, key) : ReadDesc(mat, item, ctx, out, key);
}

// R125: MetaMaterial_Instancer2 items (vt[8] 0xb4180, docs/research/R125_focus_twin_instancer.md section 2). What the
// call reads besides the per-view constants, and the view's sub-draw bits:
//  - iv = [ctx + 0x370] (the view's instancer context): v = iv->vt[0]() (index), iv->vt[8]() (technique argument);
//  - the material's [mat + 8 + 8 v] (-1: nothing drawn), the item's type t = [item + 0x108 + 4 v] (< 0: nothing);
//  - the item's MetaShaderRenderItem at +0xa8 (geometry p0, four ints +0xb0..+0xbc, params vector), as a patch's;
//  - IM2 = G->vt[11]() and X = G->vt[13]() of the terrain global G: IM2's type entries e(t, s) = [IM2 + 0xe0] + 16
//    (3 t + s) for s = 0, 1 (bit index +0, the per-draw int +8, the argument offset +0xc), its buffer members
//    (+0x2f0 ... +0x370) and [X + 0xc8] (sub-draw 1's buffer);
//  - bit(s) = [[IM2 + 0x210]][i >> 5] >> (i & 31) & 1, i = e(t, s).+0 (IM2 vt[30]): the sub-draws drawn in this view.
// The bits are not part of the descriptor (the twin enables sub-draws per view); everything else is.
using VFn0 = uint64_t(__fastcall*)(void*);
inline uint64_t VCall(void* obj, int slot) { return reinterpret_cast<VFn0>((*static_cast<void***>(obj))[slot])(obj); }
bool ReadInstDescRaw(void* mat, void* item, void* ctx, uint64_t* out, uint64_t* key, uint8_t* bits) {
  if (!g_instGlobal || !ctx) return false;
  const uint8_t* it = static_cast<const uint8_t*>(item);
  const uint8_t* c = static_cast<const uint8_t*>(ctx);
  void* iv = *reinterpret_cast<void* const*>(c + 0x370);
  if (!iv) return false;
  const int v = static_cast<int>(static_cast<uint32_t>(VCall(iv, 0)));
  if (v < 0 || v >= 64) return false;
  const uint64_t tech = VCall(iv, 8);
  const uint64_t matTech = *reinterpret_cast<const uint64_t*>(static_cast<const uint8_t*>(mat) + 8 + 8 * v);
  const int t = *reinterpret_cast<const int32_t*>(it + 0x108 + 4 * v);
  if (t < 0 || matTech == ~0ull) return false;  // DCS draws nothing (vt[6] would not have called vt[8] either)
  void* g = *g_instGlobal;
  if (!g) return false;
  const uint8_t* im2 = reinterpret_cast<const uint8_t*>(VCall(g, 11));
  if (!im2 || (g_im2Vtbl && *reinterpret_cast<void* const*>(im2) != g_im2Vtbl)) return false;
  const uint8_t* x = reinterpret_cast<const uint8_t*>(VCall(g, 13));
  const uint8_t* types = *reinterpret_cast<const uint8_t* const*>(im2 + 0xe0);
  const uint32_t* const* bitsPtr = *reinterpret_cast<const uint32_t* const* const*>(im2 + 0x210);
  if (!types || !bitsPtr || !*bitsPtr) return false;
  const uint8_t* e0 = types + 16 * (3 * static_cast<size_t>(t));
  uint8_t b = 0;
  for (int s = 0; s < 2; ++s) {
    const int32_t i = *reinterpret_cast<const int32_t*>(e0 + 16 * s);
    if (i < 0) return false;
    if (((*bitsPtr)[static_cast<uint32_t>(i) >> 5] >> (static_cast<uint32_t>(i) & 31u)) & 1u) b |= static_cast<uint8_t>(1u << s);
  }
  uint64_t h = Q(0x84222325cbf29ce4ull, U(mat));
  h = Q(Q(Q(Q(h, static_cast<uint64_t>(v)), tech), matTech), static_cast<uint64_t>(static_cast<uint32_t>(t)));
  h = QSpan(h, e0, 4);  // both entries (16 bytes each)
  for (uint32_t m : kIm2Members) h = Q(h, *reinterpret_cast<const uint64_t*>(im2 + m));
  h = Q(h, x ? *reinterpret_cast<const uint64_t*>(x + 0xc8) : 0);
  const uint8_t* msri = it + 0xa8;
  h = QSpan(h, msri + 8, 2);  // its four ints (+0xb0 the draw's int, +0xb4 the IM2 slot, +0xb8 non-zero)
  const uint8_t* pb = *reinterpret_cast<const uint8_t* const*>(msri + 0x18);
  const uint8_t* pe = *reinterpret_cast<const uint8_t* const*>(msri + 0x20);
  if (pb || pe) {
    if (pe < pb || pe - pb > 64 * 16 || (pe - pb) % 16) return false;
    h = QSpan(h, pb, static_cast<size_t>(pe - pb) / 8);
  }
  const uint8_t* geo = *reinterpret_cast<const uint8_t* const*>(msri);
  h = Q(h, U(geo));
  if (geo) {
    const uint8_t* gb = *reinterpret_cast<const uint8_t* const*>(geo + 8);
    const uint8_t* ge = *reinterpret_cast<const uint8_t* const*>(geo + 0x10);
    if (gb || ge) {
      if (ge < gb || ge - gb > 32 * 16 || (ge - gb) % 16) return false;
      for (const uint8_t* p = gb; p < ge; p += 16) {
        const uint8_t* obj = *reinterpret_cast<const uint8_t* const*>(p);
        h = Q(Q(h, U(obj)), *reinterpret_cast<const uint64_t*>(p + 8));
        if (obj) h = QSpan(h, obj + 8, 1);
      }
    }
  }
  *key = h | 1;
  h = QSpan(h, c + 0x98, 8);  // what slot 5 wrote for this entry (as for patches)
  h = Q(Q(h, *reinterpret_cast<const uint64_t*>(c + 0x358)), *reinterpret_cast<const uint64_t*>(c + 0x378));
  *out = h | 1;
  *bits = b;
  return true;
}
bool ReadInstDesc(void* mat, void* item, void* ctx, uint64_t* out, uint64_t* key, uint8_t* bits) {
  __try {
    return ReadInstDescRaw(mat, item, ctx, out, key, bits);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

// ---------------------------------------------------------------------------
// Streams
// ---------------------------------------------------------------------------
void ResetStream(Stream& s) {
  for (IUnknown* p : s.refs) p->Release();
  s.refs.clear();
  s.d.clear(), s.rec.clear(), s.up.clear(), s.blob.clear(), s.item.clear(), s.dupDesc.clear();
  s.writes.clear(), s.inherited.clear();
  memset(&s.base, 0, sizeof(s.base));
  for (int k = 0; k < kPerView; ++k) s.pv[k] = nullptr, s.pvSet[k] = false, s.pvFirst[k] = s.pvNum[k] = 0;
  s.tcBuf = nullptr;
  s.argBuf = nullptr;
  s.usable = false, s.unusable = 0;
  s.pred = Pred();
  if (!s.map) s.map = new DescMap();
  s.map->Clear();
  ReleaseTargets(s.targets);
  s.eye = -1, s.ciSeq = 0, s.frame = 0;
}
void ReleaseChunks(Stream& s, bool keepArgs) {
  if (!keepArgs) {
    for (ID3D11Buffer*& b : s.upCb)
      if (b) b->Release(), b = nullptr;
    s.upCb.clear(), s.upCbBytes.clear();
  }
  for (Chunk& c : s.chunks) {
    if (c.cl) c.cl->Release(), c.cl = nullptr;
    if (!keepArgs) {
      if (c.args) c.args->Release(), c.args = nullptr, c.argsCap = 0;
      if (c.argsUav) c.argsUav->Release(), c.argsUav = nullptr;
      if (c.mapSrv) c.mapSrv->Release(), c.mapSrv = nullptr;
      if (c.map) c.map->Release(), c.map = nullptr, c.mapCap = 0;
      c.argsUavable = false;
    }
    c.any = c.ind = false, c.count = 0;
  }
  if (!keepArgs) {
    s.chunks.clear();
    if (s.stageSrv) s.stageSrv->Release(), s.stageSrv = nullptr;
    if (s.stage) s.stage->Release(), s.stage = nullptr, s.stageCap = 0;
  }
}

// ---------------------------------------------------------------------------
// The worker: command lists from a stream
// ---------------------------------------------------------------------------
struct Emit {
  ID3D11DeviceContext* dc;
  ID3D11DeviceContext1* dc1;
  Eye* e;
  uint32_t ringMask[2];  // slots bound to ring slices by the previous record
};
inline void BindCb(Emit& em, int st, UINT slot, ID3D11Buffer* b, UINT first, UINT num) {
  if (num && em.dc1) {
    if (st == 0)
      em.dc1->VSSetConstantBuffers1(slot, 1, &b, &first, &num);
    else
      em.dc1->PSSetConstantBuffers1(slot, 1, &b, &first, &num);
  } else {
    if (st == 0)
      em.dc->VSSetConstantBuffers(slot, 1, &b);
    else
      em.dc->PSSetConstantBuffers(slot, 1, &b);
  }
}
inline void EmitCbSlot(Emit& em, const St& t, const Stream& s, int st, UINT i) {
  const int pv = PerViewIndex(i);
  if (pv >= 0) {
    if (s.pvSet[pv] && em.e->pvCopy[pv]) BindCb(em, st, i, em.e->pvCopy[pv], s.pvFirst[pv], s.pvNum[pv]);
    return;
  }
  BindCb(em, st, i, t.cb[st][i], t.cbFirst[st][i], t.cbNum[st][i]);
}
void EmitFull(Emit& em, const St& t, const Stream& s) {
  ID3D11DeviceContext* c = em.dc;
  const Targets& tg = s.targets;
  c->OMSetRenderTargets(tg.nrtv, tg.nrtv ? tg.rtv : nullptr, tg.dsv);
  if (tg.nvp) c->RSSetViewports(tg.nvp, tg.vp);
  if (tg.nsc) c->RSSetScissorRects(tg.nsc, tg.sc);
  c->RSSetState(static_cast<ID3D11RasterizerState*>(t.rs));
  c->OMSetDepthStencilState(static_cast<ID3D11DepthStencilState*>(t.dss), t.sref);
  c->OMSetBlendState(static_cast<ID3D11BlendState*>(t.bs), t.bf, t.smask);
  c->IASetPrimitiveTopology(static_cast<D3D11_PRIMITIVE_TOPOLOGY>(t.topo));
  c->IASetInputLayout(static_cast<ID3D11InputLayout*>(t.il));
  c->IASetVertexBuffers(0, kVb, t.vb, t.vbStride, t.vbOff);
  c->IASetIndexBuffer(t.ib, static_cast<DXGI_FORMAT>(t.ibFmt), t.ibOff);
  c->VSSetShader(static_cast<ID3D11VertexShader*>(t.vs), nullptr, 0);
  c->PSSetShader(static_cast<ID3D11PixelShader*>(t.ps), nullptr, 0);
  for (int st = 0; st < 2; ++st)
    for (UINT i = 0; i < kCb; ++i) EmitCbSlot(em, t, s, st, i);
  c->VSSetShaderResources(0, kSrv, t.srv[0]);
  c->PSSetShaderResources(0, kSrv, t.srv[1]);
  c->VSSetSamplers(0, kSmp, t.smp[0]);
  c->PSSetSamplers(0, kSmp, t.smp[1]);
  em.ringMask[0] = em.ringMask[1] = 0;
}
// Applies record r's deltas to t; emits them when `emit`.
void ApplyDeltas(Emit* em, St& t, const Stream& s, const Rec& r) {
  const uint64_t* p = s.d.data() + r.dOff;
  const uint64_t* end = s.d.data() + r.dEnd;
  while (p < end) {
    const uint64_t hd = *p++;
    const uint32_t op = static_cast<uint32_t>(hd & 0xff);
    const int st = static_cast<int>((hd >> 8) & 0xff);
    const UINT lo = static_cast<UINT>((hd >> 16) & 0xffff), hi = static_cast<UINT>((hd >> 32) & 0xffff);
    switch (op) {
      case dVS:
        t.vs = reinterpret_cast<void*>(*p++);
        if (em) em->dc->VSSetShader(static_cast<ID3D11VertexShader*>(t.vs), nullptr, 0);
        break;
      case dPS:
        t.ps = reinterpret_cast<void*>(*p++);
        if (em) em->dc->PSSetShader(static_cast<ID3D11PixelShader*>(t.ps), nullptr, 0);
        break;
      case dIL:
        t.il = reinterpret_cast<void*>(*p++);
        if (em) em->dc->IASetInputLayout(static_cast<ID3D11InputLayout*>(t.il));
        break;
      case dRS:
        t.rs = reinterpret_cast<void*>(*p++);
        if (em) em->dc->RSSetState(static_cast<ID3D11RasterizerState*>(t.rs));
        break;
      case dTopo:
        t.topo = static_cast<UINT>(*p++);
        if (em) em->dc->IASetPrimitiveTopology(static_cast<D3D11_PRIMITIVE_TOPOLOGY>(t.topo));
        break;
      case dIB:
        t.ib = reinterpret_cast<ID3D11Buffer*>(*p++);
        t.ibFmt = static_cast<UINT>(*p >> 32), t.ibOff = static_cast<UINT>(*p & 0xffffffffu), ++p;
        if (em) em->dc->IASetIndexBuffer(t.ib, static_cast<DXGI_FORMAT>(t.ibFmt), t.ibOff);
        break;
      case dDSS:
        t.dss = reinterpret_cast<void*>(*p++);
        t.sref = static_cast<UINT>(*p++);
        if (em) em->dc->OMSetDepthStencilState(static_cast<ID3D11DepthStencilState*>(t.dss), t.sref);
        break;
      case dBS:
        t.bs = reinterpret_cast<void*>(*p++);
        memcpy(t.bf, p, 8), memcpy(t.bf + 2, p + 1, 8), p += 2;
        t.smask = static_cast<UINT>(*p++);
        if (em) em->dc->OMSetBlendState(static_cast<ID3D11BlendState*>(t.bs), t.bf, t.smask);
        break;
      case dVB:
        for (UINT i = lo; i < hi; ++i) {
          t.vb[i] = reinterpret_cast<ID3D11Buffer*>(*p++);
          t.vbStride[i] = static_cast<UINT>(*p >> 32), t.vbOff[i] = static_cast<UINT>(*p & 0xffffffffu), ++p;
        }
        if (em) em->dc->IASetVertexBuffers(lo, hi - lo, t.vb + lo, t.vbStride + lo, t.vbOff + lo);
        break;
      case dCB:
        for (UINT i = lo; i < hi; ++i) {
          t.cb[st][i] = reinterpret_cast<ID3D11Buffer*>(*p++);
          t.cbFirst[st][i] = static_cast<UINT>(*p >> 32), t.cbNum[st][i] = static_cast<UINT>(*p & 0xffffffffu), ++p;
          if (em) EmitCbSlot(*em, t, s, st, i);
        }
        break;
      case dSRV:
        for (UINT i = lo; i < hi; ++i) t.srv[st][i] = reinterpret_cast<ID3D11ShaderResourceView*>(*p++);
        if (em) {
          if (st == 0)
            em->dc->VSSetShaderResources(lo, hi - lo, t.srv[0] + lo);
          else
            em->dc->PSSetShaderResources(lo, hi - lo, t.srv[1] + lo);
        }
        break;
      case dSMP:
        for (UINT i = lo; i < hi; ++i) t.smp[st][i] = reinterpret_cast<ID3D11SamplerState*>(*p++);
        if (em) {
          if (st == 0)
            em->dc->VSSetSamplers(lo, hi - lo, t.smp[0] + lo);
          else
            em->dc->PSSetSamplers(lo, hi - lo, t.smp[1] + lo);
        }
        break;
      default:
        return;
    }
  }
}

struct Job {
  Stream* s = nullptr;
  Eye* e = nullptr;
  int chunk = 64;
};
Job g_job[2];

bool EnsureBuffer(ID3D11Buffer*& b, UINT& cap, UINT bytes, UINT bind, UINT misc) {
  if (b && cap >= bytes) return true;
  if (b) b->Release(), b = nullptr;
  D3D11_BUFFER_DESC d{};
  d.ByteWidth = bytes;
  d.Usage = D3D11_USAGE_DEFAULT;
  d.BindFlags = bind;
  d.MiscFlags = misc;
  cap = 0;
  if (FAILED(g_dev->CreateBuffer(&d, nullptr, &b)) || !b) return false;
  cap = bytes;
  return true;
}

// R125: the start of a list holding instancer records: DCS's argument words (the span of this list's records) copied
// into the stream's stage buffer, then the gate pass: an enabled record (word 1 set by the flush) gets DCS's five
// words, any other zero. The list runs right before the replaced items' place in the focus pass (nothing of DCS's in
// between), so the words are the ones DCS's own draws would read: the focus view's culling chain's.
bool BuildGate(ID3D11DeviceContext* dc, Stream& s, Chunk& ch, int chunk) {
  D3D11_BUFFER_DESC ad{};
  s.argBuf->GetDesc(&ad);
  UINT lo = ~0u, hi = 0;
  for (int i = ch.first; i < ch.first + ch.count; ++i) {
    const Rec& r = s.rec[static_cast<size_t>(i)];
    if (!r.argBuf || s.item[static_cast<size_t>(r.item)].why != kPOk) continue;
    lo = std::min(lo, r.argOff & ~15u);
    hi = std::max(hi, r.argOff + 20u);
  }
  hi = std::min((hi + 15u) & ~15u, ad.ByteWidth);
  if (lo >= hi) return false;
  // the stage copy (and its view) of [lo, hi)
  ID3D11Buffer* stageBefore = s.stage;
  if (!EnsureBuffer(s.stage, s.stageCap, (hi - lo + 15u) & ~15u, D3D11_BIND_SHADER_RESOURCE, 0)) return false;
  if (s.stage != stageBefore && s.stageSrv) s.stageSrv->Release(), s.stageSrv = nullptr;
  if (!s.stageSrv) {
    D3D11_SHADER_RESOURCE_VIEW_DESC vd{};
    vd.Format = DXGI_FORMAT_R32_UINT;
    vd.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
    vd.Buffer.FirstElement = 0, vd.Buffer.NumElements = s.stageCap / 4u;
    if (FAILED(g_dev->CreateShaderResourceView(s.stage, &vd, &s.stageSrv)) || !s.stageSrv) return false;
  }
  // the map: per record of the list, its first word in the stage (~0u: not an enabled-able instancer record)
  ID3D11Buffer* mapBefore = ch.map;
  if (!EnsureBuffer(ch.map, ch.mapCap, static_cast<UINT>(chunk) * 4u, D3D11_BIND_SHADER_RESOURCE, 0)) return false;
  if (ch.map != mapBefore && ch.mapSrv) ch.mapSrv->Release(), ch.mapSrv = nullptr;
  if (!ch.mapSrv) {
    D3D11_SHADER_RESOURCE_VIEW_DESC vd{};
    vd.Format = DXGI_FORMAT_R32_UINT;
    vd.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
    vd.Buffer.FirstElement = 0, vd.Buffer.NumElements = ch.mapCap / 4u;
    if (FAILED(g_dev->CreateShaderResourceView(ch.map, &vd, &ch.mapSrv)) || !ch.mapSrv) return false;
  }
  if (!ch.argsUav) {
    D3D11_UNORDERED_ACCESS_VIEW_DESC ud{};
    ud.Format = DXGI_FORMAT_R32_UINT;
    ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    ud.Buffer.FirstElement = 0, ud.Buffer.NumElements = ch.argsCap / 4u;
    if (FAILED(g_dev->CreateUnorderedAccessView(ch.args, &ud, &ch.argsUav)) || !ch.argsUav) return false;
  }
  std::vector<uint32_t> map(ch.mapCap / 4u, ~0u);
  for (int i = ch.first; i < ch.first + ch.count; ++i) {
    const Rec& r = s.rec[static_cast<size_t>(i)];
    if (r.argBuf && s.item[static_cast<size_t>(r.item)].why == kPOk) map[static_cast<size_t>(i - ch.first)] = (r.argOff - lo) / 4u;
  }
  const D3D11_BOX box{lo, 0, 0, hi, 1, 1};
  dc->CopySubresourceRegion(s.stage, 0, 0, 0, 0, s.argBuf, 0, &box);
  dc->UpdateSubresource(ch.map, 0, nullptr, map.data(), 0, 0);
  dc->CSSetShader(g_gateCs, nullptr, 0);
  ID3D11ShaderResourceView* sv[2] = {s.stageSrv, ch.mapSrv};
  dc->CSSetShaderResources(0, 2, sv);
  dc->CSSetUnorderedAccessViews(0, 1, &ch.argsUav, nullptr);
  dc->Dispatch((static_cast<UINT>(ch.count) + 63u) / 64u, 1, 1);
  ID3D11UnorderedAccessView* nu = nullptr;
  ID3D11ShaderResourceView* ns[2] = {};
  dc->CSSetUnorderedAccessViews(0, 1, &nu, nullptr);
  dc->CSSetShaderResources(0, 2, ns);
  dc->CSSetShader(nullptr, nullptr, 0);
  return true;
}

// Worker body: one command list per chunk. False: nothing usable (the focus pass is stock).
bool BuildJob(defrec::Worker& w, void* user) {
  Job& j = *static_cast<Job*>(user);
  Stream& s = *j.s;
  Eye& e = *j.e;
  const int64_t t0 = defrec::Qpc();
  bool ok = s.usable && !s.rec.empty() && s.targets.valid;
  // Our per-view copies (the eye's; the size of the buffers the periphery bound).
  for (int k = 0; ok && k < kPerView; ++k) {
    if (!s.pvSet[k]) continue;
    D3D11_BUFFER_DESC bd{};
    s.pv[k]->GetDesc(&bd);
    if (e.pvCopy[k] && e.pvBytes[k] != bd.ByteWidth) e.pvCopy[k]->Release(), e.pvCopy[k] = nullptr;
    if (!e.pvCopy[k]) {
      D3D11_BUFFER_DESC d{};
      d.ByteWidth = bd.ByteWidth;
      d.Usage = D3D11_USAGE_DEFAULT;
      d.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
      if (FAILED(g_dev->CreateBuffer(&d, nullptr, &e.pvCopy[k])) || !e.pvCopy[k]) ok = false;
      e.pvBytes[k] = bd.ByteWidth;
    }
  }
  const int n = static_cast<int>(s.rec.size());
  const int chunk = j.chunk < 8 ? 8 : j.chunk;
  const int nChunks = (n + chunk - 1) / chunk;
  if (ok) {
    s.args.assign(static_cast<size_t>(nChunks) * static_cast<size_t>(chunk) * 5, 0u);
    for (int i = 0; i < n; ++i) {
      uint32_t* a = &s.args[static_cast<size_t>(i) * 5];
      if (s.rec[static_cast<size_t>(i)].argBuf) continue;  // R125: the gate pass writes an instancer record's words
      a[0] = s.rec[static_cast<size_t>(i)].idxCount, a[1] = 0, a[2] = s.rec[static_cast<size_t>(i)].startIdx;
      a[3] = static_cast<uint32_t>(s.rec[static_cast<size_t>(i)].baseVtx), a[4] = 0;
    }
    if (static_cast<int>(s.chunks.size()) < nChunks) s.chunks.resize(static_cast<size_t>(nChunks));
  }
  St t = s.base;
  Emit em{w.dc, w.dc1, &e, {0, 0}};
  for (int c = 0; ok && c < nChunks; ++c) {
    Chunk& ch = s.chunks[static_cast<size_t>(c)];
    ch.first = c * chunk;
    ch.count = std::min(chunk, n - ch.first);
    ch.any = false;
    for (int i = ch.first; i < ch.first + ch.count; ++i)
      if (s.item[static_cast<size_t>(s.rec[static_cast<size_t>(i)].item)].why == kPOk) ch.any = true;
    if (!ch.any) {  // keep the running state right, record nothing
      for (int i = ch.first; i < ch.first + ch.count; ++i) ApplyDeltas(nullptr, t, s, s.rec[static_cast<size_t>(i)]);
      if (ch.cl) ch.cl->Release(), ch.cl = nullptr;
      continue;
    }
    ch.ind = false;
    for (int i = ch.first; i < ch.first + ch.count; ++i) {
      const Rec& r = s.rec[static_cast<size_t>(i)];
      if (r.argBuf && s.item[static_cast<size_t>(r.item)].why == kPOk) ch.ind = true;
    }
    if (ch.ind && (!g_gateCs || !s.argBuf)) {
      ok = false;
      break;
    }
    // R125: with the instancer part installed every list's argument buffer is also a UAV (the gate pass's output).
    const bool uavable = g_gateCs != nullptr;
    if (ch.args && ch.argsUavable != uavable) {
      ch.args->Release(), ch.args = nullptr, ch.argsCap = 0;
      if (ch.argsUav) ch.argsUav->Release(), ch.argsUav = nullptr;
    }
    ID3D11Buffer* argsBefore = ch.args;
    if (!EnsureBuffer(ch.args, ch.argsCap, static_cast<UINT>(chunk) * 20u, uavable ? D3D11_BIND_UNORDERED_ACCESS : 0u,
                      D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS)) {
      ok = false;
      break;
    }
    ch.argsUavable = uavable;
    if (ch.args != argsBefore && ch.argsUav) ch.argsUav->Release(), ch.argsUav = nullptr;
    if (ch.ind && !BuildGate(w.dc, s, ch, chunk)) {
      ok = false;
      break;
    }
    EmitFull(em, t, s);
    for (int i = ch.first; i < ch.first + ch.count; ++i) {
      const Rec& r = s.rec[static_cast<size_t>(i)];
      ApplyDeltas(&em, t, s, r);
      // Slots the previous record bound to ring slices: back to the running state unless this record uploads them.
      uint32_t now[2] = {0, 0};
      for (uint32_t u = r.upFirst; u < r.upFirst + r.upCount; ++u) now[s.up[u].stage] |= 1u << s.up[u].slot;
      for (int st = 0; st < 2; ++st) {
        uint32_t m = em.ringMask[st] & ~now[st];
        while (m) {
          unsigned long k;
          _BitScanForward(&k, m);
          m &= m - 1;
          EmitCbSlot(em, t, s, st, k);
        }
      }
      const bool twin = s.item[static_cast<size_t>(r.item)].why == kPOk;
      if (twin) {
        // The item's uploaded bytes into this upload's own buffer (in the list), bound plainly; the same upload in
        // both stages shares one buffer.
        for (uint32_t u = r.upFirst; u < r.upFirst + r.upCount; ++u) {
          const Up& up = s.up[u];
          uint32_t same = u;
          for (uint32_t q = r.upFirst; q < u; ++q)
            if (s.up[q].off == up.off) same = q;
          if (s.upCb.size() < s.up.size()) s.upCb.resize(s.up.size(), nullptr), s.upCbBytes.resize(s.up.size(), 0);
          ID3D11Buffer*& cb = s.upCb[same];
          if (same == u) {
            const UINT bytes = (up.len + 15u) & ~15u;
            if (!EnsureBuffer(cb, s.upCbBytes[u], bytes, D3D11_BIND_CONSTANT_BUFFER, 0)) {
              ok = false;
              break;
            }
            if (bytes == up.len) {
              w.dc->UpdateSubresource(cb, 0, nullptr, s.blob.data() + up.off, 0, 0);
            } else {
              uint8_t tmp[4096] = {};
              if (bytes > sizeof(tmp)) {
                ok = false;
                break;
              }
              memcpy(tmp, s.blob.data() + up.off, up.len);
              w.dc->UpdateSubresource(cb, 0, nullptr, tmp, 0, 0);
            }
          }
          if (up.stage == 0)
            w.dc->VSSetConstantBuffers(up.slot, 1, &cb);
          else
            w.dc->PSSetConstantBuffers(up.slot, 1, &cb);
        }
        em.ringMask[0] = now[0], em.ringMask[1] = now[1];
        w.dc->DrawIndexedInstancedIndirect(ch.args, static_cast<UINT>(i - ch.first) * 20u);
      } else {
        em.ringMask[0] = em.ringMask[1] = 0;
        for (int st = 0; st < 2; ++st)
          for (uint32_t m = now[st]; m;) {
            unsigned long k;
            _BitScanForward(&k, m);
            m &= m - 1;
            EmitCbSlot(em, t, s, st, k);
          }
      }
      if (!ok) break;
    }
    ID3D11CommandList* cl = nullptr;
    const HRESULT hr = w.dc->FinishCommandList(FALSE, &cl);
    if (FAILED(hr) || !cl) {
      ok = false;
      break;
    }
    if (ch.cl) ch.cl->Release();
    ch.cl = cl;
  }
  for (size_t c = static_cast<size_t>(nChunks); c < s.chunks.size(); ++c)  // an earlier frame's extra lists
    if (s.chunks[c].cl) s.chunks[c].cl->Release(), s.chunks[c].cl = nullptr, s.chunks[c].count = 0;
  if (!ok)
    for (Chunk& c : s.chunks)
      if (c.cl) c.cl->Release(), c.cl = nullptr;
  s.buildUs = (defrec::Qpc() - t0) * QpcUs();
  // Everything the lists bind is referenced by the lists now; the stream's references go.
  for (IUnknown* p : s.refs) p->Release();
  s.refs.clear();
  s.job.store(ok ? kJOk : kJFailed, std::memory_order_release);
  return false;  // no list of the pool's own (ours are in the chunks)
}

// ---------------------------------------------------------------------------
// The TerrainContext upload (every window mode): classification at the first one
// ---------------------------------------------------------------------------
void Classify();
void GpuBegin();
bool VerifyCapture(const Targets& tg);
void OnTcUpload(ID3D11Resource* r, const D3D11_BOX* box, const void* src) {
  Win& w = g_win;
  if (!src || box) return;
  D3D11_RESOURCE_DIMENSION dim = D3D11_RESOURCE_DIMENSION_UNKNOWN;
  r->GetType(&dim);
  if (dim != D3D11_RESOURCE_DIMENSION_BUFFER) return;
  D3D11_BUFFER_DESC bd{};
  static_cast<ID3D11Buffer*>(r)->GetDesc(&bd);
  if (!(bd.BindFlags & D3D11_BIND_CONSTANT_BUFFER)) return;
  if (bd.ByteWidth == kB10Bytes && !w.inItem && w.tcUploads == 0) w.b10Buf = static_cast<ID3D11Buffer*>(r);
  if (bd.ByteWidth != kTcBytes) return;
  if (w.inItem) {  // a patch item uploading a TerrainContext: not twinnable
    MarkItem(kPPerView);
    return;
  }
  ++w.tcUploads;
  if (w.mode == kWmPending) {
    memcpy(w.tc, src, kTcBytes);
    w.haveTc = true;
    w.tcBuf = static_cast<ID3D11Buffer*>(r);
    Classify();
    return;
  }
  // A second TerrainContext in the same window: the periphery stream (its records saw two) is not usable; a focus
  // pass stops replacing items from here (our copy holds the first).
  if (memcmp(w.tc, src, kTcBytes) == 0) return;
  if (Teeing() && w.s && w.mode == kWmPeriph) w.s->usable = false, w.s->unusable = 2;
  if (w.mode == kWmFocus) {
    if (!w.pending.empty()) Flush();
    w.stopTwin = true;
  }
}

// ---------------------------------------------------------------------------
// Window begin / classification / end
// ---------------------------------------------------------------------------
bool AuditDue(Eye& e) {
  if (g_shadowKey.load(std::memory_order_relaxed)) return true;
  if (e.trust < g_trustNeeded.load(std::memory_order_relaxed)) return true;
  const int every = g_auditEvery.load(std::memory_order_relaxed);
  return every > 0 && e.sinceAudit >= static_cast<uint64_t>(every);
}
void StartTee(Stream& s, int eye) {
  Win& w = g_win;
  ResetStream(s);
  s.eye = eye;
  s.ciSeq = w.ciSeq;
  GetState(g_tbl.ctx, s.base, &s.refs);
  s.baseHash = Shadow::HashAll(s.base);
  w.sh.st = s.base;
  w.sh.h = s.baseHash;
  w.sh.dirty.Clear();
  memcpy(s.tc, w.tc, kTcBytes);
  s.tcBuf = w.tcBuf;
  s.usable = true;
  w.s = &s;
  w.nwr = 0;
  Eye& e = g_eye[eye];
  w.recAll = &s == &g_audit || AuditDue(e) || g_verifyEvery.load(std::memory_order_relaxed) > 0 || !e.good ||
             e.good->n == 0;
}
// At the first TerrainContext upload of the window: which view, and what this window does.
void Classify() {
  Win& w = g_win;
  ViewId v = ClassifyTc(w.tc);
  if (v.kind == kOther || (v.kind == kPeriph && v.eye < 0)) {
    w.mode = kWmIgnore;
    return;
  }
  if (v.kind == kFocus) {
    // The eye whose periphery pass of this frame has the same shared rows (camera, z / w, terrain parameters).
    int stale = 0;
    for (int i = 0; i < 2; ++i) {
      Eye& ei = g_eye[i];
      if (ei.cur < 0) continue;
      const Stream& si = ei.s[ei.cur];
      if (si.ciSeq != w.ci.seq) continue;
      if (si.unusable == 6) ++stale;
      if (SharedRowsEqual(si.tc, w.tc)) v.eye = i;
    }
    if (v.eye < 0) {  // no quad periphery twin this frame: an MFD zoom, a skipped periphery, a stale frame
      g_st.focusPasses++;
      g_st.why[stale ? kWStale : kWNoPeriph]++;
      w.mode = kWmIgnore;
      return;
    }
  }
  w.eye = v.eye;
  Eye& e = g_eye[v.eye];
  // F10 / map / loading: no culling since this eye's last periphery pass (gb_rec's graph-wide sort guard too).
  if (!w.ci.seq || (gbrec::SortRecorded() && !gbrec::EntryVectorFresh())) {
    g_st.staleSkips++;
    if (v.kind == kFocus) g_st.focusPasses++, g_st.why[kWStale]++;
    w.mode = kWmIgnore;
    return;
  }
  if (v.kind == kPeriph) {
    if (e.lastPeriphCi == w.ci.seq) {
      // No culling since this eye's last periphery pass (F10 / map / paused scene: the cameras were not published
      // again), or a second periphery pass in one frame: neither is used.
      g_st.staleSkips++;
      if (e.cur >= 0) e.s[e.cur].usable = false, e.s[e.cur].unusable = 6;
      w.mode = kWmIgnore;
      return;
    }
    e.lastPeriphCi = w.ci.seq;
    // A stream not in use by a worker.
    int pick = -1;
    for (int i = 0; i < 2; ++i)
      if (i != e.cur && e.s[i].job.load(std::memory_order_acquire) != kJRunning) pick = i;
    if (pick < 0 && e.cur >= 0 && e.s[e.cur].job.load(std::memory_order_acquire) != kJRunning) pick = e.cur;
    if (pick < 0) {
      e.cur = -1;
      w.mode = kWmIgnore;
      return;
    }
    e.cur = pick;
    const int64_t tp = defrec::Qpc();
    StartTee(e.s[pick], v.eye);
    g_st.periphOwnUs += (defrec::Qpc() - tp) * QpcUs();
    g_st.periphPasses++;
    w.mode = kWmPeriph;
    return;
  }
  const int64_t t0 = defrec::Qpc();
  // Focus: the decision waits for the first patch item (the window's own uploads are all in by then).
  GetTargets(g_tbl.ctx, w.tg);
  ID3D11Buffer* cb[kCb] = {};
  g_tbl.ctx->VSGetConstantBuffers(0, kCb, cb);
  for (int k = 0; k < kPerView; ++k) {
    w.pvSrc[k] = cb[kPerViewSlot[k]];  // held until the window ends (released there)
    cb[kPerViewSlot[k]] = nullptr;
  }
  for (auto* b : cb)
    if (b) b->Release();
  // The buffers DCS uploaded in this window are the per-view sources whatever is bound yet (DCS binds them per
  // draw, inside the items the twin replaces).
  ID3D11Buffer* own[kPerView] = {nullptr, nullptr, w.b10Buf, w.tcBuf};
  for (int k = 0; k < kPerView; ++k) {
    if (!own[k]) continue;
    if (w.pvSrc[k]) w.pvSrc[k]->Release();
    w.pvSrc[k] = own[k];
    own[k]->AddRef();
  }
  w.mode = kWmFocus;
  w.decided = false;
  w.twin = false;
  w.lastIdx = -1;
  w.pending.clear();
  w.sawWork = false;
  if (w.verifyRun2) {  // the same pass again: nothing counted twice
    g_st.focusOwnUs += (defrec::Qpc() - t0) * QpcUs();
    return;
  }
  if (e.runs) e.runs->Begin();
  g_st.focusPasses++;
  e.focusPasses++;
  e.sinceAudit++;
  e.sinceVerify++;
  const int ve = g_verifyEvery.load(std::memory_order_relaxed);
  if (ve > 0 && e.sinceVerify >= static_cast<uint64_t>(ve)) w.verifyCap = VerifyCapture(w.tg);  // before any draw
  if (!w.verifyCap) GpuBegin();
  g_st.focusOwnUs += (defrec::Qpc() - t0) * QpcUs();
}

bool ValidateOrWhy(Eye& e, Stream*& out, int* why) {
  Win& w = g_win;
  out = nullptr;
  if (e.cur < 0) return *why = kWNoPeriph, false;
  Stream& s = e.s[e.cur];
  if (s.ciSeq != w.ci.seq) return *why = kWStale, false;
  if (!s.usable) return *why = s.unusable == 6 ? kWStale : kWEyeBad, false;
  if (!w.haveTc) return *why = kWNoTc, false;
  if (!SharedRowsEqual(s.tc, w.tc)) return *why = kWShared, false;
  if (!s.pred.ok) {
    g_st.predWhy[s.pred.why]++;
    return *why = kWPred, false;
  }
  double dev = 0;
  const int miss = ValidateFocus(s.pred, w.tc, g_predTol.load(std::memory_order_relaxed), &dev);
  g_st.predChecks++;
  if (dev > g_st.predDevMax && dev < 1e29) g_st.predDevMax = dev;
  if (miss) return *why = kWMismatch, false;
  out = &s;
  return true;
}
// Starts an audit of this focus pass (stock, teed into g_audit).
void StartAudit(int eye) {
  Win& w = g_win;
  StartTee(g_audit, eye);
  w.mode = kWmAudit;
  g_st.audits++;
}
// The decision at the first patch item of a focus pass.
void DecideBody();
void Decide() {
  const int64_t t0 = defrec::Qpc();
  DecideBody();
  g_st.focusOwnUs += (defrec::Qpc() - t0) * QpcUs();
}
void DecideBody() {
  Win& w = g_win;
  w.decided = true;
  Eye& e = g_eye[w.eye];
  Stream* s = nullptr;
  int why = kWTwin;
  if (!ValidateOrWhy(e, s, &why)) {
    w.why = why;
    g_st.why[why]++;
    return;
  }
  // A verify's run 1 checks everything the twin needs, then draws stock; its run 2 is the twin (no audit, no trust).
  if (!w.verifyCap && !w.verifyRun2 && AuditDue(e)) {
    w.why = g_shadowKey.load() ? kWShadow : (e.trust < g_trustNeeded.load() ? kWTrust : kWAudit);
    g_st.why[w.why]++;
    e.sinceAudit = 0;
    StartAudit(w.eye);
    return;
  }
  const int js = s->job.load(std::memory_order_acquire);
  if (js == kJRunning) {
    g_st.late++;
    w.why = kWLate, g_st.why[kWLate]++;
    return;
  }
  if (js != kJOk) {
    w.why = kWBuild, g_st.why[kWBuild]++;
    return;
  }
  if (!SameTargets(w.tg, s->targets)) {
    w.why = kWTargets, g_st.why[kWTargets]++;
    return;
  }
  // Our per-view copies from DCS's focus buffers: the same objects as the periphery's (b7: any of the size).
  for (int k = 0; k < kPerView; ++k) {
    if (!s->pvSet[k]) continue;
    // Own upload in this window, else what is bound; b5 / b10 / b11 are one object for every view (R120b census), so
    // with neither the periphery's object is the one the focus draws read (its content now is what they would read).
    if (!w.pvSrc[k] && k != 1) w.pvSrc[k] = s->pv[k], s->pv[k]->AddRef();
    ID3D11Buffer* src = w.pvSrc[k];
    if (!src || !e.pvCopy[k] || (k != 1 && src != s->pv[k])) {
      w.why = kWTargets, g_st.why[kWTargets]++;
      return;
    }
    D3D11_BUFFER_DESC bd{};
    src->GetDesc(&bd);
    if (bd.ByteWidth != e.pvBytes[k]) {
      w.why = kWTargets, g_st.why[kWTargets]++;
      return;
    }
  }
  if (w.verifyCap) {  // run 1 of a verify: stock, the twin is possible
    w.verifyGo = true;
    w.why = kWVerify, g_st.why[kWVerify]++;
    return;
  }
  Pause();
  for (int k = 0; k < kPerView; ++k)
    if (s->pvSet[k]) O<void(STDMETHODCALLTYPE*)(Ctx*, ID3D11Resource*, ID3D11Resource*)>(sCopyResource)(g_tbl.ctx, e.pvCopy[k], w.pvSrc[k]);
  Resume();
  w.twin = true;
  w.why = kWTwin;
  if (!w.verifyRun2) g_st.why[kWTwin]++;
}

// The pending twin draws (render thread, in a focus window, before other work).
void Flush() {
  Win& w = g_win;
  if (w.pending.empty()) return;
  const int64_t t0 = defrec::Qpc();
  Eye& e = g_eye[w.eye];
  Stream& s = e.s[e.cur];
  int lastChunk = -1;
  Pause();
  auto run = [&](int c) {
    Chunk& ch = s.chunks[static_cast<size_t>(c)];
    if (!ch.cl) return;
    O<void(STDMETHODCALLTYPE*)(Ctx*, ID3D11Resource*, UINT, const D3D11_BOX*, const void*, UINT, UINT)>(sUpdate)(
        g_tbl.ctx, ch.args, 0, nullptr, s.args.data() + static_cast<size_t>(ch.first) * 5, 0, 0);
    O<void(STDMETHODCALLTYPE*)(Ctx*, ID3D11CommandList*, BOOL)>(sECL)(g_tbl.ctx, ch.cl, TRUE);
    g_st.ecls++;
    if (ch.ind) g_st.gateLists++;
    for (int i = ch.first; i < ch.first + ch.count; ++i) {
      uint32_t& inst = s.args[static_cast<size_t>(i) * 5 + 1];
      if (!inst) g_st.emptyDraws++;
      inst = 0;
    }
  };
  for (const Pend& pd : w.pending) {
    const Item& it = s.item[static_cast<size_t>(pd.item)];
    for (int r = it.first; r < it.first + it.count; ++r) {
      // R125: an instancer record draws only if the focus view draws its sub-draw (DCS's per-view bit).
      const int sub = s.rec[static_cast<size_t>(r)].sub;
      if (sub >= 0 && !((pd.mask >> sub) & 1u) && !g_testNoMask) {
        g_st.instMasked++;  // drawn in the periphery, not in this focus view
        continue;
      }
      const int c = ChunkOf(r, g_job[w.eye].chunk);
      if (c != lastChunk) {
        if (lastChunk >= 0) run(lastChunk);
        lastChunk = c;
      }
      s.args[static_cast<size_t>(r) * 5 + 1] = 1;
      if (sub >= 0) g_st.twinInstDraws++;
    }
  }
  if (lastChunk >= 0) run(lastChunk);
  Resume();
  w.pending.clear();
  g_st.flushes++;
  const double us = (defrec::Qpc() - t0) * QpcUs();
  g_st.focusFlushUs += us;
  g_st.focusOwnUs += us;
}

// ---------------------------------------------------------------------------
// Audit: the focus stream against the periphery's, per matched item
// ---------------------------------------------------------------------------
void FinishAudit() {
  Win& w = g_win;
  Eye& e = g_eye[w.eye];
  Stream& a = g_audit;
  if (e.cur < 0) return;
  Stream& p = e.s[e.cur];
  uint64_t compared = 0, failed = 0;
  int firstBad = -1;
  for (const Item& fi : a.item) {
    if (fi.why != kPOk) continue;
    const int pi = p.map->Get(fi.desc);
    if (pi < 0) continue;
    const Item& it = p.item[static_cast<size_t>(pi)];
    if (it.why != kPOk || it.inst != fi.inst) continue;
    if (fi.inst && (fi.subMask & ~it.subMask)) continue;  // the twin would leave it to DCS (another sub-draw)
    ++compared;
    bool same = true;
    if (fi.inst) {  // R125: each focus sub-draw against the periphery's record of the same sub-draw
      for (int k = 0; same && k < fi.count; ++k) {
        const Rec& fr = a.rec[static_cast<size_t>(fi.first + k)];
        bool found = false;
        for (int q = 0; q < it.count && !found; ++q) {
          const Rec& pr = p.rec[static_cast<size_t>(it.first + q)];
          if (pr.sub == fr.sub) found = true, same = pr.h == fr.h;
        }
        same = same && found;
      }
    } else {
      same = it.count == fi.count;
      for (int k = 0; same && k < fi.count; ++k)
        same = p.rec[static_cast<size_t>(it.first + k)].h == a.rec[static_cast<size_t>(fi.first + k)].h;
    }
    if (!same) {
      ++failed;
      if (firstBad < 0) firstBad = pi;
    }
  }
  g_st.auditCompared += compared;
  if (failed) {
    g_st.auditFail++;
    Log("focus twin: audit of eye %d: %llu of %llu matched items were drawn with other inputs in the focus "
        "than in the periphery (first: periphery item %d); off for this session",
        w.eye, static_cast<unsigned long long>(failed), static_cast<unsigned long long>(compared), firstBad);
    g_faulted = true;
    g_faultWhere = "audit";
    g_active = false;
    return;
  }
  if (compared >= 8) {
    g_st.auditOk++;
    e.trust++;
  }
}

// ---------------------------------------------------------------------------
// Hooks
// ---------------------------------------------------------------------------
bool g_dcsCalled = false;  // the current item hook reached DCS's call (or skipped it on purpose)
// Times are taken on one item in 8 (QueryPerformanceCounter is ~20 ns; four per item would be most of our own cost)
// and scaled.
uint32_t g_sampleCtr = 0;
constexpr double kSampleScale = 8.0;
inline bool SampleItem() { return (++g_sampleCtr & 7u) == 0; }
void TrackRun(Eye& e, uint64_t desc, bool matched) {
  if (!e.runs) return;
  Win& w = g_win;
  if (w.sawWork || !matched) e.runs->Break(static_cast<uint32_t>(g_island.load(std::memory_order_relaxed)));
  w.sawWork = false;
  if (matched) e.runs->Add(desc);
}
uint64_t TeeItem(void* mat, void* item, void* ctx, void* x, bool inst) {
  Win& w = g_win;
  Stream& s = *w.s;
  const bool timed = SampleItem();
  const int64_t tIn = timed ? defrec::Qpc() : 0;
  uint64_t desc = 0, key = 0;
  uint8_t bits = 0;
  const bool dOk = inst ? ReadInstDesc(mat, item, ctx, &desc, &key, &bits) : Desc(mat, item, ctx, &desc, &key);
  const int idx = static_cast<int>(s.item.size());
  s.item.push_back(Item{dOk ? desc : 0, static_cast<int>(s.rec.size()), 0, kPOk, inst, 0});
  w.inItem = true, w.item = idx, w.itemWhy = dOk ? kPOk : kPWork;
  w.itemInst = inst;
  w.itemSubN = w.itemSubNext = 0;
  for (int k = 0; k < 2; ++k)
    if (bits >> k & 1) w.itemSub[w.itemSubN++] = k;
  // Only items the focus can replace this frame are recorded: in the eye's last-frame runs (the island rule's set),
  // unless this tee records everything (an audit is due, or the focus is the audit itself).
  w.itemRec = w.recAll || w.mode != kWmPeriph || (dOk && g_eye[w.eye].good && g_eye[w.eye].good->Has(key));
  const int64_t t0 = timed ? defrec::Qpc() : 0;
  g_dcsCalled = true;
  const uint64_t r = (inst ? g_instOrig : g_matOrig)(mat, item, ctx, x);
  const int64_t t1 = timed ? defrec::Qpc() : 0;
  w.inItem = false;
  w.itemInst = false;
  Item& it = s.item[static_cast<size_t>(idx)];
  it.count = static_cast<int>(s.rec.size()) - it.first;
  it.why = w.itemWhy;
  if (inst) {
    it.subMask = bits;
    // Every announced sub-draw drew once, in order (DCS's loop); else the records cannot be told apart.
    if (it.why == kPOk && w.itemSubNext != w.itemSubN) it.why = kPSubs;
  }
  if (it.why == kPOk && !w.itemRec) it.why = kPNotCand;
  if (it.why == kPOk && it.count == 0) it.why = kPNoDraw;
  w.itemRec = true;
  if (w.mode == kWmPeriph) {
    g_st.periphItems++;
    g_st.perWhy[it.why]++;
    if (inst) g_st.periphInstItems++;
    if (timed) (inst ? g_st.periphDcsInstUs : g_st.periphDcsItemUs) += (t1 - t0) * QpcUs() * kSampleScale;
  } else if (inst) {
    if (timed) g_st.focusStockInstUs += (t1 - t0) * QpcUs() * kSampleScale;
    g_st.focusStockInstItems++;
  } else {
    if (timed) g_st.focusStockItemUs += (t1 - t0) * QpcUs() * kSampleScale;
    g_st.focusStockItems++;
  }
  if (dOk) {
    int32_t dup = -1;
    if (!s.map->Put(desc, idx, &dup) && dup >= 0) {
      s.item[static_cast<size_t>(dup)].why = kPWork;  // two items, one descriptor: neither is replaced
      it.why = kPWork;
      s.dupDesc.push_back(desc);
    }
  }
  if (w.mode == kWmAudit) {
    // The audit tracks runs as a stock focus pass does.
    Eye& e = g_eye[w.eye];
    const int pi = (e.cur >= 0 && dOk) ? e.s[e.cur].map->Get(desc) : -1;
    bool ok = pi >= 0 && e.s[e.cur].item[static_cast<size_t>(pi)].why == kPOk;
    if (ok) {
      const Item& p = e.s[e.cur].item[static_cast<size_t>(pi)];
      ok = p.inst == inst && (!inst || (bits & ~p.subMask) == 0);
    }
    TrackRun(e, key, ok);
  }
  if (timed) {
    g_st.timed++;
    const double own = static_cast<double>((t0 - tIn) + (defrec::Qpc() - t1)) * QpcUs() * kSampleScale;
    if (w.mode == kWmPeriph)
      g_st.periphOwnUs += own, g_st.periphItemOwnUs += own;
    else
      g_st.focusOwnUs += own, g_st.focusItemOwnUs += own;
  }
  return r;
}
inline int s_whyOf(Eye& e, int pi) {
  return (e.cur >= 0 && pi >= 0) ? e.s[e.cur].item[static_cast<size_t>(pi)].why : -1;
}
uint64_t FocusItem(void* mat, void* item, void* ctx, void* x, bool inst) {
  Win& w = g_win;
  if (!w.decided) Decide();
  if (w.mode == kWmAudit) return TeeItem(mat, item, ctx, x, inst);
  Eye& e = g_eye[w.eye];
  const bool timed = SampleItem();
  const int64_t t0 = timed ? defrec::Qpc() : 0;
  uint64_t desc = 0, key = 0;
  uint8_t bits = 0;
  int why = -1;
  int pi = -1;
  if (!(inst ? ReadInstDesc(mat, item, ctx, &desc, &key, &bits) : Desc(mat, item, ctx, &desc, &key))) {
    why = kIDescFault;
  } else if (e.cur >= 0 && e.s[e.cur].ciSeq == w.ci.seq) {
    Stream& s = e.s[e.cur];
    pi = s.map->Get(desc);
    if (pi < 0 || s.item[static_cast<size_t>(pi)].inst != inst) {
      why = kIUnmatched;
    } else if (s.item[static_cast<size_t>(pi)].why != kPOk) {
      why = kINotTwinnable;
    } else if (inst && (bits & ~s.item[static_cast<size_t>(pi)].subMask)) {
      why = kISubs;  // the focus draws a sub-draw the periphery did not: DCS's
    }
  } else {
    why = kIUnmatched;
  }
  bool runOk = why < 0;  // the island runs: matched and replaceable, recorded this frame or not
  if (why == kINotTwinnable && s_whyOf(e, pi) == kPNotCand) runOk = true;
  const bool matched = why < 0;
  if (!w.verifyRun2) TrackRun(e, key, runOk);  // a verify's second run is the same pass again
  g_st.focusItems++;
  if (inst) g_st.focusInstItems++;
  if (w.twin && !w.stopTwin && matched) {
    if (pi <= w.lastIdx) {
      why = kIOrder;
      w.stopTwin = true;
    } else if (!e.good || !e.good->Has(key)) {
      why = kIIsland;
    } else {
      w.lastIdx = pi;
      w.pending.push_back(Pend{pi, inst ? bits : static_cast<uint8_t>(3)});
      g_st.twinItems++;
      if (inst) {
        g_st.twinInstItems++;
        if (!bits) g_st.instNoDraw++;
      }
      if (timed) {
        const double own = (defrec::Qpc() - t0) * QpcUs() * kSampleScale;
        g_st.focusOwnUs += own, g_st.focusItemOwnUs += own;
      }
      g_dcsCalled = true;  // skipped on purpose
      return 0;
    }
  }
  if (w.twin && why >= 0) g_st.itemWhy[why]++;
  // DCS's own item. Pending twin draws go first (its first work flushes them).
  w.inItem = true;
  const int64_t t1 = timed ? defrec::Qpc() : 0;
  if (timed) {
    const double own = (t1 - t0) * QpcUs() * kSampleScale;
    g_st.focusOwnUs += own, g_st.focusItemOwnUs += own;
  }
  g_dcsCalled = true;
  const uint64_t r = (inst ? g_instOrig : g_matOrig)(mat, item, ctx, x);
  w.inItem = false;
  if (timed) {
    (inst ? g_st.focusStockInstUs : g_st.focusStockItemUs) += (defrec::Qpc() - t1) * QpcUs() * kSampleScale;
    g_st.timed++;
  }
  (inst ? g_st.focusStockInstItems : g_st.focusStockItems)++;
  return r;
}

// gbpass::g_obs2: inside a GBufferPass execution.
void Obs(void*, void*, bool begin) {
  t_gbDepth = begin ? (t_gbDepth > 0 ? t_gbDepth : 0) + 1 : (t_gbDepth > 0 ? t_gbDepth - 1 : 0);
}

uint64_t MatHookBody(void* mat, void* item, void* ctx, void* x, bool inst) {
  Win& w = g_win;
  if (w.mode == kWmPeriph) return TeeItem(mat, item, ctx, x, inst);
  if (w.mode == kWmFocus || w.mode == kWmAudit) return FocusItem(mat, item, ctx, x, inst);
  return (inst ? g_instOrig : g_matOrig)(mat, item, ctx, x);
}
// Guarded: a fault in our part before DCS's call -> DCS's call; a fault after it -> nothing more (latched).
// R125: the same body for MetaMaterial_Instancer2 items (InstHook). [Suite] FocusTwinPatches = 0: patch items are
// DCS's plain calls (their draws are other work in the window).
inline uint64_t ItemHook(void* mat, void* item, void* ctx, void* x, bool inst) {
  Win& w = g_win;
  const MatFn orig = inst ? g_instOrig : g_matOrig;
  if (!w.open || w.inItem || !g_active.load(std::memory_order_relaxed) || !g_tbl.swapped ||
      GetCurrentThreadId() != g_renderTid || (!inst && !g_patchesKey.load(std::memory_order_relaxed)))
    return orig(mat, item, ctx, x);
  g_inside.fetch_add(1, std::memory_order_acq_rel);
  uint64_t r = 0;
  bool faulted = false;
  g_dcsCalled = false;
  __try {
    r = MatHookBody(mat, item, ctx, x, inst);
  } __except (Filt(GetExceptionCode())) {
    w.inItem = false;
    w.itemInst = false;
    faulted = true;
    Latch("the focus twin item hook", g_faultCode);
  }
  // A fault before DCS's call: DCS draws the item (after one, it ran already).
  if (faulted && !g_dcsCalled) r = orig(mat, item, ctx, x);
  g_inside.fetch_sub(1, std::memory_order_acq_rel);
  return r;
}
uint64_t __fastcall MatHook(void* mat, void* item, void* ctx, void* x) { return ItemHook(mat, item, ctx, x, false); }
uint64_t __fastcall InstHook(void* mat, void* item, void* ctx, void* x) { return ItemHook(mat, item, ctx, x, true); }

void BeginWindow(void* ctx) {
  Win& w = g_win;
  (void)ctx;
  w.open = true;
  w.mode = kWmPending;
  w.eye = -1;
  w.t0 = defrec::Qpc();
  w.ci = CurrentCi();
  w.ciSeq = w.ci.seq;
  w.haveTc = false;
  w.tcUploads = 0;
  w.tcBuf = nullptr;
  w.b10Buf = nullptr;
  w.inItem = false;
  w.itemInst = false;
  w.item = -1;
  w.s = nullptr;
  w.nwr = 0;
  w.decided = w.twin = w.stopTwin = false;
  w.verifyCap = w.verifyGo = w.verifyRun2 = false;
  w.why = kWTwin;
  w.lastIdx = -1;
  w.pending.clear();
  for (auto*& b : w.pvSrc) b = nullptr;
  g_st.windows++;
}
// ---------------------------------------------------------------------------
// [Suite] FocusTwinVerify: the focus pass's targets copied before its terrain, the pass drawn stock, the result kept,
// the targets put back, the pass drawn again with the twin, both results compared byte for byte (every subresource
// of every target texture and of the depth-stencil texture). A difference latches the twin off for the session.
// ---------------------------------------------------------------------------
struct VTex {
  ID3D11Texture2D* src = nullptr;  // DCS's target texture (referenced while a verify runs)
  D3D11_TEXTURE2D_DESC d{};
  ID3D11Texture2D* cap = nullptr;  // before the terrain
  ID3D11Texture2D* out = nullptr;  // run 1's result
  ID3D11Texture2D* st[2] = {};     // staging (run 1, run 2)
};
constexpr int kMaxVTex = 9;
VTex g_vt[kMaxVTex];
int g_nvt = 0;
bool g_verifyNoted = false;
UINT FormatBytes(DXGI_FORMAT f) {
  switch (f) {
    case DXGI_FORMAT_R32G32B32A32_TYPELESS: case DXGI_FORMAT_R32G32B32A32_FLOAT: case DXGI_FORMAT_R32G32B32A32_UINT:
    case DXGI_FORMAT_R32G32B32A32_SINT:
      return 16;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: case DXGI_FORMAT_R16G16B16A16_FLOAT: case DXGI_FORMAT_R16G16B16A16_UNORM:
    case DXGI_FORMAT_R16G16B16A16_UINT: case DXGI_FORMAT_R16G16B16A16_SNORM: case DXGI_FORMAT_R16G16B16A16_SINT:
    case DXGI_FORMAT_R32G32_TYPELESS: case DXGI_FORMAT_R32G32_FLOAT: case DXGI_FORMAT_R32G32_UINT:
    case DXGI_FORMAT_R32G32_SINT: case DXGI_FORMAT_R32G8X24_TYPELESS: case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
      return 8;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS: case DXGI_FORMAT_R10G10B10A2_UNORM: case DXGI_FORMAT_R10G10B10A2_UINT:
    case DXGI_FORMAT_R11G11B10_FLOAT: case DXGI_FORMAT_R8G8B8A8_TYPELESS: case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: case DXGI_FORMAT_R8G8B8A8_UINT: case DXGI_FORMAT_R8G8B8A8_SNORM:
    case DXGI_FORMAT_R8G8B8A8_SINT: case DXGI_FORMAT_R16G16_TYPELESS: case DXGI_FORMAT_R16G16_FLOAT:
    case DXGI_FORMAT_R16G16_UNORM: case DXGI_FORMAT_R16G16_UINT: case DXGI_FORMAT_R16G16_SNORM:
    case DXGI_FORMAT_R16G16_SINT: case DXGI_FORMAT_R32_TYPELESS: case DXGI_FORMAT_D32_FLOAT: case DXGI_FORMAT_R32_FLOAT:
    case DXGI_FORMAT_R32_UINT: case DXGI_FORMAT_R32_SINT: case DXGI_FORMAT_R24G8_TYPELESS:
    case DXGI_FORMAT_D24_UNORM_S8_UINT: case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_B8G8R8A8_TYPELESS:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: case DXGI_FORMAT_B8G8R8X8_UNORM: case DXGI_FORMAT_R9G9B9E5_SHAREDEXP:
      return 4;
    case DXGI_FORMAT_R8G8_TYPELESS: case DXGI_FORMAT_R8G8_UNORM: case DXGI_FORMAT_R8G8_UINT: case DXGI_FORMAT_R8G8_SNORM:
    case DXGI_FORMAT_R8G8_SINT: case DXGI_FORMAT_R16_TYPELESS: case DXGI_FORMAT_R16_FLOAT: case DXGI_FORMAT_D16_UNORM:
    case DXGI_FORMAT_R16_UNORM: case DXGI_FORMAT_R16_UINT: case DXGI_FORMAT_R16_SNORM: case DXGI_FORMAT_R16_SINT:
      return 2;
    case DXGI_FORMAT_R8_TYPELESS: case DXGI_FORMAT_R8_UNORM: case DXGI_FORMAT_R8_UINT: case DXGI_FORMAT_R8_SNORM:
    case DXGI_FORMAT_R8_SINT: case DXGI_FORMAT_A8_UNORM:
      return 1;
    default:
      return 0;
  }
}
void ReleaseVTex() {
  for (VTex& v : g_vt) {
    if (v.cap) v.cap->Release();
    if (v.out) v.out->Release();
    for (auto*& s : v.st)
      if (s) s->Release(), s = nullptr;
    if (v.src) v.src->Release();
    v = VTex();
  }
  g_nvt = 0;
}
bool MakeVTex(VTex& v, ID3D11Texture2D* src) {
  D3D11_TEXTURE2D_DESC d{};
  src->GetDesc(&d);
  if (d.SampleDesc.Count != 1 || !FormatBytes(d.Format)) return false;
  const bool same = v.cap && memcmp(&d, &v.d, sizeof(d)) == 0;
  if (v.src) v.src->Release();
  v.src = src, src->AddRef();
  if (same) return true;
  if (v.cap) v.cap->Release(), v.cap = nullptr;
  if (v.out) v.out->Release(), v.out = nullptr;
  for (auto*& s : v.st)
    if (s) s->Release(), s = nullptr;
  v.d = d;
  D3D11_TEXTURE2D_DESC c = d;
  c.Usage = D3D11_USAGE_DEFAULT, c.BindFlags = 0, c.CPUAccessFlags = 0, c.MiscFlags = 0;
  D3D11_TEXTURE2D_DESC s = c;
  s.Usage = D3D11_USAGE_STAGING, s.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  return SUCCEEDED(g_dev->CreateTexture2D(&c, nullptr, &v.cap)) && SUCCEEDED(g_dev->CreateTexture2D(&c, nullptr, &v.out)) &&
         SUCCEEDED(g_dev->CreateTexture2D(&s, nullptr, &v.st[0])) && SUCCEEDED(g_dev->CreateTexture2D(&s, nullptr, &v.st[1]));
}
// At the focus classification (before any terrain draw of the pass): the target textures copied.
bool VerifyCapture(const Targets& tg) {
  ID3D11Resource* res[kMaxVTex] = {};
  int n = 0;
  auto add = [&](ID3D11View* view) {
    if (!view || n >= kMaxVTex) return;
    ID3D11Resource* r = nullptr;
    view->GetResource(&r);
    for (int i = 0; i < n; ++i)
      if (res[i] == r) {
        r->Release();
        return;
      }
    res[n++] = r;
  };
  for (UINT i = 0; i < tg.nrtv; ++i) add(tg.rtv[i]);
  add(tg.dsv);
  bool ok = n > 0 && tg.valid && !tg.uav;
  for (int i = 0; i < n; ++i) {
    D3D11_RESOURCE_DIMENSION dim = D3D11_RESOURCE_DIMENSION_UNKNOWN;
    res[i]->GetType(&dim);
    if (ok && (dim != D3D11_RESOURCE_DIMENSION_TEXTURE2D || !MakeVTex(g_vt[i], static_cast<ID3D11Texture2D*>(res[i]))))
      ok = false;
  }
  for (int i = 0; i < n; ++i) res[i]->Release();
  if (!ok) {
    g_st.verifySkipped++;
    if (!g_verifyNoted) g_verifyNoted = true, Log("focus twin: verify: a target is not a single-sample 2D texture of a known format; not verified");
    return false;
  }
  g_nvt = n;
  Pause();
  for (int i = 0; i < n; ++i) O<void(STDMETHODCALLTYPE*)(Ctx*, ID3D11Resource*, ID3D11Resource*)>(sCopyResource)(g_tbl.ctx, g_vt[i].cap, g_vt[i].src);
  Resume();
  return true;
}
// Between the runs: run 1's result kept, the targets back to their state before the terrain.
void VerifyBetween() {
  Pause();
  auto copy = O<void(STDMETHODCALLTYPE*)(Ctx*, ID3D11Resource*, ID3D11Resource*)>(sCopyResource);
  for (int i = 0; i < g_nvt; ++i) {
    copy(g_tbl.ctx, g_vt[i].out, g_vt[i].src);
    copy(g_tbl.ctx, g_vt[i].src, g_vt[i].cap);
  }
  Resume();
}
// After run 2: both results read back and compared (blocking: a suite mode).
uint64_t VerifyCompare(uint64_t* bytes) {
  Pause();
  auto copy = O<void(STDMETHODCALLTYPE*)(Ctx*, ID3D11Resource*, ID3D11Resource*)>(sCopyResource);
  auto map = O<HRESULT(STDMETHODCALLTYPE*)(Ctx*, ID3D11Resource*, UINT, D3D11_MAP, UINT, D3D11_MAPPED_SUBRESOURCE*)>(sMap);
  auto unmap = O<void(STDMETHODCALLTYPE*)(Ctx*, ID3D11Resource*, UINT)>(sUnmap);
  uint64_t diff = 0, total = 0;
  for (int i = 0; i < g_nvt; ++i) {
    VTex& v = g_vt[i];
    copy(g_tbl.ctx, v.st[0], v.out);
    copy(g_tbl.ctx, v.st[1], v.src);
    const UINT bpp = FormatBytes(v.d.Format);
    for (UINT a = 0; a < v.d.ArraySize; ++a)
      for (UINT m = 0; m < v.d.MipLevels; ++m) {
        const UINT sub = D3D11CalcSubresource(m, a, v.d.MipLevels);
        D3D11_MAPPED_SUBRESOURCE p0{}, p1{};
        if (FAILED(map(g_tbl.ctx, v.st[0], sub, D3D11_MAP_READ, 0, &p0))) continue;
        if (FAILED(map(g_tbl.ctx, v.st[1], sub, D3D11_MAP_READ, 0, &p1))) {
          unmap(g_tbl.ctx, v.st[0], sub);
          continue;
        }
        const UINT w = std::max(1u, v.d.Width >> m), h = std::max(1u, v.d.Height >> m);
        const size_t row = static_cast<size_t>(w) * bpp;
        for (UINT y = 0; y < h; ++y) {
          const uint8_t* r0 = static_cast<const uint8_t*>(p0.pData) + static_cast<size_t>(y) * p0.RowPitch;
          const uint8_t* r1 = static_cast<const uint8_t*>(p1.pData) + static_cast<size_t>(y) * p1.RowPitch;
          total += row;
          if (memcmp(r0, r1, row) != 0)
            for (size_t b = 0; b < row; ++b) diff += r0[b] != r1[b];
        }
        unmap(g_tbl.ctx, v.st[1], sub);
        unmap(g_tbl.ctx, v.st[0], sub);
      }
  }
  Resume();
  *bytes = total;
  return diff;
}
void GpuBegin() {
  GpuQ& q = g_gq[g_gqNext];
  if (q.pending || !q.dj) return;
  ID3D11DeviceContext* c = g_tbl.ctx;
  c->Begin(q.dj);
  c->End(q.b);
  g_win.qSlot = g_gqNext;
  g_gqNext = (g_gqNext + 1) % kGpuRing;
}
void GpuEnd(bool twin) {
  if (g_win.qSlot < 0) return;
  GpuQ& q = g_gq[g_win.qSlot];
  ID3D11DeviceContext* c = g_tbl.ctx;
  c->End(q.e);
  c->End(q.dj);
  q.pending = true;
  q.twin = twin;
  g_win.qSlot = -1;
}
void GpuCollect() {
  ID3D11DeviceContext* c = g_tbl.ctx;
  for (GpuQ& q : g_gq) {
    if (!q.pending) continue;
    D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj{};
    UINT64 b = 0, e = 0;
    if (c->GetData(q.dj, &dj, sizeof(dj), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) continue;
    if (c->GetData(q.b, &b, sizeof(b), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) continue;
    if (c->GetData(q.e, &e, sizeof(e), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) continue;
    q.pending = false;
    if (dj.Disjoint || !dj.Frequency || e < b) continue;
    const double us = static_cast<double>(e - b) * 1e6 / static_cast<double>(dj.Frequency);
    if (q.twin)
      g_st.gpuTwinUs += us, g_st.gpuTwinN++;
    else
      g_st.gpuStockUs += us, g_st.gpuStockN++;
  }
}
// Every writer of an inherited constant buffer stays DCS's (see Stream::writes).
void FinishWriters(Stream& s) {
  if (s.inherited.empty()) return;
  for (const auto& wi : s.writes) {
    if (wi.first < 0 || static_cast<size_t>(wi.first) >= s.item.size()) continue;
    if (std::find(s.inherited.begin(), s.inherited.end(), wi.second) == s.inherited.end()) continue;
    Item& it = s.item[static_cast<size_t>(wi.first)];
    if (it.why == kPOk) {
      it.why = kPInherit;
      if (&s != &g_audit) g_st.perWhy[kPOk]--, g_st.perWhy[kPInherit]++;
    }
  }
}
void Submit(Eye& e, int eyeIdx) {
  Stream& s = e.s[e.cur];
  CopyTargets(s.targets, e.targets);
  s.job.store(kJIdle, std::memory_order_release);
  if (!s.usable || !s.targets.valid || !g_poolOk) {
    if (!s.targets.valid) s.job.store(kJFailed);
    return;
  }
  Job& j = g_job[eyeIdx];
  j.s = &s, j.e = &e, j.chunk = g_chunk.load(std::memory_order_relaxed);
  s.job.store(kJRunning, std::memory_order_release);
  if (!g_pool.Submit(eyeIdx, &BuildJob, &j, nullptr)) {
    s.job.store(kJFailed, std::memory_order_release);
    g_st.buildFails++;
  }
}
void EndWindowBody() {
  Win& w = g_win;
  const int64_t t0 = defrec::Qpc();
  if (w.mode == kWmPeriph && w.s) {
    Eye& e = g_eye[w.eye];
    Stream& s = *w.s;
    FinishWriters(s);
    s.pred = PredictFocus(s.tc, w.ci, e.hist);
    Submit(e, w.eye);
    g_st.periphOwnUs += (defrec::Qpc() - t0) * QpcUs();
  } else if (w.mode == kWmFocus || w.mode == kWmAudit) {
    Eye& e = g_eye[w.eye];
    if (w.mode == kWmFocus) Flush();
    if (w.mode == kWmAudit) {
      FinishWriters(g_audit);
      FinishAudit();
    }
    if (e.runs) {
      e.runs->Break(static_cast<uint32_t>(g_island.load(std::memory_order_relaxed)));
      if (e.good) e.good->From(*e.runs);
    }
    // The next frame's prediction learns from this pass, unless its prediction missed (a camera that did not match
    // the upload must not teach the next frame): then the next frame has none (stock) and learns again.
    if (w.why == kWMismatch)
      e.hist.have = false;
    else if (w.haveTc && e.cur >= 0 && e.s[e.cur].ciSeq == w.ci.seq)
      LearnFocus(e.hist, e.s[e.cur].tc, w.tc, w.ci);
    else
      e.hist.have = false;
    CopyTargets(e.targets, w.tg);
    if (w.mode == kWmAudit) ResetStream(g_audit);
    g_st.focusOwnUs += (defrec::Qpc() - t0) * QpcUs();
  }
}
void EndWindow() {
  Win& w = g_win;
  __try {
    EndWindowBody();
  } __except (Filt(GetExceptionCode())) {
    Latch("the focus twin window end", g_faultCode);
  }
  ReleaseTargets(w.tg);
  for (auto*& b : w.pvSrc)
    if (b) b->Release(), b = nullptr;
  w.open = false;
  w.mode = kWmNone;
  w.s = nullptr;
}

// Run 2 of a verify: the window as at its start (the eye and this frame's cameras kept).
void VerifyRun2Prepare() {
  Win& w = g_win;
  Flush();
  ReleaseTargets(w.tg);
  for (auto*& b : w.pvSrc)
    if (b) b->Release(), b = nullptr;
  w.mode = kWmPending;
  w.haveTc = false;
  w.tcUploads = 0;
  w.tcBuf = w.b10Buf = nullptr;
  w.decided = w.twin = w.stopTwin = false;
  w.verifyCap = w.verifyGo = false;
  w.verifyRun2 = true;
  w.lastIdx = -1;
  w.pending.clear();
  w.qSlot = -1;
}
void VerifyFinish() {
  Win& w = g_win;
  Flush();
  Eye& e = g_eye[w.eye >= 0 ? w.eye : 0];
  e.sinceVerify = 0;
  if (!w.twin) {  // run 2 could not draw the twin after all (it changed its mind): nothing compared
    g_st.verifySkipped++;
    return;
  }
  uint64_t bytes = 0;
  const uint64_t diff = VerifyCompare(&bytes);
  g_st.verifies++;
  g_st.verifyBytes += bytes;
  g_st.verifyDiffBytes += diff;
  if (diff) {
    g_st.verifyFail++;
    Log("focus twin: verify of eye %d: %llu of %llu target bytes differ between DCS's focus terrain and the twin's; "
        "off for this session",
        w.eye, static_cast<unsigned long long>(diff), static_cast<unsigned long long>(bytes));
    g_faulted = true;
    g_faultWhere = "verify";
    g_active = false;
  }
}
uint64_t __fastcall TerrHook(void* self, void* ctx) {
  if (!g_active.load(std::memory_order_relaxed) || t_gbDepth <= 0 || g_win.open || !g_tbl.ctx)
    return g_terrOrig(self, ctx);
  if (!g_renderTid) g_renderTid = GetCurrentThreadId();
  if (GetCurrentThreadId() != g_renderTid) return g_terrOrig(self, ctx);
  g_inside.fetch_add(1, std::memory_order_acq_rel);
  bool swapped = false;
  __try {
    BeginWindow(ctx);
    swapped = SwapIn();
  } __except (Filt(GetExceptionCode())) {
    Latch("the focus twin window begin", g_faultCode);
  }
  if (!swapped) {
    g_win.open = false;
    g_inside.fetch_sub(1, std::memory_order_acq_rel);
    return g_terrOrig(self, ctx);
  }
  uint64_t r = g_terrOrig(self, ctx);
  __try {
    const bool twin = g_win.mode == kWmFocus && g_win.twin;
    if (g_win.qSlot >= 0) GpuEnd(twin);
  } __except (Filt(GetExceptionCode())) {
    Latch("the focus twin GPU timing", g_faultCode);
  }
  if (g_win.verifyGo && g_win.mode == kWmFocus && !g_faulted.load()) {
    bool again = false;
    __try {
      VerifyBetween();
      VerifyRun2Prepare();
      again = true;
    } __except (Filt(GetExceptionCode())) {
      Latch("the focus twin verify", g_faultCode);
    }
    if (again) r = g_terrOrig(self, ctx);  // the same pass with the twin
    __try {
      if (again) VerifyFinish();
    } __except (Filt(GetExceptionCode())) {
      Latch("the focus twin verify", g_faultCode);
    }
  }
  EndWindow();
  SwapOut();
  __try {
    GpuCollect();
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }
  g_inside.fetch_sub(1, std::memory_order_acq_rel);
  return r;
}

// ---------------------------------------------------------------------------
// Install / uninstall
// ---------------------------------------------------------------------------
bool BuildTable(ID3D11DeviceContext* ctx) {
  Tbl& t = g_tbl;
  if (t.ctx) return true;
  t.n = DcsQv_ContextSlotCount();
  if (t.n <= 0 || t.n > 256) return false;
  for (int s = 0; s < sCount; ++s) {
    t.slot[s] = DcsQv_ContextSlot(kSlName[s]);
    if (t.slot[s] < 0 || t.slot[s] >= t.n) {
      Log("focus twin: no context slot for %s; not installed", kSlName[s]);
      return false;
    }
  }
  if (!t.copy) {
    // Session lifetime: other features may hold pointers into the copy; never freed.
    t.copy = static_cast<void**>(VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    t.expect = static_cast<void**>(VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    t.snap = static_cast<void**>(VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    t.page = static_cast<uint8_t*>(VirtualAlloc(nullptr, 8192, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (!t.copy || !t.expect || !t.snap || !t.page) return false;
    t.origVar = reinterpret_cast<void**>(t.page);
    // Forwarder i: mov rax, [rip + origVar]; jmp qword ptr [rax + 8 i].
    uint8_t* code = t.page + 4096;
    for (int i = 0; i < 256; ++i) {
      uint8_t* p = code + 16 * i;
      const int32_t rel = static_cast<int32_t>(reinterpret_cast<uint8_t*>(t.origVar) - (p + 7));
      p[0] = 0x48, p[1] = 0x8b, p[2] = 0x05;
      memcpy(p + 3, &rel, 4);
      const int32_t off = 8 * i;
      p[7] = 0xff, p[8] = 0xa0;
      memcpy(p + 9, &off, 4);
      p[13] = 0xcc, p[14] = 0xcc, p[15] = 0xcc;
    }
    DWORD old = 0;
    if (!VirtualProtect(code, 4096, PAGE_EXECUTE_READ, &old)) return false;
    FlushInstructionCache(GetCurrentProcess(), code, 4096);
  }
  for (int i = 0; i < t.n; ++i) t.expect[i] = t.page + 4096 + 16 * i, t.tapped[i] = false;
  for (int s = 0; s < sCount; ++s) t.expect[t.slot[s]] = kTap[s], t.tapped[t.slot[s]] = true;
  HMODULE d3d = GetModuleHandleW(L"d3d11.dll");
  if (d3d) {
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(d3d);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(reinterpret_cast<const uint8_t*>(d3d) + dos->e_lfanew);
    t.d3dLo = reinterpret_cast<uintptr_t>(d3d);
    t.d3dHi = t.d3dLo + nt->OptionalHeader.SizeOfImage;
  }
  t.ctx = ctx;
  t.ctx->AddRef();
  t.orig = *reinterpret_cast<void***>(ctx);
  return true;
}

void ReleaseAll() {
  for (int i = 0; i < 2; ++i) {
    Eye& e = g_eye[i];
    for (Stream& s : e.s) {
      ResetStream(s);
      ReleaseChunks(s, false);
      delete s.map, s.map = nullptr;
      s.args.clear();
      s.job.store(kJIdle);
    }
    e.cur = -1;
    ReleaseTargets(e.targets);
    for (int k = 0; k < kPerView; ++k)
      if (e.pvCopy[k]) e.pvCopy[k]->Release(), e.pvCopy[k] = nullptr, e.pvBytes[k] = 0;
    e.hist = EyeHist();
    e.trust = 0;
    e.lastPeriphCi = 0;
    delete e.good, e.good = nullptr;
    delete e.runs, e.runs = nullptr;
  }
  ResetStream(g_audit);
  delete g_audit.map, g_audit.map = nullptr;
  ReleaseVTex();
  for (GpuQ& q : g_gq) {
    if (q.dj) q.dj->Release();
    if (q.b) q.b->Release();
    if (q.e) q.e->Release();
    q = GpuQ();
  }
  if (g_gateCs) g_gateCs->Release(), g_gateCs = nullptr;
  if (g_tbl.ctx) g_tbl.ctx->Release();
  g_tbl.ctx = nullptr;
  g_tbl.orig = nullptr;
  if (g_ctx1) g_ctx1->Release(), g_ctx1 = nullptr;
  if (g_dev) g_dev->Release(), g_dev = nullptr;
}

struct Targets2 {
  void** matSlot = nullptr;   // MetaMaterial vtable slot 8
  void** terrSlot = nullptr;  // TerrainObject vtable slot 1
  void** instSlot = nullptr;  // R125: MetaMaterial_Instancer2 vtable slot 8 (null: no instancer part)
  void** instGlobal = nullptr;  // R125: the terrain global variable (IM2 = G->vt[11]())
  void* im2Vtbl = nullptr;    // R125: InstanceManager2's vtable (null: not checked)
};
// R125: the gate pass's compute shader (d3dcompiler_47, as chain_fast.h). False: not available.
bool MakeGate(ID3D11Device* dev) {
  if (g_gateCs) return true;
  if (g_testNoGate) return false;
  HMODULE m = LoadLibraryW(L"d3dcompiler_47.dll");
  auto compile = m ? reinterpret_cast<pD3DCompile>(GetProcAddress(m, "D3DCompile")) : nullptr;
  if (!compile) return false;
  ID3DBlob* blob = nullptr;
  ID3DBlob* err = nullptr;
  const HRESULT hr = compile(kGateHlsl, sizeof(kGateHlsl) - 1, "focus_twin_gate", nullptr, nullptr, "main", "cs_5_0",
                             D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &blob, &err);
  if (err) err->Release();
  if (FAILED(hr) || !blob) return false;
  const bool ok =
      SUCCEEDED(dev->CreateComputeShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &g_gateCs)) && g_gateCs;
  blob->Release();
  return ok;
}
// The hooks on given slots, with DCS's device / context (tests: fake vtables, a WARP device).
bool InstallOn(const Targets2& tg, ID3D11Device* dev, ID3D11DeviceContext* ctx) {
  if (g_state.load() == 1) return true;
  if (!dev || !ctx || !tg.matSlot || !tg.terrSlot) return false;
  if (!BuildTable(ctx)) return false;
  g_dev = dev, g_dev->AddRef();
  ctx->QueryInterface(__uuidof(ID3D11DeviceContext1), reinterpret_cast<void**>(&g_ctx1));
  defrec::PoolConfig pc;
  pc.workers = 2;
  pc.cb.mode = defrec::CbMode::kDiscard;  // the ring is not used (the lists fill their own constant buffers)
  pc.cb.maxBucket = 16, pc.cb.perBucket = 1;
  pc.priority = THREAD_PRIORITY_NORMAL;
  pc.name = "focus twin";
  g_poolOk = g_pool.Start(dev, pc);
  if (!g_poolOk) {
    g_pool.Stop();
    ReleaseAll();
    Log("focus twin: its workers did not start (deferred contexts); not installed");
    return false;
  }
  for (int i = 0; i < 2; ++i) {
    Eye& e = g_eye[i];
    for (Stream& s : e.s) {
      if (!s.map) s.map = new DescMap();
      s.map->Clear();
    }
    if (!e.good) e.good = new GoodSet(), e.good->Clear();
    if (!e.runs) e.runs = new RunTracker();
  }
  if (!g_audit.map) g_audit.map = new DescMap();
  for (GpuQ& q : g_gq) {
    D3D11_QUERY_DESC dj{D3D11_QUERY_TIMESTAMP_DISJOINT, 0}, ts{D3D11_QUERY_TIMESTAMP, 0};
    if (FAILED(dev->CreateQuery(&dj, &q.dj)) || FAILED(dev->CreateQuery(&ts, &q.b)) ||
        FAILED(dev->CreateQuery(&ts, &q.e))) {
      if (q.dj) q.dj->Release();
      if (q.b) q.b->Release();
      if (q.e) q.e->Release();
      q = GpuQ();
    }
  }
  // R125: the instancer part ([Model] FocusTwinInstancer) needs its gate pass; without it only patches are twinned.
  g_instInstalled = false;
  g_instGlobal = tg.instGlobal, g_im2Vtbl = tg.im2Vtbl;
  const bool wantInst = g_instKey.load() == 1 && tg.instSlot && tg.instGlobal;
  if (wantInst && !MakeGate(dev))
    Log("focus twin: the instancer gate pass (compute shader) is not available; trees and buildings stay DCS's");
  g_matSlot = tg.matSlot, g_terrSlot = tg.terrSlot;
  if (!HookSlot(g_matSlot, reinterpret_cast<void*>(&MatHook), reinterpret_cast<void**>(&g_matOrig)) ||
      !HookSlot(g_terrSlot, reinterpret_cast<void*>(&TerrHook), reinterpret_cast<void**>(&g_terrOrig))) {
    if (g_matOrig) UnhookSlot(g_matSlot, reinterpret_cast<void*>(g_matOrig));
    g_pool.Stop();
    ReleaseAll();
    return false;
  }
  if (wantInst && g_gateCs) {
    g_instSlot = tg.instSlot;
    g_instInstalled = HookSlot(g_instSlot, reinterpret_cast<void*>(&InstHook), reinterpret_cast<void**>(&g_instOrig));
    if (!g_instInstalled) g_instSlot = nullptr, g_instOrig = nullptr;
  }
  gbpass::g_obs2.store(&Obs, std::memory_order_release);
  g_collectTap.store(&CollectTap, std::memory_order_release);
  g_renderTid = 0;
  g_state = 1;
  return true;
}

// R125: the instancer part's targets in DCS's edterrainGraphics41 (an error text: the instancer part is not installed).
const char* CheckInstBuild(uint8_t* etg, void*** instSlot, void*** global, void** im2Vtbl) {
  void** iv = reinterpret_cast<void**>(etg + kInstVtbl);
  void** mv = reinterpret_cast<void**>(etg + kIm2Vtbl);
  if (!allocslab::RttiIs(etg, iv, ".?AVMetaMaterial_Instancer2@edtg41@@"))
    return "MetaMaterial_Instancer2 vtable not where this build has it";
  if (!allocslab::RttiIs(etg, mv, ".?AVInstanceManager2@edtg41@@")) return "InstanceManager2 vtable not where this build has it";
  if (reinterpret_cast<uint8_t*>(SlotOriginal(&iv[kMatSlot])) != etg + kInstRender) return "MetaMaterial_Instancer2::render moved";
  for (size_t i = 0; i < sizeof(kIm2Slots) / sizeof(kIm2Slots[0]); ++i)
    if (reinterpret_cast<uint8_t*>(mv[kIm2Slots[i]]) != etg + kIm2SlotRva[i]) return "InstanceManager2 slots moved";
  for (const CodeRange& c : kInstCode)
    if (!shadowtex::CodeIs(etg, c.begin, c.end, c.hash)) return "edterrainGraphics41 instancer code differs from this build";
  *instSlot = &iv[kMatSlot];
  *global = reinterpret_cast<void**>(etg + kTerrGlobal);
  *im2Vtbl = mv;
  return nullptr;
}
// Production: DCS's modules, checked.
const char* CheckBuild(uint8_t* etg, uint8_t* vis, void*** matSlot, void*** terrSlot) {
  if (!etg || !vis) return "edterrainGraphics41.dll / Visualizer.dll not loaded";
  void** mv = reinterpret_cast<void**>(etg + kMetaMatVtbl);
  if (!allocslab::RttiIs(etg, mv, ".?AVMetaMaterial@edtg41@@")) return "MetaMaterial vtable not where this build has it";
  const VisTerr* vt = nullptr;
  for (const VisTerr& b : kVisTerr)
    if (allocslab::RttiIs(vis, reinterpret_cast<void**>(vis + b.vtbl), ".?AVTerrainObject@TerrainParser@@")) {
      vt = &b;
      break;
    }
  if (!vt) return "TerrainObject vtable not where this build has it";
  void** tv = reinterpret_cast<void**>(vis + vt->vtbl);
  if (reinterpret_cast<uint8_t*>(SlotOriginal(&mv[kMatSlot])) != etg + kMetaMatRender) return "MetaMaterial::render moved";
  if (reinterpret_cast<uint8_t*>(SlotOriginal(&tv[kTerrSlot])) != vis + vt->render) return "TerrainObject::render moved";
  for (const CodeRange& c : kEtgCode)
    if (!shadowtex::CodeIs(etg, c.begin, c.end, c.hash)) return "edterrainGraphics41 code differs from this build";
  if (!shadowtex::CodeIs(vis, vt->code.begin, vt->code.end, vt->code.hash)) return "Visualizer code differs from this build";
  *matSlot = &mv[kMatSlot];
  *terrSlot = &tv[kTerrSlot];
  return nullptr;
}
bool TryInstall() {
  auto* etg = reinterpret_cast<uint8_t*>(GetModuleHandleW(L"edterrainGraphics41.dll"));
  auto* vis = reinterpret_cast<uint8_t*>(GetModuleHandleW(L"Visualizer.dll"));
  if (!etg || !vis) return false;  // retried
  void** ms = nullptr;
  void** ts = nullptr;
  if (const char* why = CheckBuild(etg, vis, &ms, &ts)) {
    Log("focus twin: %s; not installed", why);
    g_state = -1;
    return false;
  }
  if (!gbpass::Install()) {
    Log("focus twin: the G-buffer pass hook is not available; not installed");
    g_state = -1;
    return false;
  }
  Targets2 tg{ms, ts};
  g_instRefused = false;
  if (g_instKey.load() == 1) {
    if (const char* whyI = CheckInstBuild(etg, &tg.instSlot, &tg.instGlobal, &tg.im2Vtbl)) {
      Log("focus twin: %s; trees and buildings stay DCS's", whyI);
      g_instRefused = true;
    }
  }
  ID3D11Device* dev = perfmon::DcsDevice();
  if (!dev) return false;  // retried
  ID3D11DeviceContext* ctx = nullptr;
  dev->GetImmediateContext(&ctx);
  const bool ok = InstallOn(tg, dev, ctx);
  if (ctx) ctx->Release();
  dev->Release();
  if (ok && g_instKey.load() == 1 && !g_instInstalled) g_instRefused = true;
  if (ok)
    Log("focus twin: installed (%s of the focus G-buffer passes replayed from the same eye's periphery; audits "
        "until %d pass per eye, then 1 per %d focus passes%s)",
        g_instInstalled ? (g_patchesKey.load() ? "terrain patch items and instanced trees and buildings"
                                               : "instanced trees and buildings (patches DCS's)")
                        : "terrain patch items",
        g_trustNeeded.load(), g_auditEvery.load(), g_shadowKey.load() ? "; shadow mode: measure only" : "");
  return ok;
}

// Hooks out, workers joined, objects released. False: a hook body did not drain (objects kept alive).
bool Uninstall(const char* why) {
  if (g_state.load() != 1) return true;
  g_active = false;
  gbpass::g_obs2.store(nullptr, std::memory_order_release);
  g_collectTap.store(nullptr, std::memory_order_release);
  UnhookSlot(g_terrSlot, reinterpret_cast<void*>(g_terrOrig));
  UnhookSlot(g_matSlot, reinterpret_cast<void*>(g_matOrig));
  if (g_instInstalled && g_instSlot) UnhookSlot(g_instSlot, reinterpret_cast<void*>(g_instOrig));
  g_instInstalled = false;
  const ULONGLONG t0 = GetTickCount64();
  while (g_inside.load() > 0 && GetTickCount64() - t0 < 2000) Sleep(1);
  if (g_inside.load() > 0) {
    Log("focus twin: a hook was still running at uninstall; its objects are left alive");
    g_state = 0;
    return false;
  }
  g_pool.Stop();
  g_poolOk = false;
  ReleaseAll();
  g_state = 0;
  if (why) Log("focus twin: uninstalled (%s)", why);
  return true;
}

// ---------------------------------------------------------------------------
// Statistics line
// ---------------------------------------------------------------------------
std::string StatsText(const Stats& s, double secs, double frames) {
  char b[3200];
  const double fp = s.focusPasses ? static_cast<double>(s.focusPasses) : 1.0;
  const double fr = frames > 0 ? frames : 1.0;
  // DCS's own patch item, measured on the focus items DCS drew (unmatched, island, audits); the same on the
  // periphery items, which ran under the tee taps: the difference is the taps' cost per teed item (an estimate).
  const double dcsItem = s.focusStockItems ? s.focusStockItemUs / static_cast<double>(s.focusStockItems) : 0.0;
  const uint64_t periphPatch = s.periphItems - s.periphInstItems;
  const double periphItem = periphPatch ? s.periphDcsItemUs / static_cast<double>(periphPatch) : 0.0;
  // R125: instancer items per kind (DCS's own time measured on the focus instancer items DCS drew: audits, islands,
  // unmatched; the periphery's under the tee for the taps' estimate).
  const double dcsInst = s.focusStockInstItems ? s.focusStockInstUs / static_cast<double>(s.focusStockInstItems) : 0.0;
  const double periphInst = s.periphInstItems ? s.periphDcsInstUs / static_cast<double>(s.periphInstItems) : 0.0;
  const uint64_t twinPatch = s.twinItems - s.twinInstItems;
  const double savedPatch = dcsItem * static_cast<double>(twinPatch) / fr;
  const double savedInst = dcsInst * static_cast<double>(s.twinInstItems) / fr;
  const double saved = savedPatch + savedInst;
  const double taps = ((periphItem > dcsItem ? periphItem - dcsItem : 0.0) * static_cast<double>(periphPatch) +
                       (periphInst > dcsInst ? periphInst - dcsInst : 0.0) * static_cast<double>(s.periphInstItems)) /
                      fr;
  const double periph = s.periphOwnUs / fr + taps;
  const double focus = s.focusOwnUs / fr;
  const double gpuT = s.gpuTwinN ? s.gpuTwinUs / static_cast<double>(s.gpuTwinN) : 0.0;
  const double gpuS = s.gpuStockN ? s.gpuStockUs / static_cast<double>(s.gpuStockN) : 0.0;
  int n = snprintf(b, sizeof(b),
                   "focus twin: %.0f s, %llu focus passes: twin %llu (%.0f %%)", secs,
                   static_cast<unsigned long long>(s.focusPasses), static_cast<unsigned long long>(s.why[kWTwin]),
                   100.0 * static_cast<double>(s.why[kWTwin]) / fp);
  for (int w = 1; w < kWCount && n < static_cast<int>(sizeof(b)) - 64; ++w)
    if (s.why[w]) n += snprintf(b + n, sizeof(b) - n, ", %s %llu", WhyName(w), static_cast<unsigned long long>(s.why[w]));
  if (s.why[kWPred]) {
    n += snprintf(b + n, sizeof(b) - n, " (");
    for (int w = 1; w < kPwCount; ++w)
      if (s.predWhy[w]) n += snprintf(b + n, sizeof(b) - n, "%s %llu ", PredWhyName(w), static_cast<unsigned long long>(s.predWhy[w]));
    n += snprintf(b + n, sizeof(b) - n, ")");
  }
  n += snprintf(b + n, sizeof(b) - n, "; items per focus pass: twinned %.1f, DCS's %.1f",
                static_cast<double>(s.twinItems) / fp, static_cast<double>(s.focusItems - s.twinItems) / fp);
  for (int w = 0; w < kICount; ++w)
    if (s.itemWhy[w]) n += snprintf(b + n, sizeof(b) - n, " %s %llu", ItemWhyName(w), static_cast<unsigned long long>(s.itemWhy[w]));
  n += snprintf(b + n, sizeof(b) - n, "; periphery items %.1f/pass, not twinnable",
                s.periphPasses ? static_cast<double>(s.periphItems) / static_cast<double>(s.periphPasses) : 0.0);
  for (int w = 1; w < kPCount; ++w)
    if (s.perWhy[w]) n += snprintf(b + n, sizeof(b) - n, " %s %llu", PerWhyName(w), static_cast<unsigned long long>(s.perWhy[w]));
  if (s.periphInstItems || s.focusInstItems)
    n += snprintf(b + n, sizeof(b) - n,
                  "; instancer: items per focus pass twinned %.1f of %.1f (%.1f sub-draws, %llu without a draw), "
                  "periphery %.1f/pass (%.1f records), %llu sub-draws not drawn in the focus, DCS %.2f us per item (saved ~%.0f us/frame), %llu gated lists",
                  static_cast<double>(s.twinInstItems) / fp, static_cast<double>(s.focusInstItems) / fp,
                  static_cast<double>(s.twinInstDraws) / fp, static_cast<unsigned long long>(s.instNoDraw),
                  s.periphPasses ? static_cast<double>(s.periphInstItems) / static_cast<double>(s.periphPasses) : 0.0,
                  s.periphPasses ? static_cast<double>(s.instRecords) / static_cast<double>(s.periphPasses) : 0.0,
                  static_cast<unsigned long long>(s.instMasked), dcsInst, savedInst, static_cast<unsigned long long>(s.gateLists));
  n += snprintf(b + n, sizeof(b) - n,
                "; render thread per frame: saved ~%.0f us (DCS %.2f us x %.0f items), added %.0f us (periphery "
                "tee %.0f incl. taps ~%.0f, %.2f us per teed item; focus %.0f incl. flushes %.0f, %.2f us per item: %llu "
                "flushes, %llu command lists, %.0f empty draws/frame), net gain %+.0f us; "
                "worker %.0f us/frame (%llu builds, %llu failed, %llu late); GPU focus terrain: twin %.0f us, stock "
                "%.0f us (%+.0f us); prediction max deviation %.2g in %llu checks; audits %llu ok / %llu failed (%llu "
                "items compared); verify %llu (%llu failed, %llu of %llu bytes differ, %llu skipped); %llu windows (%llu "
                "stale), %llu taps, %llu copy writes, %llu swaps refused; faults %d",
                saved, dcsItem, static_cast<double>(twinPatch) / fr, periph + focus, periph, taps,
                s.periphItems ? s.periphItemOwnUs / static_cast<double>(s.periphItems) : 0.0, focus, s.focusFlushUs / fr,
                s.focusItems ? s.focusItemOwnUs / static_cast<double>(s.focusItems) : 0.0,
                static_cast<unsigned long long>(s.flushes),
                static_cast<unsigned long long>(s.ecls),
                static_cast<double>(s.emptyDraws) / fr, saved - (periph + focus), s.buildUs / fr,
                static_cast<unsigned long long>(s.builds), static_cast<unsigned long long>(s.buildFails),
                static_cast<unsigned long long>(s.late), gpuT, gpuS, (s.gpuTwinN && s.gpuStockN) ? gpuT - gpuS : 0.0,
                s.predDevMax, static_cast<unsigned long long>(s.predChecks), static_cast<unsigned long long>(s.auditOk),
                static_cast<unsigned long long>(s.auditFail), static_cast<unsigned long long>(s.auditCompared),
                static_cast<unsigned long long>(s.verifies), static_cast<unsigned long long>(s.verifyFail),
                static_cast<unsigned long long>(s.verifyDiffBytes), static_cast<unsigned long long>(s.verifyBytes),
                static_cast<unsigned long long>(s.verifySkipped),
                static_cast<unsigned long long>(s.windows), static_cast<unsigned long long>(s.staleSkips),
                static_cast<unsigned long long>(s.tapped), static_cast<unsigned long long>(s.copyWrites),
                static_cast<unsigned long long>(s.swapRefused), g_faulted.load() ? 1 : 0);
  return std::string(b, b + (n < static_cast<int>(sizeof(b)) ? n : static_cast<int>(sizeof(b)) - 1));
}
// Worker builds are counted from the streams (render thread reads job state at the focus).
void CountBuilds() {
  for (Eye& e : g_eye)
    for (Stream& s : e.s) {
      const int js = s.job.load(std::memory_order_acquire);
      if ((js == kJOk || js == kJFailed) && s.buildUs > 0) {
        g_st.buildUs += s.buildUs;
        g_st.builds++;
        if (js == kJFailed) g_st.buildFails++;
        s.buildUs = 0;
      }
    }
}

// ---------------------------------------------------------------------------
// Tick (the payload's worker thread, once a second)
// ---------------------------------------------------------------------------
ULONGLONG g_lastLine = 0;
uint64_t g_lineFrames0 = 0;
void Tick(int key, bool engineOff, bool benchRunning, uint64_t frames) {
  const bool want = key == 1 && !engineOff && !benchRunning && !g_faulted.load();
  if (want && g_state.load() == 0) TryInstall();
  if (!want && g_state.load() == 1) Uninstall(g_faulted.load() ? "fault" : engineOff ? "kill switch" : benchRunning ? "benchmark" : "key 0");
  // R125: [Model] FocusTwinInstancer is read at install: a change reinstalls (the next tick); a build the instancer
  // part refused stays as installed until the key changes again.
  if (g_instKey.load() != 1) g_instRefused = false;
  if (want && g_state.load() == 1 && !g_testDevice && (g_instKey.load() == 1) != g_instInstalled && !g_instRefused)
    Uninstall("FocusTwinInstancer changed");
  if (g_state.load() == 1 && g_dev && g_dev->GetDeviceRemovedReason() != S_OK) {
    Log("focus twin: DCS's device was removed; uninstalled");
    Uninstall(nullptr);
  }
  if (g_state.load() == 1 && g_tbl.ctx && g_matSlot && !g_testDevice) {  // DCS on another device: start over on it
    ID3D11Device* now = perfmon::DcsDevice();
    if (now && now != g_dev) Uninstall("DCS's device changed");
    if (now) now->Release();
  }
  g_active = want && g_state.load() == 1;
  const ULONGLONG now = GetTickCount64();
  if (!g_lastLine) g_lastLine = now, g_lineFrames0 = frames;
  if (now - g_lastLine >= 30000) {
    if (g_state.load() == 1 || g_st.focusPasses) {
      CountBuilds();
      Stats copy = g_st;
      g_st = Stats();
      Log("%s", StatsText(copy, (now - g_lastLine) / 1000.0, static_cast<double>(frames - g_lineFrames0)).c_str());
    }
    g_lastLine = now;
    g_lineFrames0 = frames;
  }
}
// Payload stop.
void Shutdown() {
  Uninstall("payload stop");
  g_active = false;
}

}  // namespace ftwin
