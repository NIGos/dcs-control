// R37 night tone (night_tone.h): ini clamping, the sun-elevation weight, the written values (floor only raised,
// log blend; toe and bias linear), the uniform-block layout check, the hook's pass-through with the seven
// arguments and its writes into a fake ToneMappingModule, restore of every written value, and, on the DCS
// install's GraphicsCore.dll / SceneRenderer.dll (loaded without resolving imports), the byte checks Install
// relies on: ToneMapper at +0xe18, the exposure bias global (-0.2) behind updateRuntimeParameters, the sun at
// context +0x50, the uniform defaults (floor 0.01, toe 0.615) and SceneRenderer's import of addToneMappingPass.
#pragma once

namespace nttest {
using namespace nighttone;

// A uniform block with DCS's constructor defaults (GraphicsCore 0x6f6b0).
void FillStockUniforms(uint8_t* u) {
  memset(u, 0, 0x100);
  auto put = [&](size_t off, float v) { memcpy(u + off, &v, 4); };
  put(0x00, 1.4f); put(0x04, 0.5f); put(0x08, 4.0f); put(0x10, 0.4f); put(0x14, 0.035f); put(0x18, 0.6f);
  put(0x38, 1.4f); put(0x3c, 1.1f); put(kSceneLumMin, 0.01f); put(kSceneLumMax, 0.6f); put(kFocusWidth, 1.0f);
  put(kFocusSigma, 0.4f); put(0x50, 0.001f); put(0x54, 3.0f); put(kPercentMin, 0.3f); put(kPercentMax, 0.85f);
  put(0x68, 1.0f); put(0x6c, 0.25f); put(kSlope, 0.8f); put(kToe, 0.615f); put(0x78, 0.35f); put(0x80, 0.04f);
  put(kLutLogMin, -2.5f); put(kLutLogMax, 1.5f); put(0x98, 1.0f / 2.2f);
}

struct FakeCall {
  uint8_t* self = nullptr;
  void* a[6] = {};
  int n = 0;
};
FakeCall g_fake;
void __fastcall FakeAddPass(uint8_t* self, void* a2, void* a3, void* a4, void* a5, void* a6, void* a7) {
  g_fake.self = self;
  void* v[6] = {a2, a3, a4, a5, a6, a7};
  memcpy(g_fake.a, v, sizeof(v));
  g_fake.n++;
}

float g_fakeSun[32] = {};  // a fake ContextBase (sun direction at +0x50 = float index 20)
uint8_t* __cdecl FakeCtx() { return reinterpret_cast<uint8_t*>(g_fakeSun); }

void ResetModules() {
  for (Module& m : g_mod) {
    m.obj.store(nullptr);
    m.ok = false;
    m.written = false;
    m.seen = 0;
    m.haveTarget = false;
    m.stock = Stock{};
  }
  g_fullLogged = false;
}

void Run() {
  printf("-- night tone (R37)\n");
  // Ini clamping.
  {
    Settings s = Clamped(Settings{5, 0.001, 2.0, -9.0, true});
    Check(s.mode == 0 && s.floor == 0.01 && s.toe == 0.95 && s.ev == -4.0, "night tone: ini values clamped");
    Settings z = Clamped(Settings{1, 0.0, 0.0, 0.0, false});
    Check(z.floor == 0 && z.toe == 0 && z.ev == 0 && !z.gate, "night tone: zeros keep DCS's values");
  }
  // Sun weight.
  Check(NightWeight(0.5) == 0.0 && NightWeight(kDayY) == 0.0 && NightWeight(-0.5) == 1.0 &&
            std::fabs(NightWeight((kDayY + kNightY) / 2) - 0.5) < 1e-9,
        "night tone: weight 0 by day, 1 at night, linear in twilight");
  // Computed values.
  {
    const Stock st{0.01f, 0.615f, -0.2f};
    const Settings s{1, 0.1, 0.8, -1.0, true};
    Target t0 = Compute(s, st, 0.0), t1 = Compute(s, st, 1.0), th = Compute(s, st, 0.5);
    Check(t0.lumMin == st.lumMin && t0.toe == st.toe && t0.bias == st.bias, "night tone: weight 0 = DCS's values");
    Check(std::fabs(t1.lumMin - 0.1f) < 1e-6f && std::fabs(t1.toe - 0.8f) < 1e-6f && std::fabs(t1.bias + 1.2f) < 1e-6f,
          "night tone: weight 1 = configured values");
    Check(std::fabs(th.lumMin - std::sqrt(0.01f * 0.1f)) < 1e-6f, "night tone: floor blended in log space");
    Target lower = Compute(Settings{1, 0.005, 0, 0, true}, st, 1.0);
    Check(lower.lumMin == st.lumMin, "night tone: the floor is only raised");
  }
  // Layout check.
  {
    uint8_t u[0x100];
    FillStockUniforms(u);
    Check(UniformsLookStock(u), "night tone: DCS's uniform defaults accepted");
    float bad = 0.5f;
    memcpy(u + kFocusSigma, &bad, 4);
    Check(!UniformsLookStock(u), "night tone: a different block refused");
  }
  // Hook pass-through and writes on a fake module; restore.
  {
    ResetModules();
    std::vector<uint8_t> mod(kModuleToneMapper + kUniforms + 0x100 + 64, 0);
    uint8_t* u = mod.data() + kModuleToneMapper + kUniforms;
    FillStockUniforms(u);
    float bias = -0.2f;
    const AddPassFn savedOrig = g_orig;
    float* savedBias = g_bias;
    const CtxFn savedCtx = g_ctx;
    g_orig = &FakeAddPass;
    g_bias = &bias;
    g_biasStock = -0.2f;
    g_ctx = &FakeCtx;
    g_fakeSun[20] = 0.0f; g_fakeSun[21] = -1.0f; g_fakeSun[22] = 0.0f;  // sun straight down
    g_fake = FakeCall{};
    g_apply.store(0);
    Hook(mod.data(), (void*)1, (void*)2, (void*)3, (void*)4, (void*)5, (void*)6);
    bool args = g_fake.n == 1 && g_fake.self == mod.data();
    for (int i = 0; i < 6; ++i) args = args && g_fake.a[i] == reinterpret_cast<void*>(static_cast<uintptr_t>(i + 1));
    Check(args, "night tone: hook passes the seven arguments through");
    Check(F(u, kSceneLumMin) == 0.01f && F(u, kToe) == 0.615f, "night tone: nothing written before apply");
    Check(g_sunValid.load() && g_sunY.load() == -1.0f, "night tone: sun read from the context");
    Check(g_mod[0].obj.load() == mod.data() && g_mod[0].ok, "night tone: module noted with its stock values");
    // Apply at night through the hook and through the worker's write.
    const Settings s{1, 0.1, 0.8, -1.0, true};
    const Target t = Compute(s, g_mod[0].stock, NightWeight(-1.0));
    g_tMin.store(t.lumMin);
    g_tToe.store(t.toe);
    g_apply.store(1);
    Hook(mod.data(), nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);
    Check(std::fabs(F(u, kSceneLumMin) - 0.1f) < 1e-6f && std::fabs(F(u, kToe) - 0.8f) < 1e-6f,
          "night tone: hook writes floor and toe");
    WriteBias(t.bias);
    g_biasWritten = true;
    Check(std::fabs(bias + 1.2f) < 1e-6f, "night tone: bias written");
    // A module with a foreign layout is never written.
    std::vector<uint8_t> other(mod.size(), 0);
    Hook(other.data(), nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);
    Check(!g_mod[1].ok && F(other.data() + kModuleToneMapper + kUniforms, kSceneLumMin) == 0.0f,
          "night tone: foreign block left alone");
    RestoreAll();
    Check(F(u, kSceneLumMin) == 0.01f && F(u, kToe) == 0.615f && bias == -0.2f && g_apply.load() == 0,
          "night tone: restore puts DCS's values back");
    ResetModules();
    g_orig = savedOrig;
    g_bias = savedBias;
    g_ctx = savedCtx;
    g_sunValid.store(false);
  }
  // Byte checks on the DCS install's binaries.
  HMODULE gc = dcsinstall::LoadBinary(L"GraphicsCore.dll");
  HMODULE sr = dcsinstall::LoadBinary(L"SceneRenderer.dll");
  if (!gc || !sr) {
    dcsinstall::Skip("night tone build checks");
    return;
  }
  auto* gb = reinterpret_cast<const uint8_t*>(gc);
  auto* ctor = reinterpret_cast<const uint8_t*>(GetProcAddress(gc, kModuleCtor));
  auto* upd = reinterpret_cast<const uint8_t*>(GetProcAddress(gc, kUpdateRuntime));
  auto* sun = reinterpret_cast<const uint8_t*>(GetProcAddress(gc, kGetSunMoonDir));
  Check(ctor && CtorHasToneMapperAt(ctor), "night tone: ToneMappingModule builds its ToneMapper at +0xe18");
  const uint8_t* bias = upd ? BiasTarget(upd) : nullptr;
  float b = 0;
  if (bias && sigscan::InImage(gb, bias, 4)) memcpy(&b, bias, 4);
  Check(bias && sigscan::InImage(gb, bias, 4) && b == -0.2f, "night tone: exposure bias global is -0.2 EV");
  Check(sun && SunAt0x50(sun) && GetProcAddress(gc, kGetCurrentContext), "night tone: sun direction at context +0x50");
  {
    // ToneMapperUniforms constructor: mov dword [rcx+0x40], 0.01 ... mov dword [rcx+0x74], 0.615 close after it.
    static const uint8_t kMin[7] = {0xC7, 0x41, 0x40, 0x0A, 0xD7, 0x23, 0x3C};
    static const uint8_t kToeImm[7] = {0xC7, 0x41, 0x74, 0xA4, 0x70, 0x1D, 0x3F};
    sigscan::Range text{};
    bool found = false;
    if (sigscan::Section(gb, ".text", text))
      for (const uint8_t* p = text.begin; p + 0x100 < text.end && !found; ++p)
        if (memcmp(p, kMin, 7) == 0)
          for (size_t k = 7; k < 0xc0 && !found; ++k) found = memcmp(p + k, kToeImm, 7) == 0;
    Check(found, "night tone: uniform defaults floor 0.01 and toe 0.615");
  }
  Check(sigscan::ImportSlotRva(reinterpret_cast<const uint8_t*>(sr), "GraphicsCore.dll", kAddToneMappingPass) != 0,
        "night tone: SceneRenderer imports addToneMappingPass");
}

}  // namespace nttest
