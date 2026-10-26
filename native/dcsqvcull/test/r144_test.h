// R144 (docs/research/R144_live_phase2.md): Live mode phase 2.
//  - [Model] AllocSlabs switched on and off while threads allocate: a fake StructBufferManager with DCS's semantics
//    (page search 0xc1b0, locked bump 0xbf30, per-frame reset slot 7; no per-object free: the reset is the release).
//    Allocations made before the enable are released by the reset after it; threads that existed before the enable;
//    the reset on another thread; the switch flipped while 8 threads allocate; a disable with slab tails reserved.
//  - The live install decisions: allocslab::LiveInstallWanted and DiagnosticsLiveAction.
//  - Quad Views' settings.cfg followed live by the symbology export's config tick (post-process keys only).
// Runs after the allocator test in test_main.cpp (that one reads the global allocate counters).
#pragma once

#include <condition_variable>

namespace r144test {

namespace csy = ::dcsvr_symbology;
namespace cmask = fixdepth::cmask;

struct FakeMgr {
  void* vt;
  uint8_t* begin;
  uint8_t* end;
  uint8_t* cap;
  std::mutex m;  // stands in for the ed::mutex at +0x20
};
static_assert(offsetof(FakeMgr, m) == 0x20, "mutex offset");
FakeMgr g_mgr;
std::vector<uint8_t> g_pageMem(0x30 * 4096);
std::atomic<bool> g_pageOverflow{false};

struct Fakes {
  static void Lock(void* mu) { static_cast<std::mutex*>(mu)->lock(); }
  static void Unlock(void* mu) { static_cast<std::mutex*>(mu)->unlock(); }
  static allocslab::Page* Scan(void* m, uint32_t size, uint32_t count) {  // under the manager's lock
    auto* fm = static_cast<FakeMgr*>(m);
    for (uint8_t* p = fm->begin; p < fm->end; p += 0x30) {
      auto* pg = reinterpret_cast<allocslab::Page*>(p);
      if (pg->elemSize == size && pg->capBytes - pg->used >= size * count) return pg;
    }
    if (fm->end + 0x30 > fm->cap) {  // the test's record array is full (never in a passing run)
      g_pageOverflow = true;
      fm->end -= 0x30;
    }
    const uint32_t n = std::max(count, 0xFE00u / size);
    auto* pg = reinterpret_cast<allocslab::Page*>(fm->end);
    *pg = {static_cast<uint32_t>((fm->end - fm->begin) / 0x30), size, n, n * size, 0, 0,
           static_cast<uint8_t*>(malloc(n * size)), nullptr};
    fm->end += 0x30;
    return pg;
  }
  // DCS's allocate (what the trampoline runs): the same lock, the same search, used += size * count.
  static uint8_t* Alloc(void* m, uint32_t size, uint32_t count, uint32_t* page, uint32_t* index) {
    auto* fm = static_cast<FakeMgr*>(m);
    fm->m.lock();
    allocslab::Page* pg = Scan(m, size, count);
    const uint32_t used = pg->used;
    pg->used += size * count;
    *page = pg->idx;
    *index = used / size;
    fm->m.unlock();
    return pg->data + used;
  }
  static void Reset(void* m) {  // slot 7: every page rewound, unlocked (no culling task in flight)
    auto* fm = static_cast<FakeMgr*>(m);
    for (uint8_t* p = fm->begin; p < fm->end; p += 0x30) {
      auto* pg = reinterpret_cast<allocslab::Page*>(p);
      pg->prevUsed = pg->used;
      pg->used = 0;
    }
  }
};

struct Rec {
  uint32_t page, off, len;
  uint8_t* ptr;
  bool slab;  // made while the switch read on (informational)
};

allocslab::Page* PageOf(uint32_t idx) { return reinterpret_cast<allocslab::Page*>(g_mgr.begin + idx * 0x30); }

// Every record matches its (page, index), lies inside [0, used) of its page, and no two overlap (one frame).
bool Consistent(const std::vector<std::vector<Rec>>& recs, bool* overlap) {
  bool match = true;
  *overlap = false;
  std::map<uint32_t, std::vector<std::pair<uint32_t, uint32_t>>> byPage;
  for (const auto& v : recs)
    for (const Rec& r : v) {
      allocslab::Page* pg = PageOf(r.page);
      if (r.ptr != pg->data + r.off || r.off + r.len > pg->used) match = false;
      byPage[r.page].push_back({r.off, r.len});
    }
  for (auto& kv : byPage) {
    auto& v = kv.second;
    std::sort(v.begin(), v.end());
    for (size_t i = 1; i < v.size(); ++i)
      if (v[i - 1].first + v[i - 1].second > v[i].first) *overlap = true;
  }
  return match;
}

Rec One(int t, int i) {
  static const uint32_t sizes[4] = {0x60, 0x70, 0xA0, 0xB0};
  const uint32_t size = sizes[(i * 7 + t) % 4], count = 1 + (i % 5);
  uint32_t pg = 0, idx = 0;
  uint8_t* p = allocslab::HookAlloc(&g_mgr, size, count, &pg, &idx);
  return {pg, idx * size, size * count, p, g_allocSlabsOn.load()};
}

// Threads that stay alive across phases (like DCS's pool threads, which exist before any live enable): each phase
// runs `n` allocations per thread, then the threads wait for the next phase. A phase never overlaps a reset.
struct Pool {
  int threads;
  std::vector<std::thread> th;
  std::vector<std::vector<Rec>> recs;
  std::mutex mu;
  std::condition_variable cv;
  int phase = 0, done = 0, perPhase = 0;
  bool quit = false;
  std::atomic<int> doneA{0};  // threads done with the phase (read without the lock)
  explicit Pool(int n) : threads(n), recs(n) {
    for (int t = 0; t < n; ++t)
      th.emplace_back([this, t] {
        int seen = 0;
        for (;;) {
          int count;
          {
            std::unique_lock<std::mutex> lk(mu);
            cv.wait(lk, [&] { return quit || phase != seen; });
            if (quit) return;
            seen = phase;
            count = perPhase;
          }
          for (int i = 0; i < count; ++i) recs[t].push_back(One(t, i));
          {
            std::lock_guard<std::mutex> lk(mu);
            ++done;
            doneA = done;
          }
          cv.notify_all();
        }
      });
  }
  // Starts a phase; Wait() returns when every thread finished it.
  void Start(int n) {
    std::lock_guard<std::mutex> lk(mu);
    perPhase = n;
    done = 0;
    doneA = 0;
    ++phase;
    cv.notify_all();
  }
  void Wait() {
    std::unique_lock<std::mutex> lk(mu);
    cv.wait(lk, [&] { return done == threads; });
  }
  void Clear() {
    for (auto& v : recs) v.clear();
  }
  ~Pool() {
    {
      std::lock_guard<std::mutex> lk(mu);
      quit = true;
    }
    cv.notify_all();
    for (auto& x : th) x.join();
  }
};

uint64_t UsedTotal() {
  uint64_t u = 0;
  for (uint8_t* p = g_mgr.begin; p < g_mgr.end; p += 0x30) u += reinterpret_cast<allocslab::Page*>(p)->used;
  return u;
}

void Slabs() {
  g_mgr.begin = g_mgr.end = g_pageMem.data();
  g_mgr.cap = g_pageMem.data() + g_pageMem.size();
  const auto saveLock = allocslab::g_lock, saveUnlock = allocslab::g_unlock;
  const auto saveScan = allocslab::g_scan;
  const auto saveTramp = allocslab::g_tramp;
  const auto saveReset = allocslab::g_origReset;
  const bool saveOn = g_allocSlabsOn.load();
  const uint32_t saveBytes = g_allocSlabBytes.load();
  allocslab::g_lock = &Fakes::Lock;
  allocslab::g_unlock = &Fakes::Unlock;
  allocslab::g_scan = &Fakes::Scan;
  allocslab::g_tramp = &Fakes::Alloc;
  allocslab::g_origReset = &Fakes::Reset;
  g_allocSlabBytes = 4096;
  g_allocSlabsOn = false;
  const allocslab::Totals t0 = allocslab::Snapshot();
  {
    Pool pool(6);  // created while the switch is off: the "existing" pool threads
    // 1. Allocate before the enable (DCS's path), enable mid-frame, allocate again (slabs), release by the reset,
    //    then a new frame: no thread may reuse a slab of the frame before (it would land beyond used = 0).
    pool.Start(400);
    pool.Wait();
    const allocslab::Totals tOff = allocslab::Snapshot();
    g_allocSlabsOn = true;  // the live enable, between two culling calls of the same frame
    pool.Start(400);
    pool.Wait();
    const allocslab::Totals tOn = allocslab::Snapshot();
    bool ov1 = false;
    const bool m1 = Consistent(pool.recs, &ov1);
    const bool slowOff = tOff.slow == t0.slow;  // the switch off: no slab reserved
    const bool slabsUsed = tOn.slow > tOff.slow && tOn.slow - tOff.slow < (tOn.calls - tOff.calls) / 4;
    std::thread resetter([] { allocslab::HookReset(&g_mgr); });  // the reset on another thread (the render thread)
    resetter.join();
    const bool released = UsedTotal() == 0;
    pool.Clear();
    pool.Start(300);
    pool.Wait();
    bool ov2 = false;
    const bool m2 = Consistent(pool.recs, &ov2);
    Check(m1 && !ov1 && slowOff && slabsUsed && released && m2 && !ov2,
          "R144 AllocSlabs live enable: allocations made by DCS's path before the enable and slab allocations after it "
          "share the pages without overlap; the reset on another thread releases both (every page's used = 0); no "
          "thread reuses a slab of the frame before (pool threads created before the enable)");

    // 2. Cross-thread: threads born after the enable and the pool threads in one frame; the reset by a third thread;
    //    then both again.
    allocslab::HookReset(&g_mgr);
    pool.Clear();
    std::vector<std::vector<Rec>> late(3);
    pool.Start(300);
    {
      std::vector<std::thread> fresh;
      for (int t = 0; t < 3; ++t)
        fresh.emplace_back([&late, t] {
          for (int i = 0; i < 300; ++i) late[t].push_back(One(10 + t, i));
        });
      for (auto& x : fresh) x.join();
    }
    pool.Wait();
    std::vector<std::vector<Rec>> all = pool.recs;
    all.insert(all.end(), late.begin(), late.end());
    bool ov3 = false;
    const bool m3 = Consistent(all, &ov3);
    std::thread([] { allocslab::HookReset(&g_mgr); }).join();
    pool.Clear();
    pool.Start(200);
    pool.Wait();
    bool ov4 = false;
    const bool m4 = Consistent(pool.recs, &ov4);
    Check(m3 && !ov3 && m4 && !ov4,
          "R144 AllocSlabs cross-thread: threads started after the enable and pool threads from before it allocate in "
          "one frame without overlap; the reset made by a third thread releases them and the next frame is consistent");

    // 3. The switch flipped on and off as fast as the main thread can while 6 pool threads allocate without pause
    //    (4000 each per frame, 4 frames).
    bool m5 = true, ov5 = false;
    uint64_t flips = 0;
    for (int frame = 0; frame < 4; ++frame) {
      allocslab::HookReset(&g_mgr);
      pool.Clear();
      pool.Start(4000);
      while (pool.doneA.load() < pool.threads) {
        g_allocSlabsOn = (flips++ & 1) == 0;
        for (volatile int spin = 0; spin < 200; spin = spin + 1) {
        }
      }
      pool.Wait();
      bool ov = false;
      if (!Consistent(pool.recs, &ov)) m5 = false;
      if (ov) ov5 = true;
    }
    printf("     R144 allocator: switch flipped %llu times while 6 threads allocated 4 frames of 4000 each\n",
           static_cast<unsigned long long>(flips));
    Check(m5 && !ov5 && !g_pageOverflow.load() && flips >= 8,
          "R144 AllocSlabs switched on and off while 6 threads allocate (4 frames): every allocation matches its page "
          "and index inside the used range, none overlaps");

    // 4. Disable with slab tails reserved: DCS's path then allocates past them (used already covers the tails); a
    //    re-enable in the same frame serves only inside a slab's reserved range; the reset releases everything.
    allocslab::HookReset(&g_mgr);
    pool.Clear();
    g_allocSlabsOn = true;
    pool.Start(150);
    pool.Wait();
    const uint64_t usedOn = UsedTotal();
    g_allocSlabsOn = false;  // the live disable (one-way before R142; a flag)
    pool.Start(150);
    pool.Wait();
    g_allocSlabsOn = true;
    pool.Start(150);
    pool.Wait();
    bool ov6 = false;
    const bool m6 = Consistent(pool.recs, &ov6);
    allocslab::HookReset(&g_mgr);
    const bool released6 = UsedTotal() == 0;
    Check(usedOn > 0 && m6 && !ov6 && released6,
          "R144 AllocSlabs live disable: slab tails stay reserved until the reset, DCS's allocations after the disable "
          "and slab allocations after a re-enable in the same frame do not overlap them; the reset releases all");
  }
  g_allocSlabsOn = saveOn;
  g_allocSlabBytes = saveBytes;
  allocslab::g_lock = saveLock;
  allocslab::g_unlock = saveUnlock;
  allocslab::g_scan = saveScan;
  allocslab::g_tramp = saveTramp;
  allocslab::g_origReset = saveReset;
  allocslab::Invalidate();
  for (uint8_t* p = g_mgr.begin; p < g_mgr.end; p += 0x30) free(reinterpret_cast<allocslab::Page*>(p)->data);
  g_mgr.end = g_mgr.begin;
}

void Decisions() {
  using allocslab::LiveInstallWanted;
  Check(LiveInstallWanted(true, 0, true, true) && !LiveInstallWanted(false, 0, true, true) &&
            !LiveInstallWanted(true, 1, true, true) && !LiveInstallWanted(true, 2, true, true) &&
            !LiveInstallWanted(true, -1, true, true) && !LiveInstallWanted(true, 0, false, true) &&
            !LiveInstallWanted(true, 0, true, false),
        "R144 AllocSlabs live install: only for an enable with nothing prepared yet, after the worker's start installs, "
        "with NGModel.dll loaded; never again after a refusal (-1) or while a patch is pending / active");
  const bool on = DiagnosticsLiveAction(true, false, false, true, false) == kDiagInstall &&
                  DiagnosticsLiveAction(true, true, false, true, false) == kDiagInstall;
  const bool idempotent = DiagnosticsLiveAction(true, false, true, true, false) == kDiagNothing;
  const bool offOnce = DiagnosticsLiveAction(false, true, true, true, false) == kDiagStaysUntilRestart &&
                       DiagnosticsLiveAction(false, false, true, true, false) == kDiagNothing &&
                       DiagnosticsLiveAction(false, true, false, true, false) == kDiagNothing;
  const bool gated = DiagnosticsLiveAction(true, false, false, false, false) == kDiagNothing &&
                     DiagnosticsLiveAction(true, false, false, true, true) == kDiagNothing;
  Check(on && idempotent && offOnce && gated,
        "R144 Diagnostics live: 0 -> 1 installs (once), 1 -> 0 with the hooks installed is reported once (they stay "
        "until the next start), nothing before the start installs or in PerceptionOnly");
}

// settings.cfg written live (the post-process keys DCS Control will write): the symbology export's config tick
// (cmask::sym::TickQv(false), once per second while the export runs) follows the file without a restart.
void QvSettingsLive() {
  wchar_t tp[MAX_PATH];
  GetTempPathW(MAX_PATH, tp);
  const std::wstring cfg = std::wstring(tp) + L"r144_qv_" + std::to_wstring(GetCurrentProcessId()) + L".cfg";
  auto write = [&](const std::string& text, int stampSec) {
    FILE* f = nullptr;
    if (_wfopen_s(&f, cfg.c_str(), L"wb") == 0 && f) fwrite(text.data(), 1, text.size(), f), fclose(f);
    // A distinct write time per step (the stamp is mtime + size; a fast rewrite can share a timestamp).
    HANDLE h = CreateFileW(cfg.c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, 0, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
      SYSTEMTIME st{2026, 10, 0, 26, 12, 0, static_cast<WORD>(stampSec), 0};
      FILETIME ft{};
      SystemTimeToFileTime(&st, &ft);
      SetFileTime(h, nullptr, nullptr, &ft);
      CloseHandle(h);
    }
  };
  auto tick = [] {
    cmask::sym::g_qvLastCheck = 0;  // past the once-per-second throttle
    cmask::sym::TickQv(false);
  };
  SetEnvironmentVariableW(L"DCSVR_QUAD_SETTINGS", cfg.c_str());
  write("sharpen_focus_view=0.7\nsmoothen_focus_view_edges=0.2\nfocus_view_shape=0\n", 1);
  cmask::sym::TickQv(true);  // the export's install
  const uint32_t g0 = cmask::sym::g_qvGen.load();
  // The app's live write: sharpening, edge blend, taper, temporal (and a key we do not mirror).
  write("sharpen_focus_view=0.4\ndcsvr_periphery_sharpen=0.3\nsmoothen_focus_view_edges=0.25\nfocus_view_shape=0\n"
        "dcsvr_sharpen_taper=1\ndcsvr_temporal=1\ndcsvr_periphery_contrast=0.1\n",
        2);
  tick();
  csy::QvSettings q1;
  {
    std::lock_guard<std::mutex> lock(cmask::sym::g_qvMu);
    q1 = cmask::sym::g_qv;
  }
  const bool followed = cmask::sym::g_qvGen.load() == g0 + 1 && q1.sharpenFocusView == 0.4f &&
                        q1.peripherySharpen == 0.3f && q1.smoothenFocusViewEdges == 0.25f &&
                        (q1.flags & csy::qv_flags::sharpen_taper) && (q1.flags & csy::qv_flags::periphery_temporal) &&
                        q1.settingsGen == g0 + 1;
  // The same values rewritten (a new mtime): no new settings generation.
  write("sharpen_focus_view=0.4\ndcsvr_periphery_sharpen=0.3\nsmoothen_focus_view_edges=0.25\nfocus_view_shape=0\n"
        "dcsvr_sharpen_taper=1\ndcsvr_temporal=1\ndcsvr_periphery_contrast=0.1\n",
        3);
  tick();
  const bool sameGen = cmask::sym::g_qvGen.load() == g0 + 1;
  // Within the second after a check the file is not read again (the throttle), the next tick picks the change up.
  write("sharpen_focus_view=0.5\n", 4);
  cmask::sym::g_qvLastCheck = GetTickCount64();
  cmask::sym::TickQv(false);
  const bool throttled = cmask::sym::g_qvGen.load() == g0 + 1;
  tick();
  const bool later = cmask::sym::g_qvGen.load() == g0 + 2;
  DeleteFileW(cfg.c_str());
  SetEnvironmentVariableW(L"DCSVR_QUAD_SETTINGS", nullptr);
  cmask::sym::TickQv(true);
  Check(followed && sameGen && throttled && later,
        "R144 settings.cfg live: a write of the post-process keys (sharpening, periphery sharpening, edge blend, taper, "
        "temporal) is re-read by the next config tick (<= 1 s) without a restart and published with settingsGen + 1; "
        "an identical rewrite changes nothing; an unmirrored key is ignored");
}

void Run() {
  Slabs();
  Decisions();
  QvSettingsLive();
}

}  // namespace r144test
