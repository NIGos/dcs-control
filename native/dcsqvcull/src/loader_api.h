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
// Optional export: DCS is shutting down (the Lua hook's exit finalizer). The
// payload's threads stop touching DCS; hooks stay. Waits up to timeoutMs;
// nonzero when its worker stopped. Idempotent.
using DcsQvPayloadQuiesceFn = int (*)(uint32_t timeoutMs);
// Optional export: 1 = simulation stopped, 2 = mission load begins, 3 = the
// local player changed slot (DCS frees the previous scene's objects next;
// recorder jobs in flight are dropped).
using DcsQvPayloadMissionEventFn = void (*)(uint32_t event);
