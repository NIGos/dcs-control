// Interface between the stable loader (DcsQvCull.dll, loaded once by the Lua
// hook) and the hot-reloadable payload (DcsQvCullPayload.dll).
//
// Every vtable/IAT slot the payload hooks goes through the loader's registry,
// which remembers the slot's true original pointer. When a new payload
// version is loaded it re-patches the slots it wants and receives the true
// originals (never the previous payload's hooks); slots it no longer uses are
// restored by the loader. Old payload modules are stopped but never unloaded:
// a thread may still be executing inside one of their hooks.
#pragma once

#include <atomic>
#include <cstdint>

struct DcsQvLoaderApi {
  uint32_t version;  // kDcsQvLoaderApiVersion
  // Points *slot at hook and returns the slot's true original in *original.
  bool (*patch)(void** slot, void* hook, void** original);
  // True original of a slot (the current value if it was never patched).
  void* (*original)(void** slot);
  // Restores a slot to its true original.
  void (*restore)(void** slot);
  // Appends one line to DcsQvCull.log.
  void (*log)(const char* line);
  // Folder of the loader, with trailing backslash (ini and logs live there).
  const wchar_t* dir;
};

constexpr uint32_t kDcsQvLoaderApiVersion = 1;

// Payload exports.
using DcsQvPayloadStartFn = int (*)(const DcsQvLoaderApi* api);
using DcsQvPayloadStopFn = void (*)();
// Optional export, called before Start: the loader-owned status word the
// payload publishes to (see status_word.h); detached again by Stop.
using DcsQvPayloadSetStatusWordFn = void (*)(std::atomic<uint64_t>* word);
// Optional export, called before Start: the loader-owned dual camera status
// word (DcsQvPayload_SetDualCameraStatusWord); detached again by Stop. Payloads
// without it leave the word 0.
using DcsQvPayloadSetDualCameraStatusWordFn = void (*)(std::atomic<uint64_t>* word);
// Optional export: DCS is shutting down (the Lua hook's exit finalizer). The
// payload's threads stop touching DCS; hooks stay. Waits up to timeoutMs;
// nonzero when its worker stopped. Idempotent.
using DcsQvPayloadQuiesceFn = int (*)(uint32_t timeoutMs);
// Optional export: 1 = simulation stopped, 2 = mission load begins, 3 = the
// local player changed slot (DCS frees the previous scene's objects next;
// recorder jobs in flight are dropped).
using DcsQvPayloadMissionEventFn = void (*)(uint32_t event);
// Optional export (R51 VrFx GVision): the ownship's acceleration in G (body axes, y = normal load) and the model time,
// from the Lua hook's onSimulationFrame (loader export DcsQvCull_GLoad). Any thread; lock-free.
using DcsQvPayloadGLoadFn = void (*)(float gx, float gy, float gz, double modelTime);
// Optional export (R90): nonzero when the payload uses the G samples at all ([VrFx] GVision on). The loader's
// DcsQvCull_GLoadWanted returns it to the Lua hook, whose onSimulationFrame then skips its Export calls.
// Payloads without it count as wanting them (the hook samples every frame, as before).
using DcsQvPayloadGLoadWantedFn = int (*)();

// Loader export (R90): void DcsQvCull_LogAt(const char* line, const SYSTEMTIME* st) appends one line stamped with
// st (the time the payload's asynchronous log path took the line; nullptr = now). Payloads find it in the module
// that holds DcsQvLoaderApi::log; without it they use log (stamped at the write).
//
// Loader exports (loader.cpp), lock-free, callable from any thread:
//   unsigned long long DcsQvCull_Status()      status word (status_word.h)
//   uint64_t DcsQvCull_DualCameraStatus()      R31 option 3, [Perception]
//     CockpitTrueIpd ("dual camera": cockpit at the true eye distance under
//     Depth boost). 0 = not configured, no payload, payload swap or stopped.
//     bit 0       CockpitTrueIpd configured (1 or 2)
//     bit 1       ON: cockpit patched in the last rendered frame or in a
//                 frame of the last second (mode 1, not paused, not
//                 faulted; the frame ended less than 200 ms ago)
//     bit 2       paused: switched off by its key (Alt+Shift+F7)
//     bit 3       waiting for Depth boost: no BoostOut, stale / dry, or boost
//                 scale 1.0
//     bit 4       observe mode (CockpitTrueIpd=2)
//     bit 5       R62: no cockpit drawn (external view): not ON and no
//                 cockpit pass in the last frame or the last second; not a
//                 failure (a panel treats it like bit 3)
//     bits 6-7    0
//     bits 8-15   current boost scale x100, rounded, clamped to 255 (140 =
//                 x1.40); 0 when no fresh BoostOut is read
//     bits 16-31  frames patched in the last second, clamped to 65535
//     bits 32-63  0
//   Published by the payload's worker about once per second
//   (qvstatus::PackDualCamera in status_word.h).
