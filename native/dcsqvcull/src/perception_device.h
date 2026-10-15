// [General] PerceptionOnly: DCS's D3D11 immediate context, for fixation depth,
// the far depth export and CockpitTrueIpd (fixation_depth.h EnsureDevice).
// With PerceptionOnly=0, shadow instancing (CEffect::GetDevice) or the split
// filter (its first draw) finds DCS's device; with PerceptionOnly=1 neither
// is installed, so this learns it instead, from one call:
//   DX11Renderer::draw (dx11backend, virtual, through the exported vtable) is
//   hooked once; its first call reads the renderer's immediate context
//   (DX11Renderer+0x30: draw tests [this+0x28], [this+0x30], [this+0x40] and
//   calls Draw (context slot 13) through [this+0x30]), checks it is a d3d11
//   immediate context of its own device, and restores the slot. The hook
//   only forwards; it never changes an argument.
// Included once from main.cpp (HookSlot, UnhookSlot, SlotOriginal, Log).
#pragma once

namespace pdev {

constexpr size_t kRendererContext = 0x30;
const char kRendererVtbl[] = "??_7DX11Renderer@RenderAPI@@6B@";
const char kRendererDraw[] = "?draw@DX11Renderer@RenderAPI@@UEAAXHPEAUIShader@2@W4PRIMTYPE_ENUM@render@@HHHPEBD@Z";
using DrawFn = void(__fastcall*)(void* self, int, void*, int, int, int, int, const char*);

// 0 = not tried, 1 = hooked (waiting for the first draw), 2 = context found
// (slot restored), -1 = not available (exports, slot or context not as expected).
std::atomic<int> g_state{0};
std::atomic<ID3D11DeviceContext*> g_ctx{nullptr};  // no reference: DCS's, alive with its device
DrawFn g_orig = nullptr;
void** g_slot = nullptr;
std::atomic<bool> g_taken{false};  // the first draw claimed the capture
std::atomic<int> g_busy{0};        // threads inside Hook (drained by Uninstall)
constexpr int kTries = 64;         // draws inspected before giving up
std::atomic<int> g_tries{0};

// True when p lies inside the image of the module named `name` (no call into p).
inline bool InModule(const void* p, const wchar_t* name) {
  HMODULE m = nullptr;
  if (!p || !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                static_cast<LPCWSTR>(p), &m))
    return false;
  return m == GetModuleHandleW(name);
}

void* ReadPtrSeh(const void* at) {
  __try {
    return *reinterpret_cast<void* const*>(at);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return nullptr;
  }
}

// The context at renderer+0x30 when it is d3d11's immediate context of its own
// device (QueryInterface / AddRef / Release / GetDevice inside d3d11.dll
// before anything is called on it).
ID3D11DeviceContext* CheckContextSeh(void* p) {
  __try {
    if (!p || (reinterpret_cast<uintptr_t>(p) & 7)) return nullptr;
    // d3d11 keeps each context's table in the context's own memory (it rewrites entries when it
    // toggles), so the table is not in d3d11.dll; its IUnknown and GetDevice entries are.
    void** vt = *static_cast<void***>(p);
    if (!vt || (reinterpret_cast<uintptr_t>(vt) & 7)) return nullptr;
    for (int i = 0; i < 4; ++i)
      if (!InModule(vt[i], L"d3d11.dll")) return nullptr;
    auto* c = static_cast<ID3D11DeviceContext*>(p);
    if (c->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE) return nullptr;
    ID3D11Device* dev = nullptr;
    c->GetDevice(&dev);
    if (!dev) return nullptr;
    ID3D11DeviceContext* imm = nullptr;
    dev->GetImmediateContext(&imm);
    dev->Release();
    if (imm) imm->Release();  // identity only
    return imm == c ? c : nullptr;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return nullptr;
  }
}

void RestoreSlot() {
  if (g_slot && g_orig) UnhookSlot(g_slot, reinterpret_cast<void*>(g_orig));
}

void __fastcall Hook(void* self, int a, void* b, int c, int d, int e, int f, const char* g) {
  g_busy.fetch_add(1, std::memory_order_acq_rel);
  if (!g_taken.load(std::memory_order_relaxed) && !g_taken.exchange(true)) {
    ID3D11DeviceContext* ctx = CheckContextSeh(ReadPtrSeh(static_cast<uint8_t*>(self) + kRendererContext));
    if (ctx) {
      g_ctx = ctx;
      g_state = 2;
      RestoreSlot();
      Log("perception device: DCS's D3D11 context %p (DX11Renderer %p); draw slot restored", static_cast<void*>(ctx),
          self);
    } else if (g_tries.fetch_add(1) + 1 >= kTries) {
      g_state = -1;
      RestoreSlot();
      Log("perception device: DX11Renderer+0x%zx is not d3d11's immediate context after %d draws; slot restored "
          "(fixation depth, far depth and the cockpit ipd wait for a device)",
          kRendererContext, kTries);
    } else {
      g_taken = false;  // the next draw looks again
    }
  }
  g_busy.fetch_sub(1, std::memory_order_acq_rel);
  g_orig(self, a, b, c, d, e, f, g);
}

// Worker thread, each second until it returns true (dx11backend may load later).
bool Install() {
  const int s = g_state.load();
  if (s != 0) return s > 0;
  HMODULE dx = GetModuleHandleW(L"dx11backend.dll");
  if (!dx) return false;  // retried
  auto** vtbl = reinterpret_cast<void**>(GetProcAddress(dx, kRendererVtbl));
  void* draw = reinterpret_cast<void*>(GetProcAddress(dx, kRendererDraw));
  if (!vtbl || !draw) {
    g_state = -1;
    Log("perception device: DX11Renderer exports not found; DCS's device not learnt");
    return false;
  }
  void** slot = nullptr;
  for (int i = 0; i < 256 && !slot; ++i)
    if (SlotOriginal(&vtbl[i]) == draw) slot = &vtbl[i];
  if (!slot) {
    g_state = -1;
    Log("perception device: draw not in the DX11Renderer vtable; DCS's device not learnt");
    return false;
  }
  g_orig = reinterpret_cast<DrawFn>(draw);
  g_slot = slot;
  g_taken = false;
  g_tries = 0;
  if (!HookSlot(slot, reinterpret_cast<void*>(&Hook), nullptr)) {
    g_state = -1;
    Log("perception device: could not hook DX11Renderer::draw");
    return false;
  }
  g_state = 1;
  Log("perception device: waiting for the first DX11Renderer::draw (vtable slot %d) to learn DCS's D3D11 context",
      static_cast<int>(slot - vtbl));
  return true;
}

// DCS's device with a reference (nullptr until learnt).
ID3D11Device* Device() {
  ID3D11DeviceContext* c = g_ctx.load();
  if (!c) return nullptr;
  ID3D11Device* d = nullptr;
  c->GetDevice(&d);
  return d;
}

// Payload stop: the slot back to DCS's if the first draw never came; the hook drained.
void Uninstall() {
  if (g_state.load() == 1) {
    g_taken = true;  // no capture from now on
    RestoreSlot();
    g_state = 0;
  }
  for (int i = 0; i < 200 && g_busy.load(); ++i) Sleep(5);
}

}  // namespace pdev
