// Far shadow cascade cache, PROTOTYPE ([Shadows] CascadeCache, default 0 =
// off; R33). The coarse cascades (CascadeShadowPass2: about 70-370 m,
// CascadeShadowPass3: about 370-1500 m) keep their static casters (terrain,
// forest, buildings: the GPU-driven edterrainGraphics41 items) in a cached
// depth slice that is re-rendered only every few frames or when the light,
// the window or the scene changes; the dynamic casters (every NGModel caster:
// aircraft, vehicles, ships, parked objects) are drawn on top every frame.
// With the key at 0 nothing is installed.
//
// Mechanism [V GraphicsCore.dll 2.9.30, static; M census 2026-10-11 03:13; R33 §2]:
// - enlight::Shadows::parseShadowMap (GC 0x6c3a0) fits each cascade's
//   light-space box (impl+0x70+0x38*i: lo[3], hi[3]; the texel quantum at
//   +0x30/+0x34), snaps it, builds the ortho projection P_i and writes
//   ShadowMatrix[i] = V*P_i*B (impl+0x244+64*i, row-major; transposed and
//   inverted in place after the loop), then calls
//   ShadowMapModule::parseShadowMap(i - first, V, P_i) through the
//   SceneRender vtable (GC 0xd98d0 slot 1 = 0xa8180). That call builds the
//   cascade's culling volume and the SceneRenderContext the pass copies
//   (0x588 bytes at pass+0x60+0x10; the proj is stored verbatim, GC 0x93a05).
//   The per-view ShadowParams copies (lighting, SSLR, water, transparents)
//   are taken after the loop (GC 0xa4c60).
// - So one hook on that vtable slot sees V and DCS's fresh P_i, can hand
//   DCS another P' (DCS then culls, binds b7 and renders with P') and can
//   write ShadowMatrix[i] = V*P'*B before DCS transposes it: the pass and
//   every receiver use the same P'. [This hook: ParseHook.]
// - The pass execute (shadow_pass.h, GC 0xa5500) clears the slice, binds the
//   context and calls item->vt[1](item, data+0x10) for each caster of the
//   vector. Our innermost stand-in (shadowpass::g_inner) swaps the vector for
//   the call: refresh frames [static..., snap, dynamic...] (snap copies the
//   static-only slice into our cache texture), reuse frames [restore,
//   dynamic...] (restore copies the cache back into the slice). Depth-only
//   GREATER with writes and no blending: the order inside a cascade does not
//   change the result (R17 §4), so a refresh frame is texel-identical to
//   DCS's pass with the same P', and a reuse frame differs only where a
//   static caster changed since the capture (tree wind, LOD switches).
//
// Window: on a refresh, P' covers DCS's fresh box expanded by MarginPct of
// its extent on each side (xy) and ZMarginPct in z, so the held window keeps
// covering the cascade's slice for a few frames of motion. A reuse needs:
// the cache captured with this exact P' (generation), the same light view V
// (DCS updates the sun direction only past 0.028 deg, GC 0x686fa), the same
// map size, DCS's fresh unsnapped box of this frame inside the held window
// (xyz), no camera-anchor jump (origin shift, teleport, view change), no
// mission event, no render gap, the held texel no coarser than DCS's fresh
// one by more than the margin plus ScaleTolPct, and age < MaxAge frames
// (the later cascade defers its age refresh by one frame when another one
// refreshed this frame, so the refresh cost is spread).
//
// Safety: unknown item classes are drawn every frame (dynamic); a pass whose
// stored proj is not one of our recent plans, a missing cache or a failed
// copy draws every caster (DCS's list) with that pass's own P', which is the
// P' its ShadowMatrix holds. A fault while reading DCS memory latches the
// feature off. Included once from main.cpp after shadow_pass.h,
// gpu_pass_timing.h and periph_lod.h.
#pragma once

namespace cascache {

// ---------------------------------------------------------------------------
// DCS layout [V GraphicsCore.dll 2.9.30]
// ---------------------------------------------------------------------------
constexpr uint32_t kVtblRva = 0xd98d0;   // ??_7ShadowMapModule@render@@6BSceneRender@Shadows@enlight@@@
constexpr int kParseSlot = 1;
constexpr uint32_t kParseRva = 0xa8180;  // ShadowMapModule::parseShadowMap(int, Matrixf const&, Matrixf const&)
constexpr uint8_t kParseBytes[16] = {0x48, 0x89, 0x5c, 0x24, 0x20, 0x55, 0x56, 0x57,
                                     0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57};
constexpr uint32_t kModCount = 0x1a74;   // cascade count (data index = count - 1 - idx, GC 0xa8748)
constexpr uint32_t kModShadows = 0x1ab0; // enlight::Shadows; its impl pointer at +0
constexpr uint32_t kImplSize = 0x30, kImplBox = 0x70, kBoxStride = 0x38, kImplView = 0x150, kImplAnchor = 0x20c,
                   kImplFirst = 0x1d4, kImplSM = 0x244;
constexpr uint32_t kPassData = 0x60, kDataCtx = 0x10, kCtxBytes = 0x588;
constexpr uint32_t kSmrVtblRva = 0x59828;  // NGModel ShadowMapRenderable (R17 2)

constexpr int kMax = 8;  // pass data indices handled

// ---------------------------------------------------------------------------
// Pure math (unit-tested)
// ---------------------------------------------------------------------------
struct Box {
  double lo[3], hi[3];
};

// DCS's cascade projection [V GC 0x6c5b1-0x6c69c]: row-major, row vectors,
// x' = sx x + tx, y' = sy y + ty (sy < 0), z' = sz z + tz (sz < 0: hi.z -> 0,
// lo.z -> 1). False when P is not of that form.
bool BoxFromOrtho(const float* P, Box* b) {
  for (int i : {1, 2, 3, 4, 6, 7, 8, 9, 11})
    if (P[i] != 0.0f) return false;
  if (P[15] != 1.0f || P[0] == 0.0f || P[5] == 0.0f || P[10] == 0.0f) return false;
  const double s[3] = {P[0], P[5], P[10]}, t[3] = {P[12], P[13], P[14]};
  const double ndc0[3] = {-1, -1, 0}, ndc1[3] = {1, 1, 1};
  for (int a = 0; a < 3; ++a) {
    const double u = (ndc0[a] - t[a]) / s[a], v = (ndc1[a] - t[a]) / s[a];
    b->lo[a] = (std::min)(u, v);
    b->hi[a] = (std::max)(u, v);
    if (!(b->hi[a] > b->lo[a]) || !std::isfinite(b->lo[a]) || !std::isfinite(b->hi[a])) return false;
  }
  return true;
}

// The same formulas as DCS, in float.
void OrthoFromBox(const Box& b, float* P) {
  const float lx = static_cast<float>(b.lo[0]), hx = static_cast<float>(b.hi[0]);
  const float ly = static_cast<float>(b.lo[1]), hy = static_cast<float>(b.hi[1]);
  const float lz = static_cast<float>(b.lo[2]), hz = static_cast<float>(b.hi[2]);
  const float sx = 2.0f / (hx - lx), sy = -2.0f / (hy - ly), sz = 1.0f / (lz - hz);
  for (int i = 0; i < 16; ++i) P[i] = 0.0f;
  P[0] = sx;
  P[5] = sy;
  P[10] = sz;
  P[12] = (lx + hx) * -0.5f * sx;
  P[13] = (hy + ly) * -0.5f * sy;
  P[14] = -hz * sz;
  P[15] = 1.0f;
}

Box Expand(const Box& b, double mxy, double mz) {
  Box r = b;
  for (int a = 0; a < 3; ++a) {
    const double m = (a < 2 ? mxy : mz) * (b.hi[a] - b.lo[a]);
    r.lo[a] -= m;
    r.hi[a] += m;
  }
  return r;
}

bool Inside(const Box& outer, const Box& inner) {
  for (int a = 0; a < 3; ++a)
    if (inner.lo[a] < outer.lo[a] || inner.hi[a] > outer.hi[a]) return false;
  return true;
}

double Extent(const Box& b, int a) { return b.hi[a] - b.lo[a]; }

// ShadowMatrix = V * P * B (row vectors; B maps NDC to texture space:
// u = 0.5 x + 0.5, v = -0.5 y + 0.5) [M census: equals the per-view
// ShadowMatrix to float precision].
void ShadowMatrixOf(const float* V, const float* P, float* out) {
  double vp[16];
  for (int r = 0; r < 4; ++r)
    for (int c = 0; c < 4; ++c) {
      double s = 0;
      for (int k = 0; k < 4; ++k) s += static_cast<double>(V[r * 4 + k]) * P[k * 4 + c];
      vp[r * 4 + c] = s;
    }
  for (int r = 0; r < 4; ++r) {
    const double x = vp[r * 4 + 0], y = vp[r * 4 + 1], z = vp[r * 4 + 2], w = vp[r * 4 + 3];
    out[r * 4 + 0] = static_cast<float>(0.5 * x + 0.5 * w);
    out[r * 4 + 1] = static_cast<float>(-0.5 * y + 0.5 * w);
    out[r * 4 + 2] = static_cast<float>(z);
    out[r * 4 + 3] = static_cast<float>(w);
  }
}

// ---------------------------------------------------------------------------
// Decision (unit-tested)
// ---------------------------------------------------------------------------
enum Mode : uint8_t { kNone = 0, kRefresh = 1, kReuse = 2 };
enum Reason : uint8_t {
  kRReuse = 0,   // reused (not a refresh reason)
  kRNoCache,     // nothing captured yet, or the capture was dropped
  kRForced,      // mission event, anchor jump, render gap, first frame
  kRLight,       // the light view V changed (DCS's sun hysteresis stepped)
  kRSize,        // the map size changed
  kRNoTight,     // DCS's unsnapped box of this frame was not seen
  kROutside,     // DCS's box of this frame leaves the held window
  kRScale,       // the held texel is too coarse for DCS's box of this frame
  kRAge,         // MaxAge reached
  kReasons
};
const char* const kReasonName[kReasons] = {"reuse", "no cache", "forced", "light moved", "map size",
                                           "no box",  "outside",  "scale",  "age"};

struct Params {
  double margin = 0.03;    // window margin per side, fraction of DCS's extent (xy)
  double zMargin = 0.05;   // the same in z
  double scaleTol = 0.15;  // held texel may be (1 + 2 margin)(1 + scaleTol) times DCS's fresh one
  uint32_t maxAge = 4;     // frames a capture may serve, the capture frame included
};

// What a capture holds (render thread writes after a snap; the parse reads).
struct Tag {
  bool valid = false;
  uint32_t gen = 0;
  uint32_t frame = 0;  // parse frame of the refresh that produced it
  int size = 0;
  float V[16] = {};
  float P[16] = {};
  Box win{};
};

struct In {
  const float* V;    // this frame's light view
  Box fresh;         // DCS's snapped box of this frame (from its proj)
  bool tightValid;   // DCS's unsnapped box of this frame was read
  Box tight;
  int size;
  uint32_t frame;
  bool forced;
  bool otherRefreshed;  // an earlier cascade refreshed in this frame
};

Mode Decide(const Tag& t, const In& in, const Params& p, Reason* why) {
  auto refresh = [&](Reason r) {
    *why = r;
    return kRefresh;
  };
  if (in.forced) return refresh(kRForced);
  if (!t.valid) return refresh(kRNoCache);
  if (memcmp(t.V, in.V, sizeof(t.V)) != 0) return refresh(kRLight);
  if (t.size != in.size) return refresh(kRSize);
  if (!in.tightValid) return refresh(kRNoTight);
  if (!Inside(t.win, in.tight)) return refresh(kROutside);
  const double k = (1.0 + 2.0 * p.margin) * (1.0 + p.scaleTol);
  for (int a = 0; a < 2; ++a)
    if (Extent(t.win, a) > Extent(in.fresh, a) * k) return refresh(kRScale);
  const uint32_t age = in.frame - t.frame;  // the capture frame is age 0
  if (age >= p.maxAge) {
    // One frame of grace when another cascade already refreshed in this frame.
    if (!(in.otherRefreshed && age == p.maxAge)) return refresh(kRAge);
  }
  *why = kRReuse;
  return kReuse;
}

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------
std::atomic<int> g_state{0};        // 0 not installed, 1 installed, -1 unavailable
std::atomic<bool> g_on{false};      // route through us (config)
std::atomic<bool> g_disabled{false};// latched off after a fault
std::atomic<uint32_t> g_mask{0xc};  // cascades by CascadeShadowPass number (bit k)
std::atomic<uint32_t> g_maxAge{4};
std::atomic<uint32_t> g_marginPm{30}, g_zMarginPm{50}, g_scaleTolPm{150};
std::atomic<uint32_t> g_verifyEvery{0};  // compare every Nth reuse against a full redraw (0 = never)
std::atomic<uint32_t> g_missionGen{0};
std::atomic<int> g_insideParse{0}, g_insideExec{0};

using ParseFn = void(__fastcall*)(void* self, int idx, const float* view, const float* proj);
ParseFn g_parseOrig = nullptr;
void** g_parseSlot = nullptr;
void* g_expectVtbl = nullptr;  // *self must be this (the SceneRender vtable); tests set it
uintptr_t g_staticLo = 0, g_staticHi = 0;  // module whose items are static (edterrainGraphics41.dll)
void* g_smrVtbl = nullptr;                 // NGModel ShadowMapRenderable (statistics only)
double g_anchorJump = 30.0;                // m: per-frame anchor change that forces a refresh
int64_t g_gapQpc = 0;                      // render gap that forces a refresh (250 ms; set at install)

struct Plan {
  Mode mode = kNone;
  bool used = true;
  uint32_t frame = 0, gen = 0;
  int size = 0;
  float V[16] = {};
  float P[16] = {};
  Box win{};
};

struct Slot {
  Tag tag;
  Plan plans[2];  // the two most recent parses
  int newest = 0;
  ID3D11Texture2D* tex = nullptr;
  D3D11_TEXTURE2D_DESC desc{};
};
std::mutex g_lock;  // g_slot (parse thread vs render thread)
Slot g_slot[kMax];
uint32_t g_genNext = 0;

// Parse-thread frame state.
struct FrameState {
  uint32_t frame = 0;
  int lastI = 1 << 20;
  bool tightValid[4] = {};
  Box tight[4]{};
  bool refreshed = false;
  bool forced = true;
  bool haveAnchor = false;
  float anchor[3] = {};
  int64_t lastQpc = 0;
  uint32_t missionSeen = 0;
};
FrameState g_fs;
// The ShadowMapModule whose cascades are held (the first one seen; another
// module takes over when this one has not parsed for a second). Parse thread.
std::atomic<void*> g_module{nullptr};
int64_t g_moduleQpc = 0;

// Counters.
struct Stats {
  std::atomic<uint64_t> parses{0}, frames{0}, planRefresh[4]{}, planReuse[4]{}, reason[4][kReasons]{}, badProj{0},
      readFault{0}, smWrite{0};
  std::atomic<uint64_t> passes{0}, noPlan{0}, noVec{0}, captured{0}, restored{0}, restoreFallback{0}, reuseMiss{0},
      snapFail{0}, staticItems{0}, dynamicItems{0}, smrItems{0}, otherItems{0}, noStaticPasses{0};
  std::atomic<uint64_t> verifyPasses{0}, verifyTexels{0}, verifyBad{0}, verifyErrors{0}, verifySkipped{0},
      otherModule{0};
  std::atomic<uint32_t> verifyMaxDiffBits{0};
};
Stats g_st;

// Item classes seen (diagnostic): vtable -> static?, count.
struct SeenClass {
  void* vt;
  bool isStatic;
  uint64_t n;
};
std::mutex g_seenLock;
std::vector<SeenClass> g_seen;

Params CurParams() {
  Params p;
  p.margin = g_marginPm.load() / 1000.0;
  p.zMargin = g_zMarginPm.load() / 1000.0;
  p.scaleTol = g_scaleTolPm.load() / 1000.0;
  p.maxAge = (std::max)(1u, g_maxAge.load());
  return p;
}

int64_t QpcNow() {
  LARGE_INTEGER q;
  QueryPerformanceCounter(&q);
  return q.QuadPart;
}

void Disable(const char* why) {
  if (!g_disabled.exchange(true)) Log("cascade cache: %s; off for this session", why);
}

// ---------------------------------------------------------------------------
// Parse hook (ShadowMapModule::parseShadowMap)
// ---------------------------------------------------------------------------
struct Raw {
  uint8_t* impl;
  int first, count, size;
  float anchor[3];
  float V[16], P[16];
  bool boxValid[4];
  Box box[4];
};

// DCS memory, guarded. False on a fault or an unexpected object.
bool ReadRaw(void* self, const float* view, const float* proj, Raw* r) {
  __try {
    auto* s = static_cast<uint8_t*>(self);
    if (g_expectVtbl && *reinterpret_cast<void**>(s) != g_expectVtbl) return false;
    r->impl = *reinterpret_cast<uint8_t**>(s + kModShadows);
    if (!r->impl) return false;
    int count = *reinterpret_cast<int32_t*>(s + kModCount);
    if (count < 1 || count > kMax) count = *reinterpret_cast<uint16_t*>(s + kModCount);
    r->count = count;
    r->first = *reinterpret_cast<int32_t*>(r->impl + kImplFirst);
    r->size = *reinterpret_cast<int32_t*>(r->impl + kImplSize);
    memcpy(r->anchor, r->impl + kImplAnchor, sizeof(r->anchor));
    memcpy(r->V, view, sizeof(r->V));
    memcpy(r->P, proj, sizeof(r->P));
    for (int j = 0; j < 4; ++j) {
      const float* f = reinterpret_cast<const float*>(r->impl + kImplBox + kBoxStride * j);
      bool ok = true;
      for (int a = 0; a < 3; ++a) {
        r->box[j].lo[a] = f[a];
        r->box[j].hi[a] = f[3 + a];
        ok = ok && f[3 + a] >= f[a] && std::isfinite(f[a]) && std::isfinite(f[3 + a]);
      }
      r->boxValid[j] = ok;
    }
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

bool WriteSM(uint8_t* impl, int i, const float* sm) {
  __try {
    memcpy(impl + kImplSM + 64 * i, sm, 64);
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

// The plan for one cascade; returns the proj to hand DCS (proj or `sub`).
const float* PlanCascade(void* self, int idx, const float* view, const float* proj, float* sub) {
  Raw r;
  if (!ReadRaw(self, view, proj, &r)) {
    g_st.readFault++;
    Disable("the cascade setup could not be read (unexpected object)");
    return proj;
  }
  g_st.parses++;
  {
    const int64_t now = QpcNow();
    void* held = g_module.load();
    if (self != held) {
      LARGE_INTEGER f;
      QueryPerformanceFrequency(&f);
      if (held && now - g_moduleQpc < f.QuadPart) {
        g_st.otherModule++;  // a second cascade set (another render graph): DCS's own
        return proj;
      }
      g_module = self;
      g_fs = FrameState{};
    }
    g_moduleQpc = now;
  }
  const int i = idx + r.first;
  const int di = r.count - 1 - idx;
  if (i < 0 || i >= 4 || di < 0 || di >= kMax) return proj;
  FrameState& fs = g_fs;
  // A new frame: the cascade index did not grow.
  if (i <= fs.lastI) {
    fs.frame++;
    g_st.frames++;
    fs.refreshed = false;
    for (bool& v : fs.tightValid) v = false;
    const int64_t now = QpcNow();
    bool forced = fs.lastQpc == 0 || (g_gapQpc && now - fs.lastQpc > g_gapQpc);
    fs.lastQpc = now;
    const uint32_t mg = g_missionGen.load();
    if (mg != fs.missionSeen) forced = true;
    fs.missionSeen = mg;
    if (fs.haveAnchor)
      for (int a = 0; a < 3; ++a)
        if (!(std::fabs(r.anchor[a] - fs.anchor[a]) <= g_anchorJump)) forced = true;
    memcpy(fs.anchor, r.anchor, sizeof(fs.anchor));
    fs.haveAnchor = true;
    fs.forced = forced;
  }
  fs.lastI = i;
  // Boxes not yet snapped in this frame: every cascade after this one.
  for (int j = i + 1; j < 4; ++j) {
    fs.tightValid[j] = r.boxValid[j];
    fs.tight[j] = r.box[j];
  }
  Slot& sl = g_slot[di];
  if (!(g_mask.load() & (1u << i))) {
    std::lock_guard<std::mutex> lock(g_lock);
    sl.plans[0].mode = sl.plans[1].mode = kNone;
    return proj;
  }
  Box fresh;
  if (!BoxFromOrtho(r.P, &fresh)) {
    g_st.badProj++;
    return proj;
  }
  const Params p = CurParams();
  In in{r.V, fresh, fs.tightValid[i], fs.tight[i], r.size, fs.frame, fs.forced, fs.refreshed};
  Plan plan;
  Reason why;
  {
    std::lock_guard<std::mutex> lock(g_lock);
    const Mode m = Decide(sl.tag, in, p, &why);
    plan.mode = m;
    plan.used = false;
    plan.frame = fs.frame;
    plan.size = r.size;
    memcpy(plan.V, r.V, sizeof(plan.V));
    if (m == kReuse) {
      plan.gen = sl.tag.gen;
      memcpy(plan.P, sl.tag.P, sizeof(plan.P));
      plan.win = sl.tag.win;
    } else {
      plan.gen = ++g_genNext;
      plan.win = Expand(fresh, p.margin, p.zMargin);
      OrthoFromBox(plan.win, plan.P);
      fs.refreshed = true;
    }
    sl.newest ^= 1;
    sl.plans[sl.newest] = plan;
  }
  g_st.reason[i][why]++;
  (plan.mode == kReuse ? g_st.planReuse : g_st.planRefresh)[i]++;
  float sm[16];
  ShadowMatrixOf(r.V, plan.P, sm);
  if (!WriteSM(r.impl, i, sm)) {
    g_st.readFault++;
    Disable("ShadowMatrix could not be written");
    // DCS's own proj and matrix stay: both consistent.
    std::lock_guard<std::mutex> lock(g_lock);
    sl.plans[sl.newest].mode = kNone;
    return proj;
  }
  g_st.smWrite++;
  memcpy(sub, plan.P, sizeof(plan.P));
  return sub;
}

void __fastcall ParseHook(void* self, int idx, const float* view, const float* proj) {
  g_insideParse.fetch_add(1);
  const float* use = proj;
  float sub[16];
  if (g_on.load(std::memory_order_relaxed) && !g_disabled.load(std::memory_order_relaxed) && !qvexit::Exiting())
    use = PlanCascade(self, idx, view, proj, sub);
  g_parseOrig(self, idx, view, use);
  g_insideParse.fetch_sub(1);
}

// ---------------------------------------------------------------------------
// Pass (innermost execute)
// ---------------------------------------------------------------------------
std::atomic<int> g_projOff{-1};  // where the pass context stores the proj (learned)

int DataIdx(void* pass) {
  __try {
    return *reinterpret_cast<int32_t*>(static_cast<uint8_t*>(pass) + kPassData);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return -1;
  }
}

// True when the pass's context holds P (bitwise).
bool PassHasProj(void* pass, const float* P) {
  __try {
    const uint8_t* c = static_cast<uint8_t*>(pass) + kPassData + kDataCtx;
    const int off = g_projOff.load(std::memory_order_relaxed);
    if (off >= 0) return memcmp(c + off, P, 64) == 0;
    for (int o = 0; o + 64 <= static_cast<int>(kCtxBytes); o += 4)
      if (memcmp(c + o, P, 64) == 0) {
        g_projOff = o;
        Log("cascade cache: the pass context holds its proj at +0x%x", o);
        return true;
      }
    return false;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

// The object DCS's loop calls for our steps: vt[1](self, data).
struct Step {
  void** vt;
};
void __fastcall StepNoop(void*, void*) {}
void __fastcall SnapStep(void*, void*);
void __fastcall RestoreStep(void*, void*);
void* g_snapVt[8] = {};
void* g_restoreVt[8] = {};
Step g_snapItem{g_snapVt};
Step g_restoreItem{g_restoreVt};

void InitSteps() {
  for (int i = 0; i < 8; ++i) g_snapVt[i] = g_restoreVt[i] = reinterpret_cast<void*>(&StepNoop);
  g_snapVt[1] = reinterpret_cast<void*>(&SnapStep);
  g_restoreVt[1] = reinterpret_cast<void*>(&RestoreStep);
}

// The current pass (render thread, one at a time).
struct Cur {
  int di = -1;
  Plan plan;
  void* data = nullptr;
  std::vector<void*> statics;  // drawn by RestoreStep itself when the restore fails
  bool captured = false, restored = false;
  ID3D11Texture2D* arr = nullptr;  // the slice written (AddRef'd while set)
  UINT sub = 0;
};
Cur g_cur;
std::vector<void*> g_list;
ID3D11DeviceContext* g_ctx = nullptr;

enum ItemClass { kStatic, kDynamic };
bool VtableOf(void* item, void** vt) {
  __try {
    *vt = *reinterpret_cast<void**>(item);
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

void NoteClass(void* vt, bool isStatic) {
  std::lock_guard<std::mutex> lock(g_seenLock);
  for (SeenClass& s : g_seen)
    if (s.vt == vt) {
      s.n++;
      return;
    }
  if (g_seen.size() < 32) g_seen.push_back({vt, isStatic, 1});
}

// R49: in DCS 2.9.30 the terrain caster of each cascade pass (terrain, buildings and forest, drawn through
// edterrainGraphics41's GPU chain) is Visualizer.dll's TerrainParser::TerrainObject (live R45: one item per pass,
// Visualizer.dll+0x20ec08), not an edterrainGraphics41 class. It is recognised by its MSVC RTTI type name (the
// complete object locator at vt[-1]), so a Visualizer rebuild that moves the vtable still matches.
const char kTerrainRtti[] = ".?AVTerrainObject@TerrainParser@@";
bool RttiNameIs(void* vt, const char* name) {
  __try {
    const uint8_t* col = reinterpret_cast<const uint8_t* const*>(vt)[-1];
    if (!col || *reinterpret_cast<const uint32_t*>(col) != 1) return false;  // x64 locator signature
    const uint32_t tdRva = *reinterpret_cast<const uint32_t*>(col + 12);
    const uint32_t selfRva = *reinterpret_cast<const uint32_t*>(col + 20);
    const uint8_t* base = col - selfRva;
    const char* n = reinterpret_cast<const char*>(base + tdRva + 16);
    for (size_t i = 0;; ++i) {
      if (n[i] != name[i]) return false;
      if (!name[i]) return true;
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}
// Classes already looked up (render thread; a few per pass).
struct RttiMemo {
  void* vt;
  bool terrain;
};
RttiMemo g_rttiMemo[16];
int g_rttiMemoN = 0;
bool IsTerrainObject(void* vt) {
  for (int i = 0; i < g_rttiMemoN; ++i)
    if (g_rttiMemo[i].vt == vt) return g_rttiMemo[i].terrain;
  const bool t = RttiNameIs(vt, kTerrainRtti);
  if (g_rttiMemoN < 16) g_rttiMemo[g_rttiMemoN++] = {vt, t};
  return t;
}

ItemClass Classify(void* item) {
  void* vt = nullptr;
  if (!item || !VtableOf(item, &vt)) return kDynamic;
  const uintptr_t v = reinterpret_cast<uintptr_t>(vt);
  if (vt == g_smrVtbl) {
    g_st.smrItems++;
    return kDynamic;
  }
  const bool st = (g_staticLo && v >= g_staticLo && v < g_staticHi) || IsTerrainObject(vt);
  if (!st) g_st.otherItems++;
  NoteClass(vt, st);
  return st ? kStatic : kDynamic;
}

// Our list for the pass: refresh [static..., snap, dynamic...]; reuse
// [restore, dynamic...] with the statics kept aside for the fallback.
// Returns the number of static items.
size_t BuildList(void* const* b, void* const* e, Mode m, std::vector<void*>& out, std::vector<void*>& statics) {
  out.clear();
  statics.clear();
  std::vector<void*> dyn;
  dyn.reserve(static_cast<size_t>(e - b));
  for (void* const* p = b; p < e; ++p) {
    if (Classify(*p) == kStatic)
      statics.push_back(*p);
    else
      dyn.push_back(*p);
  }
  if (m == kRefresh) {
    out.insert(out.end(), statics.begin(), statics.end());
    out.push_back(&g_snapItem);
  } else {
    out.push_back(&g_restoreItem);
  }
  out.insert(out.end(), dyn.begin(), dyn.end());
  g_st.staticItems += statics.size();
  g_st.dynamicItems += dyn.size();
  return statics.size();
}

// The depth slice bound right now (DCS's framebuffer of the cascade).
struct Bound {
  ID3D11DepthStencilView* dsv = nullptr;
  ID3D11RenderTargetView* rtv[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
  D3D11_VIEWPORT vp[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE] = {};
  UINT nvp = 0;
  ID3D11Texture2D* tex = nullptr;
  UINT sub = 0;
  D3D11_TEXTURE2D_DESC desc{};
  void Release() {
    if (dsv) dsv->Release();
    for (auto*& r : rtv)
      if (r) r->Release(), r = nullptr;
    if (tex) tex->Release();
    dsv = nullptr;
    tex = nullptr;
  }
};

bool GetBound(ID3D11DeviceContext* c, Bound* b) {
  c->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, b->rtv, &b->dsv);
  b->nvp = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
  c->RSGetViewports(&b->nvp, b->vp);
  if (!b->dsv) return false;
  D3D11_DEPTH_STENCIL_VIEW_DESC vd;
  b->dsv->GetDesc(&vd);
  ID3D11Resource* res = nullptr;
  b->dsv->GetResource(&res);
  if (!res) return false;
  if (FAILED(res->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&b->tex)))) b->tex = nullptr;
  res->Release();
  if (!b->tex) return false;
  b->tex->GetDesc(&b->desc);
  if (b->desc.SampleDesc.Count != 1) return false;
  switch (vd.ViewDimension) {
    case D3D11_DSV_DIMENSION_TEXTURE2D:
      b->sub = D3D11CalcSubresource(vd.Texture2D.MipSlice, 0, b->desc.MipLevels);
      return true;
    case D3D11_DSV_DIMENSION_TEXTURE2DARRAY:
      if (vd.Texture2DArray.ArraySize != 1) return false;
      b->sub = D3D11CalcSubresource(vd.Texture2DArray.MipSlice, vd.Texture2DArray.FirstArraySlice, b->desc.MipLevels);
      return true;
    default:
      return false;
  }
}

// Copies with the depth target unbound, then binds exactly what was bound.
void CopyUnbound(ID3D11DeviceContext* c, Bound& b, ID3D11Resource* dst, UINT dstSub, ID3D11Resource* src, UINT srcSub) {
  UINT nrtv = 0;
  for (UINT i = 0; i < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i)
    if (b.rtv[i]) nrtv = i + 1;
  c->OMSetRenderTargets(0, nullptr, nullptr);
  c->CopySubresourceRegion(dst, dstSub, 0, 0, 0, src, srcSub, nullptr);
  c->OMSetRenderTargets(nrtv, nrtv ? b.rtv : nullptr, b.dsv);
  if (b.nvp) c->RSSetViewports(b.nvp, b.vp);
}

// The slot's cache texture for this subresource's size and format.
bool EnsureTex(Slot& sl, const Bound& b) {
  const UINT mip = b.sub % b.desc.MipLevels;
  const UINT w = (std::max)(1u, b.desc.Width >> mip), h = (std::max)(1u, b.desc.Height >> mip);
  if (sl.tex && sl.desc.Width == w && sl.desc.Height == h && sl.desc.Format == b.desc.Format) return true;
  if (sl.tex) sl.tex->Release();
  sl.tex = nullptr;
  D3D11_TEXTURE2D_DESC d = b.desc;
  d.Width = w;
  d.Height = h;
  d.MipLevels = 1;
  d.ArraySize = 1;
  d.Usage = D3D11_USAGE_DEFAULT;
  d.CPUAccessFlags = 0;
  d.MiscFlags = 0;
  d.BindFlags &= (D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE);
  ID3D11Device* dev = nullptr;
  b.tex->GetDevice(&dev);
  if (!dev) return false;
  HRESULT hr = dev->CreateTexture2D(&d, nullptr, &sl.tex);
  if (FAILED(hr) || !sl.tex) {  // a format the copy target cannot be bound with: no bind flags
    d.BindFlags = 0;
    hr = dev->CreateTexture2D(&d, nullptr, &sl.tex);
  }
  dev->Release();
  if (FAILED(hr) || !sl.tex) {
    sl.tex = nullptr;
    return false;
  }
  sl.desc = d;
  return true;
}

void KeepTarget(const Bound& b) {
  if (g_cur.arr) return;
  g_cur.arr = b.tex;
  g_cur.arr->AddRef();
  g_cur.sub = b.sub;
}

void __fastcall SnapStep(void*, void*) {
  if (!g_ctx) return;
  Bound b;
  if (!GetBound(g_ctx, &b)) {
    b.Release();
    g_st.snapFail++;
    return;
  }
  KeepTarget(b);
  Slot& sl = g_slot[g_cur.di];
  if (EnsureTex(sl, b)) {
    CopyUnbound(g_ctx, b, sl.tex, 0, b.tex, b.sub);
    g_cur.captured = true;
  } else {
    g_st.snapFail++;
  }
  b.Release();
}

void __fastcall RestoreStep(void*, void* data) {
  bool ok = false;
  Bound b;
  if (g_ctx && GetBound(g_ctx, &b)) {
    KeepTarget(b);
    Slot& sl = g_slot[g_cur.di];
    const UINT mip = b.sub % b.desc.MipLevels;
    if (sl.tex && sl.desc.Format == b.desc.Format && sl.desc.Width == (std::max)(1u, b.desc.Width >> mip) &&
        sl.desc.Height == (std::max)(1u, b.desc.Height >> mip)) {
      CopyUnbound(g_ctx, b, b.tex, b.sub, sl.tex, 0);
      ok = true;
    }
  }
  b.Release();
  if (ok) {
    g_cur.restored = true;
    return;
  }
  // No cache to put back: DCS's static casters drawn right here (order-free).
  g_st.restoreFallback++;
  using ItemFn = void(__fastcall*)(void*, void*);
  for (void* it : g_cur.statics) (*reinterpret_cast<ItemFn**>(it))[1](it, data);
}

// Staging copy of one subresource (verify).
ID3D11Texture2D* ToStaging(ID3D11Texture2D* src, UINT sub) {
  D3D11_TEXTURE2D_DESC d;
  src->GetDesc(&d);
  const UINT mip = sub % d.MipLevels;
  d.Width = (std::max)(1u, d.Width >> mip);
  d.Height = (std::max)(1u, d.Height >> mip);
  d.MipLevels = d.ArraySize = 1;
  d.Usage = D3D11_USAGE_STAGING;
  d.BindFlags = 0;
  d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  d.MiscFlags = 0;
  ID3D11Device* dev = nullptr;
  src->GetDevice(&dev);
  ID3D11Texture2D* st = nullptr;
  if (dev) {
    dev->CreateTexture2D(&d, nullptr, &st);
    dev->Release();
  }
  if (st) g_ctx->CopySubresourceRegion(st, 0, 0, 0, 0, src, sub, nullptr);
  return st;
}

// Texels that differ between two depth staging copies (-1: error); the
// largest absolute difference in depth units (0..1) in *maxDiff. DCS's
// cascade array is R16_TYPELESS (D16_UNORM) [M gdd_res.csv resource 162:
// 4096 x 4096 x 4, format 53, bind DS | SRV]; 32-bit float is accepted too.
int64_t CompareDepth(ID3D11DeviceContext* c, ID3D11Texture2D* a, ID3D11Texture2D* b, uint64_t* texels, float* maxDiff) {
  D3D11_TEXTURE2D_DESC d;
  a->GetDesc(&d);
  int bpt = 0;
  if (d.Format == DXGI_FORMAT_R32_TYPELESS || d.Format == DXGI_FORMAT_D32_FLOAT || d.Format == DXGI_FORMAT_R32_FLOAT)
    bpt = 4;
  else if (d.Format == DXGI_FORMAT_R16_TYPELESS || d.Format == DXGI_FORMAT_D16_UNORM || d.Format == DXGI_FORMAT_R16_UNORM)
    bpt = 2;
  if (!bpt) return -1;
  D3D11_MAPPED_SUBRESOURCE ma, mb;
  if (FAILED(c->Map(a, 0, D3D11_MAP_READ, 0, &ma))) return -1;
  if (FAILED(c->Map(b, 0, D3D11_MAP_READ, 0, &mb))) {
    c->Unmap(a, 0);
    return -1;
  }
  int64_t bad = 0;
  for (UINT y = 0; y < d.Height; ++y) {
    const uint8_t* ra = static_cast<const uint8_t*>(ma.pData) + size_t(y) * ma.RowPitch;
    const uint8_t* rb = static_cast<const uint8_t*>(mb.pData) + size_t(y) * mb.RowPitch;
    if (memcmp(ra, rb, size_t(d.Width) * bpt) == 0) continue;
    for (UINT x = 0; x < d.Width; ++x) {
      if (memcmp(ra + size_t(x) * bpt, rb + size_t(x) * bpt, bpt) == 0) continue;
      ++bad;
      float va, vb;
      if (bpt == 4) {
        memcpy(&va, ra + size_t(x) * 4, 4);
        memcpy(&vb, rb + size_t(x) * 4, 4);
      } else {
        uint16_t ua, ub;
        memcpy(&ua, ra + size_t(x) * 2, 2);
        memcpy(&ub, rb + size_t(x) * 2, 2);
        va = ua / 65535.0f;
        vb = ub / 65535.0f;
      }
      *maxDiff = (std::max)(*maxDiff, std::fabs(va - vb));
    }
  }
  *texels += uint64_t(d.Width) * d.Height;
  c->Unmap(b, 0);
  c->Unmap(a, 0);
  return bad;
}

bool SwapCall(void** vec, void* pass, void* ctx) {
  void* b0 = vec[0];
  void* e0 = vec[1];
  bool swapped = false;
  __try {
    vec[0] = g_list.data();
    vec[1] = g_list.data() + g_list.size();
    swapped = true;
    shadowpass::g_orig(pass, ctx);
  } __finally {
    if (swapped) {
      vec[0] = b0;
      vec[1] = e0;
    }
  }
  return true;
}

bool ReadVec(void** vec, void*** b, void*** e) {
  __try {
    *b = static_cast<void**>(vec[0]);
    *e = static_cast<void**>(vec[1]);
    return *b && *e >= *b && *e - *b < 1000000;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

void ExecInner(void* pass, void* ctx) {
  const int di = DataIdx(pass);
  if (di < 0 || di >= kMax) return shadowpass::g_orig(pass, ctx);
  // The plan this pass was parsed with: the oldest unused one whose P' it holds.
  Plan plan;
  int pick = -1;
  bool any = false;
  {
    std::lock_guard<std::mutex> lock(g_lock);
    Slot& sl = g_slot[di];
    for (int k = 0; k < 2; ++k) {
      const int n = sl.newest ^ 1 ^ k;  // older first
      const Plan& p = sl.plans[n];
      if (p.mode == kNone || p.used) continue;
      any = true;
      if (PassHasProj(pass, p.P)) {
        pick = n;
        break;
      }
    }
    if (pick >= 0) {
      plan = sl.plans[pick];
      sl.plans[pick].used = true;
      if (plan.mode == kReuse && !(sl.tag.valid && sl.tag.gen == plan.gen && sl.tex)) plan.mode = kNone;
    }
  }
  if (pick < 0) {  // not a cached cascade, or parsed with something else: DCS's pass as is
    if (any) g_st.noPlan++;
    return shadowpass::g_orig(pass, ctx);
  }
  if (plan.mode == kNone) {  // a reuse whose capture is gone: every caster, this pass's P'
    g_st.reuseMiss++;
    return shadowpass::g_orig(pass, ctx);
  }
  void** vec = shadowpass::CasterVector(pass, ctx);
  void **b = nullptr, **e = nullptr;
  if (!vec || !ReadVec(vec, &b, &e)) {
    g_st.noVec++;
    return shadowpass::g_orig(pass, ctx);
  }
  if (!g_ctx) {
    ID3D11Device* dev = shadowinst::g_device;
    if (dev) {
      dev->GetImmediateContext(&g_ctx);
    } else if (sfilt::Ctx* c = sfilt::g_ctx.load()) {
      c->GetDevice(&dev);
      if (dev) {
        dev->GetImmediateContext(&g_ctx);
        dev->Release();
      }
    }
    if (!g_ctx) return shadowpass::g_orig(pass, ctx);
  }
  g_st.passes++;
  g_cur.di = di;
  g_cur.plan = plan;
  g_cur.data = static_cast<uint8_t*>(pass) + kPassData + kDataCtx;
  g_cur.captured = g_cur.restored = false;
  const size_t nStatic = BuildList(b, e, plan.mode, g_list, g_cur.statics);
  if (!nStatic) g_st.noStaticPasses++;
  SwapCall(vec, pass, ctx);
  if (plan.mode == kRefresh && g_cur.captured) {
    g_st.captured++;
    std::lock_guard<std::mutex> lock(g_lock);
    Tag& t = g_slot[di].tag;
    t.valid = true;
    t.gen = plan.gen;
    t.frame = plan.frame;
    t.size = plan.size;
    memcpy(t.V, plan.V, sizeof(t.V));
    memcpy(t.P, plan.P, sizeof(t.P));
    t.win = plan.win;
  }
  if (plan.mode == kReuse && g_cur.restored) {
    g_st.restored++;
    static uint32_t n = 0;
    const uint32_t every = g_verifyEvery.load(std::memory_order_relaxed);
    // The full redraw runs DCS's loop a second time: not with shadow batching
    // or the shadow recorder in the chain (their per-pass plans expect one run).
    // R49: the recorder counts only while it works (shrec::Active: [Model] ShadowRecorder on now); switched off at
    // runtime its wrap passes every pass through, so a second run is safe (R45: Ready() stayed true and every
    // verify was skipped after a runtime ShadowRecorder=0).
    const bool chained = (shadowbatch::g_on.load() && shadowbatch::g_state.load() == 1) || shrec::Active();
    if (every && chained) g_st.verifySkipped++;
    if (every && !chained && g_cur.arr && ++n % every == 0) {
      // Same frame, same P': the cached composite vs every caster drawn.
      ID3D11Texture2D* a = ToStaging(g_cur.arr, g_cur.sub);
      shadowpass::g_orig(pass, ctx);  // DCS's own list: the slice ends up stock for this frame
      ID3D11Texture2D* s = ToStaging(g_cur.arr, g_cur.sub);
      if (a && s) {
        uint64_t texels = 0;
        float md = 0;
        const int64_t bad = CompareDepth(g_ctx, a, s, &texels, &md);
        if (bad < 0) {
          g_st.verifyErrors++;
        } else {
          g_st.verifyPasses++;
          g_st.verifyTexels += texels;
          g_st.verifyBad += static_cast<uint64_t>(bad);
          uint32_t bits;
          memcpy(&bits, &md, 4);
          uint32_t cur = g_st.verifyMaxDiffBits.load();
          while (bits > cur && !g_st.verifyMaxDiffBits.compare_exchange_weak(cur, bits)) {
          }
        }
      } else {
        g_st.verifyErrors++;
      }
      if (a) a->Release();
      if (s) s->Release();
    }
  }
  if (g_cur.arr) g_cur.arr->Release();
  g_cur.arr = nullptr;
  g_cur.di = -1;
}

void __fastcall Exec(void* pass, void* ctx) {
  g_insideExec.fetch_add(1);
  if (!g_on.load(std::memory_order_relaxed) || g_disabled.load(std::memory_order_relaxed) || qvexit::Exiting())
    shadowpass::g_orig(pass, ctx);
  else
    ExecInner(pass, ctx);
  g_insideExec.fetch_sub(1);
}

// ---------------------------------------------------------------------------
// Control (config / suite thread)
// ---------------------------------------------------------------------------
void ModuleRange(const wchar_t* name, uintptr_t* lo, uintptr_t* hi) {
  *lo = *hi = 0;
  HMODULE m = GetModuleHandleW(name);
  if (!m) return;
  auto* base = reinterpret_cast<uint8_t*>(m);
  auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
  auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
  *lo = reinterpret_cast<uintptr_t>(base);
  *hi = *lo + nt->OptionalHeader.SizeOfImage;
}

bool Install() {
  if (g_state.load() != 0) return g_state.load() > 0;
  HMODULE gc = GetModuleHandleW(L"GraphicsCore.dll");
  if (!gc || !GetModuleHandleW(L"edterrainGraphics41.dll")) return false;  // retried
  auto* base = reinterpret_cast<uint8_t*>(gc);
  auto** vtbl = reinterpret_cast<void**>(base + kVtblRva);
  uint8_t pro[sizeof(kParseBytes)];
  if (SlotOriginal(&vtbl[kParseSlot]) != base + kParseRva || !allocslab::ReadBytes(base + kParseRva, pro, sizeof(pro)) ||
      memcmp(pro, kParseBytes, sizeof(pro)) != 0) {
    Log("cascade cache: GraphicsCore.dll is not the analysed build (SceneRender slot 1 / parseShadowMap); unavailable");
    g_state = -1;
    return false;
  }
  shadowpass::Install();
  if (!shadowpass::g_orig) return false;  // retried
  InitSteps();
  ModuleRange(L"edterrainGraphics41.dll", &g_staticLo, &g_staticHi);
  if (HMODULE ng = GetModuleHandleW(L"NGModel.dll")) g_smrVtbl = reinterpret_cast<uint8_t*>(ng) + kSmrVtblRva;
  LARGE_INTEGER f;
  QueryPerformanceFrequency(&f);
  g_gapQpc = f.QuadPart / 4;
  g_expectVtbl = vtbl;
  g_parseOrig = reinterpret_cast<ParseFn>(base + kParseRva);
  if (!HookSlot(&vtbl[kParseSlot], reinterpret_cast<void*>(&ParseHook), nullptr)) {
    Log("cascade cache: the SceneRender slot could not be hooked; unavailable");
    g_state = -1;
    return false;
  }
  g_parseSlot = &vtbl[kParseSlot];
  shadowpass::g_inner = &Exec;
  g_state = 1;
  Log("cascade cache: ready (GraphicsCore SceneRender slot 1 and the cascade execute; static items: "
      "Visualizer's TerrainParser::TerrainObject (by RTTI) and edterrainGraphics41.dll %p-%p)",
      reinterpret_cast<void*>(g_staticLo), reinterpret_cast<void*>(g_staticHi));
  return true;
}

void ResetCaches() {
  g_module = nullptr;  // the next parse adopts its module again
  std::lock_guard<std::mutex> lock(g_lock);
  for (Slot& s : g_slot) {
    s.tag = Tag{};
    s.plans[0] = s.plans[1] = Plan{};
  }
}

void ReleaseTextures() {
  std::lock_guard<std::mutex> lock(g_lock);
  for (Slot& s : g_slot) {
    if (s.tex) s.tex->Release();
    s.tex = nullptr;
    s.desc = {};
    s.tag = Tag{};
  }
}

bool Quiet(int ms) {
  for (int t = 0; t < ms; t += 5) {
    if (g_insideParse.load() == 0 && g_insideExec.load() == 0) return true;
    Sleep(5);
  }
  return false;
}

// Config: on with the given parameters (installs on first use; false = not
// now, retried), or off (pass-through; the textures go once nothing runs).
bool Apply(bool on, uint32_t mask, uint32_t maxAge, uint32_t marginPct, uint32_t verifyEvery) {
  g_mask = mask & 0xf;
  g_maxAge = (std::max)(1u, (std::min)(64u, maxAge));
  g_marginPm = (std::min)(250u, marginPct * 10);
  g_verifyEvery = verifyEvery;
  if (!on) {
    if (g_on.exchange(false)) {
      ResetCaches();
      Sleep(50);
      if (Quiet(500)) ReleaseTextures();
      Log("cascade cache: off");
    }
    return true;
  }
  if (g_disabled.load()) return true;
  if (!Install()) return false;
  if (!g_on.exchange(true)) {
    ResetCaches();
    Log("cascade cache: on (mask 0x%x, max age %u frames, margin %.1f %% per side, verify every %u reuse)",
        g_mask.load(), g_maxAge.load(), g_marginPm.load() / 10.0, g_verifyEvery.load());
  }
  return true;
}

void OnMissionEvent() { g_missionGen.fetch_add(1); }

void LogCounters(const char* when) {
  Log("cascade cache counters (%s): state %d%s, parses %llu in %llu frames, matrix writes %llu, bad proj %llu, faults "
      "%llu",
      when, g_state.load(), g_disabled.load() ? " (latched off)" : "", (unsigned long long)g_st.parses.load(),
      (unsigned long long)g_st.frames.load(), (unsigned long long)g_st.smWrite.load(),
      (unsigned long long)g_st.badProj.load(), (unsigned long long)g_st.readFault.load());
  for (int k = 0; k < 4; ++k) {
    const uint64_t rf = g_st.planRefresh[k].load(), ru = g_st.planReuse[k].load();
    if (!rf && !ru) continue;
    char why[256];
    int n = 0;
    why[0] = 0;
    for (int r = 1; r < kReasons; ++r)
      if (uint64_t v = g_st.reason[k][r].load())
        n += snprintf(why + n, sizeof(why) - n, "%s%s %llu", n ? ", " : "", kReasonName[r], (unsigned long long)v);
    Log("  CascadeShadowPass%d: %llu reuse, %llu refresh (%.1f %% reused; refresh reasons: %s)", k,
        (unsigned long long)ru, (unsigned long long)rf, 100.0 * ru / double(ru + rf), n ? why : "-");
  }
  Log("  passes %llu: captured %llu, restored %llu, restore fallback %llu, reuse without capture %llu, no plan %llu, no "
      "vector %llu, snap failures %llu",
      (unsigned long long)g_st.passes.load(), (unsigned long long)g_st.captured.load(),
      (unsigned long long)g_st.restored.load(), (unsigned long long)g_st.restoreFallback.load(),
      (unsigned long long)g_st.reuseMiss.load(), (unsigned long long)g_st.noPlan.load(),
      (unsigned long long)g_st.noVec.load(), (unsigned long long)g_st.snapFail.load());
  Log("  items: static %llu, dynamic %llu (NGModel casters %llu, other classes %llu), passes without a static item %llu; "
      "proj offset in the pass context %d",
      (unsigned long long)g_st.staticItems.load(), (unsigned long long)g_st.dynamicItems.load(),
      (unsigned long long)g_st.smrItems.load(), (unsigned long long)g_st.otherItems.load(),
      (unsigned long long)g_st.noStaticPasses.load(), g_projOff.load());
  {
    std::lock_guard<std::mutex> lock(g_seenLock);
    for (const SeenClass& s : g_seen) {
      HMODULE m = nullptr;
      wchar_t name[MAX_PATH] = L"?";
      if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                             static_cast<LPCWSTR>(s.vt), &m) &&
          m) {
        GetModuleFileNameW(m, name, MAX_PATH);
      }
      const wchar_t* slash = wcsrchr(name, L'\\');
      Log("  item class %ls+0x%llx: %s, %llu seen", slash ? slash + 1 : name,
          (unsigned long long)(reinterpret_cast<uintptr_t>(s.vt) - reinterpret_cast<uintptr_t>(m)),
          s.isStatic ? "static (cached)" : "dynamic (every frame)", (unsigned long long)s.n);
    }
  }
  if (g_st.otherModule.load()) Log("  parses of another cascade set left to DCS: %llu", (unsigned long long)g_st.otherModule.load());
  if (g_st.verifySkipped.load())
    Log("  verify skipped %llu times (shadow batching or the shadow recorder is on)",
        (unsigned long long)g_st.verifySkipped.load());
  if (g_st.verifyPasses.load() || g_st.verifyErrors.load()) {
    uint32_t bits = g_st.verifyMaxDiffBits.load();
    float md;
    memcpy(&md, &bits, 4);
    Log("  verify: %llu reuse passes compared with a full redraw, %llu of %llu texels differ (largest depth difference "
        "%.3g), %llu errors",
        (unsigned long long)g_st.verifyPasses.load(), (unsigned long long)g_st.verifyBad.load(),
        (unsigned long long)g_st.verifyTexels.load(), md, (unsigned long long)g_st.verifyErrors.load());
  }
}

// Suite: GPU timing OFF / ON / OFF / ON / OFF (5 s each) with the configured
// parameters, then the configured state back.
void TimingPhase(bool cfgOn, uint32_t mask, uint32_t maxAge, uint32_t marginPct, std::atomic<uint64_t>& frameCounter) {
  for (int i = 0; i < 5; ++i) {
    const bool on = (i & 1) != 0;
    if (!Apply(on, mask ? mask : 0xc, maxAge, marginPct, 0) && on) {
      Log("  cascade cache: not installed (GraphicsCore not ready or another build); phase skipped");
      break;
    }
    Sleep(1500);
    Log("  cascade cache timing run %d: %s", i + 1, on ? "ON (far cascades cached)" : "OFF (stock)");
    gpt::Measure(5000, frameCounter);
    if (on) LogCounters("timing run");
  }
  Apply(cfgOn, mask, maxAge, marginPct, g_verifyEvery.load());
}

void Shutdown() {
  g_on = false;
  if (g_state.load() != 1) return;
  shadowpass::ExecFn me = &Exec;
  shadowpass::g_inner.compare_exchange_strong(me, nullptr);
  if (g_parseSlot) {
    UnhookSlot(g_parseSlot, reinterpret_cast<void*>(g_parseOrig));
    g_parseSlot = nullptr;
  }
  if (Quiet(2000)) {
    ReleaseTextures();
    if (g_ctx) g_ctx->Release();
    g_ctx = nullptr;
  }
  g_state = 0;
}

}  // namespace cascache
