// Binaural audio module tests (docs/BINAURAL_AUDIO.md 8.3): the hook checks against the real ed_sound.dll (when DCS is
// installed; read as an image resource, never executed) and against altered copies; a fake ed_sound image (audio_fake_ed.h)
// with the same call sites, driven through them end to end: install / restore, pass-through exactness, the null test
// (the mono tap re-panned with DCS's gains equals DCS's mix), the steady state equals the core's output, fades without
// steps, ownership policy, mono gain rule, kill switch, fallbacks, overruns, budget, config and Live reload, late head
// rotation, no allocation on the real-time threads, and a two-thread stress with hot reloads.
// QV_AUDIO_ONLY=1 runs these alone; QV_AUDIO_STRESS_SEC sets the stress length (default 20 s).
#pragma once

#include "audio_rig.h"  // the fake ed_sound image, the hooked and reference worlds, the frame / block drivers

namespace audiotest {

using namespace fakeed;

// The stand-in core's HRTF "sets": 48 bytes naming the set (the stand-in never reads them).
bool TestHrtf(const wchar_t* name, std::vector<uint8_t>* out) {
  if (!wcscmp(name, L"missing")) return false;
  char b[64] = {};
  snprintf(b, sizeof(b), "SET:%ls", name);
  out->assign(b, b + 48);
  return true;
}

// What a rig reset puts back besides the module: the stand-in core's refusals and slowness.
void ResetStandIn() {
  audiofake::g_refuse = false;
  audiofake::g_spinUs = 0;
}

// ---- 1. the real ed_sound.dll ----------------------------------------------------------------------------------------
const uint8_t* g_realBase = nullptr;
void* RealImports(const char* dll, const char* name) {
  const uint32_t slot = sigscan::ImportSlotRva(g_realBase, dll, name);
  void* v = nullptr;
  if (slot) memcpy(&v, g_realBase + slot, 8);
  return slot ? v : nullptr;
}

void RealEdSound() {
  const std::wstring path = dcsinstall::Bin() + L"ed_sound.dll";
  if (!dcsinstall::Found() || GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
    QvSkip("audio: real ed_sound.dll checks: DCS not installed\n");
    return;
  }
  HMODULE h = LoadLibraryExW(path.c_str(), nullptr, LOAD_LIBRARY_AS_IMAGE_RESOURCE);
  if (!h) {
    QvSkip("audio: real ed_sound.dll checks: could not map ed_sound.dll\n");
    return;
  }
  const uint8_t* base = sigscan::ImageBase(h);
  const IMAGE_NT_HEADERS64* nt = sigscan::NtHeaders(base);
  if (!nt || nt->FileHeader.TimeDateStamp != audsig::kTimestamp) {
    printf("     (installed ed_sound.dll 0x%08x is not the analysed build 0x%08x: the scan path is checked)\n",
           nt ? nt->FileHeader.TimeDateStamp : 0u, audsig::kTimestamp);
  }
  g_realBase = base;
  audhk::Resolved r;
  const bool ok = audhk::Resolve(base, audhk::Generated(), &RealImports, &r);
  if (!ok) printf("     real ed_sound.dll refused: %s\n", r.why);
  Check(ok, "audio hooks: the real ed_sound.dll passes every check (sites, function hashes, call targets, layouts, mutex, "
            "globals)");
  if (ok && nt->FileHeader.TimeDateStamp == audsig::kTimestamp) {
    auto rva = [&](const void* p) { return static_cast<uint32_t>(static_cast<const uint8_t*>(p) - base); };
    Check(r.analysedBuild && rva(r.site[0]) == 0x4d40 && rva(r.site[1]) == 0x1ba00 && rva(r.site[2]) == 0x1b1fb &&
              rva(r.site[3]) == 0x20183 && rva(reinterpret_cast<void*>(r.panner)) == 0x1c1c0 &&
              rva(reinterpret_cast<void*>(r.voiceRender)) == 0x165d0 && rva(reinterpret_cast<void*>(r.mix)) == 0x1b180,
          "audio hooks: the real build resolves to the hook spec's RVAs (P3a 0x4d40, P3b 0x1ba00, P4 0x1b1fb, P5 0x20183)");
    Check(rva(r.mutex) == 0x3ad90 && rva(const_cast<uint32_t*>(r.frameCounter)) == 0x3ad68 &&
              rva(const_cast<int32_t*>(r.engineState)) == 0x3ad70 && rva(r.ctxArray) == 0x3d370 &&
              rva(const_cast<int32_t*>(r.ctxCount)) == 0x3ad74,
          "audio hooks: the real build's mutex, frame counter, engine state and context array match the hook spec");
  }
  // A writable copy of the first 256 KB (headers, code, the globals), the rest reserved: alterations one at a time.
  const uint32_t size = nt ? nt->OptionalHeader.SizeOfImage : 0;
  uint8_t* copy = static_cast<uint8_t*>(VirtualAlloc(nullptr, size, MEM_RESERVE, PAGE_NOACCESS));
  if (!copy || !VirtualAlloc(copy, 0x40000, MEM_COMMIT, PAGE_READWRITE)) {
    QvSkip("audio: altered copies of ed_sound.dll: no memory\n");
    FreeLibrary(h);
    return;
  }
  memcpy(copy, base, 0x40000);
  g_realBase = copy;
  {
    audhk::Resolved rc;
    Check(audhk::Resolve(copy, audhk::Generated(), &RealImports, &rc), "audio hooks: an unaltered copy passes too");
  }
  struct Alt {
    const char* what;
    uint32_t at;
    uint8_t xorWith;
    const char* expect;  // in the refusal reason
  };
  const Alt alts[] = {
      {"P3a pattern byte", 0x4d31, 0x01, "P3a not found"},
      {"P3b pattern byte", 0x1b9fb, 0x01, "P3b not found"},
      {"P4 call target", 0x1b1fc, 0x10, "P4 not found"},
      {"P5 call target", 0x20184, 0x10, "P5 not found"},
      {"P5 overrun counter offset", 0x201c4, 0x04, "P5 not found"},
      {"panner per-channel store", 0x1c423 + 6, 0x08, "not found"},
      {"panner channel bound", 0x1c466 + 3, 0x04, "not found"},
      {"voice render kill flag offset", 0x165d6 + 2, 0x01, "not found"},
      {"voice render body byte", 0x16700, 0x01, "VoiceRender not found"},
      {"mix body byte", 0x1b1e3, 0x01, "not found"},
      {"SND_process lock call slot", 0x49b5, 0x08, "mutex not found"},
      {"SND_process frame counter increment", 0x4b29, 0x01, "not found"},
  };
  int refused = 0;
  for (const Alt& a : alts) {
    copy[a.at] ^= a.xorWith;
    audhk::Resolved ra;
    const bool acc = audhk::Resolve(copy, audhk::Generated(), &RealImports, &ra);
    copy[a.at] ^= a.xorWith;
    const bool good = !acc && strstr(ra.why, a.expect);
    if (!good) printf("     altered %s: %s (%s)\n", a.what, acc ? "ACCEPTED" : "refused", ra.why);
    refused += good ? 1 : 0;
  }
  Check(refused == static_cast<int>(sizeof(alts) / sizeof(alts[0])),
        "audio hooks: every altered copy of the real ed_sound.dll is refused with its reason (patterns, call targets, "
        "overrun counter, panner stores and bound, voice render, mix, mutex, frame counter)");
  {  // another build with the same code: found by pattern
    auto* cnt = reinterpret_cast<IMAGE_NT_HEADERS64*>(copy + reinterpret_cast<IMAGE_DOS_HEADER*>(copy)->e_lfanew);
    cnt->FileHeader.TimeDateStamp ^= 1;
    audhk::Resolved rs;
    const bool acc = audhk::Resolve(copy, audhk::Generated(), &RealImports, &rs);
    cnt->FileHeader.TimeDateStamp ^= 1;
    Check(acc && !rs.analysedBuild, "audio hooks: a build with another timestamp but the same code is found by pattern");
  }
  VirtualFree(copy, 0, MEM_RELEASE);
  FreeLibrary(h);
}

// ---- 2. the fake image: resolution, install, restore -----------------------------------------------------------------
void FakeResolveInstall() {
  Image im = Build(0x12345678u);
  FakeTable ft;
  MakeTable(im, 0x12345678u, kImageSize, &ft);
  g_lockExpect = im.base + kMutex;
  audhk::Resolved r;
  bool ok = audhk::Resolve(im.base, ft.t, &FakeImports, &r);
  if (!ok) printf("     fake refused: %s\n", r.why);
  Check(ok && r.analysedBuild && r.site[0] == im.base + 0x4d40 && r.site[3] == im.base + 0x20183 &&
            r.mutex == im.base + kMutex && r.frameCounter == reinterpret_cast<uint32_t*>(im.base + kFrameCounter) &&
            r.ctxArray == im.base + kCtxArray,
        "audio hooks: the fake image (ed_sound's byte sequences) resolves on the analysed-build path");
  FakeTable other;
  MakeTable(im, 0x1u, kImageSize, &other);
  ok = audhk::Resolve(im.base, other.t, &FakeImports, &r);
  Check(ok && !r.analysedBuild && r.site[2] == im.base + 0x1b1fb, "audio hooks: the fake image resolves by pattern as another build");
  FakeTable moved = ft;
  moved.locs[audsig::kP4].rva += 0x40;  // recorded RVA wrong: the pattern finds it
  moved.t.locs = moved.locs;
  moved.t.masks = moved.masks;
  ok = audhk::Resolve(im.base, moved.t, &FakeImports, &r);
  Check(ok && r.site[2] == im.base + 0x1b1fb, "audio hooks: a location whose recorded RVA moved is found by its pattern");
  // Refusals on the fake: a foreign call target, a non-ed::mutex lock.
  const std::vector<uint8_t> text(im.base + kTextBegin, im.base + kTextEnd);
  {
    DWORD old;
    VirtualProtect(im.base + kTextBegin, kTextEnd - kTextBegin, PAGE_EXECUTE_READWRITE, &old);
    int32_t rel;
    memcpy(&rel, im.base + 0x20184, 4);
    rel += 0x20;
    memcpy(im.base + 0x20184, &rel, 4);
    ok = audhk::Resolve(im.base, ft.t, &FakeImports, &r);
    Check(!ok && strstr(r.why, "P5 not found (the call goes to neither"), "audio hooks: a site calling something else is refused");
    rel -= 0x20;
    memcpy(im.base + 0x20184, &rel, 4);
    auto noLock = [](const char* dll, const char* name) -> void* {
      return strcmp(name, audhk::kLockName) ? FakeImports(dll, name) : reinterpret_cast<void*>(&FakeUnlock);
    };
    ok = audhk::Resolve(im.base, ft.t, noLock, &r);
    Check(!ok && strstr(r.why, "mutex not found"), "audio hooks: a lock call that is not edCore's ed::mutex::lock is refused");
    VirtualProtect(im.base + kTextBegin, kTextEnd - kTextBegin, PAGE_EXECUTE_READ, &old);
    Check(memcmp(text.data(), im.base + kTextBegin, text.size()) == 0, "audio hooks: refusals patch nothing");
  }
  // Thunks: install by hand, a second resolve recognises the earlier generation's thunks, restore is byte-exact.
  ok = audhk::Resolve(im.base, ft.t, &FakeImports, &r);
  void* const wr[4] = {reinterpret_cast<void*>(&audio::PanWrapA), reinterpret_cast<void*>(&audio::PanWrapB),
                       reinterpret_cast<void*>(&audio::RenderWrap), reinterpret_cast<void*>(&audio::MixWrap)};
  uint8_t* page = audhk::MakeThunks(im.base, r.expected, wr);
  bool patched = page != nullptr;
  for (int k = 0; k < 4 && patched; ++k) patched = audhk::WriteCall(r.site[k], audhk::ThunkOf(page, k), k < 2);
  bool targets = patched;
  for (int k = 0; k < 4; ++k) targets = targets && audhk::CallTarget(r.site[k]) == audhk::ThunkOf(page, k) &&
                                        audhk::ThunkOriginal(audhk::ThunkOf(page, k), k) == r.expected[k];
  Check(targets, "audio hooks: the four sites call the thunks, each thunk records the original function");
  audhk::Resolved r2;
  ok = audhk::Resolve(im.base, ft.t, &FakeImports, &r2);
  bool same = ok;
  for (int k = 0; k < 4; ++k) same = same && r2.expected[k] == r.expected[k] && r2.current[k] == audhk::ThunkOf(page, k);
  Check(same, "audio hooks: a payload generation after ours accepts our thunks as the sites' callers (hot reload)");
  for (int k = 0; k < 4; ++k) audhk::WriteCall(r.site[k], r.expected[k], k < 2);
  Check(memcmp(text.data(), im.base + kTextBegin, text.size()) == 0, "audio hooks: restore puts ed_sound's bytes back exactly");
  Free(im);
}

// ---- 3. pure parts: mono gain rule, pan-law check, tag ---------------------------------------------------------------
void MonoGain() {
  uint64_t rng = 12345;
  auto rnd = [&]() {
    rng = rng * 6364136223846793005ull + 1442695040888963407ull;
    return static_cast<double>(rng >> 11) / 9007199254740992.0;
  };
  const double spL[3] = {0.5, 0, -0.8660254037844386}, spR[3] = {0.5, 0, 0.8660254037844386};
  int n = 0, bad = 0, nMed = 0, badMed = 0, mism = 0, corrupted = 0;
  double worst = 0, worstMed = 0;
  for (int i = 0; i < 20000; ++i) {
    double u[3] = {rnd() * 2 - 1, rnd() * 2 - 1, rnd() * 2 - 1};
    const double l = std::sqrt(u[0] * u[0] + u[1] * u[1] + u[2] * u[2]);
    if (l < 1e-3) continue;
    for (double& c : u) c /= l;
    const double base = 1e-3 + rnd() * 5, x = rnd();
    const double cL = u[0] * spL[0] + u[2] * spL[2], cR = u[0] * spR[0] + u[2] * spR[2];
    float gL, gR;
    audv::PanLaw(base, x, cL, cR, &gL, &gR);
    const double want = (1.5 * (0.4 - 0.35 * x) + 0.2 + 0.7 * x) * base;
    if (std::fabs(cL - cR) >= audv::kMedianThreshold) {
      const audv::MonoSolve s = audv::SolveMono(gL, gR, cL, cR, base, 0.5);
      ++n;
      const double e = std::fabs(s.m - want) / want;
      worst = std::max(worst, e);
      if (!s.ok || e > 1e-5) ++bad;
      // Corrupted pan law: one channel 10 % off.
      const audv::MonoSolve c = audv::SolveMono(gL * 1.1f, gR, cL, cR, base, 0.5);
      ++corrupted;
      if (!c.ok) ++mism;
    } else {
      // Median plane: k from an exact solve one frame earlier (the same voice a little off the plane).
      const double cL0 = cL + 0.06, cR0 = cR - 0.06;
      float gL0, gR0;
      audv::PanLaw(base, x, cL0, cR0, &gL0, &gR0);
      const audv::MonoSolve s0 = audv::SolveMono(gL0, gR0, cL0, cR0, 0.0, 0.5);
      const double k = s0.ok && s0.wa > 0 ? s0.wb / s0.wa : 0.5;
      const audv::MonoSolve s = audv::SolveMono(gL, gR, cL, cR, 0.0, k);  // without base: the earlier frame's k
      const audv::MonoSolve sb = audv::SolveMono(gL, gR, cL, cR, base, 0.5);  // with base: exact
      if (!sb.ok || std::fabs(sb.m - want) / want > 1e-5) ++badMed;
      ++nMed;
      const double e = std::fabs(s.m - want) / want;
      worstMed = std::max(worstMed, e);
      if (!s.ok || e > 1e-3) ++badMed;
    }
  }
  printf("     mono gain: %d lateral cases worst %.2e, %d median-plane cases worst %.2e; corrupted %d/%d detected\n", n, worst,
         nMed, worstMed, mism, corrupted);
  Check(n > 10000 && bad == 0, "audio mono gain: DCS's frontal gain is solved from its two gains within 1e-5 (random "
                               "directions, crosstalk 0..1, gains)");
  Check(nMed > 50 && badMed == 0, "audio mono gain: near the median plane, solved through the sound's base gain within "
                                  "1e-5 (and without it, from one prior frame's ratio, within 1e-3)");
  Check(mism * 100 >= corrupted * 99, "audio mono gain: a pan law 10 % off on one channel is detected (mismatch)");
  {  // The speed factor as ed_sound computes it.
    const double v0[3] = {0, 0, 0}, v100[3] = {100, 0, 0}, v300[3] = {0, 300, 0};
    Check(audv::SpeedFactor(1.0f, v100) == 1.0f && audv::SpeedFactor(0.4f, v0) == 1.0f &&
              audv::SpeedFactor(0.4f, v300) == 0.4f && std::fabs(audv::SpeedFactor(0.4f, v100) - 0.7f) < 1e-6f,
          "audio mono gain: the speed factor (1 at rest or when unset, s at 200 m/s and over, linear between)");
  }
  const float tag[4] = {0.3f, 0.0f, 0.3f, 0.0f}, stock[4] = {0.3f, 0.0f, 0.0f, 0.2f}, zero[4] = {};
  Check(audv::IsTag(tag) && !audv::IsTag(stock) && !audv::IsTag(zero),
        "audio tag: the mono tag is told from DCS's stereo gains (and from silence) by its bytes");
}

// ---- 4. ownership policy (simulation side alone, no image) -----------------------------------------------------------
struct SimRig {
  audv::MetaTable meta;
  audr::Counters cnt;
  audr::EvRing ev;
  audsim::Sim* sim = nullptr;
  std::vector<uint8_t> ctxMem;
  World w;
  audsim::Cfg cfg;
  audsim::Env env;
  uint32_t frame = 100;
  SimRig() : ctxMem(kImageSize, 0) {
    sim = new audsim::Sim();
    sim->meta = &meta;
    sim->cnt = &cnt;
    sim->ev = &ev;
    sim->Reset();
    w.Init();
    cfg.on = true;
    cfg.limit = 3;
    env.boundRenderer = w.Renderer();
    env.ctxArray = ctxMem.data() + kCtxArray;
    env.ctxCount = 8;
    env.freq = 1000;
    env.now = 1000;
  }
  ~SimRig() {
    w.Destroy();
    delete sim;
  }
  int Add(int ctx, float gain, double x, double y, double z, uint32_t ch = 1, bool is3d = true) {
    const int i = w.Add(ctxMem.data(), ctx, is3d, ch, kSine, 0.5f, 440, gain);
    w.SetPos(i, x, y, z);
    return i;
  }
  // A frame: DCS's panner then ours for every voice, at time now (ms).
  void Frame(int64_t nowMs) {
    env.now = nowMs;
    ++frame;
    audsim::BeginFrame(*sim, cfg, env, frame, false);
    for (size_t i = 0; i < w.voices.size(); ++i) {
      FakePanner(w.voices[i]);
      audsim::PanVoice(*sim, cfg, env, w.voices[i], false);
    }
  }
  bool Owned(int i) {
    audv::Rec* r = sim->own.Find(w.voices[i]);
    return r && r->owned;
  }
  bool Tagged(int i) {
    float g[4];
    memcpy(g, w.voices[i] + 0x48, 16);
    return audv::IsTag(g);
  }
  // The render side's acknowledgement of every released slot.
  void AckReleased() {
    for (int s = 0; s < audv::kSlots; ++s)
      if (sim->own.slotState[s] == audv::kSlotReleasing) meta.ackGen[s].store(sim->own.slotGen[s]);
  }
};

void Ownership() {
  {
    SimRig r;
    const int a = r.Add(0, 1.0f, 10, 0, 3), b = r.Add(0, 0.5f, 10, 0, -3), c = r.Add(0, 0.25f, -10, 2, 0),
              d = r.Add(0, 0.125f, 5, 5, 5), e = r.Add(0, 0.0625f, 0, 0, 10);
    r.Frame(1000);
    r.Frame(1011);
    Check(r.Owned(a) && r.Owned(b) && r.Owned(c) && !r.Owned(d) && !r.Owned(e) && r.Tagged(a) && !r.Tagged(d),
          "audio ownership: the loudest MaxObjects voices are owned and tagged, the others keep DCS's panning");
    // d becomes 2 dB louder than c: no swap; 4 dB louder but c owned for less than 250 ms: no swap.
    W<float>(r.w.sounds[d], 0x1c0, 0.25f * 1.26f);
    r.Frame(1022);
    r.Frame(1033);
    const bool noSwap2dB = r.Owned(c) && !r.Owned(d);
    W<float>(r.w.sounds[d], 0x1c0, 0.25f * 1.6f);
    r.sim->own.Find(r.w.voices[c])->ownedSince = 1100;  // owned 100 ms ago at t = 1200
    r.Frame(1190);
    r.Frame(1200);
    const bool held = r.Owned(c) && !r.Owned(d);
    r.Frame(1400);
    r.Frame(1411);
    const bool swapped = !r.Owned(c) && r.Owned(d) && !r.Tagged(c) && r.Tagged(d);
    Check(noSwap2dB && held && swapped,
          "audio ownership: hysteresis (no swap for a 2 dB lead, none before 250 ms, a 3 dB louder voice replaces the "
          "quietest owned one after)");
    // The slot of a released voice is not reused before the render side acknowledges it.
    int releasing = 0;
    for (int s = 0; s < audv::kSlots; ++s) releasing += r.sim->own.slotState[s] == audv::kSlotReleasing;
    r.AckReleased();
    r.Frame(1422);
    int releasing2 = 0;
    for (int s = 0; s < audv::kSlots; ++s) releasing2 += r.sim->own.slotState[s] == audv::kSlotReleasing;
    Check(releasing == 1 && releasing2 == 0, "audio ownership: a released slot waits for the render side's acknowledgement");
    // The limit falls (budget back-off): the quietest owned voices go first.
    r.cfg.limit = 1;
    r.Frame(1433);
    Check(r.Owned(a) && !r.Owned(b) && !r.Owned(d), "audio ownership: a lower limit releases the quietest owned voices first");
    r.cfg.on = false;
    r.Frame(1444);
    Check(!r.Owned(a) && r.sim->own.owned == 0, "audio ownership: off releases every voice");
  }
  {
    SimRig r;
    r.cfg.limit = 16;
    r.cfg.excludeMask = 1u << 3;
    const_cast<uint8_t*>(r.env.ctxArray)[0x388 * 7 + 0xe0] = 1;  // context 7 is 2D (radio, intercom)
    const int ok = r.Add(0, 1, 10, 0, 0);
    const int twoD = r.Add(0, 1, 10, 0, 0, 1, false);
    const int radio = r.Add(7, 1, 10, 0, 0);
    const int stereo = r.Add(0, 1, 10, 0, 0, 2);
    const int excluded = r.Add(3, 1, 10, 0, 0);
    const int nearV = r.Add(0, 1, 0.03, 0, 0);
    const int other = r.Add(0, 1, 10, 0, 0);
    W<void*>(r.w.voices[other], 0x08, r.w.wasapi + 0x100);  // another renderer (Windows spatial, offline)
    r.Frame(1000);
    r.Frame(1011);
    Check(r.Owned(ok) && !r.Owned(twoD) && !r.Owned(radio) && !r.Owned(stereo) && !r.Owned(excluded) &&
              !r.Owned(nearV) && !r.Owned(other) && !r.Tagged(radio) && !r.Tagged(twoD) && !r.Tagged(stereo),
          "audio ownership: 2D sounds, 2D contexts (radio, intercom), stereo waves, excluded contexts, sources under 5 cm "
          "and other renderers are never owned or tagged");
    r.env.engineState = 2;
    r.Frame(1022);
    r.Frame(1033);
    Check(!r.Owned(ok), "audio ownership: nothing is owned while the offline renderer (AVIMaker) runs");
  }
  {
    SimRig r;
    r.cfg.limit = 4;
    const int a = r.Add(0, 1, 10, 0, 2);
    r.Frame(1000);
    r.Frame(1011);
    // Voice creation (P3b): owned and tagged at once.
    const int n = r.Add(0, 0.5f, 3, 0, -3);
    FakePanner(r.w.voices[n]);
    audsim::PanVoice(*r.sim, r.cfg, r.env, r.w.voices[n], true);
    audv::MetaData md{};
    int slot = -1;
    for (int s = 0; s < audv::kSlots; ++s)
      if (r.meta.voice[s].load() == r.w.voices[n]) slot = s;
    const bool metaOk = slot >= 0 && audv::MetaRead(r.meta.slot[slot], &md);
    Check(r.Owned(n) && r.Tagged(n) && metaOk && (md.flags & audv::meta_flags::kInitial) && md.voice == r.w.voices[n] &&
              std::fabs(md.pos[0] - 3) < 1e-6 && std::fabs(md.pos[2] + 3) < 1e-6 && r.Owned(a),
          "audio ownership: a voice created this frame (P3b) is owned at once, tagged and published with its position");
    // A pooled voice reused for another sound: a new generation.
    const uint32_t gen0 = md.gen;
    W<void*>(r.w.voices[n], 0x10, r.w.sounds[a]);  // now plays another sound
    FakePanner(r.w.voices[n]);
    audsim::PanVoice(*r.sim, r.cfg, r.env, r.w.voices[n], true);
    int slot2 = -1;
    for (int s = 0; s < audv::kSlots; ++s)
      if (r.meta.voice[s].load() == r.w.voices[n]) slot2 = s;
    audv::MetaData md2{};
    Check(slot2 >= 0 && audv::MetaRead(r.meta.slot[slot2], &md2) && md2.gen != gen0,
          "audio ownership: a reused voice gets a new generation (the render side resets its state)");
  }
}

// ---- 5. end to end through the fake image ----------------------------------------------------------------------------
const char* kIniOn =
    "[Audio]\r\nBinaural=1\r\nHrtf=default\r\nMaxObjects=16\r\nLateHeadTracking=0\r\nExcludeContexts=3\r\n";

void SetContext2D(Rig& r, int ctx, bool is2d) { W<uint8_t>(r.im.base + kCtxArray + 0x388 * ctx, 0xe0, is2d ? 1 : 0); }

// Pass-through exactness: nothing owned (only voices that never qualify, or Binaural=0) = DCS's mix bit for bit.
void PassThrough() {
  for (int mode = 0; mode < 2; ++mode) {
    Rig r;
    RigUp(r, mode == 0 ? "[Audio]\r\nBinaural=0\r\n" : kIniOn);
    SetContext2D(r, 7, true);
    AddBoth(r, 7, true, 1, kNoise, 0.5f, 0, 1.0f, 11);  // radio: a 3D sound in a 2D context
    AddBoth(r, 0, false, 1, kSine, 0.4f, 330, 0.8f);    // a 2D sound
    AddBoth(r, 0, true, 2, kSine, 0.3f, 550, 0.7f);     // a stereo wave
    AddBoth(r, 3, true, 1, kNoise, 0.3f, 0, 0.6f, 5);   // excluded context
    AddBoth(r, 0, true, 1, kSine, 0.3f, 220, 0.9f);     // under 5 cm
    PosBoth(r, 4, 0.02, 0, 0);
    for (int i = 0; i < 5; ++i) CreateVoice(r, i);
    Bind(r);
    std::vector<float> a(2 * 2048), b(2 * 2048);
    uint64_t rng = 99;
    int diffs = 0, blocks = 0;
    for (int blk = 0; blk < 10000; ++blk) {
      rng = rng * 6364136223846793005ull + 1;
      const uint32_t frames = 1 + static_cast<uint32_t>((rng >> 33) % 2048);
      if (blk % 2 == 0) {
        for (int i = 0; i < 4; ++i) PosBoth(r, i, 10 * std::cos(blk * 0.01 + i), 1, 10 * std::sin(blk * 0.01 + i));
        SimFrame(r);
      }
      Block(r, a.data(), b.data(), frames);
      if (memcmp(a.data(), b.data(), sizeof(float) * 2 * frames)) ++diffs;
      ++blocks;
    }
    const bool installed = audio::g_installed.load();
    printf("     pass-through (%s): %d blocks, %d differ; hooks %s, armed blocks %llu\n", mode ? "Binaural=1" : "Binaural=0",
           blocks, diffs, installed ? "in" : "out", static_cast<unsigned long long>(audio::g_cnt.armedBlocks.load()));
    if (mode == 0)
      Check(diffs == 0 && !installed, "audio pass-through: Binaural=0 leaves ed_sound untouched and the mix bit-identical");
    else
      Check(diffs == 0 && installed && audio::g_cnt.armedBlocks.load() > 9000 && audio::g_cnt.ownedNow.load() == 0,
            "audio pass-through: with binaural on, the radio (2D context), 2D sounds, stereo waves, excluded contexts and "
            "sources under 5 cm stay DCS's own: 10,000 random blocks bit-identical to the unhooked mix");
    RigDown(r);
  }
}

// A rig with owned voices.
void OwnedRig(Rig& r, const char* ini, bool sine = true) {
  RigUp(r, ini);
  SetContext2D(r, 7, true);
  AddBoth(r, 0, true, 1, sine ? kSine : kNoise, 0.5f, 440, 1.0f, 3);
  for (World* w : {&r.w, &r.ref}) {  // a doppler speed factor on the first source (base = 0.85 x gain)
    W<float>(w->sounds[0], 0x178, 0.5f);
    W<double>(w->sounds[0], 0x108, 60.0);
  }
  AddBoth(r, 0, true, 1, kSine, 0.4f, 97, 0.6f);
  AddBoth(r, 1, true, 1, kNoise, 0.25f, 0, 0.9f, 7);
  AddBoth(r, 7, true, 1, kSine, 0.3f, 1200, 1.0f);  // radio
  PosBoth(r, 0, 5, 1, 4);
  PosBoth(r, 1, -3, 0, -6);
  PosBoth(r, 2, 0.4, -0.2, 0.3);  // cockpit distance
  PosBoth(r, 3, 1, 0, 0);
  for (int i = 0; i < 4; ++i) CreateVoice(r, i);
  Bind(r);
}

double ErrDb(const float* a, const float* b, size_t n, double* refEnergy = nullptr) {
  double e = 0, s = 0;
  for (size_t i = 0; i < n; ++i) {
    const double d = static_cast<double>(a[i]) - b[i];
    e += d * d;
    s += static_cast<double>(b[i]) * b[i];
  }
  if (refEnergy) *refEnergy = s;
  return s > 0 ? 10 * std::log10(e / s + 1e-300) : -999;
}

// The null test: owned voices with the core refusing every call: the mono tap re-panned with DCS's own gains.
void NullTest() {
  Rig r;
  OwnedRig(r, kIniOn);
  audiofake::g_refuse = true;
  std::vector<float> a(2 * 1024), b(2 * 1024), allA, allB;
  for (int blk = 0; blk < 3000; ++blk) {
    const uint32_t frames = 256 + (blk * 37) % 769;
    if (blk % 2 == 0) {
      const double t = blk * 0.004;  // a slow orbit, and the cockpit source moving around the head
      PosBoth(r, 0, 5 * std::cos(t), 1, 5 * std::sin(t));
      PosBoth(r, 2, 0.4 * std::cos(3 * t), -0.2, 0.4 * std::sin(3 * t));
      SimFrame(r);
    }
    Block(r, a.data(), b.data(), frames);
    if (blk >= 20) {
      allA.insert(allA.end(), a.begin(), a.begin() + 2 * frames);
      allB.insert(allB.end(), b.begin(), b.begin() + 2 * frames);
    }
  }
  const double db = ErrDb(allA.data(), allB.data(), allA.size());
  printf("     null test: owned %u voices, refused %llu, re-panned %llu; error vs DCS's mix %.1f dB\n",
         audio::g_cnt.ownedNow.load(), static_cast<unsigned long long>(audio::g_cnt.coreRejected.load()),
         static_cast<unsigned long long>(audio::g_cnt.repanCalls.load()), db);
  Check(audio::g_cnt.ownedNow.load() == 3 && audio::g_cnt.repanCalls.load() > 1000 && db < -60.0,
        "audio null test: the owned voices' mono tap re-panned with DCS's own gains equals DCS's mix within -60 dB "
        "(moving sources, cockpit distance, random block sizes)");
  RigDown(r);
}

// Steady state: the output is the core's output; its mono input is DCS's signal at DCS's frontal gain.
void SteadyState() {
  Rig r;
  RigUp(r, kIniOn);
  AddBoth(r, 0, true, 1, kSine, 0.5f, 440, 0.8f);
  PosBoth(r, 0, 3, 0, 4);  // 53 deg right
  W<float>(r.w.sounds[0], kSndCrosstalk, 0.3f);
  W<float>(r.ref.sounds[0], kSndCrosstalk, 0.3f);
  CreateVoice(r, 0);
  Bind(r);
  std::vector<float> a(2 * 480), b(2 * 480);
  for (int i = 0; i < 8; ++i) {
    SimFrame(r);
    Block(r, a.data(), b.data(), 480);
  }
  bool equal = audiofake::g_lastFrames == 480;
  for (int i = 0; i < 960; ++i) equal = equal && a[i] == audiofake::g_lastAdded[i];
  // mono input = stock left / gL * m, m = DCS's gain with the source in front
  const double x = 0.3, base = 0.8;
  const double m = (1.5 * (0.4 - 0.35 * x) + 0.2 + 0.7 * x) * base;
  float gL, gR;
  const double cL = 0.6 * 0.5 + 0.8 * -0.8660254037844386, cR = 0.6 * 0.5 + 0.8 * 0.8660254037844386;
  audv::PanLaw(base, x, cL, cR, &gL, &gR);  // c = (direction to the source) . speaker
  double worst = 0, peak = 0;
  for (int i = 0; i < 480; ++i) {
    const double want = b[2 * i] / gL * m;
    worst = std::max(worst, std::fabs(audiofake::g_lastMono[i] - want));
    peak = std::max(peak, std::fabs(want));
  }
  float pl, pr;
  const float pos[3] = {3, 0, 4};
  binaural::FakePan(pos, &pl, &pr);
  double ratio = 0;
  for (int i = 0; i < 480; ++i)
    if (std::fabs(a[2 * i]) > 1e-3) ratio = a[2 * i + 1] / a[2 * i];
  printf("     steady: mono vs DCS-at-front worst %.2e of %.3f; core pan R/L %.4f (expected %.4f)\n", worst, peak, ratio, pr / pl);
  Check(equal, "audio steady state: an owned voice's output is exactly the core's output (nothing else added)");
  Check(peak > 0.1 && worst <= 2e-6 * peak + 1e-7,
        "audio steady state: the core's mono input is DCS's own signal at DCS's frontal gain (distance, doppler, "
        "filters kept; only the panning cancelled)");
  Check(std::fabs(ratio - pr / pl) < 1e-4, "audio steady state: the core gets the source's listener-frame position");
  RigDown(r);
}

// Fades: ownership in and out, kill switch and fallback, on a 0 dBFS DC source: no step above -60 dBFS.
void Fades() {
  Rig r;
  RigUp(r, kIniOn);
  AddBoth(r, 0, true, 1, kDc, 1.0f, 0, 1.0f);
  PosBoth(r, 0, 0, 0, 10);  // hard right: DCS's pan and the core's differ most
  audio::g_userOff = true;  // start as DCS's
  CreateVoice(r, 0);
  Bind(r);
  const uint32_t frames = 2048;
  std::vector<float> a(2 * frames), b(2 * frames), all;
  auto run = [&](int n) {
    for (int k = 0; k < n; ++k) {
      SimFrame(r);
      Block(r, a.data(), b.data(), frames);
      all.insert(all.end(), a.begin(), a.end());
    }
  };
  run(3);
  audio::g_userOff = false;  // owned: fade in
  run(4);
  audio::g_userOff = true;  // kill switch: released, faded back to DCS's panning
  run(4);
  audio::g_userOff = false;  // owned again
  run(4);
  audio::g_offReason = audio::kOffOverruns;  // off for the session, decided on the render side first (no sim frame)
  for (int k = 0; k < 3; ++k) {
    Block(r, a.data(), b.data(), frames);
    all.insert(all.end(), a.begin(), a.end());
  }
  run(3);
  double maxStep = 0;
  for (size_t i = 2; i < all.size(); ++i) maxStep = std::max(maxStep, static_cast<double>(std::fabs(all[i] - all[i - 2])));
  printf("     fades: largest sample-to-sample change %.2e (%.1f dBFS); fade-ins %llu, fade-outs %llu\n", maxStep,
         Db(maxStep), static_cast<unsigned long long>(audio::g_cnt.fadeIns.load()),
         static_cast<unsigned long long>(audio::g_cnt.fadeOuts.load()));
  Check(audio::g_cnt.fadeIns.load() >= 1 && audio::g_cnt.fadeOuts.load() >= 1 && maxStep < 1e-3,
        "audio fades: ownership in and out, the kill switch and an off-for-the-session on the render side change the "
        "output with no step above "
        "-60 dBFS (0 dBFS DC, 2048-frame blocks)");
  RigDown(r);
  // A voice owned from its creation: its first block is already the core's (no stock onset).
  Rig q;
  RigUp(q, kIniOn);
  AddBoth(q, 0, true, 1, kSine, 0.5f, 440, 1.0f);
  Bind(q);
  PosBoth(q, 0, 0, 0, 5);
  CreateVoice(q, 0);
  std::vector<float> c(2 * 480), d(2 * 480);
  Block(q, c.data(), d.data(), 480);
  bool sameAsCore = true;  // value compare: 0 + (-0) is +0
  for (int i = 0; i < 960; ++i) sameAsCore = sameAsCore && c[i] == audiofake::g_lastAdded[i];
  const bool onset = audio::g_cnt.fadeIns.load() == 0 && audiofake::g_lastFrames == 480 && sameAsCore;
  if (!onset)
    printf("     onset: fadeIns %llu, hrtf %llu, owned %u, lastFrames %u, candidates %llu, steady %llu, pass %llu\n",
           static_cast<unsigned long long>(audio::g_cnt.fadeIns.load()), static_cast<unsigned long long>(audio::g_cnt.hrtfCalls.load()),
           audio::g_cnt.ownedNow.load(), audiofake::g_lastFrames, static_cast<unsigned long long>(audio::g_cnt.candidateCalls.load()),
           static_cast<unsigned long long>(audio::g_cnt.steady.load()), static_cast<unsigned long long>(audio::g_cnt.passCandidates.load()));
  Check(onset, "audio onset: a voice owned at its creation (P3b) is binaural from its first sample");
  RigDown(q);
}

// Kill switch flag, ED overruns, offline path, budget.
void SwitchesAndFallbacks() {
  {
    Rig r;
    OwnedRig(r, kIniOn);
    std::vector<float> a(2 * 480), b(2 * 480);
    for (int i = 0; i < 6; ++i) {
      SimFrame(r);
      Block(r, a.data(), b.data(), 480);
    }
    const bool ownedBefore = audio::g_cnt.ownedNow.load() == 3;
    FILE* f = nullptr;
    _wfopen_s(&f, (r.dir + L"toggle_audio.flag").c_str(), L"wb");
    if (f) fclose(f);
    audio::g_loop.lastFlag = 0;
    Tick();
    const bool flagGone = GetFileAttributesW((r.dir + L"toggle_audio.flag").c_str()) == INVALID_FILE_ATTRIBUTES;
    Block(r, a.data(), b.data(), 480);  // the block that ramps HRTF -> DCS's gains
    Block(r, a.data(), b.data(), 480);
    const double db1 = ErrDb(a.data(), b.data(), 960);
    for (int i = 0; i < 4; ++i) {
      SimFrame(r);
      Block(r, a.data(), b.data(), 480);
    }
    const bool identical = memcmp(a.data(), b.data(), sizeof(float) * 960) == 0;
    printf("     kill switch: the block after the switch %.1f dB from DCS's mix; later blocks %s\n", db1,
           identical ? "bit-identical" : "differ");
    Check(ownedBefore && flagGone && audio::g_userOff.load() && db1 < -60 && identical &&
              audio::g_cnt.ownedNow.load() == 0,
          "audio kill switch: toggle_audio.flag (deleted) turns binaural off; DCS's panning within one block, then "
          "bit-identical once the voices are released");
    audio::g_loop.lastFlag = 0;
    _wfopen_s(&f, (r.dir + L"toggle_audio.flag").c_str(), L"wb");
    if (f) fclose(f);
    Tick();
    for (int i = 0; i < 3; ++i) {
      SimFrame(r);
      Block(r, a.data(), b.data(), 480);
    }
    Check(!audio::g_userOff.load() && audio::g_cnt.ownedNow.load() == 3, "audio kill switch: the flag again turns it back on");
    // ED's overrun counter counts anybody's slow block: 3 overruns while our own work is a few percent of the block
    // (a system or DCS hitch) leave binaural on.
    for (int k = 0; k < 3; ++k) {
      W<uint32_t>(r.w.wasapi, 0x2274, R<uint32_t>(r.w.wasapi, 0x2274) + 1);
      SimFrame(r);
      Block(r, a.data(), b.data(), 480);
    }
    Tick();
    Check(audio::g_offReason.load() == audio::kOffNone && audio::g_cnt.ownedNow.load() == 3,
          "audio overruns: DCS's overruns while our work is a small share of the block are not held against binaural");
    // 3 increments within 10 s while our work is a real share of the block (3 voices x 0.6 ms of 10 ms = 18 %, half the
    // 25 % budget is the line): off for the session.
    audiofake::g_spinUs = 600;
    for (int k = 0; k < 3; ++k) {
      W<uint32_t>(r.w.wasapi, 0x2274, R<uint32_t>(r.w.wasapi, 0x2274) + 1);
      SimFrame(r);
      Block(r, a.data(), b.data(), 480);
    }
    audiofake::g_spinUs = 0;
    Block(r, a.data(), b.data(), 480);
    Block(r, a.data(), b.data(), 480);
    const double db2 = ErrDb(a.data(), b.data(), 960);
    Tick();
    printf("     overruns: off reason %d, then %.1f dB from DCS's mix\n", audio::g_offReason.load(), db2);
    Check(audio::g_offReason.load() == audio::kOffOverruns && db2 < -60 && (audr::g_status.load() & qvstatus::kAudioOff),
          "audio overruns: 3 of DCS's render overruns within 10 s with our work a real share of the block turn binaural "
          "off for the session (DCS's panning, status bit 55, an audio: off line)");
    // The kill switch off and on again re-arms it (the pilot asked for it); earlier overruns no longer count.
    for (int k = 0; k < 2; ++k) {
      audio::g_loop.lastFlag = 0;
      _wfopen_s(&f, (r.dir + L"toggle_audio.flag").c_str(), L"wb");
      if (f) fclose(f);
      Tick();
    }
    for (int i = 0; i < 4; ++i) {
      SimFrame(r);
      Block(r, a.data(), b.data(), 480);
    }
    Tick();
    Check(audio::g_offReason.load() == audio::kOffNone && !audio::g_userOff.load() && audio::g_cnt.ownedNow.load() == 3 &&
              audio::g_offText.empty() && !(audr::g_status.load() & qvstatus::kAudioOff),
          "audio overruns: the kill switch off and on again brings binaural back (an audio: on line, status bit 55 clear)");
    RigDown(r);
  }
  {  // P4 outside an armed P5 block (offline renderer, Windows spatial path): DCS's own call.
    Rig r;
    OwnedRig(r, kIniOn);
    std::vector<float> a(2 * 480), b(2 * 480);
    audio::g_userOff = true;
    for (int i = 0; i < 6; ++i) {
      SimFrame(r);
      Block(r, a.data(), b.data(), 480);
    }
    const uint64_t un0 = audio::g_cnt.unarmedCalls.load();
    memset(a.data(), 0, sizeof(float) * 960);
    memset(b.data(), 0, sizeof(float) * 960);
    reinterpret_cast<MixFn>(r.im.base + kMixFn)(r.w.Renderer(), a.data(), 480);
    FakeMixRef(r.ref.Renderer(), b.data(), 480);
    Check(audio::g_cnt.unarmedCalls.load() - un0 == 4 && memcmp(a.data(), b.data(), sizeof(float) * 960) == 0,
          "audio offline / spatial paths: voice renders outside an armed WASAPI block pass straight through");
    RigDown(r);
  }
  {  // Budget back-off (pure, synthetic clock).
    audr::Binding b;
    b.rate = 48000;
    audr::Counters c;
    audr::EvRing ev;
    const int64_t freq = 1000000;  // microseconds
    int64_t now = 1;
    auto second = [&](double usedFraction, int maxObjects) {
      for (int k = 0; k < 100; ++k) {
        now += 10000;
        audr::BudgetTick(b, c, ev, {25, maxObjects}, 480, static_cast<int64_t>(usedFraction * 10000), now, freq);
      }
    };
    second(0.4, 48);  // a window closes at the first block of the next second
    second(0.4, 48);
    const uint32_t l1 = std::min<uint32_t>(c.limitNow.load(), 48);
    for (int s = 0; s < 5; ++s) second(0.4, 48);
    const uint32_t lFloor = std::min<uint32_t>(c.limitNow.load(), 48);
    for (int s = 0; s < 11; ++s) second(0.05, 48);
    const uint32_t lUp = std::min<uint32_t>(c.limitNow.load(), 48);
    for (int s = 0; s < 200; ++s) second(0.05, 48);
    const uint32_t lBack = c.limitNow.load();
    int downs = 0, ups = 0;
    audr::Ev e;
    while (ev.Pop(&e)) downs += e.code == audr::kEvBudgetDown, ups += e.code == audr::kEvBudgetUp;
    printf("     budget: 48 -> %u after 1 s over, floor %u, %u after 10 s calm, cap %u at the end (%d down, %d up events)\n",
           l1, lFloor, lUp, lBack, downs, ups);
    Check(l1 == 40 && lFloor == 8 && lUp == 12 && lBack == binaural::kMaxSources && downs == 5 && ups >= 9,
          "audio budget: over budget in >10 % of a second's blocks drops the limit by 8 (not below 8); 10 s under half "
          "the budget climbs by 4, back to MaxObjects");
  }
  {  // An injected slow core lowers the limit through the real path.
    Rig r;
    OwnedRig(r, "[Audio]\r\nBinaural=1\r\nMaxObjects=16\r\nLateHeadTracking=0\r\nCpuBudgetPercent=5\r\n");
    audiofake::g_spinUs = 400;  // 3 voices x 0.4 ms against 5 % of a 10 ms block
    std::vector<float> a(2 * 480), b(2 * 480);
    const ULONGLONG t0 = GetTickCount64();
    while (GetTickCount64() - t0 < 1300) {
      SimFrame(r);
      Block(r, a.data(), b.data(), 480);
    }
    audiofake::g_spinUs = 0;
    Tick();
    Check(audio::g_cnt.limitNow.load() == 8, "audio budget: a slow core lowers the number of binaural sounds (16 -> 8)");
    RigDown(r);
  }
}

// Config: every key, clamps, Live reload into the module and the core.
void Config() {
  const std::wstring dir = TestDir(L"audio_cfg");
  WriteIni(dir,
           "[Audio]\r\nBinaural=1\r\nHrtf=my head 2\r\nHeadCircumferenceCm=60\r\nLateHeadTracking=0\r\nNearField=0\r\n"
           "MaxObjects=32\r\nLevelTrimDb=-3.5\r\nCpuBudgetPercent=40\r\nExcludeContexts=1, 5\r\nLogVoices=1\r\n"
           "[Hotkeys]\r\nAudioToggle=121:6\r\n");
  audcfg::Read r = audcfg::ReadIni((dir + L"DcsQvCull.ini").c_str());
  const binaural_config::AudioConfig& c = r.cfg;
  Check(c.binaural && !wcscmp(c.hrtf, L"my head 2") && c.headCircumferenceCm == 60.0f && !c.lateHeadTracking &&
            !c.nearField && c.maxObjects == 32 && c.levelTrimDb == -3.5f && c.cpuBudgetPercent == 40 &&
            c.excludeContextMask == ((1u << 1) | (1u << 5)) && c.logVoices && r.toggleVk == 121 && r.toggleMods == 6 &&
            r.noteCount == 0,
        "audio config: every [Audio] key and [Hotkeys] AudioToggle is read");
  WriteIni(dir, "[Audio]\r\nMaxObjects=200\r\nLevelTrimDb=-40\r\nHeadCircumferenceCm=abc\r\nHrtf=..\\evil\r\nExcludeContexts=9\r\n");
  r = audcfg::ReadIni((dir + L"DcsQvCull.ini").c_str());
  Check(!r.cfg.binaural && r.cfg.maxObjects == 64 && r.cfg.levelTrimDb == -12.0f && r.cfg.headCircumferenceCm == 57.0f &&
            !wcscmp(r.cfg.hrtf, L"default") && r.cfg.excludeContextMask == 0 && r.noteCount == 4 && r.cfg.lateHeadTracking &&
            r.cfg.nearField && r.cfg.cpuBudgetPercent == 25 && r.toggleVk == 0,
        "audio config: defaults, clamps, a path in Hrtf and a bad context list are refused (and noted)");
  // Every key of the contract's table is read by name in the module's reader.
  const std::string src = lptest::ReadAllSources();
  int missing = 0;
  for (const binaural_config::KeyInfo& k : binaural_config::kKeys) {
    char lit[96];
    snprintf(lit, sizeof(lit), "L\"%ls\"", k.name);
    if (src.find(lit) == std::string::npos) ++missing, printf("     [Audio] %ls not read\n", k.name);
  }
  Check(missing == 0 && src.size() > 100000, "audio config: every key of binaural_config.h's table is read by the module");
  // Live reload into a running module.
  Rig q;
  OwnedRig(q, kIniOn);
  std::vector<float> a(2 * 480), b(2 * 480);
  for (int i = 0; i < 4; ++i) {
    SimFrame(q);
    Block(q, a.data(), b.data(), 480);
  }
  const uint64_t hr0 = audiofake::g_setHrtf.load(), rad0 = audiofake::g_setRadius.load(), nf0 = audiofake::g_setNear.load();
  WriteIni(q.dir, "[Audio]\r\nBinaural=1\r\nHrtf=personal\r\nMaxObjects=8\r\nLateHeadTracking=0\r\nHeadCircumferenceCm=54\r\n"
                  "NearField=0\r\nLevelTrimDb=-6\r\n");
  Tick();
  const audio::Rt rt = audio::ReadRt();
  Check(audiofake::g_setHrtf.load() == hr0 + 1 && audiofake::g_setRadius.load() == rad0 + 1 &&
            audiofake::g_setNear.load() == nf0 + 1 && std::fabs(audiofake::g_lastRadius - 0.0875f * 54.0f / 57.0f) < 1e-5 &&
            std::fabs(audiofake::g_lastEarRadius - binaural_config::HeadRadiusFromCircumferenceCm(54.0f)) < 1e-6 &&
            std::fabs(rt.trim - 0.5011872f) < 1e-5 && !audio::g_cfgRead.cfg.nearField && rt.maxObjects == 8 &&
            audio::g_hrtfName == L"personal",
        "audio Live: a changed ini reaches the module at once (HRTF set crossfaded, head size, near field, trim, MaxObjects)");
  // Near field off is the core's own fade (Renderer::SetNearField, 20 ms): the module never cuts it per source.
  audiofake::g_lastFlags = 0xffffffffu;
  for (int i = 0; i < 3; ++i) {
    SimFrame(q);
    Block(q, a.data(), b.data(), 480);
  }
  Check(audiofake::g_lastFlags.load() != 0xffffffffu && !(audiofake::g_lastFlags.load() & binaural::source_flags::kNoNearField),
        "audio Live: NearField=0 leaves the fade to the core (no per-source near-field cut)");
  WriteIni(q.dir, "[Audio]\r\nBinaural=1\r\nHrtf=missing\r\nLateHeadTracking=0\r\n");
  Tick();
  Check(audio::g_hrtfName == L"default", "audio Live: a personal HRTF that cannot be loaded falls back to the default set");
  WriteIni(q.dir, "[Audio]\r\nBinaural=0\r\n");
  Tick();
  for (int i = 0; i < 4; ++i) {
    SimFrame(q);
    Block(q, a.data(), b.data(), 480);
  }
  Check(audio::g_cnt.ownedNow.load() == 0 && memcmp(a.data(), b.data(), sizeof(float) * 960) == 0 &&
            audio::g_installed.load(),
        "audio Live: Binaural=0 fades every voice back to DCS's panning (hooks stay, pass-through)");
  // Back to 1: running again on the same binding, and the module says so (the app's status reads it as running).
  WriteIni(q.dir, "[Audio]\r\nBinaural=1\r\nLateHeadTracking=0\r\n");
  Tick();
  for (int i = 0; i < 4; ++i) {
    SimFrame(q);
    Block(q, a.data(), b.data(), 480);
  }
  Check(audio::g_cnt.ownedNow.load() == 3 && audio::g_offText.empty(), "audio Live: Binaural back to 1 renders again (off text cleared)");
  // A busy settings slot (a writer preempted between its two stores) gives the reader its last good copy, never "off".
  const uint32_t seq = audio::g_rt.seq.load();
  audio::g_rt.seq.store(seq | 1u);
  const audio::Rt busy = audio::ReadRt();
  audio::g_rt.seq.store(seq);
  Check(busy.binaural && !busy.late, "audio settings: a busy slot gives the last good copy (binaural stays on)");
  RigDown(q);
  // Head size: the default circumference gives the renderer exactly the set's own fitted radius (ITD scale 1).
  Rig h;
  OwnedRig(h, kIniOn);
  Check(std::fabs(audiofake::g_lastInitRadius - 0.0875f) < 1e-7, "audio head size: 57 cm plays the set's own timing (radius = the set's fit)");
  RigDown(h);
}

// Late head rotation: pure correction, and through the pose block end to end.
void LateHead() {
  audv::MetaData m{};
  m.pos[0] = 10;
  m.dist = 10;
  m.flags = audv::meta_flags::kPoseValid;
  m.qUsed[3] = 1;
  m.poseBase = 7;
  audr::PoseNow now;
  now.valid = true;
  now.base = 7;
  now.q = dcsvr_headpose::AxisAngle({0, 1, 0}, 30.0 * 3.14159265358979 / 180.0);  // OpenXR: head turned 30 deg left
  float p[3], g = 0;
  audr::CorrectedPosition(m, now, true, p, &g);
  const bool rot = std::fabs(p[0] - 8.660254f) < 1e-4 && std::fabs(p[2] - 5.0f) < 1e-4 && g == 1.0f;
  now.base = 8;
  audr::CorrectedPosition(m, now, true, p, &g);
  const bool otherSpace = p[0] == 10 && p[2] == 0;
  now.base = 7;
  now.q = dcsvr_headpose::AxisAngle({0, 1, 0}, 90.0 * 3.14159265358979 / 180.0);
  audr::CorrectedPosition(m, now, true, p, &g);
  const bool tooFar = p[0] == 10 && p[2] == 0;
  m.flags |= audv::meta_flags::kNear | audv::meta_flags::kPosValid;
  m.pos[0] = 0.5f;
  m.dist = 0.5f;
  now.q = {0, 0, 0, 1};
  now.posValid = true;
  now.t = {0, 0, -0.3};  // OpenXR -z forward: the head moved 30 cm towards the source
  audr::CorrectedPosition(m, now, true, p, &g);
  const bool trans = std::fabs(p[0] - 0.2f) < 1e-5 && std::fabs(g - audr::kTransMax) < 1e-6;
  Check(rot && otherSpace && tooFar && trans,
        "audio late head tracking: a 30 deg left turn puts a frontal source 30 deg right; other base space or a turn "
        "over 60 deg is not applied; the near-field translation gain is clamped to +3 dB");
  // A pose without a position (stored as zero) against one with it (the head 1.6 m above the floor origin): no
  // translation at all, either way round; with both, a jump of more than 30 cm is clamped to 30 cm.
  m.flags &= ~audv::meta_flags::kNear;
  m.pos[0] = 2.0f;
  m.dist = 2.0f;
  m.tUsed[1] = 1.6f;
  now.t = {0, 0, 0};
  now.posValid = false;
  audr::CorrectedPosition(m, now, true, p, &g);
  const bool nowMissing = std::fabs(p[0] - 2.0f) < 1e-6f && std::fabs(p[1]) < 1e-6f && std::fabs(p[2]) < 1e-6f;
  m.flags &= ~audv::meta_flags::kPosValid;
  m.tUsed[1] = 0.0f;
  now.posValid = true;
  now.t = {0, 1.6, 0};
  audr::CorrectedPosition(m, now, true, p, &g);
  const bool usedMissing = std::fabs(p[0] - 2.0f) < 1e-6f && std::fabs(p[1]) < 1e-6f && std::fabs(p[2]) < 1e-6f;
  m.flags |= audv::meta_flags::kPosValid;
  now.t = {0, 0, -1.2};  // a 1.2 m "move" towards the source within one DCS frame: a tracking glitch
  audr::CorrectedPosition(m, now, true, p, &g);
  const bool clamped = std::fabs(p[0] - (2.0f - static_cast<float>(audr::kMaxLateTransM))) < 1e-5;
  printf("     late translation: missing position now %s, missing in DCS's pose %s, 1.2 m jump -> x %.3f m\n",
         nowMissing ? "ignored" : "APPLIED", usedMissing ? "ignored" : "APPLIED", p[0]);
  Check(nowMissing && usedMissing && clamped,
        "audio late head tracking: no translation unless both poses have a position; a jump over 30 cm is clamped to 30 cm");
  // End to end: the pose ring under a test suffix.
  SetEnvironmentVariableW(L"DCSVR_PERCEPTION_SHM_SUFFIX", L"_audiotest");
  Rig r;
  RigUp(r, "[Audio]\r\nBinaural=1\r\nLateHeadTracking=1\r\n");
  dcsvr_headpose::SharedBlock writer;
  const bool mapped = writer.open(L"_audiotest");
  LARGE_INTEGER f, qn;
  QueryPerformanceFrequency(&f);
  if (mapped) dcsvr_headpose::InitWriter(writer.get(), f.QuadPart, GetCurrentProcessId(), 0);
  auto publish = [&](double yawDeg) {
    dcsvr_headpose::Entry e{};
    QueryPerformanceCounter(&qn);
    e.qpcLocate = qn.QuadPart;
    e.displayTime = qn.QuadPart;
    e.baseSpace = 42;
    e.flags = dcsvr_headpose::entry_flags::orientation_valid | dcsvr_headpose::entry_flags::position_valid;
    const dcsvr_headpose::Quat q = dcsvr_headpose::AxisAngle({0, 1, 0}, yawDeg * 3.14159265358979 / 180.0);
    e.orientation[0] = static_cast<float>(q.x), e.orientation[1] = static_cast<float>(q.y);
    e.orientation[2] = static_cast<float>(q.z), e.orientation[3] = static_cast<float>(q.w);
    dcsvr_headpose::Publish(writer.get(), e);
  };
  AddBoth(r, 0, true, 1, kSine, 0.5f, 440, 1.0f);
  PosBoth(r, 0, 10, 0, 0);  // in front
  CreateVoice(r, 0);
  Tick();  // opens the pose block
  Bind(r);
  std::vector<float> a(2 * 480), b(2 * 480);
  publish(0);
  for (int i = 0; i < 6; ++i) {
    SimFrame(r);
    Block(r, a.data(), b.data(), 480);
  }
  auto pan = [&]() {
    double l = 0, rr = 0;
    for (int i = 0; i < 480; ++i) l += a[2 * i] * a[2 * i], rr += a[2 * i + 1] * a[2 * i + 1];
    return std::sqrt(rr / (l + 1e-30));
  };
  const double front = pan();
  publish(50);  // the head turned 50 deg left after DCS's frame: the source is now to the right
  Block(r, a.data(), b.data(), 480);
  Block(r, a.data(), b.data(), 480);
  const double turned = pan();
  printf("     late head tracking: R/L %.3f facing it, %.3f after a 50 deg turn between DCS frames (no new DCS frame)\n",
         front, turned);
  Check(mapped && std::fabs(front - 1.0) < 0.02 && turned > 2.5,
        "audio late head tracking: a head turn published after DCS's frame moves the sound at the next audio block");
  RigDown(r);
  SetEnvironmentVariableW(L"DCSVR_PERCEPTION_SHM_SUFFIX", nullptr);
}

// No allocation on the simulation and render threads (both run on this thread here, flagged).
void NoAlloc() {
  Rig r;
  OwnedRig(r, kIniOn);
  std::vector<float> a(2 * 2048), b(2 * 2048);
  audiofake::g_rtAllocs = 0;
  audiofake::t_rtThread = true;
  for (int blk = 0; blk < 600; ++blk) {
    const uint32_t frames = 64 + (blk * 131) % 1985;
    if (blk == 100) audio::g_userOff = true;
    if (blk == 200) audio::g_userOff = false;
    if (blk == 300) audiofake::g_refuse = true;
    if (blk == 400) audiofake::g_refuse = false;
    if (blk % 2 == 0) {
      PosBoth(r, 0, 5 * std::cos(blk * 0.02), 1, 5 * std::sin(blk * 0.02));
      SimFrame(r);
    }
    Block(r, a.data(), b.data(), frames);
  }
  audiofake::t_rtThread = false;
  printf("     allocations on the real-time path: %llu (fades in %llu, out %llu, hrtf %llu, re-pan %llu)\n",
         static_cast<unsigned long long>(audiofake::g_rtAllocs.load()), static_cast<unsigned long long>(audio::g_cnt.fadeIns.load()),
         static_cast<unsigned long long>(audio::g_cnt.fadeOuts.load()), static_cast<unsigned long long>(audio::g_cnt.hrtfCalls.load()),
         static_cast<unsigned long long>(audio::g_cnt.repanCalls.load()));
  Check(audiofake::g_rtAllocs.load() == 0 && audio::g_cnt.fadeIns.load() > 0 && audio::g_cnt.fadeOuts.load() > 0,
        "audio real time: no heap allocation on the simulation and render paths (steady, fades, re-pan, kill switch)");
  RigDown(r);
}

// Two threads (simulation, render) on the fake for QV_AUDIO_STRESS_SEC seconds; the control side toggles, rewrites the
// ini and reinstalls (payload hot reload) while both run; voices end and are reused.
void Stress() {
  int secs = 20;
  if (const char* s = getenv("QV_AUDIO_STRESS_SEC")) secs = std::max(1, atoi(s));
  Rig r;
  RigUp(r, kIniOn);
  SetContext2D(r, 7, true);
  for (int i = 0; i < 40; ++i)
    AddBoth(r, i % 9 == 8 ? 7 : i % 3, true, i % 11 == 10 ? 2 : 1, i % 2 ? kSine : kNoise, 0.2f, 100 + 37 * i, 0.1f + 0.02f * i,
            i + 1);
  for (size_t i = 0; i < r.w.sounds.size(); ++i) CreateVoice(r, static_cast<int>(i));
  Bind(r);
  std::atomic<bool> stop{false};
  std::atomic<uint64_t> simFrames{0}, blocks{0}, nonFinite{0}, recycled{0};
  std::thread sim([&] {
    audiofake::t_rtThread = true;
    struct Off {
      ~Off() { audiofake::t_rtThread = false; }  // std::thread frees its own state on this thread afterwards
    } off;
    uint64_t rng = 5;
    uint32_t n = 0;
    while (!stop.load()) {
      FakeLock(r.im.base + kMutex);
      W<uint32_t>(r.im.base, kFrameCounter, R<uint32_t>(r.im.base, kFrameCounter) + 1);
      for (size_t i = 0; i < r.w.sounds.size(); ++i) {
        uint8_t* v = r.w.voices[i];
        auto* ended = reinterpret_cast<std::atomic<uint32_t>*>(v + kVEnded);
        if (ended->load(std::memory_order_acquire)) {  // the render thread finished it: reuse it for a new sound
          ended->store(0);
          W<int64_t>(v, kVRemaining, static_cast<int64_t>(4800 + (rng >> 40) % 48000));
          reinterpret_cast<DrvP3b>(r.im.base + kDrvP3b)(v, R<uint8_t>(v, 0xec2));
          r.w.InitRamps(static_cast<int>(i));
          reinterpret_cast<std::atomic<uint32_t>*>(v + kVAlive)->store(1, std::memory_order_release);
          recycled.fetch_add(1);
          continue;
        }
        if (!reinterpret_cast<std::atomic<uint32_t>*>(v + kVAlive)->load(std::memory_order_acquire)) continue;
        rng = rng * 6364136223846793005ull + 1;
        const double t = n * 0.01 + i;
        r.w.SetPos(static_cast<int>(i), 8 * std::cos(t), 2 * std::sin(t * 0.3), 8 * std::sin(t));
        if ((rng >> 50) % 200 == 0) W<int64_t>(v, kVRemaining, 960);  // ends soon
        reinterpret_cast<DrvP3a>(r.im.base + kDrvP3a)(reinterpret_cast<void**>(r.w.slots[i]));
      }
      FakeUnlock(r.im.base + kMutex);
      ++n;
      simFrames.fetch_add(1);
      Sleep(n % 4 == 0 ? 1 : 0);
    }
  });
  std::vector<float> renderOut(2 * 4096);
  std::thread render([&] {
    audiofake::t_rtThread = true;
    struct Off {
      ~Off() { audiofake::t_rtThread = false; }
    } off;
    std::vector<float>& out = renderOut;
    uint64_t rng = 7;
    while (!stop.load()) {
      rng = rng * 6364136223846793005ull + 1;
      const uint32_t frames = 32 + static_cast<uint32_t>((rng >> 33) % 2000);
      memset(out.data(), 0, sizeof(float) * 2 * frames);
      reinterpret_cast<DrvP5>(r.im.base + kDrvP5)(r.w.wasapi, out.data(), frames);
      for (uint32_t i = 0; i < 2 * frames; ++i)
        if (!std::isfinite(out[i])) nonFinite.fetch_add(1);
      blocks.fetch_add(1);
    }
  });
  const ULONGLONG t0 = GetTickCount64();
  int reinstalls = 0, toggles = 0, rewrites = 0;
  uint64_t rng = 11;
  while (GetTickCount64() - t0 < static_cast<ULONGLONG>(secs) * 1000) {
    Tick();
    rng = rng * 6364136223846793005ull + 1;
    const uint32_t k = static_cast<uint32_t>((rng >> 33) % 100);
    if (k < 3) {
      audio::g_userOff = !audio::g_userOff.load();
      ++toggles;
    } else if (k < 5) {
      char ini[200];
      snprintf(ini, sizeof(ini), "[Audio]\r\nBinaural=1\r\nMaxObjects=%u\r\nLateHeadTracking=0\r\nLevelTrimDb=%d\r\n",
               8 + static_cast<unsigned>((rng >> 40) % 57), -static_cast<int>((rng >> 20) % 6));
      WriteIni(r.dir, ini);
      ++rewrites;
    } else if (k < 6) {
      audio::Uninstall(300);  // payload Stop while both threads run
      audio::g_hookState = 0;
      audio::g_loop.lastInstall = 0;
      ++reinstalls;
    }
    Sleep(20);
  }
  audio::g_userOff = false;
  audio::Uninstall(400);
  const bool restored = memcmp(r.textBefore.data(), r.im.base + kTextBegin, r.textBefore.size()) == 0;
  stop = true;
  sim.join();
  render.join();
  const uint64_t allocs = audiofake::g_rtAllocs.load();
  printf("     stress %d s: %llu sim frames, %llu blocks, %llu voices reused, %d reinstalls, %d toggles, %d ini writes; "
         "torn meta reads %llu, hrtf %llu, fades %llu/%llu, forced frees %llu, non-finite %llu, rt allocations %llu, lock "
         "misuse %llu\n",
         secs, static_cast<unsigned long long>(simFrames.load()), static_cast<unsigned long long>(blocks.load()),
         static_cast<unsigned long long>(recycled.load()), reinstalls, toggles, rewrites,
         static_cast<unsigned long long>(audio::g_meta.torn.load()), static_cast<unsigned long long>(audio::g_cnt.hrtfCalls.load()),
         static_cast<unsigned long long>(audio::g_cnt.fadeIns.load()), static_cast<unsigned long long>(audio::g_cnt.fadeOuts.load()),
         static_cast<unsigned long long>(audio::g_sim.forcedFrees), static_cast<unsigned long long>(nonFinite.load()),
         static_cast<unsigned long long>(allocs), static_cast<unsigned long long>(g_lockBad.load()));
  Check(restored && nonFinite.load() == 0 && allocs == 0 && g_lockBad.load() == 0 && simFrames.load() > 100 &&
            blocks.load() > 100 && audio::g_cnt.hrtfCalls.load() > 100 && recycled.load() > 0,
        "audio stress: simulation and render threads with kill-switch toggles, Live ini writes, voice reuse and payload "
        "reinstalls: no crash, finite output, no real-time allocation, ed_sound's bytes restored");
  RigDown(r);
}

// DCS's second output (options.lua sound.hp_output): two WASAPI renderers, each with its own thread calling P5. The
// binding stays with one live renderer (no switching back and forth), and moves to the one the world's 3D sounds play
// on, even when the other output's first block came first.
void TwoOutputs() {
  Rig r;
  OwnedRig(r, kIniOn);  // the main output (r.w) bound by its first block
  World hp;
  hp.Init();
  std::vector<float> a(2 * 480), b(2 * 480), c(2 * 480);
  auto hpBlock = [&]() {
    memset(c.data(), 0, sizeof(float) * 960);
    reinterpret_cast<DrvP5>(r.im.base + kDrvP5)(hp.wasapi, c.data(), 480);
  };
  const uint32_t ep0 = audio::g_epoch.load();
  for (int i = 0; i < 100; ++i) {
    SimFrame(r);
    Block(r, a.data(), b.data(), 480);
    hpBlock();
    Tick();
    if (i % 50 == 49) {
      audio::g_loop.lastWorld = 0;
      Tick();
    }
  }
  audr::Binding* bnd = audio::g_binding.load();
  Check(audio::g_epoch.load() == ep0 && bnd && bnd->renderer == r.w.Renderer() && audio::g_cnt.ownedNow.load() == 3 &&
            audio::g_retiredBindings.empty(),
        "audio two outputs: with the main output bound, the headphones output's blocks never take the binding (100 "
        "alternating blocks, no rebind)");
  RigDown(r);
  hp.Destroy();

  // The headphones output's block comes first: it is bound, then the binding moves to the main output once the world's
  // 3D sounds are seen on it for 2 s, and stays there.
  Rig q;
  RigUp(q, kIniOn);
  World hp2;
  hp2.Init();
  SetContext2D(q, 7, true);
  AddBoth(q, 0, true, 1, kSine, 0.5f, 440, 1.0f, 3);
  AddBoth(q, 1, true, 1, kNoise, 0.25f, 0, 0.9f, 7);
  PosBoth(q, 0, 5, 1, 4);
  PosBoth(q, 1, -3, 0, -6);
  for (int i = 0; i < 2; ++i) CreateVoice(q, i);
  memset(c.data(), 0, sizeof(float) * 960);
  reinterpret_cast<DrvP5>(q.im.base + kDrvP5)(hp2.wasapi, c.data(), 480);
  Tick();
  const bool hpFirst = audio::g_binding.load() && audio::g_binding.load()->renderer == hp2.Renderer();
  const uint32_t ep1 = audio::g_epoch.load();
  int switches = 0;
  void* last = audio::g_binding.load() ? audio::g_binding.load()->renderer : nullptr;
  for (int i = 0; i < 200; ++i) {
    SimFrame(q);
    Block(q, a.data(), b.data(), 480);
    memset(c.data(), 0, sizeof(float) * 960);
    reinterpret_cast<DrvP5>(q.im.base + kDrvP5)(hp2.wasapi, c.data(), 480);
    if (i % 40 == 39) audio::g_loop.lastWorld = 0;  // one "second" of the world count every 40 blocks
    Tick();
    void* now = audio::g_binding.load() ? audio::g_binding.load()->renderer : nullptr;
    if (now != last) ++switches, last = now;
  }
  bnd = audio::g_binding.load();
  printf("     two outputs: headphones output first %s, %d switch(es), %u rebind(s)\n", hpFirst ? "bound" : "not bound",
         switches, audio::g_epoch.load() - ep1);
  Check(hpFirst && bnd && bnd->renderer == q.w.Renderer() && switches == 1 && audio::g_epoch.load() - ep1 == 1 &&
            audio::g_cnt.ownedNow.load() == 2,
        "audio two outputs: the binding moves once to the output the world's 3D sounds play on, and stays");
  RigDown(q);
  hp2.Destroy();
}

// No HRTF set at all (a build without the generic set, no personal set): ed_sound is never patched.
bool NoHrtf(const wchar_t*, std::vector<uint8_t>*) { return false; }
void NoSetNoHooks() {
  g_rigHrtf = &NoHrtf;
  Rig r;
  RigUp(r, kIniOn);
  for (int i = 0; i < 3; ++i) {
    audio::g_loop.lastInstall = 0;
    Tick();
  }
  const bool untouched = memcmp(r.textBefore.data(), r.im.base + kTextBegin, r.textBefore.size()) == 0;
  Check(!audio::g_installed.load() && audio::g_hookState.load() == 0 && untouched &&
            audio::g_offText.find("no HRTF set") == 0 && (audr::g_status.load() & qvstatus::kAudioOff),
        "audio without any HRTF set: ed_sound.dll is not patched (an audio: off (no HRTF set: ...) line, status bit 55)");
  RigDown(r);
  g_rigHrtf = &TestHrtf;
  // A personal set present without the generic one: patched (the module renders with the personal set).
  g_rigHrtf = [](const wchar_t* name, std::vector<uint8_t>* out) { return wcscmp(name, L"default") != 0 && TestHrtf(name, out); };
  Rig p;
  RigUp(p, "[Audio]\r\nBinaural=1\r\nHrtf=mine\r\nLateHeadTracking=0\r\n");
  Check(audio::g_installed.load(), "audio with only a personal HRTF set: installed");
  RigDown(p);
  g_rigHrtf = &TestHrtf;
}

// 3D audio off (the default, as in 0.7.0): no control thread, nothing polled, not one log line. Turned on Live, the
// worker's ini reload (StartIfWanted) starts the thread once; never after Stop.
void DormantWhenOff() {
  ResetModule();
  const std::wstring dir = TestDir(L"audio_rig");
  const std::wstring saved = g_dir;
  g_dir = dir;
  audio::g_testNoThread = true;
  const int s0 = audio::g_testThreadStarts.load();
  int audioLines = 0;
  for (const char* ini : {"[General]\r\nPerceptionOnly=1\r\n", "[Audio]\r\nBinaural=0\r\nHrtf=Ann Lee\r\n"}) {
    WriteIni(dir, ini);
    StartReportCapture();
    audio::Start();
    const std::string log = StopReportCapture();
    audioLines += log.find("audio:") != std::string::npos ? 1 : 0;
    if (ini[1] == 'G') audio::Stop();
  }
  const bool offIdle = audio::g_testThreadStarts.load() == s0 && audioLines == 0 && !audio::g_haveCfg && !audio::g_installed.load();
  WriteIni(dir, "[Audio]\r\nBinaural=1\r\n");
  const bool liveOn = audio::StartIfWanted() && audio::g_testThreadStarts.load() == s0 + 1;
  const bool once = !audio::StartIfWanted() && audio::g_testThreadStarts.load() == s0 + 1;
  audio::Stop();
  const bool notAfterStop = !audio::StartIfWanted() && audio::g_testThreadStarts.load() == s0 + 1;
  audio::Start();
  const bool atStart = audio::g_testThreadStarts.load() == s0 + 2;
  Check(offIdle && liveOn && once && notAfterStop && atStart,
        "audio off: no control thread and no log line (no [Audio], Binaural=0); turned on Live the worker's reload "
        "starts it once, never after Stop; Binaural=1 at payload start starts it");
  ResetModule();
  g_dir = saved;
}

// The pilot names the sets (often after a person): the module's log says "personal", never a name (config line,
// on, crossfading, not usable, on again, an invalid name's note).
void NoSetNamesInLog() {
  StartReportCapture();
  Rig r;
  OwnedRig(r, "[Audio]\r\nBinaural=1\r\nHrtf=Ann Lee\r\nLateHeadTracking=0\r\n");
  std::vector<float> a(2 * 480), b(2 * 480);
  auto run = [&](const char* ini) {
    if (ini) WriteIni(r.dir, ini);
    Tick();
    for (int i = 0; i < 3; ++i) {
      SimFrame(r);
      Block(r, a.data(), b.data(), 480);
    }
    Tick();
  };
  run(nullptr);
  run("[Audio]\r\nBinaural=1\r\nHrtf=Bo Zed\r\nLateHeadTracking=0\r\n");
  run("[Audio]\r\nBinaural=1\r\nHrtf=missing\r\nLateHeadTracking=0\r\n");
  run("[Audio]\r\nBinaural=0\r\nHrtf=Bo Zed\r\nLateHeadTracking=0\r\n");
  run("[Audio]\r\nBinaural=1\r\nHrtf=Bo Zed\r\nLateHeadTracking=0\r\n");
  run("[Audio]\r\nBinaural=1\r\nHrtf=Cy's\r\nLateHeadTracking=0\r\n");
  const std::string log = StopReportCapture();
  RigDown(r);
  Check(log.find("HRTF personal: ") != std::string::npos && log.find("HRTF personal (crossfading)") != std::string::npos &&
            log.find("HRTF personal not usable (") != std::string::npos && log.find("Hrtf=personal ") != std::string::npos &&
            log.find("re-enabled in the settings, HRTF personal: ") != std::string::npos &&
            log.find("Hrtf is not a valid name (4 characters)") != std::string::npos && log.find("Ann") == std::string::npos &&
            log.find("Bo Zed") == std::string::npos && log.find("Cy") == std::string::npos,
        "audio log: a set of the pilot's is \"personal\" in every line, never its name");
  if (log.find("Ann") != std::string::npos || log.find("Bo Zed") != std::string::npos) printf("%s\n", log.c_str());
}

// The log phrases the app reads, and the status bits.
void Phrases() {
  const std::string src = lptest::ReadAllSources();
  const char* phrases[] = {"\"audio: off (%s)\"", "\"audio: on (%.0f Hz stereo, HRTF %ls", "\"audio: running, %u sounds, p99 ",
                           "\"audio: hooks in (ed_sound.dll 0x%08x, %s)\"", "\"audio: kill switch (%s): binaural %s\"",
                           "\"audio: ctx %d 2d=%d cockpitGain=%d output=%d crosstalk=%.3f/%.3f\"",
                           "\"audio: listener-vs-pose: ", "\"audio: on (%s, HRTF %ls: %u directions)\"",
                           "\"audio: HRTF %ls not usable (%s); the default set is used\""};
  int missing = 0;
  for (const char* p : phrases)
    if (src.find(p) == std::string::npos) ++missing, printf("     audio phrase missing: %s\n", p);
  Check(missing == 0, "audio log: the lines the app reads (off reason, on, running, hooks in, kill switch, contexts, "
                      "listener-vs-pose) are in the source");
  Check(qvstatus::kAudioConfigured == 1ull << 53 && qvstatus::kAudioActive == 1ull << 54 && qvstatus::kAudioOff == 1ull << 55 &&
            !(qvstatus::kAudioConfigured & (qvstatus::kPrewarm | qvstatus::kWarmValid)),
        "audio status: bits 53-55 next to the existing fields");
  Check(!inidiff::IsEngineKey(L"Audio", L"Binaural") && !inidiff::IsEngineKey(L"Hotkeys", L"AudioToggle") &&
            inidiff::Classify(L"Audio", L"MaxObjects") == inidiff::kNotEngine,
        "audio config: [Audio] is not an engine section (a Live write keeps the engine kill switch)");
}

// A rig with one owned 0 dBFS DC source at hard right (DCS's pan and the stand-in's differ most), in steady state.
void DcRig(Rig& r, const char* ini = kIniOn) {
  RigUp(r, ini);
  AddBoth(r, 0, true, 1, kDc, 1.0f, 0, 1.0f);
  PosBoth(r, 0, 0, 0, 10);
  CreateVoice(r, 0);
  Bind(r);
  std::vector<float> a(2 * 480), b(2 * 480);
  for (int i = 0; i < 6; ++i) {
    SimFrame(r);
    Block(r, a.data(), b.data(), 480);
  }
}
double MaxStep(const std::vector<float>& all) {
  double m = 0;
  for (size_t i = 2; i < all.size(); ++i) m = std::max(m, static_cast<double>(std::fabs(all[i] - all[i - 2])));
  return m;
}

// A torn snapshot: the render thread snapshots DCS's own gains between DCS's panner store and the tag store (no lock
// between them). The voice stays binaural (no fade out and in): its mono signal is recovered from the known gain ramp.
void TornSnapshot() {
  Rig r;
  DcRig(r);
  std::vector<float> a(2 * 480), b(2 * 480), steady(2 * 480);
  Block(r, steady.data(), b.data(), 480);  // steady output (DC: constant)
  const uint64_t ins0 = audio::g_cnt.fadeIns.load(), outs0 = audio::g_cnt.fadeOuts.load();
  uint8_t* v = r.w.voices[0];
  uint8_t tag[16];
  memcpy(tag, v + 0x48, 16);
  memcpy(v + 0x48, r.ref.voices[0] + 0x48, 16);  // DCS's panner stored its gains; the tag store not yet
  double worst = 0;
  Block(r, a.data(), b.data(), 480);  // snapshots DCS's gains
  for (int i = 0; i < 960; ++i) worst = std::max(worst, static_cast<double>(std::fabs(a[i] - steady[i])));
  memcpy(v + 0x48, tag, 16);  // the tag store
  Block(r, a.data(), b.data(), 480);  // ramps back to the tag
  for (int i = 0; i < 960; ++i) worst = std::max(worst, static_cast<double>(std::fabs(a[i] - steady[i])));
  SimFrame(r);
  Block(r, a.data(), b.data(), 480);
  printf("     torn snapshot: %llu counted, fades in/out +%llu/+%llu, largest deviation from the steady output %.2e\n",
         static_cast<unsigned long long>(audio::g_cnt.tornSnaps.load()),
         static_cast<unsigned long long>(audio::g_cnt.fadeIns.load() - ins0),
         static_cast<unsigned long long>(audio::g_cnt.fadeOuts.load() - outs0), worst);
  Check(audio::g_cnt.tornSnaps.load() == 1 && audio::g_cnt.fadeIns.load() == ins0 && audio::g_cnt.fadeOuts.load() == outs0 &&
            worst < 1e-5 && audio::g_cnt.ownedNow.load() == 1,
        "audio torn snapshot: DCS's gains snapshotted between the panner's store and the tag keep the voice binaural "
        "(no fade out and in; the output stays the steady one within 1e-5)");
  RigDown(r);
}

// The core skipped for some blocks (HRTF share 0 while still tagged, e.g. off for the session decided on the render
// side): its slot state is stale; the next call starts afresh (kReset), never from that old history.
void CoreSkipThenResume() {
  Rig r;
  DcRig(r);
  std::vector<float> a(2 * 480), b(2 * 480);
  audio::g_offReason = audio::kOffOverruns;
  for (int k = 0; k < 3; ++k) Block(r, a.data(), b.data(), 480);  // h 1 -> 0, then the core is not called
  audio::g_offReason = audio::kOffNone;
  audiofake::g_lastFlags = 0;
  const uint64_t renders0 = audiofake::g_renders.load();
  Block(r, a.data(), b.data(), 480);  // h 0 -> 1
  Check(audiofake::g_renders.load() == renders0 + 1 && (audiofake::g_lastFlags.load() & binaural::source_flags::kReset),
        "audio: after blocks without a core call, the voice's next core call starts afresh (kReset, no stale history)");
  RigDown(r);
}

// The bound output's blocks pass through (longer than the core takes): the owned voices go back to DCS's panning (the
// sim releases them instead of leaving them tagged and centred), an audio: off line says why, and it comes back.
void LongBlocks() {
  Rig r;
  OwnedRig(r, kIniOn);
  std::vector<float> a(2 * 9000), b(2 * 9000);
  for (int i = 0; i < 4; ++i) {
    SimFrame(r);
    Block(r, a.data(), b.data(), 480);
  }
  const bool ownedBefore = audio::g_cnt.ownedNow.load() == 3;
  for (int i = 0; i < 4; ++i) {
    SimFrame(r);
    Block(r, a.data(), b.data(), 9000);
  }
  Tick();
  const bool released = audio::g_cnt.ownedNow.load() == 0 && audio::g_passFrames.load() == 9000;
  const bool said = audio::g_offText.find("9000 frames") != std::string::npos;
  for (int i = 0; i < 6; ++i) {
    SimFrame(r);
    Block(r, a.data(), b.data(), 480);
  }
  Tick();
  printf("     long blocks: owned %s -> %u -> %u; off text while long: %s\n", ownedBefore ? "3" : "?",
         released ? 0u : 99u, audio::g_cnt.ownedNow.load(), said ? "yes" : "no");
  Check(ownedBefore && released && said && audio::g_cnt.ownedNow.load() == 3 && audio::g_offText.empty(),
        "audio: blocks too long for the core release the owned voices to DCS's panning (an audio: off line), and "
        "binaural comes back with normal blocks");
  RigDown(r);
}

// The binding moves to another live output while the bound one renders owned voices: they fade back to DCS's panning
// on the old output first (no centred voices, no step), then the binding moves (DC source: no step above -60 dBFS).
void Handover() {
  Rig r;
  DcRig(r);
  World hp;
  hp.Init();
  std::vector<float> a(2 * 2048), b(2 * 2048), c(2 * 480), all;  // 2048-frame blocks (as Fades)
  auto hpBlock = [&]() {
    memset(c.data(), 0, sizeof(float) * 960);
    reinterpret_cast<DrvP5>(r.im.base + kDrvP5)(hp.wasapi, c.data(), 480);
  };
  const bool ownedBefore = audio::g_cnt.ownedNow.load() == 1;
  const uint64_t outs0 = audio::g_cnt.fadeOuts.load();
  audio::g_worldRenderer = hp.Renderer();  // the world's 3D sounds moved to the other output
  audio::g_newRenderer = hp.Renderer();
  Tick();
  const bool waited = audio::g_handover.load() && audio::g_binding.load()->renderer == r.w.Renderer();
  int moved = -1;
  for (int i = 0; i < 12; ++i) {
    SimFrame(r);
    Block(r, a.data(), b.data(), 2048);
    all.insert(all.end(), a.begin(), a.end());
    hpBlock();
    Tick();
    if (moved < 0 && audio::g_binding.load()->renderer == hp.Renderer()) moved = i;
  }
  const bool same = memcmp(a.data(), b.data(), sizeof(float) * 2 * 2048) == 0;
  printf("     handover: waited %s, moved after %d block(s), fade-outs +%llu, largest step %.2e, old output now DCS's %s\n",
         waited ? "yes" : "no", moved + 1, static_cast<unsigned long long>(audio::g_cnt.fadeOuts.load() - outs0),
         MaxStep(all), same ? "bit for bit" : "DIFFERS");
  Check(ownedBefore && waited && moved >= 0 && audio::g_cnt.fadeOuts.load() > outs0 && MaxStep(all) < 1e-3 && same &&
            !audio::g_handover.load(),
        "audio two outputs: when the binding moves, the old output's binaural voices fade back to DCS's panning first "
        "(no step), then the binding moves");
  RigDown(r);
  hp.Destroy();
}

// ed_sound loaded but its engine not started (state 0): nothing is locked or patched until it is.
void EngineNotStarted() {
  Rig r;
  RigUp(r, "[Audio]\r\nBinaural=0\r\n");
  W<int32_t>(r.im.base, kEngineState, 0);
  WriteIni(r.dir, kIniOn);
  audio::g_loop.lastInstall = 0;
  Tick();
  const bool waited = !audio::g_installed.load() && audio::g_hookState.load() == 0 &&
                      memcmp(r.textBefore.data(), r.im.base + kTextBegin, r.textBefore.size()) == 0;
  W<int32_t>(r.im.base, kEngineState, 3);
  audio::g_loop.lastInstall = 0;
  Tick();
  Check(waited && audio::g_installed.load(), "audio hooks: not installed while ed_sound's engine state is 0, installed once it runs");
  RigDown(r);
}

// The thunk page cannot be made executable: nothing is patched (a site calling a non-executable page would crash DCS).
BOOL WINAPI NoProtect(LPVOID, SIZE_T, DWORD, PDWORD) { return FALSE; }
void ThunksNotExecutable() {
  audhk::g_thunkProtect = &NoProtect;
  Rig r;
  RigUp(r, kIniOn);
  const bool untouched = memcmp(r.textBefore.data(), r.im.base + kTextBegin, r.textBefore.size()) == 0;
  Check(!audio::g_installed.load() && audio::g_hookState.load() == -1 && untouched &&
            audio::g_offText == "the call thunks could not be made executable",
        "audio hooks: a thunk page that cannot be made executable patches nothing (an audio: off line)");
  audhk::g_thunkProtect = &VirtualProtect;
  RigDown(r);
}

// Stop: a wrapper call that passed a call site just before it was restored counts itself only once inside; the
// bindings are freed only after a quiet grace period, never under it.
std::thread* g_lateCall = nullptr;
std::atomic<void*> g_lateSaw{nullptr};
void StopGrace() {
  Rig r;
  DcRig(r);
  void* const want = r.w.Renderer();
  g_lateSaw = nullptr;
  audio::g_stopGraceMs = 300;
  audio::g_testAfterRestore = [] {
    g_lateCall = new std::thread([] {
      Sleep(20);  // still between the call site and the wrapper's g_inflight increment
      audio::g_inflight.fetch_add(1);
      if (audr::Binding* b = audio::g_binding.load()) g_lateSaw = b->renderer;
      Sleep(100);
      audio::g_inflight.fetch_sub(1);
    });
  };
  audio::Stop();
  if (g_lateCall) {
    g_lateCall->join();
    delete g_lateCall;
    g_lateCall = nullptr;
  }
  audio::g_testAfterRestore = nullptr;
  audio::g_stopGraceMs = 50;
  Check(g_lateSaw.load() == want && audio::g_binding.load() == nullptr,
        "audio stop: a wrapper call starting just after the sites were restored still finds its binding; it is freed after");
  RigDown(r);
}

inline void Run() {
  printf("---- binaural audio module\n");
  g_rigHrtf = &TestHrtf;
  g_rigResetExtra = &ResetStandIn;
  RealEdSound();
  FakeResolveInstall();
  MonoGain();
  Ownership();
  PassThrough();
  NullTest();
  SteadyState();
  Fades();
  SwitchesAndFallbacks();
  Config();
  LateHead();
  TwoOutputs();
  TornSnapshot();
  CoreSkipThenResume();
  LongBlocks();
  Handover();
  EngineNotStarted();
  ThunksNotExecutable();
  StopGrace();
  NoSetNoHooks();
  DormantWhenOff();
  NoSetNamesInLog();
  NoAlloc();
  Phrases();
  Stress();
  audio::g_testNoThread = false;
  // The test folders (ini files, flags) away.
  const std::wstring root = TestDir(L"x").substr(0, TestDir(L"x").size() - 2);
  for (const wchar_t* sub : {L"audio_rig", L"audio_cfg", L"x"}) {  // DormantWhenOff uses audio_rig too
    const std::wstring d = root + sub + L"\\";
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((d + L"*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
      do
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) DeleteFileW((d + fd.cFileName).c_str());
      while (FindNextFileW(h, &fd));
      FindClose(h);
    }
    RemoveDirectoryW(d.c_str());
  }
  RemoveDirectoryW(root.c_str());
}

}  // namespace audiotest
