// R63 U2: partition boost writes DCSScene+0x40 and raises the collect call's last argument only when the
// running Scene.dll's collect code uses both as on 2.9.30 (scene_check.h, main.cpp CheckSceneLayout).
#pragma once

namespace r63scene {

// Scene.dll 2.9.30 collectSceneObjectsRenderables +0x69b..+0x6c7 (verbatim).
const uint8_t k2930[] = {0x41, 0x8B, 0x4C, 0x24, 0x38, 0x85, 0xC9, 0x0F, 0x84, 0xC8, 0x04, 0x00, 0x00, 0xBA, 0x04,
                         0x00, 0x00, 0x00, 0xB8, 0x02, 0x00, 0x00, 0x00, 0x83, 0xF9, 0x01, 0x0F, 0x47, 0xD0, 0x39,
                         0x8C, 0x24, 0x88, 0x20, 0x00, 0x00, 0x0F, 0x42, 0x8C, 0x24, 0x88, 0x20, 0x00, 0x00, 0xFF,
                         0xC1};

void Matcher() {
  std::vector<uint8_t> c(0x100, 0xCC);
  memcpy(c.data() + 0x40, k2930, sizeof(k2930));
  Check(scenecheck::FindThreadsMaxUse(c.data(), c.size()) == 0x40,
        "R63 U2: the 2.9.30 collect code (mov ecx,[r12+0x38] ... cmp/cmovb [rsp+0x2088]) is recognised");
  // A rebuild with other registers and frame: mov eax,[rbx+0x38] ... cmp [rsp+0x1098],eax ; cmovb eax,[rsp+0x1098].
  const uint8_t other[] = {0x8B, 0x43, 0x38, 0x85, 0xC0, 0x39, 0x84, 0x24, 0x98, 0x10, 0x00, 0x00,
                           0x0F, 0x42, 0x84, 0x24, 0x98, 0x10, 0x00, 0x00};
  std::vector<uint8_t> o(0x80, 0x90);
  memcpy(o.data() + 7, other, sizeof(other));
  Check(scenecheck::FindThreadsMaxUse(o.data(), o.size()) == 7,
        "R63 U2: the same use with other registers and another frame size is recognised");
  // SceneBase grew: the field is now read at +0x40 (scene+0x48) and +0x38 holds something else.
  std::vector<uint8_t> moved = c;
  moved[0x40 + 4] = 0x40;
  Check(scenecheck::FindThreadsMaxUse(moved.data(), moved.size()) == SIZE_MAX,
        "R63 U2: field moved (read at this+0x40): not recognised, partition boost leaves +0x40 alone");
  std::vector<uint8_t> argMoved = c;
  argMoved[0x40 + 40] = 0x90;  // cmovb reads another stack slot than the cmp
  Check(scenecheck::FindThreadsMaxUse(argMoved.data(), argMoved.size()) == SIZE_MAX,
        "R63 U2: the field is no longer capped by the same argument: not recognised");
  std::vector<uint8_t> noMin = c;
  noMin[0x40 + 36] = 0x0F;
  noMin[0x40 + 37] = 0x47;  // cmova: a max, not a min
  Check(scenecheck::FindThreadsMaxUse(noMin.data(), noMin.size()) == SIZE_MAX,
        "R63 U2: the argument is no longer the cap (cmova): not recognised");
  std::vector<uint8_t> local = c;
  local[0x40] = 0x90;  // mov ecx,[rsp+0x38] (no REX.B): a local, not this
  Check(scenecheck::FindThreadsMaxUse(local.data(), local.size()) == SIZE_MAX,
        "R63 U2: a read of [rsp+0x38] (a local) does not count as the field");
  Check(scenecheck::FindThreadsMaxUse(c.data(), 0x40 + 30) == SIZE_MAX, "R63 U2: truncated code: not recognised");
}

uint32_t g_flags = 0;
void __fastcall FakeCollect(void*, uint32_t, uint8_t*, uint8_t*, void*, uint32_t flags) { g_flags = flags; }

void HookGate() {
  alignas(16) static uint8_t sceneObj[0x200];
  memset(sceneObj, 0, sizeof(sceneObj));
  auto* field = reinterpret_cast<uint32_t*>(sceneObj + kSceneCollectThreadsMax);
  *field = 3;  // a small value of whatever now lives at +0x40
  const CollectFn orig0 = g_origCollect;
  void* const scene0 = g_scene.load();
  const uint32_t def0 = g_defaultThreadsMax.load();
  const int sl0 = g_sceneLayout.load();
  const bool po0 = g_perceptionOnly.load(), pb0 = g_partitionBoost.load();
  const int as0 = allocslab::g_state.load(), du0 = directupload::g_state.load();
  auto* const pw0 = partw::g_orig;
  allocslab::g_state = 0;
  directupload::g_state = 0;
  partw::g_orig = nullptr;
  g_origCollect = &FakeCollect;
  g_perceptionOnly = false;
  g_partitionBoost = true;
  g_defaultThreadsMax = 0;
  g_sceneLayout = -1;
  HookCollect(sceneObj + 8, 1, nullptr, nullptr, nullptr, 2);
  Check(*field == 3 && g_flags == 2,
        "R63 U2: collect code not verified: boost on leaves scene+0x40 and the argument unchanged");
  g_partitionBoost = false;
  *field = 5;
  HookCollect(sceneObj + 8, 1, nullptr, nullptr, nullptr, 2);
  Check(*field == 5 && g_flags == 2,
        "R63 U2: collect code not verified: boost off (kill switch) does not write the first value back");
  g_partitionBoost = true;
  g_defaultThreadsMax = 0;
  *field = 6;
  g_sceneLayout = 1;
  HookCollect(sceneObj + 8, 1, nullptr, nullptr, nullptr, 2);
  Check(*field == 8 && g_flags == 7, "R63 U2: verified collect code: boost raises the limit to 8 and the argument to 7");
  g_partitionBoost = false;
  HookCollect(sceneObj + 8, 1, nullptr, nullptr, nullptr, 2);
  Check(*field == 6 && g_flags == 2, "R63 U2: verified collect code: boost off puts DCS's limit back");
  g_sceneLayout = sl0;
  g_defaultThreadsMax = def0;
  g_partitionBoost = pb0;
  g_perceptionOnly = po0;
  allocslab::g_state = as0;
  directupload::g_state = du0;
  partw::g_orig = pw0;
  g_scene = scene0;
  g_origCollect = orig0;
}

void RealSceneDll() {
  HMODULE m = dcsinstall::LoadBinary(L"Scene.dll");
  if (!m) {
    dcsinstall::Skip("R63 U2 Scene.dll collect code");
    return;
  }
  void* collect = reinterpret_cast<void*>(GetProcAddress(m, kCollectExport));
  Check(collect && CheckSceneLayout(m, collect),
        "R63 U2: the installed Scene.dll's collect function reads scene+0x40 and caps it by its last argument");
  FreeLibrary(m);
}

void Run() {
  Matcher();
  HookGate();
  RealSceneDll();
}

}  // namespace r63scene
