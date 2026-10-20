// DCS Control: cockpit depth companion export v1 (R68 Stage 1), the contract between DCS Control's engine module
// (DcsQvCull, the writer) and a frame-generation layer (OFXR, the reader). It is a COMPANION of the cockpit mask
// (dcsvr_cockpit_mask.h, Local\DcsVrCockpitMask_v1): the same compute pass writes both, in the same slot index, with
// the same sequence number, committed in the same xrEndFrame and covered by the same single fence Signal. Design and
// rationale: docs/research/R68_translation_framegen.md (section 6) and R65_cockpit_mask_export.md (the mask).
//
//   Local\DcsVrCockpitDepth_v1   written by the engine module per rendered frame ([Perception] CockpitDepthExport=1,
//                                which requires CockpitMaskExport=1) while a reader stores its heartbeat in BOTH
//                                blocks; read by the frame-generation layer at its xrEndFrame next to the mask.
//
// What a texel holds. One R16G16_FLOAT texel per mask texel: the SAME atlas size, the SAME per-view rects (x, y, w, h),
// divisor, source size and fovTan as the mask slot of the same index (view_flags::same_grid_as_mask), so one tangent
// mapping serves both. Over exactly the source block the mask texel covers (divisor x divisor texels plus the mask's
// dilate apron):
//   .x  nearest COCKPIT depth: the smallest depth among the block's cockpit texels (the mask's own test: stencil
//       composition COCKPIT, or nearer than the mask's nearM); 0 when the block has no cockpit texel. Invariant:
//       .x > 0  <=>  the mask texel is 255 (cockpit). R74b: a mask texel of 128 (overlay symbology over the world,
//       mask v1.1) has .x == 0.
//   .y  farthest depth: the largest depth among all texels of the block (the background, for disocclusion fill);
//       65504 (sky_depth_m, the largest half float) for the sky (DCS's reverse-Z depth 0) and anything beyond it.
// Units: METRES, linear, positive view-space distance ALONG THE VIEW AXIS (the clip w of DCS's projection: forward
// distance, not the Euclidean ray length, not reversed-Z, not NDC). Computed from DCS's gProj of that view and frame
// (reverse-Z, infinite far plane, near 0.02 m: w = near / d), viewport MinDepth/MaxDepth undone first. Half-float
// precision: relative 2^-11 (0.3 mm at 0.6 m). The depth is that of DCS's opaque G-buffer + water, sampled at the same
// point as the mask (before the transparent passes: canopy and HUD glass are not in it; the world behind shows).
// Which camera: the cockpit texels were rendered by the COCKPIT camera, the world texels by DCS's (world) camera; with
// the dual camera (Depth boost, worldMinusCockpitM != 0) they differ by a lateral shift only, which leaves the depth
// along the view axis unchanged to well below half-float precision.
//
// Per view, beyond the mask's fields:
//  - pose: DCS's own XrCompositionLayerProjectionView pose of this view and frame (orientation x, y, z, w; position
//    x, y, z, metres), read from DCS's xrEndFrame before any API layer, in the space of DCS's projection layer
//    (slot.xrSpace, the XrSpace handle DCS submitted it with; the same handle a lower layer receives). It is the WORLD
//    camera (DCS renders with it; the pupil shift layer later gives the runtime its own poses back, so a lower layer
//    must not use the runtime poses it receives to undo this frame's projection). Valid with view_flags::pose.
//  - worldMinusCockpitM[3]: world camera centre minus cockpit camera centre, metres, in the VIEW's OWN SPACE with
//    OpenXR axes (x right, y up, z backward; the orientation of `pose`). The cockpit camera centre is therefore
//    pose.position - rotate(pose.orientation, worldMinusCockpitM) (cockpit_camera_position), with the same orientation.
//    0 when the dual camera did not move this view's cockpit (off, external view, not patched this frame).
//    view_flags::dual_camera when non-zero. A lateral vector in practice (|x| = h, y and z ~ 0).
//  - boostShiftM: Depth boost's shift of DCS's camera from the unboosted eye along the view's +x (the eye axis),
//    metres: -h for the left views (0, 2), +h for the right views (1, 3), h = (renderedIpd - baseIpd) / 2. With the
//    dual camera on, worldMinusCockpitM.x == boostShiftM. NaN when unknown (view_flags::boost clear: the dual camera
//    module is off, so the engine does not read the boost).
//  - jitterPx[2]: DLSS jitter of this view and frame in RENDER (source) texels, +x right, +y down. Sign: a point
//    appears in this frame's render at its unjittered position PLUS jitterPx; i.e. gProj = (unjittered projection of
//    xrFov) followed by a shift of the image by jitterPx. Measured as gProj's view-axis NDC minus the one of DCS's
//    submitted xrFov. NaN when unknown (view_flags::jitter clear: no CPU gProj or no submitted FOV for the view).
//  - xrFov[4]: DCS's submitted XrFovf of the view (angleLeft, angleRight, angleUp, angleDown; radians). fovTan (the
//    rect's edges) comes from gProj and so carries the jitter; xrFov does not.
//
// Pairing and synchronisation (the mask's, shared). slot.seq == the mask slot's maskSeq of the same index and frame
// (it is the fence value). Use a depth slot only when, for the frame being processed (displayTime T):
//   mask s = find_slot(maskHeader, T) >= 0, depth s' = find_slot(depthHeader, T) == s, depth.slot[s].seq ==
//   mask.slot[s].maskSeq, both headers usable (same generation: depth.generation == mask.generation), and the view
//   is valid in both. The mask slot also carries dcsvr_cockpit_mask::slot_flags::depth (16) when the companion holds
//   that slot's depth. The engine writes the depth header BEFORE the mask header of a commit, so a reader that finds
//   the mask slot finds the depth slot too. There is ONE fence: fenceHandle here is the SAME NT handle value as the
//   mask block's fenceHandle (header_flags::shares_mask_fence); Wait(fence, seq) once covers mask and depth. The
//   mask's release fence (when the reader uses it) holds the shared slot for both.
// Generation. Mask and depth textures are made together: the depth header's generation always equals the mask
// header's generation of the same layout; a reader re-opens both on any change.
// Held set (R85, semantics only; dcsvr_cockpit_mask.h "Held set"): switching CockpitDepthExport (or the mask) off
// keeps the depth textures and the fence alive; this block's last record keeps the generation and the handles with
// header_flags::active clear. Back on, the same generation and the same handles are published active (also while
// the mask stayed active in between with that generation). An unchanged generation across active 1 -> 0 -> 1 means
// the same objects (a reader may keep its opened set); a generation change means new ones. The same across the F10
// map and loading screens (R88: no relayout while no flight frame is drawn; the mask block's "Held set").
//
// Reader heartbeat (R67 rules, per block). readerHeartbeatQpc (offset 2040, the last 8 bytes) is the one field the
// reader writes (reader_heartbeat, outside the seqlock, FILE_MAP_READ | FILE_MAP_WRITE). The engine produces depth
// only while BOTH the mask block's and this block's heartbeats are fresh (<= 2 s); otherwise this block is idle
// (header_flags::idle; no depth write, no extra GPU work: the mask-only shader runs, or nothing when the mask is idle
// too). A reader that wants depth stores both heartbeats (>= once per second, whatever the headers say).
//
// Versions. version (1) changes only on an incompatible layout change. revision (contract_revision) grows with additive
// changes (new flags, a reserved field taken); a reader accepts any revision >= the one it was written for.
//
// Content time per slot (R74a, revision 2, additive: header.slotTime[3] took 144 bytes of the former reserved tail;
// every older offset, the size 2048 and version 1 are unchanged). header_flags::slot_time (64): the writer fills it.
// slotTime[s] describes the frame of slot[s] when slotTime[s].seq == slot[s].seq != 0 (slot_time_usable). The SAME
// record, byte for byte, is written into Local\DcsVrMotion_v1 (dcsvr_motion.h: header.slotTime) in the same commit, so
// a reader takes it from whichever companion it pairs (Dev: motion preferred). Fields (SlotTime, 48 bytes):
//  - poseQpc: QueryPerformanceCounter (header.qpcFrequency) at the ENTRY of DCS's own xrLocateViews call whose pose DCS
//    rendered this frame (the OpenXR dispatch entry slot 36 that Visualizer calls through, before any API layer; the
//    same table as the xrEndFrame hook). Which call: the one whose XrViewLocateInfo::displayTime equals this frame's
//    XrFrameEndInfo::displayTime; among several, the one whose returned view 0 orientation equals the orientation DCS
//    submitted for view 0 (time_flags::pose_matched); else the last such call before xrEndFrame. time_flags::
//    pose_observed when found; pose_ambiguous when more than one call had that displayTime (locateCalls counts them).
//  - poseReturnQpc: QPC when that call returned (Dev: jitter diagnostics). poseDisplayTime: its displayTime.
//  - simTime: DCS's model time in seconds (World.dll's exported wTime::modelTime, the value behind the Lua
//    DCS.getModelTime), read at that call's entry; time_flags::sim_observed when read. time_flags::sim_stable only when
//    the writer has proven live that simTime is tied 1:1 to the rendered frame AND it advanced since the previous
//    committed frame consistently with the pose clock; without it use poseQpc for world motion too.
//
// Self-contained: <atomic>, <cmath>, <cstddef>, <cstdint>, <cstring>. #pragma pack(8), little-endian. The name takes
// the optional suffix of the environment variable DCSVR_PERCEPTION_SHM_SUFFIX (as the mask; tests use it).
#pragma once

#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace dcsvr_cockpit_depth {

constexpr const wchar_t* block_name = L"Local\\DcsVrCockpitDepth_v1";
constexpr const char* name_suffix_variable = "DCSVR_PERCEPTION_SHM_SUFFIX";
constexpr std::uint32_t block_magic = 0x50444344u;  // "DCDP"
constexpr std::uint32_t block_version = 1;
constexpr std::uint32_t contract_revision = 2;      // 1 R69: the first published contract; 2 R74a: slotTime
constexpr int max_views = 4;
constexpr int slot_count = 3;
constexpr std::uint32_t format_r16g16_float = 34;   // DXGI_FORMAT_R16G16_FLOAT
constexpr float sky_depth_m = 65504.0f;             // .y of the sky / beyond half float
constexpr std::int64_t reader_heartbeat_period_ms = 1000;
constexpr std::int64_t reader_heartbeat_timeout_ms = 2000;

#pragma pack(push, 8)
struct DepthView {                // 128 bytes
    std::uint32_t flags;          // view_flags
    std::uint32_t x, y, w, h;     // the view's rect in the slot texture (texels): identical to the mask view's rect
    std::uint32_t srcW, srcH;     // DCS's depth viewport of the view (texels)
    std::uint32_t divisor;        // source texels per texel, each axis (as the mask)
    float fovTan[4];              // left, right, up, down tangents of the rect's outer edges (gProj, this frame; as the mask)
    float nearM;                  // the mask's near-depth threshold (a texel nearer than this is cockpit; 0: stencil only)
    float boostShiftM;            // DCS's camera minus the unboosted eye along +x (left -h, right +h); NaN unknown
    float worldMinusCockpitM[3];  // world camera centre minus cockpit camera centre, view space (x right, y up, z back)
    float jitterPx[2];            // DLSS jitter, render texels, +x right, +y down (image = unjittered + jitterPx); NaN unknown
    float poseOrientation[4];     // DCS's submitted pose of the view: orientation x, y, z, w (slot.xrSpace)
    float posePosition[3];        // ... position x, y, z (metres)
    float xrFov[4];               // DCS's submitted XrFovf: angleLeft, angleRight, angleUp, angleDown (radians)
    std::uint32_t reserved[2];    // 0
};

struct DepthSlot {                // 560 bytes
    std::uint64_t seq;            // == the mask slot's maskSeq of this frame (the fence value); 0: never written
    std::uint64_t engineFrame;    // the engine's frame counter (== the mask slot's)
    std::int64_t displayTime;     // XrFrameEndInfo::displayTime DCS submitted this frame with (slot_flags::display_time)
    std::int64_t qpcCommit;       // QueryPerformanceCounter at the commit
    std::uint32_t flags;          // slot_flags
    std::uint32_t viewMask;       // bit v: view[v] valid (holds this frame's depth)
    std::uint64_t xrSpace;        // the XrSpace handle of DCS's projection layer (poses are in it); 0 unknown
    DepthView view[max_views];
};

// R74a: content time of the frame of slot[s] (see "Content time per slot"); identical in dcsvr_motion.h.
struct SlotTime {                 // 48 bytes
    std::uint64_t seq;            // == slot[s].seq when this record belongs to that slot's frame; 0: none
    std::int64_t poseQpc;         // QPC at the entry of DCS's xrLocateViews whose pose DCS rendered (pose_observed)
    std::int64_t poseReturnQpc;   // QPC when that call returned
    std::int64_t poseDisplayTime; // that call's XrViewLocateInfo::displayTime (== slot displayTime when observed)
    double simTime;               // DCS model time, seconds, read at that call's entry (sim_observed)
    std::uint32_t flags;          // time_flags
    std::uint32_t locateCalls;    // DCS's xrLocateViews calls with this frame's displayTime
};

struct Header {                   // 2048 bytes
    std::uint64_t seq;                // seqlock: even = stable
    std::uint32_t magic;              // block_magic
    std::uint32_t version;            // block_version
    std::uint32_t structSize;         // sizeof(Header)
    std::uint32_t flags;              // header_flags
    std::uint32_t generation;         // == the mask header's generation (textures made together); never 0 while active
    std::uint32_t slotCount;          // slot_count
    std::uint32_t format;             // format_r16g16_float
    std::uint32_t atlasW, atlasH;     // every slot texture's size (== the mask's atlas size)
    std::uint32_t ownerPid;           // the process the handles belong to (DCS)
    std::uint64_t adapterLuid;        // LowPart | HighPart << 32 of DCS's adapter
    std::uint64_t textureHandle[slot_count];  // NT handles (D3D11_RESOURCE_MISC_SHARED_NTHANDLE), one per slot
    std::uint64_t fenceHandle;        // the mask block's fence NT handle (the same value; header_flags::shares_mask_fence)
    std::uint64_t newestSeq;          // the newest committed seq (0: none yet)
    std::int64_t heartbeatQpc;        // QueryPerformanceCounter at the last write
    std::int64_t qpcFrequency;        // QueryPerformanceFrequency
    std::uint32_t newestSlot;         // the slot of newestSeq
    std::uint32_t revision;           // contract_revision of the writer
    std::uint32_t reserved0[2];       // 0
    DepthSlot slot[slot_count];
    SlotTime slotTime[slot_count];    // R74a (revision 2, header_flags::slot_time): per slot, see slot_time_usable
    std::uint64_t reserved1[11];      // 0
    std::uint64_t readerHeartbeatQpc; // written by the READER only, outside the seqlock (R67 rules); 0 = no reader
};
#pragma pack(pop)

static_assert(sizeof(DepthView) == 128 && offsetof(DepthView, x) == 4 && offsetof(DepthView, srcW) == 20 &&
                  offsetof(DepthView, divisor) == 28 && offsetof(DepthView, fovTan) == 32 &&
                  offsetof(DepthView, nearM) == 48 && offsetof(DepthView, boostShiftM) == 52 &&
                  offsetof(DepthView, worldMinusCockpitM) == 56 && offsetof(DepthView, jitterPx) == 68 &&
                  offsetof(DepthView, poseOrientation) == 76 && offsetof(DepthView, posePosition) == 92 &&
                  offsetof(DepthView, xrFov) == 104 && offsetof(DepthView, reserved) == 120,
              "DepthView layout");
static_assert(sizeof(DepthSlot) == 560 && offsetof(DepthSlot, engineFrame) == 8 && offsetof(DepthSlot, displayTime) == 16 &&
                  offsetof(DepthSlot, qpcCommit) == 24 && offsetof(DepthSlot, flags) == 32 &&
                  offsetof(DepthSlot, viewMask) == 36 && offsetof(DepthSlot, xrSpace) == 40 && offsetof(DepthSlot, view) == 48,
              "DepthSlot layout");
static_assert(offsetof(Header, magic) == 8 && offsetof(Header, structSize) == 16 && offsetof(Header, flags) == 20 &&
                  offsetof(Header, generation) == 24 && offsetof(Header, format) == 32 && offsetof(Header, atlasW) == 36 &&
                  offsetof(Header, ownerPid) == 44 && offsetof(Header, adapterLuid) == 48 &&
                  offsetof(Header, textureHandle) == 56 && offsetof(Header, fenceHandle) == 80 &&
                  offsetof(Header, newestSeq) == 88 && offsetof(Header, heartbeatQpc) == 96 &&
                  offsetof(Header, qpcFrequency) == 104 && offsetof(Header, newestSlot) == 112 &&
                  offsetof(Header, revision) == 116 && offsetof(Header, slot) == 128 &&
                  offsetof(Header, slotTime) == 128 + 560 * 3 && offsetof(Header, reserved1) == 128 + 560 * 3 + 144 &&
                  sizeof(Header) == 2048,
              "Header layout");
static_assert(sizeof(SlotTime) == 48 && offsetof(SlotTime, poseQpc) == 8 && offsetof(SlotTime, poseReturnQpc) == 16 &&
                  offsetof(SlotTime, poseDisplayTime) == 24 && offsetof(SlotTime, simTime) == 32 &&
                  offsetof(SlotTime, flags) == 40 && offsetof(SlotTime, locateCalls) == 44 &&
                  offsetof(Header, slotTime) == 1808,
              "SlotTime layout (R74a): 48 bytes, slotTime at 1808");
static_assert(offsetof(Header, readerHeartbeatQpc) == 2040 && offsetof(Header, readerHeartbeatQpc) % 8 == 0 &&
                  offsetof(Header, readerHeartbeatQpc) + 8 == sizeof(Header),
              "readerHeartbeatQpc: the last 8 bytes, 8-aligned");

namespace header_flags {
constexpr std::uint32_t active = 1u;             // the export runs (textures and handles valid)
constexpr std::uint32_t fence = 2u;              // fenceHandle is set
constexpr std::uint32_t reader_heartbeat = 8u;   // the writer honours readerHeartbeatQpc
constexpr std::uint32_t idle = 16u;              // no fresh heartbeat (this block's or the mask's): no new slots
constexpr std::uint32_t shares_mask_fence = 32u; // fenceHandle is the mask block's fence; values = its maskSeq
constexpr std::uint32_t slot_time = 64u;         // R74a: the writer fills slotTime (revision >= 2)
}  // namespace header_flags
namespace time_flags {                           // R74a: SlotTime::flags (the same bits in dcsvr_motion.h)
constexpr std::uint32_t pose_observed = 1u;      // poseQpc / poseReturnQpc / poseDisplayTime hold the rendered pose's call
constexpr std::uint32_t pose_matched = 2u;       // its returned view 0 orientation equals DCS's submitted one
constexpr std::uint32_t pose_ambiguous = 4u;     // more than one call had this displayTime (locateCalls)
constexpr std::uint32_t sim_observed = 8u;       // simTime was read
constexpr std::uint32_t sim_stable = 16u;        // simTime is proven tied to the frame and advanced consistently
}  // namespace time_flags
namespace slot_flags {
constexpr std::uint32_t valid = 1u;          // committed, at least one view valid
constexpr std::uint32_t display_time = 2u;   // displayTime is DCS's xrEndFrame displayTime of this frame (pairable)
constexpr std::uint32_t late_commit = 4u;    // committed without the xrEndFrame hook: not pairable, no poses
constexpr std::uint32_t partial = 8u;        // a view of the mask slot has no depth in this frame
constexpr std::uint32_t dual_camera = 16u;   // some view has worldMinusCockpitM != 0 this frame
constexpr std::uint32_t poses = 32u;         // xrSpace and the views' pose / xrFov were read from DCS's xrEndFrame
}  // namespace slot_flags
namespace view_flags {
constexpr std::uint32_t valid = 1u;              // the rect holds this frame's depth of the view
constexpr std::uint32_t after_water = 2u;        // sampled at the view's WaterSeaPass end (as the mask)
constexpr std::uint32_t focus = 16u;             // a focus view (2 / 3)
constexpr std::uint32_t same_grid_as_mask = 32u; // rect, divisor, source size and fovTan equal the mask view's (always, v1)
constexpr std::uint32_t dual_camera = 64u;       // worldMinusCockpitM != 0
constexpr std::uint32_t jitter = 128u;           // jitterPx known
constexpr std::uint32_t pose = 256u;             // poseOrientation / posePosition / xrFov known
constexpr std::uint32_t boost = 512u;            // boostShiftM known
}  // namespace view_flags

// ---- Seqlock (the mask's protocol; copies bytes 8 .. 2039, never the reader's field) -----------------------------
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

// ---- Reader heartbeat (outside the seqlock) ------------------------------------------------------------------------
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

// The header describes a running export whose handles belong to process `pid`.
inline bool header_usable(const Header& h, std::uint32_t pid) {
    return h.magic == block_magic && h.version == block_version && h.structSize >= sizeof(Header) &&
           (h.flags & header_flags::active) && h.generation != 0 && h.slotCount == slot_count &&
           h.format == format_r16g16_float && h.ownerPid == pid && h.atlasW > 0 && h.atlasH > 0;
}

// The slot committed for the frame DCS submits with `displayTime`, or -1.
inline int find_slot(const Header& h, std::int64_t displayTime) {
    if (displayTime == 0) return -1;
    int best = -1;
    for (int i = 0; i < slot_count; ++i) {
        const DepthSlot& s = h.slot[i];
        const std::uint32_t need = slot_flags::valid | slot_flags::display_time;
        if ((s.flags & need) != need || s.displayTime != displayTime || s.seq == 0) continue;
        if (best < 0 || s.seq > h.slot[best].seq) best = i;
    }
    return best;
}

// Depth slot s pairs with the mask slot of the same index (maskSlotIndex from dcsvr_cockpit_mask::find_slot, its
// maskSeq and the mask header's generation): same index, same seq, same generation.
inline bool paired_with_mask(const Header& h, int s, int maskSlotIndex, std::uint64_t maskSeq, std::uint32_t maskGeneration) {
    return s >= 0 && s < slot_count && s == maskSlotIndex && maskSeq != 0 && h.slot[s].seq == maskSeq &&
           h.generation == maskGeneration;
}

// R74a: slotTime[s] describes slot s's frame (the writer fills it, and it was written in that slot's commit).
inline bool slot_time_usable(const Header& h, int s) {
    return s >= 0 && s < slot_count && (h.flags & header_flags::slot_time) && h.revision >= 2 &&
           h.slot[s].seq != 0 && h.slotTime[s].seq == h.slot[s].seq;
}

// R74a: seconds between two QPC stamps of the same block (e.g. poseQpc of captures A and B).
inline double qpc_seconds(const Header& h, std::int64_t fromQpc, std::int64_t toQpc) {
    return h.qpcFrequency > 0 ? double(toQpc - fromQpc) / double(h.qpcFrequency) : 0.0;
}

// View v of slot s is usable (its rect inside the atlas).
inline bool view_usable(const Header& h, int s, int v) {
    if (s < 0 || s >= slot_count || v < 0 || v >= max_views) return false;
    const DepthView& m = h.slot[s].view[v];
    return (m.flags & view_flags::valid) && (h.slot[s].viewMask & (1u << v)) && m.w > 0 && m.h > 0 &&
           m.x + m.w <= h.atlasW && m.y + m.h <= h.atlasH;
}

inline bool heartbeat_fresh(const Header& h, std::int64_t nowQpc, double maxAgeMs = 500.0) {
    if (h.qpcFrequency <= 0 || h.heartbeatQpc <= 0 || nowQpc < h.heartbeatQpc) return false;
    return double(nowQpc - h.heartbeatQpc) * 1000.0 / double(h.qpcFrequency) <= maxAgeMs;
}

// Texel (column, row) of view rect m for a direction given as tangents (x right, y up); false outside the rect.
inline bool tan_to_texel(const DepthView& m, float tx, float ty, std::uint32_t& col, std::uint32_t& row) {
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

// v rotated by unit quaternion q (x, y, z, w).
inline void rotate(const float q[4], const float v[3], float out[3]) {
    const float x = q[0], y = q[1], z = q[2], w = q[3];
    const float tx = 2.0f * (y * v[2] - z * v[1]), ty = 2.0f * (z * v[0] - x * v[2]), tz = 2.0f * (x * v[1] - y * v[0]);
    out[0] = v[0] + w * tx + (y * tz - z * ty);
    out[1] = v[1] + w * ty + (z * tx - x * tz);
    out[2] = v[2] + w * tz + (x * ty - y * tx);
}

// The cockpit camera's centre in slot.xrSpace: pose.position - rotate(pose.orientation, worldMinusCockpitM). Its
// orientation is the pose's. false without view_flags::pose.
inline bool cockpit_camera_position(const DepthView& m, float out[3]) {
    if (!(m.flags & view_flags::pose)) return false;
    float d[3];
    rotate(m.poseOrientation, m.worldMinusCockpitM, d);
    for (int i = 0; i < 3; ++i) out[i] = m.posePosition[i] - d[i];
    return true;
}

// The point at depth z (metres along the view axis) behind tangent (tx, ty) of the view, in the cockpit camera's view
// space (OpenXR axes: x right, y up, z backward): (tx * z, ty * z, -z).
inline void unproject(float tx, float ty, float z, float out[3]) {
    out[0] = tx * z;
    out[1] = ty * z;
    out[2] = -z;
}

}  // namespace dcsvr_cockpit_depth
