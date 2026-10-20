// DCS Control: Local\DcsVrPupilFov_v1, the pupil shift layer's FOV pairs per display time (standalone contract header:
// no dependency beyond <cstdint> and <cstddef>, so the engine module can vendor it as it is).
//
// The pupil shift OpenXR layer (native/pupil_shift) moves each eye's pose to the pupil (and to Depth boost's or the
// custom IPD's eye distance) and shifts its FOV in xrLocateViews: DCS renders and submits with those. In xrEndFrame the
// layer puts the runtime's original FOVs and poses back on the projection views, so Quad Views composes with the
// originals. This block gives, per display time, both: what DCS rendered with (shiftedFov) and what Quad Views composes
// with (originalFov), plus the pose delta between them.
//
// Writer: the layer only (one writer). Created by the layer at xrCreateInstance (CreateFileMapping: a reader that
// created it first is fine, same size), closed when the last instance is destroyed (enabled set to 0 and the entries
// cleared first). Little-endian, #pragma pack(8), named file mapping in the DCS process's session.
//
// Seqlock, the same as Local\DcsVrDepthBoost_v1 (perception_shm.h): seq is odd while the writer writes and even when it
// is done; a reader copies the whole block between two reads of seq (acquire) and keeps the copy only when both are the
// same even value. seq 0: never written. Then check magic, version and size.
//
// enabled: 1 while the layer may change the views (PupilShift Enabled=1 or DepthBoost Enabled=1, even when their
// in-flight hotkey has paused them: the entries are then identity pairs); 0 while it changes nothing (e.g. only
// PublishGaze is on): no entries are written then and the ring is cleared. No block at all: the layer is not loaded.
//
// Entries: a ring of ring_size, the newest at index (writeCount - 1) % ring_size; one entry per display time (a second
// xrLocateViews for the same time, or xrEndFrame's confirmation, rewrites the same entry). Written at xrLocateViews
// (before DCS renders that display time) for every located display time with valid tracking while enabled; a display
// time without an entry (tracking lost, too old, or disabled) was rendered with the runtime's own views. At xrEndFrame
// the layer rewrites the entry from the views DCS actually submitted and sets entry_flags::submitted: a view DCS changed
// itself (not the layer's) is then an identity pair with the submitted FOV.
//
// FOVs are XrFovf angles in radians (angleLeft, angleRight, angleUp, angleDown). Pose delta: positionDelta = shifted
// position - original position, metres, in the base space of xrLocateViews (XrViewLocateInfo::space);
// orientationDelta = conjugate(original orientation) * shifted orientation, quaternion x, y, z, w (the layer never
// rotates a view today: identity 0, 0, 0, 1).
//
// View order, as in XR_VARJO_quad_views: 0 left periphery, 1 right periphery, 2 left focus, 3 right focus; with two
// views only 0 and 1 are set (the rest zero).
//
// The name takes the optional suffix from DCSVR_PERCEPTION_SHM_SUFFIX (the offline harness only; empty in DCS).
#pragma once

#include <cstddef>
#include <cstdint>

namespace dcsvr_pupil_fov {

constexpr const wchar_t* block_name = L"Local\\DcsVrPupilFov_v1";
constexpr std::uint32_t magic_value = 0x564F4650u;  // "PFOV" in memory
constexpr std::uint32_t version_value = 1u;
constexpr std::uint32_t ring_size = 16u;

#pragma pack(push, 8)
struct View {
    float shiftedFov[4];        // what DCS rendered with (returned by xrLocateViews): left, right, up, down, radians
    float originalFov[4];       // what Quad Views composes with (the runtime's, put back at xrEndFrame)
    float positionDelta[3];     // shifted - original position, metres, base space of xrLocateViews
    float orientationDelta[4];  // conjugate(original) * shifted, x y z w (identity today)
    float reserved;             // 0
};

struct Entry {
    std::int64_t displayTime;   // XrTime; 0: empty slot
    std::uint32_t viewCount;    // 2 or 4
    std::uint32_t flags;        // entry_flags below
    View views[4];
};

struct Block {
    std::uint64_t seq;          // seqlock: even = stable, 0 = never written
    std::uint32_t magic;        // magic_value
    std::uint32_t version;      // version_value
    std::uint32_t size;         // sizeof(Block)
    std::uint32_t enabled;      // 1: the layer may shift views (entries written); 0: it changes nothing (no entries)
    std::uint32_t writerPid;    // GetCurrentProcessId() of the writer (DCS)
    std::uint32_t ringSize;     // ring_size
    std::uint64_t writeCount;   // entries appended so far (rewrites of the same display time not counted)
    std::int64_t newestDisplayTime;  // the newest entry's displayTime (0: none)
    std::uint32_t headerReserved[4];  // 0
    Entry entries[ring_size];
    std::uint32_t reservedTail[64];   // 0
};
#pragma pack(pop)

namespace entry_flags {
constexpr std::uint32_t pupil = 1u;      // the pupil shift moved the views (FOV shifted)
constexpr std::uint32_t ipd = 2u;        // the eye distance was moved (custom IPD or Depth boost): position only
constexpr std::uint32_t submitted = 4u;  // confirmed at xrEndFrame from the views DCS submitted
}  // namespace entry_flags

static_assert(sizeof(View) == 64 && offsetof(View, originalFov) == 16 && offsetof(View, positionDelta) == 32 &&
              offsetof(View, orientationDelta) == 44 && offsetof(View, reserved) == 60, "View layout");
static_assert(sizeof(Entry) == 272 && offsetof(Entry, viewCount) == 8 && offsetof(Entry, flags) == 12 && offsetof(Entry, views) == 16,
              "Entry layout");
static_assert(offsetof(Block, magic) == 8 && offsetof(Block, version) == 12 && offsetof(Block, size) == 16 &&
              offsetof(Block, enabled) == 20 && offsetof(Block, writerPid) == 24 && offsetof(Block, ringSize) == 28 &&
              offsetof(Block, writeCount) == 32 && offsetof(Block, newestDisplayTime) == 40 && offsetof(Block, headerReserved) == 48 &&
              offsetof(Block, entries) == 64 && offsetof(Block, reservedTail) == 4416 && sizeof(Block) == 4672, "Block layout");

// The entry of a display time in a consistent copy of the block, or nullptr.
inline const Entry* find(const Block& b, std::int64_t displayTime) {
    if (b.magic != magic_value || b.version != version_value || b.size != sizeof(Block) || !b.enabled || displayTime == 0) return nullptr;
    for (std::uint32_t i = 0; i < ring_size; ++i)
        if (b.entries[i].displayTime == displayTime) return &b.entries[i];
    return nullptr;
}

}  // namespace dcsvr_pupil_fov
