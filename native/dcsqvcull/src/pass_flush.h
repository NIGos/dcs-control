// Explicit ID3D11DeviceContext::Flush at chosen render-pass boundaries
// ([Model] PassFlush, default 0 = off: nothing installed). R20 2.1 (h).
//
// Why. d3d11 hands the immediate context's commands to the GPU when its
// command buffer fills, or on Flush/Present. When the render thread spends
// milliseconds of CPU inside a pass, the commands of the work before it may
// wait in the driver while the GPU idles (GpuPassTiming: 1.46 ms idle between
// top-level passes, 3.37 ms from xrEndFrame to the next frame's first pass).
// Flush changes neither the commands nor their order, only when the driver
// submits them, so what is rendered is identical by construction. It costs
// render-thread CPU (the driver's submit, about 10-30 us per call [I]).
//
// Mask bits ([Model] PassFlush, decimal or 0x hex):
//   0x01 after each shadow cascade pass (CascadeShadowPassData)
//   0x02 after the last cascade of a run of consecutive cascades (at the next
//        boundary of another pass: the cascades' parent end or the next pass)
//   0x04 after each G-buffer execution (GBufferPassData)
//   0x08 after the last G-buffer execution of a consecutive run (as 0x02)
//   0x10 after each top-level pass
//   0x20 after each ExecuteCommandList batch of our recorders (shadow_rec.h:
//        a cascade's list(s); gb_rec.h: a segment's lists)
//   0x40 right after DCS's xrBeginFrame wrapper returns (Visualizer OpenXR
//        vt[9]: xrBeginFrame + 4 x acquire/wait swapchain image, render thread
//        [V R15]); needs that hook
//   0x80 before the frame's first top-level pass (the first one after the
//        quad-frame counter moved)
// An "each" bit makes the matching group bit redundant (the group flush is
// then not scheduled). Several reasons at one boundary give one Flush.
//
// Where. pass_timing.h's execute hook calls g_flushBoundary at the start of
// every pass (before g_boundary) and at its end (after g_boundary, so
// GpuPassTiming's end timestamp is in the flushed batch). Flush runs only on
// the render thread: latched as the thread of the first cascade pass (DCS's
// cascades and the recorders' Execute calls run there [V shadow_rec.h]). The
// pass kind comes from the pass object's RTTI, cached by vtable (lock-free).
//
// Frame-start probe (bench mode 30, both blocks): render-thread QPC at the end
// of DCS's xrEndFrame wrapper (gpu_pass_timing.h's hook calls NoteXrEnd), at
// the start and end of the xrBeginFrame wrapper, and at the first top-level
// pass of the next frame: what the render thread does in the GPU's frame-start
// gap.
//
// Included once from main.cpp after pass_timing.h, sigscan.h and the globals
// it uses (Log, g_qpcToUs, HookSlot, SlotOriginal, UnhookSlot), before
// shadow_rec.h and gb_rec.h (they call AfterExecute).
#pragma once

namespace pflush {

enum : uint32_t {
  kEachCascade = 1u << 0,
  kCascadeGroup = 1u << 1,
  kEachGBuffer = 1u << 2,
  kGBufferGroup = 1u << 3,
  kEachTop = 1u << 4,
  kRecExecute = 1u << 5,
  kAfterXrBegin = 1u << 6,
  kFirstPass = 1u << 7,
  kMaskBits = 0xffu,
};
constexpr int kReasons = 8;
const char* const kReasonNames[kReasons] = {"each cascade", "cascade group", "each g-buffer", "g-buffer group",
                                            "each top-level", "recorder execute", "after xrBeginFrame",
                                            "before first pass"};

enum Kind : uint8_t { kOther = 0, kCascade = 1, kGBuffer = 2 };

// Pass kind from pass_timing's short RTTI name ("CascadeShadowPassData
// (shadow)", "GBufferPassData").
inline uint8_t KindFromName(const std::string& n) {
  auto starts = [&](const char* p) {
    const size_t k = strlen(p);
    return n.compare(0, k, p) == 0 && (n.size() == k || n[k] == ' ');
  };
  if (starts("CascadeShadowPassData")) return kCascade;
  if (starts("GBufferPassData")) return kGBuffer;
  return kOther;
}

// The decision at each boundary (render thread; offline tested). Returns the
// reason bits that ask for a Flush at this boundary (0: none).
struct Planner {
  uint32_t mask = 0;
  uint8_t pending = kOther;  // kind whose group flush is due at the next boundary of another kind

  static uint32_t EachBit(uint8_t k) { return k == kCascade ? kEachCascade : kEachGBuffer; }
  static uint32_t GroupBit(uint8_t k) { return k == kCascade ? kCascadeGroup : kGBufferGroup; }

  uint32_t Due(uint8_t kind) {
    if (pending == kOther || kind == pending) return 0;
    const uint32_t r = mask & GroupBit(pending);
    pending = kOther;
    return r;
  }
  // A pass starts; firstOfFrame: the first top-level pass since the frame counter moved.
  uint32_t Begin(uint8_t kind, int depth, bool firstOfFrame) {
    uint32_t r = Due(kind);
    if (depth == 0 && firstOfFrame) r |= mask & kFirstPass;
    return r;
  }
  uint32_t End(uint8_t kind, int depth) {
    uint32_t r = Due(kind);
    if (kind != kOther) {
      if (mask & EachBit(kind))
        r |= EachBit(kind);
      else if (mask & GroupBit(kind))
        pending = kind;
    }
    if (depth == 0) r |= mask & kEachTop;
    return r;
  }
};

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

std::atomic<uint32_t> g_mask{0};
ID3D11DeviceContext* g_ctx = nullptr;     // DCS's immediate context (our reference)
std::atomic<uint64_t>* g_frameCounter = nullptr;
std::atomic<DWORD> g_rt{0};               // render thread (first cascade pass)
std::atomic<bool> g_probe{false};         // frame-start probe recording
Planner g_plan;                           // render thread
uint64_t g_lastFrame = ~0ull;             // render thread: frame counter at the last top-level start

// Counters: render-thread writer, read by the suite thread.
std::atomic<uint64_t> g_flushes{0}, g_flushTicks{0};
std::atomic<uint64_t> g_byReason[kReasons];

inline int64_t Qpc() {
  LARGE_INTEGER q;
  QueryPerformanceCounter(&q);
  return q.QuadPart;
}
inline void Bump(std::atomic<uint64_t>& a, uint64_t d = 1) {
  a.store(a.load(std::memory_order_relaxed) + d, std::memory_order_relaxed);
}

// ---------------------------------------------------------------------------
// Kind cache: vtable -> kind, open addressing, lock-free. Key = vtable | (kind
// + 1) (vtables are 8-byte aligned). A full table classifies uncached.
// ---------------------------------------------------------------------------

constexpr int kCache = 256;
std::atomic<uintptr_t> g_cache[kCache];

bool VtableGuarded(void* pass, void** vtOut) {
  __try {
    *vtOut = *static_cast<void**>(pass);
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

uint8_t KindOf(void* pass) {
  void* vtp = nullptr;
  if (!pass || !VtableGuarded(pass, &vtp) || !vtp) return kOther;
  const uintptr_t vt = reinterpret_cast<uintptr_t>(vtp);
  if (vt & 7) return kOther;
  uint32_t h = static_cast<uint32_t>((vt >> 3) * 0x9E3779B1u) % kCache;
  for (int i = 0; i < kCache; ++i, h = (h + 1) % kCache) {
    const uintptr_t e = g_cache[h].load(std::memory_order_acquire);
    if (!e) break;
    if ((e & ~uintptr_t(7)) == vt) return static_cast<uint8_t>((e & 7) - 1);
  }
  const uint8_t k = KindFromName(ptiming::RttiName(pass));
  h = static_cast<uint32_t>((vt >> 3) * 0x9E3779B1u) % kCache;
  for (int i = 0; i < kCache; ++i, h = (h + 1) % kCache) {
    uintptr_t want = 0;
    if (g_cache[h].compare_exchange_strong(want, vt | (k + 1u), std::memory_order_acq_rel)) break;
    if ((want & ~uintptr_t(7)) == vt) break;  // another thread cached it
  }
  return k;
}

// ---------------------------------------------------------------------------
// Flush
// ---------------------------------------------------------------------------

void DoFlush(uint32_t why) {
  ID3D11DeviceContext* c = g_ctx;
  if (!c) return;
  const int64_t t0 = Qpc();
  c->Flush();
  Bump(g_flushTicks, static_cast<uint64_t>(Qpc() - t0));
  Bump(g_flushes);
  while (why) {
    unsigned long b;
    _BitScanForward(&b, why);
    why &= why - 1;
    if (b < kReasons) Bump(g_byReason[b]);
  }
}

// Our recorders, right after their ExecuteCommandList batch (render thread).
inline void AfterExecute() {
  if (!(g_mask.load(std::memory_order_relaxed) & kRecExecute)) return;
  if (GetCurrentThreadId() != g_rt.load(std::memory_order_relaxed)) return;
  DoFlush(kRecExecute);
}

// ---------------------------------------------------------------------------
// Frame-start probe (render thread writes while g_probe; the suite reads after
// stopping it and waiting)
// ---------------------------------------------------------------------------

struct Probe {
  uint64_t frames = 0;      // first top-level passes seen (after an xrEndFrame end, else after the counter moved)
  uint64_t withXr = 0;      // ... with an xrEndFrame end before them (as GpuPassTiming's frame start)
  uint64_t counterFirst = 0;  // ... that are also the first since the frame counter moved (bit 0x80's point)
  uint64_t withBegin = 0;   // ... with the xrBeginFrame wrapper between the two
  uint64_t beginOutside = 0;  // xrBeginFrame wrapper calls not between xrEndFrame and the first pass
  double xrToBeginUs = 0, beginUs = 0, beginToPassUs = 0, xrToPassUs = 0, firstPassUs = 0;
  uint64_t firstPassN = 0;
  std::vector<float> xrToPass;  // per frame (us)
  void* firstVt = nullptr;      // a first pass object (its name, reported)
};
Probe g_pr;
int64_t g_tXrEnd = 0, g_tB0 = 0, g_tB1 = 0;  // render thread
bool g_xrNoted = false;                      // an xrEndFrame end seen since the reset
void* g_firstOpen = nullptr;
int64_t g_tFirst = 0;

void ProbeReset() {
  g_pr = Probe{};
  g_pr.xrToPass.reserve(4096);
  g_tXrEnd = g_tB0 = g_tB1 = 0;
  g_xrNoted = false;
  g_firstOpen = nullptr;
}

// The end of DCS's xrEndFrame wrapper (gpu_pass_timing.h's hook, any thread).
inline void NoteXrEnd() {
  if (!g_probe.load(std::memory_order_relaxed) || GetCurrentThreadId() != g_rt.load(std::memory_order_relaxed)) return;
  g_tXrEnd = Qpc();
  g_tB0 = g_tB1 = 0;
  g_xrNoted = true;
}

void ProbeFirstPass(void* pass, int64_t now, bool counterFirst) {
  Probe& p = g_pr;
  ++p.frames;
  if (counterFirst) ++p.counterFirst;
  const double k = g_qpcToUs;
  if (g_tXrEnd) {
    ++p.withXr;
    p.xrToPassUs += (now - g_tXrEnd) * k;
    if (p.xrToPass.size() < 100000) p.xrToPass.push_back(static_cast<float>((now - g_tXrEnd) * k));
    if (g_tB0 && g_tB1 >= g_tB0 && g_tB0 >= g_tXrEnd) {
      ++p.withBegin;
      p.xrToBeginUs += (g_tB0 - g_tXrEnd) * k;
      p.beginUs += (g_tB1 - g_tB0) * k;
      p.beginToPassUs += (now - g_tB1) * k;
    }
  }
  if (!p.firstVt) p.firstVt = pass;
  g_tXrEnd = g_tB0 = g_tB1 = 0;
  g_firstOpen = pass;
  g_tFirst = now;
}

// ---------------------------------------------------------------------------
// The boundary callback (pass_timing.h's g_flushBoundary)
// ---------------------------------------------------------------------------

void OnBoundary(void* pass, bool begin, int depth) {
  const DWORD tid = GetCurrentThreadId();
  DWORD rt = g_rt.load(std::memory_order_relaxed);
  if (rt && tid != rt) return;
  const uint8_t kind = KindOf(pass);
  if (!rt) {
    if (kind != kCascade) return;
    g_rt.store(tid, std::memory_order_relaxed);
  }
  bool first = false;
  if (begin && depth == 0) {
    const uint64_t fc = g_frameCounter ? g_frameCounter->load(std::memory_order_relaxed) : 0;
    first = fc != g_lastFrame;
    g_lastFrame = fc;
    // Probe: the first top-level pass after xrEndFrame (GpuPassTiming's frame start), or after the counter
    // moved when xrEndFrame is not seen.
    if (g_probe.load(std::memory_order_relaxed) && (g_tXrEnd || (first && !g_xrNoted)))
      ProbeFirstPass(pass, Qpc(), first);
  } else if (!begin && depth == 0 && g_firstOpen == pass) {
    if (g_probe.load(std::memory_order_relaxed)) {
      g_pr.firstPassUs += (Qpc() - g_tFirst) * g_qpcToUs;
      ++g_pr.firstPassN;
    }
    g_firstOpen = nullptr;
  }
  g_plan.mask = g_mask.load(std::memory_order_relaxed) & ~(kRecExecute | kAfterXrBegin);
  const uint32_t why = begin ? g_plan.Begin(kind, depth, first) : g_plan.End(kind, depth);
  if (why) DoFlush(why);
}

// ---------------------------------------------------------------------------
// xrBeginFrame wrapper: Visualizer OpenXR vt[9] (two register arguments,
// this and the frame state; [V Visualizer.dll RVA 0x1244c0, offline]; R147:
// DCS 2.9.31's Visualizer 0x6ad87745 has it at 0x150c50, the same prologue).
// ---------------------------------------------------------------------------

constexpr int kXrBeginSlot = 9;
// Per analysed Visualizer build (PE timestamp): DCS 2.9.30, 2.9.31.
constexpr uint32_t kXrBeginStamps[] = {0x6ac12454u, 0x6ad87745u};
constexpr uint32_t kXrBeginRvas[] = {0x1244c0, 0x150c50};
const uint8_t kXrBeginPrologue[] = {0x48, 0x89, 0x5c, 0x24, 0x08, 0x57, 0x48, 0x83,
                                    0xec, 0x50, 0x48, 0x8b, 0xda, 0x48, 0x8b, 0xf9};
using XrFn = uint64_t(__fastcall*)(void*, void*, void*, void*);
XrFn g_xrOrig = nullptr;
void** g_xrSlot = nullptr;
std::atomic<uint64_t> g_xrBeginCalls{0};

uint64_t __fastcall XrBeginHook(void* a, void* b, void* c, void* d) {
  const bool rt = GetCurrentThreadId() == g_rt.load(std::memory_order_relaxed);
  const bool probe = rt && g_probe.load(std::memory_order_relaxed);
  const int64_t t0 = probe ? Qpc() : 0;
  const uint64_t r = g_xrOrig(a, b, c, d);
  if (rt) {
    Bump(g_xrBeginCalls);
    if (probe) {
      if (g_tXrEnd && t0 >= g_tXrEnd) {
        g_tB0 = t0;
        g_tB1 = Qpc();
      } else {
        ++g_pr.beginOutside;
      }
    }
    if (g_mask.load(std::memory_order_relaxed) & kAfterXrBegin) DoFlush(kAfterXrBegin);
  }
  return r;
}

bool CodeMatches(const uint8_t* p, const uint8_t* want, size_t n) {
  __try {
    return memcmp(p, want, n) == 0;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

bool InstallXr() {
  if (g_xrSlot) return true;
  HMODULE vis = GetModuleHandleW(L"Visualizer.dll");
  if (!vis) {
    Log("  pass flush: Visualizer.dll not loaded; xrBeginFrame not hooked");
    return false;
  }
  int n = 0;
  void** vt = sigscan::FindVtable(vis, ".?AVOpenXR@@", 0, false, &n);
  if (!vt) {
    Log("  pass flush: OpenXR vtable not found (%d candidates); xrBeginFrame not hooked", n);
    return false;
  }
  auto* fn = static_cast<uint8_t*>(SlotOriginal(&vt[kXrBeginSlot]));
  const uintptr_t rva = reinterpret_cast<uintptr_t>(fn) - reinterpret_cast<uintptr_t>(vis);
  const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(reinterpret_cast<const uint8_t*>(vis) +
                                                               reinterpret_cast<const IMAGE_DOS_HEADER*>(vis)->e_lfanew);
  bool known = false;
  for (size_t i = 0; i < sizeof(kXrBeginRvas) / sizeof(kXrBeginRvas[0]); ++i)
    known |= nt->FileHeader.TimeDateStamp == kXrBeginStamps[i] && rva == kXrBeginRvas[i];
  if (!known || !CodeMatches(fn, kXrBeginPrologue, sizeof(kXrBeginPrologue))) {
    Log("  pass flush: OpenXR vt[%d] is Visualizer+0x%llx, not this build's analysed RVA (0x%x, 0x%x); xrBeginFrame "
        "not hooked",
        kXrBeginSlot, static_cast<unsigned long long>(rva), kXrBeginRvas[0], kXrBeginRvas[1]);
    return false;
  }
  g_xrOrig = reinterpret_cast<XrFn>(fn);  // before the slot points at the hook
  if (!HookSlot(&vt[kXrBeginSlot], reinterpret_cast<void*>(&XrBeginHook), nullptr)) {
    Log("  pass flush: could not hook OpenXR vt[%d]", kXrBeginSlot);
    return false;
  }
  g_xrSlot = &vt[kXrBeginSlot];
  return true;
}

void UninstallXr() {
  if (!g_xrSlot) return;
  UnhookSlot(g_xrSlot, reinterpret_cast<void*>(g_xrOrig));
  g_xrSlot = nullptr;
}

// ---------------------------------------------------------------------------
// Control (config / suite thread)
// ---------------------------------------------------------------------------

// Takes DCS's immediate context (a reference) and the frame counter. True when ready.
bool Prepare(ID3D11Device* dev, std::atomic<uint64_t>* frameCounter) {
  if (g_ctx) return true;
  if (!dev || !ptiming::g_orig) return false;
  ID3D11DeviceContext* ctx = nullptr;
  dev->GetImmediateContext(&ctx);
  if (!ctx) return false;
  g_frameCounter = frameCounter;
  g_ctx = ctx;
  return true;
}

bool Ready() { return g_ctx != nullptr; }

std::atomic<bool> g_session{false};  // bench mode 30's phase: callback kept for the probe

// Installs the boundary callback while a mask or a bench session wants it and
// removes it otherwise (then the execute hook is as without this file).
void UpdateCallback() {
  ptiming::BoundaryFn me = &OnBoundary;
  if (g_mask.load() || g_session.load())
    ptiming::g_flushBoundary = me;
  else
    ptiming::g_flushBoundary.compare_exchange_strong(me, nullptr);
}

// The Flush points (0 = none).
void SetMask(uint32_t mask) {
  mask &= kMaskBits;
  if (!g_ctx) mask = 0;
  g_mask = mask;
  UpdateCallback();
}

// Bench session: the callback stays installed with mask 0 (for the probe and
// the frame detection), the probe stops recording at the end.
void SetSession(bool on) {
  g_session = on;
  if (!on) g_probe = false;
  UpdateCallback();
}

void Snapshot(uint64_t* flushes, uint64_t* ticks, uint64_t reasons[kReasons]) {
  *flushes = g_flushes.load();
  *ticks = g_flushTicks.load();
  for (int i = 0; i < kReasons; ++i) reasons[i] = g_byReason[i].load();
}

std::string MaskText(uint32_t m) {
  std::string s;
  for (int i = 0; i < kReasons; ++i)
    if (m & (1u << i)) {
      if (!s.empty()) s += ", ";
      s += kReasonNames[i];
    }
  return s.empty() ? "none" : s;
}

// Payload stop.
void Shutdown() {
  g_mask = 0;
  g_probe = false;
  g_session = false;
  ptiming::BoundaryFn me = &OnBoundary;
  ptiming::g_flushBoundary.compare_exchange_strong(me, nullptr);
  UninstallXr();
  Sleep(20);  // a render thread still inside a callback
  if (g_ctx) g_ctx->Release();
  g_ctx = nullptr;
}

// ---------------------------------------------------------------------------
// Bench mode 30 analysis (offline tested): each ON block against the mean of
// its OFF neighbours, as bench.h does, in absolute units.
// ---------------------------------------------------------------------------

struct Paired {
  double off = 0, on = 0;   // means over the valid blocks
  double delta = 0, ci = 0; // mean ON - neighbours, 95% interval
  int pairs = 0;
};

Paired PairedChange(const std::vector<double>& v, const std::vector<bool>& on, const std::vector<bool>& valid) {
  Paired p;
  int nOn = 0, nOff = 0;
  std::vector<double> d;
  for (size_t i = 0; i < v.size(); ++i) {
    if (!valid[i]) continue;
    if (on[i]) {
      p.on += v[i];
      ++nOn;
    } else {
      p.off += v[i];
      ++nOff;
    }
    if (!on[i]) continue;
    double base = 0;
    int nb = 0;
    if (i > 0 && !on[i - 1] && valid[i - 1]) base += v[i - 1], ++nb;
    if (i + 1 < v.size() && !on[i + 1] && valid[i + 1]) base += v[i + 1], ++nb;
    if (nb) d.push_back(v[i] - base / nb);
  }
  if (nOn) p.on /= nOn;
  if (nOff) p.off /= nOff;
  p.pairs = static_cast<int>(d.size());
  if (d.empty()) return p;
  for (double x : d) p.delta += x;
  p.delta /= d.size();
  if (d.size() > 1) {
    double sd = 0;
    for (double x : d) sd += (x - p.delta) * (x - p.delta);
    sd = std::sqrt(sd / (d.size() - 1));
    static const double kT95[] = {0, 12.71, 4.30, 3.18, 2.78, 2.57, 2.45, 2.36, 2.31, 2.26, 2.23, 2.20, 2.18};
    const size_t dof = d.size() - 1;
    const double t = dof < sizeof(kT95) / sizeof(kT95[0]) ? kT95[dof] : 2.10;
    p.ci = t * sd / std::sqrt(static_cast<double>(d.size()));
  }
  return p;
}

}  // namespace pflush
