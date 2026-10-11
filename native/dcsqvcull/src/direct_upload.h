// Direct writes of model data into mapped GPU pages (R16, R15 A2).
// [Model] DirectUpload, default off.
//
// Stock: the culling tasks write every object's per-instance data (matrices,
// damage, light positions) into StructBufferManager's heap pages, and at
// EndParse the per-frame upload (slot 8, NGModel 0xbdb0) copies [0, used) of
// each page into its GPU structured buffer with Map(WRITE_DISCARD), memcpy,
// Unmap (DX11StructuredBuffer::update, dx11backend 0x32290) [V]. That copy is
// about 11 MB and 0.49 ms per frame on the render thread [M 2026-10-08].
//
// Direct: at the first collect call of the parse (render thread, before any
// culling task is pushed) every page that has a GPU buffer, capacity and use
// in the previous frame is mapped, and its `data` pointer is pointed at the
// mapping. The writers then store straight into the GPU allocation. At the
// upload, update() of a mapped page only unmaps (the bytes are already
// there); afterwards every page gets its heap pointer back, found by page
// index because the page vector can reallocate while culling creates pages.
//
// Why it is exact [V R16 §1, §5; gates G1-G3 measured 0 on 2026-10-08]:
//  - nothing on the CPU reads page memory: the 8 allocate call sites only
//    store through the pointer allocate returns, the other users of the
//    manager read records and gpuBuf only;
//  - every write finishes before the culling join, which precedes EndParse;
//  - no draw or dispatch is issued between the collect call and EndParse
//    (G1), so a mapped buffer is never bound to a draw;
//  - all allocations happen inside that window, from the render thread and
//    the culling workers (G2), and no page is in use when it opens (G3); pages
//    that are in use anyway get their bytes copied into the mapping first.
// Pages created during culling (no GPU buffer yet), pages without use in the
// previous frame and anything that fails a check take the stock memcpy path
// that frame. Any fault or failed Map undoes every mapping of that frame and
// runs stock. Allocator slabs cache `data`: they are invalidated after the
// pointers change both ways.
//
// [Suite] DirectUploadVerify fills the mappings with a sentinel and the heap
// free areas with another, logs every allocation, and at the upload checks
// that (a) every allocated block holds no mapping sentinel except in the
// HLSL padding of its class, (b) the heap free areas are untouched and (c) no
// allocation happens outside the window. Any mismatch latches it off.
//
// The vt[2] hook is par_upload's (one hook for both, R16 risk R6): direct
// upload is asked first through parupload::g_pre.
// Included once from main.cpp inside its anonymous namespace, after
// alloc_slab.h, par_upload.h and du_count.h.
#pragma once

namespace directupload {

constexpr int kUploadSlot = 8;
constexpr uint32_t kUploadRva = 0xbdb0;   // StructBufferManager slot 8 (per-frame upload)
constexpr uint32_t kUploadCallRva = 0xbeb5;
// mov r9d,[rbx+0x14] (used); mov r8,[rbx+0x18] (data); xor edx,edx; call [rax+0x10] (update) [V]
const uint8_t kUploadCall[13] = {0x44, 0x8b, 0x4b, 0x14, 0x4c, 0x8b, 0x43, 0x18, 0x33, 0xd2, 0xff, 0x50, 0x10};
const uint8_t kUploadPrologue[14] = {0x48, 0x89, 0x5c, 0x24, 0x10, 0x48, 0x89, 0x6c, 0x24, 0x18, 0x56, 0x57, 0x41, 0x56};
constexpr uint32_t kMapRva = 0x321e0;     // DX11StructuredBuffer vt[3]: Map, mode 1 = WRITE_DISCARD [V]
constexpr uint32_t kUnmapRva = 0x5270;    // vt[4]: Unmap on [[this+0x10]+0x30] [V]
constexpr uint32_t kUpdateRva = 0x32290;  // vt[2]
constexpr size_t kSbD3dBuffer = 0x20;     // ID3D11Buffer* [V dx11backend 0x3222d]
constexpr size_t kSbUsage = 0x44;         // 3 = dynamic [V 0x322a8]
constexpr int32_t kDynamic = 3;
constexpr uint32_t kSentMapped = 0x7FA5A5A5;  // NaN patterns: never a matrix or position value
constexpr uint32_t kSentHeap = 0x7FB4B4B4;

using UploadFn = void(__fastcall*)(void* mgr);
using MapFn = uint8_t*(__fastcall*)(void* self, int mode);
using UnmapFn = void(__fastcall*)(void* self);

std::atomic<bool> g_on{false};
std::atomic<bool> g_verify{false};
std::atomic<int> g_state{0};  // 0 = not installed (retried), 1 = ready, -1 = unavailable
std::atomic<bool> g_disabled{false};  // latched off for the session (fault, verify mismatch)
std::atomic<bool> g_shutdown{false};
std::atomic<int> g_open{0};  // saved mappings (nonzero between the collect and the upload)
std::atomic<int> g_busy{0};  // render thread inside OnCollect or the upload hook
std::atomic<DWORD> g_renderTid{0};
std::atomic<DWORD> g_uploadTid{0};  // thread inside the upload hook while pages are mapped (else 0)
void* g_sbVtbl = nullptr;
UploadFn g_origUpload = nullptr;
void** g_uploadSlot = nullptr;
std::atomic<uint8_t*> g_mgr{nullptr};

void Disable(const char* why) {
  if (!g_disabled.exchange(true)) Log("direct upload: disabled for this session (%s)", why);
}

// One mapped page (render thread only).
struct Entry {
  uint32_t idx, used0;  // page index; used when it was mapped
  uint8_t* heap;        // the page's own data pointer
  uint8_t* mapped;      // the mapping (page data while open)
  void* buf;            // DX11StructuredBuffer
  bool swapped;         // page data points at `mapped`
  bool done;            // unmapped
  uint32_t vOff, vUsed, cap, elem;  // verify only
};
constexpr int kMaxEntries = 64;
Entry g_e[kMaxEntries];
int g_n = 0;
std::atomic<DWORD> g_tidReset{0}, g_tidCollect{0}, g_tidUpload{0};
std::atomic<uint64_t> g_skClass{0};  // pages left to the memcpy: untraced element class
std::atomic<bool> g_parseOpen{false};  // a reset was seen, its first collect not yet
bool g_winVerify = false;

struct Counters {
  std::atomic<uint64_t> windows{0}, mapped{0}, copiedAtOpen{0}, bypassed{0}, uploads{0}, fallback{0},
      fallbackNoBuf{0}, idleUnmaps{0}, restores{0}, resetRestores{0}, mapFailures{0}, faults{0}, anomalies{0},
      skipped{0}, tooMany{0}, stockFrames{0};
};
Counters g_c;

// ---- verify state ----
struct Block {
  uint32_t page, off, bytes, tid;
};
constexpr uint32_t kMaxBlocks = 1u << 17;
Block* g_blocks = nullptr;  // allocated once by StartVerify, never freed
std::atomic<uint32_t> g_nBlocks{0};
// 0 = not counting, 1 = verified window open, 2 = after its upload, 3 = after the next reset
std::atomic<int> g_vPhase{0};
struct Offender {
  uint32_t check, page, off, elemOff, tid;
};
constexpr int kMaxOffenders = 8;
struct VerifyCounters {
  std::atomic<uint64_t> frames{0}, blocks{0}, bytes{0}, a{0}, aRange{0}, b{0}, c{0}, overflow{0}, unchecked{0};
  Offender off[kMaxOffenders] = {};
  std::atomic<int> nOff{0};
};
VerifyCounters g_v;
uint8_t* g_vBuf = nullptr;  // cached copy of the mappings (render thread)
size_t g_vCap = 0;

void NoteOffender(uint32_t check, uint32_t page, uint32_t off, uint32_t elemOff, uint32_t tid) {
  const int i = g_v.nOff.fetch_add(1);
  if (i < kMaxOffenders) g_v.off[i] = {check, page, off, elemOff, tid};
}

// ---- page and buffer helpers (callers guard with SEH) ----
allocslab::Page* FindPage(uint8_t* mgr, uint32_t idx) {
  uint8_t* b = *reinterpret_cast<uint8_t**>(mgr + 8);
  uint8_t* e = *reinterpret_cast<uint8_t**>(mgr + 0x10);
  if (!b || e <= b) return nullptr;
  const size_t n = static_cast<size_t>(e - b) / allocslab::kPageStride;
  if (idx < n) {
    auto* p = reinterpret_cast<allocslab::Page*>(b + idx * allocslab::kPageStride);
    if (p->idx == idx) return p;
  }
  for (size_t i = 0; i < n; ++i) {
    auto* p = reinterpret_cast<allocslab::Page*>(b + i * allocslab::kPageStride);
    if (p->idx == idx) return p;
  }
  return nullptr;
}

// A dynamic DX11StructuredBuffer whose D3D buffer is CPU-writable and holds
// the whole page.
bool BufOk(void* buf, uint32_t capBytes) {
  auto* b = static_cast<uint8_t*>(buf);
  if (*reinterpret_cast<void**>(b) != g_sbVtbl || *reinterpret_cast<int32_t*>(b + kSbUsage) != kDynamic) return false;
  auto* d3d = *reinterpret_cast<ID3D11Buffer**>(b + kSbD3dBuffer);
  if (!d3d) return false;
  D3D11_BUFFER_DESC desc = {};
  d3d->GetDesc(&desc);
  return desc.Usage == D3D11_USAGE_DYNAMIC && (desc.CPUAccessFlags & D3D11_CPU_ACCESS_WRITE) &&
         desc.ByteWidth >= capBytes;
}

uint8_t* CallMap(void* buf) { return reinterpret_cast<MapFn>((*static_cast<void***>(buf))[3])(buf, 1); }
void CallUnmap(void* buf) { reinterpret_cast<UnmapFn>((*static_cast<void***>(buf))[4])(buf); }

void Fill32(uint8_t* p, size_t bytes, uint32_t v) { __stosd(reinterpret_cast<unsigned long*>(p), v, bytes / 4); }

// Reads WC memory with streaming loads (verify only).
void StreamCopy(uint8_t* dst, const uint8_t* src, size_t n) {
  size_t i = 0;
  if ((reinterpret_cast<uintptr_t>(src) & 15) == 0 && (reinterpret_cast<uintptr_t>(dst) & 15) == 0) {
    for (; i + 64 <= n; i += 64) {
      __m128i a = _mm_stream_load_si128(reinterpret_cast<__m128i*>(const_cast<uint8_t*>(src + i)));
      __m128i b = _mm_stream_load_si128(reinterpret_cast<__m128i*>(const_cast<uint8_t*>(src + i + 16)));
      __m128i c = _mm_stream_load_si128(reinterpret_cast<__m128i*>(const_cast<uint8_t*>(src + i + 32)));
      __m128i d = _mm_stream_load_si128(reinterpret_cast<__m128i*>(const_cast<uint8_t*>(src + i + 48)));
      _mm_store_si128(reinterpret_cast<__m128i*>(dst + i), a);
      _mm_store_si128(reinterpret_cast<__m128i*>(dst + i + 16), b);
      _mm_store_si128(reinterpret_cast<__m128i*>(dst + i + 32), c);
      _mm_store_si128(reinterpret_cast<__m128i*>(dst + i + 48), d);
    }
  }
  if (i < n) memcpy(dst + i, src + i, n - i);
}

// ---- restore ----
// Unmaps every mapping not yet unmapped and gives every page its heap
// pointer back, found by index.
void RestoreBody(uint8_t* mgr, bool afterUpload) {
  for (int i = 0; i < g_n; ++i) {
    Entry& en = g_e[i];
    if (!en.done) {
      en.done = true;
      CallUnmap(en.buf);
      if (afterUpload) g_c.idleUnmaps++;  // used == 0 at the upload: nothing to send
    }
    if (!en.swapped) continue;
    allocslab::Page* pg = FindPage(mgr, en.idx);
    if (pg && pg->data == en.mapped) {
      pg->data = en.heap;
      g_c.restores++;
    } else {
      g_c.anomalies++;  // page gone or its pointer changed by someone else: leave it
    }
    en.swapped = false;
  }
}

void RestoreGuarded(uint8_t* mgr, bool afterUpload) {
  bool fault = false;
  __try {
    RestoreBody(mgr, afterUpload);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    fault = true;
  }
  if (fault) {
    g_c.faults++;
    Disable("fault while restoring the page pointers");
  }
  g_n = 0;
  g_open.store(0, std::memory_order_release);
}

void CloseLocked(uint8_t* mgr, bool afterUpload) {
  uint8_t* mutex = mgr + allocslab::kMgrMutex;
  allocslab::g_lock(mutex);
  __try {
    RestoreGuarded(mgr, afterUpload);
  } __finally {
    allocslab::g_unlock(mutex);
  }
  if (g_vPhase.load() == 1) g_vPhase = 2;
  allocslab::Invalidate();
}

// ---- open (first collect of the parse, render thread) ----
// False on a failed Map; faults propagate to OpenLocked.
bool OpenBody(uint8_t* mgr, bool verify) {
  uint8_t* b = *reinterpret_cast<uint8_t**>(mgr + 8);
  uint8_t* e = *reinterpret_cast<uint8_t**>(mgr + 0x10);
  for (uint8_t* p = b; p && p < e; p += allocslab::kPageStride) {
    auto* pg = reinterpret_cast<allocslab::Page*>(p);
    if (!pg->capBytes || !pg->gpuBuf || !pg->prevUsed || !pg->data) continue;
    // Only the element classes whose writers R16 traced (0x60, 0x70). The
    // 0xb0 class (AIRCRAFT_REGISTRATION with damage: tail number UVs at
    // 0x60-0x9f) leaves a tail-number dword unwritten [M 2026-10-09 verify:
    // element offset 0x6c in 221 of 20,802 blocks]; stock uploads stale heap
    // bytes there, which the GPU may read, so its pages stay on the memcpy.
    if (pg->elemSize != 0x60 && pg->elemSize != 0x70) {
      g_skClass++;
      continue;
    }
    if (pg->used > pg->capBytes || !BufOk(pg->gpuBuf, pg->capBytes)) {
      g_c.skipped++;
      continue;
    }
    if (g_n == kMaxEntries) {
      g_c.tooMany++;
      continue;
    }
    uint8_t* m = CallMap(pg->gpuBuf);
    if (!m) {
      g_c.mapFailures++;
      return false;
    }
    Entry& en = g_e[g_n++];
    en = Entry{};
    en.idx = pg->idx;
    en.used0 = pg->used;
    en.heap = pg->data;
    en.mapped = m;
    en.buf = pg->gpuBuf;
    if (verify) {
      Fill32(m, pg->capBytes, kSentMapped);
      const uint32_t from = (pg->used + 3) & ~3u;
      if (from < pg->capBytes) Fill32(pg->data + from, pg->capBytes - from, kSentHeap);
    }
    if (pg->used) {
      memcpy(m, pg->data, pg->used);  // R16 §2: a page already in use keeps its bytes
      g_c.copiedAtOpen++;
    }
    pg->data = m;
    en.swapped = true;
  }
  return true;
}

bool OpenLocked(uint8_t* mgr, bool verify) {
  bool ok = false, fault = false;
  uint8_t* mutex = mgr + allocslab::kMgrMutex;
  allocslab::g_lock(mutex);
  __try {
    __try {
      ok = OpenBody(mgr, verify);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
      ok = false;
      fault = true;
    }
    if (!ok) RestoreGuarded(mgr, false);  // undo everything: stock this frame
  } __finally {
    allocslab::g_unlock(mutex);
  }
  if (fault) {
    g_c.faults++;
    Disable("fault while mapping the model data pages");
  }
  return ok;
}

// Slot 7 (BeginParse, render thread), before the original reset.
void OnReset(void* mgr) {
  g_tidReset = GetCurrentThreadId();
  g_renderTid.store(GetCurrentThreadId(), std::memory_order_relaxed);
  g_mgr.store(static_cast<uint8_t*>(mgr), std::memory_order_relaxed);
  if (g_open.load(std::memory_order_acquire) != 0) {  // the upload never came: restore before reuse
    g_busy++;
    g_c.resetRestores++;
    CloseLocked(static_cast<uint8_t*>(mgr), false);
    g_busy--;
  }
  const int ph = g_vPhase.load();
  g_vPhase = (ph == 2 && g_verify.load()) ? 3 : 0;
  g_parseOpen = true;
}

// From the collect hook, after allocslab::OnCollect.
// Why collects did not open a window (diagnostics): no reset seen since the
// last window, another thread, off, no manager or mutex, a window still open.
std::atomic<uint64_t> g_skNoReset{0}, g_skThread{0}, g_skOff{0}, g_skMgr{0}, g_skOpen{0};

void OnCollect() {
  g_tidCollect = GetCurrentThreadId();
  if (!g_parseOpen) {
    g_skNoReset++;
    return;
  }
  // DCS runs the frame's render work as tasks: BeginParse, the collect and
  // EndParse are sequential but may run on different pool threads [M 2.9.30:
  // reset and upload on one thread, the collect on another]. The immediate
  // context is used strictly in sequence either way (G1: no draw from any
  // thread inside the window), so the thread is only counted.
  if (GetCurrentThreadId() != g_renderTid.load(std::memory_order_relaxed)) g_skThread++;
  g_parseOpen = false;
  uint8_t* mgr = g_mgr.load(std::memory_order_relaxed);
  if (!g_on.load(std::memory_order_relaxed) || g_disabled.load(std::memory_order_relaxed) ||
      g_shutdown.load(std::memory_order_relaxed)) g_skOff++;
  else if (!mgr || !allocslab::g_lock) g_skMgr++;
  else if (g_open.load() != 0) g_skOpen++;
  if (!g_on.load(std::memory_order_relaxed) || g_disabled.load(std::memory_order_relaxed) ||
      g_shutdown.load(std::memory_order_relaxed) || !mgr || !allocslab::g_lock || g_open.load() != 0) {
    if (g_vPhase.load() == 3) g_vPhase = 0;
    return;
  }
  g_busy++;
  const bool verify = g_verify.load() && g_blocks;
  g_winVerify = verify;
  if (verify) g_nBlocks.store(0, std::memory_order_relaxed);
  const bool ok = OpenLocked(mgr, verify);
  if (!ok) {
    g_c.stockFrames++;
    if (g_vPhase.load() == 3) g_vPhase = 0;
  } else if (g_n > 0) {
    g_c.windows++;
    g_c.mapped += g_n;
    g_open.store(g_n, std::memory_order_release);
    if (verify) g_vPhase.store(1, std::memory_order_release);
  }
  allocslab::Invalidate();
  g_busy--;
}

// ---- vt[2] (via parupload::Hook): true = finished here ----
bool __fastcall Bypass(void* self, uint32_t offset, const void* data, int32_t) {
  if (g_uploadTid.load(std::memory_order_acquire) != GetCurrentThreadId() || g_open.load() == 0) return false;
  for (int i = 0; i < g_n; ++i) {
    Entry& en = g_e[i];
    if (en.buf != self) continue;
    if (data == en.mapped && offset == 0) {
      if (!en.done) {
        en.done = true;
        CallUnmap(self);
        g_c.bypassed++;
      } else {
        g_c.anomalies++;  // second update of the same page: the bytes are already uploaded
      }
      return true;
    }
    // Not our pointer: finish our mapping, then the original maps and copies.
    if (!en.done) {
      en.done = true;
      CallUnmap(self);
    }
    g_c.anomalies++;
    return false;
  }
  return false;
}

// ---- verify (at the upload, before Unmap) ----
bool IsPadding(uint32_t elemSize, uint32_t elemOff) {
  // PositionStruct `dummy` of the 0x70 class: 0x6c-0x6f (dir variant) or
  // 0x64-0x6f (damage variants) are never written [V R16 §1, vt_utils.hlsl].
  return elemSize == 0x70 && elemOff >= 0x64;
}

void VerifyBody(uint8_t* mgr) {
  size_t total = 0;
  for (int i = 0; i < g_n; ++i) {
    allocslab::Page* pg = FindPage(mgr, g_e[i].idx);
    Entry& en = g_e[i];
    en.vUsed = pg && en.swapped ? pg->used : 0;
    en.cap = pg ? pg->capBytes : 0;
    en.elem = pg ? pg->elemSize : 0;
    if (en.vUsed > en.cap) en.vUsed = en.cap;
    en.vOff = static_cast<uint32_t>(total);
    total += (en.vUsed + 63) & ~63u;
  }
  if (total > g_vCap) {
    if (g_vBuf) VirtualFree(g_vBuf, 0, MEM_RELEASE);
    g_vCap = (total + (1u << 20)) & ~static_cast<size_t>((1u << 20) - 1);
    g_vBuf = static_cast<uint8_t*>(VirtualAlloc(nullptr, g_vCap, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (!g_vBuf) {
      g_vCap = 0;
      g_v.unchecked++;
      return;
    }
  }
  uint64_t bytes = 0;
  for (int i = 0; i < g_n; ++i) {
    Entry& en = g_e[i];
    if (!en.swapped) continue;
    StreamCopy(g_vBuf + en.vOff, en.mapped, en.vUsed);
    bytes += en.vUsed;
    // (b) heap free area untouched: nothing was written through a stale pointer.
    const uint32_t from = (en.used0 + 3) & ~3u;
    for (uint32_t o = from; o + 4 <= en.cap; o += 4) {
      if (*reinterpret_cast<const uint32_t*>(en.heap + o) != kSentHeap) {
        g_v.b++;
        NoteOffender('b', en.idx, o, en.elem ? o % en.elem : 0, 0);
        break;  // one per page
      }
    }
  }
  // (a) every allocated block of a mapped page reached the mapping.
  uint32_t n = g_nBlocks.load(std::memory_order_acquire);
  if (n > kMaxBlocks) {
    g_v.overflow += n - kMaxBlocks;
    n = kMaxBlocks;
  }
  for (uint32_t k = 0; k < n; ++k) {
    const Block& bl = g_blocks[k];
    const Entry* en = nullptr;
    for (int i = 0; i < g_n && !en; ++i)
      if (g_e[i].idx == bl.page && g_e[i].swapped) en = &g_e[i];
    if (!en) continue;  // a stock page this frame
    if (static_cast<uint64_t>(bl.off) + bl.bytes > en->vUsed || !en->elem) {
      g_v.aRange++;
      NoteOffender('r', bl.page, bl.off, 0, bl.tid);
      continue;
    }
    const uint8_t* v = g_vBuf + en->vOff;
    for (uint32_t o = bl.off & ~3u; o + 4 <= bl.off + bl.bytes; o += 4) {
      if (*reinterpret_cast<const uint32_t*>(v + o) != kSentMapped) continue;
      const uint32_t eo = o % en->elem;
      if (IsPadding(en->elem, eo)) continue;
      g_v.a++;
      NoteOffender('a', bl.page, o, eo, bl.tid);
      break;  // one per block
    }
  }
  g_v.frames++;
  g_v.blocks += n;
  g_v.bytes += bytes;
}

void VerifyGuarded(uint8_t* mgr) {
  bool fault = false;
  __try {
    VerifyBody(mgr);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    fault = true;
  }
  if (fault) {
    g_v.unchecked++;
    g_c.faults++;
  }
  if (g_v.a.load() || g_v.aRange.load() || g_v.b.load() || g_v.c.load()) Disable("verify mismatch");
}

// Allocation observer (verify phase): the block map, and (c).
void OnBlock(uint32_t size, uint32_t count, uint32_t page, uint32_t index) {
  const int ph = g_vPhase.load(std::memory_order_acquire);
  if (ph == 1) {
    const uint32_t i = g_nBlocks.fetch_add(1, std::memory_order_relaxed);
    if (i < kMaxBlocks) g_blocks[i] = {page, index * size, size * count, GetCurrentThreadId()};
  } else if (ph >= 2) {
    g_v.c++;
    NoteOffender('c', page, index * size, 0, GetCurrentThreadId());
  }
}

// ---- upload (slot 8, render thread) ----
// Pages the original will upload with its own memcpy this frame.
void CountUploadBody(uint8_t* mgr) {
  uint8_t* b = *reinterpret_cast<uint8_t**>(mgr + 8);
  uint8_t* e = *reinterpret_cast<uint8_t**>(mgr + 0x10);
  for (uint8_t* p = b; p && p < e; p += allocslab::kPageStride) {
    const auto* pg = reinterpret_cast<const allocslab::Page*>(p);
    if (!pg->used) continue;
    bool mine = false;
    for (int i = 0; i < g_n && !mine; ++i) mine = g_e[i].swapped && g_e[i].mapped == pg->data;
    if (mine) continue;
    g_c.fallback++;
    if (!pg->gpuBuf) g_c.fallbackNoBuf++;
  }
}

void CountUploadGuarded(uint8_t* mgr) {
  __try {
    CountUploadBody(mgr);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    g_c.faults++;
  }
}

void UploadAndClose(void* mgr) {
  g_uploadTid.store(GetCurrentThreadId(), std::memory_order_release);
  __try {
    g_origUpload(mgr);
  } __finally {
    g_uploadTid.store(0, std::memory_order_release);
    CloseLocked(static_cast<uint8_t*>(mgr), true);
  }
}

void __fastcall HookUpload(void* mgr) {
  g_busy++;
  g_tidUpload = GetCurrentThreadId();
  g_c.uploads++;
  CountUploadGuarded(static_cast<uint8_t*>(mgr));
  if (g_open.load(std::memory_order_acquire) == 0) {
    g_origUpload(mgr);
  } else {
    if (g_winVerify) VerifyGuarded(static_cast<uint8_t*>(mgr));
    UploadAndClose(mgr);
  }
  g_busy--;
}

// ---- install / control (worker or suite thread) ----
bool Install() {
  const int s = g_state.load();
  if (s != 0) return s > 0;
  auto* ng = reinterpret_cast<uint8_t*>(GetModuleHandleW(L"NGModel.dll"));
  auto* dx = reinterpret_cast<uint8_t*>(GetModuleHandleW(L"dx11backend.dll"));
  if (!ng || !dx) return false;  // retried
  // The allocate detour (and with it the reset hook) is written at a collect call.
  if (allocslab::g_state.load() < 0) {
    g_state = -1;
    Log("direct upload: model allocator hooks unavailable; skipped");
    return false;
  }
  if (allocslab::g_state.load() != 2 || !allocslab::g_lock) return false;  // retried
  auto** mvt = reinterpret_cast<void**>(ng + allocslab::kVtableRva);
  void** uslot = &mvt[kUploadSlot];
  if (SlotOriginal(uslot) == ng + kUploadRva && *uslot != ng + kUploadRva) return false;  // counters phase: retried
  g_state = -1;
  uint8_t pro[14], call[13];
  auto** sbvt = reinterpret_cast<void**>(dx + parupload::kVtableRva);
  if (SlotOriginal(uslot) != ng + kUploadRva || !allocslab::ReadBytes(ng + kUploadRva, pro, 14) ||
      memcmp(pro, kUploadPrologue, 14) != 0 || !allocslab::ReadBytes(ng + kUploadCallRva, call, 13) ||
      memcmp(call, kUploadCall, 13) != 0 || !allocslab::RttiIs(dx, sbvt, ".?AVDX11StructuredBuffer@RenderAPI@@") ||
      SlotOriginal(&sbvt[2]) != dx + kUpdateRva || SlotOriginal(&sbvt[3]) != dx + kMapRva ||
      SlotOriginal(&sbvt[4]) != dx + kUnmapRva) {
    Log("direct upload: NGModel.dll or dx11backend.dll does not match this build; skipped");
    return false;
  }
  if (!parupload::Install()) {
    Log("direct upload: DX11StructuredBuffer::update hook unavailable; skipped");
    return false;
  }
  g_sbVtbl = sbvt;
  g_origUpload = reinterpret_cast<UploadFn>(ng + kUploadRva);
  allocslab::g_duReset.store(&OnReset, std::memory_order_release);
  parupload::g_pre.store(&Bypass, std::memory_order_release);
  if (!HookSlot(uslot, reinterpret_cast<void*>(&HookUpload), nullptr)) {
    parupload::g_pre = nullptr;
    allocslab::g_duReset = nullptr;
    Log("direct upload: could not patch StructBufferManager slot 8");
    return false;
  }
  g_uploadSlot = uslot;
  g_state = 1;
  Log("direct upload: ready (StructBufferManager slot 8 and DX11StructuredBuffer::update hooked; %s)",
      g_on.load() ? "ON" : "OFF");
  return true;
}

// Payload unload: no new mappings, wait for an open window to close at its
// upload (render thread), then unhook. The destructor (0xbc00, free(data))
// must never see a mapping.
void Shutdown() {
  g_shutdown = true;
  g_on = false;
  g_verify = false;
  for (int i = 0; i < 400 && (g_open.load() != 0 || g_busy.load() != 0); ++i) Sleep(5);
  if (g_open.load() != 0) {
    // Only restore from here when none of our hooks is running: with one in
    // flight (the render thread in its upload/close, a culling thread
    // writing through a mapping) a restore here raced it (double Unmap).
    if (g_busy.load() != 0) {
      Log("direct upload: a mapped window and a hook call were still active at unload; left to its own close");
    } else if (uint8_t* mgr = g_mgr.load()) {
      Log("direct upload: a mapped window did not close within 2 s; restoring from the unload thread");
      CloseLocked(mgr, false);
    }
  }
  void (*obs)(uint32_t, uint32_t, uint32_t, uint32_t) = &OnBlock;
  allocslab::g_blockObs.compare_exchange_strong(obs, nullptr);
  void (*rst)(void*) = &OnReset;
  allocslab::g_duReset.compare_exchange_strong(rst, nullptr);
  parupload::UpdateFn pre = &Bypass;
  parupload::g_pre.compare_exchange_strong(pre, nullptr);
  if (g_uploadSlot) UnhookSlot(g_uploadSlot, reinterpret_cast<void*>(g_origUpload));  // g_origUpload stays for calls in flight
  g_uploadSlot = nullptr;
}

void ResetCounters() {
  g_c.~Counters();
  new (&g_c) Counters;
  g_skNoReset = g_skThread = g_skOff = g_skMgr = g_skOpen = g_skClass = 0;
}

void LogCounters(const char* label, double frames) {
  const Counters& c = g_c;
  auto u = [](const std::atomic<uint64_t>& a) { return static_cast<unsigned long long>(a.load()); };
  const double up = c.uploads.load() ? static_cast<double>(c.uploads.load()) : 1.0;
  const double w = c.windows.load() ? static_cast<double>(c.windows.load()) : 1.0;
  Log("  direct upload %s, collects without a window: no reset %llu, on another thread than the reset %llu (not skipped), off %llu, no manager %llu, "
      "window open %llu; untraced-class pages left to the memcpy %llu; threads: reset %lu, collect %lu, upload %lu", label, u(g_skNoReset), u(g_skThread), u(g_skOff),
      u(g_skMgr), u(g_skOpen), u(g_skClass), static_cast<unsigned long>(g_tidReset.load()),
      static_cast<unsigned long>(g_tidCollect.load()), static_cast<unsigned long>(g_tidUpload.load()));
  Log("  direct upload %s (%s%s): %.0f frames, %llu uploads, %llu mapped windows; pages mapped %.2f per window "
      "(%llu already in use at the collect), updates bypassed %.2f per window, mapped but unused %llu; memcpy "
      "fallbacks %.2f pages per upload (%.2f without a GPU buffer yet); restores %llu (%llu at a reset), stock "
      "frames after a failed map %llu (%llu map failures), pages skipped by the checks %llu, over the entry limit "
      "%llu, anomalies %llu, faults %llu",
      label, g_on.load() ? "ON" : "OFF", g_disabled.load() ? ", DISABLED" : "", frames, u(c.uploads), u(c.windows),
      c.mapped / w, u(c.copiedAtOpen), c.bypassed / w, u(c.idleUnmaps), c.fallback / up, c.fallbackNoBuf / up,
      u(c.restores), u(c.resetRestores), u(c.stockFrames), u(c.mapFailures), u(c.skipped), u(c.tooMany),
      u(c.anomalies), u(c.faults));
}

// Verify phase (suite thread). The block log is allocated once and kept.
bool StartVerify() {
  if (!g_blocks)
    g_blocks = static_cast<Block*>(
        VirtualAlloc(nullptr, sizeof(Block) * kMaxBlocks, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
  if (!g_blocks) return false;
  g_v.frames = g_v.blocks = g_v.bytes = g_v.a = g_v.aRange = g_v.b = g_v.c = g_v.overflow = g_v.unchecked = 0;
  g_v.nOff = 0;
  g_vPhase = 0;
  allocslab::g_blockObs.store(&OnBlock, std::memory_order_release);
  g_verify = true;
  return true;
}

void StopVerify() {
  g_verify = false;
  for (int i = 0; i < 200 && (g_open.load() != 0 || g_vPhase.load() == 1); ++i) Sleep(5);
  void (*obs)(uint32_t, uint32_t, uint32_t, uint32_t) = &OnBlock;
  allocslab::g_blockObs.compare_exchange_strong(obs, nullptr);
  g_vPhase = 0;
  Sleep(20);  // allocations already inside the observer finish
}

bool VerifyPassed() {
  return g_v.frames.load() > 0 && !g_v.a.load() && !g_v.aRange.load() && !g_v.b.load() && !g_v.c.load() &&
         !g_v.unchecked.load();
}

void LogVerify() {
  auto u = [](const std::atomic<uint64_t>& a) { return static_cast<unsigned long long>(a.load()); };
  Log("  direct upload verify: %llu frames checked, %llu blocks, %.2f MB per frame read back; (a) blocks with an "
      "unwritten non-padding dword %llu, blocks outside the used range %llu; (b) heap pages written after the swap "
      "%llu; (c) allocations outside the window %llu; block log overflow %llu, frames not checked %llu -> %s",
      u(g_v.frames), u(g_v.blocks), g_v.frames.load() ? g_v.bytes / 1048576.0 / g_v.frames.load() : 0.0, u(g_v.a),
      u(g_v.aRange), u(g_v.b), u(g_v.c), u(g_v.overflow), u(g_v.unchecked),
      g_v.frames.load() == 0 ? "INCOMPLETE (no mapped window)" : VerifyPassed() ? "PASS" : "FAIL");
  const int n = std::min(g_v.nOff.load(), kMaxOffenders);
  for (int i = 0; i < n; ++i) {
    const Offender& o = g_v.off[i];
    Log("    check %c: page %u, offset 0x%x (element offset 0x%x), thread %u", static_cast<char>(o.check), o.page,
        o.off, o.elemOff, o.tid);
  }
}

}  // namespace directupload
