// Shorter terrain/forest GPU culling chains ([D3D] ChainFast, default 7 =
// all three kernels; 0 = off; R34). Three of DCS's chain kernels are swapped for
// equivalent kernels with a shorter critical path; nothing else changes.
// With the key at 0 nothing is installed.
//
// What and why [V shaders from Bazar\shaders\fxo\fxo.edcz (copies), M census
// 2026-10-11 03:13, M offline harness 2026-10-15; R34 §2-§4]:
// - Every camera (4 cascades, the height map, 4 views) runs one IM2 culling
//   chain of 16-22 dependent dispatches on the immediate context
//   (edterrainGraphics41.dll). The chain is serial: each step reads the
//   previous step's UAV through an SRV, a UAV or the indirect arguments.
// - instancemanager2.calcoffsets: one 256-thread group scans the 19,527
//   per-object counts serially (77-element chunks per thread, then a 255-step
//   scan by thread 0): 35 us in every chain. Variant (kCalcOffsets): one
//   1024-thread group, independent loads, a log-step group scan: 11 us.
//   Integer adds only (associative mod 2^32), same write set, so the output is
//   bit-identical for every input [M 16/16 cases vs DCS's DXBC, GPU and WARP].
// - instancemanager2.calccounters: one InterlockedAdd per instance on the
//   counter of its object id; nearly all instances share one counter (the
//   census times, 72 us at 168 k instances, match the harness only with a
//   single hot counter), so the atomics serialise. Variant (kCalcCounters):
//   the same 128-thread groups and grid; each group ranks its instances in
//   groupshared memory and issues one atomic per distinct id: 6.8 us. The
//   counts are bit-identical; the per-instance ranks are a permutation within
//   each object, exactly the freedom the stock kernel has: two stock runs on
//   the same input differ in 99.99 % of the rank words [M]. Every downstream
//   total (offsets, cluster counts, indirect arguments) is unchanged; only
//   the order of instances inside an object's range differs, as it does from
//   stock run to stock run. Integer only.
// - airflowcomputetiles computeOffsets (once per frame, CascadeShadowPass0):
//   the same serial scan pattern over 10,000 counts: 23 -> 6.6 us,
//   bit-identical (kAirflowOffsets).
//
// Mechanism: ID3D11Device::CreateComputeShader is hooked in d3d11's shared
// device table (every device of the process uses the same read-only table;
// the hook only acts on the three stock instruction streams, matched by the
// FNV-1a 64 of their SHEX chunk). For a match the variant is created on the
// same device, the stock object is pinned (AddRef: its address keeps naming
// it) and its bytecode kept. Before a pair is used, the worker runs a self
// test on a private device of the same adapter: DCS's stock bytecode and the
// variant on synthetic inputs (bit-identical / same counts and a valid
// permutation, plus a CPU reference). Then DCS's immediate context gets a
// CSSetShader hook: when DCS binds a tested stock kernel whose kind is
// enabled, the variant is bound instead. Dispatches, bindings and every
// other call are DCS's own. Toggle at runtime: the mask is read every call.
//
// Limits: DCS usually creates the terrain compute effects before the payload
// loads, so the stock kernels are mostly identified at bind time by their
// output (see "Identification at bind time" below); this also covers a hot
// reload. The swap is re-armed by split_filter.h's re-hook every frame; with
// [D3D] SplitFilter=0 only the once-per-second loop re-arms it and the swap is
// mostly inactive. A DCS update that changes the kernels leaves them stock.
// [Suite] GpuDedup also hooks CSSetShader and re-arms after every call it
// forwards; ours is never put back over another hook of this payload (R48: the
// two took each other as the original, a call cycle), so while the census runs
// DCS's binds may pass ours (stock kernels, counted as "re-arm refused").
//
// Live checks (render thread, only inside a GPU pass timing session):
// [Suite] ChainFastVerify=N runs N seconds with a same-frame check of every
// calcoffsets and calccounters dispatch: DCS's result against the stock
// kernel re-run on copies of the pre-dispatch buffers (offsets bit for bit;
// counts bit for bit, ranks: in range and per-object sums of rank and rank^2
// equal). [Suite] ChainFastTiming=1: GPU pass timing OFF/ON/OFF/ON/OFF.
// Included once from main.cpp after gpu_pass_timing.h, shadow_inst.h and
// split_filter.h.
#pragma once

extern "C" int DcsQv_DeviceSlot(const char* name);

namespace chainfast {

enum Kind : int { kCalcOffsets = 0, kCalcCounters = 1, kAirflowOffsets = 2, kKinds = 3 };

// Stock kernels [V fxo.edcz 2.9.30.28738: frames 1609, 1450, 85]: the SHEX
// chunk identifies the program; the whole-blob values are logged only.
struct StockSig {
  uint32_t shexBytes;
  uint64_t shexFnv;
  uint32_t blobBytes;
  uint64_t blobFnv;
  const char* name;
};
constexpr StockSig kStock[kKinds] = {
    {1372, 0x3a961ecf10c7e115ull, 2024, 0xc8b2a05b6a81e1a7ull, "instancemanager2.calcoffsets"},
    {528, 0x56f4964d4f876cfdull, 1780, 0xf565e9b09c8023c3ull, "instancemanager2.calccounters"},
    {1320, 0x6316a28a1fd496ccull, 1968, 0xdc6442a28065e94cull, "airflowcomputetiles.computeOffsets"}};

inline uint64_t Fnv64(const uint8_t* p, size_t n) {
  uint64_t h = 0xcbf29ce484222325ull;
  for (size_t i = 0; i < n; ++i) h = (h ^ p[i]) * 0x100000001b3ull;
  return h;
}

// The SHEX (or SHDR) chunk payload of a DXBC container; false if malformed.
inline bool ShexChunk(const void* code, size_t n, const uint8_t** out, uint32_t* len) {
  const uint8_t* b = static_cast<const uint8_t*>(code);
  if (!b || n < 32 || memcmp(b, "DXBC", 4) != 0) return false;
  uint32_t total, chunks;
  memcpy(&total, b + 24, 4);
  memcpy(&chunks, b + 28, 4);
  if (total > n || chunks > 64 || 32 + 4ull * chunks > n) return false;
  for (uint32_t i = 0; i < chunks; ++i) {
    uint32_t off, sz;
    memcpy(&off, b + 32 + 4 * i, 4);
    if (off + 8ull > n) return false;
    memcpy(&sz, b + off + 4, 4);
    if (off + 8ull + sz > n) return false;
    if (!memcmp(b + off, "SHEX", 4) || !memcmp(b + off, "SHDR", 4)) {
      *out = b + off + 8;
      *len = sz;
      return true;
    }
  }
  return false;
}

// Tests only: an extra SHEX hash per kind treated as that stock kernel (0 = none).
std::atomic<uint64_t> g_testShexFnv[kKinds] = {};

// Kind of a stock kernel, or -1.
inline int MatchStock(const void* code, size_t n) {
  const uint8_t* p = nullptr;
  uint32_t len = 0;
  if (!ShexChunk(code, n, &p, &len)) return -1;
  const uint64_t h = Fnv64(p, len);
  for (int k = 0; k < kKinds; ++k)
    if ((len == kStock[k].shexBytes && h == kStock[k].shexFnv) || (g_testShexFnv[k].load() && h == g_testShexFnv[k].load()))
      return k;
  return -1;
}

// ---------------------------------------------------------------------------
// Variants (same constant buffers, bindings and dispatch shape as stock)
// ---------------------------------------------------------------------------
const char kSrcCalcOffsets[] = R"(
cbuffer Globals : register(b0) { uint chunkSize; uint counts; };
Buffer<uint> countsIn : register(t0);
RWBuffer<uint> offsetsOut : register(u0);
#define T 1024
#define KMAX 32
groupshared uint s[T];
groupshared uint g0[256];
groupshared uint g1[256];
void StockPort(uint t) {
  bool act = t < 256;
  uint c = chunkSize, n = counts;
  if (act) {
    uint i0 = t * c;
    g0[t] = 0;
    if (i0 < n) { offsetsOut[i0] = 0; g0[t] = countsIn[i0]; }
    [loop] for (uint j = 1; j < c; ++j) {
      uint idx = t * c + j;
      if (idx >= n) break;
      uint run = g0[t];
      offsetsOut[idx] = run;
      g0[t] = countsIn[idx] + run;
    }
  }
  GroupMemoryBarrierWithGroupSync();
  if (t == 0) {
    g1[0] = 0;
    [loop] for (uint k = 1; k < 256; ++k) g1[k] = g0[k - 1] + g1[k - 1];
  }
  GroupMemoryBarrierWithGroupSync();
  if (act) {
    [loop] for (uint j = 0; j < c; ++j) {
      uint idx = t * c + j;
      if (idx >= n) break;
      offsetsOut[idx] = g1[t] + offsetsOut[idx];
    }
  }
}
[numthreads(T, 1, 1)]
void main(uint tid : SV_GroupIndex) {
  uint c = chunkSize, n = counts;
  if (c == 0) { if (tid == 0 && n > 0) offsetsOut[0] = 0; return; }
  if (c >= (1u << 24)) { StockPort(tid); return; }
  uint M = min(n, 256u * c);
  uint K = M / T + ((M % T) != 0 ? 1u : 0u);
  uint base = tid * K;
  uint sum = 0;
  uint v[KMAX];
  if (K <= KMAX) {
    [unroll] for (uint j = 0; j < KMAX; ++j) {
      uint idx = base + j;
      v[j] = (j < K && idx < M) ? countsIn[idx] : 0u;
      sum += v[j];
    }
  } else {
    [loop] for (uint j = 0; j < K; ++j) { uint idx = base + j; if (idx < M) sum += countsIn[idx]; }
  }
  s[tid] = sum;
  GroupMemoryBarrierWithGroupSync();
  [unroll] for (uint d = 1; d < T; d <<= 1) {
    uint add = tid >= d ? s[tid - d] : 0u;
    GroupMemoryBarrierWithGroupSync();
    s[tid] += add;
    GroupMemoryBarrierWithGroupSync();
  }
  uint run = s[tid] - sum;
  if (K <= KMAX) {
    [unroll] for (uint j = 0; j < KMAX; ++j) {
      uint idx = base + j;
      if (j < K && idx < M) { offsetsOut[idx] = run; run += v[j]; }
    }
  } else {
    [loop] for (uint j = 0; j < K; ++j) {
      uint idx = base + j;
      if (idx < M) { offsetsOut[idx] = run; run += countsIn[idx]; }
    }
  }
}
)";

const char kSrcCalcCounters[] = R"(
cbuffer Globals : register(b0) { int mainBufferSize; int countsBufferSize; };
struct Inst { uint4 a, b, c, d; };
Buffer<uint> sumsBuffer : register(t0);
RWStructuredBuffer<Inst> mainBuffer : register(u0);
RWBuffer<uint> indiciesNoOffsets : register(u1);
RWBuffer<uint> countsBuffer : register(u2);
groupshared uint key[128];
groupshared uint gbase[128];
[numthreads(128, 1, 1)]
void main(uint3 gid : SV_GroupID, uint tid : SV_GroupIndex) {
  uint i = gid.x * 128u + tid;
  bool active = i < sumsBuffer[0] && i < (uint)mainBufferSize;
  uint obj = 0xffffffffu;
  bool valid = false;
  if (active) {
    obj = mainBuffer[i].a.x & 0x00ffffffu;
    valid = (int)obj < countsBufferSize;
  }
  key[tid] = valid ? obj : 0xffffffffu;
  GroupMemoryBarrierWithGroupSync();
  uint local = 0, total = 0, leader = tid;
  if (valid) {
    [loop] for (uint k = 0; k < 128; ++k) {
      if (key[k] == obj) {
        if (k < tid) { if (local == 0) leader = k; ++local; }
        ++total;
      }
    }
    if (local == 0) {
      uint b;
      InterlockedAdd(countsBuffer[obj], total, b);
      gbase[tid] = b;
    }
  }
  GroupMemoryBarrierWithGroupSync();
  if (active) indiciesNoOffsets[i] = valid ? gbase[leader] + local : i;
}
)";

const char kSrcAirflowOffsets[] = R"(
StructuredBuffer<uint> countsIn : register(t0);
RWStructuredBuffer<uint> offsetsOut : register(u0);
#define T 1024
#define N 10000
#define K 10
groupshared uint s[T];
[numthreads(T, 1, 1)]
void main(uint tid : SV_GroupIndex) {
  uint base = tid * K;
  uint v[K];
  uint sum = 0;
  [unroll] for (uint j = 0; j < K; ++j) { uint idx = base + j; v[j] = idx < N ? countsIn[idx] : 0u; sum += v[j]; }
  s[tid] = sum;
  GroupMemoryBarrierWithGroupSync();
  [unroll] for (uint d = 1; d < T; d <<= 1) {
    uint add = tid >= d ? s[tid - d] : 0u;
    GroupMemoryBarrierWithGroupSync();
    s[tid] += add;
    GroupMemoryBarrierWithGroupSync();
  }
  uint run = s[tid] - sum;
  [unroll] for (uint q = 0; q < K; ++q) { uint idx = base + q; if (idx < N) { offsetsOut[idx] = run; run += v[q]; } }
}
)";

// Live check helpers (verify phase only).
// Element-wise compare of two typed uint UAVs over [0, n): mismatches -> R[slot].
const char kSrcCmp[] = R"(
cbuffer G : register(b0) { uint n; uint slot; };
RWBuffer<uint> A : register(u0);
RWBuffer<uint> B : register(u1);
RWByteAddressBuffer R : register(u2);
[numthreads(64, 1, 1)]
void main(uint3 t : SV_DispatchThreadID) {
  if (t.x < n && A[t.x] != B[t.x]) { uint o; R.InterlockedAdd(slot * 4, 1, o); }
}
)";
// calccounters ranks: per object the sums of rank and rank^2 (mod 2^32) of
// DCS's result minus the stock re-run's, ranks in range, invalid ids keep i.
const char kSrcRankAcc[] = R"(
cbuffer Dcs : register(b0) { int mainBufferSize; int countsBufferSize; };
cbuffer G : register(b1) { uint n; uint slot; };
struct Inst { uint4 a, b, c, d; };
Buffer<uint> sumsBuffer : register(t0);
RWStructuredBuffer<Inst> mainBuffer : register(u0);
RWBuffer<uint> rankA : register(u1);
RWBuffer<uint> rankB : register(u2);
RWBuffer<uint> counts : register(u3);
RWBuffer<uint> accS : register(u4);
RWBuffer<uint> accQ : register(u5);
RWByteAddressBuffer R : register(u6);
[numthreads(128, 1, 1)]
void main(uint3 t : SV_DispatchThreadID) {
  uint i = t.x;
  if (i >= sumsBuffer[0] || i >= (uint)mainBufferSize) return;
  uint obj = mainBuffer[i].a.x & 0x00ffffffu;
  uint a = rankA[i], b = rankB[i];
  uint o;
  if ((int)obj >= countsBufferSize) { if (a != i || b != i) R.InterlockedAdd(slot * 4, 1, o); return; }
  if (a >= counts[obj]) R.InterlockedAdd(slot * 4, 1, o);
  InterlockedAdd(accS[obj], a - b, o);
  InterlockedAdd(accQ[obj], a * a - b * b, o);
}
)";
const char kSrcRankFin[] = R"(
cbuffer G : register(b0) { uint n; uint slot; };
RWBuffer<uint> accS : register(u0);
RWBuffer<uint> accQ : register(u1);
RWByteAddressBuffer R : register(u2);
[numthreads(64, 1, 1)]
void main(uint3 t : SV_DispatchThreadID) {
  if (t.x < n && (accS[t.x] != 0 || accQ[t.x] != 0)) { uint o; R.InterlockedAdd(slot * 4, 1, o); }
}
)";

// ---------------------------------------------------------------------------
// Compiler (d3dcompiler_47 from System32, as fixation_depth.h)
// ---------------------------------------------------------------------------
pD3DCompile g_compile = nullptr;
bool LoadCompiler() {
  if (g_compile) return true;
  wchar_t sys[MAX_PATH];
  const UINT n = GetSystemDirectoryW(sys, MAX_PATH);
  if (!n || n >= MAX_PATH) return false;
  HMODULE m = LoadLibraryW((std::wstring(sys) + L"\\d3dcompiler_47.dll").c_str());
  if (!m) return false;
  g_compile = reinterpret_cast<pD3DCompile>(GetProcAddress(m, "D3DCompile"));
  return g_compile != nullptr;
}
bool Compile(const char* src, const char* name, std::vector<uint8_t>& out, std::string* err) {
  if (!LoadCompiler()) {
    if (err) *err = "d3dcompiler_47.dll / D3DCompile not available";
    return false;
  }
  ID3DBlob* code = nullptr;
  ID3DBlob* e = nullptr;
  const HRESULT hr = g_compile(src, strlen(src), name, nullptr, nullptr, "main", "cs_5_0",
                               D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &e);
  if (FAILED(hr) || !code) {
    if (err) *err = e ? std::string(static_cast<const char*>(e->GetBufferPointer()), e->GetBufferSize()) : "D3DCompile failed";
    if (e) e->Release();
    if (code) code->Release();
    return false;
  }
  if (e) e->Release();
  const uint8_t* p = static_cast<const uint8_t*>(code->GetBufferPointer());
  out.assign(p, p + code->GetBufferSize());
  code->Release();
  return true;
}

std::vector<uint8_t> g_var[kKinds];
std::atomic<bool> g_varReady{false};
std::string g_varErr;
const char* const kVarSrc[kKinds] = {kSrcCalcOffsets, kSrcCalcCounters, kSrcAirflowOffsets};
bool CompileVariants() {
  if (g_varReady.load()) return true;
  for (int k = 0; k < kKinds; ++k)
    if (!Compile(kVarSrc[k], kStock[k].name, g_var[k], &g_varErr)) return false;
  g_varReady = true;
  return true;
}

// ---------------------------------------------------------------------------
// Self test: stock bytecode vs variant on synthetic inputs (any device; the
// worker uses a private device of DCS's adapter, the tests WARP)
// ---------------------------------------------------------------------------
struct TBuf {
  ID3D11Buffer* b = nullptr;
  ID3D11ShaderResourceView* srv = nullptr;
  ID3D11UnorderedAccessView* uav = nullptr;
  UINT bytes = 0;
  void Release() {
    if (srv) srv->Release();
    if (uav) uav->Release();
    if (b) b->Release();
    srv = nullptr;
    uav = nullptr;
    b = nullptr;
  }
};
// stride 0: typed R32_UINT, else structured.
inline bool MakeBuf(ID3D11Device* d, UINT elems, UINT stride, const void* init, bool srv, bool uav, TBuf& r) {
  const UINT es = stride ? stride : 4;
  r.bytes = elems * es;
  D3D11_BUFFER_DESC bd{};
  bd.ByteWidth = r.bytes;
  bd.Usage = D3D11_USAGE_DEFAULT;
  bd.BindFlags = (srv ? D3D11_BIND_SHADER_RESOURCE : 0) | (uav ? D3D11_BIND_UNORDERED_ACCESS : 0);
  bd.MiscFlags = stride ? D3D11_RESOURCE_MISC_BUFFER_STRUCTURED : 0;
  bd.StructureByteStride = stride;
  D3D11_SUBRESOURCE_DATA sd{init, 0, 0};
  if (FAILED(d->CreateBuffer(&bd, init ? &sd : nullptr, &r.b))) return false;
  if (srv) {
    D3D11_SHADER_RESOURCE_VIEW_DESC v{};
    v.Format = stride ? DXGI_FORMAT_UNKNOWN : DXGI_FORMAT_R32_UINT;
    v.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
    v.Buffer.NumElements = elems;
    if (FAILED(d->CreateShaderResourceView(r.b, &v, &r.srv))) return false;
  }
  if (uav) {
    D3D11_UNORDERED_ACCESS_VIEW_DESC v{};
    v.Format = stride ? DXGI_FORMAT_UNKNOWN : DXGI_FORMAT_R32_UINT;
    v.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    v.Buffer.NumElements = elems;
    if (FAILED(d->CreateUnorderedAccessView(r.b, &v, &r.uav))) return false;
  }
  return true;
}
inline bool ReadBuf(ID3D11Device* d, ID3D11DeviceContext* c, ID3D11Buffer* b, UINT bytes, std::vector<uint32_t>& out) {
  D3D11_BUFFER_DESC bd{};
  bd.ByteWidth = bytes;
  bd.Usage = D3D11_USAGE_STAGING;
  bd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  ID3D11Buffer* s = nullptr;
  if (FAILED(d->CreateBuffer(&bd, nullptr, &s))) return false;
  D3D11_BOX box{0, 0, 0, bytes, 1, 1};
  c->CopySubresourceRegion(s, 0, 0, 0, 0, b, 0, &box);
  // R90 F4: polled with DO_NOT_WAIT (yielding, then 1 ms sleeps) instead of a blocking Map: the worker never
  // waits inside the driver for the GPU, and the verdict only arrives a little later. 5 s bound (a lost device).
  c->Flush();
  D3D11_MAPPED_SUBRESOURCE m{};
  HRESULT hr = E_FAIL;
  const ULONGLONG t0 = GetTickCount64();
  for (int i = 0;; ++i) {
    hr = c->Map(s, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m);
    if (hr != DXGI_ERROR_WAS_STILL_DRAWING || GetTickCount64() - t0 > 5000) break;
    Sleep(i < 64 ? 0 : 1);
  }
  const bool ok = SUCCEEDED(hr);
  if (ok) {
    out.resize(bytes / 4);
    memcpy(out.data(), m.pData, bytes);
    c->Unmap(s, 0);
  }
  s->Release();
  return ok;
}
inline ID3D11Buffer* MakeCb(ID3D11Device* d, const void* p, UINT n) {
  D3D11_BUFFER_DESC bd{};
  bd.ByteWidth = (n + 15) & ~15u;
  bd.Usage = D3D11_USAGE_DEFAULT;
  bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
  uint8_t tmp[256] = {};
  memcpy(tmp, p, n);
  D3D11_SUBRESOURCE_DATA sd{tmp, 0, 0};
  ID3D11Buffer* b = nullptr;
  d->CreateBuffer(&bd, &sd, &b);
  return b;
}
inline void BindCs(ID3D11DeviceContext* c, ID3D11ComputeShader* cs, ID3D11Buffer* cb, ID3D11ShaderResourceView* srv,
                   ID3D11UnorderedAccessView* const* uavs, UINT nu) {
  ID3D11UnorderedAccessView* nul[8] = {};
  c->CSSetUnorderedAccessViews(0, 8, nul, nullptr);
  c->CSSetShader(cs, nullptr, 0);
  c->CSSetConstantBuffers(0, 1, &cb);
  c->CSSetShaderResources(0, 1, &srv);
  c->CSSetUnorderedAccessViews(0, nu, uavs, nullptr);
}
inline void UnbindCs(ID3D11DeviceContext* c) {
  ID3D11UnorderedAccessView* nul[8] = {};
  ID3D11ShaderResourceView* ns = nullptr;
  ID3D11Buffer* nb = nullptr;
  c->CSSetUnorderedAccessViews(0, 8, nul, nullptr);
  c->CSSetShaderResources(0, 1, &ns);
  c->CSSetConstantBuffers(0, 1, &nb);
  c->CSSetShader(nullptr, nullptr, 0);
}

// Pseudo-random (deterministic) for the synthetic inputs.
struct Rng {
  uint64_t s;
  uint32_t operator()() {
    s = s * 6364136223846793005ull + 1442695040888963407ull;
    return static_cast<uint32_t>(s >> 32);
  }
};

// CPU transcription of the stock scan (calcoffsets / computeOffsets) for
// chunk sizes below 2^24 (chunks never overlap): `lanes` threads, chunk c,
// bound n, counts readable below srvLen (0 beyond, as D3D11 returns).
inline void StockScanRef(uint32_t lanes, uint32_t c, uint32_t n, const std::vector<uint32_t>& cnt, uint32_t srvLen,
                         std::vector<uint32_t>& out) {
  auto at = [&](uint32_t i) { return i < srvLen && i < cnt.size() ? cnt[i] : 0u; };
  std::vector<uint32_t> g0(lanes, 0), g1(lanes, 0);
  for (uint32_t t = 0; t < lanes; ++t) {
    const uint32_t i0 = t * c;
    if (i0 < n) {
      if (i0 < out.size()) out[i0] = 0;
      g0[t] = at(i0);
    }
    for (uint32_t j = 1; j < c; ++j) {
      const uint32_t idx = t * c + j;
      if (idx >= n) break;
      if (idx < out.size()) out[idx] = g0[t];
      g0[t] += at(idx);
    }
  }
  for (uint32_t k = 1; k < lanes; ++k) g1[k] = g1[k - 1] + g0[k - 1];
  for (uint32_t t = 0; t < lanes; ++t)
    for (uint32_t j = 0; j < c; ++j) {
      const uint32_t idx = t * c + j;
      if (idx >= n) break;
      if (idx < out.size()) out[idx] += g1[t];
    }
}

// Set while our own tests create shaders: the creation hook ignores them
// (the private test device shares d3d11's device table).
thread_local bool t_ownCreate = false;
struct OwnCreate {
  bool prev;
  OwnCreate() : prev(t_ownCreate) { t_ownCreate = true; }
  ~OwnCreate() { t_ownCreate = prev; }
};

struct SelfTestResult {
  bool ok = false;
  int cases = 0, stockCases = 0, failed = 0;
  std::string first;
};

// One kernel: variant against the CPU reference on every case and, when the
// stock bytecode is given, against the stock kernel run on the same device.
inline SelfTestResult SelfTest(ID3D11Device* d, ID3D11DeviceContext* c, int kind, const void* stock, size_t stockLen,
                               const std::vector<uint8_t>& var) {
  OwnCreate own;
  SelfTestResult r;
  ID3D11ComputeShader* sv = nullptr;
  ID3D11ComputeShader* ss = nullptr;
  if (FAILED(d->CreateComputeShader(var.data(), var.size(), nullptr, &sv))) {
    r.first = "variant not creatable";
    return r;
  }
  if (stock && FAILED(d->CreateComputeShader(stock, stockLen, nullptr, &ss))) {
    sv->Release();
    r.first = "stock not creatable";
    return r;
  }
  auto fail = [&](const char* what) {
    ++r.failed;
    if (r.first.empty()) r.first = what;
  };
  Rng rng{0x5eedull + kind};
  if (kind == kCalcOffsets || kind == kAirflowOffsets) {
    struct Case {
      uint32_t n, c, srvLen, outLen;
      int dist;
    };
    std::vector<Case> cases;
    if (kind == kCalcOffsets)
      cases = {{19527, 77, 19527, 19527, 0}, {19527, 77, 19527, 19527, 1}, {19527, 77, 19527, 19527, 2},
               {19527, 50, 19527, 19527, 0}, {19712, 77, 19712, 19712, 0}, {1, 1, 1, 4, 0},
               {0, 0, 4, 4, 0},              {5, 0, 5, 8, 0},              {300, 2, 300, 600, 0},
               {40000, 157, 40000, 40000, 1}, {19527, 77, 10000, 19527, 0}, {70000, 300, 70000, 70000, 1},
               {257, 1, 257, 300, 0},        {3000, 1u << 24, 3000, 3000, 0}};
    else
      cases = {{10000, 79, 10000, 10000, 0}, {10000, 79, 10000, 10000, 1}, {10000, 79, 10000, 10112, 0},
               {10000, 79, 6000, 10000, 2}};
    for (const Case& k : cases) {
      std::vector<uint32_t> cnt(k.srvLen ? k.srvLen : 1);
      for (auto& v : cnt) v = k.dist == 0 ? rng() % 64 : k.dist == 1 ? rng() : ((rng() % 13) == 0 ? rng() % 5000 : 0);
      std::vector<uint32_t> sent(k.outLen);
      for (size_t i = 0; i < sent.size(); ++i) sent[i] = 0xA5A50000u ^ static_cast<uint32_t>(i * 2654435761u);
      const UINT stride = kind == kAirflowOffsets ? 4 : 0;
      TBuf in;
      if (!MakeBuf(d, static_cast<UINT>(cnt.size()), stride, cnt.data(), true, false, in)) {
        fail("buffer");
        in.Release();
        continue;
      }
      const uint32_t cbv[2] = {k.c, k.n};
      ID3D11Buffer* cb = kind == kCalcOffsets ? MakeCb(d, cbv, 8) : nullptr;
      std::vector<uint32_t> res[2];
      for (int v = 0; v < 2; ++v) {
        ID3D11ComputeShader* cs = v ? ss : sv;
        if (!cs) continue;
        TBuf out;
        if (MakeBuf(d, k.outLen, stride, sent.data(), false, true, out)) {
          BindCs(c, cs, cb, in.srv, &out.uav, 1);
          c->Dispatch(1, 1, 1);
          UnbindCs(c);
          ReadBuf(d, c, out.b, out.bytes, res[v]);
        }
        out.Release();
      }
      ++r.cases;
      if (k.c < (1u << 24)) {  // CPU reference
        std::vector<uint32_t> ref = sent;
        StockScanRef(kind == kCalcOffsets ? 256 : 128, k.c, k.n, cnt, k.srvLen, ref);
        if (res[0] != ref) fail(kind == kCalcOffsets ? "calcoffsets variant != CPU reference" : "airflow variant != CPU reference");
      }
      if (ss) {
        ++r.stockCases;
        if (res[0] != res[1]) fail(kind == kCalcOffsets ? "calcoffsets variant != stock" : "airflow variant != stock");
      }
      if (cb) cb->Release();
      in.Release();
    }
  } else if (kind == kCalcCounters) {
    const uint32_t nObj = 19527;
    struct Case {
      uint32_t n, limit;
      int dist;
    } cases[] = {{168000, 524288, 0}, {168000, 524288, 1}, {87000, 524288, 2}, {640, 524288, 3},
                 {5000, 3000, 1},     {1, 524288, 0},      {0, 524288, 0},     {20000, 524288, 4}};
    for (const Case& k : cases) {
      const uint32_t cap = (k.n ? k.n : 1) + 300;
      std::vector<uint32_t> objs(cap), main(cap * 16, 0);
      for (uint32_t i = 0; i < cap; ++i) {
        uint32_t id;
        switch (k.dist) {
          case 0: id = 4711; break;                                              // one hot object (the census case)
          case 1: id = (rng() % 100) < 95 ? 100 + rng() % 16 : rng() % nObj; break;
          case 2: id = rng() % nObj; break;
          case 3: id = (i / 64) % 7; break;
          default: id = (rng() % 50 == 0) ? nObj + rng() % 1000 : rng() % 64; break;  // invalid ids too
        }
        objs[i] = id | ((rng() & 0xff) << 24);
        main[i * 16] = objs[i];
        main[i * 16 + 1] = rng();
      }
      const uint32_t sums[8] = {k.n, 0, 0, 0, 0, 0, 0, 0};
      const int cbv[2] = {static_cast<int>(k.limit), static_cast<int>(nObj)};
      ID3D11Buffer* cb = MakeCb(d, cbv, 8);
      TBuf sb, mb;
      std::vector<uint32_t> sentI(cap), zeros(nObj, 0);
      for (uint32_t i = 0; i < cap; ++i) sentI[i] = 0xDEAD0000u + i;
      std::vector<uint32_t> ind[2], cnt[2];
      bool built = MakeBuf(d, 8, 0, sums, true, false, sb) && MakeBuf(d, cap, 64, main.data(), false, true, mb);
      for (int v = 0; v < 2 && built; ++v) {
        ID3D11ComputeShader* cs = v ? ss : sv;
        if (!cs) continue;
        TBuf ib, cbuf;
        if (MakeBuf(d, cap, 0, sentI.data(), false, true, ib) && MakeBuf(d, nObj, 0, zeros.data(), false, true, cbuf)) {
          ID3D11UnorderedAccessView* u[3] = {mb.uav, ib.uav, cbuf.uav};
          BindCs(c, cs, cb, sb.srv, u, 3);
          c->Dispatch((k.n >> 7) + 1, 1, 1);
          UnbindCs(c);
          ReadBuf(d, c, ib.b, ib.bytes, ind[v]);
          ReadBuf(d, c, cbuf.b, cbuf.bytes, cnt[v]);
        }
        ib.Release();
        cbuf.Release();
      }
      ++r.cases;
      // CPU reference: counts, untouched tail, i for invalid ids, ranks a permutation.
      const uint32_t lim = k.n < k.limit ? k.n : k.limit;
      std::vector<uint32_t> refCnt(nObj, 0);
      for (uint32_t i = 0; i < lim; ++i)
        if ((objs[i] & 0xffffff) < nObj) ++refCnt[objs[i] & 0xffffff];
      bool ok = cnt[0] == refCnt && ind[0].size() == cap;
      std::vector<std::vector<uint8_t>> seen(nObj);
      for (uint32_t o = 0; o < nObj && ok; ++o) seen[o].assign(refCnt[o], 0);
      for (uint32_t i = 0; i < cap && ok; ++i) {
        const uint32_t o = objs[i] & 0xffffff;
        if (i >= lim) ok = ind[0][i] == sentI[i];
        else if (o >= nObj) ok = ind[0][i] == i;
        else {
          const uint32_t rk = ind[0][i];
          ok = rk < refCnt[o] && !seen[o][rk];
          if (ok) seen[o][rk] = 1;
        }
      }
      if (!ok) fail("calccounters variant: counts or ranks wrong");
      if (ss) {
        ++r.stockCases;
        bool sok = cnt[1] == cnt[0] && ind[1].size() == cap;
        for (uint32_t i = 0; i < cap && sok; ++i)
          if (i >= lim || (objs[i] & 0xffffff) >= nObj) sok = ind[1][i] == ind[0][i];
        if (!sok) fail("calccounters variant != stock (counts, tail or invalid ids)");
      }
      sb.Release();
      mb.Release();
      if (cb) cb->Release();
    }
  }
  sv->Release();
  if (ss) ss->Release();
  r.ok = r.failed == 0 && r.cases > 0;
  return r;
}

// ---------------------------------------------------------------------------
// Pairs seen at creation
// ---------------------------------------------------------------------------
struct Entry {
  ID3D11Device* dev = nullptr;
  ID3D11ComputeShader* stock = nullptr;    // pinned (AddRef)
  ID3D11ComputeShader* variant = nullptr;  // ours
  int kind = -1;
  std::vector<uint8_t> code;  // DCS's stock bytecode
  std::atomic<int> state{0};  // 0 empty, 1 created (untested), 2 tested OK, -1 failed
};
constexpr int kMaxEntries = 16;
Entry g_e[kMaxEntries];
std::atomic<int> g_n{0};
std::mutex g_mu;

std::atomic<uint32_t> g_mask{0};  // enabled kinds (bit = 1 << kind)
std::atomic<uint64_t> g_seen[kKinds], g_subs[kKinds], g_misses{0};
// Load-order diagnostics (live R34): compute shaders created through the hook,
// stock-sized SHEX chunks with another hash, and which bound shaders the hook saw.
std::atomic<uint64_t> g_createCalls{0};
constexpr int kDiagCap = 4096;
std::atomic<void*> g_created[kDiagCap];  // open addressing, pointers created after the hook
std::atomic<void*> g_bound[kDiagCap];    // distinct shaders bound in the first kDiagBinds binds
constexpr uint64_t kDiagBinds = 50000;
std::atomic<uint64_t> g_bindCalls{0}, g_boundDistinct{0}, g_boundKnown{0};
std::atomic<uint64_t> g_near[8];  // (kind << 60) | (shex fnv >> 4): stock length, other hash
std::atomic<int> g_nearN{0};
inline size_t DiagSlot(void* p) { return (reinterpret_cast<uintptr_t>(p) >> 4) * 0x9E3779B97F4A7C15ull >> 52; }
inline bool DiagInsert(std::atomic<void*>* set, void* p) {  // true if newly inserted
  for (size_t i = DiagSlot(p), n = 0; n < kDiagCap; i = (i + 1) & (kDiagCap - 1), ++n) {
    void* cur = set[i].load(std::memory_order_relaxed);
    if (cur == p) return false;
    if (!cur) {
      void* exp = nullptr;
      if (set[i].compare_exchange_strong(exp, p)) return true;
      if (exp == p) return false;
    }
  }
  return false;
}
inline bool DiagHas(std::atomic<void*>* set, void* p) {
  for (size_t i = DiagSlot(p), n = 0; n < kDiagCap; i = (i + 1) & (kDiagCap - 1), ++n) {
    void* cur = set[i].load(std::memory_order_relaxed);
    if (cur == p) return true;
    if (!cur) return false;
  }
  return false;
}
std::atomic<int> g_state{0};  // 0 not installed, 1 creation hook in place
std::string g_why;           // why not installed

using CreateCsFn = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, const void*, SIZE_T, ID3D11ClassLinkage*,
                                               ID3D11ComputeShader**);
using SetCsFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11ComputeShader*, ID3D11ClassInstance* const*, UINT);
void** g_createSlot = nullptr;
CreateCsFn g_createOrig = nullptr;
void** g_setSlot = nullptr;
SetCsFn g_setOrig = nullptr;
ID3D11DeviceContext* g_ctx = nullptr;  // DCS's immediate context (AddRef)
ID3D11Device* g_dcsDev = nullptr;

HRESULT STDMETHODCALLTYPE H_CreateCs(ID3D11Device* d, const void* code, SIZE_T n, ID3D11ClassLinkage* link,
                                     ID3D11ComputeShader** out) {
  const HRESULT hr = g_createOrig(d, code, n, link, out);
  if (FAILED(hr) || !out || !*out || !code || link || t_ownCreate) return hr;
  g_createCalls.fetch_add(1, std::memory_order_relaxed);
  DiagInsert(g_created, *out);
  const int k = MatchStock(code, n);
  if (k < 0) {
    const uint8_t* p = nullptr;
    uint32_t len = 0;
    if (ShexChunk(code, n, &p, &len))
      for (int j = 0; j < kKinds; ++j)
        if (len == kStock[j].shexBytes) {
          const int at = g_nearN.fetch_add(1);
          if (at < 8) g_near[at] = (static_cast<uint64_t>(j) << 60) | (Fnv64(p, len) >> 4);
        }
    return hr;
  }
  g_seen[k].fetch_add(1);
  std::lock_guard<std::mutex> lock(g_mu);
  const int i = g_n.load();
  if (i >= kMaxEntries) return hr;
  Entry& e = g_e[i];
  // The variant is created later by the worker (TestPending): nothing slow here.
  e.dev = d;
  e.dev->AddRef();
  e.stock = *out;
  e.stock->AddRef();
  e.variant = nullptr;
  e.kind = k;
  e.code.assign(static_cast<const uint8_t*>(code), static_cast<const uint8_t*>(code) + n);
  e.state = 1;
  g_n.store(i + 1);
  return hr;
}

// Identification by output (below): render-thread steps run from the CSSetShader hook.
std::atomic<bool> g_identOn{false};  // probe hooks in place (Dispatch, DispatchIndirect)
std::atomic<bool> g_fpLive{false};   // a fingerprint job wants the render thread
ID3D11ComputeShader* g_curCs = nullptr;  // render thread: the shader DCS last bound (before substitution)
void IdentOnSet(ID3D11DeviceContext* c);
void FpOnSet(ID3D11DeviceContext* c);
// shader_fast.h (R36, [D3D] ShaderFast): called for DCS's context before each bind (its Dispatch hook re-arm).
std::atomic<void (*)(ID3D11DeviceContext*, ID3D11ComputeShader*)> g_onSet{nullptr};

void STDMETHODCALLTYPE H_CSSetShader(ID3D11DeviceContext* c, ID3D11ComputeShader* s, ID3D11ClassInstance* const* ci,
                                     UINT nci) {
  const uint32_t mask = g_mask.load(std::memory_order_relaxed);
  if (c == g_ctx) {
    g_curCs = s;
    if (g_identOn.load(std::memory_order_relaxed)) IdentOnSet(c);
    if (g_fpLive.load(std::memory_order_relaxed)) FpOnSet(c);
    if (auto f = g_onSet.load(std::memory_order_relaxed)) f(c, s);
  }
  if (s && g_bindCalls.load(std::memory_order_relaxed) < kDiagBinds) {
    g_bindCalls.fetch_add(1, std::memory_order_relaxed);
    if (DiagInsert(g_bound, s)) {
      g_boundDistinct.fetch_add(1, std::memory_order_relaxed);
      if (DiagHas(g_created, s)) g_boundKnown.fetch_add(1, std::memory_order_relaxed);
    }
  }
  if (mask && s) {
    const int n = g_n.load(std::memory_order_acquire);
    for (int i = 0; i < n; ++i) {
      Entry& e = g_e[i];
      if (e.stock == s) {
        if ((mask >> e.kind & 1) && e.state.load(std::memory_order_relaxed) == 2) {
          s = e.variant;
          g_subs[e.kind].fetch_add(1, std::memory_order_relaxed);
        }
        break;
      }
    }
  }
  g_setOrig(c, s, ci, nci);
}

// R90 F4: one private device (default adapter) for every self test of this module and shader_fast.h, created
// at payload start by the worker (main.cpp) and kept until Shutdown. Before, each round of tests created and
// destroyed a hardware device mid-flight (R87: 21:20:12-18 and after the reload), a driver-wide operation.
// Worker thread only (InstallCreate, TestPending, sfast::PrepareClear).
ID3D11Device* g_privDev = nullptr;
ID3D11DeviceContext* g_privCtx = nullptr;
std::atomic<uint32_t> g_privCreated{0};  // devices created (tests: one per session)
void ReleasePrivate() {
  if (g_privCtx) g_privCtx->Release();
  if (g_privDev) g_privDev->Release();
  g_privCtx = nullptr;
  g_privDev = nullptr;
}
bool AcquirePrivate(ID3D11Device** d = nullptr, ID3D11DeviceContext** c = nullptr) {
  if (g_privDev && g_privDev->GetDeviceRemovedReason() != S_OK) ReleasePrivate();  // removed: a new one
  if (!g_privDev) {
    D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &fl, 1, D3D11_SDK_VERSION, &g_privDev,
                                 nullptr, &g_privCtx)) ||
        !g_privDev || !g_privCtx) {
      ReleasePrivate();
      return false;
    }
    g_privCreated.fetch_add(1);
  }
  if (d) *d = g_privDev;
  if (c) *c = g_privCtx;
  return true;
}

// Creation hook in d3d11's shared device table, through a private device
// (every device of the process uses the same read-only table [M 3 devices,
// flags 0 / BGRA / SINGLETHREADED: one table]).
bool InstallCreate() {
  if (g_state.load() == 1) return true;
  ID3D11Device* dev = nullptr;
  if (!AcquirePrivate(&dev)) {  // R90: the self tests' device, kept
    g_why = "private device not created";
    return false;
  }
  void** vt = *reinterpret_cast<void***>(dev);
  const int slot = DcsQv_DeviceSlot("CreateComputeShader");
  if (slot < 0) {
    g_why = "CreateComputeShader slot unknown";
    return false;
  }
  void* orig = nullptr;
  if (!HookSlot(&vt[slot], reinterpret_cast<void*>(&H_CreateCs), &orig) || !orig) {
    g_why = "CreateComputeShader hook failed";
    return false;
  }
  g_createOrig = reinterpret_cast<CreateCsFn>(orig);
  g_createSlot = &vt[slot];
  g_state = 1;
  return true;
}

// Self tests of new pairs (worker thread, private device of the default adapter).
void TestPending() {
  const int n = g_n.load();
  bool any = false;
  for (int i = 0; i < n; ++i) any |= g_e[i].state.load() == 1;
  if (!any) return;
  if (!CompileVariants()) {
    static bool logged = false;
    if (!logged) Log("chain fast: variant compile failed (kernels left stock): %s", g_varErr.c_str());
    logged = true;
    return;
  }
  ID3D11Device* dev = nullptr;
  ID3D11DeviceContext* ctx = nullptr;
  if (!AcquirePrivate(&dev, &ctx)) return;  // R90 F4: the device created at payload start
  for (int i = 0; i < n; ++i) {
    Entry& e = g_e[i];
    if (e.state.load() != 1) continue;
    SelfTestResult r = SelfTest(dev, ctx, e.kind, e.code.empty() ? nullptr : e.code.data(), e.code.size(), g_var[e.kind]);
    if (r.ok && !e.variant) {  // on DCS's device, free threaded (DCS creates from loader threads too)
      ID3D11ComputeShader* v = nullptr;
      if (FAILED(e.dev->CreateComputeShader(g_var[e.kind].data(), g_var[e.kind].size(), nullptr, &v)) || !v) {
        r.ok = false;
        r.first = "variant not creatable on DCS's device";
      }
      e.variant = v;
    }
    e.state = r.ok ? 2 : -1;
    if (e.code.empty()) {  // identified by its output at bind time (created before the hook)
      Log("chain fast: %s identified at bind time (DCS's shader %p), self test %s: %d cases against the CPU "
          "reference%s%s",
          kStock[e.kind].name, static_cast<void*>(e.stock), r.ok ? "PASSED" : "FAILED (kernel left stock)", r.cases,
          r.first.empty() ? "" : ": ", r.first.c_str());
      continue;
    }
    const uint8_t* p = e.code.data();
    Log("chain fast: %s seen (blob %zu bytes, FNV %016llx%s), self test %s: %d cases, %d against DCS's bytecode%s%s",
        kStock[e.kind].name, e.code.size(), static_cast<unsigned long long>(Fnv64(p, e.code.size())),
        Fnv64(p, e.code.size()) == kStock[e.kind].blobFnv ? ", the 2.9.30 blob" : ", other blob, same program",
        r.ok ? "PASSED" : "FAILED (kernel left stock)", r.cases, r.stockCases, r.first.empty() ? "" : ": ",
        r.first.c_str());
  }
}

// CSSetShader hook on DCS's immediate context (its table is per context).
// d3d11 rewrites slots 7..147 of the immediate context's table (CSSetShader among them) on
// SetMultithreadProtected, SwapDeviceContextState and the first ClearState after them (split_filter.h):
// live R34 (2026-10-16) the hook bound the variants for only ~3 of every 560 dispatches per second while
// only the worker's 1 s loop put it back. So it goes back in at once, on the thread that finds the rewrite
// (split_filter.h's re-hook, every frame), with the entry d3d11 wrote as the original; the 1 s loop stays as
// the fallback (SplitFilter off, or another hook on top: GpuDedup's census).
std::atomic<uint64_t> g_setRearms{0};
// R48: re-arms refused because another hook of this payload held the slot (gpu_dedup.h's census: it re-arms after
// each call it forwards and had taken ours as its original; ours on top of it, taking it as ours, closed a cycle
// and the next CSSetShader overflowed the stack). Its hook stays; DCS's binds go through it.
std::atomic<uint64_t> g_setRefused{0};
void RearmSet() {
  void** slot = g_setSlot;
  if (!slot) return;
  void* cur = *slot;
  if (cur == reinterpret_cast<void*>(&H_CSSetShader)) return;
  if (d3ds::InPayload(cur)) {
    g_setRefused.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  g_setOrig = reinterpret_cast<SetCsFn>(cur);
  d3ds::WriteTablePointer(slot, reinterpret_cast<void*>(&H_CSSetShader));
  g_setRearms.fetch_add(1, std::memory_order_relaxed);
}
bool InstallSet() {
  if (g_setSlot) {
    if (*g_setSlot != reinterpret_cast<void*>(&H_CSSetShader)) RearmSet();  // rewritten or taken over: back on top
    sfilt::g_afterRewrite = &RearmSet;
    return true;
  }
  ID3D11Device* dev = shadowinst::g_device;
  if (dev) dev->AddRef();
  else if (sfilt::Ctx* c = sfilt::g_ctx.load()) c->GetDevice(&dev);
  if (!dev) return false;
  ID3D11DeviceContext* ctx = nullptr;
  dev->GetImmediateContext(&ctx);
  if (!ctx) {
    dev->Release();
    return false;
  }
  void** devVt = *reinterpret_cast<void***>(dev);
  const int ds = DcsQv_DeviceSlot("CreateComputeShader");
  if (g_createSlot && &devVt[ds] != g_createSlot)
    Log("chain fast: note: DCS's device uses another device table (%p) than the hooked one; its kernels are not seen",
        static_cast<void*>(devVt));
  void** vt = *reinterpret_cast<void***>(ctx);
  const int slot = DcsQv_ContextSlot("CSSetShader");
  void* orig = slot >= 0 ? vt[slot] : nullptr;
  if (!orig) {
    ctx->Release();
    dev->Release();
    return false;
  }
  // R48: written directly, not through the loader's registry (as split_filter.h / d3dstate.h do with their context
  // slots): d3d11 rewrites this slot, RearmSet adopts each new entry, and the registry kept the first one, which
  // Shutdown (UnhookSlot) and the loader's RestoreStale wrote back over d3d11's current entry. An entry an older
  // payload left in the registry is dropped, the slot keeping its current value.
  if (g_api) {
    g_api->restore(&vt[slot]);
    d3ds::WriteTablePointer(&vt[slot], orig);
  }
  d3ds::WriteTablePointer(&vt[slot], reinterpret_cast<void*>(&H_CSSetShader));
  g_setOrig = reinterpret_cast<SetCsFn>(orig);
  g_setSlot = &vt[slot];
  g_ctx = ctx;
  g_dcsDev = dev;
  sfilt::g_afterRewrite = &RearmSet;
  return true;
}

uint32_t UsableMask() {
  uint32_t m = 0;
  const int n = g_n.load();
  for (int i = 0; i < n; ++i)
    if (g_e[i].state.load() == 2 && g_e[i].dev == g_dcsDev) m |= 1u << g_e[i].kind;
  return m;
}

void LogCounters(const char* tag) {
  Log("chain fast (%s): mask 0x%x active 0x%x, pairs %d, substitutions calcoffsets %llu, calccounters %llu, "
      "airflow %llu; CSSetShader hook re-armed %llu times (refused over another hook of ours %llu)",
      tag, g_mask.load(), UsableMask(), g_n.load(), static_cast<unsigned long long>(g_subs[0].load()),
      static_cast<unsigned long long>(g_subs[1].load()), static_cast<unsigned long long>(g_subs[2].load()),
      static_cast<unsigned long long>(g_setRearms.load()), static_cast<unsigned long long>(g_setRefused.load()));
}

// ---------------------------------------------------------------------------
// Identification at bind time, by output (live R34, 2026-10-16)
// DCS 2.9.30 creates its compute shaders while the renderer starts
// (dx11backend's DX11ShaderBinaries::loadCache of Bazar\shaders\fxo and
// Saved Games\DCS\fxo), about 25 s before Scripts\Hooks load the payload: in
// the first live run the creation hook saw 0 of the 57 compute shaders DCS
// bound [M]. So the stock kernels are also found at bind time:
// - Probe (render thread, only while kinds are missing, at most kIdentMs and
//   never while a suite runs): Dispatch and DispatchIndirect hooks on DCS's
//   immediate context, re-armed from the CSSetShader hook (d3d11 rewrites
//   draw/dispatch entries). The first dispatch of each shader DCS binds is
//   classified by its shape and bindings: calcoffsets Dispatch(1,1,1), a
//   typed R32_UINT t0 and u0, no u1, cb0 set; computeOffsets Dispatch(1,1,1),
//   4-byte structured t0 and u0, no u1; calccounters DispatchIndirect, typed
//   t0, a 64-byte structured u0, typed u1 and u2, no u3, cb0 set.
// - Fingerprint: DCS's own shader object runs on synthetic inputs on DCS's
//   device (recorded on a deferred context by the worker; executed on the
//   render thread from the CSSetShader hook with ExecuteCommandList(.., TRUE),
//   which restores the context state; read back with DO_NOT_WAIT maps). It is
//   accepted only when every case equals the CPU transcription of stock
//   (scans bit for bit, including 32-bit wrap, a short SRV and an untouched
//   tail; calccounters: counts bit for bit, ranks a permutation per object,
//   untouched tail, i for invalid ids). An accepted object becomes a pair like
//   one seen at creation and goes through the same self test of the variant.
// ---------------------------------------------------------------------------
constexpr ULONGLONG kIdentMs = 180000;
std::atomic<bool> g_identStop{false};
bool g_identDone = false;  // worker: the probe ran (once per payload)
ULONGLONG g_identT0 = 0;
void** g_idVt = nullptr;
int g_idSlot[2] = {-1, -1};  // Dispatch, DispatchIndirect
void* volatile g_idOrig[2] = {};
std::atomic<void*> g_classified[kDiagCap];
struct Cand {
  ID3D11ComputeShader* cs;  // AddRef
  int kind;
};
std::mutex g_candMu;
std::vector<Cand> g_cands;
std::atomic<uint32_t> g_found{0};  // kinds accepted by output
int g_candTested = 0, g_candRejected = 0;

inline UINT BufStride(ID3D11View* v) {
  ID3D11Resource* r = nullptr;
  v->GetResource(&r);
  UINT st = 0;
  if (r) {
    D3D11_RESOURCE_DIMENSION dim;
    r->GetType(&dim);
    if (dim == D3D11_RESOURCE_DIMENSION_BUFFER) {
      D3D11_BUFFER_DESC bd{};
      static_cast<ID3D11Buffer*>(r)->GetDesc(&bd);
      st = (bd.MiscFlags & D3D11_RESOURCE_MISC_BUFFER_STRUCTURED) ? bd.StructureByteStride : 0;
    }
    r->Release();
  }
  return st;
}
// Kind whose stock shape and bindings the current dispatch has, or -1 (render thread).
int ClassifyBound(ID3D11DeviceContext* c, bool indirect) {
  ID3D11ShaderResourceView* srv = nullptr;
  ID3D11UnorderedAccessView* u[4] = {};
  ID3D11Buffer* cb = nullptr;
  c->CSGetShaderResources(0, 1, &srv);
  c->CSGetUnorderedAccessViews(0, 4, u);
  c->CSGetConstantBuffers(0, 1, &cb);
  auto srvIs = [&](DXGI_FORMAT f, UINT stride) {
    if (!srv) return false;
    D3D11_SHADER_RESOURCE_VIEW_DESC d{};
    srv->GetDesc(&d);
    return (d.ViewDimension == D3D11_SRV_DIMENSION_BUFFER || d.ViewDimension == D3D11_SRV_DIMENSION_BUFFEREX) &&
           d.Format == f && BufStride(srv) == stride;
  };
  auto uavIs = [&](int i, DXGI_FORMAT f, UINT stride) {
    if (!u[i]) return false;
    D3D11_UNORDERED_ACCESS_VIEW_DESC d{};
    u[i]->GetDesc(&d);
    return d.ViewDimension == D3D11_UAV_DIMENSION_BUFFER && d.Format == f && BufStride(u[i]) == stride;
  };
  int kind = -1;
  if (!indirect) {
    if (srvIs(DXGI_FORMAT_R32_UINT, 0) && uavIs(0, DXGI_FORMAT_R32_UINT, 0) && !u[1] && cb)
      kind = kCalcOffsets;
    else if (srvIs(DXGI_FORMAT_UNKNOWN, 4) && uavIs(0, DXGI_FORMAT_UNKNOWN, 4) && !u[1])
      kind = kAirflowOffsets;
  } else if (srvIs(DXGI_FORMAT_R32_UINT, 0) && uavIs(0, DXGI_FORMAT_UNKNOWN, 64) && uavIs(1, DXGI_FORMAT_R32_UINT, 0) &&
             uavIs(2, DXGI_FORMAT_R32_UINT, 0) && !u[3] && cb) {
    kind = kCalcCounters;
  }
  if (srv) srv->Release();
  for (auto* v : u)
    if (v) v->Release();
  if (cb) cb->Release();
  return kind;
}
std::atomic<uint64_t> g_probed{0};  // distinct shaders classified
void Probe(ID3D11DeviceContext* c, bool indirect) {
  ID3D11ComputeShader* cs = g_curCs;
  if (!cs || !DiagInsert(g_classified, cs)) return;  // each shader once, at its first dispatch
  g_probed.fetch_add(1, std::memory_order_relaxed);
  const int k = ClassifyBound(c, indirect);
  if (k < 0) return;
  const int n = g_n.load();
  for (int i = 0; i < n; ++i)
    if (g_e[i].stock == cs) return;  // already a pair
  cs->AddRef();
  std::lock_guard<std::mutex> lock(g_candMu);
  g_cands.push_back({cs, k});
}
using DispFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, UINT);
using DispIndFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Buffer*, UINT);
void RearmId(ID3D11DeviceContext* c);
void STDMETHODCALLTYPE H_IdDispatch(ID3D11DeviceContext* c, UINT x, UINT y, UINT z) {
  if (c == g_ctx && x == 1 && y == 1 && z == 1 && g_identOn.load(std::memory_order_relaxed)) Probe(c, false);
  reinterpret_cast<DispFn>(g_idOrig[0])(c, x, y, z);
  RearmId(c);
}
void STDMETHODCALLTYPE H_IdDispatchIndirect(ID3D11DeviceContext* c, ID3D11Buffer* b, UINT o) {
  if (c == g_ctx && g_identOn.load(std::memory_order_relaxed)) Probe(c, true);
  reinterpret_cast<DispIndFn>(g_idOrig[1])(c, b, o);
  RearmId(c);
}
void* const kIdHooks[2] = {reinterpret_cast<void*>(&H_IdDispatch), reinterpret_cast<void*>(&H_IdDispatchIndirect)};
// Puts the probe hooks back where d3d11 wrote its own entry (render thread).
void RearmId(ID3D11DeviceContext* c) {
  if (c != g_ctx || !g_identOn.load(std::memory_order_relaxed) || g_identStop.load(std::memory_order_relaxed) ||
      g_benchRunningFlag.load(std::memory_order_relaxed))
    return;
  void** vt = g_idVt;
  if (*reinterpret_cast<void***>(c) != vt) return;
  for (int i = 0; i < 2; ++i) {
    void* cur = vt[g_idSlot[i]];
    if (cur == kIdHooks[i]) continue;
    // R48: never over another hook of ours (gpu_pass_timing.h's Dispatch entries re-arm after each call, as this
    // probe does: each would take the other as its original). shader_fast.h's Dispatch hook: the same rule there.
    if (d3ds::InPayload(cur)) continue;
    g_idOrig[i] = cur;
    d3ds::WriteTablePointer(&vt[g_idSlot[i]], kIdHooks[i]);
  }
}
void WithdrawId() {
  if (!g_identOn.exchange(false) || !g_idVt) return;
  for (int i = 0; i < 2; ++i)
    if (g_idVt[g_idSlot[i]] == kIdHooks[i]) d3ds::WriteTablePointer(&g_idVt[g_idSlot[i]], g_idOrig[i]);
}
void IdentOnSet(ID3D11DeviceContext* c) {
  if (g_identStop.load(std::memory_order_relaxed) || g_benchRunningFlag.load(std::memory_order_relaxed)) {
    WithdrawId();  // on the render thread: none of DCS's dispatches is inside our hook
    return;
  }
  RearmId(c);
}
bool InstallId() {
  g_idSlot[0] = DcsQv_ContextSlot("Dispatch");
  g_idSlot[1] = DcsQv_ContextSlot("DispatchIndirect");
  if (g_idSlot[0] < 0 || g_idSlot[1] < 0 || !g_ctx) return false;
  g_idVt = *reinterpret_cast<void***>(g_ctx);
  for (int i = 0; i < 2; ++i) g_idOrig[i] = g_idVt[g_idSlot[i]];
  g_identStop = false;
  g_identOn = true;
  for (int i = 0; i < 2; ++i) d3ds::WriteTablePointer(&g_idVt[g_idSlot[i]], kIdHooks[i]);
  return true;
}

// Synthetic cases with their CPU reference (fingerprint; own seeds).
struct ScanCase {
  uint32_t n, c, srvLen, outLen;
  std::vector<uint32_t> cnt, sent, ref;
};
struct CntCase {
  uint32_t n, limit, cap;
  std::vector<uint32_t> objs, main, sentI;
};
constexpr uint32_t kObj = 19527;
std::vector<ScanCase> ScanCases(int kind) {
  struct D {
    uint32_t n, c, srvLen, outLen;
    int dist;
  };
  std::vector<D> ds;
  if (kind == kCalcOffsets)
    ds = {{19527, 77, 19527, 19527, 0}, {19527, 77, 19527, 19527, 1}, {19527, 50, 19527, 19527, 0},
          {19527, 77, 10000, 19527, 2}, {300, 2, 300, 600, 0},        {1, 1, 1, 4, 0},
          {0, 0, 4, 4, 0},              {257, 1, 257, 300, 0},        {40000, 157, 40000, 40000, 1}};
  else
    ds = {{10000, 79, 10000, 10000, 0}, {10000, 79, 10000, 10000, 1}, {10000, 79, 10000, 10112, 0},
          {10000, 79, 6000, 10000, 2}};
  Rng rng{0xf17e5ull + kind};
  std::vector<ScanCase> out;
  for (const D& k : ds) {
    ScanCase sc{k.n, k.c, k.srvLen, k.outLen, {}, {}, {}};
    sc.cnt.resize(k.srvLen ? k.srvLen : 1);
    for (auto& v : sc.cnt) v = k.dist == 0 ? rng() % 64 : k.dist == 1 ? rng() : ((rng() % 13) == 0 ? rng() % 5000 : 0);
    sc.sent.resize(k.outLen);
    for (size_t i = 0; i < sc.sent.size(); ++i) sc.sent[i] = 0x5A5A0000u ^ static_cast<uint32_t>(i * 2246822519u);
    sc.ref = sc.sent;
    StockScanRef(kind == kCalcOffsets ? 256 : 128, k.c, k.n, sc.cnt, k.srvLen, sc.ref);
    out.push_back(std::move(sc));
  }
  return out;
}
std::vector<CntCase> CntCases() {
  struct D {
    uint32_t n, limit;
    int dist;
  } ds[] = {{168000, 524288, 0}, {87000, 524288, 2}, {640, 524288, 3}, {5000, 3000, 1},
            {1, 524288, 0},      {0, 524288, 0},     {20000, 524288, 4}};
  Rng rng{0xc0417ull};
  std::vector<CntCase> out;
  for (const D& k : ds) {
    CntCase cc{k.n, k.limit, (k.n ? k.n : 1) + 300, {}, {}, {}};
    cc.objs.resize(cc.cap);
    cc.main.assign(static_cast<size_t>(cc.cap) * 16, 0);
    cc.sentI.resize(cc.cap);
    for (uint32_t i = 0; i < cc.cap; ++i) {
      uint32_t id;
      switch (k.dist) {
        case 0: id = 4711; break;
        case 1: id = (rng() % 100) < 95 ? 100 + rng() % 16 : rng() % kObj; break;
        case 2: id = rng() % kObj; break;
        case 3: id = (i / 64) % 7; break;
        default: id = (rng() % 50 == 0) ? kObj + rng() % 1000 : rng() % 64; break;
      }
      cc.objs[i] = id | ((rng() & 0xff) << 24);
      cc.main[static_cast<size_t>(i) * 16] = cc.objs[i];
      cc.main[static_cast<size_t>(i) * 16 + 1] = rng();
      cc.sentI[i] = 0xBEEF0000u + i;
    }
    out.push_back(std::move(cc));
  }
  return out;
}
// Stock calccounters semantics: counts, ranks a permutation per object, tail, invalid ids.
bool CountersMatchStock(const CntCase& k, const std::vector<uint32_t>& ind, const std::vector<uint32_t>& cnt) {
  if (ind.size() != k.cap || cnt.size() != kObj) return false;
  const uint32_t lim = k.n < k.limit ? k.n : k.limit;
  std::vector<uint32_t> ref(kObj, 0);
  for (uint32_t i = 0; i < lim; ++i)
    if ((k.objs[i] & 0xffffff) < kObj) ++ref[k.objs[i] & 0xffffff];
  if (cnt != ref) return false;
  std::vector<std::vector<uint8_t>> seen(kObj);
  for (uint32_t i = 0; i < k.cap; ++i) {
    const uint32_t o = k.objs[i] & 0xffffff;
    if (i >= lim) {
      if (ind[i] != k.sentI[i]) return false;
    } else if (o >= kObj) {
      if (ind[i] != i) return false;
    } else {
      if (seen[o].empty()) seen[o].assign(ref[o], 0);
      const uint32_t rk = ind[i];
      if (rk >= ref[o] || seen[o][rk]) return false;
      seen[o][rk] = 1;
    }
  }
  return true;
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
  std::vector<ScanCase> scan;
  std::vector<CntCase> cnt;
  size_t mapped = 0;          // render thread
  std::atomic<int> state{0};  // 1 recorded, 2 executed, 3 read back, -1 failed
  ULONGLONG t0 = 0;
  void ReleaseGpu() {
    for (FpOut& o : outs)
      if (o.stage) {
        o.stage->Release();
        o.stage = nullptr;
      }
    if (cl) {
      cl->Release();
      cl = nullptr;
    }
  }
};
std::atomic<FpJob*> g_fpJob{nullptr};
std::vector<FpJob*> g_fpDone;  // kept while the render thread may still look at them (Shutdown frees them)

// Render thread (CSSetShader hook): execute the recorded test, then read it back without waiting.
void FpOnSet(ID3D11DeviceContext* c) {
  FpJob* j = g_fpJob.load(std::memory_order_acquire);
  if (!j) return;
  const int st = j->state.load(std::memory_order_acquire);
  if (st == 1) {
    j->state.store(2, std::memory_order_release);  // first: the call below may re-enter this hook
    c->ExecuteCommandList(j->cl, TRUE);             // TRUE: DCS's context state is restored afterwards
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

// Worker: records DCS's shader object on the synthetic cases (deferred context of DCS's device).
bool FpBuild(FpJob& j, ID3D11Device* d) {
  ID3D11DeviceContext* dc = nullptr;
  if (FAILED(d->CreateDeferredContext(0, &dc)) || !dc) return false;
  std::vector<TBuf> keep;
  std::vector<ID3D11Buffer*> cbs;
  bool ok = true;
  auto stage = [&](const TBuf& b) {
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
  ID3D11ComputeShader* cs = j.cand.cs;
  if (j.cand.kind == kCalcOffsets || j.cand.kind == kAirflowOffsets) {
    j.scan = ScanCases(j.cand.kind);
    const UINT stride = j.cand.kind == kAirflowOffsets ? 4 : 0;
    for (const ScanCase& k : j.scan) {
      TBuf in, out;
      ok = ok && MakeBuf(d, static_cast<UINT>(k.cnt.size()), stride, k.cnt.data(), true, false, in) &&
           MakeBuf(d, k.outLen, stride, k.sent.data(), false, true, out);
      const uint32_t cbv[2] = {k.c, k.n};
      ID3D11Buffer* cb = j.cand.kind == kCalcOffsets ? MakeCb(d, cbv, 8) : nullptr;
      if (cb) cbs.push_back(cb);
      if (ok) {
        BindCs(dc, cs, cb, in.srv, &out.uav, 1);
        dc->Dispatch(1, 1, 1);
        UnbindCs(dc);
        stage(out);
      }
      keep.push_back(in);
      keep.push_back(out);
      if (!ok) break;
    }
  } else {
    j.cnt = CntCases();
    std::vector<uint32_t> zeros(kObj, 0);
    for (const CntCase& k : j.cnt) {
      const uint32_t sums[8] = {k.n, 0, 0, 0, 0, 0, 0, 0};
      const int cbv[2] = {static_cast<int>(k.limit), static_cast<int>(kObj)};
      ID3D11Buffer* cb = MakeCb(d, cbv, 8);
      if (cb) cbs.push_back(cb);
      TBuf sb, mb, ib, cbuf;
      ok = ok && cb && MakeBuf(d, 8, 0, sums, true, false, sb) &&
           MakeBuf(d, k.cap, 64, k.main.data(), false, true, mb) &&
           MakeBuf(d, k.cap, 0, k.sentI.data(), false, true, ib) &&
           MakeBuf(d, kObj, 0, zeros.data(), false, true, cbuf);
      if (ok) {
        ID3D11UnorderedAccessView* u[3] = {mb.uav, ib.uav, cbuf.uav};
        BindCs(dc, cs, cb, sb.srv, u, 3);
        dc->Dispatch((k.n >> 7) + 1, 1, 1);
        UnbindCs(dc);
        stage(ib);
        stage(cbuf);
      }
      keep.push_back(sb);
      keep.push_back(mb);
      keep.push_back(ib);
      keep.push_back(cbuf);
      if (!ok) break;
    }
  }
  if (ok) ok = SUCCEEDED(dc->FinishCommandList(FALSE, &j.cl)) && j.cl;
  dc->Release();
  for (TBuf& b : keep) b.Release();  // the command list keeps what it uses
  for (ID3D11Buffer* b : cbs) b->Release();
  return ok;
}

// Worker: true when every case equals the stock reference.
bool FpEvaluate(const FpJob& j, int* nCases) {
  if (j.cand.kind == kCalcCounters) {
    *nCases = static_cast<int>(j.cnt.size());
    if (j.outs.size() != 2 * j.cnt.size()) return false;
    for (size_t i = 0; i < j.cnt.size(); ++i)
      if (!CountersMatchStock(j.cnt[i], j.outs[2 * i].data, j.outs[2 * i + 1].data)) return false;
    return true;
  }
  *nCases = static_cast<int>(j.scan.size());
  if (j.outs.size() != j.scan.size()) return false;
  for (size_t i = 0; i < j.scan.size(); ++i)
    if (j.outs[i].data != j.scan[i].ref) return false;
  return true;
}

uint32_t KindsWithPair() {
  uint32_t m = 0;
  const int n = g_n.load();
  for (int i = 0; i < n; ++i)
    if (g_e[i].state.load() >= 1) m |= 1u << g_e[i].kind;
  return m;
}

// Worker, every second: probe hooks in / out, one fingerprint job at a time.
void IdentTick(uint32_t mask) {
  const ULONGLONG now = GetTickCount64();
  const uint32_t missing = mask & ~KindsWithPair();
  if (!g_identOn.load() && !g_identDone && missing && !g_benchRunningFlag.load() && g_ctx) {
    if (InstallId()) {
      g_identT0 = now;
      g_identDone = true;
      Log("chain fast: kinds 0x%x not seen at creation (DCS created them before the payload started); identifying "
          "them at bind time by their output",
          missing);
    }
  }
  if (g_identOn.load() && !g_identStop.load() && (!missing || now - g_identT0 > kIdentMs)) {
    g_identStop = true;  // the render thread withdraws the probe hooks
    Log("chain fast: identification at bind time ended after %llu s: kinds found 0x%x, %llu shaders probed, %d "
        "candidates tested, %d rejected",
        static_cast<unsigned long long>((now - g_identT0) / 1000), g_found.load(),
        static_cast<unsigned long long>(g_probed.load()), g_candTested, g_candRejected);
  }
  // Fingerprint job: finish the current one, else start the next.
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
      std::lock_guard<std::mutex> lock(g_mu);
      const int i = g_n.load();
      if (i < kMaxEntries) {
        Entry& e = g_e[i];
        e.dev = g_dcsDev;
        e.dev->AddRef();
        e.stock = j->cand.cs;  // the probe's reference
        e.variant = nullptr;
        e.kind = j->cand.kind;
        e.code.clear();
        e.state = 1;
        g_n.store(i + 1);
        g_found.fetch_or(1u << j->cand.kind);
        Log("chain fast: DCS's shader %p equals the stock %s in %d of %d synthetic cases: taken as that kernel",
            static_cast<void*>(j->cand.cs), kStock[j->cand.kind].name, cases, cases);
      } else {
        j->cand.cs->Release();
      }
    } else {
      ++g_candRejected;
      if (g_candRejected <= 10)
        Log("chain fast: candidate %p for %s rejected (%s)", static_cast<void*>(j->cand.cs), kStock[j->cand.kind].name,
            st == 3 ? "output differs from the stock reference" : st == -1 ? "read back failed" : "not run in 20 s");
      j->cand.cs->Release();
    }
    j->cand.cs = nullptr;
    g_fpDone.push_back(j);
    return;
  }
  if (g_benchRunningFlag.load() || !g_dcsDev) return;
  Cand c{nullptr, -1};
  {
    std::lock_guard<std::mutex> lock(g_candMu);
    while (!g_cands.empty() && !c.cs) {
      c = g_cands.front();
      g_cands.erase(g_cands.begin());
      if (g_found.load() >> c.kind & 1) {  // its kind was found meanwhile
        c.cs->Release();
        c.cs = nullptr;
      }
    }
  }
  if (!c.cs) return;
  FpJob* j = new FpJob;
  j->cand = c;
  j->t0 = now;
  if (!FpBuild(*j, g_dcsDev)) {
    Log("chain fast: candidate %p for %s: test not recordable", static_cast<void*>(c.cs), kStock[c.kind].name);
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

// Worker thread, every second. mask = [D3D] ChainFast (0 = off).
void Apply(uint32_t mask) {
  mask &= (1u << kKinds) - 1;
  if (!mask) {
    g_mask = 0;
    // R77: off (ini ChainFast=0, the kill switch) while the bind-time probe is in: withdraw it (the render thread, at
    // its next CSSetShader), or it held DCS's Dispatch slot for the session and ShaderFast never got it. The probe
    // may run again (a new window) once a mask is back.
    if (g_identOn.load() && !g_identStop.load()) {
      g_identStop = true;
      g_identDone = false;
      Log("chain fast: off; identification at bind time withdrawn (kinds found 0x%x)", g_found.load());
    }
    return;
  }
  static bool logged = false;
  if (!InstallCreate()) {
    if (!logged) Log("chain fast: not installed: %s", g_why.c_str());
    logged = true;
    return;
  }
  if (!logged) {
    Log("chain fast: creation hook in place (mask 0x%x); waiting for DCS's terrain compute kernels", mask);
    logged = true;
  }
  TestPending();
  if (!InstallSet()) return;  // DCS's device not known yet
  IdentTick(mask);
  TestPending();  // a pair identified at bind time this second
  g_mask = mask;
  static int lastActive = -1;
  const int active = static_cast<int>(UsableMask() & mask);
  if (active != lastActive) {
    lastActive = active;
    LogCounters("state");
  }
  // Load-order diagnostics, once the bind window is full (or every 60 s while kernels are missing).
  static ULONGLONG lastDiag = 0;
  static bool diagDone = false;
  const ULONGLONG now = GetTickCount64();
  if (!diagDone && (g_bindCalls.load() >= kDiagBinds || (!active && now - lastDiag >= 60000))) {
    lastDiag = now;
    diagDone = g_bindCalls.load() >= kDiagBinds;
    Log("chain fast diag: %llu compute shaders created since the hook; %llu distinct shaders bound in %llu binds, "
        "%llu of them created after the hook; stock-sized SHEX with another hash: %d",
        static_cast<unsigned long long>(g_createCalls.load()), static_cast<unsigned long long>(g_boundDistinct.load()),
        static_cast<unsigned long long>(g_bindCalls.load()), static_cast<unsigned long long>(g_boundKnown.load()),
        g_nearN.load());
    for (int i = 0; i < (std::min)(8, g_nearN.load()); ++i)
      Log("chain fast diag:   %s length, FNV %015llx? (stock %016llx)", kStock[g_near[i] >> 60].name,
          static_cast<unsigned long long>(g_near[i] & 0x0fffffffffffffffull),
          static_cast<unsigned long long>(kStock[g_near[i] >> 60].shexFnv));
  }
}

// ---------------------------------------------------------------------------
// Live same-frame check (render thread, inside gpu_pass_timing.h's dispatch hooks)
// ---------------------------------------------------------------------------
std::atomic<bool> g_verify{false};
std::atomic<uint64_t> g_checks[2], g_mismatch[2];  // calcoffsets, calccounters
ID3D11ComputeShader* g_cmp = nullptr;
ID3D11ComputeShader* g_rankAcc = nullptr;
ID3D11ComputeShader* g_rankFin = nullptr;
ID3D11Buffer* g_res = nullptr;
ID3D11UnorderedAccessView* g_resUav = nullptr;
ID3D11Buffer* g_resStage[3] = {};
int g_stageNext = 0, g_stageLive = -1;
struct Scratch {
  ID3D11Buffer* b = nullptr;
  ID3D11UnorderedAccessView* u = nullptr;
};
std::map<std::pair<ID3D11Resource*, std::string>, Scratch> g_scratch;
struct Pending {
  int kind = -1;
  Entry* e = nullptr;
  ID3D11UnorderedAccessView* pre[2] = {};  // scratch copies of the outputs before DCS's dispatch
};
Pending t_pend;

bool PrepareVerify(ID3D11Device* d) {
  if (g_cmp) return true;
  std::vector<uint8_t> a, b, c;
  std::string err;
  if (!Compile(kSrcCmp, "qv_cf_cmp", a, &err) || !Compile(kSrcRankAcc, "qv_cf_rank", b, &err) ||
      !Compile(kSrcRankFin, "qv_cf_fin", c, &err))
    return false;
  if (FAILED(d->CreateComputeShader(a.data(), a.size(), nullptr, &g_cmp)) ||
      FAILED(d->CreateComputeShader(b.data(), b.size(), nullptr, &g_rankAcc)) ||
      FAILED(d->CreateComputeShader(c.data(), c.size(), nullptr, &g_rankFin)))
    return false;
  D3D11_BUFFER_DESC bd{};
  bd.ByteWidth = 64;
  bd.Usage = D3D11_USAGE_DEFAULT;
  bd.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
  bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
  const uint32_t zero[16] = {};
  D3D11_SUBRESOURCE_DATA sd{zero, 0, 0};
  if (FAILED(d->CreateBuffer(&bd, &sd, &g_res))) return false;
  D3D11_UNORDERED_ACCESS_VIEW_DESC ud{};
  ud.Format = DXGI_FORMAT_R32_TYPELESS;
  ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
  ud.Buffer.NumElements = 16;
  ud.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
  if (FAILED(d->CreateUnorderedAccessView(g_res, &ud, &g_resUav))) return false;
  bd.Usage = D3D11_USAGE_STAGING;
  bd.BindFlags = 0;
  bd.MiscFlags = 0;
  bd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  for (auto& s : g_resStage)
    if (FAILED(d->CreateBuffer(&bd, nullptr, &s))) return false;
  return true;
}

// A buffer like the view's resource, with a view of the same description.
// tag separates two copies of the same resource.
ID3D11UnorderedAccessView* ScratchOf(ID3D11Device* d, ID3D11UnorderedAccessView* v, int tag, ID3D11Buffer** buf) {
  D3D11_UNORDERED_ACCESS_VIEW_DESC vd{};
  v->GetDesc(&vd);
  if (vd.ViewDimension != D3D11_UAV_DIMENSION_BUFFER) return nullptr;
  ID3D11Resource* r = nullptr;
  v->GetResource(&r);
  if (!r) return nullptr;
  r->Release();  // DCS keeps it alive while bound
  auto key = std::make_pair(r, std::string(reinterpret_cast<const char*>(&vd), sizeof(vd)) + char('0' + tag));
  auto it = g_scratch.find(key);
  if (it == g_scratch.end()) {
    if (g_scratch.size() >= 64) return nullptr;
    Scratch s;
    D3D11_BUFFER_DESC bd{};
    static_cast<ID3D11Buffer*>(r)->GetDesc(&bd);
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.CPUAccessFlags = 0;
    bd.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
    bd.MiscFlags &= D3D11_RESOURCE_MISC_BUFFER_STRUCTURED | D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
    if (FAILED(d->CreateBuffer(&bd, nullptr, &s.b))) return nullptr;
    if (FAILED(d->CreateUnorderedAccessView(s.b, &vd, &s.u))) {
      s.b->Release();
      return nullptr;
    }
    it = g_scratch.emplace(key, s).first;
  }
  if (buf) *buf = it->second.b;
  return it->second.u;
}

Entry* EntryOfBound(ID3D11DeviceContext* c) {
  ID3D11ComputeShader* cs = nullptr;
  c->CSGetShader(&cs, nullptr, nullptr);
  if (!cs) return nullptr;
  cs->Release();
  const int n = g_n.load(std::memory_order_acquire);
  for (int i = 0; i < n; ++i)
    if ((g_e[i].variant == cs || g_e[i].stock == cs) && g_e[i].state.load() == 2) return &g_e[i];
  return nullptr;
}

void PreDispatch(ID3D11DeviceContext* c) {
  t_pend = Pending{};
  if (c != g_ctx || GetCurrentThreadId() != ptiming::g_topTid.load(std::memory_order_relaxed)) return;
  Entry* e = EntryOfBound(c);
  if (!e || e->kind == kAirflowOffsets || !PrepareVerify(g_dcsDev)) return;
  ID3D11UnorderedAccessView* u[3] = {};
  c->CSGetUnorderedAccessViews(0, 3, u);
  bool ok = true;
  const int first = e->kind == kCalcOffsets ? 0 : 1, count = e->kind == kCalcOffsets ? 1 : 2;
  for (int j = 0; j < count && ok; ++j) {
    ID3D11UnorderedAccessView* v = u[first + j];
    ID3D11Buffer* sb = nullptr;
    ID3D11Resource* r = nullptr;
    if (!v || !(t_pend.pre[j] = ScratchOf(g_dcsDev, v, j, &sb))) {
      ok = false;
      break;
    }
    v->GetResource(&r);
    c->CopyResource(sb, r);
    r->Release();
  }
  for (auto* v : u)
    if (v) v->Release();
  if (ok) {
    t_pend.kind = e->kind;
    t_pend.e = e;
  }
}

void ReadResults(ID3D11DeviceContext* c) {
  if (g_stageLive >= 0) {
    D3D11_MAPPED_SUBRESOURCE m{};
    if (SUCCEEDED(c->Map(g_resStage[g_stageLive], 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m))) {
      const uint32_t* p = static_cast<const uint32_t*>(m.pData);
      g_mismatch[0] = p[0];
      g_mismatch[1] = p[1];
      c->Unmap(g_resStage[g_stageLive], 0);
      g_stageLive = -1;
    }
  }
  if (g_stageLive < 0) {
    g_stageLive = g_stageNext;
    g_stageNext = (g_stageNext + 1) % 3;
    c->CopyResource(g_resStage[g_stageLive], g_res);
  }
}

// After DCS's dispatch: the stock kernel on the scratch copies, then compares.
template <class Issue>
void PostDispatch(ID3D11DeviceContext* c, Issue issueStock) {
  if (t_pend.kind < 0) return;
  const Pending p = t_pend;
  t_pend = Pending{};
  // DCS's compute state
  ID3D11ComputeShader* cs0 = nullptr;
  c->CSGetShader(&cs0, nullptr, nullptr);
  ID3D11Buffer* cb[2] = {};
  c->CSGetConstantBuffers(0, 2, cb);
  ID3D11ShaderResourceView* sr[2] = {};
  c->CSGetShaderResources(0, 2, sr);
  ID3D11UnorderedAccessView* u[8] = {};
  c->CSGetUnorderedAccessViews(0, 8, u);
  ID3D11UnorderedAccessView* nul[8] = {};
  const bool viewsOk = p.kind == kCalcOffsets ? u[0] != nullptr : (u[0] && u[1] && u[2]);
  if (!viewsOk) {
  } else if (p.kind == kCalcOffsets) {
    // stock on the copy of the pre-dispatch offsets, DCS's CB0 and t0
    c->CSSetShader(p.e->stock, nullptr, 0);
    ID3D11UnorderedAccessView* uu[1] = {p.pre[0]};
    c->CSSetUnorderedAccessViews(0, 8, nul, nullptr);
    c->CSSetUnorderedAccessViews(0, 1, uu, nullptr);
    issueStock();
    D3D11_UNORDERED_ACCESS_VIEW_DESC vd{};
    u[0]->GetDesc(&vd);
    const uint32_t g[4] = {vd.Buffer.NumElements, 0, 0, 0};
    ID3D11Buffer* gcb = MakeCb(g_dcsDev, g, 16);
    ID3D11UnorderedAccessView* cu[3] = {u[0], p.pre[0], g_resUav};
    c->CSSetUnorderedAccessViews(0, 8, nul, nullptr);
    c->CSSetShader(g_cmp, nullptr, 0);
    c->CSSetConstantBuffers(0, 1, &gcb);
    c->CSSetUnorderedAccessViews(0, 3, cu, nullptr);
    c->Dispatch((vd.Buffer.NumElements + 63) / 64, 1, 1);
    if (gcb) gcb->Release();
    g_checks[0].fetch_add(1);
  } else {
    // stock with DCS's sums (t0) and instances (u0), on the copies of ranks and counts
    c->CSSetShader(p.e->stock, nullptr, 0);
    ID3D11UnorderedAccessView* uu[3] = {u[0], p.pre[0], p.pre[1]};
    c->CSSetUnorderedAccessViews(0, 8, nul, nullptr);
    c->CSSetUnorderedAccessViews(0, 3, uu, nullptr);
    issueStock();
    // counts bit for bit
    D3D11_UNORDERED_ACCESS_VIEW_DESC vd{};
    u[2]->GetDesc(&vd);
    const uint32_t nObj = vd.Buffer.NumElements;
    const uint32_t g[4] = {nObj, 1, 0, 0};
    ID3D11Buffer* gcb = MakeCb(g_dcsDev, g, 16);
    ID3D11UnorderedAccessView* cu[3] = {u[2], p.pre[1], g_resUav};
    c->CSSetUnorderedAccessViews(0, 8, nul, nullptr);
    c->CSSetShader(g_cmp, nullptr, 0);
    c->CSSetConstantBuffers(0, 1, &gcb);
    c->CSSetUnorderedAccessViews(0, 3, cu, nullptr);
    c->Dispatch((nObj + 63) / 64, 1, 1);
    // ranks: per-object sums (accumulators: two more scratch buffers like the counts)
    ID3D11UnorderedAccessView* accS = ScratchOf(g_dcsDev, u[2], 2, nullptr);
    ID3D11UnorderedAccessView* accQ = ScratchOf(g_dcsDev, u[2], 3, nullptr);
    if (accS && accQ && cb[0] && u[1]) {
      const UINT zero[4] = {0, 0, 0, 0};
      c->ClearUnorderedAccessViewUint(accS, zero);
      c->ClearUnorderedAccessViewUint(accQ, zero);
      // b0 = DCS's CB0 {mainBufferSize, countsBufferSize}, b1 = ours {n, slot 1}
      ID3D11Buffer* rcb[2] = {cb[0], gcb};
      ID3D11UnorderedAccessView* ru[7] = {u[0], u[1], p.pre[0], u[2], accS, accQ, g_resUav};
      c->CSSetUnorderedAccessViews(0, 8, nul, nullptr);
      c->CSSetShader(g_rankAcc, nullptr, 0);
      c->CSSetConstantBuffers(0, 2, rcb);
      c->CSSetShaderResources(0, 1, &sr[0]);
      c->CSSetUnorderedAccessViews(0, 7, ru, nullptr);
      D3D11_UNORDERED_ACCESS_VIEW_DESC rd{};
      u[1]->GetDesc(&rd);
      c->Dispatch((rd.Buffer.NumElements + 127) / 128, 1, 1);
      ID3D11UnorderedAccessView* fu[3] = {accS, accQ, g_resUav};
      c->CSSetUnorderedAccessViews(0, 8, nul, nullptr);
      c->CSSetShader(g_rankFin, nullptr, 0);
      c->CSSetConstantBuffers(0, 1, &gcb);
      c->CSSetUnorderedAccessViews(0, 3, fu, nullptr);
      c->Dispatch((nObj + 63) / 64, 1, 1);
    }
    if (gcb) gcb->Release();
    g_checks[1].fetch_add(1);
  }
  ReadResults(c);
  // DCS's state back (hidden counters kept)
  UINT keep[8];
  for (UINT& k : keep) k = ~0u;
  c->CSSetUnorderedAccessViews(0, 8, nul, nullptr);
  c->CSSetShader(cs0, nullptr, 0);
  c->CSSetConstantBuffers(0, 2, cb);
  c->CSSetShaderResources(0, 2, sr);
  c->CSSetUnorderedAccessViews(0, 8, u, keep);
  if (cs0) cs0->Release();
  for (auto* x : cb)
    if (x) x->Release();
  for (auto* x : sr)
    if (x) x->Release();
  for (auto* x : u)
    if (x) x->Release();
}
void PostDispatch(ID3D11DeviceContext* c, UINT x, UINT y, UINT z) {
  PostDispatch(c, [&] { gpt::Orig<decltype(&gpt::H_Dispatch)>(gpt::kDispatch)(c, x, y, z); });
}
void PostDispatchIndirect(ID3D11DeviceContext* c, ID3D11Buffer* b, UINT o) {
  PostDispatch(c, [&] { gpt::Orig<decltype(&gpt::H_DispatchIndirect)>(gpt::kDispatchIndirect)(c, b, o); });
}

void ReleaseVerify() {
  for (auto& kv : g_scratch) {
    if (kv.second.u) kv.second.u->Release();
    if (kv.second.b) kv.second.b->Release();
  }
  g_scratch.clear();
  for (auto*& s : g_resStage)
    if (s) s->Release(), s = nullptr;
  if (g_resUav) g_resUav->Release();
  if (g_res) g_res->Release();
  if (g_cmp) g_cmp->Release();
  if (g_rankAcc) g_rankAcc->Release();
  if (g_rankFin) g_rankFin->Release();
  g_resUav = nullptr;
  g_res = nullptr;
  g_cmp = g_rankAcc = g_rankFin = nullptr;
  g_stageLive = -1;
}

// ---------------------------------------------------------------------------
// Suite phases (suite thread)
// ---------------------------------------------------------------------------
void TimingPhase(uint32_t cfgMask, std::atomic<uint64_t>& frameCounter) {
  const uint32_t want = cfgMask ? cfgMask : 0x7;
  Apply(want);
  if (!(UsableMask() & want)) {
    Log("  chain fast: no tested kernel pair (not seen at creation, or DCS's device unknown); phase skipped");
    LogCounters("timing skipped");
    Apply(cfgMask);
    return;
  }
  for (int i = 0; i < 5; ++i) {
    const bool on = (i & 1) != 0;
    g_mask = on ? want : 0;
    Sleep(1500);
    Log("  chain fast timing run %d: %s", i + 1, on ? "ON (variants bound)" : "OFF (stock)");
    gpt::Measure(5000, frameCounter);
    if (on) LogCounters("timing run");
  }
  g_mask = cfgMask;
  Apply(cfgMask);
}

// Same-frame check while the configured mask is live (stock vs stock when 0).
void VerifyPhase(int seconds, uint32_t cfgMask, std::atomic<uint64_t>& frameCounter) {
  Apply(cfgMask ? cfgMask : 0x7);
  g_mask = cfgMask;
  if (!g_dcsDev || !UsableMask()) {
    Log("  chain fast verify: no tested kernel pair; phase skipped");
    return;
  }
  g_checks[0] = g_checks[1] = 0;
  g_mismatch[0] = g_mismatch[1] = 0;
  g_verify = true;
  gpt::Measure(seconds * 1000, frameCounter);
  g_verify = false;
  Log("  chain fast verify (%s): calcoffsets %llu checks, %llu words differ; calccounters %llu checks, %llu "
      "differences (counts, rank range, per-object rank sums)",
      cfgMask ? "variants live" : "stock vs stock", static_cast<unsigned long long>(g_checks[0].load()),
      static_cast<unsigned long long>(g_mismatch[0].load()), static_cast<unsigned long long>(g_checks[1].load()),
      static_cast<unsigned long long>(g_mismatch[1].load()));
}

void Shutdown() {
  void (*me)() = &RearmSet;
  sfilt::g_afterRewrite.compare_exchange_strong(me, nullptr);
  g_mask = 0;
  g_verify = false;
  g_fpLive = false;
  WithdrawId();
  if (g_setSlot) {
    // R48: d3d11's latest entry (RearmSet keeps it); another hook on top keeps ours in its chain (a pass-through).
    if (*g_setSlot == reinterpret_cast<void*>(&H_CSSetShader))
      d3ds::WriteTablePointer(g_setSlot, reinterpret_cast<void*>(g_setOrig));
    g_setSlot = nullptr;
  }
  if (g_createSlot) {
    UnhookSlot(g_createSlot, reinterpret_cast<void*>(g_createOrig));
    g_createSlot = nullptr;
  }
  g_state = 0;
  Sleep(50);  // a CSSetShader already inside the hook finishes
  // Variants DCS's context may still hold stay alive through its reference;
  // the pinned stock objects are released (DCS owns them).
  std::lock_guard<std::mutex> lock(g_mu);
  for (int i = 0; i < g_n.load(); ++i) {
    Entry& e = g_e[i];
    if (e.stock) e.stock->Release();
    if (e.variant) e.variant->Release();
    if (e.dev) e.dev->Release();
    e.stock = e.variant = nullptr;
    e.dev = nullptr;
    e.state = 0;
  }
  g_n = 0;
  if (FpJob* j = g_fpJob.exchange(nullptr)) {
    j->ReleaseGpu();
    if (j->cand.cs) j->cand.cs->Release();
    g_fpDone.push_back(j);
  }
  for (FpJob* j : g_fpDone) delete j;
  g_fpDone.clear();
  {
    std::lock_guard<std::mutex> cl(g_candMu);
    for (Cand& c : g_cands) c.cs->Release();
    g_cands.clear();
  }
  for (auto& a : g_classified) a = nullptr;
  g_found = 0;
  g_identDone = false;
  g_identStop = false;
  g_candTested = g_candRejected = 0;
  ReleaseVerify();
  if (g_ctx) g_ctx->Release();
  if (g_dcsDev) g_dcsDev->Release();
  g_ctx = nullptr;
  g_dcsDev = nullptr;
  ReleasePrivate();  // R90 F4 (shader_fast.h's Shutdown ran before)
}

}  // namespace chainfast
