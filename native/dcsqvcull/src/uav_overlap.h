// Culling-chain overlap upper bound ([Suite] UavOverlap, default 0; R26 §13
// lead A). MEASUREMENT ONLY. Armed for one 5 s GPU timing run at a time.
//
// Modes:
//   1 = NVIDIA's NvAPI_D3D11_BeginUAVOverlap / EndUAVOverlap around the passes
//       that hold DCS's terrain/forest culling chains (the four
//       CascadeShadowPass, HeightMapPass, the four LightsBuffersPass and the
//       four GBufferPass with their 5-dispatch instancer cull), each from its
//       begin to its end. The driver then skips the synchronisation between
//       dispatches that access the same UAVs, so DCS's dependent dispatches may
//       run concurrently and read stale data: OUTPUT MAY BE WRONG while ON;
//   2 = the same around every top-level pass (frame-wide);
//   3 = no NVAPI, output unchanged: every dispatch in the chain passes is
//       issued a second time right after itself, with each bound UAV replaced
//       by our own buffer of the same description (a renamed copy: same shader,
//       inputs, grid or indirect arguments, so the same work, but writing
//       elsewhere, so DCS's results are unchanged and the driver sees no hazard
//       between it and DCS's dispatch). It stands for a second camera's chain
//       interleaved step by step with this one, the schedule an exact
//       cross-camera overlap would need. The pass time it adds, against one
//       chain's own time, is what such an overlap could hide;
//   4 = the same with three copies (four cameras interleaved).
// Phase: GPU pass timing OFF / ON / OFF / ON / OFF (5 s each).
//
// NVAPI is reached through nvapi64.dll's nvapi_QueryInterface with the
// public interface ids (NVIDIA nvapi, nvapi_interface.h): NvAPI_Initialize
// 0x0150e828, NvAPI_D3D11_BeginUAVOverlap 0x65b93ca8, NvAPI_D3D11_EndUAVOverlap
// 0x2216a357; both take the device or immediate context (IUnknown*).
// The pass boundary comes from periph_lod.h's callback (g_probe); the
// duplicates from gpu_pass_timing.h's dispatch hooks (g_dup).
// Included once from main.cpp after periph_lod.h.
#pragma once

namespace uovl {

using QueryFn = void*(__cdecl*)(uint32_t);
using InitFn = int(__cdecl*)();
using OvFn = int(__cdecl*)(IUnknown*);
OvFn g_begin = nullptr, g_end = nullptr;
std::atomic<int> g_mode{0};
bool g_open = false;  // render thread
std::atomic<uint64_t> g_begins{0}, g_ends{0}, g_fails{0};
std::atomic<int> g_lastErr{0};
std::atomic<int> g_dup{0};  // modes 3/4: copies per dispatch
bool g_inChain = false;     // render thread: inside a culling-chain top-level pass
std::atomic<uint64_t> g_dupDone{0}, g_dupSkip{0}, g_shadowBytes{0};
thread_local bool t_inDup = false;

bool Load() {
  if (g_begin) return true;
  HMODULE m = LoadLibraryW(L"nvapi64.dll");
  if (!m) return false;
  auto q = reinterpret_cast<QueryFn>(GetProcAddress(m, "nvapi_QueryInterface"));
  if (!q) return false;
  auto init = reinterpret_cast<InitFn>(q(0x0150e828));
  if (!init) return false;
  const int st = init();
  if (st != 0) {
    Log("  uav overlap: NvAPI_Initialize returned %d", st);
    return false;
  }
  g_begin = reinterpret_cast<OvFn>(q(0x65b93ca8));
  g_end = reinterpret_cast<OvFn>(q(0x2216a357));
  if (!g_begin || !g_end) {
    // Both or neither: a driver without End left g_begin set, the next Load
    // returned true and OnBoundary called a null g_end.
    g_begin = g_end = nullptr;
    return false;
  }
  return true;
}

// ---------------------------------------------------------------------------
// Renamed duplicates (modes 3/4)
// ---------------------------------------------------------------------------

struct Shadow {
  ID3D11Buffer* buf[3] = {};
  ID3D11UnorderedAccessView* uav[3] = {};
};
// Keyed by DCS's resource and view description (render thread).
std::map<std::pair<ID3D11Resource*, std::string>, Shadow> g_shadow;

ID3D11UnorderedAccessView* ShadowOf(ID3D11UnorderedAccessView* v, int copy) {
  D3D11_UNORDERED_ACCESS_VIEW_DESC vd{};
  v->GetDesc(&vd);
  if (vd.ViewDimension != D3D11_UAV_DIMENSION_BUFFER) return nullptr;
  ID3D11Resource* r = nullptr;
  v->GetResource(&r);
  if (!r) return nullptr;
  r->Release();  // DCS keeps it alive while bound
  auto key = std::make_pair(r, std::string(reinterpret_cast<const char*>(&vd), sizeof(vd)));
  auto it = g_shadow.find(key);
  if (it == g_shadow.end()) {
    if (g_shadow.size() >= 512) return nullptr;
    it = g_shadow.emplace(key, Shadow{}).first;
  }
  Shadow& sh = it->second;
  if (!sh.uav[copy]) {
    D3D11_BUFFER_DESC bd{};
    static_cast<ID3D11Buffer*>(r)->GetDesc(&bd);
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.CPUAccessFlags = 0;
    if (FAILED(plod::g_dev->CreateBuffer(&bd, nullptr, &sh.buf[copy]))) return nullptr;
    g_shadowBytes += bd.ByteWidth;
    if (FAILED(plod::g_dev->CreateUnorderedAccessView(sh.buf[copy], &vd, &sh.uav[copy]))) {
      // Released: every later dispatch created (and leaked) a new buffer here.
      sh.buf[copy]->Release();
      sh.buf[copy] = nullptr;
      return nullptr;
    }
  }
  return sh.uav[copy];
}

// After DCS's dispatch (render thread, inside gpu_pass_timing.h's hook).
template <class F>
void Dup(ID3D11DeviceContext* c, F issue) {
  if (c != plod::g_ctx || !g_inChain || t_inDup) return;
  if (GetCurrentThreadId() != ptiming::g_topTid.load(std::memory_order_relaxed)) return;
  t_inDup = true;
  constexpr UINT kN = 8;
  ID3D11UnorderedAccessView* orig[kN] = {};
  c->CSGetUnorderedAccessViews(0, kN, orig);
  const int copies = std::min(3, g_dup.load(std::memory_order_relaxed));
  bool ok = true;
  for (int k = 0; k < copies && ok; ++k) {
    ID3D11UnorderedAccessView* sh[kN] = {};
    UINT counts[kN];
    for (UINT i = 0; i < kN; ++i) {
      counts[i] = 0;
      if (orig[i] && !(sh[i] = ShadowOf(orig[i], k))) ok = false;
    }
    if (!ok) break;
    c->CSSetUnorderedAccessViews(0, kN, sh, counts);
    issue();
    ++g_dupDone;
  }
  if (!ok) ++g_dupSkip;
  UINT keep[kN];
  for (UINT i = 0; i < kN; ++i) keep[i] = ~0u;
  c->CSSetUnorderedAccessViews(0, kN, orig, keep);  // DCS's views back, hidden counters kept
  for (UINT i = 0; i < kN; ++i)
    if (orig[i]) orig[i]->Release();
  t_inDup = false;
}
void PostDispatch(ID3D11DeviceContext* c, UINT x, UINT y, UINT z) {
  Dup(c, [&] { gpt::Orig<decltype(&gpt::H_Dispatch)>(gpt::kDispatch)(c, x, y, z); });
}
void PostDispatchIndirect(ID3D11DeviceContext* c, ID3D11Buffer* b, UINT o) {
  Dup(c, [&] { gpt::Orig<decltype(&gpt::H_DispatchIndirect)>(gpt::kDispatchIndirect)(c, b, o); });
}

void ReleaseShadows() {
  for (auto& kv : g_shadow) {
    for (int k = 0; k < 3; ++k) {
      if (kv.second.uav[k]) kv.second.uav[k]->Release();
      if (kv.second.buf[k]) kv.second.buf[k]->Release();
    }
  }
  g_shadow.clear();
  g_shadowBytes = 0;
}

// ---------------------------------------------------------------------------
// Boundary and phase
// ---------------------------------------------------------------------------

bool Wanted(void* pass, int mode) {
  if (mode == 2) return true;  // modes 1, 3, 4: the chain passes
  const int k = plod::KindOf(pass);
  return k == plod::kCascade || k == plod::kHeight || k == plod::kLights || k == plod::kGBuffer;
}

// periph_lod.h's boundary (render thread).
void OnBoundary(void* pass, bool begin, int depth) {
  if (depth != 0 || !plod::g_ctx) return;
  if (begin) {
    const int mode = g_mode.load(std::memory_order_relaxed);
    g_inChain = mode >= 3 && Wanted(pass, mode);
    if (!mode || mode >= 3 || g_open || !Wanted(pass, mode)) return;
    const int st = g_begin(plod::g_ctx);
    if (st == 0) {
      g_open = true;
      ++g_begins;
    } else {
      ++g_fails;
      g_lastErr = st;
    }
  } else {
    g_inChain = false;
    if (g_open) {
      g_end(plod::g_ctx);
      g_open = false;
      ++g_ends;
    }
  }
}

void RunPhase(int mode, std::atomic<uint64_t>& frameCounter) {
  if (mode <= 2 && !Load()) {
    Log("  uav overlap: NVAPI not available; phase skipped");
    return;
  }
  if (!plod::InstallCallback()) {
    Log("  uav overlap: pass boundary callback not available; phase skipped");
    return;
  }
  plod::g_probe = &OnBoundary;
  for (int i = 0; i < 5 && !g_benchAbort.load() && !g_stop.load(); ++i) {  // abort: payload stop / suite abort
    const bool on = (i & 1) != 0;
    g_mode = on ? mode : 0;
    g_dup = on && mode >= 3 ? (mode == 4 ? 3 : 1) : 0;
    Sleep(1000);
    Log("  uav overlap run %d: %s", i + 1,
        on ? (mode == 2   ? "ON (every top-level pass inside Begin/EndUAVOverlap)"
              : mode == 1 ? "ON (cascade, height map, LightsBuffers and G-buffer passes inside Begin/EndUAVOverlap)"
              : mode == 3 ? "ON (one renamed duplicate after every dispatch in the culling-chain passes)"
                          : "ON (three renamed duplicates after every dispatch in the culling-chain passes)")
           : "OFF");
    const uint64_t b0 = g_begins.load(), d0 = g_dupDone.load(), s0 = g_dupSkip.load();
    gpt::Measure(5000, frameCounter);
    if (on && mode >= 3)
      Log("  uav overlap: %llu duplicate dispatches in the run, %llu dispatches not duplicated (non-buffer UAV), "
          "%zu renamed views (%.1f MB)",
          static_cast<unsigned long long>(g_dupDone.load() - d0), static_cast<unsigned long long>(g_dupSkip.load() - s0),
          g_shadow.size(), g_shadowBytes.load() / 1048576.0);
    if (on && mode <= 2)
      Log("  uav overlap: %llu overlap regions in the run, %llu Begin failures (last status %d)",
          static_cast<unsigned long long>(g_begins.load() - b0), static_cast<unsigned long long>(g_fails.load()),
          g_lastErr.load());
  }
  g_mode = 0;
  g_dup = 0;
  Sleep(200);  // an open region ends at its pass end
  plod::g_probe = nullptr;
  ReleaseShadows();  // nothing duplicates any more (g_dup 0, gpt ended)
}

void Shutdown() {
  g_dup = 0;
  if (!g_mode.exchange(0)) return;
  Sleep(100);  // an open region ends at its pass end
  plod::g_probe = nullptr;
}

}  // namespace uovl
