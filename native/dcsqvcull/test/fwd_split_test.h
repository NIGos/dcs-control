// Offline tests for fwd_split.h (A5): categories, views by occurrence, the
// window difference with graph replacement, the aggregation into views and
// "x", the greppable line, the item category on fake NGModel memory, and the
// override's pass-through outside a forward pass.
#pragma once

namespace fstest {
using namespace fwdsplit;

void PureTests() {
  Check(MatCatOfName(".?AVGlassMaterialMT@model@@") == kGlass && MatCatOfName(".?AVDeckMaterialMT@model@@") == kOther &&
            MatCatOfName(nullptr) == kOther,
        "fwd split: material RTTI -> glass or other");
  Check(IsTerrainName(".?AVTerrainObject@Visualizer@@") && !IsTerrainName(".?AVParticleSystem@@") && !IsTerrainName(nullptr),
        "fwd split: TerrainObject classes by RTTI");
  Check(ModelCat(8) == kFlat && ModelCat(1) == kModel && ModelCat(2) == kModel && ModelCat(0) == kModel,
        "fwd split: model pass 8 = flat_shadow, the rest models");
  Check(ViewOf(0) == 0 && ViewOf(3) == 3 && ViewOf(4) == kViews && ViewOf(100) == kViews,
        "fwd split: occurrence since the render entry -> v0..v3, then x");
  Check(CockpitKind(fwdreccount::kTransparentCockpit) && CockpitKind(fwdreccount::kTransparentCockpitDecal) &&
            !CockpitKind(fwdreccount::kTransparent),
        "fwd split: cockpit pass kinds");

  static Snap prev, now, d;
  memset(&prev, 0, sizeof(prev));
  memset(&now, 0, sizeof(now));
  const int tp = fwdreccount::kTransparent, ck = fwdreccount::kTransparentCockpit;
  // Graph 1 is the main one (most cycles); graph 0 small (an MFD graph); graph 2 replaced in the window.
  for (int v = 0; v < kViews; ++v) {
    PCell& c = now.c[1][tp][v];
    c.calls = 100;
    c.cyc[kGlass] = 1000 * (v + 1);
    c.items[kGlass] = 40;
    c.cyc[kModel] = 500;
    c.items[kModel] = 10;
    c.cyc[kOther] = 100;
    now.c[1][ck][v].calls = 100;
    now.c[1][ck][v].cyc[kGlass] = 200;
    now.c[1][ck][v].items[kGlass] = 5;
  }
  now.c[1][tp][kViews].calls = 10;
  now.c[1][tp][kViews].cyc[kTerrain] = 70;
  now.c[0][tp][0].calls = 5;
  now.c[0][tp][0].cyc[kOther] = 50;
  now.entries[1] = 100;
  prev.entries[1] = 0;
  prev.gen[2] = 1;
  now.gen[2] = 2;
  prev.c[2][tp][0].cyc[kOther] = 999;  // the old graph's counters: not subtracted from the new one
  now.c[2][tp][0].cyc[kOther] = 30;
  Diff(now, prev, d);
  Check(d.c[2][tp][0].cyc[kOther] == 30 && d.c[1][tp][2].cyc[kGlass] == 3000 && d.entries[1] == 100,
        "fwd split: window difference, a replaced graph counts from 0");
  const Agg a = Aggregate(d);
  Check(a.mainGraph == 1 && a.mainEntries == 100, "fwd split: main graph = the most forward cycles");
  Check(a.view[0].cyc[kGlass] == 1200 && a.view[3].cyc[kGlass] == 4200 && a.view[3].items[kGlass] == 45,
        "fwd split: per view = every pass kind of that occurrence");
  Check(a.view[kViews].cyc[kTerrain] == 70 && a.view[kViews].cyc[kOther] == 80,
        "fwd split: x = 5th occurrence plus the other graphs");
  Check(a.cockpit.cyc[kGlass] == 800 && a.cockpit.items[kGlass] == 20 && a.total.cyc[kGlass] == 10800,
        "fwd split: cockpit-pass glass and totals");
  const std::string line = FormatLine(a, 10, 100, 10, 1, 0, true);
  Check(line.rfind("fwd split: 10 s, 100 frames, ", 0) == 0 && line.find(" glass 10.800 ms 2") != std::string::npos &&
            line.find("ck glass 0.800 ms 0") != std::string::npos && line.find(" v3 4.200/0.000/0.000/0.500/0.100 n 0/0/0/0/0") != std::string::npos &&
            line.find(" x ") != std::string::npos && line.find("terrain classes timed 1 untimed 0") != std::string::npos &&
            line.find("views not split") == std::string::npos && line.find('\n') == std::string::npos,
        "fwd split: one greppable line with totals, cockpit glass and per-view split");
  Check(FormatLine(a, 10, 100, 10, 0, 1, false).find("views not split") != std::string::npos,
        "fwd split: the line says when the render entry is not observed");
}

// SceneRenderable item -> category on fake memory (r+0x10 item, item+0x10 material, r+0x60 model pass).
void FakeItemTests() {
  alignas(16) static uint8_t sr[0x80], item[0x20], mat[0x10], mat2[0x10];
  static void* modelVt[4] = {};
  static void* otherVt[4] = {};
  void* const savedModel = g_modelMatVt;
  const int savedN = g_matN;
  g_modelMatVt = modelVt;
  *reinterpret_cast<void**>(mat) = modelVt;
  *reinterpret_cast<void**>(mat2) = otherVt + 1;  // vt[-1] = nullptr: no RTTI
  *reinterpret_cast<uint8_t**>(item + 0x10) = mat;
  *reinterpret_cast<uint8_t**>(sr + 0x10) = item;
  *reinterpret_cast<uint32_t*>(sr + 0x60) = 8;
  const int flat = SrCatRaw(sr);
  *reinterpret_cast<uint32_t*>(sr + 0x60) = 2;
  const int model = SrCatRaw(sr);
  *reinterpret_cast<uint8_t**>(item + 0x10) = mat2;
  const int other = SrCatRaw(sr);  // no RTTI behind the fake vtable: not glass
  *reinterpret_cast<uint8_t**>(item + 0x10) = nullptr;
  const int none = SrCatRaw(sr);
  *reinterpret_cast<uint8_t**>(sr + 0x10) = reinterpret_cast<uint8_t*>(0x10);  // unreadable item
  const int fault = SrCatRaw(sr);
  Check(flat == kFlat && model == kModel && other == kOther && none == kOther && fault == kOther,
        "fwd split: item category from the model pass and the material class, faults contained");
  g_modelMatVt = savedModel;
  g_matN = savedN;
  uint64_t r = 7;
  Check(!t_inPass && !Override(nullptr, nullptr, &r) && r == 7,
        "fwd split: the override passes through outside a forward pass");
}

void Run() {
  PureTests();
  FakeItemTests();
}

}  // namespace fstest
