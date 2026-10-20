// Offline tests for cockpit_mask.h (R65, [Perception] CockpitMaskExport) and its contract dcsvr_cockpit_mask.h:
// the block layout and the reader helpers (seqlock, pairing by displayTime, staleness, tangent mapping), the pure
// writer helpers (layout, FOV of a rect, slot choice), and the whole path on a WARP device through fixation_depth.h's
// boundary callback: four HMD views (R32G8X24 2D, R24G8, R32G8X24 array slice 1, R24G8 with cPerView at an offset)
// with a cockpit / water / model / near-model / sky stencil-and-depth pattern; the mask read back through its NT
// handle against a CPU reference of the max filter (divisor 2 and 4, dilate 1, near test on and off); the sample point
// (WaterSeaPass end, GBufferLightingPass begin, a Transparent* pass begin: the canopy glass that pass writes into the
// stencil stays out); DCS's bindings identical around every dispatch; the xrEndFrame commit (displayTime, fence
// value, the hook function itself with a fake next entry); the late commit without the hook; the reader's release
// fence holding a slot; a resolution change (new generation, the old handle still open for the grace period); the
// status bits; the uninstall (block inactive). R67: the reader heartbeat (layout, helpers, old-reader compatibility;
// idle without a heartbeat: no dispatch, no fence Signal, bit 39 clear; stale -> idle within 2 s; resume).
// R69: the depth companion (dcsvr_cockpit_depth.h): layout and reader helpers; the pure helpers (CPU depth reference,
// dual-camera shift, boost sign, jitter from gProj against the submitted FOV); on WARP the fused depth variant's atlas
// read through its NT handle after the shared fence against the CPU reference (linearise + min/max downsample, both
// divisors, dilate, near test), the mask unchanged by the fused variant, pairing with the mask slot (index, seq,
// generation, slot_flags::depth), one fence Signal per frame, poses / FOV / space parsed from a real XrFrameEndInfo,
// the DLSS jitter recovered, depth idle without its own heartbeat (mask still committing) and with the mask idle,
// DCS's CS UAV 1 put back, the late commit, the dump, the uninstall.
// R70: the motion companion (dcsvr_motion.h): layout and reader helpers; on WARP DCS's MV render targets (R16G16_FLOAT
// and RGBA16F) bound with the G-buffer's DSV, noted by the OM hooks, copied verbatim into the MV atlas of views 0 / 1
// (read through the NT handle after the shared fence), the class byte (stencil composition + sky bit) against the
// scene, pairing with the mask slot, prevSeq / prevDisplayTime, both jitters, the reset flags (first commit, after
// idle, relayout, skipped frame, camera cut), the late commit, idle without its heartbeat, the dump, the uninstall.
#pragma once

namespace mask65test {

using namespace fixdepth;
namespace cm = ::dcsvr_cockpit_mask;
namespace cdp = ::dcsvr_cockpit_depth;
namespace cmv = ::dcsvr_motion;
using fdtest::FakePass;
using fdtest::MakePass;
using fdtest::Rel;
using fdtest::View;

// ---------------------------------------------------------------------------
// Contract and pure helpers
// ---------------------------------------------------------------------------

// The pre-R67 v1 reader's seqlock_read (copies all 1024 bytes, offset 1016 included) and the pre-R67 writer's
// seqlock_write (copies all 1024 bytes after seq): what an old peer does with the block.
bool OldSeqlockRead(const cm::Header* block, cm::Header& out) {
  auto& seq = cm::seq_of(const_cast<cm::Header*>(block));
  for (int i = 0; i < 4; ++i) {
    const uint64_t before = seq.load(std::memory_order_acquire);
    if (before == 0) return false;
    if (before & 1u) continue;
    memcpy(&out, block, sizeof(cm::Header));
    std::atomic_thread_fence(std::memory_order_acquire);
    if (seq.load(std::memory_order_relaxed) == before) {
      out.seq = before;
      return true;
    }
  }
  return false;
}
void OldSeqlockWrite(cm::Header* block, const cm::Header& value) {
  auto& seq = cm::seq_of(block);
  const uint64_t start = seq.load(std::memory_order_relaxed) | 1u;
  seq.store(start, std::memory_order_relaxed);
  std::atomic_thread_fence(std::memory_order_release);
  memcpy(reinterpret_cast<unsigned char*>(block) + 8, reinterpret_cast<const unsigned char*>(&value) + 8, sizeof(cm::Header) - 8);
  std::atomic_thread_fence(std::memory_order_release);
  seq.store(start + 1, std::memory_order_release);
}

void TestReaderHeartbeatContract() {
  static_assert(offsetof(cm::Header, readerHeartbeatQpc) == 1016 && offsetof(cm::Header, readerHeartbeatQpc) % 8 == 0 &&
                    sizeof(cm::Header) == 1024 && sizeof(cm::Header{}.readerHeartbeatQpc) == 8,
                "readerHeartbeatQpc at 1016, 8-aligned, 8 bytes, block 1024");
  Check(offsetof(cm::Header, readerHeartbeatQpc) == 1016 && sizeof(cm::Header) == 1024 && cm::block_version == 1 &&
            cm::header_flags::reader_heartbeat == 8 && cm::header_flags::idle == 16,
        "cockpit mask heartbeat: readerHeartbeatQpc at offset 1016 (8-aligned, the last 8 bytes), size 1024, version 1");
  // reader_alive: 10 MHz QPC.
  const int64_t F = 10000000, now = 1000 * F;
  alignas(8) cm::Header h{};
  const bool zero = !cm::reader_alive(&h, now, F);
  cm::reader_heartbeat(&h, now);
  const bool fresh = cm::reader_alive(&h, now, F) && h.readerHeartbeatQpc == static_cast<uint64_t>(now);
  cm::reader_heartbeat(&h, now - 2 * F);
  const bool edge = cm::reader_alive(&h, now, F);
  cm::reader_heartbeat(&h, now - 2 * F - F / 1000);
  const bool stale = !cm::reader_alive(&h, now, F);
  cm::reader_heartbeat(&h, now + F / 2);
  const bool ahead = cm::reader_alive(&h, now, F);
  cm::reader_heartbeat(&h, now + 3 * F);
  const bool garbage = !cm::reader_alive(&h, now, F);
  cm::reader_heartbeat(&h, now);
  const bool noFreq = !cm::reader_alive(&h, now, 0) && cm::reader_alive(&h, now + F, F, 1000) &&
                      !cm::reader_alive(&h, now + F + 1, F, 1000);
  Check(zero && fresh && edge && stale && ahead && garbage && noFreq,
        "cockpit mask heartbeat: reader_alive (0 -> no reader; fresh; exactly 2 s fresh, 2.001 s stale; slightly ahead "
        "fresh, far ahead not; no frequency -> not)");
  // The writer's header writes never overwrite the reader's field; a reader store between two header reads does not
  // disturb the seqlock.
  cm::Header blk{}, v{};
  v.magic = cm::block_magic;
  v.version = cm::block_version;
  v.structSize = sizeof(cm::Header);
  v.flags = cm::header_flags::active | cm::header_flags::reader_heartbeat;
  v.generation = 3;
  v.slotCount = cm::slot_count;
  v.format = cm::format_r8_unorm;
  v.atlasW = 10;
  v.atlasH = 10;
  v.ownerPid = 77;
  v.slot[0].maskSeq = 9;
  v.slot[0].flags = cm::slot_flags::valid | cm::slot_flags::display_time;
  v.slot[0].displayTime = 4242;
  v.slot[0].viewMask = 1;
  v.readerHeartbeatQpc = 0;  // the writer's copy: never written into the block
  cm::reader_heartbeat(&blk, now);
  cm::seqlock_write(&blk, v);
  cm::Header r1{};
  const bool kept = blk.readerHeartbeatQpc == static_cast<uint64_t>(now) && cm::seqlock_read(&blk, r1) &&
                    cm::reader_alive(&blk, now, F);
  const uint64_t seqBefore = blk.seq;
  cm::reader_heartbeat(&blk, now + 1);
  cm::Header r2{};
  const bool seqOk = blk.seq == seqBefore && cm::seqlock_read(&blk, r2) && r2.generation == 3;
  Check(kept && seqOk, "cockpit mask heartbeat: seqlock_write keeps the reader's field; a reader store leaves the "
                       "seqlock untouched (outside it)");
  // Old reader (pre-R67 v1: copies all 1024 bytes, ignores offset 1016) on a block with a heartbeat: everything it
  // parses is identical to the same block without one.
  cm::Header oldWith{}, oldWithout{}, blk2{};
  cm::seqlock_write(&blk2, v);
  const bool oldRead = OldSeqlockRead(&blk, oldWith) && OldSeqlockRead(&blk2, oldWithout);
  oldWith.seq = oldWithout.seq = 0;
  const bool same = oldRead && memcmp(&oldWith, &oldWithout, offsetof(cm::Header, readerHeartbeatQpc)) == 0 &&
                    cm::header_usable(oldWith, 77) && cm::find_slot(oldWith, 4242) == 0 && cm::view_usable(oldWith, 0, 0) == false;
  // Old writer (pre-R67: copies all 1024 bytes): the field reset to 0 at its commit (harmless: it produces regardless);
  // a new reader sees no header_flags::reader_heartbeat from it.
  cm::Header oldV = v;
  oldV.flags = cm::header_flags::active;
  OldSeqlockWrite(&blk, oldV);
  cm::Header r3{};
  const bool oldWriter = blk.readerHeartbeatQpc == 0 && !cm::reader_alive(&blk, now, F) && cm::seqlock_read(&blk, r3) &&
                         !(r3.flags & cm::header_flags::reader_heartbeat) && cm::header_usable(r3, 77);
  Check(same && oldWriter, "cockpit mask heartbeat: an old v1 reader parses a block with a heartbeat exactly as one "
                           "without (header_usable, find_slot); an old writer resets the field (no reader_heartbeat flag)");
}

void TestContract() {
  Check(sizeof(cm::Header) == 1024 && sizeof(cm::MaskSlot) == 296 && sizeof(cm::MaskView) == 64 &&
            offsetof(cm::Header, slot) == 128,
        "cockpit mask contract: Header 1024 bytes, MaskSlot 296, MaskView 64, slots at 128");
  cm::Header blk{}, out{};
  Check(!cm::seqlock_read(&blk, out), "cockpit mask contract: a never-written block reads as no data");
  cm::Header h{};
  h.magic = cm::block_magic;
  h.version = cm::block_version;
  h.structSize = sizeof(cm::Header);
  h.flags = cm::header_flags::active;
  h.generation = 7;
  h.slotCount = cm::slot_count;
  h.format = cm::format_r8_unorm;
  h.atlasW = 300;
  h.atlasH = 120;
  h.ownerPid = 1234;
  h.slot[1].maskSeq = 5;
  h.slot[1].flags = cm::slot_flags::valid | cm::slot_flags::display_time;
  h.slot[1].displayTime = 1000;
  h.slot[1].viewMask = 1;
  h.slot[1].view[0] = cm::MaskView{cm::view_flags::valid, 0, 0, 150, 120, 300, 240, 2, {-1, 1, 1, -1}, 1.0f, 0, {}};
  h.slot[2] = h.slot[1];
  h.slot[2].maskSeq = 6;  // the same displayTime twice: the newer wins
  h.slot[0].maskSeq = 4;
  h.slot[0].flags = cm::slot_flags::valid | cm::slot_flags::late_commit;
  h.slot[0].displayTime = 0;
  cm::seqlock_write(&blk, h);
  const bool read = cm::seqlock_read(&blk, out);
  Check(read && (out.seq & 1) == 0 && out.seq > 0 && out.generation == 7 && out.slot[2].maskSeq == 6,
        "cockpit mask contract: seqlock round trip (even seq, data intact)");
  Check(cm::header_usable(out, 1234) && !cm::header_usable(out, 99), "cockpit mask contract: usable only in the owner process");
  cm::Header bad = out;
  bad.flags = 0;
  Check(!cm::header_usable(bad, 1234), "cockpit mask contract: inactive header not usable");
  bad = out;
  bad.version = 2;
  Check(!cm::header_usable(bad, 1234), "cockpit mask contract: another version not usable");
  Check(cm::find_slot(out, 1000) == 2 && cm::find_slot(out, 999) == -1 && cm::find_slot(out, 0) == -1,
        "cockpit mask contract: find_slot pairs by displayTime (newest of equals), never a late slot or time 0");
  Check(cm::view_usable(out, 2, 0) && !cm::view_usable(out, 2, 1) && !cm::view_usable(out, 0, 0),
        "cockpit mask contract: view_usable needs the view bit, the valid flag and a rect inside the atlas");
  out.slot[2].view[0].w = 400;
  Check(!cm::view_usable(out, 2, 0), "cockpit mask contract: a rect outside the atlas is not usable");
  cm::Header hb{};
  hb.qpcFrequency = 10000000;
  hb.heartbeatQpc = 50000000;
  Check(cm::heartbeat_fresh(hb, 50000000 + 4000000) && !cm::heartbeat_fresh(hb, 50000000 + 6000000) &&
            !cm::heartbeat_fresh(hb, 40000000),
        "cockpit mask contract: heartbeat fresh within 500 ms only");
  cm::MaskView mv{};
  mv.w = 100;
  mv.h = 50;
  mv.fovTan[0] = -1.0f;
  mv.fovTan[1] = 1.0f;
  mv.fovTan[2] = 0.5f;
  mv.fovTan[3] = -0.5f;
  uint32_t col = 0, row = 0;
  const bool a = cm::tan_to_texel(mv, -1.0f, 0.5f, col, row) && col == 0 && row == 0;
  const bool b = cm::tan_to_texel(mv, 0.0f, 0.0f, col, row) && col == 50 && row == 25;
  const bool cc = cm::tan_to_texel(mv, 0.999f, -0.499f, col, row) && col == 99 && row == 49;
  const bool d = !cm::tan_to_texel(mv, 1.0f, 0.0f, col, row) && !cm::tan_to_texel(mv, 0.0f, 0.6f, col, row);
  Check(a && b && cc && d, "cockpit mask contract: tan_to_texel (corners, centre, outside)");
  TestReaderHeartbeatContract();
}

// R69: the depth companion's contract.
void TestDepthContract() {
  Check(sizeof(cdp::Header) == 2048 && sizeof(cdp::DepthSlot) == 560 && sizeof(cdp::DepthView) == 128 &&
            offsetof(cdp::Header, slot) == 128 && offsetof(cdp::Header, readerHeartbeatQpc) == 2040 &&
            offsetof(cdp::DepthView, jitterPx) == 68 && offsetof(cdp::DepthView, poseOrientation) == 76 &&
            cdp::block_magic == 0x50444344u && cdp::format_r16g16_float == 34 && cdp::sky_depth_m == 65504.0f &&
            cdp::contract_revision >= 1 && cm::slot_flags::depth == 16,
        "cockpit depth contract: Header 2048 (slots at 128, reader heartbeat at 2040), DepthSlot 560, DepthView 128, "
        "'DCDP', R16G16_FLOAT, sky 65504; mask slot_flags::depth = 16");
  cdp::Header blk{}, out{};
  Check(!cdp::seqlock_read(&blk, out), "cockpit depth contract: a never-written block reads as no data");
  cdp::Header h{};
  h.magic = cdp::block_magic;
  h.version = cdp::block_version;
  h.structSize = sizeof(cdp::Header);
  h.flags = cdp::header_flags::active | cdp::header_flags::fence | cdp::header_flags::shares_mask_fence;
  h.generation = 9;
  h.slotCount = cdp::slot_count;
  h.format = cdp::format_r16g16_float;
  h.atlasW = 300;
  h.atlasH = 120;
  h.ownerPid = 1234;
  h.revision = cdp::contract_revision;
  h.slot[1].seq = 5;
  h.slot[1].flags = cdp::slot_flags::valid | cdp::slot_flags::display_time;
  h.slot[1].displayTime = 1000;
  h.slot[1].viewMask = 1;
  h.slot[1].view[0].flags = cdp::view_flags::valid;
  h.slot[1].view[0].w = 150;
  h.slot[1].view[0].h = 120;
  h.slot[0].seq = 4;
  h.slot[0].flags = cdp::slot_flags::valid | cdp::slot_flags::late_commit;
  const int64_t F = 10000000, now = 1000 * F;
  cdp::reader_heartbeat(&blk, now);
  cdp::seqlock_write(&blk, h);
  const bool rd = cdp::seqlock_read(&blk, out);
  Check(rd && (out.seq & 1) == 0 && out.generation == 9 && blk.readerHeartbeatQpc == static_cast<uint64_t>(now) &&
            cdp::reader_alive(&blk, now, F) && !cdp::reader_alive(&blk, now + 2 * F + F / 1000, F),
        "cockpit depth contract: seqlock round trip keeps the reader's heartbeat (outside the seqlock); reader_alive 2 s");
  cm::Header mh{};
  mh.format = cm::format_r8_unorm;
  Check(cdp::header_usable(out, 1234) && !cdp::header_usable(out, 99) && cdp::find_slot(out, 1000) == 1 &&
            cdp::find_slot(out, 0) == -1 && cdp::find_slot(out, 999) == -1 && cdp::view_usable(out, 1, 0) &&
            !cdp::view_usable(out, 1, 1) && cdp::paired_with_mask(out, 1, 1, 5, 9) &&
            !cdp::paired_with_mask(out, 1, 2, 5, 9) && !cdp::paired_with_mask(out, 1, 1, 6, 9) &&
            !cdp::paired_with_mask(out, 1, 1, 5, 8),
        "cockpit depth contract: usable in the owner process, find_slot by displayTime (never late / 0), view_usable, "
        "paired_with_mask needs the same index, seq and generation");
  cdp::Header bad = out;
  bad.format = cm::format_r8_unorm;
  cdp::Header bad2 = out;
  bad2.flags = 0;
  Check(!cdp::header_usable(bad, 1234) && !cdp::header_usable(bad2, 1234),
        "cockpit depth contract: another format or an inactive header is not usable");
  // Pose helpers: a 90 degree yaw (about +y) maps view +x to -z; the cockpit camera = pose - R * worldMinusCockpit.
  cdp::DepthView dv{};
  dv.flags = cdp::view_flags::pose;
  const float s2 = std::sqrt(0.5f);
  const float q[4] = {0, s2, 0, s2};
  memcpy(dv.poseOrientation, q, sizeof(q));
  dv.posePosition[0] = 1.0f;
  dv.posePosition[1] = 2.0f;
  dv.posePosition[2] = 3.0f;
  dv.worldMinusCockpitM[0] = -0.01f;
  float r[3], cpos[3];
  const float ex[3] = {1, 0, 0};
  cdp::rotate(q, ex, r);
  const bool rot = fdtest::Near(r[0], 0, 1e-6) && fdtest::Near(r[1], 0, 1e-6) && fdtest::Near(r[2], -1, 1e-6);
  const bool cam = cdp::cockpit_camera_position(dv, cpos) && fdtest::Near(cpos[0], 1.0, 1e-6) &&
                   fdtest::Near(cpos[1], 2.0, 1e-6) && fdtest::Near(cpos[2], 3.0 - 0.01, 1e-6);
  cdp::DepthView np{};
  float u3[3];
  cdp::unproject(0.5f, -0.25f, 2.0f, u3);
  Check(rot && cam && !cdp::cockpit_camera_position(np, cpos) && u3[0] == 1.0f && u3[1] == -0.5f && u3[2] == -2.0f,
        "cockpit depth contract: rotate, cockpit_camera_position (pose minus the rotated shift; needs the pose flag), "
        "unproject (OpenXR view axes)");
}

// R70: the motion companion's contract.
void TestMotionContract() {
  Check(sizeof(cmv::Header) == 2048 && sizeof(cmv::MotionSlot) == 184 && sizeof(cmv::MotionView) == 64 &&
            offsetof(cmv::Header, slot) == 160 && offsetof(cmv::Header, readerHeartbeatQpc) == 2040 &&
            offsetof(cmv::Header, classTextureHandle) == 128 && cmv::block_magic == 0x564D4344u &&
            cmv::format_r16g16_float == 34 && cmv::format_r8_uint == 62 && cmv::max_views == 2,
        "motion contract: Header 2048 (slots at 160, reader heartbeat at 2040), MotionSlot 184, MotionView 64, 'DCMV'");
  cmv::Header blk{}, out{};
  Check(!cmv::seqlock_read(&blk, out), "motion contract: a never-written block reads as no data");
  cmv::Header h{};
  h.magic = cmv::block_magic;
  h.version = cmv::block_version;
  h.structSize = sizeof(cmv::Header);
  h.flags = cmv::header_flags::active;
  h.generation = 4;
  h.slotCount = cmv::slot_count;
  h.format = cmv::format_r16g16_float;
  h.atlasW = 200;
  h.atlasH = 100;
  h.ownerPid = 77;
  h.slot[2].seq = 12;
  h.slot[2].flags = cmv::slot_flags::valid | cmv::slot_flags::display_time;
  h.slot[2].displayTime = 900;
  h.slot[2].prevSeq = 11;
  h.slot[2].prevDisplayTime = 880;
  h.slot[2].viewMask = 1;
  h.slot[2].view[0].flags = cmv::view_flags::valid;
  h.slot[2].view[0].w = 100;
  h.slot[2].view[0].h = 100;
  h.slot[2].view[0].jitterCurPx[0] = 0.25f;
  h.slot[2].view[0].jitterCurPx[1] = -0.5f;
  h.slot[2].view[0].jitterPrevPx[0] = -0.25f;
  h.slot[2].view[0].jitterPrevPx[1] = 0.5f;
  const int64_t F = 10000000, now = 1000 * F;
  cmv::reader_heartbeat(&blk, now);
  cmv::seqlock_write(&blk, h);
  const bool rd = cmv::seqlock_read(&blk, out) && blk.readerHeartbeatQpc == static_cast<uint64_t>(now) &&
                  cmv::reader_alive(&blk, now, F) && !cmv::reader_alive(&blk, now + 3 * F, F);
  cmv::Header bad = out;
  bad.format = cmv::format_r8_uint;
  const float raw[2] = {3.0f, -2.0f};
  float tm[2] = {}, tf[2] = {}, tu[2] = {};
  const bool tmOk = cmv::true_motion(cmv::slot_flags::includes_jitter, out.slot[2].view[0], raw, tm);
  const bool tfOk = cmv::true_motion(cmv::slot_flags::jitter_free, out.slot[2].view[0], raw, tf) && tf[0] == 3.0f &&
                    tf[1] == -2.0f;
  const bool tuOk = !cmv::true_motion(0, out.slot[2].view[0], raw, tu);
  cmv::Header rs = out;
  rs.slot[2].flags |= cmv::slot_flags::reset;
  Check(rd && cmv::header_usable(out, 77) && !cmv::header_usable(out, 78) && !cmv::header_usable(bad, 77) &&
            cmv::find_slot(out, 900) == 2 && cmv::find_slot(out, 0) == -1 && cmv::view_usable(out, 2, 0) &&
            !cmv::view_usable(out, 2, 1) && cmv::paired_with_mask(out, 2, 2, 12, 4) && !cmv::paired_with_mask(out, 2, 2, 12, 5) &&
            cmv::usable_for_pair(out, 2, 880) && !cmv::usable_for_pair(out, 2, 881) && !cmv::usable_for_pair(rs, 2, 880) &&
            tmOk && tm[0] == 3.0f - (-0.25f - 0.25f) && tm[1] == -2.0f - (0.5f + 0.5f) && tfOk && tuOk &&
            cmv::contract_revision >= 2 && cmv::slot_flags::jitter_free == 128,  // R74a: 3 (additive)
        "motion contract: seqlock keeps the reader's heartbeat; usable / find_slot / view_usable / paired_with_mask; "
        "usable_for_pair needs prevDisplayTime == the earlier capture and no reset; true_motion: mv with jitter_free "
        "(DCS, R70 T1), mv - (prev - cur) with includes_jitter, none without either; revision >= 2");
}

void TestPure() {
  // R71: the camera-cut check of the motion reset ignores a jump that DCS's floating-origin rebase explains.
  {
    cmask::MotionPrev a;
    cmask::Frame b;
    a.camOk = b.camOk = a.camOOk = b.camOOk = true;
    for (int k = 0; k < 3; ++k) a.camF[k] = b.camF[k] = k == 2 ? 1.0 : 0.0;
    const double step[3] = {2.2, 0.1, -0.3};  // one frame at about 200 m/s
    const double oA[3] = {1000, 0, 2000}, oB[3] = {3000, 0, -1000};  // gOrigin moved by (2000, 0, -3000)
    auto set = [&](double sgn, bool rebase) {
      for (int k = 0; k < 3; ++k) {
        a.camO[k] = oA[k];
        b.camO[k] = rebase ? oB[k] : oA[k];
        a.camC[k] = 10.0 * k;
        b.camC[k] = a.camC[k] + step[k] + (rebase ? sgn * (oA[k] - oB[k]) : 0.0);
      }
    };
    bool rb = false;
    set(1, true);
    const bool plus = !cmask::CameraCut(a, b, &rb) && rb;
    set(-1, true);
    const bool minus = !cmask::CameraCut(a, b, &rb) && rb;
    set(1, false);
    const bool steady = !cmask::CameraCut(a, b, &rb) && !rb;
    set(1, true);
    b.camC[0] += 400;  // the rebase plus a real 400 m jump: a cut
    const bool realJump = cmask::CameraCut(a, b, &rb) && !rb;
    set(1, true);
    b.camOOk = false;  // the origin unknown: as before R71, a cut
    const bool unknown = cmask::CameraCut(a, b, &rb) && !rb;
    set(1, false);
    b.camC[0] += 400;  // a jump without an origin change: a cut
    const bool jump = cmask::CameraCut(a, b, &rb);
    set(1, true);
    b.camF[0] = 1.0;  // a 45 degree turn during a rebase: still a cut
    b.camF[2] = 1.0;
    const bool turn = cmask::CameraCut(a, b, &rb);
    Check(plus && minus && steady && realJump && unknown && jump && turn,
          "motion (R71): a camera jump equal to the gOrigin rebase delta (either sign) is not a cut; a jump beyond it, "
          "an unknown origin, a jump without a rebase and a 45 degree turn still are");
    // R76: a 3.6 km teleport that DCS's rebase followed in the same frame: the relative camera did not move (raw jump
    // 0, under 50 m), the world did by the origin delta: a cut.
    for (int k = 0; k < 3; ++k) b.camF[k] = k == 2 ? 1.0 : 0.0;  // no turn (the case above turned)
    b.camOOk = true;  // both origins known (a case above cleared it)
    set(1, true);
    for (int k = 0; k < 3; ++k) b.camC[k] = a.camC[k];
    const bool teleport = cmask::CameraCut(a, b, &rb) && !rb;
    for (int k = 0; k < 3; ++k) b.camC[k] = a.camC[k] + 30.0;  // 52 m relative, the world 3.6 km: still a cut
    const bool teleport2 = cmask::CameraCut(a, b, &rb) && !rb;
    // A small rebase (10 m) during steady flight: no cut; not counted as a rebase (the raw jump alone was no cut).
    set(1, false);
    b.camO[0] = a.camO[0] - 10.0;
    for (int k = 0; k < 3; ++k) b.camC[k] = a.camC[k] + step[k] + (a.camO[k] - b.camO[k]);
    const bool smallRebase = !cmask::CameraCut(a, b, &rb) && !rb;
    Check(teleport && teleport2 && smallRebase,
          "motion (R76): with both gOrigin known and moved, the 50 m limit applies to the world jump (raw jump minus the "
          "origin delta): a teleport the rebase followed (raw jump 0 or 52 m) is a cut; a small rebase in steady "
          "flight is not");
  }
  // R76: xrEndFrame / xrLocateViews hooks displaced by another feature (one true original per slot, no chaining):
  // detected (slot_time no longer advertised), and the uninstall leaves the other feature's hook in its slot.
  {
    void* table[40] = {};
    void* const foreign = reinterpret_cast<void*>(static_cast<uintptr_t>(0x1234));
    table[cmask::kXeSlot] = reinterpret_cast<void*>(&cmask::H_EndFrame);
    table[36] = reinterpret_cast<void*>(&cmask::ctime::H_LocateViews);
    cmask::g_xeSlot = &table[cmask::kXeSlot];
    cmask::ctime::g_slot = &table[36];
    cmask::g_xrHooked = true;
    cmask::ctime::g_hooked = true;
    cmask::CheckXrSlots();
    const bool intact = cmask::g_xrHooked.load() && cmask::ctime::g_hooked.load() && !cmask::g_xeLost.load();
    const bool ftlWaits = ftl::XrSlotsOwnedElsewhere();
    table[cmask::kXeSlot] = foreign;
    table[36] = foreign;
    cmask::CheckXrSlots();
    const bool lost = !cmask::g_xrHooked.load() && !cmask::ctime::g_hooked.load() && cmask::g_xeLost.load();
    cmask::UninstallXr();
    const bool left = table[cmask::kXeSlot] == foreign && table[36] == foreign && !cmask::g_xeSlot &&
                      !cmask::ctime::g_slot && !cmask::g_xeLost.load() && !ftl::XrSlotsOwnedElsewhere();
    Check(intact && ftlWaits && lost && left,
          "R76 xr slots: our hooks in place -> hooked (FrameTimeline waits); replaced by another hook -> detected, "
          "slot_time no longer advertised; the uninstall leaves the other hook in its slot");
  }
  Check(cmask::MaskDim(476, 2) == 238 && cmask::MaskDim(477, 2) == 239 && cmask::MaskDim(1176, 4) == 294 &&
            cmask::MaskDim(5, 8) == 1 && cmask::MaskDim(0, 2) == 0 && cmask::MaskDim(10, 0) == 10 && cmask::MaskDim(10, 99) == 2,
        "cockpit mask: MaskDim = ceil(src / divisor), divisor clamped to 1..8");
  const uint32_t w[4] = {476, 476, 1176, 1176}, h[4] = {468, 468, 1164, 1164};
  cmask::Layout L;
  const bool l3 = cmask::MakeLayout(w, h, 3, 2, L) && L.atlasW == 476 && L.atlasH == 234 && L.x[1] == 238 &&
                  L.w[2] == 0 && L.views == 3;
  cmask::Layout L15;
  const bool l15 = cmask::MakeLayout(w, h, 15, 2, L15) && L15.atlasW == 238 * 2 + 588 * 2 && L15.atlasH == 582 &&
                   L15.x[2] == 476 && L15.x[3] == 476 + 588;
  const uint32_t w2[4] = {476, 0, 0, 0};
  cmask::Layout Lx;
  const bool missing = !cmask::MakeLayout(w2, h, 3, 2, Lx) && cmask::MakeLayout(w2, h, 1, 2, Lx) && Lx.atlasW == 238;
  const uint32_t big[4] = {20000, 1, 1, 1}, bigh[4] = {10, 1, 1, 1};
  const bool tooBig = !cmask::MakeLayout(big, bigh, 1, 1, Lx);
  Check(l3 && l15 && missing && tooBig && L.Same(L) && !L.Same(L15),
        "cockpit mask: MakeLayout (periphery pair, all four, a view not seen yet, > 16384, Same)");
  cmask::Layout La;
  const bool autoOk = cmask::MakeLayout(w, h, 15, 0, La) && La.div == 0 && La.dv[0] == 1 && La.dv[1] == 1 &&
                      La.dv[2] == 3 && La.dv[3] == 3 && La.w[0] == 476 && La.w[2] == 392 && La.h[2] == 388 &&
                      La.atlasW == 476 * 2 + 392 * 2 && La.atlasH == 468 && !La.Same(L15);
  Check(autoOk && cmask::ViewDiv(0, 512) == 1 && cmask::ViewDiv(0, 513) == 2 && cmask::ViewDiv(0, 2448) == 5 &&
            cmask::ViewDiv(0, 9000) == 8 && cmask::ViewDiv(3, 9000) == 3,
        "cockpit mask: divisor 0 = auto per view (<= 512 wide: DLSS periphery 476 -> 1, focus 1176 -> 3), explicit kept");
  // RectFov: a divisible viewport gives DCS's FOV exactly; a non-divisible one extends by the partial block.
  float P[16];
  fdtest::MakeProj(P, -1.2, 1.0, 1.1, -0.9, 0.05, 0, true);
  const Proj p = ParseProj(P);
  double f[4];
  DcsFov(p, f);
  const Viewport vp{0, 0, 400, 300, 0, 1};
  float r[4];
  const bool exact = cmask::RectFov(p, vp, 200, 150, 2, r) && fdtest::Near(r[0], f[0], 1e-5) &&
                     fdtest::Near(r[1], f[1], 1e-5) && fdtest::Near(r[2], f[2], 1e-5) && fdtest::Near(r[3], f[3], 1e-5);
  const Viewport vp2{0, 0, 401, 301, 0, 1};
  const bool ext = cmask::RectFov(p, vp2, 101, 76, 4, r) && fdtest::Near(r[0], f[0], 1e-5) &&
                   fdtest::Near(r[1], f[0] + (f[1] - f[0]) * 404.0 / 401.0, 1e-5) &&
                   fdtest::Near(r[3], f[2] - (f[2] - f[3]) * 304.0 / 301.0, 1e-5);
  const float gf[4] = {static_cast<float>(f[0]), static_cast<float>(f[1]), static_cast<float>(f[2]), static_cast<float>(f[3])};
  float r2[4];
  const bool fromFov = cmask::RectFovFromFov(gf, 401, 301, 101, 76, 4, r2) && fdtest::Near(r2[1], r[1], 1e-5) &&
                       fdtest::Near(r2[3], r[3], 1e-5);
  Check(exact && ext && fromFov, "cockpit mask: RectFov (exact for a divisible viewport, extended by the last partial "
                                 "block, the GazeOut path agrees)");
  const uint64_t s0[3] = {0, 0, 0}, s1[3] = {7, 5, 6}, s2[3] = {7, 8, 6};
  Check(cmask::PickSlot(s0, 0) == 0 && cmask::PickSlot(s1, 0) == 1 && cmask::PickSlot(s2, 0) == 2 &&
            cmask::PickSlot(s1, 4) == -1 && cmask::PickSlot(s1, 5) == 1 && cmask::PickSlot(s2, 7) == 2,
        "cockpit mask: PickSlot (oldest slot; held while the release fence is below its maskSeq; 0 = no reader fence)");
  const uint8_t st[16] = {0, 0, 0, 0, 0, 0x28, 0, 0, 0, 0, 0, 0, 0x29, 0, 0, 0x68};
  const float dist[16] = {9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 0.5f, 9, 9, 9, 9, 9};
  // 4x4 source, divisor 2: blocks (0,0) [texel 5 = 0x28] cockpit; (1,0) none; (0,1) [12 = 0x29 -> & 0x38 = 0x28];
  // (1,1) [15 = 0x68: selected bit + cockpit] cockpit and [10 at 0.5 m] near.
  const bool ref = cmask::RefTexel(st, dist, 4, 4, 2, 0, 0, 0, 0) == 255 && cmask::RefTexel(st, dist, 4, 4, 2, 0, 0, 1, 0) == 0 &&
                   cmask::RefTexel(st, dist, 4, 4, 2, 0, 0, 0, 1) == 255 && cmask::RefTexel(st, dist, 4, 4, 2, 0, 0, 1, 1) == 255 &&
                   cmask::RefTexel(st, dist, 4, 4, 2, 1, 0, 1, 0) == 255 && cmask::RefTexel(nullptr, dist, 4, 4, 2, 0, 1.0f, 1, 1) == 255 &&
                   cmask::RefTexel(nullptr, dist, 4, 4, 2, 0, 0.4f, 1, 1) == 0;
  Check(ref, "cockpit mask: CPU reference (composition bits only, selected-object bit ignored, near test, dilate)");
  // R69: the depth reference on the same 4x4 source (distances; 0 = sky).
  const float dist2[16] = {9, 0, 9, 9, 9, 0.7f, 9, 9, 9, 9, 0.5f, 9, 0.9f, 9, 9, 70000.0f};
  float o[2][4][2];
  cmask::RefDepth(st, dist2, 4, 4, 2, 0, 1.0f, 0, 0, o[0][0]);  // texel 5 cockpit at 0.7, sky at 1 -> far 65504
  cmask::RefDepth(st, dist2, 4, 4, 2, 0, 1.0f, 1, 0, o[0][1]);  // no cockpit
  cmask::RefDepth(st, dist2, 4, 4, 2, 0, 1.0f, 0, 1, o[0][2]);  // texel 12 cockpit at 0.9
  cmask::RefDepth(st, dist2, 4, 4, 2, 0, 1.0f, 1, 1, o[0][3]);  // texel 10 near 0.5, 15 cockpit beyond half -> sky
  cmask::RefDepth(st, dist2, 4, 4, 2, 0, 0.0f, 1, 1, o[1][0]);  // near test off: 15 (cockpit, sky depth) only
  cmask::RefDepth(nullptr, dist2, 4, 4, 2, 0, 0.0f, 1, 1, o[1][1]);  // no stencil, no near test: no cockpit
  cmask::RefDepth(st, dist2, 4, 4, 2, 1, 1.0f, 1, 0, o[1][2]);  // dilate 1: the whole 4x4
  const bool rd = o[0][0][0] == 0.7f && o[0][0][1] == 65504.0f && o[0][1][0] == 0 && o[0][1][1] == 9.0f &&
                  o[0][2][0] == 0.9f && o[0][2][1] == 9.0f && o[0][3][0] == 0.5f && o[0][3][1] == 65504.0f &&
                  o[1][0][0] == 65504.0f && o[1][1][0] == 0 && o[1][1][1] == 65504.0f && o[1][2][0] == 0.5f;
  Check(rd, "cockpit depth: CPU reference (nearest cockpit distance, 0 none; farthest distance, sky / beyond half "
            "float 65504; near test; dilate)");
  // R69: the dual-camera shift in view space and the boost sign. Camera axes: right = world x, up = y, forward = z.
  const double R[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
  const double dwL[3] = {0.0064, 0, 0}, dwR[3] = {-0.0064, 0, 0}, dwK[3] = {6.4, 0.2, -1.0};
  float wl[3], wr[3], wk[3];
  cmask::WorldMinusCockpit(dwL, R, 1.0, wl);
  cmask::WorldMinusCockpit(dwR, R, 1.0, wr);
  cmask::WorldMinusCockpit(dwK, R, 1000.0, wk);
  // A rotated camera: right = world z, up = y, forward = -x.
  const double R2[3][3] = {{0, 0, 1}, {0, 1, 0}, {-1, 0, 0}};
  const double dw2[3] = {0.5, 0, 0.0064};
  float wq2[3];
  cmask::WorldMinusCockpit(dw2, R2, 1.0, wq2);
  Check(fdtest::Near(wl[0], -0.0064, 1e-7) && wl[1] == 0 && wl[2] == 0 && fdtest::Near(wr[0], 0.0064, 1e-7) &&
            fdtest::Near(wk[0], -0.0064, 1e-7) && fdtest::Near(wk[1], -0.0002, 1e-7) && fdtest::Near(wk[2], -0.001, 1e-7) &&
            fdtest::Near(wq2[0], -0.0064, 1e-7) && fdtest::Near(wq2[2], -0.5, 1e-7) &&
            cmask::BoostShift(0, 0.0064) == -0.0064f && cmask::BoostShift(2, 0.0064) == -0.0064f &&
            cmask::BoostShift(1, 0.0064) == 0.0064f && cmask::BoostShift(3, 0.0064) == 0.0064f &&
            fdtest::Near(wl[0], cmask::BoostShift(0, 0.0064), 1e-7) && fdtest::Near(wr[0], cmask::BoostShift(1, 0.0064), 1e-7),
        "cockpit depth: world-minus-cockpit = -Dw in view space (x right, y up, z backward; DCS units / K), boost -h left "
        "/ +h right, equal to the shift's x with the dual camera");
  // R69: the jitter from gProj against the submitted FOV (MakeProj's ox / oy plus a known image shift).
  const double l = -1.2, rr = 1.0, u = 1.1, d = -0.9;
  float Pj[16];
  fdtest::MakeProj(Pj, l, rr, u, d, 0.02, 0, true);
  const double W = 604, H = 596, jx = 0.31, jy = -0.42;
  Pj[8] += static_cast<float>(2.0 * jx / W);  // a point's NDC x grows: the image moves right by jx texels
  Pj[9] += static_cast<float>(-2.0 * jy / H);  // NDC y up: the image moves down by jy texels
  const Proj pp = ParseProj(Pj);
  const float xf[4] = {static_cast<float>(std::atan(l)), static_cast<float>(std::atan(rr)), static_cast<float>(std::atan(u)),
                       static_cast<float>(std::atan(d))};
  float j[2] = {};
  const bool jok = cmask::JitterPx(pp.ox, pp.oy, pp.P[0][0], pp.P[1][1], W, H, xf, j) && fdtest::Near(j[0], jx, 2e-3) &&
                   fdtest::Near(j[1], jy, 2e-3);
  // The image shift really is +jitter: a tangent's pixel under the jittered projection minus the unjittered one.
  float P0[16];
  fdtest::MakeProj(P0, l, rr, u, d, 0.02, 0, true);
  double px1, py1, px0, py0;
  const Viewport vpp{0, 0, W, H, 0, 1};
  TanToPixel(pp, vpp, 0.3, -0.2, px1, py1);
  TanToPixel(ParseProj(P0), vpp, 0.3, -0.2, px0, py0);
  const bool sign = fdtest::Near(px1 - px0, j[0], 2e-3) && fdtest::Near(py1 - py0, j[1], 2e-3);
  const float xfWide[4] = {static_cast<float>(std::atan(-1.3)), xf[1], xf[2], xf[3]};
  float jb[2] = {};
  const bool mismatch = !cmask::JitterPx(pp.ox, pp.oy, pp.P[0][0], pp.P[1][1], W, H, xfWide, jb);
  const bool jTooBig = !cmask::JitterPx(pp.ox + 3.0 * 2.0 / W, pp.oy, pp.P[0][0], pp.P[1][1], W, H, xf, jb);
  Check(jok && sign && mismatch && jTooBig,
        "cockpit depth: DLSS jitter from gProj against the submitted FOV (render texels, +x right, +y down: the image "
        "is the unjittered one shifted by it); another FOV or more than 2 texels -> unknown");
}

// ---------------------------------------------------------------------------
// WARP pipeline
// ---------------------------------------------------------------------------

// The synthetic scene of view i at texel (x, y): stencil byte and forward distance (inf = sky).
struct Px {
  uint8_t st;
  float w;
};
Px ScenePx(int i, int x, int y, int W, int H, bool glass) {
  const int frameX = W / 5 + i;  // a canopy frame bar, 3 texels wide
  if (y >= H * 7 / 10) return {0x28, 0.6f};                  // instrument panel
  if (x >= frameX && x < frameX + 3) return {0x28, 0.8f};    // canopy frame
  if (x == W - 7 && y == H / 3) return {0x28, 0.7f};         // one isolated cockpit texel (max filter)
  if (x >= W / 2 && x < W / 2 + 6 && y >= H / 2 && y < H / 2 + 5) return {0x09, 0.5f};  // a near model (not cockpit)
  if (glass && y < H * 7 / 10 && y >= H / 10) return {0x28, 2000.0f};  // canopy glass (transparent pass): world behind
  if (y < H / 10) return {0x00, INFINITY};                   // sky
  if (x < W / 8) return {0x10, 300.0f};                       // water
  return {0x09, 500.0f};                                     // world model
}

// A typeless source texture of the view's scene (no bind flags), for CopyResource into the depth target.
ID3D11Texture2D* MakeScene(ID3D11Device* dev, const View& v, DXGI_FORMAT fmt, UINT slices, UINT slice, int i, bool glass) {
  const UINT bpp = fmt == DXGI_FORMAT_R32G8X24_TYPELESS ? 8 : 4;
  std::vector<uint8_t> px(static_cast<size_t>(v.w) * v.h * bpp * slices, 0);
  for (UINT s = 0; s < slices; ++s)
    for (int y = 0; y < v.h; ++y)
      for (int x = 0; x < v.w; ++x) {
        Px q = s == slice ? ScenePx(i, x, y, v.w, v.h, glass) : Px{0x28, 0.3f};  // other slice: all cockpit (junk)
        double tx, ty;
        const Proj p = ParseProj(v.P);
        TexelToTan(p, Viewport{0, 0, static_cast<double>(v.w), static_cast<double>(v.h), 0, 1}, x, y, tx, ty);
        const float dep = std::isinf(q.w) ? 0.0f : static_cast<float>(fdtest::ProjDepth(v.P, tx, ty, q.w));
        uint8_t* o = px.data() + ((static_cast<size_t>(s) * v.h + y) * v.w + x) * bpp;
        if (fmt == DXGI_FORMAT_R24G8_TYPELESS) {
          const uint32_t u = static_cast<uint32_t>(std::lround(std::clamp(dep, 0.0f, 1.0f) * 16777215.0)) |
                             (static_cast<uint32_t>(q.st) << 24);
          memcpy(o, &u, 4);
        } else {
          memcpy(o, &dep, 4);
          o[4] = q.st;
        }
      }
  D3D11_TEXTURE2D_DESC d{};
  d.Width = v.w;
  d.Height = v.h;
  d.MipLevels = 1;
  d.ArraySize = slices;
  d.Format = fmt;
  d.SampleDesc.Count = 1;
  d.Usage = D3D11_USAGE_DEFAULT;
  std::vector<D3D11_SUBRESOURCE_DATA> init(slices);
  for (UINT s = 0; s < slices; ++s)
    init[s] = {px.data() + static_cast<size_t>(s) * v.w * v.h * bpp, static_cast<UINT>(v.w * bpp), 0};
  ID3D11Texture2D* t = nullptr;
  if (FAILED(dev->CreateTexture2D(&d, init.data(), &t))) return nullptr;
  return t;
}

bool MakeTarget(ID3D11Device* dev, View& v, DXGI_FORMAT fmt, UINT slices, UINT slice) {
  D3D11_TEXTURE2D_DESC d{};
  d.Width = v.w;
  d.Height = v.h;
  d.MipLevels = 1;
  d.ArraySize = slices;
  d.Format = fmt;
  d.SampleDesc.Count = 1;
  d.Usage = D3D11_USAGE_DEFAULT;
  d.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
  if (FAILED(dev->CreateTexture2D(&d, nullptr, &v.tex))) return false;
  D3D11_DEPTH_STENCIL_VIEW_DESC dd{};
  dd.Format = fmt == DXGI_FORMAT_R32G8X24_TYPELESS ? DXGI_FORMAT_D32_FLOAT_S8X24_UINT : DXGI_FORMAT_D24_UNORM_S8_UINT;
  if (slices > 1) {
    dd.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2DARRAY;
    dd.Texture2DArray.FirstArraySlice = slice;
    dd.Texture2DArray.ArraySize = 1;
  } else {
    dd.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
  }
  return SUCCEEDED(dev->CreateDepthStencilView(v.tex, &dd, &v.dsv));
}

// The R8 texture's texels (row-major, atlas size).
std::vector<uint8_t> ReadR8(ID3D11Device* dev, ID3D11DeviceContext* c, ID3D11Texture2D* t) {
  D3D11_TEXTURE2D_DESC d{};
  t->GetDesc(&d);
  d.Usage = D3D11_USAGE_STAGING;
  d.BindFlags = 0;
  d.MiscFlags = 0;
  d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  ID3D11Texture2D* st = nullptr;
  std::vector<uint8_t> out;
  if (FAILED(dev->CreateTexture2D(&d, nullptr, &st))) return out;
  c->CopyResource(st, t);
  D3D11_MAPPED_SUBRESOURCE m{};
  if (SUCCEEDED(c->Map(st, 0, D3D11_MAP_READ, 0, &m))) {
    const uint8_t* b = static_cast<const uint8_t*>(m.pData);
    for (UINT y = 0; y < d.Height; ++y) out.insert(out.end(), b + y * m.RowPitch, b + y * m.RowPitch + d.Width);
    c->Unmap(st, 0);
  }
  st->Release();
  return out;
}

// Mismatching texels of view rect mv in atlas a (atlas width aw) against the CPU reference of view i's scene.
int CompareView(const std::vector<uint8_t>& a, uint32_t aw, const cm::MaskView& mv, int i, int W, int H, bool glass,
                int div, int dilate, float nearM) {
  std::vector<uint8_t> st(static_cast<size_t>(W) * H);
  std::vector<float> dist(static_cast<size_t>(W) * H);
  for (int y = 0; y < H; ++y)
    for (int x = 0; x < W; ++x) {
      const Px q = ScenePx(i, x, y, W, H, glass);
      st[static_cast<size_t>(y) * W + x] = q.st;
      dist[static_cast<size_t>(y) * W + x] = std::isinf(q.w) ? 0.0f : q.w;
    }
  int bad = 0;
  for (uint32_t r = 0; r < mv.h; ++r)
    for (uint32_t c = 0; c < mv.w; ++c) {
      const uint8_t want = cmask::RefTexel(st.data(), dist.data(), W, H, div, dilate, nearM, c, r);
      const size_t at = static_cast<size_t>(mv.y + r) * aw + mv.x + c;
      if (at >= a.size() || a[at] != want) ++bad;
    }
  return bad;
}

// R70: float -> IEEE half (round to nearest even; no denormals needed here).
uint16_t HalfOf(float f) {
  uint32_t x;
  memcpy(&x, &f, 4);
  const uint32_t sg = (x >> 16) & 0x8000u;
  const int e = static_cast<int>((x >> 23) & 0xff) - 127 + 15;
  const uint32_t m = x & 0x7fffffu;
  if (e <= 0) return static_cast<uint16_t>(sg);
  if (e >= 31) return static_cast<uint16_t>(sg | 0x7c00u);
  uint32_t h = sg | (static_cast<uint32_t>(e) << 10) | (m >> 13);
  if ((m & 0x1fffu) > 0x1000u || ((m & 0x1fffu) == 0x1000u && (h & 1u))) ++h;
  return static_cast<uint16_t>(h);
}

// R69: IEEE half -> float.
float HalfToFloat(uint16_t h) {
  const uint32_t sign = (h >> 15) & 1, exp = (h >> 10) & 31, man = h & 1023;
  float v;
  if (exp == 0)
    v = std::ldexp(static_cast<float>(man), -24);
  else if (exp == 31)
    v = man ? NAN : INFINITY;
  else
    v = std::ldexp(static_cast<float>(man | 1024), static_cast<int>(exp) - 25);
  return sign ? -v : v;
}

// R69: the R16G16_FLOAT texture's texels as float pairs (row-major, atlas size).
std::vector<float> ReadRG16F(ID3D11Device* dev, ID3D11DeviceContext* c, ID3D11Texture2D* t) {
  D3D11_TEXTURE2D_DESC d{};
  t->GetDesc(&d);
  d.Usage = D3D11_USAGE_STAGING;
  d.BindFlags = 0;
  d.MiscFlags = 0;
  d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  ID3D11Texture2D* st = nullptr;
  std::vector<float> out;
  if (FAILED(dev->CreateTexture2D(&d, nullptr, &st))) return out;
  c->CopyResource(st, t);
  D3D11_MAPPED_SUBRESOURCE m{};
  if (SUCCEEDED(c->Map(st, 0, D3D11_MAP_READ, 0, &m))) {
    for (UINT y = 0; y < d.Height; ++y) {
      const uint16_t* row = reinterpret_cast<const uint16_t*>(static_cast<const uint8_t*>(m.pData) + y * m.RowPitch);
      for (UINT x = 0; x < d.Width * 2; ++x) out.push_back(HalfToFloat(row[x]));
    }
    c->Unmap(st, 0);
  }
  st->Release();
  return out;
}

// R69: mismatching texels of depth view rect dv in atlas a (float pairs, atlas width aw) against the CPU reference of
// view i's opaque scene (half-float and depth-format tolerance: 0.6 % relative; 0 and 65504 exact), plus texels where
// the invariant ".x > 0 <=> mask 255" fails against mask atlas m (when given).
int CompareDepth(const std::vector<float>& a, uint32_t aw, const cdp::DepthView& dv, int i, int W, int H, int div,
                 int dilate, float nearM, const std::vector<uint8_t>* m, int* invariantBad) {
  std::vector<uint8_t> st(static_cast<size_t>(W) * H);
  std::vector<float> dist(static_cast<size_t>(W) * H);
  for (int y = 0; y < H; ++y)
    for (int x = 0; x < W; ++x) {
      const Px q = ScenePx(i, x, y, W, H, false);
      st[static_cast<size_t>(y) * W + x] = q.st;
      dist[static_cast<size_t>(y) * W + x] = std::isinf(q.w) ? 0.0f : q.w;
    }
  int bad = 0, inv = 0;
  auto close = [](float got, float want) {
    if (want == 0 || want == 65504.0f) return got == want;
    return std::fabs(got - want) <= 6e-3f * want;
  };
  for (uint32_t r = 0; r < dv.h; ++r)
    for (uint32_t c = 0; c < dv.w; ++c) {
      float want[2];
      cmask::RefDepth(st.data(), dist.data(), W, H, div, dilate, nearM, c, r, want);
      const size_t at = static_cast<size_t>(dv.y + r) * aw + dv.x + c;
      if (at * 2 + 1 >= a.size() || !close(a[at * 2], want[0]) || !close(a[at * 2 + 1], want[1])) ++bad;
      if (m && at < m->size() && ((a[at * 2] > 0) != ((*m)[at] == 255))) ++inv;
    }
  if (invariantBad) *invariantBad = inv;
  return bad;
}

std::atomic<int> g_fakeEndCalls{0};
std::atomic<int64_t> g_fakeEndTime{0};
int32_t __stdcall FakeNextEndFrame(void* session, const void* info) {
  g_fakeEndCalls++;
  g_fakeEndTime = info ? *reinterpret_cast<const int64_t*>(static_cast<const uint8_t*>(info) + 16) : 0;
  return session == reinterpret_cast<void*>(0x1234) ? 0 : -1;
}

// ---------------------------------------------------------------------------
// R74a: content time (cockpit_time.h) and R74b: mask v1.1 overlay (cockpit_overlay.h), contract and pure parts
// ---------------------------------------------------------------------------

// The runtime below DCS's xrLocateViews: returns two views with orientation g_fakeLvQ (rc g_fakeLvRc), notes the args.
float g_fakeLvQ[4] = {0, 0, 0, 1};
int32_t g_fakeLvRc = 0;
int g_fakeLvCalls = 0;
const void* g_fakeLvInfo = nullptr;
uint32_t g_fakeLvCap = 0;
int32_t __stdcall FakeLocate(void* session, const void* info, void* state, uint32_t cap, uint32_t* count, void* views) {
  ++g_fakeLvCalls;
  g_fakeLvInfo = info;
  g_fakeLvCap = cap;
  (void)session;
  (void)state;
  if (g_fakeLvRc < 0) return g_fakeLvRc;
  if (count) *count = 2;
  auto* v = static_cast<uint8_t*>(views);
  for (uint32_t i = 0; v && i < 2 && i < cap; ++i) {
    *reinterpret_cast<int32_t*>(v + 64 * i) = 7;
    memcpy(v + 64 * i + 16, g_fakeLvQ, 16);
  }
  return g_fakeLvRc;
}
// DCS's call: XrViewLocateInfo (type 6, displayTime at 24) and two XrViews.
int32_t CallLocate(int64_t displayTime, const float q[4], int32_t rc = 0) {
  alignas(8) uint8_t info[40] = {};
  *reinterpret_cast<int32_t*>(info) = 6;
  *reinterpret_cast<int64_t*>(info + 24) = displayTime;
  alignas(8) uint8_t views[128] = {};
  uint32_t count = 0;
  memcpy(g_fakeLvQ, q, 16);
  g_fakeLvRc = rc;
  return cmask::ctime::H_LocateViews(reinterpret_cast<void*>(0x77), info, nullptr, 2, &count, views);
}

void TestR74Contract() {
  Check(sizeof(cdp::SlotTime) == 48 && sizeof(cmv::SlotTime) == 48 && offsetof(cdp::Header, slotTime) == 1808 &&
            offsetof(cmv::Header, slotTime) == 712 && sizeof(cdp::Header) == 2048 && sizeof(cmv::Header) == 2048 &&
            offsetof(cdp::Header, readerHeartbeatQpc) == 2040 && offsetof(cmv::Header, readerHeartbeatQpc) == 2040 &&
            offsetof(cdp::SlotTime, simTime) == 32 && offsetof(cmv::SlotTime, flags) == 40 &&
            cdp::contract_revision == 2 && cmv::contract_revision == 3 && cdp::header_flags::slot_time == 64 &&
            cmv::header_flags::slot_time == 128 && cdp::time_flags::pose_observed == cmv::time_flags::pose_observed &&
            cdp::time_flags::sim_stable == 16 && cmv::time_flags::sim_stable == 16,
        "R74a contract: SlotTime 48 bytes at depth 1808 / motion 712 (sizes 2048, heartbeat 2040 unchanged), revisions "
        "2 / 3, header_flags::slot_time 64 / 128, the same time_flags");
  cdp::Header d{};
  d.flags = cdp::header_flags::slot_time;
  d.revision = 2;
  d.slot[1].seq = 9;
  d.slotTime[1].seq = 9;
  cdp::Header dOld = d;
  dOld.revision = 1;
  cdp::Header dNo = d;
  dNo.flags = 0;
  cdp::Header dStale = d;
  dStale.slotTime[1].seq = 8;
  cmv::Header m{};
  m.flags = cmv::header_flags::slot_time;
  m.revision = 3;
  m.slot[2].seq = 4;
  m.slotTime[2].seq = 4;
  Check(cdp::slot_time_usable(d, 1) && !cdp::slot_time_usable(d, 0) && !cdp::slot_time_usable(dOld, 1) &&
            !cdp::slot_time_usable(dNo, 1) && !cdp::slot_time_usable(dStale, 1) && !cdp::slot_time_usable(d, 3) &&
            cmv::slot_time_usable(m, 2) && !cmv::slot_time_usable(m, 1),
        "R74a contract: slot_time_usable needs the flag, the revision, and slotTime.seq == slot.seq != 0");
  cmv::SlotTime a{}, bt{};
  a.flags = bt.flags = cmv::time_flags::pose_observed;
  a.poseQpc = 1000;
  bt.poseQpc = 1400;
  double fr = -1;
  const bool f1 = cmv::content_fraction(a, bt, 1200, fr) && fdtest::Near(fr, 0.5, 1e-12);
  const bool f2 = cmv::content_fraction(a, bt, 1500, fr) && fdtest::Near(fr, 1.25, 1e-12);
  cmv::SlotTime nb = bt;
  nb.flags = 0;
  const bool f3 = !cmv::content_fraction(a, nb, 1200, fr) && !cmv::content_fraction(bt, a, 1200, fr);
  d.qpcFrequency = 1000;
  Check(f1 && f2 && f3 && fdtest::Near(cdp::qpc_seconds(d, 1000, 1500), 0.5, 1e-12),
        "R74a contract: content_fraction (0 at A's pose, 1 at B's, extrapolates; needs both observed, B after A); "
        "qpc_seconds");
  Check(cm::value_overlay == 128 && cm::slot_flags::overlay == 32 && cm::view_flags::overlay == 32 &&
            cm::is_masked(128) && cm::is_masked(255) && !cm::is_masked(0) && cm::is_overlay(128) && !cm::is_overlay(255) &&
            cm::is_cockpit(255) && !cm::is_cockpit(128) && sizeof(cm::Header) == 1024 && cm::block_version == 1,
        "R74b contract: v1.1 value 128 (overlay), slot / view flag 32; a v1 reader's non-zero rule masks it; the mask "
        "block layout and version unchanged");
}

void TestR74Pure() {
  using cmask::ctime::Rec;
  Rec r[6];
  const float q1[4] = {0, 0.1f, 0, 0.99f}, q2[4] = {0, 0.2f, 0, 0.98f}, q2n[4] = {0, -0.2f, 0, -0.98f};
  auto set = [](Rec& x, int64_t q0, int64_t dt, const float q[4]) {
    x.q0 = q0;
    x.q1 = q0 + 5;
    x.dt = dt;
    memcpy(x.q, q, 16);
    x.qOk = true;
  };
  set(r[0], 100, 7000, q1);
  set(r[1], 200, 7000, q2);
  set(r[2], 300, 7000, q1);  // later, another orientation (a second locate DCS did not render with)
  set(r[3], 250, 7011, q2);  // the next frame's
  set(r[4], 900, 7000, q2);  // after the commit time
  const cmask::ctime::Pick p = cmask::ctime::PickLocate(r, 6, 7000, q2, 500);
  const cmask::ctime::Pick pn = cmask::ctime::PickLocate(r, 6, 7000, q2n, 500);  // the same rotation, negated
  const float q3[4] = {1, 0, 0, 0};
  const cmask::ctime::Pick pl = cmask::ctime::PickLocate(r, 6, 7000, q3, 500);
  const cmask::ctime::Pick pu = cmask::ctime::PickLocate(r, 6, 7000, nullptr, 500);
  const cmask::ctime::Pick p0 = cmask::ctime::PickLocate(r, 6, 6000, q2, 500);
  const cmask::ctime::Pick pz = cmask::ctime::PickLocate(r, 6, 0, q2, 500);
  Check(p.idx == 1 && p.matched && p.calls == 3 && pn.idx == 1 && pn.matched && pl.idx == 2 && !pl.matched &&
            pl.calls == 3 && pu.idx == 2 && !pu.matched && p0.idx == -1 && p0.calls == 0 && pz.idx == -1,
        "R74a pairing: the frame's displayTime, before the commit; the call whose view 0 orientation is DCS's submitted "
        "one (either sign), else the last; another frame's or a later call never; none -> -1");
  using cmask::ctime::SimStep;
  Check(SimStep(0.0111, 0.0111) && SimStep(0.022, 0.0222) && !SimStep(0, 0.0111) && !SimStep(-0.01, 0.0111) &&
            !SimStep(0.05, 0.0111) && !SimStep(0.0111, 0),
        "R74a sim_stable step: advanced by 0.5-1.5x the pose step (+1 ms)");
  // The hook: records entry / return QPC, displayTime, view 0 orientation, model time; forwards unchanged.
  cmask::ctime::g_orig = &FakeLocate;
  cmask::ctime::g_testRecordAlways = true;
  cmask::ctime::g_on = true;
  static double model = 42.5;
  cmask::ctime::g_modelTime = &model;
  const uint32_t n0 = cmask::ctime::g_ringN;
  g_fakeLvCalls = 0;
  const int32_t rc = CallLocate(123456, q1);
  const Rec got = cmask::ctime::g_ring[n0 % cmask::ctime::kRing];
  const bool rec = rc == 0 && g_fakeLvCalls == 1 && g_fakeLvCap == 2 && cmask::ctime::g_ringN == n0 + 1 &&
                   got.dt == 123456 && got.qOk && !memcmp(got.q, q1, 16) && got.simOk && got.sim == 42.5 &&
                   got.q0 > 0 && got.q1 >= got.q0 && got.tid == GetCurrentThreadId();
  const int32_t rcFail = CallLocate(123457, q1, -2);
  const bool fail = rcFail == -2 && cmask::ctime::g_ringN == n0 + 1;
  cmask::ctime::g_on = false;
  const int32_t rcOff = CallLocate(123458, q1);
  const bool off = rcOff == 0 && g_fakeLvCalls == 3 && cmask::ctime::g_ringN == n0 + 1;
  cmask::ctime::g_testRecordAlways = false;
  Check(rec && fail && off,
        "R74a hook: H_LocateViews forwards every argument and the result; records entry / return QPC, displayTime, "
        "view 0 orientation, model time and thread; a failed call or a hook switched off records nothing");
  cmask::ctime::g_modelTime = nullptr;
}

void TestPipeline() {
  ID3D11Device* dev = nullptr;
  ID3D11DeviceContext* imm = nullptr;
  const D3D_FEATURE_LEVEL want[] = {D3D_FEATURE_LEVEL_11_1};
  if (FAILED(D3D11CreateDevice(nullptr, QV_R84_DRIVER, nullptr, 0, want, 1, D3D11_SDK_VERSION, &dev, nullptr,
                               &imm))) {
    Check(false, "cockpit mask pipeline: a WARP 11.1 device (R86: no longer a silent SKIP)");
    return;
  }
  ID3D11DeviceContext1* c = nullptr;
  imm->QueryInterface(__uuidof(ID3D11DeviceContext1), reinterpret_cast<void**>(&c));
  Check(cmask::Compile(), "cockpit mask: compute shader variants compile (cs_5_0)");
  if (!c || !cmask::g_blobsReady.load() || !CompileShaders()) {
    Check(false, "cockpit mask pipeline: context 1, mask blobs and fixation depth shaders (R86: not a silent return)");
    Rel(c), Rel(imm), Rel(dev);
    return;
  }
  // HMD views: periphery L 96x90 (R32G8X24), periphery R 97x91 (R24G8, sizes not divisible), focus L 200x190
  // (R32G8X24 array slice 1), focus R 200x190 (R24G8, cPerView at constant 16).
  View v[4];
  const double fovs[4][4] = {{-1.4, 1.1, 1.2, -1.3}, {-1.1, 1.4, 1.2, -1.3}, {-0.65, 0.55, 0.6, -0.62},
                             {-0.55, 0.65, 0.6, -0.62}};
  const DXGI_FORMAT fmts[4] = {DXGI_FORMAT_R32G8X24_TYPELESS, DXGI_FORMAT_R24G8_TYPELESS, DXGI_FORMAT_R32G8X24_TYPELESS,
                               DXGI_FORMAT_R24G8_TYPELESS};
  ID3D11Texture2D* clean[4] = {};
  ID3D11Texture2D* glassy[4] = {};
  bool made = true;
  for (int i = 0; i < 4; ++i) {
    v[i].w = i == 0 ? 96 : i == 1 ? 97 : 200;
    v[i].h = i == 0 ? 90 : i == 1 ? 91 : 190;
    v[i].eyeX = (i & 1) ? 0.032 : -0.032;
    fdtest::MakeProj(v[i].P, fovs[i][0], fovs[i][1], fovs[i][2], fovs[i][3], 0.02, 0, true);
    const UINT slices = i == 2 ? 2 : 1, slice = i == 2 ? 1 : 0;
    made = made && MakeTarget(dev, v[i], fmts[i], slices, slice) && fdtest::MakeCb(dev, v[i], i == 3 ? 16 : 0);
    clean[i] = MakeScene(dev, v[i], fmts[i], slices, slice, i, false);
    glassy[i] = MakeScene(dev, v[i], fmts[i], slices, slice, i, true);
    made = made && clean[i] && glassy[i];
  }
  View def;
  def.w = def.h = 64;
  fdtest::MakeProj(def.P, -1, 1, 1, -1, 0.05, 0, true);
  made = made && fdtest::MakeDepth(dev, c, def, DXGI_FORMAT_R32_TYPELESS, 1, 0);
  ID3D11ComputeShader* dcsCs = nullptr;
  dev->CreateComputeShader(g_blob[0].data(), g_blob[0].size(), nullptr, &dcsCs);
  D3D11_BUFFER_DESC cd{};
  cd.ByteWidth = 1024;
  cd.Usage = D3D11_USAGE_DEFAULT;
  cd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
  ID3D11Buffer* cbA = nullptr;
  dev->CreateBuffer(&cd, nullptr, &cbA);
  made = made && dcsCs && cbA;
  Check(made, "cockpit mask: synthetic HMD views (R32G8X24 2D / array slice, R24G8, cockpit / glass scenes) created");
  if (!made) return;

  // The module: the mask only (no cone, grid or far depth), its own block name, no OpenXR table.
  g_coneOn = false;
  g_gridOn = false;
  fdexport::g_on = false;
  g_hmdOffset = 0;
  g_verify = 1;
  wchar_t gname[64];
  swprintf_s(gname, L"Local\\CmTestGaze_%lu", GetCurrentProcessId());
  g_gazeName = gname;  // never created: the FOV comes from the CPU gProj
  cmask::Configure(true, 2, 15, 1.0, 0);
  wchar_t bname[64];
  swprintf_s(bname, L"Local\\CmTestMask_%lu", GetCurrentProcessId());
  cmask::g_blockName = bname;
  cmask::ConfigureDepth(true, 0);  // R69: the depth companion configured (idle until its reader stores a heartbeat)
  wchar_t dname[64];
  swprintf_s(dname, L"Local\\CmTestDepth_%lu", GetCurrentProcessId());
  cmask::g_dBlockName = dname;
  cmask::ConfigureMotion(true, true, 0);  // R70: the motion companion (idle until its reader stores a heartbeat)
  wchar_t mname[64];
  swprintf_s(mname, L"Local\\CmTestMotion_%lu", GetCurrentProcessId());
  cmask::g_mBlockName = mname;
  cmask::g_xrHookWanted = false;
  ID3D11Device* oldDev = shadowinst::g_device;
  shadowinst::g_device = dev;
  ptiming::g_topTid = GetCurrentThreadId();
  g_fault = false;
  g_workerStop = false;
  fdexport::g_stopping = false;
  fdexport::ResetFrameState();
  g_worker = std::thread(WorkerLoop);
  OpenMappings();
  UnsyncViewCount();
  g_maskWith = true;
  // R91: the tests before R88 see every layout change at once (no flight signal: the gate off) and the builder's sets
  // in the commit that asks (the render thread waits for the builder): their frame counts stay exact. The R88 / R91
  // block below switches both back.
  cmask::g_flightGate = false;
  cmask::g_buildWaitMs = 5000;
  cmask::Install();
  g_installed = true;
  cm::Header* blk = cmask::g_view.load();
  {
    cm::Header h0{};
    const bool rd0 = blk && cm::seqlock_read(blk, h0);
    Check(cmask::Active() && cmask::g_idleA.load() && !fdexport::WantUploads() && rd0 && h0.magic == cm::block_magic &&
              (h0.flags & cm::header_flags::reader_heartbeat) && (h0.flags & cm::header_flags::idle) &&
              !(h0.flags & cm::header_flags::active) && !cm::header_usable(h0, GetCurrentProcessId()),
          "cockpit mask heartbeat: install without a reader heartbeat is idle (block written: reader_heartbeat | idle, "
          "not active; the CPU gProj capture unarmed)");
  }
  cdp::Header* dblk = cmask::g_dview.load();  // R69
  {
    cdp::Header d0{};
    const bool rd0 = dblk && cdp::seqlock_read(dblk, d0);
    Check(rd0 && d0.magic == cdp::block_magic && d0.version == 1 && d0.revision == cdp::contract_revision &&
              (d0.flags & cdp::header_flags::reader_heartbeat) && (d0.flags & cdp::header_flags::idle) &&
              !(d0.flags & cdp::header_flags::active) && cmask::g_dIdleA.load() && cmask::DepthWanted(),
          "cockpit depth: install maps the companion block (magic, v1, revision; reader_heartbeat | idle, not active)");
  }
  bool dbeating = false;                        // R69: the test's reader of the depth block
  bool mbeating = false;                        // R70: the test's reader of the motion block
  cmv::Header* mblk = cmask::g_mview.load();
  // R70: DCS's MV render targets, one per view (view 1 RGBA16F, the others R16G16_FLOAT), bound with the DSV in the
  // G-buffer passes. MV(x, y) = ((x - 40) / 4, (30 - y) / 2 + view): exact in half floats.
  ID3D11Texture2D* mvTex[4] = {};
  ID3D11RenderTargetView* mvRtv[4] = {};
  auto mvAt = [](int i, int x, int y, float out[2]) {
    out[0] = (x - 40) * 0.25f;
    out[1] = (30 - y) * 0.5f + static_cast<float>(i);
  };
  for (int i = 0; i < 4; ++i) {
    const bool rgba = i == 1;
    std::vector<uint16_t> px(static_cast<size_t>(v[i].w) * v[i].h * (rgba ? 4 : 2), 0);
    for (int y = 0; y < v[i].h; ++y)
      for (int x = 0; x < v[i].w; ++x) {
        float m[2];
        mvAt(i, x, y, m);
        uint16_t* o = px.data() + (static_cast<size_t>(y) * v[i].w + x) * (rgba ? 4 : 2);
        o[0] = HalfOf(m[0]);
        o[1] = HalfOf(m[1]);
      }
    D3D11_TEXTURE2D_DESC td{};
    td.Width = v[i].w;
    td.Height = v[i].h;
    td.MipLevels = td.ArraySize = 1;
    td.Format = rgba ? DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_R16G16_FLOAT;
    td.SampleDesc.Count = 1;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA init{px.data(), static_cast<UINT>(v[i].w * (rgba ? 8 : 4)), 0};
    if (SUCCEEDED(dev->CreateTexture2D(&td, &init, &mvTex[i]))) dev->CreateRenderTargetView(mvTex[i], nullptr, &mvRtv[i]);
  }
  const cmask::XrFrameInfo* xrp = nullptr;      // R69: DCS's submitted frame given to OnXrEndFrame
  // R69: DLSS jitter in this frame's uploaded gProj (render texels, +x right, +y down) for views 0 and 2.
  const double jit[4][2] = {{0.25, -0.4}, {0, 0}, {-0.3, 0.15}, {0, 0}};
  float Pj[4][16];
  for (int i = 0; i < 4; ++i) {
    memcpy(Pj[i], v[i].P, sizeof(Pj[i]));
    Pj[i][8] += static_cast<float>(2.0 * jit[i][0] / v[i].w);
    Pj[i][9] += static_cast<float>(-2.0 * jit[i][1] / v[i].h);
  }
  // R69: a UAV of DCS's bound at CS u1 (the depth variant binds u1: it must be put back).
  ID3D11Texture2D* u1tex = nullptr;
  ID3D11UnorderedAccessView* dcsU1 = nullptr;
  {
    D3D11_TEXTURE2D_DESC ud{};
    ud.Width = ud.Height = 4;
    ud.MipLevels = ud.ArraySize = 1;
    ud.Format = DXGI_FORMAT_R32_FLOAT;
    ud.SampleDesc.Count = 1;
    ud.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
    if (SUCCEEDED(dev->CreateTexture2D(&ud, nullptr, &u1tex))) dev->CreateUnorderedAccessView(u1tex, nullptr, &dcsU1);
  }
  bool beating = false;  // the test's reader: stores its heartbeat at every frame start
  auto qpcNow = [] {
    LARGE_INTEGER q;
    QueryPerformanceCounter(&q);
    return static_cast<int64_t>(q.QuadPart);
  };

  FakePass casc = MakePass("CascadeShadowPass0"), lights = MakePass("LightsBuffersPass"), gbuf = MakePass("GBufferPass"),
           resolve = MakePass("DepthResolverPass"), sea = MakePass("WaterSeaPass"), lit = MakePass("GBufferLightingPass"),
           trans = MakePass("TransparentPass"), tcock = MakePass("TransparentCockpitPass"), hmd = MakePass("CopyToHMDPass");
  FakePass ovlp = MakePass("OverlayHDRPass");  // R74b
  bool stateSame = true;
  int dispatchChecks = 0;
  auto ob = [&](FakePass* p, bool begin) {
    const uint64_t n0 = cmask::g_captures.load();
    const fdtest::Snap before = fdtest::TakeSnap(c);
    OnBoundary(p, begin, 0);
    if (cmask::g_captures.load() != n0) {
      ++dispatchChecks;
      if (!(before == fdtest::TakeSnap(c))) stateSame = false;
    }
  };
  auto bindDefault = [&] {
    ID3D11RenderTargetView* none = nullptr;
    c->OMSetRenderTargets(1, &none, def.dsv);
    D3D11_VIEWPORT vp{0, 0, 64, 64, 0, 1};
    c->RSSetViewports(1, &vp);
  };
  // flushMode per view: 0 WaterSeaPass, 1 GBufferLightingPass only, 2 neither (the first Transparent* pass).
  int flushMode[4] = {0, 1, 2, 0};
  std::function<void(int)> atHmd;  // R71: DCS's CopyToHMDPass draw of view i (the frame-generation dump test)
  // R74b: view i's OverlayHDRPass: phase 0 before its begin (the DLSS output), 1 inside (DCS's binds and symbols);
  // atOverlayEndBound(i): DCS leaves the target bound at the pass end; overlaySnap: DCS's bindings around both ends.
  std::function<void(int, int)> atOverlay;
  std::function<bool(int)> atOverlayEndBound;
  bool* overlaySnap = nullptr;
  int vpCut = 0;        // R88: the views' viewports this much smaller (the F10 map's / a loading screen's sizes)
  bool flight = false;  // R88: main.cpp's collect hook culls a Quad Views world frame before this frame's passes
  auto frame = [&](int64_t displayTime, bool commitAtXr) {
    if (flight) g_quadFrame++;
    if (beating && blk) cm::reader_heartbeat(blk, qpcNow());
    if (dbeating && dblk) cdp::reader_heartbeat(dblk, qpcNow());  // R69
    if (mbeating && mblk) cmv::reader_heartbeat(mblk, qpcNow());  // R70
    if (dcsU1) {
      const UINT keep = static_cast<UINT>(-1);
      c->CSSetUnorderedAccessViews(1, 1, &dcsU1, &keep);
    }
    ob(&casc, true);
    ob(&casc, false);
    for (int i = 0; i < 4; ++i) {
      c->CopyResource(v[i].tex, clean[i]);  // this frame's opaque scene
      const std::vector<float> data = fartest::CbData(v[i], Pj[i]);  // R69: with this frame's jitter
      ob(&lights, true);
      ob(&lights, false);
      bindDefault();
      for (int pass = 0; pass < 2; ++pass) {  // cockpit, main
        ob(&gbuf, true);
        c->CSSetShader(dcsCs, nullptr, 0);
        c->CSSetConstantBuffers(0, 1, &cbA);
        c->OMSetRenderTargets(1, &mvRtv[i], v[i].dsv);  // R70: DCS's MV target with the G-buffer's depth
        D3D11_VIEWPORT vp{0, 0, static_cast<float>(v[i].w - vpCut), static_cast<float>(v[i].h - vpCut), 0, 1};
        c->RSSetViewports(1, &vp);
        c->UpdateSubresource(v[i].cb, 0, nullptr, data.data(), 0, 0);
        if (v[i].cbNum)
          c->VSSetConstantBuffers1(7, 1, &v[i].cb, &v[i].cbFirst, &v[i].cbNum);
        else
          c->VSSetConstantBuffers(7, 1, &v[i].cb);
        if (i != 3) bindDefault();  // focus R keeps its DSV bound (the dispatch must unbind and restore it)
        ob(&gbuf, false);
      }
      ob(&resolve, true);
      ob(&resolve, false);
      if (flushMode[i] == 0) {
        ob(&sea, true);
        ob(&sea, false);
      }
      if (flushMode[i] <= 1) {
        ob(&lit, true);
        ob(&lit, false);
      }
      ob(&trans, true);
      ob(&trans, false);
      ob(&tcock, true);
      c->CopyResource(v[i].tex, glassy[i]);  // DCS's transparent cockpit materials write COCKPIT where the glass is
      ob(&tcock, false);
      bindDefault();
      if (atOverlay) {  // R74b
        atOverlay(i, 0);
        const fdtest::Snap s0 = fdtest::TakeSnap(c);
        OnBoundary(&ovlp, true, 0);
        if (overlaySnap && !(s0 == fdtest::TakeSnap(c))) *overlaySnap = false;
        atOverlay(i, 1);
        if (!(atOverlayEndBound && atOverlayEndBound(i))) bindDefault();
        const fdtest::Snap s1 = fdtest::TakeSnap(c);
        OnBoundary(&ovlp, false, 0);
        if (overlaySnap && !(s1 == fdtest::TakeSnap(c))) *overlaySnap = false;
        bindDefault();
      }
      ob(&hmd, true);
      if (atHmd) atHmd(i);
      ob(&hmd, false);
    }
    if (commitAtXr) cmask::OnXrEndFrame(displayTime, xrp);
  };

  // R67: no reader heartbeat (heartbeat 0): idle frames; the view count is learnt, nothing else happens.
  {
    const uint64_t c0 = cmask::g_captures.load(), k0 = cmask::g_commits.load(), s0 = cmask::g_signals.load();
    for (int f = 0; f < 3; ++f) frame(900 + f, true);
    cm::Header hi{};
    const bool rdi = cm::seqlock_read(blk, hi);
    Check(cmask::g_captures.load() == c0 && cmask::g_commits.load() == k0 && cmask::g_signals.load() == s0 &&
              cmask::g_idleFrames.load() >= 2 && cmask::StatusBits() == 0 && !fdexport::WantUploads() && rdi &&
              (hi.flags & cm::header_flags::idle) && hi.newestMaskSeq == 0 && cmask::g_relayouts.load() == 0 &&
              blk->readerHeartbeatQpc == 0,
          "cockpit mask heartbeat: heartbeat 0 -> no dispatch, no fence Signal, no commit, no relayout, bits 39 / 40 clear");
  }
  // The reader starts: frame 1000 resumes (sizes learnt, layout made at its commit: no mask), 1001 on: masks.
  beating = true;
  for (int f = 0; f < 3; ++f) frame(1000 + f, true);
  Check(fdexport::WantUploads() && cmask::Active() && !cmask::g_idleA.load() && cmask::g_toActive.load() == 1,
        "cockpit mask heartbeat: a fresh heartbeat makes the export active and arms the CPU gProj capture");
  cm::Header h{};
  const bool rd = blk && cm::seqlock_read(blk, h);
  Check(rd && cm::header_usable(h, GetCurrentProcessId()) && h.atlasW == 48 + 49 + 100 + 100 && h.atlasH == 95 &&
            (h.flags & cm::header_flags::fence) && (h.flags & cm::header_flags::release_fence) && h.textureHandle[0] &&
            h.fenceHandle && h.releaseFenceHandle && h.generation != 0,
        "cockpit mask: block active after the first frames (atlas 297x95 = 48+49+100+100, NT handles, fence, release fence)");
  const int s3 = cm::find_slot(h, 1002);
  Check(s3 >= 0 && h.slot[s3].viewMask == 15 && h.slot[s3].maskSeq == h.newestMaskSeq &&
            !(h.slot[s3].flags & cm::slot_flags::partial) && cm::find_slot(h, 1001) >= 0 && cm::find_slot(h, 1000) == -1 &&
            !(h.flags & cm::header_flags::idle) && (h.flags & cm::header_flags::reader_heartbeat) &&
            cmask::g_signals.load() == 2,
        "cockpit mask heartbeat: fresh heartbeat commits (frames 1001 / 1002 with their displayTime, all four views, one "
        "fence Signal each; 1000, the first after resuming, has no slot: layout pending)");
  // Read through the NT handle, on the device (D3D11 reader on DCS's device) and through the fence.
  ID3D11Device1* d1 = nullptr;
  dev->QueryInterface(__uuidof(ID3D11Device1), reinterpret_cast<void**>(&d1));
  ID3D11Texture2D* shared = nullptr;
  if (d1 && s3 >= 0)
    d1->OpenSharedResource1(reinterpret_cast<HANDLE>(h.textureHandle[s3]), __uuidof(ID3D11Texture2D),
                            reinterpret_cast<void**>(&shared));
  ID3D11Device5* d5 = nullptr;
  dev->QueryInterface(__uuidof(ID3D11Device5), reinterpret_cast<void**>(&d5));
  ID3D11Fence* fence = nullptr;
  if (d5) d5->OpenSharedFence(reinterpret_cast<HANDLE>(h.fenceHandle), __uuidof(ID3D11Fence), reinterpret_cast<void**>(&fence));
  bool fenceOk = false;
  if (fence && s3 >= 0) {
    c->Flush();
    for (int k = 0; k < 200 && fence->GetCompletedValue() < h.slot[s3].maskSeq; ++k) Sleep(5);
    fenceOk = fence->GetCompletedValue() >= h.slot[s3].maskSeq;
  }
  Check(shared && fenceOk, "cockpit mask: slot texture opened by its NT handle; the shared fence reaches the slot's maskSeq");
  int bad[4] = {-1, -1, -1, -1};
  if (shared) {
    const std::vector<uint8_t> a = ReadR8(dev, c, shared);
    for (int i = 0; i < 4; ++i)
      bad[i] = CompareView(a, h.atlasW, h.slot[s3].view[i], i, v[i].w, v[i].h, false, 2, 0, 1.0f);
  }
  char what[800];
  sprintf_s(what, "cockpit mask: divisor 2 masks equal the CPU max filter of the opaque scene (mismatches %d %d %d %d; "
                  "canopy glass written by TransparentCockpitPass is not in them)", bad[0], bad[1], bad[2], bad[3]);
  Check(bad[0] == 0 && bad[1] == 0 && bad[2] == 0 && bad[3] == 0, what);
  const cm::MaskView& m0 = h.slot[s3].view[0];
  const cm::MaskView& m2 = h.slot[s3].view[2];
  const cm::MaskView& m1 = h.slot[s3].view[1];
  Check((m0.flags & cm::view_flags::after_water) && !(m1.flags & cm::view_flags::after_water) &&
            (m0.flags & cm::view_flags::stencil) && (m0.flags & cm::view_flags::near_depth) &&
            (m2.flags & cm::view_flags::focus) && m0.divisor == 2 && m0.srcW == 96 && m1.w == 49 && m1.h == 46 &&
            m2.x == 97 && fdtest::Near(m0.nearM, 1.0, 1e-6),
        "cockpit mask: view records (sample point, stencil / near flags, focus, divisor, source size, rects)");
  // FOV: view 1 (97x91, divisor 2: the rect covers 98x92 source texels).
  {
    const Proj p1 = ParseProj(v[1].P);
    double f1[4];
    DcsFov(p1, f1);
    const float* t = m1.fovTan;
    Check(fdtest::Near(t[0], f1[0], 1e-5) && fdtest::Near(t[1], f1[0] + (f1[1] - f1[0]) * 98.0 / 97.0, 1e-5) &&
              fdtest::Near(t[2], f1[2], 1e-5) && fdtest::Near(t[3], f1[2] - (f1[2] - f1[3]) * 92.0 / 91.0, 1e-5),
          "cockpit mask: the rect's tangents from this frame's gProj (CPU capture), extended by the partial block");
  }
  Check(cmask::g_afterWater.load() >= 2 && cmask::g_atLighting.load() >= 1 && cmask::g_atTransparent.load() >= 1 &&
            cmask::g_droppedAtEnd.load() == 0,
        "cockpit mask: sample points (WaterSeaPass end, GBufferLightingPass begin, first Transparent* pass begin)");
  Check(stateSame && dispatchChecks >= 4 && cmask::g_stateChecks.load() >= 4 && cmask::g_stateDiffs.load() == 0,
        "cockpit mask: DCS's CS / OM bindings identical around every dispatch (focus R's DSV still bound)");
  Check((cmask::StatusBits() & (qvstatus::kCockpitMask | qvstatus::kCockpitMaskPaired)) ==
            (qvstatus::kCockpitMask | qvstatus::kCockpitMaskPaired),
        "cockpit mask: status bits 39 / 40 after a paired commit");

  // R69: frames 1000-1002 had the mask's heartbeat only: masks without depth.
  {
    cdp::Header dh{};
    const bool rdd = dblk && cdp::seqlock_read(dblk, dh);
    Check(rdd && cmask::g_dCommits.load() == 0 && s3 >= 0 && !(h.slot[s3].flags & cm::slot_flags::depth) &&
              (dh.flags & cdp::header_flags::active) && (dh.flags & cdp::header_flags::idle) &&
              dh.generation == h.generation && cmask::g_dIdleA.load() &&
              !(cmask::StatusBits() & qvstatus::kCockpitDepth),
          "cockpit depth: without its own heartbeat no depth is written while the mask commits (depth header active | "
          "idle, the mask's generation; bit 41 clear)");
  }
  // R69: the depth reader starts. DCS's submitted frame: a pose and FOV per view (the FOV of each view's gProj).
  cmask::XrFrameInfo xi{};
  xi.poses = true;
  xi.space = 0xABCD;
  xi.viewCount = 4;
  for (int i = 0; i < 4; ++i) {
    const float qv[4] = {0.0f, 0.0871557f, 0.0f, 0.9961947f};
    memcpy(xi.q[i], qv, sizeof(qv));
    xi.p[i][0] = (i & 1) ? 0.032f : -0.032f;
    xi.p[i][1] = 1.2f + 0.01f * i;
    xi.p[i][2] = 0.3f;
    for (int k = 0; k < 4; ++k) xi.fov[i][k] = static_cast<float>(std::atan(fovs[i][k]));
  }
  xrp = &xi;
  dbeating = true;
  {
    const uint64_t sig0 = cmask::g_signals.load(), dc0 = cmask::g_dCommits.load(), k0 = cmask::g_commits.load();
    for (int f = 0; f < 3; ++f) frame(1100 + f, true);
    cm::Header hm{};
    cdp::Header hd{};
    const bool rdm = cm::seqlock_read(blk, hm), rdd = cdp::seqlock_read(dblk, hd);
    const int ms = rdm ? cm::find_slot(hm, 1102) : -1, ds = rdd ? cdp::find_slot(hd, 1102) : -1;
    Check(rdm && rdd && cdp::header_usable(hd, GetCurrentProcessId()) && hd.generation == hm.generation &&
              hd.atlasW == hm.atlasW && hd.atlasH == hm.atlasH && hd.fenceHandle == hm.fenceHandle &&
              (hd.flags & cdp::header_flags::shares_mask_fence) && (hd.flags & cdp::header_flags::fence) &&
              !(hd.flags & cdp::header_flags::idle) && hd.textureHandle[0] && hd.textureHandle[0] != hm.textureHandle[0],
          "cockpit depth: header active with the mask's generation, atlas size and fence handle (shares_mask_fence), "
          "its own NT-handle textures");
    bool sameGrid = ms >= 0 && ds >= 0;
    for (int i = 0; sameGrid && i < 4; ++i) {
      const cm::MaskView& mv = hm.slot[ms].view[i];
      const cdp::DepthView& dv = hd.slot[ds].view[i];
      sameGrid = (dv.flags & cdp::view_flags::same_grid_as_mask) && dv.x == mv.x && dv.y == mv.y && dv.w == mv.w &&
                 dv.h == mv.h && dv.srcW == mv.srcW && dv.divisor == mv.divisor && !memcmp(dv.fovTan, mv.fovTan, 16) &&
                 dv.nearM == mv.nearM && cdp::view_usable(hd, ds, i);
    }
    const uint32_t needS = cdp::slot_flags::valid | cdp::slot_flags::display_time | cdp::slot_flags::poses;
    Check(ms >= 0 && ds == ms && cdp::paired_with_mask(hd, ds, ms, hm.slot[ms].maskSeq, hm.generation) &&
              (hm.slot[ms].flags & cm::slot_flags::depth) && (hd.slot[ds].flags & needS) == needS &&
              !(hd.slot[ds].flags & cdp::slot_flags::partial) && hd.slot[ds].viewMask == 15 &&
              hd.slot[ds].engineFrame == hm.slot[ms].engineFrame && hd.newestSeq == hm.newestMaskSeq && sameGrid &&
              cdp::find_slot(hd, 1101) == cm::find_slot(hm, 1101),
          "cockpit depth: paired with the mask commit (same slot index, seq = maskSeq, generation, engine frame; mask "
          "slot_flags::depth; poses; all four views on the mask's grid: rect, divisor, source, fovTan)");
    Check(cmask::g_signals.load() == sig0 + 3 && cmask::g_commits.load() == k0 + 3 && cmask::g_dCommits.load() == dc0 + 3 &&
              (cmask::StatusBits() & qvstatus::kCockpitDepth) && !cmask::g_dIdleA.load(),
          "cockpit depth: one fence Signal per frame for mask and depth together (3 frames, 3 Signals); bit 41 set");
    // The texels, through the NT handles, after the shared fence reached the slot's seq.
    ID3D11Texture2D* dtex = nullptr;
    ID3D11Texture2D* mtex = nullptr;
    if (d1 && ds >= 0) {
      d1->OpenSharedResource1(reinterpret_cast<HANDLE>(hd.textureHandle[ds]), __uuidof(ID3D11Texture2D),
                              reinterpret_cast<void**>(&dtex));
      d1->OpenSharedResource1(reinterpret_cast<HANDLE>(hm.textureHandle[ms]), __uuidof(ID3D11Texture2D),
                              reinterpret_cast<void**>(&mtex));
    }
    bool fenced = false;
    if (fence && ds >= 0) {
      c->Flush();
      for (int k = 0; k < 200 && fence->GetCompletedValue() < hd.slot[ds].seq; ++k) Sleep(5);
      fenced = fence->GetCompletedValue() >= hd.slot[ds].seq;
    }
    int dbad[4] = {-1, -1, -1, -1}, mbad[4] = {-1, -1, -1, -1}, inv[4] = {-1, -1, -1, -1};
    if (dtex && mtex) {
      const std::vector<float> a = ReadRG16F(dev, c, dtex);
      const std::vector<uint8_t> m = ReadR8(dev, c, mtex);
      for (int i = 0; i < 4; ++i) {
        dbad[i] = CompareDepth(a, hd.atlasW, hd.slot[ds].view[i], i, v[i].w, v[i].h, 2, 0, 1.0f, &m, &inv[i]);
        mbad[i] = CompareView(m, hm.atlasW, hm.slot[ms].view[i], i, v[i].w, v[i].h, false, 2, 0, 1.0f);
      }
    }
    sprintf_s(what, "cockpit depth: WARP depth atlas (through its NT handle, after the shared fence) equals the CPU "
                    "linearise + min/max downsample (divisor 2, near 1 m; mismatches %d %d %d %d); the fused variant's "
                    "mask is unchanged (%d %d %d %d); .x > 0 exactly where the mask is 255 (%d %d %d %d)",
              dbad[0], dbad[1], dbad[2], dbad[3], mbad[0], mbad[1], mbad[2], mbad[3], inv[0], inv[1], inv[2], inv[3]);
    Check(fenced && dbad[0] == 0 && dbad[1] == 0 && dbad[2] == 0 && dbad[3] == 0 && mbad[0] == 0 && mbad[1] == 0 &&
              mbad[2] == 0 && mbad[3] == 0 && inv[0] == 0 && inv[1] == 0 && inv[2] == 0 && inv[3] == 0,
          what);
    Rel(dtex);
    Rel(mtex);
    // Per-view records: pose / FOV / space as submitted, the jitter recovered, no dual camera (cockpit_ipd off).
    bool rec = ds >= 0 && hd.slot[ds].xrSpace == 0xABCD;
    double jerr = 0;
    for (int i = 0; rec && i < 4; ++i) {
      const cdp::DepthView& dv = hd.slot[ds].view[i];
      rec = (dv.flags & cdp::view_flags::pose) && (dv.flags & cdp::view_flags::jitter) &&
            !(dv.flags & cdp::view_flags::boost) && !(dv.flags & cdp::view_flags::dual_camera) &&
            std::isnan(dv.boostShiftM) && dv.worldMinusCockpitM[0] == 0 && dv.worldMinusCockpitM[2] == 0 &&
            !memcmp(dv.poseOrientation, xi.q[i], 16) && !memcmp(dv.posePosition, xi.p[i], 12) &&
            !memcmp(dv.xrFov, xi.fov[i], 16) && ((dv.flags & cdp::view_flags::focus) != 0) == (i >= 2) &&
            (dv.flags & cdp::view_flags::after_water) == ((i == 0 || i == 3) ? cdp::view_flags::after_water : 0u);
      jerr = std::max({jerr, std::fabs(dv.jitterPx[0] - jit[i][0]), std::fabs(dv.jitterPx[1] - jit[i][1])});
    }
    sprintf_s(what, "cockpit depth: per view DCS's submitted pose / FOV / space, the DLSS jitter recovered from gProj "
                    "(max error %.5f texels), boost unknown (NaN) and no shift without the dual camera; flags", jerr);
    Check(rec && jerr < 2e-3 && cmask::g_dJitterBad.load() == 0, what);
  }
  // R70: the motion companion. Its block is mapped (idle) and nothing was copied so far.
  {
    cmv::Header hm0{};
    const bool rd0 = mblk && cmv::seqlock_read(mblk, hm0);
    Check(rd0 && hm0.magic == cmv::block_magic && cmask::g_mCommits.load() == 0 && (hm0.flags & cmv::header_flags::active) &&
              (hm0.flags & cmv::header_flags::idle) && (hm0.flags & cmv::header_flags::class_atlas) &&
              hm0.generation == cmask::g_hdr.generation && !(cmask::StatusBits() & qvstatus::kCockpitMotion),
          "motion: without its own heartbeat nothing is copied (header active | idle | class_atlas, the mask's generation; "
          "bit 42 clear)");
  }
  mbeating = true;
  {
    const uint64_t sig0 = cmask::g_signals.load(), mc0 = cmask::g_mCommits.load();
    for (int f = 0; f < 4; ++f) frame(1200 + f, true);
    cm::Header hm{};
    cmv::Header hv{};
    const bool rdm = cm::seqlock_read(blk, hm), rdv = cmv::seqlock_read(mblk, hv);
    const int ms = rdm ? cm::find_slot(hm, 1203) : -1, vs = rdv ? cmv::find_slot(hv, 1203) : -1;
    const int ms2 = cm::find_slot(hm, 1202);
    Check(rdm && rdv && cmv::header_usable(hv, GetCurrentProcessId()) && hv.generation == hm.generation &&
              hv.fenceHandle == hm.fenceHandle && (hv.flags & cmv::header_flags::shares_mask_fence) &&
              hv.classFormat == cmv::format_r8_uint && hv.classTextureHandle[0] && hv.atlasW == 96 + 97 &&
              hv.atlasH == 91 && cmask::g_signals.load() == sig0 + 4 && cmask::g_mCommits.load() == mc0 + 4 &&
              (cmask::StatusBits() & qvstatus::kCockpitMotion),
          "motion: header active (the mask's generation and fence, class atlas, MV atlas 193x91 = views 0 / 1 at render "
          "resolution); one Signal per frame; bit 42 set");
    const cmv::MotionSlot* sl = vs >= 0 ? &hv.slot[vs] : nullptr;
    const bool pairOk = sl && vs == ms && cmv::paired_with_mask(hv, vs, ms, hm.slot[ms].maskSeq, hm.generation) &&
                        sl->prevSeq == hm.slot[ms2].maskSeq && sl->prevDisplayTime == 1202 &&
                        cmv::usable_for_pair(hv, vs, 1202) && !(sl->flags & cmv::slot_flags::reset) &&
                        !(sl->flags & cmv::slot_flags::partial) && sl->viewMask == 3 &&
                        !(sl->flags & cmv::slot_flags::includes_jitter) && (sl->flags & cmv::slot_flags::jitter_free) &&
                        (sl->flags & cmv::slot_flags::sky_is_infinity);
    const int first = cmv::find_slot(hv, 1201) >= 0 ? -1 : 0;  // 1200 (the first) is overwritten by now; see below
    (void)first;
    bool views = sl != nullptr;
    for (int i = 0; views && i < 2; ++i) {
      const cmv::MotionView& mvw = sl->view[i];
      views = (mvw.flags & cmv::view_flags::valid) && mvw.w == static_cast<uint32_t>(v[i].w) &&
              mvw.h == static_cast<uint32_t>(v[i].h) && mvw.x == (i ? 96u : 0u) &&
              ((mvw.flags & cmv::view_flags::from_rgba16f) != 0) == (i == 1) &&
              std::fabs(mvw.jitterCurPx[0] - jit[i][0]) < 2e-3 && std::fabs(mvw.jitterCurPx[1] - jit[i][1]) < 2e-3 &&
              mvw.jitterPrevPx[0] == mvw.jitterCurPx[0] && mvw.jitterPrevPx[1] == mvw.jitterCurPx[1] &&
              cmv::view_usable(hv, vs, i);
    }
    Check(pairOk && views, "motion: paired with the mask slot (index, seq, generation); prevSeq / prevDisplayTime = the "
                           "previous commit; no reset in a steady chain; views 0 / 1 at render resolution (rects, "
                           "RGBA16F flagged), both jitters; jitter_free and sky_is_infinity set (R70 T1 / T2), includes_jitter not");
    // The atlases through their NT handles after the shared fence.
    ID3D11Texture2D* mt = nullptr;
    ID3D11Texture2D* ct = nullptr;
    if (d1 && vs >= 0) {
      d1->OpenSharedResource1(reinterpret_cast<HANDLE>(hv.textureHandle[vs]), __uuidof(ID3D11Texture2D),
                              reinterpret_cast<void**>(&mt));
      d1->OpenSharedResource1(reinterpret_cast<HANDLE>(hv.classTextureHandle[vs]), __uuidof(ID3D11Texture2D),
                              reinterpret_cast<void**>(&ct));
    }
    bool fenced = false;
    if (fence && vs >= 0) {
      c->Flush();
      for (int k = 0; k < 200 && fence->GetCompletedValue() < hv.slot[vs].seq; ++k) Sleep(5);
      fenced = fence->GetCompletedValue() >= hv.slot[vs].seq;
    }
    int mvBad[2] = {-1, -1}, clsBad[2] = {-1, -1};
    if (mt && ct && sl) {
      const std::vector<float> a = ReadRG16F(dev, c, mt);
      const std::vector<uint8_t> cl = ReadR8(dev, c, ct);
      for (int i = 0; i < 2; ++i) {
        mvBad[i] = clsBad[i] = 0;
        const cmv::MotionView& mvw = sl->view[i];
        for (uint32_t y = 0; y < mvw.h; ++y)
          for (uint32_t x = 0; x < mvw.w; ++x) {
            const size_t at = static_cast<size_t>(y) * hv.atlasW + mvw.x + x;
            float wmv[2];
            mvAt(i, static_cast<int>(x), static_cast<int>(y), wmv);
            if (a[at * 2] != wmv[0] || a[at * 2 + 1] != wmv[1]) ++mvBad[i];
            const Px q = ScenePx(i, static_cast<int>(x), static_cast<int>(y), v[i].w, v[i].h, false);
            const uint8_t wc = static_cast<uint8_t>(((q.st >> 3) & 7) | (std::isinf(q.w) ? 0x80 : 0));
            if (cl[at] != wc) ++clsBad[i];
          }
      }
    }
    Rel(mt);
    Rel(ct);
    sprintf_s(what, "motion: WARP MV atlas (NT handle, after the shared fence) equals DCS's MV targets verbatim "
                    "(mismatches %d %d) and the class byte equals the stencil composition + sky bit (%d %d)",
              mvBad[0], mvBad[1], clsBad[0], clsBad[1]);
    Check(fenced && mvBad[0] == 0 && mvBad[1] == 0 && clsBad[0] == 0 && clsBad[1] == 0, what);
  }
  // R70: reset reasons. A camera cut between two frames, then a steady frame; the motion reader stops (idle: nothing
  // copied, one Signal per frame still) and resumes (the first slot after it is a reset).
  {
    scenecut::g_cuts++;
    frame(1300, true);
    frame(1301, true);
    cmv::Header hv{};
    cmv::seqlock_read(mblk, hv);
    const int s0 = cmv::find_slot(hv, 1300), s1 = cmv::find_slot(hv, 1301);
    const bool cut = s0 >= 0 && (hv.slot[s0].flags & cmv::slot_flags::reset) && s1 >= 0 &&
                     !(hv.slot[s1].flags & cmv::slot_flags::reset) && cmask::g_mResetCut.load() >= 1;
    LARGE_INTEGER qf;
    QueryPerformanceFrequency(&qf);
    mbeating = false;
    cmv::reader_heartbeat(mblk, qpcNow() - qf.QuadPart * 21 / 10);
    const uint64_t mc0 = cmask::g_mCommits.load(), sg0 = cmask::g_signals.load();
    frame(1302, true);
    frame(1303, true);
    cmv::seqlock_read(mblk, hv);
    const bool idle = cmask::g_mCommits.load() == mc0 && cmask::g_signals.load() == sg0 + 2 &&
                      (hv.flags & cmv::header_flags::idle) && cmv::find_slot(hv, 1303) == -1 &&
                      !(cmask::StatusBits() & qvstatus::kCockpitMotion) && (cmask::StatusBits() & qvstatus::kCockpitMask) &&
                      !fixdepth::g_mvWanted.load();  // idle: the OM hooks do not even look at the render targets
    mbeating = true;
    frame(1304, true);
    frame(1305, true);
    cmv::seqlock_read(mblk, hv);
    const int r4 = cmv::find_slot(hv, 1304), r5 = cmv::find_slot(hv, 1305);
    const bool resumed = r4 >= 0 && (hv.slot[r4].flags & cmv::slot_flags::reset) && r5 >= 0 &&
                         !(hv.slot[r5].flags & cmv::slot_flags::reset) && hv.slot[r5].prevDisplayTime == 1304 &&
                         fixdepth::g_mvWanted.load();
    Check(cut && idle && resumed, "motion: reset on a camera cut (next frame steady); its reader stops -> no copy (the "
                                  "mask still commits, one Signal per frame, bit 42 clear, no MV observation); resumes -> the first slot is a "
                                  "reset, the next one is not");
    // R76 (HB-1): the config thread's once-per-second ConfigureMotion no longer recomputes the MV note from a stale
    // g_mIdleA (the render thread's transition between its load and its store was lost for up to 1 s).
    cmask::g_mIdleA = true;  // what the worker could have loaded just before SetMotionIdle(false)
    cmask::ConfigureMotion(true, true, 0);
    const bool keptOn = fixdepth::g_mvWanted.load();
    cmask::g_mIdleA = false;
    cmask::ConfigureMotion(false, true, 0);
    const bool clearedOff = !fixdepth::g_mvWanted.load();
    cmask::ConfigureMotion(true, true, 0);
    fixdepth::g_mvWanted = cmask::MvWantedNow();  // as the reinstall the export change triggers
    // R76 (HB-2): a latched fault ends the active state's side effects (gProj capture, MV note, idle).
    const bool before = fdexport::WantUploads() && fixdepth::g_mvWanted.load();
    cmask::LatchFault();
    const bool latched = !cmask::Active() && !fdexport::WantUploads() && !fixdepth::g_mvWanted.load() &&
                         cmask::g_idleA.load() && !cmask::MvWantedNow();
    cmask::g_fault = false;  // back to the state before (as a reinstall would)
    cmask::g_idle = false;
    cmask::g_idleA = false;
    fdexport::g_maskUploads = true;
    fixdepth::g_mvWanted = cmask::MvWantedNow();
    frame(1306, true);
    cmv::seqlock_read(mblk, hv);
    const bool after = cmv::find_slot(hv, 1306) >= 0 && fixdepth::g_mvWanted.load();
    Check(keptOn && clearedOff && before && latched && after,
          "R76 heartbeat: ConfigureMotion never sets the MV note from a stale idle mirror (only clears it when the export "
          "goes off); a latched fault clears the gProj capture and the MV note and goes idle");
  }
  {
    cmask::g_xeOrig = &FakeNextEndFrame;
    cmask::g_xeOn = true;
    // R69: a whole XrFrameEndInfo (layerCount at 28, layers at 32) with a quad layer (type 36) before DCS's
    // projection layer (type 35: space at 24, viewCount at 32, views at 40; views of 96 bytes, type 48).
    alignas(8) uint8_t pv[4][96] = {};
    for (int i = 0; i < 4; ++i) {
      *reinterpret_cast<int32_t*>(pv[i]) = 48;
      const float qv[4] = {0.1f * i, 0.2f, 0.3f, 0.9f};
      const float pp[3] = {1.0f + i, 2.0f, 3.0f};
      const float fv[4] = {-0.9f, 0.8f, 0.7f - 0.01f * i, -0.75f};
      memcpy(pv[i] + 16, qv, 16);
      memcpy(pv[i] + 32, pp, 12);
      memcpy(pv[i] + 44, fv, 16);
    }
    alignas(8) uint8_t quad[56] = {};
    *reinterpret_cast<int32_t*>(quad) = 36;
    alignas(8) uint8_t projL[48] = {};
    *reinterpret_cast<int32_t*>(projL) = 35;
    *reinterpret_cast<uint64_t*>(projL + 24) = 0x5150;
    *reinterpret_cast<uint32_t*>(projL + 32) = 4;
    *reinterpret_cast<const void**>(projL + 40) = pv;
    const void* layers[2] = {quad, projL};
    alignas(8) uint8_t info[40] = {};
    *reinterpret_cast<int32_t*>(info) = 12;
    *reinterpret_cast<int64_t*>(info + 16) = 5555;
    *reinterpret_cast<uint32_t*>(info + 28) = 2;
    *reinterpret_cast<const void* const**>(info + 32) = layers;
    frame(0, false);
    const int32_t rc = cmask::H_EndFrame(reinterpret_cast<void*>(0x1234), info);
    cmask::g_xeOn = false;
    cm::Header hh{};
    const bool ok = cm::seqlock_read(blk, hh);
    const int sl = ok ? cm::find_slot(hh, 5555) : -1;
    Check(rc == 0 && g_fakeEndCalls.load() == 1 && g_fakeEndTime.load() == 5555 && sl >= 0 && hh.slot[sl].viewMask == 15,
          "cockpit mask: H_EndFrame commits with the frame's displayTime and forwards to the next entry unchanged");
    cdp::Header hdh{};
    const int dsl = cdp::seqlock_read(dblk, hdh) ? cdp::find_slot(hdh, 5555) : -1;
    bool poses = dsl == sl && dsl >= 0 && hdh.slot[dsl].xrSpace == 0x5150 && (hdh.slot[dsl].flags & cdp::slot_flags::poses);
    for (int i = 0; poses && i < 4; ++i)
      poses = !memcmp(hdh.slot[dsl].view[i].poseOrientation, pv[i] + 16, 16) &&
              !memcmp(hdh.slot[dsl].view[i].posePosition, pv[i] + 32, 12) &&
              !memcmp(hdh.slot[dsl].view[i].xrFov, pv[i] + 44, 16);
    // ReadPosesSeh alone: no projection layer, a bad view type, a null layer array.
    cmask::XrFrameInfo x1, x2, x3;
    const void* onlyQuad[1] = {quad};
    *reinterpret_cast<uint32_t*>(info + 28) = 1;
    *reinterpret_cast<const void* const**>(info + 32) = onlyQuad;
    const bool noProj = !cmask::ReadPosesSeh(info, x1) && !x1.poses;
    *reinterpret_cast<uint32_t*>(info + 28) = 2;
    *reinterpret_cast<const void* const**>(info + 32) = layers;
    *reinterpret_cast<int32_t*>(pv[2]) = 47;
    const bool badView = cmask::ReadPosesSeh(info, x2) && x2.viewCount == 2;
    *reinterpret_cast<int32_t*>(pv[2]) = 48;
    *reinterpret_cast<const void**>(info + 32) = nullptr;
    const bool noLayers = !cmask::ReadPosesSeh(info, x3);
    Check(poses && noProj && badView && noLayers,
          "cockpit depth: the hook reads DCS's first projection layer (space, per view pose and FOV) from the real "
          "XrFrameEndInfo layout, skipping other layers; stops at a view of another type; none without layers");
  }
  // R74a: content time through the commit: the hook's records -> both companions' slotTime (the same record).
  {
    cmask::ctime::g_orig = &FakeLocate;
    cmask::ctime::g_on = true;
    cmask::ctime::g_hooked = true;
    static double model = 100.0;
    cmask::ctime::g_modelTime = &model;
    cmask::XrFrameInfo xt = xi;
    const cmask::XrFrameInfo* xrSaved = xrp;
    xrp = &xt;
    const float qa[4] = {0, 0.05f, 0, 0.99875f}, qb[4] = {0, 0.06f, 0, 0.9982f};
    memcpy(xt.q[0], qb, 16);
    auto readBoth = [&](int64_t dt, cdp::Header& hd, cmv::Header& hm, int& s, int& ms) {
      s = cdp::seqlock_read(dblk, hd) ? cdp::find_slot(hd, dt) : -1;
      ms = cmv::seqlock_read(mblk, hm) ? cmv::find_slot(hm, dt) : -1;
    };
    model = 100.0;
    CallLocate(7400, qa);  // a call DCS did not render with
    model = 100.011;
    CallLocate(7400, qb);  // the rendered pose
    const cmask::ctime::Rec wantRec = cmask::ctime::g_ring[(cmask::ctime::g_ringN - 1) % cmask::ctime::kRing];
    model = 100.02;
    CallLocate(7411, qb);  // the next frame's, before this commit
    frame(7400, true);
    cdp::Header hd{};
    cmv::Header hm{};
    int s = -1, ms = -1;
    readBoth(7400, hd, hm, s, ms);
    const cdp::SlotTime* t = s >= 0 ? &hd.slotTime[s] : nullptr;
    const uint32_t wantFlags = cdp::time_flags::pose_observed | cdp::time_flags::pose_matched |
                               cdp::time_flags::pose_ambiguous | cdp::time_flags::sim_observed;
    Check(s >= 0 && ms == s && t && cdp::slot_time_usable(hd, s) && cmv::slot_time_usable(hm, ms) &&
              !memcmp(&hd.slotTime[s], &hm.slotTime[ms], sizeof(cdp::SlotTime)) && t->seq == hd.slot[s].seq &&
              t->flags == wantFlags && t->locateCalls == 2 && t->simTime == 100.011 && t->poseDisplayTime == 7400 &&
              t->poseQpc == wantRec.q0 && t->poseReturnQpc == wantRec.q1 && t->poseQpc < hd.slot[s].qpcCommit &&
              (hd.flags & cdp::header_flags::slot_time) && (hm.flags & cmv::header_flags::slot_time) &&
              hd.revision == 2 && hm.revision == 3,
          "R74a commit: slotTime of the frame in BOTH companions (byte-identical, seq == the slot's): the call with the "
          "frame's displayTime and DCS's submitted view 0 orientation (matched, 2 calls: ambiguous), its entry / return "
          "QPC before the commit, model time at its entry; the next frame's call ignored; header flags, revisions");
    frame(7422, true);  // no locate for this frame
    readBoth(7422, hd, hm, s, ms);
    const bool none = s >= 0 && cdp::slot_time_usable(hd, s) && hd.slotTime[s].flags == 0 &&
                      hd.slotTime[s].locateCalls == 0 && hd.slotTime[s].poseQpc == 0;
    CallLocate(7433, qa);
    CallLocate(7433, qa);
    frame(7433, true);  // calls of the frame, none with DCS's orientation: the last one, not matched
    readBoth(7433, hd, hm, s, ms);
    const bool last = s >= 0 && (hd.slotTime[s].flags & cdp::time_flags::pose_observed) &&
                      !(hd.slotTime[s].flags & cdp::time_flags::pose_matched) && hd.slotTime[s].locateCalls == 2 &&
                      hd.slotTime[s].poseQpc ==
                          cmask::ctime::g_ring[(cmask::ctime::g_ringN - 1) % cmask::ctime::kRing].q0;
    cmask::ctime::g_hooked = false;
    frame(7444, true);
    readBoth(7444, hd, hm, s, ms);
    const bool unhooked = s >= 0 && !(hd.flags & cdp::header_flags::slot_time) &&
                          !(hm.flags & cmv::header_flags::slot_time) && !cdp::slot_time_usable(hd, s) &&
                          hd.slotTime[s].flags == 0;
    Check(none && last && unhooked,
          "R74a commit: no call for the frame -> a record with no flags (seq set); no orientation match -> the last "
          "call, not matched; the hook out -> no slot_time flag in either header");
    cmask::ctime::g_on = false;
    cmask::ctime::g_modelTime = nullptr;
    xrp = xrSaved;
  }
  // R74b: mask v1.1, 128 = overlay symbology. Per view an HDR target at 2x the source size (view 2 an array slice),
  // DCS's OverlayHDRPass binds it, sets the viewport and draws a HUD rectangle; view 1 leaves it bound at the pass end.
  {
    ID3D11Texture2D* hdr[4] = {};
    ID3D11RenderTargetView* hdrRtv[4] = {};
    ID3D11Texture2D* alt = nullptr;
    ID3D11RenderTargetView* altRtv = nullptr;
    std::vector<uint16_t> cleanPx[4];
    UINT W2[4], H2[4];
    bool hmade = true;
    for (int i = 0; i < 4; ++i) {
      W2[i] = 2 * v[i].w;
      H2[i] = 2 * v[i].h;
      cleanPx[i].resize(static_cast<size_t>(W2[i]) * H2[i] * 4);
      for (UINT y = 0; y < H2[i]; ++y)
        for (UINT x = 0; x < W2[i]; ++x)
          for (int k = 0; k < 4; ++k)
            cleanPx[i][(static_cast<size_t>(y) * W2[i] + x) * 4 + k] =
                HalfOf(static_cast<float>((x * 7 + y * 13 + k * 3 + i) % 97) / 97.0f);
      D3D11_TEXTURE2D_DESC td{};
      td.Width = W2[i];
      td.Height = H2[i];
      td.MipLevels = 1;
      td.ArraySize = i == 2 ? 2 : 1;
      td.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
      td.SampleDesc.Count = 1;
      td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
      hmade = hmade && SUCCEEDED(dev->CreateTexture2D(&td, nullptr, &hdr[i]));
      D3D11_RENDER_TARGET_VIEW_DESC hrd{};
      hrd.Format = td.Format;
      if (i == 2) {
        hrd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DARRAY;
        hrd.Texture2DArray.FirstArraySlice = 1;
        hrd.Texture2DArray.ArraySize = 1;
      } else {
        hrd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
      }
      hmade = hmade && hdr[i] && SUCCEEDED(dev->CreateRenderTargetView(hdr[i], &hrd, &hdrRtv[i]));
      if (i == 0) {
        hmade = hmade && SUCCEEDED(dev->CreateTexture2D(&td, nullptr, &alt)) &&
               SUCCEEDED(dev->CreateRenderTargetView(alt, nullptr, &altRtv));
      }
    }
    Check(hmade, "R74b: synthetic HDR targets (RGBA16F at 2x the source, an array slice for view 2) created");
    // The HUD rectangle in output texels of view i (upper middle: world, and the canopy bar where it crosses).
    auto hud = [&](int i, UINT& x0, UINT& y0, UINT& x1, UINT& y1) {
      x0 = W2[i] * 3 / 10;
      x1 = W2[i] * 6 / 10;
      y0 = H2[i] / 10;
      y1 = H2[i] * 4 / 10;
    };
    int mode = 0;  // 0 HUD, 1 a full-screen change, 2 view 0 binds another target
    cmask::covl::g_on = true;  // [Perception] CockpitMaskOverlay=1 (opt-in)
    bool ovlSame = true;
    atOverlay = [&](int i, int phase) {
      const UINT sub = i == 2 ? D3D11CalcSubresource(0, 1, 1) : 0;
      if (phase == 0) {  // the DLSS output of this frame, before the pass
        c->UpdateSubresource(hdr[i], sub, nullptr, cleanPx[i].data(), W2[i] * 8, 0);
        if (i == 0) c->UpdateSubresource(alt, 0, nullptr, cleanPx[0].data(), W2[0] * 8, 0);
        return;
      }
      ID3D11RenderTargetView* rtv = (mode == 2 && i == 0) ? altRtv : hdrRtv[i];
      c->OMSetRenderTargets(1, &rtv, nullptr);
      D3D11_VIEWPORT vp{0, 0, static_cast<float>(W2[i]), static_cast<float>(H2[i]), 0, 1};
      c->RSSetViewports(1, &vp);
      UINT x0, y0, x1, y1;
      hud(i, x0, y0, x1, y1);
      if (mode == 1) {
        x0 = y0 = 0;
        x1 = W2[i];
        y1 = H2[i];
      }
      if (mode == 3) {  // R76: a full-width band over the top 36 %: under 40 % of the rect, over 40 % of the world
        x0 = y0 = 0;
        x1 = W2[i];
        y1 = H2[i] * 36 / 100;
      }
      std::vector<uint16_t> px(static_cast<size_t>(x1 - x0) * (y1 - y0) * 4, HalfOf(3.5f));
      D3D11_BOX box{x0, y0, 0, x1, y1, 1};
      c->UpdateSubresource((mode == 2 && i == 0) ? static_cast<ID3D11Resource*>(alt) : hdr[i], sub, &box, px.data(),
                           (x1 - x0) * 8, 0);
    };
    atOverlayEndBound = [&](int i) { return i == 1; };
    overlaySnap = &ovlSame;
    auto expectView = [&](const std::vector<uint8_t>& a, uint32_t aw, const cm::MaskView& mv, int i, bool withHud,
                          int& n128) {
      std::vector<uint8_t> st(static_cast<size_t>(v[i].w) * v[i].h);
      std::vector<float> dist(st.size());
      for (int y = 0; y < v[i].h; ++y)
        for (int x = 0; x < v[i].w; ++x) {
          const Px q = ScenePx(i, x, y, v[i].w, v[i].h, false);
          st[static_cast<size_t>(y) * v[i].w + x] = q.st;
          dist[static_cast<size_t>(y) * v[i].w + x] = std::isinf(q.w) ? 0.0f : q.w;
        }
      UINT hx0, hy0, hx1, hy1;
      hud(i, hx0, hy0, hx1, hy1);
      int nbad = 0;
      n128 = 0;
      const UINT W = v[i].w, H = v[i].h;
      for (uint32_t r = 0; r < mv.h; ++r)
        for (uint32_t cc = 0; cc < mv.w; ++cc) {
          uint8_t wantT = cmask::RefTexel(st.data(), dist.data(), v[i].w, v[i].h, 2, 0, 1.0f, cc, r);
          UINT x0 = (cc * 2 * W2[i]) / W, x1 = ((std::min)((cc + 1) * 2, W) * W2[i]) / W;
          UINT y0 = (r * 2 * H2[i]) / H, y1 = ((std::min)((r + 1) * 2, H) * H2[i]) / H;
          x1 = (std::max)(x1, x0 + 1);
          y1 = (std::max)(y1, y0 + 1);
          const bool hit = x0 < hx1 && hx0 < x1 && y0 < hy1 && hy0 < y1;
          if (withHud && hit && wantT == 0) wantT = 128;
          const size_t at = static_cast<size_t>(mv.y + r) * aw + mv.x + cc;
          if (at >= a.size() || a[at] != wantT) ++nbad;
          if (at < a.size() && a[at] == 128) ++n128;
        }
      return nbad;
    };
    auto readSlot = [&](int64_t dt, cm::Header& hh, int& sl, std::vector<uint8_t>& a) {
      sl = cm::seqlock_read(blk, hh) ? cm::find_slot(hh, dt) : -1;
      a.clear();
      if (sl < 0 || !d1) return;
      ID3D11Texture2D* sh = nullptr;
      d1->OpenSharedResource1(reinterpret_cast<HANDLE>(hh.textureHandle[sl]), __uuidof(ID3D11Texture2D),
                              reinterpret_cast<void**>(&sh));
      if (sh) {
        a = ReadR8(dev, c, sh);
        sh->Release();
      }
    };
    const uint64_t l0 = cmask::covl::g_learns.load();
    frame(7500, true);  // learns the targets (no 128 yet)
    cm::Header hh{};
    int sl = -1;
    std::vector<uint8_t> a;
    readSlot(7500, hh, sl, a);
    const bool learnt = cmask::covl::g_learns.load() == l0 + 4 && sl >= 0 && !(hh.slot[sl].flags & cm::slot_flags::overlay);
    frame(7501, true);
    readSlot(7501, hh, sl, a);
    int obad[4] = {-1, -1, -1, -1}, n[4] = {};
    bool flags = sl >= 0 && (hh.slot[sl].flags & cm::slot_flags::overlay);
    for (int i = 0; i < 4 && sl >= 0 && !a.empty(); ++i) {
      obad[i] = expectView(a, hh.atlasW, hh.slot[sl].view[i], i, true, n[i]);
      flags = flags && (hh.slot[sl].view[i].flags & cm::view_flags::overlay);
    }
    char owhat[600];
    sprintf_s(owhat,
              "R74b: OverlayHDRPass symbols -> 128 on world texels whose footprint the pass changed, cockpit stays 255, "
              "the rest (glass included) as v1 (mismatches %d %d %d %d; 128 texels %d %d %d %d); view / slot flags; "
              "learnt on the first frame",
              obad[0], obad[1], obad[2], obad[3], n[0], n[1], n[2], n[3]);
    Check(learnt && flags && obad[0] == 0 && obad[1] == 0 && obad[2] == 0 && obad[3] == 0 && n[0] > 0 && n[1] > 0 &&
              n[2] > 0 && n[3] > 0,
          owhat);
    Check(ovlSame, "R74b: DCS's bindings identical around the overlay pass boundaries (our dispatches put back the CS "
                   "state; the HDR target left bound by view 1 is unbound for the dispatch and rebound)");
    // A full-screen change: more than 40 % -> no 128 that frame.
    mode = 1;
    frame(7502, true);
    readSlot(7502, hh, sl, a);
    int full[4] = {-1, -1, -1, -1};
    for (int i = 0; i < 4 && sl >= 0 && !a.empty(); ++i) full[i] = expectView(a, hh.atlasW, hh.slot[sl].view[i], i, false, n[i]);
    // Another target bound by view 0: no post (mismatch), relearnt; the next frame back to the first one: relearnt.
    mode = 2;
    const uint64_t mm0 = cmask::covl::g_mismatch.load(), le0 = cmask::covl::g_learns.load();
    frame(7503, true);
    readSlot(7503, hh, sl, a);
    const bool switched = sl >= 0 && !(hh.slot[sl].view[0].flags & cm::view_flags::overlay) &&
                          (hh.slot[sl].view[1].flags & cm::view_flags::overlay) &&
                          cmask::covl::g_mismatch.load() == mm0 + 1 && cmask::covl::g_learns.load() == le0 + 1;
    mode = 0;
    frame(7504, true);
    frame(7505, true);
    readSlot(7505, hh, sl, a);
    int back = -1, nb = 0;
    if (sl >= 0 && !a.empty()) back = expectView(a, hh.atlasW, hh.slot[sl].view[0], 0, true, nb);
    Check(full[0] == 0 && full[1] == 0 && full[2] == 0 && full[3] == 0 && cmask::covl::g_rejected.load() >= 1 && switched &&
              back == 0 && nb > 0,
          "R74b: a full-screen change gives no 128 (rejected); a switched target gives no 128 for that view and is "
          "relearnt; back to the HUD -> 128 again");
    // R76: a change of more than 40 % of the view's world texels but under 40 % of its rect (the cockpit share makes
    // the difference; F1 head down it reaches 60 % and more): rejected too (no 128 that frame).
    mode = 3;
    frame(7506, true);
    readSlot(7506, hh, sl, a);
    int band[4] = {-1, -1, -1, -1};
    bool meaningful = sl >= 0;
    for (int i = 0; i < 4 && sl >= 0 && !a.empty(); ++i) {
      const cm::MaskView& mv = hh.slot[sl].view[i];
      band[i] = expectView(a, hh.atlasW, mv, i, false, n[i]);
      // The band's changed world texels as the GPU counts them (footprint rows overlapping the band; world = not 255).
      uint32_t world = 0, changed = 0;
      for (uint32_t r = 0; r < mv.h; ++r)
        for (uint32_t cc = 0; cc < mv.w; ++cc) {
          const size_t at = static_cast<size_t>(mv.y + r) * hh.atlasW + mv.x + cc;
          if (at >= a.size() || a[at] == 255) continue;
          ++world;
          const UINT fy0 = (r * 2 * H2[i]) / v[i].h;
          if (fy0 < H2[i] * 36 / 100) ++changed;
        }
      meaningful = meaningful && changed <= 0.4 * mv.w * mv.h && changed > 0.4 * world;
    }
    mode = 0;
    Check(meaningful && band[0] == 0 && band[1] == 0 && band[2] == 0 && band[3] == 0,
          "R76 overlay: a change of more than 40 % of a view's world texels (yet under 40 % of its rect) gives no 128");
    cmask::covl::g_on = false;
    frame(7507, true);
    readSlot(7507, hh, sl, a);
    int offBad = -1;
    if (sl >= 0 && !a.empty()) offBad = expectView(a, hh.atlasW, hh.slot[sl].view[0], 0, false, nb);
    Check(sl >= 0 && !(hh.slot[sl].flags & cm::slot_flags::overlay) && offBad == 0 && !fixdepth::g_cmOvlArm &&
              cmask::covl::g_passView < 0,
          "R74b: [Perception] CockpitMaskOverlay=0 -> v1 masks (no 128, no flags, no arm)");
    atOverlay = nullptr;
    atOverlayEndBound = nullptr;
    overlaySnap = nullptr;
    cmask::covl::ReleaseAllRes();  // our references on the test's targets
    for (int i = 0; i < 4; ++i) {
      Rel(hdrRtv[i]);
      Rel(hdr[i]);
    }
    Rel(altRtv);
    Rel(alt);
  }
  // R67: the heartbeat goes stale -> idle within 2 s (no dispatch, no Signal, bit 39 clear at once); resume.
  {
    LARGE_INTEGER qf;
    QueryPerformanceFrequency(&qf);
    const int64_t F = qf.QuadPart;
    beating = false;
    cm::reader_heartbeat(blk, qpcNow() - F * 19 / 10);  // 1.9 s old: still fresh
    frame(6000, true);
    cm::Header ha{};
    cm::seqlock_read(blk, ha);
    const bool stillOn = !cmask::g_idleA.load() && cm::find_slot(ha, 6000) >= 0;
    cm::reader_heartbeat(blk, qpcNow() - F * 21 / 10);  // 2.1 s old: stale
    const uint64_t c0 = cmask::g_captures.load(), k0 = cmask::g_commits.load(), s0 = cmask::g_signals.load(),
                   ti0 = cmask::g_toIdle.load(), dk0 = cmask::g_dCommits.load();
    frame(6001, true);
    frame(6002, true);
    cm::Header hs{};
    cm::seqlock_read(blk, hs);
    const bool idle = cmask::g_idleA.load() && cmask::g_captures.load() == c0 && cmask::g_commits.load() == k0 &&
                      cmask::g_signals.load() == s0 && cmask::g_toIdle.load() == ti0 + 1 && cmask::StatusBits() == 0 &&
                      !fdexport::WantUploads() && !fixdepth::g_mvWanted.load() && (hs.flags & cm::header_flags::idle) &&
                      (hs.flags & cm::header_flags::active) && cm::header_usable(hs, GetCurrentProcessId()) &&
                      cm::find_slot(hs, 6001) == -1 && cm::find_slot(hs, 6002) == -1;
    Check(stillOn && idle, "cockpit mask heartbeat: 1.9 s old still commits; 2.1 s old -> idle from the next frame (no "
                           "dispatch, no Signal, no commit, bit 39 clear at once, header idle | active, textures kept)");
    cdp::Header hds{};
    Check(cmask::g_dCommits.load() == dk0 && cdp::seqlock_read(dblk, hds) && (hds.flags & cdp::header_flags::idle) &&
              (hds.flags & cdp::header_flags::active) && !(cmask::StatusBits() & qvstatus::kCockpitDepth),
          "cockpit depth: mask idle -> no depth either, with its own heartbeat fresh (depth header idle, bit 41 clear)");
    // Real time: one store, then 2.05 s without one.
    cm::reader_heartbeat(blk, qpcNow());
    frame(6003, true);
    cm::Header hr{};
    cm::seqlock_read(blk, hr);
    const bool on = !cmask::g_idleA.load() && cm::find_slot(hr, 6003) >= 0;
    Sleep(2050);
    const uint64_t s1 = cmask::g_signals.load();
    frame(6004, true);
    cm::seqlock_read(blk, hr);
    Check(on && cmask::g_idleA.load() && cm::find_slot(hr, 6004) == -1 && cmask::g_signals.load() == s1,
          "cockpit mask heartbeat: a reader that stops storing -> the export stops within 2 s (real time)");
    // Resume: the reader stores again; the layout is unchanged, so that frame already has its mask.
    beating = true;
    const uint64_t ta0 = cmask::g_toActive.load();
    frame(6005, true);
    cm::seqlock_read(blk, hr);
    Check(!cmask::g_idleA.load() && cmask::g_toActive.load() == ta0 + 1 && cm::find_slot(hr, 6005) >= 0 &&
              !(hr.flags & cm::header_flags::idle) && cmask::g_signals.load() == s1 + 1 && fdexport::WantUploads() &&
              fixdepth::g_mvWanted.load() &&  // R76 (HB-3): the MV note follows the mask's heartbeat too
              (cmask::StatusBits() & (qvstatus::kCockpitMask | qvstatus::kCockpitMaskPaired)) ==
                  (qvstatus::kCockpitMask | qvstatus::kCockpitMaskPaired),
          "cockpit mask heartbeat: resume after a stale period (active, committed and signalled again, bits 39 / 40 set)");
    // R69: the depth reader stops (2.1 s old) while the mask's stays fresh: masks only, the mask-only variant.
    dbeating = false;
    cdp::reader_heartbeat(dblk, qpcNow() - F * 21 / 10);
    const uint64_t dk1 = cmask::g_dCommits.load(), sg1 = cmask::g_signals.load(), dto = cmask::g_dToIdle.load();
    frame(6100, true);
    frame(6101, true);
    cm::Header hm{};
    cdp::Header hd{};
    cm::seqlock_read(blk, hm);
    cdp::seqlock_read(dblk, hd);
    const int ms = cm::find_slot(hm, 6101);
    const bool maskOnly = ms >= 0 && !(hm.slot[ms].flags & cm::slot_flags::depth) && cdp::find_slot(hd, 6101) == -1 &&
                          cmask::g_dCommits.load() == dk1 && cmask::g_signals.load() == sg1 + 2 &&
                          cmask::g_dToIdle.load() == dto + 1 && (hd.flags & cdp::header_flags::idle) &&
                          (cmask::StatusBits() & qvstatus::kCockpitMask) && !(cmask::StatusBits() & qvstatus::kCockpitDepth);
    dbeating = true;
    frame(6102, true);
    cm::seqlock_read(blk, hm);
    cdp::seqlock_read(dblk, hd);
    const int ms2 = cm::find_slot(hm, 6102);
    const bool back = ms2 >= 0 && cdp::find_slot(hd, 6102) == ms2 && (hm.slot[ms2].flags & cm::slot_flags::depth) &&
                      !(hd.flags & cdp::header_flags::idle) && (cmask::StatusBits() & qvstatus::kCockpitDepth);
    Check(maskOnly && back, "cockpit depth: its reader stops -> masks only (no depth slot, no depth bit, one Signal per "
                            "frame, depth header idle, bit 41 clear); its reader resumes -> depth again from that frame");
  }
  // Divisor 4, dilate 1, near test off: a relayout (new generation), masks against the reference again.
  const uint32_t gen0 = h.generation;
  const HANDLE oldHandle = reinterpret_cast<HANDLE>(h.textureHandle[0]);
  cmask::g_div = 4;
  cmask::g_dilate = 1;
  cmask::g_nearM = 0.0f;
  frame(2000, true);  // layout mismatch: sizes noted, relayout at the commit
  frame(2001, true);
  {  // R70: the first commit after a relayout (new generation) is a reset; the next one is not.
    frame(2002, true);
    cmv::Header hv{};
    cmv::seqlock_read(mblk, hv);
    const int a = cmv::find_slot(hv, 2001), b = cmv::find_slot(hv, 2002);
    Check(a >= 0 && (hv.slot[a].flags & cmv::slot_flags::reset) && b >= 0 && !(hv.slot[b].flags & cmv::slot_flags::reset) &&
              hv.generation == cmask::g_hdr.generation && cmask::g_mResetPrev.load() + cmask::g_mResetGen.load() >= 1,
          "motion: the first commit after a relayout (new generation) is a reset, the next one is not; the motion "
          "header carries the new generation");
  }
  cm::Header h4{};
  cm::seqlock_read(blk, h4);
  const int s4 = cm::find_slot(h4, 2001);
  int bad4[4] = {-1, -1, -1, -1};
  ID3D11Texture2D* shared4 = nullptr;
  if (d1 && s4 >= 0)
    d1->OpenSharedResource1(reinterpret_cast<HANDLE>(h4.textureHandle[s4]), __uuidof(ID3D11Texture2D),
                            reinterpret_cast<void**>(&shared4));
  if (shared4) {
    const std::vector<uint8_t> a = ReadR8(dev, c, shared4);
    for (int i = 0; i < 4; ++i)
      bad4[i] = CompareView(a, h4.atlasW, h4.slot[s4].view[i], i, v[i].w, v[i].h, false, 4, 1, 0.0f);
  }
  DWORD hf = 0;
  const bool oldOpen = GetHandleInformation(oldHandle, &hf) != 0;
  sprintf_s(what, "cockpit mask: divisor 4 + dilate 1 + stencil only after a relayout (generation %u -> %u, atlas %ux%u; "
                  "mismatches %d %d %d %d; the old handle still open for the grace period)",
            gen0, h4.generation, h4.atlasW, h4.atlasH, bad4[0], bad4[1], bad4[2], bad4[3]);
  Check(s4 >= 0 && h4.generation != gen0 && h4.atlasW == 24 + 25 + 50 + 50 && h4.atlasH == 48 && bad4[0] == 0 &&
            bad4[1] == 0 && bad4[2] == 0 && bad4[3] == 0 && oldOpen && !(h4.slot[s4].view[0].flags & cm::view_flags::near_depth),
        what);
  {  // R69: the depth atlas follows the relayout (same generation as the mask), divisor 4 + dilate 1, stencil only.
    cdp::Header hd4{};
    cdp::seqlock_read(dblk, hd4);
    const int ds4 = cdp::find_slot(hd4, 2001);
    int dbad4[4] = {-1, -1, -1, -1}, inv4[4] = {-1, -1, -1, -1};
    ID3D11Texture2D* dt4 = nullptr;
    if (d1 && ds4 >= 0)
      d1->OpenSharedResource1(reinterpret_cast<HANDLE>(hd4.textureHandle[ds4]), __uuidof(ID3D11Texture2D),
                              reinterpret_cast<void**>(&dt4));
    if (dt4 && shared4) {
      const std::vector<float> a = ReadRG16F(dev, c, dt4);
      const std::vector<uint8_t> m = ReadR8(dev, c, shared4);
      for (int i = 0; i < 4; ++i)
        dbad4[i] = CompareDepth(a, hd4.atlasW, hd4.slot[ds4].view[i], i, v[i].w, v[i].h, 4, 1, 0.0f, &m, &inv4[i]);
    }
    Rel(dt4);
    sprintf_s(what, "cockpit depth: after the relayout (generation %u = the mask's %u, atlas %ux%u) divisor 4 + dilate 1 "
                    "+ stencil only equals the CPU reference (mismatches %d %d %d %d; invariant %d %d %d %d)",
              hd4.generation, h4.generation, hd4.atlasW, hd4.atlasH, dbad4[0], dbad4[1], dbad4[2], dbad4[3], inv4[0],
              inv4[1], inv4[2], inv4[3]);
    Check(ds4 == s4 && hd4.generation == h4.generation && hd4.atlasW == h4.atlasW && dbad4[0] == 0 && dbad4[1] == 0 &&
              dbad4[2] == 0 && dbad4[3] == 0 && inv4[0] == 0 && inv4[1] == 0 && inv4[2] == 0 && inv4[3] == 0,
          what);
  }
  // Late commit: no xrEndFrame; the next frame's first pass commits it, not pairable.
  frame(0, false);
  ob(&casc, true);  // EndFrame of that frame
  ob(&casc, false);
  cm::Header hl{};
  cm::seqlock_read(blk, hl);
  const cm::MaskSlot& late = hl.slot[hl.newestSlot];
  Check((late.flags & cm::slot_flags::late_commit) && !(late.flags & cm::slot_flags::display_time) && late.displayTime == 0 &&
            late.viewMask == 15 && cmask::g_late.load() >= 1 && (cmask::StatusBits() & qvstatus::kCockpitMaskPaired) == 0,
        "cockpit mask: without xrEndFrame the frame is committed late (late_commit, displayTime 0, bit 40 clear)");
  {  // R69
    cdp::Header hdl{};
    cdp::seqlock_read(dblk, hdl);
    const cdp::DepthSlot& dl = hdl.slot[hdl.newestSlot];
    Check(hdl.newestSlot == hl.newestSlot && dl.seq == late.maskSeq && (dl.flags & cdp::slot_flags::late_commit) &&
              !(dl.flags & cdp::slot_flags::display_time) && !(dl.flags & cdp::slot_flags::poses) && dl.displayTime == 0 &&
              dl.xrSpace == 0 && !(dl.view[0].flags & cdp::view_flags::pose) && std::isnan(dl.view[0].jitterPx[0]),
          "cockpit depth: the late commit too (late_commit, displayTime 0, no poses, jitter unknown)");
    cmv::Header hvl{};  // R70
    cmv::seqlock_read(mblk, hvl);
    const cmv::MotionSlot& ml = hvl.slot[hvl.newestSlot];
    Check(hvl.newestSlot == hl.newestSlot && ml.seq == late.maskSeq && (ml.flags & cmv::slot_flags::late_commit) &&
              !(ml.flags & cmv::slot_flags::display_time) && (ml.flags & cmv::slot_flags::reset) &&
              std::isnan(ml.view[0].jitterCurPx[0]),
          "motion: the late commit (late_commit, no displayTime, reset, jitter unknown)");
  }
  // The reader's release fence below the oldest slot's maskSeq: that frame gets no mask (no wait).
  {
    ID3D11Fence* rel = nullptr;
    if (d5)
      d5->OpenSharedFence(reinterpret_cast<HANDLE>(hl.releaseFenceHandle), __uuidof(ID3D11Fence), reinterpret_cast<void**>(&rel));
    ID3D11DeviceContext4* c4 = nullptr;
    c->QueryInterface(__uuidof(ID3D11DeviceContext4), reinterpret_cast<void**>(&c4));
    bool held = false, freed = false;
    if (rel && c4) {
      c4->Signal(rel, 1);  // released only maskSeq 1
      c->Flush();
      for (int k = 0; k < 200 && rel->GetCompletedValue() < 1; ++k) Sleep(5);
      const uint64_t busy0 = cmask::g_skipBusy.load(), commits0 = cmask::g_commits.load();
      // R76: the busy skip still refreshes the header's heartbeat (newestMaskSeq unchanged) at most every 100 ms.
      cm::Header hb0{}, hb1{};
      cm::seqlock_read(blk, hb0);
      cmask::g_hdrPubMs = 0;  // the last header write long ago
      const uint64_t beats0 = cmask::g_busyBeats.load();
      Sleep(2);
      // casc of the frame below ends the previous (late) frame: no commit pending now.
      frame(3000, true);
      cm::seqlock_read(blk, hb1);
      held = cmask::g_skipBusy.load() == busy0 + 1 && cmask::g_commits.load() == commits0;
      const bool beat = cmask::g_busyBeats.load() == beats0 + 1 && hb1.heartbeatQpc > hb0.heartbeatQpc &&
                        hb1.newestMaskSeq == hb0.newestMaskSeq && hb1.generation == hb0.generation &&
                        cm::find_slot(hb1, 3000) == -1 && cm::heartbeat_fresh(hb1, qpcNow(), 500);
      Check(beat, "R76 cockpit mask: a frame skipped for the release fence still refreshes heartbeatQpc (newestMaskSeq "
                  "unchanged, no slot): the reader's 500 ms freshness gate before its release cannot lock both sides");
      c4->Signal(rel, hl.newestMaskSeq + 10);
      c->Flush();
      for (int k = 0; k < 200 && rel->GetCompletedValue() < hl.newestMaskSeq + 10; ++k) Sleep(5);
      frame(3001, true);
      cm::Header hr{};
      cm::seqlock_read(blk, hr);
      freed = cm::find_slot(hr, 3001) >= 0;
      cmv::Header hv{};  // R70: the frame after a skipped one is a reset (its MVs point at a frame with no slot)
      cmv::seqlock_read(mblk, hv);
      const int a = cmv::find_slot(hv, 3001);
      freed = freed && a >= 0 && (hv.slot[a].flags & cmv::slot_flags::reset);
    }
    Rel(c4);
    Rel(rel);
    Check(held && freed, "cockpit mask: a slot the reader has not released is not written (frame skipped, no wait); "
                         "written again once released");
  }
  // Debug: CockpitMaskDump=2 / CockpitMaskLog=3 (staging copies mapped >= 2 commits later, files by Tick).
  {
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    wchar_t dir[MAX_PATH];
    swprintf_s(dir, L"%lscm65_dump_%lu", tmp, GetCurrentProcessId());
    CreateDirectoryW(dir, nullptr);
    cmask::Configure(true, 4, 15, 0.0, 1, 2, 3, dir);
    cmask::ConfigureDepth(true, 2);  // R69
    cmask::ConfigureMotion(true, true, 2);  // R70
    for (int f = 0; f < 8; ++f) {
      frame(4000 + f, true);
      c->Flush();  // DCS's present submits every frame; WARP runs nothing before a flush
      Sleep(5);
    }
    cmask::Tick();
    int pgms = 0;
    bool sizeOk = true;
    WIN32_FIND_DATAW fd{};
    const std::wstring pat = std::wstring(dir) + L"\\cockpit_mask\\*.pgm";
    HANDLE fh = FindFirstFileW(pat.c_str(), &fd);
    if (fh != INVALID_HANDLE_VALUE) {
      do {
        ++pgms;
        const std::wstring path = std::wstring(dir) + L"\\cockpit_mask\\" + fd.cFileName;
        FILE* in = nullptr;
        if (_wfopen_s(&in, path.c_str(), L"rb") == 0 && in) {
          std::vector<uint8_t> all(1 << 16);
          const size_t n = fread(all.data(), 1, all.size(), in);
          fclose(in);
          sizeOk = sizeOk && n > 149 * 48 && n - 149 * 48 < 200 && all[0] == 'P' && all[1] == '5';
        } else {
          sizeOk = false;
        }
        DeleteFileW(path.c_str());
      } while (FindNextFileW(fh, &fd));
      FindClose(fh);
    }
    int dumps = 0;  // R69: the depth dumps (raw R16G16_FLOAT, 149 x 48 x 4 bytes) and their text files
    bool dsizeOk = true, dtextOk = true;
    {
      const std::wstring dpat = std::wstring(dir) + L"\\cockpit_depth\\*";
      HANDLE dh = FindFirstFileW(dpat.c_str(), &fd);
      if (dh != INVALID_HANDLE_VALUE) {
        do {
          const std::wstring name = fd.cFileName;
          if (name == L"." || name == L"..") continue;
          const std::wstring path = std::wstring(dir) + L"\\cockpit_depth\\" + name;
          if (name.size() > 8 && name.substr(name.size() - 8) == L".r16g16f") {
            ++dumps;
            dsizeOk = dsizeOk && fd.nFileSizeLow == 149u * 48u * 4u;
          } else {
            FILE* in = nullptr;
            char t[64] = {};
            if (_wfopen_s(&in, path.c_str(), L"rb") == 0 && in) {
              fread(t, 1, sizeof(t) - 1, in);
              fclose(in);
            }
            dtextOk = dtextOk && strncmp(t, "seq ", 4) == 0;
          }
          DeleteFileW(path.c_str());
        } while (FindNextFileW(dh, &fd));
        FindClose(dh);
      }
      RemoveDirectoryW((std::wstring(dir) + L"\\cockpit_depth").c_str());
    }
    int mdumps = 0, mcls = 0;  // R70: the motion dumps (193 x 91 x 4 bytes MV, 193 x 91 class bytes, text)
    bool msizeOk = true;
    {
      const std::wstring mpat = std::wstring(dir) + L"\\cockpit_motion\\*";
      HANDLE mh = FindFirstFileW(mpat.c_str(), &fd);
      if (mh != INVALID_HANDLE_VALUE) {
        do {
          const std::wstring name = fd.cFileName;
          if (name == L"." || name == L"..") continue;
          if (name.size() > 8 && name.substr(name.size() - 8) == L".r16g16f") {
            ++mdumps;
            msizeOk = msizeOk && fd.nFileSizeLow == 193u * 91u * 4u;
          } else if (name.size() > 3 && name.substr(name.size() - 3) == L".r8") {
            ++mcls;
            msizeOk = msizeOk && fd.nFileSizeLow == 193u * 91u;
          }
          DeleteFileW((std::wstring(dir) + L"\\cockpit_motion\\" + name).c_str());
        } while (FindNextFileW(mh, &fd));
        FindClose(mh);
      }
      RemoveDirectoryW((std::wstring(dir) + L"\\cockpit_motion").c_str());
    }
    const std::wstring pat2 = std::wstring(dir) + L"\\cockpit_mask\\*.txt";
    fh = FindFirstFileW(pat2.c_str(), &fd);
    if (fh != INVALID_HANDLE_VALUE) {
      do DeleteFileW((std::wstring(dir) + L"\\cockpit_mask\\" + fd.cFileName).c_str());
      while (FindNextFileW(fh, &fd));
      FindClose(fh);
    }
    RemoveDirectoryW((std::wstring(dir) + L"\\cockpit_mask").c_str());
    RemoveDirectoryW(dir);
    sprintf_s(what, "cockpit mask debug: CockpitMaskDump=2 writes 2 PGM atlases (149x48) a few frames later (%d)", pgms);
    Check(pgms == 2 && sizeOk && cmask::g_dumpLeft.load() == 0 && cmask::g_logLeft.load() == 0, what);
    sprintf_s(what, "cockpit depth debug: CockpitDepthDump=2 writes 2 raw R16G16_FLOAT atlases (149x48x4 bytes) and "
                    "their text records (%d)", dumps);
    Check(dumps == 2 && dsizeOk && dtextOk && cmask::g_ddumpLeft.load() == 0, what);
    sprintf_s(what, "motion debug: CockpitMotionDump=2 writes 2 MV atlases (193x91x4 bytes) and 2 class atlases (%d, %d)",
              mdumps, mcls);
    Check(mdumps == 2 && mcls == 2 && msizeOk && cmask::g_mdumpLeft.load() == 0, what);
  }
  // R71: CockpitFgDump: N consecutive frames, each with its MV / class / depth atlases, views 0 / 1's submitted colour
  // (the viewport box of the swapchain image their CopyToHMDPass wrote, read at the pass end through the draw arm) and
  // a JSON sidecar; all paired by the commit's seq / engineFrame / displayTime; index.json at the end.
  {
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    wchar_t dir[MAX_PATH];
    swprintf_s(dir, L"%lscm71_fg_%lu\\", tmp, GetCurrentProcessId());
    CreateDirectoryW(dir, nullptr);
    {  // the reader releases every slot from here on (the release test above held it at its newest seq + 10)
      cm::Header hr{};
      cm::seqlock_read(blk, hr);
      ID3D11Fence* rel = nullptr;
      if (d5)
        d5->OpenSharedFence(reinterpret_cast<HANDLE>(hr.releaseFenceHandle), __uuidof(ID3D11Fence), reinterpret_cast<void**>(&rel));
      ID3D11DeviceContext4* c4 = nullptr;
      c->QueryInterface(__uuidof(ID3D11DeviceContext4), reinterpret_cast<void**>(&c4));
      if (rel && c4) {
        c4->Signal(rel, 1ull << 40);
        c->Flush();
        for (int k = 0; k < 200 && rel->GetCompletedValue() < (1ull << 40); ++k) Sleep(5);
      }
      Rel(c4);
      Rel(rel);
    }
    // DCS's swapchain images of views 0 / 1 (R8G8B8A8_TYPELESS 80x70, its copy into the viewport (6, 5) 64x60) and
    // the copy's source images (PS SRV 0); a draw that rasterises nothing (its "result" is the frame's pattern).
    ID3D11Texture2D* sc[2] = {};
    ID3D11RenderTargetView* scRtv[2] = {};
    ID3D11Texture2D* ldr[2] = {};
    ID3D11ShaderResourceView* ldrSrv[2] = {};
    for (int i = 0; i < 2; ++i) {
      D3D11_TEXTURE2D_DESC td{};
      td.Width = 80;
      td.Height = 70;
      td.MipLevels = td.ArraySize = 1;
      td.Format = DXGI_FORMAT_R8G8B8A8_TYPELESS;
      td.SampleDesc.Count = 1;
      td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
      if (SUCCEEDED(dev->CreateTexture2D(&td, nullptr, &sc[i]))) {
        D3D11_RENDER_TARGET_VIEW_DESC scRd{};
        scRd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        scRd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
        dev->CreateRenderTargetView(sc[i], &scRd, &scRtv[i]);
      }
      td.Width = 64;
      td.Height = 60;
      td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
      td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
      if (SUCCEEDED(dev->CreateTexture2D(&td, nullptr, &ldr[i]))) dev->CreateShaderResourceView(ldr[i], nullptr, &ldrSrv[i]);
    }
    ID3D11VertexShader* offVs = nullptr;
    ID3D11PixelShader* offPs = nullptr;
    {
      wchar_t sys[MAX_PATH];
      GetSystemDirectoryW(sys, MAX_PATH);
      HMODULE m = LoadLibraryW((std::wstring(sys) + L"\\d3dcompiler_47.dll").c_str());
      const pD3DCompile compile = m ? reinterpret_cast<pD3DCompile>(GetProcAddress(m, "D3DCompile")) : nullptr;
      const char* vsSrc = "float4 main(uint i : SV_VertexID) : SV_Position { return float4(9, 9, 0.5, 1); }";
      const char* psSrc = "float4 main() : SV_Target { return 0; }";
      ID3DBlob *vb = nullptr, *pb = nullptr;
      if (compile && SUCCEEDED(compile(vsSrc, strlen(vsSrc), "fg", nullptr, nullptr, "main", "vs_5_0", 0, 0, &vb, nullptr)) &&
          SUCCEEDED(compile(psSrc, strlen(psSrc), "fg", nullptr, nullptr, "main", "ps_5_0", 0, 0, &pb, nullptr))) {
        dev->CreateVertexShader(vb->GetBufferPointer(), vb->GetBufferSize(), nullptr, &offVs);
        dev->CreatePixelShader(pb->GetBufferPointer(), pb->GetBufferSize(), nullptr, &offPs);
      }
      if (vb) vb->Release();
      if (pb) pb->Release();
    }
    auto pat = [](long long dt, int view, int x, int y, int ch) {
      return static_cast<uint8_t>((x * 7 + y * 3 + ch * 50 + view * 90 + dt * 13) & 255);
    };
    long long curDt = 0;
    int hmdDraws = 0;
    long long skipDrawDt = -1;  // this frame's view 0 CopyToHMDPass has no draw the hook sees (the fallback)
    atHmd = [&](int i) {
      if (i > 1 || !scRtv[i] || !offVs || !offPs) return;
      std::vector<uint8_t> px(80 * 70 * 4, 0), src(64 * 60 * 4, 0);
      for (int y = 0; y < 60; ++y)
        for (int x = 0; x < 64; ++x)
          for (int ch = 0; ch < 4; ++ch)
            src[(y * 64 + x) * 4 + ch] = px[((y + 5) * 80 + x + 6) * 4 + ch] = pat(curDt, i, x, y, ch);
      c->UpdateSubresource(ldr[i], 0, nullptr, src.data(), 64 * 4, 0);  // the view's LDR image (the copy source)
      c->UpdateSubresource(sc[i], 0, nullptr, px.data(), 80 * 4, 0);  // DCS's copy, as its result
      if (i == 0 && curDt == skipDrawDt) {  // the draw arm missed this pass: nothing seen
        bindDefault();
        return;
      }
      c->OMSetRenderTargets(1, &scRtv[i], nullptr);
      D3D11_VIEWPORT vp{6, 5, 64, 60, 0, 1};
      c->RSSetViewports(1, &vp);
      c->PSSetShaderResources(0, 1, &ldrSrv[i]);
      c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
      c->IASetInputLayout(nullptr);
      c->VSSetShader(offVs, nullptr, 0);
      c->PSSetShader(offPs, nullptr, 0);
      c->Draw(3, 0);
      ++hmdDraws;
      ID3D11ShaderResourceView* noSrv = nullptr;
      c->PSSetShaderResources(0, 1, &noSrv);
      c->VSSetShader(nullptr, nullptr, 0);
      c->PSSetShader(nullptr, nullptr, 0);
      bindDefault();
    };
    auto readAll = [](const std::wstring& path, std::vector<uint8_t>& out) {
      out.clear();
      FILE* in = nullptr;
      if (_wfopen_s(&in, path.c_str(), L"rb") != 0 || !in) return false;
      fseek(in, 0, SEEK_END);
      const long n = ftell(in);
      fseek(in, 0, SEEK_SET);
      out.resize(n > 0 ? static_cast<size_t>(n) : 0);
      const size_t got = out.empty() ? 0 : fread(out.data(), 1, out.size(), in);
      fclose(in);
      return got == out.size();
    };
    auto jnum = [](const std::string& s, const char* key, size_t from = 0) -> long long {
      const std::string k = std::string("\"") + key + "\":";
      const size_t p = s.find(k, from);
      return p == std::string::npos ? -1 : strtoll(s.c_str() + p + k.size(), nullptr, 10);
    };
    // One session: N requested; the frames verified (all files, sizes, colour = its frame's pattern, seq chain).
    struct FgResult {
      int frames = 0, good = 0;
      bool gapFree = false, chain = true, paired = true, colourOk = true, sizesOk = true;
      long long stalls = -1;
    };
    auto session = [&](int n, int frames, bool flushEach) {
      FgResult res;
      cmask::ConfigureFg(0, dir);
      cmask::ConfigureFg(n, dir);
      for (int f = 0; f < frames; ++f) {
        curDt = 7000 + f + n * 100;
        frame(curDt, true);
        if (flushEach) {
          c->Flush();
          Sleep(3);
        }
      }
      c->Flush();
      Sleep(5);
      for (int f = 0; f < 3; ++f) {  // the last records' maps (>= 2 commits later) and the end
        curDt = 7900 + f + n * 100;
        frame(curDt, true);
        c->Flush();
        Sleep(3);
      }
      cmask::Tick();
      // The session folder: <dir>fg_dump\<date_time>\ (the newest).
      std::wstring sdir;
      WIN32_FIND_DATAW fd{};
      HANDLE h = FindFirstFileW((std::wstring(dir) + L"fg_dump\\*").c_str(), &fd);
      if (h != INVALID_HANDLE_VALUE) {
        do {
          const std::wstring nm = fd.cFileName;
          if (nm != L"." && nm != L".." && (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
              (sdir.empty() || nm > sdir))
            sdir = nm;
        } while (FindNextFileW(h, &fd));
        FindClose(h);
      }
      if (sdir.empty()) return res;
      sdir = std::wstring(dir) + L"fg_dump\\" + sdir + L"\\";
      std::vector<uint8_t> b;
      if (readAll(sdir + L"index.json", b)) {
        const std::string idx(b.begin(), b.end());
        res.frames = static_cast<int>(jnum(idx, "frames"));
        res.stalls = jnum(idx, "stalls");
        res.gapFree = idx.find("\"gapFree\":true") != std::string::npos && idx.find("\"files\":[\"f000\"") != std::string::npos;
      }
      long long prevSeq = -1, prevFrame = -1;
      for (int i = 0; i < res.frames; ++i) {
        wchar_t nm[16];
        swprintf_s(nm, L"f%03d", i);
        const std::wstring base = sdir + nm;
        if (!readAll(base + L".json", b)) {
          res.chain = false;
          continue;
        }
        const std::string js(b.begin(), b.end());
        const long long seq = jnum(js, "seq"), ef = jnum(js, "engineFrame"), dt = jnum(js, "displayTime");
        const size_t mo = js.find("\"motion\":{");
        const size_t dp = js.find("\"depth\":{");
        const long long mseq = mo == std::string::npos ? -2 : jnum(js, "seq", mo);
        const long long dseq = dp == std::string::npos ? -2 : jnum(js, "seq", dp);
        const long long prev = mo == std::string::npos ? -2 : jnum(js, "prevSeq", mo);
        if (mseq != seq || dseq != seq || (i > 0 && (seq != prevSeq + 1 || ef != prevFrame + 1 || prev != prevSeq)) ||
            js.find("\"gapBefore\":false") == std::string::npos)
          res.chain = false;
        if (js.find("\"paired\":true") == std::string::npos || js.find("\"paired\":false") != std::string::npos ||
            js.find("\"source\":\"swapchain\"") == std::string::npos)
          res.paired = false;
        prevSeq = seq;
        prevFrame = ef;
        std::vector<uint8_t> mvb, clb, deb;
        const long long aw = dp == std::string::npos ? 0 : jnum(js, "atlasW", dp);
        const long long ah = dp == std::string::npos ? 0 : jnum(js, "atlasH", dp);
        res.sizesOk = res.sizesOk && readAll(base + L"_mv.f16", mvb) && mvb.size() == 193u * 91u * 4u &&
                      readAll(base + L"_class.r8", clb) && clb.size() == 193u * 91u && readAll(base + L"_depth.f16", deb) &&
                      aw > 0 && ah > 0 && deb.size() == static_cast<size_t>(aw * ah * 4);
        // The MV atlas: DCS's MVs of view 0 at (0, 0) and view 1 at (96, 0) (the pipeline test's mvAt).
        if (mvb.size() == 193u * 91u * 4u) {
          float m0[2], m1[2];
          mvAt(0, 5, 7, m0);
          mvAt(1, 9, 3, m1);
          uint16_t a[2], bb[2];
          memcpy(a, &mvb[(7 * 193 + 5) * 4], 4);
          memcpy(bb, &mvb[(3 * 193 + 96 + 9) * 4], 4);
          res.sizesOk = res.sizesOk && a[0] == HalfOf(m0[0]) && a[1] == HalfOf(m0[1]) && bb[0] == HalfOf(m1[0]) &&
                        bb[1] == HalfOf(m1[1]);
        }
        bool colOk = true;
        for (int v = 0; v < 2; ++v) {
          std::vector<uint8_t> cb;
          if (!readAll(base + (v ? L"_color1.rgba8" : L"_color0.rgba8"), cb) || cb.size() != 64u * 60u * 4u) {
            colOk = false;
            continue;
          }
          for (int y = 0; y < 60 && colOk; ++y)
            for (int x = 0; x < 64 && colOk; ++x)
              for (int ch = 0; ch < 4; ++ch)
                if (cb[(y * 64 + x) * 4 + ch] != pat(dt, v, x, y, ch)) colOk = false;
        }
        res.colourOk = res.colourOk && colOk;
        if (colOk) ++res.good;
      }
      // Clean up the session.
      h = FindFirstFileW((sdir + L"*").c_str(), &fd);
      if (h != INVALID_HANDLE_VALUE) {
        do DeleteFileW((sdir + fd.cFileName).c_str());
        while (FindNextFileW(h, &fd));
        FindClose(h);
      }
      RemoveDirectoryW(sdir.c_str());
      return res;
    };
    const FgResult a = session(5, 8, true);
    sprintf_s(what, "frame-generation dump (R71): CockpitFgDump=5 writes 5 consecutive frames (frames %d, colour-checked "
                    "%d, gap-free %d, seq / engineFrame / prevSeq chain %d, colour paired from the swapchain %d, "
                    "MV / class / depth sizes and MVs %d, colour = its displayTime's pattern %d; %d HMD draws)",
              a.frames, a.good, a.gapFree, a.chain, a.paired, a.sizesOk, a.colourOk, hmdDraws);
    Check(a.frames == 5 && a.good == 5 && a.gapFree && a.chain && a.paired && a.sizesOk && a.colourOk, what);
    // A longer session without a flush per frame: the 16-record ring may fill (WARP runs nothing before a flush); its
    // oldest record is then mapped waiting (a stall) and no frame is dropped.
    const FgResult b = session(24, 26, false);
    sprintf_s(what, "frame-generation dump (R71): CockpitFgDump=24 without a flush per frame keeps every frame (frames "
                    "%d, colour-checked %d, gap-free %d, chain %d, ring stalls %lld)",
              b.frames, b.good, b.gapFree, b.chain, b.stalls);
    Check(b.frames == 24 && b.good == 24 && b.gapFree && b.chain && b.paired && b.sizesOk && b.colourOk, what);
    // A view whose CopyToHMDPass draw is not seen: its colour comes from the copy source of the previous frame's draw
    // (the same image texture, this frame's content), marked copy_source; nothing is missing.
    {
      const uint64_t fb0 = g_fgFallback.load();
      skipDrawDt = 7000 + 3 + 6 * 100;  // session(6, ...): frame 3
      const FgResult cfb = session(6, 8, true);
      skipDrawDt = -1;
      sprintf_s(what, "frame-generation dump (R71): a CopyToHMDPass without a seen draw takes the view's copy source "
                      "(frames %d, colour-checked %d, gap-free %d, chain %d, fallbacks %llu)",
                cfb.frames, cfb.good, cfb.gapFree, cfb.chain, static_cast<unsigned long long>(g_fgFallback.load() - fb0));
      Check(cfb.frames == 6 && cfb.good == 6 && cfb.gapFree && cfb.chain && cfb.sizesOk && cfb.colourOk &&
                g_fgFallback.load() == fb0 + 1 && cmask::g_fgFromSource == 1 && !g_fgLastSrc[0] && !g_fgLastSrc[1],
            what);
    }
    Check(cmask::g_fgReq.load() == 0 && !cmask::g_fgActive && !cmask::g_fgEnding && !g_fgArmOwn && g_fgView < 0 &&
              !g_fgSeen.dst && !g_fgSeen.src && !cmask::FgColourWanted(),
          "frame-generation dump (R71): after the session nothing is requested, armed or held (no colour copies)");
    {  // A full ring (16 committed records, none mapped yet): the oldest is mapped waiting (a stall), never dropped.
      const uint64_t stalls0 = cmask::g_fgStalls, frames0 = cmask::g_fgFrames.load();
      for (int i = 0; i < cmask::kFgRing; ++i) {
        cmask::g_fgRing[i].busy = true;
        cmask::g_fgRing[i].at = 100 + i;
      }
      cmask::FgRec* r = cmask::FgRecFor(123456);
      const bool oldestFreed = r == &cmask::g_fgRing[0] && r->open && r->frame == 123456;
      int stillBusy = 0;
      for (const cmask::FgRec& x : cmask::g_fgRing) stillBusy += x.busy ? 1 : 0;
      Check(oldestFreed && stillBusy == cmask::kFgRing - 1 && cmask::g_fgStalls == stalls0 + 1 &&
                cmask::g_fgFrames.load() == frames0 + 1,
            "frame-generation dump (R71): a full staging ring maps its oldest record waiting (one stall, the record "
            "written, not dropped) and reuses it");
      for (cmask::FgRec& x : cmask::g_fgRing) x.busy = x.open = false;
      std::lock_guard<std::mutex> lock(cmask::g_dbgMutex);
      cmask::g_fgOut.clear();
    }
    cmask::ConfigureFg(0, dir);
    atHmd = nullptr;
    RemoveDirectoryW((std::wstring(dir) + L"fg_dump").c_str());
    RemoveDirectoryW(dir);
    for (int i = 0; i < 2; ++i) {
      Rel(scRtv[i]);
      Rel(sc[i]);
      Rel(ldrSrv[i]);
      Rel(ldr[i]);
    }
    Rel(offVs);
    Rel(offPs);
  }
  // R69: a GBufferPass end whose begin this install did not see (a reinstall mid-pass) must not use the DSVs the
  // OM hook recorded (no references: live, an AddRef on a released DSV faulted in PickPassDsv).
  {
    ob(&gbuf, true);
    ID3D11RenderTargetView* none = nullptr;
    c->OMSetRenderTargets(1, &none, v[0].dsv);
    KeepDsv(c, v[0].dsv);  // as the OM hook records it
    const int recorded = g_seqN;
    g_gbBegun = false;     // what an install between the begin and the end leaves
    const uint64_t seen0 = g_passDsvSeen.load();
    bindDefault();
    ob(&gbuf, false);
    Check(recorded >= 1 && g_seqN == 0 && g_passDsvSeen.load() == seen0 && !g_passDsv && !g_gbBegun,
          "fixation depth (R69): a G-buffer pass end without its begin in this install ignores the recorded DSVs "
          "(no capture, sequence cleared)");
  }
  // R84 (R1b) -> R85: export off/on through the live path (fixation_depth.h Tick: Uninstall -> Install, the export keys
  // in the install key) with an OFXR-like reader on its own device. R85: the shared set is HELD across the switch: the
  // generation and every handle stay, active goes 1 -> 0 -> 1, no shared texture or fence is made, nothing of ours on
  // DCS's device grows (per type); under both reader policies (OFXR 0041: drops its set on an inactive header and
  // opens it again on active; Dev's R85 OFXR: keeps it while the generation is unchanged). Then every release path (a
  // layout change and a device change while held, the stop while held) bumps the generation, publishes R76's record
  // (flags 0) where it releases outside a relayout, and keeps the old handles' 2 s grace.
  {
    int n = 6;
    if (const char* e = getenv("QV_R84_N")) n = std::clamp(atoi(e), 1, 1000);
    const bool diag = getenv("QV_R84_N") != nullptr;  // the investigation run: video memory figures printed
    const bool realGrace = getenv("QV_R84_REAL_GRACE") != nullptr;
    const char* keyEnv = getenv("QV_R84_KEY");
    const bool motionKey = keyEnv && strcmp(keyEnv, "motion") == 0;  // the control: the motion export flips instead
    const ptiming::ExecFn execSave = ptiming::g_orig;
    if (!ptiming::g_orig) ptiming::g_orig = &r84::NoExec;  // Install's pass-timing step: present (no DCS here)
    ID3D11Device* rdev = nullptr;
    ID3D11DeviceContext* rctx = nullptr;
    D3D11CreateDevice(nullptr, QV_R84_DRIVER, nullptr, 0, want, 1, D3D11_SDK_VERSION, &rdev, nullptr, &rctx);
    ID3D11Device1* rd1 = nullptr;
    ID3D11Device5* rd5 = nullptr;
    ID3D11DeviceContext4* rc4 = nullptr;
    if (rdev) {
      rdev->QueryInterface(__uuidof(ID3D11Device1), reinterpret_cast<void**>(&rd1));
      rdev->QueryInterface(__uuidof(ID3D11Device5), reinterpret_cast<void**>(&rd5));
      rctx->QueryInterface(__uuidof(ID3D11DeviceContext4), reinterpret_cast<void**>(&rc4));
    }
    r84::Reader<cm::Header> rm;
    r84::Reader<cdp::Header> rdp;
    r84::Reader<cmv::Header> rmv;
    rm.Open(cmask::g_blockName.c_str());
    rdp.Open(cmask::g_dBlockName.c_str());
    rmv.Open(cmask::g_mBlockName.c_str());
    blk = rm.view;  // the frame's heartbeats through the reader's own mappings (they outlive our reinstalls)
    dblk = rdp.view;
    mblk = rmv.view;
    beating = dbeating = mbeating = true;
    bool keepAcross = false;  // the reader's policy (above)
    int opens = 0;            // sets the reader opened (imports of our handles)
    auto openSet = [&](auto& r, uint32_t gen, bool act, const uint64_t* h, int count) {
      if ((!act && !keepAcross) || gen != r.gen) {
        for (ID3D11Texture2D* t : r.tex)
          if (t) t->Release();
        r.tex.clear();
        r.gen = 0;
      }
      if (act && r.tex.empty() && rd1) {
        for (int i = 0; i < count; ++i) {
          ID3D11Texture2D* t = nullptr;
          if (h[i])
            rd1->OpenSharedResource1(reinterpret_cast<HANDLE>(h[i]), __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&t));
          r.tex.push_back(t);
        }
        r.gen = gen;
        ++opens;
      }
    };
    ID3D11Fence* rFence = nullptr;
    auto readAll = [&] {
      cm::Header h{};
      if (rm.view && cm::seqlock_read(rm.view, h)) {
        const bool act = (h.flags & cm::header_flags::active) != 0;
        if ((!act && !keepAcross) || h.generation != rm.gen) {
          Rel(rm.fence);
          Rel(rFence);
        }
        const int o0 = opens;
        openSet(rm, h.generation, act, h.textureHandle, cm::slot_count);
        const bool fresh = opens != o0;
        if (fresh && rd5 && h.fenceHandle)  // the commit fence (mask, depth and motion share it), held with the set
          rd5->OpenSharedFence(reinterpret_cast<HANDLE>(h.fenceHandle), __uuidof(ID3D11Fence), reinterpret_cast<void**>(&rFence));
        if (fresh && rd5 && h.releaseFenceHandle)
          rd5->OpenSharedFence(reinterpret_cast<HANDLE>(h.releaseFenceHandle), __uuidof(ID3D11Fence),
                               reinterpret_cast<void**>(&rm.fence));
        if (act && rm.fence && rc4 && h.newestMaskSeq) {
          rc4->Signal(rm.fence, h.newestMaskSeq);  // every committed slot released (the reader is done with it)
          rctx->Flush();
        }
      }
      cdp::Header d{};
      if (rdp.view && cdp::seqlock_read(rdp.view, d))
        openSet(rdp, d.generation, (d.flags & cdp::header_flags::active) != 0, d.textureHandle, cdp::slot_count);
      cmv::Header m{};
      if (rmv.view && cmv::seqlock_read(rmv.view, m))
        openSet(rmv, m.generation, (m.flags & cmv::header_flags::active) != 0, m.textureHandle, cmv::slot_count);
    };
    int64_t dt = 90000;
    auto run = [&](int frames) {
      for (int f = 0; f < frames; ++f) {
        frame(dt++, true);
        readAll();
        c->Flush();
      }
    };
    auto tick = [&] {
      Tick(false, false, 0, nullptr, false, std::wstring());
      readAll();  // the reader polls between our frames too (it sees the inactive records)
    };
    auto settle = [&] {  // old generations' NT handles: closed after their grace (Tick), the reader let go already
      if (realGrace) {
        Sleep(static_cast<DWORD>(cmask::kHandleGraceMs) + 100);
        tick();
      } else {
        cmask::CloseBuried(true);
      }
    };
    auto maskOn = [&](bool on) {  // CockpitMaskExport (the test's block names again: Configure resets them at off -> on)
      cmask::Configure(on, 2, 15, 1.0, 0);
      cmask::g_blockName = bname;
      cmask::g_dBlockName = dname;
      cmask::g_mBlockName = mname;
    };
    // The three records as the reader sees them.
    struct Rec {
      bool ok = false;
      uint32_t flags[3] = {}, gen[3] = {};
      uint64_t h[3][cm::slot_count] = {}, fence = 0, rel = 0;
    };
    auto rec = [&] {
      Rec r;
      cm::Header h{};
      cdp::Header d{};
      cmv::Header m{};
      r.ok = rm.view && cm::seqlock_read(rm.view, h) && rdp.view && cdp::seqlock_read(rdp.view, d) && rmv.view &&
             cmv::seqlock_read(rmv.view, m);
      r.flags[0] = h.flags, r.flags[1] = d.flags, r.flags[2] = m.flags;
      r.gen[0] = h.generation, r.gen[1] = d.generation, r.gen[2] = m.generation;
      for (int i = 0; i < cm::slot_count; ++i) r.h[0][i] = h.textureHandle[i], r.h[1][i] = d.textureHandle[i], r.h[2][i] = m.textureHandle[i];
      r.fence = h.fenceHandle;
      r.rel = h.releaseFenceHandle;
      return r;
    };
    auto act = [](const Rec& r, int b) { return (r.flags[b] & 1u) != 0; };  // header_flags::active is 1 in all three
    auto sameSet = [](const Rec& a, const Rec& b, int bi) {
      return a.gen[bi] == b.gen[bi] && !memcmp(a.h[bi], b.h[bi], sizeof(a.h[bi])) && a.h[bi][0] != 0;
    };
    cmask::Configure(true, 2, 15, 1.0, 0);
    cmask::ConfigureDepth(true, 0);
    cmask::ConfigureMotion(true, true, 0);
    r84::Begin(dev);
    tick();  // the live install (the test's own install had no depth / motion in its key)
    const bool liveInstall = g_installed.load() && g_depthWith && g_motionWith && cmask::g_dview.load();
    run(6);
    settle();
    run(2);
    const Rec g0 = rec();
    const bool g0ok = g0.ok && act(g0, 0) && act(g0, 1) && act(g0, 2) && g0.gen[1] == g0.gen[0] && g0.gen[2] == g0.gen[0] &&
                      g0.fence && g0.rel;
    const r84::Snap s0 = r84::Take(dev);
    const uint64_t dc0 = cmask::g_dCommits.load(), mc0 = cmask::g_mCommits.load(), tc0 = cmask::g_texCreates.load();
    const uint64_t holds0 = cmask::g_holds.load(), rep0 = cmask::g_republished.load(), rel0 = cmask::g_heldReleases.load();
    const int key = motionKey ? 2 : 1;  // the block whose export flips
    auto flipKey = [&](bool on) {
      if (motionKey)
        cmask::ConfigureMotion(on, true, 0);
      else
        cmask::ConfigureDepth(on, 0);
    };
    r84::Snap prev = s0;
    bool held = true, flipped = true;
    int opensA = 0, opensB = 0;
    for (int pass = 0; pass < 2; ++pass) {  // 0: the 0041 reader, 1: Dev's R85 reader
      keepAcross = pass == 1;
      const int o0 = opens;
      for (int i = 0; i < n; ++i) {
        flipKey(false);
        tick();
        const Rec off0 = rec();  // the reinstall's records before its first frame: all inactive, the same set
        run(4);  // the reader's heartbeats stay fresh in all three blocks (it stores them whatever the headers say)
        const Rec off1 = rec();  // mask (and the other companion) republished; the flipped block stays inactive
        settle();
        flipKey(true);
        tick();
        const Rec on0 = rec();
        run(4);
        settle();
        run(1);
        const Rec on1 = rec();
        for (int b = 0; b < 3; ++b) {
          held = held && sameSet(g0, off0, b) && sameSet(g0, off1, b) && sameSet(g0, on0, b) && sameSet(g0, on1, b);
          flipped = flipped && !act(off0, b) && act(off1, b) == (b != key) && !act(on0, b) && act(on1, b);
        }
        held = held && off1.fence == g0.fence && off1.rel == g0.rel && on1.fence == g0.fence && on1.rel == g0.rel;
        if (diag) {
          const r84::Snap si = r84::Take(dev);
          printf("     R85 cycle %3d (%s, %s reader): %s\n", i + 1, motionKey ? "motion" : "depth",
                 keepAcross ? "R85" : "0041", r84::Diff(prev, si).c_str());
          prev = si;
        }
      }
      (pass ? opensB : opensA) = opens - o0;
    }
    // CockpitMaskExport itself off and on (nothing installed in between: fixation depth is off too).
    maskOn(false);
    tick();
    const Rec moff0 = rec();
    // The reader keeps storing its heartbeats while the export is off (its contract): frames with fresh heartbeats
    // in all three blocks must leave every record inactive (nothing of ours writes them while the key is off).
    run(4);
    const Rec moff = rec();
    const bool offUninstalled = !g_installed.load() && cmask::g_held && !cmask::g_view.load() &&
                                moff0.ok && moff.ok && !memcmp(moff0.flags, moff.flags, sizeof(moff.flags)) &&
                                !memcmp(moff0.gen, moff.gen, sizeof(moff.gen)) && !memcmp(moff0.h, moff.h, sizeof(moff.h));
    settle();
    maskOn(true);
    tick();
    run(4);
    const Rec mon = rec();
    bool maskHeld = offUninstalled;
    for (int b = 0; b < 3; ++b)
      maskHeld = maskHeld && sameSet(g0, moff, b) && !act(moff, b) && sameSet(g0, mon, b) && act(mon, b);
    const r84::Snap s1 = r84::Take(dev);
    bool same = true;
    for (int k = 0; k < r84::kKinds; ++k) same = same && s1.live[k] == s0.live[k];
    const long fencesMade = s1.made[r84::kFence] - s0.made[r84::kFence];
    const uint64_t texMade = cmask::g_texCreates.load() - tc0;
    const uint64_t cycles = 4ull * n + 1;  // switches off (the mask's included)
    const bool counted = cmask::g_holds.load() - holds0 == cycles && cmask::g_republished.load() - rep0 == cycles &&
                         cmask::g_heldReleases.load() == rel0;
    const bool produced = motionKey ? cmask::g_mCommits.load() >= mc0 + static_cast<uint64_t>(2 * n)
                                    : cmask::g_dCommits.load() >= dc0 + static_cast<uint64_t>(2 * n);
    printf("     R85 %d x 2 %s off/on cycles + mask off/on: %s; shared textures made %llu, fences made %ld; reader "
           "opens 0041 %d, R85 %d\n",
           n, motionKey ? "motion" : "depth", r84::Diff(s0, s1).c_str(), static_cast<unsigned long long>(texMade),
           fencesMade, opensA, opensB);
    if (diag) {
      c->ClearState();
      c->Flush();
      IDXGIDevice3* d3 = nullptr;
      if (SUCCEEDED(dev->QueryInterface(__uuidof(IDXGIDevice3), reinterpret_cast<void**>(&d3))) && d3) {
        d3->Trim();
        d3->Release();
      }
      Sleep(500);
      const r84::Snap s2 = r84::Take(dev);
      printf("     R85 after ClearState + Flush + Trim: local VRAM %+.2f MB vs the first cycle's start\n",
             (static_cast<double>(s2.local) - static_cast<double>(s0.local)) / 1048576.0);
    }
    char r85what[900];
    sprintf_s(r85what,
              "R85: %d x 2 %s off/on cycles (+ CockpitMaskExport off/on) through Tick with a reader on its own device "
              "hold the shared set: generation %u and every handle unchanged (%d), active 1 -> 0 -> 1 (%d), mask off "
              "uninstalled and held (%d), shared textures made %llu, fences made %ld, holds / republished counted (%d), "
              "objects of ours per type %s, handles %lu -> %lu, the companion committed after every switch (%d); the "
              "0041 reader re-opened (%d sets) and stays correct, the R85 reader never re-opened (%d)",
              n, motionKey ? "motion" : "depth", g0.gen[0], held, flipped, maskHeld,
              static_cast<unsigned long long>(texMade), fencesMade, counted, same ? "all equal" : "GREW", s0.handles,
              s1.handles, produced, opensA, opensB);
    Check(liveInstall && g0ok && held && flipped && maskHeld && texMade == 0 && fencesMade == 0 && counted && same &&
              produced && opensA > 0 && opensB == 0 &&
              (QV_R84_DRIVER != D3D_DRIVER_TYPE_WARP || s1.handles <= s0.handles + 2),
          r85what);  // a hardware driver's own handles (its threads, events) settle at a few more: printed only
    r84::End();
    keepAcross = false;
    auto graves = [] {
      std::lock_guard<std::mutex> lock(cmask::g_graveMutex);
      return cmask::g_grave.size();
    };
    // R76's grace: an old handle still opens (cross-device) right after a release.
    auto opensOld = [&](uint64_t h) {
      ID3D11Texture2D* t = nullptr;
      const bool ok = rd1 && SUCCEEDED(rd1->OpenSharedResource1(reinterpret_cast<HANDLE>(h), __uuidof(ID3D11Texture2D),
                                                                reinterpret_cast<void**>(&t))) && t;
      Rel(t);
      return ok;
    };
    // A layout change while held (the divisor while the depth export is off): released, a new generation, new handles.
    {
      const Rec a = rec();
      const uint64_t r0 = cmask::g_heldReleases.load();
      cmask::CloseBuried(true);
      cmask::ConfigureDepth(false, 0);
      tick();
      const bool wasHeld = cmask::g_held;
      cmask::Configure(true, 4, 15, 1.0, 0);
      run(3);
      const Rec b = rec();
      const bool grace = graves() > 0 && opensOld(a.h[0][0]);
      const bool ok = wasHeld && !cmask::g_held && cmask::g_heldReleases.load() == r0 + 1 && b.gen[0] != a.gen[0] &&
                      b.gen[0] != 0 && act(b, 0) && b.h[0][0] != 0 && b.h[0][0] != a.h[0][0] && b.gen[2] == b.gen[0] &&
                      act(b, 2) && b.flags[1] == 0 && b.gen[1] == b.gen[0] && b.h[1][0] == 0 && grace;
      Check(ok, "R85: a layout change while the set is held (divisor 2 -> 4 with the depth export off) releases it: a "
                "new generation with new handles for mask and motion, R76's record in the depth block (that "
                "generation, flags 0, no handles), the old handles still open within their 2 s grace");
      settle();
      cmask::Configure(true, 2, 15, 1.0, 0);
      cmask::ConfigureDepth(true, 0);
      tick();
      run(4);
      const Rec c2 = rec();
      Check(c2.ok && act(c2, 0) && act(c2, 1) && act(c2, 2) && c2.gen[1] == c2.gen[0] && c2.gen[2] == c2.gen[0] &&
                c2.gen[0] != b.gen[0],
            "R85: the depth export on again after that needs a set with depth: a new generation for all three (made "
            "together)");
      settle();
    }
    // A device change while held: R76's record first (a new generation, flags 0, no handles), then a new set.
    if (rdev && rctx) {
      const Rec a = rec();
      const uint64_t r0 = cmask::g_heldReleases.load();
      cmask::ConfigureDepth(false, 0);
      tick();
      const bool wasHeld = cmask::g_held;
      const bool ens = cmask::EnsureBase(rdev, rctx);  // DCS's device as the render thread finds it: another one
      const Rec b = rec();
      const bool grace = graves() > 0 && opensOld(a.h[0][0]);
      const bool ok = wasHeld && ens && !cmask::g_held && cmask::g_heldReleases.load() == r0 + 1 && b.gen[0] != a.gen[0] &&
                      b.gen[0] != 0 && b.flags[0] == 0 && b.h[0][0] == 0 && b.fence == 0 && b.gen[2] == b.gen[0] &&
                      b.flags[2] == 0 && b.gen[1] == b.gen[0] && b.flags[1] == 0 && b.h[1][0] == 0 && grace;
      run(3);  // back on the test device: released again, made again
      const Rec c2 = rec();
      Check(ok && act(c2, 0) && c2.gen[0] != b.gen[0] && c2.h[0][0] != 0,
            "R85: a device change while the set is held releases it with R76's record (a new generation, flags 0, no "
            "handles; the old handles open within their grace); the next device gets a new set and generation");
      settle();
      cmask::ConfigureDepth(true, 0);
      tick();
      run(4);
      settle();
    }
    // The stop while held (CockpitMaskExport off, nothing installed; fixation_depth.h Shutdown's ReleaseHeld).
    {
      const Rec a = rec();
      maskOn(false);
      tick();
      const bool wasHeld = cmask::g_held && !g_installed.load();
      cmask::ReleaseHeld("test: payload stop");
      const Rec b = rec();
      const size_t gv = graves();
      const bool grace = gv > 0 && opensOld(a.h[0][0]) && opensOld(a.h[1][0]) && opensOld(a.h[2][0]);
      bool rel76 = b.ok;
      for (int k = 0; k < 3; ++k) rel76 = rel76 && b.gen[k] == b.gen[0] && b.flags[k] == 0 && b.h[k][0] == 0;
      const bool ok = wasHeld && !cmask::g_held && !cmask::g_r.dev && rel76 && b.gen[0] != a.gen[0] && b.gen[0] != 0 &&
                      b.fence == 0 && b.rel == 0 && grace;
      cmask::CloseBuried(true);
      Check(ok && graves() == 0,
            "R85: the stop while the set is held releases it: R76's records in all three blocks (a new generation, "
            "flags 0, no handles), every old handle still open within its grace, closed by the unload");
      maskOn(true);
      tick();
      run(4);
      const Rec c2 = rec();
      Check(c2.ok && act(c2, 0) && act(c2, 1) && act(c2, 2) && c2.gen[0] != b.gen[0] && c2.gen[1] == c2.gen[0],
            "R85: on again after the stop's release: a new set, a new generation");
      settle();
    }
    // R86: regressions of the R85 held set (docs\research\R86_regression_review.md).
    // C1: a companion block no reader maps keeps no stale record. Depth off (held), the reader unmaps the depth block,
    // a layout change releases the held set: the depth record in our copy must be R76's (that generation, no handles),
    // so the depth export on again (an install while the newer set is held) does not publish the old generation and
    // the released, closed handles into the block it makes anew.
    {
      const Rec a = rec();
      cmask::ConfigureDepth(false, 0);
      tick();
      const bool wasHeld = cmask::g_held;
      rdp.Close();
      dblk = nullptr;
      dbeating = false;
      cmask::Configure(true, 4, 15, 1.0, 0);
      run(3);
      const Rec b = rec();
      settle();
      cmask::ConfigureDepth(true, 0);
      tick();
      rdp.Open(dname);
      dblk = rdp.view;
      dbeating = true;
      cdp::Header d{};
      const bool rd = rdp.view && cdp::seqlock_read(rdp.view, d);
      // (b: the mask block only; the reader maps no depth block there)
      const bool ok = wasHeld && a.h[1][0] != 0 && b.gen[0] != 0 && b.gen[0] != a.gen[0] && rd &&
                      !(d.flags & cdp::header_flags::active) && d.generation != a.gen[1] && d.textureHandle[0] == 0 &&
                      d.textureHandle[1] == 0;
      if (!ok)
        printf("     (R86 C1: held %d, depth record after the reinstall: generation %u (old %u, set %u), handle %llx "
               "(old %llx), flags 0x%x)\n", wasHeld, d.generation, a.gen[1], b.gen[0],
               static_cast<unsigned long long>(d.textureHandle[0]), static_cast<unsigned long long>(a.h[1][0]), d.flags);
      Check(ok, "R86 C1: a held set released by a layout change while no reader maps the depth block: the depth export "
                "on again publishes R76's record (not the old generation and its closed NT handles)");
      cmask::Configure(true, 2, 15, 1.0, 0);
      run(4);
      const Rec c2 = rec();
      Check(c2.ok && act(c2, 0) && act(c2, 1) && act(c2, 2) && c2.gen[1] == c2.gen[0] && c2.h[1][0] != 0,
            "R86 C1: then a set with depth (all three active, one generation)");
      settle();
    }
    // C2: the atlas creation failing while the set is held (a layout change): R76's record, a new generation and no
    // handles in all three blocks (before: flags 0 under the old generation with its texture handles).
    {
      const Rec a = rec();
      cmask::ConfigureDepth(false, 0);
      tick();
      const bool wasHeld = cmask::g_held;
      cmask::g_testFailAtlas = true;
      cmask::Configure(true, 4, 15, 1.0, 0);
      run(2);
      cmask::g_testFailAtlas = false;
      const Rec b = rec();
      bool rel76 = b.ok;
      for (int k = 0; k < 3; ++k) rel76 = rel76 && b.gen[k] == b.gen[0] && !act(b, k) && b.h[k][0] == 0;
      const bool ok = wasHeld && cmask::g_fault.load() && !cmask::g_held && rel76 && b.gen[0] != a.gen[0] && b.gen[0] != 0;
      if (!ok)
        printf("     (R86 C2: held %d fault %d; generation %u/%u/%u (old %u), mask handle %llx (old %llx))\n", wasHeld,
               static_cast<int>(cmask::g_fault.load()), b.gen[0], b.gen[1], b.gen[2], a.gen[0],
               static_cast<unsigned long long>(b.h[0][0]), static_cast<unsigned long long>(a.h[0][0]));
      Check(ok, "R86 C2: the atlas creation failing while the set is held publishes R76's record in all three blocks "
                "(a new generation, flags 0, no handles)");
      cmask::Configure(true, 2, 15, 1.0, 0);
      cmask::ConfigureDepth(true, 0);
      tick();  // the reinstall clears the latched fault
      run(4);
      const Rec c2 = rec();
      Check(c2.ok && act(c2, 0) && act(c2, 1) && act(c2, 2) && c2.gen[0] != b.gen[0],
            "R86 C2: the next install after it makes a new set (a new generation)");
      settle();
    }
    // V8: HoldBlocker's release branches through the live path: a latched fault and a removed device at the export
    // switch release the set with R76's record (a new generation, flags 0) instead of holding it.
    for (int why = 0; why < 2; ++why) {
      const Rec a = rec();
      const uint64_t holds0 = cmask::g_holds.load();
      if (why == 0)
        cmask::g_fault = true;
      else
        cmask::g_testRemoved = true;
      cmask::ConfigureDepth(false, 0);
      tick();
      cmask::g_testRemoved = false;
      const Rec b = rec();
      // (the install after it publishes the mask block's empty record, generation 0, until its first relayout; the
      // depth block, its export off, keeps the release's R76 record)
      const bool ok = act(a, 0) && !cmask::g_held && cmask::g_holds.load() == holds0 && b.ok && b.gen[1] != a.gen[1] &&
                      b.gen[1] != 0 && !act(b, 0) && b.h[0][0] == 0 && b.flags[1] == 0 && b.h[1][0] == 0;
      Check(ok, why == 0 ? "R86 V8: a fault latched at the export switch: the set is released, not held (a new "
                           "generation, no handles)"
                         : "R86 V8: a removed device at the export switch: the set is released, not held (a new "
                           "generation, no handles)");
      cmask::ConfigureDepth(true, 0);
      tick();
      run(4);
      settle();
    }
    // L1 / L2 / C3: fixation depth installed for another feature (the cone) with the mask export off: its reinstalls
    // leave a held set alone (no re-hold, no count), publish no new generation when nothing is held, and the
    // once-a-second check releases a held set whose device was removed.
    {
      const Rec a = rec();
      maskOn(false);
      Tick(true, false, 0, nullptr, false, std::wstring());  // the mask export off, the cone on: installed, mask not
      readAll();
      const bool setup = g_installed.load() && !g_maskWith && cmask::g_held;
      const uint64_t holds0 = cmask::g_holds.load();
      const Rec b = rec();
      Tick(true, true, 0, nullptr, false, std::wstring());  // the grid on: a reinstall of fixation depth
      readAll();
      const Rec c2 = rec();
      const bool l1 = setup && cmask::g_held && cmask::g_holds.load() == holds0 && sameSet(b, c2, 0) && sameSet(b, c2, 1) &&
                      !act(c2, 0);
      if (!l1)
        printf("     (R86 L1: setup %d (installed %d, mask with %d, held %d), holds %llu -> %llu, generation %u -> %u)\n",
               setup, static_cast<int>(g_installed.load()), static_cast<int>(g_maskWith), static_cast<int>(cmask::g_held),
               static_cast<unsigned long long>(holds0), static_cast<unsigned long long>(cmask::g_holds.load()), b.gen[0],
               c2.gen[0]);
      Check(l1, "R86 L1: a reinstall of fixation depth for another feature while the mask export is off leaves the "
                "held set as it is (the same generation and handles, not held again)");
      cmask::g_testRemoved = true;
      Tick(true, true, 0, nullptr, false, std::wstring());  // no change: the once-a-second check
      readAll();
      cmask::g_testRemoved = false;
      const Rec d = rec();
      const bool c3 = !cmask::g_held && d.ok && d.gen[0] != c2.gen[0] && d.flags[0] == 0 && d.h[0][0] == 0 &&
                      d.gen[1] == d.gen[0] && d.h[1][0] == 0;
      Check(c3, "R86 C3: with fixation depth installed for another feature and the mask export off, a held set whose "
                "device was removed is released at the next Tick (R76's record)");
      Tick(true, false, 0, nullptr, false, std::wstring());  // the grid off: another reinstall, nothing held now
      readAll();
      const Rec e = rec();
      Check(e.ok && e.gen[0] == d.gen[0] && e.gen[1] == d.gen[1] && e.gen[2] == d.gen[2],
            "R86 L1: with nothing held and the mask export off, a reinstall for another feature publishes no new "
            "generation");
      Tick(false, false, 0, nullptr, false, std::wstring());  // nothing on: uninstalled
      readAll();
      maskOn(true);
      tick();
      run(4);
      const Rec f2 = rec();
      Check(f2.ok && act(f2, 0) && act(f2, 1) && act(f2, 2) && f2.gen[0] != a.gen[0],
            "R86: the mask export on again after that: a new set");
      settle();
    }
    // V4: the payload stop while the set is held, through fixation_depth.h's Shutdown itself (Uninstall(false) +
    // ReleaseHeld + the unload's CloseBuried): R76's records in all three blocks, nothing held or buried after it.
    {
      const Rec a = rec();
      maskOn(false);
      tick();
      const bool wasHeld = cmask::g_held && !g_installed.load();
      Shutdown();
      const Rec b = rec();
      bool rel76 = b.ok;
      for (int k = 0; k < 3; ++k) rel76 = rel76 && b.gen[k] == b.gen[0] && b.flags[k] == 0 && b.h[k][0] == 0;
      Check(wasHeld && !cmask::g_held && !cmask::g_r.dev && rel76 && b.gen[0] != a.gen[0] && graves() == 0,
            "R86 V4: fixation_depth.h Shutdown while the set is held: R76's records in all three blocks, the set "
            "released, every old handle closed");
      maskOn(true);
      tick();
      run(4);
      settle();
    }
    // R88 (agreed with Dev, option B): no relayout while no flight frame is drawn. Flight here = the collect hook culled
    // a Quad Views world frame (g_quadFrame) for this commit and the one before; the F10 map / a loading screen = other
    // view sizes and no culling. Same reader harness and both reader policies as R85. R91: the builder thread makes the
    // sets (the render thread waits for it in these frame-exact checks, except in the async check below).
    {
      cmask::g_flightGate = true;
      cmask::g_buildWaitMs = 5000;
      flight = true;
      vpCut = 0;
      run(4);
      settle();
      const Rec f0 = rec();
      const bool f0ok = f0.ok && act(f0, 0) && act(f0, 1) && act(f0, 2) && f0.gen[1] == f0.gen[0] &&
                        f0.gen[2] == f0.gen[0];
      const uint64_t tcR88 = cmask::g_texCreates.load(), rl0 = cmask::g_relayouts.load(), mh0 = cmask::g_mapHolds.load(),
                     mr0 = cmask::g_mapResumes.load(), rp0 = cmask::g_republished.load();
      bool sameR88 = true, toggled = true, noCommitInMap = true;
      int opensP[2] = {};
      for (int pass = 0; pass < 2; ++pass) {  // 0: the 0041 reader (drops its set when inactive), 1: the 0044 one
        keepAcross = pass == 1;
        const int o0 = opens;
        for (int i = 0; i < 3; ++i) {
          flight = false;  // F10 entry: the map's sizes, no world culling
          vpCut = 6;
          const uint64_t k0 = cmask::g_commits.load();
          run(4);
          noCommitInMap = noCommitInMap && cmask::g_commits.load() == k0;
          const Rec m = rec();
          flight = true;  // F10 exit: the flight sizes again
          vpCut = 0;
          run(3);
          const Rec b = rec();
          for (int k = 0; k < 3; ++k) {
            sameR88 = sameR88 && sameSet(f0, m, k) && sameSet(f0, b, k);
            toggled = toggled && !act(m, k) && act(b, k);
          }
          sameR88 = sameR88 && m.fence == f0.fence && m.rel == f0.rel && b.fence == f0.fence && b.rel == f0.rel;
        }
        opensP[pass] = opens - o0;
      }
      keepAcross = false;
      const bool countedR88 = cmask::g_texCreates.load() == tcR88 && cmask::g_relayouts.load() == rl0 &&
                           cmask::g_mapHolds.load() == mh0 + 6 && cmask::g_mapResumes.load() == mr0 + 6 &&
                           cmask::g_republished.load() == rp0;
      char whatR88[800];
      sprintf_s(whatR88,
                "R88: 2 x 3 F10 visits (other sizes, no culling) and back to flight with the same sizes keep the shared "
                "set: generation %u, every handle and both fences unchanged (%d), active 1 -> 0 -> 1 in all three "
                "blocks (%d), no commit on the map (%d), no texture made and no relayout, holds / resumes counted "
                "(%d); the 0041 reader re-opened (%d sets), the 0044 reader never (%d)",
                f0.gen[0], sameR88, toggled, noCommitInMap, countedR88, opensP[0], opensP[1]);
      Check(f0ok && sameR88 && toggled && noCommitInMap && countedR88 && opensP[0] > 0 && opensP[1] == 0, whatR88);
      // A resolution / DLSS change made on the map: other sizes on the flight frames after it release the held set.
      {
        flight = false;
        vpCut = 6;
        run(3);
        const Rec m = rec();
        const uint64_t mrl0 = cmask::g_mapRelayouts.load(), tcm = cmask::g_texCreates.load();
        flight = true;
        vpCut = 10;
        run(1);
        const Rec n1 = rec();  // the first flight commit: the gate needs two, the set stays held
        run(3);
        const Rec nNew = rec();
        bool ok = sameSet(f0, m, 0) && !act(m, 0) && sameSet(f0, n1, 0) && !act(n1, 0);
        for (int k = 0; k < 3; ++k) ok = ok && act(nNew, k) && nNew.gen[k] == nNew.gen[0];
        ok = ok && nNew.gen[0] != f0.gen[0] && nNew.h[0][0] != f0.h[0][0] && cmask::g_mapRelayouts.load() == mrl0 + 1 &&
             cmask::g_texCreates.load() > tcm;
        Check(ok, "R88: other sizes on the flight frames after F10 (a resolution change made on the map) release the "
                  "held set after two flight commits: a new generation with new handles for mask, depth and motion");
        settle();
        vpCut = 0;
        run(4);
        settle();
        const Rec back = rec();
        Check(back.ok && act(back, 0) && back.gen[0] != nNew.gen[0],
              "R88: a layout change in flight (flight commits on both sides) relays at once, as before");
      }
      // The backstop: a closed gate past g_mapHoldMaxMs takes the layout as it is (before R88's behaviour).
      {
        const Rec a = rec();
        const uint64_t to0 = cmask::g_mapTimeouts.load();
        cmask::g_mapHoldMaxMs = 30;
        flight = false;
        vpCut = 6;
        run(2);
        const Rec heldRec = rec();
        Sleep(80);
        run(2);
        const Rec b = rec();
        cmask::g_mapHoldMaxMs = 15u * 60000u;
        Check(!act(heldRec, 0) && sameSet(a, heldRec, 0) && act(b, 0) && b.gen[0] != a.gen[0] &&
                  cmask::g_mapTimeouts.load() == to0 + 1,
              "R88 backstop: a hold longer than g_mapHoldMaxMs takes the other layout (a new generation), counted once");
        settle();
        flight = true;
        vpCut = 0;
        run(4);
        settle();
      }
      // R91 async: a layout change's set is made by the builder thread and swapped in at a later commit.
      {
        run(2);
        const Rec a = rec();
        const uint64_t w0 = cmask::g_buildWaits.load(), n0 = cmask::g_buildN.load();
        cmask::g_buildWaitMs = 0;
        cmask::g_testBuildDelayMs = 300;
        cmask::g_buildTid = 0;
        vpCut = 4;
        run(1);
        const Rec b = rec();  // requested, the builder still at it: the records as they were
        const bool pend = cmask::g_bPending;
        Sleep(600);
        run(1);  // the commit that finds it made swaps it in
        const Rec c2 = rec();
        run(2);
        const Rec d = rec();
        cmask::g_testBuildDelayMs = 0;
        const DWORD tid = cmask::g_buildTid.load();
        bool ok = pend && sameSet(a, b, 0) && act(b, 0) && c2.gen[0] != a.gen[0] && act(c2, 0) &&
                  cmask::g_buildWaits.load() > w0 && cmask::g_buildN.load() == n0 + 1 && tid != 0 &&
                  tid != GetCurrentThreadId();
        for (int k = 0; k < 3; ++k) ok = ok && act(d, k) && d.gen[k] == c2.gen[0];
        Check(ok, "R91: a layout change's textures, UAVs and NT handles are made by the builder thread (not the render "
                  "thread) and swapped in at a later commit; meanwhile the records stay as they were");
        settle();
        // Requested and not wanted any more before it was made (the set's own layout again): discarded.
        const uint64_t dsc0 = cmask::g_buildsDiscarded.load();
        cmask::g_testBuildDelayMs = 300;
        vpCut = 0;
        run(1);  // requested
        vpCut = 4;
        run(1);  // the set's own layout again
        cmask::g_testBuildDelayMs = 0;
        Sleep(500);
        run(2);
        const Rec e = rec();
        Check(cmask::g_buildsDiscarded.load() == dsc0 + 1 && !cmask::g_bPending && sameSet(d, e, 0) && act(e, 0),
              "R91: a set requested and not wanted any more (the layout back to the set's own before it was made) is "
              "discarded; the set in use stays (the same generation and handles)");
        cmask::g_buildWaitMs = 5000;
        vpCut = 0;
        run(4);
        settle();
      }
      // R88 + R91: nothing made yet (a fresh install on the map): no set on the map; the base objects made by the
      // install (config thread), the set at the first flight commits.
      {
        maskOn(false);
        tick();
        Shutdown();  // the set released, the builder stopped
        const uint64_t bi0 = cmask::g_baseAtInstall.load(), bc0 = cmask::g_baseAtCapture.load(),
                       tcb = cmask::g_texCreates.load(), w0 = cmask::g_mapWaitFrames.load();
        flight = false;
        vpCut = 6;
        maskOn(true);
        tick();  // the install
        const bool atInstall = cmask::g_baseAtInstall.load() == bi0 + 1 && cmask::g_r.baseOk && cmask::g_bRunning.load();
        run(4);
        const Rec m = rec();
        const bool waited = !act(m, 0) && cmask::g_texCreates.load() == tcb && cmask::g_mapWaitFrames.load() > w0 &&
                            !cmask::g_r.haveLayout;
        flight = true;
        vpCut = 0;
        run(4);
        const Rec f = rec();
        Check(atInstall && cmask::g_baseAtCapture.load() == bc0,
              "R91: the compute shaders, parameter buffer, fences and queries are made by Install on the config thread "
              "when DCS's device is known (none at a capture on the render thread)");
        Check(waited && act(f, 0) && act(f, 1) && act(f, 2) && f.gen[1] == f.gen[0] && f.gen[2] == f.gen[0],
              "R88: with nothing made yet the map makes no set (the record stays inactive); the first flight commits "
              "make it");
        settle();
      }
      // R91 (R90 7.3): a new dump request empties its folder (files only) before its first file.
      {
        wchar_t tmp[MAX_PATH] = {};
        GetTempPathW(MAX_PATH, tmp);
        wchar_t sub[64];
        swprintf_s(sub, L"qv_r91_dump_%lu\\", GetCurrentProcessId());
        const std::wstring base = std::wstring(tmp) + sub;
        CreateDirectoryW(base.c_str(), nullptr);
        const wchar_t* kinds[3] = {L"cockpit_mask", L"cockpit_depth", L"cockpit_motion"};
        const wchar_t* olds[3] = {L"\\old_1.txt", L"\\old_2.pgm", L"\\keep\\inner.txt"};
        for (const wchar_t* k : kinds) {
          const std::wstring d = base + k;
          CreateDirectoryW(d.c_str(), nullptr);
          CreateDirectoryW((d + L"\\keep").c_str(), nullptr);
          for (const wchar_t* oldName : olds) {
            FILE* fo = nullptr;
            if (_wfopen_s(&fo, (d + oldName).c_str(), L"wb") == 0 && fo) {
              fputs("x", fo);
              fclose(fo);
            }
          }
        }
        cmask::ConfigureDepth(true, 2);
        cmask::ConfigureMotion(true, true, 2);
        cmask::Configure(true, 2, 15, 1.0, 0, 2, 0, base);
        run(6);
        tick();
        auto exists = [](const std::wstring& p) { return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES; };
        auto count = [](const std::wstring& dir, const wchar_t* pat) { return prune::List(dir + L"\\", pat, false).size(); };
        bool cleared = true, kept = true;
        for (const wchar_t* k : kinds) {
          const std::wstring d = base + k;
          cleared = cleared && !exists(d + L"\\old_1.txt") && !exists(d + L"\\old_2.pgm");
          kept = kept && exists(d + L"\\keep\\inner.txt");
        }
        const size_t nm = count(base + L"cockpit_mask", L"cockpit_mask_*.pgm"),
                     nd = count(base + L"cockpit_depth", L"cockpit_depth_*.r16g16f"),
                     nv = count(base + L"cockpit_motion", L"cockpit_motion_*.r16g16f");
        printf("     R91 dumps: mask %zu, depth %zu, motion %zu file(s) written after the clear\n", nm, nd, nv);
        Check(cleared && kept && nm >= 1 && nd >= 1 && nv >= 1,
              "R91 (R90 7.3): a new mask / depth / motion dump request empties its folder (its files, not its "
              "subfolders) before its first file is written");
        cmask::ConfigureDepth(true, 0);
        cmask::ConfigureMotion(true, true, 0);
        cmask::Configure(true, 2, 15, 1.0, 0, 0, 0, std::wstring());
        cmask::g_dumpDir.clear();
        for (const wchar_t* k : kinds) {
          const std::wstring d = base + k;
          for (const prune::Entry& e2 : prune::List(d + L"\\keep\\", L"*", false)) DeleteFileW(e2.path.c_str());
          RemoveDirectoryW((d + L"\\keep").c_str());
          for (const prune::Entry& e2 : prune::List(d + L"\\", L"*", false)) DeleteFileW(e2.path.c_str());
          RemoveDirectoryW(d.c_str());
        }
        RemoveDirectoryW(base.c_str());
      }
      cmask::g_flightGate = false;
      flight = false;
      vpCut = 0;
    }
    Rel(rFence);
    rm.Close();
    rdp.Close();
    rmv.Close();
    blk = cmask::g_view.load();  // the module's views of this install (the ones from before were unmapped)
    dblk = cmask::g_dview.load();
    mblk = cmask::g_mview.load();
    Rel(rc4);
    Rel(rd5);
    Rel(rd1);
    Rel(rctx);
    Rel(rdev);
    ptiming::g_orig = execSave;
  }
  // Uninstall: the block goes inactive.
  Rel(shared4);
  Rel(shared);
  Rel(fence);
  Rel(d5);
  Rel(d1);
  cm::Header* keepBlk = nullptr;
  HANDLE reader = OpenFileMappingW(FILE_MAP_READ, FALSE, bname);
  if (reader) keepBlk = static_cast<cm::Header*>(MapViewOfFile(reader, FILE_MAP_READ, 0, 0, sizeof(cm::Header)));
  cmv::Header* keepM = nullptr;  // R70
  HANDLE mreader = OpenFileMappingW(FILE_MAP_READ, FALSE, mname);
  if (mreader) keepM = static_cast<cmv::Header*>(MapViewOfFile(mreader, FILE_MAP_READ, 0, 0, sizeof(cmv::Header)));
  cdp::Header* keepD = nullptr;  // R69
  HANDLE dreader = OpenFileMappingW(FILE_MAP_READ, FALSE, dname);
  if (dreader) keepD = static_cast<cdp::Header*>(MapViewOfFile(dreader, FILE_MAP_READ, 0, 0, sizeof(cdp::Header)));
  cm::Header hg0{};  // R76: the generation a reader may be opening right now
  const bool readG0 = keepBlk && cm::seqlock_read(keepBlk, hg0) && (hg0.flags & cm::header_flags::active);
  Uninstall(false);  // R85: the stop (fixation_depth.h Shutdown): the shared set released, not held
  {
    cm::Header hg1{};
    cdp::Header dg1{};
    cmv::Header mg1{};
    const bool rg = readG0 && cm::seqlock_read(keepBlk, hg1) && keepD && cdp::seqlock_read(keepD, dg1) && keepM &&
                    cmv::seqlock_read(keepM, mg1);
    size_t graves = 0;
    {
      std::lock_guard<std::mutex> lock(cmask::g_graveMutex);
      graves = cmask::g_grave.size();
    }
    const bool newGen = rg && hg1.generation != hg0.generation && hg1.generation != 0 &&
                        dg1.generation == hg1.generation && mg1.generation == hg1.generation && hg1.flags == 0;
    Check(newGen && graves > 0, "R76 cockpit mask: uninstall's last records carry a new generation (flags 0; a reader's "
                                "re-check after opening fails) and the old NT handles keep their 2 s grace");
    cmask::CloseBuried(true);
  }
  {
    cdp::Header hdu{};
    const bool rdu = keepD && cdp::seqlock_read(keepD, hdu);
    Check(rdu && !(hdu.flags & cdp::header_flags::active) && hdu.textureHandle[0] == 0 && hdu.fenceHandle == 0 &&
              !cmask::g_dview.load() && !cmask::g_r.haveDepth,
          "cockpit depth: uninstall leaves the depth block inactive (no handles), textures released");
    if (keepD) UnmapViewOfFile(keepD);
    if (dreader) CloseHandle(dreader);
    cmv::Header hmu{};  // R70
    const bool rmu = keepM && cmv::seqlock_read(keepM, hmu);
    Check(rmu && !(hmu.flags & cmv::header_flags::active) && hmu.textureHandle[0] == 0 && hmu.classTextureHandle[0] == 0 &&
              !cmask::g_mview.load() && !cmask::g_r.haveMotion && !fixdepth::g_runMv && !fixdepth::g_passMv,
          "motion: uninstall leaves the motion block inactive (no handles), textures and DCS's MV targets released");
    if (keepM) UnmapViewOfFile(keepM);
    if (mreader) CloseHandle(mreader);
  }
  cm::Header hu{};
  const bool readU = keepBlk && cm::seqlock_read(keepBlk, hu);
  Check(readU && !(hu.flags & cm::header_flags::active) && hu.textureHandle[0] == 0 && !cmask::g_view.load() &&
            !fdexport::g_maskUploads.load() && !cmask::g_r.dev,
        "cockpit mask: uninstall leaves the block inactive (no handles), the capture unarmed, our objects released");
  if (keepBlk) UnmapViewOfFile(keepBlk);
  if (reader) CloseHandle(reader);
  cmask::Configure(false, 2, 3, 1.0, 0);
  cmask::ConfigureDepth(false, 0);  // R69
  cmask::ConfigureMotion(false, true, 0);  // R70
  for (int i = 0; i < 4; ++i) {
    Rel(mvRtv[i]);
    Rel(mvTex[i]);
  }
  Rel(dcsU1);
  Rel(u1tex);
  cmask::g_xrHookWanted = true;
  cmask::g_flightGate = true;  // R91: the shipped defaults
  cmask::g_buildWaitMs = 0;
  g_verify = 0;
  shadowinst::g_device = oldDev;
  for (int i = 0; i < 4; ++i) {
    Rel(clean[i]);
    Rel(glassy[i]);
    Rel(v[i].dsv);
    Rel(v[i].tex);
    Rel(v[i].cb);
  }
  Rel(def.dsv);
  Rel(def.tex);
  Rel(dcsCs);
  Rel(cbA);
  Rel(c);
  Rel(imm);
  Rel(dev);
}

void Run() {
  printf("-- cockpit_mask.h --\n");
  TestR74Contract();  // R74a / R74b
  TestR74Pure();
  TestContract();
  TestDepthContract();  // R69
  TestMotionContract();  // R70
  TestPure();
  TestPipeline();
}

}  // namespace mask65test
