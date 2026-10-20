// R95: the pipeline test's run (included inside namespace r95pipe, after its rig and frame).
#pragma once

void __fastcall NoExec(void*, uint64_t) {}

struct SetRec {
  uint32_t gen = 0, flags = 0;
  uint64_t h[3] = {}, k[3] = {}, fence = 0;
};
SetRec Rec(Pipe& p) {
  SetRec r;
  const SymRec s = ReadSym(p);
  if (!s.ok) return r;
  r.gen = s.h.generation, r.flags = s.h.flags, r.fence = s.h.fenceHandle;
  for (int i = 0; i < 3; ++i) r.h[i] = s.h.textureHandle[i], r.k[i] = s.h.classTextureHandle[i];
  return r;
}
bool SameSet(const SetRec& a, const SetRec& b) {
  return a.gen == b.gen && !memcmp(a.h, b.h, sizeof(a.h)) && !memcmp(a.k, b.k, sizeof(a.k)) && a.h[0] != 0 && a.k[0] != 0;
}
bool IsActive(const SetRec& r) { return (r.flags & csy::header_flags::active) != 0; }
uint32_t MaskGen(Pipe& p) {
  cm::Header h{};
  return p.mblk && cm::seqlock_read(p.mblk, h) ? h.generation : 0;
}
bool Contains(const csy::EmissionView& m, const cmask::sym::Box& b) {
  return b.ok && int(m.winX) <= b.x0 && int(m.winY) <= b.y0 && int(m.winX + m.winW) >= b.x1 && int(m.winY + m.winH) >= b.y1;
}

void Test(D3D_DRIVER_TYPE drv, bool hw) {
  Pipe p;
  if (!MakeRig(p.r, drv)) {
    Check(false, "R95 pipeline: device, HDR / LDR targets, DCS's tone map and symbol shaders, our colour objects");
    p.r.Release();
    return;
  }
  const char* tag = hw ? "R95 pipeline (hardware)" : "R95 pipeline";
  p.settleMs = hw ? 5 : 25;  // a frame pace (DCS: >= 11 ms): the non-blocking read-backs keep up
  Rig& r = p.r;
  // The live install path (fixation_depth.h Tick), the mask export as the master: view 0 sampled; the symbology on.
  const ptiming::ExecFn execSave = ptiming::g_orig;
  if (!ptiming::g_orig) ptiming::g_orig = &NoExec;
  ID3D11Device* oldDev = shadowinst::g_device;
  shadowinst::g_device = r.dev;
  ptiming::g_topTid = GetCurrentThreadId();
  g_coneOn = false, g_gridOn = false;
  fdexport::g_on = false;
  g_hmdOffset = 0;
  g_fault = false;
  cpass::g_fault = false;
  cmask::g_xrHookWanted = false;
  cmask::g_flightGate = false;
  cmask::g_buildWaitMs = 5000;
  cmask::g_testRemoved = false;
  wchar_t mname[64], sname[64], gname[64], pname[64];
  swprintf_s(mname, L"Local\\R95TestMask_%lu_%d", GetCurrentProcessId(), hw ? 1 : 0);
  swprintf_s(sname, L"Local\\R95TestSym_%lu_%d", GetCurrentProcessId(), hw ? 1 : 0);
  swprintf_s(gname, L"Local\\R95TestGaze_%lu", GetCurrentProcessId());
  swprintf_s(pname, L"Local\\R95TestPupil_%lu_%d", GetCurrentProcessId(), hw ? 1 : 0);
  g_gazeName = gname;
  auto configure = [&](bool mask, bool symOn, int div) {
    cmask::ConfigureDepth(false, 0);
    cmask::ConfigureMotion(false, true, 0);
    cmask::ConfigureSymbology(symOn, true);
    cmask::Configure(mask, div, 1, 1.0, 0);
    cmask::g_blockName = mname;
    cmask::g_sBlockName = sname;
  };
  // The reader's own mappings (they outlive our reinstalls).
  p.mmap = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, 4096, mname);
  p.smap = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, 4096, sname);
  p.mblk = p.mmap ? static_cast<cm::Header*>(MapViewOfFile(p.mmap, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(cm::Header))) : nullptr;
  p.sblk = p.smap ? static_cast<csy::Header*>(MapViewOfFile(p.smap, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(csy::Header))) : nullptr;
  auto tick = [&] { Tick(false, false, 0, nullptr, false, std::wstring()); };
  configure(true, true, 2);
  tick();
  cpass::g_obj.store(&r.obj);
  cpass::g_symPsReady = 1;
  cpass::g_rt = cpass::Rt{};
  cpass::g_mode = 0;
  for (int v = 0; v < 4; ++v) cpass::NoteLdr(v, kOW[v], kOH[v]);
  namespace ct = cmask::ctime;
  const auto keepLv = ct::g_orig;
  ct::g_orig = &FakeLocate4;
  ct::g_on = true;
  ct::g_hooked = true;
  ct::g_testRecordAlways = true;
  cmask::sym::ResetLearn();
  const int keepAlphaRun = cmask::sym::g_alphaOnRun.load();
  cmask::sym::g_alphaOnRun = 6;  // R97: the qualifying run of valid votes (256 in DCS) short enough for this test
  const bool installed = g_installed.load() && g_symWith && cmask::g_sview.load() && cmask::g_view.load() &&
                         cpass::g_symQuery == &cmask::sym::ToneQuery && ckipd::ocpu::g_symRec == &cmask::sym::Record;
  {
    const SymRec s0 = ReadSym(p);
    Check(installed && s0.ok && s0.h.magic == csy::block_magic && (s0.h.flags & csy::header_flags::reader_heartbeat) &&
              (s0.h.flags & csy::header_flags::idle) && !(s0.h.flags & csy::header_flags::active),
          "R95 install (Tick, SymbologyExport=1 with the mask): the block mapped (magic, v1, reader_heartbeat | idle, not "
          "active), the colour pass's query and ovl_cpu's record point set");
  }
  cmask::sym::g_testRenderPoint = true;  // ovl_cpu.h's render point (ckipd off here: the test reports the records)
  int64_t dt = 50000;
  // Learning: the first frames learn the targets, scan, read back; then windows.
  const uint64_t scans0 = cmask::sym::g_scans.load();
  for (int f = 0; f < 10; ++f) Frame(p, dt++);
  {
    const SymRec s = ReadSym(p);
    const int sl = s.ok ? csy::find_slot(s.h, dt - 1) : -1;
    int present = 0;
    for (int v = 0; v < 4 && sl >= 0; ++v) present += (s.h.slot[sl].view[v].flags & csy::view_flags::present) != 0;
    char what[300];
    sprintf_s(what, "%s: learning (target at the first record, a scan, the read-back) then windows: %d views present "
                    "after 10 frames; %llu scans; the header active with the mask's generation and fence",
              tag, present, static_cast<unsigned long long>(cmask::sym::g_scans.load() - scans0));
    cm::Header mh{};
    const bool mok = p.mblk && cm::seqlock_read(p.mblk, mh);
    Check(sl >= 0 && present == 4 && mok && s.h.generation == mh.generation && s.h.fenceHandle == mh.fenceHandle &&
              (s.h.flags & csy::header_flags::shares_mask_fence) && (s.h.flags & csy::header_flags::class_atlas) &&
              s.h.atlasW == 2560 && s.h.atlasH == 1024 && s.h.classFormat == csy::format_r8_uint &&
              (s.h.emissionFlags & ~csy::emission_flags::alpha_valid) ==
                  (csy::emission_flags::hdr_overlay | csy::emission_flags::class_collimated |
                   csy::emission_flags::class_head_locked | csy::emission_flags::common_excluded) &&
              cmask::sym::g_scans.load() > scans0,
          what);
  }
  // E: frame A (the symbols) against frame B (the same pose, the MFD only); the class plane; the alpha.
  p.readback = true;
  const int64_t dtA = dt;
  const FrameOut A = Frame(p, dt++);
  p.hudOn = p.helmetOn = false;
  const FrameOut B = Frame(p, dt++);
  p.hudOn = p.helmetOn = true;
  p.readback = false;
  {
    const SymRec s = ReadSym(p);
    cm::Header mh{};
    const bool mok = p.mblk && cm::seqlock_read(p.mblk, mh);
    const int sl = s.ok ? csy::find_slot(s.h, dtA) : -1, ml = mok ? cm::find_slot(mh, dtA) : -1;
    Atl a;
    const bool read = sl >= 0 && ReadAtlas(p, s.h, sl, a);
    bool flagsOk = sl >= 0, winOk = sl >= 0, layoutOk = false;
    int idBad = 0, nearOne = 0, zeroBad = 0, clsBad = 0, alphaBad = 0, mfdIn = 0, outside = 0, symTex = 0;
    double worst = 0;
    for (int v = 0; v < 4 && read; ++v) {
      const csy::EmissionView& m = s.h.slot[sl].view[v];
      const uint32_t need = csy::view_flags::valid | csy::view_flags::present | csy::view_flags::own_tonemap |
                            csy::view_flags::untouched_hdr;
      flagsOk = flagsOk && (m.flags & need) == need && !(m.flags & csy::view_flags::overflow) && m.outW == kOW[v] &&
                m.outH == kOH[v] && csy::view_present(s.h, sl, v) && m.capW == (v < 2 ? 512u : 1024u);
      winOk = winOk && Contains(m, A.hud[v]) && (!kHelmet[v].ok || Contains(m, kHelmet[v]));
      float t[4];
      for (int k = 0; k < 4; ++k) t[k] = static_cast<float>(std::tan(double(kFov[v][k])));
      flagsOk = flagsOk && std::fabs(m.fovTan[0] - t[0]) < 1e-6f && std::fabs(m.fovTan[3] - t[3]) < 1e-6f;
      const ViewCheck k = CheckView(m, a, A, B, v);
      idBad += k.idBad, nearOne += k.nearOne, zeroBad += k.zeroBad, clsBad += k.clsBad, alphaBad += k.alphaBad, mfdIn += k.mfdInWin;
      outside += k.outside, symTex += k.symTex, worst = std::max(worst, k.worst);
    }
    if (sl >= 0) {
      float tan[4][4], want[4];
      for (int v = 0; v < 4; ++v)
        for (int k = 0; k < 4; ++k) tan[v][k] = static_cast<float>(std::tan(double(kFov[v][k])));
      layoutOk = (s.h.slot[sl].focus.flags & csy::layout_flags::layout_valid) &&
                 s.h.slot[sl].focus.fovSource == csy::fov_source_dcs_submitted && s.h.fovSource == 0;
      for (int e = 0; e < 2; ++e) {
        csy::focus_rect(tan[e], tan[2 + e], want);
        for (int k = 0; k < 4; ++k) layoutOk = layoutOk && std::fabs(s.h.slot[sl].focus.eye[e].rectNorm[k] - want[k]) < 1e-6f;
      }
    }
    const bool paired = sl >= 0 && ml >= 0 && csy::paired_with_mask(s.h, sl, ml, mh.slot[ml].maskSeq, mh.generation) &&
                        (s.h.slot[sl].flags & (csy::slot_flags::valid | csy::slot_flags::display_time)) ==
                            (csy::slot_flags::valid | csy::slot_flags::display_time) &&
                        s.h.slot[sl].viewMask == 15 && s.h.slot[sl].engineFrame == mh.slot[ml].engineFrame;
    char what[700];
    sprintf_s(what,
              "%s: E through its NT handle after the shared fence: dec(F) - E re-encoded == F without the symbols at %d "
              "symbol texels of 4 views (worst %.2e; %d channels one code off (the GPU's rounding of that frame), %d "
              "more); E == 0 and class 0 elsewhere in "
              "the windows (%d / %d wrong; the MFD block's %d texels inside them included); class 1 HUD / 2 helmet; "
              "the coverage alpha (%d wrong); every symbol texel inside its window (%d outside)",
              tag, symTex, worst, nearOne, idBad, zeroBad, clsBad, mfdIn, alphaBad, outside);
    Check(read && symTex > 2000 && idBad == 0 && (hw || nearOne == 0) && zeroBad == 0 && clsBad == 0 && alphaBad == 0 && mfdIn > 20 &&
              outside == 0,
          what);
    Check(paired && flagsOk && winOk && layoutOk,
          "R95 commit: paired with the mask (slot index, seq == maskSeq, generation, engine frame, displayTime); per "
          "view valid | present | own_tonemap | untouched_hdr, its output size, capacity, window (HUD and "
          "helmet inside), fovTan = the submitted FOV; the focus layout from the submitted FOVs (dcs_submitted)");
  }
  // Stability (Dev's 0049 pairs two real frames carrying the same views): a still head, 12 frames: every view present
  // with the same window each frame; the coverage alpha valid (emission_flags::alpha_valid, after its agreeing
  // read-backs) and unchanged.
  {
    int changes = 0, absent = 0, alphaOn = 0, alphaOff = 0;
    bool wasOn = false, lastOn = false;
    csy::EmissionView last[4] = {};
    for (int f = 0; f < 12; ++f) {
      const int64_t d = dt;
      Frame(p, dt++);
      const SymRec s = ReadSym(p);
      const int sl = s.ok ? csy::find_slot(s.h, d) : -1;
      lastOn = sl >= 0 && (s.h.emissionFlags & csy::emission_flags::alpha_valid);
      alphaOn += lastOn;
      alphaOff += wasOn && !lastOn;  // on -> off: a flicker
      wasOn = wasOn || lastOn;
      for (int v = 0; v < 4; ++v) {
        const csy::EmissionView m = sl >= 0 ? s.h.slot[sl].view[v] : csy::EmissionView{};
        absent += !(m.flags & csy::view_flags::present);
        if (f > 0)
          changes += m.winX != last[v].winX || m.winY != last[v].winY || m.winW != last[v].winW || m.winH != last[v].winH ||
                     m.flags != last[v].flags;
        last[v] = m;
      }
    }
    char what[300];
    sprintf_s(what, "%s: a still head over 12 frames: every view present (%d absent), the same window and flags each "
                    "frame (%d changes); the coverage alpha flag (R97: 6 valid complete still read-backs here) on in %d "
                    "of 12, never back off (%d), on at the end",
              tag, absent, changes, alphaOn, alphaOff);
    Check(absent == 0 && changes == 0 && alphaOff == 0 && lastOn, what);
  }
  // Head yaw: the window follows the located pose of the frame (xrLocateViews), not the last commit.
  auto yawRun = [&](int frames, double step, int* missed) {
    *missed = 0;
    for (int f = 0; f < frames; ++f) {
      p.yaw += step;
      const int64_t d = dt;
      const FrameOut o = Frame(p, dt++);
      const SymRec s = ReadSym(p);
      const int sl = s.ok ? csy::find_slot(s.h, d) : -1;
      for (int v = 0; v < 4; ++v)
        if (o.hud[v].ok &&
            (sl < 0 || !(s.h.slot[sl].view[v].flags & csy::view_flags::present) || !Contains(s.h.slot[sl].view[v], o.hud[v])))
          ++*missed;
    }
  };
  int missPred = 0, missLast = 0;
  const uint64_t pred0 = cmask::sym::g_predicted.load(), edge0 = cmask::sym::g_edges.load();
  yawRun(6, 4.0, &missPred);
  const uint64_t predN = cmask::sym::g_predicted.load() - pred0, edgeN = cmask::sym::g_edges.load() - edge0;
  ct::g_hooked = false;  // the control: the last committed pose instead (one frame of head motion late)
  yawRun(6, -4.0, &missLast);
  ct::g_hooked = true;
  {
    char what[400];
    sprintf_s(what, "%s: head yaw 4 deg / frame (the focus HUD moves ~31 px a frame): with the located pose every "
                    "window holds the HUD (%d view-frames missed, %llu located, %llu edge hits); with the last commit's "
                    "pose instead %d missed (the control)",
              tag, missPred, static_cast<unsigned long long>(predN), static_cast<unsigned long long>(edgeN), missLast);
    Check(missPred == 0 && predN >= 24 && edgeN == 0 && missLast > 0, what);
  }
  cmask::sym::ResetLearn();  // the control widened the margins: learn afresh
  p.yaw = 0;
  for (int f = 0; f < 8; ++f) Frame(p, dt++);
  // A pass between the overlay and the tone map (the night-vision position is unknown, R94): no E in that view.
  {
    p.nvgPass = true;
    const int64_t d = dt;
    Frame(p, dt++);
    p.nvgPass = false;
    const SymRec s = ReadSym(p);
    const int sl = s.ok ? csy::find_slot(s.h, d) : -1;
    const uint32_t f2 = sl >= 0 ? s.h.slot[sl].view[2].flags : 0, f0 = sl >= 0 ? s.h.slot[sl].view[0].flags : 0;
    Check(sl >= 0 && (f2 & csy::view_flags::valid) && !(f2 & csy::view_flags::present) &&
              !(f2 & csy::view_flags::untouched_hdr) && (f0 & csy::view_flags::present) &&
              (f0 & csy::view_flags::untouched_hdr),
          "R95 guard: a pass between a view's OverlayHDRPass and its ToneMappingPass (not the histogram): no E in that "
          "view (valid, untouched_hdr clear, present clear); the others as before");
  }
  // The pupil shift layer's block: its original FOVs for the focus layout (and the views' fovTan).
  {
    static pf::Block pb{};
    HANDLE pm = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(pf::Block), pname);
    pf::Block* pv = pm ? static_cast<pf::Block*>(MapViewOfFile(pm, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(pf::Block))) : nullptr;
    cmask::sym::ClosePupil();
    cmask::sym::g_pupilName = pname;
    float orig[4][4];
    auto write = [&](int64_t d, bool match) {
      pb = pf::Block{};
      pb.magic = pf::magic_value, pb.version = pf::version_value, pb.size = sizeof(pf::Block), pb.enabled = 1;
      pb.ringSize = pf::ring_size, pb.writeCount = 1, pb.newestDisplayTime = d;
      pf::Entry& e = pb.entries[0];
      e.displayTime = d, e.viewCount = 4, e.flags = pf::entry_flags::pupil | pf::entry_flags::ipd;
      for (int v = 0; v < 4; ++v) {
        for (int k = 0; k < 4; ++k) orig[v][k] = kFov[v][k] * 1.03f;
        memcpy(e.views[v].shiftedFov, kFov[v], 16);
        if (!match) e.views[v].shiftedFov[1] += 0.01f;
        memcpy(e.views[v].originalFov, orig[v], 16);
        e.views[v].orientationDelta[3] = 1;
      }
      pb.seq = 2;
      if (pv) memcpy(pv, &pb, sizeof(pb));
    };
    write(dt, true);
    cmask::sym::TickPupil();
    const bool mapped = cmask::sym::g_pupil.load() != nullptr;
    const int64_t d1 = dt;
    Frame(p, dt++);
    const SymRec s1 = ReadSym(p);
    const int sl1 = s1.ok ? csy::find_slot(s1.h, d1) : -1;
    write(dt, false);
    const int64_t d2 = dt;
    Frame(p, dt++);
    const SymRec s2 = ReadSym(p);
    const int sl2 = s2.ok ? csy::find_slot(s2.h, d2) : -1;
    bool origOk = sl1 >= 0 && s1.h.slot[sl1].focus.fovSource == csy::fov_source_pupil_original &&
                  (s1.h.slot[sl1].focus.flags & csy::layout_flags::layout_valid);
    float to[4][4], want[4];
    for (int v = 0; v < 4; ++v)
      for (int k = 0; k < 4; ++k) to[v][k] = static_cast<float>(std::tan(double(orig[v][k])));
    for (int e = 0; e < 2 && origOk; ++e) {
      csy::focus_rect(to[e], to[2 + e], want);
      for (int k = 0; k < 4; ++k) origOk = origOk && std::fabs(s1.h.slot[sl1].focus.eye[e].rectNorm[k] - want[k]) < 1e-6f;
    }
    for (int v = 0; v < 4 && origOk; ++v) origOk = std::fabs(s1.h.slot[sl1].view[v].fovTan[1] - to[v][1]) < 1e-6f;
    const bool backOk = sl2 >= 0 && s2.h.slot[sl2].focus.fovSource == csy::fov_source_dcs_submitted && s2.h.fovSource == 0;
    Check(mapped && origOk && backOk,
          "R95 FOV source: with the pupil shift layer's entry for the frame (its shifted FOVs = DCS's submitted ones) the "
          "views' fovTan and the focus layout use its original FOVs (pupil_original); an entry of other FOVs: "
          "dcs_submitted");
    cmask::sym::ClosePupil();
    if (pv) UnmapViewOfFile(pv);
    if (pm) CloseHandle(pm);
  }
  // Quad Views' settings from its files (here the profile through DCSVR_QUAD_SETTINGS).
  {
    wchar_t tp[MAX_PATH];
    GetTempPathW(MAX_PATH, tp);
    const std::wstring cfg = std::wstring(tp) + L"r95_qv_" + std::to_wstring(GetCurrentProcessId()) + L".cfg";
    const std::string exe = cmask::sym::ExeName();
    const std::string stem = exe.substr(0, exe.rfind('.'));
    const std::string text = "sharpen_focus_view=0.5\n[exe:" + stem + "]\nsmoothen_focus_view_edges=0.3\nfocus_view_shape=2\n"
                             "dcsvr_temporal=1\n[SomeRuntime]\nsharpen_focus_view=0.1\n";
    FILE* f = nullptr;
    if (_wfopen_s(&f, cfg.c_str(), L"wb") == 0 && f) fwrite(text.data(), 1, text.size(), f), fclose(f);
    SetEnvironmentVariableW(L"DCSVR_QUAD_SETTINGS", cfg.c_str());
    cmask::sym::TickQv(true);
    const uint32_t gen1 = cmask::sym::g_qvGen.load();
    Frame(p, dt++);
    const SymRec s = ReadSym(p);
    const csy::QvSettings& q = s.h.qv;
    const bool ok = s.ok && (s.h.flags & csy::header_flags::qv_settings) && q.sharpenFocusView == 0.5f &&
                    q.smoothenFocusViewEdges == 0.3f && q.focusViewShape == 2.0f &&
                    q.flags == (csy::qv_flags::periphery_temporal | csy::qv_flags::profile_file) && q.settingsGen == gen1;
    cmask::sym::TickQv(true);  // unchanged files: the same generation
    const bool same = cmask::sym::g_qvGen.load() == gen1;
    DeleteFileW(cfg.c_str());  // DCSVR_QUAD_SETTINGS names a file that is not there (and the layer is not loaded)
    cmask::sym::TickQv(true);
    Frame(p, dt++);
    const SymRec sn = ReadSym(p);
    const bool none = sn.ok && !(sn.h.flags & csy::header_flags::qv_settings) && sn.h.qv.smoothenFocusViewEdges == 0 &&
                      sn.h.qv.sharpenFocusView == 0;
    SetEnvironmentVariableW(L"DCSVR_QUAD_SETTINGS", nullptr);
    cmask::sym::TickQv(true);
    Check(ok && same && none,
          "R95 Quad Views settings: read from its files in its order with its section rules (the profile file through "
          "DCSVR_QUAD_SETTINGS), published in the header (qv_settings), settingsGen counts changes; no file read -> "
          "qv_settings clear (the reader applies QV's defaults 0.2 / 0)");
  }
  // A frame committed without DCS's poses (late, or unreadable): the last focus layout again (0049 skips a slot
  // without one).
  {
    p.noPoses = true;
    const int64_t d = dt;
    Frame(p, dt++);
    p.noPoses = false;
    const SymRec s = ReadSym(p);
    const int sl = s.ok ? csy::find_slot(s.h, d) : -1;
    Check(sl >= 0 && (s.h.slot[sl].focus.flags & csy::layout_flags::layout_valid) && csy::layout_usable(s.h, sl) &&
              s.h.slot[sl].focus.eye[1].rectNorm[2] > s.h.slot[sl].focus.eye[1].rectNorm[0],
          "R95 focus layout: a frame without DCS's submitted poses publishes the last frame's layout (never a slot "
          "without one)");
  }
  // The reader's heartbeat: absent -> idle (no symbology slot, the mask still commits); fresh -> active again.
  {
    p.sBeat = false;
    if (p.sblk) csy::reader_heartbeat(p.sblk, 0);
    const int64_t d1 = dt;
    Frame(p, dt++);
    Frame(p, dt++);
    const SymRec s = ReadSym(p);
    cm::Header mh{};
    const bool mok = p.mblk && cm::seqlock_read(p.mblk, mh);
    const bool idle = s.ok && (s.h.flags & csy::header_flags::idle) && csy::find_slot(s.h, d1 + 1) < 0 && mok &&
                      cm::find_slot(mh, d1 + 1) >= 0 && cmask::g_sIdleA.load() &&
                      !(cmask::StatusBits() & qvstatus::kCockpitSymbology);
    p.sBeat = true;
    for (int f = 0; f < 3; ++f) Frame(p, dt++);
    const SymRec s2 = ReadSym(p);
    const bool back = s2.ok && !(s2.h.flags & csy::header_flags::idle) && csy::find_slot(s2.h, dt - 1) >= 0 &&
                      (cmask::StatusBits() & qvstatus::kCockpitSymbology);
    Check(idle && back, "R95 heartbeat: the reader's heartbeat absent -> idle (header idle, no symbology slot, the mask "
                        "still commits, status bit 43 clear); fresh again -> slots again (bit 43)");
    // R97: [Dev] SymbologyPublishAlways=1: no symbology reader heartbeat (OFXR 0053's OFF arm), still produced and
    // published with E (the mask's heartbeat still beats here); off again -> idle.
    cmask::g_symPublishAlways = true;
    p.sBeat = false;
    if (p.sblk) csy::reader_heartbeat(p.sblk, 0);
    for (int f = 0; f < 3; ++f) Frame(p, dt++);
    const int64_t dA = dt;
    Frame(p, dt++);
    const SymRec sa = ReadSym(p);
    const int sla = sa.ok ? csy::find_slot(sa.h, dA) : -1;
    int presentA = 0;
    for (int v = 0; v < 4 && sla >= 0; ++v) presentA += (sa.h.slot[sla].view[v].flags & csy::view_flags::present) != 0;
    const bool always = sa.ok && !(sa.h.flags & csy::header_flags::idle) && sla >= 0 && presentA == 4;
    cmask::g_symPublishAlways = false;
    for (int f = 0; f < 2; ++f) Frame(p, dt++);
    const SymRec sb = ReadSym(p);
    const bool idleAgain = sb.ok && (sb.h.flags & csy::header_flags::idle);
    p.sBeat = true;
    for (int f = 0; f < 3; ++f) Frame(p, dt++);
    Check(always && idleAgain,
          "R97 [Dev] SymbologyPublishAlways=1: without the symbology reader's heartbeat the slots are still committed "
          "with E in all four views (header not idle); key off -> idle again");
  }
  // Overflow: capacities below the symbology's extent (a capacity change is a generation change).
  {
    const uint32_t g0 = MaskGen(p);
    cmask::sym::g_capPeri = 24, cmask::sym::g_capFocus = 48;
    for (int f = 0; f < 4; ++f) Frame(p, dt++);
    const int64_t d = dt;
    Frame(p, dt++);
    const SymRec s = ReadSym(p);
    const int sl = s.ok ? csy::find_slot(s.h, d) : -1;
    int over = 0;
    for (int v = 0; v < 4 && sl >= 0; ++v) {
      const uint32_t fl = s.h.slot[sl].view[v].flags;
      over += (fl & csy::view_flags::overflow) && !(fl & csy::view_flags::present) && !csy::view_present(s.h, sl, v);
    }
    const bool gen = s.ok && s.h.generation != g0 && s.h.generation == MaskGen(p) && s.h.atlasW == 24 + 2 * 48 &&
                     s.h.atlasH == 48;
    cmask::sym::g_capPeri = csy::default_capacity_periphery, cmask::sym::g_capFocus = csy::default_capacity_focus;
    for (int f = 0; f < 6; ++f) Frame(p, dt++);
    const SymRec s2 = ReadSym(p);
    Check(over == 4 && gen && s2.h.atlasW == 2560 && s2.h.generation != s.h.generation,
          "R95 overflow: the symbology's extent above the capacity: present 0 and view_flags::overflow in every view; "
          "the capacity change made a new generation (the mask's too), and back");
  }
  // The coverage alpha's validity: DCS's alpha before the symbols not 0 in view 0 (the F-4E case) clears
  // emission_flags::alpha_valid after its agreeing read-backs (not at the first one); E.a is 0 at such texels.
  {
    std::vector<uint16_t> px(size_t(kOW[0]) * kOH[0] * 4);
    r.c->CopyResource(r.hdrStage[0], r.scene[0]);
    D3D11_MAPPED_SUBRESOURCE m{};
    if (SUCCEEDED(r.c->Map(r.hdrStage[0], 0, D3D11_MAP_READ, 0, &m))) {
      for (UINT y = 0; y < kOH[0]; ++y) memcpy(&px[size_t(y) * kOW[0] * 4], static_cast<uint8_t*>(m.pData) + y * m.RowPitch, kOW[0] * 8);
      r.c->Unmap(r.hdrStage[0], 0);
    }
    std::vector<uint16_t> a4 = px;
    for (size_t i = 0; i < a4.size(); i += 4) a4[i + 3] = F2H(0.4f);
    ID3D11Texture2D* keep = r.scene[0];
    ID3D11Texture2D* alt = nullptr;
    D3D11_TEXTURE2D_DESC td{};
    keep->GetDesc(&td);
    D3D11_SUBRESOURCE_DATA sd{a4.data(), kOW[0] * 8, 0};
    r.dev->CreateTexture2D(&td, &sd, &alt);
    r.scene[0] = alt;
    Frame(p, dt++);
    Frame(p, dt++);
    Frame(p, dt++);
    const bool stillOn = (ReadSym(p).h.emissionFlags & csy::emission_flags::alpha_valid) != 0;  // hysteresis
    for (int f = 0; f < 10; ++f) Frame(p, dt++);
    const int64_t d = dt;
    Frame(p, dt++);
    const SymRec s = ReadSym(p);
    const int sl = s.ok ? csy::find_slot(s.h, d) : -1;
    const uint32_t f0 = sl >= 0 ? s.h.slot[sl].view[0].flags : 0;
    const bool off = sl >= 0 && !(s.h.emissionFlags & csy::emission_flags::alpha_valid);
    Atl a;
    float maxA = -1;
    if (sl >= 0 && ReadAtlas(p, s.h, sl, a)) {
      const csy::EmissionView& v0 = s.h.slot[sl].view[0];
      maxA = 0;
      for (uint32_t j = 0; j < v0.winH; ++j)
        for (uint32_t i = 0; i < v0.winW; ++i) maxA = std::max(maxA, a.E[(size_t(v0.y + j) * a.W + v0.x + i) * 4 + 3]);
    }
    r.scene[0] = keep;
    Rel(alt);
    for (int f = 0; f < 14; ++f) Frame(p, dt++);
    const SymRec s2 = ReadSym(p);
    const bool latched = s2.ok && !(s2.h.emissionFlags & csy::emission_flags::alpha_valid) && cmask::sym::g_alphaHold.latched;
    // A new generation (a capacity change there and back) clears the latch: valid again after its run.
    const uint32_t gA = MaskGen(p);
    cmask::sym::g_capPeri = 500;
    for (int f = 0; f < 4; ++f) Frame(p, dt++);
    cmask::sym::g_capPeri = csy::default_capacity_periphery;
    for (int f = 0; f < 18; ++f) Frame(p, dt++);
    const SymRec s3 = ReadSym(p);
    const bool back = s3.ok && s3.h.generation != gA && (s3.h.emissionFlags & csy::emission_flags::alpha_valid);
    char what[500];
    sprintf_s(what, "%s alpha: DCS's alpha before the symbology not 0 (the F-4E case): emission_flags::alpha_valid "
                    "cleared at the first invalid complete still read-back (set 3 frames in: %d, a read-back lag; off 13 "
                    "frames in: %d), E.a 0 at those texels (max %.3g); R97: latched off for the generation with the "
                    "alpha before 0 again (%d); set again in a new generation (%d)",
              tag, stillOn, off, maxA, latched, back);
    Check((f0 & csy::view_flags::present) && off && maxA == 0.0f && latched && back, what);
  }
  // Cost (this device): the export's render-thread time per frame and its GPU copies / dispatches.
  // Steady state: the averages reset after a warm-up; 120 frames of four views (HUD runs in all, helmet in 1 / 3).
  {
    for (int f = 0; f < 30; ++f) Frame(p, dt++);
    cmask::sym::StatsText();  // (resets the averages)
    const uint64_t scans0 = cmask::sym::g_scans.load();
    for (int f = 0; f < 120; ++f) Frame(p, dt++);
    std::string cost;
    const std::string st = cmask::sym::StatsText(&cost);
    printf("     (%s steady state, 120 frames (%llu scans): %s)\n     (%s)\n", tag,
           static_cast<unsigned long long>(cmask::sym::g_scans.load() - scans0), st.c_str(), cost.c_str());
  }
  // ---- Lifecycle (the R85 / R86 / R88 / R91 rules, as the other companions) ----
  p.flight = true;
  cmask::g_flightGate = true;
  for (int f = 0; f < 4; ++f) Frame(p, dt++);
  const uint64_t tc0 = cmask::g_texCreates.load();
  const SetRec g0 = Rec(p);
  // L1 SymbologyExport off / on: held (the same generation and handles; inactive while off; nothing made).
  {
    configure(true, false, 2);
    tick();
    const SetRec off0 = Rec(p);
    for (int f = 0; f < 4; ++f) Frame(p, dt++);
    const SetRec off1 = Rec(p);
    const bool maskOn = MaskGen(p) == g0.gen;
    configure(true, true, 2);
    tick();
    for (int f = 0; f < 4; ++f) Frame(p, dt++);
    const SetRec on1 = Rec(p);
    Check(IsActive(g0) && !IsActive(off0) && SameSet(off0, g0) && !IsActive(off1) && SameSet(off1, g0) && maskOn &&
              IsActive(on1) && SameSet(on1, g0) && cmask::g_texCreates.load() == tc0,
          "R95 held set: SymbologyExport off -> its record inactive with the same generation and handles (the mask "
          "goes on); on again -> the same generation and handles republished, no texture made (R85)");
  }
  // L2 the mask export off / on: everything held.
  {
    configure(false, true, 2);
    tick();
    const SetRec off = Rec(p);
    configure(true, true, 2);
    tick();
    for (int f = 0; f < 4; ++f) Frame(p, dt++);
    const SetRec on = Rec(p);
    Check(!IsActive(off) && SameSet(off, g0) && IsActive(on) && SameSet(on, g0) && cmask::g_texCreates.load() == tc0,
          "R95 held set: CockpitMaskExport off / on holds the symbology's set too (the same generation and handles)");
  }
  // L3 F10 (no flight frame, other sizes): held inactive, the same generation; flight with the old sizes: resumed.
  {
    p.flight = false;
    p.vpCut = 8;
    for (int f = 0; f < 3; ++f) Frame(p, dt++);
    const SetRec map = Rec(p);
    p.flight = true;
    p.vpCut = 0;
    for (int f = 0; f < 4; ++f) Frame(p, dt++);
    const SetRec back = Rec(p);
    Check(!IsActive(map) && SameSet(map, g0) && IsActive(back) && SameSet(back, g0) && cmask::g_texCreates.load() == tc0,
          "R95 F10 hold (R88): another layout without a flight frame holds the symbology's set inactive (the same "
          "generation and handles); flight again with the held layout republishes it");
  }
  // L4 a relayout in flight (another mask divisor): a new generation for every block, new symbology handles.
  SetRec g1;
  {
    configure(true, true, 4);
    for (int f = 0; f < 4; ++f) Frame(p, dt++);
    g1 = Rec(p);
    Check(IsActive(g1) && g1.gen != g0.gen && g1.gen == MaskGen(p) && g1.h[0] && g1.h[0] != g0.h[0],
          "R95 relayout (in flight): a new generation for the mask and the symbology, new NT handles");
  }
  // L5 RetireUnmapped: the export off (its block not mapped by us, a reader still maps it), then a release by a layout
  // change: R76's record in the symbology block too (the new generation, flags 0, no handles).
  {
    configure(true, false, 4);
    tick();
    configure(true, false, 2);
    for (int f = 0; f < 4; ++f) Frame(p, dt++);
    const SetRec ret = Rec(p);
    const uint32_t mg = MaskGen(p);
    Check(ret.gen == mg && ret.gen != g1.gen && ret.flags == 0 && ret.h[0] == 0 && ret.k[0] == 0 && ret.fence == 0,
          "R95 RetireUnmapped (R85 / R86): a release while the export is off writes R76's record into the symbology "
          "block a reader still maps (the new generation, flags 0, no handles)");
    configure(true, true, 2);
    tick();
    for (int f = 0; f < 4; ++f) Frame(p, dt++);
    const SetRec on = Rec(p);
    Check(IsActive(on) && on.gen == MaskGen(p) && on.h[0] != 0, "R95: on again after that: a set with the symbology "
                                                               "(a new generation)");
  }
  // L6 a removed device at the export switch: released, not held (a new generation, flags 0).
  {
    const SetRec before = Rec(p);
    cmask::g_testRemoved = true;
    configure(true, false, 2);
    tick();
    const SetRec rel = Rec(p);
    cmask::g_testRemoved = false;
    configure(true, true, 2);
    tick();
    for (int f = 0; f < 5; ++f) Frame(p, dt++);
    const SetRec on = Rec(p);
    Check(rel.gen != before.gen && rel.flags == 0 && rel.h[0] == 0 && IsActive(on) && on.gen != rel.gen,
          "R95 device removed (R86 V8): the export switch releases the set instead of holding it (R76's record: a new "
          "generation, flags 0); on again: a new set");
  }
  // L7 the stop (fixation_depth.h Shutdown's Uninstall(false)): R76's record, released.
  {
    const SetRec before = Rec(p);
    Uninstall(false);
    const SetRec stop = Rec(p);
    Check(stop.gen != before.gen && stop.gen != 0 && stop.flags == 0 && stop.h[0] == 0 && !cmask::g_sview.load() &&
              !cmask::g_r.haveSym,
          "R95 stop: the symbology block's last record carries a new generation (flags 0, no handles); textures released");
    cmask::CloseBuried(true);
  }
  // Clean up.
  cmask::sym::g_alphaOnRun = keepAlphaRun;
  cmask::sym::g_testRenderPoint = false;
  configure(false, false, 2);
  cmask::ConfigureSymbology(false, true);
  cpass::g_symWant = false;
  cpass::g_symPsReady = 0;
  cpass::g_obj.store(nullptr);
  cpass::g_rt = cpass::Rt{};
  ct::g_orig = keepLv;
  ct::g_on = false;
  ct::g_hooked = false;
  ct::g_testRecordAlways = false;
  cmask::g_xrHookWanted = true;
  cmask::g_flightGate = true;
  cmask::g_buildWaitMs = 0;
  shadowinst::g_device = oldDev;
  ptiming::g_orig = execSave;
  if (p.mblk) UnmapViewOfFile(p.mblk);
  if (p.sblk) UnmapViewOfFile(p.sblk);
  if (p.mmap) CloseHandle(p.mmap);
  if (p.smap) CloseHandle(p.smap);
  r.Release();
}
