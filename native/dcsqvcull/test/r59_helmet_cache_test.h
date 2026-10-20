// R59 helmet-mat-cache-stale-across-missions / helmet-mat-cache-spill-perf:
// the helmet switch's material verdicts are re-judged when the address is
// reused (another name pointer) and cleared at a mission event; a full table
// is cleared instead of spilling every later lookup.
#pragma once

namespace r59helmet {
using namespace fixdepth::ckipd;

struct FakeName {
  uint8_t pad[8] = {};
  char s[64] = {};
};
struct FakeMat {
  uint8_t b[0x40] = {};
  void Name(FakeName* n) { *reinterpret_cast<FakeName**>(b + 0x20) = n; }
  const uint8_t* p() const { return b; }
};

void Run() {
  ClearHelmetMats();
  FakeName helmet, cockpit;
  strcpy_s(helmet.s, "//models/pilot_ah64_helmet.edm");
  strcpy_s(cockpit.s, "//models/ah-64d_cockpit.edm");
  FakeMat a;
  // 1. The address reused by another material (another name): judged again.
  a.Name(&cockpit);
  const int v0 = HelmetEntry(a.p())->verdict;
  a.Name(&helmet);
  const int v1 = HelmetEntry(a.p())->verdict;
  // 2. Reused with the same name pointer (the name string reused too): cleared by a mission event.
  strcpy_s(helmet.s, "//models/f-16c_bl50_cockpit.edm");
  const int v2 = HelmetEntry(a.p())->verdict;  // still the cached 1 (nothing says the address changed)
  HelmetOnMissionEvent();
  HelmetConsumeReset();
  const int nAfterReset = g_helmetMatN;
  const int v3 = HelmetEntry(a.p())->verdict;
  Check(v0 == 0 && v1 == 1 && v2 == 1 && nAfterReset == 0 && v3 == 0,
        "R59 helmet: a reused material address is re-judged (name pointer), and every verdict is dropped at a "
        "mission event");
  // 3. Many materials over a session (several cockpits): lookups stay cached (distinct, stable entries), the
  // table is cleared when full instead of spilling.
  ClearHelmetMats();
  std::vector<FakeMat> mats(3000);
  for (FakeMat& m : mats) m.Name(&cockpit);
  bool stable = true;
  for (size_t i = 0; i < mats.size(); ++i) {
    HelmetMatEntry* e = HelmetEntry(mats[i].p());
    // (a lookup that finds the table 3/4 full clears it first: the entry can move then)
    stable &= e->mat == mats[i].p() && e->verdict == 0 && HelmetEntry(mats[i].p())->mat == mats[i].p();
  }
  HelmetMatEntry* e1 = HelmetEntry(mats[2999].p());
  HelmetMatEntry* e2 = HelmetEntry(mats[2998].p());
  stable &= e1 != e2 && e1->mat == mats[2999].p() && e2->mat == mats[2998].p() &&
            g_helmetMatN <= kHelmetMats * 3 / 4;
  ClearHelmetMats();
  Check(stable, "R59 helmet: 3000 materials keep cached, distinct verdict entries (no per-lookup spill)");
}

}  // namespace r59helmet
