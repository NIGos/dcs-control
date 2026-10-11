// Exact-redundancy census of DCS's GPU work (R26 11). [Suite] GpuDedup = frames
// captured (0 = off, the default). Measure only: nothing DCS issues is skipped,
// reordered or changed; with the key at 0 none of this is installed.
//
// Runs inside a gpu_pass_timing.h session (full mode, GpuPassTiming's hooks on
// the copy/update/clear/mips/dispatch slots call in here while g_on), plus our
// own hooks on Map, Unmap, the six draw slots, CopyStructureCount, the 11.1
// update/copy/clear/discard slots and the two UAV setters (initial counts), all
// re-armed the way gpu_pass_timing.h re-arms its own (d3d11 rewrites the table).
//
// Content ids. Every resource DCS's immediate context reads gets a 64-bit
// content id (cid), a value number of its bytes:
//  - first sight: hash of the pointer (immutable / unknown origin);
//  - Map(write) + Unmap of a buffer up to HashCap bytes, UpdateSubresource of a
//    whole buffer or single-subresource texture: hash of the bytes (two uploads
//    of equal bytes get equal ids);
//  - CopyResource / full copies / resolves: the source's id;
//  - full clears of single-subresource views: hash of the view and the value;
//  - partial writes: hash of the old id and the write;
//  - Dispatch / Draw: every bound output (UAV, RTV, DSV when depth or stencil
//    writes are on) becomes hash(signature, slot); UAV hidden counters get
//    their own id (initial counts from the setters, CopyStructureCount reads).
// The signature of a dispatch or draw is the hash of: shaders, constant
// buffers (cid + first/num constants), SRVs (view desc + cid), samplers,
// pipeline state (IA, RS, OM state objects, viewports, scissors, blend factor,
// stencil ref), arguments (or the indirect buffer's cid + offset), output view
// identities, and the outputs' cids before the call. sigIn = the same without
// the outputs' prior cids. Two calls with equal signatures compute equal
// results [I: deterministic shaders; append/atomic order aside]. Writes we do
// not see (ExecuteCommandList of our recorders' deferred lists, other threads,
// other contexts) keep stale ids, so a match found here is a candidate that a
// readback must confirm; a mismatch is final.
//
// Capture: after Warm frames of id tracking, Frames consecutive frames are
// recorded on the render thread: every dispatch, draw and write op with its
// pass (innermost and top-level), issuer (first two return addresses outside
// DcsQvCull), the signature components, a GPU timestamp pair around each
// dispatch/draw/copy/clear (end - begin = incremental GPU time), and the
// arguments of indirect calls (read back). Written to the dev folder as
// gdd_events.csv, gdd_res.csv, gdd_pass.csv, gdd_issuer.csv, gdd_shader.csv;
// a summary goes to the log. The analysis is offline.
//
// Included once from main.cpp after gpu_pass_timing.h.
#pragma once

namespace gdd {

using Ctx = ID3D11DeviceContext;

std::atomic<bool> g_on{false};
// 0 idle, 1 warm (ids only), 2 capture, 3 drain (reading back), 4 done
std::atomic<int> g_phase{0};
int g_warmFrames = 30;
int g_capFrames = 6;
size_t g_hashCap = 16384;
HMODULE g_self = nullptr;
ID3D11DeviceContext1* g_ctx1 = nullptr;

constexpr int kMaxCap = 12;
constexpr int kMaxStamps = 16384;  // per frame (8192 stamped events)
constexpr int kMaxArgs = 8192;    // indirect argument records per frame

enum Kind : uint8_t {
  kDispatch, kDispatchInd, kDraw, kDrawIdx, kDrawInst, kDrawIdxInst, kDrawIdxInstInd, kDrawInstInd,
  kCopy, kCopySub, kResolve, kUpdate, kClearRTV, kClearDSV, kClearUAV, kMips, kMapW, kCopyCount, kClearView,
  kDiscard, kKindCount
};
const char* const kKindName[kKindCount] = {
    "dispatch", "dispatchInd", "draw",   "drawIdx", "drawInst", "drawIdxInst", "drawIdxInstInd",
    "drawInstInd", "copy",     "copySub", "resolve", "update",   "clrRTV",      "clrDSV",
    "clrUAV",   "mips",        "mapW",    "copyCount", "clearView", "discard"};
inline bool IsWork(int k) { return k <= kDrawInstInd; }

// ---------------------------------------------------------------------------
// Hashing and content ids (render thread only)
// ---------------------------------------------------------------------------

inline uint64_t Mix(uint64_t h, uint64_t v) {
  h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
  h *= 0xff51afd7ed558ccdull;
  return h ^ (h >> 31);
}
uint64_t HashBytesRaw(const void* p, size_t n) {
  const uint8_t* b = static_cast<const uint8_t*>(p);
  uint64_t h = 0xcbf29ce484222325ull ^ (n * 0x9e3779b97f4a7c15ull);
  size_t i = 0;
  for (; i + 8 <= n; i += 8) {
    uint64_t v;
    memcpy(&v, b + i, 8);
    h = (h ^ v) * 0x100000001b3ull;
    h ^= h >> 29;
  }
  for (; i < n; ++i) h = (h ^ b[i]) * 0x100000001b3ull;
  return h;
}
// 0 when the memory faulted.
uint64_t HashBytes(const void* p, size_t n) {
  __try {
    return HashBytesRaw(p, n) | 1;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return 0;
  }
}

struct ResInfo {
  uint32_t id = 0;
  uint32_t dim = 0;  // D3D11_RESOURCE_DIMENSION
  uint32_t bytes = 0, w = 0, h = 0, d = 0, mips = 1, arr = 1, fmt = 0, bind = 0, misc = 0, usage = 0, stride = 0,
           samples = 1;
  uint64_t cid = 0;
  uint64_t writes = 0;  // writes seen (all phases)
};
std::unordered_map<void*, ResInfo> g_res;
std::vector<void*> g_resOrder;
std::unordered_map<void*, uint64_t> g_ctr;  // UAV view -> hidden counter id
// Constant-buffer bytes seen in the first capture frame, by content id (offline diff of what differs).
std::unordered_map<uint64_t, std::vector<uint8_t>> g_cbStore;
size_t g_cbStoreBytes = 0;
uint64_t g_fresh = 0;

ResInfo& Res(ID3D11Resource* r) {
  auto it = g_res.find(r);
  if (it != g_res.end()) return it->second;
  ResInfo ri;
  ri.id = static_cast<uint32_t>(g_resOrder.size() + 1);
  D3D11_RESOURCE_DIMENSION dim = D3D11_RESOURCE_DIMENSION_UNKNOWN;
  r->GetType(&dim);
  ri.dim = dim;
  if (dim == D3D11_RESOURCE_DIMENSION_BUFFER) {
    D3D11_BUFFER_DESC d{};
    static_cast<ID3D11Buffer*>(r)->GetDesc(&d);
    ri.bytes = d.ByteWidth, ri.bind = d.BindFlags, ri.misc = d.MiscFlags, ri.usage = d.Usage;
    ri.stride = d.StructureByteStride;
  } else if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D) {
    D3D11_TEXTURE2D_DESC d{};
    static_cast<ID3D11Texture2D*>(r)->GetDesc(&d);
    ri.w = d.Width, ri.h = d.Height, ri.mips = d.MipLevels, ri.arr = d.ArraySize, ri.fmt = d.Format;
    ri.bind = d.BindFlags, ri.misc = d.MiscFlags, ri.usage = d.Usage, ri.samples = d.SampleDesc.Count;
  } else if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE3D) {
    D3D11_TEXTURE3D_DESC d{};
    static_cast<ID3D11Texture3D*>(r)->GetDesc(&d);
    ri.w = d.Width, ri.h = d.Height, ri.d = d.Depth, ri.mips = d.MipLevels, ri.fmt = d.Format;
    ri.bind = d.BindFlags, ri.misc = d.MiscFlags, ri.usage = d.Usage;
  } else if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE1D) {
    D3D11_TEXTURE1D_DESC d{};
    static_cast<ID3D11Texture1D*>(r)->GetDesc(&d);
    ri.w = d.Width, ri.mips = d.MipLevels, ri.arr = d.ArraySize, ri.fmt = d.Format;
    ri.bind = d.BindFlags, ri.misc = d.MiscFlags, ri.usage = d.Usage;
  }
  ri.cid = Mix(0x5eed0001ull, reinterpret_cast<uint64_t>(r));
  g_resOrder.push_back(r);
  return g_res.emplace(r, ri).first->second;
}
inline bool Single(const ResInfo& ri) { return ri.dim == D3D11_RESOURCE_DIMENSION_BUFFER || (ri.mips == 1 && ri.arr == 1); }
inline uint64_t Fresh() { return Mix(0xf4e5ull, ++g_fresh); }
inline void SetCid(ID3D11Resource* r, uint64_t v) {
  ResInfo& ri = Res(r);
  ri.cid = v;
  ++ri.writes;
}

ID3D11Resource* ResOf(ID3D11View* v) {
  if (!v) return nullptr;
  ID3D11Resource* r = nullptr;
  v->GetResource(&r);
  if (r) r->Release();  // kept alive by the binding / the caller
  return r;
}

// ---------------------------------------------------------------------------
// Capture storage
// ---------------------------------------------------------------------------

struct Ev {
  uint8_t kind = 0, depth = 0;
  uint32_t seq = 0;
  void* pass = nullptr;
  void* top = nullptr;
  uint32_t issuer = 0;
  int32_t stamp = -1;  // begin stamp index; end = +1
  int32_t argIdx = -1;
  uint64_t sig = 0, sigIn = 0, hSh = 0, hCb = 0, hSrv = 0, hSamp = 0, hState = 0, hArgs = 0, hOutId = 0,
           hOutPrior = 0;
  uint32_t a[5] = {};
  std::string binds;
};
struct Frame {
  std::vector<Ev> ev;
  ID3D11Query* dj = nullptr;
  ID3D11Query* ts[kMaxStamps] = {};
  int nts = 0;
  ID3D11Buffer* argStage = nullptr;
  int nArgs = 0;
  bool used = false, ended = false, read = false, disjoint = false;
  uint64_t freq = 0;
  std::vector<uint64_t> ticks;
  std::vector<uint32_t> args;  // 5 per record
  uint64_t gptSerial = 0;
};
Frame* g_frames = nullptr;
int g_cur = -1;        // capturing frame index
int g_framesSeen = 0;  // frames opened since the phase started
uint32_t g_seq = 0;
bool g_queriesOk = false;

struct Counters {
  uint64_t evTotal = 0, stampsSkipped = 0, argsSkipped = 0, mapW = 0, mapHashed = 0, mapBig = 0, mapTex = 0,
           mapOther = 0, offThread = 0, rearms = 0, rehooks = 0, faults = 0, unmapUnknown = 0;
};
Counters g_c;

// Instance-buffer snapshots ([Suite] GpuDedupSnap): in the first capture frame,
// every structured VS buffer of 64-byte records (>= 1 MB) read by an indirect
// draw is copied to staging when its top-level pass ends, then written to
// gdd_snap_<pass>_R<id>.bin (the cascades' culled instance lists).
bool g_snapOn = false;
struct Snap {
  void* top;
  ID3D11Resource* res;
  uint32_t rid;
  ID3D11Buffer* stage;
  bool done;
};
std::vector<Snap> g_snaps;
std::wstring g_snapDir;
thread_local bool t_snapDraw = false;  // StageInputs: this draw may register snapshots
std::unordered_map<void*, bool> g_isCascade;
bool IsCascade(void* top) {
  if (!top) return false;
  auto it = g_isCascade.find(top);
  if (it != g_isCascade.end()) return it->second;
  const bool c = ptiming::RttiName(top).find("CascadeShadowPassData") != std::string::npos;
  g_isCascade.emplace(top, c);
  return c;
}

// passes: stack on the render thread
void* g_stack[64];
int g_sp = 0;

// issuers
struct Caller {
  void* a = nullptr;
  void* b = nullptr;
};
std::vector<Caller> g_callers;
uint32_t Issuer() {
  void* fr[12] = {};
  const USHORT n = RtlCaptureStackBackTrace(2, 12, fr, nullptr);
  Caller c;
  for (USHORT i = 0; i < n; ++i) {
    HMODULE m = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       static_cast<LPCWSTR>(fr[i]), &m);
    if (m == g_self) continue;
    if (!c.a)
      c.a = fr[i];
    else {
      c.b = fr[i];
      break;
    }
  }
  for (size_t i = 0; i < g_callers.size(); ++i)
    if (g_callers[i].a == c.a && g_callers[i].b == c.b) return static_cast<uint32_t>(i + 1);
  g_callers.push_back(c);
  return static_cast<uint32_t>(g_callers.size());
}

std::unordered_map<void*, uint32_t> g_shaderIds;
std::vector<std::pair<void*, char>> g_shaders;
uint32_t ShaderId(void* s, char stage) {
  if (!s) return 0;
  auto it = g_shaderIds.find(s);
  if (it != g_shaderIds.end()) return it->second;
  g_shaders.push_back({s, stage});
  const uint32_t id = static_cast<uint32_t>(g_shaders.size());
  g_shaderIds.emplace(s, id);
  return id;
}

inline bool Capturing() { return g_phase.load(std::memory_order_relaxed) == 2 && g_cur >= 0; }
inline bool Tracking() {
  const int p = g_phase.load(std::memory_order_relaxed);
  return p == 1 || p == 2;
}
inline bool Mine(Ctx* c) {
  if (c != gpt::g_ctx) return false;
  if (GetCurrentThreadId() != gpt::g_rt.load(std::memory_order_relaxed)) {
    ++g_c.offThread;
    return false;
  }
  return Tracking();
}

thread_local bool t_inside = false;  // our own context calls

bool SafeCopy(void* d, const void* p, size_t n) {
  __try {
    memcpy(d, p, n);
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}
void StoreCb(const ResInfo& ri, uint64_t cid, const void* p, size_t n) {
  if (!Capturing() || g_cur > 1 || !(ri.bind & D3D11_BIND_CONSTANT_BUFFER) || n > 65536 || !n ||
      g_cbStoreBytes > (256u << 20) || g_cbStore.count(cid))
    return;
  std::vector<uint8_t> v(n);
  if (!SafeCopy(v.data(), p, n)) return;
  g_cbStoreBytes += n;
  g_cbStore.emplace(cid, std::move(v));
}

// d3d11 swaps draw/copy/clear/dispatch entries on ExecuteCommandList and Flush
// (our recorders execute lists inside the cascade and G-buffer passes), which
// silently drops a hook until the next pass boundary. Map/Unmap/Update keep
// theirs and DCS calls them before nearly every draw: re-arm from there.
void QuickRepatch() {
  if (gpt::g_opsHooked.load(std::memory_order_relaxed)) gpt::RepatchOps();
}

void Stamp(Ev& e, bool begin) {
  Frame& f = g_frames[g_cur];
  if (begin) {
    if (f.nts + 2 > kMaxStamps || !g_queriesOk) {
      ++g_c.stampsSkipped;
      return;
    }
    e.stamp = f.nts;
    gpt::g_ctx->End(f.ts[f.nts]);
    f.nts += 2;
  } else if (e.stamp >= 0) {
    gpt::g_ctx->End(f.ts[e.stamp + 1]);
  }
}

Ev* NewEv(int kind) {
  Frame& f = g_frames[g_cur];
  f.ev.emplace_back();
  Ev& e = f.ev.back();
  e.kind = static_cast<uint8_t>(kind);
  e.seq = g_seq++;
  e.pass = g_sp > 0 && g_sp <= 64 ? g_stack[g_sp - 1] : nullptr;
  e.top = g_sp > 0 ? g_stack[0] : nullptr;
  e.depth = static_cast<uint8_t>(g_sp < 255 ? g_sp : 255);
  e.issuer = Issuer();
  ++g_c.evTotal;
  return &e;
}

// Indirect arguments: copied into the frame's staging buffer (read back later).
void CopyArgs(Ev& e, ID3D11Buffer* b, UINT off, UINT bytes) {
  Frame& f = g_frames[g_cur];
  if (!f.argStage || f.nArgs >= kMaxArgs || !b) {
    ++g_c.argsSkipped;
    return;
  }
  D3D11_BOX box{off, 0, 0, off + bytes, 1, 1};
  t_inside = true;
  gpt::g_ctx->CopySubresourceRegion(f.argStage, 0, static_cast<UINT>(f.nArgs * 20), 0, 0, b, 0, &box);
  t_inside = false;
  e.argIdx = f.nArgs++;
}

// ---------------------------------------------------------------------------
// Binding snapshot and signature
// ---------------------------------------------------------------------------

struct Out {
  ID3D11Resource* res;
  void* view;  // UAV views (hidden counter), else null
  uint32_t tag;
};

struct Sig {
  uint64_t sh = 0x51, cb = 0xcb, srv = 0x5e, samp = 0x5a, state = 0x57, args = 0xa6, outId = 0x01, outPrior = 0x0b;
  Out outs[24];
  int nOut = 0;
  std::string* binds = nullptr;  // capture only
  void Add(const char* fmt, ...) {
    if (!binds) return;
    char buf[160];
    va_list ap;
    va_start(ap, fmt);
    const int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n > 0) binds->append(buf, static_cast<size_t>(std::min<int>(n, sizeof(buf) - 1)));
  }
};

inline uint32_t Cid24(uint64_t v) { return static_cast<uint32_t>(v >> 40); }

template <class V, class D>
uint64_t ViewDescHash(V* v) {
  D d;
  memset(&d, 0, sizeof(d));
  v->GetDesc(&d);
  return HashBytesRaw(&d, sizeof(d));
}

void StageInputs(Sig& s, char st, ID3D11Buffer** cbs, UINT* first, UINT* num, ID3D11ShaderResourceView** srvs,
                 ID3D11SamplerState** samp) {
  for (int i = 0; i < D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT; ++i) {
    if (!cbs[i]) continue;
    ResInfo& ri = Res(cbs[i]);
    s.cb = Mix(Mix(Mix(Mix(s.cb, (static_cast<uint64_t>(st) << 8) | i), ri.cid), first[i]), num[i]);
    s.Add("c%c%d=R%u@%u+%u:%06x ", st, i, ri.id, first[i], num[i], Cid24(ri.cid));
    cbs[i]->Release();
  }
  for (int i = 0; i < D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT; ++i) {
    if (!srvs[i]) continue;
    ID3D11Resource* r = ResOf(srvs[i]);
    const uint64_t vd = ViewDescHash<ID3D11ShaderResourceView, D3D11_SHADER_RESOURCE_VIEW_DESC>(srvs[i]);
    if (r) {
      ResInfo& ri = Res(r);
      if (t_snapDraw && st == 'V' && ri.dim == D3D11_RESOURCE_DIMENSION_BUFFER && ri.stride == 64 &&
          ri.bytes >= (1u << 20)) {
        void* top = g_sp > 0 ? g_stack[0] : nullptr;
        bool have = !IsCascade(top);
        for (const Snap& sn : g_snaps) have |= sn.top == top && sn.res == r;
        if (!have && g_snaps.size() < 64) g_snaps.push_back({top, r, ri.id, nullptr, false});
      }
      s.srv = Mix(Mix(Mix(s.srv, (static_cast<uint64_t>(st) << 8) | i), vd), ri.cid);
      s.Add("t%c%d=R%u:%06x ", st, i, ri.id, Cid24(ri.cid));
    }
    srvs[i]->Release();
  }
  for (int i = 0; i < D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT; ++i) {
    if (!samp[i]) continue;
    s.samp = Mix(Mix(s.samp, (static_cast<uint64_t>(st) << 8) | i), reinterpret_cast<uint64_t>(samp[i]));
    samp[i]->Release();
  }
}

#define GDD_STAGE(L, P)                                                                                      \
  {                                                                                                          \
    ID3D11Buffer* cbs[14] = {};                                                                              \
    UINT first[14] = {}, num[14] = {};                                                                       \
    if (g_ctx1)                                                                                              \
      g_ctx1->P##GetConstantBuffers1(0, 14, cbs, first, num);                                                \
    else                                                                                                     \
      c->P##GetConstantBuffers(0, 14, cbs);                                                                  \
    ID3D11ShaderResourceView* srvs[D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT] = {};                       \
    c->P##GetShaderResources(0, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT, srvs);                        \
    ID3D11SamplerState* samp[16] = {};                                                                       \
    c->P##GetSamplers(0, 16, samp);                                                                          \
    StageInputs(s, L, cbs, first, num, srvs, samp);                                                          \
  }

void AddUav(Sig& s, char st, int slot, ID3D11UnorderedAccessView* u) {
  ID3D11Resource* r = ResOf(u);
  D3D11_UNORDERED_ACCESS_VIEW_DESC ud;
  memset(&ud, 0, sizeof(ud));
  u->GetDesc(&ud);
  const uint64_t vd = HashBytesRaw(&ud, sizeof(ud));
  // Only append/counter buffer views have a hidden counter.
  const bool counter = ud.ViewDimension == D3D11_UAV_DIMENSION_BUFFER &&
                       (ud.Buffer.Flags & (D3D11_BUFFER_UAV_FLAG_APPEND | D3D11_BUFFER_UAV_FLAG_COUNTER)) != 0;
  if (r) {
    ResInfo& ri = Res(r);
    auto ct = counter ? g_ctr.find(u) : g_ctr.end();
    const uint64_t ctr = ct != g_ctr.end() ? ct->second : 0;
    s.outId = Mix(Mix(Mix(s.outId, (static_cast<uint64_t>(st) << 8) | slot), vd), reinterpret_cast<uint64_t>(r));
    s.outPrior = Mix(Mix(s.outPrior, ri.cid), ctr);
    s.Add("u%c%d=R%u:%06x>", st, slot, ri.id, Cid24(ri.cid));
    if (s.nOut < 24) s.outs[s.nOut++] = {r, counter ? u : nullptr, static_cast<uint32_t>((st << 8) | slot)};
  }
}

void CaptureCompute(Ctx* c, Sig& s) {
  ID3D11ComputeShader* cs = nullptr;
  c->CSGetShader(&cs, nullptr, nullptr);
  s.sh = Mix(s.sh, reinterpret_cast<uint64_t>(cs));
  s.Add("CS=S%u ", ShaderId(cs, 'C'));
  if (cs) cs->Release();
  GDD_STAGE('C', CS)
  ID3D11UnorderedAccessView* u[D3D11_1_UAV_SLOT_COUNT] = {};
  c->CSGetUnorderedAccessViews(0, D3D11_1_UAV_SLOT_COUNT, u);
  for (int i = 0; i < D3D11_1_UAV_SLOT_COUNT; ++i)
    if (u[i]) {
      AddUav(s, 'C', i, u[i]);
      u[i]->Release();
    }
}

void CaptureGraphics(Ctx* c, Sig& s) {
  ID3D11VertexShader* vs = nullptr;
  ID3D11HullShader* hs = nullptr;
  ID3D11DomainShader* ds = nullptr;
  ID3D11GeometryShader* gs = nullptr;
  ID3D11PixelShader* ps = nullptr;
  c->VSGetShader(&vs, nullptr, nullptr);
  c->HSGetShader(&hs, nullptr, nullptr);
  c->DSGetShader(&ds, nullptr, nullptr);
  c->GSGetShader(&gs, nullptr, nullptr);
  c->PSGetShader(&ps, nullptr, nullptr);
  s.sh = Mix(Mix(Mix(Mix(Mix(s.sh, reinterpret_cast<uint64_t>(vs)), reinterpret_cast<uint64_t>(hs)),
                     reinterpret_cast<uint64_t>(ds)),
                 reinterpret_cast<uint64_t>(gs)),
             reinterpret_cast<uint64_t>(ps));
  s.Add("VS=S%u HS=S%u DS=S%u GS=S%u PS=S%u ", ShaderId(vs, 'V'), ShaderId(hs, 'H'), ShaderId(ds, 'D'),
        ShaderId(gs, 'G'), ShaderId(ps, 'P'));
  if (vs) GDD_STAGE('V', VS)
  if (hs) GDD_STAGE('H', HS)
  if (ds) GDD_STAGE('D', DS)
  if (gs) GDD_STAGE('G', GS)
  if (ps) GDD_STAGE('P', PS)
  if (vs) vs->Release();
  if (hs) hs->Release();
  if (ds) ds->Release();
  if (gs) gs->Release();
  if (ps) ps->Release();
  // IA
  ID3D11InputLayout* il = nullptr;
  c->IAGetInputLayout(&il);
  D3D11_PRIMITIVE_TOPOLOGY topo{};
  c->IAGetPrimitiveTopology(&topo);
  s.sh = Mix(Mix(s.sh, reinterpret_cast<uint64_t>(il)), topo);
  if (il) il->Release();
  ID3D11Buffer* vb[D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT] = {};
  UINT str[D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT] = {}, off[D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT] = {};
  c->IAGetVertexBuffers(0, D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT, vb, str, off);
  for (int i = 0; i < D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT; ++i) {
    if (!vb[i]) continue;
    ResInfo& ri = Res(vb[i]);
    s.srv = Mix(Mix(Mix(Mix(s.srv, 0x7600 | i), ri.cid), str[i]), off[i]);
    s.Add("vb%d=R%u+%u:%06x ", i, ri.id, off[i], Cid24(ri.cid));
    vb[i]->Release();
  }
  ID3D11Buffer* ib = nullptr;
  DXGI_FORMAT ifmt{};
  UINT ioff = 0;
  c->IAGetIndexBuffer(&ib, &ifmt, &ioff);
  if (ib) {
    ResInfo& ri = Res(ib);
    s.srv = Mix(Mix(Mix(Mix(s.srv, 0x1b), ri.cid), ifmt), ioff);
    s.Add("ib=R%u+%u:%06x ", ri.id, ioff, Cid24(ri.cid));
    ib->Release();
  }
  // RS
  ID3D11RasterizerState* rs = nullptr;
  c->RSGetState(&rs);
  s.state = Mix(s.state, reinterpret_cast<uint64_t>(rs));
  if (rs) rs->Release();
  D3D11_VIEWPORT vp[16];
  UINT nvp = 16;
  c->RSGetViewports(&nvp, vp);
  s.state = Mix(s.state, HashBytesRaw(vp, nvp * sizeof(D3D11_VIEWPORT)));
  if (nvp) s.Add("vp=%.0fx%.0f@%.0f,%.0f ", vp[0].Width, vp[0].Height, vp[0].TopLeftX, vp[0].TopLeftY);
  D3D11_RECT sc[16];
  UINT nsc = 16;
  c->RSGetScissorRects(&nsc, sc);
  s.state = Mix(s.state, HashBytesRaw(sc, nsc * sizeof(D3D11_RECT)));
  // OM
  ID3D11BlendState* bs = nullptr;
  FLOAT bf[4] = {};
  UINT mask = 0;
  c->OMGetBlendState(&bs, bf, &mask);
  s.state = Mix(Mix(Mix(s.state, reinterpret_cast<uint64_t>(bs)), HashBytesRaw(bf, sizeof(bf))), mask);
  if (bs) bs->Release();
  ID3D11DepthStencilState* dss = nullptr;
  UINT sref = 0;
  c->OMGetDepthStencilState(&dss, &sref);
  bool dsWrite = true;
  if (dss) {
    D3D11_DEPTH_STENCIL_DESC d{};
    dss->GetDesc(&d);
    dsWrite = (d.DepthEnable && d.DepthWriteMask == D3D11_DEPTH_WRITE_MASK_ALL) ||
              (d.StencilEnable && d.StencilWriteMask != 0);
    s.Add("ds=%d/%d/%d ", d.DepthEnable, d.DepthWriteMask, d.DepthFunc);
  }
  s.state = Mix(Mix(s.state, reinterpret_cast<uint64_t>(dss)), sref);
  if (dss) dss->Release();
  ID3D11RenderTargetView* rtv[8] = {};
  ID3D11DepthStencilView* dsv = nullptr;
  c->OMGetRenderTargets(8, rtv, &dsv);
  for (int i = 0; i < 8; ++i) {
    if (!rtv[i]) continue;
    ID3D11Resource* r = ResOf(rtv[i]);
    const uint64_t vd = ViewDescHash<ID3D11RenderTargetView, D3D11_RENDER_TARGET_VIEW_DESC>(rtv[i]);
    if (r) {
      ResInfo& ri = Res(r);
      s.outId = Mix(Mix(Mix(s.outId, 0x7200 | i), vd), reinterpret_cast<uint64_t>(r));
      s.outPrior = Mix(s.outPrior, ri.cid);
      s.Add("rt%d=R%u:%06x> ", i, ri.id, Cid24(ri.cid));
      if (s.nOut < 24) s.outs[s.nOut++] = {r, nullptr, static_cast<uint32_t>(0x7200 | i)};
    }
    rtv[i]->Release();
  }
  if (dsv) {
    ID3D11Resource* r = ResOf(dsv);
    const uint64_t vd = ViewDescHash<ID3D11DepthStencilView, D3D11_DEPTH_STENCIL_VIEW_DESC>(dsv);
    if (r) {
      ResInfo& ri = Res(r);
      s.outId = Mix(Mix(Mix(s.outId, 0xd5), vd), reinterpret_cast<uint64_t>(r));
      s.outPrior = Mix(s.outPrior, ri.cid);  // depth test reads it
      s.Add("dsv=R%u:%06x%s ", ri.id, Cid24(ri.cid), dsWrite ? ">" : "");
      if (dsWrite && s.nOut < 24) s.outs[s.nOut++] = {r, nullptr, 0xd5};
    }
    dsv->Release();
  }
  ID3D11UnorderedAccessView* u[8] = {};
  c->OMGetRenderTargetsAndUnorderedAccessViews(0, nullptr, nullptr, 0, 8, u);
  for (int i = 0; i < 8; ++i)
    if (u[i]) {
      AddUav(s, 'O', i, u[i]);
      u[i]->Release();
    }
}

// Before a dispatch / draw: signature; capture record + begin stamp.
struct Pending {
  bool on = false;
  Ev* ev = nullptr;  // capture only (pointer into the frame vector; valid until the next NewEv)
  uint64_t sig = 0;
  Out outs[24];
  int nOut = 0;
};
thread_local Pending t_p;

void Pre(Ctx* c, int kind, const uint32_t* a, int na, ID3D11Buffer* indirect, UINT ioff) {
  t_p.on = false;
  if (t_inside || !Mine(c)) return;
  Sig s;
  Ev* e = nullptr;
  t_snapDraw = g_snapOn && Capturing() && g_cur == 0 && kind == kDrawIdxInstInd;
  if (Capturing()) {
    e = NewEv(kind);
    e->binds.reserve(256);
    s.binds = &e->binds;
  }
  __try {
    if (kind <= kDispatchInd)
      CaptureCompute(c, s);
    else
      CaptureGraphics(c, s);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    ++g_c.faults;
    return;
  }
  for (int i = 0; i < na; ++i) s.args = Mix(s.args, a[i]);
  if (indirect) {
    ResInfo& ri = Res(indirect);
    s.args = Mix(Mix(Mix(s.args, 0x1d), ri.cid), ioff);
    s.Add("arg=R%u+%u:%06x ", ri.id, ioff, Cid24(ri.cid));
  }
  const uint64_t sigIn = Mix(Mix(Mix(Mix(Mix(Mix(Mix(s.sh, s.cb), s.srv), s.samp), s.state), s.args), s.outId), kind);
  const uint64_t sig = Mix(sigIn, s.outPrior);
  t_p.on = true;
  t_p.sig = sig;
  t_p.nOut = s.nOut;
  for (int i = 0; i < s.nOut; ++i) t_p.outs[i] = s.outs[i];
  t_p.ev = e;
  if (e) {
    e->sig = sig, e->sigIn = sigIn, e->hSh = s.sh, e->hCb = s.cb, e->hSrv = s.srv, e->hSamp = s.samp;
    e->hState = s.state, e->hArgs = s.args, e->hOutId = s.outId, e->hOutPrior = s.outPrior;
    for (int i = 0; i < na && i < 5; ++i) e->a[i] = a[i];
    if (indirect) CopyArgs(*e, indirect, ioff, kind == kDispatchInd ? 12 : (kind == kDrawInstInd ? 16 : 20));
    Stamp(*e, true);
  }
}

void Post() {
  if (!t_p.on) return;
  t_p.on = false;
  if (t_p.ev && Capturing()) Stamp(*t_p.ev, false);
  for (int i = 0; i < t_p.nOut; ++i) {
    const Out& o = t_p.outs[i];
    SetCid(o.res, Mix(t_p.sig, o.tag));
    if (o.view) g_ctr[o.view] = Mix(t_p.sig, 0xc7700000ull | o.tag);
  }
  if (t_p.ev && t_p.ev->binds.size()) {
    // the outputs' new ids (truncated) close the record
    char buf[48];
    for (int i = 0; i < t_p.nOut; ++i) {
      snprintf(buf, sizeof(buf), "o%u=R%u:%06x ", t_p.outs[i].tag, Res(t_p.outs[i].res).id,
               Cid24(Res(t_p.outs[i].res).cid));
      t_p.ev->binds += buf;
    }
  }
  t_p.ev = nullptr;
}

// Write ops (no signature): the new content id, a record while capturing.
Ev* WriteEv(int kind, bool stamp) {
  if (!Capturing()) return nullptr;
  Ev* e = NewEv(kind);
  if (stamp) Stamp(*e, true);
  return e;
}
void WriteEnd(Ev* e) {
  if (e && e->stamp >= 0 && Capturing()) Stamp(*e, false);
}

// ---------------------------------------------------------------------------
// Entry points from gpu_pass_timing.h (render thread, g_on)
// ---------------------------------------------------------------------------

void SnapPassEnd(void* top) {
  for (Snap& sn : g_snaps) {
    if (sn.top != top || sn.stage) continue;
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = Res(sn.res).bytes;
    bd.Usage = D3D11_USAGE_STAGING;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (FAILED(gpt::g_dev->CreateBuffer(&bd, nullptr, &sn.stage))) {
      sn.stage = nullptr;
      sn.done = true;
      continue;
    }
    t_inside = true;
    gpt::g_ctx->CopyResource(sn.stage, sn.res);
    t_inside = false;
  }
}
// Drain: one snapshot written per call (render thread). True when none is pending.
bool SnapDrain() {
  for (Snap& sn : g_snaps) {
    if (sn.done || !sn.stage) continue;
    D3D11_MAPPED_SUBRESOURCE m{};
    t_inside = true;
    const HRESULT hr = gpt::g_ctx->Map(sn.stage, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m);
    if (hr == S_OK) {
      wchar_t nm[96];
      swprintf_s(nm, L"gdd_snap_%p_R%u.bin", sn.top, sn.rid);
      FILE* f = nullptr;
      if (!g_snapDir.empty() && _wfopen_s(&f, (g_snapDir + nm).c_str(), L"wb") == 0 && f) {
        fwrite(m.pData, 1, Res(sn.res).bytes, f);
        fclose(f);
      }
      gpt::g_ctx->Unmap(sn.stage, 0);
    }
    t_inside = false;
    if (hr != S_OK) return false;
    sn.stage->Release();
    sn.stage = nullptr;
    sn.done = true;
    return false;
  }
  return true;
}

void OnPass(void* pass, bool begin, int) {
  if (begin) {
    if (g_sp < 64) g_stack[g_sp] = pass;
    ++g_sp;
  } else if (g_sp > 0) {
    --g_sp;
    if (g_sp == 0 && !g_snaps.empty() && Capturing() && g_cur == 0) SnapPassEnd(pass);
  }
}

void PreDispatch(Ctx* c, UINT x, UINT y, UINT z) {
  const uint32_t a[3] = {x, y, z};
  Pre(c, kDispatch, a, 3, nullptr, 0);
}
void PreDispatchIndirect(Ctx* c, ID3D11Buffer* b, UINT o) { Pre(c, kDispatchInd, nullptr, 0, b, o); }
void PostWork() { Post(); }

thread_local Ev* t_w = nullptr;
void PreCopy(Ctx* c, ID3D11Resource* d, ID3D11Resource* s) {
  t_w = nullptr;
  if (t_inside || !Mine(c) || !d || !s) return;
  const uint64_t sc = Res(s).cid;
  ResInfo& rd = Res(d);
  if (Ev* e = WriteEv(kCopy, true)) {
    e->a[0] = rd.id, e->a[1] = Res(s).id;
    t_w = e;
  }
  rd.cid = sc;
  ++rd.writes;
}
void PreCopySub(Ctx* c, ID3D11Resource* d, UINT ds, UINT x, UINT y, UINT z, ID3D11Resource* s, UINT ss,
                const D3D11_BOX* b) {
  t_w = nullptr;
  if (t_inside || !Mine(c) || !d || !s) return;
  ResInfo& rs = Res(s);
  ResInfo& rd = Res(d);
  const bool full = !b && ds == 0 && ss == 0 && !x && !y && !z && Single(rd) && Single(rs);
  const uint64_t v = full ? rs.cid
                          : Mix(Mix(Mix(Mix(rd.cid, rs.cid), (static_cast<uint64_t>(ds) << 32) | ss),
                                    (static_cast<uint64_t>(x) << 40) ^ (static_cast<uint64_t>(y) << 20) ^ z),
                                b ? HashBytesRaw(b, sizeof(*b)) : 0);
  if (Ev* e = WriteEv(kCopySub, true)) {
    e->a[0] = rd.id, e->a[1] = rs.id, e->a[2] = b ? b->right - b->left : 0, e->a[3] = ds, e->a[4] = ss;
    t_w = e;
  }
  rd.cid = v;
  ++rd.writes;
}
void PreResolve(Ctx* c, ID3D11Resource* d, UINT ds, ID3D11Resource* s, UINT ss, DXGI_FORMAT f) {
  t_w = nullptr;
  if (t_inside || !Mine(c) || !d || !s) return;
  ResInfo& rs = Res(s);
  ResInfo& rd = Res(d);
  const uint64_t v = Single(rd) ? Mix(Mix(rs.cid, 0x7e5), f) : Mix(Mix(Mix(rd.cid, rs.cid), ds), ss);
  if (Ev* e = WriteEv(kResolve, true)) {
    e->a[0] = rd.id, e->a[1] = rs.id;
    t_w = e;
  }
  rd.cid = v;
  ++rd.writes;
}
size_t UpdateBytes(const ResInfo& ri, UINT sub, const D3D11_BOX* b, UINT rp, UINT dp) {
  if (ri.dim == D3D11_RESOURCE_DIMENSION_BUFFER) return b ? (b->right > b->left ? b->right - b->left : 0) : ri.bytes;
  const UINT mip = ri.mips ? sub % ri.mips : 0;
  UINT h = b ? b->bottom - b->top : std::max(1u, ri.h >> mip);
  UINT d = b ? b->back - b->front : std::max(1u, ri.d >> mip);
  const bool bc = (ri.fmt >= 70 && ri.fmt <= 84) || (ri.fmt >= 94 && ri.fmt <= 99);
  if (bc) h = (h + 3) / 4;
  if (ri.dim == D3D11_RESOURCE_DIMENSION_TEXTURE3D) return static_cast<size_t>(dp) * std::max(1u, d);
  if (ri.dim == D3D11_RESOURCE_DIMENSION_TEXTURE1D) return rp ? rp : 0;
  return static_cast<size_t>(rp) * std::max(1u, h);
}
void PreUpdate(Ctx* c, ID3D11Resource* d, UINT sub, const D3D11_BOX* b, const void* p, UINT rp, UINT dp) {
  t_w = nullptr;
  if (t_inside || !Mine(c) || !d) return;
  ResInfo& rd = Res(d);
  const size_t n = UpdateBytes(rd, sub, b, rp, dp);
  uint64_t h = n && n <= (64u << 20) ? HashBytes(p, n) : 0;
  if (!h) h = Fresh();
  const bool whole = sub == 0 && Single(rd) &&
                     (!b || (rd.dim == D3D11_RESOURCE_DIMENSION_BUFFER && b->left == 0 && b->right >= rd.bytes));
  if (Ev* e = WriteEv(kUpdate, false)) {
    e->a[0] = rd.id, e->a[1] = static_cast<uint32_t>(n), e->a[2] = sub, e->a[3] = whole;
    e->sig = h;
  }
  rd.cid = whole ? Mix(h, 0x0bda7e) : Mix(Mix(Mix(rd.cid, h), sub), b ? HashBytesRaw(b, sizeof(*b)) : 0);
  ++rd.writes;
  if (whole) StoreCb(rd, rd.cid, p, n);
  QuickRepatch();
}
template <class V, class D>
void ClearCommon(int kind, V* v, uint64_t val) {
  ID3D11Resource* r = ResOf(v);
  if (!r) return;
  ResInfo& ri = Res(r);
  const uint64_t vd = ViewDescHash<V, D>(v);
  if (Ev* e = WriteEv(kind, true)) {
    e->a[0] = ri.id;
    t_w = e;
  }
  ri.cid = Single(ri) ? Mix(Mix(Mix(0xc1ea7ull, vd), val), kind) : Mix(Mix(Mix(ri.cid, vd), val), kind);
  ++ri.writes;
}
void PreClearRTV(Ctx* c, ID3D11RenderTargetView* v, const FLOAT col[4]) {
  t_w = nullptr;
  if (t_inside || !Mine(c) || !v) return;
  ClearCommon<ID3D11RenderTargetView, D3D11_RENDER_TARGET_VIEW_DESC>(kClearRTV, v, HashBytesRaw(col, 16));
}
void PreClearDSV(Ctx* c, ID3D11DepthStencilView* v, UINT f, FLOAT d, UINT8 s) {
  t_w = nullptr;
  if (t_inside || !Mine(c) || !v) return;
  uint64_t val = Mix(Mix(f, *reinterpret_cast<const uint32_t*>(&d)), s);
  // A clear of depth only (or stencil only) keeps the other plane: partial.
  ID3D11Resource* r = ResOf(v);
  if (r && f != (D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL)) val = Mix(val, Res(r).cid);
  ClearCommon<ID3D11DepthStencilView, D3D11_DEPTH_STENCIL_VIEW_DESC>(kClearDSV, v, val);
}
void PreClearUAV(Ctx* c, ID3D11UnorderedAccessView* v, const void* vals) {
  t_w = nullptr;
  if (t_inside || !Mine(c) || !v) return;
  ClearCommon<ID3D11UnorderedAccessView, D3D11_UNORDERED_ACCESS_VIEW_DESC>(kClearUAV, v, HashBytesRaw(vals, 16));
}
void PreMips(Ctx* c, ID3D11ShaderResourceView* v) {
  t_w = nullptr;
  if (t_inside || !Mine(c) || !v) return;
  ID3D11Resource* r = ResOf(v);
  if (!r) return;
  ResInfo& ri = Res(r);
  if (Ev* e = WriteEv(kMips, true)) {
    e->a[0] = ri.id;
    t_w = e;
  }
  ri.cid = Mix(ri.cid, 0x3195);
  ++ri.writes;
}
void PostWrite() {
  if (t_w) WriteEnd(t_w);
  t_w = nullptr;
}

// ---------------------------------------------------------------------------
// Our own table hooks
// ---------------------------------------------------------------------------

enum Slot {
  sMap, sUnmap, sDraw, sDrawIndexed, sDrawInstanced, sDrawIndexedInstanced, sDrawIndexedInstancedIndirect,
  sDrawInstancedIndirect, sCopyStructureCount, sUpdateSubresource1, sCopySubresourceRegion1, sClearView,
  sDiscardResource, sDiscardView, sDiscardView1, sCSSetUnorderedAccessViews, sOMSetRenderTargetsAndUnorderedAccessViews,
  sIASetVertexBuffers, sIASetIndexBuffer, sVSSetShaderResources, sPSSetShaderResources, sCSSetShader, sCount
};
const char* const kSlotNames[sCount] = {"Map",
                                        "Unmap",
                                        "Draw",
                                        "DrawIndexed",
                                        "DrawInstanced",
                                        "DrawIndexedInstanced",
                                        "DrawIndexedInstancedIndirect",
                                        "DrawInstancedIndirect",
                                        "CopyStructureCount",
                                        "UpdateSubresource1",
                                        "CopySubresourceRegion1",
                                        "ClearView",
                                        "DiscardResource",
                                        "DiscardView",
                                        "DiscardView1",
                                        "CSSetUnorderedAccessViews",
                                        "OMSetRenderTargetsAndUnorderedAccessViews",
                                        "IASetVertexBuffers",
                                        "IASetIndexBuffer",
                                        "VSSetShaderResources",
                                        "PSSetShaderResources",
                                        "CSSetShader"};
int g_slot[sCount] = {};
void* volatile g_orig[sCount] = {};
std::atomic<bool> g_hooked{false};
void* HookOf(int s);
template <class F>
inline F O(int s) {
  return reinterpret_cast<F>(g_orig[s]);
}
inline void Rearm(Ctx* c, int s) {
  void** vt = gpt::g_table;
  if (!vt || *reinterpret_cast<void***>(c) != vt || !g_hooked.load(std::memory_order_relaxed)) return;
  void* cur = vt[g_slot[s]];
  if (cur == HookOf(s)) return;
  g_orig[s] = cur;
  if (gpt::g_tableWritable)
    InterlockedExchangePointer(&vt[g_slot[s]], HookOf(s));
  else
    d3ds::WriteTablePointer(&vt[g_slot[s]], HookOf(s));
  ++g_c.rearms;
}

struct MapRec {
  void* res;
  UINT sub;
  void* p;
  D3D11_MAP type;
};
std::vector<MapRec> g_maps;

HRESULT STDMETHODCALLTYPE H_Map(Ctx* c, ID3D11Resource* r, UINT sub, D3D11_MAP t, UINT fl,
                                D3D11_MAPPED_SUBRESOURCE* m) {
  const HRESULT hr = O<decltype(&H_Map)>(sMap)(c, r, sub, t, fl, m);
  if (SUCCEEDED(hr) && m && t != D3D11_MAP_READ && !t_inside && Mine(c)) g_maps.push_back({r, sub, m->pData, t});
  Rearm(c, sMap);
  return hr;
}
void STDMETHODCALLTYPE H_Unmap(Ctx* c, ID3D11Resource* r, UINT sub) {
  if (!t_inside && Mine(c)) {
    size_t k = g_maps.size();
    for (size_t i = g_maps.size(); i-- > 0;)
      if (g_maps[i].res == r && g_maps[i].sub == sub) {
        k = i;
        break;
      }
    if (k < g_maps.size()) {
      const MapRec mr = g_maps[k];
      g_maps.erase(g_maps.begin() + static_cast<ptrdiff_t>(k));
      ResInfo& ri = Res(r);
      ++g_c.mapW;
      uint64_t h = 0;
      if (ri.dim != D3D11_RESOURCE_DIMENSION_BUFFER) {
        ++g_c.mapTex;
      } else if (ri.bytes > g_hashCap) {
        ++g_c.mapBig;
      } else {
        h = HashBytes(mr.p, ri.bytes);
        if (h) ++g_c.mapHashed;
      }
      if (Ev* e = WriteEv(kMapW, false)) {
        e->a[0] = ri.id, e->a[1] = ri.bytes, e->a[2] = mr.type, e->a[3] = h != 0;
        e->sig = h;
      }
      ri.cid = h ? Mix(h, 0x3a9) : Fresh();
      ++ri.writes;
      if (h) StoreCb(ri, ri.cid, mr.p, ri.bytes);
      QuickRepatch();
    } else {
      ++g_c.unmapUnknown;
    }
  }
  O<decltype(&H_Unmap)>(sUnmap)(c, r, sub);
  Rearm(c, sUnmap);
}
void STDMETHODCALLTYPE H_Draw(Ctx* c, UINT n, UINT s) {
  const uint32_t a[2] = {n, s};
  Pre(c, kDraw, a, 2, nullptr, 0);
  O<decltype(&H_Draw)>(sDraw)(c, n, s);
  Post();
  Rearm(c, sDraw);
}
void STDMETHODCALLTYPE H_DrawIndexed(Ctx* c, UINT n, UINT s, INT b) {
  const uint32_t a[3] = {n, s, static_cast<uint32_t>(b)};
  Pre(c, kDrawIdx, a, 3, nullptr, 0);
  O<decltype(&H_DrawIndexed)>(sDrawIndexed)(c, n, s, b);
  Post();
  Rearm(c, sDrawIndexed);
}
void STDMETHODCALLTYPE H_DrawInstanced(Ctx* c, UINT n, UINT i, UINT s, UINT si) {
  const uint32_t a[4] = {n, i, s, si};
  Pre(c, kDrawInst, a, 4, nullptr, 0);
  O<decltype(&H_DrawInstanced)>(sDrawInstanced)(c, n, i, s, si);
  Post();
  Rearm(c, sDrawInstanced);
}
void STDMETHODCALLTYPE H_DrawIndexedInstanced(Ctx* c, UINT n, UINT i, UINT s, INT b, UINT si) {
  const uint32_t a[5] = {n, i, s, static_cast<uint32_t>(b), si};
  Pre(c, kDrawIdxInst, a, 5, nullptr, 0);
  O<decltype(&H_DrawIndexedInstanced)>(sDrawIndexedInstanced)(c, n, i, s, b, si);
  Post();
  Rearm(c, sDrawIndexedInstanced);
}
void STDMETHODCALLTYPE H_DrawIndexedInstancedIndirect(Ctx* c, ID3D11Buffer* b, UINT o) {
  Pre(c, kDrawIdxInstInd, nullptr, 0, b, o);
  O<decltype(&H_DrawIndexedInstancedIndirect)>(sDrawIndexedInstancedIndirect)(c, b, o);
  Post();
  Rearm(c, sDrawIndexedInstancedIndirect);
}
void STDMETHODCALLTYPE H_DrawInstancedIndirect(Ctx* c, ID3D11Buffer* b, UINT o) {
  Pre(c, kDrawInstInd, nullptr, 0, b, o);
  O<decltype(&H_DrawInstancedIndirect)>(sDrawInstancedIndirect)(c, b, o);
  Post();
  Rearm(c, sDrawInstancedIndirect);
}
void STDMETHODCALLTYPE H_CopyStructureCount(Ctx* c, ID3D11Buffer* d, UINT off, ID3D11UnorderedAccessView* u) {
  if (!t_inside && Mine(c) && d) {
    auto it = g_ctr.find(u);
    ResInfo& rd = Res(d);
    if (Ev* e = WriteEv(kCopyCount, false)) {
      e->a[0] = rd.id, e->a[1] = off;
      ID3D11Resource* r = ResOf(u);
      e->a[2] = r ? Res(r).id : 0;
    }
    rd.cid = Mix(Mix(Mix(rd.cid, off), it != g_ctr.end() ? it->second : 0x7777), 0xc5c);
    ++rd.writes;
  }
  O<decltype(&H_CopyStructureCount)>(sCopyStructureCount)(c, d, off, u);
  Rearm(c, sCopyStructureCount);
}
void STDMETHODCALLTYPE H_UpdateSubresource1(ID3D11DeviceContext1* c, ID3D11Resource* d, UINT sub, const D3D11_BOX* b,
                                            const void* p, UINT rp, UINT dp, UINT fl) {
  PreUpdate(c, d, sub, b, p, rp, dp);
  if (fl && d && Mine(c)) Res(d).cid = Mix(Res(d).cid, Fresh());  // NO_OVERWRITE / DISCARD: conservative
  O<decltype(&H_UpdateSubresource1)>(sUpdateSubresource1)(c, d, sub, b, p, rp, dp, fl);
  t_w = nullptr;
  Rearm(c, sUpdateSubresource1);
}
void STDMETHODCALLTYPE H_CopySubresourceRegion1(ID3D11DeviceContext1* c, ID3D11Resource* d, UINT ds, UINT x, UINT y,
                                                UINT z, ID3D11Resource* s, UINT ss, const D3D11_BOX* b, UINT fl) {
  PreCopySub(c, d, ds, x, y, z, s, ss, b);
  O<decltype(&H_CopySubresourceRegion1)>(sCopySubresourceRegion1)(c, d, ds, x, y, z, s, ss, b, fl);
  PostWrite();
  Rearm(c, sCopySubresourceRegion1);
}
void STDMETHODCALLTYPE H_ClearView(ID3D11DeviceContext1* c, ID3D11View* v, const FLOAT col[4], const D3D11_RECT* r,
                                   UINT n) {
  if (!t_inside && Mine(c) && v) {
    if (ID3D11Resource* res = ResOf(v)) {
      ResInfo& ri = Res(res);
      if (Ev* e = WriteEv(kClearView, false)) e->a[0] = ri.id;
      ri.cid = Mix(Mix(Mix(ri.cid, HashBytesRaw(col, 16)), n), r && n ? HashBytesRaw(r, n * sizeof(*r)) : 0);
      ++ri.writes;
    }
  }
  O<decltype(&H_ClearView)>(sClearView)(c, v, col, r, n);
  Rearm(c, sClearView);
}
void Discarded(Ctx* c, ID3D11Resource* r) {
  if (t_inside || !Mine(c) || !r) return;
  ResInfo& ri = Res(r);
  if (Ev* e = WriteEv(kDiscard, false)) e->a[0] = ri.id;
  ri.cid = Fresh();
  ++ri.writes;
}
void STDMETHODCALLTYPE H_DiscardResource(ID3D11DeviceContext1* c, ID3D11Resource* r) {
  Discarded(c, r);
  O<decltype(&H_DiscardResource)>(sDiscardResource)(c, r);
  Rearm(c, sDiscardResource);
}
void STDMETHODCALLTYPE H_DiscardView(ID3D11DeviceContext1* c, ID3D11View* v) {
  Discarded(c, v ? ResOf(v) : nullptr);
  O<decltype(&H_DiscardView)>(sDiscardView)(c, v);
  Rearm(c, sDiscardView);
}
void STDMETHODCALLTYPE H_DiscardView1(ID3D11DeviceContext1* c, ID3D11View* v, const D3D11_RECT* r, UINT n) {
  Discarded(c, v ? ResOf(v) : nullptr);
  O<decltype(&H_DiscardView1)>(sDiscardView1)(c, v, r, n);
  Rearm(c, sDiscardView1);
}
void NoteCounts(Ctx* c, UINT n, ID3D11UnorderedAccessView* const* u, const UINT* counts) {
  if (t_inside || !Mine(c) || !u || !counts) return;
  for (UINT i = 0; i < n && i < D3D11_1_UAV_SLOT_COUNT; ++i)
    if (u[i] && counts[i] != 0xffffffffu) g_ctr[u[i]] = Mix(0x1c7ull, counts[i]);
}
void STDMETHODCALLTYPE H_CSSetUnorderedAccessViews(Ctx* c, UINT s, UINT n, ID3D11UnorderedAccessView* const* u,
                                                   const UINT* counts) {
  NoteCounts(c, n, u, counts);
  O<decltype(&H_CSSetUnorderedAccessViews)>(sCSSetUnorderedAccessViews)(c, s, n, u, counts);
  Rearm(c, sCSSetUnorderedAccessViews);
}
void STDMETHODCALLTYPE H_OMSetRTUAV(Ctx* c, UINT nr, ID3D11RenderTargetView* const* r, ID3D11DepthStencilView* d,
                                    UINT us, UINT nu, ID3D11UnorderedAccessView* const* u, const UINT* counts) {
  if (nu != D3D11_KEEP_UNORDERED_ACCESS_VIEWS) NoteCounts(c, nu, u, counts);
  O<decltype(&H_OMSetRTUAV)>(sOMSetRenderTargetsAndUnorderedAccessViews)(c, nr, r, d, us, nu, u, counts);
  Rearm(c, sOMSetRenderTargetsAndUnorderedAccessViews);
}

// Setters nobody else hooks in the table, called before nearly every draw and
// dispatch: re-arm the swapped draw/dispatch entries (QuickRepatch), forward.
void STDMETHODCALLTYPE H_IASetVertexBuffers(Ctx* c, UINT s, UINT n, ID3D11Buffer* const* b, const UINT* st,
                                            const UINT* o) {
  if (Mine(c)) QuickRepatch();
  O<decltype(&H_IASetVertexBuffers)>(sIASetVertexBuffers)(c, s, n, b, st, o);
  Rearm(c, sIASetVertexBuffers);
}
void STDMETHODCALLTYPE H_IASetIndexBuffer(Ctx* c, ID3D11Buffer* b, DXGI_FORMAT f, UINT o) {
  if (Mine(c)) QuickRepatch();
  O<decltype(&H_IASetIndexBuffer)>(sIASetIndexBuffer)(c, b, f, o);
  Rearm(c, sIASetIndexBuffer);
}
void STDMETHODCALLTYPE H_VSSetShaderResources(Ctx* c, UINT s, UINT n, ID3D11ShaderResourceView* const* v) {
  if (Mine(c)) QuickRepatch();
  O<decltype(&H_VSSetShaderResources)>(sVSSetShaderResources)(c, s, n, v);
  Rearm(c, sVSSetShaderResources);
}
void STDMETHODCALLTYPE H_PSSetShaderResources(Ctx* c, UINT s, UINT n, ID3D11ShaderResourceView* const* v) {
  if (Mine(c)) QuickRepatch();
  O<decltype(&H_PSSetShaderResources)>(sPSSetShaderResources)(c, s, n, v);
  Rearm(c, sPSSetShaderResources);
}
void STDMETHODCALLTYPE H_CSSetShader(Ctx* c, ID3D11ComputeShader* sh, ID3D11ClassInstance* const* ci, UINT n) {
  if (Mine(c)) QuickRepatch();
  O<decltype(&H_CSSetShader)>(sCSSetShader)(c, sh, ci, n);
  Rearm(c, sCSSetShader);
}

void* const kHooks[sCount] = {reinterpret_cast<void*>(&H_Map),
                              reinterpret_cast<void*>(&H_Unmap),
                              reinterpret_cast<void*>(&H_Draw),
                              reinterpret_cast<void*>(&H_DrawIndexed),
                              reinterpret_cast<void*>(&H_DrawInstanced),
                              reinterpret_cast<void*>(&H_DrawIndexedInstanced),
                              reinterpret_cast<void*>(&H_DrawIndexedInstancedIndirect),
                              reinterpret_cast<void*>(&H_DrawInstancedIndirect),
                              reinterpret_cast<void*>(&H_CopyStructureCount),
                              reinterpret_cast<void*>(&H_UpdateSubresource1),
                              reinterpret_cast<void*>(&H_CopySubresourceRegion1),
                              reinterpret_cast<void*>(&H_ClearView),
                              reinterpret_cast<void*>(&H_DiscardResource),
                              reinterpret_cast<void*>(&H_DiscardView),
                              reinterpret_cast<void*>(&H_DiscardView1),
                              reinterpret_cast<void*>(&H_CSSetUnorderedAccessViews),
                              reinterpret_cast<void*>(&H_OMSetRTUAV),
                              reinterpret_cast<void*>(&H_IASetVertexBuffers),
                              reinterpret_cast<void*>(&H_IASetIndexBuffer),
                              reinterpret_cast<void*>(&H_VSSetShaderResources),
                              reinterpret_cast<void*>(&H_PSSetShaderResources),
                              reinterpret_cast<void*>(&H_CSSetShader)};
void* HookOf(int s) { return kHooks[s]; }

// Called from gpt::RepatchOps (render thread) while g_on.
void Repatch() {
  void** vt = gpt::g_table;
  if (!vt) return;
  if (!g_hooked.load(std::memory_order_relaxed)) {
    for (int i = 0; i < sCount; ++i) g_orig[i] = vt[g_slot[i]];
    g_hooked = true;
  }
  for (int i = 0; i < sCount; ++i) {
    void* cur = vt[g_slot[i]];
    if (cur == kHooks[i]) continue;
    g_orig[i] = cur;
    if (gpt::g_tableWritable)
      InterlockedExchangePointer(&vt[g_slot[i]], kHooks[i]);
    else
      d3ds::WriteTablePointer(&vt[g_slot[i]], kHooks[i]);
    ++g_c.rehooks;
  }
}
// Called from gpt::WithdrawOps.
void Withdraw() {
  if (!g_hooked.exchange(false)) return;
  __try {
    for (int i = 0; i < sCount; ++i)
      if (gpt::g_table[g_slot[i]] == kHooks[i]) d3ds::WriteTablePointer(&gpt::g_table[g_slot[i]], g_orig[i]);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }
}

// ---------------------------------------------------------------------------
// Frames (render thread, from gpt::OpenFrame)
// ---------------------------------------------------------------------------

bool CreateFrameObjects() {
  ID3D11Device* dev = gpt::g_dev;
  D3D11_QUERY_DESC dj{D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
  D3D11_QUERY_DESC ts{D3D11_QUERY_TIMESTAMP, 0};
  D3D11_BUFFER_DESC bd{};
  bd.ByteWidth = kMaxArgs * 20;
  bd.Usage = D3D11_USAGE_STAGING;
  bd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  for (int k = 0; k < g_capFrames; ++k) {
    Frame& f = g_frames[k];
    if (FAILED(dev->CreateQuery(&dj, &f.dj))) return false;
    for (int i = 0; i < kMaxStamps; ++i)
      if (FAILED(dev->CreateQuery(&ts, &f.ts[i]))) return false;
    if (FAILED(dev->CreateBuffer(&bd, nullptr, &f.argStage))) f.argStage = nullptr;
  }
  return true;
}

void TryRead(Frame& f) {
  if (!f.ended || f.read) return;
  if (!g_queriesOk || !f.dj) {
    // Query creation failed (driver limit, out of memory, device removed):
    // no GPU times for this frame. Before, GetData was called with a null
    // query on the render thread, or the phase waited for its 30 s timeout.
    f.freq = 0;
    f.disjoint = true;
    f.read = true;
    return;
  }
  D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj{};
  if (gpt::g_ctx->GetData(f.dj, &dj, sizeof(dj), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) return;
  f.disjoint = dj.Disjoint != 0;
  f.freq = dj.Frequency;
  f.ticks.assign(f.nts, 0);
  for (int i = 0; i < f.nts; ++i)
    if (gpt::g_ctx->GetData(f.ts[i], &f.ticks[i], sizeof(uint64_t), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) return;
  if (f.argStage && f.nArgs) {
    D3D11_MAPPED_SUBRESOURCE m{};
    t_inside = true;
    const HRESULT hr = gpt::g_ctx->Map(f.argStage, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m);
    if (hr == S_OK) {
      f.args.resize(static_cast<size_t>(f.nArgs) * 5);
      memcpy(f.args.data(), m.pData, f.args.size() * 4);
      gpt::g_ctx->Unmap(f.argStage, 0);
    }
    t_inside = false;
    if (hr != S_OK) return;
  }
  f.read = true;
}

void OnFrameOpen(uint64_t gptSerial) {
  const int ph = g_phase.load(std::memory_order_relaxed);
  if (ph == 0 || ph == 4) return;
  ++g_framesSeen;
  if (!g_queriesOk && g_frames) {
    g_queriesOk = CreateFrameObjects();
    if (!g_queriesOk) Log("  gpu dedup: query creation failed; no GPU times");
  }
  if (g_cur >= 0) {  // close the capture frame
    Frame& f = g_frames[g_cur];
    if (g_queriesOk) gpt::g_ctx->End(f.dj);
    f.ended = true;
    g_cur = g_cur + 1 < g_capFrames ? g_cur + 1 : -1;
    if (g_cur < 0) g_phase = 3;
  } else if (ph == 1 && g_framesSeen > g_warmFrames) {
    g_phase = 2;
    g_cur = 0;
  }
  if (g_cur >= 0) {
    Frame& f = g_frames[g_cur];
    f.used = true;
    f.gptSerial = gptSerial;
    f.ev.reserve(8192);
    if (g_queriesOk) gpt::g_ctx->Begin(f.dj);
  }
  bool all = true;
  for (int k = 0; k < g_capFrames; ++k) {
    TryRead(g_frames[k]);
    if (g_frames[k].used && !g_frames[k].read) all = false;
  }
  if (g_phase.load() == 3 && !SnapDrain()) all = false;
  if (g_phase.load() == 3 && all) g_phase = 4;
}

// gpt::Deactivate: a capture frame still open ends here (its disjoint query).
void OnDeactivate() {
  if (g_cur >= 0 && g_frames) {
    if (g_queriesOk) gpt::g_ctx->End(g_frames[g_cur].dj);
    g_frames[g_cur].ended = true;
    g_cur = -1;
  }
  g_sp = 0;
}

// ---------------------------------------------------------------------------
// Report (suite thread, after gpt::End)
// ---------------------------------------------------------------------------

void PassNameRaw(void* p, char* buf, size_t cap) {
  __try {
    gpt::DescribeRaw(p, buf, cap);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    strcpy_s(buf, cap, "(unreadable)");
  }
}
std::string PassNameGuarded(void* p) {
  char buf[1024] = "";
  PassNameRaw(p, buf, sizeof(buf));
  return buf;
}

bool PassBytes(void* p, uint8_t* out, size_t n) {
  __try {
    memcpy(out, p, n);
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

void Csv(const std::string& s, FILE* f) {
  fputc('"', f);
  for (char ch : s) fputc(ch == '"' ? '\'' : ch, f);
  fputc('"', f);
}

void WriteFiles(const std::wstring& dir) {
  std::unordered_map<void*, uint32_t> passIds;
  std::vector<void*> passes;
  auto pid = [&](void* p) -> uint32_t {
    if (!p) return 0;
    auto it = passIds.find(p);
    if (it != passIds.end()) return it->second;
    passes.push_back(p);
    return passIds[p] = static_cast<uint32_t>(passes.size());
  };
  size_t rows = 0;
  FILE* f = nullptr;
  if (_wfopen_s(&f, (dir + L"gdd_events.csv").c_str(), L"w") == 0 && f) {
    fprintf(f,
            "frame,seq,kind,top,pass,depth,issuer,gpu_us,sig,sigIn,hSh,hCb,hSrv,hSamp,hState,hArgs,hOutId,hOutPrior,"
            "a0,a1,a2,a3,a4,ia0,ia1,ia2,ia3,ia4,binds\n");
    for (int k = 0; k < g_capFrames; ++k) {
      Frame& fr = g_frames[k];
      if (!fr.used) continue;
      const double us = fr.read && fr.freq && !fr.disjoint ? 1e6 / static_cast<double>(fr.freq) : 0;
      for (const Ev& e : fr.ev) {
        double g = -1;
        if (us > 0 && e.stamp >= 0 && e.stamp + 1 < static_cast<int>(fr.ticks.size()) && fr.ticks[e.stamp + 1] >= fr.ticks[e.stamp])
          g = (fr.ticks[e.stamp + 1] - fr.ticks[e.stamp]) * us;
        uint32_t ia[5] = {};
        if (e.argIdx >= 0 && static_cast<size_t>(e.argIdx) * 5 + 5 <= fr.args.size())
          memcpy(ia, &fr.args[static_cast<size_t>(e.argIdx) * 5], sizeof(ia));
        fprintf(f, "%d,%u,%s,%u,%u,%u,%u,%.3f,%016llx,%016llx,%016llx,%016llx,%016llx,%016llx,%016llx,%016llx,%016llx,%016llx,"
                   "%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,",
                k, e.seq, kKindName[e.kind], pid(e.top), pid(e.pass), e.depth, e.issuer, g,
                static_cast<unsigned long long>(e.sig), static_cast<unsigned long long>(e.sigIn),
                static_cast<unsigned long long>(e.hSh), static_cast<unsigned long long>(e.hCb),
                static_cast<unsigned long long>(e.hSrv), static_cast<unsigned long long>(e.hSamp),
                static_cast<unsigned long long>(e.hState), static_cast<unsigned long long>(e.hArgs),
                static_cast<unsigned long long>(e.hOutId), static_cast<unsigned long long>(e.hOutPrior), e.a[0],
                e.a[1], e.a[2], e.a[3], e.a[4], ia[0], ia[1], ia[2], ia[3], ia[4]);
        Csv(e.binds, f);
        fputc('\n', f);
        ++rows;
      }
    }
    fclose(f);
  }
  if (_wfopen_s(&f, (dir + L"gdd_pass.csv").c_str(), L"w") == 0 && f) {
    fprintf(f, "id,ptr,name\n");
    for (size_t i = 0; i < passes.size(); ++i) {
      fprintf(f, "%zu,%p,", i + 1, passes[i]);
      Csv(PassNameGuarded(passes[i]), f);
      fputc('\n', f);
    }
    fclose(f);
  }
  if (_wfopen_s(&f, (dir + L"gdd_res.csv").c_str(), L"w") == 0 && f) {
    fprintf(f, "id,ptr,dim,bytes,w,h,d,mips,arr,fmt,bind,misc,usage,stride,samples,writes\n");
    for (void* p : g_resOrder) {
      const ResInfo& r = g_res[p];
      fprintf(f, "%u,%p,%u,%u,%u,%u,%u,%u,%u,%u,0x%x,0x%x,%u,%u,%u,%llu\n", r.id, p, r.dim, r.bytes, r.w, r.h, r.d,
              r.mips, r.arr, r.fmt, r.bind, r.misc, r.usage, r.stride, r.samples,
              static_cast<unsigned long long>(r.writes));
    }
    fclose(f);
  }
  if (_wfopen_s(&f, (dir + L"gdd_issuer.csv").c_str(), L"w") == 0 && f) {
    fprintf(f, "id,a,b\n");
    for (size_t i = 0; i < g_callers.size(); ++i) {
      char a[200] = "?", b[200] = "?";
      gpt::ModRva(g_callers[i].a, a, sizeof(a));
      gpt::ModRva(g_callers[i].b, b, sizeof(b));
      fprintf(f, "%zu,%s,%s\n", i + 1, a, b);
    }
    fclose(f);
  }
  if (_wfopen_s(&f, (dir + L"gdd_cb.csv").c_str(), L"w") == 0 && f) {
    fprintf(f, "cid24,size,hex\n");
    for (auto& kv : g_cbStore) {
      fprintf(f, "%06x,%zu,", Cid24(kv.first), kv.second.size());
      for (uint8_t b : kv.second) fprintf(f, "%02x", b);
      fputc('\n', f);
    }
    fclose(f);
  }
  if (_wfopen_s(&f, (dir + L"gdd_passbytes.csv").c_str(), L"w") == 0 && f) {
    fprintf(f, "id,hex\n");
    for (size_t i = 0; i < passes.size(); ++i) {
      uint8_t b[0x400] = {};
      if (!PassBytes(passes[i], b, sizeof(b))) continue;
      fprintf(f, "%zu,", i + 1);
      for (uint8_t x : b) fprintf(f, "%02x", x);
      fputc('\n', f);
    }
    fclose(f);
  }
  if (_wfopen_s(&f, (dir + L"gdd_shader.csv").c_str(), L"w") == 0 && f) {
    fprintf(f, "id,stage,ptr\n");
    for (size_t i = 0; i < g_shaders.size(); ++i) fprintf(f, "%zu,%c,%p\n", i + 1, g_shaders[i].second, g_shaders[i].first);
    fclose(f);
  }
  Log("  gpu dedup: %zu event rows, %zu passes, %zu resources, %zu issuers, %zu shaders written to %ls gdd_*.csv", rows,
      passes.size(), g_resOrder.size(), g_callers.size(), g_shaders.size(), dir.c_str());
}

void Reset() {
  g_res.clear();
  g_resOrder.clear();
  g_ctr.clear();
  g_callers.clear();
  g_shaderIds.clear();
  g_shaders.clear();
  g_maps.clear();
  for (Snap& sn : g_snaps)
    if (sn.stage) sn.stage->Release();
  g_snaps.clear();
  g_cbStore.clear();
  g_isCascade.clear();
  g_cbStoreBytes = 0;
  g_c = {};
  g_sp = 0;
  g_seq = 0;
  g_cur = -1;
  g_framesSeen = 0;
  g_queriesOk = false;
}

void FreeFrames() {
  if (!g_frames) return;
  for (int k = 0; k < kMaxCap; ++k) {
    Frame& f = g_frames[k];
    if (f.dj) f.dj->Release();
    for (auto*& q : f.ts)
      if (q) q->Release(), q = nullptr;
    if (f.argStage) f.argStage->Release();
  }
  delete[] g_frames;
  g_frames = nullptr;
}

// Suite phase.
void Measure(int capFrames, int warmFrames, size_t hashCap, const std::wstring& dir, bool snap,
             std::atomic<uint64_t>& frameCounter) {
  if (!g_self)
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&Measure), &g_self);
  for (int i = 0; i < sCount; ++i) {
    g_slot[i] = DcsQv_ContextSlot(kSlotNames[i]);
    if (g_slot[i] < 0) {
      Log("  gpu dedup: unknown context slot %s", kSlotNames[i]);
      return;
    }
  }
  Reset();
  g_capFrames = std::max(1, std::min(kMaxCap, capFrames));
  g_warmFrames = std::max(2, warmFrames);
  g_hashCap = hashCap;
  g_snapOn = snap && !dir.empty();
  g_snapDir = dir;
  g_frames = new (std::nothrow) Frame[kMaxCap];
  if (!g_frames) return;
  g_phase = 1;
  g_on = true;
  if (!gpt::Begin(frameCounter, false) && !gpt::g_sessDev) {
    g_on = false;
    g_phase = 0;
    FreeFrames();
    return;
  }
  if (gpt::g_ctx && !g_ctx1) gpt::g_ctx->QueryInterface(__uuidof(ID3D11DeviceContext1), reinterpret_cast<void**>(&g_ctx1));
  if (g_ctx1) g_ctx1->Release();  // the context outlives the phase; plain pointer
  ptiming::Reset();
  ptiming::g_recording = true;
  LARGE_INTEGER a, b;
  QueryPerformanceCounter(&a);
  const uint64_t f0 = frameCounter.load();
  for (int i = 0; i < 600 && g_phase.load() != 4 && gpt::g_active.load(); ++i) Sleep(50);
  QueryPerformanceCounter(&b);
  ptiming::g_recording = false;
  const uint64_t frames = frameCounter.load() - f0;
  gpt::End();
  g_on = false;
  const int ph = g_phase.exchange(0);
  int used = 0, read = 0;
  size_t evs = 0;
  for (int k = 0; k < g_capFrames; ++k) {
    used += g_frames[k].used, read += g_frames[k].read;
    evs += g_frames[k].ev.size();
  }
  Log("  gpu dedup: phase end %d, %llu frames in %.1f s, %d captured (%d read back), %zu events; stamps skipped %llu, "
      "args skipped %llu; maps (write) %llu: hashed %llu, over %zu bytes %llu, textures %llu; unmatched unmaps %llu; "
      "calls on other threads %llu; faults %llu; table rewrites taken back %llu at boundaries, %llu after a call",
      ph, static_cast<unsigned long long>(frames), (b.QuadPart - a.QuadPart) * g_qpcToUs / 1e6, used, read, evs,
      static_cast<unsigned long long>(g_c.stampsSkipped), static_cast<unsigned long long>(g_c.argsSkipped),
      static_cast<unsigned long long>(g_c.mapW), static_cast<unsigned long long>(g_c.mapHashed), g_hashCap,
      static_cast<unsigned long long>(g_c.mapBig), static_cast<unsigned long long>(g_c.mapTex),
      static_cast<unsigned long long>(g_c.unmapUnknown), static_cast<unsigned long long>(g_c.offThread),
      static_cast<unsigned long long>(g_c.faults), static_cast<unsigned long long>(g_c.rehooks),
      static_cast<unsigned long long>(g_c.rearms));
  if (!dir.empty() && used) WriteFiles(dir);
  size_t snapsWritten = 0;
  for (Snap& sn : g_snaps) {
    snapsWritten += sn.done && !sn.stage;
    if (sn.stage) sn.stage->Release();
    sn.stage = nullptr;
  }
  if (g_snapOn) Log("  gpu dedup: %zu instance-buffer snapshots registered, %zu written", g_snaps.size(), snapsWritten);
  // Summary: events and GPU time per kind and top-level pass name.
  std::map<std::string, std::pair<double, double>> sum;  // name|kind -> (count, us)
  std::unordered_map<void*, std::string> names;
  int nf = 0;
  for (int k = 0; k < g_capFrames; ++k) {
    Frame& fr = g_frames[k];
    if (!fr.used || !fr.read) continue;
    ++nf;
    const double us = fr.freq && !fr.disjoint ? 1e6 / static_cast<double>(fr.freq) : 0;
    for (const Ev& e : fr.ev) {
      auto it = names.find(e.top);
      if (it == names.end()) it = names.emplace(e.top, e.top ? ptiming::RttiName(e.top) : std::string("(outside)")).first;
      auto& s = sum[it->second + " | " + kKindName[e.kind]];
      s.first += 1;
      if (us > 0 && e.stamp >= 0 && fr.ticks[e.stamp + 1] >= fr.ticks[e.stamp]) s.second += (fr.ticks[e.stamp + 1] - fr.ticks[e.stamp]) * us;
    }
  }
  if (nf) {
    Log("  gpu dedup: per top-level pass kind and op, per frame (%d frames): count, GPU us (stamped ops)", nf);
    for (auto& kv : sum)
      Log("    %-60.60s %8.1f %9.1f", kv.first.c_str(), kv.second.first / nf, kv.second.second / nf);
  }
  FreeFrames();
}

}  // namespace gdd
