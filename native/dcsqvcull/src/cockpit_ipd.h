// Cockpit at the true eye distance while Depth boost renders the world at the
// boosted one ([Perception] CockpitTrueIpd, default 0; design: docs/research
// R31_dual_camera_boost.md, option 3). Not a performance optimization: outside
// the kill switch, not one of the status word's features.
//
// Approach A of the Perception contract (dcsvr_perception_shm.h, BoostOut, a
// verbatim copy of the layer's perception_shm.h): the pupil shift layer keeps
// returning boosted eye poses from xrLocateViews, each eye moved along the
// eye axis by h = (renderedIpdM - baseIpdM) / 2 in every view of that eye. DCS
// renders the whole frame with them. This module moves only the cockpit-class
// passes of HMD views 0-3 back to the true eye by rewriting the bytes of their
// cPerView upload (register b7, 0x970 bytes, uploaded with UpdateSubresource
// inside the pass by ContextBase::updateBuffers); every other pass, the world,
// shadows and MFD views keep DCS's bytes (the per-view overlay passes: below).
// Every failure forwards DCS's own bytes, i.e. today's frame.
//
// Which passes (R31 3.2):
//  - GBufferPass executions with shading model 4 (SM_GBUFFER_PBR_COCKPIT),
//    classified in gbpass::Hook (gb_batch.h, slot 19) before DCS's binder
//    uploads cPerView inside the same execution: gbpass::ItemVector ->
//    gbrec::ExecKeyGuarded -> CollKey.sm (tag 600 only counted as a cross
//    check). The HMD view is fixation_depth.h's CopyToHMDPass ordinal.
//  - TransparentCockpitPass / TransparentCockpitDecalPass (any name containing
//    "TransparentCockpit"), only when that view's sm 4 G-buffer pass was
//    patched this frame ([Perception] CockpitTrueIpdForward, default 1). Their
//    upload entries are armed alone, with the OM / viewport entries as re-arm
//    points, or nested in another feature's in-pass arm (ArmUsOnly in
//    fixation_depth.h).
//  - The cockpit symbology of the per-HMD-view overlay passes OverlayHDRPass /
//    OverlayLDRPass ([Perception] CockpitTrueIpdOverlays, default 1; with
//    HUD_MFD_after_DLSS = true DCS draws it after DLSS). R32 decided per element
//    which camera it needs (a symbol uses the camera of the object it is
//    attached to): cockpit-surface symbology (DED, MFD LCD quads, EHSI) the
//    true eye, with the bezels; the collimated HUD and its DED repeater DCS's
//    own boosted camera. DCS builds the HUD per eye at infinity (O = E_t + d F):
//    with its own camera its disparity against the far horizon is 0 px, moved
//    to the true eye it is 20 px (the HUD "jump" of 2026-10-13 was the correct
//    image showing through in frames our hooks missed). The claim made here
//    before R32, that rewriting the camera's translation leaves the HUD alone
//    because it is drawn at w = 0, was wrong: DCS's symbology vertices are w = 1
//    (MaterialBase.hlsl ILS: float4(pos, 1) * matWorldViewProj).
//    [Perception] CockpitTrueIpdOverlayPath=cpu (default, ovl_cpu.h): DCS's
//    per-record MVP camera (currentContext+0x1b4) is set in its record replay
//    and record filter hooks, per learned record class; the d3d11 hooks of the
//    overlay passes only observe (the learner's samples, the cpu-miss count),
//    write nothing, and b7 stays DCS's. A d3d11 table toggle therefore cannot
//    change any image. The overlay passes are armed (as an observer) only for an
//    HMD view 0-3 whose sm 4 G-buffer pass was patched this frame with Dw != 0.
//    =d3d11 (A/B only): the path before R32. The view's cPerView uploads in
//    those passes (its own camera only: MFD sensor views, MFDOverlayPass and 2D
//    GUI cameras are never touched) and every MVP of the view's boosted camera
//    found in other constant-buffer uploads (X = MVP * VP^-1 affine; patched as
//    row 3 += (-Dw, 0) * VP, rows 0..2 bit-identical) are rewritten to the true
//    eye. It moves the HUD (wrong, above) and world labels drawn in these
//    passes, and it can never be complete: FX11 uploads the effect buffer before
//    every draw, and after a d3d11 toggle (Flush, ExecuteCommandList, GetData, a
//    waiting Map, or a submit inside a state call) the next upload goes out
//    before any hook of ours runs. fixation_depth.h repairs toggles at its
//    toggle points (also the "normal, not ours" state left by the first call
//    through a toggled entry, R32 4.5) and counts what it lost; an overlay pass
//    that ends with the upload entries lost counts as failed (> 1 % over 300
//    frames: overlays at DCS's camera for the load). Not restored at the pass
//    end (CockpitTrueIpdOverlayRestore=1 restores; R32: DCS re-uploads before
//    every draw, so both draw the same).
//    [Perception] CockpitTrueIpdMvpDump=N logs the first N recognised
//    matrices; CockpitTrueIpdFrameDump=N writes N frames of the focus views
//    for tools\flicker_check.py; the first kOvlDrawProbe overlay passes count
//    draws with an effect buffer restored and not uploaded again (d3d11 path).
//  - Nothing else (CockpitGlassEffects only after the live L0 trace shows it
//    uploads its own b7; the trace below reports that).
// One h per view and frame: every class of a view (G-buffer, lighting split,
// forward, overlay) uses that view's Dw / Dp, fixed at its first cockpit
// G-buffer upload (ComputeDw); a class of a view whose G-buffer was not
// patched is not patched either. An ambiguous scale step gives view 0 the
// smallest plausible shift and views 1-3 the verified record (Matcher::Latch),
// each view consistent in itself.
//
// The patch (R31 3.4), in double, written back as float, matrices stored
// transposed (logical M[r][c] at f[c * 4 + r]), row vectors:
//   gView        T(-Dw) * V           gViewInv      VI * T(Dw)
//   gViewProj    T(-Dw) * VP          gViewProjInv  VPI * T(Dw)
//   gPrevFrameViewProj  T(-Dp) * PVP (PVP: DCS's own gPrevFrameViewProj)
//   gPrevFrameTransform += outer(A[:, 3], (Dw - Dp, 0) * PVP), A = DCS's
//                (current clip -> world) map of X = A * PVP: gViewProjInv, or
//                X * PVP^-1 when X was built with another inverse
//   gCameraPos   xyz += Dw
// gProj (DLSS jitter), gProjInv and every other byte are DCS's. A cell whose
// value does not change keeps its exact bits.
//   Dw = sigma * h * k * u: sigma +1 for views 0 and 2 (left eye, moved back to
//   its right), -1 for views 1 and 3; h from the frame latch; k DCS units per
//   metre (1 unless the learned estimate is more than 1 % off); u the eye axis,
//   learned as a head-frame constant from views 0/1 (unit(C1 - C0) in view 0's
//   camera frame, 120 frames), the view's own right axis until then.
//   Dp = the world translation between DCS's gViewProj of the same view in
//   the previous frame and the gViewProj we really sent for it (a per-view
//   store: W = sent * DCS's^-1 must be a pure translation T(-Dp); 0 when that
//   view was not patched). It must agree with the Dw recorded for that view
//   within 0.02 mm; Dp is then that recorded vector exactly. A frame whose Dw
//   is 0 with Dp != 0 patches only the two gPrev fields (tail).
// Why T(-Dp) * PVP is exact (R31 R6): any projective camera matrix M has one
// centre c ((c, 1) * M has x = y = w = 0), and T(-d) * M is the same camera
// with its centre at c + d. DLSS jitter, an unjittered or other projection
// (near / far), and a camera-origin rebase (a translation, which commutes with
// T(-Dp)) all keep that relation, so the correction is exact whenever PVP's
// centre is the previous camera of this view's eye. It is not when PVP is
// another eye's or a centre camera. The check: the centre of PVP minus DCS's
// previous camera of the view (gViewInv row 3, stored), d_t, is one common
// translation for every view of the frame (a rebase, or 0); view 0 is the
// reference, views 1-3 must agree within 8 mm (the other eye is ~65 mm away).
// A view whose check fails, or whose store disagrees with the recorded Dw, is
// not patched (today's image; the frame then follows the failure rules
// below), and every later frame stays unpatched until 30 frames agree again
// (no alternating frames). Leaving PVP as DCS's while gViewProj moves is not
// a fallback: the cockpit is a model whose motion vectors use PVP, so it would
// carry a parallax of Dw (tens of pixels) in every frame, not only when h
// changes. gPrevFrameTransform alone is shift-invariant (unchanged when
// Dw == Dp); gPrevFrameViewProj is not.
// Self-checks per upload (failure: not patched, counted; > 1 % of the patch
// attempts over 300 frames latches the option inert): |gViewInv row 0| in
// [0.99, 1.01], gViewProj == gView * gProj within 1e-4 relative, finite
// matrices; |gCameraPos - gViewInv row 3| < 1 m is logged only.
// Restore: at the end of each pass that forwarded a patched upload, DCS's
// original bytes are uploaded again into the same buffer and box (one extra
// upload, ordered by D3D11 after the pass's draws), so later binder-less
// passes and copies see DCS's content. Every later upload of such a buffer
// in the pass that is forwarded as DCS's is merged into the restore entry, so
// the restore never writes older bytes over newer ones. A struct whose
// buffer holds several learned structs (offset binding) is the one the box
// covers alone, else the one b7 binds now; else it is forwarded (ambiguous).
//
// The observe flag and h are latched once per frame (a mode change applies
// from the next frame, so Dp always describes what was really applied).
// Frame pairing (R31 3.5): BoostOut (Local\DcsVrDepthBoost_v1 plus the
// DCSVR_PERCEPTION_SHM_SUFFIX suffix) opened read-only only when the option is
// on, retried once per second, seqlock read (3 tries) at each top-level pass
// boundary, new records into a 16-entry ring. One latch per frame at view 0's
// cockpit upload, used by all views and passes: (1) all candidates (records
// newer than the previous latch, the newest and the 3 a lag can pick) agree
// within 0.05 mm: the newest; (2) the FOV fingerprint: view 0's FOV from this upload's gProj
// against a GazeOut ring, decisive only (pupil shift on); (3) the learned lag
// L (0..2 records behind the newest); when the records around L differ by
// more than 2 mm in h (a scale step), view 0 gets the smallest of them and
// views 1-3 the record |C1 - C0| verifies (Matcher::Latch explains the
// trade-off). Verification at view 1's upload: the
// measured |C1 - C0| / k against each lag's record plus a learned bias; a lag
// better by 0.3 mm for 3 frames of a changing scale is adopted; > 3 mm at every
// lag for 10 frames latches pairFault (h = 0) until 60 frames agree again.
// Latches without a new record (DCS locates the next frame on another thread,
// so its record sometimes lands before this frame's latch: that latch sees
// two, the next none; live 2026-10-13: 143 such pairs in 13531 frames, each
// had gone "dry", h 0 for the whole frame: the cockpit, HUD and MFD jumped by
// h for one frame about once a second): the record / latch phase (Matcher::
// ahead) names the frame's own record; a latch with none is served from the
// ring for up to kCarryMax latches beyond the lag.
// Fallbacks (h = 0, the tail frame still runs): no block, stale (no new
// record for 30 frames, or none for more than lag + kCarryMax latches: dry),
// enabled/applied clear, scale within 1e-3 of 1,
// h < 0.05 mm, h > 40 mm, pairFault, k outside [0.95, 1.05], axis sign wrong,
// self-check rate. Whole frame unpatched: foreign table hooks at the latch
// (gpu_pass_timing / gpu_dedup / D3D meter; configured at load: refused),
// view 0's cockpit pass not handled, a second sm 4 pass in a view or more than
// four; a frame that patched some views and failed a later one forces the
// next frame off; so does a forward cockpit pass of a moved view that was not
// armed, lost its arm or forwarded a cPerView upload unpatched (also counted
// with the self-check rate). Such an off frame (offNext, offMulti, offView0;
// not foreign hooks or a refuted gPrevFrameViewProj) still sends the gPrev
// tail (Dw 0, Dp from the store) for each view whose previous frame was
// moved, so DLSS's motion vectors match its history (TailOff). SEH anywhere
// (or a fixation depth fault): off for this load; a pass of ours open at the
// fault is still closed (restore, arm).
// [Perception] CockpitTrueIpd=2 (observe): everything above runs and is
// logged, nothing is modified. CockpitTrueIpdLog=N: a per-pass trace for N
// frames (view, pass, sm, tag, uploads of a known cPerView, Dw, restores) also
// of every pass whose name contains "Cockpit". CockpitTrueIpdPrevLog=N: for N
// frames (at most 150), per HMD view's first cockpit upload, "pv <frame> v<t>"
// lines: gViewProj, gPrevFrameViewProj, gProj (with the jitter), gOrigin /
// gCameraPos / gViewInv row 3, the stored gViewProj of the same view in the
// previous frame (DCS's and the one sent), Y = prevVP^-1 * PVP (a clip-space
// relation: jitter shows in Y[3][0..1]) and X = PVP * prevVP^-1 (a world
// relation: a rebase shows as X row 3), the centre offset d_t and the
// verdict. CockpitTrueIpdTestShiftMm:
// shift the cockpit passes by that many mm per eye without the layer
// (ignored while BoostOut reports a real boost).
// Phase 2 (CockpitTrueIpdLighting=1, default 0, R31 3.7): during a patched
// HMD view's GBufferLightingPass (in-pass arm: OM / viewport / upload and the
// draw entries) the pass's cPerView upload goes to the GPU at the true eye
// (restored at the pass end), and a non-indexed full-screen Draw(3|4, 0)
// whose PS b7 is a known cPerView buffer, which reads the cockpit pass's
// depth texture at a PS SRV, and whose RTV matches that texture's size, is
// drawn twice through a read-only DSV on it: stencil NOT_EQUAL 0x28 (mask
// 0x38) with a private b7 holding DCS's bytes (world pixels, DCS's camera),
// then EQUAL with the bound b7 (cockpit pixels, the true eye). DSV,
// depth-stencil state and ref, VS/PS b7 are put back exactly (also after a
// fault inside: SplitCap). MSAA depth, a bound state that already uses
// stencil or writes depth, compute and other draws: drawn once with the
// true eye's b7 (the cockpit right, the world from the true eye: ~13 mm).
// The rest of this paragraph is history (2026-10-13), kept for the record:
// a lighting draw that went past the draw hook (d3d11 toggles the draw slots
// after a Flush / ExecuteCommandList; live 2026-10-13: ~0.3 % of the patched
// views, one shadow "flash" per second) left that view's cockpit lit from
// the boosted eye for one frame while its G-buffer was at the true eye. The
// upload hooks re-arm the draw entries too (the binder's cPerView upload is
// the last call before the draw). A switch can still come later (inside one
// of the effect's state calls, which are not hooked). Such an arm is
// repaired once the pass is learned as one full-screen Draw (16 arms with
// exactly one draw, split; never more than one):
//  - at the frame-buffer change: DCS pops the pass's frame buffer (back to
//    its 816x804 default) inside the pass, before the pass-end callback, so
//    the old pass-end repair always found the wrong render target bound
//    (live 2026-10-13: repaired 0 of 46). The OM hooks (armed for the pass)
//    see that call before it is forwarded: no draw seen yet and the lighting
//    frame buffer recorded at its bind still bound = the whole draw state is
//    still the lighting draw's: the repair runs there;
//  - else at the pass end with the recorded frame buffer and viewport bound
//    again for the repair (the OM hooks lost too), then DCS's put back.
// The repair: blending off on every bound target: draw 2 alone (cockpit
// pixels, private b7) over DCS's draw gives the split's image. Additive
// blending (ONE / ONE / ADD per channel group, others ONE / ZERO) on float
// targets: first DCS's contribution taken out again on the cockpit pixels
// (DCS's b7, the same blend with REV_SUBTRACT), then draw 2 with DCS's blend.
// Any other blending, or the checks of the split: not repaired, counted by
// reason. Neither repair ever succeeded live (DCS unbinds the draw's SRVs
// before both points); with the b7 at the true eye such a draw lights the
// cockpit right by itself, and the repairs are gone. Every view-frame whose
// G-buffer moved with Dw != 0 but whose cockpit was lit from the boosted eye
// is counted with its reason (lightMiss).
// Included from fixation_depth.h inside namespace fixdepth (after State, the
// shared-memory globals and far_depth.h).
#pragma once

namespace ckipd {

namespace psh = ::dcsvr_perception;

// ---------------------------------------------------------------------------
// Configuration (config thread writes; render thread reads)
// ---------------------------------------------------------------------------

std::atomic<int> g_cfgMode{0};  // 0 off, 1 on, 2 observe
std::atomic<bool> g_cfgLighting{false}, g_cfgForward{true}, g_cfgForeign{false};
std::atomic<bool> g_cfgOverlays{true};  // [Perception] CockpitTrueIpdOverlays
std::atomic<bool> g_cfgOvlMvp{true};    // [Perception] CockpitTrueIpdOverlayMvp: the effect buffers' MVPs too
std::atomic<bool> g_cfgOvlRestore{false};  // [Perception] CockpitTrueIpdOverlayRestore: DCS's bytes back after each pass
// [Perception] CockpitTrueIpdOverlayPath (R32): cpu (default) sets DCS's MVP camera per record where DCS builds it
// (ovl_cpu.h; the d3d11 hooks of the overlay passes only observe); d3d11 is the path before R32 (the uploaded
// cPerView and effect MVPs rewritten: it also moves the collimated HUD, R32 2.6), kept for A/B only.
enum : int { kOvlPathCpu = 0, kOvlPathD3d11 = 1 };
std::atomic<int> g_cfgOvlPath{kOvlPathCpu};
inline bool PathCpu() { return g_cfgOvlPath.load(std::memory_order_relaxed) == kOvlPathCpu; }
std::atomic<double> g_cfgShiftMm{0.0};
std::atomic<int> g_logLeft{0};
std::atomic<int> g_prevLogLeft{0};  // [Perception] CockpitTrueIpdPrevLog: frames left of the gPrev trace
std::atomic<bool> g_on{false};     // started: the render-thread paths run
std::atomic<bool> g_fault{false};  // SEH: off for this load
std::atomic<int> g_busy{0};
// In-flight A/B ([Perception] CockpitTrueIpdToggle): true leaves the cockpit at
// the layer's boosted eye (today's image) until pressed again.
std::atomic<bool> g_userOff{false};
// Seams and diagnostics (the off-path tests read them).
std::atomic<uint64_t> g_gbInstallCalls{0}, g_boostOpens{0}, g_starts{0};
std::wstring g_boostNameOverride;  // tests: their own mapping name

inline bool Observe() { return g_cfgMode.load(std::memory_order_relaxed) == 2; }
inline bool Wanted() { return g_cfgMode.load() != 0 && !g_cfgForeign.load(); }
// The render-thread paths patch, split or classify only while this is true:
// started, no fault of ours and none of fixation_depth.h's (whose boundary
// callback then stops; a pass of ours that is still open is closed by its
// fault path, OnBoundary). g_on stays set after a fault: only Stop clears it,
// after draining g_busy, so a pass open at the fault is still closed (its
// restore and its arm) on the render thread.
inline bool Active() {
  return g_on.load(std::memory_order_relaxed) && !g_fault.load(std::memory_order_relaxed) &&
         !::fixdepth::g_fault.load(std::memory_order_relaxed);
}
// The cPerView upload entries are armed in G-buffer passes only while this is true.
inline bool WantUploads() { return Active(); }

// ---------------------------------------------------------------------------
// cPerView layout and the patch (pure)
// ---------------------------------------------------------------------------

constexpr uint32_t kCbBytes = 0x970;
constexpr uint32_t kOffView = 0x080, kOffViewInv = 0x0C0, kOffProj = 0x100, kOffProjInv = 0x140,
                   kOffViewProj = 0x180, kOffViewProjInv = 0x1C0, kOffPrevXf = 0x200, kOffPrevVP = 0x240,
                   kOffOrigin = 0x610, kOffCamPos = 0x620;
constexpr uint32_t kMaxSrc = 0x4000;  // largest upload we copy (DCS's buffer is 0x970)

struct M4 {
  double a[4][4];
};
inline M4 LoadM(const uint8_t* s, uint32_t off) {
  float f[16];
  memcpy(f, s + off, sizeof(f));
  M4 m;
  for (int r = 0; r < 4; ++r)
    for (int c = 0; c < 4; ++c) m.a[r][c] = f[c * 4 + r];
  return m;
}
inline void StoreM(uint8_t* s, uint32_t off, const M4& m) {
  float f[16];
  for (int r = 0; r < 4; ++r)
    for (int c = 0; c < 4; ++c) f[c * 4 + r] = static_cast<float>(m.a[r][c]);
  memcpy(s + off, f, sizeof(f));
}
inline M4 MulM(const M4& x, const M4& y) {
  M4 m;
  for (int r = 0; r < 4; ++r)
    for (int c = 0; c < 4; ++c) {
      double v = 0;
      for (int k = 0; k < 4; ++k) v += x.a[r][k] * y.a[k][c];
      m.a[r][c] = v;
    }
  return m;
}
inline bool NonZero(const double v[3]) { return v[0] != 0 || v[1] != 0 || v[2] != 0; }
// Gauss-Jordan with partial pivoting; false when singular or not finite.
inline bool Inv4(const M4& in, M4* out) {
  double a[4][8];
  double sc = 0;
  for (int r = 0; r < 4; ++r)
    for (int c = 0; c < 8; ++c) {
      a[r][c] = c < 4 ? in.a[r][c] : (c - 4 == r ? 1.0 : 0.0);
      if (c < 4) sc = std::max(sc, std::fabs(in.a[r][c]));
    }
  if (!(sc > 0) || !std::isfinite(sc)) return false;
  for (int c = 0; c < 4; ++c) {
    int p = c;
    for (int r = c + 1; r < 4; ++r)
      if (std::fabs(a[r][c]) > std::fabs(a[p][c])) p = r;
    if (!(std::fabs(a[p][c]) > 1e-12 * sc)) return false;
    if (p != c)
      for (int k = 0; k < 8; ++k) std::swap(a[c][k], a[p][k]);
    const double d = a[c][c];
    for (int k = 0; k < 8; ++k) a[c][k] /= d;
    for (int r = 0; r < 4; ++r)
      if (r != c) {
        const double f = a[r][c];
        if (f != 0)
          for (int k = 0; k < 8; ++k) a[r][k] -= f * a[c][k];
      }
  }
  for (int r = 0; r < 4; ++r)
    for (int c = 0; c < 4; ++c) {
      out->a[r][c] = a[r][c + 4];
      if (!std::isfinite(out->a[r][c])) return false;
    }
  return true;
}
// The centre of a projective camera matrix (row vectors): the point c with
// (c, 1) * M having x = y = w = 0. Independent of a clip-space jitter (x +=
// jx * w), of the depth mapping (near / far) and of the projection's shape; a
// world translation T(-d) * M moves it by +d. False when degenerate.
inline bool Centre(const M4& m, double c[3]) {
  const int cols[3] = {0, 1, 3};
  double A[3][3], b[3], sc = 0;
  for (int j = 0; j < 3; ++j) {
    for (int i = 0; i < 3; ++i) {
      A[j][i] = m.a[i][cols[j]];
      sc = std::max(sc, std::fabs(A[j][i]));
    }
    b[j] = -m.a[3][cols[j]];
  }
  auto det3 = [](const double M[3][3]) {
    return M[0][0] * (M[1][1] * M[2][2] - M[1][2] * M[2][1]) - M[0][1] * (M[1][0] * M[2][2] - M[1][2] * M[2][0]) +
           M[0][2] * (M[1][0] * M[2][1] - M[1][1] * M[2][0]);
  };
  const double d = det3(A);
  if (!(sc > 0) || !(std::fabs(d) > 1e-12 * sc * sc * sc) || !std::isfinite(d)) return false;
  for (int k = 0; k < 3; ++k) {
    double B[3][3];
    for (int j = 0; j < 3; ++j)
      for (int i = 0; i < 3; ++i) B[j][i] = i == k ? b[j] : A[j][i];
    c[k] = det3(B) / d;
    if (!std::isfinite(c[k])) return false;
  }
  return true;
}
// a += t, leaving a's exact bits (and the sign of a zero) when t is 0.
inline void AddTo(double& a, double t) {
  if (t != 0) a += t;
}

// Moves the camera of one cPerView struct (s: its first byte) by dw and the
// previous frame's camera by dp, world units (R31 3.4). Pure.
enum : uint32_t {
  kFView = 1, kFViewInv = 2, kFViewProj = 4, kFViewProjInv = 8, kFPrevXf = 16, kFPrevVP = 32, kFCamPos = 64,
  kFXfAlt = 128  // gPrevFrameTransform was not gViewProjInv * PVP: its own clip -> world map was used
};
// DCS's clip -> world map A of gPrevFrameTransform (X = A * PVP), column 3:
// gViewProjInv's, unless X * PVP^-1 says otherwise (an unjittered or other
// inverse; jitter itself leaves column 3 alone). alt: the latter was used.
inline void XfColumn(const M4& vpi, const M4& x, const M4& pvp, double m[4], bool* alt) {
  *alt = false;
  double sc = 1;
  for (int r = 0; r < 4; ++r) {
    m[r] = vpi.a[r][3];
    sc = std::max(sc, std::fabs(m[r]));
  }
  M4 pinv;
  if (!Inv4(pvp, &pinv)) return;
  const M4 a = MulM(x, pinv);
  double d = 0, sa = 1;
  for (int r = 0; r < 4; ++r) {
    if (!std::isfinite(a.a[r][3])) return;
    d = std::max(d, std::fabs(a.a[r][3] - m[r]));
    sa = std::max(sa, std::fabs(a.a[r][3]));
  }
  if (d <= 1e-3 * std::max(sc, sa)) return;
  for (int r = 0; r < 4; ++r) m[r] = a.a[r][3];
  *alt = true;
}
uint32_t PatchStruct(uint8_t* s, const double dw[3], const double dp[3]) {
  uint32_t changed = 0;
  const double e[3] = {dw[0] - dp[0], dw[1] - dp[1], dw[2] - dp[2]};
  const M4 vpi = LoadM(s, kOffViewProjInv);  // DCS's
  const M4 pvp = LoadM(s, kOffPrevVP);       // DCS's
  if (NonZero(dw)) {
    auto rowSub = [&](uint32_t off) {  // T(-dw) * M: row 3 -= sum_j dw_j * row j
      M4 m = LoadM(s, off);
      for (int c = 0; c < 4; ++c) AddTo(m.a[3][c], -(dw[0] * m.a[0][c] + dw[1] * m.a[1][c] + dw[2] * m.a[2][c]));
      StoreM(s, off, m);
    };
    auto colAdd = [&](uint32_t off) {  // M * T(dw): [r][c < 3] += M[r][3] * dw_c
      M4 m = LoadM(s, off);
      for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 3; ++c) AddTo(m.a[r][c], m.a[r][3] * dw[c]);
      StoreM(s, off, m);
    };
    rowSub(kOffView);
    colAdd(kOffViewInv);
    rowSub(kOffViewProj);
    colAdd(kOffViewProjInv);
    float cp[3];
    memcpy(cp, s + kOffCamPos, sizeof(cp));
    for (int i = 0; i < 3; ++i)
      if (dw[i] != 0) cp[i] = static_cast<float>(static_cast<double>(cp[i]) + dw[i]);
    memcpy(s + kOffCamPos, cp, sizeof(cp));
    changed |= kFView | kFViewInv | kFViewProj | kFViewProjInv | kFCamPos;
  }
  if (NonZero(e)) {  // A' * PVP' = A * T(dw - dp) * PVP = A * PVP + A[:, 3] (x) (e, 0) * PVP (A = VPI normally)
    double row[4], col[4];
    for (int c = 0; c < 4; ++c) row[c] = e[0] * pvp.a[0][c] + e[1] * pvp.a[1][c] + e[2] * pvp.a[2][c];
    M4 x = LoadM(s, kOffPrevXf);
    bool alt = false;
    XfColumn(vpi, x, pvp, col, &alt);
    for (int r = 0; r < 4; ++r)
      for (int c = 0; c < 4; ++c) AddTo(x.a[r][c], col[r] * row[c]);
    StoreM(s, kOffPrevXf, x);
    changed |= kFPrevXf | (alt ? kFXfAlt : 0u);
  }
  if (NonZero(dp)) {
    M4 q = pvp;
    for (int c = 0; c < 4; ++c) AddTo(q.a[3][c], -(dp[0] * pvp.a[0][c] + dp[1] * pvp.a[1][c] + dp[2] * pvp.a[2][c]));
    StoreM(s, kOffPrevVP, q);
    changed |= kFPrevVP;
  }
  return changed;
}

// Self-checks of DCS's struct (bits: failure reasons). kChkCam alone is not a failure.
enum : uint32_t { kChkAxis = 1, kChkVp = 2, kChkFinite = 4, kChkCam = 8 };
uint32_t SelfCheck(const uint8_t* s) {
  uint32_t bad = 0;
  const uint32_t offs[] = {kOffView, kOffViewInv, kOffProj, kOffViewProj, kOffViewProjInv, kOffPrevXf, kOffPrevVP};
  for (uint32_t o : offs) {
    const M4 m = LoadM(s, o);
    for (int r = 0; r < 4; ++r)
      for (int c = 0; c < 4; ++c)
        if (!std::isfinite(m.a[r][c])) bad |= kChkFinite;
  }
  if (bad) return bad;
  const M4 vi = LoadM(s, kOffViewInv);
  const double ax = std::sqrt(vi.a[0][0] * vi.a[0][0] + vi.a[0][1] * vi.a[0][1] + vi.a[0][2] * vi.a[0][2]);
  if (!(ax >= 0.99 && ax <= 1.01)) bad |= kChkAxis;
  const M4 vp = LoadM(s, kOffViewProj);
  const M4 ref = MulM(LoadM(s, kOffView), LoadM(s, kOffProj));
  double err = 0, scale = 1;
  for (int r = 0; r < 4; ++r)
    for (int c = 0; c < 4; ++c) {
      err = std::max(err, std::fabs(vp.a[r][c] - ref.a[r][c]));
      scale = std::max(scale, std::fabs(vp.a[r][c]));
    }
  if (!(err <= 1e-4 * scale)) bad |= kChkVp;
  float cp[3];
  memcpy(cp, s + kOffCamPos, sizeof(cp));
  const double dx = cp[0] - vi.a[3][0], dy = cp[1] - vi.a[3][1], dz = cp[2] - vi.a[3][2];
  if (!(dx * dx + dy * dy + dz * dz < 1.0)) bad |= kChkCam;
  return bad;
}

// The camera of a struct: position (gViewInv row 3) and axes (rows 0..2).
void Camera(const uint8_t* s, double C[3], double R[3][3]) {
  const M4 vi = LoadM(s, kOffViewInv);
  for (int i = 0; i < 3; ++i) {
    C[i] = vi.a[3][i];
    for (int j = 0; j < 3; ++j) R[i][j] = vi.a[i][j];
  }
}

// DCS's FOV tangents (left, right, up, down) from the struct's gProj; false
// when it is not a perspective projection.
bool StructFov(const uint8_t* s, double fov[4]) {
  const M4 p = LoadM(s, kOffProj);
  float m[16];
  for (int r = 0; r < 4; ++r)
    for (int c = 0; c < 4; ++c) m[r * 4 + c] = static_cast<float>(p.a[r][c]);
  const Proj pr = ParseProj(m);
  if (!pr.ok) return false;
  DcsFov(pr, fov);
  return true;
}

// ---------------------------------------------------------------------------
// Eye axis (pure): unit(C1 - C0) as a constant in view 0's camera frame
// ---------------------------------------------------------------------------

struct Axis {
  static constexpr int kLearn = 120;
  double uh[3] = {};
  int n = 0;
  bool Learned() const { return n >= kLearn; }
  void Add(const double R0[3][3], const double d[3]) {
    const double len = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    if (!(len > 1e-4)) return;
    const double a = 1.0 / std::min(n + 1, kLearn);
    for (int i = 0; i < 3; ++i) {
      const double x = (d[0] * R0[i][0] + d[1] * R0[i][1] + d[2] * R0[i][2]) / len;
      uh[i] = uh[i] * (1 - a) + x * a;
    }
    ++n;
  }
  // The axis in world space for a frame whose view 0 has axes R0.
  bool World(const double R0[3][3], double out[3]) const {
    double l = 0;
    for (int j = 0; j < 3; ++j) {
      out[j] = uh[0] * R0[0][j] + uh[1] * R0[1][j] + uh[2] * R0[2][j];
      l += out[j] * out[j];
    }
    l = std::sqrt(l);
    if (!(l > 1e-6)) return false;
    for (int j = 0; j < 3; ++j) out[j] /= l;
    return true;
  }
  // Degrees between the learned axis and the camera's right axis.
  double AngleToRightDeg() const {
    const double l = std::sqrt(uh[0] * uh[0] + uh[1] * uh[1] + uh[2] * uh[2]);
    return l > 0 ? std::acos(std::clamp(uh[0] / l, -1.0, 1.0)) * 57.29577951308232 : 0.0;
  }
};

// ---------------------------------------------------------------------------
// Pairing the layer's records with the frame DCS renders (pure)
// ---------------------------------------------------------------------------

struct Rec {
  int64_t displayTime = 0;
  uint32_t flags = 0, frame = 0;
  float scale = 1, base = 0, rendered = 0;
};
inline Rec FromBoost(const psh::BoostOut& b) {
  Rec r;
  r.displayTime = b.displayTime;
  r.flags = b.flags;
  r.frame = b.frame;
  r.scale = b.scale;
  r.base = b.baseIpdM;
  r.rendered = b.renderedIpdM;
  return r;
}
// Per-eye shift (metres) the record's views carry; 0 when nothing was applied.
inline double RecH(const Rec& r) {
  if (!(r.flags & psh::boost_flags::enabled) || !(r.flags & psh::boost_flags::applied)) return 0;
  if (!(std::fabs(r.scale - 1.0f) >= 1e-3f)) return 0;
  const double h = (static_cast<double>(r.rendered) - static_cast<double>(r.base)) / 2;
  return std::isfinite(h) ? h : 0.0;
}
// The eye distance DCS renders that record's views with (metres).
inline double RecIpd(const Rec& r) { return RecH(r) != 0 ? r.rendered : r.base; }

struct GazeRec {
  int64_t displayTime = 0;
  float fov[4] = {};
};
struct GazeRing {
  static constexpr int kN = 16;
  GazeRec r[kN];
  int n = 0, head = 0;
  const GazeRec& At(int back) const { return r[(head - 1 - back + 2 * kN) % kN]; }
  void Push(const GazeRec& g) {
    if (n && At(0).displayTime == g.displayTime) return;
    r[head] = g;
    head = (head + 1) % kN;
    if (n < kN) ++n;
  }
};

enum Path : int { kPathNone = 0, kPathSame = 1, kPathFov = 2, kPathLag = 3, kPathTest = 4 };
const char* const kPathNames[5] = {"none", "same", "fov", "lag", "test"};

struct Matcher {
  static constexpr int kRing = 16, kLags = 3, kStaleFrames = 30;
  Rec ring[kRing];
  int count = 0, head = 0;
  uint64_t lastPushDcs = 0;
  uint64_t pushes = 0, latchedPushes = 0;  // records pushed; the count at the previous latch
  int dry = 0;                              // consecutive latches without a new record
  uint64_t drys = 0;                        // diagnostics
  bool latchedAny = false;
  uint32_t latchedFrame = 0;
  // Record / latch phase. The layer writes one record per xrLocateViews, DCS
  // locates frame k + 1 on another thread while frame k renders, so record
  // k + 1 sometimes lands before latch k: that latch sees two new records,
  // the next one none (live 2026-10-13: boost reads == frames, dry 143 =
  // one double + one empty latch each). `ahead`: how many records the newest
  // is ahead of this frame's own one beyond the learned lag (+1 after such an
  // early record, back to 0 at the empty latch that follows; -1 when a record
  // is late). A latch without a new record is served from the ring (the
  // record the phase names) for up to kCarryMax consecutive latches beyond the
  // lag; only then is it dry (h 0: the layer stopped writing).
  static constexpr int kCarryMax = 2;
  int ahead = 0, aheadAge = 0;
  uint64_t lastLatchDcs = ~0ull;
  uint64_t carried = 0, carriedLate = 0, early = 0, resyncs = 0;  // diagnostics
  bool Dry() const { return dry > lag + kCarryMax; }
  int lag = 0;
  int better[kLags] = {};
  bool fault = false;
  int bad = 0, good = 0;
  double bias = 0, kEst = 1;
  int biasN = 0, kN = 0;
  // diagnostics
  uint64_t paths[5] = {}, switches = 0, faults = 0, verifies = 0, ambs = 0;
  double residSum = 0, residMax = 0;
  uint64_t residN = 0;

  const Rec& At(int back) const { return ring[(head - 1 - back + 2 * kRing) % kRing]; }
  // A new record (render thread); false when it is the newest one already.
  bool Push(const Rec& r, uint64_t dcsFrame) {
    if (count && At(0).frame == r.frame && At(0).displayTime == r.displayTime) return false;
    ring[head] = r;
    head = (head + 1) % kRing;
    if (count < kRing) ++count;
    lastPushDcs = dcsFrame;
    ++pushes;
    return true;
  }
  bool Stale(uint64_t dcsFrame) const { return !count || dcsFrame - lastPushDcs >= static_cast<uint64_t>(kStaleFrames); }
  // k: DCS units per metre (1 unless the learned estimate is more than 1 % off).
  double K() const { return kN >= 30 && std::fabs(kEst - 1) > 0.01 ? kEst : 1.0; }
  bool KBad() const { return kN >= 30 && std::fabs(kEst - 1) > 0.05; }

  // A lag-path pick whose plausible records (the learned lag and its
  // neighbours) differ by more than this in h is not used as the frame's h.
  static constexpr double kAmbH = 2e-3;
  // The |C1 - C0| verification picks a record only within this residual and
  // this margin over any record with another eye distance.
  static constexpr double kVerTol = 1.5e-3, kVerMargin = 1e-3;

  struct LatchOut {
    double h = 0;
    int path = kPathNone;
    bool stale = true, any = false;
    bool dry = false;  // stale because no new record came for more latches than the lag and the carry cover
    // No new record at this latch: served from the ring by the record / latch phase (carried), or
    // the frame's own record has not arrived yet and the newest one stands in (late, approximate).
    bool carried = false, late = false;
    int ahead = 0;  // the phase this latch used (Matcher::ahead)
    int pick = 0;   // the record used (back index from the newest)
    // Lag path across a scale step: h is the smallest plausible shift (hLow),
    // not the lag's record; view 1's verification picks the record (VerifiedH).
    bool ambiguous = false;
    double hLow = 0, hSpread = 0, hLo = 0, hHi = 0;  // hLo..hHi: the plausible records' h
    Rec rec;
    Rec lagRec[kLags];
    bool lagOk[kLags] = {};
    // R77: the ambiguity window's records (pick - 1 .. pick + 1), also candidates of VerifiedH: with lag + ahead
    // >= 2 they lie past lagRec.
    Rec ambRec[3];
    int ambN = 0;
  };

  // The FOV fingerprint: the boost record (back index) whose displayTime is
  // the one of the gaze record matching view 0's FOV decisively; -1 if none.
  int FovPick(const GazeRing& g, const double fov[4]) const {
    int best = -1;
    double bestD = 1e9, second = 1e9;
    for (int i = 0; i < g.n; ++i) {
      double d = 0;
      for (int k = 0; k < 4; ++k) d = std::max(d, std::fabs(static_cast<double>(g.At(i).fov[k]) - fov[k]));
      if (d < bestD) {
        second = bestD;
        bestD = d;
        best = i;
      } else if (d < second) {
        second = d;
      }
    }
    if (best < 0 || bestD > 0.004 || second - bestD < 0.002) return -1;
    const int64_t dt = g.At(best).displayTime;
    for (int b = 0; b < count; ++b)
      if (At(b).displayTime == dt) return b;
    return -1;
  }

  // Once per DCS frame, at view 0's cockpit upload.
  LatchOut Latch(uint64_t dcsFrame, const GazeRing* gaze, const double* fov) {
    LatchOut o;
    o.stale = Stale(dcsFrame);
    if (!count) return o;
    for (int L = 0; L < kLags; ++L) {
      o.lagOk[L] = L < count;
      if (o.lagOk[L]) o.lagRec[L] = At(L);
    }
    const uint64_t n = pushes - latchedPushes;
    latchedPushes = pushes;
    // The phase only means something between latches of consecutive frames
    // (external views, menus and loading have no cockpit latch).
    const bool consecutive = latchedAny && lastLatchDcs + 1 == dcsFrame;
    lastLatchDcs = dcsFrame;
    if (o.stale) {
      ahead = aheadAge = 0;
      return o;
    }
    // The layer writes one record per xrLocateViews (also with Depth boost
    // switched off in flight: then with h 0), so each DCS frame brings one.
    // A latch without a new record is served from the ring while the phase
    // explains it (an early record of the previous latch, or the learned lag
    // covers it); beyond kCarryMax more latches the poses are not described
    // (the layer stopped writing): h 0, and no verification on it.
    dry = n ? 0 : dry + 1;
    if (!consecutive) {
      ahead = aheadAge = 0;
    } else {
      ahead += static_cast<int>(std::min<uint64_t>(n, 8)) - 1;
      ahead = std::clamp(ahead, -(kCarryMax + kLags), kLags - 1);
      if (ahead > 0 && n >= 2) ++early;
      // A phase that does not come back within 3 latches is not a one-off
      // early / late record: the lag learning owns systematic offsets.
      aheadAge = ahead ? aheadAge + 1 : 0;
      if (aheadAge > 3) {
        ahead = aheadAge = 0;
        ++resyncs;
      }
    }
    if (Dry()) {
      o.stale = o.dry = true;
      bad = 0;  // a pairing fault needs consecutive verified frames
      ahead = aheadAge = 0;
      ++drys;
      return o;
    }
    o.ahead = ahead;
    // The records as this frame sees them: lagRec[L] is the record L behind
    // this frame's own (the newest shifted by the phase); a record that has
    // not arrived yet is not available (no verification against it).
    for (int L = 0; L < kLags; ++L) {
      const int b = L + ahead;
      o.lagOk[L] = b >= 0 && b < count;
      o.lagRec[L] = o.lagOk[L] ? At(b) : Rec{};
    }
    if (!n) {
      o.carried = true;
      o.late = lag + ahead < 0;
      ++(o.late ? carriedLate : carried);
    }
    o.any = true;
    // Candidates: records newer than the previous latch, plus the newest and
    // every record a learnable lag could pick (0..kLags-1 behind the newest).
    int nc = 0;
    for (int b = 0; b < count; ++b) {
      if (latchedAny && static_cast<int32_t>(At(b).frame - latchedFrame) <= 0) break;
      ++nc;
    }
    nc = std::max(nc, std::min(count, kLags));
    bool same = true;
    const double ipd0 = RecIpd(At(0));
    const bool h0 = RecH(At(0)) != 0;
    for (int b = 1; b < nc && same; ++b)
      if (std::fabs(RecIpd(At(b)) - ipd0) > 0.05e-3 || (RecH(At(b)) != 0) != h0) same = false;
    int pick = 0;
    if (same) {
      o.path = kPathSame;
      pick = std::clamp(lag + ahead, 0, std::min(count, kLags) - 1);  // all candidates agree: the frame's own
    } else {
      const int f = gaze && fov ? FovPick(*gaze, fov) : -1;
      if (f >= 0) {
        pick = f;
        o.path = kPathFov;
      } else {
        pick = std::clamp(lag + ahead, 0, count - 1);  // late (lag + ahead < 0): the newest stands in
        o.path = kPathLag;
        // The lag is a timing estimate, and DCS's frame vs the layer's write
        // jitters by a record: across a scale step (Depth boost's cockpit
        // return drops ~25 mm of eye distance in one record) the lag's record
        // and its neighbour can differ by the whole step, and a wrong pick
        // moves all four views' cockpit by it for that frame (live
        // 2026-10-13: resid max 25.5 mm = one full step at the current lag).
        // Such a frame is not patched with the pick: view 0 (latched here,
        // before any other camera of the frame exists) gets the smallest
        // plausible shift, views 1-3 the record view 1's |C1 - C0| verifies
        // (VerifiedH). Why the smallest shift for view 0 and not the pick or
        // none: with plausible shifts a <= b, applying a errs by 0 or b - a,
        // only ever towards DCS's (boosted) camera, never past the true eye;
        // the pick errs by 0 or b - a in either direction (an over-correction
        // is the visible inward jerk); no patch errs by a or b. With an
        // unboosted candidate (the cockpit return, a = 0) it is no patch: the
        // cockpit at the boosted eye for that frame in view 0 only. The cost:
        // view 0 (left periphery) can differ from view 2 (left focus) by
        // b - a for one frame at the focus inset's edge, against all four
        // views off by b - a with a wrong pick.
        // R77: the window is the pick's neighbours in the ring; it was capped at back index kLags - 1, so with
        // lag + ahead >= 2 the older neighbour (and from 3 on the pick itself) was left out and a step there was
        // not seen as ambiguous.
        double lo = 1e9, hi = -1e9;
        const int b1 = std::min(count - 1, pick + 1);
        for (int b = std::max(0, pick - 1); b <= b1; ++b) {
          const double h = RecH(At(b));
          lo = std::min(lo, h);
          hi = std::max(hi, h);
          if (o.ambN < 3) o.ambRec[o.ambN++] = At(b);
        }
        o.hSpread = hi - lo;
        o.hLo = lo;
        o.hHi = hi;
        if (o.hSpread > kAmbH) {
          o.ambiguous = true;
          o.hLow = lo >= 0 ? lo : hi <= 0 ? hi : 0.0;
          ++ambs;
        }
      }
    }
    latchedAny = true;
    latchedFrame = At(0).frame;
    o.pick = pick;
    o.rec = At(pick);
    o.h = fault ? 0.0 : o.ambiguous ? o.hLow : RecH(o.rec);
    paths[o.path]++;
    return o;
  }

  // The record whose eye distance (plus the learned bias) is the measured separation of the frame's eyes (sep,
  // DCS units, measured before the frame renders), among the newest records a lag and the carry can reach: within
  // tol, and every record with another eye distance (by more than margin) at least margin worse. -1: none.
  int MeasuredPick(double sep, double tol, double margin) const {
    if (!(sep > 0) || !count) return -1;
    const double meas = sep / K();
    const int n = std::min(count, kLags + kCarryMax + 1);
    int best = -1;
    double rb = 1e9;
    for (int b = 0; b < n; ++b) {
      const double r = std::fabs(meas - (RecIpd(At(b)) + bias));
      if (r < rb) rb = r, best = b;
    }
    if (best < 0 || !(rb <= tol)) return -1;
    const double ipdBest = RecIpd(At(best));
    for (int b = 0; b < n; ++b)
      if (std::fabs(RecIpd(At(b)) - ipdBest) > margin && !(std::fabs(meas - (RecIpd(At(b)) + bias)) >= rb + margin))
        return -1;
    return best;
  }

  // An ambiguous latch, at view 1's first upload (before Verify): the h of
  // the record (0..kLags-1 behind the latch's newest) whose eye distance the
  // measured |C1 - C0| (DCS units) matches within kVerTol, kVerMargin better
  // than every record with another eye distance. False when none does.
  bool VerifiedH(double sep, const LatchOut& lo, double* h) const {
    if (lo.stale || !lo.ambiguous || !(sep > 0)) return false;
    const double meas = sep / K();
    // R77: the lag records and the ambiguity window's (which a lag + ahead >= 2 puts past them)
    constexpr int kCand = kLags + 3;
    const Rec* cand[kCand] = {};
    int nc = 0;
    for (int L = 0; L < kLags; ++L)
      if (lo.lagOk[L]) cand[nc++] = &lo.lagRec[L];
    for (int k = 0; k < lo.ambN && k < 3; ++k) cand[nc++] = &lo.ambRec[k];
    double res[kCand];
    int best = -1;
    for (int i = 0; i < nc; ++i) {
      res[i] = std::fabs(meas - (RecIpd(*cand[i]) + bias));
      if (best < 0 || res[i] < res[best]) best = i;
    }
    if (best < 0 || !(res[best] <= kVerTol)) return false;
    const double ipdBest = RecIpd(*cand[best]);
    for (int i = 0; i < nc; ++i)
      if (i != best && std::fabs(RecIpd(*cand[i]) - ipdBest) > 0.5e-3 && !(res[i] >= res[best] + kVerMargin))
        return false;
    *h = RecH(*cand[best]);
    return true;
  }

  // Once per DCS frame at view 1: the measured eye separation (DCS units).
  void Verify(double sep, const LatchOut& lo) {
    if (lo.stale || !lo.lagOk[0] || !(sep > 0)) return;  // a stale or dry latch describes no record
    ++verifies;
    bool allPlain = true;
    for (int L = 0; L < kLags; ++L)
      if (lo.lagOk[L] && (RecH(lo.lagRec[L]) != 0 || !(lo.lagRec[L].base > 0) ||
                          std::fabs(lo.lagRec[L].base - lo.lagRec[0].base) > 1e-6f))
        allPlain = false;
    if (allPlain) {  // nothing applied around this frame: k and the bias
      const double a = 1.0 / std::min(kN + 1, 64);
      kEst = kEst * (1 - a) + sep / lo.lagRec[0].base * a;
      ++kN;
      const double b = 1.0 / std::min(biasN + 1, 64);
      bias = bias * (1 - b) + (sep / K() - lo.lagRec[0].base) * b;
      ++biasN;
    }
    const double meas = sep / K();
    double res[kLags];
    int best = -1;
    double lo_ = 1e9, hi = -1e9;
    for (int L = 0; L < kLags; ++L) {
      res[L] = 1e9;
      if (!lo.lagOk[L]) continue;
      const double ipd = RecIpd(lo.lagRec[L]);
      res[L] = std::fabs(meas - (ipd + bias));
      lo_ = std::min(lo_, ipd);
      hi = std::max(hi, ipd);
      if (best < 0 || res[L] < res[best]) best = L;
    }
    if (best < 0) return;
    const int cur = std::min(lag, kLags - 1);
    const double rc = lo.lagOk[cur] ? res[cur] : res[best];
    residSum += rc * 1e3;
    residMax = std::max(residMax, rc * 1e3);
    ++residN;
    const bool changing = hi - lo_ > 0.3e-3;
    if (changing) {
      if (best != cur && res[best] + 0.3e-3 < rc) {
        for (int L = 0; L < kLags; ++L)
          if (L != best) better[L] = 0;
        if (++better[best] >= 3) {
          lag = best;
          ++switches;
          for (int& x : better) x = 0;
        }
      } else {
        for (int& x : better) x = 0;
      }
    }
    if (!fault) {
      bad = res[best] > 3e-3 ? bad + 1 : 0;
      if (bad >= 10) {
        fault = true;
        good = 0;
        ++faults;
      }
    } else {
      good = rc < 0.5e-3 ? good + 1 : 0;
      if (good >= 60) {
        fault = false;
        bad = 0;
      }
    }
  }
};

// ---------------------------------------------------------------------------
// Shared memory: BoostOut (read-only) and the gaze ring (fixation_depth.h's
// GazeOut mapping, when it is open)
// ---------------------------------------------------------------------------

HANDLE g_boostMap = nullptr;
std::atomic<void*> g_boostView{nullptr};
ULONGLONG g_lastBoostTry = 0;
bool g_boostMissingLogged = false;

std::wstring BoostName() {
  if (!g_boostNameOverride.empty()) return g_boostNameOverride;
  return perc_sfx::Name(psh::boost_block_name);  // R79: the shared suffix rule (perception_suffix.h)
}

// Config thread: once per second while missing.
void OpenBoost(bool force = false) {
  if (g_boostView.load()) return;
  const ULONGLONG now = GetTickCount64();
  if (!force && g_lastBoostTry && now - g_lastBoostTry < 1000) return;
  g_lastBoostTry = now;
  const std::wstring name = BoostName();
  g_boostOpens++;
  HANDLE h = OpenFileMappingW(FILE_MAP_READ, FALSE, name.c_str());
  if (!h) {
    if (!g_boostMissingLogged) Log("cockpit ipd: %ls not found (no Depth boost layer yet); retried once per second", name.c_str());
    g_boostMissingLogged = true;
    return;
  }
  void* v = MapViewOfFile(h, FILE_MAP_READ, 0, 0, sizeof(psh::BoostOut));
  if (!v) {
    CloseHandle(h);
    return;
  }
  g_boostMap = h;
  g_boostView = v;
  Log("cockpit ipd: %ls opened (Depth boost layer)", name.c_str());
}

void CloseBoost() {
  if (void* v = g_boostView.exchange(nullptr)) UnmapViewOfFile(v);
  if (g_boostMap) CloseHandle(g_boostMap);
  g_boostMap = nullptr;
  g_lastBoostTry = 0;
  g_boostMissingLogged = false;
}

// ---------------------------------------------------------------------------
// StereoAck: Local\DcsVrCockpitStereo_v1 (engine to layer; CockpitStereo in
// dcsvr_perception_shm.h)
// ---------------------------------------------------------------------------
// While CockpitTrueIpd is 1 or 2 (started), the block is created or opened
// (CreateFileMapping, the DCSVR_PERCEPTION_SHM_SUFFIX suffix as BoostOut) and
// EndFrame writes it once per rendered frame on the render thread (seqlock):
//   active    CockpitTrueIpd=1, not paused by the in-flight key (g_userOff),
//             not faulted, not inert (pairing fault, k, self-check, axis,
//             gPrevFrameViewProj refuted), and BoostOut read and fresh (not
//             stale, not dry); the scale may be 1 (the cockpit is at the true
//             eye distance whatever the scale). Hysteresis: an active ack
//             stays active while BoostOut is not fresh for up to 150 ms
//             (g_ackHoldMs, under the layer's 200 ms freshness), so a
//             transient gap does not take the layer out of dual-camera mode;
//             the key pause and the health conditions clear it at once;
//   observing CockpitTrueIpd=2;
//   engineFrame our frame counter (g_frame); qpc QueryPerformanceCounter.
// The layer takes the ack as fresh while qpc is less than 200 ms old. Stop
// writes flags 0 once, then unmaps. CockpitTrueIpd=0: never created or written.
constexpr uint32_t kStereoActive = psh::cockpit_stereo_flags::active;
constexpr uint32_t kStereoObserve = psh::cockpit_stereo_flags::observing;
struct StereoAckFields {
  uint32_t flags = 0;
  uint64_t engineFrame = 0;
  int64_t qpc = 0;
};
// The values of one write (pure). active: the conditions above except the
// pause; paused: the in-flight key (clears active).
inline StereoAckFields StereoAckCompose(bool active, bool observe, bool paused, uint64_t engineFrame, int64_t qpc) {
  StereoAckFields v;
  v.flags = (active && !observe && !paused ? kStereoActive : 0u) | (observe ? kStereoObserve : 0u);
  v.engineFrame = engineFrame;
  v.qpc = qpc;
  return v;
}
inline void StereoAckWrite(psh::CockpitStereo* block, const StereoAckFields& v) {
  if (!block) return;
  psh::CockpitStereo b{};
  b.flags = v.flags;
  b.reserved = 0;
  b.engineFrame = v.engineFrame;
  b.qpc = v.qpc;
  psh::seqlock_write(block, b);
}
inline int64_t QpcNow() {
  LARGE_INTEGER q;
  QueryPerformanceCounter(&q);
  return q.QuadPart;
}

std::wstring g_ackNameOverride;                       // tests: their own mapping name
psh::SharedBlock<psh::CockpitStereo> g_ackBlock;      // config thread opens / closes
std::atomic<psh::CockpitStereo*> g_ackView{nullptr};  // render thread writes through it
ULONGLONG g_lastAckTry = 0;
bool g_ackFailLogged = false;
std::atomic<uint64_t> g_ackOpens{0}, g_ackWrites{0};

std::wstring AckName() {
  if (!g_ackNameOverride.empty()) return g_ackNameOverride;
  return perc_sfx::Name(psh::cockpit_stereo_block_name);  // R79: the shared suffix rule (perception_suffix.h)
}

// Config thread (Start, then TickCfg once per second while it failed).
void OpenAck(bool force = false) {
  if (g_ackView.load()) return;
  const ULONGLONG now = GetTickCount64();
  if (!force && g_lastAckTry && now - g_lastAckTry < 1000) return;
  g_lastAckTry = now;
  const std::wstring name = AckName();
  g_ackOpens++;
  if (!g_ackBlock.open(name.c_str(), nullptr)) {
    if (!g_ackFailLogged) Log("cockpit ipd: %ls could not be created (%lu); retried once per second", name.c_str(), GetLastError());
    g_ackFailLogged = true;
    return;
  }
  g_ackView = g_ackBlock.get();
  Log("cockpit ipd: %ls open (stereo ack to the Depth boost layer)", name.c_str());
}

// Stop (the render thread drained): flags 0 once, then unmapped.
void CloseAck(uint64_t engineFrame) {
  if (psh::CockpitStereo* b = g_ackView.exchange(nullptr)) StereoAckWrite(b, StereoAckFields{0, engineFrame, QpcNow()});
  g_ackBlock.close();
  g_lastAckTry = 0;
  g_ackFailLogged = false;
}

// ---------------------------------------------------------------------------
// Statistics
// ---------------------------------------------------------------------------

struct Stats {
  std::atomic<uint64_t> frames{0}, framesPatched{0}, offStale{0}, offScale1{0}, offForeign{0}, offFault{0},
      offNext{0}, offView0{0}, offMulti{0}, offInert{0}, tooBig{0}, gbExecs{0}, ckExecs{0}, ckNotHmd{0},
      tagOther{0}, idFail{0}, gbPatched{0}, fwdPatched{0}, fwdPasses{0}, fwdNoArm{0}, uploadsSeen{0},
      patched{0}, observed{0}, restored{0}, partialBox{0}, subNz{0}, bigSrc{0}, unknownBuf{0}, selfFail{0},
      camFar{0}, failNoUpload{0}, tail{0}, stencilOk{0}, stencilMismatch{0}, stencilWorld{0}, prevOk{0},
      prevBad{0}, boostReads{0}, boostTorn{0}, gazeReads{0}, nested{0}, learned{0}, badBound{0},
      lightSplit{0}, lightSkip{0}, lightArms{0}, lightNoCopy{0}, offDry{0}, fwdFail{0}, fwdFailNoArm{0}, fwdFailArmLost{0}, fwdFailUnpatched{0}, ambiguous{0},
      restFull{0};
  // Forward passes armed inside another feature's in-pass arm; such arms handed over at that feature's end.
  std::atomic<uint64_t> fwdNested{0}, fwdHandover{0};
  // Frame pairing ambiguous across a scale step (Matcher::kAmbH): frames, then views 1-3 on the record
  // |C1 - C0| verified, or not decisive (views 1-3 keep view 0's smallest shift).
  std::atomic<uint64_t> pairAmb{0}, pairAmbVerified{0}, pairAmbUnverified{0};
  // gPrevFrameViewProj (R6): the camera of its eye (d_t common to the views), the store, the gate.
  std::atomic<uint64_t> relRef{0}, relOk{0}, relBad{0}, relNoStore{0}, relNoCentre{0}, storeMis{0}, prevGate{0},
      offPrevRel{0}, xfStd{0}, xfOther{0}, xfAlt{0}, capOnly{0};
  std::atomic<uint64_t> rebase[4] = {};  // reference view's d_t explained by gOrigin: k 0, +1, -1, none
  // Overlay passes (CockpitTrueIpdOverlays): armed, not armed, nested in another feature's arm; cPerView
  // uploads seen / patched / of another camera (forwarded); failed passes; passes without a cPerView upload.
  std::atomic<uint64_t> ovlPasses{0}, ovlNoArm{0}, ovlNested{0}, ovlUploads{0}, ovlPatched{0}, ovlOtherCam{0},
      ovlFail{0}, ovlNoUpload{0};
  // The probe of their other constant-buffer uploads: examined, with an MVP of the view's camera, with its VP itself.
  std::atomic<uint64_t> ovlProbe{0}, ovlProbeMvp{0}, ovlProbeVp{0};
  // Lighting split per view-frame: arm refused (no depth texture), arm not obtained, no draw seen at the pass
  // end (the draw went past the hook), draws seen but none split (b7 not at the true eye), passes with more than
  // one draw; view-frames whose G-buffer moved but whose cockpit was lit from the boosted eye.
  std::atomic<uint64_t> lightNoDepth{0}, lightArmFail{0}, lightNoDraw{0}, lightUnsplit{0}, lightMulti{0},
      lightMiss{0};
  // StereoAck: active -> inactive transitions (the layer logs "gone") by reason: BoostOut not fresh (stale /
  // dry) for longer than the hold, not healthy (fault, inert, gPrev refuted), paused (the in-flight key).
  std::atomic<uint64_t> ackDrops{0}, ackDropBoost{0}, ackDropHealth{0}, ackDropPause{0};
  // StereoAck hysteresis: frames kept active while BoostOut was not fresh (within kAckHoldMs), gaps bridged.
  std::atomic<uint64_t> ackHeld{0}, ackBridged{0};
  // Overlay MVPs (CockpitTrueIpdOverlayMvp): effect-buffer uploads examined / patched (matrices), observed;
  // skipped: no MVP of the view's camera, refused matrices only, no restore entry, too big, sub != 0; overlay
  // passes of a moved view in which no MVP was patched.
  // examined = patched + observed + none + implOnly + restFull + big; mixed: patched uploads that also held a
  // refused matrix; rejMatrices / far: matrices refused / patched with an origin beyond kMvpFarDist.
  std::atomic<uint64_t> mvpSeen{0}, mvpPatched{0}, mvpMatrices{0}, mvpObserved{0}, mvpNone{0}, mvpImplOnly{0},
      mvpMixed{0}, mvpRejMatrices{0}, mvpFar{0}, mvpRestFull{0}, mvpBig{0}, mvpSub{0}, mvpPassNone{0};
  // Overlay passes whose upload entries were found switched by d3d11 (a table toggle not repaired in time: later
  // effect uploads of the pass may have gone past) at the pass end / at a draw (probe), and Flush /
  // ExecuteCommandList calls inside an overlay pass (each repaired at once).
  std::atomic<uint64_t> ovlArmLost{0}, ovlDrawSlotLost{0}, ovlToggles{0};
  // Overlay draw probe (the first kOvlDrawProbe overlay passes per load): draws seen, draws with a bound VS / PS
  // constant buffer that this view's earlier overlay pass restored to DCS's bytes and that was not uploaded again
  // in this pass (it is drawn with the boosted eye's matrices), passes with such a draw.
  std::atomic<uint64_t> ovlProbeDraws{0}, ovlStaleDraws{0}, ovlStalePasses{0}, ovlProbePasses{0};
  // ... draws with a bound effect buffer that their pass did not upload through our hooks; Maps of constant buffers.
  std::atomic<uint64_t> ovlDrawsUnseen{0}, ovlMapCb{0};
  // Overlay passes' upload entries found switched at a re-arm point other than our own upload; toggle points
  // (Flush / Map / GetData) called on DCS's context from another thread.
  std::atomic<uint64_t> ovlMidLoss{0}, tpOtherThread{0};
  // Context-table toggles (fixation_depth.h): Flush / ExecuteCommandList inside one of our arms, tables put back
  // to d3d11's normal entries (by our private upload) at a toggle or at an arm, upload entries armed onto a
  // toggled table before that existed.
  std::atomic<uint64_t> tblToggles{0}, tblNormalized{0}, tblNormArm{0};
  // Frame pairing: latches without a new record served from the ring (carried: the record / latch phase names
  // it; late: the newest one stands in), early records; ambiguous steps while the stereo ack was active.
  std::atomic<uint64_t> carried{0}, carriedLate{0}, ambAckActive{0};
  // Frames off (offNext / offMulti / offView0) whose cockpit views still got the gPrev tail (motion vectors).
  std::atomic<uint64_t> offTail{0};
  // Uploads on DCS's immediate context from a thread other than the render thread (forwarded untouched).
  std::atomic<uint64_t> usOtherThread{0};
  // Lighting: the lighting pass's cPerView forwarded at the true eye (restored at the pass end); arms whose draw
  // was not seen with that b7 bound (cockpit lit from the true eye, world pixels too), or without it.
  std::atomic<uint64_t> lightFwd{0}, lightNoDrawTrue{0};
  // R32 4.5: in-pass arms found "normal, not ours" at a toggle point (a toggled entry called since d3d11's toggle
  // wrote its normal entries over our hooks) and repaired there; per call in fixation_depth.h's g_tpNormalRepair.
  std::atomic<uint64_t> tpNormalRepair{0};
  // Overlay passes observed on the cpu path whose upload entries were lost (fewer observer samples; no image change).
  std::atomic<uint64_t> ovlObsLost{0};
  std::atomic<uint64_t> ovlLearnPasses{0};  // cpu path: overlay passes armed at h 0 only for the learner
};
Stats g_st;
// Render-thread cost of cockpit ipd's hooks (QPC ticks), by part: uploads, overlay-pass draw hooks, the
// G-buffer observer, the pass boundaries (arms / pass ends / restores), the cpu overlay path (ovl_cpu.h: our part
// of the record replay and filter hooks, without DCS's own work inside them, and its frame end). Logged per frame
// in microseconds.
enum CostPart { kCostUpload = 0, kCostDraw, kCostObserver, kCostBoundary, kCostOvlCpu, kCostParts };
std::atomic<uint64_t> g_cost[kCostParts] = {};
struct CostScope {
  int part;
  LARGE_INTEGER t0;
  explicit CostScope(int p) : part(p) { QueryPerformanceCounter(&t0); }
  ~CostScope() {
    LARGE_INTEGER t1;
    QueryPerformanceCounter(&t1);
    g_cost[part].fetch_add(static_cast<uint64_t>(t1.QuadPart - t0.QuadPart), std::memory_order_relaxed);
  }
};
std::mutex g_traceMutex;
std::vector<std::string> g_trace;  // render thread fills (under g_traceMutex), Tick logs
double g_lastH = 0, g_lastScale = 1;
double g_lastBoostH = NAN;  // R77: the last latch's Frame::boostH (tests, diagnostics)
int g_lastPath = 0;

void TraceLine(const char* fmt, ...) {
  char line[400];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(line, sizeof(line), fmt, ap);
  va_end(ap);
  std::lock_guard<std::mutex> lock(g_traceMutex);
  if (g_trace.size() < 6000) g_trace.emplace_back(line);
}

// ---------------------------------------------------------------------------
// Render-thread state
// ---------------------------------------------------------------------------

enum Cls : int { kClsNone = 0, kClsGb = 1, kClsFwd = 2, kClsWatch = 3, kClsLight = 4, kClsOvl = 5, kClsOwn = 6, kClsOwnFwd = 7 };
// Why a view's lighting was not split (EndFrame's lightMiss log).
enum LightWhy : int {
  kLwNone = 0, kLwNoPass = 1, kLwNoDepth = 2, kLwArmFail = 3, kLwNoDraw = 4, kLwUnsplit = 5
};
const char* const kLightWhyNames[6] = {"",
                                       "no GBufferLightingPass armed",
                                       "no depth texture of the cockpit pass",
                                       "the in-pass arm was not obtained",
                                       "the draw went past the hook with DCS's b7 bound",
                                       "draws seen, none split, DCS's b7 bound (skip / no copy / not full-screen)"};

struct Known {
  ID3D11Resource* res = nullptr;  // identity only
  UINT base = 0, size = 0;
  uint64_t lastUse = 0;
};
Known g_known[4];
uint64_t g_useClock = 0;

struct ViewFrame {
  int gbCk = 0;
  bool haveOrig = false, dwReady = false, patched = false, failed = false, done = false, selfFailed = false;
  bool gated = false;  // not patched: the gPrevFrameViewProj check or the store failed
  double C[3] = {}, R[3][3] = {}, dw[3] = {}, dp[3] = {};
  // gPrevFrameViewProj (R6), from CapturePrev at the view's first cockpit upload.
  bool relHave = false, relOk = true;  // checked against the stored previous frame; the verdict
  bool captured = false;               // CapturePrev ran (the first upload was a cockpit G-buffer one)
  bool storeOk = true;                 // dpStore known and equal to the recorded Dw of the previous frame
  double dpStore[3] = {};              // Dp: sent = T(-Dp) * DCS's, previous frame of this view (verified)
  double dpRaw[3] = {};                // the translation as solved from the stored matrices (trace)
  ID3D11Resource* depthTex = nullptr;  // phase 2: the cockpit pass's depth texture (identity)
  double h = 0;                        // the frame's h this view's Dw was computed with (ComputeDw)
  bool lit = false;                    // phase 2: its cockpit pixels were lit with this Dw (split, or the b7 at it)
  int lightWhy = kLwNone;              // phase 2: why not (the last reason seen)
  // Off frame (offNext / offMulti / offView0): only the gPrev tail (Dw 0, Dp from the store) for its cockpit passes.
  bool tailOff = false;
  // Per view-frame counts (the frame dump's CSV): forward / overlay passes patched, effect uploads with an MVP
  // patched, effect uploads left (refused matrices / no restore entry), overlay passes with the arm lost.
  int fwdN = 0, ovlN = 0, mvpN = 0, mvpRejN = 0, ovlLostN = 0;
  // Overlay draws (draw probe), those with a bound effect buffer not uploaded in their pass, Maps of constant
  // buffers in the view's overlay passes.
  int ovlDraws = 0, ovlDrawsUnseen = 0, ovlMaps = 0;
  // The overlay cpu path (ovl_cpu.h) per view-frame, for the frame dump's CSV: the decision of the view's first
  // replay (-1: none), replays, replays moved, records that passed DCS's filter, of them written VP_true / left at
  // DCS's camera, failures of the view (NoteFail), failures that froze one record key.
  int cpuWhy = -1, cpuReplays = 0, cpuMoved = 0, cpuRecs = 0, cpuRecT = 0, cpuRecB = 0, cpuFails = 0, cpuKeyFails = 0;
  // DCS's FOV tangents (left, right, up, down) of the view's cockpit G-buffer camera (the dump's pixel scale).
  bool fovOk = false;
  double fov[4] = {};
  // Helmet models (eye-anchored, drawn with their eye's camera): items switched in this view-frame (G-buffer and
  // forward passes), switches refused (the view then fails: the next frame unpatched).
  int helmetN = 0, helmetRefused = 0;
};
struct Frame {
  bool latched = false, off = false, verified = false, anyPatched = false, anyFailed = false;
  bool observe = false;  // CockpitTrueIpd=2 at the latch: the whole frame observes (a mode change applies next frame)
  bool lighting = false;  // CockpitTrueIpdLighting at the latch (the split and its per-frame check agree)
  double cullSep = 0;  // the frame's eye separation from the culling call (NoteCullSep; 0: no single sample)
  // Off (offNext, offMulti, offView0) with the gPrev tail still applied to views not patched in full (TailOff).
  bool offTail = false;
  double h = 0;
  // R77: Depth boost's per-eye shift DCS renders this frame with (the record the latch / measured pick / view 1's
  // verification decided), before the gates that leave the cockpit unmoved (inert, the limit, the pause): h is
  // what the dual camera applied. NaN: not known (no latch, stale / dry, an off frame, an undecided ambiguity).
  double boostH = NAN;
  int path = kPathNone;
  Matcher::LatchOut lo;
  ViewFrame v[4];
  int ckTotal = 0;
  // The gPrevFrameViewProj check: the reference view's d_t, comparisons made, a failure in this frame.
  bool relRefSet = false, relFail = false;
  int relRefView = -1, relCmp = 0;
  double relRef[3] = {};
};
Frame g_f;
double g_prevDw[4][3] = {};
// Per view, the previous frame's cockpit gViewProj: DCS's and the one we sent
// (DCS's when not patched), DCS's camera (gViewInv row 3) and gOrigin. g_cur
// collects this frame's, committed into g_store at the frame end.
struct PrevStore {
  bool valid = false, patched = false;
  uint64_t tag = 0;  // g_frame + 1 of the frame it describes
  M4 vp{}, sent{};
  double C[3] = {}, o[3] = {};
};
PrevStore g_store[4], g_cur[4];
// gPrevFrameViewProj not the previous camera of its eye in some frame: every
// frame stays unpatched until kRelRecover frames agree again.
constexpr int kRelRecover = 30;
constexpr double kRelTolM = 8e-3;  // d_t agreement (metres); the other eye is ~65 mm away, a centre camera ~33
std::atomic<bool> g_relRefuted{false};  // read by LogStats (config thread)
int g_relGood = 0, g_relLogs = 0, g_gateLogs = 0;
bool g_prevTraced = false;  // CockpitTrueIpdPrevLog: this frame wrote trace lines
bool g_offNext = false;
uint64_t g_frame = 0;
// R77: the per-eye shift limit. Was 40 mm, below the layer's own maximum (Enhanced MaxScale 2 x Ipd up to 90 mm / 2
// = 45 mm): such a frame was left unpatched while the StereoAck stayed active, so the cockpit was drawn at the
// boosted eye distance. Now above that maximum; a frame past it (a record no valid setting makes) also takes the
// ack down for kTooBigHoldFrames, so the layer returns to its own cockpit handling instead of trusting ours.
constexpr double kMaxShiftM = 50e-3;
constexpr uint64_t kTooBigHoldFrames = 90;
int64_t g_tooBigAt = -1;  // g_frame of the last frame past kMaxShiftM (-1: none)
Matcher g_match;
Axis g_axis;
GazeRing g_gaze;
uint64_t g_lastBoostSeq = 0, g_lastGazeSeq = 0;
bool g_inertSelf = false, g_inertAxis = false;
uint64_t g_winAttempts = 0, g_winFails = 0, g_winFrames = 0;
bool g_loggedView[4] = {};
bool g_loggedAxis = false, g_loggedK = false, g_loggedLag = false, g_loggedTag = false;
int g_loggedLagValue = 0;

// The current pass (render thread).
int t_cls = kClsNone;
int t_view = -1;
void* t_pass = nullptr;  // the forward / watched / lighting pass whose end closes the class
uint16_t t_sm = 0;
uint32_t t_tag = 0;
int t_uploads = 0, t_patchedN = 0;
int t_gbDepth = 0;  // gbpass executions open (1 normally)
bool t_gbForeign = false;  // the open execution began inside another class's pass
bool t_fwdArmed = false;   // the forward cockpit pass's upload entries were armed at its begin
bool t_capOnly = false;    // a cockpit G-buffer execution of a frame that is off: its first upload is only recorded
int t_ovlOther = 0;        // the open overlay pass: cPerView uploads of another camera (forwarded, not failures)
int t_mvpPatchedN = 0;     // the open overlay pass: effect-buffer uploads with an MVP patched (or observed)
int t_ovlStaleDraws = 0;   // the open overlay pass (draw probe): draws with a restored, not re-uploaded effect buffer
ID3D11Resource* t_ovlUp[64] = {};  // the open overlay pass: buffers uploaded through our hooks (identity)
int t_ovlUpN = 0;
bool g_dumpActiveFlag = false;   // mirrors g_dumpActive / g_dumpFramesLeft for the draw probe (declared earlier)
int g_dumpFramesLeftFlag = 0;
// Phase 2, the open lighting arm: draws the hooks saw, draws split; cPerView uploads of a known struct, and
// those forwarded at the true eye (restored at the arm's end).
int t_lightDraws = 0, t_lightSplits = 0, t_lightUploads = 0, t_lightFwdN = 0;
// Phase 2: a lighting pass with more than one draw seen (counted); the split draw's vertex count and topology.
bool g_lightMultiSeen = false;
UINT g_lightCount = 0;
D3D11_PRIMITIVE_TOPOLOGY g_lightTopo = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
int g_lightMissLogs = 0;
// Phase 2, the open lighting arm: our own OM / draw calls in progress.
bool t_lightBusy = false;
// Overlay class: its own failure window (> 1 % over 300 frames: overlays off for this load).
uint64_t g_ovlAttempts = 0, g_ovlFails = 0;
std::atomic<bool> g_ovlInert{false};
int g_ovlProbeLeft = 0;  // probe budget (uploads), set at Start
constexpr int kOvlProbeBudget = 20000;
// Overlay draw probe: for the first kOvlDrawProbe overlay passes per load the
// draw entries are armed too (OvlDrawProbe). It answers whether DCS draws in
// an overlay pass with an effect buffer that an earlier overlay pass of the
// same view restored to DCS's (boosted) bytes at its end and that this pass did
// not upload again (DCS's effects upload a constant buffer only when its CPU
// copy changed): such draws would show the boosted eye's symbology.
constexpr int kOvlDrawProbe = 3000;
int g_ovlDrawProbeLeft = 0;
bool t_ovlProbe = false;  // the open overlay pass has the draw entries armed for the probe
bool t_ovlLdr = false;    // the open overlay pass is OverlayLDRPass / GUIPass (the post-tonemap target)
struct OvlRestored {
  ID3D11Resource* res = nullptr;  // identity
  bool uploaded = false;          // uploaded again in the open overlay pass
};
constexpr int kOvlRestMax = 32;
OvlRestored g_ovlRest[kOvlRestMax];
int g_ovlRestN = 0, g_ovlRestView = -1;
uint64_t g_ovlRestFrame = ~0ull;
// StereoAck: the previous write's active flag.
bool g_ackWasActive = false;
int g_ackDropLogs = 0, g_ambAckLogs = 0;
// R63: evidence that the engine can move the cockpit at all; the ack is active only with it (the layer drops its own
// cockpit steps on an active ack). A D3D11 device learnt (fixation_depth.h's context: in PerceptionOnly
// perception_device.h's, which gives up on a wrapped context), and cockpit passes being intercepted: kAckMissFrames
// frames that drew a cockpit pass, or identified none of their G-buffer executions, without view 0's cockpit upload
// latched clear it until a frame latches one again. Not patches: at scale 1 nothing is patched, and the layer only
// raises the scale on an active ack.
constexpr int kAckMissFrames = 45;
int g_ackMissRun = 0;
uint64_t g_ackIdExecs = 0, g_ackIdFails = 0;
bool g_ackNoEvidence = false;  // the previous frame end lacked it (one log line per change)
int g_ackEvidenceLogs = 0;
bool g_ackAssumeDevice = false;  // tests: device evidence without fixation_depth.h's D3D11 context
void ResetMvpState();  // below (the overlay MVP path's per-view cache and dump budget)
void DumpEndFrame();   // below ([Perception] CockpitTrueIpdFrameDump)
void DumpCapture(ID3D11Resource* res, const D3D11_VIEWPORT& vp, int view, int src, int binds);  // below
// StereoAck hysteresis: an active ack stays active while BoostOut is not fresh for at most this long (the
// layer's freshness is 200 ms); the in-flight key and health clear it at once. Tests may change it.
std::atomic<int> g_ackHoldMs{150};
struct AckHold {
  bool bad = false;     // BoostOut not fresh since `since`
  int64_t since = 0;
};
AckHold g_ackHold;
// Pure: whether BoostOut counts as fresh for this ack write. boostOk: fresh now; wasActive: the previous
// write was active. A gap is bridged only for an active ack and only while it lasts <= holdTicks.
inline bool AckBoostUsable(AckHold& h, bool boostOk, bool wasActive, int64_t now, int64_t holdTicks, bool* held,
                           bool* bridged) {
  *held = *bridged = false;
  if (boostOk) {
    *bridged = h.bad && wasActive;
    h.bad = false;
    return true;
  }
  if (!wasActive) {  // nothing to hold: inactive until BoostOut is fresh again
    h.bad = false;
    return false;
  }
  if (!h.bad) {
    h.bad = true;
    h.since = now;
  }
  *held = now - h.since <= holdTicks;
  return *held;
}

// DCS's latest bytes of a byte range [lo, lo + len) of a buffer that took a
// patched upload in the current pass, uploaded again at the pass end.
struct Restore {
  ID3D11Resource* res = nullptr;
  UINT sub = 0;
  bool hasBox = false, us1 = false;
  D3D11_BOX box{};
  UINT lo = 0, len = 0;
  uint8_t bytes[kMaxSrc];
};
// cPerView needs one or two; the overlay passes' effect buffers (font, lines, ...) one each.
constexpr int kRestMax = 16;
Restore g_rest[kRestMax];
int g_restN = 0;
alignas(16) uint8_t g_scratch[kMaxSrc];
// Head-attached models (helmet section below): the cPerView upload the open cockpit pass (G-buffer or forward)
// had patched: DCS's bytes and the bytes we sent, its buffer and box, for the camera switch of the models drawn at
// an eye's camera (the switch uploads into the same buffer, the same way).
struct HelmetSrc {
  bool valid = false;
  ID3D11Resource* res = nullptr;  // identity: the buffer that took the patched upload
  UINT at = 0, len = 0;           // the struct's offset in the upload, the upload's size
  bool hasBox = false, us1 = false;
  D3D11_BOX box{};
  uint64_t frame = ~0ull;
  int view = -1;
  uint8_t dcs[kMaxSrc] = {};   // DCS's upload
  uint8_t sent[kMaxSrc] = {};  // ours (the true eye)
};
HelmetSrc t_helmetSrc;
// The ownship: each HMD view's cockpit G-buffer struct of this frame (DCS's bytes and where they went), kept at the
// cockpit pass's end. A world pass that uploads no cPerView of its own draws with that buffer's content (DCS's
// bytes, put back by the cockpit pass's restore); the ownship switch then starts from it.
HelmetSrc g_ownViewSrc[4];
void* g_helmetVtbl[8];  // the helmet section's switch entries' vtable
// The d3d11 upload entry itself (fixation_depth.h, past every hook of ours: the switch's bytes must not be taken
// for DCS's by the restore entries or by far depth's matrices).
void RawUpload(ID3D11Resource* r, const D3D11_BOX* box, const void* src, bool us1);
void NoteDcsEye(int t, const double C[3]);  // (the helmet section)
double g_dcsOrigin[3] = {};  // view 0's gOrigin at its cockpit upload (the world trace)
// (the ownship section) An upload of a known struct in an ownship pass.
const void* OwnUpload(ID3D11Resource* r, const D3D11_BOX* box, const uint8_t* base, UINT at, UINT len, bool us1);

// Phase 2: DCS's last bytes of each known struct (buffer and offset) uploaded in the lighting pass, and whether
// that upload went to the GPU at the true eye (fwdPatched: the bound b7 holds the true eye's camera).
struct LightCopy {
  ID3D11Resource* res = nullptr;
  UINT base = 0, len = 0;
  bool valid = false, fwdPatched = false;
  uint8_t bytes[kMaxSrc];
};
LightCopy g_lightCopy[2];

Known* FindKnown(ID3D11Resource* r, UINT base) {
  for (Known& k : g_known)
    if (k.res == r && r && k.base == base) return &k;
  return nullptr;
}
bool IsKnown(ID3D11Resource* r) {
  for (const Known& k : g_known)
    if (k.res == r && r) return true;
  return false;
}
UINT KnownSize(ID3D11Resource* r) {
  for (const Known& k : g_known)
    if (k.res == r && r) return k.size;
  return 0;
}

// The struct offset b7 binds `r` at right now (VS, else PS); false when b7
// does not bind `r`.
bool BoundBase(ID3D11Resource* r, UINT* base) {
  State& s = g_s;
  if (!s.ctx || !s.ctx1) return false;
  for (int stage = 0; stage < 2; ++stage) {
    ID3D11Buffer* cb = nullptr;
    UINT first = 0, num = 0;
    if (stage == 0)
      s.ctx1->VSGetConstantBuffers1(7, 1, &cb, &first, &num);
    else
      s.ctx1->PSGetConstantBuffers1(7, 1, &cb, &first, &num);
    if (!cb) continue;
    const bool same = static_cast<ID3D11Resource*>(cb) == r;
    cb->Release();
    if (!same) continue;
    *base = first == 0 && (num == 4096 || num == 0) ? 0 : first * 16;
    return true;
  }
  return false;
}

// The known struct an upload of [lo, hi) of `r` carries: the one learned
// struct of `r` the range covers, or, when several do (per-view structs at
// offsets of one buffer), the one b7 binds now; nullptr when that does not
// decide (forwarded as DCS's, counted).
Known* PickKnown(ID3D11Resource* r, UINT lo, UINT hi) {
  Known* k = nullptr;
  int n = 0;
  for (Known& c : g_known)
    if (c.res == r && r && c.base >= lo && c.base + kCbBytes <= hi) {
      k = &c;
      ++n;
    }
  if (n <= 1) return k;
  UINT b = 0;
  if (!BoundBase(r, &b)) return nullptr;
  for (Known& c : g_known)
    if (c.res == r && c.base == b && c.base >= lo && c.base + kCbBytes <= hi) return &c;
  return nullptr;
}

// Keeps the pass-end restore exact: DCS's bytes of every upload of a buffer
// that has a restore entry are merged into it, so the restore re-uploads
// DCS's latest content, never older bytes over newer ones. `bytes` are the
// upload's source, the content of [lo, hi). patched: this upload is forwarded
// patched and needs its own range restored (false: no free entry; the caller
// then forwards DCS's bytes). An entry the upload covers entirely is dropped
// (the buffer holds DCS's bytes there now) or replaced (patched).
bool NoteRestore(ID3D11Resource* r, UINT lo, UINT hi, const D3D11_BOX* box, const uint8_t* bytes, bool us1,
                 bool patched) {
  bool covered = false;
  for (int i = 0; i < g_restN;) {
    Restore& e = g_rest[i];
    if (e.res != r) {
      ++i;
      continue;
    }
    const UINT eHi = e.lo + e.len;
    if (lo <= e.lo && hi >= eHi) {  // covers the entry: drop it
      g_rest[i] = g_rest[g_restN - 1];
      --g_restN;
      continue;
    }
    const UINT a = std::max(lo, e.lo), b = std::min(hi, eHi);
    if (a < b) memcpy(e.bytes + (a - e.lo), bytes + (a - lo), b - a);
    if (e.lo <= lo && eHi >= hi) covered = true;  // the entry already restores this range (merged above)
    ++i;
  }
  if (!patched || covered) return true;
  if (g_restN >= kRestMax || hi - lo > kMaxSrc) return false;
  Restore& e = g_rest[g_restN++];
  e.res = r;
  e.sub = 0;
  e.hasBox = box != nullptr;
  if (box) e.box = *box;
  e.us1 = us1;
  e.lo = lo;
  e.len = hi - lo;
  memcpy(e.bytes, bytes, e.len);
  return true;
}

// An upload forwarded with DCS's own bytes: merged into the restore entries.
void NoteForwarded(ID3D11Resource* r, UINT sub, const D3D11_BOX* box, const void* src) {
  if (!g_restN || !src || sub != 0) return;
  bool any = false;
  for (int i = 0; i < g_restN; ++i) any = any || g_rest[i].res == r;
  if (!any) return;
  UINT lo = 0, hi = 0;
  if (box) {
    lo = box->left;
    hi = box->right;
  } else {
    hi = KnownSize(r);
    if (!hi) {  // no longer known (evicted): its size is unknown; its entries go (never older bytes over newer)
      for (int i = 0; i < g_restN;)
        if (g_rest[i].res == r)
          g_rest[i] = g_rest[--g_restN];
        else
          ++i;
      return;
    }
  }
  if (hi > lo) NoteRestore(r, lo, hi, box, static_cast<const uint8_t*>(src), false, false);
}

// Learns the cPerView bound at VS b7 (else PS b7) with its *SetConstantBuffers1 offset.
void LearnBound() {
  State& s = g_s;
  if (!s.ctx) return;
  ID3D11Buffer* cb = nullptr;
  UINT first = 0, num = 0;
  if (s.ctx1) {
    s.ctx1->VSGetConstantBuffers1(7, 1, &cb, &first, &num);
    if (!cb) s.ctx1->PSGetConstantBuffers1(7, 1, &cb, &first, &num);
  } else {
    s.ctx->VSGetConstantBuffers(7, 1, &cb);
    if (!cb) s.ctx->PSGetConstantBuffers(7, 1, &cb);
    num = 4096;
  }
  if (!cb) return;
  D3D11_BUFFER_DESC d{};
  cb->GetDesc(&d);
  ID3D11Resource* res = cb;
  cb->Release();  // identity only: DCS keeps it bound
  const bool offsets = !(first == 0 && (num == 4096 || num == 0));
  const UINT base = offsets ? first * 16 : 0;
  if (!(d.BindFlags & D3D11_BIND_CONSTANT_BUFFER) || base + kCbBytes > d.ByteWidth ||
      (offsets && num * 16 < kCbBytes)) {
    g_st.badBound++;
    return;
  }
  ++g_useClock;
  for (Known& k : g_known)
    if (k.res == res && k.base == base) {
      k.lastUse = g_useClock;
      k.size = d.ByteWidth;
      return;
    }
  Known* slot = &g_known[0];
  for (Known& k : g_known)
    if (!k.res || k.lastUse < slot->lastUse) slot = &k;
  *slot = Known{res, base, d.ByteWidth, g_useClock};
  if (g_st.learned++ < 4)
    Log("cockpit ipd: cPerView buffer learned (%u bytes, struct at byte %u)", d.ByteWidth, base);
}

void ResetOwnship();  // (the ownship section)
void ResetRender() {
  ResetOwnship();
  g_f = Frame{};
  memset(g_prevDw, 0, sizeof(g_prevDw));
  g_offNext = false;
  g_frame = 0;
  g_tooBigAt = -1;  // R77
  g_match = Matcher{};
  g_axis = Axis{};
  g_gaze = GazeRing{};
  g_lastBoostSeq = g_lastGazeSeq = 0;
  g_inertSelf = g_inertAxis = false;
  g_winAttempts = g_winFails = g_winFrames = 0;
  for (Known& k : g_known) k = Known{};
  g_useClock = 0;
  for (int t = 0; t < 4; ++t) g_store[t] = PrevStore{}, g_cur[t] = PrevStore{};
  g_relRefuted = false;
  g_relGood = g_relLogs = g_gateLogs = 0;
  g_prevTraced = false;
  t_capOnly = false;
  for (bool& b : g_loggedView) b = false;
  g_loggedAxis = g_loggedK = g_loggedLag = g_loggedTag = false;
  g_loggedLagValue = 0;
  t_cls = kClsNone;
  t_view = -1;
  t_pass = nullptr;
  t_gbDepth = 0;
  t_gbForeign = false;
  t_fwdArmed = false;
  g_restN = 0;
  for (LightCopy& l : g_lightCopy) l.valid = false, l.res = nullptr, l.base = 0;
  t_lightDraws = t_lightSplits = 0;
  g_lightMultiSeen = false;
  g_lightCount = 0;
  g_lightTopo = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
  g_lightMissLogs = 0;
  t_lightBusy = false;
  t_lightUploads = t_lightFwdN = 0;
  g_ovlAttempts = g_ovlFails = 0;
  g_ovlInert = false;
  g_ovlProbeLeft = kOvlProbeBudget;
  g_ovlDrawProbeLeft = kOvlDrawProbe;
  t_ovlProbe = false;
  g_ovlRestN = 0;
  g_ovlRestView = -1;
  g_ovlRestFrame = ~0ull;
  g_ackWasActive = false;
  g_ackDropLogs = g_ambAckLogs = 0;
  g_ackHold = AckHold{};
  g_ackMissRun = 0;
  g_ackNoEvidence = false;
  ResetMvpState();
}

// Each top-level pass boundary: new BoostOut and GazeOut records.
void Poll() {
  if (void* v = g_boostView.load(std::memory_order_relaxed)) {
    const uint64_t seq = *static_cast<const volatile uint64_t*>(v);
    if (seq != g_lastBoostSeq) {
      psh::BoostOut b{};
      if (psh::seqlock_read(static_cast<const psh::BoostOut*>(v), b, 3)) {
        g_lastBoostSeq = b.seq;
        g_st.boostReads++;
        g_match.Push(FromBoost(b), g_frame);
      } else if (seq != 0) {
        g_st.boostTorn++;
      }
    }
  }
  if (void* v = g_gazeView.load(std::memory_order_relaxed)) {
    const uint64_t seq = *static_cast<const volatile uint64_t*>(v);
    if (seq != g_lastGazeSeq && !(seq & 1)) {
      GazeOut g{};
      if (SeqRead(v, g, 3)) {
        g_lastGazeSeq = g.seq;
        GazeRec r;
        r.displayTime = g.displayTime;
        for (int k = 0; k < 4; ++k) r.fov[k] = g.fovTan[0][k];
        g_gaze.Push(r);
        g_st.gazeReads++;
      }
    }
  }
}

bool InertNow(const char** why) {
  if (g_match.fault) return *why = "pairFault", true;
  if (g_match.KBad()) return *why = "k out of range", true;
  if (g_inertSelf) return *why = "self-check rate", true;
  if (g_inertAxis) return *why = "eye axis sign", true;
  return false;
}

// The frame's eye separation measured before it renders: main.cpp's culling hook (HookCollect) passes the distance
// between the two peripheral views' frustum apexes (DCS units, the eyes DCS renders this frame with, pupil shift
// included) once per collect call with exactly two peripheral / focus pairs. The latch uses a sample only when
// exactly one arrived since the previous latch (that frame's own; DCS culls a frame's views in one call before
// its passes).
std::atomic<uint64_t> g_cullSeq{0};
std::atomic<double> g_cullSep{0.0};
uint64_t g_cullSeqLatched = 0;
void NoteCullSep(double sep) {
  if (!(sep > 0) || !std::isfinite(sep)) return;
  g_cullSep.store(sep, std::memory_order_relaxed);
  g_cullSeq.fetch_add(1, std::memory_order_release);
}
// A latch without a new BoostOut record (carried) keeps the last record's h. The layer writes nothing at all once
// Depth boost is switched off in its ini ([DepthBoost] Enabled=0: the scale is 1 from that display time on, no
// final enabled = 0 record), so the carried h would push the cockpit past the true eye by h for up to lag +
// kCarryMax frames while DCS already renders the true eyes. Pure: true when the measured separation (sepM / k,
// less the learned bias) lies nearer the record's base IPD than its rendered IPD, which differ by 1 mm or more.
inline bool CarriedUnboosted(double sepM, double k, double bias, const Rec& rec) {
  if (!(sepM > 0) || !(k > 0) || RecH(rec) == 0) return false;
  const double meas = sepM / k - bias;
  const double base = rec.base, rendered = RecIpd(rec);
  if (!(std::fabs(rendered - base) >= 1e-3)) return false;
  return std::fabs(meas - base) < std::fabs(meas - rendered);
}
std::atomic<uint64_t> g_carriedUnboosted{0}, g_carriedNoSep{0};
std::atomic<uint64_t> g_cullAgree{0}, g_cullDisagree{0};
std::atomic<uint64_t> g_ambByCull{0}, g_ambCullUndecided{0};  // ambiguous latches decided by the measured separation
// The measured separation's precision (against view 1's |C1 - C0| of the same frame, DCS units): an EMA and the
// largest deviation; the per-frame pick below runs only once it is established (kCullPreciseN samples, EMA under
// kCullPreciseM). Picks made, and those that changed the latch's h.
constexpr int kCullPreciseN = 60;
constexpr double kCullPreciseM = 0.01e-3, kPickTol = 0.05e-3, kPickMargin = 0.02e-3;
double g_cullDevEma = 0, g_cullDevMax = 0;
uint64_t g_cullDevN = 0;
std::atomic<uint64_t> g_pickMeas{0}, g_pickChanged{0}, g_pickNone{0};
inline bool CullPrecise() { return g_cullDevN >= static_cast<uint64_t>(kCullPreciseN) && g_cullDevEma < kCullPreciseM; }  // at view 1: the sample vs |C1 - C0| within 0.3 mm
int g_carriedUnboostedLogs = 0;

// The frame's one latch (view 0's first cockpit upload, DCS's original bytes).
void LatchFrame(const uint8_t* s0) {
  Frame& f = g_f;
  f.latched = true;
  const uint64_t cullSeq = g_cullSeq.load(std::memory_order_acquire);  // (consumed by every latch)
  const bool oneSample = cullSeq == g_cullSeqLatched + 1;
  g_cullSeqLatched = cullSeq;
  f.cullSep = oneSample ? g_cullSep.load(std::memory_order_relaxed) : 0.0;
  f.observe = Observe();
  f.lighting = g_cfgLighting.load(std::memory_order_relaxed);
  if (ForeignTableHooks()) {
    f.off = true;
    g_st.offForeign++;
    g_offNext = false;
    return;
  }
  if (g_offNext) {
    f.off = true;
    f.offTail = true;
    g_st.offNext++;
    g_offNext = false;
    return;
  }
  if (g_relRefuted) {  // gPrevFrameViewProj not established: today's image until kRelRecover frames agree
    f.off = true;
    g_st.offPrevRel++;
    return;
  }
  double fov[4];
  const bool fovOk = StructFov(s0, fov);
  f.lo = g_match.Latch(g_frame, g_gaze.n ? &g_gaze : nullptr, fovOk ? fov : nullptr);
  f.path = f.lo.path;
  double h = f.lo.stale ? 0.0 : f.lo.h;  // ambiguous: the smallest plausible shift (Matcher::Latch)
  {
    if (!f.lo.stale && f.lo.carried && h != 0) {
      if (!oneSample) {
        g_carriedNoSep++;
      } else if (CarriedUnboosted(g_cullSep.load(std::memory_order_relaxed), g_match.K(), g_match.bias, f.lo.rec)) {
        h = 0;  // DCS renders this frame at the true eyes: nothing to move back
        g_carriedUnboosted++;
        if (g_carriedUnboostedLogs < 10) {
          ++g_carriedUnboostedLogs;
          Log("cockpit ipd: frame %llu: no new BoostOut record and the frame's eyes (measured %.2f mm apart) are "
              "unboosted (record: base %.2f, rendered %.2f mm): h 0",
              static_cast<unsigned long long>(g_frame), g_cullSep.load() / g_match.K() * 1e3, f.lo.rec.base * 1e3,
              RecIpd(f.lo.rec) * 1e3);
        }
      }
    }
  }
  // An ambiguous latch (a scale step between the plausible records: Depth boost switched off or on, a stepped
  // return) gave view 0 the smallest plausible shift, which is wrong for view 0 whenever DCS still renders the
  // larger one (live 2026-10-14, AH-64D, Depth boost off: view 0's cockpit at the boosted eye for that frame, views
  // 1-3 right). The frame's own eye separation, measured before it renders, picks the record the way view 1's
  // |C1 - C0| does (VerifiedH), so every view gets it.
  bool ambDecided = false;  // R77: the ambiguity decided by the frame's measured separation
  if (!f.lo.stale && f.lo.ambiguous && f.cullSep > 0 && !g_match.fault) {
    double hv = 0;
    if (g_match.VerifiedH(f.cullSep, f.lo, &hv)) {
      h = hv;
      ambDecided = true;
      g_ambByCull++;
    } else {
      g_ambCullUndecided++;
    }
  }
  // Every latch whose candidate records differ (a ramp, a step): the record the frame's measured separation names
  // (live 2026-10-14, F/A-18, a 5 mm/s dual-camera ramp: the lag path took the record one ahead of the one DCS
  // rendered in alternate frames, the cockpit 0.1 mm off the true eye every other frame: edge shimmer).
  if (!f.lo.stale && f.lo.path != kPathSame && f.cullSep > 0 && !g_match.fault && CullPrecise()) {
    const int b = g_match.MeasuredPick(f.cullSep, kPickTol, kPickMargin);
    if (b >= 0) {
      const double hm = RecH(g_match.At(b));
      if (std::fabs(hm - h) > 5e-6) g_pickChanged++;
      h = hm;
      f.lo.rec = g_match.At(b);
      f.lo.ambiguous = false;  // (views 1-3 keep this h: the frame's own separation decided it)
      g_pickMeas++;
    } else {
      g_pickNone++;
    }
  }
  // R77: the boost DCS renders with, before the gates below (an ambiguity not decided here: views 1-3's
  // verification may decide it, else unknown)
  f.boostH = f.lo.stale || (f.lo.ambiguous && !ambDecided) ? NAN : h;
  g_lastBoostH = f.boostH;
  const bool real = !f.lo.stale && (RecH(f.lo.rec) != 0 || f.lo.ambiguous);
  if (f.lo.stale) (f.lo.dry ? g_st.offDry : g_st.offStale)++;
  else if (!real) g_st.offScale1++;
  if (!f.lo.stale && f.lo.ambiguous) {
    g_st.pairAmb++;
    // The stereo ack promises the layer ramps the scale (no steps): a step while it was active is a contract
    // violation of the layer's, counted and logged (the frame is still handled as an ambiguous step).
    if (g_ackWasActive) {
      g_st.ambAckActive++;
      if (g_ambAckLogs < 5) {
        ++g_ambAckLogs;
        Log("cockpit ipd: frame %llu: a scale step of %.1f mm per eye (h %.2f..%.2f mm) while the stereo ack was "
            "active (the layer should ramp)",
            static_cast<unsigned long long>(g_frame), f.lo.hSpread * 1e3, f.lo.hLo * 1e3, f.lo.hHi * 1e3);
      }
    }
  }
  if (f.lo.carried) (f.lo.late ? g_st.carriedLate : g_st.carried)++;
  const char* why = nullptr;
  if (h != 0 && InertNow(&why)) {
    h = 0;
    g_st.offInert++;
  } else if (real && g_match.fault) {
    g_st.offFault++;
  }
  // An ambiguous frame with a plausible record beyond the limit: view 0 not
  // patched (views 1-3 follow their verified record, with the same limit).
  const double hBig = f.lo.ambiguous ? std::max(std::fabs(f.lo.hLo), std::fabs(f.lo.hHi)) : std::fabs(h);
  if (std::fabs(h) > kMaxShiftM || hBig > kMaxShiftM) {
    if (g_st.tooBig++ == 0)
      Log("cockpit ipd: per-eye shift %.1f mm > %.0f mm ignored", std::max(std::fabs(h), hBig) * 1e3, kMaxShiftM * 1e3);
    g_tooBigAt = static_cast<int64_t>(g_frame);
    h = 0;
  }
  if (std::fabs(h) < 0.05e-3) h = 0;
  if (g_userOff.load(std::memory_order_relaxed)) h = 0;
  const double shift = g_cfgShiftMm.load(std::memory_order_relaxed);
  if (!real && shift != 0) {
    h = shift * 1e-3;
    f.path = kPathTest;
  }
  f.h = h;
  g_lastH = h;
  g_lastPath = f.path;
  g_lastScale = real ? f.lo.rec.scale : 1.0;
}

// View t's Dw / Dp for this frame (at its first cockpit upload).
void ComputeDw(int t) {
  ViewFrame& vf = g_f.v[t];
  if (vf.dwReady) return;
  vf.dwReady = true;
  if (!vf.captured) {  // no store entry read: consistent only when no shift was recorded either
    vf.dpStore[0] = vf.dpStore[1] = vf.dpStore[2] = 0;
    vf.storeOk = !NonZero(g_prevDw[t]);
  }
  // Dp: the translation between DCS's previous gViewProj of this view and the one we sent (the store).
  for (int i = 0; i < 3; ++i) vf.dp[i] = vf.dpStore[i], vf.dw[i] = 0;
  if (g_f.off) {
    vf.dp[0] = vf.dp[1] = vf.dp[2] = 0;
    return;
  }
  if (g_f.h == 0) return;
  vf.h = g_f.h;  // every class of this view uses this Dw (an ambiguous frame's later f.h change is not seen here)
  double u[3] = {vf.R[0][0], vf.R[0][1], vf.R[0][2]};
  if (!(g_axis.Learned() && g_f.v[0].haveOrig && g_axis.World(g_f.v[0].R, u))) {
    const double l = std::sqrt(u[0] * u[0] + u[1] * u[1] + u[2] * u[2]);
    if (!(l > 0.5)) return;
    for (double& x : u) x /= l;
  }
  const double sg = (t & 1) ? -1.0 : 1.0;
  const double m = sg * g_f.h * g_match.K();
  for (int i = 0; i < 3; ++i) vf.dw[i] = m * u[i];
}

// View 1's first original upload: the measured eye separation.
void VerifyFrame() {
  Frame& f = g_f;
  f.verified = true;
  const ViewFrame& a = f.v[0];
  const ViewFrame& b = f.v[1];
  const double d[3] = {b.C[0] - a.C[0], b.C[1] - a.C[1], b.C[2] - a.C[2]};
  const double sep = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
  g_axis.Add(a.R, d);
  double u[3] = {a.R[0][0], a.R[0][1], a.R[0][2]};
  if (g_axis.Learned()) {
    g_axis.World(a.R, u);
    if (!g_loggedAxis) {
      g_loggedAxis = true;
      Log("cockpit ipd: eye axis learned, %.2f deg from the camera's right axis", g_axis.AngleToRightDeg());
    }
  }
  if (sep > 1e-4 && d[0] * u[0] + d[1] * u[1] + d[2] * u[2] <= 0 && !g_inertAxis) {
    g_inertAxis = true;
    Log("cockpit ipd: the right eye is not on the +axis side of the left one; inert");
  }
  // The culling call's separation against this frame's |C1 - C0| (the same eyes when it is this frame's sample).
  if (f.cullSep > 0 && sep > 0) {
    const double dev = std::fabs(f.cullSep - sep);
    (dev <= 0.3e-3 ? g_cullAgree : g_cullDisagree)++;
    const double a = 1.0 / static_cast<double>(std::min<uint64_t>(g_cullDevN + 1, 64));
    g_cullDevEma = g_cullDevEma * (1 - a) + std::min(dev, 1e-3) * a;
    g_cullDevMax = std::max(g_cullDevMax, dev);
    ++g_cullDevN;
  }
  if (!f.off && !f.lo.stale && f.lo.lagOk[0]) {
    // An ambiguous latch (a scale step between plausible records): views 1-3
    // take the record this frame's |C1 - C0| verifies, with the latch's gates;
    // not decisive: they keep view 0's smallest plausible shift.
    if (f.lo.ambiguous) {
      double hv = 0;
      const char* why = nullptr;
      if (!g_match.fault && g_match.VerifiedH(sep, f.lo, &hv)) {
        f.boostH = hv;  // R77: the verified record, before the gates
        g_lastBoostH = hv;
        if (hv != 0 && InertNow(&why)) hv = 0;
        if (std::fabs(hv) > kMaxShiftM) g_tooBigAt = static_cast<int64_t>(g_frame);  // R77
        if (std::fabs(hv) > kMaxShiftM || std::fabs(hv) < 0.05e-3 || g_userOff.load(std::memory_order_relaxed)) hv = 0;
        f.h = hv;
        g_lastH = hv;
        g_st.pairAmbVerified++;
      } else {
        g_st.pairAmbUnverified++;
      }
    }
    const int lagBefore = g_match.lag;
    const bool faultBefore = g_match.fault;
    g_match.Verify(sep, f.lo);
    if (g_match.lag != lagBefore) Log("cockpit ipd: frame pairing lag %d -> %d records", lagBefore, g_match.lag);
    if (g_match.fault != faultBefore)
      Log(g_match.fault ? "cockpit ipd: pairing fault (eye distance off by > 3 mm at every lag for 10 frames; DCS "
                          "overrides the eye distance?); shift 0 until 60 frames agree"
                        : "cockpit ipd: pairing agrees again");
    if (!g_loggedK && g_match.kN >= 30) {
      g_loggedK = true;
      Log("cockpit ipd: k = %.4f DCS units per metre%s", g_match.kEst,
          g_match.KBad() ? " (outside 0.95..1.05: inert)" : std::fabs(g_match.kEst - 1) > 0.01 ? " (warning: used)" : "");
    }
  }
}

// The world translation of the store: sent = T(-dp) * vp, i.e. dp = -(row 3
// of W = sent * vp^-1), W a pure translation (rows 0..2 the identity). False
// when it is not one. 0 when that frame was not patched.
bool StoreShift(const PrevStore& p, double dp[3]) {
  dp[0] = dp[1] = dp[2] = 0;
  if (!p.patched || !memcmp(&p.sent, &p.vp, sizeof(M4))) return true;
  M4 inv;
  if (!Inv4(p.vp, &inv)) return false;
  const M4 w = MulM(p.sent, inv);
  for (int r = 0; r < 4; ++r)
    for (int c = 0; c < 4; ++c) {
      if (r == 3 && c < 3) continue;
      if (!(std::fabs(w.a[r][c] - (r == c ? 1.0 : 0.0)) <= 1e-4)) return false;
    }
  for (int i = 0; i < 3; ++i) {
    dp[i] = -w.a[3][i];
    if (!std::isfinite(dp[i])) return false;
  }
  return true;
}

inline double Norm3(const double v[3]) { return std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); }

// A matrix as 16 compact values, rows separated by '|'.
void FmtM(char* out, size_t n, const M4& m) {
  size_t k = 0;
  out[0] = 0;
  for (int r = 0; r < 4 && k < n; ++r) {
    const int w = snprintf(out + k, n - k, "%s%.6g %.6g %.6g %.6g", r ? "|" : "", m.a[r][0], m.a[r][1], m.a[r][2],
                           m.a[r][3]);
    if (w < 0) break;
    k += static_cast<size_t>(w);
  }
}

// [Perception] CockpitTrueIpdPrevLog: the raw fields and the relations of one view's first cockpit upload.
void TracePrev(int t, const uint8_t* s, const PrevStore& st, bool have, const ViewFrame& vf, const double d[3],
               bool centreOk, double xfRes) {
  const unsigned long long fr = static_cast<unsigned long long>(g_frame);
  char a[300];
  FmtM(a, sizeof(a), LoadM(s, kOffViewProj));
  TraceLine("pv %llu v%d VP %s", fr, t, a);
  const M4 pvp = LoadM(s, kOffPrevVP);
  FmtM(a, sizeof(a), pvp);
  TraceLine("pv %llu v%d PVP %s", fr, t, a);
  FmtM(a, sizeof(a), LoadM(s, kOffProj));
  TraceLine("pv %llu v%d P %s", fr, t, a);
  float o[4], cp[4];
  memcpy(o, s + kOffOrigin, sizeof(o));
  memcpy(cp, s + kOffCamPos, sizeof(cp));
  TraceLine("pv %llu v%d origin %.6g %.6g %.6g %.6g campos %.6g %.6g %.6g vi3 %.6g %.6g %.6g xfres %.2g", fr, t, o[0],
            o[1], o[2], o[3], cp[0], cp[1], cp[2], vf.C[0], vf.C[1], vf.C[2], xfRes);
  if (!have) {
    TraceLine("pv %llu v%d prev none (no stored frame %llu)", fr, t, fr ? fr - 1 : 0ull);
    return;
  }
  FmtM(a, sizeof(a), st.vp);
  TraceLine("pv %llu v%d prevVP %s", fr, t, a);
  if (st.patched) {
    FmtM(a, sizeof(a), st.sent);
    TraceLine("pv %llu v%d prevSent %s", fr, t, a);
  }
  M4 inv;
  if (Inv4(st.vp, &inv)) {
    FmtM(a, sizeof(a), MulM(inv, pvp));
    TraceLine("pv %llu v%d Y=prevVP^-1*PVP %s", fr, t, a);
    FmtM(a, sizeof(a), MulM(pvp, inv));
    TraceLine("pv %llu v%d X=PVP*prevVP^-1 %s", fr, t, a);
  }
  TraceLine("pv %llu v%d rel: d %.3f %.3f %.3f mm (ref v%d %.3f %.3f %.3f) prevC %.6g %.6g %.6g prevO %.6g %.6g %.6g "
            "dp %.3f %.3f %.3f mm (recorded %.3f %.3f %.3f) %s%s%s",
            fr, t, d[0] * 1e3, d[1] * 1e3, d[2] * 1e3, g_f.relRefView, g_f.relRef[0] * 1e3, g_f.relRef[1] * 1e3,
            g_f.relRef[2] * 1e3, st.C[0], st.C[1], st.C[2], st.o[0], st.o[1], st.o[2], vf.dpRaw[0] * 1e3,
            vf.dpRaw[1] * 1e3, vf.dpRaw[2] * 1e3, g_prevDw[t][0] * 1e3, g_prevDw[t][1] * 1e3,
            g_prevDw[t][2] * 1e3, !centreOk ? "NO CENTRE" : vf.relOk ? "ok" : "BAD (not this eye's previous camera)",
            vf.storeOk ? "" : ", STORE MISMATCH", g_f.relRefView == t ? ", reference" : "");
}

// The view's first cockpit G-buffer upload (DCS's bytes): the gPrevFrameViewProj
// check against the previous frame's store, Dp from the store, this frame's
// store entry, the exact-equality statistic and the trace.
void CapturePrev(int t, const uint8_t* s) {
  Frame& f = g_f;
  ViewFrame& vf = f.v[t];
  vf.captured = true;
  const M4 pvp = LoadM(s, kOffPrevVP), vp = LoadM(s, kOffViewProj);
  const PrevStore& st = g_store[t];
  const bool have = st.valid && st.tag == g_frame;  // committed by the previous frame (tag = its g_frame + 1)
  float of[3];
  memcpy(of, s + kOffOrigin, sizeof(of));
  const double o[3] = {of[0], of[1], of[2]};
  // Exact equality with DCS's previous gViewProj (the original R6 statistic).
  if (have) {
    double err = 0, sc = 1;
    for (int r = 0; r < 4; ++r)
      for (int c = 0; c < 4; ++c) {
        err = std::max(err, std::fabs(pvp.a[r][c] - st.vp.a[r][c]));
        sc = std::max(sc, std::fabs(pvp.a[r][c]));
      }
    (err <= 1e-5 * sc ? g_st.prevOk : g_st.prevBad)++;
  }
  // gPrevFrameTransform against gViewProjInv * gPrevFrameViewProj (diagnostic).
  double xfRes = 0;
  {
    const M4 x = LoadM(s, kOffPrevXf), ref = MulM(LoadM(s, kOffViewProjInv), pvp);
    double sc = 1;
    for (int r = 0; r < 4; ++r)
      for (int c = 0; c < 4; ++c) {
        xfRes = std::max(xfRes, std::fabs(x.a[r][c] - ref.a[r][c]));
        sc = std::max(sc, std::fabs(ref.a[r][c]));
      }
    xfRes /= sc;
    (xfRes <= 1e-3 ? g_st.xfStd : g_st.xfOther)++;
  }
  // Dp from the store; it must equal the Dw recorded for that view (bookkeeping).
  vf.storeOk = true;
  if (have && !StoreShift(st, vf.dpStore)) vf.storeOk = false;
  if (!have) vf.dpStore[0] = vf.dpStore[1] = vf.dpStore[2] = 0;
  const double tolS = 2e-5 + 2e-6 * Norm3(vf.C);
  for (int i = 0; i < 3; ++i)
    if (!(std::fabs(vf.dpStore[i] - g_prevDw[t][i]) <= tolS)) vf.storeOk = false;
  // Verified: the sent matrix is T(-Dp) * DCS's for the recorded Dp. Dp is
  // then that exact vector (the matrix inverse adds ~1e-17 of noise, which
  // would turn every unpatched frame into a "tail" one).
  for (int i = 0; i < 3; ++i) vf.dpRaw[i] = vf.dpStore[i];
  if (vf.storeOk)
    for (int i = 0; i < 3; ++i) vf.dpStore[i] = g_prevDw[t][i];
  if (!vf.storeOk) {
    g_st.storeMis++;
    f.relFail = true;
  }
  // The camera of gPrevFrameViewProj against DCS's previous camera of this view.
  double d[3] = {};
  bool centreOk = true;
  vf.relHave = have;
  vf.relOk = true;
  if (have) {
    double cp[3];
    centreOk = Centre(pvp, cp);
    if (!centreOk) {
      vf.relOk = false;
      g_st.relNoCentre++;
    } else {
      for (int i = 0; i < 3; ++i) d[i] = cp[i] - st.C[i];
      const double tol = kRelTolM * g_match.K() + 1e-6 * (Norm3(st.C) + Norm3(cp));
      if (!f.relRefSet) {  // the reference: which origin delta (if any) explains it, diagnostic only
        f.relRefSet = true;
        f.relRefView = t;
        for (int i = 0; i < 3; ++i) f.relRef[i] = d[i];
        g_st.relRef++;
        const double tolO = tol + 1e-6 * (Norm3(o) + Norm3(st.o));
        int k = 3;
        const double ks[3] = {0, 1, -1};
        for (int j = 0; j < 3 && k == 3; ++j) {
          double e[3];
          for (int i = 0; i < 3; ++i) e[i] = d[i] - ks[j] * (st.o[i] - o[i]);
          if (Norm3(e) <= tolO) k = j;
        }
        g_st.rebase[k]++;
      } else {
        ++f.relCmp;
        double e[3];
        for (int i = 0; i < 3; ++i) e[i] = d[i] - f.relRef[i];
        vf.relOk = Norm3(e) <= tol;
      }
    }
    if (f.relRefView != t) (vf.relOk ? g_st.relOk : g_st.relBad)++;
    if (!vf.relOk) f.relFail = true;
  } else {
    g_st.relNoStore++;
  }
  // This frame's entry (the sent matrix is set when the upload is patched).
  PrevStore& c = g_cur[t];
  c.valid = true;
  c.tag = g_frame + 1;
  c.vp = vp;
  c.sent = vp;
  c.patched = false;
  for (int i = 0; i < 3; ++i) c.C[i] = vf.C[i], c.o[i] = o[i];
  if (g_prevLogLeft.load(std::memory_order_relaxed) > 0) {
    TracePrev(t, s, st, have, vf, d, centreOk, xfRes);
    g_prevTraced = true;
  }
}

// An overlay pass's cPerView describes the view's own camera: DCS's camera of
// the view's cockpit G-buffer upload (position and axes). Anything else (an MFD
// or 2D GUI camera) is forwarded as DCS's.
bool SameCamera(const uint8_t* s, const ViewFrame& vf) {
  if (!vf.haveOrig) return false;
  double C[3], R[3][3];
  Camera(s, C, R);
  const double tol = 1e-4 * (1 + Norm3(vf.C));
  for (int i = 0; i < 3; ++i) {
    if (!(std::fabs(C[i] - vf.C[i]) <= tol)) return false;
    for (int j = 0; j < 3; ++j)
      if (!(std::fabs(R[i][j] - vf.R[i][j]) <= 1e-4)) return false;
  }
  return true;
}

// Pure: does the constant data hold a 4x4 matrix M (either storage order)
// with X = M * vp^-1 affine (column 3 = (0, 0, 0, 1)), i.e. an MVP of the
// camera vp describes? 0 none, 1 an MVP (model != identity), 2 vp itself.
// DLSS jitter (a clip-space shear) and another near / far keep column 3.
// The matrix at `off` in one storage order (0: DCS's transposed float4x4, 1:
// row-major) when it is an MVP of the camera whose inverse VP is vpi: m the
// logical matrix, x = m * vpi (affine). Pure.
// res: the worst row's cancellation residual of column 3, |x[r][3] - (0, 0, 0, 1)[r]| over the sum of the
// absolute products that make it: float rounding for a real camera matrix (~1e-7, ~1e-4 with render
// coordinates of a few km), O(1) for a window that only happens to pass the gates (a misaligned one over a
// real matrix with large entries); refused above kMvpMaxRes, and the scan keeps the best of overlapping hits.
constexpr double kMvpMaxRes = 1e-2;
bool RecogniseMvp(const uint8_t* data, UINT off, int order, const M4& vpi, double vpiMax, M4* m, M4* x,
                  double* res = nullptr) {
  *m = LoadM(data, off);
  if (order) {
    M4 t;
    for (int r = 0; r < 4; ++r)
      for (int c = 0; c < 4; ++c) t.a[r][c] = m->a[c][r];
    *m = t;
  }
  double sc = 0;
  for (int r = 0; r < 4; ++r)
    for (int c = 0; c < 4; ++c) {
      if (!std::isfinite(m->a[r][c])) return false;
      sc = std::max(sc, std::fabs(m->a[r][c]));
    }
  if (!(sc > 1e-6)) return false;
  // Column 3 of x first (16 products): it must be (0, 0, 0, 1) within 1e-3 * max(1, xs), and xs is at most
  // 4 * sc * vpiMax; most constants fail here.
  double dev = 0, worst = 0, e[4], a[4], aMax = 0;
  for (int r = 0; r < 4; ++r) {
    double v = 0;
    a[r] = 0;
    for (int k = 0; k < 4; ++k) {
      v += m->a[r][k] * vpi.a[k][3];
      a[r] += std::fabs(m->a[r][k] * vpi.a[k][3]);
    }
    e[r] = std::fabs(r == 3 ? v - 1 : v);
    dev += e[r];
    aMax = std::max(aMax, a[r]);
  }
  // (A row whose terms are all at the inverse's noise floor is measured against the largest row.)
  for (int r = 0; r < 4; ++r)
    if (e[r] > 0) worst = std::max(worst, e[r] / std::max(a[r], 1e-6 * aMax + 1e-30));
  if (!(dev <= 1e-3 * std::max(1.0, 4 * sc * vpiMax)) || !(worst <= kMvpMaxRes)) return false;
  if (res) *res = worst;
  *x = MulM(*m, vpi);
  double xs = 0;
  for (int r = 0; r < 4; ++r)
    for (int c = 0; c < 3; ++c) xs = std::max(xs, std::fabs(x->a[r][c]));
  if (!(xs > 1e-3) || !std::isfinite(xs)) return false;
  dev = std::fabs(x->a[0][3]) + std::fabs(x->a[1][3]) + std::fabs(x->a[2][3]) + std::fabs(x->a[3][3] - 1);
  return dev <= 1e-3 * std::max(1.0, xs);
}
inline double MaxAbs3(const M4& m) {
  double s = 0;
  for (int r = 0; r < 4; ++r)
    for (int c = 0; c < 3; ++c) s = std::max(s, std::fabs(m.a[r][c]));
  return s;
}

// Pure: does the constant data hold a 4x4 matrix M (either storage order)
// with X = M * vp^-1 affine (column 3 = (0, 0, 0, 1)), i.e. an MVP of the
// camera vp describes? 0 none, 1 an MVP (model != identity), 2 vp itself.
// DLSS jitter (a clip-space shear) and another near / far keep column 3.
// The whole buffer is scanned (font.fx keeps its MVP at byte 2160).
int MvpOfCamera(const uint8_t* data, UINT len, const M4& vp) {
  M4 vpi;
  if (!Inv4(vp, &vpi)) return 0;
  const double vm = MaxAbs3(vpi);
  int found = 0;
  for (UINT off = 0; off + 64 <= len && off + 64 <= kMaxSrc; off += 16) {
    for (int order = 0; order < 2; ++order) {
      M4 m, x;
      if (!RecogniseMvp(data, off, order, vpi, vm, &m, &x)) continue;
      double xs = MaxAbs3(x), id = 0;
      for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) id = std::max(id, std::fabs(x.a[r][c] - (r == c ? 1.0 : 0.0)));
      found = std::max(found, id <= 1e-3 * std::max(1.0, xs) ? 2 : 1);
      if (found == 2) return 2;
    }
  }
  return found;
}

// The overlay MVP gate on the model X = MVP * VP^-1 (pure). The patch
// row 3 += (-Dw, 0) * VP is exact for every affine X (sheared, rotated after a
// non-uniform scale, flat, far away: MVP' = X * T(-Dw) * VP), so the gate only
// refuses what cannot be a transform at all: a non-finite matrix or axis
// lengths outside [kMvpMinScale, kMvpMaxScale] (DCS units per model unit).
// Until 2026-10-13 it also refused axes more than 75.5 degrees apart and
// origins more than 100 km from the camera: a symbol rotated in a non-square
// indicator (R(theta) * S(1, a)) flipped between patched and DCS's camera with
// its angle, and every effect upload carried one recognised camera matrix the
// distance test refused (world-space symbology: X ~ identity, origin at the
// world origin, hundreds of km away), left at the boosted eye next to the
// patched MVP. Far origins are now only counted (kMvpFarDist).
constexpr double kMvpMinScale = 1e-7, kMvpMaxScale = 1e7, kMvpFarDist = 1e5;
struct ModelInfo {
  double len[3] = {}, maxCos = 0, dist = 0;
  bool usable = false, isFar = false;
};
ModelInfo DescribeModel(const M4& x, const double C[3]) {
  ModelInfo mi;
  double lmax = 0;
  bool finite = true;
  for (int i = 0; i < 3; ++i) {
    mi.len[i] = std::sqrt(x.a[i][0] * x.a[i][0] + x.a[i][1] * x.a[i][1] + x.a[i][2] * x.a[i][2]);
    finite = finite && std::isfinite(mi.len[i]);
    lmax = std::max(lmax, mi.len[i]);
  }
  for (int i = 0; i < 3; ++i)
    for (int j = i + 1; j < 3; ++j)
      if (mi.len[i] > 1e-3 * lmax && mi.len[j] > 1e-3 * lmax) {
        const double dot = x.a[i][0] * x.a[j][0] + x.a[i][1] * x.a[j][1] + x.a[i][2] * x.a[j][2];
        mi.maxCos = std::max(mi.maxCos, std::fabs(dot) / (mi.len[i] * mi.len[j]));
      }
  double d2 = 0;
  for (int i = 0; i < 3; ++i) {
    const double d = x.a[3][i] - C[i];
    d2 += d * d;
    finite = finite && std::isfinite(x.a[3][i]);
  }
  mi.dist = std::sqrt(d2);
  mi.usable = finite && lmax >= kMvpMinScale && lmax <= kMvpMaxScale;
  mi.isFar = mi.usable && !(mi.dist <= kMvpFarDist);
  return mi;
}
bool PlausibleModel(const M4& x, const double C[3]) { return DescribeModel(x, C).usable; }

// Pure: every MVP of the camera (vpi = its VP^-1, vm = MaxAbs3(vpi)) in
// data[0, len) (either storage order, non-overlapping), with its verdict.
// Every 16-byte offset is a candidate; overlapping candidates keep the one
// with the smallest residual (a real matrix beats a misaligned window over it,
// which can pass the gates when its entries are large: before 2026-10-13 such
// a window could be taken first and hide the real matrix behind it).
constexpr int kMvpMaxHits = 32;
struct MvpHit {
  UINT off = 0;
  int order = 0;
  bool usable = false;
  double res = 0;
  ModelInfo mi;
};
struct MvpScan {
  int patched = 0, implausible = 0, nFar = 0;  // usable (patched by ApplyMvps), refused, usable but far
  int n = 0;
  MvpHit hit[kMvpMaxHits];
};
MvpScan ScanMvps(const uint8_t* data, UINT len, const M4& vpi, double vm, const double C[3]) {
  MvpScan out;
  constexpr int kCand = 64;
  MvpHit cand[kCand];
  int nc = 0;
  for (UINT off = 0; off + 64 <= len && off + 64 <= kMaxSrc; off += 16) {
    MvpHit best;
    bool have = false;
    for (int order = 0; order < 2; ++order) {
      M4 m, x;
      double res = 0;
      if (!RecogniseMvp(data, off, order, vpi, vm, &m, &x, &res)) continue;
      if (have && !(res < best.res)) continue;
      have = true;
      best.off = off;
      best.order = order;
      best.res = res;
      best.mi = DescribeModel(x, C);
      best.usable = best.mi.usable;
    }
    if (!have) continue;
    if (nc < kCand) {
      cand[nc++] = best;
    } else {  // full (not expected): replace the worst candidate if this one is better
      int w = 0;
      for (int i = 1; i < kCand; ++i)
        if (cand[i].res > cand[w].res) w = i;
      if (best.res < cand[w].res) cand[w] = best;
    }
  }
  // Best residual first; a candidate overlapping an accepted one is dropped.
  std::sort(cand, cand + nc, [](const MvpHit& a, const MvpHit& b) { return a.res < b.res; });
  for (int i = 0; i < nc && out.n < kMvpMaxHits; ++i) {
    bool overlaps = false;
    for (int j = 0; j < out.n && !overlaps; ++j)
      overlaps = cand[i].off < out.hit[j].off + 64 && out.hit[j].off < cand[i].off + 64;
    if (overlaps) continue;
    out.hit[out.n++] = cand[i];
    if (!cand[i].usable) {
      ++out.implausible;
    } else {
      ++out.patched;
      if (cand[i].mi.isFar) ++out.nFar;
    }
  }
  std::sort(out.hit, out.hit + out.n, [](const MvpHit& a, const MvpHit& b) { return a.off < b.off; });
  return out;
}// Pure: the usable hits of `sc` (found in these very bytes) get row 3 += d,
// d = (-dw, 0) * VP; rows 0..2 keep their bits, a zero delta keeps a cell's.
void ApplyMvps(uint8_t* data, const MvpScan& sc, const M4& vp, const double dw[3]) {
  double d[4];
  for (int c = 0; c < 4; ++c) d[c] = -(dw[0] * vp.a[0][c] + dw[1] * vp.a[1][c] + dw[2] * vp.a[2][c]);
  for (int i = 0; i < sc.n; ++i) {
    const MvpHit& h = sc.hit[i];
    if (!h.usable) continue;
    M4 m = LoadM(data, h.off);
    if (h.order) {
      M4 t;
      for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) t.a[r][c] = m.a[c][r];
      m = t;
    }
    for (int c = 0; c < 4; ++c) AddTo(m.a[3][c], d[c]);
    if (h.order) {
      M4 t;
      for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) t.a[r][c] = m.a[c][r];
      m = t;
    }
    StoreM(data, h.off, m);
  }
}
// Pure: every MVP of the camera vp in data[0, len) whose model is usable gets
// MVP' = MVP * VP^-1 * T(-dw) * VP, i.e. row 3 += (-dw, 0) * VP (exact for an
// affine model; rows 0..2 keep their bits, a zero delta keeps a cell's bits).
// C: the camera (gViewInv row 3), for the far count. Patched in place.
MvpScan PatchMvps(uint8_t* data, UINT len, const M4& vp, const double dw[3], const double C[3]) {
  M4 vpi;
  if (!Inv4(vp, &vpi)) return MvpScan{};
  const MvpScan sc = ScanMvps(data, len, vpi, MaxAbs3(vpi), C);
  ApplyMvps(data, sc, vp, dw);
  return sc;
}

// Per effect buffer (identity and size), the matrices its last full scan found: the next uploads test only
// those offsets (each one verified again: a wrong entry can only miss, never patch a non-matrix); a full scan
// every kMvpRescan uploads of the buffer, and whenever a learned offset no longer holds a camera matrix
// (live 2026-10-13: ~660 effect uploads per frame, the full scan of each cost ~2 ms of render thread).
constexpr int kMvpCache = 256, kMvpRescan = 32, kMvpCacheHits = 4;
struct MvpCacheEntry {
  ID3D11Resource* res = nullptr;
  UINT len = 0;
  int n = 0, uses = 0;
  UINT off[kMvpCacheHits] = {};
  int order[kMvpCacheHits] = {};
};
MvpCacheEntry g_mvpCache[kMvpCache];
std::atomic<uint64_t> g_mvpFull{0}, g_mvpFast{0};  // full scans / cached
// The scan through the cache (render thread). Same result as ScanMvps for a buffer whose layout did not change.
MvpScan ScanMvpsCached(ID3D11Resource* r, const uint8_t* data, UINT len, const M4& vpi, double vm, const double C[3]) {
  MvpCacheEntry& e = g_mvpCache[(reinterpret_cast<uintptr_t>(r) >> 4) % kMvpCache];
  if (e.res == r && e.len == len && e.n >= 0 && (++e.uses % kMvpRescan) != 0) {
    MvpScan out;
    bool ok = true;
    for (int i = 0; i < e.n && ok; ++i) {
      M4 m, x;
      double res = 0;
      if (!RecogniseMvp(data, e.off[i], e.order[i], vpi, vm, &m, &x, &res)) {
        ok = false;
        break;
      }
      MvpHit& h = out.hit[out.n++];
      h.off = e.off[i];
      h.order = e.order[i];
      h.res = res;
      h.mi = DescribeModel(x, C);
      h.usable = h.mi.usable;
      if (!h.usable) {
        ++out.implausible;
      } else {
        ++out.patched;
        if (h.mi.isFar) ++out.nFar;
      }
    }
    if (ok) {
      g_mvpFast++;
      return out;
    }
  }
  g_mvpFull++;
  const MvpScan sc = ScanMvps(data, len, vpi, vm, C);
  e.res = r;
  e.len = len;
  e.uses = 1;
  e.n = sc.n <= kMvpCacheHits ? sc.n : -1;  // more matrices than the cache holds: always the full scan
  for (int i = 0; i < sc.n && i < kMvpCacheHits; ++i) e.off[i] = sc.hit[i].off, e.order[i] = sc.hit[i].order;
  return sc;
}

// DCS's gViewProj of view t in this frame (the store entry CapturePrev wrote),
// its inverse and MaxAbs3, computed once per view and frame (render thread).
struct OvlCam {
  uint64_t tag = 0;
  bool ok = false;
  M4 vp{}, vpi{};
  double vm = 0;
};
OvlCam g_ovlCam[4];
const OvlCam* OvlCamOf(int t) {
  const PrevStore& c = g_cur[t];
  if (!c.valid || c.tag != g_frame + 1) return nullptr;
  OvlCam& o = g_ovlCam[t];
  if (o.tag != c.tag) {
    o.tag = c.tag;
    o.vp = c.vp;
    o.ok = Inv4(c.vp, &o.vpi);
    o.vm = o.ok ? MaxAbs3(o.vpi) : 0;
  }
  return o.ok ? &o : nullptr;
}

// [Perception] CockpitTrueIpdMvpDump=N: the first N recognised matrices of
// the overlay passes' effect uploads (per load), one trace line each.
std::atomic<int> g_cfgMvpDump{0};
std::atomic<int> g_mvpDumpLeft{0};  // Configure (a new value) and ResetMvpState set it; the render thread counts down
void ResetMvpState() {
  for (OvlCam& o : g_ovlCam) o = OvlCam{};
  for (MvpCacheEntry& e : g_mvpCache) e = MvpCacheEntry{};
  g_mvpDumpLeft = std::clamp(g_cfgMvpDump.load(), 0, 2000);
}
void DumpMvps(int t, ID3D11Resource* r, UINT len, const MvpScan& sc) {
  for (int i = 0; i < sc.n && g_mvpDumpLeft.load(std::memory_order_relaxed) > 0; ++i, g_mvpDumpLeft--) {
    const MvpHit& h = sc.hit[i];
    TraceLine("mvp frame %llu v%d buf %p bytes %u off %u %s (residual %.2g): axes %.4g %.4g %.4g, max |cos| %.3f, origin %.4g from "
              "the camera: %s%s",
              static_cast<unsigned long long>(g_frame), t, static_cast<void*>(r), len, h.off,
              h.order ? "row-major" : "column-major", h.res, h.mi.len[0], h.mi.len[1], h.mi.len[2], h.mi.maxCos, h.mi.dist,
              h.usable ? "patched" : "refused (scale / not finite)", h.mi.isFar ? " (far)" : "");
  }
}

#include "ovl_cpu.h"  // R32: the symbology's camera set per record on the CPU (CockpitTrueIpdOverlayPath=cpu)

// An overlay pass's upload of a buffer that is not a learned cPerView (render
// thread): the bounded probe (reported); on the cpu path the observer of the
// record classes (nothing written); on the d3d11 path the MVP patch of a moved
// view (CockpitTrueIpdOverlayMvp). The pointer to forward: g_scratch when patched.
// The source is scanned in place; it is copied only when a matrix is patched.
const void* OvlEffectUpload(ID3D11Resource* r, UINT sub, const D3D11_BOX* box, const void* src, bool us1) {
  if (!r || !src || t_view < 0 || t_view > 3) return src;
  const int t = t_view;
  const OvlCam* cam = OvlCamOf(t);  // DCS's gViewProj of this view in this frame
  if (!cam) return src;
  const bool patchOn = g_cfgOvlMvp.load(std::memory_order_relaxed);
  if (g_ovlProbeLeft <= 0 && !patchOn && !PathCpu()) return src;
  if (sub != 0) {
    if (patchOn) g_st.mvpSub++;
    return src;
  }
  // A buffer: its ID3D11Buffer interface is the resource pointer itself (no QueryInterface / reference).
  D3D11_RESOURCE_DIMENSION dim = D3D11_RESOURCE_DIMENSION_UNKNOWN;
  r->GetType(&dim);
  if (dim != D3D11_RESOURCE_DIMENSION_BUFFER) return src;
  D3D11_BUFFER_DESC bd{};
  static_cast<ID3D11Buffer*>(r)->GetDesc(&bd);
  if (!(bd.BindFlags & D3D11_BIND_CONSTANT_BUFFER)) return src;
  if (box && (box->top != 0 || box->bottom != 1 || box->front != 0 || box->back != 1)) return src;
  const UINT len = box ? (box->right > box->left ? box->right - box->left : 0) : bd.ByteWidth;
  if (len < 64) return src;
  const uint8_t* s = static_cast<const uint8_t*>(src);
  if (g_ovlProbeLeft > 0) {
    --g_ovlProbeLeft;
    g_st.ovlProbe++;
    const int k = MvpOfCamera(s, std::min<UINT>(len, kMaxSrc), cam->vp);
    if (k == 1) g_st.ovlProbeMvp++;
    if (k == 2) g_st.ovlProbeVp++;
  }
  if (PathCpu()) {  // R32: DCS built these bytes with the camera we set per record; they are only observed
    if (ocpu::Installed()) ocpu::ObserveUpload(r, s, std::min<UINT>(len, kMaxSrc));
    return src;
  }
  if (!patchOn) return src;
  const Frame& f = g_f;
  ViewFrame& vf = g_f.v[t];
  if (f.off || !vf.patched || vf.failed || !NonZero(vf.dw) || !vf.haveOrig) return src;
  g_st.mvpSeen++;
  if (len > kMaxSrc) {
    g_st.mvpBig++;
    return src;
  }
  const MvpScan sc = ScanMvpsCached(r, s, len, cam->vpi, cam->vm, vf.C);
  if (g_mvpDumpLeft > 0 && sc.n) DumpMvps(t, r, len, sc);
  g_st.mvpRejMatrices += static_cast<uint64_t>(sc.implausible);
  g_st.mvpFar += static_cast<uint64_t>(sc.nFar);
  if (!sc.patched) {
    // Nothing usable: DCS's bytes. An upload with refused matrices only draws them at the boosted eye.
    (sc.implausible ? g_st.mvpImplOnly : g_st.mvpNone)++;
    if (sc.implausible) ++vf.mvpRejN;
    return src;
  }
  if (sc.implausible) g_st.mvpMixed++;  // the usable ones are patched, the refused ones left
  if (f.observe) {
    g_st.mvpObserved++;
    ++t_mvpPatchedN;
    return src;
  }
  // Not restored at the pass end (CockpitTrueIpdOverlayRestore=0, the default) for simplicity only. The reason
  // given here before R32 was wrong: DCS does not skip "unchanged" effect uploads (FX11 CheckAndUpdateCB_FX tests
  // only its dirty bit, and every setter sets it: one upload before every symbology draw, dx11backend 0x68620), so
  // restore and no-restore draw the same. The frames whose uploads dropped from ~165 were uploads that went past
  // our hooks after a d3d11 table toggle (R32 2.3 / 2.4): the reason this path can never be complete, and why the
  // cpu path replaced it. This path also moves the collimated HUD to the true eye, which de-collimates it (R32 2.6).
  if (g_cfgOvlRestore.load(std::memory_order_relaxed) &&
      !NoteRestore(r, box ? box->left : 0, box ? box->right : len, box, s, us1, true)) {
    g_st.mvpRestFull++;
    ++vf.mvpRejN;
    return src;
  }
  memcpy(g_scratch, s, len);
  ApplyMvps(g_scratch, sc, cam->vp, vf.dw);
  g_st.mvpPatched++;
  g_st.mvpMatrices += static_cast<uint64_t>(sc.patched);
  ++t_mvpPatchedN;
  ++vf.mvpN;
  return g_scratch;
}

// A cockpit-class upload of view t in a frame that is off (offNext, offMulti,
// offView0; not foreign hooks, not a refuted gPrevFrameViewProj, not observe):
// gPrevFrameViewProj / gPrevFrameTransform moved by the verified store's Dp,
// every other byte DCS's (gViewProj unchanged: Dw 0). Only for a view not
// patched in full earlier in this frame (a second cockpit pass keeps DCS's).
const void* TailOff(ID3D11Resource* r, const D3D11_BOX* box, const uint8_t* base, UINT at, UINT len, bool us1, int t) {
  Frame& f = g_f;
  ViewFrame& vf = f.v[t];
  const uint8_t* s = base + at;
  if (!f.offTail || f.observe || g_relRefuted.load(std::memory_order_relaxed) || (t_cls != kClsGb && t_cls != kClsFwd))
    return base;
  if (vf.patched && !vf.tailOff) return base;
  if (!vf.captured || !vf.storeOk || (vf.relHave && !vf.relOk) || !NonZero(vf.dpStore) || vf.failed) return base;
  if (SelfCheck(s) & ~kChkCam) return base;
  if (!NoteRestore(r, box ? box->left : 0, box ? box->right : len, box, base, us1, true)) return base;
  const double zero[3] = {0, 0, 0};
  memcpy(g_scratch, base, len);
  PatchStruct(g_scratch + at, zero, vf.dpStore);
  if (!vf.tailOff) g_st.offTail++;
  vf.tailOff = true;
  vf.patched = true;  // dw 0: EndFrame records g_prevDw 0 and the store's sent = DCS's gViewProj
  for (int i = 0; i < 3; ++i) vf.dw[i] = 0, vf.dp[i] = vf.dpStore[i];
  vf.dwReady = true;
  ++t_patchedN;
  if (t_cls == kClsGb && !g_cur[t].patched && g_cur[t].tag == g_frame + 1) {
    g_cur[t].sent = LoadM(g_scratch + at, kOffViewProj);
    g_cur[t].patched = true;
  }
  g_st.patched++;
  (t_cls == kClsGb ? g_st.gbPatched : g_st.fwdPatched)++;
  return g_scratch;
}

// UsHook / Us1Hook (render thread, DCS's context): the pointer to forward.
const void* OnUploadImpl(ID3D11Resource* r, UINT sub, const D3D11_BOX* box, const void* src, bool us1) {
  if (!src || t_cls == kClsNone) return src;
  if (!IsKnown(r)) {
    if (t_cls == kClsGb || t_cls == kClsFwd) g_st.unknownBuf++;
    if (t_cls == kClsOvl) {
      {
        bool have = false;
        for (int i = 0; i < t_ovlUpN && !have; ++i) have = t_ovlUp[i] == r;
        if (!have && t_ovlUpN < 64) t_ovlUp[t_ovlUpN++] = r;
      }
      for (int i = 0; i < g_ovlRestN; ++i)
        if (g_ovlRest[i].res == r) g_ovlRest[i].uploaded = true;
      return OvlEffectUpload(r, sub, box, src, us1);  // probe; MVPs of a moved view
    }
    return src;
  }
  g_st.uploadsSeen++;
  ++t_uploads;
  if (t_cls == kClsLight) ++t_lightUploads;  // every known struct upload of the lighting arm (LightB7True)
  if (t_cls == kClsWatch) return src;
  if (sub != 0) {
    g_st.subNz++;
    return src;
  }
  if (box && (box->top != 0 || box->bottom != 1 || box->front != 0 || box->back != 1 || box->right <= box->left)) {
    g_st.partialBox++;
    return src;
  }
  Known* k = PickKnown(r, box ? box->left : 0, box ? box->right : 0xffffffffu);
  if (!k) {
    // No learned struct of this buffer inside the box, or several and b7 does not say which one.
    (box ? g_st.partialBox : g_st.ambiguous)++;
    return src;
  }
  const uint8_t* base = static_cast<const uint8_t*>(src);
  UINT len = k->size, at = k->base;
  if (box) {
    len = box->right - box->left;
    at = k->base - box->left;
  }
  if (len > kMaxSrc || at + kCbBytes > len) {
    g_st.bigSrc++;
    return src;
  }
  const uint8_t* s = base + at;
  if (t_cls == kClsOwn || t_cls == kClsOwnFwd) return OwnUpload(r, box, base, at, len, us1);
  if (t_cls == kClsLight) {
    // Phase 2: DCS's bytes of this struct kept (the split's world draw binds them), and the upload itself
    // forwarded at the true eye: whatever DCS draws with it, also a draw that goes past the draw hooks, lights
    // the cockpit pixels from the camera their G-buffer was drawn with. Restored at the arm's end.
    LightCopy* lc = nullptr;
    for (LightCopy& l : g_lightCopy)
      if (l.res == r && l.base == k->base) lc = &l;
    if (!lc)
      for (LightCopy& l : g_lightCopy)
        if (!l.valid) {
          lc = &l;
          break;
        }
    if (!lc) lc = &g_lightCopy[1];
    lc->res = r;
    lc->base = k->base;
    lc->len = kCbBytes;
    memcpy(lc->bytes, s, kCbBytes);
    lc->valid = true;
    lc->fwdPatched = false;
    const int tl = t_view;
    if (tl < 0 || tl > 3) return src;
    const ViewFrame& vl = g_f.v[tl];
    if (g_f.off || g_f.observe || !vl.patched || vl.failed || !NonZero(vl.dw)) return src;
    if (!NoteRestore(r, box ? box->left : 0, box ? box->right : len, box, base, us1, true)) {
      g_st.restFull++;
      return src;
    }
    memcpy(g_scratch, base, len);
    PatchStruct(g_scratch + at, vl.dw, vl.dp);
    lc->fwdPatched = true;
    ++t_lightFwdN;
    g_st.lightFwd++;
    return g_scratch;
  }
  const int t = t_view;
  if (t < 0 || t > 3) return src;
  Frame& f = g_f;
  ViewFrame& vf = f.v[t];
  if (t_cls == kClsOvl) {
    // An overlay pass of a view whose G-buffer was patched (OvlBegin): the
    // view's own Dw / Dp, nothing latched, verified or recorded here.
    g_st.ovlUploads++;
    if (!SameCamera(s, vf)) {  // another camera (2D GUI, a sensor view): DCS's, not a failure
      g_st.ovlOtherCam++;
      ++t_ovlOther;
      return src;
    }
    // The cpu path leaves b7 as DCS's: a pure eye translation of the camera does not move any symbol through it
    // (line widening and the GS use gProj / gProjInv / gViewInv consistently; R32 3).
    if (PathCpu()) return src;
    if (f.off || !vf.patched || vf.failed || !NonZero(vf.dw)) return src;  // a failure at the pass end
    ++g_ovlAttempts;
    if (SelfCheck(s) & ~kChkCam) {
      ++g_ovlFails;
      g_st.selfFail++;
      return src;
    }
    if (!f.observe && !NoteRestore(r, box ? box->left : 0, box ? box->right : len, box, base, us1, true)) {
      ++g_ovlFails;
      g_st.restFull++;
      return src;
    }
    ++t_patchedN;
    if (f.observe) {
      g_st.observed++;
      return src;
    }
    memcpy(g_scratch, base, len);
    PatchStruct(g_scratch + at, vf.dw, vf.dp);
    g_st.patched++;
    g_st.ovlPatched++;
    return g_scratch;
  }
  if (!vf.haveOrig) {
    vf.haveOrig = true;
    Camera(s, vf.C, vf.R);
    vf.fovOk = StructFov(s, vf.fov);
    if (t_cls == kClsGb) CapturePrev(t, s);
    NoteDcsEye(t, vf.C);
    if (t == 0) {  // (the world trace: gOrigin of view 0's camera)
      float o[4];
      memcpy(o, s + kOffOrigin, sizeof(o));
      for (int k = 0; k < 3; ++k) g_dcsOrigin[k] = o[k];
    }
  }
  // A frame that is off: recorded for the gPrev check; when the previous frame
  // drew this view's cockpit at T(-Dw), the gPrev tail still goes out (Dw 0,
  // Dp from the verified store), so its motion vectors match what is in DLSS's
  // history (header: "a frame whose Dw is 0 with Dp != 0 patches the tail").
  if (t_capOnly) return TailOff(r, box, base, at, len, us1, t);
  if (t == 0 && !f.latched) LatchFrame(s);
  if (t == 1 && !f.verified && f.v[0].haveOrig) VerifyFrame();
  if (!f.latched) return src;
  if (f.off) return TailOff(r, box, base, at, len, us1, t);
  ComputeDw(t);
  if (!NonZero(vf.dw) && !NonZero(vf.dp)) return src;
  // R6 gate: gPrevFrameViewProj is not the previous camera of this view's eye
  // (or the store disagrees with the recorded Dw): not patched, rather than
  // patched with wrong motion vectors.
  if ((vf.relHave && !vf.relOk) || !vf.storeOk) {
    if (!vf.gated) {
      g_st.prevGate++;
      if (g_gateLogs < 5) {
        ++g_gateLogs;
        Log("cockpit ipd: frame %llu view %d not patched: %s", static_cast<unsigned long long>(g_frame), t,
            !vf.storeOk ? "the stored previous gViewProj disagrees with the recorded shift"
                        : "gPrevFrameViewProj is not the previous camera of this view's eye");
      }
    }
    vf.gated = true;
    vf.failed = true;
    return src;
  }
  const uint32_t chk = SelfCheck(s);
  if (chk & kChkCam) g_st.camFar++;
  ++g_winAttempts;
  if (chk & ~kChkCam) {
    ++g_winFails;
    g_st.selfFail++;
    vf.selfFailed = true;
    vf.failed = true;
    return src;
  }
  // DCS's bytes for the restore at the pass end; no free entry: not patched
  // (a patched buffer that cannot be restored would leak into later passes).
  if (!f.observe && !NoteRestore(r, box ? box->left : 0, box ? box->right : len, box, base, us1, true)) {
    ++g_winFails;
    g_st.restFull++;
    vf.failed = true;
    return src;
  }
  vf.patched = true;
  f.anyPatched = true;
  ++t_patchedN;
  if (!NonZero(vf.dw)) g_st.tail++;
  if (f.observe) {
    g_st.observed++;
    return src;
  }
  memcpy(g_scratch, base, len);
  if (PatchStruct(g_scratch + at, vf.dw, vf.dp) & kFXfAlt) g_st.xfAlt++;
  if (t_cls == kClsGb || t_cls == kClsFwd) {  // for the helmet models' camera switch in this pass
    HelmetSrc& hs = t_helmetSrc;
    hs.valid = true;
    hs.res = r;
    hs.at = at;
    hs.len = len;
    hs.hasBox = box != nullptr;
    if (box) hs.box = *box;
    hs.us1 = us1;
    hs.frame = g_frame;
    hs.view = t;
    memcpy(hs.dcs, base, len);
    memcpy(hs.sent, g_scratch, len);
  }
  if (t_cls == kClsGb && !g_cur[t].patched && g_cur[t].tag == g_frame + 1) {  // the gViewProj really sent
    g_cur[t].sent = LoadM(g_scratch + at, kOffViewProj);
    g_cur[t].patched = true;
  }
  g_st.patched++;
  (t_cls == kClsGb ? g_st.gbPatched : g_st.fwdPatched)++;
  return g_scratch;
}

// The pointer to forward; every upload forwarded with DCS's bytes is merged
// into the pass's restore entries (the restore never writes older bytes).
const void* OnUpload(ID3D11Resource* r, UINT sub, const D3D11_BOX* box, const void* src, bool us1) {
  const void* out = OnUploadImpl(r, sub, box, src, us1);
  if (out == src) NoteForwarded(r, sub, box, src);
  return out;
}

// UsHook / Us1Hook. Counted in g_busy before g_on is read, so Stop (which
// clears g_on, then drains g_busy) never releases or resets state under it.
// After a fault (Active() false, g_on still set) only the restore bookkeeping
// runs: the pass end, closed on the render thread, restores DCS's latest bytes.
const void* OnUploadSeh(ID3D11Resource* r, UINT sub, const D3D11_BOX* box, const void* src, bool us1) {
  g_busy.fetch_add(1);
  const void* out = src;
  if (g_on.load()) {
    __try {
      if (Active())
        out = OnUpload(r, sub, box, src, us1);
      else
        NoteForwarded(r, sub, box, src);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
      g_fault = true;
      out = src;
    }
  }
  g_busy.fetch_sub(1);
  return out;
}

// DCS's bytes back into every buffer that took a patched upload in this pass
// (through the context: the upload hooks forward it unchanged).
void RestoreAll() {
  State& s = g_s;
  const int n = g_restN;
  g_restN = 0;
  if (!s.ctx) return;
  for (int i = 0; i < n; ++i) {
    Restore& e = g_rest[i];
    if (e.us1 && s.ctx1)
      s.ctx1->UpdateSubresource1(e.res, e.sub, e.hasBox ? &e.box : nullptr, e.bytes, 0, 0, 0);
    else
      s.ctx->UpdateSubresource(e.res, e.sub, e.hasBox ? &e.box : nullptr, e.bytes, 0, 0);
    g_st.restored++;
  }
}

// ---------------------------------------------------------------------------
// gbpass observer: the G-buffer executions (render thread)
// ---------------------------------------------------------------------------

// Trace (CockpitTrueIpdLog): the cockpit G-buffer execution's items (renderable r = vector entry, item [r+0x10],
// material [item+0x10], mesh [item+0xc0], page / element [item+0xd0 / 0xd4], the 4x4 at item+0x60 that 0x15e20
// copies to the material's +0xb0) of the first 8 traced cockpit executions; logged at the execution's end with the
// view's camera (DCS's, from its cPerView), nearest first: which objects sit at the eye (a helmet display unit).
struct ItemRow {
  const void *r, *item, *mat, *mesh;
  uint32_t page, elem;
  uint8_t ck, pass;
  float m[16];
  uint32_t pf[6];  // the page struct's dwords +0x00..+0x14 (+0x14 used bytes)
  float el[32];    // the element's first 32 floats (CPU data [page+0x18] + elem * stride)
  uint32_t stride;
};
constexpr size_t kItemSnapMax = 4096;
ItemRow g_itemRows[kItemSnapMax];
size_t g_itemN = 0;
int g_itemView = -1, g_itemDumps = 0, g_fwdItemDumps = 0;
uint8_t* g_itemBase = nullptr;  // [[ctx]+0x438] of the frame's cockpit G-buffer execution (the collections' vectors)
void* g_itemRg = nullptr;       // its render graph (the collection descriptors: how many vectors there are)
void ItemSnap(void** vec, int view) {
  g_itemN = 0;
  g_itemView = view;
  auto* b = static_cast<uint8_t**>(vec[0]);
  auto* e = static_cast<uint8_t**>(vec[1]);
  for (uint8_t** p = b; p && p < e && g_itemN < kItemSnapMax; ++p) {
    ItemRow& w = g_itemRows[g_itemN];
    w = ItemRow{};
    const uint8_t* r = *p;
    if (!r || *reinterpret_cast<void* const*>(r) == static_cast<void*>(g_helmetVtbl)) continue;  // (a switch entry)
    w.r = r;
    const uint8_t* item = *reinterpret_cast<uint8_t* const*>(r + 0x10);
    w.ck = r[0x64];
    w.pass = static_cast<uint8_t>(*reinterpret_cast<const uint32_t*>(r + 0x60));
    if (item) {
      w.item = item;
      w.mat = *reinterpret_cast<void* const*>(item + 0x10);
      w.mesh = *reinterpret_cast<void* const*>(item + 0xc0);
      w.page = *reinterpret_cast<const uint32_t*>(item + 0xd0);
      w.elem = *reinterpret_cast<const uint32_t*>(item + 0xd4);
      memcpy(w.m, item + 0x60, sizeof(w.m));
      const uint8_t* const* gl = gbrec::g_env.globals;
      const uint8_t* g = gl ? *gl : nullptr;
      const uint8_t* base = g ? *reinterpret_cast<const uint8_t* const*>(g + 0x80) : nullptr;
      if (base) {
        const uint8_t* ps = base + static_cast<size_t>(w.page) * 0x30;
        memcpy(w.pf, ps, sizeof(w.pf));
        const uint8_t* data = *reinterpret_cast<const uint8_t* const*>(ps + 0x18);
        const uint32_t stride = w.pf[1];
        w.stride = stride;
        if (data && stride >= 48 && stride <= 256 && static_cast<uint64_t>(w.elem + 1) * stride <= w.pf[5])
          memcpy(w.el, data + static_cast<size_t>(w.elem) * stride, std::min<size_t>(sizeof(w.el), stride));
      }
    }
    ++g_itemN;
  }
}
void ItemLog(const ViewFrame& vf) {
  if (g_itemView < 0) return;
  ++g_itemDumps;
  std::vector<std::pair<double, size_t>> d;
  for (size_t i = 0; i < g_itemN; ++i) {
    const float* m = g_itemRows[i].el + 9;  // the sbPositions element's position
    double s = 0;
    for (int k = 0; k < 3; ++k) s += (m[k] - vf.C[k]) * (m[k] - vf.C[k]);
    d.emplace_back(std::sqrt(s), i);
  }
  std::sort(d.begin(), d.end());
  Log("cockpit ipd item trace: frame %llu view %d: %zu items; camera %.4f %.4f %.4f right %.4f %.4f %.4f fwd %.4f "
      "%.4f %.4f; nearest (by the 4x4's row 3) first:",
      static_cast<unsigned long long>(g_frame), g_itemView, g_itemN, vf.C[0], vf.C[1], vf.C[2], vf.R[0][0], vf.R[0][1],
      vf.R[0][2], vf.R[2][0], vf.R[2][1], vf.R[2][2]);
  for (size_t k = 0; k < d.size() && k < 120; ++k) {
    const ItemRow& w = g_itemRows[d[k].second];
    double rel[3];
    for (int j = 0; j < 3; ++j) rel[j] = w.el[9 + j] - vf.C[j];
    const double x = rel[0] * vf.R[0][0] + rel[1] * vf.R[0][1] + rel[2] * vf.R[0][2];
    const double y = rel[0] * vf.R[1][0] + rel[1] * vf.R[1][1] + rel[2] * vf.R[1][2];
    const double z = rel[0] * vf.R[2][0] + rel[1] * vf.R[2][1] + rel[2] * vf.R[2][2];
    Log("  #%zu dist %.4f m (right %+.4f up %+.4f fwd %+.4f) r %p ck %u pass %u item %p mat %p mesh %p page %u elem %u "
        "pagef %u %u %u %u %u %u stride %u el %.4f %.4f %.4f %.4f | %.4f %.4f %.4f %.4f | %.4f %.4f %.4f %.4f | %.4f "
        "%.4f %.4f %.4f | %.4f %.4f %.4f %.4f | %.4f %.4f %.4f %.4f",
        d[k].second, d[k].first, x, y, z, w.r, w.ck, w.pass, w.item, w.mat, w.mesh, w.page, w.elem, w.pf[0], w.pf[1],
        w.pf[2], w.pf[3], w.pf[4], w.pf[5], w.stride, w.el[0], w.el[1], w.el[2], w.el[3], w.el[4], w.el[5], w.el[6],
        w.el[7], w.el[8], w.el[9], w.el[10], w.el[11], w.el[12], w.el[13], w.el[14], w.el[15], w.el[16], w.el[17],
        w.el[18], w.el[19], w.el[20], w.el[21], w.el[22], w.el[23]);
  }
  g_itemView = -1;
}

// ---------------------------------------------------------------------------
// Helmet models: head-attached geometry that DCS anchors to one eye
// ---------------------------------------------------------------------------
// Live 2026-10-14 (AH-64D): the pilot's helmet model (//models/pilot_ah64_helmet.edm, with the IHADSS monocle in
// front of the right eye) is placed by DCS from its own right eye: with DCS's IPD at 65.5 mm its origin sat 7.8 mm
// right of the eye midpoint, at 90 mm 20.0 mm (+12.2 mm for +12.25 mm of half IPD), 81 mm behind the eyes. DCS's
// eye is the boosted one, so drawn from the true eye (option 3) the monocle sat h beside the IHADSS symbology it
// frames (the symbology is collimated, at DCS's camera): 160 px in the context view at the 13.1 mm test shift.
// Rule (R32's: an object is drawn with the camera of what it is attached to): such a model is attached to DCS's
// eye A. In reality it sits at the true eye A, so it is drawn as if moved by Dw_A = (true eye A - DCS's eye A):
// in a view of eye A that is DCS's own camera; in a view of the other eye V the camera E_V(true) - Dw_A =
// DCS's camera moved by Dw_V - Dw_A = 2 Dw_V (Dw_A = -Dw_V: the same h, opposite sides). Everything else of the
// pass keeps the true eye.
// Which items: a ModelMaterialMT item of the pass whose model path (material +0x20 -> +8, the .edm path) contains
// "helmet" (DCS's pilot helmet models: AH-64D, A-10C II, F-16C, F/A-18C, MiG-29; Bazar helmet_hmcs / helmet_hmd)
// and whose origin (its sbPositions element: 3x3 rows, then the position, floats 9..11, in the page's CPU data
// [page+0x18], stride [page+4]) lies within 0.15 m of DCS's eye midpoint (on the head, not one lying in the
// cockpit). The anchor eye: the side of the eye midpoint the origin lies on, beyond 3 mm (a helmet DCS places
// head-centred has its origin on the midpoint: drawn at the true eye like the cockpit, as before).
// How (both passes read their item vector once before their loop [V GraphicsCore 0x896e1, 0xa961c]): the vector's
// {begin, end} handed to DCS's loop point at a list of ours, the items in DCS's order with a switch entry before
// each run of helmet items and one after (objects whose vt[1] runs our code, as gb_rec.h's exec entries); the
// vector is put back at the pass end. The switch in: VS b7 (and PS b7 when it is the same buffer) bound to a
// private buffer holding DCS's bytes of the pass's struct (patched by 2 Dw / 2 Dp in the other eye's views),
// with DCS's *SetConstantBuffers1 offsets; the switch out binds DCS's buffers back. DCS does not rebind b7 per
// draw (gb_rec.h's lists bind b6-b8 once per list). Lighting stays the true eye's for these pixels (positions
// reconstructed ~h from the helmet's surface: shading only). A switch that cannot be made in a moved view (no
// struct seen, another buffer bound) fails the view: the next frame goes unpatched, never a mixed state.
// [Perception] CockpitTrueIpdHelmet (default 1) = 0: no switch (helmet models at the true eye as before).
std::atomic<bool> g_cfgHelmet{true};
constexpr double kHelmetHeadDist = 0.15;  // m from the eye midpoint
constexpr double kHelmetSide = 0.003;     // m off the midpoint to count as anchored to that side's eye
// DCS's eye cameras (views 0 / 1, the latest frame's), for the head frame at a pass begin.
double g_dcsEye[2][3] = {};
double g_dcsUp[3] = {}, g_dcsFwd[3] = {};  // (trace) view 0's up / forward axes
int g_helmetTraceLines = 0;
uint64_t g_dcsEyeFrame[2] = {~0ull, ~0ull};
// Model-path verdicts per material (render thread): 1 helmet, 0 not, -1 unreadable. Open addressing on the
// material pointer. Each entry keeps the material's name pointer (+0x20), compared at every hit: a material freed
// and another allocated at its address is judged again. The table is cleared when 3/4 full, and at a mission
// event or a gap scene cut (HelmetOnMissionEvent / ocpu::SceneCuts, consumed at the next HelmetSwap), when DCS
// frees and reuses the cockpit's heap objects. Before, a linear 512-entry table, cleared only at Stop, kept stale
// verdicts across missions (a new helmet material on a freed "not helmet" address was drawn from the true eye),
// and once full re-parsed every unlisted material's path at each lookup (linear scan of 512 plus the parse).
struct HelmetMatEntry {
  const void* mat = nullptr;
  const void* name = nullptr;  // [mat+0x20] when judged
  int8_t verdict = -1;
};
constexpr int kHelmetMats = 2048;  // power of two
HelmetMatEntry g_helmetMats[kHelmetMats];
int g_helmetMatN = 0;
std::atomic<int> g_helmetResetReq{0};  // a mission event (Lua thread)
uint64_t g_helmetCuts = 0;             // ocpu::SceneCuts() at the last reset check (render thread)
std::atomic<uint64_t> g_helmetMatResets{0};
std::atomic<uint64_t> g_helmetItems{0}, g_helmetSwitches{0}, g_helmetRefused{0}, g_helmetLists{0},
    g_helmetHeadCentred{0}, g_helmetFar{0};
std::atomic<int> g_helmetLastAnchor{-1};
struct LoopEntry {  // what DCS's item loop calls: (*p)->vt[1](p, data)
  void** vtbl;
  int anchor;  // 0 left eye, 1 right eye
  int in;      // 1 the switch in, 0 the switch out
  // Zeros where a SceneRenderable holds its item (+0x10), pass (+0x60) and flags (+0x64): a reader of the vector
  // that does not check the vtable first finds no item (every reader of ours checks it: gb_batch, gb_rec, the
  // trace; live 2026-10-14 the trace did not, read a switch entry as a renderable and faulted).
  uint8_t pad[0xf0] = {};
};
uint64_t __fastcall HelmetVt1(void* self, void* data);
uint64_t __fastcall HelmetNop(void*, void*) { return 0; }
LoopEntry g_helmetIn[2], g_helmetOut[2];
constexpr size_t kHelmetListMax = 8192;
void* g_helmetList[kHelmetListMax];
// The swapped vector of the open pass (render thread).
struct HelmetSwapState {
  void** vec = nullptr;
  void* b0 = nullptr;
  void* e0 = nullptr;
};
HelmetSwapState t_helmetSwap;
// The switch state inside the open pass: the pass's buffer holds the helmet's camera.
struct HelmetBind {
  bool active = false;
  ID3D11Resource* res = nullptr;  // identity
};
HelmetBind t_helmetBind;

void ClearHelmetMats() {
  for (HelmetMatEntry& e : g_helmetMats) e = HelmetMatEntry{};
  g_helmetMatN = 0;
}
void ReleaseHelmet() { ClearHelmetMats(); }
void InitHelmetVtbl() {
  for (auto*& p : g_helmetVtbl) p = reinterpret_cast<void*>(&HelmetNop);
  g_helmetVtbl[1] = reinterpret_cast<void*>(&HelmetVt1);
  for (int a = 0; a < 2; ++a) {
    g_helmetIn[a] = LoopEntry{g_helmetVtbl, a, 1};
    g_helmetOut[a] = LoopEntry{g_helmetVtbl, a, 0};
  }
}
// DCS's camera of view 0 / 1 (OnUploadImpl, at the view's first cockpit upload of a frame).
void NoteDcsEye(int t, const double C[3]) {
  if (t < 0 || t > 1) return;
  memcpy(g_dcsEye[t], C, sizeof(g_dcsEye[t]));
  g_dcsEyeFrame[t] = g_frame;
  if (t == 0) {
    const ViewFrame& vf = g_f.v[0];
    for (int k = 0; k < 3; ++k) g_dcsUp[k] = vf.R[1][k], g_dcsFwd[k] = vf.R[2][k];
  }
}
bool HelmetNameRaw(const uint8_t* mat, bool* helmet) {
  const uint8_t* np = *reinterpret_cast<const uint8_t* const*>(mat + 0x20);
  if (!np) return false;
  char s[128];
  memcpy(s, np + 8, sizeof(s) - 1);
  s[sizeof(s) - 1] = 0;
  size_t n = 0;
  while (n < sizeof(s) - 1 && s[n] >= 0x20 && s[n] < 0x7f) ++n;
  if (n < 5) return false;  // not a path
  s[n] = 0;
  for (size_t i = 0; i < n; ++i) s[i] = static_cast<char>(tolower(static_cast<unsigned char>(s[i])));
  *helmet = strstr(s, "helmet") != nullptr;
  return true;
}
inline size_t HelmetMatHash(const void* mat) {
  return static_cast<size_t>((reinterpret_cast<uintptr_t>(mat) >> 4) * 0x9E3779B97F4A7C15ull >> 40) &
         (kHelmetMats - 1);
}
HelmetMatEntry* HelmetEntry(const uint8_t* mat) {
  const void* name = nullptr;
  __try {
    name = *reinterpret_cast<const void* const*>(mat + 0x20);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    name = nullptr;
  }
  if (g_helmetMatN >= kHelmetMats * 3 / 4) {
    ClearHelmetMats();
    g_helmetMatResets.fetch_add(1, std::memory_order_relaxed);
  }
  size_t i = HelmetMatHash(mat);
  while (g_helmetMats[i].mat && g_helmetMats[i].mat != mat) i = (i + 1) & (kHelmetMats - 1);
  HelmetMatEntry* e = &g_helmetMats[i];
  if (e->mat == mat && e->name == name) return e;
  bool helmet = false;
  int v = -1;
  __try {
    if (HelmetNameRaw(mat, &helmet)) v = helmet ? 1 : 0;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    v = -1;
  }
  if (!e->mat) ++g_helmetMatN;
  *e = HelmetMatEntry{mat, name, static_cast<int8_t>(v)};
  return e;
}
// The world origin of an item (its sbPositions element in the page's CPU data), false when not readable.
bool ItemOriginRaw(const uint8_t* item, double o[3]) {
  const uint8_t* const* gl = gbrec::g_env.globals;
  const uint8_t* g = gl ? *gl : nullptr;
  const uint8_t* base = g ? *reinterpret_cast<const uint8_t* const*>(g + 0x80) : nullptr;
  if (!base) return false;
  const uint32_t page = *reinterpret_cast<const uint32_t*>(item + 0xd0);
  const uint32_t elem = *reinterpret_cast<const uint32_t*>(item + 0xd4);
  const uint8_t* ps = base + static_cast<size_t>(page) * 0x30;
  const uint32_t stride = *reinterpret_cast<const uint32_t*>(ps + 4);
  const uint32_t used = *reinterpret_cast<const uint32_t*>(ps + 0x14);
  const uint8_t* data = *reinterpret_cast<const uint8_t* const*>(ps + 0x18);
  if (!data || stride < 48 || stride > 256 || static_cast<uint64_t>(elem + 1) * stride > used) return false;
  float f[12];
  memcpy(f, data + static_cast<size_t>(elem) * stride, sizeof(f));
  for (int r = 0; r < 3; ++r) {  // a rotation (any scale between 0.5 and 2): the layout the trace showed
    const double n = std::sqrt(double(f[r * 3]) * f[r * 3] + double(f[r * 3 + 1]) * f[r * 3 + 1] +
                               double(f[r * 3 + 2]) * f[r * 3 + 2]);
    if (!(n > 0.5 && n < 2.0)) return false;
  }
  for (int k = 0; k < 3; ++k) {
    o[k] = f[9 + k];
    if (!std::isfinite(o[k])) return false;
  }
  return true;
}
// A helmet model instance (its items share the texture-entry vector [item+0x18], one per model instance, gb_batch.h):
// the anchor eye is the instance's, decided from its item nearest DCS's eye midpoint (on the head within 0.15 m:
// the AH-64D helmet's root 81 mm behind the eyes; its glass and mount items have their own node origins, up to
// 17 cm away, live 2026-10-14, and must follow the same camera), with hysteresis (anchored from 3 mm off the
// midpoint, kept down to 2 mm), and kept for the frame so the G-buffer and the forward pass agree.
struct HelmetInst {
  const void* key = nullptr;
  uint64_t frame = ~0ull;
  int8_t side = -1;
};
constexpr int kHelmetInsts = 32;
HelmetInst g_helmetInst[kHelmetInsts];
int g_helmetInstNext = 0;
HelmetInst* HelmetInstOf(const void* key) {
  for (HelmetInst& h : g_helmetInst)
    if (h.key == key) return &h;
  HelmetInst& h = g_helmetInst[g_helmetInstNext];
  g_helmetInstNext = (g_helmetInstNext + 1) % kHelmetInsts;
  h = HelmetInst{key, ~0ull, -1};
  return &h;
}
// An entry's helmet item: its instance key and origin; false when it is not a readable helmet-model item.
bool HelmetItemRaw(const uint8_t* r, const void** key, double o[3]) {
  if (!r || *reinterpret_cast<void* const*>(r) == static_cast<void*>(g_helmetVtbl)) return false;
  const uint8_t* item = *reinterpret_cast<const uint8_t* const*>(r + 0x10);
  if (!item) return false;
  const uint8_t* mat = *reinterpret_cast<const uint8_t* const*>(item + 0x10);
  HelmetMatEntry* me = mat ? HelmetEntry(mat) : nullptr;
  if (!me || me->verdict != 1) return false;
  *key = *reinterpret_cast<void* const*>(item + 0x18);
  return *key && ItemOriginRaw(item, o);
}
// At a moved cockpit pass's begin: the vector handed to DCS's loop with the switch entries, when it holds helmet
// items. False: left as it is.
bool HelmetSwapRaw(void** vec) {
  auto** b = static_cast<uint8_t**>(vec[0]);
  auto** e = static_cast<uint8_t**>(vec[1]);
  if (!b || e <= b || static_cast<size_t>(e - b) * 3 > kHelmetListMax) return false;
  // DCS's eyes of this frame or the one before (a stale pair, e.g. across a load, is no head frame).
  for (uint64_t f : g_dcsEyeFrame)
    if (f == ~0ull || f + 2 < g_frame) return false;
  double mid[3], ax[3], ipd = 0;
  for (int k = 0; k < 3; ++k) {
    mid[k] = 0.5 * (g_dcsEye[0][k] + g_dcsEye[1][k]);
    ax[k] = g_dcsEye[1][k] - g_dcsEye[0][k];
    ipd += ax[k] * ax[k];
  }
  ipd = std::sqrt(ipd);
  if (!(ipd > 0.03 && ipd < 0.2)) return false;
  for (double& a : ax) a /= ipd;
  // Pass 1: each helmet instance's item nearest the eye midpoint.
  struct Near {
    const void* key;
    double dist, lat;
  };
  Near nears[kHelmetInsts];
  int nn = 0;
  for (uint8_t** p = b; p < e; ++p) {
    const void* key = nullptr;
    double o[3];
    if (!HelmetItemRaw(*p, &key, o)) continue;
    double d2 = 0, lat = 0;
    for (int k = 0; k < 3; ++k) {
      d2 += (o[k] - mid[k]) * (o[k] - mid[k]);
      lat += (o[k] - mid[k]) * ax[k];
    }
    const double d = std::sqrt(d2);
    int f = -1;
    for (int k = 0; k < nn; ++k)
      if (nears[k].key == key) f = k;
    if (f < 0 && nn < kHelmetInsts) nears[f = nn++] = Near{key, d, lat};
    if (f >= 0 && d < nears[f].dist) nears[f] = Near{key, d, lat};
    if (g_logLeft.load(std::memory_order_relaxed) > 0 && g_helmetTraceLines < 400) {
      ++g_helmetTraceLines;
      double up = 0, fwd = 0;
      for (int k = 0; k < 3; ++k) up += (o[k] - mid[k]) * g_dcsUp[k], fwd += (o[k] - mid[k]) * g_dcsFwd[k];
      TraceLine("helmet item: view %d %s, instance %p: origin %.4f m from the eye midpoint (right %+.4f up %+.4f "
                "fwd %+.4f)", g_s.hmdOrd, t_cls == kClsFwd ? "forward" : "G-buffer", key, d, lat, up, fwd);
    }
  }
  if (!nn) return false;
  // Each instance's side (once per frame, with hysteresis).
  int8_t sides[kHelmetInsts];
  for (int k = 0; k < nn; ++k) {
    HelmetInst* h = HelmetInstOf(nears[k].key);
    if (h->frame != g_frame) {
      h->frame = g_frame;
      const double lat = nears[k].lat;
      int side = h->side;
      if (nears[k].dist > kHelmetHeadDist) {
        side = -1;  // not on this pilot's head (another crew member's helmet)
        g_helmetFar++;
      } else if (side >= 0 ? (std::fabs(lat) < kHelmetSide - 0.001 || (lat > 0) != (side == 1))
                           : std::fabs(lat) >= kHelmetSide) {
        side = std::fabs(lat) >= kHelmetSide ? (lat > 0 ? 1 : 0) : -1;
      }
      if (side < 0 && nears[k].dist <= kHelmetHeadDist) g_helmetHeadCentred++;
      h->side = static_cast<int8_t>(side);
      if (g_logLeft.load(std::memory_order_relaxed) > 0)
        TraceLine("helmet instance %p: nearest item %.4f m from the eye midpoint, %+.4f m to the right: %s",
                  nears[k].key, nears[k].dist, lat, side == 1 ? "right eye" : side == 0 ? "left eye" :
                  nears[k].dist > kHelmetHeadDist ? "not on the head" : "head-centred");
    }
    sides[k] = h->side;
  }
  // Pass 2: the list, a switch entry around each run of one anchor.
  size_t n = 0;
  int open = -1;  // the anchor of the open run
  uint64_t items = 0;
  for (uint8_t** p = b; p < e; ++p) {
    int a = -1;
    const void* key = nullptr;
    double o[3];
    if (HelmetItemRaw(*p, &key, o))
      for (int k = 0; k < nn; ++k)
        if (nears[k].key == key) a = sides[k];
    if (a != open) {
      if (open >= 0) g_helmetList[n++] = &g_helmetOut[open];
      if (a >= 0) g_helmetList[n++] = &g_helmetIn[a];
      open = a;
    }
    if (a >= 0) {
      ++items;
      g_helmetLastAnchor = a;
    }
    g_helmetList[n++] = *p;
  }
  if (open >= 0) g_helmetList[n++] = &g_helmetOut[open];
  if (!items) return false;
  g_helmetItems += items;
  g_helmetLists++;
  t_helmetSwap.vec = vec;
  t_helmetSwap.b0 = vec[0];
  t_helmetSwap.e0 = vec[1];
  vec[0] = g_helmetList;
  vec[1] = g_helmetList + n;
  return true;
}
// main.cpp's OnMissionEvent (Lua thread): the cockpit's materials and model instances are about to be freed.
inline void HelmetOnMissionEvent() { g_helmetResetReq.store(1, std::memory_order_release); }
// At a pass begin (render thread): a mission event or a gap scene cut since the last check clears the material
// verdicts and the instance sides (keyed by heap pointers DCS may now reuse).
void HelmetConsumeReset() {
  const uint64_t cuts = ocpu::SceneCuts();
  if (!g_helmetResetReq.load(std::memory_order_relaxed) && cuts == g_helmetCuts) return;
  g_helmetResetReq.store(0, std::memory_order_relaxed);
  g_helmetCuts = cuts;
  ClearHelmetMats();
  for (HelmetInst& h : g_helmetInst) h = HelmetInst{};
  g_helmetMatResets.fetch_add(1, std::memory_order_relaxed);
}
void HelmetSwap(void** vec) {
  if (!g_cfgHelmet.load(std::memory_order_relaxed) || !vec || t_helmetSwap.vec) return;
  HelmetConsumeReset();
  __try {
    HelmetSwapRaw(vec);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    if (t_helmetSwap.vec) {  // (a fault after the write cannot happen: the write is the last step)
      t_helmetSwap.vec[0] = t_helmetSwap.b0;
      t_helmetSwap.vec[1] = t_helmetSwap.e0;
    }
    t_helmetSwap = HelmetSwapState{};
  }
}
// The switch out: the pass's buffer back to the bytes we sent (the true eye).
void HelmetUnbind() {
  HelmetBind& hb = t_helmetBind;
  if (!hb.active) return;
  hb.active = false;
  const HelmetSrc& hs = t_helmetSrc;
  if (hs.valid && hs.res == hb.res) RawUpload(hs.res, hs.hasBox ? &hs.box : nullptr, hs.sent, hs.us1);
}
// At the pass end: DCS's vector back, our bytes back (the switch out always runs; a backstop).
void HelmetPassEnd() {
  HelmetUnbind();
  if (t_helmetSwap.vec) {
    __try {
      t_helmetSwap.vec[0] = t_helmetSwap.b0;
      t_helmetSwap.vec[1] = t_helmetSwap.e0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    t_helmetSwap = HelmetSwapState{};
  }
  t_helmetSrc.valid = false;
}
// The switch in: the pass's own cPerView buffer takes DCS's bytes (the helmet's eye is the view's) or DCS's
// camera moved by 2 Dw (the other eye's), uploaded the way the pass's upload was; DCS's draws read that buffer
// whatever it rebinds (a forward pass rebinds its constant buffers per draw: live 2026-10-14, a private b7 bound
// here was not used by the AH-64D's monocle glass). False when it cannot be made in a moved view.
bool HelmetSwitchIn(int anchor) {
  const int t = t_view;
  if ((t_cls != kClsGb && t_cls != kClsFwd) || t < 0 || t > 3 || t_capOnly) return true;
  Frame& f = g_f;
  ViewFrame& vf = f.v[t];
  if (f.off || f.observe || !vf.patched || vf.failed || !NonZero(vf.dw)) return true;  // the pass is DCS's
  const HelmetSrc& hs = t_helmetSrc;
  if (!hs.valid || hs.frame != g_frame || hs.view != t || hs.at + kCbBytes > hs.len) return false;
  HelmetUnbind();
  memcpy(g_scratch, hs.dcs, hs.len);
  if (anchor != (t & 1)) {  // the other eye's helmet model: DCS's camera moved by 2 Dw (2 Dp for the gPrev fields)
    const double dw2[3] = {2 * vf.dw[0], 2 * vf.dw[1], 2 * vf.dw[2]};
    const double dp2[3] = {2 * vf.dp[0], 2 * vf.dp[1], 2 * vf.dp[2]};
    PatchStruct(g_scratch + hs.at, dw2, dp2);
  }
  RawUpload(hs.res, hs.hasBox ? &hs.box : nullptr, g_scratch, hs.us1);
  t_helmetBind.active = true;
  t_helmetBind.res = hs.res;
  ++vf.helmetN;
  g_helmetSwitches++;
  return true;
}
uint64_t __fastcall HelmetVt1(void* self, void* data) {
  (void)data;
  const LoopEntry* le = static_cast<const LoopEntry*>(self);
  g_busy.fetch_add(1);
  __try {
    if (!le->in) {
      HelmetUnbind();
    } else if (!HelmetSwitchIn(le->anchor)) {
      g_helmetRefused++;
      const int t = t_view;
      if (t >= 0 && t <= 3) {  // the helmet would be drawn at the true eye: the view fails (next frame unpatched)
        ViewFrame& vf = g_f.v[t];
        ++vf.helmetRefused;
        vf.failed = true;
        g_f.anyFailed = true;
        if (g_f.anyPatched) g_offNext = true;
      }
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    g_fault = true;
  }
  g_busy.fetch_sub(1);
  return 0;
}

// ---------------------------------------------------------------------------
// Own aircraft's exterior model at the true eye ([Perception] CockpitTrueIpdOwnship, default 0; stage 1)
// ---------------------------------------------------------------------------
// R31 R2 / R3: DCS draws the own aircraft's exterior model in the world passes, so with the cockpit at the true eye
// it stays at the boosted one: the AH-64D's CPG (its body is part of the exterior model AH-64D_bl2, argument 472)
// and the front seat's back float about 34 cm nearer than the cockpit around them at x1.4 (1.0-1.45 m away: 62-90
// arcmin of stereo error), and own wings / nose / probe seams shift at the sill.
// Stage 1: the exterior model instance is locked once (a mission's own aircraft), and every item of that instance
// is drawn from the true eye (the view's cockpit Dw / Dp) in the HMD views' world G-buffer (sm 0), decal (sm 3)
// and transparent (TransparentPass) passes: the reverse of the helmet switch. Everything else keeps DCS's camera.
//  - Lock (view 0's world G-buffer execution, render thread): the NGModel SceneRenderable item origins (sbPositions)
//    nearest DCS's eye midpoint, per model instance ([item+0x18]); models whose path names "cockpit" or "helmet"
//    are not candidates. The instance with the nearest origin within kOwnLockDist, at least kOwnLockMargin nearer
//    than any other instance, the same for kOwnLockFrames consecutive frames, is locked (logged with its path).
//    It stays locked until it has been absent from view 0's world vector for kOwnLostFrames frames (a long time
//    without it in view) or the payload stops: a fixed selection, never toggled per frame. A mission or slot event
//    (main.cpp's OnMissionEvent: simulation stop, mission load, local player slot change) unlocks it at once (0.5.9
//    kept the old aircraft's instance, a pointer DCS frees with its mission, locked for the 900 frames after each
//    load: live 2026-10-14, the new aircraft locked ~10 s late), and after a gap or jump scene cut it must be seen
//    again within kOwnRecheckFrames lock steps (kOwnRecheckWindow), else it is unlocked; a turn cut (a head turn
//    over a frame hitch) changes nothing. Lock steps run only in frames whose view 0 drew its cockpit.
//  - Switch: a pass of a moved view (Dw != 0, or the previous frame's ownship Dp: the tail) hands DCS's loop a list
//    with a switch entry before and after each run of the locked instance's items (as the helmet switch); in: the
//    pass's own cPerView buffer gets DCS's bytes of this pass patched by the view's Dw / Dp (RawUpload, past our
//    hooks), out: DCS's bytes again. The pass's uploads are captured (DCS's bytes, forwarded unchanged); an upload
//    while a switch is in forwards the patched copy. A switch with no captured upload is counted (ownNoSrc; the
//    items keep DCS's camera in that pass) and must stay 0 live.
//  - The G-buffer recorder (gb_rec.h) steps aside for exactly the executions whose vector was swapped
//    (gbrec::t_ownAside: the slot's pass drawn by DCS stock, its slot kept alive), and no recorder job is started
//    while any execution stepped aside within the last kOwnAsideHold frames (gbrec::g_ownAsideHint).
//  - Lighting keeps the world's camera for these pixels (shading positions off by about h).
// 0 off, 1 stage 1 (the recorder steps aside), 2 stage 2 (the recorder keeps the execution: gb_rec.h's g_ownKey).
std::atomic<int> g_cfgOwnship{0};
void SetOwnship(int mode) { g_cfgOwnship = mode; }  // main.cpp each second; the render thread unlocks at a frame end
constexpr double kOwnLockDist = 6.0;     // m from DCS's eye midpoint to the instance's nearest item origin
constexpr double kOwnLockMargin = 1.5;   // m nearer than any other instance
constexpr int kOwnLockFrames = 60;
constexpr int kOwnLostFrames = 900;
constexpr int kOwnRecheckFrames = 10;   // after a gap / jump scene cut: absent this many lock steps in a row: unlocked
constexpr int kOwnRecheckWindow = 300;  // lock steps after the cut that apply it (until the instance is seen)
constexpr int kOwnAsideHold = 90;
struct OwnLock {
  const void* key = nullptr;  // the locked instance
  char path[128] = {};
  double dist = 0;
  const void* cand = nullptr;  // the candidate and its consecutive frames
  int candN = 0;
  int absent = 0;  // frames view 0's world vector did not hold the locked instance
};
OwnLock g_own;
std::atomic<bool> g_ownLocked{false};
// Mission / slot events (any thread: main.cpp's OnMissionEvent on the Lua thread), consumed by the render thread.
std::atomic<int> g_ownEventReq{0};
uint64_t g_ownCutsSeen = 0;  // scene_cut.h's gap + jump cuts at the last check (render thread)
int g_ownRecheck = 0;        // lock steps left in which a missing instance is unlocked after kOwnRecheckFrames
std::atomic<uint64_t> g_ownEventUnlocks{0}, g_ownCutUnlocks{0};
inline uint64_t OwnSceneCuts() {
  return scenecut::g_cutsByReason[scenecut::kGap].load(std::memory_order_relaxed) +
         scenecut::g_cutsByReason[scenecut::kJump].load(std::memory_order_relaxed);
}
// main.cpp's OnMissionEvent: the instance is unlocked at the render thread's next lock step or frame end.
inline void OwnOnMissionEvent() { g_ownEventReq.store(1, std::memory_order_release); }
uint64_t g_ownLockFrame = ~0ull;  // the frame whose lock step ran
std::atomic<uint64_t> g_ownFromCockpit{0};
std::atomic<uint64_t> g_ownLocks{0}, g_ownUnlocks{0}, g_ownPasses{0}, g_ownItems{0}, g_ownSwitches{0}, g_ownNoSrc{0},
    g_ownFwdPasses{0}, g_ownReupload{0}, g_ownTail{0};
bool g_ownPrev[4] = {}, g_ownCur[4] = {};  // the view's ownship moved (locked, Dw / Dp) in the previous / this frame
int g_ownAsideAge = 1 << 20;               // frames since an execution last stepped aside
void* g_ownVtbl[8];
LoopEntry g_ownIn, g_ownOut;
constexpr size_t kOwnListMax = 1 << 15;
void* g_ownList[kOwnListMax];
struct OwnSwapState {
  void** vec = nullptr;
  void* b0 = nullptr;
  void* e0 = nullptr;
};
OwnSwapState t_ownSwap;
struct OwnSrc {
  bool valid = false, hasBox = false, us1 = false, bound = false;
  ID3D11Resource* res = nullptr;
  UINT at = 0, len = 0;
  D3D11_BOX box{};
  uint8_t dcs[kMaxSrc] = {};
  uint8_t sent[kMaxSrc] = {};
};
OwnSrc t_ownSrc;
double t_ownDw[3] = {}, t_ownDp[3] = {};  // the open pass's ownship shift
// The open pass moves the ownship (stage 1 or 2): only then do the switch entries switch (a stage-2 job's list may
// carry them in a pass that has nothing to move, e.g. Depth boost at scale 1).
thread_local bool t_ownArmed = false;
// gb_rec.h's verify mask run (stage 2): the ownship's camera moved 10 km aside, so the ownship leaves the view and
// every texel it covers at the true eye differs between that run and A (its texels: the mask).
thread_local bool t_ownAway = false;
uint8_t g_ownAwayBytes[kMaxSrc];
std::atomic<uint64_t> g_own2Passes{0}, g_own2Unused{0}, g_ownSwitchIdle{0}, g_ownSwRetarget{0};

const void* g_ownSrVtOverride = nullptr;  // tests: the fake SceneRenderable vtable (no NGModel.dll)
// R63 U5: the SceneRenderable vtable of the NGModel image at `ng`, by RTTI: the recorded RVA when its locator names
// model::SceneRenderable (the analysed build), else the one vtable with that RTTI (a rebuild that moved .rdata).
// nullptr when neither: ownship matching stays off. *moved: found away from `recorded`.
constexpr char kSrRtti[] = ".?AVSceneRenderable@model@@";
const void* ResolveSrVtbl(const uint8_t* ng, uint32_t recorded, bool* moved) {
  *moved = false;
  if (!ng) return nullptr;
  auto* const* at = reinterpret_cast<void* const*>(ng + recorded);
  if (sigscan::InImage(ng, at, 8)) {
    const char* n = sigscan::RttiName(ng, at);
    if (n && strcmp(n, kSrRtti) == 0) return at;
  }
  void** v = sigscan::FindVtable(ng, kSrRtti);
  *moved = v != nullptr;
  return v;
}
std::atomic<const uint8_t*> g_srNg{nullptr};
std::atomic<const void*> g_srVt{nullptr};
const void* SrVtbl() {
  if (g_ownSrVtOverride) return g_ownSrVtOverride;
  const auto* ng = reinterpret_cast<const uint8_t*>(GetModuleHandleW(L"NGModel.dll"));
  if (!ng) return nullptr;
  if (g_srNg.load(std::memory_order_acquire) == ng) return g_srVt.load(std::memory_order_relaxed);
  bool moved = false;
  const void* vt = ResolveSrVtbl(ng, gbbatch::kSrVtbl, &moved);
  g_srVt.store(vt, std::memory_order_relaxed);
  if (g_srNg.exchange(ng, std::memory_order_acq_rel) != ng) {
    if (!vt)
      Log("cockpit ipd: ownship matching off (SceneRenderable vtable not in this NGModel build)");
    else if (moved)
      Log("cockpit ipd: ownship: SceneRenderable vtable at NGModel+0x%llx (recorded 0x%x; found by RTTI)",
          static_cast<unsigned long long>(static_cast<const uint8_t*>(vt) - ng), gbbatch::kSrVtbl);
  }
  return vt;
}
inline bool OwnPathExcluded(const char* p) {  // (lower case)
  return strstr(p, "cockpit") != nullptr || strstr(p, "helmet") != nullptr;
}
bool ModelPathRaw(const uint8_t* mat, char* out, size_t n) {
  const uint8_t* np = *reinterpret_cast<const uint8_t* const*>(mat + 0x20);
  if (!np || n < 8) return false;
  memcpy(out, np + 8, n - 1);
  out[n - 1] = 0;
  size_t k = 0;
  while (k < n - 1 && out[k] >= 0x20 && out[k] < 0x7f) ++k;
  out[k] = 0;
  for (size_t i = 0; i < k; ++i) out[i] = static_cast<char>(tolower(static_cast<unsigned char>(out[i])));
  return k >= 5;
}
uint64_t __fastcall OwnVt1(void* self, void* data);
bool OwnSwapForRec(void** vec);
void InitOwnVtbl() {
  for (auto*& p : g_ownVtbl) p = reinterpret_cast<void*>(&HelmetNop);
  g_ownVtbl[1] = reinterpret_cast<void*>(&OwnVt1);
  g_ownIn = LoopEntry{g_ownVtbl, 0, 1};
  g_ownOut = LoopEntry{g_ownVtbl, 0, 0};
  gbrec::g_ownInE = &g_ownIn;
  gbrec::g_ownOutE = &g_ownOut;
  gbrec::g_ownSwapFn = &OwnSwapForRec;
  gbrec::g_ownAwayFn = [](bool on) { t_ownAway = on; };
}
inline bool IsSwitchEntry(const uint8_t* r) {
  if (!r) return false;
  void* const vt = *reinterpret_cast<void* const*>(r);
  return vt == static_cast<void*>(g_helmetVtbl) || vt == static_cast<void*>(g_ownVtbl);
}
void ResetOwnship() {
  g_own = OwnLock{};
  g_ownLocked = false;
  g_ownEventReq = 0;
  g_ownCutsSeen = OwnSceneCuts();
  g_ownRecheck = 0;
  g_ownLockFrame = ~0ull;
  for (int t = 0; t < 4; ++t) g_ownPrev[t] = g_ownCur[t] = false;
  g_ownAsideAge = 1 << 20;
  gbrec::g_ownAsideHint = false;
  gbrec::t_ownAside = false;
  gbrec::g_ownKey = nullptr;
  gbrec::t_own2 = false;
  t_ownArmed = false;
  t_ownSwap = OwnSwapState{};
  t_ownSrc.valid = t_ownSrc.bound = false;
  InitOwnVtbl();
}

// Lock step (view 0's world G-buffer vector, once per frame). Pure in its inputs except the globals it updates.
void OwnLockStepRaw(void** vec) {
  auto** b = static_cast<uint8_t**>(vec[0]);
  auto** e = static_cast<uint8_t**>(vec[1]);
  if (!b || e < b) return;
  const void* srVt = SrVtbl();
  if (!srVt) return;
  double mid[3];
  for (int k = 0; k < 3; ++k) mid[k] = 0.5 * (g_dcsEye[0][k] + g_dcsEye[1][k]);
  if (g_own.key) {  // locked: only whether it is still there
    bool seen = false;
    for (uint8_t** p = b; p < e && !seen; ++p) {
      const uint8_t* r = *p;
      if (!r || IsSwitchEntry(r) || *reinterpret_cast<void* const*>(r) != srVt) continue;
      const uint8_t* item = *reinterpret_cast<const uint8_t* const*>(r + 0x10);
      seen = item && *reinterpret_cast<void* const*>(item + 0x18) == g_own.key;
    }
    g_own.absent = seen ? 0 : g_own.absent + 1;
    const bool recheck = g_ownRecheck > 0;
    if (seen) {
      g_ownRecheck = 0;  // seen again after the cut: the same aircraft
    } else if (recheck) {
      --g_ownRecheck;
    }
    const int limit = recheck ? kOwnRecheckFrames : kOwnLostFrames;
    if (g_own.absent >= limit) {
      Log("cockpit ipd: ownship: instance %p (%s) absent for %d frames%s; unlocked", g_own.key, g_own.path, limit,
          recheck ? " after a scene cut" : "");
      g_own = OwnLock{};
      g_ownLocked = false;
      g_ownRecheck = 0;
      g_ownUnlocks++;
      if (recheck) g_ownCutUnlocks++;
    }
    return;
  }
  // Nearest item origin per instance.
  struct Near {
    const void* key;
    double d;
    const uint8_t* mat;
  };
  Near best{nullptr, 1e9, nullptr}, second{nullptr, 1e9, nullptr};
  const void* lastMat = nullptr;
  bool lastOk = false;
  for (uint8_t** p = b; p < e; ++p) {
    const uint8_t* r = *p;
    if (!r || IsSwitchEntry(r) || *reinterpret_cast<void* const*>(r) != srVt) continue;
    const uint8_t* item = *reinterpret_cast<const uint8_t* const*>(r + 0x10);
    if (!item) continue;
    const uint8_t* mat = *reinterpret_cast<const uint8_t* const*>(item + 0x10);
    if (!mat) continue;
    if (mat != lastMat) {
      char path[128];
      lastMat = mat;
      lastOk = ModelPathRaw(mat, path, sizeof(path)) && !OwnPathExcluded(path);
    }
    if (!lastOk) continue;
    double o[3];
    if (!ItemOriginRaw(item, o)) continue;
    double d2 = 0;
    for (int k = 0; k < 3; ++k) d2 += (o[k] - mid[k]) * (o[k] - mid[k]);
    const double d = std::sqrt(d2);
    const void* key = *reinterpret_cast<void* const*>(item + 0x18);
    if (!key) continue;
    if (key == best.key) {
      if (d < best.d) best.d = d;
    } else if (d < best.d) {
      if (best.key) second = best;
      best = Near{key, d, mat};
    } else if (key == second.key) {
      if (d < second.d) second.d = d;
    } else if (d < second.d) {
      second = Near{key, d, mat};
    }
  }
  const bool ok = best.key && best.d <= kOwnLockDist && second.d >= best.d + kOwnLockMargin;
  if (!ok) {
    g_own.cand = nullptr;
    g_own.candN = 0;
    return;
  }
  if (best.key != g_own.cand) {
    g_own.cand = best.key;
    g_own.candN = 0;
  }
  if (++g_own.candN < kOwnLockFrames) return;
  g_own.key = best.key;
  g_own.dist = best.d;
  g_own.absent = 0;
  g_own.path[0] = 0;
  ModelPathRaw(best.mat, g_own.path, sizeof(g_own.path));
  g_ownLocked = true;
  g_ownLocks++;
  Log("cockpit ipd: ownship: instance %p locked (%s, nearest item origin %.2f m from the eyes, the next instance's "
      "%.2f m)",
      g_own.key, g_own.path, best.d, second.d < 1e8 ? second.d : -1.0);
}
// Render thread: a pending mission / slot event unlocks at once (the key withdrawn from the recorder's jobs too);
// a new gap or jump scene cut while locked opens the recheck window.
void OwnConsumeEvents() {
  if (g_ownEventReq.exchange(0, std::memory_order_acq_rel)) {
    if (g_own.key) {
      Log("cockpit ipd: ownship: instance %p (%s) unlocked: mission or slot event", g_own.key, g_own.path);
      g_ownUnlocks++;
      g_ownEventUnlocks++;
    }
    g_own = OwnLock{};
    g_ownLocked = false;
    g_ownRecheck = 0;
    gbrec::g_ownKey.store(nullptr, std::memory_order_release);
  }
  const uint64_t cuts = OwnSceneCuts();
  if (cuts != g_ownCutsSeen) {
    g_ownCutsSeen = cuts;
    if (g_own.key) g_ownRecheck = kOwnRecheckWindow;
  }
}
// Whether this frame's passes move the locked instance. Stage 2 with the recorder on: only once the key is
// published to the recorder's jobs (OwnEndFrame), so the frame that locks (or a frame whose jobs were started for
// no instance) leaves it at DCS's camera in every view and pass alike instead of making the recorder drop its jobs
// (kOwnFbKey); the published key changes only at a frame end or an event, never between two views of a frame.
inline bool OwnMoves() {
  if (!g_ownLocked.load(std::memory_order_relaxed)) return false;
  const bool stage2 = g_cfgOwnship.load(std::memory_order_relaxed) == 2 && gbrec::Active();
  return !stage2 || (g_own.key && gbrec::g_ownKey.load(std::memory_order_acquire) == g_own.key);
}
void OwnLockStep(void** vec) {
  __try {
    OwnLockStepRaw(vec);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    g_own.cand = nullptr;
    g_own.candN = 0;
  }
}

// The view's ownship shift this frame: Dw the cockpit's, Dp the cockpit's previous-frame Dw when the ownship was
// moved last frame (else 0). False: nothing to move (DCS's camera is the ownship's).
bool OwnShift(int t, double dw[3], double dp[3]) {
  const Frame& f = g_f;
  if (t < 0 || t > 3 || f.off || f.observe) return false;
  const ViewFrame& vf = f.v[t];
  if (!vf.patched || vf.failed || !vf.dwReady) return false;
  for (int i = 0; i < 3; ++i) {
    dw[i] = vf.dw[i];
    dp[i] = g_ownPrev[t] ? vf.dp[i] : 0.0;
  }
  return NonZero(dw) || NonZero(dp);
}
// The pass's vector handed to DCS's loop with the switch entries around the locked instance's runs. False: no
// item of it here (left as it is).
bool OwnSwapRaw(void** vec) {
  auto** b = static_cast<uint8_t**>(vec[0]);
  auto** e = static_cast<uint8_t**>(vec[1]);
  if (!b || e <= b || static_cast<size_t>(e - b) * 3 > kOwnListMax) return false;
  const void* srVt = SrVtbl();
  size_t n = 0;
  bool open = false;
  uint64_t items = 0;
  for (uint8_t** p = b; p < e; ++p) {
    const uint8_t* r = *p;
    bool mine = false;
    if (r && *reinterpret_cast<void* const*>(r) == srVt) {
      const uint8_t* item = *reinterpret_cast<const uint8_t* const*>(r + 0x10);
      mine = item && *reinterpret_cast<void* const*>(item + 0x18) == g_own.key;
    }
    if (mine != open) {
      g_ownList[n++] = mine ? static_cast<void*>(&g_ownIn) : static_cast<void*>(&g_ownOut);
      open = mine;
    }
    if (mine) ++items;
    g_ownList[n++] = *p;
  }
  if (open) g_ownList[n++] = &g_ownOut;
  if (!items) return false;
  g_ownItems += items;
  t_ownSwap.vec = vec;
  t_ownSwap.b0 = vec[0];
  t_ownSwap.e0 = vec[1];
  vec[0] = g_ownList;
  vec[1] = g_ownList + n;
  return true;
}
bool OwnSwap(void** vec) {
  if (!vec || t_ownSwap.vec) return false;
  __try {
    return OwnSwapRaw(vec);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    t_ownSwap = OwnSwapState{};
    return false;
  }
}
// A pass of class kClsOwn / kClsOwnFwd begins: true when its vector was swapped (then the recorder steps aside).
bool OwnBegin(void** vec, int t) {
  t_ownSrc.valid = t_ownSrc.bound = false;
  if (!OwnShift(t, t_ownDw, t_ownDp)) return false;
  // The ownship of this view counts as moved in this frame whether or not its items are in this vector (an item
  // that appears next frame was not drawn at DCS's camera before: its Dp is the cockpit's).
  g_ownCur[t] = true;
  if (!OwnSwap(vec)) return false;
  if (!NonZero(t_ownDw)) g_ownTail++;
  t_ownArmed = true;
  return true;
}
// Stage 2 (the recorder keeps the execution): the shift and the request (gbrec::t_own2); the recorder brackets the
// ownship's runs with the switch entries in its list, or falls back to stage 1 (OwnSwapForRec) for the execution.
bool OwnBegin2(int t) {
  t_ownSrc.valid = t_ownSrc.bound = false;
  if (!OwnShift(t, t_ownDw, t_ownDp)) return false;
  g_ownCur[t] = true;
  if (!NonZero(t_ownDw)) g_ownTail++;
  t_ownArmed = true;
  gbrec::t_own2 = true;
  g_own2Passes++;
  return true;
}
// The recorder's fallback for a stage-2 execution: stage 1's vector (restored at the pass end).
bool OwnSwapForRec(void** vec) { return OwnSwap(vec); }
// An upload of a known struct in an ownship pass (DCS's bytes): kept; forwarded as DCS's, or patched while a
// switch is in (DCS re-uploaded under it).
const void* OwnUpload(ID3D11Resource* r, const D3D11_BOX* box, const uint8_t* base, UINT at, UINT len, bool us1) {
  OwnSrc& o = t_ownSrc;
  o.valid = true;
  o.res = r;
  o.at = at;
  o.len = len;
  o.hasBox = box != nullptr;
  if (box) o.box = *box;
  o.us1 = us1;
  memcpy(o.dcs, base, len);
  memcpy(o.sent, base, len);
  PatchStruct(o.sent + at, t_ownDw, t_ownDp);
  if (o.bound) {
    g_ownReupload++;
    return o.sent;
  }
  return base;
}
// No upload in this pass: the view's cockpit struct, when the buffer bound at VS b7 (at the struct's offset) is
// the one the cockpit pass uploaded it to.
bool OwnFromCockpit() {
  const int t = t_view;
  if (t < 0 || t > 3 || !g_s.ctx) return false;
  const HelmetSrc& h = g_ownViewSrc[t];
  if (!h.valid || h.frame != g_frame || h.view != t || h.at + kCbBytes > h.len) return false;
  ID3D11DeviceContext1* c1 = nullptr;
  ID3D11Buffer* b = nullptr;
  UINT first = 0, num = 0;
  if (FAILED(g_s.ctx->QueryInterface(__uuidof(ID3D11DeviceContext1), reinterpret_cast<void**>(&c1))) || !c1)
    return false;
  c1->VSGetConstantBuffers1(7, 1, &b, &first, &num);
  c1->Release();
  const UINT structAt = (h.hasBox ? h.box.left : 0) + h.at;  // the struct's byte offset in the buffer
  const bool same = b && static_cast<ID3D11Resource*>(b) == h.res && first * 16 == structAt;
  if (b) b->Release();
  if (!same) return false;
  OwnSrc& o = t_ownSrc;
  o.valid = true;
  o.res = h.res;
  o.at = h.at;
  o.len = h.len;
  o.hasBox = h.hasBox;
  o.box = h.box;
  o.us1 = h.us1;
  memcpy(o.dcs, h.dcs, h.len);
  memcpy(o.sent, h.dcs, h.len);
  PatchStruct(o.sent + o.at, t_ownDw, t_ownDp);
  g_ownFromCockpit++;
  return true;
}
void OwnSwitch(bool in) {
  OwnSrc& o = t_ownSrc;
  if (in && !t_ownArmed) {
    g_ownSwitchIdle++;
    return;
  }
  if (in) {
    if (!o.valid && !OwnFromCockpit()) {
      g_ownNoSrc++;
        return;
    }
    {  // The struct bound at VS b7 now: DCS keeps two cPerView buffers and may bind the other one without an upload
       // (it already holds these bytes: a pass run twice in one frame, the recorder's verify). Then that one is
       // switched (same size, same offset; its DCS bytes are o.dcs, since DCS did not upload).
      ID3D11DeviceContext1* c1 = nullptr;
      if (g_s.ctx && SUCCEEDED(g_s.ctx->QueryInterface(__uuidof(ID3D11DeviceContext1), reinterpret_cast<void**>(&c1))) &&
          c1) {
        ID3D11Buffer* bb = nullptr;
        UINT first = 0, num = 0;
        c1->VSGetConstantBuffers1(7, 1, &bb, &first, &num);
        c1->Release();
        const UINT structAt = (o.hasBox ? o.box.left : 0) + o.at;
        ID3D11Resource* br = bb;
        if (br && (br != o.res || first * 16 != structAt)) {
          gbrec::g_ownSwBoundDiff++;
          if (!o.hasBox && first * 16 == structAt && IsKnown(br) && KnownSize(br) == KnownSize(o.res)) {
            o.res = br;  // (the bound buffer itself; DCS binds it with this pass's camera)
            g_ownSwRetarget++;
          }
        }
        if (bb) bb->Release();
      }
    }
    if (t_ownAway) {
      memcpy(g_ownAwayBytes, o.dcs, o.len);
      const double away[3] = {1e4, 0, 0}, zero[3] = {0, 0, 0};
      PatchStruct(g_ownAwayBytes + o.at, away, zero);
      RawUpload(o.res, o.hasBox ? &o.box : nullptr, g_ownAwayBytes, o.us1);
    } else {
      RawUpload(o.res, o.hasBox ? &o.box : nullptr, o.sent, o.us1);
    }
    o.bound = true;
    g_ownSwitches++;
    gbrec::g_ownSwIn++;
  } else if (o.bound) {
    RawUpload(o.res, o.hasBox ? &o.box : nullptr, o.dcs, o.us1);
    o.bound = false;
  }
}
uint64_t __fastcall OwnVt1(void* self, void* data) {
  (void)data;
  const LoopEntry* le = static_cast<const LoopEntry*>(self);
  g_busy.fetch_add(1);
  __try {
    OwnSwitch(le->in != 0);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    g_fault = true;
  }
  g_busy.fetch_sub(1);
  return 0;
}
// At the pass end: DCS's bytes back (a backstop for the switch out), DCS's vector back.
void OwnPassEnd() {
  if (t_ownSrc.bound) OwnSwitch(false);
  t_ownSrc.valid = false;
  gbrec::t_ownAside = false;
  if (gbrec::t_own2) g_own2Unused++;  // stage 2 asked, the recorder never ran for it (the ownship at DCS's eye)
  gbrec::t_own2 = false;
  t_ownArmed = false;
  if (t_ownSwap.vec) {
    __try {
      t_ownSwap.vec[0] = t_ownSwap.b0;
      t_ownSwap.vec[1] = t_ownSwap.e0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    t_ownSwap = OwnSwapState{};
  }
}
// Frame end: which views drew their ownship moved (the next frame's Dp), the recorder's job hint.
void OwnEndFrame() {
  OwnConsumeEvents();
  if (!g_cfgOwnship.load(std::memory_order_relaxed) && (g_own.key || g_own.cand)) {  // switched off: unlocked
    if (g_own.key) g_ownUnlocks++;
    g_own = OwnLock{};
    g_ownLocked = false;
  }
  bool any = false;
  for (int t = 0; t < 4; ++t) {
    g_ownPrev[t] = g_ownCur[t];
    any = any || g_ownCur[t];
    g_ownCur[t] = false;
  }
  g_ownAsideAge = any ? 0 : std::min(g_ownAsideAge + 1, 1 << 20);
  const int mode = g_cfgOwnship.load(std::memory_order_relaxed);
  gbrec::g_ownAsideHint = mode == 1 && g_ownAsideAge < kOwnAsideHold;
  gbrec::g_ownKey.store(mode == 2 && g_ownLocked.load(std::memory_order_relaxed) ? g_own.key : nullptr,
                        std::memory_order_release);
}

// CockpitTrueIpdLog's world-pass trace (WorldTrace, below): traced executions per load, the radius around the eyes.
constexpr int kWorldTraceMax = 6;
constexpr double kWorldTraceDist = 12.0;
int g_worldDumps = 0;
void WorldTrace(void** vec, int t, uint32_t tag);
void GbBegin(void* pass, void* ctx) {
  if (t_gbDepth++ > 0) {  // nested (not expected): the inner execution is left alone
    g_st.nested++;
    return;
  }
  if (t_cls != kClsNone) {  // inside another class's pass (not expected): left alone
    t_gbForeign = true;
    g_st.nested++;
    return;
  }
  t_gbForeign = false;
  t_cls = kClsNone;
  t_view = -1;
  t_capOnly = false;
  t_uploads = t_patchedN = 0;
  t_sm = 0xffff;
  t_tag = 0;
  if (!WantUploads()) return;
  g_st.gbExecs++;
  void** vec = gbpass::ItemVector(pass, ctx);
  gbrec::CollKey key;
  void* rg = nullptr;
  if (!vec || !gbrec::ExecKeyGuarded(vec, ctx, &key, &rg)) {
    g_st.idFail++;
    return;
  }
  t_sm = key.sm;
  t_tag = key.tag;
  if (key.sm != gbrec::kSmCockpit) {
    if (key.sm == 0 && g_logLeft.load(std::memory_order_relaxed) > 0 && g_worldDumps < kWorldTraceMax &&
        g_s.lastHmdCount >= 2 && g_s.hmdOrd >= 0 && g_s.hmdOrd <= 1)
      WorldTrace(vec, g_s.hmdOrd, key.tag);
    // The ownship (CockpitTrueIpdOwnship): an HMD view's world G-buffer / decal execution after that view's cockpit
    // execution this frame (MFD sensor views run before view 0's cockpit pass: never).
    const int ot = g_s.hmdOrd;
    if ((key.sm == 0 || key.sm == 3) && g_cfgOwnship.load(std::memory_order_relaxed) && g_s.lastHmdCount >= 2 &&
        ot >= 0 && ot <= 3 && g_f.v[ot].gbCk > 0) {
      if (key.sm == 0 && ot == 0 && g_ownLockFrame != g_frame) {
        g_ownLockFrame = g_frame;
        OwnConsumeEvents();
        OwnLockStep(vec);
      }
      // Stage 2 with the recorder on: it keeps the execution (gb_rec.h); else stage 1.
      const bool stage2 = g_cfgOwnship.load(std::memory_order_relaxed) == 2 && gbrec::Active();
      if (OwnMoves() && (stage2 ? OwnBegin2(ot) : OwnBegin(vec, ot))) {
        t_cls = kClsOwn;
        t_view = ot;
        if (!stage2) gbrec::t_ownAside = true;  // the recorder steps aside for this execution (its vector is ours)
        g_ownPasses++;
      }
    }
    return;
  }
  g_st.ckExecs++;
  if (key.tag != 600) {
    g_st.tagOther++;
    if (!g_loggedTag) Log("cockpit ipd: cockpit G-buffer execution with viewport tag %u (600 expected); counted", key.tag);
    g_loggedTag = true;
  }
  const State& s = g_s;
  const int t = s.hmdOrd;
  if (s.lastHmdCount < 2 || t > 3 || t >= s.lastHmdCount) {
    g_st.ckNotHmd++;
    return;
  }
  Frame& f = g_f;
  // A frame that is off still records each view's first upload (capture
  // only, forwarded as DCS's): the gPrevFrameViewProj check needs every view
  // of every frame, also to recover after a refuted frame.
  bool cap = f.off;
  if (!cap) {
    ViewFrame& vf = f.v[t];
    ++f.ckTotal;
    if (++vf.gbCk > 1 || f.ckTotal > 4) {  // a second cockpit pass in this view: the rest of the frame as DCS's
      f.off = true;
      f.offTail = true;
      g_st.offMulti++;
      if (f.anyPatched) g_offNext = true;
      cap = true;
    } else if (t > 0 && !f.v[0].done) {  // view 0 was not handled: the whole frame as DCS's
      f.off = true;
      f.offTail = true;
      g_st.offView0++;
      cap = true;
    }
  }
  t_cls = kClsGb;
  t_view = t;
  t_capOnly = cap;
  if (cap) {
    g_st.capOnly++;
    return;
  }
  if (!g_loggedView[t]) {
    g_loggedView[t] = true;
    Log("cockpit ipd: first cockpit G-buffer execution of HMD view %d (tag %u)", t, key.tag);
  }
  g_itemBase = *reinterpret_cast<uint8_t**>(*static_cast<uint8_t**>(ctx) + 0x438);  // (forward passes' vectors)
  g_itemRg = rg;
  if (g_logLeft.load(std::memory_order_relaxed) > 0 && g_itemDumps < 8) ItemSnap(vec, t);  // (DCS's own vector)
  t_helmetSrc.valid = false;
  HelmetSwap(vec);
}
// Trace: a forward cockpit pass's items (SimplePassData: vector [[ctx]+0x438] + word[[pass+0x68]+0x38] * 24,
// fwd_rec_count.h), with the base of the frame's G-buffer execution.
void FwdItemTraceRaw(void* pass, int t) {
  if (!g_itemBase || g_fwdItemDumps >= 8) return;
  const uint8_t* node = *reinterpret_cast<uint8_t* const*>(static_cast<uint8_t*>(pass) + 0x68);
  if (!node) return;
  const int16_t idx = *reinterpret_cast<const int16_t*>(node + 0x38);
  if (idx < 0) return;
  ++g_fwdItemDumps;
  ItemSnap(reinterpret_cast<void**>(g_itemBase + static_cast<int64_t>(idx) * 24), t);
  Log("cockpit ipd item trace: the next list is a forward cockpit pass's");
  ItemLog(g_f.v[t]);
}
// A forward cockpit pass's item vector (SimplePassData, fwd_rec_count.h: [[ctx]+0x438] + word[[pass+0x68]+0x38] *
// 24), with the base of this frame's cockpit G-buffer execution (the same render context); nullptr when unknown.
void** FwdItemVectorRaw(void* pass) {
  if (!g_itemBase || !g_itemRg) return nullptr;
  const uint8_t* node = *reinterpret_cast<uint8_t* const*>(static_cast<uint8_t*>(pass) + 0x68);
  if (!node) return nullptr;
  const int16_t idx = *reinterpret_cast<const int16_t*>(node + 0x38);
  size_t n = 0;
  if (idx < 0 || !gbrec::CollDescs(g_itemRg, &n) || static_cast<size_t>(idx) >= n) return nullptr;
  return reinterpret_cast<void**>(g_itemBase + static_cast<int64_t>(idx) * 24);
}
void** FwdItemVector(void* pass) {
  __try {
    return FwdItemVectorRaw(pass);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return nullptr;
  }
}
void FwdItemTrace(void* pass, int t) {
  __try {
    FwdItemTraceRaw(pass, t);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    g_itemView = -1;
  }
}

// Trace (CockpitTrueIpdLog, diagnostics for R31 R3 / the AH-64D's second crew member): a world G-buffer execution's
// (sm 0) entries of HMD views 0 / 1, the first kWorldTraceMax per load: the entry classes (NGModel SceneRenderable or
// another vtable, as module+rva), and the SceneRenderable items whose origin lies within kWorldTraceDist of DCS's eye
// midpoint, grouped by model path (count, instances, nearest / farthest, the nearest's offset right / up / forward),
// with the renderable's pass and flag bytes.
void ModRva(const void* p, char* out, size_t n) {
  HMODULE m = nullptr;
  wchar_t w[MAX_PATH] = {};
  if (p && GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                              static_cast<LPCWSTR>(p), &m) &&
      GetModuleFileNameW(m, w, MAX_PATH)) {
    const wchar_t* b = wcsrchr(w, L'\\');
    snprintf(out, n, "%ls+0x%llx", b ? b + 1 : w,
             static_cast<unsigned long long>(static_cast<const uint8_t*>(p) - reinterpret_cast<const uint8_t*>(m)));
  } else {
    snprintf(out, n, "%p", p);
  }
}
void WorldTraceRaw(void** vec, int t, uint32_t tag) {
  auto** b = static_cast<uint8_t**>(vec[0]);
  auto** e = static_cast<uint8_t**>(vec[1]);
  if (!b || e < b) return;
  const void* srVt = SrVtbl();  // R63 U5: by RTTI, not the raw recorded RVA
  if (!srVt) return;
  double mid[3], ax[3], ipd = 0;
  for (int k = 0; k < 3; ++k) {
    mid[k] = 0.5 * (g_dcsEye[0][k] + g_dcsEye[1][k]);
    ax[k] = g_dcsEye[1][k] - g_dcsEye[0][k];
    ipd += ax[k] * ax[k];
  }
  ipd = std::sqrt(ipd);
  if (!(ipd > 0.03 && ipd < 0.2)) return;
  for (double& a : ax) a /= ipd;
  struct Grp {
    std::string path;
    int n = 0;
    std::vector<const void*> insts;
    double dmin = 1e9, dmax = 0, rel[3] = {};
    uint32_t pass = 0, flags = 0;
    int readable = 0;
    uint32_t page = 0, elem = 0, stride = 0;
    float el[12] = {};
  };
  std::vector<Grp> grp;
  std::vector<std::pair<std::string, int>> other;
  size_t sr = 0, total = static_cast<size_t>(e - b);
  for (uint8_t** p = b; p < e; ++p) {
    const uint8_t* r = *p;
    if (!r) continue;
    const void* vt = *reinterpret_cast<void* const*>(r);
    if (vt != srVt) {
      char nm[96];
      ModRva(vt, nm, sizeof(nm));
      bool f = false;
      for (auto& o : other)
        if (o.first == nm) ++o.second, f = true;
      if (!f && other.size() < 16) other.emplace_back(nm, 1);
      continue;
    }
    ++sr;
    const uint8_t* item = *reinterpret_cast<const uint8_t* const*>(r + 0x10);
    if (!item) continue;
    const uint8_t* mat = *reinterpret_cast<const uint8_t* const*>(item + 0x10);
    char path[128] = "?";
    if (mat) {
      const uint8_t* np = *reinterpret_cast<const uint8_t* const*>(mat + 0x20);
      if (np) {
        memcpy(path, np + 8, sizeof(path) - 1);
        path[sizeof(path) - 1] = 0;
        size_t n = 0;
        while (n < sizeof(path) - 1 && path[n] >= 0x20 && path[n] < 0x7f) ++n;
        path[n] = 0;
      }
    }
    Grp* g = nullptr;
    for (auto& x : grp)
      if (x.path == path) g = &x;
    if (!g) {
      if (grp.size() >= 192) continue;
      grp.emplace_back();
      g = &grp.back();
      g->path = path;
      // (the first item's sbPositions element as read: page, element, stride, floats 0..11)
      const uint8_t* const* gl = gbrec::g_env.globals;
      const uint8_t* gg = gl ? *gl : nullptr;
      const uint8_t* base = gg ? *reinterpret_cast<const uint8_t* const*>(gg + 0x80) : nullptr;
      g->page = *reinterpret_cast<const uint32_t*>(item + 0xd0);
      g->elem = *reinterpret_cast<const uint32_t*>(item + 0xd4);
      if (base) {
        const uint8_t* ps = base + static_cast<size_t>(g->page) * 0x30;
        g->stride = *reinterpret_cast<const uint32_t*>(ps + 4);
        const uint32_t used = *reinterpret_cast<const uint32_t*>(ps + 0x14);
        const uint8_t* data = *reinterpret_cast<const uint8_t* const*>(ps + 0x18);
        if (data && g->stride >= 48 && g->stride <= 256 && static_cast<uint64_t>(g->elem + 1) * g->stride <= used)
          memcpy(g->el, data + static_cast<size_t>(g->elem) * g->stride, sizeof(g->el));
      }
    }
    ++g->n;
    const void* key = *reinterpret_cast<void* const*>(item + 0x18);
    if (std::find(g->insts.begin(), g->insts.end(), key) == g->insts.end()) g->insts.push_back(key);
    double o[3];
    if (!ItemOriginRaw(item, o)) continue;
    ++g->readable;
    double d2 = 0, rel[3] = {0, 0, 0};
    for (int k = 0; k < 3; ++k) {
      d2 += (o[k] - mid[k]) * (o[k] - mid[k]);
      rel[0] += (o[k] - mid[k]) * ax[k];
      rel[1] += (o[k] - mid[k]) * g_dcsUp[k];
      rel[2] += (o[k] - mid[k]) * g_dcsFwd[k];
    }
    const double d = std::sqrt(d2);
    if (d < g->dmin) {
      g->dmin = d;
      memcpy(g->rel, rel, sizeof(rel));
      g->pass = *reinterpret_cast<const uint32_t*>(r + 0x60);
      g->flags = *reinterpret_cast<const uint32_t*>(r + 0x64);
    }
    g->dmax = std::max(g->dmax, d);
  }
  std::sort(grp.begin(), grp.end(), [](const Grp& x, const Grp& y) { return x.dmin < y.dmin; });
  std::string oth;
  for (auto& o : other) oth += " " + o.first + " x" + std::to_string(o.second);
  Log("cockpit ipd world trace: frame %llu HMD view %d world G-buffer (sm 0, tag %u): %zu entries, %zu NGModel "
      "SceneRenderable, others:%s; eye midpoint %.3f %.3f %.3f (gOrigin %.1f %.1f %.1f); up to %d models, nearest the "
      "eyes first (readable item origins; within %.1f m marked *):",
      static_cast<unsigned long long>(g_frame), t, tag, total, sr, oth.empty() ? " none" : oth.c_str(), mid[0], mid[1],
      mid[2], g_dcsOrigin[0], g_dcsOrigin[1], g_dcsOrigin[2], 48, kWorldTraceDist);
  if (grp.size() > 48) grp.resize(48);
  for (auto& g : grp)
    Log("  %s%s: %d items (%d with a readable origin), %zu instances, %.3f..%.3f m (nearest right %+.3f up %+.3f fwd "
        "%+.3f), pass %u flags 0x%08x; first item page %u elem %u stride %u el %.3f %.3f %.3f | %.3f %.3f %.3f | %.3f "
        "%.3f %.3f | %.3f %.3f %.3f",
        g.dmin <= kWorldTraceDist ? "* " : "", g.path.c_str(), g.n, g.readable, g.insts.size(), g.dmin, g.dmax,
        g.rel[0], g.rel[1], g.rel[2], g.pass, g.flags, g.page, g.elem, g.stride, g.el[0], g.el[1], g.el[2], g.el[3],
        g.el[4], g.el[5], g.el[6], g.el[7], g.el[8], g.el[9], g.el[10], g.el[11]);
}
void WorldTrace(void** vec, int t, uint32_t tag) {
  ++g_worldDumps;
  __try {
    WorldTraceRaw(vec, t, tag);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    Log("cockpit ipd world trace: fault while reading the item vector (stopped)");
    g_worldDumps = kWorldTraceMax;
  }
}

void GbEnd() {
  if (t_gbDepth > 0 && --t_gbDepth > 0) return;  // a nested execution's end
  if (t_gbForeign) {
    t_gbForeign = false;
    return;
  }
  int cls = t_cls;
  const int t = t_view;
  // The ownship: this view's cockpit struct, for a world pass without an upload of its own (before
  // HelmetPassEnd clears the source).
  if (t_cls == kClsGb && t_view >= 0 && t_view <= 3 && !t_capOnly && t_helmetSrc.valid &&
      t_helmetSrc.frame == g_frame && t_helmetSrc.view == t_view)
    g_ownViewSrc[t_view] = t_helmetSrc;
  HelmetPassEnd();  // DCS's item vector and b7 back (helmet models' camera switch)
  OwnPassEnd();     // ... and the ownship's
  if (cls == kClsNone && t_sm == 0xffff && !g_restN) {
    if (WantUploads()) LearnBound();
    return;
  }
  t_cls = kClsNone;  // before the restore: it goes through the upload hook unchanged
  t_view = -1;
  const bool capOnly = t_capOnly;
  t_capOnly = false;
  if (WantUploads()) LearnBound();
  int restores = g_restN;
  if (!WantUploads()) cls = kClsNone;  // after a fault: restore only (below), no frame bookkeeping
  if (cls == kClsGb && capOnly) {
    // recorded only (frame off): no frame bookkeeping
  } else if (cls == kClsGb && t >= 0) {
    Frame& f = g_f;
    ViewFrame& vf = f.v[t];
    const bool needed =
        !f.off && (vf.dwReady ? (NonZero(vf.dw) || NonZero(vf.dp)) : (f.h != 0 || NonZero(g_prevDw[t])));
    // Not patched, or some cPerView upload of this execution forwarded as
    // DCS's (partial box, ambiguous offset, self-check): its draws mix eyes.
    if (needed && (!vf.patched || t_patchedN < t_uploads)) {
      if (!vf.selfFailed && !vf.gated && !vf.patched) g_st.failNoUpload++;
      vf.failed = true;
    }
    if (vf.failed) {
      f.anyFailed = true;
      if (t == 0)
        f.off = true;
      else if (f.anyPatched)
        g_offNext = true;
    } else {
      vf.done = true;
    }
    // Stencil cross-check (counted only: the state bound at the end need not be the last draw's).
    State& s = g_s;
    if (s.ctx) {
      ID3D11DepthStencilState* dss = nullptr;
      UINT ref = 0;
      s.ctx->OMGetDepthStencilState(&dss, &ref);
      D3D11_DEPTH_STENCIL_DESC d{};
      if (dss) {
        dss->GetDesc(&d);
        dss->Release();
      }
      (dss && d.StencilEnable && ref == 40 ? g_st.stencilOk : g_st.stencilMismatch)++;
    }
    vf.depthTex = PassDepthTexNow();
    if (g_itemView == t) ItemLog(vf);
  } else if (t_sm != 0xffff && g_s.ctx) {
    ID3D11DepthStencilState* dss = nullptr;
    UINT ref = 0;
    g_s.ctx->OMGetDepthStencilState(&dss, &ref);
    if (dss) {
      D3D11_DEPTH_STENCIL_DESC d{};
      dss->GetDesc(&d);
      dss->Release();
      if (d.StencilEnable && ref == 40) g_st.stencilWorld++;
    }
  }
  RestoreAll();
  if (g_logLeft.load(std::memory_order_relaxed) > 0) {
    const ViewFrame* vf = t >= 0 ? &g_f.v[t] : nullptr;
    TraceLine("frame %llu view %d GBufferPass sm %u tag %u: %s, cPerView uploads %d, patched %d, Dw %.2f,%.2f,%.2f "
              "mm Dp %.2f,%.2f,%.2f mm, restores %d",
              static_cast<unsigned long long>(g_frame), t >= 0 ? t : g_s.hmdOrd, t_sm, t_tag,
              cls == kClsGb ? (capOnly ? "cockpit, frame off (recorded)" : "cockpit") : "not patched", t_uploads, t_patchedN, vf ? vf->dw[0] * 1e3 : 0.0,
              vf ? vf->dw[1] * 1e3 : 0.0, vf ? vf->dw[2] * 1e3 : 0.0, vf ? vf->dp[0] * 1e3 : 0.0,
              vf ? vf->dp[1] * 1e3 : 0.0, vf ? vf->dp[2] * 1e3 : 0.0, restores);
  }
  t_sm = 0xffff;
}

// After a fault inside the observer: DCS's bytes back into the buffers this
// execution patched (never left at the true eye), under its own guard.
void RestoreAfterFault() {
  __try {
    RestoreAll();
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    g_restN = 0;
  }
}
void ObserverBody(void* pass, void* ctx, bool begin);
void Observer(void* pass, void* ctx, bool begin) {
  if (GetCurrentThreadId() != ptiming::g_topTid.load(std::memory_order_relaxed)) return;
  CostScope cost(kCostObserver);
  ObserverBody(pass, ctx, begin);
}
void ObserverBody(void* pass, void* ctx, bool begin) {
  g_busy.fetch_add(1);
  __try {
    if (begin)
      GbBegin(pass, ctx);
    else
      GbEnd();
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    g_fault = true;
    t_cls = kClsNone;
    t_capOnly = false;
    t_gbDepth = 0;
    HelmetPassEnd();
    OwnPassEnd();
    RestoreAfterFault();
  }
  g_busy.fetch_sub(1);
}

// ---------------------------------------------------------------------------
// Other passes (fixation_depth.h's boundary callback, render thread)
// ---------------------------------------------------------------------------

// A pass begins: true when its upload entries should be armed (forward
// cockpit pass of a patched view, or a watched "Cockpit" pass while tracing).
bool PassBegin(void* pass) {
  if (!WantUploads() || t_cls != kClsNone) return false;
  char n[48];
  PassName(pass, n, sizeof(n));
  if (strstr(n, "TransparentPass") && !strstr(n, "Cockpit")) {  // the world's transparent pass: the ownship
    const int ot = g_s.hmdOrd;
    if (!g_cfgOwnship.load(std::memory_order_relaxed) || !OwnMoves() || ot < 0 ||
        ot > 3 || g_f.v[ot].gbCk == 0)
      return false;
    t_uploads = t_patchedN = 0;
    t_fwdArmed = false;
    if (!OwnBegin(FwdItemVector(pass), ot)) return false;
    t_cls = kClsOwnFwd;
    t_view = ot;
    t_pass = pass;
    g_ownFwdPasses++;
    return true;
  }
  if (!strstr(n, "Cockpit")) return false;
  const int t = g_s.hmdOrd;
  const bool tc = strstr(n, "TransparentCockpit") != nullptr;
  t_uploads = t_patchedN = 0;
  t_fwdArmed = false;
  if (tc && t >= 0 && t <= 3 && g_logLeft.load(std::memory_order_relaxed) > 0) FwdItemTrace(pass, t);
  if (tc && g_cfgForward.load(std::memory_order_relaxed) && t >= 0 && t <= 3 && (!g_f.off || g_f.v[t].tailOff) &&
      g_f.v[t].patched && !g_f.v[t].failed) {
    t_cls = kClsFwd;
    t_view = t;
    t_pass = pass;
    g_st.fwdPasses++;
    t_helmetSrc.valid = false;
    if (!g_f.off) HelmetSwap(FwdItemVector(pass));
    return true;
  }
  if (g_logLeft.load(std::memory_order_relaxed) > 0) {
    t_cls = kClsWatch;
    t_view = t;
    t_pass = pass;
    return true;
  }
  return false;
}
// The per-HMD-view overlay passes that draw the cockpit's symbology after DLSS
// (HUD_MFD_after_DLSS). Exact names: MFDOverlayPass (the MFD render) is not one.
bool IsOverlayName(const char* n) {
  return strcmp(n, "OverlayHDRPass") == 0 || strcmp(n, "OverlayLDRPass") == 0 || strcmp(n, "GUIPass") == 0;
}
// An overlay pass begins (after far depth's own begin work for it, so a far
// depth arm is already held and ours nests in it): true when its upload
// entries should be armed. Only an HMD view 0-3 whose cockpit G-buffer pass was
// patched this frame with Dw != 0 (same h; an MFD sensor view's or the GUI's
// pass after the last view never qualifies), or watched while tracing.
// The learner's arm at h 0: about one frame in four, picked by a hash of the frame number (a fixed modulus would
// alias with content drawn on a frame cadence, e.g. an MFD page alternating every other frame, never observed).
inline bool OvlLearnFrameH0(uint64_t frame) { return ((frame * 0x9e3779b97f4a7c15ull) >> 62) == 0; }
bool OvlBegin(void* pass) {
  if (!WantUploads() || t_cls != kClsNone) return false;
  char n[48];
  PassName(pass, n, sizeof(n));
  if (!IsOverlayName(n)) return false;
  const int t = g_s.hmdOrd;
  t_uploads = t_patchedN = t_ovlOther = t_mvpPatchedN = 0;
  t_fwdArmed = false;
  // A moved view's pass; or, on the cpu path, a pass at h 0 (Depth boost at scale 1, CockpitTrueIpd=1): armed for
  // the learner only. Its observer compares each record between the two eyes (cockpit-fixed vs eye-relative),
  // which needs no shift, so the classes are committed before the first boost and its first frame moves the
  // displays (was: about 16 boosted frames at DCS's eye after each mission load). The arm writes nothing on the
  // cpu path; the replay decision keeps every view at DCS's camera at h 0. About one frame in four only:
  // the observer costs ~0.19 ms per frame (live F-16), and a quarter of the frames at scale 1 still commit a class
  // in about 64 frames (0.7 s) after a mission load.
  const bool moveArm = t >= 0 && t <= 3 && g_f.v[t].patched && !g_f.v[t].failed && NonZero(g_f.v[t].dw);
  const bool learnArm = PathCpu() && t >= 0 && t <= 3 && !g_f.observe && g_f.h == 0 && !NonZero(g_f.v[t].dw) &&
                        !g_f.v[t].failed && OvlLearnFrameH0(g_frame);
  if (g_cfgOverlays.load(std::memory_order_relaxed) && !g_ovlInert.load(std::memory_order_relaxed) && !g_f.off &&
      (moveArm || learnArm) && (!PathCpu() || ocpu::Installed())) {
    if (!moveArm) g_st.ovlLearnPasses++;
    t_cls = kClsOvl;
    t_view = t;
    t_pass = pass;
    t_ovlProbe = false;
    t_ovlStaleDraws = 0;
    t_ovlUpN = 0;
    t_ovlLdr = strcmp(n, "OverlayHDRPass") != 0;
    if (g_ovlRestView != t || g_ovlRestFrame != g_frame) {
      g_ovlRestN = 0;
      g_ovlRestView = t;
      g_ovlRestFrame = g_frame;
    }
    for (int i = 0; i < g_ovlRestN; ++i) g_ovlRest[i].uploaded = false;
    g_st.ovlPasses++;
    return true;
  }
  if (g_logLeft.load(std::memory_order_relaxed) > 0) {
    t_cls = kClsWatch;
    t_view = t;
    t_pass = pass;
    return true;
  }
  return false;
}
void PassArmed(bool ok) {
  t_fwdArmed = ok;
  if (!ok && t_cls == kClsFwd) g_st.fwdNoArm++;
  if (!ok && t_cls == kClsOvl) g_st.ovlNoArm++;
}
// The overlay draw probe (fixation_depth.h's CockpitIpdOverlayBegin / OvlDrawSeh).
bool OvlProbeDraws() { return g_ovlDrawProbeLeft > 0 || (g_dumpActiveFlag && g_dumpFramesLeftFlag > 0); }
void OvlProbePassArmed() {
  if (g_ovlDrawProbeLeft > 0) --g_ovlDrawProbeLeft;
  g_st.ovlProbePasses++;
  t_ovlProbe = true;
}
// A draw of the open overlay pass (render thread, before it is forwarded): a VS constant buffer bound now that
// an earlier overlay pass of this view restored to DCS's bytes and this pass has not uploaded again.
void OvlDrawProbe(ID3D11DeviceContext* c) {
  if (!t_ovlProbe || t_cls != kClsOvl) return;
  g_st.ovlProbeDraws++;
  ID3D11Buffer* cb[D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT] = {};
  c->VSGetConstantBuffers(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT, cb);
  bool stale = false, unseen = false;
  for (ID3D11Buffer* b : cb) {
    if (!b) continue;
    ID3D11Resource* r = b;
    for (int i = 0; i < g_ovlRestN && !stale; ++i) stale = g_ovlRest[i].res == r && !g_ovlRest[i].uploaded;
    // An effect buffer (not a learned cPerView) this pass has not uploaded through our hooks.
    if (!IsKnown(r)) {
      bool seen = false;
      for (int i = 0; i < t_ovlUpN && !seen; ++i) seen = t_ovlUp[i] == r;
      unseen = unseen || !seen;
    }
    b->Release();
  }
  if (stale) {
    ++t_ovlStaleDraws;
    g_st.ovlStaleDraws++;
  }
  if (t_view >= 0 && t_view <= 3) {
    ++g_f.v[t_view].ovlDraws;
    if (unseen) {
      ++g_f.v[t_view].ovlDrawsUnseen;
      g_st.ovlDrawsUnseen++;
    }
  }
}
void NoteOvlMidLoss() {
  g_st.ovlMidLoss++;
  if (t_view >= 0 && t_view <= 3) ++g_f.v[t_view].ovlLostN;
}
// A Map of DCS's context inside an open overlay pass (fixation_depth.h's Map hook): a dynamic constant buffer
// written with WRITE_DISCARD is a symbology path the upload hooks cannot see (counted per view).
// The cpu path also observes such a write at its Unmap (OvlNoteUnmap): the bytes DCS wrote, read back from the
// mapping before it is handed to the GPU.
ID3D11Resource* t_mapRes = nullptr;
UINT t_mapSub = 0, t_mapLen = 0;
const void* t_mapData = nullptr;
void OvlNoteMap(ID3D11Resource* r, UINT sub, D3D11_MAP type, const D3D11_MAPPED_SUBRESOURCE* m) {
  if (t_cls != kClsOvl || !r || t_view < 0 || t_view > 3) return;
  D3D11_RESOURCE_DIMENSION dim = D3D11_RESOURCE_DIMENSION_UNKNOWN;
  r->GetType(&dim);
  if (dim != D3D11_RESOURCE_DIMENSION_BUFFER) return;
  D3D11_BUFFER_DESC bd{};
  static_cast<ID3D11Buffer*>(r)->GetDesc(&bd);
  if (!(bd.BindFlags & D3D11_BIND_CONSTANT_BUFFER)) return;
  g_st.ovlMapCb++;
  ++g_f.v[t_view].ovlMaps;
  if (PathCpu() && m && m->pData && sub == 0 && type != D3D11_MAP_READ) {
    t_mapRes = r;
    t_mapSub = sub;
    t_mapData = m->pData;
    t_mapLen = bd.ByteWidth;
  }
}
void OvlNoteUnmap(ID3D11Resource* r, UINT sub) {
  if (!r || r != t_mapRes || sub != t_mapSub) return;
  const void* d = t_mapData;
  const UINT len = t_mapLen;
  t_mapRes = nullptr;
  t_mapData = nullptr;
  if (t_cls != kClsOvl || !PathCpu() || !ocpu::Installed() || !Active() || !d) return;
  ocpu::ObserveUpload(r, static_cast<const uint8_t*>(d), std::min<UINT>(len, kMaxSrc));
}
bool PassOwns(void* pass) {
  return t_pass && t_pass == pass &&
         (t_cls == kClsFwd || t_cls == kClsWatch || t_cls == kClsOvl || t_cls == kClsOwnFwd);
}
// A pass of ours (forward, watched, overlay, lighting) is open: its end must
// still be handled after a fault.
bool PassOpen() {
  return t_pass &&
         (t_cls == kClsFwd || t_cls == kClsWatch || t_cls == kClsLight || t_cls == kClsOvl || t_cls == kClsOwnFwd);
}
// armIntact: the upload entries were still ours at the end (fixation_depth.h
// checks). d3d11 also switches them after the binder's upload (seen on WARP),
// so a lost arm counts only when no cPerView upload was seen: then the
// switch may have come before the binder and its upload went past us.
void PassEnd(void* pass, bool armIntact = true) {
  const int cls = t_cls, t = t_view;
  HelmetPassEnd();  // DCS's item vector and b7 back (helmet models' camera switch in a forward cockpit pass)
  OwnPassEnd();     // ... and the ownship's in the world's transparent pass
  // Read before the reset below: the failure reason must use the begin's
  // verdict (live 2026-10-13 every failure read "not armed" while the arm
  // never failed: the reason read t_fwdArmed after it was cleared).
  const bool armedAtBegin = t_fwdArmed;
  const bool armed = armedAtBegin && (armIntact || t_uploads > 0);
  t_cls = kClsNone;
  t_view = -1;
  t_pass = nullptr;
  t_fwdArmed = false;
  const bool active = Active();
  if (active) LearnBound();
  const int restores = g_restN;
  if (cls == kClsOvl && t >= 0 && t <= 3 && g_ovlRestView == t && g_ovlRestFrame == g_frame)
    for (int i = 0; i < g_restN; ++i) {  // effect buffers (not a learned cPerView) restored now
      if (IsKnown(g_rest[i].res)) continue;
      bool have = false;
      for (int j = 0; j < g_ovlRestN && !have; ++j) have = g_ovlRest[j].res == g_rest[i].res;
      if (!have && g_ovlRestN < kOvlRestMax) g_ovlRest[g_ovlRestN++] = OvlRestored{g_rest[i].res, false};
    }
  t_ovlProbe = false;
  RestoreAll();
  // A forward cockpit pass of a view whose opaque cockpit was moved must have
  // moved every cPerView upload: not armed, the arm lost to a table switch
  // with no upload seen, or an upload forwarded as DCS's (partial box, ambiguous offset,
  // self-check, no restore entry) drew the transparent cockpit at the boosted
  // eye over the opaque one at the true eye. The pass is drawn; the next frame
  // goes unpatched and the failure counts towards the self-check rate (> 1 %:
  // inert), so a persistent cause does not alternate frames.
  bool failed = false;
  if (active && cls == kClsFwd && t >= 0 && t <= 3) {
    Frame& f = g_f;
    const ViewFrame& vf = f.v[t];
    const bool needed = !f.off && vf.dwReady && (NonZero(vf.dw) || NonZero(vf.dp));
    if (needed && (!armed || t_patchedN < t_uploads)) {
      failed = true;
      g_st.fwdFail++;
      (!armedAtBegin ? g_st.fwdFailNoArm : !armed ? g_st.fwdFailArmLost : g_st.fwdFailUnpatched)++;
      static std::atomic<int> logged{0};
      if (logged.fetch_add(1) < 10) {
        char n[48];
        PassName(pass, n, sizeof(n));
        Log("cockpit ipd: forward pass failed: frame %llu view %d %s: %s, cPerView uploads %d, patched %d",
            static_cast<unsigned long long>(g_frame), t, n,
            !armedAtBegin ? "not armed" : !armed ? "arm lost before any upload" : "upload not patched", t_uploads,
            t_patchedN);
      }
      f.anyFailed = true;
      if (f.anyPatched) g_offNext = true;
      ++g_winAttempts;
      ++g_winFails;
    }
  }
  // An overlay pass of a moved view: not armed, the arm lost before any
  // upload, or an upload of the view's camera forwarded as DCS's drew that
  // symbology at the boosted eye (today's image for that pass). Counted in the
  // overlays' own window only: the cockpit itself is unaffected, so the frame
  // and the next stay patched.
  if (active && cls == kClsOvl && t >= 0 && t <= 3 && PathCpu()) {
    // The cpu path: this arm only observes (the learner's samples). A lost arm costs samples, never an image.
    if (armedAtBegin && !armIntact) g_st.ovlObsLost++;
  } else if (active && cls == kClsOvl && t >= 0 && t <= 3) {
    const int own = t_uploads - t_ovlOther;
    if (own == 0) g_st.ovlNoUpload++;
    // No effect-buffer MVP of the view's camera patched in this pass (nothing drawn with one, or drawn with a
    // buffer the upload hooks did not see, e.g. a mapped one).
    if (g_cfgOvlMvp.load(std::memory_order_relaxed) && t_mvpPatchedN == 0) g_st.mvpPassNone++;
    // The upload entries no longer ours at the end: d3d11 toggled the table after our last re-arm point and
    // nothing of ours put them back (Flush / ExecuteCommandList are repaired at once, fixation_depth.h), so
    // effect uploads after the toggle may have gone out at the boosted eye. Counted as a failure of the pass
    // (the overlays' own window: a persistent cause turns the overlays off rather than flashing them).
    const bool lost = armedAtBegin && !armIntact;
    if (lost) {
      g_st.ovlArmLost++;
      ++g_f.v[t].ovlLostN;
    }
    if (t_ovlStaleDraws > 0) g_st.ovlStalePasses++;
    if (!armed || t_patchedN < own || lost) {
      failed = true;
      g_st.ovlFail++;
      ++g_ovlAttempts;
      ++g_ovlFails;
      static std::atomic<int> logged{0};
      if (logged.fetch_add(1) < 10) {
        char n[48];
        PassName(pass, n, sizeof(n));
        Log("cockpit ipd: overlay pass not moved: frame %llu view %d %s: %s, cPerView uploads %d (other camera %d), "
            "patched %d",
            static_cast<unsigned long long>(g_frame), t, n,
            !armedAtBegin ? "not armed"
            : !armed      ? "arm lost before any upload"
            : lost        ? "upload entries switched by d3d11 inside the pass"
                          : "upload not patched",
            t_uploads, t_ovlOther, t_patchedN);
      }
    }
  }
  if (active && !failed && t >= 0 && t <= 3) {
    if (cls == kClsFwd) ++g_f.v[t].fwdN;
    if (cls == kClsOvl) ++g_f.v[t].ovlN;
  }
  if (g_logLeft.load(std::memory_order_relaxed) > 0) {
    char n[48];
    PassName(pass, n, sizeof(n));
    TraceLine("frame %llu view %d %s: %s, cPerView uploads %d (other camera %d), patched %d, effect MVP uploads "
              "patched %d, restores %d%s",
              static_cast<unsigned long long>(g_frame), t, n,
              cls == kClsFwd ? "forward cockpit" : cls == kClsOvl ? "overlay" : "watched", t_uploads, t_ovlOther,
              t_patchedN, t_mvpPatchedN, restores,
              failed ? (armed ? ", FAILED (upload not patched)" : ", FAILED (not armed)") : "");
  }
  t_ovlOther = t_mvpPatchedN = t_ovlStaleDraws = 0;
}

// The frame's state for DualCameraStatusNow (render thread writes at EndFrame).
std::atomic<bool> g_dcBoostOk{false}, g_dcPatched{false};
// R62: the last frame drew a cockpit pass (view 0 latched, or a cockpit G-buffer pass in any view); frames that did,
// counted for the per-second sample (an external view draws none: nothing to patch, not a failure).
std::atomic<bool> g_dcCockpit{false};
std::atomic<uint64_t> g_dcCockpitFrames{0};
std::atomic<float> g_dcScale{0.0f};  // the newest BoostOut record's scale (1 when not applied); 0 without BoostOut
std::atomic<int64_t> g_dcQpc{0};     // QPC of the last EndFrame

// BoostOut read and fresh: mapped, a record within kStaleFrames, not dry.
bool BoostFresh() {
  return g_boostView.load(std::memory_order_relaxed) && !g_match.Stale(g_frame) && !g_match.Dry();
}

// R63: the frame end's evidence for an active StereoAck (g_ackMissRun above); *why when there is none.
bool AckEvidence(const Frame& f, const char** why) {
  const uint64_t ex = g_st.gbExecs.load(std::memory_order_relaxed), fl = g_st.idFail.load(std::memory_order_relaxed);
  const bool noneIdentified = ex > g_ackIdExecs && fl >= g_ackIdFails && fl - g_ackIdFails == ex - g_ackIdExecs;
  g_ackIdExecs = ex;
  g_ackIdFails = fl;
  if (f.latched || f.anyPatched)
    g_ackMissRun = 0;
  else if ((f.ckTotal > 0 || noneIdentified) && g_ackMissRun < kAckMissFrames)
    ++g_ackMissRun;
  if (!g_s.ctx && !g_ackAssumeDevice) {
    *why = "no D3D11 device learnt";
    return false;
  }
  if (g_ackMissRun >= kAckMissFrames) {
    *why = "cockpit passes drawn but none intercepted";
    return false;
  }
  return true;
}

// EndFrame: the StereoAck write (when the block is open) and the status inputs.
void AckFrame(const Frame& f) {
  const int mode = g_cfgMode.load(std::memory_order_relaxed);
  const bool boostOk = BoostFresh();
  const char* why = nullptr;
  const char* noEvidence = nullptr;
  const bool evidence = AckEvidence(f, &noEvidence);
  // R77: a recent frame past the shift limit was not patched: the engine does not hold the cockpit at the true eye
  const bool tooBig = g_tooBigAt >= 0 && g_frame - static_cast<uint64_t>(g_tooBigAt) < kTooBigHoldFrames;
  const bool healthy =
      Active() && !InertNow(&why) && !g_relRefuted.load(std::memory_order_relaxed) && evidence && !tooBig;
  const int64_t q = QpcNow();
  float scale = 0.0f;
  if (boostOk && g_match.count) {
    const Rec& r = g_match.At(0);
    scale = RecH(r) != 0 ? r.scale : 1.0f;
  }
  g_dcBoostOk.store(boostOk, std::memory_order_relaxed);
  g_dcScale.store(scale, std::memory_order_relaxed);
  g_dcPatched.store(f.anyPatched && !f.observe, std::memory_order_relaxed);
  const bool cockpit = f.latched || f.ckTotal > 0;
  g_dcCockpit.store(cockpit, std::memory_order_relaxed);
  if (cockpit) g_dcCockpitFrames.fetch_add(1, std::memory_order_relaxed);
  g_dcQpc.store(q, std::memory_order_release);
  if (psh::CockpitStereo* b = g_ackView.load(std::memory_order_relaxed)) {
    const bool paused = g_userOff.load(std::memory_order_relaxed);
    // Hysteresis: a transient BoostOut gap (stale / dry for a frame or a few) keeps an active ack active for
    // up to g_ackHoldMs, so the layer does not leave dual-camera mode (and move the scale) for it. The key
    // pause and health clear it at once (StereoAckCompose / healthy below).
    LARGE_INTEGER fq;
    QueryPerformanceFrequency(&fq);
    const int64_t holdTicks = fq.QuadPart * std::max(0, g_ackHoldMs.load(std::memory_order_relaxed)) / 1000;
    bool held = false, bridged = false;
    const bool boostAck =
        AckBoostUsable(g_ackHold, boostOk, g_ackWasActive && mode == 1 && healthy && !paused, q, holdTicks, &held,
                       &bridged);
    const StereoAckFields v = StereoAckCompose(mode == 1 && healthy && boostAck, mode == 2, paused, g_frame, q);
    StereoAckWrite(b, v);
    g_ackWrites.fetch_add(1, std::memory_order_relaxed);
    if (held && (v.flags & kStereoActive)) g_st.ackHeld++;
    if (bridged) g_st.ackBridged++;
    // The layer logs "engine dual camera gone" on such a drop and returns to its usual cockpit-return steps.
    const bool act = (v.flags & kStereoActive) != 0;
    if (g_ackWasActive && !act && mode == 1) {
      g_st.ackDrops++;
      (!healthy ? g_st.ackDropHealth : paused ? g_st.ackDropPause : g_st.ackDropBoost)++;
      if (g_ackDropLogs < 10) {
        ++g_ackDropLogs;
        const char* inert = nullptr;
        const char* reason = !Active()             ? "fault"
                             : InertNow(&inert)    ? inert
                             : g_relRefuted.load() ? "gPrevFrameViewProj refuted"
                             : !evidence           ? noEvidence
                             : tooBig              ? "per-eye shift above the engine's limit"
                             : paused              ? "paused (in-flight key)"
                             : !boostAck           ? "BoostOut not fresh (stale / dry) for longer than the hold"
                                                   : "?";
        Log("cockpit ipd: frame %llu: stereo ack inactive (%s)", static_cast<unsigned long long>(g_frame), reason);
      }
    }
    g_ackWasActive = act;
    if (mode == 1 && !evidence != g_ackNoEvidence) {
      g_ackNoEvidence = !evidence;
      if (g_ackEvidenceLogs < 10) {
        ++g_ackEvidenceLogs;
        if (!evidence)
          Log("cockpit ipd: frame %llu: stereo ack held inactive (%s); the layer keeps its own cockpit steps",
              static_cast<unsigned long long>(g_frame), noEvidence);
        else
          Log("cockpit ipd: frame %llu: stereo ack may be active again (cockpit pass intercepted)",
              static_cast<unsigned long long>(g_frame));
      }
    }
  }
}
// At fixation_depth.h's frame end.
void EndFrame() {
  Frame& f = g_f;
  const bool obs = f.observe;
  // The camera each view's cockpit was really rendered with (also in a frame
  // turned off after some views were patched); 0 when not patched.
  for (int t = 0; t < 4; ++t)
    for (int i = 0; i < 3; ++i) g_prevDw[t][i] = !obs && f.v[t].patched ? f.v[t].dw[i] : 0.0;
  // The store: this frame's gViewProj per view, DCS's and the one really sent.
  for (int t = 0; t < 4; ++t) {
    PrevStore& c = g_cur[t];
    if (c.valid && c.tag == g_frame + 1) {
      if (obs || !f.v[t].patched) {
        c.sent = c.vp;
        c.patched = false;
      }
      g_store[t] = c;
    } else {
      g_store[t].valid = false;
    }
    c = PrevStore{};
  }
  // gPrevFrameViewProj refuted in this frame: every frame off until kRelRecover frames agree.
  if (f.relFail) {
    if (!g_relRefuted && g_relLogs < 10) {
      ++g_relLogs;
      Log("cockpit ipd: frame %llu: gPrevFrameViewProj is not the previous camera of its eye in some view (or the "
          "store disagrees); cockpit not patched until %d frames agree",
          static_cast<unsigned long long>(g_frame), kRelRecover);
    }
    g_relRefuted = true;
    g_relGood = 0;
  } else if (g_relRefuted && f.relCmp > 0 && ++g_relGood >= kRelRecover) {
    g_relRefuted = false;
    g_relGood = 0;
    if (g_relLogs < 10) {
      ++g_relLogs;
      Log("cockpit ipd: gPrevFrameViewProj agrees again (%d frames)", kRelRecover);
    }
  }
  if (g_prevTraced && g_prevLogLeft.load(std::memory_order_relaxed) > 0) g_prevLogLeft--;  // N frames traced
  g_prevTraced = false;
  if (f.anyPatched && f.anyFailed) g_offNext = true;
  {  // The first frames of a boost after scale 1 (the first-boost check, R32): each view's overlay records at the
     // true eye and at DCS's camera, and the learner's committed keys. Three onsets per session, 3 frames each.
    static double prevH = 0;
    static int onsets = 0, left = 0;
    if (!obs && f.h != 0 && prevH == 0 && onsets < 3) {
      ++onsets;
      left = 3;
    }
    if (left > 0) {
      --left;
      Log("cockpit ipd: boost onset %d, frame %llu (h %.3f mm): overlay records true eye / DCS's per view %d/%d %d/%d "
          "%d/%d %d/%d, replays moved %d %d %d %d",
          onsets, static_cast<unsigned long long>(g_frame), f.h * 1e3, f.v[0].cpuRecT, f.v[0].cpuRecB,
          f.v[1].cpuRecT, f.v[1].cpuRecB, f.v[2].cpuRecT, f.v[2].cpuRecB, f.v[3].cpuRecT, f.v[3].cpuRecB,
          f.v[0].cpuMoved, f.v[1].cpuMoved, f.v[2].cpuMoved, f.v[3].cpuMoved);
    }
    prevH = obs ? 0 : f.h;
  }
  g_st.frames++;
  if (f.anyPatched) g_st.framesPatched++;
  // Phase 2: every view whose G-buffer moved (Dw != 0) had its lighting split with that Dw.
  if (!obs && f.lighting) {
    for (int t = 0; t < 4; ++t) {
      const ViewFrame& v = f.v[t];
      if (!v.patched || v.failed || !NonZero(v.dw) || v.lit) continue;
      g_st.lightMiss++;
      if (g_lightMissLogs < 10) {
        ++g_lightMissLogs;
        Log("cockpit ipd: frame %llu view %d: G-buffer at the true eye, cockpit lit from the boosted eye (%s)",
            static_cast<unsigned long long>(g_frame), t, kLightWhyNames[v.lightWhy ? v.lightWhy : kLwNoPass]);
      }
    }
  }
  // R32: the frame stop's block step, the learner's commits, the pending resets (before the inert verdict). Not
  // at all on the d3d11 overlay path or with the option off (nothing of ovl_cpu.h runs there).
  if (PathCpu() && g_cfgMode.load(std::memory_order_relaxed) != 0 && ocpu::g_state.load() != ocpu::kStateNone)
    ocpu::EndFrame();
  if (++g_winFrames >= 300) {
    if (g_winAttempts >= 30 && g_winFails * 100 > g_winAttempts && !g_inertSelf) {
      g_inertSelf = true;
      Log("cockpit ipd: self-checks failed on %llu of %llu patch attempts in 300 frames; inert until reload",
          static_cast<unsigned long long>(g_winFails), static_cast<unsigned long long>(g_winAttempts));
    }
    if (g_ovlAttempts >= 30 && g_ovlFails * 100 > g_ovlAttempts && !g_ovlInert.load()) {
      g_ovlInert = true;
      Log("cockpit ipd: overlay passes failed on %llu of %llu attempts in 300 frames; overlays at DCS's eye until "
          "reload (the cockpit stays patched)",
          static_cast<unsigned long long>(g_ovlFails), static_cast<unsigned long long>(g_ovlAttempts));
    }
    g_winFrames = g_winAttempts = g_winFails = 0;
    g_ovlAttempts = g_ovlFails = 0;
  }
  if (g_logLeft.load(std::memory_order_relaxed) > 0) {
    TraceLine("frame %llu end: h %.3f mm (%s), scale %.3f, %s%s", static_cast<unsigned long long>(g_frame),
              f.h * 1e3, kPathNames[f.path], f.lo.any ? f.lo.rec.scale : 1.0f, f.off ? "off" : "on",
              f.anyPatched ? ", patched" : "");
    g_logLeft--;
  }
  OwnEndFrame();  // the ownship's moved views (the next frame's Dp), the recorder's job hint
  AckFrame(f);  // after this frame's refutation / inert verdicts
  DumpEndFrame();  // [Perception] CockpitTrueIpdFrameDump
  g_f = Frame{};
  ++g_frame;
}

// ---------------------------------------------------------------------------
// Phase 2: stencil-split lighting (render thread, inside GBufferLightingPass)
// ---------------------------------------------------------------------------

struct LightRes {
  ID3D11Resource* tex = nullptr;          // identity (held by dsv)
  ID3D11DepthStencilView* dsv = nullptr;  // read-only depth and stencil
  // ne / eq were derived from this description of DCS's bound state (compared by value: a released state whose
  // address is reused by another one cannot hand its derived states over).
  D3D11_DEPTH_STENCIL_DESC srcDesc{};
  bool haveSrc = false;
  ID3D11DepthStencilState* ne = nullptr;
  ID3D11DepthStencilState* eq = nullptr;
  ID3D11Buffer* cb = nullptr;  // private b7 (same size as DCS's)
  UINT cbSize = 0;
};
LightRes g_light[4];

void ReleaseLight() {
  for (LightRes& l : g_light) {
    if (l.dsv) l.dsv->Release();
    if (l.ne) l.ne->Release();
    if (l.eq) l.eq->Release();
    if (l.cb) l.cb->Release();
    l = LightRes{};
  }
}

// GBufferLightingPass begins: true when the draw entries (and uploads) should be armed.
// Only for a view whose G-buffer moved with Dw != 0 in this frame: the split
// uses that view's Dw / Dp (the same h), nothing else.
bool LightBegin(void* pass) {
  if (!WantUploads() || !g_f.lighting || g_f.observe || t_cls != kClsNone) return false;
  const int t = g_s.hmdOrd;
  if (t < 0 || t > 3 || g_f.off || !g_f.v[t].patched || g_f.v[t].failed || !NonZero(g_f.v[t].dw)) return false;
  if (!g_f.v[t].depthTex) {
    g_st.lightNoDepth++;
    g_f.v[t].lightWhy = kLwNoDepth;
    return false;
  }
  t_cls = kClsLight;
  t_view = t;
  t_pass = pass;
  t_lightDraws = t_lightSplits = 0;
  t_lightUploads = t_lightFwdN = 0;
  t_lightBusy = false;
  for (LightCopy& l : g_lightCopy) l.valid = false;
  g_st.lightArms++;
  return true;
}
bool LightOwns(void* pass) { return t_cls == kClsLight && t_pass == pass; }
void LightClose() {
  t_cls = kClsNone;
  t_view = -1;
  t_pass = nullptr;
  t_lightDraws = t_lightSplits = 0;
  t_lightUploads = t_lightFwdN = 0;
  t_lightBusy = false;
}
// The in-pass arm (OM / viewport / upload and draw entries) was not obtained.
void LightArmFailed() {
  if (t_cls == kClsLight && t_view >= 0 && t_view <= 3) g_f.v[t_view].lightWhy = kLwArmFail;
  g_st.lightArmFail++;
  LightClose();
}
// A draw of the open lighting arm reached the draw hooks (before it is split or forwarded).
void LightDrawSeen() {
  if (t_cls != kClsLight) return;
  ++t_lightDraws;
}
// Every cPerView upload of the open lighting arm went to the GPU at the true eye: whatever DCS draws with it
// (seen or not, split or not) lights the cockpit pixels from the camera the G-buffer was drawn with.
inline bool LightB7True() { return t_lightUploads > 0 && t_lightFwdN == t_lightUploads; }

DXGI_FORMAT DsvFormatOf(DXGI_FORMAT tex) {
  switch (tex) {
    case DXGI_FORMAT_R24G8_TYPELESS:
    case DXGI_FORMAT_D24_UNORM_S8_UINT:
      return DXGI_FORMAT_D24_UNORM_S8_UINT;
    case DXGI_FORMAT_R32G8X24_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
      return DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
    default:
      return DXGI_FORMAT_UNKNOWN;  // no stencil plane
  }
}

// DCS's pipeline state SplitDraw reads and changes, with the references it
// took (plain data: fixation_depth.h's SEH handler puts it back after a fault).
struct SplitCap {
  ID3D11DeviceContext* c = nullptr;
  ID3D11RenderTargetView* rtv[8] = {};
  UINT nrtv = 0;
  ID3D11DepthStencilView* dsv0 = nullptr;
  ID3D11DepthStencilState* dss0 = nullptr;
  UINT ref0 = 0;
  ID3D11Buffer* b7 = nullptr;  // PS b7
  UINT first = 0, num = 0;
  ID3D11Buffer* vs7 = nullptr;  // VS b7
  UINT vsFirst = 0, vsNum = 0;
  bool active = false;  // state changed: to be put back
  int cls = kClsNone;   // t_cls while SplitDraw runs
  int draws = 0;        // draws issued
};
inline bool CbOffsets(UINT first, UINT num) { return !(first == 0 && (num == 4096 || num == 0)); }
// Puts DCS's state back exactly (when changed) and releases the references. Idempotent.
void FinishSplit(SplitCap& k) {
  State& s = g_s;
  if (k.active && k.c && s.ctx1) {
    if (CbOffsets(k.first, k.num))
      s.ctx1->PSSetConstantBuffers1(7, 1, &k.b7, &k.first, &k.num);
    else
      k.c->PSSetConstantBuffers(7, 1, &k.b7);
    if (k.vs7 && k.vs7 == k.b7) {
      if (CbOffsets(k.vsFirst, k.vsNum))
        s.ctx1->VSSetConstantBuffers1(7, 1, &k.vs7, &k.vsFirst, &k.vsNum);
      else
        k.c->VSSetConstantBuffers(7, 1, &k.vs7);
    }
    k.c->OMSetRenderTargetsAndUnorderedAccessViews(k.nrtv, k.rtv, k.dsv0, k.nrtv, D3D11_KEEP_UNORDERED_ACCESS_VIEWS,
                                                  nullptr, nullptr);
    k.c->OMSetDepthStencilState(k.dss0, k.ref0);
    t_cls = k.cls;
  }
  k.active = false;
  t_lightBusy = false;
  for (ID3D11RenderTargetView*& r : k.rtv)
    if (r) r->Release(), r = nullptr;
  if (k.dsv0) k.dsv0->Release(), k.dsv0 = nullptr;
  if (k.dss0) k.dss0->Release(), k.dss0 = nullptr;
  if (k.b7) k.b7->Release(), k.b7 = nullptr;
  if (k.vs7) k.vs7->Release(), k.vs7 = nullptr;
}

// The split of one draw. drawFn issues DCS's draw on the context. True when
// the draw was issued here (twice); false: the caller issues it once as DCS's.
// The lighting pass's cPerView normally reached the GPU at the true eye
// (OnUploadImpl): draw 1 (world pixels, stencil NOT_EQUAL) binds a private b7
// with DCS's own bytes, draw 2 (cockpit pixels, EQUAL) DCS's bound one. When
// it did not (no restore entry), the other way round: draw 1 with DCS's b7,
// draw 2 with a private b7 patched with the view's Dw / Dp. `cap` holds what
// was changed until FinishSplit puts it back (also after a fault: the caller's
// handler calls it).
template <class DrawFn>
bool SplitDraw(ID3D11DeviceContext* c, UINT count, UINT start, DrawFn drawFn, SplitCap* cap = nullptr) {
  SplitCap own;  // callers without a fault handler of their own (tests)
  if (!cap) cap = &own;
  State& s = g_s;
  const int t = t_view;
  // Callers hold g_busy (Stop releases g_light only after draining it).
  if (!Active() || t_cls != kClsLight || t < 0 || t > 3 || c != s.ctx || !s.ctx1 || !s.dev) return false;
  ViewFrame& vf = g_f.v[t];
  SplitCap& k = *cap;
  k.c = c;
  k.cls = t_cls;
  auto refused = [&] {
    FinishSplit(k);
    g_st.lightSkip++;
    vf.lightWhy = kLwUnsplit;
    return false;
  };
  if (!(count == 3 || count == 4) || start != 0) return refused();
  D3D11_PRIMITIVE_TOPOLOGY topo = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
  c->IAGetPrimitiveTopology(&topo);
  // PS b7 a known cPerView, its lighting-pass bytes captured.
  s.ctx1->PSGetConstantBuffers1(7, 1, &k.b7, &k.first, &k.num);
  if (!k.b7) return refused();
  ID3D11Resource* b7r = k.b7;
  // The struct the lighting shader reads: b7's own offset (*SetConstantBuffers1).
  const bool psOffB = CbOffsets(k.first, k.num);
  const UINT psBase = psOffB ? k.first * 16 : 0;
  Known* kn = (!psOffB || k.num * 16 >= kCbBytes) ? FindKnown(b7r, psBase) : nullptr;
  const LightCopy* lc = nullptr;
  for (const LightCopy& l : g_lightCopy)
    if (l.valid && l.res == b7r && l.base == psBase) lc = &l;
  if (!kn || !lc) {
    g_st.lightNoCopy++;
    return refused();
  }
  // The depth texture: single-sample, with a stencil plane; RTV 0 of its size.
  ID3D11Texture2D* dt = nullptr;
  D3D11_TEXTURE2D_DESC td{};
  if (FAILED(vf.depthTex->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&dt))) || !dt)
    return refused();
  dt->GetDesc(&td);
  dt->Release();
  c->OMGetRenderTargets(8, k.rtv, &k.dsv0);
  for (UINT i = 0; i < 8; ++i)
    if (k.rtv[i]) k.nrtv = i + 1;
  if (!(td.SampleDesc.Count == 1 && DsvFormatOf(td.Format) != DXGI_FORMAT_UNKNOWN && k.rtv[0] && td.ArraySize == 1))
    return refused();
  {
    ID3D11Resource* rr = nullptr;
    k.rtv[0]->GetResource(&rr);
    ID3D11Texture2D* rt = nullptr;
    bool ok = false;
    if (rr && SUCCEEDED(rr->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&rt))) && rt) {
      D3D11_TEXTURE2D_DESC rd{};
      rt->GetDesc(&rd);
      rt->Release();
      ok = rd.Width == td.Width && rd.Height == td.Height && rd.SampleDesc.Count == 1;
    }
    if (rr) rr->Release();
    if (!ok) return refused();
  }
  // A PS SRV views the view's depth texture (the lighting reads the G-buffer depth).
  {
    ID3D11ShaderResourceView* srv[16] = {};
    c->PSGetShaderResources(0, 16, srv);
    bool reads = false;
    for (ID3D11ShaderResourceView* v : srv) {
      if (!v) continue;
      ID3D11Resource* r = nullptr;
      v->GetResource(&r);
      if (r == vf.depthTex) reads = true;
      if (r) r->Release();
      v->Release();
    }
    if (!reads) return refused();
  }
  // The bound depth-stencil state: no stencil use, no depth write; a depth
  // test only against this very depth texture.
  c->OMGetDepthStencilState(&k.dss0, &k.ref0);
  D3D11_DEPTH_STENCIL_DESC dd{};
  if (k.dss0)
    k.dss0->GetDesc(&dd);
  else
    dd = CD3D11_DEPTH_STENCIL_DESC(D3D11_DEFAULT);
  if (dd.StencilEnable || (dd.DepthEnable && dd.DepthWriteMask != D3D11_DEPTH_WRITE_MASK_ZERO)) return refused();
  if (dd.DepthEnable) {
    ID3D11Resource* dr = nullptr;
    if (k.dsv0) k.dsv0->GetResource(&dr);
    const bool same = dr == vf.depthTex;
    if (dr) dr->Release();
    if (!same) return refused();
  }
  LightRes& L = g_light[t];
  if (L.tex != vf.depthTex || !L.dsv) {
    if (L.dsv) L.dsv->Release();
    L.dsv = nullptr;
    D3D11_DEPTH_STENCIL_VIEW_DESC vd{};
    vd.Format = DsvFormatOf(td.Format);
    vd.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
    vd.Flags = D3D11_DSV_READ_ONLY_DEPTH | D3D11_DSV_READ_ONLY_STENCIL;
    const bool ok = SUCCEEDED(s.dev->CreateDepthStencilView(vf.depthTex, &vd, &L.dsv)) && L.dsv;
    L.tex = ok ? vf.depthTex : nullptr;
    if (!ok) return refused();
  }
  if (!L.ne || !L.eq || !L.haveSrc || memcmp(&L.srcDesc, &dd, sizeof(dd)) != 0) {
    if (L.ne) L.ne->Release();
    if (L.eq) L.eq->Release();
    L.ne = L.eq = nullptr;
    L.haveSrc = false;
    D3D11_DEPTH_STENCIL_DESC d = dd;
    d.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    d.StencilEnable = TRUE;
    d.StencilReadMask = 0x38;
    d.StencilWriteMask = 0;
    d.FrontFace = {D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP, D3D11_COMPARISON_NOT_EQUAL};
    d.BackFace = d.FrontFace;
    bool ok = SUCCEEDED(s.dev->CreateDepthStencilState(&d, &L.ne));
    d.FrontFace.StencilFunc = d.BackFace.StencilFunc = D3D11_COMPARISON_EQUAL;
    ok = ok && SUCCEEDED(s.dev->CreateDepthStencilState(&d, &L.eq));
    if (!ok) return refused();
    L.srcDesc = dd;
    L.haveSrc = true;
  }
  D3D11_BUFFER_DESC bd{};
  k.b7->GetDesc(&bd);
  if (!L.cb || L.cbSize != bd.ByteWidth) {
    if (L.cb) L.cb->Release();
    L.cb = nullptr;
    D3D11_BUFFER_DESC nd{};
    nd.ByteWidth = bd.ByteWidth;
    nd.Usage = D3D11_USAGE_DEFAULT;
    nd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    const bool ok = SUCCEEDED(s.dev->CreateBuffer(&nd, nullptr, &L.cb)) && L.cb;
    L.cbSize = ok ? bd.ByteWidth : 0;
    if (!ok) return refused();
  }
  if (kn->base + kCbBytes > L.cbSize || L.cbSize > kMaxSrc) return refused();
  // The private b7: DCS's lighting-pass bytes (draw 1) when its own upload went out at the true eye, else those
  // bytes patched with this frame's Dw / Dp (draw 2).
  const bool b7True = lc->fwdPatched;
  memset(g_scratch, 0, L.cbSize);
  memcpy(g_scratch + kn->base, lc->bytes, kCbBytes);
  if (!b7True) PatchStruct(g_scratch + kn->base, vf.dw, vf.dp);
  t_cls = kClsNone;  // our own upload passes the hook unchanged
  c->UpdateSubresource(L.cb, 0, nullptr, g_scratch, 0, 0);
  t_cls = k.cls;
  s.ctx1->VSGetConstantBuffers1(7, 1, &k.vs7, &k.vsFirst, &k.vsNum);
  auto bindB7 = [&](ID3D11Buffer* b) {
    if (psOffB)
      s.ctx1->PSSetConstantBuffers1(7, 1, &b, &k.first, &k.num);
    else
      c->PSSetConstantBuffers(7, 1, &b);
    if (k.vs7 && k.vs7 == k.b7) {
      if (CbOffsets(k.vsFirst, k.vsNum))
        s.ctx1->VSSetConstantBuffers1(7, 1, &b, &k.vsFirst, &k.vsNum);
      else
        c->VSSetConstantBuffers(7, 1, &b);
    }
  };
  k.active = true;
  t_lightBusy = true;  // our OM calls below pass the OM hooks unexamined
  c->OMSetRenderTargetsAndUnorderedAccessViews(k.nrtv, k.rtv, L.dsv, k.nrtv, D3D11_KEEP_UNORDERED_ACCESS_VIEWS, nullptr,
                                              nullptr);
  // Draw 1: world pixels, DCS's camera.
  if (b7True) bindB7(L.cb);
  c->OMSetDepthStencilState(L.ne, 0x28);
  drawFn();
  ++k.draws;
  // Draw 2: cockpit pixels, the true eye's camera.
  bindB7(b7True ? k.b7 : L.cb);
  c->OMSetDepthStencilState(L.eq, 0x28);
  drawFn();
  ++k.draws;
  FinishSplit(k);  // everything back exactly
  g_st.lightSplit++;
  ++t_lightSplits;
  vf.lit = true;  // this view's lighting used its own Dw / Dp (the G-buffer's)
  vf.lightWhy = kLwNone;
  g_lightCount = count;
  g_lightTopo = topo;
  return true;
}

// GBufferLightingPass end (render thread, callers hold g_busy): the arm's
// outcome. A draw that went past the hooks (d3d11 toggles the draw entries
// after a Flush / ExecuteCommandList) was drawn once with the bound b7: with
// the lighting cPerView at the true eye (the default) the cockpit pixels are
// lit from the camera of their G-buffer (world pixels from the true eye too: a
// translation of every reconstructed position by Dw, at most ~13 mm, for that
// view and frame); only a b7 that went out as DCS's leaves the cockpit lit from
// the boosted eye (counted, lightMiss). Nothing is drawn again here: the
// pass-end / frame-buffer-change repairs of 2026-10-13 never succeeded live
// (DCS unbinds the lighting draw's SRVs before either point) and are gone.
void LightEnd() {
  const int t = t_view;
  if (t_cls == kClsLight && t >= 0 && t <= 3 && Active()) {
    ViewFrame& vf = g_f.v[t];
    if (t_lightDraws > 1) {
      g_st.lightMulti++;
      g_lightMultiSeen = true;
    }
    const bool b7True = LightB7True();
    if (t_lightSplits == 0) {
      if (t_lightDraws == 0) g_st.lightNoDraw++;
      if (b7True) {
        vf.lit = true;  // DCS's own draw(s) with the true eye's b7
        vf.lightWhy = kLwNone;
        if (t_lightDraws == 0) g_st.lightNoDrawTrue++;
      } else if (!vf.lit) {
        if (t_lightDraws > 0) g_st.lightUnsplit++;
        vf.lightWhy = t_lightDraws == 0 ? kLwNoDraw : (vf.lightWhy == kLwNone ? kLwUnsplit : vf.lightWhy);
      }
    }
  }
  LightClose();
}

// ---------------------------------------------------------------------------
// [Perception] CockpitTrueIpdFrameDump=N (dev only, default 0): N consecutive
// frames of HMD views 2 and 3 (the focus views; [Perception]
// CockpitTrueIpdFrameDumpViews picks others: digits 0-3, m = DCS's desktop
// mirror target as bound at view 3's CopyToHMDPass begin, written as view 4), each as
// its own post-tonemap LDR image with the HUD / MFD symbology, copied at its CopyToHMDPass
// begin, plus one CSV row per view and frame with what cockpit ipd did to that view.
// The image of an HMD view is the render target its OverlayLDRPass (or GUIPass) bound
// inside the pass (fixation_depth.h's OM / viewport hooks, armed for these passes while a
// dump captures: DumpPassOpen / DumpOmBind / DumpVpSet), whatever cockpit ipd's overlays do
// (inert, off, test shift 0); never what is bound at the CopyToHMDPass begin: between passes DCS has its default frame buffer (the desktop
// mirror) bound, the same for every view (live 2026-10-14: views 1-3 dumped as the mirror,
// view 0 black, once the overlays were inert and no overlay arm recorded the target). A
// view-frame without a known target is not written (counted "no target").
// Validation in the CSV: the texture size, the box, src (where the image came from),
// dup_of (another view of the frame copied from the same texture box), the image's FNV-1a
// hash, mean and black flag (tools\flicker_check.py refuses a dump whose distinct views
// are byte-identical or black).
// Written to <log folder>\cockpit_frames\<date-time>\ as 24-bit BMP files and
// frames.csv; tools\flicker_check.py and tools\onoff_check.py analyse them.
// Cost: nothing while N is 0 (one relaxed load per CopyToHMDPass). While
// dumping: the OM / viewport hooks in the views' OverlayLDRPass / GUIPass (an arm of
// their own when no other feature holds one), one CopySubresourceRegion of the viewport
// box into a staging texture of an 8-slot ring per view and frame; each slot is mapped
// (DO_NOT_WAIT) at least 2 frames later and copied into memory, a worker
// thread converts and writes the files. Nothing DCS renders changes.
// ---------------------------------------------------------------------------

std::atomic<int> g_cfgFrameDump{0};  // Configure: a new value starts a dump of that many frames
std::atomic<int> g_dumpReq{0};       // frames requested (config thread), taken by the render thread
std::atomic<int> g_cfgDumpViews{0x0C};  // Configure: bit v = HMD view v (0-3), bit 4 = the desktop mirror
int g_dumpViews = 0x0C;                 // render thread: this dump's views (taken at its start)
std::mutex g_dumpDirMutex;
std::wstring g_dumpBase;             // <log folder>\cockpit_frames\ (Configure)
std::atomic<uint64_t> g_dumpWritten{0}, g_dumpDropped{0}, g_dumpSkipped{0};

struct DumpMeta {
  uint64_t frame = 0;
  int view = 0;
  double hMm = 0;
  int path = 0;
  bool carried = false, off = false, patched = false, tail = false, lit = false;
  int fwdN = 0, ovlN = 0, mvpN = 0, mvpRejN = 0, ovlLostN = 0, ovlDraws = 0, ovlDrawsUnseen = 0, ovlMaps = 0;
  double dwMm = 0;
  char passes[400] = {};  // the view's top-level passes after its G-buffer (names, with our overlay draws)
  UINT w = 0, h = 0;
  DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN;
  // Where the image came from (kDumpSrc*), the render targets the view's OverlayLDRPass / GUIPass bound, the
  // texture's size, the box's origin in it, another view of this frame captured from the same texture box (-1).
  int src = 0, binds = 0, dupOf = -1;
  UINT texW = 0, texH = 0, boxX = 0, boxY = 0;
  // Pixel scale (focal length in pixels of the captured box, 0 unknown) and Dw along the camera's right axis.
  double fxPx = 0, fyPx = 0, dwRightMm = 0;
  // The overlay cpu path (ViewFrame's cpu* fields) and the learner's committed keys T / B, overlays inert.
  int cpuWhy = -1, cpuReplays = 0, cpuMoved = 0, cpuRecs = 0, cpuRecT = 0, cpuRecB = 0, cpuFails = 0, cpuKeyFails = 0;
  int keysT = 0, keysB = 0;
  bool inert = false;
  // The view's camera (its cockpit G-buffer struct, DCS's own): FOV tangents (left, right, up, down) and the axes
  // (gViewInv rows 0..2, world). With them tools\onoff_check.py maps a pixel to the other eye's view at infinity
  // (the far world's exact disparity, not a guessed horizon). camOk false: not known (the columns stay empty).
  bool camOk = false;
  double fov[4] = {}, R[3][3] = {};
  int helmetN = 0;  // helmet-model runs drawn with their eye's camera (ViewFrame::helmetN)
};
// The image source of a dumped view: its OverlayLDRPass / GUIPass render target (bound inside the pass, seen by the
// OM hook), or the bound target at view 3's CopyToHMDPass begin (the desktop mirror, view 4 only).
enum : int { kDumpSrcNone = 0, kDumpSrcBind = 1, kDumpSrcMirror = 2 };
const char* const kDumpSrcNames[4] = {"none", "ldr_bind", "mirror", "?"};
// ovl_cpu.h's Why codes (the view's first replay), as the CSV writes them.
const char* const kCpuWhyNames[13] = {"moved",      "not_hmd", "outside",  "cockpit_off", "view_not_moved",
                                      "off_inert",  "reset",   "frame_stop", "no_filter", "observe",
                                      "latched",    "fail_cam", "fail_split"};
struct DumpSlot {
  ID3D11Texture2D* st = nullptr;
  UINT w = 0, h = 0;
  DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN;
  bool busy = false;
  uint64_t copiedAt = 0;  // g_frame of the copy
  DumpMeta meta;
};
constexpr int kDumpRing = 8;
DumpSlot g_dumpRing[kDumpRing];
int g_dumpFramesLeft = 0;     // render thread: frames still to capture
bool g_dumpActive = false;    // render thread: a dump runs (capturing or draining the ring)
char g_dumpPasses[4][400] = {};  // views 0-3: this frame's top-level pass names after the G-buffer (DumpNotePass)
// The view's render target as its OverlayLDRPass / GUIPass bound it (fixation_depth.h's OM hook while the pass
// runs): the last bound target other than the one bound at the pass begin (DCS's default frame buffer), with the
// first viewport set after that bind.
struct DumpBindTarget {
  ID3D11Resource* res = nullptr;  // reference
  D3D11_VIEWPORT vp{};
  bool vpSet = false;
  int binds = 0;  // binds of a target other than the previous one in the view's watched passes this frame
  uint64_t frame = ~0ull;
};
DumpBindTarget g_dumpBind[4];
int g_dumpPassView = -1;                    // render thread: the watched pass's HMD view (-1: none open)
ID3D11Resource* g_dumpPassBegin = nullptr;  // identity: the target bound at that pass's begin
bool g_dumpBindPending = false;             // a bind of the open pass waits for its viewport
int g_dumpBindLogs = 0;                     // per dump: the first binds logged (diagnostics)
// This frame's captured texture boxes per view (identity only), for dup_of.
struct DumpIdent {
  ID3D11Resource* res = nullptr;
  D3D11_BOX box{};
  uint64_t frame = ~0ull;
};
DumpIdent g_dumpIdent[5];
std::atomic<uint64_t> g_dumpNoTarget{0}, g_dumpDup{0}, g_dumpBlack{0};
uint64_t g_dumpSession = 0;   // render thread: frame of the dump's start (folder name)

struct DumpJob {
  DumpMeta meta;
  UINT rowPitch = 0;
  std::vector<uint8_t> px;  // h rows of rowPitch bytes
  std::wstring dir;
};
std::mutex g_dumpQMutex;
std::condition_variable g_dumpQCv;
std::condition_variable g_dumpQSpace;  // the worker took a job (a full queue has room again)
std::deque<DumpJob> g_dumpQ;
std::thread g_dumpWorker;
std::atomic<bool> g_dumpStop{false};
std::wstring g_dumpDir;  // render thread: this dump's folder (copied into each job)
constexpr size_t kDumpQMax = 64;  // ~16 MB per focus frame (2028 x 2004): at most ~1 GB queued
// A full queue makes the render thread wait for the worker (dev only, while dumping: DCS slows down, no image of
// the dump is lost; live 2026-10-14 the focus views' 12 MB images outran the worker and 386 of 537 were dropped).
// Dropped only when the worker makes no room for this long.
constexpr int kDumpQWaitMs = 10000;

inline float HalfToFloat(uint16_t h) {
  const uint32_t s = (h >> 15) & 1, e = (h >> 10) & 31, m = h & 1023;
  float v;
  if (e == 0)
    v = std::ldexp(static_cast<float>(m), -24);
  else if (e == 31)
    v = m ? 0.0f : 65504.0f;
  else
    v = std::ldexp(static_cast<float>(m + 1024), static_cast<int>(e) - 25);
  return s ? -v : v;
}
inline float SmallFloat(uint32_t bits, int mantBits) {  // R11G11B10's 6 / 5-bit-mantissa floats (no sign)
  const uint32_t e = bits >> mantBits, m = bits & ((1u << mantBits) - 1);
  if (e == 0) return std::ldexp(static_cast<float>(m), -14 - mantBits);
  if (e == 31) return 65504.0f;
  return std::ldexp(static_cast<float>(m + (1u << mantBits)), static_cast<int>(e) - 15 - mantBits);
}
inline uint8_t U8(float v) { return static_cast<uint8_t>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); }
// One texel of a supported format as 8-bit B, G, R; false: format not supported.
bool TexelBgr(DXGI_FORMAT f, const uint8_t* p, uint8_t out[3]) {
  switch (f) {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
      out[0] = p[2], out[1] = p[1], out[2] = p[0];
      return true;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8X8_TYPELESS:
    case DXGI_FORMAT_B8G8R8X8_UNORM:
    case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
      out[0] = p[0], out[1] = p[1], out[2] = p[2];
      return true;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:
    case DXGI_FORMAT_R10G10B10A2_UNORM: {
      uint32_t v;
      memcpy(&v, p, 4);
      out[2] = static_cast<uint8_t>((v & 1023) >> 2);
      out[1] = static_cast<uint8_t>(((v >> 10) & 1023) >> 2);
      out[0] = static_cast<uint8_t>(((v >> 20) & 1023) >> 2);
      return true;
    }
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
    case DXGI_FORMAT_R16G16B16A16_FLOAT: {
      uint16_t h[3];
      memcpy(h, p, 6);
      out[2] = U8(HalfToFloat(h[0])), out[1] = U8(HalfToFloat(h[1])), out[0] = U8(HalfToFloat(h[2]));
      return true;
    }
    case DXGI_FORMAT_R11G11B10_FLOAT: {
      uint32_t v;
      memcpy(&v, p, 4);
      out[2] = U8(SmallFloat(v & 2047, 6));
      out[1] = U8(SmallFloat((v >> 11) & 2047, 6));
      out[0] = U8(SmallFloat((v >> 22) & 1023, 5));
      return true;
    }
    default:
      return false;
  }
}
UINT TexelBytes(DXGI_FORMAT f) {
  switch (f) {
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
      return 8;
    default:
      return 4;
  }
}
// Worker: one BMP (24-bit, bottom-up) and its CSV row.
bool WriteDumpJob(const DumpJob& j) {
  const DumpMeta& m = j.meta;
  CreateDirectoryW(j.dir.c_str(), nullptr);
  wchar_t name[64];
  swprintf_s(name, L"f%06llu_v%d.bmp", static_cast<unsigned long long>(m.frame), m.view);
  const std::wstring path = j.dir + name;
  const UINT row = (m.w * 3 + 3) & ~3u;
  std::vector<uint8_t> img(static_cast<size_t>(row) * m.h, 0);
  const UINT tb = TexelBytes(m.fmt);
  const bool rgba8 = m.fmt == DXGI_FORMAT_R8G8B8A8_TYPELESS || m.fmt == DXGI_FORMAT_R8G8B8A8_UNORM ||
                     m.fmt == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
  for (UINT y = 0; y < m.h; ++y) {
    const uint8_t* srcRow = j.px.data() + static_cast<size_t>(y) * j.rowPitch;
    uint8_t* dst = img.data() + static_cast<size_t>(m.h - 1 - y) * row;
    if (rgba8) {  // the usual LDR format: a plain swizzle
      for (UINT x = 0; x < m.w; ++x) {
        dst[x * 3] = srcRow[x * 4 + 2];
        dst[x * 3 + 1] = srcRow[x * 4 + 1];
        dst[x * 3 + 2] = srcRow[x * 4];
      }
      continue;
    }
    for (UINT x = 0; x < m.w; ++x)
      if (!TexelBgr(m.fmt, srcRow + x * tb, dst + x * 3)) return false;
  }
  // Validation: FNV-1a 64 of the image bytes (as written, rows bottom-up with their padding), mean, black.
  uint64_t hash = 0xcbf29ce484222325ull;
  uint64_t sum = 0;
  uint8_t mx = 0;
  for (size_t i = 0; i < img.size(); ++i) {
    hash ^= img[i];
    hash *= 0x100000001b3ull;
  }
  for (UINT y = 0; y < m.h; ++y) {
    const uint8_t* r = img.data() + static_cast<size_t>(y) * row;
    for (UINT x = 0; x < m.w * 3; ++x) {
      sum += r[x];
      mx = std::max(mx, r[x]);
    }
  }
  const double mean = m.w && m.h ? static_cast<double>(sum) / (3.0 * m.w * m.h) : 0.0;
  const bool black = mx < 16 && mean < 1.0;
  if (black) g_dumpBlack++;
  FILE* f = nullptr;
  if (_wfopen_s(&f, path.c_str(), L"wb") || !f) return false;
  uint8_t hdr[54] = {'B', 'M'};
  auto put32 = [&](int at, uint32_t v) { memcpy(hdr + at, &v, 4); };
  put32(2, static_cast<uint32_t>(54 + img.size()));
  put32(10, 54);
  put32(14, 40);
  put32(18, m.w);
  put32(22, m.h);
  hdr[26] = 1;
  hdr[28] = 24;
  put32(34, static_cast<uint32_t>(img.size()));
  fwrite(hdr, 1, sizeof(hdr), f);
  fwrite(img.data(), 1, img.size(), f);
  fclose(f);
  // frames.csv (header written with the first row of a folder).
  const std::wstring csv = j.dir + L"frames.csv";
  const bool fresh = GetFileAttributesW(csv.c_str()) == INVALID_FILE_ATTRIBUTES;
  FILE* c = nullptr;
  if (!_wfopen_s(&c, csv.c_str(), L"a") && c) {
    if (fresh)
      fprintf(c, "frame,view,h_mm,dw_mm,path,carried,off,gbuffer,tail,lit,fwd_passes,overlay_passes,mvp_uploads,"
                 "mvp_refused_uploads,overlay_arm_lost,overlay_draws,overlay_draws_unuploaded,overlay_cb_maps,width,"
                 "height,format,src,binds,tex_w,tex_h,box_x,box_y,dup_of,hash,mean,black,fx_px,fy_px,dw_right_mm,"
                 "cpu_decision,cpu_replays,cpu_replays_moved,cpu_records,cpu_records_true,cpu_records_dcs,"
                 "cpu_failures,cpu_key_failures,keys_t,keys_b,overlays_inert,helmet_switches,fov_l,fov_r,fov_u,fov_d,r00,r01,r02,r10,"
                 "r11,r12,r20,r21,r22,file,passes\n");
    char cam[320] = ",,,,,,,,,,,,";  // 13 columns, empty when the camera is not known
    if (m.camOk)
      snprintf(cam, sizeof(cam), "%.7f,%.7f,%.7f,%.7f,%.8f,%.8f,%.8f,%.8f,%.8f,%.8f,%.8f,%.8f,%.8f", m.fov[0], m.fov[1],
               m.fov[2], m.fov[3], m.R[0][0], m.R[0][1], m.R[0][2], m.R[1][0], m.R[1][1], m.R[1][2], m.R[2][0],
               m.R[2][1], m.R[2][2]);
    fprintf(c,
            "%llu,%d,%.4f,%.4f,%s,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%u,%u,%d,%s,%d,%u,%u,%u,%u,%d,%016llx,%.3f,%d,"
            "%.2f,%.2f,%.4f,%s,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%s,f%06llu_v%d.bmp,%s\n",
            static_cast<unsigned long long>(m.frame), m.view, m.hMm, m.dwMm, kPathNames[m.path], m.carried, m.off,
            m.patched, m.tail, m.lit, m.fwdN, m.ovlN, m.mvpN, m.mvpRejN, m.ovlLostN, m.ovlDraws, m.ovlDrawsUnseen,
            m.ovlMaps, m.w, m.h, static_cast<int>(m.fmt), kDumpSrcNames[m.src & 3], m.binds, m.texW, m.texH, m.boxX,
            m.boxY, m.dupOf, static_cast<unsigned long long>(hash), mean, black ? 1 : 0, m.fxPx, m.fyPx, m.dwRightMm,
            m.cpuWhy >= 0 && m.cpuWhy < 13 ? kCpuWhyNames[m.cpuWhy] : "none", m.cpuReplays, m.cpuMoved, m.cpuRecs,
            m.cpuRecT, m.cpuRecB, m.cpuFails, m.cpuKeyFails, m.keysT, m.keysB, m.inert ? 1 : 0, m.helmetN, cam,
            static_cast<unsigned long long>(m.frame), m.view, m.passes);
    fclose(c);
  }
  return true;
}
void DumpWorker() {
  for (;;) {
    DumpJob j;
    {
      std::unique_lock<std::mutex> lock(g_dumpQMutex);
      g_dumpQCv.wait(lock, [] { return g_dumpStop.load() || !g_dumpQ.empty(); });
      if (g_dumpQ.empty()) return;  // stopping, nothing left
      j = std::move(g_dumpQ.front());
      g_dumpQ.pop_front();
    }
    g_dumpQSpace.notify_all();
    if (WriteDumpJob(j))
      g_dumpWritten++;
    else
      g_dumpDropped++;
  }
}
void DumpWorkerStop() {
  {
    std::lock_guard<std::mutex> lock(g_dumpQMutex);
    g_dumpStop = true;
  }
  g_dumpQCv.notify_all();
  if (g_dumpWorker.joinable()) g_dumpWorker.join();
  g_dumpStop = false;
}
// Render thread: slots copied at least 2 frames ago are read back (never waiting on the GPU).
void DumpPoll(bool force) {
  State& s = g_s;
  if (!s.ctx) return;
  for (DumpSlot& d : g_dumpRing) {
    if (!d.busy || (!force && g_frame - d.copiedAt < 2)) continue;
    D3D11_MAPPED_SUBRESOURCE m{};
    const HRESULT hr = s.ctx->Map(d.st, 0, D3D11_MAP_READ, force ? 0 : D3D11_MAP_FLAG_DO_NOT_WAIT, &m);
    if (hr == DXGI_ERROR_WAS_STILL_DRAWING) continue;
    d.busy = false;
    if (FAILED(hr)) {
      g_dumpDropped++;
      continue;
    }
    DumpJob j;
    j.meta = d.meta;
    j.rowPitch = d.w * TexelBytes(d.fmt);
    j.px.resize(static_cast<size_t>(j.rowPitch) * d.h);
    for (UINT y = 0; y < d.h; ++y)
      memcpy(j.px.data() + static_cast<size_t>(y) * j.rowPitch, static_cast<const uint8_t*>(m.pData) + y * m.RowPitch,
             j.rowPitch);
    s.ctx->Unmap(d.st, 0);
    j.dir = g_dumpDir;
    {
      std::unique_lock<std::mutex> lock(g_dumpQMutex);
      if (g_dumpQ.size() >= kDumpQMax && g_dumpWorker.joinable())
        g_dumpQSpace.wait_for(lock, std::chrono::milliseconds(kDumpQWaitMs),
                              [] { return g_dumpQ.size() < kDumpQMax || g_dumpStop.load(); });
      if (g_dumpQ.size() >= kDumpQMax) {
        g_dumpDropped++;
        continue;
      }
      g_dumpQ.push_back(std::move(j));
    }
    g_dumpQCv.notify_one();
  }
}
void ReleaseDumpRing() {
  for (DumpSlot& d : g_dumpRing) {
    if (d.st) d.st->Release();
    d = DumpSlot{};
  }
  for (DumpBindTarget& d : g_dumpBind) {
    if (d.res) d.res->Release();
    d = DumpBindTarget{};
  }
  for (DumpIdent& d : g_dumpIdent) d = DumpIdent{};
  g_dumpPassView = -1;
  g_dumpPassBegin = nullptr;
  g_dumpBindPending = false;
}
bool DumpWants(int view) { return view >= 0 && view <= 4 && (g_dumpViews >> view & 1); }
// fixation_depth.h, each top-level pass begin / end of an HMD view 0-3 while a dump captures: the pass names
// (and, at an overlay pass end, our draw count in it) for the CSV.
void DumpNotePass(int view, const char* name, bool begin) {
  if (!g_dumpActive || g_dumpFramesLeft <= 0 || view < 0 || view > 3 || !DumpWants(view)) return;
  char* b = g_dumpPasses[view];
  if (begin && strcmp(name, "GBufferLightingPass") == 0) b[0] = 0;  // from the lighting on: the late passes
  const size_t n = strlen(b);
  if (n + 40 >= sizeof(g_dumpPasses[0])) return;
  if (begin)
    snprintf(b + n, sizeof(g_dumpPasses[0]) - n, "%s%.24s", n ? " " : "", name);
  else if (t_cls == kClsOvl || IsOverlayName(name))
    snprintf(b + n, sizeof(g_dumpPasses[0]) - n, "(%d)", t_view == view ? g_f.v[view].ovlDraws : -1);
}
// fixation_depth.h, a top-level pass of HMD view `view` begins: true when it is one whose render target is the
// view's image (OverlayLDRPass, GUIPass) and a dump captures that view now. The caller arms the OM / viewport hooks
// (its own arm when none is held) and calls DumpPassOpen.
bool DumpStart();  // below
bool DumpPassWanted(int view, const char* name) {
  if (!g_dumpActive && (view != 0 || g_dumpReq.load(std::memory_order_relaxed) <= 0)) return false;
  if (!g_dumpActive && (strcmp(name, "OverlayLDRPass") != 0 || !DumpStart())) return false;
  return g_dumpActive && g_dumpFramesLeft > 0 && view >= 0 && view <= 3 && DumpWants(view) &&
         (strcmp(name, "OverlayLDRPass") == 0 || strcmp(name, "GUIPass") == 0);
}
void DumpPassOpen(int view) {
  g_dumpPassView = view;
  g_dumpBindPending = false;
  g_dumpPassBegin = nullptr;
  if (!g_s.ctx) return;
  ID3D11RenderTargetView* rtv = nullptr;
  g_s.ctx->OMGetRenderTargets(1, &rtv, nullptr);
  if (rtv) {
    rtv->GetResource(&g_dumpPassBegin);
    if (g_dumpPassBegin) g_dumpPassBegin->Release();  // identity only: DCS keeps it bound
    rtv->Release();
  }
}
void DumpPassClose() {
  g_dumpPassView = -1;
  g_dumpBindPending = false;
}
// fixation_depth.h's OMSetRenderTargets* hooks (DCS's context, before the call) while a watched pass is open.
void DumpOmBind(ID3D11RenderTargetView* rtv) {
  const int v = g_dumpPassView;
  if (v < 0 || v > 3) return;
  g_dumpBindPending = false;
  if (!rtv) return;
  ID3D11Resource* res = nullptr;
  rtv->GetResource(&res);
  if (!res) return;
  if (res == g_dumpPassBegin) {  // DCS's default frame buffer bound back: not the view's image
    res->Release();
    return;
  }
  DumpBindTarget& d = g_dumpBind[v];
  if (d.frame != g_frame) d.binds = 0;
  if (d.frame != g_frame || d.res != res) ++d.binds;
  if (g_dumpBindLogs < 24) {
    ++g_dumpBindLogs;
    ID3D11Texture2D* t = nullptr;
    D3D11_TEXTURE2D_DESC td{};
    if (SUCCEEDED(res->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&t))) && t) {
      t->GetDesc(&td);
      t->Release();
    }
    Log("cockpit ipd: frame dump: frame %llu HMD view %d binds render target %p (%ux%u, format %d) in its overlay "
        "pass",
        static_cast<unsigned long long>(g_frame), v, static_cast<void*>(res), td.Width, td.Height,
        static_cast<int>(td.Format));
  }
  if (d.res) d.res->Release();
  d.res = res;  // the reference GetResource took
  d.vp = D3D11_VIEWPORT{};
  d.vpSet = false;
  d.frame = g_frame;
  g_dumpBindPending = true;
}
// fixation_depth.h's RSSetViewports hook: the first viewport after a bind of the open watched pass.
void DumpVpSet(const D3D11_VIEWPORT& vp) {
  const int v = g_dumpPassView;
  if (v < 0 || v > 3 || !g_dumpBindPending) return;
  g_dumpBindPending = false;
  DumpBindTarget& d = g_dumpBind[v];
  if (d.frame != g_frame) return;
  d.vp = vp;
  d.vpSet = true;
}
// fixation_depth.h, at CopyToHMDPass begin of HMD view `view` (render thread).
// A requested dump starts (render thread: view 0's first watched pass, or a CopyToHMDPass begin). False: none.
bool DumpStart() {
  {
    const int req = g_dumpReq.exchange(0);
    if (req <= 0) return false;
    std::wstring base;
    {
      std::lock_guard<std::mutex> lock(g_dumpDirMutex);
      base = g_dumpBase;
    }
    if (base.empty()) return false;
    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t sub[64];
    swprintf_s(sub, L"%04u%02u%02u_%02u%02u%02u\\", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    CreateDirectoryW(base.c_str(), nullptr);
    g_dumpDir = base + sub;
    g_dumpFramesLeft = std::clamp(req, 1, 600);
    g_dumpActive = true;
    g_dumpActiveFlag = true;
    g_dumpFramesLeftFlag = g_dumpFramesLeft;
    g_dumpSession = g_frame;
    g_dumpViews = g_cfgDumpViews.load() & 0x1F;
    if (!g_dumpViews) g_dumpViews = 0x0C;
    g_dumpBindLogs = 0;
    if (!g_dumpWorker.joinable()) g_dumpWorker = std::thread([] { qvexit::RunVoid(&DumpWorker); });
    char vs[16] = {};
    int n = 0;
    for (int v = 0; v < 5; ++v)
      if (DumpWants(v)) vs[n++] = v == 4 ? 'm' : static_cast<char>('0' + v);
    Log("cockpit ipd: frame dump of %d frames (views %s, m = desktop mirror as view 4) to %ls", g_dumpFramesLeft, vs,
        g_dumpDir.c_str());
  }
  return true;
}
void DumpAtHmd(int view) {
  if (!g_dumpActive && !DumpStart()) return;
  DumpPoll(false);
  if (g_dumpFramesLeft <= 0 || view < 0 || view > 3) return;
  State& s = g_s;
  if (!s.ctx || !s.dev) return;
  if (DumpWants(4) && view == 3) {  // the mirror: bound at the focus views' CopyToHMDPass begin, captured now
    ID3D11RenderTargetView* rtv = nullptr;
    s.ctx->OMGetRenderTargets(1, &rtv, nullptr);
    if (rtv) {
      ID3D11Resource* r = nullptr;
      rtv->GetResource(&r);
      rtv->Release();
      DumpCapture(r, D3D11_VIEWPORT{}, 4, kDumpSrcMirror, 0);  // the whole target
      if (r) r->Release();
    }
  }
  if (!DumpWants(view)) return;
  // The view's own image: the target its OverlayLDRPass / GUIPass bound this frame. Never the target bound now
  // (DCS's default frame buffer: the desktop mirror, the same for every view).
  const DumpBindTarget& db = g_dumpBind[view];
  if (db.res && db.frame == g_frame) {
    DumpCapture(db.res, db.vpSet ? db.vp : D3D11_VIEWPORT{}, view, kDumpSrcBind, db.binds);
  } else {
    g_dumpNoTarget++;
    static int logged = 0;
    if (logged < 4) {
      ++logged;
      Log("cockpit ipd: frame dump: frame %llu HMD view %d: no render target of its overlay pass seen (not armed, or "
          "no OverlayLDRPass / GUIPass); not written",
          static_cast<unsigned long long>(g_frame), view);
    }
  }
}
// One capture of `res` (the viewport box of it) as view `view` (4: the mirror, with view 0's CSV fields).
void DumpCapture(ID3D11Resource* res, const D3D11_VIEWPORT& vp, int view, int src, int binds) {
  State& s = g_s;
  ID3D11Texture2D* tex = nullptr;
  if (res) res->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&tex));
  if (!tex) {
    g_dumpSkipped++;
    return;
  }
  D3D11_TEXTURE2D_DESC td{};
  tex->GetDesc(&td);
  uint8_t probe[3];
  const uint8_t zero8[8] = {};
  if (td.SampleDesc.Count != 1 || !TexelBgr(td.Format, zero8, probe)) {
    static bool logged = false;
    if (!logged) Log("cockpit ipd: frame dump: target format %d / %u samples not supported", static_cast<int>(td.Format),
                     td.SampleDesc.Count);
    logged = true;
    tex->Release();
    g_dumpSkipped++;
    return;
  }
  D3D11_BOX box{0, 0, 0, td.Width, td.Height, 1};
  if (vp.Width >= 16 && vp.Height >= 16) {
    box.left = static_cast<UINT>(std::clamp(vp.TopLeftX, 0.0f, static_cast<float>(td.Width)));
    box.top = static_cast<UINT>(std::clamp(vp.TopLeftY, 0.0f, static_cast<float>(td.Height)));
    box.right = static_cast<UINT>(std::clamp(vp.TopLeftX + vp.Width, 0.0f, static_cast<float>(td.Width)));
    box.bottom = static_cast<UINT>(std::clamp(vp.TopLeftY + vp.Height, 0.0f, static_cast<float>(td.Height)));
    if (box.right <= box.left + 8 || box.bottom <= box.top + 8) box = D3D11_BOX{0, 0, 0, td.Width, td.Height, 1};
  }
  const UINT w = box.right - box.left, h = box.bottom - box.top;
  DumpSlot* slot = nullptr;
  for (DumpSlot& d : g_dumpRing)
    if (!d.busy) {
      slot = &d;
      break;
    }
  if (!slot) {
    DumpPoll(true);  // all slots in flight: wait for the oldest (dev only, while dumping)
    for (DumpSlot& d : g_dumpRing)
      if (!d.busy) {
        slot = &d;
        break;
      }
  }
  if (!slot) {
    tex->Release();
    g_dumpSkipped++;
    return;
  }
  if (!slot->st || slot->w != w || slot->h != h || slot->fmt != td.Format) {
    if (slot->st) slot->st->Release();
    slot->st = nullptr;
    D3D11_TEXTURE2D_DESC sd{};
    sd.Width = w;
    sd.Height = h;
    sd.MipLevels = sd.ArraySize = 1;
    sd.Format = td.Format;
    sd.SampleDesc.Count = 1;
    sd.Usage = D3D11_USAGE_STAGING;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (FAILED(s.dev->CreateTexture2D(&sd, nullptr, &slot->st)) || !slot->st) {
      slot->st = nullptr;
      tex->Release();
      g_dumpSkipped++;
      return;
    }
    slot->w = w, slot->h = h, slot->fmt = td.Format;
  }
  s.ctx->CopySubresourceRegion(slot->st, 0, 0, 0, 0, tex, 0, &box);
  tex->Release();
  // Another view of this frame copied from the same texture box (identity: the resource and the box).
  int dupOf = -1;
  for (int o = 0; o < 5 && dupOf < 0; ++o) {
    const DumpIdent& id = g_dumpIdent[o];
    if (o != view && id.frame == g_frame && id.res == res && !memcmp(&id.box, &box, sizeof(box))) dupOf = o;
  }
  if (dupOf >= 0 && view < 4 && dupOf < 4) g_dumpDup++;
  g_dumpIdent[view] = DumpIdent{res, box, g_frame};
  slot->busy = true;
  slot->copiedAt = g_frame;
  DumpMeta& m = slot->meta;
  m = DumpMeta{};
  const Frame& f = g_f;
  const ViewFrame& vf = f.v[view < 4 ? view : 0];
  m.frame = g_frame;
  m.view = view;
  m.hMm = (vf.dwReady && !f.off ? vf.h : 0.0) * 1e3;
  m.dwMm = Norm3(vf.dw) * 1e3;
  m.path = f.path;
  m.carried = f.lo.carried;
  m.off = f.off;
  m.patched = vf.patched && !vf.tailOff;
  m.tail = vf.tailOff || (vf.patched && !NonZero(vf.dw));
  m.lit = vf.lit;
  m.fwdN = vf.fwdN, m.ovlN = vf.ovlN, m.mvpN = vf.mvpN, m.mvpRejN = vf.mvpRejN, m.ovlLostN = vf.ovlLostN;
  m.ovlDraws = vf.ovlDraws, m.ovlDrawsUnseen = vf.ovlDrawsUnseen, m.ovlMaps = vf.ovlMaps;
  m.src = src, m.binds = binds, m.dupOf = dupOf;
  m.texW = td.Width, m.texH = td.Height, m.boxX = box.left, m.boxY = box.top;
  if (view < 4 && vf.fovOk && vf.fov[1] > vf.fov[0] && vf.fov[2] > vf.fov[3]) {
    m.fxPx = w / (vf.fov[1] - vf.fov[0]);
    m.fyPx = h / (vf.fov[2] - vf.fov[3]);
  }
  if (view < 4) m.dwRightMm = (vf.dw[0] * vf.R[0][0] + vf.dw[1] * vf.R[0][1] + vf.dw[2] * vf.R[0][2]) * 1e3;
  if (view < 4) m.helmetN = vf.helmetN;
  if (view < 4 && vf.fovOk && vf.haveOrig) {
    m.camOk = true;
    memcpy(m.fov, vf.fov, sizeof(m.fov));
    memcpy(m.R, vf.R, sizeof(m.R));
  }
  m.cpuWhy = vf.cpuWhy, m.cpuReplays = vf.cpuReplays, m.cpuMoved = vf.cpuMoved, m.cpuRecs = vf.cpuRecs;
  m.cpuRecT = vf.cpuRecT, m.cpuRecB = vf.cpuRecB, m.cpuFails = vf.cpuFails, m.cpuKeyFails = vf.cpuKeyFails;
  m.keysT = ocpu::g_learn.keysT, m.keysB = ocpu::g_learn.keysB;
  m.inert = g_ovlInert.load(std::memory_order_relaxed);
  if (view < 4) {
    strncpy_s(m.passes, g_dumpPasses[view], _TRUNCATE);
    for (char* q = m.passes; *q; ++q)
      if (*q == ',') *q = ';';
    g_dumpPasses[view][0] = 0;  // the next frame's list
  }
  m.w = w, m.h = h, m.fmt = td.Format;
}
// At the frame end (render thread): one frame of the dump done; when all are, the ring is drained.
void DumpEndFrame() {
  if (!g_dumpActive) return;
  if (g_dumpFramesLeft > 0) --g_dumpFramesLeft;
  g_dumpFramesLeftFlag = g_dumpFramesLeft;
  DumpPoll(false);
  if (g_dumpFramesLeft > 0) return;
  bool any = false;
  for (const DumpSlot& d : g_dumpRing) any = any || d.busy;
  if (any && g_frame - g_dumpSession < 100000) return;  // drained over the next frames
  g_dumpActive = false;
  g_dumpActiveFlag = false;
  ReleaseDumpRing();
  Log("cockpit ipd: frame dump done (%llu images written so far, %llu dropped, %llu skipped, %llu view-frames "
      "without a target of their own, %llu copied from another view's texture box, %llu black)",
      static_cast<unsigned long long>(g_dumpWritten.load()), static_cast<unsigned long long>(g_dumpDropped.load()),
      static_cast<unsigned long long>(g_dumpSkipped.load()), static_cast<unsigned long long>(g_dumpNoTarget.load()),
      static_cast<unsigned long long>(g_dumpDup.load()), static_cast<unsigned long long>(g_dumpBlack.load()));
}

// ---------------------------------------------------------------------------
// Control (config thread)
// ---------------------------------------------------------------------------

bool g_refusedLogged = false, g_mismatchLogged = false, g_faultLoggedCk = false;
ULONGLONG g_lastStats = 0;

// From main.cpp each second (before fixdepth::Tick).
void Configure(int mode, bool lighting, bool forward, int logFrames, double shiftMm, bool foreignConfigured,
               int prevLogFrames = 0, bool overlays = true, bool overlayMvp = true, int frameDump = 0,
               int mvpDump = 0, const std::wstring& logDir = std::wstring(), bool overlayRestore = false,
               int frameDumpViews = 0x0C, int overlayPath = kOvlPathCpu) {
  g_cfgOvlRestore = overlayRestore;
  g_cfgOvlPath = overlayPath == kOvlPathD3d11 ? kOvlPathD3d11 : kOvlPathCpu;
  g_cfgDumpViews = frameDumpViews;
  g_cfgMode = mode == 1 || mode == 2 ? mode : 0;
  g_cfgLighting = lighting;
  g_cfgForward = forward;
  g_cfgOverlays = overlays;
  g_cfgOvlMvp = overlayMvp;
  g_cfgShiftMm = std::clamp(shiftMm, -40.0, 40.0);
  g_cfgForeign = foreignConfigured;
  if (mode && foreignConfigured && !g_refusedLogged) {
    g_refusedLogged = true;
    Log("cockpit ipd: refused: GPU pass timing, GPU dedup or the D3D meter is configured (their context-table hooks "
        "would chain with ours)");
  }
  if (!mode || !foreignConfigured) g_refusedLogged = false;
  static int lastLog = 0;
  if (logFrames != lastLog) {
    lastLog = logFrames;
    g_logLeft = std::clamp(logFrames, 0, 600);
  }
  static int lastPrevLog = 0;  // [Perception] CockpitTrueIpdPrevLog: a new value (re)starts the gPrev trace
  if (prevLogFrames != lastPrevLog) {
    lastPrevLog = prevLogFrames;
    g_prevLogLeft = std::clamp(prevLogFrames, 0, 150);
  }
  // [Perception] CockpitTrueIpdMvpDump / CockpitTrueIpdFrameDump: a new value (re)starts the dump.
  if (mvpDump != g_cfgMvpDump.load()) {
    g_cfgMvpDump = mvpDump;
    g_mvpDumpLeft = std::clamp(mvpDump, 0, 2000);
  }
  if (!logDir.empty()) {
    std::lock_guard<std::mutex> lock(g_dumpDirMutex);
    g_dumpBase = logDir + L"cockpit_frames\\";
  }
  if (frameDump != g_cfgFrameDump.load()) {
    g_cfgFrameDump = frameDump;
    g_dumpReq = std::clamp(frameDump, 0, 600);
  }
}

// True when the render thread left every path of ours (g_busy drained within 1 s).
bool ObserverOut() {
  gbpass::ObsFn me = &Observer;
  gbpass::g_obs.compare_exchange_strong(me, nullptr);
  for (int i = 0; i < 200 && g_busy.load(); ++i) Sleep(5);
  return g_busy.load() == 0;
}

// Starts the render-thread paths (fixation_depth.h's Install, or Tick retries).
bool Start() {
  if (g_on.load()) return true;
  if (!Wanted() || g_fault.load()) return false;
  if (gbpass::g_state == 0) {
    g_gbInstallCalls++;
    gbpass::Install();
  }
  if (gbpass::g_state < 0) {
    if (!g_mismatchLogged) Log("cockpit ipd: build mismatch (G-buffer pass execute not hooked); off");
    g_mismatchLogged = true;
    return false;
  }
  if (gbpass::g_state == 0) return false;  // GraphicsCore not loaded yet: retried
  ResetRender();
  InitHelmetVtbl();
  OpenBoost(true);
  OpenAck(true);  // StereoAck (CockpitTrueIpd 1 or 2 only: Start needs Wanted())
  gbpass::g_obs = &Observer;
  if (PathCpu()) ocpu::Install();  // R32 (retried each second while DCS's modules load)
  g_on = true;
  g_starts++;
  Log("cockpit ipd: on (%s; forward cockpit passes %s, lighting split %s, overlay passes %s, overlay path %s%s)",
      Observe() ? "observe: nothing modified" : "cockpit passes at the true eye distance",
      g_cfgForward.load() ? "on" : "off", g_cfgLighting.load() ? "on" : "off", g_cfgOverlays.load() ? "on" : "off",
      PathCpu() ? "cpu (DCS's MVP camera per record)" : "d3d11 (A/B: uploads rewritten, the HUD de-collimated)",
      !PathCpu() && g_cfgOverlays.load() && g_cfgOvlMvp.load() ? ", effect MVPs on" : "");
  return true;
}

// fixation_depth.h's Uninstall, after its boundary callback is drained.
// Clearing g_on, then draining g_busy (the observer, the upload and the
// split-draw hooks count themselves in it before they read g_on), makes the
// render thread leave every object below alone before it is released or reset.
void Stop() {
  const bool was = g_on.exchange(false);
  ocpu::Uninstall();  // the replay / filter slots back to DCS's, its hooks drained
  const bool drained = ObserverOut();
  CloseBoost();
  CloseAck(g_frame);  // flags 0 once, then unmapped
  if (drained) {
    ReleaseLight();
    ReleaseHelmet();
    ReleaseDumpRing();
    g_dumpActive = false;
    t_lightBusy = false;
    t_cls = kClsNone;
    t_capOnly = false;
    g_restN = 0;
  } else {
    // The render thread is still inside one of our paths (a driver wait in a split draw?): its D3D objects and
    // state are left alone (leaked on purpose) rather than released under it.
    Log("cockpit ipd: the render thread did not leave our hooks within 1 s at the stop; lighting objects kept");
  }
  DumpWorkerStop();  // the queued frame-dump files written first
  g_faultLoggedCk = false;
  if (was) Log("cockpit ipd: off");
}

uint64_t g_costFrames0 = 0;
void LogStats() {
  auto u = [](const std::atomic<uint64_t>& a) { return static_cast<unsigned long long>(a.load()); };
  {
    LARGE_INTEGER fq;
    QueryPerformanceFrequency(&fq);
    const uint64_t fr = g_st.frames.load(), nf = fr > g_costFrames0 ? fr - g_costFrames0 : 1;
    double us[kCostParts];
    for (int i = 0; i < kCostParts; ++i) us[i] = g_cost[i].exchange(0) * 1e6 / fq.QuadPart / nf;
    g_costFrames0 = fr;
    Log("cockpit ipd: render-thread cost per frame: uploads %.0f us, overlay draw hooks %.0f us, G-buffer observer "
        "%.0f us, pass boundaries %.0f us, cpu overlay path (replay / filter hooks, frame end) %.0f us (total %.0f "
        "us); MVP scans full %llu cached %llu",
        us[kCostUpload], us[kCostDraw], us[kCostObserver], us[kCostBoundary], us[kCostOvlCpu],
        us[kCostUpload] + us[kCostDraw] + us[kCostObserver] + us[kCostBoundary] + us[kCostOvlCpu],
        static_cast<unsigned long long>(g_mvpFull.load()),
        static_cast<unsigned long long>(g_mvpFast.load()));
  }
  const Matcher& m = g_match;
  Log("cockpit ipd: frames %llu patched %llu off(stale %llu scale1 %llu foreign %llu fault %llu inert %llu next %llu "
      "view0 %llu multi %llu big %llu) scale %.3f h %.2fmm k %.4f lag %d match same/fov/lag/test %llu/%llu/%llu/%llu "
      "(ambiguous step %llu: views 1-3 verified %llu, not %llu; while the stereo ack was active %llu) resid "
      "%.2f/%.2fmm bias %.2fmm; latches without a new record: carried %llu, late %llu, dry (h 0) %llu, unboosted by "
      "the measured eye separation (h 0) %llu, not measured %llu (separation vs |C1 - C0| agree %llu, differ %llu); "
      "ambiguous steps decided by it %llu, undecided %llu; records picked by it %llu (changed the lag's %llu, none %llu; "
      "deviation from |C1 - C0| EMA %.4f mm max %.4f mm); early records %llu, phase resyncs %llu; off frames "
      "with the gPrev tail %llu",
      u(g_st.frames), u(g_st.framesPatched), u(g_st.offStale), u(g_st.offScale1), u(g_st.offForeign),
      u(g_st.offFault), u(g_st.offInert), u(g_st.offNext), u(g_st.offView0), u(g_st.offMulti), u(g_st.tooBig),
      g_lastScale, g_lastH * 1e3, m.kEst, m.lag, static_cast<unsigned long long>(m.paths[kPathSame]),
      static_cast<unsigned long long>(m.paths[kPathFov]), static_cast<unsigned long long>(m.paths[kPathLag]),
      static_cast<unsigned long long>(m.paths[kPathTest]), u(g_st.pairAmb), u(g_st.pairAmbVerified),
      u(g_st.pairAmbUnverified), u(g_st.ambAckActive), m.residN ? m.residSum / m.residN : 0.0, m.residMax,
      m.bias * 1e3, u(g_st.carried), u(g_st.carriedLate), u(g_st.offDry), u(g_carriedUnboosted), u(g_carriedNoSep), u(g_cullAgree), u(g_cullDisagree), u(g_ambByCull),
      u(g_ambCullUndecided), u(g_pickMeas), u(g_pickChanged), u(g_pickNone), g_cullDevEma * 1e3, g_cullDevMax * 1e3,
      static_cast<unsigned long long>(m.early),
      static_cast<unsigned long long>(m.resyncs), u(g_st.offTail));
  Log("cockpit ipd: G-buffer execs %llu (cockpit %llu, not HMD %llu, other tag %llu, no identity %llu) passes "
      "gb/fwd %llu/%llu (forward passes %llu, not armed %llu, nested %llu, handed over %llu) uploads %llu patched %llu observed %llu tail %llu "
      "restored %llu; skipped: partialBox %llu sub %llu big %llu unknown %llu selfchk %llu (camera far %llu) no "
      "upload %llu ambiguous %llu restFull %llu fwdFail %llu (not armed %llu, arm lost %llu, unpatched %llu); stencil ok %llu mismatch %llu world40 %llu; prev VP ok "
      "%llu bad %llu; boost reads %llu torn %llu dry %llu, gaze %llu; lightSplit %llu lightSkip %llu (no copy %llu) "
      "arms %llu; uploads from another thread %llu",
      u(g_st.gbExecs), u(g_st.ckExecs), u(g_st.ckNotHmd), u(g_st.tagOther), u(g_st.idFail), u(g_st.gbPatched),
      u(g_st.fwdPatched), u(g_st.fwdPasses), u(g_st.fwdNoArm), u(g_st.fwdNested), u(g_st.fwdHandover), u(g_st.uploadsSeen), u(g_st.patched),
      u(g_st.observed), u(g_st.tail), u(g_st.restored), u(g_st.partialBox), u(g_st.subNz), u(g_st.bigSrc),
      u(g_st.unknownBuf), u(g_st.selfFail), u(g_st.camFar), u(g_st.failNoUpload), u(g_st.ambiguous),
      u(g_st.restFull), u(g_st.fwdFail), u(g_st.fwdFailNoArm), u(g_st.fwdFailArmLost), u(g_st.fwdFailUnpatched), u(g_st.stencilOk), u(g_st.stencilMismatch), u(g_st.stencilWorld),
      u(g_st.prevOk), u(g_st.prevBad), u(g_st.boostReads), u(g_st.boostTorn), u(g_st.offDry), u(g_st.gazeReads),
      u(g_st.lightSplit), u(g_st.lightSkip), u(g_st.lightNoCopy),
      u(g_st.lightArms), u(g_st.usOtherThread));
  Log("cockpit ipd: gPrevFrameViewProj: camera of its eye ok %llu bad %llu (reference %llu, no store %llu, no "
      "centre %llu), store mismatch %llu, gated %llu, frames off %llu (%s); reference explained by gOrigin delta "
      "k0/+1/-1/none %llu/%llu/%llu/%llu; gPrevFrameTransform = VPI*PVP %llu other %llu (alt map used %llu); "
      "recorded-only uploads %llu",
      u(g_st.relOk), u(g_st.relBad), u(g_st.relRef), u(g_st.relNoStore), u(g_st.relNoCentre), u(g_st.storeMis),
      u(g_st.prevGate), u(g_st.offPrevRel), g_relRefuted ? "refuted now" : "established", u(g_st.rebase[0]),
      u(g_st.rebase[1]), u(g_st.rebase[2]), u(g_st.rebase[3]), u(g_st.xfStd), u(g_st.xfOther), u(g_st.xfAlt),
      u(g_st.capOnly));
  Log("cockpit ipd: lighting per view-frame: G-buffer moved but cockpit lit from the boosted eye %llu (no depth "
      "%llu, arm not obtained %llu, draws not split with DCS's b7 %llu); lighting b7 forwarded at the true eye %llu, "
      "draws not seen %llu (with the true eye's b7 bound %llu); passes with > 1 draw %llu%s; overlays %s: passes "
      "%llu (not armed %llu, nested %llu, failed %llu, no cPerView upload %llu, upload entries lost %llu) cPerView "
      "uploads %llu patched %llu other camera %llu; probe of their other constant buffers %llu: MVP of the view's "
      "camera %llu, its VP %llu; stereo ack drops %llu (BoostOut %llu, health %llu, paused %llu)",
      u(g_st.lightMiss), u(g_st.lightNoDepth), u(g_st.lightArmFail), u(g_st.lightUnsplit), u(g_st.lightFwd),
      u(g_st.lightNoDraw), u(g_st.lightNoDrawTrue), u(g_st.lightMulti), g_lightMultiSeen ? " (seen)" : "",
      !g_cfgOverlays.load() ? "off" : g_ovlInert.load() ? "INERT" : "on", u(g_st.ovlPasses), u(g_st.ovlNoArm),
      u(g_st.ovlNested), u(g_st.ovlFail), u(g_st.ovlNoUpload), u(g_st.ovlArmLost), u(g_st.ovlUploads),
      u(g_st.ovlPatched), u(g_st.ovlOtherCam), u(g_st.ovlProbe), u(g_st.ovlProbeMvp), u(g_st.ovlProbeVp),
      u(g_st.ackDrops), u(g_st.ackDropBoost), u(g_st.ackDropHealth), u(g_st.ackDropPause));
  Log("cockpit ipd: helmet models %s: passes with helmet items %llu, items %llu (last anchored to the %s eye), "
      "switches %llu, refused %llu (view failed); helmet items not on the head %llu, head-centred %llu; materials "
      "named %d",
      g_cfgHelmet.load() ? "on" : "off", u(g_helmetLists), u(g_helmetItems),
      g_helmetLastAnchor.load() == 1 ? "right" : g_helmetLastAnchor.load() == 0 ? "left" : "-", u(g_helmetSwitches),
      u(g_helmetRefused), u(g_helmetFar), u(g_helmetHeadCentred), g_helmetMatN);
  Log("cockpit ipd: ownship %s: %s%s%s; passes moved %llu (transparent %llu), items %llu, switches %llu, no upload to "
      "switch %llu (from the view's cockpit struct %llu), re-uploads under a switch %llu, tail %llu; locks %llu unlocks "
      "%llu; recorder stepped aside %llu executions, jobs not started %llu; stage 2: requests %llu, recorded "
      "executions %llu (items %llu in %llu runs, %llu of them from command lists), stage-1 fallbacks %llu (outside the "
      "recorder's executions %llu, drawn stock anyway %llu, a ready job built for another instance %llu; no key %llu), "
      "unused %llu, idle switches %llu; switch-ins %llu, of a struct not bound at VS b7 then %llu (the bound one "
      "switched instead %llu); unlocks by a mission or slot event %llu, after a scene cut %llu",
      g_cfgOwnship.load() == 2 ? "stage 2" : g_cfgOwnship.load() ? "stage 1" : "off", g_ownLocked.load() ? "locked " : "not locked",
      g_ownLocked.load() ? g_own.path : "", g_ownLocked.load() ? "" : "", u(g_ownPasses), u(g_ownFwdPasses),
      u(g_ownItems), u(g_ownSwitches), u(g_ownNoSrc), u(g_ownFromCockpit), u(g_ownReupload), u(g_ownTail), u(g_ownLocks),
      u(g_ownUnlocks), u(gbrec::g_ownAsidePasses), u(gbrec::g_ownAsideJobsSkipped), u(g_own2Passes),
      u(gbrec::g_own2Live), u(gbrec::g_own2Items), u(gbrec::g_own2Runs), u(gbrec::g_own2RecItems),
      u(gbrec::g_own2Fallback), u(gbrec::g_own2FbWhy[gbrec::kOwnFbScope]), u(gbrec::g_own2FbWhy[gbrec::kOwnFbStock]),
      u(gbrec::g_own2FbWhy[gbrec::kOwnFbKey]), u(gbrec::g_own2NoKey), u(g_own2Unused), u(g_ownSwitchIdle),
      u(gbrec::g_ownSwIn), u(gbrec::g_ownSwBoundDiff), u(g_ownSwRetarget), u(g_ownEventUnlocks), u(g_ownCutUnlocks));
  Log("cockpit ipd: overlay MVPs %s: effect uploads examined %llu = patched %llu (%llu matrices, %llu with a "
      "refused one too) + observed %llu + no MVP of the view's camera %llu + only refused matrices %llu + restore "
      "full %llu + too big %llu; sub %llu; matrices refused %llu, patched far (> 100 km) %llu; overlay passes with "
      "none patched %llu; context-table toggles in our arms %llu (in overlay passes %llu), normalized %llu (at an arm "
      "%llu); overlay draw probe: passes %llu, draws %llu, upload entries lost at a draw %llu, draws with an effect "
      "buffer restored and not uploaded again %llu (passes %llu), upload entries found switched mid-pass %llu, "
      "constant-buffer Maps %llu; toggle points from another thread %llu; stereo ack held %llu frames (gaps "
      "bridged %llu)",
      !g_cfgOverlays.load() || !g_cfgOvlMvp.load() ? "off" : "on", u(g_st.mvpSeen), u(g_st.mvpPatched),
      u(g_st.mvpMatrices), u(g_st.mvpMixed), u(g_st.mvpObserved), u(g_st.mvpNone), u(g_st.mvpImplOnly),
      u(g_st.mvpRestFull), u(g_st.mvpBig), u(g_st.mvpSub), u(g_st.mvpRejMatrices), u(g_st.mvpFar),
      u(g_st.mvpPassNone), u(g_st.tblToggles), u(g_st.ovlToggles), u(g_st.tblNormalized), u(g_st.tblNormArm),
      u(g_st.ovlProbePasses), u(g_st.ovlProbeDraws), u(g_st.ovlDrawSlotLost), u(g_st.ovlStaleDraws),
      u(g_st.ovlStalePasses), u(g_st.ovlMidLoss), u(g_st.ovlMapCb), u(g_st.tpOtherThread), u(g_st.ackHeld),
      u(g_st.ackBridged));
  Log("cockpit ipd: toggle points: arms found normal-not-ours and repaired %llu; observer arms lost %llu; overlay "
      "passes armed at h 0 for the learner %llu",
      u(g_st.tpNormalRepair), u(g_st.ovlObsLost), u(g_st.ovlLearnPasses));
  ocpu::LogStats();
}

// DcsQvCull_DualCameraStatus (loader export; bits in loader_api.h and
// status_word.h's PackDualCamera). The payload's worker, about once per second.
struct DualRate {
  uint64_t last = 0;
  ULONGLONG lastMs = 0;
  bool have = false;
  uint64_t lastCk = 0;  // R62: g_dcCockpitFrames at the previous call
  bool haveCk = false;
};
DualRate g_dcRate;
uint64_t DualCameraStatusNow() {
  const int mode = g_cfgMode.load();
  if (mode != 1 && mode != 2) {
    g_dcRate = DualRate{};
    return 0;
  }
  // Frames patched in the last second (the delta since the previous call, per second).
  const uint64_t cur = g_st.framesPatched.load();
  const ULONGLONG nowMs = GetTickCount64();
  uint32_t perSec = 0;
  if (g_dcRate.have && cur >= g_dcRate.last)
    perSec = ::qvstatus::RatePerSecond(cur - g_dcRate.last, nowMs - g_dcRate.lastMs);
  g_dcRate.last = cur;
  g_dcRate.lastMs = nowMs;
  g_dcRate.have = true;
  // The render thread's last frame, when it ended less than 200 ms ago.
  LARGE_INTEGER fq;
  QueryPerformanceFrequency(&fq);
  const int64_t last = g_dcQpc.load(std::memory_order_acquire);
  const bool recent = g_on.load() && last != 0 && QpcNow() - last < fq.QuadPart / 5;
  const bool boostOk = recent && g_dcBoostOk.load(std::memory_order_relaxed);
  const double scale = boostOk ? g_dcScale.load(std::memory_order_relaxed) : 0.0;
  const bool paused = g_userOff.load();
  // R62: ON when the last frame or a frame of the last second was patched (the last frame alone flickered "no dual
  // camera" on the panel whenever the one sampled frame was gated: camera cut, gPrev check).
  const bool on = mode == 1 && recent && !paused && Active() &&
                  (g_dcPatched.load(std::memory_order_relaxed) || perSec > 0);
  const bool waiting = !boostOk || !(std::fabs(scale - 1.0) >= 1e-3);
  // R62: no cockpit pass in the last frame nor in the last second (external view): not a failure (status_word.h
  // bit 5); the panel warned "no dual camera" in amber for the whole external view.
  const uint64_t ckCur = g_dcCockpitFrames.load(std::memory_order_relaxed);
  const bool cockpitInSecond = g_dcRate.haveCk && ckCur != g_dcRate.lastCk;
  g_dcRate.lastCk = ckCur;
  g_dcRate.haveCk = true;
  const bool noCockpit = mode == 1 && recent && !paused && !g_dcCockpit.load(std::memory_order_relaxed) &&
                         !cockpitInSecond;
  return ::qvstatus::PackDualCamera(true, on, paused, waiting, mode == 2, scale, perSec, noCockpit);
}

// fixation_depth.h's Tick, each second while the option is wanted.
void TickCfg() {
  if (!g_on.load()) Start();
  // A fault (ours, or fixation_depth.h's) makes Active() false: nothing is
  // patched, split or classified any more. g_on stays set (and the gbpass
  // observer in) so the render thread still closes a pass that was open at the
  // fault (restore, disarm); Stop clears it at the uninstall.
  if ((g_fault.load() || ::fixdepth::g_fault.load()) && g_on.load() && !g_faultLoggedCk) {
    g_faultLoggedCk = true;
    Log(g_fault.load() ? "cockpit ipd: fault latched; off until the payload reloads"
                       : "cockpit ipd: off (fixation depth fault latched) until the payload reloads");
  }
  if (g_on.load() && Active()) OpenBoost();
  if (g_on.load()) OpenAck();  // retried while it could not be created
  // DCS's modules loaded, or hooks kept by an uninstall whose replay outlived the drain (taken back once it ended).
  if (g_on.load() && Active() && PathCpu() && ocpu::NeedsInstall()) ocpu::Install();
  std::vector<std::string> tl;
  {
    std::lock_guard<std::mutex> lock(g_traceMutex);
    tl.swap(g_trace);
  }
  for (const std::string& l : tl) Log("cockpit ipd trace: %s", l.c_str());
  const ULONGLONG now = GetTickCount64();
  if (g_on.load() && now - g_lastStats >= 10000) {
    if (g_lastStats) LogStats();
    g_lastStats = now;
  }
}

}  // namespace ckipd
