// R74a: content time per committed frame for the frame-generation companions (dcsvr_cockpit_depth.h and
// dcsvr_motion.h: header.slotTime, the same record in both; design and live check:
// docs/research/R74_content_time_and_mask_v11.md).
//
//  - poseQpc. DCS calls xrLocateViews through the OpenXR dispatch table entry slot 36 (XrGeneratedDispatchTable:
//    xrEndFrame 35, xrLocateViews 36; Visualizer's trampoline at +0x1b7409: mov r10, [rax+48h]; mov rax, [r10+120h],
//    the same table as the xrEndFrame hook). The hook below (installed with the xrEndFrame hook, same stamp and
//    instance checks plus its own trampoline check) stamps each call: QPC at entry and at return, the call's
//    XrViewLocateInfo::displayTime, the returned view 0 orientation, DCS's model time at entry and the thread. It
//    forwards unchanged and writes nothing DCS sees. Calls land in a small ring (an SRW lock: DCS may call it from
//    another thread than the render thread).
//  - Pairing (render thread, in the xrEndFrame commit): the frame's XrFrameEndInfo::displayTime selects the calls
//    with the same displayTime made before the commit; among them the one whose returned view 0 orientation equals
//    the orientation DCS submitted for view 0 (pose_matched), else the last one. More than one candidate:
//    pose_ambiguous (locateCalls counts them).
//  - simTime. World.dll exports the data symbol ?modelTime@wTime@@0NA (static double wTime::modelTime: DCS's model
//    time in seconds, what the Lua DCS.getModelTime returns). It is read (one double load under SEH, no code run) at
//    the picked call's entry. sim_stable needs kSimProven (set only after a live check showed simTime tied 1:1 to the
//    rendered frame) and, per frame, a step since the previous commit consistent with the pose clock.
// Live (R74 doc §4): DCS calls xrLocateViews once per frame on a thread other than the render thread (its simulation
// / main thread), 33 ms (p50; GPU-bound at ~55 fps) or 4-5 ms (at 90 fps) before the frame's xrEndFrame commit; the
// picked call matched DCS's submitted view 0 orientation in every frame. Model time is quantized to 1 ms and advances
// once per frame, but its steps scatter by ~2-4 ms (sd) around the pose clock's: kSimProven stays false.
// Cost: per DCS xrLocateViews call two QPC reads, a lock and a few loads, only while the mask export and a companion
// (depth or motion) have a reader; per commit a scan of 32 records. Nothing otherwise (the hook forwards at once).
// Included from cockpit_mask.h inside namespace fixdepth::cmask, before the commit.
#pragma once

namespace ctime {

constexpr int kRing = 32;
constexpr int kLvSlot = 36;                    // XrGeneratedDispatchTable: xrLocateViews
constexpr uint32_t kLvTrampRva = 0x1b7409;     // Visualizer: mov r10, [rax+48h]; mov rax, [r10+120h]
const uint8_t kLvTramp[] = {0x4c, 0x8b, 0x50, 0x48, 0x49, 0x8b, 0x82, 0x20, 0x01, 0x00, 0x00};
constexpr const char* kModelTimeExport = "?modelTime@wTime@@0NA";  // World.dll: static double wTime::modelTime
// Not proven (R74 doc §4): simTime advances once per rendered frame, but no live oracle tied its steps to the frame's
// content (world / ownship motion); while false the writer never sets time_flags::sim_stable.
constexpr bool kSimProven = false;

// One DCS xrLocateViews call.
struct Rec {
  int64_t q0 = 0, q1 = 0;  // QPC at entry / return (q0 0: empty)
  int64_t dt = 0;          // XrViewLocateInfo::displayTime (0: unreadable)
  double sim = 0;          // model time at entry
  float q[4] = {};         // returned view 0 orientation (x, y, z, w)
  uint32_t vn = 0;         // R95: views read below (up to 4: the symbology export's window prediction)
  float vq[4][4] = {};     // R95: each returned view's orientation (x, y, z, w)
  float vfov[4][4] = {};   // R95: and its FOV (angles: left, right, up, down)
  bool qOk = false, simOk = false;
  int32_t rc = 0;
  uint32_t tid = 0;
};

Rec g_ring[kRing];
uint32_t g_ringN = 0;  // records ever written (index = g_ringN % kRing)
SRWLOCK g_lock = SRWLOCK_INIT;

using LocateFn = int32_t(__stdcall*)(void*, const void*, void*, uint32_t, uint32_t*, void*);
LocateFn g_orig = nullptr;  // kept after the unhook (a call in flight still forwards through it)
void** g_slot = nullptr;
// R93: an observer chained behind this hook ([Perf] FrameTimeline's xrLocateViews hook): when set, the hook forwards
// to it instead of g_orig, and it calls g_orig. Cleared by its owner (frame_timeline.h) and by Uninstall.
std::atomic<LocateFn> g_next{nullptr};
std::atomic<bool> g_on{false};      // the hook records (else forwards at once)
// g_hooked (installed: the companions carry header_flags::slot_time) is declared near the top of cockpit_mask.h.
std::atomic<int> g_busy{0};
std::atomic<const double*> g_modelTime{nullptr};
std::atomic<bool> g_testRecordAlways{false};  // tests: record without an active export

std::atomic<uint64_t> g_calls{0}, g_callsOff{0}, g_failed{0}, g_records{0}, g_observed{0}, g_matched{0},
    g_ambiguous{0}, g_noLocate{0}, g_simObserved{0}, g_simStable{0}, g_simBack{0}, g_simSame{0};

// The previous commit's record (render thread), for sim_stable.
struct Prev {
  bool ok = false, simOk = false;
  double sim = 0;
  int64_t q0 = 0;
};
Prev g_prev;
cdp::SlotTime g_rec{};           // this commit's record (render thread): copied into both companions
double g_simAtCommit = 0;        // model time read at the commit (diagnostics: CockpitMaskLog)
bool g_simAtCommitOk = false;
uint32_t g_pickTid = 0;          // the picked call's thread (diagnostics)

bool ReadModelTimeSeh(double& out) {
  const double* p = g_modelTime.load(std::memory_order_relaxed);
  if (!p) return false;
  __try {
    out = *p;
    return out == out;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

// XrViewLocateInfo (type 6): displayTime at 24.
int64_t ReadLocateTimeSeh(const void* info) {
  __try {
    if (!info || *static_cast<const int32_t*>(info) != 6) return 0;
    return *reinterpret_cast<const int64_t*>(static_cast<const uint8_t*>(info) + 24);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return 0;
  }
}

// XrView (type 7, 64 bytes): orientation at 16, FOV at 44. R95: every view up to 4 (orientation and FOV).
bool ReadView0Seh(const uint32_t* count, const void* views, uint32_t cap, float q[4], Rec* all = nullptr) {
  __try {
    if (!views || !count || cap == 0 || *count == 0) return false;
    const auto* v = static_cast<const uint8_t*>(views);
    if (*reinterpret_cast<const int32_t*>(v) != 7) return false;
    memcpy(q, v + 16, 16);
    if (all) {
      const uint32_t n = std::min<uint32_t>(std::min(*count, cap), 4);
      all->vn = 0;
      for (uint32_t i = 0; i < n; ++i) {
        const uint8_t* x = v + 64 * i;
        if (*reinterpret_cast<const int32_t*>(x) != 7) break;
        memcpy(all->vq[i], x + 16, 16);
        memcpy(all->vfov[i], x + 44, 16);
        all->vn = i + 1;
      }
    }
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

void Push(const Rec& r) {
  AcquireSRWLockExclusive(&g_lock);
  g_ring[g_ringN % kRing] = r;
  ++g_ringN;
  ReleaseSRWLockExclusive(&g_lock);
  g_records++;
}

bool Recording() {
  return g_on.load(std::memory_order_acquire) &&
         (g_testRecordAlways.load(std::memory_order_relaxed) ||
          (Active() && !g_idleA.load(std::memory_order_relaxed) &&
           (!g_dIdleA.load(std::memory_order_relaxed) || !g_mIdleA.load(std::memory_order_relaxed) ||
            !g_sIdleA.load(std::memory_order_relaxed))));  // R95: the symbology's window prediction
}

int32_t __stdcall H_LocateViews(void* session, const void* info, void* state, uint32_t cap, uint32_t* count,
                                void* views) {
  g_busy.fetch_add(1, std::memory_order_acq_rel);
  const LocateFn next = g_next.load(std::memory_order_acquire);  // R93: FrameTimeline chained behind us
  const LocateFn orig = next ? next : g_orig;
  if (!Recording()) {
    g_busy.fetch_sub(1, std::memory_order_acq_rel);
    return orig(session, info, state, cap, count, views);
  }
  g_calls++;
  Rec r;
  LARGE_INTEGER t0, t1;
  QueryPerformanceCounter(&t0);
  r.q0 = t0.QuadPart;
  r.dt = ReadLocateTimeSeh(info);
  r.simOk = ReadModelTimeSeh(r.sim);
  r.tid = GetCurrentThreadId();
  const int32_t rc = orig(session, info, state, cap, count, views);
  QueryPerformanceCounter(&t1);
  r.q1 = t1.QuadPart;
  r.rc = rc;
  if (rc >= 0) {
    r.qOk = ReadView0Seh(count, views, cap, r.q, &r);
    Push(r);
  } else {
    g_failed++;
  }
  if (r.tid != ptiming::g_topTid.load(std::memory_order_relaxed)) g_callsOff++;
  g_busy.fetch_sub(1, std::memory_order_acq_rel);
  return rc;
}

// ---- Pure pairing (tests) ----------------------------------------------------------------------------------------

// Same rotation: equal components, or equal up to the quaternion's sign.
inline bool SameOrientation(const float a[4], const float b[4], float eps = 1e-6f) {
  bool same = true, neg = true;
  for (int i = 0; i < 4; ++i) {
    same = same && std::fabs(a[i] - b[i]) <= eps;
    neg = neg && std::fabs(a[i] + b[i]) <= eps;
  }
  return same || neg;
}

struct Pick {
  int idx = -1;        // the record picked, or -1
  uint32_t calls = 0;  // records with that displayTime made before `beforeQpc`
  bool matched = false;
};

// The call whose pose DCS rendered in the frame it ends with `displayTime`: same displayTime, made before beforeQpc;
// the latest whose view 0 orientation equals q0 (DCS's submitted view 0; nullptr unknown), else the latest.
inline Pick PickLocate(const Rec* recs, int n, int64_t displayTime, const float* q0, int64_t beforeQpc) {
  Pick p;
  if (displayTime == 0) return p;
  int last = -1, match = -1;
  for (int i = 0; i < n; ++i) {
    const Rec& r = recs[i];
    if (!r.q0 || r.dt != displayTime || r.q0 > beforeQpc) continue;
    ++p.calls;
    if (last < 0 || r.q0 > recs[last].q0) last = i;
    if (q0 && r.qOk && SameOrientation(r.q, q0) && (match < 0 || r.q0 > recs[match].q0)) match = i;
  }
  p.idx = match >= 0 ? match : last;
  p.matched = match >= 0;
  return p;
}

// sim_stable for one step (seconds): it advanced, by between half and 1.5 times the pose clock's step (+1 ms).
inline bool SimStep(double dSim, double dPose) {
  return dSim > 0 && dPose > 0 && dSim >= 0.5 * dPose - 0.001 && dSim <= 1.5 * dPose + 0.001;
}

// ---- The commit (render thread) ---------------------------------------------------------------------------------

// Builds g_rec for the commit of `seq` (the frame DCS ends with displayTime; paired: from the xrEndFrame hook).
void Make(uint64_t seq, int64_t displayTime, bool paired, const XrFrameInfo* xr, int64_t nowQpc) {
  g_rec = cdp::SlotTime{};
  g_rec.seq = seq;
  g_simAtCommitOk = ReadModelTimeSeh(g_simAtCommit);
  g_pickTid = 0;
  if (!g_hooked.load(std::memory_order_relaxed) || !paired || !displayTime) {
    g_prev = Prev{};
    return;
  }
  Rec recs[kRing];
  AcquireSRWLockShared(&g_lock);
  memcpy(recs, g_ring, sizeof(recs));
  ReleaseSRWLockShared(&g_lock);
  const float* q0 = xr && xr->poses && xr->viewCount > 0 ? xr->q[0] : nullptr;
  const Pick p = PickLocate(recs, kRing, displayTime, q0, nowQpc);
  g_rec.locateCalls = p.calls;
  if (p.idx < 0) {
    g_noLocate++;
    g_prev = Prev{};
    return;
  }
  const Rec& r = recs[p.idx];
  g_pickTid = r.tid;
  g_rec.poseQpc = r.q0;
  g_rec.poseReturnQpc = r.q1;
  g_rec.poseDisplayTime = r.dt;
  g_rec.flags = cdp::time_flags::pose_observed | (p.matched ? cdp::time_flags::pose_matched : 0u) |
                (p.calls > 1 ? cdp::time_flags::pose_ambiguous : 0u);
  g_observed++;
  if (p.matched) g_matched++;
  if (p.calls > 1) g_ambiguous++;
  if (r.simOk) {
    g_rec.simTime = r.sim;
    g_rec.flags |= cdp::time_flags::sim_observed;
    g_simObserved++;
    if (g_prev.ok && g_prev.simOk) {
      const double dSim = r.sim - g_prev.sim, dPose = double(r.q0 - g_prev.q0) / double(g_qpcFreq > 0 ? g_qpcFreq : 1);
      if (dSim < 0) g_simBack++;
      if (dSim == 0) g_simSame++;
      if (kSimProven && SimStep(dSim, dPose)) {
        g_rec.flags |= cdp::time_flags::sim_stable;
        g_simStable++;
      }
    }
  }
  g_prev.ok = true;
  g_prev.simOk = r.simOk;
  g_prev.sim = r.sim;
  g_prev.q0 = r.q0;
}

// Into the motion companion (the identical record).
inline cmv::SlotTime ToMotion(const cdp::SlotTime& t) {
  static_assert(sizeof(cmv::SlotTime) == sizeof(cdp::SlotTime), "identical SlotTime");
  cmv::SlotTime m;
  memcpy(&m, &t, sizeof(m));
  return m;
}

// CockpitMaskLog: this commit's content time (appended to the frame line).
std::string LogText(int64_t commitQpc) {
  char b[220];
  const double f = g_qpcFreq > 0 ? 1000.0 / double(g_qpcFreq) : 0.0;
  if (!(g_rec.flags & cdp::time_flags::pose_observed)) {
    sprintf_s(b, " | time: no locate (%u calls) sim@commit %.4f", g_rec.locateCalls,
              g_simAtCommitOk ? g_simAtCommit : -1.0);
    return b;
  }
  sprintf_s(b, " | time: pose %lld (commit -%.3f ms, call %.3f ms) %s calls %u flags 0x%x sim %.4f sim@commit %.4f",
            static_cast<long long>(g_rec.poseQpc), (commitQpc - g_rec.poseQpc) * f,
            (g_rec.poseReturnQpc - g_rec.poseQpc) * f,
            g_pickTid == ptiming::g_topTid.load(std::memory_order_relaxed) ? "render" : "other", g_rec.locateCalls,
            g_rec.flags, g_rec.simTime, g_simAtCommitOk ? g_simAtCommit : -1.0);
  return b;
}

// ---- Install (config thread, from InstallXr with the verified table) ---------------------------------------------

bool Install(uint8_t* vis, void** disp) {
  if (g_slot) return true;
  if (!gpt::CodeMatches(vis + kLvTrampRva, kLvTramp, sizeof(kLvTramp)) || !gpt::Readable(&disp[kLvSlot])) {
    Log("cockpit mask: xrLocateViews trampoline differs; no content time (slotTime)");
    return false;
  }
  char where[200];
  void* orig = SlotOriginal(&disp[kLvSlot]);
  if (!orig || !gpt::ModRva(orig, where, sizeof(where))) {
    Log("cockpit mask: xrLocateViews dispatch entry is not in a loaded image; no content time");
    return false;
  }
  if (HMODULE world = GetModuleHandleW(L"World.dll"))
    g_modelTime = reinterpret_cast<const double*>(GetProcAddress(world, kModelTimeExport));
  g_orig = reinterpret_cast<LocateFn>(orig);
  if (!HookSlot(&disp[kLvSlot], reinterpret_cast<void*>(&H_LocateViews), nullptr)) {
    Log("cockpit mask: could not hook xrLocateViews; no content time");
    return false;
  }
  g_slot = &disp[kLvSlot];
  AcquireSRWLockExclusive(&g_lock);
  for (Rec& r : g_ring) r = Rec{};
  g_ringN = 0;
  ReleaseSRWLockExclusive(&g_lock);
  g_prev = Prev{};
  g_on = true;
  g_hooked = true;
  Log("cockpit mask: xrLocateViews hooked (-> %s): content time per slot; model time %s", where,
      g_modelTime.load() ? "from World.dll wTime::modelTime" : "not found (simTime not observed)");
  return true;
}

// R76: our hook still in slot 36 (another feature that hooks it after us bypasses ours: the loader registry keeps one
// true original per slot, no chaining). Displaced: the companions stop advertising slot_time (no records behind it).
void CheckSlot() {
  if (g_slot && g_hooked.load() && *g_slot != reinterpret_cast<void*>(&H_LocateViews)) {
    g_hooked = false;
    Log("cockpit mask: the xrLocateViews dispatch slot was replaced by another hook: no content time (slotTime) until "
        "the export is reinstalled");
  }
}

void Uninstall() {
  g_on = false;
  g_hooked = false;
  g_next = nullptr;  // R93: a chained observer (FrameTimeline) re-arms on its own once the slot is free
  if (g_slot) {
    // R76: the original back only while the slot still holds our hook (else it is another feature's now).
    if (*g_slot == reinterpret_cast<void*>(&H_LocateViews)) UnhookSlot(g_slot, reinterpret_cast<void*>(g_orig));
    g_slot = nullptr;
  }
  // A call in flight holds g_busy across the runtime's xrLocateViews (it reads the result after it returns).
  for (int i = 0; i < 2000 && g_busy.load(); ++i) Sleep(1);
  if (g_busy.load()) Log("cockpit mask: xrLocateViews hook still busy after 2 s at uninstall");
}

std::string StatsText() {
  auto u = [](const std::atomic<uint64_t>& a) { return static_cast<unsigned long long>(a.load()); };
  char b[400];
  sprintf_s(b,
            "content time: xrLocateViews %s (%llu calls, %llu off the render thread, %llu failed); slots %llu with "
            "the pose call (%llu orientation-matched, %llu ambiguous), %llu without; simTime %s %llu (stable %llu, "
            "same %llu, backwards %llu)",
            g_hooked.load() ? "hooked" : "not hooked", u(g_calls), u(g_callsOff), u(g_failed), u(g_observed),
            u(g_matched), u(g_ambiguous), u(g_noLocate), g_modelTime.load() ? "observed" : "not found", u(g_simObserved),
            u(g_simStable), u(g_simSame), u(g_simBack));
  return b;
}

}  // namespace ctime
