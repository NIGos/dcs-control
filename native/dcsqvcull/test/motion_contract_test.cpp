// Motion contract revision 4 (docs/FOCUS_MOTION.md): the header compiles on its own, the revision 3 layout is unchanged
// byte for byte (an older reader's view of the block), and the revision 4 reader helpers (focus views, depth plane,
// previous slot, per-view jitter proof) behave as documented. Header-only: no D3D, no DCS.
#include "../src/dcsvr_cockpit_depth.h"
#include "../src/dcsvr_motion.h"

#include <cmath>
#include <cstdio>

namespace {

int g_failures = 0;
void Check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_failures;
}

namespace cmv = dcsvr_motion;
namespace cdp = dcsvr_cockpit_depth;

// The revision 3 header as an older reader compiled it (dcsvr_motion.h before revision 4).
#pragma pack(push, 8)
struct MotionSlotR3 {
    std::uint64_t seq, engineFrame;
    std::int64_t displayTime, qpcCommit;
    std::uint64_t prevSeq;
    std::int64_t prevDisplayTime;
    std::uint32_t flags, viewMask;
    cmv::MotionView view[2];
};
struct HeaderR3 {
    std::uint64_t seq;
    std::uint32_t magic, version, structSize, flags;
    std::uint32_t generation;
    std::uint32_t slotCount, format, atlasW, atlasH, ownerPid;
    std::uint64_t adapterLuid;
    std::uint64_t textureHandle[3];
    std::uint64_t fenceHandle;
    std::uint64_t newestSeq;
    std::int64_t heartbeatQpc, qpcFrequency;
    std::uint32_t newestSlot, revision;
    std::uint32_t classFormat, reserved0;
    std::uint64_t classTextureHandle[3];
    std::uint64_t reserved1;
    MotionSlotR3 slot[3];
    cmv::SlotTime slotTime[3];
    std::uint64_t reserved2[148];
    std::uint64_t readerHeartbeatQpc;
};
#pragma pack(pop)

#define SAME_OFFSET(field) (offsetof(HeaderR3, field) == offsetof(cmv::Header, field))
static_assert(sizeof(HeaderR3) == sizeof(cmv::Header) && sizeof(MotionSlotR3) == sizeof(cmv::MotionSlot), "sizes");
static_assert(SAME_OFFSET(magic) && SAME_OFFSET(version) && SAME_OFFSET(structSize) && SAME_OFFSET(flags) &&
                  SAME_OFFSET(generation) && SAME_OFFSET(slotCount) && SAME_OFFSET(format) && SAME_OFFSET(atlasW) &&
                  SAME_OFFSET(atlasH) && SAME_OFFSET(ownerPid) && SAME_OFFSET(adapterLuid) &&
                  SAME_OFFSET(textureHandle) && SAME_OFFSET(fenceHandle) && SAME_OFFSET(newestSeq) &&
                  SAME_OFFSET(heartbeatQpc) && SAME_OFFSET(qpcFrequency) && SAME_OFFSET(newestSlot) &&
                  SAME_OFFSET(revision) && SAME_OFFSET(classFormat) && SAME_OFFSET(classTextureHandle) &&
                  SAME_OFFSET(slot) && SAME_OFFSET(slotTime) && SAME_OFFSET(readerHeartbeatQpc),
              "every revision 3 field keeps its offset");
static_assert(offsetof(HeaderR3, reserved2) == offsetof(cmv::Header, focusView), "revision 4 starts the old tail");
static_assert(cmv::periphery_views == 2 && cmv::focus_views == 2 && cmv::max_views == 4 && cmv::contract_revision == 4 &&
                  cmv::format_r16_float == 54 && cmv::block_version == 1 && cdp::contract_revision == 3 &&
                  cdp::header_flags::per_pixel_in_motion == 128 && cdp::block_version == 1 &&
                  cmv::view_flags::focus == cdp::view_flags::focus,
              "revision 4 constants");

// The older reader's view_usable (revision 3), on a revision 4 block.
bool ViewUsableR3(const HeaderR3& h, int s, int v) {
    if (s < 0 || s >= 3 || v < 0 || v >= 2) return false;
    const cmv::MotionView& m = h.slot[s].view[v];
    return (m.flags & cmv::view_flags::valid) && (h.slot[s].viewMask & (1u << v)) && m.w > 0 && m.h > 0 &&
           m.x + m.w <= h.atlasW && m.y + m.h <= h.atlasH;
}

cmv::MotionView Rect(std::uint32_t x, std::uint32_t y, std::uint32_t w, std::uint32_t h, std::uint32_t flags) {
    cmv::MotionView m{};
    m.flags = flags;
    m.x = x;
    m.y = y;
    m.w = m.srcW = w;
    m.h = m.srcH = h;
    m.fovTan[0] = -1.0f;
    m.fovTan[1] = 1.0f;
    m.fovTan[2] = 1.0f;
    m.fovTan[3] = -1.0f;
    return m;
}

// A revision 4 block in the recommended layout: periphery 624x616 stacked at x = 0, focus 1132x1120 side by side.
cmv::Header MakeR4(bool focus, bool depth) {
    cmv::Header h{};
    h.magic = cmv::block_magic;
    h.version = cmv::block_version;
    h.structSize = sizeof(cmv::Header);
    h.flags = cmv::header_flags::active | cmv::header_flags::fence | cmv::header_flags::shares_mask_fence |
              cmv::header_flags::class_atlas | cmv::header_flags::slot_time;
    h.generation = 7;
    h.slotCount = cmv::slot_count;
    h.format = cmv::format_r16g16_float;
    h.classFormat = cmv::format_r8_uint;
    h.atlasW = 624 + 2 * 1132;
    h.atlasH = 2 * 616;
    h.ownerPid = 42;
    h.revision = cmv::contract_revision;
    if (focus) h.flags |= cmv::header_flags::focus_views;
    if (depth) {
        h.flags |= cmv::header_flags::depth_plane;
        h.depthFormat = cmv::format_r16_float;
    }
    for (int s = 0; s < cmv::slot_count; ++s) {
        h.textureHandle[s] = 0x100 + s;
        h.classTextureHandle[s] = 0x200 + s;
        if (depth) h.depthTextureHandle[s] = 0x300 + s;
        cmv::MotionSlot& sl = h.slot[s];
        sl.seq = 10 + s;
        sl.displayTime = 1000 + 10 * s;
        sl.prevSeq = s ? 9 + s : 0;
        sl.prevDisplayTime = s ? 1000 + 10 * (s - 1) : 0;
        sl.flags = cmv::slot_flags::valid | cmv::slot_flags::display_time | cmv::slot_flags::jitter_free |
                   cmv::slot_flags::sky_is_infinity;
        const std::uint32_t pv = cmv::view_flags::valid | cmv::view_flags::after_water | cmv::view_flags::jitter_proven |
                                 (depth ? cmv::view_flags::depth_plane : 0u);
        sl.view[0] = Rect(0, 0, 624, 616, pv);
        sl.view[1] = Rect(0, 616, 624, 616, pv);
        sl.viewMask = 3u;
        if (focus) {
            // Focus left proven, focus right not yet (jitter_proven clear).
            const std::uint32_t fv = cmv::view_flags::valid | cmv::view_flags::after_water | cmv::view_flags::focus |
                                     (depth ? cmv::view_flags::depth_plane : 0u);
            h.focusView[s][0] = Rect(624, 0, 1132, 1120, fv | cmv::view_flags::jitter_proven);
            h.focusView[s][1] = Rect(624 + 1132, 0, 1132, 1120, fv);
            sl.viewMask |= 12u;
        }
        if (depth) sl.flags |= cmv::slot_flags::depth_plane;
    }
    return h;
}

}  // namespace

int main() {
    // Revision 3 compatibility: with neither option on, a revision 4 record is revision 3's bytes.
    {
        const cmv::Header h = MakeR4(false, false);
        unsigned char tail = 0;
        const unsigned char* p = reinterpret_cast<const unsigned char*>(&h) + offsetof(cmv::Header, focusView);
        for (std::size_t i = 0; i < offsetof(cmv::Header, readerHeartbeatQpc) - offsetof(cmv::Header, focusView); ++i)
            tail |= p[i];
        Check(tail == 0, "options off: the former reserved tail stays zero (an older reader sees revision 3 bytes)");
        Check(cmv::view_usable(h, 0, 0) && cmv::view_usable(h, 0, 1) && !cmv::view_usable(h, 0, 2) &&
                  !cmv::view_usable(h, 0, 3) && cmv::view_record(h, 0, 2) == nullptr && !cmv::depth_usable(h, 0, 0),
              "options off: views 0 / 1 only, no focus record, no depth plane");
    }
    // An older reader on a revision 4 block with everything on: its views 0 / 1 unchanged, the focus views invisible.
    {
        const cmv::Header h = MakeR4(true, true);
        HeaderR3 old{};
        static_assert(sizeof(old) == sizeof(h), "same size");
        std::memcpy(&old, &h, sizeof(h));
        Check(cmv::header_usable(h, 42) && ViewUsableR3(old, 1, 0) && ViewUsableR3(old, 1, 1) &&
                  !ViewUsableR3(old, 1, 2) && old.slot[1].view[1].y == 616 && old.revision == 4,
              "older reader: header usable, views 0 / 1 as before, bits 2 / 3 ignored");
    }
    // Revision 4 reader helpers.
    {
        cmv::Header h = MakeR4(true, true);
        Check(cmv::view_usable(h, 2, 2) && cmv::view_usable(h, 2, 3) && cmv::view_record(h, 2, 3) == &h.focusView[2][1] &&
                  cmv::view_record(h, 2, 1) == &h.slot[2].view[1] && cmv::view_record(h, 2, 4) == nullptr &&
                  cmv::view_record(h, 3, 0) == nullptr,
              "view_record / view_usable: 0 / 1 in the slot, 2 / 3 in focusView, out of range nullptr");
        Check(cmv::depth_usable(h, 1, 0) && cmv::depth_usable(h, 1, 3), "depth plane usable for periphery and focus");
        float mv[2] = {1.5f, -2.0f}, out[2] = {};
        Check(cmv::true_motion_view(h, 1, 0, mv, out) && out[0] == 1.5f && cmv::true_motion_view(h, 1, 2, mv, out) &&
                  !cmv::true_motion_view(h, 1, 3, mv, out),
              "true_motion_view: focus needs jitter_proven per view (2 proven, 3 not)");
        Check(cmv::prev_slot(h, 2) == 1 && cmv::prev_slot(h, 1) == 0 && cmv::prev_slot(h, 0) == -1,
              "prev_slot: the slot with seq == prevSeq and displayTime == prevDisplayTime");
        h.slot[1].displayTime = 999;
        Check(cmv::prev_slot(h, 2) == -1, "prev_slot: a reused slot (other displayTime) is not the previous capture");
        h = MakeR4(true, true);
        h.revision = 3;
        Check(!cmv::view_usable(h, 0, 2) && !cmv::depth_usable(h, 0, 0),
              "revision 3 writer: focus records and depth plane ignored even if the flags were set");
        h = MakeR4(true, true);
        h.depthFormat = 41;  // R32_FLOAT: not this contract's format
        Check(!cmv::depth_usable(h, 0, 0), "depth plane: an unknown format is not used");
        h = MakeR4(true, true);
        h.slot[0].viewMask &= ~8u;
        h.slot[0].flags |= cmv::slot_flags::focus_partial;
        Check(!cmv::view_usable(h, 0, 3) && cmv::view_usable(h, 0, 2), "focus_partial: the missing view is not usable");
        h = MakeR4(true, false);
        Check(cmv::view_usable(h, 0, 2) && !cmv::depth_usable(h, 0, 2), "focus without the depth plane");
        h.focusView[0][0].x = h.atlasW;  // a rect outside the atlas
        Check(!cmv::view_usable(h, 0, 2), "focus rect outside the atlas is not usable");
    }
    std::printf("\n%d failure(s)\n", g_failures);
    return g_failures ? 1 : 0;
}
