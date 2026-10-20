// GPU time per render pass ([Suite] GpuPassTiming, default 0, a 5 s suite
// phase). Measure only: nothing is skipped or changed, and with the key at 0
// none of this is installed.
//
// Timestamps. pass_timing.h's execute hook calls g_boundary at the start and
// the end of every pass execute (nested passes too). On the render thread
// each call ends a D3D11_QUERY_TIMESTAMP on DCS's immediate context; one
// D3D11_QUERY_TIMESTAMP_DISJOINT brackets a frame. kRing frames are in
// flight; at every frame boundary the older ones are read back with
// GetData(D3D11_ASYNC_GETDATA_DONOTFLUSH), never waiting: a frame not ready
// by the time its ring slot comes round again is dropped (counted). Disjoint
// and truncated frames are discarded. A frame ends at the first top-level
// pass after DCS's xrEndFrame wrapper (Visualizer OpenXR vt[10], hooked during
// the phase only, with a timestamp before and after it), or after the frame
// counter moved when that hook is unavailable.
//
// Context ops. During the phase only, the copy / resolve / update / clear /
// mips / dispatch slots of DCS's immediate context table are hooked (count,
// then forward to d3d11's entry). d3d11 rewrites that table (QVFR's
// SwapDeviceContextState in xrEndFrame every frame; Flush and
// ExecuteCommandList toggle draw/copy/clear slots [V split_filter.h]), so the
// hooks are put back at every pass boundary and after xrEndFrame on the render
// thread, taking d3d11's current entry as the original (as d3dstate.h's
// refresher does). d3d11 also switches an entry from inside a copy or clear
// (offline: the first ClearRenderTargetView after a Flush rewrote its slot),
// so each hook re-arms its own slot after forwarding. Calls made while a hook
// was gone are not counted (lower bounds); both re-hook rates are logged.
// Each call counts for the innermost pass running on the render thread.
//
// Coexistence. Query Begin/End/GetData set no pipeline binding [I, D3D11 spec:
// queries are not part of the context state], so split_filter.h's shadow (VS,
// PS, CB, samplers, layout, topology, blend, depth, raster) stays exact; none
// of the slots hooked here is one the split filter or the D3D meter hooks.
// shadow_rec.h's ExecuteCommandList inside the cascade pass lands between that
// pass's two timestamps, so the replay is timed with the pass.
//
// Pipeline statistics ([Suite] GpuPassStats, default 0, only with
// GpuPassTiming and never in light mode): one D3D11_QUERY_PIPELINE_STATISTICS
// and one D3D11_QUERY_OCCLUSION bracket every pass that starts while no such
// pair is open (the outermost passes; the pairs never overlap), read back with
// the frame's timestamps (a frame whose statistics are not ready yet keeps
// its timestamps). Reported per pass kind and, for kinds with several calls,
// per call: input primitives, VS invocations, rasterised primitives, PS
// invocations, depth/stencil-passing samples (shadow cascades: the depth
// writes, compared with the cascade's viewport area), CS invocations.
//
// Identification: kinds whose short name is shared by unrelated passes
// (PassData, SimplePassData) are listed per call with the full RTTI name and
// the code and vtable pointers found in the pass object (module+RVA), so the
// costly calls can be looked up in the binaries.
//
// Included once from main.cpp after pass_timing.h, d3dstate.h, shadow_inst.h,
// split_filter.h, pass_flush.h and the globals it uses (Log, g_qpcToUs, HookSlot).
#pragma once

// Measure-only probes defined later (frame_start.h, run_threads.h); each
// returns at once unless its suite counter is running.
namespace fstart {
void OnXrEndEnter(DWORD tid);
void OnXrEndReturn(DWORD tid);
void OnTopPass(int64_t qpc, uint64_t gpuSerial);
}  // namespace fstart
namespace rthreads {
void OnXrEndEnter(DWORD tid);
}  // namespace rthreads
namespace gpt {
struct FrameSlot;
struct Done;
}  // namespace gpt
// gap_attrib.h ([Suite] GapAttrib, default 0): between-pass gap attribution.
// Every call below is behind gapat::g_on (relaxed load), false unless that
// phase runs.
namespace gapat {
extern std::atomic<bool> g_on;
bool OpBegin(int op);   // CountOp outside passes: true = bracket opened
void OpEnd();           // after the forwarded op
void OnTopBegin();      // top-level pass begin, before its stamp
void OnTopEnd();        // top-level pass end, after its stamp
void OnClose(gpt::FrameSlot& f);
void OnOpen(gpt::FrameSlot& f);
void OnRead(gpt::FrameSlot& f, gpt::Done& d);
}  // namespace gapat

// gpu_dedup.h ([Suite] GpuDedup, default 0): exact-redundancy census. Every
// call below is behind gdd::g_on (relaxed load), false unless that phase runs.
namespace gdd {
extern std::atomic<bool> g_on;
void OnPass(void* pass, bool begin, int depth);
void OnFrameOpen(uint64_t gptSerial);
void OnDeactivate();
void Repatch();
void Withdraw();
void PreDispatch(ID3D11DeviceContext* c, UINT x, UINT y, UINT z);
void PreDispatchIndirect(ID3D11DeviceContext* c, ID3D11Buffer* b, UINT o);
void PostWork();
void PreCopy(ID3D11DeviceContext* c, ID3D11Resource* d, ID3D11Resource* s);
void PreCopySub(ID3D11DeviceContext* c, ID3D11Resource* d, UINT ds, UINT x, UINT y, UINT z, ID3D11Resource* s,
                UINT ss, const D3D11_BOX* b);
void PreResolve(ID3D11DeviceContext* c, ID3D11Resource* d, UINT ds, ID3D11Resource* s, UINT ss, DXGI_FORMAT f);
void PreUpdate(ID3D11DeviceContext* c, ID3D11Resource* d, UINT sub, const D3D11_BOX* b, const void* p, UINT rp,
               UINT dp);
void PreClearRTV(ID3D11DeviceContext* c, ID3D11RenderTargetView* v, const FLOAT col[4]);
void PreClearDSV(ID3D11DeviceContext* c, ID3D11DepthStencilView* v, UINT f, FLOAT d, UINT8 s);
void PreClearUAV(ID3D11DeviceContext* c, ID3D11UnorderedAccessView* v, const void* vals);
void PreMips(ID3D11DeviceContext* c, ID3D11ShaderResourceView* v);
void PostWrite();
}  // namespace gdd

// uav_overlap.h ([Suite] UavOverlap 3/4, default 0): a renamed duplicate of
// each culling-chain dispatch, issued right after it. Behind uovl::g_dup
// (relaxed load), 0 unless that phase runs.
namespace uovl {
extern std::atomic<int> g_dup;
void PostDispatch(ID3D11DeviceContext* c, UINT x, UINT y, UINT z);
void PostDispatchIndirect(ID3D11DeviceContext* c, ID3D11Buffer* b, UINT o);
}  // namespace uovl

// chain_fast.h ([Suite] ChainFastVerify, default 0): same-frame check of the
// culling-chain kernels against the stock ones. Behind chainfast::g_verify
// (relaxed load), false unless that phase runs.
namespace chainfast {
extern std::atomic<bool> g_verify;
void PreDispatch(ID3D11DeviceContext* c);
void PostDispatch(ID3D11DeviceContext* c, UINT x, UINT y, UINT z);
void PostDispatchIndirect(ID3D11DeviceContext* c, ID3D11Buffer* b, UINT o);
}  // namespace chainfast

// shader_fast.h ([D3D] ShaderFast, default 0; R36): while this hook sits on DCS's Dispatch entry, it hands each
// dispatch to TryDispatch, which issues the wider variant grid for an identified kernel (false: not handled).
namespace sfast {
extern std::atomic<bool> g_live;
using DispFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, UINT);
bool TryDispatch(ID3D11DeviceContext* c, UINT x, UINT y, UINT z, DispFn next);
}  // namespace sfast

namespace gpt {

constexpr int kRing = 6;            // frames in flight
constexpr int kMaxEvents = 2048;    // timestamps per frame
constexpr int kMaxDepth = 64;
constexpr int kMaxStats = 256;      // statistics query pairs per frame (outermost passes)

// kOpBegin/kOpEnd/kCal: gap_attrib.h only (pass = the op index, kCal = clock calibration).
enum EvType : uint8_t { kBegin, kEnd, kXrBegin, kXrEnd, kOpBegin, kOpEnd, kCal };
struct Event {
  void* pass;
  uint8_t type;
  uint8_t depth;
};

enum Op {
  kCopyResource, kCopySubresourceRegion, kResolveSubresource, kUpdateSubresource, kClearRTV, kClearDSV,
  kClearUAVUint, kClearUAVFloat, kGenerateMips, kDispatch, kDispatchIndirect, kOpCount
};
const char* const kSlotNames[kOpCount] = {
    "CopyResource",          "CopySubresourceRegion", "ResolveSubresource",           "UpdateSubresource",
    "ClearRenderTargetView", "ClearDepthStencilView", "ClearUnorderedAccessViewUint", "ClearUnorderedAccessViewFloat",
    "GenerateMips",          "Dispatch",              "DispatchIndirect"};
const char* const kOpShort[kOpCount] = {"copy", "copySub", "resolve", "update", "clrRTV", "clrDSV",
                                        "clrUAVu", "clrUAVf", "mips", "disp", "dispInd"};

struct Counts {
  uint64_t n[kOpCount] = {};
};

// A frame read back: its events and their GPU ticks.
struct PassStats {
  void* pass = nullptr;
  uint64_t ia = 0, vs = 0, cPrims = 0, ps = 0, cs = 0, samples = 0;  // input prims, VS, rasterised prims, PS, CS, samples passed
};
struct Done {
  uint64_t serial = 0;
  uint64_t freq = 0;
  int64_t qpc = 0;  // CPU QPC when the frame opened (first top-level pass)
  std::vector<Event> ev;
  std::vector<uint64_t> ticks;
  std::vector<PassStats> stats;  // GpuPassStats: outermost passes in order (empty when off or not ready)
  // gap_attrib.h (empty when off): CPU QPC at each stamp's End, pipeline
  // statistics of each between-pass gap (gapEv = the event index the gap
  // follows), the calibration stamp's QPC after its Flush returned.
  std::vector<int64_t> cpu;
  std::vector<PassStats> gapStats;
  std::vector<int> gapEv;
  int64_t calAfter = 0;
};

struct FrameSlot {
  ID3D11Query* disjoint = nullptr;
  ID3D11Query* ts[kMaxEvents] = {};
  Event ev[kMaxEvents];
  int n = 0;
  bool truncated = false;
  bool pending = false;  // ended, not read yet
  uint64_t serial = 0;
  int64_t qpc = 0;
  // GpuPassStats
  ID3D11Query* pq[kMaxStats] = {};  // pipeline statistics
  ID3D11Query* oq[kMaxStats] = {};  // occlusion (samples passed)
  void* sPass[kMaxStats] = {};
  int ns = 0;
  int sOpen = -1, sDepth = 0;
  bool sBroken = false;  // a pair was ended by the frame end, not by its pass
  // gap_attrib.h
  int64_t cpu[kMaxEvents];
  ID3D11Query* gq[kMaxStats] = {};  // pipeline statistics per between-pass gap
  int gEv[kMaxStats] = {};
  int ng = 0;
  bool gOpen = false;
  int64_t calAfter = 0;
};

// ---------------------------------------------------------------------------
// State. The g_r* fields belong to the render thread while g_active.
// ---------------------------------------------------------------------------

ID3D11Device* g_dev = nullptr;
ID3D11DeviceContext* g_ctx = nullptr;
std::atomic<uint64_t>* g_frameCounter = nullptr;
FrameSlot* g_ring = nullptr;
std::atomic<bool> g_queriesReady{false};
bool g_createOnRenderThread = false;  // single-threaded device

std::atomic<int> g_want{0};          // suite: 1 measure, 0 stop
// Light mode (view_profile.h's YawProfile / RotationProfile): timestamps only,
// no context-op hooks, no table re-patching, no per-pass op counters; set
// before g_want and fixed while active.
std::atomic<bool> g_light{false};
// GpuPassStats: set by the suite before Measure; g_statsOn is fixed per session.
std::atomic<bool> g_statsWanted{false};
std::atomic<bool> g_statsOn{false};
// xrEndFrame wrapper wall time on the render thread (QPC ticks, calls), while
// the xrEndFrame hook is installed; read as differences.
std::atomic<uint64_t> g_xrWallTicks{0}, g_xrWallCalls{0};
std::atomic<bool> g_active{false};   // render thread is measuring
std::atomic<DWORD> g_rt{0};          // the render thread (latched at activation)
std::atomic<DWORD> g_xrTid{0};       // thread calling the xrEndFrame wrapper

int g_rOpen = -1;                    // ring index of the open frame
uint64_t g_rSerial = 0;
uint64_t g_rFrameAtOpen = 0;
bool g_rXrSeen = false;              // xrEndFrame ended since the frame opened
Counts* g_rStack[kMaxDepth];
int g_rSp = 0;
Counts* g_rSavedCur = nullptr;
uint64_t g_rOverheadTicks = 0;       // QPC ticks spent in our render-thread code

thread_local Counts* t_cur = nullptr;  // innermost pass on the render thread
std::unordered_map<void*, Counts> g_passCounts;  // render thread while active
Counts g_xrCounts;
std::atomic<uint64_t> g_outside[kOpCount];

struct Stats {
  uint64_t framesOpened, framesRead, disjoint, truncated, dropped, missing, rehooks, xrCalls, xrNested,
      offThread, createFailed, rearms, statsRead, statsLate, statsFull;
};
Stats g_s{};

std::mutex g_doneMutex;
std::vector<Done> g_done;

// ---------------------------------------------------------------------------
// Context table hooks (count, forward)
// ---------------------------------------------------------------------------

using Ctx = ID3D11DeviceContext;
void** g_table = nullptr;
int g_slot[kOpCount] = {};
void* volatile g_orig[kOpCount] = {};
std::atomic<bool> g_opsHooked{false};

inline void CountOp(Ctx* c, int op) {
  if (c != g_ctx) return;
  if (Counts* k = t_cur) {
    k->n[op]++;
  } else {
    g_outside[op].fetch_add(1, std::memory_order_relaxed);
    if (gapat::g_on.load(std::memory_order_relaxed)) gapat::OpBegin(op);
  }
}
template <class F>
inline F Orig(int op) {
  return reinterpret_cast<F>(g_orig[op]);
}
void* HookOf(int op);
bool g_tableWritable = false;  // DCS's table is inside the context object (heap)
// After the forwarded call: d3d11 switches an entry to another variant from
// inside its own implementation (the first copy/clear after a Flush rewrites
// the slot, offline on this PC), which would drop the hook for the rest of the
// pass. Take the new entry as the original and hook again at once.
inline void Rearm(Ctx* c, int op) {
  if (gapat::g_on.load(std::memory_order_relaxed)) gapat::OpEnd();
  void** vt = g_table;
  if (*reinterpret_cast<void***>(c) != vt || !g_opsHooked.load(std::memory_order_relaxed)) return;
  void* cur = vt[g_slot[op]];
  if (cur == HookOf(op)) return;
  g_orig[op] = cur;
  if (g_tableWritable)
    InterlockedExchangePointer(&vt[g_slot[op]], HookOf(op));
  else
    d3ds::WriteTablePointer(&vt[g_slot[op]], HookOf(op));
  ++g_s.rearms;
}

void STDMETHODCALLTYPE H_CopyResource(Ctx* c, ID3D11Resource* d, ID3D11Resource* s) {
  CountOp(c, kCopyResource);
  if (gdd::g_on.load(std::memory_order_relaxed)) gdd::PreCopy(c, d, s);
  Orig<decltype(&H_CopyResource)>(kCopyResource)(c, d, s);
  if (gdd::g_on.load(std::memory_order_relaxed)) gdd::PostWrite();
  Rearm(c, kCopyResource);
}
void STDMETHODCALLTYPE H_CopySubresourceRegion(Ctx* c, ID3D11Resource* d, UINT ds, UINT x, UINT y, UINT z,
                                               ID3D11Resource* s, UINT ss, const D3D11_BOX* b) {
  CountOp(c, kCopySubresourceRegion);
  if (gdd::g_on.load(std::memory_order_relaxed)) gdd::PreCopySub(c, d, ds, x, y, z, s, ss, b);
  Orig<decltype(&H_CopySubresourceRegion)>(kCopySubresourceRegion)(c, d, ds, x, y, z, s, ss, b);
  if (gdd::g_on.load(std::memory_order_relaxed)) gdd::PostWrite();
  Rearm(c, kCopySubresourceRegion);
}
void STDMETHODCALLTYPE H_ResolveSubresource(Ctx* c, ID3D11Resource* d, UINT ds, ID3D11Resource* s, UINT ss,
                                            DXGI_FORMAT f) {
  CountOp(c, kResolveSubresource);
  if (gdd::g_on.load(std::memory_order_relaxed)) gdd::PreResolve(c, d, ds, s, ss, f);
  Orig<decltype(&H_ResolveSubresource)>(kResolveSubresource)(c, d, ds, s, ss, f);
  if (gdd::g_on.load(std::memory_order_relaxed)) gdd::PostWrite();
  Rearm(c, kResolveSubresource);
}
void STDMETHODCALLTYPE H_UpdateSubresource(Ctx* c, ID3D11Resource* d, UINT ds, const D3D11_BOX* b, const void* p,
                                           UINT rp, UINT dp) {
  CountOp(c, kUpdateSubresource);
  if (gdd::g_on.load(std::memory_order_relaxed)) gdd::PreUpdate(c, d, ds, b, p, rp, dp);
  Orig<decltype(&H_UpdateSubresource)>(kUpdateSubresource)(c, d, ds, b, p, rp, dp);
  if (gdd::g_on.load(std::memory_order_relaxed)) gdd::PostWrite();
  Rearm(c, kUpdateSubresource);
}
void STDMETHODCALLTYPE H_ClearRenderTargetView(Ctx* c, ID3D11RenderTargetView* v, const FLOAT col[4]) {
  CountOp(c, kClearRTV);
  if (gdd::g_on.load(std::memory_order_relaxed)) gdd::PreClearRTV(c, v, col);
  Orig<decltype(&H_ClearRenderTargetView)>(kClearRTV)(c, v, col);
  if (gdd::g_on.load(std::memory_order_relaxed)) gdd::PostWrite();
  Rearm(c, kClearRTV);
}
void STDMETHODCALLTYPE H_ClearDepthStencilView(Ctx* c, ID3D11DepthStencilView* v, UINT f, FLOAT d, UINT8 s) {
  CountOp(c, kClearDSV);
  if (gdd::g_on.load(std::memory_order_relaxed)) gdd::PreClearDSV(c, v, f, d, s);
  Orig<decltype(&H_ClearDepthStencilView)>(kClearDSV)(c, v, f, d, s);
  if (gdd::g_on.load(std::memory_order_relaxed)) gdd::PostWrite();
  Rearm(c, kClearDSV);
}
void STDMETHODCALLTYPE H_ClearUAVUint(Ctx* c, ID3D11UnorderedAccessView* v, const UINT val[4]) {
  CountOp(c, kClearUAVUint);
  if (gdd::g_on.load(std::memory_order_relaxed)) gdd::PreClearUAV(c, v, val);
  Orig<decltype(&H_ClearUAVUint)>(kClearUAVUint)(c, v, val);
  if (gdd::g_on.load(std::memory_order_relaxed)) gdd::PostWrite();
  Rearm(c, kClearUAVUint);
}
void STDMETHODCALLTYPE H_ClearUAVFloat(Ctx* c, ID3D11UnorderedAccessView* v, const FLOAT val[4]) {
  CountOp(c, kClearUAVFloat);
  if (gdd::g_on.load(std::memory_order_relaxed)) gdd::PreClearUAV(c, v, val);
  Orig<decltype(&H_ClearUAVFloat)>(kClearUAVFloat)(c, v, val);
  if (gdd::g_on.load(std::memory_order_relaxed)) gdd::PostWrite();
  Rearm(c, kClearUAVFloat);
}
void STDMETHODCALLTYPE H_GenerateMips(Ctx* c, ID3D11ShaderResourceView* v) {
  CountOp(c, kGenerateMips);
  if (gdd::g_on.load(std::memory_order_relaxed)) gdd::PreMips(c, v);
  Orig<decltype(&H_GenerateMips)>(kGenerateMips)(c, v);
  if (gdd::g_on.load(std::memory_order_relaxed)) gdd::PostWrite();
  Rearm(c, kGenerateMips);
}
void STDMETHODCALLTYPE H_Dispatch(Ctx* c, UINT x, UINT y, UINT z) {
  CountOp(c, kDispatch);
  if (gdd::g_on.load(std::memory_order_relaxed)) gdd::PreDispatch(c, x, y, z);
  const bool cfv = chainfast::g_verify.load(std::memory_order_relaxed);
  if (cfv) chainfast::PreDispatch(c);
  if (!sfast::g_live.load(std::memory_order_relaxed) ||
      !sfast::TryDispatch(c, x, y, z, reinterpret_cast<sfast::DispFn>(Orig<decltype(&H_Dispatch)>(kDispatch))))
    Orig<decltype(&H_Dispatch)>(kDispatch)(c, x, y, z);
  if (cfv) chainfast::PostDispatch(c, x, y, z);
  if (gdd::g_on.load(std::memory_order_relaxed)) gdd::PostWork();
  if (uovl::g_dup.load(std::memory_order_relaxed)) uovl::PostDispatch(c, x, y, z);
  Rearm(c, kDispatch);
}
void STDMETHODCALLTYPE H_DispatchIndirect(Ctx* c, ID3D11Buffer* b, UINT o) {
  CountOp(c, kDispatchIndirect);
  if (gdd::g_on.load(std::memory_order_relaxed)) gdd::PreDispatchIndirect(c, b, o);
  const bool cfv = chainfast::g_verify.load(std::memory_order_relaxed);
  if (cfv) chainfast::PreDispatch(c);
  Orig<decltype(&H_DispatchIndirect)>(kDispatchIndirect)(c, b, o);
  if (cfv) chainfast::PostDispatchIndirect(c, b, o);
  if (gdd::g_on.load(std::memory_order_relaxed)) gdd::PostWork();
  if (uovl::g_dup.load(std::memory_order_relaxed)) uovl::PostDispatchIndirect(c, b, o);
  Rearm(c, kDispatchIndirect);
}

void* const kHooks[kOpCount] = {
    reinterpret_cast<void*>(&H_CopyResource),       reinterpret_cast<void*>(&H_CopySubresourceRegion),
    reinterpret_cast<void*>(&H_ResolveSubresource), reinterpret_cast<void*>(&H_UpdateSubresource),
    reinterpret_cast<void*>(&H_ClearRenderTargetView), reinterpret_cast<void*>(&H_ClearDepthStencilView),
    reinterpret_cast<void*>(&H_ClearUAVUint),       reinterpret_cast<void*>(&H_ClearUAVFloat),
    reinterpret_cast<void*>(&H_GenerateMips),       reinterpret_cast<void*>(&H_Dispatch),
    reinterpret_cast<void*>(&H_DispatchIndirect)};
void* HookOf(int op) { return kHooks[op]; }

// Puts our hooks back where d3d11 wrote its own entry, taking that entry as
// the original. Render thread (or the stopping thread when nothing renders).
void RepatchOps() {
  void** vt = g_table;
  for (int i = 0; i < kOpCount; ++i) {
    void* cur = vt[g_slot[i]];
    if (cur == kHooks[i]) continue;
    g_orig[i] = cur;  // visible before the hook (both writes below are full barriers)
    if (g_tableWritable)
      InterlockedExchangePointer(&vt[g_slot[i]], kHooks[i]);
    else
      d3ds::WriteTablePointer(&vt[g_slot[i]], kHooks[i]);
    ++g_s.rehooks;
  }
  if (gdd::g_on.load(std::memory_order_relaxed)) gdd::Repatch();
}

bool SafeRepatch() {
  __try {
    RepatchOps();
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

void WithdrawOps() {
  gdd::Withdraw();  // no-op unless gpu_dedup.h hooked its slots
  if (!g_opsHooked.exchange(false)) return;
  __try {
    for (int i = 0; i < kOpCount; ++i)
      if (g_table[g_slot[i]] == kHooks[i]) d3ds::WriteTablePointer(&g_table[g_slot[i]], g_orig[i]);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }
}

bool HookOps() {
  g_table = *reinterpret_cast<void***>(g_ctx);
  MEMORY_BASIC_INFORMATION mbi{};
  g_tableWritable = VirtualQuery(&g_table[g_slot[0]], &mbi, sizeof(mbi)) &&
                    (mbi.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE)) && mbi.State == MEM_COMMIT;
  for (int i = 0; i < kOpCount; ++i) g_orig[i] = g_table[g_slot[i]];
  g_opsHooked = true;
  const uint64_t before = g_s.rehooks;
  if (!SafeRepatch()) {
    g_opsHooked = false;
    return false;
  }
  g_s.rehooks = before;  // the initial patch is not a re-hook
  return true;
}

// ---------------------------------------------------------------------------
// Queries and frames
// ---------------------------------------------------------------------------

void ReleaseQueries() {
  if (!g_ring) return;
  for (int s = 0; s < kRing; ++s) {
    FrameSlot& f = g_ring[s];
    if (f.disjoint) f.disjoint->Release();
    f.disjoint = nullptr;
    for (auto*& q : f.ts) {
      if (q) q->Release();
      q = nullptr;
    }
    for (auto*& q : f.pq) {
      if (q) q->Release();
      q = nullptr;
    }
    for (auto*& q : f.oq) {
      if (q) q->Release();
      q = nullptr;
    }
    for (auto*& q : f.gq) {  // gap_attrib.h
      if (q) q->Release();
      q = nullptr;
    }
    f.ng = 0;
    f.gOpen = false;
    f.pending = false;
  }
  g_queriesReady = false;
}

bool CreateQueries() {
  if (g_queriesReady.load()) return true;
  if (!g_ring) g_ring = new (std::nothrow) FrameSlot[kRing];
  if (!g_ring) return false;
  D3D11_QUERY_DESC dj{D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
  D3D11_QUERY_DESC ts{D3D11_QUERY_TIMESTAMP, 0};
  for (int s = 0; s < kRing; ++s) {
    FrameSlot& f = g_ring[s];
    if (!f.disjoint && FAILED(g_dev->CreateQuery(&dj, &f.disjoint))) {
      f.disjoint = nullptr;
      ReleaseQueries();
      return false;
    }
    for (auto*& q : f.ts)
      if (!q && FAILED(g_dev->CreateQuery(&ts, &q))) {
        q = nullptr;
        ReleaseQueries();
        return false;
      }
    if (g_statsOn.load()) {
      D3D11_QUERY_DESC pd{D3D11_QUERY_PIPELINE_STATISTICS, 0};
      D3D11_QUERY_DESC od{D3D11_QUERY_OCCLUSION, 0};
      for (int i = 0; i < kMaxStats && g_statsOn.load(); ++i)
        if ((!f.pq[i] && FAILED(g_dev->CreateQuery(&pd, &f.pq[i]))) ||
            (!f.oq[i] && FAILED(g_dev->CreateQuery(&od, &f.oq[i]))))
          g_statsOn = false;  // timestamps only (the created ones are released with the rest)
    }
    f.n = 0;
    f.ns = 0;
    f.sOpen = -1;
    f.pending = false;
  }
  g_queriesReady = true;
  return true;
}

// Non-blocking read of one ended frame. True when the slot is free again.
bool TryRead(FrameSlot& f) {
  D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj{};
  if (g_ctx->GetData(f.disjoint, &dj, sizeof(dj), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) return false;
  f.pending = false;
  if (dj.Disjoint || !dj.Frequency) {
    ++g_s.disjoint;
    return true;
  }
  if (f.truncated) {
    ++g_s.truncated;
    return true;
  }
  Done d;
  d.serial = f.serial;
  d.qpc = f.qpc;
  d.freq = dj.Frequency;
  d.ticks.resize(f.n);
  for (int i = 0; i < f.n; ++i)
    if (g_ctx->GetData(f.ts[i], &d.ticks[i], sizeof(uint64_t), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) {
      ++g_s.missing;  // cannot happen once the disjoint query is done [I]; discarded
      return true;
    }
  d.ev.assign(f.ev, f.ev + f.n);
  if (g_statsOn.load(std::memory_order_relaxed) && f.ns && !f.sBroken) {
    d.stats.resize(f.ns);
    for (int i = 0; i < f.ns; ++i) {
      D3D11_QUERY_DATA_PIPELINE_STATISTICS p{};
      UINT64 smp = 0;
      if (g_ctx->GetData(f.pq[i], &p, sizeof(p), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK ||
          g_ctx->GetData(f.oq[i], &smp, sizeof(smp), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) {
        d.stats.clear();  // not ready with the timestamps: this frame's timing is kept, its statistics dropped
        ++g_s.statsLate;
        break;
      }
      d.stats[i] = {f.sPass[i], p.IAPrimitives, p.VSInvocations, p.CPrimitives, p.PSInvocations, p.CSInvocations, smp};
    }
    if (!d.stats.empty()) ++g_s.statsRead;
  }
  if (gapat::g_on.load(std::memory_order_relaxed)) gapat::OnRead(f, d);
  ++g_s.framesRead;
  std::lock_guard<std::mutex> lock(g_doneMutex);
  g_done.push_back(std::move(d));
  return true;
}

// Reads every ended frame that is ready, oldest first.
void Poll() {
  for (uint64_t k = g_rSerial > kRing ? g_rSerial - kRing : 0; k < g_rSerial; ++k) {
    FrameSlot& f = g_ring[k % kRing];
    if (f.pending && f.serial == k && !TryRead(f)) break;  // later frames are not done either
  }
}

void Stamp(EvType type, void* pass, int depth) {
  if (g_rOpen < 0) return;
  FrameSlot& f = g_ring[g_rOpen];
  if (f.n >= kMaxEvents) {
    f.truncated = true;
    return;
  }
  if (gapat::g_on.load(std::memory_order_relaxed)) {
    LARGE_INTEGER q;
    QueryPerformanceCounter(&q);
    f.cpu[f.n] = q.QuadPart;  // issue time: the stamp cannot run on the GPU before it
  }
  g_ctx->End(f.ts[f.n]);
  f.ev[f.n] = {pass, static_cast<uint8_t>(type), static_cast<uint8_t>(depth < 255 ? depth : 255)};
  ++f.n;
}

void OpenFrame() {
  const int s = static_cast<int>(g_rSerial % kRing);
  FrameSlot& f = g_ring[s];
  if (f.pending && !TryRead(f)) {
    ++g_s.dropped;
    f.pending = false;
  }
  f.n = 0;
  f.ns = 0;
  f.sOpen = -1;
  f.sBroken = false;
  f.truncated = false;
  f.serial = g_rSerial++;
  LARGE_INTEGER q;
  QueryPerformanceCounter(&q);
  f.qpc = q.QuadPart;
  if (gapat::g_on.load(std::memory_order_relaxed)) gapat::OnOpen(f);
  g_ctx->Begin(f.disjoint);
  g_rOpen = s;
  g_rXrSeen = false;
  g_rFrameAtOpen = g_frameCounter ? g_frameCounter->load(std::memory_order_relaxed) : 0;
  ++g_s.framesOpened;
  if (gdd::g_on.load(std::memory_order_relaxed)) gdd::OnFrameOpen(f.serial);
}

// GpuPassStats: the outermost pass's query pair (render thread, non-light).
void StatsBoundary(void* pass, bool begin, int depth) {
  if (g_rOpen < 0) return;
  FrameSlot& f = g_ring[g_rOpen];
  if (begin) {
    if (f.sOpen >= 0) return;
    if (f.ns >= kMaxStats) {
      ++g_s.statsFull;
      return;
    }
    g_ctx->Begin(f.pq[f.ns]);
    g_ctx->Begin(f.oq[f.ns]);
    f.sPass[f.ns] = pass;
    f.sOpen = f.ns++;
    f.sDepth = depth;
  } else if (f.sOpen >= 0 && depth == f.sDepth && f.sPass[f.sOpen] == pass) {
    g_ctx->End(f.pq[f.sOpen]);
    g_ctx->End(f.oq[f.sOpen]);
    f.sOpen = -1;
  }
}

void CloseFrame() {
  if (g_rOpen < 0) return;
  FrameSlot& f = g_ring[g_rOpen];
  if (f.sOpen >= 0) {
    g_ctx->End(f.pq[f.sOpen]);
    g_ctx->End(f.oq[f.sOpen]);
    f.sOpen = -1;
    f.sBroken = true;
  }
  if (gapat::g_on.load(std::memory_order_relaxed)) gapat::OnClose(f);
  g_ctx->End(f.disjoint);
  f.pending = true;
  g_rOpen = -1;
  Poll();
}

bool Activate() {
  if (!g_queriesReady.load()) {
    if (!g_createOnRenderThread || !CreateQueries()) {
      ++g_s.createFailed;
      g_want = 0;
      return false;
    }
  }
  if (!g_light.load() && !HookOps()) {
    ++g_s.createFailed;
    g_want = 0;
    return false;
  }
  g_rOpen = -1;
  g_rSp = 0;
  t_cur = nullptr;
  g_rt = GetCurrentThreadId();
  g_active = true;
  return true;
}

// Render thread: frames still in flight get one last non-blocking read, the
// open one is abandoned, the hooks go and the queries are released.
void Deactivate() {
  if (g_rOpen >= 0) {
    FrameSlot& f = g_ring[g_rOpen];
    if (f.sOpen >= 0) {
      g_ctx->End(f.pq[f.sOpen]);
      g_ctx->End(f.oq[f.sOpen]);
      f.sOpen = -1;
    }
    if (gapat::g_on.load(std::memory_order_relaxed)) gapat::OnClose(f);
    g_ctx->End(g_ring[g_rOpen].disjoint);
    g_rOpen = -1;
  }
  if (gdd::g_on.load(std::memory_order_relaxed)) gdd::OnDeactivate();
  Poll();
  WithdrawOps();
  t_cur = nullptr;
  g_rSp = 0;
  ReleaseQueries();
  g_active = false;
}

// Top-level pass start on the render thread: activation, frame boundaries.
void TopBoundary() {
  const bool want = g_want.load(std::memory_order_relaxed) != 0;
  if (!g_active.load(std::memory_order_relaxed)) {
    if (!want || !Activate()) return;
  } else if (!want) {
    Deactivate();
    return;
  }
  if (g_rOpen >= 0) {
    const uint64_t fc = g_frameCounter ? g_frameCounter->load(std::memory_order_relaxed) : 0;
    const bool counterMoved = fc != g_rFrameAtOpen;
    if (g_rXrSeen || (!g_s.xrCalls && counterMoved) || fc - g_rFrameAtOpen >= 2) CloseFrame();
  }
  if (g_rOpen < 0) OpenFrame();
}

bool OnRenderThread(DWORD tid) {
  const DWORD xr = g_xrTid.load(std::memory_order_relaxed);
  if (g_active.load(std::memory_order_relaxed)) return tid == g_rt.load(std::memory_order_relaxed);
  return !xr || tid == xr;  // activation only on the thread that ends the OpenXR frame, once known
}

// ptiming::g_boundary.
void OnBoundary(void* pass, bool begin, int depth) {
  const DWORD tid = GetCurrentThreadId();
  if (!OnRenderThread(tid)) {
    if (g_active.load(std::memory_order_relaxed)) ++g_s.offThread;  // racy count; diagnostic only
    return;
  }
  LARGE_INTEGER a, b;
  QueryPerformanceCounter(&a);
  if (begin && depth == 0) {
    TopBoundary();
    fstart::OnTopPass(a.QuadPart, g_active.load(std::memory_order_relaxed) && g_rOpen >= 0 ? g_ring[g_rOpen].serial
                                                                                            : ~0ull);
  }
  if (g_active.load(std::memory_order_relaxed) && g_light.load(std::memory_order_relaxed)) {
    Stamp(begin ? kBegin : kEnd, pass, depth);
  } else if (g_active.load(std::memory_order_relaxed)) {
    if (g_opsHooked.load(std::memory_order_relaxed)) RepatchOps();
    if (begin) {
      if (depth == 0 && gapat::g_on.load(std::memory_order_relaxed)) gapat::OnTopBegin();
      Stamp(kBegin, pass, depth);
      if (g_statsOn.load(std::memory_order_relaxed)) StatsBoundary(pass, true, depth);
      if (g_rSp < kMaxDepth) g_rStack[g_rSp] = t_cur;
      ++g_rSp;
      t_cur = &g_passCounts[pass];
      if (gdd::g_on.load(std::memory_order_relaxed)) gdd::OnPass(pass, true, depth);
    } else {
      if (gdd::g_on.load(std::memory_order_relaxed)) gdd::OnPass(pass, false, depth);
      if (g_rSp > 0 && --g_rSp < kMaxDepth) t_cur = g_rStack[g_rSp];
      if (g_rSp == 0) t_cur = nullptr;
      if (g_statsOn.load(std::memory_order_relaxed)) StatsBoundary(pass, false, depth);
      Stamp(kEnd, pass, depth);
      if (depth == 0 && gapat::g_on.load(std::memory_order_relaxed)) gapat::OnTopEnd();
    }
  }
  QueryPerformanceCounter(&b);
  g_rOverheadTicks += b.QuadPart - a.QuadPart;
}

// ---------------------------------------------------------------------------
// xrEndFrame wrapper: DCS's OpenXR vt[10] (4 x xrReleaseSwapchainImage +
// xrEndFrame; QVFR composes and the runtime submits inside it).
// [V Visualizer.dll RVA 0x124630, vtable RTTI .?AVOpenXR@@, R11/R15 notes;
// two register arguments (this, frame info), checked offline.]
// ---------------------------------------------------------------------------

constexpr int kXrEndSlot = 10;
constexpr uint32_t kXrEndRva = 0x124630;
const uint8_t kXrEndPrologue[] = {0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57};
using XrEndFn = uint64_t(__fastcall*)(void*, void*, void*, void*);
XrEndFn g_xrOrig = nullptr;
void** g_xrSlot = nullptr;

uint64_t __fastcall XrEndHook(void* a, void* b, void* c, void* d) {
  const DWORD tid = GetCurrentThreadId();
  g_xrTid.store(tid, std::memory_order_relaxed);
  const bool on = g_active.load(std::memory_order_relaxed) && tid == g_rt.load(std::memory_order_relaxed);
  LARGE_INTEGER w0, w1;
  if (!on) {
    fstart::OnXrEndEnter(tid);
    rthreads::OnXrEndEnter(tid);
    QueryPerformanceCounter(&w0);
    const uint64_t r = g_xrOrig(a, b, c, d);
    QueryPerformanceCounter(&w1);
    fstart::OnXrEndReturn(tid);
    g_xrWallTicks.fetch_add(static_cast<uint64_t>(w1.QuadPart - w0.QuadPart), std::memory_order_relaxed);
    g_xrWallCalls.fetch_add(1, std::memory_order_relaxed);
    pflush::NoteXrEnd();  // bench mode 30's frame-start probe
    return r;
  }
  const bool light = g_light.load(std::memory_order_relaxed);
  LARGE_INTEGER t0, t1;
  QueryPerformanceCounter(&t0);
  ++g_s.xrCalls;
  if (ptiming::t_depth != 0) ++g_s.xrNested;
  if (!light && g_opsHooked.load(std::memory_order_relaxed)) RepatchOps();
  Stamp(kXrBegin, nullptr, ptiming::t_depth);
  g_rSavedCur = t_cur;
  if (!light) t_cur = &g_xrCounts;
  QueryPerformanceCounter(&t1);
  g_rOverheadTicks += t1.QuadPart - t0.QuadPart;
  fstart::OnXrEndEnter(tid);
  rthreads::OnXrEndEnter(tid);
  QueryPerformanceCounter(&w0);
  const uint64_t r = g_xrOrig(a, b, c, d);
  QueryPerformanceCounter(&t0);
  fstart::OnXrEndReturn(tid);
  g_xrWallTicks.fetch_add(static_cast<uint64_t>(t0.QuadPart - w0.QuadPart), std::memory_order_relaxed);
  g_xrWallCalls.fetch_add(1, std::memory_order_relaxed);
  t_cur = g_rSavedCur;
  // QVFR's SwapDeviceContextState rewrote the table: back before DCS's next calls.
  if (!light && g_opsHooked.load(std::memory_order_relaxed)) RepatchOps();
  Stamp(kXrEnd, nullptr, ptiming::t_depth);
  g_rXrSeen = true;
  QueryPerformanceCounter(&t1);
  g_rOverheadTicks += t1.QuadPart - t0.QuadPart;
  pflush::NoteXrEnd();
  return r;
}

bool CodeMatches(const uint8_t* p, const uint8_t* want, size_t n) {
  __try {
    return memcmp(p, want, n) == 0;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

bool InstallXr() {
  if (g_xrSlot) return true;
  HMODULE vis = GetModuleHandleW(L"Visualizer.dll");
  if (!vis) {
    Log("  gpu pass timing: Visualizer.dll not loaded; xrEndFrame not timed");
    return false;
  }
  int n = 0;
  void** vt = sigscan::FindVtable(vis, ".?AVOpenXR@@", 0, false, &n);
  if (!vt) {
    Log("  gpu pass timing: OpenXR vtable not found (%d candidates); xrEndFrame not timed", n);
    return false;
  }
  auto* fn = static_cast<uint8_t*>(SlotOriginal(&vt[kXrEndSlot]));
  const uintptr_t rva = reinterpret_cast<uintptr_t>(fn) - reinterpret_cast<uintptr_t>(vis);
  if (rva != kXrEndRva || !CodeMatches(fn, kXrEndPrologue, sizeof(kXrEndPrologue))) {
    Log("  gpu pass timing: OpenXR vt[%d] is Visualizer+0x%llx, not the analysed 0x%x; xrEndFrame not timed",
        kXrEndSlot, static_cast<unsigned long long>(rva), kXrEndRva);
    return false;
  }
  g_xrOrig = reinterpret_cast<XrEndFn>(fn);  // before the slot points at the hook
  if (!HookSlot(&vt[kXrEndSlot], reinterpret_cast<void*>(&XrEndHook), nullptr)) {
    Log("  gpu pass timing: could not hook OpenXR vt[%d]; xrEndFrame not timed", kXrEndSlot);
    return false;
  }
  g_xrSlot = &vt[kXrEndSlot];
  return true;
}

void UninstallXr() {
  if (!g_xrSlot) return;
  UnhookSlot(g_xrSlot, reinterpret_cast<void*>(g_xrOrig));
  g_xrSlot = nullptr;
}

// ---------------------------------------------------------------------------
// Analysis (suite thread, offline-testable)
// ---------------------------------------------------------------------------

struct KindStat {
  std::vector<double> perFrame;  // exclusive GPU ms per read frame
  double inclMs = 0;
  uint64_t calls = 0;
  std::vector<double> occMs;     // exclusive GPU ms by call order within the frame
  std::vector<uint64_t> occN;
  std::vector<void*> occPass;    // the pass object of each call (last frame seen)
};
// GpuPassStats per kind (sums over the frames that have statistics).
struct KindPipe {
  uint64_t frames = 0;  // frames with statistics in which the kind ran
  PassStats sum;
  std::vector<PassStats> occ;   // by call order (outermost calls of the kind)
  std::vector<uint64_t> occN;
};
struct Result {
  int frames = 0;
  std::map<std::string, KindStat> kinds;
  std::vector<double> span;      // first top-level pass start -> last top-level pass end
  std::vector<double> gaps;      // sum of GPU time between consecutive top-level passes
  std::vector<double> toXr;      // last top-level pass end -> xrEndFrame wrapper start
  std::vector<double> xr;        // xrEndFrame wrapper start -> end
  std::vector<double> period;    // first pass start -> next frame's first pass start
  std::vector<double> afterXr;   // xrEndFrame end (else last pass end) -> next frame's first pass start
  uint64_t unmatched = 0;
  int statFrames = 0;            // frames with pipeline statistics
  std::map<std::string, KindPipe> pipe;
};

Result Analyze(std::vector<Done>& done, std::string (*nameOf)(void*)) {
  Result r;
  std::sort(done.begin(), done.end(), [](const Done& a, const Done& b) { return a.serial < b.serial; });
  std::unordered_map<void*, std::string> names;
  auto name = [&](void* p) -> const std::string& {
    auto it = names.find(p);
    if (it == names.end()) it = names.emplace(p, nameOf(p)).first;
    return it->second;
  };
  struct Open {
    void* pass;
    uint64_t t, child;
  };
  struct Edge {
    uint64_t serial, freq;
    int64_t first, end;  // first pass start; xrEndFrame end, else last pass end (-1 unknown)
  };
  std::vector<Edge> edges;
  std::vector<Open> st;
  for (const Done& d : done) {
    const double ms = 1000.0 / static_cast<double>(d.freq);
    const int fi = r.frames++;
    st.clear();
    int64_t firstTop = -1, lastTopEnd = -1, xrB = -1, xrE = -1;
    uint64_t gap = 0;
    std::map<std::string, double> local;
    std::map<std::string, int> occ;
    for (size_t i = 0; i < d.ev.size(); ++i) {
      const Event& e = d.ev[i];
      const uint64_t t = d.ticks[i];
      switch (e.type) {
        case kBegin:
          if (st.empty()) {
            if (firstTop < 0) firstTop = static_cast<int64_t>(t);
            if (lastTopEnd >= 0 && t > static_cast<uint64_t>(lastTopEnd)) gap += t - lastTopEnd;
          }
          st.push_back({e.pass, t, 0});
          break;
        case kEnd: {
          if (st.empty() || st.back().pass != e.pass) {
            ++r.unmatched;
            st.clear();
            break;
          }
          const Open o = st.back();
          st.pop_back();
          const uint64_t incl = t > o.t ? t - o.t : 0;
          const uint64_t excl = incl > o.child ? incl - o.child : 0;
          if (!st.empty())
            st.back().child += incl;
          else
            lastTopEnd = static_cast<int64_t>(t);
          const std::string& nm = name(e.pass);
          local[nm] += excl * ms;
          KindStat& k = r.kinds[nm];
          k.inclMs += incl * ms;
          ++k.calls;
          const size_t n = static_cast<size_t>(occ[nm]++);
          if (k.occMs.size() <= n) {
            k.occMs.resize(n + 1, 0.0);
            k.occN.resize(n + 1, 0);
            k.occPass.resize(n + 1, nullptr);
          }
          k.occMs[n] += excl * ms;
          ++k.occN[n];
          k.occPass[n] = e.pass;
          break;
        }
        case kXrBegin:
          xrB = static_cast<int64_t>(t);
          break;
        case kXrEnd:
          xrE = static_cast<int64_t>(t);
          break;
      }
    }
    r.unmatched += st.size();
    if (!d.stats.empty()) {
      ++r.statFrames;
      std::map<std::string, int> socc;
      for (const PassStats& p : d.stats) {
        const std::string& nm = name(p.pass);
        KindPipe& kp = r.pipe[nm];
        const size_t n = static_cast<size_t>(socc[nm]++);
        if (n == 0) ++kp.frames;
        kp.sum.ia += p.ia, kp.sum.vs += p.vs, kp.sum.cPrims += p.cPrims, kp.sum.ps += p.ps, kp.sum.cs += p.cs;
        kp.sum.samples += p.samples;
        if (kp.occ.size() <= n) {
          kp.occ.resize(n + 1);
          kp.occN.resize(n + 1, 0);
        }
        PassStats& o = kp.occ[n];
        o.pass = p.pass;
        o.ia += p.ia, o.vs += p.vs, o.cPrims += p.cPrims, o.ps += p.ps, o.cs += p.cs, o.samples += p.samples;
        ++kp.occN[n];
      }
    }
    for (auto& kv : local) {
      KindStat& k = r.kinds[kv.first];
      if (k.perFrame.size() <= static_cast<size_t>(fi)) k.perFrame.resize(fi + 1, 0.0);
      k.perFrame[fi] += kv.second;
    }
    if (firstTop >= 0 && lastTopEnd >= firstTop) {
      r.span.push_back((lastTopEnd - firstTop) * ms);
      r.gaps.push_back(gap * ms);
    }
    if (xrB >= 0 && xrE >= xrB) {
      r.xr.push_back((xrE - xrB) * ms);
      if (lastTopEnd >= 0 && xrB >= lastTopEnd) r.toXr.push_back((xrB - lastTopEnd) * ms);
    }
    edges.push_back({d.serial, d.freq, firstTop, xrE >= 0 ? xrE : lastTopEnd});
  }
  for (auto& kv : r.kinds) kv.second.perFrame.resize(r.frames, 0.0);
  for (size_t i = 1; i < edges.size(); ++i) {
    const Edge& a = edges[i - 1];
    const Edge& b = edges[i];
    if (b.serial != a.serial + 1 || b.freq != a.freq || a.first < 0 || b.first <= a.first) continue;
    const double ms = 1000.0 / static_cast<double>(a.freq);
    r.period.push_back((b.first - a.first) * ms);
    if (a.end >= 0 && b.first >= a.end) r.afterXr.push_back((b.first - a.end) * ms);
  }
  return r;
}

double Mean(const std::vector<double>& v) {
  if (v.empty()) return 0;
  double s = 0;
  for (double x : v) s += x;
  return s / v.size();
}
double P95(std::vector<double> v) {
  if (v.empty()) return 0;
  std::sort(v.begin(), v.end());
  return v[std::min(v.size() - 1, static_cast<size_t>(v.size() * 0.95))];
}

// ---------------------------------------------------------------------------
// Suite phase
// ---------------------------------------------------------------------------

void ResetState() {
  {
    std::lock_guard<std::mutex> lock(g_doneMutex);
    g_done.clear();
  }
  g_passCounts.clear();
  g_xrCounts = {};
  for (auto& o : g_outside) o = 0;
  g_s = {};
  g_rOverheadTicks = 0;
  g_rSerial = 0;
  g_rOpen = -1;
}

// Device, context and slots; queries now unless the device is single-threaded.
bool Prepare(ID3D11Device* dev, ID3D11DeviceContext* ctx, std::atomic<uint64_t>* frameCounter,
             bool singleThreaded) {
  for (int i = 0; i < kOpCount; ++i) {
    g_slot[i] = DcsQv_ContextSlot(kSlotNames[i]);
    if (g_slot[i] < 0) {
      Log("  gpu pass timing: unknown context slot %s", kSlotNames[i]);
      return false;
    }
  }
  g_dev = dev;
  g_ctx = ctx;
  g_frameCounter = frameCounter;
  g_createOnRenderThread = singleThreaded;
  ResetState();
  if (!singleThreaded && !CreateQueries()) {
    Log("  gpu pass timing: CreateQuery failed");
    return false;
  }
  return true;
}

// Payload stop: hooks out (from the stopping thread; the render thread may be
// inside a pass, the forwarders stay valid since modules are never unloaded).
void Shutdown() {
  g_want = 0;
  ptiming::BoundaryFn b = &OnBoundary;
  ptiming::g_boundary.compare_exchange_strong(b, nullptr);
  WithdrawOps();
  UninstallXr();
  g_active = false;
}

// "Module.dll+0x1234" for an address inside a loaded image (false: none).
bool ModRva(const void* p, char* out, size_t cap) {
  HMODULE m = nullptr;
  if (reinterpret_cast<uintptr_t>(p) < 0x10000 ||
      !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                          static_cast<LPCWSTR>(p), &m) ||
      !m)
    return false;
  wchar_t path[MAX_PATH] = {};
  GetModuleFileNameW(m, path, MAX_PATH);
  const wchar_t* base = wcsrchr(path, L'\\');
  base = base ? base + 1 : path;
  snprintf(out, cap, "%ls+0x%llx", base,
           static_cast<unsigned long long>(static_cast<const uint8_t*>(p) - reinterpret_cast<const uint8_t*>(m)));
  return true;
}

bool Readable(const void* p) {
  MEMORY_BASIC_INFORMATION mbi{};
  return reinterpret_cast<uintptr_t>(p) >= 0x10000 && VirtualQuery(p, &mbi, sizeof(mbi)) && mbi.State == MEM_COMMIT &&
         !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD));
}

// A pass object's identity (suite thread, SEH): full RTTI name, then up to 6
// pointers into loaded images within its first 0x200 bytes (its vtable, code
// pointers, callbacks), and the vtables of heap objects it points to
// (std::function targets, callback objects), as +offset=module+RVA.
void DescribeRaw(void* pass, char* out, size_t cap) {
  ptiming::RttiRaw(pass, out, cap);
  size_t len = strlen(out);
  // The pass name copied inline by the pass constructor (length at +8, at most
  // 32 chars at +0x10; GraphicsCore 0xa9310 for SimplePassData [V], R21):
  // the only thing that tells the addSimpleRenderingPass calls apart.
  {
    const uint8_t* b = static_cast<const uint8_t*>(pass);
    const uint64_t n = *reinterpret_cast<const uint64_t*>(b + 8);
    bool printable = n > 0 && n <= 32;
    for (uint64_t i = 0; printable && i < n; ++i) printable = b[0x10 + i] >= 0x20 && b[0x10 + i] <= 0x7e;
    if (printable && b[0x10 + n] == 0 && len + n + 12 < cap)
      len += snprintf(out + len, cap - len, " name \"%.*s\"", static_cast<int>(n), reinterpret_cast<const char*>(b + 0x10));
  }
  const void* const* q = static_cast<const void* const*>(pass);
  int hits = 0;
  char m[200];
  for (int i = 0; i < 64 && hits < 6 && len + 8 < cap; ++i) {
    const void* v = q[i];
    if (ModRva(v, m, sizeof(m))) {
      len += snprintf(out + len, cap - len, " +0x%x=%s", i * 8, m);
      ++hits;
      continue;
    }
    if (!Readable(v)) continue;
    const void* inner = *static_cast<const void* const*>(v);
    if (ModRva(inner, m, sizeof(m))) {
      len += snprintf(out + len, cap - len, " +0x%x->%s", i * 8, m);
      ++hits;
    }
    if (len >= cap) len = cap - 1;
  }
}

void DescribeGuarded(void* pass, char* out, size_t cap) {
  __try {
    DescribeRaw(pass, out, cap);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    strncpy_s(out, cap, "(pass object not readable)", _TRUNCATE);
  }
}

// CascadeShadowPassData: the cascade index at pass+0x60 (data+0, R17 1), -1 if unreadable.
int CascadeOfGuarded(void* pass) {
  __try {
    const int v = *reinterpret_cast<const int*>(static_cast<const uint8_t*>(pass) + 0x60);
    return v >= 0 && v < 8 ? v : -1;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return -1;
  }
}

// GpuPassStats report: per kind (sorted like the timing rows) and per call.
void ReportPipe(const Result& r, const std::vector<std::string>& order) {
  if (!r.statFrames) {
    if (g_statsOn.load() || g_s.statsLate)
      Log("  pipeline statistics: no frame with complete statistics (%llu frames late, %llu pairs over the limit)",
          static_cast<unsigned long long>(g_s.statsLate), static_cast<unsigned long long>(g_s.statsFull));
    return;
  }
  const double sf = r.statFrames, M = 1e6;
  Log("  pipeline statistics: %d frames (%llu late, %llu passes over the %d-pair limit); outermost passes, millions "
      "per frame (samples = depth/stencil-passing samples, all bound targets):",
      r.statFrames, static_cast<unsigned long long>(g_s.statsLate), static_cast<unsigned long long>(g_s.statsFull),
      kMaxStats);
  Log("    %-44s %9s %9s %9s %9s %9s %9s", "pass kind", "IA prims", "VS inv", "raster", "PS inv", "samples", "CS inv");
  for (const std::string& nm : order) {
    auto it = r.pipe.find(nm);
    if (it == r.pipe.end()) continue;
    const PassStats& t = it->second.sum;
    Log("    %-44.44s %9.3f %9.3f %9.3f %9.3f %9.3f %9.3f", nm.c_str(), t.ia / sf / M, t.vs / sf / M, t.cPrims / sf / M,
        t.ps / sf / M, t.samples / sf / M, t.cs / sf / M);
  }
  Log("  pipeline statistics per call (millions per call; kinds with several calls and >= 0.1 M PS or samples):");
  for (const std::string& nm : order) {
    auto it = r.pipe.find(nm);
    if (it == r.pipe.end() || it->second.occ.size() < 2) continue;
    const KindPipe& kp = it->second;
    const bool shadow = nm.find("(shadow)") != std::string::npos;
    for (size_t n = 0; n < kp.occ.size() && n < 32; ++n) {
      const PassStats& o = kp.occ[n];
      const double c = static_cast<double>(std::max<uint64_t>(1, kp.occN[n]));
      if ((o.ps + o.samples) / c < 0.1 * M) continue;
      char extra[160] = "";
      if (shadow) {
        const int casc = CascadeOfGuarded(o.pass);
        double area = 0;
        if (casc >= 0 && casc < shrec::kSlots && shrec::g_casc[casc].learn.target)
          area = static_cast<double>(shrec::g_casc[casc].learn.vp[0].Width) * shrec::g_casc[casc].learn.vp[0].Height;
        if (area > 0)
          snprintf(extra, sizeof(extra), "  cascade %d, viewport %.0f x %.0f: %.2f depth writes per texel", casc,
                   shrec::g_casc[casc].learn.vp[0].Width, shrec::g_casc[casc].learn.vp[0].Height, o.samples / c / area);
        else
          snprintf(extra, sizeof(extra), "  cascade %d", casc);
      }
      Log("    %-30.30s #%-2zu IA %8.3f VS %8.3f raster %8.3f PS %8.3f samples %8.3f%s", nm.c_str(), n, o.ia / c / M,
          o.vs / c / M, o.cPrims / c / M, o.ps / c / M, o.samples / c / M, extra);
    }
  }
}

void Report(uint64_t quadFrames, const std::map<std::string, ptiming::Stat>& cpu) {
  std::vector<Done> done;
  {
    std::lock_guard<std::mutex> lock(g_doneMutex);
    done.swap(g_done);
  }
  Result r = Analyze(done, [](void* p) -> std::string { return ptiming::RttiName(p); });
  const double opened = static_cast<double>(std::max<uint64_t>(1, g_s.framesOpened));
  Log("  gpu passes: %d frames read of %llu measured (disjoint %llu, truncated %llu, dropped unread %llu, missing "
      "%llu), unmatched pass events %llu, passes on other threads %llu",
      r.frames, static_cast<unsigned long long>(g_s.framesOpened), static_cast<unsigned long long>(g_s.disjoint),
      static_cast<unsigned long long>(g_s.truncated), static_cast<unsigned long long>(g_s.dropped),
      static_cast<unsigned long long>(g_s.missing), static_cast<unsigned long long>(r.unmatched),
      static_cast<unsigned long long>(g_s.offThread));
  Log("  gpu passes: render-thread cost of the queries and boundary hooks %.3f ms/frame; d3d11 context-table "
      "rewrites taken back %.1f/frame at boundaries, %.1f/frame after a call; xrEndFrame calls %.2f/frame (%llu "
      "inside a pass)",
      g_rOverheadTicks * g_qpcToUs / 1000.0 / opened, g_s.rehooks / opened, g_s.rearms / opened, g_s.xrCalls / opened,
      static_cast<unsigned long long>(g_s.xrNested));
  if (!r.frames) return;
  Log("  gpu frame: period %.2f ms (p95 %.2f), passes span %.2f ms (p95 %.2f), of which between top-level passes "
      "%.3f ms (p95 %.3f, GPU idle or work issued outside passes)",
      Mean(r.period), P95(r.period), Mean(r.span), P95(r.span), Mean(r.gaps), P95(r.gaps));
  if (!r.xr.empty())
    Log("  gpu frame: last pass end -> xrEndFrame %.3f ms, xrEndFrame wrapper (QVFR + runtime) %.3f ms (p95 %.3f), "
        "xrEndFrame end -> next frame's first pass %.3f ms (p95 %.3f)",
        Mean(r.toXr), Mean(r.xr), P95(r.xr), Mean(r.afterXr), P95(r.afterXr));
  else
    Log("  gpu frame: last pass end -> next frame's first pass %.3f ms (p95 %.3f) (xrEndFrame not timed)",
        Mean(r.afterXr), P95(r.afterXr));
  const double f = static_cast<double>(r.frames);
  const double qf = static_cast<double>(std::max<uint64_t>(1, quadFrames));
  struct Row {
    std::string name;
    const KindStat* k;
    double mean;
  };
  std::vector<Row> rows;
  double total = 0;
  for (auto& kv : r.kinds) {
    rows.push_back({kv.first, &kv.second, Mean(kv.second.perFrame)});
    total += rows.back().mean;
  }
  std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.mean > b.mean; });
  Log("  gpu passes: %.2f ms/frame exclusive GPU time in passes, %zu kinds (CPU: pass_timing, render thread, "
      "measured with these hooks on)",
      total, rows.size());
  Log("    %-44s %8s %7s %8s %7s %8s %8s", "pass kind", "GPU ms", "p95", "incl", "calls", "ms/call", "CPU ms");
  for (size_t i = 0; i < rows.size() && i < 40; ++i) {
    const Row& w = rows[i];
    auto c = cpu.find(w.name);
    const double cpuMs = c != cpu.end() ? c->second.exclUs / 1000.0 / qf : 0.0;
    Log("    %-44.44s %8.3f %7.3f %8.3f %7.1f %8.4f %8.3f", w.name.c_str(), w.mean, P95(w.k->perFrame),
        w.k->inclMs / f, w.k->calls / f, w.k->calls ? w.mean * f / w.k->calls : 0.0, cpuMs);
  }
  Log("  gpu passes by call order within the frame (exclusive GPU ms per frame; kinds with several calls, >= 0.05 ms):");
  for (const Row& w : rows) {
    if (w.k->calls / f < 1.5 || w.mean < 0.05) continue;
    std::string line;
    char buf[48];
    for (size_t n = 0; n < w.k->occMs.size() && n < 24; ++n) {
      snprintf(buf, sizeof(buf), " #%zu %.3f%s", n, w.k->occMs[n] / f,
               w.k->occN[n] * 10 < static_cast<uint64_t>(r.frames) * 9 ? "*" : "");
      line += buf;
    }
    Log("    %-36.36s%s", w.name.c_str(), line.c_str());
  }
  Log("    (* = that call happened in fewer than 90%% of the frames)");
  // Calls of kinds whose short name several unrelated passes share.
  for (const Row& w : rows) {
    if (w.name != "PassData" && w.name != "SimplePassData") continue;
    Log("  %s calls >= 0.02 ms: full RTTI name, then image pointers in the pass object (+offset=module+RVA; "
        "+offset->: the vtable of an object it points to):",
        w.name.c_str());
    for (size_t n = 0; n < w.k->occMs.size() && n < w.k->occPass.size(); ++n) {
      const double msn = w.k->occMs[n] / f;
      if (msn < 0.02 || !w.k->occPass[n]) continue;
      char desc[1024] = {};
      DescribeGuarded(w.k->occPass[n], desc, sizeof(desc));
      Log("    #%-2zu %.3f ms: %s", n, msn, desc);
    }
  }
  {
    std::vector<std::string> order;
    for (const Row& w : rows) order.push_back(w.name);
    ReportPipe(r, order);
  }
  // Context ops by pass kind (innermost pass), per measured frame.
  std::map<std::string, Counts> byName;
  for (auto& kv : g_passCounts) {
    Counts& c = byName[ptiming::RttiName(kv.first)];
    for (int i = 0; i < kOpCount; ++i) c.n[i] += kv.second.n[i];
  }
  std::string hdr;
  char buf[32];
  for (int i = 0; i < kOpCount; ++i) {
    snprintf(buf, sizeof(buf), " %8s", kOpShort[i]);
    hdr += buf;
  }
  Log("  context ops per frame by pass kind (lower bounds while d3d11 had a hook replaced):");
  Log("    %-36s%s", "pass kind", hdr.c_str());
  auto row = [&](const char* nm, const Counts& c) {
    uint64_t sum = 0;
    for (int i = 0; i < kOpCount; ++i) sum += c.n[i];
    if (!sum) return;
    std::string line;
    for (int i = 0; i < kOpCount; ++i) {
      snprintf(buf, sizeof(buf), " %8.1f", c.n[i] / opened);
      line += buf;
    }
    Log("    %-36.36s%s", nm, line.c_str());
  };
  for (auto& kv : byName) row(kv.first.c_str(), kv.second);
  row("(in xrEndFrame)", g_xrCounts);
  Counts out;
  for (int i = 0; i < kOpCount; ++i) out.n[i] = g_outside[i].load();
  row("(outside passes)", out);
}

// Session: device, queries, xrEndFrame hook and the boundary callback; the
// render thread activates at its next top-level pass. light = timestamps only
// (no context-op hooks). True when the render thread is measuring.
ID3D11Device* g_sessDev = nullptr;
ID3D11DeviceContext* g_sessCtx = nullptr;

bool Begin(std::atomic<uint64_t>& frameCounter, bool light) {
  if (!ptiming::g_orig) {
    Log("  pass timing not installed");
    return false;
  }
  if (g_sessDev) return g_active.load();
  ID3D11Device* dev = shadowinst::g_device;
  if (dev) {
    dev->AddRef();
  } else if (sfilt::Ctx* c = sfilt::g_ctx.load()) {
    c->GetDevice(&dev);
  }
  if (!dev) {
    Log("  gpu pass timing: DCS's device not known yet (it comes from [Model] ShadowInstancing=1 or "
        "[D3D] SplitFilter=1)");
    return false;
  }
  ID3D11DeviceContext* ctx = nullptr;
  dev->GetImmediateContext(&ctx);
  if (sfilt::Ctx* c = sfilt::g_ctx.load())
    if (static_cast<ID3D11DeviceContext*>(c) != ctx)
      Log("  gpu pass timing: note: the split filter's context %p is not the device's immediate context %p",
          static_cast<void*>(c), static_cast<void*>(ctx));
  g_statsOn = g_statsWanted.load() && !light;
  if (!ctx || !Prepare(dev, ctx, &frameCounter, shadowinst::g_deviceSingleThreaded)) {
    if (ctx) ctx->Release();
    dev->Release();
    return false;
  }
  g_sessDev = dev;
  g_sessCtx = ctx;
  g_light = light;
  const bool xr = InstallXr();
  if (xr) Sleep(100);  // the xrEndFrame thread is known before activation
  ptiming::g_boundary = &OnBoundary;
  g_want = 1;
  for (int i = 0; i < 200 && !g_active.load() && g_want.load(); ++i) Sleep(10);
  return g_active.load();
}

// Frames read back so far (and forgets them).
std::vector<Done> TakeDone() {
  std::vector<Done> done;
  std::lock_guard<std::mutex> lock(g_doneMutex);
  done.swap(g_done);
  return done;
}

// Ends the session: the render thread deactivates at its next top-level pass
// (frames in flight get a last non-blocking read), hooks out, queries freed.
void End() {
  if (!g_sessDev) return;
  g_want = 0;
  for (int i = 0; i < 200 && g_active.load(); ++i) Sleep(10);
  ptiming::BoundaryFn b = &OnBoundary;
  ptiming::g_boundary.compare_exchange_strong(b, nullptr);
  UninstallXr();
  Sleep(50);  // threads still inside a hook
  if (g_active.load()) {
    // No pass started since: nothing renders, so the stopping thread can clean up.
    Log("  gpu pass timing: render thread did not stop the measurement; cleaned up from the suite thread");
    WithdrawOps();
    ReleaseQueries();
    g_active = false;
  } else if (g_queriesReady.load()) {
    ReleaseQueries();  // never activated
  }
  if (g_s.createFailed) Log("  gpu pass timing: could not start on the render thread (queries or context hooks)");
  g_light = false;
  g_statsOn = false;
  g_sessCtx->Release();
  g_sessDev->Release();
  g_sessCtx = nullptr;
  g_sessDev = nullptr;
  g_ctx = nullptr;
  g_dev = nullptr;
}

// Render-thread cost of the timestamps and boundary hooks per opened frame (ms).
double OverheadMsPerFrame() {
  return g_rOverheadTicks * g_qpcToUs / 1000.0 / static_cast<double>(std::max<uint64_t>(1, g_s.framesOpened));
}

// Per read frame (offline tested): GPU ms from the first top-level pass start
// to the last top-level pass end (span), the GPU time inside top-level passes
// (busy = span minus the gaps between them) and the xrEndFrame wrapper's GPU
// interval (-1 when not stamped). False when the frame has no complete pass.
bool FrameGpu(const Done& d, double* spanMs, double* busyMs, double* xrMs) {
  if (!d.freq || d.ticks.size() != d.ev.size()) return false;
  const double ms = 1000.0 / static_cast<double>(d.freq);
  int depth = 0;
  int64_t first = -1, lastEnd = -1, xrB = -1, xrE = -1, topStart = -1;
  uint64_t busy = 0;
  for (size_t i = 0; i < d.ev.size(); ++i) {
    const int64_t t = static_cast<int64_t>(d.ticks[i]);
    switch (d.ev[i].type) {
      case kBegin:
        if (depth == 0) {
          topStart = t;
          if (first < 0) first = t;
        }
        ++depth;
        break;
      case kEnd:
        if (depth > 0 && --depth == 0 && topStart >= 0 && t >= topStart) {
          busy += static_cast<uint64_t>(t - topStart);
          lastEnd = t;
        }
        break;
      case kXrBegin:
        xrB = t;
        break;
      case kXrEnd:
        xrE = t;
        break;
    }
  }
  if (first < 0 || lastEnd < first) return false;
  if (spanMs) *spanMs = (lastEnd - first) * ms;
  if (busyMs) *busyMs = busy * ms;
  if (xrMs) *xrMs = (xrB >= 0 && xrE >= xrB) ? (xrE - xrB) * ms : -1.0;
  return true;
}

// light: timestamps only (no context-op hooks, no pipeline statistics); for callers whose own table arms must
// not meet the op hooks (periph_inst_filter.h's timing phase).
void Measure(int ms, std::atomic<uint64_t>& frameCounter, bool light = false) {
  if (!Begin(frameCounter, light) && !g_sessDev) return;
  // CPU per pass over the same window, for comparison.
  ptiming::Reset();
  ptiming::g_recording = true;
  const uint64_t f0 = frameCounter.load();
  if (g_active.load()) Sleep(ms);
  ptiming::g_recording = false;
  const uint64_t frames = frameCounter.load() - f0;
  End();
  std::map<std::string, ptiming::Stat> cpu = ptiming::TakeByName(nullptr, nullptr);
  if (g_s.framesOpened)
    Report(frames, cpu);
  else
    Log("  gpu pass timing: no frame measured (no top-level pass on the render thread)");
}
}  // namespace gpt
