// Model instance-data allocator: per-thread slabs, plus counters.
//
// NGModel's StructBufferManager::allocate (RVA 0xbf30) hands out per-instance
// data slots (matrices, per-object constants) to every culling task. Each
// call takes one global ed::mutex and scans every page for one with room
// (0xc1b0). With 7 pool threads parsing models, about half of ParseMT2's CPU
// is spent waiting for that lock (profile 2026-10-07).
//
// With slabs on, a thread reserves a larger run of slots (SlabBytes) under the
// original lock and then serves its following requests from it without
// locking. Every allocation still gets its own slots in the same page memory;
// only their positions differ, and those already change every frame with
// thread scheduling. Unused slab tails are uploaded but never referenced.
//
// Slabs are invalidated (generation bump) by the per-frame reset (vtable slot
// 7, 0xbd80, the only path that resets the pages) and at the start of every
// culling call.
//
// The function is not virtual: it is detoured with a 14-byte absolute jump to
// a persistent stub whose target pointer is registered with the loader, so
// payload hot reloads switch hooks like any vtable slot.
//
// Included once from main.cpp inside its anonymous namespace.
#pragma once

namespace posesweep {
bool PatchAllSuspended(uint8_t* addr, const uint8_t* bytes, size_t len);  // pose_sweep.h
}

namespace allocslab {

constexpr uint32_t kAllocRva = 0xbf30;
constexpr uint32_t kScanRva = 0xc1b0;
constexpr uint32_t kVtableRva = 0x58650;  // StructBufferManager (RTTI)
constexpr int kResetSlot = 7;
constexpr uint32_t kLockIat = 0x57720;    // edCore ?lock@mutex@ed@@
constexpr uint32_t kUnlockIat = 0x57718;  // edCore ?unlock@mutex@ed@@
const uint8_t kPrologue[14] = {0x48, 0x89, 0x5c, 0x24, 0x10, 0x48, 0x89, 0x6c, 0x24, 0x18, 0x56, 0x57, 0x41, 0x56};
const uint8_t kScanPrologue[8] = {0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x6c};

// Page record in the manager's vector (begin +8, end +0x10), stride 0x30.
struct Page {
  uint32_t idx, elemSize, capCount, capBytes, prevUsed, used;
  uint8_t* data;
  void* gpuBuf;
};
static_assert(sizeof(Page) == 0x28, "page layout");
constexpr size_t kPageStride = 0x30;
constexpr size_t kMgrMutex = 0x20;

using AllocFn = uint8_t*(__fastcall*)(void* mgr, uint32_t size, uint32_t count, uint32_t* page, uint32_t* index);
using ScanFn = Page*(__fastcall*)(void* mgr, uint32_t size, uint32_t count);
using MutexFn = void(__fastcall*)(void* mutex);
using ResetFn = void(__fastcall*)(void* mgr);

// Persistent detour: two pages, never freed, found again after payload
// reloads from the patched bytes. The first page holds code (stub and
// trampoline) and stays execute-read. The second page holds the target
// pointer: the loader changes that page's protection to write it, which
// must never touch a page other threads are executing (a hot reload while
// culling tasks ran through the stub crashed DCS on 2026-10-08).
struct DetourPage {
  uint8_t stub[16];   // jmp [rip -> slot in the data page]
  uint8_t tramp[32];  // original 14 bytes + jmp back
  uint32_t magic;
  uint32_t pad2;
  uint8_t* target;
};
constexpr uint32_t kMagic = 0x32515644;  // "DVQ2": code page + separate data page
constexpr size_t kPage = 4096;
inline void** SlotOf(DetourPage* pg) { return reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(pg) + kPage); }

std::atomic<bool>& g_on = g_allocSlabsOn;
std::atomic<bool>& texbind_measure() { return g_hookMeasure; }  // slabs active (else pass-through with counters)
std::atomic<uint64_t> g_gen{1};
std::atomic<uint32_t>& g_slabBytes = g_allocSlabBytes;
AllocFn g_tramp = nullptr;
ScanFn g_scan = nullptr;
MutexFn g_lock = nullptr, g_unlock = nullptr;
ResetFn g_origReset = nullptr;
void** g_resetSlot = nullptr;
DetourPage* g_page = nullptr;
std::atomic<int> g_state{0};  // 0 = not installed, 1 = patch pending (next collect), 2 = active, -1 = failed
std::atomic<void*> g_mgr{nullptr};
std::atomic<uint64_t> g_resets{0};
// Measurement observers (du_count.h), null outside its suite phase.
std::atomic<void (*)(uint32_t bytes)> g_allocObs{nullptr};
std::atomic<void (*)()> g_collectObs{nullptr};
std::atomic<void (*)(void* mgr)> g_resetObs{nullptr};
// Direct upload (direct_upload.h): every allocation's page and element index
// (verify phase only, else null), and the per-frame reset (null until it is
// installed).
std::atomic<void (*)(uint32_t size, uint32_t count, uint32_t page, uint32_t index)> g_blockObs{nullptr};
std::atomic<void (*)(void* mgr)> g_duReset{nullptr};

struct Counters {
  uint64_t calls = 0, bytes = 0, slow = 0, cycles = 0;
};
std::mutex g_regMutex;
std::vector<Counters*> g_counters;

struct Slab {
  uint64_t gen = 0;
  void* mgr = nullptr;
  uint32_t size = 0, pageIdx = 0, cur = 0, end = 0;
  uint8_t* data = nullptr;
};
constexpr int kSlabs = 6;
struct ThreadState {
  Counters* c = nullptr;
  Slab slabs[kSlabs];
  int next = 0;
};

ThreadState& Tls() {
  thread_local ThreadState t;
  if (!t.c) {
    t.c = new Counters;
    std::lock_guard<std::mutex> lock(g_regMutex);
    g_counters.push_back(t.c);
  }
  return t;
}

void Invalidate() { g_gen.fetch_add(1, std::memory_order_acq_rel); }

uint8_t* AllocImpl(void* mgr, uint32_t size, uint32_t count, uint32_t* page, uint32_t* index) {
  ThreadState& t = Tls();
  const uint32_t need = size * count;
  t.c->calls++;
  t.c->bytes += need;
  if (auto* obs = g_allocObs.load(std::memory_order_relaxed)) obs(need);
  if (g_mgr.load(std::memory_order_relaxed) != mgr) g_mgr.store(mgr, std::memory_order_relaxed);
  const bool measure = texbind_measure().load(std::memory_order_relaxed);
  uint64_t c0 = measure ? __rdtsc() : 0;
  if (!g_on.load(std::memory_order_relaxed) || size == 0 || need == 0) {
    uint8_t* r = g_tramp(mgr, size, count, page, index);
    if (measure) t.c->cycles += __rdtsc() - c0;
    return r;
  }
  const uint64_t gen = g_gen.load(std::memory_order_acquire);
  for (Slab& s : t.slabs) {
    if (s.gen == gen && s.mgr == mgr && s.size == size && s.end - s.cur >= need) {
      uint32_t off = s.cur;
      s.cur += need;
      *page = s.pageIdx;
      *index = off / size;
      if (measure) t.c->cycles += __rdtsc() - c0;
      return s.data + off;
    }
  }
  // Slow path: reserve a slab under the original lock, with the original
  // page search (which also creates pages exactly as DCS does).
  t.c->slow++;
  uint8_t* mutex = static_cast<uint8_t*>(mgr) + kMgrMutex;
  g_lock(mutex);
  Page* p = g_scan(mgr, size, count);
  const uint32_t used = p->used;
  const uint32_t room = p->capBytes - used;
  uint32_t want = std::max(need, (g_slabBytes.load(std::memory_order_relaxed) / size) * size);
  uint32_t take = std::min(room, want);
  if (take < need) take = need;  // the search guarantees room for `need`
  p->used = used + take;
  const uint32_t pageIdx = p->idx;
  uint8_t* data = p->data;
  g_unlock(mutex);

  Slab* s = nullptr;
  for (Slab& x : t.slabs)
    if (x.size == size && x.mgr == mgr) s = &x;
  if (!s) {
    s = &t.slabs[t.next];
    t.next = (t.next + 1) % kSlabs;
  }
  *s = {gen, mgr, size, pageIdx, used + need, used + take, data};
  *page = pageIdx;
  *index = used / size;
  if (measure) t.c->cycles += __rdtsc() - c0;
  return data + used;
}

uint8_t* __fastcall HookAlloc(void* mgr, uint32_t size, uint32_t count, uint32_t* page, uint32_t* index) {
  uint8_t* r = AllocImpl(mgr, size, count, page, index);
  if (auto* obs = g_blockObs.load(std::memory_order_relaxed)) obs(size, count, *page, *index);
  return r;
}

// Per element-size class: peak bytes used in one frame and page count, read
// at the per-frame reset (before it clears `used`). For sizing bigger pages.
struct ClassStat {
  uint32_t elemSize = 0;
  uint32_t peakBytes = 0, peakPages = 0, lastBytes = 0, pages = 0;
};
ClassStat g_classes[8];
void NoteClasses(void* mgr) {
  auto* b = *reinterpret_cast<uint8_t**>(static_cast<uint8_t*>(mgr) + 8);
  auto* e = *reinterpret_cast<uint8_t**>(static_cast<uint8_t*>(mgr) + 0x10);
  uint32_t bytes[8] = {}, pages[8] = {}, total[8] = {};
  for (uint8_t* p = b; p && p < e; p += kPageStride) {
    const auto* pg = reinterpret_cast<const Page*>(p);
    int c = 0;
    while (c < 8 && g_classes[c].elemSize && g_classes[c].elemSize != pg->elemSize) ++c;
    if (c == 8) continue;
    g_classes[c].elemSize = pg->elemSize;
    ++total[c];
    if (pg->used) {
      bytes[c] += pg->used;
      ++pages[c];
    }
  }
  for (int c = 0; c < 8 && g_classes[c].elemSize; ++c) {
    g_classes[c].lastBytes = bytes[c];
    g_classes[c].pages = total[c];
    if (bytes[c] > g_classes[c].peakBytes) g_classes[c].peakBytes = bytes[c];
    if (pages[c] > g_classes[c].peakPages) g_classes[c].peakPages = pages[c];
  }
}

void LogClasses() {
  for (int c = 0; c < 8 && g_classes[c].elemSize; ++c)
    Log("  model data pages, element size %u: last frame %u bytes, peak %u bytes in %u pages (%u pages exist)",
        g_classes[c].elemSize, g_classes[c].lastBytes, g_classes[c].peakBytes, g_classes[c].peakPages,
        g_classes[c].pages);
}

void (*g_onReset)(void* mgr) = nullptr;  // set by big_pages.h

void __fastcall HookReset(void* mgr) {
  if (auto* obs = g_resetObs.load(std::memory_order_relaxed)) obs(mgr);
  if (auto* du = g_duReset.load(std::memory_order_acquire)) du(mgr);
  NoteClasses(mgr);
  if (g_onReset) g_onReset(mgr);
  Invalidate();
  g_resets.fetch_add(1, std::memory_order_relaxed);
  g_origReset(mgr);
}

bool ReadBytes(const void* p, void* out, size_t n) {
  __try {
    memcpy(out, p, n);
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

bool RttiIs(uint8_t* base, void** vtbl, const char* name) {
  void* col = nullptr;
  if (!ReadBytes(vtbl - 1, &col, sizeof(col)) || !col) return false;
  uint32_t td = 0;
  if (!ReadBytes(static_cast<uint8_t*>(col) + 12, &td, 4)) return false;
  char buf[64] = {};
  if (!ReadBytes(base + td + 16, buf, sizeof(buf) - 1)) return false;
  return strcmp(buf, name) == 0;
}

// Checks the build and prepares everything except the code patch, which is
// written at a quiet moment (the next collect call, on the render thread,
// before any culling task of that frame runs).
bool Prepare() {
  // R144: callers on different threads (the worker's live enable / DirectUpload, the suite thread's
  // InstallDiagnostics) are serialised: two concurrent first calls would both build a detour page.
  static std::mutex s_prepareMu;
  std::lock_guard<std::mutex> prepareLock(s_prepareMu);
  if (g_state.load() != 0) return g_state.load() > 0;
  auto* base = reinterpret_cast<uint8_t*>(GetModuleHandleW(L"NGModel.dll"));
  if (!base) {
    Log("model allocator: NGModel.dll not loaded");
    g_state = -1;
    return false;
  }
  // Recorded RVAs, or where reloc.h re-found them in another build (0: not found).
  const uint32_t vtRva = reloc::Rva(hooksig::NG_StructBufferManager_vtbl, kVtableRva);
  const uint32_t allocRva = reloc::Rva(hooksig::NG_StructBufferManager_allocate, kAllocRva);
  const uint32_t scanRva = reloc::Rva(hooksig::NG_StructBufferManager_scan, kScanRva);
  const uint32_t lockIat = reloc::Rva(hooksig::NG_iat_ed_mutex_lock, kLockIat);
  const uint32_t unlockIat = reloc::Rva(hooksig::NG_iat_ed_mutex_unlock, kUnlockIat);
  auto** vtbl = reinterpret_cast<void**>(base + vtRva);
  uint8_t scanBytes[8];
  uint8_t cur[14];
  if (!vtRva || !allocRva || !scanRva || !lockIat || !unlockIat ||
      !RttiIs(base, vtbl, ".?AVStructBufferManager@model@@") ||
      !reloc::SlotIs(hooksig::NG_StructBufferManager_reset, base, SlotOriginal(&vtbl[kResetSlot])) ||
      !ReadBytes(base + scanRva, scanBytes, 8) || memcmp(scanBytes, kScanPrologue, 8) != 0 ||
      !ReadBytes(base + allocRva, cur, 14)) {
    Log("model allocator: NGModel.dll does not match this build; not installed");
    g_state = -1;
    return false;
  }
  g_scan = reinterpret_cast<ScanFn>(base + scanRva);
  g_lock = *reinterpret_cast<MutexFn*>(base + lockIat);
  g_unlock = *reinterpret_cast<MutexFn*>(base + unlockIat);

  // R144: the state (1 = patch pending, 2 = active) is published only after the reset and target hooks are
  // registered, so the render thread's OnCollect never writes the detour (and a failed registration below can never
  // be overwritten by OnCollect's "2") while the hooks are half set up. That matters for a live enable, which runs
  // this on the worker while the render thread is collecting; at the start it was benign (the stub's slot points at
  // the trampoline until HookAlloc is registered: pass-through).
  int next = 0;
  if (memcmp(cur, kPrologue, 14) == 0) {
    auto* pg = static_cast<DetourPage*>(VirtualAlloc(nullptr, 2 * kPage, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (!pg) {
      g_state = -1;
      return false;
    }
    memset(pg, 0xCC, sizeof(DetourPage));
    // stub: jmp qword ptr [rip + (slot - (stub + 6))], slot at the start of the data page
    int32_t rel = static_cast<int32_t>(kPage - 6);
    pg->stub[0] = 0xFF;
    pg->stub[1] = 0x25;
    memcpy(pg->stub + 2, &rel, 4);
    memcpy(pg->tramp, kPrologue, 14);
    uint8_t* back = base + allocRva + 14;
    const uint8_t jmpAbs[6] = {0xFF, 0x25, 0, 0, 0, 0};
    memcpy(pg->tramp + 14, jmpAbs, 6);
    memcpy(pg->tramp + 20, &back, 8);
    *SlotOf(pg) = pg->tramp;
    pg->magic = kMagic;
    pg->target = base + allocRva;
    DWORD oldProt;
    if (!VirtualProtect(pg, kPage, PAGE_EXECUTE_READ, &oldProt)) {
      g_state = -1;
      return false;
    }
    FlushInstructionCache(GetCurrentProcess(), pg, kPage);
    g_page = pg;
    next = 1;  // patch on the next collect
  } else if (cur[0] == 0xFF && cur[1] == 0x25 && cur[2] == 0 && cur[3] == 0 && cur[4] == 0 && cur[5] == 0) {
    // Already patched by an earlier payload: reuse its page.
    DetourPage* pg = nullptr;
    memcpy(&pg, cur + 6, 8);
    uint32_t magic = 0;
    if (!pg || !ReadBytes(&pg->magic, &magic, 4) || magic != kMagic || pg->target != base + allocRva) {
      Log("model allocator: unknown patch at NGModel+0x%x; not installed", allocRva);
      g_state = -1;
      return false;
    }
    g_page = pg;
    next = 2;
  } else {
    Log("model allocator: NGModel+0x%x has unexpected bytes; not installed", allocRva);
    g_state = -1;
    return false;
  }
  g_tramp = reinterpret_cast<AllocFn>(static_cast<void*>(g_page->tramp));
  g_resetSlot = &vtbl[kResetSlot];
  g_origReset = reinterpret_cast<ResetFn>(SlotOriginal(g_resetSlot));
  if (!HookSlot(g_resetSlot, reinterpret_cast<void*>(&HookReset), nullptr) ||
      !HookSlot(SlotOf(g_page), reinterpret_cast<void*>(&HookAlloc), nullptr)) {
    Log("model allocator: hook registration failed");
    g_state = -1;
    return false;
  }
  g_state.store(next, std::memory_order_release);
  if (next == 2) Log("model allocator: hooked (existing detour reused)");
  return true;
}

// R144: a live enable of [Model] AllocSlabs (ini 0 -> 1, or the kill switch back ON) prepares the detour when nothing
// did yet (AllocSlabs off at the start, no Diagnostics, no DirectUpload, no suite): after the worker's start installs,
// with NGModel.dll loaded (Prepare latches a missing module as a failure), never in PerceptionOnly (LoadConfig returns
// before). The 14-byte patch itself is then written by the next collect call exactly as after a start Prepare:
// every other thread suspended and none inside the 14 bytes (PatchAllSuspended), before that frame's culling tasks.
// Why the swap is safe mid-session (docs/research/R144_live_phase2.md): StructBufferManager has no per-object free
// (the per-frame reset, slot 7, rewinds every page's used count, the only release), a slab is carved under DCS's
// own lock by advancing the same page's used count (so DCS's path never hands out a byte of it), thread state is
// created lazily on a thread's first call (generation 0 never matches), and g_on is read per call.
inline bool LiveInstallWanted(bool on, int state, bool startInstallsDone, bool ngModelLoaded) {
  return on && state == 0 && startInstallsDone && ngModelLoaded;
}

// Called by the collect hook before the original runs: writes the pending
// detour, and starts a new slab generation for this culling call.
void OnCollect() {
  if (auto* obs = g_collectObs.load(std::memory_order_relaxed)) obs();
  Invalidate();
  int expect = 1;
  if (!g_state.compare_exchange_strong(expect, 3)) return;
  uint8_t* target = g_page->target;
  uint8_t patch[14] = {0xFF, 0x25, 0, 0, 0, 0};
  DetourPage* pg = g_page;
  memcpy(patch + 6, &pg, 8);
  // With every other thread suspended and none inside the 14 bytes (as the
  // other code patches do): a plain memcpy could be executed half-written by
  // a thread entering allocate outside this collect's culling tasks.
  if (!posesweep::PatchAllSuspended(target, patch, 14)) {
    Log("model allocator: StructBufferManager::allocate could not be patched; not installed");
    g_state = -1;
    return;
  }
  g_state = 2;
  Log("model allocator: StructBufferManager::allocate detoured (slabs %s, %u bytes)", g_on.load() ? "ON" : "OFF",
      g_slabBytes.load());
}

struct Totals {
  uint64_t calls = 0, bytes = 0, slow = 0, cycles = 0, resets = 0;
};
Totals Snapshot() {
  Totals t;
  t.resets = g_resets.load();
  std::lock_guard<std::mutex> lock(g_regMutex);
  for (Counters* c : g_counters) {
    t.calls += c->calls;
    t.bytes += c->bytes;
    t.slow += c->slow;
    t.cycles += c->cycles;
  }
  return t;
}

// Page list under the manager's own lock.
void LogPages() {
  void* mgr = g_mgr.load();
  if (!mgr || !g_lock) return;
  uint8_t* m = static_cast<uint8_t*>(mgr);
  g_lock(m + kMgrMutex);
  uint8_t* b = *reinterpret_cast<uint8_t**>(m + 8);
  uint8_t* e = *reinterpret_cast<uint8_t**>(m + 0x10);
  size_t n = (b && e > b) ? static_cast<size_t>(e - b) / kPageStride : 0;
  uint64_t cap = 0, prev = 0;
  std::map<uint32_t, int> bySize;
  for (size_t i = 0; i < n; ++i) {
    const Page* p = reinterpret_cast<const Page*>(b + i * kPageStride);
    cap += p->capBytes;
    prev += p->prevUsed;
    bySize[p->elemSize]++;
  }
  g_unlock(m + kMgrMutex);
  std::string sizes;
  for (auto& kv : bySize) sizes += " " + std::to_string(kv.first) + "B x" + std::to_string(kv.second);
  Log("  pages %zu (%.1f MB capacity), last frame used %.2f MB; pages by element size:%s", n, cap / 1048576.0,
      prev / 1048576.0, sizes.c_str());
}

void Report(const Totals& a, const Totals& b, uint64_t frames, double tscHz) {
  if (!frames) return;
  double f = static_cast<double>(frames);
  uint64_t calls = b.calls - a.calls;
  Log("  allocate calls/frame %.0f, %.1f KB/frame, slow path %.1f%% of calls, resets/frame %.2f", calls / f,
      (b.bytes - a.bytes) / 1024.0 / f, calls ? 100.0 * (b.slow - a.slow) / calls : 0.0, (b.resets - a.resets) / f);
  Log("  time inside allocate: %.3f ms/frame summed over threads (%s)",
      (b.cycles - a.cycles) / tscHz * 1000.0 / f, g_on.load() ? "slabs ON" : "original allocator");
  LogPages();
}

}  // namespace allocslab

// NGModel's per-draw-item thunk (RVA 0x44350) adds every drawn item's
// triangle count to a global statistics counter with a locked add, about
// 34k times per frame on the render thread. The counter is only read for
// statistics (getNRenderedTris). Replacing the LOCK prefix (F0) with a DS
// segment prefix (3E, ignored in 64-bit mode) keeps the same instruction
// length and makes it a plain add. Writing one byte of an instruction prefix
// is atomic, so threads running the code see either form.
namespace tricount {

constexpr uint32_t kRva = 0x44371;
const uint8_t kLocked[8] = {0xF0, 0x48, 0x01, 0x82, 0xE8, 0x01, 0x00, 0x00};  // lock add [rdx+0x1e8], rax
uint8_t* g_site = nullptr;
std::atomic<bool> g_plain{false};

bool Prepare() {
  if (g_site) return true;
  auto* base = reinterpret_cast<uint8_t*>(GetModuleHandleW(L"NGModel.dll"));
  if (!base) return false;
  const uint32_t rva = reloc::Rva(hooksig::NG_SceneRenderable_render_triCounterAdd, kRva);
  uint8_t cur[8];
  if (!rva || !allocslab::ReadBytes(base + rva, cur, 8)) return false;  // not found: reloc.h logged it
  if (memcmp(cur + 1, kLocked + 1, 7) != 0 || (cur[0] != 0xF0 && cur[0] != 0x3E)) {
    Log("triangle counter: NGModel+0x%x does not match this build; skipped", rva);
    return false;
  }
  g_site = base + rva;
  g_plain = cur[0] == 0x3E;
  return true;
}

void SetPlain(bool plain) {
  if (!g_site || g_plain.load() == plain) return;
  DWORD old;
  if (!VirtualProtect(g_site, 1, PAGE_EXECUTE_READWRITE, &old)) return;
  *g_site = plain ? 0x3E : 0xF0;
  VirtualProtect(g_site, 1, old, &old);
  FlushInstructionCache(GetCurrentProcess(), g_site, 1);
  g_plain = plain;
}

}  // namespace tricount
