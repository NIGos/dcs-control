// D3D11 state-call redundancy meter. Hooks the ID3D11DeviceContext vtables in
// d3d11.dll (found by creating private devices: immediate, single-threaded and
// deferred contexts) and keeps a shadow copy of the bindings of every context
// it sees. Each state call is passed to the driver unchanged; the meter only
// counts how many set exactly what is already bound.
//
// Implicit state changes are handled so the shadow stays exact:
//  - OMSetRenderTargets*, CSSetUnorderedAccessViews: the runtime silently
//    unbinds conflicting shader resource views -> SRV shadow becomes unknown.
//  - SOSetTargets: may unbind vertex/index buffers -> unknown.
//  - ClearState, SwapDeviceContextState, ExecuteCommandList, FinishCommandList,
//    *SetConstantBuffers1: affected shadow becomes unknown.
// Included once from main.cpp after the globals it uses (Log).
#pragma once

#include <d3d11_1.h>

#pragma comment(lib, "d3d11.lib")

extern "C" int DcsQv_ContextSlot(const char* name);

namespace d3ds {

enum Op {
  kPSSRV, kVSSRV, kPSCB, kVSCB, kPSSamp, kVSSamp, kVB, kIB, kVS, kPS, kIL, kTopo, kBlend, kDepth, kRS,
  kOpCount
};
const char* const kOpNames[kOpCount] = {"PS SRV",  "VS SRV", "PS CB", "VS CB",   "PS samp",
                                        "VS samp", "VB",     "IB",    "VS",      "PS",
                                        "layout",  "topo",   "blend", "depth",   "raster"};

// Sentinel meaning "binding unknown": never equal to a real pointer.
void* const kUnknown = reinterpret_cast<void*>(1);

struct Shadow {
  void* psSrv[D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT];
  void* vsSrv[D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT];
  void* psCb[D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT];
  void* vsCb[D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT];
  void* psSamp[D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT];
  void* vsSamp[D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT];
  void* vb[D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT];
  UINT vbStride[D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT];
  UINT vbOffset[D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT];
  void* ib;
  UINT ibFormat, ibOffset;
  void* vs;
  void* ps;
  void* il;
  UINT topo;
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
  void ForgetSrv() {
    Forget(psSrv);
    Forget(vsSrv);
  }
  void ForgetIa() {
    Forget(vb);
    ib = kUnknown;
  }
  void ForgetInputs() {
    ForgetSrv();
    Forget(psCb);
    Forget(vsCb);
    ForgetIa();
  }
  void ForgetAll() {
    ForgetSrv();
    Forget(psCb);
    Forget(vsCb);
    Forget(psSamp);
    Forget(vsSamp);
    ForgetIa();
    vs = ps = il = blend = depth = rs = kUnknown;
    topo = ~0u;
  }
};

// Per-thread counters, registered once so the stats thread can sum them
// without atomics on the hot path.
struct Counters {
  uint64_t calls[kOpCount];
  uint64_t redundant[kOpCount];
  uint64_t mismatch[kOpCount];
  uint64_t draws;
  uint64_t resets;  // ClearState, SwapDeviceContextState, ExecuteCommandList
};
std::mutex g_regMutex;
std::vector<Counters*> g_counters;

Counters* MyCounters() {
  thread_local Counters* c = nullptr;
  if (!c) {
    c = new Counters{};
    std::lock_guard<std::mutex> lock(g_regMutex);
    g_counters.push_back(c);
  }
  return c;
}

// Shadow lookup: few contexts exist, a thread-local cache hits almost always.
struct CtxEntry {
  void* ctx;
  Shadow* shadow;
};
constexpr int kMaxCtx = 64;
CtxEntry g_ctx[kMaxCtx];
std::atomic<int> g_ctxCount{0};
std::mutex g_ctxMutex;

// The first context seen (DCS's immediate context) is checked without TLS.
void* volatile g_fastCtx = nullptr;
Shadow* volatile g_fastShadow = nullptr;

Shadow* ShadowOfSlow(void* ctx);
inline Shadow* ShadowOf(void* ctx) {
  if (ctx == g_fastCtx) return g_fastShadow;
  return ShadowOfSlow(ctx);
}

Shadow* ShadowOfSlow(void* ctx) {
  thread_local void* lastCtx = nullptr;
  thread_local Shadow* lastShadow = nullptr;
  if (ctx == lastCtx) return lastShadow;
  int n = g_ctxCount.load(std::memory_order_acquire);
  for (int i = 0; i < n; ++i)
    if (g_ctx[i].ctx == ctx) {
      lastCtx = ctx;
      lastShadow = g_ctx[i].shadow;
      return lastShadow;
    }
  std::lock_guard<std::mutex> lock(g_ctxMutex);
  n = g_ctxCount.load();
  for (int i = 0; i < n; ++i)
    if (g_ctx[i].ctx == ctx) return g_ctx[i].shadow;
  if (n >= kMaxCtx) return nullptr;
  auto* s = new Shadow;
  s->ForgetAll();
  g_ctx[n] = {ctx, s};
  g_ctxCount.store(n + 1, std::memory_order_release);
  if (n == 0) {
    g_fastShadow = s;
    g_fastCtx = ctx;
  }
  lastCtx = ctx;
  lastShadow = s;
  return s;
}

// Compares and stores a range of pointer bindings. Returns true if redundant.
template <size_t N>
bool Range(void* (&shadow)[N], UINT start, UINT count, void* const* values) {
  if (start >= N || count > N - start) return false;
  bool same = values != nullptr;
  for (UINT i = 0; i < count; ++i) {
    void* v = values ? values[i] : nullptr;
    if (shadow[start + i] != v) same = false;
    shadow[start + i] = v;
  }
  return same;
}

void Count(Op op, bool redundant) {
  Counters* c = MyCounters();
  c->calls[op]++;
  if (redundant) c->redundant[op]++;
}

// ---- hooks ----------------------------------------------------------------

// Each patched vtable keeps its own originals (context classes in d3d11.dll
// need not share implementations); the hook picks them by the object's vtable.
#define ORIG(name) reinterpret_cast<decltype(&H_##name)>(OrigFor(c, k##name))
enum Slot {
  kVSSetConstantBuffers, kPSSetShaderResources, kPSSetShader, kPSSetSamplers, kVSSetShader, kDrawIndexed,
  kDraw, kPSSetConstantBuffers, kIASetInputLayout, kIASetVertexBuffers, kIASetIndexBuffer,
  kDrawIndexedInstanced, kDrawInstanced, kIASetPrimitiveTopology, kVSSetShaderResources, kVSSetSamplers,
  kOMSetRenderTargets, kOMSetRenderTargetsAndUnorderedAccessViews, kOMSetBlendState,
  kOMSetDepthStencilState, kRSSetState, kCSSetUnorderedAccessViews, kExecuteCommandList, kClearState,
  kVSSetConstantBuffers1, kPSSetConstantBuffers1, kSwapDeviceContextState, kSOSetTargets,
  kFinishCommandList, kSlotCount
};
const char* const kSlotNames[kSlotCount] = {
    "VSSetConstantBuffers", "PSSetShaderResources", "PSSetShader", "PSSetSamplers", "VSSetShader",
    "DrawIndexed", "Draw", "PSSetConstantBuffers", "IASetInputLayout", "IASetVertexBuffers",
    "IASetIndexBuffer", "DrawIndexedInstanced", "DrawInstanced", "IASetPrimitiveTopology",
    "VSSetShaderResources", "VSSetSamplers", "OMSetRenderTargets",
    "OMSetRenderTargetsAndUnorderedAccessViews", "OMSetBlendState", "OMSetDepthStencilState",
    "RSSetState", "CSSetUnorderedAccessViews", "ExecuteCommandList", "ClearState",
    "VSSetConstantBuffers1", "PSSetConstantBuffers1", "SwapDeviceContextState", "SOSetTargets",
    "FinishCommandList"};
constexpr int kMaxVtables = 8;
void** g_vtbl[kMaxVtables];
void* volatile g_origs[kMaxVtables][kSlotCount];
std::atomic<int> g_vtblCount{0};

void* OrigForSlow(void** vt, int slot);
inline void* OrigFor(void* obj, int slot) {
  void** vt = *reinterpret_cast<void***>(obj);
  if (vt == g_vtbl[0]) return g_origs[0][slot];
  return OrigForSlow(vt, slot);
}

void* OrigForSlow(void** vt, int slot) {
  int n = g_vtblCount.load(std::memory_order_acquire);
  for (int i = 0; i < n; ++i)
    if (g_vtbl[i] == vt) return g_origs[i][slot];
  return g_origs[0][slot];  // unreachable: only patched vtables reach the hooks
}

using Ctx = ID3D11DeviceContext1;

// 0 = count only, 1 = skip redundant calls, 2 = verify: redundant calls are
// still forwarded, but first the real binding is read back from d3d11 and
// compared with the shadow, so a skip that would have been wrong is counted.
std::atomic<int>& g_mode = g_d3dMode;
std::atomic<int> g_depthSamples{0};
std::mutex g_sampleMutex;
std::vector<std::string> g_samples;
extern std::atomic<uint64_t> g_rewrites;
inline int Mode() { return g_mode.load(std::memory_order_relaxed); }

void Mismatch(Op op) { MyCounters()->mismatch[op]++; }

template <class T>
void ReleaseAll(T** a, UINT n) {
  for (UINT i = 0; i < n; ++i)
    if (a[i]) a[i]->Release();
}

// Compares a range read back from d3d11 with the values being set.
template <class T>
bool SameRange(T** got, UINT n, T* const* want) {
  bool same = true;
  for (UINT i = 0; i < n; ++i)
    if (got[i] != (want ? want[i] : nullptr)) same = false;
  ReleaseAll(got, n);
  return same;
}

// Shared tail of every state hook: count, then skip / verify / forward.
#define DISPATCH(OP, SAME, VERIFY, CALL)        \
  do {                                          \
    bool same_ = (SAME);                        \
    int m_ = Mode();                            \
    if (m_ == 1) {                              \
      if (same_) return;                        \
    } else {                                    \
      Count(OP, same_);                         \
      if (same_ && m_ == 2 && !(VERIFY)) Mismatch(OP); \
    }                                           \
    CALL;                                       \
  } while (0)

#define RANGE_HOOK(NAME, TYPE, FIELD, OP, GET)                                    \
  void STDMETHODCALLTYPE H_##NAME(Ctx* c, UINT s, UINT n, TYPE* const* v) {      \
    Shadow* sh = ShadowOf(c);                                                     \
    DISPATCH(OP, sh && Range(sh->FIELD, s, n, reinterpret_cast<void* const*>(v)), \
             ([&] {                                                               \
               TYPE* got[128] = {};                                               \
               if (n > 128) return true;                                          \
               c->GET(s, n, got);                                                 \
               return SameRange(got, n, v);                                       \
             }()),                                                                \
             ORIG(NAME)(c, s, n, v));                                             \
  }
RANGE_HOOK(PSSetShaderResources, ID3D11ShaderResourceView, psSrv, kPSSRV, PSGetShaderResources)
RANGE_HOOK(VSSetShaderResources, ID3D11ShaderResourceView, vsSrv, kVSSRV, VSGetShaderResources)
RANGE_HOOK(PSSetConstantBuffers, ID3D11Buffer, psCb, kPSCB, PSGetConstantBuffers)
RANGE_HOOK(VSSetConstantBuffers, ID3D11Buffer, vsCb, kVSCB, VSGetConstantBuffers)
RANGE_HOOK(PSSetSamplers, ID3D11SamplerState, psSamp, kPSSamp, PSGetSamplers)
RANGE_HOOK(VSSetSamplers, ID3D11SamplerState, vsSamp, kVSSamp, VSGetSamplers)

void STDMETHODCALLTYPE H_IASetVertexBuffers(Ctx* c, UINT s, UINT n, ID3D11Buffer* const* b,
                                            const UINT* strides, const UINT* offsets) {
  Shadow* sh = ShadowOf(c);
  bool same = false;
  if (sh && b && strides && offsets && s < D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT &&
      n <= D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT - s) {
    same = true;
    for (UINT i = 0; i < n; ++i) {
      if (sh->vb[s + i] != b[i] || sh->vbStride[s + i] != strides[i] || sh->vbOffset[s + i] != offsets[i])
        same = false;
      sh->vb[s + i] = b[i];
      sh->vbStride[s + i] = strides[i];
      sh->vbOffset[s + i] = offsets[i];
    }
  } else if (sh) {
    Shadow::Forget(sh->vb);
  }
  DISPATCH(kVB, same, ([&] {
             ID3D11Buffer* got[D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT] = {};
             UINT st[D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT] = {};
             UINT of[D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT] = {};
             c->IAGetVertexBuffers(s, n, got, st, of);
             bool ok = true;
             for (UINT i = 0; i < n; ++i)
               if (st[i] != strides[i] || of[i] != offsets[i]) ok = false;
             return SameRange(got, n, b) && ok;
           }()),
           ORIG(IASetVertexBuffers)(c, s, n, b, strides, offsets));
}

void STDMETHODCALLTYPE H_IASetIndexBuffer(Ctx* c, ID3D11Buffer* b, DXGI_FORMAT f, UINT o) {
  Shadow* sh = ShadowOf(c);
  bool same = sh && sh->ib == b && sh->ibFormat == static_cast<UINT>(f) && sh->ibOffset == o;
  if (sh) {
    sh->ib = b;
    sh->ibFormat = f;
    sh->ibOffset = o;
  }
  DISPATCH(kIB, same, ([&] {
             ID3D11Buffer* got = nullptr;
             DXGI_FORMAT gf = DXGI_FORMAT_UNKNOWN;
             UINT go = 0;
             c->IAGetIndexBuffer(&got, &gf, &go);
             bool ok = got == b && gf == f && go == o;
             if (got) got->Release();
             return ok;
           }()),
           ORIG(IASetIndexBuffer)(c, b, f, o));
}

#define SHADER_HOOK(NAME, TYPE, FIELD, OP, GET)                                                    \
  void STDMETHODCALLTYPE H_##NAME(Ctx* c, TYPE* sh_, ID3D11ClassInstance* const* ci, UINT nci) { \
    Shadow* sh = ShadowOf(c);                                                                      \
    bool same = sh && nci == 0 && sh->FIELD == sh_;                                                \
    if (sh) sh->FIELD = nci == 0 ? static_cast<void*>(sh_) : kUnknown;                            \
    DISPATCH(OP, same, ([&] {                                                                      \
               TYPE* got = nullptr;                                                                \
               UINT gn = 0;                                                                        \
               c->GET(&got, nullptr, &gn);                                                         \
               bool ok = got == sh_ && gn == 0;                                                    \
               if (got) got->Release();                                                            \
               return ok;                                                                          \
             }()),                                                                                 \
             ORIG(NAME)(c, sh_, ci, nci));                                                         \
  }
SHADER_HOOK(VSSetShader, ID3D11VertexShader, vs, kVS, VSGetShader)
SHADER_HOOK(PSSetShader, ID3D11PixelShader, ps, kPS, PSGetShader)

void STDMETHODCALLTYPE H_IASetInputLayout(Ctx* c, ID3D11InputLayout* l) {
  Shadow* sh = ShadowOf(c);
  bool same = sh && sh->il == l;
  if (sh) sh->il = l;
  DISPATCH(kIL, same, ([&] {
             ID3D11InputLayout* got = nullptr;
             c->IAGetInputLayout(&got);
             bool ok = got == l;
             if (got) got->Release();
             return ok;
           }()),
           ORIG(IASetInputLayout)(c, l));
}

void STDMETHODCALLTYPE H_IASetPrimitiveTopology(Ctx* c, D3D11_PRIMITIVE_TOPOLOGY t) {
  Shadow* sh = ShadowOf(c);
  bool same = sh && sh->topo == static_cast<UINT>(t);
  if (sh) sh->topo = t;
  DISPATCH(kTopo, same, ([&] {
             D3D11_PRIMITIVE_TOPOLOGY got = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
             c->IAGetPrimitiveTopology(&got);
             return got == t;
           }()),
           ORIG(IASetPrimitiveTopology)(c, t));
}

void STDMETHODCALLTYPE H_OMSetBlendState(Ctx* c, ID3D11BlendState* b, const FLOAT f[4], UINT mask) {
  Shadow* sh = ShadowOf(c);
  float ff[4] = {1, 1, 1, 1};
  if (f) memcpy(ff, f, sizeof(ff));
  bool same = sh && sh->blend == b && memcmp(sh->blendFactor, ff, sizeof(ff)) == 0 && sh->sampleMask == mask;
  if (sh) {
    sh->blend = b;
    memcpy(sh->blendFactor, ff, sizeof(ff));
    sh->sampleMask = mask;
  }
  DISPATCH(kBlend, same, ([&] {
             ID3D11BlendState* got = nullptr;
             float gf[4] = {};
             UINT gm = 0;
             c->OMGetBlendState(&got, gf, &gm);
             bool ok = got == b && memcmp(gf, ff, sizeof(ff)) == 0 && gm == mask;
             if (got) got->Release();
             return ok;
           }()),
           ORIG(OMSetBlendState)(c, b, f, mask));
}

void STDMETHODCALLTYPE H_OMSetDepthStencilState(Ctx* c, ID3D11DepthStencilState* d, UINT ref) {
  // d3d11 keeps only the low 8 bits of the stencil reference (stencil
  // buffers are 8 bit): 0xFFFFFFFF and 0xFF are the same setting.
  const UINT ref8 = ref & 0xFF;
  Shadow* sh = ShadowOf(c);
  bool same = sh && sh->depth == d && sh->stencilRef == ref8;
  if (sh) {
    sh->depth = d;
    sh->stencilRef = ref8;
  }
  DISPATCH(kDepth, same, ([&] {
             ID3D11DepthStencilState* got = nullptr;
             UINT gr = 0;
             c->OMGetDepthStencilState(&got, &gr);
             bool ok = got == d && (gr & 0xFF) == ref8;
             if (!ok && g_depthSamples.fetch_add(1) < 8) {
               char line[200];
               snprintf(line, sizeof(line), "depth mismatch: set %p ref %u, d3d11 has %p ref %u, thread %lu, rewrites %llu",
                        static_cast<void*>(d), ref, static_cast<void*>(got), gr, GetCurrentThreadId(),
                        static_cast<unsigned long long>(g_rewrites.load()));
               std::lock_guard<std::mutex> lock(g_sampleMutex);
               g_samples.push_back(line);
             }
             if (got) got->Release();
             return ok;
           }()),
           ORIG(OMSetDepthStencilState)(c, d, ref));
}

void STDMETHODCALLTYPE H_RSSetState(Ctx* c, ID3D11RasterizerState* r) {
  Shadow* sh = ShadowOf(c);
  bool same = sh && sh->rs == r;
  if (sh) sh->rs = r;
  DISPATCH(kRS, same, ([&] {
             ID3D11RasterizerState* got = nullptr;
             c->RSGetState(&got);
             bool ok = got == r;
             if (got) got->Release();
             return ok;
           }()),
           ORIG(RSSetState)(c, r));
}

// Draws are only counted.
void STDMETHODCALLTYPE H_DrawIndexed(Ctx* c, UINT a, UINT b, INT d) {
  MyCounters()->draws++;
  ORIG(DrawIndexed)(c, a, b, d);
}
void STDMETHODCALLTYPE H_Draw(Ctx* c, UINT a, UINT b) {
  MyCounters()->draws++;
  ORIG(Draw)(c, a, b);
}
void STDMETHODCALLTYPE H_DrawIndexedInstanced(Ctx* c, UINT a, UINT b, UINT d, INT e, UINT f) {
  MyCounters()->draws++;
  ORIG(DrawIndexedInstanced)(c, a, b, d, e, f);
}
void STDMETHODCALLTYPE H_DrawInstanced(Ctx* c, UINT a, UINT b, UINT d, UINT e) {
  MyCounters()->draws++;
  ORIG(DrawInstanced)(c, a, b, d, e);
}

// Invalidations. Binding a resource for writing (render target, UAV, stream
// output) makes d3d11 silently unbind it from every input slot: shader
// resources, constant, vertex and index buffers. All input shadows go.
void STDMETHODCALLTYPE H_OMSetRenderTargets(Ctx* c, UINT n, ID3D11RenderTargetView* const* r,
                                            ID3D11DepthStencilView* d) {
  if (Shadow* sh = ShadowOf(c)) sh->ForgetInputs();
  ORIG(OMSetRenderTargets)(c, n, r, d);
}
void STDMETHODCALLTYPE H_OMSetRenderTargetsAndUnorderedAccessViews(
    Ctx* c, UINT n, ID3D11RenderTargetView* const* r, ID3D11DepthStencilView* d, UINT us, UINT un,
    ID3D11UnorderedAccessView* const* u, const UINT* ic) {
  if (Shadow* sh = ShadowOf(c)) sh->ForgetInputs();
  ORIG(OMSetRenderTargetsAndUnorderedAccessViews)(c, n, r, d, us, un, u, ic);
}
void STDMETHODCALLTYPE H_CSSetUnorderedAccessViews(Ctx* c, UINT s, UINT n, ID3D11UnorderedAccessView* const* u,
                                                   const UINT* ic) {
  if (Shadow* sh = ShadowOf(c)) sh->ForgetInputs();
  ORIG(CSSetUnorderedAccessViews)(c, s, n, u, ic);
}
void STDMETHODCALLTYPE H_SOSetTargets(Ctx* c, UINT n, ID3D11Buffer* const* b, const UINT* o) {
  if (Shadow* sh = ShadowOf(c)) sh->ForgetInputs();
  ORIG(SOSetTargets)(c, n, b, o);
}
void STDMETHODCALLTYPE H_ClearState(Ctx* c) {
  MyCounters()->resets++;
  if (Shadow* sh = ShadowOf(c)) sh->ForgetAll();
  ORIG(ClearState)(c);
}
void STDMETHODCALLTYPE H_ExecuteCommandList(Ctx* c, ID3D11CommandList* l, BOOL restore) {
  MyCounters()->resets++;
  if (Shadow* sh = ShadowOf(c)) sh->ForgetAll();
  ORIG(ExecuteCommandList)(c, l, restore);
}
HRESULT STDMETHODCALLTYPE H_FinishCommandList(Ctx* c, BOOL restore, ID3D11CommandList** l) {
  if (Shadow* sh = ShadowOf(c)) sh->ForgetAll();
  return ORIG(FinishCommandList)(c, restore, l);
}
void STDMETHODCALLTYPE H_SwapDeviceContextState(Ctx* c, ID3DDeviceContextState* s, ID3DDeviceContextState** p) {
  MyCounters()->resets++;
  if (Shadow* sh = ShadowOf(c)) sh->ForgetAll();
  ORIG(SwapDeviceContextState)(c, s, p);
  if (Shadow* sh = ShadowOf(c)) sh->ForgetAll();
}
void STDMETHODCALLTYPE H_VSSetConstantBuffers1(Ctx* c, UINT s, UINT n, ID3D11Buffer* const* b,
                                               const UINT* f, const UINT* k) {
  if (Shadow* sh = ShadowOf(c)) Shadow::Forget(sh->vsCb);
  ORIG(VSSetConstantBuffers1)(c, s, n, b, f, k);
}
void STDMETHODCALLTYPE H_PSSetConstantBuffers1(Ctx* c, UINT s, UINT n, ID3D11Buffer* const* b,
                                               const UINT* f, const UINT* k) {
  if (Shadow* sh = ShadowOf(c)) Shadow::Forget(sh->psCb);
  ORIG(PSSetConstantBuffers1)(c, s, n, b, f, k);
}

void* const kHooks[kSlotCount] = {
    &H_VSSetConstantBuffers, &H_PSSetShaderResources, &H_PSSetShader, &H_PSSetSamplers, &H_VSSetShader,
    &H_DrawIndexed, &H_Draw, &H_PSSetConstantBuffers, &H_IASetInputLayout, &H_IASetVertexBuffers,
    &H_IASetIndexBuffer, &H_DrawIndexedInstanced, &H_DrawInstanced, &H_IASetPrimitiveTopology,
    &H_VSSetShaderResources, &H_VSSetSamplers, &H_OMSetRenderTargets,
    &H_OMSetRenderTargetsAndUnorderedAccessViews, &H_OMSetBlendState, &H_OMSetDepthStencilState,
    &H_RSSetState, &H_CSSetUnorderedAccessViews, &H_ExecuteCommandList, &H_ClearState,
    &H_VSSetConstantBuffers1, &H_PSSetConstantBuffers1, &H_SwapDeviceContextState, &H_SOSetTargets,
    &H_FinishCommandList};

// ---- installation -----------------------------------------------------------

// The immediate context's vtable lives inside the context object (heap) and
// d3d11.dll rewrites it at runtime (e.g. to switch between its locked and
// unlocked implementations), dropping our pointers. A refresher thread puts
// the hooks back every millisecond, each time taking whatever d3d11 wrote as
// the new original, so calls always end in the implementation d3d11 chose.
void RestoreDrawSlot();
extern void** g_rendererDrawSlot;
extern std::atomic<bool> g_captured;
int g_slot[kSlotCount];
HANDLE g_refresher = nullptr;
std::atomic<bool> g_stopRefresher{false};
std::atomic<uint64_t> g_reverts{0};

std::atomic<uint64_t> g_rewrites{0}, g_variantSwitches{0};

// false: the refresher keeps d3d11's own pointers in the table (no hook
// overhead at all); true: hooks are (re)applied.
std::atomic<bool> g_hooksWanted{true};

// Writes one vtable entry. DCS's immediate context keeps its table on the
// heap (writable), other contexts may point into d3d11.dll's read-only data.
void WriteTablePointer(void** slot, void* value) {
  MEMORY_BASIC_INFORMATION mbi{};
  if (VirtualQuery(slot, &mbi, sizeof(mbi)) &&
      (mbi.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_WRITECOPY))) {
    InterlockedExchangePointer(slot, value);
    return;
  }
  DWORD old;
  if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) return;
  InterlockedExchangePointer(slot, value);
  VirtualProtect(slot, sizeof(void*), old, &old);
}

void Repatch(int v);

void Withdraw(int v) {
  void** vt = g_vtbl[v];
  for (int i = 0; i < kSlotCount; ++i)
    if (vt[g_slot[i]] == kHooks[i]) WriteTablePointer(&vt[g_slot[i]], g_origs[v][i]);
}

// A context can be destroyed while the refresher runs (tests, device loss):
// a fault stops the refresher instead of the process.
bool SafeRefresh(bool want) {
  __try {
    int n = g_vtblCount.load(std::memory_order_acquire);
    for (int v = 0; v < n; ++v) want ? Repatch(v) : Withdraw(v);
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

void Repatch(int v) {
  void** vt = g_vtbl[v];
  bool reverted = false;
  for (int i = 0; i < kSlotCount; ++i)
    if (vt[g_slot[i]] != kHooks[i]) reverted = true;
  if (!reverted) return;
  // Calls made while our pointers were gone were not seen: every shadow of
  // this context is stale. Forget them before the hooks are back, so no
  // hooked call can compare against an outdated binding.
  g_rewrites.fetch_add(1, std::memory_order_relaxed);
  int nctx = g_ctxCount.load(std::memory_order_acquire);
  for (int k = 0; k < nctx; ++k)
    if (*static_cast<void***>(g_ctx[k].ctx) == vt) g_ctx[k].shadow->ForgetAll();
  MemoryBarrier();
  for (int i = 0; i < kSlotCount; ++i) {
    void* cur = vt[g_slot[i]];
    if (cur == kHooks[i]) continue;
    if (cur != g_origs[v][i]) g_variantSwitches.fetch_add(1, std::memory_order_relaxed);
    g_origs[v][i] = cur;
    WriteTablePointer(&vt[g_slot[i]], kHooks[i]);
    g_reverts.fetch_add(1, std::memory_order_relaxed);
  }
}

DWORD WINAPI Refresher(void*) {
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
  HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
  while (!g_stopRefresher.load()) {
    if (!SafeRefresh(g_hooksWanted.load())) {
      Log("d3d meter: context table no longer accessible; refresher stopped");
      break;
    }
    if (timer) {
      LARGE_INTEGER due;
      due.QuadPart = -10000;  // 1 ms
      SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE);
      WaitForSingleObject(timer, 100);
    } else {
      Sleep(1);
    }
  }
  if (timer) CloseHandle(timer);
  return 0;
}

// Puts d3d11's own pointers back (payload stop / hot reload).
void Uninstall() {
  g_stopRefresher = true;
  if (g_refresher) {
    WaitForSingleObject(g_refresher, 1000);
    CloseHandle(g_refresher);
    g_refresher = nullptr;
  }
  // Under SEH, as the refresher does: it stops exactly when the context
  // table became inaccessible, and a plain Withdraw then faulted here.
  if (!SafeRefresh(false)) Log("d3d meter: context table not accessible at unload; left as is");
  g_vtblCount = 0;
  if (g_rendererDrawSlot && !g_captured.load()) RestoreDrawSlot();
}

bool PatchVtable(void** vtbl) {
  int n = g_vtblCount.load();
  for (int i = 0; i < n; ++i)
    if (g_vtbl[i] == vtbl) return true;
  if (n >= kMaxVtables) return false;
  int slots[kSlotCount];
  for (int i = 0; i < kSlotCount; ++i) {
    slots[i] = DcsQv_ContextSlot(kSlotNames[i]);
    if (slots[i] < 0) {
      Log("d3d meter: unknown vtable slot %s", kSlotNames[i]);
      return false;
    }
  }
  for (int i = 0; i < kSlotCount; ++i) {
    // Drop registry entries an older payload made for this vtable, so the
    // loader never writes its stale originals over d3d11's live table.
    if (g_api) g_api->restore(&vtbl[slots[i]]);
    g_slot[i] = slots[i];
    g_origs[n][i] = vtbl[slots[i]];
  }
  g_vtbl[n] = vtbl;
  g_vtblCount.store(n + 1, std::memory_order_release);  // originals visible before any hook runs
  Repatch(n);
  if (!g_refresher) {
    g_stopRefresher = false;
    g_refresher = CreateThread(nullptr, 0, qvexit::Thread<Refresher>, nullptr, 0, nullptr);
  }
  Log("d3d meter: patched context vtable %p", vtbl);
  return true;
}

// Address of d3d11.dll's PSSetShaderResources implementation, read from a
// private device: every context vtable of any device points to it.
void* ReferenceImpl() {
  ID3D11Device* dev = nullptr;
  ID3D11DeviceContext* ctx = nullptr;
  D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
  if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &fl, 1, D3D11_SDK_VERSION,
                               &dev, nullptr, &ctx)))
    return nullptr;
  void* impl = (*reinterpret_cast<void***>(ctx))[DcsQv_ContextSlot("PSSetShaderResources")];
  ctx->Release();
  dev->Release();
  return impl;
}

bool LooksLikeContext(void* p, void* impl) {
  __try {
    if (!p || reinterpret_cast<uintptr_t>(p) & 7) return false;
    void** vt = *reinterpret_cast<void***>(p);
    if (!vt || reinterpret_cast<uintptr_t>(vt) & 7) return false;
    return vt[DcsQv_ContextSlot("PSSetShaderResources")] == impl;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

void* ReadPtr(const void* at) {
  __try {
    return *reinterpret_cast<void* const*>(at);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return nullptr;
  }
}

// DCS's immediate context is DX11Renderer+0x30 (dx11backend). The renderer
// is heap allocated with no global pointer, so its virtual draw() is hooked
// once through the exported DX11Renderer vtable: the first draw call hands us
// `this`, the meter is installed on its context and the slot is restored.
constexpr size_t kRendererContext = 0x30;
using RendererDrawFn = void(__fastcall*)(void* self, int, void*, int, int, int, int, const char*);
RendererDrawFn g_rendererDraw = nullptr;
void** g_rendererDrawSlot = nullptr;
void* g_impl = nullptr;
std::atomic<bool> g_captured{false};

void RestoreDrawSlot() { UnhookSlot(g_rendererDrawSlot, reinterpret_cast<void*>(g_rendererDraw)); }

// Names the module an address belongs to ("heap" otherwise), for diagnostics.
void DescribeAddress(void* p, char* out, size_t n) {
  HMODULE m = nullptr;
  if (p && GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                              static_cast<LPCWSTR>(p), &m)) {
    char name[MAX_PATH] = {};
    GetModuleFileNameA(m, name, MAX_PATH);
    const char* base = strrchr(name, '\\');
    snprintf(out, n, "%s+0x%llx", base ? base + 1 : name,
             static_cast<unsigned long long>(static_cast<uint8_t*>(p) - reinterpret_cast<uint8_t*>(m)));
  } else {
    snprintf(out, n, "%p (no module)", p);
  }
}

void LogCodeBytes(const char* what, void* p) {
  uint8_t bytes[16] = {};
  __try {
    memcpy(bytes, p, sizeof(bytes));
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return;
  }
  char hex[64] = {};
  for (int i = 0; i < 16; ++i) snprintf(hex + i * 3, 4, "%02x ", bytes[i]);
  Log("d3d diag:   %s code %s", what, hex);
}

// Logs where DCS's context object and its methods live, to see which layer
// (if any) sits between DCS and d3d11.dll.
void DiagnoseContext(void* ctx) {
  char buf[MAX_PATH + 32];
  void** vt = static_cast<void**>(ReadPtr(ctx));
  DescribeAddress(vt, buf, sizeof(buf));
  Log("d3d diag: context %p vtable %p = %s", ctx, vt, buf);
  DescribeAddress(g_impl, buf, sizeof(buf));
  Log("d3d diag: reference d3d11 PSSetShaderResources = %s", buf);
  const char* names[] = {"PSSetShaderResources", "PSSetShader", "DrawIndexed", "DrawIndexedInstanced",
                         "OMSetBlendState", "IASetVertexBuffers"};
  for (const char* nm : names) {
    void* fn = vt ? ReadPtr(&vt[DcsQv_ContextSlot(nm)]) : nullptr;
    DescribeAddress(fn, buf, sizeof(buf));
    Log("d3d diag:   %-22s -> %s", nm, buf);
    LogCodeBytes(nm, fn);
  }
  // Interfaces reachable via QueryInterface: a wrapper usually returns
  // itself, the real object would differ.
  auto* unk = static_cast<IUnknown*>(ctx);
  ID3D11DeviceContext* base = nullptr;
  if (SUCCEEDED(unk->QueryInterface(__uuidof(ID3D11DeviceContext), reinterpret_cast<void**>(&base)))) {
    void** bvt = static_cast<void**>(ReadPtr(base));
    DescribeAddress(bvt ? ReadPtr(&bvt[DcsQv_ContextSlot("PSSetShaderResources")]) : nullptr, buf, sizeof(buf));
    Log("d3d diag: QI(ID3D11DeviceContext) %p, PSSetShaderResources -> %s", base, buf);
    ID3D11Device* dev = nullptr;
    base->GetDevice(&dev);
    if (dev) {
      ID3D11DeviceContext* imm = nullptr;
      dev->GetImmediateContext(&imm);
      if (imm) {
        void** ivt = static_cast<void**>(ReadPtr(imm));
        DescribeAddress(ivt ? ReadPtr(&ivt[DcsQv_ContextSlot("PSSetShaderResources")]) : nullptr, buf,
                        sizeof(buf));
        Log("d3d diag: device %p immediate context %p, PSSetShaderResources -> %s", dev, imm, buf);
        imm->Release();
      }
      dev->Release();
    }
    base->Release();
  }
}

void __fastcall CaptureDraw(void* self, int a, void* b, int c, int d, int e, int f, const char* g) {
  if (!g_captured.exchange(true)) {
    void* ctx = ReadPtr(static_cast<uint8_t*>(self) + kRendererContext);
    if (ctx) DiagnoseContext(ctx);
    if (LooksLikeContext(ctx, g_impl)) {
      Log("d3d meter: DCS context %p (DX11Renderer %p)", ctx, self);
      PatchVtable(*reinterpret_cast<void***>(ctx));
    } else {
      Log("d3d meter: DX11Renderer+0x30 is not a D3D11 context; meter disabled");
    }
    RestoreDrawSlot();
  }
  g_rendererDraw(self, a, b, c, d, e, f, g);
}

bool Install() {
  g_impl = ReferenceImpl();
  if (!g_impl) {
    Log("d3d meter: could not create a reference device");
    return false;
  }
  HMODULE dx = GetModuleHandleW(L"dx11backend.dll");
  if (!dx) return false;
  auto vtbl = reinterpret_cast<void**>(GetProcAddress(dx, "??_7DX11Renderer@RenderAPI@@6B@"));
  void* draw = reinterpret_cast<void*>(GetProcAddress(
      dx, "?draw@DX11Renderer@RenderAPI@@UEAAXHPEAUIShader@2@W4PRIMTYPE_ENUM@render@@HHHPEBD@Z"));
  if (!vtbl || !draw) {
    Log("d3d meter: DX11Renderer exports not found");
    return false;
  }
  for (int i = 0; i < 256 && !g_rendererDrawSlot; ++i)
    if (SlotOriginal(&vtbl[i]) == draw) g_rendererDrawSlot = &vtbl[i];
  if (!g_rendererDrawSlot) {
    Log("d3d meter: draw not found in DX11Renderer vtable");
    return false;
  }
  g_rendererDraw = reinterpret_cast<RendererDrawFn>(draw);
  if (!HookSlot(g_rendererDrawSlot, reinterpret_cast<void*>(&CaptureDraw), nullptr)) return false;
  Log("d3d meter: waiting for the first DX11Renderer::draw to find DCS's context");
  return true;
}

// Test helper: hooks the vtable of an arbitrary context.
bool InstallFor(void* ctx) { return PatchVtable(*reinterpret_cast<void***>(ctx)); }

struct Totals {
  uint64_t calls[kOpCount] = {};
  uint64_t redundant[kOpCount] = {};
  uint64_t mismatch[kOpCount] = {};
  uint64_t draws = 0;
  uint64_t resets = 0;
  uint64_t rewrites = 0;
  uint64_t switches = 0;
};

Totals Snapshot() {
  Totals t;
  t.rewrites = g_rewrites.load();
  t.switches = g_variantSwitches.load();
  std::lock_guard<std::mutex> lock(g_regMutex);
  for (Counters* c : g_counters) {
    for (int i = 0; i < kOpCount; ++i) {
      t.calls[i] += c->calls[i];
      t.redundant[i] += c->redundant[i];
      t.mismatch[i] += c->mismatch[i];
    }
    t.draws += c->draws;
    t.resets += c->resets;
  }
  return t;
}

// Logs per-frame call counts and redundancy between two snapshots.
uint64_t Mismatches(const Totals& a, const Totals& b) {
  uint64_t m = 0;
  for (int i = 0; i < kOpCount; ++i) m += b.mismatch[i] - a.mismatch[i];
  return m;
}

void Report(const Totals& a, const Totals& b, uint64_t frames) {
  if (!frames) return;
  double f = static_cast<double>(frames);
  uint64_t calls = 0, red = 0;
  for (int i = 0; i < kOpCount; ++i) {
    calls += b.calls[i] - a.calls[i];
    red += b.redundant[i] - a.redundant[i];
  }
  Log("  d3d11 table rewrites/frame %.2f (implementation switches %llu), state resets/frame %.2f",
      (b.rewrites - a.rewrites) / f, static_cast<unsigned long long>(b.switches - a.switches),
      (b.resets - a.resets) / f);
  {
    std::lock_guard<std::mutex> lock(g_sampleMutex);
    for (auto& l : g_samples) Log("  %s", l.c_str());
    g_samples.clear();
  }
  Log("  draws/frame %.0f, state calls/frame %.0f, redundant %.1f%% (%.0f/frame)", (b.draws - a.draws) / f,
      calls / f, calls ? 100.0 * red / calls : 0.0, red / f);
  for (int i = 0; i < kOpCount; ++i) {
    uint64_t c = b.calls[i] - a.calls[i], r = b.redundant[i] - a.redundant[i];
    if (!c) continue;
    uint64_t m = b.mismatch[i] - a.mismatch[i];
    Log("    %-8s calls/frame %7.0f  redundant %5.1f%%  verify mismatches %llu", kOpNames[i], c / f,
        100.0 * r / c, static_cast<unsigned long long>(m));
  }
}

}  // namespace d3ds
