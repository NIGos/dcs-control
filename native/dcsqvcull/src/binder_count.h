// MetaShader binder repeat counter (measurement only). metaShader.dll's
// MetaShaderImpl2 (vtable RVA 0xd9cc8) slot 15 = render(item, context, pass)
// and slot 14 = setupParams(item, context) walk the shader's binder map and
// call every binder's slot 4 with (shader, binder data, item->subItems[i],
// context) [V 0x71fa0, 0x723a0]. When a call has the same shader, item
// sub-item array and context as the call just before it, every binder sees
// the same inputs again: the count is an upper bound on what a binder memo
// could skip (R8 C2).
// Included once from main.cpp inside its anonymous namespace.
#pragma once

namespace bindcount {

constexpr uint32_t kVtableRva = 0xd9cc8;
using RenderFn = uint64_t(__fastcall*)(void* self, void* item, void* ctx, uint64_t pass);
using SetupFn = uint64_t(__fastcall*)(void* self, void* item, void* ctx, uint64_t a4);
RenderFn g_origRender = nullptr;
SetupFn g_origSetup = nullptr;
std::atomic<bool> g_on{false};

struct Key {
  void* shader = nullptr;
  void* subItems = nullptr;
  void* firstSub = nullptr;
  void* ctx = nullptr;
};
struct Counts {
  uint64_t calls = 0, sameAll = 0, sameShader = 0;
};
Counts g_render, g_setup;
Key g_last;  // previous call of either slot (render thread only)
DWORD g_thread = 0;

Key MakeKey(void* self, void* item, void* ctx) {
  Key k{self, nullptr, nullptr, ctx};
  if (item) {
    k.subItems = *reinterpret_cast<void**>(static_cast<uint8_t*>(item) + 0x18);
    if (k.subItems) k.firstSub = *static_cast<void**>(k.subItems);
  }
  return k;
}

void Note(Counts& c, const Key& k) {
  if (GetCurrentThreadId() != g_thread) {
    if (g_thread) return;
    g_thread = GetCurrentThreadId();
  }
  c.calls++;
  if (k.shader == g_last.shader) {
    c.sameShader++;
    if (k.subItems == g_last.subItems && k.firstSub == g_last.firstSub && k.ctx == g_last.ctx) c.sameAll++;
  }
  g_last = k;
}

uint64_t __fastcall HookRender(void* self, void* item, void* ctx, uint64_t pass) {
  if (g_on.load(std::memory_order_relaxed)) Note(g_render, MakeKey(self, item, ctx));
  return g_origRender(self, item, ctx, pass);
}

uint64_t __fastcall HookSetup(void* self, void* item, void* ctx, uint64_t a4) {
  if (g_on.load(std::memory_order_relaxed)) Note(g_setup, MakeKey(self, item, ctx));
  return g_origSetup(self, item, ctx, a4);
}

bool Install() {
  if (g_origRender) return true;
  auto* base = reinterpret_cast<uint8_t*>(GetModuleHandleW(L"metaShader.dll"));
  if (!base) return false;
  auto** vtbl = reinterpret_cast<void**>(base + kVtableRva);
  if (!allocslab::RttiIs(base, vtbl, ".?AVMetaShaderImpl2@render@@")) {
    Log("binder counters: vtable does not match this build; skipped");
    return false;
  }
  g_origRender = reinterpret_cast<RenderFn>(SlotOriginal(&vtbl[15]));
  g_origSetup = reinterpret_cast<SetupFn>(SlotOriginal(&vtbl[14]));
  return HookSlot(&vtbl[15], reinterpret_cast<void*>(&HookRender), nullptr) &&
         HookSlot(&vtbl[14], reinterpret_cast<void*>(&HookSetup), nullptr);
}

void Measure(int ms, std::atomic<uint64_t>& frames) {
  if (!Install()) return;
  g_thread = 0;
  g_render = {};
  g_setup = {};
  g_last = {};
  uint64_t f0 = frames.load();
  g_on = true;
  Sleep(ms);
  g_on = false;
  Sleep(50);
  double f = static_cast<double>(frames.load() - f0);
  if (f <= 0) return;
  auto line = [&](const char* name, const Counts& c) {
    Log("  metashader %s: %.0f calls/frame, same shader as previous call %.1f%%, same shader+item+context %.1f%%",
        name, c.calls / f, c.calls ? 100.0 * c.sameShader / c.calls : 0.0,
        c.calls ? 100.0 * c.sameAll / c.calls : 0.0);
  };
  line("render", g_render);
  line("setupParams", g_setup);
}

}  // namespace bindcount

// InstanceManager2 vtable (edterrainGraphics41 RVA 0x14f4f8) slot 19 =
// per-view instance visibility marking in the terrain parse (R8 C1, 0x9fbf0,
// four register arguments, no stack arguments [V]). Time per frame and the
// thread it runs on: a rewrite can only gain while that thread is on the
// critical path.
namespace imcount {

constexpr uint32_t kVtableRva = 0x14f4f8;
using Fn = uint64_t(__fastcall*)(void*, void*, void*, void*);
Fn g_orig = nullptr;
std::atomic<bool> g_on{false};
std::atomic<uint64_t> g_calls{0}, g_cycles{0}, g_renderThreadCalls{0};
DWORD g_renderThread = 0;

uint64_t __fastcall Hook(void* a, void* b, void* c, void* d) {
  if (!g_on.load(std::memory_order_relaxed)) return g_orig(a, b, c, d);
  uint64_t t0 = __rdtsc();
  uint64_t r = g_orig(a, b, c, d);
  g_cycles += __rdtsc() - t0;
  g_calls++;
  if (GetCurrentThreadId() == g_renderThread) g_renderThreadCalls++;
  return r;
}

bool Install() {
  if (g_orig) return true;
  auto* base = reinterpret_cast<uint8_t*>(GetModuleHandleW(L"edterrainGraphics41.dll"));
  if (!base) return false;
  auto** vtbl = reinterpret_cast<void**>(base + kVtableRva);
  if (!allocslab::RttiIs(base, vtbl, ".?AVInstanceManager2@edtg41@@") ||
      reinterpret_cast<uint8_t*>(SlotOriginal(&vtbl[19])) != base + 0x9fbf0) {
    Log("instance-manager counter: vtable does not match this build; skipped");
    return false;
  }
  g_orig = reinterpret_cast<Fn>(SlotOriginal(&vtbl[19]));
  return HookSlot(&vtbl[19], reinterpret_cast<void*>(&Hook), nullptr);
}

void Measure(int ms, std::atomic<uint64_t>& frames, double tscHz, DWORD renderThread) {
  if (!Install()) return;
  g_renderThread = renderThread;
  g_calls = 0;
  g_cycles = 0;
  g_renderThreadCalls = 0;
  uint64_t f0 = frames.load();
  g_on = true;
  Sleep(ms);
  g_on = false;
  Sleep(50);
  double f = static_cast<double>(frames.load() - f0);
  if (f <= 0) return;
  Log("  instance visibility marking (slot 19): %.2f calls/frame (%.0f%% on the render thread), %.3f ms/frame",
      g_calls / f, g_calls ? 100.0 * g_renderThreadCalls / g_calls : 0.0, g_cycles / tscHz * 1000.0 / f);
}

}  // namespace imcount

// Shadow casters through NGModel material slot 5 (R10 C1): GlassMaterialMT
// (vtable 0x590d8) and ModelMaterialMT (vtable 0x592f0). Slot 5 is reached by
// a tail jump from ShadowMapRenderable vt[1](renderable, ctx), so only the
// four register arguments exist. Glass casters whose properties byte +0x33 is
// set use lockon_shadows_transparent, whose glass pixel shader always
// discards: those draws write nothing.
namespace shadowcount {

using Fn = uint64_t(__fastcall*)(void*, void*, void*, void*);
Fn g_origGlass = nullptr, g_origModel = nullptr;
std::atomic<bool> g_on{false};
std::atomic<uint64_t> g_glass{0}, g_glassTransparent{0}, g_model{0}, g_glassCycles{0};
std::atomic<uint64_t> g_modelCycles{0}, g_texCalls{0}, g_texCycles{0}, g_texCallsAll{0}, g_texCyclesAll{0};
std::atomic<uint64_t> g_texNullAll{0};
thread_local bool t_inShadow = false;
// The shadow texture skip (shadow_tex.h) also owns model slot 5. Once this
// counter has patched the slot, it forwards to that hook instead of the
// original, so both can be active.
std::atomic<Fn> g_modelNext{nullptr};
inline Fn ModelTarget() {
  Fn n = g_modelNext.load(std::memory_order_relaxed);
  return n ? n : g_origModel;
}

// DX11Shader slot 26 (dx11backend vtable 0xb43b8, 0x1fd70) = set a material
// texture: (shader, handle, texture, aux, Vec2i) [V R10]. Timed inside model
// slot 5 (shadow casters) and overall, to bound R10 C2.
using TexFn = uint64_t(__fastcall*)(void*, void*, void*, void*, uint64_t, uint64_t);
TexFn g_origTex = nullptr;
uint64_t __fastcall HookTex(void* a, void* b, void* c, void* d, uint64_t e, uint64_t f) {
  if (!g_on.load(std::memory_order_relaxed)) return g_origTex(a, b, c, d, e, f);
  uint64_t t0 = __rdtsc();
  uint64_t r = g_origTex(a, b, c, d, e, f);
  uint64_t dt = __rdtsc() - t0;
  g_texCallsAll++;
  g_texCyclesAll += dt;
  if (!c) g_texNullAll++;  // slot 26 does nothing for a NULL texture
  if (t_inShadow) {
    g_texCalls++;
    g_texCycles += dt;
  }
  return r;
}

bool Transparent(void* mat) {
  auto* props = *reinterpret_cast<uint8_t**>(static_cast<uint8_t*>(mat) + 0x28);
  return props && props[0x33] != 0;
}

uint64_t __fastcall HookGlass(void* a, void* b, void* c, void* d) {
  if (!g_on.load(std::memory_order_relaxed)) return g_origGlass(a, b, c, d);
  g_glass++;
  if (!Transparent(a)) return g_origGlass(a, b, c, d);
  g_glassTransparent++;
  uint64_t t0 = __rdtsc();
  uint64_t r = g_origGlass(a, b, c, d);
  g_glassCycles += __rdtsc() - t0;
  return r;
}

uint64_t __fastcall HookModel(void* a, void* b, void* c, void* d) {
  if (!g_on.load(std::memory_order_relaxed)) return ModelTarget()(a, b, c, d);
  g_model++;
  uint64_t t0 = __rdtsc();
  t_inShadow = true;
  uint64_t r = ModelTarget()(a, b, c, d);
  t_inShadow = false;
  g_modelCycles += __rdtsc() - t0;
  return r;
}

bool Install() {
  if (g_origGlass) return true;
  auto* base = reinterpret_cast<uint8_t*>(GetModuleHandleW(L"NGModel.dll"));
  if (!base) return false;
  // NGModel 2.9.31 RVAs, mapped to this build (ng_build.h: 2.9.32 moved ModelMaterialMT and both slot targets).
  auto** glass = reinterpret_cast<void**>(base + ngb::At(base, 0x590d8));
  auto** model = reinterpret_cast<void**>(base + ngb::At(base, 0x592f0));
  if (!allocslab::RttiIs(base, glass, ".?AVGlassMaterialMT@model@@") ||
      !allocslab::RttiIs(base, model, ".?AVModelMaterialMT@model@@") ||
      reinterpret_cast<uint8_t*>(SlotOriginal(&glass[5])) != base + ngb::At(base, 0x14980) ||
      reinterpret_cast<uint8_t*>(SlotOriginal(&model[5])) != base + ngb::At(base, 0x17750)) {
    Log("shadow caster counter: NGModel vtables do not match this build; skipped");
    return false;
  }
  auto* dx = reinterpret_cast<uint8_t*>(GetModuleHandleW(L"dx11backend.dll"));
  auto** shader = dx ? reinterpret_cast<void**>(dx + 0xb43b8) : nullptr;
  if (!shader || !allocslab::RttiIs(dx, shader, ".?AVDX11Shader@RenderAPI@@") ||
      reinterpret_cast<uint8_t*>(SlotOriginal(&shader[26])) != dx + 0x1fd70) {
    Log("shadow caster counter: DX11Shader vtable does not match this build; skipped");
    return false;
  }
  g_origTex = reinterpret_cast<TexFn>(SlotOriginal(&shader[26]));
  if (!HookSlot(&shader[26], reinterpret_cast<void*>(&HookTex), nullptr)) return false;
  g_origGlass = reinterpret_cast<Fn>(SlotOriginal(&glass[5]));
  g_origModel = reinterpret_cast<Fn>(SlotOriginal(&model[5]));
  return HookSlot(&glass[5], reinterpret_cast<void*>(&HookGlass), nullptr) &&
         HookSlot(&model[5], reinterpret_cast<void*>(&HookModel), nullptr);
}

void Measure(int ms, std::atomic<uint64_t>& frames, double tscHz) {
  if (!Install()) return;
  g_glass = g_glassTransparent = g_model = g_glassCycles = 0;
  g_modelCycles = g_texCalls = g_texCycles = g_texCallsAll = g_texCyclesAll = g_texNullAll = 0;
  uint64_t f0 = frames.load();
  g_on = true;
  Sleep(ms);
  g_on = false;
  Sleep(50);
  double f = static_cast<double>(frames.load() - f0);
  if (f <= 0) return;
  Log("  shadow casters per frame: model %.0f, glass %.0f, glass with the transparent flag %.0f (%.3f ms/frame in them)",
      g_model / f, g_glass / f, g_glassTransparent / f, g_glassCycles / tscHz * 1000.0 / f);
  const double k = 1000.0 / tscHz / f;
  Log("  model shadow casters (slot 5): %.3f ms/frame; texture sets inside them %.0f/frame, %.3f ms/frame "
      "(all texture sets: %.0f/frame, %.3f ms/frame)",
      g_modelCycles * k, g_texCalls / f, g_texCycles * k, g_texCallsAll / f, g_texCyclesAll * k);
  Log("  texture sets with a NULL texture (slot 26 does nothing): %.1f/frame of all sets", g_texNullAll / f);
}

}  // namespace shadowcount
