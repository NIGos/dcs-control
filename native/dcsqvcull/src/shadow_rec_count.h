// Shadow recorder stage S0 counters (R17 section 7, R15 A1). Measurement only:
// nothing is drawn differently, no DCS memory is written.
//
// Tags: [V] verified in the binary (DCS 2.9.30) or the shader sources, [I]
// inferred. Facts from R17 unless stated.
//
// While [Suite] ShadowRecCount runs (1 s discovery + 5 s count), per cascade
// pass (GraphicsCore 0xa5500 via shadow_pass.h's execute hook):
//  1. Caster mix. The pass's caster vector ([[ctx]+0x438] + id*24) is walked
//     before the pass runs and every entry is classed: ShadowMapRenderable
//     (NGModel vtbl 0x59828) with ModelMaterialMT (0x592f0), untextured or
//     textured (binds texture sets in the shadow path: inst_count.h
//     IsTextured, NGModel 0x177a5..0x1785c [V]); SMR + DeckMaterialMT; SMR +
//     GlassMaterialMT (both by RTTI name); SMR + anything else; every other
//     renderable class by vtable (RTTI name + module!rva in the log).
//  2. Render-thread time inside the cascade loop by class. The loop calls
//     item->vt[1](item, data+0x10) per caster [V GC 0xa5640]:
//      - SMR casters through inst_count.h's hook: our override is chained in
//        front of the existing one (shadow_batch.h) for the phase, times the
//        call (the batching override or the original) and always handles it;
//      - other classes: their vt[1] slot is redirected for the phase to a
//        timing thunk (only slots no other module hooks; the others stay
//        untimed and are listed as such).
//     Times: wrap total (shadow batching's planner work included), DCS's
//     execute, its pre-loop part (frame buffer, clear, per-view binder: up to
//     the first caster), the loop (first caster entry to last caster exit),
//     the post-loop part (frame-buffer pop, plus batching's RestoreMats
//     outside the execute). Our own probes and bookkeeping inside the loop
//     are measured (rdtsc) and subtracted. R17 gate: ModelMaterialMT casters
//     >= 70 % of the loop time.
//  3. Pass setup stability per learned cascade (render graph, collection
//     index): at the first caster (after setRenderTargets, clear and binder)
//     OMGetRenderTargets -> DSV object, its resource and DSV_DESC (format,
//     slice); RSGetViewports; DX11Renderer +0xd4 flags (wireframe/+0x178/
//     +0xf2 bits, ApplyPassBlock's rasterizer index [V 0x1aa10, 0x67f90]) and
//     +0x2120 (draw-skip debug flags [V 0x14870]). Changes against the
//     previous pass of the same cascade are counted.
//  4. Depth/blend/raster state per caster class: OMGetDepthStencilState,
//     OMGetBlendState, RSGetState before and after the first caster of each
//     class per pass. Distinct combinations are logged with their descs and
//     whether the caster set them (state differs after the call). R17 section
//     4: order is free when no blending and a max depth test (GREATER or
//     GREATER_EQUAL). [I] A caster that draws several times inside one vt[1]
//     shows only its last state.
//  5. Material constant-buffer bytes between RenderGraph::render entry and the
//     pass. At render entry (shadow_batch.h's import hook, through an
//     observer; the hook is installed for the phase if batching did not) the
//     def_uniforms bytes (mat+0x90..0x1c0) of every ModelMaterialMT in the
//     learned cascade vectors of that graph are copied. At the pass each
//     material is compared once: the shadow-read register (diffuseShift, CB
//     +0xc0 = mat+0x150, read by the alpha-tested shadow PS; posStructOffset
//     CB +0xfc excluded, rewritten per caster anyway) and the whole CB except
//     posStructOffset (upper bound). Also static: the material's animated
//     property list (mat+8..0x10, {dst, prop} stride 16, ModelDesc
//     AnimatedProperty<..> / ArgumentProperty writing 4..16 bytes at dst [V
//     gb_batch.h]) writes into the read register / into the CB.
//  6. Texture predictability of textured ModelMaterialMT casters (S4 needs a
//     render-thread prediction of every SRV slot 26 binds): per texture set
//     the shadow pass reads (shadow_tex.h's mask when cached; every set
//     otherwise), gb_batch.h PredictView with size Vec2i(-1,-1) (slot 5
//     passes that size [V NGModel 0x17750]); reasons when not predictable;
//     unique textures per pass (the vt[23] replay count S4 would add).
//  7. Driver: D3D11_FEATURE_THREADING (DriverCommandLists,
//     DriverConcurrentCreates), D3D11_FEATURE_D3D11_OPTIONS
//     (ConstantBufferOffsetting, MapNoOverwriteOnDynamicConstantBuffer),
//     ID3D11Device1 / ID3D11DeviceContext1, creation flags (SINGLETHREADED).
//
// With the key at 0 nothing here runs: no hook, no observer, no chained
// pointer. All chained pointers are put back and all slots restored after the
// phase (and in Shutdown at payload stop).
// Included once from main.cpp inside its anonymous namespace, after
// pass_timing.h (RttiRaw), shadow_pass.h, inst_count.h, shadow_tex.h,
// shadow_inst.h, shadow_batch.h and gb_batch.h (PredictView).
#pragma once

namespace shadowrec {

constexpr int kSlots = 8;    // learned cascades; index kSlots = not learnable
constexpr int kMaxNs = 16;   // other renderable classes (by vtable)
enum : int { kClsModel = 0, kClsModelTex, kClsDeck, kClsGlass, kClsSmrOther, kClsNs0 };
constexpr int kClsNsOver = kClsNs0 + kMaxNs;
constexpr int kClasses = kClsNsOver + 1;
constexpr size_t kMaxCasters = 1 << 15;
constexpr uint32_t kCbOff = 0x90, kCbLen = 0x130;  // def_uniforms bytes in the material
constexpr uint32_t kPsoCb = 0xfc;                  // posStructOffset (CB offset)
constexpr uint32_t kReadCb = 0xc0, kReadLen = 0x10; // diffuseShift register (CB offset)
constexpr size_t kMatTable = 1 << 14;               // power of two
constexpr size_t kTexSet = 1 << 13;                 // power of two
constexpr int kCombos = 32;
constexpr int32_t kNoCascade = INT32_MIN;

std::atomic<bool> g_on{false};
std::atomic<bool> g_chained{false};
std::atomic<int> g_inside{0};
DWORD g_renderTid = 0;
double g_tscHz = 0;
void* g_smrVt = nullptr;
void* g_modelMatVt = nullptr;
uint8_t* g_md = nullptr;  // ModelDesc.dll
ID3D11DeviceContext* g_ctx = nullptr;
bool g_texOk = false;
gbbatch::TexEnv g_env;
shadowpass::WrapFn g_prevWrap = nullptr;
instcount::OverrideFn g_prevOverride = nullptr;
bool g_ownRenderHook = false;

// ---- Classes ----
using VtFn = uint64_t(__fastcall*)(void*, void*, void*, void*);
struct NsClass {
  void* vt;
  void** slot;     // &vt[1]
  int state;       // 0 = not hooked, 1 = hooked, -1 = slot hooked by another module, -2 = no module
  bool timed;      // hooked during the last count (for the report)
  char name[160];
};
NsClass g_ns[kMaxNs];
std::atomic<int> g_nsCount{0};
void* g_nsOrig[kMaxNs] = {};
struct MatClass {
  void* vt;
  int cls;
  char name[96];
};
MatClass g_matCls[16];
int g_matClsCount = 0;

// ---- Per-cascade statistics (render thread; read after the phase) ----
struct ClassStat {
  uint64_t casters, calls, cyc;
};
struct Combo {
  int cls;
  void* dss;
  void* bs;
  void* rs;
  UINT ref;
  bool changed;
  uint64_t count;
  bool haveDesc;
  D3D11_DEPTH_STENCIL_DESC dd;
  D3D11_BLEND_DESC bd;
  D3D11_RASTERIZER_DESC rd;
};
struct SlotStat {
  uint64_t passes, emptyPasses, origCalls, faults, oversize;
  uint64_t cycWrap, cycOrig, cycPre, cycLoop, cycPost, cycProbe, cycAnalyse;
  ClassStat cls[kClasses];
  int32_t cascade;
  bool cascadeVaries;
  // Setup stability.
  uint64_t samples, dsvNull, dsvChange, resChange, descChange, vpChange, vpCountChange, flagChange, dbgChange;
  bool have;
  void* dsv;
  void* res;
  D3D11_DEPTH_STENCIL_VIEW_DESC desc;
  UINT nvp;
  D3D11_VIEWPORT vp;
  uint32_t flags, dbg;
  bool flagsOk;
  void* resSeen[4];
  int resSeenCount;
  bool resSeenOver;
  // Materials (ModelMaterialMT), once per material per pass.
  uint64_t mats, matsNoSnap, matsReadChg, matsCbChg, matsTex, matsTexReadChg, matsAnimRead, matsAnimCb, matsNoEntry;
  uint64_t castersReadChg, castersCbChg, castersAnimRead;
  // Textures of textured ModelMaterialMT casters.
  uint64_t texCasters, texPredictable, texNoShader, texNoEntries, texTooMany, texNoMask, texRead, texMaskSkipped,
      texUnique, texBadHandle;
  uint64_t texReason[gbbatch::kReasons];
};
SlotStat g_stat[kSlots + 1];
struct Learned {
  void* rg;
  uint32_t idx;
};
Learned g_learned[kSlots];
int g_learnedCount = 0;
Combo g_combo[kCombos];
int g_comboCount = 0;
uint64_t g_comboOver = 0;
uint64_t g_unplanned = 0;     // caster calls not found in the walked vector
uint64_t g_snapRenders = 0, g_snapCyc = 0, g_snapMats = 0, g_snapFaults = 0, g_snapNoVector = 0;
uint64_t g_tableWipes = 0;

// ---- Material table (render thread) ----
enum : uint8_t { kMfTextured = 1, kMfNoSnap = 2, kMfReadChg = 4, kMfCbChg = 8, kMfAnimRead = 16, kMfAnimCb = 32 };
struct MatEntry {
  uint8_t* mat;
  uint32_t snapGen;  // g_renderGen of the copy (0 = none)
  uint32_t passGen;  // g_passGen of the last compare
  uint8_t flags;
  uint8_t cb[kCbLen];
};
MatEntry* g_mats = nullptr;
size_t g_matsUsed = 0;
uint32_t g_renderGen = 0, g_passGen = 0;
void* g_renderRg = nullptr;  // graph of the latest render entry

// ---- Per-pass thread state (render thread) ----
uint8_t g_cls[kMaxCasters];
struct TexSlot {
  void* p;
  uint32_t gen;
};
TexSlot g_texSet[kTexSet];
thread_local bool t_inPass = false;
thread_local int t_depth = 0;
thread_local int t_slot = kSlots;
thread_local void* const* t_begin = nullptr;
thread_local size_t t_n = 0, t_cursor = 0;
thread_local uint64_t t_first = 0, t_last = 0, t_probe = 0;
thread_local uint32_t t_probed = 0;
thread_local shadowpass::ExecFn t_origFn = nullptr;

inline size_t PtrHash(const void* p, size_t mask) {
  return static_cast<size_t>((reinterpret_cast<uintptr_t>(p) >> 4) * 0x9E3779B97F4A7C15ull >> 40) & mask;
}

MatEntry* MatFind(uint8_t* mat) {
  if (!g_mats) return nullptr;
  size_t i = PtrHash(mat, kMatTable - 1);
  for (size_t k = 0; k < kMatTable; ++k, i = (i + 1) & (kMatTable - 1)) {
    MatEntry& e = g_mats[i];
    if (e.mat == mat) return &e;
    if (!e.mat) {
      if (g_matsUsed >= kMatTable * 3 / 4) return nullptr;
      e.mat = mat;
      e.snapGen = e.passGen = 0;
      e.flags = 0;
      ++g_matsUsed;
      return &e;
    }
  }
  return nullptr;
}

// ---- Class lookup ----
void VtName(void* vt, char* buf, size_t n) {
  buf[0] = 0;
  void* fake = vt;
  ptiming::RttiRaw(&fake, buf, n);
  if (!buf[0]) snprintf(buf, n, "(no RTTI)");
}

int MatClassOf(void* vt) {
  for (int i = 0; i < g_matClsCount; ++i)
    if (g_matCls[i].vt == vt) return g_matCls[i].cls;
  char name[96];
  VtName(vt, name, sizeof(name));
  const int cls = strstr(name, "DeckMaterial") ? kClsDeck : strstr(name, "GlassMaterial") ? kClsGlass : kClsSmrOther;
  if (g_matClsCount < 16) {
    MatClass& m = g_matCls[g_matClsCount++];
    m.vt = vt;
    m.cls = cls;
    memcpy(m.name, name, sizeof(name));
  }
  return cls;
}

int NsClassOf(void* vt) {
  const int n = g_nsCount.load(std::memory_order_relaxed);
  for (int i = 0; i < n; ++i)
    if (g_ns[i].vt == vt) return kClsNs0 + i;
  if (n >= kMaxNs) return kClsNsOver;
  NsClass& c = g_ns[n];
  c.vt = vt;
  c.slot = static_cast<void**>(vt) + 1;
  c.state = 0;
  c.timed = false;
  char rtti[112];
  VtName(vt, rtti, sizeof(rtti));
  HMODULE m = nullptr;
  char mod[MAX_PATH] = "?";
  uintptr_t rva = 0;
  if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                         static_cast<LPCWSTR>(vt), &m) &&
      m) {
    char path[MAX_PATH];
    if (GetModuleFileNameA(m, path, MAX_PATH)) {
      const char* b = strrchr(path, '\\');
      strncpy_s(mod, b ? b + 1 : path, _TRUNCATE);
    }
    rva = reinterpret_cast<uintptr_t>(vt) - reinterpret_cast<uintptr_t>(m);
  } else {
    c.state = -2;
  }
  snprintf(c.name, sizeof(c.name), "%s %s!0x%llx", rtti, mod, static_cast<unsigned long long>(rva));
  g_nsCount.store(n + 1, std::memory_order_release);
  return kClsNs0 + n;
}

// Class of one caster; -1 = ModelMaterialMT (textured or not: decided from
// the material entry).
int ClassOf(void* r, uint8_t** matOut, uint8_t** itemOut) {
  void* vt = *static_cast<void**>(r);
  if (vt != g_smrVt) return NsClassOf(vt);
  auto* item = *reinterpret_cast<uint8_t**>(static_cast<uint8_t*>(r) + 0x10);
  if (!item) return kClsSmrOther;
  auto* mat = *reinterpret_cast<uint8_t**>(item + 0x10);
  if (!mat) return kClsSmrOther;
  void* mv = *reinterpret_cast<void**>(mat);
  if (mv == g_modelMatVt) {
    *matOut = mat;
    *itemOut = item;
    return -1;
  }
  return MatClassOf(mv);
}

// ---- Material checks ----
// Animated property list: does any entry write into [lo, hi) of the material?
bool AnimWrites(uint8_t* mat, uint32_t lo, uint32_t hi) {
  uint8_t* b = *reinterpret_cast<uint8_t**>(mat + 8);
  uint8_t* e = *reinterpret_cast<uint8_t**>(mat + 0x10);
  if (!b || e <= b || (e - b) % 16 || (e - b) / 16 > 256) return false;
  for (uint8_t* p = b; p < e; p += 16) {
    uint8_t* dst = *reinterpret_cast<uint8_t**>(p);
    uint8_t* prop = *reinterpret_cast<uint8_t**>(p + 8);
    uint32_t bytes = 16;  // unknown class: widest write
    if (prop && g_md) {
      const void* pv = *reinterpret_cast<void**>(prop);
      for (const gbbatch::PropClass& pc : gbbatch::kProps)
        if (pv == g_md + pc.vtbl) bytes = pc.bytes;
    }
    const intptr_t off = dst - mat;
    if (off < static_cast<intptr_t>(hi) && off + static_cast<intptr_t>(bytes) > static_cast<intptr_t>(lo)) return true;
  }
  return false;
}

void VisitMat(MatEntry& e, uint8_t* mat, SlotStat& st) {
  e.passGen = g_passGen;
  uint8_t f = 0;
  if (instcount::IsTextured(mat)) f |= kMfTextured;
  if (!e.snapGen || e.snapGen != g_renderGen) {
    f |= kMfNoSnap;
  } else {
    const uint8_t* now = mat + kCbOff;
    if (memcmp(e.cb + kReadCb, now + kReadCb, kReadLen) != 0) f |= kMfReadChg;
    if (memcmp(e.cb, now, kPsoCb) != 0 || memcmp(e.cb + kPsoCb + 4, now + kPsoCb + 4, kCbLen - kPsoCb - 4) != 0)
      f |= kMfCbChg;
  }
  if (AnimWrites(mat, kCbOff + kReadCb, kCbOff + kReadCb + kReadLen)) f |= kMfAnimRead;
  if (AnimWrites(mat, kCbOff, kCbOff + kCbLen)) f |= kMfAnimCb;
  e.flags = f;
  st.mats++;
  if (f & kMfTextured) st.matsTex++;
  if (f & kMfNoSnap) st.matsNoSnap++;
  if (f & kMfReadChg) {
    st.matsReadChg++;
    if (f & kMfTextured) st.matsTexReadChg++;
  }
  if (f & kMfCbChg) st.matsCbChg++;
  if (f & kMfAnimRead) st.matsAnimRead++;
  if (f & kMfAnimCb) st.matsAnimCb++;
}

// ---- Texture predictability (textured ModelMaterialMT caster) ----
bool TexInsert(void* p) {
  size_t i = PtrHash(p, kTexSet - 1);
  for (size_t k = 0; k < kTexSet; ++k, i = (i + 1) & (kTexSet - 1)) {
    TexSlot& s = g_texSet[i];
    if (s.gen != g_passGen) {
      s.p = p;
      s.gen = g_passGen;
      return true;
    }
    if (s.p == p) return false;
  }
  return false;
}

void CheckTextures(uint8_t* mat, uint8_t* item, SlotStat& st) {
  st.texCasters++;
  uint8_t* sh = *reinterpret_cast<uint8_t**>(mat + 0x30);
  if (!sh || *reinterpret_cast<void**>(sh) != shadowtex::g_shaderVtblPtr) {
    st.texNoShader++;
    return;
  }
  const uint64_t ta = *reinterpret_cast<uint64_t*>(mat + 0x210), tb = *reinterpret_cast<uint64_t*>(mat + 0x218);
  const shadowtex::MaskEntry* m = nullptr;
  if (shadowtex::g_cache)
    m = shadowtex::g_cache->Find(sh, ta, tb, *reinterpret_cast<void**>(sh + 0x50), *reinterpret_cast<void**>(sh + 0xc8),
                                 *reinterpret_cast<void**>(sh + 0xd0), *reinterpret_cast<void**>(sh + 0xb0));
  if (m && m->state <= 0) m = nullptr;
  if (!m) st.texNoMask++;
  auto* props = *reinterpret_cast<uint8_t**>(mat + 0x28);
  auto* arr = *reinterpret_cast<uint8_t***>(item + 0x18);
  if (!props || !arr || !*arr) {
    st.texNoEntries++;
    return;
  }
  const uint32_t n = ngb::TexN(mat);
  if (n > 32) {
    st.texTooMany++;
    return;
  }
  const uint8_t* en = *arr + static_cast<size_t>(*reinterpret_cast<uint32_t*>(props + 0x26c)) * 0x18;
  const uint8_t* recs = *reinterpret_cast<uint8_t**>(sh + 0xc8);
  const uint8_t* recEnd = *reinterpret_cast<uint8_t**>(sh + 0xd0);
  const int64_t nrec = recs && recEnd > recs ? (recEnd - recs) / 0x50 : 0;
  bool ok = true;
  for (uint32_t i = 0; i < n; ++i, en += 0x18) {
    const int64_t h = ngb::Tex(mat)[i];
    if (h == -1) continue;
    if (m && shadowtex::Skippable(*m, h)) {
      st.texMaskSkipped++;
      continue;
    }
    if (h < 0 || h >= nrec) {
      st.texBadHandle++;
      ok = false;
      continue;
    }
    const int32_t type = *reinterpret_cast<const int32_t*>(recs + h * 0x50 + 0xc);
    void* view = nullptr;
    auto* tex = *reinterpret_cast<uint8_t* const*>(en + 8);
    const uint8_t why =
        gbbatch::PredictView(g_env, tex, *reinterpret_cast<const int64_t*>(en), type, ~0ull /* Vec2i(-1,-1) */, &view);
    st.texRead++;
    st.texReason[why < gbbatch::kReasons ? why : 0]++;
    if (why != gbbatch::kOk) ok = false;
    else if (TexInsert(tex)) st.texUnique++;
  }
  if (ok) st.texPredictable++;
}

// Walks the caster vector before the pass. Plain C body (SEH).
bool AnalyseRaw(void* const* b, size_t n, SlotStat& st) {
  __try {
    for (size_t k = 0; k < n; ++k) {
      void* r = b[k];
      uint8_t *mat = nullptr, *item = nullptr;
      int c = r ? ClassOf(r, &mat, &item) : kClsSmrOther;
      if (c == -1) {
        uint8_t f = 0;
        if (MatEntry* e = MatFind(mat)) {
          if (e->passGen != g_passGen) VisitMat(*e, mat, st);
          f = e->flags;
        } else {
          st.matsNoEntry++;
          f = instcount::IsTextured(mat) ? kMfTextured : 0;
        }
        c = (f & kMfTextured) ? kClsModelTex : kClsModel;
        if (f & kMfReadChg) st.castersReadChg++;
        if (f & kMfCbChg) st.castersCbChg++;
        if (f & kMfAnimRead) st.castersAnimRead++;
        if (c == kClsModelTex && g_texOk) CheckTextures(mat, item, st);
      }
      if (k < kMaxCasters) g_cls[k] = static_cast<uint8_t>(c);
      st.cls[c].casters++;
    }
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

// ---- Render entry: material snapshots ----
void SnapshotRaw(void* rg, void* renderables) {
  __try {
    const size_t count = shadowbatch::VectorCountRaw(static_cast<uint8_t*>(rg), renderables);
    if (!count) {
      g_snapNoVector++;
      return;
    }
    for (int k = 0; k < g_learnedCount; ++k) {
      if (g_learned[k].rg != rg || g_learned[k].idx >= count) continue;
      void* const* vec = reinterpret_cast<void* const*>(static_cast<uint8_t*>(renderables) + g_learned[k].idx * 24);
      void* const* b = static_cast<void* const*>(vec[0]);
      void* const* e = static_cast<void* const*>(vec[1]);
      if (!b || e < b || e - b > 200000) continue;
      for (void* const* p = b; p < e; ++p) {
        void* r = *p;
        if (!r || *static_cast<void**>(r) != g_smrVt) continue;
        auto* item = *reinterpret_cast<uint8_t**>(static_cast<uint8_t*>(r) + 0x10);
        auto* mat = item ? *reinterpret_cast<uint8_t**>(item + 0x10) : nullptr;
        if (!mat || *reinterpret_cast<void**>(mat) != g_modelMatVt) continue;
        MatEntry* me = MatFind(mat);
        if (!me || me->snapGen == g_renderGen) continue;
        memcpy(me->cb, mat + kCbOff, kCbLen);
        me->snapGen = g_renderGen;
        g_snapMats++;
      }
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    g_snapFaults++;
  }
}

void OnRender(void* rg, void* renderables) {
  if (!g_on.load(std::memory_order_relaxed) || GetCurrentThreadId() != g_renderTid) return;
  g_inside.fetch_add(1);
  const uint64_t t0 = __rdtsc();
  if (g_matsUsed >= kMatTable * 3 / 4) {
    memset(g_mats, 0, kMatTable * sizeof(MatEntry));
    g_matsUsed = 0;
    g_tableWipes++;
  }
  if (++g_renderGen == 0) g_renderGen = 1;
  g_renderRg = rg;
  SnapshotRaw(rg, renderables);
  g_snapRenders++;
  g_snapCyc += __rdtsc() - t0;
  g_inside.fetch_sub(1);
}

// ---- D3D probes (render thread, inside the loop; their time is subtracted) ----
struct States {
  ID3D11DepthStencilState* dss;
  UINT ref;
  ID3D11BlendState* bs;
  ID3D11RasterizerState* rs;
};
void GetStates(States& s) {
  s = {};
  float f[4];
  UINT mask;
  g_ctx->OMGetDepthStencilState(&s.dss, &s.ref);
  g_ctx->OMGetBlendState(&s.bs, f, &mask);
  g_ctx->RSGetState(&s.rs);
}
void ReleaseStates(States& s) {
  if (s.dss) s.dss->Release();
  if (s.bs) s.bs->Release();
  if (s.rs) s.rs->Release();
  s = {};
}
void RecordCombo(int cls, const States& a, const States& b) {
  const bool changed = a.dss != b.dss || a.bs != b.bs || a.rs != b.rs || a.ref != b.ref;
  for (int i = 0; i < g_comboCount; ++i) {
    Combo& c = g_combo[i];
    if (c.cls == cls && c.dss == b.dss && c.bs == b.bs && c.rs == b.rs && c.ref == b.ref && c.changed == changed) {
      c.count++;
      return;
    }
  }
  if (g_comboCount == kCombos) {
    g_comboOver++;
    return;
  }
  Combo& c = g_combo[g_comboCount++];
  memset(&c, 0, sizeof(c));
  c.cls = cls;
  c.dss = b.dss;
  c.bs = b.bs;
  c.rs = b.rs;
  c.ref = b.ref;
  c.changed = changed;
  c.count = 1;
  if (b.dss && b.bs && b.rs) {
    b.dss->GetDesc(&c.dd);
    b.bs->GetDesc(&c.bd);
    b.rs->GetDesc(&c.rd);
    c.haveDesc = true;
  }
}

void ProbeSetup(SlotStat& st) {
  ID3D11DepthStencilView* dsv = nullptr;
  g_ctx->OMGetRenderTargets(0, nullptr, &dsv);
  ID3D11Resource* res = nullptr;
  D3D11_DEPTH_STENCIL_VIEW_DESC desc;
  memset(&desc, 0, sizeof(desc));
  if (dsv) {
    dsv->GetResource(&res);
    dsv->GetDesc(&desc);
  }
  D3D11_VIEWPORT vps[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
  memset(vps, 0, sizeof(vps));
  UINT nvp = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
  g_ctx->RSGetViewports(&nvp, vps);
  uint32_t flags = 0, dbg = 0;
  bool flagsOk = false;
  if (auto* r = reinterpret_cast<uint8_t*>(shadowbatch::g_rendererObj)) {
    __try {
      flags = *reinterpret_cast<uint32_t*>(r + 0xd4);
      dbg = *reinterpret_cast<uint32_t*>(r + 0x2120);
      flagsOk = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
  }
  st.samples++;
  if (!dsv) st.dsvNull++;
  if (st.have) {
    if (dsv != st.dsv) st.dsvChange++;
    if (res != st.res) st.resChange++;
    if (memcmp(&desc, &st.desc, sizeof(desc)) != 0) st.descChange++;
    if (nvp != st.nvp) st.vpCountChange++;
    if (memcmp(&vps[0], &st.vp, sizeof(D3D11_VIEWPORT)) != 0) st.vpChange++;
    if (flagsOk && st.flagsOk && flags != st.flags) st.flagChange++;
    if (flagsOk && st.flagsOk && dbg != st.dbg) st.dbgChange++;
  }
  st.have = true;
  st.dsv = dsv;
  st.res = res;
  st.desc = desc;
  st.nvp = nvp;
  st.vp = vps[0];
  st.flags = flags;
  st.dbg = dbg;
  st.flagsOk = flagsOk;
  bool seen = false;
  for (int i = 0; i < st.resSeenCount; ++i) seen |= st.resSeen[i] == res;
  if (!seen) {
    if (st.resSeenCount < 4) st.resSeen[st.resSeenCount++] = res;
    else st.resSeenOver = true;
  }
  // Identity only: the pointers are compared, never used after release.
  if (res) res->Release();
  if (dsv) dsv->Release();
}

// ---- Timed caster call ----
int ClassAt(void* self) {
  for (size_t k = t_cursor; k < t_n; ++k)
    if (t_begin[k] == self) {
      t_cursor = k + 1;
      return k < kMaxCasters ? g_cls[k] : -1;
    }
  return -1;
}

template <typename Call>
uint64_t TimedCall(int fallback, void* self, Call&& call) {
  SlotStat& st = g_stat[t_slot];
  const uint64_t t0 = __rdtsc();
  if (!t_first) {
    t_first = t0;
    if (g_ctx) ProbeSetup(st);
  }
  int c = ClassAt(self);
  if (c < 0) {
    g_unplanned++;
    c = fallback;
  }
  const bool probe = g_ctx && !(t_probed & (1u << c));
  States a{}, b{};
  if (probe) GetStates(a);
  const uint64_t t1 = __rdtsc();
  ++t_depth;
  const uint64_t ret = call();
  --t_depth;
  const uint64_t t2 = __rdtsc();
  if (probe) {
    GetStates(b);
    RecordCombo(c, a, b);
    ReleaseStates(a);
    ReleaseStates(b);
    t_probed |= 1u << c;
  }
  st.cls[c].calls++;
  st.cls[c].cyc += t2 - t1;
  const uint64_t t3 = __rdtsc();
  t_probe += (t1 - t0) + (t3 - t2);
  t_last = t3;
  return ret;
}

uint64_t SmrCall(void* self, void* ctx) {
  uint64_t r;
  if (instcount::OverrideFn p = g_prevOverride)
    if (p(self, ctx, &r)) return r;
  return instcount::g_orig(self, ctx);
}

bool Override(void* self, void* ctx, uint64_t* ret) {
  if (!t_inPass || t_depth) {
    if (instcount::OverrideFn p = g_prevOverride) return p(self, ctx, ret);
    return false;
  }
  *ret = TimedCall(kClsSmrOther, self, [&] { return SmrCall(self, ctx); });
  return true;
}

template <int I>
uint64_t __fastcall NsHook(void* a, void* b, void* c, void* d) {
  auto orig = reinterpret_cast<VtFn>(g_nsOrig[I]);
  if (!t_inPass || t_depth) return orig(a, b, c, d);
  return TimedCall(kClsNs0 + I, a, [&] { return orig(a, b, c, d); });
}
static_assert(kMaxNs == 16, "kNsHooks lists one thunk per class");
void* const kNsHooks[kMaxNs] = {
    reinterpret_cast<void*>(&NsHook<0>),  reinterpret_cast<void*>(&NsHook<1>),  reinterpret_cast<void*>(&NsHook<2>),
    reinterpret_cast<void*>(&NsHook<3>),  reinterpret_cast<void*>(&NsHook<4>),  reinterpret_cast<void*>(&NsHook<5>),
    reinterpret_cast<void*>(&NsHook<6>),  reinterpret_cast<void*>(&NsHook<7>),  reinterpret_cast<void*>(&NsHook<8>),
    reinterpret_cast<void*>(&NsHook<9>),  reinterpret_cast<void*>(&NsHook<10>), reinterpret_cast<void*>(&NsHook<11>),
    reinterpret_cast<void*>(&NsHook<12>), reinterpret_cast<void*>(&NsHook<13>), reinterpret_cast<void*>(&NsHook<14>),
    reinterpret_cast<void*>(&NsHook<15>)};

// ---- Cascade execute ----
void __fastcall TimedOrig(void* pass, void* ctx) {
  SlotStat& st = g_stat[t_slot];
  t_first = t_last = t_probe = 0;
  t_probed = 0;
  t_cursor = 0;
  t_inPass = true;
  const uint64_t a = __rdtsc();
  t_origFn(pass, ctx);
  const uint64_t b = __rdtsc();
  t_inPass = false;
  st.origCalls++;
  st.cycOrig += b - a;
  if (t_first) {
    st.cycPre += t_first - a;
    st.cycLoop += t_last - t_first;
    st.cycPost += b - t_last;
    st.cycProbe += t_probe;
  } else {
    st.emptyPasses++;
  }
}

int SlotOf(void* pass, void* ctx, void** vec) {
  __try {
    auto* rg = *static_cast<uint8_t**>(ctx);
    auto* base = *reinterpret_cast<uint8_t**>(rg + 0x438);
    const intptr_t d = reinterpret_cast<uint8_t*>(vec) - base;
    const int32_t cascade = *reinterpret_cast<int32_t*>(static_cast<uint8_t*>(pass) + 0x60);
    if (!base || d < 0 || d % 24 != 0 || d / 24 > 0xffff) return kSlots;
    const uint32_t idx = static_cast<uint32_t>(d / 24);
    int s = -1;
    for (int k = 0; k < g_learnedCount; ++k)
      if (g_learned[k].rg == rg && g_learned[k].idx == idx) s = k;
    if (s < 0) {
      if (g_learnedCount == kSlots) return kSlots;
      s = g_learnedCount++;
      g_learned[s] = {rg, idx};
      g_stat[s].cascade = cascade;
    }
    if (g_stat[s].cascade == kNoCascade) g_stat[s].cascade = cascade;
    else if (g_stat[s].cascade != cascade) g_stat[s].cascadeVaries = true;
    return s;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return kSlots;
  }
}

void Wrap(void* pass, void* ctx, shadowpass::ExecFn orig) {
  const shadowpass::WrapFn prev = g_prevWrap;
  if (!g_on.load(std::memory_order_relaxed) || t_inPass || GetCurrentThreadId() != g_renderTid) {
    if (prev) prev(pass, ctx, orig);
    else orig(pass, ctx);
    return;
  }
  g_inside.fetch_add(1);
  void** vec = shadowpass::CasterVector(pass, ctx);
  const int slot = vec ? SlotOf(pass, ctx, vec) : kSlots;
  SlotStat& st = g_stat[slot];
  const uint64_t t0 = __rdtsc();
  t_begin = nullptr;
  t_n = 0;
  if (vec) {
    auto* b = static_cast<void* const*>(vec[0]);
    auto* e = static_cast<void* const*>(vec[1]);
    if (b && e >= b && e - b <= 200000) {
      if (++g_passGen == 0) g_passGen = 1;
      const size_t n = static_cast<size_t>(e - b);
      if (n > kMaxCasters) st.oversize++;
      if (AnalyseRaw(b, n, st)) {
        t_begin = b;
        t_n = n < kMaxCasters ? n : kMaxCasters;
      } else {
        st.faults++;
      }
    }
  }
  st.cycAnalyse += __rdtsc() - t0;
  t_slot = slot;
  t_origFn = orig;
  const uint64_t a = __rdtsc();
  if (prev) prev(pass, ctx, &TimedOrig);
  else TimedOrig(pass, ctx);
  st.cycWrap += __rdtsc() - a;
  st.passes++;
  t_begin = nullptr;
  t_n = 0;
  g_inside.fetch_sub(1);
}

// ---- Install / teardown ----
void HookClasses() {
  const int n = g_nsCount.load(std::memory_order_acquire);
  for (int i = 0; i < n; ++i) {
    NsClass& c = g_ns[i];
    c.timed = false;
    if (c.state == -2 || c.state == 1) continue;
    if (*c.slot != SlotOriginal(c.slot)) {
      c.state = -1;  // another module's hook: untimed, never touched
      continue;
    }
    g_nsOrig[i] = *c.slot;
    c.state = HookSlot(c.slot, kNsHooks[i], nullptr) ? 1 : -1;
    c.timed = c.state == 1;
  }
}

void UnhookClasses() {
  for (int i = 0; i < kMaxNs; ++i) {
    NsClass& c = g_ns[i];
    if (c.state != 1) continue;
    UnhookSlot(c.slot, g_nsOrig[i]);  // g_nsOrig stays for calls in flight
    c.state = 0;
  }
}

void Unchain() {
  g_on = false;
  if (!g_chained.exchange(false)) return;
  instcount::OverrideFn ov = &Override;
  instcount::g_override.compare_exchange_strong(ov, g_prevOverride);
  shadowpass::WrapFn w = &Wrap;
  shadowpass::g_wrap.compare_exchange_strong(w, g_prevWrap);
  shadowbatch::RenderObserverFn ob = &OnRender;
  shadowbatch::g_renderObserver.compare_exchange_strong(ob, nullptr);
  UnhookClasses();
  if (g_ownRenderHook && shadowbatch::g_state.load() != 1) {
    if (shadowbatch::g_renderSlot) UnhookSlot(shadowbatch::g_renderSlot, reinterpret_cast<void*>(shadowbatch::g_origRender));
    if (shadowbatch::g_renderSlot2) UnhookSlot(shadowbatch::g_renderSlot2, reinterpret_cast<void*>(shadowbatch::g_origRender));
    shadowbatch::g_renderSlot = nullptr;
    shadowbatch::g_renderSlot2 = nullptr;
  }
  g_ownRenderHook = false;
}

// Waits for passes and render entries in flight, then drops the context.
bool Drain() {
  for (int i = 0; i < 400 && g_inside.load() != 0; ++i) Sleep(5);
  if (g_inside.load() != 0) return false;
  if (g_ctx) g_ctx->Release();
  g_ctx = nullptr;
  return true;
}

void Shutdown() {
  if (!g_chained.load()) return;
  Unchain();
  if (!Drain()) Log("shadow rec counter: a pass was still running at unload; its context reference is left");
}

void ResetStats() {
  memset(g_stat, 0, sizeof(g_stat));
  for (SlotStat& st : g_stat) st.cascade = kNoCascade;  // set again at the next pass
  memset(g_combo, 0, sizeof(g_combo));
  g_comboCount = 0;
  g_comboOver = 0;
  g_unplanned = 0;
  g_snapRenders = g_snapCyc = g_snapMats = g_snapFaults = g_snapNoVector = 0;
  g_tableWipes = 0;
}

const char* CmpName(D3D11_COMPARISON_FUNC f) {
  static const char* const k[] = {"?", "NEVER", "LESS", "EQUAL", "LESS_EQUAL", "GREATER", "NOT_EQUAL", "GREATER_EQUAL",
                                  "ALWAYS"};
  return f >= 1 && f <= 8 ? k[f] : k[0];
}

std::string ClassName(int c) {
  switch (c) {
    case kClsModel: return "SMR ModelMaterialMT untextured";
    case kClsModelTex: return "SMR ModelMaterialMT textured";
    case kClsDeck: return "SMR DeckMaterialMT";
    case kClsGlass: return "SMR GlassMaterialMT";
    case kClsSmrOther: return "SMR other/null material";
    case kClsNsOver: return "other renderable (class table full)";
    default: break;
  }
  const int i = c - kClsNs0;
  if (i < 0 || i >= g_nsCount.load()) return "?";
  return std::string(g_ns[i].name) + (g_ns[i].timed ? "" : g_ns[i].state == -1 ? " [untimed: slot hooked elsewhere]"
                                                                                  : " [untimed]");
}

void Report(double f) {
  const double ms = 1000.0 / g_tscHz / f;  // cycles -> ms/frame
  SlotStat tot{};
  for (int s = 0; s <= kSlots; ++s) {
    const SlotStat& a = g_stat[s];
    tot.passes += a.passes;
    tot.emptyPasses += a.emptyPasses;
    tot.origCalls += a.origCalls;
    tot.faults += a.faults;
    tot.oversize += a.oversize;
    tot.cycWrap += a.cycWrap;
    tot.cycOrig += a.cycOrig;
    tot.cycPre += a.cycPre;
    tot.cycLoop += a.cycLoop;
    tot.cycPost += a.cycPost;
    tot.cycProbe += a.cycProbe;
    tot.cycAnalyse += a.cycAnalyse;
    for (int c = 0; c < kClasses; ++c) {
      tot.cls[c].casters += a.cls[c].casters;
      tot.cls[c].calls += a.cls[c].calls;
      tot.cls[c].cyc += a.cls[c].cyc;
    }
    tot.mats += a.mats;
    tot.matsNoSnap += a.matsNoSnap;
    tot.matsReadChg += a.matsReadChg;
    tot.matsCbChg += a.matsCbChg;
    tot.matsTex += a.matsTex;
    tot.matsTexReadChg += a.matsTexReadChg;
    tot.matsAnimRead += a.matsAnimRead;
    tot.matsAnimCb += a.matsAnimCb;
    tot.matsNoEntry += a.matsNoEntry;
    tot.castersReadChg += a.castersReadChg;
    tot.castersCbChg += a.castersCbChg;
    tot.castersAnimRead += a.castersAnimRead;
    tot.texCasters += a.texCasters;
    tot.texPredictable += a.texPredictable;
    tot.texNoShader += a.texNoShader;
    tot.texNoEntries += a.texNoEntries;
    tot.texTooMany += a.texTooMany;
    tot.texNoMask += a.texNoMask;
    tot.texRead += a.texRead;
    tot.texMaskSkipped += a.texMaskSkipped;
    tot.texUnique += a.texUnique;
    tot.texBadHandle += a.texBadHandle;
    for (int r = 0; r < gbbatch::kReasons; ++r) tot.texReason[r] += a.texReason[r];
  }
  auto pct = [](double a, double b) { return b > 0 ? 100.0 * a / b : 0.0; };
  uint64_t casters = 0, timedCyc = 0;
  for (int c = 0; c < kClasses; ++c) {
    casters += tot.cls[c].casters;
    timedCyc += tot.cls[c].cyc;
  }
  const double loopNet = static_cast<double>(tot.cycLoop) - static_cast<double>(tot.cycProbe);
  Log("  shadow rec S0: %.0f frames; %d cascades learned; %.2f passes/frame (%.2f without casters, %.2f DCS executes), "
      "%.0f casters/frame; %llu analysis faults, %llu oversized vectors, %llu caster calls outside the walked vector",
      f, g_learnedCount, tot.passes / f, tot.emptyPasses / f, tot.origCalls / f, casters / f,
      static_cast<unsigned long long>(tot.faults), static_cast<unsigned long long>(tot.oversize),
      static_cast<unsigned long long>(g_unplanned));
  Log("  shadow rec S0 time (render thread, ms/frame): cascade wrap %.3f = batching planner/restore %.3f + DCS execute "
      "%.3f; execute = pre-loop (frame buffer, clear, binder) %.3f + loop %.3f + post-loop %.3f; loop net of our "
      "probes %.3f (probes %.3f); our caster walk before each pass %.3f (not in the above)",
      tot.cycWrap * ms, (static_cast<double>(tot.cycWrap) - tot.cycOrig) * ms, tot.cycOrig * ms, tot.cycPre * ms,
      tot.cycLoop * ms, tot.cycPost * ms, loopNet * ms, tot.cycProbe * ms, tot.cycAnalyse * ms);
  Log("  shadow rec S0 classes (casters/frame, timed calls/frame, ms/frame, ns/call, share of net loop):");
  for (int c = 0; c < kClasses; ++c) {
    const ClassStat& k = tot.cls[c];
    if (!k.casters && !k.calls) continue;
    Log("    %-70.70s %8.1f %8.1f %7.3f %7.0f %5.1f%%", ClassName(c).c_str(), k.casters / f, k.calls / f, k.cyc * ms,
        k.calls ? k.cyc * 1e9 / g_tscHz / k.calls : 0.0, pct(static_cast<double>(k.cyc), loopNet));
  }
  const double modelCyc = static_cast<double>(tot.cls[kClsModel].cyc + tot.cls[kClsModelTex].cyc);
  const double share = pct(modelCyc, loopNet);
  Log("  shadow rec S0 gate (R17: ModelMaterialMT casters >= 70%% of loop time): model %.3f ms of net loop %.3f ms = "
      "%.1f%% -> %s; model share of timed caster time %.1f%%; non-model timed %.3f ms, untimed loop rest %.3f ms",
      modelCyc * ms, loopNet * ms, share, share >= 70.0 ? "PASS" : "FAIL", pct(modelCyc, static_cast<double>(timedCyc)),
      (static_cast<double>(timedCyc) - modelCyc) * ms, (loopNet - static_cast<double>(timedCyc)) * ms);
  for (int s = 0; s <= kSlots; ++s) {
    const SlotStat& a = g_stat[s];
    if (!a.passes) continue;
    uint64_t n = 0, mc = 0, oc = 0;
    for (int c = 0; c < kClasses; ++c) {
      n += a.cls[c].casters;
      (c == kClsModel || c == kClsModelTex ? mc : oc) += a.cls[c].cyc;
    }
    if (s == kSlots)
      Log("  cascade (not learnable): %.2f passes/frame, %.0f casters/frame", a.passes / f, n / f);
    else
      Log("  cascade slot %d (index %d%s, graph %p, collection %u): %.2f passes/frame, casters/frame %.0f (model "
          "untextured %.0f, textured %.0f, deck %.0f, other %.0f); ms/frame: pre-loop %.3f, loop %.3f (net %.3f: model "
          "%.3f, others %.3f), post %.3f",
          s, a.cascade, a.cascadeVaries ? ", varies" : "", g_learned[s].rg, g_learned[s].idx, a.passes / f, n / f,
          a.cls[kClsModel].casters / f, a.cls[kClsModelTex].casters / f, a.cls[kClsDeck].casters / f,
          (n - a.cls[kClsModel].casters - a.cls[kClsModelTex].casters - a.cls[kClsDeck].casters) / f, a.cycPre * ms,
          a.cycLoop * ms, (static_cast<double>(a.cycLoop) - a.cycProbe) * ms, mc * ms, oc * ms, a.cycPost * ms);
    if (a.samples)
      Log("    setup over %llu passes: DSV null %llu, DSV object changed %llu, resource changed %llu (%d distinct%s), "
          "DSV desc changed %llu, viewport changed %llu (count changed %llu), renderer+0xd4 flags changed %llu, "
          "+0x2120 changed %llu; last: format %d dim %d slice %u+%u mip %u, viewport %u x %.0fx%.0f at %.0f,%.0f "
          "depth %.2f-%.2f, flags 0x%x, +0x2120 0x%x%s",
          static_cast<unsigned long long>(a.samples), static_cast<unsigned long long>(a.dsvNull),
          static_cast<unsigned long long>(a.dsvChange), static_cast<unsigned long long>(a.resChange),
          a.resSeenCount, a.resSeenOver ? "+" : "", static_cast<unsigned long long>(a.descChange),
          static_cast<unsigned long long>(a.vpChange), static_cast<unsigned long long>(a.vpCountChange),
          static_cast<unsigned long long>(a.flagChange), static_cast<unsigned long long>(a.dbgChange),
          static_cast<int>(a.desc.Format), static_cast<int>(a.desc.ViewDimension), a.desc.Texture2DArray.FirstArraySlice,
          a.desc.Texture2DArray.ArraySize, a.desc.Texture2DArray.MipSlice, a.nvp, a.vp.Width, a.vp.Height,
          a.vp.TopLeftX, a.vp.TopLeftY, a.vp.MinDepth, a.vp.MaxDepth, a.flags, a.dbg,
          a.flagsOk ? "" : " (renderer not readable)");
  }
  Log("  shadow rec S0 depth/blend/raster states per class (first caster of each class per pass):");
  for (int i = 0; i < g_comboCount; ++i) {
    const Combo& c = g_combo[i];
    if (!c.haveDesc) {
      Log("    %-50.50s %llu x: state object missing (dss %p bs %p rs %p)", ClassName(c.cls).c_str(),
          static_cast<unsigned long long>(c.count), c.dss, c.bs, c.rs);
      continue;
    }
    bool blend = false;
    for (int t = 0; t < 8; ++t) blend |= c.bd.RenderTarget[t].BlendEnable != FALSE;
    const bool maxTest = c.dd.DepthEnable && (c.dd.DepthFunc == D3D11_COMPARISON_GREATER ||
                                              c.dd.DepthFunc == D3D11_COMPARISON_GREATER_EQUAL);
    Log("    %-50.50s %llu x, %s: depth %s %s write %s, stencil %d ref %u; blend %s a2c %d; cull %d fill %d bias %d "
        "slope %.2f clamp %.2f clip %d -> %s",
        ClassName(c.cls).c_str(), static_cast<unsigned long long>(c.count),
        c.changed ? "set by the caster" : "unchanged by the caster", c.dd.DepthEnable ? "on" : "off",
        CmpName(c.dd.DepthFunc), c.dd.DepthWriteMask ? "all" : "zero", static_cast<int>(c.dd.StencilEnable), c.ref,
        blend ? "ON" : "off", static_cast<int>(c.bd.AlphaToCoverageEnable), static_cast<int>(c.rd.CullMode),
        static_cast<int>(c.rd.FillMode), c.rd.DepthBias, c.rd.SlopeScaledDepthBias, c.rd.DepthBiasClamp,
        static_cast<int>(c.rd.DepthClipEnable), !blend && maxTest ? "order-free" : "NOT order-free");
  }
  if (g_comboOver) Log("    (%llu more combinations not kept)", static_cast<unsigned long long>(g_comboOver));
  for (int i = 0; i < g_matClsCount; ++i)
    Log("  shadow rec S0 SMR material class %s -> %s", g_matCls[i].name, ClassName(g_matCls[i].cls).c_str());
  Log("  shadow rec S0 materials (ModelMaterialMT, once per material per pass; per frame): %.0f checked (%.0f textured), "
      "%.0f without a render-entry copy, %.0f not in the table; changed since RenderGraph::render entry: diffuseShift "
      "register (CB +0xc0..+0xcf) %.1f (%.2f%%; textured %.1f), whole CB except posStructOffset %.1f (%.2f%%); casters "
      "with a changed read register %.1f, changed CB %.1f",
      tot.mats / f, tot.matsTex / f, tot.matsNoSnap / f, tot.matsNoEntry / f, tot.matsReadChg / f,
      pct(static_cast<double>(tot.matsReadChg), static_cast<double>(tot.mats - tot.matsNoSnap)),
      tot.matsTexReadChg / f, tot.matsCbChg / f,
      pct(static_cast<double>(tot.matsCbChg), static_cast<double>(tot.mats - tot.matsNoSnap)),
      tot.castersReadChg / f, tot.castersCbChg / f);
  Log("  shadow rec S0 materials, static: animated property list writes the read register %.1f/frame (%.2f%%, casters "
      "%.1f/frame), writes into the CB at all %.1f/frame; render-entry copies %.1f renders/frame, %.0f materials/frame, "
      "%.3f ms/frame, %llu faults, %llu entries without the vector, %llu table wipes",
      tot.matsAnimRead / f, pct(static_cast<double>(tot.matsAnimRead), static_cast<double>(tot.mats)),
      tot.castersAnimRead / f, tot.matsAnimCb / f, g_snapRenders / f, g_snapMats / f, g_snapCyc * ms,
      static_cast<unsigned long long>(g_snapFaults), static_cast<unsigned long long>(g_snapNoVector),
      static_cast<unsigned long long>(g_tableWipes));
  if (!g_texOk) {
    Log("  shadow rec S0 textures: not checked (shadow texture skip not installed: its build check provides the "
        "texture classes)");
  } else {
    Log("  shadow rec S0 textures (textured ModelMaterialMT casters, per frame): %.0f casters, %.0f predictable "
        "(%.1f%%); %.0f without a DX11Shader, %.0f without entries, %.0f with > 32 sets, %.0f bad handles, %.0f "
        "without a cached read mask (every set counted); sets read %.0f, skipped by the mask %.0f; unique textures "
        "%.0f/frame (the vt[23] replays per frame S4 would add)",
        tot.texCasters / f, tot.texPredictable / f,
        pct(static_cast<double>(tot.texPredictable), static_cast<double>(tot.texCasters)), tot.texNoShader / f,
        tot.texNoEntries / f, tot.texTooMany / f, tot.texBadHandle / f, tot.texNoMask / f, tot.texRead / f,
        tot.texMaskSkipped / f, tot.texUnique / f);
    std::string line;
    char buf[96];
    for (int r = 0; r < gbbatch::kReasons; ++r) {
      if (!tot.texReason[r]) continue;
      snprintf(buf, sizeof(buf), "%s%s %.1f", line.empty() ? "" : ", ", r == gbbatch::kOk ? "predicted" : gbbatch::kReasonName[r],
               tot.texReason[r] / f);
      line += buf;
    }
    Log("  shadow rec S0 texture sets read, by prediction result (per frame): %s", line.empty() ? "none" : line.c_str());
  }
}

void LogDriver(ID3D11Device* dev) {
  D3D11_FEATURE_DATA_THREADING th{};
  const bool thOk = SUCCEEDED(dev->CheckFeatureSupport(D3D11_FEATURE_THREADING, &th, sizeof(th)));
  D3D11_FEATURE_DATA_D3D11_OPTIONS o{};
  const bool oOk = SUCCEEDED(dev->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS, &o, sizeof(o)));
  bool dev1 = false, ctx1 = false;
  ID3D11Device1* d1 = nullptr;
  if (SUCCEEDED(dev->QueryInterface(__uuidof(ID3D11Device1), reinterpret_cast<void**>(&d1))) && d1) {
    dev1 = true;
    d1->Release();
  }
  if (g_ctx) {
    ID3D11DeviceContext1* c1 = nullptr;
    if (SUCCEEDED(g_ctx->QueryInterface(__uuidof(ID3D11DeviceContext1), reinterpret_cast<void**>(&c1))) && c1) {
      ctx1 = true;
      c1->Release();
    }
  }
  const UINT flags = dev->GetCreationFlags();
  Log("  shadow rec S0 driver: THREADING %s (DriverCommandLists %d, DriverConcurrentCreates %d); D3D11_OPTIONS %s "
      "(ConstantBufferOffsetting %d, MapNoOverwriteOnDynamicConstantBuffer %d); ID3D11Device1 %d, "
      "ID3D11DeviceContext1 %d; feature level 0x%x; creation flags 0x%x (SINGLETHREADED %d) -> R17 requirements %s",
      thOk ? "ok" : "FAILED", static_cast<int>(th.DriverCommandLists), static_cast<int>(th.DriverConcurrentCreates),
      oOk ? "ok" : "FAILED", static_cast<int>(o.ConstantBufferOffsetting),
      static_cast<int>(o.MapNoOverwriteOnDynamicConstantBuffer), dev1, ctx1, static_cast<unsigned>(dev->GetFeatureLevel()),
      flags, (flags & D3D11_CREATE_DEVICE_SINGLETHREADED) ? 1 : 0,
      thOk && oOk && th.DriverCommandLists && o.ConstantBufferOffsetting && ctx1 &&
              !(flags & D3D11_CREATE_DEVICE_SINGLETHREADED)
          ? "MET"
          : "NOT met");
}

void Measure(int ms, std::atomic<uint64_t>& frames, double tscHz) {
  if (g_chained.load()) {
    Log("  shadow rec counter: already running");
    return;
  }
  g_tscHz = tscHz;
  shadowpass::Install();
  if (!shadowpass::g_orig || !tscHz) {
    Log("  shadow rec counter: cascade execute not hooked (GraphicsCore build) or no TSC rate; skipped");
    return;
  }
  auto* ng = reinterpret_cast<uint8_t*>(GetModuleHandleW(L"NGModel.dll"));
  const uint32_t smrRva = reloc::Rva(hooksig::NG_ShadowMapRenderable_vtbl, instcount::kVtableRva);
  const uint32_t matRva = reloc::Rva(hooksig::NG_ModelMaterialMT_vtbl, 0x592f0);
  if (!ng || !smrRva || !matRva) {
    Log("  shadow rec counter: NGModel.dll not loaded or its classes not found; skipped");
    return;
  }
  g_smrVt = ng + smrRva;
  g_modelMatVt = ng + matRva;
  g_md = reinterpret_cast<uint8_t*>(GetModuleHandleW(L"ModelDesc.dll"));
  // Pass-through hook on ShadowMapRenderable vt[1] (shadow batching's too).
  const bool smrTimed = instcount::Install();
  if (!smrTimed) Log("  shadow rec counter: ShadowMapRenderable hook unavailable: model casters counted, not timed");
  if (!instcount::g_getTex || !instcount::g_valid)
    Log("  shadow rec counter: ModelDesc texture getters not found: every ModelMaterialMT caster counts as untextured");
  if (!shadowbatch::InstallRenderer()) Log("  shadow rec counter: DX11Renderer not matched: +0xd4/+0x2120 not read");
  if (ID3D11Device* dev = shadowinst::g_device) {
    dev->GetImmediateContext(&g_ctx);
    LogDriver(dev);
  } else {
    Log("  shadow rec counter: no D3D11 device yet (needs [Model] ShadowInstancing=1): no driver query, setup and state "
        "probes");
  }
  g_texOk = shadowtex::g_state.load() == 1 && shadowtex::g_texVtblPtr;
  if (g_texOk) {
    g_env.texVtbl = shadowtex::g_texVtblPtr;
    g_env.inner[0] = shadowtex::g_innerFile;
    g_env.inner[1] = shadowtex::g_innerArray;
    g_env.inner[2] = shadowtex::g_innerDummy;
    g_env.getDesc = shadowtex::g_getDesc;
    g_env.compat = shadowtex::g_compat;
  }
  if (!g_mats) {
    g_mats = static_cast<MatEntry*>(VirtualAlloc(nullptr, kMatTable * sizeof(MatEntry), MEM_COMMIT | MEM_RESERVE,
                                                 PAGE_READWRITE));
    if (!g_mats) Log("  shadow rec counter: no memory for the material table: material checks off");
  } else {
    memset(g_mats, 0, kMatTable * sizeof(MatEntry));
  }
  g_matsUsed = 0;
  g_renderTid = 0;
  // The render thread: the one shadow batching saw running cascade passes,
  // else the one running top-level render passes (pass_timing.h).
  g_renderTid = shadowbatch::g_renderThread;
  if (!g_renderTid) g_renderTid = ptiming::g_topTid.load();
  if (!g_renderTid) {
    Log("  shadow rec counter: render thread not known yet (pass timing has not run); skipped");
    if (g_ctx) g_ctx->Release();
    g_ctx = nullptr;
    return;
  }
  // RenderGraph::render entry: batching's import hook, ours for the phase if absent.
  g_ownRenderHook = false;
  if (!shadowbatch::g_renderSlot && !shadowbatch::g_renderSlot2) g_ownRenderHook = shadowbatch::InstallRenderHook();
  const bool haveEntry = shadowbatch::g_renderSlot || shadowbatch::g_renderSlot2;
  if (!haveEntry) Log("  shadow rec counter: RenderGraph::render entry not hooked: no material copies (all 'without copy')");
  g_learnedCount = 0;
  memset(g_learned, 0, sizeof(g_learned));
  ResetStats();
  g_prevWrap = shadowpass::g_wrap.load();
  g_prevOverride = instcount::g_override.load();
  shadowbatch::g_renderObserver = &OnRender;
  shadowpass::g_wrap = &Wrap;
  if (smrTimed) instcount::g_override = &Override;
  g_chained = true;
  // Discovery: learn the cascades and the caster classes.
  g_on = true;
  Sleep(1000);
  g_on = false;
  Sleep(100);
  for (int i = 0; i < 400 && g_inside.load() != 0; ++i) Sleep(5);  // a pass may still be counting
  HookClasses();
  ResetStats();
  const uint64_t f0 = frames.load();
  g_on = true;
  Sleep(ms);
  g_on = false;
  const double f = static_cast<double>(frames.load() - f0);
  Sleep(100);
  Unchain();
  const bool drained = Drain();
  if (!drained) Log("  shadow rec counter: a pass was still running; context reference left");
  int foreign = 0;
  for (int i = 0; i < g_nsCount.load(); ++i) foreign += g_ns[i].state == -1;
  if (f <= 0 || !drained) {
    Log("  shadow rec counter: no frames counted");
    return;
  }
  Report(f);
  if (foreign) Log("  shadow rec S0: %d renderable classes untimed (vt[1] hooked by another module)", foreign);
}

}  // namespace shadowrec
