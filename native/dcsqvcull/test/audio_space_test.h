// Cockpit space, the audio module's side (docs/COCKPIT_SPACE.md 8.2-8.6, tests M1-M15 of section 13) on the fake
// ed_sound image with the recording fake Room (audio_fake_core.cpp): aircraft families and unknown aircraft, the canopy,
// inside / outside from ed_sound's use-cockpit-gain flag (and the camera fallback), outside voices heard through the
// canopy against cockpit voices, the radio untouched, the kill switch, Off exact, the aircraft frame and its late
// rotation, the optional signatures, the budget's first step, the Lua bridge, the log lines and CSV columns.
// Included inside namespace audiotest by audio_test.h (its rig helpers and kIniOn).
#pragma once

namespace spacetest {

const char* kIniSpace = "[Audio]\r\nBinaural=1\r\nHrtf=default\r\nMaxObjects=16\r\nLateHeadTracking=0\r\nCockpitSpace=1\r\n";
constexpr int kCockpitCtx = 2, kWorldCtx = 3, kRadioCtx = 7;

void SetCtxCockpitGain(Rig& r, int ctx, bool on) { W<uint8_t>(r.im.base + kCtxArray + 0x388 * ctx, 0xe1, on ? 1 : 0); }

// Two cockpit voices (context 2, under 1.5 m), one outside voice (context 3, 20 m, its host allows cockpit gain, DCS
// damps it to 0.5) and the radio (context 7, 2D).
// third: a third cockpit voice (index 4, 0.5 m), for the frame's cluster.
void SpaceRig(Rig& r, const char* ini = kIniSpace, bool third = false) {
  RigUp(r, ini);
  SetContext2D(r, kRadioCtx, true);
  AddBoth(r, kCockpitCtx, true, 1, kSine, 0.4f, 440, 1.0f, 3);
  AddBoth(r, kCockpitCtx, true, 1, kNoise, 0.3f, 0, 0.8f, 5);
  AddBoth(r, kWorldCtx, true, 1, kSine, 0.3f, 97, 0.9f);
  AddBoth(r, kRadioCtx, true, 1, kSine, 0.3f, 1200, 1.0f);
  PosBoth(r, 0, 0.4, 0.3, -0.2);
  PosBoth(r, 1, 0.5, -0.2, 0.3);
  PosBoth(r, 2, 20, 0, 5);
  PosBoth(r, 3, 1, 0, 0);
  r.w.SetCockpitGain(2, true, 0.5f);
  r.ref.SetCockpitGain(2, true, 0.5f);
  if (third) {
    AddBoth(r, kCockpitCtx, true, 1, kSine, 0.2f, 660, 0.9f, 7);
    PosBoth(r, 4, 0.3, 0.1, 0.4);
  }
  for (int i = 0; i < (third ? 5 : 4); ++i) CreateVoice(r, i);
  Bind(r);
}

void Frames(Rig& r, int n, uint32_t frames = 480) {
  std::vector<float> a(2 * frames), b(2 * frames);
  for (int i = 0; i < n; ++i) {
    SimFrame(r);
    Block(r, a.data(), b.data(), frames);
  }
}

int SlotOf(const Rig& r, int i) {
  for (int s = 0; s < audv::kSlots; ++s)
    if (audio::g_meta.voice[s].load() == r.w.voices[i]) return s;
  return -1;
}

void Families() {
  StartReportCapture();
  Rig r;
  SpaceRig(r);
  audio::LuaAircraft("F-16C_50");
  Tick();
  const bool f16 = audio::g_spaceSt.family == cockpit_space::Family::Bubble && audiofake::g_roomLastParams.strength == 1 &&
                   audiofake::g_roomLastParams.family == cockpit_space::Family::Bubble;
  audio::LuaAircraft("Su-57_Felon_X");  // not in the table: generic, never a failure
  Tick();
  const bool unknown = audio::g_spaceSt.family == cockpit_space::Family::Generic &&
                       audiofake::g_roomLastParams.family == cockpit_space::Family::Generic;
  audio::LuaAircraft("UH-1H");
  Tick();
  const bool heli = audiofake::g_roomLastParams.family == cockpit_space::Family::Helicopter;
  audio::LuaAircraft("");
  Tick();
  WriteIni(r.dir, "[Audio]\r\nBinaural=1\r\nLateHeadTracking=0\r\nCockpitSpace=3\r\nCockpitSpaceFamily=warbird\r\n");
  audio::LuaAircraft("F-16C_50");
  Tick();
  const bool setting = audiofake::g_roomLastParams.family == cockpit_space::Family::Warbird &&
                       audiofake::g_roomLastParams.strength == 3;
  WriteIni(r.dir, "[Audio]\r\nBinaural=1\r\nLateHeadTracking=0\r\nCockpitSpace=7\r\nCockpitSpaceFamily=zeppelin\r\n");
  Tick();
  const bool clamped = audio::g_cfgRead.space.level == 3 && audio::g_cfgRead.space.familyAuto &&
                       audiofake::g_roomLastParams.family == cockpit_space::Family::Bubble;
  const std::string log = StopReportCapture();
  Check(f16 && log.find("audio: cockpit space: Subtle (bubble auto, aircraft F-16C_50)") != std::string::npos,
        "cockpit space: the aircraft's type picks its family (F-16C_50: bubble canopy), Subtle by default, preset sent to "
        "the room");
  Check(unknown && heli && log.find("Subtle (generic auto, aircraft Su-57_Felon_X)") != std::string::npos &&
            log.find("Subtle (generic auto, aircraft unknown)") != std::string::npos,
        "cockpit space: an aircraft not in the table and an unknown one fall back to the generic family; a helicopter "
        "gets the helicopter cabin");
  Check(setting && log.find("Strong (warbird setting, aircraft F-16C_50)") != std::string::npos,
        "cockpit space: CockpitSpaceFamily overrides the table, CockpitSpace=3 is Strong (Live)");
  Check(clamped && log.find("CockpitSpace=7 out of range, 3") != std::string::npos &&
            log.find("CockpitSpaceFamily is not a cockpit type (8 characters), auto") != std::string::npos,
        "cockpit space config: an out-of-range level is clamped and an unknown type is automatic, each said once");
  RigDown(r);
}

void InsideOutside() {
  StartReportCapture();
  Rig r;
  SpaceRig(r);
  // Nothing reported yet by DCS (the camera check unknown): the cockpit.
  Frames(r, 40);
  Tick();
  const bool unknownInside = audiofake::g_roomLastFrame.wet == 1.0f;
  // DCS's world context uses cockpit gain: inside, known from DCS.
  SetCtxCockpitGain(r, kWorldCtx, true);
  Frames(r, 5);
  // The camera check says outside while the flag says inside: outside (the flag's drop in external views is not
  // proven live yet), then the camera back in the cockpit: inside again.
  audio::LuaState(-1, 0, 0);
  Tick();
  Frames(r, 40);
  const bool cameraOut = audiofake::g_roomLastFrame.wet == 0.0f;
  audio::LuaState(-1, 0, 1);
  Tick();
  Frames(r, 40);
  const bool dcsWins = audiofake::g_roomLastFrame.wet == 1.0f;
  // F2: DCS clears the flag: the space fades out in 0.3 s (linear, small steps), then the room idles (no call at all).
  SetCtxCockpitGain(r, kWorldCtx, false);
  float last = 1.0f, worstStep = 0.0f;
  int blocks = 0;
  std::vector<float> a(960), b(960);
  for (; blocks < 60 && audiofake::g_roomLastFrame.wet > 0.0f; ++blocks) {
    SimFrame(r);
    Block(r, a.data(), b.data(), 480);
    worstStep = std::max(worstStep, last - audiofake::g_roomLastFrame.wet);
    last = audiofake::g_roomLastFrame.wet;
  }
  Frames(r, 5);
  const uint64_t begins = audiofake::g_roomBegins.load();
  Frames(r, 20);
  const bool idle = audiofake::g_roomBegins.load() == begins;
  Tick();
  const bool insideLogged = audio::g_spaceSt.insideNow == 0;
  SetCtxCockpitGain(r, kWorldCtx, true);
  Frames(r, 40);
  const bool back = audiofake::g_roomLastFrame.wet == 1.0f;
  Tick();
  const std::string log = StopReportCapture();
  printf("     inside/outside: out in %d blocks (worst step %.4f), idle %d\n", blocks, worstStep, idle ? 1 : 0);
  Check(unknownInside, "cockpit space: with no signal yet the listener counts as in the cockpit (wet 1)");
  Check(cameraOut && dcsWins, "cockpit space: DCS's use-cockpit-gain flag and the camera check must both say inside (a camera "
                              "outside wins until the flag is verified live)");
  Check(log.find("audio: cockpit space: inside signals: ed_sound inside, camera outside") != std::string::npos &&
            log.find("audio: cockpit space: inside signals: ed_sound inside, camera inside") != std::string::npos,
        "cockpit space log: both inside signals are logged on change");
  Check(blocks >= 29 && blocks <= 31 && worstStep < 0.034f && idle && insideLogged && back,
        "cockpit space: an external view fades the space out over 0.3 s in steps of one block, the room then costs no "
        "call at all; back in the cockpit it fades in again");
  Check(log.find("audio: cockpit space: inside 0, canopy unknown (none)") != std::string::npos,
        "cockpit space log: inside / canopy line on change");
  RigDown(r);
  // The camera check decides while DCS never reported the flag (its fields' code not found, or not set yet).
  Rig q;
  SpaceRig(q);
  audio::LuaState(-1, 0, 0);
  Tick();
  Frames(q, 40);
  Check(audiofake::g_roomLastFrame.wet == 0.0f || audiofake::g_roomBegins.load() == 0,
        "cockpit space: without DCS's flag the Lua bridge's camera check (external view) keeps the space out");
  RigDown(q);
}

void Canopy() {
  StartReportCapture();
  Rig r;
  SpaceRig(r);
  audio::LuaState(0.0, 1, 1);
  Tick();
  Frames(r, 40);
  const bool closed = audiofake::g_roomLastFrame.canopyOpen == 0.0f;
  audio::LuaState(1.0, 1, 1);
  Tick();
  // The room gets the canopy as it is and smooths it once itself (native/binaural C15); d takes the room's value.
  Frames(r, 1);
  const bool raw = audiofake::g_roomLastFrame.canopyOpen == 1.0f && audiofake::g_roomLastDirectCanopy == 1.0f;
  Frames(r, 149);
  const bool open = audiofake::g_roomLastFrame.canopyOpen == 1.0f;
  WriteIni(r.dir, "[Audio]\r\nBinaural=1\r\nLateHeadTracking=0\r\nCockpitSpaceCanopy=0\r\n");
  Tick();
  Frames(r, 150);
  const bool ignored = audiofake::g_roomLastFrame.canopyOpen < 0.01f;
  WriteIni(r.dir, "[Audio]\r\nBinaural=1\r\nLateHeadTracking=0\r\n");
  audio::LuaState(0.5, 2, 1);
  Tick();
  audio::LuaState(-1, 0, 1);  // multiplayer without ownship exports: unknown, treated as closed
  Tick();
  Frames(r, 150);
  const bool unknownClosed = audiofake::g_roomLastFrame.canopyOpen < 0.01f;
  const std::string log = StopReportCapture();
  Check(closed && raw && open,
        "cockpit space: the canopy reaches the room as the Lua bridge has it (the room smooths it once) and d uses "
        "the room's canopy");
  Check(ignored, "cockpit space: CockpitSpaceCanopy=0 keeps the space closed whatever the canopy does (Live)");
  Check(unknownClosed && log.find("canopy 1.00 (mech)") != std::string::npos &&
            log.find("canopy 0.50 (arg38)") != std::string::npos && log.find("canopy unknown (none)") != std::string::npos,
        "cockpit space: an unknown canopy counts as closed; the log names the canopy's source (mech, arg38, none)");
  RigDown(r);
}

void ThroughAndRadio() {
  Rig r;
  SpaceRig(r);
  SetCtxCockpitGain(r, kWorldCtx, true);
  const double q[4] = {0, 0.25881904510252074, 0, 0.9659258262890683};  // 30 deg about y
  for (int i = 0; i < 2; ++i) r.w.SetOrient(i, q);
  Frames(r, 12);
  const int sC = SlotOf(r, 0), sW = SlotOf(r, 2), sR = SlotOf(r, 3);
  audv::MetaData mc{}, mw{};
  const bool okC = sC >= 0 && audv::MetaRead(audio::g_meta.slot[sC], &mc);
  const bool okW = sW >= 0 && audv::MetaRead(audio::g_meta.slot[sW], &mw);
  const bool throughMeta = okW && (mw.flags & audv::meta_flags::kThroughCanopy) && mw.cockpitGain == 0.5f &&
                           !(mw.flags & audv::meta_flags::kCockpitCtx);
  const bool cockpitMeta = okC && (mc.flags & audv::meta_flags::kCockpitCtx) && !(mc.flags & audv::meta_flags::kThroughCanopy) &&
                           mc.cockpitGain == 1.0f;
  const bool throughRoom = sW >= 0 && (audiofake::g_roomSlot[sW].flags & binaural::room_flags::kThroughCanopy) &&
                           audiofake::g_roomSlot[sW].send > 0.0f;
  const bool cockpitRoom = sC >= 0 && !(audiofake::g_roomSlot[sC].flags & binaural::room_flags::kThroughCanopy) &&
                           audiofake::g_roomSlot[sC].adds > 0;
  // The radio (a 2D context) is never owned: no slot, no send; three voices are sent per block, never four.
  const uint64_t adds0 = audiofake::g_roomAdds.load();
  Frames(r, 10);
  const uint64_t perBlock = (audiofake::g_roomAdds.load() - adds0) / 10;
  Check(throughMeta && throughRoom,
        "cockpit space: an outside voice whose host allows cockpit gain is heard through the canopy (flag to the room; "
        "DCS's damping 0.5 kept in its metadata)");
  Check(cockpitMeta && cockpitRoom,
        "cockpit space: the cockpit context's voices are voted as cockpit voices and sent without the through flag");
  Check(sR < 0 && perBlock == 3 && audio::g_cnt.ownedNow.load() == 3,
        "cockpit space: the radio (2D) is untouched: never owned, never sent to the room");
  // With the canopy fields' code not matched, no voice is through the canopy (and no inside flag is read).
  audio::g_res.spaceGain = false;
  Frames(r, 4);
  audv::MetaData m2{};
  const bool off = sW >= 0 && audv::MetaRead(audio::g_meta.slot[sW], &m2) && !(m2.flags & audv::meta_flags::kThroughCanopy) &&
                   !(audiofake::g_roomSlot[sW].flags & binaural::room_flags::kThroughCanopy);
  Check(off, "cockpit space: without the matched canopy code no voice is classed through the canopy");
  RigDown(r);
}

void KillSwitch() {
  Rig r;
  SpaceRig(r);
  Frames(r, 40);
  const int s = SlotOf(r, 0);
  const bool sending = s >= 0 && audiofake::g_roomSlot[s].w1 == 1.0f && audiofake::g_roomLastFrame.wet == 1.0f;
  audio::g_userOff = true;
  Frames(r, 1);
  const bool fadeOut = s >= 0 && audiofake::g_roomSlot[s].w0 == 1.0f && audiofake::g_roomSlot[s].w1 == 0.0f;
  const uint64_t weighted = audiofake::g_roomAddsWeighted.load();
  Frames(r, 45);
  const uint64_t begins = audiofake::g_roomBegins.load();
  Frames(r, 10);
  const bool quiet = audiofake::g_roomAddsWeighted.load() == weighted && audiofake::g_roomBegins.load() == begins;
  audio::g_userOff = false;
  Frames(r, 2);
  const bool back = s >= 0 && audiofake::g_roomSlot[s].w1 > 0.0f && audiofake::g_roomBegins.load() > begins;
  Check(sending && fadeOut && quiet && back,
        "cockpit space: the kill switch reaches the room through the voices' weights (1 -> 0 in one block, as the "
        "direct path), the space fades out and idles; on again it comes back");
  RigDown(r);
}

// M1: CockpitSpace=0 is exactly the module without a room (the fake adding its output would show otherwise).
void OffExact() {
  std::vector<float> outA, outB;
  for (int pass = 0; pass < 2; ++pass) {
    Rig r;
    SpaceRig(r, pass == 0 ? "[Audio]\r\nBinaural=1\r\nMaxObjects=16\r\nLateHeadTracking=0\r\nCockpitSpace=0\r\n" : kIniSpace);
    audiofake::g_roomOutput = pass == 0;
    if (pass == 1) {  // the same module with no room at all (a build without it)
      audr::Binding* b = audio::g_binding.load();
      audr::RoomDelete(b->room);
      b->room = nullptr;
    }
    std::vector<float>& out = pass == 0 ? outA : outB;
    std::vector<float> a(960), bb(960);
    for (int i = 0; i < 200; ++i) {
      if (i % 2 == 0) {
        PosBoth(r, 0, 0.4 * std::cos(i * 0.05), 0.3, 0.4 * std::sin(i * 0.05));
        SimFrame(r);
      }
      Block(r, a.data(), bb.data(), 480);
      out.insert(out.end(), a.begin(), a.end());
    }
    if (pass == 0)
      Check(audiofake::g_roomBegins.load() == 0 && audiofake::g_roomAdds.load() == 0 && audiofake::g_roomRenders.load() == 0,
            "cockpit space Off: the room is never called");
    RigDown(r);
  }
  Check(outA.size() == outB.size() && memcmp(outA.data(), outB.data(), sizeof(float) * outA.size()) == 0,
        "cockpit space Off: the output is bit-identical to the module without a room");
}

// M4 / M5: the aircraft frame from the cockpit sounds' listener-frame orientation, its checks, and its late rotation.
void Frame() {
  Rig r;
  SpaceRig(r);
  const double q[4] = {0, 0.25881904510252074, 0, 0.9659258262890683};  // 30 deg about y (up)
  for (int i = 0; i < 2; ++i) r.w.SetOrient(i, q);
  Frames(r, 6);
  audspace::SpaceData sd{};
  audspace::SpaceRead(audio::g_space, &sd);
  const binaural::RoomFrame f = audiofake::g_roomLastFrame;
  const bool got = (sd.flags & audspace::flags::kFrameValid) && sd.cockpitCtx == kCockpitCtx && f.frameValid &&
                   audiofake::g_roomLastEarlyMix == 1.0f &&
                   std::fabs(f.aircraftToHead[1] - static_cast<float>(q[1])) < 1e-6f &&
                   std::fabs(f.aircraftToHead[3] - static_cast<float>(q[3])) < 1e-6f;
  // The voice's aircraft-frame position: conj(C) applied to its head-frame position.
  const int s = SlotOf(r, 0);
  const float ci[4] = {0, -static_cast<float>(q[1]), 0, static_cast<float>(q[3])}, pos[3] = {0.4f, 0.3f, -0.2f};
  float want[3];
  audspace::QRotate(ci, pos, want);
  const bool posA = s >= 0 && std::fabs(audiofake::g_roomSlot[s].pos[0] - want[0]) < 1e-5f &&
                    std::fabs(audiofake::g_roomSlot[s].pos[2] - want[2]) < 1e-5f;
  // A second cockpit sound 5 deg off: unreliable, the last good frame kept.
  const double q5[4] = {0, 0.29237170472273677, 0, 0.9563047559630354};  // 34 deg
  r.w.SetOrient(1, q5);
  Frames(r, 3);
  audspace::SpaceRead(audio::g_space, &sd);
  const bool spread = sd.unreliable >= 2 && sd.spreadDeg > 3.0f && std::fabs(sd.c[1] - static_cast<float>(q[1])) < 1e-6f;
  // No cockpit sound carries a frame for over 1 s (and no head pose): no frame, the early reflections fade.
  const double zero[4] = {0, 0, 0, 0};
  for (int i = 0; i < 2; ++i) r.w.SetOrient(i, zero);
  Frames(r, 2);
  Sleep(1100);
  Frames(r, 2);
  const bool lost = !audiofake::g_roomLastFrame.frameValid && audiofake::g_roomLastEarlyMix == 0.0f;
  Check(got && posA, "cockpit space: the aircraft frame comes from the cockpit context's sounds (sound+0xe8); the room "
                     "gets each voice in the aircraft frame");
  Check(spread, "cockpit space: cockpit sounds disagreeing by more than 1 deg make a frame unreliable (the last good one kept)");
  Check(lost, "cockpit space: no cockpit sound for 1 s and no head pose: no frame (the early reflections fade out, and d "
              "no longer makes room for them)");
  RigDown(r);
  // Late rotation (pure): C_dcs identity, the head turned 30 deg left since DCS's frame: the aircraft's forward axis is
  // seen 30 deg to the right; another base space: C_dcs as it is.
  audr::Binding b;
  b.rate = 48000;
  b.room = audr::RoomNew();
  char err[64];
  audr::RoomInit(b.room, 48000, 64, err, sizeof(err));
  audr::Block blk;
  blk.b = &b;
  blk.frames = 480;
  blk.on = true;
  blk.late = true;
  blk.pose.valid = true;
  blk.pose.base = 7;
  blk.pose.q = dcsvr_headpose::AxisAngle({0, 1, 0}, 30.0 * 3.14159265358979 / 180.0);
  audspace::SpaceData d{};
  d.flags = audspace::flags::kFrameValid | audspace::flags::kPoseValid | audspace::flags::kReads;
  d.c[3] = 1;
  d.qUsed[3] = 1;
  d.poseBase = 7;
  d.frameQpc = audio::Qpc();
  audr::SpaceCtl ctl;
  ctl.level = 1;
  ctl.haveParams = true;
  audr::RoomBlockBegin(blk, &d, ctl, audio::Qpc(), audio::QpcFreq());
  const float fwd[3] = {1, 0, 0};
  float seen[3];
  audspace::QRotate(blk.roomC, fwd, seen);
  const bool turned = blk.roomOn && blk.roomFrameValid && std::fabs(seen[0] - 0.8660254f) < 1e-4f && std::fabs(seen[2] - 0.5f) < 1e-4f;
  audr::RoomEnd(b.room);
  d.poseBase = 8;
  audr::RoomBlockBegin(blk, &d, ctl, audio::Qpc(), audio::QpcFreq());
  const bool asIs = blk.roomFrameValid && blk.roomC[3] == 1.0f;
  audr::RoomEnd(b.room);
  audr::RoomDelete(b.room);
  Check(turned && asIs, "cockpit space: the room turns against the head at audio rate (late head correction of the "
                        "aircraft frame); another base space leaves DCS's frame as it is");
}

// The optional signature rows: a mismatch keeps binaural and turns only the space's reads off (said once).
void Signatures() {
  Rig r;
  SpaceRig(r);
  FakeTable alt = r.tab;
  alt.t.locs = alt.locs;
  alt.t.masks = alt.masks;
  alt.locs[audsig::kSpaceGain].fnHash ^= 1;
  audhk::Resolved rr;
  const bool ok = audhk::Resolve(r.im.base, alt.t, &FakeImports, &rr);
  const bool gainOff = ok && !rr.spaceGain && rr.spaceOrient && strstr(rr.spaceWhy, "SpaceGain not found");
  alt.locs[audsig::kSpaceGain].fnHash ^= 1;
  alt.locs[audsig::kSpaceOrient].fnHash ^= 1;
  const bool ok2 = audhk::Resolve(r.im.base, alt.t, &FakeImports, &rr);
  const bool orientOff = ok2 && rr.spaceGain && !rr.spaceOrient;
  audhk::Resolved full;
  const bool both = audhk::Resolve(r.im.base, r.tab.t, &FakeImports, &full) && full.spaceGain && full.spaceOrient;
  // The module installed with such a table: binaural runs, the log says what is missing.
  audio::Uninstall(400);
  alt.locs[audsig::kSpaceGain].fnHash ^= 1;
  audio::g_testTable = &alt.t;
  audio::g_hookState = 0;
  audio::g_loop.lastInstall = 0;
  StartReportCapture();
  Tick();
  Bind(r);
  Frames(r, 6);
  Tick();
  const std::string log = StopReportCapture();
  const bool installed = audio::g_installed.load() && audio::g_cnt.ownedNow.load() == 3 && !audio::g_res.spaceGain;
  audio::g_testTable = &r.tab.t;
  Check(gainOff && orientOff && both,
        "cockpit space signatures: the canopy damping and the orientation code are checked on their own (hash), each "
        "optional");
  Check(installed && log.find("audio: cockpit space: ed_sound's cockpit fields not found (") != std::string::npos &&
            log.find("no through-canopy class") != std::string::npos,
        "cockpit space signatures: unmatched code keeps binaural on and turns only the space's reads off (said once)");
  RigDown(r);
}

// M9: over budget with the early reflections running: they go first, the sounds stay; back after the calm.
void Budget() {
  StartReportCapture();
  Rig r;
  SpaceRig(r, "[Audio]\r\nBinaural=1\r\nMaxObjects=16\r\nLateHeadTracking=0\r\nCpuBudgetPercent=5\r\nCockpitSpace=2\r\n");
  const double q[4] = {0, 0, 0, 1};
  for (int i = 0; i < 2; ++i) r.w.SetOrient(i, q);
  Frames(r, 4);
  audiofake::g_spinUs = 400;
  std::vector<float> a(960), b(960);
  ULONGLONG t0 = GetTickCount64();
  while (GetTickCount64() - t0 < 1300) {
    SimFrame(r);
    Block(r, a.data(), b.data(), 480);
  }
  audiofake::g_spinUs = 0;
  Tick();
  const bool earlyOff = !audiofake::g_roomLastFrame.early && audio::g_cnt.limitNow.load() == binaural::kMaxSources;
  // Calm: the early reflections come back after 10 s under half the budget (shortened here: the window that still
  // held slow blocks closes first, then 9.5 s are taken as already calm).
  t0 = GetTickCount64();
  while (GetTickCount64() - t0 < 1200) {
    SimFrame(r);
    Block(r, a.data(), b.data(), 480);
  }
  audio::g_binding.load()->calmSeconds = 9.5;
  t0 = GetTickCount64();
  while (GetTickCount64() - t0 < 1300) {
    SimFrame(r);
    Block(r, a.data(), b.data(), 480);
  }
  Tick();
  const bool earlyOn = audiofake::g_roomLastFrame.early;
  const std::string log = StopReportCapture();
  const size_t off = log.find("audio: cockpit space: early reflections off (CPU budget)");
  const size_t down = log.find("audio: CPU budget: at most");
  Check(earlyOff && off != std::string::npos && (down == std::string::npos || down > off),
        "cockpit space budget: step 1 turns the early reflections off before any sound is dropped");
  Check(earlyOn && log.find("audio: cockpit space: early reflections on again") != std::string::npos,
        "cockpit space budget: after the calm they come back (logged)");
  RigDown(r);
}

// M15: the log lines and CSV columns of docs/COCKPIT_SPACE.md 8.5 / 8.6.
void LogAndCsv() {
  const std::string src = lptest::ReadAllSources();
  const char* phrases[] = {"\"audio: cockpit space: %s (%ls %s, aircraft %s)\"",
                           "\"audio: cockpit space: inside %d, canopy %.2f (%s)\"",
                           "\"audio: cockpit space: inside %d, canopy unknown (none)\"",
                           "\"audio: cockpit space: frame from %s, cockpit ctx %d, spread %.2f deg, unreliable frames %u, "
                           "outlier sounds %u\"",
                           "\"audio: cockpit space: inside signals: ed_sound %s, camera %s\"",
                           "\"audio: cockpit space: early reflections off (CPU budget)\"",
                           "\"audio: cockpit space: early reflections on again\"",
                           "\"audio: ctx %d listener gain=%.4g pitch=%.4g lowpass=%.4g\"",
                           "\"%s %ls inside %d wet voices %u early %s room p99 %.2f ms, direct %.1f dB, match "
                           "%+.1f/%+.1f/%+.1f dB\"",
                           "\"%llu), overruns %llu; space %s; head pose %s\""};
  int missing = 0;
  for (const char* p : phrases)
    if (src.find(p) == std::string::npos) ++missing, printf("     cockpit space phrase missing: %s\n", p);
  Check(missing == 0, "cockpit space log: every line of the design is in the source (the running line keeps ending "
                      "with \"; head pose ...\")");
  Rig r;
  DeleteFileW((TestDir(L"audio_rig") + L"audio_space.csv").c_str());
  DeleteFileW((TestDir(L"audio_rig") + L"audio_voices.csv").c_str());
  audio::g_spaceCsv.clear();
  audio::g_voicesCsv.clear();
  SpaceRig(r, "[Audio]\r\nBinaural=1\r\nMaxObjects=16\r\nLateHeadTracking=0\r\nLogVoices=1\r\n");
  StartReportCapture();
  audio::LuaAircraft("F-16C_50");
  audio::LuaState(0.25, 1, 1);
  Tick();
  Frames(r, 10);
  audio::g_loop.lastVoices = 0;
  audio::g_loop.lastStats = 0;
  Tick();
  const std::string log = StopReportCapture();
  auto firstLine = [](const std::wstring& p) {
    std::string s;
    FILE* f = nullptr;
    if (!_wfopen_s(&f, p.c_str(), L"rb") && f) {
      char buf[1024] = {};
      if (fgets(buf, sizeof(buf), f)) s = buf;
      fclose(f);
    }
    return s;
  };
  const std::string space = firstLine(r.dir + L"audio_space.csv"), voices = firstLine(r.dir + L"audio_voices.csv");
  Check(space == "time,level,family,type,inside,canopy,canopy_src,frame_src,cockpit_ctx,yaw_deg,pitch_deg,roll_deg,"
                 "spread_deg,wet,early,wet_voices,room_p50_us,room_p99_us,dcomp_mean_db,lg_gain,lg_lowpass,match_lo_db,"
                 "match_mid_db,match_hi_db\r\n",
        "cockpit space CSV: audio_space.csv with the design's columns (LogVoices)");
  Check(voices.find(",power,room,send_db,through,cgain,ax,ay,az,dcomp_db\r\n") != std::string::npos,
        "cockpit space CSV: audio_voices.csv gains the room columns after power");
  Check(log.find("; space Subtle bubble inside 1 wet voices 3 early on room p99 ") != std::string::npos &&
            log.find("; head pose unused") != std::string::npos,
        "cockpit space log: the running line says the space's level, family, inside and voices before the head pose");
  RigDown(r);
  DeleteFileW((r.dir + L"audio_space.csv").c_str());
  DeleteFileW((r.dir + L"audio_voices.csv").c_str());
  audio::g_spaceCsv.clear();
  audio::g_voicesCsv.clear();
}

// M12: the Lua hook with a stand-in loader (qv_fake_payload.dll's DcsQvCull_Audio*): no Export call for the space while
// the payload does not want it; the type, canopy and camera check passed when it does.
void LuaBridge() {
  HMODULE lua = dcsinstall::Found() ? LoadLibraryW((dcsinstall::Bin() + L"lua.dll").c_str()) : nullptr;
  if (!lua) {
    dcsinstall::Skip("cockpit space Lua bridge (DCS's lua.dll)");
    return;
  }
  using namespace r63lua;
  auto newState = reinterpret_cast<NewStateFn>(GetProcAddress(lua, "luaL_newstate"));
  auto openLibs = reinterpret_cast<OpenLibsFn>(GetProcAddress(lua, "luaL_openlibs"));
  auto loadString = reinterpret_cast<LoadStringFn>(GetProcAddress(lua, "luaL_loadstring"));
  auto loadFile = reinterpret_cast<LoadFileFn>(GetProcAddress(lua, "luaL_loadfile"));
  auto pcall = reinterpret_cast<PcallFn>(GetProcAddress(lua, "lua_pcall"));
  auto toLString = reinterpret_cast<ToLStringFn>(GetProcAddress(lua, "lua_tolstring"));
  auto setTop = reinterpret_cast<SetTopFn>(GetProcAddress(lua, "lua_settop"));
  auto closeState = reinterpret_cast<CloseFn>(GetProcAddress(lua, "lua_close"));
  const std::wstring exe = ExeDir();
  wchar_t tmp[MAX_PATH] = {};
  GetTempPathW(MAX_PATH, tmp);
  const std::wstring root = std::wstring(tmp) + L"qv_space_lua" + std::to_wstring(GetCurrentProcessId()) + L"\\";
  const std::wstring mod = root + L"Scripts\\DcsQvCull\\";
  CreateDirectoryW(root.c_str(), nullptr);
  CreateDirectoryW((root + L"Scripts").c_str(), nullptr);
  CreateDirectoryW(mod.c_str(), nullptr);
  const bool copied = CopyFileW((exe + L"qv_fake_payload.dll").c_str(), (mod + L"DcsQvCull.dll").c_str(), FALSE) != 0;
  if (!newState || !openLibs || !loadString || !loadFile || !pcall || !toLString || !setTop || !closeState || !copied) {
    QvSkip("cockpit space Lua bridge: lua.dll exports or the stand-in module unavailable\n");
  } else {
    State L = newState();
    openLibs(L);
    const std::string prelude =
        "lfs = {writedir = function() return [[" + Narrow(root) +
        "]] end}\nlog = {INFO = 1, ERROR = 2, write = function() end}\nCALLS = 0\n"
        "Export = {LoGetMechInfo = function() CALLS = CALLS + 1; return {canopy = {status = 1, value = 0.75}} end,\n"
        "  LoGetCameraPosition = function() CALLS = CALLS + 1; return {p = {x = 1, y = 2, z = 3}} end,\n"
        "  LoGetSelfData = function() CALLS = CALLS + 1; return {Name = 'F-16C_50', Position = {x = 1, y = 2, z = 4}} end}\n"
        "DCS = {setUserCallbacks = function(cb) CB = cb end, getModelTime = function() return 1 end,\n"
        "  getPlayerUnitType = function() CALLS = CALLS + 1; return 'FA-18C_hornet' end}\n";
    bool ok = loadString(L, prelude.c_str()) == 0 && pcall(L, 0, 0, 0) == 0;
    const std::string hook = Narrow(exe) + "..\\lua\\DcsQvCull.lua";
    ok = ok && loadFile(L, hook.c_str()) == 0 && pcall(L, 0, 0, 0) == 0;
    const char* frames =
        "collectgarbage('collect'); collectgarbage('stop'); CALLS = 0\n"
        "local before = collectgarbage('count')\n"
        "for i = 1, 200 do CB.onSimulationFrame() end\n"
        "local grew = collectgarbage('count') - before\n"
        "collectgarbage('restart')\n"
        "return tostring(CALLS) .. ' ' .. string.format('%.3f', grew)";
    auto run = [&](const char* wanted, int* calls, double* grewKb) {
      SetEnvironmentVariableA("QV_TEST_AUDIO_WANTED", wanted);
      *calls = -1;
      *grewKb = -1;
      if (!(loadString(L, frames) == 0 && pcall(L, 0, 1, 0) == 0)) return false;
      size_t n = 0;
      const char* s = toLString(L, -1, &n);
      const bool parsed = s && sscanf_s(s, "%d %lf", calls, grewKb) == 2;
      setTop(L, 0);
      return parsed;
    };
    int callsOff = -1, callsOn = -1;
    double grewOff = -1, grewOn = -1;
    ok = ok && run("0", &callsOff, &grewOff) && run("1", &callsOn, &grewOn);
    HMODULE fake = GetModuleHandleW((mod + L"DcsQvCull.dll").c_str());
    using TypeFn = const char* (*)();
    using StateFn = void (*)(double*, int*, int*, long*);
    auto typeFn = fake ? reinterpret_cast<TypeFn>(GetProcAddress(fake, "QvFakeAudioType")) : nullptr;
    auto stateFn = fake ? reinterpret_cast<StateFn>(GetProcAddress(fake, "QvFakeAudioState")) : nullptr;
    double canopy = -9;
    int src = -9, inside = -9;
    long states = -1;
    if (stateFn) stateFn(&canopy, &src, &inside, &states);
    const std::string type = typeFn ? typeFn() : "";
    printf("     (cockpit space Lua: off %d Export calls, heap +%.3f KB; on %d calls, type %s, canopy %.2f (%d), inside %d, "
           "%ld state calls)\n",
           callsOff, grewOff, callsOn, type.c_str(), canopy, src, inside, states);
    Check(ok && callsOff == 0 && grewOff == 0.0,
          "cockpit space Lua bridge: while the payload does not want it, no Export call and nothing allocated");
    Check(ok && type == "FA-18C_hornet" && canopy == 0.75 && src == 1 && inside == 1 && states >= 1 && states <= 20,
          "cockpit space Lua bridge: the type (DCS.getPlayerUnitType), the canopy (LoGetMechInfo) and the camera check "
          "reach the payload, the state at most every 0.2 s");
    closeState(L);
    SetEnvironmentVariableA("QV_TEST_AUDIO_WANTED", nullptr);
  }
  DeleteFileW((mod + L"DcsQvCull.dll").c_str());
  RemoveDirectoryW(mod.c_str());
  RemoveDirectoryW((root + L"Scripts").c_str());
  RemoveDirectoryW(root.c_str());
  FreeLibrary(lua);
}

// M14: the room path allocates nothing on the real-time threads (sends, flushes, kill switch, inside / outside, frame).
void NoAllocRoom() {
  Rig r;
  SpaceRig(r);
  const double q[4] = {0, 0, 0, 1};
  for (int i = 0; i < 2; ++i) r.w.SetOrient(i, q);
  audiofake::g_roomOutput = true;
  std::vector<float> a(2 * 2048), b(2 * 2048);
  audiofake::g_rtAllocs = 0;
  audiofake::t_rtThread = true;
  for (int blk = 0; blk < 400; ++blk) {
    const uint32_t frames = 64 + (blk * 131) % 1985;
    if (blk == 100) audio::g_userOff = true;
    if (blk == 150) audio::g_userOff = false;
    if (blk == 200) SetCtxCockpitGain(r, kWorldCtx, true);
    if (blk == 250) SetCtxCockpitGain(r, kWorldCtx, false);
    if (blk == 320) SetCtxCockpitGain(r, kWorldCtx, true);
    if (blk % 2 == 0) SimFrame(r);
    Block(r, a.data(), b.data(), frames);
  }
  audiofake::t_rtThread = false;
  Check(audiofake::g_rtAllocs.load() == 0 && audiofake::g_roomAdds.load() > 100,
        "cockpit space real time: no heap allocation on the simulation and render paths with the room running");
  RigDown(r);
}

// M16: [AudioTest] Active=1 with Level plays that level in place of CockpitSpace (Live: the listening test's Part B),
// and the module never names it while the test is on (the pilot rates blind).
void TestLevel() {
  StartReportCapture();
  Rig r;
  SpaceRig(r);
  audio::LuaAircraft("F-16C_50");
  Tick();
  auto ini = [&](const char* test) {
    const std::string t = std::string(kIniSpace) + test;
    WriteIni(r.dir, t.c_str());
    Tick();
  };
  ini("[AudioTest]\r\nActive=1\r\nSeed=7\r\nTrials=12\r\nLevel=2\r\n");
  const bool two = audio::g_cfgRead.space.level == 1 && audio::g_cfgRead.space.Effective() == 2 &&
                   audiofake::g_roomLastParams.strength == 2;
  Frames(r, 40);
  const bool playing = audiofake::g_roomLastFrame.wet == 1.0f;
  audio::g_loop.lastStats = 0;
  Tick();
  ini("[AudioTest]\r\nActive=1\r\nSeed=7\r\nTrials=12\r\nLevel=0\r\n");
  Frames(r, 40);
  const bool off = audiofake::g_roomLastFrame.wet == 0.0f;
  ini("[AudioTest]\r\nActive=1\r\nSeed=7\r\nTrials=12\r\nLevel=3\r\n");
  const bool three = audiofake::g_roomLastParams.strength == 3;
  ini("[AudioTest]\r\nActive=0\r\nSeed=7\r\nTrials=12\r\nLevel=3\r\n");
  const bool own = audio::g_cfgRead.space.Effective() == 1 && audiofake::g_roomLastParams.strength == 1;
  ini("[AudioTest]\r\nActive=1\r\nLevel=5\r\n");
  const bool bad = audio::g_cfgRead.space.Effective() == 1 && !audio::g_cfgRead.space.Blind();
  ini("[AudioTest]\r\nActive=1\r\nSeed=7\r\nTrials=12\r\n");  // started, no trial yet: the pilot's own level
  const bool noLevel = audio::g_cfgRead.space.Effective() == 1 && !audio::g_cfgRead.space.Blind();
  const std::string log = StopReportCapture();
  const bool named = log.find("Normal (bubble") != std::string::npos || log.find("Strong (bubble") != std::string::npos ||
                     log.find("space Normal") != std::string::npos || log.find("space Strong") != std::string::npos;
  Check(two && playing && off && three,
        "cockpit space test: [AudioTest] Level plays that level in place of CockpitSpace (Live; 0 fades the space out)");
  Check(own && bad && noLevel,
        "cockpit space test: Active=0, no Level or a Level out of 0-3 leave the pilot's own level");
  Check(!named && log.find("audio: cockpit space: Test (bubble auto, aircraft F-16C_50)") != std::string::npos &&
            log.find("; space Test; head pose ") != std::string::npos &&
            log.find("listening test level in use (not named)") != std::string::npos,
        "cockpit space test: while the test is on the log never names the level it plays (blind)");
  RigDown(r);
}

// The aircraft frame's robustness: one stray cockpit sound does not cost the frame (outliers counted); a jumped frame
// that holds for 0.4 s is taken (a view snap with no head pose); with no head pose a frame older than 1 s is dropped, so
// the next one is not held against it.
void FrameRobust() {
  Rig r;
  SpaceRig(r, kIniSpace, true);
  const double id[4] = {0, 0, 0, 1};
  const double off5[4] = {0, 0.043619387365336, 0, 0.999048221581858};  // 5 deg about y
  const double turn[4] = {0, 0.707106781186548, 0, 0.707106781186548};  // 90 deg about y
  const double zero[4] = {0, 0, 0, 0};
  const int cockpit[3] = {0, 1, 4};
  r.w.SetOrient(0, id);
  r.w.SetOrient(1, id);
  r.w.SetOrient(4, off5);
  Frames(r, 6);
  audspace::SpaceData sd{};
  audspace::SpaceRead(audio::g_space, &sd);
  const bool outlier = (sd.flags & audspace::flags::kFrameFromSound) && (sd.flags & audspace::flags::kFrameValid) &&
                       sd.c[3] > 0.99999f && sd.outliers >= 3 && sd.unreliable == 0 && sd.spreadDeg > 4.0f;
  for (int i : cockpit) r.w.SetOrient(i, turn);
  Frames(r, 3);
  audspace::SpaceRead(audio::g_space, &sd);
  const bool held = sd.c[3] > 0.99999f && sd.unreliable >= 2;
  Sleep(450);
  Frames(r, 3);
  audspace::SpaceRead(audio::g_space, &sd);
  const bool taken = (sd.flags & audspace::flags::kFrameValid) && std::fabs(sd.c[1] - 0.70710678f) < 1e-5f;
  for (int i : cockpit) r.w.SetOrient(i, zero);
  Frames(r, 3);
  Sleep(1100);
  for (int i : cockpit) r.w.SetOrient(i, id);  // 90 deg from the last good frame
  Frames(r, 3);
  audspace::SpaceRead(audio::g_space, &sd);
  const bool fresh = (sd.flags & audspace::flags::kFrameValid) && sd.c[3] > 0.99999f;
  printf("     frame: outliers %u, unreliable %u, spread %.2f deg\n", sd.outliers, sd.unreliable, sd.spreadDeg);
  Check(outlier, "cockpit space frame: two of three cockpit sounds agreeing give the frame (the stray one counted, "
                 "not a refused frame)");
  Check(held && taken, "cockpit space frame: a jump over 30 deg is refused at first, taken once it held 0.4 s");
  Check(fresh, "cockpit space frame: with no head pose a frame older than 1 s is dropped; the next one is taken at once");
  RigDown(r);
}

// The cockpit vote in a big cabin: every cockpit sound beyond 1.5 m wins no vote; the 3D context without
// use-cockpit-gain while the world context has it is then the cockpit.
void BigCabin() {
  Rig r;
  RigUp(r, kIniSpace);
  SetContext2D(r, kRadioCtx, true);
  AddBoth(r, kCockpitCtx, true, 1, kSine, 0.4f, 440, 1.0f, 3);
  AddBoth(r, kCockpitCtx, true, 1, kNoise, 0.3f, 0, 0.8f, 5);
  AddBoth(r, kWorldCtx, true, 1, kSine, 0.3f, 97, 0.9f);
  PosBoth(r, 0, 3.0, 0.2, 0.5);
  PosBoth(r, 1, -2.6, 0.3, 1.0);
  PosBoth(r, 2, 20, 0, 5);
  for (int i = 0; i < 3; ++i) CreateVoice(r, i);
  Bind(r);
  Frames(r, 6);
  audspace::SpaceData sd{};
  audspace::SpaceRead(audio::g_space, &sd);
  const bool none = sd.cockpitCtx == 8;
  SetCtxCockpitGain(r, kWorldCtx, true);
  Frames(r, 4);
  audspace::SpaceRead(audio::g_space, &sd);
  const bool fallback = sd.cockpitCtx == kCockpitCtx;
  Check(none && fallback, "cockpit space vote: in a big cabin (no sound within 1.5 m) the context without "
                          "use-cockpit-gain while the world context has it is the cockpit");
  RigDown(r);
}

// The room's cost in the log and CSV counts its per-voice work (the sends' taps in AddSource, the direct and send
// gains), not only BeginBlock / Render / EndBlock.
void RoomCost() {
  Rig r;
  SpaceRig(r);
  const double q[4] = {0, 0, 0, 1};
  for (int i = 0; i < 2; ++i) r.w.SetOrient(i, q);
  Frames(r, 10);
  audiofake::g_roomAddSpinUs = 300;
  Frames(r, 40);
  audiofake::g_roomAddSpinUs = 0;
  double p50 = 0, p99 = 0;
  audio::RoomTimes(&p50, &p99);
  printf("     room cost with 3 x 0.3 ms per-voice sends: p50 %.0f us, p99 %.0f us\n", p50, p99);
  Check(p50 >= 800.0, "cockpit space cost: the room's p50 / p99 include its per-voice sends");
  RigDown(r);
}

// A new HRTF set while the room idles (CockpitSpace=0, outside): one empty block lets it adopt the set (so the old one
// is retired by both renderers and freed); no output, no further call.
void IdleSetAdopt() {
  audr::Binding b;
  b.rate = 48000;
  b.room = audr::RoomNew();
  char err[64];
  audr::RoomInit(b.room, 48000, 64, err, sizeof(err));
  audr::Block blk;
  blk.b = &b;
  blk.frames = 480;
  blk.on = true;
  audr::SpaceCtl ctl;  // level 0: the room idles
  const uint64_t b0 = audiofake::g_roomBegins.load(), e0 = audiofake::g_roomEnds.load(), r0 = audiofake::g_roomRenders.load();
  for (int i = 0; i < 5; ++i) audr::RoomBlockBegin(blk, nullptr, ctl, audio::Qpc(), audio::QpcFreq());
  const bool skipped = audiofake::g_roomBegins.load() == b0 && !blk.roomOn;
  b.roomSetGen.fetch_add(1);
  for (int i = 0; i < 5; ++i) audr::RoomBlockBegin(blk, nullptr, ctl, audio::Qpc(), audio::QpcFreq());
  const bool once = audiofake::g_roomBegins.load() == b0 + 1 && audiofake::g_roomEnds.load() == e0 + 1 &&
                    audiofake::g_roomRenders.load() == r0 && !blk.roomOn;
  audr::RoomDelete(b.room);
  Check(skipped && once, "cockpit space: a new HRTF set while the room idles is shown to it in one empty block (the old "
                         "set can be freed); otherwise the idle room is never called");
}

inline void Run() {
  printf("---- cockpit space (audio module side)\n");
  Families();
  InsideOutside();
  Canopy();
  ThroughAndRadio();
  KillSwitch();
  OffExact();
  Frame();
  Signatures();
  Budget();
  LogAndCsv();
  LuaBridge();
  NoAllocRoom();
  TestLevel();
  FrameRobust();
  BigCabin();
  RoomCost();
  IdleSetAdopt();
}

}  // namespace spacetest
