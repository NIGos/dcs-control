// R34 shorter culling chains (chain_fast.h): container parsing and matching,
// the three variants against the CPU transcription of the stock kernels on
// WARP, the creation hook -> self test -> CSSetShader substitution path with
// HLSL transcriptions of the stock kernels standing in for DCS's bytecode,
// runtime toggling, the live same-frame check (clean and with an injected
// error), and shutdown. When QV_R34_STOCK_DIR names a folder with DCS's own
// stock .dxbc files (extracted from Bazar\shaders\fxo\fxo.edcz, file name =
// md5), they are matched and self-tested too; otherwise those checks SKIP.
#pragma once

namespace cftest {
using namespace chainfast;

// R48: f(arg) on a thread with a 256 KB stack. True when it did not return: a stack overflow (two hooks forwarding
// to each other) or still running after 10 s (the same cycle compiled to tail jumps). The thread is abandoned then.
struct R48Call {
  void (*fn)(void*);
  void* arg;
  volatile bool overflow;
};
DWORD WINAPI R48Thread(void* p) {
  R48Call* c = static_cast<R48Call*>(p);
  ULONG guarantee = 64 * 1024;  // room for the handler once the stack is gone
  SetThreadStackGuarantee(&guarantee);
  __try {
    c->fn(c->arg);
  } __except (GetExceptionCode() == EXCEPTION_STACK_OVERFLOW ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
    c->overflow = true;
  }
  return 0;
}
bool R48Cycles(void (*fn)(void*), void* arg) {
  static R48Call c;  // outlives an abandoned thread
  c = R48Call{fn, arg, false};
  HANDLE t = CreateThread(nullptr, 256 * 1024, &R48Thread, &c, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
  if (!t) return false;
  const bool done = WaitForSingleObject(t, 10000) == WAIT_OBJECT_0;
  CloseHandle(t);
  return !done || c.overflow;
}
struct R48Bind {
  ID3D11DeviceContext* ctx;
  ID3D11ComputeShader* s;
  ID3D11ComputeShader* got;
};
// A loader registry for the shutdown check: the CSSetShader slot's "first seen" original is a stale entry.
namespace R48Api {
SetCsFn real = nullptr;
void** setSlot = nullptr;
void** createSlot = nullptr;
void* createOrig = nullptr;
void STDMETHODCALLTYPE Stale(ID3D11DeviceContext* c, ID3D11ComputeShader* s, ID3D11ClassInstance* const* ci, UINT n) {
  real(c, s, ci, n);
}
bool Patch(void** slot, void* hook, void** original) {
  if (original) *original = *slot;
  d3ds::WriteTablePointer(slot, hook);
  return true;
}
void* Original(void** slot) { return *slot; }
void Restore(void** slot) {
  if (slot == setSlot) d3ds::WriteTablePointer(slot, reinterpret_cast<void*>(&Stale));
  if (slot == createSlot) d3ds::WriteTablePointer(slot, createOrig);
}
void LogLine(const char* line) { printf("%s\n", line); }
}  // namespace R48Api
// R107: [Suite] GpuDedup (gpu_dedup.h, archived) was the payload hook R48 met over CSSetShader. This stands in for it:
// a hook of this payload (InPayload) that forwards to the entry it took as its original, as the census hook did.
void* g_r48CensusOrig = nullptr;
void STDMETHODCALLTYPE R48CensusCs(ID3D11DeviceContext* c, ID3D11ComputeShader* s, ID3D11ClassInstance* const* ci,
                                   UINT n) {
  reinterpret_cast<SetCsFn>(g_r48CensusOrig)(c, s, ci, n);
}
void R48BindCall(void* p) {
  R48Bind* b = static_cast<R48Bind*>(p);
  b->ctx->CSSetShader(b->s, nullptr, 0);
  b->ctx->CSGetShader(&b->got, nullptr, nullptr);
  if (b->got) b->got->Release();
}

// The stock programs, transcribed from the 2.9.30 disassembly (same CB,
// bindings, thread counts and loops): stand-ins for DCS's bytecode.
const char kStockOffsetsHlsl[] = R"(
cbuffer Globals : register(b0) { uint chunkSize; uint counts; };
Buffer<uint> countsIn : register(t0);
RWBuffer<uint> offsetsOut : register(u0);
groupshared uint g0[256];
groupshared uint g1[256];
[numthreads(256, 1, 1)]
void main(uint t : SV_GroupIndex) {
  uint c = chunkSize, n = counts;
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
  GroupMemoryBarrierWithGroupSync();
  if (t == 0) { g1[0] = 0; [loop] for (uint k = 1; k < 256; ++k) g1[k] = g0[k - 1] + g1[k - 1]; }
  GroupMemoryBarrierWithGroupSync();
  [loop] for (uint q = 0; q < c; ++q) {
    uint idx = t * c + q;
    if (idx >= n) break;
    offsetsOut[idx] = g1[t] + offsetsOut[idx];
  }
}
)";
const char kStockCountersHlsl[] = R"(
cbuffer Globals : register(b0) { int mainBufferSize; int countsBufferSize; };
struct Inst { uint4 a, b, c, d; };
Buffer<uint> sumsBuffer : register(t0);
RWStructuredBuffer<Inst> mainBuffer : register(u0);
RWBuffer<uint> indiciesNoOffsets : register(u1);
RWBuffer<uint> countsBuffer : register(u2);
[numthreads(128, 1, 1)]
void main(uint3 gid : SV_GroupID, uint tid : SV_GroupIndex) {
  uint i = gid.x * 128u + tid;
  if (i >= sumsBuffer[0]) return;
  if (i >= (uint)mainBufferSize) return;
  indiciesNoOffsets[i] = i;
  uint obj = mainBuffer[i].a.x & 0x00ffffffu;
  if ((int)obj >= countsBufferSize) return;
  uint r;
  InterlockedAdd(countsBuffer[obj], 1, r);
  indiciesNoOffsets[i] = r;
}
)";
const char kStockAirflowHlsl[] = R"(
StructuredBuffer<uint> countsIn : register(t0);
RWStructuredBuffer<uint> offsetsOut : register(u0);
groupshared uint g0[128];
groupshared uint g1[128];
[numthreads(128, 1, 1)]
void main(uint t : SV_GroupIndex) {
  uint i0 = t * 79;
  g0[t] = 0;
  if (i0 < 10000) { offsetsOut[i0] = 0; g0[t] = countsIn[i0]; }
  [loop] for (uint j = 1; j < 79; ++j) {
    uint idx = t * 79 + j;
    if (idx >= 10000) break;
    uint run = g0[t];
    offsetsOut[idx] = run;
    g0[t] = countsIn[idx] + run;
  }
  GroupMemoryBarrierWithGroupSync();
  if (t == 0) { g1[0] = 0; [loop] for (uint k = 1; k < 128; ++k) g1[k] = g0[k - 1] + g1[k - 1]; }
  GroupMemoryBarrierWithGroupSync();
  [loop] for (uint q = 0; q < 79; ++q) {
    uint idx = t * 79 + q;
    if (idx >= 10000) break;
    offsetsOut[idx] = g1[t] + offsetsOut[idx];
  }
}
)";

std::vector<uint8_t> ReadAll(const std::wstring& p) {
  std::vector<uint8_t> b;
  HANDLE h = CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
  if (h == INVALID_HANDLE_VALUE) return b;
  LARGE_INTEGER sz{};
  GetFileSizeEx(h, &sz);
  b.resize(static_cast<size_t>(sz.QuadPart));
  DWORD got = 0;
  if (!ReadFile(h, b.data(), static_cast<DWORD>(b.size()), &got, nullptr) || got != b.size()) b.clear();
  CloseHandle(h);
  return b;
}

uint64_t ShexHash(const std::vector<uint8_t>& code) {
  const uint8_t* p = nullptr;
  uint32_t len = 0;
  return ShexChunk(code.data(), code.size(), &p, &len) ? Fnv64(p, len) : 0;
}

void Run() {
  // --- container parsing / matching (pure)
  {
    std::vector<uint8_t> v;
    std::string err;
    const bool ok = Compile(kSrcCalcOffsets, "t", v, &err);
    const uint8_t* p = nullptr;
    uint32_t len = 0;
    Check(ok && ShexChunk(v.data(), v.size(), &p, &len) && len > 64 && p > v.data() && p + len <= v.data() + v.size(),
          "chain fast: SHEX chunk found in a compiled container");
    Check(MatchStock(v.data(), v.size()) == -1, "chain fast: our own variant is not taken for a stock kernel");
    std::vector<uint8_t> bad = v;
    if (bad.size() > 40) bad[28] = 200;  // chunk count out of range
    Check(!ShexChunk(bad.data(), bad.size(), &p, &len) && MatchStock(bad.data(), bad.size()) == -1 &&
              MatchStock(v.data(), 16) == -1 && MatchStock(nullptr, 0) == -1,
          "chain fast: malformed containers are rejected");
    Check(Fnv64(reinterpret_cast<const uint8_t*>("a"), 1) == 0xaf63dc4c8601ec8cull, "chain fast: FNV-1a 64 reference value");
  }
  Check(CompileVariants(), "chain fast: the three variants compile (cs_5_0, O3)");

  ID3D11Device* dev = nullptr;
  ID3D11DeviceContext* ctx = nullptr;
  D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
  if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, &fl, 1, D3D11_SDK_VERSION, &dev, nullptr, &ctx))) {
    Check(false, "chain fast: WARP device");
    return;
  }
  // --- variants vs the CPU transcription (no stock bytecode)
  for (int k = 0; k < kKinds; ++k) {
    const SelfTestResult r = SelfTest(dev, ctx, k, nullptr, 0, g_var[k]);
    char what[160];
    sprintf_s(what, "chain fast: %s variant equals the CPU transcription of stock on WARP (%d cases)%s%s",
              kStock[k].name, r.cases, r.first.empty() ? "" : ": ", r.first.c_str());
    Check(r.ok && r.cases >= 4, what);
  }
  // --- HLSL transcriptions of stock as stand-ins: variant vs "stock" on WARP
  std::vector<uint8_t> fake[kKinds];
  const char* fakeSrc[kKinds] = {kStockOffsetsHlsl, kStockCountersHlsl, kStockAirflowHlsl};
  bool fakeOk = true;
  for (int k = 0; k < kKinds; ++k) fakeOk &= Compile(fakeSrc[k], "stock_transcription", fake[k], nullptr);
  Check(fakeOk, "chain fast: stock transcriptions compile");
  for (int k = 0; k < kKinds && fakeOk; ++k) {
    const SelfTestResult r = SelfTest(dev, ctx, k, fake[k].data(), fake[k].size(), g_var[k]);
    char what[160];
    sprintf_s(what, "chain fast: %s variant vs the stock transcription on WARP (%d against stock)%s%s", kStock[k].name,
              r.stockCases, r.first.empty() ? "" : ": ", r.first.c_str());
    Check(r.ok && r.stockCases == r.cases, what);
  }
  // --- DCS's own stock bytecode (optional)
  {
    const wchar_t* dir = _wgetenv(L"QV_R34_STOCK_DIR");
    const wchar_t* md5[kKinds] = {L"7fa44bc571591d74ca45a2e8eb14de88", L"e04d8a271d158223394c66f6bb4ad4f5",
                                  L"e492c7c173036520cf5b41c1021bd63d"};
    if (!dir || !*dir) {
      QvSkip("chain fast: DCS's stock bytecode (set QV_R34_STOCK_DIR to a folder of extracted .dxbc)\n");
    } else {
      for (int k = 0; k < kKinds; ++k) {
        const std::vector<uint8_t> b = ReadAll(std::wstring(dir) + L"\\" + md5[k] + L".dxbc");
        char what[200];
        sprintf_s(what, "chain fast: DCS's %s (%zu bytes) is matched, blob FNV as recorded", kStock[k].name, b.size());
        Check(!b.empty() && MatchStock(b.data(), b.size()) == k && Fnv64(b.data(), b.size()) == kStock[k].blobFnv &&
                  b.size() == kStock[k].blobBytes,
              what);
        if (b.empty()) continue;
        const SelfTestResult r = SelfTest(dev, ctx, k, b.data(), b.size(), g_var[k]);
        sprintf_s(what, "chain fast: %s variant vs DCS's bytecode on WARP: %d cases%s%s", kStock[k].name, r.stockCases,
                  r.first.empty() ? "" : ": ", r.first.c_str());
        Check(r.ok && r.stockCases == r.cases, what);
      }
    }
  }

  // --- creation hook -> self test -> substitution on a WARP "DCS" device
  ID3D11Device* saveDev = shadowinst::g_device;
  sfilt::Ctx* saveSf = sfilt::g_ctx.load();
  shadowinst::g_device = nullptr;  // other tests may have left theirs: InstallSet must see only ours
  sfilt::g_ctx = nullptr;
  for (int k = 0; k < kKinds; ++k) g_testShexFnv[k] = ShexHash(fake[k]);
  Apply(0x7);
  Check(g_state.load() == 1 && g_createSlot && *g_createSlot == reinterpret_cast<void*>(&H_CreateCs),
        "chain fast: creation hook in d3d11's shared device table");
  ID3D11ComputeShader* stock[kKinds] = {};
  for (int k = 0; k < kKinds; ++k) dev->CreateComputeShader(fake[k].data(), fake[k].size(), nullptr, &stock[k]);
  ID3D11ComputeShader* other = nullptr;
  dev->CreateComputeShader(g_var[0].data(), g_var[0].size(), nullptr, &other);  // not a stock kernel
  Check(g_n.load() == kKinds && g_e[0].stock == stock[0] && g_e[1].stock == stock[1] && g_e[2].stock == stock[2] &&
            g_e[0].state.load() == 1 && !g_e[0].variant,
        "chain fast: the three stock kernels are recorded at creation (pinned, variant created later)");
  shadowinst::g_device = dev;  // the "DCS" device for InstallSet
  Apply(0x7);  // self tests, variants on the device, CSSetShader hook on its immediate context
  Check(g_e[0].state.load() == 2 && g_e[1].state.load() == 2 && g_e[2].state.load() == 2 && g_e[0].variant &&
            g_setSlot && *g_setSlot == reinterpret_cast<void*>(&H_CSSetShader) && g_mask.load() == 0x7 &&
            UsableMask() == 0x7,
        "chain fast: self tests pass, CSSetShader hooked, mask 0x7 active");
  auto bound = [&](ID3D11ComputeShader* s) {
    ctx->CSSetShader(s, nullptr, 0);
    ID3D11ComputeShader* b = nullptr;
    ctx->CSGetShader(&b, nullptr, nullptr);
    if (b) b->Release();
    return b;
  };
  Check(bound(stock[0]) == g_e[0].variant && bound(stock[1]) == g_e[1].variant && bound(stock[2]) == g_e[2].variant &&
            bound(other) == other && g_n.load() == kKinds,
        "chain fast: binding a stock kernel binds its variant; other shaders pass through");
  {
    // d3d11 rewrites the table entry (live 2026-10-16): the split filter's re-hook callback puts ours back.
    d3ds::WriteTablePointer(g_setSlot, reinterpret_cast<void*>(g_setOrig));
    const bool lost = bound(stock[0]) == stock[0];
    auto cb = sfilt::g_afterRewrite.load();
    if (cb) cb();
    Check(lost && cb && *g_setSlot == reinterpret_cast<void*>(&H_CSSetShader) && bound(stock[0]) == g_e[0].variant &&
              g_setRearms.load() >= 1,
          "chain fast: after d3d11 rewrites the CSSetShader entry, the split filter's re-hook callback re-arms ours");
  }
  {
    // R48: [Suite] GpuDedup's census hook (R107: a stand-in, R48CensusCs) went in over ours (taking ours as its original) and re-arms after every
    // call it forwards. Ours must not go back on top of it taking it as the original: the two then forward to each
    // other (before R48 the split filter's re-hook callback and the 1 s loop did exactly that; the next CSSetShader
    // overflowed the stack). Ours stays under it: DCS's binds still reach ours through it.
    void* const gddHook = reinterpret_cast<void*>(&R48CensusCs);  // R107: the census hook's stand-in
    const SetCsFn real = g_setOrig;
    g_r48CensusOrig = reinterpret_cast<void*>(&H_CSSetShader);
    d3ds::WriteTablePointer(g_setSlot, gddHook);
    if (auto cb = sfilt::g_afterRewrite.load()) cb();  // the split filter's re-hook callback
    InstallSet();                                      // the worker's 1 s loop
    const bool tookIt = reinterpret_cast<void*>(g_setOrig) == gddHook;
    R48Bind b{ctx, stock[0], nullptr};
    const bool cycles = R48Cycles(&R48BindCall, &b);
#ifndef R48_PREFIX
    const bool counted = g_setRefused.load() >= 1;
#else
    const bool counted = true;
#endif
    Check(!tookIt && !cycles && *g_setSlot == gddHook && b.got == g_e[0].variant && counted,
          "R48 chain fast: no re-arm over GpuDedup's CSSetShader hook (no call cycle; binds reach ours through it)");
    g_setOrig = real;
    d3ds::WriteTablePointer(g_setSlot, reinterpret_cast<void*>(&H_CSSetShader));
    g_r48CensusOrig = nullptr;
  }
  Apply(0x1);
  Check(bound(stock[0]) == g_e[0].variant && bound(stock[1]) == stock[1] && bound(stock[2]) == stock[2],
        "chain fast: the mask selects kernels at run time (0x1: calcoffsets only)");
  Apply(0);
  Check(bound(stock[0]) == stock[0] && g_mask.load() == 0 && g_n.load() == kKinds,
        "chain fast: mask 0 binds stock (hooks stay, pass through; self tests add no pairs)");

  // --- live same-frame check (render-thread path, called directly)
  Apply(0x7);
  ptiming::g_topTid = GetCurrentThreadId();
  g_verify = true;
  {
    // calcoffsets: DCS-like buffers, the variant bound through the hook
    std::vector<uint32_t> cnt(19527);
    Rng rng{42};
    for (auto& v : cnt) v = rng() % 50;
    TBuf in, out;
    MakeBuf(dev, 19527, 0, cnt.data(), true, false, in);
    MakeBuf(dev, 19527, 0, nullptr, true, true, out);
    const uint32_t cbv[2] = {77, 19527};
    ID3D11Buffer* cb = MakeCb(dev, cbv, 8);
    auto run = [&](bool corrupt) {
      ctx->CSSetShader(stock[0], nullptr, 0);  // -> variant
      ctx->CSSetConstantBuffers(0, 1, &cb);
      ctx->CSSetShaderResources(0, 1, &in.srv);
      ctx->CSSetUnorderedAccessViews(0, 1, &out.uav, nullptr);
      PreDispatch(ctx);
      ctx->Dispatch(1, 1, 1);
      if (corrupt) {
        const UINT bad[4] = {7, 7, 7, 7};
        ctx->ClearUnorderedAccessViewUint(out.uav, bad);  // DCS's result spoiled before the check
      }
      PostDispatch(ctx, [&] { ctx->Dispatch(1, 1, 1); });
    };
    run(false);
    std::vector<uint32_t> res;
    ReadBuf(dev, ctx, g_res, 64, res);
    const uint32_t clean = res.size() ? res[0] : 1u;
    ID3D11ComputeShader* afterCs = nullptr;
    ctx->CSGetShader(&afterCs, nullptr, nullptr);
    ID3D11UnorderedAccessView* afterU = nullptr;
    ctx->CSGetUnorderedAccessViews(0, 1, &afterU);
    Check(clean == 0 && g_checks[0].load() == 1 && afterCs == g_e[0].variant && afterU == out.uav,
          "chain fast verify: calcoffsets variant vs stock re-run on the pre-dispatch copy: 0 words differ, state restored");
    if (afterCs) afterCs->Release();
    if (afterU) afterU->Release();
    run(true);
    ReadBuf(dev, ctx, g_res, 64, res);
    Check(res.size() && res[0] > 19000, "chain fast verify: a spoiled offsets buffer is reported");
    in.Release();
    out.Release();
    if (cb) cb->Release();
  }
  {
    // calccounters: one hot object plus invalid ids, ranks permuted by the variant
    const uint32_t n = 20000, cap = 20480, nObj = 19527;
    std::vector<uint32_t> main(cap * 16, 0);
    Rng rng{7};
    for (uint32_t i = 0; i < cap; ++i) main[i * 16] = (rng() % 40 == 0) ? nObj + 5 : (rng() % 3 ? 11u : rng() % 100);
    const uint32_t sums[8] = {n, 0, 0, 0, 0, 0, 0, 0};
    std::vector<uint32_t> zeros(nObj, 0);
    TBuf sb, mb, ib, cbuf;
    MakeBuf(dev, 8, 0, sums, true, false, sb);
    MakeBuf(dev, cap, 64, main.data(), false, true, mb);
    MakeBuf(dev, cap, 0, nullptr, false, true, ib);
    MakeBuf(dev, nObj, 0, zeros.data(), false, true, cbuf);
    const int cbv[2] = {524288, static_cast<int>(nObj)};
    ID3D11Buffer* cb = MakeCb(dev, cbv, 8);
    auto run = [&](bool corrupt) {
      const UINT z[4] = {0, 0, 0, 0};
      ctx->ClearUnorderedAccessViewUint(cbuf.uav, z);
      ctx->CSSetShader(stock[1], nullptr, 0);  // -> variant
      ctx->CSSetConstantBuffers(0, 1, &cb);
      ctx->CSSetShaderResources(0, 1, &sb.srv);
      ID3D11UnorderedAccessView* u[3] = {mb.uav, ib.uav, cbuf.uav};
      ctx->CSSetUnorderedAccessViews(0, 3, u, nullptr);
      PreDispatch(ctx);
      ctx->Dispatch((n >> 7) + 1, 1, 1);
      if (corrupt) {  // one rank duplicated: instance 0 takes instance 1's rank when both are object 11
        std::vector<uint32_t> r;
        ReadBuf(dev, ctx, ib.b, ib.bytes, r);
        uint32_t a = ~0u, b = ~0u;
        for (uint32_t i = 0; i < n && b == ~0u; ++i)
          if ((main[i * 16] & 0xffffff) == 11) (a == ~0u ? a : b) = i;
        if (b != ~0u) {
          D3D11_BOX box{a * 4, 0, 0, a * 4 + 4, 1, 1};
          ctx->UpdateSubresource(ib.b, 0, &box, &r[b], 0, 0);
        }
      }
      PostDispatch(ctx, [&] { ctx->Dispatch((n >> 7) + 1, 1, 1); });
    };
    std::vector<uint32_t> res0, res;
    ReadBuf(dev, ctx, g_res, 64, res0);
    run(false);
    ReadBuf(dev, ctx, g_res, 64, res);
    Check(res.size() && res0.size() && res[1] == res0[1] && g_checks[1].load() == 1,
          "chain fast verify: calccounters variant vs stock re-run: counts identical, ranks in range, per-object rank sums "
          "equal");
    run(true);
    ReadBuf(dev, ctx, g_res, 64, res);
    Check(res.size() && res[1] > res0[1], "chain fast verify: a duplicated rank is reported");
    sb.Release();
    mb.Release();
    ib.Release();
    cbuf.Release();
    if (cb) cb->Release();
  }
  g_verify = false;
  ptiming::g_topTid = 0;

  // --- shutdown: hooks out, pairs released
  void** cs = g_createSlot;
  void** ss = g_setSlot;
  // R48: under the loader (a registry that keeps the first entry it saw). d3d11 has rewritten the CSSetShader entry
  // since (RearmSet took the new one): Shutdown must put back d3d11's current entry, not the registry's stale one.
  R48Api::real = g_setOrig;
  R48Api::createSlot = cs;
  R48Api::createOrig = reinterpret_cast<void*>(g_createOrig);
  R48Api::setSlot = ss;
  const DcsQvLoaderApi* const api0 = g_api;
  const DcsQvLoaderApi fakeApi{kDcsQvLoaderApiVersion, &R48Api::Patch, &R48Api::Original, &R48Api::Restore,
                               &R48Api::LogLine, L""};
  g_api = &fakeApi;
  Shutdown();
  g_api = api0;
  const bool current = ss && *ss == reinterpret_cast<void*>(R48Api::real);
  if (ss && *ss == reinterpret_cast<void*>(&R48Api::Stale)) d3ds::WriteTablePointer(ss, reinterpret_cast<void*>(R48Api::real));
  Check(current, "R48 chain fast: shutdown under the loader restores d3d11's current CSSetShader entry, not a stale one");
  Check(cs && *cs != reinterpret_cast<void*>(&H_CreateCs) && ss && *ss != reinterpret_cast<void*>(&H_CSSetShader) &&
            g_n.load() == 0 && g_state.load() == 0 && bound(stock[0]) == stock[0],
        "chain fast: shutdown restores both table entries and releases the pairs");
  for (int k = 0; k < kKinds; ++k) {
    g_testShexFnv[k] = 0;
    if (stock[k]) stock[k]->Release();
  }
  if (other) other->Release();

  // --- identification at bind time: stock kernels created before the payload (live 2.9.30 case)
  if (fakeOk) {
    ID3D11ComputeShader* pre[kKinds] = {};
    for (int k = 0; k < kKinds; ++k) dev->CreateComputeShader(fake[k].data(), fake[k].size(), nullptr, &pre[k]);
    // A decoy with calcoffsets' shape and bindings but another function: must be rejected by its output.
    const char kDecoy[] = R"(
cbuffer Globals : register(b0) { uint chunkSize; uint counts; };
Buffer<uint> countsIn : register(t0);
RWBuffer<uint> offsetsOut : register(u0);
[numthreads(256, 1, 1)]
void main(uint tid : SV_GroupIndex) {
  for (uint i = tid; i < counts; i += 256) offsetsOut[i] = countsIn[i] + 1;
}
)";
    std::vector<uint8_t> decoyCode;
    ID3D11ComputeShader* decoy = nullptr;
    if (Compile(kDecoy, "decoy", decoyCode, nullptr))
      dev->CreateComputeShader(decoyCode.data(), decoyCode.size(), nullptr, &decoy);
    shadowinst::g_device = dev;
    Apply(0x7);
    Check(g_n.load() == 0 && g_identOn.load() && g_ctx == ctx,
          "chain fast id: kernels not seen at creation -> probe hooks on DCS's context (Dispatch, DispatchIndirect)");
    {  // R77: ChainFast 0 while the probe is in withdraws it (ShaderFast can take Dispatch); back on, it probes again
      Apply(0);
      bound(nullptr);
      void** vt0 = *reinterpret_cast<void***>(ctx);
      const bool out = !g_identOn.load() && vt0[DcsQv_ContextSlot("Dispatch")] != reinterpret_cast<void*>(&H_IdDispatch) &&
                       vt0[DcsQv_ContextSlot("DispatchIndirect")] != reinterpret_cast<void*>(&H_IdDispatchIndirect);
      Apply(0x7);
      Check(out && g_identOn.load() && !g_identStop.load(),
            "R77: chain fast id: ChainFast=0 withdraws the bind-time probe; ChainFast back on probes again");
    }
    // "DCS" dispatches through the hooked context: the decoy first, then the three stock kernels.
    std::vector<uint32_t> cnt(19527, 3), z(19527, 0);
    TBuf in, out, ain, aout, sb, mb, ib, cbuf, args;
    MakeBuf(dev, 19527, 0, cnt.data(), true, false, in);
    MakeBuf(dev, 19527, 0, z.data(), false, true, out);
    MakeBuf(dev, 10000, 4, cnt.data(), true, false, ain);
    MakeBuf(dev, 10000, 4, z.data(), false, true, aout);
    const uint32_t sums[8] = {640, 0, 0, 0, 0, 0, 0, 0};
    std::vector<uint32_t> main(1024 * 16, 0);
    for (uint32_t i = 0; i < 1024; ++i) main[i * 16] = i % 7;
    MakeBuf(dev, 8, 0, sums, true, false, sb);
    MakeBuf(dev, 1024, 64, main.data(), false, true, mb);
    MakeBuf(dev, 1024, 0, z.data(), false, true, ib);
    MakeBuf(dev, 19527, 0, z.data(), false, true, cbuf);
    D3D11_BUFFER_DESC ad{};
    ad.ByteWidth = 12;
    ad.Usage = D3D11_USAGE_DEFAULT;
    ad.MiscFlags = D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS;
    const UINT argv[3] = {6, 1, 1};
    D3D11_SUBRESOURCE_DATA asd{argv, 0, 0};
    dev->CreateBuffer(&ad, &asd, &args.b);
    const uint32_t cbo[2] = {77, 19527};
    const int cbc[2] = {524288, 19527};
    ID3D11Buffer* cb0 = MakeCb(dev, cbo, 8);
    ID3D11Buffer* cb1 = MakeCb(dev, cbc, 8);
    auto frame = [&]() {
      BindCs(ctx, decoy, cb0, in.srv, &out.uav, 1);
      ctx->Dispatch(1, 1, 1);
      BindCs(ctx, pre[0], cb0, in.srv, &out.uav, 1);
      ctx->Dispatch(1, 1, 1);
      BindCs(ctx, pre[2], cb0, ain.srv, &aout.uav, 1);
      ctx->Dispatch(1, 1, 1);
      ID3D11UnorderedAccessView* u[3] = {mb.uav, ib.uav, cbuf.uav};
      BindCs(ctx, pre[1], cb1, sb.srv, u, 3);
      ctx->DispatchIndirect(args.b, 0);
      UnbindCs(ctx);
      ctx->Flush();
    };
    for (int i = 0; i < 400 && (g_found.load() != 0x7 || g_candTested < 4 || UsableMask() != 0x7); ++i) {
      frame();
      Apply(0x7);
      Sleep(2);
    }
    char what[200];
    sprintf_s(what, "chain fast id: the three stock objects are taken by their output, the decoy is rejected (found 0x%x, "
              "%d tested, %d rejected, usable 0x%x)", g_found.load(), g_candTested, g_candRejected, UsableMask());
    Check(g_found.load() == 0x7 && g_candTested == 4 && g_candRejected == 1 && UsableMask() == 0x7 && g_n.load() == 3,
          what);
    Check(bound(pre[0]) == g_e[0].variant || bound(pre[0]) == g_e[1].variant || bound(pre[0]) == g_e[2].variant,
          "chain fast id: binding an identified stock object binds its variant");
    Check(bound(decoy) == decoy, "chain fast id: the rejected decoy passes through");
    for (int i = 0; i < 5; ++i) {
      Apply(0x7);
      bound(nullptr);  // the render thread withdraws the probe hooks
    }
    void** vt = *reinterpret_cast<void***>(ctx);
    Check(!g_identOn.load() && vt[DcsQv_ContextSlot("Dispatch")] != reinterpret_cast<void*>(&H_IdDispatch) &&
              vt[DcsQv_ContextSlot("DispatchIndirect")] != reinterpret_cast<void*>(&H_IdDispatchIndirect),
          "chain fast id: probe hooks withdrawn once every kind is found");
    Shutdown();
    Check(g_n.load() == 0 && g_found.load() == 0 && bound(pre[0]) == pre[0], "chain fast id: shutdown releases the pairs");
    for (TBuf* b : {&in, &out, &ain, &aout, &sb, &mb, &ib, &cbuf, &args}) b->Release();
    cb0->Release();
    cb1->Release();
    for (auto* k : pre)
      if (k) k->Release();
    if (decoy) decoy->Release();
  }
  shadowinst::g_device = saveDev;
  sfilt::g_ctx = saveSf;
  ctx->ClearState();
  ctx->Release();
  dev->Release();
}

}  // namespace cftest
