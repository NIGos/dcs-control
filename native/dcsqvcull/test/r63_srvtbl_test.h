// R63 U5: the cockpit ipd's ownship matching finds NGModel's SceneRenderable vtable by RTTI (cockpit_ipd.h
// ResolveSrVtbl), not by the raw recorded RVA, so a rebuilt NGModel with shifted .rdata does not silently compare
// every G-buffer entry with a stale address.
#pragma once

namespace r63sr {

void Run() {
  namespace ck = fixdepth::ckipd;
  bool moved = true;
  Check(ck::ResolveSrVtbl(nullptr, gbbatch::kSrVtbl, &moved) == nullptr && !moved,
        "R63 U5: no NGModel: no SceneRenderable vtable");
  HMODULE ng = dcsinstall::LoadBinary(L"NGModel.dll");
  HMODULE sc = dcsinstall::LoadBinary(L"Scene.dll");
  if (!ng || !sc) {
    dcsinstall::Skip("R63 U5 SceneRenderable vtable by RTTI");
  } else {
    const auto* b = reinterpret_cast<const uint8_t*>(ng);
    const void* vt = ck::ResolveSrVtbl(b, gbbatch::kSrVtbl, &moved);
    const char* n = vt ? sigscan::RttiName(b, static_cast<void* const*>(vt)) : nullptr;
    Check(vt && n && strcmp(n, ".?AVSceneRenderable@model@@") == 0,
          "R63 U5: the installed NGModel's SceneRenderable vtable is found (RTTI model::SceneRenderable)");
    // A rebuild that shifted .rdata: the recorded RVA now names another class (or nothing).
    bool moved2 = false;
    const void* vt2 = ck::ResolveSrVtbl(b, gbbatch::kSrVtbl + 0x40, &moved2);
    Check(vt2 == vt && moved2,
          "R63 U5: recorded RVA off by 0x40 (shifted .rdata): the vtable is found again by RTTI, reported as moved");
    // A module without the class: nothing (ownship matching off, logged), never the raw address.
    bool moved3 = true;
    Check(ck::ResolveSrVtbl(reinterpret_cast<const uint8_t*>(sc), gbbatch::kSrVtbl, &moved3) == nullptr && !moved3,
          "R63 U5: an image without model::SceneRenderable: no vtable (not image+0x59810)");
  }
  if (ng) FreeLibrary(ng);
  if (sc) FreeLibrary(sc);
}

}  // namespace r63sr
