// Per-model timing of NGModelLodsMT::ParseMT2 (NGModel.dll), the per-object
// work of the culling phase: LOD selection and renderable creation for every
// view an object is visible in. Finds the long pole the render thread waits
// for. The vtable is located by RVA for this NGModel.dll build and verified
// against the exported function before patching.
// Included once from main.cpp after the globals it uses (Log, g_qpcToUs).
#pragma once

namespace mtiming {

constexpr uint32_t kVtableRva = 0x5a008;  // ??_7NGModelLodsMT@model@@6B@ (RTTI)
constexpr int kParseSlot = 31;
const char kParseExport[] =
    "?ParseMT2@NGModelLodsMT@model@@UEAAXAEAVModelInstance@Graphics@@IPEAPEBUCollectionInfo@render@@"
    "QEB_NPEAPEAV?$vector@PEAUISceneRenderable@render@@V?$allocator@PEAUISceneRenderable@render@@@ed@@@ed@@"
    "AEAUILightProbeSampler@6@AEBV?$wPosition3@M@@@Z";

using ParseFn = void(__fastcall*)(void* self, void* inst, uint32_t count, void** infos, const bool* mask,
                                  void** outs, void* sampler, void* pos);
ParseFn g_orig = nullptr;
std::atomic<bool> g_recording{false};

// Renderables per kind of view: 0 periphery, 1 focus, 2 shadow cascades, 3 other passes.
constexpr int kKinds = 4;
struct Stat {
  uint64_t calls = 0, renderables = 0;
  uint64_t byKind[kKinds] = {};
  double totalUs = 0, maxUs = 0;
};

int KindOf(const uint8_t* info) {
  uint16_t sm = *reinterpret_cast<const uint16_t*>(info + layout::kCiShadingModel);
  uint32_t tag = *reinterpret_cast<const uint32_t*>(info + layout::kCiViewportTag);
  if (sm == 14) return 2;
  if (sm == 0 && tag <= 1) return 0;
  if (sm == 0 && (tag == 2 || tag == 3)) return 1;
  return 3;
}

size_t VecSize(void* vec) {
  auto v = static_cast<uint8_t**>(vec);
  return (v && v[0] && v[1] > v[0]) ? static_cast<size_t>(v[1] - v[0]) / sizeof(void*) : 0;
}
std::mutex g_mutex;
std::unordered_map<void*, Stat> g_stats;
// Busy time per thread per frame, to measure load balance of the parse tasks.
std::unordered_map<DWORD, double> g_threadUs;

// Per culling window (one collectSceneObjectsRenderables call): busy time of
// each thread inside ParseMT2. The longest-busy thread bounds the window.
std::unordered_map<DWORD, double> g_windowUs;  // guarded by g_mutex
// Per window, per thread: models parsed (to name the long pole).
std::unordered_map<DWORD, std::unordered_map<void*, double>> g_windowModels;  // guarded by g_mutex
std::unordered_map<void*, double> g_poleModels;  // time of each model on the busiest thread
uint64_t g_poleIsCaller = 0;                     // windows where the busiest thread ran the collect call
struct Balance {
  uint64_t windows = 0;
  double windowMs = 0, maxThreadMs = 0, totalMs = 0, threads = 0;
};
Balance g_balance;  // guarded by g_mutex

// Called by the collect hook after each culling window.
void EndWindow(double windowMs, DWORD callerTid) {
  if (!g_recording.load(std::memory_order_relaxed)) return;
  std::lock_guard<std::mutex> lock(g_mutex);
  double mx = 0, total = 0;
  DWORD busiest = 0;
  for (auto& kv : g_windowUs) {
    if (kv.second > mx) {
      mx = kv.second;
      busiest = kv.first;
    }
    total += kv.second;
  }
  if (busiest == callerTid) g_poleIsCaller++;
  for (auto& kv : g_windowModels[busiest]) g_poleModels[kv.first] += kv.second;
  g_windowModels.clear();
  g_balance.windows++;
  g_balance.windowMs += windowMs;
  g_balance.maxThreadMs += mx / 1000.0;
  g_balance.totalMs += total / 1000.0;
  g_balance.threads += static_cast<double>(g_windowUs.size());
  g_windowUs.clear();
}

constexpr uint32_t kMaxPer = 1024;  // views per call (was 64: an F-4 scene has 68, R24 12)

size_t SumOutputs(uint32_t count, void** outs) {
  size_t n = 0;
  if (!outs || count > kMaxPer) return 0;
  for (uint32_t i = 0; i < count; ++i) {
    auto v = static_cast<uint8_t**>(outs[i]);
    if (v && v[0] && v[1] > v[0]) n += static_cast<size_t>(v[1] - v[0]) / sizeof(void*);
  }
  return n;
}

void __fastcall Hook(void* self, void* inst, uint32_t count, void** infos, const bool* mask, void** outs,
                     void* sampler, void* pos) {
  if (!g_recording.load(std::memory_order_relaxed)) return g_orig(self, inst, count, infos, mask, outs, sampler, pos);
  size_t before = SumOutputs(count, outs);
  size_t per[kMaxPer];
  bool perOk = outs && infos && count <= kMaxPer;
  if (perOk)
    for (uint32_t i = 0; i < count; ++i) per[i] = VecSize(outs[i]);
  LARGE_INTEGER a, b;
  QueryPerformanceCounter(&a);
  g_orig(self, inst, count, infos, mask, outs, sampler, pos);
  QueryPerformanceCounter(&b);
  size_t after = SumOutputs(count, outs);
  double us = (b.QuadPart - a.QuadPart) * g_qpcToUs;
  std::lock_guard<std::mutex> lock(g_mutex);
  g_threadUs[GetCurrentThreadId()] += us;
  g_windowUs[GetCurrentThreadId()] += us;
  g_windowModels[GetCurrentThreadId()][self] += us;
  Stat& s = g_stats[self];
  s.calls++;
  s.totalUs += us;
  if (us > s.maxUs) s.maxUs = us;
  if (after > before) s.renderables += after - before;
  if (perOk)
    for (uint32_t i = 0; i < count; ++i) {
      size_t now = VecSize(outs[i]);
      if (now > per[i] && infos[i]) s.byKind[KindOf(static_cast<const uint8_t*>(infos[i]))] += now - per[i];
    }
}

void Install() {
  HMODULE ng = GetModuleHandleW(L"NGModel.dll");
  if (!ng) return;
  auto vtbl = reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(ng) + ngb::At(reinterpret_cast<uint8_t*>(ng), kVtableRva));
  void* parse = reinterpret_cast<void*>(GetProcAddress(ng, kParseExport));
  if (!parse || SlotOriginal(&vtbl[kParseSlot]) != parse) {
    Log("model timing: NGModelLodsMT vtable does not match this NGModel.dll build; skipped");
    return;
  }
  if (!HookSlot(&vtbl[kParseSlot], reinterpret_cast<void*>(&Hook), reinterpret_cast<void**>(&g_orig))) return;
  Log("model timing: hooked NGModelLodsMT::ParseMT2");
}

// Best-effort model name: first readable string in the object that looks
// like a model/asset name.
void GuessNameInto(void* obj, char* buf) {
  __try {
    auto p = static_cast<uint8_t*>(obj);
    for (int off = 0; off < 0x300; off += 8) {
      // Inline (SSO) string.
      const char* s = reinterpret_cast<const char*>(p + off);
      int n = 0;
      while (n < 64 && s[n] >= 0x20 && s[n] < 0x7f) ++n;
      if (n >= 5 && s[n] == 0) {
        memcpy(buf, s, n);
        break;
      }
      // Heap string pointer.
      auto q = *reinterpret_cast<const char**>(p + off);
      if (reinterpret_cast<uintptr_t>(q) > 0x10000 && reinterpret_cast<uintptr_t>(q) < 0x7fffffffffff) {
        n = 0;
        while (n < 80 && q[n] >= 0x20 && q[n] < 0x7f) ++n;
        if (n >= 5 && q[n] == 0) {
          memcpy(buf, q, n);
          break;
        }
      }
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }
}

std::string GuessName(void* obj) {
  char buf[96] = {};
  GuessNameInto(obj, buf);
  return buf;
}

void Measure(int ms, std::atomic<uint64_t>& frameCounter) {
  if (!g_orig) {
    Log("  model timing not installed");
    return;
  }
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_stats.clear();
    g_threadUs.clear();
    g_windowUs.clear();
    g_windowModels.clear();
    g_poleModels.clear();
    g_poleIsCaller = 0;
    g_balance = Balance{};
  }
  uint64_t f0 = frameCounter.load();
  g_recording = true;
  Sleep(ms);
  g_recording = false;
  Sleep(50);  // let in-flight calls finish
  uint64_t frames = frameCounter.load() - f0;
  std::unordered_map<void*, Stat> snap;
  std::unordered_map<DWORD, double> threads;
  Balance bal;
  std::unordered_map<void*, double> pole;
  uint64_t poleIsCaller;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    snap.swap(g_stats);
    threads.swap(g_threadUs);
    bal = g_balance;
    pole.swap(g_poleModels);
    poleIsCaller = g_poleIsCaller;
  }
  if (bal.windows) {
    double w = static_cast<double>(bal.windows);
    Log("  culling window %.2f ms: parse work %.2f ms over %.1f threads, busiest thread %.2f ms "
        "(ideal with 6 threads %.2f ms)",
        bal.windowMs / w, bal.totalMs / w, bal.threads / w, bal.maxThreadMs / w, bal.totalMs / w / 6.0);
    Log("  busiest thread is the one running the collect call in %.0f%% of windows", 100.0 * poleIsCaller / w);
    std::vector<std::pair<double, void*>> pv;
    for (auto& kv : pole) pv.emplace_back(kv.second, kv.first);
    std::sort(pv.rbegin(), pv.rend());
    Log("  models on the busiest thread (ms per window):");
    for (size_t i = 0; i < pv.size() && i < 8; ++i)
      Log("    %-28.28s %.3f ms", GuessName(pv[i].second).c_str(), pv[i].first / 1000.0 / w);
  }
  if (!frames) return;
  double f = static_cast<double>(frames);
  std::vector<std::pair<void*, Stat>> v(snap.begin(), snap.end());
  double total = 0;
  uint64_t calls = 0, rend = 0;
  for (auto& kv : v) {
    total += kv.second.totalUs;
    calls += kv.second.calls;
    rend += kv.second.renderables;
  }
  std::sort(v.begin(), v.end(), [](const auto& a, const auto& b) { return a.second.totalUs > b.second.totalUs; });
  Log("  ParseMT2: %.2f ms CPU/frame, %.0f models/frame, %.0f renderables/frame (%zu distinct models)",
      total / 1000.0 / f, calls / f, rend / f, v.size());
  {
    std::vector<double> t;
    for (auto& kv : threads) t.push_back(kv.second / 1000.0 / f);
    std::sort(t.rbegin(), t.rend());
    char line[512];
    int n = snprintf(line, sizeof(line), "  parse work per thread (ms/frame):");
    for (double x : t) n += snprintf(line + n, sizeof(line) - n, " %.2f", x);
    Log("%s", line);
  }
  Log("  most expensive models (per frame: ms, renderables; max single call):");
  for (size_t i = 0; i < v.size() && i < 12; ++i) {
    const Stat& s = v[i].second;
    std::string name = GuessName(v[i].first);
    Log("    %p %-24.24s %6.3f ms  %6.0f rend (periph %5.0f focus %5.0f shadow %5.0f other %5.0f)  max %.3f ms  "
        "calls/frame %.2f",
        v[i].first, name.c_str(), s.totalUs / 1000.0 / f, s.renderables / f, s.byKind[0] / f, s.byKind[1] / f,
        s.byKind[2] / f, s.byKind[3] / f, s.maxUs / 1000.0, s.calls / f);
  }
}

}  // namespace mtiming
