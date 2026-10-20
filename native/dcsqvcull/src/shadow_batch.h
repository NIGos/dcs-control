// Shadow caster instancing, stage 2 (R12): one instanced draw per group of
// identical casters in each cascade pass.
//
// Group key, per cascade pass execution (R12 2.2, measured in inst_count.h):
// ModelMaterialMT material, mesh, sbPositions page [item+0xd0], and the
// texture entries for casters that bind textures. Within a group only the
// per-object element offset [item+0xd4] differs; the material constant
// buffer bytes are otherwise identical (1.87M live comparisons, 0 differ).
//
// Flow (render thread):
//  1. Cascade execute wrapper (shadow_pass.h g_wrap), before the original:
//     plan the groups from the pass's caster vector; write every group's
//     [item+0xd4] values, in original order, into our structured buffer.
//     With [Model] ShadowPlanAsync the grouping is built ahead on planner
//     threads from RenderGraph::render entry and checked against the vector
//     here (identical, else rebuilt inline); see "Planner threads" below.
//  2. The original loop calls ShadowMapRenderable vt[1] per caster
//     (inst_count.h g_override):
//       solo   -> original call;
//       member -> skipped (the loop ignores the result [V GC 0xa5640]);
//       leader -> slot 5 (shadow_tex.h's copy, "Leader draw" below) with
//                 posStructOffset = the group's base index into our
//                 buffer, the effect pass's VS block pointing at our
//                 instanced variant of DCS's own shadow VS (shadow_inst.h),
//                 our buffer at VS t127, and DX11Renderer::draw's instance
//                 count argument = group size (a6, [V dx11backend 0x14bcb]:
//                 non-zero gives DrawIndexedInstanced(count, N, start, 0, 0)
//                 instead of DrawIndexed(count, start, 0)).
//     Our VS computes posStructOffset = qvInstOffsets[qvPsoBase + id]: the
//     same value each member would have used.
//  3. After the original: every touched material's mat+0x18c is set to the
//     value the stock loop would have left (last caster in original order).
// Depth-only output with GREATER and no blending does not depend on order.
//
// Verification ([Suite] ShadowInstVerify): each cascade pass runs twice in
// the same frame, stock then batched (each run clears depth itself), and the
// two depth targets are compared bit for bit; any mismatch turns batching off.
//
// The instance count reaches DX11Renderer::draw through a copy of the
// renderer's vtable, swapped into the renderer object only for the leader's
// call and restored right after.
// Included once from main.cpp inside its anonymous namespace, after
// inst_count.h, shadow_pass.h and shadow_inst.h.
#pragma once

namespace shadowbatch {

constexpr uint32_t kSmrVtbl = 0x59828;      // NGModel ShadowMapRenderable
constexpr uint32_t kModelMatVtbl = 0x592f0; // NGModel ModelMaterialMT
constexpr uint32_t kRendererVtbl = 0xb2a60; // dx11backend DX11Renderer
constexpr uint32_t kDrawRva = 0x14870;      // DX11Renderer::draw (slot 35)
constexpr int kDrawSlot = 35;
constexpr int kVtblCopy = 92;  // DX11Renderer vtable entries in this build (entry 92 is string data)
constexpr uint32_t kMaxOffsets = 1 << 16;
constexpr size_t kTable = 1 << 14;  // casters per pass (power of two)

std::atomic<bool> g_on{false};
std::atomic<bool> g_verify{false};
std::atomic<int> g_state{0};  // 0 = not tried, 1 = ready, -1 = unavailable
// Latched off for the session on any fault or verification mismatch.
std::atomic<bool> g_disabled{false};
std::atomic<bool> g_shutdown{false};
std::atomic<int> g_inWrap{0};
void Disable(const char* why) {
  if (!g_disabled.exchange(true)) Log("shadow batching: disabled for this session (%s)", why);
}

uint8_t* g_ng = nullptr;
void* g_smrVtbl = nullptr;
void* g_modelMatVtbl = nullptr;
void*** g_rendererApi = nullptr;  // renderer.dll RenderAPI::pRenderApi
void** g_rendererObj = nullptr;  // the DX11Renderer instance
void** g_rendererVtbl = nullptr;
void* g_myTable[kVtblCopy + 1];
void** g_myVtbl = nullptr;
using DrawFn = void(__fastcall*)(void* self, int a1, void* shader, int prim, int a4, int a5, int instances,
                                 const char* name);
DrawFn g_origDraw = nullptr;
thread_local uint32_t t_instances = 0;

ID3D11DeviceContext* g_ctx = nullptr;
ID3D11Buffer* g_buf = nullptr;
ID3D11ShaderResourceView* g_srv = nullptr;
DWORD g_renderThread = 0;

// Counters (render thread).
std::atomic<uint64_t> g_replayed{0};
std::atomic<uint64_t> g_passes{0}, g_casters{0}, g_groups{0}, g_leaders{0}, g_skipped{0}, g_fallbacks{0}, g_tooLarge{0};
std::atomic<uint64_t> g_verifyPasses{0}, g_verifyMismatch{0}, g_verifyPixels{0}, g_verifyBadPixels{0}, g_verifyErrors{0};

// Optional draw-time check (gb_batch.h): runs inside the leader's call just
// before DX11Renderer::draw, after every bind of that draw; false = draw the
// leader alone (DCS's own instance count), its members then draw themselves.
using DrawCheckFn = bool (*)(void* arg);
thread_local DrawCheckFn t_check = nullptr;
thread_local void* t_checkArg = nullptr;
// Optional observer (shadow_rec.h probes), called right after the original
// draw with the arguments it got: the immediate context then holds exactly
// the bindings of that D3D draw. nullptr when unused.
using DrawAfterFn = void (*)(void* self, int a1, void* shader, int prim, int a4, int a5, int instances);
thread_local DrawAfterFn t_after = nullptr;

void __fastcall DrawN(void* self, int a1, void* shader, int prim, int a4, int a5, int instances, const char* name) {
  uint32_t n = t_instances;
  if (n && t_check && !t_check(t_checkArg)) n = 0;
  const int count = n ? static_cast<int>(n) : instances;
  g_origDraw(self, a1, shader, prim, a4, a5, count, name);
  if (DrawAfterFn f = t_after) f(self, a1, shader, prim, a4, a5, count);
}

// ---- Plan (per cascade pass execution) ----
//
// Planner threads (R15 F1, [Model] ShadowPlanAsync). Tags: [V] verified in
// the binary (DCS 2.9.30 Visualizer / GraphicsCore / Scene), [I] inferred.
//
// When the caster vectors are final [V]:
//  - Visualizer 0x1705a1 calls collectRenderablesAggregated(n, req, tq =
//    [this+0x738], ...). Inside (GC 0xbcc60) the culling join is the wait
//    loop 0xbd9b0-0xbda5d; the terrain merge 0xbdb64 (0xbba30) follows, then
//    IView vt[4] sortAndBatchRenderables (0xbdbb6) with the same tq (r12 =
//    [rsp+0x60] = the r8 argument, 0xbcc96/0xbcc99/0xbdafc) and vt[5].
//  - Scene sortAndBatchRenderables (0x25780) pushes every chunk of its
//    per-collection work to that tq (0xfd40 -> tq->vt[4] at 0xfe5b) and only
//    waits for them when byte Scene+0x4659e is set (0x2594d); nothing writes
//    that byte (only reads at 0xff9f, 0x17246, 0x2594d), so it returns while
//    the chunks still run on the pool. Shading model 14 (the cascades) is in
//    the list built at Scene 0x10e0 {6,0,1,2,14,11,23,24,25}, whose
//    collections go through sortAndBatchSingleCollection -> 0x1d5b0, which
//    rewrites the vector in place (batched runs).
//  - Those chunks are joined by Visualizer 0x1707b6, [this+0x738]->vt[3]
//    (ed::ThreadPool wait-all), after EndParse (0x1705b1) and BeginFrame.
//  - Then Visualizer calls RenderGraph::render(rg, [rg+0xa18], [rg+0xa30],
//    params) through its GraphicsCore import (0x1708bb, 0x170a2a, 0x170af1),
//    which stores the vector array at [rg+0x438] (GC 0x5dbfc) before it runs
//    the passes; the cascade pass reads [[ctx]+0x438] + id * 24 (GC 0x8bbc0).
// So the vectors are final at the entry of RenderGraph::render, not when
// collectRenderablesAggregated returns. HookRender (that import) queues the
// plans of the cascades seen in earlier frames ([rg+0xa18] = the argument,
// count = ([rg+0xa20] - [rg+0xa18]) / 24 [V Visualizer 0x170f30]); the
// G-buffer passes run first inside render(), so the planners have that time.
//
// Thread safety of a build off the render thread:
//  - Caster pointers: copied into the plan (snap) first; at the pass the DCS
//    vector must still have the same descriptor, begin, size and the same
//    pointers (memcmp), else the plan is rebuilt on the render thread.
//  - Item/material fields the grouping reads ([r], [r+0x10], item+0x10,
//    +0x18 texture entries, +0xc0, +0xd0; mat+0x28/0x30/0x210/0x218/0x240../
//    0x2d8; props) are written by the parse before the join and not by the
//    draws [I; the G-buffer draw writes mat+0x18c, mat+0x90..0x1c0 and
//    mat+0xb0..0x118, gb_batch.h notes, none of them read here]. Texture
//    swaps happen in the frame-start drain [V R5], before the join.
//  - item+0xd4 (offsets, restore values) is NOT read by the planners: our own
//    leader draws (G-buffer batching; shadow leaders without g_psoDirect)
//    swap it temporarily on the render thread. The pass reads it on the render thread (Commit), at
//    the same point the inline planner always did.
//  - shadowinst::FindVs: lock-free map, entries published with release
//    stores and never removed before unload [V shadow_inst.h FindEntry /
//    PublishLocked]. A publish between the build and the pass would change
//    eligibility: the build records g_mapUsed (incremented after each
//    publish) and the pass requires it unchanged.
//  - shadowtex::Lookup may build and insert a mask (shadow inst reads, log,
//    mutex): render thread only. Planners use the lock-free MaskCache::Find;
//    a miss that Lookup would build defers the whole plan to the render
//    thread (kResNeedRender). The skip state (on, installed, cache full) is
//    taken once per build and must be unchanged at the pass; every mask used
//    must still be keyed to its shader (a destroyed shader tombstones it).
//  - instcount::IsTextured calls ModelDesc PropertiesSet::getTexture and
//    Texture2dProperties::valid, const readers [I].
// Any fault on a planner thread is caught (__try) and the plan is rebuilt on
// the render thread.
enum : uint8_t { kSolo = 0, kLeader = 1, kMember = 2 };
struct Slot {  // by caster index
  void* r;
  uint32_t gen;
  uint8_t role;
  uint32_t base, count, group;
  const shadowtex::MaskEntry* mask;  // member: replay its skipped texture sets (masked group key)
};
struct GroupSlot {  // by key
  uint32_t gen;
  void* mat;
  void* mesh;
  uint32_t page;
  uint64_t tex;
  int32_t first, last;  // caster indices, chained through Plan::next
  uint32_t count;
  const shadowtex::MaskEntry* mask;
};
struct MatLast {
  uint8_t* mat;
  uint32_t pso;   // [item+0xd4] of `last`, read on the render thread (FillPso)
  uint32_t gen;
  int32_t last;   // caster index of the material's last caster in original order
  int8_t eligible;  // -1 = not checked this pass; 0/1 = our VS exists and <= 32 textures
  int8_t textured;  // binds textures in the shadow path (props, not per item)
  const shadowtex::MaskEntry* mask;  // texture-skip mask when ready (masked group key), else nullptr
  void* maskShader;                  // the DX11Shader `mask` was found for
};
constexpr size_t kMaxCasters = kTable / 2;
enum : int { kResNone = 0, kResBuilt = 1, kResTooLarge = 2, kResNeedRender = 3, kResFault = 4 };
enum : int { kIdle = 0, kQueued = 1, kBuilding = 2, kDone = 3, kInUse = 4 };

struct Plan {
  // What it was built from (identity, checked at the pass).
  void** vec = nullptr;    // the {begin, end, cap} descriptor
  void** begin = nullptr;  // its begin when built
  size_t n = 0;
  uint32_t frame = 0;    // planner plans: g_renderGen when queued
  uint32_t mapUsed = 0;  // shadowinst::g_mapUsed when the build started
  int texMode = 0;       // TexMode() when the build started
  int result = kResNone;
  bool byWorker = false;
  std::atomic<int> state{kIdle};
  // The grouping.
  uint32_t gen = 0;
  uint32_t groupCount = 0, matCount = 0, offsetCount = 0, groups = 0, leaders = 0, skipped = 0;
  size_t cursor = 0;  // next caster index the pass loop should reach (render thread)
  Slot slots[kMaxCasters];
  int32_t next[kMaxCasters];
  uint32_t order[kMaxCasters];  // caster index of each buffer element
  void* snap[kMaxCasters];      // the caster pointers a planner thread built from
  GroupSlot gslots[kTable];
  uint32_t groupList[kTable];   // group slots created, in first-seen order
  uint8_t groupFailed[kTable];  // by group slot: the leader fell back, members draw themselves
  MatLast mats[kTable];         // by material pointer
  uint32_t matList[kTable];     // used slots, in first-seen order
};

Plan g_syncPlan;           // built on the render thread
Plan* g_cur = nullptr;     // plan of the pass being drawn (render thread)
uint32_t g_offsets[kMaxCasters];
bool g_planActive = false;
bool g_batching = false;  // current run of the pass uses the plan
// Verification: called with true before the stock run and false before the
// batched run, so other caster-path optimizations (the shadow texture skip)
// are off in the reference and on in the compared run; nullptr = unused.
void (*g_verifyStock)(bool stock) = nullptr;
bool g_capture = false;   // verification run: capture the depth target at the first caster
ID3D11Resource* g_depth = nullptr;  // verification: depth target seen at the first caster

// Planner threads.
constexpr int kWorkers = 2;
constexpr int kMaxCascades = 8;     // cascade passes with their own plan slot
constexpr uint32_t kForget = 120;   // stop planning a cascade not drawn for this many renders
std::atomic<bool> g_async{false};   // [Model] ShadowPlanAsync
std::atomic<bool> g_workersUp{false};
std::atomic<bool> g_workersStop{false};
HANDLE g_sem = nullptr;
HANDLE g_workers[kWorkers] = {};
std::atomic<Plan*> g_plans[kMaxCascades] = {};
struct Learned {
  void* rg;       // RenderGraph ([ctx] of the pass)
  uint32_t idx;   // its collection index ([rg+0x438] + idx * 24)
  uint32_t seen;  // g_renderGen when its pass last ran
  Plan* plan;
};
Learned g_learned[kMaxCascades];  // render thread
int g_learnedCount = 0;
uint32_t g_renderGen = 0;         // RenderGraph::render entries (render thread)
std::atomic<int> g_inQueue{0};
using RenderFn = void(__fastcall*)(void* rg, void* renderables, void* lights, void* params);
RenderFn g_origRender = nullptr;
void** g_renderSlot = nullptr;   // Visualizer.dll's import
void** g_renderSlot2 = nullptr;  // SceneRenderer.dll's import (the caller in 2.9.30)
constexpr char kRenderImport[] =
    "?render@RenderGraph@0@QEAAXQEBV?$vector@PEAUISceneRenderable@render@@V?$allocator@PEAUISceneRenderable@"
    "render@@@ed@@@ed@@QEBV?$vector@PEAUSceneLight@render@@V?$allocator@PEAUSceneLight@render@@@ed@@@3@"
    "AEBURenderParams@10@@Z";

// Planner counters.
std::atomic<uint64_t> g_planWorker{0};       // plans from a planner thread used at the pass
std::atomic<uint64_t> g_planRender{0};       // plans built on the render thread
std::atomic<uint64_t> g_planStolen{0};       // ... of which queued but not started by a planner
std::atomic<uint64_t> g_planWorkerBuilt{0};  // builds finished by planner threads
std::atomic<uint64_t> g_planWaits{0}, g_planWaitUs{0}, g_planWaitMaxUs{0}, g_planTimeouts{0};
// R90 H1 ([Model] ShadowPlanWaitUs, default 300; 20000 = the pre-R90 cap): how long the render thread spins for
// a plan a planner thread is still building before it builds the pass's plan itself (the inline plan, the same
// one a planner builds). A planner preempted mid-build used to hold the render thread up to 20 ms.
std::atomic<uint32_t> g_planWaitCapUs{300};
std::atomic<uint64_t> g_planMismatch{0}, g_planDeferred{0}, g_planFaults{0};
// Queue diagnostics: render entries seen, entries that passed the gates,
// entries whose vector did not match [rg+0xa18], entries that queued nothing.
std::atomic<uint64_t> g_qEntries{0}, g_qGated{0}, g_qNoVector{0}, g_qNone{0};
void* g_qLastRg = nullptr;
void* g_qLastLearnRg = nullptr;

inline size_t PtrHash(const void* p) {
  uint64_t h = reinterpret_cast<uint64_t>(p) * 0x9E3779B97F4A7C15ull;
  return static_cast<size_t>(h >> 50) & (kTable - 1);
}

// The pass loop calls vt[1] for its casters in vector order [V GC 0xa5640];
// only ShadowMapRenderable entries reach our hook, so the cursor moves
// forward to `r` (skipping other classes). Not found = this caster was not
// planned (treated as solo; a member already skipped was drawn by its leader,
// and drawing a planned member again alone writes the same depth).
Slot* FindSlot(Plan& p, void* r) {
  for (size_t k = p.cursor; k < p.n; ++k) {
    if (p.begin[k] == r) {
      p.cursor = k + 1;
      return &p.slots[k];
    }
  }
  return nullptr;
}

// FindSlot without moving the cursor (shadow_rec.h: the role of a caster it
// may draw itself instead of passing it on).
Slot* PeekSlot(Plan& p, void* r) {
  for (size_t k = p.cursor; k < p.n; ++k)
    if (p.begin[k] == r) return &p.slots[k];
  return nullptr;
}

// Technique handle the shadow submit will use, and our VS for it.
uint64_t TechOf(uint8_t* mat) {
  auto* props = *reinterpret_cast<uint8_t**>(mat + 0x28);
  return *reinterpret_cast<uint64_t*>(mat + (props[0x33] ? 0x218 : 0x210));
}

// Hash of the (aux, tex) entries the shadow passes read (the texture skip's
// mask); the skipped ones are replayed per member instead (never 0).
uint64_t MaskedTextureKey(uint8_t* mat, uint8_t* item, const shadowtex::MaskEntry& e) {
  auto* props = *reinterpret_cast<uint8_t**>(mat + 0x28);
  auto* arr = *reinterpret_cast<uint8_t***>(item + 0x18);
  if (!props || !arr || !*arr) return 1;
  const uint8_t* en = *arr + static_cast<size_t>(*reinterpret_cast<uint32_t*>(props + 0x26c)) * 0x18;
  uint64_t h = 0xcbf29ce484222325ull ^ 0x5a5a;
  const uint32_t n = *reinterpret_cast<uint32_t*>(mat + 0x2d8);
  for (uint32_t i = 0; i < n && i < 32; ++i, en += 0x18) {
    const int64_t hd = *reinterpret_cast<int64_t*>(mat + 0x240 + 8 * i);
    if (hd == -1 || shadowtex::Skippable(e, hd)) continue;
    h = (h ^ *reinterpret_cast<const uint64_t*>(en)) * 0x100000001b3ull;
    h = (h ^ *reinterpret_cast<const uint64_t*>(en + 8)) * 0x100000001b3ull;
  }
  return h | 1;
}

// Shadow texture skip state a plan depends on: bit 0 = masks in use, bit 1 =
// mask cache full (Lookup then builds nothing).
int TexMode() {
  return (shadowtex::g_on.load(std::memory_order_relaxed) && shadowtex::g_state.load() > 0 ? 1 : 0) |
         (shadowtex::g_cacheFull.load() ? 2 : 0);
}

// The texture-skip mask of a textured material. Render thread: Lookup (may
// build it). Planner thread: cache lookup only; *defer = true when Lookup
// would build one.
const shadowtex::MaskEntry* MaskFor(uint8_t* mat, bool renderThread, bool* defer, void** shaderOut) {
  uint8_t* sh = *reinterpret_cast<uint8_t**>(mat + 0x30);
  if (!sh || *reinterpret_cast<void**>(sh) != shadowtex::g_shaderVtblPtr) return nullptr;
  const uint64_t ta = *reinterpret_cast<uint64_t*>(mat + 0x210), tb = *reinterpret_cast<uint64_t*>(mat + 0x218);
  const shadowtex::MaskEntry* e = nullptr;
  if (renderThread) {
    bool pending = false;
    e = shadowtex::Lookup(sh, ta, tb, &pending);
  } else {
    if (shadowtex::g_cache)
      e = shadowtex::g_cache->Find(sh, ta, tb, *reinterpret_cast<void**>(sh + 0x50), *reinterpret_cast<void**>(sh + 0xc8),
                                   *reinterpret_cast<void**>(sh + 0xd0), *reinterpret_cast<void**>(sh + 0xb0));
    if (!e && !shadowtex::g_cacheFull.load()) {
      *defer = true;
      return nullptr;
    }
  }
  if (!e || e->state <= 0) return nullptr;
  *shaderOut = sh;
  return e;
}

// Groups the casters c[0..n) into p (CPU only: no D3D, no writes to DCS
// memory, item+0xd4 not read). Same result on any thread, except that a
// planner thread returns kResNeedRender where the render thread would build
// a texture-skip mask.
int BuildCore(Plan& p, void* const* c, size_t n, bool renderThread, int texMode) {
  p.groupCount = p.matCount = p.offsetCount = p.groups = p.leaders = p.skipped = 0;
  if (!c || n == 0) return kResNone;
  if (n > kMaxCasters) return kResTooLarge;
  if (++p.gen == 0) {
    memset(p.slots, 0, sizeof(p.slots));
    memset(p.gslots, 0, sizeof(p.gslots));
    memset(p.mats, 0, sizeof(p.mats));
    p.gen = 1;
  }
  const uint32_t gen = p.gen;
  uint32_t groups = 0;
  for (size_t i = 0; i < n; ++i) {
    void* r = c[i];
    Slot* s = &p.slots[i];
    *s = {r, gen, kSolo, 0, 0, 0, nullptr};
    p.next[i] = -1;
    if (*static_cast<void**>(r) != g_smrVtbl) continue;
    auto* item = *reinterpret_cast<uint8_t**>(static_cast<uint8_t*>(r) + 0x10);
    if (!item) continue;
    auto* mat = *reinterpret_cast<uint8_t**>(item + 0x10);
    if (!mat || *reinterpret_cast<void**>(mat) != g_modelMatVtbl) continue;
    // Last caster per material in original order (for step 3), and the
    // per-material facts (checked once per pass).
    MatLast* ml = nullptr;
    {
      size_t mi = PtrHash(mat);
      for (size_t k = 0;; ++k, mi = (mi + 1) & (kTable - 1)) {
        if (k == kTable) return kResNone;
        MatLast& m = p.mats[mi];
        if (m.gen != gen) {
          m = {mat, 0, gen, static_cast<int32_t>(i), -1, 0, nullptr, nullptr};
          p.matList[p.matCount++] = static_cast<uint32_t>(mi);
          ml = &m;
          break;
        }
        if (m.mat == mat) {
          m.last = static_cast<int32_t>(i);
          ml = &m;
          break;
        }
      }
    }
    if (ml->eligible < 0) {
      void* shader = *reinterpret_cast<void**>(mat + 0x30);
      ml->eligible = shader && *reinterpret_cast<uint32_t*>(mat + 0x2d8) <= 32 &&
                             shadowinst::FindVs(shader, TechOf(mat), 0) != nullptr
                         ? 1
                         : 0;
      ml->textured = ml->eligible ? (instcount::IsTextured(mat) ? 1 : 0) : 0;
      ml->mask = nullptr;
      if (ml->textured && (texMode & 1)) {
        bool defer = false;
        ml->mask = MaskFor(mat, renderThread, &defer, &ml->maskShader);
        if (defer) return kResNeedRender;
      }
    }
    if (!ml->eligible) continue;
    void* mesh = *reinterpret_cast<void**>(item + 0xc0);
    const uint32_t page = *reinterpret_cast<uint32_t*>(item + 0xd0);
    const uint64_t tex = !ml->textured ? 0
                         : ml->mask       ? MaskedTextureKey(mat, item, *ml->mask)
                                          : instcount::TextureEntriesKey(mat, item);
    uint64_t h = reinterpret_cast<uint64_t>(mat) * 0x9E3779B97F4A7C15ull ^
                 reinterpret_cast<uint64_t>(mesh) * 0xC2B2AE3D27D4EB4Full ^ (page + tex) * 0x165667B19E3779F9ull;
    size_t gi = static_cast<size_t>(h >> 50) & (kTable - 1);
    for (size_t k = 0;; ++k, gi = (gi + 1) & (kTable - 1)) {
      if (k == kTable) return kResNone;
      GroupSlot& g = p.gslots[gi];
      if (g.gen != gen) {
        g = {gen, mat, mesh, page, tex, static_cast<int32_t>(i), static_cast<int32_t>(i), 1, ml->mask};
        p.groupList[p.groupCount++] = static_cast<uint32_t>(gi);
        ++groups;
        break;
      }
      if (g.mat == mat && g.mesh == mesh && g.page == page && g.tex == tex) {
        p.next[g.last] = static_cast<int32_t>(i);
        g.last = static_cast<int32_t>(i);
        ++g.count;
        break;
      }
    }
  }
  // Assign roles and the buffer order (the offsets themselves are read at
  // the pass, Commit).
  uint32_t cursor = 0, leaders = 0, skipped = 0;
  for (uint32_t li = 0; li < p.groupCount; ++li) {
    const uint32_t gi = p.groupList[li];
    GroupSlot& g = p.gslots[gi];
    if (g.count < 2) continue;
    if (cursor + g.count > kMaxCasters) break;  // cannot happen (cursor + count <= n)
    const uint32_t base = cursor;
    p.groupFailed[gi] = 0;
    for (int32_t i = g.first; i >= 0; i = p.next[i]) {
      p.order[cursor++] = static_cast<uint32_t>(i);
      Slot* s = &p.slots[i];
      s->role = i == g.first ? kLeader : kMember;
      s->base = base;
      s->count = g.count;
      s->group = gi;
      s->mask = g.mask;
    }
    ++leaders;
    skipped += g.count - 1;
  }
  p.offsetCount = cursor;
  p.groups = groups;
  p.leaders = leaders;
  p.skipped = skipped;
  return cursor ? kResBuilt : kResNone;
}

// Reads the vector, snapshots it (planner threads) and builds.
int BuildRaw(Plan& p, bool renderThread) {
  auto** begin = static_cast<void**>(p.vec[0]);
  auto** end = static_cast<void**>(p.vec[1]);
  p.begin = begin;
  p.n = static_cast<size_t>(end - begin);
  if (!begin || p.n == 0) return kResNone;
  if (p.n > kMaxCasters) return kResTooLarge;
  void* const* c = begin;
  if (!renderThread) {
    memcpy(p.snap, begin, p.n * sizeof(void*));
    c = p.snap;
  }
  return BuildCore(p, c, p.n, renderThread, p.texMode);
}

int BuildGuarded(Plan& p, bool renderThread) {
  p.mapUsed = shadowinst::g_mapUsed.load();
  p.texMode = TexMode();
  __try {
    return BuildRaw(p, renderThread);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return kResFault;
  }
}

// True when the plan is what the render thread would build for `vec` now.
bool MatchesRaw(const Plan& p, void** vec, uint32_t frame) {
  if (p.vec != vec || p.frame != frame) return false;
  auto** begin = static_cast<void**>(vec[0]);
  const size_t n = static_cast<size_t>(static_cast<void**>(vec[1]) - begin);
  if (begin != p.begin || n != p.n) return false;
  if (p.mapUsed != shadowinst::g_mapUsed.load() || p.texMode != TexMode()) return false;
  if (p.result == kResTooLarge || !begin || n == 0) return true;
  if (memcmp(p.snap, begin, n * sizeof(void*)) != 0) return false;
  for (uint32_t i = 0; i < p.matCount; ++i) {
    const MatLast& m = p.mats[p.matList[i]];
    if (m.mask && m.mask->key.load(std::memory_order_acquire) != m.maskShader) return false;
  }
  return true;
}

bool Matches(const Plan& p, void** vec, uint32_t frame) {
  __try {
    return MatchesRaw(p, vec, frame);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

// Render thread, at the pass: the values the stock loop leaves in mat+0x18c
// (step 3) and each group's element offsets in buffer order.
void FillPso(Plan& p) {
  for (uint32_t i = 0; i < p.matCount; ++i) {
    MatLast& m = p.mats[p.matList[i]];
    auto* item = *reinterpret_cast<uint8_t**>(static_cast<uint8_t*>(p.begin[m.last]) + 0x10);
    m.pso = *reinterpret_cast<uint32_t*>(item + 0xd4);
  }
}

void GatherOffsets(const Plan& p, uint32_t* dst) {
  for (uint32_t k = 0; k < p.offsetCount; ++k) {
    auto* item = *reinterpret_cast<uint8_t**>(static_cast<uint8_t*>(p.begin[p.order[k]]) + 0x10);
    dst[k] = *reinterpret_cast<uint32_t*>(item + 0xd4);
  }
}

bool CommitRaw(Plan& p) {
  FillPso(p);
  GatherOffsets(p, g_offsets);
  D3D11_MAPPED_SUBRESOURCE m;
  if (FAILED(g_ctx->Map(g_buf, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) return false;
  memcpy(m.pData, g_offsets, p.offsetCount * sizeof(uint32_t));
  g_ctx->Unmap(g_buf, 0);
  return true;
}

bool Commit(Plan& p) {
  bool ok = false;
  __try {
    ok = CommitRaw(p);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    ok = false;
  }
  if (!ok) return false;
  g_casters += p.n;
  g_groups += p.groups;
  g_leaders += p.leaders;
  g_skipped += p.skipped;
  return true;
}

void RestoreMats(const Plan& p) {
  for (uint32_t i = 0; i < p.matCount; ++i) {
    const MatLast& m = p.mats[p.matList[i]];
    *reinterpret_cast<uint32_t*>(m.mat + 0x18c) = m.pso;
  }
}

// Activates a built plan: nullptr = draw the pass as stock.
Plan* Activate(Plan& p) {
  if (p.result == kResTooLarge) g_tooLarge++;
  if (p.result != kResBuilt || !Commit(p)) return nullptr;
  return &p;
}

void ReleasePlan(Plan* p) {
  if (p && p != &g_syncPlan) p->state.store(kIdle, std::memory_order_release);
}

void NoteMax(std::atomic<uint64_t>& a, uint64_t v) {
  uint64_t cur = a.load();
  while (v > cur && !a.compare_exchange_weak(cur, v)) {
  }
}

// Render thread, at the pass: the plan for `vec` (planner thread, else built
// here), committed; nullptr = draw the pass as stock. Release with ReleasePlan.
Plan* PlanForPass(void** vec) {
  if (g_async.load(std::memory_order_relaxed) && g_workersUp.load()) {
    for (int k = 0; k < g_learnedCount; ++k) {
      Plan* p = g_learned[k].plan;
      if (p->vec != vec) continue;
      int st = p->state.load(std::memory_order_acquire);
      if (st == kQueued && p->state.compare_exchange_strong(st, kBuilding, std::memory_order_acq_rel)) {
        // No planner thread started it yet: build it here, now (it may build
        // masks), which is the inline plan itself.
        p->byWorker = false;
        p->vec = vec;
        p->result = BuildGuarded(*p, true);
        p->state.store(kInUse, std::memory_order_relaxed);
        g_planStolen++;
        g_planRender++;
        Plan* a = Activate(*p);
        if (!a) ReleasePlan(p);
        return a;
      } else if (st == kBuilding) {
        LARGE_INTEGER a, b;
        QueryPerformanceCounter(&a);
        const int64_t limit = static_cast<int64_t>(g_planWaitCapUs.load(std::memory_order_relaxed) /
                                                   (g_qpcToUs > 0 ? g_qpcToUs : 1.0));  // R90 H1
        for (;;) {
          if (p->state.load(std::memory_order_acquire) != kBuilding) break;
          YieldProcessor();
          QueryPerformanceCounter(&b);
          if (b.QuadPart - a.QuadPart > limit) break;
        }
        QueryPerformanceCounter(&b);
        const uint64_t us = static_cast<uint64_t>((b.QuadPart - a.QuadPart) * g_qpcToUs);
        g_planWaits++;
        g_planWaitUs += us;
        NoteMax(g_planWaitMaxUs, us);
      }
      if (p->state.load(std::memory_order_acquire) != kDone) {
        g_planTimeouts++;  // still building: leave it to its planner thread, build the pass's plan here
        break;
      }
      p->state.store(kInUse, std::memory_order_relaxed);
      const int res = p->result;
      if (res != kResNeedRender && res != kResFault && Matches(*p, vec, g_renderGen)) {
        g_planWorker++;
        Plan* a = Activate(*p);
        if (!a) ReleasePlan(p);
        return a;
      }
      if (res == kResNeedRender)
        g_planDeferred++;
      else if (res == kResFault)
        g_planFaults++;
      else
        g_planMismatch++;
      ReleasePlan(p);
      break;
    }
  }
  Plan& s = g_syncPlan;
  s.vec = vec;
  s.frame = g_renderGen;
  s.result = BuildGuarded(s, true);
  g_planRender++;
  return Activate(s);
}

// Render thread: remembers the cascade (render graph, collection index) so
// the next RenderGraph::render entries plan it ahead.
bool LearnRaw(void** vec, void* ctx, void** rgOut, uint32_t* idxOut) {
  auto* rg = *static_cast<uint8_t**>(ctx);
  auto* base = *reinterpret_cast<uint8_t**>(rg + 0x438);
  const intptr_t d = reinterpret_cast<uint8_t*>(vec) - base;
  if (!base || d < 0 || d % 24 != 0 || d / 24 > 0xffff) return false;
  g_qLastLearnRg = rg;
  *rgOut = rg;
  *idxOut = static_cast<uint32_t>(d / 24);
  return true;
}

void Learn(void** vec, void* ctx) {
  void* rg = nullptr;
  uint32_t idx = 0;
  __try {
    if (!LearnRaw(vec, ctx, &rg, &idx)) return;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return;
  }
  for (int k = 0; k < g_learnedCount; ++k) {
    if (g_learned[k].rg == rg && g_learned[k].idx == idx) {
      g_learned[k].seen = g_renderGen;
      return;
    }
  }
  if (g_learnedCount == kMaxCascades) return;
  void* mem = VirtualAlloc(nullptr, sizeof(Plan), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
  if (!mem) return;
  Plan* p = new (mem) Plan;
  g_learned[g_learnedCount] = {rg, idx, g_renderGen, p};
  g_plans[g_learnedCount].store(p, std::memory_order_release);
  ++g_learnedCount;
}

// Render thread, at RenderGraph::render entry (the vectors are final): queues
// the plans of this graph's known cascades.
size_t VectorCountRaw(uint8_t* rg, void* renderables) {
  auto* b = *reinterpret_cast<uint8_t**>(rg + 0xa18);
  auto* e = *reinterpret_cast<uint8_t**>(rg + 0xa20);
  if (b != renderables || e < b) return 0;
  return static_cast<size_t>(e - b) / 24;
}

void QueuePlans(void* rg, void* renderables) {
  size_t count = 0;
  __try {
    count = VectorCountRaw(static_cast<uint8_t*>(rg), renderables);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    count = 0;
  }
  const uint32_t frame = ++g_renderGen;
  g_qLastRg = rg;
  if (!count) {
    g_qNoVector++;
    return;
  }
  LONG queued = 0;
  for (int k = 0; k < g_learnedCount; ++k) {
    Learned& l = g_learned[k];
    if (l.rg != rg || l.idx >= count || frame - l.seen > kForget) continue;
    Plan* p = l.plan;
    int st = p->state.load(std::memory_order_acquire);
    if (st == kBuilding || st == kInUse) continue;
    if (st == kQueued && !p->state.compare_exchange_strong(st, kIdle, std::memory_order_acq_rel)) continue;
    p->vec = reinterpret_cast<void**>(static_cast<uint8_t*>(renderables) + l.idx * 24);
    p->frame = frame;
    p->state.store(kQueued, std::memory_order_release);
    ++queued;
  }
  if (queued) ReleaseSemaphore(g_sem, queued, nullptr);
  else g_qNone++;
}

// Optional observer at RenderGraph::render entry (shadow_rec_count.h, for a
// measurement phase only), called on the caller's thread before the plans are
// queued; nullptr when unused.
using RenderObserverFn = void (*)(void* rg, void* renderables);
std::atomic<RenderObserverFn> g_renderObserver{nullptr};
// The shadow recorder's job queue (shadow_rec.h), same call point.
std::atomic<RenderObserverFn> g_recObserver{nullptr};
// Measurement counters (run_threads.h: render entry), same call point, first.
std::atomic<RenderObserverFn> g_measureObserver{nullptr};

void __fastcall HookRender(void* rg, void* renderables, void* lights, void* params) {
  g_inQueue.fetch_add(1);
  g_qEntries++;
  if (RenderObserverFn ob = g_measureObserver.load(std::memory_order_relaxed)) ob(rg, renderables);
  if (RenderObserverFn ob = g_renderObserver.load(std::memory_order_relaxed)) ob(rg, renderables);
  if (RenderObserverFn ob = g_recObserver.load(std::memory_order_relaxed)) ob(rg, renderables);
  if (!g_shutdown.load() && g_async.load(std::memory_order_relaxed) && g_on.load(std::memory_order_relaxed) &&
      !g_disabled.load(std::memory_order_relaxed) && g_state.load() == 1 && g_workersUp.load() &&
      GetCurrentThreadId() == g_renderThread) {
    g_qGated++;
    QueuePlans(rg, renderables);
  }
  g_inQueue.fetch_sub(1);
  g_origRender(rg, renderables, lights, params);
}

DWORD WINAPI PlanWorker(void*) {
  for (;;) {
    WaitForSingleObject(g_sem, INFINITE);
    if (g_workersStop.load()) return 0;
    for (int k = 0; k < kMaxCascades; ++k) {
      Plan* p = g_plans[k].load(std::memory_order_acquire);
      if (!p) continue;
      int st = kQueued;
      if (!p->state.compare_exchange_strong(st, kBuilding, std::memory_order_acq_rel)) continue;
      p->byWorker = true;
      p->result = BuildGuarded(*p, false);
      g_planWorkerBuilt++;
      p->state.store(kDone, std::memory_order_release);
      break;
    }
  }
}

bool StartWorkers() {
  if (g_workersUp.load()) return true;
  g_workersStop = false;
  g_sem = CreateSemaphoreW(nullptr, 0, 1 << 20, nullptr);
  if (!g_sem) return false;
  for (int i = 0; i < kWorkers; ++i) {
    g_workers[i] = CreateThread(nullptr, 0, qvexit::Thread<PlanWorker>, nullptr, 0, nullptr);
    if (!g_workers[i]) {
      g_workersStop = true;
      ReleaseSemaphore(g_sem, kWorkers, nullptr);
      for (int j = 0; j < i; ++j) {
        WaitForSingleObject(g_workers[j], 2000);
        CloseHandle(g_workers[j]);
        g_workers[j] = nullptr;
      }
      CloseHandle(g_sem);
      g_sem = nullptr;
      return false;
    }
  }
  g_workersUp = true;
  return true;
}

// Joins the planner threads (each finishes the build it is on). False when
// one did not exit in time (its plan memory must then stay).
bool StopWorkers() {
  if (!g_workersUp.load()) return true;
  g_workersUp = false;
  g_workersStop = true;
  ReleaseSemaphore(g_sem, kWorkers, nullptr);
  const DWORD w = WaitForMultipleObjects(kWorkers, g_workers, TRUE, 3000);
  if (w == WAIT_TIMEOUT || w == WAIT_FAILED) return false;
  for (HANDLE& h : g_workers) {
    CloseHandle(h);
    h = nullptr;
  }
  CloseHandle(g_sem);
  g_sem = nullptr;
  return true;
}

// Frees the per-cascade plans (no planner thread, pass or queue may run).
void FreePlans() {
  for (int k = 0; k < kMaxCascades; ++k) {
    Plan* p = g_plans[k].exchange(nullptr);
    if (!p) continue;
    p->~Plan();
    VirtualFree(p, 0, MEM_RELEASE);
  }
  g_learnedCount = 0;
}

// Both importers of RenderGraph::render are hooked: Visualizer.dll and
// SceneRenderer.dll (2.9.30 renders the scene through the latter; the hook
// on Visualizer's import alone never ran). Both slots hold GraphicsCore's
// function, so one original serves both.
bool InstallRenderHook() {
  if (g_renderSlot || g_renderSlot2) return true;
  HMODULE gc = GetModuleHandleW(L"GraphicsCore.dll");
  if (!gc) return false;
  void* fn = reinterpret_cast<void*>(GetProcAddress(gc, kRenderImport));
  if (!fn) return false;
  void** slots[2] = {};
  const wchar_t* mods[2] = {L"Visualizer.dll", L"SceneRenderer.dll"};
  for (int i = 0; i < 2; ++i) {
    HMODULE m = GetModuleHandleW(mods[i]);
    void** slot = m ? timercache::FindImport(m, "GraphicsCore.dll", kRenderImport) : nullptr;
    if (slot && SlotOriginal(slot) == fn) slots[i] = slot;
  }
  if (!slots[0] && !slots[1]) {
    Log("shadow plan: RenderGraph::render import not found or not GraphicsCore's; plans stay on the render thread");
    return false;
  }
  // One HookRender serves both slots, so both must forward to GraphicsCore's
  // function itself (no other hook chained in either).
  for (int i = 0; i < 2; ++i)
    if (slots[i] && *slots[i] != fn) slots[i] = nullptr;
  if (!slots[0] && !slots[1]) {
    Log("shadow plan: RenderGraph::render imports already hooked by something else; plans stay on the render thread");
    return false;
  }
  g_origRender = reinterpret_cast<RenderFn>(fn);
  void* orig = nullptr;
  if (slots[0] && HookSlot(slots[0], reinterpret_cast<void*>(&HookRender), &orig)) g_renderSlot = slots[0];
  if (slots[1] && HookSlot(slots[1], reinterpret_cast<void*>(&HookRender), &orig)) g_renderSlot2 = slots[1];
  return g_renderSlot || g_renderSlot2;
}

// ---- Leader draw ----
// The group base reaches the shader as posStructOffset (CB +0xfc =
// mat+0x18c, written by slot 5). Slot 5 copies it from [item+0xd4], so the
// first version swapped [item+0xd4] for the call. The shadow recorder
// (shadow_rec.h) reads [item+0xd4] on a worker thread while the render thread
// draws, so leaders now leave it alone (g_psoDirect): ShadowMapRenderable
// vt[1] is a plain tail jump to the material's slot 5 with (material, item,
// mesh) [V NGModel 0x443e0, bytes checked at install], and the leader calls
// slot 5's C copy with the base instead (shadowtex::DrawPso: the calls slot 5
// would make now with [item+0xd4] == base). Without the copy (shadow texture
// skip not installable, other code at 0x443e0) the swap stays (recorder off).
bool g_psoDirect = false;
constexpr uint8_t kSmrRenderBytes[] = {0x48, 0x8b, 0x51, 0x10, 0x48, 0x8b, 0x4a, 0x10, 0x4c, 0x8b, 0x82,
                                       0xc0, 0x00, 0x00, 0x00, 0x48, 0x8b, 0x01, 0x48, 0xff, 0x60, 0x28};

bool SmrRenderIsTailJump() {
  const uint32_t rva = reloc::Rva(hooksig::NG_ShadowMapRenderable_render, 0x443e0);
  uint8_t b[sizeof(kSmrRenderBytes)];
  return g_ng && rva && allocslab::ReadBytes(g_ng + rva, b, sizeof(b)) && memcmp(b, kSmrRenderBytes, sizeof(b)) == 0;
}

// Draws the whole group with one instanced draw. Returns false when it fell
// back before touching anything (the caller then draws the leader alone and
// the members draw themselves). Every change to DCS state is made inside the
// __try and undone in the __finally. Plain C (no unwinding objects).
bool LeaderRaw(void* self, void* ctx, const Slot& s, uint64_t* ret) {
  auto* item = *reinterpret_cast<uint8_t**>(static_cast<uint8_t*>(self) + 0x10);
  auto* mat = *reinterpret_cast<uint8_t**>(item + 0x10);
  auto* shader = *reinterpret_cast<uint8_t**>(mat + 0x30);
  const uint64_t tech = TechOf(mat);
  void* expectVs = nullptr;
  ID3D11VertexShader* ours = shadowinst::FindVsEx(shader, tech, 0, &expectVs);
  if (!ours || !expectVs || !g_ctx || !g_srv) return false;
  if (*g_rendererApi != static_cast<void*>(g_rendererObj) || *g_rendererObj != static_cast<void*>(g_rendererVtbl))
    return false;
  auto* techBegin = *reinterpret_cast<uint8_t**>(shader + 0xb0);
  auto* techObj = *reinterpret_cast<uint8_t**>(techBegin + (tech - 1) * 0x50 + 0x20);
  using PassFn = uint8_t*(__fastcall*)(void*, uint32_t);
  auto* fxpass = (*reinterpret_cast<PassFn**>(techObj))[7](techObj, 0);
  auto* vsBlock = *reinterpret_cast<uint8_t**>(fxpass + 0xc0);
  auto* dcsVs = *reinterpret_cast<ID3D11VertexShader**>(vsBlock + 0x18);
  if (static_cast<void*>(dcsVs) != expectVs) return false;  // not the VS our variant was built from
  const bool direct = g_psoDirect;
  const int mode = direct ? shadowtex::Slot5Mode() : shadowtex::kPsoNone;
  if (direct && mode == shadowtex::kPsoNone) return false;  // slot 5 holds foreign code: the group draws stock
  void* mesh = *reinterpret_cast<void**>(item + 0xc0);
  const uint32_t pso = *reinterpret_cast<uint32_t*>(item + 0xd4);
  bool psoSet = false, vsSet = false, srvSet = false, vtblSet = false;
  __try {
    if (!direct) {
      *reinterpret_cast<uint32_t*>(item + 0xd4) = s.base;
      psoSet = true;
    }
    *reinterpret_cast<ID3D11VertexShader**>(vsBlock + 0x18) = ours;
    vsSet = true;
    g_ctx->VSSetShaderResources(127, 1, &g_srv);
    srvSet = true;
    t_instances = s.count;
    *g_rendererObj = g_myVtbl;
    vtblSet = true;
    *ret = direct ? shadowtex::DrawPso(mat, item, mesh, s.base, mode) : instcount::g_orig(self, ctx);
  } __finally {
    if (vtblSet) *g_rendererObj = g_rendererVtbl;
    t_instances = 0;
    if (vsSet) {
      *reinterpret_cast<ID3D11VertexShader**>(vsBlock + 0x18) = dcsVs;
      g_ctx->VSSetShader(dcsVs, nullptr, 0);
    }
    if (srvSet) {
      ID3D11ShaderResourceView* none = nullptr;
      g_ctx->VSSetShaderResources(127, 1, &none);
    }
    if (psoSet) *reinterpret_cast<uint32_t*>(item + 0xd4) = pso;
  }
  return true;
}

int AvFilter(unsigned long code) {
  return code == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH;
}

void CaptureDepth() {
  if (!g_capture || g_depth) return;
  ID3D11DepthStencilView* dsv = nullptr;
  g_ctx->OMGetRenderTargets(0, nullptr, &dsv);
  if (dsv) {
    dsv->GetResource(&g_depth);
    dsv->Release();
  }
}

bool Override(void* self, void* ctx, uint64_t* ret) {
  if (GetCurrentThreadId() != g_renderThread || !g_planActive) return false;
  CaptureDepth();
  if (!g_batching) return false;
  Slot* s = g_cur ? FindSlot(*g_cur, self) : nullptr;
  if (!s || s->role == kSolo) return false;
  if (s->role == kMember) {
    if (g_cur->groupFailed[s->group]) return false;  // the leader drew alone: draw this one too
    if (s->mask) {
      auto* item = *reinterpret_cast<uint8_t**>(static_cast<uint8_t*>(self) + 0x10);
      g_replayed += shadowtex::ReplayMemberSkippedSets(*reinterpret_cast<uint8_t**>(item + 0x10), item, s->mask);
    }
    *ret = 0;
    return true;
  }
  bool done = false;
  __try {
    done = LeaderRaw(self, ctx, *s, ret);
  } __except (AvFilter(GetExceptionCode())) {
    done = false;
    Disable("access violation in the instanced leader draw");
  }
  if (done) return true;
  g_fallbacks++;
  g_cur->groupFailed[s->group] = 1;
  return false;  // the original draws the leader alone; the members follow
}

// ---- Verification ----
ID3D11Texture2D* CopyToStaging(ID3D11Resource* res) {
  ID3D11Texture2D* tex = nullptr;
  if (FAILED(res->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&tex)))) return nullptr;
  D3D11_TEXTURE2D_DESC d;
  tex->GetDesc(&d);
  tex->Release();
  if (d.SampleDesc.Count != 1) return nullptr;
  d.Usage = D3D11_USAGE_STAGING;
  d.BindFlags = 0;
  d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  d.MiscFlags = 0;
  ID3D11Device* dev = nullptr;
  g_ctx->GetDevice(&dev);
  ID3D11Texture2D* st = nullptr;
  if (dev) {
    dev->CreateTexture2D(&d, nullptr, &st);
    dev->Release();
  }
  if (st) g_ctx->CopyResource(st, res);
  return st;
}

// Bytes per texel of the depth formats a shadow map can use; 0 = unknown.
UINT TexelBytes(DXGI_FORMAT f) {
  switch (f) {
    case DXGI_FORMAT_R16_TYPELESS:
    case DXGI_FORMAT_D16_UNORM:
    case DXGI_FORMAT_R16_UNORM:
    case DXGI_FORMAT_R16_FLOAT:
      return 2;
    case DXGI_FORMAT_R32_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT:
    case DXGI_FORMAT_R32_FLOAT:
    case DXGI_FORMAT_R24G8_TYPELESS:
    case DXGI_FORMAT_D24_UNORM_S8_UINT:
    case DXGI_FORMAT_R24_UNORM_X8_TYPELESS:
      return 4;
    case DXGI_FORMAT_R32G8X24_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
    case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS:
      return 8;
    default:
      return 0;
  }
}

DXGI_FORMAT g_lastFormat = DXGI_FORMAT_UNKNOWN;

// Compares two staging copies texel by texel (mip 0 of every slice);
// returns mismatching texels, or -1 on error or an unknown format.
int64_t Compare(ID3D11Texture2D* a, ID3D11Texture2D* b, uint64_t* texels) {
  D3D11_TEXTURE2D_DESC d;
  a->GetDesc(&d);
  g_lastFormat = d.Format;
  const UINT bpt = TexelBytes(d.Format);
  if (bpt == 0) return -1;
  const size_t rowBytes = static_cast<size_t>(d.Width) * bpt;
  int64_t bad = 0;
  for (UINT slice = 0; slice < d.ArraySize; ++slice) {
    const UINT sub = D3D11CalcSubresource(0, slice, d.MipLevels);
    D3D11_MAPPED_SUBRESOURCE ma, mb;
    if (FAILED(g_ctx->Map(a, sub, D3D11_MAP_READ, 0, &ma))) return -1;
    if (FAILED(g_ctx->Map(b, sub, D3D11_MAP_READ, 0, &mb))) {
      g_ctx->Unmap(a, sub);
      return -1;
    }
    if (ma.RowPitch < rowBytes || mb.RowPitch < rowBytes) {
      g_ctx->Unmap(b, sub);
      g_ctx->Unmap(a, sub);
      return -1;
    }
    for (UINT y = 0; y < d.Height; ++y) {
      const auto* ra = static_cast<const uint8_t*>(ma.pData) + static_cast<size_t>(y) * ma.RowPitch;
      const auto* rb = static_cast<const uint8_t*>(mb.pData) + static_cast<size_t>(y) * mb.RowPitch;
      if (memcmp(ra, rb, rowBytes) == 0) continue;
      for (UINT x = 0; x < d.Width; ++x) bad += memcmp(ra + x * bpt, rb + x * bpt, bpt) != 0;
    }
    *texels += static_cast<uint64_t>(d.Width) * d.Height;
    g_ctx->Unmap(b, sub);
    g_ctx->Unmap(a, sub);
  }
  return bad;
}

int64_t CompareGuarded(ID3D11Texture2D* a, ID3D11Texture2D* b, uint64_t* texels) {
  __try {
    return Compare(a, b, texels);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return -1;
  }
}

void WrapInner(void* pass, void* ctx, shadowpass::ExecFn orig);

void Wrap(void* pass, void* ctx, shadowpass::ExecFn orig) {
  g_inWrap.fetch_add(1);
  if (g_shutdown.load() || g_disabled.load(std::memory_order_relaxed) || !g_on.load(std::memory_order_relaxed) ||
      g_state.load() != 1)
    orig(pass, ctx);
  else
    WrapInner(pass, ctx, orig);
  g_inWrap.fetch_sub(1);
}

void WrapInner(void* pass, void* ctx, shadowpass::ExecFn orig) {
  if (!g_renderThread) g_renderThread = GetCurrentThreadId();
  if (GetCurrentThreadId() != g_renderThread) return orig(pass, ctx);
  void** vec = shadowpass::CasterVector(pass, ctx);
  if (!vec) return orig(pass, ctx);
  if (g_async.load(std::memory_order_relaxed)) Learn(vec, ctx);
  Plan* p = PlanForPass(vec);
  if (!p) return orig(pass, ctx);
  g_passes++;
  g_cur = p;
  g_planActive = true;
  if (g_verify.load(std::memory_order_relaxed)) {
    // Stock run, copy; batched run, copy; compare.
    g_batching = false;
    g_capture = true;
    if (g_verifyStock) g_verifyStock(true);
    p->cursor = 0;
    orig(pass, ctx);
    g_capture = false;
    RestoreMats(*p);
    ID3D11Texture2D* a = g_depth ? CopyToStaging(g_depth) : nullptr;
    if (g_depth) {
      g_depth->Release();
      g_depth = nullptr;
    }
    g_batching = true;
    g_capture = true;
    if (g_verifyStock) g_verifyStock(false);
    p->cursor = 0;
    orig(pass, ctx);
    g_capture = false;
    RestoreMats(*p);
    ID3D11Texture2D* b = g_depth ? CopyToStaging(g_depth) : nullptr;
    if (g_depth) {
      g_depth->Release();
      g_depth = nullptr;
    }
    if (a && b) {
      uint64_t texels = 0;
      const int64_t bad = CompareGuarded(a, b, &texels);
      if (bad < 0) g_verifyErrors++;
      g_verifyPasses++;
      g_verifyPixels += texels;
      if (bad > 0) {
        g_verifyMismatch++;
        g_verifyBadPixels += static_cast<uint64_t>(bad);
        Disable("depth mismatch between the stock and the batched cascade");
      }
    }
    if (a) a->Release();
    if (b) b->Release();
  } else {
    g_batching = true;
    p->cursor = 0;
    orig(pass, ctx);
    RestoreMats(*p);
  }
  g_batching = false;
  g_planActive = false;
  g_cur = nullptr;
  ReleasePlan(p);
}

// The renderer vtable copy with DrawN at slot 35, shared by shadow and
// G-buffer batching (gb_batch.h). 1 = ready, -1 = this build does not match.
int g_rendererState = 0;
bool InstallRenderer() {
  if (g_rendererState != 0) return g_rendererState > 0;
  auto* dx = reinterpret_cast<uint8_t*>(GetModuleHandleW(L"dx11backend.dll"));
  HMODULE rmod = GetModuleHandleW(L"renderer.dll");
  if (!dx || !rmod) return false;  // retried later
  auto** api = reinterpret_cast<void***>(GetProcAddress(rmod, "?pRenderApi@RenderAPI@@3PEAUIRenderAPI@1@EA"));
  if (!api || !*api) return false;
  g_rendererState = -1;
  auto** obj = reinterpret_cast<void**>(*api);
  auto** vtbl = static_cast<void**>(*obj);
  // Recorded RVAs, or where reloc.h re-found them in another build (0: not
  // found; the vtable must then also have kVtblCopy entries).
  const uint32_t vtRva = reloc::Rva(hooksig::DX_DX11Renderer_vtbl, kRendererVtbl);
  const uint32_t drawRva = reloc::Rva(hooksig::DX_DX11Renderer_draw, kDrawRva);
  if (!vtRva || !drawRva || reinterpret_cast<uint8_t*>(vtbl) != dx + vtRva ||
      SlotOriginal(&vtbl[kDrawSlot]) != dx + drawRva) {
    Log("batching: DX11Renderer does not match this build; skipped");
    return false;
  }
  // The copy below has kVtblCopy entries: the entry after them must not be
  // code of dx11backend (in 2.9.30 it is string data; checked against the
  // shipped DLL). A relocated vtable of another build with more virtuals
  // would otherwise send a call to slot 92+ through whatever follows the copy.
  if (reloc::InCode(dx, static_cast<const uint8_t*>(vtbl[kVtblCopy]), 1)) {
    Log("batching: DX11Renderer's vtable is longer than %d entries in this build; skipped", kVtblCopy);
    return false;
  }
  g_rendererApi = api;
  g_rendererObj = obj;
  g_rendererVtbl = vtbl;
  g_origDraw = reinterpret_cast<DrawFn>(SlotOriginal(&g_rendererVtbl[kDrawSlot]));
  g_myTable[0] = g_rendererVtbl[-1];  // RTTI locator
  memcpy(g_myTable + 1, g_rendererVtbl, kVtblCopy * sizeof(void*));
  g_myVtbl = g_myTable + 1;
  g_myVtbl[kDrawSlot] = reinterpret_cast<void*>(&DrawN);
  g_rendererState = 1;
  return true;
}

// Dynamic structured buffer of uint offsets (t127) and its view.
bool CreateOffsetBuffer(ID3D11Device* dev, uint32_t count, ID3D11Buffer** buf, ID3D11ShaderResourceView** srv) {
  D3D11_BUFFER_DESC bd = {};
  bd.ByteWidth = count * sizeof(uint32_t);
  bd.Usage = D3D11_USAGE_DYNAMIC;
  bd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
  bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
  bd.StructureByteStride = sizeof(uint32_t);
  if (FAILED(dev->CreateBuffer(&bd, nullptr, buf))) return false;
  D3D11_SHADER_RESOURCE_VIEW_DESC sd = {};
  sd.Format = DXGI_FORMAT_UNKNOWN;
  sd.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
  sd.Buffer.FirstElement = 0;
  sd.Buffer.NumElements = count;
  if (FAILED(dev->CreateShaderResourceView(*buf, &sd, srv))) {
    (*buf)->Release();
    *buf = nullptr;
    return false;
  }
  return true;
}

bool Install() {
  if (g_state.load() != 0) return g_state.load() > 0;
  if (shadowinst::g_state.load() != 1 || !shadowinst::g_device) return false;  // not yet
  if (!instcount::Install()) return false;
  shadowpass::Install();
  if (!shadowpass::g_orig) return false;
  if (!InstallRenderer()) {
    if (g_rendererState < 0) g_state = -1;
    return false;
  }
  g_state = -1;
  // The context and buffer below are made on the payload worker: not allowed
  // on a device created D3D11_CREATE_DEVICE_SINGLETHREADED (shadow_inst moves
  // its own creates to the render thread for that case; both recorders refuse
  // it). DCS 2.9.30 creates a thread-safe device, so this never triggers there.
  if (shadowinst::g_deviceSingleThreaded) {
    Log("shadow batching: the D3D11 device is single-threaded; off for this session");
    return false;
  }
  g_ng = reinterpret_cast<uint8_t*>(GetModuleHandleW(L"NGModel.dll"));
  if (!g_ng) return false;
  const uint32_t smrRva = reloc::Rva(hooksig::NG_ShadowMapRenderable_vtbl, kSmrVtbl);
  const uint32_t matRva = reloc::Rva(hooksig::NG_ModelMaterialMT_vtbl, kModelMatVtbl);
  if (!smrRva || !matRva) return false;  // not found in this build: reloc.h logged it
  g_smrVtbl = g_ng + smrRva;
  g_modelMatVtbl = g_ng + matRva;
  shadowinst::g_device->GetImmediateContext(&g_ctx);
  if (!CreateOffsetBuffer(shadowinst::g_device, kMaxOffsets, &g_buf, &g_srv)) {
    g_ctx->Release();  // latched off (-1): Shutdown does not release it
    g_ctx = nullptr;
    return false;
  }
  // Leaders through slot 5's copy (no [item+0xd4] swap): the copy's build
  // checks (installing it does not attach the texture skip) and the tail jump.
  g_psoDirect = shadowtex::Install() && SmrRenderIsTailJump();
  instcount::g_override = &Override;
  shadowpass::g_wrap = &Wrap;
  g_state = 1;
  Log("shadow batching: ready (cascade execute and caster hooks, DX11Renderer instanced draw; group base %s)",
      g_psoDirect ? "passed to slot 5's copy, [item+0xd4] untouched" : "swapped into [item+0xd4] for the leader's call");
  // Planner threads (async plans): optional, the inline planner stays.
  if (InstallRenderHook() && StartWorkers())
    Log("shadow plan: hooked RenderGraph::render (imports:%s%s); %d planner threads", g_renderSlot ? " Visualizer" : "",
        g_renderSlot2 ? " SceneRenderer" : "", kWorkers);
  else
    Log("shadow plan: planner threads unavailable; plans stay on the render thread");
  return true;
}

// True when its objects were released (false: a pass or planner still runs;
// the caller then keeps shadow_inst's VS objects and results alive too).
bool Shutdown() {
  g_shutdown = true;
  g_on = false;
  g_async = false;
  shadowpass::g_wrap = nullptr;
  instcount::g_override = nullptr;
  if (g_renderSlot) {
    UnhookSlot(g_renderSlot, reinterpret_cast<void*>(g_origRender));  // g_origRender stays for calls in flight
    g_renderSlot = nullptr;
  }
  if (g_renderSlot2) {
    UnhookSlot(g_renderSlot2, reinterpret_cast<void*>(g_origRender));
    g_renderSlot2 = nullptr;
  }
  // Planner threads first (each finishes its build), then a pass in flight
  // (a verification pass can take a while) and a queue in flight.
  const bool joined = StopWorkers();
  for (int i = 0; i < 400 && (g_inWrap.load() != 0 || g_inQueue.load() != 0); ++i) Sleep(5);
  if (!joined || g_inWrap.load() != 0 || g_inQueue.load() != 0) {
    Log("shadow batching: a pass or planner thread was still running at unload; its objects are left alive");
    return false;
  }
  FreePlans();
  if (g_srv) g_srv->Release();
  if (g_buf) g_buf->Release();
  if (g_ctx) g_ctx->Release();
  g_srv = nullptr;
  g_buf = nullptr;
  g_ctx = nullptr;
  return true;
}

void LogCounters(const char* label) {
  const uint64_t p = g_passes.load();
  Log("  shadow batching %s: %llu passes, casters %llu, groups %llu, instanced draws %llu, casters skipped %llu, "
      "fallbacks %llu, oversized passes %llu%s; verify: %llu passes compared, %llu with differences, %llu of %llu texels differ, "
      "%llu compare errors (format %d)",
      label, static_cast<unsigned long long>(p), static_cast<unsigned long long>(g_casters.load()),
      static_cast<unsigned long long>(g_groups.load()), static_cast<unsigned long long>(g_leaders.load()),
      static_cast<unsigned long long>(g_skipped.load()), static_cast<unsigned long long>(g_fallbacks.load()),
      static_cast<unsigned long long>(g_tooLarge.load()), g_disabled.load() ? ", DISABLED" : "",
      static_cast<unsigned long long>(g_verifyPasses.load()), static_cast<unsigned long long>(g_verifyMismatch.load()),
      static_cast<unsigned long long>(g_verifyBadPixels.load()), static_cast<unsigned long long>(g_verifyPixels.load()),
      static_cast<unsigned long long>(g_verifyErrors.load()), static_cast<int>(g_lastFormat));
  Log("  shadow plan %s (%s): %llu from planner threads, %llu built on the render thread (%llu queued but not "
      "started), %llu planner builds; render-thread waits %llu (%.3f ms total, max %.3f ms, %llu timeouts); "
      "rebuilt on the render thread: %llu identity mismatches, %llu deferred mask builds, %llu planner faults",
      label, g_async.load() ? (g_workersUp.load() ? "async" : "async, no planner threads") : "inline",
      static_cast<unsigned long long>(g_planWorker.load()), static_cast<unsigned long long>(g_planRender.load()),
      static_cast<unsigned long long>(g_planStolen.load()), static_cast<unsigned long long>(g_planWorkerBuilt.load()),
      static_cast<unsigned long long>(g_planWaits.load()), g_planWaitUs.load() / 1000.0,
      g_planWaitMaxUs.load() / 1000.0, static_cast<unsigned long long>(g_planTimeouts.load()),
      static_cast<unsigned long long>(g_planMismatch.load()), static_cast<unsigned long long>(g_planDeferred.load()),
      static_cast<unsigned long long>(g_planFaults.load()));
  Log("  shadow plan queue %s: %llu render entries, %llu passed the gates, %llu without the vector, %llu queued "
      "nothing; %d cascades learned; last render graph %p, last learned graph %p",
      label, static_cast<unsigned long long>(g_qEntries.load()), static_cast<unsigned long long>(g_qGated.load()),
      static_cast<unsigned long long>(g_qNoVector.load()), static_cast<unsigned long long>(g_qNone.load()),
      g_learnedCount, g_qLastRg, g_qLastLearnRg);
}

void ResetCounters() {
  g_passes = g_casters = g_groups = g_leaders = g_skipped = 0;
  g_verifyPasses = g_verifyMismatch = g_verifyPixels = g_verifyBadPixels = g_verifyErrors = 0;
  g_planWorker = g_planRender = g_planStolen = g_planWorkerBuilt = 0;
  g_planWaits = g_planWaitUs = g_planWaitMaxUs = g_planTimeouts = 0;
  g_planMismatch = g_planDeferred = g_planFaults = 0;
  g_qEntries = g_qGated = g_qNoVector = g_qNone = 0;
}

}  // namespace shadowbatch
