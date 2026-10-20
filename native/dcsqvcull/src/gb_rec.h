// G-buffer recorder, R18 stages S1-S3: the G-buffer passes' ModelMaterialMT
// draws recorded on worker threads into D3D11 command lists in DCS's exact
// order and executed with ExecuteCommandList(TRUE) inside DCS's own G-buffer
// pass, DCS drawing every other item between the lists. [Model]
// GBufferRecorder (default 0), GBufferRecorderScope, GBufferRecorderWaitUs,
// GBufferRecorderIsland, GBufferRecorderMaxSegments, GBufferRecorderHelpers, [Suite]
// GBufferRecVerify (+Sec, +Stride), [Suite]
// BenchGBufferRecorder (bench mode 29).
//
// Tags: [V] verified in the binary (DCS 2.9.30: NGModel, dx11backend,
// GraphicsCore) or the shader sources, [I] inferred, [M] measured. Design and
// section numbers: docs/research/R18_gbuffer_recorder.md; the template is
// shadow_rec.h (R17 A1).
//
// What DCS does per G-buffer model draw [V NGModel, re-read 2026-10-10]:
//  SceneRenderable vt[1] 0x44350: pass [r+0x60] in {1,2} -> lock add
//  [globals+0x1e8], [r+0x78]; mat->vt[4] 0x16140(mat, item, &{1,1,1e5},
//  pass, mesh [item+0xc0], mesh2, &ctx r+0x64, &size r+0x6c):
//   mat+0x18c = [item+0xd4]; DX11Shader vt[27](h [mat+0x68], page buffer):
//   sbPositions; 0xeb60: per {dst, prop} at [mat+8, mat+0x10)
//   prop->vt[9](prop, &copy of item[0..16], dst) [V 0xeb80-0xeb97]; 0xcf80:
//   slot 26 per texture handle (streaming request vt[23](size), compat,
//   getSRV, SetResource); then 0x15e20: mat+0xb0..0xef = item+0x60..0x9c,
//   mat+0x110 = arg 3 (1.0), mat+0x114/0x118 = viewport / frame-buffer size
//   ratios (renderer vt[40], vt[54]) [V 0x15e2b-0x1600d]; pass 1 with
//   props+0x33 == 0 and ctx byte 0 == 0: technique [mat+0x1d8], pass index 0
//   [V 0x16019-0x16081]; the submit 0x131e0: [mat+0x1c0]->vt[2](mat+0x90,
//   0x130), shader vt[13](h [mat+0x70], buffer), indices, vertex stream,
//   technique, DX11Renderer::draw (FX Apply, input layout, DrawIndexed(3 *
//   [mesh+0x210])) [V 0x131fa-0x132e8]. The draw itself is the shadow
//   recorder's (same submit, same mesh fields): its mesh table and probe
//   checks are reused.
//
// This file:
//  - Scope ([Model] GBufferRecorderScope): bits 0-15 = G-buffer executions by
//    their ordinal since RenderGraph::render entry (the "#k" of the GPU pass
//    profile); up to 4 of them, one job each on a primary worker plus up to
//    2 helpers ([Model] GBufferRecorderHelpers). Bit 16 = also
//    BM_ALPHA_TEST (A2C) draws besides BM_NONE. Default 0x10004: execution #2
//    (the heaviest in the S0 GPU profile, 2.7 ms, and later in the frame than
//    #0, so its job has more lead), BM_NONE and A2C.
//  - Execution identity (R24 2): SceneRenderer adds per main viewport up to
//    three G-buffer passes, each with its own collection: SM_GBUFFER_PBR_COCKPIT
//    (4, F1 only, first), SM_GBUFFER_PBR (0) and SM_GBUFFER_PBR_DECAL (3); the
//    MFD sensor views (TV/FLIR/NVD, alternate frames) run the same view setup
//    before the main viewports [V SceneRenderer 0x39020, 0x22c8f, 0x23bdf]. So
//    plain ordinals shift in F1 and on MFD frames. A slot is bound to a
//    collection identity instead: (shading model, viewport tag, rank among
//    the frame's collections with both equal), read from the render graph's
//    collection descriptors [V GraphicsCore 0x56a60, 0x40a20, 0x50fa4]. The
//    scope bits keep their meaning: bit o = the o-th non-cockpit execution of
//    a base frame (one with no more executions than the frame before, seen
//    twice with the same sequence), which is the plain ordinal in F2. [Model]
//    GBufferRecorderByOrdinal=1: the plain ordinal (before R24).
//  - Cockpit ([Model] GBufferRecorderCockpit, default 0; R24 4): for each
//    scoped SM_GBUFFER_PBR execution, its view's cockpit execution in slot
//    s + 4, and cockpit items (renderable byte 0x64) recorded with the
//    normal_cockpit* technique [mat+0x1e0], P0 (stencil ref 40 from DCS's own
//    state, as every key) [V NGModel 0x16015-0x160b2: the per-draw writes are
//    the same, 0x190 is not written on the opaque pass-1 route]. A material
//    drawn both in and out of the cockpit in one vector stays DCS's. A
//    render-target texture of a segment must not be a target of the pass
//    (else the segment is stock); whether it was drawn to since its last mip
//    generation is counted at the exec entries (the writer-before-reader
//    order, R24 3.4).
//  - Recordable (S3): NGModel SceneRenderable items with ModelMaterialMT,
//    model pass 1, opaque (props+0x33 == 0), not cockpit (ctx byte 0), blend
//    mode in scope, key and mesh probed, sbPositions page view present, every
//    texture set in the texture table (usable, no swap due), every texture the
//    pass reads mapped to a PS slot by the key's probe. Everything else is
//    residual: DCS draws it in place, between our lists.
//  - Keys (DX11Shader, technique [mat+0x1d8], renderer+0xd4 flags): the VS
//    slots from the reflection of shadow_inst's (b) G-buffer VS (only
//    def_uniforms, sbPositions, cPerFrame b6 / cPerView b7 / cAmbientMap b8
//    and samplers allowed); the rest from DCS's own draw read back right after
//    its D3D draw (probe): VS, PS, states, samplers, PS slots holding the
//    material CB, the page view and each read texture (found by the view the
//    texture's FX variable holds after the draw). The pass's read names
//    (shadow_inst's (a) RDEF) must all be def_uniforms, the three context
//    buffers, sbPositions, samplers or the shader's texture records (the
//    reads table, per (shader, technique)).
//  - Texture table (R18 3.3): per (texture, aux, type) DCS's getSRV inputs
//    and one reference per view, built on the render thread (getDesc and
//    compat are DCS calls); workers pick a view with getSRV's own math from
//    the entry (no DCS call). Missing or unusable entries make the draw
//    residual and are (re)built after the pass. Changed only under an
//    exclusive SRW lock taken with TryAcquire on the render thread; a job
//    holds it shared from its first read until its lists are recorded.
//  - Jobs (workers): the vector is snapshotted; pass 1 decides per item and
//    marks per DX11Shader whether any of its items here inherits an FX
//    variable (a texture record the pass reads that the item does not set, or
//    no page buffer) or is not analysed: such a shader is drawn by DCS
//    entirely (R18 3.1, gb_batch's rule). Pass 2 checks keys, meshes, pages,
//    textures. Runs of recordable items shorter than the island ([Model]
//    GBufferRecorderIsland, default 30) stay residual; at most [Model]
//    GBufferRecorderMaxSegments (default 12) segments, the longest. Stage A
//    is split: the per-item reads of DCS memory in chunks on the primary and
//    the helpers, then a serial commit in vector order. The recorded draws are
//    cut by draw count into one run per recording worker; each run is one
//    command list per segment it covers, so a segment executes its lists one
//    after the other, in order. Each list: the frame buffer's
//    RTVs, DSV, viewports and scissors, our copies of b6-b8, then per draw
//    the key's objects, the mesh's input layout and buffers, the page view,
//    the read textures' views and the CB window, in vector order. CB window =
//    the material's def_uniforms bytes (snapshot), +0xfc = [item+0xd4], the
//    animated properties written by DCS's own vt[9] into the window, +0x20 =
//    item+0x60..0x9c, +0x80 = the 12 bytes DCS wrote at mat+0x110 in this
//    execution (learned from a stock draw, inputs checked at the pass).
//  - Pass (render thread): DCS's loop is handed [front entry, residual items
//    and one exec entry per segment, in order] ({begin, end} swapped for the
//    call, as shadow_rec.h S6). The front entry runs after the frame buffer,
//    clears and binder: it checks RTVs, DSV, viewports, scissors, renderer
//    flags, the context buffers and the ratio inputs against the job. An exec
//    entry replays its segment's streaming requests (vt[23] once per
//    (texture, size)), checks its textures (table entries against the live
//    textures), the guarded material dwords (what the key's stages read, minus
//    the per-draw overrides) and the page views, copies DCS's bound b6-b8 into
//    ours and executes the list; on any failure DCS draws that segment's
//    items itself right there (vt[1], in order). After the pass, for executed
//    segments: the triangle counter sum, each material whose last item was
//    recorded gets the bytes DCS's last draw leaves (0x18c, animated, 0xb0,
//    0x110), and each FX variable whose last writer was recorded is set to
//    that writer's value (texture view or page view).
// Verification ([Suite] GBufferRecVerify): the scoped execution runs twice in
// the same frame from the same target contents (gb_batch.h's verifier:
// targets captured at the front entry, restored between the runs, all 6 RTs
// and the DSV compared bit for bit); the first 3 compares stock vs stock
// (reproducibility), then stock vs recorded; any difference latches the
// recorder off. While verifying, the immediate state is read before and after
// each ExecuteCommandList and must be equal (R18 T9). [Suite]
// GBufferRecVerifyStride=N forces every N-th recordable draw residual and the
// island to N-1 (R18 T10).
//
// Fail-safe: a failed check makes the segment (or the execution) stock;
// DCS memory is read under SEH; a fault, a verify mismatch or a worker fault
// latches the recorder off for the session; off = pass-through. Requires
// G-buffer batching off, a device that is not SINGLETHREADED, driver command
// lists and constant-buffer offsets.
// Included once from main.cpp inside its anonymous namespace, after
// pass_timing.h, gb_count.h, shadow_inst.h, shadow_batch.h, gb_batch.h,
// split_filter.h, shadow_rec.h and gb_rec_count.h.
#pragma once

namespace gbrec {

// cockpit_ipd.h's ownship ([Perception] CockpitTrueIpdOwnship): an HMD view's world execution whose item vector
// it handed DCS's loop with its own switch entries. The recorder steps aside for exactly that execution (DCS draws
// it stock, over that vector; the slot stays bound and alive), and starts no job while the hint is set (an
// execution stepped aside within the last frames), so no worker records a pass it will not execute.
thread_local bool t_ownAside = false;
std::atomic<bool> g_ownAsideHint{false};
std::atomic<uint64_t> g_ownAsidePasses{0}, g_ownAsideJobsSkipped{0};
// Stage 2 (CockpitTrueIpdOwnship=2): the recorder keeps the execution. Jobs started while g_ownKey names the locked
// instance record its items in segments of their own (a run of ownship items never shares a segment with other
// items: PlanSegments splits at the change) and bracket each run of them in the handed list, segment exec entries
// and residual items alike, with cockpit_ipd.h's switch entries (g_ownInE / g_ownOutE: the true eye's cPerView in,
// DCS's back). An ownship segment reads the switched cPerView: ExecSegment copies the b6-b8 DCS has bound at its
// exec entry, i.e. after the switch-in. 0.5.9 left them residual (kROwnship): an execution that showed little but
// the ownship (F-14 rear view: ~85 of ~88 items) then had nothing recordable left and went stock as a whole, and the
// ownship's ~40-70 items per execution were drawn by DCS every frame, scale 1 included (live 2026-10-14). With
// g_ownRecSegs off (tests) they stay residual as in 0.5.9. t_own2: cockpit_ipd.h's GbBegin asks for the ownship at
// the true eye in this execution. The recorder uses its job only when the job was built for the same instance;
// otherwise (no job, another key, a stock path, verify's stock run) it hands the vector to g_ownSwapFn (stage 1:
// DCS's loop over the vector with the switch entries) and draws stock. All or nothing per execution, never a mix.
std::atomic<const void*> g_ownKey{nullptr};
std::atomic<bool> g_ownRecSegs{true};
void* g_ownInE = nullptr;
void* g_ownOutE = nullptr;
bool (*g_ownSwapFn)(void** vec) = nullptr;
thread_local bool t_own2 = false;
uint64_t g_ownSwIn = 0, g_ownSwBoundDiff = 0;  // cockpit_ipd.h's switch-ins; of a struct not bound at VS b7
void (*g_ownAwayFn)(bool on) = nullptr;  // cockpit_ipd.h: the ownship's camera moved far aside (verify's mask run)
// Verify's ownship mask (stage 2): runs, failures, compares classified, differing texels inside / outside it.
std::atomic<uint64_t> g_ownMaskRuns{0}, g_ownMaskFailed{0}, g_ownMaskCompares{0}, g_ownMaskIn{0}, g_ownMaskOut{0};  // cockpit_ipd.h's switch-ins done / without a source (verify diagnostics)
std::atomic<uint64_t> g_own2Live{0}, g_own2Fallback{0}, g_own2Items{0}, g_own2Runs{0}, g_own2NoKey{0};
std::atomic<uint64_t> g_own2RecItems{0};  // ownship items drawn from command lists in the executions kept
// Why an execution fell back to stage 1 (the sum is g_own2Fallback):
//  kOwnFbScope: not one of the recorder's executions (decal / A2C ordinals out of its scope, the recorder off or
//               on another thread, a contained fault): stage 1 by design, nothing recorded was lost;
//  kOwnFbStock: one of its executions, but drawn stock anyway (no job yet, late, failed, nothing recordable, item
//               vector changed, verify's stock run): stage 1 costs nothing extra there;
//  kOwnFbKey:   its job was ready but built for another instance (or none): the only case that drops a usable job.
enum : int { kOwnFbScope = 0, kOwnFbStock = 1, kOwnFbKey = 2 };
std::atomic<uint64_t> g_own2FbWhy[3] = {};
// Stage 2's fallback for this execution (render thread, before DCS's loop runs over vec): stage 1's vector.
void Own2Fallback(void** vec, int why = kOwnFbScope) {
  if (!t_own2) return;
  t_own2 = false;
  g_own2Fallback++;
  g_own2FbWhy[why >= 0 && why < 3 ? why : 0]++;
  if (vec && g_ownSwapFn) g_ownSwapFn(vec);
}

using shrec::Mix;
using shrec::SafeRel;

// ---------------------------------------------------------------------------
// Constants, scope, reasons
// ---------------------------------------------------------------------------
constexpr uint32_t kCbOff = 0x90, kCbLen = 0x130, kPsoCb = 0xfc;
constexpr uint32_t kMatrixCb = 0x20, kMatrixLen = 0x40;  // mat+0xb0..0xef = item+0x60..0x9c
constexpr uint32_t kRatioCb = 0x80, kRatioLen = 12;      // mat+0x110..0x11b
constexpr uint32_t kDbgBits = 0x1 | 0x4 | 0x10 | 0x20 | 0x40;  // renderer+0x2120: skips (0x20: no sbPositions set)
constexpr int kSlots = 8;       // slots s < 4: the scope's executions; s + 4: the cockpit execution of slot s's view
constexpr int kScopeSlots = 4;
constexpr int kMaxSegments = 16;          // array size; [Model] GBufferRecorderMaxSegments (default 12) caps a job
constexpr uint32_t kDefaultMaxSeg = 12;
constexpr uint32_t kDefaultIsland = 30;
constexpr size_t kMaxItems = 1 << 15;
constexpr int kThreads = 3;               // per job: the primary worker and up to 2 helpers (stage A and recording)
constexpr uint32_t kTSets = 1 << 15;      // texture sets of the candidate draws, per stage-A thread
constexpr uint32_t kTViews = 1 << 15;     // PS views of the candidate draws, per stage-A thread
constexpr uint32_t kTMisses = 2048, kTRefresh = 1024, kTDedupe = 4096;  // per thread
constexpr uint32_t kPreMatTable = 1 << 12;
constexpr uint32_t kPreChunk = 32;        // items per stage-A chunk
constexpr double kPreWaitUs = 20000.0;    // the primary waits this long for a helper's last chunk
constexpr uint32_t kSplitMinDraws = 192;  // fewer recorded draws: one worker records them all
constexpr int kMaxPieces = kMaxSegments + kThreads;
constexpr uint32_t kMaxTex = 32;           // texture handles per material (mat+0x2d8)
constexpr int kMaxPsTex = 12;              // texture records a key's pass reads
constexpr uint32_t kRecWords = 8;          // 512 texture records per shader
constexpr uint32_t kMaxRecords = 64 * kRecWords;
constexpr uint32_t kMaxAnim = 32;
constexpr uint32_t kMatTable = 1 << 14;    // powers of two
constexpr uint32_t kMaxMats = 1 << 13;
constexpr uint32_t kShaderTable = 1 << 12;
constexpr uint32_t kMaxShaders = 1 << 11;
constexpr uint32_t kDedupe = 1 << 16;
constexpr uint32_t kWantTable = 512;  // power of two
constexpr uint32_t kMeshCache = 1 << 13;
constexpr uint32_t kMaxWants = 64;
constexpr uint32_t kMaxReadWants = 64;
constexpr uint32_t kMaxMisses = 4096;
constexpr uint32_t kMaxRefresh = 4096;
constexpr uint32_t kMaxSegEnts = 1 << 16;
constexpr uint32_t kMaxSegPairs = 1 << 16;
constexpr uint32_t kMaxSegMats = 1 << 14;
constexpr uint32_t kMaxSegPages = 4096;
constexpr uint32_t kMaxRestores = 1 << 15;
constexpr int kProbesPerPass = 64;
constexpr size_t kKeyTable = 4096, kReadsTable = 4096, kMeshTable = 32768;  // powers of two
constexpr UINT kVp = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
constexpr UINT kSrvSlots = D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT;
constexpr UINT kSampSlots = D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT;
// DX11StateManager's sampler pool, bound at s5-s15 on every stage (samplers11.hlsl USE_SAMPLERSTATEPOOL)
// [V R19 2.3]. Per execution: read at the front entry, checked at every exec entry, bound in each list's prologue.
constexpr UINT kPoolFirst = 5;
constexpr uint32_t kPoolMask = ((1u << kSampSlots) - 1) & ~((1u << kPoolFirst) - 1);
constexpr UINT kCbSlots = D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT;
constexpr UINT kRtv = D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT;
constexpr UINT kCtx0 = 6;  // b6 cPerFrame, b7 cPerView, b8 cAmbientMap [V common/context.hlsl, AmbientCube.hlsl]
constexpr uint8_t kNoSeg = 0xff;
shrec::WorkLoad g_load;  // this recorder's worker regions (shadow_rec.h)
shrec::ClassCensus g_census;  // texture classes the table refused (shadow_rec.h)

// [Model] GBufferRecorderScope: bits 0-15 = executions by ordinal, bit 16 = A2C.
enum : uint32_t { kScopeExec = 0xffff, kScopeA2c = 1u << 16 };
constexpr uint32_t kDefaultScope = (1u << 2) | kScopeA2c;

// The scoped ordinal of slot s (the s-th set bit, at most kScopeSlots), or -1.
inline int OrdinalOfSlot(uint32_t scope, int s) {
  if (s < 0 || s >= kScopeSlots) return -1;
  int k = 0;
  for (int o = 0; o < 16; ++o)
    if (scope & (1u << o)) {
      if (k == s) return o;
      ++k;
    }
  return -1;
}
// The slot of ordinal o under scope, or -1.
inline int SlotOfOrdinal(uint32_t scope, int o) {
  if (o < 0 || o > 15 || !(scope & (1u << o))) return -1;
  int k = 0;
  for (int b = 0; b < o; ++b) k += (scope >> b) & 1;
  return k < kScopeSlots ? k : -1;
}

// ---- Execution identity (R24 2) ----
// GraphicsCore's render graph: per collection k (the item vector [rg+0x438] +
// k * 24, the resource node with word +0x38 = k) one descriptor at
// [rg+0x408] + k * 0x6d8: the shading model (word +0), the scene context
// (+0x10, 0x588 bytes) and the viewport tag (dword +0x690) given to
// addGBufferPass [V 0x56a60 pushes node and descriptor together; 0x40a20
// copies +0 and +0x690; the collection dump 0x50fa4 reads node k, descriptor
// k and vector k together].
constexpr uint32_t kCollDescs = 0x408, kCollStride = 0x6d8, kCollTag = 0x690;
constexpr uint16_t kSmGbuffer = 0, kSmDecal = 3, kSmCockpit = 4;  // SM_GBUFFER_PBR, _DECAL, _COCKPIT [V string table]
struct CollKey {
  uint16_t sm = 0, rank = 0;  // rank: earlier descriptors with the same model and tag
  uint32_t tag = 0;
  bool ok = false;
  const void* vol = nullptr;  // the collection's ClippingVolume (+0x8; not part of the identity)
  uint16_t vrank = 0;         // earlier descriptors with the same model and volume
};
inline bool SameKey(const CollKey& a, const CollKey& b) {
  return a.ok == b.ok && (!a.ok || (a.sm == b.sm && a.tag == b.tag && a.rank == b.rank));
}
inline uint32_t CollHash(const CollKey& k) {
  return k.ok ? (static_cast<uint32_t>(k.sm) << 24 ^ static_cast<uint32_t>(k.rank) << 16 ^ k.tag * 0x9E3779B1u) | 1u
              : 0u;
}
// The key of descriptor idx of an array of count descriptors. Plain.
inline bool CollKeyAt(const uint8_t* descs, size_t count, uint32_t idx, CollKey* out) {
  *out = CollKey();
  if (!descs || idx >= count) return false;
  const uint8_t* d = descs + static_cast<size_t>(idx) * kCollStride;
  out->sm = *reinterpret_cast<const uint16_t*>(d);
  out->tag = *reinterpret_cast<const uint32_t*>(d + kCollTag);
  out->vol = *reinterpret_cast<const void* const*>(d + 8);
  for (uint32_t k = 0; k < idx; ++k) {
    const uint8_t* e = descs + static_cast<size_t>(k) * kCollStride;
    if (*reinterpret_cast<const uint16_t*>(e) != out->sm) continue;
    if (*reinterpret_cast<const uint32_t*>(e + kCollTag) == out->tag) ++out->rank;
    if (*reinterpret_cast<const void* const*>(e + 8) == out->vol) ++out->vrank;
  }
  out->ok = true;
  return true;
}
// Descriptor idx has key `want` (cheap fields first). Plain.
inline bool CollIsAt(const uint8_t* descs, size_t count, uint32_t idx, const CollKey& want) {
  if (!want.ok || !descs || idx >= count) return false;
  const uint8_t* d = descs + static_cast<size_t>(idx) * kCollStride;
  if (*reinterpret_cast<const uint16_t*>(d) != want.sm || *reinterpret_cast<const uint32_t*>(d + kCollTag) != want.tag)
    return false;
  CollKey k;
  return CollKeyAt(descs, count, idx, &k) && k.rank == want.rank;
}
// The index of the descriptor with key `want`, or -1. Plain.
inline int CollFindIn(const uint8_t* descs, size_t count, const CollKey& want) {
  if (!want.ok || !descs) return -1;
  uint32_t rank = 0;
  for (size_t k = 0; k < count && k <= 0xffff; ++k) {
    const uint8_t* e = descs + k * kCollStride;
    if (*reinterpret_cast<const uint16_t*>(e) != want.sm || *reinterpret_cast<const uint32_t*>(e + kCollTag) != want.tag)
      continue;
    if (rank++ == want.rank) return static_cast<int>(k);
  }
  return -1;
}
// The view's collection by (model, ClippingVolume, vrank): the volume is the
// view object's own (`addGBufferPass`'s ClippingVolume& is a member of the
// viewport [V SR 0x3abc3: r12+0x30]), stable across frames while collection
// indexes move when other views add collections (R24 13). Index or -1. Plain.
inline int CollFindVolIn(const uint8_t* descs, size_t count, uint16_t sm, const void* vol, uint16_t vrank) {
  if (!descs || !vol) return -1;
  uint32_t r = 0;
  for (size_t k = 0; k < count && k <= 0xffff; ++k) {
    const uint8_t* e = descs + k * kCollStride;
    if (*reinterpret_cast<const uint16_t*>(e) != sm || *reinterpret_cast<const void* const*>(e + 8) != vol) continue;
    if (r++ == vrank) return static_cast<int>(k);
  }
  return -1;
}

// The render graph's descriptor array. Plain.
inline const uint8_t* CollDescs(const void* rg, size_t* count) {
  const uint8_t* b = *reinterpret_cast<const uint8_t* const*>(static_cast<const uint8_t*>(rg) + kCollDescs);
  const uint8_t* e = *reinterpret_cast<const uint8_t* const*>(static_cast<const uint8_t*>(rg) + kCollDescs + 8);
  *count = b && e > b && (e - b) % kCollStride == 0 ? static_cast<size_t>(e - b) / kCollStride : 0;
  return *count ? b : nullptr;
}

// The scope's executions as identities. A frame's G-buffer executions in
// order: the non-cockpit ones by position (the scope bits' ordinals), the
// cockpit ones apart. A base frame has no more non-cockpit executions than
// the frame before (MFD frames add theirs); its sequence binds the slots once
// two base frames in a row agree.
constexpr int kSeqMax = 16, kSeqCockpit = 8;
struct FrameSeq {
  uint32_t n = 0, nck = 0;  // non-cockpit and cockpit executions (all, beyond the arrays too)
  CollKey id[kSeqMax];
  CollKey ck[kSeqCockpit];
};
inline void SeqAdd(FrameSeq& s, const CollKey& k) {
  if (k.ok && k.sm == kSmCockpit) {
    if (s.nck < static_cast<uint32_t>(kSeqCockpit)) s.ck[s.nck] = k;
    ++s.nck;
    return;
  }
  if (s.n < static_cast<uint32_t>(kSeqMax)) s.id[s.n] = k;
  ++s.n;
}
inline bool SameSeq(const FrameSeq& a, const FrameSeq& b) {
  if (a.n != b.n || a.nck != b.nck) return false;
  for (uint32_t k = 0; k < a.n && k < static_cast<uint32_t>(kSeqMax); ++k)
    if (!SameKey(a.id[k], b.id[k])) return false;
  for (uint32_t k = 0; k < a.nck && k < static_cast<uint32_t>(kSeqCockpit); ++k)
    if (!SameKey(a.ck[k], b.ck[k])) return false;
  return true;
}
struct Binder {
  FrameSeq cur, base;
  uint32_t prevN = 0, stable = 0;
  uint64_t frames = 0, baseFrames = 0, extraFrames = 0, commits = 0;
};
// The frame in b.cur ended. True when b.base is a confirmed base sequence
// (the slots are (re)bound from it). b.cur is cleared.
inline bool EndFrame(Binder& b) {
  bool commit = false;
  if (b.cur.n || b.cur.nck) {
    ++b.frames;
    const bool isBase = b.cur.n > 0 && (b.prevN == 0 || b.cur.n <= b.prevN);
    b.prevN = b.cur.n;
    if (isBase) {
      ++b.baseFrames;
      if (b.stable && SameSeq(b.cur, b.base)) {
        if (b.stable < 0xffff) ++b.stable;
      } else {
        b.base = b.cur;
        b.stable = 1;
      }
      commit = b.stable >= 2;
    } else {
      ++b.extraFrames;
    }
  }
  b.cur = FrameSeq();
  if (commit) ++b.commits;
  return commit;
}
// Identity is ambiguous when two executions of a frame share model and tag
// (the tag is not per view in every scene: a user mission showed all four
// main views with tag 0x1f4, R24 11). Then the scope falls back to plain
// ordinals.
inline bool SeqAmbiguous(const FrameSeq& s) {
  const uint32_t n = s.n < static_cast<uint32_t>(kSeqMax) ? s.n : static_cast<uint32_t>(kSeqMax);
  for (uint32_t a = 0; a < n; ++a)
    for (uint32_t b = a + 1; b < n; ++b)
      if (s.id[a].ok && s.id[b].ok && s.id[a].sm == s.id[b].sm && s.id[a].tag == s.id[b].tag) return true;
  const uint32_t m = s.nck < static_cast<uint32_t>(kSeqCockpit) ? s.nck : static_cast<uint32_t>(kSeqCockpit);
  for (uint32_t a = 0; a < m; ++a)
    for (uint32_t b = a + 1; b < m; ++b)
      if (s.ck[a].ok && s.ck[b].ok && s.ck[a].tag == s.ck[b].tag) return true;
  return false;
}
// The G-buffer graph to follow: a new graph replaces the followed one once
// that one had no render entry for staleTicks (DCS rebuilt its render graph,
// e.g. another mission: the old pointer never renders again, so the frame
// never ended and the ordinal never reset; R24 11). True when it changed.
inline bool FollowGraph(void*& followed, int64_t followedEntry, void* rg, int64_t now, int64_t staleTicks) {
  if (!rg || rg == followed) return false;
  if (followed && now - followedEntry < staleTicks) return false;
  followed = rg;
  return true;
}
// A bound slot whose execution has not run for staleTicks while other G-buffer
// executions did (its identity matches nothing any more).
inline bool SlotStarved(bool bound, int64_t lastPass, int64_t boundAt, int64_t now, int64_t staleTicks,
                        bool executionsRan) {
  if (!bound || !executionsRan) return false;
  const int64_t last = lastPass > boundAt ? lastPass : boundAt;
  return now - last > staleTicks;
}

// ---- Quad-view executions (R24 12) ----
// The culling hook classifies each collect call's collections (the same
// descriptors as the render graph's) into the quad views' peripheral/focus
// pairs and publishes their ClippingVolumes (every pass of one view shares
// its volume). An execution of another view (an F-4's extra views: 68
// collections and 12 G-buffer executions per frame in the user's scene, or an
// MFD sensor view) is then not counted: scope bit o is the o-th non-cockpit
// execution of a quad view, as in F2 without extra views. Without a
// classification (no quad session) every execution counts, as before.
constexpr int kQuadVols = 16;
struct QuadSet {
  const void* vol[kQuadVols] = {};
  int n = 0;
  bool Has(const void* v) const {
    for (int i = 0; i < n; ++i)
      if (vol[i] == v) return true;
    return false;
  }
};
// An execution counts toward the ordinals: no set, its key unread, or one of the set's volumes.
inline bool CountsAsQuad(const QuadSet& q, const CollKey& k) { return q.n == 0 || !k.ok || q.Has(k.vol); }
// Seqlock-published set (writer: the culling thread; reader: the render thread at its render entry).
std::atomic<uint32_t> g_qvSeq{0};
std::atomic<int64_t> g_qvQpc{0};
QuadSet g_qvPub;
inline void PublishQuadVolumes(const void* const* vols, int n, int64_t qpc) {
  const uint32_t s0 = g_qvSeq.load(std::memory_order_relaxed);
  g_qvSeq.store(s0 + 1, std::memory_order_release);
  std::atomic_thread_fence(std::memory_order_release);
  g_qvPub.n = 0;
  for (int i = 0; i < n && g_qvPub.n < kQuadVols; ++i)
    if (vols[i] && !g_qvPub.Has(vols[i])) g_qvPub.vol[g_qvPub.n++] = vols[i];
  g_qvQpc.store(qpc, std::memory_order_relaxed);
  g_qvSeq.store(s0 + 2, std::memory_order_release);
}
// The published set if it is at most maxAgeTicks old (else empty: count every execution).
inline QuadSet ReadQuadVolumes(int64_t now, int64_t maxAgeTicks) {
  QuadSet q;
  for (int tries = 0; tries < 4; ++tries) {
    const uint32_t s0 = g_qvSeq.load(std::memory_order_acquire);
    if (s0 & 1) continue;
    q = g_qvPub;
    const int64_t at = g_qvQpc.load(std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_acquire);
    if (g_qvSeq.load(std::memory_order_relaxed) != s0) continue;
    if (!at || now - at > maxAgeTicks) q = QuadSet();
    return q;
  }
  return QuadSet();
}

// The identity of each slot under scope from a base sequence: s < 4 the
// scope's ordinals; s + 4 (cockpit on) the cockpit execution with the same
// tag and rank as slot s's SM_GBUFFER_PBR one.
inline void BindSlots(uint32_t scope, bool cockpit, const FrameSeq& base, CollKey out[kSlots]) {
  for (int s = 0; s < kSlots; ++s) out[s] = CollKey();
  for (int s = 0; s < kScopeSlots; ++s) {
    const int o = OrdinalOfSlot(scope, s);
    if (o < 0 || o >= kSeqMax || static_cast<uint32_t>(o) >= base.n) continue;
    out[s] = base.id[o];
    if (!cockpit || !out[s].ok || out[s].sm != kSmGbuffer) continue;
    for (uint32_t k = 0; k < base.nck && k < static_cast<uint32_t>(kSeqCockpit); ++k)
      if (base.ck[k].ok && base.ck[k].tag == out[s].tag && base.ck[k].rank == out[s].rank) out[s + kScopeSlots] = base.ck[k];
  }
}

enum ItemReason : int {
  kRecorded = 0,
  kRNotModel,
  kRPass,
  kRTransparent,
  kRCockpit,
  kRNoShader,
  kRBlend,
  kRMatExcluded,
  kRAnim,
  kRUnclean,
  kRNoCompile,
  kRKeyPending,
  kRKeyRejected,
  kRKeyCooling,
  kRMeshPending,
  kRMeshRejected,
  kRMeshChanged,
  kRPage,
  kRTexMiss,
  kRTexUnusable,
  kRTexSlot,
  kRTooMany,
  kRForced,
  kRIsland,
  kRSegments,
  kRCockpitMix,
  kROwnship,
  kReasons
};
const char* const kReasonName[kReasons] = {
    "recorded",
    "not a ModelMaterialMT SceneRenderable (terrain, shells, other materials)",
    "model pass number is not 1",
    "transparent material",
    "cockpit technique",
    "no DX11Shader",
    "blend mode not in scope",
    "its material has an item of another pass in this vector",
    "an animated property not replicable",
    "its DX11Shader has an item here that inherits an FX variable, or one not analysed yet",
    "key not compiled by shadow inst yet",
    "key not probed yet",
    "key not recordable",
    "its key's state varied between draws: relearnt after a cooldown",
    "mesh not probed yet",
    "mesh not recordable",
    "mesh differs from its probe",
    "no sbPositions view for the page",
    "a texture not in the texture table yet",
    "a texture not usable (class, swap due, mip-set shape)",
    "a read texture without a PS slot in its key",
    "a job table is full",
    "forced residual (verify stride)",
    "in a run shorter than the island",
    "beyond the segment limit",
    "its material is drawn both in and out of the cockpit here",
    "the ownship (stage 2: DCS draws it between the lists, at the true eye)"
};

// Per execution: every segment executed, or why the whole execution was stock.
enum PassReason : int {
  kPExecuted = 0,
  kPNoJob,
  kPLate,
  kPFailed,
  kPNothing,
  kPIdentity,
  kPTarget,
  kPFlags,
  kPCtx,
  kPRatio,
  kPPool,
  kPNoEntry,
  kPVerifyAa,
  kPVerifyCapture,
  kPOwnship,
  kPassReasons
};
const char* const kPassReasonName[kPassReasons] = {
    "executed",
    "no job (execution or target not learned yet, or no render entry)",
    "job still running at the pass",
    "job failed or faulted",
    "nothing recordable",
    "item vector changed since the job read it",
    "render targets, depth target, viewports or scissors differ",
    "renderer flags differ (rasterizer index or debug bits)",
    "context buffers b6-b8 missing or of another size",
    "frame-buffer or viewport size differs from the learned ratios",
    "the sampler pool s5-s15 differs from the learned one",
    "the front entry did not run",
    "verify: stock vs stock run",
    "verify: targets not captured",
    "ownship stage 2: the job was not built for the locked instance (stage 1 for this execution)"};

// Per segment of an execution that passed the front checks.
enum SegReason : int {
  kSExecuted = 0,
  kSTexture,
  kSGuard,
  kSPage,
  kSCtx,
  kSPool,
  kSFlags,
  kSRedo,
  kSFault,
  kSRtBound,
  kSegReasons
};
const char* const kSegReasonName[kSegReasons] = {
    "executed", "a texture's view changed or a swap is due", "a material's shader-read CB dwords changed",
    "a page's sbPositions view changed", "a context buffer changed", "the sampler pool s5-s15 changed",
    "renderer flags (rasterizer index or debug bits) changed",
    "a texture changed and the re-recorded list was not ready", "fault in the checks",
    "a render-target texture of the segment is a target of the pass"};

// ---------------------------------------------------------------------------
// Pure helpers (offline tested)
// ---------------------------------------------------------------------------
// The pool slots s5-s15 of two sampler arrays are the same objects.
inline bool PoolSame(const void* const* a, const void* const* b) {
  for (UINT q = kPoolFirst; q < kSampSlots; ++q)
    if (a[q] != b[q]) return false;
  return true;
}
// The sampler slots a key binds itself (below the pool): its FX dependencies,
// or every slot when those were not read.
inline uint32_t KeyOwnSamplers(uint32_t deps) { return deps & ~kPoolMask; }

inline void MarkDwords(uint64_t m[2], uint32_t off, uint32_t size) {
  for (uint32_t d = off / 4; d < (off + size + 3) / 4 && d < kCbLen / 4; ++d) m[d >> 6] |= 1ull << (d & 63);
}
// The per-draw writes of 0x16140/0x15e20 (pass 1 opaque) besides the animated ones.
inline void BaseOverrides(uint64_t m[2]) {
  MarkDwords(m, kPsoCb, 4);
  MarkDwords(m, kMatrixCb, kMatrixLen);
  MarkDwords(m, kRatioCb, kRatioLen);
}
inline void AllDwords(uint64_t m[2]) {
  m[0] = ~0ull;
  m[1] = (1ull << (kCbLen / 4 - 64)) - 1;
}

// Runs of recordable items (flag != 0) in vector order; a run is items of
// the same flag value (stage 2's ownship items carry 2, the others 1: the two
// never share a segment). Runs shorter than island are cleared; at most
// maxSeg runs are kept (the longest, the earlier on equal length). Returns
// the runs kept as [first, end) pairs in vector order; out must hold maxSeg
// pairs. Cleared items get the reason (island or segment limit) in why[]
// (1 = island, 2 = limit).
inline int PlanSegments(const uint8_t* flag, size_t n, uint32_t island, int maxSeg, uint32_t (*out)[2],
                        uint8_t* why) {
  struct Run {
    uint32_t a, b;
  };
  std::vector<Run> runs;
  size_t i = 0;
  while (i < n) {
    if (!flag[i]) {
      ++i;
      continue;
    }
    size_t e = i;
    while (e < n && flag[e] == flag[i]) ++e;
    if (e - i >= island) {
      runs.push_back({static_cast<uint32_t>(i), static_cast<uint32_t>(e)});
    } else {
      for (size_t k = i; k < e; ++k) why[k] = 1;
    }
    i = e;
  }
  if (static_cast<int>(runs.size()) > maxSeg) {
    std::vector<size_t> idx(runs.size());
    for (size_t k = 0; k < idx.size(); ++k) idx[k] = k;
    std::stable_sort(idx.begin(), idx.end(),
                     [&](size_t x, size_t y) { return runs[x].b - runs[x].a > runs[y].b - runs[y].a; });
    std::vector<uint8_t> keep(runs.size(), 0);
    for (int k = 0; k < maxSeg; ++k) keep[idx[k]] = 1;
    std::vector<Run> kept;
    for (size_t k = 0; k < runs.size(); ++k) {
      if (keep[k]) {
        kept.push_back(runs[k]);
      } else {
        for (uint32_t q = runs[k].a; q < runs[k].b; ++q) why[q] = 2;
      }
    }
    runs.swap(kept);
  }
  for (size_t k = 0; k < runs.size(); ++k) {
    out[k][0] = runs[k].a;
    out[k][1] = runs[k].b;
  }
  return static_cast<int>(runs.size());
}

// The texture table lives in shadow_rec.h (both recorders).
using shrec::kTexViews;
using shrec::kTeEmpty;
using shrec::kTeLive;
using shrec::kTeTomb;
using shrec::kTmFixed;
using shrec::kTmAreas;
using shrec::TexWhy;
using shrec::kTwOk;
using shrec::kTwClass;
using shrec::kTw678;
using shrec::kTwMipSet;
using shrec::kTwSwap;
using shrec::kTwFault;
using shrec::kTwCount;
using shrec::kTexWhyName;
using shrec::TexEntry;
using shrec::TexTable;
using shrec::TexHash;
using shrec::TexFind;
using shrec::TexReleaseViews;
using shrec::TexInsert;
using shrec::TexBuildRaw;
using shrec::TexBuildGuarded;
using shrec::TexCheckRaw;
using shrec::TexPick;
using shrec::TexSwapDueNow;
using shrec::TexSwapDueGuarded;
using shrec::TexWipe;
using shrec::TexEvict;
using shrec::AllocTex;
using shrec::FreeTex;
using shrec::ReplayVt18;

// ---------------------------------------------------------------------------
// Reads table: the texture records a (shader, technique) pass 0 reads
// ---------------------------------------------------------------------------
struct ReadsEntry {
  std::atomic<void*> shader{nullptr};
  uint64_t tech = 0;
  void* effect = nullptr;
  void* techBegin = nullptr;
  int state = 0;  // 1: mask valid; -1: the pass binds something unmapped or was not analysed
  const char* why = nullptr;
  uint64_t mask[kRecWords] = {};
  uint32_t count = 0;
  mutable std::atomic<uint32_t> lastUse{0};  // render entry of the last job lookup (compaction age)
};

// ---------------------------------------------------------------------------
// Keys: (DX11Shader, technique, renderer flags)
// ---------------------------------------------------------------------------
struct VsStatic {
  const char* why = nullptr;  // nullptr = the VS reads only what the recorder binds
  UINT cbSlot = ~0u, sbSlot = ~0u;
  uint32_t ctxMask = 0;  // bit s: the VS reads a context buffer at b<s> (6..8)
};

// The VS bindings of shadow_inst's (b) G-buffer blob (CompareVariants: equal
// to (a)'s but for qvInstOffsets).
inline const char* AnalyseGbVs(decltype(&D3DReflect) reflect, const void* code, size_t n, const char* cbName,
                               VsStatic& out) {
  out = VsStatic();
  ID3D11ShaderReflection* r = nullptr;
  if (!reflect || !code || !cbName ||
      FAILED(reflect(code, n, __uuidof(ID3D11ShaderReflection), reinterpret_cast<void**>(&r))) || !r)
    return out.why = "D3DReflect failed";
  D3D11_SHADER_DESC d = {};
  r->GetDesc(&d);
  const char* why = nullptr;
  static const char* const kCtxNames[3] = {"cPerFrame", "cPerView", "cAmbientMap"};
  for (UINT i = 0; i < d.BoundResources && !why; ++i) {
    D3D11_SHADER_INPUT_BIND_DESC b = {};
    r->GetResourceBindingDesc(i, &b);
    const bool one = b.BindCount == 1;
    if (b.Type == D3D_SIT_CBUFFER && strcmp(b.Name, cbName) == 0 && one) {
      out.cbSlot = b.BindPoint;
    } else if (b.Type == D3D_SIT_CBUFFER) {
      int k = 0;
      while (k < 3 && strcmp(b.Name, kCtxNames[k]) != 0) ++k;
      if (k == 3 || b.BindPoint != kCtx0 + k || !one) why = "the VS reads another constant buffer";
      else out.ctxMask |= 1u << b.BindPoint;
    } else if (b.Type == D3D_SIT_STRUCTURED && strcmp(b.Name, "sbPositions") == 0 && one) {
      out.sbSlot = b.BindPoint;
    } else if (b.Type == D3D_SIT_STRUCTURED && strcmp(b.Name, "qvInstOffsets") == 0 && b.BindPoint == 127) {
      // (b) only
    } else if (b.Type == D3D_SIT_SAMPLER) {
      // samplers: every slot is copied from DCS's draw
    } else {
      why = "the VS reads a texture or another buffer";
    }
  }
  if (!why && out.cbSlot == ~0u) why = "the VS reads no def_uniforms";
  if (!why && out.sbSlot == ~0u) why = "the VS reads no sbPositions";
  if (!why && out.cbSlot >= kCtx0 && out.cbSlot < kCtx0 + 3) why = "def_uniforms is bound at a context slot";
  r->Release();
  return out.why = why;
}

struct GbKey {
  std::atomic<void*> shader{nullptr};
  uint64_t tech = 0;
  uint32_t flags = 0;
  void* effect = nullptr;
  void* techBegin = nullptr;
  uint32_t scope = 0;         // the execution's collection index (keys are learnt per G-buffer view)
  std::atomic<int> state{0};  // 1 recordable, -1 not, kRetired (state changed; relearnt after the cooldown)
  const char* why = nullptr;
  uint32_t retiredAt = 0;     // render entry + 1
  VsStatic st;
  // From DCS's draw (references held while recordable).
  ID3D11VertexShader* vs = nullptr;
  ID3D11PixelShader* ps = nullptr;
  ID3D11DepthStencilState* dss = nullptr;
  UINT stencilRef = 0;
  ID3D11BlendState* bs = nullptr;
  float blendFactor[4] = {};
  UINT sampleMask = 0;
  ID3D11RasterizerState* rs = nullptr;
  ID3D11SamplerState* vsSamp[kSampSlots] = {};
  ID3D11SamplerState* psSamp[kSampSlots] = {};
  uint32_t vsSampDeps = 0xffff, psSampDeps = 0xffff;  // sampler slots FX Apply sets (all when not read)
  uint32_t psCbMask = 0;     // PS slots holding the material's CB (of the slots FX Apply sets)
  uint32_t psCtxMask = 0;    // PS slots 6..8 holding a context buffer (informative: lists bind the front's)
  uint64_t psSbMask[2] = {}; // PS slots holding the page view
  int psTexCount = 0;        // = the reads entry's record count
  int32_t psTexH[kMaxPsTex] = {};
  UINT psTexSlot[kMaxPsTex] = {};
  mutable std::atomic<uint32_t> lastUse{0};  // render entry of the last job lookup (compaction age)
};

struct Tables {
  GbKey* keys = nullptr;          // kKeyTable
  ReadsEntry* reads = nullptr;    // kReadsTable
  shrec::Tables mesh;             // meshes only (shadow_rec.h's entries and checks)
  uint32_t keysUsed = 0, readsUsed = 0;
};

inline size_t KeyHash(const void* s, uint64_t tech, uint32_t flags) {
  return Mix(reinterpret_cast<uintptr_t>(s) * 0x9E3779B97F4A7C15ull ^ tech * 0xC2B2AE3D27D4EB4Full ^ flags);
}
inline uint32_t ScopedFlags(uint32_t flags, uint32_t scope) { return flags ^ (scope * 0x9E3779B1u); }

constexpr int kRetired = shrec::kRetired;
constexpr uint32_t kKeyCooldown = shrec::kKeyCooldown;

const GbKey* FindKey(const Tables& t, void* shader, uint64_t tech, uint32_t flags, void* effect, void* techBegin,
                     uint32_t scope = 0) {
  if (!t.keys || !shader) return nullptr;
  size_t i = KeyHash(shader, tech, ScopedFlags(flags, scope)) & (kKeyTable - 1);
  for (size_t p = 0; p < kKeyTable; ++p, i = (i + 1) & (kKeyTable - 1)) {
    const GbKey& e = t.keys[i];
    void* k = e.shader.load(std::memory_order_acquire);
    if (!k) return nullptr;
    if (k == shader && e.tech == tech && e.flags == flags && e.scope == scope && e.effect == effect &&
        e.techBegin == techBegin && e.state.load(std::memory_order_acquire) != kRetired)
      return &e;
  }
  return nullptr;
}

// A retired entry of the key exists (anyTime) or was retired less than
// kKeyCooldown render entries before `now`.
bool KeyCooling(const Tables& t, void* shader, uint64_t tech, uint32_t flags, void* effect, void* techBegin,
                uint32_t scope, uint32_t now, bool anyTime) {
  if (!t.keys || !shader) return false;
  size_t i = KeyHash(shader, tech, ScopedFlags(flags, scope)) & (kKeyTable - 1);
  for (size_t p = 0; p < kKeyTable; ++p, i = (i + 1) & (kKeyTable - 1)) {
    const GbKey& e = t.keys[i];
    void* k = e.shader.load(std::memory_order_acquire);
    if (!k) return false;
    if (k == shader && e.tech == tech && e.flags == flags && e.scope == scope && e.effect == effect &&
        e.techBegin == techBegin && e.state.load(std::memory_order_acquire) == kRetired &&
        (anyTime || now + 1 - e.retiredAt < kKeyCooldown))
      return true;
  }
  return false;
}

GbKey* NewKey(Tables& t, void* shader, uint64_t tech, uint32_t flags, uint32_t scope = 0) {
  if (!t.keys || t.keysUsed >= kKeyTable * 3 / 4) return nullptr;
  size_t i = KeyHash(shader, tech, ScopedFlags(flags, scope)) & (kKeyTable - 1);
  for (size_t p = 0; p < kKeyTable; ++p, i = (i + 1) & (kKeyTable - 1))
    if (!t.keys[i].shader.load(std::memory_order_relaxed)) return &t.keys[i];
  return nullptr;
}

void PublishKey(Tables& t, GbKey& e, void* shader) {
  ++t.keysUsed;
  e.shader.store(shader, std::memory_order_release);
}

const ReadsEntry* FindReads(const Tables& t, void* shader, uint64_t tech, void* effect, void* techBegin) {
  if (!t.reads || !shader) return nullptr;
  size_t i = KeyHash(shader, tech, 0) & (kReadsTable - 1);
  for (size_t p = 0; p < kReadsTable; ++p, i = (i + 1) & (kReadsTable - 1)) {
    const ReadsEntry& e = t.reads[i];
    void* k = e.shader.load(std::memory_order_acquire);
    if (!k) return nullptr;
    if (k == shader && e.tech == tech && e.effect == effect && e.techBegin == techBegin) return &e;
  }
  return nullptr;
}

ReadsEntry* NewReads(Tables& t, void* shader, uint64_t tech) {
  if (!t.reads || t.readsUsed >= kReadsTable * 3 / 4) return nullptr;
  size_t i = KeyHash(shader, tech, 0) & (kReadsTable - 1);
  for (size_t p = 0; p < kReadsTable; ++p, i = (i + 1) & (kReadsTable - 1))
    if (!t.reads[i].shader.load(std::memory_order_relaxed)) return &t.reads[i];
  return nullptr;
}

void PublishReads(Tables& t, ReadsEntry& e, void* shader) {
  ++t.readsUsed;
  e.shader.store(shader, std::memory_order_release);
}

bool AllocTables(Tables& t) {
  if (!t.keys) t.keys = new (std::nothrow) GbKey[kKeyTable];
  if (!t.reads) t.reads = new (std::nothrow) ReadsEntry[kReadsTable];
  if (!t.mesh.meshes) t.mesh.meshes = new (std::nothrow) shrec::MeshEntry[shrec::kMeshTable];
  return t.keys && t.reads && t.mesh.meshes;
}

void FreeTables(Tables& t) {
  if (t.keys)
    for (size_t i = 0; i < kKeyTable; ++i) {
      GbKey& e = t.keys[i];
      SafeRel(e.vs), SafeRel(e.ps), SafeRel(e.dss), SafeRel(e.bs), SafeRel(e.rs);
      for (auto*& s : e.vsSamp) SafeRel(s);
      for (auto*& s : e.psSamp) SafeRel(s);
    }
  delete[] t.keys;
  delete[] t.reads;
  t.keys = nullptr;
  t.reads = nullptr;
  shrec::FreeTables(t.mesh);
  t.keysUsed = t.readsUsed = 0;
}

// Long sessions (render thread): the keys, read sets and meshes worth keeping
// of `from`, into `to` (allocated, empty), as shrec::CompactInto: looked up in
// the last `age` render entries; retired keys while they cool down; meshes
// superseded within shrec::kMeshCooldown.
void CompactInto(const Tables& from, Tables& to, uint32_t now, uint32_t age, shrec::CompactStats& cs) {
  if (from.keys)
    for (size_t i = 0; i < kKeyTable; ++i) {
      const GbKey& e = from.keys[i];
      void* key = e.shader.load(std::memory_order_acquire);
      if (!key) continue;
      ++cs.keysIn;
      if (e.state.load(std::memory_order_acquire) == kRetired) {
        if (now + 1 - e.retiredAt >= kKeyCooldown) {
          ++cs.retired;
          continue;
        }
      } else if (!shrec::Young(now, e.lastUse.load(std::memory_order_relaxed), age)) {
        ++cs.aged;
        continue;
      }
      GbKey* d = NewKey(to, key, e.tech, e.flags, e.scope);
      if (!d) {
        ++cs.aged;
        continue;
      }
      shrec::CopyEntry(*d, e);
      shrec::AddRefIf(d->vs), shrec::AddRefIf(d->ps), shrec::AddRefIf(d->dss), shrec::AddRefIf(d->bs);
      shrec::AddRefIf(d->rs);
      for (ID3D11SamplerState* q : d->vsSamp) shrec::AddRefIf(q);
      for (ID3D11SamplerState* q : d->psSamp) shrec::AddRefIf(q);
      PublishKey(to, *d, key);
      ++cs.keysOut;
    }
  if (from.reads)
    for (size_t i = 0; i < kReadsTable; ++i) {
      const ReadsEntry& e = from.reads[i];
      void* key = e.shader.load(std::memory_order_acquire);
      if (!key) continue;
      ++cs.readsIn;
      if (!shrec::Young(now, e.lastUse.load(std::memory_order_relaxed), age)) {
        ++cs.aged;
        continue;
      }
      ReadsEntry* d = NewReads(to, key, e.tech);
      if (!d) {
        ++cs.aged;
        continue;
      }
      shrec::CopyEntry(*d, e);  // no references
      PublishReads(to, *d, key);
      ++cs.readsOut;
    }
  shrec::CompactMeshes(from.mesh, to.mesh, now, age, cs);
}

// A job snapshot `a` still points at one of `old`'s arrays.
inline bool SharesArrays(const Tables& a, const Tables& old) {
  return (old.keys && a.keys == old.keys) || (old.reads && a.reads == old.reads) ||
         shrec::SharesArrays(a.mesh, old.mesh);
}

// ---------------------------------------------------------------------------
// Environment of a job (DCS's classes and globals; fakes in the tests)
// ---------------------------------------------------------------------------
using Vt1Fn = uint64_t(__fastcall*)(void* self, void* ctx);
using PropFn = uint64_t(__fastcall*)(void* prop, void* span, void* dst);
using EvalAnimFn = void(__fastcall*)(void* mat, const void* span);
using SetResFn = long(__fastcall*)(void* var, void* srv);

struct Env {
  void* srVt = nullptr;      // NGModel SceneRenderable
  void* modelVt = nullptr;   // NGModel ModelMaterialMT
  void* shaderVt = nullptr;  // dx11backend DX11Shader
  uint8_t* const* globals = nullptr;  // &model::globals (GlobalsMT*)
  const void* propVt[gbbatch::kPropClasses] = {};  // ModelDesc animated property classes (pure vt[9])
  uint32_t propBytes[gbbatch::kPropClasses] = {};
  void* setResource = nullptr;  // the FX variables' vt[31] (SetResource)
  EvalAnimFn evalAnim = nullptr;  // NGModel 0xeb60 (render thread, restores)
  bool (*compiled)(void* shader, uint64_t tech) = nullptr;  // shadow_inst has the G-buffer key's compile
  bool (*cbUsed)(void* shader, uint64_t tech, uint64_t out[2]) = nullptr;  // def_uniforms dwords pass 0 reads
  int (*blend)(uint8_t* shader) = nullptr;  // BLEND_MODE (gb_rec_count.h codes)
  gbbatch::TexEnv tex;
};

// sbPositions of page `page` [V NGModel 0xc310: [[globals]+0x80] + 0x20 +
// page * 0x30, view at +0x30] (as shadow_rec.h). Plain.
inline uint8_t* PageBuffer(const Env& env, uint32_t page) {
  const uint8_t* g = env.globals ? *env.globals : nullptr;
  if (!g) return nullptr;
  const uint8_t* base = *reinterpret_cast<const uint8_t* const*>(g + 0x80);
  if (!base) return nullptr;
  return *reinterpret_cast<uint8_t* const*>(base + 0x20 + static_cast<size_t>(page) * 0x30);
}
inline ID3D11ShaderResourceView* PageSrv(const Env& env, uint32_t page) {
  const uint8_t* buf = PageBuffer(env, page);
  return buf ? *reinterpret_cast<ID3D11ShaderResourceView* const*>(buf + 0x30) : nullptr;
}
// The FX variable of the shader's parameter record h ([shader+0xc8] + h*0x50, +0x40).
inline uint8_t* VarOf(const uint8_t* shader, int64_t h) {
  return *reinterpret_cast<uint8_t* const*>(*reinterpret_cast<uint8_t* const*>(shader + 0xc8) + h * 0x50 + 0x40);
}
inline int64_t RecordCount(const uint8_t* shader) {
  const uint8_t* b = *reinterpret_cast<uint8_t* const*>(shader + 0xc8);
  const uint8_t* e = *reinterpret_cast<uint8_t* const*>(shader + 0xd0);
  return b && e > b ? (e - b) / 0x50 : 0;
}
// The material's texture entries for this item ([item+0x18] array, base [props+0x26c]).
inline const uint8_t* EntriesOf(const uint8_t* mat, const uint8_t* item) {
  const uint8_t* props = *reinterpret_cast<uint8_t* const*>(mat + 0x28);
  auto* arr = *reinterpret_cast<uint8_t* const* const*>(item + 0x18);
  if (!props || !arr || !*arr) return nullptr;
  return *arr + static_cast<size_t>(*reinterpret_cast<const uint32_t*>(props + 0x26c)) * 0x18;
}

// ---------------------------------------------------------------------------
// Job: built and recorded on a worker
// ---------------------------------------------------------------------------
enum : int { kJobEmpty = 0, kJobBuilt = 1 };
enum : int { kBuildNone = 0, kBuildOk, kBuildNoVector, kBuildOversize, kBuildVarClass, kBuildPreLate, kBuildRecycled };
enum : int { kStartIdle = 0, kStartArmed = 1, kStartRun = 2, kStartAbort = 3 };
enum : int { kGoWait = 0, kGoRun = 1, kGoAbort = 2 };
constexpr DWORD kStartWaitMs = 250;  // an armed job waits this long for its vector to be final
constexpr DWORD kStartLateWaitMs = 100;  // JobMain: a start that raced the timeout publishes vec

struct Item {
  uint32_t mat;      // mats[] or ~0u
  uint32_t shader;   // shaders[] or ~0u
  uint32_t cand;     // cands[] or ~0u
  uint8_t reason;
  uint8_t seg;       // kNoSeg: residual
  uint8_t writer;    // pass-1 opaque non-cockpit model item with a DX11Shader (writes its FX variables)
  uint8_t want;      // probe request
};

struct MatRec {
  uint8_t* mat;
  uint8_t* shader;
  uint64_t tech;
  void* effect;
  void* techBegin;
  const GbKey* key;
  uint32_t shaderIdx;
  int32_t last;       // vector index of its last ModelMaterialMT item
  int8_t why;         // -1: not decided; kRecorded; else the reason of all its items
  bool excluded;      // an item of another pass, transparent
  bool shaderOk;      // a DX11Shader
  bool transparent;
  bool ck;            // its items here are cockpit items (technique [mat+0x1e0]; cockpit jobs only)
  bool mixed;         // cockpit and other items here (cockpit jobs only)
  uint32_t animN;
  uint32_t rec;       // recorded draws
  uint32_t segStamp;  // dedupe per segment
  uint64_t guard[2];  // CB dwords compared at the exec entry
  uint8_t cb[kCbLen]; // def_uniforms at the job's read
};

struct ShaderRec {
  uint8_t* shader;
  uint32_t gen;
  uint8_t unclean;
  uint32_t rec;
};

struct SetRef {  // one texture set of a candidate draw
  const TexEntry* e;
  uint8_t* tex;
  uint64_t size;
  int64_t h;
};

struct Cand {
  const GbKey* key;
  const shrec::MeshEntry* mesh;
  ID3D11ShaderResourceView* page;
  uint32_t item;
  uint32_t pageIdx;
  uint32_t setFirst, viewFirst;  // in the stage-A thread's arrays (tsets/tviews[thread])
  uint16_t setCount;
  uint8_t alive;
  uint8_t thread;
};

struct Seg {
  uint32_t first, end;           // vector indexes
  uint32_t candFirst, candEnd;   // cands[] (alive ones are the draws)
  uint32_t entFirst, entEnd;     // segEnts[]
  uint32_t pairFirst, pairEnd;   // segPairs[] (texture, size): the streaming replay
  uint32_t matFirst, matEnd;     // segMats[]
  uint32_t pageFirst, pageEnd;   // segPages[]
  uint32_t draws;
  uint64_t tris;
  uint32_t pieceFirst, pieceEnd; // pieces[]: its command lists, executed in order by its exec entry
};

// A contiguous range of a segment's draws recorded by one worker into one
// command list (a segment split over workers has several, in order).
// A segment re-recorded during its pass (the slot's idle workers).
struct Job;
struct RedoArg {
  Job* j;
  uint8_t seg, worker;
};
constexpr uint32_t kMaxRedoEnts = 256;
enum : int { kRedoNone = 0, kRedoWant, kRedoRun, kRedoOk, kRedoFail };

struct Piece {
  uint32_t candFirst, candEnd, draws;
  uint8_t seg, role;  // role: 0 the primary, 1.. the helpers in their arrival order
  uint8_t redo;       // 1: a segment re-recorded during its pass (RedoRecord): stops once that pass is over
  ID3D11CommandList* cl;
};

// Stage A's reads per material, per thread (cache).
struct PreMat {
  uint8_t* mat;
  uint32_t gen;
  uint8_t* shader;
  bool shaderOk, transparent;
  uint64_t tech;
  void* effect;
  void* techBegin;
  const ReadsEntry* rd;
  const GbKey* key;
  uint64_t techCk;          // cockpit jobs: [mat+0x1e0] and its reads and key
  const ReadsEntry* rdCk;
  const GbKey* keyCk;
};
inline uint64_t PmTech(const PreMat* pm, bool ck) { return ck ? pm->techCk : pm->tech; }
inline const ReadsEntry* PmReads(const PreMat* pm, bool ck) { return ck ? pm->rdCk : pm->rd; }
inline const GbKey* PmKey(const PreMat* pm, bool ck) { return ck ? pm->keyCk : pm->key; }

// Stage A's reads per item (any thread; the serial commit decides from them).
enum : uint8_t { kPreNone = 0, kPreModel = 1 };
struct Pre {
  uint8_t* item;
  uint8_t* mat;
  uint8_t* mesh;
  const PreMat* pm;
  const GbKey* key;            // the key the mesh and textures were read against (nullptr: not read)
  const shrec::MeshEntry* me;
  ID3D11ShaderResourceView* page;
  uint32_t pageIdx;
  uint32_t setFirst, viewFirst;
  uint16_t setCount;
  uint8_t kind;      // kPreModel: a ModelMaterialMT SceneRenderable
  uint8_t pass1, cockpit;
  uint8_t inherit;   // pass-1 writer: a read record not set by it
  uint8_t pageOk;    // a page buffer (vt[27] sets sbPositions)
  uint8_t meshCode;  // kRecorded or the mesh reason
  uint8_t texCode;   // kRecorded or the texture reason
  uint8_t thread;
};

struct Pair {
  uint8_t* tex;
  uint64_t size;
};
struct PageRef {
  uint32_t page;
  ID3D11ShaderResourceView* srv;
};
struct MatRestore {
  uint32_t mat;
  uint32_t item;
  uint8_t seg;
};
struct VarRestore {
  uint8_t* var;
  void* value;
  uint8_t seg;
  // A texture value is resolved at restore time from its set's entry (the
  // newest one after a re-record), not kept as the view picked at build: a
  // re-recorded segment draws with the swapped entry's view, and the build's
  // view may have been released by then (SetResource on freed memory).
  const TexEntry* e = nullptr;
  uint64_t size = 0;
};
struct Dd {
  uint64_t a, b;
  uint32_t stamp;
};
struct Miss {
  uint8_t* tex;
  int64_t aux;
  int32_t type;
};
struct ReadWant {
  uint8_t* shader;
  uint64_t tech;
};

struct Job {
  // Inputs (render thread, written only while the worker is idle).
  void** vec = nullptr;
  // Frame-heap generation at the start (StartJob); the workers stop reading DCS's
  // renderables once it changes (HeapRecycled), and set `recycled`.
  uint64_t heapGen = 0;
  std::atomic<uint8_t> recycled{0};
  uint32_t scope = 0, flags = 0, island = kDefaultIsland, stride = 0;
  uint32_t splitMin = kSplitMinDraws;  // fewer recorded draws: one worker records them
  uint32_t keyScope = 0;               // keys of this execution's collection (its identity hash, R24)
  bool cockpit = false;                // [Model] GBufferRecorderCockpit: cockpit items are candidates
  bool armById = false;                // the sort observer starts it by armKey (else by armIdx)
  const void* armVol = nullptr;        // by (armSm, armVol, armVrank) when set (before armById, R24 13)
  uint16_t armSm = 0, armVrank = 0;
  CollKey armKey;
  ID3D11RenderTargetView* rtv[kRtv] = {};  // references held by the job
  ID3D11DepthStencilView* dsv = nullptr;
  UINT nvp = 0, nsc = 0;
  D3D11_VIEWPORT vp[kVp] = {};
  D3D11_RECT sc[kVp] = {};
  ID3D11Buffer* ctxBuf[2][3] = {};  // ours (VS, PS b6-b8), bound in the lists; null where DCS binds none
  ID3D11Buffer* ctxRef[2][3] = {};  // Arm's references on ctxBuf (released with the other target references)
  ID3D11SamplerState* pool[2][kSampSlots] = {};  // VS, PS: DCS's s5-s15 at the front entry (references)
  uint8_t ratio[kRatioLen] = {};
  void* ratioFb = nullptr;
  int32_t ratioVp[2] = {};
  void* front = nullptr;
  void* execObj[kMaxSegments] = {};
  const Env* env = nullptr;
  const Tables* tab = nullptr;
  Tables tabSnap;  // the tables as Arm saw them (tab points here in the game; see shadow_rec.h's Job)
  const TexTable* tex = nullptr;
  SRWLOCK* texLock = nullptr;
  uint32_t useGen = 0;
  HANDLE start = nullptr;
  std::atomic<int> startState{kStartIdle};
  void* armRg = nullptr;
  uint32_t armIdx = 0;
  bool startedEarly = false;
  int64_t tStart = 0;
  uint32_t entryGen = 0;
  // Outputs (the worker; readable once it is idle).
  std::atomic<int> phase{kJobEmpty};
  int result = kBuildNone;
  void** begin = nullptr;
  size_t n = 0;
  uint32_t gen = 0, ddStamp = 0;
  uint32_t matCount = 0, shaderCount = 0, candCount = 0, segCount = 0, maxSeg = kDefaultMaxSeg;
  uint32_t entCount = 0, pairCount = 0, segMatCount = 0, pageCount = 0, matRestCount = 0, varRestCount = 0;
  uint32_t wantCount = 0, readWantCount = 0, missCount = 0, refreshCount = 0;
  uint32_t recorded = 0, draws = 0;
  uint32_t reasons[kReasons] = {};
  double buildUs = 0, recordUs = 0, preUs = 0, commitUs = 0;
  uint64_t cbBytes = 0;
  int64_t tDone = 0, tPreDone = 0, tBuilt = 0;
  // Helpers (stage A chunks and recording pieces).
  HANDLE preGo = nullptr;  // manual-reset: stage A may start (or aborted)
  HANDLE recGo = nullptr;  // manual-reset: the pieces are planned (or aborted)
  std::atomic<int> preState{kGoWait}, recState{kGoWait};
  std::atomic<uint32_t> preNext{0}, preDone{0}, started{0}, ready{0}, recFail{0};
  uint32_t helpers = 0, recWorkers = 1, pieceCount = 0;
  uint32_t preItems[kThreads] = {};
  double recUs[kThreads] = {};
  uint64_t cbBytesT[kThreads] = {};
  int64_t tRecDone[kThreads] = {};
  Piece pieces[kMaxPieces] = {};
  Seg segs[kMaxSegments] = {};
  void* snap[kMaxItems];
  Item items[kMaxItems];
  Cand cands[kMaxItems];
  defrec::CbSlice slices[kMaxItems];
  uint8_t flag[kMaxItems];
  uint8_t segWhy[kMaxItems];
  Pre pre[kMaxItems];
  SetRef tsets[kThreads][kTSets];
  ID3D11ShaderResourceView* tviews[kThreads][kTViews];
  uint32_t tsetCount[kThreads], tviewCount[kThreads];
  PreMat preMats[kThreads][kPreMatTable];
  Miss tmiss[kThreads][kTMisses];
  uint32_t tmissCount[kThreads];
  const TexEntry* trefresh[kThreads][kTRefresh];
  uint32_t trefreshCount[kThreads];
  Dd tdd[kThreads][kTDedupe];
  MatRec mats[kMaxMats];
  uint32_t matIdx[kMatTable];   // mats index + 1
  uint32_t matGen[kMatTable];
  ShaderRec shaders[kMaxShaders];
  uint32_t shIdx[kShaderTable];
  uint32_t shGen[kShaderTable];
  const shrec::MeshEntry* meshKey[kThreads][kMeshCache];
  uint32_t meshGen[kThreads][kMeshCache];
  uint8_t meshOk[kThreads][kMeshCache];
  Dd dd[kDedupe];
  const TexEntry* segEnts[kMaxSegEnts];
  uint32_t segEntVer[kMaxSegEnts];  // the entry's version when the job read it
  Pair segPairs[kMaxSegPairs];
  uint32_t segMats[kMaxSegMats];
  PageRef segPages[kMaxSegPages];
  MatRestore matRest[kMaxRestores];
  VarRestore varRest[kMaxRestores];
  Miss misses[kMaxMisses];
  const TexEntry* refresh[kMaxRefresh];  // entries found unusable (rebuilt after the pass)
  ReadWant readWants[kMaxReadWants];
  uint64_t wantKey[kWantTable];
  uint32_t wantGen[kWantTable];
  void* swapList[2 * kMaxItems + 4];  // + the ownship's switch entries (at most one pair per residual item)
  uint32_t swapCount = 0;
  const void* ownKey = nullptr;  // stage 2: the ownship instance this job left residual (g_ownKey at its start)
  uint32_t ownItems = 0, ownRuns = 0, ownRecItems = 0;  // ownship items, bracketed runs, items in segments
  bool ownRec = true;  // stage 2: ownship items recorded in segments of their own (g_ownRecSegs at its start)
  uint8_t own[kMaxItems];  // stage 2: the item is the ownship's (any SceneRenderable of the instance, any material)
  // Segments whose textures changed after the job read them (render thread
  // plans at the front and exec entries; the slot's idle workers re-record).
  TexEntry redoEnt[kMaxRedoEnts];  // the job's own entries built from the live textures (views referenced)
  const TexEntry* redoFrom[kMaxRedoEnts];
  uint32_t redoEntCount = 0;
  std::atomic<int> redoState[kMaxSegments];
  ID3D11CommandList* redoCl[kMaxSegments];
  RedoArg redoArg[kMaxSegments];
  // Issue #3: 1 once the job's pass returned (WrapSlot). A re-record still
  // running then is useless (RedoReady gave up on it, the segment was drawn
  // stock) and stops before its next read of a renderable, instead of reading
  // on until the next frame's heap reset. 0 again at Arm (ReleaseRedo).
  std::atomic<int> redoStop{0};
};

Job* NewJob() {
  void* mem = VirtualAlloc(nullptr, sizeof(Job), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);  // zeroed
  Job* j = mem ? new (mem) Job : nullptr;
  if (j) {
    j->start = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    j->preGo = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    j->recGo = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  }
  if (j && (!j->start || !j->preGo || !j->recGo)) {
    if (j->start) CloseHandle(j->start);
    if (j->preGo) CloseHandle(j->preGo);
    if (j->recGo) CloseHandle(j->recGo);
    j->~Job();
    VirtualFree(j, 0, MEM_RELEASE);
    j = nullptr;
  }
  return j;
}

// The pieces' lists nobody executed, released (workers idle).
void ReleaseSegLists(Job& j) {
  for (Piece& p : j.pieces) SafeRel(p.cl);
  for (auto*& l : j.redoCl) SafeRel(l);
}

// The re-record state of the last pass (workers idle).
void ReleaseRedo(Job& j) {
  for (auto*& l : j.redoCl) SafeRel(l);
  for (uint32_t i = 0; i < j.redoEntCount; ++i) {
    TexReleaseViews(j.redoEnt[i]);
    j.redoEnt[i].usable = 0;
    j.redoEnt[i].slot.store(kTeEmpty, std::memory_order_relaxed);
  }
  j.redoEntCount = 0;
  for (auto& st : j.redoState) st.store(kRedoNone, std::memory_order_relaxed);
  j.redoStop.store(0, std::memory_order_relaxed);
}

void ReleaseTargetRefs(Job& j) {
  for (auto*& p : j.rtv) SafeRel(p);
  SafeRel(j.dsv);
  for (auto& row : j.ctxRef)
    for (auto*& p : row) SafeRel(p);
  for (auto& row : j.pool)
    for (auto*& p : row) SafeRel(p);
}

void FreeJob(Job*& j) {
  if (!j) return;
  ReleaseSegLists(*j);
  ReleaseRedo(*j);
  ReleaseTargetRefs(*j);
  if (j->start) CloseHandle(j->start);
  if (j->preGo) CloseHandle(j->preGo);
  if (j->recGo) CloseHandle(j->recGo);
  j->~Job();
  VirtualFree(j, 0, MEM_RELEASE);
  j = nullptr;
}

// The texture sets and PS views of candidate c (in its stage-A thread's arrays).
inline const SetRef* SetsOf(const Job& j, const Cand& c) { return j.tsets[c.thread] + c.setFirst; }
inline ID3D11ShaderResourceView* const* ViewsOf(const Job& j, const Cand& c) { return j.tviews[c.thread] + c.viewFirst; }

// Gen-stamped dedupe of (a, b) pairs under the current ddStamp: true when new.
// A full table answers "new" (a repeat streaming request or check costs only time).
bool DdInsert(Job& j, uint64_t a, uint64_t b) {
  size_t s = Mix(a * 0x9E3779B97F4A7C15ull ^ b * 0xC2B2AE3D27D4EB4Full) & (kDedupe - 1);
  for (uint32_t p = 0; p < 64; ++p, s = (s + 1) & (kDedupe - 1)) {
    Dd& d = j.dd[s];
    if (d.stamp != j.ddStamp) {
      d = {a, b, j.ddStamp};
      return true;
    }
    if (d.a == a && d.b == b) return false;
  }
  return true;
}
bool DdFind(const Job& j, uint64_t a, uint64_t b) {
  size_t s = Mix(a * 0x9E3779B97F4A7C15ull ^ b * 0xC2B2AE3D27D4EB4Full) & (kDedupe - 1);
  for (uint32_t p = 0; p < 64; ++p, s = (s + 1) & (kDedupe - 1)) {
    const Dd& d = j.dd[s];
    if (d.stamp != j.ddStamp) return false;
    if (d.a == a && d.b == b) return true;
  }
  return false;
}
void NextStamp(Job& j) {
  if (++j.ddStamp == 0) {
    memset(j.dd, 0, sizeof(j.dd));
    j.ddStamp = 1;
  }
}

ShaderRec* ShaderFor(Job& j, uint8_t* sh, uint32_t* idx) {
  size_t i = Mix(reinterpret_cast<uintptr_t>(sh)) & (kShaderTable - 1);
  for (uint32_t p = 0; p < kShaderTable; ++p, i = (i + 1) & (kShaderTable - 1)) {
    if (j.shGen[i] != j.gen) {
      if (j.shaderCount == kMaxShaders) return nullptr;
      j.shGen[i] = j.gen;
      j.shIdx[i] = j.shaderCount + 1;
      ShaderRec& s = j.shaders[j.shaderCount];
      s = {sh, j.gen, 0, 0};
      *idx = j.shaderCount++;
      return &s;
    }
    ShaderRec& s = j.shaders[j.shIdx[i] - 1];
    if (s.shader == sh) {
      *idx = j.shIdx[i] - 1;
      return &s;
    }
  }
  return nullptr;
}

void WantReads(Job& j, uint8_t* sh, uint64_t tech) {
  if (j.readWantCount == kMaxReadWants) return;
  for (uint32_t k = 0; k < j.readWantCount; ++k)
    if (j.readWants[k].shader == sh && j.readWants[k].tech == tech) return;
  j.readWants[j.readWantCount++] = {sh, tech};
}

// One probe request per (shader, technique, mesh): its first item.
void WantProbe(Job& j, size_t i, const void* sh, uint64_t tech, const void* mesh) {
  if (j.wantCount >= kMaxWants) return;
  const uint64_t h = (reinterpret_cast<uintptr_t>(sh) * 0x9E3779B97F4A7C15ull ^ tech * 0xC2B2AE3D27D4EB4Full ^
                      reinterpret_cast<uintptr_t>(mesh) * 0x165667B19E3779F9ull) |
                     1;
  size_t s = Mix(h) & (kWantTable - 1);
  for (uint32_t p = 0; p < kWantTable; ++p, s = (s + 1) & (kWantTable - 1)) {
    if (j.wantGen[s] != j.gen) {
      j.wantGen[s] = j.gen;
      j.wantKey[s] = h;
      j.items[i].want = 1;
      ++j.wantCount;
      return;
    }
    if (j.wantKey[s] == h) return;
  }
}

// Pass 2 runs under one dedupe stamp: misses and rebuild requests once each.
void AddMiss(Job& j, uint8_t* tex, int64_t aux, int32_t type) {
  if (j.missCount == kMaxMisses ||
      !DdInsert(j, reinterpret_cast<uintptr_t>(tex) ^ 0x1111000000000000ull,
                static_cast<uint64_t>(aux) * 0x9E3779B97F4A7C15ull ^ static_cast<uint32_t>(type)))
    return;
  j.misses[j.missCount++] = {tex, aux, type};
}

void AddRefresh(Job& j, const TexEntry* e) {
  if (j.refreshCount == kMaxRefresh || !DdInsert(j, reinterpret_cast<uintptr_t>(e), 0x2222)) return;
  j.refresh[j.refreshCount++] = e;
}

// Material-level facts, at its first item in the job (pass 1 of stage A);
// ck: that item is a cockpit item of a cockpit job (technique [mat+0x1e0]).
MatRec* MatFor(Job& j, uint8_t* mat, uint32_t* idx, bool ck = false) {
  const Env& env = *j.env;
  size_t i = Mix(reinterpret_cast<uintptr_t>(mat)) & (kMatTable - 1);
  for (uint32_t p = 0; p < kMatTable; ++p, i = (i + 1) & (kMatTable - 1)) {
    if (j.matGen[i] != j.gen) {
      if (j.matCount == kMaxMats) return nullptr;
      j.matGen[i] = j.gen;
      j.matIdx[i] = j.matCount + 1;
      MatRec& m = j.mats[j.matCount];
      *idx = j.matCount++;
      m.mat = mat;
      m.key = nullptr;
      m.shaderIdx = ~0u;
      m.last = -1;
      m.why = -1;
      m.excluded = false;
      m.ck = ck;
      m.mixed = false;
      m.animN = 0;
      m.rec = 0;
      m.segStamp = 0;
      m.guard[0] = m.guard[1] = 0;
      const uint8_t* props = *reinterpret_cast<uint8_t* const*>(mat + 0x28);
      m.transparent = props && props[0x33] != 0;
      m.shader = *reinterpret_cast<uint8_t**>(mat + 0x30);
      m.shaderOk = m.shader && *reinterpret_cast<void**>(m.shader) == env.shaderVt;
      m.tech = *reinterpret_cast<uint64_t*>(mat + (ck ? 0x1e0 : 0x1d8));
      m.effect = m.shaderOk ? *reinterpret_cast<void**>(m.shader + 0x50) : nullptr;
      m.techBegin = m.shaderOk ? *reinterpret_cast<void**>(m.shader + 0xb0) : nullptr;
      memcpy(m.cb, mat + kCbOff, kCbLen);
      if (m.shaderOk) ShaderFor(j, m.shader, &m.shaderIdx);
      return &m;
    }
    MatRec& m = j.mats[j.matIdx[i] - 1];
    if (m.mat == mat) {
      *idx = j.matIdx[i] - 1;
      return &m;
    }
  }
  return nullptr;
}

// The animated property list of the material: classes whose vt[9] is pure
// (gb_batch.h notes), destinations inside def_uniforms and off
// posStructOffset. Adds their destinations to the overrides. False: not
// replicable.
bool AnimOk(const Env& env, const uint8_t* mat, uint32_t* count, uint64_t over[2]) {
  const uint8_t* b = *reinterpret_cast<uint8_t* const*>(mat + 8);
  const uint8_t* e = *reinterpret_cast<uint8_t* const*>(mat + 0x10);
  const intptr_t bytes = e - b;
  if (bytes < 0 || bytes % 16 || bytes / 16 > kMaxAnim) return false;
  *count = static_cast<uint32_t>(bytes / 16);
  for (const uint8_t* p = b; p < e; p += 16) {
    const uint8_t* dst = *reinterpret_cast<uint8_t* const*>(p);
    const uint8_t* prop = *reinterpret_cast<uint8_t* const*>(p + 8);
    const void* vt = prop ? *reinterpret_cast<void* const*>(prop) : nullptr;
    int c = 0;
    while (c < gbbatch::kPropClasses && env.propVt[c] != vt) ++c;
    if (!vt || c == gbbatch::kPropClasses) return false;
    const intptr_t off = dst - mat;
    const intptr_t end = off + static_cast<intptr_t>(env.propBytes[c]);
    if (off < static_cast<intptr_t>(kCbOff) || end > static_cast<intptr_t>(kCbOff + kCbLen) ||
        (off < 0x190 && end > 0x18c))
      return false;
    MarkDwords(over, static_cast<uint32_t>(off - kCbOff), env.propBytes[c]);
  }
  return true;
}

// Material-level decision (its first candidate item).
void DecideMat(Job& j, MatRec& m) {
  const Env& env = *j.env;
  const int blend = env.blend ? env.blend(m.shader) : gbreccount::kBmNone;
  if (blend != gbreccount::kBmNone && !(blend == gbreccount::kBmAlphaTest && (j.scope & kScopeA2c))) {
    m.why = kRBlend;
    return;
  }
  uint64_t over[2] = {};
  BaseOverrides(over);
  if (!AnimOk(env, m.mat, &m.animN, over)) {
    m.why = kRAnim;
    return;
  }
  uint64_t used[2];
  if (!env.cbUsed || !env.cbUsed(m.shader, m.tech, used)) AllDwords(used);
  m.guard[0] = used[0] & ~over[0];
  m.guard[1] = used[1] & ~over[1];
  m.key = FindKey(*j.tab, m.shader, m.tech, j.flags, m.effect, m.techBegin, j.keyScope);
  if (m.key) shrec::Touch(m.key->lastUse, j.useGen);
  if (!m.key && KeyCooling(*j.tab, m.shader, m.tech, j.flags, m.effect, m.techBegin, j.keyScope, 0, true))
    m.why = kRKeyCooling;  // probed again once the cooldown ends
  else if (!m.key)
    m.why = static_cast<int8_t>(env.compiled && !env.compiled(m.shader, m.tech) ? kRNoCompile : kRKeyPending);
  else if (m.key->state.load(std::memory_order_acquire) <= 0)
    m.why = kRKeyRejected;
  else
    m.why = kRecorded;
}

// Mesh entry still describes the live mesh (once per entry per job).
bool MeshLiveOk(Job& j, int t, const uint8_t* mesh, const shrec::MeshEntry* me) {
  auto live = [&] {
    shrec::MeshLive l;
    const char* why = shrec::ReadMesh(mesh, l);
    return shrec::MeshSame(l, why, *me);  // a rejected entry: the same identity
  };
  size_t s = Mix(reinterpret_cast<uintptr_t>(me)) & (kMeshCache - 1);
  for (uint32_t p = 0; p < kMeshCache; ++p, s = (s + 1) & (kMeshCache - 1)) {
    if (j.meshGen[t][s] != j.gen) {
      const bool ok = live();
      j.meshGen[t][s] = j.gen;
      j.meshKey[t][s] = me;
      j.meshOk[t][s] = ok ? 1 : 0;
      return ok;
    }
    if (j.meshKey[t][s] == me) return j.meshOk[t][s] != 0;
  }
  return live();
}

// Stage A is split in two (R17 S5's pattern): the reads of DCS memory per
// item (PreOne, any worker, chunks of items, no shared writes but the item's
// own Pre slot and the thread's arrays), then the serial commit in vector
// order (CommitBuild, the primary: materials, shaders, decisions, segments).

// The material's reads for thread t (cached per job); a scratch entry when the cache is full.
const PreMat* PreMatFor(Job& j, int t, uint8_t* mat, PreMat* scratch) {
  const Env& env = *j.env;
  PreMat* tab = j.preMats[t];
  size_t i = Mix(reinterpret_cast<uintptr_t>(mat)) & (kPreMatTable - 1);
  PreMat* s = scratch;
  for (uint32_t p = 0; p < 16; ++p, i = (i + 1) & (kPreMatTable - 1)) {
    if (tab[i].gen == j.gen && tab[i].mat == mat) return &tab[i];
    if (tab[i].gen != j.gen) {
      s = &tab[i];
      break;
    }
  }
  *s = PreMat();
  s->mat = mat;
  s->gen = j.gen;
  const uint8_t* props = *reinterpret_cast<uint8_t* const*>(mat + 0x28);
  s->transparent = props && props[0x33] != 0;
  s->shader = *reinterpret_cast<uint8_t**>(mat + 0x30);
  s->shaderOk = s->shader && *reinterpret_cast<void**>(s->shader) == env.shaderVt;
  s->tech = *reinterpret_cast<uint64_t*>(mat + 0x1d8);
  if (s->shaderOk) {
    s->effect = *reinterpret_cast<void**>(s->shader + 0x50);
    s->techBegin = *reinterpret_cast<void**>(s->shader + 0xb0);
    s->rd = FindReads(*j.tab, s->shader, s->tech, s->effect, s->techBegin);
    s->key = FindKey(*j.tab, s->shader, s->tech, j.flags, s->effect, s->techBegin, j.keyScope);
    if (s->rd) shrec::Touch(s->rd->lastUse, j.useGen);
    if (s->key) shrec::Touch(s->key->lastUse, j.useGen);
    if (j.cockpit) {
      s->techCk = *reinterpret_cast<uint64_t*>(mat + 0x1e0);
      s->rdCk = FindReads(*j.tab, s->shader, s->techCk, s->effect, s->techBegin);
      s->keyCk = FindKey(*j.tab, s->shader, s->techCk, j.flags, s->effect, s->techBegin, j.keyScope);
      if (s->rdCk) shrec::Touch(s->rdCk->lastUse, j.useGen);
      if (s->keyCk) shrec::Touch(s->keyCk->lastUse, j.useGen);
    }
  }
  return s;
}

// Per-thread dedupe (stamp = the job's gen): true when new.
bool DdInsertT(Job& j, int t, uint64_t a, uint64_t b) {
  size_t s = Mix(a * 0x9E3779B97F4A7C15ull ^ b * 0xC2B2AE3D27D4EB4Full) & (kTDedupe - 1);
  for (uint32_t p = 0; p < 32; ++p, s = (s + 1) & (kTDedupe - 1)) {
    Dd& d = j.tdd[t][s];
    if (d.stamp != j.gen) {
      d = {a, b, j.gen};
      return true;
    }
    if (d.a == a && d.b == b) return false;
  }
  return true;
}

void AddMissT(Job& j, int t, uint8_t* tex, int64_t aux, int32_t type) {
  if (j.tmissCount[t] == kTMisses ||
      !DdInsertT(j, t, reinterpret_cast<uintptr_t>(tex) ^ 0x1111000000000000ull,
                 static_cast<uint64_t>(aux) * 0x9E3779B97F4A7C15ull ^ static_cast<uint32_t>(type)))
    return;
  j.tmiss[t][j.tmissCount[t]++] = {tex, aux, type};
}

void AddRefreshT(Job& j, int t, const TexEntry* e) {
  if (j.trefreshCount[t] == kTRefresh || !DdInsertT(j, t, reinterpret_cast<uintptr_t>(e), 0x2222)) return;
  j.trefresh[t][j.trefreshCount[t]++] = e;
}

// Stage A reads of item i on thread t: class, material (cached), the stock
// inheritance facts of a pass-1 writer, and for a recordable key the mesh,
// page and texture sets (PS views in the key's slot order). Plain.
// R30 live crash (2026-10-11 20:09:55, AV in RecordRange -> BuildWindow on
// [item+0xd4]): a job whose pass did not come in its frame (stale, late, an
// execution that skipped this frame) kept its workers reading the frame's
// renderables after DCS reset the frame heap for the next frame; the
// renderable's item pointer was another allocation's bytes by then. The
// workers now check the generation before every read phase and stop (the
// job fails: DCS draws that pass stock, as for any failed job); a fault that
// still slips between the check and the read is contained per job when the
// heap was recycled (RecycledFilter), and still latches the recorder off
// otherwise (a real bug).
std::atomic<uint64_t> g_recycledJobs{0}, g_recycledFaults{0}, g_recycledAtPass{0};
std::atomic<uint64_t> g_redoFaultsContained{0};  // issue #3: of g_recycledFaults, in RedoMain
// heapGen 0 = not started through StartJob (offline tests driving a job directly): no check.
inline bool HeapRecycled(const Job& j) {
  return j.heapGen && g_frameHeapGen.load(std::memory_order_acquire) != j.heapGen;
}
// A fault near a scene change (slot change, new render graph): shadow_rec.h SceneChanging.
inline int RecycledFilter(unsigned long code, const Job& j) {
  if (code == EXCEPTION_ACCESS_VIOLATION && (HeapRecycled(j) || shrec::SceneChanging(j.tStart))) {
    g_recycledFaults.fetch_add(1, std::memory_order_relaxed);
    return EXCEPTION_EXECUTE_HANDLER;
  }
  return EXCEPTION_CONTINUE_SEARCH;
}

// R73 (shadow_rec.h R61, second layer): a renderable, item or material pointer
// that is not a plausible heap object (shrec::PlausiblePtr: 8-aligned, at or
// above 64 KB, below 0x800000000000) means the vector names recycled memory.
// The job is dropped like one on a recycled heap: JobMain does not commit it
// (kBuildRecycled), DCS draws the pass stock. Steady state: DCS's objects are
// heap allocations (16-aligned), so this never fires there.
std::atomic<uint64_t> g_implausible{0};  // items whose pointers were not plausible (their jobs dropped)
inline void Implausible(Job& j, Pre& p) {
  g_implausible.fetch_add(1, std::memory_order_relaxed);
  j.recycled.store(1, std::memory_order_relaxed);
  p.kind = kPreNone;
}

void PreOne(Job& j, int t, size_t i) {
  const Env& env = *j.env;
  Pre& p = j.pre[i];
  p.kind = kPreNone;
  p.thread = static_cast<uint8_t>(t);
  p.pm = nullptr;
  p.key = nullptr;
  p.me = nullptr;
  p.page = nullptr;
  p.setCount = 0;
  p.inherit = p.pageOk = 0;
  p.meshCode = kRMeshPending;
  p.texCode = kRTexUnusable;
  auto* r = static_cast<uint8_t*>(j.snap[i]);
  if (r && !shrec::PlausiblePtr(r)) return Implausible(j, p);
  if (!r || *reinterpret_cast<void**>(r) != env.srVt) return;
  auto* item = *reinterpret_cast<uint8_t**>(r + 0x10);
  if (item && !shrec::PlausiblePtr(item)) return Implausible(j, p);
  auto* mat = item ? *reinterpret_cast<uint8_t**>(item + 0x10) : nullptr;
  if (mat && !shrec::PlausiblePtr(mat)) return Implausible(j, p);
  if (!mat || *reinterpret_cast<void**>(mat) != env.modelVt) return;
  p.kind = kPreModel;
  p.item = item;
  p.mat = mat;
  p.mesh = *reinterpret_cast<uint8_t**>(item + 0xc0);
  p.pass1 = *reinterpret_cast<uint32_t*>(r + 0x60) == 1 ? 1 : 0;
  p.cockpit = r[0x64] ? 1 : 0;
  PreMat scratch;
  const PreMat* pm = PreMatFor(j, t, mat, &scratch);
  if (pm == &scratch) return;  // the cache is full: the commit sees no reads (residual)
  p.pm = pm;
  if (!p.pass1 || (p.cockpit && !j.cockpit) || pm->transparent || !pm->shaderOk) return;
  const bool ck = p.cockpit != 0;
  const uint32_t page = *reinterpret_cast<uint32_t*>(item + 0xd0);
  p.pageIdx = page;
  p.pageOk = PageBuffer(env, page) ? 1 : 0;
  const ReadsEntry* rd = PmReads(pm, ck);
  const uint32_t ntex = *reinterpret_cast<uint32_t*>(mat + 0x2d8);
  const uint8_t* en = ntex ? EntriesOf(mat, item) : nullptr;
  if (rd && rd->state > 0) {
    uint64_t set[kRecWords] = {};
    for (uint32_t k = 0; k < ntex && k < kMaxTex && en; ++k) {
      const int64_t h = *reinterpret_cast<int64_t*>(mat + 0x240 + 8 * k);
      if (h < 0 || h >= static_cast<int64_t>(kMaxRecords)) continue;
      if (*reinterpret_cast<void* const*>(en + k * 0x18 + 8)) set[h >> 6] |= 1ull << (h & 63);
    }
    for (uint32_t w = 0; w < kRecWords; ++w)
      if (rd->mask[w] & ~set[w]) p.inherit = 1;
  }
  const GbKey* key = PmKey(pm, ck);
  if (!key || key->state.load(std::memory_order_acquire) <= 0 || !rd || rd->state <= 0 || p.inherit || !p.pageOk)
    return;  // the commit gives the reason
  p.key = key;
  const shrec::MeshEntry* me =
      shrec::FindMesh(j.tab->mesh, p.mesh, pm->shader, PmTech(pm, ck), pm->effect, pm->techBegin);
  if (!me) return;  // kRMeshPending
  shrec::Touch(me->lastUse, j.useGen);
  if (!MeshLiveOk(j, t, p.mesh, me)) {
    p.meshCode = kRMeshChanged;  // another object at the address: probed again (the commit asks)
    return;
  }
  if (me->state.load(std::memory_order_acquire) <= 0) {
    p.meshCode = kRMeshRejected;
    return;
  }
  p.meshCode = kRecorded;
  p.me = me;
  p.page = PageSrv(env, page);
  if (!p.page) return;
  if (ntex > kMaxTex || j.tsetCount[t] + ntex > kTSets ||
      j.tviewCount[t] + static_cast<uint32_t>(key->psTexCount) > kTViews) {
    p.texCode = kRTooMany;
    return;
  }
  const uint8_t* recs = *reinterpret_cast<uint8_t* const*>(pm->shader + 0xc8);
  const int64_t nrec = RecordCount(pm->shader);
  const uint64_t size = *reinterpret_cast<uint64_t*>(r + 0x6c);
  p.setFirst = j.tsetCount[t];
  p.viewFirst = j.tviewCount[t];
  SetRef* sets = j.tsets[t] + p.setFirst;
  ID3D11ShaderResourceView** views = j.tviews[t] + p.viewFirst;
  uint32_t found = 0;
  int why = kRecorded;
  for (uint32_t k = 0; k < ntex && en; ++k) {
    const int64_t h = *reinterpret_cast<int64_t*>(mat + 0x240 + 8 * k);
    if (h == -1) continue;
    auto* tex = *reinterpret_cast<uint8_t* const*>(en + k * 0x18 + 8);
    if (!tex) continue;  // slot 26 returns at once: nothing set (an unread record: the inheritance check covers the read ones)
    if (h < 0 || h >= nrec) {
      why = kRTexUnusable;
      break;
    }
    const int64_t aux = *reinterpret_cast<const int64_t*>(en + k * 0x18);
    const int32_t type = *reinterpret_cast<const int32_t*>(recs + h * 0x50 + 0xc);
    const TexEntry* e = TexFind(*j.tex, tex, aux, type);
    if (!e) {
      AddMissT(j, t, tex, aux, type);
      if (why == kRecorded) why = kRTexMiss;
      continue;  // every miss of the draw is requested
    }
    ID3D11ShaderResourceView* v = nullptr;
    if (!e->usable || e->dirty.load(std::memory_order_relaxed) || !TexPick(*e, size, &v) || TexSwapDueGuarded(*e)) {
      // A class, 0x678 or mip-set refusal stays until the texture changes; the rest is retried.
      if (e->usable || e->why == kTwSwap || e->why == kTwFault || e->dirty.load(std::memory_order_relaxed))
        AddRefreshT(j, t, e);
      if (why == kRecorded) why = kRTexUnusable;
      continue;
    }
    if (why != kRecorded) continue;
    sets[p.setCount++] = {e, tex, size, h};
    int q = 0;
    while (q < key->psTexCount && key->psTexH[q] != h) ++q;
    if (q < key->psTexCount) {
      views[q] = v;  // a repeated record: the last set wins, as stock
      found |= 1u << q;
    }
  }
  if (why == kRecorded && found != (1u << key->psTexCount) - 1) why = kRTexSlot;
  p.texCode = static_cast<uint8_t>(why);
  if (why != kRecorded) {
    p.setCount = 0;
    return;
  }
  j.tsetCount[t] += p.setCount;
  j.tviewCount[t] += static_cast<uint32_t>(key->psTexCount);
}

// Reads chunks of items until none is left (t: 0 primary, 1.. helpers).
void PreChunks(Job& j, int t) {
  shrec::LoadScope load(g_load);
  const uint32_t n = static_cast<uint32_t>(j.n);
  for (;;) {
    const uint32_t from = j.preNext.fetch_add(kPreChunk, std::memory_order_relaxed);
    if (from >= n) return;
    const uint32_t to = from + kPreChunk < n ? from + kPreChunk : n;
    // Recycled frame heap (the job's frame is over): the chunk counts as read
    // without touching DCS's renderables; the primary then drops the job.
    if (HeapRecycled(j))
      j.recycled.store(1, std::memory_order_relaxed);
    else
      for (uint32_t i = from; i < to; ++i) PreOne(j, t, i);
    j.preItems[t] += to - from;
    j.preDone.fetch_add(to - from, std::memory_order_release);
  }
}

// Primary: every chunk read (the helpers' last ones included). False after
// kPreWaitUs (a helper stalled: the job is not built). True at once when the
// job is recycled: a helper whose stage-A read faulted (contained) never
// counts its chunk, and the job is dropped anyway (the caller sees recycled);
// before, the primary waited the full kPreWaitUs holding the texture lock.
// Helpers still reading keep the slot busy (SlotBusy), as after a timeout.
bool WaitPre(Job& j) {
  const int64_t t0 = defrec::Qpc();
  const double k = defrec::QpcToUs();
  for (uint32_t spin = 0; j.preDone.load(std::memory_order_acquire) < j.n; ++spin) {
    if (j.recycled.load(std::memory_order_relaxed)) return true;
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

// Commit, pass 2 for one candidate: the material's decision, then the
// item's reads. kRecorded or why not.
int Pass2Commit(Job& j, size_t i) {
  Item& it = j.items[i];
  MatRec& m = j.mats[it.mat];
  const Pre& p = j.pre[i];
  if (m.excluded) return m.mixed ? kRCockpitMix : kRMatExcluded;
  if (j.shaders[it.shader].unclean) return kRUnclean;
  if (m.why < 0) DecideMat(j, m);
  if (m.why != kRecorded) {
    if (m.why == kRKeyPending || m.why == kRKeyCooling) WantProbe(j, i, m.shader, m.tech, p.mesh);
    return m.why;
  }
  if (!p.key || p.key != m.key || !p.pm || p.pm->shader != m.shader || PmTech(p.pm, m.ck) != m.tech)
    return kRKeyPending;  // read against another key state (published meanwhile)
  if (p.meshCode != kRecorded) {
    // A changed mesh is probed again unless an entry of it was superseded lately.
    if (p.meshCode == kRMeshPending ||
        (p.meshCode == kRMeshChanged &&
         !shrec::MeshCooling(j.tab->mesh, p.mesh, m.shader, m.tech, m.effect, m.techBegin, j.useGen)))
      WantProbe(j, i, m.shader, m.tech, p.mesh);
    return p.meshCode;
  }
  if (!p.page) return kRPage;
  if (p.texCode != kRecorded) return p.texCode;
  Cand& c = j.cands[j.candCount];
  c.key = m.key;
  c.mesh = p.me;
  c.page = p.page;
  c.item = static_cast<uint32_t>(i);
  c.pageIdx = p.pageIdx;
  c.setFirst = p.setFirst;
  c.viewFirst = p.viewFirst;
  c.setCount = p.setCount;
  c.thread = p.thread;
  c.alive = 1;
  it.cand = j.candCount++;
  return kRecorded;
}

int FinishBuild(Job& j);

// The serial commit (primary, in vector order, from the Pre slots): texture
// requests, materials and shaders (pass 1: stock inheritance per shader),
// the decisions (pass 2), the verify stride, the island rule and the
// segment limit, then FinishBuild. Plain.
int CommitBuild(Job& j) {
  NextStamp(j);
  j.ownItems = j.ownRuns = j.ownRecItems = 0;
  for (int t = 0; t < kThreads; ++t) {
    for (uint32_t k = 0; k < j.tmissCount[t]; ++k) AddMiss(j, j.tmiss[t][k].tex, j.tmiss[t][k].aux, j.tmiss[t][k].type);
    for (uint32_t k = 0; k < j.trefreshCount[t]; ++k) AddRefresh(j, j.trefresh[t][k]);
  }
  for (size_t i = 0; i < j.n; ++i) {
    Item& it = j.items[i];
    it = {~0u, ~0u, ~0u, static_cast<uint8_t>(kRNotModel), kNoSeg, 0, 0};
    const Pre& p = j.pre[i];
    // Stage 2: every SceneRenderable of the ownship instance (stage 1's test: any material, so also the ones the
    // recorder never records) is bracketed in the handed list; the recordable ones go to segments of their own (or stay
    // residual, kROwnship, with g_ownRecSegs off).
    j.own[i] = 0;
    if (j.ownKey) {
      const auto* r = static_cast<const uint8_t*>(j.snap[i]);
      if (r && *reinterpret_cast<void* const*>(r) == j.env->srVt) {
        const auto* item = *reinterpret_cast<const uint8_t* const*>(r + 0x10);
        if (item && *reinterpret_cast<void* const*>(item + 0x18) == j.ownKey) {
          j.own[i] = 1;
          ++j.ownItems;
        }
      }
    }
    if (p.kind != kPreModel) continue;
    uint32_t mi = 0;
    const bool ck = j.cockpit && p.cockpit;  // a cockpit candidate (cockpit jobs)
    MatRec* m = MatFor(j, p.mat, &mi, ck);
    if (!m) return j.result = kBuildOversize;  // a material untracked: its end state could not be restored
    it.mat = mi;
    it.shader = m->shaderIdx;
    m->last = static_cast<int32_t>(i);
    ShaderRec* sh = m->shaderIdx != ~0u ? &j.shaders[m->shaderIdx] : nullptr;
    if (!p.pass1 || m->transparent) {
      m->excluded = true;  // another route writes other material bytes
      if (sh) sh->unclean = 1;
      it.reason = static_cast<uint8_t>(!p.pass1 ? kRPass : kRTransparent);
      continue;
    }
    if (p.cockpit && !j.cockpit) {
      if (sh) sh->unclean = 1;  // normal_cockpit*: its pass index and reads are not analysed here
      it.reason = kRCockpit;
      continue;
    }
    if (j.cockpit && m->ck != ck) {  // one material, two techniques in this vector: DCS draws it
      m->excluded = m->mixed = true;
      if (sh) sh->unclean = 1;
      it.reason = kRCockpitMix;
      continue;
    }
    if (!m->shaderOk) {
      it.reason = kRNoShader;
      continue;
    }
    if (!sh) return j.result = kBuildOversize;
    it.writer = 1;
    // The ownship's items: recorded in segments of their own (g_ownRecSegs), else residual. Residual, they are
    // still a writer and a reader of its shader's FX variables, so its shader is DCS's entirely when one of them
    // inherits a variable (a residual draw after a recorded writer would read the stale value: live 2026-10-14,
    // AH-64D, verify found the ownship's geometry off in 1 of 25 compares).
    it.reason = static_cast<uint8_t>(j.own[i] && !j.ownRec ? kROwnship : kRecorded);  // candidate so far
    if (sh->unclean) continue;
    const ReadsEntry* rd =
        p.pm && p.pm->shader == m->shader && PmTech(p.pm, ck) == m->tech ? PmReads(p.pm, ck) : nullptr;
    if (!rd) {
      WantReads(j, m->shader, m->tech);
      sh->unclean = 1;
      continue;
    }
    if (rd->state <= 0 || p.inherit || !p.pageOk) sh->unclean = 1;
  }
  uint32_t candSeen = 0;
  for (size_t i = 0; i < j.n; ++i) {
    Item& it = j.items[i];
    if (it.reason != kRecorded) continue;
    int why = Pass2Commit(j, i);
    if (why == kRecorded && j.stride > 1 && (candSeen++ % j.stride) == j.stride - 1) {
      why = kRForced;
      j.cands[it.cand].alive = 0;
      it.cand = ~0u;
    }
    it.reason = static_cast<uint8_t>(why);
  }
  for (size_t i = 0; i < j.n; ++i) {
    j.flag[i] = j.items[i].reason == kRecorded ? (j.own[i] ? 2 : 1) : 0;  // 2: an ownship segment's
    j.segWhy[i] = 0;
  }
  uint32_t runs[kMaxSegments][2];
  const uint32_t island = j.stride > 1 ? (std::min)(j.island, j.stride - 1) : j.island;
  const int maxSeg = static_cast<int>(j.maxSeg < 1 ? 1 : j.maxSeg > kMaxSegments ? kMaxSegments : j.maxSeg);
  const int nseg = PlanSegments(j.flag, j.n, island ? island : 1, maxSeg, runs, j.segWhy);
  for (size_t i = 0; i < j.n; ++i) {
    if (!j.segWhy[i]) continue;
    Item& it = j.items[i];
    it.reason = static_cast<uint8_t>(j.segWhy[i] == 1 ? kRIsland : kRSegments);
    if (it.cand != ~0u) j.cands[it.cand].alive = 0;
    it.cand = ~0u;
  }
  j.segCount = static_cast<uint32_t>(nseg);
  for (int k = 0; k < nseg; ++k) {
    Seg& s = j.segs[k];
    s.first = runs[k][0];
    s.end = runs[k][1];
    s.candFirst = j.items[s.first].cand;
    s.candEnd = j.items[s.end - 1].cand + 1;
    for (uint32_t i = s.first; i < s.end; ++i) j.items[i].seg = static_cast<uint8_t>(k);
  }
  for (size_t i = 0; i < j.n; ++i) ++j.reasons[j.items[i].reason];
  return FinishBuild(j);
}

// Stage A. CPU only: DCS memory is read, never written; no D3D call; texture
// views come from the table (the caller holds its lock shared). Plain (the
// pool runs it under SEH).
int BeginBuild(Job& j) {
  j.matCount = j.shaderCount = j.candCount = j.segCount = j.pieceCount = 0;
  for (int t = 0; t < kThreads; ++t) {
    j.tsetCount[t] = j.tviewCount[t] = j.tmissCount[t] = j.trefreshCount[t] = j.preItems[t] = 0;
    j.recUs[t] = 0;
    j.cbBytesT[t] = 0;
    j.tRecDone[t] = 0;
  }
  j.preNext.store(0, std::memory_order_relaxed);
  j.preDone.store(0, std::memory_order_relaxed);
  j.preUs = j.commitUs = 0;
  j.tPreDone = j.tBuilt = 0;
  j.entCount = j.pairCount = j.segMatCount = j.pageCount = j.matRestCount = j.varRestCount = 0;
  j.wantCount = j.readWantCount = j.missCount = j.refreshCount = 0;
  j.recorded = j.draws = 0;
  j.cbBytes = 0;
  j.buildUs = j.recordUs = 0;
  j.swapCount = 0;
  memset(j.reasons, 0, sizeof(j.reasons));
  for (Seg& s : j.segs) s = Seg();
  for (Piece& p : j.pieces) {
    SafeRel(p.cl);
    p = Piece();
  }
  j.n = 0;
  j.begin = nullptr;
  j.result = kBuildNone;
  auto** begin = static_cast<void**>(j.vec[0]);
  auto** end = static_cast<void**>(j.vec[1]);
  j.begin = begin;
  if (!begin || end < begin) return j.result = kBuildNoVector;
  const size_t n = static_cast<size_t>(end - begin);
  if (n > kMaxItems) return j.result = kBuildOversize;
  memcpy(j.snap, begin, n * sizeof(void*));
  j.n = n;
  if (++j.gen == 0) {
    memset(j.matGen, 0, sizeof(j.matGen));
    memset(j.shGen, 0, sizeof(j.shGen));
    memset(j.meshGen, 0, sizeof(j.meshGen));
    memset(j.wantGen, 0, sizeof(j.wantGen));
    memset(j.preMats, 0, sizeof(j.preMats));
    memset(j.tdd, 0, sizeof(j.tdd));
    j.gen = 1;
  }
  return kBuildOk;
}

// The per-segment unique lists, the restore lists and the caster list.
int FinishBuild(Job& j) {
  const Env& env = *j.env;
  for (Seg& s : j.segs) s.draws = 0;
  for (uint32_t k = 0; k < j.segCount; ++k) {
    Seg& s = j.segs[k];
    s.entFirst = j.entCount;
    s.pairFirst = j.pairCount;
    s.matFirst = j.segMatCount;
    s.pageFirst = j.pageCount;
    s.tris = 0;
    NextStamp(j);
    for (uint32_t c = s.candFirst; c < s.candEnd; ++c) {
      Cand& cd = j.cands[c];
      if (!cd.alive) continue;
      ++s.draws;
      Item& it = j.items[cd.item];
      MatRec& m = j.mats[it.mat];
      ++m.rec;
      ++j.shaders[it.shader].rec;
      s.tris += *reinterpret_cast<uint64_t*>(static_cast<uint8_t*>(j.snap[cd.item]) + 0x78);
      if (m.segStamp != j.ddStamp) {
        m.segStamp = j.ddStamp;
        if (j.segMatCount == kMaxSegMats) return j.result = kBuildOversize;
        j.segMats[j.segMatCount++] = it.mat;
      }
      if (DdInsert(j, reinterpret_cast<uintptr_t>(cd.page), 3)) {
        if (j.pageCount == kMaxSegPages) return j.result = kBuildOversize;
        j.segPages[j.pageCount++] = {cd.pageIdx, cd.page};
      }
      for (uint32_t q = 0; q < cd.setCount; ++q) {
        const SetRef& sr = SetsOf(j, cd)[q];
        if (DdInsert(j, reinterpret_cast<uintptr_t>(sr.e), 1)) {
          if (j.entCount == kMaxSegEnts) return j.result = kBuildOversize;
          j.segEntVer[j.entCount] = sr.e->version.load(std::memory_order_relaxed);
          j.segEnts[j.entCount++] = sr.e;
          const_cast<TexEntry*>(sr.e)->lastUse.store(j.useGen, std::memory_order_relaxed);
        }
        if (DdInsert(j, reinterpret_cast<uintptr_t>(sr.tex), sr.size ^ 0x5a5a000000000000ull)) {
          if (j.pairCount == kMaxSegPairs) return j.result = kBuildOversize;
          j.segPairs[j.pairCount++] = {sr.tex, sr.size};
        }
      }
    }
    s.entEnd = j.entCount;
    s.pairEnd = j.pairCount;
    s.matEnd = j.segMatCount;
    s.pageEnd = j.pageCount;
    j.draws += s.draws;
  }
  j.recorded = j.draws;
  // Materials whose last item here is recorded: their end state after the pass.
  for (uint32_t mi = 0; mi < j.matCount; ++mi) {
    const MatRec& m = j.mats[mi];
    if (!m.rec || m.last < 0) continue;
    const Item& it = j.items[m.last];
    if (it.seg == kNoSeg) continue;
    if (j.matRestCount == kMaxRestores) return j.result = kBuildOversize;
    j.matRest[j.matRestCount++] = {mi, static_cast<uint32_t>(m.last), it.seg};
  }
  // FX variables whose last writer here is recorded (reverse scan over the
  // writers of the shaders with recorded draws: the first sight is the last
  // writer in vector order).
  NextStamp(j);
  for (size_t i = j.n; i-- > 0;) {
    const Item& it = j.items[i];
    if (!it.writer || it.shader == ~0u || !j.shaders[it.shader].rec) continue;
    const MatRec& m = j.mats[it.mat];
    auto* r = static_cast<uint8_t*>(j.snap[i]);
    auto* item = *reinterpret_cast<uint8_t**>(r + 0x10);
    const bool recorded = it.seg != kNoSeg;
    const Cand* cd = recorded ? &j.cands[it.cand] : nullptr;
    auto note = [&](uint8_t* var, void* value, const TexEntry* te = nullptr, uint64_t tsize = 0) -> bool {
      if (!var) return true;
      if (*reinterpret_cast<void**>(var) == nullptr ||
          (*reinterpret_cast<void***>(var))[31] != env.setResource)
        return false;
      if (!DdInsert(j, reinterpret_cast<uintptr_t>(var), 7)) return true;
      if (!recorded) return true;
      if (j.varRestCount == kMaxRestores) return false;
      j.varRest[j.varRestCount++] = {var, value, it.seg, te, tsize};
      return true;
    };
    const uint32_t page = *reinterpret_cast<uint32_t*>(item + 0xd0);
    if (PageBuffer(env, page)) {
      const int64_t hsb = *reinterpret_cast<int64_t*>(m.mat + 0x68);
      if (!note(VarOf(m.shader, hsb), recorded ? static_cast<void*>(cd->page) : nullptr))
        return j.result = kBuildVarClass;
    }
    const uint32_t ntex = *reinterpret_cast<uint32_t*>(m.mat + 0x2d8);
    const uint8_t* en = ntex ? EntriesOf(m.mat, item) : nullptr;
    for (uint32_t k = 0; k < ntex && k < kMaxTex && en; ++k) {
      const int64_t h = *reinterpret_cast<int64_t*>(m.mat + 0x240 + 8 * k);
      if (h == -1 || !*reinterpret_cast<void* const*>(en + k * 0x18 + 8)) continue;
      void* value = nullptr;
      const TexEntry* te = nullptr;
      uint64_t tsize = 0;
      if (recorded) {
        // The set's entry (Pass2One kept every set with a texture, in handle order).
        for (uint32_t q = 0; q < cd->setCount; ++q) {
          const SetRef& sr = SetsOf(j, *cd)[q];
          if (sr.h == h) {
            ID3D11ShaderResourceView* v = nullptr;
            TexPick(*sr.e, sr.size, &v);
            value = v;  // the last set of h wins
            te = sr.e;
            tsize = sr.size;
          }
        }
      }
      if (!note(VarOf(m.shader, h), value, te, tsize)) return j.result = kBuildVarClass;
    }
  }
  // DCS's loop: [front, residual items and one exec entry per segment, in order].
  // Stage 2: each run of ownship items bracketed by the switch entries (an exec entry or another residual item
  // closes the run).
  j.swapCount = 0;
  j.ownRuns = 0;
  j.ownRecItems = 0;
  j.swapList[j.swapCount++] = j.front;
  const bool wrap = j.ownItems && g_ownInE && g_ownOutE;
  bool open = false;
  for (size_t i = 0; i < j.n; ++i) {
    const uint8_t s = j.items[i].seg;
    const bool own = wrap && j.own[i];
    if (own && s != kNoSeg) ++j.ownRecItems;
    if (s != kNoSeg && i != j.segs[s].first) continue;
    if (own != open) {
      j.swapList[j.swapCount++] = own ? g_ownInE : g_ownOutE;
      open = own;
      if (own) ++j.ownRuns;
    }
    j.swapList[j.swapCount++] = s == kNoSeg ? j.snap[i] : j.execObj[s];
  }
  if (open) j.swapList[j.swapCount++] = g_ownOutE;
  return j.result = kBuildOk;
}

// Stage A on one thread (the offline tests; workers split PreChunks).
int BuildJob(Job& j) {
  if (BeginBuild(j) != kBuildOk) return j.result;
  PreChunks(j, 0);
  return CommitBuild(j);
}

// ---------------------------------------------------------------------------
// Recording (worker)
// ---------------------------------------------------------------------------
// The CB bytes DCS's draw uploads for candidate c (R18 3.2): the material
// snapshot, +0xfc = [item+0xd4], the animated properties by DCS's own vt[9]
// into the window, +0x20 = item+0x60..0x9c, +0x80 = the learned 12 bytes.
void BuildWindow(const Job& j, const Cand& c, uint8_t* w) {
  const Item& it = j.items[c.item];
  const MatRec& m = j.mats[it.mat];
  auto* r = static_cast<uint8_t*>(j.snap[c.item]);
  auto* item = *reinterpret_cast<uint8_t**>(r + 0x10);
  memcpy(w, m.cb, kCbLen);
  memcpy(w + kPsoCb, item + 0xd4, 4);
  if (m.animN) {
    const uint8_t* b = *reinterpret_cast<uint8_t* const*>(m.mat + 8);
    for (uint32_t k = 0; k < m.animN; ++k) {
      const uint8_t* p = b + 16 * k;
      uint8_t* dst = *reinterpret_cast<uint8_t* const*>(p);
      void* prop = *reinterpret_cast<void* const*>(p + 8);
      alignas(16) uint8_t span[16];
      memcpy(span, item, 16);
      (*reinterpret_cast<PropFn**>(prop))[9](prop, span, w + (dst - m.mat - kCbOff));
    }
  }
  memcpy(w + kMatrixCb, item + 0x60, kMatrixLen);
  memcpy(w + kRatioCb, j.ratio, kRatioLen);
}

// One piece on the worker's deferred context: the frame buffer's state and
// DCS's sampler pool (s5-s15, checked at every exec entry), then every draw in
// vector order with exactly the bindings DCS's draw leaves for its shaders
// (bindings equal to the previous draw's are not repeated). A key binds only
// its FX sampler dependencies below the pool (all slots below it when those
// were not read); its probe proved the pool slots equal the pool.
bool RecordRange(defrec::Worker& w, Job& j, const Piece& s, int role) {
  shrec::LoadScope load(g_load);
  ID3D11DeviceContext* dc = w.dc;
  alignas(16) uint8_t win[kCbLen];
  for (uint32_t c = s.candFirst; c < s.candEnd; ++c) {
    const Cand& cd = j.cands[c];
    if (!cd.alive) continue;
    if (HeapRecycled(j)) {  // BuildWindow reads the renderable and its item: not after the heap reset
      j.recycled.store(1, std::memory_order_relaxed);
      return false;
    }
    if (s.redo && j.redoStop.load(std::memory_order_acquire)) return false;  // its pass is over (issue #3)
    defrec::CbSlice sl = w.ring.Alloc(kCbLen);
    if (!sl) return false;
    BuildWindow(j, cd, win);
    memcpy(sl.ptr, win, kCbLen);
    j.slices[c] = sl;
    j.cbBytesT[role] += static_cast<uint64_t>(sl.num) * 16;
  }
  w.ring.Close();
  dc->OMSetRenderTargets(kRtv, j.rtv, j.dsv);
  dc->RSSetViewports(j.nvp, j.vp);
  dc->RSSetScissorRects(j.nsc, j.nsc ? j.sc : nullptr);
  for (UINT k = 0; k < 3; ++k) {
    if (j.ctxBuf[0][k]) dc->VSSetConstantBuffers(kCtx0 + k, 1, &j.ctxBuf[0][k]);
    if (j.ctxBuf[1][k]) dc->PSSetConstantBuffers(kCtx0 + k, 1, &j.ctxBuf[1][k]);
  }
  const GbKey* ck = nullptr;
  const shrec::MeshEntry* cm = nullptr;
  auto* const kNone = reinterpret_cast<void*>(1);  // never equal to a binding
  void *vs = kNone, *ps = kNone, *dss = kNone, *bs = kNone, *rs = kNone, *il = kNone, *vb = kNone, *ib = kNone;
  void* vsSamp[kSampSlots];
  void* psSamp[kSampSlots];
  for (auto*& p : vsSamp) p = kNone;
  for (auto*& p : psSamp) p = kNone;
  dc->VSSetSamplers(kPoolFirst, kSampSlots - kPoolFirst, &j.pool[0][kPoolFirst]);
  dc->PSSetSamplers(kPoolFirst, kSampSlots - kPoolFirst, &j.pool[1][kPoolFirst]);
  for (UINT q = kPoolFirst; q < kSampSlots; ++q) vsSamp[q] = j.pool[0][q], psSamp[q] = j.pool[1][q];
  void* vsv[kSrvSlots];
  void* psv[kSrvSlots];
  for (auto*& v : vsv) v = kNone;
  for (auto*& v : psv) v = kNone;
  UINT ref = 0, mask = 0, stride = 0;
  float factor[4] = {};
  DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN;
  bool topo = false;
  for (uint32_t c = s.candFirst; c < s.candEnd; ++c) {
    const Cand& cd = j.cands[c];
    if (!cd.alive) continue;
    const GbKey& k = *cd.key;
    const shrec::MeshEntry& m = *cd.mesh;
    if (&k != ck) {
      ck = &k;
      if (k.vs != vs) dc->VSSetShader(k.vs, nullptr, 0), vs = k.vs;
      if (k.ps != ps) dc->PSSetShader(k.ps, nullptr, 0), ps = k.ps;
      const uint32_t vm = KeyOwnSamplers(k.vsSampDeps), pm = KeyOwnSamplers(k.psSampDeps);
      for (UINT q = 0; q < kPoolFirst; ++q) {
        if (((vm >> q) & 1) && k.vsSamp[q] != vsSamp[q]) dc->VSSetSamplers(q, 1, &k.vsSamp[q]), vsSamp[q] = k.vsSamp[q];
        if (((pm >> q) & 1) && k.psSamp[q] != psSamp[q]) dc->PSSetSamplers(q, 1, &k.psSamp[q]), psSamp[q] = k.psSamp[q];
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
    ID3D11ShaderResourceView* page = cd.page;
    if (vsv[k.st.sbSlot] != page) dc->VSSetShaderResources(k.st.sbSlot, 1, &page), vsv[k.st.sbSlot] = page;
    for (int w2 = 0; w2 < 2; ++w2)
      for (uint64_t bits = k.psSbMask[w2]; bits; bits &= bits - 1) {
        unsigned long b;
        _BitScanForward64(&b, bits);
        const UINT slot = static_cast<UINT>(w2 * 64 + b);
        if (psv[slot] != page) dc->PSSetShaderResources(slot, 1, &page), psv[slot] = page;
      }
    for (int q = 0; q < k.psTexCount; ++q) {
      const UINT slot = k.psTexSlot[q];
      ID3D11ShaderResourceView* v = ViewsOf(j, cd)[q];
      if (psv[slot] != v) dc->PSSetShaderResources(slot, 1, &v), psv[slot] = v;
    }
    const defrec::CbSlice& sl = j.slices[c];
    w.ring.BindVS(k.st.cbSlot, sl);
    for (uint32_t bits = k.psCbMask; bits; bits &= bits - 1) {
      unsigned long slot;
      _BitScanForward(&slot, bits);
      w.ring.BindPS(slot, sl);
    }
    dc->DrawIndexed(m.indexCount, 0, 0);
  }
  return true;
}

// The pieces: the recorded draws, in vector order, cut into `workers` runs of
// about equal draw counts (worker r records run r); a run that spans segments
// is one piece per segment. Each segment's pieces stay in order, so its exec
// entry executes them one after the other (one Execute more per cut).
void PlanPieces(Job& j, uint32_t workers) {
  j.pieceCount = 0;
  if (workers < 1) workers = 1;
  const uint32_t quota = (j.draws + workers - 1) / workers;
  uint32_t role = 0, acc = 0;
  for (uint32_t k = 0; k < j.segCount; ++k) {
    Seg& s = j.segs[k];
    s.pieceFirst = j.pieceCount;
    uint32_t c = s.candFirst;
    while (c < s.candEnd) {
      Piece& p = j.pieces[j.pieceCount++];
      p = Piece();
      p.seg = static_cast<uint8_t>(k);
      p.role = static_cast<uint8_t>(role);
      p.candFirst = c;
      while (c < s.candEnd && (acc < quota || role + 1 == workers)) {
        if (j.cands[c].alive) ++acc, ++p.draws;
        ++c;
      }
      p.candEnd = c;
      if (acc >= quota && role + 1 < workers) {
        ++role;
        acc = 0;
      }
    }
    s.pieceEnd = j.pieceCount;
  }
  j.recWorkers = role + 1;
}

// Worker `role`'s pieces, each finished into its own list. False: a piece
// could not be recorded (the job is not used).
bool RecordRole(defrec::Worker& w, Job& j, int role) {
  if (!w.dc1 || w.ring.Mode() != defrec::CbMode::kOffsets) return false;
  for (uint32_t k = 0; k < j.pieceCount; ++k) {
    Piece& p = j.pieces[k];
    if (p.role != role) continue;
    if (j.recFail.load(std::memory_order_relaxed)) return false;
    w.ring.Reset();
    if (!RecordRange(w, j, p, role)) return false;
    w.ring.Close();
    if (FAILED(w.dc->FinishCommandList(FALSE, &p.cl)) || !p.cl) return false;
  }
  return true;
}

// A segment re-recorded during its pass with the job's rebuilt texture
// entries (RedoArg; one list, finished here). The texture table's lock is
// held shared, as for any recording (the unchanged entries' views stay alive).
bool RedoRecord(defrec::Worker& w, Job& j, uint32_t k, int role) {
  if (!w.dc1 || w.ring.Mode() != defrec::CbMode::kOffsets) return false;
  const Seg& sg = j.segs[k];
  Piece p = Piece();
  p.candFirst = sg.candFirst;
  p.candEnd = sg.candEnd;
  p.draws = sg.draws;
  p.seg = static_cast<uint8_t>(k);
  p.role = static_cast<uint8_t>(role);
  p.redo = 1;
  if (HeapRecycled(j) || j.redoStop.load(std::memory_order_acquire)) return false;  // queued past its pass
  w.ring.Reset();
  const bool ok = RecordRange(w, j, p, role);
  w.ring.Close();
  ID3D11CommandList* cl = nullptr;
  if (FAILED(w.dc->FinishCommandList(FALSE, &cl)) || !cl) return false;
  if (!ok) {
    cl->Release();  // a partial list is never executed
    return false;
  }
  j.redoCl[k] = cl;
  return true;
}

// Issue #3 (0.5.8, "gbuffer recorder: disabled for this session (exception
// 0xc0000005 at +0x10bec8 in worker 7, data address 0xffffffffffffffff)",
// stack RecordRange <- RedoMain <- Pool::RunGuarded): a re-record reads the
// job's renderables like JobMain and the helpers do, and can lose the same
// race against DCS's frame-heap reset between RecordRange's generation check
// and BuildWindow's read of [renderable+0x10]+0xd4 (a recycled renderable:
// the item pointer is another allocation's bytes, here non-canonical). JobMain
// and HelperMain contain that fault per job (RecycledFilter); RedoMain did
// not, so the pool latched the whole recorder off. Now contained the same way:
// the segment is drawn stock (or already was), the recorder stays on.
bool RedoMain(defrec::Worker& w, void* u) {
  RedoArg& a = *static_cast<RedoArg*>(u);
  Job& j = *a.j;
  bool ok = false;
  if (j.texLock) AcquireSRWLockShared(j.texLock);
  __try {
   __try {
    ok = RedoRecord(w, j, a.seg, a.worker);
   } __except (RecycledFilter(GetExceptionCode(), j)) {
    ok = false;
    g_redoFaultsContained.fetch_add(1, std::memory_order_relaxed);
   }
  } __finally {
    if (j.texLock) ReleaseSRWLockShared(j.texLock);
    j.redoState[a.seg].store(ok ? kRedoOk : kRedoFail, std::memory_order_release);
  }
  return false;  // the list is the job's (redoCl), the pool's stays empty
}

// Starts an armed job on vector `vec` (the sort hook on a pool thread, or the
// render thread at RenderGraph::render entry; the first one wins).
bool StartJob(Job& j, void** vec, bool early) {
  int st = kStartArmed;
  if (!j.startState.compare_exchange_strong(st, kStartRun, std::memory_order_acq_rel)) return false;
  j.heapGen = g_frameHeapGen.load(std::memory_order_acquire);
  j.recycled.store(0, std::memory_order_relaxed);
  j.vec = vec;
  j.ownKey = g_ownKey.load(std::memory_order_acquire);
  j.ownRec = g_ownRecSegs.load(std::memory_order_relaxed);
  j.startedEarly = early;
  j.tStart = defrec::Qpc();
  SetEvent(j.start);
  return true;
}

// Ends the helpers' waits (nothing (more) to do).
void ReleaseHelpers(Job& j) {
  int st = kGoWait;
  if (j.preState.compare_exchange_strong(st, kGoAbort, std::memory_order_acq_rel)) SetEvent(j.preGo);
  st = kGoWait;
  if (j.recState.compare_exchange_strong(st, kGoAbort, std::memory_order_acq_rel)) SetEvent(j.recGo);
}

// Stage A on the primary after BeginBuild (JobMain; R73's offline test): the
// helpers let go, its own chunks, the wait for theirs, then the commit unless
// a chunk was not read (recycled heap, an implausible item pointer). The
// job's result (kBuildOk when committed).
int PreAndCommit(Job& j, int64_t t0) {
  j.preState.store(kGoRun, std::memory_order_release);
  SetEvent(j.preGo);
  PreChunks(j, 0);
  j.tPreDone = defrec::Qpc();
  j.preUs = (j.tPreDone - t0) * defrec::QpcToUs();
  if (!WaitPre(j))
    j.result = kBuildPreLate;
  else if (j.recycled.load(std::memory_order_relaxed) || HeapRecycled(j))
    j.result = kBuildRecycled;  // some chunks were not read: never committed
  else
    CommitBuild(j);
  return j.result;
}

// The primary worker: the start (sort hook or render entry), stage A with
// the helpers (reads in chunks, then the serial commit), the pieces, its own
// recording. The helpers record theirs on their own contexts. Every list is
// finished by the worker that recorded it (the pool's own list stays empty).
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
    ReleaseHelpers(j);
    j.phase.store(kJobEmpty, std::memory_order_release);
    return false;
  }
  const int64_t t0 = defrec::Qpc();
  if (j.texLock) AcquireSRWLockShared(j.texLock);
  // The lock is released and the helpers let go even when a DCS read faults
  // (the pool latches off then).
  __try {
   __try {
    if (BeginBuild(j) == kBuildOk) PreAndCommit(j, t0);
    j.tBuilt = defrec::Qpc();
    j.commitUs = (j.tBuilt - (j.tPreDone ? j.tPreDone : t0)) * defrec::QpcToUs();
    j.buildUs = (j.tBuilt - t0) * defrec::QpcToUs();
    if (j.result == kBuildOk && j.segCount) {
      // Helpers that read chunks are at (or about to reach) the recording barrier.
      const int64_t b0 = defrec::Qpc();
      while (j.ready.load(std::memory_order_acquire) < j.started.load(std::memory_order_acquire) &&
             (defrec::Qpc() - b0) * defrec::QpcToUs() < 100.0)
        _mm_pause();
      uint32_t workers = 1 + j.ready.load(std::memory_order_acquire);
      if (workers > static_cast<uint32_t>(kThreads)) workers = kThreads;
      if (j.draws < j.splitMin) workers = 1;
      PlanPieces(j, workers);
      j.recState.store(kGoRun, std::memory_order_release);  // publishes the pieces and recWorkers
      SetEvent(j.recGo);
      const int64_t r0 = defrec::Qpc();
      if (!RecordRole(w, j, 0)) j.recFail.store(1, std::memory_order_release);
      j.tRecDone[0] = defrec::Qpc();
      j.recUs[0] = (j.tRecDone[0] - r0) * defrec::QpcToUs();
    }
   } __except (RecycledFilter(GetExceptionCode(), j)) {
    j.recycled.store(1, std::memory_order_relaxed);
    j.recFail.store(1, std::memory_order_release);
    if (j.result == kBuildOk || j.result == kBuildNone) j.result = kBuildRecycled;
   }
    if (j.recycled.load(std::memory_order_relaxed)) {
      g_recycledJobs.fetch_add(1, std::memory_order_relaxed);
      j.recFail.store(1, std::memory_order_release);
    }
  } __finally {
    ReleaseHelpers(j);
    if (j.texLock) ReleaseSRWLockShared(j.texLock);
    j.tDone = defrec::Qpc();
    j.phase.store(kJobBuilt, std::memory_order_release);
  }
  return false;
}

// Helper t (1..kThreads-1): stage-A chunks, then its recording role (by
// arrival at the barrier) when the primary planned pieces for it.
template <int T>
bool HelperMain(defrec::Worker& w, void* u) {
  Job& j = *static_cast<Job*>(u);
  WaitForSingleObject(j.preGo, kStartWaitMs + 50);
  if (j.preState.load(std::memory_order_acquire) != kGoRun) return false;
  j.started.fetch_add(1, std::memory_order_acq_rel);
  if (j.texLock) AcquireSRWLockShared(j.texLock);
  __try {
   __try {
    PreChunks(j, T);
    const uint32_t role = j.ready.fetch_add(1, std::memory_order_acq_rel) + 1;
    WaitForSingleObject(j.recGo, 200);
    if (j.recState.load(std::memory_order_acquire) == kGoRun && role < j.recWorkers) {
      const int64_t r0 = defrec::Qpc();
      if (!RecordRole(w, j, static_cast<int>(role))) j.recFail.store(1, std::memory_order_release);
      j.tRecDone[role] = defrec::Qpc();
      j.recUs[role] = (j.tRecDone[role] - r0) * defrec::QpcToUs();
    }
   } __except (RecycledFilter(GetExceptionCode(), j)) {
    // A fault in stage A: its chunk is never counted; the primary's WaitPre
    // stops waiting on recycled (the job fails).
    j.recycled.store(1, std::memory_order_relaxed);
    j.recFail.store(1, std::memory_order_release);
   }
  } __finally {
    if (j.texLock) ReleaseSRWLockShared(j.texLock);
  }
  return false;
}

// ---------------------------------------------------------------------------
// Render thread: checks, streaming replay, restores
// ---------------------------------------------------------------------------
// The vector is still the job's (identity; the pass compares every pointer).
bool SameVectorRaw(const Job& j, void** vec) {
  auto** b = static_cast<void**>(vec[0]);
  auto** e = static_cast<void**>(vec[1]);
  return vec == j.vec && b == j.begin && e >= b && static_cast<size_t>(e - b) == j.n &&
         memcmp(j.snap, b, j.n * sizeof(void*)) == 0;
}
bool SameVectorGuarded(const Job& j, void** vec) {
  __try {
    return SameVectorRaw(j, vec);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

// One streaming request per (texture, size) of the segment (slot 26's vt[23]).
uint32_t ReplayRaw(const Job& j, const Seg& s) {
  for (uint32_t k = s.pairFirst; k < s.pairEnd; ++k) {
    uint8_t* tex = j.segPairs[k].tex;
    (*reinterpret_cast<shrec::Tex23Fn**>(tex))[23](tex, j.segPairs[k].size);
  }
  return s.pairEnd - s.pairFirst;
}

// Segment checks after the replay: textures (every failing entry is marked
// dirty for the rebuild after the pass), material dwords, page views.
int CheckSegmentRaw(const Job& j, const Seg& s) {
  int why = kSExecuted;
  const gbbatch::TexEnv& tenv = j.env->tex;
  for (uint32_t k = s.entFirst; k < s.entEnd; ++k) {
    const TexEntry& e = *j.segEnts[k];
    if (e.version.load(std::memory_order_relaxed) != j.segEntVer[k] ||
        e.slot.load(std::memory_order_relaxed) != kTeLive) {
      why = kSTexture;  // rebuilt or evicted since the job read it
      continue;
    }
    if (TexCheckRaw(tenv, e)) continue;
    const_cast<TexEntry&>(e).dirty.store(1, std::memory_order_relaxed);
    why = kSTexture;
  }
  if (why) return why;
  for (uint32_t k = s.matFirst; k < s.matEnd; ++k) {
    const MatRec& m = j.mats[j.segMats[k]];
    for (int w = 0; w < 2; ++w)
      for (uint64_t bits = m.guard[w]; bits; bits &= bits - 1) {
        unsigned long d;
        _BitScanForward64(&d, bits);
        const uint32_t off = (static_cast<uint32_t>(w) * 64 + d) * 4;
        if (off + 4 <= kCbLen && memcmp(m.mat + kCbOff + off, m.cb + off, 4) != 0) return kSGuard;
      }
  }
  for (uint32_t k = s.pageFirst; k < s.pageEnd; ++k)
    if (PageSrv(*j.env, j.segPages[k].page) != j.segPages[k].srv) return kSPage;
  return kSExecuted;
}

// After the checks, before the list: slot 26's vt[18] of the segment's
// render-target textures (their one-time mip generation, as their first draw
// in the segment would do it; shadow_rec.h InitInnerRt). Returns the calls.
uint32_t ReplayRtRaw(const Job& j, const Seg& s) {
  uint32_t n = 0;
  for (uint32_t k = s.entFirst; k < s.entEnd; ++k)
    if (j.segEnts[k]->rt) {
      ReplayVt18(j.segEnts[k]->tex);
      ++n;
    }
  return n;
}

bool ReplayRtGuarded(const Job& j, const Seg& s, uint32_t* calls) {
  __try {
    *calls += ReplayRtRaw(j, s);
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

// Cockpit jobs (R24 3.4): segment s's render-target textures (MFD and
// indicator targets): counted, with those DCS drew to since their last mip
// generation (vt[18]'s condition: [inner+0x54] 0 and [[inner+8]+0x44] & 0x10
// [V shadow_rec.h InitInnerRt]); one whose view is a view of a target of the
// pass (a feedback loop D3D would resolve on each context) makes the segment
// stock. Their contents are not recorded: the list samples them when it
// executes, at the stock draws' place, after every earlier writer. Plain.
int RtCheckRaw(const Job& j, const Seg& s, uint32_t* seen, uint32_t* drawn) {
  ID3D11Resource* tgt[kRtv + 1] = {};
  bool haveTgt = false;
  int why = kSExecuted;
  for (uint32_t k = s.entFirst; k < s.entEnd && !why; ++k) {
    const TexEntry* e = j.segEnts[k];
    if (!e->rt) continue;
    ++*seen;
    const uint8_t* inner = *reinterpret_cast<uint8_t* const*>(e->tex + 0x10);
    if (inner && !inner[0x54]) {
      const uint8_t* d = *reinterpret_cast<const uint8_t* const*>(inner + 8);
      if (d && (d[0x44] & 0x10)) ++*drawn;
    }
    if (!haveTgt) {
      for (UINT q = 0; q < kRtv; ++q)
        if (j.rtv[q]) j.rtv[q]->GetResource(&tgt[q]);
      if (j.dsv) j.dsv->GetResource(&tgt[kRtv]);
      haveTgt = true;
    }
    for (int v = -1; v < static_cast<int>(e->nviews) && v < kTexViews && !why; ++v) {
      auto* view = v < 0 ? static_cast<ID3D11ShaderResourceView*>(e->g190) : e->views[v];
      if (!view) continue;
      ID3D11Resource* r = nullptr;
      view->GetResource(&r);
      for (ID3D11Resource* t : tgt)
        if (r && t == r) why = kSRtBound;
      SafeRel(r);
    }
  }
  for (auto*& t : tgt) SafeRel(t);
  return why;
}

int RtCheckGuarded(const Job& j, const Seg& s, uint32_t* seen, uint32_t* drawn) {
  __try {
    return RtCheckRaw(j, s, seen, drawn);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return kSFault;
  }
}

int ReplayAndCheckGuarded(const Job& j, const Seg& s, uint32_t* replays) {
  __try {
    *replays += ReplayRaw(j, s);
    return CheckSegmentRaw(j, s);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return kSFault;
  }
}

const TexEntry* RedoOf(const Job& j, const TexEntry* e);

// After the pass, for the executed segments: the triangle counter, the
// materials' and the FX variables' end state as DCS's stock loop leaves them.
void RestoreRaw(const Job& j, const uint8_t* executed) {
  uint64_t tris = 0;
  for (uint32_t k = 0; k < j.segCount; ++k)
    if (executed[k]) tris += j.segs[k].tris;
  if (tris && j.env->globals && *j.env->globals)
    _InterlockedExchangeAdd64(reinterpret_cast<volatile long long*>(*j.env->globals + 0x1e8),
                              static_cast<long long>(tris));
  for (uint32_t k = 0; k < j.matRestCount; ++k) {
    const MatRestore& mr = j.matRest[k];
    if (!executed[mr.seg]) continue;
    const MatRec& m = j.mats[mr.mat];
    auto* r = static_cast<uint8_t*>(j.snap[mr.item]);
    auto* item = *reinterpret_cast<uint8_t**>(r + 0x10);
    *reinterpret_cast<uint32_t*>(m.mat + 0x18c) = *reinterpret_cast<uint32_t*>(item + 0xd4);
    if (m.animN && j.env->evalAnim) {
      alignas(16) uint8_t span[16];
      memcpy(span, item, 16);
      j.env->evalAnim(m.mat, span);
    }
    memcpy(m.mat + kCbOff + kMatrixCb, item + 0x60, kMatrixLen);
    memcpy(m.mat + kCbOff + kRatioCb, j.ratio, kRatioLen);
  }
  for (uint32_t k = 0; k < j.varRestCount; ++k) {
    const VarRestore& vr = j.varRest[k];
    if (!executed[vr.seg]) continue;
    void* value = vr.value;
    if (vr.e) {
      // The entry the segment's list drew with (PlanSegRaw's re-record chain).
      const TexEntry* e = vr.e;
      for (const TexEntry* r = RedoOf(j, e); r; r = RedoOf(j, e)) e = r;
      ID3D11ShaderResourceView* v = nullptr;
      if (TexPick(*e, vr.size, &v)) value = v;
    }
    (*reinterpret_cast<SetResFn**>(vr.var))[31](vr.var, value);
  }
}

bool RestoreGuarded(const Job& j, const uint8_t* executed) {
  __try {
    RestoreRaw(j, executed);
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

// ---------------------------------------------------------------------------
// Texture table and reads maintenance (render thread, exclusive)
// ---------------------------------------------------------------------------
std::atomic<uint64_t> g_texBuilt{0}, g_texRefreshed{0}, g_texFull{0}, g_texDeferred{0};
uint64_t g_texWhy[kTwCount] = {};

// An entry the job rebuilt for itself during a pass (not the table's).
inline bool IsRedoEnt(const Job& j, const TexEntry* e) { return e >= j.redoEnt && e < j.redoEnt + kMaxRedoEnts; }

// Builds the job's misses and rebuilds its unusable and dirty entries.
void MaintainTex(TexTable& t, const gbbatch::TexEnv& env, const Job& j, uint32_t gen) {
  if (t.live + t.tomb >= t.size * 3 / 4) TexWipe(t);  // rare: everything is rebuilt on demand
  for (uint32_t k = 0; k < j.missCount; ++k) {
    const Miss& m = j.misses[k];
    bool fresh = false;
    TexEntry* e = TexInsert(t, m.tex, m.aux, m.type, &fresh);
    if (!e) {
      g_texFull++;
      break;
    }
    if (!fresh && !e->dirty.load(std::memory_order_relaxed)) continue;
    const uint8_t why = TexBuildGuarded(env, *e, &g_census);
    g_texWhy[why < kTwCount ? why : kTwFault]++;
    e->dirty.store(why == kTwOk ? 0 : 1, std::memory_order_relaxed);
    e->lastUse.store(gen, std::memory_order_relaxed);
    g_texBuilt++;
  }
  auto rebuild = [&](const TexEntry* ce) {
    TexEntry& e = *const_cast<TexEntry*>(ce);
    if (e.slot.load(std::memory_order_relaxed) != kTeLive) return;
    const uint8_t why = TexBuildGuarded(env, e, &g_census);
    g_texWhy[why < kTwCount ? why : kTwFault]++;
    e.dirty.store(why == kTwOk ? 0 : 1, std::memory_order_relaxed);
    g_texRefreshed++;
  };
  for (uint32_t k = 0; k < j.refreshCount; ++k) rebuild(j.refresh[k]);
  for (uint32_t k = 0; k < j.entCount; ++k)
    if (!IsRedoEnt(j, j.segEnts[k]) && j.segEnts[k]->dirty.load(std::memory_order_relaxed)) rebuild(j.segEnts[k]);
}

// The texture records pass 0 of (shader, tech) reads, from shadow_inst's (a)
// RDEF names: every name must be def_uniforms, a context buffer, sbPositions,
// a sampler or one of the shader's records. Returns false while pending.
bool ShaderPrintGuarded(const uint8_t* shader, void** effect, void** techBegin) {
  __try {
    *effect = *reinterpret_cast<void* const*>(shader + 0x50);
    *techBegin = *reinterpret_cast<void* const*>(shader + 0xb0);
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

bool ResolveReads(Tables& t, uint8_t* shader, uint64_t tech, uint32_t now = 0) {
  void* effect = nullptr;
  void* techBegin = nullptr;
  if (!ShaderPrintGuarded(shader, &effect, &techBegin)) return true;
  if (FindReads(t, shader, tech, effect, techBegin)) return true;
  std::vector<std::string> bound;
  std::string why;
  const int st = shadowinst::GbPassReadTextures(shader, tech, 0, &bound, nullptr, &why, nullptr);
  if (st == shadowtex::kReadsPending) return false;
  ReadsEntry* e = NewReads(t, shader, tech);
  if (!e) return true;
  e->tech = tech;
  e->effect = effect;
  e->techBegin = techBegin;
  e->lastUse.store(now, std::memory_order_relaxed);
  e->state = 1;
  e->count = 0;
  memset(e->mask, 0, sizeof(e->mask));
  if (st != shadowtex::kReadsReady) {
    e->state = -1;
    e->why = "the pass's read names are not analysed";
  } else {
    for (const std::string& n : bound) {
      if (gbreccount::NameCatOf(n.c_str()) != gbreccount::kNcOther) continue;
      const int r = gbreccount::RecordOfGuarded(shader, n.c_str());
      if (r < 0 || r >= static_cast<int>(kMaxRecords)) {
        e->state = -1;
        e->why = "the pass binds a resource that is no material texture, context buffer, sbPositions or sampler";
        break;
      }
      if (!(e->mask[r >> 6] & (1ull << (r & 63)))) ++e->count;
      e->mask[r >> 6] |= 1ull << (r & 63);
    }
    if (e->state > 0 && e->count > static_cast<uint32_t>(kMaxPsTex)) {
      e->state = -1;
      e->why = "the pass reads more textures than a key maps";
    }
  }
  PublishReads(t, *e, shader);
  return true;
}

// ---------------------------------------------------------------------------
// Probes: DCS's own draw, read back
// ---------------------------------------------------------------------------
struct Extra {
  ID3D11Buffer* vsCtx[3] = {};             // VS b6-b8 (references)
  ID3D11SamplerState* vsSamp[kSampSlots] = {};
};

void CaptureExtra(ID3D11DeviceContext* c, Extra& x) {
  c->VSGetConstantBuffers(kCtx0, 3, x.vsCtx);
  c->VSGetSamplers(0, kSampSlots, x.vsSamp);
}

void ReleaseExtra(Extra& x) {
  for (auto*& p : x.vsCtx) SafeRel(p);
  for (auto*& p : x.vsSamp) SafeRel(p);
}

// What DCS's draw must have used, from DCS memory and the pass.
struct Facts {
  void* dcsVs = nullptr;                 // the FX pass VS, equal to shadow_inst's map entry's
  ID3D11ShaderResourceView* pageSrv = nullptr;
  shrec::MeshLive mesh;
  const char* meshWhy = nullptr;
  ID3D11Buffer* passCtx[2][3] = {};      // VS/PS b6-b8 at the front entry (identity)
  const ReadsEntry* reads = nullptr;
  // The read records and the views their FX variables hold after the draw.
  int readCount = 0;
  bool readOk = true;
  int32_t readH[kMaxPsTex] = {};
  void* readView[kMaxPsTex] = {};
  // The slots FX Apply sets (shrec::FxStageDeps; not read: every slot counts).
  bool psDepsOk = false, vsDepsOk = false;
  uint32_t psSampDeps = 0xffff, psCbDeps = 0x3fff, vsSampDeps = 0xffff;
};

struct PsMap {
  int count = 0;
  int32_t h[kMaxPsTex] = {};
  UINT slot[kMaxPsTex] = {};
  uint32_t cbMask = 0, ctxMask = 0;
  uint64_t sbMask[2] = {};
  uint32_t sampMask = 0xffff, vsSampMask = 0xffff;  // the sampler slots that count
};

const char* const kInconclusive = "inconclusive";

// nullptr, kInconclusive (publish nothing yet), or why the key's draws are not
// recordable.
const char* CheckKey(const shrec::Capture& c, const Extra& x, const VsStatic& st, const Facts& f, PsMap* ps) {
  *ps = PsMap();
  if (c.classInstances) return "class instances are bound";
  if (!c.vs || c.vs != f.dcsVs) return "the bound VS is not the FX pass's (or not shadow inst's)";
  if (c.gs || c.hs || c.ds) return "a GS, HS or DS is bound";
  if (!c.cb) return "no def_uniforms buffer is bound";
  if (!c.sb || c.sb != f.pageSrv) return "sbPositions is not the page buffer's view at +0x30";
  for (UINT k = 0; k < 3; ++k)
    if ((st.ctxMask >> (kCtx0 + k)) & 1)
      if (!x.vsCtx[k] || x.vsCtx[k] != f.passCtx[0][k]) return "a VS context buffer is not the pass's";
  if (!c.ps) return "no pixel shader is bound";
  if (!f.reads || f.reads->state <= 0) return "the pass's texture reads are not known";
  for (UINT k = 0; k < 3; ++k) {
    ID3D11Buffer* b = c.psCb[kCtx0 + k];
    if (!b) continue;
    if (b != f.passCtx[1][k]) return "a PS context buffer is not the pass's";
    ps->ctxMask |= 1u << (kCtx0 + k);
  }
  if (!f.readOk) return kInconclusive;  // a read record unset or without a view: another draw maps it
  if (f.readCount != static_cast<int>(f.reads->count)) return kInconclusive;
  uint64_t texSlots[2] = {};
  for (int k = 0; k < f.readCount; ++k) {
    if (!f.readView[k]) return kInconclusive;
    int hits = 0;
    UINT slot = 0;
    for (UINT s = 0; s < kSrvSlots; ++s)
      if (c.psSrv[s] == f.readView[k]) ++hits, slot = s;
    if (hits == 0) return "a read texture's view is not bound at any PS slot";
    if (hits > 1) return kInconclusive;  // the same view also at another slot (an earlier draw's)
    for (int q = 0; q < k; ++q)
      if (ps->slot[q] == slot) return kInconclusive;  // two records with the same view
    ps->h[k] = f.readH[k];
    ps->slot[k] = slot;
    texSlots[slot >> 6] |= 1ull << (slot & 63);
  }
  ps->count = f.readCount;
  ps->sampMask = f.psDepsOk ? f.psSampDeps : 0xffff;
  ps->vsSampMask = f.vsDepsOk ? f.vsSampDeps : 0xffff;
  for (UINT s = 0; s < kCbSlots; ++s)
    if (c.psCb[s] && c.psCb[s] == c.cb && (s < kCtx0 || s >= kCtx0 + 3) && (!f.psDepsOk || ((f.psCbDeps >> s) & 1)))
      ps->cbMask |= 1u << s;
  for (UINT s = 0; s < kSrvSlots; ++s)
    if (c.psSrv[s] && c.psSrv[s] == f.pageSrv && !((texSlots[s >> 6] >> (s & 63)) & 1))
      ps->sbMask[s >> 6] |= 1ull << (s & 63);
  return nullptr;
}

// What differs between a key's objects and another probe of it (shadow_rec.h
// kD* bits; 0: same). Samplers count only at the slots FX Apply sets (both
// probes' dependencies; every slot when unknown): other slots hold whatever
// earlier draws left there, which depends on the draw history (the view).
// The context-buffer pattern does not count: lists bind the front entry's.
uint32_t KeyDiff(const GbKey& k, const shrec::Capture& c, const Extra& x, const PsMap& ps, uint32_t* sampSlots) {
  uint32_t d = 0;
  d |= k.vs != c.vs ? shrec::kDVs : 0;
  d |= k.ps != c.ps ? shrec::kDPs : 0;
  d |= k.dss != c.dss ? shrec::kDDepth : 0;
  d |= k.stencilRef != c.stencilRef ? shrec::kDStencilRef : 0;
  d |= k.bs != c.bs ? shrec::kDBlend : 0;
  d |= memcmp(k.blendFactor, c.blendFactor, sizeof(c.blendFactor)) != 0 ? shrec::kDBlendFactor : 0;
  d |= k.sampleMask != c.sampleMask ? shrec::kDSampleMask : 0;
  d |= k.rs != c.rs ? shrec::kDRaster : 0;
  *sampSlots = 0;
  // Pool slots count per execution (each probe checks them against the pool), not per key.
  const uint32_t pm = KeyOwnSamplers(k.psSampDeps & ps.sampMask), vm = KeyOwnSamplers(k.vsSampDeps & ps.vsSampMask);
  uint32_t vsSlots = 0;
  for (UINT q = 0; q < kSampSlots; ++q) {
    if (((pm >> q) & 1) && k.psSamp[q] != c.psSamp[q]) *sampSlots |= 1u << q;
    if (((vm >> q) & 1) && k.vsSamp[q] != x.vsSamp[q]) vsSlots |= 1u << q;
  }
  d |= *sampSlots ? shrec::kDSamplers : 0;
  d |= vsSlots ? shrec::kDVsSamplers : 0;
  *sampSlots |= vsSlots << 16;
  d |= k.psCbMask != ps.cbMask ? shrec::kDCbSlots : 0;
  bool tex = k.psTexCount != ps.count;
  for (int p = 0; p < ps.count && !tex; ++p) {
    int q = 0;
    while (q < k.psTexCount && k.psTexH[q] != ps.h[p]) ++q;
    tex = q == k.psTexCount || k.psTexSlot[q] != ps.slot[p];
  }
  d |= tex ? shrec::kDTexSlots : 0;
  return d;
}

struct ProbeIn {
  uint32_t scope = 0;  // the execution's collection index
  int ordinal = -1;    // its ordinal (log only)
  uint8_t* r = nullptr;
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
    g_tableFull{0}, g_probeInconclusive{0}, g_keyRelearnt{0}, g_probePool{0};
std::atomic<uint64_t> g_meshRelearnt{0};  // meshes relearnt because another object took their address
std::atomic<uint32_t> g_diffLogBudget{32};
// Why a wanted item was not probed (10 s line; a stall shows as wants with 0 probes).
enum ProbeSkip : int { kSkNotModel = 0, kSkRejected, kSkCooling, kSkKnown, kSkNoReads, kSkStatic, kSkRenderer,
                       kSkBudget, kSkips };
const char* const kSkipName[kSkips] = {"not a pass-1 opaque model draw", "key not recordable", "key cooling down",
                                       "key and mesh known under the pass's scope", "reads not resolved yet",
                                       "published without a probe (static)", "renderer vtable not DCS's",
                                       "pass probe budget used"};
std::atomic<uint64_t> g_probeSkip[kSkips] = {};
std::atomic<uint64_t> g_probeWants{0};  // probe requests of the jobs taken by passes

void LogReject(const char* what, const ProbeIn& in, uint32_t flags, const char* why) {
  if (g_logBudget.load() == 0) return;
  g_logBudget.fetch_sub(1);
  Log("gbuffer recorder: %s not recordable: %s (shader %p, technique %llu, flags 0x%x, mesh %p)", what, why, in.shader,
      static_cast<unsigned long long>(in.tech), flags, in.mesh);
}

// Publishes what one probe learned (render thread, or the offline tests).
// Returns false when inconclusive.
bool ProcessProbe(Tables& t, const ProbeIn& in, const VsStatic& st, const Facts& f, const shrec::Capture& c,
                  const Extra& x, uint32_t now = 0) {
  GbKey* k = const_cast<GbKey*>(FindKey(t, in.shader, in.tech, c.flags, in.effect, in.techBegin, in.scope));
  if (!k && KeyCooling(t, in.shader, in.tech, c.flags, in.effect, in.techBegin, in.scope, now, false)) return true;
  PsMap ps;
  const char* why = st.why ? st.why : CheckKey(c, x, st, f, &ps);
  const bool inconclusive = why == kInconclusive;
  if (inconclusive) {
    g_probeInconclusive++;
    if (!k || k->state.load() <= 0) return false;
    why = nullptr;  // a recordable key: the mesh still applies
  }
  if (!k) {
    const bool relearn = KeyCooling(t, in.shader, in.tech, c.flags, in.effect, in.techBegin, in.scope, now, true);
    k = NewKey(t, in.shader, in.tech, c.flags, in.scope);
    if (!k) {
      g_tableFull++;
      return true;
    }
    if (relearn) g_keyRelearnt++;
    k->lastUse.store(now, std::memory_order_relaxed);
    k->tech = in.tech;
    k->flags = c.flags;
    k->scope = in.scope;
    k->effect = in.effect;
    k->techBegin = in.techBegin;
    k->st = st;
    k->why = why;
    if (!why) {
      if ((k->vs = c.vs)) k->vs->AddRef();
      if ((k->ps = c.ps)) k->ps->AddRef();
      if ((k->dss = c.dss)) k->dss->AddRef();
      k->stencilRef = c.stencilRef;
      if ((k->bs = c.bs)) k->bs->AddRef();
      memcpy(k->blendFactor, c.blendFactor, sizeof(c.blendFactor));
      k->sampleMask = c.sampleMask;
      if ((k->rs = c.rs)) k->rs->AddRef();
      for (UINT s = 0; s < kSampSlots; ++s) {
        if ((k->psSamp[s] = c.psSamp[s])) k->psSamp[s]->AddRef();
        if ((k->vsSamp[s] = x.vsSamp[s])) k->vsSamp[s]->AddRef();
      }
      k->psSampDeps = ps.sampMask;
      k->vsSampDeps = ps.vsSampMask;
      k->psCbMask = ps.cbMask;
      k->psCtxMask = ps.ctxMask;
      k->psSbMask[0] = ps.sbMask[0];
      k->psSbMask[1] = ps.sbMask[1];
      k->psTexCount = ps.count;
      for (int p = 0; p < ps.count; ++p) k->psTexH[p] = ps.h[p], k->psTexSlot[p] = ps.slot[p];
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
    uint32_t slots = 0;
    const uint32_t diff = why ? 0 : KeyDiff(*k, c, x, ps, &slots);
    if (why || diff) {
      // Another draw of a recordable key bound something else: retired (its
      // draws are drawn by DCS), relearnt by a probe after the cooldown.
      k->why = why ? why : "state objects differ between draws of the key";
      k->retiredAt = now + 1;
      k->state.store(kRetired, std::memory_order_release);
      g_keyDowngrades++;
      if (g_diffLogBudget.load() > 0) {
        g_diffLogBudget.fetch_sub(1);
        Log("gbuffer recorder: key retired (relearnt after %u render entries): %s%s (shader %p, technique %llu, flags "
            "0x%x, collection %u, execution #%d; stencil ref %u -> %u; sampler slots that count PS 0x%x / 0x%x, VS 0x%x "
            "/ 0x%x; differing sampler slots PS 0x%x VS 0x%x; FX dependencies read PS %d VS %d)",
            kKeyCooldown, why ? why : "differs: ", why ? "" : shrec::DiffText(diff, slots & 0xffff).c_str(), in.shader,
            static_cast<unsigned long long>(in.tech), c.flags, in.scope, in.ordinal, k->stencilRef, c.stencilRef,
            k->psSampDeps, ps.sampMask, k->vsSampDeps, ps.vsSampMask, slots & 0xffff, slots >> 16, f.psDepsOk ? 1 : 0,
            f.vsDepsOk ? 1 : 0);
      }
      return true;
    }
  }
  if (k->state.load() <= 0) return true;
  shrec::ProbeFacts mf;
  mf.mesh = f.mesh;
  mf.meshWhy = f.meshWhy;
  const char* mwhy = shrec::CheckMesh(c, mf);
  // A new mesh at a reused address supersedes the old entry (shadow_rec.h's rules).
  const int pm = shrec::PublishProbedMesh(t.mesh, in.mesh, in.shader, in.tech, in.effect, in.techBegin, f.mesh,
                                          f.meshWhy, mwhy, c.il, now);
  if (pm == shrec::kPmKnown || pm == shrec::kPmCooling) return true;
  if (pm == shrec::kPmFull) {
    g_tableFull++;
    return true;
  }
  if (pm == shrec::kPmRelearnt) g_meshRelearnt++;
  if (mwhy) {
    g_meshesRejected++;
    LogReject("mesh", in, c.flags, mwhy);
  } else {
    g_meshesOk++;
  }
  return true;
}

// The read records' views as their FX variables hold them after the draw
// (slot 26's SetResource: *[var+8] = view [V dx11backend 0x61ca0]). Plain.
void ReadSetsRaw(const ProbeIn& in, const ReadsEntry& rd, Facts& f) {
  f.readCount = 0;
  f.readOk = true;
  uint64_t seen[kRecWords] = {};
  const uint32_t ntex = *reinterpret_cast<uint32_t*>(in.mat + 0x2d8);
  const uint8_t* en = ntex ? EntriesOf(in.mat, in.item) : nullptr;
  for (uint32_t k = 0; k < ntex && k < kMaxTex && en; ++k) {
    const int64_t h = *reinterpret_cast<int64_t*>(in.mat + 0x240 + 8 * k);
    if (h < 0 || h >= static_cast<int64_t>(kMaxRecords) || !((rd.mask[h >> 6] >> (h & 63)) & 1)) continue;
    if (!*reinterpret_cast<void* const*>(en + k * 0x18 + 8)) {
      f.readOk = false;  // inherits: not this draw's to map
      return;
    }
    if ((seen[h >> 6] >> (h & 63)) & 1) continue;
    seen[h >> 6] |= 1ull << (h & 63);
    if (f.readCount == kMaxPsTex) {
      f.readOk = false;
      return;
    }
    const uint8_t* var = VarOf(in.shader, h);
    f.readH[f.readCount] = static_cast<int32_t>(h);
    f.readView[f.readCount++] = var ? **reinterpret_cast<void** const*>(var + 8) : nullptr;
  }
}

bool ReadSetsGuarded(const ProbeIn& in, const ReadsEntry& rd, Facts& f) {
  __try {
    ReadSetsRaw(in, rd, f);
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    f.readOk = false;
    return false;
  }
}

// Static facts of (shader, technique): reflection of shadow_inst's (b)
// G-buffer VS of pass 0 (render thread, once per key).
const char* StaticFor(uint8_t* shader, uint64_t tech, VsStatic& st) {
  st = VsStatic();
  const shadowinst::MapEntry* e = shadowinst::FindEntry(shader, tech, 0);
  if (!e || !e->result || e->kind != shadowinst::kKindGb) return st.why = "shadow inst has no G-buffer compile of the key";
  char name[64];
  if (!shrec::TechNameGuarded(shader, tech, name, sizeof(name))) return st.why = "technique name not readable";
  std::vector<uint8_t> code;
  std::string cbName;
  {
    std::lock_guard<std::mutex> lock(shadowinst::g_mutex);
    const shadowinst::KeyResult& r = *e->result;
    for (const shadowinst::TechVs& tv : r.techs) {
      if (tv.name != name || tv.passVs.empty()) continue;
      const int idx = tv.passVs[0];
      if (idx < 0 || static_cast<size_t>(idx) >= r.vs.size()) break;
      code = r.vs[idx].bytecode;
      cbName = r.vs[idx].diff.cbName;
      break;
    }
  }
  if (code.empty()) return st.why = "shadow inst has no VS of pass 0";
  if (cbName.empty()) cbName = "def_uniforms";
  return AnalyseGbVs(shadowinst::g_compiler.reflect, code.data(), code.size(), cbName.c_str(), st);
}

// ---------------------------------------------------------------------------
// Runtime state
// ---------------------------------------------------------------------------
std::atomic<bool> g_on{false};      // [Model] GBufferRecorder, the kill switch, bench mode 29
std::atomic<bool> g_verify{false};  // [Suite] GBufferRecVerify
std::atomic<uint32_t> g_scope{kDefaultScope};
std::atomic<uint32_t> g_waitUs{300};
std::atomic<uint32_t> g_island{kDefaultIsland};
std::atomic<uint32_t> g_stride{0};  // [Suite] GBufferRecVerifyStride (verify only)
std::atomic<uint32_t> g_maxSeg{kDefaultMaxSeg};  // [Model] GBufferRecorderMaxSegments
std::atomic<bool> g_redoOn{true};      // [Model] GBufferRecorderRedo: re-record segments whose textures changed
std::atomic<bool> g_swapAhead{true};   // [Model] GBufferRecorderSwapAhead: do a due mip-set swap at the entry
std::atomic<uint32_t> g_helpers{kThreads - 1};   // [Model] GBufferRecorderHelpers (0-2 per job)
std::atomic<bool> g_cockpit{false};    // [Model] GBufferRecorderCockpit (R24 4; read at install for the pool size)
std::atomic<bool> g_byOrdinal{false};  // [Model] GBufferRecorderByOrdinal: slots by plain ordinal (before R24)
bool g_idMode = false;                 // executions bound by identity (install: not by ordinal and the build matches)
int g_poolSlots = kScopeSlots;         // slots with workers: 4, 8 with the cockpit (install)
// Workers per slot (the primary and its helpers): 3; 2 with the cockpit slots, so that g_poolSlots * g_threads
// fits defrec::Pool::kMaxWorkers (16; a larger request is clamped by the pool and worker indexes past it are
// outside its array: the R24 F1 crash, PumpRedo reading and releasing worker 16 + s).
int g_threads = kThreads;
Binder g_bind;                         // render thread
std::atomic<uint64_t> g_bindChanges{0}, g_idFail{0};
std::atomic<int> g_state{0};        // 0 = not tried (retried), 1 = ready, -1 = unavailable
std::atomic<bool> g_disabled{false};
// Passes that drew at least one segment from a command list (monotonic;
// the status word reads it to tell a working recorder from an idle one).
std::atomic<uint64_t> g_workPasses{0};
std::atomic<bool> g_shutdown{false};
std::atomic<int> g_inside{0};
std::atomic<bool> g_chained{false};
gbpass::WrapFn g_prevWrap = nullptr;
gbcount::OverrideFn g_prevOverride = nullptr;
shadowbatch::RenderObserverFn g_prevObserver = nullptr;
bool g_ownRenderHook = false, g_ownSort = false;
DWORD g_renderTid = 0;
Env g_env;
Tables g_tab;
// Long sessions (render thread): the tables a compaction replaced, freed once
// no job snapshot uses them; when the next compaction is due; totals.
Tables g_tabOld;
shrec::CompactPlan g_plan;
shrec::CompactStats g_lastCompact;
std::atomic<uint64_t> g_compactions{0}, g_oldFreed{0};
TexTable g_tex;
SRWLOCK g_texLock = SRWLOCK_INIT;
shrec::TexIdle g_texIdle;  // R77: the table released at a mission event / while the recorder is inactive
inline void NoteMissionEvent() { g_texIdle.eventGen.fetch_add(1, std::memory_order_acq_rel); }
defrec::Pool g_pool;  // worker s + g_poolSlots * k: slot s's primary (k 0) and helpers (k 1, 2)
inline int Wk(int s, int k) { return s + g_poolSlots * k; }
// Worker k of slot s exists (a slot past the pool's slots has none).
inline bool WkOk(int s, int k) {
  return s >= 0 && s < g_poolSlots && k >= 0 && k < g_threads && Wk(s, k) < g_pool.Workers();
}
inline bool SlotBusy(int s) {
  for (int k = 0; k < g_threads; ++k)
    if (WkOk(s, k) && g_pool.Busy(Wk(s, k))) return true;
  return false;
}
// Drops whatever the slot's workers left (they finish their own lists; the pool's are empty).
inline void DropPoolLists(int s) {
  for (int k = 0; k < g_threads; ++k)
    if (WkOk(s, k))
      if (ID3D11CommandList* l = g_pool.TakeList(Wk(s, k))) l->Release();
}
ID3D11DeviceContext* g_ctx = nullptr;
ID3D11Device* g_dev = nullptr;
uint32_t g_entryGen = 0;
uint32_t g_activeEntryGen = 0;  // g_entryGen at the last render entry of the followed graph seen while Active()
// A pass may arm its slot's next job only in a frame whose render entry the active recorder saw (WrapSlot).
inline bool PassMayArm(bool armed) { return !armed && g_activeEntryGen == g_entryGen; }
int64_t g_entryQpc = 0;
int g_ordinal = 0;       // G-buffer executions since the last render entry (render thread)
void* g_gbRg = nullptr;  // the render graph of the G-buffer executions
bool g_collOk = false;   // the collection descriptors as analysed (install)
QuadSet g_frameQuad;     // the quad views' volumes for this frame (render thread, at its first execution)
uint32_t g_frameQuadGen = 0, g_frameExecs = 0, g_frameCounted = 0, g_quadMissFrames = 0;
bool g_quadFilterOn = true;
std::atomic<uint64_t> g_nonQuad{0};  // executions of other views (not counted, stock)
int64_t g_gbRgEntryQpc = 0;  // its last render entry
std::atomic<uint64_t> g_graphChanges{0};
int64_t g_lastPassQpc = 0, g_gbEntryQpc = 0;
std::atomic<uint64_t> g_faults{0}, g_probes{0}, g_probeOdd{0}, g_entries{0}, g_gbExecs{0}, g_ordMax{0};
constexpr uint32_t kEvictAge = 1500;  // render entries an unused texture entry is kept

void Disable(const char* why) {
  if (!g_disabled.exchange(true)) Log("gbuffer recorder: disabled for this session (%s)", why);
}

// ---- Fault containment (R24 crash follow-up) ----
// Every entry point DCS calls (the pass wrap, the loop entries, the item
// override, the render and sort observers) runs our code under SEH: a fault
// in our code latches the recorder off and DCS's work is done stock. Calls
// into DCS from our code raise t_dcs; a fault while it is above the entry's
// level is DCS's own and is left to DCS's handlers (as without us).
thread_local int t_dcs = 0;
thread_local bool t_passRan = false;    // the wrap: DCS's pass function was called
thread_local bool t_itemDrawn = false;  // the override: DCS's draw of the item was called
thread_local bool t_segExecuted = false;  // an exec entry: its lists were executed
std::atomic<uint64_t> g_contained{0};
int GbFilter(unsigned long code, int lvl) {
  if (t_dcs != lvl) return EXCEPTION_CONTINUE_SEARCH;
  switch (code) {
    case EXCEPTION_ACCESS_VIOLATION:
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
    case EXCEPTION_DATATYPE_MISALIGNMENT:
    case EXCEPTION_ILLEGAL_INSTRUCTION:
    case EXCEPTION_IN_PAGE_ERROR:
    case EXCEPTION_INT_DIVIDE_BY_ZERO:
    case EXCEPTION_PRIV_INSTRUCTION:
    // A C++ exception thrown by our own code at this level (std::bad_alloc
    // from a table, string or vector on the render thread) is contained the
    // same way; before, it unwound into DCS's frames and ended the process.
    // DCS's own throws happen above `lvl` and are still left to DCS.
    case 0xE06D7363:  // MSVC C++ exception
      return EXCEPTION_EXECUTE_HANDLER;
    default:
      return EXCEPTION_CONTINUE_SEARCH;
  }
}
void Contained(const char* where, unsigned long code, int lvl) {
  t_dcs = lvl;
  g_contained++;
  char why[96];
  snprintf(why, sizeof(why), "exception 0x%08lx in %s, contained", code, where);
  Disable(why);
}

bool Active() {
  return g_on.load(std::memory_order_relaxed) && !g_disabled.load(std::memory_order_relaxed) &&
         !g_shutdown.load(std::memory_order_relaxed) && g_state.load(std::memory_order_relaxed) == 1 &&
         !gbbatch::g_on.load(std::memory_order_relaxed) && !gbreccount::g_chained.load(std::memory_order_relaxed);
}

// The front and exec entries handed to DCS's loop: objects whose vtable's
// slot 1 runs our code. Only vt[1] is ever called on loop entries [V GC
// 0x89700-0x89713]; the other slots point at a no-op.
struct LoopObj {
  void** vtbl;
  int slot;
  int seg;
};
uint64_t __fastcall FrontVt1(void* self, void* ctx);
uint64_t __fastcall ExecVt1(void* self, void* ctx);
uint64_t __fastcall LoopNop(void*, void*) { return 0; }
void* g_frontVtbl[8];
void* g_execVtbl[8];

struct Learned {
  bool target = false;
  void* rg = nullptr;
  uint32_t idx = 0;
  bool haveVec = false;
  ID3D11RenderTargetView* rtv[kRtv] = {};  // references
  ID3D11DepthStencilView* dsv = nullptr;
  UINT nvp = 0, nsc = 0;
  D3D11_VIEWPORT vp[kVp] = {};
  D3D11_RECT sc[kVp] = {};
  uint32_t flags = 0;
  ID3D11SamplerState* pool[2][kSampSlots] = {};  // VS, PS s5-s15 (references)
  UINT ctxBytes[2][3] = {};  // DCS's b6-b8 sizes (0: none bound)
  // The 12 bytes DCS writes at mat+0x110 in this execution and their inputs.
  bool ratioOk = false;
  void* ratioFb = nullptr;
  int32_t ratioVp[2] = {};
  uint8_t ratio[kRatioLen] = {};
};

struct Stat {
  std::atomic<uint64_t> passes{0}, passReason[kPassReasons] = {}, partial{0}, segs{0}, segReason[kSegReasons] = {};
  std::atomic<uint64_t> items{0}, itemReason[kReasons] = {}, recorded{0}, draws{0}, jobs{0}, jobsUsed{0}, refused{0};
  std::atomic<uint64_t> cbBytes{0}, vt23{0}, segPlanned{0};
  std::atomic<uint64_t> buildNs{0}, recordNs{0}, waitNs{0}, waitMaxNs{0}, frontNs{0}, execNs{0}, executeNs{0},
      restoreNs{0}, maintNs{0};
  std::atomic<uint64_t> tlSamples{0}, early{0}, recWorkers{0}, pieces{0}, helpersQueued{0}, helperItems{0},
      jobItems{0}, preNs{0}, commitNs{0}, recMaxNs{0};
  std::atomic<int64_t> tlPass{0}, tlStart{0}, tlDone{0}, tlPre{0}, tlBuilt{0}, tlRec{0};
  // Late jobs (pass drawn stock, the job finished later): same timeline, read when the slot is re-armed.
  std::atomic<uint64_t> lateSamples{0};
  std::atomic<int64_t> latePass{0}, lateStart{0}, lateBuilt{0}, lateDone{0};
  std::atomic<uint64_t> staleStart{0}, misses{0}, refreshes{0};
  std::atomic<uint64_t> aaClean{0};  // consecutive equal stock vs stock compares
  // Re-recorded segments: planned, executed, not ready at their exec entry, failed; swaps done ahead; plan time.
  std::atomic<uint64_t> redoPlanned{0}, redoExecuted{0}, redoLate{0}, redoFail{0}, swapsAhead{0}, redoNs{0};
  std::atomic<uint64_t> slackJobs{0};  // jobs armed with the slack settings (smaller island, more segments)
  std::atomic<uint64_t> rtReplays{0};  // render-target textures' vt[18] replayed at exec entries
  // Cockpit jobs: render-target textures at exec entries: seen, drawn to since their last mip generation, bound
  // as a target of the pass (R24 3.4).
  std::atomic<uint64_t> rtSeen{0}, rtDrawn{0}, rtBound{0};
  // "Item vector changed": the job read another collection's vector, or the same one changed after its read.
  std::atomic<uint64_t> idOtherVec{0}, idSameVec{0};
  std::atomic<uint64_t> verifyAa{0}, verifyAb{0}, verifyAaBad{0}, verifyAbBad{0}, verifyTexels{0}, verifyBadTexels{0},
      verifySkipped{0}, verifyErrors{0}, verifyStateBad{0}, verifyExecs{0};
};

struct Slot {
  Learned learn;
  CollKey id;  // the execution it is bound to (identity mode)
  Job* job = nullptr;
  bool armed = false;
  uint32_t passGen = 0;
  ID3D11Buffer* ours[2][3] = {};  // DEFAULT copies of DCS's b6-b8 (per stage)
  UINT oursBytes[2][3] = {};
  void** stockList = nullptr;     // [front, the vector's items] for passes without a job
  LoopObj front = {};
  LoopObj exec[kMaxSegments] = {};
  int64_t lateEntry = 0, latePassQpc = 0;  // the render entry and pass time of a late job (0: none)
  int64_t boundQpc = 0, lastPassQpc = 0;    // identity self-check: when bound, its last pass
  // Its execution's collection as last seen at the pass (model, volume, vrank): jobs start on this frame's
  // collection with them (R24 13). vol null: not known (raw index).
  const void* lvol = nullptr;
  uint16_t lsm = 0, lvrank = 0;
  double workUs = -1;  // the slot's jobs' worker time (stage A on every thread, commit, recording), smoothed
  double slackMs = -1;  // the slot's jobs in time: pass minus done, smoothed
  Stat st;
};
Slot g_slot[kSlots];

// Keys are learnt per execution: its identity, or its collection index.
// Keys are learnt per execution: its identity, else its view's collection
// (model, ClippingVolume, vrank) once seen, else the collection index. The
// index alone moves when other views add collections (F-4 scene): the jobs
// were armed with the previous frame's index and the probes ran under this
// frame's, so a key probed under one never served the other and probing
// stalled with thousands of items "key not probed yet" (R24 14).
inline uint32_t VolScope(uint16_t sm, const void* vol, uint16_t vrank) {
  const uint64_t v = reinterpret_cast<uintptr_t>(vol);
  return static_cast<uint32_t>(Mix(v * 0x9E3779B97F4A7C15ull ^ (static_cast<uint64_t>(sm) << 48) ^
                                   (static_cast<uint64_t>(vrank) << 32))) | 0x80000000u;
}
inline uint32_t KeyScopeFor(bool idMode, const CollKey& id, const void* lvol, uint16_t lsm, uint16_t lvrank,
                            uint32_t idx) {
  if (idMode) return CollHash(id);
  if (lvol) return VolScope(lsm, lvol, lvrank);
  return idx;
}
inline uint32_t KeyScopeOf(int s) {
  const Slot& sl = g_slot[s];
  return KeyScopeFor(g_idMode, sl.id, sl.lvol, sl.lsm, sl.lvrank, sl.learn.idx);
}
// The scope ordinal a slot serves (a cockpit slot: its view's slot's), and its log name.
inline int SlotOrdinal(int s) { return OrdinalOfSlot(g_scope.load(), s % kScopeSlots); }
inline void SlotName(int s, char* out, size_t n) {
  snprintf(out, n, "execution #%d%s", SlotOrdinal(s), s >= kScopeSlots ? " cockpit" : "");
}

// Helpers per job from the slot's measured work: a helper costs a wake-up and
// a share of the stage A chunks, worth it only for long jobs [I].
constexpr double kOneHelperUs = 1000.0, kTwoHelpersUs = 2500.0;
inline uint32_t HelpersFor(double workUs, uint32_t cap) {
  if (workUs < 0) return cap;  // not measured yet
  const uint32_t want = workUs < kOneHelperUs ? 0u : workUs < kTwoHelpersUs ? 1u : 2u;
  return want < cap ? want : cap;
}
inline double JobWorkUs(const Job& j) {
  double rec = 0;
  for (int r = 0; r < kThreads; ++r) rec += j.recUs[r];
  return j.preUs * (1 + j.helpers) + j.commitUs + rec;
}
inline void NoteWork(Slot& sl, double us) { sl.workUs = sl.workUs < 0 ? us : 0.5 * (sl.workUs + us); }
// A slot whose jobs finish this long before their pass records more of it:
// up to 16 segments and islands of 12 draws (one more exec entry costs about
// 16 us on the render thread; 12 stock draws more) [M live yaw profile].
constexpr double kSlackMs = 2.0;
constexpr uint32_t kSlackIsland = 12;
inline void NoteSlack(Slot& sl, double ms) { sl.slackMs = sl.slackMs < 0 ? ms : 0.5 * (sl.slackMs + ms); }
std::atomic<Job*> g_jobPub[kSlots];  // the slots' jobs for the sort observer (published once allocated)

// R73 (shadow_rec.h R61 for the cascades): the frame-heap generation when
// Scene's sort last finished one of the G-buffer graph's collections (any of
// them; 0: never). A render-entry start needs it current (EntryVectorFresh):
// the scene was parsed and sorted since the last heap reset.
//   Live R73 (qvbench_fg_sea_wind): 16-24 G-buffer collection sorts per frame
// in flight, 0 in the F10 map (the G-buffer passes still run there, 8 per
// frame, on vectors no sort rebuilt: the R61 structure). A per-collection
// record (R61's rule for the cascades) also refused non-empty collections
// that DCS refills without that sort in flight (F2 view changes: up to 40%
// of a 10-s block's passes), so the G-buffer rule is graph-wide.
std::atomic<uint64_t> g_graphSortGen{0};
std::atomic<void*> g_sortRg{nullptr};  // g_gbRg for the pool threads (LearnVector)
constexpr uint32_t kSortColls = 4096;  // collections of the graph's array the sort observer recognises
std::atomic<uint64_t> g_unsortedStarts{0};  // render-entry starts skipped: no sort since the last heap reset
std::atomic<uint64_t> g_entryStarts{0};     // render-entry starts made (the gate passed)
std::atomic<uint64_t> g_unsortedEmpty{0};   // of those, on an empty vector with no sort since the last reset
std::atomic<uint64_t> g_sortsNoted{0};      // Scene's sorts of the G-buffer graph's collections (NoteSorted)

// An empty collection vector (begin == end, or no storage): it names no renderable. A fault reading the header
// (the graph's own array) counts as not empty: the gate then skips the start.
bool VectorEmptyGuarded(void** vec) {
  __try {
    return vec[0] == vec[1];
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

// Notes a finished sort of `out` when it is one of the G-buffer graph's collections (pool thread).
void NoteSorted(void** out) {
  void* rg = g_sortRg.load(std::memory_order_acquire);
  uint8_t* base = rg ? shrec::ArrayBaseGuarded(rg) : nullptr;
  if (!base) return;
  const intptr_t d = reinterpret_cast<uint8_t*>(out) - base;
  if (d < 0 || d % 24 || d / 24 >= static_cast<intptr_t>(kSortColls)) return;
  g_graphSortGen.store(g_frameHeapGen.load(std::memory_order_acquire), std::memory_order_release);
  g_sortsNoted.fetch_add(1, std::memory_order_relaxed);
}

void OnSorted(void** out);
// The sort observer is ours and the sort call sites are patched: Scene's sorts are recorded.
inline bool SortRecorded() {
  return shrec::g_sortPatched && shrec::g_sortObserver2.load(std::memory_order_relaxed) == &OnSorted;
}

// R73: a render-entry start may read the collections' vectors: Scene's sort
// finished one of the G-buffer graph's collections since the last frame-heap
// reset. Always true without the sort hook (no record: the old behaviour).
// In normal frames the sorts run after the frame's resets and before render
// entry; when DCS renders the G-buffer graph without parsing the scene again
// (the F10 map), the vectors are an earlier frame's and may name recycled
// memory (the R55 / R60 shadow faults in F10).
inline bool EntryVectorFreshAt(bool recorded, uint64_t sortGen, uint64_t heapGen) {
  return !recorded || sortGen == heapGen;
}
bool EntryVectorFresh() {
  return EntryVectorFreshAt(SortRecorded(), g_graphSortGen.load(std::memory_order_acquire),
                            g_frameHeapGen.load(std::memory_order_acquire));
}

void NoteMax(std::atomic<uint64_t>& a, uint64_t v) {
  uint64_t cur = a.load();
  while (v > cur && !a.compare_exchange_weak(cur, v)) {
  }
}
uint64_t NsSince(int64_t t0) { return static_cast<uint64_t>((defrec::Qpc() - t0) * defrec::QpcToUs() * 1000.0); }
int64_t QpcNs(int64_t a, int64_t b) { return a && b ? static_cast<int64_t>((b - a) * defrec::QpcToUs() * 1000.0) : 0; }

// ---------------------------------------------------------------------------
// Render thread: the pass state at the front entry
// ---------------------------------------------------------------------------
struct Target {
  ID3D11RenderTargetView* rtv[kRtv];  // references
  ID3D11DepthStencilView* dsv;
  UINT nvp, nsc;
  D3D11_VIEWPORT vp[kVp];
  D3D11_RECT sc[kVp];
  ID3D11Buffer* ctx[2][3];  // references
  UINT ctxBytes[2][3];
  ID3D11SamplerState* pool[2][kSampSlots];  // VS, PS s5-s15 (references)
  uint32_t flags, dbg;
  bool rendererOk;
  void* fb;
  int32_t vpd[2];
  bool ratioIn;
};

bool ReadRendererRaw(Target& t) {
  auto* r = reinterpret_cast<uint8_t*>(shadowbatch::g_rendererObj);
  if (!r) return false;
  __try {
    t.flags = *reinterpret_cast<uint32_t*>(r + 0xd4);
    t.dbg = *reinterpret_cast<uint32_t*>(r + 0x2120);
    t.rendererOk = true;
    t.fb = gbbatch::FbNow();
    gbbatch::VpNow(t.vpd);
    t.ratioIn = true;
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

void ReadTarget(ID3D11DeviceContext* c, Target& t) {
  memset(&t, 0, sizeof(t));
  c->OMGetRenderTargets(kRtv, t.rtv, &t.dsv);
  t.nvp = kVp;
  c->RSGetViewports(&t.nvp, t.vp);
  t.nsc = kVp;
  c->RSGetScissorRects(&t.nsc, t.sc);
  c->VSGetConstantBuffers(kCtx0, 3, t.ctx[0]);
  c->PSGetConstantBuffers(kCtx0, 3, t.ctx[1]);
  c->VSGetSamplers(kPoolFirst, kSampSlots - kPoolFirst, &t.pool[0][kPoolFirst]);
  c->PSGetSamplers(kPoolFirst, kSampSlots - kPoolFirst, &t.pool[1][kPoolFirst]);
  for (int s = 0; s < 2; ++s)
    for (int k = 0; k < 3; ++k)
      if (t.ctx[s][k]) {
        D3D11_BUFFER_DESC d;
        t.ctx[s][k]->GetDesc(&d);
        t.ctxBytes[s][k] = d.ByteWidth;
      }
  ReadRendererRaw(t);
}

void ReleaseTarget(Target& t) {
  for (auto*& p : t.rtv) SafeRel(p);
  SafeRel(t.dsv);
  for (auto& row : t.ctx)
    for (auto*& p : row) SafeRel(p);
  for (auto& row : t.pool)
    for (auto*& p : row) SafeRel(p);
}

// The pass state the next jobs of slot s record against (render thread).
void LearnTarget(int s, const Target& t) {
  Slot& sl = g_slot[s];
  Learned& l = sl.learn;
  bool anyRt = false;
  for (auto* p : t.rtv) anyRt |= p != nullptr;
  if (!t.dsv || !anyRt || !t.rendererOk || (t.dbg & kDbgBits)) {
    l.target = false;
    return;
  }
  for (int st = 0; st < 2; ++st)
    for (int k = 0; k < 3; ++k) {
      const UINT bytes = t.ctxBytes[st][k];
      l.ctxBytes[st][k] = bytes;
      if (!bytes || sl.oursBytes[st][k] == bytes) continue;
      SafeRel(sl.ours[st][k]);
      sl.oursBytes[st][k] = 0;
      D3D11_BUFFER_DESC d = {};
      d.ByteWidth = bytes;
      d.Usage = D3D11_USAGE_DEFAULT;
      d.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
      if (!g_dev || FAILED(g_dev->CreateBuffer(&d, nullptr, &sl.ours[st][k])) || !sl.ours[st][k]) {
        l.target = false;
        return;
      }
      sl.oursBytes[st][k] = bytes;
    }
  for (int k = 0; k < static_cast<int>(kRtv); ++k)
    if (l.rtv[k] != t.rtv[k]) {
      SafeRel(l.rtv[k]);
      if ((l.rtv[k] = t.rtv[k])) l.rtv[k]->AddRef();
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
  for (int st = 0; st < 2; ++st)
    for (UINT q = kPoolFirst; q < kSampSlots; ++q)
      if (l.pool[st][q] != t.pool[st][q]) {
        SafeRel(l.pool[st][q]);
        if ((l.pool[st][q] = t.pool[st][q])) l.pool[st][q]->AddRef();
      }
  l.target = true;
}

int CheckFront(const Slot& sl, const Job& j, const Target& t) {
  if (t.dsv != j.dsv || memcmp(t.rtv, j.rtv, sizeof(t.rtv)) != 0 || t.nvp != j.nvp ||
      memcmp(t.vp, j.vp, t.nvp * sizeof(D3D11_VIEWPORT)) != 0 || t.nsc != j.nsc ||
      memcmp(t.sc, j.sc, t.nsc * sizeof(D3D11_RECT)) != 0)
    return kPTarget;
  if (!t.rendererOk || t.flags != j.flags || (t.dbg & kDbgBits)) return kPFlags;
  for (int s = 0; s < 2; ++s)
    for (int k = 0; k < 3; ++k) {
      const bool have = t.ctx[s][k] != nullptr;
      if (have != (j.ctxBuf[s][k] != nullptr)) return kPCtx;
      if (have && (t.ctxBytes[s][k] != sl.oursBytes[s][k] || j.ctxBuf[s][k] != sl.ours[s][k])) return kPCtx;
    }
  if (!t.ratioIn || t.fb != j.ratioFb || t.vpd[0] != j.ratioVp[0] || t.vpd[1] != j.ratioVp[1]) return kPRatio;
  for (int st = 0; st < 2; ++st)
    if (!PoolSame(reinterpret_cast<void* const*>(t.pool[st]), reinterpret_cast<void* const*>(j.pool[st])))
      return kPPool;
  return kPExecuted;
}

// ---------------------------------------------------------------------------
// Render thread: the pass
// ---------------------------------------------------------------------------
enum : int { kPsNone = 0, kPsArmed, kPsLive, kPsStock, kPsStockRun };
struct PassCtx {
  int slot = 0;
  Job* job = nullptr;
  ID3D11CommandList* cl[kMaxPieces] = {};  // by piece
  int state = kPsNone;
  int reason = kPNoJob;
  bool front = false;
  int probesLeft = 0;
  size_t cursor = 0;
  uint32_t flags = 0;
  ID3D11Buffer* frontCtx[2][3] = {};  // identity only
  void* pool[2][kSampSlots] = {};      // VS, PS s5-s15 at the front entry (identity only)
  void* fb = nullptr;
  int32_t vpd[2] = {};
  bool ratioSeen = false;
  uint8_t segDone[kMaxSegments] = {};
  uint8_t segWhy[kMaxSegments] = {};
  int verify = 0;  // 1: capture the targets at the front (run 1), 2: check the views (run 2)
  bool captured = false, sameViews = false, stateBad = false;
  const char* captureWhy = nullptr;
  uint64_t frontNs = 0, execNs = 0, executeNs = 0;
  uint32_t replays = 0;
};
PassCtx* g_pass = nullptr;
void PlanRedo(PassCtx& pc, uint32_t from, uint32_t to);

void FrontBody(void* self, void* ctx) {
  (void)ctx;
  PassCtx* pc = g_pass;
  if (!pc || GetCurrentThreadId() != g_renderTid || static_cast<LoopObj*>(self)->slot != pc->slot) return;
  const int64_t t0 = defrec::Qpc();
  pc->front = true;
  Slot& sl = g_slot[pc->slot];
  Target t;
  ReadTarget(g_ctx, t);
  pc->flags = t.flags;
  for (int s = 0; s < 2; ++s)
    for (int k = 0; k < 3; ++k) pc->frontCtx[s][k] = t.ctx[s][k];
  for (int s = 0; s < 2; ++s)
    for (UINT q = 0; q < kSampSlots; ++q) pc->pool[s][q] = t.pool[s][q];
  pc->fb = t.fb;
  pc->vpd[0] = t.vpd[0];
  pc->vpd[1] = t.vpd[1];
  if (pc->state == kPsArmed) {  // checked before learning (learning may replace our context buffers)
    const int why = CheckFront(sl, *pc->job, t);
    if (why) {
      pc->state = kPsStock;
      pc->reason = why;
    } else {
      pc->state = kPsLive;
    }
  }
  if (pc->state == kPsLive) PlanRedo(*pc, 0, pc->job->segCount);  // textures changed since the job read them
  LearnTarget(pc->slot, t);
  if (!t.rendererOk || (t.dbg & kDbgBits)) pc->probesLeft = 0;
  if (pc->verify == 1) {
    const char* why = gbverify::Capture();
    pc->captureWhy = why;
    pc->captured = !why;
    if (pc->captured) gbverify::CopyAll(true, false);  // init
  } else if (pc->verify == 2) {
    ID3D11RenderTargetView* rtv[8] = {};
    ID3D11DepthStencilView* dsv = nullptr;
    g_ctx->OMGetRenderTargets(8, rtv, &dsv);
    bool same = static_cast<ID3D11View*>(dsv) == gbverify::g_t[8].view;
    for (int i = 0; i < 8; ++i) same &= static_cast<ID3D11View*>(rtv[i]) == gbverify::g_t[i].view;
    for (auto*& p : rtv) SafeRel(p);
    SafeRel(dsv);
    pc->sameViews = same;
  }
  ReleaseTarget(t);
  pc->frontNs = NsSince(t0);
  return;
}

uint64_t __fastcall FrontVt1(void* self, void* ctx) {
  const int lvl = t_dcs;
  __try {
    FrontBody(self, ctx);
  } __except (GbFilter(GetExceptionCode(), lvl)) {
    Contained("the front entry", GetExceptionCode(), lvl);
    if (PassCtx* pc = g_pass) pc->state = kPsStock;  // the exec entries draw their segments stock
  }
  return 0;
}

// DCS draws segment k's items itself, in order (their own vt[1]).
void DrawSegmentStock(const Job& j, int k, void* ctx) {
  const Seg& s = j.segs[k];
  ++t_dcs;
  for (uint32_t c = s.candFirst; c < s.candEnd; ++c) {
    const Cand& cd = j.cands[c];
    if (!cd.alive) continue;
    void* r = j.snap[cd.item];
    (*reinterpret_cast<Vt1Fn**>(r))[1](r, ctx);
  }
  --t_dcs;
}

// What a residual between segments could have changed in the state the lists
// assume: the renderer's flags (rasterizer index, debug bits) and DCS's sampler
// pool (R19 4.3). 0: unchanged.
int CheckExecEntry(const Job& j) {
  uint32_t flags = 0, dbg = 0;
  if (!shrec::ReadRenderer(&flags, &dbg) || flags != j.flags || (dbg & kDbgBits)) return kSFlags;
  ID3D11SamplerState* now[2][kSampSlots] = {};
  g_ctx->VSGetSamplers(kPoolFirst, kSampSlots - kPoolFirst, &now[0][kPoolFirst]);
  g_ctx->PSGetSamplers(kPoolFirst, kSampSlots - kPoolFirst, &now[1][kPoolFirst]);
  int why = 0;
  for (int st = 0; st < 2; ++st) {
    if (!PoolSame(reinterpret_cast<void* const*>(now[st]), reinterpret_cast<void* const*>(j.pool[st]))) why = kSPool;
    for (auto*& p : now[st]) SafeRel(p);
  }
  return why;
}


// ---- Re-recording segments whose textures changed (render thread) ----
// A texture of a segment that changed after the job read it (another pass
// swapped its mip sets, or a swap is due now) used to make the whole segment
// stock. Instead, at the front entry and at each exec entry, the job builds
// its own entry from the live texture (doing a due swap first: getSRV, as the
// segment's first draw of the texture would; no getSRV of it runs in between
// [I]), re-picks the views of the segment's draws and has an idle worker of
// the slot re-record the segment. The exec checks then compare with the new
// entries, as before.

// The latest entry of the job rebuilt for `e` (nullptr: none).
const TexEntry* RedoOf(const Job& j, const TexEntry* e) {
  for (uint32_t i = j.redoEntCount; i-- > 0;)
    if (j.redoFrom[i] == e) return &j.redoEnt[i];
  return nullptr;
}

bool EntryStaleRaw(const gbbatch::TexEnv& env, const TexEntry& e, uint32_t ver) {
  return e.version.load(std::memory_order_relaxed) != ver || e.slot.load(std::memory_order_relaxed) != kTeLive ||
         !TexCheckRaw(env, e);
}

// The job's own entry for e's texture, built now (nullptr: not usable).
// Plain (callers hold SEH).
const TexEntry* RedoEntryRaw(Job& j, const TexEntry& e, bool swapAhead, Stat& st) {
  const gbbatch::TexEnv& env = j.env->tex;
  if (const TexEntry* r = RedoOf(j, &e))
    if (r->usable && !EntryStaleRaw(env, *r, r->version.load(std::memory_order_relaxed))) return r;
  if (j.redoEntCount == kMaxRedoEnts) return nullptr;
  if (!IsRedoEnt(j, &e)) {  // the table's entry: rebuilt after the pass (the segments now use the job's)
    const_cast<TexEntry&>(e).dirty.store(1, std::memory_order_relaxed);
    if (j.refreshCount < kMaxRefresh) j.refresh[j.refreshCount++] = &e;
  }
  TexEntry& p = j.redoEnt[j.redoEntCount];
  p.tex = e.tex;
  p.aux = e.aux;
  p.type = e.type;
  p.dirty.store(0, std::memory_order_relaxed);
  uint8_t why = TexBuildRaw(env, p, &g_census);
  if (why == kTwSwap && swapAhead && shadowtex::g_getSrv) {
    const uint64_t zero = 0;  // the size picks a view only; the swap does not depend on it
    shadowtex::g_getSrv(p.tex, reinterpret_cast<void*>(static_cast<intptr_t>(p.aux)), &zero);
    st.swapsAhead++;
    why = TexBuildRaw(env, p, &g_census);
  }
  if (why != kTwOk) {
    TexReleaseViews(p);
    p.usable = 0;
    return nullptr;
  }
  p.slot.store(kTeLive, std::memory_order_release);
  j.redoFrom[j.redoEntCount++] = &e;
  return &p;
}

// Segment k's textures still give the job's views; if not, its draws get the
// rebuilt entries' views and the segment is queued for re-recording. Nothing
// changes for the segment unless every stale entry could be rebuilt and every
// draw's view picked. Plain (callers hold SEH).
void PlanSegRaw(Job& j, uint32_t k, bool swapAhead, Stat& st) {
  const int state = j.redoState[k].load(std::memory_order_acquire);
  if (state == kRedoWant || state == kRedoRun) return;
  const gbbatch::TexEnv& env = j.env->tex;
  const Seg& sg = j.segs[k];
  bool stale = false;
  for (uint32_t q = sg.entFirst; q < sg.entEnd; ++q)
    if (EntryStaleRaw(env, *j.segEnts[q], j.segEntVer[q])) {
      stale = true;
      if (!RedoEntryRaw(j, *j.segEnts[q], swapAhead, st)) return;
    }
  if (!stale) return;
  // Every draw's views from the newest entries (checked before anything changes).
  for (int pass = 0; pass < 2; ++pass) {
    for (uint32_t c = sg.candFirst; c < sg.candEnd; ++c) {
      const Cand& cd = j.cands[c];
      if (!cd.alive) continue;
      SetRef* sets = j.tsets[cd.thread] + cd.setFirst;
      ID3D11ShaderResourceView** views = j.tviews[cd.thread] + cd.viewFirst;
      const GbKey& key = *cd.key;
      for (uint32_t m = 0; m < cd.setCount; ++m) {
        const TexEntry* e = sets[m].e;
        for (const TexEntry* r = RedoOf(j, e); r; r = RedoOf(j, e)) e = r;
        ID3D11ShaderResourceView* v = nullptr;
        if (!TexPick(*e, sets[m].size, &v)) return;
        if (!pass) continue;
        sets[m].e = e;
        int qv = 0;
        while (qv < key.psTexCount && key.psTexH[qv] != sets[m].h) ++qv;
        if (qv < key.psTexCount) views[qv] = v;  // the last set wins, as stock
      }
    }
  }
  for (uint32_t q = sg.entFirst; q < sg.entEnd; ++q) {
    const TexEntry* e = j.segEnts[q];
    for (const TexEntry* r = RedoOf(j, e); r; r = RedoOf(j, e)) e = r;
    j.segEnts[q] = e;
    j.segEntVer[q] = e->version.load(std::memory_order_relaxed);
  }
  if (j.redoCl[k]) {
    j.redoCl[k]->Release();
    j.redoCl[k] = nullptr;
  }
  j.redoState[k].store(kRedoWant, std::memory_order_release);
  st.redoPlanned++;
}

bool PlanSegsGuarded(Job& j, uint32_t from, uint32_t to, bool swapAhead, Stat& st) {
  __try {
    for (uint32_t k = from; k < to && k < j.segCount; ++k) PlanSegRaw(j, k, swapAhead, st);
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

// Queued segments onto the slot's idle workers, in segment order.
void PumpRedoRaw(int s, Job& j) {
  for (uint32_t k = 0; k < j.segCount; ++k) {
    if (j.redoState[k].load(std::memory_order_acquire) != kRedoWant) continue;
    int wk = -1;
    for (int t = 0; t < g_threads && wk < 0; ++t)
      if (WkOk(s, t) && !g_pool.Busy(Wk(s, t))) wk = t;
    if (wk < 0) return;
    j.redoArg[k] = {&j, static_cast<uint8_t>(k), static_cast<uint8_t>(wk)};
    j.redoState[k].store(kRedoRun, std::memory_order_release);
    SafeRel(g_pool.At(Wk(s, wk)).list);  // nothing: our workers finish their own lists
    if (!g_pool.Submit(Wk(s, wk), &RedoMain, &j.redoArg[k], nullptr))
      j.redoState[k].store(kRedoFail, std::memory_order_release);
  }
}

// Nothing more is queued: a segment waiting for its list draws stock. Guarded (j may be what faulted).
void FailWantsRaw(Job& j) {
  for (auto& st : j.redoState) {
    int want = kRedoWant;
    st.compare_exchange_strong(want, kRedoFail);
  }
}
void FailWantsGuarded(Job& j) {
  __try {
    FailWantsRaw(j);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }
}

void PumpRedo(int s, Job& j) {
  const int lvl = t_dcs;
  __try {
    PumpRedoRaw(s, j);
  } __except (GbFilter(GetExceptionCode(), lvl)) {
    Contained("PumpRedo", GetExceptionCode(), lvl);
    FailWantsGuarded(j);
  }
}

// Render thread: plans segments [from, to) and starts the queued ones.
void PlanRedoRaw(PassCtx& pc, uint32_t from, uint32_t to) {
  if (!g_redoOn.load(std::memory_order_relaxed) || g_disabled.load(std::memory_order_relaxed)) return;
  Job& j = *pc.job;
  Stat& st = g_slot[pc.slot].st;
  const int64_t t0 = defrec::Qpc();
  if (!PlanSegsGuarded(j, from, to, g_swapAhead.load(std::memory_order_relaxed), st)) g_faults++;
  PumpRedo(pc.slot, j);
  st.redoNs += NsSince(t0);
}

void PlanRedo(PassCtx& pc, uint32_t from, uint32_t to) {
  const int lvl = t_dcs;
  __try {
    PlanRedoRaw(pc, from, to);
  } __except (GbFilter(GetExceptionCode(), lvl)) {
    Contained("PlanRedo", GetExceptionCode(), lvl);
  }
}

// Segment k's list(s) at its exec entry: its pieces, or its re-recorded list
// (waited for up to about 2 us per draw, what drawing it stock would cost
// at least). kSExecuted, or kSRedo when the re-recorded list is not there.
int RedoReady(PassCtx& pc, uint32_t k) {
  Job& j = *pc.job;
  int st = j.redoState[k].load(std::memory_order_acquire);
  if (st == kRedoNone) return kSExecuted;
  if (st == kRedoWant) {
    PumpRedo(pc.slot, j);
    st = j.redoState[k].load(std::memory_order_acquire);
  }
  if (st == kRedoRun) {
    const double limit = (std::min)(2000.0, 2.0 * j.segs[k].draws);
    const int64_t t0 = defrec::Qpc();
    const double q = defrec::QpcToUs();
    while ((st = j.redoState[k].load(std::memory_order_acquire)) == kRedoRun && (defrec::Qpc() - t0) * q < limit)
      _mm_pause();
  }
  Stat& s = g_slot[pc.slot].st;
  if (st == kRedoOk && j.redoCl[k]) return kSExecuted;
  (st == kRedoFail ? s.redoFail : s.redoLate)++;
  return kSRedo;
}

int ExecSegment(PassCtx& pc, int k) {
  Job& j = *pc.job;
  Slot& sl = g_slot[pc.slot];
  const int64_t t0 = defrec::Qpc();
  PlanRedo(pc, static_cast<uint32_t>(k), static_cast<uint32_t>(k) + 2);  // a texture swapped by a residual draw
  int why = CheckExecEntry(j);
  if (!why) why = RedoReady(pc, static_cast<uint32_t>(k));
  const bool redo = !why && j.redoState[k].load(std::memory_order_acquire) == kRedoOk;
  if (!why) why = ReplayAndCheckGuarded(j, j.segs[k], &pc.replays);
  if (!why) {
    // DCS's b6-b8 as bound now, into ours (GPU timeline: right before the list).
    ID3D11Buffer* now[2][3] = {};
    g_ctx->VSGetConstantBuffers(kCtx0, 3, now[0]);
    g_ctx->PSGetConstantBuffers(kCtx0, 3, now[1]);
    for (int s = 0; s < 2 && !why; ++s)
      for (int q = 0; q < 3 && !why; ++q) {
        if (!j.ctxBuf[s][q]) continue;
        UINT bytes = 0;
        if (now[s][q]) {
          D3D11_BUFFER_DESC d;
          now[s][q]->GetDesc(&d);
          bytes = d.ByteWidth;
        }
        if (!now[s][q] || bytes != sl.oursBytes[s][q]) why = kSCtx;
      }
    if (!why)
      for (int s = 0; s < 2; ++s)
        for (int q = 0; q < 3; ++q)
          if (j.ctxBuf[s][q]) g_ctx->CopyResource(j.ctxBuf[s][q], now[s][q]);
    for (auto& row : now)
      for (auto*& p : row) SafeRel(p);
  }
  if (!why && j.cockpit) {
    uint32_t seen = 0, drawn = 0;
    why = RtCheckGuarded(j, j.segs[k], &seen, &drawn);
    sl.st.rtSeen += seen;
    sl.st.rtDrawn += drawn;
    if (why == kSRtBound) sl.st.rtBound++;
    if (why == kSFault) g_faults++;
  }
  if (!why) {
    uint32_t rt = 0;
    if (!ReplayRtGuarded(j, j.segs[k], &rt)) {
      g_faults++;
      why = kSFault;
    }
    sl.st.rtReplays += rt;
  }
  if (!why) {
    sfilt::NotifyBeforeEcl(g_ctx);  // R53: an open focus-broadcast block closes first (the lists bind DCS's targets)
    defrec::PassState before = {}, after = {};
    const bool t9 = pc.verify == 2;
    if (t9) defrec::Capture(g_ctx, &before);
    const bool gs = gsplit::g_on.load(std::memory_order_relaxed) != 0;  // [Suite] GpuSplit
    if (gs) gsplit::Bracket(1, pc.slot, true);
    const int64_t e0 = defrec::Qpc();
    if (redo) {
      g_ctx->ExecuteCommandList(j.redoCl[k], TRUE);
      sl.st.redoExecuted++;
    } else {
      for (uint32_t q = j.segs[k].pieceFirst; q < j.segs[k].pieceEnd; ++q) g_ctx->ExecuteCommandList(pc.cl[q], TRUE);
    }
    sfilt::NotifyEcl(g_ctx);  // d3d11 toggled the context table's draw / update entries (in-pass hooks repair)
    t_segExecuted = true;
    pc.executeNs += NsSince(e0);
    if (gs) {
      gsplit::Bracket(1, pc.slot, false);
      for (uint32_t c = j.segs[k].candFirst; c < j.segs[k].candEnd; ++c) {
        const Cand& cd = j.cands[c];
        if (cd.alive && cd.mesh)
          gsplit::MeshDraw(1, pc.slot, cd.mesh->ib, cd.mesh->ibFormat, cd.mesh->indexCount, 1, false, nullptr, 0);
      }
    }
    pflush::AfterExecute();  // [Model] PassFlush 0x20 (one relaxed load when off)
    if (t9) {
      defrec::Capture(g_ctx, &after);
      if (!defrec::Equal(before, after)) pc.stateBad = true;
      defrec::Release(before);
      defrec::Release(after);
    }
    if (sfilt::Live()) sfilt::g_sh.ForgetAll();  // restored exactly; the filter's shadow is cheap to rebuild
  }
  pc.execNs += NsSince(t0);
  return why;
}

void ExecBody(void* self, void* ctx) {
  PassCtx* pc = g_pass;
  const LoopObj* o = static_cast<LoopObj*>(self);
  if (!pc || GetCurrentThreadId() != g_renderTid || o->slot != pc->slot || !pc->job) return;
  const int k = o->seg;
  if (k < 0 || k >= static_cast<int>(pc->job->segCount)) return;
  if (pc->state == kPsLive && !g_disabled.load(std::memory_order_relaxed)) {
    const int why = ExecSegment(*pc, k);
    pc->segWhy[k] = static_cast<uint8_t>(why);
    if (!why) {
      pc->segDone[k] = 1;
      return;
    }
  }
  DrawSegmentStock(*pc->job, k, ctx);
}

uint64_t __fastcall ExecVt1(void* self, void* ctx) {
  const int lvl = t_dcs;
  t_segExecuted = false;
  __try {
    ExecBody(self, ctx);
  } __except (GbFilter(GetExceptionCode(), lvl)) {
    Contained("an exec entry", GetExceptionCode(), lvl);
    PassCtx* pc = g_pass;
    const LoopObj* o = static_cast<LoopObj*>(self);
    if (pc && pc->job && o->seg >= 0 && o->seg < static_cast<int>(pc->job->segCount)) {
      pc->state = kPsStock;
      if (!t_segExecuted) DrawSegmentStock(*pc->job, o->seg, ctx);  // DCS's own draws (a fault there is DCS's)
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
shrec::Capture g_cap;  // render thread, during one probe call
Extra g_extra;
const VsStatic* g_capSt = nullptr;

// ---- State dump ([Suite] GBufferRecStateDump=1, for tools/fx_states.py check) ----
// One JSON line per new (DX11Shader, technique, renderer+0xd4) seen by a probe:
// the states DCS's draw bound, by GetDesc, in fx_states.py's dump format.
// Lines are queued on the render thread and appended by the 10 s log to
// gbrec_states.jsonl next to the ini in use.
std::atomic<bool> g_dumpOn{false};
std::mutex g_dumpMutex;
std::string g_dumpBuf;  // complete lines, under g_dumpMutex
std::atomic<uint64_t> g_dumpLines{0}, g_dumpDropped{0};
constexpr size_t kDumpSeen = 8192;  // power of two
constexpr uint64_t kDumpMaxLines = 20000;
struct DumpSeen {
  const void* shader;
  uint64_t tech;
  uint32_t flags;
};
DumpSeen* g_dumpSeen = nullptr;  // render thread
uint32_t g_dumpSeenCount = 0;

void JsonStr(std::string& o, const char* s) {
  o += '"';
  for (; *s; ++s) {
    const unsigned char ch = static_cast<unsigned char>(*s);
    if (ch == '"' || ch == '\\') {
      o += '\\';
      o += static_cast<char>(ch);
    } else if (ch < 0x20) {
      char b[8];
      snprintf(b, sizeof(b), "\\u%04x", ch);
      o += b;
    } else {
      o += static_cast<char>(ch);
    }
  }
  o += '"';
}

void JsonNum(std::string& o, double v) {
  char b[32];
  snprintf(b, sizeof(b), "%.9g", v);
  o += b;
}

void JsonUInt(std::string& o, uint32_t v) {
  char b[16];
  snprintf(b, sizeof(b), "%u", v);
  o += b;
}

// The line for one probe; null state objects are D3D11's defaults (what they
// bind). Pure (offline tested).
std::string StateJson(const char* key, const char* tech, uint32_t pass, uint32_t flags, const D3D11_BLEND_DESC* bd,
                      const float factor[4], UINT mask, const D3D11_DEPTH_STENCIL_DESC* dd, UINT ref,
                      const D3D11_RASTERIZER_DESC* rd) {
  const CD3D11_BLEND_DESC defB{CD3D11_DEFAULT()};
  const CD3D11_DEPTH_STENCIL_DESC defD{CD3D11_DEFAULT()};
  const CD3D11_RASTERIZER_DESC defR{CD3D11_DEFAULT()};
  const D3D11_BLEND_DESC& b = bd ? *bd : defB;
  const D3D11_DEPTH_STENCIL_DESC& d = dd ? *dd : defD;
  const D3D11_RASTERIZER_DESC& r = rd ? *rd : defR;
  std::string o;
  o.reserve(1024);
  o += "{\"key\":";
  JsonStr(o, key);
  o += ",\"tech\":";
  JsonStr(o, tech);
  o += ",\"pass\":";
  JsonUInt(o, pass);
  o += ",\"flags\":";
  JsonUInt(o, flags);
  o += ",\"blend\":{\"a2c\":";
  JsonUInt(o, b.AlphaToCoverageEnable ? 1 : 0);
  o += ",\"ind\":";
  JsonUInt(o, b.IndependentBlendEnable ? 1 : 0);
  o += ",\"rt\":[";
  for (int i = 0; i < 8; ++i) {
    const D3D11_RENDER_TARGET_BLEND_DESC& t = b.RenderTarget[i];
    const uint32_t v[8] = {t.BlendEnable ? 1u : 0u,
                           static_cast<uint32_t>(t.SrcBlend),
                           static_cast<uint32_t>(t.DestBlend),
                           static_cast<uint32_t>(t.BlendOp),
                           static_cast<uint32_t>(t.SrcBlendAlpha),
                           static_cast<uint32_t>(t.DestBlendAlpha),
                           static_cast<uint32_t>(t.BlendOpAlpha),
                           t.RenderTargetWriteMask};
    o += i ? ",[" : "[";
    for (int k = 0; k < 8; ++k) {
      if (k) o += ',';
      JsonUInt(o, v[k]);
    }
    o += ']';
  }
  o += "]},\"factor\":[";
  for (int k = 0; k < 4; ++k) {
    if (k) o += ',';
    JsonNum(o, factor[k]);
  }
  o += "],\"mask\":";
  JsonUInt(o, mask);
  auto face = [&](const D3D11_DEPTH_STENCILOP_DESC& f) {
    o += '[';
    JsonUInt(o, f.StencilFailOp);
    o += ',';
    JsonUInt(o, f.StencilDepthFailOp);
    o += ',';
    JsonUInt(o, f.StencilPassOp);
    o += ',';
    JsonUInt(o, f.StencilFunc);
    o += ']';
  };
  o += ",\"dss\":{\"de\":";
  JsonUInt(o, d.DepthEnable ? 1 : 0);
  o += ",\"dwm\":";
  JsonUInt(o, d.DepthWriteMask);
  o += ",\"df\":";
  JsonUInt(o, d.DepthFunc);
  o += ",\"se\":";
  JsonUInt(o, d.StencilEnable ? 1 : 0);
  o += ",\"srm\":";
  JsonUInt(o, d.StencilReadMask);
  o += ",\"swm\":";
  JsonUInt(o, d.StencilWriteMask);
  o += ",\"ff\":";
  face(d.FrontFace);
  o += ",\"bf\":";
  face(d.BackFace);
  o += "},\"ref\":";
  JsonUInt(o, ref);
  o += ",\"rs\":{\"fill\":";
  JsonUInt(o, r.FillMode);
  o += ",\"cull\":";
  JsonUInt(o, r.CullMode);
  o += ",\"fcc\":";
  JsonUInt(o, r.FrontCounterClockwise ? 1 : 0);
  o += ",\"db\":";
  char db[16];
  snprintf(db, sizeof(db), "%d", r.DepthBias);
  o += db;
  o += ",\"dbc\":";
  JsonNum(o, r.DepthBiasClamp);
  o += ",\"ssdb\":";
  JsonNum(o, r.SlopeScaledDepthBias);
  o += ",\"dce\":";
  JsonUInt(o, r.DepthClipEnable ? 1 : 0);
  o += ",\"se\":";
  JsonUInt(o, r.ScissorEnable ? 1 : 0);
  o += ",\"ms\":";
  JsonUInt(o, r.MultisampleEnable ? 1 : 0);
  o += ",\"aal\":";
  JsonUInt(o, r.AntialiasedLineEnable ? 1 : 0);
  o += "}}\n";
  return o;
}

bool CopyKeyGuarded(const uint8_t* shader, char* out, size_t cap) {
  __try {
    return shadowinst::CopyStdString(shader + 0x78, out, cap);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

// Render thread, after a probe whose draw was read back: queues the line once
// per (shader, technique, flags).
void DumpState(uint8_t* shader, uint64_t tech, const shrec::Capture& c) {
  if (!g_dumpSeen) {
    g_dumpSeen = static_cast<DumpSeen*>(calloc(kDumpSeen, sizeof(DumpSeen)));
    if (!g_dumpSeen) return;
  }
  if (g_dumpSeenCount >= kDumpSeen / 2) return;
  size_t h = static_cast<size_t>((reinterpret_cast<uintptr_t>(shader) >> 4) * 0x9e3779b97f4a7c15ull ^
                                 tech * 0xff51afd7ed558ccdull ^ c.flags);
  for (;; ++h) {
    DumpSeen& e = g_dumpSeen[h & (kDumpSeen - 1)];
    if (!e.shader) {
      e.shader = shader;
      e.tech = tech;
      e.flags = c.flags;
      ++g_dumpSeenCount;
      break;
    }
    if (e.shader == shader && e.tech == tech && e.flags == c.flags) return;
  }
  char key[1024], name[128];
  if (!CopyKeyGuarded(shader, key, sizeof(key))) snprintf(key, sizeof(key), "?shader %p", static_cast<void*>(shader));
  if (!shrec::TechNameGuarded(shader, tech, name, sizeof(name)))
    snprintf(name, sizeof(name), "?technique 0x%llx", static_cast<unsigned long long>(tech));
  D3D11_BLEND_DESC bd;
  D3D11_DEPTH_STENCIL_DESC dd;
  D3D11_RASTERIZER_DESC rd;
  if (c.bs) c.bs->GetDesc(&bd);
  if (c.dss) c.dss->GetDesc(&dd);
  if (c.rs) c.rs->GetDesc(&rd);
  std::string line = StateJson(key, name, 0, c.flags, c.bs ? &bd : nullptr, c.blendFactor, c.sampleMask,
                               c.dss ? &dd : nullptr, c.stencilRef, c.rs ? &rd : nullptr);
  std::lock_guard<std::mutex> lock(g_dumpMutex);
  if (g_dumpLines.load() >= kDumpMaxLines) {
    g_dumpDropped++;
    return;
  }
  g_dumpBuf += line;
  g_dumpLines++;
}

// The ini in use: the deployed one, or the one its [Dev] IniPath names.
std::wstring DumpIni() {
  std::wstring ini = g_dir + L"DcsQvCull.ini";
  const qvdev::DevPath dev = qvdev::ReadDevPath(L"IniPath", ini);  // R62: as LoadConfig reads it
  if (dev.state == qvdev::kDevOk) ini = dev.path;
  return ini;
}

// Any thread but the render thread: reads the switches, appends the queued lines.
void PollStateDump() {
  const std::wstring ini = DumpIni();
  const bool on = GetPrivateProfileIntW(L"Suite", L"GBufferRecStateDump", 0, ini.c_str()) != 0;
  // Re-recording of segments whose textures changed, and swaps done ahead (both on unless set to 0).
  const bool redo = GetPrivateProfileIntW(L"Model", L"GBufferRecorderRedo", 1, ini.c_str()) != 0;
  const bool ahead = GetPrivateProfileIntW(L"Model", L"GBufferRecorderSwapAhead", 1, ini.c_str()) != 0;
  if (redo != g_redoOn.load() || ahead != g_swapAhead.load()) {
    g_redoOn = redo;
    g_swapAhead = ahead;
    Log("gbuffer recorder: re-recording of segments whose textures changed %s, due swaps done ahead %s",
        redo ? "on" : "off", ahead ? "on" : "off");
  }
  const std::wstring out = ini.substr(0, ini.find_last_of(L"\\/") + 1) + L"gbrec_states.jsonl";
  if (on != g_dumpOn.load()) {
    g_dumpOn = on;
    Log("gbuffer recorder: state dump %s (%ls)", on ? "on, one line per new key and flags" : "off", out.c_str());
  }
  std::string buf;
  {
    std::lock_guard<std::mutex> lock(g_dumpMutex);
    buf.swap(g_dumpBuf);
  }
  if (buf.empty()) return;
  FILE* f = nullptr;
  if (_wfopen_s(&f, out.c_str(), L"ab") != 0 || !f) {
    Log("gbuffer recorder: state dump: cannot append to %ls", out.c_str());
    return;
  }
  fwrite(buf.data(), 1, buf.size(), f);
  fclose(f);
}

void DrawAfter(void*, int, void* shader, int prim, int a4, int a5, int instances) {
  shrec::Capture& c = g_cap;
  if (c.draws++) return;
  if (shader) shrec::ReadStreamsGuarded(static_cast<const uint8_t*>(shader), c);
  c.prim = prim;
  c.a4 = a4;
  c.a5 = a5;
  c.instances = instances;
  c.rendererOk = shrec::ReadRenderer(&c.flags, &c.dbg);
  shrec::CaptureState(g_ctx, g_capSt->cbSlot, g_capSt->sbSlot, c);
  CaptureExtra(g_ctx, g_extra);
}

ID3D11ShaderResourceView* PageSrvGuarded(const Env& env, uint32_t page) {
  __try {
    return PageSrv(env, page);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return nullptr;
  }
}

bool ReadProbeInRaw(void* self, ProbeIn& in, const Env& env) {
  __try {
    in.r = static_cast<uint8_t*>(self);
    if (*reinterpret_cast<void**>(in.r) != env.srVt) return false;
    const bool ck = in.r[0x64] != 0;
    if (*reinterpret_cast<uint32_t*>(in.r + 0x60) != 1 || (ck && !g_cockpit.load(std::memory_order_relaxed)))
      return false;
    in.item = *reinterpret_cast<uint8_t**>(in.r + 0x10);
    in.mat = in.item ? *reinterpret_cast<uint8_t**>(in.item + 0x10) : nullptr;
    if (!in.mat || *reinterpret_cast<void**>(in.mat) != env.modelVt) return false;
    const uint8_t* props = *reinterpret_cast<uint8_t**>(in.mat + 0x28);
    if (!props || props[0x33]) return false;
    in.shader = *reinterpret_cast<uint8_t**>(in.mat + 0x30);
    if (!in.shader || *reinterpret_cast<void**>(in.shader) != env.shaderVt) return false;
    in.tech = *reinterpret_cast<uint64_t*>(in.mat + (ck ? 0x1e0 : 0x1d8));  // normal_cockpit* in the cockpit [V 0x16084]
    in.mesh = *reinterpret_cast<uint8_t**>(in.item + 0xc0);
    in.page = *reinterpret_cast<uint32_t*>(in.item + 0xd0);
    in.effect = *reinterpret_cast<void**>(in.shader + 0x50);
    in.techBegin = *reinterpret_cast<void**>(in.shader + 0xb0);
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

// DCS's own draw of the item, read back right after its D3D draw. Every
// change to DCS state is undone in the __finally. Plain.
bool ProbeCallRaw(void* self, void* ctx, uint64_t* ret) {
  bool vt = false;
  __try {
    shadowbatch::t_after = &DrawAfter;
    *shadowbatch::g_rendererObj = shadowbatch::g_myVtbl;
    vt = true;
    t_itemDrawn = true;
    *ret = gbcount::g_orig(self, ctx);
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
    Disable("access violation in a probed draw");
    *ret = 0;
    return true;
  }
}

// Probes the item when its key or mesh is still unknown: true = drawn here
// (DCS's own draw), false = not probed (the caller draws it as usual).
bool TryProbe(PassCtx& pc, void* self, void* ctx, uint64_t* ret) {
  ProbeIn in;
  auto skip = [](int why) {
    g_probeSkip[why]++;
    return false;
  };
  if (!ReadProbeInRaw(self, in, g_env)) return skip(kSkNotModel);
  in.scope = pc.job ? pc.job->keyScope : KeyScopeOf(pc.slot);  // the scope the job's keys were looked up in
  in.ordinal = SlotOrdinal(pc.slot);
  const GbKey* k = FindKey(g_tab, in.shader, in.tech, pc.flags, in.effect, in.techBegin, in.scope);
  if (k && k->state.load() <= 0) return skip(kSkRejected);
  if (!k && KeyCooling(g_tab, in.shader, in.tech, pc.flags, in.effect, in.techBegin, in.scope, g_entryGen, false))
    return skip(kSkCooling);
  if (k) {
    // Learned, unless another mesh took the address (and was not relearnt lately).
    const shrec::MeshEntry* m = shrec::FindMesh(g_tab.mesh, in.mesh, in.shader, in.tech, in.effect, in.techBegin);
    if (m && (shrec::LiveMeshSameGuarded(in.mesh, *m) ||
              shrec::MeshCooling(g_tab.mesh, in.mesh, in.shader, in.tech, in.effect, in.techBegin, g_entryGen)))
      return skip(kSkKnown);
  }
  const ReadsEntry* rd = FindReads(g_tab, in.shader, in.tech, in.effect, in.techBegin);
  if (!rd) return skip(kSkNoReads);  // resolved after the pass; probed later
  VsStatic st;
  if (k) {
    st = k->st;
  } else if (StaticFor(in.shader, in.tech, st)) {
    // Not recordable whatever the draw binds: published without a probe.
    shrec::Capture none;
    none.flags = pc.flags;
    ProcessProbe(g_tab, in, st, Facts(), none, Extra(), g_entryGen);
    return skip(kSkStatic);
  }
  Facts f;
  f.dcsVs = shrec::FxPassVsGuarded(in.shader, in.tech);
  const shadowinst::MapEntry* me = shadowinst::FindEntry(in.shader, in.tech, 0);
  if (!me || me->dcsVs != f.dcsVs) f.dcsVs = nullptr;
  f.pageSrv = PageSrvGuarded(g_env, in.page);
  f.meshWhy = shrec::ReadMeshGuarded(in.mesh, f.mesh);
  memcpy(f.passCtx, pc.frontCtx, sizeof(f.passCtx));
  f.reads = rd;
  if (*shadowbatch::g_rendererApi != static_cast<void*>(shadowbatch::g_rendererObj) ||
      *shadowbatch::g_rendererObj != static_cast<void*>(shadowbatch::g_rendererVtbl))
    return skip(kSkRenderer);
  g_cap = shrec::Capture();
  g_extra = Extra();
  g_capSt = &st;
  ProbeCall(self, ctx, ret);
  if (rd->state > 0) ReadSetsGuarded(in, *rd, f);
  if (g_cap.ps) {
    const shrec::FxDeps d = shrec::FxStageDepsGuarded(in.shader, in.tech, 1, g_cap.ps, g_cap.psSamp);
    f.psDepsOk = d.ok;
    if (d.ok) f.psSampDeps = d.samp, f.psCbDeps = d.cb;
  }
  if (g_cap.vs) {
    const shrec::FxDeps d = shrec::FxStageDepsGuarded(in.shader, in.tech, 0, g_cap.vs, g_extra.vsSamp);
    f.vsDepsOk = d.ok;
    if (d.ok) f.vsSampDeps = d.samp;
  }
  const bool poolOk = pc.front && PoolSame(reinterpret_cast<void* const*>(g_extra.vsSamp), pc.pool[0]) &&
                      PoolSame(reinterpret_cast<void* const*>(g_cap.psSamp), pc.pool[1]);
  if (g_cap.draws != 1 || !g_cap.rendererOk || (g_cap.dbg & kDbgBits)) {
    g_probeOdd++;
  } else if (!poolOk) {
    // The lists bind the front entry's pool at s5-s15: a draw that sees other
    // samplers there (an FX dependency on a pool slot, or the pool rebound) is
    // not learnt from; probed again later.
    if (g_probePool++ < 8)
      Log("gbuffer recorder: probe of shader %p technique 0x%llx: sampler pool slots differ from the front "
          "entry's (front entry %s); not learnt",
          static_cast<void*>(in.shader), static_cast<unsigned long long>(in.tech), pc.front ? "seen" : "not seen");
  } else if (!g_disabled.load()) {
    ProcessProbe(g_tab, in, st, f, g_cap, g_extra, g_entryGen);
  }
  if (g_cap.draws == 1 && g_cap.rendererOk && g_dumpOn.load(std::memory_order_relaxed))
    DumpState(in.shader, in.tech, g_cap);
  shrec::ReleaseCapture(g_cap);
  ReleaseExtra(g_extra);
  g_capSt = nullptr;
  g_probes++;
  --pc.probesLeft;
  return true;
}

// The 12 bytes DCS's own draw just wrote at mat+0x110 (pass 1 opaque, not
// cockpit; cockpit too with the cockpit on: 0x15e20 writes them before the
// technique choice [V 0x15ff5-0x1600d]), for this execution's inputs. Plain.
bool LearnRatioRaw(void* self, uint8_t out[kRatioLen]) {
  auto* r = static_cast<uint8_t*>(self);
  if (*reinterpret_cast<void**>(r) != g_env.srVt || *reinterpret_cast<uint32_t*>(r + 0x60) != 1 ||
      (r[0x64] && !g_cockpit.load(std::memory_order_relaxed)))
    return false;
  auto* item = *reinterpret_cast<uint8_t**>(r + 0x10);
  auto* mat = item ? *reinterpret_cast<uint8_t**>(item + 0x10) : nullptr;
  if (!mat || *reinterpret_cast<void**>(mat) != g_env.modelVt) return false;
  const uint8_t* props = *reinterpret_cast<uint8_t**>(mat + 0x28);
  if (!props || props[0x33]) return false;
  memcpy(out, mat + kCbOff + kRatioCb, kRatioLen);
  return true;
}

bool LearnRatioGuarded(void* self, uint8_t out[kRatioLen]) {
  __try {
    return LearnRatioRaw(self, out);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

uint64_t CallItem(void* self, void* ctx) {
  uint64_t r = 0;
  t_itemDrawn = true;
  ++t_dcs;
  if (gbcount::OverrideFn p = g_prevOverride) {
    if (!p(self, ctx, &r)) r = gbcount::g_orig(self, ctx);
  } else {
    r = gbcount::g_orig(self, ctx);
  }
  --t_dcs;
  return r;
}

bool OverrideBody(void* self, void* ctx, uint64_t* ret) {
  PassCtx* pc = g_pass;
  if (pc && GetCurrentThreadId() == g_renderTid) {
    if (pc->job && pc->job->wantCount) {
      const int64_t k = Locate(*pc, self);
      if (k >= 0 && pc->job->items[k].want) {
        if (pc->probesLeft <= 0)
          g_probeSkip[kSkBudget]++;
        else if (TryProbe(*pc, self, ctx, ret))
          return true;
      }
    }
    if (!pc->ratioSeen && pc->front && pc->fb) {
      *ret = CallItem(self, ctx);
      uint8_t b[kRatioLen];
      if (LearnRatioGuarded(self, b)) {
        pc->ratioSeen = true;
        Learned& l = g_slot[pc->slot].learn;
        memcpy(l.ratio, b, kRatioLen);
        l.ratioFb = pc->fb;
        l.ratioVp[0] = pc->vpd[0];
        l.ratioVp[1] = pc->vpd[1];
        l.ratioOk = true;
      }
      return true;
    }
  }
  if (gbcount::OverrideFn p = g_prevOverride) return p(self, ctx, ret);
  return false;
}

bool Override(void* self, void* ctx, uint64_t* ret) {
  const int lvl = t_dcs;
  t_itemDrawn = false;
  __try {
    return OverrideBody(self, ctx, ret);
  } __except (GbFilter(GetExceptionCode(), lvl)) {
    Contained("the item override", GetExceptionCode(), lvl);
  }
  if (!t_itemDrawn) *ret = CallItem(self, ctx);  // DCS's own draw of the item (a fault there is DCS's)
  return true;
}

void PrevWrap(void* pass, void* ctx, gbpass::ExecFn orig) {
  t_passRan = true;
  ++t_dcs;
  if (gbpass::WrapFn p = g_prevWrap)
    p(pass, ctx, orig);
  else
    orig(pass, ctx);
  --t_dcs;
}

// DCS's loop runs over our list: {begin, end} of the vector swapped for the
// call, restored in the __finally (read once by 0x88de0 at 0x896e1 [V]).
struct Swap {
  gbpass::ExecFn orig;
  void** vec;
  void** list;
  size_t count;
};
Swap g_swap;

void __fastcall SwapOrigRaw(void* pass, void* ctx) {
  void** vec = g_swap.vec;
  void* b0 = vec[0];
  void* e0 = vec[1];
  bool swapped = false;
  __try {
    vec[0] = g_swap.list;
    vec[1] = g_swap.list + g_swap.count;
    swapped = true;
    g_swap.orig(pass, ctx);
  } __finally {
    if (swapped) {
      vec[0] = b0;
      vec[1] = e0;
    }
  }
}

// [front, the vector's items] for a pass drawn stock (the front still learns).
bool StockListRaw(Slot& sl, void** vec, size_t* count) {
  auto** b = static_cast<void**>(vec[0]);
  auto** e = static_cast<void**>(vec[1]);
  if (!b || e <= b || static_cast<size_t>(e - b) > kMaxItems) return false;
  const size_t n = static_cast<size_t>(e - b);
  sl.stockList[0] = &sl.front;
  memcpy(sl.stockList + 1, b, n * sizeof(void*));
  *count = n + 1;
  return true;
}
bool StockListGuarded(Slot& sl, void** vec, size_t* count) {
  __try {
    return StockListRaw(sl, vec, count);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

void RunPass(PassCtx& pc, void* pass, void* ctx, gbpass::ExecFn orig, void** vec) {
  Slot& sl = g_slot[pc.slot];
  if (t_own2) {
    const void* key = g_ownKey.load(std::memory_order_acquire);
    if (pc.state == kPsArmed && key && pc.job->ownKey == key) {
      g_own2Live++;  // the job's list brackets the ownship's runs (none here: nothing to switch)
      g_own2Items += pc.job->ownItems;
      g_own2Runs += pc.job->ownRuns;
      g_own2RecItems += pc.job->ownRecItems;
      t_own2 = false;
    } else {
      if (!key) g_own2NoKey++;
      // Stage 1's vector (the switch entries around the ownship's runs): a ready job dropped only for its key.
      Own2Fallback(vec, pc.state == kPsArmed ? kOwnFbKey : kOwnFbStock);
      if (pc.state == kPsArmed) {  // the job is not used: DCS's loop over that vector
        pc.state = kPsStock;
        pc.reason = kPOwnship;
        g_pass = nullptr;
        PrevWrap(pass, ctx, orig);
        return;
      }
      // A stock run (verify's first run, a learning pass): the stock list below is built from that vector, so the
      // front entry still runs (verify's capture) and the ownship is drawn as in the recorded run.
    }
  }
  g_pass = &pc;
  size_t count = 0;
  if (pc.state == kPsArmed) {
    g_swap = {orig, vec, pc.job->swapList, pc.job->swapCount};
    PrevWrap(pass, ctx, &SwapOrigRaw);
  } else if (sl.stockList && StockListGuarded(sl, vec, &count)) {
    g_swap = {orig, vec, sl.stockList, count};
    PrevWrap(pass, ctx, &SwapOrigRaw);
  } else {
    PrevWrap(pass, ctx, orig);
  }
  g_pass = nullptr;
  if (pc.state == kPsArmed) {  // the front entry did not run: DCS drew nothing of ours
    pc.state = kPsStock;
    pc.reason = kPNoEntry;
  }
  if (pc.state == kPsLive) {
    bool any = false;
    for (uint32_t k = 0; k < pc.job->segCount; ++k) any |= pc.segDone[k] != 0;
    if (any) {
      const int64_t t0 = defrec::Qpc();
      if (!RestoreGuarded(*pc.job, pc.segDone)) {
        g_faults++;
        Disable("access violation restoring the materials' and FX variables' end state");
      }
      sl.st.restoreNs += NsSince(t0);
    }
  }
}

// Stock run, then recorded run (or stock again until kAaPasses consecutive
// stock vs stock compares are equal), same frame, from the same target
// contents; every target compared bit for bit. A stock vs stock difference
// (a view or load transition) is logged and retried, never latched.
constexpr uint64_t kAaPasses = 3;
const char* g_verifyWhy = nullptr;  // why the last capture failed
std::atomic<bool> g_verifyNoLatch{false};  // [Suite] GBufferRecVerifyNoLatch (test only): log differences, stay on
void VerifyPass(PassCtx& pc, void* pass, void* ctx, gbpass::ExecFn orig, void** vec) {
  Stat& st = g_slot[pc.slot].st;
  const bool aa = st.aaClean.load() < kAaPasses;
  const bool own2 = t_own2;  // stage 2: every run decides the ownship alike (a stock run consumes the request)
  PassCtx a;
  a.slot = pc.slot;
  a.state = kPsStockRun;
  a.verify = 1;
  RunPass(a, pass, ctx, orig, vec);
  if (!a.captured) {
    st.verifySkipped++;
    if (a.captureWhy) {
      st.verifyErrors++;
      g_verifyWhy = a.captureWhy;
    }
    gbverify::ReleaseViews();
    // The recorded lists were not used: this execution was drawn stock.
    pc.state = kPsStock;
    pc.reason = kPVerifyCapture;
    return;
  }
  gbverify::CopyAll(true, true);    // A
  gbverify::CopyAll(false, false);  // init back into the targets
  // Stage 2 (CockpitTrueIpdOwnship=2): the ownship mask. A third run, stock with the ownship's camera 10 km aside
  // (out of the view), from the same init: a texel that differs from A there is one the ownship covers or shades
  // (an ownship texel). The compare below then counts the stock-vs-recorded differences inside and outside it; only
  // those outside (non-ownship texels, and every MSAA sample: no mask there) latch the recorder off.
  bool masked = false;
  if (own2 && !aa && g_ownAwayFn) {
    PassCtx m;
    m.slot = pc.slot;
    m.state = kPsStockRun;
    m.probesLeft = 0;
    t_own2 = true;
    g_ownAwayFn(true);
    RunPass(m, pass, ctx, orig, vec);
    g_ownAwayFn(false);
    masked = gbverify::CopyAllMask();
    gbverify::CopyAll(false, false);  // init back again
    masked = masked && gbverify::BuildMask();
    g_ownMaskRuns++;
    if (!masked) g_ownMaskFailed++;
  }
  PassCtx b;
  if (aa) {
    b.slot = pc.slot;
    b.state = kPsStockRun;
  } else {
    b = pc;
  }
  b.verify = 2;
  b.probesLeft = 0;
  t_own2 = own2;
  RunPass(b, pass, ctx, orig, vec);
  bool executed = false;
  if (!aa && b.state == kPsLive)
    for (uint32_t k = 0; k < b.job->segCount; ++k) executed |= b.segDone[k] != 0;
  if (!b.sameViews || (!aa && !executed)) {
    st.verifySkipped++;
  } else {
    gbverify::CopyAll(true, false);  // B
    int64_t bad[gbverify::kMaxTargets];
    uint64_t texels = 0;
    gbverify::g_badBox[0] = gbverify::g_badBox[1] = 1 << 30;
    gbverify::g_badBox[2] = gbverify::g_badBox[3] = -1;
    gbverify::g_maskOn = masked;
    const int64_t total = gbverify::CompareGuarded(bad, &texels);
    gbverify::g_maskOn = false;
    // Non-ownship differences: all of them without a mask; with one, the staging texels outside it plus every
    // difference the mask cannot classify (MSAA samples).
    const int64_t inMask = masked ? gbverify::g_badIn : 0;
    const int64_t counted = total < 0 ? total : total - inMask;
    if (masked && total >= 0) {
      g_ownMaskIn += static_cast<uint64_t>(inMask);
      g_ownMaskOut += static_cast<uint64_t>(counted);
      g_ownMaskCompares++;
    }
    if (total < 0) {
      st.verifyErrors++;
    } else {
      (aa ? st.verifyAa : st.verifyAb)++;
      st.verifyTexels += texels;
      if (aa) st.aaClean = total > 0 ? 0 : st.aaClean.load() + 1;
      if (total > 0) {
        std::string per;
        char buf[48];
        for (int i = 0; i < gbverify::kMaxTargets; ++i) {
          if (!gbverify::g_t[i].res) continue;
          snprintf(buf, sizeof(buf), "%s%s%d %lld", per.empty() ? "" : ", ", i == 8 ? "DS" : "RT", i == 8 ? 0 : i,
                   static_cast<long long>(bad[i]));
          per += buf;
        }
        Log("gbuffer recorder verify: %s differs: %lld of %llu texels/samples (%s); box %d,%d..%d,%d%s",
            aa ? "stock vs stock" : "stock vs recorded", static_cast<long long>(total),
            static_cast<unsigned long long>(texels), per.c_str(), gbverify::g_badBox[0], gbverify::g_badBox[1],
            gbverify::g_badBox[2], gbverify::g_badBox[3],
            masked ? ("; ownship texels " + std::to_string(inMask) + ", other " + std::to_string(counted)).c_str()
                   : "");
      }
      if (counted > 0) {
        (aa ? st.verifyAaBad : st.verifyAbBad)++;
        st.verifyBadTexels += static_cast<uint64_t>(counted);
        if (aa)
          Log("gbuffer recorder verify: stock vs stock not reproducible in this frame (a transition?): retried, "
              "%llu consecutive equal compares needed before stock vs recorded",
              static_cast<unsigned long long>(kAaPasses));
        else if (!g_verifyNoLatch.load(std::memory_order_relaxed))
          Disable("G-buffer differs between the stock and the recorded execution");
      }
    }
  }
  if (b.stateBad) {
    st.verifyStateBad++;
    Disable("the immediate context's state differs after ExecuteCommandList(TRUE)");
  }
  if (!aa) st.verifyExecs++;
  gbverify::ReleaseViews();
  if (aa) {
    pc.state = kPsStock;
    pc.reason = kPVerifyAa;
    return;
  }
  pc.state = b.state;
  pc.reason = b.reason;
  memcpy(pc.segDone, b.segDone, sizeof(pc.segDone));
  memcpy(pc.segWhy, b.segWhy, sizeof(pc.segWhy));
  pc.frontNs = b.frontNs;
  pc.execNs = b.execNs;
  pc.executeNs = b.executeNs;
  pc.replays = b.replays;
}

// Waits (within [Model] GBufferRecorderWaitUs) for this render entry's job of
// slot s; decides what the pass does with it. Not ready in time: the pass is
// drawn stock (counted late); the job finishes on its worker and is dropped.
void TakeJob(PassCtx& pc, void** vec) {
  Slot& sl = g_slot[pc.slot];
  Stat& st = sl.st;
  Job& j = *sl.job;
  const int64_t t0 = defrec::Qpc();
  const double budget = static_cast<double>(g_waitUs.load());
  const double k = defrec::QpcToUs();
  while (SlotBusy(pc.slot) && (defrec::Qpc() - t0) * k < budget) _mm_pause();
  const uint64_t ns = NsSince(t0);
  st.waitNs += ns;
  NoteMax(st.waitMaxNs, ns);
  if (SlotBusy(pc.slot)) {
    pc.reason = kPLate;
    sl.lateEntry = g_entryQpc;  // its timeline is read once the workers are idle
    sl.latePassQpc = t0;
    return;
  }
  DropPoolLists(pc.slot);
  if (j.phase.load(std::memory_order_acquire) != kJobBuilt || j.result != kBuildOk ||
      j.recFail.load(std::memory_order_acquire)) {
    ReleaseSegLists(j);
    pc.reason = kPFailed;
    return;
  }
  if (HeapRecycled(j)) g_recycledAtPass.fetch_add(1, std::memory_order_relaxed);  // diagnostic: expected 0
  st.jobsUsed++;
  st.buildNs += static_cast<uint64_t>(j.buildUs * 1000.0);
  st.preNs += static_cast<uint64_t>(j.preUs * 1000.0);
  st.commitNs += static_cast<uint64_t>(j.commitUs * 1000.0);
  double recSum = 0, recMax = 0;
  int64_t recDone = 0;
  for (int r = 0; r < kThreads; ++r) {
    recSum += j.recUs[r];
    recMax = (std::max)(recMax, j.recUs[r]);
    recDone = (std::max)(recDone, j.tRecDone[r]);
  }
  st.recordNs += static_cast<uint64_t>(recSum * 1000.0);
  st.recMaxNs += static_cast<uint64_t>(recMax * 1000.0);
  st.recWorkers += j.segCount ? j.recWorkers : 0;
  st.pieces += j.pieceCount;
  st.helpersQueued += j.helpers;
  NoteWork(sl, JobWorkUs(j));
  st.jobItems += j.n;
  st.helperItems += j.n - j.preItems[0];
  st.tlSamples++;
  st.early += j.startedEarly;
  st.tlPass += QpcNs(g_entryQpc, t0);
  st.tlStart += QpcNs(g_entryQpc, j.tStart);
  st.tlPre += QpcNs(g_entryQpc, j.tPreDone ? j.tPreDone : j.tBuilt);
  st.tlBuilt += QpcNs(g_entryQpc, j.tBuilt);
  st.tlRec += QpcNs(g_entryQpc, recDone ? recDone : j.tBuilt);
  st.tlDone += QpcNs(g_entryQpc, (std::max)(j.tDone, recDone));
  NoteSlack(sl, QpcNs((std::max)(j.tDone, recDone), t0) / 1e6);
  if (!SameVectorGuarded(j, vec)) {
    (j.vec != vec ? st.idOtherVec : st.idSameVec)++;
    ReleaseSegLists(j);
    pc.reason = kPIdentity;
    return;
  }
  pc.job = &j;  // its probe requests apply to this vector
  st.items += j.n;
  for (int r = 0; r < kReasons; ++r) st.itemReason[r] += j.reasons[r];
  st.misses += j.missCount;
  st.refreshes += j.refreshCount;
  st.segPlanned += j.segCount;
  if (!j.segCount || !j.pieceCount) {
    ReleaseSegLists(j);
    pc.reason = j.segCount ? kPFailed : kPNothing;
    return;
  }
  for (uint32_t q = 0; q < j.pieceCount; ++q) {
    pc.cl[q] = j.pieces[q].cl;
    j.pieces[q].cl = nullptr;
  }
  for (uint32_t q = 0; q < j.pieceCount; ++q)
    if (!pc.cl[q]) {
      for (auto*& c : pc.cl) SafeRel(c);
      pc.reason = kPFailed;
      return;
    }
  pc.state = kPsArmed;
}

bool Arm(int s);

// Slot s's job served this entry's pass (or was late for it): once its worker
// is idle, its leftovers are released, the texture table is maintained and
// the next frame's job is armed.
bool RearmIfIdle(int s) {
  Slot& sl = g_slot[s];
  if (SlotBusy(s)) return false;
  DropPoolLists(s);
  ReleaseSegLists(*sl.job);
  sl.armed = false;
  return Arm(s);
}

// Texture table and reads table upkeep from this pass's job (render thread).
void Maintain(Slot& sl) {
  if (!sl.job) return;
  const Job& j = *sl.job;
  const int64_t t0 = defrec::Qpc();
  for (uint32_t k = 0; k < j.readWantCount; ++k) ResolveReads(g_tab, j.readWants[k].shader, j.readWants[k].tech, g_entryGen);
  if (TryAcquireSRWLockExclusive(&g_texLock)) {
    MaintainTex(g_tex, g_env.tex, j, g_entryGen);
    TexEvict(g_tex, g_entryGen, kEvictAge, 512);
    ReleaseSRWLockExclusive(&g_texLock);
  } else {
    g_texDeferred++;
  }
  sl.st.maintNs += NsSince(t0);
}

bool LearnVector(Slot& sl, void** vec, void* ctx) {
  void* rg = nullptr;
  uint32_t idx = 0;
  bool ok = false;
  __try {
    ok = shadowbatch::LearnRaw(vec, ctx, &rg, &idx);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    ok = false;
  }
  if (!ok) return false;
  sl.learn.rg = rg;
  sl.learn.idx = idx;
  sl.learn.haveVec = true;
  g_gbRg = rg;
  g_sortRg.store(rg, std::memory_order_release);
  return true;
}

void WrapSlot(int s, void* pass, void* ctx, gbpass::ExecFn orig) {
  void** vec = gbpass::ItemVector(pass, ctx);
  if (!vec) {
    t_own2 = false;
    return PrevWrap(pass, ctx, orig);
  }
  Slot& sl = g_slot[s];
  LearnVector(sl, vec, ctx);
  sl.st.passes++;
  sl.lastPassQpc = defrec::Qpc();
  PassCtx pc;
  pc.slot = s;
  const bool mine = sl.job && sl.armed && sl.job->entryGen && sl.job->entryGen == g_entryGen;
  if (mine) TakeJob(pc, vec);
  pc.probesLeft = pc.job && pc.job->wantCount ? kProbesPerPass : 0;
  if (pc.job) g_probeWants += pc.job->wantCount;
  if (g_verify.load(std::memory_order_relaxed) && pc.state == kPsArmed)
    VerifyPass(pc, pass, ctx, orig, vec);
  else
    RunPass(pc, pass, ctx, orig, vec);
  // Issue #3: the pass is over; a re-record of its segments still on a worker stops before its next read.
  if (mine) sl.job->redoStop.store(1, std::memory_order_release);
  for (auto*& c : pc.cl) SafeRel(c);
  Stat& st = sl.st;
  st.frontNs += pc.frontNs;
  st.vt23 += pc.replays;
  if (pc.state == kPsLive) {
    uint32_t done = 0;
    for (uint32_t k = 0; k < pc.job->segCount; ++k) {
      st.segs++;
      st.segReason[pc.segDone[k] ? kSExecuted : pc.segWhy[k]]++;
      done += pc.segDone[k];
    }
    if (done) {
      g_workPasses.fetch_add(1, std::memory_order_relaxed);
      const Job& j = *pc.job;
      st.recorded += j.recorded;
      st.draws += j.draws;
      st.cbBytes += j.cbBytes;
      st.execNs += pc.execNs;
      st.executeNs += pc.executeNs;
    }
    if (done == pc.job->segCount)
      st.passReason[kPExecuted]++;
    else
      st.partial++;
  } else {
    st.passReason[pc.reason == kPExecuted ? kPNoEntry : pc.reason]++;
  }
  sl.passGen = g_entryGen;
  g_lastPassQpc = defrec::Qpc();
  if (mine) {
    if (!SlotBusy(s)) Maintain(sl);
    RearmIfIdle(s);
  } else if (PassMayArm(sl.armed)) {
    // Only in a frame whose render entry ran with the recorder active (issue #3: the recorder turned on mid-frame
    // by the kill switch, the governor or the bench arms at the next entry, not in the rest of a frame it did not
    // see begin: its ordinals, the vectors it learnt and the heap generation belong to that whole frame).
    Arm(s);
  }
}

bool EnsureSlot(int s);

// The identity of the execution whose item vector is vec (render graph rg). Plain.
bool ExecKeyRaw(void** vec, void* ctx, CollKey* k, void** rgOut) {
  void* rg = nullptr;
  uint32_t idx = 0;
  if (!shadowbatch::LearnRaw(vec, ctx, &rg, &idx)) return false;
  size_t n = 0;
  const uint8_t* d = CollDescs(rg, &n);
  if (!CollKeyAt(d, n, idx, k)) return false;
  *rgOut = rg;
  return true;
}
bool ExecKeyGuarded(void** vec, void* ctx, CollKey* k, void** rgOut) {
  __try {
    return ExecKeyRaw(vec, ctx, k, rgOut);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    *k = CollKey();
    return false;
  }
}
int CollFindGuarded(void* rg, const CollKey& k) {
  __try {
    size_t n = 0;
    const uint8_t* d = CollDescs(rg, &n);
    return CollFindIn(d, n, k);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return -1;
  }
}
int CollFindVolGuarded(void* rg, uint16_t sm, const void* vol, uint16_t vrank) {
  __try {
    size_t n = 0;
    const uint8_t* d = CollDescs(rg, &n);
    return CollFindVolIn(d, n, sm, vol, vrank);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return -1;
  }
}
// Descriptor idx is the (sm, vol, vrank) collection (cheap fields first). Guarded.
bool CollIsVolGuarded(void* rg, uint32_t idx, uint16_t sm, const void* vol, uint16_t vrank) {
  __try {
    size_t n = 0;
    const uint8_t* d = CollDescs(rg, &n);
    if (!d || idx >= n) return false;
    const uint8_t* e = d + static_cast<size_t>(idx) * kCollStride;
    if (*reinterpret_cast<const uint16_t*>(e) != sm || *reinterpret_cast<const void* const*>(e + 8) != vol) return false;
    return CollFindVolIn(d, n, sm, vol, vrank) == static_cast<int>(idx);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}
bool CollIsGuarded(void* rg, uint32_t idx, const CollKey& k) {
  __try {
    size_t n = 0;
    const uint8_t* d = CollDescs(rg, &n);
    return CollIsAt(d, n, idx, k);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

// The slot bound to identity k (identity mode), or -1.
int SlotOfKey(const CollKey& k) {
  if (!k.ok) return -1;
  for (int s = 0; s < g_poolSlots; ++s)
    if (g_slot[s].id.ok && SameKey(g_slot[s].id, k)) return s;
  return -1;
}

const char* SmName(uint16_t sm) {
  return sm == kSmGbuffer ? "SM_GBUFFER_PBR" : sm == kSmDecal ? "SM_GBUFFER_PBR_DECAL"
                                         : sm == kSmCockpit ? "SM_GBUFFER_PBR_COCKPIT" : "other";
}

bool DropStale(int s);

// A confirmed base sequence: (re)binds the slots (render thread). A slot
// whose identity changes forgets what it learnt for the old execution.
void Rebind() {
  CollKey want[kSlots];
  BindSlots(g_scope.load(), g_cockpit.load() && g_poolSlots > kScopeSlots, g_bind.base, want);
  for (int s = 0; s < g_poolSlots; ++s) {
    Slot& sl = g_slot[s];
    if (SameKey(sl.id, want[s])) continue;
    if (sl.armed && sl.job && !DropStale(s)) continue;  // its job still runs: next base frame
    if (want[s].ok && !EnsureSlot(s)) continue;
    sl.id = want[s];
    sl.lvol = nullptr;
    sl.boundQpc = defrec::Qpc();
    sl.learn.haveVec = false;
    sl.learn.target = false;
    sl.learn.ratioOk = false;
    g_bindChanges++;
    char name[48];
    SlotName(s, name, sizeof(name));
    if (want[s].ok)
      Log("gbuffer recorder: slot %d (%s) bound to the G-buffer execution with %s, viewport tag 0x%x, rank %u", s,
          name, SmName(want[s].sm), want[s].tag, want[s].rank);
    else
      Log("gbuffer recorder: slot %d (%s) unbound (no such execution in the base frame: %u non-cockpit, %u cockpit)",
          s, name, g_bind.base.n, g_bind.base.nck);
  }
}

// R62: the two fallback lines below keep the recorder running. They avoid the
// app's "turned itself off" vocabulary (EngineOptimizations.cs: "off for this
// session", "disabled for this session", "could not", ...), which made the app
// warn that the multi-threaded G-buffer had turned itself off.
constexpr const char kIdentityDroppedFmt[] =
    "gbuffer recorder: execution identity dropped for this session (%s); the scope bits are plain ordinals again, "
    "the recorder keeps running";
constexpr const char kQuadFilterDroppedLine[] =
    "gbuffer recorder: quad-view filter dropped for this session (30 frames in which no G-buffer execution used a "
    "quad view's volume); every execution counts again, the recorder keeps running";

// Identity binding off for the session: the slots by plain ordinal (render
// thread). Logged once with the reason.
void FallbackToOrdinal(const char* why) {
  if (!g_idMode) return;
  g_idMode = false;
  for (int s = 0; s < kSlots; ++s) {
    Slot& sl = g_slot[s];
    if (sl.armed && sl.job) DropStale(s);  // a job still running is dropped at its next arm
    sl.id = CollKey();
    sl.learn.haveVec = false;
    sl.learn.target = false;
    sl.learn.ratioOk = false;
  }
  g_ordinal = 0;
  Log(kIdentityDroppedFmt, why);
}

// The graph of a G-buffer execution, followed when the old one went stale. Plain.
void* ExecGraphRaw(void* ctx) { return *static_cast<void**>(ctx); }
void* ExecGraphGuarded(void* ctx) {
  __try {
    return ExecGraphRaw(ctx);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return nullptr;
  }
}
constexpr double kGraphStaleMs = 500.0, kStarvedMs = 5000.0;
inline int64_t MsTicks(double ms) { return static_cast<int64_t>(ms / (defrec::QpcToUs() / 1000.0)); }

// The ownship's step aside (t_ownAside): the execution drawn by DCS stock over the vector it was handed, the
// slot's job left unused (dropped as stale at the next render entry), the slot counted as having run (its
// starvation check). False: not stepped aside.
bool AsideRun(int s, void* pass, void* ctx, gbpass::ExecFn orig) {
  if (s < 0 || s >= kSlots || !t_ownAside) return false;
  g_ownAsidePasses++;
  g_slot[s].lastPassQpc = defrec::Qpc();
  g_lastPassQpc = g_slot[s].lastPassQpc;
  PrevWrap(pass, ctx, orig);
  return true;
}

void WrapBody(void* pass, void* ctx, gbpass::ExecFn orig) {
  if (Active() && g_pool.Disabled()) Disable("worker fault (see the deferred rec line above)");
  if (!Active()) {
    if (t_own2) Own2Fallback(gbpass::ItemVector(pass, ctx));
    PrevWrap(pass, ctx, orig);
  } else {
    if (!g_renderTid) g_renderTid = GetCurrentThreadId();
    if (GetCurrentThreadId() != g_renderTid) {
      if (t_own2) Own2Fallback(gbpass::ItemVector(pass, ctx));
      PrevWrap(pass, ctx, orig);
    } else {
      if (FollowGraph(g_gbRg, g_gbRgEntryQpc, ExecGraphGuarded(ctx), defrec::Qpc(), MsTicks(kGraphStaleMs))) {
        g_graphChanges++;
        if (g_graphChanges.load() > 1) g_sceneChangeQpc.store(defrec::Qpc(), std::memory_order_release);
        g_ordinal = 0;
        g_bind.cur = FrameSeq();
        g_bind.prevN = 0;
        g_bind.stable = 0;
        if (g_graphChanges.load() > 1)
          Log("gbuffer recorder: the G-buffer executions moved to render graph %p (the one followed had no render entry "
              "for %.0f ms)", g_gbRg, kGraphStaleMs);
      }
      g_gbExecs++;
      if (g_frameQuadGen != g_entryGen) {  // after this frame's culling, which published the quad views
        g_frameQuadGen = g_entryGen;
        g_frameQuad = g_collOk && g_quadFilterOn ? ReadQuadVolumes(defrec::Qpc(), MsTicks(250.0)) : QuadSet();
      }
      ++g_frameExecs;
      CollKey k;
      void* rg = nullptr;
      if (g_collOk) {
        void** vec = gbpass::ItemVector(pass, ctx);
        if ((!vec || !ExecKeyGuarded(vec, ctx, &k, &rg)) && g_idMode) g_idFail++;
      }
      int s = -1;
      if (CountsAsQuad(g_frameQuad, k)) ++g_frameCounted;
      if (!CountsAsQuad(g_frameQuad, k)) {
        g_nonQuad++;  // another view's execution (R24 12): stock, not counted
      } else if (g_idMode) {
        if (!k.ok || rg == g_gbRg) {
          SeqAdd(g_bind.cur, k);
          s = SlotOfKey(k);
        }
      } else if (g_frameQuad.n && k.ok && k.sm == kSmCockpit) {
        g_nonQuad++;  // with the quad views known, the ordinals count their non-cockpit executions
      } else {
        const int ord = g_ordinal++;
        NoteMax(g_ordMax, static_cast<uint64_t>(ord));
        s = SlotOfOrdinal(g_scope.load(), ord);
      }
      if (s >= 0 && k.ok && k.vol) {
        Slot& sl = g_slot[s];
        sl.lvol = k.vol;
        sl.lsm = k.sm;
        sl.lvrank = k.vrank;
      }
      if (AsideRun(s, pass, ctx, orig)) {
        // stepped aside for cockpit_ipd.h's ownship (stock)
      } else if (s < 0 || !EnsureSlot(s)) {
        if (t_own2) Own2Fallback(gbpass::ItemVector(pass, ctx));
        PrevWrap(pass, ctx, orig);
      } else {
        WrapSlot(s, pass, ctx, orig);
      }
    }
  }
}

void Wrap(void* pass, void* ctx, gbpass::ExecFn orig) {
  const int lvl = t_dcs;
  t_passRan = false;
  g_inside.fetch_add(1);
  __try {
    WrapBody(pass, ctx, orig);
  } __except (GbFilter(GetExceptionCode(), lvl)) {
    Contained("the G-buffer pass wrap", GetExceptionCode(), lvl);
    g_pass = nullptr;
    if (!t_passRan) {
      if (t_own2) Own2Fallback(gbpass::ItemVector(pass, ctx));
      PrevWrap(pass, ctx, orig);  // DCS's pass, stock (a fault there is DCS's)
    }
  }
  g_inside.fetch_sub(1);
}

// ---------------------------------------------------------------------------
// Render thread: RenderGraph::render entry; arming; stale jobs
// ---------------------------------------------------------------------------
size_t VectorCountGuarded(void* rg, void* renderables) {
  __try {
    return shadowbatch::VectorCountRaw(static_cast<uint8_t*>(rg), renderables);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return 0;
  }
}

// Arms slot s's job for its next render entry (render thread, worker idle).
// The timeline of the slot's last late job, from the render entry it served
// (workers idle).
void NoteLate(Slot& sl, const Job& j) {
  if (!sl.lateEntry) return;
  const int64_t e = sl.lateEntry;
  sl.lateEntry = 0;
  if (!j.tStart) return;
  int64_t done = j.tDone;
  for (int r = 0; r < kThreads; ++r) done = (std::max)(done, j.tRecDone[r]);
  if (done < j.tStart) return;  // aborted before it finished
  Stat& st = sl.st;
  st.lateSamples++;
  st.latePass += QpcNs(e, sl.latePassQpc);
  st.lateStart += QpcNs(e, j.tStart);
  st.lateBuilt += QpcNs(e, j.tBuilt >= j.tStart ? j.tBuilt : done);
  st.lateDone += QpcNs(e, done);
  NoteWork(sl, JobWorkUs(j));
}

bool Arm(int s) {
  Slot& sl = g_slot[s];
  if (!sl.job || sl.armed) return sl.armed;
  const Learned& l = sl.learn;
  if (!l.haveVec || !l.target || !l.ratioOk) return false;
  if (SlotBusy(s)) {  // a late job's primary or helper still reads it
    sl.st.refused++;
    return false;
  }
  Job& j = *sl.job;
  NoteLate(sl, j);
  DropPoolLists(s);
  ReleaseSegLists(j);
  ReleaseTargetRefs(j);
  j.vec = nullptr;
  j.flags = l.flags;
  j.scope = g_scope.load();
  j.island = g_island.load();
  const bool slack = sl.slackMs > kSlackMs;
  j.keyScope = KeyScopeOf(s);
  j.cockpit = g_cockpit.load();
  j.armById = g_idMode;
  j.armKey = sl.id;
  j.armVol = sl.lvol;
  j.armSm = sl.lsm;
  j.armVrank = sl.lvrank;
  j.maxSeg = g_maxSeg.load();
  if (slack) {
    j.island = (std::min)(j.island, kSlackIsland);
    j.maxSeg = kMaxSegments;
    sl.st.slackJobs++;
  }
  ReleaseRedo(j);
  j.stride = g_verify.load() ? g_stride.load() : 0;
  for (UINT k = 0; k < kRtv; ++k)
    if ((j.rtv[k] = l.rtv[k])) j.rtv[k]->AddRef();
  if ((j.dsv = l.dsv)) j.dsv->AddRef();
  j.nvp = l.nvp;
  j.nsc = l.nsc;
  memcpy(j.vp, l.vp, sizeof(j.vp));
  memcpy(j.sc, l.sc, sizeof(j.sc));
  for (int st = 0; st < 2; ++st)
    for (int k = 0; k < 3; ++k)
      // Referenced: LearnTarget may replace sl.ours (a size change) while
      // this job still records on a worker, which then bound a released
      // buffer. Released with the job's other target references.
      if ((j.ctxRef[st][k] = j.ctxBuf[st][k] = l.ctxBytes[st][k] ? sl.ours[st][k] : nullptr)) j.ctxRef[st][k]->AddRef();
  for (int st = 0; st < 2; ++st)
    for (UINT q = kPoolFirst; q < kSampSlots; ++q)
      if ((j.pool[st][q] = l.pool[st][q])) j.pool[st][q]->AddRef();
  memcpy(j.ratio, l.ratio, kRatioLen);
  j.ratioFb = l.ratioFb;
  j.ratioVp[0] = l.ratioVp[0];
  j.ratioVp[1] = l.ratioVp[1];
  j.front = &sl.front;
  for (int k = 0; k < kMaxSegments; ++k) j.execObj[k] = &sl.exec[k];
  j.env = &g_env;
  j.tabSnap = g_tab;  // the arrays this job reads, whatever a compaction does meanwhile
  j.tab = &j.tabSnap;
  j.tex = &g_tex;
  j.texLock = &g_texLock;
  j.useGen = g_entryGen + 1;
  j.result = kBuildNone;
  j.startedEarly = false;
  j.entryGen = 0;
  j.tStart = j.tDone = 0;
  j.armRg = l.rg;
  j.armIdx = l.idx;
  ResetEvent(j.start);
  ResetEvent(j.preGo);
  ResetEvent(j.recGo);
  j.preState.store(kGoWait, std::memory_order_relaxed);
  j.recState.store(kGoWait, std::memory_order_relaxed);
  j.started.store(0, std::memory_order_relaxed);
  j.ready.store(0, std::memory_order_relaxed);
  j.recFail.store(0, std::memory_order_relaxed);
  j.recWorkers = 1;
  j.pieceCount = 0;
  j.phase.store(kJobEmpty, std::memory_order_relaxed);
  j.startState.store(kStartArmed, std::memory_order_release);  // the sort hook may start it from here on
  // Helpers first (they wait for stage A to open); a busy one (a late job) is left out.
  j.helpers = 0;
  const uint32_t helpers = HelpersFor(sl.workUs, (std::min)(g_helpers.load(), static_cast<uint32_t>(g_threads - 1)));
  static const defrec::JobFn kHelper[kThreads - 1] = {&HelperMain<1>, &HelperMain<2>};
  for (uint32_t h = 0; h < helpers; ++h)
    if (WkOk(s, static_cast<int>(h) + 1) && g_pool.Submit(Wk(s, static_cast<int>(h) + 1), kHelper[h], &j, nullptr))
      ++j.helpers;
  if (!g_pool.Submit(s, &JobMain, &j, nullptr)) {
    int st = kStartArmed;
    if (!j.startState.compare_exchange_strong(st, kStartIdle)) j.startState.store(kStartIdle);
    ReleaseHelpers(j);
    sl.st.refused++;
    return false;
  }
  sl.armed = true;
  sl.st.jobs++;
  return true;
}

// An armed job whose render entry passed without its pass: released once its
// worker is idle. False while it still runs.
bool DropStale(int s) {
  Slot& sl = g_slot[s];
  if (SlotBusy(s)) {
    const int64_t t0 = defrec::Qpc();
    while (SlotBusy(s) && (defrec::Qpc() - t0) * defrec::QpcToUs() < 200.0) _mm_pause();
    if (SlotBusy(s)) return false;
  }
  DropPoolLists(s);
  ReleaseSegLists(*sl.job);
  sl.armed = false;
  return true;
}

// A job started (by Scene's sort) for an earlier frame than this entry's:
// started, not yet given to an entry, and started before the previous entry
// or pass of its graph. Pure (offline tests).
inline bool StaleStart(int startState, uint32_t entryGen, int64_t tStart, int64_t prevEntry) {
  return startState == kStartRun && !entryGen && (!prevEntry || tStart < prevEntry);
}

// The replaced tables are in use while a job armed with them is armed or its
// workers run (render thread).
bool OldTablesInUse() {
  for (int s = 0; s < kSlots; ++s) {
    const Slot& sl = g_slot[s];
    if (sl.job && SharesArrays(sl.job->tabSnap, g_tabOld) && (sl.armed || SlotBusy(s))) return true;
  }
  return false;
}

// Long sessions (render thread, the G-buffer graph's render entry, before its
// jobs are armed): shadow_rec.h's MaintainTables for the G-buffer tables.
void MaintainTables() {
  if (g_tabOld.keys || g_tabOld.reads || g_tabOld.mesh.meshes) {
    if (OldTablesInUse()) return;
    FreeTables(g_tabOld);
    g_oldFreed++;
  }
  if (!g_tab.keys || !g_tab.reads || !g_tab.mesh.meshes ||
      !shrec::CompactDue(g_plan, g_tab.keysUsed, g_tab.mesh.meshesUsed, g_tab.readsUsed, g_tableFull.load(),
                         g_entryGen))
    return;
  const int64_t t0 = defrec::Qpc();
  Tables nt;
  auto done = [&] {
    shrec::CompactDone(g_plan, g_tab.keysUsed, kKeyTable, g_tab.mesh.meshesUsed, g_tableFull.load(), g_entryGen);
    g_plan.nextReads = shrec::NextAt(g_tab.readsUsed, kReadsTable);
  };
  if (!AllocTables(nt)) {
    FreeTables(nt);
    done();
    return;
  }
  shrec::CompactStats cs;
  CompactInto(g_tab, nt, g_entryGen, shrec::kEvictAge, cs);
  g_tabOld = g_tab;
  g_tab = nt;
  cs.ms = (defrec::Qpc() - t0) * defrec::QpcToUs() / 1000.0;
  g_lastCompact = cs;
  done();
  Log("gbuffer recorder: tables compacted (#%llu, render entry %u): %s",
      static_cast<unsigned long long>(++g_compactions), g_entryGen, shrec::CompactText(cs).c_str());
}

void OnRenderBody(void* rg, void* renderables) {
  if (!g_renderTid || GetCurrentThreadId() != g_renderTid) return;
  g_inside.fetch_add(1);
  g_entries++;
  if (!g_gbRg || rg == g_gbRg) {
    g_ordinal = 0;
    ++g_entryGen;
    g_entryQpc = defrec::Qpc();
    g_gbRgEntryQpc = g_entryQpc;
    // The quad filter matched nothing in a whole frame of executions (volumes not the executions' own): off.
    if (g_frameQuad.n && g_frameExecs && !g_frameCounted && ++g_quadMissFrames >= 30 && g_quadFilterOn) {
      g_quadFilterOn = false;
      Log("%s", kQuadFilterDroppedLine);
    } else if (g_frameCounted) {
      g_quadMissFrames = 0;
    }
    g_frameExecs = g_frameCounted = 0;
    if (g_idMode && EndFrame(g_bind) && Active()) {
      if (SeqAmbiguous(g_bind.base)) {
        char why[160];
        snprintf(why, sizeof(why), "executions share a viewport tag: %u non-cockpit, first tag 0x%x", g_bind.base.n,
                 g_bind.base.id[0].tag);
        FallbackToOrdinal(why);
      } else {
        Rebind();
      }
    }
  }
  // Self-check: a bound slot that has not run for a while although G-buffer executions did.
  static uint64_t s_execsAt = 0;
  static int64_t s_checkQpc = 0;
  const int64_t nowQpc = defrec::Qpc();
  if (g_idMode && Active() && nowQpc - s_checkQpc > MsTicks(1000.0)) {
    const uint64_t execs = g_gbExecs.load();
    const bool ran = execs != s_execsAt;
    const int64_t now = nowQpc;
    s_execsAt = execs;
    s_checkQpc = now;
    for (int s = 0; s < g_poolSlots && g_idMode; ++s) {
      const Slot& sl = g_slot[s];
      if (SlotStarved(sl.id.ok, sl.lastPassQpc, sl.boundQpc, now, MsTicks(kStarvedMs), ran)) {
        char why[160];
        snprintf(why, sizeof(why), "slot %d (viewport tag 0x%x, rank %u) saw no execution for %.0f s", s, sl.id.tag,
                 sl.id.rank, kStarvedMs / 1000.0);
        FallbackToOrdinal(why);
      }
    }
  }
  const bool active = Active();
  shrec::TexIdleStep(g_tex, g_texLock, g_texIdle, active);  // R77
  if (active && (!g_gbRg || rg == g_gbRg)) g_activeEntryGen = g_entryGen;
  // Paused (kill switch, bench mode 29, the R30 governor's stage 3): the
  // entry mark still advances, so a job a sort started just before the pause
  // counts as stale at the first entry after the resume (DropStale) instead
  // of reaching its pass, where only the vector check would stop it. The
  // shadow recorder's mark (g_cascEntryQpc) always advanced.
  if (!active && rg == g_gbRg) g_gbEntryQpc = g_entryQpc;
  if (active && rg == g_gbRg) {
    MaintainTables();  // the key, read-set and mesh tables (long sessions)
    const int64_t prevEntry = g_lastPassQpc > g_gbEntryQpc ? g_lastPassQpc : g_gbEntryQpc;
    g_gbEntryQpc = g_entryQpc;
    size_t count = SIZE_MAX;
    for (int s = 0; s < kSlots; ++s) {
      Slot& sl = g_slot[s];
      if (!sl.job || !sl.learn.haveVec || sl.learn.rg != rg) continue;
      Job& j = *sl.job;
      const int st = j.startState.load();
      const bool oldStart = StaleStart(st, j.entryGen, j.tStart, prevEntry);
      if (oldStart) sl.st.staleStart++;
      if (sl.armed && (j.entryGen || oldStart || st == kStartAbort) && !DropStale(s)) {
        sl.st.refused++;
        continue;
      }
      if (!sl.armed && !Arm(s)) continue;
      if (g_ownAsideHint.load(std::memory_order_relaxed)) {  // the ownship's passes are drawn stock: no job
        g_ownAsideJobsSkipped++;
        continue;
      }
      if (count == SIZE_MAX) count = VectorCountGuarded(rg, renderables);
      if (sl.lvol || g_idMode) {  // this frame's index of the slot's execution (other views shift them)
        const int idx = sl.lvol ? CollFindVolGuarded(rg, sl.lsm, sl.lvol, sl.lvrank) : CollFindGuarded(rg, sl.id);
        if (idx < 0) continue;
        sl.learn.idx = static_cast<uint32_t>(idx);
      }
      if (sl.learn.idx >= count) continue;
      // R73: not started by the sort hook and no G-buffer collection sorted
      // since the last frame-heap reset: the vector may name recycled memory.
      // Not started; it stays armed (a later sort may start it, else its start
      // wait expires and it is dropped as stale) and the pass is drawn stock.
      void** const vec = reinterpret_cast<void**>(static_cast<uint8_t*>(renderables) + sl.learn.idx * 24);
      if (j.startState.load(std::memory_order_acquire) == kStartArmed) {
        if (!EntryVectorFresh()) {
          // An empty vector names nothing (the job builds no segment, the
          // pass is stock as before): started as before, counted apart.
          if (!VectorEmptyGuarded(vec)) {
            g_unsortedStarts.fetch_add(1, std::memory_order_relaxed);
            continue;
          }
          g_unsortedEmpty.fetch_add(1, std::memory_order_relaxed);
        }
        g_entryStarts.fetch_add(1, std::memory_order_relaxed);
      }
      StartJob(j, vec, false);
      j.entryGen = g_entryGen;
    }
  }
  g_inside.fetch_sub(1);
}

void OnRender(void* rg, void* renderables) {
  if (shadowbatch::RenderObserverFn p = g_prevObserver) p(rg, renderables);
  const int lvl = t_dcs;
  __try {
    OnRenderBody(rg, renderables);
  } __except (GbFilter(GetExceptionCode(), lvl)) {
    Contained("the render entry", GetExceptionCode(), lvl);
    g_inside.fetch_sub(1);  // the body's own decrement did not run
  }
}

// A collection's vector is final (Scene's sort, pool thread): start the armed
// job watching it.
void OnSortedBody(void** out) {
  if (g_shutdown.load(std::memory_order_relaxed)) return;
  NoteSorted(out);  // R73: every sort (armed or not, on or off), for the render-entry start's check
  if (!g_on.load(std::memory_order_relaxed) || g_disabled.load(std::memory_order_relaxed)) return;
  for (int s = 0; s < kSlots; ++s) {
    Job* j = g_jobPub[s].load(std::memory_order_acquire);
    if (!j || j->startState.load(std::memory_order_acquire) != kStartArmed) continue;
    uint8_t* base = shrec::ArrayBaseGuarded(j->armRg);
    if (!base) continue;
    if (j->armVol) {
      const intptr_t d = reinterpret_cast<uint8_t*>(out) - base;
      if (d >= 0 && d % 24 == 0 && d / 24 <= 0xffff &&
          CollIsVolGuarded(j->armRg, static_cast<uint32_t>(d / 24), j->armSm, j->armVol, j->armVrank))
        StartJob(*j, out, true);
    } else if (j->armById) {
      const intptr_t d = reinterpret_cast<uint8_t*>(out) - base;
      if (d >= 0 && d % 24 == 0 && d / 24 <= 0xffff &&
          CollIsGuarded(j->armRg, static_cast<uint32_t>(d / 24), j->armKey))
        StartJob(*j, out, true);
    } else if (reinterpret_cast<uint8_t*>(out) == base + static_cast<size_t>(j->armIdx) * 24) {
      StartJob(*j, out, true);
    }
  }
}

void OnSorted(void** out) {
  const int lvl = t_dcs;
  __try {
    OnSortedBody(out);
  } __except (GbFilter(GetExceptionCode(), lvl)) {
    Contained("the sort observer", GetExceptionCode(), lvl);
  }
}

// ---------------------------------------------------------------------------
// Install / teardown
// ---------------------------------------------------------------------------
bool Compiled(void* shader, uint64_t tech) {
  const shadowinst::MapEntry* e = shadowinst::FindEntry(shader, tech, 0);
  return e && e->result && e->kind == shadowinst::kKindGb;
}
bool CbUsed(void* shader, uint64_t tech, uint64_t out[2]) { return shadowinst::CbUsedDwords(shader, tech, 0, out); }
int BlendOf(uint8_t* shader) { return gbreccount::ReadBlendRaw(shader); }

// Slot s's job and caster-list buffer, allocated on first use (the
// installing thread, or the render thread when the scope grows).
bool EnsureSlot(int s) {
  Slot& sl = g_slot[s];
  if (!sl.stockList) {
    sl.stockList = static_cast<void**>(
        VirtualAlloc(nullptr, (kMaxItems + 2) * sizeof(void*), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (!sl.stockList) return false;
  }
  if (!sl.job) {
    if (!(sl.job = NewJob())) return false;
    g_jobPub[s].store(sl.job, std::memory_order_release);
  }
  return true;
}

bool EnsureSlots() {
  const uint32_t scope = g_scope.load();
  for (int s = 0; s < kSlots; ++s)
    if (OrdinalOfSlot(scope, s) >= 0 && !EnsureSlot(s)) return false;
  return true;
}

// Slots with workers and workers per slot: 4 x 3, or 8 x 2 with the cockpit
// slots (the pool has at most defrec::Pool::kMaxWorkers = 16; R24 crash).
void PoolShape(bool cockpitSlots, int* slots, int* threads) {
  *slots = cockpitSlots ? kSlots : kScopeSlots;
  *threads = kThreads;
  while (*slots * *threads > defrec::Pool::kMaxWorkers && *threads > 1) --*threads;
}

// GraphicsCore's collection descriptors as R24 2 read them [V 2.9.30]; nullptr
// or why not (then executions are taken by plain ordinal).
const char* CollBuildWhy() {
  static const struct {
    uint32_t begin, end;
    uint64_t hash;
  } kCode[] = {
      {0x56a74, 0x56aee, 0x2a1c1d647e5002e5ull},  // node and descriptor pushed together, word +0x38 = index
      {0x40a30, 0x40a52, 0x87c79dd0df9dd11bull},  // descriptor +0: the shading model
      {0x40aea, 0x40af6, 0x0b9b7cd978e36a57ull},  // descriptor +0x690: the viewport tag
      {0x50fa4, 0x5101d, 0x3e6a336882e78f05ull},  // node k, descriptor k (0x6d8 bytes) and vector k read together
  };
  auto* gc = reinterpret_cast<uint8_t*>(GetModuleHandleW(L"GraphicsCore.dll"));
  if (!gc) return "GraphicsCore.dll not loaded";
  for (const auto& c : kCode)
    if (!shadowtex::CodeIs(gc, c.begin, c.end, c.hash)) return "GraphicsCore's collection descriptors differ from R24's";
  return nullptr;
}

// Installed on first use; false = not now (retried every second by main.cpp)
// or unavailable (g_state -1, logged once).
bool Install() {
  if (g_state.load() != 0) return g_state.load() > 0;
  if (gbreccount::g_chained.load()) return false;  // the S0 counter phase owns the chain right now
  if (shrec::g_sortBorrowed.load()) return false;
  if (shadowinst::g_state.load() != 1 || !shadowinst::g_gbInstalled.load() || !shadowinst::g_device) return false;
  auto fail = [](const char* why) {
    Log("gbuffer recorder: %s; unavailable", why);
    g_state = -1;
    // Latched off and Shutdown frees nothing unless ready: free what this
    // attempt allocated (jobs ~20 MB each, stock lists, tables) and its
    // context/device references. No worker runs here (pool stopped or not
    // started), and nothing is chained yet.
    for (auto& p : g_jobPub) p.store(nullptr);
    for (Slot& sl : g_slot) {
      FreeJob(sl.job);
      if (sl.stockList) VirtualFree(sl.stockList, 0, MEM_RELEASE);
      sl.stockList = nullptr;
    }
    FreeTables(g_tab);
    FreeTables(g_tabOld);
    g_plan = shrec::CompactPlan();
    FreeTex(g_tex);
    SafeRel(g_ctx);
    SafeRel(g_dev);
    return false;
  };
  if (!gbpass::Install()) {
    if (gbpass::g_state < 0) return fail("G-buffer pass execute not hooked (GraphicsCore build)");
    return false;
  }
  if (!shadowbatch::InstallRenderer()) {
    if (shadowbatch::g_rendererState < 0) return fail("DX11Renderer not matched");
    return false;
  }
  auto* ng = reinterpret_cast<uint8_t*>(GetModuleHandleW(L"NGModel.dll"));
  auto* dx = reinterpret_cast<uint8_t*>(GetModuleHandleW(L"dx11backend.dll"));
  auto* md = reinterpret_cast<uint8_t*>(GetModuleHandleW(L"ModelDesc.dll"));
  if (!ng || !dx || !md) return false;
  if (const char* why = gbbatch::VerifyBuild(ng, dx, md)) return fail(why);
  auto** rv = shadowbatch::g_rendererVtbl;
  if (reinterpret_cast<uint8_t*>(SlotOriginal(&rv[54])) != dx + 0x15d60 ||
      reinterpret_cast<uint8_t*>(SlotOriginal(&rv[40])) != dx + 0x17010)
    return fail("DX11Renderer frame-buffer/viewport getters do not match this build");
  ID3D11Device* dev = shadowinst::g_device;
  if (shadowinst::g_deviceSingleThreaded) return fail("the D3D11 device is single-threaded");
  const defrec::Caps caps = defrec::QueryCaps(dev);
  if (!caps.commandLists || !caps.cbOffsetting || !caps.device1)
    return fail(defrec::DescribeCapsFailure(dev, caps));
  if (!gbverify::Install()) return fail("the G-buffer verifier could not start");
  if (!gbbatch::g_ctx) dev->GetImmediateContext(&gbbatch::g_ctx);  // the verifier's context (released by gb_batch)
  if (!AllocTables(g_tab) || !AllocTex(g_tex, 1u << 14)) return fail("no memory for the tables");
  if (!EnsureSlots()) return fail("no memory for the jobs");
  g_env.srVt = ng + gbcount::kVtableRva;
  g_env.modelVt = gbcount::g_modelMatVtbl;
  g_env.shaderVt = dx + shadowtex::g_at.shader;
  g_env.globals = reinterpret_cast<uint8_t* const*>(ng + shadowtex::g_at.globals);
  for (int c = 0; c < gbbatch::kPropClasses; ++c) {
    g_env.propVt[c] = md + gbbatch::kProps[c].vtbl;
    g_env.propBytes[c] = gbbatch::kProps[c].bytes;
  }
  g_env.setResource = dx + gbbatch::kSetResource;
  g_env.evalAnim = reinterpret_cast<EvalAnimFn>(ng + gbbatch::kEvalAnim);
  g_env.compiled = &Compiled;
  g_env.cbUsed = &CbUsed;
  g_env.blend = &BlendOf;
  g_env.tex.texVtbl = dx + shadowtex::g_at.tex;
  g_env.tex.inner[0] = dx + shadowtex::g_at.innerFile;
  g_env.tex.inner[1] = dx + shadowtex::g_at.innerArray;
  g_env.tex.inner[2] = dx + shadowtex::g_at.innerDummy;
  if (!shrec::InitInnerRt(dx)) Log("gbuffer recorder: render-target textures not supported (dx11backend's class not as analysed)");
  g_env.tex.getDesc = reinterpret_cast<gbbatch::GetDescFn>(dx + shadowtex::g_at.getDesc);
  g_env.tex.compat = reinterpret_cast<gbbatch::CompatFn>(dx + shadowtex::g_at.compat);
  for (auto*& p : g_frontVtbl) p = reinterpret_cast<void*>(&LoopNop);
  for (auto*& p : g_execVtbl) p = reinterpret_cast<void*>(&LoopNop);
  g_frontVtbl[1] = reinterpret_cast<void*>(&FrontVt1);
  g_execVtbl[1] = reinterpret_cast<void*>(&ExecVt1);
  for (int s = 0; s < kSlots; ++s) {
    g_slot[s].front = {g_frontVtbl, s, -1};
    for (int k = 0; k < kMaxSegments; ++k) g_slot[s].exec[k] = {g_execVtbl, s, k};
  }
  // Execution identity (R24 2): the collection descriptors as analysed, else plain ordinals.
  const char* collWhy = CollBuildWhy();
  g_collOk = !collWhy;
  const char* idWhy = g_byOrdinal.load() ? "[Model] GBufferRecorderByOrdinal=1" : collWhy;
  g_idMode = !idWhy;
  PoolShape(g_cockpit.load() && g_idMode, &g_poolSlots, &g_threads);
  if (g_cockpit.load() && !g_idMode) {
    Log("gbuffer recorder: cockpit executions need the execution identity (%s); cockpit items are recorded only in "
        "scoped executions", idWhy);
  }
  defrec::PoolConfig pc;
  pc.workers = g_poolSlots * g_threads;
  pc.cb.mode = defrec::CbMode::kOffsets;
  pc.priority = THREAD_PRIORITY_NORMAL;
  pc.name = "gbuffer recorder";
  if (!g_pool.Start(dev, pc)) return fail("its workers did not start");
  if (g_pool.Workers() < g_poolSlots * g_threads) {
    g_pool.Stop();
    return fail("fewer workers than slots x threads");
  }
  if (g_pool.At(0).ring.Mode() != defrec::CbMode::kOffsets) {
    g_pool.Stop();
    return fail("constant-buffer offsets unavailable on the worker contexts");
  }
  dev->GetImmediateContext(&g_ctx);
  g_dev = dev;
  g_dev->AddRef();
  // RenderGraph::render entry: shadow batching's import hook, ours if absent.
  g_ownRenderHook = false;
  if (!shadowbatch::g_renderSlot && !shadowbatch::g_renderSlot2) g_ownRenderHook = shadowbatch::InstallRenderHook();
  if (!shadowbatch::g_renderSlot && !shadowbatch::g_renderSlot2) {
    g_pool.Stop();
    return fail("RenderGraph::render entry not hooked");
  }
  // Jobs start when their vector is final (Scene's sort), else at render entry.
  const bool wasPatched = shrec::g_sortPatched;
  const bool early = shrec::EnsureSortHook();
  g_ownSort = early && !wasPatched;
  shrec::g_sortObserver2 = &OnSorted;
  g_renderTid = shadowbatch::g_renderThread ? shadowbatch::g_renderThread : ptiming::g_topTid.load();
  g_prevWrap = gbpass::g_wrap.load();
  g_prevOverride = gbcount::g_override.load();
  g_prevObserver = shadowbatch::g_renderObserver.load();
  gbpass::g_wrap = &Wrap;
  gbcount::g_override = &Override;
  shadowbatch::g_renderObserver = &OnRender;
  g_chained = true;
  g_state = 1;
  Log("gbuffer recorder: ready (scope 0x%x: executions by ordinal bits 0-15, A2C bit 16; %d workers; DCS's G-buffer "
      "loop runs over [front, residual items, one exec entry per segment]; island %u, at most %d segments; wait "
      "budget %u us; jobs start %s)",
      g_scope.load(), g_poolSlots * g_threads, g_island.load(), static_cast<int>(g_maxSeg.load()), g_waitUs.load(),
      early ? "when Scene's sort finishes their vector" : "at RenderGraph::render entry");
  Log("gbuffer recorder: executions %s; cockpit %s",
      g_idMode ? "bound by identity (shading model, viewport tag, rank) from a base frame's ordinals"
               : "by plain ordinal",
      g_cockpit.load() ? (g_poolSlots > kScopeSlots ? "on (items and the scoped views' cockpit executions)"
                                                    : "on (items only)")
                       : "off");
  PollStateDump();
  return true;
}

bool Ready() { return g_state.load() == 1 && !g_disabled.load(); }

// True when its objects were released (false: left alive, still in use).
bool Shutdown() {
  g_shutdown = true;
  g_on = false;
  if (g_chained.exchange(false)) {
    gbpass::WrapFn w = &Wrap;
    gbpass::g_wrap.compare_exchange_strong(w, g_prevWrap);
    gbcount::OverrideFn o = &Override;
    gbcount::g_override.compare_exchange_strong(o, g_prevOverride);
    shadowbatch::RenderObserverFn ob = &OnRender;
    shadowbatch::g_renderObserver.compare_exchange_strong(ob, g_prevObserver);
    shrec::SortObserverFn so = &OnSorted;
    shrec::g_sortObserver2.compare_exchange_strong(so, nullptr);
  }
  if (g_state.load() != 1) return true;
  for (int i = 0; i < 400 && (g_inside.load() != 0 || shrec::g_sortInside.load() != 0); ++i) Sleep(5);
  for (Slot& sl : g_slot)
    if (sl.job) {
      int st = kStartArmed;
      sl.job->startState.compare_exchange_strong(st, kStartAbort);
      SetEvent(sl.job->start);
      ReleaseHelpers(*sl.job);
    }
  // A pass still inside after the wait may be in TakeList/Wait on the pool:
  // stopping it then (Teardown releases lists and contexts) raced the render
  // thread (double Release). Leave everything alive instead.
  const bool stopped = g_inside.load() == 0 && g_pool.Stop();
  if (g_ownSort && shrec::g_state.load() != 1) {
    if (!shrec::PatchSortSites(false)) Log("gbuffer recorder: WARNING could not restore Scene's sort call sites");
    if (shrec::g_sortStub)
      reinterpret_cast<std::atomic<void*>*>(shrec::g_sortStub + 16)->store(reinterpret_cast<void*>(shrec::g_sortOrig));
  }
  g_ownSort = false;
  if (g_ownRenderHook && shadowbatch::g_state.load() != 1) {
    if (shadowbatch::g_renderSlot)
      UnhookSlot(shadowbatch::g_renderSlot, reinterpret_cast<void*>(shadowbatch::g_origRender));
    if (shadowbatch::g_renderSlot2)
      UnhookSlot(shadowbatch::g_renderSlot2, reinterpret_cast<void*>(shadowbatch::g_origRender));
    shadowbatch::g_renderSlot = nullptr;
    shadowbatch::g_renderSlot2 = nullptr;
  }
  g_ownRenderHook = false;
  if (!stopped || g_inside.load() != 0) {
    Log("gbuffer recorder: a pass or a worker was still running at unload; its objects are left alive");
    return false;
  }
  for (auto& p : g_jobPub) p.store(nullptr);
  for (Slot& sl : g_slot) {
    FreeJob(sl.job);
    for (auto*& p : sl.learn.rtv) SafeRel(p);
    SafeRel(sl.learn.dsv);
    for (auto& row : sl.learn.pool)
      for (auto*& p : row) SafeRel(p);
    sl.learn = Learned();
    for (auto& row : sl.ours)
      for (auto*& p : row) SafeRel(p);
    memset(sl.oursBytes, 0, sizeof(sl.oursBytes));
    if (sl.stockList) VirtualFree(sl.stockList, 0, MEM_RELEASE);
    sl.stockList = nullptr;
  }
  FreeTables(g_tab);
  FreeTables(g_tabOld);  // no job runs: nothing reads the replaced tables either
  g_plan = shrec::CompactPlan();
  FreeTex(g_tex);
  PollStateDump();  // the queued lines
  free(g_dumpSeen);
  g_dumpSeen = nullptr;
  g_dumpSeenCount = 0;
  SafeRel(g_ctx);
  SafeRel(g_dev);
  g_state = -1;
  return true;
}

void ResetCounters() {
  for (Slot& sl : g_slot) {
    Stat& s = sl.st;
    s.passes = s.partial = s.segs = s.items = s.recorded = s.draws = s.jobs = s.jobsUsed = s.refused = 0;
    for (auto& a : s.passReason) a = 0;
    for (auto& a : s.segReason) a = 0;
    for (auto& a : s.itemReason) a = 0;
    s.cbBytes = s.vt23 = s.segPlanned = 0;
    s.buildNs = s.recordNs = s.waitNs = s.waitMaxNs = s.frontNs = s.execNs = s.executeNs = s.restoreNs = s.maintNs = 0;
    s.tlSamples = s.early = s.recWorkers = s.pieces = s.helpersQueued = s.helperItems = s.jobItems = 0;
    s.preNs = s.commitNs = s.recMaxNs = 0;
    s.tlPass = s.tlStart = s.tlDone = s.tlPre = s.tlBuilt = s.tlRec = 0;
    s.lateSamples = 0;
    s.latePass = s.lateStart = s.lateBuilt = s.lateDone = 0;
    s.staleStart = s.misses = s.refreshes = 0;
    s.aaClean = 0;
    s.redoPlanned = s.redoExecuted = s.redoLate = s.redoFail = s.swapsAhead = s.redoNs = s.slackJobs = 0;
    s.rtReplays = s.rtSeen = s.rtDrawn = s.rtBound = 0;
    s.idOtherVec = s.idSameVec = 0;
    s.verifyAa = s.verifyAb = s.verifyAaBad = s.verifyAbBad = s.verifyTexels = s.verifyBadTexels = 0;
    s.verifySkipped = s.verifyErrors = s.verifyStateBad = s.verifyExecs = 0;
  }
  g_probes = g_probeOdd = g_probePool = g_probeWants = 0;
  for (auto& a : g_probeSkip) a = 0;
  g_entries = g_gbExecs = g_ordMax = g_nonQuad = 0;
  g_texBuilt = g_texRefreshed = g_texFull = g_texDeferred = 0;
  memset(g_texWhy, 0, sizeof(g_texWhy));
}

// R30 L7: the 10-s block as one line (the full block: [Log] RecorderDetail=1).
// Summed over the slots in scope: executions fully/partly drawn from command
// lists and why the others were stock, jobs refused, items drawn from lists,
// the render thread's wait and replay/execute time, CB ring bytes; and the
// execution layout (quad views known, highest ordinal).
double HeapResetsPerFrame(double f) {
  static uint64_t last = 0;
  const uint64_t g = g_frameHeapGen.load();
  const double r = last ? (g - last) / f : 0.0;
  last = g;
  return r;
}

void LogSummary(const char* label, double frames) {
  const double f = frames > 0 ? frames : 1.0;
  const uint32_t scope = g_scope.load();
  uint64_t passes = 0, exec = 0, partial = 0, late = 0, refused = 0, items = 0, draws = 0;
  uint64_t waitNs = 0, waitMax = 0, execNs = 0, cb = 0;
  int slots = 0;
  for (int s = 0; s < kSlots; ++s) {
    if (OrdinalOfSlot(scope, s % kScopeSlots) < 0) continue;
    const Stat& st = g_slot[s].st;
    if (!st.passes.load()) continue;
    ++slots;
    passes += st.passes.load();
    exec += st.passReason[kPExecuted].load();
    partial += st.partial.load();
    late += st.passReason[kPLate].load();
    refused += st.refused.load();
    items += st.items.load();
    draws += st.draws.load();
    waitNs += st.waitNs.load();
    waitMax = (std::max)(waitMax, st.waitMaxNs.load());
    execNs += st.execNs.load();
    cb += st.cbBytes.load();
  }
  const uint64_t stock = passes > exec + partial + late ? passes - exec - partial - late : 0;
  Log("  gbuffer recorder %s: %.0f frames, %d executions recorded (helpers %u)%s; fully executed %llu of %llu "
      "(%.1f%%), partly %llu, late %llu, other stock %llu; jobs refused %llu; items from lists %.0f/frame of %.0f "
      "(%.1f%%); render thread wait %.3f ms/frame (max %.3f), replay+execute %.3f ms/frame; CB ring %.2f MB/frame; "
      "%.2f G-buffer executions/frame, highest ordinal %llu, quad views %d known; frame heap %.2f resets/frame, jobs "
      "stopped on a recycled heap %llu (faults contained %llu, %llu of them in re-records), used after a reset %llu; "
      "tables keys %u meshes %u, "
      "meshes relearnt %llu, compactions %llu; render-entry starts %llu (%llu of them on an empty "
      "unsorted vector), skipped (no G-buffer collection sorted since the last heap reset) %llu, implausible "
      "items %llu, "
      "G-buffer collection sorts %.1f/frame",
      label, frames, slots, g_helpers.load(), g_disabled.load() ? ", DISABLED" : "",
      static_cast<unsigned long long>(exec), static_cast<unsigned long long>(passes),
      passes ? 100.0 * exec / passes : 0.0, static_cast<unsigned long long>(partial),
      static_cast<unsigned long long>(late), static_cast<unsigned long long>(stock),
      static_cast<unsigned long long>(refused), draws / f, items / f, items ? 100.0 * draws / items : 0.0,
      waitNs / 1e6 / f, waitMax / 1e6, execNs / 1e6 / f, cb / 1048576.0 / f, g_gbExecs.load() / f,
      static_cast<unsigned long long>(g_ordMax.load()), g_frameQuad.n, HeapResetsPerFrame(f),
      static_cast<unsigned long long>(g_recycledJobs.exchange(0)),
      static_cast<unsigned long long>(g_recycledFaults.exchange(0)),
      static_cast<unsigned long long>(g_redoFaultsContained.exchange(0)),
      static_cast<unsigned long long>(g_recycledAtPass.exchange(0)), g_tab.keysUsed, g_tab.mesh.meshesUsed,
      static_cast<unsigned long long>(g_meshRelearnt.load()), static_cast<unsigned long long>(g_compactions.load()),
      static_cast<unsigned long long>(g_entryStarts.exchange(0)),
      static_cast<unsigned long long>(g_unsortedEmpty.exchange(0)),
      static_cast<unsigned long long>(g_unsortedStarts.exchange(0)),
      static_cast<unsigned long long>(g_implausible.exchange(0)), g_sortsNoted.exchange(0) / f);
}

void LogCounters(const char* label, double frames) {
  PollStateDump();
  const double f = frames > 0 ? frames : 1.0;
  const uint32_t scope = g_scope.load();
  Log("  gbuffer recorder %s: scope 0x%x, %.0f frames%s; %.2f render entries and %.2f G-buffer executions/frame "
      "(highest ordinal %llu; other views' executions %.2f/frame, quad views %d known); probes %llu (unusable %llu, inconclusive %llu, sampler pool differs %llu); keys %u (recordable %llu, not %llu, "
      "retired for a state change %llu, relearnt %llu), meshes %u (recordable %llu, not %llu), read sets %u, table full %llu; faults %llu",
      label, scope, frames, g_disabled.load() ? ", DISABLED" : "", g_entries.load() / f, g_gbExecs.load() / f,
      static_cast<unsigned long long>(g_ordMax.load()), g_nonQuad.load() / f, g_frameQuad.n,
      static_cast<unsigned long long>(g_probes.load()),
      static_cast<unsigned long long>(g_probeOdd.load()), static_cast<unsigned long long>(g_probeInconclusive.load()),
      static_cast<unsigned long long>(g_probePool.load()), g_tab.keysUsed, static_cast<unsigned long long>(g_keysOk.load()),
      static_cast<unsigned long long>(g_keysRejected.load()), static_cast<unsigned long long>(g_keyDowngrades.load()),
      static_cast<unsigned long long>(g_keyRelearnt.load()),
      g_tab.mesh.meshesUsed, static_cast<unsigned long long>(g_meshesOk.load()),
      static_cast<unsigned long long>(g_meshesRejected.load()), g_tab.readsUsed,
      static_cast<unsigned long long>(g_tableFull.load()), static_cast<unsigned long long>(g_faults.load()));
  Log("  gbuffer recorder %s tables: keys %u of %zu, read sets %u of %zu, meshes %u of %zu; meshes relearnt at a "
      "reused address %llu; compactions %llu (replaced tables freed %llu%s)%s%s",
      label, g_tab.keysUsed, kKeyTable, g_tab.readsUsed, kReadsTable, g_tab.mesh.meshesUsed, shrec::kMeshTable,
      static_cast<unsigned long long>(g_meshRelearnt.load()), static_cast<unsigned long long>(g_compactions.load()),
      static_cast<unsigned long long>(g_oldFreed.load()), g_tabOld.keys ? ", one set still in use" : "",
      g_compactions.load() ? "; last: " : "", g_compactions.load() ? shrec::CompactText(g_lastCompact).c_str() : "");
  Log("  gbuffer recorder %s texture table: %u entries live, %.0f built/frame, %.0f rebuilt/frame, %llu evicted, %llu "
      "wipes, %llu table full, %llu maintenances deferred (lock busy); built as: usable %llu, class %llu, 0x678 %llu, "
      "mip-set %llu, swap due %llu, fault %llu",
      label, g_tex.live, g_texBuilt.load() / f, g_texRefreshed.load() / f,
      static_cast<unsigned long long>(g_tex.evicted), static_cast<unsigned long long>(g_tex.wipes),
      static_cast<unsigned long long>(g_texFull.load()), static_cast<unsigned long long>(g_texDeferred.load()),
      static_cast<unsigned long long>(g_texWhy[kTwOk]), static_cast<unsigned long long>(g_texWhy[kTwClass]),
      static_cast<unsigned long long>(g_texWhy[kTw678]), static_cast<unsigned long long>(g_texWhy[kTwMipSet]),
      static_cast<unsigned long long>(g_texWhy[kTwSwap]), static_cast<unsigned long long>(g_texWhy[kTwFault]));
  for (int s = 0; s < kSlots; ++s) {
    const int ord = OrdinalOfSlot(scope, s % kScopeSlots);
    if (ord < 0 || (s >= kScopeSlots && !g_slot[s].id.ok && !g_slot[s].st.passes.load())) continue;
    char name[48];
    SlotName(s, name, sizeof(name));
    if (g_idMode)
      Log("  gbuffer recorder %s %s: slot %d, bound to %s, viewport tag 0x%x, rank %u", label, name, s,
          g_slot[s].id.ok ? SmName(g_slot[s].id.sm) : "nothing", g_slot[s].id.tag, g_slot[s].id.rank);
    const Stat& st = g_slot[s].st;
    const uint64_t passes = st.passes.load(), exec = st.passReason[kPExecuted].load(), used = st.jobsUsed.load();
    auto ms = [&](const std::atomic<uint64_t>& ns) { return ns.load() / 1e6 / f; };
    Log("  gbuffer recorder %s %s: %.2f passes/frame, %llu of %llu fully executed, %llu partly; items in "
        "the jobs used %.0f/frame, drawn from command lists %.0f/frame (%.1f%%); segments %.2f per used job, %llu "
        "executed of %llu; %llu jobs, %llu used, %llu refused (worker busy)",
        label, name, passes / f, static_cast<unsigned long long>(exec), static_cast<unsigned long long>(passes),
        static_cast<unsigned long long>(st.partial.load()), st.items.load() / f, st.draws.load() / f,
        st.items.load() ? 100.0 * st.draws.load() / st.items.load() : 0.0,
        used ? static_cast<double>(st.segPlanned.load()) / used : 0.0,
        static_cast<unsigned long long>(st.segReason[kSExecuted].load()), static_cast<unsigned long long>(st.segs.load()),
        static_cast<unsigned long long>(st.jobs.load()), static_cast<unsigned long long>(used),
        static_cast<unsigned long long>(st.refused.load()));
    const double tl = st.tlSamples.load() ? 1e6 * st.tlSamples.load() : 1.0;
    const double u = used ? static_cast<double>(used) : 1.0;
    Log("  gbuffer recorder %s %s times (ms/frame): workers build %.3f + record %.3f (per job: stage A %.3f "
        "= reads %.3f + commit %.3f; record %.3f summed, %.3f on the slowest worker); render thread: wait %.3f (max "
        "%.3f ms), front %.3f, replay + checks + copies + execute %.3f (execute %.3f), restore %.3f, table upkeep %.3f; "
        "CB ring %.2f MB/frame; streaming replays %.0f/frame; texture misses %.0f/frame, unusable %.0f/frame",
        label, name, ms(st.buildNs), ms(st.recordNs), st.buildNs.load() / 1e6 / u, st.preNs.load() / 1e6 / u,
        st.commitNs.load() / 1e6 / u, st.recordNs.load() / 1e6 / u, st.recMaxNs.load() / 1e6 / u, ms(st.waitNs),
        st.waitMaxNs.load() / 1e6, ms(st.frontNs), ms(st.execNs), ms(st.executeNs), ms(st.restoreNs), ms(st.maintNs),
        st.cbBytes.load() / 1048576.0 / f, st.vt23.load() / f, st.misses.load() / f, st.refreshes.load() / f);
    Log("  gbuffer recorder %s %s timeline from RenderGraph::render entry (ms, mean per used job, negative = "
        "before): vector final / job start %.3f (%llu of %llu by Scene's sort, the rest at entry), stage-A reads done "
        "%.3f, commit done %.3f, recording done %.3f, job done %.3f, pass %.3f; %.0f items per job, %.1f%% read by "
        "helpers (%.2f helpers queued), %.2f recording workers and %.2f lists per job; stale early starts %llu",
        label, name, st.tlStart.load() / tl, static_cast<unsigned long long>(st.early.load()),
        static_cast<unsigned long long>(st.tlSamples.load()), st.tlPre.load() / tl, st.tlBuilt.load() / tl,
        st.tlRec.load() / tl, st.tlDone.load() / tl, st.tlPass.load() / tl, st.jobItems.load() / u,
        st.jobItems.load() ? 100.0 * st.helperItems.load() / st.jobItems.load() : 0.0, st.helpersQueued.load() / u,
        st.recWorkers.load() / u, st.pieces.load() / u, static_cast<unsigned long long>(st.staleStart.load()));
    std::string line;
    char buf[220];
    for (int r = 1; r < kPassReasons; ++r) {
      if (!st.passReason[r].load()) continue;
      snprintf(buf, sizeof(buf), "%s%s %llu", line.empty() ? "" : "; ", kPassReasonName[r],
               static_cast<unsigned long long>(st.passReason[r].load()));
      line += buf;
    }
    Log("  gbuffer recorder %s %s drawn stock: %s", label, name, line.empty() ? "none" : line.c_str());
    if (st.idOtherVec.load() || st.idSameVec.load())
      Log("  gbuffer recorder %s %s item vector changed: %llu times another collection's vector, %llu times the same "
          "vector with other items",
          label, name, static_cast<unsigned long long>(st.idOtherVec.load()),
          static_cast<unsigned long long>(st.idSameVec.load()));
    line.clear();
    for (int r = 1; r < kSegReasons; ++r) {
      if (!st.segReason[r].load()) continue;
      snprintf(buf, sizeof(buf), "%s%s %llu", line.empty() ? "" : "; ", kSegReasonName[r],
               static_cast<unsigned long long>(st.segReason[r].load()));
      line += buf;
    }
    Log("  gbuffer recorder %s %s segments drawn stock: %s", label, name, line.empty() ? "none" : line.c_str());
    line.clear();
    for (int r = 1; r < kReasons; ++r) {
      if (!st.itemReason[r].load()) continue;
      snprintf(buf, sizeof(buf), "%s%s %.1f", line.empty() ? "" : "; ", kReasonName[r], st.itemReason[r].load() / f);
      line += buf;
    }
    Log("  gbuffer recorder %s %s residual items/frame (DCS draws them): %s", label, name,
        line.empty() ? "none" : line.c_str());
    if (st.verifyAa.load() || st.verifyAb.load() || st.verifySkipped.load() || g_verify.load())
      Log("  gbuffer recorder %s %s verify: stock vs stock %llu (%llu differ), stock vs recorded %llu (%llu "
          "differ), %llu of %llu texels/samples differ; %llu not compared, %llu errors; state after Execute differs "
          "%llu of %llu",
          label, name, static_cast<unsigned long long>(st.verifyAa.load()),
          static_cast<unsigned long long>(st.verifyAaBad.load()), static_cast<unsigned long long>(st.verifyAb.load()),
          static_cast<unsigned long long>(st.verifyAbBad.load()),
          static_cast<unsigned long long>(st.verifyBadTexels.load()),
          static_cast<unsigned long long>(st.verifyTexels.load()),
          static_cast<unsigned long long>(st.verifySkipped.load()),
          static_cast<unsigned long long>(st.verifyErrors.load()),
          static_cast<unsigned long long>(st.verifyStateBad.load()),
          static_cast<unsigned long long>(st.verifyExecs.load()));
  }
  if (!gbverify::g_lastFormats.empty() && (g_verify.load() || g_slot[0].st.verifyAb.load()))
    Log("  gbuffer recorder %s verify: last captured targets: %s", label, gbverify::g_lastFormats.c_str());
  if (g_verifyWhy) Log("  gbuffer recorder %s verify: last capture failure: %s", label, g_verifyWhy);
  if (g_ownMaskRuns.load())
    Log("  gbuffer recorder %s verify, ownship stage 2: mask runs %llu (failed %llu), compares classified %llu; "
        "differing texels in the ownship's texels %llu, elsewhere %llu",
        label, static_cast<unsigned long long>(g_ownMaskRuns.load()),
        static_cast<unsigned long long>(g_ownMaskFailed.load()),
        static_cast<unsigned long long>(g_ownMaskCompares.load()), static_cast<unsigned long long>(g_ownMaskIn.load()),
        static_cast<unsigned long long>(g_ownMaskOut.load()));
  uint64_t rt = 0, rtSeen = 0, rtDrawn = 0, rtBound = 0;
  for (int k = 0; k < kSlots; ++k) {
    rt += g_slot[k].st.rtReplays.load();
    rtSeen += g_slot[k].st.rtSeen.load();
    rtDrawn += g_slot[k].st.rtDrawn.load();
    rtBound += g_slot[k].st.rtBound.load();
  }
  {
    std::string sk;
    char b[96];
    for (int r = 0; r < kSkips; ++r) {
      if (!g_probeSkip[r].load()) continue;
      snprintf(b, sizeof(b), "%s%s %.1f", sk.empty() ? "" : "; ", kSkipName[r], g_probeSkip[r].load() / f);
      sk += b;
    }
    Log("  gbuffer recorder %s probes: requests %.1f/frame in the jobs used, probed %.2f/frame%s; not probed/frame: "
        "%s",
        label, g_probeWants.load() / f, g_probes.load() / f,
        g_probeWants.load() && !g_probes.load() ? " (STALLED: requests but no probe)" : "", sk.empty() ? "none" : sk.c_str());
  }
  if (g_idMode)
    Log("  gbuffer recorder %s identity: %llu frames, %llu base, %llu with more executions (MFD frames); last base "
        "frame %u non-cockpit and %u cockpit executions; %llu bindings changed; %llu executions not identified; "
        "render graph changes %llu",
        label, static_cast<unsigned long long>(g_bind.frames), static_cast<unsigned long long>(g_bind.baseFrames),
        static_cast<unsigned long long>(g_bind.extraFrames), g_bind.base.n, g_bind.base.nck,
        static_cast<unsigned long long>(g_bindChanges.load()), static_cast<unsigned long long>(g_idFail.load()),
        static_cast<unsigned long long>(g_graphChanges.load()));
  if (g_cockpit.load())
    Log("  gbuffer recorder %s cockpit: render-target textures at exec entries %.2f/frame, drawn to since their last "
        "mip generation %.2f/frame, a target of the pass %.2f/frame",
        label, rtSeen / f, rtDrawn / f, rtBound / f);
  Log("  gbuffer recorder %s: render-target mip replays %.2f/frame (%s); texture classes the table refused (since "
      "start): %s",
      label, rt / f, shrec::g_innerRt ? "class supported" : "class not supported", shrec::CensusText(g_census).c_str());
}


// ---------------------------------------------------------------------------
// Drawn-stock reasons between two snapshots (view profile lines)
// ---------------------------------------------------------------------------
struct StockSnap {
  uint64_t passes = 0;
  uint64_t pass[kPassReasons] = {};
  uint64_t seg[kSegReasons] = {};
  uint64_t item[kReasons] = {};
  uint64_t late[kSlots] = {};
  uint64_t lateN[kSlots] = {};
  int64_t latePass[kSlots] = {}, lateStart[kSlots] = {}, lateBuilt[kSlots] = {}, lateDone[kSlots] = {};
  uint64_t tlN[kSlots] = {};
  int64_t tlPass[kSlots] = {}, tlDone[kSlots] = {};
  uint64_t redoPlanned = 0, redoExecuted = 0, redoLate = 0, redoFail = 0, swapsAhead = 0, redoNs = 0, slackJobs = 0;
  shrec::LoadSnap load;
};

StockSnap TakeStock() {
  StockSnap s;
  const auto rl = std::memory_order_relaxed;
  for (int k = 0; k < kSlots; ++k) {
    const Stat& st = g_slot[k].st;
    s.passes += st.passes.load(rl);
    for (int r = 0; r < kPassReasons; ++r) s.pass[r] += st.passReason[r].load(rl);
    for (int r = 0; r < kSegReasons; ++r) s.seg[r] += st.segReason[r].load(rl);
    for (int r = 0; r < kReasons; ++r) s.item[r] += st.itemReason[r].load(rl);
    s.late[k] = st.passReason[kPLate].load(rl);
    s.lateN[k] = st.lateSamples.load(rl);
    s.latePass[k] = st.latePass.load(rl);
    s.lateStart[k] = st.lateStart.load(rl);
    s.lateBuilt[k] = st.lateBuilt.load(rl);
    s.lateDone[k] = st.lateDone.load(rl);
    s.tlN[k] = st.tlSamples.load(rl);
    s.tlPass[k] = st.tlPass.load(rl);
    s.tlDone[k] = st.tlDone.load(rl);
    s.redoPlanned += st.redoPlanned.load(rl);
    s.redoExecuted += st.redoExecuted.load(rl);
    s.redoLate += st.redoLate.load(rl);
    s.redoFail += st.redoFail.load(rl);
    s.swapsAhead += st.swapsAhead.load(rl);
    s.redoNs += st.redoNs.load(rl);
    s.slackJobs += st.slackJobs.load(rl);
  }
  s.load = shrec::TakeLoad(g_load);
  return s;
}

// Per frame: scoped executions drawn stock by reason (late per slot), segments
// drawn stock in executed passes, and the residual items' top reasons.
std::string StockText(const StockSnap& a, const StockSnap& b, double frames) {
  const double f = frames > 0 ? frames : 1.0;
  char buf[256];
  snprintf(buf, sizeof(buf), "executions %.2f/frame, executed %.2f; late by slot %.2f %.2f %.2f %.2f; stock: ",
           (b.passes - a.passes) / f, (b.pass[kPExecuted] - a.pass[kPExecuted]) / f, (b.late[0] - a.late[0]) / f,
           (b.late[1] - a.late[1]) / f, (b.late[2] - a.late[2]) / f, (b.late[3] - a.late[3]) / f);
  std::string s = buf;
  s += shrec::ReasonList(kPassReasonName, a.pass, b.pass, kPassReasons, 1, frames, 6);
  s += "; segments stock: ";
  s += shrec::ReasonList(kSegReasonName, a.seg, b.seg, kSegReasons, 1, frames, 4);
  snprintf(buf, sizeof(buf),
           "; re-recorded segments %.2f planned, %.2f executed, %.2f not ready, %.2f failed; swaps ahead %.2f; "
           "planning %.3f ms; jobs with slack settings %.2f",
           (b.redoPlanned - a.redoPlanned) / f, (b.redoExecuted - a.redoExecuted) / f, (b.redoLate - a.redoLate) / f,
           (b.redoFail - a.redoFail) / f, (b.swapsAhead - a.swapsAhead) / f, (b.redoNs - a.redoNs) / (1e6 * f),
           (b.slackJobs - a.slackJobs) / f);
  s += buf;
  s += "; residual items: ";
  s += shrec::ReasonList(kReasonName, a.item, b.item, kReasons, 2, frames, 4);  // not a model item: never
  // Per slot from the render entry (ms): jobs in time (done, pass) and late jobs (pass, start, built, done).
  for (int k = 0; k < kSlots; ++k) {
    const uint64_t n = b.tlN[k] - a.tlN[k], m = b.lateN[k] - a.lateN[k];
    if (n) {
      snprintf(buf, sizeof(buf), "; slot %d in time (%llu): done %.2f, pass %.2f", k, static_cast<unsigned long long>(n),
               (b.tlDone[k] - a.tlDone[k]) / (1e6 * n), (b.tlPass[k] - a.tlPass[k]) / (1e6 * n));
      s += buf;
    }
    if (m) {
      const double d = 1e6 * m;
      snprintf(buf, sizeof(buf), "; slot %d late (%llu): pass %.2f, start %.2f, built %.2f, done %.2f", k,
               static_cast<unsigned long long>(m), (b.latePass[k] - a.latePass[k]) / d,
               (b.lateStart[k] - a.lateStart[k]) / d, (b.lateBuilt[k] - a.lateBuilt[k]) / d,
               (b.lateDone[k] - a.lateDone[k]) / d);
      s += buf;
    }
  }
  s += "; ";
  s += shrec::LoadText(a.load, b.load, frames, g_tscHz);
  return s;
}
}  // namespace gbrec
