// G-buffer batching potential (measurement only, R13 stage 0). NGModel's
// SceneRenderable (vtable RVA 0x59810) slot 1 = render(ctx) draws one model
// item in the main passes through mat->vt[4] [V 0x44350]. After the call the
// material constant buffer bytes (mat+0x90..0x1c0) hold exactly what this
// item uploaded (posStructOffset at 0x18c, the per-item matrix at 0xb0..0xef,
// the argument-driven fields), so a group key taken after the call is exact.
// Key: context, pass number [r+0x60], material, mesh [item+0xc0], second
// mesh [item+0xc8], position page [item+0xd0], size [r+0x6c] (8 bytes),
// bound texture entries, and the CB bytes except posStructOffset. A second
// count also leaves the per-item matrix out of the key.
// The same hook calls g_observer (shadow_inst.h's G-buffer key collection)
// before the original, then g_override (gb_batch.h's batching), which may
// handle the call itself.
// Included once from main.cpp inside its anonymous namespace, after
// inst_count.h.
#pragma once

namespace gbcount {

constexpr uint32_t kVtableRva = 0x59810;  // 2.9.31; SrVtblRva: this build's
inline uint32_t SrVtblRva(const uint8_t* ng) { return ngb::At(ng, kVtableRva); }
using Fn = uint64_t(__fastcall*)(void* self, void* ctx);
Fn g_orig = nullptr;
std::atomic<bool> g_on{false};
// Optional per-item observer (shadow_inst.h, G-buffer keys), called before the
// original on the caller's thread; nullptr when unused. This hook is the only
// HookSlot on SceneRenderable vt[1]: other users register here.
using ObserverFn = void (*)(void* self, void* ctx);
std::atomic<ObserverFn> g_observer{nullptr};
// Optional per-item override (gb_batch.h, G-buffer batching), called after the
// observer: returns true when it handled the call (result in *ret; the
// counter below is then skipped); nullptr when unused.
using OverrideFn = bool (*)(void* self, void* ctx, uint64_t* ret);
std::atomic<OverrideFn> g_override{nullptr};
void* g_modelMatVtbl = nullptr;
DWORD g_thread = 0;

struct Key {
  uint64_t a, b, c, d;
  bool operator==(const Key& o) const { return a == o.a && b == o.b && c == o.c && d == o.d; }
};
struct KeyHash {
  size_t operator()(const Key& k) const {
    uint64_t h = k.a * 0x9E3779B97F4A7C15ull ^ k.b * 0xC2B2AE3D27D4EB4Full ^ k.c * 0x165667B19E3779F9ull ^
                 k.d * 0x27D4EB2F165667C5ull;
    return static_cast<size_t>(h ^ (h >> 29));
  }
};

uint64_t Fnv(const uint8_t* p, size_t n, uint64_t h = 0xcbf29ce484222325ull) {
  for (size_t i = 0; i < n; ++i) h = (h ^ p[i]) * 0x100000001b3ull;
  return h;
}

std::unordered_map<Key, uint32_t, KeyHash> g_full, g_noMatrix, g_noCb, g_noSize, g_noSizeTex, g_matMesh, g_srvKey, g_srvNoM;
std::unordered_map<uint32_t, uint64_t> g_byPass;  // pass number -> draws (whole window)
std::unordered_set<uint64_t> g_sizes;
uint64_t g_frame = 0, g_frames = 0, g_draws = 0, g_other = 0, g_identity = 0;
uint64_t g_dFull = 0, g_dNoMatrix = 0, g_dNoCb = 0, g_fDraws = 0, g_sizeCount = 0, g_dNoSize = 0, g_dNoSizeTex = 0, g_dMatMesh = 0, g_dSrv = 0, g_dSrvNoM = 0;
std::atomic<uint64_t>* g_frameCounter = nullptr;

void EndFrame() {
  g_dFull += g_full.size();
  g_dNoMatrix += g_noMatrix.size();
  g_dNoCb += g_noCb.size();
  g_dNoSize += g_noSize.size();
  g_dNoSizeTex += g_noSizeTex.size();
  g_dMatMesh += g_matMesh.size();
  g_dSrv += g_srvKey.size();
  g_dSrvNoM += g_srvNoM.size();
  g_srvKey.clear();
  g_srvNoM.clear();
  g_noSize.clear();
  g_noSizeTex.clear();
  g_matMesh.clear();
  g_draws += g_fDraws;
  g_sizeCount += g_sizes.size();
  g_full.clear();
  g_noMatrix.clear();
  g_noCb.clear();
  g_sizes.clear();
  g_fDraws = 0;
  ++g_frames;
}

void Note(void* self, void* ctx) {
  auto* r = static_cast<uint8_t*>(self);
  auto* item = *reinterpret_cast<uint8_t**>(r + 0x10);
  if (!item) return;
  auto* mat = *reinterpret_cast<uint8_t**>(item + 0x10);
  if (!mat || *reinterpret_cast<void**>(mat) != g_modelMatVtbl) {
    ++g_other;
    return;
  }
  const uint64_t f = g_frameCounter->load(std::memory_order_relaxed);
  if (f != g_frame) {
    if (g_frame) EndFrame();
    g_frame = f;
  }
  ++g_fDraws;
  const uint32_t pass = *reinterpret_cast<uint32_t*>(r + 0x60);
  ++g_byPass[pass];
  const uint64_t size = *reinterpret_cast<uint64_t*>(r + 0x6c);
  g_sizes.insert(size);
  // Bound texture entries ((*[item+0x18]) + [props+0x26c]*0x18, as slot 5).
  uint64_t tex = 0;
  auto* props = *reinterpret_cast<uint8_t**>(mat + 0x28);
  auto* arr = *reinterpret_cast<uint8_t***>(item + 0x18);
  if (props && arr && *arr) {
    const uint8_t* e = *arr + static_cast<size_t>(*reinterpret_cast<uint32_t*>(props + 0x26c)) * 0x18;
    const uint32_t n = ngb::TexN(mat);
    tex = Fnv(e, static_cast<size_t>(n < 32 ? n : 32) * 0x18);
  }
  // CB bytes 0x90..0x1c0 without posStructOffset (0x18c..0x18f).
  const uint8_t* cb = mat + 0x90;
  const uint64_t cbAll = Fnv(cb + 0xfc + 4, 0x130 - 0x100, Fnv(cb, 0xfc));
  // ... and without the per-item matrix (mat+0xb0..0xef = cb+0x20..0x5f).
  const uint64_t cbNoM = Fnv(cb + 0xfc + 4, 0x130 - 0x100, Fnv(cb + 0x60, 0xfc - 0x60, Fnv(cb, 0x20)));
  static const float kIdentity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
  if (memcmp(mat + 0xb0, kIdentity, sizeof(kIdentity)) == 0) ++g_identity;
  const uint64_t base = reinterpret_cast<uint64_t>(ctx) ^ (static_cast<uint64_t>(pass) << 56);
  const uint64_t geo = reinterpret_cast<uint64_t>(*reinterpret_cast<void**>(item + 0xc0)) * 31 ^
                       reinterpret_cast<uint64_t>(*reinterpret_cast<void**>(item + 0xc8)) ^
                       (static_cast<uint64_t>(*reinterpret_cast<uint32_t*>(item + 0xd0)) << 48);
  ++g_full[{base, reinterpret_cast<uint64_t>(mat) ^ size, geo ^ tex, cbAll}];
  ++g_noMatrix[{base, reinterpret_cast<uint64_t>(mat) ^ size, geo ^ tex, cbNoM}];
  ++g_noCb[{base, reinterpret_cast<uint64_t>(mat) ^ size, geo ^ tex, 0}];
  ++g_noSize[{base, reinterpret_cast<uint64_t>(mat), geo ^ tex, 0}];
  // The views actually bound: each texture variable's resource after the
  // call, rec = [shader+0xc8] + h*0x50, var = [rec+0x40], srv = [[var+8]]
  // [V dx11backend 0x1fd70, SetResource 0x61ca0].
  uint64_t srvs = 0xcbf29ce484222325ull;
  if (auto* shader = *reinterpret_cast<uint8_t**>(mat + 0x30)) {
    auto* recs = *reinterpret_cast<uint8_t**>(shader + 0xc8);
    const uint32_t n = ngb::TexN(mat);
    for (uint32_t i = 0; recs && i < n && i < 32; ++i) {
      const int64_t h = ngb::Tex(mat)[i];
      if (h == -1) continue;
      auto* var = *reinterpret_cast<uint8_t**>(recs + h * 0x50 + 0x40);
      const uint64_t srv = var ? **reinterpret_cast<uint64_t**>(var + 8) : 0;
      srvs = (srvs ^ srv) * 0x100000001b3ull;
    }
  }
  ++g_srvKey[{base, reinterpret_cast<uint64_t>(mat), geo ^ srvs, cbAll}];
  ++g_srvNoM[{base, reinterpret_cast<uint64_t>(mat), geo ^ srvs, cbNoM}];
  ++g_noSizeTex[{base, reinterpret_cast<uint64_t>(mat), geo, 0}];
  ++g_matMesh[{reinterpret_cast<uint64_t>(ctx), reinterpret_cast<uint64_t>(mat),
               reinterpret_cast<uint64_t>(*reinterpret_cast<void**>(item + 0xc0)), pass}];
}

void NoteGuarded(void* self, void* ctx) {
  __try {
    Note(self, ctx);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }
}

uint64_t __fastcall Hook(void* self, void* ctx) {
  if (ObserverFn ob = g_observer.load(std::memory_order_relaxed)) ob(self, ctx);
  if (OverrideFn ov = g_override.load(std::memory_order_relaxed)) {
    uint64_t ret;
    if (ov(self, ctx, &ret)) return ret;
  }
  const uint64_t r = g_orig(self, ctx);
  if (g_on.load(std::memory_order_relaxed)) {
    if (!g_thread) g_thread = GetCurrentThreadId();
    if (GetCurrentThreadId() == g_thread) NoteGuarded(self, ctx);
  }
  return r;
}

bool Install() {
  if (g_orig) return true;
  auto* base = reinterpret_cast<uint8_t*>(GetModuleHandleW(L"NGModel.dll"));
  if (!base) return false;
  auto** vtbl = reinterpret_cast<void**>(base + SrVtblRva(base));
  if (!allocslab::RttiIs(base, vtbl, ".?AVSceneRenderable@model@@") ||
      reinterpret_cast<uint8_t*>(SlotOriginal(&vtbl[1])) != base + ngb::At(base, 0x44350)) {
    Log("g-buffer counter: SceneRenderable vtable does not match this build; skipped");
    return false;
  }
  g_modelMatVtbl = base + ngb::At(base, 0x592f0);
  g_orig = reinterpret_cast<Fn>(SlotOriginal(&vtbl[1]));
  return HookSlot(&vtbl[1], reinterpret_cast<void*>(&Hook), nullptr);
}

void Measure(int ms, std::atomic<uint64_t>& frames) {
  g_frameCounter = &frames;
  if (!Install()) return;
  g_full.clear();
  g_noMatrix.clear();
  g_noCb.clear();
  g_byPass.clear();
  g_sizes.clear();
  g_frame = g_frames = g_draws = g_other = g_identity = 0;
  g_dFull = g_dNoMatrix = g_dNoCb = g_fDraws = g_sizeCount = 0;
  g_dNoSize = g_dNoSizeTex = g_dMatMesh = g_dSrv = g_dSrvNoM = 0;
  g_srvKey.clear();
  g_srvNoM.clear();
  g_noSize.clear();
  g_noSizeTex.clear();
  g_matMesh.clear();
  g_thread = 0;
  g_on = true;
  Sleep(ms);
  g_on = false;
  Sleep(100);
  if (g_frames == 0) return;
  const double f = static_cast<double>(g_frames);
  const double d = g_draws / f;
  Log("  model main-pass draws: %.0f/frame (ModelMaterialMT), other materials %.0f/frame, distinct sizes %.1f/frame, "
      "per-item matrix identity in %.1f%%",
      d, g_other / f, g_sizeCount / f, g_draws ? 100.0 * g_identity / g_draws : 0.0);
  Log("  groups/frame: full key %.0f (%.1f%% joinable), without the per-item matrix %.0f (%.1f%%), "
      "without CB contents %.0f (%.1f%%)",
      g_dFull / f, d ? 100.0 * (d - g_dFull / f) / d : 0.0, g_dNoMatrix / f,
      d ? 100.0 * (d - g_dNoMatrix / f) / d : 0.0, g_dNoCb / f, d ? 100.0 * (d - g_dNoCb / f) / d : 0.0);
  Log("  groups/frame: also without size %.0f (%.1f%%), also without textures %.0f (%.1f%%), "
      "context+material+mesh+pass only %.0f (%.1f%%)",
      g_dNoSize / f, d ? 100.0 * (d - g_dNoSize / f) / d : 0.0, g_dNoSizeTex / f,
      d ? 100.0 * (d - g_dNoSizeTex / f) / d : 0.0, g_dMatMesh / f, d ? 100.0 * (d - g_dMatMesh / f) / d : 0.0);
  Log("  groups/frame keyed by the bound views instead of texture entries and size: %.0f (%.1f%%), "
      "same without the per-item matrix %.0f (%.1f%%)",
      g_dSrv / f, d ? 100.0 * (d - g_dSrv / f) / d : 0.0, g_dSrvNoM / f, d ? 100.0 * (d - g_dSrvNoM / f) / d : 0.0);
  std::string passes;
  char buf[64];
  for (auto& kv : g_byPass) {
    snprintf(buf, sizeof(buf), " %u:%.0f", kv.first, kv.second / f);
    passes += buf;
  }
  Log("  draws/frame by pass number:%s", passes.c_str());
}

}  // namespace gbcount
