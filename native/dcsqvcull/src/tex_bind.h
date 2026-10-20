// Texture streaming requests. dx11backend's DX11Texture vtable slot 23 (RVA
// 0x49e90) runs on every texture bind (about 190k per frame at the test
// airfield) and records streaming usage: a "used" flag, the mip size the
// screen size needs with a timestamp, and a poll of the streaming loader.
// About 90% of the calls repeat the same (texture, size) within the same
// millisecond.
//
// With dedupe on, a repeat of the same (texture, size) within the same 1 ms
// tick is skipped: the first call of the tick already set the same flag and
// stored the same {level, time} (the streaming clock is the 1 ms timer cache),
// and the loader poll runs again on the first bind of the next tick. Callers
// ignore the return value [V: dx11backend 0x1fdba, 0x1ff4a, 0x1f996]. The
// size is passed by value. Without dedupe the hook only counts.
// Included once from main.cpp inside its anonymous namespace, after g_quadFrame.
#pragma once

namespace texbind {

constexpr uint32_t kVtableRva = 0xb7140;  // .?AVDX11Texture@RenderAPI@@
constexpr int kSlot = 23;
using Fn = uint64_t(__fastcall*)(void* tex, uint64_t packedSize);
Fn g_orig = nullptr;
std::atomic<bool>& g_dedupe = g_texDedupeOn;
void** g_slot = nullptr;
double g_tscPerMs = 0;
std::atomic<bool> g_attached{false};

struct Entry {
  void* tex;
  uint64_t size;
  uint64_t ms;
  uint64_t frame;
};
struct Counters {
  uint64_t calls = 0, sameMs = 0, sameFrame = 0, cycles = 0, skipped = 0;
};
std::mutex g_regMutex;
std::vector<Counters*> g_counters;

// Timing of the hook itself is only taken while the suite measures it.
std::atomic<bool>& g_measure = g_hookMeasure;

// The 1 ms tick: the timer cache's value, refreshed every millisecond by its
// helper thread (a plain load). Without the timer cache, the TSC.
inline uint64_t Tick(uint64_t tsc) {
  if (timercache::g_orig) {
    double v = timercache::g_cached.load(std::memory_order_relaxed);
    uint64_t bits;
    memcpy(&bits, &v, sizeof(bits));
    return bits;
  }
  return g_tscPerMs > 0 ? static_cast<uint64_t>(tsc / g_tscPerMs) : 0;
}

// Full path with counters, used while the suite measures.
uint64_t HookMeasured(void* tex, uint64_t packedSize) {
  // The table on the heap, per thread at its first measured call: a
  // thread_local array (128 KB) went into the payload's static TLS, which
  // Windows allocates and copies for every DCS thread, measured or not.
  thread_local Entry* table = nullptr;
  thread_local Counters* c = nullptr;
  if (!table) {
    table = static_cast<Entry*>(calloc(4096, sizeof(Entry)));
    if (!table) return g_orig(tex, packedSize);
  }
  if (!c) {
    c = new Counters;
    std::lock_guard<std::mutex> lock(g_regMutex);
    g_counters.push_back(c);
  }
  uint64_t t0 = __rdtsc();
  uint64_t ms = Tick(t0);
  uint64_t frame = g_quadFrame.load(std::memory_order_relaxed);
  uint64_t h = (reinterpret_cast<uint64_t>(tex) >> 4) * 0x9E3779B97F4A7C15ull ^ packedSize * 0xC2B2AE3D27D4EB4Full;
  Entry& e = table[(h >> 52) & 4095];
  c->calls++;
  if (e.tex == tex && e.size == packedSize) {
    if (e.ms == ms) {
      c->sameMs++;
      if (g_dedupe.load(std::memory_order_relaxed)) {
        c->skipped++;
        e.frame = frame;
        c->cycles += __rdtsc() - t0;
        return 0;
      }
    }
    if (e.frame == frame) c->sameFrame++;
  }
  e = {tex, packedSize, ms, frame};
  uint64_t r = g_orig(tex, packedSize);
  c->cycles += __rdtsc() - t0;
  return r;
}

// Hot path: one table probe. Only the render thread binds textures; a
// concurrent caller could at worst skip or repeat one streaming request
// within the same millisecond, which is harmless.
// 16-byte entries (64 KB table instead of 96 KB): the size is stored as two
// 16-bit halves when both dimensions fit (sizes that do not fit are never
// cached, so never skipped), and the tick is the timer cache's refresh count,
// which changes exactly when the cached streaming time can change. Without the
// timer cache the tick is the TSC in milliseconds.
struct FastEntry {
  void* tex;
  uint32_t size;
  uint32_t tick;
};
static_assert(sizeof(FastEntry) == 16, "FastEntry layout");
FastEntry g_fast[4096];

inline uint32_t FastTick() {
  if (timercache::g_on.load(std::memory_order_relaxed))
    return static_cast<uint32_t>(timercache::g_refreshes.load(std::memory_order_relaxed));
  return g_tscPerMs > 0 ? static_cast<uint32_t>(__rdtsc() / g_tscPerMs) : 0;
}

// Requests that reached dx11backend through the fast path (not skipped as a
// repeat): a plain add on the render thread, read as differences by the
// suite's YawProfile / RotationProfile (view_profile.h).
uint64_t g_passed = 0;

uint64_t __fastcall Hook(void* tex, uint64_t packedSize) {
  if (g_measure.load(std::memory_order_relaxed)) return HookMeasured(tex, packedSize);
  const int32_t w = static_cast<int32_t>(packedSize);
  const int32_t h = static_cast<int32_t>(packedSize >> 32);
  if (static_cast<int16_t>(w) != w || static_cast<int16_t>(h) != h) {
    ++g_passed;
    return g_orig(tex, packedSize);
  }
  const uint32_t size = static_cast<uint16_t>(w) | static_cast<uint32_t>(static_cast<uint16_t>(h)) << 16;
  const uint32_t tick = FastTick();
  FastEntry& e = g_fast[((reinterpret_cast<uint64_t>(tex) >> 4) ^ (size * 0x9E3779B1u)) & 4095];
  if (e.tex == tex && e.size == size && e.tick == tick && g_dedupe.load(std::memory_order_relaxed)) return 0;
  e.tex = tex;
  e.size = size;
  e.tick = tick;
  ++g_passed;
  return g_orig(tex, packedSize);
}

// Previous fast path (24-byte entries keyed by the cached time's bits), kept
// for the A/B against the 16-byte table (bench mode 20).
struct FastEntryV1 {
  void* tex;
  uint64_t size;
  uint64_t ms;
};
FastEntryV1 g_fastV1[4096];

uint64_t __fastcall HookV1(void* tex, uint64_t packedSize) {
  if (g_measure.load(std::memory_order_relaxed)) return HookMeasured(tex, packedSize);
  const uint64_t ms = timercache::g_orig ? Tick(0) : Tick(__rdtsc());
  FastEntryV1& e = g_fastV1[((reinterpret_cast<uint64_t>(tex) >> 4) ^ (packedSize * 0x9E3779B1u)) & 4095];
  if (e.tex == tex && e.size == packedSize && e.ms == ms && g_dedupe.load(std::memory_order_relaxed)) return 0;
  e.tex = tex;
  e.size = packedSize;
  e.ms = ms;
  return g_orig(tex, packedSize);
}

// Bench mode 20: true = 16-byte table (shipped), false = previous table.
void UseCompactTable(bool on) {
  if (!g_orig || !g_attached.load()) return;
  HookSlot(g_slot, reinterpret_cast<void*>(on ? &Hook : &HookV1), nullptr);
}

bool Install(double tscHz) {
  if (g_orig) return true;
  auto* base = reinterpret_cast<uint8_t*>(GetModuleHandleW(L"dx11backend.dll"));
  if (!base) return false;
  const uint32_t vtRva = reloc::Rva(hooksig::DX_DX11Texture_vtbl, kVtableRva);  // 0: not in this build
  auto** vtbl = reinterpret_cast<void**>(base + vtRva);
  const uint32_t fnRva = reloc::Rva(hooksig::DX_DX11Texture_requestMip, 0x49e90);
  if (!vtRva || !fnRva || !allocslab::RttiIs(base, vtbl, ".?AVDX11Texture@RenderAPI@@") ||
      SlotOriginal(&vtbl[kSlot]) != base + fnRva) {
    Log("texture binds: DX11Texture vtable does not match this dx11backend.dll build; skipped");
    return false;
  }
  g_tscPerMs = tscHz / 1000.0;
  g_slot = &vtbl[kSlot];
  g_orig = reinterpret_cast<Fn>(SlotOriginal(g_slot));
  if (!HookSlot(g_slot, reinterpret_cast<void*>(&Hook), nullptr)) {
    g_orig = nullptr;
    return false;
  }
  g_attached = true;
  Log("texture binds: counting DX11Texture streaming requests (slot 23)");
  return true;
}

// Puts dx11backend's own function back (no hook cost at all), or the hook again.
void SetAttached(bool on) {
  if (!g_orig || on == g_attached.load()) return;
  if (on) {
    HookSlot(g_slot, reinterpret_cast<void*>(&Hook), nullptr);
  } else {
    UnhookSlot(g_slot, reinterpret_cast<void*>(g_orig));
  }
  g_attached = on;
}

struct Totals {
  uint64_t calls = 0, sameMs = 0, sameFrame = 0, cycles = 0, skipped = 0;
};
Totals Snapshot() {
  Totals t;
  std::lock_guard<std::mutex> lock(g_regMutex);
  for (Counters* c : g_counters) {
    t.calls += c->calls;
    t.sameMs += c->sameMs;
    t.sameFrame += c->sameFrame;
    t.cycles += c->cycles;
    t.skipped += c->skipped;
  }
  return t;
}

void Report(const Totals& a, const Totals& b, uint64_t frames, double tscHz) {
  if (!frames || !g_orig) {
    Log("  texture bind counters not installed");
    return;
  }
  double f = static_cast<double>(frames);
  uint64_t calls = b.calls - a.calls;
  Log("  streaming requests/frame %.0f, repeated within 1 ms %.1f%%, repeated within the frame %.1f%%, "
      "time in slot 23 %.3f ms/frame (includes the timing itself), skipped %.1f%% (dedupe %s)",
      calls / f, calls ? 100.0 * (b.sameMs - a.sameMs) / calls : 0.0,
      calls ? 100.0 * (b.sameFrame - a.sameFrame) / calls : 0.0, (b.cycles - a.cycles) / tscHz * 1000.0 / f,
      calls ? 100.0 * (b.skipped - a.skipped) / calls : 0.0, g_dedupe.load() ? "on" : "off");
}

}  // namespace texbind

// FX11 (DCS's D3DX11Effects fork) SConstantBuffer::SetConstantBuffer, vtable
// slot 31 (RVA 0x67970). NGModel sets the material constant buffer on every
// draw; each call runs CEffect::ReplaceCBReference (0x61020) over the
// effect's dependency slots, even when the same buffer is already set.
// When the buffer is already overridden (flag 4 at +0x5C) and the override
// (+0x10) is the same buffer, the original would replace that buffer with
// itself and return S_OK; the skip returns S_OK directly. [V 0x67970-0x679ea]
namespace cbskip {

constexpr uint32_t kVtableRva = 0xb9400;  // .?AUSConstantBuffer@D3DX11Effects@@
constexpr int kSlot = 31;
using Fn = long(__fastcall*)(uint8_t* cb, void* buffer);
Fn g_orig = nullptr;
void** g_slot = nullptr;
std::atomic<bool> g_attached{false};
std::atomic<uint64_t> g_calls{0}, g_hits{0};

long __fastcall Hook(uint8_t* cb, void* buffer) {
  const bool same = (cb[0x5c] & 4) != 0 && *reinterpret_cast<void**>(cb + 0x10) == buffer && (cb[0x5c] & 2) == 0;
  if (g_hookMeasure.load(std::memory_order_relaxed)) {
    g_calls.fetch_add(1, std::memory_order_relaxed);
    if (same) g_hits.fetch_add(1, std::memory_order_relaxed);
  }
  if (same) return 0;
  return g_orig(cb, buffer);
}

bool Install() {
  if (g_orig) return true;
  auto* base = reinterpret_cast<uint8_t*>(GetModuleHandleW(L"dx11backend.dll"));
  if (!base) return false;
  const uint32_t vtRva = reloc::Rva(hooksig::DX_SConstantBuffer_vtbl, kVtableRva);  // 0: not in this build
  auto** vtbl = reinterpret_cast<void**>(base + vtRva);
  const uint32_t fnRva = reloc::Rva(hooksig::DX_SConstantBuffer_SetConstantBuffer, 0x67970);
  if (!vtRva || !fnRva || !allocslab::RttiIs(base, vtbl, ".?AUSConstantBuffer@D3DX11Effects@@") ||
      SlotOriginal(&vtbl[kSlot]) != base + fnRva) {
    Log("constant buffer skip: FX vtable does not match this dx11backend.dll build; skipped");
    return false;
  }
  g_slot = &vtbl[kSlot];
  g_orig = reinterpret_cast<Fn>(SlotOriginal(g_slot));
  return true;
}

void SetAttached(bool on) {
  if (!g_orig || on == g_attached.load()) return;
  if (on) {
    HookSlot(g_slot, reinterpret_cast<void*>(&Hook), nullptr);
  } else {
    UnhookSlot(g_slot, reinterpret_cast<void*>(g_orig));
  }
  g_attached = on;
}

}  // namespace cbskip

// dx11backend DX11ConstantBuffer::update (vtable RVA 0xb1358, slot 2 = 0xb970):
// update(this, data, size) -> bool. NGModel uploads one material constant
// buffer (304 bytes) per draw with Map(WRITE_DISCARD) or UpdateSubresource.
// The GPU buffer (+0x28) belongs to this object, so when the bytes equal the
// last upload of the same object into the same buffer, the buffer already
// holds them and the upload can be skipped. A per-object shadow copy keeps
// the last bytes. [V 0xb970-0xbab7]
namespace cbupload {

constexpr uint32_t kVtableRva = 0xb1358;
constexpr int kSlot = 2;
constexpr uint32_t kMax = 512;
using Fn = bool(__fastcall*)(uint8_t* cb, const void* data, int size);
Fn g_orig = nullptr;
void** g_slot = nullptr;
std::atomic<bool> g_attached{false};
std::atomic<uint64_t> g_calls{0}, g_same{0};

struct Entry {
  uint8_t* cb;
  void* gpu;
  int size;
  uint8_t bytes[kMax];
};
constexpr size_t kTable = 8192;
Entry* g_table = nullptr;  // render thread only

// Measurement only: the per-view context buffer (GraphicsCore
// ContextBase::updateBuffers, 0x970 bytes, R9 C2) is larger than kMax, so it
// is counted separately against its own shadow copies.
constexpr int kViewSize = 0x970;
std::atomic<bool> g_viewMeasure{false};
uint64_t g_viewCalls = 0, g_viewSame = 0;  // render thread only
struct ViewEntry {
  uint8_t* cb = nullptr;
  uint8_t bytes[kViewSize];
};
ViewEntry g_view[8];

void NoteView(uint8_t* cb, const void* data) {
  ++g_viewCalls;
  for (ViewEntry& v : g_view) {
    if (v.cb == cb) {
      if (memcmp(v.bytes, data, kViewSize) == 0) ++g_viewSame;
      memcpy(v.bytes, data, kViewSize);
      return;
    }
  }
  for (ViewEntry& v : g_view) {
    if (!v.cb) {
      v.cb = cb;
      memcpy(v.bytes, data, kViewSize);
      return;
    }
  }
}

bool __fastcall Hook(uint8_t* cb, const void* data, int size) {
  if (size == kViewSize && g_viewMeasure.load(std::memory_order_relaxed)) NoteView(cb, data);
  void* gpu = *reinterpret_cast<void**>(cb + 0x28);
  const int mode = *reinterpret_cast<int*>(cb + 0x20);
  if (size <= 0 || size > static_cast<int>(kMax) || !gpu || (mode != 2 && mode != 3) || !g_table)
    return g_orig(cb, data, size);
  Entry& e = g_table[(reinterpret_cast<uintptr_t>(cb) >> 4) * 0x9E3779B97F4A7C15ull >> 51];
  const bool same = e.cb == cb && e.gpu == gpu && e.size == size && memcmp(e.bytes, data, size) == 0;
  if (g_hookMeasure.load(std::memory_order_relaxed)) {
    g_calls.fetch_add(1, std::memory_order_relaxed);
    if (same) g_same.fetch_add(1, std::memory_order_relaxed);
  }
  if (same && g_cbUploadSkipOn.load(std::memory_order_relaxed)) return true;
  bool r = g_orig(cb, data, size);
  if (r) {
    e.cb = cb;
    e.gpu = gpu;
    e.size = size;
    memcpy(e.bytes, data, size);
  } else {
    e.cb = nullptr;
  }
  return r;
}

bool Install() {
  if (g_orig) return true;
  auto* base = reinterpret_cast<uint8_t*>(GetModuleHandleW(L"dx11backend.dll"));
  if (!base) return false;
  auto** vtbl = reinterpret_cast<void**>(base + kVtableRva);
  if (!allocslab::RttiIs(base, vtbl, ".?AVDX11ConstantBuffer@RenderAPI@@") ||
      SlotOriginal(&vtbl[kSlot]) != base + 0xb970) {
    Log("constant buffer upload: vtable does not match this dx11backend.dll build; skipped");
    return false;
  }
  g_table = static_cast<Entry*>(calloc(kTable, sizeof(Entry)));
  if (!g_table) {  // checked: the hook and SetAttached(false) use it unconditionally
    Log("constant buffer upload: no memory for the table; skipped");
    return false;
  }
  g_slot = &vtbl[kSlot];
  g_orig = reinterpret_cast<Fn>(SlotOriginal(g_slot));
  return true;
}

void SetAttached(bool on) {
  if (!g_orig || on == g_attached.load()) return;
  if (on) {
    HookSlot(g_slot, reinterpret_cast<void*>(&Hook), nullptr);
  } else {
    UnhookSlot(g_slot, reinterpret_cast<void*>(g_orig));
    // Forget the shadow copies: uploads made while detached are not seen.
    memset(g_table, 0, kTable * sizeof(Entry));
  }
  g_attached = on;
}

}  // namespace cbupload
