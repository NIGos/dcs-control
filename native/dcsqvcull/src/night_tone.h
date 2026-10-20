// R37 night tone: opt-in, adjustable darkening of DCS's night image through
// DCS's own tone mapper parameters ([Night] NightTone, default 0 = nothing
// installed, nothing changed).
//
// Why (docs\research\R37_night_rendering.md): DCS's tone mapper
// (Bazar\shaders\deferred\toneMap.hlsl, Tonemap.fx; GraphicsCore
// ToneMapper) exposes every frame with
//   exposure = 0.18 / clamp(avgLum, sceneLuminanceMin, sceneLuminanceMax) * 2^EC
// where avgLum is the adapted scene luminance. The stock floor is
// sceneLuminanceMin = 0.01 (ToneMapperUniforms constructor, GraphicsCore
// 0x6f77b), EC = 0 - 0.2 in the render-graph path. Any night scene whose
// average stays above 0.01 is therefore brought to the same mid-grey as a day
// scene: the grey night. Only the floor, the shadow toe of the filmic curve and
// the EC bias decide how dark night is allowed to look.
//
// What this does, on the live ToneMappingModule objects (their ToneMapper at
// +0xe18; its uniform block at +8 is copied into cbTonemapParams every frame
// by updateRuntimeParameters, so a changed field takes effect next frame):
//   NightExposureFloor  sceneLuminanceMin raised (only raised) toward the value:
//                       a scene darker than the floor keeps its darkness
//                       (brightness proportional to avgLum / floor);
//   NightToe            the filmic toe (stock 0.615): higher = darker shadows,
//                       mid-grey and highlights nearly unchanged, black stays 0;
//   NightExposureEV     added to GraphicsCore's exposure bias global (stock
//                       -0.2 EV, the value updateRuntimeParameters adds).
// NightGate 1 (default) scales all three by a night weight from the sun's
// elevation (sun direction y of GraphicsCore's current context: 0 at +0.05,
// 1 at -0.10 and below, linear between), so daylight stays stock. 0 = full
// strength always (tests in a day mission).
// NightTone 2 = observe only: installed, logs the stock values and the sun
// weight, writes nothing.
//
// Hook: SceneRenderer's import of ToneMappingModule::addToneMappingPass (one
// IAT slot through the loader's registry); the hook notes the module (this)
// and passes through with the same seven 64-bit arguments. Before anything is
// written the build is checked: the ToneMappingModule constructor builds its
// ToneMapper at +0xe18, updateRuntimeParameters adds a rip-relative float
// (the bias global, in the image), getSunMoonDir reads the sun at context
// +0x50, and each module's uniform block holds DCS's constant defaults
// (focus 1 / 0.4, histogram percent 0.3 / 0.85, LUT log range -2.5 / 1.5).
// Any mismatch: nothing written, one log line.
// Off again (NightTone 0, payload stop): every written field and the bias back
// to the values first seen.
// R62: a module is written only from the hook, for the live `this` DCS passes
// (the worker used to write through every pointer ever noted, once a second,
// with no liveness check: DCS frees and moves ToneMappingModules with its
// viewports, so those were writes into freed memory). The worker only computes
// each module's target; the hook writes it, or DCS's values back once applying
// stops. At payload stop (the hook about to go) only modules the hook saw in
// the last kLiveMs are restored directly, under SEH.
// Included once from main.cpp inside its anonymous namespace, after
// shader_fast.h (uses Log, HookSlot, UnhookSlot, timercache::FindImport,
// sigscan).
#pragma once

namespace nighttone {

// GraphicsCore exports (2.9.30.28738; checked by bytes before use).
constexpr const char kAddToneMappingPass[] =
    "?addToneMappingPass@ToneMappingModule@render@@QEAAXAEAVRenderGraph@2@AEBUParams@12@VRenderGraphTexture@2@2PEBU"
    "SceneRenderContext@2@PEBUISceneRenderContextBinder@2@@Z";
constexpr const char kModuleCtor[] = "??0ToneMappingModule@render@@QEAA@XZ";
constexpr const char kUpdateRuntime[] =
    "?updateRuntimeParameters@ToneMapper@deferredShading@@QEAAXAEBVVec2i@osg@@AEBVVec4f@4@MPEBUToneMapperParams@2@@Z";
constexpr const char kGetCurrentContext[] = "?getCurrentContext@ContextBase@GraphicsCore@@SAPEAV12@XZ";
constexpr const char kGetSunMoonDir[] = "?getSunMoonDir@ContextBase@GraphicsCore@@QEBA?AVVec3f@osg@@XZ";

// Layout (cbTonemapParams order, tonemapCommon.hlsl).
constexpr size_t kModuleToneMapper = 0xe18;  // ToneMappingModule::ToneMapper
constexpr size_t kUniforms = 8;              // ToneMapper::ToneMapperUniforms (0x100 bytes)
constexpr size_t kSceneLumMin = 0x40, kSceneLumMax = 0x44, kFocusWidth = 0x48, kFocusSigma = 0x4c;
constexpr size_t kPercentMin = 0x60, kPercentMax = 0x64, kSlope = 0x70, kToe = 0x74;
constexpr size_t kLutLogMin = 0x84, kLutLogMax = 0x88;
constexpr size_t kSunDir = 0x50;  // ContextBase: sun direction (getSunMoonDir)
constexpr double kDayY = 0.05, kNightY = -0.10;  // sun direction y: weight 0 at and above, 1 at and below

struct Settings {
  int mode = 0;          // [Night] NightTone: 0 off, 1 apply, 2 observe only
  double floor = 0.0;    // [Night] NightExposureFloor (0 = DCS's floor kept)
  double toe = 0.0;      // [Night] NightToe (0 = DCS's toe kept)
  double ev = 0.0;       // [Night] NightExposureEV
  bool gate = true;      // [Night] NightGate
  bool operator==(const Settings& o) const {
    return mode == o.mode && floor == o.floor && toe == o.toe && ev == o.ev && gate == o.gate;
  }
};

// Ini values clamped to their useful range.
inline Settings Clamped(Settings s) {
  s.mode = s.mode < 0 || s.mode > 2 ? 0 : s.mode;
  s.floor = s.floor <= 0 ? 0.0 : (std::min)(1.0, (std::max)(0.01, s.floor));
  s.toe = s.toe <= 0 ? 0.0 : (std::min)(0.95, (std::max)(0.3, s.toe));
  s.ev = (std::min)(1.0, (std::max)(-4.0, s.ev));
  return s;
}

// Night weight from the sun direction's y.
inline double NightWeight(double sunY) {
  if (sunY >= kDayY) return 0.0;
  if (sunY <= kNightY) return 1.0;
  return (kDayY - sunY) / (kDayY - kNightY);
}

struct Stock {
  float lumMin = 0.01f, toe = 0.615f, bias = -0.2f;
};
struct Target {
  float lumMin, toe, bias;
};

// The values to write for weight w (0..1, already gated): the floor is only
// raised and blended in log space, the toe and the bias linearly.
inline Target Compute(const Settings& s, const Stock& st, double w) {
  Target t{st.lumMin, st.toe, st.bias};
  w = (std::min)(1.0, (std::max)(0.0, w));
  if (s.floor > 0 && st.lumMin > 0 && s.floor > st.lumMin)
    t.lumMin = static_cast<float>(std::exp(std::log(st.lumMin) + (std::log(s.floor) - std::log(st.lumMin)) * w));
  if (s.toe > 0) t.toe = static_cast<float>(st.toe + (s.toe - st.toe) * w);
  t.bias = static_cast<float>(st.bias + s.ev * w);
  return t;
}

inline float F(const uint8_t* u, size_t off) {
  float v;
  memcpy(&v, u + off, 4);
  return v;
}
inline bool Near(float a, float b) { return std::fabs(a - b) <= 1e-4f; }

// A uniform block that holds DCS's constant defaults (fields nothing else
// writes) and sane adjustable fields.
inline bool UniformsLookStock(const uint8_t* u) {
  const float mn = F(u, kSceneLumMin), mx = F(u, kSceneLumMax), toe = F(u, kToe), slope = F(u, kSlope);
  return Near(F(u, kFocusWidth), 1.0f) && Near(F(u, kFocusSigma), 0.4f) && Near(F(u, kPercentMin), 0.3f) &&
         Near(F(u, kPercentMax), 0.85f) && Near(F(u, kLutLogMin), -2.5f) && Near(F(u, kLutLogMax), 1.5f) &&
         mn > 0 && mn < 1 && mx > mn && toe > 0.2f && toe < 1.0f && slope > 0.3f && slope < 2.0f;
}

// Byte checks on the GraphicsCore image (also run by the tests on the DCS
// install's file). Each returns false when the build differs.
inline bool CtorHasToneMapperAt(const uint8_t* ctor) {  // lea rcx,[rbx+0xe18]; call
  static const uint8_t k[8] = {0x48, 0x8D, 0x8B, 0x18, 0x0E, 0x00, 0x00, 0xE8};
  for (size_t i = 0; i + 8 <= 0x100; ++i)
    if (memcmp(ctor + i, k, 8) == 0) return true;
  return false;
}
// updateRuntimeParameters: the first `addss xmm3, [rip+d]` (the bias). Returns its target or nullptr.
inline const uint8_t* BiasTarget(const uint8_t* fn) {
  for (size_t i = 0; i + 8 <= 0x40; ++i)
    if (fn[i] == 0xF3 && fn[i + 1] == 0x0F && fn[i + 2] == 0x58 && fn[i + 3] == 0x1D) {
      int32_t d;
      memcpy(&d, fn + i + 4, 4);
      return fn + i + 8 + d;
    }
  return nullptr;
}
inline bool SunAt0x50(const uint8_t* fn) {  // getSunMoonDir: lea rdx,[rcx+0x50] and mulss xmm3,[rcx+0x54]
  static const uint8_t lea[4] = {0x48, 0x8D, 0x51, 0x50}, mul[5] = {0xF3, 0x0F, 0x59, 0x59, 0x54};
  bool a = false, b = false;
  for (size_t i = 0; i + 5 <= 0x40; ++i) {
    a = a || memcmp(fn + i, lea, 4) == 0;
    b = b || memcmp(fn + i, mul, 5) == 0;
  }
  return a && b;
}

using AddPassFn = void(__fastcall*)(uint8_t*, void*, void*, void*, void*, void*, void*);
using CtxFn = uint8_t*(__cdecl*)();

constexpr int kMaxModules = 8;
constexpr ULONGLONG kLiveMs = 1500;  // R62: a module the hook saw this recently is taken as still alive
struct Module {
  std::atomic<uint8_t*> obj{nullptr};
  Stock stock;   // the values first seen
  bool ok = false;     // its uniform block matched (written to only then)
  std::atomic<bool> written{false};
  std::atomic<ULONGLONG> seen{0};             // R62: GetTickCount64 of the hook's last call for it
  std::atomic<float> tMin{0}, tToe{0};        // R62: its own target (the worker computes, the hook writes)
  std::atomic<bool> haveTarget{false};
};

std::atomic<int> g_state{0};  // 0 not installed, 1 installed, -1 build mismatch (never retried)
AddPassFn g_orig = nullptr;
void** g_slot = nullptr;
float* g_bias = nullptr;
float g_biasStock = -0.2f;
bool g_biasWritten = false;
CtxFn g_ctx = nullptr;
Module g_mod[kMaxModules];
std::mutex g_modMutex;
std::atomic<int> g_inflight{0};
std::atomic<uint64_t> g_calls{0};
std::atomic<int> g_apply{0};  // 1: the hook writes g_target into new / known modules
std::atomic<float> g_tMin{0}, g_tToe{0};
std::atomic<bool> g_fullLogged{false};  // R62: "more than kMaxModules modules" logged once
std::atomic<float> g_sunY{0};
std::atomic<bool> g_sunValid{false};
Settings g_settings;  // worker thread

// The sun direction's y from GraphicsCore's current context (unit-length check).
bool ReadSunY(float* y) {
  if (!g_ctx) return false;
  __try {
    const uint8_t* c = g_ctx();
    if (!c) return false;
    float v[3];
    memcpy(v, c + kSunDir, 12);
    const float len = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (!(len > 0.98f && len < 1.02f)) return false;
    *y = v[1];
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

// R42 colour modes: the sun direction without installing anything (GraphicsCore's getCurrentContext, checked as
// Install checks it). False until GraphicsCore is loaded or when the build differs.
bool SunY(float* y) {
  static int state = 0;  // 0 not tried, 1 ready, -1 build mismatch
  if (!g_ctx && state == 0) {
    HMODULE gc = GetModuleHandleW(L"GraphicsCore.dll");
    if (!gc) return false;
    auto* sun = reinterpret_cast<const uint8_t*>(GetProcAddress(gc, kGetSunMoonDir));
    auto ctx = reinterpret_cast<CtxFn>(GetProcAddress(gc, kGetCurrentContext));
    state = sun && ctx && SunAt0x50(sun) ? 1 : -1;
    if (state > 0) g_ctx = ctx;
  }
  return g_ctx && ReadSunY(y);
}

bool ReadUniforms(const uint8_t* module, uint8_t out[0x100]) {
  __try {
    memcpy(out, module + kModuleToneMapper + kUniforms, 0x100);
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

// R62: into the module passed (the hook's live `this`), not the stored pointer.
void WriteFields(uint8_t* obj, float lumMin, float toe) {
  uint8_t* u = obj + kModuleToneMapper + kUniforms;
  memcpy(u + kSceneLumMin, &lumMin, 4);
  memcpy(u + kToe, &toe, 4);
}
bool WriteFieldsGuarded(uint8_t* obj, float lumMin, float toe) {
  __try {
    WriteFields(obj, lumMin, toe);
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

// A module seen for the first time: its stock values and the layout check.
Module* Note(uint8_t* self) {
  for (Module& m : g_mod)
    if (m.obj.load(std::memory_order_acquire) == self) return &m;
  std::lock_guard<std::mutex> lock(g_modMutex);
  for (Module& m : g_mod)
    if (m.obj.load() == self) return &m;
  for (Module& m : g_mod) {
    if (m.obj.load()) continue;
    uint8_t u[0x100];
    m.ok = ReadUniforms(self, u) && UniformsLookStock(u);
    if (m.ok) {
      m.stock.lumMin = F(u, kSceneLumMin);
      m.stock.toe = F(u, kToe);
    }
    m.stock.bias = g_biasStock;
    m.haveTarget.store(false);
    m.written.store(false);
    m.obj.store(self, std::memory_order_release);
    Log("night tone: tone mapping module %p %s (floor %.4f, toe %.3f)", self,
        m.ok ? "found" : "found, uniform block not DCS's layout: left alone", m.stock.lumMin, m.stock.toe);
    return &m;
  }
  if (!g_fullLogged.exchange(true))  // R62: was silent
    Log("night tone: more than %d tone mapping modules; module %p and later ones left alone", kMaxModules, self);
  return nullptr;
}

void __fastcall Hook(uint8_t* self, void* a2, void* a3, void* a4, void* a5, void* a6, void* a7) {
  g_inflight.fetch_add(1, std::memory_order_acq_rel);
  g_calls.fetch_add(1, std::memory_order_relaxed);
  if (self) {
    Module* m = Note(self);
    float y;
    if (ReadSunY(&y)) {
      g_sunY.store(y, std::memory_order_relaxed);
      g_sunValid.store(true, std::memory_order_relaxed);
    }
    if (m) m->seen.store(GetTickCount64(), std::memory_order_relaxed);
    if (m && m->ok && g_apply.load(std::memory_order_acquire)) {
      const bool own = m->haveTarget.load(std::memory_order_acquire);
      WriteFields(self, own ? m->tMin.load(std::memory_order_relaxed) : g_tMin.load(std::memory_order_relaxed),
                  own ? m->tToe.load(std::memory_order_relaxed) : g_tToe.load(std::memory_order_relaxed));
      m->written.store(true, std::memory_order_relaxed);
    } else if (m && m->ok && m->written.exchange(false)) {
      WriteFields(self, m->stock.lumMin, m->stock.toe);  // R62: applying stopped: DCS's values back, live module
    }
  }
  g_orig(self, a2, a3, a4, a5, a6, a7);
  g_inflight.fetch_sub(1, std::memory_order_acq_rel);
}

// Writable data (the bias lives in GraphicsCore's .data).
void WriteBias(float v) {
  if (!g_bias) return;
  DWORD old;
  if (VirtualProtect(g_bias, 4, PAGE_READWRITE, &old)) {
    *g_bias = v;
    VirtualProtect(g_bias, 4, old, &old);
  }
}

// Applying stops and the bias goes back. R62: the modules get DCS's values back from the hook at their next call
// (live `this`); with direct (payload stop: the hook is about to go), those the hook saw in the last kLiveMs are
// restored here, under SEH. A module not seen for longer may be freed: never written through again.
void RestoreAll(bool direct = true) {
  g_apply.store(0, std::memory_order_release);
  std::lock_guard<std::mutex> lock(g_modMutex);
  const ULONGLONG now = GetTickCount64();
  for (Module& m : g_mod) {
    m.haveTarget.store(false);
    uint8_t* obj = m.obj.load();
    if (!direct || !obj || !m.ok || !m.written.load()) continue;
    if (now - m.seen.load() <= kLiveMs && m.written.exchange(false))
      WriteFieldsGuarded(obj, m.stock.lumMin, m.stock.toe);
  }
  if (g_bias && g_biasWritten) {
    WriteBias(g_biasStock);
    g_biasWritten = false;
  }
}

bool Install() {
  if (g_state.load() != 0) return g_state.load() > 0;
  HMODULE gc = GetModuleHandleW(L"GraphicsCore.dll");
  HMODULE sr = GetModuleHandleW(L"SceneRenderer.dll");
  if (!gc || !sr) return false;  // retried next second
  auto* base = reinterpret_cast<const uint8_t*>(gc);
  auto* ctor = reinterpret_cast<const uint8_t*>(GetProcAddress(gc, kModuleCtor));
  auto* upd = reinterpret_cast<const uint8_t*>(GetProcAddress(gc, kUpdateRuntime));
  auto* sun = reinterpret_cast<const uint8_t*>(GetProcAddress(gc, kGetSunMoonDir));
  auto ctx = reinterpret_cast<CtxFn>(GetProcAddress(gc, kGetCurrentContext));
  const uint8_t* bias = upd ? BiasTarget(upd) : nullptr;
  void** slot = timercache::FindImport(sr, "GraphicsCore.dll", kAddToneMappingPass);
  const char* why = !ctor || !CtorHasToneMapperAt(ctor) ? "ToneMappingModule constructor"
                    : !bias || !sigscan::InImage(base, bias, 4) ? "exposure bias in updateRuntimeParameters"
                    : !sun || !ctx || !SunAt0x50(sun) ? "sun direction in the context"
                    : !slot ? "SceneRenderer's import of addToneMappingPass"
                    : nullptr;
  float b = 0;
  if (!why) {
    memcpy(&b, bias, 4);
    if (!(b >= -1.0f && b <= 1.0f)) why = "exposure bias value";
  }
  if (why) {
    g_state = -1;
    Log("night tone: GraphicsCore does not match this build (%s); not installed", why);
    return false;
  }
  g_bias = const_cast<float*>(reinterpret_cast<const float*>(bias));
  g_biasStock = b;
  g_ctx = ctx;
  void* orig = nullptr;
  if (!HookSlot(slot, reinterpret_cast<void*>(&Hook), &orig) || !orig) {
    g_state = -1;
    Log("night tone: addToneMappingPass import not hooked");
    return false;
  }
  g_orig = reinterpret_cast<AddPassFn>(orig);
  g_slot = slot;
  g_state = 1;
  Log("night tone: installed (exposure bias %.3f EV at GraphicsCore+0x%llx)", b,
      static_cast<unsigned long long>(bias - base));
  return true;
}

// Once per second (worker thread).
void Tick(Settings s) {
  s = Clamped(s);
  const bool changed = !(s == g_settings);
  g_settings = s;
  if (s.mode == 0) {
    if (changed && g_state.load() > 0) {
      RestoreAll(false);  // R62: the hook puts them back on the live modules
      Log("night tone: off (DCS's values back)");
    }
    return;
  }
  if (!Install()) return;
  float y = g_sunY.load();
  bool valid = g_sunValid.load();
  if (!valid) valid = ReadSunY(&y);
  const double w = s.gate ? (valid ? NightWeight(y) : 0.0) : 1.0;
  Stock st;
  st.bias = g_biasStock;
  int found = 0;
  for (Module& m : g_mod)
    if (m.obj.load() && m.ok) {
      st = m.stock;
      st.bias = g_biasStock;
      ++found;
    }
  const Target t = Compute(s, st, w);
  if (s.mode == 1) {
    g_tMin.store(t.lumMin);
    g_tToe.store(t.toe);
    {
      // R62: each module's target for the hook to write (no write through a stored pointer here).
      std::lock_guard<std::mutex> lock(g_modMutex);
      for (Module& m : g_mod)
        if (m.obj.load() && m.ok) {
          const Target tm = Compute(s, Stock{m.stock.lumMin, m.stock.toe, g_biasStock}, w);
          m.tMin.store(tm.lumMin, std::memory_order_relaxed);
          m.tToe.store(tm.toe, std::memory_order_relaxed);
          m.haveTarget.store(true, std::memory_order_release);
        }
    }
    g_apply.store(1, std::memory_order_release);
    WriteBias(t.bias);
    g_biasWritten = true;
  } else if (changed) {
    RestoreAll(false);  // observe only: DCS's own values (put back by the hook, R62)
  }
  static ULONGLONG last = 0;
  static double lastW = -1;
  const ULONGLONG now = GetTickCount64();
  if (changed || now - last >= 30000 || std::fabs(w - lastW) >= 0.1) {
    last = now;
    lastW = w;
    Log("night tone: %s, sun y %s%.3f, weight %.2f%s, modules %d, calls %llu; floor %.4f (DCS %.4f), toe %.3f "
        "(DCS %.3f), bias %+.2f EV (DCS %+.2f)",
        s.mode == 1 ? "applying" : "observing", valid ? "" : "unknown ", valid ? y : 0.0f, w,
        s.gate ? "" : " (ungated)", found, static_cast<unsigned long long>(g_calls.load()), t.lumMin, st.lumMin,
        t.toe, st.toe, s.mode == 1 ? t.bias : g_biasStock, g_biasStock);
  }
}

// Payload stop: values back, the slot back, in-flight hooks drained (bounded).
void Shutdown() {
  if (g_state.load() <= 0) return;
  g_apply.store(0, std::memory_order_release);
  if (g_slot) UnhookSlot(g_slot, reinterpret_cast<void*>(g_orig));
  for (int i = 0; i < 200 && g_inflight.load() > 0; ++i) Sleep(1);
  RestoreAll();  // R62: after the hook is gone (no hook writing at the same time); recently seen modules only
  g_slot = nullptr;
  g_state = 0;
}

}  // namespace nighttone
