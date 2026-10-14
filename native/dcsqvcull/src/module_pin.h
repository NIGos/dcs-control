// Pins the module containing an address: FreeLibrary never unmaps it again
// (it stays until the process ends). Used by the loader (loader.cpp), which
// DCS's Lua frees when it closes the hooks' state at exit while the payload
// it loaded still runs.
#pragma once

#include <windows.h>

namespace qvpin {

inline bool PinModuleOf(const void* addressInModule) {
  HMODULE m = nullptr;
  return GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                            static_cast<LPCWSTR>(addressInModule), &m) != FALSE &&
         m != nullptr;
}

}  // namespace qvpin
