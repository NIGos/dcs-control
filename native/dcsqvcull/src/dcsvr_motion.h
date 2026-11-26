// DCS Control: motion-vector companion export v1 (R68 Stage 2, design R70 §9), the contract between DCS Control's
// engine module (DcsQvCull, the writer) and a frame-generation layer (OFXR, the reader). A COMPANION of the cockpit mask
// (dcsvr_cockpit_mask.h) and of the cockpit depth (dcsvr_cockpit_depth.h): written in the same xrEndFrame commit, in the
// slot of the same index, with seq == the mask slot's maskSeq, covered by the same single fence Signal. Design and
// rationale: docs/research/R70_mv_export_design.md.
//
//   Local\DcsVrMotion_v1   written by the engine module per rendered frame ([Perception] CockpitMotionExport=1, which
//                          requires CockpitMaskExport=1; independent of CockpitDepthExport) while a reader stores its
//                          heartbeat in this block AND in the mask block.
//
// What it holds (periphery views 0 / 1; revision 4 adds the focus views 2 / 3 and a per-pixel linear depth plane, see
// "Revision 4" below; design: docs/FOCUS_MOTION.md).
//  - MV atlas (R16G16_FLOAT, `format`): DCS's own G-buffer motion vectors of each view, copied verbatim at the view's
//    RENDER resolution (the DLSS input, before DLSS; divisor 1: the rect is DCS's viewport, w = srcW, h = srcH), at the
//    mask's sample point (after the view's opaque G-buffer and water passes). Units: render pixels of the view, +x
//    right, +y down; direction B -> A: the pixel's position in the PREVIOUS frame = its current position + MV (DCS's
//    calcMotionVector, Bazar/shaders/deferred/GBuffer.hlsl). Ownship (aircraft) and head motion are included.
//    Jitter (R70 live T1, revision 2): DCS's MVs are JITTER-FREE (slot_flags::jitter_free): with still frames and a
//    DLSS jitter of 0.3-0.95 px between them, every MV is 0 +- 0.001 px. So the true motion between two DLSS outputs is
//        mvTrue = mv                                          (render pixels; then scale render -> output)
//    (true_motion() applies the flags). slot_flags::includes_jitter is never set by this writer; were it set, the
//    formula would be mv - (jitterPrevPx - jitterCurPx). Without either flag treat the MVs as unproven (use flow).
//    Sky (R70 live T2, slot_flags::sky_is_infinity): texels beyond 65504 m (class bit 7) hold the camera's motion at
//    infinity; with synthetic yaw turns every terrain / sky / model MV matched the head rotation within 0.016 px (p99.9).
//  - Class atlas (R8_UINT, `classFormat`, header_flags::class_atlas), the same size and rects: per render texel,
//    bits 0-2 DCS's stencil composition ((stencil >> 3) & 7: 0 surface, 1 model, 2 water, 3 foliage, 4 grass,
//    5 cockpit, 6 underwater, 7 empty; Bazar/shaders/common/stencil.hlsl), bit 7 sky (no finite distance below 65504 m
//    along the view axis: DCS's sky dome and anything beyond, as the depth companion's 65504), bit 6
//    reserved (v1: never set; "composited later / no MV" is not detectable at the sample point: particles, smoke,
//    clouds and glass write no depth, stencil or MV), bits 3-5 0. Without a stencil plane bits 0-2 are 7 (unknown).
//
// Mapping to a stereo image: by tangent, as the mask (fovTan of the rect: DCS's gProj this frame, jitter included).
//
// Pairing (the mask's, shared): use slot s only when the mask slot s = find_slot(maskHeader, T) has the same seq,
// this block's find_slot(T) == s, generation == the mask's, the slot has valid | display_time and the view is valid.
// Additionally, for a pair (A, B) of captures: B.prevDisplayTime == A's displayTime and B has no slot_flags::reset.
// Reset is set on: a camera cut (DCS's view jumped or turned beyond head motion: F1/F2/F10, respawn), a mission load
// or scene change, the first commit after install / idle / resume, a generation change, prevSeq != the previous
// committed seq or a non-consecutive engine frame (a dropped frame), and either jitter unknown.
// Partial slots: use the MVs of a view only when its view_flags::valid is set.
//
// Fence: fenceHandle is the SAME NT handle value as the mask block's (header_flags::shares_mask_fence). One
// Wait(fence, seq) covers mask, depth and motion. Generation == the mask's generation (textures made together).
// Held set (R85, semantics only; dcsvr_cockpit_mask.h "Held set"): switching CockpitMotionExport (or the mask) off
// keeps the MV / class textures and the fence alive; this block's last record keeps the generation and the handles
// with header_flags::active clear. Back on, the same generation and handles are published active; the first commit
// after it is a reset (as after any install). An unchanged generation across active 1 -> 0 -> 1 means the same
// objects (a reader may keep its opened set); a generation change means new ones. The same across the F10 map and
// loading screens (R88: no relayout while no flight frame is drawn; the mask block's "Held set"); the first commit
// after it is a reset.
//
// Reader heartbeat (R67 rules): readerHeartbeatQpc (offset 2040), stored by the reader (reader_heartbeat), at least
// once per second while it consumes MVs, whatever the header says, in BOTH this block and the mask block. Without it
// the engine copies nothing (no extra GPU work) and sets header_flags::idle.
//
// Content time per slot (R74a, revision 3, additive: header.slotTime[3] took 144 bytes at the start of the former
// reserved tail; every older offset, the size 2048 and version 1 are unchanged). header_flags::slot_time (128): the
// writer fills it. slotTime[s] describes slot[s]'s frame when slotTime[s].seq == slot[s].seq != 0 (slot_time_usable).
// The same record, byte for byte, is in the depth companion (dcsvr_cockpit_depth.h, header.slotTime, where the fields
// are documented): poseQpc = QPC at the entry of DCS's own xrLocateViews call whose pose DCS rendered this frame
// (paired by displayTime, then by the returned view 0 orientation, else the last such call), poseReturnQpc, its
// displayTime, simTime = DCS model time (seconds), time_flags. A frame-generation reader can time a pair (A, B) by
// content: fraction of a display time T between them = (QPC(T) - A.poseQpc) / (B.poseQpc - A.poseQpc), with QPC(T)
// from XR_KHR_win32_convert_performance_counter_time; with time_flags::sim_stable on both, simTime may time world and
// ownship motion instead.
//
// Revision 4 (Q1 + Q2 of the motion study; additive: version 1, the size 2048, every older offset and the meaning of
// every older field are unchanged; the new records took 416 bytes at the start of the former reserved tail).
//  - Focus views (header_flags::focus_views, [Perception] CockpitMotionFocus=1). The focus views 2 (left) and 3 (right)
//    get MV and class rects in the SAME atlases, sampled the same way as views 0 / 1: DCS's own G-buffer MVs of that
//    focus view at its render resolution (divisor 1), copied inside the view's own pass at its sample point (the
//    view's WaterSeaPass end, view_flags::after_water, else its GBufferPass end; per-view targets are bound only inside
//    the passes), before DLSSPass, so the same DLSS input DCS (or Cheeky's private focus SR) gets, read only. Their
//    records are header.focusView[s][v - 2] (MotionSlot::view keeps 2 entries: an older reader never sees them); slot
//    s's viewMask bits 2 / 3 say which are valid; use view_record() / view_usable() for any view 0..3. Units and
//    direction as views 0 / 1 (render pixels of THAT view, B -> A). Mapping by tangent (fovTan of the focus rect).
//    Jitter: the jitter-free proof (R70 T1) was made on views 0 / 1 only. A view's MVs count as jitter-free only with
//    view_flags::jitter_proven on that view (the writer sets it on 0 / 1 together with slot_flags::jitter_free, and on
//    2 / 3 only once the same check has passed live for the focus views); true_motion_view() applies it. Without it a
//    reader uses flow (or the periphery MVs by tangent) in the focus region. slot_flags::focus_partial: the mask slot
//    has a focus view that this slot has no MVs for (slot_flags::partial stays about views 0 / 1 only).
//  - Depth plane (header_flags::depth_plane, [Perception] CockpitMotionDepth=1). A third per-slot texture,
//    depthFormat (format_r16_float), depthTextureHandle[s], the SAME size and rects as the MV atlas: per render texel of
//    every view whose view_flags::depth_plane is set, the LINEAR view depth in metres along the view axis (the depth
//    companion's unit: w = near / d from DCS's gProj of that view and frame, reverse-Z, viewport MinDepth/MaxDepth
//    undone first), taken from the view's own DSV at the SAME sample point as its MVs (so MV, class and depth of a texel
//    describe the same surface), sky_depth_m (65504) for the sky and anything beyond (class bit 7), cockpit texels at
//    the cockpit camera's depth (as the depth companion). Half-float steps: 1 m at 1-2 km, 2 m at 2-4 km. Texels
//    outside the rects are undefined. The depth companion (dcsvr_cockpit_depth.h, revision 3) sets its
//    header_flags::per_pixel_in_motion while this plane is published, and keeps its own block min/max grid unchanged.
//  - Previous frame (for depth-ordered composition): the pair (A, B) of usable_for_pair() has A in the slot whose seq is
//    B.prevSeq (prev_slot()). The engine reuses a slot after slot_count - 1 newer frames, so at B's xrEndFrame both A
//    and B are intact on the GPU; a reader that reads A later than that, or on another queue that may run after the
//    next frame's writes, holds it with the mask block's releaseFence (signal A.seq - 1) or keeps its own copy of A
//    (recommended: copy each frame's slot at its own xrEndFrame, as today, and keep the last copy as "previous").
//  - Layout. Rects are authoritative; the writer packs the four views in one atlas (recommended: periphery 0 above 1 in
//    a column at x = 0, focus 2 and 3 side by side to its right). With neither option on, the atlases and records are
//    revision 3's (header_flags focus_views / depth_plane clear, focusView, depthFormat and depthTextureHandle zero);
//    the only differences are revision 4 and view_flags::jitter_proven on views 0 / 1.
//  - Held set (R85). The first switch-on of either option is a layout change (a companion the held set lacks: a new
//    generation, once). Switching it off keeps the larger set (same generation and handles; the flag clears, the focus
//    records and viewMask bits 2 / 3 are 0, the depth plane is not written); back on, the same generation again.
//  - Older readers (revision <= 3) keep working: they read views 0 / 1 only, ignore viewMask bits 2 / 3 and the new
//    flags, and see a larger atlasW / atlasH (they size their copies from it: with the focus views on, an older OFXR
//    allocates larger copy textures). Older writers: header_flags focus_views / depth_plane clear, revision 3.
//
// Self-contained: <atomic>, <cstddef>, <cstdint>, <cstring>. #pragma pack(8), little-endian. The name takes the optional
// suffix of the environment variable DCSVR_PERCEPTION_SHM_SUFFIX (as the mask; tests use it).
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace dcsvr_motion {

constexpr const wchar_t* block_name = L"Local\\DcsVrMotion_v1";
constexpr const char* name_suffix_variable = "DCSVR_PERCEPTION_SHM_SUFFIX";
constexpr std::uint32_t block_magic = 0x564D4344u;  // "DCMV"
constexpr std::uint32_t block_version = 1;
constexpr std::uint32_t contract_revision = 4;  // 2 (R70 live): slot_flags::jitter_free, the sky bit beyond 65504 m;
                                                // 3 (R74a): slotTime; 4 (Q1 + Q2): focus views 2 / 3, depth plane
constexpr int periphery_views = 2;  // MotionSlot::view: 0 left, 1 right (every revision)
constexpr int focus_views = 2;      // revision 4: Header::focusView: 2 left, 3 right
constexpr int max_views = periphery_views + focus_views;  // view indices 0..3 (revision <= 3: 2; use view_record())
constexpr int slot_count = 3;
constexpr std::uint32_t format_r16g16_float = 34;  // DXGI_FORMAT_R16G16_FLOAT
constexpr std::uint32_t format_r8_uint = 62;       // DXGI_FORMAT_R8_UINT
constexpr std::uint32_t format_r16_float = 54;     // DXGI_FORMAT_R16_FLOAT (revision 4: the depth plane)
constexpr float sky_depth_m = 65504.0f;            // depth plane: the sky / beyond half float (as the depth companion)
constexpr std::uint8_t class_composition_mask = 7;
constexpr std::uint8_t class_sky = 0x80;
constexpr std::uint8_t class_reserved_late = 0x40;  // never set in v1
constexpr std::uint8_t composition_surface = 0, composition_model = 1, composition_water = 2,
                       composition_foliage = 3, composition_grass = 4, composition_cockpit = 5,
                       composition_underwater = 6, composition_empty = 7;
constexpr std::int64_t reader_heartbeat_period_ms = 1000;
constexpr std::int64_t reader_heartbeat_timeout_ms = 2000;

#pragma pack(push, 8)
struct MotionView {               // 64 bytes
    std::uint32_t flags;          // view_flags
    std::uint32_t x, y, w, h;     // rect in the MV atlas (and the class atlas): w = srcW, h = srcH (divisor 1)
    std::uint32_t srcW, srcH;     // DCS's viewport of the view (render resolution)
    float fovTan[4];              // left, right, up, down tangents of the rect's edges (gProj this frame, jitter included)
    float jitterCurPx[2];         // DLSS jitter of this frame (render texels, +x right, +y down; image = unjittered + it)
    float jitterPrevPx[2];        // the same view's jitter in the previous committed frame (NaN unknown -> reset)
    std::uint32_t reserved;       // 0
};

struct MotionSlot {               // 184 bytes
    std::uint64_t seq;            // == the mask slot's maskSeq (fence value); 0 never written
    std::uint64_t engineFrame;
    std::int64_t displayTime;     // XrFrameEndInfo::displayTime of this frame (slot_flags::display_time)
    std::int64_t qpcCommit;
    std::uint64_t prevSeq;        // seq of the previous committed frame the MVs point to (0: none)
    std::int64_t prevDisplayTime; // its displayTime (0: unknown); the reader requires == its A capture's displayTime
    std::uint32_t flags;          // slot_flags
    std::uint32_t viewMask;       // bit v: view v valid (bits 2 / 3: revision 4 focus views, Header::focusView)
    MotionView view[periphery_views];  // views 0 / 1 (the focus views' records are Header::focusView[slot])
};

// R74a: content time of the frame of slot[s]; identical to dcsvr_cockpit_depth::SlotTime (fields documented there).
struct SlotTime {                 // 48 bytes
    std::uint64_t seq;            // == slot[s].seq when this record belongs to that slot's frame; 0: none
    std::int64_t poseQpc;         // QPC at the entry of DCS's xrLocateViews whose pose DCS rendered (pose_observed)
    std::int64_t poseReturnQpc;   // QPC when that call returned
    std::int64_t poseDisplayTime; // that call's XrViewLocateInfo::displayTime
    double simTime;               // DCS model time, seconds (sim_observed)
    std::uint32_t flags;          // time_flags
    std::uint32_t locateCalls;    // DCS's xrLocateViews calls with this frame's displayTime
};

struct Header {                   // 2048 bytes
    std::uint64_t seq;
    std::uint32_t magic, version, structSize, flags;
    std::uint32_t generation;     // == the mask header's generation
    std::uint32_t slotCount, format, atlasW, atlasH, ownerPid;
    std::uint64_t adapterLuid;
    std::uint64_t textureHandle[slot_count];       // MV atlases (NT handles)
    std::uint64_t fenceHandle;                     // == the mask block's fenceHandle (shares_mask_fence)
    std::uint64_t newestSeq;
    std::int64_t heartbeatQpc, qpcFrequency;
    std::uint32_t newestSlot, revision;
    std::uint32_t classFormat, reserved0;          // format_r8_uint with header_flags::class_atlas, else 0
    std::uint64_t classTextureHandle[slot_count];  // class atlases (NT handles), or 0
    std::uint64_t reserved1;
    MotionSlot slot[slot_count];
    SlotTime slotTime[slot_count];                 // R74a (revision 3, header_flags::slot_time): see slot_time_usable
    MotionView focusView[slot_count][focus_views]; // revision 4 (header_flags::focus_views): views 2 / 3 of slot s
    std::uint32_t depthFormat, reserved3;          // revision 4: format_r16_float with header_flags::depth_plane, else 0
    std::uint64_t depthTextureHandle[slot_count];  // revision 4: depth planes (NT handles, the MV atlas's size), or 0
    std::uint64_t reserved2[96];
    std::uint64_t readerHeartbeatQpc;              // offset 2040, written by the READER only (R67 rules)
};
#pragma pack(pop)

static_assert(sizeof(MotionView) == 64 && offsetof(MotionView, x) == 4 && offsetof(MotionView, srcW) == 20 &&
                  offsetof(MotionView, fovTan) == 28 && offsetof(MotionView, jitterCurPx) == 44 &&
                  offsetof(MotionView, jitterPrevPx) == 52 && offsetof(MotionView, reserved) == 60,
              "MotionView layout");
static_assert(sizeof(MotionSlot) == 184 && offsetof(MotionSlot, prevSeq) == 32 && offsetof(MotionSlot, prevDisplayTime) == 40 &&
                  offsetof(MotionSlot, flags) == 48 && offsetof(MotionSlot, viewMask) == 52 && offsetof(MotionSlot, view) == 56,
              "MotionSlot layout");
static_assert(offsetof(Header, magic) == 8 && offsetof(Header, flags) == 20 && offsetof(Header, generation) == 24 &&
                  offsetof(Header, format) == 32 && offsetof(Header, atlasW) == 36 && offsetof(Header, ownerPid) == 44 &&
                  offsetof(Header, adapterLuid) == 48 && offsetof(Header, textureHandle) == 56 &&
                  offsetof(Header, fenceHandle) == 80 && offsetof(Header, newestSeq) == 88 &&
                  offsetof(Header, heartbeatQpc) == 96 && offsetof(Header, qpcFrequency) == 104 &&
                  offsetof(Header, newestSlot) == 112 && offsetof(Header, revision) == 116 &&
                  offsetof(Header, classFormat) == 120 && offsetof(Header, classTextureHandle) == 128 &&
                  offsetof(Header, slot) == 160 && sizeof(Header) == 2048,
              "Header layout");
static_assert(offsetof(Header, readerHeartbeatQpc) == 2040, "readerHeartbeatQpc: the last 8 bytes");
static_assert(sizeof(SlotTime) == 48 && offsetof(SlotTime, poseQpc) == 8 && offsetof(SlotTime, poseReturnQpc) == 16 &&
                  offsetof(SlotTime, poseDisplayTime) == 24 && offsetof(SlotTime, simTime) == 32 &&
                  offsetof(SlotTime, flags) == 40 && offsetof(SlotTime, locateCalls) == 44 &&
                  offsetof(Header, slotTime) == 712,
              "SlotTime layout (R74a): 48 bytes, slotTime at 712");
static_assert(offsetof(Header, focusView) == 712 + 144 && offsetof(Header, depthFormat) == 856 + 384 &&
                  offsetof(Header, reserved3) == 1244 && offsetof(Header, depthTextureHandle) == 1248 &&
                  offsetof(Header, reserved2) == 1272 && 1272 + sizeof(Header::reserved2) == 2040,
              "Revision 4 layout: focusView at 856, depthFormat at 1240, depthTextureHandle at 1248, tail to 2040");

namespace header_flags {
constexpr std::uint32_t active = 1u;
constexpr std::uint32_t fence = 2u;
constexpr std::uint32_t reader_heartbeat = 8u;
constexpr std::uint32_t idle = 16u;              // no fresh heartbeat (this block's or the mask's): no new slots
constexpr std::uint32_t shares_mask_fence = 32u;
constexpr std::uint32_t class_atlas = 64u;       // classFormat / classTextureHandle set
constexpr std::uint32_t slot_time = 128u;        // R74a: the writer fills slotTime (revision >= 3)
constexpr std::uint32_t focus_views = 256u;      // revision 4: focusView records and viewMask bits 2 / 3 are written
constexpr std::uint32_t depth_plane = 512u;      // revision 4: depthFormat / depthTextureHandle set, the plane written
}  // namespace header_flags
namespace time_flags {                           // R74a: SlotTime::flags (as dcsvr_cockpit_depth::time_flags)
constexpr std::uint32_t pose_observed = 1u;
constexpr std::uint32_t pose_matched = 2u;
constexpr std::uint32_t pose_ambiguous = 4u;
constexpr std::uint32_t sim_observed = 8u;
constexpr std::uint32_t sim_stable = 16u;
}  // namespace time_flags
namespace slot_flags {
constexpr std::uint32_t valid = 1u;
constexpr std::uint32_t display_time = 2u;
constexpr std::uint32_t late_commit = 4u;        // no xrEndFrame hook: not pairable
constexpr std::uint32_t partial = 8u;            // a periphery view of the mask slot has no MVs: use view_flags::valid
constexpr std::uint32_t reset = 16u;             // the MVs must not be used for the pair ending here (flow instead)
constexpr std::uint32_t includes_jitter = 32u;   // proven live (R70 T1): mv includes jitterPrev - jitterCur
constexpr std::uint32_t sky_is_infinity = 64u;   // proven live (R70 T2): sky texels hold the camera MV at infinity
constexpr std::uint32_t jitter_free = 128u;      // proven live (R70 T1, revision 2): mv is the jitter-free motion
constexpr std::uint32_t focus_partial = 256u;    // revision 4: a focus view of the mask slot has no MVs here
constexpr std::uint32_t depth_plane = 512u;      // revision 4: some view of this slot has view_flags::depth_plane
}  // namespace slot_flags
namespace view_flags {
constexpr std::uint32_t valid = 1u;
constexpr std::uint32_t after_water = 2u;
constexpr std::uint32_t from_rgba16f = 4u;       // DCS's MV target was R16G16B16A16_FLOAT (.xy copied)
constexpr std::uint32_t no_stencil = 8u;         // class bits 0-2 unknown (7) for this view
constexpr std::uint32_t focus = 16u;             // revision 4: a focus view (2 / 3); as dcsvr_cockpit_depth's bit 16
constexpr std::uint32_t depth_plane = 32u;       // revision 4: the depth plane's rect holds this view's depth this frame
constexpr std::uint32_t jitter_proven = 64u;     // revision 4: the slot's jitter flag is proven live for THIS view
}  // namespace view_flags

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

// ---- Reader heartbeat ------------------------------------------------------------------------------------------------
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
           h.format == format_r16g16_float && h.ownerPid == pid && h.atlasW > 0 && h.atlasH > 0;
}
inline int find_slot(const Header& h, std::int64_t displayTime) {
    if (displayTime == 0) return -1;
    int best = -1;
    for (int i = 0; i < slot_count; ++i) {
        const MotionSlot& s = h.slot[i];
        const std::uint32_t need = slot_flags::valid | slot_flags::display_time;
        if ((s.flags & need) != need || s.displayTime != displayTime || s.seq == 0) continue;
        if (best < 0 || s.seq > h.slot[best].seq) best = i;
    }
    return best;
}
// Slot s pairs with the mask slot of the same index (seq == maskSeq, same generation).
inline bool paired_with_mask(const Header& h, int s, int maskSlotIndex, std::uint64_t maskSeq, std::uint32_t maskGeneration) {
    return s >= 0 && s < slot_count && s == maskSlotIndex && maskSeq != 0 && h.slot[s].seq == maskSeq &&
           h.generation == maskGeneration;
}
// R74a: slotTime[s] describes slot s's frame.
inline bool slot_time_usable(const Header& h, int s) {
    return s >= 0 && s < slot_count && (h.flags & header_flags::slot_time) && h.revision >= 3 &&
           h.slot[s].seq != 0 && h.slotTime[s].seq == h.slot[s].seq;
}
// R74a: the content fraction of a time t (QPC ticks) between captures A and B (their slotTime): 0 at A's pose, 1 at
// B's; false when either pose is not observed or B is not after A.
inline bool content_fraction(const SlotTime& a, const SlotTime& b, std::int64_t tQpc, double& out) {
    if (!(a.flags & time_flags::pose_observed) || !(b.flags & time_flags::pose_observed) || b.poseQpc <= a.poseQpc)
        return false;
    out = double(tQpc - a.poseQpc) / double(b.poseQpc - a.poseQpc);
    return true;
}
// Revision 4: the record of view v (0..3) of slot s: slot[s].view[v] for 0 / 1; focusView[s][v - 2] for 2 / 3 when the
// writer publishes focus views (header_flags::focus_views, revision >= 4); else nullptr.
inline const MotionView* view_record(const Header& h, int s, int v) {
    if (s < 0 || s >= slot_count || v < 0 || v >= max_views) return nullptr;
    if (v < periphery_views) return &h.slot[s].view[v];
    if (!(h.flags & header_flags::focus_views) || h.revision < 4) return nullptr;
    return &h.focusView[s][v - periphery_views];
}
inline bool view_usable(const Header& h, int s, int v) {
    const MotionView* r = view_record(h, s, v);
    if (r == nullptr) return false;
    const MotionView& m = *r;
    return (m.flags & view_flags::valid) && (h.slot[s].viewMask & (1u << v)) && m.w > 0 && m.h > 0 &&
           m.x + m.w <= h.atlasW && m.y + m.h <= h.atlasH;
}
// Revision 4: the depth plane of slot s holds view v's per-pixel linear depth (metres along the view axis) in the
// view's MV rect.
inline bool depth_usable(const Header& h, int s, int v) {
    if (!view_usable(h, s, v) || !(h.flags & header_flags::depth_plane) || h.revision < 4 ||
        h.depthFormat != format_r16_float || h.depthTextureHandle[s] == 0)
        return false;
    return (view_record(h, s, v)->flags & view_flags::depth_plane) != 0;
}
// Revision 4: the slot holding the earlier capture A of the pair that ends at slot s (seq == slot[s].prevSeq, its
// displayTime == slot[s].prevDisplayTime), or -1 when the ring no longer (or never) held it.
inline int prev_slot(const Header& h, int s) {
    if (s < 0 || s >= slot_count) return -1;
    const MotionSlot& b = h.slot[s];
    if (b.prevSeq == 0 || b.prevDisplayTime == 0) return -1;
    const std::uint32_t need = slot_flags::valid | slot_flags::display_time;
    for (int i = 0; i < slot_count; ++i) {
        const MotionSlot& a = h.slot[i];
        if (i != s && a.seq == b.prevSeq && (a.flags & need) == need && a.displayTime == b.prevDisplayTime) return i;
    }
    return -1;
}
// The MVs of slot s may be used for the pair whose earlier capture had displayTime prevCaptureDisplayTime.
inline bool usable_for_pair(const Header& h, int s, std::int64_t prevCaptureDisplayTime) {
    if (s < 0 || s >= slot_count) return false;
    const MotionSlot& m = h.slot[s];
    return !(m.flags & slot_flags::reset) && m.prevSeq != 0 && m.prevDisplayTime != 0 &&
           m.prevDisplayTime == prevCaptureDisplayTime;
}
// The jitter-free motion (render pixels) of a texel's raw MV for view m of a slot with slotFlags: mv itself with
// slot_flags::jitter_free (DCS, R70 T1), mv - (jitterPrev - jitterCur) with slot_flags::includes_jitter; false (out
// unset) without either (unproven: use flow).
inline bool true_motion(std::uint32_t slotFlags, const MotionView& m, const float mv[2], float out[2]) {
    if (slotFlags & slot_flags::jitter_free) {
        out[0] = mv[0];
        out[1] = mv[1];
        return true;
    }
    if (slotFlags & slot_flags::includes_jitter) {
        out[0] = mv[0] - (m.jitterPrevPx[0] - m.jitterCurPx[0]);
        out[1] = mv[1] - (m.jitterPrevPx[1] - m.jitterCurPx[1]);
        return true;
    }
    return false;
}
// Revision 4: true_motion for view v (0..3) of slot s: additionally, a focus view (2 / 3) needs view_flags::jitter_proven
// on its record (the jitter-free proof is per view). Views 0 / 1 keep the revision 2 rule (the slot flags alone).
inline bool true_motion_view(const Header& h, int s, int v, const float mv[2], float out[2]) {
    const MotionView* m = view_record(h, s, v);
    if (m == nullptr) return false;
    if (v >= periphery_views && !(m->flags & view_flags::jitter_proven)) return false;
    return true_motion(h.slot[s].flags, *m, mv, out);
}
inline bool tan_to_texel(const MotionView& m, float tx, float ty, std::uint32_t& col, std::uint32_t& row) {
    const float l = m.fovTan[0], r = m.fovTan[1], u = m.fovTan[2], d = m.fovTan[3];
    if (!(r > l) || !(u > d)) return false;
    const float fx = (tx - l) / (r - l), fy = (u - ty) / (u - d);
    if (!(fx >= 0.0f && fx < 1.0f && fy >= 0.0f && fy < 1.0f)) return false;
    col = static_cast<std::uint32_t>(fx * float(m.w));
    row = static_cast<std::uint32_t>(fy * float(m.h));
    if (col >= m.w) col = m.w - 1;
    if (row >= m.h) row = m.h - 1;
    return true;
}

}  // namespace dcsvr_motion
