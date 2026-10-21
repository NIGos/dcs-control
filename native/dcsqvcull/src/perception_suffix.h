// R79 (R77c N1-N3): the one reader of DCSVR_PERCEPTION_SHM_SUFFIX on the engine side.
//
// The Perception layer's shared blocks (dcsvr_perception_shm.h) and the frame-generation exports (dcsvr_cockpit_mask.h,
// dcsvr_cockpit_depth.h, dcsvr_motion.h) take an optional name suffix from this variable; the offline harnesses set it
// so a test run never touches the blocks of a DCS that is running at the same time. Production leaves it unset.
//
// Rule (the same as the peer's pupil_shift_layer.cpp at xrCreateInstance): GetEnvironmentVariableW into a 64-wchar
// buffer; the suffix is empty when the variable is unset, empty, or 64 characters or longer (it does not fit, and the
// peer's buffer then keeps its zeroed start). So at most 63 characters are used, wide as they are (no narrowing or
// zero-extending of bytes), and a value set with SetEnvironmentVariable at run time is seen (no CRT copy involved).
//
// When it is read: on every call. The engine calls it at each open attempt of a block (fixation_depth.h OpenMappings,
// cockpit_ipd.h BoostName / AckName) and at each install of the cockpit mask export
// (cockpit_mask.h Configure, whose names stay fixed while it is installed). A block already open keeps the name it was
// opened with. The layer reads the variable once per instance; neither side expects it to change during a flight.
//
// Included from fixation_depth.h inside main.cpp's anonymous namespace, before its own namespace: no standard headers here, main.cpp has <windows.h> and <string>.
#pragma once

namespace perc_sfx {

constexpr const wchar_t* kVariable = L"DCSVR_PERCEPTION_SHM_SUFFIX";  // = dcsvr_perception::name_suffix_variable
constexpr DWORD kBufChars = 64;                                      // the peer's shm_suffix[64]

// The suffix now ("" when none).
inline std::wstring SuffixW() {
  wchar_t buf[kBufChars] = {};
  const DWORD k = GetEnvironmentVariableW(kVariable, buf, kBufChars);
  if (k == 0 || k >= kBufChars || buf[0] == 0) return std::wstring();
  return std::wstring(buf, k);
}

// A block's mapping name: its base name plus the suffix now.
inline std::wstring Name(const wchar_t* base) { return std::wstring(base) + SuffixW(); }

}  // namespace perc_sfx
