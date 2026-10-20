// R59 L1: after DcsQvPayload_Stop, the old payload's frame heap and model
// allocator hooks pass straight through (the loader keeps them patched until
// the next generation repoints each slot, one at a time).
#pragma once

namespace r59stop {

uint8_t* g_fakeHeap = nullptr;
uint32_t g_origCalls = 0;

// edCore's quick_get_memory as frame_heap.h sees it: limit at +0x10, cursor at +0x18.
void* __fastcall FakeGet(void* heap, size_t size) {
  ++g_origCalls;
  uint8_t* h = static_cast<uint8_t*>(heap);
  uint8_t* cur = *reinterpret_cast<uint8_t**>(h + 0x18);
  *reinterpret_cast<uint8_t**>(h + 0x18) = cur + ((size + 15) & ~static_cast<size_t>(15));
  return cur;
}

void Run() {
  const frameheap::GetFn savedOrig = frameheap::g_orig;
  const bool savedFh = g_frameHeapOn.load(), savedAs = g_allocSlabsOn.load(), savedTc = timercache::g_on.load();
  const size_t kArena = 4u << 20;
  std::vector<uint8_t> mem(kArena + 0x40);
  uint8_t* heap = mem.data();
  uint8_t* arena = mem.data() + 0x40;
  *reinterpret_cast<uint8_t**>(heap + 0x10) = arena + kArena;
  *reinterpret_cast<uint8_t**>(heap + 0x18) = arena;
  frameheap::g_orig = &FakeGet;
  g_frameHeapOn = true;
  g_allocSlabsOn = true;
  frameheap::g_gen.fetch_add(1);  // no slab of an earlier test
  // Old payload: a thread takes a slab, serves a second object from it.
  void* a = frameheap::HookGet(heap, 32);
  void* b = frameheap::HookGet(heap, 32);
  const bool slabbed = a == arena && b == arena + 32 && g_origCalls == 1;
  // Stop. Then the new payload hooks the reset slot first: DCS resets the heap
  // through the NEW hook (the old generation is not bumped) while this old
  // get hook is still patched in a get slot.
  PassThroughAfterStop();
  *reinterpret_cast<uint8_t**>(heap + 0x18) = arena;  // reset_memory: the cursor back to the base
  void* c = frameheap::HookGet(heap, 32);   // must be the original's (the cursor), not the stale slab
  void* d = FakeGet(heap, 32);              // the original's next object
  Check(slabbed && c == arena && d == arena + 16 * 2 && c != b && !g_frameHeapOn.load() && !g_allocSlabsOn.load() &&
            !timercache::g_on.load(),
        "R59 L1: after payload Stop the old frame heap / alloc slab hooks pass through (no stale slab after a reset "
        "hooked by the new payload)");
  frameheap::g_gen.fetch_add(1);  // this thread's slab points at a freed heap: never served again
  frameheap::g_orig = savedOrig;
  g_frameHeapOn = savedFh;
  g_allocSlabsOn = savedAs;
  timercache::g_on = savedTc;
}

}  // namespace r59stop
