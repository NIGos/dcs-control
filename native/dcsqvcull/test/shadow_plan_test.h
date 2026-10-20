// Offline tests for shadow_batch.h's planner threads ([Model] ShadowPlanAsync):
// the plan identity check and async (planner thread) / inline (render thread)
// equivalence, on fake caster vectors.
#pragma once

#include <array>

namespace sptest {
using shadowbatch::Plan;

constexpr int kSh = 6, kMats = 24, kMeshes = 6, kObjs = 600, kCasc = 4, kDescs = 6;

struct World {
  std::vector<uint8_t> smrVt = std::vector<uint8_t>(64), otherVt = std::vector<uint8_t>(64),
                       matVt = std::vector<uint8_t>(64), shVt = std::vector<uint8_t>(64);
  std::vector<std::vector<uint8_t>> sh, mat, props, item, rend;
  std::vector<std::array<uint64_t, 6>> entries;  // per object: 2 texture entries of 0x18 bytes
  std::vector<uint8_t*> arrPtr;                  // per object: [item+0x18] points here
  std::vector<std::vector<void*>> vec;           // cascade caster vectors
  std::vector<void*> moved;                      // a copy of a vector elsewhere (begin changes)
  std::vector<uint8_t> descs = std::vector<uint8_t>(kDescs * 24);
  std::vector<uint8_t> rg = std::vector<uint8_t>(0x1000), rg2 = std::vector<uint8_t>(0x1000);
  void* ctx[1] = {};

  void** Desc(int c) { return reinterpret_cast<void**>(descs.data() + (c + 1) * 24); }
  void SetVec(int c, void** b, size_t n) {
    Desc(c)[0] = b;
    Desc(c)[1] = b + n;
    Desc(c)[2] = b + n;
  }
  void ResetVecs() {
    for (int c = 0; c < kCasc; ++c) SetVec(c, vec[c].data(), vec[c].size());
  }

  void Init() {
    sh.assign(kSh, std::vector<uint8_t>(0x100));
    mat.assign(kMats, std::vector<uint8_t>(0x400));
    props.assign(kMats, std::vector<uint8_t>(0x300));
    item.assign(kObjs, std::vector<uint8_t>(0x100));
    rend.assign(kObjs, std::vector<uint8_t>(0x80));
    entries.assign(kObjs, {});
    arrPtr.assign(kObjs, nullptr);
    for (int s = 0; s < kSh; ++s) {
      *reinterpret_cast<void**>(sh[s].data()) = shVt.data();
      *reinterpret_cast<uintptr_t*>(sh[s].data() + 0x50) = 0xeff0 + s;
      *reinterpret_cast<uintptr_t*>(sh[s].data() + 0xb0) = 0x7ec0 + s;
      *reinterpret_cast<uintptr_t*>(sh[s].data() + 0xc8) = 0x9000 + s * 0x100;
      *reinterpret_cast<uintptr_t*>(sh[s].data() + 0xd0) = 0x9000 + s * 0x100 + 2 * 0x50;
    }
    for (int m = 0; m < kMats; ++m) {
      uint8_t* mt = mat[m].data();
      *reinterpret_cast<void**>(mt) = m == 23 ? otherVt.data() : matVt.data();  // 23: another material class
      *reinterpret_cast<uint8_t**>(mt + 0x28) = props[m].data();
      *reinterpret_cast<uint8_t**>(mt + 0x30) = m == 5 ? nullptr : sh[m % kSh].data();  // 5: no shader
      *reinterpret_cast<uint64_t*>(mt + 0x210) = 1;
      *reinterpret_cast<uint64_t*>(mt + 0x218) = 2;
      *reinterpret_cast<uint32_t*>(mt + 0x2d8) = m == 7 ? 40 : 2;  // 7: too many textures
      *reinterpret_cast<int64_t*>(mt + 0x240) = 0;
      *reinterpret_cast<int64_t*>(mt + 0x248) = 1;
      props[m][0x33] = m % 4 == 0 ? 1 : 0;                          // technique from mat+0x218
      *reinterpret_cast<uint32_t*>(props[m].data() + 8) = m % 3 == 0 ? 1 : 0;  // textured
    }
    for (int i = 0; i < kObjs; ++i) {
      uint8_t* it = item[i].data();
      *reinterpret_cast<uint8_t**>(it + 0x10) = mat[i % kMats].data();
      entries[i] = {0xA0, 0xB0u + i % 2, 0, 0xC0, 0xD0u + i % 5, 0};
      arrPtr[i] = reinterpret_cast<uint8_t*>(entries[i].data());
      *reinterpret_cast<uint8_t***>(it + 0x18) = &arrPtr[i];
      *reinterpret_cast<uintptr_t*>(it + 0xc0) = 0x1000 + (i / kMats) % kMeshes;
      *reinterpret_cast<uint32_t*>(it + 0xd0) = (i / 7) % 2;
      *reinterpret_cast<uint32_t*>(it + 0xd4) = 1000 + i;
      *reinterpret_cast<void**>(rend[i].data()) = i % 37 == 0 ? otherVt.data() : smrVt.data();
      *reinterpret_cast<uint8_t**>(rend[i].data() + 0x10) = it;
    }
    // Cascades: shuffled subsets (deterministic), each object at most once.
    uint64_t seed = 0x1234567;
    auto rnd = [&]() {
      seed = seed * 6364136223846793005ull + 1442695040888963407ull;
      return static_cast<uint32_t>(seed >> 33);
    };
    vec.assign(kCasc, {});
    for (int c = 0; c < kCasc; ++c) {
      for (int i = 0; i < kObjs; ++i)
        if (static_cast<int>(rnd() % 100) < 70 - c * 10) vec[c].push_back(rend[i].data());
      for (size_t i = vec[c].size(); i > 1; --i) std::swap(vec[c][i - 1], vec[c][rnd() % i]);
    }
    ResetVecs();
    *reinterpret_cast<uint8_t**>(rg.data() + 0x438) = descs.data();
    *reinterpret_cast<uint8_t**>(rg.data() + 0xa18) = descs.data();
    *reinterpret_cast<uint8_t**>(rg.data() + 0xa20) = descs.data() + descs.size();
    *reinterpret_cast<uint8_t**>(rg2.data() + 0xa18) = descs.data();
    *reinterpret_cast<uint8_t**>(rg2.data() + 0xa20) = descs.data() + descs.size();
    ctx[0] = rg.data();
  }
};

const void* __fastcall FGetTex(const void*, uint32_t) { return nullptr; }
bool __fastcall FValid(const void*) { return false; }

void Publish(uint8_t* shader, uint64_t tech) {
  using namespace shadowinst;
  size_t i = MapHash(shader, tech, 0);
  while (g_map[i].shader.load()) i = (i + 1) & (kMapSize - 1);
  MapEntry& e = g_map[i];
  e.tech = tech;
  e.pass = 0;
  e.effect = *reinterpret_cast<void**>(shader + 0x50);
  e.techBegin = *reinterpret_cast<void**>(shader + 0xb0);
  e.vs.store(reinterpret_cast<ID3D11VertexShader*>(uintptr_t{0x5000}));
  e.shader.store(shader);
}

bool SamePlan(const Plan& a, const Plan& b) {
  if (a.result != b.result || a.n != b.n || a.offsetCount != b.offsetCount || a.groups != b.groups ||
      a.leaders != b.leaders || a.skipped != b.skipped || a.matCount != b.matCount || a.groupCount != b.groupCount)
    return false;
  for (size_t i = 0; i < a.n; ++i) {
    const shadowbatch::Slot &x = a.slots[i], &y = b.slots[i];
    if (x.r != y.r || x.role != y.role || x.mask != y.mask) return false;
    if (x.role != shadowbatch::kSolo && (x.base != y.base || x.count != y.count || x.group != y.group)) return false;
  }
  for (uint32_t k = 0; k < a.offsetCount; ++k)
    if (a.order[k] != b.order[k]) return false;
  for (uint32_t i = 0; i < a.groupCount; ++i) {
    if (a.groupList[i] != b.groupList[i]) return false;
    const shadowbatch::GroupSlot &x = a.gslots[a.groupList[i]], &y = b.gslots[b.groupList[i]];
    if (x.mat != y.mat || x.mesh != y.mesh || x.page != y.page || x.tex != y.tex || x.first != y.first ||
        x.last != y.last || x.count != y.count || x.mask != y.mask)
      return false;
    if (x.count >= 2 && a.groupFailed[a.groupList[i]] != b.groupFailed[b.groupList[i]]) return false;
  }
  for (uint32_t i = 0; i < a.matCount; ++i) {
    if (a.matList[i] != b.matList[i]) return false;
    const shadowbatch::MatLast &x = a.mats[a.matList[i]], &y = b.mats[b.matList[i]];
    if (x.mat != y.mat || x.pso != y.pso || x.last != y.last || x.eligible != y.eligible ||
        x.textured != y.textured || x.mask != y.mask)
      return false;
  }
  return true;
}

// The inline (render thread) plan of `vec` now, with its pso values and offsets.
void Reference(Plan& ref, void** vec, std::vector<uint32_t>* offs) {
  ref.vec = vec;
  ref.result = shadowbatch::BuildGuarded(ref, true);
  shadowbatch::FillPso(ref);
  offs->assign(ref.offsetCount, 0);
  shadowbatch::GatherOffsets(ref, offs->data());
}

bool WaitDone(int count) {
  // A deadline, not a spin count: planner threads on a loaded machine are slow.
  for (const ULONGLONG until = GetTickCount64() + 60000;;) {
    int done = 0;
    for (int k = 0; k < count; ++k) done += shadowbatch::g_learned[k].plan->state.load() == shadowbatch::kDone;
    if (done == count) return true;
    if (GetTickCount64() >= until) return false;
    Sleep(1);
  }
}

std::vector<uint32_t> ReadBuffer(ID3D11Device* dev, ID3D11DeviceContext* ctx, uint32_t count) {
  std::vector<uint32_t> out;
  D3D11_BUFFER_DESC bd = {};
  bd.ByteWidth = shadowbatch::kMaxOffsets * sizeof(uint32_t);
  bd.Usage = D3D11_USAGE_STAGING;
  bd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  ID3D11Buffer* st = nullptr;
  if (FAILED(dev->CreateBuffer(&bd, nullptr, &st))) return out;
  ctx->CopyResource(st, shadowbatch::g_buf);
  D3D11_MAPPED_SUBRESOURCE m;
  if (SUCCEEDED(ctx->Map(st, 0, D3D11_MAP_READ, 0, &m))) {
    out.assign(static_cast<const uint32_t*>(m.pData), static_cast<const uint32_t*>(m.pData) + count);
    ctx->Unmap(st, 0);
  }
  st->Release();
  return out;
}

void Run() {
  namespace sb = shadowbatch;
  ID3D11Device* dev = nullptr;
  ID3D11DeviceContext* dctx = nullptr;
  D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
  if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, &fl, 1, D3D11_SDK_VERSION, &dev, nullptr,
                               &dctx))) {
    Check(false, "shadow plan: WARP device");
    return;
  }
  static World w;  // large; built once
  w.Init();
  // Saved globals.
  void* const smr0 = sb::g_smrVtbl;
  void* const mat0 = sb::g_modelMatVtbl;
  const DWORD rt0 = sb::g_renderThread;
  auto* const getTex0 = instcount::g_getTex;
  auto* const valid0 = instcount::g_valid;
  shadowinst::MapEntry* const map0 = shadowinst::g_map;
  const uint32_t used0 = shadowinst::g_mapUsed.load();
  sb::g_smrVtbl = w.smrVt.data();
  sb::g_modelMatVtbl = w.matVt.data();
  sb::g_renderThread = GetCurrentThreadId();
  instcount::g_getTex = &FGetTex;
  instcount::g_valid = &FValid;
  shadowinst::g_map = new shadowinst::MapEntry[shadowinst::kMapSize];
  for (int s = 0; s < 4; ++s) {  // shaders 4 and 5 have no instanced VS
    Publish(w.sh[s].data(), 1);
    Publish(w.sh[s].data(), 2);
  }
  shadowinst::g_mapUsed = 8;
  sb::g_ctx = dctx;
  bool bufOk = sb::CreateOffsetBuffer(dev, sb::kMaxOffsets, &sb::g_buf, &sb::g_srv);
  sb::ResetCounters();
  sb::g_renderGen = 0;
  sb::g_async = true;
  Plan* ref = new Plan;
  std::vector<uint32_t> refOffs;

  // 1. Learn the cascades, queue at RenderGraph::render entry, planner
  //    threads build; each pass gets a plan equal to the inline one.
  const bool started = sb::StartWorkers();
  for (int c = 0; c < kCasc; ++c) sb::Learn(w.Desc(c), w.ctx);
  bool ok = started && bufOk && sb::g_learnedCount == kCasc;
  sb::QueuePlans(w.rg.data(), w.descs.data());
  ok = ok && WaitDone(kCasc);
  bool same = ok, offsOk = ok, gpuOk = false, someGroups = false;
  for (int c = 0; c < kCasc && ok; ++c) {
    Reference(*ref, w.Desc(c), &refOffs);
    Plan* p = sb::PlanForPass(w.Desc(c));
    same = same && p == sb::g_learned[c].plan && p->byWorker && SamePlan(*p, *ref);
    offsOk = offsOk && p && std::equal(refOffs.begin(), refOffs.end(), sb::g_offsets);
    someGroups = someGroups || (ref->leaders > 0 && ref->skipped > 0);
    if (c == kCasc - 1 && p) {
      const std::vector<uint32_t> gpu = ReadBuffer(dev, dctx, p->offsetCount);
      gpuOk = gpu == refOffs;
    }
    sb::ReleasePlan(p);
  }
  Check(ok && same && someGroups, "shadow plan: planner-thread plans equal the inline plans (4 cascades)");
  Check(offsOk && gpuOk, "shadow plan: offsets read on the render thread reach the buffer in plan order");
  Check(sb::g_planWorker.load() == kCasc && sb::g_planRender.load() == 0 && sb::g_planMismatch.load() == 0 &&
            sb::g_planWorkerBuilt.load() == kCasc,
        "shadow plan: counters (4 from planner threads, 0 inline, 0 mismatches)");

  // 2. Identity: a vector changed after the build -> rebuilt inline, equal to
  //    the inline plan of the changed vector.
  sb::ResetCounters();
  sb::QueuePlans(w.rg.data(), w.descs.data());
  ok = WaitDone(kCasc);
  std::swap(w.vec[0][3], w.vec[0][10]);                                    // order
  w.SetVec(1, w.vec[1].data(), w.vec[1].size() - 1);                       // size
  w.moved.assign(w.vec[2].begin(), w.vec[2].end());
  w.SetVec(2, w.moved.data(), w.moved.size());                             // begin
  bool rebuilt = ok;
  for (int c = 0; c < kCasc && ok; ++c) {
    if (c == 3) shadowinst::g_mapUsed++;  // a VS published after the build
    Reference(*ref, w.Desc(c), &refOffs);
    Plan* p = sb::PlanForPass(w.Desc(c));
    rebuilt = rebuilt && p == &sb::g_syncPlan && SamePlan(*p, *ref) &&
              std::equal(refOffs.begin(), refOffs.end(), sb::g_offsets);
    sb::ReleasePlan(p);
  }
  Check(rebuilt && sb::g_planMismatch.load() == kCasc && sb::g_planRender.load() == kCasc &&
            sb::g_planWorker.load() == 0,
        "shadow plan: changed order, size, begin or VS map -> rebuilt inline, equal to the inline plan");
  std::swap(w.vec[0][3], w.vec[0][10]);
  w.ResetVecs();

  // 3. Direct identity checks on one planner-thread build.
  Plan* q = new Plan;
  q->vec = w.Desc(0);
  q->frame = sb::g_renderGen;
  q->result = sb::BuildGuarded(*q, false);
  bool id = q->result == sb::kResBuilt && sb::Matches(*q, w.Desc(0), sb::g_renderGen);
  id = id && !sb::Matches(*q, w.Desc(1), sb::g_renderGen);       // another descriptor
  id = id && !sb::Matches(*q, w.Desc(0), sb::g_renderGen + 1);   // another render (stale plan)
  std::swap(w.vec[0][0], w.vec[0][1]);
  id = id && !sb::Matches(*q, w.Desc(0), sb::g_renderGen);       // content
  std::swap(w.vec[0][0], w.vec[0][1]);
  void* last = w.vec[0].back();
  w.vec[0].back() = w.rend[1].data();
  id = id && !sb::Matches(*q, w.Desc(0), sb::g_renderGen);       // last element replaced
  w.vec[0].back() = last;
  w.SetVec(0, w.vec[0].data(), w.vec[0].size() - 1);
  id = id && !sb::Matches(*q, w.Desc(0), sb::g_renderGen);       // size
  w.ResetVecs();
  shadowtex::g_cacheFull = true;
  id = id && !sb::Matches(*q, w.Desc(0), sb::g_renderGen);       // texture skip state
  shadowtex::g_cacheFull = false;
  shadowinst::g_mapUsed++;
  id = id && !sb::Matches(*q, w.Desc(0), sb::g_renderGen);       // VS map
  shadowinst::g_mapUsed--;
  id = id && sb::Matches(*q, w.Desc(0), sb::g_renderGen);        // all restored
  Check(id, "shadow plan: identity check (descriptor, render, content, size, texture skip state, VS map)");

  // 4. Stale plans: another render entry since the queue -> rebuilt inline.
  sb::ResetCounters();
  sb::QueuePlans(w.rg.data(), w.descs.data());
  ok = WaitDone(kCasc);
  sb::QueuePlans(w.rg2.data(), w.descs.data());  // another graph: nothing queued, render count moves on
  Reference(*ref, w.Desc(0), &refOffs);
  Plan* p = sb::PlanForPass(w.Desc(0));
  Check(ok && p == &sb::g_syncPlan && SamePlan(*p, *ref) && sb::g_planMismatch.load() == 1,
        "shadow plan: a plan from an earlier render is not used");
  sb::ReleasePlan(p);
  for (int c = 1; c < kCasc; ++c) sb::ReleasePlan(sb::g_learned[c].plan);

  // 5. Queued but not started (no planner thread free): the pass builds it.
  ok = sb::StopWorkers();
  sb::ResetCounters();
  sb::g_workersUp = true;  // pretend: queued plans stay queued
  sb::QueuePlans(w.rg.data(), w.descs.data());
  Reference(*ref, w.Desc(2), &refOffs);
  p = sb::PlanForPass(w.Desc(2));
  Check(ok && p == sb::g_learned[2].plan && !p->byWorker && SamePlan(*p, *ref) && sb::g_planStolen.load() == 1 &&
            sb::g_planRender.load() == 1,
        "shadow plan: a queued plan no planner started is built by the pass itself");
  sb::ReleasePlan(p);
  sb::g_workersUp = false;
  for (int c = 0; c < kCasc; ++c) sb::g_learned[c].plan->state = sb::kIdle;

  // R90 H1: a plan a planner thread is still building (it was preempted): the pass waits at most
  //    g_planWaitCapUs, then builds its own plan inline (equal to the reference); the old cap waited 20 ms.
  {
    const uint32_t cap0 = sb::g_planWaitCapUs.load();
    bool h1 = true;
    double msNew = 0, msOld = 0;
    for (int run = 0; run < 2; ++run) {
      sb::ResetCounters();
      sb::g_planWaitCapUs = run == 0 ? 300u : 20000u;
      sb::g_workersUp = true;  // pretend: the plan below stays in kBuilding (no planner thread runs)
      Plan* stuck = sb::g_learned[1].plan;
      stuck->vec = w.Desc(1);
      stuck->state = sb::kBuilding;
      Reference(*ref, w.Desc(1), &refOffs);
      LARGE_INTEGER a, b, f;
      QueryPerformanceFrequency(&f);
      QueryPerformanceCounter(&a);
      Plan* got = sb::PlanForPass(w.Desc(1));
      QueryPerformanceCounter(&b);
      const double ms = (b.QuadPart - a.QuadPart) * 1000.0 / f.QuadPart;
      (run == 0 ? msNew : msOld) = ms;
      h1 = h1 && got == &sb::g_syncPlan && SamePlan(*got, *ref) &&
           std::equal(refOffs.begin(), refOffs.end(), sb::g_offsets) && sb::g_planTimeouts.load() == 1 &&
           sb::g_planRender.load() == 1 && stuck->state.load() == sb::kBuilding;
      sb::ReleasePlan(got);
      stuck->state = sb::kIdle;
      sb::g_workersUp = false;
    }
    sb::g_planWaitCapUs = cap0;
    printf("     (R90 H1: stuck planner plan, pass returned after %.2f ms with the 300 us cap, %.2f ms with 20 ms)\n",
           msNew, msOld);
    Check(h1 && msNew < 10.0 && msOld >= 19.0,
          "R90 H1 shadow plan: a plan still building after the wait cap (300 us) is built inline (equal plan, one "
          "timeout); the pre-R90 cap waited 20 ms");
  }

  // 6. Texture-skip masks: planner threads use cached masks only.
  const bool texOn0 = g_shadowTexSkipOn.load();
  const int texState0 = shadowtex::g_state.load();
  void* const shVt0 = shadowtex::g_shaderVtblPtr;
  shadowtex::MaskCache* const cache0 = shadowtex::g_cache;
  g_shadowTexSkipOn = true;
  shadowtex::g_state = 1;
  shadowtex::g_shaderVtblPtr = w.shVt.data();
  shadowtex::g_cache = new shadowtex::MaskCache;
  shadowtex::g_cacheFull = false;
  q->vec = w.Desc(0);
  q->frame = sb::g_renderGen;
  bool masks = sb::BuildGuarded(*q, false) == sb::kResNeedRender;  // no mask cached yet: inline only
  shadowtex::g_cacheFull = true;  // full cache: Lookup builds nothing either
  q->result = sb::BuildGuarded(*q, false);
  Reference(*ref, w.Desc(0), &refOffs);
  sb::FillPso(*q);  // the pass reads the restore values on the render thread
  masks = masks && q->result == sb::kResBuilt && SamePlan(*q, *ref);
  shadowtex::g_cacheFull = false;
  for (int s = 0; s < kSh; ++s) {  // handle 0 read, handle 1 not read by the shadow passes
    shadowtex::MaskEntry e;
    e.effect = *reinterpret_cast<void**>(w.sh[s].data() + 0x50);
    e.recBegin = *reinterpret_cast<void**>(w.sh[s].data() + 0xc8);
    e.recEnd = *reinterpret_cast<void**>(w.sh[s].data() + 0xd0);
    e.techBegin = *reinterpret_cast<void**>(w.sh[s].data() + 0xb0);
    e.techA = 1;
    e.techB = 2;
    e.recCount = 2;
    e.state = 1;
    e.read[0] = 1;
    shadowtex::g_cache->Insert(w.sh[s].data(), e);
  }
  const uint32_t unmaskedGroups = ref->groups;
  q->result = sb::BuildGuarded(*q, false);
  Reference(*ref, w.Desc(0), &refOffs);
  sb::FillPso(*q);
  bool anyMask = false;
  for (uint32_t i = 0; i < q->matCount; ++i) anyMask = anyMask || q->mats[q->matList[i]].mask != nullptr;
  masks = masks && q->result == sb::kResBuilt && anyMask && SamePlan(*q, *ref) && ref->groups < unmaskedGroups &&
          sb::Matches(*q, w.Desc(0), sb::g_renderGen);
  shadowtex::g_cache->Invalidate(w.sh[0].data());  // a shader destroyed after the build
  masks = masks && !sb::Matches(*q, w.Desc(0), sb::g_renderGen);
  Check(masks, "shadow plan: masks from the cache only (miss -> inline), equal to the inline plan, tombstone detected");
  delete shadowtex::g_cache;
  shadowtex::g_cache = cache0;
  shadowtex::g_cacheFull = false;
  shadowtex::g_shaderVtblPtr = shVt0;
  shadowtex::g_state = texState0;
  g_shadowTexSkipOn = texOn0;

  // Cleanup.
  sb::FreePlans();
  delete q;
  delete ref;
  sb::g_async = false;
  sb::g_renderGen = 0;
  sb::ResetCounters();
  if (sb::g_srv) sb::g_srv->Release();
  if (sb::g_buf) sb::g_buf->Release();
  sb::g_srv = nullptr;
  sb::g_buf = nullptr;
  sb::g_ctx = nullptr;
  delete[] shadowinst::g_map;
  shadowinst::g_map = map0;
  shadowinst::g_mapUsed = used0;
  instcount::g_getTex = getTex0;
  instcount::g_valid = valid0;
  sb::g_renderThread = rt0;
  sb::g_modelMatVtbl = mat0;
  sb::g_smrVtbl = smr0;
  dctx->Release();
  dev->Release();
}

}  // namespace sptest
