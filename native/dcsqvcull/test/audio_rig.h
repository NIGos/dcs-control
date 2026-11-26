// The binaural audio test rig, shared by audio_test.h (the module against the stand-in core) and audio_e2e_test.cpp
// (the module against the real BinauralCore): the fake ed_sound image's signature table, a hooked world driven through
// ed_sound's call sites and an identical unhooked reference world, the module reset between scenarios, and the frame
// and block drivers.
#pragma once

#include "audio_fake_ed.h"

namespace audiotest {

using namespace fakeed;

// Set by the test file: the HRTF bytes the module loads (audio::g_testHrtf), and what else a reset puts back (the
// stand-in core's controls).
bool (*g_rigHrtf)(const wchar_t* name, std::vector<uint8_t>* out) = nullptr;
void (*g_rigResetExtra)() = nullptr;

// ---- a signature table for the fake image (same patterns and function ranges, the fake's own hashes) ---------------
struct FakeTable {
  audsig::Loc locs[audsig::kCount];
  audsig::Mask masks[1024];
  uint32_t nMasks = 0;
  audhk::SigTable t{};
};

inline void MakeTable(const Image& im, uint32_t stamp, uint32_t size, FakeTable* ft) {
  ft->nMasks = 0;
  for (int i = 0; i < audsig::kCount; ++i) {
    audsig::Loc l = audsig::kLocs[i];
    const uint32_t fs = static_cast<uint32_t>(static_cast<int64_t>(l.rva) + l.fnAt);
    l.maskFirst = ft->nMasks;
    for (uint32_t f : im.rel)
      if (f >= fs && f + 4 <= fs + l.fnLen) ft->masks[ft->nMasks++] = {static_cast<uint16_t>(f - fs), 4};
    for (uint32_t f : im.imm64)
      if (f >= fs && f + 8 <= fs + l.fnLen) ft->masks[ft->nMasks++] = {static_cast<uint16_t>(f - fs), 8};
    l.maskCount = ft->nMasks - l.maskFirst;
    std::vector<uint8_t> buf(im.base + fs, im.base + fs + l.fnLen);
    for (uint32_t k = l.maskFirst; k < ft->nMasks; ++k)
      for (uint32_t j = 0; j < ft->masks[k].len; ++j) buf[ft->masks[k].at + j] = 0;
    l.fnHash = audhk::Fnv(buf.data(), buf.size());
    ft->locs[i] = l;
  }
  ft->t = {ft->locs, ft->masks, stamp, size};
}

inline void* FakeImports(const char* dll, const char* name) {
  if (_stricmp(dll, "edCore.dll")) return nullptr;
  if (!strcmp(name, audhk::kLockName)) return reinterpret_cast<void*>(&FakeLock);
  if (!strcmp(name, audhk::kUnlockName)) return reinterpret_cast<void*>(&FakeUnlock);
  return nullptr;
}

// ---- the rig: the fake image, a hooked world (through the call sites) and an identical reference world (direct) ----
struct Rig {
  Image im;
  FakeTable tab;
  World w, ref;
  std::wstring dir, savedDir;
  std::vector<uint8_t> textBefore;
  bool ok = false;
};
Rig* g_rig = nullptr;
const uint8_t* RigBase() { return g_rig ? g_rig->im.base : nullptr; }

inline std::wstring TestDir(const wchar_t* name) {
  wchar_t tmp[MAX_PATH] = {};
  GetTempPathW(MAX_PATH, tmp);
  std::wstring d = std::wstring(tmp) + L"qv_audio_" + std::to_wstring(GetCurrentProcessId()) + L"\\";
  CreateDirectoryW(d.c_str(), nullptr);
  d += name;
  d += L"\\";
  CreateDirectoryW(d.c_str(), nullptr);
  return d;
}

inline void WriteIni(const std::wstring& dir, const char* text) {
  const std::wstring p = dir + L"DcsQvCull.ini";
  FILE* f = nullptr;
  if (_wfopen_s(&f, p.c_str(), L"wb") || !f) return;
  fputs(text, f);
  fclose(f);
  // A new write time even within the file system's resolution: the reader compares write times.
  static LONGLONG bump = 0;
  HANDLE h = CreateFileW(p.c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
  if (h != INVALID_HANDLE_VALUE) {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER u;
    u.LowPart = ft.dwLowDateTime, u.HighPart = ft.dwHighDateTime;
    u.QuadPart += static_cast<ULONGLONG>(++bump) * 10000000ull;
    ft.dwLowDateTime = u.LowPart, ft.dwHighDateTime = u.HighPart;
    SetFileTime(h, nullptr, nullptr, &ft);
    CloseHandle(h);
  }
}

// Module state back to "never started" (between scenarios; the module itself never needs this).
inline void ResetModule() {
  audio::Stop();
  audio::g_hookState = 0;
  audio::g_userOff = false;
  audio::g_offReason = audio::kOffNone;
  audio::g_stopping = false;
  audio::g_cnt.Reset();
  audio::g_haveCfg = false;
  audio::g_cfgRead = audcfg::Read{};
  audio::g_offText.clear();
  audio::g_rejectedRenderer = nullptr;
  audio::g_newRenderer = nullptr;
  audio::g_hrtfName.clear();
  audio::g_hrtfRequested.clear();
  audio::g_cfgIni.clear();
  audio::g_loop = audio::Loop{};
  audio::g_pose = nullptr;
  audio::WriteRt(audio::Rt{});
  // cockpit space: the control thread's state, the Lua bridge's values, the sim's last frame
  audio::g_spaceSt = audio::SpaceState{};
  audio::g_spaceCtlNow = audr::SpaceCtl{};
  audr::WriteCtl(audio::g_spaceCtl, audr::SpaceCtl{});
  audspace::SpaceWrite(audio::g_space, audspace::SpaceData{});
  audio::LuaAircraft("");
  audio::LuaState(-1.0, 0, -1);
  audio::g_luaCalls = 0;
  audio::g_luaWanted = false;
  if (g_rigResetExtra) g_rigResetExtra();
}

// One pass of the module's control thread, with its ini and flag timers due (the thread runs them every 250 / 1000 ms).
inline void Tick() {
  audio::g_loop.lastCfg = 0;
  audio::g_loop.lastFlag = 0;
  audio::ControlTick();
}

// Image + worlds + module (installed through ControlTick, like the control thread does).
inline bool RigUp(Rig& r, const char* ini, uint32_t stamp = 0x6acb8057u) {
  ResetModule();
  r.im = Build(stamp);
  if (!r.im.base) return false;
  MakeTable(r.im, stamp, kImageSize, &r.tab);
  g_lockExpect = r.im.base + kMutex;
  r.textBefore.assign(r.im.base + kTextBegin, r.im.base + kTextEnd);
  r.w.Init();
  r.ref.Init();
  r.dir = TestDir(L"audio_rig");
  r.savedDir = g_dir;
  g_dir = r.dir;
  DeleteFileW((r.dir + L"toggle_audio.flag").c_str());
  WriteIni(r.dir, ini);
  g_rig = &r;
  audio::g_testNoThread = true;
  audio::g_luaHoldMs = 0;  // the space's hold for a fresh camera check: off in the rig (its own test turns it on)
  audio::g_testEdBase = &RigBase;
  audio::g_testImports = &FakeImports;
  audio::g_testTable = &r.tab.t;
  audio::g_testHrtf = g_rigHrtf;
  audio::Start();
  Tick();
  r.ok = true;
  return true;
}

inline void RigDown(Rig& r) {
  ResetModule();
  r.w.Destroy();
  r.ref.Destroy();
  Free(r.im);
  g_dir = r.savedDir;
  g_rig = nullptr;
  audio::g_testEdBase = nullptr;
  audio::g_testImports = nullptr;
  audio::g_testTable = nullptr;
  audio::g_testHrtf = nullptr;
}

// Both worlds get the same sound (the reference never touches the image's code).
inline int AddBoth(Rig& r, int ctx, bool is3d, uint32_t channels, Kind kind, float amp, double hz, float gain, uint32_t seed = 1) {
  r.w.Add(r.im.base, ctx, is3d, channels, kind, amp, hz, gain, seed);
  return r.ref.Add(r.im.base, ctx, is3d, channels, kind, amp, hz, gain, seed);
}
inline void PosBoth(Rig& r, int i, double x, double y, double z) {
  r.w.SetPos(i, x, y, z);
  r.ref.SetPos(i, x, y, z);
}
inline void AutoWidthBoth(Rig& r, int i, float innerRadius) {
  r.w.SetAutoWidth(i, innerRadius);
  r.ref.SetAutoWidth(i, innerRadius);
}

// One SND_process frame: hooked world through P3a (ed mutex held, frame counter advanced), the reference directly.
inline void SimFrame(Rig& r) {
  FakeLock(r.im.base + kMutex);
  W<uint32_t>(r.im.base, kFrameCounter, R<uint32_t>(r.im.base, kFrameCounter) + 1);
  const DrvP3a drv = reinterpret_cast<DrvP3a>(r.im.base + kDrvP3a);
  for (size_t i = 0; i < r.w.sounds.size(); ++i)
    if (R<uint32_t>(r.w.voices[i], kVAlive)) drv(reinterpret_cast<void**>(r.w.slots[i]));
  FakeUnlock(r.im.base + kMutex);
  for (size_t i = 0; i < r.ref.sounds.size(); ++i)
    if (R<uint32_t>(r.ref.voices[i], kVAlive)) FakePanner(r.ref.voices[i]);
}
// Voice creation (P3b) for sound i in both worlds.
inline void CreateVoice(Rig& r, int i) {
  FakeLock(r.im.base + kMutex);
  W<uint32_t>(r.im.base, kFrameCounter, R<uint32_t>(r.im.base, kFrameCounter) + 1);
  reinterpret_cast<DrvP3b>(r.im.base + kDrvP3b)(r.w.voices[i], R<uint8_t>(r.w.voices[i], 0xec2));
  FakeUnlock(r.im.base + kMutex);
  r.w.InitRamps(i);
  FakePanner(r.ref.voices[i]);
  r.ref.InitRamps(i);
}
inline void Block(Rig& r, float* out, float* refOut, uint32_t frames) {
  memset(out, 0, sizeof(float) * 2 * frames);
  reinterpret_cast<DrvP5>(r.im.base + kDrvP5)(r.w.wasapi, out, frames);
  if (refOut) {
    memset(refOut, 0, sizeof(float) * 2 * frames);
    FakeMixRef(r.ref.Renderer(), refOut, frames);
  }
}
// The first block binds the renderer (the control thread validates it and starts the core).
inline void Bind(Rig& r) {
  std::vector<float> a(2 * 480), b(2 * 480);
  Block(r, a.data(), b.data(), 480);
  Tick();
}

inline double Db(double ratio) { return ratio > 0 ? 20.0 * std::log10(ratio) : -999.0; }
}  // namespace audiotest
