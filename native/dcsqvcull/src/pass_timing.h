// Render-thread CPU time per rendering pass. Every pass of DCS's render graph
// runs through the virtual GraphicsCore!render::BaseRenderingPass::execute,
// referenced by GraphicsCore's pass vtables and imported by SceneRenderer.dll
// (whose pass vtables reach it through the import). Both are redirected; each
// pass object's exclusive time (nested passes subtracted) is attributed to the
// pass, named from its RTTI class.
// Included once from main.cpp after the globals it uses (Log, g_qpcToUs).
#pragma once

namespace ptiming {

const char kExecuteExport[] = "?execute@BaseRenderingPass@render@@UEAAX_K@Z";
using ExecFn = void(__fastcall*)(void* pass, uint64_t frame);
ExecFn g_orig = nullptr;
std::atomic<bool> g_recording{false};

struct Stat {
  uint64_t calls = 0;
  double exclUs = 0, inclUs = 0;
};
std::mutex g_mutex;
std::unordered_map<void*, Stat> g_stats;
std::atomic<uint64_t> g_frames{0};
double g_topLevelUs = 0;  // guarded by g_mutex

thread_local int t_depth = 0;
std::atomic<DWORD> g_topTid{0};  // thread running top-level passes (the render thread)
thread_local double t_childUs[64];
// Optional callback after each top-level pass, on the thread that ran it
// (shadow_rec.h: its texture snapshot between render entry and the cascades);
// nullptr when unused.
using AfterTopFn = void (*)();
std::atomic<AfterTopFn> g_afterTop{nullptr};
// Optional callback before and after every pass, nested ones too (shadow_rec.h:
// a texture snapshot as soon as a job's keys are final); nullptr when unused.
using EachFn = void (*)();
std::atomic<EachFn> g_eachPass{nullptr};
// Optional callback at the start and the end of every pass, nested ones too,
// with the pass object and its nesting depth (gpu_pass_timing.h: timestamps
// around each pass); nullptr when unused. Loaded once per execute, so a pass
// sees both calls or neither.
using BoundaryFn = void (*)(void* pass, bool begin, int depth);
std::atomic<BoundaryFn> g_boundary{nullptr};
// Optional second boundary callback (pass_flush.h: Flush at chosen pass
// boundaries), called before g_boundary at a start and after it at an end;
// nullptr when unused.
std::atomic<BoundaryFn> g_flushBoundary{nullptr};
// Optional third boundary callback (periph_lod.h: per-view LOD table swap and
// the uav_overlap.h probe), called after g_boundary at a start and before it at
// an end; nullptr unless [Model] PeripheralLodScale or a suite phase set it.
std::atomic<BoundaryFn> g_viewBoundary{nullptr};
// Optional fourth boundary callback (fixation_depth.h: the depth window after
// a view's G-buffer passes), called after every other one at a start and
// after every other one at an end; nullptr unless [Perception] FixationDepth.
std::atomic<BoundaryFn> g_depthBoundary{nullptr};
// The frame argument of the last top-level execute (diagnostics).
std::atomic<uint64_t> g_execFrame{0};

void __fastcall Hook(void* pass, uint64_t frame) {
  if (t_depth == 0) {
    qvalog::NoteFrameThread();  // R93: the render thread (main.cpp's async log: its lines never wait)
    g_topTid.store(GetCurrentThreadId(), std::memory_order_relaxed);
    g_execFrame.store(frame, std::memory_order_relaxed);
  }
  EachFn each = g_eachPass.load(std::memory_order_relaxed);
  BoundaryFn bnd = g_boundary.load(std::memory_order_relaxed);
  BoundaryFn fl = g_flushBoundary.load(std::memory_order_relaxed);
  BoundaryFn vb = g_viewBoundary.load(std::memory_order_relaxed);
  BoundaryFn db = g_depthBoundary.load(std::memory_order_relaxed);
  if (!g_recording.load(std::memory_order_relaxed)) {
    AfterTopFn after = g_afterTop.load(std::memory_order_relaxed);
    if (!after && !each && !bnd && !fl && !vb && !db) return g_orig(pass, frame);
    if (each) each();
    if (fl) fl(pass, true, t_depth);
    if (bnd) bnd(pass, true, t_depth);
    if (vb) vb(pass, true, t_depth);
    if (db) db(pass, true, t_depth);
    ++t_depth;
    g_orig(pass, frame);
    const bool top = --t_depth == 0;
    if (vb) vb(pass, false, t_depth);
    if (bnd) bnd(pass, false, t_depth);
    if (fl) fl(pass, false, t_depth);
    if (db) db(pass, false, t_depth);
    if (each) each();
    if (top && after) after();
    return;
  }
  if (each) each();
  if (fl) fl(pass, true, t_depth);
  if (bnd) bnd(pass, true, t_depth);
  if (vb) vb(pass, true, t_depth);
  if (db) db(pass, true, t_depth);
  int d = t_depth++;
  if (d < 64) t_childUs[d] = 0;
  LARGE_INTEGER a, b;
  QueryPerformanceCounter(&a);
  g_orig(pass, frame);
  QueryPerformanceCounter(&b);
  t_depth--;
  double us = (b.QuadPart - a.QuadPart) * g_qpcToUs;
  double excl = d < 64 ? us - t_childUs[d] : us;
  if (d > 0 && d - 1 < 64) t_childUs[d - 1] += us;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    Stat& s = g_stats[pass];
    s.calls++;
    s.inclUs += us;
    s.exclUs += excl;
    if (d == 0) g_topLevelUs += us;
  }
  if (vb) vb(pass, false, d);
  if (bnd) bnd(pass, false, d);
  if (fl) fl(pass, false, d);
  if (db) db(pass, false, d);
  if (each) each();
  if (d == 0)
    if (AfterTopFn after = g_afterTop.load(std::memory_order_relaxed)) after();
}

void RttiRaw(void* obj, char* buf, size_t size) {
  __try {
    void** vt = *reinterpret_cast<void***>(obj);
    auto col = reinterpret_cast<uint8_t*>(vt[-1]);
    if (*reinterpret_cast<uint32_t*>(col) == 1) {
      auto self = *reinterpret_cast<uint32_t*>(col + 0x14);
      auto base = col - self;
      auto td = base + *reinterpret_cast<uint32_t*>(col + 0xC);
      strncpy_s(buf, size, reinterpret_cast<const char*>(td + 0x10), _TRUNCATE);
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }
}

// Short readable name from the pass object's RTTI.
std::string RttiName(void* obj) {
  char buf[512] = {};
  RttiRaw(obj, buf, sizeof(buf));
  std::string s = buf;
  // Pick the most telling token: a "...PassData" struct, a "...Module", or the class.
  size_t p = s.find("PassData@");
  if (p != std::string::npos) {
    size_t b = s.rfind('U', p);
    size_t q = s.rfind("?$", p);
    size_t start = (b != std::string::npos && (q == std::string::npos || b > q)) ? b + 1 : 0;
    return s.substr(start, p + 8 - start) + (s.find("parseShadowMap") != std::string::npos ? " (shadow)" : "");
  }
  for (const char* key : {"Module@", "Pass@"}) {
    p = s.find(key);
    if (p != std::string::npos) {
      size_t b = s.rfind('@', p - 1);
      size_t start = b == std::string::npos ? 0 : b + 1;
      size_t a = s.rfind("V", p);
      if (a != std::string::npos && a + 1 > start) start = a + 1;
      return s.substr(start, p + strlen(key) - 1 - start);
    }
  }
  return s.size() > 60 ? s.substr(0, 60) : s;
}

// Patches every pointer to execute inside GraphicsCore's read-only data (its
// own pass vtables) and SceneRenderer's import of it.
void Install() {
  if (g_orig) return;  // once (diagnostics and the shadow recorder both ask)
  HMODULE gc = GetModuleHandleW(L"GraphicsCore.dll");
  if (!gc) return;
  void* exec = reinterpret_cast<void*>(GetProcAddress(gc, kExecuteExport));
  if (!exec) {
    Log("pass timing: execute export not found");
    return;
  }
  g_orig = reinterpret_cast<ExecFn>(exec);
  int n = 0;
  auto base = reinterpret_cast<uint8_t*>(gc);
  auto nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(base + reinterpret_cast<IMAGE_DOS_HEADER*>(base)->e_lfanew);
  auto sec = IMAGE_FIRST_SECTION(nt);
  for (int i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
    if (memcmp(sec->Name, ".rdata", 6) != 0) continue;
    auto p = reinterpret_cast<void**>(base + sec->VirtualAddress);
    size_t count = sec->Misc.VirtualSize / sizeof(void*);
    for (size_t k = 0; k < count; ++k)
      if (SlotOriginal(&p[k]) == exec && HookSlot(&p[k], reinterpret_cast<void*>(&Hook), nullptr)) ++n;
  }
  if (HMODULE sr = GetModuleHandleW(L"SceneRenderer.dll"))
    if (void** slot = timercache::FindImport(sr, "GraphicsCore.dll", kExecuteExport))
      if (HookSlot(slot, reinterpret_cast<void*>(&Hook), nullptr)) ++n;
  Log("pass timing: %d references to BaseRenderingPass::execute redirected", n);
}

// Clears the statistics (before g_recording is set).
void Reset() {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_stats.clear();
  g_topLevelUs = 0;
}

// Takes the statistics since Reset, pass objects with the same name merged.
std::map<std::string, Stat> TakeByName(double* topUs, size_t* objects) {
  std::unordered_map<void*, Stat> snap;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    snap.swap(g_stats);
    if (topUs) *topUs = g_topLevelUs;
  }
  if (objects) *objects = snap.size();
  std::map<std::string, Stat> byName;
  for (auto& kv : snap) {
    Stat& s = byName[RttiName(kv.first)];
    s.calls += kv.second.calls;
    s.exclUs += kv.second.exclUs;
    s.inclUs += kv.second.inclUs;
  }
  return byName;
}

void Measure(int ms, std::atomic<uint64_t>& frameCounter) {
  if (!g_orig) {
    Log("  pass timing not installed");
    return;
  }
  Reset();
  uint64_t f0 = frameCounter.load();
  g_recording = true;
  Sleep(ms);
  g_recording = false;
  Sleep(50);
  uint64_t frames = frameCounter.load() - f0;
  double top = 0;
  size_t objects = 0;
  std::map<std::string, Stat> byName = TakeByName(&top, &objects);
  if (!frames) return;
  double f = static_cast<double>(frames);
  std::vector<std::pair<std::string, Stat>> v(byName.begin(), byName.end());
  std::sort(v.begin(), v.end(), [](const auto& a, const auto& b) { return a.second.exclUs > b.second.exclUs; });
  Log("  render passes: %.2f ms/frame of pass execution (%zu pass objects, %zu kinds)", top / 1000.0 / f,
      objects, v.size());
  for (size_t i = 0; i < v.size() && i < 30; ++i)
    Log("    %-48.48s %6.3f ms excl  %6.3f ms incl  calls/frame %.1f", v[i].first.c_str(), v[i].second.exclUs / 1000.0 / f,
        v[i].second.inclUs / 1000.0 / f, v[i].second.calls / f);
}

}  // namespace ptiming
