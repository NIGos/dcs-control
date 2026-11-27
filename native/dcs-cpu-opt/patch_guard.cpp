#include <windows.h>

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "../dcsqvcull/src/patch_threads.h"

extern "C" int DcsCpuOpt_WriteGuardedPatch(uint8_t* target, const uint8_t* bytes, size_t length,
                                           const uint8_t* expectedAddr, const uint8_t* expected,
                                           size_t expectedLength) {
  const qvpatch::Site site{target, bytes, length, expected, expectedAddr, expectedLength};
  // A failed thread walk or context check must never fall back to an unverified write.
  return qvpatch::WriteSuspendedProcess(&site, 1, 20, true) ? 1 : 0;
}

extern "C" void* DcsCpuOpt_AllocNearPatch(const void* target, size_t length) {
  const HMODULE kernel = GetModuleHandleW(L"kernelbase.dll");
  using Alloc2 = void*(WINAPI*)(HANDLE, void*, size_t, ULONG, ULONG, MEM_EXTENDED_PARAMETER*, ULONG);
  const auto alloc = kernel ? reinterpret_cast<Alloc2>(GetProcAddress(kernel, "VirtualAlloc2")) : nullptr;
  if (!alloc) return nullptr;

  SYSTEM_INFO info{};
  GetSystemInfo(&info);
  const uintptr_t site = reinterpret_cast<uintptr_t>(target);
  const uintptr_t minAddr = reinterpret_cast<uintptr_t>(info.lpMinimumApplicationAddress);
  const uintptr_t maxAddr = reinterpret_cast<uintptr_t>(info.lpMaximumApplicationAddress);
  // A 1 GiB interval on either side fits easily in a rel32 jump, including alignment.
  constexpr uintptr_t radius = 0x40000000;
  const uintptr_t low = site > minAddr + radius ? site - radius : minAddr;
  const uintptr_t high = maxAddr - site > radius ? site + radius : maxAddr;
  const uintptr_t alignMask = static_cast<uintptr_t>(info.dwAllocationGranularity) - 1;
  MEM_ADDRESS_REQUIREMENTS requirements{};
  requirements.LowestStartingAddress = reinterpret_cast<void*>((low + alignMask) & ~alignMask);
  requirements.HighestEndingAddress = reinterpret_cast<void*>(((high + 1) & ~alignMask) - 1);
  MEM_EXTENDED_PARAMETER parameter{};
  parameter.Type = MemExtendedParameterAddressRequirements;
  parameter.Pointer = &requirements;
  return alloc(GetCurrentProcess(), nullptr, length, MEM_RESERVE | MEM_COMMIT,
               PAGE_EXECUTE_READWRITE, &parameter, 1);
}
