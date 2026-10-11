// Shadow recorder, R17 stages S1-S6 (R15 A1): the cascades' ModelMaterialMT
// shadow casters recorded on worker threads into D3D11 command lists, which
// the render thread executes inside DCS's own cascade passes. [Model]
// ShadowRecorder (default 0), ShadowRecorderScope, ShadowRecorderWaitUs,
// ShadowRecorderPriority, ShadowRecorderSplit, ShadowRecorderHelpers, ShadowRecorderInstancing,
// [Suite] ShadowRecVerify, [Suite] BenchShadowRecorder (bench mode 28).
//
// Tags: [V] verified in the binary (DCS 2.9.30: NGModel, dx11backend,
// GraphicsCore) or the shader sources, [I] inferred, [M] measured. Design and
// section numbers: docs/research/R17_shadow_recorder.md.
//
// What DCS does per ModelMaterialMT caster [V, R17 2.1]: ShadowMapRenderable
// vt[1] (NGModel 0x443e0) tail-jumps to the material's slot 5 (0x17750:
// mat+0x18c = [item+0xd4]; FX sbPositions = page [item+0xd0]; when textured,
// slot 26 per texture handle: streaming request vt[23], view, SetResource;
// shadow_tex.h runs a copy that skips the sets no shadow pass reads but keeps
// their streaming request), then the submit 0x131e0: material CB upload
// (mat+0x90, 0x130 bytes, Map DISCARD on [mat+0x1c0]), indices [mesh+0], the
// vertex stream [mesh+0x18] (DX11Shader vt[38] 0x1dbc0), the technique, and
// DX11Renderer::draw 0x14870(pass 0, prim [mesh+0x20c], start 0, count
// [mesh+0x210], instances 0): FX pass Apply (states by renderer+0xd4,
// shaders, CBs, SRVs, samplers), input layout and vertex buffers (0x1ad10),
// topology, index buffer ([ib+0x28], [ib+0x3c] == 4 ? R32 : R16, offset 0)
// and, for prim 4, DrawIndexed(3 * count, 0, 0) [V 0x14a5e, 0x14c1c].
//
// This file:
//  - Scope ([Model] ShadowRecorderScope): bits 0-3 = cascades (the index at
//    pass+0x60; one worker and one command list each, S5), bit 8 =
//    untextured casters (S3), bit 9 = textured casters (S4). 0x101 = cascade
//    0 untextured, 0x30f = all four cascades, both kinds. DCS draws every
//    other caster of those passes as before, in the same pass.
//  - Probes (render thread, R17 S2): a caster whose key or mesh is not known
//    yet is drawn by DCS through shadow batching's renderer vtable copy, and
//    right after DCS's D3D draw (shadowbatch::t_after) the immediate context
//    and the drawing DX11Shader's stream state are read back. Keys are
//    (DX11Shader, technique, renderer flags): VS (reflection of shadow_inst's
//    compile (b) of pass 0: only b7, def_uniforms, sbPositions), states, and
//    for alpha-tested keys the PS with its samplers and the slot of each
//    texture the shadow pass reads (found by the view DCS's own draw bound
//    there). Meshes are (mesh, DX11Shader, technique).
//  - Jobs (workers, queued at RenderGraph::render entry, R17 6.4): each
//    cascade's vector is snapshotted and classified; recordable casters get
//    the material's 0x130 CB bytes with +0xfc = [item+0xd4] in the worker's CB
//    ring and one DrawIndexed with the key's and mesh's objects (and, when
//    textured, the read textures' views at the key's PS slots). Split
//    cascades ([Model] ShadowRecorderSplit) share each job with up to 3
//    helper workers ([Model] ShadowRecorderHelpers; by caster count, one
//    more after a late or tight job): stage A's reads (committed by the
//    primary in vector order as they arrive) and chunks of both phases'
//    groups, each helper on its own list. Scheduling only: the same
//    classification, the same draws, executed in list order (order-free).
//  - Texture views (S4): a streamed texture's views are released by DCS's
//    mip-set swap on the render thread (getSRV 0x47c60 -> 0x49ca0 -> 0x48e60,
//    which calls Release on every view of the old set [V]), so a worker never
//    reads a view itself. The worker collects the job's (texture, aux, type)
//    keys and waits; the render thread predicts each view (gb_batch.h
//    PredictView, DCS's getSRV without a swap) and takes a reference
//    (snapshot), after the first top-level render pass that ends once the
//    keys are ready (pass_timing.h's execute hook), at the latest at the
//    cascade's pass; then the worker records with those views. A key with a
//    swap due, an unknown texture class or an unpredictable view makes its
//    casters residual.
//  - Pass (render thread): the job is waited for (bounded) and checked:
//    vector identity, page views, the CB dwords its keys' shaders read
//    (guard: the G-buffer passes run between render entry and the cascades
//    and may rewrite material bytes [R17 3]). Then DCS's loop is given a new
//    caster list for the call (S6): {begin, end} of the cascade's vector
//    descriptor (read once by 0xa5500 at 0xa5631, after its clear and binder;
//    the loop calls only item->vt[1](item, data+0x10) [V 0xa5640]) point at
//    [our exec object, residual casters...], restored in a __finally. The
//    exec object's vt[1] runs first in the loop: it checks the depth target,
//    viewports, scissors and renderer flags against the recorded ones,
//    replays each recorded texture's streaming request once (vt[23], size
//    Vec2i(-1,-1) as slot 5) and re-predicts every recorded texture's view
//    (must equal the snapshot, no swap due), then CopyResource(our b7, DCS's
//    b7) and ExecuteCommandList(list, TRUE) (the immediate state is
//    restored). If a check fails it calls vt[1] of every recorded caster
//    itself, so DCS draws them stock right there. Recorded casters never
//    reach any hook. After the pass each recorded material's mat+0x18c is set
//    to its last caster's [item+0xd4] (the stock end state).
// Order: every caster of a cascade is depth GREATER with writes and no
// blending [M S0], so each texel's result does not depend on the draw order
// [R17 4]. Skipping a caster's slot-5 work (FX variable sets, material CB
// upload, DX11Shader stream lists) is what shadow batching's members already
// do (shadow_batch.h; shadow_tex.h exactness part 3). Streaming requests:
// stock makes one per texture set per caster; the replay makes one per
// texture per cascade with the same size [I: tex_bind.h, repeats of the same
// (texture, size) within a 1 ms tick store the same flag and {level, time};
// the replay runs within the cascade's pass]. Shadow batching's groups
// (material, mesh, page, read-texture key) are recorded whole or not at all:
// a group class with any residual caster makes all of its casters residual.
//
// Verification ([Suite] ShadowRecVerify): each recorded cascade runs twice in
// the same frame, stock (DCS draws every caster, the original list) and
// recorded, each from its own clear; the depth targets are compared texel for
// texel (shadow_batch.h's compare), per cascade; any difference latches the
// recorder off.
//
// Fail-safe: an unknown class, an unprobed key or mesh, a failed check or a
// changed field makes that caster residual (DCS draws it) or the whole pass
// stock; DCS memory is read under SEH; a fault, a verify mismatch or a
// worker fault latches the recorder off for the session; off = pass-through
// (the chain stays installed once it was). Requires shadow batching's hooks
// (installed, batching itself on or off), its leaders passing the group base
// without swapping [item+0xd4] (shadowbatch::g_psoDirect), G-buffer batching
// off (its leaders still swap it), a device that is not SINGLETHREADED and
// has driver command lists and constant-buffer offsets.
// Included once from main.cpp inside its anonymous namespace, after
// pass_timing.h, shadow_batch.h, gb_batch.h, shadow_rec_count.h,
// deferred_rec.h and split_filter.h.
#pragma once

// gpu_split.h (measure only, [Suite] GpuSplit; also called from gb_rec.h):
// GPU query brackets and a mesh census around the recorders' replays. g_on
// stays 0 unless that suite phase runs.
namespace gsplit {
std::atomic<int> g_on{0};
void Bracket(int site, int idx, bool begin);
void MeshDraw(int site, int idx, ID3D11Buffer* ib, DXGI_FORMAT fmt, UINT count, UINT weight, bool textured,
              ID3D11Buffer* vb, UINT stride);
}  // namespace gsplit

namespace shrec {

// ---------------------------------------------------------------------------
// Constants, scope, reasons
// ---------------------------------------------------------------------------
constexpr uint32_t kCbOff = 0x90, kCbLen = 0x130, kPsoCb = 0xfc;  // def_uniforms in the material [V NGModel 0x13219]
constexpr UINT kB7 = 7;              // per-view buffer (binder, R17 1 step 5)
constexpr uint32_t kPrimTriList = 4;  // draw prim 4: TRIANGLELIST, 3 indices per primitive [V dx11backend 0x14a5e]
// renderer+0x2120 bits that skip a draw or a part of it [V 0x14925, 0x14b83, 0x14c27, 0x1ad38].
constexpr uint32_t kDbgBits = 0x1 | 0x4 | 0x10 | 0x40;
constexpr uint64_t kTexSize = ~0ull;  // Vec2i(-1, -1): the size slot 5 passes [V NGModel 0x177f4]
constexpr int kSlots = 4;             // cascades 0-3, one worker each
constexpr size_t kMaxCasters = 1 << 15;
constexpr uint32_t kMatTable = 1 << 14;  // power of two
constexpr uint32_t kMaxRecMats = 1 << 12;
constexpr uint32_t kMaxPages = 256;
constexpr uint32_t kMaxWants = 64;    // probe requests per job
constexpr uint32_t kWantTable = 512;  // power of two
constexpr uint32_t kMaxTexKeys = 4096;
constexpr uint32_t kTexTable = 8192;  // power of two
constexpr uint32_t kMaxSetRefs = 1 << 17;
constexpr uint32_t kGroupTable = 1 << 15;  // power of two
constexpr int kMaxPsTex = 4;
constexpr int kProbesPerPass = 48;
constexpr DWORD kWaitMs = 4;          // the render thread's wait for keys before a late snapshot (bounded by the budget)
constexpr DWORD kGoWaitMs = 100;  // a worker waits this long for its texture snapshot
constexpr size_t kKeyTable = 4096, kMeshTable = 32768;  // powers of two
constexpr UINT kVp = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
constexpr UINT kSrvSlots = D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT;
constexpr UINT kSampSlots = D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT;
// DX11StateManager's sampler pool, bound at s5-s15 on every stage (samplers11.hlsl USE_SAMPLERSTATEPOOL)
// [V R19 2.3]. Per pass: read at the exec entry and compared with the job's, bound in each list's prologue.
constexpr UINT kPoolFirst = 5;
constexpr uint32_t kPoolMask = ((1u << kSampSlots) - 1) & ~((1u << kPoolFirst) - 1);
// The PS pool slots s5-s15 of two sampler arrays are the same objects.
inline bool PoolSame(const void* const* a, const void* const* b) {
  for (UINT q = kPoolFirst; q < kSampSlots; ++q)
    if (a[q] != b[q]) return false;
  return true;
}
// The sampler slots a key binds itself (below the pool): its FX dependencies,
// or every slot when those were not read.
inline uint32_t KeyOwnSamplers(uint32_t deps) { return deps & ~kPoolMask; }
constexpr UINT kCbSlots = D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT;

// Texture classes a recorder refused (the texture's vtable and its inner
// object's), counted: what the "texture class" residuals are. Names from the
// classes' MSVC RTTI (vtable[-1] -> complete object locator -> type descriptor).
constexpr int kCensus = 16;
struct ClassCensus {
  std::atomic<uintptr_t> vt[kCensus] = {};
  std::atomic<uintptr_t> inner[kCensus] = {};
  std::atomic<uint64_t> n[kCensus] = {};
  std::atomic<uint64_t> other{0};
};
inline void CensusAdd(ClassCensus& c, const void* vt, const void* inner) {
  const uintptr_t v = reinterpret_cast<uintptr_t>(vt), in = reinterpret_cast<uintptr_t>(inner);
  for (int i = 0; i < kCensus; ++i) {
    uintptr_t cur = c.vt[i].load(std::memory_order_acquire);
    if (!cur) {
      if (c.vt[i].compare_exchange_strong(cur, v, std::memory_order_acq_rel)) {
        c.inner[i].store(in, std::memory_order_release);
        c.n[i].fetch_add(1, std::memory_order_relaxed);
        return;
      }
    }
    if (cur == v && c.inner[i].load(std::memory_order_acquire) == in) {
      c.n[i].fetch_add(1, std::memory_order_relaxed);
      return;
    }
  }
  c.other.fetch_add(1, std::memory_order_relaxed);
}
// "module+0xRVA (RTTI name)" for a vtable. Plain part SEH-guarded.
bool RttiNameRaw(const void* vt, char* out, size_t cap) {
  const uint8_t* col = static_cast<const uint8_t* const*>(vt)[-1];
  if (!col || *reinterpret_cast<const uint32_t*>(col) != 1) return false;  // x64 locator signature
  const uint32_t self = *reinterpret_cast<const uint32_t*>(col + 0x14);
  const uint8_t* base = col - self;
  const uint32_t td = *reinterpret_cast<const uint32_t*>(col + 0xc);
  const char* name = reinterpret_cast<const char*>(base + td + 0x10);
  size_t i = 0;
  for (; i + 1 < cap && name[i] && i < 120; ++i) out[i] = name[i];
  out[i] = 0;
  return i > 0;
}
bool RttiNameGuarded(const void* vt, char* out, size_t cap) {
  __try {
    return RttiNameRaw(vt, out, cap);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    out[0] = 0;
    return false;
  }
}
inline std::string ClassName(uintptr_t vt) {
  if (!vt) return "none";
  char buf[200], name[128] = "";
  HMODULE mod = nullptr;
  wchar_t path[MAX_PATH] = L"?";
  if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                         reinterpret_cast<LPCWSTR>(vt), &mod) && mod)
    GetModuleFileNameW(mod, path, MAX_PATH);
  const wchar_t* file = wcsrchr(path, L'\\');
  file = file ? file + 1 : path;
  RttiNameGuarded(reinterpret_cast<const void*>(vt), name, sizeof(name));
  snprintf(buf, sizeof(buf), "%ls+0x%llx%s%s%s", file,
           static_cast<unsigned long long>(mod ? vt - reinterpret_cast<uintptr_t>(mod) : vt), name[0] ? " (" : "",
           name, name[0] ? ")" : "");
  return buf;
}
inline std::string CensusText(const ClassCensus& c) {
  std::string s;
  for (int i = 0; i < kCensus; ++i) {
    const uintptr_t v = c.vt[i].load(std::memory_order_acquire);
    if (!v) break;
    char n[32];
    snprintf(n, sizeof(n), "%llu x ", static_cast<unsigned long long>(c.n[i].load(std::memory_order_relaxed)));
    s += s.empty() ? "" : "; ";
    s += n;
    s += ClassName(v) + " / inner " + ClassName(c.inner[i].load(std::memory_order_acquire));
  }
  if (c.other.load()) s += "; more classes " + std::to_string(c.other.load());
  return s.empty() ? "none" : s;
}
ClassCensus g_census;  // this recorder's refused texture classes (cumulative)
std::atomic<uint64_t> g_classNoSets{0}, g_classRange{0};  // the other "texture class" cases: no set array, record out of range

// Recorder worker load (both recorders): the compute regions (stage A chunks,
// list recording) counted with their wall time, their thread's cycles
// (QueryThreadCycleTime: a share below 100 % of wall x TSC means the thread
// was preempted or stalled) and how many regions of both recorders ran at once.
struct WorkLoad {
  std::atomic<uint64_t> regions{0}, wallNs{0}, cycles{0}, concSum{0}, concPeak{0};
};
std::atomic<int> g_activeRegions{0};  // both recorders
WorkLoad g_load;                      // this recorder's regions
class LoadScope {
 public:
  explicit LoadScope(WorkLoad& l) : l_(l) {
    const uint64_t a = static_cast<uint64_t>(g_activeRegions.fetch_add(1, std::memory_order_relaxed) + 1);
    l_.concSum.fetch_add(a, std::memory_order_relaxed);
    uint64_t cur = l_.concPeak.load(std::memory_order_relaxed);
    while (a > cur && !l_.concPeak.compare_exchange_weak(cur, a, std::memory_order_relaxed)) {
    }
    QueryThreadCycleTime(GetCurrentThread(), &c0_);
    q0_ = defrec::Qpc();
  }
  ~LoadScope() {
    ULONG64 c1 = 0;
    QueryThreadCycleTime(GetCurrentThread(), &c1);
    l_.wallNs.fetch_add(static_cast<uint64_t>((defrec::Qpc() - q0_) * defrec::QpcToUs() * 1000.0),
                        std::memory_order_relaxed);
    l_.cycles.fetch_add(c1 - c0_, std::memory_order_relaxed);
    l_.regions.fetch_add(1, std::memory_order_relaxed);
    g_activeRegions.fetch_sub(1, std::memory_order_relaxed);
  }
  LoadScope(const LoadScope&) = delete;
  LoadScope& operator=(const LoadScope&) = delete;

 private:
  WorkLoad& l_;
  int64_t q0_ = 0;
  ULONG64 c0_ = 0;
};
struct LoadSnap {
  uint64_t regions = 0, wallNs = 0, cycles = 0, concSum = 0, concPeak = 0;
};
inline LoadSnap TakeLoad(const WorkLoad& l) {
  LoadSnap s;
  s.regions = l.regions.load(std::memory_order_relaxed);
  s.wallNs = l.wallNs.load(std::memory_order_relaxed);
  s.cycles = l.cycles.load(std::memory_order_relaxed);
  s.concSum = l.concSum.load(std::memory_order_relaxed);
  s.concPeak = l.concPeak.load(std::memory_order_relaxed);
  return s;
}
// "regions n/frame, x ms/frame, run share y %, concurrent at entry mean m (peak p since start)". tscHz: the
// TSC rate (0: share not shown).
inline std::string LoadText(const LoadSnap& a, const LoadSnap& b, double frames, double tscHz) {
  const double f = frames > 0 ? frames : 1.0;
  const uint64_t n = b.regions - a.regions;
  const double wallS = (b.wallNs - a.wallNs) * 1e-9;
  char share[32] = "n/a";
  if (tscHz > 0 && wallS > 0) snprintf(share, sizeof(share), "%.0f %%", 100.0 * (b.cycles - a.cycles) / (wallS * tscHz));
  char buf[200];
  snprintf(buf, sizeof(buf), "worker regions %.1f/frame, %.2f ms/frame, run share %s, concurrent at entry mean %.1f "
           "(peak %llu since start)",
           n / f, wallS * 1e3 / f, share, n ? static_cast<double>(b.concSum - a.concSum) / n : 0.0,
           static_cast<unsigned long long>(b.concPeak));
  return buf;
}

// [Model] ShadowRecorderScope: bits 0-3 = cascades, bit 8 = untextured
// ModelMaterialMT casters, bit 9 = textured ones.
enum : uint32_t { kScopeCascades = 0xf, kScopeUntextured = 1u << 8, kScopeTextured = 1u << 9 };
constexpr uint32_t kDefaultScope = kScopeUntextured | 1u;

inline bool InScope(uint32_t scope, int cascade) {
  return cascade >= 0 && cascade < kSlots && (scope & (1u << cascade)) != 0;
}

// Per caster: recorded, or why DCS draws it (residual).
enum CasterReason : int {
  kRecorded = 0,
  kRNotSmr,
  kRNotModel,
  kRMatTable,
  kRTextured,
  kRScope,
  kRNoShader,
  kRNoCompile,
  kRKeyPending,
  kRKeyRejected,
  kRMeshPending,
  kRMeshRejected,
  kRMeshChanged,
  kRPage,
  kRTooMany,
  kRTexMask,
  kRTexClass,
  kRTexNull,
  kRTexMissing,
  kRTexView,
  kRGroup,
  kRKeyCooling,
  kCasterReasons
};
const char* const kCasterReasonName[kCasterReasons] = {
    "recorded",
    "not a ShadowMapRenderable (terrain, other renderables)",
    "not a ModelMaterialMT caster (deck, glass, none)",
    "material table full",
    "textured (scope bit 9 off)",
    "untextured (scope bit 8 off)",
    "no DX11Shader",
    "key not compiled by shadow inst yet",
    "key not probed yet",
    "key not recordable",
    "mesh not probed yet",
    "mesh not recordable",
    "mesh differs from its probe",
    "no sbPositions view for the page",
    "too many recorded materials, pages or textures",
    "no texture-skip mask (skip off or key not compiled)",
    "a texture of an unreplicated class (or no entries)",
    "a read texture set has no texture",
    "the caster's read textures do not match its key's PS textures",
    "a texture's view not predictable or a swap due at the snapshot",
    "its batching group has a residual caster",
    "its key's state varied between draws: relearnt after a cooldown"};

// Per pass of a recorded cascade: executed, or why DCS drew all of it.
enum PassReason : int {
  kPExecuted = 0,
  kPNoJob,
  kPLate,
  kPFailed,
  kPNothing,
  kPIdentity,
  kPOversize,
  kPPage,
  kPGuard,
  kPTarget,
  kPFlags,
  kPB7,
  kPTexture,
  kPPool,
  kPNoCaster,
  kPassReasons
};
const char* const kPassReasonName[kPassReasons] = {
    "executed",
    "no job (cascade or target not learned yet, or no render entry)",
    "job still running at the pass",
    "job failed or faulted",
    "nothing recordable",
    "caster vector changed since render entry",
    "more casters than a job holds",
    "a page's sbPositions view changed",
    "a recorded material's shader-read CB dwords changed",
    "depth target, viewport or scissor differ",
    "renderer flags differ (rasterizer index or debug bits)",
    "per-view b7 missing or of another size",
    "a recorded texture's view changed or a swap is due",
    "the PS sampler pool s5-s15 differs from the learned one",
    "the exec entry did not run"};

// ---------------------------------------------------------------------------
// Static key facts: reflection of shadow_inst's (b) VS of pass 0
// ---------------------------------------------------------------------------
struct KeyStatic {
  const char* why = nullptr;  // nullptr = the VS reads only what the recorder binds
  UINT cbSlot = ~0u, sbSlot = ~0u, cbBytes = 0, psoOff = 0;
  uint64_t used[2] = {};  // def_uniforms dwords the VS reads, posStructOffset excluded
  char b7Name[32] = {};   // the per-view buffer's name (the pass's read-name check)
};

inline void MarkDwords(uint64_t m[2], uint32_t off, uint32_t size) {
  for (uint32_t d = off / 4; d < (off + size + 3) / 4 && d < 128; ++d) m[d >> 6] |= 1ull << (d & 63);
}

// The bindings and def_uniforms usage of one VS blob. cbName/psoOff: the
// material buffer's name and posStructOffset's offset in it (shadow_inst's
// VariantDiff). The (b) blob also binds qvInstOffsets at t127 and names the
// offset qvPsoBase; both are what CompareVariants allows and are ignored.
inline const char* AnalyseVs(decltype(&D3DReflect) reflect, const void* code, size_t n, const char* cbName,
                             UINT psoOff, KeyStatic& out) {
  out = KeyStatic();
  out.psoOff = psoOff;
  ID3D11ShaderReflection* r = nullptr;
  if (!reflect || !code || !cbName ||
      FAILED(reflect(code, n, __uuidof(ID3D11ShaderReflection), reinterpret_cast<void**>(&r))) || !r)
    return out.why = "D3DReflect failed";
  D3D11_SHADER_DESC d = {};
  r->GetDesc(&d);
  const char* why = nullptr;
  for (UINT i = 0; i < d.BoundResources && !why; ++i) {
    D3D11_SHADER_INPUT_BIND_DESC b = {};
    r->GetResourceBindingDesc(i, &b);
    const bool one = b.BindCount == 1;
    if (b.Type == D3D_SIT_CBUFFER && strcmp(b.Name, cbName) == 0 && one) {
      out.cbSlot = b.BindPoint;
    } else if (b.Type == D3D_SIT_CBUFFER && b.BindPoint == kB7 && one) {
      strncpy_s(out.b7Name, b.Name, _TRUNCATE);  // the per-view buffer: our late copy of DCS's b7
    } else if (b.Type == D3D_SIT_STRUCTURED && strcmp(b.Name, "sbPositions") == 0 && one) {
      out.sbSlot = b.BindPoint;
    } else if (b.Type == D3D_SIT_STRUCTURED && strcmp(b.Name, "qvInstOffsets") == 0 && b.BindPoint == 127) {
      // (b) only
    } else {
      why = b.Type == D3D_SIT_CBUFFER ? "the VS reads another constant buffer"
                                      : "the VS reads a texture, sampler or another buffer";
    }
  }
  if (!why && out.cbSlot == ~0u) why = "the VS reads no def_uniforms";
  if (!why && out.sbSlot == ~0u) why = "the VS reads no sbPositions";
  if (!why && out.cbSlot == kB7) why = "def_uniforms is bound at b7";
  if (!why && psoOff != kPsoCb) why = "posStructOffset is not at def_uniforms +0xfc";
  if (!why) {
    ID3D11ShaderReflectionConstantBuffer* cb = r->GetConstantBufferByName(cbName);
    D3D11_SHADER_BUFFER_DESC bd = {};
    if (!cb || FAILED(cb->GetDesc(&bd))) {
      why = "def_uniforms has no reflection";
    } else {
      out.cbBytes = bd.Size;
      if (bd.Size != kCbLen) why = "def_uniforms is not 0x130 bytes";
      for (UINT v = 0; v < bd.Variables && !why; ++v) {
        D3D11_SHADER_VARIABLE_DESC vd = {};
        if (SUCCEEDED(cb->GetVariableByIndex(v)->GetDesc(&vd)) && (vd.uFlags & D3D_SVF_USED))
          MarkDwords(out.used, vd.StartOffset, vd.Size);
      }
    }
  }
  out.used[(kPsoCb / 4) >> 6] &= ~(1ull << ((kPsoCb / 4) & 63));
  r->Release();
  return out.why = why;
}

// Every def_uniforms dword but posStructOffset: the guard of keys with a PS
// (its reads are not reflected: DCS's live effect keeps no bytecode, and
// shadow_inst keeps only the VS of (b)).
inline void AllCbDwords(uint64_t m[2]) {
  m[0] = m[1] = 0;
  MarkDwords(m, 0, kCbLen);
  m[(kPsoCb / 4) >> 6] &= ~(1ull << ((kPsoCb / 4) & 63));
}

// ---------------------------------------------------------------------------
// Key and mesh tables (inserts on the render thread, lock-free lookups on
// the workers; entries are published with a release store of their key and
// never removed before unload)
// ---------------------------------------------------------------------------
struct KeyEntry {
  std::atomic<void*> shader{nullptr};
  uint64_t tech = 0;
  uint32_t flags = 0;          // renderer+0xd4 (ApplyPassBlock's rasterizer index [V 0x67f90])
  void* effect = nullptr;      // fingerprint of the DX11Shader ([+0x50], [+0xb0]) at the probe
  void* techBegin = nullptr;
  std::atomic<int> state{0};   // 1 = recordable, -1 = not (why); 1 can later drop to -1
  const char* why = nullptr;
  KeyStatic st;
  uint64_t guard[2] = {};      // CB dwords compared at the pass
  // From DCS's draw (references held while recordable).
  ID3D11VertexShader* vs = nullptr;
  ID3D11DepthStencilState* dss = nullptr;
  UINT stencilRef = 0;
  ID3D11BlendState* bs = nullptr;
  float blendFactor[4] = {};
  UINT sampleMask = 0;
  ID3D11RasterizerState* rs = nullptr;
  // Alpha-tested keys: the PS, its samplers (every bound slot), the PS slots
  // holding the material's CB, and per read texture handle its PS slot.
  ID3D11PixelShader* ps = nullptr;
  ID3D11SamplerState* psSamp[kSampSlots] = {};
  UINT psSampCount = 0;  // highest bound slot + 1
  uint32_t psCbMask = 0;
  int psTexCount = 0;
  int64_t psTexH[kMaxPsTex] = {};
  UINT psTexSlot[kMaxPsTex] = {};
  uint32_t psSampDeps = 0xffff;  // PS sampler slots FX Apply sets (all 16 when the dependencies were not read)
  uint32_t retiredAt = 0;        // render entry + 1 when retired (state kRetired)
};

// A key whose bound state differed between two probes (after the slots the
// shaders do not read were left out): kept for the references, skipped by
// lookups, relearnt by a new probe after kKeyCooldown render entries.
constexpr int kRetired = -2;
constexpr uint32_t kKeyCooldown = 900;

struct MeshEntry {
  std::atomic<void*> mesh{nullptr};
  void* shader = nullptr;
  uint64_t tech = 0;
  void* effect = nullptr;  // the input layout depends on the shader's VS: same fingerprint as KeyEntry
  void* techBegin = nullptr;
  std::atomic<int> state{0};
  const char* why = nullptr;
  uint64_t fp = 0;                  // MeshLive::fp at the probe
  ID3D11InputLayout* il = nullptr;  // reference held
  ID3D11Buffer* vb = nullptr;       // not held: equal to the live mesh's at every use
  UINT stride = 0;
  ID3D11Buffer* ib = nullptr;
  DXGI_FORMAT ibFormat = DXGI_FORMAT_UNKNOWN;
  UINT indexCount = 0;
};

struct Tables {
  KeyEntry* keys = nullptr;      // kKeyTable
  MeshEntry* meshes = nullptr;   // kMeshTable
  uint32_t keysUsed = 0, meshesUsed = 0;  // inserting thread
};

inline size_t Mix(uint64_t h) {
  h ^= h >> 31;
  h *= 0xBF58476D1CE4E5B9ull;
  h ^= h >> 29;
  return static_cast<size_t>(h);
}
inline size_t KeyHash(const void* s, uint64_t tech, uint32_t flags) {
  return Mix(reinterpret_cast<uintptr_t>(s) * 0x9E3779B97F4A7C15ull ^ tech * 0xC2B2AE3D27D4EB4Full ^ flags);
}
inline size_t MeshHash(const void* m, const void* s, uint64_t tech) {
  return Mix(reinterpret_cast<uintptr_t>(m) * 0x9E3779B97F4A7C15ull ^ reinterpret_cast<uintptr_t>(s) * 0xC2B2AE3D27D4EB4Full ^
             tech * 0x165667B19E3779F9ull);
}

const KeyEntry* FindKey(const Tables& t, void* shader, uint64_t tech, uint32_t flags, void* effect, void* techBegin) {
  if (!t.keys || !shader) return nullptr;
  size_t i = KeyHash(shader, tech, flags) & (kKeyTable - 1);
  for (size_t p = 0; p < kKeyTable; ++p, i = (i + 1) & (kKeyTable - 1)) {
    const KeyEntry& e = t.keys[i];
    void* k = e.shader.load(std::memory_order_acquire);
    if (!k) return nullptr;
    if (k == shader && e.tech == tech && e.flags == flags && e.effect == effect && e.techBegin == techBegin &&
        e.state.load(std::memory_order_acquire) != kRetired)
      return &e;
  }
  return nullptr;
}

// A retired entry of the key exists (anyTime) or was retired less than
// kKeyCooldown render entries before `now`.
bool KeyCooling(const Tables& t, void* shader, uint64_t tech, uint32_t flags, void* effect, void* techBegin,
                uint32_t now, bool anyTime) {
  if (!t.keys || !shader) return false;
  size_t i = KeyHash(shader, tech, flags) & (kKeyTable - 1);
  for (size_t p = 0; p < kKeyTable; ++p, i = (i + 1) & (kKeyTable - 1)) {
    const KeyEntry& e = t.keys[i];
    void* k = e.shader.load(std::memory_order_acquire);
    if (!k) return false;
    if (k == shader && e.tech == tech && e.flags == flags && e.effect == effect && e.techBegin == techBegin &&
        e.state.load(std::memory_order_acquire) == kRetired && (anyTime || now + 1 - e.retiredAt < kKeyCooldown))
      return true;
  }
  return false;
}

// A free slot on the key's probe chain (fill it, then PublishKey), or nullptr
// when the table is 3/4 full. Inserting thread only.
KeyEntry* NewKey(Tables& t, void* shader, uint64_t tech, uint32_t flags) {
  if (!t.keys || t.keysUsed >= kKeyTable * 3 / 4) return nullptr;
  size_t i = KeyHash(shader, tech, flags) & (kKeyTable - 1);
  for (size_t p = 0; p < kKeyTable; ++p, i = (i + 1) & (kKeyTable - 1))
    if (!t.keys[i].shader.load(std::memory_order_relaxed)) return &t.keys[i];
  return nullptr;
}
void PublishKey(Tables& t, KeyEntry& e, void* shader) {
  ++t.keysUsed;
  e.shader.store(shader, std::memory_order_release);
}

const MeshEntry* FindMesh(const Tables& t, void* mesh, void* shader, uint64_t tech, void* effect, void* techBegin) {
  if (!t.meshes || !mesh) return nullptr;
  size_t i = MeshHash(mesh, shader, tech) & (kMeshTable - 1);
  for (size_t p = 0; p < kMeshTable; ++p, i = (i + 1) & (kMeshTable - 1)) {
    const MeshEntry& e = t.meshes[i];
    void* k = e.mesh.load(std::memory_order_acquire);
    if (!k) return nullptr;
    if (k == mesh && e.shader == shader && e.tech == tech && e.effect == effect && e.techBegin == techBegin) return &e;
  }
  return nullptr;
}
MeshEntry* NewMesh(Tables& t, void* mesh, void* shader, uint64_t tech) {
  if (!t.meshes || t.meshesUsed >= kMeshTable * 3 / 4) return nullptr;
  size_t i = MeshHash(mesh, shader, tech) & (kMeshTable - 1);
  for (size_t p = 0; p < kMeshTable; ++p, i = (i + 1) & (kMeshTable - 1))
    if (!t.meshes[i].mesh.load(std::memory_order_relaxed)) return &t.meshes[i];
  return nullptr;
}
void PublishMesh(Tables& t, MeshEntry& e, void* mesh) {
  ++t.meshesUsed;
  e.mesh.store(mesh, std::memory_order_release);
}

template <class T>
void SafeRel(T*& p) {
  if (p) p->Release();
  p = nullptr;
}

bool AllocTables(Tables& t) {
  if (!t.keys) t.keys = new (std::nothrow) KeyEntry[kKeyTable];
  if (!t.meshes) t.meshes = new (std::nothrow) MeshEntry[kMeshTable];
  return t.keys && t.meshes;
}

// Releases every held reference and frees the tables (no reader may run).
void FreeTables(Tables& t) {
  if (t.keys)
    for (size_t i = 0; i < kKeyTable; ++i) {
      KeyEntry& e = t.keys[i];
      SafeRel(e.vs), SafeRel(e.dss), SafeRel(e.bs), SafeRel(e.rs), SafeRel(e.ps);
      for (auto*& s : e.psSamp) SafeRel(s);
    }
  if (t.meshes)
    for (size_t i = 0; i < kMeshTable; ++i) SafeRel(t.meshes[i].il);
  delete[] t.keys;
  delete[] t.meshes;
  t = Tables();
}

// ---------------------------------------------------------------------------
// Mesh fields
// ---------------------------------------------------------------------------
// What DCS's shadow draw reads of a mesh [V NGModel submit 0x13244..0x132e8,
// dx11backend vt[38] 0x1dbc0, setVertexBuffers 0x1ad10, draw 0x14bc4..]:
//   [mesh+0]     index buffer object: ID3D11Buffer at +0x28, index size +0x3c
//                (4: R32_UINT, else R16_UINT); none: DCS uses Draw;
//   [mesh+0x18]  vertex buffer object: ID3D11Buffer +0x130, stride +0x150,
//                element format words +0x2c[k] and +0xac[k];
//   +0x208       element count; element i: shader-table index (dword at
//                +0x58 + 8i) and buffer element k (dword at +0x130 + 8i):
//                the descriptor vt[38] packs, from which 0x43c30 picks the
//                input layout (one vertex buffer: slot 0, offset 0);
//   +0x20c       primitive type, +0x210 primitive count.
// fp hashes every one of them, so an equal fp with equal buffer pointers
// means the probed input layout, buffers and index count still apply.
struct MeshLive {
  void* ibObj = nullptr;
  ID3D11Buffer* ib = nullptr;
  DXGI_FORMAT ibFormat = DXGI_FORMAT_UNKNOWN;
  void* vbObj = nullptr;
  ID3D11Buffer* vb = nullptr;
  UINT stride = 0;
  uint32_t elems = 0, prim = 0, count = 0;
  uint64_t fp = 0;
};

inline uint64_t Fnv8(uint64_t h, uint64_t v) {
  for (int i = 0; i < 8; ++i) {
    h ^= (v >> (8 * i)) & 0xff;
    h *= 0x100000001b3ull;
  }
  return h;
}

// nullptr, or why DCS's draw of this mesh is not recordable. Plain (callers
// hold SEH).
inline const char* ReadMesh(const uint8_t* mesh, MeshLive& m) {
  m = MeshLive();
  if (!mesh) return "no mesh";
  m.ibObj = *reinterpret_cast<void* const*>(mesh);
  m.vbObj = *reinterpret_cast<void* const*>(mesh + 0x18);
  m.elems = *reinterpret_cast<const uint32_t*>(mesh + 0x208);
  m.prim = *reinterpret_cast<const uint32_t*>(mesh + 0x20c);
  m.count = *reinterpret_cast<const uint32_t*>(mesh + 0x210);
  if (!m.ibObj) return "not indexed";
  if (!m.vbObj) return "no vertex buffer object";
  const auto* ib = static_cast<const uint8_t*>(m.ibObj);
  const auto* vb = static_cast<const uint8_t*>(m.vbObj);
  m.ib = *reinterpret_cast<ID3D11Buffer* const*>(ib + 0x28);
  const uint32_t isz = *reinterpret_cast<const uint32_t*>(ib + 0x3c);
  m.ibFormat = isz == 4 ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_R16_UINT;
  m.vb = *reinterpret_cast<ID3D11Buffer* const*>(vb + 0x130);
  m.stride = *reinterpret_cast<const UINT*>(vb + 0x150);
  if (!m.ib || !m.vb) return "no index or vertex buffer";
  if (m.prim != kPrimTriList) return "not a triangle list";
  if (m.count == 0 || m.count > 0x15555555u) return "primitive count out of range";
  // The two element arrays (stride 8) end where +0x208 begins: 27 elements.
  if (m.elems == 0 || m.elems > 27) return "vertex element count out of range";
  uint64_t h = 0xcbf29ce484222325ull;
  h = Fnv8(h, reinterpret_cast<uintptr_t>(m.ibObj));
  h = Fnv8(h, reinterpret_cast<uintptr_t>(m.ib));
  h = Fnv8(h, isz);
  h = Fnv8(h, reinterpret_cast<uintptr_t>(m.vbObj));
  h = Fnv8(h, reinterpret_cast<uintptr_t>(m.vb));
  h = Fnv8(h, m.stride | static_cast<uint64_t>(m.elems) << 32);
  h = Fnv8(h, m.prim | static_cast<uint64_t>(m.count) << 32);
  for (uint32_t i = 0; i < m.elems; ++i) {
    const uint32_t a = *reinterpret_cast<const uint32_t*>(mesh + 0x58 + 8 * i);
    const int32_t k = *reinterpret_cast<const int32_t*>(mesh + 0x130 + 8 * i);
    if (k < 0 || k >= 32) return "vertex element index out of range";
    h = Fnv8(h, a | static_cast<uint64_t>(static_cast<uint32_t>(k)) << 32);
    h = Fnv8(h, *reinterpret_cast<const uint32_t*>(vb + 0x2c + 4 * k) |
                    static_cast<uint64_t>(*reinterpret_cast<const uint32_t*>(vb + 0xac + 4 * k)) << 32);
  }
  m.fp = h;
  return nullptr;
}

const char* ReadMeshGuarded(const uint8_t* mesh, MeshLive& m) {
  __try {
    return ReadMesh(mesh, m);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return "mesh not readable";
  }
}

// ---------------------------------------------------------------------------
// Textures (S4)
// ---------------------------------------------------------------------------
// DX11Texture with an inner class whose vt[18]/getDesc need no replay
// (shadow_tex.h exactness part 2): the classes slot 26 and SkipSet handle
// without side effects but the streaming request.
inline bool InnerOk(const gbbatch::TexEnv& env, const void* iv);  // below (texture table)
inline bool TexClassOk(const gbbatch::TexEnv& env, const uint8_t* tex) {
  if (*reinterpret_cast<void* const*>(tex) != env.texVtbl) return false;
  const void* inner = *reinterpret_cast<void* const*>(tex + 0x10);
  const void* iv = inner ? *static_cast<void* const*>(inner) : nullptr;
  return InnerOk(env, iv);
}

// getSRV would swap the streamed mip sets (SkipSet's condition, shadow_tex.h).
inline bool SwapDue(const uint8_t* tex, int64_t aux) {
  if (aux != -1 || *reinterpret_cast<void* const*>(tex + 0x190)) return false;
  const uint8_t* back = *reinterpret_cast<uint8_t* const*>(tex + 0x1a0);
  return !back || gbbatch::SetReady(back);
}

using Tex23Fn = uint64_t(__fastcall*)(void* tex, uint64_t packedSize);

// ---------------------------------------------------------------------------
// The render-target inner class (DX11TextureInternalImpl, dx11backend
// 0xb28d8) [V dx11backend 2.9.30]: getDesc's inner vt[4] 0x111a0 is a plain
// getter (mov eax, [rcx+0x50]); slot 26's tex vt[18] -> inner vt[8] 0x33f70
// generates the mips once after the target was drawn to: if !byte [inner+0x54]
// and the texture's flags [[inner+8]+0x44] & 0x10, immediate context
// GenerateMips([tex+0x190]) and the byte set to 1. Its view is [tex+0x190]
// (getSRV's +0x190 path). Supported by replaying vt[18] on the render thread
// at the exec entry, after the checks and before the list: the draws before
// the texture's first use in the list do not use it, so the mips are the ones
// that first use would have generated [I].
// ---------------------------------------------------------------------------
constexpr uint32_t kInnerRtVtbl = 0xb28d8;
constexpr uint8_t kInnerRtSlot4[] = {0x8b, 0x41, 0x50, 0xc3};
constexpr uint8_t kInnerRtSlot8[] = {
    0x40, 0x53, 0x48, 0x83, 0xec, 0x20, 0x80, 0x79, 0x54, 0x00, 0x48, 0x8b, 0xd9, 0x75, 0x56, 0x48, 0x8b, 0x41,
    0x08, 0xf6, 0x40, 0x44, 0x10, 0x74, 0x4c, 0x48, 0x83, 0xb8, 0x88, 0x01, 0x00, 0x00, 0x00, 0x75, 0x18, 0x4c,
    0x8d, 0x05, 0x66, 0x1c, 0x08, 0x00, 0xba, 0x08, 0x00, 0x00, 0x00, 0x48, 0x8d, 0x0d, 0xda, 0xca, 0x07, 0x00,
    0xe8, 0x85, 0x0f, 0xfd, 0xff, 0x48, 0x8b, 0x53, 0x08, 0x48, 0x8b, 0x82, 0x88, 0x01, 0x00, 0x00, 0x48, 0x8b,
    0x92, 0x90, 0x01, 0x00, 0x00, 0x48, 0x8b, 0x88, 0xf8, 0x00, 0x00, 0x00, 0x48, 0x8b, 0x49, 0x30, 0x48, 0x8b,
    0x01, 0xff, 0x90, 0xb0, 0x01, 0x00, 0x00, 0xc6, 0x43, 0x54, 0x01, 0x48, 0x83, 0xc4, 0x20, 0x5b, 0xc3};
const void* g_innerRt = nullptr;  // its vtable once verified (both recorders' installs)

// Finds and verifies the class in dx11backend (base dx): RTTI name, slot 4
// and slot 8 code. False: not supported (refused as before).
bool InitInnerRt(uint8_t* dx) {
  if (g_innerRt) return true;
  if (!dx) return false;
  void** vt = reinterpret_cast<void**>(dx + kInnerRtVtbl);
  if (!allocslab::RttiIs(dx, vt, ".?AVDX11TextureInternalImpl@RenderAPI@@")) return false;
  void* s4 = nullptr;
  void* s8 = nullptr;
  uint8_t b4[sizeof(kInnerRtSlot4)], b8[sizeof(kInnerRtSlot8)];
  if (!allocslab::ReadBytes(vt + 4, &s4, sizeof(s4)) || !allocslab::ReadBytes(vt + 8, &s8, sizeof(s8)) || !s4 || !s8 ||
      !allocslab::ReadBytes(s4, b4, sizeof(b4)) || !allocslab::ReadBytes(s8, b8, sizeof(b8)) ||
      memcmp(b4, kInnerRtSlot4, sizeof(b4)) != 0 || memcmp(b8, kInnerRtSlot8, sizeof(b8)) != 0)
    return false;
  g_innerRt = vt;
  return true;
}

// The replicated inner classes: file, array, dummy, and the render target once verified.
inline bool InnerOk(const gbbatch::TexEnv& env, const void* iv) {
  return iv && (iv == env.inner[0] || iv == env.inner[1] || iv == env.inner[2] || (g_innerRt && iv == g_innerRt));
}

// Slot 26's vt[18] for a texture (render thread; plain: callers hold SEH).
using Tex18Fn = void(__fastcall*)(void* tex);
inline void ReplayVt18(uint8_t* tex) { (*reinterpret_cast<Tex18Fn**>(tex))[18](tex); }

// ---------------------------------------------------------------------------
// Texture table (R18 3.3): DCS's getSRV inputs per (texture, aux, type)
// (both recorders: each has its own table and lock)
// ---------------------------------------------------------------------------
// getSRV 0x47c60 and slot 26 0x1fd70 [V, gb_batch.h PredictView]: compat
// false -> null view; [tex+0x678] set: type 0x20/0x21 use size 0, others are
// not replicated; aux != -1 -> [[tex+0x90] + aux*24 + 0x10]; [tex+0x190] set
// -> it; P = [tex+0x1a0] ready -> swap (not replicated: unusable now); S =
// [tex+0x198] not ready -> [tex+0x1b8] ?: [tex+0x80]; else the first view i
// with area >= thr[i] (area = int32(size.x * size.y)), views[0] when none.
constexpr int kTexViews = 14;
enum : uint8_t { kTeEmpty = 0, kTeLive = 1, kTeTomb = 2 };
enum : uint8_t { kTmFixed = 0, kTmAreas = 1 };
enum TexWhy : uint8_t { kTwOk = 0, kTwClass, kTw678, kTwMipSet, kTwSwap, kTwFault, kTwCount };
const char* const kTexWhyName[kTwCount] = {"usable", "texture class", "tex+0x678 path", "mip-set shape",
                                           "swap due", "fault"};

struct TexEntry {
  std::atomic<uint8_t> slot{kTeEmpty};
  uint8_t usable = 0, mode = kTmFixed, t678 = 0, compatNull = 0, sReady = 0, nviews = 0, nthr = 0;
  uint8_t rt = 0;  // render-target inner class: slot 26's vt[18] may generate mips (replayed at exec)
  uint8_t why = kTwOk;
  std::atomic<uint8_t> dirty{0};
  std::atomic<uint32_t> lastUse{0};
  // Bumped by every rebuild, eviction and wipe: a job's lists are valid only
  // for the versions it recorded with (another slot's maintenance may rebuild
  // an entry between that job's recording and its pass).
  std::atomic<uint32_t> version{0};
  uint8_t* tex = nullptr;
  int64_t aux = -1;
  int32_t type = 0;
  // What the views were derived from (TexCheck compares them with the live texture).
  void* gVt = nullptr;
  void* gInnerVt = nullptr;
  void* g678 = nullptr;
  void* gAux = nullptr;
  void* g190 = nullptr;
  uint8_t* gS = nullptr;
  uint8_t* gP = nullptr;
  void* g1b8 = nullptr;
  void* g80 = nullptr;
  void* gVb = nullptr;
  void* gVe = nullptr;
  void* gTb = nullptr;
  void* gTe = nullptr;
  ID3D11ShaderResourceView* views[kTexViews] = {};  // one reference each (null allowed)
  int32_t thr[kTexViews] = {};
};

struct TexTable {
  TexEntry* e = nullptr;  // kTexTable
  uint32_t size = 0;
  uint32_t live = 0, tomb = 0, cursor = 0;
  uint64_t built = 0, refreshed = 0, evicted = 0, wipes = 0;
};

inline size_t TexHash(const uint8_t* tex, int64_t aux, int32_t type) {
  return Mix(reinterpret_cast<uintptr_t>(tex) * 0x9E3779B97F4A7C15ull ^ static_cast<uint64_t>(aux) * 0xC2B2AE3D27D4EB4Full ^
             static_cast<uint64_t>(static_cast<uint32_t>(type)) * 0x165667B19E3779F9ull);
}

// Lock-free for readers that hold the table's lock shared.
const TexEntry* TexFind(const TexTable& t, const uint8_t* tex, int64_t aux, int32_t type) {
  if (!t.e) return nullptr;
  size_t i = TexHash(tex, aux, type) & (t.size - 1);
  for (uint32_t p = 0; p < t.size; ++p, i = (i + 1) & (t.size - 1)) {
    const TexEntry& e = t.e[i];
    const uint8_t s = e.slot.load(std::memory_order_acquire);
    if (s == kTeEmpty) return nullptr;
    if (s == kTeLive && e.tex == tex && e.aux == aux && e.type == type) return &e;
  }
  return nullptr;
}

void TexReleaseViews(TexEntry& e) {
  for (auto*& v : e.views) SafeRel(v);
  e.nviews = e.nthr = 0;
  e.version.fetch_add(1, std::memory_order_relaxed);
}

// The entry of (tex, aux, type), inserted when absent (exclusive holder only);
// nullptr when the table is full.
TexEntry* TexInsert(TexTable& t, uint8_t* tex, int64_t aux, int32_t type, bool* fresh) {
  if (!t.e) return nullptr;
  *fresh = false;
  size_t i = TexHash(tex, aux, type) & (t.size - 1);
  TexEntry* tomb = nullptr;
  for (uint32_t p = 0; p < t.size; ++p, i = (i + 1) & (t.size - 1)) {
    TexEntry& e = t.e[i];
    const uint8_t s = e.slot.load(std::memory_order_relaxed);
    if (s == kTeLive && e.tex == tex && e.aux == aux && e.type == type) return &e;
    if (s == kTeTomb && !tomb) tomb = &e;
    if (s == kTeEmpty) {
      if (t.live + t.tomb >= t.size * 3 / 4 && !tomb) return nullptr;
      TexEntry* d = tomb ? tomb : &e;
      if (tomb) --t.tomb;
      d->tex = tex;
      d->aux = aux;
      d->type = type;
      d->usable = 0;
      d->dirty.store(1, std::memory_order_relaxed);
      d->lastUse.store(0, std::memory_order_relaxed);
      d->slot.store(kTeLive, std::memory_order_release);
      ++t.live;
      *fresh = true;
      return d;
    }
  }
  return nullptr;
}

// DCS's getSRV inputs read into e (render thread; plain: callers hold SEH).
// Views get one reference each. kTwOk when the worker may pick from it.
uint8_t TexBuildRaw(const gbbatch::TexEnv& env, TexEntry& e, ClassCensus* census = nullptr) {
  TexReleaseViews(e);
  e.usable = 0;
  e.mode = kTmFixed;
  e.t678 = e.compatNull = e.sReady = e.rt = 0;
  e.g678 = e.gAux = e.g190 = e.g1b8 = e.g80 = e.gVb = e.gVe = e.gTb = e.gTe = nullptr;
  e.gS = e.gP = nullptr;
  uint8_t* tex = e.tex;
  e.gVt = *reinterpret_cast<void**>(tex);
  if (e.gVt != env.texVtbl) {  // another class: its +0x10 is not read
    if (census) CensusAdd(*census, e.gVt, nullptr);
    return e.why = kTwClass;
  }
  void* inner = *reinterpret_cast<void**>(tex + 0x10);
  e.gInnerVt = inner ? *static_cast<void**>(inner) : nullptr;
  if (!InnerOk(env, e.gInnerVt)) {
    if (census) CensusAdd(*census, e.gVt, e.gInnerVt);
    return e.why = kTwClass;
  }
  e.rt = e.gInnerVt == g_innerRt ? 1 : 0;
  e.g678 = *reinterpret_cast<void**>(tex + 0x678);
  const uint8_t* desc = env.getDesc(tex);
  if (!env.compat(e.type, *reinterpret_cast<const int32_t*>(desc + 0x20))) {
    e.compatNull = 1;
    e.usable = 1;
    return e.why = kTwOk;  // slot 26: SetResource(0)
  }
  if (e.g678) {
    if (e.type != 0x20 && e.type != 0x21) return e.why = kTw678;
    e.t678 = 1;
  }
  ID3D11ShaderResourceView* v0 = nullptr;
  if (e.aux != -1) {
    e.gAux = *reinterpret_cast<void**>(tex + 0x90);
    v0 = *reinterpret_cast<ID3D11ShaderResourceView* const*>(static_cast<uint8_t*>(e.gAux) + e.aux * 24 + 0x10);
  } else if ((e.g190 = *reinterpret_cast<void**>(tex + 0x190)) != nullptr) {
    v0 = static_cast<ID3D11ShaderResourceView*>(e.g190);
  } else {
    e.gS = *reinterpret_cast<uint8_t**>(tex + 0x198);
    e.gP = *reinterpret_cast<uint8_t**>(tex + 0x1a0);
    if (!e.gS || !e.gP) return e.why = kTwMipSet;
    if (gbbatch::SetReady(e.gP)) return e.why = kTwSwap;
    e.sReady = gbbatch::SetReady(e.gS) ? 1 : 0;
    if (!e.sReady) {
      e.g1b8 = *reinterpret_cast<void**>(tex + 0x1b8);
      e.g80 = *reinterpret_cast<void**>(tex + 0x80);
      v0 = static_cast<ID3D11ShaderResourceView*>(e.g1b8 ? e.g1b8 : e.g80);
    } else {
      e.gVb = *reinterpret_cast<void**>(e.gS + 8);
      e.gVe = *reinterpret_cast<void**>(e.gS + 0x10);
      e.gTb = *reinterpret_cast<void**>(e.gS + 0x28);
      e.gTe = *reinterpret_cast<void**>(e.gS + 0x30);
      const intptr_t nv = (static_cast<uint8_t*>(e.gVe) - static_cast<uint8_t*>(e.gVb)) / 8;
      const intptr_t nt = (static_cast<uint8_t*>(e.gTe) - static_cast<uint8_t*>(e.gTb)) / 4;
      if (nv == 0) {
        v0 = nullptr;  // empty view vector: getSRV returns null
      } else {
        if (nv < 0 || nt < 0 || nt > 256 || nv > kTexViews || nt > kTexViews) return e.why = kTwMipSet;
        e.mode = kTmAreas;
        e.nviews = static_cast<uint8_t>(nv);
        e.nthr = static_cast<uint8_t>(nt);
        // A view enters e.views only after its AddRef: a fault in the copies
        // or an AddRef used to leave unreferenced (or garbage) pointers in
        // e.views, which TexReleaseViews then Released, dropping DCS's own
        // reference (use after free in DCS) or faulting on the render thread.
        ID3D11ShaderResourceView* src[kTexViews];
        memcpy(src, e.gVb, static_cast<size_t>(nv) * 8);
        memcpy(e.thr, e.gTb, static_cast<size_t>(nt) * 4);
        for (int k = 0; k < e.nviews; ++k) {
          if (src[k]) src[k]->AddRef();
          e.views[k] = src[k];
        }
        e.usable = 1;
        return e.why = kTwOk;
      }
    }
  }
  e.nviews = 1;
  if (v0) v0->AddRef();  // before it enters e.views (see the multi-view case)
  e.views[0] = v0;
  e.usable = 1;
  return e.why = kTwOk;
}

uint8_t TexBuildGuarded(const gbbatch::TexEnv& env, TexEntry& e, ClassCensus* census = nullptr) {
  __try {
    return TexBuildRaw(env, e, census);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    // Only views already referenced are in e.views: released with the entry.
    e.usable = 0;
    return e.why = kTwFault;
  }
}

// The live texture still gives the entry's views (render thread, at the
// exec entry, after the streaming replay). Plain (callers hold SEH).
bool TexCheckRaw(const gbbatch::TexEnv& env, const TexEntry& e) {
  if (!e.usable) return false;
  const uint8_t* tex = e.tex;
  if (*reinterpret_cast<void* const*>(tex) != e.gVt) return false;
  const void* inner = *reinterpret_cast<void* const*>(tex + 0x10);
  if (!inner || *static_cast<void* const*>(inner) != e.gInnerVt) return false;
  (void)env;
  if (*reinterpret_cast<void* const*>(tex + 0x678) != e.g678) return false;
  if (e.compatNull) return true;
  if (e.aux != -1) {
    void* a = *reinterpret_cast<void* const*>(tex + 0x90);
    return a == e.gAux &&
           *reinterpret_cast<void* const*>(static_cast<uint8_t*>(a) + e.aux * 24 + 0x10) == e.views[0];
  }
  void* v190 = *reinterpret_cast<void* const*>(tex + 0x190);
  if (v190 != e.g190) return false;
  if (e.g190) return true;
  const uint8_t* s = *reinterpret_cast<uint8_t* const*>(tex + 0x198);
  const uint8_t* p = *reinterpret_cast<uint8_t* const*>(tex + 0x1a0);
  if (s != e.gS || p != e.gP || gbbatch::SetReady(p)) return false;
  const uint8_t ready = gbbatch::SetReady(s) ? 1 : 0;
  if (ready != e.sReady) return false;
  if (!ready) {
    void* a = *reinterpret_cast<void* const*>(tex + 0x1b8);
    void* b = *reinterpret_cast<void* const*>(tex + 0x80);
    return a == e.g1b8 && b == e.g80;
  }
  if (*reinterpret_cast<void* const*>(s + 8) != e.gVb || *reinterpret_cast<void* const*>(s + 0x10) != e.gVe ||
      *reinterpret_cast<void* const*>(s + 0x28) != e.gTb || *reinterpret_cast<void* const*>(s + 0x30) != e.gTe)
    return false;
  if (e.mode != kTmAreas) return e.gVb == e.gVe;
  return memcmp(e.gVb, e.views, static_cast<size_t>(e.nviews) * 8) == 0 &&
         memcmp(e.gTb, e.thr, static_cast<size_t>(e.nthr) * 4) == 0;
}

// getSRV's choice for this size from the entry (pure). False: the view index
// is outside the view vector (DCS would read past it).
inline bool TexPick(const TexEntry& e, uint64_t size, ID3D11ShaderResourceView** v) {
  if (e.compatNull) {
    *v = nullptr;
    return true;
  }
  if (e.mode == kTmFixed) {
    *v = e.views[0];
    return true;
  }
  const uint64_t sz = e.t678 ? 0 : size;
  const int32_t area = static_cast<int32_t>(static_cast<uint32_t>(sz) * static_cast<uint32_t>(sz >> 32));
  for (int i = 0; i < e.nthr; ++i)
    if (area >= e.thr[i]) {
      if (i >= e.nviews) return false;
      *v = e.views[i];
      return true;
    }
  *v = e.views[0];
  return true;
}

// A swap would be due now (worker, read only; the exec check decides).
inline bool TexSwapDueNow(const TexEntry& e) {
  return e.gP && !e.compatNull && e.aux == -1 && !e.g190 && gbbatch::SetReady(e.gP);
}

bool TexSwapDueGuarded(const TexEntry& e) {
  __try {
    return TexSwapDueNow(e);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return true;
  }
}

void TexWipe(TexTable& t) {
  if (!t.e) return;
  for (uint32_t i = 0; i < t.size; ++i) {
    TexEntry& e = t.e[i];
    if (e.slot.load(std::memory_order_relaxed) == kTeLive) TexReleaseViews(e);
    e.slot.store(kTeEmpty, std::memory_order_relaxed);
  }
  t.live = t.tomb = t.cursor = 0;
  ++t.wipes;
}

// Evicts entries unused since gen - age, `count` slots per call (exclusive holder).
void TexEvict(TexTable& t, uint32_t gen, uint32_t age, uint32_t count) {
  if (!t.e || !t.live) return;
  for (uint32_t k = 0; k < count; ++k) {
    TexEntry& e = t.e[t.cursor];
    t.cursor = (t.cursor + 1) & (t.size - 1);
    if (e.slot.load(std::memory_order_relaxed) != kTeLive) continue;
    const uint32_t used = e.lastUse.load(std::memory_order_relaxed);
    if (gen - used <= age) continue;
    TexReleaseViews(e);
    e.slot.store(kTeTomb, std::memory_order_relaxed);
    --t.live;
    ++t.tomb;
    ++t.evicted;
  }
}

bool AllocTex(TexTable& t, uint32_t size) {
  if (t.e) return true;
  t.e = new (std::nothrow) TexEntry[size];
  t.size = t.e ? size : 0;
  return t.e != nullptr;
}

void FreeTex(TexTable& t) {
  TexWipe(t);
  delete[] t.e;
  t = TexTable();
}

// ---------------------------------------------------------------------------
// Environment of a job (DCS's classes and globals; fakes in the tests)
// ---------------------------------------------------------------------------
struct Env {
  void* smrVt = nullptr;     // NGModel ShadowMapRenderable
  void* modelVt = nullptr;   // NGModel ModelMaterialMT
  void* shaderVt = nullptr;  // dx11backend DX11Shader
  uint8_t* const* globals = nullptr;          // &model::globals (GlobalsMT*)
  bool (*compiled)(void* shader, uint64_t tech) = nullptr;  // shadow_inst has the key's compile (pass 0)
  // The texture-skip mask slot 5 uses for this material's shader now
  // (shadow_tex.h's lock-free cache, only while the skip runs), or nullptr.
  const shadowtex::MaskEntry* (*maskFor)(uint8_t* mat) = nullptr;
  // shadow_inst's instanced variant of a key's VS, or nullptr (InstancedVs; fakes in the tests).
  ID3D11VertexShader* (*instVs)(const KeyEntry& key) = nullptr;
  gbbatch::TexEnv tex;  // texture classes (any thread), getDesc/compat (render thread: PredictView)
};

// sbPositions of page `page`: slot 5 hands vt[27] the buffer object
// [[[globals]+0x80] + 0x20 + page * 0x30] [V NGModel 0x1777d..0x177a2, the
// copy in shadow_tex.h Draw]; its view is at +0x30 [R17 2.1; every key probe
// checks it against the view DCS's draw bound]. Plain (callers hold SEH).
inline ID3D11ShaderResourceView* PageSrv(const Env& env, uint32_t page) {
  const uint8_t* g = env.globals ? *env.globals : nullptr;
  if (!g) return nullptr;
  const uint8_t* base = *reinterpret_cast<const uint8_t* const*>(g + 0x80);
  if (!base) return nullptr;
  const uint8_t* buf = *reinterpret_cast<const uint8_t* const*>(base + 0x20 + static_cast<size_t>(page) * 0x30);
  return buf ? *reinterpret_cast<ID3D11ShaderResourceView* const*>(buf + 0x30) : nullptr;
}

ID3D11ShaderResourceView* PageSrvGuarded(const Env& env, uint32_t page) {
  __try {
    return PageSrv(env, page);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return nullptr;
  }
}

// ---------------------------------------------------------------------------
// Job: built and recorded on workers
// ---------------------------------------------------------------------------
// Stage A (primary worker): the vector is snapshotted and classified, per
// material and per mesh once (cached for the job), per caster only what
// differs (mesh, page, texture sets, posStructOffset). Phase 1: the
// untextured records are grouped and recorded at once. Phase 2, after the
// render thread's texture snapshot: the textured records likewise. With
// splitting on, up to kThreads - 1 helper workers (by the cascade's caster
// count and lateness) read caster chunks in stage A while the primary commits
// the chunks already read in vector order, and take chunks of both phases'
// groups into their own command lists (every list runs at the pass, in
// order); the primary resolves the texture keys while the helpers start on
// the untextured groups.
// Groups: records of one material, mesh, page view and PS texture views draw
// with identical bindings except posStructOffset, so a group of two or more
// is one DrawIndexedInstanced(count, n, 0, 0, 0) with shadow_inst's
// instanced VS (posStructOffset = qvInstOffsets[qvPsoBase + SV_InstanceID],
// qvPsoBase = the group's base in our t127 buffer: CB +0xfc), exactly the
// draw shadow batching's leaders make (shadow_batch.h LeaderRaw), with the
// VS variant shadow_inst built against the key's VS (FindVsEx). Order-free.
enum : int { kJobEmpty = 0, kJobBuilt = 1 };
enum : int { kBuildNone = 0, kBuildOk, kBuildNoVector, kBuildOversize };
enum : int { kGoWait = 0, kGoRun = 1, kGoAbort = 2 };
// Job::recPhase: the group range the helpers may take chunks of (published by the primary).
enum : int { kRpWait = 0, kRpUntex = 1, kRpTex = 2, kRpEnd = 3 };
constexpr double kP1WaitUs = 20000.0;  // the primary waits this long for the helpers' last chunk of a phase
std::atomic<bool> g_testP1Wait{false};  // offline tests only: the helpers take the first shared chunks
enum : int { kStartIdle = 0, kStartArmed = 1, kStartRun = 2, kStartAbort = 3 };
constexpr DWORD kStartWaitMs = 250;  // an armed job waits this long for its vector to be final
constexpr DWORD kStartLateWaitMs = 100;  // JobMain: a start that raced the timeout publishes vec
constexpr int kThreads = 4;           // per job: the primary worker and up to 3 helpers
constexpr uint32_t kChunksPerThread = 6;  // a phase's groups are shared in about this many chunks per thread
constexpr uint32_t kMinChunk = 4;
constexpr double kHelperSpinUs = 40.0;  // a helper spins this long for the next phase before it blocks
constexpr uint32_t kMeshCache = 4096;      // power of two
constexpr uint32_t kGroupHash = 1 << 15;   // power of two
constexpr uint32_t kMaxOffsets = 1 << 16;  // t127 elements per list
constexpr uint32_t kMinSplitGroups = 64;   // textured groups worth a helper list
// Helpers per job from the cascade's last caster count (more casters, more
// helpers), one more after a job finished less than kTightMs before its pass
// or late (for the next kBoostJobs jobs). A helper costs a wake-up and one
// more ExecuteCommandList at the pass (about 8 us) [M]. Pure.
constexpr size_t kOneHelperN = 400, kTwoHelpersN = 1500;
constexpr double kTightMs = 0.25;
constexpr uint32_t kBoostJobs = 240;
inline uint32_t HelpersFor(size_t lastN, bool tight, uint32_t cap) {
  if (cap > static_cast<uint32_t>(kThreads - 1)) cap = kThreads - 1;
  if (!cap) return 0;
  uint32_t want = !lastN ? cap : lastN < kOneHelperN ? 1u : lastN < kTwoHelpersN ? 2u : 3u;  // 0: not measured
  if (tight) ++want;
  return want < cap ? want : cap;
}
// Chunk size for `groups` groups shared by `threads` threads. Pure.
inline uint32_t ChunkFor(uint32_t groups, uint32_t threads) {
  const uint32_t c = groups / (kChunksPerThread * (threads ? threads : 1));
  return c > kMinChunk ? c : kMinChunk;
}

struct Rec {
  const KeyEntry* key;
  const MeshEntry* mesh;
  ID3D11ShaderResourceView* srv;
  uint32_t caster, mat, pso;
  uint32_t setFirst, setCount;  // its texture sets' keys (Job::setKeys)
  uint16_t psKey[kMaxPsTex];    // texture key per key->psTex entry
  ID3D11ShaderResourceView* psView[kMaxPsTex];
  uint8_t tex;    // the material binds textures (phase 2)
  uint8_t alive;  // still recorded
};
struct RecMat {
  uint8_t* mat;
  const KeyEntry* key;
  uint32_t slot;  // matSlots index
  int32_t last;   // vector index of the material's last ModelMaterialMT caster (any reason)
  bool alive;     // a recorded caster still uses it
  uint8_t cb[kCbLen];
};
struct PageRef {
  uint32_t page;
  ID3D11ShaderResourceView* srv;
};
// Per material, decided once per job (its first caster).
struct MatSlot {
  uint8_t* mat;
  uint32_t gen;
  int32_t last, rec;
  int8_t textured;
  int8_t why;  // -1: not decided; kRecorded: per-caster checks follow; else every caster's reason
  uint8_t* shader;
  uint64_t tech;
  void* effect;
  void* techBegin;
  const KeyEntry* key;
  const shadowtex::MaskEntry* mask;
};
struct TexKey {
  uint8_t* tex;
  int64_t aux;
  int32_t type;
  uint8_t read;   // a recorded caster binds its view (else only its streaming request and swap state matter)
  uint8_t ok;     // snapshot: view predicted (read) or class ok and no swap due
  uint8_t used;   // a recorded caster still uses it
  uint8_t held;   // view referenced by the snapshot
  ID3D11ShaderResourceView* view;
  const TexEntry* e;  // texture table (read keys): the entry the view came from, and its version
  uint32_t ver;
};
struct TexMiss {  // a read key without a table entry
  uint8_t* tex;
  int64_t aux;
  int32_t type;
};
struct Group {
  uint32_t first;  // first record (chain: Job::recNext)
  uint32_t last;
  uint32_t count;
  const KeyEntry* key;
  const MeshEntry* mesh;
  ID3D11ShaderResourceView* srv;
  ID3D11VertexShader* instVs;  // shadow_inst's instanced variant (count >= 2), else nullptr: one draw per record
  uint32_t mat;
};

// Stage A's reads, split over the primary and (split cascades) the helpers:
// per caster everything Classify needs from DCS memory, so the serial
// commit only touches the job's own tables. Per thread (0 primary, 1..
// helpers). The primary commits each chunk in vector order once it is read
// (Job::chunkGen), and reads chunks itself while the next one is not ready.
constexpr uint32_t kPreMatTable = 1 << 12;  // power of two
constexpr uint32_t kPreChunk = 32;          // casters per chunk
constexpr uint32_t kPreChunks = static_cast<uint32_t>(kMaxCasters / kPreChunk);
constexpr uint32_t kPreSets = kMaxSetRefs;  // texture sets per thread (full: kRTooMany, residual)
constexpr double kPreWaitUs = 20000.0;      // the primary waits this long for a helper's chunk
struct PreMat {  // a material's reads
  uint8_t* mat;
  uint32_t gen;
  int8_t textured;
  bool shaderOk;  // a DX11Shader
  const shadowtex::MaskEntry* mask;
  uint8_t* shader;
  uint64_t tech;
  void* effect;
  void* techBegin;
};
struct PreSet {  // one texture set of a textured caster (slot 5's loop)
  uint8_t* tex;
  int64_t aux;
  int32_t type;
  int32_t h;
  uint8_t read;
};
struct Pre {
  uint8_t* item;
  uint8_t* mat;
  uint8_t* mesh;
  const PreMat* pm;
  const MeshEntry* me;
  ID3D11ShaderResourceView* srv;  // the page's view as read
  uint64_t group;
  uint32_t page, pso, setFirst;
  uint16_t setCount;
  int8_t code;     // a final reason (not a model caster, table full), or -1: the material decides
  int8_t texCode;  // textured: kRecorded (its sets in preSets[thread]) or why not
  uint8_t thread;
  uint8_t meshOk;  // the mesh entry was published and still describes the live mesh
};

// Per recording thread of a job (0 primary, 1.. helpers).
struct ThreadOut {
  uint32_t draws;
  uint64_t cbBytes;
  double recordUs;  // time in its chunks (waits excluded)
  int64_t tDone;    // QPC: helper done (0: not started)
  uint32_t pre;     // casters it read in stage A
  bool drew;        // helper: its list is part of the pass
};

struct Job {
  // Inputs (render thread, written only while the workers are idle).
  void** vec = nullptr;
  uint32_t flags = 0, scope = 0;
  ID3D11DepthStencilView* dsv = nullptr;  // reference held by the job
  UINT nvp = 0, nsc = 0;
  D3D11_VIEWPORT vp[kVp] = {};
  D3D11_RECT sc[kVp] = {};
  ID3D11Buffer* b7 = nullptr;  // ours (late copy)
  ID3D11SamplerState* pool[kSampSlots] = {};  // DCS's PS s5-s15 when learnt (references)
  // Texture table (null: the render thread's per-job snapshot, offline tests):
  // the workers pick the read keys' views from it under the lock (shared).
  const TexTable* tex = nullptr;
  SRWLOCK* texLock = nullptr;
  uint32_t useGen = 0;   // the render entry it serves (entries' last use)
  bool resolved = false;  // worker: the keys were resolved from the table (no snapshot)
  void* execObj = nullptr;     // first entry of the caster list handed to DCS's loop
  const Env* env = nullptr;
  const Tables* tab = nullptr;
  ID3D11Buffer* offBuf[kThreads] = {};       // t127 buffers: primary, helper workers
  ID3D11ShaderResourceView* offSrv[kThreads] = {};
  bool splitAllowed = false;   // helper workers share the job (stage A reads, both phases' groups)
  uint32_t helpers = 0;        // helpers queued with the job (1..kThreads-1 when split)
  uint32_t minSplit = kMinSplitGroups;
  bool instancing = true;      // groups of two or more as one instanced draw
  HANDLE start = nullptr;      // auto-reset: the caster vector is final (sort hook, or render entry at the latest)
  HANDLE go = nullptr;         // auto-reset: the texture snapshot is ready (or the job is aborted)
  HANDLE helperGo[kThreads] = {};  // auto-reset, per helper (1..): recPhase moved on
  HANDLE preGo = nullptr;      // manual-reset (reset at Arm): the helpers may read casters (stage A), or aborted
  std::atomic<uint32_t>* snapBell = nullptr;  // render glue: stage A done with textured casters (bit slotBit)
  uint32_t slotBit = 0;
  // Handshake.
  std::atomic<int> startState{kStartIdle};
  void* armRg = nullptr;       // the vector the sort hook watches: [[armRg+0xa18] + armIdx*24]
  uint32_t armIdx = 0;
  bool startedEarly = false;   // started by the sort hook (before RenderGraph::render entry)
  int64_t tStart = 0;
  uint32_t entryGen = 0;       // render thread: the render entry the job serves (0: not yet)
  std::atomic<int> stageA{0};  // primary: casters classified, texture keys final
  std::atomic<int> goState{kGoWait};
  std::atomic<int> preState{kGoWait};
  std::atomic<uint32_t> preNext{0}, preDone{0};  // stage A chunks taken / casters read
  bool needGo = false;         // primary: textured casters wait for the snapshot
  bool snapped = false;        // render thread: snapshot taken for this job
  // A phase's groups [from, texTo) are taken in chunks by the primary and the
  // helpers, whichever is free (depth is order-free: GREATER, no blend). The
  // primary publishes a phase (recPhase, release), closes it (recClosed) and
  // waits until no helper is inside it (recActive) before the next one.
  std::atomic<int> recPhase{kRpWait};
  std::atomic<int> recClosed{kRpWait};
  std::atomic<int> recActive{0};
  std::atomic<uint32_t> joined{0};  // bit t: helper t recorded a chunk (its list is part of the pass)
  std::atomic<uint32_t> nextGroup{0};
  uint32_t texTo = 0, chunk = 0;
  std::atomic<bool> chunkFail{false};  // a chunk could not be recorded: the job is not used
  // QPC timeline: stage A committed (tCommit) and keys resolved (tStageA), untextured recorded (tP1).
  int64_t tCommit = 0, tStageA = 0, tSnap = 0, tDone = 0, tP1 = 0;
  ThreadOut th[kThreads] = {};  // [0]: the primary's reads only (its draws are below)
  // Outputs (workers; readable once they are idle again).
  std::atomic<int> phase{kJobEmpty};
  int result = kBuildNone;
  void** begin = nullptr;
  size_t n = 0;
  uint32_t gen = 0, recCount = 0, matCount = 0, pageCount = 0, wantCount = 0, recorded = 0;
  uint32_t texKeyCount = 0, setCount = 0, vt23Count = 0, swapCount = 0, textured = 0, texRecorded = 0;
  uint32_t groupCount = 0, groupsTex = 0, draws = 0;  // draws: the primary's list
  double buildUs = 0, recordUs = 0, commitUs = 0, resolveUs = 0;  // primary time spent (waits excluded)
  uint64_t cbBytes = 0;
  uint32_t reasons[kCasterReasons] = {};
  void* snap[kMaxCasters];
  uint8_t reason[kMaxCasters];
  uint8_t want[kMaxCasters];  // probe request (one per (shader, technique, mesh))
  uint64_t group[kMaxCasters];  // shadow batching's group class (0: not a model caster)
  Rec recs[kMaxCasters];
  uint32_t recNext[kMaxCasters];  // group chains
  defrec::CbSlice slices[kMaxCasters];  // by the first record of each draw
  Group groups[kMaxCasters];
  uint32_t gHashIdx[kGroupHash];  // groups index + 1
  uint32_t gHashGen[kGroupHash];
  uint32_t offsets[kThreads][kMaxOffsets];  // t127 contents per list (primary, helpers)
  RecMat mats[kMaxRecMats];
  PageRef pages[kMaxPages];
  MatSlot matSlots[kMatTable];
  const MeshEntry* meshKey[kMeshCache];  // mesh validation cache: entry -> live fields still equal
  uint32_t meshGen[kMeshCache];
  uint8_t meshOk[kMeshCache];
  uint64_t wantKey[kWantTable];
  uint32_t wantGen[kWantTable];
  TexKey texKeys[kMaxTexKeys];
  uint32_t texIdx[kTexTable];  // texKeys index + 1
  uint32_t texGen[kTexTable];
  uint16_t setKeys[kMaxSetRefs];
  uint8_t* vt23[kMaxTexKeys];  // unique textures of the recorded casters' sets
  uint64_t groupKey[kGroupTable];
  uint32_t groupGen[kGroupTable];
  void* swapList[kMaxCasters + 1];
  TexMiss misses[kMaxTexKeys];  // read keys without a table entry (built after the pass)
  const TexEntry* refresh[kMaxTexKeys];  // read keys' entries found unusable (rebuilt after the pass)
  uint32_t missCount = 0, refreshCount = 0;
  Pre pre[kMaxCasters];
  std::atomic<uint32_t> chunkGen[kPreChunks];  // = gen once chunk k's casters are read
  uint32_t preSetCount[kThreads];
  PreSet preSets[kThreads][kPreSets];
  PreMat preMats[kThreads][kPreMatTable];
  const MeshEntry* preMeshKey[kThreads][kMeshCache];
  uint32_t preMeshGen[kThreads][kMeshCache];
  uint8_t preMeshOk[kThreads][kMeshCache];
};

// Helper totals (the lists that joined the pass).
inline uint32_t HelperDraws(const Job& j) {
  uint32_t n = 0;
  for (int t = 1; t < kThreads; ++t) n += j.th[t].drew ? j.th[t].draws : 0;
  return n;
}
inline uint64_t HelperCbBytes(const Job& j) {
  uint64_t n = 0;
  for (int t = 1; t < kThreads; ++t) n += j.th[t].drew ? j.th[t].cbBytes : 0;
  return n;
}
inline uint32_t HelperReads(const Job& j) {
  uint32_t n = 0;
  for (int t = 1; t < kThreads; ++t) n += j.th[t].pre;
  return n;
}
inline int64_t HelpersDone(const Job& j) {  // the last joined helper's end (0: none joined)
  int64_t d = 0;
  const uint32_t m = j.joined.load(std::memory_order_acquire);
  for (int t = 1; t < kThreads; ++t)
    if (((m >> t) & 1) && j.th[t].tDone > d) d = j.th[t].tDone;
  return d;
}

Job* NewJob() {
  void* mem = VirtualAlloc(nullptr, sizeof(Job), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);  // zeroed
  Job* j = mem ? new (mem) Job : nullptr;
  if (j) {
    j->start = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    j->go = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    j->preGo = CreateEventW(nullptr, TRUE, FALSE, nullptr);  // manual-reset: every helper wakes
    for (int t = 1; t < kThreads; ++t) j->helperGo[t] = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  }
  bool events = j && j->start && j->go && j->preGo;
  for (int t = 1; events && t < kThreads; ++t) events = j->helperGo[t] != nullptr;
  if (j && !events) {
    if (j->preGo) CloseHandle(j->preGo);
    if (j->start) CloseHandle(j->start);
    if (j->go) CloseHandle(j->go);
    for (HANDLE h : j->helperGo)
      if (h) CloseHandle(h);
    j->~Job();
    VirtualFree(j, 0, MEM_RELEASE);
    j = nullptr;
  }
  return j;
}

// Releases the snapshot's view references (render thread; workers idle).
void ReleaseSnapshot(Job& j) {
  for (uint32_t k = 0; k < j.texKeyCount; ++k) {
    TexKey& t = j.texKeys[k];
    if (t.held && t.view) t.view->Release();
    t.held = 0;
  }
}

void ReleasePool(ID3D11SamplerState** pool) {
  for (UINT q = 0; q < kSampSlots; ++q) SafeRel(pool[q]);
}

void FreeJob(Job*& j) {
  if (!j) return;
  ReleaseSnapshot(*j);
  SafeRel(j->dsv);
  ReleasePool(j->pool);
  if (j->start) CloseHandle(j->start);
  if (j->go) CloseHandle(j->go);
  for (HANDLE h : j->helperGo)
    if (h) CloseHandle(h);
  if (j->preGo) CloseHandle(j->preGo);
  j->~Job();
  VirtualFree(j, 0, MEM_RELEASE);
  j = nullptr;
}

// Every handshake event unsignalled (Arm, the job's workers idle).
void ResetEvents(Job& j) {
  ResetEvent(j.start);
  ResetEvent(j.go);
  ResetEvent(j.preGo);
  for (int t = 1; t < kThreads; ++t) ResetEvent(j.helperGo[t]);
}

// A t127 offsets buffer (dynamic structured uint) and its view, one per worker.
bool CreateOffsets(ID3D11Device* dev, ID3D11Buffer** buf, ID3D11ShaderResourceView** srv) {
  return shadowbatch::CreateOffsetBuffer(dev, kMaxOffsets, buf, srv);
}

MatSlot* MatSlotFor(Job& j, uint8_t* mat) {
  size_t i = Mix(reinterpret_cast<uintptr_t>(mat)) & (kMatTable - 1);
  for (uint32_t p = 0; p < kMatTable; ++p, i = (i + 1) & (kMatTable - 1)) {
    MatSlot& s = j.matSlots[i];
    if (s.gen != j.gen) {
      s = MatSlot();
      s.mat = mat;
      s.gen = j.gen;
      s.last = s.rec = -1;
      s.textured = s.why = -1;
      return &s;
    }
    if (s.mat == mat) return &s;
  }
  return nullptr;
}

void Want(Job& j, size_t i, const void* shader, uint64_t tech, const void* mesh) {
  if (j.wantCount >= kMaxWants) return;
  const uint64_t h = (reinterpret_cast<uintptr_t>(shader) * 0x9E3779B97F4A7C15ull ^ tech * 0xC2B2AE3D27D4EB4Full ^
                      reinterpret_cast<uintptr_t>(mesh) * 0x165667B19E3779F9ull) |
                     1;
  size_t s = Mix(h) & (kWantTable - 1);
  for (uint32_t p = 0; p < kWantTable; ++p, s = (s + 1) & (kWantTable - 1)) {
    if (j.wantGen[s] != j.gen) {
      j.wantGen[s] = j.gen;
      j.wantKey[s] = h;
      j.want[i] = 1;
      ++j.wantCount;
      return;
    }
    if (j.wantKey[s] == h) return;
  }
}

// The page's view for the job: the first caster's read wins (the pass
// compares it with the live one).
ID3D11ShaderResourceView* PageFor(Job& j, uint32_t page, ID3D11ShaderResourceView* read, bool* full) {
  for (uint32_t k = 0; k < j.pageCount; ++k)
    if (j.pages[k].page == page) return j.pages[k].srv;
  if (j.pageCount == kMaxPages) {
    *full = true;
    return nullptr;
  }
  if (read) j.pages[j.pageCount++] = {page, read};
  return read;
}

// The texture key (texture, aux, type) of one set; -1 when the table is full.
int32_t TexKeyFor(Job& j, uint8_t* tex, int64_t aux, int32_t type, bool read) {
  const uint64_t h = reinterpret_cast<uintptr_t>(tex) * 0x9E3779B97F4A7C15ull ^ static_cast<uint64_t>(aux) * 0xC2B2AE3D27D4EB4Full ^
                     static_cast<uint64_t>(static_cast<uint32_t>(type));
  size_t s = Mix(h) & (kTexTable - 1);
  for (uint32_t p = 0; p < kTexTable; ++p, s = (s + 1) & (kTexTable - 1)) {
    if (j.texGen[s] != j.gen) {
      if (j.texKeyCount == kMaxTexKeys) return -1;
      j.texGen[s] = j.gen;
      j.texIdx[s] = j.texKeyCount + 1;
      j.texKeys[j.texKeyCount] = {tex, aux, type, static_cast<uint8_t>(read), 0, 0, 0, nullptr};
      return static_cast<int32_t>(j.texKeyCount++);
    }
    TexKey& t = j.texKeys[j.texIdx[s] - 1];
    if (t.tex == tex && t.aux == aux && t.type == type) {
      t.read |= static_cast<uint8_t>(read);
      return static_cast<int32_t>(j.texIdx[s] - 1);
    }
  }
  return -1;
}

// Shadow batching's group of a caster (shadow_batch.h BuildCore: material,
// mesh, page, and for textured materials the read-set key with a mask, every
// entry without), as one hash. Batching's groups never span two classes.
uint64_t GroupOf(uint8_t* mat, uint8_t* item, bool textured, const shadowtex::MaskEntry* mask) {
  void* mesh = *reinterpret_cast<void**>(item + 0xc0);
  const uint32_t page = *reinterpret_cast<uint32_t*>(item + 0xd0);
  const uint64_t tex = !textured ? 0 : mask ? shadowbatch::MaskedTextureKey(mat, item, *mask)
                                            : instcount::TextureEntriesKey(mat, item);
  const uint64_t h = reinterpret_cast<uint64_t>(mat) * 0x9E3779B97F4A7C15ull ^
                     reinterpret_cast<uint64_t>(mesh) * 0xC2B2AE3D27D4EB4Full ^ (page + tex) * 0x165667B19E3779F9ull;
  return h | 1;
}

bool GroupMarked(Job& j, uint64_t g, bool mark) {
  size_t s = Mix(g) & (kGroupTable - 1);
  for (uint32_t p = 0; p < kGroupTable; ++p, s = (s + 1) & (kGroupTable - 1)) {
    if (j.groupGen[s] != j.gen) {
      if (!mark) return false;
      j.groupGen[s] = j.gen;
      j.groupKey[s] = g;
      return true;
    }
    if (j.groupKey[s] == g) return true;
  }
  return mark;  // table full: every lookup "marked" (residual), the safe side
}

// ---------------------------------------------------------------------------
// Stage A, part 1: the reads (any worker, chunks of casters)
// ---------------------------------------------------------------------------
PreMat* PreMatFor(Job& j, int t, uint8_t* mat) {
  const Env& env = *j.env;
  PreMat* tab = j.preMats[t];
  size_t i = Mix(reinterpret_cast<uintptr_t>(mat)) & (kPreMatTable - 1);
  for (uint32_t p = 0; p < kPreMatTable; ++p, i = (i + 1) & (kPreMatTable - 1)) {
    PreMat& s = tab[i];
    if (s.gen == j.gen && s.mat == mat) return &s;
    if (s.gen == j.gen) continue;
    s = PreMat();
    s.mat = mat;
    s.gen = j.gen;
    s.textured = instcount::IsTextured(mat) ? 1 : 0;
    s.mask = s.textured && env.maskFor ? env.maskFor(mat) : nullptr;
    s.shader = *reinterpret_cast<uint8_t**>(mat + 0x30);
    s.shaderOk = s.shader && *reinterpret_cast<void**>(s.shader) == env.shaderVt;
    if (s.shaderOk) {
      s.tech = shadowbatch::TechOf(mat);
      s.effect = *reinterpret_cast<void**>(s.shader + 0x50);
      s.techBegin = *reinterpret_cast<void**>(s.shader + 0xb0);
    }
    return &s;
  }
  return nullptr;
}

// The mesh entry still describes the live mesh (once per entry per thread).
bool PreMeshOk(Job& j, int t, uint8_t* mesh, const MeshEntry* me) {
  auto live = [&] {
    MeshLive l;
    return !ReadMesh(mesh, l) && l.fp == me->fp && l.vb == me->vb && l.ib == me->ib && l.stride == me->stride &&
           l.ibFormat == me->ibFormat;
  };
  size_t s = Mix(reinterpret_cast<uintptr_t>(me)) & (kMeshCache - 1);
  for (uint32_t p = 0; p < kMeshCache; ++p, s = (s + 1) & (kMeshCache - 1)) {
    if (j.preMeshGen[t][s] != j.gen) {
      const bool ok = live();
      j.preMeshGen[t][s] = j.gen;
      j.preMeshKey[t][s] = me;
      j.preMeshOk[t][s] = ok ? 1 : 0;
      return ok;
    }
    if (j.preMeshKey[t][s] == me) return j.preMeshOk[t][s] != 0;
  }
  return live();  // cache full: check directly
}

// The texture sets of a textured caster (slot 5's loop [V NGModel
// 0x177f4..0x1785c]), appended to preSets[t]. kRecorded, or why not.
int PreTextures(Job& j, int t, Pre& p, const PreMat& pm) {
  const Env& env = *j.env;
  uint8_t* mat = p.mat;
  auto* props = *reinterpret_cast<uint8_t**>(mat + 0x28);
  auto* arr = *reinterpret_cast<uint8_t***>(p.item + 0x18);
  if (!props || !arr || !*arr) {
    g_classNoSets++;
    return kRTexClass;
  }
  const uint32_t n = *reinterpret_cast<uint32_t*>(mat + 0x2d8);
  if (n > 32) return kRTooMany;
  const uint8_t* recs = *reinterpret_cast<uint8_t**>(pm.shader + 0xc8);
  const uint8_t* recEnd = *reinterpret_cast<uint8_t**>(pm.shader + 0xd0);
  const int64_t nrec = recs && recEnd > recs ? (recEnd - recs) / 0x50 : 0;
  const uint8_t* en = *arr + static_cast<size_t>(*reinterpret_cast<uint32_t*>(props + 0x26c)) * 0x18;
  p.setFirst = j.preSetCount[t];
  p.setCount = 0;
  for (uint32_t i = 0; i < n; ++i, en += 0x18) {
    const int64_t h = *reinterpret_cast<int64_t*>(mat + 0x240 + 8 * i);
    if (h == -1) continue;
    auto* tex = *reinterpret_cast<uint8_t* const*>(en + 8);
    const int64_t aux = *reinterpret_cast<const int64_t*>(en);
    const bool read = !shadowtex::Skippable(*pm.mask, h);
    if (!tex) {
      if (read) return kRTexNull;  // slot 26 leaves the variable as an earlier draw set it
      continue;                    // nothing at all
    }
    if (h < 0 || h >= nrec) {
      g_classRange++;
      return kRTexClass;
    }
    if (!TexClassOk(env.tex, tex)) {
      const void* vt = *reinterpret_cast<void* const*>(tex);
      const void* inner = vt == env.tex.texVtbl ? *reinterpret_cast<void* const*>(tex + 0x10) : nullptr;
      CensusAdd(g_census, vt, inner ? *static_cast<void* const*>(inner) : nullptr);  // as TexClassOk read them
      return kRTexClass;
    }
    if (j.preSetCount[t] == kPreSets) return kRTooMany;
    const int32_t type = *reinterpret_cast<const int32_t*>(recs + h * 0x50 + 0xc);
    j.preSets[t][j.preSetCount[t]++] = {tex, aux, type, static_cast<int32_t>(h), static_cast<uint8_t>(read)};
    ++p.setCount;
  }
  return kRecorded;
}

void PreOne(Job& j, int t, size_t i) {
  const Env& env = *j.env;
  Pre& p = j.pre[i];
  p.thread = static_cast<uint8_t>(t);
  p.pm = nullptr;
  p.me = nullptr;
  p.srv = nullptr;
  p.group = 0;
  p.setCount = 0;
  p.texCode = kRecorded;
  p.meshOk = 0;
  void* r = j.snap[i];
  if (!r || *static_cast<void**>(r) != env.smrVt) {
    p.code = kRNotSmr;
    return;
  }
  auto* item = *reinterpret_cast<uint8_t**>(static_cast<uint8_t*>(r) + 0x10);
  auto* mat = item ? *reinterpret_cast<uint8_t**>(item + 0x10) : nullptr;
  if (!mat || *reinterpret_cast<void**>(mat) != env.modelVt) {
    p.code = kRNotModel;
    return;
  }
  p.item = item;
  p.mat = mat;
  p.mesh = *reinterpret_cast<uint8_t**>(item + 0xc0);
  const PreMat* pm = PreMatFor(j, t, mat);
  if (!pm) {
    p.code = kRMatTable;
    return;
  }
  p.code = -1;
  p.pm = pm;
  const bool textured = pm->textured != 0;
  p.group = GroupOf(mat, item, textured, pm->mask);
  if (!(j.scope & (textured ? kScopeTextured : kScopeUntextured)) || (textured && !pm->mask) || !pm->shaderOk)
    return;  // the material's decision is the reason
  p.me = FindMesh(*j.tab, p.mesh, pm->shader, pm->tech, pm->effect, pm->techBegin);
  if (p.me && p.me->state.load(std::memory_order_acquire) > 0) p.meshOk = PreMeshOk(j, t, p.mesh, p.me) ? 1 : 0;
  p.page = *reinterpret_cast<uint32_t*>(item + 0xd0);
  p.srv = PageSrv(env, p.page);
  p.pso = *reinterpret_cast<uint32_t*>(item + 0xd4);
  if (textured) {
    const uint32_t s0 = j.preSetCount[t];
    p.texCode = static_cast<int8_t>(PreTextures(j, t, p, *pm));
    if (p.texCode != kRecorded) j.preSetCount[t] = s0;
  }
}

// Reads the chunk starting at caster `from` on thread t and publishes it
// (chunkGen, release: its Pre entries and thread t's tables it points into).
void PreChunk(Job& j, int t, uint32_t from) {
  const uint32_t n = static_cast<uint32_t>(j.n);
  const uint32_t to = from + kPreChunk < n ? from + kPreChunk : n;
  for (uint32_t i = from; i < to; ++i) PreOne(j, t, i);
  j.th[t].pre += to - from;
  j.chunkGen[from / kPreChunk].store(j.gen, std::memory_order_release);
  j.preDone.fetch_add(to - from, std::memory_order_release);
}

// Reads chunks of casters until none is left (t: 0 primary, 1.. helpers).
void PreChunks(Job& j, int t) {
  LoadScope load(g_load);
  const uint32_t n = static_cast<uint32_t>(j.n);
  for (;;) {
    const uint32_t from = j.preNext.fetch_add(kPreChunk, std::memory_order_relaxed);
    if (from >= n) return;
    PreChunk(j, t, from);
  }
}

// Primary: every chunk read (the helpers' last ones included). False after
// kPreWaitUs (a helper stalled: the job is not built).
bool WaitPre(Job& j) {
  const int64_t t0 = defrec::Qpc();
  const double k = defrec::QpcToUs();
  for (uint32_t spin = 0; j.preDone.load(std::memory_order_acquire) < j.n; ++spin) {
    if ((defrec::Qpc() - t0) * k > kPreWaitUs) return false;
    // Yield after a short spin (as ClosePhase): on 2-4 core CPUs a pure spin
    // can hold the core the helper it waits for needs, so the job ran late.
    if (spin < 4096)
      _mm_pause();
    else
      SwitchToThread();
  }
  return true;
}

// ---------------------------------------------------------------------------
// Stage A, part 2: the commit (primary, in vector order; job tables only)
// ---------------------------------------------------------------------------
// The keys of a textured caster's sets, the read ones matched to the key's
// PS slots. kRecorded, or why not.
int CommitTextures(Job& j, const Pre& p, const KeyEntry& key, Rec& rc) {
  rc.setFirst = j.setCount;
  rc.setCount = 0;
  uint32_t found = 0;
  const PreSet* s = j.preSets[p.thread] + p.setFirst;
  for (uint32_t k = 0; k < p.setCount; ++k) {
    const int32_t tk = TexKeyFor(j, s[k].tex, s[k].aux, s[k].type, s[k].read != 0);
    if (tk < 0 || j.setCount == kMaxSetRefs) return kRTooMany;
    j.setKeys[j.setCount++] = static_cast<uint16_t>(tk);
    ++rc.setCount;
    if (!s[k].read) continue;
    int q = 0;
    while (q < key.psTexCount && key.psTexH[q] != s[k].h) ++q;
    if (q == key.psTexCount || (found & (1u << q))) return kRTexMissing;
    found |= 1u << q;
    rc.psKey[q] = static_cast<uint16_t>(tk);
  }
  if (found != (1u << key.psTexCount) - 1) return kRTexMissing;
  return kRecorded;
}

// Material-level decision (its first caster in the job), from that caster's reads.
void DecideMaterial(Job& j, MatSlot& ms, const PreMat& pm) {
  const Env& env = *j.env;
  ms.textured = pm.textured;
  ms.mask = pm.mask;
  ms.shader = pm.shader;
  if (!(j.scope & (ms.textured ? kScopeTextured : kScopeUntextured))) {
    ms.why = static_cast<int8_t>(ms.textured ? kRTextured : kRScope);
    return;
  }
  if (ms.textured && !ms.mask) {
    ms.why = kRTexMask;
    return;
  }
  if (!pm.shaderOk) {
    ms.why = kRNoShader;
    return;
  }
  ms.tech = pm.tech;
  ms.effect = pm.effect;
  ms.techBegin = pm.techBegin;
  ms.key = FindKey(*j.tab, ms.shader, ms.tech, j.flags, ms.effect, ms.techBegin);
  if (!ms.key && KeyCooling(*j.tab, ms.shader, ms.tech, j.flags, ms.effect, ms.techBegin, 0, true)) {
    ms.why = kRKeyCooling;  // relearnt by a probe once its cooldown ends (Want below)
  } else if (!ms.key) {
    ms.why = static_cast<int8_t>(env.compiled && !env.compiled(ms.shader, ms.tech) ? kRNoCompile : kRKeyPending);
  } else if (ms.key->state.load(std::memory_order_acquire) <= 0) {
    ms.why = kRKeyRejected;
  } else if (!ms.textured && ms.key->psTexCount) {
    ms.why = kRTexMissing;  // its PS would read a texture this caster never sets
  } else {
    ms.why = kRecorded;
  }
}

int Classify(Job& j, size_t i) {
  const Pre& p = j.pre[i];
  j.group[i] = 0;
  if (p.code >= 0) return p.code;
  MatSlot* ms = MatSlotFor(j, p.mat);
  if (!ms) return kRMatTable;
  ms->last = static_cast<int32_t>(i);
  const PreMat& pm = *p.pm;
  if (ms->why < 0) DecideMaterial(j, *ms, pm);
  const bool textured = ms->textured != 0;
  // Two threads read the material: a caster whose reads differ from the
  // decision's (a mask published in between) is residual, in the class the
  // decision's reads give it.
  if (pm.textured != ms->textured || pm.mask != ms->mask || pm.shader != ms->shader) {
    j.group[i] = GroupOf(p.mat, p.item, textured, ms->mask);
    return kRTexMask;
  }
  j.group[i] = p.group;
  if (ms->why != kRecorded) {
    if (ms->why == kRKeyPending || ms->why == kRKeyCooling) Want(j, i, ms->shader, ms->tech, p.mesh);
    return ms->why;
  }
  if (pm.tech != ms->tech || pm.effect != ms->effect || pm.techBegin != ms->techBegin) return kRNoShader;
  const KeyEntry* key = ms->key;
  const MeshEntry* me = p.me;
  if (!me) {
    Want(j, i, ms->shader, ms->tech, p.mesh);
    return kRMeshPending;
  }
  if (me->state.load(std::memory_order_acquire) <= 0) return kRMeshRejected;
  if (!p.meshOk) return kRMeshChanged;
  bool full = false;
  ID3D11ShaderResourceView* srv = PageFor(j, p.page, p.srv, &full);
  if (!srv) return full ? kRTooMany : kRPage;
  Rec rc = {};
  rc.key = key;
  rc.mesh = me;
  rc.srv = srv;
  rc.caster = static_cast<uint32_t>(i);
  rc.pso = p.pso;
  rc.tex = textured ? 1 : 0;
  rc.alive = 1;
  if (textured) {
    if (p.texCode != kRecorded) return p.texCode;
    const uint32_t sets0 = j.setCount;
    const int why = CommitTextures(j, p, *key, rc);
    if (why != kRecorded) {
      j.setCount = sets0;
      return why;
    }
  }
  if (ms->rec < 0) {
    if (j.matCount == kMaxRecMats) return kRTooMany;
    RecMat& rm = j.mats[j.matCount];
    rm.mat = p.mat;
    rm.key = key;
    rm.slot = static_cast<uint32_t>(ms - j.matSlots);
    rm.last = -1;
    rm.alive = false;
    memcpy(rm.cb, p.mat + kCbOff, kCbLen);
    ms->rec = static_cast<int32_t>(j.matCount++);
  }
  rc.mat = static_cast<uint32_t>(ms->rec);
  j.recs[j.recCount++] = rc;
  if (textured) {
    ++j.textured;
    j.needGo = true;
  }
  return kRecorded;
}

// Stage A (workers; the offline tests call BuildJob directly). CPU only: DCS
// memory is read, never written; no D3D call; no texture view is read.
// BeginBuild snapshots the vector; the reads (PreChunks) run on one or two
// workers; CommitBuild classifies in vector order from them. Every batching
// group class with a residual caster is marked (its recorded casters become
// residual in FinishPhase). Plain (the pool runs it under SEH).
int BeginBuild(Job& j) {
  j.recCount = j.matCount = j.pageCount = j.wantCount = j.recorded = 0;
  j.texKeyCount = j.setCount = j.vt23Count = j.swapCount = j.textured = j.texRecorded = 0;
  j.groupCount = j.groupsTex = j.draws = 0;
  j.needGo = j.resolved = false;
  j.missCount = j.refreshCount = 0;
  j.chunkFail.store(false, std::memory_order_relaxed);
  j.nextGroup.store(0, std::memory_order_relaxed);
  j.texTo = j.chunk = 0;
  j.cbBytes = 0;
  j.recordUs = j.commitUs = j.resolveUs = 0;
  memset(j.reasons, 0, sizeof(j.reasons));
  j.n = 0;
  j.begin = nullptr;
  j.result = kBuildNone;
  j.preNext.store(0, std::memory_order_relaxed);
  j.preDone.store(0, std::memory_order_relaxed);
  for (uint32_t& c : j.preSetCount) c = 0;
  for (ThreadOut& o : j.th) o = ThreadOut();  // the helpers touch theirs only after preGo
  j.joined.store(0, std::memory_order_relaxed);
  j.recPhase.store(kRpWait, std::memory_order_relaxed);  // published to the helpers by preGo
  j.recClosed.store(kRpWait, std::memory_order_relaxed);
  j.recActive.store(0, std::memory_order_relaxed);
  j.tCommit = j.tP1 = 0;
  auto** begin = static_cast<void**>(j.vec[0]);
  auto** end = static_cast<void**>(j.vec[1]);
  j.begin = begin;
  if (!begin || end < begin) return j.result = kBuildNoVector;
  const size_t n = static_cast<size_t>(end - begin);
  if (n > kMaxCasters) return j.result = kBuildOversize;
  memcpy(j.snap, begin, n * sizeof(void*));
  j.n = n;
  if (++j.gen == 0) {
    memset(j.matSlots, 0, sizeof(j.matSlots));
    memset(j.wantGen, 0, sizeof(j.wantGen));
    memset(j.texGen, 0, sizeof(j.texGen));
    memset(j.groupGen, 0, sizeof(j.groupGen));
    memset(j.meshGen, 0, sizeof(j.meshGen));
    memset(j.gHashGen, 0, sizeof(j.gHashGen));
    memset(j.preMats, 0, sizeof(j.preMats));
    memset(j.preMeshGen, 0, sizeof(j.preMeshGen));
    for (auto& g : j.chunkGen) g.store(0, std::memory_order_relaxed);
    j.gen = 1;
  }
  return kBuildOk;
}

// The commit of casters [from, to), in vector order (their reads are done).
void CommitRange(Job& j, size_t from, size_t to) {
  for (size_t i = from; i < to; ++i) {
    j.want[i] = 0;
    const int r = Classify(j, i);
    j.reason[i] = static_cast<uint8_t>(r);
    ++j.reasons[r];
  }
}

int CommitEnd(Job& j) {
  for (uint32_t m = 0; m < j.matCount; ++m) j.mats[m].last = j.matSlots[j.mats[m].slot].last;
  for (size_t i = 0; i < j.n; ++i)
    if (j.group[i] && j.reason[i] != kRecorded) GroupMarked(j, j.group[i], true);
  return j.result = kBuildOk;
}

int CommitBuild(Job& j) {
  CommitRange(j, 0, j.n);
  return CommitEnd(j);
}

// Primary: the reads and the commit pipelined. The next chunk in vector
// order is committed as soon as it is read (by any thread); while it is not
// ready the primary reads an untaken chunk itself, else waits (bounded). The
// classification equals CommitBuild's after all reads: the same Classify
// calls in the same order on the same reads. False when a helper stalled
// (kPreWaitUs): the job is not built.
bool PreAndCommit(Job& j) {
  const uint32_t n = static_cast<uint32_t>(j.n);
  const uint32_t chunks = (n + kPreChunk - 1) / kPreChunk;
  const double k = defrec::QpcToUs();
  double commitUs = 0;
  uint32_t next = 0;
  int64_t w0 = 0;  // start of a wait for a chunk a helper holds
  uint32_t spins = 0;
  {
    LoadScope load(g_load);
    while (next < chunks) {
      if (j.chunkGen[next].load(std::memory_order_acquire) == j.gen) {
        const int64_t c0 = defrec::Qpc();
        const size_t from = static_cast<size_t>(next) * kPreChunk;
        CommitRange(j, from, from + kPreChunk < n ? from + kPreChunk : n);
        commitUs += (defrec::Qpc() - c0) * k;
        ++next;
        w0 = 0;
        continue;
      }
      if (j.preNext.load(std::memory_order_relaxed) < n) {
        const uint32_t from = j.preNext.fetch_add(kPreChunk, std::memory_order_relaxed);
        if (from < n) {
          PreChunk(j, 0, from);
          continue;
        }
      }
      if (!w0) {
        w0 = defrec::Qpc();
        spins = 0;
      }
      if ((defrec::Qpc() - w0) * k > kPreWaitUs) {
        j.commitUs = commitUs;
        return false;
      }
      if (++spins < 4096)  // then yield: see WaitPre
        _mm_pause();
      else
        SwitchToThread();
    }
  }
  const int64_t c0 = defrec::Qpc();
  CommitEnd(j);
  j.commitUs = commitUs + (defrec::Qpc() - c0) * k;
  return true;
}

// The texture keys from the table (worker, after the commit, the table's lock
// held shared): a read key takes its entry's view (getSRV's pick for slot 5's
// size) when the entry is usable, current and has no swap due; a missing or
// stale entry is built after the pass (its casters are residual this frame).
// An unread key needs only a replicated class and no swap due. Replaces the
// render thread's snapshot: phase 2 does not wait.
void ResolveTexKeys(Job& j) {
  const TexTable& t = *j.tex;
  const gbbatch::TexEnv& env = j.env->tex;
  for (uint32_t k = 0; k < j.texKeyCount; ++k) {
    TexKey& tk = j.texKeys[k];
    tk.ok = 0;
    tk.view = nullptr;
    tk.e = nullptr;
    if (!tk.read) {
      tk.ok = TexClassOk(env, tk.tex) && !SwapDue(tk.tex, tk.aux) ? 1 : 0;
      continue;
    }
    const TexEntry* e = TexFind(t, tk.tex, tk.aux, tk.type);
    if (!e) {
      if (j.missCount < kMaxTexKeys) j.misses[j.missCount++] = {tk.tex, tk.aux, tk.type};
      continue;
    }
    const_cast<TexEntry*>(e)->lastUse.store(j.useGen, std::memory_order_relaxed);
    ID3D11ShaderResourceView* v = nullptr;
    if (!e->usable || e->dirty.load(std::memory_order_relaxed) || TexSwapDueGuarded(*e) || !TexPick(*e, kTexSize, &v)) {
      if ((e->usable || e->why == kTwSwap || e->why == kTwFault || e->dirty.load(std::memory_order_relaxed)) &&
          j.refreshCount < kMaxTexKeys)
        j.refresh[j.refreshCount++] = e;
      continue;
    }
    tk.e = e;
    tk.ver = e->version.load(std::memory_order_relaxed);
    tk.view = v;
    tk.ok = 1;
  }
  j.resolved = true;
}

int BuildJob(Job& j) {
  const int r = BeginBuild(j);
  if (r != kBuildOk) return r;
  PreChunks(j, 0);
  return CommitBuild(j);
}

// Render thread: the snapshot of a job's texture keys. Read keys: the view
// DCS's getSRV would return now (PredictView; no swap due), referenced until
// the pass is done; the other keys: replicated class, no swap due.
uint8_t PredictViewGuarded(const gbbatch::TexEnv& env, uint8_t* tex, int64_t aux, int32_t type, void** view) {
  __try {
    return gbbatch::PredictView(env, tex, aux, type, kTexSize, view);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return gbbatch::kTexClass;
  }
}

bool SkipStateOkGuarded(const gbbatch::TexEnv& env, const uint8_t* tex, int64_t aux) {
  __try {
    return TexClassOk(env, tex) && !SwapDue(tex, aux);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

bool AddRefGuarded(IUnknown* p) {
  __try {
    p->AddRef();
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

void TakeSnapshot(Job& j) {
  const gbbatch::TexEnv& env = j.env->tex;
  for (uint32_t k = 0; k < j.texKeyCount; ++k) {
    TexKey& t = j.texKeys[k];
    t.ok = 0;
    t.view = nullptr;
    if (t.read) {
      void* v = nullptr;
      if (PredictViewGuarded(env, t.tex, t.aux, t.type, &v) == gbbatch::kOk &&
          (!v || AddRefGuarded(static_cast<IUnknown*>(v)))) {
        t.view = static_cast<ID3D11ShaderResourceView*>(v);
        t.held = v != nullptr;
        t.ok = 1;
      }
    } else {
      t.ok = SkipStateOkGuarded(env, t.tex, t.aux) ? 1 : 0;
    }
  }
  j.snapped = true;
  j.tSnap = defrec::Qpc();
  j.goState.store(kGoRun, std::memory_order_release);
  SetEvent(j.go);
}

void Demote(Job& j, uint32_t r, int why) {
  Rec& rc = j.recs[r];
  rc.alive = 0;
  j.reason[rc.caster] = static_cast<uint8_t>(why);
  --j.reasons[kRecorded];
  ++j.reasons[why];
}

// One phase's records (untextured, or textured with the snapshot's views):
// textured records without a usable snapshot become residual and mark their
// class; records in a marked class become residual; the materials, texture
// keys and streaming requests still used are marked.
void FinishPhase(Job& j, bool textured, bool snapshot) {
  if (textured)
    for (uint32_t r = 0; r < j.recCount; ++r) {
      Rec& rc = j.recs[r];
      if (!rc.tex || !rc.alive) continue;
      bool ok = snapshot;
      for (uint32_t s = 0; ok && s < rc.setCount; ++s) ok = j.texKeys[j.setKeys[rc.setFirst + s]].ok != 0;
      if (ok) continue;
      Demote(j, r, kRTexView);
      GroupMarked(j, j.group[rc.caster], true);
    }
  for (uint32_t r = 0; r < j.recCount; ++r) {
    Rec& rc = j.recs[r];
    if (rc.tex != (textured ? 1 : 0) || !rc.alive) continue;
    if (GroupMarked(j, j.group[rc.caster], false)) {
      Demote(j, r, kRGroup);
      continue;
    }
    j.mats[rc.mat].alive = true;
    if (!textured) continue;
    for (int p = 0; p < rc.key->psTexCount; ++p) rc.psView[p] = j.texKeys[rc.psKey[p]].view;
    for (uint32_t s = 0; s < rc.setCount; ++s) {
      TexKey& t = j.texKeys[j.setKeys[rc.setFirst + s]];
      if (t.used) continue;
      t.used = 1;
      bool seen = false;  // one streaming request per texture (keys differ by aux/type)
      for (uint32_t v = 0; v < j.vt23Count && !seen; ++v) seen = j.vt23[v] == t.tex;
      if (!seen) j.vt23[j.vt23Count++] = t.tex;
    }
  }
}

// shadow_inst's instanced variant of the key's VS, built against that VS
// (lock-free map; shadow batching's leaders use the same).
ID3D11VertexShader* InstancedVs(const KeyEntry& k) {
  void* expect = nullptr;
  ID3D11VertexShader* vs = shadowinst::FindVsEx(k.shader.load(std::memory_order_relaxed), k.tech, 0, &expect);
  return vs && expect == static_cast<void*>(k.vs) ? vs : nullptr;
}

// Groups one phase's live records (vector order within a group), sorted by
// key and mesh (fewer state changes; order-free). Appends to groups[].
void BuildGroups(Job& j, bool textured, bool instancing) {
  const uint32_t g0 = j.groupCount;
  for (uint32_t r = 0; r < j.recCount; ++r) {
    const Rec& rc = j.recs[r];
    if (rc.tex != (textured ? 1 : 0) || !rc.alive) continue;
    uint64_t h = (static_cast<uint64_t>(rc.mat) + 1) * 0x9E3779B97F4A7C15ull ^
                 reinterpret_cast<uintptr_t>(rc.mesh) * 0xC2B2AE3D27D4EB4Full ^
                 reinterpret_cast<uintptr_t>(rc.srv) * 0x165667B19E3779F9ull;
    for (int p = 0; p < rc.key->psTexCount; ++p) h = (h ^ reinterpret_cast<uintptr_t>(rc.psView[p])) * 0x100000001b3ull;
    // Slots of the other phase's groups (sorted since) never match: other materials.
    size_t s = Mix(h) & (kGroupHash - 1);
    uint32_t gi = ~0u;
    for (uint32_t p = 0; p < kGroupHash; ++p, s = (s + 1) & (kGroupHash - 1)) {
      if (j.gHashGen[s] != j.gen) {
        j.gHashGen[s] = j.gen;
        j.gHashIdx[s] = j.groupCount + 1;
        gi = j.groupCount++;
        j.groups[gi] = {r, r, 0, rc.key, rc.mesh, rc.srv, nullptr, rc.mat};
        break;
      }
      const uint32_t idx = j.gHashIdx[s] - 1;
      if (idx < g0) continue;
      const Group& g = j.groups[idx];
      const Rec& f = j.recs[g.first];
      bool same = f.mat == rc.mat && f.mesh == rc.mesh && f.srv == rc.srv;
      for (int q = 0; same && q < rc.key->psTexCount; ++q) same = f.psView[q] == rc.psView[q];
      if (same) {
        gi = idx;
        break;
      }
    }
    if (gi == ~0u) {  // table full: its own group
      gi = j.groupCount++;
      j.groups[gi] = {r, r, 0, rc.key, rc.mesh, rc.srv, nullptr, rc.mat};
    }
    Group& g = j.groups[gi];
    if (g.count) j.recNext[g.last] = r;
    g.last = r;
    j.recNext[r] = ~0u;
    ++g.count;
  }
  for (uint32_t gi = g0; gi < j.groupCount; ++gi) {
    Group& g = j.groups[gi];
    g.instVs = instancing && g.count >= 2 && j.env->instVs ? j.env->instVs(*g.key) : nullptr;
  }
  std::sort(j.groups + g0, j.groups + j.groupCount, [](const Group& a, const Group& b) {
    if (a.key != b.key) return a.key < b.key;
    if (a.mesh != b.mesh) return a.mesh < b.mesh;
    return a.first < b.first;
  });
}

// After phase 2: the recorded count and the caster list for DCS's loop:
// [exec object, every caster not recorded, in vector order].
void FinishList(Job& j) {
  j.recorded = j.texRecorded = 0;
  for (uint32_t r = 0; r < j.recCount; ++r)
    if (j.recs[r].alive) {
      ++j.recorded;
      j.texRecorded += j.recs[r].tex;
    }
  j.swapCount = 0;
  j.swapList[j.swapCount++] = j.execObj;
  for (size_t i = 0; i < j.n; ++i)
    if (j.reason[i] != kRecorded) j.swapList[j.swapCount++] = j.snap[i];
}

// Both phases at once (offline tests; the workers interleave them with recording).
void FinishJob(Job& j, bool snapshot) {
  FinishPhase(j, false, false);
  BuildGroups(j, false, j.instancing);
  FinishPhase(j, true, snapshot);
  j.groupsTex = j.groupCount;
  BuildGroups(j, true, j.instancing);
  FinishList(j);
}

// Records groups [from, to) on a worker's deferred context: the pass-level
// state of DCS's frame buffer and binder (no render target, DCS's depth
// view, viewports, scissors; VS and PS b7 = our late-copied per-view buffer),
// then per draw exactly the bindings DCS's draw leaves for its shaders (VS,
// PS or none, samplers, depth/blend/raster states, input layout, vertex and
// index buffers, topology, def_uniforms at the VS slot and at the PS slots
// holding it, sbPositions, the read textures' views at the PS slots) and
// DrawIndexed(3 * count, 0, 0); a group of two or more with an instanced VS:
// that VS, our offsets at t127 and DrawIndexedInstanced(3 * count, n, 0, 0,
// 0), CB +0xfc = the group's base. GS/HS/DS stay NULL as in DCS's draws of
// these keys (probe). Bindings equal to the previous draw's are not
// repeated. CB windows and offsets first (one Map per ring chunk / buffer),
// then the draws (deferred_rec.h's batched pattern).
bool RecordGroups(defrec::Worker& w, Job& j, uint32_t from, uint32_t to, int list, uint32_t* draws, uint64_t* cbBytes) {
  LoadScope load(g_load);
  ID3D11DeviceContext* dc = w.dc;
  if (!w.dc1 || w.ring.Mode() != defrec::CbMode::kOffsets || !j.dsv || !j.b7) return false;
  if (from >= to) return true;
  uint32_t* off = j.offsets[list];
  uint32_t nOff = 0;
  for (uint32_t gi = from; gi < to; ++gi) {
    Group& g = j.groups[gi];
    const uint8_t* cb = j.mats[g.mat].cb;
    if (g.instVs && nOff + g.count > kMaxOffsets) g.instVs = nullptr;  // this list's t127 buffer is full
    if (g.instVs) {
      const uint32_t base = nOff;
      for (uint32_t r = g.first, k = 0; k < g.count; ++k, r = j.recNext[r]) off[nOff++] = j.recs[r].pso;
      defrec::CbSlice s = w.ring.Alloc(kCbLen);
      if (!s) return false;
      memcpy(s.ptr, cb, kCbLen);
      memcpy(s.ptr + kPsoCb, &base, 4);
      j.slices[g.first] = s;
      *cbBytes += static_cast<uint64_t>(s.num) * 16;
      continue;
    }
    for (uint32_t r = g.first, k = 0; k < g.count; ++k, r = j.recNext[r]) {
      defrec::CbSlice s = w.ring.Alloc(kCbLen);
      if (!s) return false;
      memcpy(s.ptr, cb, kCbLen);
      memcpy(s.ptr + kPsoCb, &j.recs[r].pso, 4);
      j.slices[r] = s;
      *cbBytes += static_cast<uint64_t>(s.num) * 16;
    }
  }
  w.ring.Close();
  ID3D11Buffer* ob = j.offBuf[list];
  if (nOff) {
    D3D11_MAPPED_SUBRESOURCE m;
    if (!ob || FAILED(dc->Map(ob, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) return false;
    memcpy(m.pData, off, nOff * sizeof(uint32_t));
    dc->Unmap(ob, 0);
    dc->VSSetShaderResources(127, 1, &j.offSrv[list]);
  }
  dc->OMSetRenderTargets(0, nullptr, j.dsv);
  dc->RSSetViewports(j.nvp, j.vp);
  dc->RSSetScissorRects(j.nsc, j.nsc ? j.sc : nullptr);
  dc->VSSetConstantBuffers(kB7, 1, &j.b7);
  dc->PSSetConstantBuffers(kB7, 1, &j.b7);
  const KeyEntry* ck = nullptr;
  const MeshEntry* cm = nullptr;
  auto* const kNone = reinterpret_cast<void*>(1);  // never equal to a binding
  void *vs = kNone, *ps = kNone, *dss = kNone, *bs = kNone, *rs = kNone, *il = kNone, *vb = kNone, *ib = kNone,
       *srv = kNone;
  void* samp[kSampSlots];
  for (auto*& s : samp) s = kNone;
  // DCS's sampler pool as the exec entry checks it; keys never rebind it.
  dc->PSSetSamplers(kPoolFirst, kSampSlots - kPoolFirst, &j.pool[kPoolFirst]);
  for (UINT q = kPoolFirst; q < kSampSlots; ++q) samp[q] = j.pool[q];
  void* psv[kSrvSlots];
  for (auto*& v : psv) v = kNone;
  UINT ref = 0, mask = 0, stride = 0, sbSlot = ~0u;
  float factor[4] = {};
  DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN;
  bool topo = false;
  uint32_t n = 0;
  for (uint32_t gi = from; gi < to; ++gi) {
    const Group& g = j.groups[gi];
    const KeyEntry& k = *g.key;
    const MeshEntry& m = *g.mesh;
    if (&k != ck) {
      ck = &k;
      if (k.ps != ps) dc->PSSetShader(k.ps, nullptr, 0), ps = k.ps;
      if (k.ps) {  // its FX sampler slots below the pool (every slot below it when not read)
        const uint32_t own = KeyOwnSamplers(k.psSampDeps);
        for (UINT s = 0; s < kPoolFirst; ++s)
          if (((own >> s) & 1) && k.psSamp[s] != samp[s]) dc->PSSetSamplers(s, 1, &k.psSamp[s]), samp[s] = k.psSamp[s];
      }
      if (k.dss != dss || k.stencilRef != ref) dc->OMSetDepthStencilState(k.dss, k.stencilRef), dss = k.dss, ref = k.stencilRef;
      if (k.bs != bs || k.sampleMask != mask || memcmp(k.blendFactor, factor, sizeof(factor)) != 0) {
        dc->OMSetBlendState(k.bs, k.blendFactor, k.sampleMask);
        bs = k.bs;
        mask = k.sampleMask;
        memcpy(factor, k.blendFactor, sizeof(factor));
      }
      if (k.rs != rs) dc->RSSetState(k.rs), rs = k.rs;
    }
    ID3D11VertexShader* gvs = g.instVs ? g.instVs : k.vs;
    if (gvs != vs) dc->VSSetShader(gvs, nullptr, 0), vs = gvs;
    if (&m != cm) {
      cm = &m;
      if (m.il != il) dc->IASetInputLayout(m.il), il = m.il;
      if (m.vb != vb || m.stride != stride) {
        const UINT zero = 0;
        dc->IASetVertexBuffers(0, 1, &m.vb, &m.stride, &zero);
        vb = m.vb;
        stride = m.stride;
      }
      if (m.ib != ib || m.ibFormat != fmt) dc->IASetIndexBuffer(m.ib, m.ibFormat, 0), ib = m.ib, fmt = m.ibFormat;
      if (!topo) dc->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST), topo = true;
    }
    if (g.srv != srv || k.st.sbSlot != sbSlot) {
      ID3D11ShaderResourceView* v = g.srv;
      dc->VSSetShaderResources(k.st.sbSlot, 1, &v);
      srv = g.srv;
      sbSlot = k.st.sbSlot;
    }
    const Rec& f = j.recs[g.first];
    for (int p = 0; p < k.psTexCount; ++p) {
      const UINT s = k.psTexSlot[p];
      if (psv[s] != f.psView[p]) {
        ID3D11ShaderResourceView* v = f.psView[p];
        dc->PSSetShaderResources(s, 1, &v);
        psv[s] = v;
      }
    }
    auto bind = [&](const defrec::CbSlice& s) {
      w.ring.BindVS(k.st.cbSlot, s);
      for (uint32_t bits = k.psCbMask; bits; bits &= bits - 1) {
        unsigned long slot;
        _BitScanForward(&slot, bits);
        w.ring.BindPS(slot, s);
      }
    };
    if (g.instVs) {
      bind(j.slices[g.first]);
      dc->DrawIndexedInstanced(m.indexCount, g.count, 0, 0, 0);
      ++n;
      continue;
    }
    for (uint32_t r = g.first, q = 0; q < g.count; ++q, r = j.recNext[r]) {
      bind(j.slices[r]);
      dc->DrawIndexed(m.indexCount, 0, 0);
      ++n;
    }
  }
  *draws += n;
  return true;
}

// The whole job on one context (offline tests).
bool RecordJob(defrec::Worker& w, Job& j) {
  return RecordGroups(w, j, 0, j.groupCount, 0, &j.draws, &j.cbBytes);
}

// The helpers (if queued) have nothing (more) to do: stage A is aborted if
// it did not open, and no phase follows. Idempotent.
void ReleaseHelpers(Job& j) {
  int ps = kGoWait;
  if (j.preState.compare_exchange_strong(ps, kGoAbort, std::memory_order_acq_rel)) SetEvent(j.preGo);
  j.recPhase.store(kRpEnd, std::memory_order_seq_cst);
  for (int t = 1; t < kThreads; ++t)
    if (j.helperGo[t]) SetEvent(j.helperGo[t]);
}

bool RecordChunks(defrec::Worker& w, Job& j, int list, uint32_t* draws, uint64_t* cbBytes);

// RecordChunks with the texture table's lock held shared (a helper's
// textured chunks; the primary holds it for the whole job).
bool RecordChunksLocked(defrec::Worker& w, Job& j, int list, uint32_t* draws, uint64_t* cbBytes) {
  bool r = false;
  SRWLOCK* lock = j.texLock;
  if (lock) AcquireSRWLockShared(lock);
  __try {
    r = RecordChunks(w, j, list, draws, cbBytes);
  } __finally {
    if (lock) ReleaseSRWLockShared(lock);
  }
  return r;
}

// Takes chunks of the open phase's groups until none is left (primary: list
// 0, helper t: list t, which then joins the pass). False (and the job marked
// failed) when one cannot be recorded.
bool RecordChunks(defrec::Worker& w, Job& j, int list, uint32_t* draws, uint64_t* cbBytes) {
  for (;;) {
    if (j.chunkFail.load(std::memory_order_acquire)) return false;
    const uint32_t from = j.nextGroup.fetch_add(j.chunk, std::memory_order_acq_rel);
    if (from >= j.texTo) return true;
    const uint32_t to = from + j.chunk < j.texTo ? from + j.chunk : j.texTo;
    if (list) j.joined.fetch_or(1u << list, std::memory_order_relaxed);  // read after recActive drops (seq_cst)
    if (!RecordGroups(w, j, from, to, list, draws, cbBytes)) {
      j.chunkFail.store(true, std::memory_order_release);
      return false;
    }
  }
}

// Phases (primary). A phase's groups [from, to) are shared in chunks with the
// helpers (depth only, GREATER, no blend: the order does not matter; the
// helpers' lists run after the primary's). OpenPhase publishes them (the
// primary's first chunk is its own) and returns that chunk's end; ClosePhase
// returns once no helper is inside the phase (every chunk taken is recorded),
// so the next phase may reuse the chunk counters. A helper enters a phase
// only by raising recActive and then finding it not closed (both seq_cst:
// either the primary sees it inside, or it sees the phase closed).
uint32_t OpenPhase(Job& j, int ph, uint32_t from, uint32_t to) {
  j.texTo = to;
  j.chunk = ChunkFor(to - from, 1 + j.helpers);
  const uint32_t first = from + (j.chunk < to - from ? j.chunk : to - from);
  j.nextGroup.store(first, std::memory_order_relaxed);
  j.recPhase.store(ph, std::memory_order_seq_cst);  // publishes texTo, chunk, the groups
  for (uint32_t t = 1; t <= j.helpers && t < static_cast<uint32_t>(kThreads); ++t) SetEvent(j.helperGo[t]);
  if (g_testP1Wait.load(std::memory_order_relaxed)) {  // offline tests: let every helper take a chunk first
    const uint32_t want = ((1u << (j.helpers + 1)) - 1) & ~1u;
    const int64_t t0 = defrec::Qpc();
    while ((j.joined.load(std::memory_order_acquire) & want) != want &&
           j.nextGroup.load(std::memory_order_acquire) < j.texTo && (defrec::Qpc() - t0) * defrec::QpcToUs() < 1e6)
      SwitchToThread();
  }
  return first;
}

bool ClosePhase(Job& j, int ph) {
  j.recClosed.store(ph, std::memory_order_seq_cst);
  const int64_t t0 = defrec::Qpc();
  const double k = defrec::QpcToUs();
  for (uint32_t spin = 0; j.recActive.load(std::memory_order_seq_cst) != 0; ++spin) {
    if ((defrec::Qpc() - t0) * k > kP1WaitUs) {
      j.chunkFail.store(true, std::memory_order_release);
      return false;
    }
    if (spin < 4096)
      _mm_pause();
    else
      SwitchToThread();
  }
  return !j.chunkFail.load(std::memory_order_acquire);
}

// One phase's groups [from, to) on the primary's list, shared with the
// helpers when there are enough of them (`shared`: OpenPhase already ran and
// returned `first`). Returns once every chunk is recorded. False on a failure.
bool RecordPhase(defrec::Worker& w, Job& j, int ph, uint32_t from, uint32_t to, bool shared, uint32_t first) {
  if (!shared) return RecordGroups(w, j, from, to, 0, &j.draws, &j.cbBytes);
  bool ok = RecordGroups(w, j, from, first, 0, &j.draws, &j.cbBytes);
  if (!ok) j.chunkFail.store(true, std::memory_order_release);
  ok = ok && RecordChunks(w, j, 0, &j.draws, &j.cbBytes);
  return ClosePhase(j, ph) && ok;
}

// A phase of `groups` groups is worth sharing.
inline bool SharePhase(const Job& j, uint32_t groups) {
  return j.helpers && groups >= j.minSplit && groups > kMinChunk;
}

// Starts an armed job on vector `vec` (the sort hook on a pool thread, or the
// render thread at RenderGraph::render entry; the first one wins). False when
// the job was not armed (already started, aborted, idle).
bool StartJob(Job& j, void** vec, bool early) {
  int st = kStartArmed;
  if (!j.startState.compare_exchange_strong(st, kStartRun, std::memory_order_acq_rel)) return false;
  j.vec = vec;
  j.startedEarly = early;
  j.tStart = defrec::Qpc();
  SetEvent(j.start);  // publishes vec to the worker
  return true;
}

bool JobBody(defrec::Worker& w, Job& j);

// From its start to its end the job holds the texture table's lock shared:
// the views it picks stay alive until its lists hold their own references.
// The helpers are let go even when a DCS read faults (the pool latches off).
bool JobMain(defrec::Worker& w, void* u) {
  Job& j = *static_cast<Job*>(u);
  const DWORD started = WaitForSingleObject(j.start, kStartWaitMs);
  int st = kStartArmed;
  // StartJob may win the start between our timeout and this exchange: it
  // stores j.vec after its own exchange and only then sets the event, so
  // wait for that event (bounded) before reading j.vec. Before, the worker
  // went on with j.vec still null (Arm clears it), BeginBuild faulted on
  // j.vec[0] and the recorder latched off for the session (most likely at
  // low fps, where starts land near the 250 ms timeout).
  if (j.startState.compare_exchange_strong(st, kStartAbort, std::memory_order_acq_rel) || st != kStartRun ||
      (started != WAIT_OBJECT_0 && WaitForSingleObject(j.start, kStartLateWaitMs) != WAIT_OBJECT_0)) {
    ReleaseHelpers(j);  // nobody started it: the helpers end too
    j.phase.store(kJobEmpty, std::memory_order_release);
    return false;
  }
  bool r = false;
  SRWLOCK* lock = j.texLock;
  if (lock) AcquireSRWLockShared(lock);
  __try {
    r = JobBody(w, j);
  } __finally {
    ReleaseHelpers(j);
    if (lock) ReleaseSRWLockShared(lock);
  }
  return r;
}

// Primary worker: stage A (reads with the helpers, commit pipelined), phase 1
// (untextured groups, shared; the texture keys are resolved meanwhile), the
// wait for the texture snapshot (no table only), phase 2 (textured groups,
// shared).
bool JobBody(defrec::Worker& w, Job& j) {
  const int64_t t0 = defrec::Qpc();
  const double k = defrec::QpcToUs();
  if (BeginBuild(j) == kBuildOk) {
    if (j.helpers) {  // the helpers (queued with the job) read casters too
      j.preState.store(kGoRun, std::memory_order_release);
      SetEvent(j.preGo);
    }
    PreAndCommit(j);  // j.result stays kBuildNone when a helper stalled
  }
  j.tCommit = defrec::Qpc();
  const bool built = j.result == kBuildOk;
  bool share1 = false;
  uint32_t first1 = 0;
  if (built) {
    // Phase 1's groups need no texture key: the helpers start on them while
    // the primary resolves the keys.
    FinishPhase(j, false, false);
    BuildGroups(j, false, j.instancing);
    j.groupsTex = j.groupCount;
    share1 = SharePhase(j, j.groupsTex);
    if (share1) first1 = OpenPhase(j, kRpUntex, 0, j.groupsTex);
    if (j.tex && j.needGo) {
      const int64_t q0 = defrec::Qpc();
      ResolveTexKeys(j);
      j.needGo = false;  // no snapshot to wait for
      j.resolveUs = (defrec::Qpc() - q0) * k;
    }
  }
  j.tStageA = defrec::Qpc();
  j.buildUs = (j.tStageA - t0) * k;
  if (built && j.needGo && j.snapBell)
    j.snapBell->fetch_or(j.slotBit, std::memory_order_release);  // the render thread snapshots at its next pass boundary
  j.stageA.store(1, std::memory_order_release);
  if (!built) {
    ReleaseHelpers(j);
    j.phase.store(kJobBuilt, std::memory_order_release);
    return false;
  }
  const int64_t r0 = defrec::Qpc();
  bool ok = RecordPhase(w, j, kRpUntex, 0, j.groupsTex, share1, first1);
  j.tP1 = defrec::Qpc();
  j.recordUs = (j.tP1 - r0) * k;
  bool snapshot = j.resolved;
  if (ok && j.needGo) {
    WaitForSingleObject(j.go, kGoWaitMs);
    const int g = j.goState.load(std::memory_order_acquire);
    if (g == kGoAbort) ok = false;
    snapshot = g == kGoRun;
  }
  if (ok) {
    const int64_t r1 = defrec::Qpc();
    FinishPhase(j, true, snapshot);
    BuildGroups(j, true, j.instancing);
    const uint32_t texGroups = j.groupCount - j.groupsTex;
    const bool share2 = SharePhase(j, texGroups);
    const uint32_t first2 = share2 ? OpenPhase(j, kRpTex, j.groupsTex, j.groupCount) : 0;
    ok = RecordPhase(w, j, kRpTex, j.groupsTex, j.groupCount, share2, first2);
    FinishList(j);
    j.recordUs += (defrec::Qpc() - r1) * k;
  }
  ReleaseHelpers(j);  // no further phase
  j.tDone = defrec::Qpc();
  j.phase.store(kJobBuilt, std::memory_order_release);
  return ok && j.recorded > 0 && j.draws > 0;
}

// Helper t: waits for recPhase to move past `seen` (spins kHelperSpinUs,
// then blocks on its own event; bounded from t0). The new phase, or kRpEnd.
int WaitPhase(Job& j, int t, int seen, int64_t t0) {
  const double k = defrec::QpcToUs();
  const double limitMs = kStartWaitMs + kGoWaitMs + 50;
  const int64_t s0 = defrec::Qpc();
  for (;;) {
    const int ph = j.recPhase.load(std::memory_order_seq_cst);
    if (ph != seen) return ph;  // phases only move forward
    const int64_t now = defrec::Qpc();
    if ((now - s0) * k < kHelperSpinUs) {
      _mm_pause();
      continue;
    }
    const double spentMs = (now - t0) * k / 1000.0;
    if (spentMs >= limitMs) return kRpEnd;
    WaitForSingleObject(j.helperGo[t], static_cast<DWORD>(limitMs - spentMs) + 1);
  }
}

// Helper worker T (1..kThreads-1): its share of stage A, then chunks of each
// phase while it is open, on its own list. Every wait is bounded.
template <int T>
bool HelperMain(defrec::Worker& w, void* u) {
  static_assert(T >= 1 && T < kThreads, "helper index");
  Job& j = *static_cast<Job*>(u);
  WaitForSingleObject(j.preGo, kStartWaitMs + 50);
  if (j.preState.load(std::memory_order_acquire) != kGoRun) return false;
  ThreadOut& o = j.th[T];
  PreChunks(j, T);
  const int64_t t0 = defrec::Qpc();
  bool ok = true;
  for (int seen = kRpWait; ok;) {
    const int ph = WaitPhase(j, T, seen, t0);
    if (ph == kRpEnd || ph == kRpWait) break;
    seen = ph;
    j.recActive.fetch_add(1, std::memory_order_seq_cst);
    if (j.recClosed.load(std::memory_order_seq_cst) < ph) {
      const int64_t r0 = defrec::Qpc();
      ok = ph == kRpTex ? RecordChunksLocked(w, j, T, &o.draws, &o.cbBytes) : RecordChunks(w, j, T, &o.draws, &o.cbBytes);
      o.recordUs += (defrec::Qpc() - r0) * defrec::QpcToUs();
    }
    j.recActive.fetch_sub(1, std::memory_order_seq_cst);
  }
  o.drew = ok && o.draws > 0;  // nothing taken: no list, the others drew every chunk
  o.tDone = defrec::Qpc();
  return o.drew;
}
constexpr defrec::JobFn kHelperMain[kThreads] = {nullptr, &HelperMain<1>, &HelperMain<2>, &HelperMain<3>};

// Render thread, at the pass: the job still describes this vector, its pages
// and the CB bytes its keys' shaders read (kPExecuted = 0 when so).
int CheckJobRaw(const Job& j) {
  auto** b = static_cast<void**>(j.vec[0]);
  auto** e = static_cast<void**>(j.vec[1]);
  if (b != j.begin || e < b || static_cast<size_t>(e - b) != j.n || memcmp(j.snap, b, j.n * sizeof(void*)) != 0)
    return kPIdentity;
  for (uint32_t p = 0; p < j.pageCount; ++p)
    if (PageSrv(*j.env, j.pages[p].page) != j.pages[p].srv) return kPPage;
  for (uint32_t m = 0; m < j.matCount; ++m) {
    const RecMat& rm = j.mats[m];
    if (!rm.alive) continue;
    for (int w = 0; w < 2; ++w) {
      uint64_t bits = rm.key->guard[w];
      while (bits) {
        unsigned long d;
        _BitScanForward64(&d, bits);
        bits &= bits - 1;
        const uint32_t off = (static_cast<uint32_t>(w) * 64 + d) * 4;
        if (off + 4 > kCbLen) continue;
        if (memcmp(rm.mat + kCbOff + off, rm.cb + off, 4) != 0) return kPGuard;
      }
    }
  }
  return kPExecuted;
}

int CheckJobGuarded(const Job& j) {
  __try {
    return CheckJobRaw(j);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return kPIdentity;
  }
}

// Render thread, in the pass before the list runs: one streaming request per
// recorded texture (vt[23] with slot 5's size, as slot 26 and SkipSet call
// it). Returns the calls made.
uint32_t ReplayStreamingRaw(const Job& j) {
  for (uint32_t v = 0; v < j.vt23Count; ++v) {
    uint8_t* tex = j.vt23[v];
    (*reinterpret_cast<Tex23Fn**>(tex))[23](tex, kTexSize);
  }
  return j.vt23Count;
}

// Render thread, in the pass after the replay: every recorded texture still
// gives the snapshot's view and has no swap due (kPExecuted = 0 when so).
int CheckTexturesRaw(const Job& j) {
  const gbbatch::TexEnv& env = j.env->tex;
  for (uint32_t k = 0; k < j.texKeyCount; ++k) {
    const TexKey& t = j.texKeys[k];
    if (!t.used) continue;
    if (t.read && j.resolved) {
      // The table's entry is still the one the view came from and still describes the live texture.
      if (!t.e || t.e->version.load(std::memory_order_relaxed) != t.ver ||
          t.e->slot.load(std::memory_order_relaxed) != kTeLive || !TexCheckRaw(env, *t.e)) {
        if (t.e) const_cast<TexEntry*>(t.e)->dirty.store(1, std::memory_order_relaxed);  // rebuilt after the pass
        return kPTexture;
      }
    } else if (t.read) {
      void* v = nullptr;
      if (gbbatch::PredictView(env, t.tex, t.aux, t.type, kTexSize, &v) != gbbatch::kOk || v != t.view)
        return kPTexture;
    } else if (!TexClassOk(env, t.tex) || SwapDue(t.tex, t.aux)) {
      return kPTexture;
    }
  }
  return kPExecuted;
}

// Render thread, after the checks and before the list: slot 26's vt[18] of the
// recorded render-target textures (their one-time mip generation, as their
// first draw would do it). Returns the calls.
uint32_t ReplayRtRaw(const Job& j) {
  if (!g_innerRt) return 0;
  const gbbatch::TexEnv& env = j.env->tex;
  uint32_t n = 0;
  for (uint32_t k = 0; k < j.texKeyCount; ++k) {
    const TexKey& t = j.texKeys[k];
    if (!t.used || *reinterpret_cast<void* const*>(t.tex) != env.texVtbl) continue;
    const void* inner = *reinterpret_cast<void* const*>(t.tex + 0x10);
    if (!inner || *static_cast<void* const*>(inner) != g_innerRt) continue;
    ReplayVt18(t.tex);
    ++n;
  }
  return n;
}

bool ReplayRtGuarded(const Job& j, uint32_t* calls) {
  __try {
    *calls += ReplayRtRaw(j);
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

int ReplayAndCheckGuarded(const Job& j, uint32_t* replays) {
  __try {
    *replays += ReplayStreamingRaw(j);
    return CheckTexturesRaw(j);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return kPTexture;
  }
}

// After an executed pass: mat+0x18c as the stock loop leaves it.
void RestoreMatsRaw(const Job& j) {
  for (uint32_t m = 0; m < j.matCount; ++m) {
    const RecMat& rm = j.mats[m];
    if (rm.last < 0 || !rm.alive) continue;
    auto* item = *reinterpret_cast<uint8_t**>(static_cast<uint8_t*>(j.snap[rm.last]) + 0x10);
    *reinterpret_cast<uint32_t*>(rm.mat + 0x18c) = *reinterpret_cast<uint32_t*>(item + 0xd4);
  }
}

bool RestoreMatsGuarded(const Job& j) {
  __try {
    RestoreMatsRaw(j);
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

// ---------------------------------------------------------------------------
// Probes: DCS's own draw, read back
// ---------------------------------------------------------------------------
struct Capture {
  int draws = 0;
  int prim = 0, a4 = 0, a5 = 0, instances = 0;  // DX11Renderer::draw arguments
  uint32_t flags = 0, dbg = 0;                   // renderer+0xd4, +0x2120 at the draw
  bool rendererOk = false;
  UINT classInstances = 0;
  ID3D11VertexShader* vs = nullptr;
  ID3D11PixelShader* ps = nullptr;
  ID3D11GeometryShader* gs = nullptr;
  ID3D11HullShader* hs = nullptr;
  ID3D11DomainShader* ds = nullptr;
  ID3D11InputLayout* il = nullptr;
  ID3D11Buffer* vb = nullptr;
  UINT stride = 0, vbOffset = 0;
  ID3D11Buffer* ib = nullptr;
  DXGI_FORMAT ibFormat = DXGI_FORMAT_UNKNOWN;
  UINT ibOffset = 0;
  D3D11_PRIMITIVE_TOPOLOGY topo = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
  ID3D11DepthStencilState* dss = nullptr;
  UINT stencilRef = 0;
  ID3D11BlendState* bs = nullptr;
  float blendFactor[4] = {};
  UINT sampleMask = 0;
  ID3D11RasterizerState* rs = nullptr;
  ID3D11Buffer* b7 = nullptr;
  ID3D11Buffer* cb = nullptr;
  ID3D11ShaderResourceView* sb = nullptr;
  // PS stage (references held).
  ID3D11Buffer* psCb[kCbSlots] = {};
  ID3D11ShaderResourceView* psSrv[kSrvSlots] = {};
  ID3D11SamplerState* psSamp[kSampSlots] = {};
  // The drawing DX11Shader's stream state at the draw (ReadStreams).
  bool streamsOk = false;
  uint64_t streamCount = 0;      // vertex buffer objects bound: IASetVertexBuffers(0, count, ...)
  void* stream0 = nullptr;       // the first one
  uint32_t descCount = 0;        // packed element descriptors the input layout is built from
  bool descSlot0 = false;        // every descriptor: input slot 0, per-vertex data
};

// The stream state of the DX11Shader that DX11Renderer::draw just drew with
// (its argument, = renderer+0xe0) [V dx11backend]: vt[38] 0x1dbc0 appends the
// mesh's vertex buffer object to the vector at +0x1a8 (count +0x1b0) unless
// present, and packs one dword per element at +0x114[i] (count +0x110):
// ((fmtA << 12 | fmtB) << 9 | semantic) | (slot | (a6 == 1 ? 0x8000 : 0)) <<
// 16, slot = the buffer's index in the vector [V 0x1dcf7..0x1dd54]; vt[37]
// 0x1dd90 puts slot and the instance flag at the same bits. The draw's
// setVertexBuffers 0x1ad10 takes the input layout for these descriptors
// (0x43c30) and binds IASetVertexBuffers(0, count, [obj+0x130], [obj+0x150],
// 0). One buffer object and every descriptor at slot 0, per-vertex, so the
// layout reads input slot 0 only, and slot 0 holds the mesh's buffer. The
// submit's vt[6] 0x1e970 zeroes all of it right after the draw [V
// 0x1e985..0x1e99a]: it is read inside the draw (DrawAfter). The first probes
// read it after the caster's call and saw 0 streams [M 2026-10-09].
// Plain (callers hold SEH).
inline void ReadStreams(const uint8_t* sh, Capture& c) {
  c.streamCount = *reinterpret_cast<const uint64_t*>(sh + 0x1b0);
  const auto* list = *reinterpret_cast<void* const* const*>(sh + 0x1a8);
  c.stream0 = c.streamCount && list ? list[0] : nullptr;
  c.descCount = *reinterpret_cast<const uint32_t*>(sh + 0x110);
  c.descSlot0 = c.descCount > 0 && c.descCount <= 32;
  for (uint32_t i = 0; c.descSlot0 && i < c.descCount; ++i) {
    const uint32_t v = *reinterpret_cast<const uint32_t*>(sh + 0x114 + 4 * i);
    if ((v >> 16) & 0x801f) c.descSlot0 = false;  // input slot != 0, or per-instance
  }
  c.streamsOk = true;
}

bool ReadStreamsGuarded(const uint8_t* sh, Capture& c) {
  __try {
    ReadStreams(sh, c);
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    c.streamsOk = false;
    return false;
  }
}

template <class S, class Get>
UINT GetShaderInto(Get get, S** out) {
  ID3D11ClassInstance* inst[256] = {};
  UINT n = 256;
  get(out, inst, &n);
  for (UINT i = 0; i < n && i < 256; ++i) SafeRel(inst[i]);
  return n;
}

// Everything DCS's draw bound that a depth-only draw of its shaders depends
// on (references held until ReleaseCapture).
void CaptureState(ID3D11DeviceContext* c, UINT cbSlot, UINT sbSlot, Capture& k) {
  k.classInstances += GetShaderInto(
      [&](ID3D11VertexShader** s, ID3D11ClassInstance** i, UINT* n) { c->VSGetShader(s, i, n); }, &k.vs);
  k.classInstances += GetShaderInto(
      [&](ID3D11PixelShader** s, ID3D11ClassInstance** i, UINT* n) { c->PSGetShader(s, i, n); }, &k.ps);
  GetShaderInto([&](ID3D11GeometryShader** s, ID3D11ClassInstance** i, UINT* n) { c->GSGetShader(s, i, n); }, &k.gs);
  GetShaderInto([&](ID3D11HullShader** s, ID3D11ClassInstance** i, UINT* n) { c->HSGetShader(s, i, n); }, &k.hs);
  GetShaderInto([&](ID3D11DomainShader** s, ID3D11ClassInstance** i, UINT* n) { c->DSGetShader(s, i, n); }, &k.ds);
  c->IAGetInputLayout(&k.il);
  c->IAGetVertexBuffers(0, 1, &k.vb, &k.stride, &k.vbOffset);
  c->IAGetIndexBuffer(&k.ib, &k.ibFormat, &k.ibOffset);
  c->IAGetPrimitiveTopology(&k.topo);
  c->OMGetDepthStencilState(&k.dss, &k.stencilRef);
  c->OMGetBlendState(&k.bs, k.blendFactor, &k.sampleMask);
  c->RSGetState(&k.rs);
  c->VSGetConstantBuffers(kB7, 1, &k.b7);
  if (cbSlot < kCbSlots) c->VSGetConstantBuffers(cbSlot, 1, &k.cb);
  if (sbSlot < kSrvSlots) c->VSGetShaderResources(sbSlot, 1, &k.sb);
  if (k.ps) {
    c->PSGetConstantBuffers(0, kCbSlots, k.psCb);
    c->PSGetShaderResources(0, kSrvSlots, k.psSrv);
    c->PSGetSamplers(0, kSampSlots, k.psSamp);
  }
}

void ReleaseCapture(Capture& k) {
  SafeRel(k.vs), SafeRel(k.ps), SafeRel(k.gs), SafeRel(k.hs), SafeRel(k.ds), SafeRel(k.il), SafeRel(k.vb);
  SafeRel(k.ib), SafeRel(k.dss), SafeRel(k.bs), SafeRel(k.rs), SafeRel(k.b7), SafeRel(k.cb), SafeRel(k.sb);
  for (auto*& p : k.psCb) SafeRel(p);
  for (auto*& p : k.psSrv) SafeRel(p);
  for (auto*& p : k.psSamp) SafeRel(p);
  k = Capture();
}

// What DCS's draw must have used, from DCS memory and the pass.
struct ProbeFacts {
  void* dcsVs = nullptr;  // FX pass 0 VS ([[fxpass+0xc0]+0x18]), equal to shadow_inst's map entry's
  ID3D11Buffer* passB7 = nullptr;  // VS b7 at the pass's first caster (the binder's)
  ID3D11ShaderResourceView* pageSrv = nullptr;
  MeshLive mesh;
  const char* meshWhy = nullptr;
  // The caster's texture sets the shadow pass reads (texture-skip mask), with
  // the views slot 26 just bound (predicted after the draw, render thread).
  int readCount = 0;
  bool readOk = true;  // every read set predictable (else readCount is meaningless)
  int64_t readH[kMaxPsTex] = {};
  void* readView[kMaxPsTex] = {};
  const char* namesWhy = nullptr;  // the pass reads a name that is no material texture, CB, sbPositions or sampler
  // The slots FX Apply sets for the PS (FxStageDeps; not read: every slot counts).
  bool depsOk = false;
  uint32_t psSampDeps = 0xffff, psCbDeps = 0x3fff;
};

// The PS side of a key, from one probe.
struct PsMap {
  int count = 0;
  int64_t h[kMaxPsTex] = {};
  UINT slot[kMaxPsTex] = {};
  uint32_t cbMask = 0;
  uint32_t sampMask = 0xffff;  // the PS sampler slots that count (FX dependencies, else all)
};

const char* const kInconclusive = "a pixel shader is bound but the caster sets no read texture";

// nullptr, kInconclusive (publish nothing yet), or why the key's draws are not
// recordable.
const char* CheckKey(const Capture& c, const ProbeFacts& f, PsMap* ps) {
  if (c.classInstances) return "class instances are bound";
  if (!c.vs || c.vs != f.dcsVs) return "the bound VS is not the FX pass's (or not shadow inst's)";
  if (c.gs || c.hs || c.ds) return "a GS, HS or DS is bound";
  if (!c.b7 || c.b7 != f.passB7) return "VS b7 is not the pass's per-view buffer";
  if (!c.cb) return "no def_uniforms buffer is bound";
  if (!c.sb || c.sb != f.pageSrv) return "sbPositions is not the page buffer's view at +0x30";
  *ps = PsMap();
  if (!f.readOk) return kInconclusive;  // a read texture without a predictable view: another caster may map it
  if (!c.ps) return f.readCount ? "textures are read without a pixel shader" : nullptr;
  if (f.namesWhy) return f.namesWhy;
  if (!f.readCount) return kInconclusive;
  if (f.readCount > kMaxPsTex) return "more read textures than recorded per caster";
  if (c.psCb[kB7] && c.psCb[kB7] != f.passB7) return "PS b7 is not the pass's per-view buffer";
  for (int k = 0; k < f.readCount; ++k) {
    if (!f.readView[k]) return kInconclusive;  // no view (compat): its slot cannot be found from this caster
    int hits = 0;
    UINT slot = 0;
    for (UINT s = 0; s < kSrvSlots; ++s)
      if (c.psSrv[s] == f.readView[k]) ++hits, slot = s;
    if (hits == 0) return "a read texture's view is not bound at any PS slot";
    if (hits > 1) return kInconclusive;  // the same view also at another slot (an earlier draw's)
    for (int q = 0; q < k; ++q)
      if (ps->slot[q] == slot) return "two read textures share a PS slot";
    ps->h[k] = f.readH[k];
    ps->slot[k] = slot;
  }
  ps->count = f.readCount;
  ps->sampMask = f.depsOk ? f.psSampDeps : 0xffff;
  for (UINT s = 0; s < kCbSlots; ++s)
    if (c.psCb[s] && c.psCb[s] == c.cb && (!f.depsOk || ((f.psCbDeps >> s) & 1))) ps->cbMask |= 1u << s;
  return nullptr;
}

// nullptr, or why this mesh's draws (with this key) are not recordable.
const char* CheckMesh(const Capture& c, const ProbeFacts& f) {
  if (f.meshWhy) return f.meshWhy;
  if (c.prim != static_cast<int>(f.mesh.prim) || c.a4 != 0 || static_cast<uint32_t>(c.a5) != f.mesh.count)
    return "draw arguments differ from the mesh fields";
  if (c.instances != 0) return "instanced draw";
  if (c.topo != D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST) return "topology is not a triangle list";
  if (c.ib != f.mesh.ib || c.ibFormat != f.mesh.ibFormat || c.ibOffset != 0)
    return "the bound index buffer is not the mesh's";
  if (c.vb != f.mesh.vb || c.stride != f.mesh.stride || c.vbOffset != 0)
    return "the bound vertex buffer is not the mesh's";
  if (!c.streamsOk) return "the drawing shader's stream state is not readable";
  if (c.streamCount == 0) return "no vertex stream in the drawing shader's list";
  if (c.streamCount > 1) return "more than one vertex stream bound";
  if (c.stream0 != f.mesh.vbObj) return "the stream is not the mesh's vertex buffer object";
  if (c.descCount != f.mesh.elems) return "element descriptor count differs from the mesh's";
  if (!c.descSlot0) return "an element reads another input slot or per-instance data";
  if (!c.il) return "no input layout";
  return nullptr;
}

UINT SampCount(ID3D11SamplerState* const* s) {
  UINT n = kSampSlots;
  while (n && !s[n - 1]) --n;
  return n;
}

// What differs between a key's objects and another probe of it (0: same).
// Samplers count only at the slots FX Apply sets for the PS (both probes'
// dependencies; every slot when unknown): other slots hold whatever earlier
// draws left there, which depends on the draw history (the view).
enum : uint32_t {
  kDVs = 1, kDPs = 2, kDDepth = 4, kDStencilRef = 8, kDBlend = 16, kDBlendFactor = 32, kDSampleMask = 64,
  kDRaster = 128, kDSamplers = 256, kDCbSlots = 512, kDTexSlots = 1024, kDVsSamplers = 2048, kDiffBits = 12
};
const char* const kDiffName[kDiffBits] = {"VS", "PS", "depth-stencil state", "stencil ref", "blend state",
                                          "blend factor", "sample mask", "rasterizer state", "PS samplers",
                                          "PS slots of the material CB", "PS texture slots", "VS samplers"};
uint32_t KeyDiff(const KeyEntry& k, const Capture& c, const PsMap& ps, uint32_t* sampSlots) {
  uint32_t d = 0;
  d |= k.vs != c.vs ? kDVs : 0;
  d |= k.ps != c.ps ? kDPs : 0;
  d |= k.dss != c.dss ? kDDepth : 0;
  d |= k.stencilRef != c.stencilRef ? kDStencilRef : 0;
  d |= k.bs != c.bs ? kDBlend : 0;
  d |= memcmp(k.blendFactor, c.blendFactor, sizeof(c.blendFactor)) != 0 ? kDBlendFactor : 0;
  d |= k.sampleMask != c.sampleMask ? kDSampleMask : 0;
  d |= k.rs != c.rs ? kDRaster : 0;
  *sampSlots = 0;
  if (!c.ps || !k.ps) return d;
  // Pool slots count per pass (each probe checks them against the pool), not per key.
  const uint32_t mask = KeyOwnSamplers(k.psSampDeps & ps.sampMask);
  for (UINT q = 0; q < kSampSlots; ++q)
    if (((mask >> q) & 1) && k.psSamp[q] != c.psSamp[q]) *sampSlots |= 1u << q;
  d |= *sampSlots ? kDSamplers : 0;
  d |= k.psCbMask != ps.cbMask ? kDCbSlots : 0;
  bool tex = k.psTexCount != ps.count;
  for (int p = 0; p < ps.count && !tex; ++p) {
    int q = 0;
    while (q < k.psTexCount && k.psTexH[q] != ps.h[p]) ++q;
    tex = q == k.psTexCount || k.psTexSlot[q] != ps.slot[p];
  }
  d |= tex ? kDTexSlots : 0;
  return d;
}

std::string DiffText(uint32_t d, uint32_t sampSlots) {
  std::string out;
  for (uint32_t b = 0; b < kDiffBits; ++b)
    if ((d >> b) & 1) {
      if (!out.empty()) out += ", ";
      out += kDiffName[b];
      if ((1u << b) == kDSamplers || (1u << b) == kDVsSamplers) {
        char buf[24];
        snprintf(buf, sizeof(buf), " (slots 0x%x)", sampSlots);
        out += buf;
      }
    }
  return out;
}

// What the probed caster is.
struct ProbeIn {
  uint8_t* item = nullptr;
  uint8_t* mat = nullptr;
  uint8_t* shader = nullptr;
  uint64_t tech = 0;
  uint8_t* mesh = nullptr;
  uint32_t page = 0;
  void* effect = nullptr;
  void* techBegin = nullptr;
};

std::atomic<uint32_t> g_logBudget{24};
std::atomic<uint64_t> g_keysOk{0}, g_keysRejected{0}, g_keyDowngrades{0}, g_meshesOk{0}, g_meshesRejected{0},
    g_tableFull{0}, g_probeInconclusive{0}, g_keyRelearnt{0};
std::atomic<uint32_t> g_diffLogBudget{32};

void LogReject(const char* what, const ProbeIn& in, uint32_t flags, const char* why) {
  if (g_logBudget.load() == 0) return;
  g_logBudget.fetch_sub(1);
  Log("shadow recorder: %s not recordable: %s (shader %p, technique %llu, flags 0x%x, mesh %p)", what, why, in.shader,
      static_cast<unsigned long long>(in.tech), flags, in.mesh);
}

// Publishes what one probe learned (inserting thread: the render thread, or
// the offline tests). The key first: its static facts (st) and the capture;
// the mesh only under a recordable key. Returns false when inconclusive.
bool ProcessProbe(Tables& t, const ProbeIn& in, const KeyStatic& st, const ProbeFacts& f, const Capture& c,
                  uint32_t now = 0) {
  KeyEntry* k = const_cast<KeyEntry*>(FindKey(t, in.shader, in.tech, c.flags, in.effect, in.techBegin));
  if (!k && KeyCooling(t, in.shader, in.tech, c.flags, in.effect, in.techBegin, now, false)) return true;
  PsMap ps;
  const char* why = st.why ? st.why : CheckKey(c, f, &ps);
  const bool inconclusive = why == kInconclusive;
  if (inconclusive) {
    g_probeInconclusive++;
    if (!k || k->state.load() <= 0) return false;
    why = nullptr;  // a recordable key: its PS map came from another caster; the mesh still applies
  }
  if (!k) {
    const bool relearn = KeyCooling(t, in.shader, in.tech, c.flags, in.effect, in.techBegin, now, true);
    k = NewKey(t, in.shader, in.tech, c.flags);
    if (!k) {
      g_tableFull++;
      return true;
    }
    if (relearn) g_keyRelearnt++;
    k->tech = in.tech;
    k->flags = c.flags;
    k->effect = in.effect;
    k->techBegin = in.techBegin;
    k->st = st;
    k->why = why;
    if (!why) {
      if ((k->vs = c.vs)) k->vs->AddRef();
      if ((k->dss = c.dss)) k->dss->AddRef();
      k->stencilRef = c.stencilRef;
      if ((k->bs = c.bs)) k->bs->AddRef();
      memcpy(k->blendFactor, c.blendFactor, sizeof(c.blendFactor));
      k->sampleMask = c.sampleMask;
      if ((k->rs = c.rs)) k->rs->AddRef();
      if ((k->ps = c.ps)) {
        k->ps->AddRef();
        for (UINT s = 0; s < kSampSlots; ++s)
          if ((k->psSamp[s] = c.psSamp[s])) k->psSamp[s]->AddRef();
        k->psSampCount = SampCount(c.psSamp);
        k->psSampDeps = ps.sampMask;
        k->psCbMask = ps.cbMask;
        k->psTexCount = ps.count;
        for (int p = 0; p < ps.count; ++p) k->psTexH[p] = ps.h[p], k->psTexSlot[p] = ps.slot[p];
      }
      k->guard[0] = st.used[0];
      k->guard[1] = st.used[1];
      if (k->ps) AllCbDwords(k->guard);
    }
    k->state.store(why ? -1 : 1, std::memory_order_relaxed);
    PublishKey(t, *k, in.shader);
    if (why) {
      g_keysRejected++;
      LogReject("key", in, c.flags, why);
    } else {
      g_keysOk++;
    }
  } else if (!inconclusive && k->state.load() > 0) {
    uint32_t sampSlots = 0;
    const uint32_t diff = why ? 0 : KeyDiff(*k, c, ps, &sampSlots);
    if (why || diff) {
      // Another draw of a recordable key bound something else: retired (its
      // draws are drawn by DCS), relearnt by a probe after the cooldown.
      k->why = why ? why : "state objects differ between draws of the key";
      k->retiredAt = now + 1;
      k->state.store(kRetired, std::memory_order_release);
      g_keyDowngrades++;
      if (g_diffLogBudget.load() > 0) {
        g_diffLogBudget.fetch_sub(1);
        Log("shadow recorder: key retired (relearnt after %u render entries): %s%s (shader %p, technique %llu, flags "
            "0x%x, stencil ref %u -> %u, PS sampler slots that count 0x%x / 0x%x)",
            kKeyCooldown, why ? why : "differs: ", why ? "" : DiffText(diff, sampSlots).c_str(), in.shader,
            static_cast<unsigned long long>(in.tech), c.flags, k->stencilRef, c.stencilRef, k->psSampDeps,
            ps.sampMask);
      }
      return true;
    }
  }
  if (k->state.load() <= 0 || FindMesh(t, in.mesh, in.shader, in.tech, in.effect, in.techBegin)) return true;
  const char* mwhy = CheckMesh(c, f);
  MeshEntry* m = NewMesh(t, in.mesh, in.shader, in.tech);
  if (!m) {
    g_tableFull++;
    return true;
  }
  m->shader = in.shader;
  m->tech = in.tech;
  m->effect = in.effect;
  m->techBegin = in.techBegin;
  m->why = mwhy;
  if (!mwhy) {
    m->fp = f.mesh.fp;
    if ((m->il = c.il)) m->il->AddRef();
    m->vb = f.mesh.vb;
    m->stride = f.mesh.stride;
    m->ib = f.mesh.ib;
    m->ibFormat = f.mesh.ibFormat;
    m->indexCount = 3 * f.mesh.count;
  }
  m->state.store(mwhy ? -1 : 1, std::memory_order_relaxed);
  PublishMesh(t, *m, in.mesh);
  if (mwhy) {
    g_meshesRejected++;
    LogReject("mesh", in, c.flags, mwhy);
  } else {
    g_meshesOk++;
  }
  return true;
}

// The caster's texture sets the shadow pass reads, with the views slot 26
// bound (render thread, right after its draw). Plain (callers hold SEH).
void ReadSetsRaw(const gbbatch::TexEnv& env, const ProbeIn& in, const shadowtex::MaskEntry& mask, ProbeFacts& f) {
  f.readCount = 0;
  f.readOk = true;
  auto* props = *reinterpret_cast<uint8_t**>(in.mat + 0x28);
  auto* arr = *reinterpret_cast<uint8_t***>(in.item + 0x18);
  if (!props || !arr || !*arr) return;
  const uint32_t n = *reinterpret_cast<uint32_t*>(in.mat + 0x2d8);
  const uint8_t* recs = *reinterpret_cast<uint8_t**>(in.shader + 0xc8);
  const uint8_t* recEnd = *reinterpret_cast<uint8_t**>(in.shader + 0xd0);
  const int64_t nrec = recs && recEnd > recs ? (recEnd - recs) / 0x50 : 0;
  const uint8_t* en = *arr + static_cast<size_t>(*reinterpret_cast<uint32_t*>(props + 0x26c)) * 0x18;
  for (uint32_t i = 0; i < n && i < 32; ++i, en += 0x18) {
    const int64_t h = *reinterpret_cast<int64_t*>(in.mat + 0x240 + 8 * i);
    if (h == -1 || shadowtex::Skippable(mask, h)) continue;
    auto* tex = *reinterpret_cast<uint8_t* const*>(en + 8);
    if (!tex || h < 0 || h >= nrec || f.readCount == kMaxPsTex) {
      f.readOk = false;
      return;
    }
    const int32_t type = *reinterpret_cast<const int32_t*>(recs + h * 0x50 + 0xc);
    void* v = nullptr;
    if (gbbatch::PredictView(env, tex, *reinterpret_cast<const int64_t*>(en), type, kTexSize, &v) != gbbatch::kOk) {
      f.readOk = false;
      return;
    }
    f.readH[f.readCount] = h;
    f.readView[f.readCount++] = v;
  }
}

bool ReadSetsGuarded(const gbbatch::TexEnv& env, const ProbeIn& in, const shadowtex::MaskEntry& mask, ProbeFacts& f) {
  __try {
    ReadSetsRaw(env, in, mask, f);
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    f.readOk = false;
    return false;
  }
}

// ---------------------------------------------------------------------------
// Runtime state
// ---------------------------------------------------------------------------
std::atomic<bool> g_on{false};      // [Model] ShadowRecorder, the kill switch, bench mode 28
std::atomic<bool> g_verify{false};  // [Suite] ShadowRecVerify
std::atomic<uint32_t> g_scope{kDefaultScope};
std::atomic<int> g_state{0};        // 0 = not tried (retried), 1 = ready, -1 = unavailable
std::atomic<bool> g_disabled{false};
// Passes drawn from a command list (monotonic; the status word reads it to
// tell a working recorder from an idle one).
std::atomic<uint64_t> g_workPasses{0};
std::atomic<bool> g_shutdown{false};
std::atomic<int> g_inside{0};
bool g_chained = false;
shadowpass::WrapFn g_prevWrap = nullptr;
instcount::OverrideFn g_prevOverride = nullptr;
DWORD g_renderTid = 0;
Env g_env;
Tables g_tab;
defrec::Pool g_pool;  // worker Wk(s, t): cascade s's primary (t 0) and helpers (t 1..kThreads-1)
inline int Wk(int s, int t) { return s + kSlots * t; }
inline bool SlotBusy(int s) {
  for (int t = 0; t < kThreads; ++t)
    if (g_pool.Busy(Wk(s, t))) return true;
  return false;
}
ID3D11DeviceContext* g_ctx = nullptr;
ID3D11Buffer* g_offBuf[kThreads * kSlots] = {};               // t127 per worker
ID3D11ShaderResourceView* g_offSrv[kThreads * kSlots] = {};
// [Model] ShadowRecorderWaitUs: the render thread's wait for a job at the pass
// (longer: the pass is drawn stock and counted late); ShadowRecorderPriority:
// worker thread priority (-2..2, THREAD_PRIORITY_*); ShadowRecorderSplit:
// cascades whose jobs are shared with helper workers (bits 0-3);
// ShadowRecorderHelpers: at most this many helpers per job (0-3; the count
// follows the cascade's casters and lateness, HelpersFor);
// ShadowRecorderInstancing: groups as one instanced draw.
std::atomic<uint32_t> g_waitUs{200};
std::atomic<int> g_priority{THREAD_PRIORITY_NORMAL};
std::atomic<uint32_t> g_split{0xf};
std::atomic<uint32_t> g_helpers{kThreads - 1};
std::atomic<bool> g_instancing{true};
int g_priorityApplied = 0x7fff;
// Timeline (render thread): this render entry, and top-level passes since.
int64_t g_entryQpc = 0;
uint32_t g_topCount = 0;
uint32_t g_entryGen = 0;  // RenderGraph::render entries (render thread)
std::atomic<uint32_t> g_snapBell{0};  // slots whose stage A finished with textured casters (workers set)
int64_t g_cascEntryQpc = 0;  // the last entry of the cascades' RenderGraph (render thread)
int64_t g_lastPassQpc = 0;   // the end of the last cascade pass (render thread)

void Disable(const char* why) {
  if (!g_disabled.exchange(true)) Log("shadow recorder: disabled for this session (%s)", why);
}

// The exec entry handed to DCS's loop first (S6): an object whose vtable's
// slot 1 runs the cascade's list. Only vt[1] is ever called on loop entries
// [V GC 0xa5640]; the other slots point at a no-op.
struct ExecObj {
  void** vtbl;
  int slot;
};
using Vt1Fn = uint64_t(__fastcall*)(void* self, void* ctx);
uint64_t __fastcall ExecVt1(void* self, void* ctx);
uint64_t __fastcall ExecNop(void*, void*) { return 0; }
void* g_execVtbl[8];
ExecObj g_execObj[kSlots];

// Per recorded cascade (render thread unless noted).
struct Learned {
  bool cascade = false;  // render graph and collection index
  void* rg = nullptr;
  uint32_t idx = 0;
  bool target = false;   // the pass state below
  ID3D11DepthStencilView* dsv = nullptr;  // reference
  UINT nvp = 0, nsc = 0;
  D3D11_VIEWPORT vp[kVp] = {};
  D3D11_RECT sc[kVp] = {};
  uint32_t flags = 0;
  ID3D11SamplerState* pool[kSampSlots] = {};  // PS s5-s15 (references)
};
struct Stat {
  std::atomic<uint64_t> passes{0}, passReason[kPassReasons] = {}, casters{0}, casterReason[kCasterReasons] = {};
  std::atomic<uint64_t> recorded{0}, textured{0}, jobs{0}, jobsUsed{0}, refused{0}, cbBytes{0}, vt23{0};
  std::atomic<uint64_t> draws{0}, drawsHelper{0}, groups{0}, splitJobs{0}, helperLate{0}, preHelper{0}, casterReads{0};
  std::atomic<uint64_t> buildNs{0}, recordNs{0}, waitNs{0}, waitMaxNs{0}, checkNs{0}, execNs{0}, restoreNs{0};
  std::atomic<uint64_t> replayNs{0}, executeNs{0};
  // Timeline from RenderGraph::render entry (jobs used at a pass): signed sums in ns (negative: before entry).
  std::atomic<uint64_t> tlSamples{0}, tlSnapSamples{0}, tlHelperSamples{0}, tlTop{0}, early{0};
  std::atomic<int64_t> tlPass{0}, tlStart{0}, tlStageA{0}, tlSnap{0}, tlDone{0}, tlHelper{0};
  // Late jobs (pass drawn stock, the job finished later): same timeline, read when the cascade is re-armed.
  std::atomic<uint64_t> lateSamples{0};
  std::atomic<int64_t> latePass{0}, lateStart{0}, lateStageA{0}, lateDone{0}, lateP1{0}, lateSnap{0};
  std::atomic<uint64_t> lateSnapSamples{0};
  std::atomic<int64_t> tlP1{0};  // untextured groups recorded (jobs in time)
  std::atomic<int64_t> tlCommit{0}, lateCommit{0};  // stage A committed (before the keys' resolve)
  std::atomic<uint64_t> helpersQueued{0}, commitNs{0}, resolveNs{0};  // jobs used: helpers, primary commit/resolve time
  std::atomic<uint64_t> snapEarly{0}, snapLate{0}, snapKeys{0}, snapPre{0}, rearmLate{0}, staleStart{0};
  std::atomic<uint64_t> verifyPasses{0}, verifyMismatch{0}, verifyTexels{0}, verifyBadTexels{0}, verifyErrors{0},
      verifySkipped{0};
};
struct Casc {
  Learned learn;
  Job* job = nullptr;
  bool armed = false;       // the job was submitted and not consumed yet (render thread)
  uint32_t passGen = 0;     // the render entry whose cascade pass ran last
  ID3D11Buffer* b7 = nullptr;  // our per-view buffer (DEFAULT), copied from DCS's before each execute
  UINT b7Bytes = 0;
  int64_t lateEntry = 0, latePassQpc = 0;  // the render entry and pass time of a late job (0: none)
  bool lateMaint = false;  // the late job's table maintenance is still due
  uint32_t boost = 0;      // jobs left with one more helper (a recent job was late or tight)
  Stat st;
};
Casc g_casc[kSlots];
std::atomic<uint64_t> g_probes{0}, g_probeOdd{0}, g_probeLeaders{0}, g_faults{0}, g_probePool{0};
// Texture table (the render thread maintains it; workers read it under the lock, shared).
TexTable g_tex;
SRWLOCK g_texLock = SRWLOCK_INIT;
constexpr uint32_t kTexEvictAge = 600;  // render entries without use
std::atomic<uint64_t> g_texBuilt{0}, g_texRefreshed{0}, g_texDeferred{0}, g_texFull{0}, g_rtReplays{0};
uint64_t g_texWhy[kTwCount] = {};
// Pending maintenance (render thread): the finished jobs' misses and stale entries.
TexMiss g_pendMiss[kMaxTexKeys];
const TexEntry* g_pendStale[kMaxTexKeys];
uint32_t g_pendMissCount = 0, g_pendStaleCount = 0, g_pendGen = 0;
bool g_pendEvict = false;

// A finished job's misses and stale entries, queued (render thread, its workers idle).
void QueueMaint(const Job& j) {
  for (uint32_t k = 0; k < j.missCount && g_pendMissCount < kMaxTexKeys; ++k) g_pendMiss[g_pendMissCount++] = j.misses[k];
  for (uint32_t k = 0; k < j.refreshCount && g_pendStaleCount < kMaxTexKeys; ++k)
    g_pendStale[g_pendStaleCount++] = j.refresh[k];
  for (uint32_t k = 0; k < j.texKeyCount && g_pendStaleCount < kMaxTexKeys; ++k)
    if (j.texKeys[k].e && j.texKeys[k].e->dirty.load(std::memory_order_relaxed))
      g_pendStale[g_pendStaleCount++] = j.texKeys[k].e;
  g_pendGen = j.useGen;
  g_pendEvict = true;
}

// Builds the queued misses and rebuilds the queued entries when no job holds
// the lock (render thread: after the cascade passes, after every top-level
// pass, at the render entry).
void TryMaintain() {
  if ((!g_pendMissCount && !g_pendStaleCount && !g_pendEvict) || !g_tex.e) return;
  if (!TryAcquireSRWLockExclusive(&g_texLock)) {
    g_texDeferred++;
    return;
  }
  const gbbatch::TexEnv& env = g_env.tex;
  if (g_tex.live + g_tex.tomb >= g_tex.size * 3 / 4) TexWipe(g_tex);  // rare: rebuilt on demand
  for (uint32_t k = 0; k < g_pendMissCount; ++k) {
    const TexMiss& m = g_pendMiss[k];
    bool fresh = false;
    TexEntry* e = TexInsert(g_tex, m.tex, m.aux, m.type, &fresh);
    if (!e) {
      g_texFull++;
      break;
    }
    if (!fresh && !e->dirty.load(std::memory_order_relaxed)) continue;
    const uint8_t why = TexBuildGuarded(env, *e, &g_census);
    g_texWhy[why < kTwCount ? why : kTwFault]++;
    e->dirty.store(why == kTwOk ? 0 : 1, std::memory_order_relaxed);
    e->lastUse.store(g_pendGen, std::memory_order_relaxed);
    g_texBuilt++;
  }
  for (uint32_t k = 0; k < g_pendStaleCount; ++k) {
    TexEntry& e = *const_cast<TexEntry*>(g_pendStale[k]);
    if (e.slot.load(std::memory_order_relaxed) != kTeLive) continue;
    const uint8_t why = TexBuildGuarded(env, e, &g_census);
    g_texWhy[why < kTwCount ? why : kTwFault]++;
    e.dirty.store(why == kTwOk ? 0 : 1, std::memory_order_relaxed);
    g_texRefreshed++;
  }
  if (g_pendEvict) TexEvict(g_tex, g_pendGen, kTexEvictAge, 256);
  g_pendMissCount = g_pendStaleCount = 0;
  g_pendEvict = false;
  ReleaseSRWLockExclusive(&g_texLock);
}

void NoteMax(std::atomic<uint64_t>& a, uint64_t v) {
  uint64_t cur = a.load();
  while (v > cur && !a.compare_exchange_weak(cur, v)) {
  }
}
uint64_t NsSince(int64_t t0) { return static_cast<uint64_t>((defrec::Qpc() - t0) * defrec::QpcToUs() * 1000.0); }

bool Active() {
  return g_on.load(std::memory_order_relaxed) && !g_disabled.load(std::memory_order_relaxed) &&
         !g_shutdown.load(std::memory_order_relaxed) && g_state.load(std::memory_order_relaxed) == 1 &&
         !gbbatch::g_on.load(std::memory_order_relaxed) && !shadowbatch::g_verify.load(std::memory_order_relaxed) &&
         !shadowrec::g_chained.load(std::memory_order_relaxed);
}

// ---------------------------------------------------------------------------
// Render thread: pass target
// ---------------------------------------------------------------------------
struct Target {
  ID3D11DepthStencilView* dsv;  // reference
  UINT rtvs, nvp, nsc;
  D3D11_VIEWPORT vp[kVp];
  D3D11_RECT sc[kVp];
  ID3D11Buffer* b7;  // reference
  UINT b7Bytes;
  ID3D11SamplerState* pool[kSampSlots];  // PS s5-s15 (references)
  uint32_t flags, dbg;
  bool rendererOk;
};

bool ReadRenderer(uint32_t* flags, uint32_t* dbg) {
  auto* r = reinterpret_cast<uint8_t*>(shadowbatch::g_rendererObj);
  if (!r) return false;
  __try {
    *flags = *reinterpret_cast<uint32_t*>(r + 0xd4);
    *dbg = *reinterpret_cast<uint32_t*>(r + 0x2120);
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

void ReadTarget(ID3D11DeviceContext* c, Target& t) {
  memset(&t, 0, sizeof(t));
  ID3D11RenderTargetView* rtv[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
  c->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtv, &t.dsv);
  for (auto*& p : rtv)
    if (p) {
      ++t.rtvs;
      p->Release();
    }
  t.nvp = kVp;
  c->RSGetViewports(&t.nvp, t.vp);
  t.nsc = kVp;
  c->RSGetScissorRects(&t.nsc, t.sc);
  c->VSGetConstantBuffers(kB7, 1, &t.b7);
  if (t.b7) {
    D3D11_BUFFER_DESC d;
    t.b7->GetDesc(&d);
    t.b7Bytes = d.ByteWidth;
  }
  c->PSGetSamplers(kPoolFirst, kSampSlots - kPoolFirst, &t.pool[kPoolFirst]);
  t.rendererOk = ReadRenderer(&t.flags, &t.dbg);
}

void ReleaseTarget(Target& t) {
  SafeRel(t.dsv);
  SafeRel(t.b7);
  ReleasePool(t.pool);
}

// The pass state the next jobs of cascade slot s record against (and our b7,
// once its size is known).
void LearnTarget(int s, const Target& t) {
  Casc& cs = g_casc[s];
  Learned& l = cs.learn;
  if (!t.dsv || t.rtvs || !t.b7 || !t.rendererOk || (t.dbg & kDbgBits)) {
    l.target = false;
    return;
  }
  if (!cs.b7) {
    D3D11_BUFFER_DESC d = {};
    d.ByteWidth = t.b7Bytes;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    ID3D11Device* dev = nullptr;
    g_ctx->GetDevice(&dev);
    if (!dev || FAILED(dev->CreateBuffer(&d, nullptr, &cs.b7)) || !cs.b7) {
      SafeRel(dev);
      l.target = false;
      return;
    }
    dev->Release();
    cs.b7Bytes = t.b7Bytes;
  }
  if (l.dsv != t.dsv) {
    SafeRel(l.dsv);
    l.dsv = t.dsv;
    l.dsv->AddRef();
  }
  l.nvp = t.nvp;
  l.nsc = t.nsc;
  memcpy(l.vp, t.vp, sizeof(t.vp));
  memcpy(l.sc, t.sc, sizeof(t.sc));
  l.flags = t.flags;
  for (UINT q = kPoolFirst; q < kSampSlots; ++q)
    if (l.pool[q] != t.pool[q]) {
      SafeRel(l.pool[q]);
      if ((l.pool[q] = t.pool[q])) l.pool[q]->AddRef();
    }
  l.target = true;
}

int CheckTarget(const Casc& cs, const Job& j, const Target& t) {
  if (!t.dsv || t.dsv != j.dsv || t.rtvs || t.nvp != j.nvp || memcmp(t.vp, j.vp, t.nvp * sizeof(D3D11_VIEWPORT)) != 0 ||
      t.nsc != j.nsc || memcmp(t.sc, j.sc, t.nsc * sizeof(D3D11_RECT)) != 0)
    return kPTarget;
  if (!t.rendererOk || t.flags != j.flags || (t.dbg & kDbgBits)) return kPFlags;
  if (!t.b7 || t.b7Bytes != cs.b7Bytes || !cs.b7) return kPB7;
  if (!PoolSame(reinterpret_cast<void* const*>(t.pool), reinterpret_cast<void* const*>(j.pool))) return kPPool;
  return kPExecuted;
}

// ---------------------------------------------------------------------------
// Render thread: the pass
// ---------------------------------------------------------------------------
enum : int { kPsNone = 0, kPsArmed, kPsExecuted, kPsStock, kPsStockRun };
struct PassCtx {
  int slot = 0;
  Job* job = nullptr;               // a built job whose snapshot is this pass's vector
  ID3D11CommandList* cl[kThreads] = {};  // armed: executed by the exec entry (primary's, then the helpers')
  int state = kPsNone;
  int reason = kPNoJob;
  bool first = false;
  bool captureDepth = false;
  int probesLeft = 0;
  size_t cursor = 0;
  uint32_t flags = 0;               // renderer+0xd4 at the first caster
  ID3D11Buffer* passB7 = nullptr;   // VS b7 at the first caster (identity only)
  void* pool[kSampSlots] = {};      // PS s5-s15 at the first caster (identity only)
  ID3D11Resource* depth = nullptr;  // captureDepth: reference
  uint64_t execNs = 0, replayNs = 0, executeNs = 0;
};
PassCtx* g_pass = nullptr;  // the recorded cascade's pass being drawn (render thread)

// At the first entry of the loop (the exec entry when armed, else the first
// ShadowMapRenderable caster): DCS's frame buffer, clear and binder have run.
void FirstCaster(PassCtx& pc) {
  pc.first = true;
  Casc& cs = g_casc[pc.slot];
  Target t;
  ReadTarget(g_ctx, t);
  pc.flags = t.flags;
  pc.passB7 = t.b7;
  for (UINT q = 0; q < kSampSlots; ++q) pc.pool[q] = t.pool[q];
  LearnTarget(pc.slot, t);
  if (pc.captureDepth && t.dsv) t.dsv->GetResource(&pc.depth);
  if (!t.b7 || !t.rendererOk || (t.dbg & kDbgBits)) pc.probesLeft = 0;
  if (pc.state == kPsArmed) {
    const int64_t t0 = defrec::Qpc();
    int why = CheckTarget(cs, *pc.job, t);
    if (!why && pc.job->vt23Count) {
      uint32_t replays = 0;
      why = ReplayAndCheckGuarded(*pc.job, &replays);
      cs.st.vt23 += replays;
    }
    pc.replayNs = NsSince(t0);
    if (why) {
      pc.state = kPsStock;
      pc.reason = why;
    } else if (uint32_t rt = 0; !ReplayRtGuarded(*pc.job, &rt)) {
      pc.state = kPsStock;  // DCS's own draws replay nothing more: the fault was before any mip generation
      pc.reason = kPTexture;
      g_faults++;
    } else {
      g_rtReplays += rt;
      g_ctx->CopyResource(cs.b7, t.b7);
      const bool gs = gsplit::g_on.load(std::memory_order_relaxed) != 0;  // [Suite] GpuSplit
      if (gs) gsplit::Bracket(0, pc.slot, true);
      const int64_t e0 = defrec::Qpc();
      for (ID3D11CommandList* l : pc.cl)
        if (l) g_ctx->ExecuteCommandList(l, TRUE);
      pc.executeNs = NsSince(e0);
      if (gs) {
        gsplit::Bracket(0, pc.slot, false);
        const Job& gj = *pc.job;
        for (uint32_t gi = 0; gi < gj.groupCount; ++gi) {
          const Group& g = gj.groups[gi];
          if (g.mesh)
            gsplit::MeshDraw(0, pc.slot, g.mesh->ib, g.mesh->ibFormat, g.mesh->indexCount, g.count, gi >= gj.groupsTex,
                             g.mesh->vb, g.mesh->stride);
        }
      }
      pflush::AfterExecute();  // [Model] PassFlush 0x20 (one relaxed load when off)
      if (sfilt::Live()) sfilt::g_sh.ForgetAll();  // restored exactly, but the filter's shadow is cheap to rebuild
      pc.state = kPsExecuted;
      pc.reason = kPExecuted;
    }
    pc.execNs = NsSince(t0);
  }
  ReleaseTarget(t);
}

uint64_t __fastcall ExecVt1(void* self, void* ctx) {
  PassCtx* pc = g_pass;
  if (!pc || GetCurrentThreadId() != g_renderTid || static_cast<ExecObj*>(self)->slot != pc->slot) return 0;
  if (!pc->first) FirstCaster(*pc);
  if (pc->state != kPsExecuted && pc->job) {
    // The list did not run: DCS draws the recorded casters here, through
    // their own vt[1] (our override passes them on: the pass is stock).
    if (pc->state == kPsArmed) {
      pc->state = kPsStock;
      pc->reason = kPNoCaster;
    }
    const Job& j = *pc->job;
    for (uint32_t r = 0; r < j.recCount; ++r) {
      if (!j.recs[r].alive) continue;
      void* c = j.snap[j.recs[r].caster];
      (*reinterpret_cast<Vt1Fn**>(c))[1](c, ctx);
    }
  }
  return 0;
}

int64_t Locate(PassCtx& pc, void* self) {
  const Job& j = *pc.job;
  for (size_t k = pc.cursor; k < j.n; ++k)
    if (j.snap[k] == self) {
      pc.cursor = k + 1;
      return static_cast<int64_t>(k);
    }
  return -1;
}

// ---- Probe call ----
Capture g_cap;            // render thread, during one probe call
const KeyStatic* g_capSt = nullptr;

void DrawAfter(void*, int, void* shader, int prim, int a4, int a5, int instances) {
  Capture& c = g_cap;
  if (c.draws++) return;
  if (shader) ReadStreamsGuarded(static_cast<const uint8_t*>(shader), c);
  c.prim = prim;
  c.a4 = a4;
  c.a5 = a5;
  c.instances = instances;
  c.rendererOk = ReadRenderer(&c.flags, &c.dbg);
  CaptureState(g_ctx, g_capSt->cbSlot, g_capSt->sbSlot, c);
}

bool ReadProbeInRaw(void* self, ProbeIn& in) {
  __try {
    in.item = *reinterpret_cast<uint8_t**>(static_cast<uint8_t*>(self) + 0x10);
    in.mat = *reinterpret_cast<uint8_t**>(in.item + 0x10);
    in.shader = *reinterpret_cast<uint8_t**>(in.mat + 0x30);
    if (!in.shader) return false;
    in.tech = shadowbatch::TechOf(in.mat);
    in.mesh = *reinterpret_cast<uint8_t**>(in.item + 0xc0);
    in.page = *reinterpret_cast<uint32_t*>(in.item + 0xd0);
    in.effect = *reinterpret_cast<void**>(in.shader + 0x50);
    in.techBegin = *reinterpret_cast<void**>(in.shader + 0xb0);
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

// The VS of pass 0 of the technique as the FX pass block holds it (what
// Apply binds; LeaderRaw reads the same).
void* FxPassVsGuarded(uint8_t* shader, uint64_t tech) {
  __try {
    auto* techBegin = *reinterpret_cast<uint8_t**>(shader + 0xb0);
    auto* techEnd = *reinterpret_cast<uint8_t**>(shader + 0xb8);
    // Range-checked as in FxStageDepsRaw: an out-of-range technique could
    // read a function pointer from unrelated memory and call it (SEH only
    // catches the cases that fault).
    if (!techBegin || tech < 1 || tech > static_cast<uint64_t>((techEnd - techBegin) / 0x50)) return nullptr;
    auto* techObj = *reinterpret_cast<uint8_t**>(techBegin + (tech - 1) * 0x50 + 0x20);
    using PassFn = uint8_t*(__fastcall*)(void*, uint32_t);
    auto* fxpass = (*reinterpret_cast<PassFn**>(techObj))[7](techObj, 0);
    if (!fxpass) return nullptr;
    auto* vsBlock = *reinterpret_cast<uint8_t**>(fxpass + 0xc0);
    return *reinterpret_cast<void**>(vsBlock + 0x18);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return nullptr;
  }
}

// The slots FX Apply binds for one stage of the technique's pass 0: the
// effect runtime's shader block (dx11backend ApplyShaderBlock 0x68470 [V]):
// +0x18 the D3D shader, +0x20/+0x28 constant-buffer dependencies, +0x30/+0x38
// sampler dependencies, each {u32 first slot, u32 count, FX pointers, D3D
// objects set} with stride 0x20; pass block +0xc0 VS, +0xc8 PS [V
// LeaderRaw, R17]. Checked against the probe: the block's shader is the bound
// one and every sampler dependency's object is the one bound at its slot.
struct FxDeps {
  bool ok = false;
  uint32_t samp = 0, cb = 0;
};
FxDeps FxStageDepsRaw(uint8_t* shader, uint64_t tech, int stage, const void* bound,
                      ID3D11SamplerState* const* boundSamp) {
  FxDeps d;
  auto* techBegin = *reinterpret_cast<uint8_t**>(shader + 0xb0);
  auto* techEnd = *reinterpret_cast<uint8_t**>(shader + 0xb8);
  if (!techBegin || tech < 1 || tech > static_cast<uint64_t>((techEnd - techBegin) / 0x50)) return d;
  auto* techObj = *reinterpret_cast<uint8_t**>(techBegin + (tech - 1) * 0x50 + 0x20);
  using PassFn = uint8_t*(__fastcall*)(void*, uint32_t);
  auto* fxpass = (*reinterpret_cast<PassFn**>(techObj))[7](techObj, 0);
  if (!fxpass) return d;
  const uint8_t* block = *reinterpret_cast<uint8_t* const*>(fxpass + 0xc0 + 8 * stage);
  if (!block || *reinterpret_cast<void* const*>(block + 0x18) != bound) return d;
  const uint32_t cbN = *reinterpret_cast<const uint32_t*>(block + 0x20);
  const uint8_t* cbDeps = *reinterpret_cast<uint8_t* const*>(block + 0x28);
  const uint32_t sN = *reinterpret_cast<const uint32_t*>(block + 0x30);
  const uint8_t* sDeps = *reinterpret_cast<uint8_t* const*>(block + 0x38);
  if (cbN > kCbSlots || sN > kSampSlots || (cbN && !cbDeps) || (sN && !sDeps)) return d;
  for (uint32_t k = 0; k < cbN; ++k) {
    const uint32_t first = *reinterpret_cast<const uint32_t*>(cbDeps + k * 0x20);
    const uint32_t n = *reinterpret_cast<const uint32_t*>(cbDeps + k * 0x20 + 4);
    if (first + n > kCbSlots) return d;
    for (uint32_t q = 0; q < n; ++q) d.cb |= 1u << (first + q);
  }
  for (uint32_t k = 0; k < sN; ++k) {
    const uint32_t first = *reinterpret_cast<const uint32_t*>(sDeps + k * 0x20);
    const uint32_t n = *reinterpret_cast<const uint32_t*>(sDeps + k * 0x20 + 4);
    auto* objs = *reinterpret_cast<ID3D11SamplerState* const* const*>(sDeps + k * 0x20 + 0x10);
    if (first + n > kSampSlots || (n && !objs)) return d;
    for (uint32_t q = 0; q < n; ++q) {
      if (boundSamp && objs[q] != boundSamp[first + q]) return d;
      d.samp |= 1u << (first + q);
    }
  }
  d.ok = true;
  return d;
}

FxDeps FxStageDepsGuarded(uint8_t* shader, uint64_t tech, int stage, const void* bound,
                          ID3D11SamplerState* const* boundSamp) {
  __try {
    return FxStageDepsRaw(shader, tech, stage, bound, boundSamp);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return FxDeps();
  }
}

bool TechNameGuarded(uint8_t* shader, uint64_t tech, char* out, size_t cap) {
  __try {
    const char* n = shadowtex::TechName(shader, tech);
    size_t i = 0;
    for (; i + 1 < cap && n[i]; ++i) out[i] = n[i];
    out[i] = 0;
    return i > 0;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

// Static facts of (shader, technique): reflection of shadow_inst's (b) VS of
// pass 0 (render thread, once per key).
const char* StaticForBody(uint8_t* shader, uint64_t tech, KeyStatic& st) {
  st = KeyStatic();
  const shadowinst::MapEntry* e = shadowinst::FindEntry(shader, tech, 0);
  if (!e || !e->result) return st.why = "shadow inst has no compile of the key";
  char name[64];
  if (!TechNameGuarded(shader, tech, name, sizeof(name))) return st.why = "technique name not readable";
  std::vector<uint8_t> code;
  std::string cbName;
  UINT psoOff = 0;
  {
    std::lock_guard<std::mutex> lock(shadowinst::g_mutex);
    const shadowinst::KeyResult& r = *e->result;
    for (const shadowinst::TechVs& tv : r.techs) {
      if (tv.name != name || tv.passVs.empty()) continue;
      const int idx = tv.passVs[0];
      if (idx < 0 || static_cast<size_t>(idx) >= r.vs.size()) break;
      code = r.vs[idx].bytecode;
      cbName = r.vs[idx].diff.cbName;
      psoOff = r.vs[idx].diff.cbOffset;
      break;
    }
  }
  if (code.empty()) return st.why = "shadow inst has no VS of pass 0";
  return AnalyseVs(shadowinst::g_compiler.reflect, code.data(), code.size(), cbName.c_str(), psoOff, st);
}

// Every resource name the technique's passes bind (shadow_inst's (a) RDEF of
// every stage) must be the material's buffer, the per-view buffer,
// sbPositions, one of the material's texture records, or a sampler (DCS's
// samplers11.hlsl names: g...Sampler); else the PS may read something no
// probe can map. Render thread.
bool NameIsMaterialTexture(uint8_t* mat, uint8_t* shader, const char* name) {
  const uint8_t* recs = *reinterpret_cast<uint8_t**>(shader + 0xc8);
  const uint8_t* recEnd = *reinterpret_cast<uint8_t**>(shader + 0xd0);
  const int64_t nrec = recs && recEnd > recs ? (recEnd - recs) / 0x50 : 0;
  const uint32_t n = *reinterpret_cast<uint32_t*>(mat + 0x2d8);
  for (uint32_t i = 0; i < n && i < 32; ++i) {
    const int64_t h = *reinterpret_cast<int64_t*>(mat + 0x240 + 8 * i);
    if (h < 0 || h >= nrec) continue;
    const char* rn = *reinterpret_cast<const char* const*>(recs + h * 0x50 + 0x30);
    if (rn && shadowtex::NameRefers(rn, name)) return true;
  }
  return false;
}

bool NameIsMaterialTextureGuarded(uint8_t* mat, uint8_t* shader, const char* name) {
  __try {
    return NameIsMaterialTexture(mat, shader, name);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

const char* NamesCheckBody(uint8_t* mat, uint8_t* shader, uint64_t tech, const KeyStatic& st) {
  const shadowinst::MapEntry* e = shadowinst::FindEntry(shader, tech, 0);
  char techName[64];
  if (!e || !e->result || !TechNameGuarded(shader, tech, techName, sizeof(techName)))
    return "the pass's read names are not known";
  std::vector<std::string> names;
  {
    std::lock_guard<std::mutex> lock(shadowinst::g_mutex);
    for (const shadowinst::TechReads& r : e->result->reads)
      if (r.name == techName && r.ok) names = r.bound;
  }
  if (names.empty()) return "the pass's read names are not known";
  for (const std::string& n : names) {
    if (n == "def_uniforms" || n == "sbPositions" || n == "qvInstOffsets" || (st.b7Name[0] && n == st.b7Name)) continue;
    if (n.find("Sampler") != std::string::npos || n.find("sampler") != std::string::npos) continue;
    if (!NameIsMaterialTextureGuarded(mat, shader, n.c_str())) return "the pass reads a resource that is no material texture, CB, sbPositions or sampler";
  }
  return nullptr;
}

// Render-thread entries of the two above: they copy vectors and strings, and
// a std::bad_alloc must not unwind into DCS's caster loop (the probe then
// treats the key as not known, as for any other missing fact).
const char* StaticFor(uint8_t* shader, uint64_t tech, KeyStatic& st) {
  try {
    return StaticForBody(shader, tech, st);
  } catch (...) {
    st = KeyStatic();
    return st.why = "out of memory";
  }
}
const char* NamesCheck(uint8_t* mat, uint8_t* shader, uint64_t tech, const KeyStatic& st) {
  try {
    return NamesCheckBody(mat, shader, tech, st);
  } catch (...) {
    return "out of memory";
  }
}

// DCS's own draw of the caster, read back right after its D3D draw. Every
// change to DCS state is undone in the __finally. Plain (no unwinding objects).
bool ProbeCallRaw(void* self, void* ctx, uint64_t* ret) {
  bool vt = false;
  __try {
    shadowbatch::t_after = &DrawAfter;
    *shadowbatch::g_rendererObj = shadowbatch::g_myVtbl;
    vt = true;
    *ret = instcount::g_orig(self, ctx);
  } __finally {
    if (vt) *shadowbatch::g_rendererObj = shadowbatch::g_rendererVtbl;
    shadowbatch::t_after = nullptr;
  }
  return true;
}

bool ProbeCall(void* self, void* ctx, uint64_t* ret) {
  __try {
    return ProbeCallRaw(self, ctx, ret);
  } __except (shadowbatch::AvFilter(GetExceptionCode())) {
    g_faults++;
    Disable("access violation in a probed caster draw");
    *ret = 0;
    return true;
  }
}

const shadowtex::MaskEntry* MaskForMat(uint8_t* mat);

// Probes the caster when its key or mesh is still unknown: true = drawn here
// (DCS's own draw), false = not probed (the caller draws it as usual).
bool TryProbe(PassCtx& pc, void* self, void* ctx, uint64_t* ret) {
  ProbeIn in;
  if (!ReadProbeInRaw(self, in)) return false;
  const KeyEntry* k = FindKey(g_tab, in.shader, in.tech, pc.flags, in.effect, in.techBegin);
  if (k && k->state.load() <= 0) return false;
  if (!k && KeyCooling(g_tab, in.shader, in.tech, pc.flags, in.effect, in.techBegin, g_entryGen, false)) return false;
  if (k && FindMesh(g_tab, in.mesh, in.shader, in.tech, in.effect, in.techBegin)) return false;  // already learned
  const bool textured = instcount::IsTextured(in.mat);
  const shadowtex::MaskEntry* mask = textured ? MaskForMat(in.mat) : nullptr;
  if (textured && !mask) return false;  // its read sets are not known now: probe it later
  KeyStatic st;
  if (k) {
    st = k->st;
  } else if (StaticFor(in.shader, in.tech, st)) {
    // Not recordable whatever the draw binds: published without a probe.
    Capture none;
    none.flags = pc.flags;
    ProcessProbe(g_tab, in, st, ProbeFacts(), none);
    return false;
  }
  ProbeFacts f;
  f.dcsVs = FxPassVsGuarded(in.shader, in.tech);
  const shadowinst::MapEntry* me = shadowinst::FindEntry(in.shader, in.tech, 0);
  if (!me || me->dcsVs != f.dcsVs) f.dcsVs = nullptr;
  f.passB7 = pc.passB7;
  f.pageSrv = PageSrvGuarded(g_env, in.page);
  f.meshWhy = ReadMeshGuarded(in.mesh, f.mesh);
  if (*shadowbatch::g_rendererApi != static_cast<void*>(shadowbatch::g_rendererObj) ||
      *shadowbatch::g_rendererObj != static_cast<void*>(shadowbatch::g_rendererVtbl))
    return false;
  // A batching leader is drawn alone (its group falls back: the members draw
  // themselves, batching's own fallback path); members are never probed.
  shadowbatch::Slot* s = nullptr;
  if (shadowbatch::g_planActive && shadowbatch::g_batching && shadowbatch::g_cur) {
    s = shadowbatch::PeekSlot(*shadowbatch::g_cur, self);
    if (s && s->role == shadowbatch::kMember) return false;
    if (s && s->role != shadowbatch::kLeader) s = nullptr;
  }
  if (s) {
    shadowbatch::g_cur->groupFailed[s->group] = 1;
    g_probeLeaders++;
  }
  g_cap = Capture();
  g_capSt = &st;
  ProbeCall(self, ctx, ret);
  // After the draw (render thread): the read sets' views slot 26 bound, and
  // the pass's read names (keys with a PS).
  if (mask) ReadSetsGuarded(g_env.tex, in, *mask, f);
  if (g_cap.ps) f.namesWhy = NamesCheck(in.mat, in.shader, in.tech, st);
  if (g_cap.ps) {
    const FxDeps d = FxStageDepsGuarded(in.shader, in.tech, 1, g_cap.ps, g_cap.psSamp);
    f.depsOk = d.ok;
    if (d.ok) f.psSampDeps = d.samp, f.psCbDeps = d.cb;
  }
  // The lists bind the pass's pool at s5-s15: a PS draw that sees other
  // samplers there (an FX dependency on a pool slot, or the pool rebound) is
  // not learnt from; probed again later.
  const bool poolOk = !g_cap.ps || (pc.first && PoolSame(reinterpret_cast<void* const*>(g_cap.psSamp), pc.pool));
  if (g_cap.draws != 1 || !g_cap.rendererOk || (g_cap.dbg & kDbgBits)) {
    g_probeOdd++;
  } else if (!poolOk) {
    if (g_probePool++ < 8)
      Log("shadow recorder: probe of shader %p technique 0x%llx: PS sampler pool slots differ from the pass's "
          "(first caster %s); not learnt",
          static_cast<void*>(in.shader), static_cast<unsigned long long>(in.tech), pc.first ? "seen" : "not seen");
  } else if (!g_disabled.load()) {
    ProcessProbe(g_tab, in, st, f, g_cap, g_entryGen);
  }
  ReleaseCapture(g_cap);
  g_capSt = nullptr;
  g_probes++;
  --pc.probesLeft;
  return true;
}

bool Override(void* self, void* ctx, uint64_t* ret) {
  PassCtx* pc = g_pass;
  if (pc && GetCurrentThreadId() == g_renderTid) {
    if (!pc->first) FirstCaster(*pc);
    if (pc->probesLeft > 0 && pc->job) {
      const int64_t k = Locate(*pc, self);
      if (k >= 0 && pc->job->want[k] && TryProbe(*pc, self, ctx, ret)) return true;
    }
  }
  if (instcount::OverrideFn p = g_prevOverride) return p(self, ctx, ret);
  return false;
}

void PrevWrap(void* pass, void* ctx, shadowpass::ExecFn orig) {
  if (shadowpass::WrapFn p = g_prevWrap)
    p(pass, ctx, orig);
  else
    orig(pass, ctx);
}

// S6: DCS's loop runs over [exec entry, residual casters]. The cascade's
// vector descriptor is swapped for the call and restored in the __finally;
// shadow batching planned on the original before this (it calls `orig` after
// its own planning). Plain (no unwinding objects).
struct Swap {
  shadowpass::ExecFn orig;
  void** vec;
  Job* job;
};
Swap g_swap;  // render thread, for the call

void __fastcall SwapOrigRaw(void* pass, void* ctx) {
  void** vec = g_swap.vec;
  void* b0 = vec[0];
  void* e0 = vec[1];
  bool swapped = false;
  __try {
    vec[0] = g_swap.job->swapList;
    vec[1] = g_swap.job->swapList + g_swap.job->swapCount;
    swapped = true;
    g_swap.orig(pass, ctx);
  } __finally {
    if (swapped) {
      vec[0] = b0;
      vec[1] = e0;
    }
  }
}

void RunPass(PassCtx& pc, void* pass, void* ctx, shadowpass::ExecFn orig, void** vec) {
  g_pass = &pc;
  if (pc.state == kPsArmed) {
    g_swap = {orig, vec, pc.job};
    PrevWrap(pass, ctx, &SwapOrigRaw);
  } else {
    PrevWrap(pass, ctx, orig);
  }
  g_pass = nullptr;
  if (pc.state == kPsArmed) {
    pc.state = kPsStock;
    pc.reason = kPNoCaster;
  }
  if (pc.state == kPsExecuted) {
    const int64_t t0 = defrec::Qpc();
    if (!RestoreMatsGuarded(*pc.job)) {
      g_faults++;
      Disable("access violation restoring the materials' posStructOffset");
    }
    g_casc[pc.slot].st.restoreNs += NsSince(t0);
  }
}

// Stock run, then recorded run, same frame; depth compared texel for texel.
void VerifyPass(PassCtx& pc, void* pass, void* ctx, shadowpass::ExecFn orig, void** vec) {
  Stat& st = g_casc[pc.slot].st;
  PassCtx a = pc;
  a.state = kPsStockRun;
  for (auto*& l : a.cl) l = nullptr;  // pc's (released by WrapTarget)
  a.captureDepth = true;
  a.probesLeft = 0;
  RunPass(a, pass, ctx, orig, vec);
  ID3D11Texture2D* sa = a.depth ? shadowbatch::CopyToStaging(a.depth) : nullptr;
  SafeRel(a.depth);
  PassCtx b = pc;
  b.captureDepth = true;
  b.probesLeft = 0;
  RunPass(b, pass, ctx, orig, vec);
  ID3D11Texture2D* sb = b.depth ? shadowbatch::CopyToStaging(b.depth) : nullptr;
  SafeRel(b.depth);
  if (b.state == kPsExecuted && sa && sb) {
    uint64_t texels = 0;
    const int64_t bad = shadowbatch::CompareGuarded(sa, sb, &texels);
    st.verifyPasses++;
    st.verifyTexels += texels;
    if (bad < 0) st.verifyErrors++;
    if (bad > 0) {
      st.verifyMismatch++;
      st.verifyBadTexels += static_cast<uint64_t>(bad);
      Disable("depth differs between the stock and the recorded cascade");
    }
  } else {
    st.verifySkipped++;
  }
  SafeRel(sa);
  SafeRel(sb);
  pc.state = b.state;
  pc.reason = b.reason;
  pc.execNs = b.execNs;
}

int CascadeOfGuarded(void* pass) {
  __try {
    return *reinterpret_cast<int32_t*>(static_cast<uint8_t*>(pass) + 0x60);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return -1;
  }
}

// The job's texture snapshot, once its keys are final (render thread).
void SnapshotIfReady(int s, bool late) {
  Casc& cs = g_casc[s];
  if (!cs.armed || !cs.job) return;
  Job& j = *cs.job;
  if (j.startState.load(std::memory_order_acquire) != kStartRun || j.snapped ||
      !j.stageA.load(std::memory_order_acquire) || !j.needGo)
    return;
  TakeSnapshot(j);
  (late ? cs.st.snapLate : cs.st.snapEarly)++;
  if (!j.entryGen) cs.st.snapPre++;  // before this RenderGraph's entry (another graph's top-level pass)
  cs.st.snapKeys += j.texKeyCount;
}

// After every top-level render pass (pass_timing.h): the snapshots whose keys
// are ready, ahead of the cascades.
// Before and after every render pass, nested ones too (pass_timing.h): the
// snapshot of a job whose stage A just finished, without waiting for a
// top-level pass to end. One relaxed load when nothing is pending.
void EachPass() {
  if (!g_snapBell.load(std::memory_order_relaxed)) return;
  if (!g_renderTid || GetCurrentThreadId() != g_renderTid) return;
  // Counted before the check (as Wrap does): Shutdown sets g_shutdown and
  // then waits for g_inside == 0, so a call past the check is always waited for.
  g_inside.fetch_add(1);
  if (!Active()) {
    g_inside.fetch_sub(1);
    return;
  }
  uint32_t bits = g_snapBell.exchange(0, std::memory_order_acquire);
  while (bits) {
    unsigned long s;
    _BitScanForward(&s, bits);
    bits &= bits - 1;
    if (s < kSlots) SnapshotIfReady(static_cast<int>(s), false);
  }
  g_inside.fetch_sub(1);
}

bool RearmIfIdle(int s);

void AfterTopPass() {
  if (!g_renderTid || GetCurrentThreadId() != g_renderTid) return;
  ++g_topCount;
  g_inside.fetch_add(1);  // before the check: see EachPass
  if (!Active()) {
    g_inside.fetch_sub(1);
    return;
  }
  TryMaintain();  // the texture table, when no job holds its lock
  for (int s = 0; s < kSlots; ++s) {
    SnapshotIfReady(s, false);
    Casc& cs = g_casc[s];
    // Late for this entry's pass: armed again as soon as its workers finish.
    if (cs.job && cs.armed && cs.passGen == g_entryGen && cs.job->entryGen == g_entryGen && RearmIfIdle(s))
      cs.st.rearmLate++;
  }
  g_inside.fetch_sub(1);
}

// Waits up to `us` for worker w (spin; the budget is short). True when idle.
bool WaitWorkerUs(int w, double us) {
  const int64_t t0 = defrec::Qpc();
  const double k = defrec::QpcToUs();
  while (g_pool.Busy(w) && (defrec::Qpc() - t0) * k < us) _mm_pause();
  return !g_pool.Busy(w);
}

int64_t QpcNs(int64_t a, int64_t b) {
  return a && b ? static_cast<int64_t>((b - a) * defrec::QpcToUs() * 1000.0) : 0;
}

// Waits (within [Model] ShadowRecorderWaitUs) for this render entry's job of
// slot s, its snapshot taken first if still due; decides what the pass does
// with it. Not ready in time: the pass is drawn stock (counted late), the job
// finishes on its workers and is dropped. The helpers whose lists join the
// pass (Job::joined, final once the primary is idle) are waited for too;
// the others hold no list.
void TakeJob(PassCtx& pc, void** vec) {
  Casc& cs = g_casc[pc.slot];
  Stat& st = cs.st;
  Job& j = *cs.job;
  const int64_t t0 = defrec::Qpc();
  const double budget = static_cast<double>(g_waitUs.load());
  const double k = defrec::QpcToUs();
  auto left = [&] {
    const double spent = (defrec::Qpc() - t0) * k;
    return spent >= budget ? 0.0 : budget - spent;
  };
  if (!j.snapped) {  // keys not ready at the last top-level pass: wait for them here
    while (g_pool.Busy(pc.slot) && !j.stageA.load(std::memory_order_acquire) && (defrec::Qpc() - t0) * k < budget)
      _mm_pause();
    SnapshotIfReady(pc.slot, true);
  }
  bool ready = WaitWorkerUs(pc.slot, left());
  const uint32_t joined = ready ? j.joined.load(std::memory_order_acquire) : 0;
  for (int t = 1; ready && t < kThreads; ++t)
    if (((joined >> t) & 1) && !WaitWorkerUs(Wk(pc.slot, t), left())) {
      ready = false;
      st.helperLate++;
    }
  const uint64_t ns = NsSince(t0);
  st.waitNs += ns;
  NoteMax(st.waitMaxNs, ns);
  if (!ready) {
    pc.reason = kPLate;  // abandoned: the workers still own the job
    cs.lateMaint = true;
    cs.lateEntry = g_entryQpc;  // its timeline is read once the workers are idle
    cs.latePassQpc = t0;
    cs.boost = kBoostJobs;  // one more helper for the next jobs
    return;
  }
  ID3D11CommandList* cl[kThreads] = {};
  auto drop = [&] {
    for (auto*& l : cl) SafeRel(l);
  };
  cl[0] = g_pool.TakeList(pc.slot);
  bool lists = cl[0] != nullptr;
  for (int t = 1; t < kThreads; ++t) {
    const int h = Wk(pc.slot, t);
    if ((joined >> t) & 1) {
      cl[t] = g_pool.TakeList(h);
      lists = lists && cl[t] && j.th[t].drew;
    } else if (!g_pool.Busy(h)) {
      if (ID3D11CommandList* l = g_pool.TakeList(h)) l->Release();  // none expected: it took no chunk
    }
  }
  if (j.phase.load(std::memory_order_acquire) != kJobBuilt || j.chunkFail.load(std::memory_order_acquire)) {
    drop();
    pc.reason = kPFailed;
    return;
  }
  st.jobsUsed++;
  st.buildNs += static_cast<uint64_t>(j.buildUs * 1000.0);
  st.commitNs += static_cast<uint64_t>(j.commitUs * 1000.0);
  st.resolveNs += static_cast<uint64_t>(j.resolveUs * 1000.0);
  st.helpersQueued += j.helpers;
  // Recording time proper (the worker's lastRecordUs also holds its waits for the start and the snapshot).
  if (cl[0]) st.recordNs += static_cast<uint64_t>((j.recordUs + g_pool.At(pc.slot).lastFinishUs) * 1000.0);
  for (int t = 1; t < kThreads; ++t)
    if (cl[t]) st.recordNs += static_cast<uint64_t>((j.th[t].recordUs + g_pool.At(Wk(pc.slot, t)).lastFinishUs) * 1000.0);
  st.preHelper += HelperReads(j);
  st.casterReads += j.n;
  // Timeline from the render entry.
  st.tlSamples++;
  st.early += j.startedEarly;
  st.tlPass += QpcNs(g_entryQpc, t0);
  st.tlStart += QpcNs(g_entryQpc, j.tStart);
  st.tlCommit += QpcNs(g_entryQpc, j.tCommit >= j.tStart ? j.tCommit : j.tStageA);
  st.tlStageA += QpcNs(g_entryQpc, j.tStageA);
  st.tlDone += QpcNs(g_entryQpc, j.tDone);
  st.tlP1 += QpcNs(g_entryQpc, j.tP1 >= j.tStart ? j.tP1 : j.tDone);
  st.tlTop += g_topCount;
  if (j.snapped && j.needGo) {
    st.tlSnapSamples++;
    st.tlSnap += QpcNs(g_entryQpc, j.tSnap);
  }
  if (joined) {
    st.tlHelperSamples++;
    st.tlHelper += QpcNs(g_entryQpc, HelpersDone(j));
  }
  // Ready with less than kTightMs to spare: one more helper for the next jobs.
  const int64_t done = HelpersDone(j) > j.tDone ? HelpersDone(j) : j.tDone;
  if (done && (t0 - done) * k < kTightMs * 1000.0) cs.boost = kBoostJobs;
  if (j.vec != vec || j.result != kBuildOk) {
    drop();
    pc.reason = j.result == kBuildOversize ? kPOversize : kPIdentity;
    return;
  }
  const int64_t c0 = defrec::Qpc();
  const int why = CheckJobGuarded(j);
  st.checkNs += NsSince(c0);
  if (why == kPIdentity) {
    drop();
    pc.reason = why;  // the snapshot is not this vector: no probes from it either
    return;
  }
  pc.job = &j;
  st.casters += j.n;
  for (int r = 0; r < kCasterReasons; ++r) st.casterReason[r] += j.reasons[r];
  if (why || !lists || j.chunkFail.load(std::memory_order_acquire)) {
    drop();
    pc.reason = why ? why : j.recorded ? kPFailed : kPNothing;
    return;
  }
  for (int t = 0; t < kThreads; ++t) pc.cl[t] = cl[t];
  pc.state = kPsArmed;
}

void LearnCascade(int s, void** vec, void* ctx) {
  void* rg = nullptr;
  uint32_t idx = 0;
  bool ok = false;
  __try {
    ok = shadowbatch::LearnRaw(vec, ctx, &rg, &idx);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    ok = false;
  }
  Learned& l = g_casc[s].learn;
  if (ok) {
    l.cascade = true;
    l.rg = rg;
    l.idx = idx;
  }
}

bool Arm(int s);

// Slot s's job served this entry's pass (or was late for it): once its
// workers are idle, its leftovers are released and the next frame's job is
// armed, so the sort hook can start it early. Render thread.
// The slot's lists nobody took (a late job's), released (its workers idle).
void DropLists(int s) {
  for (int t = 0; t < kThreads; ++t)
    if (ID3D11CommandList* l = g_pool.TakeList(Wk(s, t))) l->Release();
}

bool RearmIfIdle(int s) {
  Casc& cs = g_casc[s];
  if (SlotBusy(s)) return false;
  DropLists(s);
  ReleaseSnapshot(*cs.job);  // the executed lists hold their own references
  cs.armed = false;
  return Arm(s);
}

void WrapTarget(int s, void* pass, void* ctx, shadowpass::ExecFn orig) {
  void** vec = shadowpass::CasterVector(pass, ctx);
  if (!vec) return PrevWrap(pass, ctx, orig);
  Casc& cs = g_casc[s];
  LearnCascade(s, vec, ctx);
  cs.st.passes++;
  PassCtx pc;
  pc.slot = s;
  Job& job = *cs.job;
  const bool mine = cs.armed && job.entryGen && job.entryGen == g_entryGen;
  if (mine) TakeJob(pc, vec);
  pc.probesLeft = pc.job && pc.job->wantCount ? kProbesPerPass : 0;
  if (g_verify.load(std::memory_order_relaxed) && pc.state == kPsArmed)
    VerifyPass(pc, pass, ctx, orig, vec);
  else
    RunPass(pc, pass, ctx, orig, vec);
  for (auto*& l : pc.cl) SafeRel(l);
  if (pc.state == kPsExecuted) {
    const Job& j = *pc.job;
    cs.st.recorded += j.recorded;
    cs.st.textured += j.texRecorded;
    cs.st.cbBytes += j.cbBytes + HelperCbBytes(j);
    cs.st.draws += j.draws;
    cs.st.drawsHelper += HelperDraws(j);
    cs.st.groups += j.groupCount;
    cs.st.splitJobs += j.joined.load(std::memory_order_relaxed) != 0;
    cs.st.execNs += pc.execNs;
    cs.st.replayNs += pc.replayNs;
    cs.st.executeNs += pc.executeNs;
  }
  cs.st.passReason[pc.state == kPsExecuted ? kPExecuted : pc.reason]++;
  if (pc.state == kPsExecuted) g_workPasses.fetch_add(1, std::memory_order_relaxed);
  if (mine && job.resolved && !SlotBusy(s)) QueueMaint(job);
  TryMaintain();
  // Consumed: arm the next frame's job now, so the sort hook can start it as
  // soon as its vector is final. A late job stays armed (dropped at the next
  // render entry once its workers are idle).
  cs.passGen = g_entryGen;
  g_lastPassQpc = defrec::Qpc();
  if (mine) RearmIfIdle(s);
}

void Wrap(void* pass, void* ctx, shadowpass::ExecFn orig) {
  g_inside.fetch_add(1);
  if (Active() && g_pool.Disabled()) Disable("worker fault (see the deferred rec line above)");
  if (!Active()) {
    PrevWrap(pass, ctx, orig);
  } else {
    if (!g_renderTid) g_renderTid = GetCurrentThreadId();
    const int c = CascadeOfGuarded(pass);
    if (GetCurrentThreadId() != g_renderTid || !InScope(g_scope.load(), c) || !g_casc[c].job)
      PrevWrap(pass, ctx, orig);
    else
      WrapTarget(c, pass, ctx, orig);
  }
  g_inside.fetch_sub(1);
}

// ---------------------------------------------------------------------------
// Render thread: RenderGraph::render entry (the vectors are final)
// ---------------------------------------------------------------------------
size_t VectorCountGuarded(void* rg, void* renderables) {
  __try {
    return shadowbatch::VectorCountRaw(static_cast<uint8_t*>(rg), renderables);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return 0;
  }
}

// The timeline of the cascade's last late job, from the render entry it
// served (workers idle).
void NoteLate(Casc& cs, const Job& j) {
  if (!cs.lateEntry) return;
  const int64_t e = cs.lateEntry;
  cs.lateEntry = 0;
  if (!j.tStart) return;
  int64_t done = j.tDone;
  if (HelpersDone(j) > done) done = HelpersDone(j);
  if (done < j.tStart) return;  // aborted before it finished
  Stat& st = cs.st;
  st.lateSamples++;
  st.latePass += QpcNs(e, cs.latePassQpc);
  st.lateStart += QpcNs(e, j.tStart);
  st.lateCommit += QpcNs(e, j.tCommit >= j.tStart ? j.tCommit : done);
  st.lateStageA += QpcNs(e, j.tStageA >= j.tStart ? j.tStageA : done);
  st.lateDone += QpcNs(e, done);
  st.lateP1 += QpcNs(e, j.tP1 >= j.tStart ? j.tP1 : done);
  if (j.snapped && j.needGo && j.tSnap >= j.tStart) {
    st.lateSnapSamples++;
    st.lateSnap += QpcNs(e, j.tSnap);
  }
}

// Arms slot s's job for its next render entry (render thread, workers idle):
// every input but the vector, which the starter gives (StartJob). The
// primary and its helpers (HelpersFor: the last job's caster count, one more
// after a late or tight job) are queued now and wait for the start.
bool Arm(int s) {
  Casc& cs = g_casc[s];
  Job& j = *cs.job;
  const Learned& l = cs.learn;
  if (cs.armed || !l.cascade || !l.target || !l.dsv || !cs.b7) return cs.armed;
  if (SlotBusy(s)) {
    cs.st.refused++;  // a late job of an earlier frame still runs
    return false;
  }
  NoteLate(cs, j);
  if (cs.lateMaint && j.resolved) QueueMaint(j);  // a late job's misses (its workers are idle now)
  cs.lateMaint = false;
  SafeRel(g_pool.At(s).list);  // a result nobody took (Submit drops it too)
  ReleaseSnapshot(j);
  SafeRel(j.dsv);
  ReleasePool(j.pool);
  for (UINT q = kPoolFirst; q < kSampSlots; ++q)
    if ((j.pool[q] = l.pool[q])) j.pool[q]->AddRef();
  j.vec = nullptr;
  j.flags = l.flags;
  j.scope = g_scope.load();
  j.dsv = l.dsv;
  j.dsv->AddRef();
  j.nvp = l.nvp;
  j.nsc = l.nsc;
  memcpy(j.vp, l.vp, sizeof(j.vp));
  memcpy(j.sc, l.sc, sizeof(j.sc));
  j.b7 = cs.b7;
  j.execObj = &g_execObj[s];
  j.env = &g_env;
  j.tab = &g_tab;
  j.tex = g_tex.e ? &g_tex : nullptr;
  j.texLock = &g_texLock;
  j.useGen = g_entryGen;
  for (int t = 0; t < kThreads; ++t) {
    j.offBuf[t] = g_offBuf[Wk(s, t)];
    j.offSrv[t] = g_offSrv[Wk(s, t)];
  }
  j.instancing = g_instancing.load();
  j.splitAllowed = (g_split.load() >> s) & 1;
  j.minSplit = 2;  // split cascades always share their groups
  const uint32_t helpers = j.splitAllowed ? HelpersFor(j.n, cs.boost > 0, g_helpers.load()) : 0;
  if (cs.boost) --cs.boost;
  j.result = kBuildNone;
  j.snapped = false;
  j.needGo = false;
  j.startedEarly = false;
  j.entryGen = 0;
  j.tStart = j.tCommit = j.tStageA = j.tSnap = j.tDone = j.tP1 = 0;
  j.armRg = l.rg;
  j.armIdx = l.idx;
  j.stageA.store(0, std::memory_order_relaxed);
  j.goState.store(kGoWait, std::memory_order_relaxed);
  j.recPhase.store(kRpWait, std::memory_order_relaxed);
  j.joined.store(0, std::memory_order_relaxed);
  ResetEvents(j);
  j.preState.store(kGoWait, std::memory_order_relaxed);
  j.snapBell = &g_snapBell;
  j.slotBit = 1u << s;
  j.phase.store(kJobEmpty, std::memory_order_relaxed);
  j.startState.store(kStartArmed, std::memory_order_release);  // the sort hook may start it from here on
  // Helpers first (they wait for stage A to open); one that cannot be queued ends the count.
  j.helpers = 0;
  for (uint32_t t = 1; t <= helpers; ++t) {
    if (!g_pool.Submit(Wk(s, static_cast<int>(t)), kHelperMain[t], &j, nullptr)) break;
    j.helpers = t;
  }
  if (!g_pool.Submit(s, &JobMain, &j, nullptr)) {
    int st = kStartArmed;
    if (!j.startState.compare_exchange_strong(st, kStartIdle)) j.startState.store(kStartIdle);
    ReleaseHelpers(j);  // queued helpers end at once
    cs.st.refused++;
    return false;
  }
  cs.armed = true;
  cs.st.jobs++;
  return true;
}

// An armed job whose render entry passed without its pass (render thread):
// released once its workers are idle. False while they still run.
bool DropStale(int s) {
  Casc& cs = g_casc[s];
  Job& j = *cs.job;
  if (SlotBusy(s)) {
    if (j.startState.load() == kStartRun && !j.snapped && j.stageA.load()) {  // waiting for a snapshot: release it
      j.goState.store(kGoAbort, std::memory_order_release);
      SetEvent(j.go);
    }
    const int64_t t0 = defrec::Qpc();
    while (SlotBusy(s) && (defrec::Qpc() - t0) * defrec::QpcToUs() < 200.0) _mm_pause();
    if (SlotBusy(s)) return false;
  }
  DropLists(s);
  ReleaseSnapshot(j);
  cs.armed = false;
  return true;
}

// Worker priority ([Model] ShadowRecorderPriority), applied when it changes.
void ApplyPriority() {
  const int p = g_priority.load();
  if (p == g_priorityApplied || g_state.load() != 1) return;
  for (int w = 0; w < g_pool.Workers(); ++w)
    if (HANDLE t = g_pool.At(w).thread) SetThreadPriority(t, p);
  g_priorityApplied = p;
}

void OnRender(void* rg, void* renderables) {
  if (!g_renderTid || GetCurrentThreadId() != g_renderTid) return;
  g_inside.fetch_add(1);
  g_entryQpc = defrec::Qpc();
  g_topCount = 0;
  ++g_entryGen;
  ApplyPriority();
  const bool active = Active();
  if (active) TryMaintain();  // the texture table, before this entry's jobs take its lock
  const uint32_t scope = g_scope.load();
  // A job started before the last cascade pass of the previous entry (or
  // before that entry) was started for an earlier frame (e.g. while the
  // recorder was off): its classification is not this frame's.
  const int64_t prevEntry = g_lastPassQpc > g_cascEntryQpc ? g_lastPassQpc : g_cascEntryQpc;
  for (const Casc& cs : g_casc)
    if (cs.learn.cascade && rg == cs.learn.rg) g_cascEntryQpc = g_entryQpc;
  size_t count = SIZE_MAX;
  for (int s = 0; s < kSlots; ++s) {
    Casc& cs = g_casc[s];
    if (!active || !InScope(scope, s) || !cs.job || !cs.learn.cascade || rg != cs.learn.rg) continue;
    Job& j = *cs.job;
    const int st = j.startState.load();
    const bool oldStart = st == kStartRun && !j.entryGen && (!prevEntry || j.tStart < prevEntry);
    if (oldStart) cs.st.staleStart++;
    // Armed for an earlier entry whose pass did not run, started for an
    // earlier frame, or never started (start wait expired): dropped.
    if (cs.armed && (j.entryGen || oldStart || st == kStartAbort) && !DropStale(s)) {
      cs.st.refused++;
      continue;
    }
    if (!cs.armed && !Arm(s)) continue;
    if (count == SIZE_MAX) count = VectorCountGuarded(rg, renderables);
    if (cs.learn.idx >= count) continue;
    // The latest start: the vector is final now (the sort hook may have started it already).
    StartJob(j, reinterpret_cast<void**>(static_cast<uint8_t*>(renderables) + cs.learn.idx * 24), false);
    j.entryGen = g_entryGen;
    SnapshotIfReady(s, false);  // classified already when started early: the snapshot before any pass
  }
  g_inside.fetch_sub(1);
}

// ---------------------------------------------------------------------------
// Early start: Scene's per-collection sort (vectors final before render entry)
// ---------------------------------------------------------------------------
// Scene sortAndBatchRenderables pushes chunks to the frame's task queue that
// run 0x25bf0(scene, CollectionInfo*, &outVector, x) per collection, which
// rewrites that collection's caster vector in place [V Scene 0xfd40..0xff98,
// 0x171e0..0x1723d, sync path 0x259fa; shadow_batch.h "Planner threads"]. The
// chunks run on the pool during EndParse and BeginFrame and are joined at
// Visualizer 0x1707b6, before RenderGraph::render. When 0x25bf0 returns for a
// cascade's collection, its vector is final: nothing writes it again before
// the pass [V shadow_batch.h notes; the pass's identity check (memcmp of
// every pointer) guards it anyway]. The parse has written the items and
// materials before the sort [V R6 order]; page GPU buffers and views are
// created at EndParse once per page and kept (a page read before its creation
// is residual; the page check at the pass compares every view) [I]. The
// three call sites (`call rel32`, return value unused) are pointed at a stub
// that jumps to SortHook, which calls 0x25bf0 and then starts the armed job
// whose vector this is. Written with every other thread suspended, restored
// at unload; fixed RVAs with byte checks (a DCS update: start at render entry).
constexpr uint32_t kSortRva = 0x25bf0;
constexpr uint8_t kSortFnBytes[21] = {0x48, 0x89, 0x6c, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x18, 0x48,
                                      0x89, 0x7c, 0x24, 0x20, 0x41, 0x56, 0x48, 0x83, 0xec, 0x30};
struct SortSite {
  uint32_t rva;
  uint8_t before[8], after[8];
};
const SortSite kSortSites[3] = {
    {0xff87, {0x4c, 0x8b, 0x4e, 0x10, 0x48, 0x8b, 0x4f, 0x18}, {0xff, 0xc3, 0x8b, 0x46, 0x08, 0x03, 0x46, 0x04}},
    {0x1722c, {0x48, 0x03, 0x51, 0x20, 0x48, 0x8b, 0x49, 0x18}, {0x48, 0x8b, 0x47, 0x08, 0xff, 0xc3, 0x8b, 0x48}},
    {0x259fa, {0xc3, 0x49, 0x8d, 0x4f, 0xf8, 0x49, 0x8b, 0xd6}, {0x48, 0x83, 0xc3, 0x18, 0x49, 0x81, 0xc6, 0xd8}}};
using SortFn = uint64_t(__fastcall*)(void* scene, void* ci, void** out, void* x);
SortFn g_sortOrig = nullptr;
uint8_t* g_sortBase = nullptr;
uint8_t* g_sortStub = nullptr;
bool g_sortPatched = false;
std::atomic<int> g_sortInside{0};
std::atomic<uint64_t> g_sortCalls{0}, g_sortStarts{0};
// Optional observer after each sort call returned (gb_rec_count.h, measurement
// only: when a G-buffer collection's vector is final); nullptr when unused.
using SortObserverFn = void (*)(void** out);
std::atomic<SortObserverFn> g_sortObserver{nullptr};
// The G-buffer recorder's job starts (gb_rec.h), same call point; nullptr when unused.
std::atomic<SortObserverFn> g_sortObserver2{nullptr};
// Set while gb_rec_count.h's phase owns the sort call sites (it patched them
// itself): Install waits (retried every second) so the two never patch at once.
std::atomic<bool> g_sortBorrowed{false};

uint8_t* ArrayBaseGuarded(void* rg) {
  __try {
    return *reinterpret_cast<uint8_t**>(static_cast<uint8_t*>(rg) + 0xa18);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return nullptr;
  }
}

// A collection's vector is final (pool thread): start the armed job watching it.
void OnSorted(void** out) {
  if (!g_on.load(std::memory_order_relaxed) || g_disabled.load(std::memory_order_relaxed)) return;  // off: no starts
  for (int s = 0; s < kSlots; ++s) {
    Job* j = g_casc[s].job;
    if (!j || j->startState.load(std::memory_order_acquire) != kStartArmed) continue;
    uint8_t* base = ArrayBaseGuarded(j->armRg);
    if (base && reinterpret_cast<uint8_t*>(out) == base + static_cast<size_t>(j->armIdx) * 24 &&
        StartJob(*j, out, true))
      g_sortStarts++;
  }
}

uint64_t __fastcall SortHook(void* scene, void* ci, void** out, void* x) {
  g_sortInside.fetch_add(1);  // counted across the original too: it returns here
  const uint64_t r = g_sortOrig(scene, ci, out, x);
  g_sortCalls++;
  if (!g_shutdown.load(std::memory_order_relaxed)) OnSorted(out);
  if (SortObserverFn ob = g_sortObserver.load(std::memory_order_relaxed)) ob(out);
  if (SortObserverFn ob = g_sortObserver2.load(std::memory_order_relaxed)) ob(out);
  g_sortInside.fetch_sub(1);
  return r;
}

// The sites' bytes as analysed (and the call target); nullptr or why not.
const char* CheckSortSites(uint8_t* base) {
  uint8_t fn[sizeof(kSortFnBytes)];
  if (!allocslab::ReadBytes(base + kSortRva, fn, sizeof(fn)) || memcmp(fn, kSortFnBytes, sizeof(fn)) != 0)
    return "Scene sortAndBatchSingleCollection differs from the analysed build";
  for (const SortSite& s : kSortSites) {
    uint8_t b[21];
    if (!allocslab::ReadBytes(base + s.rva - 8, b, sizeof(b)) || memcmp(b, s.before, 8) != 0 ||
        memcmp(b + 13, s.after, 8) != 0 || b[8] != 0xE8)
      return "a Scene sort call site differs from the analysed build";
    int32_t rel;
    memcpy(&rel, b + 9, 4);
    if (base + s.rva + 5 + rel != base + kSortRva) return "a Scene sort call site calls something else";
  }
  return nullptr;
}

// Rewrites `n` 5-byte `call rel32` sites at once with every other thread
// suspended, retrying while one stands inside a site (split_filter.h's way).
bool WriteCallSitesSuspended(uint8_t* const* sites, const uint8_t (*want)[5], int n) {
  for (int attempt = 0; attempt < 50; ++attempt) {
    HANDLE held[2048];
    int nHeld = 0;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;
    THREADENTRY32 te{sizeof(te)};
    bool clear = true;
    for (BOOL ok = Thread32First(snap, &te); ok && nHeld < 2048; ok = Thread32Next(snap, &te)) {
      if (te.th32OwnerProcessID != GetCurrentProcessId() || te.th32ThreadID == GetCurrentThreadId()) continue;
      HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, te.th32ThreadID);
      if (!h) continue;
      if (SuspendThread(h) == static_cast<DWORD>(-1)) {
        CloseHandle(h);
        continue;
      }
      held[nHeld++] = h;
      CONTEXT ctx{};
      ctx.ContextFlags = CONTEXT_CONTROL;
      if (GetThreadContext(h, &ctx))
        for (int i = 0; i < n; ++i) {
          const uintptr_t a = reinterpret_cast<uintptr_t>(sites[i]);
          if (ctx.Rip > a && ctx.Rip < a + 5) clear = false;
        }
    }
    CloseHandle(snap);
    bool done = false;
    if (clear) {
      done = true;
      for (int i = 0; i < n; ++i) {
        DWORD old;
        if (!VirtualProtect(sites[i], 5, PAGE_EXECUTE_READWRITE, &old)) {
          done = false;
          continue;
        }
        memcpy(sites[i], want[i], 5);
        VirtualProtect(sites[i], 5, old, &old);
        FlushInstructionCache(GetCurrentProcess(), sites[i], 5);
      }
    }
    for (int k = 0; k < nHeld; ++k) {
      ResumeThread(held[k]);
      CloseHandle(held[k]);
    }
    if (clear) return done;
    Sleep(2);
  }
  return false;
}

// The `call rel32` bytes of a site to `target`.
void CallBytes(const uint8_t* site, const void* target, uint8_t out[5]) {
  const int32_t rel = static_cast<int32_t>(static_cast<const uint8_t*>(target) - (site + 5));
  out[0] = 0xE8;
  memcpy(out + 1, &rel, 4);
}

bool PatchSortSites(bool on) {
  if (g_sortPatched == on || !g_sortBase || !g_sortStub) return g_sortPatched == on;
  uint8_t* sites[3];
  uint8_t want[3][5];
  for (int i = 0; i < 3; ++i) {
    sites[i] = g_sortBase + kSortSites[i].rva;
    CallBytes(sites[i], on ? static_cast<void*>(g_sortStub) : static_cast<void*>(g_sortBase + kSortRva), want[i]);
  }
  if (!WriteCallSitesSuspended(sites, want, 3)) return false;
  g_sortPatched = on;
  return true;
}

bool InstallSortHook() {
  auto* base = reinterpret_cast<uint8_t*>(GetModuleHandleW(L"Scene.dll"));
  if (!base) return false;
  if (const char* why = CheckSortSites(base)) {
    Log("shadow recorder: %s; jobs start at RenderGraph::render entry", why);
    return false;
  }
  uint8_t* page = pacer::AllocNear(base);
  if (!page) {
    Log("shadow recorder: no memory within reach of Scene.dll; jobs start at RenderGraph::render entry");
    return false;
  }
  memset(page, 0xCC, 4096);
  const uint8_t stub[9] = {0x48, 0x8B, 0x05, 0x09, 0x00, 0x00, 0x00,  // mov rax, [rip+9] (the qword at +16)
                           0xFF, 0xE0};                               // jmp rax
  memcpy(page, stub, sizeof(stub));
  *reinterpret_cast<void**>(page + 16) = reinterpret_cast<void*>(&SortHook);  // aligned: retargeted atomically
  DWORD old;
  VirtualProtect(page, 4096, PAGE_EXECUTE_READWRITE, &old);
  FlushInstructionCache(GetCurrentProcess(), page, 4096);
  g_sortBase = base;
  g_sortStub = page;
  g_sortOrig = reinterpret_cast<SortFn>(base + kSortRva);
  if (!PatchSortSites(true)) {
    Log("shadow recorder: could not patch Scene's sort call sites; jobs start at RenderGraph::render entry");
    return false;
  }
  return true;
}

// The sort call sites patched, by whichever recorder installs first (the
// shadow recorder or gb_rec.h); the stub stays shared. False: unavailable.
bool EnsureSortHook() {
  if (g_sortPatched) return true;
  if (g_sortStub) return PatchSortSites(true);
  return InstallSortHook();
}

// ---------------------------------------------------------------------------
// Install / teardown
// ---------------------------------------------------------------------------
bool Compiled(void* shader, uint64_t tech) {
  const shadowinst::MapEntry* e = shadowinst::FindEntry(shader, tech, 0);
  return e && e->result;
}

// The mask slot 5's copy uses for this material now (shadow_tex.h Hook): only
// while the skip runs and the slot reaches it; lock-free (any thread).
const shadowtex::MaskEntry* MaskForMat(uint8_t* mat) {
  if (!shadowtex::g_on.load(std::memory_order_relaxed) || !shadowtex::g_attached.load(std::memory_order_relaxed) ||
      shadowtex::Slot5Mode() != shadowtex::kPsoHook || !shadowtex::g_cache)
    return nullptr;
  uint8_t* sh = *reinterpret_cast<uint8_t**>(mat + 0x30);
  if (!sh || *reinterpret_cast<void**>(sh) != shadowtex::g_shaderVtblPtr) return nullptr;
  const shadowtex::MaskEntry* e = shadowtex::g_cache->Find(
      sh, *reinterpret_cast<uint64_t*>(mat + 0x210), *reinterpret_cast<uint64_t*>(mat + 0x218),
      *reinterpret_cast<void**>(sh + 0x50), *reinterpret_cast<void**>(sh + 0xc8), *reinterpret_cast<void**>(sh + 0xd0),
      *reinterpret_cast<void**>(sh + 0xb0));
  return e && e->state > 0 ? e : nullptr;
}

// Installed on first use; false = not now (retried every second by main.cpp)
// or unavailable (g_state -1, logged once).
bool Install() {
  if (g_state.load() != 0) return g_state.load() > 0;
  if (!shadowbatch::Install()) return false;  // shadow_inst, hooks: not yet
  if (shadowrec::g_chained.load()) return false;  // the S0 counter phase owns the chain right now
  if (g_sortBorrowed.load()) return false;       // the G-buffer S0 phase has the sort call sites right now
  auto fail = [](const char* why) {
    Log("shadow recorder: %s; unavailable", why);
    g_state = -1;
    // Latched off for the session and Shutdown frees nothing unless ready:
    // free what this attempt allocated (jobs ~30 MB each, tables, offset
    // buffers) instead of leaking it into every later payload generation.
    // No worker runs here (the pool is not started or was stopped).
    for (Casc& cs : g_casc) FreeJob(cs.job);
    FreeTables(g_tab);
    FreeTex(g_tex);
    for (auto*& p : g_offSrv) SafeRel(p);
    for (auto*& p : g_offBuf) SafeRel(p);
    return false;
  };
  if (!shadowbatch::g_psoDirect)
    return fail("shadow batching's leaders still swap [item+0xd4] (slot-5 copy or ShadowMapRenderable code not as "
                "analysed)");
  if (!shadowbatch::g_renderSlot && !shadowbatch::g_renderSlot2) return fail("RenderGraph::render entry not hooked");
  if (!instcount::g_getTex || !instcount::g_valid) return fail("ModelDesc texture getters not found");
  if (!shadowbatch::g_rendererObj || !shadowbatch::g_myVtbl || !shadowbatch::g_ctx)
    return fail("DX11Renderer or the immediate context not matched");
  if (shadowtex::g_state.load() != 1 || !shadowtex::g_shaderVtblPtr) return fail("shadow texture skip not installed");
  if (!shadowinst::g_compiler.reflect) return fail("D3DReflect not loaded");
  ID3D11Device* dev = shadowinst::g_device;
  if (!dev) return false;
  if (shadowinst::g_deviceSingleThreaded) return fail("the D3D11 device is single-threaded");
  const defrec::Caps caps = defrec::QueryCaps(dev);
  if (!caps.commandLists || !caps.cbOffsetting || !caps.device1)
    return fail("the driver lacks command lists or constant-buffer offsets");
  if (!AllocTables(g_tab)) return fail("no memory for the tables");
  if (!g_tex.e && !AllocTex(g_tex, 1u << 13)) return fail("no memory for the texture table");
  for (int s = 0; s < kSlots; ++s)
    if (!g_casc[s].job && !(g_casc[s].job = NewJob())) return fail("no memory for the jobs");
  if (!InitInnerRt(shadowtex::g_dx)) Log("shadow recorder: render-target textures not supported (dx11backend's class not as analysed)");
  g_env.smrVt = shadowbatch::g_smrVtbl;
  g_env.modelVt = shadowbatch::g_modelMatVtbl;
  g_env.shaderVt = shadowtex::g_shaderVtblPtr;
  g_env.globals = reinterpret_cast<uint8_t* const*>(shadowtex::g_ng + shadowtex::g_at.globals);
  g_env.compiled = &Compiled;
  g_env.maskFor = &MaskForMat;
  g_env.instVs = &InstancedVs;
  g_env.tex.texVtbl = shadowtex::g_texVtblPtr;
  g_env.tex.inner[0] = shadowtex::g_innerFile;
  g_env.tex.inner[1] = shadowtex::g_innerArray;
  g_env.tex.inner[2] = shadowtex::g_innerDummy;
  g_env.tex.getDesc = shadowtex::g_getDesc;
  g_env.tex.compat = shadowtex::g_compat;
  for (auto*& p : g_execVtbl) p = reinterpret_cast<void*>(&ExecNop);
  g_execVtbl[1] = reinterpret_cast<void*>(&ExecVt1);
  for (int s = 0; s < kSlots; ++s) g_execObj[s] = {g_execVtbl, s};
  static_assert(kThreads * kSlots <= defrec::Pool::kMaxWorkers, "a worker per cascade thread");
  for (int w = 0; w < kThreads * kSlots; ++w)
    if (!g_offBuf[w] && !CreateOffsets(dev, &g_offBuf[w], &g_offSrv[w])) return fail("no t127 offset buffers");
  defrec::PoolConfig pc;
  pc.workers = kThreads * kSlots;
  pc.cb.mode = defrec::CbMode::kOffsets;
  pc.priority = g_priority.load();
  pc.name = "shadow recorder";
  if (!g_pool.Start(dev, pc)) return fail("its workers did not start");
  g_priorityApplied = pc.priority;
  if (g_pool.At(0).ring.Mode() != defrec::CbMode::kOffsets) {
    g_pool.Stop();
    return fail("constant-buffer offsets unavailable on the worker contexts");
  }
  dev->GetImmediateContext(&g_ctx);
  // Jobs start when their vector is final (Scene's sort), else at render entry.
  const bool early = EnsureSortHook();
  // Texture snapshots after the top-level passes (falls back to the cascade pass without it).
  ptiming::Install();
  if (ptiming::g_orig) {
    ptiming::g_afterTop = &AfterTopPass;
    ptiming::g_eachPass = &EachPass;
  }
  g_prevWrap = shadowpass::g_wrap.load();
  g_prevOverride = instcount::g_override.load();
  shadowpass::g_wrap = &Wrap;
  instcount::g_override = &Override;
  shadowbatch::g_recObserver = &OnRender;
  g_chained = true;
  g_state = 1;
  Log("shadow recorder: ready (scope 0x%x: cascades 0-3 by bit, untextured bit 8, textured bit 9; %d workers; the "
      "cascade loop runs over [exec entry, residual casters]; texture snapshots %s)",
      g_scope.load(), kThreads * kSlots, ptiming::g_orig ? "after the top-level passes" : "at the cascade pass");
  Log("shadow recorder: worker priority %d, wait budget %u us, split mask 0x%x (up to %u helpers per job), "
      "instancing %d; jobs start %s",
      g_priority.load(), g_waitUs.load(), g_split.load(), g_helpers.load(), g_instancing.load() ? 1 : 0,
      early ? "when Scene's sort finishes their vector (3 call sites patched)" : "at RenderGraph::render entry");
  return true;
}

bool Ready() { return g_state.load() == 1 && !g_disabled.load(); }

// True when its objects were released (false: left alive, still in use or
// never installed by this payload with nothing to free).
bool Shutdown() {
  g_shutdown = true;
  g_on = false;
  if (g_chained) {
    shadowpass::WrapFn w = &Wrap;
    shadowpass::g_wrap.compare_exchange_strong(w, g_prevWrap);
    instcount::OverrideFn o = &Override;
    instcount::g_override.compare_exchange_strong(o, g_prevOverride);
    shadowbatch::RenderObserverFn ob = &OnRender;
    shadowbatch::g_recObserver.compare_exchange_strong(ob, nullptr);
    ptiming::AfterTopFn af = &AfterTopPass;
    ptiming::g_afterTop.compare_exchange_strong(af, nullptr);
    ptiming::EachFn ef = &EachPass;
    ptiming::g_eachPass.compare_exchange_strong(ef, nullptr);
    g_chained = false;  // g_prevWrap/g_prevOverride stay for calls in flight
  }
  if (g_state.load() != 1) return true;
  const bool unpatched = PatchSortSites(false);
  // The stub (never freed) goes straight to the original from now on: a call
  // already past a site, or sites that could not be restored, never reach
  // this module once it is unloaded.
  if (g_sortStub)
    reinterpret_cast<std::atomic<void*>*>(g_sortStub + 16)->store(reinterpret_cast<void*>(g_sortOrig));
  if (!unpatched) Log("shadow recorder: WARNING could not restore Scene's sort call sites (they call the original)");
  for (int i = 0; i < 400 && (g_inside.load() != 0 || g_sortInside.load() != 0); ++i) Sleep(5);
  for (Casc& cs : g_casc)
    if (cs.job) {
      int st = kStartArmed;
      cs.job->startState.compare_exchange_strong(st, kStartAbort);
      SetEvent(cs.job->start);
      cs.job->goState.store(kGoAbort);
      SetEvent(cs.job->go);
      ReleaseHelpers(*cs.job);
    }
  // A pass still inside after the wait may be in TakeList/Wait/ExecuteAll on
  // the pool: stopping it then (Teardown releases the lists and contexts)
  // raced the render thread (double Release). Leave everything alive instead.
  const bool busy = g_inside.load() != 0 || g_sortInside.load() != 0;
  const bool stopped = !busy && g_pool.Stop();
  if (!stopped || g_inside.load() != 0 || g_sortInside.load() != 0) {
    Log("shadow recorder: a pass, a worker or a sort call was still running at unload; its objects are left alive");
    return false;
  }
  for (Casc& cs : g_casc) {
    FreeJob(cs.job);
    SafeRel(cs.learn.dsv);
    ReleasePool(cs.learn.pool);
    cs.learn = Learned();
    SafeRel(cs.b7);
  }
  FreeTables(g_tab);
  FreeTex(g_tex);
  g_pendMissCount = g_pendStaleCount = 0;
  g_pendEvict = false;
  for (auto*& p : g_offSrv) SafeRel(p);
  for (auto*& p : g_offBuf) SafeRel(p);
  SafeRel(g_ctx);
  g_state = -1;
  return true;
}

void ResetCounters() {
  for (Casc& cs : g_casc) {
    Stat& s = cs.st;
    s.passes = s.casters = s.recorded = s.textured = s.jobs = s.jobsUsed = s.refused = s.cbBytes = s.vt23 = 0;
    for (auto& a : s.passReason) a = 0;
    for (auto& a : s.casterReason) a = 0;
    s.buildNs = s.recordNs = s.waitNs = s.waitMaxNs = s.checkNs = s.execNs = s.restoreNs = 0;
    s.draws = s.drawsHelper = s.groups = s.splitJobs = s.helperLate = s.replayNs = s.executeNs = 0;
    s.preHelper = s.casterReads = 0;
    s.tlSamples = s.tlSnapSamples = s.tlHelperSamples = s.tlTop = s.early = 0;
    s.tlPass = s.tlStart = s.tlStageA = s.tlSnap = s.tlDone = s.tlHelper = 0;
    s.lateSamples = 0;
    s.latePass = s.lateStart = s.lateStageA = s.lateDone = s.lateP1 = s.lateSnap = s.tlP1 = 0;
    s.tlCommit = s.lateCommit = 0;
    s.helpersQueued = s.commitNs = s.resolveNs = 0;
    s.lateSnapSamples = 0;
    s.snapEarly = s.snapLate = s.snapKeys = s.snapPre = s.rearmLate = s.staleStart = 0;
    s.verifyPasses = s.verifyMismatch = s.verifyTexels = s.verifyBadTexels = s.verifyErrors = s.verifySkipped = 0;
  }
  g_probes = g_probeOdd = g_probeLeaders = g_probePool = 0;
  g_texBuilt = g_texRefreshed = g_texDeferred = g_texFull = g_rtReplays = 0;
  memset(g_texWhy, 0, sizeof(g_texWhy));
  g_sortCalls = g_sortStarts = 0;
}

void LogCounters(const char* label, double frames) {
  const double f = frames > 0 ? frames : 1.0;
  const uint32_t scope = g_scope.load();
  Log("  shadow recorder %s: scope 0x%x, %.0f frames%s; probes %llu (batching leaders drawn alone %llu, unusable "
      "%llu, inconclusive %llu, PS sampler pool differs %llu); keys %u (recordable %llu, not %llu, retired for a state change %llu, relearnt %llu), "
      "meshes %u (recordable %llu, not %llu), table full %llu; faults %llu",
      label, scope, frames, g_disabled.load() ? ", DISABLED" : "", static_cast<unsigned long long>(g_probes.load()),
      static_cast<unsigned long long>(g_probeLeaders.load()), static_cast<unsigned long long>(g_probeOdd.load()),
      static_cast<unsigned long long>(g_probeInconclusive.load()), static_cast<unsigned long long>(g_probePool.load()),
      g_tab.keysUsed,
      static_cast<unsigned long long>(g_keysOk.load()), static_cast<unsigned long long>(g_keysRejected.load()),
      static_cast<unsigned long long>(g_keyDowngrades.load()), static_cast<unsigned long long>(g_keyRelearnt.load()),
      g_tab.meshesUsed, static_cast<unsigned long long>(g_meshesOk.load()),
      static_cast<unsigned long long>(g_meshesRejected.load()), static_cast<unsigned long long>(g_tableFull.load()),
      static_cast<unsigned long long>(g_faults.load()));
  Log("  shadow recorder %s sort hook: %s; %.0f collection sorts/frame, %.2f job starts/frame from it", label,
      g_sortPatched ? "on" : "off (jobs start at render entry)", g_sortCalls.load() / f, g_sortStarts.load() / f);
  for (int c = 0; c < kSlots; ++c) {
    if (!InScope(scope, c)) continue;
    const Stat& s = g_casc[c].st;
    const uint64_t passes = s.passes.load(), exec = s.passReason[kPExecuted].load(), casters = s.casters.load();
    const uint64_t used = s.jobsUsed.load();
    auto ms = [&](const std::atomic<uint64_t>& ns) { return ns.load() / 1e6 / f; };
    Log("  shadow recorder %s cascade %d: %.2f passes/frame, %llu of %llu executed (%.1f%%); casters in the jobs used "
        "%.0f/frame, drawn by the command list %.0f/frame (%.1f%%, textured %.0f/frame); %llu jobs, %llu used, %llu "
        "refused (worker busy)",
        label, c, passes / f, static_cast<unsigned long long>(exec), static_cast<unsigned long long>(passes),
        passes ? 100.0 * exec / passes : 0.0, casters / f, s.recorded.load() / f,
        casters ? 100.0 * s.recorded.load() / casters : 0.0, s.textured.load() / f,
        static_cast<unsigned long long>(s.jobs.load()), static_cast<unsigned long long>(used),
        static_cast<unsigned long long>(s.refused.load()));
    Log("  shadow recorder %s cascade %d times (ms/frame): worker build %.3f + record %.3f (per job %.3f + %.3f; "
        "the primary's commit %.3f and key resolve %.3f per job); "
        "render thread: wait %.3f (max %.3f ms), checks %.3f, replay + b7 copy + execute %.3f, restore %.3f; CB ring "
        "%.2f MB/frame; streaming replays %.0f/frame; texture snapshots %llu early (%llu before render entry), %llu at "
        "the pass (%.0f keys/frame); jobs re-armed after a top-level pass (late, or a helper still ending) %llu, stale "
        "early starts dropped %llu",
        label, c, ms(s.buildNs), ms(s.recordNs), used ? s.buildNs.load() / 1e6 / used : 0.0,
        used ? s.recordNs.load() / 1e6 / used : 0.0, used ? s.commitNs.load() / 1e6 / used : 0.0,
        used ? s.resolveNs.load() / 1e6 / used : 0.0, ms(s.waitNs), s.waitMaxNs.load() / 1e6, ms(s.checkNs),
        ms(s.execNs), ms(s.restoreNs), s.cbBytes.load() / 1048576.0 / f, s.vt23.load() / f,
        static_cast<unsigned long long>(s.snapEarly.load()), static_cast<unsigned long long>(s.snapPre.load()),
        static_cast<unsigned long long>(s.snapLate.load()), s.snapKeys.load() / f,
        static_cast<unsigned long long>(s.rearmLate.load()), static_cast<unsigned long long>(s.staleStart.load()));
    const double tl = s.tlSamples.load() ? 1e6 * s.tlSamples.load() : 1.0;  // ns sums -> ms per job (signed)
    Log("  shadow recorder %s cascade %d draws: %.0f/frame for %.0f recorded casters (%.0f on helper lists, %llu split "
        "jobs, %.2f helpers queued per job, %llu helper late), %.0f groups/frame; render thread: replay+checks %.3f, "
        "ExecuteCommandList %.3f ms/frame; timeline from RenderGraph::render entry (ms, mean per used job, negative = "
        "before): start %.3f (%llu of %llu started by the sort hook), committed %.3f, stage A %.3f, snapshot %.3f "
        "(%llu jobs), primary done %.3f, helpers done %.3f (%llu), pass %.3f after %.1f top-level passes; stage A "
        "reads %.1f%% on the helpers",
        label, c, (s.draws.load() + s.drawsHelper.load()) / f, s.recorded.load() / f, s.drawsHelper.load() / f,
        static_cast<unsigned long long>(s.splitJobs.load()), used ? static_cast<double>(s.helpersQueued.load()) / used : 0.0,
        static_cast<unsigned long long>(s.helperLate.load()),
        s.groups.load() / f, ms(s.replayNs), ms(s.executeNs), s.tlStart.load() / tl,
        static_cast<unsigned long long>(s.early.load()), static_cast<unsigned long long>(s.tlSamples.load()),
        s.tlCommit.load() / tl, s.tlStageA.load() / tl,
        s.tlSnapSamples.load() ? s.tlSnap.load() / (1e6 * s.tlSnapSamples.load()) : 0.0,
        static_cast<unsigned long long>(s.tlSnapSamples.load()), s.tlDone.load() / tl,
        s.tlHelperSamples.load() ? s.tlHelper.load() / (1e6 * s.tlHelperSamples.load()) : 0.0,
        static_cast<unsigned long long>(s.tlHelperSamples.load()), s.tlPass.load() / tl,
        s.tlSamples.load() ? static_cast<double>(s.tlTop.load()) / s.tlSamples.load() : 0.0,
        s.casterReads.load() ? 100.0 * s.preHelper.load() / s.casterReads.load() : 0.0);
    std::string line;
    char buf[200];
    for (int r = 1; r < kPassReasons; ++r) {
      if (!s.passReason[r].load()) continue;
      snprintf(buf, sizeof(buf), "%s%s %llu", line.empty() ? "" : "; ", kPassReasonName[r],
               static_cast<unsigned long long>(s.passReason[r].load()));
      line += buf;
    }
    Log("  shadow recorder %s cascade %d passes drawn stock: %s", label, c, line.empty() ? "none" : line.c_str());
    line.clear();
    for (int r = 1; r < kCasterReasons; ++r) {
      if (!s.casterReason[r].load()) continue;
      snprintf(buf, sizeof(buf), "%s%s %.1f", line.empty() ? "" : "; ", kCasterReasonName[r],
               s.casterReason[r].load() / f);
      line += buf;
    }
    Log("  shadow recorder %s cascade %d residual casters/frame (DCS draws them): %s", label, c,
        line.empty() ? "none" : line.c_str());
    if (s.verifyPasses.load() || s.verifySkipped.load() || g_verify.load())
      Log("  shadow recorder %s cascade %d verify: %llu passes compared (%llu not compared: no command list "
          "executed), %llu with differences, %llu of %llu texels differ, %llu compare errors (format %d)",
          label, c, static_cast<unsigned long long>(s.verifyPasses.load()),
          static_cast<unsigned long long>(s.verifySkipped.load()),
          static_cast<unsigned long long>(s.verifyMismatch.load()),
          static_cast<unsigned long long>(s.verifyBadTexels.load()),
          static_cast<unsigned long long>(s.verifyTexels.load()), static_cast<unsigned long long>(s.verifyErrors.load()),
          static_cast<int>(shadowbatch::g_lastFormat));
  }
  Log("  shadow recorder %s texture table: %u entries live, %.1f built/frame, %.1f rebuilt/frame, %llu maintenances "
      "deferred (lock busy), %llu table full; render-target mip replays %.2f/frame; built as: usable %llu, class %llu, "
      "0x678 %llu, mip-set %llu, swap due %llu, fault %llu",
      label, g_tex.live, g_texBuilt.load() / f, g_texRefreshed.load() / f,
      static_cast<unsigned long long>(g_texDeferred.load()), static_cast<unsigned long long>(g_texFull.load()),
      g_rtReplays.load() / f, static_cast<unsigned long long>(g_texWhy[kTwOk]),
      static_cast<unsigned long long>(g_texWhy[kTwClass]), static_cast<unsigned long long>(g_texWhy[kTw678]),
      static_cast<unsigned long long>(g_texWhy[kTwMipSet]), static_cast<unsigned long long>(g_texWhy[kTwSwap]),
      static_cast<unsigned long long>(g_texWhy[kTwFault]));
  Log("  shadow recorder %s: texture class residuals (since start): no set array %llu, record out of range %llu; "
      "classes not replicated: %s",
      label, static_cast<unsigned long long>(g_classNoSets.load()), static_cast<unsigned long long>(g_classRange.load()),
      CensusText(g_census).c_str());
}


// ---------------------------------------------------------------------------
// Drawn-stock reasons between two snapshots (view profile lines)
// ---------------------------------------------------------------------------
// "name a.aa/frame" for the nonzero reasons from index first, most frequent
// first, at most top of them. Pure.
std::string ReasonList(const char* const* names, const uint64_t* a, const uint64_t* b, int n, int first,
                       double frames, int top) {
  std::vector<std::pair<uint64_t, int>> v;
  for (int r = first; r < n; ++r)
    if (b[r] > a[r]) v.push_back({b[r] - a[r], r});
  std::sort(v.begin(), v.end(), [](const std::pair<uint64_t, int>& x, const std::pair<uint64_t, int>& y) {
    return x.first != y.first ? x.first > y.first : x.second < y.second;
  });
  const double f = frames > 0 ? frames : 1.0;
  std::string s;
  char buf[256];
  for (size_t i = 0; i < v.size() && static_cast<int>(i) < top; ++i) {
    snprintf(buf, sizeof(buf), "%s%s %.2f", s.empty() ? "" : ", ", names[v[i].second], v[i].first / f);
    s += buf;
  }
  if (v.size() > static_cast<size_t>(top)) s += ", ...";
  return s.empty() ? "none" : s;
}

struct StockSnap {
  uint64_t passes = 0, executed = 0;
  uint64_t pass[kPassReasons] = {};
  uint64_t caster[kCasterReasons] = {};
  uint64_t late[kSlots] = {};
  uint64_t lateN[kSlots] = {};
  int64_t latePass[kSlots] = {}, lateStart[kSlots] = {}, lateStageA[kSlots] = {}, lateDone[kSlots] = {};
  uint64_t tlN[kSlots] = {}, tlSnapN[kSlots] = {}, lateSnapN[kSlots] = {};
  int64_t tlStageA[kSlots] = {}, tlDone[kSlots] = {}, tlPass[kSlots] = {}, tlP1[kSlots] = {}, tlSnap[kSlots] = {};
  int64_t lateP1[kSlots] = {}, lateSnap[kSlots] = {};
  int64_t tlCommit[kSlots] = {}, lateCommit[kSlots] = {};
  uint64_t used[kSlots] = {}, helpers[kSlots] = {};
  LoadSnap load;
};

StockSnap TakeStock() {
  StockSnap s;
  const auto rl = std::memory_order_relaxed;
  for (int c = 0; c < kSlots; ++c) {
    const Stat& st = g_casc[c].st;
    s.passes += st.passes.load(rl);
    for (int r = 0; r < kPassReasons; ++r) s.pass[r] += st.passReason[r].load(rl);
    for (int r = 0; r < kCasterReasons; ++r) s.caster[r] += st.casterReason[r].load(rl);
    s.late[c] = st.passReason[kPLate].load(rl);
    s.lateN[c] = st.lateSamples.load(rl);
    s.latePass[c] = st.latePass.load(rl);
    s.lateStart[c] = st.lateStart.load(rl);
    s.lateStageA[c] = st.lateStageA.load(rl);
    s.lateDone[c] = st.lateDone.load(rl);
    s.tlN[c] = st.tlSamples.load(rl);
    s.tlStageA[c] = st.tlStageA.load(rl);
    s.tlDone[c] = st.tlDone.load(rl);
    s.tlPass[c] = st.tlPass.load(rl);
    s.tlP1[c] = st.tlP1.load(rl);
    s.tlSnapN[c] = st.tlSnapSamples.load(rl);
    s.tlSnap[c] = st.tlSnap.load(rl);
    s.lateP1[c] = st.lateP1.load(rl);
    s.lateSnapN[c] = st.lateSnapSamples.load(rl);
    s.lateSnap[c] = st.lateSnap.load(rl);
    s.tlCommit[c] = st.tlCommit.load(rl);
    s.lateCommit[c] = st.lateCommit.load(rl);
    s.used[c] = st.jobsUsed.load(rl);
    s.helpers[c] = st.helpersQueued.load(rl);
  }
  s.executed = s.pass[kPExecuted];
  s.load = TakeLoad(g_load);
  return s;
}

// Per frame: the recorded cascades' passes drawn stock by reason (late per
// cascade), and the residual casters' top reasons.
std::string StockText(const StockSnap& a, const StockSnap& b, double frames) {
  const double f = frames > 0 ? frames : 1.0;
  char buf[256];
  snprintf(buf, sizeof(buf), "passes %.2f/frame, executed %.2f; late by cascade %.2f %.2f %.2f %.2f; stock: ",
           (b.passes - a.passes) / f, (b.executed - a.executed) / f, (b.late[0] - a.late[0]) / f,
           (b.late[1] - a.late[1]) / f, (b.late[2] - a.late[2]) / f, (b.late[3] - a.late[3]) / f);
  std::string s = buf;
  s += ReasonList(kPassReasonName, a.pass, b.pass, kPassReasons, 1, frames, 6);
  s += "; residual casters: ";
  s += ReasonList(kCasterReasonName, a.caster, b.caster, kCasterReasons, 2, frames, 4);  // not SMR/model: never
  // Per cascade from the render entry (ms): jobs in time (committed, stage A, untextured, done, pass; helpers per
  // used job), late jobs (pass, start, committed, stage A, untextured, done).
  for (int c = 0; c < kSlots; ++c) {
    const uint64_t m = b.tlN[c] - a.tlN[c];
    if (m) {
      const double d = 1e6 * m;
      const uint64_t sn = b.tlSnapN[c] - a.tlSnapN[c];
      const uint64_t u = b.used[c] - a.used[c];
      snprintf(buf, sizeof(buf), "; c%d in time (%llu, %.1f helpers): committed %.2f, stage A %.2f, untextured "
               "recorded %.2f, snapshot %.2f (%llu), done %.2f, pass %.2f",
               c, static_cast<unsigned long long>(m), u ? static_cast<double>(b.helpers[c] - a.helpers[c]) / u : 0.0,
               (b.tlCommit[c] - a.tlCommit[c]) / d, (b.tlStageA[c] - a.tlStageA[c]) / d, (b.tlP1[c] - a.tlP1[c]) / d,
               sn ? (b.tlSnap[c] - a.tlSnap[c]) / (1e6 * sn) : 0.0, static_cast<unsigned long long>(sn),
               (b.tlDone[c] - a.tlDone[c]) / d, (b.tlPass[c] - a.tlPass[c]) / d);
      s += buf;
    }
    const uint64_t n = b.lateN[c] - a.lateN[c];
    if (!n) continue;
    const double d = 1e6 * n;
    const uint64_t sn = b.lateSnapN[c] - a.lateSnapN[c];
    snprintf(buf, sizeof(buf),
             "; late c%d (%llu): pass %.2f, start %.2f, committed %.2f, stage A %.2f, untextured recorded %.2f, "
             "snapshot %.2f (%llu), done %.2f",
             c, static_cast<unsigned long long>(n), (b.latePass[c] - a.latePass[c]) / d,
             (b.lateStart[c] - a.lateStart[c]) / d, (b.lateCommit[c] - a.lateCommit[c]) / d,
             (b.lateStageA[c] - a.lateStageA[c]) / d, (b.lateP1[c] - a.lateP1[c]) / d,
             sn ? (b.lateSnap[c] - a.lateSnap[c]) / (1e6 * sn) : 0.0, static_cast<unsigned long long>(sn),
             (b.lateDone[c] - a.lateDone[c]) / d);
    s += buf;
  }
  s += "; ";
  s += LoadText(a.load, b.load, frames, g_tscHz);
  return s;
}
}  // namespace shrec
