// Cockpit mask export for a frame-generation layer ([Perception] CockpitMaskExport, default 0; contract:
// dcsvr_cockpit_mask.h; design: docs/research/R65_cockpit_mask_export.md). Not a performance optimization: outside
// the kill switch, not one of the status word's 18 features (its own status bits 39 / 40).
//
// Per frame, on the render thread (fixation_depth.h's boundary callback and the xrEndFrame hook below):
//  1. Views. CloseRun gives each HMD view's G-buffer run (DCS's per-view depth target, its viewport and cPerView, the
//     CopyToHMDPass ordinal = HMD view). A configured view ([Perception] CockpitMaskViews, bit mask, default 3 = the
//     two periphery views; 15 adds the focus views) is kept pending when the identity is certain: two or four HMD views
//     in the previous frame, the learnt HMD offset 0.
//  2. Sample point. The pending view is sampled at its WaterSeaPass end (after the opaque G-buffer and the water, as
//     the fixation depth and the far depth export), else at its GBufferLightingPass begin, else at the begin of the
//     first pass whose name contains "Transparent" (counted), else at the next view's passes. Never later: DCS's
//     transparent cockpit materials (BM_TRANSPARENT, Bazar/shaders/model/common/shader_macroses.hlsl
//     ENABLE_RO_DEPTH_BUFFER_COCKPIT) write the COCKPIT composition into the stencil too, so after
//     TransparentCockpitPass the canopy glass would be "cockpit".
//  3. Dispatch. One compute dispatch per view (fixation_depth.h RunCs: CS shader, CS CB 0-1, SRV 0-1, UAV 0 and, when
//     DCS's DSV is the sampled texture, the OM targets are saved and put back exactly): one thread per mask texel
//     reads its divisor x divisor block of the viewport ([Perception] CockpitMaskDivisor, 1-8; 0 = auto, the
//     default: per view the smallest divisor that keeps it <= 512 texels wide, e.g. DLSS periphery 476 -> 1,
//     focus 1176 -> 3) (plus `dilate` blocks of apron) and writes 1.0 (255) when any
//     texel has the stencil composition COCKPIT ((s & 0x38) == 0x28) or, with [Perception] CockpitMaskNearM > 0, a
//     forward distance below that (from this view's gProj in cPerView, bound as b1 like the grid), else 0. The output
//     is the view's rect in the frame's slot texture (an R8_UNORM atlas, views left to right in view order).
//  4. FOV. The rect's tangents from this frame's gProj on the CPU (far_depth.h's cPerView capture: the upload entries
//     are armed during GBufferPass while the export is on), else from the Perception layer's GazeOut fovTan of that
//     view (the FOV DCS renders with), else the view is not valid this frame (counted).
//  5. Commit. In DCS's xrEndFrame call (the OpenXR dispatch entry slot 35 that Visualizer's trampoline at +0x1b6abf
//     calls through; same table as frame_start.h's probes), on the render thread, before the call reaches any API
//     layer: the fence (a shared ID3D11Fence) is signalled with the slot's maskSeq on DCS's immediate context, then
//     the header is written (seqlock) with XrFrameEndInfo::displayTime. Without the hook (another Visualizer build,
//     table not found) a frame is committed at the next frame's first pass, flagged late_commit, displayTime 0.
//  6. Slots. Three textures (NT handles), reused oldest first; a slot whose previous maskSeq the reader has not
//     released (its release fence, when it uses it) is not written: that frame gets no mask (never a wait).
//  7. Layout. The atlas is laid out from the views' source sizes; a size change (resolution, DLSS mode) makes new
//     textures at the next commit (new generation; the old NT handles are closed 2 s later, by the config thread).
//  8. Reader heartbeat (R67, contract: readerHeartbeatQpc). At each frame's first pass (OnEndFrame) the reader's
//     heartbeat is read (one QPC + one acquire load): 0 or older than 2 s -> idle: no view kept (NoteRun), so no
//     dispatch, no timestamp query, no fence Signal (the Flush-priced call, R66), no per-frame header write, no
//     relayout, the CPU gProj capture unarmed (fdexport::g_maskUploads), the xrEndFrame hook returns at once, status
//     bits 39 / 40 clear; header_flags::idle published once at the transition. Fresh again -> active from that frame.
//     What remains while idle: that check, a 4-bool HasPending test per pass boundary, the hook's forward; and fixation
//     depth's boundary callback itself when the mask is its only user (it stays installed: CockpitMaskExport is the
//     master switch). Transitions are logged by Tick (at most one line per 5 s).
//  9. Depth companion (R69 = R68 Stage 1; [Perception] CockpitDepthExport, default 0, requires CockpitMaskExport;
//     contract dcsvr_cockpit_depth.h). The same dispatch, a shader variant with a second UAV (u1), also writes per mask
//     texel (R16G16_FLOAT, the same atlas layout, a second set of three NT-handle textures made with the mask's):
//     .x the nearest cockpit texel's depth (clip w, metres; 0 none), .y the farthest depth of the block (65504 sky).
//     The depth variant reads the whole block (no early exit). It runs only while the depth block's reader heartbeat
//     AND the mask's are fresh; otherwise the mask-only variant runs (or nothing, mask idle): idle depth adds nothing.
//     The commit writes the depth slot (same index, seq = maskSeq) BEFORE the mask header, then the mask slot with
//     slot_flags::depth; one fence Signal for both. Per view it adds: the dual camera's world-minus-cockpit shift and
//     Depth boost's h (cockpit_ipd.h, this frame), the DLSS jitter (this frame's gProj against DCS's submitted FOV)
//     and DCS's submitted pose / FOV / space (read from the XrFrameEndInfo in the xrEndFrame hook). Status bit 41.
// 10. Motion companion (R70 = R68 Stage 2; [Perception] CockpitMotionExport, default 0, requires CockpitMaskExport,
//     independent of the depth; contract dcsvr_motion.h). fixation_depth.h's OM hooks also note the G-buffer pass's
//     motion-vector render target (the single R16G16_FLOAT / RGBA16F RTV bound with the DSV); at the periphery views'
//     sample point a second dispatch copies DCS's MVs (render resolution, verbatim) into the slot's MV atlas and writes
//     the class byte (stencil composition + sky bit) into the class atlas. Its own heartbeat (and the mask's); the
//     commit writes the motion slot first (prevSeq / prevDisplayTime, both jitters, reset flags), then the depth slot,
//     then the mask header; one fence Signal. Status bit 42.
// 11. Held set (R85). A switch of an export key (mask, depth or motion: fixation_depth.h Tick uninstalls and installs)
//     does NOT release the shared set any more: the slot textures (+ UAVs), both shared fences and their NT handles,
//     the shaders and queries stay alive on DCS's device (Uninstall(hold)); the blocks get their last record with the
//     SAME generation and the same handles, header_flags::active clear (slots invalid, maskSeq kept). The install after
//     it keeps that record; the first Relayout whose layout is the held one republishes it active: same generation,
//     same handles (no texture, fence or handle is made, a reader has nothing to import). Released (R76: a new
//     generation, flags 0, the handles' 2 s grace) only on: a layout change (size, divisor, views, a companion wanted
//     that the set lacks, the motion class byte), a device change (EnsureBase) or a removed device (Install,
//     CheckHeld), the payload stop or reload (fixation_depth.h Shutdown: Uninstall(false) + ReleaseHeld), a fault.
//     Why: each reader open of an NT-handle texture adds its size to PDH Dedicated Usage for good (R1b, an accounting
//     artefact, not real VRAM; R84b / Dev's import bench), so opens must be rare. The held
//     set costs its own size while the exports are off (DLSS periphery views 476x468, auto divisor 1: atlas 952x468;
//     mask 3 x 0.4 MB, depth 3 x 1.7 MB, motion 3 x 1.7 MB + class 3 x 0.4 MB: about 13 MB; mask only about 1.3 MB).
// 12. F10 / loading hold (R88, agreed with Dev: option B). A layout change seen while no flight frame is drawn (the F10
//     map, loading screens, menus: they render at other sizes, R87 A2) does NOT relayout: the set is held as in 11.,
//     its blocks stay mapped and their records go inactive with the SAME generation and handles (header_flags::active
//     clear, slots invalid, maskSeq kept), and every later frame re-validates. With nothing made yet nothing is made
//     (the record stays inactive). Flight frame (FlightGateOpen): main.cpp's collect hook culled a Quad Views world
//     frame (g_quadFrame: one per culled world frame; none in the F10 map or a load, R87's scene-cut gaps) since the
//     previous commit, for this commit and the one before. No signal (no quad frame culled yet in this payload: two
//     views only, or before the first flight), or a hold of more than g_mapHoldMaxMs (15 min): the layout is taken
//     at once, as before R88. Resume: the first frame whose layout is the held one republishes it (same generation
//     and handles: nothing to import); another layout on a flight frame releases it (a new generation, as any layout
//     change). The motion companion has no source in the map: inactive there anyway.
// 13. Off the render thread (R91 = R90 phase 2). The shaders compile and the base objects (parameter buffer, both
//     fences and their NT handles, the timing queries) are made by Install on the config thread when DCS's device is
//     known (else by the render thread at the first capture, as before). A new set's textures, UAVs and NT handles
//     are made by the builder thread (BuilderLoop): the render thread requests the set at the relayout and swaps it
//     in at a later commit (SwapIn: the clears, the generation, the records; the old set released there, as by
//     CreateAtlas before). Generations and the R85 / R86 release semantics are unchanged. g_asyncBuild 0 makes the set
//     on the render thread (the old path); g_buildWaitMs (tests) waits for the builder in the same commit.
// 14. Symbology companion (R95; [Perception] SymbologyExport, default 0, requires CockpitMaskExport; contract
//     dcsvr_symbology.h; symbology_block.h, symbology.h). Per HMD view (all four), in its OverlayHDRPass the HUD /
//     helmet records' change of DCS's HDR target is accumulated over a capacity window chosen on the CPU, and the
//     tone-map draw (color_pass.h) evaluates the tone map a second time without it: E = dec(F) - dec(C) and the
//     coverage alpha into an RGBA16F atlas, the class into an R8_UINT atlas (both in the shared set: same generation,
//     held set, builder thread). Its own heartbeat (and the mask's); the commit writes its slot first; one fence
//     Signal. Status bit 43.
// Cost: GPU timestamps around each dispatch, render-thread QPC around the capture + commit; both in the 30 s log line.
// SEH: the capture and the commit run inside the boundary callback's / the hook's __try; a fault latches the export
// off for this load (fixation_depth.h g_fault for the callback, g_fault here for the hook).
// Included from fixation_depth.h inside namespace fixdepth, after RunCs.
#pragma once

namespace cmask {

namespace cm = ::dcsvr_cockpit_mask;
namespace cdp = ::dcsvr_cockpit_depth;
namespace cmv = ::dcsvr_motion;
namespace csy = ::dcsvr_symbology;  // R95
namespace sym {  // R95 (symbology.h)
bool CompileCs(pD3DCompile compile, std::string& err);
bool MakeBase(ID3D11Device* dev);
void ReleaseBase();
void ReleaseTargets();
void MakeQueriesB(ID3D11Device* dev);
void ReleaseQueriesB();
}

// ---------------------------------------------------------------------------
// Configuration (config thread writes; render thread reads)
// ---------------------------------------------------------------------------

constexpr int kMaxDiv = 8, kMaxDilate = 4, kSlots = cm::slot_count, kTsFrames = 4;
constexpr uint32_t kAutoMaxW = 512;  // CockpitMaskDivisor=0 (auto): the smallest divisor giving a view <= 512 texels wide
constexpr ULONGLONG kHandleGraceMs = 2000;
// R76: while every slot is held by the reader's release fence (each frame skipped, "reader busy"), the header is still
// published at least this often, so heartbeatQpc stays fresh: the reader's lookup gates its release on that freshness
// (500 ms), so a frozen heartbeat after a 0.5-2 s reader pause locked both sides for the rest of the generation.
constexpr ULONGLONG kBusyBeatMs = 100;

std::atomic<bool> g_on{false};        // [Perception] CockpitMaskExport
std::atomic<int> g_div{0};            // [Perception] CockpitMaskDivisor (1-8; 0 = auto, per view: width <= 512)
std::atomic<int> g_dilate{0};         // [Perception] CockpitMaskDilate (0-4 mask texels)
std::atomic<float> g_nearM{1.0f};     // [Perception] CockpitMaskNearM (0: stencil only)
std::atomic<uint32_t> g_views{3u};    // [Perception] CockpitMaskViews (bit v: HMD view v)
std::atomic<bool> g_xrHookWanted{true};  // tests: false keeps the OpenXR table untouched
std::atomic<bool> g_stopping{false};
std::atomic<bool> g_fault{false};
std::wstring g_blockName = cm::block_name;  // + DCSVR_PERCEPTION_SHM_SUFFIX (Configure)

// R69: [Perception] CockpitDepthExport (the depth companion; only with the mask export).
std::atomic<bool> g_depthOn{false};
std::wstring g_dBlockName = cdp::block_name;  // + DCSVR_PERCEPTION_SHM_SUFFIX (Configure)

// R70: [Perception] CockpitMotionExport / CockpitMotionClass (the motion companion; only with the mask export).
std::atomic<bool> g_motionOn{false};
std::atomic<bool> g_motionClass{true};
std::wstring g_mBlockName = cmv::block_name;  // + DCSVR_PERCEPTION_SHM_SUFFIX (Configure)

bool Wanted() { return g_on.load(std::memory_order_relaxed); }
bool MotionWanted() { return g_on.load(std::memory_order_relaxed) && g_motionOn.load(std::memory_order_relaxed); }
bool DepthWanted() { return g_on.load(std::memory_order_relaxed) && g_depthOn.load(std::memory_order_relaxed); }
bool Active() { return g_on.load(std::memory_order_relaxed) && !g_stopping.load(std::memory_order_relaxed) && !g_fault.load(std::memory_order_relaxed); }

// R67 reader heartbeat: idle while no reader heartbeat is fresh (render thread decides at a frame's first pass; the
// config thread sets the initial state at the install, before the callback goes in).
bool g_idle = true;                     // render thread
std::atomic<bool> g_idleA{true};        // its mirror for the status bits and the log (any thread)
int64_t g_qpcFreq = 0;                  // QueryPerformanceFrequency (Install)
std::atomic<uint64_t> g_idleFrames{0}, g_toIdle{0}, g_toActive{0}, g_signals{0};
// R69: the depth block's own reader heartbeat (depth is produced only while it AND the mask's are fresh).
bool g_dIdle = true;                    // render thread
std::atomic<bool> g_dIdleA{true};
std::atomic<uint64_t> g_dToIdle{0}, g_dToActive{0};
// R70: the motion block's own reader heartbeat (motion only while it AND the mask's are fresh).
bool g_mIdle = true;                    // render thread
std::atomic<bool> g_mIdleA{true};
std::atomic<uint64_t> g_mToIdle{0}, g_mToActive{0};

// R76 (HB-1, HB-3): the OM hooks' MV note (fixdepth::g_mvWanted) only while MVs are copied: the motion export on, its
// heartbeat AND the mask's fresh, no fault. Written only where g_idle / g_mIdle change (render thread; Install before
// the callback goes in); ConfigureMotion (config thread) only clears it when the export goes off (it recomputed it
// from g_mIdleA, so a transition between its load and its store was overwritten for up to 1 s).
bool MvWantedNow() { return MotionWanted() && !g_idle && !g_mIdle && !g_fault.load(std::memory_order_relaxed); }

// R76 (HB-2): a latched fault (render thread: the atlas could not be made, or an SEH in the xrEndFrame hook) also ends
// the active state's side effects. Active() is false from now on, so OnEndFrame never reaches UpdateReader / SetIdle:
// without this the gProj upload capture (fdexport::g_maskUploads) and the MV note stayed on until the next install.
void LatchFault() {
  g_fault = true;
  fdexport::g_maskUploads = false;
  fixdepth::g_mvWanted = false;
  g_idle = true;
  g_idleA.store(true, std::memory_order_relaxed);
}

// ---------------------------------------------------------------------------
// Statistics (any thread)
// ---------------------------------------------------------------------------

std::atomic<uint64_t> g_captures{0}, g_commits{0}, g_paired{0}, g_late{0}, g_partial{0}, g_skipNoSrv{0},
    g_skipFormat{0}, g_skipIdentity{0}, g_skipSize{0}, g_skipNoFov{0}, g_skipBusy{0}, g_skipNoRes{0}, g_relayouts{0},
    g_xeCalls{0}, g_xeOffThread{0}, g_xeEmpty{0}, g_afterWater{0}, g_atLighting{0}, g_atTransparent{0}, g_atOther{0},
    g_droppedAtEnd{0}, g_stateChecks{0}, g_stateDiffs{0}, g_fovGaze{0}, g_noStencil{0}, g_texCreates{0};
std::atomic<ULONGLONG> g_lastCommitMs{0};
std::atomic<bool> g_lastPaired{false};
// R69 depth: commits, views with depth, views without cPerView (no depth), jitter known / refused (FOV mismatch or
// > 2 px), slots with poses, views with a dual-camera shift.
std::atomic<uint64_t> g_dCommits{0}, g_dViews{0}, g_dNoCb{0}, g_dJitter{0}, g_dJitterBad{0}, g_dPoses{0}, g_dDual{0};
std::atomic<ULONGLONG> g_lastDepthMs{0};
// R70 motion: commits, views copied, slots flagged reset (and why: no previous, generation, sequence, jitter, cut),
// views without an MV target / without its SRV / of another format or size, partial slots.
std::atomic<uint64_t> g_mCommits{0}, g_mViews{0}, g_mResets{0}, g_mResetPrev{0}, g_mResetGen{0}, g_mResetSeq{0},
    g_mResetJit{0}, g_mResetCut{0}, g_mNoMv{0}, g_mNoSrv{0}, g_mBadFmt{0}, g_mPartial{0};
std::atomic<ULONGLONG> g_lastMotionMs{0};
// R70: the live checks (R70 ง10): T1 (still frames, DLSS jitter 0.3-0.95 px between frames): DCS's MVs are 0 +- 0.001
// px, i.e. JITTER-FREE (includes_jitter is never set; jitter_free is); T2 (synthetic yaw turns): every terrain / sky /
// model texel's MV equals the head rotation's motion at infinity within 0.016 px (p99.9 at 20 deg/s), the sky included.
constexpr bool kMvIncludesJitter = false;
constexpr bool kMvJitterFree = true;
constexpr bool kMvSkyIsInfinity = true;
std::atomic<bool> g_xrHooked{false};
namespace ctime {
std::atomic<bool> g_hooked{false};  // R74a (cockpit_time.h): the xrLocateViews hook is in; companions carry slotTime
}
std::mutex g_statMutex;
double g_gpuUsSum = 0, g_gpuUsMax = 0, g_cpuUsSum = 0, g_cpuUsMax = 0;  // under g_statMutex
// R66: the fence Signal's share of the render-thread time (ID3D11DeviceContext4::Signal submits the context's queued
// work like a Flush: ~55 us on the bare driver, more inside DCS; DCS's own xrEndFrame / Present flush then has less to
// submit, so this share is mostly moved, not added; the paired A/B is the authority). Under g_statMutex.
double g_sigUsSum = 0, g_sigUsMax = 0;
uint64_t g_gpuN = 0, g_cpuN = 0;

// ---------------------------------------------------------------------------
// Pure helpers (offline tested)
// ---------------------------------------------------------------------------

inline int ClampDiv(int d) { return std::clamp(d, 1, kMaxDiv); }
// A view's divisor: the configured one, or (0 = auto) the smallest that makes the view at most kAutoMaxW wide
// (DLSS periphery 476 -> 1, focus 1176 -> 3, a native 2448 -> 5).
inline int ViewDiv(int cfgDiv, uint32_t srcW) {
  if (cfgDiv > 0) return ClampDiv(cfgDiv);
  return ClampDiv(static_cast<int>((srcW + kAutoMaxW - 1) / kAutoMaxW));
}
inline uint32_t MaskDim(uint32_t src, int div) {
  div = ClampDiv(div);
  return src ? (src + static_cast<uint32_t>(div) - 1) / static_cast<uint32_t>(div) : 0;
}

struct Layout {
  uint32_t x[4] = {}, w[4] = {}, h[4] = {}, srcW[4] = {}, srcH[4] = {};
  uint32_t atlasW = 0, atlasH = 0, views = 0;
  int div = 0;          // the configured divisor (0 = auto)
  int dv[4] = {};       // each view's divisor
  bool Same(const Layout& o) const {
    return atlasW == o.atlasW && atlasH == o.atlasH && views == o.views && div == o.div && !memcmp(dv, o.dv, sizeof(dv)) &&
           !memcmp(x, o.x, sizeof(x)) &&
           !memcmp(w, o.w, sizeof(w)) && !memcmp(h, o.h, sizeof(h)) && !memcmp(srcW, o.srcW, sizeof(srcW)) &&
           !memcmp(srcH, o.srcH, sizeof(srcH));
  }
};

// The configured views left to right in view order, each ceil(src / divisor) texels (divisor: ViewDiv); false when a
// configured view's
// source size is not known yet or the atlas would exceed 16384 texels.
bool MakeLayout(const uint32_t srcW[4], const uint32_t srcH[4], uint32_t views, int div, Layout& out) {
  out = Layout{};
  div = div > 0 ? ClampDiv(div) : 0;
  views &= 15u;
  if (!views) return false;
  uint32_t x = 0, hMax = 0;
  for (int v = 0; v < 4; ++v) {
    if (!(views & (1u << v))) continue;
    if (!srcW[v] || !srcH[v]) return false;
    out.x[v] = x;
    out.dv[v] = ViewDiv(div, srcW[v]);
    out.w[v] = MaskDim(srcW[v], out.dv[v]);
    out.h[v] = MaskDim(srcH[v], out.dv[v]);
    out.srcW[v] = srcW[v];
    out.srcH[v] = srcH[v];
    x += out.w[v];
    hMax = std::max(hMax, out.h[v]);
  }
  if (x > 16384 || hMax > 16384) return false;
  out.atlasW = x;
  out.atlasH = hMax;
  out.views = views;
  out.div = div;
  return true;
}

// The rect's outer edges as tangents (left, right, up, down) for a separable projection p: the rect covers the source
// texels [vp.x, vp.x + w * div) x [vp.y, vp.y + h * div), which may pass the viewport's right / bottom edge by less
// than one block.
bool RectFov(const Proj& p, const Viewport& vp, uint32_t w, uint32_t h, int div, float out[4]) {
  if (!p.ok || vp.w <= 0 || vp.h <= 0) return false;
  const double ex = static_cast<double>(w) * div / vp.w, ey = static_cast<double>(h) * div / vp.h;
  const double nxL = -1.0, nxR = -1.0 + 2.0 * ex, nyU = 1.0, nyD = 1.0 - 2.0 * ey;
  const double v[4] = {(nxL - p.ox) / p.P[0][0], (nxR - p.ox) / p.P[0][0], (nyU - p.oy) / p.P[1][1],
                       (nyD - p.oy) / p.P[1][1]};
  for (int k = 0; k < 4; ++k) {
    if (!std::isfinite(v[k])) return false;
    out[k] = static_cast<float>(v[k]);
  }
  return out[1] > out[0] && out[2] > out[3];
}

// The same from a view FOV (left, right, up, down) covering exactly the viewport (GazeOut fovTan).
bool RectFovFromFov(const float fov[4], double vpW, double vpH, uint32_t w, uint32_t h, int div, float out[4]) {
  if (!(fov[1] > fov[0]) || !(fov[2] > fov[3]) || vpW <= 0 || vpH <= 0) return false;
  const double ex = static_cast<double>(w) * div / vpW, ey = static_cast<double>(h) * div / vpH;
  out[0] = fov[0];
  out[1] = static_cast<float>(fov[0] + (static_cast<double>(fov[1]) - fov[0]) * ex);
  out[2] = fov[2];
  out[3] = static_cast<float>(fov[2] - (static_cast<double>(fov[2]) - fov[3]) * ey);
  return true;
}

// The slot to write this frame: the one with the oldest maskSeq; -1 when the reader has not released it yet
// (releaseCompleted: the release fence's completed value; 0 = no reader uses it).
int PickSlot(const uint64_t seqs[kSlots], uint64_t releaseCompleted) {
  int best = 0;
  for (int i = 1; i < kSlots; ++i)
    if (seqs[i] < seqs[best]) best = i;
  const uint64_t prev = seqs[best];
  if (prev == 0 || releaseCompleted == 0 || prev <= releaseCompleted) return best;
  return -1;
}

// The CPU reference of the shader (tests): mask value of texel (col, row) of a view from its stencil bytes and
// forward distances (row-major, srcW x srcH, the viewport at the origin).
uint8_t RefTexel(const uint8_t* stencil, const float* distM, uint32_t srcW, uint32_t srcH, int div, int dilate,
                 float nearM, uint32_t col, uint32_t row) {
  const int apron = dilate * div;
  const int x0 = std::max(0, static_cast<int>(col) * div - apron), y0 = std::max(0, static_cast<int>(row) * div - apron);
  const int x1 = std::min(static_cast<int>(srcW), (static_cast<int>(col) + 1) * div + apron);
  const int y1 = std::min(static_cast<int>(srcH), (static_cast<int>(row) + 1) * div + apron);
  for (int y = y0; y < y1; ++y)
    for (int x = x0; x < x1; ++x) {
      const size_t i = static_cast<size_t>(y) * srcW + x;
      if (stencil && (stencil[i] & 0x38) == 0x28) return 255;
      if (distM && nearM > 0 && distM[i] > 0 && distM[i] < nearM) return 255;
    }
  return 0;
}

// R69: the CPU reference of the depth variant for the same texel and block: out[0] the nearest cockpit distance (0
// none), out[1] the farthest distance (cdp::sky_depth_m for the sky: distM 0 / inf, or beyond). Cockpit = RefTexel's
// test per texel.
void RefDepth(const uint8_t* stencil, const float* distM, uint32_t srcW, uint32_t srcH, int div, int dilate, float nearM,
              uint32_t col, uint32_t row, float out[2]) {
  const int apron = dilate * div;
  const int x0 = std::max(0, static_cast<int>(col) * div - apron), y0 = std::max(0, static_cast<int>(row) * div - apron);
  const int x1 = std::min(static_cast<int>(srcW), (static_cast<int>(col) + 1) * div + apron);
  const int y1 = std::min(static_cast<int>(srcH), (static_cast<int>(row) + 1) * div + apron);
  float nearC = cdp::sky_depth_m, farW = 0;
  bool any = false;
  for (int y = y0; y < y1; ++y)
    for (int x = x0; x < x1; ++x) {
      const size_t i = static_cast<size_t>(y) * srcW + x;
      const float d = distM ? distM[i] : 0.0f;
      const float w = (d > 0 && d < cdp::sky_depth_m) ? d : cdp::sky_depth_m;
      bool ck = stencil && (stencil[i] & 0x38) == 0x28;
      if (nearM > 0 && w < nearM) ck = true;
      if (ck) {
        any = true;
        nearC = std::min(nearC, w);
      }
      farW = std::max(farW, w);
    }
  out[0] = any ? nearC : 0.0f;
  out[1] = farW;
}

// R69: the dual camera's shift of view `idx` as the contract states it: world camera centre minus cockpit camera centre
// in the view's own space with OpenXR axes (x right, y up, z backward), metres. cockpit_ipd.h moves the cockpit camera
// by +dw (DCS world units, K units per metre; R rows: the camera's right, up, forward axes in DCS's world), so world
// minus cockpit = -dw: x = -(dw . right), y = -(dw . up), z = +(dw . forward) (z backward).
void WorldMinusCockpit(const double dw[3], const double R[3][3], double K, float out[3]) {
  const double k = K > 0 ? K : 1.0;
  const double r = dw[0] * R[0][0] + dw[1] * R[0][1] + dw[2] * R[0][2];
  const double u = dw[0] * R[1][0] + dw[1] * R[1][1] + dw[2] * R[1][2];
  const double f = dw[0] * R[2][0] + dw[1] * R[2][1] + dw[2] * R[2][2];
  out[0] = static_cast<float>(-r / k);
  out[1] = static_cast<float>(-u / k);
  out[2] = static_cast<float>(f / k);
}

// R69: Depth boost's shift of DCS's camera from the unboosted eye along the view's +x: left views (0, 2) -h, right +h.
float BoostShift(int idx, double h) { return static_cast<float>((idx & 1) ? h : -h); }
// R77: a view's boostShiftM and boost flag from the dual camera's frame: the boost record DCS renders with
// (Frame::boostH), not the shift the dual camera applied (Frame::h: 0 in a paused, inert, limited or off frame
// although DCS's camera is boosted). Unknown (NaN, flag clear) when the frame decided no record.
inline void FrameBoost(const ckipd::Frame& cf, int idx, float* shiftM, uint32_t* flags) {
  if (!cf.latched || !std::isfinite(cf.boostH)) return;
  *shiftM = BoostShift(idx, cf.boostH);
  *flags |= cdp::view_flags::boost;
}

// R69: the DLSS jitter in render texels (+x right, +y down; image = unjittered + jitter) from this frame's gProj view
// axis (ox, oy: P20 / P23, P21 / P23) and scales (p00, p11) against DCS's submitted XrFovf (angles, radians) of the
// view, viewport vw x vh. false when the FOVs disagree (the scales differ by more than 0.2 %) or the result exceeds
// 2 texels (not a jitter).
bool JitterPx(double ox, double oy, double p00, double p11, double vw, double vh, const float xrFov[4], float out[2]) {
  const double l = std::tan(static_cast<double>(xrFov[0])), r = std::tan(static_cast<double>(xrFov[1]));
  const double u = std::tan(static_cast<double>(xrFov[2])), d = std::tan(static_cast<double>(xrFov[3]));
  if (!(r > l) || !(u > d) || !(vw > 0) || !(vh > 0)) return false;
  const double sx = 2.0 / (r - l), sy = 2.0 / (u - d);
  if (!(std::fabs(p00 - sx) <= 2e-3 * sx) || !(std::fabs(p11 - sy) <= 2e-3 * sy)) return false;
  const double oxu = -(r + l) / (r - l), oyu = -(u + d) / (u - d);
  const double jx = (ox - oxu) * vw * 0.5, jy = -(oy - oyu) * vh * 0.5;
  if (!std::isfinite(jx) || !std::isfinite(jy) || std::fabs(jx) > 2.0 || std::fabs(jy) > 2.0) return false;
  out[0] = static_cast<float>(jx);
  out[1] = static_cast<float>(jy);
  return true;
}

// ---------------------------------------------------------------------------
// The shader
// ---------------------------------------------------------------------------

// One thread per mask texel. gFlags bit 0: test the stencil composition (a stencil SRV is bound), bit 1: test the
// near depth (cPerView bound as b1). The depth is undone from the viewport's MinDepth/MaxDepth first; distances are
// the clip w (forward distance) as ForwardDist computes it on the CPU.
const char kShader[] = R"(
cbuffer MaskParams : register(b0) {
  int2 gSrcOrg; int2 gSrcEnd; int2 gDstOrg; int2 gDstSize;
  int gDiv; int gApron; uint gFlags; float gNearM;
  float gMinD; float gMaxD; float2 gPad;
};
cbuffer cPerView : register(b1) {
  float4x4 gLocal, gLocalInv;
  float4x4 gView, gViewInv;
  float4x4 gProj, gProjInv;
};
#if FD_MS && FD_ARRAY
Texture2DMSArray<float> gDepth : register(t0);
Texture2DMSArray<uint2> gStencil : register(t1);
#define LOADD(p) gDepth.Load(int3(p, 0), 0)
#define LOADS(p) gStencil.Load(int3(p, 0), 0).g
#elif FD_MS
Texture2DMS<float> gDepth : register(t0);
Texture2DMS<uint2> gStencil : register(t1);
#define LOADD(p) gDepth.Load(p, 0)
#define LOADS(p) gStencil.Load(p, 0).g
#elif FD_ARRAY
Texture2DArray<float> gDepth : register(t0);
Texture2DArray<uint2> gStencil : register(t1);
#define LOADD(p) gDepth.Load(int4(p, 0, 0))
#define LOADS(p) gStencil.Load(int4(p, 0, 0)).g
#else
Texture2D<float> gDepth : register(t0);
Texture2D<uint2> gStencil : register(t1);
#define LOADD(p) gDepth.Load(int3(p, 0))
#define LOADS(p) gStencil.Load(int3(p, 0)).g
#endif
RWTexture2D<unorm float> gOut : register(u0);
#if FD_DEPTH
RWTexture2D<float2> gDepthOut : register(u1);
#endif
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  if (id.x >= (uint)gDstSize.x || id.y >= (uint)gDstSize.y) return;
  int2 lo = max(gSrcOrg + int2(id.xy) * gDiv - gApron, gSrcOrg);
  int2 hi = min(gSrcOrg + (int2(id.xy) + 1) * gDiv + gApron, gSrcEnd);
  float p22 = gProj[2][2], p23 = gProj[2][3], p32 = gProj[3][2], p33 = gProj[3][3];
  float dscale = gMaxD - gMinD;
#if FD_DEPTH
  // R69: the whole block (no early exit): the nearest cockpit distance and the farthest distance (65504 = sky).
  const float kSky = 65504.0;
  float nearC = kSky, farW = 0.0;
  bool anyC = false;
  [loop] for (int y = lo.y; y < hi.y; ++y) {
    [loop] for (int x = lo.x; x < hi.x; ++x) {
      int2 p = int2(x, y);
      bool ck = false;
      if (gFlags & 1u) {
        uint st = LOADS(p);
        ck = (st & 0x38u) == 0x28u;
      }
      float d = LOADD(p);
      float dn = dscale != 0 ? (d - gMinD) / dscale : d;
      float den = dn * p23 - p22;
      float w = kSky;
      if (abs(den) > 1e-30) {
        float z = (p32 - dn * p33) / den;
        float ww = z * p23 + p33;
        if (ww > 0 && ww < kSky) w = ww;
      }
      if ((gFlags & 2u) && w < gNearM) ck = true;
      if (ck) {
        anyC = true;
        nearC = min(nearC, w);
      }
      farW = max(farW, w);
    }
  }
  gOut[gDstOrg + int2(id.xy)] = anyC ? 1.0 : 0.0;
  gDepthOut[gDstOrg + int2(id.xy)] = float2(anyC ? nearC : 0.0, farW);
#else
  bool hit = false;
  [loop] for (int y = lo.y; y < hi.y && !hit; ++y) {
    [loop] for (int x = lo.x; x < hi.x && !hit; ++x) {
      int2 p = int2(x, y);
      if (gFlags & 1u) {
        uint st = LOADS(p);
        if ((st & 0x38u) == 0x28u) hit = true;
      }
      if (!hit && (gFlags & 2u)) {
        float d = LOADD(p);
        float dn = dscale != 0 ? (d - gMinD) / dscale : d;
        float den = dn * p23 - p22;
        if (abs(den) > 1e-30) {
          float z = (p32 - dn * p33) / den;
          float w = z * p23 + p33;
          if (w > 0 && w < gNearM) hit = true;
        }
      }
    }
  }
  gOut[gDstOrg + int2(id.xy)] = hit ? 1.0 : 0.0;
#endif
}
)";

// R70: the motion copy. One thread per render texel of the view's viewport: DCS's MV (t2, .xy) into the MV atlas (u0,
// R16G16_FLOAT) and, with gFlags bit 2, the class byte into the class atlas (u1, R8_UINT): bits 0-2 the stencil
// composition (bit 0 of gFlags: a stencil SRV is bound; else 7), bit 7 sky (the depth is at infinity: from gProj with
// gFlags bit 1, else depth == 0, DCS's reverse-Z). The same MaskParams: gSrcOrg / gSrcEnd the viewport (clipped to the
// texture), gDstOrg / gDstSize the rect.
const char kMotionShader[] = R"(
cbuffer MaskParams : register(b0) {
  int2 gSrcOrg; int2 gSrcEnd; int2 gDstOrg; int2 gDstSize;
  int gDiv; int gApron; uint gFlags; float gNearM;
  float gMinD; float gMaxD; float2 gPad;
};
cbuffer cPerView : register(b1) {
  float4x4 gLocal, gLocalInv;
  float4x4 gView, gViewInv;
  float4x4 gProj, gProjInv;
};
#if FD_MS && FD_ARRAY
Texture2DMSArray<float> gDepth : register(t0);
Texture2DMSArray<uint2> gStencil : register(t1);
#define LOADD(p) gDepth.Load(int3(p, 0), 0)
#define LOADS(p) gStencil.Load(int3(p, 0), 0).g
#elif FD_MS
Texture2DMS<float> gDepth : register(t0);
Texture2DMS<uint2> gStencil : register(t1);
#define LOADD(p) gDepth.Load(p, 0)
#define LOADS(p) gStencil.Load(p, 0).g
#elif FD_ARRAY
Texture2DArray<float> gDepth : register(t0);
Texture2DArray<uint2> gStencil : register(t1);
#define LOADD(p) gDepth.Load(int4(p, 0, 0))
#define LOADS(p) gStencil.Load(int4(p, 0, 0)).g
#else
Texture2D<float> gDepth : register(t0);
Texture2D<uint2> gStencil : register(t1);
#define LOADD(p) gDepth.Load(int3(p, 0))
#define LOADS(p) gStencil.Load(int3(p, 0)).g
#endif
Texture2D<float4> gMv : register(t2);
RWTexture2D<float2> gOutMv : register(u0);
RWTexture2D<uint> gOutClass : register(u1);
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  if (id.x >= (uint)gDstSize.x || id.y >= (uint)gDstSize.y) return;
  int2 p = gSrcOrg + int2(id.xy);
  bool inside = p.x < gSrcEnd.x && p.y < gSrcEnd.y;
  float2 mv = inside ? gMv.Load(int3(p, 0)).xy : float2(0, 0);
  gOutMv[gDstOrg + int2(id.xy)] = mv;
  if (gFlags & 4u) {
    uint cls = 7u;
    if (inside) {
      if (gFlags & 1u) cls = (LOADS(p) >> 3) & 7u;
      float d = LOADD(p);
      float dscale = gMaxD - gMinD;
      float dn = dscale != 0 ? (d - gMinD) / dscale : d;
      bool sky;
      if (gFlags & 2u) {  // R70 live: DCS's sky is a far dome, not depth 0: sky = no finite distance below 65504 m
        float den = dn * gProj[2][3] - gProj[2][2];
        sky = !(abs(den) > 1e-30);
        if (!sky) {
          float z = (gProj[3][2] - dn * gProj[3][3]) / den;
          float w = z * gProj[2][3] + gProj[3][3];
          sky = !(w > 0 && w < 65504.0);
        }
      } else {
        sky = dn == 0;
      }
      if (sky) cls |= 0x80u;
    }
    gOutClass[gDstOrg + int2(id.xy)] = cls;
  }
}
)";
std::vector<uint8_t> g_mblob[4];  // R70: variant = ms * 2 + array

struct MaskParams {
  int32_t srcOrg[2], srcEnd[2], dstOrg[2], dstSize[2];
  int32_t div, apron;
  uint32_t flags;
  float nearM, minD, maxD, pad[2];
};
static_assert(sizeof(MaskParams) == 64, "MaskParams layout");

std::vector<uint8_t> g_blob[8];  // variant = depth * 4 + ms * 2 + array (R69: 4-7 also write the depth, u1)
std::atomic<bool> g_blobsReady{false};
std::string g_compileError;

bool Compile() {
  if (g_blobsReady.load()) return true;
  wchar_t sys[MAX_PATH];
  const UINT n = GetSystemDirectoryW(sys, MAX_PATH);
  if (!n || n >= MAX_PATH) return false;
  HMODULE m = LoadLibraryW((std::wstring(sys) + L"\\d3dcompiler_47.dll").c_str());
  if (!m) {
    g_compileError = "d3dcompiler_47.dll not loadable";
    return false;
  }
  auto compile = reinterpret_cast<pD3DCompile>(GetProcAddress(m, "D3DCompile"));
  if (!compile) {
    g_compileError = "D3DCompile not exported";
    return false;
  }
  for (int v = 0; v < 8; ++v) {
    const D3D_SHADER_MACRO defs[] = {{"FD_MS", (v & 2) ? "1" : "0"}, {"FD_ARRAY", (v & 1) ? "1" : "0"},
                                     {"FD_DEPTH", (v & 4) ? "1" : "0"}, {nullptr, nullptr}};
    ID3DBlob* code = nullptr;
    ID3DBlob* err = nullptr;
    const HRESULT hr = compile(kShader, sizeof(kShader) - 1, "cockpit_mask", defs, nullptr, "main", "cs_5_0",
                               D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &err);
    if (FAILED(hr) || !code) {
      g_compileError = err ? std::string(static_cast<const char*>(err->GetBufferPointer()), err->GetBufferSize())
                           : std::string("D3DCompile failed");
      if (err) err->Release();
      if (code) code->Release();
      return false;
    }
    const uint8_t* b = static_cast<const uint8_t*>(code->GetBufferPointer());
    g_blob[v].assign(b, b + code->GetBufferSize());
    code->Release();
    if (err) err->Release();
  }
  for (int v = 0; v < 4; ++v) {  // R70: the motion copy
    const D3D_SHADER_MACRO defs[] = {{"FD_MS", (v & 2) ? "1" : "0"}, {"FD_ARRAY", (v & 1) ? "1" : "0"}, {nullptr, nullptr}};
    ID3DBlob* code = nullptr;
    ID3DBlob* err = nullptr;
    const HRESULT hr = compile(kMotionShader, sizeof(kMotionShader) - 1, "cockpit_motion", defs, nullptr, "main", "cs_5_0",
                               D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &err);
    if (FAILED(hr) || !code) {
      g_compileError = err ? std::string(static_cast<const char*>(err->GetBufferPointer()), err->GetBufferSize())
                           : std::string("D3DCompile failed (motion)");
      if (err) err->Release();
      if (code) code->Release();
      return false;
    }
    const uint8_t* b = static_cast<const uint8_t*>(code->GetBufferPointer());
    g_mblob[v].assign(b, b + code->GetBufferSize());
    code->Release();
    if (err) err->Release();
  }
  if (!sym::CompileCs(compile, g_compileError)) return false;  // R95
  g_blobsReady = true;
  return true;
}

// ---------------------------------------------------------------------------
// D3D resources, the shared block (render thread; install / uninstall on the config thread)
// ---------------------------------------------------------------------------

struct Res {
  ID3D11Device* dev = nullptr;  // referenced
  ID3D11DeviceContext4* ctx4 = nullptr;
  ID3D11ComputeShader* cs[8] = {};  // R69: 4-7 the depth variants
  ID3D11Buffer* params = nullptr;
  ID3D11Texture2D* tex[kSlots] = {};
  ID3D11UnorderedAccessView* uav[kSlots] = {};
  HANDLE texH[kSlots] = {};
  // R69: the depth companion's R16G16_FLOAT atlas (made with the mask's when the depth export is configured).
  ID3D11Texture2D* dtex[kSlots] = {};
  ID3D11UnorderedAccessView* duav[kSlots] = {};
  HANDLE dtexH[kSlots] = {};
  bool haveDepth = false;
  // R70: the motion companion: its copy shaders, the MV atlas (R16G16_FLOAT) and the class atlas (R8_UINT), periphery
  // views 0 / 1 left to right at render resolution.
  ID3D11ComputeShader* mcs[4] = {};
  ID3D11Texture2D* mtex[kSlots] = {};
  ID3D11UnorderedAccessView* muav[kSlots] = {};
  HANDLE mtexH[kSlots] = {};
  ID3D11Texture2D* ctex[kSlots] = {};
  ID3D11UnorderedAccessView* cuav[kSlots] = {};
  HANDLE ctexH[kSlots] = {};
  bool haveMotion = false, haveClass = false;
  uint32_t mx[2] = {}, mw[2] = {}, mh[2] = {}, mW = 0, mH = 0;
  // R95: the symbology companion (symbology.h): the emission atlases (RGBA16F) and class atlases (R8_UINT), shared
  // (NT handles); private: the run-start copy of a window (RGBA16F), the accumulator (RGBA32F), the scan signatures.
  ID3D11Texture2D* stex[kSlots] = {};
  ID3D11UnorderedAccessView* suav[kSlots] = {};
  HANDLE stexH[kSlots] = {};
  ID3D11Texture2D* sctex[kSlots] = {};
  ID3D11UnorderedAccessView* scuav[kSlots] = {};
  HANDLE sctexH[kSlots] = {};
  ID3D11Texture2D* sPre = nullptr;
  ID3D11ShaderResourceView* sPreSrv = nullptr;
  ID3D11Texture2D* sAcc = nullptr;
  ID3D11ShaderResourceView* sAccSrv = nullptr;
  ID3D11UnorderedAccessView* sAccUav = nullptr;
  ID3D11Texture2D* sSig = nullptr;
  ID3D11UnorderedAccessView* sSigUav = nullptr;
  bool haveSym = false;
  uint32_t sCap[2] = {}, sW = 0, sH = 0;  // the capacities (periphery, focus) and the atlas size of this set
  // DCS's MV targets as SRVs (keyed by the resource; references held).
  struct MvSrv {
    ID3D11Resource* res = nullptr;
    ID3D11ShaderResourceView* srv = nullptr;
    UINT w = 0, h = 0;
    bool rgba = false;
    uint64_t used = 0;
  } mvSrv[4];
  ID3D11Fence* fence = nullptr;
  HANDLE fenceH = nullptr;
  ID3D11Fence* rel = nullptr;  // the reader's release fence (we create it; the reader signals it)
  HANDLE relH = nullptr;
  uint64_t luid = 0;
  Layout layout;
  bool haveLayout = false, baseOk = false, baseFailed = false;
  // GPU timing: per frame of a 4-frame ring, a disjoint query and a timestamp pair per view.
  ID3D11Query* dj[kTsFrames] = {};
  ID3D11Query* ts[kTsFrames][4][2] = {};
  bool tsUsed[kTsFrames][4] = {};
  bool tsPending[kTsFrames] = {};
};
Res g_r;

// R85: the shared set (g_r's textures, fences, NT handles) is held across an export uninstall: its last record went
// out inactive with this generation and these handles; the next Relayout of the same layout republishes it (config
// thread at the uninstall / install, render thread in between: the install boundary orders them).
bool g_held = false;
std::atomic<uint64_t> g_holds{0}, g_republished{0}, g_heldReleases{0};
// R88: the set is held by the flight gate (no flight frame, a new layout; 12. at the top), not by an uninstall.
bool g_heldForMap = false;  // as g_held
std::atomic<uint64_t> g_mapHolds{0}, g_mapResumes{0}, g_mapRelayouts{0}, g_mapHeldFrames{0}, g_mapWaitFrames{0},
    g_mapTimeouts{0};
std::atomic<bool> g_flightGate{true};              // tests: false = before R88 (a layout change relays at once)
std::atomic<uint32_t> g_mapHoldMaxMs{15u * 60000u};  // a hold longer than this takes the layout (a dead signal)
uint64_t g_quadSeen = 0;      // render thread: g_quadFrame (main.cpp) at the previous commit
uint32_t g_flightBits = 0;    // render thread: bit 0 = a quad frame was culled for this commit, bit 1 the one before
ULONGLONG g_gateClosedMs = 0;  // render thread: since when FlightGateOpen has said no (0: open)
bool g_gateTimedOut = false;   // render thread: this closed stretch passed g_mapHoldMaxMs (the backstop)
// R91: where EnsureBase made the base objects (Install on the config thread, or a capture on the render thread).
std::atomic<uint64_t> g_baseAtInstall{0}, g_baseAtCapture{0};
bool g_baseFromInstall = false;  // config thread, around Install's EnsureBase
uint64_t g_flightNoted = ~0ull;  // render thread: the engine frame (g_s.frame) last sampled
// Once per engine frame, at its first commit (render thread): its xrEndFrame's, or, without the hook or when that one
// did not come, the late one at the next frame's first pass (OnEndFrame commits every frame while a reader is there,
// so a frame can see two commits: only the first samples). DCS culls a frame's world before its passes (R39), so the
// quad frames counted since the previous sample are this frame's.
void NoteFlightFrame() {
  if (g_s.frame == g_flightNoted) return;
  g_flightNoted = g_s.frame;
  const uint64_t q = g_quadFrame.load(std::memory_order_relaxed);
  g_flightBits = (g_flightBits << 1) | (q != g_quadSeen ? 1u : 0u);
  g_quadSeen = q;
  if ((g_flightBits & 3u) == 3u) {
    g_gateClosedMs = 0;
    g_gateTimedOut = false;
  }
}
// A layout change may be made now: a flight frame (this commit and the one before culled a Quad Views world frame),
// or no signal (gate off, no quad frame culled yet in this payload), or a hold past g_mapHoldMaxMs.
bool FlightGateOpen() {
  if (!g_flightGate.load(std::memory_order_relaxed) || g_quadFrame.load(std::memory_order_relaxed) == 0) return true;
  if ((g_flightBits & 3u) == 3u) return true;
  const ULONGLONG now = GetTickCount64();
  if (!g_gateClosedMs) g_gateClosedMs = now;
  if (now - g_gateClosedMs <= g_mapHoldMaxMs.load(std::memory_order_relaxed)) return false;
  if (!g_gateTimedOut) {
    g_gateTimedOut = true;
    if (g_mapTimeouts++ == 0)
      Log("cockpit mask: no flight frame (no Quad Views world culling) for %u s with another layout: the layout is "
          "taken as it is until the next flight frames (R88 backstop; logged once)", g_mapHoldMaxMs.load() / 1000u);
  }
  return true;
}

// NT handles of older generations, closed kHandleGraceMs after they were replaced (config thread).
std::mutex g_graveMutex;
std::vector<std::pair<HANDLE, ULONGLONG>> g_grave;
void Bury(HANDLE h) {
  if (!h) return;
  std::lock_guard<std::mutex> lock(g_graveMutex);
  g_grave.emplace_back(h, GetTickCount64() + kHandleGraceMs);
}
void CloseBuried(bool all) {
  std::lock_guard<std::mutex> lock(g_graveMutex);
  const ULONGLONG now = GetTickCount64();
  for (size_t i = 0; i < g_grave.size();) {
    if (all || now >= g_grave[i].second) {
      CloseHandle(g_grave[i].first);
      g_grave[i] = g_grave.back();
      g_grave.pop_back();
    } else {
      ++i;
    }
  }
}

// The block (mapped at install; the render thread is its only writer while installed).
HANDLE g_map = nullptr;
std::atomic<cm::Header*> g_view{nullptr};
cm::Header g_hdr{};  // the writer's copy (render thread; config thread at install / uninstall)
uint32_t g_gen = 0;
uint64_t g_seq = 0;  // the last maskSeq committed
// R86: the first generation of this payload is random (a reader cannot mistake it for a previous payload's); every
// later one is +1 (never 0). Before R86 a release before the first Install (g_gen 0) published generation 1.
uint32_t SeedGen() { return (GetTickCount() ^ (GetCurrentProcessId() << 16)) | 1u; }
void BumpGen() {
  if (!g_gen)
    g_gen = SeedGen();
  else if (++g_gen == 0)
    g_gen = 1;
}
// R86 (tests): GetDeviceRemovedReason as seen by the held-set checks (a removed device cannot be made on WARP).
std::atomic<bool> g_testRemoved{false};
bool DevRemoved(ID3D11Device* d) { return g_testRemoved.load() || d->GetDeviceRemovedReason() != S_OK; }
bool g_cmInstalled = false;  // R86: Install ran (the export on) and its Uninstall has not

// final: the uninstall's last record (flags as they are: 0).
ULONGLONG g_hdrPubMs = 0;  // R76: GetTickCount64 of the last header write (render thread; the busy skip's refresh)
std::atomic<uint64_t> g_busyBeats{0};  // R76: header refreshes during release-fence busy skips
void PublishHeader(bool final = false) {
  cm::Header* v = g_view.load();
  if (!v) return;
  LARGE_INTEGER q, f;
  QueryPerformanceCounter(&q);
  QueryPerformanceFrequency(&f);
  constexpr uint32_t hbFlags = cm::header_flags::reader_heartbeat | cm::header_flags::idle;
  if (!final)
    g_hdr.flags = (g_hdr.flags & ~hbFlags) | cm::header_flags::reader_heartbeat | (g_idle ? cm::header_flags::idle : 0u);
  g_hdr.magic = cm::block_magic;
  g_hdr.version = cm::block_version;
  g_hdr.structSize = sizeof(cm::Header);
  g_hdr.slotCount = cm::slot_count;
  g_hdr.format = cm::format_r8_unorm;
  g_hdr.ownerPid = GetCurrentProcessId();
  g_hdr.heartbeatQpc = q.QuadPart;
  g_hdr.qpcFrequency = f.QuadPart;
  cm::seqlock_write(v, g_hdr);
  g_hdrPubMs = GetTickCount64();  // R76
}

// existingOnly (R85 ReleaseHeld): only a block a reader still maps (none is created).
bool OpenBlock(bool existingOnly = false) {
  if (g_view.load()) return true;
  g_map = existingOnly ? OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, g_blockName.c_str())
                       : CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, 4096, g_blockName.c_str());
  if (!g_map) return false;
  void* v = MapViewOfFile(g_map, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(cm::Header));
  if (!v) {
    CloseHandle(g_map);
    g_map = nullptr;
    return false;
  }
  g_view = static_cast<cm::Header*>(v);
  return true;
}

// The block back to "inactive" (a last record) and unmapped. keep (R85, the set held): the record keeps its
// generation and handles, only active goes (no slot valid; maskSeq kept: the release fence still gates the slots).
void CloseBlock(bool keep = false) {
  if (cm::Header* v = g_view.load()) {
    if (keep) {
      g_hdr.flags &= ~cm::header_flags::active;
      for (cm::MaskSlot& s : g_hdr.slot) {
        const uint64_t k = s.maskSeq;
        s = cm::MaskSlot{};
        s.maskSeq = k;
      }
    } else {
      g_hdr.flags = 0;
      for (auto& h : g_hdr.textureHandle) h = 0;
      g_hdr.fenceHandle = g_hdr.releaseFenceHandle = 0;
      for (cm::MaskSlot& s : g_hdr.slot) s = cm::MaskSlot{};
    }
    PublishHeader(true);
    g_view = nullptr;
    UnmapViewOfFile(v);
  }
  if (g_map) CloseHandle(g_map);
  g_map = nullptr;
}

// R69: the depth companion block (mapped at install when CockpitDepthExport=1; render thread writes it).
HANDLE g_dmap = nullptr;
std::atomic<cdp::Header*> g_dview{nullptr};
cdp::Header g_dhdr{};

// Idle (no header_flags::active-gated production) while this block's heartbeat or the mask's is not fresh.
void PublishDepthHeader(bool final = false) {
  cdp::Header* v = g_dview.load();
  if (!v) return;
  LARGE_INTEGER q, f;
  QueryPerformanceCounter(&q);
  QueryPerformanceFrequency(&f);
  constexpr uint32_t hbFlags = cdp::header_flags::reader_heartbeat | cdp::header_flags::idle;
  if (!final)
    g_dhdr.flags = (g_dhdr.flags & ~hbFlags) | cdp::header_flags::reader_heartbeat |
                   ((g_idle || g_dIdle) ? cdp::header_flags::idle : 0u);
  if (!final)  // R74a: slotTime is filled while the xrLocateViews hook is in
    g_dhdr.flags = (g_dhdr.flags & ~cdp::header_flags::slot_time) |
                   (ctime::g_hooked.load(std::memory_order_relaxed) ? cdp::header_flags::slot_time : 0u);
  g_dhdr.magic = cdp::block_magic;
  g_dhdr.version = cdp::block_version;
  g_dhdr.structSize = sizeof(cdp::Header);
  g_dhdr.slotCount = cdp::slot_count;
  g_dhdr.format = cdp::format_r16g16_float;
  g_dhdr.ownerPid = GetCurrentProcessId();
  g_dhdr.revision = cdp::contract_revision;
  g_dhdr.heartbeatQpc = q.QuadPart;
  g_dhdr.qpcFrequency = f.QuadPart;
  cdp::seqlock_write(v, g_dhdr);
}

bool OpenDepthBlock(bool existingOnly = false) {
  if (g_dview.load()) return true;
  g_dmap = existingOnly ? OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, g_dBlockName.c_str())
                        : CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, 4096, g_dBlockName.c_str());
  if (!g_dmap) return false;
  void* v = MapViewOfFile(g_dmap, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(cdp::Header));
  if (!v) {
    CloseHandle(g_dmap);
    g_dmap = nullptr;
    return false;
  }
  g_dview = static_cast<cdp::Header*>(v);
  return true;
}

// R70: the motion companion block (mapped at install when CockpitMotionExport=1; render thread writes it).
HANDLE g_mmap = nullptr;
std::atomic<cmv::Header*> g_mview{nullptr};
cmv::Header g_mhdr{};

void PublishMotionHeader(bool final = false) {
  cmv::Header* v = g_mview.load();
  if (!v) return;
  LARGE_INTEGER q, f;
  QueryPerformanceCounter(&q);
  QueryPerformanceFrequency(&f);
  constexpr uint32_t hbFlags = cmv::header_flags::reader_heartbeat | cmv::header_flags::idle;
  if (!final)
    g_mhdr.flags = (g_mhdr.flags & ~hbFlags) | cmv::header_flags::reader_heartbeat |
                   ((g_idle || g_mIdle) ? cmv::header_flags::idle : 0u);
  if (!final)  // R74a
    g_mhdr.flags = (g_mhdr.flags & ~cmv::header_flags::slot_time) |
                   (ctime::g_hooked.load(std::memory_order_relaxed) ? cmv::header_flags::slot_time : 0u);
  g_mhdr.magic = cmv::block_magic;
  g_mhdr.version = cmv::block_version;
  g_mhdr.structSize = sizeof(cmv::Header);
  g_mhdr.slotCount = cmv::slot_count;
  g_mhdr.format = cmv::format_r16g16_float;
  g_mhdr.ownerPid = GetCurrentProcessId();
  g_mhdr.revision = cmv::contract_revision;
  g_mhdr.heartbeatQpc = q.QuadPart;
  g_mhdr.qpcFrequency = f.QuadPart;
  cmv::seqlock_write(v, g_mhdr);
}

bool OpenMotionBlock(bool existingOnly = false) {
  if (g_mview.load()) return true;
  g_mmap = existingOnly ? OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, g_mBlockName.c_str())
                        : CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, 4096, g_mBlockName.c_str());
  if (!g_mmap) return false;
  void* v = MapViewOfFile(g_mmap, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(cmv::Header));
  if (!v) {
    CloseHandle(g_mmap);
    g_mmap = nullptr;
    return false;
  }
  g_mview = static_cast<cmv::Header*>(v);
  return true;
}

void CloseMotionBlock(bool keep = false) {  // keep: R85 (CloseBlock)
  if (cmv::Header* v = g_mview.load()) {
    if (keep) {
      g_mhdr.flags &= ~cmv::header_flags::active;
    } else {
      g_mhdr.flags = 0;
      for (auto& h : g_mhdr.textureHandle) h = 0;
      for (auto& h : g_mhdr.classTextureHandle) h = 0;
      g_mhdr.fenceHandle = 0;
      g_mhdr.classFormat = 0;
    }
    for (cmv::MotionSlot& s : g_mhdr.slot) s = cmv::MotionSlot{};
    for (cmv::SlotTime& t : g_mhdr.slotTime) t = cmv::SlotTime{};  // R74a
    PublishMotionHeader(true);
    g_mview = nullptr;
    UnmapViewOfFile(v);
  }
  if (g_mmap) CloseHandle(g_mmap);
  g_mmap = nullptr;
}

// The depth block back to "inactive" (a last record) and unmapped.
void CloseDepthBlock(bool keep = false) {  // keep: R85 (CloseBlock)
  if (cdp::Header* v = g_dview.load()) {
    if (keep) {
      g_dhdr.flags &= ~cdp::header_flags::active;
    } else {
      g_dhdr.flags = 0;
      for (auto& h : g_dhdr.textureHandle) h = 0;
      g_dhdr.fenceHandle = 0;
    }
    for (cdp::DepthSlot& s : g_dhdr.slot) s = cdp::DepthSlot{};
    for (cdp::SlotTime& t : g_dhdr.slotTime) t = cdp::SlotTime{};  // R74a
    PublishDepthHeader(true);
    g_dview = nullptr;
    UnmapViewOfFile(v);
  }
  if (g_dmap) CloseHandle(g_dmap);
  g_dmap = nullptr;
}

#include "symbology_block.h"  // R95: the symbology companion's configuration and block (as the motion block)

// R85: a release while a block is not mapped by us (its export off, its record the held one): R76's record (this
// generation, flags 0, no handles) into it when a reader still maps it (none is created).
// R86: our copy of such a record becomes R76's too whether or not a reader maps the block: before, a block no reader
// mapped kept the old generation and the released set's (buried, later closed) NT handles in g_dhdr / g_mhdr, and
// an Install while a newer set was held (no reset of the copy) published them into the block made anew.
void RetireMotionRec() {
  g_mhdr.generation = g_gen;
  g_mhdr.flags = 0;
  for (auto& h : g_mhdr.textureHandle) h = 0;
  for (auto& h : g_mhdr.classTextureHandle) h = 0;
  g_mhdr.fenceHandle = 0;
  g_mhdr.classFormat = 0;
  for (cmv::MotionSlot& s : g_mhdr.slot) s = cmv::MotionSlot{};
  for (cmv::SlotTime& t : g_mhdr.slotTime) t = cmv::SlotTime{};
}
void RetireDepthRec() {
  g_dhdr.generation = g_gen;
  g_dhdr.flags = 0;
  for (auto& h : g_dhdr.textureHandle) h = 0;
  g_dhdr.fenceHandle = 0;
  for (cdp::DepthSlot& s : g_dhdr.slot) s = cdp::DepthSlot{};
  for (cdp::SlotTime& t : g_dhdr.slotTime) t = cdp::SlotTime{};
}
void RetireUnmapped() {
  if (!g_sview.load()) {  // R95
    RetireSymRec();
    if (OpenSymBlock(true)) CloseSymBlock();
  }
  if (!g_mview.load()) {
    RetireMotionRec();
    if (OpenMotionBlock(true)) CloseMotionBlock();
  }
  if (!g_dview.load()) {
    RetireDepthRec();
    if (OpenDepthBlock(true)) CloseDepthBlock();
  }
  if (!g_view.load()) {
    g_hdr.generation = g_gen;
    g_hdr.flags = 0;
    for (auto& h : g_hdr.textureHandle) h = 0;
    g_hdr.fenceHandle = g_hdr.releaseFenceHandle = 0;
    for (cm::MaskSlot& s : g_hdr.slot) {
      const uint64_t k = s.maskSeq;
      s = cm::MaskSlot{};
      s.maskSeq = k;
    }
    if (OpenBlock(true)) CloseBlock();
  }
}

template <class T>
void RelP(T*& p) {
  if (p) p->Release();
  p = nullptr;
}

namespace covl {
void ReleaseAtlasRes();  // R74b (cockpit_overlay.h)
void ReleaseAllRes();
}
void ReleaseAtlas() {
  covl::ReleaseAtlasRes();  // R74b: its SRVs of the slot textures and the scratch atlas first
  for (int i = 0; i < kSlots; ++i) {
    RelP(g_r.uav[i]);
    RelP(g_r.tex[i]);
    Bury(g_r.texH[i]);
    g_r.texH[i] = nullptr;
    RelP(g_r.duav[i]);  // R69
    RelP(g_r.dtex[i]);
    Bury(g_r.dtexH[i]);
    g_r.dtexH[i] = nullptr;
    RelP(g_r.muav[i]);  // R70
    RelP(g_r.mtex[i]);
    Bury(g_r.mtexH[i]);
    g_r.mtexH[i] = nullptr;
    RelP(g_r.cuav[i]);
    RelP(g_r.ctex[i]);
    Bury(g_r.ctexH[i]);
    g_r.ctexH[i] = nullptr;
    RelP(g_r.suav[i]);  // R95
    RelP(g_r.stex[i]);
    Bury(g_r.stexH[i]);
    g_r.stexH[i] = nullptr;
    RelP(g_r.scuav[i]);
    RelP(g_r.sctex[i]);
    Bury(g_r.sctexH[i]);
    g_r.sctexH[i] = nullptr;
  }
  RelP(g_r.sPreSrv);  // R95: the private textures
  RelP(g_r.sPre);
  RelP(g_r.sAccUav);
  RelP(g_r.sAccSrv);
  RelP(g_r.sAcc);
  RelP(g_r.sSigUav);
  RelP(g_r.sSig);
  g_r.haveSym = false;
  g_r.sCap[0] = g_r.sCap[1] = g_r.sW = g_r.sH = 0;
  g_r.haveDepth = false;
  g_r.haveMotion = g_r.haveClass = false;
  g_r.haveLayout = false;
  g_r.layout = Layout{};
}

// Every D3D object and handle (the uninstall, a device change). The header keeps describing nothing afterwards.
void ReleaseDumps();
void DiscardBuild();  // R91 (below)
void ReleaseAll() {
  DiscardBuild();  // R91: a set being made for this device is not wanted any more
  g_heldForMap = false;  // R88
  covl::ReleaseAllRes();  // R74b: DCS's HDR targets it holds too
  sym::ReleaseTargets();  // R95: DCS's HDR targets too
  ReleaseDumps();
  ReleaseAtlas();
  sym::ReleaseBase();  // R95
  for (auto*& c : g_r.cs) RelP(c);
  for (auto*& c : g_r.mcs) RelP(c);  // R70
  for (auto& m : g_r.mvSrv) {
    RelP(m.srv);
    RelP(m.res);
  }
  RelP(g_r.params);
  RelP(g_r.fence);
  Bury(g_r.fenceH);
  RelP(g_r.rel);
  Bury(g_r.relH);
  for (int f = 0; f < kTsFrames; ++f) {
    RelP(g_r.dj[f]);
    for (int v = 0; v < 4; ++v)
      for (auto*& q : g_r.ts[f][v]) RelP(q);
  }
  RelP(g_r.ctx4);
  RelP(g_r.dev);
  g_r = Res{};
  g_hdr.flags = 0;
  for (auto& h : g_hdr.textureHandle) h = 0;
  g_hdr.fenceHandle = g_hdr.releaseFenceHandle = 0;
  g_dhdr.flags = 0;  // R69
  for (auto& h : g_dhdr.textureHandle) h = 0;
  g_dhdr.fenceHandle = 0;
  g_mhdr.flags = 0;  // R70
  for (auto& h : g_mhdr.textureHandle) h = 0;
  for (auto& h : g_mhdr.classTextureHandle) h = 0;
  g_mhdr.fenceHandle = 0;
  g_shdr.flags = 0;  // R95
  ClearSymSet();
}

HANDLE SharedHandleOf(ID3D11Resource* r) {
  IDXGIResource1* d = nullptr;
  HANDLE h = nullptr;
  if (SUCCEEDED(r->QueryInterface(__uuidof(IDXGIResource1), reinterpret_cast<void**>(&d))) && d) {
    if (FAILED(d->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &h)))
      h = nullptr;
    d->Release();
  }
  return h;
}

// The GPU timing queries (EnsureBase; R85 HoldRes makes them anew: a frame begun at the uninstall left one begun).
void MakeQueries(ID3D11Device* dev) {
  D3D11_QUERY_DESC q{D3D11_QUERY_TIMESTAMP, 0};
  D3D11_QUERY_DESC qd{D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
  for (int f = 0; f < kTsFrames; ++f) {
    dev->CreateQuery(&qd, &g_r.dj[f]);
    for (int v = 0; v < 4; ++v)
      for (auto*& t : g_r.ts[f][v]) dev->CreateQuery(&q, &t);
  }
}
void ReleaseQueries() {
  for (int f = 0; f < kTsFrames; ++f) {
    RelP(g_r.dj[f]);
    for (int v = 0; v < 4; ++v)
      for (auto*& q : g_r.ts[f][v]) RelP(q);
    g_r.tsPending[f] = false;
    for (bool& u : g_r.tsUsed[f]) u = false;
  }
}

// Shaders, parameters, fences, timing queries on `dev` (render thread at the first capture, or the install).
bool EnsureBase(ID3D11Device* dev, ID3D11DeviceContext* ctx) {
  if (g_r.dev == dev && g_r.baseOk) return true;
  if (g_r.dev == dev && g_r.baseFailed) return false;
  if (g_r.dev) {
    // A device change releases everything (a held set too, R85) with R76's record first: a new generation, flags 0.
    if (g_held) {
      g_held = false;
      g_heldReleases++;
      Log("cockpit mask: held shared set of generation %u released (device change)", g_gen);
    }
    ReleaseAll();
    BumpGen();
    g_hdr.generation = g_dhdr.generation = g_mhdr.generation = g_shdr.generation = g_gen;
    PublishSymHeader(true);  // R95
    PublishMotionHeader(true);
    PublishDepthHeader(true);
    PublishHeader(true);
    RetireUnmapped();  // a companion whose export is off (its held record)
  }
  dev->AddRef();
  g_r.dev = dev;
  g_r.baseFailed = true;
  if (!Compile()) return false;
  for (int v = 0; v < 8; ++v)
    if (FAILED(dev->CreateComputeShader(g_blob[v].data(), g_blob[v].size(), nullptr, &g_r.cs[v]))) return false;
  for (int v = 0; v < 4; ++v)  // R70
    if (FAILED(dev->CreateComputeShader(g_mblob[v].data(), g_mblob[v].size(), nullptr, &g_r.mcs[v]))) return false;
  if (!sym::MakeBase(dev)) return false;  // R95: its compute shaders, constants, statistics and queries
  D3D11_BUFFER_DESC pd{};
  pd.ByteWidth = 256;  // 16 constants: a whole *SetConstantBuffers1 range (RunCs binds 0..16)
  pd.Usage = D3D11_USAGE_DYNAMIC;
  pd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
  pd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
  if (FAILED(dev->CreateBuffer(&pd, nullptr, &g_r.params))) return false;
  // The fences (D3D11.4): optional; without them a reader must read on DCS's device.
  ID3D11Device5* d5 = nullptr;
  if (SUCCEEDED(dev->QueryInterface(__uuidof(ID3D11Device5), reinterpret_cast<void**>(&d5))) && d5) {
    if (ctx) ctx->QueryInterface(__uuidof(ID3D11DeviceContext4), reinterpret_cast<void**>(&g_r.ctx4));
    if (g_r.ctx4 &&
        SUCCEEDED(d5->CreateFence(g_seq, D3D11_FENCE_FLAG_SHARED, __uuidof(ID3D11Fence), reinterpret_cast<void**>(&g_r.fence))) &&
        g_r.fence) {
      if (FAILED(g_r.fence->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &g_r.fenceH))) g_r.fenceH = nullptr;
      if (SUCCEEDED(d5->CreateFence(0, D3D11_FENCE_FLAG_SHARED, __uuidof(ID3D11Fence), reinterpret_cast<void**>(&g_r.rel))) &&
          g_r.rel && FAILED(g_r.rel->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &g_r.relH)))
        g_r.relH = nullptr;
    }
    d5->Release();
  }
  if (!g_r.fenceH) {
    RelP(g_r.fence);
    RelP(g_r.ctx4);
  }
  if (!g_r.relH) RelP(g_r.rel);
  IDXGIDevice* dx = nullptr;
  if (SUCCEEDED(dev->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&dx))) && dx) {
    IDXGIAdapter* a = nullptr;
    if (SUCCEEDED(dx->GetAdapter(&a)) && a) {
      DXGI_ADAPTER_DESC ad{};
      if (SUCCEEDED(a->GetDesc(&ad)))
        g_r.luid = static_cast<uint64_t>(ad.AdapterLuid.LowPart) | (static_cast<uint64_t>(static_cast<uint32_t>(ad.AdapterLuid.HighPart)) << 32);
      a->Release();
    }
    dx->Release();
  }
  MakeQueries(dev);
  g_r.baseOk = true;
  g_r.baseFailed = false;
  (g_baseFromInstall ? g_baseAtInstall : g_baseAtCapture)++;  // R91
  return true;
}

void PublishSet(bool depth, bool motion, bool sym = false);
// R91 (R90 phase 2, F9): a new set's objects (three slot textures with their UAVs and NT handles per atlas: mask, and
// depth / motion / class when wanted) are made by the builder thread (13. at the top), or inline with g_asyncBuild 0;
// SwapIn (render thread) clears them and publishes them. Before R91 CreateAtlas made them on the render thread.
std::atomic<bool> g_testFailAtlas{false};  // R86 (tests): the set's creation fails (SwapIn: after the old set's release)
std::atomic<int> g_testBuildFault{0};  // R115 (tests): BuildSet raises an access violation on the builder thread
std::atomic<DWORD> g_bLoopTid{0};      // R115: the builder thread's id (BuilderLoop)
struct BuildKey {
  ID3D11Device* dev = nullptr;  // referenced by the Built that carries it
  Layout L;
  bool depth = false, motion = false, cls = false;
  bool sym = false;           // R95: the symbology atlases (capacities sCap: periphery, focus)
  uint32_t sCap[2] = {};
  bool Same(const BuildKey& o) const {
    return dev == o.dev && L.Same(o.L) && depth == o.depth && motion == o.motion && cls == o.cls && sym == o.sym &&
           sCap[0] == o.sCap[0] && sCap[1] == o.sCap[1];
  }
};
struct Built {
  BuildKey key;
  bool ok = false;
  int made = 0;  // textures made
  ID3D11Texture2D* tex[kSlots] = {};
  ID3D11UnorderedAccessView* uav[kSlots] = {};
  HANDLE texH[kSlots] = {};
  ID3D11Texture2D* dtex[kSlots] = {};
  ID3D11UnorderedAccessView* duav[kSlots] = {};
  HANDLE dtexH[kSlots] = {};
  ID3D11Texture2D* mtex[kSlots] = {};
  ID3D11UnorderedAccessView* muav[kSlots] = {};
  HANDLE mtexH[kSlots] = {};
  ID3D11Texture2D* ctex[kSlots] = {};
  ID3D11UnorderedAccessView* cuav[kSlots] = {};
  HANDLE ctexH[kSlots] = {};
  bool haveDepth = false, haveMotion = false, haveClass = false;
  uint32_t mx[2] = {}, mw[2] = {}, mh[2] = {}, mW = 0, mH = 0;
  // R95: as Res's symbology objects.
  ID3D11Texture2D* stex[kSlots] = {};
  ID3D11UnorderedAccessView* suav[kSlots] = {};
  HANDLE stexH[kSlots] = {};
  ID3D11Texture2D* sctex[kSlots] = {};
  ID3D11UnorderedAccessView* scuav[kSlots] = {};
  HANDLE sctexH[kSlots] = {};
  ID3D11Texture2D* sPre = nullptr;
  ID3D11ShaderResourceView* sPreSrv = nullptr;
  ID3D11Texture2D* sAcc = nullptr;
  ID3D11ShaderResourceView* sAccSrv = nullptr;
  ID3D11UnorderedAccessView* sAccUav = nullptr;
  ID3D11Texture2D* sSig = nullptr;
  ID3D11UnorderedAccessView* sSigUav = nullptr;
  bool haveSym = false;
  uint32_t sCap[2] = {}, sW = 0, sH = 0;
  int64_t sUse0 = -1, sUse1 = -1;  // R97: DXGI local CurrentUsage right before / after the symbology objects were made
};
int64_t g_sUse0 = -1, g_sUse1 = -1;  // R97: the published set's DXGI usage around its symbology textures
// A set that was never published (failed, discarded or stale): its NT handles were in no record, so they close now.
void ReleaseBuilt(Built& b) {
  auto closeH = [](HANDLE& h) {
    if (h) CloseHandle(h);
    h = nullptr;
  };
  for (int i = 0; i < kSlots; ++i) {
    RelP(b.uav[i]);
    RelP(b.tex[i]);
    closeH(b.texH[i]);
    RelP(b.duav[i]);
    RelP(b.dtex[i]);
    closeH(b.dtexH[i]);
    RelP(b.muav[i]);
    RelP(b.mtex[i]);
    closeH(b.mtexH[i]);
    RelP(b.cuav[i]);
    RelP(b.ctex[i]);
    closeH(b.ctexH[i]);
    RelP(b.suav[i]);  // R95
    RelP(b.stex[i]);
    closeH(b.stexH[i]);
    RelP(b.scuav[i]);
    RelP(b.sctex[i]);
    closeH(b.sctexH[i]);
  }
  RelP(b.sPreSrv);
  RelP(b.sPre);
  RelP(b.sAccUav);
  RelP(b.sAccSrv);
  RelP(b.sAcc);
  RelP(b.sSigUav);
  RelP(b.sSig);
  RelP(b.key.dev);
  b = Built{};
}
// The objects of b.key's set (any thread: D3D11 resource creation is free-threaded; nothing of g_r is read or
// written). False: what was made stays in b (ReleaseBuilt).
bool BuildSet(Built& b) {
  const Layout& L = b.key.L;
  ID3D11Device* dev = b.key.dev;
  if (!dev || g_testFailAtlas.load()) return false;
  if (g_testBuildFault.load(std::memory_order_relaxed) && GetCurrentThreadId() == g_bLoopTid.load())
    RaiseException(EXCEPTION_ACCESS_VIOLATION, 0, 0, nullptr);  // R115 (tests): a driver fault inside the creates
  D3D11_TEXTURE2D_DESC td{};
  td.Width = L.atlasW;
  td.Height = L.atlasH;
  td.MipLevels = td.ArraySize = 1;
  td.Format = DXGI_FORMAT_R8_UNORM;
  td.SampleDesc.Count = 1;
  td.Usage = D3D11_USAGE_DEFAULT;
  td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
  td.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
  for (int i = 0; i < kSlots; ++i) {
    if (FAILED(dev->CreateTexture2D(&td, nullptr, &b.tex[i])) || !b.tex[i]) return false;
    ++b.made;
    if (FAILED(dev->CreateUnorderedAccessView(b.tex[i], nullptr, &b.uav[i])) || !b.uav[i]) return false;
    b.texH[i] = SharedHandleOf(b.tex[i]);
    if (!b.texH[i]) return false;
  }
  // R69: the depth companion's textures, the same size, made with the mask's (one generation for both).
  if (b.key.depth) {
    D3D11_TEXTURE2D_DESC dd = td;
    dd.Format = DXGI_FORMAT_R16G16_FLOAT;
    for (int i = 0; i < kSlots; ++i) {
      if (FAILED(dev->CreateTexture2D(&dd, nullptr, &b.dtex[i])) || !b.dtex[i]) return false;
      ++b.made;
      if (FAILED(dev->CreateUnorderedAccessView(b.dtex[i], nullptr, &b.duav[i])) || !b.duav[i]) return false;
      b.dtexH[i] = SharedHandleOf(b.dtex[i]);
      if (!b.dtexH[i]) return false;
    }
    b.haveDepth = true;
  }
  // R70: the motion atlases: views 0 / 1 (when configured) at their render resolution, left to right.
  if (b.key.motion) {
    uint32_t x = 0, hMax = 0;
    for (int v = 0; v < 2; ++v) {
      b.mx[v] = b.mw[v] = b.mh[v] = 0;
      if (!(L.views & (1u << v))) continue;
      b.mx[v] = x;
      b.mw[v] = L.srcW[v];
      b.mh[v] = L.srcH[v];
      x += L.srcW[v];
      hMax = std::max(hMax, L.srcH[v]);
    }
    if (x > 16384 || hMax > 16384 || !x || !hMax) return false;
    b.mW = x;
    b.mH = hMax;
    D3D11_TEXTURE2D_DESC md = td;
    md.Width = x;
    md.Height = hMax;
    md.Format = DXGI_FORMAT_R16G16_FLOAT;
    D3D11_TEXTURE2D_DESC cd = md;
    cd.Format = DXGI_FORMAT_R8_UINT;
    for (int i = 0; i < kSlots; ++i) {
      if (FAILED(dev->CreateTexture2D(&md, nullptr, &b.mtex[i])) || !b.mtex[i]) return false;
      ++b.made;
      if (FAILED(dev->CreateUnorderedAccessView(b.mtex[i], nullptr, &b.muav[i])) || !b.muav[i]) return false;
      b.mtexH[i] = SharedHandleOf(b.mtex[i]);
      if (!b.mtexH[i]) return false;
      if (b.key.cls) {
        if (FAILED(dev->CreateTexture2D(&cd, nullptr, &b.ctex[i])) || !b.ctex[i]) return false;
        ++b.made;
        if (FAILED(dev->CreateUnorderedAccessView(b.ctex[i], nullptr, &b.cuav[i])) || !b.cuav[i]) return false;
        b.ctexH[i] = SharedHandleOf(b.ctex[i]);
        if (!b.ctexH[i]) return false;
      }
    }
    b.haveMotion = true;
    b.haveClass = b.key.cls;
  }
  // R95: the symbology atlases (the contract's packing: the periphery windows stacked, the focus windows beside them)
  // and the private textures sized for the larger capacity.
  if (b.key.sym) {
    b.sUse0 = LocalUsageBytes(dev);  // R97
    const uint32_t cp = b.key.sCap[0], cf = b.key.sCap[1];
    const uint32_t W = cp + 2 * cf, H = std::max(2 * cp, cf), cmax = std::max(cp, cf);
    if (!cp || !cf || W > 16384 || H > 16384) return false;
    D3D11_TEXTURE2D_DESC ed = td;
    ed.Width = W;
    ed.Height = H;
    ed.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    D3D11_TEXTURE2D_DESC cd = ed;
    cd.Format = DXGI_FORMAT_R8_UINT;
    for (int i = 0; i < kSlots; ++i) {
      if (FAILED(dev->CreateTexture2D(&ed, nullptr, &b.stex[i])) || !b.stex[i]) return false;
      ++b.made;
      if (FAILED(dev->CreateUnorderedAccessView(b.stex[i], nullptr, &b.suav[i])) || !b.suav[i]) return false;
      b.stexH[i] = SharedHandleOf(b.stex[i]);
      if (!b.stexH[i]) return false;
      if (FAILED(dev->CreateTexture2D(&cd, nullptr, &b.sctex[i])) || !b.sctex[i]) return false;
      ++b.made;
      if (FAILED(dev->CreateUnorderedAccessView(b.sctex[i], nullptr, &b.scuav[i])) || !b.scuav[i]) return false;
      b.sctexH[i] = SharedHandleOf(b.sctex[i]);
      if (!b.sctexH[i]) return false;
    }
    D3D11_TEXTURE2D_DESC pd{};
    pd.Width = pd.Height = cmax;
    pd.MipLevels = pd.ArraySize = 1;
    pd.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    pd.SampleDesc.Count = 1;
    pd.Usage = D3D11_USAGE_DEFAULT;
    pd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(dev->CreateTexture2D(&pd, nullptr, &b.sPre)) || FAILED(dev->CreateShaderResourceView(b.sPre, nullptr, &b.sPreSrv)))
      return false;
    pd.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    pd.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    if (FAILED(dev->CreateTexture2D(&pd, nullptr, &b.sAcc)) || FAILED(dev->CreateShaderResourceView(b.sAcc, nullptr, &b.sAccSrv)) ||
        FAILED(dev->CreateUnorderedAccessView(b.sAcc, nullptr, &b.sAccUav)))
      return false;
    pd.Width = pd.Height = 512;  // sym::kScanTiles
    pd.Format = DXGI_FORMAT_R32_UINT;
    pd.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
    if (FAILED(dev->CreateTexture2D(&pd, nullptr, &b.sSig)) || FAILED(dev->CreateUnorderedAccessView(b.sSig, nullptr, &b.sSigUav)))
      return false;
    b.haveSym = true;
    b.sCap[0] = cp, b.sCap[1] = cf, b.sW = W, b.sH = H;
    b.sUse1 = LocalUsageBytes(dev);  // R97
  }
  b.ok = true;
  return true;
}

// The builder thread (R91): one request at a time. g_bState (under g_bMutex): 0 idle, 1 requested, 2 being made,
// 3 made (g_bJob holds the result until the render thread takes it, or a discard releases it).
std::mutex g_bMutex;
std::condition_variable g_bCv;
// Config thread (EnsureBuilder / StopBuilder). A pointer, never destroyed at unload: a joinable std::thread destroyed
// by the DLL's static destructors (DCS exiting without a payload stop) would call std::terminate.
std::thread* g_bThread = nullptr;
std::atomic<bool> g_bRunning{false};
bool g_bStop = false;             // under g_bMutex
bool g_bCancel = false;           // under g_bMutex: the set being made is not wanted any more
int g_bState = 0;                 // under g_bMutex
Built g_bJob;                     // under g_bMutex: the request (1) / the result (3)
// Render thread (config thread while the module's render paths are out: Install, Uninstall, a device change there):
// a set was requested and is not swapped in yet, and for what.
bool g_bPending = false;
BuildKey g_bPendKey;
std::atomic<int> g_asyncBuild{1};   // 0: SwapIn's set made inline on the render thread (before R91)
std::atomic<int> g_buildWaitMs{0};  // tests: the render thread waits up to this for the builder in the same commit
std::atomic<uint64_t> g_buildN{0}, g_buildUsSum{0}, g_buildUsMax{0}, g_buildsInline{0}, g_buildWaits{0},
    g_buildsDiscarded{0}, g_buildsFailed{0};
std::atomic<uint64_t> g_buildFaults{0};  // R115: contained faults inside BuildSet on the builder thread
std::atomic<DWORD> g_buildTid{0};          // the thread that made the last set (tests, diagnostics)
std::atomic<int> g_testBuildDelayMs{0};    // tests: the builder waits this long before it makes a set

void NoteBuildTime(int64_t q0, int64_t q1) {
  LARGE_INTEGER f;
  QueryPerformanceFrequency(&f);
  const uint64_t us = static_cast<uint64_t>((q1 - q0) * 1000000 / (f.QuadPart ? f.QuadPart : 1));
  g_buildN++;
  g_buildUsSum += us;
  uint64_t m = g_buildUsMax.load();
  while (us > m && !g_buildUsMax.compare_exchange_weak(m, us)) {
  }
}

// R115 (H2): BuildSet on the builder thread under its own SEH frame (no C++ objects with destructors here: C2712).
// A contained fault (exit_guard.h's Filter: an access violation in the driver, ...) returns false instead of ending
// the thread under qvexit::RunVoid with g_bState stuck at 2 (every later RequestBuild refused, the export silently
// never relaid).
bool BuildSetSeh(Built& b, qvexit::Fault* f) {
  __try {
    BuildSet(b);
    return true;
  } __except (qvexit::Filter(GetExceptionInformation(), f)) {
    return false;
  }
}

void BuilderLoop() {
  g_bLoopTid = GetCurrentThreadId();
  std::unique_lock<std::mutex> lock(g_bMutex);
  for (;;) {
    g_bCv.wait(lock, [] { return g_bStop || g_bState == 1; });
    if (g_bState != 1) return;  // stopped with nothing requested
    Built b = g_bJob;
    g_bJob = Built{};
    g_bState = 2;
    g_bCancel = false;
    lock.unlock();
    if (const int ms = g_testBuildDelayMs.load()) Sleep(static_cast<DWORD>(ms));
    LARGE_INTEGER q0, q1;
    QueryPerformanceCounter(&q0);
    qvexit::Fault fault;
    if (!BuildSetSeh(b, &fault)) {
      // R115 (H2): the set is a failed one (b.ok false): SwapIn's failure path (R76's record under a new generation,
      // flags 0, no handles; the fault latched) or a discard. What was made before the fault is leaked on purpose:
      // the driver faulted inside a create, so those objects and handles are not trusted to release. The device
      // reference (the request's) is kept in b.key and released with it as usual.
      const BuildKey key = b.key;
      b = Built{};
      b.key = key;
      if (g_buildFaults++ == 0) {
        char at[160];
        qvexit::Where(fault.at, at, sizeof(at));
        Log("cockpit mask: exception 0x%08lx at %s while the builder thread made a set; the set counts as failed, the "
            "builder thread goes on (R115; logged once)", fault.code, at);
      }
    }
    QueryPerformanceCounter(&q1);
    NoteBuildTime(q0.QuadPart, q1.QuadPart);
    g_buildTid = GetCurrentThreadId();
    lock.lock();
    if (g_bCancel || g_bStop) {
      ReleaseBuilt(b);
      g_bState = 0;
    } else {
      g_bJob = b;
      g_bState = 3;
    }
    g_bCancel = false;
    g_bCv.notify_all();
  }
}

// Config thread (Install): the builder runs from the first install to the payload stop (an idle wait otherwise).
void EnsureBuilder() {
  if (g_bRunning.load()) return;
  {
    std::lock_guard<std::mutex> lock(g_bMutex);
    g_bStop = false;
  }
  try {
    g_bThread = new std::thread([] { qvexit::RunVoid(&BuilderLoop); });
  } catch (...) {  // no thread: the render thread makes the sets (the path before R91)
    g_bThread = nullptr;
    Log("cockpit mask: the builder thread could not be started; sets are made on the render thread");
    return;
  }
  g_bRunning = true;
}

// Config thread (fixation_depth.h Shutdown, after the uninstall): the builder ends; a set it made is released.
void StopBuilder() {
  if (!g_bRunning.load()) return;
  {
    std::lock_guard<std::mutex> lock(g_bMutex);
    g_bStop = true;
    g_bCancel = true;
    if (g_bState == 1) {
      ReleaseBuilt(g_bJob);
      g_bState = 0;
    }
  }
  g_bCv.notify_all();
  if (g_bThread) {
    if (g_bThread->joinable()) g_bThread->join();
    delete g_bThread;
    g_bThread = nullptr;
  }
  g_bRunning = false;
  std::lock_guard<std::mutex> lock(g_bMutex);
  if (g_bState == 3) ReleaseBuilt(g_bJob);
  g_bState = 0;
  g_bStop = false;
  g_bCancel = false;
  g_bPending = false;
}

// Render thread: a set for key (its device referenced by the request). False: the builder is still busy with one
// that was discarded (retried at the next commit).
bool RequestBuild(const BuildKey& key) {
  std::lock_guard<std::mutex> lock(g_bMutex);
  if (g_bState != 0) return false;
  g_bJob = Built{};
  g_bJob.key = key;
  key.dev->AddRef();
  g_bState = 1;
  g_bCv.notify_all();
  return true;
}

// Render thread: the made set, when there is one (moved into out).
bool TakeBuild(Built& out) {
  std::lock_guard<std::mutex> lock(g_bMutex);
  if (g_bState != 3) return false;
  out = g_bJob;
  g_bJob = Built{};
  g_bState = 0;
  return true;
}

// Tests (g_buildWaitMs): until the builder made the set (or nothing is requested).
void WaitBuild(int ms) {
  std::unique_lock<std::mutex> lock(g_bMutex);
  g_bCv.wait_for(lock, std::chrono::milliseconds(ms), [] { return g_bState == 3 || g_bState == 0; });
}

// The requested set is not wanted (another layout, a device change, the uninstall): released when made, else when
// the builder finishes it (any thread; the render thread's g_bPending, see above).
void DiscardBuild() {
  if (!g_bPending) return;
  g_bPending = false;
  g_buildsDiscarded++;
  std::lock_guard<std::mutex> lock(g_bMutex);
  if (g_bState == 1 || g_bState == 3) {
    ReleaseBuilt(g_bJob);
    g_bState = 0;
  } else if (g_bState == 2) {
    g_bCancel = true;
  }
}

// The set's records active with generation g_gen: motion, depth (each when published), then the mask (CreateAtlas;
// R85 Relayout: the held set again, the same generation and handles).
void PublishSet(bool depth, bool motion, bool sym) {
  const Layout& L = g_r.layout;
  g_hdr.generation = g_gen;
  g_hdr.flags = cm::header_flags::active | (g_r.fenceH ? cm::header_flags::fence : 0) |
                (g_r.relH ? cm::header_flags::release_fence : 0);
  g_hdr.atlasW = L.atlasW;
  g_hdr.atlasH = L.atlasH;
  g_hdr.adapterLuid = g_r.luid;
  for (int i = 0; i < kSlots; ++i) g_hdr.textureHandle[i] = reinterpret_cast<uint64_t>(g_r.texH[i]);
  g_hdr.fenceHandle = reinterpret_cast<uint64_t>(g_r.fenceH);
  g_hdr.releaseFenceHandle = reinterpret_cast<uint64_t>(g_r.relH);
  for (cm::MaskSlot& s : g_hdr.slot) {  // new textures: nothing in them; the maskSeq counter keeps growing
    const uint64_t keep = s.maskSeq;
    s = cm::MaskSlot{};
    s.maskSeq = keep;
  }
  if (sym) {  // R95: published first, as the other companions
    g_shdr.generation = g_gen;
    g_shdr.flags = csy::header_flags::active | csy::header_flags::class_atlas |
                   (g_r.fenceH ? csy::header_flags::fence | csy::header_flags::shares_mask_fence : 0u);
    g_shdr.atlasW = g_r.sW;
    g_shdr.atlasH = g_r.sH;
    g_shdr.adapterLuid = g_r.luid;
    for (int i = 0; i < kSlots; ++i) {
      g_shdr.textureHandle[i] = reinterpret_cast<uint64_t>(g_r.stexH[i]);
      g_shdr.classTextureHandle[i] = reinterpret_cast<uint64_t>(g_r.sctexH[i]);
    }
    g_shdr.classFormat = csy::format_r8_uint;
    g_shdr.fenceHandle = reinterpret_cast<uint64_t>(g_r.fenceH);
    g_shdr.emissionFlags = csy::emission_flags::hdr_overlay | csy::emission_flags::class_collimated |
                           csy::emission_flags::class_head_locked | csy::emission_flags::common_excluded;
    // (emission_flags::alpha_valid: set by the commits once the read-backs agree, symbology.h AlphaVote)
    for (csy::EmissionSlot& s : g_shdr.slot) s = csy::EmissionSlot{};
    PublishSymHeader();
  }
  if (motion) {  // R70: published first, as the depth
    g_mhdr.generation = g_gen;
    g_mhdr.flags = cmv::header_flags::active |
                   (g_r.fenceH ? cmv::header_flags::fence | cmv::header_flags::shares_mask_fence : 0u) |
                   (g_r.haveClass ? cmv::header_flags::class_atlas : 0u);
    g_mhdr.atlasW = g_r.mW;
    g_mhdr.atlasH = g_r.mH;
    g_mhdr.adapterLuid = g_r.luid;
    for (int i = 0; i < kSlots; ++i) {
      g_mhdr.textureHandle[i] = reinterpret_cast<uint64_t>(g_r.mtexH[i]);
      g_mhdr.classTextureHandle[i] = reinterpret_cast<uint64_t>(g_r.ctexH[i]);
    }
    g_mhdr.classFormat = g_r.haveClass ? cmv::format_r8_uint : 0u;
    g_mhdr.fenceHandle = reinterpret_cast<uint64_t>(g_r.fenceH);
    for (cmv::MotionSlot& s : g_mhdr.slot) s = cmv::MotionSlot{};
    PublishMotionHeader();
  }
  if (depth) {  // R69: published first (a reader that finds the new mask generation finds this one too)
    g_dhdr.generation = g_gen;
    g_dhdr.flags = cdp::header_flags::active |
                   (g_r.fenceH ? cdp::header_flags::fence | cdp::header_flags::shares_mask_fence : 0u);
    g_dhdr.atlasW = L.atlasW;
    g_dhdr.atlasH = L.atlasH;
    g_dhdr.adapterLuid = g_r.luid;
    for (int i = 0; i < kSlots; ++i) g_dhdr.textureHandle[i] = reinterpret_cast<uint64_t>(g_r.dtexH[i]);
    g_dhdr.fenceHandle = reinterpret_cast<uint64_t>(g_r.fenceH);
    for (cdp::DepthSlot& s : g_dhdr.slot) s = cdp::DepthSlot{};
    PublishDepthHeader();
  }
  PublishHeader();
}

// ---------------------------------------------------------------------------
// The frame (render thread)
// ---------------------------------------------------------------------------

// R69: what the commit needs to derive a view's DLSS jitter (this frame's gProj on the CPU, the viewport).
struct ProjNote {
  bool ok = false;
  double ox = 0, oy = 0, p00 = 0, p11 = 0, vw = 0, vh = 0;
};
struct Frame {
  bool begun = false, skipped = false;
  bool depth = false;  // R69: this frame writes the depth companion (both heartbeats fresh, depth textures made)
  bool motion = false;  // R70: this frame writes the motion companion
  bool sym = false;     // R95: this frame writes the symbology companion
  uint32_t motionMask = 0;
  cmv::MotionView mview[2] = {};
  bool camOk = false;   // R70: view 0's DCS camera (gViewInv: position row 3, forward row 2), for the cut check
  double camC[3] = {}, camF[3] = {};
  bool camOOk = false;  // R71: view 0's gOrigin (DCS's floating render origin), for the rebase check
  double camO[3] = {};
  int slot = -1, tsFrame = -1;
  uint64_t engineFrame = 0;
  uint32_t viewMask = 0, wantMask = 0, depthMask = 0;
  cm::MaskView view[4] = {};
  cdp::DepthView dview[4] = {};  // R69
  ProjNote proj[4] = {};         // R69
  double cpuUs = 0;
  uint32_t ovlDone = 0, ovlPre = 0, ovlMask = 0;  // R74b: overlay pass seen / pre signature taken / 128 applied, per view
};

// R69: DCS's submitted frame as the xrEndFrame hook read it (XrFrameEndInfo and its first projection layer).
struct XrFrameInfo {
  int64_t displayTime = 0;
  bool poses = false;  // space and the views below were read
  uint64_t space = 0;
  uint32_t viewCount = 0;
  float q[4][4] = {}, p[4][3] = {}, fov[4][4] = {};
};
Frame g_f;
#include "cockpit_time.h"  // R74a: content time per slot (xrLocateViews hook, model time)
#include "cockpit_overlay.h"  // R74b: mask v1.1, 128 = overlay symbology over the world
#include "symbology.h"  // R95: the symbology emission companion (windows, accumulation, the second tone map, commit)
// R70: what the next motion commit compares against (render thread): the previous committed motion slot.
struct MotionPrev {
  bool valid = false;
  uint64_t seq = 0, engineFrame = 0;
  int64_t displayTime = 0;
  uint32_t gen = 0;
  uint64_t cuts = 0;
  float jit[2][2] = {};
  bool jitOk[2] = {};
  bool camOk = false;
  double camC[3] = {}, camF[3] = {};
  bool camOOk = false;  // R71
  double camO[3] = {};
  bool poseOk[2] = {};
  float q[2][4] = {}, p[2][3] = {}, fov[2][4] = {};
};
MotionPrev g_mPrev;
uint32_t g_srcW[4] = {}, g_srcH[4] = {};  // the views' source sizes as last seen
bool g_relayout = false;
int g_tsNext = 0;
Run g_pend[4];
bool g_hasPend[4] = {};
ID3D11RenderTargetView* g_pendMv[4] = {};  // R70: the pending run's MV target (reference)

// The configured views the previous frame had (two HMD views: the periphery pair only).
uint32_t EffectiveViews() {
  const int n = g_s.lastHmdCount;
  return g_views.load(std::memory_order_relaxed) & (n == 4 ? 15u : n == 2 ? 3u : 0u);
}

bool HasPending() { return g_hasPend[0] || g_hasPend[1] || g_hasPend[2] || g_hasPend[3]; }

void DropPending() {
  for (int i = 0; i < 4; ++i) {
    if (g_hasPend[i]) {
      ReleaseRun(g_pend[i]);
      g_hasPend[i] = false;
    }
    RelP(g_pendMv[i]);  // R70
  }
}

// CloseRun: HMD view idx's G-buffer run (hmdCount HMD views in the previous frame).
void NoteRun(const Run& r, int idx, int hmdCount, ID3D11RenderTargetView* mvRtv = nullptr) {
  if (g_idle || !Active() || idx < 0 || idx > 3 || !(g_views.load(std::memory_order_relaxed) & (1u << idx))) return;
  if ((hmdCount != 2 && hmdCount != 4) || idx >= hmdCount || g_hmdOffset.load() != 0) {
    g_skipIdentity++;
    return;
  }
  if (g_hasPend[idx]) ReleaseRun(g_pend[idx]);
  g_pend[idx] = r;
  if (r.dsv) r.dsv->AddRef();
  if (r.cb) r.cb->AddRef();
  g_hasPend[idx] = true;
  RelP(g_pendMv[idx]);  // R70: the run's MV target, for the motion copy of views 0 / 1
  if (mvRtv && idx < 2 && MotionWanted()) {
    g_pendMv[idx] = mvRtv;
    mvRtv->AddRef();
  }
}

// GPU timing of older frames (DONOTFLUSH; never waits).
void PollTiming() {
  ID3D11DeviceContext* c = g_s.ctx;
  if (!c) return;
  for (int f = 0; f < kTsFrames; ++f) {
    if (!g_r.tsPending[f] || !g_r.dj[f]) continue;
    D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj{};
    if (c->GetData(g_r.dj[f], &dj, sizeof(dj), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) continue;
    double us = 0;
    bool ok = !dj.Disjoint && dj.Frequency > 0;
    for (int v = 0; v < 4 && ok; ++v) {
      if (!g_r.tsUsed[f][v]) continue;
      UINT64 a = 0, b = 0;
      if (c->GetData(g_r.ts[f][v][0], &a, sizeof(a), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK ||
          c->GetData(g_r.ts[f][v][1], &b, sizeof(b), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) {
        ok = false;
        break;
      }
      us += (b >= a ? b - a : 0) * 1e6 / static_cast<double>(dj.Frequency);
    }
    g_r.tsPending[f] = false;
    for (bool& u : g_r.tsUsed[f]) u = false;
    if (!ok) continue;
    std::lock_guard<std::mutex> lock(g_statMutex);
    g_gpuUsSum += us;
    g_gpuUsMax = std::max(g_gpuUsMax, us);
    ++g_gpuN;
  }
}

// The frame's slot, at its first capture.
bool BeginFrame() {
  if (g_f.begun) return !g_f.skipped;
  g_f = Frame{};
  g_f.begun = true;
  g_f.engineFrame = g_s.frame;
  g_f.wantMask = EffectiveViews();
  g_f.depth = !g_dIdle && DepthWanted() && g_r.haveDepth && g_dview.load() != nullptr;  // R69
  g_f.motion = !g_mIdle && MotionWanted() && g_r.haveMotion && g_mview.load() != nullptr;  // R70
  g_f.sym = !g_sIdle && SymWanted() && g_r.haveSym && g_sview.load() != nullptr;  // R95
  if (const int note = g_symVramNote.exchange(0)) {  // R97 (Dev): DXGI's local usage when the export turned on / off
    const int64_t u = LocalUsageBytes(g_r.dev);
    Log("cockpit mask: SymbologyExport %s; DXGI local usage %.1f MB (process-wide; the set is made / held by the next "
        "install)",
        note == 1 ? "on" : "off", u >= 0 ? u / 1e6 : -1.0);
  }
  sym::FrameBegin();
  uint64_t seqs[kSlots];
  for (int i = 0; i < kSlots; ++i) seqs[i] = g_hdr.slot[i].maskSeq;
  const uint64_t released = g_r.rel ? g_r.rel->GetCompletedValue() : 0;
  g_f.slot = PickSlot(seqs, released == UINT64_MAX ? 0 : released);  // UINT64_MAX: device removed
  if (g_f.slot < 0) {
    g_f.skipped = true;
    g_skipBusy++;
    return false;
  }
  const int f = g_tsNext;
  if (!g_r.tsPending[f] && g_r.dj[f]) {
    g_f.tsFrame = f;
    g_tsNext = (g_tsNext + 1) % kTsFrames;
    g_s.ctx->Begin(g_r.dj[f]);
  }
  return true;
}

// Samples pending view idx into the frame's slot. point: 0 WaterSeaPass end, 1 GBufferLightingPass begin, 2 a
// Transparent* pass begin, 3 another flush point.
// R70: DCS's MV target as an SRV (cached per resource); null when it is not a 2D texture with a shader-resource bind.
ID3D11ShaderResourceView* MvSrvOf(ID3D11RenderTargetView* rtv, UINT& w, UINT& h, bool& rgba) {
  ID3D11Resource* res = nullptr;
  rtv->GetResource(&res);
  if (!res) return nullptr;
  static uint64_t useClock = 0;
  ++useClock;
  for (auto& m : g_r.mvSrv)
    if (m.res == res && m.srv) {
      res->Release();
      m.used = useClock;
      w = m.w;
      h = m.h;
      rgba = m.rgba;
      return m.srv;
    }
  D3D11_RENDER_TARGET_VIEW_DESC rd{};
  rtv->GetDesc(&rd);
  ID3D11Texture2D* t = nullptr;
  D3D11_TEXTURE2D_DESC td{};
  if (FAILED(res->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&t))) || !t) {
    res->Release();
    return nullptr;
  }
  t->GetDesc(&td);
  t->Release();
  if (!(td.BindFlags & D3D11_BIND_SHADER_RESOURCE) || td.SampleDesc.Count != 1) {
    res->Release();
    return nullptr;
  }
  D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
  sd.Format = rd.Format;
  sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
  sd.Texture2D.MostDetailedMip = 0;
  sd.Texture2D.MipLevels = 1;
  ID3D11ShaderResourceView* srv = nullptr;
  if (FAILED(g_r.dev->CreateShaderResourceView(res, &sd, &srv)) || !srv) {
    res->Release();
    return nullptr;
  }
  auto* slot = &g_r.mvSrv[0];
  for (auto& m : g_r.mvSrv)
    if (m.used < slot->used) slot = &m;
  RelP(slot->srv);
  RelP(slot->res);
  slot->res = res;  // the reference from GetResource
  slot->srv = srv;
  slot->w = td.Width;
  slot->h = td.Height;
  slot->rgba = rd.Format == DXGI_FORMAT_R16G16B16A16_FLOAT;
  slot->used = useClock;
  w = slot->w;
  h = slot->h;
  rgba = slot->rgba;
  return srv;
}

void Capture(const Run& r, int idx, int point, ID3D11RenderTargetView* mvRtv = nullptr) {
  State& s = g_s;
  if (!Active() || !s.ctx || !s.dev) return;
  LARGE_INTEGER t0, t1;
  QueryPerformanceCounter(&t0);
  if (!EnsureBase(s.dev, s.ctx)) {
    g_skipNoRes++;
    return;
  }
  if (r.variant < 0 || r.texFmt == DXGI_FORMAT_UNKNOWN || !r.tex) {
    g_skipFormat++;
    return;
  }
  const uint32_t vx = static_cast<uint32_t>(std::max(0.0f, r.vp.TopLeftX)),
                 vy = static_cast<uint32_t>(std::max(0.0f, r.vp.TopLeftY));
  const uint32_t vw = static_cast<uint32_t>(r.vp.Width), vh = static_cast<uint32_t>(r.vp.Height);
  if (!vw || !vh) {
    g_skipFormat++;
    return;
  }
  // The learnt size; a change re-lays the atlas at the commit.
  if (g_srcW[idx] != vw || g_srcH[idx] != vh) {
    g_srcW[idx] = vw;
    g_srcH[idx] = vh;
    g_relayout = true;
  }
  const int cfgDiv = g_div.load(std::memory_order_relaxed) > 0 ? ClampDiv(g_div.load(std::memory_order_relaxed)) : 0;
  const uint32_t views = EffectiveViews();
  // R85: a held set is not written before Relayout republished it; a companion of the set not wanted now is kept.
  if (g_held || !g_r.haveLayout || g_r.layout.div != cfgDiv || g_r.layout.views != views || g_r.layout.srcW[idx] != vw ||
      g_r.layout.srcH[idx] != vh || (DepthWanted() && g_dview.load() != nullptr && !g_r.haveDepth) ||
      (MotionWanted() && g_mview.load() != nullptr && (views & 3u) && !g_r.haveMotion) ||
      (SymWanted() && g_sview.load() != nullptr &&
       (!g_r.haveSym || g_r.sCap[0] != sym::g_capPeri.load(std::memory_order_relaxed) ||
        g_r.sCap[1] != sym::g_capFocus.load(std::memory_order_relaxed)))) {  // R95 (capacities: tests)
    g_relayout = true;
    g_skipSize++;
    return;
  }
  // The rect's FOV: this pass's gProj on the CPU, else the Perception layer's view FOV.
  const Layout& L = g_r.layout;
  const int div = L.dv[idx];
  cm::MaskView mv{};
  const Viewport vpd{r.vp.TopLeftX, r.vp.TopLeftY, r.vp.Width, r.vp.Height, r.vp.MinDepth, r.vp.MaxDepth};
  bool fov = false;
  if (r.cpuProj) fov = RectFov(ParseProj(r.proj), vpd, L.w[idx], L.h[idx], div, mv.fovTan);
  if (!fov) {
    if (void* gv = g_gazeView.load()) {
      GazeOut g{};
      if (SeqRead(gv, g) && g.viewCount > static_cast<uint32_t>(idx) &&
          RectFovFromFov(g.fovTan[idx], r.vp.Width, r.vp.Height, L.w[idx], L.h[idx], div, mv.fovTan)) {
        fov = true;
        g_fovGaze++;
      }
    }
  }
  if (!fov) {
    g_skipNoFov++;
    return;
  }
  const SrvEntry* se = FindSrv(r);
  if (!se) {
    g_skipNoSrv++;
    return;
  }
  if (!BeginFrame()) return;
  const float nearM = std::max(0.0f, g_nearM.load(std::memory_order_relaxed));
  const int dilate = std::clamp(g_dilate.load(std::memory_order_relaxed), 0, kMaxDilate);
  const bool nearTest = nearM > 0 && r.cb != nullptr;
  if (!se->stencil) g_noStencil++;
  if (!se->stencil && !nearTest) {
    g_skipFormat++;
    return;
  }
  ID3D11DeviceContext* c = s.ctx;
  D3D11_MAPPED_SUBRESOURCE m{};
  if (FAILED(c->Map(g_r.params, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) return;
  MaskParams p{};
  p.srcOrg[0] = static_cast<int32_t>(vx);
  p.srcOrg[1] = static_cast<int32_t>(vy);
  p.srcEnd[0] = static_cast<int32_t>(std::min(vx + vw, r.texW));
  p.srcEnd[1] = static_cast<int32_t>(std::min(vy + vh, r.texH));
  p.dstOrg[0] = static_cast<int32_t>(L.x[idx]);
  p.dstOrg[1] = 0;
  p.dstSize[0] = static_cast<int32_t>(L.w[idx]);
  p.dstSize[1] = static_cast<int32_t>(L.h[idx]);
  p.div = div;
  p.apron = dilate * div;
  p.flags = (se->stencil ? 1u : 0u) | (nearTest ? 2u : 0u);
  p.nearM = nearM;
  p.minD = r.vp.MinDepth;
  p.maxD = r.vp.MaxDepth;
  memcpy(m.pData, &p, sizeof(p));
  c->Unmap(g_r.params, 0);
  ID3D11ShaderResourceView* srvs[2] = {se->srv, se->stencil};
  const bool check = g_verify.load(std::memory_order_relaxed) != 0;
  Bind before;
  if (check) before = ReadBind(c, s.ctx1);
  const int tf = g_f.tsFrame;
  if (tf >= 0) c->End(g_r.ts[tf][idx][0]);
  // R69: the depth variant (second UAV) when this frame writes depth and the view's cPerView is known (gProj).
  const bool depthView = g_f.depth && r.cb != nullptr && g_r.duav[g_f.slot] != nullptr;
  if (g_f.depth && !r.cb) g_dNoCb++;
  RunCs(r, g_r.params, g_r.cs[r.variant + (depthView ? 4 : 0)], srvs, g_r.uav[g_f.slot], (L.w[idx] + 7) / 8,
        (L.h[idx] + 7) / 8, depthView ? g_r.duav[g_f.slot] : nullptr);
  // R70: the motion copy of a periphery view (render resolution, verbatim) and its class byte.
  bool motionView = false;
  bool motionRgba = false;
  if (g_f.motion && idx < 2 && g_r.muav[g_f.slot] && g_r.mw[idx] == vw && g_r.mh[idx] == vh) {
    UINT tw = 0, th = 0;
    ID3D11ShaderResourceView* mvSrv = mvRtv ? MvSrvOf(mvRtv, tw, th, motionRgba) : nullptr;
    if (!mvRtv) {
      g_mNoMv++;
    } else if (!mvSrv) {
      g_mNoSrv++;
    } else if (tw < vx + vw || th < vy + vh) {
      g_mBadFmt++;
    } else {
      D3D11_MAPPED_SUBRESOURCE mm{};
      if (SUCCEEDED(c->Map(g_r.params, 0, D3D11_MAP_WRITE_DISCARD, 0, &mm))) {
        MaskParams q = p;
        q.dstOrg[0] = static_cast<int32_t>(g_r.mx[idx]);
        q.dstOrg[1] = 0;
        q.dstSize[0] = static_cast<int32_t>(vw);
        q.dstSize[1] = static_cast<int32_t>(vh);
        q.srcEnd[0] = static_cast<int32_t>(std::min({vx + vw, r.texW, tw}));
        q.srcEnd[1] = static_cast<int32_t>(std::min({vy + vh, r.texH, th}));
        q.div = 1;
        q.apron = 0;
        const bool cls = g_r.haveClass && g_r.cuav[g_f.slot] != nullptr;
        q.flags = (se->stencil ? 1u : 0u) | (r.cb ? 2u : 0u) | (cls ? 4u : 0u);
        memcpy(mm.pData, &q, sizeof(q));
        c->Unmap(g_r.params, 0);
        RunCs(r, g_r.params, g_r.mcs[r.variant], srvs, g_r.muav[g_f.slot], (vw + 7) / 8, (vh + 7) / 8,
              cls ? g_r.cuav[g_f.slot] : nullptr, mvSrv);
        motionView = true;
      }
    }
  }
  if (tf >= 0) {
    c->End(g_r.ts[tf][idx][1]);
    g_r.tsUsed[tf][idx] = true;
  }
  if (check) {
    g_stateChecks++;
    if (!(ReadBind(c, s.ctx1) == before)) g_stateDiffs++;
  }
  mv.flags = cm::view_flags::valid | (point == 0 ? cm::view_flags::after_water : 0) |
             (se->stencil ? cm::view_flags::stencil : 0) | (nearTest ? cm::view_flags::near_depth : 0) |
             (idx >= 2 ? cm::view_flags::focus : 0);
  mv.x = L.x[idx];
  mv.y = 0;
  mv.w = L.w[idx];
  mv.h = L.h[idx];
  mv.srcW = vw;
  mv.srcH = vh;
  mv.divisor = static_cast<uint32_t>(div);
  mv.nearM = nearTest ? nearM : 0.0f;
  mv.dilate = static_cast<uint32_t>(dilate);
  g_f.view[idx] = mv;
  g_f.viewMask |= 1u << idx;
  if (depthView) {  // R69: the depth view record (pose, xrFov and jitter are added at the commit)
    cdp::DepthView dv{};
    dv.flags = cdp::view_flags::valid | cdp::view_flags::same_grid_as_mask |
               (point == 0 ? cdp::view_flags::after_water : 0u) | (idx >= 2 ? cdp::view_flags::focus : 0u);
    dv.x = mv.x;
    dv.y = mv.y;
    dv.w = mv.w;
    dv.h = mv.h;
    dv.srcW = mv.srcW;
    dv.srcH = mv.srcH;
    dv.divisor = mv.divisor;
    memcpy(dv.fovTan, mv.fovTan, sizeof(dv.fovTan));
    dv.nearM = mv.nearM;
    dv.boostShiftM = NAN;
    dv.jitterPx[0] = dv.jitterPx[1] = NAN;
    // The dual camera (cockpit_ipd.h): this view's frame shift, fixed at its cockpit G-buffer upload (before now).
    if (ckipd::g_on.load(std::memory_order_relaxed)) {
      const ckipd::Frame& cf = ckipd::g_f;
      FrameBoost(cf, idx, &dv.boostShiftM, &dv.flags);  // R77: the record's boost, not the applied shift
      const ckipd::ViewFrame& vf = cf.v[idx];
      if (!cf.observe && vf.patched && vf.dwReady &&
          (vf.dw[0] != 0 || vf.dw[1] != 0 || vf.dw[2] != 0)) {
        WorldMinusCockpit(vf.dw, vf.R, ckipd::g_match.K(), dv.worldMinusCockpitM);
        dv.flags |= cdp::view_flags::dual_camera;
        g_dDual++;
      }
    }
    g_f.dview[idx] = dv;
    g_f.depthMask |= 1u << idx;
    g_dViews++;
  }
  {  // R69 / R70: what the commit needs for the jitter (depth and motion)
    ProjNote& pn = g_f.proj[idx];
    pn = ProjNote{};
    if (r.cpuProj) {
      const Proj pp = ParseProj(r.proj);
      if (pp.ok) {
        pn.ok = true;
        pn.ox = pp.ox;
        pn.oy = pp.oy;
        pn.p00 = pp.P[0][0];
        pn.p11 = pp.P[1][1];
        pn.vw = r.vp.Width;
        pn.vh = r.vp.Height;
      }
    }
  }
  if (motionView) {  // R70: the motion view record (the jitters are added at the commit)
    cmv::MotionView mrec{};
    mrec.flags = cmv::view_flags::valid | (point == 0 ? cmv::view_flags::after_water : 0u) |
              (motionRgba ? cmv::view_flags::from_rgba16f : 0u) | (se->stencil ? 0u : cmv::view_flags::no_stencil);
    mrec.x = g_r.mx[idx];
    mrec.y = 0;
    mrec.w = vw;
    mrec.h = vh;
    mrec.srcW = vw;
    mrec.srcH = vh;
    if (!(r.cpuProj && RectFov(ParseProj(r.proj), vpd, vw, vh, 1, mrec.fovTan)))
      memcpy(mrec.fovTan, mv.fovTan, sizeof(mrec.fovTan));  // GazeOut path: the mask rect's (its last partial block
                                                            // included); its jitter is unknown, so the slot is a reset
    mrec.jitterCurPx[0] = mrec.jitterCurPx[1] = mrec.jitterPrevPx[0] = mrec.jitterPrevPx[1] = NAN;
    g_f.mview[idx] = mrec;
    g_f.motionMask |= 1u << idx;
    g_mViews++;
  }
  if (idx == 0 && r.cpuProj) {  // R70: view 0's DCS camera for the cut check (gViewInv row 3 position, row 2 forward)
    g_f.camOk = true;
    for (int k = 0; k < 3; ++k) {
      g_f.camC[k] = r.viewInv[12 + k];
      g_f.camF[k] = r.viewInv[8 + k];
      g_f.camO[k] = r.origin[k];
    }
    g_f.camOOk = r.originOk;
  }
  g_captures++;
  (point == 0 ? g_afterWater : point == 1 ? g_atLighting : point == 2 ? g_atTransparent : g_atOther)++;
  QueryPerformanceCounter(&t1);
  g_f.cpuUs += (t1.QuadPart - t0.QuadPart) * g_qpcToUs;
}

// Flushes every pending view (point as Capture).
void Flush(int point) {
  for (int i = 0; i < 4; ++i)
    if (g_hasPend[i]) {
      g_hasPend[i] = false;
      Capture(g_pend[i], i, point, g_pendMv[i]);
      ReleaseRun(g_pend[i]);
      RelP(g_pendMv[i]);  // R70
    }
}

const char* HoldBlocker();  // (below)
// R88: the flight gate holds the set (12. at the top): the blocks stay mapped, their records go inactive with the same
// generation and handles (slots invalid, maskSeq kept). Render thread, at a relayout; never with a hold blocker.
void MapHold(const Layout& L) {
  g_held = true;
  g_heldForMap = true;
  g_mapHolds++;
  g_mPrev.valid = false;  // R70: the first motion commit after it is a reset
  g_hdr.flags &= ~cm::header_flags::active;
  for (cm::MaskSlot& sl : g_hdr.slot) {
    const uint64_t k = sl.maskSeq;
    sl = cm::MaskSlot{};
    sl.maskSeq = k;
  }
  if (g_sview.load()) {  // R95
    g_shdr.flags &= ~csy::header_flags::active;
    for (csy::EmissionSlot& sl : g_shdr.slot) sl = csy::EmissionSlot{};
    PublishSymHeader();
  }
  if (g_mview.load()) {
    g_mhdr.flags &= ~cmv::header_flags::active;
    for (cmv::MotionSlot& sl : g_mhdr.slot) sl = cmv::MotionSlot{};
    for (cmv::SlotTime& t : g_mhdr.slotTime) t = cmv::SlotTime{};
    PublishMotionHeader();
  }
  if (g_dview.load()) {
    g_dhdr.flags &= ~cdp::header_flags::active;
    for (cdp::DepthSlot& sl : g_dhdr.slot) sl = cdp::DepthSlot{};
    for (cdp::SlotTime& t : g_dhdr.slotTime) t = cdp::SlotTime{};
    PublishDepthHeader();
  }
  PublishHeader();
  Log("cockpit mask: no flight frame (no Quad Views world culling: F10 map, loading or menu) and another layout (atlas "
      "%ux%u, views 0x%x: %ux%u %ux%u %ux%u %ux%u): shared set of generation %u held, inactive, the same textures, "
      "fences and NT handles; re-checked every frame (R88)",
      L.atlasW, L.atlasH, L.views, L.srcW[0], L.srcH[0], L.srcW[1], L.srcH[1], L.srcW[2], L.srcH[2], L.srcW[3],
      L.srcH[3], g_gen);
}

// R91: the made set replaces the old one (render thread, at a commit): the old set released (its handles buried), the
// new one cleared and published under a new generation. What CreateAtlas did after its creates (and the relayout's
// release accounting and log lines), now at the swap. b's objects move into g_r (b empty afterwards).
void SwapIn(Built& b) {
  const Layout L = b.key.L;
  if (g_held) {
    g_heldReleases++;
    if (g_heldForMap) g_mapRelayouts++;  // R88: flight again with another layout
    Log("cockpit mask: held shared set of generation %u released (layout change)", g_gen);
  }
  g_relayouts++;
  g_mPrev.valid = false;  // R70
  ReleaseAtlas();  // the old set (R85: a held one too); its handles buried
  if (!b.ok) {
    ReleaseBuilt(b);
    g_buildsFailed++;
    g_held = false;
    g_heldForMap = false;
    // R86: the released set's handles (buried) leave the records, and they carry a new generation (R76's record):
    // before, flags 0 went out under the old generation with the old texture handles, which a reader keeping its set
    // by generation (OFXR 0044) could take for the held set.
    BumpGen();
    RetireUnmapped();  // R85
    RetireSymRec();  // R95
    PublishSymHeader();
    RetireMotionRec();  // R70
    PublishMotionHeader();
    RetireDepthRec();  // R69
    PublishDepthHeader();
    g_hdr.generation = g_gen;
    g_hdr.flags = 0;
    for (auto& h : g_hdr.textureHandle) h = 0;
    g_hdr.fenceHandle = g_hdr.releaseFenceHandle = 0;
    for (cm::MaskSlot& ms : g_hdr.slot) {
      const uint64_t k = ms.maskSeq;
      ms = cm::MaskSlot{};
      ms.maskSeq = k;
    }
    PublishHeader();
    LatchFault();  // R76 (HB-2)
    Log("cockpit mask: shared R8 textures (%ux%u, NT handles) could not be created; export off for this load",
        L.atlasW, L.atlasH);
    return;
  }
  ID3D11DeviceContext* ctx = g_s.ctx;
  const float zero[4] = {};
  const UINT zu[4] = {};
  for (int i = 0; i < kSlots; ++i) {
    g_r.tex[i] = b.tex[i];
    g_r.uav[i] = b.uav[i];
    g_r.texH[i] = b.texH[i];
    ctx->ClearUnorderedAccessViewFloat(g_r.uav[i], zero);
    if (b.haveDepth) {
      g_r.dtex[i] = b.dtex[i];
      g_r.duav[i] = b.duav[i];
      g_r.dtexH[i] = b.dtexH[i];
      ctx->ClearUnorderedAccessViewFloat(g_r.duav[i], zero);
    }
    if (b.haveMotion) {
      g_r.mtex[i] = b.mtex[i];
      g_r.muav[i] = b.muav[i];
      g_r.mtexH[i] = b.mtexH[i];
      ctx->ClearUnorderedAccessViewFloat(g_r.muav[i], zero);
      if (b.haveClass) {
        g_r.ctex[i] = b.ctex[i];
        g_r.cuav[i] = b.cuav[i];
        g_r.ctexH[i] = b.ctexH[i];
        ctx->ClearUnorderedAccessViewUint(g_r.cuav[i], zu);
      }
    }
    if (b.haveSym) {  // R95
      g_r.stex[i] = b.stex[i];
      g_r.suav[i] = b.suav[i];
      g_r.stexH[i] = b.stexH[i];
      ctx->ClearUnorderedAccessViewFloat(g_r.suav[i], zero);
      g_r.sctex[i] = b.sctex[i];
      g_r.scuav[i] = b.scuav[i];
      g_r.sctexH[i] = b.sctexH[i];
      ctx->ClearUnorderedAccessViewUint(g_r.scuav[i], zu);
    }
  }
  if (b.haveSym) g_sUse0 = b.sUse0, g_sUse1 = b.sUse1;  // R97
  if (b.haveSym) {  // R95: the private textures
    g_r.sPre = b.sPre, g_r.sPreSrv = b.sPreSrv;
    g_r.sAcc = b.sAcc, g_r.sAccSrv = b.sAccSrv, g_r.sAccUav = b.sAccUav;
    g_r.sSig = b.sSig, g_r.sSigUav = b.sSigUav;
    g_r.sCap[0] = b.sCap[0], g_r.sCap[1] = b.sCap[1], g_r.sW = b.sW, g_r.sH = b.sH;
  }
  g_r.haveSym = b.haveSym;
  g_texCreates += static_cast<uint64_t>(b.made);
  g_r.haveDepth = b.haveDepth;
  g_r.haveMotion = b.haveMotion;
  g_r.haveClass = b.haveClass;
  for (int v = 0; v < 2; ++v) {
    g_r.mx[v] = b.mx[v];
    g_r.mw[v] = b.mw[v];
    g_r.mh[v] = b.mh[v];
  }
  g_r.mW = b.mW;
  g_r.mH = b.mH;
  RelP(b.key.dev);
  b = Built{};  // the objects are g_r's now
  g_r.layout = L;
  g_r.haveLayout = true;
  BumpGen();
  g_held = false;  // R85: a held set was released above (ReleaseAtlas: its handles buried)
  g_heldForMap = false;
  PublishSet(g_r.haveDepth, g_r.haveMotion, g_r.haveSym);
  if (!g_r.haveSym && g_sview.load()) {  // R95 (as R86's below)
    RetireSymRec();
    PublishSymHeader();
  }
  // R86: a companion block we map that this set has no atlas for (motion with neither view 0 nor 1): R76's record of
  // this generation, not the released set's handles.
  if (!g_r.haveMotion && g_mview.load()) {
    RetireMotionRec();
    PublishMotionHeader();
  }
  if (!g_r.haveDepth && g_dview.load()) {
    RetireDepthRec();
    PublishDepthHeader();
  }
  RetireUnmapped();  // R85: the old set's companion whose block is not mapped (its export off) gets R76's record
  Log("cockpit mask: atlas %ux%u (divisor %d%s, views 0x%x: %ux%u /%d %ux%u /%d %ux%u /%d %ux%u /%d), generation %u, "
      "fence %s, release fence %s%s",
      L.atlasW, L.atlasH, L.div, L.div ? "" : " = auto", L.views, L.w[0], L.h[0], L.dv[0], L.w[1], L.h[1], L.dv[1],
      L.w[2], L.h[2], L.dv[2], L.w[3], L.h[3], L.dv[3], g_gen,
      g_r.fenceH ? "shared" : "none", g_r.relH ? "shared" : "none",
      g_r.haveDepth ? "; depth companion R16G16_FLOAT atlas of the same size" : "");
  if (g_r.haveMotion)
    Log("cockpit mask: motion companion atlas %ux%u R16G16_FLOAT (views 0 / 1 at render resolution: %ux%u at %u, %ux%u at %u)%s",
        g_r.mW, g_r.mH, g_r.mw[0], g_r.mh[0], g_r.mx[0], g_r.mw[1], g_r.mh[1], g_r.mx[1],
        g_r.haveClass ? " + class atlas R8_UINT" : "");
  if (g_r.haveSym) {  // R95; R97: its VRAM (R96 measured ON - OFF +591 ... +804 MB for us and OFXR together)
    const sym::Vram vr = sym::VramOf(g_r.sW, g_r.sH, g_r.sCap[0], g_r.sCap[1]);
    Log("cockpit mask: symbology companion atlas %ux%u RGBA16F + class R8_UINT (capacities periphery %u, focus %u); "
        "VRAM: shared %.1f MB (%d slots x (E %.1f + class %.1f)), private %.1f MB (the run-start snapshot %ux%u "
        "RGBA16F %.1f, the accumulator RGBA32F %.1f, the scan signatures %.1f; one each, reused view by view), "
        "statistics and read-back %.3f MB: %.1f MB in all",
        g_r.sW, g_r.sH, g_r.sCap[0], g_r.sCap[1], vr.shared, kSlots, vr.e, vr.cls, vr.priv, vr.snap, vr.snap, vr.pre,
        vr.acc, vr.sig, vr.stats, vr.total);
    // R97 (Dev): the allocation estimates (64 KiB placement) and DXGI's local usage around the builder's symbology
    // textures (process-wide: other allocations in that interval count too).
    const double MB = 1e6;
    const uint64_t eA = TexAlloc64k(g_r.sW, g_r.sH, 8), cA = TexAlloc64k(g_r.sW, g_r.sH, 1);
    const uint32_t sn = std::max(g_r.sCap[0], g_r.sCap[1]);
    const uint64_t pA = TexAlloc64k(sn, sn, 8), aA = TexAlloc64k(sn, sn, 16), gA = TexAlloc64k(512, 512, 4);
    Log("cockpit mask: symbology VRAM (generation %u): allocations (64 KiB placement estimate; D3D11 has no "
        "GetResourceAllocationInfo) E %llu B x %d, class %llu B x %d, snapshot %llu B, accumulator %llu B, signatures "
        "%llu B: %.1f MB; DXGI local usage %.1f MB before / %.1f MB after the symbology textures (delta %+.1f MB)",
        g_gen, static_cast<unsigned long long>(eA), kSlots, static_cast<unsigned long long>(cA), kSlots,
        static_cast<unsigned long long>(pA), static_cast<unsigned long long>(aA), static_cast<unsigned long long>(gA),
        (double(eA + cA) * kSlots + double(pA + aA + gA)) / MB, g_sUse0 >= 0 ? g_sUse0 / MB : -1.0,
        g_sUse1 >= 0 ? g_sUse1 / MB : -1.0, g_sUse0 >= 0 && g_sUse1 >= 0 ? double(g_sUse1 - g_sUse0) / MB : 0.0);
  }
}

// A new layout from the learnt sizes (render thread, between frames).
void Relayout() {
  State& s = g_s;
  g_relayout = false;
  if (!s.dev || !s.ctx || !EnsureBase(s.dev, s.ctx)) return;
  Layout L;
  if (!MakeLayout(g_srcW, g_srcH, EffectiveViews(), g_div.load(std::memory_order_relaxed), L))
    return;  // a configured view not seen yet: retried at the next commit
  const bool wantDepth = DepthWanted() && g_dview.load() != nullptr;  // R69
  const bool wantMotion = MotionWanted() && g_mview.load() != nullptr && (L.views & 3u);  // R70
  const bool wantSym = SymWanted() && g_sview.load() != nullptr;  // R95
  const uint32_t capP = sym::g_capPeri.load(), capF = sym::g_capFocus.load();
  // R85: the set as it is (held or not) when it has this layout and every companion wanted now; a companion it has
  // that is not wanted now stays in it (its block inactive), so its re-enable needs nothing new either.
  const bool cls = g_motionClass.load(std::memory_order_relaxed);
  const bool clsChange = wantMotion && g_r.haveMotion && g_r.haveClass != cls;
  const bool capChange = wantSym && g_r.haveSym && (g_r.sCap[0] != capP || g_r.sCap[1] != capF);  // R95
  if (g_r.haveLayout && g_r.layout.Same(L) && (!wantDepth || g_r.haveDepth) && (!wantMotion || g_r.haveMotion) &&
      (!wantSym || g_r.haveSym) && !clsChange && !capChange) {
    DiscardBuild();  // R91: back to the set's layout before a requested one was swapped in
    if (g_held) {
      const bool map = g_heldForMap;  // R88: flight frames again with the held layout
      g_held = false;
      g_heldForMap = false;
      (map ? g_mapResumes : g_republished)++;
      g_mPrev.valid = false;  // R70: the first motion commit after it is a reset
      PublishSet(wantDepth && g_r.haveDepth, wantMotion && g_r.haveMotion, wantSym && g_r.haveSym);
      if (map)
        Log("cockpit mask: the held layout again: shared set republished, generation %u, the same textures, fences and "
            "NT handles (atlas %ux%u%s%s; R88)", g_gen, L.atlasW, L.atlasH, wantDepth ? ", depth" : "",
            wantMotion ? ", motion" : "");
      else
        Log("cockpit mask: held shared set republished: generation %u, the same textures, fences and NT handles "
            "(atlas %ux%u%s%s; R85)", g_gen, L.atlasW, L.atlasH, wantDepth ? ", depth" : "", wantMotion ? ", motion" : "");
    }
    return;
  }
  // R88: another layout while no flight frame is drawn: the set is held (made: MapHold once) or, with nothing made,
  // nothing is made; re-checked at every commit (Capture asks for a relayout while the layout differs). A hold blocker
  // (a fault, a removed device) releases as before.
  if (!FlightGateOpen()) {
    const char* blocker = HoldBlocker();
    if (!blocker || strcmp(blocker, "nothing made") == 0) {
      DiscardBuild();
      if (blocker) {
        g_mapWaitFrames++;
      } else {
        if (!g_held) MapHold(L);
        g_mapHeldFrames++;
      }
      return;
    }
  }
  BuildKey key{s.dev, L, wantDepth, wantMotion, wantMotion && cls};
  if (wantSym) key.sym = true, key.sCap[0] = capP, key.sCap[1] = capF;  // R95
  Built b;
  if (g_asyncBuild.load(std::memory_order_relaxed) && g_bRunning.load()) {
    // R91: the builder makes the set; swapped in at the commit that finds it made (Capture skips the views meanwhile:
    // the layout differs, so it asks for this relayout again).
    if (g_bPending && !g_bPendKey.Same(key)) DiscardBuild();
    if (!g_bPending) {
      if (!RequestBuild(key)) {
        g_relayout = true;  // the builder still finishes a discarded set
        return;
      }
      g_bPending = true;
      g_bPendKey = key;
    }
    if (const int waitMs = g_buildWaitMs.load(std::memory_order_relaxed)) WaitBuild(waitMs);
    if (!TakeBuild(b)) {
      g_buildWaits++;
      g_relayout = true;
      return;
    }
    g_bPending = false;
    if (!b.key.Same(key)) {  // (not expected: a request for another key is discarded above)
      ReleaseBuilt(b);
      g_relayout = true;
      return;
    }
  } else {
    b.key = key;
    key.dev->AddRef();
    LARGE_INTEGER q0, q1;
    QueryPerformanceCounter(&q0);
    BuildSet(b);
    QueryPerformanceCounter(&q1);
    NoteBuildTime(q0.QuadPart, q1.QuadPart);
    g_buildTid = GetCurrentThreadId();
    g_buildsInline++;
  }
  SwapIn(b);
}

// ---------------------------------------------------------------------------
// Debug: [Perception] CockpitMaskDump (N committed frames as PGM + text) and CockpitMaskLog (N commit lines)
// ---------------------------------------------------------------------------

std::atomic<int> g_dumpLeft{0}, g_logLeft{0};
std::wstring g_dumpDir;  // config thread (before g_dumpLeft is raised)
// R91 (R90 7.3): a new CockpitMaskDump / CockpitDepthDump / CockpitMotionDump request empties its folder before its
// first file is written (the files are named by maskSeq, which starts again at every load: old ones would mix in).
std::atomic<bool> g_clearMaskDir{false}, g_clearDepthDir{false}, g_clearMotionDir{false};
// The files directly in `dir` (no subfolder, no reparse point followed; config thread). Returns how many went.
size_t ClearDumpDir(const std::wstring& dir) {
  size_t n = 0;
  for (const prune::Entry& e : prune::List(dir + L"\\", L"*", false))
    if (DeleteFileW(e.path.c_str())) ++n;
  if (n) Log("cockpit mask: a new dump: %zu old file(s) removed from %ls", n, dir.c_str());
  return n;
}
struct DumpSlot {
  ID3D11Texture2D* st = nullptr;
  UINT w = 0, h = 0;
  bool busy = false;
  uint64_t at = 0;  // g_commitN at the copy
  cm::MaskSlot meta{};
  uint32_t gen = 0;
};
DumpSlot g_dump[3];
uint64_t g_commitN = 0;
int64_t g_prevDt = 0;
struct DumpFile {
  std::wstring name;
  std::string text;
  std::vector<uint8_t> pgm;
};
std::mutex g_dbgMutex;
std::vector<DumpFile> g_dumpOut;  // render thread fills, Tick writes
std::vector<std::string> g_logOut;

// R69: [Perception] CockpitDepthDump: N committed depth atlases (raw R16G16_FLOAT rows, little-endian, w * 4 bytes per
// row, no padding) as cockpit_depth_<seq>.r16g16f plus a text file of the slot and its view records.
std::atomic<int> g_ddumpLeft{0};
struct DepthDumpSlot {
  ID3D11Texture2D* st = nullptr;
  UINT w = 0, h = 0;
  bool busy = false;
  uint64_t at = 0;
  cdp::DepthSlot meta{};
  uint32_t gen = 0;
};
DepthDumpSlot g_ddump[3];
std::vector<DumpFile> g_ddumpOut;  // under g_dbgMutex
// R70: [Perception] CockpitMotionDump: N committed motion slots: the MV atlas as raw R16G16_FLOAT rows
// (cockpit_motion_<seq>.r16g16f), the class atlas as raw bytes (.r8) and a text file (the slot, its views, and for the
// live checks DCS's submitted pose / FOV of this frame and of the previous committed one per view).
std::atomic<int> g_mdumpLeft{0};
struct MotionDumpSlot {
  ID3D11Texture2D* st = nullptr;
  ID3D11Texture2D* stc = nullptr;
  UINT w = 0, h = 0;
  bool busy = false;
  uint64_t at = 0;
  cmv::MotionSlot meta{};
  std::string poses;
  uint32_t gen = 0;
};
MotionDumpSlot g_mdump[3];
std::vector<DumpFile> g_mdumpOut;  // under g_dbgMutex; DumpFile.pgm holds the MV bytes, .text the record
std::vector<std::vector<uint8_t>> g_mdumpCls;  // the class bytes of g_mdumpOut's entries (same order)

void FgRelease();  // R71 (below)
void ReleaseDumps() {
  FgRelease();  // R71: CockpitFgDump's staging ring
  for (DumpSlot& d : g_dump) {
    RelP(d.st);
    d = DumpSlot{};
  }
  for (DepthDumpSlot& d : g_ddump) {  // R69
    RelP(d.st);
    d = DepthDumpSlot{};
  }
  for (MotionDumpSlot& d : g_mdump) {  // R70
    RelP(d.st);
    RelP(d.stc);
    d = MotionDumpSlot{};
  }
}

std::string DepthViewText(const cdp::DepthSlot& sl) {
  std::string t;
  char b[720];
  for (int v = 0; v < 4; ++v) {
    if (!(sl.viewMask & (1u << v))) continue;
    const cdp::DepthView& m = sl.view[v];
    sprintf_s(b,
              "view %d flags 0x%x rect %u,%u %ux%u src %ux%u div %u near %.2f fov L %+.5f R %+.5f U %+.5f D %+.5f "
              "boost %+.5f wmc %+.5f %+.5f %+.5f jitter %+.4f %+.4f q %+.6f %+.6f %+.6f %+.6f p %+.5f %+.5f %+.5f "
              "xrFov %+.5f %+.5f %+.5f %+.5f\n",
              v, m.flags, m.x, m.y, m.w, m.h, m.srcW, m.srcH, m.divisor, m.nearM, m.fovTan[0], m.fovTan[1], m.fovTan[2],
              m.fovTan[3], m.boostShiftM, m.worldMinusCockpitM[0], m.worldMinusCockpitM[1], m.worldMinusCockpitM[2],
              m.jitterPx[0], m.jitterPx[1], m.poseOrientation[0], m.poseOrientation[1], m.poseOrientation[2],
              m.poseOrientation[3], m.posePosition[0], m.posePosition[1], m.posePosition[2], m.xrFov[0], m.xrFov[1],
              m.xrFov[2], m.xrFov[3]);
    t += b;
  }
  return t;
}

std::string ViewText(const cm::MaskSlot& sl) {
  std::string t;
  char b[320];
  for (int v = 0; v < 4; ++v) {
    if (!(sl.viewMask & (1u << v))) continue;
    const cm::MaskView& m = sl.view[v];
    sprintf_s(b, "view %d flags 0x%x rect %u,%u %ux%u src %ux%u div %u dilate %u near %.2f fov L %+.4f R %+.4f U %+.4f D %+.4f\n",
              v, m.flags, m.x, m.y, m.w, m.h, m.srcW, m.srcH, m.divisor, m.dilate, m.nearM, m.fovTan[0], m.fovTan[1],
              m.fovTan[2], m.fovTan[3]);
    t += b;
  }
  return t;
}

// Staging copies mapped >= 2 commits later (DO_NOT_WAIT; render thread).
void PollDumps() {
  ID3D11DeviceContext* c = g_s.ctx;
  if (!c) return;
  for (DumpSlot& d : g_dump) {
    if (!d.busy || g_commitN < d.at + 2) continue;
    D3D11_MAPPED_SUBRESOURCE m{};
    const HRESULT hr = c->Map(d.st, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m);
    if (hr == DXGI_ERROR_WAS_STILL_DRAWING) continue;
    d.busy = false;
    if (FAILED(hr)) continue;
    DumpFile f;
    char hdr[200];
    sprintf_s(hdr, "P5\n# DcsQvCull cockpit mask seq %llu frame %llu displayTime %lld\n%u %u\n255\n",
              static_cast<unsigned long long>(d.meta.maskSeq), static_cast<unsigned long long>(d.meta.engineFrame),
              static_cast<long long>(d.meta.displayTime), d.w, d.h);
    f.pgm.assign(hdr, hdr + strlen(hdr));
    const uint8_t* b = static_cast<const uint8_t*>(m.pData);
    for (UINT y = 0; y < d.h; ++y)
      f.pgm.insert(f.pgm.end(), b + static_cast<size_t>(y) * m.RowPitch, b + static_cast<size_t>(y) * m.RowPitch + d.w);
    c->Unmap(d.st, 0);
    wchar_t name[96];
    swprintf_s(name, L"cockpit_mask_%06llu", static_cast<unsigned long long>(d.meta.maskSeq));
    f.name = name;
    char line[200];
    sprintf_s(line, "seq %llu engine frame %llu displayTime %lld slot flags 0x%x views 0x%x generation %u\n",
              static_cast<unsigned long long>(d.meta.maskSeq), static_cast<unsigned long long>(d.meta.engineFrame),
              static_cast<long long>(d.meta.displayTime), d.meta.flags, d.meta.viewMask, d.gen);
    f.text = std::string(line) + ViewText(d.meta);
    std::lock_guard<std::mutex> lock(g_dbgMutex);
    if (g_dumpOut.size() < 64) g_dumpOut.push_back(std::move(f));
  }
  for (DepthDumpSlot& d : g_ddump) {  // R69
    if (!d.busy || g_commitN < d.at + 2) continue;
    D3D11_MAPPED_SUBRESOURCE m{};
    const HRESULT hr = c->Map(d.st, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m);
    if (hr == DXGI_ERROR_WAS_STILL_DRAWING) continue;
    d.busy = false;
    if (FAILED(hr)) continue;
    DumpFile f;
    const uint8_t* b = static_cast<const uint8_t*>(m.pData);
    f.pgm.reserve(static_cast<size_t>(d.w) * d.h * 4);
    for (UINT y = 0; y < d.h; ++y)
      f.pgm.insert(f.pgm.end(), b + static_cast<size_t>(y) * m.RowPitch, b + static_cast<size_t>(y) * m.RowPitch + d.w * 4);
    c->Unmap(d.st, 0);
    wchar_t name[96];
    swprintf_s(name, L"cockpit_depth_%06llu", static_cast<unsigned long long>(d.meta.seq));
    f.name = name;
    char line[300];
    sprintf_s(line, "seq %llu engine frame %llu displayTime %lld slot flags 0x%x views 0x%x generation %u atlas %ux%u "
                    "R16G16_FLOAT space 0x%llx\n",
              static_cast<unsigned long long>(d.meta.seq), static_cast<unsigned long long>(d.meta.engineFrame),
              static_cast<long long>(d.meta.displayTime), d.meta.flags, d.meta.viewMask, d.gen, d.w, d.h,
              static_cast<unsigned long long>(d.meta.xrSpace));
    f.text = std::string(line) + DepthViewText(d.meta);
    std::lock_guard<std::mutex> lock(g_dbgMutex);
    if (g_ddumpOut.size() < 64) g_ddumpOut.push_back(std::move(f));
  }
  for (MotionDumpSlot& d : g_mdump) {  // R70
    if (!d.busy || g_commitN < d.at + 2) continue;
    D3D11_MAPPED_SUBRESOURCE m{};
    const HRESULT hr = c->Map(d.st, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m);
    if (hr == DXGI_ERROR_WAS_STILL_DRAWING) continue;
    if (FAILED(hr)) {
      d.busy = false;
      continue;
    }
    DumpFile f;
    const uint8_t* b = static_cast<const uint8_t*>(m.pData);
    for (UINT y = 0; y < d.h; ++y)
      f.pgm.insert(f.pgm.end(), b + static_cast<size_t>(y) * m.RowPitch, b + static_cast<size_t>(y) * m.RowPitch + d.w * 4);
    c->Unmap(d.st, 0);
    std::vector<uint8_t> cls;
    if (d.stc && SUCCEEDED(c->Map(d.stc, 0, D3D11_MAP_READ, 0, &m))) {
      const uint8_t* cb = static_cast<const uint8_t*>(m.pData);
      for (UINT y = 0; y < d.h; ++y)
        cls.insert(cls.end(), cb + static_cast<size_t>(y) * m.RowPitch, cb + static_cast<size_t>(y) * m.RowPitch + d.w);
      c->Unmap(d.stc, 0);
    }
    d.busy = false;
    wchar_t name[96];
    swprintf_s(name, L"cockpit_motion_%06llu", static_cast<unsigned long long>(d.meta.seq));
    f.name = name;
    char line[400];
    sprintf_s(line, "seq %llu engine frame %llu displayTime %lld prevSeq %llu prevDisplayTime %lld slot flags 0x%x views 0x%x "
                    "generation %u atlas %ux%u R16G16_FLOAT class %s\n",
              static_cast<unsigned long long>(d.meta.seq), static_cast<unsigned long long>(d.meta.engineFrame),
              static_cast<long long>(d.meta.displayTime), static_cast<unsigned long long>(d.meta.prevSeq),
              static_cast<long long>(d.meta.prevDisplayTime), d.meta.flags, d.meta.viewMask, d.gen, d.w, d.h,
              cls.empty() ? "none" : "R8_UINT");
    std::string t = line;
    for (int v = 0; v < 2; ++v) {
      if (!(d.meta.viewMask & (1u << v))) continue;
      const cmv::MotionView& mv = d.meta.view[v];
      char b2[400];
      sprintf_s(b2, "view %d flags 0x%x rect %u,%u %ux%u src %ux%u fov L %+.6f R %+.6f U %+.6f D %+.6f jitterCur %+.4f %+.4f "
                    "jitterPrev %+.4f %+.4f\n",
                v, mv.flags, mv.x, mv.y, mv.w, mv.h, mv.srcW, mv.srcH, mv.fovTan[0], mv.fovTan[1], mv.fovTan[2],
                mv.fovTan[3], mv.jitterCurPx[0], mv.jitterCurPx[1], mv.jitterPrevPx[0], mv.jitterPrevPx[1]);
      t += b2;
    }
    f.text = t + d.poses;
    std::lock_guard<std::mutex> lock(g_dbgMutex);
    if (g_mdumpOut.size() < 64) {
      g_mdumpOut.push_back(std::move(f));
      g_mdumpCls.push_back(std::move(cls));
    }
  }
}

// After a commit of slot `slot` (render thread): the log line, the dump copy.
void DebugCommit(int slot, const cm::MaskSlot& sl) {
  ++g_commitN;
  if (g_logLeft.load(std::memory_order_relaxed) > 0) {
    g_logLeft--;
    char line[240];
    sprintf_s(line, "frame: seq %llu engine frame %llu displayTime %lld (+%.3f ms) slot %d flags 0x%x views 0x%x",
              static_cast<unsigned long long>(sl.maskSeq), static_cast<unsigned long long>(sl.engineFrame),
              static_cast<long long>(sl.displayTime),
              g_prevDt && sl.displayTime ? (sl.displayTime - g_prevDt) / 1e6 : 0.0, slot, sl.flags, sl.viewMask);
    std::string text = line;
    if (ctime::g_rec.seq == sl.maskSeq) text += ctime::LogText(sl.qpcCommit);  // R74a
    text += OverlayLogText(sl);  // R74b
    std::lock_guard<std::mutex> lock(g_dbgMutex);
    if (g_logOut.size() < 1024) g_logOut.push_back(text);
  }
  if (sl.displayTime) g_prevDt = sl.displayTime;
  if (g_dumpLeft.load(std::memory_order_relaxed) <= 0 || !g_r.tex[slot]) return;
  DumpSlot* d = nullptr;
  for (DumpSlot& x : g_dump)
    if (!x.busy) {
      d = &x;
      break;
    }
  if (!d) return;
  const Layout& L = g_r.layout;
  if (!d->st || d->w != L.atlasW || d->h != L.atlasH) {
    RelP(d->st);
    D3D11_TEXTURE2D_DESC td{};
    td.Width = L.atlasW;
    td.Height = L.atlasH;
    td.MipLevels = td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_STAGING;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (FAILED(g_r.dev->CreateTexture2D(&td, nullptr, &d->st))) return;
    d->w = L.atlasW;
    d->h = L.atlasH;
  }
  g_s.ctx->CopyResource(d->st, g_r.tex[slot]);
  d->busy = true;
  d->at = g_commitN;
  d->meta = sl;
  d->gen = g_gen;
  g_dumpLeft--;
}

// R69: after a depth commit of slot `slot`: the dump copy.
void DebugCommitDepth(int slot, const cdp::DepthSlot& sl) {
  if (g_ddumpLeft.load(std::memory_order_relaxed) <= 0 || !g_r.dtex[slot]) return;
  DepthDumpSlot* d = nullptr;
  for (DepthDumpSlot& x : g_ddump)
    if (!x.busy) {
      d = &x;
      break;
    }
  if (!d) return;
  const Layout& L = g_r.layout;
  if (!d->st || d->w != L.atlasW || d->h != L.atlasH) {
    RelP(d->st);
    D3D11_TEXTURE2D_DESC td{};
    td.Width = L.atlasW;
    td.Height = L.atlasH;
    td.MipLevels = td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R16G16_FLOAT;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_STAGING;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (FAILED(g_r.dev->CreateTexture2D(&td, nullptr, &d->st))) return;
    d->w = L.atlasW;
    d->h = L.atlasH;
  }
  g_s.ctx->CopyResource(d->st, g_r.dtex[slot]);
  d->busy = true;
  d->at = g_commitN;
  d->meta = sl;
  d->gen = g_gen;
  g_ddumpLeft--;
}

// The camera of view 0 jumped or turned beyond any head or aircraft motion between two frames (R70 reset): more than
// 50 m (2 km/s at 40 fps) or more than 30 degrees. R71: a jump explained by DCS's floating-origin rebase is not a cut.
// gViewInv's position is relative to gOrigin, so a rebase moves it by +-(gOrigin_a - gOrigin_b) while the world camera
// moved normally (live R71: in flight at 200 m/s about 1 frame in 34 was flagged; its MVs were continuous). The jump
// is then the residual after the origin delta (either sign), when both frames' gOrigin are known.
// R76: with both origins known and different, the 50 m limit applies to that residual (the world jump), never to the
// raw relative jump: a teleport that DCS's rebase follows in the same frame (F-key / F11 jump, respawn) leaves the
// relative camera within its rebase cell, so the raw jump can be under 50 m while the world jump is kilometres.
// Either sign is still accepted (the sign is not live-proven): a world jump of exactly -2 x the origin delta passes.
std::atomic<uint64_t> g_mRebaseNoCut{0};  // R71: jumps > 50 m explained by the gOrigin delta (no reset)
bool CameraCut(const MotionPrev& a, const Frame& b, bool* rebased = nullptr) {
  if (rebased) *rebased = false;
  if (!a.camOk || !b.camOk) return false;
  double d[3], od[3], dot = 0, na = 0, nb = 0;
  for (int k = 0; k < 3; ++k) {
    d[k] = b.camC[k] - a.camC[k];
    od[k] = a.camO[k] - b.camO[k];
    dot += a.camF[k] * b.camF[k];
    na += a.camF[k] * a.camF[k];
    nb += b.camF[k] * b.camF[k];
  }
  if (na > 0 && nb > 0 && dot / std::sqrt(na * nb) < 0.866) return true;
  const double lim2 = 50.0 * 50.0;
  const double d2 = d[0] * d[0] + d[1] * d[1] + d[2] * d[2];
  if (a.camOOk && b.camOOk && (od[0] != 0 || od[1] != 0 || od[2] != 0)) {
    for (const double s : {1.0, -1.0}) {
      double e2 = 0;
      for (int k = 0; k < 3; ++k) {
        const double e = d[k] - s * od[k];
        e2 += e * e;
      }
      if (e2 <= lim2) {
        if (rebased) *rebased = d2 > lim2;  // counted only when the raw jump alone would have been a cut
        return false;
      }
    }
    return true;  // R76: the world jump (either sign) is beyond 50 m, whatever the raw jump
  }
  return d2 > lim2;
}

void DebugCommitMotion(int slot, const cmv::MotionSlot& sl, const XrFrameInfo* xr) {
  if (g_mdumpLeft.load(std::memory_order_relaxed) <= 0 || !g_r.mtex[slot]) return;
  MotionDumpSlot* d = nullptr;
  for (MotionDumpSlot& x : g_mdump)
    if (!x.busy) {
      d = &x;
      break;
    }
  if (!d) return;
  if (!d->st || d->w != g_r.mW || d->h != g_r.mH) {
    RelP(d->st);
    RelP(d->stc);
    D3D11_TEXTURE2D_DESC td{};
    td.Width = g_r.mW;
    td.Height = g_r.mH;
    td.MipLevels = td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R16G16_FLOAT;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_STAGING;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (FAILED(g_r.dev->CreateTexture2D(&td, nullptr, &d->st))) return;
    td.Format = DXGI_FORMAT_R8_UINT;
    if (g_r.haveClass && FAILED(g_r.dev->CreateTexture2D(&td, nullptr, &d->stc))) d->stc = nullptr;
    d->w = g_r.mW;
    d->h = g_r.mH;
  }
  g_s.ctx->CopyResource(d->st, g_r.mtex[slot]);
  if (d->stc && g_r.ctex[slot]) g_s.ctx->CopyResource(d->stc, g_r.ctex[slot]);
  d->busy = true;
  d->at = g_commitN;
  d->meta = sl;
  d->gen = g_gen;
  d->poses.clear();
  char b[400];
  for (int v = 0; v < 2; ++v) {
    if (xr && xr->poses && static_cast<uint32_t>(v) < xr->viewCount) {
      sprintf_s(b, "pose %d cur q %+.7f %+.7f %+.7f %+.7f p %+.6f %+.6f %+.6f fov %+.6f %+.6f %+.6f %+.6f\n", v,
                xr->q[v][0], xr->q[v][1], xr->q[v][2], xr->q[v][3], xr->p[v][0], xr->p[v][1], xr->p[v][2],
                xr->fov[v][0], xr->fov[v][1], xr->fov[v][2], xr->fov[v][3]);
      d->poses += b;
    }
    if (g_mPrev.valid && g_mPrev.poseOk[v]) {
      sprintf_s(b, "pose %d prev q %+.7f %+.7f %+.7f %+.7f p %+.6f %+.6f %+.6f fov %+.6f %+.6f %+.6f %+.6f\n", v,
                g_mPrev.q[v][0], g_mPrev.q[v][1], g_mPrev.q[v][2], g_mPrev.q[v][3], g_mPrev.p[v][0], g_mPrev.p[v][1],
                g_mPrev.p[v][2], g_mPrev.fov[v][0], g_mPrev.fov[v][1], g_mPrev.fov[v][2], g_mPrev.fov[v][3]);
      d->poses += b;
    }
  }
  g_mdumpLeft--;
}

// ---------------------------------------------------------------------------
// R71: [Perception] CockpitFgDump=N (dev only, default 0): a frame-generation fixture of N CONSECUTIVE committed DCS
// frames. Per frame, all from the same commit (the same seq, engineFrame and displayTime):
//   fNNN_mv.f16        the motion atlas: DCS's MVs of views 0 / 1 at render resolution, R16G16_FLOAT raw rows
//                      (mvW x mvH x 4 bytes, little-endian, no padding); rects, jitters, prevSeq in the sidecar
//   fNNN_class.r8      the class atlas (R8_UINT, mvW x mvH): stencil composition (bits 0-2) + sky (bit 7)
//   fNNN_depth.f16     the depth atlas (R16G16_FLOAT, atlasW x atlasH x 4: .x nearest cockpit, .y farthest; metres,
//                      65504 = sky), on the mask's grid (divisor in the sidecar)
//   fNNN_color0.rgba8  views 0 / 1 as DCS submitted them to the HMD: the viewport box of the swapchain image DCS's
//   fNNN_color1.rgba8  CopyToHMDPass wrote, read at that pass's end (4 bytes per texel, w x h; the order and format
//                      in the sidecar); the pass's source image when the target is not readable
//   fNNN.json          every header and per-view field: mask, depth and motion slots, DCS's submitted poses / FOVs
// plus index.json (the session: frames, gaps, stalls), in <dev folder>\fg_dump\<date_time>\.
// Gap-free: the session starts at the first commit whose views 0 / 1 both have their colour, then keeps EVERY
// commit. A 16-record staging ring is mapped >= 2 commits later without waiting; when it is full, the oldest record
// is mapped waiting (a stall, counted; never a dropped frame). The bytes stay in memory until Tick writes them (once
// per second). A DCS frame that commits nothing (slot busy, no view) is counted as a gap; the sidecar's seq,
// engineFrame and motion.prevSeq show any break. Needs the mask, depth and motion exports with their readers alive.
// Cost: nothing while N is 0 (one relaxed load per commit and per boundary of fixation depth's probes).
// ---------------------------------------------------------------------------

constexpr int kFgRing = 16;
constexpr int kFgTex = 5;  // 0 mv, 1 class, 2 depth, 3 colour view 0, 4 colour view 1
const wchar_t* const kFgSuffix[kFgTex] = {L"_mv.f16", L"_class.r8", L"_depth.f16", L"_color0.rgba8", L"_color1.rgba8"};
std::atomic<int> g_fgReq{0};       // config thread: frames requested; taken by the render thread at the session start
std::mutex g_fgBaseMutex;
std::wstring g_fgBase;             // <dev folder> (ConfigureFg)
struct FgColour {
  bool ok = false;
  int src = 0;                     // 1 the swapchain image (CopyToHMDPass's target), 2 its source image
  uint64_t frame = 0;              // g_s.frame at the copy
  UINT w = 0, h = 0, texW = 0, texH = 0, boxX = 0, boxY = 0, sub = 0;
  DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN;
};
struct FgTex {
  ID3D11Texture2D* st = nullptr;
  UINT w = 0, h = 0, bpp = 0;
  DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN;
  bool used = false, done = false;
};
struct FgRec {
  FgTex t[kFgTex];
  bool open = false;               // colour copied for `frame`, not committed yet
  bool busy = false;               // committed: waiting for its maps
  uint64_t frame = 0, at = 0;
  int index = 0;
  FgColour col[2];
  std::wstring dir, name;
  std::string json;
  std::vector<uint8_t> bytes[kFgTex];
};
FgRec g_fgRing[kFgRing];
struct FgOut {
  std::wstring dir, name;
  std::string json;
  std::vector<uint8_t> bytes[kFgTex];
  bool end = false;                // the session's end: json holds index.json
};
std::vector<FgOut> g_fgOut;        // under g_dbgMutex; Tick writes
// Render thread: the running session.
bool g_fgActive = false, g_fgEnding = false;
int g_fgLeft = 0, g_fgIndex = 0, g_fgWait = 0, g_fgN = 0;
uint64_t g_fgPrevSeq = 0, g_fgPrevFrame = 0;
uint64_t g_fgGaps = 0, g_fgNoCommit = 0, g_fgStalls = 0, g_fgNoColour = 0, g_fgFirstSeq = 0, g_fgLastSeq = 0;
uint64_t g_fgFromSource = 0;  // colours of committed frames taken from the copy source (fixation_depth.h's fallback)
std::wstring g_fgDir;
std::atomic<uint64_t> g_fgSessions{0}, g_fgFrames{0};  // tests / stats

bool FgColourWanted() {
  return Active() && (g_fgActive || g_fgReq.load(std::memory_order_relaxed) > 0);
}

// JSON helpers: numbers (non-finite as null) and arrays.
void FgJ(std::string& s, const char* key, double v) {
  char b[64];
  if (std::isfinite(v))
    sprintf_s(b, "\"%s\":%.9g,", key, v);
  else
    sprintf_s(b, "\"%s\":null,", key);
  s += b;
}
void FgJu(std::string& s, const char* key, unsigned long long v) {
  char b[64];
  sprintf_s(b, "\"%s\":%llu,", key, v);
  s += b;
}
void FgJi(std::string& s, const char* key, long long v) {
  char b[64];
  sprintf_s(b, "\"%s\":%lld,", key, v);
  s += b;
}
void FgJa(std::string& s, const char* key, const float* v, int n) {
  s += "\"";
  s += key;
  s += "\":[";
  for (int i = 0; i < n; ++i) {
    char b[32];
    if (std::isfinite(v[i]))
      sprintf_s(b, "%.9g", static_cast<double>(v[i]));
    else
      strcpy_s(b, "null");
    s += b;
    s += i + 1 < n ? "," : "";
  }
  s += "],";
}
void FgJs(std::string& s, const char* key, const char* v) {
  s += "\"";
  s += key;
  s += "\":\"";
  s += v;
  s += "\",";
}
// Closes an object / array opened by the caller: drops the trailing comma.
void FgClose(std::string& s, char c) {
  if (!s.empty() && s.back() == ',') s.pop_back();
  s += c;
  s += ',';
}

bool FgStage(FgTex& t, UINT w, UINT h, DXGI_FORMAT fmt, UINT bpp) {
  if (!g_r.dev || !w || !h) return false;
  if (!t.st || t.w != w || t.h != h || t.fmt != fmt) {
    RelP(t.st);
    D3D11_TEXTURE2D_DESC td{};
    td.Width = w;
    td.Height = h;
    td.MipLevels = td.ArraySize = 1;
    td.Format = fmt;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_STAGING;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (FAILED(g_r.dev->CreateTexture2D(&td, nullptr, &t.st))) {
      t = FgTex{};
      return false;
    }
    t.w = w;
    t.h = h;
    t.fmt = fmt;
  }
  t.bpp = bpp;
  t.used = true;
  t.done = false;
  return true;
}

// Maps a committed record's textures into memory (render thread). wait: blocking (the ring is full). True: done.
bool FgMap(FgRec& r, bool wait) {
  ID3D11DeviceContext* c = g_s.ctx;
  if (!c) return false;
  for (int k = 0; k < kFgTex; ++k) {
    FgTex& t = r.t[k];
    if (!t.used || t.done) continue;
    D3D11_MAPPED_SUBRESOURCE m{};
    const HRESULT hr = c->Map(t.st, 0, D3D11_MAP_READ, wait ? 0 : D3D11_MAP_FLAG_DO_NOT_WAIT, &m);
    if (hr == DXGI_ERROR_WAS_STILL_DRAWING) return false;
    t.done = true;
    if (FAILED(hr)) continue;  // written without this file (the sidecar lists the files)
    const size_t row = static_cast<size_t>(t.w) * t.bpp;
    std::vector<uint8_t>& o = r.bytes[k];
    o.resize(row * t.h);
    const uint8_t* b = static_cast<const uint8_t*>(m.pData);
    for (UINT y = 0; y < t.h; ++y) memcpy(o.data() + row * y, b + static_cast<size_t>(y) * m.RowPitch, row);
    c->Unmap(t.st, 0);
  }
  FgOut out;
  out.dir = r.dir;
  out.name = r.name;
  out.json.swap(r.json);
  for (int k = 0; k < kFgTex; ++k) {
    out.bytes[k].swap(r.bytes[k]);
    r.t[k].used = r.t[k].done = false;
  }
  r.busy = false;
  g_fgFrames++;
  std::lock_guard<std::mutex> lock(g_dbgMutex);
  g_fgOut.push_back(std::move(out));
  return true;
}

void FgPushEnd(bool aborted) {
  std::string s = "{";
  FgJi(s, "frames", g_fgIndex);
  FgJi(s, "requested", g_fgN);
  FgJu(s, "firstSeq", g_fgFirstSeq);
  FgJu(s, "lastSeq", g_fgLastSeq);
  FgJu(s, "seqGaps", g_fgGaps);
  FgJu(s, "framesWithoutCommit", g_fgNoCommit);
  FgJu(s, "framesWithoutColour", g_fgNoColour);
  FgJu(s, "coloursFromCopySource", g_fgFromSource);
  FgJu(s, "stalls", g_fgStalls);
  s += aborted ? "\"aborted\":true," : "\"aborted\":false,";
  s += "\"gapFree\":";
  s += !aborted && g_fgGaps == 0 && g_fgNoCommit == 0 ? "true," : "false,";
  FgJs(s, "mvFormat", "R16G16_FLOAT, render pixels, +x right, +y down, current -> previous (jitter-free)");
  FgJs(s, "depthFormat", "R16G16_FLOAT metres along the view axis: .x nearest cockpit (0 none), .y farthest (65504 sky)");
  FgJs(s, "classFormat", "R8_UINT: bits 0-2 stencil composition (0 surface 1 model 2 water 3 foliage 4 grass 5 cockpit "
                         "6 underwater 7 empty), bit 7 sky");
  FgJs(s, "colorFormat", "8-bit 4-channel bytes as submitted to the HMD (channelOrder in each frame's sidecar)");
  FgClose(s, '}');
  s.pop_back();
  FgOut out;
  out.dir = g_fgDir;
  out.end = true;
  out.json = s;
  Log("cockpit mask: frame-generation dump %s: %d of %d frames (seq %llu..%llu), seq gaps %llu, frames without a "
      "commit %llu, without colour %llu, ring stalls %llu, colours from the copy source %llu",
      aborted ? "ABORTED" : "done", g_fgIndex, g_fgN, static_cast<unsigned long long>(g_fgFirstSeq),
      static_cast<unsigned long long>(g_fgLastSeq), static_cast<unsigned long long>(g_fgGaps),
      static_cast<unsigned long long>(g_fgNoCommit), static_cast<unsigned long long>(g_fgNoColour),
      static_cast<unsigned long long>(g_fgStalls), static_cast<unsigned long long>(g_fgFromSource));
  std::lock_guard<std::mutex> lock(g_dbgMutex);
  g_fgOut.push_back(std::move(out));
}

// Committed records whose copies are at least 2 commits old (render thread, each Commit).
void PollFg() {
  for (FgRec& r : g_fgRing)
    if (r.busy && g_commitN >= r.at + 2) FgMap(r, false);
  if (g_fgEnding) {
    for (const FgRec& r : g_fgRing)
      if (r.busy) return;
    g_fgEnding = false;
    FgPushEnd(false);
  }
}

// The record of DCS frame `frame` (render thread): the one its colour opened, or a free one. A full ring maps its
// oldest record waiting (a stall: never a dropped frame).
FgRec* FgRecFor(uint64_t frame) {
  for (FgRec& r : g_fgRing)
    if (r.open && r.frame == frame) return &r;
  for (FgRec& r : g_fgRing)
    if (r.open) {  // a frame that never committed: its colour is dropped
      r.open = false;
      for (FgTex& t : r.t) t.used = t.done = false;
    }
  for (int pass = 0; pass < 2; ++pass) {
    for (FgRec& r : g_fgRing)
      if (!r.open && !r.busy) {
        r.open = true;
        r.frame = frame;
        r.col[0] = r.col[1] = FgColour{};
        for (FgTex& t : r.t) t.used = t.done = false;
        return &r;
      }
    FgRec* oldest = nullptr;
    for (FgRec& r : g_fgRing)
      if (r.busy && (!oldest || r.at < oldest->at)) oldest = &r;
    if (!oldest) return nullptr;
    g_fgStalls++;
    FgMap(*oldest, true);
  }
  return nullptr;
}

bool FgColourFormat(DXGI_FORMAT f, const char** order) {
  switch (f) {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
      if (order) *order = "RGBA";
      return true;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
      if (order) *order = "BGRA";
      return true;
    default:
      return false;
  }
}

// fixation_depth.h, at CopyToHMDPass end of HMD view `view` (0 / 1; render thread): DCS's copy into the swapchain
// image `dst` (subresource dstSub, viewport vp) is in the command stream; the submitted box is copied. `src` (the
// pass's PS SRV 0, the view's image) when the target is not a readable 8-bit 4-channel texture.
void FgNoteColour(int view, ID3D11Resource* dst, UINT dstSub, const D3D11_VIEWPORT& vp, ID3D11Resource* src,
                  UINT srcSub) {
  if (view < 0 || view > 1 || !FgColourWanted() || !g_s.ctx || !g_r.dev) return;
  ID3D11Texture2D* tex = nullptr;
  D3D11_TEXTURE2D_DESC td{};
  int kind = 0;
  UINT sub = 0;
  D3D11_BOX box{};
  if (dst && SUCCEEDED(dst->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&tex))) && tex) {
    tex->GetDesc(&td);
    if (td.SampleDesc.Count == 1 && FgColourFormat(td.Format, nullptr) && vp.Width >= 1 && vp.Height >= 1) {
      kind = 1;
      sub = dstSub;
      const UINT mip = td.MipLevels ? sub % td.MipLevels : 0;
      const UINT mw = std::max(1u, td.Width >> mip), mh = std::max(1u, td.Height >> mip);
      box.left = static_cast<UINT>(std::clamp(vp.TopLeftX, 0.0f, static_cast<float>(mw)));
      box.top = static_cast<UINT>(std::clamp(vp.TopLeftY, 0.0f, static_cast<float>(mh)));
      box.right = static_cast<UINT>(std::clamp(vp.TopLeftX + vp.Width, 0.0f, static_cast<float>(mw)));
      box.bottom = static_cast<UINT>(std::clamp(vp.TopLeftY + vp.Height, 0.0f, static_cast<float>(mh)));
      box.back = 1;
      if (box.right <= box.left || box.bottom <= box.top) kind = 0;
    }
    if (!kind) RelP(tex);
  }
  if (!kind && src && SUCCEEDED(src->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&tex))) && tex) {
    tex->GetDesc(&td);
    if (td.SampleDesc.Count == 1 && FgColourFormat(td.Format, nullptr)) {
      kind = 2;
      sub = srcSub;
      const UINT mip = td.MipLevels ? sub % td.MipLevels : 0;
      box = D3D11_BOX{0, 0, 0, std::max(1u, td.Width >> mip), std::max(1u, td.Height >> mip), 1};
    } else {
      RelP(tex);
    }
  }
  if (!kind || !tex) return;
  FgRec* r = FgRecFor(g_s.frame);
  if (r && FgStage(r->t[3 + view], box.right - box.left, box.bottom - box.top, td.Format, 4)) {
    g_s.ctx->CopySubresourceRegion(r->t[3 + view].st, 0, 0, 0, 0, tex, sub, &box);
    FgColour& c = r->col[view];
    c.ok = true;
    c.src = kind;
    c.frame = g_s.frame;
    c.w = box.right - box.left;
    c.h = box.bottom - box.top;
    c.texW = td.Width;
    c.texH = td.Height;
    c.boxX = box.left;
    c.boxY = box.top;
    c.sub = sub;
    c.fmt = td.Format;
  }
  tex->Release();
}

// The sidecar of one frame (render thread, at its commit).
std::string FgJson(const FgRec& r, const Frame& f, const cm::MaskSlot& sl, const cdp::DepthSlot* ds,
                   const cmv::MotionSlot* ms, const XrFrameInfo* xr, bool gapBefore) {
  std::string s = "{";
  FgJi(s, "index", r.index);
  FgJu(s, "seq", sl.maskSeq);
  FgJu(s, "engineFrame", sl.engineFrame);
  FgJi(s, "displayTime", sl.displayTime);
  FgJi(s, "qpcCommit", sl.qpcCommit);
  FgJu(s, "generation", g_gen);
  s += gapBefore ? "\"gapBefore\":true," : "\"gapBefore\":false,";
  FgJi(s, "slot", f.slot);
  if (ctime::g_rec.seq == sl.maskSeq) {  // R74a: the slot's content time (as written into slotTime)
    s += "\"time\":{";
    FgJu(s, "flags", ctime::g_rec.flags);
    FgJi(s, "poseQpc", ctime::g_rec.poseQpc);
    FgJi(s, "poseReturnQpc", ctime::g_rec.poseReturnQpc);
    FgJi(s, "poseDisplayTime", ctime::g_rec.poseDisplayTime);
    {
      char st[64];
      sprintf_s(st, "\"simTime\":%.6f,", ctime::g_rec.simTime);
      s += st;
    }
    FgJu(s, "locateCalls", ctime::g_rec.locateCalls);
    FgJi(s, "qpcFrequency", g_qpcFreq);
    FgClose(s, '}');
  }
  // The mask slot (the commit's reference: seq, frame, displayTime).
  s += "\"mask\":{";
  FgJu(s, "flags", sl.flags);
  FgJu(s, "viewMask", sl.viewMask);
  FgJu(s, "atlasW", g_r.layout.atlasW);
  FgJu(s, "atlasH", g_r.layout.atlasH);
  s += "\"views\":[";
  for (int v = 0; v < 4; ++v) {
    if (!(sl.viewMask & (1u << v))) continue;
    const cm::MaskView& m = sl.view[v];
    s += "{";
    FgJi(s, "view", v);
    FgJu(s, "flags", m.flags);
    FgJu(s, "x", m.x), FgJu(s, "y", m.y), FgJu(s, "w", m.w), FgJu(s, "h", m.h);
    FgJu(s, "srcW", m.srcW), FgJu(s, "srcH", m.srcH), FgJu(s, "divisor", m.divisor);
    FgJa(s, "fovTan", m.fovTan, 4);
    FgJ(s, "nearM", m.nearM);
    FgJu(s, "dilate", m.dilate);
    FgClose(s, '}');
  }
  FgClose(s, ']');
  FgClose(s, '}');
  // The depth slot.
  if (ds) {
    s += "\"depth\":{";
    FgJs(s, "file", "depth");
    FgJu(s, "seq", ds->seq);
    FgJu(s, "flags", ds->flags);
    FgJu(s, "viewMask", ds->viewMask);
    FgJu(s, "xrSpace", ds->xrSpace);
    FgJu(s, "atlasW", r.t[2].w);
    FgJu(s, "atlasH", r.t[2].h);
    s += "\"views\":[";
    for (int v = 0; v < 4; ++v) {
      if (!(ds->viewMask & (1u << v))) continue;
      const cdp::DepthView& d = ds->view[v];
      s += "{";
      FgJi(s, "view", v);
      FgJu(s, "flags", d.flags);
      FgJu(s, "x", d.x), FgJu(s, "y", d.y), FgJu(s, "w", d.w), FgJu(s, "h", d.h);
      FgJu(s, "srcW", d.srcW), FgJu(s, "srcH", d.srcH), FgJu(s, "divisor", d.divisor);
      FgJa(s, "fovTan", d.fovTan, 4);
      FgJ(s, "nearM", d.nearM);
      FgJ(s, "boostShiftM", d.boostShiftM);
      FgJa(s, "worldMinusCockpitM", d.worldMinusCockpitM, 3);
      FgJa(s, "jitterPx", d.jitterPx, 2);
      FgJa(s, "poseOrientation", d.poseOrientation, 4);
      FgJa(s, "posePosition", d.posePosition, 3);
      FgJa(s, "xrFov", d.xrFov, 4);
      FgClose(s, '}');
    }
    FgClose(s, ']');
    FgClose(s, '}');
  } else {
    s += "\"depth\":null,";
  }
  // The motion slot.
  if (ms) {
    s += "\"motion\":{";
    FgJs(s, "file", "mv");
    s += r.t[1].used ? "\"classFile\":\"class\"," : "\"classFile\":null,";
    FgJu(s, "seq", ms->seq);
    FgJu(s, "prevSeq", ms->prevSeq);
    FgJi(s, "prevDisplayTime", ms->prevDisplayTime);
    FgJu(s, "flags", ms->flags);
    s += "\"flagNames\":[";
    const char* const names[8] = {"valid", "display_time", "late_commit", "partial", "reset", "includes_jitter",
                                  "sky_is_infinity", "jitter_free"};
    bool any = false;
    for (int b = 0; b < 8; ++b)
      if (ms->flags & (1u << b)) {
        s += any ? ",\"" : "\"";
        s += names[b];
        s += "\"";
        any = true;
      }
    s += "],";
    FgJu(s, "viewMask", ms->viewMask);
    FgJu(s, "atlasW", r.t[0].w);
    FgJu(s, "atlasH", r.t[0].h);
    s += "\"views\":[";
    for (int v = 0; v < 2; ++v) {
      if (!(ms->viewMask & (1u << v))) continue;
      const cmv::MotionView& m = ms->view[v];
      s += "{";
      FgJi(s, "view", v);
      FgJu(s, "flags", m.flags);
      FgJu(s, "x", m.x), FgJu(s, "y", m.y), FgJu(s, "w", m.w), FgJu(s, "h", m.h);
      FgJu(s, "srcW", m.srcW), FgJu(s, "srcH", m.srcH);
      FgJa(s, "fovTan", m.fovTan, 4);
      FgJa(s, "jitterCurPx", m.jitterCurPx, 2);
      FgJa(s, "jitterPrevPx", m.jitterPrevPx, 2);
      FgClose(s, '}');
    }
    FgClose(s, ']');
    FgClose(s, '}');
  } else {
    s += "\"motion\":null,";
  }
  // DCS's submitted frame (its xrEndFrame, before any layer).
  if (xr && xr->poses) {
    s += "\"xr\":{";
    FgJu(s, "space", xr->space);
    FgJu(s, "viewCount", xr->viewCount);
    s += "\"views\":[";
    for (uint32_t v = 0; v < xr->viewCount && v < 4; ++v) {
      s += "{";
      FgJi(s, "view", v);
      FgJa(s, "orientation", xr->q[v], 4);
      FgJa(s, "position", xr->p[v], 3);
      FgJa(s, "fov", xr->fov[v], 4);
      FgClose(s, '}');
    }
    FgClose(s, ']');
    FgClose(s, '}');
  } else {
    s += "\"xr\":null,";
  }
  // The colour of views 0 / 1.
  s += "\"color\":[";
  for (int v = 0; v < 2; ++v) {
    const FgColour& c = r.col[v];
    s += "{";
    FgJi(s, "view", v);
    if (c.ok) {
      const char* order = "RGBA";
      FgColourFormat(c.fmt, &order);
      FgJs(s, "file", v ? "color1" : "color0");
      FgJs(s, "source", c.src == 1 ? "swapchain" : "copy_source");
      FgJu(s, "w", c.w), FgJu(s, "h", c.h);
      FgJu(s, "dxgiFormat", static_cast<unsigned>(c.fmt));
      FgJs(s, "channelOrder", order);
      FgJu(s, "texW", c.texW), FgJu(s, "texH", c.texH), FgJu(s, "boxX", c.boxX), FgJu(s, "boxY", c.boxY);
      FgJu(s, "subresource", c.sub);
      FgJu(s, "captureEngineFrame", c.frame);
      s += c.frame == sl.engineFrame ? "\"paired\":true," : "\"paired\":false,";
    } else {
      s += "\"file\":null,";
    }
    FgClose(s, '}');
  }
  FgClose(s, ']');
  s.pop_back();  // "...]," -> "...]"
  s += "}";
  return s;
}

// After a commit (render thread): the frame's record, when a session runs or starts. committed: false when the frame
// committed nothing (a gap).
void FgCommit(const Frame& f, bool committed, const cm::MaskSlot* sl, bool withDepth, bool withMotion,
              const XrFrameInfo* xr) {
  if (!g_fgActive) {
    if (!committed || g_fgReq.load(std::memory_order_relaxed) <= 0 || g_fgEnding) return;
    FgRec* open = nullptr;
    for (FgRec& r : g_fgRing)
      if (r.open && r.frame == f.engineFrame) open = &r;
    const bool both = open && open->col[0].ok && open->col[1].ok;
    if (!both && ++g_fgWait < 30) return;  // the first frame with both colours (or after 30 frames: without)
    std::wstring base;
    {
      std::lock_guard<std::mutex> lock(g_fgBaseMutex);
      base = g_fgBase;
    }
    const int n = g_fgReq.exchange(0);
    if (n <= 0 || base.empty()) return;
    if (base.back() != L'\\' && base.back() != L'/') base += L"\\";
    base += L"fg_dump\\";
    CreateDirectoryW(base.c_str(), nullptr);
    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t sub[48];
    swprintf_s(sub, L"%04u%02u%02u_%02u%02u%02u_%03u\\", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
               st.wMilliseconds);
    g_fgDir = base + sub;
    CreateDirectoryW(g_fgDir.c_str(), nullptr);
    g_fgActive = true;
    g_fgN = g_fgLeft = n;
    g_fgIndex = g_fgWait = 0;
    g_fgPrevSeq = g_fgPrevFrame = 0;
    g_fgGaps = g_fgNoCommit = g_fgStalls = g_fgNoColour = g_fgFirstSeq = g_fgLastSeq = g_fgFromSource = 0;
    g_fgSessions++;
    Log("cockpit mask: frame-generation dump of %d consecutive frames to %ls (mask, depth %s, motion %s)", n,
        g_fgDir.c_str(), withDepth ? "on" : "OFF", withMotion ? "on" : "OFF");
  }
  if (!committed || !sl) {
    g_fgNoCommit++;
    return;
  }
  FgRec* r = FgRecFor(f.engineFrame);
  if (!r) {
    g_fgNoCommit++;
    return;
  }
  ID3D11DeviceContext* c = g_s.ctx;
  const int s = f.slot;
  const cdp::DepthSlot* ds = nullptr;
  const cmv::MotionSlot* ms = nullptr;
  if (withMotion && g_r.mtex[s] && FgStage(r->t[0], g_r.mW, g_r.mH, DXGI_FORMAT_R16G16_FLOAT, 4)) {
    c->CopyResource(r->t[0].st, g_r.mtex[s]);
    ms = &g_mhdr.slot[s];
    if (g_r.haveClass && g_r.ctex[s] && FgStage(r->t[1], g_r.mW, g_r.mH, DXGI_FORMAT_R8_UINT, 1))
      c->CopyResource(r->t[1].st, g_r.ctex[s]);
  }
  if (withDepth && g_r.dtex[s] &&
      FgStage(r->t[2], g_r.layout.atlasW, g_r.layout.atlasH, DXGI_FORMAT_R16G16_FLOAT, 4)) {
    c->CopyResource(r->t[2].st, g_r.dtex[s]);
    ds = &g_dhdr.slot[s];
  }
  if (!r->col[0].ok || !r->col[1].ok) g_fgNoColour++;
  for (const FgColour& fc : r->col)
    if (fc.ok && fc.src == 2) g_fgFromSource++;
  const bool gap = g_fgIndex > 0 && (sl->maskSeq != g_fgPrevSeq + 1 || f.engineFrame != g_fgPrevFrame + 1);
  if (gap) g_fgGaps++;
  r->index = g_fgIndex++;
  if (r->index == 0) g_fgFirstSeq = sl->maskSeq;
  g_fgLastSeq = sl->maskSeq;
  g_fgPrevSeq = sl->maskSeq;
  g_fgPrevFrame = f.engineFrame;
  wchar_t name[48];
  swprintf_s(name, L"f%03d", r->index);
  r->dir = g_fgDir;
  r->name = name;
  r->json = FgJson(*r, f, *sl, ds, ms, xr, gap);
  r->open = false;
  r->busy = true;
  r->at = g_commitN;
  if (--g_fgLeft <= 0) {
    g_fgActive = false;
    g_fgEnding = true;  // PollFg pushes the end once every record is out
  }
}

// Every staging texture (ReleaseDumps: uninstall, device change). A running session ends as aborted.
void FgRelease() {
  if (g_fgActive || g_fgEnding) {
    g_fgActive = g_fgEnding = false;
    FgPushEnd(true);
  }
  for (FgRec& r : g_fgRing) {
    for (FgTex& t : r.t) RelP(t.st);
    r = FgRec{};
  }
}

// Tick (config thread): the records and the session's index.json.
void FgWrite(std::vector<FgOut>& outs) {
  static std::wstring indexDir;
  static std::string indexFrames;
  for (FgOut& o : outs) {
    if (o.dir.empty()) continue;
    if (o.end) {
      std::string idx = o.json;
      idx.pop_back();  // '}'
      idx += ",\"files\":[" + (o.dir == indexDir ? indexFrames : std::string()) + "]}";
      if (FILE* f = _wfsopen((o.dir + L"index.json").c_str(), L"wb", _SH_DENYWR)) {
        fwrite(idx.data(), 1, idx.size(), f);
        fclose(f);
      }
      Log("cockpit mask: frame-generation dump written to %ls", o.dir.c_str());
      indexDir.clear();
      indexFrames.clear();
      continue;
    }
    if (o.dir != indexDir) {
      indexDir = o.dir;
      indexFrames.clear();
    }
    const std::wstring base = o.dir + o.name;
    for (int k = 0; k < kFgTex; ++k) {
      if (o.bytes[k].empty()) continue;
      if (FILE* f = _wfsopen((base + kFgSuffix[k]).c_str(), L"wb", _SH_DENYWR)) {
        fwrite(o.bytes[k].data(), 1, o.bytes[k].size(), f);
        fclose(f);
      }
    }
    if (FILE* f = _wfsopen((base + L".json").c_str(), L"wb", _SH_DENYWR)) {
      fwrite(o.json.data(), 1, o.json.size(), f);
      fclose(f);
    }
    char nm[48];
    sprintf_s(nm, "\"%ls\"", o.name.c_str());
    if (!indexFrames.empty()) indexFrames += ",";
    indexFrames += nm;
  }
}

// R70: the motion slot of this frame (render thread, in the commit, before the depth and mask headers). Returns true
// when written.
bool CommitMotion(const Frame& f, uint64_t seq, int64_t displayTime, bool paired, int64_t qpc, const XrFrameInfo* xr) {
  if (!f.motion || !g_mview.load()) return false;
  cmv::MotionSlot& ms = g_mhdr.slot[f.slot];
  ms = cmv::MotionSlot{};
  if (!f.motionMask) {  // nothing copied this frame: the slot describes nothing; the next one starts afresh
    g_mhdr.slotTime[f.slot] = cmv::SlotTime{};  // R74a
    PublishMotionHeader();
    g_mPrev.valid = false;
    return false;
  }
  const bool poses = paired && xr && xr->poses;
  const uint64_t cuts = scenecut::g_cuts.load(std::memory_order_relaxed);
  // The reset reasons (R70 ยง9.2).
  bool rPrev = !g_mPrev.valid || !paired || g_mPrev.displayTime == 0;
  const bool rGen = g_mPrev.valid && g_mPrev.gen != g_gen;
  const bool rSeq = g_mPrev.valid && (g_mPrev.seq + 1 != seq || g_mPrev.engineFrame + 1 != f.engineFrame);
  bool rebased = false;
  const bool rCut = g_mPrev.valid && (g_mPrev.cuts != cuts || CameraCut(g_mPrev, f, &rebased));
  if (rebased && g_mPrev.cuts == cuts) g_mRebaseNoCut++;  // R71
  bool rJit = false;
  float jitCur[2][2] = {};
  bool jitCurOk[2] = {};
  for (int v = 0; v < 2; ++v) {
    if (!(f.motionMask & (1u << v))) continue;
    cmv::MotionView m = f.mview[v];
    jitCur[v][0] = jitCur[v][1] = NAN;
    const ProjNote& pn = f.proj[v];
    if (poses && static_cast<uint32_t>(v) < xr->viewCount && pn.ok &&
        JitterPx(pn.ox, pn.oy, pn.p00, pn.p11, pn.vw, pn.vh, xr->fov[v], jitCur[v]))
      jitCurOk[v] = true;
    m.jitterCurPx[0] = jitCur[v][0];
    m.jitterCurPx[1] = jitCur[v][1];
    if (g_mPrev.valid && g_mPrev.jitOk[v]) {
      m.jitterPrevPx[0] = g_mPrev.jit[v][0];
      m.jitterPrevPx[1] = g_mPrev.jit[v][1];
    }
    if (!jitCurOk[v] || !(g_mPrev.valid && g_mPrev.jitOk[v])) rJit = true;
    ms.view[v] = m;
  }
  const bool reset = rPrev || rGen || rSeq || rCut || rJit;
  ms.seq = seq;
  ms.engineFrame = f.engineFrame;
  ms.displayTime = paired ? displayTime : 0;
  ms.qpcCommit = qpc;
  ms.prevSeq = g_mPrev.valid ? g_mPrev.seq : 0;
  ms.prevDisplayTime = g_mPrev.valid ? g_mPrev.displayTime : 0;
  ms.viewMask = f.motionMask;
  const uint32_t want = f.wantMask & 3u;
  ms.flags = cmv::slot_flags::valid | (paired && displayTime ? cmv::slot_flags::display_time : 0u) |
             (paired ? 0u : cmv::slot_flags::late_commit) | ((f.motionMask & want) != want ? cmv::slot_flags::partial : 0u) |
             (reset ? cmv::slot_flags::reset : 0u) | (kMvIncludesJitter ? cmv::slot_flags::includes_jitter : 0u) |
             (kMvJitterFree ? cmv::slot_flags::jitter_free : 0u) | (kMvSkyIsInfinity ? cmv::slot_flags::sky_is_infinity : 0u);
  g_mhdr.slotTime[f.slot] = ctime::ToMotion(ctime::g_rec);  // R74a: this frame's content time (seq == this seq)
  g_mhdr.newestSeq = seq;
  g_mhdr.newestSlot = static_cast<uint32_t>(f.slot);
  PublishMotionHeader();
  g_mCommits++;
  if (reset) {
    g_mResets++;
    (rPrev ? g_mResetPrev : rGen ? g_mResetGen : rSeq ? g_mResetSeq : rCut ? g_mResetCut : g_mResetJit)++;
  }
  if ((f.motionMask & want) != want) g_mPartial++;
  g_lastMotionMs = GetTickCount64();
  DebugCommitMotion(f.slot, ms, xr);
  // This commit is the next one's previous.
  MotionPrev np;
  np.valid = paired && displayTime != 0;
  np.seq = seq;
  np.engineFrame = f.engineFrame;
  np.displayTime = paired ? displayTime : 0;
  np.gen = g_gen;
  np.cuts = cuts;
  np.camOk = f.camOk;
  np.camOOk = f.camOOk;  // R71
  for (int k = 0; k < 3; ++k) np.camC[k] = f.camC[k], np.camF[k] = f.camF[k], np.camO[k] = f.camO[k];
  for (int v = 0; v < 2; ++v) {
    np.jitOk[v] = jitCurOk[v];
    np.jit[v][0] = jitCurOk[v] ? jitCur[v][0] : 0.0f;
    np.jit[v][1] = jitCurOk[v] ? jitCur[v][1] : 0.0f;
    if (poses && static_cast<uint32_t>(v) < xr->viewCount) {
      np.poseOk[v] = true;
      memcpy(np.q[v], xr->q[v], sizeof(np.q[v]));
      memcpy(np.p[v], xr->p[v], sizeof(np.p[v]));
      memcpy(np.fov[v], xr->fov[v], sizeof(np.fov[v]));
    }
  }
  g_mPrev = np;
  return true;
}

// R69: the depth slot of this frame (render thread, in the commit, before the mask header): DCS's submitted pose /
// FOV / space per view and the DLSS jitter from them. Returns true when written.
bool CommitDepth(const Frame& f, uint64_t seq, int64_t displayTime, bool paired, int64_t qpc, const XrFrameInfo* xr) {
  if (!f.depth || !g_dview.load()) return false;
  cdp::DepthSlot& ds = g_dhdr.slot[f.slot];
  ds = cdp::DepthSlot{};
  if (!f.depthMask) {  // nothing written this frame: the slot describes nothing (no stale pairing)
    g_dhdr.slotTime[f.slot] = cdp::SlotTime{};  // R74a
    PublishDepthHeader();
    return false;
  }
  const bool poses = paired && xr && xr->poses;
  ds.seq = seq;
  ds.engineFrame = f.engineFrame;
  ds.displayTime = paired ? displayTime : 0;
  ds.qpcCommit = qpc;
  ds.viewMask = f.depthMask;
  ds.xrSpace = poses ? xr->space : 0;
  bool dual = false;
  for (int v = 0; v < 4; ++v) {
    if (!(f.depthMask & (1u << v))) continue;
    cdp::DepthView dv = f.dview[v];
    if (poses && static_cast<uint32_t>(v) < xr->viewCount) {
      memcpy(dv.poseOrientation, xr->q[v], sizeof(dv.poseOrientation));
      memcpy(dv.posePosition, xr->p[v], sizeof(dv.posePosition));
      memcpy(dv.xrFov, xr->fov[v], sizeof(dv.xrFov));
      dv.flags |= cdp::view_flags::pose;
      const ProjNote& pn = f.proj[v];
      if (pn.ok) {
        if (JitterPx(pn.ox, pn.oy, pn.p00, pn.p11, pn.vw, pn.vh, dv.xrFov, dv.jitterPx)) {
          dv.flags |= cdp::view_flags::jitter;
          g_dJitter++;
        } else {
          g_dJitterBad++;
        }
      }
    }
    if (dv.flags & cdp::view_flags::dual_camera) dual = true;
    ds.view[v] = dv;
  }
  ds.flags = cdp::slot_flags::valid | (paired && displayTime ? cdp::slot_flags::display_time : 0u) |
             (paired ? 0u : cdp::slot_flags::late_commit) |
             ((f.depthMask & f.viewMask) != f.viewMask ? cdp::slot_flags::partial : 0u) |
             (dual ? cdp::slot_flags::dual_camera : 0u) | (poses ? cdp::slot_flags::poses : 0u);
  g_dhdr.slotTime[f.slot] = ctime::g_rec;  // R74a: this frame's content time (seq == this seq)
  g_dhdr.newestSeq = seq;
  g_dhdr.newestSlot = static_cast<uint32_t>(f.slot);
  PublishDepthHeader();
  g_dCommits++;
  if (poses) g_dPoses++;
  g_lastDepthMs = GetTickCount64();
  DebugCommitDepth(f.slot, ds);
  return true;
}

// The frame's commit: the fence signalled after the frame's dispatches, then the header (render thread).
void Commit(int64_t displayTime, bool paired, const XrFrameInfo* xr = nullptr) {
  NoteFlightFrame();  // R88
  if (!g_f.begun) {
    if (g_relayout) Relayout();
    return;
  }
  LARGE_INTEGER t0, t1;
  QueryPerformanceCounter(&t0);
  const Frame f = g_f;
  g_f = Frame{};
  double sigUs = 0;
  bool symCommitted = false;  // R95
  ID3D11DeviceContext* c = g_s.ctx;
  if (f.tsFrame >= 0 && c) {
    c->End(g_r.dj[f.tsFrame]);
    g_r.tsPending[f.tsFrame] = true;
  }
  if (!f.skipped && f.slot >= 0 && f.viewMask && g_r.haveLayout) {
    const uint64_t seq = ++g_seq;
    if (f.depth || f.motion) ctime::Make(seq, displayTime, paired, xr, t0.QuadPart);  // R74a: before both companions
    if (g_r.ctx4 && g_r.fence) {
      LARGE_INTEGER s0, s1;
      QueryPerformanceCounter(&s0);
      g_r.ctx4->Signal(g_r.fence, seq);
      QueryPerformanceCounter(&s1);
      g_signals++;
      sigUs = (s1.QuadPart - s0.QuadPart) * g_qpcToUs;
    }
    sym::Commit(f, seq, displayTime, paired, xr);  // R95: first (before the other companions and the mask header)
    symCommitted = true;
    const bool withMotion = CommitMotion(f, seq, displayTime, paired, t0.QuadPart, xr);  // R70: first
    const bool withDepth = CommitDepth(f, seq, displayTime, paired, t0.QuadPart, xr);  // R69: before the mask header
    cm::MaskSlot& sl = g_hdr.slot[f.slot];
    sl.maskSeq = seq;
    sl.engineFrame = f.engineFrame;
    sl.displayTime = paired ? displayTime : 0;
    sl.qpcCommit = t0.QuadPart;
    sl.flags = cm::slot_flags::valid | (paired && displayTime ? cm::slot_flags::display_time : 0) |
               (paired ? 0 : cm::slot_flags::late_commit) | ((f.viewMask & f.wantMask) != f.wantMask ? cm::slot_flags::partial : 0) |
               (withDepth ? cm::slot_flags::depth : 0u) | (f.ovlMask ? cm::slot_flags::overlay : 0u);
    sl.viewMask = f.viewMask;
    for (int v = 0; v < 4; ++v) sl.view[v] = (f.viewMask & (1u << v)) ? f.view[v] : cm::MaskView{};
    g_hdr.newestMaskSeq = seq;
    g_hdr.newestSlot = static_cast<uint32_t>(f.slot);
    PublishHeader();
    g_commits++;
    (paired && displayTime ? g_paired : g_late)++;
    if ((f.viewMask & f.wantMask) != f.wantMask) g_partial++;
    g_lastCommitMs = GetTickCount64();
    g_lastPaired = paired && displayTime != 0;
    DebugCommit(f.slot, sl);
    FgCommit(f, true, &sl, withDepth, withMotion, xr);  // R71: CockpitFgDump
  } else {
    FgCommit(f, false, nullptr, false, false, xr);  // R71: a frame without a commit (counted as a gap while dumping)
    // R76: a frame skipped because the release fence holds every slot: the header (newestMaskSeq unchanged) still
    // refreshes heartbeatQpc, so the reader finds it fresh, finds no slot for its frame and releases newestMaskSeq.
    if (f.skipped && GetTickCount64() - g_hdrPubMs >= kBusyBeatMs) {
      PublishHeader();
      g_busyBeats++;
    }
  }
  if (f.skipped) g_mPrev.valid = false;  // R70: a frame without a commit breaks the MV chain
  covl::FrameEnd(f);  // R74b
  sym::FrameEnd(symCommitted);  // R95: its statistics ring, the learning from older frames
  PollTiming();
  PollDumps();
  PollFg();  // R71
  if (g_relayout) Relayout();
  QueryPerformanceCounter(&t1);
  const double us = f.cpuUs + (t1.QuadPart - t0.QuadPart) * g_qpcToUs;
  std::lock_guard<std::mutex> lock(g_statMutex);
  g_cpuUsSum += us;
  g_cpuUsMax = std::max(g_cpuUsMax, us);
  g_sigUsSum += sigUs;
  g_sigUsMax = std::max(g_sigUsMax, sigUs);
  ++g_cpuN;
}

// DCS's xrEndFrame (render thread, before the layers): views still pending were not sampled before their
// transparent passes: dropped. Then the commit with the frame's displayTime.
void OnXrEndFrame(int64_t displayTime, const XrFrameInfo* xr = nullptr) {
  if (!Active()) return;
  if (g_idle && !g_f.begun && !HasPending()) return;  // R67: no reader; nothing was sampled this frame
  if (HasPending()) {
    g_droppedAtEnd++;
    DropPending();
  }
  if (!g_f.begun) g_xeEmpty++;
  Commit(displayTime, true, xr);
}

// R67: idle <-> active (render thread, at a frame boundary: nothing pending, nothing begun). One header write.
void SetIdle(bool idle) {
  g_idle = idle;
  g_idleA.store(idle, std::memory_order_relaxed);
  fdexport::g_maskUploads = !idle;  // the CPU gProj capture only while masks are made
  fixdepth::g_mvWanted = MvWantedNow();  // R76 (HB-3): the MV note follows the mask's heartbeat too
  (idle ? g_toIdle : g_toActive)++;
  PublishDepthHeader();  // R69: its idle flag follows the mask's too
  PublishMotionHeader();  // R70
  PublishSymHeader();  // R95
  if (idle) g_mPrev.valid = false;
  if (!idle) {
    // R115 (H1): NoteFlightFrame samples only at commits, and an idle reader makes none: g_flightBits' bit 0 / 1 are the
    // last samples before the idle stretch, and the first commit after it would count every quad frame culled meanwhile
    // as this frame's (bit 0). Both stale bits said "flight" -> the gate opened at a resume in the F10 map (a map
    // relayout). The signal starts again here: two fresh samples are needed (the first fresh one's bit 0 may still
    // count the idle stretch's quad frames: g_quadSeen is not moved, because this frame's world, culled before its
    // first pass, is already in g_quadFrame). The closed stretch restarts too: an idle stretch is not a closed-gate
    // hold (it ran g_gateClosedMs past g_mapHoldMaxMs and opened the backstop at once). g_flightNoted is kept: it is a
    // pre-idle engine frame (g_s.frame advances every frame, idle or not), so this frame's first commit samples.
    g_flightBits = 0;
    g_gateClosedMs = 0;
    g_gateTimedOut = false;
  }
  PublishHeader();
}

// R95: the symbology block's reader heartbeat.
void SetSymIdle(bool idle) {
  g_sIdle = idle;
  g_sIdleA.store(idle, std::memory_order_relaxed);
  (idle ? g_sToIdle : g_sToActive)++;
  PublishSymHeader();
}

// R70: the motion block's reader heartbeat.
void SetMotionIdle(bool idle) {
  g_mIdle = idle;
  g_mIdleA.store(idle, std::memory_order_relaxed);
  fixdepth::g_mvWanted = MvWantedNow();  // the OM hooks' MV note only while copying (R76: and the mask active)
  (idle ? g_mToIdle : g_mToActive)++;
  g_mPrev.valid = false;  // the first commit after idle / resume is a reset
  PublishMotionHeader();
}

// R69: the depth block's reader heartbeat (depth is made only while it and the mask's are fresh).
void SetDepthIdle(bool idle) {
  g_dIdle = idle;
  g_dIdleA.store(idle, std::memory_order_relaxed);
  (idle ? g_dToIdle : g_dToActive)++;
  PublishDepthHeader();
}

// R67: the reader's heartbeat, once per frame at its first pass; sets the state for this frame.
void UpdateReader() {
  cm::Header* v = g_view.load(std::memory_order_relaxed);
  LARGE_INTEGER q;
  QueryPerformanceCounter(&q);
  const bool alive = v && cm::reader_alive(v, q.QuadPart, g_qpcFreq);
  if (alive == g_idle) SetIdle(!alive);
  if (g_idle) g_idleFrames++;
  if (cdp::Header* d = g_dview.load(std::memory_order_relaxed)) {  // R69
    const bool dAlive = cdp::reader_alive(d, q.QuadPart, g_qpcFreq);
    if (dAlive == g_dIdle) SetDepthIdle(!dAlive);
  }
  if (cmv::Header* m = g_mview.load(std::memory_order_relaxed)) {  // R70
    const bool mAlive = cmv::reader_alive(m, q.QuadPart, g_qpcFreq);
    if (mAlive == g_mIdle) SetMotionIdle(!mAlive);
  }
  if (csy::Header* sv = g_sview.load(std::memory_order_relaxed)) {  // R95
    const bool sAlive = csy::reader_alive(sv, q.QuadPart, g_qpcFreq) ||
                        g_symPublishAlways.load(std::memory_order_relaxed);  // R97: [Dev] SymbologyPublishAlways
    if (sAlive == g_sIdle) SetSymIdle(!sAlive);
  }
}

// fixation_depth.h's EndFrame (the next frame's first pass): a frame xrEndFrame did not commit is committed late.
// Then the reader heartbeat decides this frame (R67).
void OnEndFrame() {
  if (HasPending()) {
    g_droppedAtEnd++;
    DropPending();
  }
  if (!Active()) return;
  if (g_f.begun || !g_idle) Commit(0, false);
  UpdateReader();
}

// ---------------------------------------------------------------------------
// The xrEndFrame hook (OpenXR dispatch slot 35, the table frame_start.h finds)
// ---------------------------------------------------------------------------

using XrEndFn = int32_t(__stdcall*)(void*, const void*);
constexpr int kXeSlot = 35;  // XrGeneratedDispatchTable: ... xrWaitFrame 33, xrBeginFrame 34, xrEndFrame 35
// Visualizer: mov r8, [rax+48h]; mov rax, [r8+118h], per fstart::kVisBuilds build (2.9.30, 2.9.31: R147, .text +0x2c790)
constexpr uint32_t kXeTrampRva[fstart::kVisBuildCount] = {0x1b6abf, 0x1e324f};
const uint8_t kXeTramp[] = {0x4c, 0x8b, 0x40, 0x48, 0x49, 0x8b, 0x80, 0x18, 0x01, 0x00, 0x00};
XrEndFn g_xeOrig = nullptr;  // kept after the unhook (a call in flight still forwards through it)
void** g_xeSlot = nullptr;
// R93: an observer chained behind this hook ([Perf] FrameTimeline's xrEndFrame hook, frame_timeline.h): when set, the
// hook forwards to it instead of g_xeOrig, and it calls g_xeOrig. The slot keeps one owner (this hook); the loader
// registry's one-true-original rule is untouched. Cleared by its owner (frame_timeline.h) and by UninstallXr.
std::atomic<XrEndFn> g_xeNext{nullptr};
std::atomic<int> g_xeBusy{0};
std::atomic<bool> g_xeOn{false};

// XrFrameEndInfo: type (12) at 0, next at 8, displayTime at 16.
int64_t ReadDisplayTimeSeh(const void* info) {
  __try {
    if (!info || *static_cast<const int32_t*>(info) != 12) return 0;
    return *reinterpret_cast<const int64_t*>(static_cast<const uint8_t*>(info) + 16);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return 0;
  }
}

// R69: DCS's first XrCompositionLayerProjection (type 35) of the XrFrameEndInfo: its space and, per view (type 48,
// 96 bytes: pose at 16 = orientation x y z w, position x y z; fov at 44), the pose and FOV, views 0..3 in view order.
// XrFrameEndInfo: layerCount (u32) at 28, layers (pointer array) at 32. Projection layer: space at 24, viewCount
// (u32) at 32, views at 40. Never throws (SEH: nothing read).
bool ReadPosesSeh(const void* info, XrFrameInfo& o) {
  __try {
    const auto* b = static_cast<const uint8_t*>(info);
    if (!b || *reinterpret_cast<const int32_t*>(b) != 12) return false;
    const uint32_t n = *reinterpret_cast<const uint32_t*>(b + 28);
    const uint8_t* const* layers = *reinterpret_cast<const uint8_t* const* const*>(b + 32);
    if (!layers || n == 0 || n > 64) return false;
    for (uint32_t i = 0; i < n; ++i) {
      const uint8_t* l = layers[i];
      if (!l || *reinterpret_cast<const int32_t*>(l) != 35) continue;
      const uint32_t vc = *reinterpret_cast<const uint32_t*>(l + 32);
      const uint8_t* views = *reinterpret_cast<const uint8_t* const*>(l + 40);
      if (!views || vc == 0 || vc > 16) return false;
      o.space = *reinterpret_cast<const uint64_t*>(l + 24);
      o.viewCount = vc < 4 ? vc : 4;
      for (uint32_t v = 0; v < o.viewCount; ++v) {
        const uint8_t* pv = views + 96 * v;
        if (*reinterpret_cast<const int32_t*>(pv) != 48) {
          o.viewCount = v;
          break;
        }
        memcpy(o.q[v], pv + 16, 16);
        memcpy(o.p[v], pv + 32, 12);
        memcpy(o.fov[v], pv + 44, 16);
      }
      o.poses = o.viewCount > 0;
      return o.poses;
    }
    return false;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    o = XrFrameInfo{};
    return false;
  }
}

void OnXrEndFrameSeh(int64_t dt, const XrFrameInfo* xr) {
  __try {
    OnXrEndFrame(dt, xr);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    LatchFault();  // R76 (HB-2)
  }
}

int32_t __stdcall H_EndFrame(void* session, const void* info) {
  qvalog::NoteFrameThread();  // R93: the render thread (main.cpp's async log: its lines never wait)
  g_xeBusy.fetch_add(1, std::memory_order_acq_rel);
  if (g_xeOn.load(std::memory_order_acquire)) {
    g_xeCalls++;
    if (GetCurrentThreadId() == ptiming::g_topTid.load(std::memory_order_relaxed) && !fixdepth::g_fault.load()) {
      XrFrameInfo xr;
      xr.displayTime = ReadDisplayTimeSeh(info);
      if (g_f.depth || g_f.motion || g_f.sym) ReadPosesSeh(info, xr);  // R69 / R70 / R95: only when a companion writes
      OnXrEndFrameSeh(xr.displayTime, &xr);
    } else
      g_xeOffThread++;
  }
  const XrEndFn next = g_xeNext.load(std::memory_order_acquire);  // R93: FrameTimeline chained behind us
  const XrEndFn orig = next ? next : g_xeOrig;
  g_xeBusy.fetch_sub(1, std::memory_order_acq_rel);
  return orig(session, info);
}

// Config thread (install): DCS's OpenXR dispatch table through Visualizer's verified instance (frame_start.h's
// stamp, lea and trampoline checks, plus the xrEndFrame trampoline).
bool InstallXr() {
  if (g_xeSlot) return true;
  auto* vis = reinterpret_cast<uint8_t*>(GetModuleHandleW(L"Visualizer.dll"));
  if (!vis) {
    Log("cockpit mask: Visualizer.dll not loaded; frames committed late (no displayTime pairing)");
    return false;
  }
  const int build = fstart::VisBuildOf(vis);
  if (build < 0 || !gpt::CodeMatches(vis + kXeTrampRva[build], kXeTramp, sizeof(kXeTramp))) {
    Log("cockpit mask: Visualizer.dll is not an analysed build; frames committed late (no displayTime pairing)");
    return false;
  }
  void** disp = fstart::DispatchGuarded(vis, build);
  if (!disp || !gpt::Readable(disp) || !gpt::Readable(&disp[kXeSlot])) {
    Log("cockpit mask: OpenXR dispatch table not found (no session yet?); retried");
    return false;
  }
  char where[200];
  void* orig = SlotOriginal(&disp[kXeSlot]);
  if (!orig || !gpt::ModRva(orig, where, sizeof(where))) {
    Log("cockpit mask: xrEndFrame dispatch entry is not in a loaded image; retried");
    return false;
  }
  g_xeOrig = reinterpret_cast<XrEndFn>(orig);
  if (!HookSlot(&disp[kXeSlot], reinterpret_cast<void*>(&H_EndFrame), nullptr)) {
    Log("cockpit mask: could not hook xrEndFrame");
    return false;
  }
  g_xeSlot = &disp[kXeSlot];
  g_xeOn = true;
  g_xrHooked = true;
  Log("cockpit mask: xrEndFrame hooked (-> %s): masks committed with DCS's displayTime", where);
  ctime::Install(vis, disp);  // R74a: xrLocateViews, the same verified table (failure: no slotTime, logged)
  return true;
}

// R76: our hooks still in their dispatch slots. The loader registry keeps one true original per slot, so two hooks of
// one slot never chain: a feature that hooks it after us bypasses ours ([Perf] FrameTimeline did; it now waits while
// we own 35 / 36). Displaced: logged once, the stats say "not hooked", slot_time no longer advertised.
std::atomic<bool> g_xeLost{false};
void CheckXrSlots() {
  if (g_xeSlot && !g_xeLost.load() && *g_xeSlot != reinterpret_cast<void*>(&H_EndFrame)) {
    g_xeLost = true;
    g_xrHooked = false;
    Log("cockpit mask: the xrEndFrame dispatch slot was replaced by another hook: frames committed late (no "
        "displayTime pairing) until the export is reinstalled");
  }
  ctime::CheckSlot();
}

void UninstallXr() {
  ctime::Uninstall();  // R74a: first (drained, its slot back)
  g_xeOn = false;
  g_xeNext = nullptr;  // R93: a chained observer (FrameTimeline) re-arms on its own once the slot is free
  if (g_xeSlot) {
    // R76: put the original back only while the slot still holds our hook (else it is another feature's now).
    if (*g_xeSlot == reinterpret_cast<void*>(&H_EndFrame)) UnhookSlot(g_xeSlot, reinterpret_cast<void*>(g_xeOrig));
    g_xeSlot = nullptr;
  }
  g_xeLost = false;
  g_xrHooked = false;
  for (int i = 0; i < 200 && g_xeBusy.load(); ++i) Sleep(1);
}

// ---------------------------------------------------------------------------
// Control (config thread)
// ---------------------------------------------------------------------------

// [Perception] CockpitMask* (before fixdepth::Tick, which installs or uninstalls).
void Configure(bool on, int divisor, int views, double nearM, int dilate, int dump = 0, int logFrames = 0,
               const std::wstring& dumpDir = std::wstring()) {
  // CockpitMaskDump / CockpitMaskLog: a new value (re)starts them.
  static int lastDump = 0, lastLog = 0;
  // R70: the dump folder for every dump (the depth and motion dumps too, without a mask dump; live, R70's first run
  // lost its motion dumps: the folder was set only by CockpitMaskDump).
  if (on && !dumpDir.empty()) g_dumpDir = dumpDir;
  if (dump != lastDump) {
    lastDump = dump;
    if (on && dump > 0 && !dumpDir.empty()) {
      g_dumpDir = dumpDir;
      g_clearMaskDir = true;  // R91
      g_dumpLeft = std::min(dump, 300);
    }
  }
  if (logFrames != lastLog) {
    lastLog = logFrames;
    g_logLeft = on ? std::clamp(logFrames, 0, 1000) : 0;
  }
  if (!g_on.load() && on) {  // the name is fixed while installed
    g_blockName = cm::block_name;
    g_dBlockName = cdp::block_name;  // R69
    g_mBlockName = cmv::block_name;  // R70
    g_sBlockName = csy::block_name;  // R95
    const std::wstring suffix = perc_sfx::SuffixW();  // R79: the shared rule (perception_suffix.h), was CRT getenv_s
    g_blockName += suffix;
    g_dBlockName += suffix;
    g_mBlockName += suffix;
    g_sBlockName += suffix;
  }
  g_div = divisor > 0 ? ClampDiv(divisor) : 0;
  g_views = static_cast<uint32_t>(views) & 15u ? static_cast<uint32_t>(views) & 15u : 3u;
  g_nearM = static_cast<float>(std::clamp(nearM, 0.0, 20.0));
  g_dilate = std::clamp(dilate, 0, kMaxDilate);
  g_on = on;
  cpass::g_symWant = on && g_symOn.load();  // R95 (ConfigureSymbology runs before this)
}

// R69: [Perception] CockpitDepthExport / CockpitDepthDump (before Configure; a change of the export reinstalls the
// mask export through fixation_depth.h's Tick: DepthWanted() is part of its install key).
void ConfigureDepth(bool on, int dump = 0) {
  static int lastDump = 0;
  if (dump != lastDump) {
    lastDump = dump;
    if (on && dump > 0) {
      g_clearDepthDir = true;  // R91
      g_ddumpLeft = std::min(dump, 300);
    }
  }
  g_depthOn = on;
}

// R70: [Perception] CockpitMotionExport / CockpitMotionClass / CockpitMotionDump (before Configure; a change of the
// export reinstalls the mask export through fixation_depth.h's Tick: MotionWanted() is part of its install key).
void ConfigureMotion(bool on, bool cls = true, int dump = 0) {
  static int lastDump = 0;
  if (dump != lastDump) {
    lastDump = dump;
    if (on && dump > 0) {
      g_clearMotionDir = true;  // R91
      g_mdumpLeft = std::min(dump, 300);
    }
  }
  g_motionClass = cls;
  g_motionOn = on;
  // The OM hooks note the MV render target only while MVs are copied (a reader alive): idle costs nothing.
  // R76 (HB-1): the render thread owns it (SetIdle / SetMotionIdle; Install); here it is only cleared when the export
  // goes off (a change of the export reinstalls, and Install sets it again).
  if (!on) fixdepth::g_mvWanted = false;
}

// R95: [Perception] SymbologyExport / SymbologyAlpha (before Configure; a change of the export reinstalls the mask export
// through fixation_depth.h's Tick: SymWanted() is part of its install key). The colour pass is wanted with it (the
// tone-map draw's second evaluation).
// R97: publishAlways = [Dev] SymbologyPublishAlways (default 0): produced and published without the symbology block's
// reader heartbeat (OFXR 0053's OFF arm reads the windows with hud_symbology=0); read live by the render thread.
void ConfigureSymbology(bool on, bool alpha = true, bool publishAlways = false) {
  if (on != g_symOn.load()) g_symVramNote = on ? 1 : 2;  // R97 (Dev): logged by the render thread (its device)
  if (publishAlways != g_symPublishAlways.load())
    Log("cockpit mask: [Dev] SymbologyPublishAlways=%d (the symbology %s)", publishAlways ? 1 : 0,
        publishAlways ? "is produced and published without its reader's heartbeat; the mask's heartbeat still applies"
                      : "follows its reader's heartbeat again");
  g_symPublishAlways = publishAlways;
  g_symAlpha = alpha;
  g_symOn = on;
  cpass::g_symWant = on && g_on.load();
}

// R71: [Perception] CockpitFgDump=N (a new value > 0 requests a session of N consecutive frames, 1-300) into
// <base>g_dump\<date_time>\; base is the dev folder (DcsQvCull\dev\ in dev mode), else the log folder.
void ConfigureFg(int frames, const std::wstring& base) {
  static int last = 0;
  if (frames == last) return;
  last = frames;
  if (frames > 0 && !base.empty()) {
    {
      std::lock_guard<std::mutex> lock(g_fgBaseMutex);
      g_fgBase = base;
    }
    g_fgReq = std::min(frames, 300);
  } else {
    g_fgReq = 0;
  }
}

// fixation_depth.h Install, before its callback goes in.
void ReleaseHeld(const char* why);
void Install() {
  if (!g_on.load()) return;
  // R85: a held set whose device was removed is released before anything is published.
  if (g_held && g_r.dev && DevRemoved(g_r.dev)) ReleaseHeld("device removed");
  g_cmInstalled = true;  // R86
  g_stopping = false;
  g_fault = false;
  DropPending();
  g_f = Frame{};
  g_relayout = true;
  for (auto& w : g_srcW) w = 0;
  for (auto& h : g_srcH) h = 0;
  // R88: the flight signal starts again (the quad frames before this install are not this frame's).
  g_quadSeen = g_quadFrame.load(std::memory_order_relaxed);
  g_flightBits = 0;
  g_flightNoted = ~0ull;
  g_gateClosedMs = 0;
  g_gateTimedOut = false;
  g_bPending = false;  // R91 (Uninstall discarded it; the render thread is not in this module now)
  EnsureBuilder();
  if (!g_gen) g_gen = SeedGen();
  if (!g_held) g_hdr = cm::Header{};  // R85: held, the inactive record (generation, handles, maskSeq) stays
  if (!OpenBlock()) Log("cockpit mask: %ls could not be mapped; export off", g_blockName.c_str());
  // R67: idle until a reader heartbeat is fresh (a reader that kept the block mapped across a reload: active at once).
  LARGE_INTEGER f, q;
  QueryPerformanceFrequency(&f);
  QueryPerformanceCounter(&q);
  g_qpcFreq = f.QuadPart;
  cm::Header* blk = g_view.load();
  g_idle = !(blk && cm::reader_alive(blk, q.QuadPart, g_qpcFreq));
  g_idleA = g_idle;
  // R69: the depth companion (its own block and heartbeat), mapped before the mask header goes out.
  if (!g_held) g_dhdr = cdp::Header{};  // R85
  g_dIdle = true;
  if (DepthWanted() && blk) {
    if (OpenDepthBlock()) {
      cdp::Header* dblk = g_dview.load();
      g_dIdle = !cdp::reader_alive(dblk, q.QuadPart, g_qpcFreq);
      PublishDepthHeader();  // magic, v1, reader_heartbeat (+ idle), not active yet
    } else {
      Log("cockpit mask: %ls could not be mapped; depth companion off", g_dBlockName.c_str());
    }
  }
  g_dIdleA = g_dIdle;
  // R70: the motion companion (its own block and heartbeat).
  if (!g_held) g_mhdr = cmv::Header{};  // R85
  g_mIdle = true;
  g_mPrev = MotionPrev{};
  if (MotionWanted() && blk) {
    if (OpenMotionBlock()) {
      g_mIdle = !cmv::reader_alive(g_mview.load(), q.QuadPart, g_qpcFreq);
      PublishMotionHeader();
    } else {
      Log("cockpit mask: %ls could not be mapped; motion companion off", g_mBlockName.c_str());
    }
  }
  g_mIdleA = g_mIdle;
  // R95: the symbology companion (its own block and heartbeat); the colour pass's hook into it.
  if (!g_held) g_shdr = csy::Header{};
  g_sIdle = true;
  cpass::g_symQuery = &sym::ToneQuery;
  cpass::g_symDone = &sym::ToneDone;
  cpass::g_symDrawBegin = &sym::ToneDrawBegin;  // R97: the draw's GPU time in the symbology's cost line
  ckipd::ocpu::g_symRec = &sym::Record;  // ovl_cpu.h's render point: the records' types
  ckipd::ocpu::g_symReplayEnd = &sym::ReplayEnd;
  if (SymWanted() && blk) {
    if (OpenSymBlock()) {
      g_sIdle = !(csy::reader_alive(g_sview.load(), q.QuadPart, g_qpcFreq) ||
                  g_symPublishAlways.load(std::memory_order_relaxed));  // R97: [Dev] SymbologyPublishAlways
      sym::TickQv(true);
      sym::NoteInstall();  // R97: the record render point's state is logged by Tick once it is known
      PublishSymHeader();
    } else {
      Log("cockpit mask: %ls could not be mapped; symbology companion off", g_sBlockName.c_str());
    }
  }
  g_sIdleA = g_sIdle;
  fixdepth::g_mvWanted = MvWantedNow();  // R76: the mask's heartbeat too (HB-3)
  PublishHeader();  // magic, v1, reader_heartbeat (+ idle), not active yet: a reader may store its heartbeat now
  fdexport::g_maskUploads = !g_idle;  // this pass's gProj on the CPU (the rect's FOV), only while masks are made
  // R91 (R90 phase 2): the 12 compute shaders (D3DCompile, 50-400 ms) and the base objects (parameter buffer, fences
  // and their NT handles, queries) here on the config thread, not at the first capture on the render thread; the
  // render thread's EnsureBase then finds them made. DCS's device unknown here (fixation_depth.h Install found none):
  // the render thread makes the base objects at its first capture, as before (the shaders are compiled already).
  if (!Compile()) {
    static bool logged = false;
    if (!logged) Log("cockpit mask: compute shaders not compiled (%s); the export makes no mask", g_compileError.c_str());
    logged = true;
  } else if (g_s.dev && g_s.ctx) {
    g_baseFromInstall = true;
    const bool ok = EnsureBase(g_s.dev, g_s.ctx);
    g_baseFromInstall = false;
    if (!ok) Log("cockpit mask: shaders / fences could not be made on DCS's device at the install");
  }
  if (g_xrHookWanted.load()) InstallXr();
  Log("cockpit mask: on (%ls; divisor %d, views 0x%x, near %.2f m, dilate %d); reader heartbeat %s",
      g_blockName.c_str(), g_div.load(), g_views.load(), g_nearM.load(), g_dilate.load(),
      g_idle ? "absent: idle (no dispatch, no fence signal) until a reader stores one" : "fresh: active");
  if (g_dview.load())
    Log("cockpit mask: depth companion on (%ls, R16G16_FLOAT metres, contract revision %u); its reader heartbeat %s",
        g_dBlockName.c_str(), cdp::contract_revision, g_dIdle ? "absent: no depth until a reader stores one" : "fresh");
  if (g_mview.load())
    Log("cockpit mask: motion companion on (%ls, DCS's MVs of views 0 / 1 at render resolution%s, contract revision %u; "
        "jitter_free %s, sky_is_infinity %s); its reader heartbeat %s",
        g_mBlockName.c_str(), g_motionClass.load() ? " + class byte" : "", cmv::contract_revision,
        kMvJitterFree ? "set (R70 T1)" : "not set (unproven)", kMvSkyIsInfinity ? "set (R70 T2)" : "not set (unproven)",
        g_mIdle ? "absent: no copy until a reader stores one" : "fresh");
  if (g_sview.load())  // R95
    Log("cockpit mask: symbology companion on (%ls, E RGBA16F + class R8_UINT, capacities %u / %u, coverage alpha %s, "
        "contract revision %u; record render point %s); its reader heartbeat %s%s",
        g_sBlockName.c_str(), sym::g_capPeri.load(), sym::g_capFocus.load(), g_symAlpha.load() ? "on" : "off",
        csy::contract_revision,
        sym::RenderPoint() ? "hooked"
                           : "not hooked yet (ovl_cpu.h installs it with the cockpit dual camera, CockpitTrueIpd with "
                             "the cpu overlay path; checked every frame, its state is logged once known)",
        g_sIdle ? "absent: nothing until a reader stores one" : "fresh",
        g_symPublishAlways.load() ? "; [Dev] SymbologyPublishAlways=1: produced and published without its reader's "
                                    "heartbeat (the mask's still applies)"
                                  : "");
}

// fixation_depth.h Uninstall: first (the hook out, while the boundary callback still runs), then Release after the
// callback drained.
void UninstallHook() {
  g_stopping = true;
  UninstallXr();
}
// R85: why the set cannot be held (nullptr: it can).
const char* HoldBlocker() {
  if (!g_r.dev || !g_r.baseOk || !g_r.haveLayout || !g_r.tex[0] || !g_r.texH[0]) return "nothing made";
  if (g_fault.load() || fixdepth::g_fault.load()) return "a fault is latched";
  if (DevRemoved(g_r.dev)) return "the device was removed";
  return nullptr;
}

// R85: what an uninstall releases while the set is held: DCS's targets (the MV SRVs, the overlay's), the overlay's
// views, the debug staging; the timing queries are made anew (one may be begun). Textures, UAVs, fences, NT handles,
// shaders and the parameter buffer stay.
void HoldRes() {
  covl::ReleaseAllRes();
  sym::ReleaseTargets();  // R95: DCS's HDR targets
  sym::ReleaseQueriesB();
  sym::MakeQueriesB(g_r.dev);
  ReleaseDumps();
  for (auto& m : g_r.mvSrv) {
    RelP(m.srv);
    RelP(m.res);
    m = Res::MvSrv{};
  }
  ReleaseQueries();
  MakeQueries(g_r.dev);
}

// hold (R85; false: the payload stop): keep the shared set (see the top: 11.) unless HoldBlocker says why not.
void Uninstall(bool hold = true) {
  fdexport::g_maskUploads = false;
  DiscardBuild();  // R91: a set requested but not swapped in (made by the builder or being made): released
  DropPending();
  g_f = Frame{};
  // R86: fixation_depth.h's Uninstall calls this at every reinstall, also when the mask export is off (this module not
  // installed): a held set stays as it is (before: held again, counted and logged per reinstall), and with nothing
  // held no new generation goes into the blocks a reader maps (before: one per reinstall of another feature).
  if (!g_cmInstalled && hold) {
    CloseBuried(false);
    fixdepth::g_mvWanted = false;
    return;
  }
  g_cmInstalled = false;
  const char* blocker = HoldBlocker();
  if (hold && !blocker) {
    HoldRes();
    g_held = true;
    g_heldForMap = false;  // R88: an uninstall's hold now (republished, counted as R85)
    g_holds++;
    CloseSymBlock(true);  // R95
    CloseMotionBlock(true);  // inactive, the same generation and handles
    CloseDepthBlock(true);
    CloseBlock(true);
    Log("cockpit mask: off: shared set of generation %u held (mask%s%s: the same textures, fences and NT handles when "
        "the export is on again; R85)", g_gen, g_r.haveDepth ? ", depth" : "", g_r.haveMotion ? ", motion" : "");
  } else {
    if (g_held || (blocker && strcmp(blocker, "nothing made") != 0) || (!hold && !blocker)) {
      if (g_held) g_heldReleases++;
      Log("cockpit mask: shared set of generation %u released at the uninstall (%s)", g_gen,
          !hold ? "payload stop" : blocker);
    }
    g_held = false;
    ReleaseAll();
    // R76: the last records carry a new generation (flags 0), so a reader that read the old one and opens its handles
    // right now fails its re-check (generation) instead of keeping what it opened; and the handles keep their 2 s
    // grace (closed by the config loop's Tick, fixation_depth.h; at Shutdown at once), as the contract promises.
    BumpGen();
    g_hdr.generation = g_dhdr.generation = g_mhdr.generation = g_shdr.generation = g_gen;
    RetireUnmapped();  // R85: first, a companion whose block this install did not map (its held record)
    CloseSymBlock();  // R95
    CloseMotionBlock();  // R70
    CloseDepthBlock();  // R69: inactive first
    CloseBlock();
  }
  CloseBuried(false);
  g_dIdle = true;
  g_dIdleA = true;
  g_mIdle = true;
  g_mIdleA = true;
  g_sIdle = true;  // R95
  g_sIdleA = true;
  sym::ClosePupil();
  g_mPrev = MotionPrev{};
  fixdepth::g_mvWanted = false;
}

// R85: the held set released while the export is off (config thread, nothing installed; or Install before it maps):
// R76's records (a new generation, flags 0, no handles) into the blocks a reader still maps, the handles' 2 s grace.
void ReleaseHeld(const char* why) {
  if (!g_held) return;
  g_held = false;
  g_heldReleases++;
  const uint32_t old = g_gen;
  ReleaseAll();
  BumpGen();
  g_hdr.generation = g_dhdr.generation = g_mhdr.generation = g_shdr.generation = g_gen;
  PublishSymHeader(true);  // R95
  PublishMotionHeader(true);  // the blocks mapped (not expected: the callers run while nothing is installed)
  PublishDepthHeader(true);
  PublishHeader(true);
  RetireUnmapped();  // the others: a reader may still map them
  CloseBuried(false);
  Log("cockpit mask: held shared set of generation %u released (%s); generation %u published inactive, the old NT "
      "handles closed after their grace", old, why, g_gen);
}

// R85: once per second while nothing is installed (fixation_depth.h Tick): a removed device releases the held set.
void CheckHeld() {
  if (g_held && g_r.dev && DevRemoved(g_r.dev)) ReleaseHeld("device removed");
}

// Once per second: a hook not installed yet (no OpenXR session at the install) is retried; old handles closed.
void Tick() {
  CloseBuried(false);
  std::vector<DumpFile> files, dfiles, mfiles;
  std::vector<std::vector<uint8_t>> mcls;
  std::vector<std::string> lines;
  std::vector<FgOut> fgOuts;  // R71
  {
    std::lock_guard<std::mutex> lock(g_dbgMutex);
    fgOuts.swap(g_fgOut);
    files.swap(g_dumpOut);
    dfiles.swap(g_ddumpOut);
    mfiles.swap(g_mdumpOut);
    mcls.swap(g_mdumpCls);
    lines.swap(g_logOut);
  }
  if (!fgOuts.empty()) FgWrite(fgOuts);  // R71: CockpitFgDump
  if (!mfiles.empty() && !g_dumpDir.empty()) {  // R70: motion dumps
    const wchar_t last = g_dumpDir.back();
    const std::wstring dir = g_dumpDir + (last == L'\\' || last == L'/' ? L"" : L"\\") + L"cockpit_motion";
    CreateDirectoryW(dir.c_str(), nullptr);
    if (g_clearMotionDir.exchange(false)) ClearDumpDir(dir);  // R91
    for (size_t i = 0; i < mfiles.size(); ++i) {
      const DumpFile& f = mfiles[i];
      const std::wstring base = dir + L"\\" + f.name;
      if (FILE* o = _wfsopen((base + L".r16g16f").c_str(), L"wb", _SH_DENYWR)) {
        fwrite(f.pgm.data(), 1, f.pgm.size(), o);
        fclose(o);
      }
      if (i < mcls.size() && !mcls[i].empty())
        if (FILE* o = _wfsopen((base + L".r8").c_str(), L"wb", _SH_DENYWR)) {
          fwrite(mcls[i].data(), 1, mcls[i].size(), o);
          fclose(o);
        }
      if (FILE* o = _wfsopen((base + L".txt").c_str(), L"wb", _SH_DENYWR)) {
        fwrite(f.text.data(), 1, f.text.size(), o);
        fclose(o);
      }
    }
    Log("cockpit mask: %zu motion dump(s) written to %ls", mfiles.size(), dir.c_str());
  }
  if (!dfiles.empty() && !g_dumpDir.empty()) {  // R69: depth dumps
    const wchar_t last = g_dumpDir.back();
    const std::wstring dir = g_dumpDir + (last == L'\\' || last == L'/' ? L"" : L"\\") + L"cockpit_depth";
    CreateDirectoryW(dir.c_str(), nullptr);
    if (g_clearDepthDir.exchange(false)) ClearDumpDir(dir);  // R91
    for (const DumpFile& f : dfiles) {
      const std::wstring base = dir + L"\\" + f.name;
      if (FILE* o = _wfsopen((base + L".r16g16f").c_str(), L"wb", _SH_DENYWR)) {
        fwrite(f.pgm.data(), 1, f.pgm.size(), o);
        fclose(o);
      }
      if (FILE* o = _wfsopen((base + L".txt").c_str(), L"wb", _SH_DENYWR)) {
        fwrite(f.text.data(), 1, f.text.size(), o);
        fclose(o);
      }
    }
    Log("cockpit mask: %zu depth dump(s) written to %ls", dfiles.size(), dir.c_str());
  }
  for (const std::string& l : lines) Log("cockpit mask %s", l.c_str());
  if (!files.empty() && !g_dumpDir.empty()) {
    // R66: the dev / log folder comes with its trailing separator (live it logged "dev\\\\cockpit_mask").
    const wchar_t last = g_dumpDir.back();
    const std::wstring dir = g_dumpDir + (last == L'\\' || last == L'/' ? L"" : L"\\") + L"cockpit_mask";
    CreateDirectoryW(dir.c_str(), nullptr);
    if (g_clearMaskDir.exchange(false)) ClearDumpDir(dir);  // R91
    for (const DumpFile& f : files) {
      const std::wstring base = dir + L"\\" + f.name;
      if (FILE* o = _wfsopen((base + L".pgm").c_str(), L"wb", _SH_DENYWR)) {
        fwrite(f.pgm.data(), 1, f.pgm.size(), o);
        fclose(o);
      }
      if (FILE* o = _wfsopen((base + L".txt").c_str(), L"wb", _SH_DENYWR)) {
        fwrite(f.text.data(), 1, f.text.size(), o);
        fclose(o);
      }
    }
    Log("cockpit mask: %zu dump(s) written to %ls", files.size(), dir.c_str());
  }
  const ULONGLONG now = GetTickCount64();
  // R67: idle <-> active transitions, at most one line per 5 s (the count covers what happened since the last line).
  {
    static uint64_t seenIdle = 0, seenActive = 0;
    static ULONGLONG lastLine = 0;
    const uint64_t ti = g_toIdle.load(), ta = g_toActive.load();
    if ((ti != seenIdle || ta != seenActive) && now - lastLine >= 5000) {
      lastLine = now;
      const unsigned long long n = (ti - seenIdle) + (ta - seenActive);
      seenIdle = ti;
      seenActive = ta;
      Log("cockpit mask: reader %s (%llu transition%s since the last line)",
          g_idleA.load() ? "heartbeat absent or older than 2 s: idle, no dispatch, no fence signal"
                         : "heartbeat fresh: active, masks committed",
          n, n == 1 ? "" : "s");
    }
  }
  if (g_mview.load()) {  // R70: the motion block's heartbeat transitions, at most one line per 5 s
    static uint64_t seenIdle = 0, seenActive = 0;
    static ULONGLONG lastLine = 0;
    const uint64_t ti = g_mToIdle.load(), ta = g_mToActive.load();
    if ((ti != seenIdle || ta != seenActive) && now - lastLine >= 5000) {
      lastLine = now;
      const unsigned long long n = (ti - seenIdle) + (ta - seenActive);
      seenIdle = ti;
      seenActive = ta;
      Log("cockpit mask: motion reader %s (%llu transition%s since the last line)",
          g_mIdleA.load() ? "heartbeat absent or older than 2 s: no MV copy" : "heartbeat fresh: MVs copied while the mask is active",
          n, n == 1 ? "" : "s");
    }
  }
  if (g_sview.load()) {  // R95: the symbology block's heartbeat transitions, at most one line per 5 s
    static uint64_t seenIdle = 0, seenActive = 0;
    static ULONGLONG lastLine = 0;
    const uint64_t ti = g_sToIdle.load(), ta = g_sToActive.load();
    if ((ti != seenIdle || ta != seenActive) && now - lastLine >= 5000) {
      lastLine = now;
      const unsigned long long n = (ti - seenIdle) + (ta - seenActive);
      seenIdle = ti;
      seenActive = ta;
      Log("cockpit mask: symbology reader %s (%llu transition%s since the last line)",
          g_sIdleA.load() ? "heartbeat absent or older than 2 s: no symbology" : "heartbeat fresh: E committed while the mask is active",
          n, n == 1 ? "" : "s");
    }
    sym::TickQv(false);
    sym::TickPupil();
    sym::TickRenderPoint();  // R97
  }
  if (g_dview.load()) {  // R69: the depth block's heartbeat transitions, at most one line per 5 s
    static uint64_t seenIdle = 0, seenActive = 0;
    static ULONGLONG lastLine = 0;
    const uint64_t ti = g_dToIdle.load(), ta = g_dToActive.load();
    if ((ti != seenIdle || ta != seenActive) && now - lastLine >= 5000) {
      lastLine = now;
      const unsigned long long n = (ti - seenIdle) + (ta - seenActive);
      seenIdle = ti;
      seenActive = ta;
      Log("cockpit mask: depth reader %s (%llu transition%s since the last line)",
          g_dIdleA.load() ? "heartbeat absent or older than 2 s: no depth (mask-only shader)"
                          : "heartbeat fresh: depth committed while the mask is active",
          n, n == 1 ? "" : "s");
    }
  }
  CheckXrSlots();  // R76
  static ULONGLONG lastTry = 0;
  if (g_on.load() && g_xrHookWanted.load() && !g_xeSlot && now - lastTry >= 10000) {
    lastTry = now;
    InstallXr();
  }
}

// DcsQvCull_Status bits 39 / 40 (status_word.h): configured and committed in the last second; that commit paired.
uint64_t StatusBits() {
  if (!g_on.load() || g_idleA.load(std::memory_order_relaxed)) return 0;  // R67: idle (no reader) = not committing
  const ULONGLONG now = GetTickCount64();
  const ULONGLONG last = g_lastCommitMs.load();
  if (!last || now - last > 1000) return 0;
  // R69: bit 41, a depth slot committed in the last second (depth configured, its reader heartbeat fresh).
  const ULONGLONG lastD = g_lastDepthMs.load();
  const bool depth = g_depthOn.load() && !g_dIdleA.load(std::memory_order_relaxed) && lastD && now - lastD <= 1000;
  // R70: bit 42, a motion slot committed in the last second (motion configured, its reader heartbeat fresh).
  const ULONGLONG lastM = g_lastMotionMs.load();
  const bool motion = g_motionOn.load() && !g_mIdleA.load(std::memory_order_relaxed) && lastM && now - lastM <= 1000;
  // R95: bit 43, a symbology slot committed in the last second (configured, its reader heartbeat fresh).
  const ULONGLONG lastS = sym::g_lastCommitMs.load();
  const bool symb = g_symOn.load() && !g_sIdleA.load(std::memory_order_relaxed) && lastS && now - lastS <= 1000;
  return qvstatus::kCockpitMask | (g_lastPaired.load() ? qvstatus::kCockpitMaskPaired : 0) |
         (depth ? qvstatus::kCockpitDepth : 0) | (motion ? qvstatus::kCockpitMotion : 0) |
         (symb ? qvstatus::kCockpitSymbology : 0);
}

void LogStats() {
  if (!g_on.load()) return;
  double gAvg, gMax, cAvg, cMax, sAvg, sMax;
  uint64_t gN;
  {
    std::lock_guard<std::mutex> lock(g_statMutex);
    gAvg = g_gpuN ? g_gpuUsSum / g_gpuN : 0.0;
    gMax = g_gpuUsMax;
    gN = g_gpuN;
    cAvg = g_cpuN ? g_cpuUsSum / g_cpuN : 0.0;
    cMax = g_cpuUsMax;
    sAvg = g_cpuN ? g_sigUsSum / g_cpuN : 0.0;
    sMax = g_sigUsMax;
    g_gpuUsSum = g_gpuUsMax = g_cpuUsSum = g_cpuUsMax = g_sigUsSum = g_sigUsMax = 0;
    g_gpuN = g_cpuN = 0;
  }
  auto u = [](const std::atomic<uint64_t>& a) { return static_cast<unsigned long long>(a.load()); };
  // R67: the reader's state and its heartbeat's age (config thread: an acquire read of the reader's field).
  char reader[200] = "no block";
  if (cm::Header* v = g_view.load()) {
    const uint64_t hb = cm::reader_heartbeat_of(v).load(std::memory_order_acquire);
    LARGE_INTEGER q;
    QueryPerformanceCounter(&q);
    char age[48] = "never";
    if (hb && g_qpcFreq > 0)
      sprintf_s(age, "%.0f ms ago", (q.QuadPart - static_cast<int64_t>(hb)) * 1000.0 / static_cast<double>(g_qpcFreq));
    sprintf_s(reader, "%s (heartbeat %s; %llu idle frames, %llu -> idle, %llu -> active, %llu fence signals)",
              g_idleA.load() ? "idle" : "active", age, u(g_idleFrames), u(g_toIdle), u(g_toActive), u(g_signals));
  }
  Log("cockpit mask: %llu views sampled (%llu after water, %llu at lighting, %llu at a transparent pass, %llu other), "
      "%llu commits (%llu paired, %llu late, %llu partial), xrEndFrame %s (%llu calls, %llu off thread, %llu empty); "
      "skipped: identity %llu, format %llu, size %llu, no FOV %llu (FOV from GazeOut %llu), no SRV %llu, reader busy %llu "
      "(%llu header refreshes), "
      "no resources %llu, dropped at frame end %llu; no stencil %llu; %llu relayouts, %llu textures; state checks %llu "
      "diffs %llu; generation %u; cost GPU %.1f us/frame avg / %.1f max (%llu timed), render thread %.1f us/frame avg / "
      "%.1f max (of which the fence signal's flush %.1f avg / %.1f max); reader %s%s",
      u(g_captures), u(g_afterWater), u(g_atLighting), u(g_atTransparent), u(g_atOther), u(g_commits), u(g_paired),
      u(g_late), u(g_partial), g_xrHooked.load() ? "hooked" : "not hooked", u(g_xeCalls), u(g_xeOffThread),
      u(g_xeEmpty), u(g_skipIdentity), u(g_skipFormat), u(g_skipSize), u(g_skipNoFov), u(g_fovGaze), u(g_skipNoSrv),
      u(g_skipBusy), u(g_busyBeats), u(g_skipNoRes), u(g_droppedAtEnd), u(g_noStencil), u(g_relayouts), u(g_texCreates),
      u(g_stateChecks), u(g_stateDiffs), g_gen, gAvg, gMax, static_cast<unsigned long long>(gN), cAvg, cMax, sAvg, sMax,
      reader, g_fault.load() ? "; FAULT latched" : "");
  {  // R88 / R91
    const uint64_t bn = g_buildN.load(), bus = g_buildUsSum.load();
    Log("cockpit mask: F10 / loading hold (R88; gate %s, quad frames %llu): %llu holds, %llu resumed with the held "
        "layout, %llu released by a new layout, %llu frames held, %llu frames waiting without a set, %llu backstops; "
        "off the render thread (R91): base objects made at the install %llu, at a capture %llu; %llu sets made by "
        "the builder (%llu inline), %llu commits waited for one, %llu discarded, %llu failed; build %.2f ms avg / "
        "%.2f max",
        g_flightGate.load() ? "on" : "off", static_cast<unsigned long long>(g_quadFrame.load()), u(g_mapHolds),
        u(g_mapResumes), u(g_mapRelayouts), u(g_mapHeldFrames), u(g_mapWaitFrames), u(g_mapTimeouts),
        u(g_baseAtInstall), u(g_baseAtCapture), static_cast<unsigned long long>(bn), u(g_buildsInline), u(g_buildWaits),
        u(g_buildsDiscarded), u(g_buildsFailed), bn ? bus / 1000.0 / static_cast<double>(bn) : 0.0,
        g_buildUsMax.load() / 1000.0);
  }
  if (g_dview.load() || g_mview.load()) Log("cockpit mask: %s", ctime::StatsText().c_str());  // R74a
  Log("cockpit mask: %s", covl::StatsText().c_str());  // R74b
  if (g_sview.load()) {  // R95
    const uint64_t hb = csy::reader_heartbeat_of(g_sview.load()).load(std::memory_order_acquire);
    LARGE_INTEGER q;
    QueryPerformanceCounter(&q);
    char age[48] = "never";
    if (hb && g_qpcFreq > 0)
      sprintf_s(age, "%.0f ms ago", (q.QuadPart - static_cast<int64_t>(hb)) * 1000.0 / static_cast<double>(g_qpcFreq));
    std::string symCost;
    const std::string symMain = sym::StatsText(&symCost);
    Log("cockpit mask: %s; symbology reader %s (heartbeat %s; %llu -> idle, %llu -> active); tone-map draws with the "
        "second evaluation %llu (shaders not ready %llu)",
        symMain.c_str(), g_sIdleA.load() ? "idle" : "active", age, u(g_sToIdle), u(g_sToActive),
        u(cpass::g_symDraws), u(cpass::g_symNoPs));
    Log("cockpit mask: %s", symCost.c_str());  // R97
  }
  if (g_dview.load()) {  // R69
    const uint64_t hb = cdp::reader_heartbeat_of(g_dview.load()).load(std::memory_order_acquire);
    LARGE_INTEGER q;
    QueryPerformanceCounter(&q);
    char age[48] = "never";
    if (hb && g_qpcFreq > 0)
      sprintf_s(age, "%.0f ms ago", (q.QuadPart - static_cast<int64_t>(hb)) * 1000.0 / static_cast<double>(g_qpcFreq));
    Log("cockpit mask: depth companion %llu commits (%llu with DCS's poses), %llu views (%llu with a dual-camera shift), "
        "no cPerView %llu, jitter known %llu / refused %llu; depth reader %s (heartbeat %s; %llu -> idle, %llu -> active)",
        u(g_dCommits), u(g_dPoses), u(g_dViews), u(g_dDual), u(g_dNoCb), u(g_dJitter), u(g_dJitterBad),
        g_dIdleA.load() ? "idle" : "active", age, u(g_dToIdle), u(g_dToActive));
  }
  if (g_mview.load()) {  // R70
    const uint64_t hb = cmv::reader_heartbeat_of(g_mview.load()).load(std::memory_order_acquire);
    LARGE_INTEGER q;
    QueryPerformanceCounter(&q);
    char age[48] = "never";
    if (hb && g_qpcFreq > 0)
      sprintf_s(age, "%.0f ms ago", (q.QuadPart - static_cast<int64_t>(hb)) * 1000.0 / static_cast<double>(g_qpcFreq));
    Log("cockpit mask: motion companion %llu commits, %llu views copied, %llu partial; reset %llu (first / after idle "
        "%llu, generation %llu, sequence %llu, camera cut %llu, jitter unknown %llu; origin rebases not counted as cuts %llu); no MV target %llu, no SRV %llu, "
        "size %llu, ambiguous binds %llu; motion reader %s (heartbeat %s; %llu -> idle, %llu -> active)",
        u(g_mCommits), u(g_mViews), u(g_mPartial), u(g_mResets), u(g_mResetPrev), u(g_mResetGen), u(g_mResetSeq),
        u(g_mResetCut), u(g_mResetJit), u(g_mRebaseNoCut.load()), u(g_mNoMv), u(g_mNoSrv), u(g_mBadFmt), u(fixdepth::g_mvAmbiguous),
        g_mIdleA.load() ? "idle" : "active", age, u(g_mToIdle), u(g_mToActive));
  }
}

}  // namespace cmask
