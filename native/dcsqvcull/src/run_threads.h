// Runnable-but-waiting threads of the DCS process ([Suite] RunnableThreads,
// default 0; R22 E3). Measure only: nothing is scheduled differently, and with
// the key at 0 none of this runs (the recorder pools' job-start hook pointer
// stays null: one relaxed load per job).
//
// Samples. Every [Suite] RunnableThreadsEvery-th frame (default 16) the
// render thread takes one NtQuerySystemInformation(SystemProcessInformation)
// snapshot at render entry (RenderGraph::render, shadow_batch.h's import hook
// that the recorders use: g_measureObserver; the first call after each
// xrEndFrame wrapper entry), and in the frame half-way between two of those the first recorder
// job to start (defrec::g_jobStartHook, on its worker thread) takes one. Each
// snapshot stores every DCS thread's KTHREAD state and wait reason: Running
// (2), runnable-but-waiting = Ready (1), Standby (3), Transition (6),
// DeferredReady (7), else waiting. The snapshot is synchronous on the thread
// at that point (no helper thread that would itself displace a worker); its
// cost (the system-wide call, about 0.2-1 ms [I]) delays that thread after the
// states were read, so sampled frames are listed apart from all frames when
// late recorder jobs are compared (bias check).
//
// Per frame (render thread, every frame while measuring): the recorders' late
// and wait counters at render entry, QueryThreadCycleTime at render entry and
// at DCS's xrEndFrame wrapper entry (gpu_pass_timing.h's hook): the pass
// region's run share (cycles / wall x TSC), also without the recorder wait.
//
// Per thread over a window (suite thread): kernel+user time from one snapshot
// at the start and one at the end, with a role (render, xrWaitFrame caller,
// process main thread = the earliest created, our recorder and planner
// workers) or else the thread description and start address (module+RVA).
//
// Gate (R22 E3): runnable-waiting >= 2 at render entry in more than 30% of
// the heavy sampled frames (frame time >= the window's median), or a pass
// region run share below 95% -> PASS (worth building the yield stub / cap).
//
// Included once from main.cpp after gpu_pass_timing.h (gpt::InstallXr,
// gpt::ModRva) and the recorders (shrec, gbrec, shadowinst), before
// frame_start.h and view_profile.h.
#pragma once

namespace rthreads {

// ---------------------------------------------------------------------------
// SystemProcessInformation layout (x64) and the pure walk (offline tested)
// ---------------------------------------------------------------------------

struct SysThread {
  LARGE_INTEGER kernel, user, create;
  ULONG waitTime;
  void* start;
  HANDLE pid, tid;
  LONG priority, basePriority;
  ULONG contextSwitches, state, waitReason;
};
static_assert(sizeof(SysThread) == 0x50, "SYSTEM_THREAD_INFORMATION size");
static_assert(offsetof(SysThread, tid) == 0x30 && offsetof(SysThread, state) == 0x44, "SYSTEM_THREAD_INFORMATION");
constexpr size_t kProcNumThreads = 0x04, kProcPid = 0x50, kProcThreads = 0x100;
constexpr ULONG kSystemProcessInformation = 5;
constexpr LONG kStatusInfoLengthMismatch = static_cast<LONG>(0xC0000004);

inline bool IsRunning(uint32_t s) { return s == 2; }
inline bool IsRunnable(uint32_t s) { return s == 1 || s == 3 || s == 6 || s == 7; }

// Calls f(const SysThread&) for every thread of process pid in a
// SystemProcessInformation buffer of len bytes. False when pid is not in it.
template <class F>
bool ForEachThread(const uint8_t* buf, size_t len, DWORD pid, F f) {
  size_t off = 0;
  while (off + kProcThreads <= len) {
    const uint8_t* p = buf + off;
    const ULONG next = *reinterpret_cast<const ULONG*>(p);
    const ULONG n = *reinterpret_cast<const ULONG*>(p + kProcNumThreads);
    const uintptr_t id = reinterpret_cast<uintptr_t>(*reinterpret_cast<HANDLE const*>(p + kProcPid));
    if (id == pid) {
      if (off + kProcThreads + static_cast<size_t>(n) * sizeof(SysThread) > len) return false;
      const SysThread* t = reinterpret_cast<const SysThread*>(p + kProcThreads);
      for (ULONG i = 0; i < n; ++i) f(t[i]);
      return true;
    }
    if (!next) break;
    off += next;
  }
  return false;
}

using NtQsiFn = LONG(NTAPI*)(ULONG, void*, ULONG, ULONG*);
using NtQitFn = LONG(NTAPI*)(HANDLE, int, void*, ULONG, ULONG*);
NtQsiFn g_nqsi = nullptr;
NtQitFn g_nqit = nullptr;

bool ResolveNt() {
  if (g_nqsi) return true;
  HMODULE nt = GetModuleHandleW(L"ntdll.dll");
  if (!nt) return false;
  g_nqsi = reinterpret_cast<NtQsiFn>(GetProcAddress(nt, "NtQuerySystemInformation"));
  g_nqit = reinterpret_cast<NtQitFn>(GetProcAddress(nt, "NtQueryInformationThread"));
  return g_nqsi != nullptr;
}

// One snapshot into buf (grown as needed; suite thread). Bytes used, 0 on failure.
size_t QueryAll(std::vector<uint8_t>& buf) {
  if (!ResolveNt()) return 0;
  if (buf.size() < (1u << 20)) buf.resize(1u << 20);
  for (int tries = 0; tries < 6; ++tries) {
    ULONG len = 0;
    const LONG st = g_nqsi(kSystemProcessInformation, buf.data(), static_cast<ULONG>(buf.size()), &len);
    if (st >= 0) return len ? len : buf.size();
    if (st != kStatusInfoLengthMismatch) return 0;
    buf.resize(std::max<size_t>(buf.size() * 2, static_cast<size_t>(len) + (256u << 10)));
  }
  return 0;
}

// ---------------------------------------------------------------------------
// Samples and per-frame records (written by the render thread / a worker,
// read by the suite thread)
// ---------------------------------------------------------------------------

enum Point : uint8_t { kEntry = 0, kJob = 1 };
struct SampleHdr {
  int64_t qpc = 0;      // the point (render entry: the collect end QPC; job: the hook's QPC)
  uint64_t frame = 0;   // frame index (render entries since the session began)
  uint8_t point = kEntry;
  uint16_t running = 0, runnable = 0, waiting = 0, total = 0;
  uint32_t first = 0, count = 0;  // thread states in g_thr
  int64_t costTicks = 0;          // the snapshot call plus the walk
};
struct ThrState {
  uint32_t tid;
  uint8_t state, reason;
};
struct FrameRec {
  int64_t qpc = 0;       // render entry
  uint64_t frame = 0;
  uint64_t lateCum = 0;  // recorder jobs still running at their pass (shadow + gbuffer), cumulative
  uint64_t waitNsCum = 0;
  uint64_t cyc = 0;      // render-thread cycles at entry
  int64_t qpcXr = 0;     // xrEndFrame wrapper entry after it (0 = not seen)
  uint64_t cycXr = 0;
};

constexpr int kMaxSamples = 2048;
constexpr int kMaxThr = kMaxSamples * 320;
constexpr size_t kMaxFrames = 1 << 16;

std::atomic<bool> g_on{false};
std::atomic<DWORD> g_rt{0};
int g_every = 16;
double g_tscHz = 1;
DWORD g_pid = 0;
uint8_t* g_buf = nullptr;
ULONG g_cap = 0;
SampleHdr* g_samples = nullptr;
ThrState* g_thr = nullptr;
std::atomic<int> g_nSamples{0};
int g_nThr = 0;  // under g_busy
std::atomic<bool> g_busy{false}, g_armJob{false};
std::atomic<uint64_t> g_skipBusy{0}, g_skipFull{0}, g_skipBuf{0}, g_entryTicks{0};
std::atomic<ULONG> g_needCap{0};
uint64_t g_frame = 0;            // render thread
bool g_entrySampled = false;     // render thread: this frame's entry was sampled (frame_start.h reads it)
uint64_t g_jobFrame = 0;         // frame whose job start is armed
std::mutex g_mutex;
std::vector<FrameRec> g_frames;  // g_mutex
std::atomic<DWORD> g_xrWaitTid{0};  // set by frame_start.h (the xrWaitFrame caller)

inline int64_t Qpc() {
  LARGE_INTEGER q;
  QueryPerformanceCounter(&q);
  return q.QuadPart;
}

// The recorders' counters (relaxed; vprof::TakeRec reads the same ones).
void RecCounters(uint64_t* late, uint64_t* waitNs) {
  const auto rl = std::memory_order_relaxed;
  uint64_t l = 0, w = 0;
  if (shrec::g_state.load(rl) == 1)
    for (int c = 0; c < shrec::kSlots; ++c) {
      l += shrec::g_casc[c].st.passReason[shrec::kPLate].load(rl);
      w += shrec::g_casc[c].st.waitNs.load(rl);
    }
  if (gbrec::g_state.load(rl) == 1)
    for (int k = 0; k < gbrec::kSlots; ++k) {
      l += gbrec::g_slot[k].st.passReason[gbrec::kPLate].load(rl);
      w += gbrec::g_slot[k].st.waitNs.load(rl);
    }
  *late = l;
  *waitNs = w;
}

void Sample(uint8_t point, uint64_t frame, int64_t qpc) {
  bool idle = false;
  if (!g_busy.compare_exchange_strong(idle, true, std::memory_order_acquire)) {
    g_skipBusy.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  const int n = g_nSamples.load(std::memory_order_relaxed);
  if (n >= kMaxSamples || g_nThr >= kMaxThr) {
    g_skipFull.fetch_add(1, std::memory_order_relaxed);
    g_busy.store(false, std::memory_order_release);
    return;
  }
  const int64_t t0 = Qpc();
  ULONG len = 0;
  const LONG st = g_nqsi(kSystemProcessInformation, g_buf, g_cap, &len);
  if (st < 0) {
    if (st == kStatusInfoLengthMismatch) g_needCap.store(len, std::memory_order_relaxed);
    g_skipBuf.fetch_add(1, std::memory_order_relaxed);
    g_busy.store(false, std::memory_order_release);
    return;
  }
  SampleHdr& h = g_samples[n];
  h = SampleHdr{};
  h.qpc = qpc;
  h.frame = frame;
  h.point = point;
  h.first = static_cast<uint32_t>(g_nThr);
  ForEachThread(g_buf, len ? len : g_cap, g_pid, [&](const SysThread& t) {
    const uint32_t s = t.state;
    if (IsRunning(s))
      ++h.running;
    else if (IsRunnable(s))
      ++h.runnable;
    else
      ++h.waiting;
    ++h.total;
    if (g_nThr < kMaxThr)
      g_thr[g_nThr++] = {static_cast<uint32_t>(reinterpret_cast<uintptr_t>(t.tid)), static_cast<uint8_t>(s),
                         static_cast<uint8_t>(t.waitReason)};
  });
  h.count = static_cast<uint32_t>(g_nThr) - h.first;
  h.costTicks = Qpc() - t0;
  g_nSamples.store(n + 1, std::memory_order_release);
  g_busy.store(false, std::memory_order_release);
}

void JobStart() {
  if (!g_armJob.load(std::memory_order_relaxed) || !g_armJob.exchange(false)) return;
  Sample(kJob, g_jobFrame, Qpc());
}

// Render entry of a frame (qpc: now).
void OnEntry(int64_t qpc) {
  if (!g_on.load(std::memory_order_relaxed)) return;
  const DWORD tid = GetCurrentThreadId();
  DWORD rt = g_rt.load(std::memory_order_relaxed);
  if (!rt) g_rt.store(rt = tid, std::memory_order_relaxed);
  if (tid != rt) return;
  const int64_t a = Qpc();
  FrameRec r;
  r.qpc = qpc;
  r.frame = ++g_frame;
  RecCounters(&r.lateCum, &r.waitNsCum);
  ULONG64 cyc = 0;
  QueryThreadCycleTime(GetCurrentThread(), &cyc);
  r.cyc = cyc;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_frames.size() < kMaxFrames) g_frames.push_back(r);
  }
  g_entrySampled = false;
  const uint64_t ph = g_frame % static_cast<uint64_t>(g_every);
  if (ph == 0) {
    Sample(kEntry, g_frame, qpc);
    g_entrySampled = true;
  } else if (ph == static_cast<uint64_t>(g_every / 2)) {
    g_jobFrame = g_frame;
    g_armJob.store(true, std::memory_order_relaxed);
  }
  g_entryTicks.fetch_add(static_cast<uint64_t>(Qpc() - a), std::memory_order_relaxed);
}

bool g_entryOpen = false;  // render thread: an xrEndFrame wrapper entry since the last render entry

// shadowbatch::HookRender (RenderGraph::render entry; may run more than once
// a frame): the first call after an xrEndFrame wrapper entry is the frame's
// render entry (every call when the xrEndFrame hook is not installed).
void OnRenderEntry(void*, void*) {
  if (!g_on.load(std::memory_order_relaxed)) return;
  if (gpt::g_xrSlot && !g_entryOpen) return;
  g_entryOpen = false;
  OnEntry(Qpc());
}

// gpu_pass_timing.h's xrEndFrame wrapper hook, before the original.
void OnXrEndEnter(DWORD tid) {
  if (!g_on.load(std::memory_order_relaxed) || tid != g_rt.load(std::memory_order_relaxed)) return;
  g_entryOpen = true;
  ULONG64 cyc = 0;
  QueryThreadCycleTime(GetCurrentThread(), &cyc);
  const int64_t q = Qpc();
  std::lock_guard<std::mutex> lock(g_mutex);
  if (!g_frames.empty() && !g_frames.back().qpcXr) {
    g_frames.back().qpcXr = q;
    g_frames.back().cycXr = cyc;
  }
}

// ---------------------------------------------------------------------------
// Window analysis (pure; offline tested)
// ---------------------------------------------------------------------------

struct Bin {
  int n = 0;
  double late = 0, frameMs = 0;
};
constexpr int kBins = 5;
const char* const kBinName[kBins] = {"0", "1", "2-3", "4-7", "8+"};
inline int BinOf(int r) { return r <= 0 ? 0 : r == 1 ? 1 : r <= 3 ? 2 : r <= 7 ? 3 : 4; }

struct PointStats {
  int n = 0;
  double running = 0, runnable = 0, p95 = 0, max = 0, share2 = -1;  // share2: % of samples with >= 2 runnable
  double costUs = 0, costMaxUs = 0;
};
struct Summary {
  int frames = 0;
  double frameMs = 0, latePerFrame = 0;
  PointStats entry, job;
  int heavyN = 0;
  double heavyShare2 = -1, medianMs = 0;
  double sampledLate = 0, sampledMs = 0;  // entry-sampled frames (bias check)
  Bin bins[kBins];
  double corrLate = 0, corrMs = 0;
  double runShare = -1, runShareExWait = -1, passRegionMs = 0, recWaitMs = 0;
  bool gate = false;
  bool valid = false;
};

inline double Pearson(const std::vector<double>& x, const std::vector<double>& y) {
  const size_t n = std::min(x.size(), y.size());
  if (n < 3) return 0;
  double mx = 0, my = 0;
  for (size_t i = 0; i < n; ++i) mx += x[i], my += y[i];
  mx /= n, my /= n;
  double sxy = 0, sxx = 0, syy = 0;
  for (size_t i = 0; i < n; ++i) {
    sxy += (x[i] - mx) * (y[i] - my);
    sxx += (x[i] - mx) * (x[i] - mx);
    syy += (y[i] - my) * (y[i] - my);
  }
  return sxx > 0 && syy > 0 ? sxy / std::sqrt(sxx * syy) : 0;
}

inline PointStats Points(const std::vector<SampleHdr>& s, uint8_t point, double qpcToUs) {
  PointStats p;
  std::vector<double> r;
  int ge2 = 0;
  for (const SampleHdr& h : s) {
    if (h.point != point) continue;
    ++p.n;
    p.running += h.running;
    p.runnable += h.runnable;
    r.push_back(h.runnable);
    if (h.runnable >= 2) ++ge2;
    const double us = h.costTicks * qpcToUs;
    p.costUs += us;
    p.costMaxUs = std::max(p.costMaxUs, us);
  }
  if (!p.n) return p;
  p.running /= p.n;
  p.runnable /= p.n;
  p.costUs /= p.n;
  p.share2 = 100.0 * ge2 / p.n;
  std::sort(r.begin(), r.end());
  p.p95 = r[std::min(r.size() - 1, static_cast<size_t>(r.size() * 0.95))];
  p.max = r.back();
  return p;
}

// frames: the window's per-frame records in order; samples: its snapshots.
inline Summary Summarize(const std::vector<FrameRec>& frames, const std::vector<SampleHdr>& samples, double qpcToUs,
                         double tscHz) {
  Summary s;
  s.entry = Points(samples, kEntry, qpcToUs);
  s.job = Points(samples, kJob, qpcToUs);
  // Per frame: time to the next entry and late jobs in between (consecutive indices only).
  std::unordered_map<uint64_t, std::pair<double, double>> byFrame;  // frame -> (ms, late)
  std::vector<double> allMs;
  double lateSum = 0, msSum = 0;
  double cyc = 0, wallS = 0, waitS = 0;
  for (size_t i = 0; i + 1 < frames.size(); ++i) {
    const FrameRec& a = frames[i];
    const FrameRec& b = frames[i + 1];
    if (b.frame != a.frame + 1 || b.qpc <= a.qpc) continue;
    const double ms = (b.qpc - a.qpc) * qpcToUs / 1000.0;
    const double late = static_cast<double>(b.lateCum >= a.lateCum ? b.lateCum - a.lateCum : 0);
    byFrame[a.frame] = {ms, late};
    allMs.push_back(ms);
    msSum += ms;
    lateSum += late;
    ++s.frames;
    if (a.qpcXr > a.qpc && a.cycXr >= a.cyc && a.qpcXr <= b.qpc) {
      cyc += static_cast<double>(a.cycXr - a.cyc);
      wallS += (a.qpcXr - a.qpc) * qpcToUs / 1e6;
      waitS += (b.waitNsCum >= a.waitNsCum ? b.waitNsCum - a.waitNsCum : 0) / 1e9;
    }
  }
  if (!s.frames) return s;
  s.valid = true;
  s.frameMs = msSum / s.frames;
  s.latePerFrame = lateSum / s.frames;
  if (wallS > 0 && tscHz > 0) {
    s.runShare = 100.0 * cyc / (wallS * tscHz);
    s.passRegionMs = wallS * 1000.0 / s.frames;
    s.recWaitMs = waitS * 1000.0 / s.frames;
    if (wallS > waitS) s.runShareExWait = 100.0 * cyc / ((wallS - waitS) * tscHz);
  }
  {
    std::vector<double> m = allMs;
    std::nth_element(m.begin(), m.begin() + m.size() / 2, m.end());
    s.medianMs = m[m.size() / 2];
  }
  std::vector<double> rx, lateY, msY;
  int heavy2 = 0, sampled = 0;
  for (const SampleHdr& h : samples) {
    if (h.point != kEntry) continue;
    auto it = byFrame.find(h.frame);
    if (it == byFrame.end()) continue;
    const double ms = it->second.first, late = it->second.second;
    ++sampled;
    s.sampledLate += late;
    s.sampledMs += ms;
    Bin& b = s.bins[BinOf(h.runnable)];
    ++b.n;
    b.late += late;
    b.frameMs += ms;
    rx.push_back(h.runnable);
    lateY.push_back(late);
    msY.push_back(ms);
    if (ms >= s.medianMs) {
      ++s.heavyN;
      if (h.runnable >= 2) ++heavy2;
    }
  }
  if (sampled) {
    s.sampledLate /= sampled;
    s.sampledMs /= sampled;
  }
  for (Bin& b : s.bins)
    if (b.n) b.late /= b.n, b.frameMs /= b.n;
  if (s.heavyN) s.heavyShare2 = 100.0 * heavy2 / s.heavyN;
  s.corrLate = Pearson(rx, lateY);
  s.corrMs = Pearson(rx, msY);
  s.gate = (s.heavyShare2 > 30.0) || (s.runShare >= 0 && s.runShare < 95.0);
  return s;
}

// ---------------------------------------------------------------------------
// Per-thread CPU (suite thread)
// ---------------------------------------------------------------------------

struct ThrCpu {
  int64_t cpu = 0;     // kernel + user, 100 ns
  int64_t create = 0;
  uintptr_t start = 0;
};
using CpuSnap = std::unordered_map<uint32_t, ThrCpu>;

std::vector<uint8_t> g_suiteBuf;

bool SnapCpu(CpuSnap& out) {
  out.clear();
  const size_t len = QueryAll(g_suiteBuf);
  if (!len) return false;
  return ForEachThread(g_suiteBuf.data(), len, GetCurrentProcessId(), [&](const SysThread& t) {
    out[static_cast<uint32_t>(reinterpret_cast<uintptr_t>(t.tid))] = {t.kernel.QuadPart + t.user.QuadPart,
                                                                       t.create.QuadPart,
                                                                       reinterpret_cast<uintptr_t>(t.start)};
  });
}

// Role of a thread (suite thread): ours and the known DCS ones, else its
// description and Win32 start address.
std::string Describe(uint32_t tid) {
  std::string out;
  HANDLE h = OpenThread(THREAD_QUERY_INFORMATION | THREAD_QUERY_LIMITED_INFORMATION, FALSE, tid);
  if (!h) return out;
  using GtdFn = HRESULT(WINAPI*)(HANDLE, PWSTR*);
  static GtdFn gtd = reinterpret_cast<GtdFn>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetThreadDescription"));
  if (gtd) {
    PWSTR d = nullptr;
    if (SUCCEEDED(gtd(h, &d)) && d) {
      if (*d) {
        char b[96];
        snprintf(b, sizeof(b), "\"%ls\" ", d);
        out += b;
      }
      LocalFree(d);
    }
  }
  void* start = nullptr;
  if (g_nqit && g_nqit(h, 9 /* ThreadQuerySetWin32StartAddress */, &start, sizeof(start), nullptr) >= 0 && start) {
    char m[200];
    if (gpt::ModRva(start, m, sizeof(m)))
      out += m;
    else {
      snprintf(m, sizeof(m), "start %p", start);
      out += m;
    }
  }
  CloseHandle(h);
  return out;
}

std::unordered_map<uint32_t, std::string> Roles(const CpuSnap& snap) {
  std::unordered_map<uint32_t, std::string> r;
  DWORD ids[32];
  int n = shrec::g_pool.ThreadIds(ids, 32);
  for (int i = 0; i < n; ++i) r[ids[i]] = "shadow rec worker " + std::to_string(i);
  n = gbrec::g_pool.ThreadIds(ids, 32);
  for (int i = 0; i < n; ++i) r[ids[i]] = "gbuffer rec worker " + std::to_string(i);
  for (int i = 0; i < shadowinst::kMaxWorkers; ++i)
    if (shadowinst::g_workers[i]) r[GetThreadId(shadowinst::g_workers[i])] = "shadow planner " + std::to_string(i);
  uint32_t mainTid = 0;
  int64_t first = INT64_MAX;
  for (auto& kv : snap)
    if (kv.second.create && kv.second.create < first) first = kv.second.create, mainTid = kv.first;
  if (mainTid) r[mainTid] = "process main thread";
  if (const DWORD x = g_xrWaitTid.load()) r[x] = x == mainTid ? "process main thread, xrWaitFrame caller" : "xrWaitFrame caller";
  if (const DWORD rt = ptiming::g_topTid.load()) r[rt] = "render thread";
  return r;
}

// The group a thread's CPU is summed under: its role kind, else the start module.
std::string GroupOf(const std::string& role, const std::string& desc) {
  if (!role.empty()) {
    const size_t k = role.find(" worker");
    if (k != std::string::npos) return role.substr(0, k) + " workers";
    if (role.rfind("shadow planner", 0) == 0) return "shadow planner";
    return role;
  }
  const size_t q = desc.rfind('"');
  std::string mod = q == std::string::npos ? desc : desc.substr(q + 2);
  const size_t plus = mod.find('+');
  if (plus != std::string::npos) mod = mod.substr(0, plus);
  return mod.empty() ? "?" : mod;
}

// ---------------------------------------------------------------------------
// Session and windows (suite thread)
// ---------------------------------------------------------------------------

bool g_installedXr = false;
std::atomic<bool> g_sessionOn{false};

bool Begin(int every, double tscHz) {
  if (g_sessionOn.load()) return true;
  if (!ResolveNt()) {
    Log("  runnable threads: NtQuerySystemInformation not available");
    return false;
  }
  std::vector<uint8_t> probe;
  const size_t len = QueryAll(probe);
  if (!len) {
    Log("  runnable threads: SystemProcessInformation query failed");
    return false;
  }
  g_cap = static_cast<ULONG>(std::max<size_t>(probe.size(), len * 2 + (1u << 20)));
  g_buf = static_cast<uint8_t*>(VirtualAlloc(nullptr, g_cap, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
  if (!g_samples) g_samples = new (std::nothrow) SampleHdr[kMaxSamples];  // kept across sessions
  if (!g_thr) g_thr = new (std::nothrow) ThrState[kMaxThr];
  if (!g_buf || !g_samples || !g_thr) {
    Log("  runnable threads: out of memory");
    if (g_buf) VirtualFree(g_buf, 0, MEM_RELEASE);
    delete[] g_samples;
    delete[] g_thr;
    g_buf = nullptr;
    g_samples = nullptr;
    g_thr = nullptr;
    return false;
  }
  g_pid = GetCurrentProcessId();
  g_every = std::max(2, std::min(1024, every));
  g_tscHz = tscHz > 0 ? tscHz : 1;
  g_nSamples = 0;
  g_nThr = 0;
  g_skipBusy = g_skipFull = g_skipBuf = g_entryTicks = 0;
  g_needCap = 0;
  g_frame = 0;
  g_armJob = false;
  g_rt = static_cast<DWORD>(ptiming::g_topTid.load());
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_frames.clear();
    g_frames.reserve(8192);
  }
  g_installedXr = !gpt::g_xrSlot && gpt::InstallXr();
  if (!gpt::g_xrSlot) Log("  runnable threads: xrEndFrame wrapper not hooked; no pass-region run share");
  g_entryOpen = false;
  // Render entry: the recorders' RenderGraph::render import hook (installed here when no recorder did; it only
  // forwards, and stays like the recorders' own).
  if (!shadowbatch::g_renderSlot && !shadowbatch::g_renderSlot2 && !shadowbatch::InstallRenderHook())
    Log("  runnable threads: RenderGraph::render entry not hooked; no render-entry samples");
  shadowbatch::g_measureObserver = &OnRenderEntry;
  defrec::g_jobStartHook = &JobStart;
  g_sessionOn = true;
  g_on = true;
  return true;
}

void End() {
  if (!g_sessionOn.exchange(false)) return;
  g_on = false;
  shadowbatch::RenderObserverFn ob = &OnRenderEntry;
  shadowbatch::g_measureObserver.compare_exchange_strong(ob, nullptr);
  defrec::g_jobStartHook = nullptr;
  g_armJob = false;
  for (int i = 0; i < 200 && g_busy.load(); ++i) Sleep(5);
  Sleep(20);  // a worker that passed the hook pointer before it was cleared
  if (g_installedXr && !gpt::g_sessDev) gpt::UninstallXr();
  g_installedXr = false;
  if (g_skipBuf.load())
    Log("  runnable threads: %llu snapshots skipped (buffer too small, %lu bytes wanted)",
        static_cast<unsigned long long>(g_skipBuf.load()), static_cast<unsigned long>(g_needCap.load()));
  // Samples stay readable until the next Begin; the snapshot buffer goes.
  for (int i = 0; i < 100 && g_busy.load(); ++i) Sleep(5);
  if (g_buf) VirtualFree(g_buf, 0, MEM_RELEASE);
  g_buf = nullptr;
}

struct Window {
  int64_t qa = 0;
  int s0 = 0;
  CpuSnap cpu0;
};

Window Open() {
  Window w;
  w.qa = Qpc();
  w.s0 = g_nSamples.load(std::memory_order_acquire);
  SnapCpu(w.cpu0);
  return w;
}

struct ThreadRow {
  uint32_t tid;
  std::string role, desc, group;
  double cpuPct, runPct, readyPct;  // CPU % of one core; % of samples running / runnable-waiting
  int samples;
};
struct Result {
  Summary sum;
  std::vector<ThreadRow> threads;  // by CPU, largest first
  std::vector<SampleHdr> samples;
  double wallS = 0;
  uint64_t skipped = 0;
};

Result Close(const Window& w) {
  Result r;
  const int64_t qb = Qpc();
  r.wallS = (qb - w.qa) * g_qpcToUs / 1e6;
  CpuSnap cpu1;
  SnapCpu(cpu1);
  const int s1 = g_samples ? g_nSamples.load(std::memory_order_acquire) : 0;
  struct Cnt {
    int run = 0, ready = 0, all = 0;
  };
  std::unordered_map<uint32_t, Cnt> st;
  for (int i = w.s0; i < s1; ++i) {
    const SampleHdr& h = g_samples[i];
    r.samples.push_back(h);
    for (uint32_t k = 0; k < h.count; ++k) {
      const ThrState& t = g_thr[h.first + k];
      Cnt& c = st[t.tid];
      if (IsRunning(t.state)) ++c.run;
      if (IsRunnable(t.state)) ++c.ready;
      ++c.all;
    }
  }
  std::vector<FrameRec> fr;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    for (const FrameRec& f : g_frames)
      if (f.qpc >= w.qa && f.qpc <= qb) fr.push_back(f);
  }
  r.sum = Summarize(fr, r.samples, g_qpcToUs, g_tscHz);
  r.skipped = g_skipBusy.load() + g_skipFull.load() + g_skipBuf.load();
  const auto roles = Roles(cpu1);
  const double wall100ns = r.wallS * 1e7;
  for (auto& kv : cpu1) {
    auto it = w.cpu0.find(kv.first);
    const int64_t d = kv.second.cpu - (it != w.cpu0.end() ? it->second.cpu : 0);
    ThreadRow t{};
    t.tid = kv.first;
    t.cpuPct = wall100ns > 0 ? 100.0 * d / wall100ns : 0;
    auto rl = roles.find(kv.first);
    if (rl != roles.end()) t.role = rl->second;
    auto sc = st.find(kv.first);
    if (sc != st.end() && sc->second.all) {
      t.samples = sc->second.all;
      t.runPct = 100.0 * sc->second.run / t.samples;
      t.readyPct = 100.0 * sc->second.ready / t.samples;
    }
    r.threads.push_back(t);
  }
  std::sort(r.threads.begin(), r.threads.end(), [](const ThreadRow& a, const ThreadRow& b) { return a.cpuPct > b.cpuPct; });
  // Descriptions for the busier threads only (each opens the thread).
  for (size_t i = 0; i < r.threads.size(); ++i) {
    ThreadRow& t = r.threads[i];
    if (i < 48 || t.cpuPct >= 1.0) t.desc = Describe(t.tid);
    t.group = GroupOf(t.role, t.desc);
  }
  return r;
}

// ---------------------------------------------------------------------------
// Report
// ---------------------------------------------------------------------------

std::string Share(double v) {
  if (v < 0) return "n/a";
  char b[16];
  snprintf(b, sizeof(b), "%.0f%%", v);
  return b;
}

// Compact (one or two lines, yaw rows) or full (with the bins and the thread table).
void LogResult(const char* label, const Result& r, bool full) {
  const Summary& s = r.sum;
  if (!s.valid) {
    Log("  runnable threads %s: no frames recorded (render entry not seen)", label);
    return;
  }
  Log("  runnable threads %s: render entry %d samples: running %.1f, runnable-waiting %.2f (p95 %.0f, max %.0f), "
      ">= 2 waiting in %s (heavy frames %s of %d); recorder job start %d samples: running %.1f, waiting %.2f, >= 2 in "
      "%s | late recorder jobs %.2f/frame (sampled frames %.2f), frame %.2f ms | pass-region run share %s (without "
      "recorder wait %s) | gate %s",
      label, s.entry.n, s.entry.running, s.entry.runnable, s.entry.p95, s.entry.max, Share(s.entry.share2).c_str(),
      Share(s.heavyShare2).c_str(), s.heavyN, s.job.n, s.job.running, s.job.runnable, Share(s.job.share2).c_str(),
      s.latePerFrame, s.sampledLate, s.frameMs, Share(s.runShare).c_str(), Share(s.runShareExWait).c_str(),
      s.gate ? "PASS" : "FAIL");
  if (!full) return;
  std::string bins;
  char buf[160];
  for (int b = 0; b < kBins; ++b) {
    if (!s.bins[b].n) continue;
    snprintf(buf, sizeof(buf), "%s %s: %d frames, late %.2f, frame %.2f ms", bins.empty() ? "" : ";", kBinName[b],
             s.bins[b].n, s.bins[b].late, s.bins[b].frameMs);
    bins += buf;
  }
  Log("  runnable threads %s: by waiting threads at render entry:%s; correlation with late jobs %+.2f, with frame "
      "time %+.2f",
      label, bins.empty() ? " -" : bins.c_str(), s.corrLate, s.corrMs);
  Log("  runnable threads %s: pass region (render entry -> xrEndFrame) %.2f ms/frame, recorder wait %.3f ms/frame; "
      "snapshot cost %.0f us mean (max %.0f) at entry, %.0f us at job start; %llu skipped; gate (R22 E3): heavy frames "
      "with >= 2 waiting %s > 30%% or run share %s < 95%% -> %s",
      label, s.passRegionMs, s.recWaitMs, s.entry.costUs, s.entry.costMaxUs, s.job.costUs,
      static_cast<unsigned long long>(r.skipped), Share(s.heavyShare2).c_str(), Share(s.runShare).c_str(),
      s.gate ? "PASS" : "FAIL");
  // Groups.
  std::map<std::string, std::pair<double, int>> groups;
  double total = 0;
  for (const ThreadRow& t : r.threads) {
    auto& g = groups[t.group];
    g.first += t.cpuPct;
    ++g.second;
    total += t.cpuPct;
  }
  std::vector<std::pair<std::string, std::pair<double, int>>> gv(groups.begin(), groups.end());
  std::sort(gv.begin(), gv.end(), [](const auto& a, const auto& b) { return a.second.first > b.second.first; });
  Log("  runnable threads %s: CPU over %.1f s, %zu threads, %.0f%% of one logical CPU in total; by group (CPU %% of "
      "one logical CPU, threads):",
      label, r.wallS, r.threads.size(), total);
  std::string line;
  for (size_t i = 0; i < gv.size() && i < 14; ++i) {
    snprintf(buf, sizeof(buf), "%s%s %.0f%% (%d)", line.empty() ? "    " : ", ", gv[i].first.c_str(),
             gv[i].second.first, gv[i].second.second);
    line += buf;
  }
  Log("%s", line.c_str());
  Log("    %-8s %6s %7s %7s  %s", "tid", "CPU%", "run%", "wait%", "role / description / start");
  for (size_t i = 0; i < r.threads.size() && i < 24; ++i) {
    const ThreadRow& t = r.threads[i];
    if (t.cpuPct < 0.5 && i >= 12) break;
    char run[16] = "-", rdy[16] = "-";
    if (t.samples) {
      snprintf(run, sizeof(run), "%.0f", t.runPct);
      snprintf(rdy, sizeof(rdy), "%.0f", t.readyPct);
    }
    Log("    %-8u %6.1f %7s %7s  %s%s%s", t.tid, t.cpuPct, run, rdy, t.role.c_str(),
        !t.role.empty() && !t.desc.empty() ? " / " : "", t.desc.c_str());
  }
  Log("    (run%% / wait%%: share of this window's snapshots in which the thread was running / runnable-waiting)");
}

// still -> turning (RotationProfile).
void LogCompare(const Result& a, const Result& b) {
  const Summary& s = a.sum;
  const Summary& t = b.sum;
  if (!s.valid || !t.valid) return;
  Log("  runnable threads still -> turning: waiting at render entry %.2f -> %.2f (>= 2 in %s -> %s), at job start "
      "%.2f -> %.2f; late recorder jobs %.2f -> %.2f /frame; pass-region run share %s -> %s; frame %.2f -> %.2f ms",
      s.entry.runnable, t.entry.runnable, Share(s.entry.share2).c_str(), Share(t.entry.share2).c_str(),
      s.job.runnable, t.job.runnable, s.latePerFrame, t.latePerFrame, Share(s.runShare).c_str(),
      Share(t.runShare).c_str(), s.frameMs, t.frameMs);
}

// Standalone phase: [Suite] RunnableThreadsSec at the current (held) view.
void RunPhase(int sec, int every, double tscHz, std::atomic<bool>& abort) {
  if (!Begin(every, tscHz)) return;
  Sleep(500);
  const Window w = Open();
  for (int t = 0; t < sec * 1000 && !abort; t += 50) Sleep(50);
  const Result r = Close(w);
  End();
  LogResult("phase", r, true);
  Log("  runnable threads: measurement cost at render entry %.4f ms/frame averaged (per-frame counters plus the "
      "snapshots: one every %d frames at entry, one every %d at a job start, on the worker)",
      r.sum.frames ? g_entryTicks.load() * g_qpcToUs / 1000.0 / std::max(1, r.sum.frames) : 0.0, g_every, g_every);
}

}  // namespace rthreads
