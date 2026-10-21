// DCS Control: symbology emission companion export v1 (R89, Dev's answers and decisions folded in; DRAFT, not in the
// repo yet, nothing shipped), the contract between DCS Control's engine module (DcsQvCull, the writer) and a
// frame-generation layer (OFXR, the reader). Design and rationale: docs/research/R89_hud_layer_fg.md (§4-6).
//
//   Local\DcsVrSymbology_v1   a COMPANION of the cockpit mask (dcsvr_cockpit_mask.h), like the depth and motion
//                             companions: written in the same xrEndFrame commit, in the slot of the same index, with
//                             seq == the mask slot's maskSeq, covered by the same single fence Signal, same generation.
//                             [Perception] CockpitSymbologyExport=0 (default); requires CockpitMaskExport=1; produced
//                             only while a reader stores its heartbeat in this block AND in the mask block (R67).
//
// What it holds. Per HMD view v (0 left periphery, 1 right periphery, 2 left focus, 3 right focus; all four published),
// a CAPACITY WINDOW of that view's DCS output image (the post-DLSS image DCS copies into its swapchain, e.g. 1040 x 1028
// periphery, 2028 x 2004 focus), stored in a fixed sub-rect of two atlases:
//  - Emission atlas, R16G16B16A16_FLOAT (`format`):
//      rgb  E = dec(F) - dec(C), signed, in LINEAR light of an sRGB decode of the swapchain bytes. F = the bytes DCS
//           submitted for the view; C = the bytes DCS would have submitted without the included symbology draws (the
//           tone map evaluated by the writer on the image before them; post-tone-map draws removed by a snapshot).
//           So, per view, inside the window: dec(F) - E == dec(C) to the byte. DCS writes 2.2-gamma bytes into an
//           R8G8B8A8_UNORM_SRGB swapchain; Quad Views decodes them as sRGB and composes in linear light.
//      a    coverage alpha, valid only with emission_flags::alpha_valid ("coverage alpha valid"). The writer may set it and latches it off for the rest of the generation after a failed check (R97), so it can change within a generation; a == 0 where invalid.
//  - Class atlas, R8_UINT (`classFormat`), the same sub-rects: per texel the class of the symbology that produced E.
//
// Capacity windows (Dev's decision: fixed capacities, no full-view rects). view[v] has a FIXED atlas sub-rect (x, y,
// capW, capH), constant per generation (default capacities: periphery 512 x 512, focus 1024 x 1024; R89 §6: sized from
// the measured F-4E reticle and the HUD / helmet fields of view, refined by the live trace). Per frame the record gives
// the window (winX, winY, winW, winH) in THAT VIEW'S output pixels, winW <= capW, winH <= capH, inside the view:
//     view pixel (winX + i, winY + j)  <->  atlas texel (x + i, y + j),   0 <= i < winW, 0 <= j < winH.
// The window is the view's symbology bbox (Dev's item 7), chosen by the writer on the CPU before the view's tone map:
// collimated symbology from its learnt direction cone in the tracking space projected with this frame's pose and FOV,
// helmet symbology from its learnt view-pixel extent, plus a margin. Inside the window, texels without included
// symbology hold E = 0 and class 0 (the window is cleared each frame). If the predicted extent exceeds the capacity, the
// view gets present = 0 and view_flags::overflow, and the reader does nothing special for it (today's behaviour). A
// symbol pixel that falls outside the window anyway is not in E (it stays in the image, as today); the writer counts
// those texels on the GPU (late, diagnostics only) and widens its margin.
//
// Included draws (v1). Only symbology DCS declares COLLIMATOR (HUD, HUD repeaters, gunsights: class 1) or HELMET
// (helmet displays: class 2), identified per draw by DCS's own indicator type in the cockpit record replay (record byte
// +0xc9; R32 / ovl_cpu.h's render point), in OverlayHDRPass (pre-tone-map) and, with emission_flags::ldr_overlay,
// OverlayLDRPass (post-tone-map: the A-10C Scorpion HMCS). COMMON indicators (MFD / DED / panel displays on cockpit
// surfaces) are EXCLUDED FROM E, not only labelled: the writer attributes each change of the overlay pass to the replay
// segment that drew it and never accumulates a COMMON segment into E (fallback: E zeroed under the cockpit mask,
// view_flags::mfd_by_mask). GUIPass content (messages, labels, menus) is never in E in v1.
//
// Classes: 0 none (E == 0 there), 1 collimated, 2 head-locked (helmet). 3 and 4 are reserved (v1: never written).
//
// CONSUMER RULE (v1, agreed with Dev; OFXR is the only consumer):
//  1. Use E only where the view's view_flags has valid AND present, and only inside its window; present == 0 (also with
//     overflow) -> skip the view.
//  2. Un-blend: subtract the composed E (see "Composition") from each REAL frame before flow / synthesis.
//  3. Re-add into a synthetic frame E of the NEAREST real frame (in time) only, never a cross-fade of two frames.
//  4. Reproject by class: 1 collimated = rotation-only at infinity in the tracking (cockpit) space, i.e. the head
//     rotation between that real frame's pose and the synthetic pose, no depth, no world motion; 2 head-locked = no
//     motion in the view (screen-locked); any other class or texel is not touched (it stays part of the image).
//  5. Re-composite: synthetic = W' + E (v1, a == 0). With alpha_valid (later): W' + E + a (C - W'), C = dec(F) - E at the
//     symbol's source texel (recomposite() below).
//
// Composition into the stereo eye image (the reader sees only Quad Views' composed stereo images; layer order
// pupil_shift -> quad_views -> OFXR). Quad Views draws, per eye e, the periphery view e over the whole eye image and the
// focus view 2 + e inside a rect that follows the gaze every frame, blended at its edge; E is composed the same way:
//    E_eye(u, v) = a_f(u, v) * E_focus(focus uv) + (1 - a_f(u, v)) * E_periphery(periphery uv)
//  - FOVs. Quad Views composes with the ORIGINAL FOVs (the pupil shift layer substitutes shifted FOVs for DCS to render
//    with and restores the originals in xrEndFrame), so every tangent this block publishes for composition (the views'
//    fovTan, the focus layout's eyeFovTan / focusFovTan / rectNorm) is in the FOV source given by header.fovSource and
//    slot.focus.flags: fov_source_pupil_original (1) = the pupil shift layer's original FOVs (exact for Quad Views'
//    composition); fov_source_dcs_submitted (0) = DCS's submitted FOVs (exact when pupil shift is off; with pupil shift
//    on they are the shifted ones and the composition is off by the shift). focus_rect() MUST be given the original
//    FOVs when available. Where E's texels came from (the image DCS rendered with the shifted FOV) does not matter:
//    Quad Views stretches that image over the original FOV, and E with it.
//    The writer reads Local\DcsVrPupilFov_v1 (dcsvr_pupil_fov.h, Dev's layout; keyed by display time: the substituted
//    FOV pairs per view) in its xrEndFrame commit and publishes the originals for that frame's displayTime with
//    fov_source_pupil_original; without the block or a matching record it publishes fov_source_dcs_submitted (R95).
//  - The rect: slot.focus.eye[e].rectNorm = (u0, v0, u1, v1), NORMALISED [0, 1] coordinates of the stereo eye image, u
//    right, v down (multiply by the eye image's width / height). Derived per frame from the FOV of view e (the
//    periphery: the stereo view's FOV, Quad Views' m_cachedEyeFov) and of view 2 + e (the focus), by tangents:
//    u = (tan(x) - tan(eyeLeft)) / (tan(eyeRight) - tan(eyeLeft)), v = (tan(eyeUp) - tan(y)) / (tan(eyeUp) - tan(eyeDown)).
//    The tangents are in eyeFovTan / focusFovTan (left, right, up, down; x right, y up), so a reader whose stereo FOV
//    differs can recompute the rect against its own (focus_rect). The focus view shares its eye's orientation in Quad
//    Views (R65 §2.5). Per frame (paired with the slot); layout_valid clear -> no composition this frame (skip).
//  - Periphery uv / focus uv of an eye pixel: by tangent over the view's fovTan (focus uv = ((u - u0) / (u1 - u0),
//    (v - v0) / (v1 - v0))); view pixel = uv * (outW, outH); then the window and the atlas sub-rect as above.
//  - a_f, Quad Views' focus blend (ProjectionPS.hlsl), from qv.smoothenFocusViewEdges (s) and qv.focusViewShape (k), in
//    the focus view's NDC n = 2 * focus uv - 1 (|n| < 1 inside):
//      k >= 1: r = (|n.x|^k + |n.y|^k)^(1/k); a_f = (r < 1) * (1 - smoothstep(1 - clamp(2 s, 0.01, 0.95), 1, r));
//      k < 1 and s > 0: t = smoothstep(0, s, uv) - smoothstep(1 - s, 1, uv) per axis; a_f = inside * max(0.5, t.x * t.y);
//      s == 0: a_f = inside.
//  - Sharpening: Quad Views sharpens the focus view with CAS (qv.sharpenFocusView, 0..1) before composing, and with
//    dcsvr_periphery_sharpen (qv.peripherySharpen) the periphery; E is NOT sharpened. v1: accept the residual (Dev's
//    option b, measured in the trace); a reader may apply the same CAS to E over the window (option a).
//  - qv.flags tells when Quad Views does more than this (far depth warp, periphery temporal stability): then the
//    composition above is approximate (see qv_flags).
//
// Pairing, synchronisation, staleness: the mask's (dcsvr_cockpit_mask.h): find_slot by displayTime, seq == maskSeq of
// the same slot index, generation == the mask's, one fence (fenceHandle == the mask block's; queue Wait(fence, seq)
// before reading). Every E / class / window / layout write of a frame precedes that Signal (E of a view is finalised at
// its CopyToHMDPass begin; the layout at the commit). Three slots: the two real frames that bracket a synthetic one and
// the one being written. Reader heartbeat at offset 2040 (R67 rules, this block AND the mask's).
//
// Generation and held set (R85 semantics, as the other companions). The atlases, the class atlases and their NT handles
// belong to the export's shared set: switching CockpitSymbologyExport (or another export) off keeps them alive and the
// block's last record keeps the generation and the handles with header_flags::active clear; on again the SAME
// generation and handles are published. A new generation (objects released, old handles open >= 2 s) only on: a layout
// change (a capacity, the views, the companion wanted while the held set lacks it: once per session; an output size or
// DLSS mode change only when it changes a capacity or the atlas), a device change or removal, the payload stop or
// reload, a builder fault (R115 H2). A latched symbology fault only turns the export off (inactive, no view present) until the next install, without a new generation (R115 S2). On such a release the writer publishes R76's record (a new non-zero generation, flags 0, no
// handles) first, and RetireUnmapped writes it into this block too when the block is still mapped by a reader while the
// writer does not map it (export off). A reader keeps its opened set across an inactive stretch with an unchanged
// generation and drops it on a generation change.
//
// Quad Views settings (header.qv). Read by the writer's config thread from the same files Quad Views reads, in its
// order: <Quad Views layer dir>\settings.cfg, then %DCSVR_QUAD_SETTINGS% (or %LocalAppData%\Quad-Views-Foveated\
// settings.cfg without it), with Quad Views' section rules ([app:..], [exe:..], [runtime / system name]); re-read when
// a file changes (at most once per second); qv.settingsGen counts changes. Keys: sharpen_focus_view (default 0.7),
// dcsvr_periphery_sharpen (0), smoothen_focus_view_edges (0.2, clamped 0..0.5), focus_view_shape (0, clamped 0..16),
// dcsvr_far_depth, dcsvr_temporal, dcsvr_sharpen_taper, debug_keys. Quad Views' debug keys can change sharpening and
// smoothing at runtime without a file change: qv_flags::debug_keys says the values may be stale.
//
// Self-contained: <atomic>, <cmath>, <cstddef>, <cstdint>, <cstring>. #pragma pack(8), little-endian. The name takes the
// optional suffix of the environment variable DCSVR_PERCEPTION_SHM_SUFFIX (as the Perception blocks; tests use it).
#pragma once

#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace dcsvr_symbology {

constexpr const wchar_t* block_name = L"Local\\DcsVrSymbology_v1";
constexpr const char* name_suffix_variable = "DCSVR_PERCEPTION_SHM_SUFFIX";
constexpr std::uint32_t block_magic = 0x59534344u;  // "DCSY"
constexpr std::uint32_t block_version = 1;
constexpr std::uint32_t contract_revision = 1;
constexpr int max_views = 4;   // all four HMD views are published
constexpr int eye_count = 2;
constexpr int slot_count = 3;  // the two real frames bracketing a synthetic one + the one being written
constexpr std::uint32_t format_rgba16f = 10;   // DXGI_FORMAT_R16G16B16A16_FLOAT
constexpr std::uint32_t format_r8_uint = 62;   // DXGI_FORMAT_R8_UINT
constexpr std::uint8_t class_none = 0, class_collimated = 1, class_head_locked = 2;  // 3, 4 reserved (never in v1)
constexpr std::int64_t reader_heartbeat_period_ms = 1000;
constexpr std::int64_t reader_heartbeat_timeout_ms = 2000;
// The writer's default capacities (R89 §6) and atlas packing: the two periphery windows stacked in one column, the two
// focus windows beside it: v0 (0, 0), v1 (0, 512), v2 (512, 0), v3 (1536, 0); atlas 2560 x 1024, no unused texel.
// Per slot 2560 x 1024 x (8 + 1) bytes = 23.6 MB; 3 slots = 70.8 MB (10^6 bytes; 67.5 MiB). A reader takes the actual
// values from the header and the view records, never from these constants.
constexpr std::uint32_t default_capacity_periphery = 512;
constexpr std::uint32_t default_capacity_focus = 1024;
constexpr std::uint32_t default_atlas_w = 2560, default_atlas_h = 1024;
constexpr std::uint32_t fov_source_dcs_submitted = 0;  // DCS's submitted XrFovf (exact when pupil shift is off)
constexpr std::uint32_t fov_source_pupil_original = 1; // the pupil shift layer's original FOVs (Local\DcsVrPupilFov_v1)

#pragma pack(push, 8)
struct EmissionView {             // 72 bytes
    std::uint32_t flags;          // view_flags
    std::uint32_t x, y;           // the view's FIXED atlas sub-rect origin (constant per generation)
    std::uint32_t capW, capH;     // its capacity (constant per generation)
    std::uint32_t outW, outH;     // DCS's output viewport of the view (post-DLSS)
    std::uint32_t winX, winY;     // this frame's window origin in the VIEW's output pixels
    std::uint32_t winW, winH;     // this frame's window size (<= capW / capH; 0 when not present)
    float fovTan[4];              // left, right, up, down tangents of the view's FOV (header.fovSource / layout flags)
    std::uint32_t reserved[3];    // 0
};

struct EyeFocus {                 // 48 bytes: where Quad Views puts focus view 2 + e inside stereo eye e this frame
    float rectNorm[4];            // u0, v0, u1, v1: normalised [0, 1] stereo-eye-image coordinates (u right, v down)
    float eyeFovTan[4];           // the periphery view e's FOV tangents (left, right, up, down)
    float focusFovTan[4];         // the focus view 2 + e's FOV tangents
};

struct FocusLayout {              // 104 bytes
    EyeFocus eye[eye_count];
    std::uint32_t flags;          // layout_flags
    std::uint32_t fovSource;      // this frame's FOV source (fov_source_*)
};

struct EmissionSlot {             // 32 + 4 * 72 + 104 = 424 bytes
    std::uint64_t seq;            // == the mask slot's maskSeq (fence value); 0 never written
    std::uint64_t engineFrame;
    std::int64_t displayTime;     // XrFrameEndInfo::displayTime of this frame (slot_flags::display_time)
    std::uint32_t flags;          // slot_flags
    std::uint32_t viewMask;       // bit v: view[v] valid
    EmissionView view[max_views];
    FocusLayout focus;            // per frame (the focus follows the gaze)
};

struct QvSettings {               // 24 bytes: Quad Views' composition settings as read from its configuration
    float sharpenFocusView;       // sharpen_focus_view: CAS amount on the focus view, 0..1 (0 = off)
    float peripherySharpen;       // dcsvr_periphery_sharpen: CAS amount on the periphery, 0..1 (0 = off)
    float smoothenFocusViewEdges; // smoothen_focus_view_edges, 0..0.5 (the edge blend, see "Composition")
    float focusViewShape;         // focus_view_shape, 0..16 (< 1: rectangular focus; >= 1: superellipse exponent)
    std::uint32_t flags;          // qv_flags
    std::uint32_t settingsGen;    // increments when a re-read changed any value
};

struct Header {                   // 2048 bytes
    std::uint64_t seq;            // seqlock (dcsvr_perception_shm.h)
    std::uint32_t magic, version, structSize, flags;
    std::uint32_t generation;     // == the mask header's generation
    std::uint32_t slotCount, format, atlasW, atlasH, ownerPid;
    std::uint64_t adapterLuid;
    std::uint64_t textureHandle[slot_count];       // emission atlases (NT handles of the DCS process)
    std::uint64_t fenceHandle;                     // == the mask block's fenceHandle (shares_mask_fence)
    std::uint64_t newestSeq;
    std::int64_t heartbeatQpc, qpcFrequency;
    std::uint32_t newestSlot, revision;
    std::uint32_t classFormat, emissionFlags;      // format_r8_uint; emission_flags (constant per generation, except alpha_valid)
    std::uint64_t classTextureHandle[slot_count];  // class atlases (NT handles)
    QvSettings qv;                                 // offset 152
    std::uint32_t fovSource;                       // offset 176: fov_source_* the writer currently publishes
    std::uint32_t reserved1;                       // 0
    EmissionSlot slot[slot_count];                 // offset 184
    std::uint64_t reserved2[73];                   // 0
    std::uint64_t readerHeartbeatQpc;              // offset 2040, written by the READER only (R67 rules)
};
#pragma pack(pop)

namespace header_flags {
constexpr std::uint32_t active = 1u;
constexpr std::uint32_t fence = 2u;
constexpr std::uint32_t reader_heartbeat = 8u;
constexpr std::uint32_t idle = 16u;               // no fresh heartbeat (this block's or the mask's): no new slots
constexpr std::uint32_t shares_mask_fence = 32u;
constexpr std::uint32_t class_atlas = 64u;
constexpr std::uint32_t qv_settings = 128u;       // header.qv was read (else all 0: Quad Views' settings unknown)
}  // namespace header_flags
namespace emission_flags {                        // what this writer can produce (constant per generation, except alpha_valid)
constexpr std::uint32_t hdr_overlay = 1u;         // OverlayHDRPass content (F-16 / F/A-18 / A-10C HUD, F-4E sight,
                                                  // F-16 / F/A-18 helmet displays)
constexpr std::uint32_t ldr_overlay = 2u;         // reserved: never produced (OverlayLDRPass content, e.g. the A-10C Scorpion HMCS)
constexpr std::uint32_t alpha_valid = 8u;         // "coverage alpha valid": E.a is the symbol coverage; may change within a generation (R97 latch)
constexpr std::uint32_t class_collimated = 16u;
constexpr std::uint32_t class_head_locked = 32u;
constexpr std::uint32_t common_excluded = 64u;    // COMMON (MFD / DED) draws are excluded from E (always in v1)
}  // namespace emission_flags
namespace slot_flags {
constexpr std::uint32_t valid = 1u;
constexpr std::uint32_t display_time = 2u;
constexpr std::uint32_t late_commit = 4u;         // no xrEndFrame hook: not pairable
constexpr std::uint32_t partial = 8u;             // a configured view is not usable this frame (see its view_flags)
}  // namespace slot_flags
namespace view_flags {
constexpr std::uint32_t valid = 1u;               // the view's record is this frame's (E may still be absent: present)
constexpr std::uint32_t present = 2u;             // included symbology in the window this frame; clear -> skip the view
constexpr std::uint32_t rejected = 4u;            // reserved: never produced (was: > 40 % of the world changed, NVG)
constexpr std::uint32_t untouched_hdr = 8u;       // no pass wrote the HDR target between OverlayHDRPass and the tone map
constexpr std::uint32_t own_tonemap = 16u;        // E evaluated inside the writer's tone-map PS (stock replica or a mode)
constexpr std::uint32_t pre_dlss_hud = 32u;       // HUD_MFD_after_DLSS = false: the HUD went through DLSS; no E
constexpr std::uint32_t overflow = 64u;           // the symbology's extent exceeded the capacity: present clear, no E
constexpr std::uint32_t mfd_by_mask = 128u;       // reserved: never produced (was: COMMON draws excluded by the cockpit mask)
}  // namespace view_flags
namespace layout_flags {
constexpr std::uint32_t layout_valid = 1u;        // slot.focus describes this frame (four views seen)
constexpr std::uint32_t from_submit = 2u;         // tangents from submitted / original XrFovf (else gProj: with jitter)
}  // namespace layout_flags
namespace qv_flags {
constexpr std::uint32_t far_depth_warp = 1u;      // dcsvr_far_depth on: Quad Views displaces samples by a depth field
constexpr std::uint32_t periphery_temporal = 2u;  // dcsvr_temporal on: the periphery carries a history blend
constexpr std::uint32_t sharpen_taper = 4u;       // dcsvr_sharpen_taper on: CAS fades out over the blend band
constexpr std::uint32_t debug_keys = 8u;          // debug_keys on: the values may have changed at runtime
constexpr std::uint32_t profile_file = 16u;       // %DCSVR_QUAD_SETTINGS% was read
}  // namespace qv_flags

static_assert(sizeof(EmissionView) == 72 && offsetof(EmissionView, x) == 4 && offsetof(EmissionView, capW) == 12 &&
                  offsetof(EmissionView, outW) == 20 && offsetof(EmissionView, winX) == 28 &&
                  offsetof(EmissionView, winW) == 36 && offsetof(EmissionView, fovTan) == 44 &&
                  offsetof(EmissionView, reserved) == 60,
              "EmissionView layout");
static_assert(sizeof(EyeFocus) == 48 && offsetof(EyeFocus, eyeFovTan) == 16 && offsetof(EyeFocus, focusFovTan) == 32,
              "EyeFocus layout");
static_assert(sizeof(FocusLayout) == 104 && offsetof(FocusLayout, flags) == 96 && offsetof(FocusLayout, fovSource) == 100,
              "FocusLayout layout");
static_assert(sizeof(EmissionSlot) == 424 && offsetof(EmissionSlot, flags) == 24 && offsetof(EmissionSlot, viewMask) == 28 &&
                  offsetof(EmissionSlot, view) == 32 && offsetof(EmissionSlot, focus) == 320,
              "EmissionSlot layout");
static_assert(sizeof(QvSettings) == 24 && offsetof(QvSettings, flags) == 16 && offsetof(QvSettings, settingsGen) == 20,
              "QvSettings layout");
static_assert(offsetof(Header, magic) == 8 && offsetof(Header, flags) == 20 && offsetof(Header, generation) == 24 &&
                  offsetof(Header, format) == 32 && offsetof(Header, atlasW) == 36 && offsetof(Header, ownerPid) == 44 &&
                  offsetof(Header, adapterLuid) == 48 && offsetof(Header, textureHandle) == 56 &&
                  offsetof(Header, fenceHandle) == 80 && offsetof(Header, newestSeq) == 88 &&
                  offsetof(Header, heartbeatQpc) == 96 && offsetof(Header, qpcFrequency) == 104 &&
                  offsetof(Header, newestSlot) == 112 && offsetof(Header, revision) == 116 &&
                  offsetof(Header, classFormat) == 120 && offsetof(Header, emissionFlags) == 124 &&
                  offsetof(Header, classTextureHandle) == 128 && offsetof(Header, qv) == 152 &&
                  offsetof(Header, fovSource) == 176 && offsetof(Header, slot) == 184 &&
                  offsetof(Header, reserved2) == 184 + 3 * 424 && sizeof(Header) == 2048,
              "Header layout");
static_assert(offsetof(Header, readerHeartbeatQpc) == 2040, "readerHeartbeatQpc: the last 8 bytes");

// ---- Seqlock (copies bytes 8 .. 2039, never the reader's field) ----------------------------------------------------
static_assert(sizeof(std::atomic<std::uint64_t>) == 8 && std::atomic<std::uint64_t>::is_always_lock_free, "seq");
inline std::atomic<std::uint64_t>& seq_of(Header* h) { return *reinterpret_cast<std::atomic<std::uint64_t>*>(&h->seq); }
inline void seqlock_write(Header* block, const Header& value) {
    auto& seq = seq_of(block);
    const std::uint64_t start = seq.load(std::memory_order_relaxed) | 1u;
    seq.store(start, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    constexpr std::size_t offset = sizeof(std::uint64_t);
    constexpr std::size_t end = offsetof(Header, readerHeartbeatQpc);
    std::memcpy(reinterpret_cast<unsigned char*>(block) + offset, reinterpret_cast<const unsigned char*>(&value) + offset,
                end - offset);
    std::atomic_thread_fence(std::memory_order_release);
    seq.store(start + 1, std::memory_order_release);
}
inline bool seqlock_read(const Header* block, Header& out, int attempts = 4) {
    auto& seq = seq_of(const_cast<Header*>(block));
    for (int i = 0; i < attempts; ++i) {
        const std::uint64_t before = seq.load(std::memory_order_acquire);
        if (before == 0) return false;
        if (before & 1u) continue;
        std::memcpy(&out, block, sizeof(Header));
        std::atomic_thread_fence(std::memory_order_acquire);
        if (seq.load(std::memory_order_relaxed) == before) {
            out.seq = before;
            return true;
        }
    }
    return false;
}

// ---- Reader heartbeat (R67) ----------------------------------------------------------------------------------------
inline std::atomic<std::uint64_t>& reader_heartbeat_of(Header* h) {
    return *reinterpret_cast<std::atomic<std::uint64_t>*>(&h->readerHeartbeatQpc);
}
inline void reader_heartbeat(Header* h, std::int64_t nowQpc) {
    reader_heartbeat_of(h).store(static_cast<std::uint64_t>(nowQpc), std::memory_order_release);
}
inline bool reader_alive(const Header* h, std::int64_t nowQpc, std::int64_t qpcFrequency,
                         std::int64_t maxAgeMs = reader_heartbeat_timeout_ms) {
    const std::uint64_t hb = reader_heartbeat_of(const_cast<Header*>(h)).load(std::memory_order_acquire);
    if (hb == 0 || qpcFrequency <= 0 || nowQpc <= 0) return false;
    const std::int64_t limit = qpcFrequency / 1000 * maxAgeMs + qpcFrequency % 1000 * maxAgeMs / 1000;
    const std::int64_t age = nowQpc - static_cast<std::int64_t>(hb);
    return age <= limit && -age <= limit;
}

// ---- Reader helpers (pure) -----------------------------------------------------------------------------------------
inline bool header_usable(const Header& h, std::uint32_t pid) {
    return h.magic == block_magic && h.version == block_version && h.structSize >= sizeof(Header) &&
           (h.flags & header_flags::active) && h.generation != 0 && h.slotCount == slot_count &&
           h.format == format_rgba16f && h.ownerPid == pid && h.atlasW > 0 && h.atlasH > 0;
}
inline int find_slot(const Header& h, std::int64_t displayTime) {
    if (displayTime == 0) return -1;
    int best = -1;
    for (int i = 0; i < slot_count; ++i) {
        const EmissionSlot& s = h.slot[i];
        const std::uint32_t need = slot_flags::valid | slot_flags::display_time;
        if ((s.flags & need) != need || s.displayTime != displayTime || s.seq == 0) continue;
        if (best < 0 || s.seq > h.slot[best].seq) best = i;
    }
    return best;
}
inline bool paired_with_mask(const Header& h, int s, int maskSlotIndex, std::uint64_t maskSeq, std::uint32_t maskGeneration) {
    return s >= 0 && s < slot_count && s == maskSlotIndex && maskSeq != 0 && h.slot[s].seq == maskSeq &&
           h.generation == maskGeneration;
}
// Consumer rule 1: the view has E this frame: valid AND present, not overflow, a non-empty window inside the view and
// within the capacity, the capacity sub-rect inside the atlas.
inline bool view_present(const Header& h, int s, int v) {
    if (s < 0 || s >= slot_count || v < 0 || v >= max_views) return false;
    const EmissionView& m = h.slot[s].view[v];
    const std::uint32_t need = view_flags::valid | view_flags::present;
    return (m.flags & need) == need && !(m.flags & view_flags::overflow) && (h.slot[s].viewMask & (1u << v)) &&
           m.capW > 0 && m.capH > 0 && m.x + m.capW <= h.atlasW && m.y + m.capH <= h.atlasH && m.winW > 0 &&
           m.winH > 0 && m.winW <= m.capW && m.winH <= m.capH && m.winX + m.winW <= m.outW && m.winY + m.winH <= m.outH;
}
// A view pixel (px, py) of view v -> its atlas texel; false outside the window (E is 0 / not published there).
inline bool view_to_atlas(const EmissionView& m, std::uint32_t px, std::uint32_t py, std::uint32_t& ax, std::uint32_t& ay) {
    if (px < m.winX || py < m.winY || px - m.winX >= m.winW || py - m.winY >= m.winH) return false;
    ax = m.x + (px - m.winX);
    ay = m.y + (py - m.winY);
    return true;
}
inline bool layout_usable(const Header& h, int s) {
    return s >= 0 && s < slot_count && (h.slot[s].focus.flags & layout_flags::layout_valid);
}
// The focus rect of eye e from the tangents (what the writer stores in rectNorm), against any stereo FOV. Give it the
// ORIGINAL FOVs (fov_source_pupil_original) whenever they are available: Quad Views composes with those.
inline bool focus_rect(const float eyeTan[4], const float focusTan[4], float out[4]) {
    const float l = eyeTan[0], r = eyeTan[1], u = eyeTan[2], d = eyeTan[3];
    if (!(r > l) || !(u > d)) return false;
    out[0] = (focusTan[0] - l) / (r - l);
    out[1] = (u - focusTan[2]) / (u - d);
    out[2] = (focusTan[1] - l) / (r - l);
    out[3] = (u - focusTan[3]) / (u - d);
    return out[2] > out[0] && out[3] > out[1];
}
// Quad Views' focus weight (ProjectionPS.hlsl) at focus uv (0..1 inside the focus view).
inline float smooth_step(float a, float b, float x) {
    float t = (x - a) / (b - a);
    t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
    return t * t * (3.0f - 2.0f * t);
}
inline float pow_pos(float b, float e) { return b <= 0.0f ? 0.0f : std::pow(b, e); }
inline float focus_weight(const QvSettings& qv, float fu, float fv) {
    const float nx = 2.0f * fu - 1.0f, ny = 1.0f - 2.0f * fv;
    const float ax = nx < 0 ? -nx : nx, ay = ny < 0 ? -ny : ny;
    if (!(ax < 1.0f && ay < 1.0f)) return 0.0f;
    const float s = qv.smoothenFocusViewEdges, k = qv.focusViewShape;
    if (k >= 1.0f) {
        const float rr = pow_pos(pow_pos(ax, k) + pow_pos(ay, k), 1.0f / k);
        if (!(rr < 1.0f)) return 0.0f;
        float w = 2.0f * s;
        w = w < 0.01f ? 0.01f : (w > 0.95f ? 0.95f : w);
        return 1.0f - smooth_step(1.0f - w, 1.0f, rr);
    }
    if (s > 0.0f) {
        const float tx = smooth_step(0.0f, s, fu) - smooth_step(1.0f - s, 1.0f, fu);
        const float ty = smooth_step(0.0f, s, fv) - smooth_step(1.0f - s, 1.0f, fv);
        const float t = tx * ty;
        return t > 0.5f ? t : 0.5f;
    }
    return 1.0f;
}

// Consumer rule 5: the re-composite of one texel (linear light). a == 0 in v1 (pure delta).
struct Rgb { float r, g, b; };
inline Rgb recomposite(Rgb synth, Rgb e, float a, Rgb cleanAtSource) {
    return {synth.r + e.r + a * (cleanAtSource.r - synth.r), synth.g + e.g + a * (cleanAtSource.g - synth.g),
            synth.b + e.b + a * (cleanAtSource.b - synth.b)};
}

}  // namespace dcsvr_symbology
