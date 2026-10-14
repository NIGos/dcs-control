// Parallel copy for large dynamic structured-buffer uploads.
//
// DX11StructuredBuffer::update (dx11backend vtable RVA 0xb5980, slot 2,
// 0x32290) with a dynamic buffer ([this+0x44] == 3) does
//   p = this->vt[3](this, 1);  memcpy(p + offset, data, size);  this->vt[4](this);
// [V 0x322a8-0x322de], i.e. Map(WRITE_DISCARD), one copy, Unmap. NGModel's
// per-frame model data upload (StructBufferManager slot 8, 0xbdb0) sends
// several MB this way on the render thread (memcpy 1.3% of it, 2026-10-08).
// Writing a freshly renamed WRITE_DISCARD allocation is limited by first
// touch, and splitting the copy over 2 more threads halves it (offline on
// this PC: 7 MB in 0.68 ms with one thread, 0.36 ms with two).
//
// With it on, an update of a dynamic buffer of at least kMinBytes makes the
// same Map call, copies the same bytes split into chunks by the calling
// thread and the helper threads, waits for all chunks, then makes the same
// Unmap call. The buffer receives exactly the same bytes; only the copying
// threads differ. Smaller or non-dynamic updates call the original.
// Included once from main.cpp inside its anonymous namespace.
#pragma once

namespace parupload {

constexpr uint32_t kVtableRva = 0xb5980;  // .?AVDX11StructuredBuffer@RenderAPI@@
constexpr int kSlot = 2;
constexpr size_t kMinBytes = 512u << 10;
constexpr int kHelpers = 2;

using UpdateFn = bool(__fastcall*)(void* self, uint32_t offset, const void* data, int32_t size);
using MapFn = uint8_t*(__fastcall*)(void* self, int mode);
using UnmapFn = void(__fastcall*)(void* self);
UpdateFn g_orig = nullptr;
void** g_slot = nullptr;
std::atomic<bool> g_on{false};
std::atomic<int> g_state{0};
std::atomic<bool> g_stop{false};
HANDLE g_threads[kHelpers] = {};
HANDLE g_wake[kHelpers] = {};

// done: 1 = idle/finished, 0 = queued, 2 = a helper is copying, 3 = the
// caller took the chunk back (helper gone or too slow). Exactly one side
// claims a queued chunk (0 -> 2 or 0 -> 3), so the caller never Unmaps while
// a helper still writes into the mapping.
struct Job {
  std::atomic<uint8_t*> dst{nullptr};
  const uint8_t* src = nullptr;
  size_t len = 0;
  std::atomic<int> done{1};
};
std::atomic<int> g_inHook{0};      // calls inside Hook (Shutdown waits for 0 before closing the events)
std::atomic<bool> g_busy{false};   // one split update at a time (g_jobs is shared)
constexpr uint64_t kTakeBackCycles = 50'000'000;  // ~10-20 ms: a helper not started by then is not waited for
Job g_jobs[kHelpers];
std::atomic<uint64_t> g_calls{0}, g_bytes{0}, g_cycles{0};
// Direct upload (direct_upload.h) shares this slot (R16 risk R6): it is asked
// first and returns true when it finished the update itself (a page it
// mapped: Unmap only). Null unless direct upload is installed.
std::atomic<UpdateFn> g_pre{nullptr};

DWORD WINAPI Helper(void* arg) {
  const int i = static_cast<int>(reinterpret_cast<intptr_t>(arg));
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
  while (!g_stop.load()) {
    WaitForSingleObject(g_wake[i], 100);
    Job& j = g_jobs[i];
    int queued = 0;
    if (!j.done.compare_exchange_strong(queued, 2, std::memory_order_acq_rel)) continue;
    memcpy(j.dst.load(std::memory_order_relaxed), j.src, j.len);
    j.done.store(1, std::memory_order_release);
  }
  return 0;
}

bool SplitUpdate(void* self, uint32_t offset, const void* data, int32_t size);

// Shutdown waits on g_inHook before it stops the helpers and closes their
// events; a second thread updating at the same time takes the stock path
// (g_jobs is shared: a concurrent split could hand a helper a torn job).
bool __fastcall Hook(void* self, uint32_t offset, const void* data, int32_t size) {
  if (UpdateFn pre = g_pre.load(std::memory_order_acquire))
    if (pre(self, offset, data, size)) return true;
  auto* b = static_cast<uint8_t*>(self);
  if (!g_on.load(std::memory_order_relaxed) || size < static_cast<int32_t>(kMinBytes) ||
      *reinterpret_cast<int32_t*>(b + 0x44) != 3)
    return g_orig(self, offset, data, size);
  g_inHook.fetch_add(1);
  if (g_stop.load() || g_busy.exchange(true, std::memory_order_acquire)) {
    g_inHook.fetch_sub(1);
    return g_orig(self, offset, data, size);
  }
  const bool r = SplitUpdate(self, offset, data, size);
  g_busy.store(false, std::memory_order_release);
  g_inHook.fetch_sub(1);
  return r;
}

bool SplitUpdate(void* self, uint32_t offset, const void* data, int32_t size) {
  const uint64_t t0 = __rdtsc();
  void** vt = *static_cast<void***>(self);
  uint8_t* p = reinterpret_cast<MapFn>(vt[3])(self, 1);
  uint8_t* dst = p + offset;
  const auto* src = static_cast<const uint8_t*>(data);
  const size_t total = static_cast<size_t>(size);
  const size_t chunk = ((total / (kHelpers + 1)) + 63) & ~static_cast<size_t>(63);
  size_t pos = chunk;  // the caller copies [0, chunk)
  for (int i = 0; i < kHelpers; ++i) {
    Job& j = g_jobs[i];
    // The last helper takes the rest: chunk is rounded from total / 3, so
    // 3 * chunk can fall 1-2 bytes short of total (never copied before).
    const size_t len = pos >= total ? 0 : (pos + chunk > total || i == kHelpers - 1 ? total - pos : chunk);
    if (len == 0) continue;
    j.src = src + pos;
    j.len = len;
    j.dst.store(dst + pos, std::memory_order_relaxed);
    j.done.store(0, std::memory_order_release);
    SetEvent(g_wake[i]);
    pos += len;
  }
  memcpy(dst, src, chunk < total ? chunk : total);
  // Bounded: a chunk no helper has started (helpers stopping, or starved on a
  // busy CPU) is taken back and copied here. Before, this spun forever when
  // a payload stop ended the helpers between their wake and this wait.
  for (int i = 0; i < kHelpers; ++i) {
    Job& j = g_jobs[i];
    const uint64_t w0 = __rdtsc();
    for (;;) {
      const int s = j.done.load(std::memory_order_acquire);
      if (s == 1) break;
      if (s == 0 && (g_stop.load(std::memory_order_relaxed) || __rdtsc() - w0 > kTakeBackCycles)) {
        int queued = 0;
        if (j.done.compare_exchange_strong(queued, 3, std::memory_order_acq_rel)) {
          memcpy(j.dst.load(std::memory_order_relaxed), j.src, j.len);
          j.done.store(1, std::memory_order_release);
          break;
        }
      }
      _mm_pause();  // s == 2: a helper is copying (finishes in bounded time)
    }
  }
  reinterpret_cast<UnmapFn>(vt[4])(self);
  g_calls.fetch_add(1, std::memory_order_relaxed);
  g_bytes.fetch_add(total, std::memory_order_relaxed);
  g_cycles.fetch_add(__rdtsc() - t0, std::memory_order_relaxed);
  return true;
}

void StopHelpers();

bool StartHelpers() {
  g_stop = false;
  for (int i = 0; i < kHelpers; ++i) {
    g_jobs[i].done.store(1);
    g_wake[i] = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    g_threads[i] = CreateThread(nullptr, 0, qvexit::Thread<Helper>, reinterpret_cast<void*>(static_cast<intptr_t>(i)), 0, nullptr);
    if (!g_wake[i] || !g_threads[i]) return false;
  }
  return true;
}

bool Install() {
  // Claimed atomically (-2 while installing): the payload worker, the suite
  // and direct upload all call this, and two first calls used to start two
  // helper sets and hook the slot twice.
  int idle = 0;
  if (!g_state.compare_exchange_strong(idle, -2)) return g_state.load() > 0;
  auto* base = reinterpret_cast<uint8_t*>(GetModuleHandleW(L"dx11backend.dll"));
  if (!base) {
    g_state = 0;  // not loaded yet: retried
    return false;
  }
  g_state = -1;
  auto** vtbl = reinterpret_cast<void**>(base + kVtableRva);
  if (!allocslab::RttiIs(base, vtbl, ".?AVDX11StructuredBuffer@RenderAPI@@") ||
      SlotOriginal(&vtbl[kSlot]) != base + 0x32290) {
    Log("parallel upload: DX11StructuredBuffer does not match this build; skipped");
    return false;
  }
  // The dynamic path's exact shape: mode check, Map(1), memcpy, Unmap [V 0x322a8].
  const uint8_t kShape[] = {0x83, 0x79, 0x44, 0x03};
  uint8_t got[4];
  if (!allocslab::ReadBytes(base + 0x322a8, got, 4) || memcmp(got, kShape, 4) != 0) {
    Log("parallel upload: DX11StructuredBuffer::update does not match this build; skipped");
    return false;
  }
  if (!StartHelpers()) {
    StopHelpers();  // the helpers that did start
    return false;
  }
  g_slot = &vtbl[kSlot];
  g_orig = reinterpret_cast<UpdateFn>(SlotOriginal(g_slot));
  if (!HookSlot(g_slot, reinterpret_cast<void*>(&Hook), nullptr)) return false;
  g_state = 1;
  Log("parallel upload: DX11StructuredBuffer::update hooked (dynamic updates >= %zu KB split over %d threads)",
      kMinBytes >> 10, kHelpers + 1);
  return true;
}

void StopHelpers() {
  g_stop = true;
  for (int i = 0; i < kHelpers; ++i) {
    if (g_wake[i]) SetEvent(g_wake[i]);
    if (g_threads[i]) {
      WaitForSingleObject(g_threads[i], 1000);
      CloseHandle(g_threads[i]);
    }
    if (g_wake[i]) CloseHandle(g_wake[i]);
    g_threads[i] = g_wake[i] = nullptr;
  }
}

void Shutdown() {
  g_on = false;
  if (g_slot && g_orig) UnhookSlot(g_slot, reinterpret_cast<void*>(g_orig));
  // A call in flight still sets the helpers' events: wait for it (bounded;
  // it takes its chunks back once g_stop is set) before closing them.
  g_stop = true;
  for (int i = 0; i < 400 && g_inHook.load() != 0; ++i) Sleep(5);
  if (g_inHook.load() != 0) {
    Log("parallel upload: an update was still running at unload; helpers left running");
    return;
  }
  StopHelpers();
}

}  // namespace parupload
