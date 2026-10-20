// R62 nighttone-stale-module-writes: night tone writes only live modules (night_tone.h).
#pragma once

namespace r62nt {

void NightToneLiveness() {
  // nighttone-stale-module-writes: only the hook's live `this` is written; a module not seen recently is never
  // written through again; more than kMaxModules modules is logged.
  using namespace nighttone;
  nttest::ResetModules();
  const AddPassFn savedOrig = g_orig;
  const CtxFn savedCtx = g_ctx;
  float* savedBias = g_bias;
  g_orig = &nttest::FakeAddPass;
  g_ctx = nullptr;
  g_bias = nullptr;
  std::vector<uint8_t> mod(kModuleToneMapper + kUniforms + 0x100 + 64, 0);
  uint8_t* u = mod.data() + kModuleToneMapper + kUniforms;
  nttest::FillStockUniforms(u);
  g_apply.store(0);
  Hook(mod.data(), nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);
  // The worker's per-module target, written by the hook.
  g_mod[0].tMin = 0.05f, g_mod[0].tToe = 0.7f;
  g_mod[0].haveTarget = true;
  g_tMin = 0.5f, g_tToe = 0.9f;  // the global (another module's) target: not used for this one
  g_apply.store(1);
  Hook(mod.data(), nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);
  const bool own = F(u, kSceneLumMin) == 0.05f && F(u, kToe) == 0.7f && g_mod[0].written.load();
  // Applying stops (NightTone 0): no direct write; the hook's next call puts DCS's values back.
  RestoreAll(false);
  const bool notDirect = F(u, kSceneLumMin) == 0.05f;
  Hook(mod.data(), nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);
  const bool hookRestored = F(u, kSceneLumMin) == 0.01f && F(u, kToe) == 0.615f && !g_mod[0].written.load();
  Check(own && notDirect && hookRestored,
        "R62 night tone: the hook writes each module's own target and puts DCS's values back on the live module");
  // Payload stop: a module the hook has not seen for longer than kLiveMs (freed by DCS) is not written.
  g_mod[0].haveTarget = true;  // the worker's next tick
  g_apply.store(1);
  Hook(mod.data(), nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);
  g_mod[0].seen = GetTickCount64() - (kLiveMs + 1000);
  const float before = F(u, kSceneLumMin);
  RestoreAll();
  const bool staleLeft = F(u, kSceneLumMin) == before && before == 0.05f;
  g_apply.store(1);
  Hook(mod.data(), nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);  // seen now
  RestoreAll();
  const bool liveRestored = F(u, kSceneLumMin) == 0.01f && F(u, kToe) == 0.615f;
  Check(staleLeft && liveRestored,
        "R62 night tone: at payload stop only modules the hook saw in the last 1.5 s are written (no stale pointer)");
  // More modules than slots: logged once.
  std::vector<std::vector<uint8_t>> many(kMaxModules + 1, std::vector<uint8_t>(mod.size(), 0));  // zero blocks: not DCS's layout
  g_apply.store(0);
  {
    std::lock_guard<std::mutex> lock(g_logMutex);
    g_reportCapture = true;
    g_reportThread = GetCurrentThreadId();
    g_reportBuf.clear();
  }
  // Slot 0 holds `mod`; kMaxModules more fill the rest and overflow by one, twice.
  for (auto& m : many) Hook(m.data(), nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);
  Hook(many.back().data(), nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);
  std::string captured;
  {
    std::lock_guard<std::mutex> lock(g_logMutex);
    g_reportCapture = false;
    captured.swap(g_reportBuf);
  }
  size_t n = 0;
  for (size_t at = captured.find("more than 8 tone mapping modules"); at != std::string::npos;
       at = captured.find("more than 8 tone mapping modules", at + 1))
    ++n;
  Check(n == 1, "R62 night tone: a module past the 8 slots is logged once (was silent)");
  nttest::ResetModules();
  g_orig = savedOrig;
  g_ctx = savedCtx;
  g_bias = savedBias;
  g_apply.store(0);
}

void Run() {
  NightToneLiveness();
}

}  // namespace r62nt
