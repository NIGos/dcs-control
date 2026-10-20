// Wider grids for two single-SM DCS compute kernels ([D3D] ShaderFast, default 3 = both; 0 = off; R36).
// Bit 1: grass cull, bit 2: IM2 counts clear. With the key at 0 nothing is installed.
//
// What and why [V shaders from Bazar\shaders\fxo\fxo.edcz (copies), M census 2026-10-11 03:13 and the R36
// offline harness on the RTX 5090 and WARP, 2026-10-16; R36 §3]:
// - grass2.fx grassPrepareCullDataTech (kGrassCull, stock DXBC md5 e837e87e, once per view inside GBufferPass,
//   edterrainGraphics41.dll+0x33d5e): ONE group of 32x32 threads, one thread per 32x32-texel cell of the
//   1024x1024 height map, 25 height samples per thread, then a distance test and a 6-plane box test, then an
//   append (atomic on u1[8], cell id into u0). A single SM does all of it: 30 us per view in the census,
//   19.6 us offline. Variant: the same per-thread program (the same DXBC float operations in the same order for
//   each sample, the same min/max/distance folds and plane tests), mapped as 32 groups of 32 threads, one row
//   of cells per group, so 32 SMs share the work: 4.5 us offline. The appended SET of cells and the count are
//   identical; the append ORDER is the atomic's order, which already differs between two stock runs on the GPU
//   (110 of 192 runs offline) [M: 6,336 cases identical on the GPU and on WARP, 6,144 of them borderline:
//   distance and plane thresholds set to the exact bits of a cell's own value and one ulp off].
//   Every value the kernel stores is a cell id or the atomic's count: floats only feed comparisons.
// - instancemanager2.clear.fx (kImClear, stock 8d918e57): numthreads(1,1,1), Dispatch(N) writes u0[g] = 0 for
//   g < N. DCS issues it with N = 19,527 (the object count) in every culling chain (9 per frame): 19,527 one-
//   thread groups, 11 us each. Variant: 64-thread groups, Dispatch(ceil(N/64)), the same write set and value
//   (N is compiled into the variant, one variant per N seen): 2.3 us [M R34 T4]. Bit-identical. Dispatches
//   with fewer than kClearMin groups (the 8- and 2-group clears) stay stock.
//
// Mechanism. DCS creates these shaders before the payload loads (R34), so they are identified at dispatch
// time by their output: while kinds are missing (at most kIdentMs, never during a suite), a dispatch whose
// grid and bindings have a kind's shape makes DCS's bound shader object a candidate; the worker records it
// on a deferred context of DCS's device together with our variant on synthetic inputs, the render thread
// executes that list (ExecuteCommandList(.., TRUE) restores DCS's state) and reads it back without waiting,
// and the candidate is taken only if every case equals the variant (grass: same count and words, same set) or
// the CPU reference (clear). That one test is the identification and the variant's self test on DCS's own
// device and driver.
// Substitution: DCS's immediate context gets a Dispatch hook (re-armed from chain_fast.h's CSSetShader hook
// whenever DCS binds one of the identified kernels: d3d11 swaps the dispatch entry on every
// ExecuteCommandList/Flush). For a dispatch of an identified kernel with its stock grid, the hook confirms
// the bound shader, binds the variant through d3d11's own CSSetShader, dispatches the wider grid and binds
// DCS's shader again. Nothing else in DCS's state changes. If the hook is not in place for a dispatch, the
// stock kernel runs with its stock grid: always the stock result, only slower. During GPU pass timing its own
// dispatch hook sits on top and calls TryDispatch itself.
//
// Live checks: [Suite] ShaderFastVerify=N (same-frame: the stock kernel re-run on copies of the pre-dispatch
// outputs, compared on the CPU: grass count and set, clear every word), [Suite] ShaderFastTiming=1 (GPU pass
// timing OFF/ON/OFF/ON/OFF). Needs chain_fast.h's CSSetShader hook (installed here when ChainFast is 0).
// Included once from main.cpp after chain_fast.h.
#pragma once

namespace sfast {

enum Kind : int { kGrassCull = 0, kImClear = 1, kKinds = 2 };
const char* const kName[kKinds] = {"grass cull (grass2 grassPrepareCullDataTech)",
                                   "IM2 counts clear (instancemanager2.clear)"};
constexpr UINT kGrassRows = 32;      // variant grid: one group per row of 32 cells
constexpr UINT kClearMin = 256;      // smaller clears stay stock (they are at the dispatch floor)
constexpr ULONGLONG kIdentMs = 180000;
constexpr int kMaxStock = 4;         // identified stock objects per kind

// ---------------------------------------------------------------------------
// Variants
// ---------------------------------------------------------------------------
// The stock per-thread program of e837e87e (cs_5_0, numthreads(32,32,1), Dispatch(1,1,1)) with the cell's
// row taken from the group: same CB0/CB1 registers, sampler s8, t0, u0, u1. Per sample: iadd, utof, add 0.5,
// mul by cb0[55].z, mad *2-1, mov -y, dp3 with cb0[32].xyw and cb0[34].xyw, sample_l level 0, add -cb1[35].y,
// then min/max/sqrt-distance folds; then the distance cut against cb0[55].x and the 6-plane test of the box.
const char kSrcGrass[] = R"(
cbuffer CB0 : register(b0) { float4 c0[56]; }
cbuffer CB1 : register(b1) { float4 c1[39]; }
SamplerState s8 : register(s8);
Texture2D<float4> t0 : register(t0);
RWBuffer<uint> u0 : register(u0);
RWBuffer<uint> u1 : register(u1);
[numthreads(32, 1, 1)]
void main(uint3 gtid : SV_GroupThreadID, uint3 gid : SV_GroupID) {
  uint3 tid = uint3(gtid.x, gid.x, 0);
  uint2 base = tid.xy << 5;
  float3 mn = float3(1e30, 1e30, 1e30);
  float3 mx = float3(-1e30, -1e30, -1e30);
  float dmin = 100000.0;
  [loop] for (uint j = 0; j < 5; ++j) {
    float3 p[5];
    [unroll] for (uint i = 0; i < 5; ++i) {
      float2 uv = (float2)(base + uint2(i * 8, j * 8));
      uv = uv + 0.5;
      uv = uv * c0[55].z;
      float4 r;
      r.xy = uv * 2.0 - 1.0;
      r.z = -r.y;
      r.w = 1.0;
      float h = t0.SampleLevel(s8, uv, 0).x;
      p[i] = float3(dot(r.xzw, c0[32].xyw), h - c1[35].y, dot(r.xzw, c0[34].xyw));
    }
    [unroll] for (uint k = 0; k < 5; ++k) {
      mn = min(mn, p[k]);
      mx = max(mx, p[k]);
      float3 d = p[k] - c1[38].xyz;
      dmin = min(dmin, sqrt(dot(d, d)));
    }
  }
  if (c0[55].x < dmin) return;
  float3 ctr = mn + mx;
  float4 ext = float4(mx, mx.x) - float4(mn, mn.x);
  float4 h4 = ext * float4(0.5, 0.5, 0.5, -0.5);
  float2 hn = ext.yz * -0.5;
  bool culled = false;
  [unroll] for (uint pl = 0; pl < 6; ++pl) {
    float4 P = c0[49 + pl];
    float3 q;
    q.x = P.x < 0 ? h4.w : h4.x;
    q.y = P.y < 0 ? hn.x : h4.y;
    q.z = P.z < 0 ? hn.y : h4.z;
    float3 a = ctr * 0.5 - q;
    float3 b = ctr * 0.5 + q;
    if (!(dot(P, float4(a, 1.0)) >= 0 || dot(P, float4(b, 1.0)) >= 0)) { culled = true; break; }
  }
  if (culled) return;
  uint at;
  InterlockedAdd(u1[8], 1, at);
  u0[at] = tid.y * 0x200000 + (tid.x << 5);
}
)";
// instancemanager2.clear with the dispatch's group count N compiled in.
inline std::string ClearSrc(UINT n) {
  char b[256];
  snprintf(b, sizeof(b),
           "RWBuffer<uint> u0 : register(u0);\n"
           "[numthreads(64, 1, 1)] void main(uint3 t : SV_DispatchThreadID) { if (t.x < %uu) u0[t.x] = 0; }\n",
           n);
  return b;
}

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------
std::atomic<uint32_t> g_mask{0};
std::atomic<bool> g_live{false};  // hook path wanted: mask set, hooks installable
std::atomic<ID3D11ComputeShader*> g_stock[kKinds][kMaxStock];  // DCS's identified objects (AddRef)
std::atomic<int> g_nStock[kKinds];
std::atomic<ID3D11ComputeShader*> g_grassVar{nullptr};  // on DCS's device
std::vector<uint8_t> g_grassCode;
struct ClearVar {
  UINT n = 0;
  ID3D11ComputeShader* cs = nullptr;
};
constexpr int kMaxClear = 8;
ClearVar g_clear[kMaxClear];
std::atomic<int> g_nClear{0};
std::atomic<UINT> g_wantN{0};
std::atomic<uint64_t> g_subs[kKinds], g_rearms{0}, g_probed{0};
std::string g_err;

// Dispatch hook on DCS's immediate context.
using DispFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, UINT);
void** g_vt = nullptr;
int g_slot = -1;
void* volatile g_orig = nullptr;
thread_local bool t_inside = false;

inline HMODULE SelfModule() {
  static HMODULE m = [] {
    HMODULE h = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&SelfModule), &h);
    return h;
  }();
  return m;
}
// R90: the payload's image range read once (d3ds::InPayload); Rearm runs on the render thread, where the loader
// lock of GetModuleHandleExW (taken per call before) could wait behind a module load on another thread.
inline bool InThisModule(void* p) { return p && d3ds::InPayload(p); }

inline int KindOf(ID3D11ComputeShader* s) {
  if (!s) return -1;
  for (int k = 0; k < kKinds; ++k) {
    const int n = g_nStock[k].load(std::memory_order_acquire);
    for (int i = 0; i < n; ++i)
      if (g_stock[k][i].load(std::memory_order_relaxed) == s) return k;
  }
  return -1;
}
inline ID3D11ComputeShader* ClearVariant(UINT n) {
  const int c = g_nClear.load(std::memory_order_acquire);
  for (int i = 0; i < c; ++i)
    if (g_clear[i].n == n) return g_clear[i].cs;
  return nullptr;
}

// ---------------------------------------------------------------------------
// Identification by output (render thread: classification and execution; worker: record, evaluate)
// ---------------------------------------------------------------------------
std::atomic<bool> g_identOn{false};
bool g_identDone = false;
ULONGLONG g_identT0 = 0;
std::atomic<uint32_t> g_found{0};
int g_candTested = 0, g_candRejected = 0;
struct Cand {
  ID3D11ComputeShader* cs;  // AddRef
  int kind;
};
std::mutex g_candMu;
std::vector<Cand> g_cands;
std::atomic<void*> g_tried[chainfast::kDiagCap];  // shaders queued once (open addressing)

inline bool TypedR32Buffer(ID3D11UnorderedAccessView* v, UINT minElems) {
  if (!v) return false;
  D3D11_UNORDERED_ACCESS_VIEW_DESC d{};
  v->GetDesc(&d);
  return d.ViewDimension == D3D11_UAV_DIMENSION_BUFFER && d.Format == DXGI_FORMAT_R32_UINT &&
         d.Buffer.FirstElement == 0 && d.Buffer.NumElements >= minElems && d.Buffer.Flags == 0;
}
inline UINT CbBytes(ID3D11Buffer* b) {
  if (!b) return 0;
  D3D11_BUFFER_DESC d{};
  b->GetDesc(&d);
  return d.ByteWidth;
}

// A dispatch with a kind's grid: does it have that kind's bindings? Then DCS's bound object is a candidate.
void Classify(ID3D11DeviceContext* c, UINT x, UINT y, UINT z) {
  int kind = -1;
  if (x == 1 && y == 1 && z == 1) kind = kGrassCull;
  else if (y == 1 && z == 1 && x >= kClearMin) kind = kImClear;
  if (kind < 0 || ((g_found.load(std::memory_order_relaxed) >> kind) & 1)) return;
  ID3D11ComputeShader* cs = nullptr;
  c->CSGetShader(&cs, nullptr, nullptr);
  if (!cs) return;
  if (chainfast::DiagHas(g_tried, cs)) {
    cs->Release();
    return;
  }
  ID3D11UnorderedAccessView* u[3] = {};
  c->CSGetUnorderedAccessViews(0, 3, u);
  bool ok = false;
  if (kind == kGrassCull) {
    ok = TypedR32Buffer(u[0], 1024) && TypedR32Buffer(u[1], 9) && !u[2];
    if (ok) {
      ID3D11ShaderResourceView* t = nullptr;
      c->CSGetShaderResources(0, 1, &t);
      D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
      if (t) t->GetDesc(&sd);
      ok = t && sd.ViewDimension == D3D11_SRV_DIMENSION_TEXTURE2D;
      if (t) t->Release();
      ID3D11Buffer* cb[2] = {};
      c->CSGetConstantBuffers(0, 2, cb);
      ok = ok && CbBytes(cb[0]) >= 56 * 16 && CbBytes(cb[1]) >= 39 * 16;
      for (auto* b : cb)
        if (b) b->Release();
    }
  } else {
    ok = TypedR32Buffer(u[0], x) && !u[1];
  }
  for (auto* v : u)
    if (v) v->Release();
  if (!ok || !chainfast::DiagInsert(g_tried, cs)) {
    cs->Release();
    return;
  }
  g_probed.fetch_add(1, std::memory_order_relaxed);
  std::lock_guard<std::mutex> lock(g_candMu);
  g_cands.push_back(Cand{cs, kind});  // keeps the reference
}

// Synthetic grass inputs: a 1024x1024 R32_FLOAT height map and CB0/CB1 with the registers the kernel reads.
struct GrassCase {
  int map;                 // 0 smooth, 1 flat (ties), 2 rough
  float cam[3], dist;      // c1[38].xyz, c0[55].x
  float planes[6][4];      // c0[49..54]
  uint32_t start;          // u1[8] before
};
inline std::vector<GrassCase> GrassCases() {
  std::vector<GrassCase> v;
  chainfast::Rng rng{0x6a55ull};
  auto f = [&](float lo, float hi) { return lo + (hi - lo) * static_cast<float>(rng() % 100000) / 100000.0f; };
  for (int i = 0; i < 6; ++i) {
    GrassCase g{};
    g.map = i % 3;
    g.cam[0] = f(-150, 150);
    g.cam[1] = f(-10, 10);
    g.cam[2] = f(-150, 150);
    g.dist = f(60, 300);
    for (int p = 0; p < 6; ++p) {
      g.planes[p][0] = f(-1, 1);
      g.planes[p][1] = f(-0.2f, 0.2f);
      g.planes[p][2] = f(-1, 1);
      g.planes[p][3] = f(40, 260);
    }
    if (i == 3) g.planes[0][3] = -1e6f;  // one case culls everything
    g.start = i == 4 ? 17u : 0u;
    v.push_back(g);
  }
  return v;
}
inline void GrassMap(int kind, std::vector<float>& h) {
  h.resize(1024 * 1024);
  chainfast::Rng rng{0x4d41ull + kind};
  for (int y = 0; y < 1024; ++y)
    for (int x = 0; x < 1024; ++x) {
      float v;
      if (kind == 0)
        v = 256.0f + 8.0f * sinf(x * 0.013f) * cosf(y * 0.017f + 1.0f) + 2.0f * sinf(x * 0.11f + y * 0.07f) +
            static_cast<float>(rng() % 1000) * 5e-5f;
      else if (kind == 1)
        v = 248.0f;
      else
        v = static_cast<float>(rng() % 2048) * 0.25f;
      h[static_cast<size_t>(y) * 1024 + x] = v;
    }
}
inline void GrassCbs(const GrassCase& g, std::vector<uint8_t>& c0, std::vector<uint8_t>& c1) {
  c0.assign(56 * 16, 0);
  c1.assign(39 * 16, 0);
  auto set = [](std::vector<uint8_t>& b, int reg, int comp, float v) { memcpy(&b[reg * 16 + comp * 4], &v, 4); };
  // heightMapProjInv rows as DCS sets them (2.9.30 census): x = 256*(-ndc.y) - 6, z = 256*ndc.x - 13.5
  set(c0, 32, 0, -0.0f), set(c0, 32, 1, 256.0f), set(c0, 32, 3, -6.0f);
  set(c0, 34, 0, 256.0f), set(c0, 34, 1, -0.0f), set(c0, 34, 3, -13.5f);
  for (int p = 0; p < 6; ++p)
    for (int k = 0; k < 4; ++k) set(c0, 49 + p, k, g.planes[p][k]);
  set(c0, 55, 0, g.dist), set(c0, 55, 1, 0.707f), set(c0, 55, 2, 1.0f / 1024.0f);
  set(c1, 35, 1, 256.0f);
  for (int k = 0; k < 3; ++k) set(c1, 38, k, g.cam[k]);
  set(c1, 38, 3, 1.0f);
}
inline ID3D11Buffer* MakeCbBytes(ID3D11Device* d, const std::vector<uint8_t>& v) {
  D3D11_BUFFER_DESC bd{};
  bd.ByteWidth = static_cast<UINT>((v.size() + 15) & ~size_t{15});
  bd.Usage = D3D11_USAGE_DEFAULT;
  bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
  std::vector<uint8_t> t(bd.ByteWidth, 0);
  memcpy(t.data(), v.data(), v.size());
  D3D11_SUBRESOURCE_DATA sd{t.data(), 0, 0};
  ID3D11Buffer* b = nullptr;
  d->CreateBuffer(&bd, &sd, &b);
  return b;
}
inline uint32_t Sentinel(uint32_t i) { return 0x5A5A0000u ^ (i * 2246822519u); }
// Grass results equal: u1 word for word; u0 the same multiset in [start, count), the rest word for word.
inline bool SameGrass(const std::vector<uint32_t>& a0, const std::vector<uint32_t>& a1, const std::vector<uint32_t>& b0,
                      const std::vector<uint32_t>& b1, uint32_t start) {
  if (a1 != b1 || a0.size() != b0.size() || a1.size() < 9) return false;
  const uint32_t n = (std::min)(a1[8], static_cast<uint32_t>(a0.size()));
  const uint32_t s = (std::min)(start, n);
  for (size_t i = 0; i < a0.size(); ++i)
    if ((i < s || i >= n) && a0[i] != b0[i]) return false;
  std::vector<uint32_t> x(a0.begin() + s, a0.begin() + n), y(b0.begin() + s, b0.begin() + n);
  std::sort(x.begin(), x.end());
  std::sort(y.begin(), y.end());
  return x == y;
}

struct FpOut {
  ID3D11Buffer* stage = nullptr;
  UINT bytes = 0;
  std::vector<uint32_t> data;
};
struct FpJob {
  Cand cand{};
  ID3D11CommandList* cl = nullptr;
  std::vector<FpOut> outs;
  std::vector<GrassCase> grass;
  std::vector<std::pair<UINT, UINT>> clear;  // (groups, buffer elements)
  size_t mapped = 0;
  std::atomic<int> state{0};  // 1 recorded, 2 executed, 3 read back, -1 failed
  ULONGLONG t0 = 0;
  void ReleaseGpu() {
    for (FpOut& o : outs)
      if (o.stage) o.stage->Release(), o.stage = nullptr;
    if (cl) cl->Release(), cl = nullptr;
  }
};
std::atomic<FpJob*> g_fpJob{nullptr};
std::atomic<bool> g_fpLive{false};
std::vector<FpJob*> g_fpDone;

// Render thread (from the CSSetShader hook): run the recorded test, read it back without waiting.
void FpOnRender(ID3D11DeviceContext* c) {
  FpJob* j = g_fpJob.load(std::memory_order_acquire);
  if (!j) return;
  const int st = j->state.load(std::memory_order_acquire);
  if (st == 1) {
    j->state.store(2, std::memory_order_release);
    c->ExecuteCommandList(j->cl, TRUE);  // DCS's state comes back afterwards
    return;
  }
  if (st != 2) return;
  while (j->mapped < j->outs.size()) {
    FpOut& o = j->outs[j->mapped];
    D3D11_MAPPED_SUBRESOURCE m{};
    const HRESULT hr = c->Map(o.stage, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m);
    if (hr == DXGI_ERROR_WAS_STILL_DRAWING) return;
    if (FAILED(hr)) {
      j->state.store(-1, std::memory_order_release);
      return;
    }
    o.data.resize(o.bytes / 4);
    memcpy(o.data.data(), m.pData, o.bytes);
    c->Unmap(o.stage, 0);
    ++j->mapped;
  }
  j->state.store(3, std::memory_order_release);
}

// Worker: record DCS's object (and, for the grass kind, our variant) on the synthetic cases.
bool FpBuild(FpJob& j, ID3D11Device* d) {
  ID3D11DeviceContext* dc = nullptr;
  if (FAILED(d->CreateDeferredContext(0, &dc)) || !dc) return false;
  std::vector<chainfast::TBuf> keep;
  std::vector<IUnknown*> keepU;
  bool ok = true;
  auto stage = [&](const chainfast::TBuf& b) {
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = b.bytes;
    bd.Usage = D3D11_USAGE_STAGING;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    FpOut o;
    o.bytes = b.bytes;
    if (FAILED(d->CreateBuffer(&bd, nullptr, &o.stage))) {
      ok = false;
      return;
    }
    dc->CopyResource(o.stage, b.b);
    j.outs.push_back(std::move(o));
  };
  ID3D11UnorderedAccessView* nul[8] = {};
  if (j.cand.kind == kImClear) {
    j.clear = {{300, 1000}, {1000, 1000}, {19527, 19600}};
    for (const auto& k : j.clear) {
      std::vector<uint32_t> s(k.second);
      for (uint32_t i = 0; i < k.second; ++i) s[i] = Sentinel(i);
      chainfast::TBuf out;
      ok = ok && chainfast::MakeBuf(d, k.second, 0, s.data(), false, true, out);
      if (ok) {
        dc->CSSetShader(j.cand.cs, nullptr, 0);
        dc->CSSetUnorderedAccessViews(0, 1, &out.uav, nullptr);
        dc->Dispatch(k.first, 1, 1);
        dc->CSSetUnorderedAccessViews(0, 8, nul, nullptr);
        stage(out);
      }
      keep.push_back(out);
      if (!ok) break;
    }
  } else {
    ID3D11ComputeShader* var = g_grassVar.load();
    ok = var != nullptr;
    j.grass = GrassCases();
    D3D11_SAMPLER_DESC sdesc{D3D11_FILTER_MIN_MAG_MIP_LINEAR, D3D11_TEXTURE_ADDRESS_CLAMP, D3D11_TEXTURE_ADDRESS_CLAMP,
                             D3D11_TEXTURE_ADDRESS_CLAMP, 0, 1, D3D11_COMPARISON_NEVER, {0, 0, 0, 0}, 0,
                             D3D11_FLOAT32_MAX};
    ID3D11SamplerState* samp = nullptr;
    ok = ok && SUCCEEDED(d->CreateSamplerState(&sdesc, &samp));
    if (samp) keepU.push_back(samp);
    ID3D11ShaderResourceView* maps[3] = {};
    for (int m = 0; m < 3 && ok; ++m) {
      std::vector<float> h;
      GrassMap(m, h);
      D3D11_TEXTURE2D_DESC td{1024, 1024, 1, 1, DXGI_FORMAT_R32_FLOAT, {1, 0}, D3D11_USAGE_DEFAULT,
                              D3D11_BIND_SHADER_RESOURCE, 0, 0};
      D3D11_SUBRESOURCE_DATA sd{h.data(), 1024 * 4, 0};
      ID3D11Texture2D* t = nullptr;
      ok = SUCCEEDED(d->CreateTexture2D(&td, &sd, &t)) && SUCCEEDED(d->CreateShaderResourceView(t, nullptr, &maps[m]));
      if (t) keepU.push_back(t);
      if (maps[m]) keepU.push_back(maps[m]);
    }
    for (const GrassCase& g : j.grass) {
      if (!ok) break;
      std::vector<uint8_t> c0, c1;
      GrassCbs(g, c0, c1);
      ID3D11Buffer* cb[2] = {MakeCbBytes(d, c0), MakeCbBytes(d, c1)};
      for (auto* b : cb)
        if (b) keepU.push_back(b);
      ok = cb[0] && cb[1];
      std::vector<uint32_t> s0(1024), s1(32);
      for (uint32_t i = 0; i < 1024; ++i) s0[i] = Sentinel(i);
      for (uint32_t i = 0; i < 32; ++i) s1[i] = 0x1000u + i;
      s1[8] = g.start;
      for (int v = 0; v < 2 && ok; ++v) {
        chainfast::TBuf u0, u1;
        ok = chainfast::MakeBuf(d, 1024, 0, s0.data(), false, true, u0) &&
             chainfast::MakeBuf(d, 32, 0, s1.data(), false, true, u1);
        if (ok) {
          dc->CSSetShader(v ? var : j.cand.cs, nullptr, 0);
          dc->CSSetConstantBuffers(0, 2, cb);
          dc->CSSetShaderResources(0, 1, &maps[g.map]);
          dc->CSSetSamplers(8, 1, &samp);
          ID3D11UnorderedAccessView* uu[2] = {u0.uav, u1.uav};
          dc->CSSetUnorderedAccessViews(0, 2, uu, nullptr);
          dc->Dispatch(v ? kGrassRows : 1, 1, 1);
          dc->CSSetUnorderedAccessViews(0, 8, nul, nullptr);
          stage(u0);
          stage(u1);
        }
        keep.push_back(u0);
        keep.push_back(u1);
      }
    }
  }
  if (ok) ok = SUCCEEDED(dc->FinishCommandList(FALSE, &j.cl)) && j.cl;
  dc->Release();
  for (chainfast::TBuf& b : keep) b.Release();  // the command list keeps what it uses
  for (IUnknown* u : keepU) u->Release();
  return ok;
}

// Worker: every case equals the reference (clear: CPU; grass: the variant on the same device), and the grass
// cases are not degenerate (some cells appended, not all, not the same count everywhere).
bool FpEvaluate(const FpJob& j, int* nCases) {
  if (j.cand.kind == kImClear) {
    *nCases = static_cast<int>(j.clear.size());
    if (j.outs.size() != j.clear.size()) return false;
    for (size_t i = 0; i < j.clear.size(); ++i) {
      const auto& o = j.outs[i].data;
      if (o.size() != j.clear[i].second) return false;
      for (uint32_t e = 0; e < o.size(); ++e)
        if (o[e] != (e < j.clear[i].first ? 0u : Sentinel(e))) return false;
    }
    return true;
  }
  *nCases = static_cast<int>(j.grass.size());
  if (j.outs.size() != 4 * j.grass.size()) return false;
  uint32_t lo = ~0u, hi = 0;
  bool partial = false;
  for (size_t i = 0; i < j.grass.size(); ++i) {
    const auto &a0 = j.outs[4 * i].data, &a1 = j.outs[4 * i + 1].data, &b0 = j.outs[4 * i + 2].data,
               &b1 = j.outs[4 * i + 3].data;
    if (!SameGrass(a0, a1, b0, b1, j.grass[i].start)) return false;
    const uint32_t n = a1[8] - j.grass[i].start;
    lo = (std::min)(lo, n);
    hi = (std::max)(hi, n);
    partial |= n > 0 && n < 1024;
  }
  return partial && lo < hi;
}

void TakeCandidate(const FpJob& j, int cases) {
  const int k = j.cand.kind;
  const int n = g_nStock[k].load();
  if (n >= kMaxStock) {
    j.cand.cs->Release();
    return;
  }
  g_stock[k][n].store(j.cand.cs);  // keeps the probe's reference
  g_nStock[k].store(n + 1, std::memory_order_release);
  g_found.fetch_or(1u << k);
  Log("shader fast: DCS's shader %p is the stock %s: %d of %d synthetic cases equal %s on DCS's device",
      static_cast<void*>(j.cand.cs), kName[k], cases, cases,
      k == kGrassCull ? "the 32-group variant (same count, same cell set)" : "the CPU reference");
}

// Worker, every second: identification window, one fingerprint job at a time.
void IdentTick(uint32_t mask) {
  const ULONGLONG now = GetTickCount64();
  const uint32_t missing = mask & ~g_found.load();
  if (!g_identOn.load() && !g_identDone && missing && !g_benchRunningFlag.load()) {
    g_identOn = true;
    g_identDone = true;
    g_identT0 = now;
    Log("shader fast: kinds 0x%x: identifying DCS's shader objects at dispatch time by their output", missing);
  }
  if (g_identOn.load() && (!missing || now - g_identT0 > kIdentMs || g_benchRunningFlag.load())) {
    g_identOn = false;
    Log("shader fast: identification ended after %llu s: kinds found 0x%x, %llu candidates queued, %d tested, %d "
        "rejected",
        static_cast<unsigned long long>((now - g_identT0) / 1000), g_found.load(),
        static_cast<unsigned long long>(g_probed.load()), g_candTested, g_candRejected);
  }
  if (FpJob* j = g_fpJob.load()) {
    const int st = j->state.load();
    if (st != 3 && st != -1 && now - j->t0 <= 20000) return;
    g_fpLive = false;
    g_fpJob = nullptr;
    int cases = 0;
    const bool pass = st == 3 && FpEvaluate(*j, &cases);
    ++g_candTested;
    j->ReleaseGpu();
    if (pass) {
      TakeCandidate(*j, cases);
    } else {
      ++g_candRejected;
      if (g_candRejected <= 10)
        Log("shader fast: candidate %p for %s rejected (%s)", static_cast<void*>(j->cand.cs), kName[j->cand.kind],
            st == 3 ? "output differs" : st == -1 ? "read back failed" : "not run in 20 s");
      j->cand.cs->Release();
    }
    j->cand.cs = nullptr;
    g_fpDone.push_back(j);
    return;
  }
  if (g_benchRunningFlag.load() || !chainfast::g_dcsDev) return;
  Cand c{nullptr, -1};
  {
    std::lock_guard<std::mutex> lock(g_candMu);
    for (size_t i = 0; i < g_cands.size();) {
      const Cand x = g_cands[i];
      if ((g_found.load() >> x.kind) & 1) {  // its kind was found meanwhile
        x.cs->Release();
        g_cands.erase(g_cands.begin() + i);
        continue;
      }
      if (x.kind == kGrassCull && !g_grassVar.load()) {  // needs the variant on DCS's device: later
        ++i;
        continue;
      }
      c = x;
      g_cands.erase(g_cands.begin() + i);
      break;
    }
  }
  if (!c.cs) return;
  FpJob* j = new FpJob;
  j->cand = c;
  j->t0 = now;
  if (!FpBuild(*j, chainfast::g_dcsDev)) {
    Log("shader fast: candidate %p for %s: test not recordable", static_cast<void*>(c.cs), kName[c.kind]);
    j->ReleaseGpu();
    c.cs->Release();
    j->cand.cs = nullptr;
    g_fpDone.push_back(j);
    return;
  }
  j->state = 1;
  g_fpJob = j;
  g_fpLive = true;
}

// ---------------------------------------------------------------------------
// Variants on DCS's device (worker)
// ---------------------------------------------------------------------------
bool PrepareGrass() {
  if (g_grassVar.load()) return true;
  static bool failed = false;
  if (failed || !chainfast::g_dcsDev) return false;
  if (!chainfast::Compile(kSrcGrass, "qv_sf_grass", g_grassCode, &g_err)) {
    failed = true;
    Log("shader fast: grass variant does not compile (left stock): %s", g_err.c_str());
    return false;
  }
  ID3D11ComputeShader* v = nullptr;
  if (FAILED(chainfast::g_dcsDev->CreateComputeShader(g_grassCode.data(), g_grassCode.size(), nullptr, &v)) || !v) {
    failed = true;
    Log("shader fast: grass variant not creatable on DCS's device (left stock)");
    return false;
  }
  g_grassVar.store(v);
  return true;
}

// Clear variant for N: compiled, checked against the CPU reference on a private device, created on DCS's.
bool ClearSelfTest(ID3D11Device* d, ID3D11DeviceContext* c, const std::vector<uint8_t>& code, UINT n) {
  ID3D11ComputeShader* cs = nullptr;
  if (FAILED(d->CreateComputeShader(code.data(), code.size(), nullptr, &cs)) || !cs) return false;
  bool ok = true;
  for (UINT elems : {n, n + 77}) {
    std::vector<uint32_t> s(elems), r;
    for (uint32_t i = 0; i < elems; ++i) s[i] = Sentinel(i);
    chainfast::TBuf b;
    ok = ok && chainfast::MakeBuf(d, elems, 0, s.data(), false, true, b);
    if (ok) {
      c->CSSetShader(cs, nullptr, 0);
      c->CSSetUnorderedAccessViews(0, 1, &b.uav, nullptr);
      c->Dispatch((n + 63) / 64, 1, 1);
      ID3D11UnorderedAccessView* nul = nullptr;
      c->CSSetUnorderedAccessViews(0, 1, &nul, nullptr);
      ok = chainfast::ReadBuf(d, c, b.b, b.bytes, r) && r.size() == elems;
      for (uint32_t i = 0; ok && i < elems; ++i) ok = r[i] == (i < n ? 0u : Sentinel(i));
    }
    b.Release();
  }
  c->CSSetShader(nullptr, nullptr, 0);
  cs->Release();
  return ok;
}
void PrepareClear() {
  const UINT n = g_wantN.exchange(0);
  if (!n || ClearVariant(n) || g_nClear.load() >= kMaxClear || !chainfast::g_dcsDev) return;
  std::vector<uint8_t> code;
  const std::string src = ClearSrc(n);
  bool ok = chainfast::Compile(src.c_str(), "qv_sf_clear", code, &g_err);
  if (ok) {
    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    ok = chainfast::AcquirePrivate(&dev, &ctx) && ClearSelfTest(dev, ctx, code, n);  // R90 F4: no device per test
  }
  ID3D11ComputeShader* v = nullptr;
  ok = ok && SUCCEEDED(chainfast::g_dcsDev->CreateComputeShader(code.data(), code.size(), nullptr, &v)) && v;
  if (!ok) {
    Log("shader fast: clear variant for %u groups not usable (that clear stays stock)", n);
    if (v) v->Release();
    ClearVar& e = g_clear[g_nClear.load()];
    e.n = n;
    e.cs = nullptr;  // remembered as failed: ClearVariant returns null, no retry
    g_nClear.fetch_add(1, std::memory_order_release);
    return;
  }
  ClearVar& e = g_clear[g_nClear.load()];
  e.n = n;
  e.cs = v;
  g_nClear.fetch_add(1, std::memory_order_release);
  Log("shader fast: clear variant for %u groups ready (%u groups of 64, self test against the CPU reference "
      "passed)",
      n, (n + 63) / 64);
}

// ---------------------------------------------------------------------------
// Live same-frame check (render thread)
// ---------------------------------------------------------------------------
std::atomic<bool> g_verify{false};
std::atomic<uint64_t> g_checks[kKinds], g_diff[kKinds], g_skipped{0};
std::atomic<int> g_pending{0};          // ring slots waiting for their read back
std::atomic<bool> g_testSpoil{false};  // tests: spoil DCS's result before the comparison
struct Scratch {
  ID3D11Buffer* b = nullptr;
  ID3D11UnorderedAccessView* u = nullptr;
};
std::map<std::pair<ID3D11Resource*, int>, Scratch> g_scratch;
// One check: staging copies of DCS's u0 (, u1), the stock re-run's u0 (, u1) and (grass) u1 before both.
struct VSlot {
  int kind = -1;
  ID3D11Buffer* st[5] = {};
  bool live = false;
};
constexpr int kSlots = 8;
VSlot g_ring[kSlots];
struct Pre {
  int kind = -1;
  ID3D11UnorderedAccessView* dcs[2] = {};  // AddRef
  Scratch copy[2];
};
Pre t_pre;

Scratch ScratchFor(ID3D11Device* d, ID3D11UnorderedAccessView* v, int tag) {
  D3D11_UNORDERED_ACCESS_VIEW_DESC vd{};
  v->GetDesc(&vd);
  if (vd.ViewDimension != D3D11_UAV_DIMENSION_BUFFER) return {};
  ID3D11Resource* r = nullptr;
  v->GetResource(&r);
  if (!r) return {};
  r->Release();  // DCS keeps it alive while bound
  const auto key = std::make_pair(r, tag);
  auto it = g_scratch.find(key);
  if (it != g_scratch.end()) return it->second;
  if (g_scratch.size() >= 32) return {};
  Scratch s;
  D3D11_BUFFER_DESC bd{};
  static_cast<ID3D11Buffer*>(r)->GetDesc(&bd);
  bd.Usage = D3D11_USAGE_DEFAULT;
  bd.CPUAccessFlags = 0;
  bd.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
  bd.MiscFlags &= D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS | D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS;
  if (FAILED(d->CreateBuffer(&bd, nullptr, &s.b))) return {};
  if (FAILED(d->CreateUnorderedAccessView(s.b, &vd, &s.u))) {
    s.b->Release();
    return {};
  }
  g_scratch.emplace(key, s);
  return s;
}
ID3D11Buffer* StagingOf(ID3D11Device* d, ID3D11Resource* src) {
  D3D11_BUFFER_DESC bd{};
  static_cast<ID3D11Buffer*>(src)->GetDesc(&bd);
  bd.Usage = D3D11_USAGE_STAGING;
  bd.BindFlags = 0;
  bd.MiscFlags = 0;
  bd.StructureByteStride = 0;
  bd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  ID3D11Buffer* s = nullptr;
  d->CreateBuffer(&bd, nullptr, &s);
  return s;
}

// Before the variant's dispatch: copies of the outputs as they are.
bool VerifyPre(ID3D11DeviceContext* c, int kind) {
  t_pre = Pre{};
  ID3D11Device* d = chainfast::g_dcsDev;
  if (!d) return false;
  const int n = kind == kGrassCull ? 2 : 1;
  ID3D11UnorderedAccessView* u[2] = {};
  c->CSGetUnorderedAccessViews(0, n, u);
  bool ok = true;
  for (int i = 0; i < n && ok; ++i) {
    ok = u[i] && (t_pre.copy[i] = ScratchFor(d, u[i], i)).u;
    if (ok) {
      ID3D11Resource* r = nullptr;
      u[i]->GetResource(&r);
      c->CopyResource(t_pre.copy[i].b, r);
      r->Release();
    }
  }
  if (!ok) {
    for (auto* v : u)
      if (v) v->Release();
    t_pre = Pre{};
    g_skipped.fetch_add(1);
    return false;
  }
  t_pre.kind = kind;
  t_pre.dcs[0] = u[0];
  t_pre.dcs[1] = u[1];
  return true;
}

// After the variant's dispatch (the stock object is bound again): the stock grid on the pre-dispatch
// copies, then staging copies of both results into a ring slot, compared when the render thread reads
// them back (no wait).
void VerifyPost(ID3D11DeviceContext* c, int kind, UINT x, DispFn next) {
  Pre p = t_pre;
  t_pre = Pre{};
  if (p.kind != kind) return;
  ID3D11Device* d = chainfast::g_dcsDev;
  const int n = kind == kGrassCull ? 2 : 1;
  VSlot* k = nullptr;
  for (VSlot& s : g_ring)
    if (!s.live) {
      k = &s;
      break;
    }
  bool ok = k != nullptr;
  if (ok) {
    for (auto*& s : k->st)
      if (s) s->Release(), s = nullptr;
    *k = VSlot{};
    k->kind = kind;
    if (kind == kGrassCull) {  // u1 before (the append's start count)
      k->st[4] = StagingOf(d, p.copy[1].b);
      ok = k->st[4] != nullptr;
      if (ok) c->CopyResource(k->st[4], p.copy[1].b);
    }
  }
  if (ok) {
    ID3D11UnorderedAccessView* uu[2] = {p.copy[0].u, p.copy[1].u};
    UINT keep[2] = {~0u, ~0u};
    c->CSSetUnorderedAccessViews(0, n, uu, keep);
    t_inside = true;
    next(c, x, 1, 1);
    t_inside = false;
    c->CSSetUnorderedAccessViews(0, n, p.dcs, keep);
    if (g_testSpoil.load()) {
      const UINT bad[4] = {7, 7, 7, 7};
      c->ClearUnorderedAccessViewUint(p.dcs[0], bad);
    }
    for (int i = 0; i < n && ok; ++i) {
      ID3D11Resource* r = nullptr;
      p.dcs[i]->GetResource(&r);
      k->st[i] = StagingOf(d, r);
      k->st[2 + i] = StagingOf(d, p.copy[i].b);
      ok = k->st[i] && k->st[2 + i];
      if (ok) {
        c->CopyResource(k->st[i], r);
        c->CopyResource(k->st[2 + i], p.copy[i].b);
      }
      r->Release();
    }
  }
  if (ok) {
    k->live = true;
    g_pending.fetch_add(1);
  } else {
    g_skipped.fetch_add(1);
  }
  for (auto* v : p.dcs)
    if (v) v->Release();
}

std::vector<uint32_t> MapAll(ID3D11DeviceContext* c, ID3D11Buffer* b, bool* busy) {
  std::vector<uint32_t> out;
  D3D11_MAPPED_SUBRESOURCE m{};
  const HRESULT hr = c->Map(b, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m);
  if (hr == DXGI_ERROR_WAS_STILL_DRAWING) {
    *busy = true;
    return out;
  }
  if (FAILED(hr)) return out;
  D3D11_BUFFER_DESC bd{};
  b->GetDesc(&bd);
  out.resize(bd.ByteWidth / 4);
  memcpy(out.data(), m.pData, out.size() * 4);
  c->Unmap(b, 0);
  return out;
}
// Render thread: compare the slots whose copies are done.
void ReadChecks(ID3D11DeviceContext* c) {
  for (VSlot& k : g_ring) {
    if (!k.live) continue;
    bool busy = false;
    std::vector<uint32_t> v[5];
    for (int i = 0; i < 5 && !busy; ++i)
      if (k.st[i]) v[i] = MapAll(c, k.st[i], &busy);
    if (busy) continue;
    bool same;
    if (k.kind == kGrassCull)
      same = !v[0].empty() && v[4].size() > 8 && SameGrass(v[0], v[1], v[2], v[3], v[4][8]);
    else
      same = !v[0].empty() && v[0] == v[2];
    g_checks[k.kind].fetch_add(1);
    if (!same) g_diff[k.kind].fetch_add(1);
    k.live = false;
    g_pending.fetch_sub(1);
  }
}

// ---------------------------------------------------------------------------
// Hooks (render thread)
// ---------------------------------------------------------------------------
// The substitution. next = the Dispatch entry below whoever calls (ours or gpu_pass_timing.h's).
bool TryDispatch(ID3D11DeviceContext* c, UINT x, UINT y, UINT z, DispFn next) {
  if (!g_live.load(std::memory_order_relaxed) || c != chainfast::g_ctx || t_inside) return false;
  ID3D11ComputeShader* cur = chainfast::g_curCs;
  const int k = KindOf(cur);
  if (k < 0) {
    if (g_identOn.load(std::memory_order_relaxed)) Classify(c, x, y, z);
    return false;
  }
  if (!((g_mask.load(std::memory_order_relaxed) >> k) & 1) || !chainfast::g_setOrig) return false;
  ID3D11ComputeShader* var = nullptr;
  UINT gx = 0;
  if (k == kGrassCull) {
    if (x != 1 || y != 1 || z != 1) return false;
    var = g_grassVar.load(std::memory_order_acquire);
    gx = kGrassRows;
  } else {
    if (y != 1 || z != 1 || x < kClearMin) return false;
    var = ClearVariant(x);
    if (!var) {
      g_wantN.store(x, std::memory_order_relaxed);
      return false;
    }
    gx = (x + 63) / 64;
  }
  if (!var) return false;
  // The CSSetShader hook can miss binds while d3d11 has its own entry back: confirm what is bound.
  ID3D11ComputeShader* bound = nullptr;
  c->CSGetShader(&bound, nullptr, nullptr);
  if (bound) bound->Release();
  if (bound != cur) return false;
  const bool ver = g_verify.load(std::memory_order_relaxed) && VerifyPre(c, k);
  chainfast::g_setOrig(c, var, nullptr, 0);
  t_inside = true;
  next(c, gx, 1, 1);
  t_inside = false;
  chainfast::g_setOrig(c, cur, nullptr, 0);
  if (ver) VerifyPost(c, k, x, next);
  g_subs[k].fetch_add(1, std::memory_order_relaxed);
  return true;
}

void STDMETHODCALLTYPE H_Dispatch(ID3D11DeviceContext* c, UINT x, UINT y, UINT z) {
  const DispFn next = reinterpret_cast<DispFn>(g_orig);
  if (!TryDispatch(c, x, y, z, next)) next(c, x, y, z);
}

// Our Dispatch hook goes back in where d3d11 wrote its own entry; another hook of this payload on top
// (gpu_pass_timing.h's, chain_fast.h's identification probe) is left alone: the former calls TryDispatch,
// under the latter DCS's dispatches simply run stock.
void Rearm(ID3D11DeviceContext* c) {
  if (!g_vt || g_slot < 0 || *reinterpret_cast<void***>(c) != g_vt) return;
  void* cur = g_vt[g_slot];
  if (cur == reinterpret_cast<void*>(&H_Dispatch) || InThisModule(cur)) return;
  g_orig = cur;
  d3ds::WriteTablePointer(&g_vt[g_slot], reinterpret_cast<void*>(&H_Dispatch));
  g_rearms.fetch_add(1, std::memory_order_relaxed);
}

// chain_fast.h's CSSetShader hook calls this for DCS's context, before the bind.
void OnSet(ID3D11DeviceContext* c, ID3D11ComputeShader* s) {
  if (g_fpLive.load(std::memory_order_relaxed)) FpOnRender(c);
  if (g_pending.load(std::memory_order_relaxed) > 0) ReadChecks(c);
  if (!g_live.load(std::memory_order_relaxed)) return;
  if (g_identOn.load(std::memory_order_relaxed) || KindOf(s) >= 0) Rearm(c);
}

void Withdraw() {
  if (g_vt && g_slot >= 0 && g_vt[g_slot] == reinterpret_cast<void*>(&H_Dispatch))
    d3ds::WriteTablePointer(&g_vt[g_slot], g_orig);
}

uint32_t UsableMask() {
  uint32_t m = 0;
  if (g_nStock[kGrassCull].load() && g_grassVar.load()) m |= 1u << kGrassCull;
  if (g_nStock[kImClear].load()) m |= 1u << kImClear;  // variants per group count, on demand
  return m;
}
void LogCounters(const char* tag) {
  Log("shader fast (%s): mask 0x%x usable 0x%x, substitutions grass %llu, clear %llu; dispatch hook re-armed %llu "
      "times",
      tag, g_mask.load(), UsableMask(), static_cast<unsigned long long>(g_subs[kGrassCull].load()),
      static_cast<unsigned long long>(g_subs[kImClear].load()), static_cast<unsigned long long>(g_rearms.load()));
}

// Worker thread, every second. mask = [D3D] ShaderFast (0 = off: TryDispatch passes everything through).
void Apply(uint32_t mask) {
  mask &= (1u << kKinds) - 1;
  g_mask = mask;
  if (!mask) {
    g_live = false;
    return;
  }
  if (!chainfast::InstallSet()) return;  // DCS's device not known yet
  if (!g_vt) {
    g_slot = DcsQv_ContextSlot("Dispatch");
    if (g_slot < 0 || !chainfast::g_ctx) return;
    g_vt = *reinterpret_cast<void***>(chainfast::g_ctx);
    chainfast::g_onSet = &OnSet;
    Log("shader fast: mask 0x%x; Dispatch hook re-armed from the CSSetShader hook", mask);
  }
  if (mask & (1u << kGrassCull)) PrepareGrass();
  PrepareClear();
  g_live = true;
  IdentTick(mask);
  static int last = -1;
  const int act = static_cast<int>(UsableMask() & mask);
  if (act != last) {
    last = act;
    LogCounters("state");
  }
}

// ---------------------------------------------------------------------------
// Suite phases (suite thread)
// ---------------------------------------------------------------------------
void TimingPhase(uint32_t cfgMask, std::atomic<uint64_t>& frameCounter) {
  const uint32_t want = cfgMask ? cfgMask : 0x3;
  Apply(want);
  if (!(UsableMask() & want)) {
    Log("  shader fast: no identified kernel (identification still running, or DCS's device unknown); phase "
        "skipped");
    LogCounters("timing skipped");
    Apply(cfgMask);
    return;
  }
  for (int i = 0; i < 5; ++i) {
    const bool on = (i & 1) != 0;
    g_mask = on ? want : 0;
    Sleep(1500);
    Log("  shader fast timing run %d: %s", i + 1, on ? "ON (wide variants)" : "OFF (stock)");
    gpt::Measure(5000, frameCounter);
    if (on) LogCounters("timing run");
  }
  g_mask = cfgMask;
  Apply(cfgMask);
}
void VerifyPhase(int seconds, uint32_t cfgMask, std::atomic<uint64_t>& frameCounter) {
  const uint32_t want = cfgMask ? cfgMask : 0x3;
  Apply(want);
  if (!(UsableMask() & want)) {
    Log("  shader fast verify: no identified kernel; phase skipped");
    Apply(cfgMask);
    return;
  }
  for (int k = 0; k < kKinds; ++k) g_checks[k] = g_diff[k] = 0;
  g_skipped = 0;
  g_verify = true;
  gpt::Measure(seconds * 1000, frameCounter);
  Sleep(500);  // the last slots are read back by the render thread
  g_verify = false;
  Log("  shader fast verify: grass %llu checks, %llu differ (count, cell set, untouched words); clear %llu checks, "
      "%llu differ (every word); %llu skipped",
      static_cast<unsigned long long>(g_checks[kGrassCull].load()),
      static_cast<unsigned long long>(g_diff[kGrassCull].load()),
      static_cast<unsigned long long>(g_checks[kImClear].load()),
      static_cast<unsigned long long>(g_diff[kImClear].load()), static_cast<unsigned long long>(g_skipped.load()));
  Apply(cfgMask);
}

void Shutdown() {
  g_live = false;
  g_mask = 0;
  g_verify = false;
  g_fpLive = false;
  g_identOn = false;
  void (*me)(ID3D11DeviceContext*, ID3D11ComputeShader*) = &OnSet;
  chainfast::g_onSet.compare_exchange_strong(me, nullptr);
  Withdraw();
  Sleep(50);  // a dispatch already inside the hook finishes
  for (int k = 0; k < kKinds; ++k) {
    for (int i = 0; i < g_nStock[k].load(); ++i)
      if (ID3D11ComputeShader* s = g_stock[k][i].exchange(nullptr)) s->Release();
    g_nStock[k] = 0;
  }
  if (ID3D11ComputeShader* v = g_grassVar.exchange(nullptr)) v->Release();
  for (int i = 0; i < g_nClear.load(); ++i)
    if (g_clear[i].cs) g_clear[i].cs->Release(), g_clear[i].cs = nullptr;
  g_nClear = 0;
  if (FpJob* j = g_fpJob.exchange(nullptr)) {
    j->ReleaseGpu();
    if (j->cand.cs) j->cand.cs->Release();
    g_fpDone.push_back(j);
  }
  for (FpJob* j : g_fpDone) delete j;
  g_fpDone.clear();
  {
    std::lock_guard<std::mutex> lock(g_candMu);
    for (Cand& c : g_cands) c.cs->Release();
    g_cands.clear();
  }
  for (auto& a : g_tried) a = nullptr;
  for (VSlot& k : g_ring) {
    for (auto*& s : k.st)
      if (s) s->Release(), s = nullptr;
    k.live = false;
  }
  g_pending = 0;
  g_wantN = 0;
  for (auto& kv : g_scratch) {
    if (kv.second.u) kv.second.u->Release();
    if (kv.second.b) kv.second.b->Release();
  }
  g_scratch.clear();
  g_found = 0;
  g_identDone = false;
  g_candTested = g_candRejected = 0;
  g_vt = nullptr;
  g_slot = -1;
  g_orig = nullptr;
}

}  // namespace sfast
