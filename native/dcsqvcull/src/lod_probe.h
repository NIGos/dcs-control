// Instancer LOD factor probe ([Suite] LodProbe, default 0; R26 §10, Part B).
// LOSSY, measure-once experiment: while armed, DCS's trees, shrubs and
// buildings switch LOD and fade out closer to the camera, in every view. It is
// never armed outside this suite phase, and the original factors are passed
// again (DCS rebuilds its LOD buffers back) before the phase ends.
//
// Path [V edterrainGraphics41.dll 2.9.30, static analysis]: the terrain parse
// (once per frame, all views) copies Visualizer's ParseOptions into the
// terrain singleton and calls InstanceManager2 vt[24] (ETG+0xa3700, slot
// ETG+0x14f5b8) with it. vt[24] compares the factors with its cache
// (IM2+0x258..0x268) and only on a change logs "instancer factors has been
// changed. Update lod buffers." and rescales every object type's LOD switch
// and max distances into the GPU LOD buffers (forest/shrub/single_tree types:
// the forest factors; everything else: instancerDetailsFactor; indexed
// objects: +0x50). The options struct (0x60 bytes) holds +0x10 double
// instancerDetailsFactor, +0x48 float forestDistanceFactor, +0x4c float
// forestDetailsFactor, +0x50 double (unnamed).
//
// Phase: GPU pass timing (stock factors) -> the slot wrapped, the call gets a
// copy with +0x10, +0x4c and +0x50 multiplied by [Suite] LodProbeScale (and
// +0x48 by it too, clamped to DCS's [0.3, 1]) -> settle, GPU pass timing ->
// slot restored, the original values passed again -> settle, GPU pass timing.
// The factors are global: every view and the cascades change. The peripheral
// G-buffer executions' delta is the per-view potential; the cascades' delta
// is only reachable globally.
//
// Included once from main.cpp after gpu_pass_timing.h.
#pragma once

namespace lodp {

constexpr uint32_t kSlotRva = 0x14f5b8;
constexpr uint32_t kFnRva = 0xa3700;
constexpr size_t kOptBytes = 0x60;

using Fn = uint64_t(__fastcall*)(void*, void*, void*, void*);
Fn g_orig = nullptr;
void** g_slot = nullptr;
std::atomic<double> g_scale{1.0};
std::atomic<uint64_t> g_calls{0};
double g_seen[4] = {};  // last original +0x10, +0x48, +0x4c, +0x50

bool CopyScaled(const void* src, uint8_t* dst, double s) {
  __try {
    memcpy(dst, src, kOptBytes);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
  double d10, d50;
  float f48, f4c;
  memcpy(&d10, dst + 0x10, 8);
  memcpy(&f48, dst + 0x48, 4);
  memcpy(&f4c, dst + 0x4c, 4);
  memcpy(&d50, dst + 0x50, 8);
  g_seen[0] = d10, g_seen[1] = f48, g_seen[2] = f4c, g_seen[3] = d50;
  d10 *= s, d50 *= s;
  f4c = static_cast<float>(f4c * s);
  f48 = static_cast<float>(std::min(1.0, std::max(0.3, f48 * s)));
  memcpy(dst + 0x10, &d10, 8);
  memcpy(dst + 0x48, &f48, 4);
  memcpy(dst + 0x4c, &f4c, 4);
  memcpy(dst + 0x50, &d50, 8);
  return true;
}

uint64_t __fastcall Hook(void* self, void* opt, void* c, void* d) {
  g_calls.fetch_add(1, std::memory_order_relaxed);
  const double s = g_scale.load(std::memory_order_relaxed);
  alignas(16) uint8_t copy[kOptBytes];
  if (s != 1.0 && opt && CopyScaled(opt, copy, s)) return g_orig(self, copy, c, d);
  return g_orig(self, opt, c, d);
}

bool Install() {
  if (g_slot) return true;
  HMODULE m = GetModuleHandleW(L"edterrainGraphics41.dll");
  if (!m) {
    Log("  lod probe: edterrainGraphics41.dll not loaded");
    return false;
  }
  auto** slot = reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(m) + kSlotRva);
  void* cur = SlotOriginal(slot);
  if (cur != reinterpret_cast<uint8_t*>(m) + kFnRva) {
    Log("  lod probe: ETG+0x%x holds %p, not ETG+0x%x (other DCS build); not installed", kSlotRva, cur, kFnRva);
    return false;
  }
  g_orig = reinterpret_cast<Fn>(cur);
  if (!HookSlot(slot, reinterpret_cast<void*>(&Hook), nullptr)) {
    Log("  lod probe: could not patch the slot");
    return false;
  }
  g_slot = slot;
  return true;
}

void Uninstall() {
  if (!g_slot) return;
  UnhookSlot(g_slot, reinterpret_cast<void*>(g_orig));
  g_slot = nullptr;
}

// gpt::Measure three times: stock, scaled, stock again.
void RunPhase(double scale, int ms, std::atomic<uint64_t>& frameCounter) {
  Log("  lod probe: A = stock factors");
  gpt::Measure(ms, frameCounter);
  if (!Install()) return;
  g_calls = 0;
  g_scale = scale;
  Sleep(3000);  // DCS rebuilds the LOD buffers on the next parse; GPU-driven culling follows at once
  Log("  lod probe: B = instancer factors x %.2f (wrapper called %.1f/s; DCS's values +0x10 %.4f, +0x48 %.4f, "
      "+0x4c %.4f, +0x50 %.4f)",
      scale, g_calls.load() / 3.0, g_seen[0], g_seen[1], g_seen[2], g_seen[3]);
  gpt::Measure(ms, frameCounter);
  g_scale = 1.0;
  Sleep(1000);  // at least one call with the original values: DCS rebuilds back
  Uninstall();
  Sleep(2000);
  Log("  lod probe: C = stock factors again (slot restored)");
  gpt::Measure(ms, frameCounter);
}

}  // namespace lodp
