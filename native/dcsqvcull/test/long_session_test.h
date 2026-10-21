// Offline tests for the long-session round (2026-10-12):
//  - shadow recorder: a new mesh at a reused address is relearnt (probe
//    requested, old entry superseded, the new mesh recorded), not more often
//    than once per cooldown; a refused mesh's address reused by a recordable
//    one is relearnt too; the key and mesh tables stay bounded across
//    "missions" (compaction on the render thread, the replaced tables freed
//    only once no armed or running job's snapshot points at them, references
//    balanced);
//  - G-buffer recorder: the same reused-address relearn and compaction
//    (keys per collection, read sets, meshes);
//  - cost weights: the per-object table evicts objects unused for a while, so
//    a later mission's objects are measured and the scale follows them;
//  - big model pages: pages retired by one payload are restored by the next
//    one (hot reload) when big pages are turned off;
//  - perf line: DCS's GPU share (D3DKMT per-process query) answers, and a
//    handle kind that stops answering is replaced.
// QV_LONGSESS_ONLY=1: only these tests. Built against sources without the
// fixes (no QV_LONG_SESSION_FIXES), the checks that need no new function
// still run and fail: the reproduction.
#pragma once

namespace lstest {

#ifndef QV_LONG_SESSION_FIXES
constexpr uint32_t kCooldown = 900;
#else
constexpr uint32_t kCooldown = shrec::kMeshCooldown;
#endif

// ---------------------------------------------------------------------------
// Shadow recorder: probes of World meshes through ProcessProbe
// ---------------------------------------------------------------------------
using srtest::FakeView;
using srtest::World;

struct ShProbe {
  FakeView vs, il;
  shrec::Capture Cap(World& w, int m, int instances = 0) {
    shrec::Capture c;
    shrec::MeshLive l;
    shrec::ReadMesh(w.mesh[m], l);
    c.draws = 1;
    c.rendererOk = true;
    c.vs = reinterpret_cast<ID3D11VertexShader*>(static_cast<IUnknown*>(&vs));
    c.b7 = reinterpret_cast<ID3D11Buffer*>(uintptr_t{0x7720});
    c.cb = reinterpret_cast<ID3D11Buffer*>(uintptr_t{0x7730});
    c.sb = reinterpret_cast<ID3D11ShaderResourceView*>(uintptr_t{0x7740});
    c.prim = static_cast<int>(shrec::kPrimTriList);
    c.a5 = static_cast<int>(l.count);
    c.instances = instances;
    c.topo = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    c.ib = l.ib;
    c.ibFormat = l.ibFormat;
    c.vb = l.vb;
    c.stride = l.stride;
    c.il = reinterpret_cast<ID3D11InputLayout*>(static_cast<IUnknown*>(&il));
    c.streamsOk = true;
    c.streamCount = 1;
    c.stream0 = l.vbObj;
    c.descCount = l.elems;
    c.descSlot0 = true;
    return c;
  }
  shrec::ProbeFacts Facts(World& w, int m, const shrec::Capture& c) {
    shrec::ProbeFacts f;
    f.dcsVs = c.vs;
    f.passB7 = c.b7;
    f.pageSrv = c.sb;
    f.meshWhy = shrec::ReadMeshGuarded(w.mesh[m], f.mesh);
    return f;
  }
  static shrec::ProbeIn In(World& w, int s, int m) {
    shrec::ProbeIn in;
    in.shader = w.shader[s];
    in.tech = 1;
    in.mesh = w.mesh[m];
    in.effect = *reinterpret_cast<void**>(w.shader[s] + 0x50);
    in.techBegin = *reinterpret_cast<void**>(w.shader[s] + 0xb0);
    return in;
  }
  // DCS draws caster (shader s, mesh m) under the probe: what TryProbe publishes.
  void Probe(shrec::Tables& t, World& w, int s, int m, uint32_t now, int instances = 0) {
    shrec::Capture c = Cap(w, m, instances);
    shrec::ProbeFacts f = Facts(w, m, c);
    shrec::ProcessProbe(t, In(w, s, m), srtest::TestStatic(), f, c, now);
  }
};

// Casters of (untextured model) mesh m: [recorded, changed, rejected, wanted].
struct MeshOut {
  int all = 0, recorded = 0, changed = 0, rejected = 0, wanted = 0;
  UINT indexCount = 0;
  ID3D11Buffer* ib = nullptr;
};
MeshOut Outcome(const shrec::Job& j, World& w, int m) {
  MeshOut o;
  for (int i = 0; i < srtest::kObjs; ++i) {
    if (!w.Smr(i) || World::MatOf(i) == srtest::kTexturedMat || World::MeshOf(i) != m) continue;
    ++o.all;
    o.recorded += j.reason[i] == shrec::kRecorded;
    o.changed += j.reason[i] == shrec::kRMeshChanged;
    o.rejected += j.reason[i] == shrec::kRMeshRejected;
    o.wanted += j.want[i] != 0;
  }
  for (uint32_t r = 0; r < j.recCount; ++r) {
    const shrec::Rec& rc = j.recs[r];
    if (World::MeshOf(static_cast<int>(rc.caster)) == m) {
      o.indexCount = rc.mesh->indexCount;
      o.ib = rc.mesh->ib;
    }
  }
  return o;
}

void ShadowReuse() {
  using namespace shrec;
  srtest::TexFakes fakes;
  World* w = new World;
  w->Init();
  Tables t;
  AllocTables(t);
  ShProbe pr;
  // Mission 1: every (shader, mesh) probed; mesh 5 refused (an instanced draw).
  for (int s = 0; s < 2; ++s)
    for (int m = 0; m < srtest::kMeshes; ++m) pr.Probe(t, *w, s, m, 100, m == 5 ? 4 : 0);
  Job* j = NewJob();
  srtest::InitJob(*j, *w, t);
  j->useGen = 150;
  BuildJob(*j);
  const MeshOut base2 = Outcome(*j, *w, 2), base5 = Outcome(*j, *w, 5);
  Check(base2.all > 0 && base2.recorded == base2.all && base2.indexCount == 3 * w->tris[2] && base5.all > 0 &&
            base5.rejected == base5.all,
        "long session (shadow): probed meshes are recorded, a refused one is residual");
  // Mission 2: DCS freed meshes 2 and 5 and allocated other meshes at the same
  // addresses (other buffers, another primitive count).
  w->SetMeshBuffers(2, reinterpret_cast<void*>(uintptr_t{0x5000}), reinterpret_cast<void*>(uintptr_t{0x6000}));
  *reinterpret_cast<uint32_t*>(w->mesh[2] + 0x210) = 77;
  w->SetMeshBuffers(5, reinterpret_cast<void*>(uintptr_t{0x5100}), reinterpret_cast<void*>(uintptr_t{0x6100}));
  *reinterpret_cast<uint32_t*>(w->mesh[5] + 0x210) = 55;
  j->useGen = 200;
  BuildJob(*j);
  const MeshOut re2 = Outcome(*j, *w, 2), re5 = Outcome(*j, *w, 5);
  Check(re2.changed == re2.all && re2.recorded == 0 && re5.changed == re5.all,
        "long session (shadow): a new mesh at a reused address is never drawn from the old entry (residual)");
  Check(re2.wanted == 2 && re5.wanted == 2,
        "long session (shadow): a reused address asks for a probe, once per (shader, mesh) (was: never relearnt)");
  // The probes: DCS draws them; what it bound is learnt.
  for (int s = 0; s < 2; ++s) {
    pr.Probe(t, *w, s, 2, 210);
    pr.Probe(t, *w, s, 5, 210);
  }
  j->useGen = 220;
  BuildJob(*j);
  const MeshOut after2 = Outcome(*j, *w, 2), after5 = Outcome(*j, *w, 5);
  Check(after2.recorded == after2.all && after2.indexCount == 3 * 77 &&
            after2.ib == reinterpret_cast<ID3D11Buffer*>(uintptr_t{0x5000}) && after5.recorded == after5.all &&
            after5.indexCount == 3 * 55,
        "long session (shadow): after the probe the new mesh is recorded with its own buffers and count (coverage "
        "back)");
#ifdef QV_LONG_SESSION_FIXES
  // Cooldown: the same address changing again at once stays residual and asks for nothing.
  const uint64_t rel0 = g_meshRelearnt.load();
  *reinterpret_cast<uint32_t*>(w->mesh[2] + 0x210) = 78;
  j->useGen = 230;
  BuildJob(*j);
  const MeshOut cool = Outcome(*j, *w, 2);
  pr.Probe(t, *w, 0, 2, 230);
  const MeshEntry* me = FindMesh(t, w->mesh[2], w->shader[0], 1, *reinterpret_cast<void**>(w->shader[0] + 0x50),
                                 *reinterpret_cast<void**>(w->shader[0] + 0xb0));
  const bool cooled = cool.changed == cool.all && cool.wanted == 0 && me && me->indexCount == 3 * 77 &&
                      g_meshRelearnt.load() == rel0;
  j->useGen = 210 + kCooldown + 5;
  BuildJob(*j);
  const MeshOut warm = Outcome(*j, *w, 2);
  pr.Probe(t, *w, 0, 2, 210 + kCooldown + 5);
  pr.Probe(t, *w, 1, 2, 210 + kCooldown + 5);
  BuildJob(*j);
  const MeshOut back = Outcome(*j, *w, 2);
  Check(cooled && warm.wanted == 2 && back.recorded == back.all && back.indexCount == 3 * 78 &&
            g_meshRelearnt.load() == rel0 + 2,
        "long session (shadow): an address relearnt lately is not relearnt again before the cooldown (residual, "
        "no probe), then is");
#endif
  FreeJob(j);
  FreeTables(t);
  Check(pr.vs.refs == 1 && pr.il.refs == 1, "long session (shadow): the tables release every reference they took");
  delete w;
}

// ---------------------------------------------------------------------------
// Shadow recorder: tables across missions (the production MaintainTables)
// ---------------------------------------------------------------------------
void ShadowMissions() {
  using namespace shrec;
  srtest::TexFakes fakes;
  World* w = new World;
  w->Init();
  ShProbe pr;
  const shrec::Capture c = pr.Cap(*w, 0);
  const shrec::ProbeFacts f = pr.Facts(*w, 0, c);
  const KeyStatic st = srtest::TestStatic();
  // Six missions of 640 keys and 6000 meshes each: 3840 keys and 36000 meshes
  // over the session, beyond the 3/4 limits (3072, 24576) of the tables.
  constexpr int kMissions = 6, kKeysPer = 640, kMeshesPer = 6000, kLength = 4000, kStep = 50;
  auto ShaderAt = [&](int mission, int k) {
    return reinterpret_cast<uint8_t*>(uintptr_t{0x100000000} + (uintptr_t(mission) * kKeysPer + k) * 0x200);
  };
  auto MeshAt = [&](int mission, int i) {
    return reinterpret_cast<uint8_t*>(uintptr_t{0x900000000} + (uintptr_t(mission) * kMeshesPer + i) * 0x220);
  };
  void* const eff = reinterpret_cast<void*>(uintptr_t{0xeff0});
  void* const tb = reinterpret_cast<void*>(uintptr_t{0x7ec0});
  auto In = [&](int mission, int i) {
    ProbeIn in;
    in.shader = ShaderAt(mission, i % kKeysPer);
    in.tech = 1;
    in.mesh = MeshAt(mission, i);
    in.effect = eff;
    in.techBegin = tb;
    return in;
  };
#ifdef QV_LONG_SESSION_FIXES
  const Tables savedTab = g_tab, savedOld = g_tabOld;
  const CompactPlan savedPlan = g_plan;
  g_tabOld = Tables();
  g_plan = CompactPlan();
  const uint64_t comp0 = g_compactions.load(), freed0 = g_oldFreed.load();
#else
  const Tables savedTab = g_tab;
#endif
  const uint32_t savedGen = g_entryGen;
  g_tab = Tables();
  AllocTables(g_tab);
  const uint64_t full0 = g_tableFull.load();
  uint32_t peakKeys = 0, peakMeshes = 0;
  for (int mi = 0; mi < kMissions; ++mi) {
    for (int step = 0; step < kLength; step += kStep) {
      g_entryGen += kStep;
      if (step == 0)  // the mission loads: every object probed
        for (int i = 0; i < kMeshesPer; ++i) ProcessProbe(g_tab, In(mi, i), st, f, c, g_entryGen);
#ifdef QV_LONG_SESSION_FIXES
      if (step % 500 == 0)  // the jobs' lookups of this mission's objects
        for (int i = 0; i < kMeshesPer; ++i) {
          const ProbeIn in = In(mi, i);
          if (const KeyEntry* k = FindKey(g_tab, in.shader, 1, 0, eff, tb)) Touch(k->lastUse, g_entryGen);
          if (const MeshEntry* m = FindMesh(g_tab, in.mesh, in.shader, 1, eff, tb)) Touch(m->lastUse, g_entryGen);
        }
      MaintainTables();  // the render entry's maintenance
#endif
      peakKeys = (std::max)(peakKeys, g_tab.keysUsed);
      peakMeshes = (std::max)(peakMeshes, g_tab.meshesUsed);
    }
  }
  int found = 0, gone = 0;
  for (int i = 0; i < kMeshesPer; ++i) {
    const ProbeIn a = In(kMissions - 1, i), b = In(0, i);
    const MeshEntry* m = FindMesh(g_tab, a.mesh, a.shader, 1, eff, tb);
    found += m && m->state.load() > 0 && m->il == c.il && m->indexCount == 3 * w->tris[0];
    gone += !FindMesh(g_tab, b.mesh, b.shader, 1, eff, tb);
  }
  printf("     long session (shadow): peak keys %u of %zu, meshes %u of %zu; table full %llu\n", peakKeys, kKeyTable,
         peakMeshes, kMeshTable, static_cast<unsigned long long>(g_tableFull.load() - full0));
  Check(g_tableFull.load() == full0,
        "long session (shadow): six missions never fill the key or mesh table (was: table full from mission 5)");
  Check(found == kMeshesPer && gone == kMeshesPer && peakKeys < kKeyTable * 3 / 4 && peakMeshes < kMeshTable * 3 / 4,
        "long session (shadow): the current mission's entries are all there, unchanged; the first mission's are gone");
#ifdef QV_LONG_SESSION_FIXES
  MaintainTables();  // nothing armed: the last replaced set is freed now
  const uint64_t comps = g_compactions.load() - comp0;
  Check(comps > 0 && g_oldFreed.load() - freed0 == comps && !g_tabOld.keys && !g_tabOld.meshes,
        "long session (shadow): compactions ran and every replaced table set was freed");
  // A job armed with the current tables keeps them alive across a compaction.
  Job* j = NewJob();
  g_casc[0].job = j;
  j->tabSnap = g_tab;
  j->tab = &j->tabSnap;
  g_casc[0].armed = true;
  const KeyEntry* held = FindKey(g_tab, ShaderAt(kMissions - 1, 3), 1, 0, eff, tb);
  g_plan.nextMeshes = 0;  // due now
  MaintainTables();
  const bool swapped = g_tabOld.keys == j->tabSnap.keys && g_tab.keys != j->tabSnap.keys;
  MaintainTables();
  MaintainTables();
  const bool kept = g_tabOld.keys == j->tabSnap.keys && held && held->state.load() == 1 &&
                    FindKey(j->tabSnap, ShaderAt(kMissions - 1, 3), 1, 0, eff, tb) == held;
  const uint64_t compsArmed = g_compactions.load();
  g_plan.nextMeshes = 0;
  MaintainTables();  // due, but the old set is still in use: no second old set
  const bool single = g_compactions.load() == compsArmed;
  g_casc[0].armed = false;  // consumed and not re-armed
  const uint64_t freedAt = g_oldFreed.load();
  MaintainTables();  // frees the job's set, then runs the compaction that was due meanwhile
  const bool freed = g_oldFreed.load() == freedAt + 1 && g_tabOld.keys != j->tabSnap.keys &&
                     g_tabOld.meshes != j->tabSnap.meshes;
  g_casc[0].job = nullptr;
  FreeJob(j);
  Check(swapped && kept && single && freed,
        "long session (shadow): replaced tables stay alive while an armed job's snapshot uses them, then are freed");
  FreeTables(g_tab);
  FreeTables(g_tabOld);
  g_tab = savedTab;
  g_tabOld = savedOld;
  g_plan = savedPlan;
#else
  FreeTables(g_tab);
  g_tab = savedTab;
#endif
  g_entryGen = savedGen;
  Check(pr.vs.refs == 1 && pr.il.refs == 1,
        "long session (shadow): copies and frees balance every reference (no leak, no extra release)");
  delete w;
}

// ---------------------------------------------------------------------------
// G-buffer recorder
// ---------------------------------------------------------------------------
void GbReuse() {
  using namespace gbrec;
  grtest::World* w = new grtest::World;
  w->Init();
  Tables t;
  AllocTables(t);
  TexTable tt;
  AllocTex(tt, 256);
  FakeView views[grtest::kTex], pages[grtest::kPages], il;
  for (int k = 0; k < grtest::kTex; ++k) w->SetView(k, &views[k]);
  for (int p = 0; p < grtest::kPages; ++p) w->SetPageSrv(p, &pages[p]);
  grtest::PubAll(t, *w);
  grtest::FillTex(tt, *w);
  Job* j = NewJob();
  grtest::InitJob(*j, *w, t, tt);
  j->useGen = 100;
  BuildJob(*j);
  constexpr int kM = 1;
  std::vector<uint8_t> base(grtest::kN);
  int onMesh = 0;
  for (int i = 0; i < grtest::kN; ++i) {
    base[i] = j->items[i].reason;
    onMesh += base[i] == kRecorded && grtest::World::MeshOf(i) == kM;
  }
  w->SetMeshBuffers(kM, reinterpret_cast<void*>(uintptr_t{0x5000}), reinterpret_cast<void*>(uintptr_t{0x6000}));
  *reinterpret_cast<uint32_t*>(w->mesh[kM] + 0x210) = 41;
  j->useGen = 200;
  BuildJob(*j);
  int changed = 0, wanted = 0, drawnOld = 0;
  for (int i = 0; i < grtest::kN; ++i) {
    if (grtest::World::MeshOf(i) != kM || base[i] != kRecorded) continue;
    changed += j->items[i].reason == kRMeshChanged;
    wanted += j->items[i].want != 0;
    drawnOld += j->items[i].reason == kRecorded;
  }
  Check(onMesh > 0 && changed == onMesh && drawnOld == 0,
        "long session (gbuffer): a new mesh at a reused address is residual (never drawn from the old entry)");
  Check(wanted >= 1, "long session (gbuffer): a reused address asks for a probe (was: never relearnt)");
#ifdef QV_LONG_SESSION_FIXES
  // What the probe publishes (gbrec::ProcessProbe's mesh step).
  shrec::MeshLive live;
  const char* lw = shrec::ReadMesh(w->mesh[kM], live);
  int relearnt = 0;
  for (int s = 0; s < grtest::kShaders; ++s)
    relearnt += shrec::PublishProbedMesh(t.mesh, w->mesh[kM], w->shader[s], 1,
                                         *reinterpret_cast<void**>(w->shader[s] + 0x50),
                                         *reinterpret_cast<void**>(w->shader[s] + 0xb0), live, lw, nullptr,
                                         reinterpret_cast<ID3D11InputLayout*>(static_cast<IUnknown*>(&il)),
                                         210) == shrec::kPmRelearnt;
  j->useGen = 220;
  BuildJob(*j);
  bool same = true;
  for (int i = 0; i < grtest::kN; ++i) same = same && j->items[i].reason == base[i];
  bool counts = true;
  for (uint32_t k = 0; k < j->candCount; ++k)
    if (grtest::World::MeshOf(static_cast<int>(j->cands[k].item)) == kM) counts = counts && j->cands[k].mesh->indexCount == 3 * 41;
  Check(relearnt == grtest::kShaders && same && counts,
        "long session (gbuffer): after the probe every item is recorded as before, with the new mesh's count");
#endif
  FreeJob(j);
  FreeTex(tt);
  FreeTables(t);
  Check(il.refs == 1, "long session (gbuffer): mesh references balanced");
  delete w;
}

#ifdef QV_LONG_SESSION_FIXES
void GbCompaction() {
  using namespace gbrec;
  FakeView vs, il;
  Tables t;
  AllocTables(t);
  auto Sh = [](int k) { return reinterpret_cast<uint8_t*>(uintptr_t{0x200000000} + uintptr_t(k) * 0x200); };
  void* const eff = reinterpret_cast<void*>(uintptr_t{0xe0});
  void* const tb = reinterpret_cast<void*>(uintptr_t{0xb0});
  const uint32_t now = 10000;
  // Keys of collections 4 and 5: young, old, retired lately, retired long ago.
  auto Key = [&](int k, uint32_t scope, uint32_t used, int state, uint32_t retiredAt) {
    GbKey* e = NewKey(t, Sh(k), 1, 0, scope);
    e->tech = 1;
    e->scope = scope;
    e->effect = eff;
    e->techBegin = tb;
    e->vs = reinterpret_cast<ID3D11VertexShader*>(static_cast<IUnknown*>(&vs));
    vs.AddRef();
    e->lastUse.store(used);
    e->retiredAt = retiredAt;
    e->state.store(state);
    PublishKey(t, *e, Sh(k));
  };
  Key(0, 4, now - 10, 1, 0);
  Key(0, 5, now - 10, 1, 0);
  Key(1, 4, now - 5000, 1, 0);
  Key(2, 4, now - 5000, kRetired, now - 100);
  Key(3, 4, now - 5000, kRetired, now - 2000);
  auto Reads = [&](int k, uint32_t used) {
    ReadsEntry* e = NewReads(t, Sh(k), 1);
    e->tech = 1;
    e->effect = eff;
    e->techBegin = tb;
    e->state = 1;
    e->lastUse.store(used);
    PublishReads(t, *e, Sh(k));
  };
  Reads(0, now);
  Reads(1, now - 5000);
  shrec::MeshLive live;
  live.fp = 0x1234;
  live.count = 9;
  auto Mesh = [&](int k, uint32_t publishAt) {
    return shrec::PublishProbedMesh(t.mesh, reinterpret_cast<void*>(uintptr_t{0x700000} + k * 0x220), Sh(0), 1, eff,
                                    tb, live, nullptr, nullptr,
                                    reinterpret_cast<ID3D11InputLayout*>(static_cast<IUnknown*>(&il)), publishAt);
  };
  Mesh(0, now);
  Mesh(1, now - 5000);
  live.fp = 0x5678;  // mesh 0's address reused: the old entry superseded at now - 50
  const int re = Mesh(0, now - 50);
  Tables n;
  AllocTables(n);
  shrec::CompactStats cs;
  CompactInto(t, n, now, shrec::kEvictAge, cs);
  const GbKey* k04 = FindKey(n, Sh(0), 1, 0, eff, tb, 4);
  const bool keys = k04 && k04->vs == reinterpret_cast<ID3D11VertexShader*>(static_cast<IUnknown*>(&vs)) &&
                    FindKey(n, Sh(0), 1, 0, eff, tb, 5) && !FindKey(n, Sh(1), 1, 0, eff, tb, 4) &&
                    KeyCooling(n, Sh(2), 1, 0, eff, tb, 4, now, false) && !KeyCooling(n, Sh(3), 1, 0, eff, tb, 4, now, true);
  const bool reads = FindReads(n, Sh(0), 1, eff, tb) && !FindReads(n, Sh(1), 1, eff, tb);
  const shrec::MeshEntry* m0 = shrec::FindMesh(n.mesh, reinterpret_cast<void*>(uintptr_t{0x700000}), Sh(0), 1, eff, tb);
  const bool meshes = re == shrec::kPmRelearnt && m0 && m0->fp == 0x5678 &&
                      shrec::MeshCooling(n.mesh, reinterpret_cast<void*>(uintptr_t{0x700000}), Sh(0), 1, eff, tb, now) &&
                      !shrec::FindMesh(n.mesh, reinterpret_cast<void*>(uintptr_t{0x700000 + 0x220}), Sh(0), 1, eff, tb);
  Check(keys && reads && meshes && cs.keysIn == 5 && cs.keysOut == 3 && cs.readsOut == 1 && cs.meshesIn == 3 &&
            cs.meshesOut == 2 && n.keysUsed == 3 && n.readsUsed == 1 && n.mesh.meshesUsed == 2,
        "long session (gbuffer): compaction keeps the keys per collection, read sets and meshes in use, the retired "
        "and superseded ones while they cool down, and drops the rest");
  FreeTables(t);
  const bool own = vs.refs == 1 + 3 && il.refs == 1 + 2;  // only the copies' references remain
  FreeTables(n);
  Check(own && vs.refs == 1 && il.refs == 1, "long session (gbuffer): each table set owns and releases its references");
  // The production maintenance: due at half the mesh table, the old set freed once no slot's job uses it.
  const Tables savedTab = g_tab, savedOld = g_tabOld;
  const shrec::CompactPlan savedPlan = g_plan;
  const uint32_t savedGen = g_entryGen;
  Job* savedJobs[kSlots];
  bool savedArmed[kSlots];
  for (int s = 0; s < kSlots; ++s) {
    savedJobs[s] = g_slot[s].job;
    savedArmed[s] = g_slot[s].armed;
    g_slot[s].job = nullptr;
    g_slot[s].armed = false;
  }
  g_tab = Tables();
  g_tabOld = Tables();
  g_plan = shrec::CompactPlan();
  AllocTables(g_tab);
  g_entryGen = 1000;
  const uint64_t full0 = g_tableFull.load();
  const uint64_t comp0 = g_compactions.load();
  bool bounded = true;
  for (int mi = 0; mi < 5; ++mi) {
    for (int i = 0; i < 6000; ++i) {
      live.fp = 0x9000 + i;
      shrec::PublishProbedMesh(g_tab.mesh, reinterpret_cast<void*>(uintptr_t{0x800000000} + (uintptr_t(mi) * 6000 + i) * 0x220),
                               Sh(0), 1, eff, tb, live, nullptr, nullptr, nullptr, g_entryGen);
      if (i % 100 == 0) {
        g_entryGen += 40;
        MaintainTables();
      }
    }
    g_entryGen += 4000;  // the mission is flown (its entries are looked up by the jobs only during it)
    MaintainTables();
    bounded = bounded && g_tab.mesh.meshesUsed < shrec::kMeshTable * 3 / 4;
  }
  Job* j = NewJob();
  g_slot[2].job = j;
  j->tabSnap = g_tab;
  g_slot[2].armed = true;
  g_plan.nextMeshes = 0;
  MaintainTables();
  MaintainTables();
  const bool held = g_tabOld.mesh.meshes == j->tabSnap.mesh.meshes;
  g_slot[2].armed = false;
  MaintainTables();
  const bool freed = !g_tabOld.mesh.meshes && !g_tabOld.keys;
  g_slot[2].job = nullptr;
  FreeJob(j);
  Check(bounded && g_tableFull.load() == full0 && g_compactions.load() > comp0 && held && freed,
        "long session (gbuffer): five missions stay bounded (no table full); replaced tables live while a slot's "
        "armed job uses them");
  FreeTables(g_tab);
  FreeTables(g_tabOld);
  g_tab = savedTab;
  g_tabOld = savedOld;
  g_plan = savedPlan;
  g_entryGen = savedGen;
  for (int s = 0; s < kSlots; ++s) {
    g_slot[s].job = savedJobs[s];
    g_slot[s].armed = savedArmed[s];
  }
}
#endif

// ---------------------------------------------------------------------------
// Cost weights
// ---------------------------------------------------------------------------
void __fastcall FakeCollect(void*, uint32_t, void*, void*, void*, void*) {}
float __fastcall FakeWeight(void* self) { return *reinterpret_cast<float*>(static_cast<uint8_t*>(self) + 0x30); }

void CostWeights() {
  using namespace partw;
  const Fn c0 = g_targets[0].collect;
  const GetterFn g0 = g_targets[0].getter;
  uint8_t* const lo0 = g_sceneLo;
  uint8_t* const hi0 = g_sceneHi;
  g_targets[0].collect = &FakeCollect;
  g_targets[0].getter = &FakeWeight;
  g_sceneLo = nullptr;
  g_sceneHi = reinterpret_cast<uint8_t*>(~uintptr_t{0});
  for (Slot& s : g_table) {
    s.obj.store(nullptr);
    s.cost.store(0);
    s.orig.store(0);
  }
#ifdef QV_LONG_SESSION_FIXES
  const uint32_t age0 = g_evictAge.load();
  g_evictAge = 40;
  for (Slot& s : g_table) s.seen.store(0);
  g_live = 0;
  g_tombs = 0;
#endif
  // Three missions of 12000 objects each (36000 over the session, the table holds 32768).
  constexpr int kPer = 12000, kMissions = 3, kFrames = 320;
  std::vector<uint8_t> mem(size_t(kPer) * kMissions * 64 + 64);
  uint8_t* base = reinterpret_cast<uint8_t*>((reinterpret_cast<uintptr_t>(mem.data()) + 63) & ~uintptr_t{63});
  auto Obj = [&](int mission, int i) { return base + (size_t(mission) * kPer + i) * 64; };
  const float weightOf[kMissions] = {100.f, 10.f, 1.f};
  for (int mi = 0; mi < kMissions; ++mi)
    for (int i = 0; i < kPer; ++i) *reinterpret_cast<float*>(Obj(mi, i) + 0x30) = weightOf[mi];
  for (int mi = 0; mi < kMissions; ++mi)
    for (int fr = 0; fr < kFrames; ++fr) {
      Apply(nullptr);  // before the collect call
      if (fr % 4) continue;
      for (int i = 0; i < kPer; ++i) {  // the collect: Scene asks each entry's weight, the pool culls each object
        Getter<0>(Obj(mi, i));
        Hook<0>(Obj(mi, i), 0, nullptr, nullptr, nullptr, nullptr);
      }
    }
  int measured = 0;
  double w = 0, c = 0;
  for (int i = 0; i < kPer; ++i)
    if (Slot* s = Find(Obj(kMissions - 1, i), false)) {
      ++measured;
      w += s->orig.load();
      c += s->cost.load();
    }
  Apply(nullptr);
  const double want = c > 0 ? w / c : 0, got = g_scale.load();
  printf("     long session (cost weights): last mission measured %d of %d; scale %.6g, its own objects give %.6g\n",
         measured, kPer, got, want);
  Check(measured == kPer,
        "long session (cost weights): a later mission's objects are all measured (was: the table only filled)");
  Check(want > 0 && std::fabs(got - want) <= 0.01 * want,
        "long session (cost weights): the scale follows the objects in play, not the departed ones");
#ifdef QV_LONG_SESSION_FIXES
  Check(g_evicted.load() > 0 && g_live.load() <= kTable, "long session (cost weights): departed objects evicted");
  g_evictAge = age0;
  for (Slot& s : g_table) {
    s.obj.store(nullptr);
    s.cost.store(0);
    s.orig.store(0);
    s.seen.store(0);
  }
  g_live = 0;
  g_tombs = 0;
#else
  for (Slot& s : g_table) {
    s.obj.store(nullptr);
    s.cost.store(0);
    s.orig.store(0);
  }
#endif
  g_targets[0].collect = c0;
  g_targets[0].getter = g0;
  g_sceneLo = lo0;
  g_sceneHi = hi0;
  g_scale = 0.f;
}

// ---------------------------------------------------------------------------
// Big model pages across a payload hot reload
// ---------------------------------------------------------------------------
void BigPagesReload() {
#ifdef QV_LONG_SESSION_FIXES
  using namespace bigpages;
  alignas(16) static uint8_t insn[5] = {0xB8, 0x00, 0x00, 0x40, 0x00};  // the constant as a payload left it (4 MB)
  uint8_t* const insn0 = g_insn;
  const bool want0 = g_wanted.load(), applied0 = g_applied;
  const uint32_t big0 = g_bigBytes;
  g_insn = insn;
  g_bigBytes = 4u << 20;
  // The manager's page vector: small pages, one big page.
  constexpr uint32_t kPages = 40;
  std::vector<uint8_t> pages(kPages * allocslab::kPageStride, 0);
  for (uint32_t i = 0; i < kPages; ++i) {
    auto* pg = reinterpret_cast<allocslab::Page*>(pages.data() + i * allocslab::kPageStride);
    pg->idx = i;
    pg->elemSize = i & 1 ? 96 : 112;
    pg->capCount = 0xfe00 / pg->elemSize;
    pg->capBytes = i == 7 ? g_bigBytes : pg->capCount * pg->elemSize;
  }
  std::vector<uint32_t> cap0(kPages);
  for (uint32_t i = 0; i < kPages; ++i)
    cap0[i] = reinterpret_cast<allocslab::Page*>(pages.data() + i * allocslab::kPageStride)->capBytes;
  alignas(16) uint8_t mgr[0x40] = {};
  *reinterpret_cast<uint8_t**>(mgr + 8) = pages.data();
  *reinterpret_cast<uint8_t**>(mgr + 0x10) = pages.data() + pages.size();
  SharedRetired* sh = SharedList();
  const uint32_t count0 = sh ? sh->count : 0;
  // Payload 1: big pages on: the small pages retired.
  g_applied = true;
  g_wanted = true;
  OnReset(mgr);
  uint32_t retired = 0;
  for (uint32_t i = 0; i < kPages; ++i)
    retired += reinterpret_cast<allocslab::Page*>(pages.data() + i * allocslab::kPageStride)->capBytes == 0;
  // Hot reload: payload 2 starts with its own statics (the old module stays loaded, never called again).
  g_shared = nullptr;
  g_retiredCount = 0;
  g_applied = true;  // Install: the constant is still patched
  // Payload 2 turns big pages off: the pages payload 1 retired come back.
  g_wanted = false;
  OnReset(mgr);
  bool restored = true;
  for (uint32_t i = 0; i < kPages; ++i)
    restored = restored &&
               reinterpret_cast<allocslab::Page*>(pages.data() + i * allocslab::kPageStride)->capBytes == cap0[i];
  const uint32_t stock = (static_cast<uint32_t>(insn[1]) | static_cast<uint32_t>(insn[2]) << 8 |
                          static_cast<uint32_t>(insn[3]) << 16 | static_cast<uint32_t>(insn[4]) << 24);
  Check(sh && count0 == 0 && retired == kPages - 1 && restored && stock == kStockBytes && !g_applied &&
            SharedList() && SharedList()->count == 0,
        "long session (big pages): pages retired by one payload are restored by the next one (was: lost on reload)");
  g_insn = insn0;
  g_wanted = want0;
  g_applied = applied0;
  g_bigBytes = big0;
#else
  QvSkip("long session (big pages): the retired list is the payload's own (no offline hook before the fix)\n");
#endif
}

// ---------------------------------------------------------------------------
// The perf line's "(dcs N%)": D3DKMT per-process statistics
// ---------------------------------------------------------------------------
// GPU work on our own device (clears and copies of two 2048^2 textures, about `ms`).
struct GpuWork {
  ID3D11Device* dev = nullptr;
  ID3D11DeviceContext* ctx = nullptr;
  ID3D11Texture2D *a = nullptr, *b = nullptr;
  ID3D11RenderTargetView* rtv = nullptr;
  bool Init() {
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &dev,
                                 nullptr, &ctx)))
      return false;
    D3D11_TEXTURE2D_DESC td{};
    td.Width = td.Height = 2048;
    td.MipLevels = td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    td.SampleDesc.Count = 1;
    td.BindFlags = D3D11_BIND_RENDER_TARGET;
    return SUCCEEDED(dev->CreateTexture2D(&td, nullptr, &a)) && SUCCEEDED(dev->CreateTexture2D(&td, nullptr, &b)) &&
           SUCCEEDED(dev->CreateRenderTargetView(a, nullptr, &rtv));
  }
  void Run(DWORD ms) {
    const ULONGLONG t0 = GetTickCount64();
    for (int k = 0; GetTickCount64() - t0 < ms; ++k) {
      const float c[4] = {float(k & 1), 0, 0, 1};
      ctx->ClearRenderTargetView(rtv, c);
      ctx->CopyResource(b, a);
      if (k % 8 == 7) ctx->Flush();
    }
    ctx->Flush();
  }
  ~GpuWork() {
    if (rtv) rtv->Release();
    if (a) a->Release();
    if (b) b->Release();
    if (ctx) ctx->Release();
    if (dev) dev->Release();
  }
};

#ifdef QV_LONG_SESSION_FIXES
// D3DKMTQueryStatistics, but process queries through the real handle fail as
// the field saw them (the pseudo handle still answers).
perfmon::Kmt::QsFn g_realQs = nullptr;
NTSTATUS APIENTRY QsRealHandleFails(const D3DKMT_QUERYSTATISTICS* q) {
  if (q->Type == D3DKMT_QUERYSTATISTICS_PROCESS_NODE && q->hProcess != GetCurrentProcess())
    return static_cast<NTSTATUS>(0xC000000DL);
  return g_realQs(q);
}
#endif

void DcsGpuShare() {
  GpuWork g;
  if (!g.Init()) {
    QvSkip("long session (gpu share): no hardware D3D11 device\n");
    return;
  }
  IDXGIDevice* dx = nullptr;
  IDXGIAdapter* ad = nullptr;
  if (SUCCEEDED(g.dev->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&dx))) && dx) {
    dx->GetAdapter(&ad);
    dx->Release();
  }
  if (!ad) {
    QvSkip("long session (gpu share): no adapter\n");
    return;
  }
  perfmon::SetAdapter(ad);
  ad->Release();
  if (!perfmon::g_ad.haveDesc || !perfmon::KmtOpen(perfmon::g_ad.desc.AdapterLuid)) {
    QvSkip("long session (gpu share): D3DKMT statistics not available\n");
    perfmon::Shutdown();
    perfmon::g_kmt = perfmon::Kmt();
    perfmon::g_ad = perfmon::Adapter();
    return;
  }
  double a = -1, p = -1;
  g.Run(100);
  perfmon::KmtSample(&a, &p);  // primes
  g.Run(300);
  const bool ok = perfmon::KmtSample(&a, &p);
  printf("     long session (gpu share): adapter %.0f%%, this process %.0f%%\n", a, p);
  if (ok && p < 0 && perfmon::g_kmt.procStatus != 0) {
    // The driver does not answer this process's query (STATUS_INVALID_PARAMETER: no device of this process on
    // the adapter it reports, or a sandboxed test host): nothing to verify here.
    QvSkip("long session (gpu share): the driver does not answer the per-process query (NTSTATUS 0x%08lx)\n",
           static_cast<unsigned long>(perfmon::g_kmt.procStatus));
    perfmon::Shutdown();
    perfmon::g_kmt = perfmon::Kmt();
    perfmon::g_ad = perfmon::Adapter();
    return;
  }
  Check(ok && p >= 0 && p <= 100,
        "long session (gpu share): this process's share answers (was: n/a, the LIMITED handle's query fails)");
#ifdef QV_LONG_SESSION_FIXES
  // The handle kind in use stops answering: replaced after kKmtRetry samples.
  g_realQs = perfmon::g_kmt.qs;
  perfmon::g_kmt.qs = &QsRealHandleFails;
  const int kind0 = perfmon::g_kmt.selfKind;
  int naRun = 0;
  for (int k = 0; k < perfmon::kKmtRetry; ++k) {
    g.Run(60);
    perfmon::KmtSample(&a, &p);
    naRun += p < 0;
  }
  g.Run(60);
  perfmon::KmtSample(&a, &p);  // primes the new handle's counters
  g.Run(200);
  const bool back = perfmon::KmtSample(&a, &p);
  Check(kind0 == 0 && naRun == perfmon::kKmtRetry && perfmon::g_kmt.selfKind == 1 && perfmon::g_kmt.switches == 1 &&
            back && p >= 0,
        "long session (gpu share): a handle whose process queries fail is replaced, the share recovers");
  perfmon::g_kmt.qs = g_realQs;
#endif
  perfmon::Shutdown();
  perfmon::g_kmt = perfmon::Kmt();
  perfmon::g_ad = perfmon::Adapter();
}

void Run() {
  ShadowReuse();
  ShadowMissions();
  GbReuse();
#ifdef QV_LONG_SESSION_FIXES
  GbCompaction();
#endif
  CostWeights();
  BigPagesReload();
  DcsGpuShare();
}

}  // namespace lstest
