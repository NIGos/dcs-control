// Per-thread slabs for edCore's frame heap (TFrameMemoryHeap<1>).
//
// quick_get_memory (edCore 0x20e40) bumps one shared cursor (this+0x18) with
// a locked xadd for every RenderData and renderable the culling tasks create,
// so that cache line bounces between all pool threads. With slabs on, a
// thread takes 4 KB at a time from the original and serves its next requests
// from it. Memory comes from the same heap, 16-byte aligned like the
// original; only addresses differ.
//
// Invalidation: reset_memory<1> is only ever called through the import tables
// of Scene.dll, GraphicsVista.dll and Visualizer.dll (no internal callers in
// edCore [V]); those three imports are hooked to start a new slab generation.
// Near the end of the heap (less than 1 MB left) slabs are not used, so the
// original's behaviour when it runs out is unchanged.
// Included once from main.cpp inside its anonymous namespace.
#pragma once

namespace frameheap {

using GetFn = void*(__fastcall*)(void* heap, size_t size);
using ResetFn = void(__fastcall*)(void* heap);
GetFn g_orig = nullptr;
ResetFn g_origReset = nullptr;
std::atomic<uint64_t>& g_gen = g_frameHeapGen;  // main.cpp: the recorders read it too
std::atomic<bool>& g_on = g_frameHeapOn;
constexpr size_t kSlab = 4096;
constexpr size_t kMinFree = 1 << 20;

struct Slab {
  uint64_t gen = 0;
  void* heap = nullptr;
  uint8_t* cur = nullptr;
  uint8_t* end = nullptr;
};

void* __fastcall HookGet(void* heap, size_t size) {
  if (!g_on.load(std::memory_order_relaxed)) return g_orig(heap, size);
  const size_t need = (size + 15) & ~static_cast<size_t>(15);
  thread_local Slab s;
  const uint64_t gen = g_gen.load(std::memory_order_acquire);
  if (s.gen == gen && s.heap == heap && static_cast<size_t>(s.end - s.cur) >= need) {
    uint8_t* p = s.cur;
    s.cur += need;
    return p;
  }
  if (need > kSlab / 4) return g_orig(heap, size);
  const uint8_t* cursor = *reinterpret_cast<uint8_t* const volatile*>(static_cast<uint8_t*>(heap) + 0x18);
  const uint8_t* limit = *reinterpret_cast<uint8_t* const*>(static_cast<uint8_t*>(heap) + 0x10);
  if (limit < cursor || static_cast<size_t>(limit - cursor) < kMinFree) return g_orig(heap, size);
  auto* block = static_cast<uint8_t*>(g_orig(heap, kSlab));
  if (!block) return g_orig(heap, size);
  s = {gen, heap, block + need, block + kSlab};
  return block;
}

template <int N>
void __fastcall HookReset(void* heap) {
  g_gen.fetch_add(1, std::memory_order_acq_rel);
  g_origReset(heap);
}

std::atomic<int> g_state{0};

bool Install() {
  if (g_state.load() != 0) return g_state.load() > 0;
  g_state = -1;
  auto* ed = GetModuleHandleW(L"edCore.dll");
  if (!ed) return false;
  auto get = reinterpret_cast<GetFn>(GetProcAddress(ed, "?quick_get_memory@?$TFrameMemoryHeap@$00@@QEAAPEAX_K@Z"));
  auto reset = reinterpret_cast<ResetFn>(GetProcAddress(ed, "?reset_memory@?$TFrameMemoryHeap@$00@@QEAAXXZ"));
  if (!get || !reset) return false;
  g_orig = get;
  g_origReset = reset;
  const wchar_t* resetUsers[] = {L"Scene.dll", L"GraphicsVista.dll", L"Visualizer.dll"};
  void* const resetHooks[] = {reinterpret_cast<void*>(&HookReset<0>), reinterpret_cast<void*>(&HookReset<1>),
                              reinterpret_cast<void*>(&HookReset<2>)};
  int resets = 0;
  for (int i = 0; i < 3; ++i) {
    HMODULE m = GetModuleHandleW(resetUsers[i]);
    void** slot = m ? timercache::FindImport(m, "edCore.dll", "?reset_memory@?$TFrameMemoryHeap@$00@@QEAAXXZ") : nullptr;
    if (slot && HookSlot(slot, resetHooks[i], nullptr)) ++resets;
  }
  if (resets != 3) {
    Log("frame heap: could not hook every reset_memory import (%d/3); not installed", resets);
    return false;
  }
  const wchar_t* getUsers[] = {L"NGModel.dll", L"Scene.dll", L"GraphicsVista.dll", L"GraphicsCore.dll", L"Visualizer.dll"};
  int gets = 0;
  for (const wchar_t* u : getUsers) {
    HMODULE m = GetModuleHandleW(u);
    void** slot = m ? timercache::FindImport(m, "edCore.dll", "?quick_get_memory@?$TFrameMemoryHeap@$00@@QEAAPEAX_K@Z") : nullptr;
    if (slot && HookSlot(slot, reinterpret_cast<void*>(&HookGet), nullptr)) ++gets;
  }
  g_state = 1;
  Log("frame heap: quick_get_memory hooked in %d modules, reset_memory in 3", gets);
  return true;
}

}  // namespace frameheap
