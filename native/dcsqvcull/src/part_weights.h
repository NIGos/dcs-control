// Cost-weighted culling partition.
//
// Scene.dll splits each object collection into contiguous chunks for the job
// pool by a per-entry weight (IView+0x190 + k*0x18: {begin, end, cap}, entry
// stride 0x48, object pointer at +0x08, float weight at +0x10). The weight is
// a constant per object type (1, 15, 100, 125), so a run of heavy aircraft
// lands in one chunk and the render thread waits for it.
//
// This measures each object's real culling cost (Graphics::GraphSceneObject::
// collectRenderables, Visualizer's GraphSceneObject vtable slot 1, called once
// per object per frame with all its views) and, before each collect, rewrites
// the weights of measured entries to their cost, scaled so the collection's
// total weight is unchanged. Only the chunk boundaries move: the same objects
// are culled with the same views and produce the same renderables.
// Included once from main.cpp inside its anonymous namespace.
#pragma once

namespace partw {

constexpr uint32_t kVtableRva = 0x20d7e8;  // Visualizer .?AVGraphSceneObject@Graphics@@
constexpr int kSlot = 1;
constexpr size_t kCollections = 0x190;
constexpr size_t kEntryStride = 0x48;
constexpr int kCollectionCount = 7;
using Fn = void(__fastcall*)(void* self, uint32_t count, void* infos, void* flags, void* outs, void* probe);
using GetterFn = float(__fastcall*)(void* self);
// Object classes whose vtable has collectRenderables at slot 1 and the
// partition weight getter at slot 2.
struct Target {
  const wchar_t* module;
  uint32_t vtRva;
  const char* rtti;
  Fn collect = nullptr;
  GetterFn getter = nullptr;
};
Target g_targets[] = {
    {L"Visualizer.dll", 0x20d7e8, ".?AVGraphSceneObject@Graphics@@"},
    {L"animator.dll", 0x93650, ".?AVCharacterInstanceGraphicsReflection@Animator@@"},
};
constexpr int kTargets = sizeof(g_targets) / sizeof(g_targets[0]);
// reloc.h ids per target: the vtable, its slot 1 and slot 2 targets.
const hooksig::Id kTargetIds[kTargets][3] = {
    {hooksig::VIS_GraphSceneObject_vtbl, hooksig::VIS_GraphSceneObject_collectRenderables,
     hooksig::VIS_GraphSceneObject_partitionWeight},
    {hooksig::ANIM_CharacterInstanceGraphicsReflection_vtbl,
     hooksig::ANIM_CharacterInstanceGraphicsReflection_collectRenderables,
     hooksig::ANIM_CharacterInstanceGraphicsReflection_partitionWeight},
};
Fn g_orig = nullptr;  // non-null once installed
void** g_slot = nullptr;
std::atomic<bool>& g_on = g_costWeightsOn;
std::atomic<bool>& g_sanity = g_costWeightsSanity;

// Open-addressing table: object pointer -> smoothed cost (TSC cycles).
//
// Long sessions: objects come and go (mission loads, spawned and destroyed
// units), so every slot remembers the epoch (Apply call) it was last measured
// or asked for; Apply ages a few slots per call (Age) and frees those unused
// for g_evictAge epochs: their slot becomes a tombstone (kTomb, which lookups
// probe past and inserts reuse), and a rebuild in Apply drops the tombstones
// once there are many. Before, the table only filled: new objects beyond its
// 16-slot probe windows were never measured, and the dead objects' costs and
// weights stayed in UpdateScale's sums. Weights only move chunk boundaries,
// so a lookup racing an eviction can at worst misplace one boundary once.
constexpr size_t kTable = 1 << 15;
constexpr int kProbe = 16;
void* const kTomb = reinterpret_cast<void*>(uintptr_t{1});  // an evicted slot (no object is at address 1)
struct Slot {
  std::atomic<void*> obj{nullptr};
  std::atomic<float> cost{0};
  std::atomic<float> orig{0};  // DCS's own weight, captured before the first rewrite
  std::atomic<uint32_t> seen{0};  // the epoch of the last measure or getter call
};
Slot g_table[kTable];
constexpr uint32_t kEvictAge = 20000;  // Apply calls (several per frame: about 0.5-2 min)
constexpr uint32_t kSweep = kTable / 256;  // slots aged per Apply call (a full sweep every 256 calls)
std::atomic<uint32_t> g_evictAge{kEvictAge};  // the tests shorten it
std::atomic<uint32_t> g_epoch{1};
uint32_t g_cursor = 0;  // Apply only
std::atomic<uint32_t> g_live{0}, g_tombs{0};
std::atomic<uint64_t> g_evicted{0}, g_rebuilds{0}, g_noSlot{0};

// Scene.dll multiplies an entry's weight by its visible count (the sizes of
// the index vectors at +0x18, in 32-bit elements) [V Scene 0x180020618]. The
// measured cost already covers all views, so the weight is cost / count.
inline double VisibleCount(uint8_t* ent) {
  uint8_t* vb = *reinterpret_cast<uint8_t**>(ent + 0x18);
  uint8_t* ve = *reinterpret_cast<uint8_t**>(ent + 0x20);
  double n = (vb && ve > vb) ? static_cast<double>(ve - vb) / 4.0 : 1.0;
  return n < 1.0 ? 1.0 : n;
}

inline size_t Hash(void* p) { return ((reinterpret_cast<uintptr_t>(p) >> 4) * 0x9E3779B97F4A7C15ull) >> 49; }

inline void Seen(Slot& s) {
  const uint32_t e = g_epoch.load(std::memory_order_relaxed);
  if (s.seen.load(std::memory_order_relaxed) != e) s.seen.store(e, std::memory_order_relaxed);
}

// The object's slot; with insert, a new one (an empty slot or the window's
// first tombstone) when it has none. nullptr: not found, or the 16-slot
// window is full.
Slot* Find(void* obj, bool insert) {
  if (!obj || obj == kTomb) return nullptr;
  size_t h = Hash(obj) & (kTable - 1);
  Slot* tomb = nullptr;
  for (int i = 0; i < kProbe; ++i) {
    Slot& s = g_table[(h + i) & (kTable - 1)];
    void* cur = s.obj.load(std::memory_order_relaxed);
    if (cur == obj) return &s;
    if (cur == kTomb) {
      if (!tomb) tomb = &s;
      continue;
    }
    if (!cur) {
      if (!insert) return nullptr;
      Slot& d = tomb ? *tomb : s;
      void* expect = tomb ? kTomb : nullptr;
      if (d.obj.compare_exchange_strong(expect, obj)) {
        d.seen.store(g_epoch.load(std::memory_order_relaxed), std::memory_order_relaxed);
        g_live.fetch_add(1, std::memory_order_relaxed);
        if (tomb) g_tombs.fetch_sub(1, std::memory_order_relaxed);
        return &d;
      }
      if (expect == obj) return &d;
      return nullptr;  // lost a race for the slot: measured next time
    }
  }
  if (insert && tomb) {
    void* expect = kTomb;
    if (tomb->obj.compare_exchange_strong(expect, obj)) {
      tomb->seen.store(g_epoch.load(std::memory_order_relaxed), std::memory_order_relaxed);
      g_live.fetch_add(1, std::memory_order_relaxed);
      g_tombs.fetch_sub(1, std::memory_order_relaxed);
      return tomb;
    }
    if (expect == obj) return tomb;
  }
  if (insert) g_noSlot.fetch_add(1, std::memory_order_relaxed);
  return nullptr;
}

template <int N>
void __fastcall Hook(void* self, uint32_t count, void* infos, void* flags, void* outs, void* probe) {
  uint64_t t0 = __rdtsc();
  g_targets[N].collect(self, count, infos, flags, outs, probe);
  float c = static_cast<float>(__rdtsc() - t0);
  if (Slot* s = Find(self, true)) {
    float old = s->cost.load(std::memory_order_relaxed);
    s->cost.store(old == 0 ? c : old * 0.8f + c * 0.2f, std::memory_order_relaxed);
    Seen(*s);
  }
}

// Weight getter: GraphSceneObject vtable slot 2 (Visualizer 0x63110:
// movss xmm0,[rcx+0x30]; ret). Scene.dll calls it when it inserts the object
// entries, every frame, and copies the result to entry+0x10, so weights
// written into the entries are overwritten. With cost weights on, calls coming
// from Scene.dll get the measured cost instead (scaled so the measured
// objects keep the same total weight); every other caller gets DCS's value.
uint8_t* g_sceneLo = nullptr;
uint8_t* g_sceneHi = nullptr;
std::atomic<float> g_scale{0.f};
std::atomic<uint64_t> g_getterCalls{0}, g_getterRewritten{0};

template <int N>
float __fastcall Getter(void* self) {
  float w = g_targets[N].getter(self);
  uint8_t* ret = static_cast<uint8_t*>(_ReturnAddress());
  if (ret < g_sceneLo || ret >= g_sceneHi) return w;
  g_getterCalls.fetch_add(1, std::memory_order_relaxed);
  Slot* s = Find(self, true);
  if (!s) return w;
  Seen(*s);
  if (s->orig.load(std::memory_order_relaxed) != w) s->orig.store(w, std::memory_order_relaxed);
  float c = s->cost.load(std::memory_order_relaxed);
  float k = g_scale.load(std::memory_order_relaxed);
  if (!g_on.load(std::memory_order_relaxed) || c <= 0 || k <= 0) return w;
  g_getterRewritten.fetch_add(1, std::memory_order_relaxed);
  return std::max(0.05f, c * k);
}

// Called before each collect: scale = sum of DCS weights / sum of costs over
// the measured objects, so the rewritten weights keep DCS's total.
void UpdateScale() {
  double w = 0, c = 0;
  for (size_t i = 0; i < kTable; ++i) {
    void* o = g_table[i].obj.load(std::memory_order_relaxed);
    if (!o || o == kTomb) continue;
    float cc = g_table[i].cost.load(std::memory_order_relaxed);
    float oo = g_table[i].orig.load(std::memory_order_relaxed);
    if (cc > 0 && oo > 0) {
      w += oo;
      c += cc;
    }
  }
  g_scale.store(c > 0 ? static_cast<float>(w / c) : 0.f, std::memory_order_relaxed);
}

bool Install() {
  if (g_orig) return true;
  auto* scene = reinterpret_cast<uint8_t*>(GetModuleHandleW(L"Scene.dll"));
  if (!scene) return false;
  auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(scene + reinterpret_cast<IMAGE_DOS_HEADER*>(scene)->e_lfanew);
  g_sceneLo = scene;
  g_sceneHi = scene + nt->OptionalHeader.SizeOfImage;
  void* const collects[kTargets] = {reinterpret_cast<void*>(&Hook<0>), reinterpret_cast<void*>(&Hook<1>)};
  void* const getters[kTargets] = {reinterpret_cast<void*>(&Getter<0>), reinterpret_cast<void*>(&Getter<1>)};
  int ok = 0;
  for (int i = 0; i < kTargets; ++i) {
    Target& t = g_targets[i];
    auto* base = reinterpret_cast<uint8_t*>(GetModuleHandleW(t.module));
    if (!base) continue;
    // Recorded RVA, or where reloc.h re-found it in another build (0: not found);
    // there the slot targets must also be the recorded functions.
    const uint32_t vtRva = reloc::Rva(kTargetIds[i][0], t.vtRva);
    auto** vtbl = reinterpret_cast<void**>(base + vtRva);
    if (!vtRva || !allocslab::RttiIs(base, vtbl, t.rtti) ||
        !reloc::SlotIs(kTargetIds[i][1], base, SlotOriginal(&vtbl[1])) ||
        !reloc::SlotIs(kTargetIds[i][2], base, SlotOriginal(&vtbl[2]))) {
      Log("partition weights: %s vtable does not match this build; skipped", t.rtti);
      continue;
    }
    t.collect = reinterpret_cast<Fn>(SlotOriginal(&vtbl[1]));
    t.getter = reinterpret_cast<GetterFn>(SlotOriginal(&vtbl[2]));
    if (HookSlot(&vtbl[1], collects[i], nullptr) && HookSlot(&vtbl[2], getters[i], nullptr)) ++ok;
  }
  if (!ok) return false;
  g_orig = g_targets[0].collect ? g_targets[0].collect : g_targets[1].collect;
  Log("partition weights: timing %d object classes", ok);
  return true;
}

struct Stats {
  uint64_t entries = 0, matched = 0, rewritten = 0;
};
std::atomic<uint64_t> g_entries{0}, g_matched{0};

bool ClassName(void* obj, char* buf, size_t n) {
  __try {
    void** vt = *static_cast<void***>(obj);
    auto* col = static_cast<uint8_t*>(vt[-1]);
    if (*reinterpret_cast<uint32_t*>(col) != 1) return false;
    uint8_t* base = col - *reinterpret_cast<uint32_t*>(col + 0x14);
    const char* name = reinterpret_cast<const char*>(base + *reinterpret_cast<uint32_t*>(col + 0xC) + 0x10);
    strncpy_s(buf, n, name, _TRUNCATE);
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

std::string ClassOf(void* obj) {
  char buf[96];
  return ClassName(obj, buf, sizeof(buf)) ? std::string(buf) : std::string("?");
}

std::atomic<bool> g_censusRequest{false};

// One-shot census of partition entries by object class and weight.
void Census(uint8_t* view) {
  std::map<std::string, std::pair<int, float>> byClass;
  for (int k = 0; k < kCollectionCount; ++k) {
    uint8_t* b = *reinterpret_cast<uint8_t**>(view + kCollections + k * 0x18);
    uint8_t* e = *reinterpret_cast<uint8_t**>(view + kCollections + k * 0x18 + 8);
    if (!b || e <= b) continue;
    for (uint8_t* ent = b; ent < e; ent += kEntryStride) {
      void* obj = *reinterpret_cast<void**>(ent + 8);
      std::string cls = std::to_string(k) + " " + (obj ? ClassOf(obj) : "null");
      if (Find(obj, false)) cls += " [measured]";
      auto& v = byClass[cls];
      v.first++;
      v.second = *reinterpret_cast<float*>(ent + 0x10);
    }
  }
  for (auto& kv : byClass) Log("  partition census: collection %s: %d entries, weight %.2f", kv.first.c_str(), kv.second.first, kv.second.second);
}

// Drops every tombstone: the live slots are inserted again from scratch
// (Apply only; a slot that no longer fits its window is measured again).
void Rebuild() {
  struct Kept {
    void* obj;
    float cost, orig;
    uint32_t seen;
  };
  static std::vector<Kept> live;
  if (live.capacity() < kTable) live.reserve(kTable);  // once: no allocation per rebuild
  live.clear();
  for (Slot& s : g_table) {
    void* o = s.obj.load(std::memory_order_relaxed);
    if (o && o != kTomb)
      live.push_back({o, s.cost.load(std::memory_order_relaxed), s.orig.load(std::memory_order_relaxed),
                      s.seen.load(std::memory_order_relaxed)});
    s.obj.store(nullptr, std::memory_order_relaxed);
    s.cost.store(0, std::memory_order_relaxed);
    s.orig.store(0, std::memory_order_relaxed);
  }
  g_live = 0;
  g_tombs = 0;
  for (const Kept& k : live)
    if (Slot* s = Find(k.obj, true)) {
      s->cost.store(k.cost, std::memory_order_relaxed);
      s->orig.store(k.orig, std::memory_order_relaxed);
      s->seen.store(k.seen, std::memory_order_relaxed);
    }
  g_rebuilds.fetch_add(1, std::memory_order_relaxed);
}

// One epoch: kSweep slots aged; those unused for g_evictAge epochs evicted
// (tombstones), a rebuild once tombstones are an eighth of the table.
void Age() {
  const uint32_t now = g_epoch.fetch_add(1, std::memory_order_relaxed) + 1;
  const uint32_t age = g_evictAge.load(std::memory_order_relaxed);
  for (uint32_t k = 0; k < kSweep; ++k) {
    Slot& s = g_table[g_cursor];
    g_cursor = (g_cursor + 1) & (kTable - 1);
    void* o = s.obj.load(std::memory_order_relaxed);
    if (!o || o == kTomb || now - s.seen.load(std::memory_order_relaxed) <= age) continue;
    s.obj.store(kTomb, std::memory_order_relaxed);
    s.cost.store(0, std::memory_order_relaxed);
    s.orig.store(0, std::memory_order_relaxed);
    g_live.fetch_sub(1, std::memory_order_relaxed);
    g_tombs.fetch_add(1, std::memory_order_relaxed);
    g_evicted.fetch_add(1, std::memory_order_relaxed);
  }
  if (g_tombs.load(std::memory_order_relaxed) > kTable / 8) Rebuild();
}

// Before the collect call (render thread, pool idle).
void Apply(uint8_t* view) {
  Age();
  UpdateScale();
  if (g_censusRequest.exchange(false)) Census(view);
}

}  // namespace partw
