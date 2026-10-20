// DCS Control: cockpit mask export v1, the contract between DCS Control's engine module (DcsQvCull, the writer) and a
// frame-generation layer (OFXR Bridge, the reader). Design and rationale: docs/research/R65_cockpit_mask_export.md.
//
//   Local\DcsVrCockpitMask_v1   written by the engine module once per rendered frame ([Perception] CockpitMaskExport=1),
//                               read by the frame-generation layer at its xrEndFrame: where DCS drew cockpit geometry
//                               in each HMD view of that frame, as a small R8 mask on DCS's GPU.
//
// What the mask holds. One R8_UNORM texel per divisor x divisor block of DCS's G-buffer depth target of the view
// (DLSS: the render resolution, e.g. 476 x 468 periphery, 1176 x 1164 focus), max-filtered (conservative): 255 when any
// texel of the block (plus `dilate` blocks around it) is cockpit, 0 when none is. "Cockpit" = the stencil composition
// type COCKPIT (DCS's Bazar/shaders/common/stencil.hlsl: (stencil & 0x38) == 0x28, written by every opaque cockpit
// draw) or, when nearM > 0, a depth nearer than nearM metres along the view axis. Sampled after the view's opaque
// G-buffer and water passes and before its transparent passes, so canopy and HUD glass are NOT in it (DCS's
// transparent cockpit materials also write COCKPIT into the stencil; reading later would cover the whole canopy).
// 128 (v1.1, R74b) = overlay symbology: a world texel (not cockpit) on which DCS's OverlayHDRPass drew (the HUD and the
// other cockpit indicators DCS draws after DLSS with HUD_MFD_after_DLSS); see "Overlay symbology" below. A v1 reader
// treats any non-zero value as masked, which stays correct.
//
// Where. Each slot is one texture (an atlas): view v of the slot occupies the rect (x, y, w, h) of view[v]; its edges
// are the tangents fovTan (left, right, up, down; x right, y up, in the view's own frame) of DCS's projection of that
// view and frame. Views: 0 left periphery, 1 right periphery, 2 left focus, 3 right focus (XR_VARJO_quad_views order,
// as dcsvr_perception_shm.h); with Quad Views the stereo projection layer it submits has the periphery views' FOV, so a
// reader working on that stereo image uses views 0 / 1 and maps by tangent: u = (tanX - left) / (right - left),
// v = (up - tanY) / (up - down). Focus views (2 / 3) are present only when configured (CockpitMaskViews).
//
// Pairing. slot.displayTime is XrFrameEndInfo::displayTime of the xrEndFrame call that submits the frame the mask was
// drawn from: the engine reads it from DCS's own xrEndFrame call (the OpenXR dispatch entry DCS calls through, before
// any API layer) and commits the slot there, on the render thread, before the call goes down the layer chain. A layer
// below (Quad Views, the frame-generation layer) therefore always finds the slot of the frame it is ending already
// committed, with the same displayTime it receives. Without that hook (another Visualizer.dll build) the slot is
// committed at the next frame's first pass with displayTime 0 and without slot_flags::display_time: such a slot is
// published for diagnostics only (a reader must not pair it).
//
// Synchronisation (GPU). The textures and the fence are created on DCS's D3D11 device with NT handles (this process
// only: ownerPid). After the frame's last mask write the engine signals `fence` with slot.maskSeq on DCS's immediate
// context, in the xrEndFrame commit (so before Quad Views' composition and anything a lower layer queues on that
// context). A D3D12 reader opens the handles (ID3D12Device::OpenSharedHandle) and makes its queue Wait(fence,
// slot.maskSeq) before reading the slot; a D3D11 reader on DCS's device may read without waiting (one context, D3D11
// orders it). The engine reuses a slot after slot_count - 1 newer frames; a reader that holds a slot longer (or reads on
// another queue) signals `releaseFence` with the maskSeq it finished reading: the engine then never writes a slot whose
// maskSeq is above the completed value of releaseFence (that frame gets no mask instead; it never waits). A reader
// that never signals releaseFence (value stays 0) is not waited for: it must copy the slot out on its queue at once.
// While active, a frame skipped because releaseFence holds every slot still refreshes heartbeatQpc (at least every
// 100 ms; newestMaskSeq and the slots unchanged; R76), so a reader whose release is gated on a fresh header always gets
// to release: a reader that finds a fresh header but no slot for its frame should signal releaseFence with
// newestMaskSeq (it has nothing pending). Clarification only: no field, flag, size or version changes.
//
// Header updates. The block is guarded by the seqlock of dcsvr_perception_shm.h (seq odd while writing, even when
// stable; a reader copies between two equal even reads). One writer. The engine writes it at each commit and when it
// changes the textures (generation): the handles of an older generation stay open for at least 2 s after the
// generation changed, so a reader that opens the handles right after a consistent read never opens a closed one; a
// reader re-checks generation after opening and drops what it opened when it changed.
//
// Held set across an export off / on (R85; semantics only: no field, flag, size or version change). Switching an
// export (CockpitMaskExport, CockpitDepthExport, CockpitMotionExport) off no longer releases the shared objects:
// within one engine session on one D3D11 device the textures, both fences and their NT handles of mask, depth and
// motion stay alive while the export is off, and when it is on again the SAME generation is published with the SAME
// handle values. Meanwhile the block's last record keeps that generation and those handles with header_flags::active
// clear (no slot valid; maskSeq kept): unusable as before (header_usable needs active). So:
//  - active 1 -> 0 -> 1 with an unchanged generation: the handles are the ones of the opened set; a reader MAY keep
//    its opened set (textures, fences) across the inactive stretch and use it again (no new import); a reader that
//    drops its set on an inactive header and opens the handles again on active also stays correct (they are open).
//  - A generation change still means new objects: drop the old set (the old handles stay open >= 2 s, as above).
//  - The engine changes the generation (and releases the old objects) only on: a layout change (render size, DLSS
//    mode, divisor, views, a companion switched on that the held set lacks, the motion class byte), a device change
//    or a removed device, the engine payload's stop or reload, a fault. Such a release while the export is off
//    publishes a new non-zero generation with flags 0 and no handles first (R76), then the old handles' 2 s grace.
//  - No relayout while no flight frame is drawn (R88; semantics only). The F10 map, loading screens and menus render
//    at other sizes; a layout change seen there no longer releases the set: the block keeps the generation and the
//    handles with header_flags::active clear (as for an export off; mask, depth and motion alike), and the first
//    frame with the held layout again publishes it active under the SAME generation (nothing to import). A layout
//    that still differs on flight frames (a resolution or DLSS change made on the map) releases it as any layout
//    change: a new generation. A set not made yet is not made on the map either (the record stays inactive).
//  - Cost: the held set's video memory while the export is off (DLSS periphery atlas 952 x 468: mask ~1.3 MB, with
//    depth and motion ~13 MB). Why: every OPEN of a shared NT-handle texture adds its full size to Windows' PDH
//    "GPU Process Memory\Dedicated Usage" for good (also when the same allocation is opened again; nothing brings it
//    down). An accounting artefact, not real memory (nvidia-smi and DXGI CurrentUsage stay flat; Dev's import bench),
//    but tools and users read it: R84b ~100 MB/min while the export keys were switched. Fewer opens is the only lever,
//    so the generation changes only where it must, and a reader should keep its set across an inactive stretch.
//
// Staleness (reader rules). Use a slot only when header.flags has active, magic / version / structSize / ownerPid
// match, slot.flags has valid and display_time, slot.displayTime equals the displayTime of the frame being processed,
// and the view's view_flags has valid. Anything else (no block, inactive, generation change in flight, no matching
// slot, view not valid): no mask for that frame, i.e. today's behaviour; never a mask of another frame (the cockpit
// moves on screen with the head between frames). heartbeatQpc older than 500 ms (QueryPerformanceCounter, the same
// clock in the process): treat the export as off.
//
// Reader heartbeat (v1, additive; R67). readerHeartbeatQpc (offset 1016, the last 8 bytes of the block, formerly
// reserved1) is the one field the READER writes: QueryPerformanceCounter ticks (the same clock in the process; the
// writer converts with QueryPerformanceFrequency) of the reader's last "I want masks". It lies outside the seqlock: the
// reader stores it with reader_heartbeat() (one aligned 8-byte atomic store, release), the writer reads it with
// reader_alive() (acquire); seqlock_write never copies it, so the writer's header writes never overwrite it.
//  - Reader rules. While frame generation is active and consumes masks, store it at least once per second (every
//    xrEndFrame is fine: one store). Store it whenever the block is mapped, whatever header.flags says (not active,
//    idle, a stale heartbeatQpc): the writer only starts producing after it sees one, so gating the store on finding
//    masks would never start. Stop storing when generation is off or stalled. The mapping must be opened writable
//    (FILE_MAP_READ | FILE_MAP_WRITE) for this one field; nothing else in the block is ever written by the reader.
//  - Writer rules (header_flags::reader_heartbeat set). Once per frame, at the frame's first pass, the writer reads it:
//    0 or older than reader_heartbeat_timeout_ms (2 s) -> idle: no mask dispatch, no fence Signal, no header write per
//    frame, status bit 39 (and 40) clear, header_flags::idle set (one header write at the transition). A fresh value
//    -> active again from that frame on; the first frame after resuming may have no mask (a layout to (re)make).
//    While idle, heartbeatQpc (the writer's own) is not refreshed: a reader keeps storing its heartbeat regardless.
//  - Compatibility. The layout, the size (1024), the version (1) and every existing field are unchanged; the field was
//    reserved (0). An old reader never reads offset 1016 (seqlock_read copies it; header_usable and find_slot ignore
//    it), so it keeps parsing the block, but it never stores a heartbeat: with a writer that sets
//    header_flags::reader_heartbeat it sees an idle export (no new slots: no mask for any frame, its documented stale
//    path) and must adopt the heartbeat to get masks. A new reader with an old writer (no header_flags::reader_heartbeat):
//    the old writer's header writes copy the whole block and reset the field to 0 at every commit; harmless, the old
//    writer produces masks regardless.
//
// Overlay symbology (v1.1, R74b, additive: no layout change, version 1). With [Perception] CockpitMaskOverlay=1 (opt-in;
// default 0 until a live check with lit HUD symbology) the writer marks, per view, every mask texel that is 0 (world)
// and on which DCS's OverlayHDRPass of that view changed the HDR image (a per-texel signature of the view's HDR
// target, taken at the pass begin and at its end, over the texel's footprint in DCS's output resolution) with
// value_overlay (128). Cockpit texels stay 255. The
// see-through glass (canopy, HUD combiner) is not drawn in that pass, so it stays 0 where no symbol is: only the
// symbols themselves (and their anti-aliased edges) are 128. Safety: when the changed world texels exceed 40 % of the
// view's rect (a full-screen pass, a target DCS switched), the view gets no 128 that frame. view_flags::overlay (32):
// the view's overlay coverage was computed this frame (texels can be 128); slot_flags::overlay (32): some view has it.
// Without the flag a view carries 0 / 255 only (v1). Symbology drawn later, after the tone map (OverlayLDRPass,
// GUIPass), is not covered in v1.1.
//
// Depth companion (R69, v1 additive). With [Perception] CockpitDepthExport=1 the same pass also writes the cockpit
// depth into Local\DcsVrCockpitDepth_v1 (dcsvr_cockpit_depth.h): same slot index, same seq (maskSeq), same commit, the
// same fence. A mask slot whose depth was written carries slot_flags::depth (16); a v1 reader ignores the bit.
//
// Self-contained: <atomic>, <cstddef>, <cstdint>, <cstring>. #pragma pack(8), little-endian. The name takes the optional
// suffix of the environment variable DCSVR_PERCEPTION_SHM_SUFFIX (as the Perception blocks; tests use it).
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace dcsvr_cockpit_mask {

constexpr const wchar_t* block_name = L"Local\\DcsVrCockpitMask_v1";
constexpr const char* name_suffix_variable = "DCSVR_PERCEPTION_SHM_SUFFIX";
constexpr std::uint32_t block_magic = 0x4B4D4344u;  // "DCMK"
constexpr std::uint32_t block_version = 1;
constexpr int max_views = 4;
constexpr int slot_count = 3;
constexpr std::uint32_t format_r8_unorm = 61;  // DXGI_FORMAT_R8_UNORM
constexpr std::uint8_t value_world = 0;
constexpr std::uint8_t value_overlay = 128;  // v1.1 (R74b): overlay symbology over the world (view_flags::overlay)
constexpr std::uint8_t value_cockpit = 255;
constexpr std::int64_t reader_heartbeat_period_ms = 1000;   // the reader stores readerHeartbeatQpc at least this often
constexpr std::int64_t reader_heartbeat_timeout_ms = 2000;  // the writer goes idle when it is 0 or older than this

#pragma pack(push, 8)
struct MaskView {
    std::uint32_t flags;          // view_flags
    std::uint32_t x, y, w, h;     // the view's rect in the slot texture (texels)
    std::uint32_t srcW, srcH;     // the source: DCS's depth viewport of the view (texels)
    std::uint32_t divisor;        // source texels per mask texel, each axis (the last column / row may cover fewer)
    float fovTan[4];              // left, right, up, down tangents of the rect's outer edges (DCS's gProj, this frame)
    float nearM;                  // the near-depth threshold used (0: stencil only)
    std::uint32_t dilate;         // extra mask texels of max filter around each block (0: the block only)
    std::uint32_t reserved[2];    // 0
};

struct MaskSlot {
    std::uint64_t maskSeq;        // fence value signalled after this slot's writes (0: never written); grows by 1 per commit
    std::uint64_t engineFrame;    // the engine's frame counter
    std::int64_t displayTime;     // XrFrameEndInfo::displayTime DCS submitted this frame with (slot_flags::display_time)
    std::int64_t qpcCommit;       // QueryPerformanceCounter at the commit
    std::uint32_t flags;          // slot_flags
    std::uint32_t viewMask;       // bit v: view[v] valid in this slot (= view[v].flags & view_flags::valid)
    MaskView view[max_views];
};

struct Header {
    std::uint64_t seq;                // seqlock: even = stable
    std::uint32_t magic;              // block_magic
    std::uint32_t version;            // block_version
    std::uint32_t structSize;         // sizeof(Header)
    std::uint32_t flags;              // header_flags
    std::uint32_t generation;         // changes whenever the textures or the fences change (never 0 while active)
    std::uint32_t slotCount;          // slot_count
    std::uint32_t format;             // format_r8_unorm
    std::uint32_t atlasW, atlasH;     // every slot texture's size
    std::uint32_t ownerPid;           // the process the handles belong to (DCS)
    std::uint64_t adapterLuid;        // LowPart | HighPart << 32 of DCS's adapter
    std::uint64_t textureHandle[slot_count];  // NT handles (D3D11_RESOURCE_MISC_SHARED_NTHANDLE), one texture per slot
    std::uint64_t fenceHandle;        // NT handle of the ID3D11Fence (header_flags::fence), else 0
    std::uint64_t releaseFenceHandle; // NT handle of the reader's release fence (header_flags::release_fence), else 0
    std::uint64_t newestMaskSeq;      // the newest committed maskSeq (0: none yet)
    std::int64_t heartbeatQpc;        // QueryPerformanceCounter at the last write (commit or generation)
    std::int64_t qpcFrequency;        // QueryPerformanceFrequency
    std::uint32_t newestSlot;         // the slot of newestMaskSeq
    std::uint32_t reserved0;          // 0
    MaskSlot slot[slot_count];
    std::uint64_t readerHeartbeatQpc; // written by the READER only, outside the seqlock (reader_heartbeat / reader_alive):
                                      // QueryPerformanceCounter ticks of its last store, >= once per second while it
                                      // consumes masks; 0 = no reader. Formerly reserved1 (v1, additive).
};
#pragma pack(pop)

static_assert(sizeof(MaskView) == 64 && offsetof(MaskView, x) == 4 && offsetof(MaskView, srcW) == 20 &&
                  offsetof(MaskView, divisor) == 28 && offsetof(MaskView, fovTan) == 32 && offsetof(MaskView, nearM) == 48 &&
                  offsetof(MaskView, dilate) == 52,
              "MaskView layout");
static_assert(sizeof(MaskSlot) == 296 && offsetof(MaskSlot, engineFrame) == 8 && offsetof(MaskSlot, displayTime) == 16 &&
                  offsetof(MaskSlot, qpcCommit) == 24 && offsetof(MaskSlot, flags) == 32 && offsetof(MaskSlot, viewMask) == 36 &&
                  offsetof(MaskSlot, view) == 40,
              "MaskSlot layout");
static_assert(offsetof(Header, magic) == 8 && offsetof(Header, structSize) == 16 && offsetof(Header, generation) == 24 &&
                  offsetof(Header, format) == 32 && offsetof(Header, atlasW) == 36 && offsetof(Header, ownerPid) == 44 &&
                  offsetof(Header, adapterLuid) == 48 && offsetof(Header, textureHandle) == 56 &&
                  offsetof(Header, fenceHandle) == 80 && offsetof(Header, releaseFenceHandle) == 88 &&
                  offsetof(Header, newestMaskSeq) == 96 && offsetof(Header, heartbeatQpc) == 104 &&
                  offsetof(Header, qpcFrequency) == 112 && offsetof(Header, newestSlot) == 120 &&
                  offsetof(Header, slot) == 128 && sizeof(Header) == 1024,
              "Header layout");
static_assert(offsetof(Header, readerHeartbeatQpc) == 1016 && offsetof(Header, readerHeartbeatQpc) % 8 == 0 &&
                  sizeof(Header::readerHeartbeatQpc) == 8 && offsetof(Header, readerHeartbeatQpc) + 8 == sizeof(Header) &&
                  offsetof(Header, slot) + sizeof(MaskSlot) * slot_count == offsetof(Header, readerHeartbeatQpc),
              "readerHeartbeatQpc: the last 8 bytes, 8-aligned, right after the slots");

namespace header_flags {
constexpr std::uint32_t active = 1u;         // the export runs (textures and handles valid)
constexpr std::uint32_t fence = 2u;          // fenceHandle is set (ID3D11Device5 available); else read on DCS's device only
constexpr std::uint32_t release_fence = 4u;  // releaseFenceHandle is set
constexpr std::uint32_t reader_heartbeat = 8u;  // the writer honours readerHeartbeatQpc (produces only while it is fresh)
constexpr std::uint32_t idle = 16u;          // no fresh reader heartbeat: no new slots (textures stay valid when active)
}  // namespace header_flags
namespace slot_flags {
constexpr std::uint32_t valid = 1u;          // committed, at least one view valid
constexpr std::uint32_t display_time = 2u;   // displayTime is DCS's xrEndFrame displayTime of this frame (pairable)
constexpr std::uint32_t late_commit = 4u;    // committed at the next frame's first pass (no xrEndFrame hook): not pairable
constexpr std::uint32_t partial = 8u;        // a configured view is missing in this frame
constexpr std::uint32_t depth = 16u;         // R69 (v1, additive): the companion block Local\DcsVrCockpitDepth_v1
                                             // (dcsvr_cockpit_depth.h) holds this slot's depth, same index and seq
constexpr std::uint32_t overlay = 32u;       // R74b (v1.1, additive): some view has view_flags::overlay (128 texels)
}  // namespace slot_flags
namespace view_flags {
constexpr std::uint32_t valid = 1u;          // the rect holds this frame's mask of the view
constexpr std::uint32_t after_water = 2u;    // sampled at the view's WaterSeaPass end (else at its GBufferLightingPass)
constexpr std::uint32_t stencil = 4u;        // the stencil composition was read (else near depth only)
constexpr std::uint32_t near_depth = 8u;     // the near-depth test was used (nearM > 0)
constexpr std::uint32_t focus = 16u;         // a focus view (2 / 3)
constexpr std::uint32_t overlay = 32u;       // R74b (v1.1): overlay symbology computed for this view (texels may be 128)
}  // namespace view_flags

// R74b: a mask texel value: 0 world, 128 overlay symbology over the world (v1.1), 255 cockpit; a v1 reader's rule
// (non-zero = masked) is is_masked.
inline bool is_masked(std::uint8_t t) { return t != value_world; }
inline bool is_cockpit(std::uint8_t t) { return t >= 192; }
inline bool is_overlay(std::uint8_t t) { return t >= 64 && t < 192; }

// ---- Seqlock (same protocol as dcsvr_perception_shm.h) ------------------------------------------------------------------
static_assert(sizeof(std::atomic<std::uint64_t>) == 8 && std::atomic<std::uint64_t>::is_always_lock_free, "seq");

inline std::atomic<std::uint64_t>& seq_of(Header* h) { return *reinterpret_cast<std::atomic<std::uint64_t>*>(&h->seq); }

// The single writer: odd, data, even. value.seq and value.readerHeartbeatQpc (the reader's field) are ignored: the
// copy stops before readerHeartbeatQpc, so a reader's store is never lost.
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

// A reader: true with a consistent copy, false while the writer is busy (after `attempts` tries) or never written.
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

// ---- Reader heartbeat (outside the seqlock; both sides) ---------------------------------------------------------------
inline std::atomic<std::uint64_t>& reader_heartbeat_of(Header* h) {
    return *reinterpret_cast<std::atomic<std::uint64_t>*>(&h->readerHeartbeatQpc);
}

// The reader (on the mapped block, writable): "I consume masks" at QueryPerformanceCounter value nowQpc. One aligned
// 8-byte atomic store, release. At least once per reader_heartbeat_period_ms while consuming; stop when not.
inline void reader_heartbeat(Header* h, std::int64_t nowQpc) {
    reader_heartbeat_of(h).store(static_cast<std::uint64_t>(nowQpc), std::memory_order_release);
}

// The writer (on the mapped block): a reader stored a heartbeat within maxAgeMs of nowQpc (qpcFrequency: ticks per
// second). 0 (never / stopped) or older -> false. A stamp slightly ahead of nowQpc (stored after nowQpc was read)
// counts as fresh; one more than maxAgeMs ahead does not (garbage).
inline bool reader_alive(const Header* h, std::int64_t nowQpc, std::int64_t qpcFrequency,
                         std::int64_t maxAgeMs = reader_heartbeat_timeout_ms) {
    const std::uint64_t hb = reader_heartbeat_of(const_cast<Header*>(h)).load(std::memory_order_acquire);
    if (hb == 0 || qpcFrequency <= 0 || nowQpc <= 0) return false;
    const std::int64_t limit = qpcFrequency / 1000 * maxAgeMs + qpcFrequency % 1000 * maxAgeMs / 1000;
    const std::int64_t age = nowQpc - static_cast<std::int64_t>(hb);
    return age <= limit && -age <= limit;
}

// ---- Reader helpers (pure) ---------------------------------------------------------------------------------------------

// The header describes a running export whose handles belong to process `pid`.
inline bool header_usable(const Header& h, std::uint32_t pid) {
    return h.magic == block_magic && h.version == block_version && h.structSize >= sizeof(Header) &&
           (h.flags & header_flags::active) && h.generation != 0 && h.slotCount == slot_count &&
           h.format == format_r8_unorm && h.ownerPid == pid && h.atlasW > 0 && h.atlasH > 0;
}

// The slot committed for the frame DCS submits with `displayTime`, or -1 (no mask for that frame).
inline int find_slot(const Header& h, std::int64_t displayTime) {
    if (displayTime == 0) return -1;
    int best = -1;
    for (int i = 0; i < slot_count; ++i) {
        const MaskSlot& s = h.slot[i];
        const std::uint32_t need = slot_flags::valid | slot_flags::display_time;
        if ((s.flags & need) != need || s.displayTime != displayTime || s.maskSeq == 0) continue;
        if (best < 0 || s.maskSeq > h.slot[best].maskSeq) best = i;
    }
    return best;
}

// View v of slot s is usable (its rect inside the atlas).
inline bool view_usable(const Header& h, int s, int v) {
    if (s < 0 || s >= slot_count || v < 0 || v >= max_views) return false;
    const MaskView& m = h.slot[s].view[v];
    return (m.flags & view_flags::valid) && (h.slot[s].viewMask & (1u << v)) && m.w > 0 && m.h > 0 &&
           m.x + m.w <= h.atlasW && m.y + m.h <= h.atlasH;
}

// The heartbeat is recent: at most maxAgeMs before nowQpc.
inline bool heartbeat_fresh(const Header& h, std::int64_t nowQpc, double maxAgeMs = 500.0) {
    if (h.qpcFrequency <= 0 || h.heartbeatQpc <= 0 || nowQpc < h.heartbeatQpc) return false;
    return double(nowQpc - h.heartbeatQpc) * 1000.0 / double(h.qpcFrequency) <= maxAgeMs;
}

// Mask texel (column, row) of view rect m for a direction given as tangents (x right, y up); false outside the rect.
inline bool tan_to_texel(const MaskView& m, float tx, float ty, std::uint32_t& col, std::uint32_t& row) {
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

}  // namespace dcsvr_cockpit_mask
