// FX pass Apply repeat counter and lean Apply (fable-perf-audit A3, R39 section 4.3).
// [D3D] FxApplySkip, default 0 (off: no hook, stock).
//
// Every FX pass Apply goes through SPassBlock vtable slot 13 (0x65850), which
// stores the context in effect+0x158, calls ApplyPassBlock 0x67f90 and clears
// effect+0x158 again. What 0x67f90 does [V dx11backend.dll 0x6aae1d9d,
// disassembled 2026-11-11; fx_apply_count.h has the block layouts]:
//   1. 0x67e10 pass assignments: effect+0x140 (FX timer) += 1; per record
//      (pass+0x10 count, pass+0x18 array, 0x38 bytes): types 2 / 4 / 8 copy or
//      index a variable when the variable's change time (var+0x40, deps at
//      rec+0x18) is >= the record's last evaluation (rec+8); every record then
//      gets rec+8 = timer. Nothing else is written.
//   2. blend / depth / raster blocks (pass+0x58/0x60/0x68) through 0x68240:
//      user-managed (blk+4 bit 0) -> nothing; else 0x67cc0 evaluates the
//      block's records the same way (no timer step) and only when one was
//      re-evaluated releases and re-creates the state object (blk+0x18).
//      Then pass+0x20 = blend obj, OMSetBlendState(pass+0x20, pass+0x28,
//      [pass+0x38]); pass+0x40 = depth obj, OMSetDepthStencilState(pass+0x40,
//      [pass+0x48]); RSSetState([raster + rs*8 + 0x18]) - three context-table
//      call sites (0x67ff1 / 0x68022 / 0x6804c, the split filter's sites).
//   3. pass+0x78 RT block -> OMSetRenderTargets; CS block -> VS / PS / CS.
//   4. ApplyShaderBlock 0x68470 for VS, PS, GS, HS, DS (that order): per CB
//      dependency the dirty FX buffers (flags +0x5c bit 0, not bit 7) are
//      uploaded with UpdateSubresource (table 0x180) and un-dirtied, then the
//      call table's SetConstantBuffers; per sampler dependency 0x68240 on each
//      sampler block, then SetSamplers; UAV dependencies (+0x60) refresh their
//      bound arrays (only CS binds them); dirty tbuffers uploaded; per SRV
//      dependency the bound array (+0x10) is refreshed from the variables
//      (*fx[i]) and DX11Renderer::setShaderResources 0x1ac20 is called with the
//      views of all dependencies so far (it keeps a per-stage cache in the
//      renderer, srv_span_count.h, and binds only what differs); SetShader.
//      0x5e210 maps the call table to a type: 0x1c (CS) reaches the UAV
//      setter 0x1a590, 0x19 (-> 0x1ab40) is never returned.
//   5. Tessellation: if neither HS nor DS has a shader and the renderer's
//      flag (renderer+0xd8) is set, HSSetShader(null) and DSSetShader(null);
//      the flag is then the new HS/DS state.
//
// Lean Apply (exact by construction): when no record would be re-evaluated
// (pass, blend/depth/raster and sampler blocks), there is no RT block, no CS
// block, no UAV dependency and the tessellation branch would not call, the
// whole of 0x67f90 reduces to: the timer step and the record stamps, the two
// pass fields, the dirty uploads, and the very same state calls through the
// very same entry points (the three call sites' current targets - split
// filter stubs or the context table -, the block call tables, the SRV call
// site's current target 0x688b8) with the same arguments in the same order.
// A stage with one SRV dependency whose views the renderer cache already holds
// (count <= n, slots equal) binds nothing in stock either: only the count and
// the bound array are written. The lean path skips the FX bookkeeping around
// those calls (stack frames and cookies, 0x68240 / 0x67cc0 / 0x5e210 / 0x15da0
// calls, the redundant 0x1ac20 call); every D3D call stock makes is made.
// Anything else (a record to evaluate, a fault reading the structures, a call
// site with unexpected bytes) runs stock Apply.
//
// Modes ([D3D] FxApplySkip): 1 = count only (stock Apply always); 2 = lean
// Apply on byte-identical repeats (same pass, raster index and context as the
// previous render-thread Apply, same signature of everything it binds);
// 3 = lean Apply on every eligible Apply. The line "fx repeat:" every
// [D3D] FxApplySkipLogSec (default 10 s): Applies, same-pass repeats,
// identical repeats and eligible ones per frame (all callers and inside
// DX11Renderer::drawIndirect, return address 0x14f8f), render-thread time
// from rdtsc on every 8th Apply (stock time in them, lean time when skipping,
// the check's own time), refusal reasons, verify results.
// [D3D] FxApplySkipVerify=1: every [D3D] FxApplySkipVerifyEvery-th lean
// decision runs stock Apply instead and compares, against what the lean path
// predicts, the FX memory it writes (timer, stamps, pass fields, state
// objects, CB flags, SRV bound arrays, the renderer's SRV cache and count, the
// tessellation flag, effect+0x158) and the context state read back with Get*
// (blend, depth, raster, shaders, CB / sampler / SRV slots). A mismatch
// latches the lean path off for the session and logs a sample.
// [Suite] BenchFxApplySkip: bench mode 36, OFF = hook out (stock), ON = mode 2.
// Fail-safe: vtable, slot or code-hash mismatch (masked at the split filter's
// and srv trim's call sites) -> off with a log line.
// Included once from main.cpp inside its anonymous namespace, after
// srv_span_count.h (srvspan::Fn), split_filter.h and alloc_slab.h (ReadBytes, RttiIs).
#pragma once

#include <intrin.h>

namespace fxskip {

// ---------------------------------------------------------------------------
// Layout [V dx11backend.dll 0x6aae1d9d]
// ---------------------------------------------------------------------------

constexpr uint32_t kPassVtbl = 0xb96b0;  // .?AUSPassBlock@D3DX11Effects@@
constexpr int kSlot = 13;
constexpr uint32_t kApply = 0x65850, kRendererGet = 0x15da0, kDiRet = 0x14f8f;
constexpr uint32_t kSiteBlend = 0x67ff1, kSiteDepth = 0x68022, kSiteRaster = 0x6804c, kSiteSrv = 0x688b8;

struct CodeRange {
  uint32_t begin, end;
  uint64_t hash;  // FNV-1a 64 of the file bytes, kMasks zeroed
};
const CodeRange kCode[] = {
    {0x65850, 0x65893, 0x82ae1bb9caeeadf9ull},  // SPassBlock::Apply
    {0x67cc0, 0x67e0f, 0x9f4b397c523ed264ull},  // state block records
    {0x67e10, 0x67f8a, 0x6283ccf029d1aeb9ull},  // pass records
    {0x67f90, 0x68234, 0xbcf70d5bc593c0abull},  // ApplyPassBlock
    {0x68240, 0x68463, 0x08d547beb0aed40bull},  // ApplyRenderStateBlock
    {0x68470, 0x68918, 0x6182750f188947baull},  // ApplyShaderBlock
    {0x15da0, 0x15dab, 0xb18d1e0de33e026dull},  // renderer getter
    {0x1ac20, 0x1ad0b, 0xce5d23c1a21ed3d1ull},  // DX11Renderer::setShaderResources (srvspan::kFnHash)
    {0x1a590, 0x1a5c1, 0xdbcb1676e2cdf257ull},  // CS UAV setter
    {0x5e210, 0x5e283, 0x05f55acc9fafcfefull},  // call table -> type
    {0x14f8c, 0x14f8f, 0xf87dc11be38b62f6ull},  // drawIndirect: call [rax+0x68]
};
struct Mask {
  uint32_t at, n;
};
// Patched at run time by the split filter (6 bytes) and the srv trim / span counter (5 bytes).
const Mask kMasks[] = {{kSiteBlend, 6}, {kSiteDepth, 6}, {kSiteRaster, 6}, {0x68546, 5}, {kSiteSrv, 5}};

// Pass.
constexpr uint32_t kPAssignN = 0x10, kPAssign = 0x18, kPBlendObj = 0x20, kPFactor = 0x28, kPMask = 0x38,
                   kPDepthObj = 0x40, kPRef = 0x48, kPBlendBlk = 0x58, kPDepthBlk = 0x60, kPRasterBlk = 0x68,
                   kPRtBlk = 0x78, kPCs = 0xd8, kPEffect = 0x108;
constexpr int kStages = 5;  // apply order
constexpr uint32_t kStageOff[kStages] = {0xc0, 0xc8, 0xd0, 0xe8, 0xe0};
constexpr int kStageIdx[kStages] = {0, 4, 1, 2, 3};  // setShaderResources stage index
const char* const kStageNames[kStages] = {"VS", "PS", "GS", "HS", "DS"};
// Effect.
constexpr uint32_t kETimer = 0x140, kECtx = 0x158;
// Assignment record.
constexpr uint32_t kRecSize = 0x38, kRecType = 4, kRecLast = 8, kRecDeps = 0x18, kVarMod = 0x40;
// State block.
constexpr uint32_t kSbFlags = 4, kSbAssignN = 8, kSbAssign = 0x10, kSbObj = 0x18;
// Shader block and its dependencies (0x20 each: start, count, fx pointers, bound).
constexpr uint32_t kBTable = 8, kBShader = 0x18, kBCbN = 0x20, kBCb = 0x28, kBSampN = 0x30, kBSamp = 0x38,
                   kBSrvN = 0x50, kBSrv = 0x58, kBUavN = 0x60, kBTbN = 0x70, kBTb = 0x78;
constexpr uint32_t kDepSize = 0x20, kDStart = 0, kDCount = 4, kDFx = 8, kDBound = 0x10;
// SConstantBuffer.
constexpr uint32_t kCbBuffer = 0x10, kCbBytes = 0x20, kCbSize = 0x28, kCbFlags = 0x5c;
// DX11Renderer.
constexpr uint32_t kRCtx = 0x30, kRTess = 0xd8, kRSrvCount = 0xf8, kRSrvCache = 0x118, kRSrvStride = 0x400;
// Context table byte offsets used directly by 0x67f90 / 0x68470.
constexpr uint32_t kVtUpdate = 0x180, kVtHsSet = 0x1e0, kVtDsSet = 0x200;

constexpr uint32_t kMaxRecs = 256, kMaxDeps = 64, kMaxSlots = 128;

template <class T>
inline T& At(const void* p, uint64_t off) {
  return *reinterpret_cast<T*>(const_cast<uint8_t*>(static_cast<const uint8_t*>(p)) + off);
}

using SetBlendFn = void(STDMETHODCALLTYPE*)(void* ctx, void* state, const float* factor, UINT mask);
using SetDepthFn = void(STDMETHODCALLTYPE*)(void* ctx, void* state, UINT ref);
using SetOneFn = void(STDMETHODCALLTYPE*)(void* ctx, void* obj);
using SetRangeFn = void(STDMETHODCALLTYPE*)(void* ctx, UINT start, UINT n, void* const* v);
using SetShaderFn = void(STDMETHODCALLTYPE*)(void* ctx, void* shader, void* const* ci, UINT nci);
using UpdateFn = void(STDMETHODCALLTYPE*)(void* ctx, void* res, UINT sub, const void* box, const void* data,
                                          UINT row, UINT depth);
using SrvFn = srvspan::Fn;

inline void* VtEntry(void* ctx, uint32_t off) { return *reinterpret_cast<void**>(*static_cast<uint8_t**>(ctx) + off); }

// What a 6-byte context-table call site calls now: `call [rax+off32]` (stock) or
// `nop; call rel32` (split filter stub). One 8-byte load: a patch writer
// suspends every thread, and a load is never split by a suspension.
inline void* Via6(const uint8_t* site, void* ctx) {
  const uint64_t w = *reinterpret_cast<const volatile uint64_t*>(site);
  const uint8_t b0 = static_cast<uint8_t>(w), b1 = static_cast<uint8_t>(w >> 8);
  const int32_t rel = static_cast<int32_t>(static_cast<uint32_t>(w >> 16));
  if (b0 == 0x90 && b1 == 0xE8) return const_cast<uint8_t*>(site) + 6 + rel;
  if (b0 == 0xFF && b1 == 0x90 && rel >= 0 && rel < 0x400) return VtEntry(ctx, static_cast<uint32_t>(rel));
  return nullptr;
}
// What the 5-byte `call rel32` at the SRV site calls now (0x1ac20, or a trim / counter stub).
inline void* Via5(const uint8_t* site) {
  const uint64_t w = *reinterpret_cast<const volatile uint64_t*>(site);
  if (static_cast<uint8_t>(w) != 0xE8) return nullptr;
  return const_cast<uint8_t*>(site) + 5 + static_cast<int32_t>(static_cast<uint32_t>(w >> 8));
}

// ---------------------------------------------------------------------------
// The decision (read only)
// ---------------------------------------------------------------------------

enum Why : uint8_t { kOk, kRt, kCompute, kUav, kAssign, kStateAssign, kTess, kOverflow, kWhyCount };
const char* const kWhyNames[kWhyCount] = {"ok", "rt", "compute", "uav", "assign", "state-assign", "tess", "overflow"};

// 0x67e10 / 0x67cc0: is this record evaluated (the `jb` not taken)?
inline bool WouldEvaluate(const uint8_t* rec) {
  const uint32_t type = At<uint32_t>(rec, kRecType);
  if (type != 2 && type != 4 && type != 8) return false;
  const uint64_t last = At<uint64_t>(rec, kRecLast);
  void* const* deps = At<void* const*>(rec, kRecDeps);
  if (At<uint64_t>(deps[0], kVarMod) >= last) return true;
  return type == 4 && At<uint64_t>(deps[1], kVarMod) >= last;
}
// Returns false when a record would be evaluated (or too many to look at).
inline bool RecordsQuiet(const uint8_t* recs, uint32_t n) {
  if (n > kMaxRecs) return false;
  for (uint32_t i = 0; i < n; ++i)
    if (WouldEvaluate(recs + i * kRecSize)) return false;
  return true;
}
inline bool StateBlockQuiet(const uint8_t* blk) {
  return (At<uint8_t>(blk, kSbFlags) & 1) || RecordsQuiet(At<uint8_t*>(blk, kSbAssign), At<uint32_t>(blk, kSbAssignN));
}
inline bool CbDirty(const uint8_t* cb) {
  const uint8_t f = At<uint8_t>(cb, kCbFlags);
  return (f & 1) && !(f & 0x80);
}

struct Fnv {
  uint64_t h = 0xcbf29ce484222325ull;
  void Add(uint64_t v) {
    for (int i = 0; i < 8; ++i) {
      h ^= static_cast<uint8_t>(v >> (8 * i));
      h *= 0x100000001b3ull;
    }
  }
  void Add(const void* p) { Add(reinterpret_cast<uintptr_t>(p)); }
};

struct Plan {
  uint64_t sig = 0;      // FNV over everything the Apply binds (and the pass, raster index)
  uint32_t uploads = 0;  // dirty FX constant buffers / tbuffers it uploads
  uint8_t why = kOk;     // first reason the lean path cannot run
  uint8_t srvFast = 0;   // bit k (apply order): one SRV dependency already in the renderer cache, nothing bound
  bool tess = false;     // HS or DS has a shader
};

// Fills `p` from the live structures. Reads only.
inline void Check(const uint8_t* pass, uint32_t rs, const uint8_t* r, Plan& p) {
  p = Plan{};
  Fnv f;
  auto refuse = [&](Why w) {
    if (p.why == kOk) p.why = w;
  };
  f.Add(pass);
  f.Add(rs);
  if (At<void*>(pass, kPRtBlk)) refuse(kRt);
  if (At<void*>(pass, kPCs)) refuse(kCompute);
  if (!RecordsQuiet(At<uint8_t*>(pass, kPAssign), At<uint32_t>(pass, kPAssignN))) refuse(kAssign);
  if (const uint8_t* b = At<uint8_t*>(pass, kPBlendBlk)) {
    if (!StateBlockQuiet(b)) refuse(kStateAssign);
    f.Add(At<void*>(b, kSbObj));
    f.Add(At<uint64_t>(pass, kPFactor));
    f.Add(At<uint64_t>(pass, kPFactor + 8));
    f.Add(At<uint32_t>(pass, kPMask));
  }
  if (const uint8_t* b = At<uint8_t*>(pass, kPDepthBlk)) {
    if (!StateBlockQuiet(b)) refuse(kStateAssign);
    f.Add(At<void*>(b, kSbObj));
    f.Add(At<uint32_t>(pass, kPRef));
  }
  if (const uint8_t* b = At<uint8_t*>(pass, kPRasterBlk)) {
    if (!StateBlockQuiet(b)) refuse(kStateAssign);
    f.Add(At<void*>(b, static_cast<uint64_t>(rs) * 8 + kSbObj));
  }
  for (int k = 0; k < kStages; ++k) {
    const uint8_t* blk = At<uint8_t*>(pass, kStageOff[k]);
    f.Add(blk);
    if (!blk) continue;
    f.Add(At<void*>(blk, kBShader));
    if (At<uint32_t>(blk, kBUavN)) refuse(kUav);
    const uint32_t nCb = At<uint32_t>(blk, kBCbN), nSamp = At<uint32_t>(blk, kBSampN), nSrv = At<uint32_t>(blk, kBSrvN),
                   nTb = At<uint32_t>(blk, kBTbN);
    if (nCb > kMaxDeps || nSamp > kMaxDeps || nSrv > kMaxDeps || nTb > kMaxDeps) {
      refuse(kOverflow);
      continue;
    }
    for (uint32_t d = 0; d < nCb; ++d) {
      const uint8_t* dep = At<uint8_t*>(blk, kBCb) + d * kDepSize;
      const uint32_t n = At<uint32_t>(dep, kDCount);
      if (n > kMaxSlots) {
        refuse(kOverflow);
        break;
      }
      f.Add(At<uint32_t>(dep, kDStart) | static_cast<uint64_t>(n) << 32);
      for (uint32_t i = 0; i < n; ++i) {
        const uint8_t* cb = At<uint8_t**>(dep, kDFx)[i];
        if (CbDirty(cb)) p.uploads++;
        f.Add(At<void**>(dep, kDBound)[i]);
      }
    }
    for (uint32_t d = 0; d < nSamp; ++d) {
      const uint8_t* dep = At<uint8_t*>(blk, kBSamp) + d * kDepSize;
      const uint32_t n = At<uint32_t>(dep, kDCount);
      if (n > kMaxSlots) {
        refuse(kOverflow);
        break;
      }
      f.Add(At<uint32_t>(dep, kDStart) | static_cast<uint64_t>(n) << 32);
      for (uint32_t i = 0; i < n; ++i) {
        if (!StateBlockQuiet(At<uint8_t**>(dep, kDFx)[i])) refuse(kStateAssign);
        f.Add(At<void**>(dep, kDBound)[i]);
      }
    }
    for (uint32_t i = 0; i < nTb; ++i)
      if (CbDirty(At<uint8_t**>(blk, kBTb)[i])) p.uploads++;
    uint32_t total = 0;
    for (uint32_t d = 0; d < nSrv; ++d) {
      const uint8_t* dep = At<uint8_t*>(blk, kBSrv) + d * kDepSize;
      const uint32_t n = At<uint32_t>(dep, kDCount);
      if (n > kMaxSlots - total) {
        refuse(kOverflow);
        break;
      }
      total += n;
      for (uint32_t i = 0; i < n; ++i) f.Add(*At<void***>(dep, kDFx)[i]);
    }
    if (nSrv == 1 && total <= kMaxSlots) {
      const int st = kStageIdx[k];
      const uint32_t cnt = At<uint32_t>(r, kRSrvCount + st * 4);
      void* const* cache = reinterpret_cast<void* const*>(r + kRSrvCache + st * kRSrvStride);
      const uint8_t* dep = At<uint8_t*>(blk, kBSrv);
      bool same = cnt <= total;
      for (uint32_t i = 0; same && i < total; ++i) same = cache[i] == *At<void***>(dep, kDFx)[i];
      if (same) p.srvFast |= static_cast<uint8_t>(1u << k);
    }
  }
  const uint8_t* hs = At<uint8_t*>(pass, kStageOff[3]);
  const uint8_t* ds = At<uint8_t*>(pass, kStageOff[4]);
  p.tess = (hs && At<void*>(hs, kBShader)) || (ds && At<void*>(ds, kBShader));
  if (!p.tess && At<uint8_t>(r, kRTess)) refuse(kTess);
  f.Add(p.uploads);  // an upload changes buffer contents: not a byte-identical repeat
  p.sig = f.h;
}

// ---------------------------------------------------------------------------
// The lean Apply (only after Check said kOk on the same structures)
// ---------------------------------------------------------------------------

struct Sites {
  const uint8_t* blend;
  const uint8_t* depth;
  const uint8_t* raster;
  const uint8_t* srv;
};

inline void Stamp(uint8_t* recs, uint32_t n, uint64_t t) {
  for (uint32_t i = 0; i < n; ++i) At<uint64_t>(recs + i * kRecSize, kRecLast) = t;
}
inline void StampBlock(uint8_t* blk, uint64_t t) {
  if (!(At<uint8_t>(blk, kSbFlags) & 1)) Stamp(At<uint8_t*>(blk, kSbAssign), At<uint32_t>(blk, kSbAssignN), t);
}
inline void Upload(void* ctx, uint8_t* cb) {
  if (!CbDirty(cb)) return;
  const UINT size = At<uint32_t>(cb, kCbSize);
  reinterpret_cast<UpdateFn>(VtEntry(ctx, kVtUpdate))(ctx, At<void*>(cb, kCbBuffer), 0, nullptr, At<void*>(cb, kCbBytes),
                                                      size, size);
  At<uint8_t>(cb, kCbFlags) &= 0xfe;
}

inline void RunStage(void* ctx, uint8_t* r, uint8_t* blk, int k, bool srvFast, SrvFn srvFn, uint64_t t) {
  void* const* tbl = At<void* const*>(blk, kBTable);
  const int st = kStageIdx[k];
  for (uint32_t d = 0; d < At<uint32_t>(blk, kBCbN); ++d) {
    uint8_t* dep = At<uint8_t*>(blk, kBCb) + d * kDepSize;
    for (uint32_t i = 0; i < At<uint32_t>(dep, kDCount); ++i) Upload(ctx, At<uint8_t**>(dep, kDFx)[i]);
    reinterpret_cast<SetRangeFn>(tbl[1])(ctx, At<uint32_t>(dep, kDStart), At<uint32_t>(dep, kDCount),
                                         At<void**>(dep, kDBound));
  }
  for (uint32_t d = 0; d < At<uint32_t>(blk, kBSampN); ++d) {
    uint8_t* dep = At<uint8_t*>(blk, kBSamp) + d * kDepSize;
    for (uint32_t i = 0; i < At<uint32_t>(dep, kDCount); ++i) StampBlock(At<uint8_t**>(dep, kDFx)[i], t);
    reinterpret_cast<SetRangeFn>(tbl[2])(ctx, At<uint32_t>(dep, kDStart), At<uint32_t>(dep, kDCount),
                                         At<void**>(dep, kDBound));
  }
  for (uint32_t i = 0; i < At<uint32_t>(blk, kBTbN); ++i) Upload(ctx, At<uint8_t**>(blk, kBTb)[i]);
  const uint32_t nSrv = At<uint32_t>(blk, kBSrvN);
  if (nSrv && srvFast) {
    uint8_t* dep = At<uint8_t*>(blk, kBSrv);
    const uint32_t n = At<uint32_t>(dep, kDCount);
    for (uint32_t i = 0; i < n; ++i) At<void**>(dep, kDBound)[i] = *At<void***>(dep, kDFx)[i];
    At<uint32_t>(r, kRSrvCount + st * 4) = n;  // 0x1ac20: cache slots already equal, nothing bound
  } else if (nSrv) {
    void* views[kMaxSlots];
    uint32_t c = 0;
    for (uint32_t d = 0; d < nSrv; ++d) {
      uint8_t* dep = At<uint8_t*>(blk, kBSrv) + d * kDepSize;
      for (uint32_t i = 0; i < At<uint32_t>(dep, kDCount); ++i) {
        void* v = *At<void***>(dep, kDFx)[i];
        At<void**>(dep, kDBound)[i] = v;
        views[c++] = v;
      }
      srvFn(r, st, tbl[3], 0, c, views);
    }
  }
  reinterpret_cast<SetShaderFn>(tbl[0])(ctx, At<void*>(blk, kBShader), nullptr, 0);
}

// Returns false (nothing done) when a call site holds unexpected bytes.
inline bool Run(uint8_t* pass, void* ctx, uint32_t rs, uint8_t* r, const Plan& p, const Sites& s) {
  uint8_t* blend = At<uint8_t*>(pass, kPBlendBlk);
  uint8_t* depth = At<uint8_t*>(pass, kPDepthBlk);
  uint8_t* raster = At<uint8_t*>(pass, kPRasterBlk);
  void* fBlend = blend ? Via6(s.blend, ctx) : nullptr;
  void* fDepth = depth ? Via6(s.depth, ctx) : nullptr;
  void* fRaster = raster ? Via6(s.raster, ctx) : nullptr;
  if ((blend && !fBlend) || (depth && !fDepth) || (raster && !fRaster)) return false;
  SrvFn fSrv = nullptr;
  for (int k = 0; k < kStages; ++k) {
    const uint8_t* blk = At<uint8_t*>(pass, kStageOff[k]);
    if (blk && At<uint32_t>(blk, kBSrvN) && !((p.srvFast >> k) & 1)) {
      fSrv = reinterpret_cast<SrvFn>(Via5(s.srv));
      if (!fSrv) return false;
      break;
    }
  }
  uint8_t* fx = At<uint8_t*>(pass, kPEffect);
  At<void*>(fx, kECtx) = ctx;  // 0x65850
  const uint64_t t = ++At<uint64_t>(fx, kETimer);
  Stamp(At<uint8_t*>(pass, kPAssign), At<uint32_t>(pass, kPAssignN), t);
  if (blend) {
    StampBlock(blend, t);
    At<void*>(pass, kPBlendObj) = At<void*>(blend, kSbObj);
    reinterpret_cast<SetBlendFn>(fBlend)(ctx, At<void*>(pass, kPBlendObj), &At<float>(pass, kPFactor),
                                         At<uint32_t>(pass, kPMask));
  }
  if (depth) {
    StampBlock(depth, t);
    At<void*>(pass, kPDepthObj) = At<void*>(depth, kSbObj);
    reinterpret_cast<SetDepthFn>(fDepth)(ctx, At<void*>(pass, kPDepthObj), At<uint32_t>(pass, kPRef));
  }
  if (raster) {
    StampBlock(raster, t);
    reinterpret_cast<SetOneFn>(fRaster)(ctx, At<void*>(raster, static_cast<uint64_t>(rs) * 8 + kSbObj));
  }
  for (int k = 0; k < kStages; ++k)
    if (uint8_t* blk = At<uint8_t*>(pass, kStageOff[k])) RunStage(ctx, r, blk, k, (p.srvFast >> k) & 1, fSrv, t);
  const uint8_t* hs = At<uint8_t*>(pass, kStageOff[3]);
  const uint8_t* ds = At<uint8_t*>(pass, kStageOff[4]);
  const bool tess = (hs && At<void*>(hs, kBShader)) || (ds && At<void*>(ds, kBShader));
  if (!tess && At<uint8_t>(r, kRTess)) {  // Check refuses this; kept as stock for completeness
    reinterpret_cast<SetShaderFn>(VtEntry(ctx, kVtHsSet))(ctx, nullptr, nullptr, 0);
    reinterpret_cast<SetShaderFn>(VtEntry(ctx, kVtDsSet))(ctx, nullptr, nullptr, 0);
  }
  At<uint8_t>(r, kRTess) = tess ? 1 : 0;
  At<void*>(fx, kECtx) = nullptr;  // 0x65850
  return true;
}

// ---------------------------------------------------------------------------
// Verify: what the lean path predicts, compared after stock Apply
// ---------------------------------------------------------------------------

struct MemExp {
  void* at;
  uint64_t want;
  uint8_t size;  // 1, 4 or 8
};
constexpr int kMaxExp = 2048;

struct ExpList {
  MemExp e[kMaxExp];
  int n = 0;
  bool overflow = false;
  void Add(void* at, uint64_t want, uint8_t size) {
    if (n < kMaxExp)
      e[n++] = {at, want, size};
    else
      overflow = true;
  }
  void AddRecs(uint8_t* recs, uint32_t cnt, uint64_t t) {
    for (uint32_t i = 0; i < cnt; ++i) Add(recs + i * kRecSize + kRecLast, t, 8);
  }
  void AddBlock(uint8_t* blk, uint64_t t) {
    Add(blk + kSbObj, reinterpret_cast<uintptr_t>(At<void*>(blk, kSbObj)), 8);  // not re-created
    if (!(At<uint8_t>(blk, kSbFlags) & 1)) AddRecs(At<uint8_t*>(blk, kSbAssign), At<uint32_t>(blk, kSbAssignN), t);
  }
  void AddCb(uint8_t* cb) {
    if (CbDirty(cb)) Add(cb + kCbFlags, At<uint8_t>(cb, kCbFlags) & 0xfe, 1);
  }
};

inline uint64_t ReadSized(const void* at, uint8_t size) {
  return size == 1 ? At<uint8_t>(at, 0) : size == 4 ? At<uint32_t>(at, 0) : At<uint64_t>(at, 0);
}

// Everything the lean path writes, with the values it writes (plus the state
// objects that stay). Call before Apply runs.
inline void PredictMem(uint8_t* pass, uint32_t rs, uint8_t* r, const Plan& p, ExpList& x) {
  (void)rs;
  x.n = 0;
  x.overflow = false;
  uint8_t* fx = At<uint8_t*>(pass, kPEffect);
  const uint64_t t = At<uint64_t>(fx, kETimer) + 1;
  x.Add(fx + kETimer, t, 8);
  x.Add(fx + kECtx, 0, 8);
  x.AddRecs(At<uint8_t*>(pass, kPAssign), At<uint32_t>(pass, kPAssignN), t);
  if (uint8_t* b = At<uint8_t*>(pass, kPBlendBlk)) {
    x.AddBlock(b, t);
    x.Add(pass + kPBlendObj, reinterpret_cast<uintptr_t>(At<void*>(b, kSbObj)), 8);
  }
  if (uint8_t* b = At<uint8_t*>(pass, kPDepthBlk)) {
    x.AddBlock(b, t);
    x.Add(pass + kPDepthObj, reinterpret_cast<uintptr_t>(At<void*>(b, kSbObj)), 8);
  }
  if (uint8_t* b = At<uint8_t*>(pass, kPRasterBlk)) x.AddBlock(b, t);
  for (int k = 0; k < kStages; ++k) {
    uint8_t* blk = At<uint8_t*>(pass, kStageOff[k]);
    if (!blk) continue;
    for (uint32_t d = 0; d < At<uint32_t>(blk, kBCbN); ++d) {
      uint8_t* dep = At<uint8_t*>(blk, kBCb) + d * kDepSize;
      for (uint32_t i = 0; i < At<uint32_t>(dep, kDCount); ++i) x.AddCb(At<uint8_t**>(dep, kDFx)[i]);
    }
    for (uint32_t d = 0; d < At<uint32_t>(blk, kBSampN); ++d) {
      uint8_t* dep = At<uint8_t*>(blk, kBSamp) + d * kDepSize;
      for (uint32_t i = 0; i < At<uint32_t>(dep, kDCount); ++i) x.AddBlock(At<uint8_t**>(dep, kDFx)[i], t);
    }
    for (uint32_t i = 0; i < At<uint32_t>(blk, kBTbN); ++i) x.AddCb(At<uint8_t**>(blk, kBTb)[i]);
    const uint32_t nSrv = At<uint32_t>(blk, kBSrvN);
    if (!nSrv) continue;
    const int st = kStageIdx[k];
    uint32_t c = 0;
    void** cache = reinterpret_cast<void**>(r + kRSrvCache + st * kRSrvStride);
    for (uint32_t d = 0; d < nSrv; ++d) {
      uint8_t* dep = At<uint8_t*>(blk, kBSrv) + d * kDepSize;
      for (uint32_t i = 0; i < At<uint32_t>(dep, kDCount); ++i, ++c) {
        void* v = *At<void***>(dep, kDFx)[i];
        x.Add(&At<void**>(dep, kDBound)[i], reinterpret_cast<uintptr_t>(v), 8);
        x.Add(&cache[c], reinterpret_cast<uintptr_t>(v), 8);
      }
    }
    const uint32_t old = At<uint32_t>(r, kRSrvCount + st * 4);
    for (uint32_t i = c; i < old && i < kMaxSlots; ++i) x.Add(&cache[i], 0, 8);  // stock's zero fill (only when it shrank)
    x.Add(r + kRSrvCount + st * 4, c, 4);
  }
  x.Add(r + kRTess, p.tess ? 1 : 0, 1);
}

// Index of the first expectation memory does not meet, or -1.
inline int CheckMem(const ExpList& x) {
  for (int i = 0; i < x.n; ++i)
    if (ReadSized(x.e[i].at, x.e[i].size) != x.e[i].want) return i;
  return -1;
}

// Context state of the stages and OM / RS state an Apply binds.
constexpr UINT kCbSlots = D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT;  // 14
constexpr UINT kSampSlots = D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT;           // 16
void* const kAny = reinterpret_cast<void*>(1);  // not compared
struct DevState {
  bool om = false, rs = false;
  void* blend;
  float factor[4];
  UINT mask;
  void* depth;
  UINT ref;
  void* raster;
  uint8_t stages = 0;  // bit k: stage k (apply order) recorded
  void* shader[kStages];
  void* cb[kStages][kCbSlots];
  void* samp[kStages][kSampSlots];
  uint32_t nSrv[kStages];
  void* srv[kStages][kMaxSlots];
};

// What the lean path leaves bound: its arguments; SRVs of fast stages as before (`before`, read with Read).
inline void PredictDev(const uint8_t* pass, uint32_t rs, const Plan& p, const DevState& before, DevState& d) {
  d.om = d.rs = false;
  d.stages = 0;
  if (const uint8_t* b = At<uint8_t*>(pass, kPBlendBlk)) {
    d.om = true;
    d.blend = At<void*>(b, kSbObj);
    memcpy(d.factor, pass + kPFactor, sizeof(d.factor));
    d.mask = At<uint32_t>(pass, kPMask);
  } else {
    d.blend = kAny;
  }
  if (const uint8_t* b = At<uint8_t*>(pass, kPDepthBlk)) {
    d.om = true;
    d.depth = At<void*>(b, kSbObj);
    d.ref = At<uint32_t>(pass, kPRef) & 0xff;  // d3d11 keeps the low 8 bits (split_filter.h)
  } else {
    d.depth = kAny;
  }
  if (const uint8_t* b = At<uint8_t*>(pass, kPRasterBlk)) {
    d.rs = true;
    d.raster = At<void*>(b, static_cast<uint64_t>(rs) * 8 + kSbObj);
  }
  for (int k = 0; k < kStages; ++k) {
    const uint8_t* blk = At<uint8_t*>(pass, kStageOff[k]);
    if (!blk) continue;
    d.stages |= static_cast<uint8_t>(1u << k);
    d.shader[k] = At<void*>(blk, kBShader);
    for (auto& v : d.cb[k]) v = kAny;
    for (auto& v : d.samp[k]) v = kAny;
    for (uint32_t dd = 0; dd < At<uint32_t>(blk, kBCbN); ++dd) {
      const uint8_t* dep = At<uint8_t*>(blk, kBCb) + dd * kDepSize;
      const uint32_t s0 = At<uint32_t>(dep, kDStart), n = At<uint32_t>(dep, kDCount);
      for (uint32_t i = 0; i < n && s0 + i < kCbSlots; ++i) d.cb[k][s0 + i] = At<void**>(dep, kDBound)[i];
    }
    for (uint32_t dd = 0; dd < At<uint32_t>(blk, kBSampN); ++dd) {
      const uint8_t* dep = At<uint8_t*>(blk, kBSamp) + dd * kDepSize;
      const uint32_t s0 = At<uint32_t>(dep, kDStart), n = At<uint32_t>(dep, kDCount);
      for (uint32_t i = 0; i < n && s0 + i < kSampSlots; ++i) d.samp[k][s0 + i] = At<void**>(dep, kDBound)[i];
    }
    d.nSrv[k] = 0;
    if (At<uint32_t>(blk, kBSrvN) && ((p.srvFast >> k) & 1) && ((before.stages >> k) & 1)) {
      d.nSrv[k] = before.nSrv[k];
      memcpy(d.srv[k], before.srv[k], before.nSrv[k] * sizeof(void*));
    }
  }
}

// First difference as text (empty when equal). `want` from PredictDev, `got` read after stock.
inline bool CompareDev(const DevState& want, const DevState& got, char* why, size_t cap) {
  auto diff = [&](const char* what, int k, int slot, const void* a, const void* b) {
    snprintf(why, cap, "%s%s%s slot %d: lean %p, stock %p", k >= 0 ? kStageNames[k] : "", k >= 0 ? " " : "", what, slot,
             a, b);
    return false;
  };
  if (want.blend != kAny && (want.blend != got.blend || memcmp(want.factor, got.factor, sizeof(want.factor)) != 0 ||
                             want.mask != got.mask))
    return diff("blend", -1, 0, want.blend, got.blend);
  if (want.depth != kAny && (want.depth != got.depth || want.ref != got.ref))
    return diff("depth", -1, 0, want.depth, got.depth);
  if (want.rs && want.raster != got.raster) return diff("raster", -1, 0, want.raster, got.raster);
  for (int k = 0; k < kStages; ++k) {
    if (!((want.stages >> k) & 1)) continue;
    if (want.shader[k] != got.shader[k]) return diff("shader", k, 0, want.shader[k], got.shader[k]);
    for (UINT i = 0; i < kCbSlots; ++i)
      if (want.cb[k][i] != kAny && want.cb[k][i] != got.cb[k][i]) return diff("CB", k, i, want.cb[k][i], got.cb[k][i]);
    for (UINT i = 0; i < kSampSlots; ++i)
      if (want.samp[k][i] != kAny && want.samp[k][i] != got.samp[k][i])
        return diff("sampler", k, i, want.samp[k][i], got.samp[k][i]);
    for (uint32_t i = 0; i < want.nSrv[k] && i < got.nSrv[k]; ++i)
      if (want.srv[k][i] != got.srv[k][i]) return diff("SRV", k, i, want.srv[k][i], got.srv[k][i]);
  }
  why[0] = 0;
  return true;
}

// Reads the context state for the stages in `stageMask` (SRVs: `nSrv[k]` slots
// from 0). Every returned reference is released.
template <class T>
inline void ReleaseAll(T* const* v, UINT n) {
  for (UINT i = 0; i < n; ++i)
    if (v[i]) v[i]->Release();
}
inline void ReadDev(ID3D11DeviceContext* c, uint8_t stageMask, const uint32_t* nSrv, bool om, DevState& d) {
  d.stages = stageMask;
  d.om = om;
  d.rs = true;
  if (om) {
    ID3D11BlendState* b = nullptr;
    c->OMGetBlendState(&b, d.factor, &d.mask);
    d.blend = b;
    if (b) b->Release();
    ID3D11DepthStencilState* s = nullptr;
    c->OMGetDepthStencilState(&s, &d.ref);
    d.depth = s;
    if (s) s->Release();
  }
  ID3D11RasterizerState* r = nullptr;
  c->RSGetState(&r);
  d.raster = r;
  if (r) r->Release();
  for (int k = 0; k < kStages; ++k) {
    if (!((stageMask >> k) & 1)) continue;
    UINT nci = 0;
    ID3D11Buffer* cb[kCbSlots] = {};
    ID3D11SamplerState* sp[kSampSlots] = {};
    ID3D11ShaderResourceView* sv[kMaxSlots] = {};
    const UINT ns = nSrv[k] < kMaxSlots ? nSrv[k] : kMaxSlots;
    IUnknown* sh = nullptr;
    switch (kStageIdx[k]) {
      case 0: {
        ID3D11VertexShader* s = nullptr;
        c->VSGetShader(&s, nullptr, &nci);
        sh = s;
        c->VSGetConstantBuffers(0, kCbSlots, cb);
        c->VSGetSamplers(0, kSampSlots, sp);
        if (ns) c->VSGetShaderResources(0, ns, sv);
        break;
      }
      case 4: {
        ID3D11PixelShader* s = nullptr;
        c->PSGetShader(&s, nullptr, &nci);
        sh = s;
        c->PSGetConstantBuffers(0, kCbSlots, cb);
        c->PSGetSamplers(0, kSampSlots, sp);
        if (ns) c->PSGetShaderResources(0, ns, sv);
        break;
      }
      case 1: {
        ID3D11GeometryShader* s = nullptr;
        c->GSGetShader(&s, nullptr, &nci);
        sh = s;
        c->GSGetConstantBuffers(0, kCbSlots, cb);
        c->GSGetSamplers(0, kSampSlots, sp);
        if (ns) c->GSGetShaderResources(0, ns, sv);
        break;
      }
      case 2: {
        ID3D11HullShader* s = nullptr;
        c->HSGetShader(&s, nullptr, &nci);
        sh = s;
        c->HSGetConstantBuffers(0, kCbSlots, cb);
        c->HSGetSamplers(0, kSampSlots, sp);
        if (ns) c->HSGetShaderResources(0, ns, sv);
        break;
      }
      default: {
        ID3D11DomainShader* s = nullptr;
        c->DSGetShader(&s, nullptr, &nci);
        sh = s;
        c->DSGetConstantBuffers(0, kCbSlots, cb);
        c->DSGetSamplers(0, kSampSlots, sp);
        if (ns) c->DSGetShaderResources(0, ns, sv);
        break;
      }
    }
    d.shader[k] = sh;
    if (sh) sh->Release();
    for (UINT i = 0; i < kCbSlots; ++i) d.cb[k][i] = cb[i];
    for (UINT i = 0; i < kSampSlots; ++i) d.samp[k][i] = sp[i];
    for (UINT i = 0; i < ns; ++i) d.srv[k][i] = sv[i];
    d.nSrv[k] = ns;
    ReleaseAll(cb, kCbSlots);
    ReleaseAll(sp, kSampSlots);
    ReleaseAll(sv, ns);
  }
}

// ---------------------------------------------------------------------------
// Hook and counters
// ---------------------------------------------------------------------------

using ApplyFn = uint64_t(__fastcall*)(uint8_t* pass, uint64_t flags, void* ctx, uint64_t rsIndex);
using RendererFn = uint8_t*(__fastcall*)();

struct Counters {  // render thread writes, the worker reads deltas (monotonic, no reset)
  uint64_t calls, callsDi, same, sameDi, ident, identDi, elig, eligDi, eligUpload;
  uint64_t why[kWhyCount];  // identical repeats the lean path refused, by reason
  uint64_t skipped, refusedSite, faults, other;
  uint64_t timed, cycAll, cycIdent, cycElig, cycEligDi, cycCheck, timedLean, cycLean;
  uint64_t verified, verifySkipped, mismatches;
};
Counters g_c{};
Counters g_last{};

ApplyFn g_orig = nullptr;
RendererFn g_renderer = nullptr;
void** g_slot = nullptr;
uint8_t* g_dx = nullptr;
const void* g_diRet = nullptr;
Sites g_sites{};
std::atomic<int> g_mode{0};       // 0 off, 1 count, 2 lean on identical repeats, 3 lean on every eligible Apply
std::atomic<bool> g_verify{false};
std::atomic<uint32_t> g_verifyEvery{64};
std::atomic<bool> g_timing{true};
std::atomic<bool> g_disabled{false};  // latched off (verify mismatch)
std::atomic<int> g_state{0};          // 0 not installed, 1 ready, -1 failed
std::atomic<bool> g_hooked{false};
std::atomic<bool> g_bench{false};     // bench mode 36 running: config reloads leave the mode alone
std::atomic<DWORD> g_tid{0};          // the render thread: first thread seen on the renderer's context

struct Prev {
  const uint8_t* pass;
  uint32_t rs;
  uint64_t sig;
  bool valid;
};
Prev g_prev{};
uint64_t g_verifyTick = 0;
ExpList g_exp;           // render thread only
DevState g_devBefore, g_devWant, g_devGot;
std::mutex g_sampleMutex;
std::vector<std::string> g_samples;

bool CheckGuarded(const uint8_t* pass, uint32_t rs, const uint8_t* r, Plan& p) {
  __try {
    Check(pass, rs, r, p);
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}
bool PredictGuarded(uint8_t* pass, uint32_t rs, uint8_t* r, const Plan& p, ExpList& x) {
  __try {
    PredictMem(pass, rs, r, p, x);
    PredictDev(pass, rs, p, g_devBefore, g_devWant);
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

void Sample(const char* line) {
  std::lock_guard<std::mutex> lock(g_sampleMutex);
  if (g_samples.size() < 8) g_samples.push_back(line);
}

// Stock Apply in place of a lean decision, compared against the prediction.
__declspec(noinline) uint64_t Verify(uint8_t* pass, uint64_t flags, void* ctx, uint64_t rsIndex, uint8_t* r,
                                     const Plan& p) {
  const uint32_t rs = static_cast<uint32_t>(rsIndex);
  auto* c = static_cast<ID3D11DeviceContext*>(ctx);
  uint8_t mask = 0;
  uint32_t nSrv[kStages] = {};
  bool om = false;
  {
    const uint8_t* b = At<uint8_t*>(pass, kPBlendBlk);
    om = b || At<uint8_t*>(pass, kPDepthBlk);
    for (int k = 0; k < kStages; ++k) {
      const uint8_t* blk = At<uint8_t*>(pass, kStageOff[k]);
      if (!blk) continue;
      mask |= static_cast<uint8_t>(1u << k);
      if ((p.srvFast >> k) & 1) nSrv[k] = At<uint32_t>(At<uint8_t*>(blk, kBSrv), kDCount);
    }
  }
  ReadDev(c, mask, nSrv, om, g_devBefore);
  if (!PredictGuarded(pass, rs, r, p, g_exp) || g_exp.overflow) {
    g_c.verifySkipped++;
    return g_orig(pass, flags, ctx, rsIndex);
  }
  const uint64_t ret = g_orig(pass, flags, ctx, rsIndex);
  for (int k = 0; k < kStages; ++k) nSrv[k] = g_devWant.nSrv[k];
  ReadDev(c, mask, nSrv, om, g_devGot);
  g_c.verified++;
  char why[200];
  const int bad = CheckMem(g_exp);
  bool ok = bad < 0;
  if (!ok)
    snprintf(why, sizeof(why), "memory %+lld from the pass: lean %llx, stock %llx (%d bytes)",
             static_cast<long long>(static_cast<uint8_t*>(g_exp.e[bad].at) - pass),
             static_cast<unsigned long long>(g_exp.e[bad].want),
             static_cast<unsigned long long>(ReadSized(g_exp.e[bad].at, g_exp.e[bad].size)), g_exp.e[bad].size);
  else
    ok = CompareDev(g_devWant, g_devGot, why, sizeof(why));
  if (!ok) {
    g_c.mismatches++;
    g_disabled = true;
    char line[300];
    snprintf(line, sizeof(line), "fx repeat: verify MISMATCH pass %p rs %u: %s; lean Apply latched off", pass, rs, why);
    Sample(line);
  }
  return ret;
}

uint64_t __fastcall Hook(uint8_t* pass, uint64_t flags, void* ctx, uint64_t rsIndex) {
  const int mode = g_mode.load(std::memory_order_relaxed);
  if (mode == 0) return g_orig(pass, flags, ctx, rsIndex);
  uint8_t* r = g_renderer();
  const DWORD tid = GetCurrentThreadId();
  DWORD rt = g_tid.load(std::memory_order_relaxed);
  if (!r || ctx != At<void*>(r, kRCtx)) {
    if (tid == rt) g_prev.valid = false;  // a deferred context on the render thread
    if (tid == rt) g_c.other++;
    return g_orig(pass, flags, ctx, rsIndex);
  }
  if (tid != rt) {
    if (rt || !g_tid.compare_exchange_strong(rt, tid)) return g_orig(pass, flags, ctx, rsIndex);
  }
  Counters& c = g_c;
  c.calls++;
  const bool di = _ReturnAddress() == g_diRet;
  if (di) c.callsDi++;
  const bool timed = g_timing.load(std::memory_order_relaxed) && (c.calls & 7) == 0;
  const uint32_t rs = static_cast<uint32_t>(rsIndex);
  const uint64_t t0 = timed ? __rdtsc() : 0;
  Plan p;
  if (!CheckGuarded(pass, rs, r, p)) {
    c.faults++;
    g_prev.valid = false;
    return g_orig(pass, flags, ctx, rsIndex);
  }
  const bool same = g_prev.valid && g_prev.pass == pass && g_prev.rs == rs;
  const bool ident = same && g_prev.sig == p.sig;
  const bool elig = p.why == kOk;
  g_prev = {pass, rs, p.sig, true};
  if (same) {
    c.same++;
    if (di) c.sameDi++;
  }
  if (ident) {
    c.ident++;
    if (di) c.identDi++;
    if (elig) {
      c.elig++;
      if (di) c.eligDi++;
      if (p.uploads) c.eligUpload++;
    } else {
      c.why[p.why]++;
    }
  }
  if (timed) {
    c.timed++;
    c.cycCheck += __rdtsc() - t0;
  }
  if (elig && !g_disabled.load(std::memory_order_relaxed) && ((mode == 2 && ident) || mode == 3)) {
    if (g_verify.load(std::memory_order_relaxed) && ++g_verifyTick % g_verifyEvery.load(std::memory_order_relaxed) == 0)
      return Verify(pass, flags, ctx, rsIndex, r, p);
    const uint64_t t1 = timed ? __rdtsc() : 0;
    if (Run(pass, ctx, rs, r, p, g_sites)) {
      c.skipped++;
      if (timed) {
        c.timedLean++;
        c.cycLean += __rdtsc() - t1;
      }
      return 0;  // 0x65850 returns 0
    }
    c.refusedSite++;
  }
  const uint64_t t1 = timed ? __rdtsc() : 0;
  const uint64_t ret = g_orig(pass, flags, ctx, rsIndex);
  if (timed) {
    const uint64_t dt = __rdtsc() - t1;
    c.cycAll += dt;
    if (ident) c.cycIdent += dt;
    if (ident && elig) {
      c.cycElig += dt;
      if (di) c.cycEligDi += dt;
    }
  }
  return ret;
}

// ---------------------------------------------------------------------------
// Install / attach
// ---------------------------------------------------------------------------

// FNV-1a 64 of [begin, end) with the kMasks bytes counted as zero.
bool MaskedCodeIs(const uint8_t* base, const CodeRange& cr) {
  uint64_t h = 0xcbf29ce484222325ull;
  uint8_t buf[256];
  for (uint32_t at = cr.begin; at < cr.end;) {
    const uint32_t n = cr.end - at < sizeof(buf) ? cr.end - at : static_cast<uint32_t>(sizeof(buf));
    if (!allocslab::ReadBytes(base + at, buf, n)) return false;
    for (uint32_t i = 0; i < n; ++i) {
      uint8_t v = buf[i];
      for (const Mask& m : kMasks)
        if (at + i >= m.at && at + i < m.at + m.n) v = 0;
      h ^= v;
      h *= 0x100000001b3ull;
    }
    at += n;
  }
  return h == cr.hash;
}

bool Install() {
  const int st = g_state.load();
  if (st != 0) return st > 0;
  auto* dx = reinterpret_cast<uint8_t*>(GetModuleHandleW(L"dx11backend.dll"));
  if (!dx) return false;  // retried
  auto** vtbl = reinterpret_cast<void**>(dx + kPassVtbl);
  const char* why = nullptr;
  if (!allocslab::RttiIs(dx, vtbl, ".?AUSPassBlock@D3DX11Effects@@") ||
      reinterpret_cast<uint8_t*>(SlotOriginal(&vtbl[kSlot])) != dx + kApply)
    why = "fx repeat: SPassBlock vtable does not match this dx11backend.dll build; off";
  for (const CodeRange& cr : kCode)
    if (!why && !MaskedCodeIs(dx, cr)) why = "fx repeat: dx11backend code differs from the analysed build; off";
  if (why) {
    Log("%s", why);
    g_state = -1;
    return false;
  }
  g_dx = dx;
  g_slot = &vtbl[kSlot];
  g_orig = reinterpret_cast<ApplyFn>(dx + kApply);
  g_renderer = reinterpret_cast<RendererFn>(dx + kRendererGet);
  g_diRet = dx + kDiRet;
  g_sites = {dx + kSiteBlend, dx + kSiteDepth, dx + kSiteRaster, dx + kSiteSrv};
  g_state = 1;
  return true;
}

bool Hooked() { return g_hooked.load(); }

// Mode 0 = hook out (stock); 1-3 as above. Returns false when not installed.
bool SetMode(int mode) {
  if (mode <= 0) {
    g_mode = 0;
    if (g_hooked.exchange(false)) UnhookSlot(g_slot, reinterpret_cast<void*>(g_orig));
    return true;
  }
  if (!Install()) return false;
  g_prev.valid = false;
  g_mode = mode;
  if (!g_hooked.load()) {
    if (*g_slot != reinterpret_cast<void*>(g_orig)) {
      Log("fx repeat: the SPassBlock Apply slot is hooked by something else; off");
      g_mode = 0;
      return false;
    }
    if (!HookSlot(g_slot, reinterpret_cast<void*>(&Hook), nullptr)) {
      Log("fx repeat: could not patch the SPassBlock Apply slot; off");
      g_mode = 0;
      return false;
    }
    g_hooked = true;
  }
  return true;
}

void Shutdown() { SetMode(0); }

// Once per worker pass: the line every `sec` seconds while hooked.
void Tick(uint64_t nowMs, uint64_t frames, double tscHz, int sec) {
  static uint64_t lastMs = 0, lastFrames = 0;
  static bool was = false;
  if (!g_hooked.load() || tscHz <= 0) {
    was = false;
    return;
  }
  if (!was) {
    was = true;
    lastMs = nowMs;
    lastFrames = frames;
    g_last = g_c;
    return;
  }
  if (nowMs - lastMs < static_cast<uint64_t>(sec) * 1000) return;
  const Counters c = g_c;
  Counters d;
  const uint64_t* a = reinterpret_cast<const uint64_t*>(&c);
  const uint64_t* b = reinterpret_cast<const uint64_t*>(&g_last);
  uint64_t* o = reinterpret_cast<uint64_t*>(&d);
  for (size_t i = 0; i < sizeof(Counters) / 8; ++i) o[i] = a[i] - b[i];
  g_last = c;
  const double f = static_cast<double>(frames - lastFrames);
  const double secs = (nowMs - lastMs) / 1000.0;
  lastMs = nowMs;
  lastFrames = frames;
  if (f <= 0) return;
  // Sampled every 8th Apply: cycles x 8 per frame -> us.
  const double us = 8.0 * 1e6 / tscHz / f;
  const double nsCheck = d.timed ? d.cycCheck * 1e9 / tscHz / d.timed : 0.0;
  const double nsLean = d.timedLean ? d.cycLean * 1e9 / tscHz / d.timedLean : 0.0;
  Log("fx repeat: mode %d%s, %.1f s, %.0f frames; Applies/frame %.0f (drawIndirect %.0f); same pass again %.0f "
      "(drawIndirect %.0f); identical %.0f (drawIndirect %.0f); identical and lean-eligible %.0f (drawIndirect %.0f, "
      "with CB uploads %.0f); stock us/frame: all %.0f, identical %.0f, eligible %.0f (drawIndirect %.0f); check %.0f "
      "ns/Apply; lean %.0f/frame at %.0f ns, %.0f us/frame; site refusals %llu; identical not eligible/frame: rt %.0f "
      "compute %.0f uav %.0f assign %.0f state-assign %.0f tess %.0f overflow %.0f; verify %llu checks, %llu skipped, "
      "%llu mismatches; faults %llu; other context %.0f/frame",
      g_mode.load(), g_disabled.load() ? " (LATCHED OFF)" : "", secs, f, d.calls / f, d.callsDi / f, d.same / f,
      d.sameDi / f, d.ident / f, d.identDi / f, d.elig / f, d.eligDi / f, d.eligUpload / f, d.cycAll * us,
      d.cycIdent * us, d.cycElig * us, d.cycEligDi * us, nsCheck, d.skipped / f, nsLean, d.skipped / f * nsLean / 1000.0,
      static_cast<unsigned long long>(d.refusedSite), d.why[kRt] / f, d.why[kCompute] / f, d.why[kUav] / f,
      d.why[kAssign] / f, d.why[kStateAssign] / f, d.why[kTess] / f, d.why[kOverflow] / f,
      static_cast<unsigned long long>(d.verified), static_cast<unsigned long long>(d.verifySkipped),
      static_cast<unsigned long long>(d.mismatches), static_cast<unsigned long long>(d.faults), d.other / f);
  std::lock_guard<std::mutex> lock(g_sampleMutex);
  for (auto& l : g_samples) Log("%s", l.c_str());
  g_samples.clear();
}

}  // namespace fxskip
