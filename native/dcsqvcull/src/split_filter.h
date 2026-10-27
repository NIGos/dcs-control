// Split-path redundant D3D11 state filter (R15 F5). [D3D] SplitFilter, default off.
//
// The rejected filter (d3dstate.h mode 1) hooked every call of the context
// vtable and lost 3-5 %: the hook cost more than the skipped calls. Here the
// skip happens only where dx11backend issues the redundant calls, and the
// vtable hooks see only everyone else:
//
//  - Patched sites. Each `call [rax+off]` below (6 bytes, FF 90 off32) becomes
//    `nop; call rel32` into a near stub that jumps to the op's filter. Same
//    registers and stack as the original call, return address site+6 in both
//    layouts (a thread still inside a filter when the site is restored
//    returns to an instruction boundary). The six FX setter-table entries
//    are data and are swapped through the loader's slot registry.
//  - The filter compares the arguments with a shadow of DCS's immediate
//    context. Equal: return. Different: store and call d3d11's own entry
//    (the original taken from the context table), never through the table.
//  - Vtable hooks on the same 11 setters record what foreign callers bind
//    (QVFR, the OpenXR runtime in xrEndFrame, our batching code, dx11backend
//    paths that are not patched) and forward. ClearState,
//    ExecuteCommandList, SwapDeviceContextState and VS/PSSetConstantBuffers1
//    forget the shadow. Output binds (OMSetRenderTargets*, UAVs, SO) need no
//    hook: they only unbind resources from input slots, and none of the
//    filtered bindings is a resource view; a constant buffer cannot also be
//    a render target, UAV or stream-output buffer (D3D11_BIND_CONSTANT_BUFFER
//    excludes every other bind flag) [I, D3D11 spec].
//
// Sites [V dx11backend.dll of 2026-09-19, 0x6aae1d9d]:
//   ApplyPassBlock 0x67f90 (FX pass Apply, context effect+0x158):
//     0x67ff1 OMSetBlendState, 0x68022 OMSetDepthStencilState, 0x6804c RSSetState.
//   DX11Renderer::draw 0x14870 (context renderer+0x30): IASetPrimitiveTopology
//     in the nine cases of its PRIMTYPE switch (table 0x14cd4) at 0x149f2,
//     0x14a13, 0x14a38, 0x14a6a, 0x14a8f, 0x14abd, 0x14ade, 0x14b0d, 0x14b38.
//   DX11Renderer::setVertexBuffers 0x1ad10 (called by draw): IASetInputLayout
//     at 0x1ad61 (null layout) and 0x1ada4.
//   ApplyShaderBlock 0x68470 calls SetShader, SetConstantBuffers and
//     SetSamplers through the block's per-stage call table ([block+8], entries
//     +0, +8, +0x10; `call rax` at 0x688e0, 0x68685, 0x68703). The tables are
//     static .data arrays of five member-function pointers per stage (vcall
//     thunks `mov rax,[rcx]; jmp [rax+off]`): PS 0x11d178, VS 0x11d1a0, GS
//     0x11d1c8, HS 0x11d1f0, DS 0x11d218, CS 0x11d240; ApplyShaderBlock reads
//     the SRV entry (+0x18) of VS and PS by RIP at 0x684e5 and 0x6851d, the
//     shader blocks get the table pointer from 0x4f550 / 0x51a15 [V xref].
//     The VS and PS entries +0, +8, +0x10 are swapped for the filters.
// Topology calls elsewhere (drawIndirect, drawMultIndirect, drawWithOffset,
// 0x33850, 0x41770) and every other caller reach the vtable hooks.
//
// d3d11 rewrites the immediate context's table (it lives in the context
// object) [V d3d11.dll on this PC, offline]: SetMultithreadProtected,
// SwapDeviceContextState and the first ClearState after one of them rewrite
// slots 7..147 (every hooked slot except SwapDeviceContextState 131); Flush
// and ExecuteCommandList toggle 24 draw/copy/clear slots (none hooked). While
// a hook is gone the calls through it are not seen. So each filter call
// checks that its own slot and ClearState's still hold our hooks (a rewrite
// replaces both together); if not, the shadow is forgotten, the hooks are put
// back at once by that same thread and the call is forwarded.
//
// Only DCS's immediate context is filtered (renderer+0x30, taken from the
// first topology or input-layout site call; GetType must say immediate).
// Other contexts, deferred ones included, are forwarded through their table.
// Skipping a redundant Set* leaves the binding and its reference unchanged;
// a performed Set* AddRefs the new object and Releases the old one, which is
// the same object, so no reference count differs once the call returns.
// Concurrent use of the immediate context from two threads is as racy with
// the filter as without it (d3d11 only orders the calls).
//
// [D3D] SplitFilterOps picks the filtered ops (bit 1 << Op: VS 1, PS 2, VS CB
// 4, PS CB 8, VS samp 16, PS samp 32, layout 64, topology 128, blend 256,
// depth 512, raster 1024); default 0x4ff, without blend and depth (see
// kDefaultOps). An op that is off has no site patch and no hook.
// [Suite] SplitFilterVerify: before each skip the binding is read back with
// the Get* call and compared; a mismatch forwards that call, latches the
// filter off for the session and logs a sample. [Suite] BenchSplitFilter:
// bench mode 27, OFF = stock (no patch, no hook), ON = filter.
// Off (key, kill switch, bench OFF) restores every site, table entry and
// vtable slot: byte-identical stock.
// Fail-safe: any byte, hash, export or thunk mismatch, or the D3D meter
// already hooking the context ([D3D] Meter=1), turns it off with a log line.
// Not covered by reloc.h (fixed RVAs, exact FNV): a DCS update turns it off.
// Included once from main.cpp inside its anonymous namespace, after
// d3dstate.h (d3ds::WriteTablePointer, d3ds::g_vtblCount), pacer.h
// (AllocNear) and alloc_slab.h (ReadBytes).
#pragma once

#include <intrin.h>
#include <tlhelp32.h>

namespace sfilt {

using Ctx = ID3D11DeviceContext1;

// ---------------------------------------------------------------------------
// Ops, shadow
// ---------------------------------------------------------------------------

enum Op { kVS, kPS, kVSCB, kPSCB, kVSSamp, kPSSamp, kIL, kTopo, kBlend, kDepth, kRS, kOpCount };
const char* const kOpNames[kOpCount] = {"VS",     "PS",   "VS CB", "PS CB", "VS samp", "PS samp",
                                        "layout", "topo", "blend", "depth", "raster"};
// Offline micro-benchmark on this PC (median ns per redundant d3d11 call,
// NOTES_candidates F5 row); samplers were not measured: shader value used [I].
const double kNsPerSkip[kOpCount] = {3.7, 3.7, 6.3, 7.1, 3.7, 3.7, 3.7, 3.6, 1.8, 1.1, 4.6};

// Hooked context slots: the 11 setters (same order as Op), then the invalidators.
enum Hook { kHClearState = kOpCount, kHExecuteCommandList, kHSwap, kHVSSetCB1, kHPSSetCB1, kHookCount };
const char* const kSlotNames[kHookCount] = {
    "VSSetShader",          "PSSetShader",           "VSSetConstantBuffers",  "PSSetConstantBuffers",
    "VSSetSamplers",        "PSSetSamplers",         "IASetInputLayout",      "IASetPrimitiveTopology",
    "OMSetBlendState",      "OMSetDepthStencilState", "RSSetState",           "ClearState",
    "ExecuteCommandList",   "SwapDeviceContextState", "VSSetConstantBuffers1", "PSSetConstantBuffers1"};

void* const kUnknown = reinterpret_cast<void*>(1);  // never equal to a real binding
constexpr UINT kCb = D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT;  // 14
constexpr UINT kSamp = D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT;           // 16

struct Shadow {
  void* vs;
  void* ps;
  void* il;
  void* vsCb[kCb];
  void* psCb[kCb];
  void* vsSamp[kSamp];
  void* psSamp[kSamp];
  UINT topo;  // ~0u = unknown
  void* blend;
  float blendFactor[4];
  UINT sampleMask;
  void* depth;
  UINT stencilRef;
  void* rs;

  template <size_t N>
  static void Forget(void* (&a)[N]) {
    for (auto& p : a) p = kUnknown;
  }
  void ForgetAll() {
    vs = ps = il = blend = depth = rs = kUnknown;
    Forget(vsCb);
    Forget(psCb);
    Forget(vsSamp);
    Forget(psSamp);
    topo = ~0u;
  }
};

// The whole range [start, start+n) is bound to exactly these values.
template <size_t N>
bool SameRange(void* const (&sh)[N], UINT start, UINT n, const void* const* v) {
  if (!v || n == 0 || start >= N || n > N - start) return false;
  for (UINT i = 0; i < n; ++i)
    if (sh[start + i] != v[i]) return false;
  return true;
}

// Records a range set; anything d3d11 may not take as given (null array,
// out of range) makes the whole stage's shadow unknown.
template <size_t N>
void StoreRange(void* (&sh)[N], UINT start, UINT n, const void* const* v) {
  if (!v || start >= N || n > N - start) {
    Shadow::Forget(sh);
    return;
  }
  for (UINT i = 0; i < n; ++i) sh[start + i] = const_cast<void*>(v[i]);
}

inline void Factor(const FLOAT* f, float (&out)[4]) {
  if (f) {
    memcpy(out, f, sizeof(out));
  } else {
    out[0] = out[1] = out[2] = out[3] = 1.0f;  // d3d11: null = {1, 1, 1, 1}
  }
}

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

Shadow g_sh;
std::atomic<Ctx*> g_live{nullptr};   // DCS's context while filtering, else null
std::atomic<Ctx*> g_ctx{nullptr};    // DCS's immediate context once captured
// Called after every ExecuteCommandList on an immediate context (d3d11 toggles the table's draw / copy / update
// entries there; fixation_depth.h repairs its in-pass hooks at once). Set by fixation_depth.h's Install.
std::atomic<void (*)(void* ctx)> g_afterEcl{nullptr};
// R139: a second listener (vrs_probe.h binds its shading-rate state again); nullptr unless its suite phase runs.
std::atomic<void (*)(void* ctx)> g_afterEclProbe{nullptr};
inline void NotifyEcl(void* ctx) {
  if (auto f = g_afterEcl.load(std::memory_order_acquire)) f(ctx);
  if (auto f = g_afterEclProbe.load(std::memory_order_acquire)) f(ctx);
}
// Called after d3d11 rewrote the context table (slots 7..147: CSSetShader too) and our hooks went back in, on the
// thread that found it (the filtering thread). Set by chain_fast.h (its CSSetShader hook goes back in as well).
std::atomic<void (*)()> g_afterRewrite{nullptr};
void** g_table = nullptr;            // its vtable (inside the context object)
int g_slot[kHookCount] = {};
void* volatile g_orig[kHookCount] = {};  // d3d11's entries, refreshed with every re-hook
std::atomic<bool> g_verify{false};
std::atomic<bool> g_count{false};
std::atomic<bool> g_disabled{false};  // latched off for the session (verify mismatch)
SRWLOCK g_patchLock = SRWLOCK_INIT;

struct Counters {
  uint64_t site[32];             // per patched site (code sites, then table entries)
  uint64_t calls[kOpCount];      // patched-site calls on DCS's context while live
  uint64_t skipped[kOpCount];
  uint64_t foreign[kOpCount];    // vtable hook calls on DCS's context (bypassing the sites)
  uint64_t mismatch[kOpCount];
  uint64_t resets;               // foreign ClearState / ExecuteCommandList / Swap / *SetConstantBuffers1
  uint64_t lost;                 // patched-site calls that found d3d11's table rewritten
  uint64_t other;                // patched-site calls for another context or while not live
};
Counters g_c;
std::atomic<uint64_t> g_rewrites{0}, g_switches{0};
// R48: re-hooks refused because another hook of this payload held one of our slots (see RepatchLocked).
std::atomic<uint64_t> g_oursOnTop{0};
std::mutex g_sampleMutex;
std::vector<std::string> g_samples;

void ResetCounters() {
  memset(&g_c, 0, sizeof(g_c));
  g_rewrites = 0;
  g_switches = 0;
  g_oursOnTop = 0;
}

// ---------------------------------------------------------------------------
// Patched locations
// ---------------------------------------------------------------------------

struct Site {
  uint32_t rva;
  uint8_t op;
};
const Site kSites[] = {
    {0x67ff1, kBlend}, {0x68022, kDepth}, {0x6804c, kRS},                                          // ApplyPassBlock
    {0x149f2, kTopo},  {0x14a13, kTopo},  {0x14a38, kTopo}, {0x14a6a, kTopo}, {0x14a8f, kTopo},    // draw
    {0x14abd, kTopo},  {0x14ade, kTopo},  {0x14b0d, kTopo}, {0x14b38, kTopo},
    {0x1ad61, kIL},    {0x1ada4, kIL},                                                             // setVertexBuffers
};
struct TableEntry {
  uint32_t rva;
  uint8_t op;
};
constexpr uint32_t kPsTable = 0x11d178, kVsTable = 0x11d1a0;  // FX per-stage call tables (.data)
const TableEntry kTables[] = {
    {kVsTable, kVS}, {kVsTable + 8, kVSCB}, {kVsTable + 16, kVSSamp},  // +0 SetShader, +8 SetConstantBuffers,
    {kPsTable, kPS}, {kPsTable + 8, kPSCB}, {kPsTable + 16, kPSSamp},  // +0x10 SetSamplers
};
constexpr int kMaxSites = 24, kMaxTables = 8;

struct CodeRange {
  uint32_t begin, end;
  uint64_t hash;  // FNV-1a 64 of the file bytes (no base relocations inside)
};
const CodeRange kCode[] = {
    {0x67f90, 0x68234, 0xc787fe71c10919ffull},  // ApplyPassBlock (= fxapply::kCode[1])
    {0x68470, 0x68546, 0x449ee222221bf71bull},  // ApplyShaderBlock, without srvspan's two call sites
    {0x6854b, 0x688b8, 0xe2f5dc3ea951e52cull},
    {0x688bd, 0x68918, 0x181344a50069723dull},
    {0x14870, 0x14cd4, 0xe65642f938456babull},  // DX11Renderer::draw
    {0x14cd4, 0x14d2c, 0xa4376be1c02fd1c2ull},  // its PRIMTYPE jump table (22 RVAs)
    {0x1ad10, 0x1ae42, 0x8c739dbcd7a1104dull},  // DX11Renderer::setVertexBuffers
};
const char kDrawExport[] = "?draw@DX11Renderer@RenderAPI@@UEAAXHPEAUIShader@2@W4PRIMTYPE_ENUM@render@@HHHPEBD@Z";
const char kSvbExport[] =
    "?setVertexBuffers@DX11Renderer@RenderAPI@@IEAAXPEAVDX11Shader@2@AEAVDX11ShaderTechnique@2@H@Z";
constexpr uint32_t kDrawRva = 0x14870, kSvbRva = 0x1ad10;

// What is patched where (production: dx11backend; tests: a fake module).
uint8_t* g_base = nullptr;
const Site* g_sites = kSites;
int g_siteCount = static_cast<int>(sizeof(kSites) / sizeof(kSites[0]));
const TableEntry* g_tables = kTables;
int g_tableCount = static_cast<int>(sizeof(kTables) / sizeof(kTables[0]));
uint8_t* g_stub = nullptr;  // near page, one 16-byte stub per op
uint8_t g_savedSite[kMaxSites][6] = {};
void* g_tableOrig[kMaxTables] = {};
bool g_tableHooked[kMaxTables] = {};
// Filtered ops ([D3D] SplitFilterOps, bit = 1 << Op). Blend and depth are off
// by default: their redundant d3d11 call (1.8 / 1.1 ns) is cheaper than a
// filtered site call (2.5-3 ns, test/split_filter_test.h on this PC), so they
// keep DCS's own call and get neither site patch nor hook.
constexpr uint32_t kAllOps = (1u << kOpCount) - 1;
constexpr uint32_t kDefaultOps = kAllOps & ~((1u << kBlend) | (1u << kDepth));
std::atomic<uint32_t> g_opMask{kDefaultOps};
uint32_t g_active = 0;  // the mask the current patches and hooks were made with
inline bool On(int h) { return h >= kOpCount || ((g_active >> h) & 1); }
std::atomic<int> g_state{0};  // 0 not installed, 1 checked and ready to attach, -1 failed
std::atomic<bool> g_sitesPatched{false}, g_hooked{false}, g_attached{false};

// ---------------------------------------------------------------------------
// Forwarding helpers
// ---------------------------------------------------------------------------

template <class F>
inline F Orig(int h) {
  return reinterpret_cast<F>(g_orig[h]);
}
// Through c's own table (our hook when hooked, else whatever d3d11 has there).
template <class F>
inline F Table(Ctx* c, int h) {
  return reinterpret_cast<F>((*reinterpret_cast<void***>(c))[g_slot[h]]);
}

// ---------------------------------------------------------------------------
// Vtable hooks: foreign callers (record and forward) and invalidators
// ---------------------------------------------------------------------------

inline bool Mine(Ctx* c) { return c == g_ctx.load(std::memory_order_relaxed); }
inline void Foreign(int op) {
  if (g_count.load(std::memory_order_relaxed)) g_c.foreign[op]++;
}

void STDMETHODCALLTYPE H_OMSetBlendState(Ctx* c, ID3D11BlendState* b, const FLOAT f[4], UINT mask) {
  if (Mine(c)) {
    g_sh.blend = b;
    Factor(f, g_sh.blendFactor);
    g_sh.sampleMask = mask;
    Foreign(kBlend);
  }
  Orig<decltype(&H_OMSetBlendState)>(kBlend)(c, b, f, mask);
}
void STDMETHODCALLTYPE H_OMSetDepthStencilState(Ctx* c, ID3D11DepthStencilState* d, UINT ref) {
  if (Mine(c)) {
    g_sh.depth = d;
    g_sh.stencilRef = ref;
    Foreign(kDepth);
  }
  Orig<decltype(&H_OMSetDepthStencilState)>(kDepth)(c, d, ref);
}
void STDMETHODCALLTYPE H_RSSetState(Ctx* c, ID3D11RasterizerState* r) {
  if (Mine(c)) {
    g_sh.rs = r;
    Foreign(kRS);
  }
  Orig<decltype(&H_RSSetState)>(kRS)(c, r);
}
void STDMETHODCALLTYPE H_IASetPrimitiveTopology(Ctx* c, D3D11_PRIMITIVE_TOPOLOGY t) {
  if (Mine(c)) {
    g_sh.topo = t;
    Foreign(kTopo);
  }
  Orig<decltype(&H_IASetPrimitiveTopology)>(kTopo)(c, t);
}
void STDMETHODCALLTYPE H_IASetInputLayout(Ctx* c, ID3D11InputLayout* l) {
  if (Mine(c)) {
    g_sh.il = l;
    Foreign(kIL);
  }
  Orig<decltype(&H_IASetInputLayout)>(kIL)(c, l);
}
#define SH_SHADER(NAME, TYPE, OP, FIELD)                                                        \
  void STDMETHODCALLTYPE H_##NAME(Ctx* c, TYPE* s, ID3D11ClassInstance* const* ci, UINT nci) { \
    if (Mine(c)) {                                                                              \
      g_sh.FIELD = nci == 0 ? static_cast<void*>(s) : kUnknown;                                 \
      Foreign(OP);                                                                              \
    }                                                                                           \
    Orig<decltype(&H_##NAME)>(OP)(c, s, ci, nci);                                               \
  }
SH_SHADER(VSSetShader, ID3D11VertexShader, kVS, vs)
SH_SHADER(PSSetShader, ID3D11PixelShader, kPS, ps)
#define SH_RANGE(NAME, TYPE, OP, FIELD)                                       \
  void STDMETHODCALLTYPE H_##NAME(Ctx* c, UINT s, UINT n, TYPE* const* v) {  \
    if (Mine(c)) {                                                            \
      StoreRange(g_sh.FIELD, s, n, reinterpret_cast<const void* const*>(v)); \
      Foreign(OP);                                                            \
    }                                                                         \
    Orig<decltype(&H_##NAME)>(OP)(c, s, n, v);                                \
  }
SH_RANGE(VSSetConstantBuffers, ID3D11Buffer, kVSCB, vsCb)
SH_RANGE(PSSetConstantBuffers, ID3D11Buffer, kPSCB, psCb)
SH_RANGE(VSSetSamplers, ID3D11SamplerState, kVSSamp, vsSamp)
SH_RANGE(PSSetSamplers, ID3D11SamplerState, kPSSamp, psSamp)
#undef SH_SHADER
#undef SH_RANGE

inline void Reset(Ctx* c) {
  if (!Mine(c)) return;
  g_sh.ForgetAll();
  if (g_count.load(std::memory_order_relaxed)) g_c.resets++;
}
// The shadow is forgotten before and after: d3d11 may rewrite the table
// inside the call, and the state after it is not tracked.
void STDMETHODCALLTYPE H_ClearState(Ctx* c) {
  Reset(c);
  Orig<decltype(&H_ClearState)>(kHClearState)(c);
  if (Mine(c)) g_sh.ForgetAll();
}
void STDMETHODCALLTYPE H_ExecuteCommandList(Ctx* c, ID3D11CommandList* l, BOOL restore) {
  Reset(c);
  Orig<decltype(&H_ExecuteCommandList)>(kHExecuteCommandList)(c, l, restore);
  if (Mine(c)) g_sh.ForgetAll();
  NotifyEcl(c);
}
void STDMETHODCALLTYPE H_SwapDeviceContextState(Ctx* c, ID3DDeviceContextState* s, ID3DDeviceContextState** p) {
  Reset(c);
  Orig<decltype(&H_SwapDeviceContextState)>(kHSwap)(c, s, p);
  if (Mine(c)) g_sh.ForgetAll();
}
// Constant buffers with offsets: the pointer alone no longer says what is bound.
void STDMETHODCALLTYPE H_VSSetConstantBuffers1(Ctx* c, UINT s, UINT n, ID3D11Buffer* const* b, const UINT* f,
                                               const UINT* k) {
  if (Mine(c)) {
    Shadow::Forget(g_sh.vsCb);
    if (g_count.load(std::memory_order_relaxed)) g_c.resets++;
  }
  Orig<decltype(&H_VSSetConstantBuffers1)>(kHVSSetCB1)(c, s, n, b, f, k);
  if (Mine(c)) Shadow::Forget(g_sh.vsCb);
}
void STDMETHODCALLTYPE H_PSSetConstantBuffers1(Ctx* c, UINT s, UINT n, ID3D11Buffer* const* b, const UINT* f,
                                               const UINT* k) {
  if (Mine(c)) {
    Shadow::Forget(g_sh.psCb);
    if (g_count.load(std::memory_order_relaxed)) g_c.resets++;
  }
  Orig<decltype(&H_PSSetConstantBuffers1)>(kHPSSetCB1)(c, s, n, b, f, k);
  if (Mine(c)) Shadow::Forget(g_sh.psCb);
}

void* const kHooks[kHookCount] = {
    reinterpret_cast<void*>(&H_VSSetShader),           reinterpret_cast<void*>(&H_PSSetShader),
    reinterpret_cast<void*>(&H_VSSetConstantBuffers),  reinterpret_cast<void*>(&H_PSSetConstantBuffers),
    reinterpret_cast<void*>(&H_VSSetSamplers),         reinterpret_cast<void*>(&H_PSSetSamplers),
    reinterpret_cast<void*>(&H_IASetInputLayout),      reinterpret_cast<void*>(&H_IASetPrimitiveTopology),
    reinterpret_cast<void*>(&H_OMSetBlendState),       reinterpret_cast<void*>(&H_OMSetDepthStencilState),
    reinterpret_cast<void*>(&H_RSSetState),            reinterpret_cast<void*>(&H_ClearState),
    reinterpret_cast<void*>(&H_ExecuteCommandList),    reinterpret_cast<void*>(&H_SwapDeviceContextState),
    reinterpret_cast<void*>(&H_VSSetConstantBuffers1), reinterpret_cast<void*>(&H_PSSetConstantBuffers1)};

bool TryRepatchInline();

// The table still routes this op and ClearState to our hooks (d3d11 rewrites
// slots 7..147 together, which covers every other invalidator; Swap's slot
// 131 is never rewritten [M offline]; checking all six slots cost the whole
// gain: 0.0% vs +1.1% in the A/B, 2026-10-09). If not,
// the calls made since d3d11 rewrote it were not seen: forget, re-hook,
// forward. Re-hooking happens only here, on the filtering thread: a separate
// re-hook thread raced with the filter (a site call between its forget and
// its re-hook, then a foreign call through the rewritten slot, left a stale
// shadow that a later site call skipped against; seen once in the offline
// seeded test, 2026-10-09).
__declspec(noinline) void Lost() {
  g_sh.ForgetAll();
  if (g_count.load(std::memory_order_relaxed)) g_c.lost++;
  TryRepatchInline();
}
inline bool Hooked(Ctx* c, int op) {
  void** vt = *reinterpret_cast<void***>(c);
  if (vt[g_slot[op]] == kHooks[op] && vt[g_slot[kHClearState]] == kHooks[kHClearState]) return true;
  Lost();
  return false;
}

void Capture(Ctx* c);
// DCS's context is taken from the renderer sites (renderer+0x30), or from any
// site when neither of them is filtered (the FX context is the same one [I]).
inline bool CaptureAt(int op) {
  return op == kTopo || op == kIL || !(g_active & ((1u << kTopo) | (1u << kIL)));
}

// Counts the call at its site (counting mode only).
__declspec(noinline) void CountSite(int op, void* ret, int table) {
  if (table >= 0) {
    g_c.site[g_siteCount + table]++;
    return;
  }
  const uint8_t* r = static_cast<uint8_t*>(ret);
  for (int i = 0; i < g_siteCount; ++i)
    if (g_sites[i].op == op && g_base + g_sites[i].rva + 6 == r) {
      g_c.site[i]++;
      return;
    }
}

__declspec(noinline) void Mismatch(int op, const char* what, uintptr_t want, uintptr_t got) {
  g_c.mismatch[op]++;
  if (!g_disabled.exchange(true)) g_live = nullptr;  // latched; the worker detaches
  std::lock_guard<std::mutex> lock(g_sampleMutex);
  if (g_samples.size() < 8) {
    char line[200];
    snprintf(line, sizeof(line), "%s %s mismatch: shadow %p, d3d11 has %p (thread %lu)", kOpNames[op], what,
             reinterpret_cast<void*>(want), reinterpret_cast<void*>(got), GetCurrentThreadId());
    g_samples.push_back(line);
  }
}

// ---------------------------------------------------------------------------
// Verify (Get* read-back before a skip)
// ---------------------------------------------------------------------------

template <class T>
bool VerifyRange(Ctx* c, int op, UINT s, UINT n, T* const* v,
                 void(STDMETHODCALLTYPE ID3D11DeviceContext::*get)(UINT, UINT, T**)) {
  T* got[16] = {};
  (c->*get)(s, n, got);
  bool ok = true;
  for (UINT i = 0; i < n; ++i) {
    if (ok && got[i] != v[i]) {
      Mismatch(op, "slot", reinterpret_cast<uintptr_t>(v[i]), reinterpret_cast<uintptr_t>(got[i]));
      ok = false;
    }
    if (got[i]) got[i]->Release();
  }
  return ok;
}

template <class T>
bool VerifyObject(int op, T* want, T* got) {
  const bool ok = got == want;
  if (!ok) Mismatch(op, "object", reinterpret_cast<uintptr_t>(want), reinterpret_cast<uintptr_t>(got));
  if (got) got->Release();
  return ok;
}

bool VerifyShader(Ctx* c, int op, void* want) {
  UINT n = 0;
  if (op == kVS) {
    ID3D11VertexShader* s = nullptr;
    c->VSGetShader(&s, nullptr, &n);
    if (n) Mismatch(op, "class instances", 0, n);
    return VerifyObject(op, static_cast<ID3D11VertexShader*>(want), s) && !n;
  }
  ID3D11PixelShader* s = nullptr;
  c->PSGetShader(&s, nullptr, &n);
  if (n) Mismatch(op, "class instances", 0, n);
  return VerifyObject(op, static_cast<ID3D11PixelShader*>(want), s) && !n;
}

// ---------------------------------------------------------------------------
// Filters (reached from the patched sites and table entries)
// ---------------------------------------------------------------------------

#define SF_ENTER(OP, TABLE)                                                    \
  const bool counting_ = g_count.load(std::memory_order_relaxed);             \
  if (counting_) CountSite(OP, _ReturnAddress(), TABLE);                      \
  const bool live_ = c == g_live.load(std::memory_order_relaxed) && Hooked(c, OP); \
  if (!live_ && !g_ctx.load(std::memory_order_relaxed) && CaptureAt(OP)) Capture(c); \
  if (counting_) {                                                            \
    if (live_)                                                                \
      g_c.calls[OP]++;                                                        \
    else                                                                      \
      g_c.other++;                                                            \
  }

#define SF_SKIP(OP, VERIFY)                                       \
  do {                                                            \
    if (!g_verify.load(std::memory_order_relaxed) || (VERIFY)) { \
      if (counting_) g_c.skipped[OP]++;                           \
      return;                                                     \
    }                                                             \
  } while (0)

void STDMETHODCALLTYPE F_OMSetBlendState(Ctx* c, ID3D11BlendState* b, const FLOAT f[4], UINT mask) {
  SF_ENTER(kBlend, -1)
  if (live_) {
    float ff[4];
    Factor(f, ff);
    if (g_sh.blend == b && memcmp(g_sh.blendFactor, ff, sizeof(ff)) == 0 && g_sh.sampleMask == mask)
      SF_SKIP(kBlend, ([&] {
                ID3D11BlendState* got = nullptr;
                float gf[4] = {};
                UINT gm = 0;
                c->OMGetBlendState(&got, gf, &gm);
                if (memcmp(gf, ff, sizeof(ff)) != 0 || gm != mask) Mismatch(kBlend, "factor/mask", mask, gm);
                return VerifyObject(kBlend, b, got) && memcmp(gf, ff, sizeof(ff)) == 0 && gm == mask;
              }()));
    g_sh.blend = b;
    memcpy(g_sh.blendFactor, ff, sizeof(ff));
    g_sh.sampleMask = mask;
    return Orig<decltype(&F_OMSetBlendState)>(kBlend)(c, b, f, mask);
  }
  Table<decltype(&F_OMSetBlendState)>(c, kBlend)(c, b, f, mask);
}

void STDMETHODCALLTYPE F_OMSetDepthStencilState(Ctx* c, ID3D11DepthStencilState* d, UINT ref) {
  SF_ENTER(kDepth, -1)
  if (live_) {
    // Full 32-bit reference compared (d3dstate.h compared the low 8 bits only).
    if (g_sh.depth == d && g_sh.stencilRef == ref)
      SF_SKIP(kDepth, ([&] {
                ID3D11DepthStencilState* got = nullptr;
                UINT gr = 0;
                c->OMGetDepthStencilState(&got, &gr);
                // d3d11 keeps only the low 8 bits of the reference [M 2026-10-09: DCS
                // sets 0xFFFFFFFF, OMGetDepthStencilState returns 0xFF].
                if (gr != (ref & 0xff)) Mismatch(kDepth, "stencil ref", ref, gr);
                return VerifyObject(kDepth, d, got) && gr == (ref & 0xff);
              }()));
    g_sh.depth = d;
    g_sh.stencilRef = ref;
    return Orig<decltype(&F_OMSetDepthStencilState)>(kDepth)(c, d, ref);
  }
  Table<decltype(&F_OMSetDepthStencilState)>(c, kDepth)(c, d, ref);
}

void STDMETHODCALLTYPE F_RSSetState(Ctx* c, ID3D11RasterizerState* r) {
  SF_ENTER(kRS, -1)
  if (live_) {
    if (g_sh.rs == r) SF_SKIP(kRS, ([&] {
                        ID3D11RasterizerState* got = nullptr;
                        c->RSGetState(&got);
                        return VerifyObject(kRS, r, got);
                      }()));
    g_sh.rs = r;
    return Orig<decltype(&F_RSSetState)>(kRS)(c, r);
  }
  Table<decltype(&F_RSSetState)>(c, kRS)(c, r);
}

void STDMETHODCALLTYPE F_IASetPrimitiveTopology(Ctx* c, D3D11_PRIMITIVE_TOPOLOGY t) {
  SF_ENTER(kTopo, -1)
  if (live_) {
    if (g_sh.topo == static_cast<UINT>(t)) SF_SKIP(kTopo, ([&] {
                                               D3D11_PRIMITIVE_TOPOLOGY got = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
                                               c->IAGetPrimitiveTopology(&got);
                                               if (got != t) Mismatch(kTopo, "value", t, got);
                                               return got == t;
                                             }()));
    g_sh.topo = t;
    return Orig<decltype(&F_IASetPrimitiveTopology)>(kTopo)(c, t);
  }
  Table<decltype(&F_IASetPrimitiveTopology)>(c, kTopo)(c, t);
}

void STDMETHODCALLTYPE F_IASetInputLayout(Ctx* c, ID3D11InputLayout* l) {
  SF_ENTER(kIL, -1)
  if (live_) {
    if (g_sh.il == l) SF_SKIP(kIL, ([&] {
                        ID3D11InputLayout* got = nullptr;
                        c->IAGetInputLayout(&got);
                        return VerifyObject(kIL, l, got);
                      }()));
    g_sh.il = l;
    return Orig<decltype(&F_IASetInputLayout)>(kIL)(c, l);
  }
  Table<decltype(&F_IASetInputLayout)>(c, kIL)(c, l);
}

#define SF_SHADER(NAME, TYPE, OP, FIELD, TABLE)                                                      \
  void STDMETHODCALLTYPE F_##NAME(Ctx* c, TYPE* s, ID3D11ClassInstance* const* ci, UINT nci) {      \
    SF_ENTER(OP, TABLE)                                                                              \
    if (live_) {                                                                                     \
      if (nci == 0 && g_sh.FIELD == s) SF_SKIP(OP, VerifyShader(c, OP, s));                          \
      g_sh.FIELD = nci == 0 ? static_cast<void*>(s) : kUnknown;                                      \
      return Orig<decltype(&F_##NAME)>(OP)(c, s, ci, nci);                                           \
    }                                                                                                \
    Table<decltype(&F_##NAME)>(c, OP)(c, s, ci, nci);                                                \
  }
SF_SHADER(VSSetShader, ID3D11VertexShader, kVS, vs, 0)
SF_SHADER(PSSetShader, ID3D11PixelShader, kPS, ps, 3)

#define SF_RANGE(NAME, TYPE, OP, FIELD, GET, TABLE)                                                        \
  void STDMETHODCALLTYPE F_##NAME(Ctx* c, UINT s, UINT n, TYPE* const* v) {                               \
    SF_ENTER(OP, TABLE)                                                                                    \
    if (live_) {                                                                                           \
      if (SameRange(g_sh.FIELD, s, n, reinterpret_cast<const void* const*>(v)))                            \
        SF_SKIP(OP, VerifyRange<TYPE>(c, OP, s, n, v, &ID3D11DeviceContext::GET));                         \
      StoreRange(g_sh.FIELD, s, n, reinterpret_cast<const void* const*>(v));                               \
      return Orig<decltype(&F_##NAME)>(OP)(c, s, n, v);                                                    \
    }                                                                                                      \
    Table<decltype(&F_##NAME)>(c, OP)(c, s, n, v);                                                         \
  }
SF_RANGE(VSSetConstantBuffers, ID3D11Buffer, kVSCB, vsCb, VSGetConstantBuffers, 1)
SF_RANGE(PSSetConstantBuffers, ID3D11Buffer, kPSCB, psCb, PSGetConstantBuffers, 4)
SF_RANGE(VSSetSamplers, ID3D11SamplerState, kVSSamp, vsSamp, VSGetSamplers, 2)
SF_RANGE(PSSetSamplers, ID3D11SamplerState, kPSSamp, psSamp, PSGetSamplers, 5)

#undef SF_SHADER
#undef SF_RANGE
#undef SF_SKIP
#undef SF_ENTER

// The table index (TABLE above) is the entry's index in kTables; the test's
// fake tables use the same order.
void* const kFilters[kOpCount] = {
    reinterpret_cast<void*>(&F_VSSetShader),          reinterpret_cast<void*>(&F_PSSetShader),
    reinterpret_cast<void*>(&F_VSSetConstantBuffers), reinterpret_cast<void*>(&F_PSSetConstantBuffers),
    reinterpret_cast<void*>(&F_VSSetSamplers),        reinterpret_cast<void*>(&F_PSSetSamplers),
    reinterpret_cast<void*>(&F_IASetInputLayout),     reinterpret_cast<void*>(&F_IASetPrimitiveTopology),
    reinterpret_cast<void*>(&F_OMSetBlendState),      reinterpret_cast<void*>(&F_OMSetDepthStencilState),
    reinterpret_cast<void*>(&F_RSSetState)};

// DCS's immediate context: the first context a site passes (see CaptureAt)
// that d3d11 calls immediate.
__declspec(noinline) void Capture(Ctx* c) {
  if (c->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE) return;
  Ctx* none = nullptr;
  g_ctx.compare_exchange_strong(none, c);
}


// ---------------------------------------------------------------------------
// Context table hooks (as d3dstate.h's, restricted to kHookCount slots)
// ---------------------------------------------------------------------------

// Puts the hooks back where d3d11 replaced them, taking d3d11's new pointer
// as the original. The shadow is forgotten first: calls made while a hook was
// gone were not seen. Caller holds g_patchLock.
// R48: never over another hook of this payload. Such a hook took ours as its
// original when it went in (fixation_depth.h's state-call toggle points, a
// re-arming suite hook); taking it as ours made the two forward to each other
// until the stack ran out (R47: DCS gone without a crash report). It is left on
// top: the slot's calls reach ours through it, or, if it forwards to d3d11
// directly, each site call of that op finds the slot not ours (Hooked), forgets
// the shadow and forwards; ours goes back once the slot shows d3d11 again.
bool RepatchLocked() {
  void** vt = g_table;
  bool reverted = false;
  for (int i = 0; i < kHookCount; ++i) {
    if (!On(i) || vt[g_slot[i]] == kHooks[i]) continue;
    if (d3ds::InPayload(vt[g_slot[i]])) {
      g_oursOnTop.fetch_add(1, std::memory_order_relaxed);
      continue;
    }
    reverted = true;
  }
  if (!reverted) return false;
  g_sh.ForgetAll();
  g_rewrites.fetch_add(1, std::memory_order_relaxed);
  MemoryBarrier();
  for (int i = 0; i < kHookCount; ++i) {
    if (!On(i)) continue;
    void* cur = vt[g_slot[i]];
    if (cur == kHooks[i] || d3ds::InPayload(cur)) continue;  // R48: not over a hook of ours
    if (cur != g_orig[i]) g_switches.fetch_add(1, std::memory_order_relaxed);
    g_orig[i] = cur;
    d3ds::WriteTablePointer(&vt[g_slot[i]], kHooks[i]);
  }
  if (auto f = g_afterRewrite.load(std::memory_order_acquire)) f();
  return true;
}

void WithdrawLocked() {
  void** vt = g_table;
  for (int i = 0; i < kHookCount; ++i)
    if (On(i) && vt[g_slot[i]] == kHooks[i]) d3ds::WriteTablePointer(&vt[g_slot[i]], g_orig[i]);
}

bool SafeRepatch(bool want) {
  __try {
    if (want)
      RepatchLocked();
    else
      WithdrawLocked();
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

// Filter thread: re-hook at once unless another thread is doing it.
bool TryRepatchInline() {
  if (!g_hooked.load(std::memory_order_relaxed) || !TryAcquireSRWLockExclusive(&g_patchLock)) return false;
  const bool ok = g_hooked.load() && SafeRepatch(true);
  ReleaseSRWLockExclusive(&g_patchLock);
  return ok;
}

bool HookTable() {
  if (g_hooked.load()) return true;
  Ctx* c = g_ctx.load();
  if (!c) return false;
  g_table = *reinterpret_cast<void***>(c);
  AcquireSRWLockExclusive(&g_patchLock);
  for (int i = 0; i < kHookCount; ++i) {
    if (!On(i)) continue;
    void** slot = &g_table[g_slot[i]];
    if (g_api) g_api->restore(slot);  // drop stale registry entries an older payload made here
    g_orig[i] = *slot;
  }
  g_hooked = true;
  const bool ok = SafeRepatch(true);
  ReleaseSRWLockExclusive(&g_patchLock);
  if (!ok) {
    g_hooked = false;
    return false;
  }
  return true;
}

void UnhookTable() {
  AcquireSRWLockExclusive(&g_patchLock);
  if (g_hooked.exchange(false)) SafeRepatch(false);
  ReleaseSRWLockExclusive(&g_patchLock);
}

// ---------------------------------------------------------------------------
// Checks and patching
// ---------------------------------------------------------------------------

// FNV-1a 64 over any length (shadowtex::CodeIs reads at most 512 bytes).
bool CodeIs(const uint8_t* base, uint32_t begin, uint32_t end, uint64_t hash) {
  uint64_t h = 0xcbf29ce484222325ull;
  uint8_t buf[256];
  for (uint32_t at = begin; at < end;) {
    const uint32_t n = end - at < sizeof(buf) ? end - at : static_cast<uint32_t>(sizeof(buf));
    if (!allocslab::ReadBytes(base + at, buf, n)) return false;
    for (uint32_t i = 0; i < n; ++i) {
      h ^= buf[i];
      h *= 0x100000001b3ull;
    }
    at += n;
  }
  return h == hash;
}

int MethodOffset(int op) { return DcsQv_ContextSlot(kSlotNames[op]) * 8; }

// A vcall thunk for context method `op`: mov rax,[rcx]; jmp [rax+disp8/32].
bool IsThunk(const void* p, int op) {
  uint8_t b[9] = {};
  if (!p || !allocslab::ReadBytes(p, b, sizeof(b)) || b[0] != 0x48 || b[1] != 0x8B || b[2] != 0x01 || b[3] != 0xFF)
    return false;
  const int off = MethodOffset(op);
  if (b[4] == 0x60) return off < 0x80 && b[5] == off;
  int32_t d;
  memcpy(&d, b + 5, 4);
  return b[4] == 0xA0 && d == off;
}

// Site bytes and table entries (production and tests).
const char* CheckSites(uint8_t* base) {
  for (int i = 0; i < kHookCount; ++i)
    if (DcsQv_ContextSlot(kSlotNames[i]) < 0) return "split filter: unknown context slot; skipped";
  if (g_siteCount > kMaxSites || g_tableCount > kMaxTables) return "split filter: too many sites; skipped";
  for (int i = 0; i < g_siteCount; ++i) {
    uint8_t b[6];
    if (!allocslab::ReadBytes(base + g_sites[i].rva, b, 6)) return "split filter: a call site is not readable; skipped";
    if (b[0] == 0x90 && b[1] == 0xE8)
      return "split filter: a call site is still patched by an earlier payload; skipped";
    int32_t off;
    memcpy(&off, b + 2, 4);
    if (b[0] != 0xFF || b[1] != 0x90 || off != MethodOffset(g_sites[i].op))
      return "split filter: unexpected bytes at a call site; skipped";
  }
  for (int i = 0; i < g_tableCount; ++i) {
    void* cur = SlotOriginal(reinterpret_cast<void**>(base + g_tables[i].rva));
    if (!IsThunk(cur, g_tables[i].op)) return "split filter: an FX call-table entry is not the expected thunk; skipped";
  }
  return nullptr;
}

const char* CheckDcs(uint8_t* dx) {
  const HMODULE m = reinterpret_cast<HMODULE>(dx);
  if (reinterpret_cast<uint8_t*>(GetProcAddress(m, kDrawExport)) != dx + kDrawRva ||
      reinterpret_cast<uint8_t*>(GetProcAddress(m, kSvbExport)) != dx + kSvbRva)
    return "split filter: DX11Renderer exports are not where the analysed build has them; skipped";
  for (const CodeRange& r : kCode)
    if (!CodeIs(dx, r.begin, r.end, r.hash)) return "split filter: dx11backend code differs from the analysed build; skipped";
  return nullptr;
}

// Writes every site with all other threads suspended, retrying while a thread
// stands inside one of them (as posesweep::PatchAllSuspended, all sites at once).
bool WriteSitesSuspended(bool on) {
  uint8_t want[kMaxSites][6];
  for (int i = 0; i < g_siteCount; ++i) {
    uint8_t* site = g_base + g_sites[i].rva;
    if (on && !On(g_sites[i].op)) {
      memcpy(g_savedSite[i], site, 6);  // op not filtered: left as is
      memcpy(want[i], site, 6);
    } else if (on) {
      memcpy(g_savedSite[i], site, 6);
      uint8_t* stub = g_stub + 16 * g_sites[i].op;
      const int32_t rel = static_cast<int32_t>(stub - (site + 6));
      want[i][0] = 0x90;  // nop; call rel32: returns to site+6 as the original call did
      want[i][1] = 0xE8;
      memcpy(&want[i][2], &rel, 4);
    } else {
      memcpy(want[i], g_savedSite[i], 6);
    }
  }
  // R90: qvpatch::WriteSuspended (patch_threads.h; [General] PatchMode).
  qvpatch::Site s[kMaxSites];
  for (int i = 0; i < g_siteCount; ++i) s[i] = {g_base + g_sites[i].rva, want[i], 6};
  return qvpatch::WriteSuspended(s, g_siteCount, 50);
}

void UnhookTables() {
  for (int k = 0; k < g_tableCount; ++k)
    if (g_tableHooked[k]) {
      UnhookSlot(reinterpret_cast<void**>(g_base + g_tables[k].rva), g_tableOrig[k]);
      g_tableHooked[k] = false;
    }
}

bool PatchSites(bool on) {
  if (g_sitesPatched.load() == on) return true;
  if (on) {
    for (int i = 0; i < g_tableCount; ++i) {
      if (!On(g_tables[i].op)) continue;
      void** slot = reinterpret_cast<void**>(g_base + g_tables[i].rva);
      if (!HookSlot(slot, kFilters[g_tables[i].op], &g_tableOrig[i])) {
        UnhookTables();
        return false;
      }
      g_tableHooked[i] = true;
    }
    if (!WriteSitesSuspended(true)) {
      WriteSitesSuspended(false);
      UnhookTables();
      return false;
    }
  } else {
    if (!WriteSitesSuspended(false)) Log("split filter: WARNING could not restore the call sites");
    UnhookTables();
  }
  g_sitesPatched = on;
  return true;
}

// ---------------------------------------------------------------------------
// Install / attach / detach
// ---------------------------------------------------------------------------

// Checks and stubs; production passes dx11backend, tests a fake module with
// their own sites (dcs = false: no export or hash checks).
bool InstallAt(uint8_t* base, bool dcs) {
  const int st = g_state.load();
  if (st != 0) return st > 0;
  if (d3ds::g_vtblCount.load() > 0) {
    Log("split filter: the D3D meter ([D3D] Meter=1) hooks the same context; skipped");
    g_state = -1;
    return false;
  }
  const char* why = dcs ? CheckDcs(base) : nullptr;
  if (!why) why = CheckSites(base);
  if (why) {
    Log("%s", why);
    g_state = -1;
    return false;
  }
  for (int i = 0; i < kHookCount; ++i) g_slot[i] = DcsQv_ContextSlot(kSlotNames[i]);
  uint8_t* page = pacer::AllocNear(base);
  if (!page) {
    Log("split filter: no memory within reach of the patched module; skipped");
    g_state = -1;
    return false;
  }
  memset(page, 0xCC, 4096);
  for (int op = 0; op < kOpCount; ++op) {
    uint8_t* p = page + 16 * op;
    p[0] = 0x48;
    p[1] = 0xB8;  // mov rax, imm64
    memcpy(p + 2, &kFilters[op], 8);
    p[10] = 0xFF;
    p[11] = 0xE0;  // jmp rax
  }
  DWORD old;
  VirtualProtect(page, 4096, PAGE_EXECUTE_READ, &old);
  FlushInstructionCache(GetCurrentProcess(), page, 4096);
  g_stub = page;
  g_base = base;
  g_state = 1;
  Log("split filter: %d call sites and %d FX call-table entries checked", g_siteCount, g_tableCount);
  return true;
}

bool Install() {
  if (g_state.load() != 0) return g_state.load() > 0;
  if (g_cfg.d3dMeter || d3ds::g_rendererDrawSlot) {  // the meter hooks the same table at its first draw
    Log("split filter: the D3D meter ([D3D] Meter=1) hooks the same context; skipped");
    g_state = -1;
    return false;
  }
  auto* dx = reinterpret_cast<uint8_t*>(GetModuleHandleW(L"dx11backend.dll"));
  if (!dx) return false;  // retried
  return InstallAt(dx, true);
}

// Order: vtable hooks first, then the shadow is forgotten, then filtering
// starts. The sites may be patched before the context is known: until then
// they forward and capture it (the next Attach completes the install).
bool Attach() {
  if (g_state.load() <= 0 || g_disabled.load()) return false;
  if (!g_sitesPatched.load() && !g_hooked.load()) g_active = g_opMask.load();
  if (!g_sitesPatched.load() && !PatchSites(true)) {
    Log("split filter: could not patch the call sites; off");
    return false;
  }
  g_attached = true;
  if (!g_ctx.load()) return false;  // waiting for the first draw
  if (!g_hooked.load()) {
    // R146: DCS's context is a device proxy's wrapper (ReShade): its own calls on the real context never reach the
    // shadow, so nothing is skipped. Sites back to stock (they only forwarded and captured so far), latched.
    if (d3dproxy::RefuseSplitFilter(g_ctx.load())) {
      if (g_sitesPatched.load()) PatchSites(false);
      g_attached = false;
      g_state = -1;
      return false;
    }
    if (!HookTable()) {
      Log("split filter: could not hook the context table; off");
      return false;
    }
    Log("split filter: DCS context %p, table %p hooked; filtered ops 0x%x", static_cast<void*>(g_ctx.load()),
        static_cast<void*>(g_table), g_active);
  }
  g_sh.ForgetAll();
  MemoryBarrier();
  g_live = g_ctx.load();
  return true;
}

void Detach() {
  g_live = nullptr;
  if (g_sitesPatched.load()) PatchSites(false);
  UnhookTable();
  g_attached = false;
}

bool Live() { return g_live.load() != nullptr; }

// Payload stop / hot reload: everything back to stock.
void Shutdown() {
  if (g_state.load() > 0) Detach();
}

// ---------------------------------------------------------------------------
// Report
// ---------------------------------------------------------------------------

void LogCounters(const char* label, double frames) {
  const Counters c = g_c;
  if (frames <= 0) frames = 1;
  uint64_t calls = 0, skipped = 0, foreign = 0, mism = 0;
  double estMs = 0;
  for (int i = 0; i < kOpCount; ++i) {
    calls += c.calls[i];
    skipped += c.skipped[i];
    foreign += c.foreign[i];
    mism += c.mismatch[i];
    estMs += c.skipped[i] * kNsPerSkip[i] * 1e-6 / frames;
  }
  Log("  split filter (%s): ops 0x%x, %.0f frames, %s%s; site calls %.0f/frame, skipped %.0f/frame (%.1f%%), foreign calls "
      "%.0f/frame, foreign invalidations %.1f/frame, table rewrites found at a site %.1f/frame (re-hooks %llu, "
      "implementation switches %llu, refused over another hook of ours %llu), other context or not live %.0f/frame, "
      "verify mismatches %llu",
      label, g_active, frames, Live() ? "live" : "not live", g_disabled.load() ? ", DISABLED" : "", calls / frames,
      skipped / frames, calls ? 100.0 * skipped / calls : 0.0, foreign / frames, c.resets / frames, c.lost / frames,
      static_cast<unsigned long long>(g_rewrites.load()), static_cast<unsigned long long>(g_switches.load()),
      static_cast<unsigned long long>(g_oursOnTop.load()), c.other / frames, static_cast<unsigned long long>(mism));
  for (int i = 0; i < kOpCount; ++i) {
    if (!c.calls[i] && !c.foreign[i]) continue;
    Log("    %-8s site calls/frame %7.0f  skipped %5.1f%%  foreign calls/frame %6.0f  mismatches %llu", kOpNames[i],
        c.calls[i] / frames, c.calls[i] ? 100.0 * c.skipped[i] / c.calls[i] : 0.0, c.foreign[i] / frames,
        static_cast<unsigned long long>(c.mismatch[i]));
  }
  {
    char line[1024];
    int n = snprintf(line, sizeof(line), "    per site calls/frame:");
    for (int i = 0; i < g_siteCount && n < static_cast<int>(sizeof(line)) - 32; ++i)
      n += snprintf(line + n, sizeof(line) - n, " %x=%.0f", g_sites[i].rva, c.site[i] / frames);
    for (int i = 0; i < g_tableCount && n < static_cast<int>(sizeof(line)) - 32; ++i)
      n += snprintf(line + n, sizeof(line) - n, " [%x]=%.0f", g_tables[i].rva, c.site[g_siteCount + i] / frames);
    Log("%s", line);
  }
  Log("    estimate at the offline ns per redundant call: %.3f ms/frame saved by the skips (filter overhead not "
      "included)",
      estMs);
  std::lock_guard<std::mutex> lock(g_sampleMutex);
  for (auto& l : g_samples) Log("    %s", l.c_str());
  g_samples.clear();
}

}  // namespace sfilt
