// Deferred-context recording infrastructure for R15 A1 (our own shadow
// cascade recorder). DCS-independent: nothing here hooks or reads DCS; the
// recorder that replaces the cascade pass body is built on top of it.
//
//   defrec::Caps        what the device supports (command lists, CB offsets,
//                       NO_OVERWRITE on dynamic CBs, ID3D11DeviceContext1).
//   defrec::PassState   POD snapshot of the pipeline state a recorded pass
//                       starts from, captured once on the immediate context
//                       (Get*; every returned reference is held and released
//                       by Release) and applied to a deferred context at job
//                       start (a deferred context starts every command list
//                       from the default state; we finish with FALSE).
//   defrec::CbRing      per-deferred-context dynamic constant-buffer
//                       allocator (deferred contexts only allow WRITE_DISCARD,
//                       or NO_OVERWRITE after a DISCARD of the same resource
//                       in the same command list, on 11.1 runtimes that
//                       report MapNoOverwriteOnDynamicConstantBuffer).
//   defrec::Pool        N persistent worker threads, each owning one deferred
//                       context and one CbRing. One job per worker per frame;
//                       the job runs under SEH; a fault latches the pool off
//                       for the session and the caller draws stock.
//
// Frame protocol (render thread):
//   PassState s; Capture(imm, &s);                 // per cascade if DSV differs
//   pool.Submit(w, fn, user, &s);                  // per worker
//   ... other render-thread work ...
//   if (pool.Wait(ms)) for w: imm->ExecuteCommandList(pool.TakeList(w), TRUE), Release
//   else: draw stock for the workers whose Status != kOk (their lists are dropped)
//   Release(s);   // only once !pool.AnyBusy() (a timed-out job may still read it)
//
// ExecuteCommandList(list, TRUE) saves the immediate context's state before
// the list and restores it afterwards (documented D3D11 behaviour; verified
// with Get* by test/deferred_rec_test.h, including *SetConstantBuffers1
// offsets), so DCS's own state caches stay valid. With FALSE the immediate
// context is left in the default (cleared) state instead.
//
// Lifetime rules: `user` data and the PassState passed to Submit must stay
// valid until the worker is idle again (Wait returned true, or !Busy(w)). A
// job that timed out keeps running; its list is dropped at the next Submit to
// that worker (refused while it still runs). Stop() joins the threads (each
// finishes its current job) and releases the contexts; if a thread does not
// exit in time its objects are left alive and Stop returns false (the payload
// must then not be unloaded, as in shadow_batch.h).
//
// Self-contained (system headers only) except for Log(const char*, ...),
// which must be declared before inclusion. Included from main.cpp at global
// scope; header-only, C++17.
#pragma once

#include <windows.h>
#include <d3d11_1.h>
#include <intrin.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace defrec {

// ---------------------------------------------------------------------------
// Capabilities
// ---------------------------------------------------------------------------
struct Caps {
  bool commandLists = false;       // THREADING.DriverCommandLists (else the runtime emulates)
  bool concurrentCreates = false;  // THREADING.DriverConcurrentCreates
  bool cbOffsetting = false;       // D3D11_OPTIONS.ConstantBufferOffsetting (*SetConstantBuffers1)
  bool noOverwriteCb = false;      // D3D11_OPTIONS.MapNoOverwriteOnDynamicConstantBuffer
  bool device1 = false;            // ID3D11Device1 / ID3D11DeviceContext1 available
};

inline Caps QueryCaps(ID3D11Device* dev) {
  Caps c;
  D3D11_FEATURE_DATA_THREADING th{};
  if (SUCCEEDED(dev->CheckFeatureSupport(D3D11_FEATURE_THREADING, &th, sizeof(th)))) {
    c.commandLists = th.DriverCommandLists != FALSE;
    c.concurrentCreates = th.DriverConcurrentCreates != FALSE;
  }
  D3D11_FEATURE_DATA_D3D11_OPTIONS o{};
  if (SUCCEEDED(dev->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS, &o, sizeof(o)))) {
    c.cbOffsetting = o.ConstantBufferOffsetting != FALSE;
    c.noOverwriteCb = o.MapNoOverwriteOnDynamicConstantBuffer != FALSE;
  }
  ID3D11Device1* d1 = nullptr;
  if (SUCCEEDED(dev->QueryInterface(__uuidof(ID3D11Device1), reinterpret_cast<void**>(&d1))) && d1) {
    c.device1 = true;
    d1->Release();
  }
  if (!c.device1) c.cbOffsetting = c.noOverwriteCb = false;
  return c;
}

// Why the recorders cannot run on this device, for the "unavailable" log
// line: each capability separately plus DCS's adapter (name, vendor, LUID).
// DriverCommandLists=0 on an NVIDIA adapter that supports them is almost
// always NVIDIA Control Panel "Threaded optimization" = Off for DCS: the
// driver then reports no command-list support to the app. The text lives in a
// static buffer (install runs once at a time).
inline const char* DescribeCapsFailure(ID3D11Device* dev, const Caps& c) {
  static char buf[512];
  char adapter[200] = "unknown adapter";
  UINT vendor = 0;
  IDXGIDevice* xd = nullptr;
  if (dev && SUCCEEDED(dev->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&xd))) && xd) {
    IDXGIAdapter* ad = nullptr;
    DXGI_ADAPTER_DESC d{};
    if (SUCCEEDED(xd->GetAdapter(&ad)) && ad && SUCCEEDED(ad->GetDesc(&d))) {
      char name[128] = {};
      WideCharToMultiByte(CP_UTF8, 0, d.Description, -1, name, sizeof(name) - 1, nullptr, nullptr);
      vendor = d.VendorId;
      snprintf(adapter, sizeof(adapter), "%s (vendor 0x%04x, LUID %08lx:%08lx)", name, vendor,
               static_cast<unsigned long>(d.AdapterLuid.HighPart), d.AdapterLuid.LowPart);
    }
    if (ad) ad->Release();
    xd->Release();
  }
  // A device created with PREVENT_INTERNAL_THREADING_OPTIMIZATIONS (by DCS or
  // by a layer/injector that creates or wraps it) also reports no command lists.
  const UINT flags = dev ? dev->GetCreationFlags() : 0;
  const char* hint = "";
  if (!c.commandLists && (flags & D3D11_CREATE_DEVICE_PREVENT_INTERNAL_THREADING_OPTIMIZATIONS))
    hint = " -- the device was created with PREVENT_INTERNAL_THREADING_OPTIMIZATIONS (check injectors/overlays)";
  else if (!c.commandLists && vendor == 0x10de)
    hint = " -- NVIDIA Control Panel for DCS: Threaded optimization Auto or On, and Antialiasing - MFAA Off (MFAA On "
           "turns the driver's command lists off)";
  else if (!c.commandLists && vendor != 0 && vendor != 0x10de)
    hint = " -- this GPU's driver does not report native command lists";
  snprintf(buf, sizeof(buf),
           "the D3D11 device lacks what the recorder needs: DriverCommandLists=%d ConstantBufferOffsetting=%d "
           "ID3D11DeviceContext1=%d creation flags 0x%x on %s%s",
           c.commandLists ? 1 : 0, c.cbOffsetting ? 1 : 0, c.device1 ? 1 : 0, flags, adapter, hint);
  return buf;
}

inline int64_t Qpc() {
  LARGE_INTEGER t;
  QueryPerformanceCounter(&t);
  return t.QuadPart;
}
inline double QpcToUs() {
  static double k = [] {
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    return 1e6 / static_cast<double>(f.QuadPart);
  }();
  return k;
}

// ---------------------------------------------------------------------------
// PassState
// ---------------------------------------------------------------------------
constexpr UINT kCbSlots = D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT;  // 14
constexpr UINT kSrvSlots = D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT;     // 128
constexpr UINT kSampSlots = D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT;           // 16
constexpr UINT kVbSlots = D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT;         // 32
constexpr UINT kRtvSlots = D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT;           // 8
constexpr UINT kVpSlots = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;  // 16

// What Capture saw that Apply does not reproduce (the pass would differ).
enum Unsupported : uint32_t {
  kUnClassInstances = 1u << 0,  // VS/PS bound with class instances
  kUnOtherStages = 1u << 1,     // GS/HS/DS bound (bound shaders are applied, their resources are not)
  kUnUav = 1u << 2,             // OM UAVs bound
  kUnPredication = 1u << 3,     // predicate set
  kUnStreamOut = 1u << 4,       // SO targets bound
};

struct StageState {
  ID3D11Buffer* cb[kCbSlots];
  UINT cbFirst[kCbSlots];  // in 16-byte constants, as *GetConstantBuffers1 returns them
  UINT cbNum[kCbSlots];
  uint32_t cbRanged;       // bit per slot: bound with an offset/size narrower than the buffer
  UINT cbCount, srvCount, sampCount;  // highest non-null slot + 1
  ID3D11ShaderResourceView* srv[kSrvSlots];
  ID3D11SamplerState* samp[kSampSlots];
};

// Zero-initialised POD; every non-null pointer holds one reference (Release).
struct PassState {
  bool valid;
  uint32_t unsupported;  // Unsupported bits
  ID3D11RenderTargetView* rtv[kRtvSlots];
  UINT rtvCount;
  ID3D11DepthStencilView* dsv;
  D3D11_VIEWPORT vp[kVpSlots];
  UINT vpCount;
  D3D11_RECT sc[kVpSlots];
  UINT scCount;
  ID3D11RasterizerState* rs;
  ID3D11DepthStencilState* dss;
  UINT stencilRef;
  ID3D11BlendState* bs;
  FLOAT blendFactor[4];
  UINT sampleMask;
  D3D11_PRIMITIVE_TOPOLOGY topo;
  ID3D11InputLayout* il;
  ID3D11Buffer* vb[kVbSlots];
  UINT vbStride[kVbSlots];
  UINT vbOffset[kVbSlots];
  UINT vbCount;
  ID3D11Buffer* ib;
  DXGI_FORMAT ibFormat;
  UINT ibOffset;
  ID3D11VertexShader* vs;
  ID3D11PixelShader* ps;
  ID3D11GeometryShader* gs;
  ID3D11HullShader* hs;
  ID3D11DomainShader* ds;
  StageState vsStage;
  StageState psStage;
};

template <class T>
inline void SafeRelease(T*& p) {
  if (p) p->Release();
  p = nullptr;
}

template <class T>
inline UINT CountUsed(T* const* a, UINT n) {
  UINT k = n;
  while (k > 0 && !a[k - 1]) --k;
  return k;
}

inline void Release(PassState& s) {
  for (auto*& p : s.rtv) SafeRelease(p);
  SafeRelease(s.dsv);
  SafeRelease(s.rs);
  SafeRelease(s.dss);
  SafeRelease(s.bs);
  SafeRelease(s.il);
  for (auto*& p : s.vb) SafeRelease(p);
  SafeRelease(s.ib);
  SafeRelease(s.vs);
  SafeRelease(s.ps);
  SafeRelease(s.gs);
  SafeRelease(s.hs);
  SafeRelease(s.ds);
  for (StageState* st : {&s.vsStage, &s.psStage}) {
    for (auto*& p : st->cb) SafeRelease(p);
    for (auto*& p : st->srv) SafeRelease(p);
    for (auto*& p : st->samp) SafeRelease(p);
  }
  memset(&s, 0, sizeof(s));
}

namespace detail {

inline void FinishCbs(StageState& st) {
  st.cbRanged = 0;
  for (UINT i = 0; i < kCbSlots; ++i) {
    if (!st.cb[i]) {
      st.cbFirst[i] = st.cbNum[i] = 0;
      continue;
    }
    D3D11_BUFFER_DESC d;
    st.cb[i]->GetDesc(&d);
    // Plain binding: offset 0 and a window covering the buffer (the runtime
    // reports 0/0 or 0/4096 for buffers bound without offsets).
    const bool ranged = st.cbFirst[i] != 0 || (st.cbNum[i] != 0 && st.cbNum[i] * 16u < d.ByteWidth);
    if (ranged) st.cbRanged |= 1u << i;
  }
  st.cbCount = CountUsed(st.cb, kCbSlots);
  st.srvCount = CountUsed(st.srv, kSrvSlots);
  st.sampCount = CountUsed(st.samp, kSampSlots);
}

template <class Shader>
inline uint32_t ReleaseInstances(ID3D11ClassInstance** inst, UINT n) {
  for (UINT i = 0; i < n; ++i) SafeRelease(inst[i]);
  return n ? kUnClassInstances : 0;
}

}  // namespace detail

// Snapshots the state of `c` (immediate or deferred). Any previous contents
// of `s` are released first.
inline void Capture(ID3D11DeviceContext* c, PassState* s) {
  Release(*s);
  ID3D11DeviceContext1* c1 = nullptr;
  c->QueryInterface(__uuidof(ID3D11DeviceContext1), reinterpret_cast<void**>(&c1));

  ID3D11UnorderedAccessView* uav[D3D11_1_UAV_SLOT_COUNT] = {};
  c->OMGetRenderTargetsAndUnorderedAccessViews(kRtvSlots, s->rtv, &s->dsv, 0, D3D11_1_UAV_SLOT_COUNT, uav);
  for (auto*& u : uav)
    if (u) {
      s->unsupported |= kUnUav;
      SafeRelease(u);
    }
  s->rtvCount = CountUsed(s->rtv, kRtvSlots);
  UINT n = kVpSlots;
  c->RSGetViewports(&n, s->vp);
  s->vpCount = n;
  n = kVpSlots;
  c->RSGetScissorRects(&n, s->sc);
  s->scCount = n;
  c->RSGetState(&s->rs);
  c->OMGetDepthStencilState(&s->dss, &s->stencilRef);
  c->OMGetBlendState(&s->bs, s->blendFactor, &s->sampleMask);
  c->IAGetPrimitiveTopology(&s->topo);
  c->IAGetInputLayout(&s->il);
  c->IAGetVertexBuffers(0, kVbSlots, s->vb, s->vbStride, s->vbOffset);
  s->vbCount = CountUsed(s->vb, kVbSlots);
  c->IAGetIndexBuffer(&s->ib, &s->ibFormat, &s->ibOffset);

  ID3D11ClassInstance* inst[D3D11_SHADER_MAX_INTERFACES] = {};
  UINT ni = D3D11_SHADER_MAX_INTERFACES;
  c->VSGetShader(&s->vs, inst, &ni);
  s->unsupported |= detail::ReleaseInstances<ID3D11VertexShader>(inst, ni);
  ni = D3D11_SHADER_MAX_INTERFACES;
  c->PSGetShader(&s->ps, inst, &ni);
  s->unsupported |= detail::ReleaseInstances<ID3D11PixelShader>(inst, ni);
  ni = D3D11_SHADER_MAX_INTERFACES;
  c->GSGetShader(&s->gs, inst, &ni);
  detail::ReleaseInstances<ID3D11GeometryShader>(inst, ni);
  ni = D3D11_SHADER_MAX_INTERFACES;
  c->HSGetShader(&s->hs, inst, &ni);
  detail::ReleaseInstances<ID3D11HullShader>(inst, ni);
  ni = D3D11_SHADER_MAX_INTERFACES;
  c->DSGetShader(&s->ds, inst, &ni);
  detail::ReleaseInstances<ID3D11DomainShader>(inst, ni);
  if (s->gs || s->hs || s->ds) s->unsupported |= kUnOtherStages;

  if (c1) {
    c1->VSGetConstantBuffers1(0, kCbSlots, s->vsStage.cb, s->vsStage.cbFirst, s->vsStage.cbNum);
    c1->PSGetConstantBuffers1(0, kCbSlots, s->psStage.cb, s->psStage.cbFirst, s->psStage.cbNum);
  } else {
    c->VSGetConstantBuffers(0, kCbSlots, s->vsStage.cb);
    c->PSGetConstantBuffers(0, kCbSlots, s->psStage.cb);
  }
  c->VSGetShaderResources(0, kSrvSlots, s->vsStage.srv);
  c->PSGetShaderResources(0, kSrvSlots, s->psStage.srv);
  c->VSGetSamplers(0, kSampSlots, s->vsStage.samp);
  c->PSGetSamplers(0, kSampSlots, s->psStage.samp);
  detail::FinishCbs(s->vsStage);
  detail::FinishCbs(s->psStage);

  ID3D11Predicate* pred = nullptr;
  BOOL predValue = FALSE;
  c->GetPredication(&pred, &predValue);
  if (pred) {
    s->unsupported |= kUnPredication;
    pred->Release();
  }
  ID3D11Buffer* so[D3D11_SO_BUFFER_SLOT_COUNT] = {};
  c->SOGetTargets(D3D11_SO_BUFFER_SLOT_COUNT, so);
  for (auto*& b : so)
    if (b) {
      s->unsupported |= kUnStreamOut;
      SafeRelease(b);
    }
  if (c1) c1->Release();
  s->valid = true;
}

namespace detail {
template <class SetCb, class SetCb1>
inline void ApplyCbs(const StageState& st, SetCb set, SetCb1 set1, bool haveC1) {
  if (!st.cbCount) return;
  set(0, st.cbCount, st.cb);
  if (!st.cbRanged || !haveC1) return;
  for (UINT i = 0; i < st.cbCount; ++i)
    if (st.cbRanged & (1u << i)) set1(i, 1, &st.cb[i], &st.cbFirst[i], &st.cbNum[i]);
}
}  // namespace detail

// Sets the whole PassState on `c` (a deferred context at the start of a
// command list, which is in the default state: null slots above the captured
// counts are already null). `c1` may be null (no ranged CB bindings then).
inline void Apply(ID3D11DeviceContext* c, ID3D11DeviceContext1* c1, const PassState& s) {
  c->OMSetRenderTargets(s.rtvCount, s.rtvCount ? s.rtv : nullptr, s.dsv);
  c->RSSetViewports(s.vpCount, s.vp);
  c->RSSetScissorRects(s.scCount, s.sc);
  c->RSSetState(s.rs);
  c->OMSetDepthStencilState(s.dss, s.stencilRef);
  c->OMSetBlendState(s.bs, s.blendFactor, s.sampleMask);
  c->IASetPrimitiveTopology(s.topo);
  c->IASetInputLayout(s.il);
  if (s.vbCount) c->IASetVertexBuffers(0, s.vbCount, s.vb, s.vbStride, s.vbOffset);
  c->IASetIndexBuffer(s.ib, s.ibFormat, s.ibOffset);
  c->VSSetShader(s.vs, nullptr, 0);
  c->PSSetShader(s.ps, nullptr, 0);
  if (s.gs) c->GSSetShader(s.gs, nullptr, 0);
  if (s.hs) c->HSSetShader(s.hs, nullptr, 0);
  if (s.ds) c->DSSetShader(s.ds, nullptr, 0);
  detail::ApplyCbs(
      s.vsStage, [&](UINT a, UINT n, ID3D11Buffer* const* b) { c->VSSetConstantBuffers(a, n, b); },
      [&](UINT a, UINT n, ID3D11Buffer* const* b, const UINT* f, const UINT* k) {
        c1->VSSetConstantBuffers1(a, n, b, f, k);
      },
      c1 != nullptr);
  detail::ApplyCbs(
      s.psStage, [&](UINT a, UINT n, ID3D11Buffer* const* b) { c->PSSetConstantBuffers(a, n, b); },
      [&](UINT a, UINT n, ID3D11Buffer* const* b, const UINT* f, const UINT* k) {
        c1->PSSetConstantBuffers1(a, n, b, f, k);
      },
      c1 != nullptr);
  if (s.vsStage.srvCount) c->VSSetShaderResources(0, s.vsStage.srvCount, s.vsStage.srv);
  if (s.psStage.srvCount) c->PSSetShaderResources(0, s.psStage.srvCount, s.psStage.srv);
  if (s.vsStage.sampCount) c->VSSetSamplers(0, s.vsStage.sampCount, s.vsStage.samp);
  if (s.psStage.sampCount) c->PSSetSamplers(0, s.psStage.sampCount, s.psStage.samp);
}

// True when both snapshots describe the same state (same objects, same values).
inline bool Equal(const PassState& a, const PassState& b) { return memcmp(&a, &b, sizeof(PassState)) == 0; }

// ---------------------------------------------------------------------------
// Constant-buffer ring (one per deferred context)
// ---------------------------------------------------------------------------
enum class CbMode : int {
  kAuto = 0,     // kDefaultMode when the device supports it, else kDiscard
  kOffsets = 1,  // large dynamic chunks, 256-byte windows bound with *SetConstantBuffers1;
                 // first map of a chunk in a list DISCARD, later maps NO_OVERWRITE
                 // (or a fresh DISCARD when unsupported). Allocations made between
                 // two Close() calls share one Map.
  kDiscard = 2,  // small dynamic buffers in power-of-two buckets, each allocation
                 // one Map(DISCARD) (legal any number of times on a deferred context);
                 // at most perBucket allocations per bucket between two Close() calls
};

// Picked by test/deferred_rec_test.h QV_DEFREC_BENCH on the dev PC (NVIDIA,
// DriverCommandLists=1, offsets + NO_OVERWRITE supported; 2026-10-09, DCS
// running). 5,000 draws, one 80-byte VS CB each, record + Finish on one
// deferred context (medians of 2 x 3 runs of 15 rounds):
//   kDiscard per draw 0.24-0.27 ms, kOffsets per draw (NO_OVERWRITE) 0.26-0.27
//   ms, kOffsets batched (all CBs, one Close, then the draws) 0.15-0.16 ms.
// Pool end-to-end (4 x 5,000): discard 0.48-0.51 ms, offsets/draw 0.41-0.47,
// offsets batched 0.33-0.38. kOffsets is never slower and enables the batched
// pattern; kDiscard stays the fallback without ConstantBufferOffsetting.
constexpr CbMode kDefaultMode = CbMode::kOffsets;

inline const char* ModeName(CbMode m) {
  return m == CbMode::kOffsets ? "offsets" : m == CbMode::kDiscard ? "discard" : "auto";
}

struct CbConfig {
  CbMode mode = CbMode::kAuto;
  UINT chunkBytes = 256u << 10;  // kOffsets: chunk size (a bound window is at most 64 KB)
  UINT maxChunks = 64;           // kOffsets: per ring (16 MB at the default size)
  UINT minBucket = 16;           // kDiscard: smallest bucket (multiple of 16)
  UINT maxBucket = 4096;         // kDiscard: largest bucket
  UINT perBucket = 8;            // kDiscard: buffers per bucket (<= 32)
};

// A constant-buffer allocation. Bind with CbRing::BindVS/BindPS. `num` == 0
// means the whole (small) buffer is bound with the plain *SetConstantBuffers.
struct CbSlice {
  ID3D11Buffer* buf = nullptr;
  uint8_t* ptr = nullptr;  // write here until Close()
  UINT first = 0;          // in 16-byte constants (multiple of 16)
  UINT num = 0;            // in 16-byte constants (multiple of 16)
  explicit operator bool() const { return buf != nullptr; }
};

class CbRing {
 public:
  static constexpr int kMaxBuckets = 12;

  bool Init(ID3D11Device* dev, ID3D11DeviceContext* dc, const Caps& caps, const CbConfig& cfg) {
    dev_ = dev;
    dc_ = dc;
    cfg_ = cfg;
    if (cfg_.perBucket < 1) cfg_.perBucket = 1;
    if (cfg_.perBucket > 32) cfg_.perBucket = 32;
    if (cfg_.chunkBytes < 65536) cfg_.chunkBytes = 65536;
    cfg_.chunkBytes &= ~255u;
    if (cfg_.maxChunks > kMaxChunks) cfg_.maxChunks = kMaxChunks;
    noOverwrite_ = caps.noOverwriteCb;
    dc->QueryInterface(__uuidof(ID3D11DeviceContext1), reinterpret_cast<void**>(&dc1_));
    mode_ = cfg.mode;
    if (mode_ == CbMode::kAuto) mode_ = kDefaultMode;
    if (mode_ == CbMode::kOffsets && (!caps.cbOffsetting || !dc1_)) mode_ = CbMode::kDiscard;
    buckets_ = 0;
    for (UINT b = cfg_.minBucket < 16 ? 16 : (cfg_.minBucket + 15) & ~15u; b <= cfg_.maxBucket && buckets_ < kMaxBuckets;
         b *= 2)
      bucketSize_[buckets_++] = b;
    Reset();
    return true;
  }

  void Release() {
    Close();
    for (auto*& c : chunks_) SafeRelease(c);
    for (auto& bk : bufs_)
      for (auto*& b : bk) SafeRelease(b);
    SafeRelease(dc1_);
    dc_ = nullptr;
    dev_ = nullptr;
  }

  CbMode Mode() const { return mode_; }

  // Start of a new command list on the ring's context (nothing mapped yet).
  void Reset() {
    cur_ = -1;
    off_ = 0;
    chunkOpen_ = false;
    chunkMappedInList_ = false;
    memset(rr_, 0, sizeof(rr_));
    memset(open_, 0, sizeof(open_));
  }

  // `bytes` must cover the shader's whole cbuffer (the bound window/buffer is
  // at least that large; bytes past `bytes` are unspecified).
  CbSlice Alloc(UINT bytes) {
    return mode_ == CbMode::kOffsets ? AllocOffsets(bytes) : AllocDiscard(bytes);
  }
  CbSlice Upload(const void* data, UINT bytes) {
    CbSlice s = Alloc(bytes);
    if (s) memcpy(s.ptr, data, bytes);
    return s;
  }

  // Unmaps everything Alloc mapped. Required before a draw reads any of it.
  void Close() {
    if (chunkOpen_) {
      dc_->Unmap(chunks_[cur_], 0);
      chunkOpen_ = false;
    }
    for (int b = 0; b < buckets_; ++b) {
      uint32_t m = open_[b];
      while (m) {
        unsigned long i;
        _BitScanForward(&i, m);
        m &= m - 1;
        dc_->Unmap(bufs_[b][i], 0);
      }
      open_[b] = 0;
    }
  }

  void BindVS(UINT slot, const CbSlice& s) {
    if (s.num)
      dc1_->VSSetConstantBuffers1(slot, 1, &s.buf, &s.first, &s.num);
    else
      dc_->VSSetConstantBuffers(slot, 1, &s.buf);
  }
  void BindPS(UINT slot, const CbSlice& s) {
    if (s.num)
      dc1_->PSSetConstantBuffers1(slot, 1, &s.buf, &s.first, &s.num);
    else
      dc_->PSSetConstantBuffers(slot, 1, &s.buf);
  }

  uint64_t maps = 0, discards = 0, overflows = 0, bytesUsed = 0, created = 0;
  uint64_t createdBytes = 0;  // ByteWidth summed over the buffers created (vram_count.h)

 private:
  static constexpr int kMaxChunks = 256;

  CbSlice AllocOffsets(UINT bytes) {
    const UINT size = (bytes + 255u) & ~255u;
    if (!size || size > 65536u) return Overflow();
    if (cur_ < 0 || off_ + size > cfg_.chunkBytes) {
      if (chunkOpen_) {
        dc_->Unmap(chunks_[cur_], 0);
        chunkOpen_ = false;
      }
      if (cur_ + 1 >= static_cast<int>(cfg_.maxChunks)) return Overflow();
      ++cur_;
      if (!chunks_[cur_] && !Create(cfg_.chunkBytes, &chunks_[cur_])) return Overflow();
      off_ = 0;
      chunkMappedInList_ = false;
    }
    if (!chunkOpen_) {
      D3D11_MAP type = D3D11_MAP_WRITE_DISCARD;
      if (chunkMappedInList_ && noOverwrite_)
        type = D3D11_MAP_WRITE_NO_OVERWRITE;
      else
        off_ = 0;  // a fresh DISCARD renames the chunk: earlier draws keep the old contents
      D3D11_MAPPED_SUBRESOURCE m;
      if (FAILED(dc_->Map(chunks_[cur_], 0, type, 0, &m))) return Overflow();
      ++maps;
      if (type == D3D11_MAP_WRITE_DISCARD) ++discards;
      chunkBase_ = static_cast<uint8_t*>(m.pData);
      chunkOpen_ = true;
      chunkMappedInList_ = true;
    }
    CbSlice s;
    s.buf = chunks_[cur_];
    s.ptr = chunkBase_ + off_;
    s.first = off_ / 16;
    s.num = size / 16;
    off_ += size;
    bytesUsed += size;
    return s;
  }

  CbSlice AllocDiscard(UINT bytes) {
    int b = 0;
    while (b < buckets_ && bucketSize_[b] < bytes) ++b;
    if (b == buckets_ || bytes == 0) return Overflow();
    const UINT i = rr_[b];
    rr_[b] = (i + 1) % cfg_.perBucket;
    if (open_[b] & (1u << i)) return Overflow();  // more live allocations than perBucket before Close()
    if (!bufs_[b][i] && !Create(bucketSize_[b], &bufs_[b][i])) return Overflow();
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(dc_->Map(bufs_[b][i], 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) return Overflow();
    ++maps;
    ++discards;
    open_[b] |= 1u << i;
    CbSlice s;
    s.buf = bufs_[b][i];
    s.ptr = static_cast<uint8_t*>(m.pData);
    bytesUsed += bucketSize_[b];
    return s;
  }

  bool Create(UINT size, ID3D11Buffer** out) {
    D3D11_BUFFER_DESC d{};
    d.ByteWidth = size;
    d.Usage = D3D11_USAGE_DYNAMIC;
    d.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    const bool ok = SUCCEEDED(dev_->CreateBuffer(&d, nullptr, out)) && *out;  // free-threaded
    if (ok) {
      ++created;
      createdBytes += size;
    }
    return ok;
  }

  CbSlice Overflow() {
    ++overflows;
    return CbSlice{};
  }

  ID3D11Device* dev_ = nullptr;          // not owned
  ID3D11DeviceContext* dc_ = nullptr;    // not owned
  ID3D11DeviceContext1* dc1_ = nullptr;  // owned (QueryInterface)
  CbConfig cfg_;
  CbMode mode_ = CbMode::kDiscard;
  bool noOverwrite_ = false;
  // kOffsets
  ID3D11Buffer* chunks_[kMaxChunks] = {};
  int cur_ = -1;
  UINT off_ = 0;
  bool chunkOpen_ = false;
  bool chunkMappedInList_ = false;
  uint8_t* chunkBase_ = nullptr;
  // kDiscard
  int buckets_ = 0;
  UINT bucketSize_[kMaxBuckets] = {};
  ID3D11Buffer* bufs_[kMaxBuckets][32] = {};
  UINT rr_[kMaxBuckets] = {};
  uint32_t open_[kMaxBuckets] = {};
};

// ---------------------------------------------------------------------------
// Worker pool
// ---------------------------------------------------------------------------
struct Worker;
// Records into w.dc (w.dc1 may be null, w.ring is reset and the PassState is
// already applied). Returns false to give up (the list is dropped, the caller
// draws stock). Runs under SEH on the worker thread.
using JobFn = bool (*)(Worker& w, void* user);

enum JobState : int { kIdle = 0, kQueued, kRunning, kOk, kFailed, kFaulted };

// Measurement hook ([Suite] RunnableThreads, run_threads.h): called on the
// worker thread at the start of every job while set (null otherwise: one
// relaxed load per job).
inline std::atomic<void (*)()> g_jobStartHook{nullptr};

inline const char* StateName(int s) {
  static const char* const k[] = {"idle", "queued", "running", "ok", "failed", "faulted"};
  return s >= 0 && s <= kFaulted ? k[s] : "?";
}

struct Worker {
  int index = 0;
  ID3D11DeviceContext* dc = nullptr;
  ID3D11DeviceContext1* dc1 = nullptr;
  CbRing ring;
  // Job (written by the submitting thread before the release store of kQueued).
  JobFn fn = nullptr;
  void* user = nullptr;
  const PassState* pass = nullptr;
  // Result (written by the worker before the release store of the final state).
  ID3D11CommandList* list = nullptr;
  DWORD faultCode = 0;
  void* faultAddr = nullptr;
  uintptr_t faultData = 0;         // access violation: the address read or written
  void* faultFrames[8] = {};       // the faulting stack (unwound from the exception context)
  int faultFrameCount = 0;
  double lastRecordUs = 0, lastFinishUs = 0;
  // Stats.
  uint64_t jobs = 0, ok = 0, failed = 0, faults = 0;
  double recordUs = 0, finishUs = 0;

  std::atomic<int> state{kIdle};
  HANDLE thread = nullptr;
  HANDLE wake = nullptr;  // auto-reset
  HANDLE done = nullptr;  // manual-reset; reset at Submit, set when the job ends
  class Pool* pool = nullptr;
};

struct PoolConfig {
  int workers = 4;
  CbConfig cb;
  int priority = THREAD_PRIORITY_HIGHEST;
  DWORD spinUs = 300;     // Wait spins this long before blocking on the done events
  DWORD joinMs = 3000;    // Stop: per-pool join limit
  const char* name = "deferred rec";
};

class Pool {
 public:
  static constexpr int kMaxWorkers = 16;

  // No destructor join: a global Pool must be stopped explicitly (Shutdown),
  // never from DllMain (joining there would wait on the loader lock).

  // `dev` is DCS's device (a reference is held while the pool runs). Fails
  // (and logs) when deferred contexts cannot be created, e.g. a device made
  // with D3D11_CREATE_DEVICE_SINGLETHREADED.
  bool Start(ID3D11Device* dev, const PoolConfig& cfg = PoolConfig()) {
    if (started_) return true;
    cfg_ = cfg;
    if (cfg_.workers < 1) cfg_.workers = 1;
    if (cfg_.workers > kMaxWorkers) cfg_.workers = kMaxWorkers;
    caps_ = QueryCaps(dev);
    dev_ = dev;
    dev_->AddRef();
    stop_ = false;
    disabled_ = false;
    leaked_ = false;
    n_ = 0;
    for (int i = 0; i < cfg_.workers; ++i) {
      Worker& w = w_[i];
      w.index = i;
      w.pool = this;
      HRESULT hr = dev->CreateDeferredContext(0, &w.dc);
      if (FAILED(hr) || !w.dc) {
        Log("%s: CreateDeferredContext failed (0x%08lx); not started", cfg_.name, static_cast<unsigned long>(hr));
        Teardown();
        return false;
      }
      w.dc->QueryInterface(__uuidof(ID3D11DeviceContext1), reinterpret_cast<void**>(&w.dc1));
      w.ring.Init(dev, w.dc, caps_, cfg_.cb);
      w.wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
      w.done = CreateEventW(nullptr, TRUE, TRUE, nullptr);
      w.state.store(kIdle);
      w.thread = (w.wake && w.done) ? CreateThread(nullptr, 0, qvexit::Thread<&Pool::ThreadMain>, &w, 0, nullptr) : nullptr;
      if (!w.thread) {
        Log("%s: worker thread creation failed; not started", cfg_.name);
        ++n_;
        Teardown();
        return false;
      }
      SetThreadPriority(w.thread, cfg_.priority);
      ++n_;
    }
    started_ = true;
    Log("%s: %d workers on deferred contexts (DriverCommandLists=%d, CB mode %s, offsets %d, no-overwrite CB %d)",
        cfg_.name, n_, caps_.commandLists, ModeName(w_[0].ring.Mode()), caps_.cbOffsetting, caps_.noOverwriteCb);
    return true;
  }

  // Joins the workers (each finishes its current job) and releases contexts,
  // lists and buffers. False when a worker did not exit within joinMs: its
  // objects are then left alive (do not unload the code it runs).
  bool Stop() {
    if (!started_) return !leaked_;
    started_ = false;
    return Teardown();
  }

  bool Started() const { return started_; }
  int Workers() const { return n_; }
  // Thread ids of the running workers (at most cap); the count written.
  int ThreadIds(DWORD* out, int cap) const {
    int k = 0;
    for (int i = 0; i < n_ && k < cap; ++i)
      if (w_[i].thread) out[k++] = GetThreadId(w_[i].thread);
    return k;
  }
  const Caps& DeviceCaps() const { return caps_; }
  bool Disabled() const { return disabled_.load(std::memory_order_relaxed); }
  Worker& At(int i) { return w_[i]; }
  int Status(int w) const { return w_[w].state.load(std::memory_order_acquire); }
  bool Busy(int w) const {
    const int s = Status(w);
    return s == kQueued || s == kRunning;
  }
  bool AnyBusy() const {
    for (int i = 0; i < n_; ++i)
      if (Busy(i)) return true;
    return false;
  }

  // Latches the pool off for the session (also done on a worker fault).
  void Disable(const char* why) {
    if (!disabled_.exchange(true)) Log("%s: disabled for this session (%s)", cfg_.name, why);
  }

  // Queues one job on worker `wi`. False (draw stock) when the pool is off,
  // or that worker still runs an earlier (timed-out) job.
  bool Submit(int wi, JobFn fn, void* user, const PassState* pass) {
    if (!started_ || disabled_.load(std::memory_order_relaxed) || wi < 0 || wi >= n_ || !fn) return false;
    Worker& w = w_[wi];
    int s = w.state.load(std::memory_order_acquire);
    if (s == kQueued || s == kRunning) {
      ++refused_;
      return false;
    }
    if (s == kOk) SafeRelease(w.list);  // a late result nobody took
    w.fn = fn;
    w.user = user;
    w.pass = pass;
    w.list = nullptr;
    w.faultCode = 0;
    w.faultAddr = nullptr;
    ResetEvent(w.done);
    w.state.store(kQueued, std::memory_order_release);
    submitted_ |= 1u << wi;
    SetEvent(w.wake);
    return true;
  }

  // Waits for every job submitted since the last Wait. True when all of them
  // recorded successfully (Status kOk); false on a failure, fault or timeout
  // (Status tells which; failed/faulted workers are idle again, timed-out
  // ones keep running).
  bool Wait(DWORD timeoutMs) {
    const uint32_t mask = submitted_;
    submitted_ = 0;
    if (!mask) return true;
    const int64_t t0 = Qpc();
    const double k = QpcToUs();
    auto pending = [&] {
      for (int i = 0; i < n_; ++i)
        if ((mask & (1u << i)) && Busy(i)) return true;
      return false;
    };
    while (pending() && (Qpc() - t0) * k < cfg_.spinUs) _mm_pause();
    if (pending()) {
      HANDLE h[kMaxWorkers];
      DWORD c = 0;
      for (int i = 0; i < n_; ++i)
        if ((mask & (1u << i)) && Busy(i)) h[c++] = w_[i].done;
      const double spent = (Qpc() - t0) * k / 1000.0;
      const DWORD left = spent >= timeoutMs ? 0 : static_cast<DWORD>(timeoutMs - spent);
      WaitForMultipleObjects(c, h, TRUE, left);
    }
    bool all = true;
    for (int i = 0; i < n_; ++i) {
      if (!(mask & (1u << i))) continue;
      const int s = Status(i);
      if (s == kOk) continue;
      all = false;
      if (s == kQueued || s == kRunning) {
        ++timeouts_;
      } else if (s == kFailed || s == kFaulted) {
        w_[i].state.store(kIdle, std::memory_order_release);
      }
    }
    lastWaitUs_ = (Qpc() - t0) * k;
    return all;
  }

  // The command list of a successful job (caller executes and releases it);
  // the worker is idle afterwards. Null when the job did not succeed.
  ID3D11CommandList* TakeList(int wi) {
    Worker& w = w_[wi];
    if (w.state.load(std::memory_order_acquire) != kOk) return nullptr;
    ID3D11CommandList* l = w.list;
    w.list = nullptr;
    w.state.store(kIdle, std::memory_order_release);
    return l;
  }

  // Executes every successful list in worker order on `imm` (restoring its
  // state each time); returns how many were executed.
  int ExecuteAll(ID3D11DeviceContext* imm, BOOL restore = TRUE) {
    int k = 0;
    for (int i = 0; i < n_; ++i) {
      ID3D11CommandList* l = TakeList(i);
      if (!l) continue;
      imm->ExecuteCommandList(l, restore);
      l->Release();
      ++k;
    }
    return k;
  }

  uint64_t timeouts() const { return timeouts_; }
  uint64_t refused() const { return refused_; }
  double lastWaitUs() const { return lastWaitUs_; }

 private:
  static int Filter(EXCEPTION_POINTERS* ep, Worker* w) {
    w->faultCode = ep->ExceptionRecord->ExceptionCode;
    w->faultAddr = ep->ExceptionRecord->ExceptionAddress;
    w->faultData = ep->ExceptionRecord->NumberParameters >= 2 ? ep->ExceptionRecord->ExceptionInformation[1] : 0;
    w->faultFrameCount = UnwindFrames(*ep->ContextRecord, w->faultFrames, 8);
    return EXCEPTION_EXECUTE_HANDLER;
  }

 public:
  // Return addresses from an exception context (x64 unwind data; a frame
  // without unwind data is taken as a leaf). At most n frames.
  static int UnwindFrames(const CONTEXT& from, void** out, int n) {
    CONTEXT ctx = from;  // a local: CONTEXT needs 16-byte alignment
    int k = 0;
    __try {
      for (; k < n && ctx.Rip; ++k) {
        out[k] = reinterpret_cast<void*>(ctx.Rip);
        DWORD64 base = 0;
        PRUNTIME_FUNCTION fe = RtlLookupFunctionEntry(ctx.Rip, &base, nullptr);
        if (!fe) {
          ctx.Rip = *reinterpret_cast<DWORD64*>(ctx.Rsp);
          ctx.Rsp += 8;
        } else {
          void* hd = nullptr;
          DWORD64 est = 0;
          RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, ctx.Rip, fe, &ctx, &hd, &est, nullptr);
        }
      }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return k;
  }
  // "module+0xoffset" (or the raw address).
  static void ModOff(const void* a, char* out, size_t n) {
    HMODULE m = nullptr;
    wchar_t path[MAX_PATH] = {};
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           static_cast<LPCWSTR>(a), &m) &&
        GetModuleFileNameW(m, path, MAX_PATH)) {
      const wchar_t* b = wcsrchr(path, L'\\');
      char name[MAX_PATH] = {};
      WideCharToMultiByte(CP_UTF8, 0, b ? b + 1 : path, -1, name, MAX_PATH, nullptr, nullptr);
      snprintf(out, n, "%s+0x%llx", name,
               static_cast<unsigned long long>(static_cast<const uint8_t*>(a) - reinterpret_cast<uint8_t*>(m)));
    } else {
      snprintf(out, n, "%p", a);
    }
  }

 private:

  // 1 = list recorded, 0 = the job gave up / Finish failed, -1 = fault.
  // Plain function body (no unwinding objects) for __try.
  static int RunGuarded(Worker* w) {
    __try {
      if (w->pass) Apply(w->dc, w->dc1, *w->pass);
      const int64_t t0 = Qpc();
      const bool ok = w->fn(*w, w->user);
      w->ring.Close();
      const int64_t t1 = Qpc();
      w->lastRecordUs = (t1 - t0) * QpcToUs();
      if (!ok) return 0;
      const HRESULT hr = w->dc->FinishCommandList(FALSE, &w->list);
      w->lastFinishUs = (Qpc() - t1) * QpcToUs();
      return SUCCEEDED(hr) && w->list ? 1 : 0;
    } __except (Filter(GetExceptionInformation(), w)) {
      return -1;
    }
  }

  // Drops whatever the context recorded so the next list starts clean.
  static void Discard(Worker* w) {
    __try {
      w->ring.Close();
      SafeRelease(w->list);
      ID3D11CommandList* l = nullptr;
      w->dc->FinishCommandList(FALSE, &l);
      if (l) l->Release();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
  }

  static void RunJob(Worker& w) {
    if (void (*hook)() = g_jobStartHook.load(std::memory_order_relaxed)) hook();
    w.ring.Reset();
    w.lastRecordUs = w.lastFinishUs = 0;
    const int r = RunGuarded(&w);
    ++w.jobs;
    w.recordUs += w.lastRecordUs;
    w.finishUs += w.lastFinishUs;
    if (r == 1) {
      ++w.ok;
      w.state.store(kOk, std::memory_order_release);
    } else {
      Discard(&w);
      if (r < 0) {
        ++w.faults;
        char why[640], at[160];
        ModOff(w.faultAddr, at, sizeof(at));
        int len = snprintf(why, sizeof(why), "exception 0x%08lx at %p (%s) in worker %d, data address 0x%llx; stack:",
                           static_cast<unsigned long>(w.faultCode), w.faultAddr, at, w.index,
                           static_cast<unsigned long long>(w.faultData));
        for (int f = 1; f < w.faultFrameCount && len > 0 && len < static_cast<int>(sizeof(why)) - 2; ++f) {
          ModOff(w.faultFrames[f], at, sizeof(at));
          len += snprintf(why + len, sizeof(why) - len, " %s", at);
        }
        w.pool->Disable(why);
      } else {
        ++w.failed;
      }
      w.state.store(r < 0 ? kFaulted : kFailed, std::memory_order_release);
    }
    SetEvent(w.done);
  }

  static DWORD WINAPI ThreadMain(void* arg) {
    Worker& w = *static_cast<Worker*>(arg);
    Pool& p = *w.pool;
    for (;;) {
      WaitForSingleObject(w.wake, INFINITE);
      if (p.stop_.load(std::memory_order_acquire)) break;
      int s = kQueued;
      if (!w.state.compare_exchange_strong(s, kRunning, std::memory_order_acq_rel)) continue;
      RunJob(w);
    }
    return 0;
  }

  // Joins and releases everything created so far (n_ workers).
  bool Teardown() {
    stop_.store(true, std::memory_order_release);
    HANDLE th[kMaxWorkers];
    DWORD c = 0;
    for (int i = 0; i < n_; ++i) {
      if (w_[i].wake) SetEvent(w_[i].wake);
      if (w_[i].thread) th[c++] = w_[i].thread;
    }
    if (c) {
      const DWORD r = WaitForMultipleObjects(c, th, TRUE, cfg_.joinMs);
      if (r == WAIT_TIMEOUT || r == WAIT_FAILED) {
        leaked_ = true;
        Log("%s: a worker was still recording at shutdown; its objects are left alive", cfg_.name);
        return false;
      }
    }
    for (int i = 0; i < n_; ++i) {
      Worker& w = w_[i];
      if (w.thread) CloseHandle(w.thread);
      if (w.wake) CloseHandle(w.wake);
      if (w.done) CloseHandle(w.done);
      w.thread = w.wake = w.done = nullptr;
      SafeRelease(w.list);
      w.ring.Release();
      SafeRelease(w.dc1);
      SafeRelease(w.dc);
      w.state.store(kIdle);
    }
    // A worker whose Start failed before the thread existed.
    if (n_ < kMaxWorkers) {
      Worker& w = w_[n_];
      if (w.wake) CloseHandle(w.wake);
      if (w.done) CloseHandle(w.done);
      w.wake = w.done = nullptr;
      w.ring.Release();
      SafeRelease(w.dc1);
      SafeRelease(w.dc);
    }
    n_ = 0;
    submitted_ = 0;
    SafeRelease(dev_);
    return true;
  }

  PoolConfig cfg_;
  Caps caps_;
  ID3D11Device* dev_ = nullptr;
  Worker w_[kMaxWorkers];
  int n_ = 0;
  bool started_ = false;
  bool leaked_ = false;
  uint32_t submitted_ = 0;
  std::atomic<bool> stop_{false};
  std::atomic<bool> disabled_{false};
  uint64_t timeouts_ = 0, refused_ = 0;
  double lastWaitUs_ = 0;
};

}  // namespace defrec
