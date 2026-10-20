// Cascade shadow pass instrumentation (GraphicsCore.dll). The execute callback
// of each cascade's rendering pass draws its caster list in order:
//   vtable 0x1800d9b68 slot 19 -> thunk 0x1800a6ec0 (rcx = pass, rdx = ctx)
//   -> 0x1800a5500 (pass+0x60 data, pass+0x48 resources, ctx)
// The caster list is ctx[0]+0x438 + word[[pass+0x48+0x10]+0x38] * 24, an
// ed::vector<ISceneRenderable*>. We measure time per cascade and how often
// consecutive casters switch model (renderable vtable + [r+0x10]), versus the
// minimum if they were grouped. Depth-only output does not depend on order.
// Included once from main.cpp after the globals it uses (Log, g_qpcToUs).
#pragma once

namespace shadowpass {

constexpr uint32_t kVtableRva = 0xd9b68;
constexpr int kExecSlot = 19;
constexpr uint32_t kThunkRva = 0xa6ec0;

using ExecFn = void(__fastcall*)(void* pass, void* ctx);
ExecFn g_orig = nullptr;
std::atomic<bool> g_recording{false};

struct Stat {
  uint64_t calls = 0, casters = 0, switches = 0, distinct = 0;
  double us = 0;
};
std::mutex g_mutex;
Stat g_stat;

// Returns the caster vector {begin, end, cap} or nullptr. SEH guarded.
void** CasterVector(void* pass, void* ctx) {
  __try {
    auto res = static_cast<uint8_t*>(pass) + 0x48;
    auto node = *reinterpret_cast<uint8_t**>(res + 0x10);
    if (!node) return nullptr;
    int16_t idx = *reinterpret_cast<int16_t*>(node + 0x38);
    if (idx == -1) return nullptr;
    auto ctx0 = *reinterpret_cast<uint8_t**>(ctx);
    auto base = *reinterpret_cast<uint8_t**>(ctx0 + 0x438);
    if (!base) return nullptr;
    return reinterpret_cast<void**>(base + idx * 24);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return nullptr;
  }
}

struct Key {
  void* vt;
  void* model;
  bool operator==(const Key& o) const { return vt == o.vt && model == o.model; }
};

bool ReadKey(void* r, Key* k) {
  __try {
    k->vt = *reinterpret_cast<void**>(r);
    k->model = *reinterpret_cast<void**>(static_cast<uint8_t*>(r) + 0x10);
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

void Analyse(void** vec, Stat& s) {
  auto b = static_cast<void**>(vec[0]);
  auto e = static_cast<void**>(vec[1]);
  if (!b || e < b || e - b > 200000) return;
  size_t n = static_cast<size_t>(e - b);
  s.casters += n;
  std::vector<Key> keys;
  keys.reserve(n);
  Key prev{}, k{};
  for (size_t i = 0; i < n; ++i) {
    if (!ReadKey(b[i], &k)) return;
    if (i == 0 || !(k == prev)) s.switches++;
    prev = k;
    keys.push_back(k);
  }
  std::sort(keys.begin(), keys.end(), [](const Key& x, const Key& y) {
    return x.vt != y.vt ? x.vt < y.vt : x.model < y.model;
  });
  s.distinct += static_cast<uint64_t>(std::unique(keys.begin(), keys.end()) - keys.begin());
}

// Optional wrapper around the cascade execute (shadow_batch.h): it calls
// `orig` itself (possibly twice in its verification mode); nullptr when unused.
using WrapFn = void (*)(void* pass, void* ctx, ExecFn orig);
std::atomic<WrapFn> g_wrap{nullptr};

// Optional innermost stand-in for DCS's execute (cascade_cache.h, R33): when
// set, every wrapper's `orig` is this function, which calls g_orig itself
// (so it sees the caster list the wrappers handed to DCS's loop). nullptr
// when unused: the chain is exactly as before.
std::atomic<ExecFn> g_inner{nullptr};

ExecFn Innermost() {
  const ExecFn i = g_inner.load(std::memory_order_relaxed);
  return i ? i : g_orig;
}

void CallOrig(void* pass, void* ctx) {
  const ExecFn o = Innermost();
  if (WrapFn w = g_wrap.load(std::memory_order_relaxed)) return w(pass, ctx, o);
  o(pass, ctx);
}

void __fastcall Hook(void* pass, void* ctx) {
  if (!g_recording.load(std::memory_order_relaxed)) return CallOrig(pass, ctx);
  Stat local;
  if (void** vec = CasterVector(pass, ctx)) Analyse(vec, local);
  LARGE_INTEGER a, b;
  QueryPerformanceCounter(&a);
  CallOrig(pass, ctx);
  QueryPerformanceCounter(&b);
  local.calls = 1;
  local.us = (b.QuadPart - a.QuadPart) * g_qpcToUs;
  std::lock_guard<std::mutex> lock(g_mutex);
  g_stat.calls += local.calls;
  g_stat.casters += local.casters;
  g_stat.switches += local.switches;
  g_stat.distinct += local.distinct;
  g_stat.us += local.us;
}

void Install() {
  if (g_orig) return;
  HMODULE gc = GetModuleHandleW(L"GraphicsCore.dll");
  if (!gc) return;
  auto base = reinterpret_cast<uint8_t*>(gc);
  // Recorded RVAs, or where reloc.h re-found them in another build (0: not found).
  const uint32_t vtRva = reloc::Rva(hooksig::GC_CascadeShadowPass_vtbl, kVtableRva);
  const uint32_t thunkRva = reloc::Rva(hooksig::GC_CascadeShadowPass_execute, kThunkRva);
  auto vtbl = reinterpret_cast<void**>(base + vtRva);
  if (!vtRva || !thunkRva || SlotOriginal(&vtbl[kExecSlot]) != base + thunkRva) {
    Log("shadow pass: vtable does not match this GraphicsCore.dll build; skipped");
    return;
  }
  if (!HookSlot(&vtbl[kExecSlot], reinterpret_cast<void*>(&Hook), reinterpret_cast<void**>(&g_orig))) return;
  Log("shadow pass: hooked cascade execute");
}

void Measure(int ms, std::atomic<uint64_t>& frameCounter) {
  if (!g_orig) {
    Log("  shadow pass not hooked");
    return;
  }
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_stat = Stat{};
  }
  uint64_t f0 = frameCounter.load();
  g_recording = true;
  Sleep(ms);
  g_recording = false;
  Sleep(50);
  uint64_t frames = frameCounter.load() - f0;
  Stat s;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    s = g_stat;
  }
  if (!frames || !s.calls) {
    Log("  no cascade passes recorded");
    return;
  }
  double f = static_cast<double>(frames);
  Log("  cascades/frame %.2f, shadow pass %.2f ms/frame, casters/frame %.0f", s.calls / f, s.us / 1000.0 / f,
      s.casters / f);
  Log("  model switches/frame %.0f vs minimum %.0f (distinct models per cascade, summed)", s.switches / f,
      s.distinct / f);
}

}  // namespace shadowpass
