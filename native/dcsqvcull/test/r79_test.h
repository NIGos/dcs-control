// R79 (R77c N1-N3): one DCSVR_PERCEPTION_SHM_SUFFIX rule (perception_suffix.h) for every shared block.
#pragma once

namespace r79 {

namespace psh = ::dcsvr_perception;

struct EnvSave {  // the variable as it was, put back at the end
  wchar_t old[256] = {};
  DWORD had = 0;
  EnvSave() { had = GetEnvironmentVariableW(perc_sfx::kVariable, old, 256); }
  ~EnvSave() { SetEnvironmentVariableW(perc_sfx::kVariable, had && had < 256 ? old : nullptr); }
};

inline bool SameName(const char* narrow) {
  std::wstring w;
  for (const char* p = narrow; *p; ++p) w += static_cast<wchar_t>(static_cast<unsigned char>(*p));
  return w == perc_sfx::kVariable;
}

// The rule itself: the peer's GetEnvironmentVariableW into 64 wchar (pupil_shift_layer.cpp).
inline void Rule() {
  EnvSave keep;
  Check(SameName(psh::name_suffix_variable) && SameName(::dcsvr_cockpit_mask::name_suffix_variable) &&
            SameName(::dcsvr_cockpit_depth::name_suffix_variable) && SameName(::dcsvr_motion::name_suffix_variable),
        "R79 suffix: the helper reads the variable every contract header names");
  SetEnvironmentVariableW(perc_sfx::kVariable, nullptr);
  const bool unset = perc_sfx::SuffixW().empty();
  SetEnvironmentVariableW(perc_sfx::kVariable, L"");
  const bool empty = perc_sfx::SuffixW().empty();
  const std::wstring s63(63, L'x'), s64(64, L'y');
  SetEnvironmentVariableW(perc_sfx::kVariable, s63.c_str());
  const bool fits = perc_sfx::SuffixW() == s63;
  SetEnvironmentVariableW(perc_sfx::kVariable, s64.c_str());
  const bool tooLong = perc_sfx::SuffixW().empty();
  const wchar_t* wide = L"_r79_é中";  // not ASCII: kept wide (the old A read zero-extended bytes)
  SetEnvironmentVariableW(perc_sfx::kVariable, wide);
  const bool keptWide = perc_sfx::SuffixW() == wide &&
                        perc_sfx::Name(psh::boost_block_name) == std::wstring(psh::boost_block_name) + wide;
  Check(unset && empty && fits && tooLong && keptWide,
        "R79 suffix: unset / empty -> none, 63 chars used as they are, 64 or more -> none (as the peer), wide kept");
}

inline bool MapRw(const std::wstring& name, HANDLE& h, void*& v, size_t size) {
  h = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, name.c_str());
  v = h ? MapViewOfFile(h, FILE_MAP_ALL_ACCESS, 0, 0, size) : nullptr;
  return v != nullptr;
}
inline void Unmap(HANDLE& h, void*& v) {
  if (v) UnmapViewOfFile(v);
  if (h) CloseHandle(h);
  h = nullptr;
  v = nullptr;
}
// The same memory: a marker written through one view is read through the other (seq is the first field everywhere).
inline bool SameBlock(void* writeThrough, const void* readThrough, uint64_t marker) {
  if (!writeThrough || !readThrough) return false;
  *static_cast<volatile uint64_t*>(writeThrough) = marker;
  return *static_cast<const volatile uint64_t*>(readThrough) == marker;
}

// A suffixed run: GazeOut, DepthIn, DepthGrid (fixation_depth.h), BoostOut and StereoAck (cockpit_ipd.h) and the
// periphery filter's GazeOut are all opened under the suffixed names, never the production ones.
inline void SuffixedRun() {
  namespace fd = fixdepth;
  namespace ck = fixdepth::ckipd;
  EnvSave keep;
  wchar_t sfx[48];
  swprintf_s(sfx, L"_r79sfx%lu", GetCurrentProcessId());
  SetEnvironmentVariableW(perc_sfx::kVariable, sfx);
  const std::wstring gazeN = std::wstring(psh::gaze_block_name) + sfx;
  const std::wstring depthN = std::wstring(psh::depth_block_name) + sfx;
  const std::wstring gridN = std::wstring(psh::grid_block_name) + sfx;
  const std::wstring boostN = std::wstring(psh::boost_block_name) + sfx;
  const std::wstring ackN = std::wstring(psh::cockpit_stereo_block_name) + sfx;

  // Saved state; no test overrides (the production path).
  const std::wstring oG = fd::g_gazeName, oD = fd::g_depthName, oGr = fd::g_gridName, oB = ck::g_boostNameOverride,
                     oA = ck::g_ackNameOverride;
  const bool oCone = fd::g_coneOn.load(), oGrid = fd::g_gridOn.load(), oTest = fd::g_testGaze.load();
  fd::g_gazeName.clear(), fd::g_depthName.clear(), fd::g_gridName.clear();
  ck::g_boostNameOverride.clear(), ck::g_ackNameOverride.clear();
  fd::CloseMappings();
  ck::CloseBoost();
  ck::CloseAck(0);

  Check(fd::GazeName() == gazeN && fd::DepthName() == depthN && fd::GridName() == gridN && ck::BoostName() == boostN &&
            ck::AckName() == ackN,
        "R79 suffixed run: GazeOut, DepthIn, DepthGrid, BoostOut, StereoAck names = base + DCSVR_PERCEPTION_SHM_SUFFIX");

  // The layer's side creates GazeOut and BoostOut (pupil_shift_layer.cpp), under the suffixed names.
  HANDLE lg = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(psh::GazeOut), gazeN.c_str());
  void* lgv = lg ? MapViewOfFile(lg, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(psh::GazeOut)) : nullptr;
  HANDLE lb = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(psh::BoostOut), boostN.c_str());
  void* lbv = lb ? MapViewOfFile(lb, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(psh::BoostOut)) : nullptr;

  // The engine opens / creates its blocks as at install and Start.
  fd::g_coneOn = true;
  fd::g_gridOn = true;
  fd::g_testGaze = false;
  fd::OpenMappings();
  ck::OpenBoost(true);
  ck::OpenAck(true);

  // The layer's side opens the engine's DepthIn, DepthGrid and StereoAck under the suffixed names.
  HANDLE ld = nullptr, lgr = nullptr, la = nullptr;
  void *ldv = nullptr, *lgrv = nullptr, *lav = nullptr;
  MapRw(depthN, ld, ldv, sizeof(psh::DepthIn));
  MapRw(gridN, lgr, lgrv, sizeof(psh::DepthGrid));
  MapRw(ackN, la, lav, sizeof(psh::CockpitStereo));

  const bool gaze = !fd::g_gazeOwned && SameBlock(lgv, fd::g_gazeView.load(), 0x7901);
  const bool depth = SameBlock(fd::g_depthView.load(), ldv, 0x7902);
  const bool grid = SameBlock(fd::g_gridView.load(), lgrv, 0x7903);
  const bool boost = SameBlock(lbv, ck::g_boostView.load(), 0x7904);
  const bool ack = SameBlock(ck::g_ackView.load(), lav, 0x7905);
  Check(gaze, "R79 suffixed run: fixation depth reads the layer's GazeOut under the suffixed name (N1)");
  Check(depth, "R79 suffixed run: fixation depth writes DepthIn under the suffixed name (N1)");
  Check(grid, "R79 suffixed run: fixation depth writes DepthGrid under the suffixed name (N1)");
  Check(boost, "R79 suffixed run: the dual camera reads BoostOut under the suffixed name");
  Check(ack, "R79 suffixed run: the dual camera writes StereoAck under the suffixed name");

  // Teardown: the engine's views first (CloseMappings / CloseAck write their stop records), then the layer's.
  fd::CloseMappings();
  ck::CloseBoost();
  ck::CloseAck(0);
  Unmap(ld, ldv), Unmap(lgr, lgrv), Unmap(la, lav), Unmap(lg, lgv), Unmap(lb, lbv);
  fd::g_gazeName = oG, fd::g_depthName = oD, fd::g_gridName = oGr;
  ck::g_boostNameOverride = oB, ck::g_ackNameOverride = oA;
  fd::g_coneOn = oCone, fd::g_gridOn = oGrid, fd::g_testGaze = oTest;
}

inline void Run() {
  Rule();
  SuffixedRun();
}

}  // namespace r79
