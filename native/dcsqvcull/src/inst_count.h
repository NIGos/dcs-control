// Shadow caster batching potential (measurement only). NGModel's
// ShadowMapRenderable (vtable RVA 0x59828) slot 1 = render(ctx) is a tail
// jump to item->material->vt[5](material, item, mesh), with item = [this+0x10],
// material = [item+0x10], mesh = [item+0xc0] [V 0x443e0]. Casters that share
// (context, material, mesh) differ only in the per-object offset the material
// writes into its constant buffer (item+0xd4), so each such group could be one
// instanced draw with a modified shadow vertex shader. Counts, per frame:
// casters, distinct groups, and casters that directly follow one of their
// group (batchable without reordering).
// Included once from main.cpp inside its anonymous namespace.
#pragma once

namespace instcount {

constexpr uint32_t kVtableRva = 0x59828;
using Fn = uint64_t(__fastcall*)(void* self, void* ctx);
Fn g_orig = nullptr;
std::atomic<bool> g_on{false};
// Optional per-caster observer (shadow_inst.h), called before the counter on
// the caller's thread; nullptr when unused.
using ObserverFn = void (*)(void* self, void* ctx);
std::atomic<ObserverFn> g_observer{nullptr};
// Optional per-caster override (shadow_batch.h): returns true when it handled
// the call (result in *ret); nullptr when unused.
using OverrideFn = bool (*)(void* self, void* ctx, uint64_t* ret);
std::atomic<OverrideFn> g_override{nullptr};

// Full group key (R12 §2.2): cascade context, material, mesh, the
// sbPositions selector [item+0xd0] and the texture-entry array [item+0x18].
struct Key {
  void* ctx;
  void* mat;
  void* mesh;
  uint64_t sel;
  void* tex;
  bool operator==(const Key& o) const {
    return ctx == o.ctx && mat == o.mat && mesh == o.mesh && sel == o.sel && tex == o.tex;
  }
};
struct KeyHash {
  size_t operator()(const Key& k) const {
    uint64_t h = reinterpret_cast<uint64_t>(k.ctx) * 0x9E3779B97F4A7C15ull;
    h ^= reinterpret_cast<uint64_t>(k.mat) * 0xC2B2AE3D27D4EB4Full;
    h ^= reinterpret_cast<uint64_t>(k.mesh) * 0x165667B19E3779F9ull;
    h ^= (k.sel + reinterpret_cast<uint64_t>(k.tex)) * 0x27D4EB2F165667C5ull;
    return static_cast<size_t>(h ^ (h >> 29));
  }
};

// Render thread only (pass execution); the frame counter splits frames.
// Material constant-buffer bytes mat+0x90..0x1c0 (0x130), snapshot at the
// group's first caster; posStructOffset (mat+0x18c) is excluded from the
// compare. A member whose bytes differ would break one-draw-per-group.
constexpr size_t kCbOff = 0x90, kCbLen = 0x130, kPsoOff = 0x18c;
struct Group {
  uint32_t count = 0;
  uint8_t cb[kCbLen];
};
std::unordered_map<Key, Group, KeyHash> g_groups;
std::unordered_map<Key, uint32_t, KeyHash> g_groupsNoTex;  // same key without [item+0x18]
uint64_t g_distinctNoTex = 0;
std::unordered_set<uint32_t> g_sels;
uint64_t g_selCount = 0;
uint32_t g_selMax = 0;
uint64_t g_cbChecked = 0, g_cbDiffer = 0, g_otherMat = 0;
uint64_t g_hist[6] = {};  // casters in groups of 1, 2-3, 4-7, 8-15, 16-63, 64+
void* g_modelMatVtbl = nullptr;
std::unordered_set<void*> g_ctxs;
uint64_t g_frame = 0;
Key g_prev{};
uint64_t g_casters = 0, g_distinct = 0, g_consecutive = 0, g_frames = 0, g_ctxCount = 0;
uint64_t g_big = 0;  // casters in groups of 4 or more (counted at frame end)
uint64_t g_fCasters = 0, g_fConsecutive = 0;  // current frame, added at its end
std::atomic<uint64_t>* g_frameCounter = nullptr;

void EndFrame() {
  for (auto& kv : g_groups) {
    const uint32_t n = kv.second.count;
    if (n >= 4) g_big += n;
    g_hist[n == 1 ? 0 : n < 4 ? 1 : n < 8 ? 2 : n < 16 ? 3 : n < 64 ? 4 : 5] += n;
  }
  g_distinct += g_groups.size();
  g_distinctNoTex += g_groupsNoTex.size();
  g_groupsNoTex.clear();
  g_selCount += g_sels.size();
  g_sels.clear();
  g_casters += g_fCasters;
  g_consecutive += g_fConsecutive;
  g_fCasters = g_fConsecutive = 0;
  g_ctxCount += g_ctxs.size();
  g_groups.clear();
  g_ctxs.clear();
  ++g_frames;
}

// Textured caster test and texture key, as ModelMaterialMT slot 5 does it
// [V NGModel 0x177a5..0x1785c]: a caster binds textures when
// [props+8] != 0 or props has a valid texture 0xF or 0x12; the bound entries
// are (aux, tex) pairs at (*[item+0x18]) + [props+0x26c]*0x18, stride 0x18,
// for every i < [mat+0x2d8] whose handle [mat+0x240+8i] is not -1.
using GetTexFn = const void*(__fastcall*)(const void* props, uint32_t id);
using ValidFn = bool(__fastcall*)(const void* tex);
GetTexFn g_getTex = nullptr;
ValidFn g_valid = nullptr;
uint64_t g_textured = 0;

// Per material: does this caster bind textures in the shadow path.
bool IsTextured(uint8_t* mat) {
  auto* props = *reinterpret_cast<uint8_t**>(mat + 0x28);
  if (!props || !g_getTex || !g_valid) return false;
  if (*reinterpret_cast<uint32_t*>(props + 8) != 0) return true;
  return g_valid(g_getTex(props, 0xF)) || g_valid(g_getTex(props, 0x12));
}

// Per item: hash of the bound (aux, tex) entries (never 0).
uint64_t TextureEntriesKey(uint8_t* mat, uint8_t* item) {
  auto* props = *reinterpret_cast<uint8_t**>(mat + 0x28);
  auto* arr = *reinterpret_cast<uint8_t***>(item + 0x18);
  if (!props || !arr || !*arr) return 1;
  const uint8_t* e = *arr + static_cast<size_t>(*reinterpret_cast<uint32_t*>(props + 0x26c)) * 0x18;
  uint64_t h = 0xcbf29ce484222325ull;
  const uint32_t n = ngb::TexN(mat);
  for (uint32_t i = 0; i < n && i < 32; ++i, e += 0x18) {
    if (ngb::Tex(mat)[i] == -1) continue;
    h = (h ^ *reinterpret_cast<const uint64_t*>(e)) * 0x100000001b3ull;
    h = (h ^ *reinterpret_cast<const uint64_t*>(e + 8)) * 0x100000001b3ull;
  }
  return h | 1;
}

uint64_t TextureKey(uint8_t* mat, uint8_t* item) {
  if (!IsTextured(mat)) return 0;
  ++g_textured;
  return TextureEntriesKey(mat, item);
}

uint64_t __fastcall Hook(void* self, void* ctx) {
  if (ObserverFn ob = g_observer.load(std::memory_order_relaxed)) ob(self, ctx);
  if (OverrideFn ov = g_override.load(std::memory_order_relaxed)) {
    uint64_t ret;
    if (ov(self, ctx, &ret)) return ret;
  }
  if (g_on.load(std::memory_order_relaxed)) {
    const uint64_t f = g_frameCounter->load(std::memory_order_relaxed);
    if (f != g_frame) {
      if (g_frame) EndFrame();
      g_frame = f;
      g_prev = {};
    }
    auto* item = *reinterpret_cast<uint8_t**>(static_cast<uint8_t*>(self) + 0x10);
    auto* mat = item ? *reinterpret_cast<uint8_t**>(item + 0x10) : nullptr;
    if (mat && *reinterpret_cast<void**>(mat) != g_modelMatVtbl) {
      ++g_otherMat;
      item = nullptr;  // only ModelMaterialMT casters are candidates (R12 eligibility)
    }
    if (item) {
      Key k{ctx, mat, *reinterpret_cast<void**>(item + 0xc0), *reinterpret_cast<uint32_t*>(item + 0xd0),
            reinterpret_cast<void*>(TextureKey(mat, item))};
      ++g_fCasters;
      g_sels.insert(static_cast<uint32_t>(k.sel));
      if (static_cast<uint32_t>(k.sel) > g_selMax) g_selMax = static_cast<uint32_t>(k.sel);
      Key k2 = k;
      k2.tex = nullptr;
      ++g_groupsNoTex[k2];
      Group& g = g_groups[k];
      if (g.count++ == 0) {
        memcpy(g.cb, mat + kCbOff, kCbLen);
      } else {
        ++g_cbChecked;
        const size_t p = kPsoOff - kCbOff;
        if (memcmp(g.cb, mat + kCbOff, p) != 0 || memcmp(g.cb + p + 4, mat + kCbOff + p + 4, kCbLen - p - 4) != 0)
          ++g_cbDiffer;
      }
      g_ctxs.insert(ctx);
      if (k == g_prev) ++g_fConsecutive;
      g_prev = k;
    }
  }
  return g_orig(self, ctx);
}

bool Install() {
  if (g_orig) return true;
  auto* base = reinterpret_cast<uint8_t*>(GetModuleHandleW(L"NGModel.dll"));
  if (!base) return false;
  // Recorded RVAs, or where reloc.h re-found them in another build (0: not found).
  const uint32_t vtRva = reloc::Rva(hooksig::NG_ShadowMapRenderable_vtbl, kVtableRva);
  const uint32_t fnRva = reloc::Rva(hooksig::NG_ShadowMapRenderable_render, 0x443e0);
  const uint32_t matRva = reloc::Rva(hooksig::NG_ModelMaterialMT_vtbl, 0x592f0);
  auto** vtbl = reinterpret_cast<void**>(base + vtRva);
  if (!vtRva || !fnRva || !matRva || !allocslab::RttiIs(base, vtbl, ".?AVShadowMapRenderable@model@@") ||
      reinterpret_cast<uint8_t*>(SlotOriginal(&vtbl[1])) != base + fnRva) {
    Log("shadow batching counter: vtable does not match this build; skipped");
    return false;
  }
  g_modelMatVtbl = base + matRva;
  if (HMODULE md = GetModuleHandleW(L"ModelDesc.dll")) {
    g_getTex = reinterpret_cast<GetTexFn>(
        GetProcAddress(md, "?getTexture@PropertiesSet@model@@QEBAAEBUTexture2dProperties@2@I@Z"));
    g_valid = reinterpret_cast<ValidFn>(GetProcAddress(md, "?valid@Texture2dProperties@model@@QEBA_NXZ"));
  }
  g_orig = reinterpret_cast<Fn>(SlotOriginal(&vtbl[1]));
  return HookSlot(&vtbl[1], reinterpret_cast<void*>(&Hook), nullptr);
}

void Measure(int ms, std::atomic<uint64_t>& frames) {
  g_frameCounter = &frames;
  if (!Install()) return;
  g_groups.clear();
  g_ctxs.clear();
  g_frame = 0;
  g_casters = g_distinct = g_consecutive = g_frames = g_ctxCount = g_big = 0;
  g_fCasters = g_fConsecutive = 0;
  g_cbChecked = g_cbDiffer = g_otherMat = 0;
  g_textured = 0;
  g_distinctNoTex = 0;
  g_groupsNoTex.clear();
  g_sels.clear();
  g_selCount = 0;
  g_selMax = 0;
  memset(g_hist, 0, sizeof(g_hist));
  g_on = true;
  Sleep(ms);
  g_on = false;
  Sleep(100);  // the render thread may still be inside a frame; its partial frame is dropped
  if (g_frames == 0) return;
  const double f = static_cast<double>(g_frames);
  Log("  shadow casters: %.0f/frame in %.1f contexts, %.0f distinct (context, material, mesh) groups; "
      "%.0f could join their group (%.1f%%), %.0f follow their group directly; %.0f in groups of 4+",
      g_casters / f, g_ctxCount / f, g_distinct / f, (g_casters - g_distinct) / f,
      g_casters ? 100.0 * (g_casters - g_distinct) / g_casters : 0.0, g_consecutive / f, g_big / f);
  Log("  shadow groups by size (casters/frame): 1: %.0f, 2-3: %.0f, 4-7: %.0f, 8-15: %.0f, 16-63: %.0f, 64+: %.0f; "
      "other material classes: %.0f/frame",
      g_hist[0] / f, g_hist[1] / f, g_hist[2] / f, g_hist[3] / f, g_hist[4] / f, g_hist[5] / f, g_otherMat / f);
  Log("  shadow casters binding textures: %.0f/frame (texture entries are part of their group key)", g_textured / f);
  Log("  shadow groups without the texture-array key: %.0f distinct/frame; distinct [item+0xd0] values %.0f/frame, max %u",
      g_distinctNoTex / f, g_selCount / f, g_selMax);
  Log("  shadow group constant-buffer check: %llu members compared, %llu differ from their group's first caster",
      static_cast<unsigned long long>(g_cbChecked), static_cast<unsigned long long>(g_cbDiffer));
}

}  // namespace instcount
