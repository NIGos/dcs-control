// Forward recorder stage S0 counters (R21): can the shipped in-order recorder
// (gb_rec.h) take the draws inside DCS's SimplePassData passes? Measurement
// only: nothing is drawn differently and no DCS memory is written. [Suite]
// ForwardRecCount (default 0): 1 s discovery, the model keys' compiles waited
// for, then a 5 s count.
//
// Tags: [V] verified in the binary (DCS 2.9.30) or the shader sources, [I]
// inferred. Facts and section numbers: docs/research/R21_forward_recorder.md.
//
// The pass [V GraphicsCore]: every addSimpleRenderingPass pass is one class,
// TBaseRenderingPassWithData<SimplePassData, ...> (vtable 0xd9fc0); slot 19 =
// thunk 0xa9ed0 -> 0xa95c0(data = pass+0x70, res = pass+0x48, ctx). Its item
// vector is [[ctx]+0x438] + word[[res+0x20]+0x38] * 24 (0xa961c), read once
// before the loop 0xa9997: (*p)->vt[1](p, data). Before the loop: frame buffer
// (color res+0, depth res+0x10), clears, binder [data+0x588]->vt[1](data,
// data+0x590), toggles data+0x5ac (wireframe) / +0x5ad (flip winding). The
// pass name (what SceneRenderer passes to addSimpleRenderingPass, a view
// prefix plus "OpaquePass", "TransparentPass", ...) is copied inline into the
// pass object: length at pass+8 (at most 32), chars at pass+0x10 (0xa9310).
// The screen-space SimplePassData passes (addScreenSpaceRenderingPass) are
// another class and draw no items: not counted.
//
// Per call (render thread):
//  1. Identity: name -> kind by its longest known suffix; slot = (render
//     graph, collection index, name); ordinal among the SimplePassData calls
//     since RenderGraph::render entry; offset from the entry; when the
//     collection's vector became final (Scene sort hook, as gb_rec_count.h).
//  2. Item mix: NGModel SceneRenderable + ModelMaterialMT by model pass number
//     [r+0x60] (1, 2, 3, 8, other), cockpit (r+0x64), transparent
//     (props+0x33), BLEND_MODE; the FX pass index NGModel 0x15e20 picks
//     (FxPassIndex, below); other materials by RTTI; other renderables by
//     vtable. Render-thread time per class through the SceneRenderable
//     override chain and vt[1] thunks (gb_rec_count.h's method); our probes
//     are timed and subtracted.
//  3. Recordable by gb_rec's rules, generalised to the forward techniques
//     (first failing rule per ModelMaterialMT draw): DX11Shader; technique
//     normal* / normal_cockpit* (the ones shadow_inst compiles; pass 8
//     flat_shadow and the 4..13 routes are other techniques); animated
//     properties of the five replicated classes; mesh (shadow_rec.h ReadMesh:
//     indexed triangle list); every texture view predictable (gb_batch.h
//     PredictView: class known, no swap due); the census of the (shader,
//     technique, FX pass) read set (shadow_inst's (a) compile): pending /
//     failed / a read texture record the draw leaves unset (FX variable
//     inheritance) / names outside def_uniforms, b6-b8, sbPositions,
//     samplers, material textures: either fixed-register context globals
//     (t87-t127, b4/b5: shadow maps, light lists, environment, atmosphere;
//     kRegGlobals) or anything else (unmapped). Sets: strict (every rule),
//     +globals (the only failing rule is fixed-register globals: a recorder
//     that binds them per call from its front entry), any model draw with a
//     DX11Shader. Islands 1/30/100 (gb_rec_count.h's
//     segment rule). Blended draws (BLEND_MODE transparent, additive, decal,
//     shadowed transparent; flat_shadow) are counted: they need DCS's exact
//     order, which the in-order recorder keeps.
//  4. Pass setup at the first item: RTVs, DSV, viewports, scissors,
//     DX11Renderer +0xd4 / +0x2120, the frame-buffer / viewport ratio inputs,
//     the toggle bytes; compared with the slot's previous call.
//  5. Material bytes between render entry and the pass: def_uniforms of every
//     ModelMaterialMT in the learned vectors copied at render entry; at the
//     pass, the dwords the key's stages read (CbUsedDwords of the FX pass)
//     minus every route's per-draw writes (R21 3.2) are compared.
//
// Coexistence: as gb_rec_count.h (wrapper chain, class slots, render and sort
// observers chained in front and taken out with compare-exchange). Slot 19
// of the SimplePassData vtable has no other owner; it is hooked for the phase
// and put back after it. With the key at 0 none of this runs.
// Included once from main.cpp inside its anonymous namespace, after
// gb_rec_count.h.
#pragma once

namespace fwdreccount {

using gbreccount::Median;
using gbreccount::Quantile;
using gbreccount::MarkCb;
using gbreccount::DwordsDiffer;
using gbreccount::PtrHash;
using gbreccount::VtName;
using gbreccount::NameCatOf;
using gbreccount::kNcOther;
constexpr uint32_t kCbOff = gbreccount::kCbOff, kCbLen = gbreccount::kCbLen, kCbDwords = gbreccount::kCbDwords;

// ---------------------------------------------------------------------------
// Constants and pure helpers (offline tested)
// ---------------------------------------------------------------------------
constexpr uint32_t kVtableRva = 0xd9fc0;
constexpr int kExecSlot = 19;
constexpr uint32_t kThunkRva = 0xa9ed0;
constexpr const char* kRtti = ".?AV?$TBaseRenderingPassWithData@USimplePassData@?1??addSimpleR";  // first 63 chars
struct Code {
  uint32_t begin, end;
  uint64_t hash;
};
constexpr Code kCode[] = {
    {0xa9ed0, 0xa9ee0, 0x898f193e482b56efull},  // slot 19 thunk: data = pass+0x70, res = pass+0x48
    {0xa961c, 0xa9647, 0x71b9088f24edff3full},  // item vector
    {0xa9997, 0xa99b8, 0x2ed39f5bb7da3930ull},  // item loop
};

constexpr int kSlots = 48;   // learned calls (graph, collection, name); index kSlots = not learnable
constexpr int kMaxNs = 16;
constexpr int kMatCls = 8;
constexpr int kBuckets = 5;  // model pass 1, 2, 3, 8, other
constexpr int kModelKinds = kBuckets * 32;
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
constexpr size_t kMatTable = 1 << 14;
constexpr size_t kBlendTable = 4096;
constexpr size_t kCensusTable = 4096;
constexpr uint32_t kMaxRecords = 512;
constexpr int kPassIdx = 10;  // FX pass index P0..P8, then "other"
constexpr int kSets = 3;
constexpr int kIslands = 3;
constexpr uint32_t kIsland[kIslands] = {1, 30, 100};
constexpr int kGateSet = 1, kGateIsland = 1;  // +globals, island 30
constexpr size_t kMaxSamples = 32768;
constexpr double kExecMs = 0.016;  // ExecuteCommandList(TRUE) with targets bound, median 15.9 us [M R18 S0]
constexpr UINT kGlobFirst = 87, kGlobCount = 41;  // PS slots t87..t127 of the fixed-register globals
constexpr uint32_t kGlobEvery = 16;               // re-read them after every 16th item call of a pass

// Pass kinds by name suffix (SceneRenderer's strings [V]; GraphicsCore "LightMapPass").
enum Kind : int {
  kOpaque = 0,
  kTransparentDecal,
  kTransparent,
  kTransparentCockpitDecal,
  kTransparentCockpit,
  kSeparateTransparent,
  kFlatShadow,
  kHotAir,
  kMirrorOpaque,
  kMirrorTransparent,
  kMfdMap,
  kDemoSceneMap,
  kDemoSceneMapTd,
  kLightMap,
  kKindOther,
  kKinds
};
const char* const kKindSuffix[kKindOther] = {
    "OpaquePass",       "TransparentDecalPass",  "TransparentPass", "TransparentCockpitDecalPass",
    "TransparentCockpitPass", "SeparateTransparentPass", "SimpleFlatShadowPass", "HotAirPass",
    "MirrorOpaquePass", "MirrorTransparentPass", "MFDMapPass",      "DemoSceneMapPass",
    "DemoSceneMapTDPass", "LightMapPass"};
const char* const kKindName[kKinds] = {
    "OpaquePass",       "TransparentDecalPass",  "TransparentPass", "TransparentCockpitDecalPass",
    "TransparentCockpitPass", "SeparateTransparentPass", "SimpleFlatShadowPass", "HotAirPass",
    "MirrorOpaquePass", "MirrorTransparentPass", "MFDMapPass",      "DemoSceneMapPass",
    "DemoSceneMapTDPass", "LightMapPass", "other name"};

// The kind whose suffix is the longest one name ends with; kKindOther if none.
inline int KindOfName(const char* name) {
  if (!name) return kKindOther;
  const size_t n = strlen(name);
  int best = kKindOther;
  size_t bestLen = 0;
  for (int k = 0; k < kKindOther; ++k) {
    const size_t l = strlen(kKindSuffix[k]);
    if (l <= n && l > bestLen && strcmp(name + n - l, kKindSuffix[k]) == 0) {
      best = k;
      bestLen = l;
    }
  }
  return best;
}

// The FX pass index NGModel 0x15e20 hands to the submit [V 0x16015-0x16104,
// jump table 0x16110] for model pass `pass` (not 4..13, which 0x16140 routes
// elsewhere): ck = byte r+0x64 (cockpit technique [mat+0x1e0]), b65 = byte
// r+0x65, b68 = byte r+0x68 bit 0, opaque = props+0x33 == 0, selected =
// props+0x12d != 0.
inline int FxPassIndex(uint32_t pass, bool ck, bool b65, bool b68, bool opaque, bool selected) {
  int e = -1;
  if (!(pass == 1 && opaque)) {
    const int edx = ck && b68 ? 2 : 0;
    e = b65 ? (edx | 1) : edx;
    if (selected) e = !ck ? -1 : (pass == 3 ? 1 : 0);
  }
  if (!ck) return e == 0 ? 1 : e == 1 ? 2 : 0;
  const int i = e + 1;  // jump table: -1 -> P0, 0 -> P1, 1 -> P2, 2..7 -> P3..P8
  return i < 0 || i > 8 ? 0 : i == 0 ? 0 : i;
}

inline int Bucket(uint32_t pass) { return pass == 1 ? 0 : pass == 2 ? 1 : pass == 3 ? 2 : pass == 8 ? 3 : 4; }
inline int KindCode(uint32_t pass, bool cockpit, bool transparent, int blend) {
  return Bucket(pass) << 5 | (cockpit ? 16 : 0) | (transparent ? 8 : 0) | (blend & 7);
}
inline std::string ModelKindName(int k) {
  static const char* const kPass[kBuckets] = {"pass 1", "pass 2", "pass 3", "pass 8 (flat_shadow)", "pass other"};
  std::string s = std::string("SceneRenderable ModelMaterialMT ") + kPass[(k >> 5) % kBuckets];
  if (k & 16) s += " cockpit";
  if (k & 8) s += " transparent";
  s += " ";
  s += gbreccount::kBlendName[k & 7];
  return s;
}

// Draws whose blend depends on what is already in the target (order matters):
// BLEND_MODE transparent, additive, decal, shadowed transparent [V R19 1.2],
// and flat_shadow (alpha blended, def_material.fx [V R18 2]).
inline bool Blended(int blend, uint32_t pass) {
  using namespace gbreccount;
  return pass == 8 || blend == kBmTransparent || blend == kBmAdditive || blend == kBmDecal ||
         blend == kBmDecalDeferred || blend == kBmShadowedTransparent || blend == kBmUnknown;
}

// First failing rule of a ModelMaterialMT draw (gb_rec's rules generalised).
enum Why : int {
  kWRecordable = 0,
  kWNoShader,
  kWTechnique,
  kWAnim,
  kWMesh,
  kWTexture,
  kWCensusPending,
  kWCensusFailed,
  kWInherit,
  kWUnmapped,
  kWGlobals,
  kWhys
};
const char* const kWhyName[kWhys] = {"recordable by gb_rec's rules",
                                     "no DX11Shader",
                                     "technique not normal*/normal_cockpit* (flat_shadow, other routes)",
                                     "an animated property not replicable",
                                     "mesh not recordable (not an indexed triangle list)",
                                     "a texture view not predictable (class, swap due, null)",
                                     "key not compiled by shadow inst yet",
                                     "key's read set not analysed",
                                     "a read texture record left unset (FX variable inheritance)",
                                     "binds an unmapped name that is not a fixed-register global",
                                     "binds fixed-register globals only (t87-t127, b4/b5: late-bound per call)"};

// Context resources the shaders declare at a fixed register, bound by DCS's
// renderer slot binders like b6-b8, not by the material's FX Apply [V
// Bazar/shaders common, deferred, enlight, indirectLighting, model/functions
// glass.hlsl]. A recorder binds them per call from what is bound at its front
// entry (as the sampler pool, R19 4.2) and checks them at every exec entry.
struct RegGlobal {
  const char* name;
  const char* reg;
};
constexpr RegGlobal kRegGlobals[] = {
    {"avgLuminance", "t87"},        {"iceHaloTexture", "t88"},       {"cloudsDensityMap", "t89"},
    {"cloudsLightMapSPTex", "t90"}, {"cloudsShadowTex3D", "t91"},    {"transmittanceTex2", "t92"},
    {"miePhaseFuncTex", "t93"},     {"LightsIdxOffsets", "t94"},     {"LightsIdx", "t95"},
    {"spots", "t96"},               {"omnis", "t97"},                {"SSLRMap", "t98"},
    {"environmentCockpitMap", "t100"}, {"CockpitRefraction", "t103"}, {"RainDroplets", "t104"},
    {"terrainESM", "t105"},         {"terrainShadowMap", "t106"},    {"skyTex2", "t107"},
    {"cockpitEnvironmentMap", "t108"}, {"resolvedIndirectLightKnots", "t112"}, {"g_NormalTexture", "t113"},
    {"g_DepthTexture", "t114"},     {"g_ReflectionTexture", "t115"}, {"g_RefractionTexture", "t116"},
    {"preintegratedGF", "t117"},    {"depthMSAA", "t118"},           {"sbAtmosphereSamples", "t119"},
    {"cloudsShadowTex", "t120"},    {"secondaryShadowMap", "t121"},  {"cascadeShadowMap", "t122"},
    {"environmentMap", "t123"},     {"skyTex", "t124"},              {"transmittanceTex", "t125"},
    {"irradianceTex", "t126"},      {"inscatterTex", "t127"},        {"cbFLIRParams", "b4"},
    {"cbWaterParams", "b5"}};
inline const char* RegisterOf(const char* name) {
  if (!name) return nullptr;
  for (const RegGlobal& g : kRegGlobals)
    if (strcmp(g.name, name) == 0) return g.reg;
  return nullptr;
}

// Item flags: bit 0 model, 1 strict, 2 strict or only globals, 3 any model
// draw with a DX11Shader, 4 blended.
enum : uint8_t { kFModel = 1, kFStrict = 2, kFGlobals = 4, kFAny = 8, kFBlend = 16 };
const uint8_t kSetNeed[kSets] = {kFStrict, kFGlobals, kFAny};
const char* const kSetName[kSets] = {"strict (gb_rec rules)", "+late-bound globals", "any model draw"};

inline uint8_t FlagsFor(int why, bool shader, bool blended) {
  uint8_t f = kFModel;
  if (shader) f |= kFAny;
  if (why == kWRecordable) f |= kFStrict | kFGlobals;
  if (why == kWGlobals) f |= kFGlobals;
  if (blended) f |= kFBlend;
  return f;
}

// The census's comma-separated unmapped names, split.
inline std::vector<std::string> SplitNames(const std::string& s) {
  std::vector<std::string> out;
  size_t a = 0;
  while (a < s.size()) {
    size_t b = s.find(',', a);
    if (b == std::string::npos) b = s.size();
    if (b > a) out.push_back(s.substr(a, b - a));
    a = b + 1;
  }
  return out;
}

struct SegResult {
  uint32_t segments = 0, recItems = 0, resBetweenItems = 0, blendItems = 0;
  uint64_t recCyc = 0, resBetweenCyc = 0, resOutsideCyc = 0;
};

// Runs of >= island consecutive items carrying `need` become segments (one
// ExecuteCommandList each); everything else stays DCS's, in place. cyc may be
// nullptr.
inline SegResult Segments(const uint8_t* flags, const uint32_t* cyc, size_t n, uint8_t need, uint32_t island) {
  SegResult r;
  uint64_t pend = 0, total = 0;
  uint32_t pendItems = 0;
  bool any = false;
  size_t i = 0;
  while (i < n) {
    if ((flags[i] & need) != need) {
      const uint64_t c = cyc ? cyc[i] : 0;
      pend += c;
      total += c;
      ++pendItems;
      ++i;
      continue;
    }
    size_t j = i;
    uint64_t s = 0;
    uint32_t bl = 0;
    while (j < n && (flags[j] & need) == need) {
      if (cyc) s += cyc[j];
      bl += (flags[j] & kFBlend) ? 1 : 0;
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
      r.blendItems += bl;
    } else {
      pend += s;
      pendItems += len;
    }
    i = j;
  }
  r.resOutsideCyc = total - r.recCyc - r.resBetweenCyc;
  return r;
}

// def_uniforms dwords any model route writes per draw [V NGModel 0x16140,
// 0x15e20]: posStructOffset +0xfc; 0x15e20: +0x20..+0x5f, +0x80..+0x8b and,
// off the G-buffer P0 path, +0x100 (mat+0x190 = item+0xd8); flat_shadow:
// +0x10..+0x1f (mat+0xa0) and +0xd0..+0xd7 (mat+0x160); normal_ir: +0x60..+0x6f
// (mat+0xf0).
inline void RouteOverrides(uint64_t m[2]) {
  gbreccount::BaseOverrides(m);
  MarkCb(m, 0x100, 4);
  MarkCb(m, 0x10, 0x10);
  MarkCb(m, 0xd0, 8);
  MarkCb(m, 0x60, 0x10);
}

// Gates (R21 8): 1 pass, 0 fail, -1 not measured.
struct GateIn {
  double modelMs = 0;     // ModelMaterialMT time in SimplePassData, net of probes
  double recMs = 0;       // +globals, island 30: time of the draws a recorder would take
  bool segMeasured = false;
  double medianSeg = 0;   // per main call, +globals, island 30
  double segPerFrame = 0;
  bool censusMeasured = false;
  double strictPct = 0;   // strict share of the +globals draws
  uint64_t stableSamples = 0;
  double stablePct = 0;
  bool guardMeasured = false;
  uint64_t guardChanges = 0;
  uint64_t globCalls = 0;  // calls whose t87-t127 were re-read after an item
  double globStablePct = 0;
};
struct GateOut {
  int g[7] = {-1, -1, -1, -1, -1, -1, -1};
  // 2 GO (gb_rec as is), 1 GO with late-bound globals (gate 4 failed, gate 7 passed), 0 NO-GO, -1 incomplete
  int verdict = 0;
};
inline GateOut Evaluate(const GateIn& in) {
  GateOut o;
  o.g[0] = in.modelMs >= 1.5 ? 1 : 0;
  o.g[1] = in.recMs >= 1.0 ? 1 : 0;
  o.g[2] = !in.segMeasured ? -1 : (in.medianSeg <= 4.0 && in.segPerFrame * kExecMs <= 0.2 * in.recMs) ? 1 : 0;
  o.g[3] = !in.censusMeasured ? -1 : in.strictPct >= 90.0 ? 1 : 0;
  o.g[4] = !in.stableSamples ? -1 : in.stablePct >= 99.0 ? 1 : 0;
  o.g[5] = !in.guardMeasured ? -1 : in.guardChanges == 0 ? 1 : 0;
  o.g[6] = !in.globCalls ? -1 : in.globStablePct >= 99.0 ? 1 : 0;
  bool fail = false, missing = false;
  for (int i : {0, 1, 2, 4, 5}) {
    fail |= o.g[i] == 0;
    missing |= o.g[i] < 0;
  }
  missing |= o.g[3] < 0;
  if (o.g[3] == 0) {  // globals needed: they must hold still within a call
    fail |= o.g[6] == 0;
    missing |= o.g[6] < 0;
  }
  o.verdict = fail ? 0 : missing ? -1 : o.g[3] > 0 ? 2 : 1;
  return o;
}
inline const char* GateWord(int g) { return g > 0 ? "PASS" : g == 0 ? "FAIL" : "NOT MEASURED"; }
inline const char* VerdictText(int v) {
  return v == 2   ? "GO (gb_rec's recorder as is, generalised to the forward passes)"
         : v == 1 ? "GO WITH LATE-BOUND GLOBALS (gate 4 failed, gates 1-3 and 5-7 passed: the recorder binds the "
                    "fixed-register globals per call from its front entry, R21 S2)"
         : v == 0 ? "NO-GO"
                  : "INCOMPLETE (a gate was not measured, none failed)";
}

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------
using ExecFn = void(__fastcall*)(void* pass, void* ctx);
ExecFn g_orig = nullptr;
void** g_slot19 = nullptr;
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
bool g_texOk = false, g_ratioOk = false, g_census = false;
gbbatch::TexEnv g_env;
gbcount::OverrideFn g_prevOverride = nullptr;
shadowbatch::RenderObserverFn g_prevObserver = nullptr;
bool g_ownRenderHook = false, g_ownSort = false, g_sortObserved = false;

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
  uint64_t segments, recItems, recCyc, resBetweenCyc, resOutsideCyc, resBetweenItems, blendItems;
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
  uint8_t toggles[2];
  bool togglesOk;
};
struct SlotStat {
  // Sums (every field up to sortMinLead is a uint64_t: Totals adds them).
  uint64_t passes, emptyPasses, origCalls, faults, oversize;
  uint64_t cycWrap, cycOrig, cycPre, cycLoop, cycPost, cycProbe, cycAnalyse;
  uint64_t items, modelItems, modelCyc;
  ClassStat cls[kClasses];
  uint64_t passIdx[kPassIdx];
  uint64_t why[kWhys], whyCyc[kWhys];
  uint64_t blendItems, blendCyc;
  uint64_t texSets, texReason[gbbatch::kReasons];
  uint64_t mats, matsNoSnap, matsNoRefl, matsGuardChg, matsCbChg, matsNoEntry, itemsGuardChg;
  uint64_t samples, unstable, chRtv, chDsv, chVp, chSc, chFlags, chDbg, chRatio, chToggles, rendererMissing;
  uint64_t globCalls, globChanged, globReads, globSlotsChanged, globBound;  // t87-t127 within calls
  uint64_t offN, offCyc, offMaxCyc, noEntry, sortN, sortLeadCyc, sortMissing, sortLate, ordinalSum;
  int64_t sortMinLead;
  SetStat seg[kSets][kIslands];
  bool have;
  Setup last;
};
SlotStat g_stat[kSlots + 1];
struct Learned {
  void* rg;
  uint32_t idx;
  uint64_t nameHash;
  char name[40];
  int kind;
};
Learned g_learned[kSlots];
std::atomic<int> g_learnedCount{0};
struct ExecSample {
  uint8_t slot;
  uint16_t seg[kSets][kIslands];
};
std::vector<ExecSample> g_samples;  // reserved before the phase; never grown by the render thread
uint64_t g_samplesDropped = 0;
uint64_t g_unplanned = 0, g_analyseFaults = 0, g_otherClass = 0;
uint64_t g_snapRenders = 0, g_snapCyc = 0, g_snapMats = 0, g_snapFaults = 0, g_tableWipes = 0;
std::atomic<uint64_t> g_sortCalls{0};
std::atomic<uint64_t> g_sortTick[kSlots];
uint64_t g_entryTick = 0;
uint32_t g_ordinal = 0;

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

// ---- BLEND_MODE per DX11Shader (render thread) ----
struct BlendEntry {
  void* shader;
  void* effect;
  int code;
};
BlendEntry g_blend[kBlendTable];

// ---- Census per (DX11Shader, technique, FX pass) (render thread) ----
struct CensusEntry {
  void* shader;
  uint64_t tech;
  uint32_t pass;
  void* effect;
  void* techBegin;
  int state;  // 0 pending, 1 ready, 2 failed
  uint32_t tryGen;
  uint64_t recMask[kMaxRecords / 64];
  uint32_t otherCount, globalCount;
  uint64_t draws, cyc;  // walk draws and their call cycles in the counted phase
  std::string other, globals, why;
};
CensusEntry* g_cen = nullptr;
size_t g_cenUsed = 0;
uint64_t g_cenFull = 0;
constexpr uint16_t kNoCen = 0xffff;

// ---- Per-pass state (render thread) ----
uint8_t g_cls[kMaxItems];
uint8_t g_flags[kMaxItems];
uint8_t g_why[kMaxItems];  // kWhys, 0xff = not a model draw
uint16_t g_cenIdx[kMaxItems];  // census entry of a model draw, kNoCen = none
static_assert(kCensusTable < kNoCen, "census indices fit 16 bits");
uint32_t g_cyc[kMaxItems];
thread_local bool t_inPass = false;
thread_local int t_depth = 0;
thread_local int t_slot = kSlots;
thread_local void* t_pass = nullptr;
thread_local void* const* t_begin = nullptr;
thread_local size_t t_n = 0, t_cursor = 0;
thread_local uint64_t t_first = 0, t_last = 0, t_probe = 0;
thread_local void* t_glob[kGlobCount];  // PS t87-t127 at the first item (identity only)
thread_local bool t_globOk = false, t_globChanged = false, t_globRead = false;
thread_local uint32_t t_calls = 0;

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

// ---- Classes ----
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
  if (c < kModelKinds) return ModelKindName(c);
  if (c == kClsMatOver) return "SceneRenderable, other material (class table full)";
  if (c == kClsSrNull) return "SceneRenderable without item or material";
  if (c == kClsNsOver) return "other renderable (class table full)";
  if (c >= kClsMat0 && c < kClsMat0 + g_matClsCount) return std::string("SceneRenderable ") + g_matCls[c - kClsMat0].name;
  const int i = c - kClsNs0;
  if (i < 0 || i >= g_nsCount.load()) return "?";
  return std::string(g_ns[i].name) + (g_ns[i].timed ? "" : g_ns[i].state == -1 ? " [untimed: slot hooked elsewhere]"
                                                                                  : " [untimed]");
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
      e.code = gbreccount::ReadBlendRaw(sh);
      return e.code;
    }
  }
  return gbreccount::ReadBlendRaw(sh);
}

// ---- Census ----
void ResolveCensus(CensusEntry& e) {
  std::vector<std::string> bound;
  std::string why;
  const int st = shadowinst::GbPassReadTextures(e.shader, e.tech, e.pass, &bound, nullptr, &why, nullptr);
  if (st == shadowtex::kReadsPending) return;
  if (st != shadowtex::kReadsReady) {
    e.state = 2;
    e.why = why;
    return;
  }
  for (const std::string& n : bound) {
    if (NameCatOf(n.c_str()) != kNcOther) continue;
    if (RegisterOf(n.c_str())) {
      e.globals += (e.globals.empty() ? "" : ",") + n;
      ++e.globalCount;
      continue;
    }
    const int r = gbreccount::RecordOfGuarded(static_cast<uint8_t*>(e.shader), n.c_str());
    if (r >= 0 && r < static_cast<int>(kMaxRecords)) {
      e.recMask[r >> 6] |= 1ull << (r & 63);
    } else {
      e.other += (e.other.empty() ? "" : ",") + n;
      ++e.otherCount;
    }
  }
  e.state = 1;
}

CensusEntry* CensusFind(void* shader, uint64_t tech, uint32_t pass) {
  if (!g_cen) return nullptr;
  uint8_t* s = static_cast<uint8_t*>(shader);
  void* effect = *reinterpret_cast<void**>(s + 0x50);
  void* techBegin = *reinterpret_cast<void**>(s + 0xb0);
  size_t i = PtrHash(reinterpret_cast<uint64_t>(shader) ^ tech * 0x9E37ull ^ pass * 0x51ull, kCensusTable - 1);
  for (size_t k = 0; k < kCensusTable; ++k, i = (i + 1) & (kCensusTable - 1)) {
    CensusEntry& e = g_cen[i];
    if (e.shader == shader && e.tech == tech && e.pass == pass && e.effect == effect && e.techBegin == techBegin)
      return &e;
    if (!e.shader) {
      if (g_cenUsed >= kCensusTable * 3 / 4) {
        ++g_cenFull;
        return nullptr;
      }
      e.shader = shader;
      e.tech = tech;
      e.pass = pass;
      e.effect = effect;
      e.techBegin = techBegin;
      e.state = 0;
      e.tryGen = 0;
      e.otherCount = e.globalCount = 0;
      ++g_cenUsed;
      return &e;
    }
  }
  return nullptr;
}

// ---- Materials ----
// Animated properties: their destinations are per-draw overrides; false when
// a class is not one of the five gb_batch replicates (or the list is odd).
bool AnimInfo(uint8_t* mat, uint64_t m[2]) {
  uint8_t* b = *reinterpret_cast<uint8_t**>(mat + 8);
  uint8_t* e = *reinterpret_cast<uint8_t**>(mat + 0x10);
  if (b == e) return true;
  if (!b || e < b || (e - b) % 16 || (e - b) / 16 > 32) return false;
  bool ok = true;
  for (uint8_t* p = b; p < e; p += 16) {
    uint8_t* dst = *reinterpret_cast<uint8_t**>(p);
    uint8_t* prop = *reinterpret_cast<uint8_t**>(p + 8);
    uint32_t bytes = 16;
    bool known = false;
    if (prop && g_md) {
      const void* pv = *reinterpret_cast<void**>(prop);
      for (const gbbatch::PropClass& pc : gbbatch::kProps)
        if (pv == g_md + pc.vtbl) {
          bytes = pc.bytes;
          known = true;
        }
    }
    ok &= known;
    MarkCb(m, (dst - mat) - static_cast<int64_t>(kCbOff), bytes);
  }
  return ok;
}

void VisitMat(MatEntry& e, uint8_t* mat, uint8_t* shader, uint64_t tech, uint32_t fxPass, SlotStat& st) {
  e.passGen = g_passGen;
  uint8_t f = 0;
  uint64_t over[2] = {};
  RouteOverrides(over);
  AnimInfo(mat, over);
  uint64_t all[2] = {~0ull, (1ull << (kCbDwords - 64)) - 1};
  all[0] &= ~over[0];
  all[1] &= ~over[1];
  uint64_t guard[2] = {all[0], all[1]};
  uint64_t used[2];
  if (shader && shadowinst::CbUsedDwords(shader, tech, fxPass, used)) {
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

inline uint8_t* EntriesOf(uint8_t* mat, uint8_t* item) {
  const uint8_t* props = *reinterpret_cast<uint8_t**>(mat + 0x28);
  auto* arr = *reinterpret_cast<uint8_t***>(item + 0x18);
  if (!props || !arr || !*arr) return nullptr;
  return *arr + static_cast<uint64_t>(*reinterpret_cast<const uint32_t*>(props + 0x26c)) * 0x18;
}

// ---- The walk before the pass (plain C body: callers hold SEH) ----
void ModelItem(size_t k, uint8_t* r, uint8_t* item, uint8_t* mat, SlotStat& st) {
  const uint32_t pass = *reinterpret_cast<uint32_t*>(r + 0x60);
  const uint8_t* props = *reinterpret_cast<uint8_t**>(mat + 0x28);
  const bool transparent = props && props[0x33] != 0;
  const bool selected = props && props[0x12d] != 0;
  const bool cockpit = r[0x64] != 0;
  uint8_t* sh = *reinterpret_cast<uint8_t**>(mat + 0x30);
  const bool dxShader = sh && g_shaderVt && *reinterpret_cast<void**>(sh) == g_shaderVt;
  const int blend = dxShader ? BlendOf(sh) : gbreccount::kBmUnknown;
  g_cls[k] = static_cast<uint8_t>(KindCode(pass, cockpit, transparent, blend));
  // The route 0x16140 takes: 4..13 elsewhere (8 flat_shadow), else 0x15e20.
  const bool normalRoute = pass < 4 || pass > 13;
  const int fxPass = normalRoute ? FxPassIndex(pass, cockpit, r[0x65] != 0, (r[0x68] & 1) != 0, !transparent, selected)
                                 : -1;
  st.passIdx[fxPass >= 0 && fxPass < kPassIdx - 1 ? fxPass : kPassIdx - 1]++;
  const uint64_t tech = *reinterpret_cast<uint64_t*>(mat + (cockpit ? 0x1e0 : 0x1d8));
  int why = kWRecordable;
  if (!dxShader) why = kWNoShader;
  else if (!normalRoute) why = kWTechnique;
  // Material bytes since render entry (any route).
  if (MatEntry* e = MatFind(mat)) {
    if (e->passGen != g_passGen)
      VisitMat(*e, mat, dxShader && normalRoute ? sh : nullptr, tech, fxPass > 0 ? fxPass : 0, st);
    if (e->flags & kMfGuardChg) st.itemsGuardChg++;
  } else {
    st.matsNoEntry++;
  }
  if (why == kWRecordable) {
    uint64_t dummy[2] = {};
    if (!AnimInfo(mat, dummy)) why = kWAnim;
  }
  if (why == kWRecordable) {
    shrec::MeshLive ml;
    if (shrec::ReadMesh(*reinterpret_cast<uint8_t**>(item + 0xc0), ml)) why = kWMesh;
  }
  // Textures: handles, entries, prediction; read records set by the draw.
  const uint32_t ntex = ngb::TexN(mat);
  uint64_t boundRec[kMaxRecords / 64] = {};
  const uint8_t* recs = dxShader ? *reinterpret_cast<uint8_t**>(sh + 0xc8) : nullptr;
  const uint8_t* recEnd = dxShader ? *reinterpret_cast<uint8_t**>(sh + 0xd0) : nullptr;
  const int64_t nrec = recs && recEnd > recs ? (recEnd - recs) / 0x50 : 0;
  const uint8_t* en = ntex ? EntriesOf(mat, item) : nullptr;
  const uint64_t size = *reinterpret_cast<uint64_t*>(r + 0x6c);
  bool texBad = false, anyTex = false;
  for (uint32_t i = 0; i < ntex && i < 32 && dxShader; ++i) {
    const int64_t h = ngb::Tex(mat)[i];
    if (h == -1) continue;
    uint8_t* tex = en ? *reinterpret_cast<uint8_t* const*>(en + i * 0x18 + 8) : nullptr;
    if (!tex) continue;  // nothing set: the inheritance check covers read records
    anyTex = true;
    if (h >= 0 && h < static_cast<int64_t>(kMaxRecords)) boundRec[h >> 6] |= 1ull << (h & 63);
    st.texSets++;
    if (!g_texOk) continue;
    if (h < 0 || h >= nrec) {
      texBad = true;
      continue;
    }
    const int32_t type = *reinterpret_cast<const int32_t*>(recs + h * 0x50 + 0xc);
    void* view = nullptr;
    const uint8_t w =
        gbbatch::PredictView(g_env, tex, *reinterpret_cast<const int64_t*>(en + i * 0x18), type, size, &view);
    st.texReason[w < gbbatch::kReasons ? w : 0]++;
    if (w != gbbatch::kOk) texBad = true;
  }
  if (why == kWRecordable && (texBad || (anyTex && !g_texOk))) why = kWTexture;
  CensusEntry* ce = nullptr;
  if (why == kWRecordable) {
    ce = g_census ? CensusFind(sh, tech, static_cast<uint32_t>(fxPass)) : nullptr;
    if (ce && ce->state == 0 && ce->tryGen != g_passGen) {
      ce->tryGen = g_passGen;
      ResolveCensus(*ce);
    }
    if (ce) g_cenIdx[k] = static_cast<uint16_t>(ce - g_cen);
    if (!ce || ce->state == 0) {
      why = kWCensusPending;
    } else if (ce->state == 2) {
      why = kWCensusFailed;
    } else {
      bool inherit = false;
      for (uint32_t w = 0; w < kMaxRecords / 64; ++w) inherit |= (ce->recMask[w] & ~boundRec[w]) != 0;
      if (inherit) why = kWInherit;
      else if (ce->otherCount) why = kWUnmapped;
      else if (ce->globalCount) why = kWGlobals;
    }
  }
  g_why[k] = static_cast<uint8_t>(why);
  g_flags[k] = FlagsFor(why, dxShader, Blended(blend, pass));
  st.modelItems++;
  st.why[why]++;
}

bool AnalyseRaw(void* const* b, size_t n, SlotStat& st) {
  __try {
    for (size_t k = 0; k < n && k < kMaxItems; ++k) {
      g_cyc[k] = 0;
      g_flags[k] = 0;
      g_why[k] = 0xff;
      g_cenIdx[k] = kNoCen;
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

// ---- Render entry: material copies, sort lead, ordinals ----
void SnapshotRaw(void* rg, void* renderables) {
  __try {
    const size_t count = shadowbatch::VectorCountRaw(static_cast<uint8_t*>(rg), renderables);
    if (!count) return;
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
  g_ordinal = 0;
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
  if (g_mats && g_matsUsed >= kMatTable * 3 / 4) {
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

// ---- Setup probe at the first item (render thread; time subtracted) ----
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

bool ReadTogglesRaw(void* pass, uint8_t out[2]) {
  __try {
    memcpy(out, static_cast<uint8_t*>(pass) + 0x70 + 0x5ac, 2);
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
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
  for (int i = 0; i < 8; ++i) {
    s.rtv[i] = rtv[i];
    s.nrt += rtv[i] != nullptr;
  }
  s.dsv = dsv;
  s.nvp = nvp;
  for (UINT i = 0; i < nvp && i < 4; ++i) s.vp[i] = vps[i];
  s.nsc = nsc;
  for (UINT i = 0; i < nsc && i < 4; ++i) s.sc[i] = scs[i];
  {
    ID3D11ShaderResourceView* g[kGlobCount] = {};
    g_ctx->PSGetShaderResources(kGlobFirst, kGlobCount, g);
    uint64_t bound = 0;
    for (UINT i = 0; i < kGlobCount; ++i) {
      t_glob[i] = g[i];
      if (g[i]) {
        bound |= 1ull << i;
        g[i]->Release();
      }
    }
    st.globBound |= bound;
    t_globOk = true;
    t_globChanged = t_globRead = false;
  }
  if (!ReadRendererRaw(s)) s.rendererOk = s.ratioOk = false;
  s.togglesOk = t_pass && ReadTogglesRaw(t_pass, s.toggles);
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
    const bool cTog = s.togglesOk && p.togglesOk && memcmp(s.toggles, p.toggles, 2) != 0;
    st.chRtv += cRtv;
    st.chDsv += cDsv;
    st.chVp += cVp;
    st.chSc += cSc;
    st.chFlags += cFlags;
    st.chDbg += cDbg;
    st.chRatio += cRatio;
    st.chToggles += cTog;
    if (cRtv || cDsv || cVp || cSc || cFlags || cDbg || cRatio || cTog) st.unstable++;
  }
  st.have = true;
  st.last = s;
  // Identity only: the pointers are compared, never used after release.
  for (auto* p : rtv)
    if (p) p->Release();
  if (dsv) dsv->Release();
}

// After an item: are t87-t127 still what the first item saw? (identity only)
void RecheckGlobals(SlotStat& st) {
  ID3D11ShaderResourceView* g[kGlobCount] = {};
  g_ctx->PSGetShaderResources(kGlobFirst, kGlobCount, g);
  st.globReads++;
  t_globRead = true;
  for (UINT i = 0; i < kGlobCount; ++i) {
    if (g[i] != t_glob[i]) {
      st.globSlotsChanged |= 1ull << i;
      t_globChanged = true;
    }
    if (g[i]) g[i]->Release();
  }
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
  const int c = k >= 0 ? g_cls[k] : fallback;
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
  if (t_globOk && g_ctx && (++t_calls % kGlobEvery) == 0) RecheckGlobals(st);
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

// ---- The SimplePassData execute ----
// The pass's item vector {begin, end, cap}, or nullptr (empty static vector) [V 0xa961c].
void** ItemVector(void* pass, void* ctx) {
  __try {
    auto* node = *reinterpret_cast<uint8_t**>(static_cast<uint8_t*>(pass) + 0x48 + 0x20);
    if (!node) return nullptr;
    const int16_t idx = *reinterpret_cast<int16_t*>(node + 0x38);
    if (idx == -1) return nullptr;
    auto* base = *reinterpret_cast<uint8_t**>(*static_cast<uint8_t**>(ctx) + 0x438);
    if (!base) return nullptr;
    return reinterpret_cast<void**>(base + static_cast<int64_t>(idx) * 24);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return nullptr;
  }
}

// The inline name (length at pass+8, at most 32 chars at pass+0x10 [V 0xa9310]).
void PassNameRaw(void* pass, char out[40]) {
  out[0] = 0;
  __try {
    const uint64_t n = *reinterpret_cast<uint64_t*>(static_cast<uint8_t*>(pass) + 8);
    if (n > 32) return;
    memcpy(out, static_cast<uint8_t*>(pass) + 0x10, static_cast<size_t>(n));
    out[n] = 0;
    for (uint64_t i = 0; i < n; ++i)
      if (out[i] < 0x20 || out[i] > 0x7e) out[i] = '?';
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    out[0] = 0;
  }
}

inline uint64_t NameHash(const char* s) {
  uint64_t h = 0xcbf29ce484222325ull;
  for (; *s; ++s) h = (h ^ static_cast<uint8_t>(*s)) * 0x100000001b3ull;
  return h;
}

int SlotOf(void* pass, void* ctx, void** vec) {
  char name[40];
  PassNameRaw(pass, name);
  const uint64_t nh = NameHash(name);
  __try {
    auto* rg = *static_cast<uint8_t**>(ctx);
    auto* base = *reinterpret_cast<uint8_t**>(rg + 0x438);
    const intptr_t d = reinterpret_cast<uint8_t*>(vec) - base;
    if (!base || d < 0 || d % 24 != 0 || d / 24 > 0xffff) return kSlots;
    const uint32_t idx = static_cast<uint32_t>(d / 24);
    const int n = g_learnedCount.load(std::memory_order_relaxed);
    for (int k = 0; k < n; ++k)
      if (g_learned[k].rg == rg && g_learned[k].idx == idx && g_learned[k].nameHash == nh) return k;
    if (n == kSlots) return kSlots;
    Learned& l = g_learned[n];
    l.rg = rg;
    l.idx = idx;
    l.nameHash = nh;
    memcpy(l.name, name, sizeof(l.name));
    l.kind = KindOfName(name);
    g_learnedCount.store(n + 1, std::memory_order_release);
    return n;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return kSlots;
  }
}

void AfterPass(int slot, size_t n) {
  SlotStat& st = g_stat[slot];
  ExecSample es{};
  es.slot = static_cast<uint8_t>(slot);
  for (size_t k = 0; k < n; ++k) {
    if (g_why[k] == 0xff) continue;
    st.whyCyc[g_why[k]] += g_cyc[k];
    st.modelCyc += g_cyc[k];
    if (g_flags[k] & kFBlend) {
      st.blendItems++;
      st.blendCyc += g_cyc[k];
    }
    if (g_cenIdx[k] != kNoCen && g_cen) {
      CensusEntry& e = g_cen[g_cenIdx[k]];
      e.draws++;
      e.cyc += g_cyc[k];
    }
  }
  for (int s = 0; s < kSets; ++s)
    for (int i = 0; i < kIslands; ++i) {
      const SegResult r = Segments(g_flags, g_cyc, n, kSetNeed[s], kIsland[i]);
      SetStat& a = st.seg[s][i];
      a.segments += r.segments;
      a.recItems += r.recItems;
      a.recCyc += r.recCyc;
      a.resBetweenCyc += r.resBetweenCyc;
      a.resOutsideCyc += r.resOutsideCyc;
      a.resBetweenItems += r.resBetweenItems;
      a.blendItems += r.blendItems;
      es.seg[s][i] = static_cast<uint16_t>(r.segments < 0xffff ? r.segments : 0xffff);
    }
  if (g_samples.size() < g_samples.capacity()) g_samples.push_back(es);
  else g_samplesDropped++;
}

void __fastcall Hook(void* pass, void* ctx) {
  const DWORD tid = GetCurrentThreadId();
  if (g_on.load(std::memory_order_relaxed) && !g_renderTid.load(std::memory_order_relaxed)) g_renderTid = tid;
  if (!g_on.load(std::memory_order_relaxed) || t_inPass || tid != g_renderTid.load(std::memory_order_relaxed)) {
    g_orig(pass, ctx);
    return;
  }
  g_inside.fetch_add(1);
  const uint64_t t0 = __rdtsc();
  void** vec = ItemVector(pass, ctx);
  const int slot = vec ? SlotOf(pass, ctx, vec) : kSlots;
  SlotStat& st = g_stat[slot];
  st.ordinalSum += g_ordinal++;
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
  if (vec) {
    auto* b = static_cast<void* const*>(vec[0]);
    auto* e = static_cast<void* const*>(vec[1]);
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
  t_first = t_last = t_probe = 0;
  t_cursor = 0;
  t_calls = 0;
  t_globOk = t_globChanged = t_globRead = false;
  t_inPass = true;
  const uint64_t a = __rdtsc();
  g_orig(pass, ctx);
  const uint64_t b = __rdtsc();
  t_inPass = false;
  if (t_globRead) {
    st.globCalls++;
    st.globChanged += t_globChanged;
  }
  st.origCalls++;
  st.cycOrig += b - a;
  st.cycWrap += b - a;
  if (t_first) {
    st.cycPre += t_first - a;
    st.cycLoop += t_last - t_first;
    st.cycPost += b - t_last;
    st.cycProbe += t_probe;
  } else {
    st.emptyPasses++;
  }
  st.passes++;
  if (t_n) AfterPass(slot, n);
  t_begin = nullptr;
  t_n = 0;
  t_pass = nullptr;
  g_inside.fetch_sub(1);
}

// ---------------------------------------------------------------------------
// Install / teardown
// ---------------------------------------------------------------------------
const char* VerifyBuild(uint8_t* gc) {
  auto** vtbl = reinterpret_cast<void**>(gc + kVtableRva);
  if (!allocslab::RttiIs(gc, vtbl, kRtti)) return "SimplePassData vtable not found at the analysed address";
  if (reinterpret_cast<uint8_t*>(SlotOriginal(&vtbl[kExecSlot])) != gc + kThunkRva)
    return "SimplePassData execute slot differs from the analysed build";
  if (vtbl[kExecSlot] != SlotOriginal(&vtbl[kExecSlot])) return "SimplePassData execute slot hooked by another module";
  for (const Code& c : kCode)
    if (!shadowtex::CodeIs(gc, c.begin, c.end, c.hash)) return "SimplePassData execute code differs from the analysed build";
  return nullptr;
}

bool HookPass() {
  auto* gc = reinterpret_cast<uint8_t*>(GetModuleHandleW(L"GraphicsCore.dll"));
  if (!gc) return false;
  if (const char* why = VerifyBuild(gc)) {
    Log("  forward rec counter: %s; skipped", why);
    return false;
  }
  g_slot19 = &reinterpret_cast<void**>(gc + kVtableRva)[kExecSlot];
  g_orig = reinterpret_cast<ExecFn>(SlotOriginal(g_slot19));
  if (!HookSlot(g_slot19, reinterpret_cast<void*>(&Hook), nullptr)) {
    g_slot19 = nullptr;
    return false;
  }
  return true;
}

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
    if (!shrec::PatchSortSites(false)) Log("  forward rec S0: WARNING could not restore Scene's sort call sites");
    g_ownSort = false;
  }
  shrec::g_sortBorrowed = false;
}

void Unchain() {
  g_on = false;
  if (!g_chained.exchange(false)) return;
  if (g_slot19) UnhookSlot(g_slot19, reinterpret_cast<void*>(g_orig));  // g_orig stays for calls in flight
  g_slot19 = nullptr;
  gbcount::OverrideFn ov = &Override;
  gbcount::g_override.compare_exchange_strong(ov, g_prevOverride);
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

bool Drain() {
  for (int i = 0; i < 400 && (g_inside.load() != 0 || shrec::g_sortInside.load() != 0); ++i) Sleep(5);
  if (g_inside.load() != 0) return false;
  if (g_ctx) g_ctx->Release();
  g_ctx = nullptr;
  return true;
}

// Payload stop.
void Shutdown() {
  if (!g_chained.load()) return;
  Unchain();
  if (shrec::g_sortStub && !shrec::g_sortPatched)
    reinterpret_cast<std::atomic<void*>*>(shrec::g_sortStub + 16)->store(reinterpret_cast<void*>(shrec::g_sortOrig));
  if (!Drain()) Log("forward rec counter: a pass was still running at unload; its context reference is left");
}

void ResetStats() {
  for (SlotStat& st : g_stat) memset(&st, 0, sizeof(st));
  g_samples.clear();
  g_samplesDropped = 0;
  g_unplanned = g_analyseFaults = 0;
  g_snapRenders = g_snapCyc = g_snapMats = g_snapFaults = g_tableWipes = 0;
  g_sortCalls = 0;
  for (auto& t : g_sortTick) t = 0;
  if (g_cen)
    for (size_t i = 0; i < kCensusTable; ++i) g_cen[i].draws = g_cen[i].cyc = 0;
  g_cenFull = 0;
  g_entryTick = 0;
  g_ordinal = 0;
}

// ---------------------------------------------------------------------------
// Report
// ---------------------------------------------------------------------------
void AddSlot(SlotStat& t, const SlotStat& a) {
  const uint64_t* src = reinterpret_cast<const uint64_t*>(&a);
  uint64_t* dst = reinterpret_cast<uint64_t*>(&t);
  const size_t sums = offsetof(SlotStat, sortMinLead) / sizeof(uint64_t);
  for (size_t i = 0; i < sums; ++i) dst[i] += src[i];
  for (int x = 0; x < kSets; ++x)
    for (int i = 0; i < kIslands; ++i) {
      SetStat& d = t.seg[x][i];
      const SetStat& s = a.seg[x][i];
      d.segments += s.segments;
      d.recItems += s.recItems;
      d.recCyc += s.recCyc;
      d.resBetweenCyc += s.resBetweenCyc;
      d.resOutsideCyc += s.resOutsideCyc;
      d.resBetweenItems += s.resBetweenItems;
      d.blendItems += s.blendItems;
    }
}

void Report(double f) {
  const double ms = 1000.0 / g_tscHz / f;  // cycles -> ms/frame
  const double cycMs = 1000.0 / g_tscHz;   // cycles -> ms
  auto pct = [](double a, double b) { return b > 0 ? 100.0 * a / b : 0.0; };
  SlotStat* tp = new SlotStat();
  memset(tp, 0, sizeof(*tp));
  SlotStat& t = *tp;
  for (int s = 0; s <= kSlots; ++s) AddSlot(t, g_stat[s]);
  uint64_t modelCyc = 0, modelItems = 0;
  for (int c = 0; c < kClasses; ++c) {
    if (c < kModelKinds) {
      modelCyc += t.cls[c].cyc;
      modelItems += t.cls[c].items;
    }
  }
  const double loopNet = static_cast<double>(t.cycLoop) - static_cast<double>(t.cycProbe);
  const int nl = g_learnedCount.load();
  Log("  forward rec S0: %.0f frames; %d SimplePassData calls learned; %.2f calls/frame (%.2f without items), %.0f "
      "items/frame, %.0f ModelMaterialMT draws/frame; %llu walk faults, %llu oversized vectors, %llu item calls "
      "outside the walked vector",
      f, nl, t.passes / f, t.emptyPasses / f, t.items / f, modelItems / f, static_cast<unsigned long long>(t.faults),
      static_cast<unsigned long long>(t.oversize), static_cast<unsigned long long>(g_unplanned));
  Log("  forward rec S0 time (render thread, ms/frame): SimplePassData execute %.3f = pre-loop (frame buffer, clears, "
      "binder) %.3f + loop %.3f + post-loop %.3f; loop net of our probes %.3f (probes %.3f); our walk before each "
      "call %.3f (not in the above)",
      t.cycOrig * ms, t.cycPre * ms, t.cycLoop * ms, t.cycPost * ms, loopNet * ms, t.cycProbe * ms, t.cycAnalyse * ms);
  // Per pass kind (name suffix).
  {
    struct K {
      uint64_t calls = 0, items = 0, model = 0, cycOrig = 0, cycLoop = 0, cycProbe = 0, modelCyc = 0, blend = 0;
      uint64_t rec = 0, recCyc = 0, segs = 0, strictCyc = 0;
      int slots = 0;
    };
    K k[kKinds];
    for (int s = 0; s < nl; ++s) {
      const SlotStat& a = g_stat[s];
      K& x = k[g_learned[s].kind];
      x.slots++;
      x.calls += a.passes;
      x.items += a.items;
      x.model += a.modelItems;
      x.cycOrig += a.cycOrig;
      x.cycLoop += a.cycLoop;
      x.cycProbe += a.cycProbe;
      x.modelCyc += a.modelCyc;
      x.blend += a.blendItems;
      x.rec += a.seg[kGateSet][kGateIsland].recItems;
      x.recCyc += a.seg[kGateSet][kGateIsland].recCyc;
      x.segs += a.seg[kGateSet][kGateIsland].segments;
      x.strictCyc += a.seg[0][kGateIsland].recCyc;
    }
    Log("  forward rec S0 by pass kind (per frame; ms render thread; recordable = +late-bound globals, island 30):");
    Log("    %-28s %5s %6s %7s %7s %7s %7s %7s %8s %8s %8s %6s", "kind", "slots", "calls", "items", "model",
        "blended", "ms exec", "ms net", "ms model", "ms rec", "ms strict", "segs");
    for (int i = 0; i < kKinds; ++i) {
      const K& x = k[i];
      if (!x.calls) continue;
      Log("    %-28s %5d %6.2f %7.0f %7.0f %7.0f %7.3f %7.3f %8.3f %8.3f %8.3f %6.2f", kKindName[i], x.slots, x.calls / f,
          x.items / f, x.model / f, x.blend / f, x.cycOrig * ms,
          (static_cast<double>(x.cycLoop) - static_cast<double>(x.cycProbe)) * ms, x.modelCyc * ms, x.recCyc * ms,
          x.strictCyc * ms, x.segs / f);
    }
  }
  Log("  forward rec S0 classes (items/frame, timed calls/frame, ms/frame, ns/call, share of net loop):");
  std::vector<int> order;
  for (int c = 0; c < kClasses; ++c)
    if (t.cls[c].items || t.cls[c].calls) order.push_back(c);
  std::sort(order.begin(), order.end(), [&](int a, int b) { return t.cls[a].cyc > t.cls[b].cyc; });
  for (int c : order) {
    const ClassStat& x = t.cls[c];
    Log("    %-84.84s %8.1f %8.1f %7.3f %6.0f %5.1f%%", ClassName(c).c_str(), x.items / f, x.calls / f, x.cyc * ms,
        x.calls ? x.cyc * 1e9 / g_tscHz / x.calls : 0.0, pct(static_cast<double>(x.cyc), loopNet));
  }
  {
    double byB[kBuckets] = {}, msB[kBuckets] = {};
    for (int c = 0; c < kModelKinds; ++c) {
      byB[(c >> 5) % kBuckets] += t.cls[c].items / f;
      msB[(c >> 5) % kBuckets] += t.cls[c].cyc * ms;
    }
    Log("  forward rec S0 ModelMaterialMT by model pass number (per frame): pass 1 %.0f (%.3f ms), pass 2 %.0f (%.3f "
        "ms), pass 3 %.0f (%.3f ms), pass 8 flat_shadow %.0f (%.3f ms), other %.0f (%.3f ms)",
        byB[0], msB[0], byB[1], msB[1], byB[2], msB[2], byB[3], msB[3], byB[4], msB[4]);
    std::string line;
    char b[48];
    for (int i = 0; i < kPassIdx; ++i) {
      if (!t.passIdx[i]) continue;
      snprintf(b, sizeof(b), "%s%s%d %.0f", line.empty() ? "" : ", ", i < kPassIdx - 1 ? "P" : "other route ",
               i < kPassIdx - 1 ? i : 0, t.passIdx[i] / f);
      line += b;
    }
    Log("  forward rec S0 FX pass index NGModel 0x15e20 picks (per frame; \"other route\" = model passes 4..13): %s",
        line.empty() ? "none" : line.c_str());
  }
  Log("  forward rec S0 ModelMaterialMT draws by first failing gb_rec rule (per frame, ms/frame of their calls):");
  for (int w = 0; w < kWhys; ++w)
    if (t.why[w])
      Log("    %-96.96s %8.1f %7.3f", kWhyName[w], t.why[w] / f, t.whyCyc[w] * ms);
  Log("  forward rec S0 blended model draws (order-dependent; kept in order by the recorder): %.0f/frame, %.3f ms/frame "
      "(%.1f%% of the model draws)",
      t.blendItems / f, t.blendCyc * ms, pct(static_cast<double>(t.blendItems), static_cast<double>(t.modelItems)));
  // Census keys: unmapped names (globals).
  if (g_cen) {
    struct O {
      int keys = 0;
      uint64_t draws = 0, cyc = 0;
    };
    std::map<std::string, O> other;
    std::map<std::string, int> failed;
    std::map<std::string, uint64_t> byName;  // each unmapped name: draws of the keys binding it
    std::map<std::string, uint64_t> byGlobal;  // each fixed-register global: draws of the keys binding it
    uint32_t keys = 0, keysClean = 0, keysGlobals = 0;
    for (size_t i = 0; i < kCensusTable; ++i) {
      const CensusEntry& e = g_cen[i];
      if (!e.shader) continue;
      if (e.state == 1) {
        ++keys;
        keysClean += e.otherCount == 0 && e.globalCount == 0;
        keysGlobals += e.otherCount == 0 && e.globalCount != 0;
        for (const std::string& nm : SplitNames(e.globals)) byGlobal[nm] += e.draws;
        if (!e.otherCount) continue;
        O& o = other[e.other];
        o.keys++;
        o.draws += e.draws;
        o.cyc += e.cyc;
        for (const std::string& nm : SplitNames(e.other)) byName[nm] += e.draws;
      } else if (e.state == 2) {
        failed[e.why]++;
      }
    }
    Log("  forward rec S0 census keys (shader, technique, FX pass): %u analysed, %u bind only mappable names, %u also "
        "fixed-register globals%s",
        keys, keysClean, keysGlobals, g_cenFull ? "; census table full" : "");
    {
      std::vector<std::pair<uint64_t, std::string>> gl;
      for (auto& kv : byGlobal) gl.push_back({kv.second, kv.first});
      std::sort(gl.rbegin(), gl.rend());
      std::string gline;
      char gb[64];
      for (size_t i = 0; i < gl.size() && i < 40; ++i) {
        snprintf(gb, sizeof(gb), " (%s) %.0f", RegisterOf(gl[i].second.c_str()), gl[i].first / f);
        gline += (i ? ", " : "") + gl[i].second + gb;
      }
      if (!gline.empty())
        Log("  forward rec S0 fixed-register globals (draws/frame of the keys binding each): %.1500s", gline.c_str());
    }
    std::vector<std::pair<uint64_t, std::string>> n;
    for (auto& kv : byName) n.push_back({kv.second, kv.first});
    std::sort(n.rbegin(), n.rend());
    std::string line;
    char b[48];
    for (size_t i = 0; i < n.size() && i < 24; ++i) {
      snprintf(b, sizeof(b), " %.0f", n[i].first / f);
      line += (i ? ", " : "") + n[i].second + b;
    }
    if (!line.empty())
      Log("  forward rec S0 unmapped names (draws/frame of the keys binding each): %.1500s", line.c_str());
    std::vector<std::pair<uint64_t, std::string>> v;
    for (auto& kv : other) v.push_back({kv.second.draws, kv.first});
    std::sort(v.rbegin(), v.rend());
    for (size_t i = 0; i < v.size() && i < 8; ++i) {
      const O& o = other[v[i].second];
      Log("    %3d keys, %7.1f draws/frame, %.3f ms/frame bind: %.700s", o.keys, o.draws / f, o.cyc * ms,
          v[i].second.c_str());
    }
    line.clear();
    for (auto& kv : failed) line += (line.empty() ? "" : "; ") + kv.first + " x" + std::to_string(kv.second);
    if (!line.empty()) Log("  forward rec S0 census failed keys: %.400s", line.c_str());
  }
  if (g_texOk) {
    std::string line;
    char buf[96];
    for (int r = 0; r < gbbatch::kReasons; ++r) {
      if (!t.texReason[r]) continue;
      snprintf(buf, sizeof(buf), "%s%s %.1f", line.empty() ? "" : ", ",
               r == gbbatch::kOk ? "predicted" : gbbatch::kReasonName[r], t.texReason[r] / f);
      line += buf;
    }
    Log("  forward rec S0 texture sets of model draws: %.0f/frame; views by prediction (per frame): %s", t.texSets / f,
        line.empty() ? "none" : line.c_str());
  } else {
    Log("  forward rec S0 texture views: not predicted (texture classes of this build not verified): every textured "
        "draw counts as not recordable");
  }
  Log("  forward rec S0 materials (once per material per call, per frame): %.0f checked, %.0f without a render-entry "
      "copy, %.0f without reflection (whole CB guarded), %.0f not in the table; changed since RenderGraph::render "
      "entry: guarded dwords %.1f (draws %.1f), whole CB minus route overrides %.1f; render-entry copies %.0f "
      "materials/frame, %.3f ms/frame, %llu faults, %llu wipes",
      t.mats / f, t.matsNoSnap / f, t.matsNoRefl / f, t.matsNoEntry / f, t.matsGuardChg / f, t.itemsGuardChg / f,
      t.matsCbChg / f, g_snapMats / f, g_snapCyc * ms, static_cast<unsigned long long>(g_snapFaults),
      static_cast<unsigned long long>(g_tableWipes));
  // Per slot.
  double maxModel = 0;
  double slotModel[kSlots] = {};
  for (int s = 0; s < nl; ++s) {
    slotModel[s] = g_stat[s].passes ? static_cast<double>(g_stat[s].modelItems) / g_stat[s].passes : 0.0;
    maxModel = std::max(maxModel, slotModel[s]);
  }
  bool mainSlot[kSlots] = {};
  for (int s = 0; s < nl; ++s) mainSlot[s] = maxModel > 0 && slotModel[s] >= 0.25 * maxModel;
  std::vector<int> slots;
  for (int s = 0; s < nl; ++s)
    if (g_stat[s].passes) slots.push_back(s);
  std::sort(slots.begin(), slots.end(), [](int a, int b) { return g_stat[a].cycOrig > g_stat[b].cycOrig; });
  Log("  forward rec S0 calls (slot = graph, collection, name; sorted by execute time):");
  for (int s : slots) {
    const SlotStat& a = g_stat[s];
    std::vector<double> segs;
    for (const ExecSample& e : g_samples)
      if (e.slot == s) segs.push_back(e.seg[kGateSet][kGateIsland]);
    char sortBuf[160];
    if (!g_sortObserved)
      snprintf(sortBuf, sizeof(sortBuf), "not observed");
    else if (!a.sortN)
      snprintf(sortBuf, sizeof(sortBuf), "never seen by the sort hook (%llu entries)",
               static_cast<unsigned long long>(a.sortMissing + a.sortLate));
    else
      snprintf(sortBuf, sizeof(sortBuf), "%.3f ms before entry (min %.3f)", a.sortLeadCyc * cycMs / a.sortN,
               a.sortMinLead * cycMs);
    const double p = static_cast<double>(a.passes);
    Log("    slot %2d%s \"%s\" (%s, collection %u): %.2f/frame, ordinal #%.1f, start %.3f ms after entry (max %.3f); "
        "items %.0f, model %.0f, blended %.0f per call; ms per call: execute %.3f (pre %.3f, loop net %.3f, model "
        "%.3f, post %.3f), recordable (+globals, island 30) %.3f, strict %.3f; segments median %.1f max %.0f; vector "
        "final %s",
        s, mainSlot[s] ? " main" : "", g_learned[s].name, kKindName[g_learned[s].kind], g_learned[s].idx, p / f,
        a.ordinalSum / p, a.offN ? a.offCyc * cycMs / a.offN : 0.0, a.offMaxCyc * cycMs, a.items / p,
        a.modelItems / p, a.blendItems / p, a.cycOrig * cycMs / p, a.cycPre * cycMs / p,
        (static_cast<double>(a.cycLoop) - static_cast<double>(a.cycProbe)) * cycMs / p, a.modelCyc * cycMs / p,
        a.cycPost * cycMs / p, a.seg[kGateSet][kGateIsland].recCyc * cycMs / p, a.seg[0][kGateIsland].recCyc * cycMs / p,
        Median(segs), segs.empty() ? 0.0 : *std::max_element(segs.begin(), segs.end()), sortBuf);
    if (a.samples)
      Log("      setup over %llu calls: changed vs the previous one: RTVs %llu (%u bound), DSV %llu, viewports %llu "
          "(%u: %.0fx%.0f), scissors %llu, renderer+0xd4 %llu (0x%x), +0x2120 %llu (0x%x), ratios %llu, toggles %llu "
          "(%02x %02x)%s; unstable %llu",
          static_cast<unsigned long long>(a.samples), static_cast<unsigned long long>(a.chRtv), a.last.nrt,
          static_cast<unsigned long long>(a.chDsv), static_cast<unsigned long long>(a.chVp), a.last.nvp,
          a.last.vp[0].Width, a.last.vp[0].Height, static_cast<unsigned long long>(a.chSc),
          static_cast<unsigned long long>(a.chFlags), a.last.flags, static_cast<unsigned long long>(a.chDbg),
          a.last.dbg, static_cast<unsigned long long>(a.chRatio), static_cast<unsigned long long>(a.chToggles),
          a.last.toggles[0], a.last.toggles[1], a.rendererMissing ? ", renderer not readable" : "",
          static_cast<unsigned long long>(a.unstable));
  }
  if (g_stat[kSlots].passes)
    Log("    (not learnable: %.2f calls/frame, %.0f items/frame)", g_stat[kSlots].passes / f, g_stat[kSlots].items / f);
  // Segments.
  Log("  forward rec S0 segments (one Execute each; main calls: median/p90/max per call; all calls: per frame):");
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
      Log("    %-22s island %3u: median %5.1f p90 %5.0f max %5.0f; per frame %6.1f segments, recorded %6.0f draws "
          "(%5.1f%% of model, %.0f blended) %.3f ms, residual between segments %.3f ms (%.0f items), outside %.3f ms",
          kSetName[x], kIsland[i], med, Quantile(v, 0.9), v.empty() ? 0.0 : *std::max_element(v.begin(), v.end()),
          a.segments / f, a.recItems / f, pct(static_cast<double>(a.recItems), static_cast<double>(modelItems)),
          a.blendItems / f, a.recCyc * ms, a.resBetweenCyc * ms, a.resBetweenItems / f, a.resOutsideCyc * ms);
    }
  if (g_samplesDropped) Log("    (%llu calls not kept for the medians)", static_cast<unsigned long long>(g_samplesDropped));
  if (g_sortObserved)
    Log("  forward rec S0 sort hook: %.1f sort calls/frame observed (%s)", g_sortCalls.load() / f,
        g_ownSort ? "call sites patched for this phase" : "the recorders' patch");
  // Gates.
  GateIn in;
  in.modelMs = modelCyc * ms;
  in.recMs = t.seg[kGateSet][kGateIsland].recCyc * ms;
  in.segMeasured = segMeasured;
  in.medianSeg = gateMedian;
  in.segPerFrame = t.seg[kGateSet][kGateIsland].segments / f;
  const double globalsItems = static_cast<double>(t.why[kWRecordable] + t.why[kWGlobals]);
  in.censusMeasured = g_census && globalsItems > 0;
  in.strictPct = pct(static_cast<double>(t.why[kWRecordable]), globalsItems);
  uint64_t compared = 0;
  for (int s = 0; s < kSlots; ++s) compared += g_stat[s].samples ? g_stat[s].samples - 1 : 0;
  in.stableSamples = compared;
  in.stablePct = compared ? 100.0 - pct(static_cast<double>(t.unstable), static_cast<double>(compared)) : 0.0;
  in.guardMeasured = t.mats > t.matsNoSnap;
  in.guardChanges = t.matsGuardChg;
  in.globCalls = t.globCalls;
  in.globStablePct = t.globCalls ? 100.0 - pct(static_cast<double>(t.globChanged), static_cast<double>(t.globCalls)) : 0.0;
  uint64_t globBound = 0, globChangedMask = 0;
  for (int s = 0; s <= kSlots; ++s) {
    globBound |= g_stat[s].globBound;
    globChangedMask |= g_stat[s].globSlotsChanged;
  }
  std::string boundList, changedList;
  for (UINT i = 0; i < kGlobCount; ++i) {
    char b[8];
    snprintf(b, sizeof(b), "%st%u", "", kGlobFirst + i);
    if (globBound >> i & 1) boundList += (boundList.empty() ? "" : " ") + std::string(b);
    if (globChangedMask >> i & 1) changedList += (changedList.empty() ? "" : " ") + std::string(b);
  }
  Log("  forward rec S0 PS t87-t127 within calls (re-read after every %u-th item call): %llu calls checked, %llu with "
      "a change (%.0f reads); bound at a first item: %s; changed: %s",
      kGlobEvery, static_cast<unsigned long long>(t.globCalls), static_cast<unsigned long long>(t.globChanged),
      static_cast<double>(t.globReads), boundList.empty() ? "none" : boundList.c_str(),
      changedList.empty() ? "none" : changedList.c_str());
  const GateOut o = Evaluate(in);
  Log("  forward rec S0 gate 1 (ModelMaterialMT time in SimplePassData >= 1.5 ms/frame): %.3f ms/frame of net loop "
      "%.3f ms (%.1f%%), %.0f draws/frame -> %s",
      in.modelMs, loopNet * ms, pct(static_cast<double>(modelCyc), loopNet), modelItems / f, GateWord(o.g[0]));
  Log("  forward rec S0 gate 2 (recordable time, +late-bound globals, island 30, >= 1.0 ms/frame): %.3f ms/frame "
      "(strict rules: %.3f) -> %s",
      in.recMs, t.seg[0][kGateIsland].recCyc * ms, GateWord(o.g[1]));
  Log("  forward rec S0 gate 3 (median segments per main call <= 4 and Executes x %.0f us <= 20%% of the recordable "
      "time): median %.1f, %.1f segments/frame = %.3f ms -> %s",
      kExecMs * 1000, in.medianSeg, in.segPerFrame, in.segPerFrame * kExecMs, GateWord(o.g[2]));
  Log("  forward rec S0 gate 4 (census: strictly mappable >= 90%% of the +globals draws; FAIL = the recorder must "
      "late-bind per-pass globals): %.1f%% of %.0f draws/frame -> %s",
      in.strictPct, globalsItems / f, GateWord(o.g[3]));
  Log("  forward rec S0 gate 5 (RTs/DSV/viewport/scissor/flags/ratios/toggles stable >= 99%% of calls): %.2f%% of "
      "%llu compared -> %s",
      in.stablePct, static_cast<unsigned long long>(compared), GateWord(o.g[4]));
  Log("  forward rec S0 gate 6 (guarded material dwords unchanged between render entry and the pass): %llu changes "
      "in %.0f material checks/frame -> %s",
      static_cast<unsigned long long>(t.matsGuardChg), t.mats / f, GateWord(o.g[5]));
  Log("  forward rec S0 gate 7 (PS t87-t127 constant within a call >= 99%% of the checked calls; needed when gate 4 "
      "fails): %.2f%% of %llu -> %s",
      in.globStablePct, static_cast<unsigned long long>(in.globCalls), GateWord(o.g[6]));
  Log("  forward rec S0: %s (R21 8)", VerdictText(o.verdict));
  delete tp;
}

// ---------------------------------------------------------------------------
// Suite phase
// ---------------------------------------------------------------------------
void Measure(int ms, std::atomic<uint64_t>& frames, double tscHz, const std::atomic<bool>& abort) {
  if (g_chained.load()) {
    Log("  forward rec counter: already running");
    return;
  }
  g_tscHz = tscHz;
  auto* ng = reinterpret_cast<uint8_t*>(GetModuleHandleW(L"NGModel.dll"));
  auto* dx = reinterpret_cast<uint8_t*>(GetModuleHandleW(L"dx11backend.dll"));
  g_md = reinterpret_cast<uint8_t*>(GetModuleHandleW(L"ModelDesc.dll"));
  if (!ng || !dx || !tscHz) {
    Log("  forward rec counter: NGModel/dx11backend not loaded or no TSC rate; skipped");
    return;
  }
  if (!gbcount::Install()) {
    Log("  forward rec counter: SceneRenderable hook unavailable (this build); skipped");
    return;
  }
  g_srVt = ng + gbcount::SrVtblRva(ng);
  g_modelMatVt = gbcount::g_modelMatVtbl;
  g_texOk = shadowtex::VerifyBuild(ng, dx) == nullptr;
  g_shaderVt = dx + shadowtex::g_at.shader;
  if (!allocslab::RttiIs(dx, static_cast<void**>(g_shaderVt), ".?AVDX11Shader@RenderAPI@@")) {
    Log("  forward rec counter: DX11Shader vtable not found: blend modes unknown, no census");
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
    Log("  forward rec counter: texture classes of this build not verified: no view prediction");
  }
  g_ratioOk = false;
  if (!shadowbatch::InstallRenderer()) {
    Log("  forward rec counter: DX11Renderer not matched: +0xd4/+0x2120 and ratios not read");
  } else {
    auto** rv = shadowbatch::g_rendererVtbl;
    g_ratioOk = reinterpret_cast<uint8_t*>(SlotOriginal(&rv[54])) == dx + 0x15d60 &&
                reinterpret_cast<uint8_t*>(SlotOriginal(&rv[40])) == dx + 0x17010 &&
                shadowtex::CodeIs(dx, gbbatch::kDxCode[2].begin, gbbatch::kDxCode[2].end, gbbatch::kDxCode[2].hash) &&
                shadowtex::CodeIs(dx, gbbatch::kDxCode[3].begin, gbbatch::kDxCode[3].end, gbbatch::kDxCode[3].hash);
  }
  ID3D11Device* dev = shadowinst::g_device;
  if (dev) {
    dev->AddRef();
  } else if (sfilt::Ctx* c = sfilt::g_ctx.load()) {
    c->GetDevice(&dev);
  }
  if (dev) {
    dev->GetImmediateContext(&g_ctx);
    dev->Release();
  } else {
    Log("  forward rec counter: DCS's device not known yet (it comes from [Model] ShadowInstancing=1 or [D3D] "
        "SplitFilter=1): no setup probes");
  }
  g_census = g_shaderVt && shadowinst::InstallGb();
  if (!g_census) Log("  forward rec counter: shadow_inst's model keys unavailable (needs [Model] ShadowInstancing=1): no census");
  if (g_census) shadowinst::g_collectGb = true;
  if (!g_mats) {
    g_mats = static_cast<MatEntry*>(VirtualAlloc(nullptr, kMatTable * sizeof(MatEntry), MEM_COMMIT | MEM_RESERVE,
                                                 PAGE_READWRITE));
    if (!g_mats) Log("  forward rec counter: no memory for the material table: material checks off");
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
  DWORD rt = shadowbatch::g_renderThread;
  if (!rt) rt = ptiming::g_topTid.load();
  g_renderTid = rt;
  g_ownRenderHook = false;
  if (!shadowbatch::g_renderSlot && !shadowbatch::g_renderSlot2) g_ownRenderHook = shadowbatch::InstallRenderHook();
  if (!shadowbatch::g_renderSlot && !shadowbatch::g_renderSlot2)
    Log("  forward rec counter: RenderGraph::render entry not hooked: no material copies, no entry offsets");
  g_ownSort = false;
  g_sortObserved = false;
  if (shrec::g_sortObserver.load() == nullptr) {
    shrec::g_sortBorrowed = true;
    if (shrec::g_sortPatched) {
      g_sortObserved = true;
    } else if (!g_cfg.shadowRecorder && shrec::g_state.load() != 1) {
      const bool ok = shrec::g_sortStub ? shrec::PatchSortSites(true) : shrec::InstallSortHook();
      g_ownSort = ok && shrec::g_sortPatched;
      g_sortObserved = g_ownSort;
    }
    if (!g_sortObserved) shrec::g_sortBorrowed = false;
  }
  if (!g_sortObserved) Log("  forward rec counter: Scene sort hook not available: when the vectors become final is not measured");
  g_learnedCount = 0;
  memset(g_learned, 0, sizeof(g_learned));
  ResetStats();
  if (!HookPass()) {
    ReleaseSort();
    if (g_ownRenderHook && shadowbatch::g_state.load() != 1) {
      if (shadowbatch::g_renderSlot) UnhookSlot(shadowbatch::g_renderSlot, reinterpret_cast<void*>(shadowbatch::g_origRender));
      if (shadowbatch::g_renderSlot2) UnhookSlot(shadowbatch::g_renderSlot2, reinterpret_cast<void*>(shadowbatch::g_origRender));
      shadowbatch::g_renderSlot = nullptr;
      shadowbatch::g_renderSlot2 = nullptr;
    }
    g_ownRenderHook = false;
    if (g_census) shadowinst::g_collectGb = shadowinst::g_onGb.load();
    Drain();
    return;
  }
  g_prevOverride = gbcount::g_override.load();
  g_prevObserver = shadowbatch::g_renderObserver.load();
  shadowbatch::g_renderObserver = &OnRender;
  gbcount::g_override = &Override;
  if (g_sortObserved) shrec::g_sortObserver = &OnSorted;
  g_chained = true;
  // Discovery: learn the calls and the classes; the keys are collected.
  g_on = true;
  Sleep(1000);
  g_on = false;
  Sleep(100);
  for (int i = 0; i < 400 && g_inside.load() != 0; ++i) Sleep(5);
  if (g_census) {
    const double waitS = shadowinst::WaitCompiles(abort);
    const shadowinst::Totals k = shadowinst::Snap(shadowinst::kKindGb);
    Log("  forward rec counter: waited %.1f s for the model keys (%u keys: %u OK, %u failed, %u not finished)", waitS,
        k.keys, k.ok, k.done - k.ok, k.keys - k.done);
  }
  if (abort.load()) {
    Unchain();
    Drain();
    if (g_census) shadowinst::g_collectGb = shadowinst::g_onGb.load();
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
  int foreign = 0;
  for (int i = 0; i < g_nsCount.load(); ++i) foreign += g_ns[i].state == -1;
  if (f <= 0 || !drained) {
    Log("  forward rec counter: no frames counted%s", drained ? "" : " (a pass was still running)");
    return;
  }
  Report(f);
  if (foreign) Log("  forward rec S0: %d renderable classes untimed (vt[1] hooked by another module)", foreign);
  for (size_t i = 0; g_cen && i < kCensusTable; ++i) {
    g_cen[i].other.clear();
    g_cen[i].globals.clear();
    g_cen[i].why.clear();
  }
}

}  // namespace fwdreccount
