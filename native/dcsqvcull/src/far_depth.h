// Far depth engine export for the bundled Quad Views compositor ([Perception]
// FarDepthExport, default 0; contract: dcsvr_depth_export.h, copied verbatim
// from dlss5vrframegen native/quadviews; design: its docs/FAR_DEPTH.md,
// read-only here). Not a performance optimization: outside the kill switch,
// not one of the status word's features.
//
// Per frame, on the render thread (fixation_depth.h's boundary callback):
//  1. Wanted. At the frame's first pass boundary Quad Views'
//     DcsVr_QuadViewsDepthWanted() is called (DcsVr_QuadViewsDepthApi() must
//     return 1). Missing layer or export, or mask 0: nothing else runs (no
//     hook armed, no copy, no texture); after 120 such frames the copies are
//     released (Quad Views first, then ours) and the generation changes.
//  2. Views. fixation_depth.h's run capture (the G-buffer's DSV, viewport and
//     cPerView, captured inside GBufferPass) and its CopyToHMDPass ordinal
//     give HMD views 2 and 3 (left / right focus). A view is pushed only when
//     its identity is certain: four HMD views in the previous frame, the
//     learned HMD offset 0, the in-pass arm intact, single-sample, and, when a
//     GazeOut record exists, no decisive MatchFov in favour of another view.
//  3. Projection on the CPU. Quad Views needs this frame's gProj in the
//     descriptor, which fixation_depth.h only reads back on the GPU two frames
//     later. DCS uploads cPerView (0x970 bytes, GraphicsCore's ping-pong pair)
//     with UpdateSubresource inside each pass (R17 step 5, R18 step 3), so
//     while a GBufferPass runs the immediate context's UpdateSubresource
//     entry is redirected too (the same in-pass arm as OMSetRenderTargets*):
//     uploads to a buffer known to be cPerView (learned from VS b7 at earlier
//     pass ends, with its *SetConstantBuffers1 offset) keep the bytes of
//     gViewInv (192) and gProj (256). At the pass end the bound b7 must have
//     been uploaded in this very pass, else the view is not pushed. The
//     matrices are stored column-major (DCS's default packing); the
//     descriptor holds logical P[r][c] at [r * 4 + c].
//  4. Copy. At the end of the view's WaterSeaPass (else its
//     GBufferLightingPass, the next view or the frame end, flagged
//     FALLBACK_CAPTURE): CopySubresourceRegion(ours, 0, 0, 0, 0, DCS's depth,
//     the DSV's subresource, NULL), whole subresource (no source box on
//     depth-stencil). Ours: same typeless format and size, ArraySize 1,
//     MipLevels 1, DEFAULT, BIND_SHADER_RESOURCE, single-sample. A ring of
//     [Perception] FarDepthExportSlots (2, or 1 under VRAM pressure) per view,
//     by engine frame. No binding is read or changed (state checked in
//     FixationDepthVerify mode and in the tests). R91 (R90 phase 2): a view's
//     slots are made in one go at its first copy after a size or format change
//     (one creation stall on the render thread, not a second one a frame later).
//  5. Pre-overlay colour (v1.1, [Perception] FarDepthPreOverlay, default 1).
//     DCS draws HUD/HMD symbology after DLSS (HUD_MFD_after_DLSS): per view
//     ... ToneMappingPass, DepthResolverPass2, OverlayLDRPass, CopyToHMDPass,
//     and a GUIPass at the frame start (live trace 2026-10-13). While an
//     overlay-class pass (GUIPass, OverlayLDRPass) runs, the in-pass arm
//     redirects OMSetRenderTargets* and RSSetViewports again; the first
//     render target bound in it that is a focus view's colour target (the
//     learned target of that view, or the first one bound by the view's own
//     OverlayLDRPass by CopyToHMDPass ordinal) is copied at the first viewport
//     set after the bind (DCS binds, then sets the viewport, then draws), box
//     = that viewport, into our texture at (0, 0): the typeless family of the
//     target (R8G8B8A8, B8G8R8A8, R10G10B10A2, R16G16B16A16) or its own format
//     (R11G11B10_FLOAT). Only for a view whose depth this frame is waiting to
//     be pushed: a GUIPass before the view rendered (the frame-start one)
//     never counts. MSAA, another view dimension or no viewport: the depth is
//     pushed without HAS_PRE_OVERLAY.
//  5b. Copy point. Live 2026-10-13 (parked F-4: Quad Views' pre-overlay vs
//     submitted diff 0 %; F-16 in flight: 76 %, following head motion): the
//     target can still hold the previous frame at that viewport, when the
//     OverlayLDRPass first composites the tone-mapped scene into it with one
//     full-screen draw and only then draws HUD/MFD. So while a focus view's
//     OverlayLDRPass runs, the immediate context's draw entries are
//     redirected too (same arm and re-arm as above, focus view only), and the
//     first draw into the bound target decides per view, over its first
//     kLearnObs OverlayLDRPasses that have one (majority; cached until the
//     reinstall, logged once):
//       full-screen (3/4 vertices or 3/4/6 indices, one instance, viewport
//       covering >= 90 % of the copied box both ways) -> the copy is made
//       right AFTER that draw (the scene composite), before anything else;
//       otherwise -> the viewport point above, and no draw hook any more.
//     While learning, the viewport copy is made and a composite overwrites
//     it. Decided "after the composite": no viewport copy; the first draw
//     into the target that is not full-screen gets the copy just before it
//     (counted), no draw at all -> pushed without HAS_PRE_OVERLAY (counted).
//     The draw hooks are taken out right after the copy (or after 64 draws
//     into other targets). Not armed while GPU pass timing / dedup or the D3D
//     meter hold the context table (their re-arm would chain with ours): the
//     viewport point is used then. [Perception] FarDepthOverlayTrace = N
//     logs, for N frames, the focus views' passes from ToneMappingPass to the
//     end of OverlayLDRPass: the first 12 draws of each pass (call, count,
//     RTV resource and size, viewport, PS, PS SRV 0, full-screen, whether the
//     RTV is the pre-overlay target), per pass the draws into the target, and
//     what is left bound at each pass end (RTV 0 / CS UAVs = the target).
//  5c. Post-overlay colour ([Perception] FarDepthPostOverlay, default 1, only
//     with the pre-overlay): our pre-overlay texture is 2w x h (when 2w fits
//     in 16384, else w x h and no post copy, counted); at the view's
//     CopyToHMDPass begin (after its last overlay pass, GUIPass included) the
//     same box of the same target is copied to (w, 0) and
//     DCSVR_DEPTH_PRE_POST_OVERLAY is set, only when both copies were made
//     in this frame. Quad Views can then diff pre against post in one format,
//     without CopyToHMDPass's conversion. The push then follows the post copy
//     (CopyToHMDPass begin instead of the overlay pass end).
//  5d. HDR coverage ([Perception] FarDepthCoverageMode=hdr, the default;
//     "overlay" selects 5/5b/5c, any other value is logged once and means hdr).
//     Live 2026-10-13 (F-16, HUD_MFD_after_DLSS): the HUD is drawn into the
//     HDR buffer after DLSS (DLSSPass -> OverlayHDRPass -> HistogramPass ->
//     ToneMappingPass), so the LDR pre-overlay already holds it. In hdr mode
//     the source is the resource ToneMappingPass's first draw reads at PS SRV
//     0 (learnt per focus view with the draw hooks of that pass, AddRef'd,
//     re-checked every frame; a change voids that frame's colour), its box
//     the viewport of that draw clamped to it: pre = that box at the view's
//     DLSSPass end (no DLSSPass: at the OverlayHDRPass / HistogramPass begin,
//     counted), post = the same box at the ToneMappingPass begin, to (w, 0);
//     preOverlayFormat = the SRV's format; texture the typeless family or the
//     same format, 2w x h; HAS_PRE_OVERLAY | PRE_POST_OVERLAY only with both
//     copies in the frame. The push stays at CopyToHMDPass.
//     FarDepthOverlayTrace in both modes: the window runs from the view's
//     DLSSPass end (else the first of OverlayHDRPass / HistogramPass /
//     ToneMappingPass) to its CopyToHMDPass begin: every pass, its first 16
//     draws (RTV format and whether RTV 0 is the HDR source too), what
//     DLSSPass left bound, and ToneMappingPass's PS SRV 0.
//  5e. HDR fallback (hdr mode only). Per focus view and per frame in which it
//     is wanted and its CopyToHMDPass ended, the frame has a usable HDR pair
//     when: a ToneMappingPass source is learnt, a pre point came (DLSSPass
//     end, else OverlayHDRPass / HistogramPass begin), a ToneMappingPass
//     began after it, and the source's format is a float HDR one
//     (R16G16B16A16 / R32G32B32A32 / R32G32B32 FLOAT, R11G11B10_FLOAT,
//     R9G9B9E5). kHdrMissFallback consecutive such frames without a pair: the
//     view uses the overlay-mode copies (5 / 5b / 5c) instead, logged once per
//     view with the reason; kHdrOkReturn consecutive frames with a pair while
//     fallen back: hdr again (the source is still learnt at every
//     ToneMappingPass). Each switch releases the view's pre-overlay textures
//     the way a format change does (Quad Views first, then ours; a new
//     generation). Overlay mode never evaluates this.
//  6. Push. DcsVr_QuadViewsDepth(&d) on the render thread, same context: at
//     the end of the overlay pass that copied the view's colour, else at the
//     view's CopyToHMDPass end (with the pre-overlay off: right after the
//     depth copy); a view still waiting at the frame end (after xrEndFrame)
//     is dropped. Quad Views reads at xrEndFrame, after
//     all of these. displayTime 0 (DCS's frame/xrLocateViews pairing is not
//     known), jitterPx 0 without JITTER_KNOWN (measured live 2026-10-13: the
//     jitter is in gProj's P20/P21, about +-0.3-0.5 px; not read from NGX),
//     outputWidth/Height 0.
//  7. Release. Every export call resolves the layer per call (GetModuleHandleEx
//     with a module reference held during the call, as scene_cut.h): the
//     OpenXR loader may unload and reload the layer. R91 (R90 phase 2): the
//     frame's Wanted and pushes share one resolution: DecideFrame references
//     the module and resolves the exports once; the frame's last wanted push
//     (else EndFrame, or the uninstall) lets it go; a push without it resolves
//     per call as before. Before our textures are
//     freed (device change, texture recreation, 120 unwanted frames, payload
//     unload) DcsVr_QuadViewsDepthRelease(generation) is called for every
//     generation pushed since the last release; the uninstall asks the render
//     thread to do it (bounded wait) and does it itself only when the render
//     thread does not come (DCS not rendering). Quad Views AddRefs, so a
//     texture it still holds never dangles either way.
// [Perception] FarDepthLogProj = N logs P20/P21/P23 and the viewport origin of
// the focus views for the next N frames (DLSS jitter measurement); it arms the
// upload capture even when Quad Views wants nothing.
// Included from fixation_depth.h inside namespace fixdepth (after State, Run,
// ReadBind). The contract header is included by main.cpp at global scope.
#pragma once

namespace fdexport {

constexpr uint32_t kFocusMask = 0b1100u;
constexpr int kMaxSlots = 2;
constexpr int kUnwantedRelease = 120;

std::atomic<bool> g_on{false};        // [Perception] FarDepthExport
std::atomic<bool> g_preOn{true};      // [Perception] FarDepthPreOverlay
std::atomic<bool> g_postOn{true};     // [Perception] FarDepthPostOverlay (only with the pre-overlay)
// [Perception] FarDepthCoverageMode: 1 hdr, the default (5d: the DLSS output
// from the DLSSPass end to the ToneMappingPass begin; per view 5e falls back
// to overlay), 0 overlay (pre at the OverlayLDRPass, post at CopyToHMDPass
// begin).
constexpr int kCovOverlay = 0, kCovHdr = 1;
std::atomic<int> g_covMode{kCovHdr};
// The configured mode is hdr (the source is learnt, 5e evaluated).
bool HdrMode() { return g_covMode.load(std::memory_order_relaxed) == kCovHdr; }

// 5e. The HDR fallback per focus view (render thread).
constexpr int kHdrMissFallback = 120;  // consecutive frames without a usable HDR pair: overlay copies
constexpr int kHdrOkReturn = 60;       // consecutive frames with one while fallen back: hdr again
struct HdrWatch {
  bool fb = false;             // the view uses the overlay-mode copies
  int miss = 0, ok = 0;        // consecutive evaluated frames without / with a pair
  bool rendered = false;       // this frame: the view's CopyToHMDPass ended
  bool pre = false;            // this frame: a pre point (DLSSPass end, OverlayHDRPass / HistogramPass begin)
  bool tone = false;           // this frame: a ToneMappingPass began after the pre point
  bool fbLogged = false, backLogged = false;
};
HdrWatch g_hw[2];
// This view's copies follow hdr mode (5d) now: hdr configured and not fallen back.
bool ViewHdr(int view) { return HdrMode() && !g_hw[view - 2].fb; }
void ResetHdrWatch() {
  for (HdrWatch& w : g_hw) w = HdrWatch{};
}
// A float HDR colour format (5e).
bool HdrFloatFormat(DXGI_FORMAT f) {
  return f == DXGI_FORMAT_R16G16B16A16_FLOAT || f == DXGI_FORMAT_R32G32B32A32_FLOAT ||
         f == DXGI_FORMAT_R32G32B32_FLOAT || f == DXGI_FORMAT_R11G11B10_FLOAT || f == DXGI_FORMAT_R9G9B9E5_SHAREDEXP;
}
std::atomic<bool> g_stopping{false};  // uninstall in progress: no new copy or push
std::atomic<int> g_slots{2};          // [Perception] FarDepthExportSlots (1-2)
std::atomic<int> g_logProjLeft{0};    // [Perception] FarDepthLogProj frames left
// Uninstall -> render thread: release the layer's references and our copies.
std::atomic<bool> g_releaseReq{false}, g_releaseDone{false};
bool Active() { return g_on.load(std::memory_order_relaxed) && !g_stopping.load(std::memory_order_relaxed); }

// The layer and its export names (tests use their own).
std::wstring g_module = L"XR_APILAYER_MBUCCHIA_quad_views_foveated.dll";
std::string g_nameApi = "DcsVr_QuadViewsDepthApi", g_nameWanted = "DcsVr_QuadViewsDepthWanted",
            g_nameDepth = "DcsVr_QuadViewsDepth", g_nameRelease = "DcsVr_QuadViewsDepthRelease";
void SetExportPrefix(const char* prefix) {
  const std::string p = prefix;
  g_nameApi = p + "DepthApi";
  g_nameWanted = p + "DepthWanted";
  g_nameDepth = p + "Depth";
  g_nameRelease = p + "DepthRelease";
}

std::atomic<uint64_t> g_frames{0}, g_wantedFrames{0}, g_noLayer{0}, g_noExport{0}, g_badApi{0}, g_copies{0},
    g_pushes{0}, g_accepted{0}, g_rejected[8] = {}, g_skipIdentity{0}, g_skipMismatch{0}, g_skipNoProj{0},
    g_skipMsaa{0}, g_skipFormat{0}, g_skipArm{0}, g_fallback{0}, g_afterWater{0}, g_texCreates{0}, g_releases{0},
    g_releaseCalls{0}, g_uploadsSeen{0}, g_stateChecks{0}, g_stateDiffs{0}, g_usRearms{0}, g_usLost{0},
    g_omLost{0}, g_preCopies{0}, g_prePushed{0}, g_preMissing{0}, g_preSkipMsaa{0}, g_preSkipFormat{0},
    g_preNoViewport{0}, g_preArms{0}, g_dropped{0}, g_postCopies{0}, g_postSkipWide{0}, g_postMissing{0},
    g_hdrPre{0}, g_hdrPost{0}, g_hdrNoSource{0}, g_hdrMismatch{0}, g_hdrNoPost{0}, g_hdrLearns{0}, g_hdrFallback{0},
    g_hdrBadFormat{0}, g_hdrToOverlay{0}, g_hdrToHdr{0}, g_hdrOverlayFrames{0}, g_hdrNoPair[4] = {};
std::atomic<uint32_t> g_lastMask{0};
std::atomic<bool> g_loggedFirst{false};
std::mutex g_statMutex;
double g_cpuUsSum = 0, g_cpuUsMax = 0;  // under g_statMutex
uint64_t g_cpuN = 0;

// ---------------------------------------------------------------------------
// Quad Views exports (resolved per call; module referenced during the call)
// ---------------------------------------------------------------------------

// The layer's module, referenced (FreeLibrary after use); null when not loaded.
HMODULE RefLayer() {
  HMODULE m = nullptr;
  if (!GetModuleHandleExW(0, g_module.c_str(), &m)) return nullptr;
  return m;
}

// R91: the frame's layer module (referenced by DecideFrame, let go by EndFrame / DropFrameModule; render thread, or
// the uninstall once the callback is out) and its push export.
// It is let go as soon as every wanted view of the frame was pushed, so a last frame (DCS stops rendering, e.g. before
// xrDestroyInstance unloads the layer) keeps no reference on the layer.
HMODULE g_frameMod = nullptr;
PFN_DcsVr_QuadViewsDepth g_frameDepth = nullptr;
uint32_t g_frameWant = 0, g_framePushed = 0;  // the frame's wanted views (DecideFrame) and those pushed
void DropFrameModule() {
  if (g_frameMod) FreeLibrary(g_frameMod);
  g_frameMod = nullptr;
  g_frameDepth = nullptr;
  g_frameWant = g_framePushed = 0;
}

// The wanted mask of the referenced module m (0 when an export is missing or the interface version is not 1).
uint32_t WantedOf(HMODULE m) {
  uint32_t mask = 0;
  const auto api = reinterpret_cast<PFN_DcsVr_QuadViewsDepthApi>(GetProcAddress(m, g_nameApi.c_str()));
  const auto wanted = reinterpret_cast<PFN_DcsVr_QuadViewsDepthWanted>(GetProcAddress(m, g_nameWanted.c_str()));
  if (!api || !wanted) {
    g_noExport++;
  } else if (api() != 1) {
    g_badApi++;
  } else {
    mask = wanted();
  }
  return mask;
}

// The wanted mask (0 when the layer or the export is missing or the
// interface version is not 1).
uint32_t QvWanted() {
  HMODULE m = RefLayer();
  if (!m) {
    g_noLayer++;
    return 0;
  }
  const uint32_t mask = WantedOf(m);
  FreeLibrary(m);
  return mask;
}

// Pushes one descriptor; returns the layer's result, or ~0u when the layer
// or the export is missing (nothing was handed over).
uint32_t QvPush(const DcsVrDepthView& d) {
  if (g_frameMod) {  // R91: this frame's resolution (DecideFrame)
    if (!g_frameDepth) {
      g_noExport++;
      return ~0u;
    }
    return g_frameDepth(&d);
  }
  HMODULE m = RefLayer();
  if (!m) {
    g_noLayer++;
    return ~0u;
  }
  uint32_t r = ~0u;
  if (const auto fn = reinterpret_cast<PFN_DcsVr_QuadViewsDepth>(GetProcAddress(m, g_nameDepth.c_str())))
    r = fn(&d);
  else
    g_noExport++;
  FreeLibrary(m);
  return r;
}

bool QvRelease(uint32_t generation) {
  HMODULE m = RefLayer();
  if (!m) return false;
  const auto fn = reinterpret_cast<PFN_DcsVr_QuadViewsDepthRelease>(GetProcAddress(m, g_nameRelease.c_str()));
  if (fn) fn(generation);
  FreeLibrary(m);
  return fn != nullptr;
}

// ---------------------------------------------------------------------------
// Projection from DCS's cPerView uploads (render thread)
// ---------------------------------------------------------------------------

// Buffers known to be cPerView (VS b7 at a GBufferPass end) with the byte
// offset of the struct inside them (*SetConstantBuffers1 first * 16).
struct KnownCb {
  ID3D11Resource* res = nullptr;  // identity only
  UINT base = 0;
  uint64_t serial = 0;  // pass serial of the last upload seen (0: none)
  float m[32] = {};     // bytes 192..320 of the struct: gViewInv, gProj (column-major)
  uint64_t lastUse = 0;
  // R71: gOrigin (float3 at byte 0x610 of the struct: DCS's floating render origin) with the matrices, when the
  // upload covers it: a boxed update over it, or a whole update of a binding known (span, from the bound constants)
  // to reach it. oSerial: the pass of that copy.
  UINT span = 0;        // bytes of the struct bound to the shader (cbNum * 16, or the buffer's size; 0 unknown)
  float o[3] = {};
  uint64_t oSerial = 0;
};
constexpr UINT kOriginOff = 0x610;  // cPerView gOrigin (cockpit_ipd.h kOffOrigin)
KnownCb g_known[4];
uint64_t g_passSerial = 0;  // GBufferPass begins (render thread)
uint64_t g_useClock = 0;

// One UpdateSubresource seen while armed: keep the matrices of a known
// cPerView buffer when the upload covers them.
void NoteUpload(ID3D11Resource* dst, UINT sub, const D3D11_BOX* box, const void* src) {
  if (!dst || !src || sub != 0) return;
  for (KnownCb& k : g_known) {
    if (k.res != dst) continue;
    const UINT lo = k.base + 192, hi = k.base + 320;
    UINT left = 0;
    if (box) {
      if (box->left > lo || box->right < hi) return;  // a partial update without the matrices
      left = box->left;
    }
    memcpy(k.m, static_cast<const uint8_t*>(src) + (lo - left), sizeof(k.m));
    k.serial = g_passSerial;
    const UINT olo = k.base + kOriginOff, ohi = olo + 12;
    if (box ? (box->left <= olo && box->right >= ohi) : k.span >= kOriginOff + 12) {  // R71
      memcpy(k.o, static_cast<const uint8_t*>(src) + (olo - left), sizeof(k.o));
      k.oSerial = g_passSerial;
    }
    g_uploadsSeen++;
    return;
  }
}

// Called at a GBufferPass begin (before the arm).
void BeginGBufferPass() { ++g_passSerial; }

// At a GBufferPass end: the CPU matrices of the bound cPerView when it was
// uploaded in this pass (logical P[r][c] at [r * 4 + c]); otherwise learns
// the buffer for the next frames and returns false.
// R71: origin / originOk (optional): this pass's gOrigin when the upload carried it.
// bufBytes: the buffer's ByteWidth (0 unknown), the span of a whole-buffer binding.
bool TakeProj(ID3D11Buffer* cb, UINT cbFirst, UINT cbNum, float proj[16], float viewInv[16], float* origin = nullptr,
              bool* originOk = nullptr, UINT bufBytes = 0) {
  if (originOk) *originOk = false;
  if (!cb) return false;
  const bool offsets = !(cbFirst == 0 && (cbNum == 4096 || cbNum == 0));
  const UINT base = offsets ? cbFirst * 16 : 0;
  ID3D11Resource* res = cb;
  ++g_useClock;
  for (KnownCb& k : g_known) {
    if (k.res != res || k.base != base) continue;
    k.lastUse = g_useClock;
    k.span = offsets ? cbNum * 16 : bufBytes;
    if (k.serial != g_passSerial || !g_passSerial) return false;
    for (int r = 0; r < 4; ++r)
      for (int c = 0; c < 4; ++c) {
        viewInv[r * 4 + c] = k.m[c * 4 + r];
        proj[r * 4 + c] = k.m[16 + c * 4 + r];
      }
    if (origin && originOk && k.oSerial == g_passSerial) {
      memcpy(origin, k.o, sizeof(k.o));
      *originOk = true;
    }
    return true;
  }
  KnownCb* slot = &g_known[0];
  for (KnownCb& k : g_known)
    if (!k.res || k.lastUse < slot->lastUse) slot = &k;
  *slot = KnownCb{};
  slot->res = res;
  slot->base = base;
  slot->span = offsets ? cbNum * 16 : bufBytes;
  slot->lastUse = g_useClock;
  return false;
}

// ---------------------------------------------------------------------------
// Our copies and the generation (render thread, or the uninstall)
// ---------------------------------------------------------------------------

struct Tex {
  ID3D11Texture2D* t = nullptr;
  DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN;
  UINT w = 0, h = 0;
};
Tex g_tex[2][kMaxSlots];  // [hmdView - 2][slot]
Tex g_pre[2][kMaxSlots];  // pre-overlay colour, [hmdView - 2][slot]
// The descriptor of a focus view whose depth was copied this frame, waiting
// for its pre-overlay colour (or its CopyToHMDPass / the frame end).
struct PendingPush {
  bool on = false, copied = false;  // copied: the pre-overlay copy was attempted (made or skipped)
  DcsVrDepthView d{};
  // 5c. The post-overlay copy's source: DCS's target (referenced from the pre
  // copy to the post copy), its subresource and box; our texture (2w wide).
  ID3D11Resource* postSrc = nullptr;
  UINT postSub = 0;
  D3D11_BOX postBox{};
  ID3D11Texture2D* postDst = nullptr;
};
// Drops a waiting descriptor and the post-overlay source reference.
void ClearPend(PendingPush& p) {
  if (p.postSrc) p.postSrc->Release();
  p = PendingPush{};
}
PendingPush g_pend[2];
ID3D11Resource* g_target[2] = {};  // learned colour target per focus view (identity only)
bool g_preLogged[2] = {};

// 5b. The copy point per focus view (render thread), decided from the first
// draw into the target in the view's first OverlayLDRPasses (cached).
enum PreMode : int { kPreLearn = 0, kPreAtViewport = 1, kPreAfterComposite = 2 };
constexpr int kLearnObs = 3;         // OverlayLDRPasses with a draw into the target
constexpr int kLearnMaxPasses = 30;  // learning OverlayLDRPasses without any: the viewport point
int g_preMode[2] = {};
int g_learnSeen[2] = {}, g_learnFull[2] = {}, g_learnEmpty[2] = {};
bool g_modeLogged[2] = {};
std::atomic<int> g_ovTraceLeft{0};  // [Perception] FarDepthOverlayTrace frames left
std::vector<std::string> g_ovTrace;  // under g_statMutex; Tick writes it to the log
std::atomic<uint64_t> g_cmpCopies{0}, g_cmpMiss{0}, g_cmpNoDraw{0}, g_cmpGaveUp{0}, g_learnCopies{0},
    g_drawArms{0}, g_drawRearms{0}, g_drawLost{0}, g_drawSkipForeign{0}, g_usSkipForeign{0};

// Draw entries (fixation_depth.h's in-pass draw hooks).
enum DrawKind {
  kDkDraw, kDkIndexed, kDkInstanced, kDkIndexedInstanced, kDkAuto, kDkIndexedIndirect, kDkInstancedIndirect,
  kDkCount
};
const char* const kDrawNames[kDkCount] = {"Draw",         "DrawIndexed",
                                          "DrawInstanced", "DrawIndexedInstanced",
                                          "DrawAuto",      "DrawIndexedInstancedIndirect",
                                          "DrawInstancedIndirect"};

// A full-screen draw: 3 or 4 vertices (3, 4 or 6 indices), one instance, the
// viewport covering at least 90 % of the w x h box in both directions.
bool FullScreenDraw(int kind, UINT count, UINT inst, const D3D11_VIEWPORT& vp, UINT w, UINT h) {
  bool n = false;
  switch (kind) {
    case kDkDraw: n = count == 3 || count == 4; break;
    case kDkInstanced: n = (count == 3 || count == 4) && inst == 1; break;
    case kDkIndexed: n = count == 3 || count == 4 || count == 6; break;
    case kDkIndexedInstanced: n = (count == 3 || count == 4 || count == 6) && inst == 1; break;
    default: return false;  // DrawAuto / indirect: the count is not known on the CPU
  }
  return n && w && h && vp.Width >= 0.9f * static_cast<float>(w) && vp.Height >= 0.9f * static_cast<float>(h);
}

// One learning observation of view `view` (2 or 3): its first draw into the
// target in an OverlayLDRPass was (not) full-screen. Returns the mode, which
// is decided after kLearnObs observations (majority).
int Learn(int view, bool full) {
  int& m = g_preMode[view - 2];
  if (m != kPreLearn) return m;
  ++g_learnSeen[view - 2];
  if (full) ++g_learnFull[view - 2];
  if (g_learnSeen[view - 2] >= kLearnObs)
    m = 2 * g_learnFull[view - 2] > g_learnSeen[view - 2] ? kPreAfterComposite : kPreAtViewport;
  return m;
}

// A learning OverlayLDRPass of `view` without a draw into the target.
int LearnEmpty(int view) {
  int& m = g_preMode[view - 2];
  if (m == kPreLearn && ++g_learnEmpty[view - 2] >= kLearnMaxPasses) m = kPreAtViewport;
  return m;
}

void ResetCopyPoint() {
  for (int i = 0; i < 2; ++i) {
    g_preMode[i] = kPreLearn;
    g_learnSeen[i] = g_learnFull[i] = g_learnEmpty[i] = 0;
    g_modeLogged[i] = false;
  }
}

void TraceLine(const char* l) {
  std::lock_guard<std::mutex> lock(g_statMutex);
  if (g_ovTrace.size() < 4000) g_ovTrace.emplace_back(l);
}
uint32_t g_gen = 0;
uint32_t g_held[8] = {};  // generations pushed (accepted) since their last release
int g_heldN = 0;
int g_unwanted = 0;  // consecutive frames without a wanted focus view

void NewGeneration() {
  uint32_t g = g_gen + 1;
  if (!g) g = 1;
  g_gen = g;
}

// Seeds the generation (payload load): a reloaded payload never repeats the
// id its predecessor may have left in the layer.
void SeedGeneration() {
  LARGE_INTEGER q;
  QueryPerformanceCounter(&q);
  g_gen = static_cast<uint32_t>(q.QuadPart * 2654435761ull) | 1u;
}

bool HaveTextures() {
  for (auto& v : g_tex)
    for (Tex& t : v)
      if (t.t) return true;
  for (auto& v : g_pre)
    for (Tex& t : v)
      if (t.t) return true;
  return false;
}

// Quad Views drops every generation we pushed (render thread unless the
// render thread is gone; see Uninstall).
void ReleaseHeld() {
  for (int i = 0; i < g_heldN; ++i) {
    g_releaseCalls++;
    QvRelease(g_held[i]);
  }
  g_heldN = 0;
}

void NoteHeld(uint32_t gen) {
  for (int i = 0; i < g_heldN; ++i)
    if (g_held[i] == gen) return;
  if (g_heldN == 8) {
    QvRelease(g_held[0]);  // never more than one or two in practice
    memmove(g_held, g_held + 1, sizeof(g_held) - sizeof(g_held[0]));
    --g_heldN;
  }
  g_held[g_heldN++] = gen;
}

// Releases the layer's references first, then ours; a new generation.
void ReleaseAll() {
  ReleaseHeld();
  const bool any = HaveTextures();
  for (PendingPush& p : g_pend) ClearPend(p);  // their textures go now
  for (auto& v : g_tex)
    for (Tex& t : v) {
      if (t.t) t.t->Release();
      t = Tex{};
    }
  for (auto& v : g_pre)
    for (Tex& t : v) {
      if (t.t) t.t->Release();
      t = Tex{};
    }
  if (any) {
    g_releases++;
    NewGeneration();
  }
}

// Our texture for (view, slot) matching DCS's depth; a mismatch recreates all
// of them under a new generation. R91: the view's first `slots` slots are made
// together (one stall, not one per slot at its first use).
ID3D11Texture2D* EnsureTex(ID3D11Device* dev, int view, int slot, DXGI_FORMAT fmt, UINT w, UINT h, int slots = 1) {
  Tex* ring = g_tex[view - 2];
  auto fits = [&](const Tex& t) { return t.t && t.fmt == fmt && t.w == w && t.h == h; };
  if (fits(ring[slot])) return ring[slot].t;
  for (int k = 0; k < kMaxSlots; ++k)
    if (ring[k].t && !fits(ring[k])) {
      ReleaseAll();  // size or format changed: the layer must not mix generations
      break;
    }
  D3D11_TEXTURE2D_DESC d{};
  d.Width = w;
  d.Height = h;
  d.MipLevels = 1;
  d.ArraySize = 1;
  d.Format = fmt;
  d.SampleDesc.Count = 1;
  d.Usage = D3D11_USAGE_DEFAULT;
  d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  const int n = std::clamp(std::max(slots, slot + 1), 1, kMaxSlots);
  for (int k = 0; k < n; ++k) {
    if (fits(ring[k])) continue;
    ID3D11Texture2D* nt = nullptr;
    if (FAILED(dev->CreateTexture2D(&d, nullptr, &nt)) || !nt) {
      if (k == slot) return nullptr;
      continue;  // retried at that slot's first use
    }
    g_texCreates++;
    ring[k] = Tex{nt, fmt, w, h};
  }
  return ring[slot].t;
}

// Our pre-overlay texture for (view, slot); a mismatch recreates it under a
// new generation (the layer's references to the old one are dropped first).
// R91: the view's first `slots` slots are made together (as EnsureTex).
ID3D11Texture2D* EnsurePreTex(ID3D11Device* dev, int view, int slot, DXGI_FORMAT fmt, UINT w, UINT h, int slots = 1) {
  Tex* ring = g_pre[view - 2];
  auto fits = [&](const Tex& t) { return t.t && t.fmt == fmt && t.w == w && t.h == h; };
  if (fits(ring[slot])) return ring[slot].t;
  bool released = false;
  for (int k = 0; k < kMaxSlots; ++k)
    if (ring[k].t && !fits(ring[k])) {
      if (!released) ReleaseHeld();
      released = true;
      ring[k].t->Release();
      ring[k] = Tex{};
    }
  if (released) {
    NewGeneration();
    g_releases++;
  }
  D3D11_TEXTURE2D_DESC d{};
  d.Width = w;
  d.Height = h;
  d.MipLevels = 1;
  d.ArraySize = 1;
  d.Format = fmt;
  d.SampleDesc.Count = 1;
  d.Usage = D3D11_USAGE_DEFAULT;
  d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  const int n = std::clamp(std::max(slots, slot + 1), 1, kMaxSlots);
  for (int k = 0; k < n; ++k) {
    if (fits(ring[k])) continue;
    ID3D11Texture2D* nt = nullptr;
    if (FAILED(dev->CreateTexture2D(&d, nullptr, &nt)) || !nt) {
      if (k == slot) return nullptr;
      continue;
    }
    g_texCreates++;
    ring[k] = Tex{nt, fmt, w, h};
  }
  return ring[slot].t;
}

// 5e. A view's coverage mode switches: its pre-overlay textures go the way a
// format change takes them (the layer's references first, then ours; a new
// generation). Called at the frame end, when nothing waits to be pushed.
void ReleasePreView(int view) {
  bool any = false;
  for (Tex& t : g_pre[view - 2]) any = any || t.t != nullptr;
  if (!any) return;
  ReleaseHeld();
  for (Tex& t : g_pre[view - 2]) {
    if (t.t) t.t->Release();
    t = Tex{};
  }
  NewGeneration();
  g_releases++;
}

// The typeless family of a colour format (a copy between members of one
// family is legal), else the format itself (R11G11B10_FLOAT has none).
DXGI_FORMAT TypelessOf(DXGI_FORMAT f) {
  switch (f) {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case DXGI_FORMAT_R8G8B8A8_UINT:
    case DXGI_FORMAT_R8G8B8A8_SNORM:
    case DXGI_FORMAT_R8G8B8A8_SINT:
      return DXGI_FORMAT_R8G8B8A8_TYPELESS;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
      return DXGI_FORMAT_B8G8R8A8_TYPELESS;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:
    case DXGI_FORMAT_R10G10B10A2_UNORM:
    case DXGI_FORMAT_R10G10B10A2_UINT:
      return DXGI_FORMAT_R10G10B10A2_TYPELESS;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
    case DXGI_FORMAT_R16G16B16A16_UNORM:
    case DXGI_FORMAT_R16G16B16A16_UINT:
    case DXGI_FORMAT_R16G16B16A16_SNORM:
    case DXGI_FORMAT_R16G16B16A16_SINT:
      return DXGI_FORMAT_R16G16B16A16_TYPELESS;
    default:
      return f;
  }
}

// The copied box: the viewport in the target, clamped to it (integer texels).
// False when nothing is left or a side exceeds 16 bits.
bool PreBox(const D3D11_VIEWPORT& vp, UINT texW, UINT texH, D3D11_BOX& box) {
  if (!(vp.Width > 0) || !(vp.Height > 0)) return false;
  const double x0 = std::max(0.0, std::floor(static_cast<double>(vp.TopLeftX) + 1e-4));
  const double y0 = std::max(0.0, std::floor(static_cast<double>(vp.TopLeftY) + 1e-4));
  const double x1 =
      std::min(static_cast<double>(texW), std::floor(static_cast<double>(vp.TopLeftX) + vp.Width + 0.5));
  const double y1 =
      std::min(static_cast<double>(texH), std::floor(static_cast<double>(vp.TopLeftY) + vp.Height + 0.5));
  if (x1 <= x0 || y1 <= y0 || x1 - x0 > 65535 || y1 - y0 > 65535) return false;
  box = D3D11_BOX{static_cast<UINT>(x0), static_cast<UINT>(y0), 0, static_cast<UINT>(x1), static_cast<UINT>(y1), 1};
  return true;
}

bool FormatOk(DXGI_FORMAT f) {
  return f == DXGI_FORMAT_R32G8X24_TYPELESS || f == DXGI_FORMAT_R24G8_TYPELESS || f == DXGI_FORMAT_R32_TYPELESS;
}

// ---------------------------------------------------------------------------
// The descriptor (pure; tests use it)
// ---------------------------------------------------------------------------

struct CaptureInfo {
  uint32_t hmdView = 0;
  uint64_t engineFrame = 0;
  uint32_t generation = 0;
  DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
  ID3D11Texture2D* texture = nullptr;
  UINT texW = 0, texH = 0;
  D3D11_VIEWPORT vp{};
  float proj[16] = {};
  float viewInv[16] = {};
  bool viewInvValid = false, afterWater = false, fallback = false;
};

// False when the projection is not a perspective one (nothing is pushed).
bool FillView(const CaptureInfo& c, DcsVrDepthView& d) {
  d = DcsVrDepthView{};
  d.structSize = sizeof(DcsVrDepthView);
  d.version = 1;
  d.hmdView = c.hmdView;
  d.engineFrame = c.engineFrame;
  d.displayTime = 0;  // the xrLocateViews pairing of DCS's frame is not known
  d.generation = c.generation;
  d.format = static_cast<uint32_t>(c.format);
  d.texture = c.texture;
  d.texWidth = c.texW;
  d.texHeight = c.texH;
  d.arraySlice = 0;
  d.sampleCount = 1;
  d.viewport[0] = c.vp.TopLeftX;
  d.viewport[1] = c.vp.TopLeftY;
  d.viewport[2] = c.vp.Width;
  d.viewport[3] = c.vp.Height;
  d.viewport[4] = c.vp.MinDepth;
  d.viewport[5] = c.vp.MaxDepth;
  memcpy(d.proj, c.proj, sizeof(d.proj));
  if (c.viewInvValid) memcpy(d.viewInv, c.viewInv, sizeof(d.viewInv));
  const Proj p = ParseProj(c.proj);
  if (!p.ok) return false;
  double f[4];
  DcsFov(p, f);
  for (int k = 0; k < 4; ++k) d.fovTan[k] = static_cast<float>(f[k]);
  d.flags = DCSVR_DEPTH_VALID | (p.reverse ? DCSVR_DEPTH_REVERSE_Z : 0) | (p.infinite ? DCSVR_DEPTH_INFINITE_FAR : 0) |
            (c.afterWater ? DCSVR_DEPTH_AFTER_WATER : 0) | (c.fallback ? DCSVR_DEPTH_FALLBACK_CAPTURE : 0) |
            (StencilSrvFormat(c.format) != DXGI_FORMAT_UNKNOWN ? DCSVR_DEPTH_HAS_STENCIL : 0) |
            (c.viewInvValid ? DCSVR_DEPTH_VIEWINV_VALID : 0) | DCSVR_DEPTH_IDENTITY_BY_COPYTOHMD;
  return true;
}

// Pushes the waiting descriptor of HMD view `view` (2 or 3) under the current
// generation (render thread).
void DoPush(int view) {
  PendingPush& p = g_pend[view - 2];
  if (!p.on) return;
  DcsVrDepthView d = p.d;
  const bool postWanted = p.postSrc != nullptr;
  ClearPend(p);
  if (postWanted && !(d.flags & DCSVR_DEPTH_PRE_POST_OVERLAY)) g_postMissing++;
  d.generation = g_gen;
  if (d.flags & DCSVR_DEPTH_HAS_PRE_OVERLAY)
    g_prePushed++;
  else if (g_preOn.load(std::memory_order_relaxed))
    g_preMissing++;
  const uint32_t res = QvPush(d);
  g_framePushed |= 1u << view;  // R91: the frame's module goes after its last wanted view
  if (g_frameMod && (g_framePushed & g_frameWant) == g_frameWant) DropFrameModule();
  if (res == ~0u) return;
  g_pushes++;
  if (res == DCSVR_DEPTH_PUSH_ACCEPTED) {
    g_accepted++;
    NoteHeld(d.generation);
  } else {
    g_rejected[std::min<uint32_t>(res, 7)]++;
  }
  if (!g_loggedFirst.exchange(true))
    Log("far depth: first push: HMD view %u, depth %ux%u format %u, viewport %.2f,%.2f %.0fx%.0f depth %.2f..%.2f, "
        "flags 0x%x, generation %u; Quad Views result %u",
        d.hmdView, d.texWidth, d.texHeight, d.format, d.viewport[0], d.viewport[1], d.viewport[2], d.viewport[3],
        d.viewport[4], d.viewport[5], d.flags, d.generation, res);
}

// ---------------------------------------------------------------------------
// Projection log (DLSS jitter measurement)
// ---------------------------------------------------------------------------

struct ProjLog {
  uint64_t frame = 0;
  uint32_t view = 0;
  float p20 = 0, p21 = 0, p23 = 0, p00 = 0, p11 = 0, vx = 0, vy = 0, vw = 0;
};
std::vector<ProjLog> g_projLog;  // under g_statMutex; Tick writes it to the log

void LogProj(uint64_t frame, uint32_t view, const float P[16], const D3D11_VIEWPORT& vp) {
  std::lock_guard<std::mutex> lock(g_statMutex);
  if (g_projLog.size() >= 512) return;
  g_projLog.push_back(ProjLog{frame, view, P[8], P[9], P[11], P[0], P[5], vp.TopLeftX, vp.TopLeftY, vp.Width});
}

// ---------------------------------------------------------------------------
// Frame logic (render thread)
// ---------------------------------------------------------------------------

uint32_t g_mask = 0;          // this frame's wanted focus views
bool g_decided = false;       // the Wanted call of this frame was made

// At the frame's first pass boundary.
void DecideFrame() {
  if (g_decided) return;
  g_decided = true;
  DropFrameModule();  // (a frame whose end did not come)
  g_mask = 0;
  if (Active()) {
    // R91: one module reference and one resolution per frame (Wanted, then this frame's pushes).
    if (HMODULE m = RefLayer()) {
      g_mask = WantedOf(m) & kFocusMask;
      if (g_mask) {
        g_frameMod = m;
        g_frameDepth = reinterpret_cast<PFN_DcsVr_QuadViewsDepth>(GetProcAddress(m, g_nameDepth.c_str()));
        g_frameWant = g_mask;
        g_framePushed = 0;
      } else {
        FreeLibrary(m);
      }
    } else {
      g_noLayer++;
    }
  }
  g_lastMask = g_mask;
}

bool g_loggedThisFrame = false;  // a projection log entry was made this frame
bool g_tracedThisFrame = false;  // an overlay trace window was open this frame

// The upload capture is armed in GBufferPasses only when something uses it.
// R65: cockpit_mask.h wants this pass's gProj too (the mask rect's FOV), with or without the export to Quad Views.
std::atomic<bool> g_maskUploads{false};
bool WantUploads() {
  return (Active() && (g_mask || g_logProjLeft.load() > 0)) || g_maskUploads.load(std::memory_order_relaxed);
}

// Install: a clean render-thread state (no texture is held here: ReleaseAll ran at the uninstall).
void ResetFrameState() {
  g_mask = 0;
  g_lastMask = 0;
  g_decided = false;
  g_loggedThisFrame = false;
  g_tracedThisFrame = false;
  g_unwanted = 0;
  g_passSerial = 0;
  g_useClock = 0;
  for (KnownCb& k : g_known) k = KnownCb{};
  for (PendingPush& p : g_pend) ClearPend(p);
  DropFrameModule();  // R91
  g_target[0] = g_target[1] = nullptr;
  g_preLogged[0] = g_preLogged[1] = false;
  ResetCopyPoint();
  ResetHdrWatch();
}

// At the frame end (fixation_depth.h EndFrame, before the frame counter moves).
void EndFrame() {
  // A view still waiting here missed its CopyToHMDPass: the frame end comes
  // after xrEndFrame, so a push now would be read with the next frame. Dropped.
  for (PendingPush& p : g_pend) {
    if (p.on) g_dropped++;
    ClearPend(p);
  }
  g_frames++;
  if (g_mask) {
    g_wantedFrames++;
    g_unwanted = 0;
  } else if (++g_unwanted >= kUnwantedRelease && (HaveTextures() || g_heldN)) {
    ReleaseAll();
  }
  if (g_loggedThisFrame) {
    g_loggedThisFrame = false;
    if (g_logProjLeft.load() > 0) g_logProjLeft--;
  }
  if (g_tracedThisFrame) {
    g_tracedThisFrame = false;
    if (g_ovTraceLeft.load() > 0) g_ovTraceLeft--;
  }
  DropFrameModule();  // R91: the frame's module reference (DecideFrame)
  g_decided = false;
}

}  // namespace fdexport
