// R95: the HUD symbology emission export (Local\DcsVrSymbology_v1; src/symbology_block.h, src/symbology.h, the tone
// map's second evaluation in src/color_pass.h; docs/research/R95_symbology_export.md). Offline:
//  - the contract (vendored dcsvr_symbology.h, Dev's dcsvr_pupil_fov.h): layout, packing, seqlock / heartbeat, the
//    per-view alpha flag, Dev's 0049 re-composite rule against recomposite();
//  - the pure parts: boxes, poses, the tracking-space field (learn / project under head rotation), the window choice
//    (margin, clipping, overflow), the located-pose pick, Quad Views' settings parser, the pupil shift pick, the focus
//    layout, cockpit_time.h's per-view locate record;
//  - ovl_cpu.h's render point reporting a record's indicator type;
//  - the tone-map stage on WARP (the real color_pass.h draw: Mode 0 stock replica, Fix, Enhanced with glare, Enhanced
//    at night with night vision, GazeDof, GVision): dec(F) - E == dec(C) for every window texel (a test-only shader
//    variant writes C), E == 0 exactly where nothing of type 1 / 2 changed the HDR (MFD texels included), the
//    production shader's E equal to the test variant's, against an independent reference frame where the tone map is
//    local, the coverage alpha, DCS's bindings back;
//  - the whole path through fixation_depth.h's boundary callback on WARP (records of types 1 / 0 / 2 drawn into DCS's
//    HDR targets of four views, DCS's tone-map draw through our arm, the xrEndFrame commit with poses / FOVs): learning
//    by scan, windows predicted from the located pose under head yaw, E against a reference frame without the symbols,
//    the class plane, MFD cancellation, the alpha path, overflow, the focus layout with and without a pupil shift
//    block, Quad Views' settings, pairing with the mask, the heartbeat, the generation / held set across export
//    off / on, mask off / on, F10, relayout, capacity change, device removal, RetireUnmapped and the stop.
//  QV_R95_HW=1: the cost bench on the GPU (hardware device: copies, accumulation, scan, the tone map with and without
//  the second evaluation at the real view sizes).
#pragma once

#include "../tools/vrfx_rig.h"

namespace r95test {

using namespace fixdepth;
namespace csy = ::dcsvr_symbology;
namespace cm = ::dcsvr_cockpit_mask;
namespace pf = ::dcsvr_pupil_fov;
using fdtest::Rel;

inline double SrgbDec(double c) { return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4); }
inline double Dec8(uint8_t b) { return SrgbDec(b / 255.0); }
inline float H2F(uint16_t h) { return cgrade::HalfToFloat(h); }
inline uint16_t F2H(float f) { return vfxrig::ToHalf(f); }
inline float Rh(float f) { return H2F(F2H(f)); }  // as an RGBA16F texel stores f
// The tolerance of E stored as a half float: one ulp (D3D lets a float -> half store truncate; WARP does) plus the
// shader's pow.
inline double ETol(double e) { return std::fabs(e) * (1.0 / 1024.0) + 2e-6; }

// ---------------------------------------------------------------------------
// Contract
// ---------------------------------------------------------------------------
void TestContract() {
  using namespace csy;
  const uint32_t vf[] = {view_flags::valid, view_flags::present, view_flags::rejected, view_flags::untouched_hdr,
                         view_flags::own_tonemap, view_flags::pre_dlss_hud, view_flags::overflow,
                         view_flags::mfd_by_mask};
  uint32_t all = 0;
  bool distinct = true;
  for (uint32_t f : vf) distinct = distinct && !(all & f) && (f & (f - 1)) == 0, all |= f;
  Check(sizeof(Header) == 2048 && offsetof(Header, readerHeartbeatQpc) == 2040 && offsetof(Header, slot) == 184 &&
            sizeof(EmissionSlot) == 424 && sizeof(EmissionView) == 72 && distinct &&
            emission_flags::alpha_valid == 8 && contract_revision == 1 && block_version == 1,
        "R95 contract: the vendored v1 header as Dev copied it (2048-byte block, slots at 184, heartbeat 2040, flags; "
        "the coverage alpha's validity is emission_flags::alpha_valid), revision 1");
  // Packing: the default capacities fill the 2560 x 1024 atlas exactly, the windows never overlap (also the test's).
  bool packOk = true;
  for (const auto& caps : {std::pair<uint32_t, uint32_t>{512, 1024}, std::pair<uint32_t, uint32_t>{32, 64},
                           std::pair<uint32_t, uint32_t>{100, 300}}) {
    const cmask::sym::Pack p = cmask::sym::MakePack(caps.first, caps.second);
    uint64_t area = 0;
    bool overlap = false;
    for (int a = 0; a < 4; ++a) {
      area += uint64_t(p.cap[a]) * p.cap[a];
      packOk = packOk && p.x[a] + p.cap[a] <= p.W && p.y[a] + p.cap[a] <= p.H;
      for (int b = a + 1; b < 4; ++b)
        overlap = overlap || (p.x[a] < p.x[b] + p.cap[b] && p.x[b] < p.x[a] + p.cap[a] && p.y[a] < p.y[b] + p.cap[b] &&
                              p.y[b] < p.y[a] + p.cap[a]);
    }
    packOk = packOk && !overlap && (caps.first * 2 != caps.second || area == uint64_t(p.W) * p.H);
  }
  const cmask::sym::Pack d = cmask::sym::MakePack(default_capacity_periphery, default_capacity_focus);
  Check(packOk && d.W == default_atlas_w && d.H == default_atlas_h && d.x[1] == 0 && d.y[1] == 512 && d.x[2] == 512 &&
            d.x[3] == 1536,
        "R95 contract: the writer's packing is the contract's (v0 (0,0), v1 (0,512), v2 (512,0), v3 (1536,0), atlas "
        "2560 x 1024 without an unused texel); no two windows overlap for other capacities either");
  // Seqlock and heartbeat on the block.
  Header w{}, r{};
  w.magic = block_magic, w.version = 1, w.generation = 77, w.slot[2].seq = 9, w.slot[2].view[3].winW = 123;
  w.qv.sharpenFocusView = 0.7f, w.fovSource = fov_source_pupil_original;
  Header blk{};
  seqlock_write(&blk, w);
  reader_heartbeat(&blk, 1000);
  const bool rd = seqlock_read(&blk, r);
  Check(rd && r.generation == 77 && r.slot[2].view[3].winW == 123 && r.fovSource == 1 && r.qv.sharpenFocusView == 0.7f &&
            blk.readerHeartbeatQpc == 1000 && reader_alive(&blk, 1500, 1000) && !reader_alive(&blk, 4000, 1000),
        "R95 contract: seqlock copy (bytes 8..2039) and the reader's heartbeat at 2040 (fresh within 2 s)");
  // Dev's 0049 re-composite against recomposite(): with the flag pre = P - D, S = D + a pre, out = B (1 - a) + S; without
  // it out = B + D; at the real frame (B = pre) out = P.
  double worst = 0;
  uint32_t s = 12345;
  auto rnd = [&s] {
    s = s * 1664525u + 1013904223u;
    return (s >> 8) / 16777216.0f;
  };
  for (int i = 0; i < 1000; ++i) {
    const float a = rnd(), B = rnd() * 2, P = rnd() * 2, D = rnd() - 0.3f;
    const float pre = P - D, dev = B * (1 - a) + (D + a * pre);
    const Rgb rc = recomposite({B, B, B}, {D, D, D}, a, {pre, pre, pre});
    const Rgb r0 = recomposite({B, B, B}, {D, D, D}, 0, {pre, pre, pre});
    const Rgb at = recomposite({pre, pre, pre}, {D, D, D}, a, {pre, pre, pre});
    worst = std::max({worst, std::fabs(double(dev) - rc.r), std::fabs(double(B + D) - r0.g), std::fabs(double(P) - at.b)});
  }
  Check(worst < 1e-5, "R95 contract: Dev's 0049 rule (pre = P - D, S = D + a pre, B (1 - a) + S; additive B + D without "
                      "the view's alpha flag) equals recomposite(); at the real frame it gives P back");
  // Dev's pupil FOV header: vendored verbatim (its static_asserts), find() by display time while enabled.
  static pf::Block b{};
  b.magic = pf::magic_value, b.version = pf::version_value, b.size = sizeof(pf::Block), b.enabled = 1;
  b.entries[3].displayTime = 555;
  const bool found = pf::find(b, 555) == &b.entries[3] && !pf::find(b, 556) && !pf::find(b, 0);
  b.enabled = 0;
  Check(found && !pf::find(b, 555) && sizeof(pf::Block) == 4672,
        "R95 contract: dcsvr_pupil_fov.h (Dev's, byte-identical): 4672-byte block, find() by display time, nothing while "
        "disabled");
}

// ---------------------------------------------------------------------------
// Pure helpers
// ---------------------------------------------------------------------------
inline void QuatYawPitch(double yawDeg, double pitchDeg, double rollDeg, float q[4]) {
  // q = yaw (about +y) * pitch (about +x) * roll (about -z, the view axis)
  const double y = yawDeg * 0.5 * 3.14159265358979 / 180, p = pitchDeg * 0.5 * 3.14159265358979 / 180,
               r = rollDeg * 0.5 * 3.14159265358979 / 180;
  const double qy[4] = {0, std::sin(y), 0, std::cos(y)}, qp[4] = {std::sin(p), 0, 0, std::cos(p)},
               qr[4] = {0, 0, std::sin(r), std::cos(r)};
  auto mul = [](const double a[4], const double b[4], double o[4]) {
    o[0] = a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1];
    o[1] = a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0];
    o[2] = a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3];
    o[3] = a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2];
  };
  double t[4], o[4];
  mul(qy, qp, t);
  mul(t, qr, o);
  for (int k = 0; k < 4; ++k) q[k] = static_cast<float>(o[k]);
}

void TestPure() {
  using namespace cmask::sym;
  // Boxes and the statistics' encoding.
  const uint32_t w[4] = {0xffffffffu - 10, 0xffffffffu - 20, 31, 41};
  const Box b = DecodeBox(w), z = DecodeBox(std::array<uint32_t, 4>{}.data());
  const Box u = Union(b, Box{true, 5, 30, 12, 50}), c = Clip(Box{true, -5, -5, 300, 40}, 100, 30);
  Check(b.ok && b.x0 == 10 && b.y0 == 20 && b.x1 == 31 && b.y1 == 41 && !z.ok && u.x0 == 5 && u.y0 == 20 && u.x1 == 31 &&
            u.y1 == 50 && c.ok && c.x0 == 0 && c.y1 == 30 && c.x1 == 100 && !Clip(Box{true, 200, 0, 300, 5}, 100, 30).ok,
        "R95 pure: the statistics' box encoding (max of ~x0 / ~y0 / x1 / y1, 0 empty), union, clip");
  // Pose round trip: pixel -> tracking direction -> pixel, rotated head, asymmetric FOV.
  float q[4];
  QuatYawPitch(23, -7, 4, q);
  const float fov[4] = {-0.95f, 0.78f, 0.81f, -0.86f};
  const Pose p = PoseOf(q, fov);
  double worst = 0;
  for (int i = 0; i < 50; ++i) {
    const double px = 3 + i * 19.7, py = 7 + (i * 37) % 900;
    double d[3], x = 0, y = 0;
    const bool ok = PixelDir(p, 1040, 1028, px, py, d) && DirPixel(p, 1040, 1028, d, x, y);
    worst = std::max(worst, ok ? std::max(std::fabs(x - px), std::fabs(y - py)) : 1e9);
  }
  double back[3] = {0, 0, 1}, bx, by;
  QRot(q, std::array<double, 3>{0, 0, 1}.data(), back, false);
  Check(p.ok && worst < 1e-6 && !DirPixel(p, 1040, 1028, back, bx, by),
        "R95 pure: a view pixel's tracking-space direction and back (rotated head, asymmetric FOV: < 1e-6 px); a "
        "direction behind the view has no pixel");
  // The collimated field: learnt from a box at one pose, projected at another (head yaw 12 deg, pitch 3 deg, roll 5):
  // it contains the box's own reprojection there, and not much more.
  Cone cone;
  const Box hud{true, 420, 380, 640, 560};
  const bool learnt = LearnCone(cone, p, 1040, 1028, hud);
  const Box same = ProjectCone(cone, p, 1040, 1028);
  float q2[4];
  QuatYawPitch(23 + 12, -7 + 3, 4 + 5, q2);
  const Pose p2 = PoseOf(q2, fov);
  const Box moved = ProjectCone(cone, p2, 1040, 1028);
  double rx0 = 1e9, ry0 = 1e9, rx1 = -1e9, ry1 = -1e9;
  for (int i = 0; i <= 20; ++i)
    for (int j = 0; j <= 20; ++j) {
      double d[3], x, y;
      PixelDir(p, 1040, 1028, hud.x0 + (hud.x1 - hud.x0) * i / 20.0, hud.y0 + (hud.y1 - hud.y0) * j / 20.0, d);
      DirPixel(p2, 1040, 1028, d, x, y);
      rx0 = std::min(rx0, x), ry0 = std::min(ry0, y), rx1 = std::max(rx1, x), ry1 = std::max(ry1, y);
    }
  const bool contains = same.ok && same.x0 <= hud.x0 && same.y0 <= hud.y0 && same.x1 >= hud.x1 && same.y1 >= hud.y1 &&
                        moved.ok && moved.x0 <= rx0 + 0.5 && moved.y0 <= ry0 + 0.5 && moved.x1 >= rx1 - 0.5 &&
                        moved.y1 >= ry1 - 0.5;
  const double grow = double((moved.x1 - moved.x0) * (moved.y1 - moved.y0)) / ((rx1 - rx0) * (ry1 - ry0));
  printf("     (R95 field: box %dx%d at %d,%d; after yaw 12 / pitch 3 / roll 5: reprojected %.0fx%.0f at %.0f,%.0f, the "
         "field's box %dx%d at %d,%d (area x%.3f))\n",
         hud.x1 - hud.x0, hud.y1 - hud.y0, hud.x0, hud.y0, rx1 - rx0, ry1 - ry0, rx0, ry0, moved.x1 - moved.x0,
         moved.y1 - moved.y0, moved.x0, moved.y0, grow);
  Cone behind = cone;
  float qb[4];
  QuatYawPitch(23 + 180, -7, 4, qb);
  const Box none = ProjectCone(behind, PoseOf(qb, fov), 1040, 1028);
  Check(learnt && contains && grow < 1.35 && !none.ok,
        "R95 pure: the HUD's field in the tracking space projected with a rotated head covers the box's own "
        "reprojection (rotation-only at infinity), within 35 % more area (roll); turned away: nothing");
  // The window: margin 10 % + 4 px (+ extra), clipped, union with the helmet extent, overflow.
  const WinOut w1 = ChooseWindow(Box{true, 100, 200, 300, 260}, Box{}, 1040, 1028, 512, 512, 0);
  const WinOut w2 = ChooseWindow(Box{true, -50, 1000, 30, 1100}, Box{}, 1040, 1028, 512, 512, 0);
  const WinOut w3 = ChooseWindow(Box{true, 100, 100, 300, 200}, Box{true, 600, 150, 700, 400}, 1040, 1028, 512, 512, 0);
  const WinOut w4 = ChooseWindow(Box{true, 100, 100, 700, 200}, Box{}, 1040, 1028, 512, 512, 0);
  const WinOut w5 = ChooseWindow(Box{true, 100, 200, 300, 260}, Box{}, 1040, 1028, 512, 512, 8);
  const WinOut w6 = ChooseWindow(Box{}, Box{}, 1040, 1028, 512, 512, 0);
  Check(w1.any && !w1.overflow && w1.x == 100 - 14 && w1.w == 200 + 28 && w1.y == 200 - 7 && w1.h == 60 + 14 &&
            w2.any && w2.x == 0 && w2.y + w2.h == 1028 && w3.x == 100 - 34 && w3.x + w3.w == 700 + 34 && w4.overflow &&
            w4.any && w5.w == w1.w + 16 && !w6.any,
        "R95 pure: the window is the union grown by 5 % + 4 px per side (+ the learnt extra), clipped to the view; "
        "wider than the capacity -> overflow; nothing learnt -> none");
  // Stability: the size grows at once, shrinks after 30 frames below; an overflow ends after 10 fitting frames; the
  // alpha flag changes after 8 agreeing read-backs.
  {
    Hold h;
    WinOut c;
    c.any = true, c.x = 100, c.y = 100, c.w = 80, c.h = 60;
    WinOut o = Stabilize(h, c, 1040, 1028, 512);
    WinOut big = c;
    big.w = 120, big.x = 90;
    o = Stabilize(h, big, 1040, 1028, 512);
    const bool grow = o.w == 120 && o.x == 90;
    WinOut smallW = c;
    int held = 0;
    for (int i = 0; i < 29; ++i) held += Stabilize(h, smallW, 1040, 1028, 512).w == 120;
    const WinOut s30 = Stabilize(h, smallW, 1040, 1028, 512);
    const bool centred = held == 29 && s30.w == 80;
    WinOut edge = c;
    edge.x = 1000, edge.w = 40;
    Hold h2;
    h2.w = 200;
    const WinOut e2 = Stabilize(h2, edge, 1040, 1028, 512);
    WinOut ov;
    ov.overflow = true;
    Hold h3;
    Stabilize(h3, ov, 1040, 1028, 512);
    int stillOver = 0;
    for (int i = 0; i < 9; ++i) stillOver += Stabilize(h3, c, 1040, 1028, 512).overflow;
    const WinOut after = Stabilize(h3, c, 1040, 1028, 512);
    // R97: the alpha flag: on after g_alphaOnRun valid votes in a row (a neutral one restarts the run), off at the first
    // invalid one and latched off until AlphaReset (a new generation).
    const int keepRun = g_alphaOnRun.load();
    g_alphaOnRun = 8;
    AlphaHold a;
    int on = 0;
    for (int i = 0; i < 4; ++i) AlphaVote(a, 1), on += a.on;
    AlphaVote(a, 0);  // a neutral read-back (incomplete / moving): the run restarts
    for (int i = 0; i < 7; ++i) AlphaVote(a, 1), on += a.on;
    AlphaVote(a, 1);
    const bool aOn = on == 0 && a.on;
    AlphaVote(a, 0), AlphaVote(a, 1);  // neutral and valid votes while on: on
    const bool aStill = a.on;
    AlphaVote(a, -1);
    int back = 0;
    for (int i = 0; i < 40; ++i) AlphaVote(a, 1), back += a.on;
    const bool latched = !a.on && a.latched && back == 0;
    AlphaReset(a);
    for (int i = 0; i < 8; ++i) AlphaVote(a, 1);
    const bool newGen = a.on && !a.latched && a.flips == 3;
    g_alphaOnRun = keepRun;
    Check(grow && centred && e2.w == 200 && e2.x + e2.w == 1040 && stillOver == 9 && !after.overflow && after.any &&
              aOn && aStill && latched && newGen,
          "R95 pure: stability for Dev's pairs: the window grows at once and shrinks after 30 frames below (centred, "
          "inside the view); an overflow lasts until 10 fitting frames; R97: the alpha flag comes on after its run of "
          "valid votes (a neutral one restarts it), goes off at the first invalid one and stays off (latched) until a "
          "new generation");
  }
  // The located pose of the frame being rendered: the smallest displayTime after the last committed one, made before
  // now, with its views; the latest call of that time.
  cmask::ctime::Rec r[6];
  auto set = [](cmask::ctime::Rec& x, int64_t q0, int64_t dt, uint32_t vn) {
    x.q0 = q0, x.dt = dt, x.vn = vn;
  };
  set(r[0], 100, 7000, 4), set(r[1], 200, 7011, 4), set(r[2], 250, 7011, 4), set(r[3], 300, 7022, 4),
      set(r[4], 900, 7005, 4), set(r[5], 50, 7008, 0);
  Check(PickPredict(r, 6, 7000, 500) == 2 && PickPredict(r, 6, 7011, 500) == 3 && PickPredict(r, 6, 7022, 500) == -1 &&
            PickPredict(r, 6, 6000, 500) == 0 && PickPredict(r, 6, 7000, 1000) == 4,
        "R95 pure: the window's pose is the located call of the next display time after the last committed frame "
        "(the latest call of it, made before now, with its views)");
  // Quad Views' settings: QV's statement rules, its clamps, its file order.
  QvVals v;
  QvParseText("# comment\r\n// another\r\nsharpen_focus_view=0.4\r\n[exe:DCS]\r\nsmoothen_focus_view_edges=0.9\r\n"
              "focus_view_shape=2.5\r\n[Pimax Crystal]\r\nsharpen_focus_view=0.1\r\n[app:DCS]\r\ndcsvr_far_depth=1\r\n"
              "dcsvr_periphery_sharpen=x\r\ndebug_keys=1\r\n[exe:Other]\r\ndcsvr_temporal=1\r\n",
              "DCS", "DCS.exe", v);
  const QvVals v1 = v;
  QvParseText("[exe:DCS]\nsharpen_focus_view=7\ndcsvr_sharpen_taper=1\ndebug_keys=0\n", "DCS", "DCS.exe", v);
  const csy::QvSettings qs = QvSettingsOf(v, true);
  Check(v1.sharpen == 0.4f && v1.smooth == 0.5f && v1.shape == 2.5f && v1.farDepth == 1 && v1.periSharpen == 0.0f &&
            v1.temporal == 0 && v1.debugKeys == 1 && v.sharpen == 1.0f && v.taper == 1 && v.debugKeys == 0 &&
            qs.flags == (csy::qv_flags::far_depth_warp | csy::qv_flags::sharpen_taper | csy::qv_flags::profile_file) &&
            qs.smoothenFocusViewEdges == 0.5f && QvVals{}.sharpen == 0.7f && QvVals{}.smooth == 0.2f,
        "R95 pure: Quad Views' settings as QV parses them (comments, [exe:] / [app:] sections by name, runtime sections "
        "skipped, clamps, a bad value skipped, later files override); defaults 0.7 / 0.2 / 0");
  // The pupil shift layer's originals: the entry of this display time whose shifted FOVs are DCS's submitted ones.
  static pf::Block pb{};
  pb.magic = pf::magic_value, pb.version = pf::version_value, pb.size = sizeof(pf::Block), pb.enabled = 1;
  float sub[4][4], orig[4][4], got[4][4] = {};
  for (int i = 0; i < 4; ++i)
    for (int k = 0; k < 4; ++k) sub[i][k] = 0.1f * (k + 1) * (k & 1 ? 1 : -1) + 0.01f * i, orig[i][k] = sub[i][k] * 1.02f;
  pf::Entry& e = pb.entries[5];
  e.displayTime = 9000, e.viewCount = 4, e.flags = pf::entry_flags::pupil;
  for (int i = 0; i < 4; ++i) memcpy(e.views[i].shiftedFov, sub[i], 16), memcpy(e.views[i].originalFov, orig[i], 16);
  const bool use = PupilOriginal(pb, 9000, sub, 4, got) && !memcmp(got, orig, sizeof(got));
  const bool otherTime = !PupilOriginal(pb, 9001, sub, 4, got);
  float sub2[4][4];
  memcpy(sub2, sub, sizeof(sub2));
  sub2[2][1] += 1e-3f;
  const bool otherFov = !PupilOriginal(pb, 9000, sub2, 4, got);
  e.viewCount = 2;
  const bool fewer = !PupilOriginal(pb, 9000, sub, 4, got) && PupilOriginal(pb, 9000, sub, 2, got);
  Check(use && otherTime && otherFov && fewer,
        "R95 pure: the pupil shift layer's original FOVs for the frame's display time when its shifted FOVs are what "
        "DCS submitted (the submitted flag is not required: our commit runs before the layer's xrEndFrame)");
  // The focus layout from the four views' tangents.
  float tan[4][4] = {{-1.2f, 1.0f, 1.1f, -1.1f}, {-1.0f, 1.2f, 1.1f, -1.1f}, {-0.3f, 0.2f, 0.25f, -0.3f}, {-0.1f, 0.4f, 0.3f, -0.2f}};
  csy::FocusLayout L;
  const bool lok = MakeLayout(tan, L);
  float want[4];
  csy::focus_rect(tan[1], tan[3], want);
  Check(lok && (L.flags & csy::layout_flags::layout_valid) && !memcmp(L.eye[1].rectNorm, want, 16) &&
            std::fabs(L.eye[0].rectNorm[0] - (-0.3f + 1.2f) / 2.2f) < 1e-6 && !memcmp(L.eye[0].focusFovTan, tan[2], 16),
        "R95 pure: the focus layout per eye (rectNorm = focus_rect(eye, focus) in the published FOV source)");
  // cockpit_time.h: every located view's orientation and FOV (XrView: orientation at 16, FOV at 44).
  alignas(8) uint8_t views[64 * 4] = {};
  for (int i = 0; i < 4; ++i) {
    *reinterpret_cast<int32_t*>(views + 64 * i) = 7;
    const float qq[4] = {0.01f * i, 0.2f, 0, 0.98f}, ff[4] = {-0.5f - i, 0.4f, 0.6f, -0.7f};
    memcpy(views + 64 * i + 16, qq, 16);
    memcpy(views + 64 * i + 44, ff, 16);
  }
  uint32_t cnt = 4;
  cmask::ctime::Rec rec;
  float q0[4];
  const bool rv = cmask::ctime::ReadView0Seh(&cnt, views, 4, q0, &rec);
  Check(rv && rec.vn == 4 && rec.vq[3][0] == 0.03f && rec.vfov[2][0] == -2.5f && q0[1] == 0.2f,
        "R95 cockpit_time: the xrLocateViews record keeps every view's orientation and FOV (the window's pose)");
}

// ---------------------------------------------------------------------------
// ovl_cpu.h's render point: a record's type reported before DCS draws it, only for the views asked, on the render
// thread, inside the overlay HDR pass.
// ---------------------------------------------------------------------------
int g_glueView = -9, g_glueC9 = -9, g_glueCalls = 0, g_renderCalls = 0;
void GlueRec(int v, int c9) {
  g_glueView = v, g_glueC9 = c9, ++g_glueCalls;
}
void GlueRender(void*, uint32_t) { ++g_renderCalls; }
void TestOvlGlue() {
  namespace oc = ckipd::ocpu;
  const auto keepRender = oc::g_renderOrig;
  const void* keepRet = oc::g_renderRet;
  const auto keepRec = oc::g_symRec;
  const DWORD keepTid = ptiming::g_topTid.load();
  oc::g_renderOrig = &GlueRender;
  oc::g_renderRet = nullptr;
  oc::g_symRec = &GlueRec;
  ptiming::g_topTid = GetCurrentThreadId();
  alignas(16) uint8_t record[0xd0] = {};
  record[oc::kRecC9] = 2;
  record[oc::kRecC8] = 21;
  alignas(16) uint8_t ro[0x100] = {};
  const uint8_t* recp = record + oc::kRecFromRo;
  memcpy(ro + oc::kRoRec, &recp, sizeof(recp));
  oc::t_ovlOpen = true, oc::t_ovlLdr = false, oc::t_ovlView = 3;
  oc::g_symViews = 1u << 3;
  oc::RenderHook(ro, 0);
  const bool got = g_glueCalls == 1 && g_glueView == 3 && g_glueC9 == 2 && g_renderCalls == 1;
  oc::g_symViews = 1u << 2;  // another view's bit
  oc::RenderHook(ro, 0);
  oc::g_symViews = 1u << 3;
  oc::t_ovlLdr = true;  // OverlayLDRPass: not reported
  oc::RenderHook(ro, 0);
  oc::t_ovlLdr = false;
  ptiming::g_topTid = GetCurrentThreadId() + 1;  // another thread
  oc::RenderHook(ro, 0);
  ptiming::g_topTid = GetCurrentThreadId();
  memset(ro + oc::kRoRec, 0xff, 8);  // an unreadable record pointer: the SEH latch
  const uint64_t f0 = oc::g_symFaults.load();
  oc::RenderHook(ro, 0);
  const bool fault = oc::g_symFaults.load() == f0 + 1 && oc::g_symViews.load() == 0 && g_renderCalls == 5;
  Check(got && g_glueCalls == 1 && fault,
        "R95 ovl_cpu render point: each record's indicator type (+0xc9) reported before DCS's render call, only in the "
        "asked views' OverlayHDRPass on the render thread; an unreadable record latches the reporting off; DCS's call "
        "always made");
  // R115: the fault latches the export (sym::g_fault, read by sym::Ready()) beyond this frame: the next frame's
  // PassBegin sets no view bit (R95 only cleared this frame's bits); the next install clears it (NoteInstall).
  const bool latched = cmask::sym::g_fault.load();
  cmask::sym::FrameBegin();
  cmask::sym::PassBegin(3);
  const bool noBits = oc::g_symViews.load() == 0 && !cmask::sym::Ready();
  cmask::sym::NoteInstall();
  const bool cleared = !cmask::sym::g_fault.load();
  Check(latched && noBits && cleared,
        "R115 symbology fault latch: a fault in the record render point sets sym::g_fault (export not Ready, no view "
        "bit at the next PassBegin) until the next install clears it");
  cmask::sym::g_fault = false;
  oc::t_ovlOpen = false, oc::t_ovlView = -1;
  oc::g_symViews = 0;
  oc::g_renderOrig = keepRender;
  oc::g_renderRet = keepRet;
  oc::g_symRec = keepRec;
  ptiming::g_topTid = keepTid;
}

}  // namespace r95test
