// G-buffer recorder stage S0 counters (R18 section 7/8). Measurement only:
// nothing is drawn differently and no DCS memory is written. [Suite]
// GBufferRecCount (default 0): 1 s discovery, the G-buffer keys' compiles
// waited for, then a 5 s count.
//
// Tags: [V] verified in the binary (DCS 2.9.30) or the shader sources, [I]
// inferred. Facts from R18 unless stated.
//
// Per G-buffer pass execution (GraphicsCore vtable 0xd8770 slot 19, gb_batch.h
// gbpass, through its wrapper chain), on the render thread:
//  1. Item mix. The pass's item vector ([[ctx]+0x438] + id*24, read once by
//     0x88de0 at 0x896e1 [V]) is walked before the pass runs. Every item is
//     classed: NGModel SceneRenderable (vtbl 0x59810) with ModelMaterialMT by
//     model pass number [r+0x60] (1, 2, 8, other), cockpit (ctx byte [r+0x64],
//     normal_cockpit*), transparent (props+0x33) and the DX11Shader's
//     BLEND_MODE define (+0x98 define vector [V shadow_inst.h]; enums.hlsl:
//     BM_NONE 0 .. BM_SHADOWED_TRANSPARENT 6; absent = BM_NONE);
//     SceneRenderable with another material (RTTI name); every other
//     renderable class by vtable (shell, decals, terrain, characters, cloth).
//     These are the G-buffer-pass-only counts (R18 correction: gb_count.h's
//     21,938 draws/frame counted every pass).
//  2. Render-thread time inside the item loop by class: SceneRenderable vt[1]
//     through gb_count.h's override chain (our override in front of the
//     existing one, e.g. G-buffer batching's, for the phase), other classes
//     through a timing thunk on their vt[1] slot (only slots no other module
//     hooks). Pre-loop (frame buffer, clear, binder), loop, post-loop; our
//     probes inside the loop are measured and subtracted. Gate 1: model
//     (SceneRenderable + ModelMaterialMT) time >= 5 ms/frame.
//  3. Segments (R18 4, island rule). Candidate sets of recordable model
//     draws (model pass 1, not transparent): {BM_NONE}, {+BM_ALPHA_TEST},
//     {+BM_DECAL/_DEFERRED}, {+cockpit}; with islands 1/30/100 a recordable
//     run shorter than the island stays residual. Per execution: segments
//     (one Execute each), recorded draws and their time, residual time
//     between the first and the last segment and outside. Gate 2: median
//     segments per main execution <= 4 for BM_NONE+A2C, island 30, and the
//     residual items between the segments < 1 ms/frame. A main execution:
//     its slot's mean model items >= 25 % of the largest slot's.
//  4. Census (R18 3.1, 4): the resources every stage of the draw's technique
//     pass binds (RDEF names of shadow_inst's (a) compile of its G-buffer key,
//     PassGate::reads, pass 0 of normal* / normal_cockpit*). Mappable:
//     def_uniforms, the context buffers cPerFrame b6 / cPerView b7 /
//     cAmbientMap b8 [V common/context.hlsl, common/AmbientCube.hlsl],
//     sbPositions, samplers, and texture records the draw's own material sets
//     (handle at mat+0x240 with a texture in its entry). A read texture record
//     the material does not set (handle -1 or an empty entry) keeps the last
//     writer's view (FX variable inheritance, R18 3.1): "inherited". Gate 3:
//     strictly mappable draws >= 90 % of the model draws.
//  5. Pass setup at the first item (after pushFrameBuffer, clear, binder):
//     OMGetRenderTargets (8 RTVs + DSV), viewports, scissors, DX11Renderer
//     +0xd4 / +0x2120, the two values 0x15e20 turns into mat+0x114/0x118
//     (getCurrentFrameBuffer vt[54], getViewportDims vt[40]), the pass flag
//     bytes data+0x598..0x59b, VS/PS b6..b8 objects. Compared with the
//     previous execution of the same slot (render graph, collection index).
//     Gate 4: >= 99 % of the executions with OM, viewport, scissor, flags,
//     ratios and pass flags unchanged (context buffer objects reported, not
//     gated: b7 is rebound per execution).
//  6. Material bytes between RenderGraph::render entry and the pass (R18
//     3.2): at render entry (shadow_batch.h's observer; the import hook ours
//     for the phase if absent) def_uniforms (mat+0x90..0x1c0) of every
//     ModelMaterialMT in the learned G-buffer vectors is copied; at each
//     execution each material is compared once: the dwords the key's stages
//     read (shadow_inst CbUsedDwords of the technique pass; the whole CB when
//     not reflected) minus the per-draw overrides (+0xfc posStructOffset,
//     +0x20..+0x5f, +0x80..+0x8b, the animated properties' destinations).
//     Gate 5: 0 changes.
//  7. ExecuteCommandList(TRUE) at the first item of every 4th execution,
//     the G-buffer targets bound: an empty command list, and one that binds
//     the same RTVs, DSV and viewports (recorded on our deferred context; with
//     TRUE the immediate state is restored, so DCS's caches and split_filter.h's
//     shadow stay true: the filter forgets its shadow on any foreign Execute).
//     Gate 6: median of the binding list <= 15 us (R18 S1 gate, measured here).
//  8. Textures: per execution unique textures and (texture, size) pairs (the
//     vt[23] replays S2 would add), gb_batch.h PredictView results with the
//     item's size (mirror coverage; "swap pending" = getSRV would swap).
//     Untextured and empty-entry draws.
//  9. Timing: execution offset from RenderGraph::render entry and duration;
//     when the G-buffer collections' vectors became final (Scene 0x25bf0
//     return, shadow_rec.h's sort hook: observed when the recorder patched the
//     call sites, else patched for the phase with shadow_rec.h's code while
//     [Model] ShadowRecorder is 0) relative to render entry.
//
// Coexistence: our wrapper and override are chained in front of whatever is
// there (G-buffer batching's) and taken out with compare-exchange; the shadow
// recorder (cascade passes only), shadow batching and the split filter are
// not touched. With the key at 0 none of this runs. After the phase the
// chained pointers, class slots, render observer and sort observer are gone;
// slot 19 goes back when we hooked it (unless [Model] GBufferBatching=1); the
// SceneRenderable hook and shadow_inst's G-buffer observer stay registered as
// pass-throughs (as after [Suite] GBufferTexCount).
// Included once from main.cpp inside its anonymous namespace, after
// pass_timing.h, gb_count.h, shadow_tex.h, shadow_inst.h, shadow_batch.h,
// gb_batch.h, split_filter.h and shadow_rec.h.
#pragma once

namespace gbreccount {

// ---------------------------------------------------------------------------
// Constants and pure helpers (offline tested)
// ---------------------------------------------------------------------------
constexpr int kSlots = 16;    // learned executions (render graph, collection); index kSlots = not learnable
constexpr int kMaxNs = 16;    // other renderable classes (by vtable)
constexpr int kMatCls = 8;    // SceneRenderable with other material classes
constexpr int kModelKinds = 128;
enum : int {
  kClsMat0 = kModelKinds,
  kClsMatOver = kClsMat0 + kMatCls,
  kClsSrNull,
  kClsNs0,
  kClsNsOver = kClsNs0 + kMaxNs,
  kClasses
};
static_assert(kClasses <= 255, "classes are stored in a byte");
constexpr size_t kMaxItems = 1 << 15;
constexpr uint32_t kCbOff = 0x90, kCbLen = 0x130, kCbDwords = kCbLen / 4;
constexpr size_t kMatTable = 1 << 14;   // power of two
constexpr size_t kTexSet = 1 << 14;     // power of two
constexpr size_t kBlendTable = 4096;    // power of two
constexpr size_t kCensusTable = 2048;   // power of two
constexpr uint32_t kMaxRecords = 512;   // texture records per shader in the census mask
constexpr int kSets = 5;                // 4 candidate sets + (set 1 and census-clean)
constexpr int kIslands = 3;
constexpr uint32_t kIsland[kIslands] = {1, 30, 100};
constexpr int kGateSet = 1, kGateIsland = 1;  // BM_NONE + A2C, island 30
constexpr size_t kMaxSamples = 16384;
constexpr int kMaxExec = 512;
constexpr int kExecEvery = 4;
constexpr int32_t kNoSlot = -1;

enum Blend : int {
  kBmNone = 0,
  kBmTransparent,
  kBmAlphaTest,
  kBmAdditive,
  kBmDecal,
  kBmDecalDeferred,
  kBmShadowedTransparent,
  kBmUnknown
};
const char* const kBlendName[8] = {"BM_NONE",  "BM_TRANSPARENT",    "BM_ALPHA_TEST",           "BM_ADDITIVE",
                                   "BM_DECAL", "BM_DECAL_DEFERRED", "BM_SHADOWED_TRANSPARENT", "blend unknown"};

// BLEND_MODE define value -> code (model/common/enums.hlsl [V]); nullptr
// (define absent: #if sees 0) = BM_NONE.
inline int BlendCode(const char* v) {
  if (!v) return kBmNone;
  for (int i = 0; i < kBmUnknown; ++i)
    if (strcmp(v, kBlendName[i]) == 0) return i;
  if (v[0] >= '0' && v[0] <= '6' && v[1] == 0) return v[0] - '0';
  return kBmUnknown;
}

// Model kind code: pass bucket (1, 2, 8, other) << 5 | cockpit << 4 |
// transparent << 3 | blend.
inline int PassBucket(uint32_t pass) { return pass == 1 ? 0 : pass == 2 ? 1 : pass == 8 ? 2 : 3; }
inline int KindCode(uint32_t pass, bool cockpit, bool transparent, int blend) {
  return PassBucket(pass) << 5 | (cockpit ? 16 : 0) | (transparent ? 8 : 0) | (blend & 7);
}
inline std::string KindName(int k) {
  static const char* const kPass[4] = {"pass 1", "pass 2", "pass 8", "pass other"};
  std::string s = std::string("SceneRenderable ModelMaterialMT ") + kPass[(k >> 5) & 3];
  if (k & 16) s += " cockpit";
  if (k & 8) s += " transparent";
  s += " ";
  s += kBlendName[k & 7];
  return s;
}

// Item flags for the candidate sets.
enum : uint8_t { kFNone = 1, kFA2c = 2, kFDecal = 4, kFCockpit = 8, kFClean = 16, kFModel = 32 };
const char* const kSetName[kSets] = {"BM_NONE", "BM_NONE+A2C", "+decals", "+cockpit", "BM_NONE+A2C census-clean"};

// Set 0: opaque BM_NONE; 1: + alpha test; 2: + decals; 3: + the cockpit
// technique of those; 4: set 1 restricted to census-clean draws.
inline bool InSet(uint8_t f, int set) {
  if (!(f & (kFNone | kFA2c | kFDecal))) return false;
  if (set == 4) return (f & kFClean) && !(f & kFCockpit) && !(f & kFDecal);
  if ((f & kFCockpit) && set < 3) return false;
  if (f & kFDecal) return set >= 2;
  if (f & kFA2c) return set >= 1;
  return true;
}

struct SegResult {
  uint32_t segments = 0, recItems = 0, resBetweenItems = 0;
  uint64_t recCyc = 0, resBetweenCyc = 0, resOutsideCyc = 0;
};

// One execution's item vector in order: runs of >= island consecutive items
// of the set become segments (one ExecuteCommandList each), everything else
// is residual (DCS draws it in place). cyc may be nullptr.
inline SegResult Segments(const uint8_t* flags, const uint32_t* cyc, size_t n, int set, uint32_t island) {
  SegResult r;
  uint64_t pend = 0, total = 0;
  uint32_t pendItems = 0;
  bool any = false;
  size_t i = 0;
  while (i < n) {
    if (!InSet(flags[i], set)) {
      const uint64_t c = cyc ? cyc[i] : 0;
      pend += c;
      total += c;
      ++pendItems;
      ++i;
      continue;
    }
    size_t j = i;
    uint64_t s = 0;
    while (j < n && InSet(flags[j], set)) {
      if (cyc) s += cyc[j];
      ++j;
    }
    total += s;
    const uint32_t len = static_cast<uint32_t>(j - i);
    if (len >= island) {
      if (any) {
        r.resBetweenCyc += pend;
        r.resBetweenItems += pendItems;
      }
      any = true;
      pend = 0;
      pendItems = 0;
      ++r.segments;
      r.recItems += len;
      r.recCyc += s;
    } else {
      pend += s;
      pendItems += len;
    }
    i = j;
  }
  r.resOutsideCyc = total - r.recCyc - r.resBetweenCyc;
  return r;
}

inline double Median(std::vector<double> v) {
  if (v.empty()) return 0;
  std::sort(v.begin(), v.end());
  const size_t m = v.size() / 2;
  return v.size() % 2 ? v[m] : 0.5 * (v[m - 1] + v[m]);
}
inline double Quantile(std::vector<double> v, double q) {
  if (v.empty()) return 0;
  std::sort(v.begin(), v.end());
  return v[std::min(v.size() - 1, static_cast<size_t>(v.size() * q))];
}

// def_uniforms dword masks (bit i = CB bytes 4i..4i+3).
inline void MarkCb(uint64_t m[2], int64_t off, int64_t size) {
  for (int64_t d = off / 4; d < (off + size + 3) / 4; ++d)
    if (d >= 0 && d < static_cast<int64_t>(kCbDwords)) m[d >> 6] |= 1ull << (d & 63);
}
// What 0x16140 / 0x15e20 write per draw before the upload [V NGModel]:
// posStructOffset (+0xfc = mat+0x18c), the per-item matrix (+0x20..+0x5f =
// mat+0xb0..0xef), 1.0 and the two ratios (+0x80..+0x8b = mat+0x110..0x11b).
inline void BaseOverrides(uint64_t m[2]) {
  MarkCb(m, 0xfc, 4);
  MarkCb(m, 0x20, 0x40);
  MarkCb(m, 0x80, 0xc);
}
inline bool DwordsDiffer(const uint8_t* a, const uint8_t* b, const uint64_t mask[2]) {
  for (uint32_t d = 0; d < kCbDwords; ++d)
    if ((mask[d >> 6] >> (d & 63) & 1) && memcmp(a + 4 * d, b + 4 * d, 4) != 0) return true;
  return false;
}

// Bound-resource name classes (census).
enum NameCat : int { kNcMaterialCb, kNcPerFrame, kNcPerView, kNcAmbient, kNcPositions, kNcSampler, kNcTexture, kNcOther, kNameCats };
const char* const kNameCatName[kNameCats] = {"def_uniforms", "cPerFrame b6", "cPerView b7", "cAmbientMap b8",
                                             "sbPositions",  "samplers",     "material textures", "other"};
// kNcOther here means "not a fixed name": the caller then checks the
// shader's texture records (kNcTexture) before calling it unmappable.
inline int NameCatOf(const char* n) {
  if (!n) return kNcOther;
  if (strcmp(n, "def_uniforms") == 0) return kNcMaterialCb;
  if (strcmp(n, "cPerFrame") == 0) return kNcPerFrame;
  if (strcmp(n, "cPerView") == 0) return kNcPerView;
  if (strcmp(n, "cAmbientMap") == 0) return kNcAmbient;
  if (strcmp(n, "sbPositions") == 0) return kNcPositions;
  if (strstr(n, "ampler")) return kNcSampler;  // sampler state variables (Sampler/sampler)
  return kNcOther;
}

// Gates (R18 8): 1 pass, 0 fail, -1 not measured.
struct GateIn {
  double modelMs = 0;
  bool segMeasured = false;
  double medianSeg = 0, resBetweenMs = 0;
  bool censusMeasured = false;
  double censusPct = 0;
  uint64_t stableSamples = 0;
  double stablePct = 0;
  bool guardMeasured = false;
  uint64_t guardChanges = 0;
  bool execMeasured = false;
  double execUs = 0;
};
struct GateOut {
  int g[6] = {-1, -1, -1, -1, -1, -1};
  int verdict = 0;  // 1 GO, 0 NO-GO, -1 incomplete (a gate 1-5 not measured, none failed)
  bool trigger = false;  // an R18 no-go trigger (segments > 8, model share < 3.5 ms)
};
inline GateOut Evaluate(const GateIn& in) {
  GateOut o;
  o.g[0] = in.modelMs >= 5.0 ? 1 : 0;
  o.g[1] = !in.segMeasured ? -1 : (in.medianSeg <= 4.0 && in.resBetweenMs < 1.0) ? 1 : 0;
  o.g[2] = !in.censusMeasured ? -1 : in.censusPct >= 90.0 ? 1 : 0;
  o.g[3] = !in.stableSamples ? -1 : in.stablePct >= 99.0 ? 1 : 0;
  o.g[4] = !in.guardMeasured ? -1 : in.guardChanges == 0 ? 1 : 0;
  o.g[5] = !in.execMeasured ? -1 : in.execUs <= 15.0 ? 1 : 0;
  o.trigger = in.modelMs < 3.5 || (in.segMeasured && in.medianSeg > 8.0);
  bool fail = o.trigger, missing = false;
  for (int i = 0; i < 6; ++i) {
    fail |= o.g[i] == 0;
    if (i < 5) missing |= o.g[i] < 0;
  }
  o.verdict = fail ? 0 : missing ? -1 : 1;
  return o;
}
inline const char* GateWord(int g) { return g > 0 ? "PASS" : g == 0 ? "FAIL" : "NOT MEASURED"; }

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------
std::atomic<bool> g_on{false};
std::atomic<bool> g_chained{false};
std::atomic<int> g_inside{0};
std::atomic<DWORD> g_renderTid{0};
double g_tscHz = 0;
void* g_srVt = nullptr;
void* g_modelMatVt = nullptr;
void* g_shaderVt = nullptr;
uint8_t* g_md = nullptr;
ID3D11DeviceContext* g_ctx = nullptr;
ID3D11DeviceContext* g_dc = nullptr;  // our deferred context (Execute timing)
bool g_texOk = false, g_ratioOk = false, g_census = false;
gbbatch::TexEnv g_env;
gbpass::WrapFn g_prevWrap = nullptr;
gbcount::OverrideFn g_prevOverride = nullptr;
shadowbatch::RenderObserverFn g_prevObserver = nullptr;
bool g_ownRenderHook = false, g_ownPassHook = false, g_ownSort = false, g_sortObserved = false;

// ---- Classes ----
using VtFn = uint64_t(__fastcall*)(void*, void*, void*, void*);
struct NsClass {
  void* vt;
  void** slot;
  int state;  // 0 = not hooked, 1 = hooked, -1 = slot hooked by another module, -2 = no module
  bool timed;
  char name[160];
};
NsClass g_ns[kMaxNs];
std::atomic<int> g_nsCount{0};
void* g_nsOrig[kMaxNs] = {};
struct MatClass {
  void* vt;
  char name[96];
};
MatClass g_matCls[kMatCls];
int g_matClsCount = 0;

// ---- Statistics (render thread; read after the phase) ----
struct ClassStat {
  uint64_t items, calls, cyc;
};
struct SetStat {
  uint64_t segments, recItems, recCyc, resBetweenCyc, resOutsideCyc, resBetweenItems;
};
struct Setup {
  void* rtv[8];
  void* dsv;
  UINT nrt, nvp, nsc;
  D3D11_VIEWPORT vp[4];
  D3D11_RECT sc[4];
  uint32_t flags, dbg;
  bool rendererOk;
  void* fb;
  int32_t vpd[2];
  bool ratioOk;
  uint8_t passFlags[4];
  bool passFlagsOk;
  void* cb[2][3];  // VS, PS b6..b8
  UINT cbBytes[3];
};
struct SlotStat {
  uint64_t passes, emptyPasses, origCalls, faults, oversize;
  uint64_t cycWrap, cycOrig, cycPre, cycLoop, cycPost, cycProbe, cycAnalyse;
  uint64_t items, modelItems;
  ClassStat cls[kClasses];
  // Timing against render entry and the sort.
  uint64_t offN, offCyc, offMaxCyc, noEntry, sortN, sortLeadCyc, sortMissing, sortLate;
  int64_t sortMinLead;
  SetStat seg[kSets][kIslands];
  // Setup stability.
  uint64_t samples, unstable, chRtv, chDsv, chVp, chSc, chFlags, chDbg, chRatio, chPassFlags, chCtxCb, rendererMissing;
  bool have;
  Setup last;
  void* rtv0Seen[4];
  int rtv0SeenCount;
  bool rtv0SeenOver;
  // Materials, once per material per execution.
  uint64_t mats, matsNoSnap, matsNoRefl, matsGuardChg, matsCbChg, matsNoEntry, itemsGuardChg;
  // Census of model pass-1 opaque draws.
  uint64_t cenDraws, cenClean, cenInherit, cenOther, cenPending, cenFailed, cenNotG, nullEntryDraws, untextured;
  // Textures.
  uint64_t texSets, texUnique, texSizeUnique, texReason[gbbatch::kReasons];
  uint64_t texBadHandle;
};
SlotStat g_stat[kSlots + 1];
struct Learned {
  void* rg;
  uint32_t idx;
};
Learned g_learned[kSlots];
std::atomic<int> g_learnedCount{0};
struct ExecSample {
  uint8_t slot;
  uint16_t seg[kSets][kIslands];
  uint32_t modelItems;
};
std::vector<ExecSample> g_samples;  // reserved before the phase; never grown by the render thread
uint64_t g_samplesDropped = 0;
double g_execUs[2][kMaxExec];
int g_execN[2] = {};
uint64_t g_execFail = 0, g_execSeq = 0;
uint64_t g_unplanned = 0, g_analyseFaults = 0;
uint64_t g_snapRenders = 0, g_snapCyc = 0, g_snapMats = 0, g_snapFaults = 0, g_snapNoVector = 0, g_tableWipes = 0;
std::atomic<uint64_t> g_sortCalls{0};
std::atomic<uint64_t> g_sortTick[kSlots];
uint64_t g_entryTick = 0;
DWORD g_driverCl = 0;
bool g_driverOk = false;

// ---- Material table (render thread) ----
enum : uint8_t { kMfNoSnap = 1, kMfNoRefl = 2, kMfGuardChg = 4, kMfCbChg = 8 };
struct MatEntry {
  uint8_t* mat;
  uint32_t snapGen, passGen;
  uint8_t flags;
  uint8_t cb[kCbLen];
};
MatEntry* g_mats = nullptr;
size_t g_matsUsed = 0;
uint32_t g_renderGen = 0, g_passGen = 0;

// ---- Per-shader BLEND_MODE (render thread) ----
struct BlendEntry {
  void* shader;
  void* effect;
  int code;
};
BlendEntry g_blend[kBlendTable];

// ---- Census per (DX11Shader, technique) (render thread) ----
struct CensusEntry {
  void* shader;
  uint64_t tech;
  void* effect;
  void* techBegin;
  int state;  // 0 pending, 1 ready, 2 failed
  uint32_t tryGen;
  uint64_t recMask[kMaxRecords / 64];
  uint32_t cats;
  uint32_t otherCount;
  std::string other, why, key;
  uint64_t draws, clean, inherit, otherDraws;
};
CensusEntry* g_cen = nullptr;  // kCensusTable entries
size_t g_cenUsed = 0;
uint64_t g_cenFull = 0;

// ---- Per-pass state (render thread) ----
uint8_t g_cls[kMaxItems];
uint8_t g_flags[kMaxItems];
uint32_t g_cyc[kMaxItems];
struct TexSlot {
  uint64_t key;
  uint32_t gen;
};
TexSlot g_texSet[kTexSet];
TexSlot g_texSizeSet[kTexSet];
thread_local bool t_inPass = false;
thread_local int t_depth = 0;
thread_local int t_slot = kSlots;
thread_local void* t_pass = nullptr;
thread_local void* const* t_begin = nullptr;
thread_local size_t t_n = 0, t_cursor = 0;
thread_local uint64_t t_first = 0, t_last = 0, t_probe = 0;
thread_local gbpass::ExecFn t_origFn = nullptr;

inline size_t PtrHash(uint64_t p, size_t mask) {
  return static_cast<size_t>((p >> 4) * 0x9E3779B97F4A7C15ull >> 40) & mask;
}

MatEntry* MatFind(uint8_t* mat) {
  if (!g_mats) return nullptr;
  size_t i = PtrHash(reinterpret_cast<uint64_t>(mat), kMatTable - 1);
  for (size_t k = 0; k < kMatTable; ++k, i = (i + 1) & (kMatTable - 1)) {
    MatEntry& e = g_mats[i];
    if (e.mat == mat) return &e;
    if (!e.mat) {
      if (g_matsUsed >= kMatTable * 3 / 4) return nullptr;
      e.mat = mat;
      e.snapGen = e.passGen = 0;
      e.flags = 0;
      ++g_matsUsed;
      return &e;
    }
  }
  return nullptr;
}

// True when key is new in this execution's set.
bool SetInsert(TexSlot* set, uint64_t key) {
  size_t i = PtrHash(key, kTexSet - 1);
  for (size_t k = 0; k < kTexSet; ++k, i = (i + 1) & (kTexSet - 1)) {
    TexSlot& s = set[i];
    if (s.gen != g_passGen) {
      s.key = key;
      s.gen = g_passGen;
      return true;
    }
    if (s.key == key) return false;
  }
  return false;
}

// ---- Class lookup ----
void VtName(void* vt, char* buf, size_t n) {
  buf[0] = 0;
  void* fake = vt;
  ptiming::RttiRaw(&fake, buf, n);
  if (!buf[0]) snprintf(buf, n, "(no RTTI)");
}

int MatClassOf(void* vt) {
  for (int i = 0; i < g_matClsCount; ++i)
    if (g_matCls[i].vt == vt) return kClsMat0 + i;
  if (g_matClsCount == kMatCls) return kClsMatOver;
  MatClass& m = g_matCls[g_matClsCount];
  m.vt = vt;
  VtName(vt, m.name, sizeof(m.name));
  return kClsMat0 + g_matClsCount++;
}

int NsClassOf(void* vt) {
  const int n = g_nsCount.load(std::memory_order_relaxed);
  for (int i = 0; i < n; ++i)
    if (g_ns[i].vt == vt) return kClsNs0 + i;
  if (n >= kMaxNs) return kClsNsOver;
  NsClass& c = g_ns[n];
  c.vt = vt;
  c.slot = static_cast<void**>(vt) + 1;
  c.state = 0;
  c.timed = false;
  char rtti[112];
  VtName(vt, rtti, sizeof(rtti));
  HMODULE m = nullptr;
  char mod[MAX_PATH] = "?";
  uintptr_t rva = 0;
  if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                         static_cast<LPCWSTR>(vt), &m) &&
      m) {
    char path[MAX_PATH];
    if (GetModuleFileNameA(m, path, MAX_PATH)) {
      const char* b = strrchr(path, '\\');
      strncpy_s(mod, b ? b + 1 : path, _TRUNCATE);
    }
    rva = reinterpret_cast<uintptr_t>(vt) - reinterpret_cast<uintptr_t>(m);
  } else {
    c.state = -2;
  }
  snprintf(c.name, sizeof(c.name), "%s %s!0x%llx", rtti, mod, static_cast<unsigned long long>(rva));
  g_nsCount.store(n + 1, std::memory_order_release);
  return kClsNs0 + n;
}

std::string ClassName(int c) {
  if (c < kModelKinds) return KindName(c);
  if (c == kClsMatOver) return "SceneRenderable, other material (class table full)";
  if (c == kClsSrNull) return "SceneRenderable without item or material";
  if (c == kClsNsOver) return "other renderable (class table full)";
  if (c >= kClsMat0 && c < kClsMat0 + g_matClsCount) return std::string("SceneRenderable ") + g_matCls[c - kClsMat0].name;
  const int i = c - kClsNs0;
  if (i < 0 || i >= g_nsCount.load()) return "?";
  return std::string(g_ns[i].name) + (g_ns[i].timed ? "" : g_ns[i].state == -1 ? " [untimed: slot hooked elsewhere]"
                                                                                  : " [untimed]");
}

// ---- BLEND_MODE of a DX11Shader (define vector at +0x98, 0xa0-byte entries,
// name char[] at +8, value char[] at +0x58 [V shadow_inst.h]) ----
int ReadBlendRaw(uint8_t* sh) {
  uint8_t* db = *reinterpret_cast<uint8_t**>(sh + 0x98);
  uint8_t* de = *reinterpret_cast<uint8_t**>(sh + 0xa0);
  if (db == de) return kBmNone;  // no defines (an empty std::vector holds null pointers)
  if (!db || de < db || (de - db) % 0xa0 != 0 || (de - db) / 0xa0 > 256) return kBmUnknown;
  for (uint8_t* e = db; e < de; e += 0xa0) {
    const char* name = reinterpret_cast<const char*>(e + 8);
    if (!memchr(name, 0, 0x48) || strcmp(name, "BLEND_MODE") != 0) continue;
    const char* v = reinterpret_cast<const char*>(e + 0x58);
    return memchr(v, 0, 0x48) ? BlendCode(v) : kBmUnknown;
  }
  return kBmNone;
}

int BlendOf(uint8_t* sh) {
  void* effect = *reinterpret_cast<void**>(sh + 0x50);
  size_t i = PtrHash(reinterpret_cast<uint64_t>(sh), kBlendTable - 1);
  for (size_t k = 0; k < kBlendTable; ++k, i = (i + 1) & (kBlendTable - 1)) {
    BlendEntry& e = g_blend[i];
    if (e.shader == sh && e.effect == effect) return e.code;
    if (!e.shader || e.shader == sh) {
      e.shader = sh;
      e.effect = effect;
      e.code = ReadBlendRaw(sh);
      return e.code;
    }
  }
  return ReadBlendRaw(sh);
}

// ---- Census ----
// Index of the shader's first named parameter record (0x50 bytes at
// [sh+0xc8], name char* at +0x30) the binding name refers to; -1 if none.
int RecordOfGuarded(uint8_t* sh, const char* name) {
  __try {
    const uint8_t* b = *reinterpret_cast<uint8_t**>(sh + 0xc8);
    const uint8_t* e = *reinterpret_cast<uint8_t**>(sh + 0xd0);
    if (!b || e < b) return -1;
    const int64_t n = (e - b) / 0x50;
    for (int64_t r = 0; r < n && r < 4096; ++r) {
      const char* rn = *reinterpret_cast<const char* const*>(b + r * 0x50 + 0x30);
      if (rn && shadowtex::NameRefers(rn, name)) return static_cast<int>(r);
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }
  return -1;
}

void ResolveCensus(CensusEntry& e) {
  std::vector<std::string> bound;
  std::string why, key;
  const int st = shadowinst::GbPassReadTextures(e.shader, e.tech, 0, &bound, nullptr, &why, &key);
  if (st == shadowtex::kReadsPending) return;
  e.key = key;
  if (st != shadowtex::kReadsReady) {
    e.state = 2;
    e.why = why;
    return;
  }
  for (const std::string& n : bound) {
    int c = NameCatOf(n.c_str());
    if (c == kNcOther) {
      const int r = RecordOfGuarded(static_cast<uint8_t*>(e.shader), n.c_str());
      if (r >= 0 && r < static_cast<int>(kMaxRecords)) {
        e.recMask[r >> 6] |= 1ull << (r & 63);
        c = kNcTexture;
      } else {
        e.other += (e.other.empty() ? "" : ",") + n;
        ++e.otherCount;
      }
    }
    e.cats |= 1u << c;
  }
  e.state = 1;
}

CensusEntry* CensusFind(void* shader, uint64_t tech) {
  if (!g_cen) return nullptr;
  uint8_t* s = static_cast<uint8_t*>(shader);
  void* effect = *reinterpret_cast<void**>(s + 0x50);
  void* techBegin = *reinterpret_cast<void**>(s + 0xb0);
  size_t i = PtrHash(reinterpret_cast<uint64_t>(shader) ^ tech * 0x9E37ull, kCensusTable - 1);
  for (size_t k = 0; k < kCensusTable; ++k, i = (i + 1) & (kCensusTable - 1)) {
    CensusEntry& e = g_cen[i];
    if (e.shader == shader && e.tech == tech && e.effect == effect && e.techBegin == techBegin) return &e;
    if (!e.shader) {
      if (g_cenUsed >= kCensusTable * 3 / 4) {
        ++g_cenFull;
        return nullptr;
      }
      e.shader = shader;
      e.tech = tech;
      e.effect = effect;
      e.techBegin = techBegin;
      e.state = 0;
      e.tryGen = 0;
      ++g_cenUsed;
      return &e;
    }
  }
  return nullptr;
}

// ---- Materials ----
void AnimOverrides(uint8_t* mat, uint64_t m[2]) {
  uint8_t* b = *reinterpret_cast<uint8_t**>(mat + 8);
  uint8_t* e = *reinterpret_cast<uint8_t**>(mat + 0x10);
  if (!b || e <= b || (e - b) % 16 || (e - b) / 16 > 256) return;
  for (uint8_t* p = b; p < e; p += 16) {
    uint8_t* dst = *reinterpret_cast<uint8_t**>(p);
    uint8_t* prop = *reinterpret_cast<uint8_t**>(p + 8);
    uint32_t bytes = 16;  // unknown class: widest write
    if (prop && g_md) {
      const void* pv = *reinterpret_cast<void**>(prop);
      for (const gbbatch::PropClass& pc : gbbatch::kProps)
        if (pv == g_md + pc.vtbl) bytes = pc.bytes;
    }
    MarkCb(m, (dst - mat) - static_cast<int64_t>(kCbOff), bytes);
  }
}

void VisitMat(MatEntry& e, uint8_t* mat, uint8_t* shader, uint64_t tech, SlotStat& st) {
  e.passGen = g_passGen;
  uint8_t f = 0;
  uint64_t over[2] = {};
  BaseOverrides(over);
  AnimOverrides(mat, over);
  uint64_t all[2] = {~0ull, (1ull << (kCbDwords - 64)) - 1};
  all[0] &= ~over[0];
  all[1] &= ~over[1];
  uint64_t guard[2] = {all[0], all[1]};
  uint64_t used[2];
  if (shader && shadowinst::CbUsedDwords(shader, tech, 0, used)) {
    guard[0] = used[0] & all[0];
    guard[1] = used[1] & all[1];
  } else {
    f |= kMfNoRefl;
  }
  if (!e.snapGen || e.snapGen != g_renderGen) {
    f |= kMfNoSnap;
  } else {
    const uint8_t* now = mat + kCbOff;
    if (DwordsDiffer(e.cb, now, guard)) f |= kMfGuardChg;
    if (DwordsDiffer(e.cb, now, all)) f |= kMfCbChg;
  }
  e.flags = f;
  st.mats++;
  if (f & kMfNoSnap) st.matsNoSnap++;
  if (f & kMfNoRefl) st.matsNoRefl++;
  if (f & kMfGuardChg) st.matsGuardChg++;
  if (f & kMfCbChg) st.matsCbChg++;
}

// ---- The walk before the pass (plain C body: SEH) ----
inline uint8_t* EntriesOf(uint8_t* mat, uint8_t* item) {
  const uint8_t* props = *reinterpret_cast<uint8_t**>(mat + 0x28);
  auto* arr = *reinterpret_cast<uint8_t***>(item + 0x18);
  if (!props || !arr || !*arr) return nullptr;
  return *arr + static_cast<uint64_t>(*reinterpret_cast<const uint32_t*>(props + 0x26c)) * 0x18;
}

void ModelItem(size_t k, uint8_t* r, uint8_t* item, uint8_t* mat, SlotStat& st) {
  const uint32_t pass = *reinterpret_cast<uint32_t*>(r + 0x60);
  const uint8_t* props = *reinterpret_cast<uint8_t**>(mat + 0x28);
  const bool transparent = props && props[0x33] != 0;
  const bool cockpit = r[0x64] != 0;
  uint8_t* sh = *reinterpret_cast<uint8_t**>(mat + 0x30);
  const bool dxShader = sh && *reinterpret_cast<void**>(sh) == g_shaderVt;
  const int blend = dxShader ? BlendOf(sh) : kBmUnknown;
  const int kind = KindCode(pass, cockpit, transparent, blend);
  g_cls[k] = static_cast<uint8_t>(kind);
  uint8_t f = kFModel;
  const uint64_t tech = *reinterpret_cast<uint64_t*>(mat + (cockpit ? 0x1e0 : 0x1d8));
  const bool gbuffer = pass == 1 && !transparent && dxShader;
  if (gbuffer) {
    if (blend == kBmNone) f |= kFNone;
    else if (blend == kBmAlphaTest) f |= kFA2c;
    else if (blend == kBmDecal || blend == kBmDecalDeferred) f |= kFDecal;
    if (cockpit) f |= kFCockpit;
  }
  // Material bytes since render entry.
  if (MatEntry* e = MatFind(mat)) {
    if (e->passGen != g_passGen) VisitMat(*e, mat, dxShader ? sh : nullptr, tech, st);
    if (e->flags & kMfGuardChg) st.itemsGuardChg++;
  } else {
    st.matsNoEntry++;
  }
  // Textures: handles, entries, prediction, uniqueness.
  const uint32_t ntex = *reinterpret_cast<uint32_t*>(mat + 0x2d8);
  uint64_t boundRec[kMaxRecords / 64] = {};
  bool anyTex = false, nullEntry = false;
  const uint8_t* recs = dxShader ? *reinterpret_cast<uint8_t**>(sh + 0xc8) : nullptr;
  const uint8_t* recEnd = dxShader ? *reinterpret_cast<uint8_t**>(sh + 0xd0) : nullptr;
  const int64_t nrec = recs && recEnd > recs ? (recEnd - recs) / 0x50 : 0;
  const uint8_t* en = ntex ? EntriesOf(mat, item) : nullptr;
  const uint64_t size = *reinterpret_cast<uint64_t*>(r + 0x6c);
  for (uint32_t i = 0; i < ntex && i < 32; ++i) {
    const int64_t h = *reinterpret_cast<int64_t*>(mat + 0x240 + 8 * i);
    if (h == -1) continue;
    anyTex = true;
    uint8_t* tex = en ? *reinterpret_cast<uint8_t* const*>(en + i * 0x18 + 8) : nullptr;
    if (!tex) {
      nullEntry = true;
      continue;
    }
    if (h >= 0 && h < static_cast<int64_t>(kMaxRecords)) boundRec[h >> 6] |= 1ull << (h & 63);
    if (!gbuffer) continue;
    st.texSets++;
    if (SetInsert(g_texSet, reinterpret_cast<uint64_t>(tex))) st.texUnique++;
    if (SetInsert(g_texSizeSet, reinterpret_cast<uint64_t>(tex) * 0x9E3779B97F4A7C15ull ^ size)) st.texSizeUnique++;
    if (!g_texOk) continue;
    if (h < 0 || h >= nrec) {
      st.texBadHandle++;
      continue;
    }
    const int32_t type = *reinterpret_cast<const int32_t*>(recs + h * 0x50 + 0xc);
    void* view = nullptr;
    const uint8_t why =
        gbbatch::PredictView(g_env, tex, *reinterpret_cast<const int64_t*>(en + i * 0x18), type, size, &view);
    st.texReason[why < gbbatch::kReasons ? why : 0]++;
  }
  if (gbuffer) {
    if (!anyTex) st.untextured++;
    if (nullEntry) st.nullEntryDraws++;
    st.cenDraws++;
    CensusEntry* ce = g_census ? CensusFind(sh, tech) : nullptr;
    if (ce && ce->state == 0 && ce->tryGen != g_passGen) {
      ce->tryGen = g_passGen;
      ResolveCensus(*ce);
    }
    if (!ce || ce->state == 0) {
      st.cenPending++;
    } else if (ce->state == 2) {
      st.cenFailed++;
    } else {
      ce->draws++;
      bool inherit = false;
      for (uint32_t w = 0; w < kMaxRecords / 64; ++w) inherit |= (ce->recMask[w] & ~boundRec[w]) != 0;
      if (ce->otherCount) {
        st.cenOther++;
        ce->otherDraws++;
      } else if (inherit) {
        st.cenInherit++;
        ce->inherit++;
      } else {
        st.cenClean++;
        ce->clean++;
        f |= kFClean;
      }
    }
  } else if (pass == 1 && !transparent) {
    st.cenNotG++;  // no DX11Shader
  }
  g_flags[k] = f;
  st.modelItems++;
}

bool AnalyseRaw(void* const* b, size_t n, SlotStat& st) {
  __try {
    for (size_t k = 0; k < n && k < kMaxItems; ++k) {
      g_cyc[k] = 0;
      g_flags[k] = 0;
      auto* r = static_cast<uint8_t*>(b[k]);
      int c = kClsSrNull;
      if (!r) {
        c = kClsSrNull;
      } else if (*reinterpret_cast<void**>(r) != g_srVt) {
        c = NsClassOf(*reinterpret_cast<void**>(r));
      } else {
        auto* item = *reinterpret_cast<uint8_t**>(r + 0x10);
        auto* mat = item ? *reinterpret_cast<uint8_t**>(item + 0x10) : nullptr;
        if (!mat) {
          c = kClsSrNull;
        } else if (*reinterpret_cast<void**>(mat) != g_modelMatVt) {
          c = MatClassOf(*reinterpret_cast<void**>(mat));
        } else {
          ModelItem(k, r, item, mat, st);
          c = g_cls[k];
        }
      }
      g_cls[k] = static_cast<uint8_t>(c);
      st.cls[c].items++;
      st.items++;
    }
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

// ---- Render entry: material snapshots, sort lead ----
void SnapshotRaw(void* rg, void* renderables) {
  __try {
    const size_t count = shadowbatch::VectorCountRaw(static_cast<uint8_t*>(rg), renderables);
    if (!count) {
      g_snapNoVector++;
      return;
    }
    const int nl = g_learnedCount.load(std::memory_order_relaxed);
    for (int k = 0; k < nl; ++k) {
      if (g_learned[k].rg != rg || g_learned[k].idx >= count) continue;
      void* const* vec = reinterpret_cast<void* const*>(static_cast<uint8_t*>(renderables) + g_learned[k].idx * 24);
      void* const* b = static_cast<void* const*>(vec[0]);
      void* const* e = static_cast<void* const*>(vec[1]);
      if (!b || e < b || e - b > 200000) continue;
      for (void* const* p = b; p < e; ++p) {
        void* r = *p;
        if (!r || *static_cast<void**>(r) != g_srVt) continue;
        auto* item = *reinterpret_cast<uint8_t**>(static_cast<uint8_t*>(r) + 0x10);
        auto* mat = item ? *reinterpret_cast<uint8_t**>(item + 0x10) : nullptr;
        if (!mat || *reinterpret_cast<void**>(mat) != g_modelMatVt) continue;
        MatEntry* me = MatFind(mat);
        if (!me || me->snapGen == g_renderGen) continue;
        memcpy(me->cb, mat + kCbOff, kCbLen);
        me->snapGen = g_renderGen;
        g_snapMats++;
      }
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    g_snapFaults++;
  }
}

void OnRender(void* rg, void* renderables) {
  if (shadowbatch::RenderObserverFn p = g_prevObserver) p(rg, renderables);
  if (!g_on.load(std::memory_order_relaxed) || GetCurrentThreadId() != g_renderTid.load(std::memory_order_relaxed))
    return;
  g_inside.fetch_add(1);
  const uint64_t t0 = __rdtsc();
  g_entryTick = t0;
  // The collections' vectors became final before this entry (sort lead).
  const int nl = g_learnedCount.load(std::memory_order_relaxed);
  for (int s = 0; s < nl; ++s) {
    if (g_learned[s].rg != rg) continue;
    SlotStat& st = g_stat[s];
    const uint64_t tick = g_sortTick[s].exchange(0);
    if (!g_sortObserved) continue;
    if (!tick) {
      st.sortMissing++;
      continue;
    }
    const int64_t lead = static_cast<int64_t>(t0 - tick);
    if (lead < 0) {
      st.sortLate++;
      continue;
    }
    st.sortN++;
    st.sortLeadCyc += static_cast<uint64_t>(lead);
    if (st.sortN == 1 || lead < st.sortMinLead) st.sortMinLead = lead;
  }
  if (g_matsUsed >= kMatTable * 3 / 4) {
    memset(g_mats, 0, kMatTable * sizeof(MatEntry));
    g_matsUsed = 0;
    g_tableWipes++;
  }
  if (++g_renderGen == 0) g_renderGen = 1;
  SnapshotRaw(rg, renderables);
  g_snapRenders++;
  g_snapCyc += __rdtsc() - t0;
  g_inside.fetch_sub(1);
}

// Pool threads (Scene's per-collection sort returned): a learned vector is final.
void OnSorted(void** out) {
  if (!g_on.load(std::memory_order_relaxed)) return;
  g_sortCalls.fetch_add(1, std::memory_order_relaxed);
  const int nl = g_learnedCount.load(std::memory_order_acquire);
  for (int s = 0; s < nl; ++s) {
    uint8_t* base = shrec::ArrayBaseGuarded(g_learned[s].rg);
    if (base && reinterpret_cast<uint8_t*>(out) == base + static_cast<size_t>(g_learned[s].idx) * 24)
      g_sortTick[s].store(__rdtsc(), std::memory_order_relaxed);
  }
}

// ---- Probes at the first item (render thread; time subtracted) ----
bool ReadRendererRaw(Setup& s) {
  __try {
    if (auto* r = reinterpret_cast<uint8_t*>(shadowbatch::g_rendererObj)) {
      s.flags = *reinterpret_cast<uint32_t*>(r + 0xd4);
      s.dbg = *reinterpret_cast<uint32_t*>(r + 0x2120);
      s.rendererOk = true;
    }
    if (g_ratioOk) {
      s.fb = gbbatch::FbNow();
      gbbatch::VpNow(s.vpd);
      s.ratioOk = true;
    }
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

// ExecuteCommandList(TRUE) with the G-buffer targets bound: variant 0 an
// empty list, variant 1 a list binding the same RTVs, DSV and viewports.
void ExecProbe(ID3D11RenderTargetView* const* rtv, ID3D11DepthStencilView* dsv, UINT nvp,
               const D3D11_VIEWPORT* vp) {
  const int v = static_cast<int>((g_execSeq / kExecEvery) & 1);
  if (g_execN[v] >= kMaxExec) return;
  if (v == 1) {
    g_dc->OMSetRenderTargets(8, rtv, dsv);
    if (nvp) g_dc->RSSetViewports(nvp, vp);
  }
  ID3D11CommandList* cl = nullptr;
  if (FAILED(g_dc->FinishCommandList(FALSE, &cl)) || !cl) {
    g_execFail++;
    return;
  }
  const uint64_t a = __rdtsc();
  g_ctx->ExecuteCommandList(cl, TRUE);
  const uint64_t b = __rdtsc();
  cl->Release();
  g_execUs[v][g_execN[v]++] = (b - a) * 1e6 / g_tscHz;
}

void ProbeSetup(SlotStat& st) {
  Setup s;
  memset(&s, 0, sizeof(s));
  ID3D11RenderTargetView* rtv[8] = {};
  ID3D11DepthStencilView* dsv = nullptr;
  g_ctx->OMGetRenderTargets(8, rtv, &dsv);
  D3D11_VIEWPORT vps[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
  UINT nvp = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
  g_ctx->RSGetViewports(&nvp, vps);
  D3D11_RECT scs[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
  UINT nsc = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
  g_ctx->RSGetScissorRects(&nsc, scs);
  ID3D11Buffer* cb[2][3] = {};
  g_ctx->VSGetConstantBuffers(6, 3, cb[0]);
  g_ctx->PSGetConstantBuffers(6, 3, cb[1]);
  for (int i = 0; i < 8; ++i) {
    s.rtv[i] = rtv[i];
    s.nrt += rtv[i] != nullptr;
  }
  s.dsv = dsv;
  s.nvp = nvp;
  for (UINT i = 0; i < nvp && i < 4; ++i) s.vp[i] = vps[i];
  s.nsc = nsc;
  for (UINT i = 0; i < nsc && i < 4; ++i) s.sc[i] = scs[i];
  for (int k = 0; k < 2; ++k)
    for (int i = 0; i < 3; ++i) s.cb[k][i] = cb[k][i];
  for (int i = 0; i < 3; ++i) {
    ID3D11Buffer* b = cb[1][i] ? cb[1][i] : cb[0][i];
    if (b) {
      D3D11_BUFFER_DESC d{};
      b->GetDesc(&d);
      s.cbBytes[i] = d.ByteWidth;
    }
  }
  if (!ReadRendererRaw(s)) s.rendererOk = s.ratioOk = false;
  __try {
    memcpy(s.passFlags, static_cast<uint8_t*>(t_pass) + 0x90 + 0x598, 4);
    s.passFlagsOk = true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    s.passFlagsOk = false;
  }
  if (g_dc && (g_execSeq++ % kExecEvery) == 0) ExecProbe(rtv, dsv, nvp < 4 ? nvp : 4, vps);
  st.samples++;
  if (!s.rendererOk) st.rendererMissing++;
  if (st.have) {
    const Setup& p = st.last;
    const bool cRtv = memcmp(s.rtv, p.rtv, sizeof(s.rtv)) != 0;
    const bool cDsv = s.dsv != p.dsv;
    const bool cVp = s.nvp != p.nvp || memcmp(s.vp, p.vp, sizeof(s.vp)) != 0;
    const bool cSc = s.nsc != p.nsc || memcmp(s.sc, p.sc, sizeof(s.sc)) != 0;
    const bool cFlags = s.rendererOk && p.rendererOk && s.flags != p.flags;
    const bool cDbg = s.rendererOk && p.rendererOk && s.dbg != p.dbg;
    const bool cRatio = s.ratioOk && p.ratioOk && (s.fb != p.fb || s.vpd[0] != p.vpd[0] || s.vpd[1] != p.vpd[1]);
    const bool cPass = s.passFlagsOk && p.passFlagsOk && memcmp(s.passFlags, p.passFlags, 4) != 0;
    const bool cCb = memcmp(s.cb, p.cb, sizeof(s.cb)) != 0;
    st.chRtv += cRtv;
    st.chDsv += cDsv;
    st.chVp += cVp;
    st.chSc += cSc;
    st.chFlags += cFlags;
    st.chDbg += cDbg;
    st.chRatio += cRatio;
    st.chPassFlags += cPass;
    st.chCtxCb += cCb;
    if (cRtv || cDsv || cVp || cSc || cFlags || cDbg || cRatio || cPass) st.unstable++;
  }
  st.have = true;
  st.last = s;
  bool seen = false;
  for (int i = 0; i < st.rtv0SeenCount; ++i) seen |= st.rtv0Seen[i] == s.rtv[0];
  if (!seen) {
    if (st.rtv0SeenCount < 4) st.rtv0Seen[st.rtv0SeenCount++] = s.rtv[0];
    else st.rtv0SeenOver = true;
  }
  // Identity only: the pointers are compared, never used after release.
  for (auto* p : rtv)
    if (p) p->Release();
  if (dsv) dsv->Release();
  for (auto& row : cb)
    for (auto* p : row)
      if (p) p->Release();
}

// ---- Timed item call ----
int ItemAt(void* self) {
  for (size_t k = t_cursor; k < t_n; ++k)
    if (t_begin[k] == self) {
      t_cursor = k + 1;
      return static_cast<int>(k);
    }
  return -1;
}

template <typename Call>
uint64_t TimedCall(int fallback, void* self, Call&& call) {
  SlotStat& st = g_stat[t_slot];
  const uint64_t t0 = __rdtsc();
  if (!t_first) {
    t_first = t0;
    if (g_ctx) ProbeSetup(st);
  }
  const int k = ItemAt(self);
  int c = k >= 0 ? g_cls[k] : fallback;
  if (k < 0) g_unplanned++;
  const uint64_t t1 = __rdtsc();
  ++t_depth;
  const uint64_t ret = call();
  --t_depth;
  const uint64_t t2 = __rdtsc();
  const uint64_t d = t2 - t1;
  st.cls[c].calls++;
  st.cls[c].cyc += d;
  if (k >= 0) g_cyc[k] = d > 0xffffffffull ? 0xffffffffu : static_cast<uint32_t>(d);
  const uint64_t t3 = __rdtsc();
  t_probe += (t1 - t0) + (t3 - t2);
  t_last = t3;
  return ret;
}

uint64_t SrCall(void* self, void* ctx) {
  uint64_t r;
  if (gbcount::OverrideFn p = g_prevOverride)
    if (p(self, ctx, &r)) return r;
  return gbcount::g_orig(self, ctx);
}

bool Override(void* self, void* ctx, uint64_t* ret) {
  if (!t_inPass || t_depth) {
    if (gbcount::OverrideFn p = g_prevOverride) return p(self, ctx, ret);
    return false;
  }
  *ret = TimedCall(kClsSrNull, self, [&] { return SrCall(self, ctx); });
  return true;
}

template <int I>
uint64_t __fastcall NsHook(void* a, void* b, void* c, void* d) {
  auto orig = reinterpret_cast<VtFn>(g_nsOrig[I]);
  if (!t_inPass || t_depth) return orig(a, b, c, d);
  return TimedCall(kClsNs0 + I, a, [&] { return orig(a, b, c, d); });
}
static_assert(kMaxNs == 16, "kNsHooks lists one thunk per class");
void* const kNsHooks[kMaxNs] = {
    reinterpret_cast<void*>(&NsHook<0>),  reinterpret_cast<void*>(&NsHook<1>),  reinterpret_cast<void*>(&NsHook<2>),
    reinterpret_cast<void*>(&NsHook<3>),  reinterpret_cast<void*>(&NsHook<4>),  reinterpret_cast<void*>(&NsHook<5>),
    reinterpret_cast<void*>(&NsHook<6>),  reinterpret_cast<void*>(&NsHook<7>),  reinterpret_cast<void*>(&NsHook<8>),
    reinterpret_cast<void*>(&NsHook<9>),  reinterpret_cast<void*>(&NsHook<10>), reinterpret_cast<void*>(&NsHook<11>),
    reinterpret_cast<void*>(&NsHook<12>), reinterpret_cast<void*>(&NsHook<13>), reinterpret_cast<void*>(&NsHook<14>),
    reinterpret_cast<void*>(&NsHook<15>)};

// ---- The G-buffer execute ----
void __fastcall TimedOrig(void* pass, void* ctx) {
  SlotStat& st = g_stat[t_slot];
  t_first = t_last = t_probe = 0;
  t_cursor = 0;
  t_inPass = true;
  const uint64_t a = __rdtsc();
  t_origFn(pass, ctx);
  const uint64_t b = __rdtsc();
  t_inPass = false;
  st.origCalls++;
  st.cycOrig += b - a;
  if (t_first) {
    st.cycPre += t_first - a;
    st.cycLoop += t_last - t_first;
    st.cycPost += b - t_last;
    st.cycProbe += t_probe;
  } else {
    st.emptyPasses++;
  }
}

int SlotOf(void* ctx, void** vec) {
  __try {
    auto* rg = *static_cast<uint8_t**>(ctx);
    auto* base = *reinterpret_cast<uint8_t**>(rg + 0x438);
    const intptr_t d = reinterpret_cast<uint8_t*>(vec) - base;
    if (!base || d < 0 || d % 24 != 0 || d / 24 > 0xffff) return kSlots;
    const uint32_t idx = static_cast<uint32_t>(d / 24);
    const int n = g_learnedCount.load(std::memory_order_relaxed);
    for (int k = 0; k < n; ++k)
      if (g_learned[k].rg == rg && g_learned[k].idx == idx) return k;
    if (n == kSlots) return kSlots;
    g_learned[n] = {rg, idx};
    g_learnedCount.store(n + 1, std::memory_order_release);
    return n;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return kSlots;
  }
}

void AfterPass(int slot, size_t n, uint32_t modelItems) {
  SlotStat& st = g_stat[slot];
  ExecSample es{};
  es.slot = static_cast<uint8_t>(slot);
  es.modelItems = modelItems;
  for (int s = 0; s < kSets; ++s)
    for (int i = 0; i < kIslands; ++i) {
      const SegResult r = Segments(g_flags, g_cyc, n, s, kIsland[i]);
      SetStat& a = st.seg[s][i];
      a.segments += r.segments;
      a.recItems += r.recItems;
      a.recCyc += r.recCyc;
      a.resBetweenCyc += r.resBetweenCyc;
      a.resOutsideCyc += r.resOutsideCyc;
      a.resBetweenItems += r.resBetweenItems;
      es.seg[s][i] = static_cast<uint16_t>(r.segments < 0xffff ? r.segments : 0xffff);
    }
  if (g_samples.size() < g_samples.capacity()) g_samples.push_back(es);
  else g_samplesDropped++;
}

// The vector's begin/end, read under SEH (the vector comes from an item index
// ItemVector does not range-check): false when unreadable.
bool ReadVecGuarded(void** vec, void* const** b, void* const** e) {
  __try {
    *b = static_cast<void* const*>(vec[0]);
    *e = static_cast<void* const*>(vec[1]);
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

void Wrap(void* pass, void* ctx, gbpass::ExecFn orig) {
  const gbpass::WrapFn prev = g_prevWrap;
  const DWORD tid = GetCurrentThreadId();
  if (g_on.load(std::memory_order_relaxed) && !g_renderTid.load(std::memory_order_relaxed)) g_renderTid = tid;
  if (!g_on.load(std::memory_order_relaxed) || t_inPass || tid != g_renderTid.load(std::memory_order_relaxed)) {
    if (prev) prev(pass, ctx, orig);
    else orig(pass, ctx);
    return;
  }
  g_inside.fetch_add(1);
  const uint64_t t0 = __rdtsc();
  void** vec = gbpass::ItemVector(pass, ctx);
  const int slot = vec ? SlotOf(ctx, vec) : kSlots;
  SlotStat& st = g_stat[slot];
  if (g_entryTick) {
    const uint64_t off = t0 - g_entryTick;
    st.offN++;
    st.offCyc += off;
    if (off > st.offMaxCyc) st.offMaxCyc = off;
  } else {
    st.noEntry++;
  }
  t_begin = nullptr;
  t_n = 0;
  size_t n = 0;
  const uint64_t model0 = st.modelItems;
  void* const* b = nullptr;
  void* const* e = nullptr;
  if (vec && !ReadVecGuarded(vec, &b, &e)) {
    st.faults++;
    g_analyseFaults++;
  } else if (vec) {
    if (b && e >= b && e - b <= 200000) {
      if (++g_passGen == 0) g_passGen = 1;
      n = static_cast<size_t>(e - b);
      if (n > kMaxItems) {
        st.oversize++;
        n = kMaxItems;
      }
      if (AnalyseRaw(b, n, st)) {
        t_begin = b;
        t_n = n;
      } else {
        st.faults++;
        g_analyseFaults++;
        n = 0;
      }
    }
  }
  st.cycAnalyse += __rdtsc() - t0;
  t_slot = slot;
  t_pass = pass;
  t_origFn = orig;
  const uint64_t a = __rdtsc();
  if (prev) prev(pass, ctx, &TimedOrig);
  else TimedOrig(pass, ctx);
  st.cycWrap += __rdtsc() - a;
  st.passes++;
  if (t_n) AfterPass(slot, n, static_cast<uint32_t>(st.modelItems - model0));
  t_begin = nullptr;
  t_n = 0;
  g_inside.fetch_sub(1);
}

// ---------------------------------------------------------------------------
// Install / teardown
// ---------------------------------------------------------------------------
void HookClasses() {
  const int n = g_nsCount.load(std::memory_order_acquire);
  for (int i = 0; i < n; ++i) {
    NsClass& c = g_ns[i];
    c.timed = false;
    if (c.state == -2 || c.state == 1) continue;
    if (*c.slot != SlotOriginal(c.slot)) {
      c.state = -1;  // another module's hook: untimed, never touched
      continue;
    }
    g_nsOrig[i] = *c.slot;
    c.state = HookSlot(c.slot, kNsHooks[i], nullptr) ? 1 : -1;
    c.timed = c.state == 1;
  }
}

void UnhookClasses() {
  for (int i = 0; i < kMaxNs; ++i) {
    NsClass& c = g_ns[i];
    if (c.state != 1) continue;
    UnhookSlot(c.slot, g_nsOrig[i]);  // g_nsOrig stays for calls in flight
    c.state = 0;
  }
}

void ReleaseSort() {
  shrec::SortObserverFn so = &OnSorted;
  shrec::g_sortObserver.compare_exchange_strong(so, nullptr);
  if (g_ownSort) {
    if (!shrec::PatchSortSites(false)) Log("  gbuffer rec S0: WARNING could not restore Scene's sort call sites");
    g_ownSort = false;
  }
  shrec::g_sortBorrowed = false;
}

void Unchain() {
  g_on = false;
  if (!g_chained.exchange(false)) return;
  gbcount::OverrideFn ov = &Override;
  gbcount::g_override.compare_exchange_strong(ov, g_prevOverride);
  gbpass::WrapFn w = &Wrap;
  gbpass::g_wrap.compare_exchange_strong(w, g_prevWrap);
  shadowbatch::RenderObserverFn ob = &OnRender;
  shadowbatch::g_renderObserver.compare_exchange_strong(ob, g_prevObserver);
  ReleaseSort();
  UnhookClasses();
  if (g_ownRenderHook && shadowbatch::g_state.load() != 1) {
    if (shadowbatch::g_renderSlot) UnhookSlot(shadowbatch::g_renderSlot, reinterpret_cast<void*>(shadowbatch::g_origRender));
    if (shadowbatch::g_renderSlot2) UnhookSlot(shadowbatch::g_renderSlot2, reinterpret_cast<void*>(shadowbatch::g_origRender));
    shadowbatch::g_renderSlot = nullptr;
    shadowbatch::g_renderSlot2 = nullptr;
  }
  g_ownRenderHook = false;
}

// Waits for passes, render entries and sort calls in flight, then drops the
// contexts.
bool Drain() {
  for (int i = 0; i < 400 && (g_inside.load() != 0 || shrec::g_sortInside.load() != 0); ++i) Sleep(5);
  if (g_inside.load() != 0) return false;
  if (g_dc) g_dc->Release();
  if (g_ctx) g_ctx->Release();
  g_dc = nullptr;
  g_ctx = nullptr;
  return true;
}

// Payload stop.
void Shutdown() {
  if (!g_chained.load()) return;
  Unchain();
  if (shrec::g_sortStub && !shrec::g_sortPatched)
    reinterpret_cast<std::atomic<void*>*>(shrec::g_sortStub + 16)->store(reinterpret_cast<void*>(shrec::g_sortOrig));
  if (!Drain()) Log("gbuffer rec counter: a pass was still running at unload; its context references are left");
}

void ResetStats() {
  for (SlotStat& st : g_stat) memset(&st, 0, sizeof(st));
  g_samples.clear();
  g_samplesDropped = 0;
  g_execN[0] = g_execN[1] = 0;
  g_execFail = g_execSeq = 0;
  g_unplanned = g_analyseFaults = 0;
  g_snapRenders = g_snapCyc = g_snapMats = g_snapFaults = g_snapNoVector = g_tableWipes = 0;
  g_sortCalls = 0;
  for (auto& t : g_sortTick) t = 0;
  if (g_cen)
    for (size_t i = 0; i < kCensusTable; ++i) g_cen[i].draws = g_cen[i].clean = g_cen[i].inherit = g_cen[i].otherDraws = 0;
  g_cenFull = 0;
  g_entryTick = 0;
}

// ---------------------------------------------------------------------------
// Report
// ---------------------------------------------------------------------------
SlotStat Totals() {
  SlotStat t;
  memset(&t, 0, sizeof(t));
  for (int s = 0; s <= kSlots; ++s) {
    const SlotStat& a = g_stat[s];
    const uint64_t* src = reinterpret_cast<const uint64_t*>(&a);
    uint64_t* dst = reinterpret_cast<uint64_t*>(&t);
    // Counters only: every field up to sortMinLead is a uint64_t sum.
    const size_t sums = offsetof(SlotStat, sortMinLead) / sizeof(uint64_t);
    for (size_t i = 0; i < sums; ++i) dst[i] += src[i];
    for (int x = 0; x < kSets; ++x)
      for (int i = 0; i < kIslands; ++i) {
        t.seg[x][i].segments += a.seg[x][i].segments;
        t.seg[x][i].recItems += a.seg[x][i].recItems;
        t.seg[x][i].recCyc += a.seg[x][i].recCyc;
        t.seg[x][i].resBetweenCyc += a.seg[x][i].resBetweenCyc;
        t.seg[x][i].resOutsideCyc += a.seg[x][i].resOutsideCyc;
        t.seg[x][i].resBetweenItems += a.seg[x][i].resBetweenItems;
      }
    t.samples += a.samples;
    t.unstable += a.unstable;
    t.chRtv += a.chRtv;
    t.chDsv += a.chDsv;
    t.chVp += a.chVp;
    t.chSc += a.chSc;
    t.chFlags += a.chFlags;
    t.chDbg += a.chDbg;
    t.chRatio += a.chRatio;
    t.chPassFlags += a.chPassFlags;
    t.chCtxCb += a.chCtxCb;
    t.rendererMissing += a.rendererMissing;
    t.mats += a.mats;
    t.matsNoSnap += a.matsNoSnap;
    t.matsNoRefl += a.matsNoRefl;
    t.matsGuardChg += a.matsGuardChg;
    t.matsCbChg += a.matsCbChg;
    t.matsNoEntry += a.matsNoEntry;
    t.itemsGuardChg += a.itemsGuardChg;
    t.cenDraws += a.cenDraws;
    t.cenClean += a.cenClean;
    t.cenInherit += a.cenInherit;
    t.cenOther += a.cenOther;
    t.cenPending += a.cenPending;
    t.cenFailed += a.cenFailed;
    t.cenNotG += a.cenNotG;
    t.nullEntryDraws += a.nullEntryDraws;
    t.untextured += a.untextured;
    t.texSets += a.texSets;
    t.texUnique += a.texUnique;
    t.texSizeUnique += a.texSizeUnique;
    t.texBadHandle += a.texBadHandle;
    for (int r = 0; r < gbbatch::kReasons; ++r) t.texReason[r] += a.texReason[r];
  }
  return t;
}

double ExecMedian(int v) { return Median(std::vector<double>(g_execUs[v], g_execUs[v] + g_execN[v])); }
double ExecQ(int v, double q) { return Quantile(std::vector<double>(g_execUs[v], g_execUs[v] + g_execN[v]), q); }

void Report(double f) {
  const double ms = 1000.0 / g_tscHz / f;  // cycles -> ms/frame
  const double cycMs = 1000.0 / g_tscHz;   // cycles -> ms
  auto pct = [](double a, double b) { return b > 0 ? 100.0 * a / b : 0.0; };
  const SlotStat t = Totals();
  uint64_t timedCyc = 0, modelCyc = 0, modelItems = 0, modelCalls = 0;
  for (int c = 0; c < kClasses; ++c) {
    timedCyc += t.cls[c].cyc;
    if (c < kModelKinds) {
      modelCyc += t.cls[c].cyc;
      modelItems += t.cls[c].items;
      modelCalls += t.cls[c].calls;
    }
  }
  const double loopNet = static_cast<double>(t.cycLoop) - static_cast<double>(t.cycProbe);
  const int nl = g_learnedCount.load();
  Log("  gbuffer rec S0: %.0f frames; %d executions learned; %.2f executions/frame (%.2f without items, %.2f DCS "
      "executes), %.0f items/frame (G-buffer passes only), %.0f ModelMaterialMT draws/frame (%.0f timed); %llu walk "
      "faults, %llu oversized vectors, %llu item calls outside the walked vector%s",
      f, nl, t.passes / f, t.emptyPasses / f, t.origCalls / f, t.items / f, modelItems / f, modelCalls / f,
      static_cast<unsigned long long>(t.faults), static_cast<unsigned long long>(t.oversize),
      static_cast<unsigned long long>(g_unplanned),
      gbbatch::g_on.load() && gbbatch::g_state.load() == 1 ? "; NOTE [Model] GBufferBatching is on: batched passes measured" : "");
  Log("  gbuffer rec S0 time (render thread, ms/frame): G-buffer wrap %.3f = other wrappers %.3f + DCS execute %.3f; "
      "execute = pre-loop (attachments, frame buffer, clears, binder) %.3f + loop %.3f + post-loop %.3f; loop net of "
      "our probes %.3f (probes %.3f); our walk before each execution %.3f (not in the above)",
      t.cycWrap * ms, (static_cast<double>(t.cycWrap) - t.cycOrig) * ms, t.cycOrig * ms, t.cycPre * ms, t.cycLoop * ms,
      t.cycPost * ms, loopNet * ms, t.cycProbe * ms, t.cycAnalyse * ms);
  Log("  gbuffer rec S0 classes (items/frame, timed calls/frame, ms/frame, ns/call, share of net loop):");
  std::vector<int> order;
  for (int c = 0; c < kClasses; ++c)
    if (t.cls[c].items || t.cls[c].calls) order.push_back(c);
  std::sort(order.begin(), order.end(), [&](int a, int b) { return t.cls[a].cyc > t.cls[b].cyc; });
  for (int c : order) {
    const ClassStat& k = t.cls[c];
    Log("    %-78.78s %8.1f %8.1f %7.3f %6.0f %5.1f%%", ClassName(c).c_str(), k.items / f, k.calls / f, k.cyc * ms,
        k.calls ? k.cyc * 1e9 / g_tscHz / k.calls : 0.0, pct(static_cast<double>(k.cyc), loopNet));
  }
  // By model pass number (the R18 correction: G-buffer-only counts).
  {
    double byPass[4] = {}, msPass[4] = {};
    for (int c = 0; c < kModelKinds; ++c) {
      byPass[(c >> 5) & 3] += t.cls[c].items / f;
      msPass[(c >> 5) & 3] += t.cls[c].cyc * ms;
    }
    Log("  gbuffer rec S0 ModelMaterialMT draws inside G-buffer passes by model pass number (per frame): pass 1 %.0f "
        "(%.3f ms), pass 2 %.0f (%.3f ms), pass 8 %.0f (%.3f ms), other %.0f (%.3f ms)",
        byPass[0], msPass[0], byPass[1], msPass[1], byPass[2], msPass[2], byPass[3], msPass[3]);
  }
  // Per execution slot.
  double maxModel = 0;
  double slotModel[kSlots] = {};
  for (int s = 0; s < nl; ++s) {
    slotModel[s] = g_stat[s].passes ? static_cast<double>(g_stat[s].modelItems) / g_stat[s].passes : 0.0;
    maxModel = std::max(maxModel, slotModel[s]);
  }
  bool mainSlot[kSlots] = {};
  for (int s = 0; s < nl; ++s) mainSlot[s] = maxModel > 0 && slotModel[s] >= 0.25 * maxModel;
  for (int s = 0; s <= kSlots; ++s) {
    const SlotStat& a = g_stat[s];
    if (!a.passes) continue;
    uint64_t mc = 0;
    for (int c = 0; c < kModelKinds; ++c) mc += a.cls[c].cyc;
    std::vector<double> segs;
    for (const ExecSample& e : g_samples)
      if (e.slot == s) segs.push_back(e.seg[kGateSet][kGateIsland]);
    if (s == kSlots) {
      Log("  execution (not learnable): %.2f/frame, %.0f items/frame", a.passes / f, a.items / f);
      continue;
    }
    char sortBuf[160];
    if (!g_sortObserved)
      snprintf(sortBuf, sizeof(sortBuf), "not observed (sort hook unavailable)");
    else if (!a.sortN)
      snprintf(sortBuf, sizeof(sortBuf), "never seen by the sort hook (%llu entries)",
               static_cast<unsigned long long>(a.sortMissing + a.sortLate));
    else
      snprintf(sortBuf, sizeof(sortBuf), "%.3f ms before entry (min %.3f; %llu entries without a sort, %llu after)",
               a.sortLeadCyc * cycMs / a.sortN, a.sortMinLead * cycMs, static_cast<unsigned long long>(a.sortMissing),
               static_cast<unsigned long long>(a.sortLate));
    Log("  execution slot %d%s (graph %p, collection %u): %.2f/frame, items %.0f (model %.0f) per execution; ms per "
        "execution: wrap %.3f (pre %.3f, loop %.3f, model %.3f, post %.3f); start %.3f ms after render entry (max "
        "%.3f, %llu without an entry); vector final %s; segments (BM_NONE+A2C, island 30) median %.1f, max %.0f",
        s, mainSlot[s] ? " main" : "", g_learned[s].rg, g_learned[s].idx, a.passes / f,
        static_cast<double>(a.items) / a.passes, static_cast<double>(a.modelItems) / a.passes,
        a.cycWrap * cycMs / a.passes, a.cycPre * cycMs / a.passes, a.cycLoop * cycMs / a.passes,
        mc * cycMs / a.passes, a.cycPost * cycMs / a.passes, a.offN ? a.offCyc * cycMs / a.offN : 0.0,
        a.offMaxCyc * cycMs, static_cast<unsigned long long>(a.noEntry), sortBuf, Median(segs), segs.empty() ? 0.0 : *std::max_element(segs.begin(), segs.end()));
    if (a.samples)
      Log("    setup over %llu executions: changed vs the previous one: RTVs %llu (%u bound, %d distinct RTV0%s), DSV "
          "%llu, viewports %llu (%u: %.0fx%.0f at %.0f,%.0f), scissors %llu (%u), renderer+0xd4 %llu (0x%x), +0x2120 "
          "%llu (0x%x), fb/viewport ratios %llu (%dx%d%s), pass flags %llu (%02x %02x %02x %02x), b6-b8 objects %llu "
          "(bytes %u/%u/%u)%s; unstable %llu",
          static_cast<unsigned long long>(a.samples), static_cast<unsigned long long>(a.chRtv), a.last.nrt,
          a.rtv0SeenCount, a.rtv0SeenOver ? "+" : "", static_cast<unsigned long long>(a.chDsv),
          static_cast<unsigned long long>(a.chVp), a.last.nvp, a.last.vp[0].Width, a.last.vp[0].Height,
          a.last.vp[0].TopLeftX, a.last.vp[0].TopLeftY, static_cast<unsigned long long>(a.chSc), a.last.nsc,
          static_cast<unsigned long long>(a.chFlags), a.last.flags, static_cast<unsigned long long>(a.chDbg),
          a.last.dbg, static_cast<unsigned long long>(a.chRatio), a.last.vpd[0], a.last.vpd[1],
          a.last.ratioOk ? "" : ", not read", static_cast<unsigned long long>(a.chPassFlags), a.last.passFlags[0],
          a.last.passFlags[1], a.last.passFlags[2], a.last.passFlags[3], static_cast<unsigned long long>(a.chCtxCb),
          a.last.cbBytes[0], a.last.cbBytes[1], a.last.cbBytes[2], a.rendererMissing ? ", renderer not readable" : "",
          static_cast<unsigned long long>(a.unstable));
  }
  // Segments per candidate set and island.
  Log("  gbuffer rec S0 segments (one Execute each; main executions: median/p90/max per execution; all executions: "
      "per frame):");
  double gateMedian = 0;
  bool segMeasured = false;
  for (int x = 0; x < kSets; ++x)
    for (int i = 0; i < kIslands; ++i) {
      std::vector<double> v;
      for (const ExecSample& e : g_samples)
        if (e.slot < kSlots && mainSlot[e.slot]) v.push_back(e.seg[x][i]);
      const SetStat& a = t.seg[x][i];
      const double med = Median(v);
      if (x == kGateSet && i == kGateIsland) {
        gateMedian = med;
        segMeasured = !v.empty();
      }
      Log("    %-26s island %3u: median %5.1f p90 %5.0f max %5.0f; per frame %6.1f segments, recorded %6.0f draws "
          "(%5.1f%% of model) %.3f ms, residual between segments %.3f ms (%.0f items), outside %.3f ms",
          kSetName[x], kIsland[i], med, Quantile(v, 0.9), v.empty() ? 0.0 : *std::max_element(v.begin(), v.end()),
          a.segments / f, a.recItems / f, pct(static_cast<double>(a.recItems), static_cast<double>(modelItems)),
          a.recCyc * ms, a.resBetweenCyc * ms, a.resBetweenItems / f, a.resOutsideCyc * ms);
    }
  if (g_samplesDropped) Log("    (%llu executions not kept for the medians)", static_cast<unsigned long long>(g_samplesDropped));
  // Census.
  const double cenKnown = static_cast<double>(t.cenClean + t.cenInherit + t.cenOther);
  Log("  gbuffer rec S0 census (model pass-1 opaque draws, per frame): %.0f draws: strictly mappable %.0f (%.1f%%), "
      "inherit a texture variable %.0f (%.1f%%), bind an unmapped resource %.0f (%.1f%%), key not compiled %.0f, key "
      "failed %.0f; %.0f without a DX11Shader; of the analysed draws %.1f%% strictly mappable",
      t.cenDraws / f, t.cenClean / f, pct(static_cast<double>(t.cenClean), static_cast<double>(t.cenDraws)),
      t.cenInherit / f, pct(static_cast<double>(t.cenInherit), static_cast<double>(t.cenDraws)), t.cenOther / f,
      pct(static_cast<double>(t.cenOther), static_cast<double>(t.cenDraws)), t.cenPending / f, t.cenFailed / f,
      t.cenNotG / f, pct(static_cast<double>(t.cenClean), cenKnown));
  if (g_cen) {
    std::map<std::string, double> other, failed;
    uint32_t keys = 0, keysClean = 0, inheritShaders = 0;
    uint32_t catKeys[kNameCats] = {};
    for (size_t i = 0; i < kCensusTable; ++i) {
      const CensusEntry& e = g_cen[i];
      if (!e.shader) continue;
      if (e.state == 1) {
        ++keys;
        keysClean += e.otherCount == 0;
        inheritShaders += e.inherit != 0;
        for (int c = 0; c < kNameCats; ++c) catKeys[c] += (e.cats >> c) & 1;
        if (e.otherCount) other[e.other] += e.otherDraws / f;
      } else if (e.state == 2) {
        failed[e.why] += 1;
      }
    }
    std::string cats;
    for (int c = 0; c < kNameCats; ++c) cats += (c ? ", " : "") + std::string(kNameCatName[c]) + " " + std::to_string(catKeys[c]);
    Log("  gbuffer rec S0 census keys (shader, technique): %u analysed, %u bind only mappable names, %u with draws "
        "inheriting a texture variable (null-handle or empty-entry shaders); keys binding: %s%s",
        keys, keysClean, inheritShaders, cats.c_str(), g_cenFull ? "; census table full" : "");
    std::vector<std::pair<double, std::string>> v;
    for (auto& kv : other) v.push_back({kv.second, kv.first});
    std::sort(v.rbegin(), v.rend());
    std::string line;
    char b[48];
    for (size_t i = 0; i < v.size() && i < 8; ++i) {
      snprintf(b, sizeof(b), " %.1f", v[i].first);
      line += (i ? "; " : "") + v[i].second + b;
    }
    if (!line.empty()) Log("  gbuffer rec S0 census unmapped names (draws/frame): %s", line.c_str());
    line.clear();
    for (auto& kv : failed) line += (line.empty() ? "" : "; ") + kv.first + " x" + std::to_string(static_cast<int>(kv.second));
    if (!line.empty()) Log("  gbuffer rec S0 census failed keys: %.400s", line.c_str());
  }
  Log("  gbuffer rec S0 textures (pass-1 opaque model draws, per frame): untextured draws %.0f (%.1f%%), draws with an "
      "empty texture entry %.0f, sets %.0f, unique textures %.0f and (texture, size) pairs %.0f per execution summed "
      "(the vt[23] replays S2 would add), bad handles %.0f",
      t.untextured / f, pct(static_cast<double>(t.untextured), static_cast<double>(t.cenDraws)), t.nullEntryDraws / f,
      t.texSets / f, t.texUnique / f, t.texSizeUnique / f, t.texBadHandle / f);
  if (g_texOk) {
    std::string line;
    char buf[96];
    for (int r = 0; r < gbbatch::kReasons; ++r) {
      if (!t.texReason[r]) continue;
      snprintf(buf, sizeof(buf), "%s%s %.1f", line.empty() ? "" : ", ",
               r == gbbatch::kOk ? "predicted" : gbbatch::kReasonName[r], t.texReason[r] / f);
      line += buf;
    }
    Log("  gbuffer rec S0 texture views by prediction (mirror coverage; swap pending = getSRV swaps the mip sets; per "
        "frame): %s",
        line.empty() ? "none" : line.c_str());
  } else {
    Log("  gbuffer rec S0 texture views: not predicted (texture classes of this build not verified)");
  }
  Log("  gbuffer rec S0 materials (once per material per execution, per frame): %.0f checked, %.0f without a "
      "render-entry copy, %.0f without reflection (whole CB guarded), %.0f not in the table; changed since "
      "RenderGraph::render entry: guarded dwords %.1f (draws %.1f), whole CB minus overrides %.1f; render-entry copies "
      "%.1f renders/frame, %.0f materials/frame, %.3f ms/frame, %llu faults, %llu without the vector, %llu wipes",
      t.mats / f, t.matsNoSnap / f, t.matsNoRefl / f, t.matsNoEntry / f, t.matsGuardChg / f, t.itemsGuardChg / f,
      t.matsCbChg / f, g_snapRenders / f, g_snapMats / f, g_snapCyc * ms,
      static_cast<unsigned long long>(g_snapFaults), static_cast<unsigned long long>(g_snapNoVector),
      static_cast<unsigned long long>(g_tableWipes));
  Log("  gbuffer rec S0 ExecuteCommandList(TRUE) at the first item, G-buffer targets bound (us): empty list n=%d median "
      "%.2f p90 %.2f; list binding the same RTVs/DSV/viewports n=%d median %.2f p90 %.2f; %llu failed; driver command "
      "lists %s",
      g_execN[0], ExecMedian(0), ExecQ(0, 0.9), g_execN[1], ExecMedian(1), ExecQ(1, 0.9),
      static_cast<unsigned long long>(g_execFail), !g_driverOk ? "unknown" : g_driverCl ? "yes" : "NO (runtime emulated)");
  if (g_sortObserved)
    Log("  gbuffer rec S0 sort hook: %.1f sort calls/frame observed (%s)", g_sortCalls.load() / f,
        g_ownSort ? "call sites patched for this phase" : "shadow recorder's patch");
  // Gates.
  GateIn in;
  in.modelMs = modelCyc * ms;
  in.segMeasured = segMeasured;
  in.medianSeg = gateMedian;
  in.resBetweenMs = t.seg[kGateSet][kGateIsland].resBetweenCyc * ms;
  in.censusMeasured = g_census && t.cenDraws > 0;
  in.censusPct = pct(static_cast<double>(t.cenClean), static_cast<double>(t.cenDraws + t.cenNotG));
  uint64_t compared = 0;
  for (int s = 0; s < kSlots; ++s) compared += g_stat[s].samples ? g_stat[s].samples - 1 : 0;
  in.stableSamples = compared;
  in.stablePct = compared ? 100.0 - pct(static_cast<double>(t.unstable), static_cast<double>(compared)) : 0.0;
  in.guardMeasured = t.mats > t.matsNoSnap;
  in.guardChanges = t.matsGuardChg;
  in.execMeasured = g_execN[1] > 0;
  in.execUs = ExecMedian(1);
  const GateOut o = Evaluate(in);
  Log("  gbuffer rec S0 gate 1 (ModelMaterialMT share of the G-buffer passes >= 5 ms/frame): %.3f ms/frame of net loop "
      "%.3f ms (%.1f%%), %.0f draws/frame -> %s",
      in.modelMs, loopNet * ms, pct(static_cast<double>(modelCyc), loopNet), modelItems / f, GateWord(o.g[0]));
  Log("  gbuffer rec S0 gate 2 (median segments per main execution <= 4, BM_NONE+A2C, island 30; residual between "
      "segments < 1 ms/frame): median %.1f, residual between %.3f ms/frame -> %s",
      in.medianSeg, in.resBetweenMs, GateWord(o.g[1]));
  Log("  gbuffer rec S0 gate 3 (census: strictly mappable >= 90%% of the model pass-1 opaque draws): %.1f%% (%.0f of "
      "%.0f draws/frame; inherited %.1f%%; %.1f%% of all %.0f model draws in the passes) -> %s",
      in.censusPct, t.cenClean / f, (t.cenDraws + t.cenNotG) / f,
      pct(static_cast<double>(t.cenInherit), static_cast<double>(t.cenDraws + t.cenNotG)),
      pct(static_cast<double>(t.cenClean), static_cast<double>(modelItems)), modelItems / f, GateWord(o.g[2]));
  Log("  gbuffer rec S0 gate 4 (RTs/DSV/viewport/scissor/flags/ratios/pass flags stable >= 99%% of executions): %.2f%% "
      "of %llu compared -> %s",
      in.stablePct, static_cast<unsigned long long>(compared), GateWord(o.g[3]));
  Log("  gbuffer rec S0 gate 5 (guarded material dwords unchanged between render entry and the pass): %llu changes in "
      "%.0f material checks/frame -> %s",
      static_cast<unsigned long long>(t.matsGuardChg), t.mats / f, GateWord(o.g[4]));
  Log("  gbuffer rec S0 gate 6 (ExecuteCommandList(TRUE) with the G-buffer targets bound <= 15 us): median %.2f us "
      "(n=%d) -> %s",
      in.execUs, g_execN[1], GateWord(o.g[5]));
  Log("  gbuffer rec S0: %s%s (R18 8: S2-S6 need gates 1-6; no-go triggers: segments > 8, model share < 3.5 ms)",
      o.verdict > 0 ? "GO" : o.verdict == 0 ? "NO-GO" : "INCOMPLETE (a gate was not measured, none failed)",
      o.trigger ? ", a no-go trigger hit" : "");
}

// ---------------------------------------------------------------------------
// Suite phase
// ---------------------------------------------------------------------------
void Measure(int ms, std::atomic<uint64_t>& frames, double tscHz, const std::atomic<bool>& abort) {
  if (g_chained.load()) {
    Log("  gbuffer rec counter: already running");
    return;
  }
  g_tscHz = tscHz;
  auto* ng = reinterpret_cast<uint8_t*>(GetModuleHandleW(L"NGModel.dll"));
  auto* dx = reinterpret_cast<uint8_t*>(GetModuleHandleW(L"dx11backend.dll"));
  g_md = reinterpret_cast<uint8_t*>(GetModuleHandleW(L"ModelDesc.dll"));
  if (!ng || !dx || !tscHz) {
    Log("  gbuffer rec counter: NGModel/dx11backend not loaded or no TSC rate; skipped");
    return;
  }
  if (!gbcount::Install()) {
    Log("  gbuffer rec counter: SceneRenderable hook unavailable (this build); skipped");
    return;
  }
  g_ownPassHook = gbpass::g_state == 0;
  if (!gbpass::Install()) {
    Log("  gbuffer rec counter: G-buffer pass execute not hooked (GraphicsCore build); skipped");
    return;
  }
  g_srVt = ng + gbcount::kVtableRva;
  g_modelMatVt = gbcount::g_modelMatVtbl;
  g_texOk = shadowtex::VerifyBuild(ng, dx) == nullptr;
  g_shaderVt = dx + shadowtex::g_at.shader;
  if (!allocslab::RttiIs(dx, static_cast<void**>(g_shaderVt), ".?AVDX11Shader@RenderAPI@@")) {
    Log("  gbuffer rec counter: DX11Shader vtable not found: blend modes unknown, no census");
    g_shaderVt = nullptr;
  }
  if (g_texOk) {
    g_env.texVtbl = dx + shadowtex::g_at.tex;
    g_env.inner[0] = dx + shadowtex::g_at.innerFile;
    g_env.inner[1] = dx + shadowtex::g_at.innerArray;
    g_env.inner[2] = dx + shadowtex::g_at.innerDummy;
    g_env.getDesc = reinterpret_cast<gbbatch::GetDescFn>(dx + shadowtex::g_at.getDesc);
    g_env.compat = reinterpret_cast<gbbatch::CompatFn>(dx + shadowtex::g_at.compat);
  } else {
    Log("  gbuffer rec counter: texture classes of this build not verified: no view prediction");
  }
  g_ratioOk = false;
  if (!shadowbatch::InstallRenderer()) {
    Log("  gbuffer rec counter: DX11Renderer not matched: +0xd4/+0x2120 and ratios not read");
  } else {
    auto** rv = shadowbatch::g_rendererVtbl;
    g_ratioOk = reinterpret_cast<uint8_t*>(SlotOriginal(&rv[54])) == dx + 0x15d60 &&
                reinterpret_cast<uint8_t*>(SlotOriginal(&rv[40])) == dx + 0x17010 &&
                shadowtex::CodeIs(dx, gbbatch::kDxCode[2].begin, gbbatch::kDxCode[2].end, gbbatch::kDxCode[2].hash) &&
                shadowtex::CodeIs(dx, gbbatch::kDxCode[3].begin, gbbatch::kDxCode[3].end, gbbatch::kDxCode[3].hash);
    if (!g_ratioOk) Log("  gbuffer rec counter: DX11Renderer frame-buffer/viewport getters not as analysed: ratios not read");
  }
  // Device: the immediate context for the setup probes, our deferred one for the Execute timing.
  ID3D11Device* dev = shadowinst::g_device;
  if (dev) {
    dev->AddRef();
  } else if (sfilt::Ctx* c = sfilt::g_ctx.load()) {
    c->GetDevice(&dev);
  }
  g_driverOk = false;
  if (dev) {
    dev->GetImmediateContext(&g_ctx);
    D3D11_FEATURE_DATA_THREADING th{};
    if (SUCCEEDED(dev->CheckFeatureSupport(D3D11_FEATURE_THREADING, &th, sizeof(th)))) {
      g_driverOk = true;
      g_driverCl = th.DriverCommandLists;
    }
    if (dev->GetCreationFlags() & D3D11_CREATE_DEVICE_SINGLETHREADED) {
      Log("  gbuffer rec counter: device is SINGLETHREADED: no deferred context, Execute cost not measured");
    } else if (FAILED(dev->CreateDeferredContext(0, &g_dc))) {
      g_dc = nullptr;
      Log("  gbuffer rec counter: CreateDeferredContext failed: Execute cost not measured");
    }
    dev->Release();
  } else {
    Log("  gbuffer rec counter: DCS's device not known yet (it comes from [Model] ShadowInstancing=1 or [D3D] "
        "SplitFilter=1): no setup probes, no Execute timing");
  }
  // Census: shadow_inst's G-buffer keys (collected and compiled for the phase).
  g_census = g_shaderVt && shadowinst::InstallGb();
  if (!g_census) Log("  gbuffer rec counter: shadow_inst's G-buffer keys unavailable (needs [Model] ShadowInstancing=1): no census");
  if (g_census) shadowinst::g_collectGb = true;
  // Tables.
  if (!g_mats) {
    g_mats = static_cast<MatEntry*>(VirtualAlloc(nullptr, kMatTable * sizeof(MatEntry), MEM_COMMIT | MEM_RESERVE,
                                                 PAGE_READWRITE));
    if (!g_mats) Log("  gbuffer rec counter: no memory for the material table: material checks off");
  } else {
    memset(g_mats, 0, kMatTable * sizeof(MatEntry));
  }
  g_matsUsed = 0;
  delete[] g_cen;
  g_cen = new (std::nothrow) CensusEntry[kCensusTable]();
  g_cenUsed = 0;
  memset(g_blend, 0, sizeof(g_blend));
  g_samples.clear();
  g_samples.shrink_to_fit();
  g_samples.reserve(kMaxSamples);
  // Render thread: the one shadow batching saw, else the one running top-level passes, else the first G-buffer pass.
  DWORD rt = shadowbatch::g_renderThread;
  if (!rt) rt = ptiming::g_topTid.load();
  g_renderTid = rt;
  // RenderGraph::render entry: batching's import hook, ours for the phase if absent.
  g_ownRenderHook = false;
  if (!shadowbatch::g_renderSlot && !shadowbatch::g_renderSlot2) g_ownRenderHook = shadowbatch::InstallRenderHook();
  if (!shadowbatch::g_renderSlot && !shadowbatch::g_renderSlot2)
    Log("  gbuffer rec counter: RenderGraph::render entry not hooked: no material copies, no entry offsets");
  // Scene's sort: observe the recorder's patch, or patch for the phase while the recorder is off.
  g_ownSort = false;
  g_sortObserved = false;
  shrec::g_sortBorrowed = true;
  if (shrec::g_sortPatched) {
    g_sortObserved = true;
  } else if (!g_cfg.shadowRecorder && shrec::g_state.load() != 1) {
    const bool ok = shrec::g_sortStub ? shrec::PatchSortSites(true) : shrec::InstallSortHook();
    g_ownSort = ok && shrec::g_sortPatched;
    g_sortObserved = g_ownSort;
  }
  if (!g_sortObserved) {
    shrec::g_sortBorrowed = false;
    Log("  gbuffer rec counter: Scene sort hook not available: when the vectors become final is not measured");
  }
  g_learnedCount = 0;
  memset(g_learned, 0, sizeof(g_learned));
  ResetStats();
  g_prevWrap = gbpass::g_wrap.load();
  g_prevOverride = gbcount::g_override.load();
  g_prevObserver = shadowbatch::g_renderObserver.load();
  shadowbatch::g_renderObserver = &OnRender;
  gbpass::g_wrap = &Wrap;
  gbcount::g_override = &Override;
  if (g_sortObserved) shrec::g_sortObserver = &OnSorted;
  g_chained = true;
  // Discovery: learn the executions and the classes; the keys are collected.
  g_on = true;
  Sleep(1000);
  g_on = false;
  Sleep(100);
  for (int i = 0; i < 400 && g_inside.load() != 0; ++i) Sleep(5);
  if (g_census) {
    const double waitS = shadowinst::WaitCompiles(abort);
    const shadowinst::Totals k = shadowinst::Snap(shadowinst::kKindGb);
    Log("  gbuffer rec counter: waited %.1f s for the G-buffer keys (%u keys: %u OK, %u failed, %u not finished)", waitS,
        k.keys, k.ok, k.done - k.ok, k.keys - k.done);
  }
  if (abort.load()) {
    Unchain();
    Drain();
    if (g_census) shadowinst::g_collectGb = shadowinst::g_onGb.load();
    if (g_ownPassHook && !g_cfg.gbufferBatch) gbpass::Uninstall();
    g_ownPassHook = false;
    return;
  }
  HookClasses();
  ResetStats();
  const uint64_t f0 = frames.load();
  g_on = true;
  Sleep(ms);
  g_on = false;
  const double f = static_cast<double>(frames.load() - f0);
  Sleep(100);
  Unchain();
  const bool drained = Drain();
  if (g_census) shadowinst::g_collectGb = shadowinst::g_onGb.load();
  if (g_ownPassHook && !g_cfg.gbufferBatch) gbpass::Uninstall();
  g_ownPassHook = false;
  int foreign = 0;
  for (int i = 0; i < g_nsCount.load(); ++i) foreign += g_ns[i].state == -1;
  if (f <= 0 || !drained) {
    Log("  gbuffer rec counter: no frames counted%s", drained ? "" : " (a pass was still running)");
    return;
  }
  Report(f);
  if (foreign) Log("  gbuffer rec S0: %d renderable classes untimed (vt[1] hooked by another module)", foreign);
  for (size_t i = 0; g_cen && i < kCensusTable; ++i) {
    CensusEntry& e = g_cen[i];
    e.other.clear();
    e.why.clear();
    e.key.clear();
  }
}

}  // namespace gbreccount
