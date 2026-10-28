// R52: per-frame timeline, pose-latency probe and event log (measure only).
// Keys ([Perf], all default 0, read live every second):
//   FrameTimeline  0 off | 1 a "timeline:" summary every TimelineSec | 2 also
//                  timeline_*.csv (one row per frame) and events_*.csv
//   TimelineSec    summary window in seconds (default 10, 2-600)
//   PoseProbe      0 off | 1 the head pose is located again for the same
//                  display time at xrBeginFrame and at xrEndFrame (needs
//                  FrameTimeline >= 1)
//   EventLog       0 off | 1 our own periodic worker tasks, ID3D11Device
//                  creates and page faults per frame (needs FrameTimeline >= 1)
// With every key at 0 nothing is installed and nothing runs per frame.
//
// What DCS does per frame (R15 [V], frame_start.h live 2026-10-10 [M]):
//   Main:   xrWaitFrame (slot 33), then one xrLocateViews (slot 36) for the
//           frame it will simulate;
//   render: xrBeginFrame (34), culling, passes, xrEndFrame (35).
// Main runs one frame ahead: its xrWaitFrame returns ~0.4 ms after the render
// thread's xrBeginFrame of the previous frame. So the pose DCS renders with is
// sampled about one frame before that frame's rendering starts.
//
// Hooks: the OpenXR loader dispatch table Visualizer's trampolines call
// through (frame_start.h: active instance static 0x2c8068, table at +0x48;
// trampolines verified byte for byte [V Visualizer.dll 0x6ac12454]:
//   xrWaitFrame 0x1b7840 (+0x108), xrBeginFrame 0x1b648f (+0x110),
//   xrEndFrame 0x1b6abf (+0x118), xrLocateViews 0x1b7409 (+0x120),
//   xrLocateSpace 0x1b7365 (+0x90), xrCreateReferenceSpace 0x1b6710 (+0x78);
//   R147: DCS 2.9.31's Visualizer 0x6ad87745, the same bytes at .text +0x2c790, kTramp).
// Every hook calls the original first or last and only reads its arguments;
// a failed or unexpected call is forwarded untouched. frame_start.h hooks
// slots 33/34 during its suite phase: the timeline then pauses and re-arms
// when the slot holds the original again.
//
// Per frame (paired by display time: xrWaitFrame's predictedDisplayTime ==
// the xrLocateViews displayTime == the xrEndFrame displayTime):
//   interval   xrEndFrame return to xrEndFrame return (what the runtime sees)
//   segments   pre-begin (previous xrEndFrame return -> xrBeginFrame call:
//              frame start, including any wait for Main), begin (inside
//              xrBeginFrame), body CPU / body blocked (xrBeginFrame return ->
//              xrEndFrame call, split by the render thread's cycle count),
//              submit (inside xrEndFrame: driver drain, compositor RPC)
//   budget     the steady cadence: TargetSlots(median interval, display
//              period) refreshes; a frame above it shows for one refresh
//              more than its neighbours (judder without Smart Smoothing)
//   class      the segment with the largest excess over its window median
//   margin     predictedDisplayTime - xrEndFrame return (needs the runtime's
//              XrTime to be QPC nanoseconds: checked per window, n/a else)
//   slots      refreshes between consecutive predicted display times
//   pose age   DCS's xrLocateViews return -> xrEndFrame return
//   depth      xrEndFrame calls between the pose sample and this frame's own
//              xrEndFrame (1 = one frame pipelined)
//   pose probe head pose (a VIEW reference space created once) in DCS's base
//              space at DCS's display time: right after DCS's xrLocateViews
//              (ref), at xrBeginFrame return and at xrEndFrame entry. The
//              rotation (deg) and translation (mm) between ref and the later
//              samples are what a late latch at that point would correct.
// Event log: our worker tasks (TaskScope, main.cpp), ID3D11Device creates
// (CreateBuffer, CreateTexture2D, CreateInputLayout, CreateVertex/Geometry/
// Pixel/ComputeShader; d3d11's own entries only) with the calling thread,
// the process page-fault count and the recorders' late passes per frame.
//
// Included once from main.cpp after thread_place.h.
#pragma once

#include <cstddef>

namespace ftl {

// ---------------------------------------------------------------------------
// OpenXR 1.0 structure layouts used here (openxr.h; checked below)
// ---------------------------------------------------------------------------
struct XrQuat {
  float x, y, z, w;
};
struct XrVec3 {
  float x, y, z;
};
struct XrPose {
  XrQuat o;
  XrVec3 p;
};
struct FrameState {
  int32_t type;
  void* next;
  int64_t predictedDisplayTime;
  int64_t predictedDisplayPeriod;
  uint32_t shouldRender;
};
struct ViewLocateInfo {
  int32_t type;
  const void* next;
  int32_t viewConfigurationType;
  int64_t displayTime;
  uint64_t space;
};
struct FrameEndInfo {
  int32_t type;
  const void* next;
  int64_t displayTime;
  int32_t environmentBlendMode;
  uint32_t layerCount;
  const void* const* layers;
};
struct SpaceLocation {
  int32_t type;
  void* next;
  uint64_t locationFlags;
  XrPose pose;
};
struct RefSpaceCreateInfo {
  int32_t type;
  const void* next;
  int32_t referenceSpaceType;
  XrPose poseInReferenceSpace;
};
static_assert(offsetof(FrameState, predictedDisplayTime) == 16 && offsetof(FrameState, shouldRender) == 32, "XrFrameState");
static_assert(offsetof(ViewLocateInfo, displayTime) == 24 && offsetof(ViewLocateInfo, space) == 32, "XrViewLocateInfo");
static_assert(offsetof(FrameEndInfo, displayTime) == 16 && offsetof(FrameEndInfo, layers) == 32, "XrFrameEndInfo");
static_assert(offsetof(SpaceLocation, pose) == 24 && sizeof(XrPose) == 28, "XrSpaceLocation");
static_assert(offsetof(RefSpaceCreateInfo, poseInReferenceSpace) == 20, "XrReferenceSpaceCreateInfo");
constexpr int32_t kTypeRefSpaceCreateInfo = 37, kTypeSpaceLocation = 42, kRefSpaceView = 1;
constexpr uint64_t kLocOrientationValid = 1, kLocPositionValid = 2;

// ---------------------------------------------------------------------------
// Pure parts (offline tested)
// ---------------------------------------------------------------------------

// QPC ticks -> nanoseconds without overflow.
inline int64_t QpcToNs(int64_t q, int64_t freq) {
  if (freq <= 0) return 0;
  return (q / freq) * 1000000000LL + (q % freq) * 1000000000LL / freq;
}

// The runtime's XrTime is QPC nanoseconds when a predicted display time lies
// between 50 ms in the past and 500 ms in the future of the QPC clock.
inline bool ClockPlausible(int64_t xrMinusQpcNs) { return xrMinusQpcNs > -50000000LL && xrMinusQpcNs < 500000000LL; }

// Refreshes between two predicted display times (0: unknown).
inline int SlotsBetween(int64_t prev, int64_t cur, int64_t period) {
  if (prev <= 0 || cur <= prev || period <= 0) return 0;
  return static_cast<int>((cur - prev + period / 2) / period);
}

// The steady cadence: refreshes per frame for the window's median interval,
// rounded (frames of 11.7 ms at 90 Hz drift against the vsync and still show
// mostly one refresh each; 21.2 ms show mostly two).
inline int TargetSlots(double medianMs, double periodMs) {
  if (medianMs <= 0 || periodMs <= 0) return 0;
  const int n = static_cast<int>(std::floor(medianMs / periodMs + 0.5));
  return n < 1 ? 1 : n;
}

inline double Pct(std::vector<double> v, double p) {
  if (v.empty()) return 0;
  const size_t i = std::min(v.size() - 1, static_cast<size_t>(p * (v.size() - 1) + 0.5));
  std::nth_element(v.begin(), v.begin() + i, v.end());
  return v[i];
}

// Angle between two orientations (deg) and the distance between two positions (mm).
inline double RotDeg(const XrQuat& a, const XrQuat& b) {
  double d = std::fabs(static_cast<double>(a.x) * b.x + static_cast<double>(a.y) * b.y +
                       static_cast<double>(a.z) * b.z + static_cast<double>(a.w) * b.w);
  const double na = std::sqrt(static_cast<double>(a.x) * a.x + a.y * a.y + a.z * a.z + a.w * a.w);
  const double nb = std::sqrt(static_cast<double>(b.x) * b.x + b.y * b.y + b.z * b.z + b.w * b.w);
  if (na <= 0 || nb <= 0) return 0;
  d /= na * nb;
  if (d > 1) d = 1;
  return 2.0 * std::acos(d) * 57.29577951308232;
}
inline double DistMm(const XrVec3& a, const XrVec3& b) {
  const double dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
  return std::sqrt(dx * dx + dy * dy + dz * dz) * 1000.0;
}

// One frame, completed at its xrEndFrame return. QPC values, 0 = not seen.
struct Rec {
  int64_t displayTime = 0;            // xrEndFrame's
  int64_t pdt = 0, period = 0;        // the xrWaitFrame that was begun for this frame
  int64_t w0 = 0, w1 = 0;             // that xrWaitFrame call
  int64_t l0 = 0, l1 = 0;             // DCS's first xrLocateViews for displayTime
  int locN = 0;                       // xrLocateViews calls for displayTime
  int64_t b0 = 0, b1 = 0;             // xrBeginFrame
  int64_t e0 = 0, e1 = 0;             // xrEndFrame
  int64_t prevE1 = 0;                 // the previous frame's xrEndFrame return
  uint64_t cycPrevE1 = 0, cycB1 = 0, cycE0 = 0, cycE1 = 0;  // render thread cycles
  uint64_t mainCyc = 0;               // Main's cycles from the previous xrWaitFrame call to this one
  uint64_t seqAtLoc = 0, seq = 0;     // xrEndFrame sequence at the pose sample / of this frame
  bool hasRef = false, hasBeg = false, hasEnd = false;
  XrPose ref{}, beg{}, end{};
  uint32_t creates = 0;               // ID3D11Device creates on the render thread in this frame
  int64_t createTicks = 0;
  uint32_t pageFaults = 0;
  uint32_t shLate = 0, gbLate = 0;    // recorder passes drawn stock because the job was late
};

enum Cls : int { kOnTime = 0, kSim, kRuntime, kRenderCpu, kRenderBlocked, kSubmit, kMixed, kNoData, kCls };
const char* const kClsName[kCls] = {"on time", "sim/frame start (before xrBeginFrame)", "runtime (xrBeginFrame)",
                                    "render CPU", "render blocked (not on CPU)",
                                    "submit (xrEndFrame: GPU drain/driver/compositor)", "mixed", "no data"};
const char* const kClsShort[kCls] = {"ok", "sim", "runtime", "rcpu", "rblocked", "submit", "mixed", "nodata"};

struct Segs {
  bool valid = false;
  double interval = 0, preBegin = 0, begin = 0, bodyCpu = 0, bodyIdle = 0, submit = 0;
};

inline Segs Split(const Rec& r, double qpcToMs, double cycToMs) {
  Segs s;
  if (!r.prevE1 || !r.b0 || !r.b1 || !r.e0 || !r.e1 || r.b0 < r.prevE1 || r.b1 < r.b0 || r.e0 < r.b1 || r.e1 < r.e0)
    return s;
  s.valid = true;
  s.interval = (r.e1 - r.prevE1) * qpcToMs;
  s.preBegin = (r.b0 - r.prevE1) * qpcToMs;
  s.begin = (r.b1 - r.b0) * qpcToMs;
  const double body = (r.e0 - r.b1) * qpcToMs;
  double cpu = r.cycE0 >= r.cycB1 && r.cycB1 ? (r.cycE0 - r.cycB1) * cycToMs : body;
  if (cpu > body) cpu = body;  // cycle and QPC clocks differ slightly
  s.bodyCpu = cpu;
  s.bodyIdle = body - cpu;
  s.submit = (r.e1 - r.e0) * qpcToMs;
  return s;
}

// A frame is over budget when its interval exceeds slots * period + tol. Its
// class is the segment whose excess over the window median is at least half
// of all positive excess; otherwise "mixed".
inline Cls Attribute(const Segs& s, const Segs& med, double budgetMs, double tolMs = 0.5) {
  if (!s.valid) return kNoData;
  if (budgetMs <= 0 || s.interval <= budgetMs + tolMs) return kOnTime;
  const double ex[5] = {s.preBegin - med.preBegin, s.begin - med.begin, s.bodyCpu - med.bodyCpu,
                        s.bodyIdle - med.bodyIdle, s.submit - med.submit};
  const Cls c[5] = {kSim, kRuntime, kRenderCpu, kRenderBlocked, kSubmit};
  double pos = 0, best = 0;
  int bi = -1;
  for (int i = 0; i < 5; ++i)
    if (ex[i] > 0) {
      pos += ex[i];
      if (ex[i] > best) {
        best = ex[i];
        bi = i;
      }
    }
  if (bi < 0) return kMixed;
  return best >= 0.5 * pos ? c[bi] : kMixed;
}

// Our worker's periodic tasks (event log).
enum TaskKind : int { kTPerf = 0, kTPlace, kTConfig, kTCensus, kTStats, kTXrState, kTFlags, kTTimeline, kTasks };
const char* const kTaskName[kTasks] = {"perf_mon", "placement", "config+apply", "census", "stats", "xr_state",
                                       "flags", "timeline"};
struct Task {
  int kind = 0;
  int64_t t0 = 0, t1 = 0;
};

// Per-frame derived row (CSV and summary).
struct Row {
  double t = 0;  // s since the first frame
  Segs s;
  double mainCpu = -1, waitMs = -1, locateMs = -1;
  double budget = 0;  // the window's cadence budget (ms)
  Cls cls = kNoData;
  bool over = false;
  int slots = 0;
  bool hasMargin = false;
  double margin = 0, horizon = 0;
  double age = -1;
  int depth = -1;
  double rotBeg = -1, trBeg = -1, rotEnd = -1, trEnd = -1;
  double taskMs = 0;
  uint32_t taskMask = 0;
  uint32_t creates = 0;
  double createMs = 0;
  uint32_t pageFaults = 0, shLate = 0, gbLate = 0;
};

struct Summary {
  int frames = 0, valid = 0;
  double periodMs = 0, budgetMs = 0;
  int targetSlots = 0;
  double ivP50 = 0, ivP95 = 0, ivP99 = 0, ivMax = 0;
  Segs med;
  int cls[kCls] = {};
  int over = 0;
  int slots[5] = {};  // 1, 2, 3, 4, 5+ refreshes between predicted display times
  int cadenceChanges = 0;
  double seconds = 0;
  bool clockOk = false;
  int marginN = 0, late = 0, nearLate = 0, nearEarly = 0;  // margin < 0, [-2, 0), [0, 2) ms
  double marginP5 = 0, marginP50 = 0, marginP95 = 0, horizonP50 = 0;
  double ageP50 = 0, ageP95 = 0;
  int depth[4] = {};  // 0, 1, 2, 3+
  int poseN = 0;
  double rotBegP50 = 0, rotBegP95 = 0, trBegP50 = 0, trBegP95 = 0, rotEndP50 = 0, rotEndP95 = 0, trEndP50 = 0,
         trEndP95 = 0;
  double mainCpuP50 = 0, waitP50 = 0, locateP50 = 0;
  int overWithTask = 0, overWithCreate = 0, overWithFaults = 0, overWithLate = 0;
  uint32_t taskOverMask = 0;
  int unpaired = 0, multiLocate = 0;
};

// Rows from records (ordered by xrEndFrame), with the window's tasks.
// qpcFreq for the XrTime check; faultGate: page faults per frame that count.
inline Summary Summarize(const std::vector<Rec>& recs, const std::vector<Task>& tasks, double qpcToMs, double cycToMs,
                         int64_t qpcFreq, std::vector<Row>* rowsOut = nullptr, int64_t t0Qpc = 0,
                         uint32_t faultGate = 2000) {
  Summary m;
  m.frames = static_cast<int>(recs.size());
  if (recs.empty()) return m;
  std::vector<Segs> segs(recs.size());
  std::vector<double> iv, pre, beg, cpu, idle, sub, period;
  for (size_t i = 0; i < recs.size(); ++i) {
    segs[i] = Split(recs[i], qpcToMs, cycToMs);
    if (!segs[i].valid) continue;
    ++m.valid;
    iv.push_back(segs[i].interval);
    pre.push_back(segs[i].preBegin);
    beg.push_back(segs[i].begin);
    cpu.push_back(segs[i].bodyCpu);
    idle.push_back(segs[i].bodyIdle);
    sub.push_back(segs[i].submit);
    if (recs[i].period > 0) period.push_back(recs[i].period / 1e6);
  }
  if (!iv.empty()) {
    m.ivP50 = Pct(iv, 0.5);
    m.ivP95 = Pct(iv, 0.95);
    m.ivP99 = Pct(iv, 0.99);
    m.ivMax = *std::max_element(iv.begin(), iv.end());
    m.med.valid = true;
    m.med.interval = m.ivP50;
    m.med.preBegin = Pct(pre, 0.5);
    m.med.begin = Pct(beg, 0.5);
    m.med.bodyCpu = Pct(cpu, 0.5);
    m.med.bodyIdle = Pct(idle, 0.5);
    m.med.submit = Pct(sub, 0.5);
  }
  m.periodMs = period.empty() ? 0 : Pct(period, 0.5);
  m.targetSlots = TargetSlots(m.ivP50, m.periodMs);
  m.budgetMs = m.targetSlots * m.periodMs;
  // XrTime clock: QPC nanoseconds when the predicted display times lie just ahead of the QPC clock.
  int plausible = 0, tested = 0;
  for (const Rec& r : recs)
    if (r.pdt > 0 && r.w1) {
      ++tested;
      plausible += ClockPlausible(r.pdt - QpcToNs(r.w1, qpcFreq)) ? 1 : 0;
    }
  m.clockOk = tested > 0 && plausible * 10 >= tested * 9;
  std::vector<double> margins, horizons, ages, rb, tb, re, te, mainCpu, waits, locates;
  int64_t prevPdt = 0;
  int prevSlots = 0;
  const int64_t first = t0Qpc ? t0Qpc : recs.front().e1;
  for (size_t i = 0; i < recs.size(); ++i) {
    const Rec& r = recs[i];
    Row w;
    w.t = (r.e1 - first) * qpcToMs / 1000.0;
    w.s = segs[i];
    if (r.mainCyc) w.mainCpu = r.mainCyc * cycToMs;
    if (r.w0 && r.w1 >= r.w0) w.waitMs = (r.w1 - r.w0) * qpcToMs;
    if (w.mainCpu >= 0) mainCpu.push_back(w.mainCpu);
    if (w.waitMs >= 0) waits.push_back(w.waitMs);
    if (r.l0 && r.l1 >= r.l0) {
      w.locateMs = (r.l1 - r.l0) * qpcToMs;  // DCS's xrLocateViews through the layers (pupil shift, Quad Views)
      locates.push_back(w.locateMs);
    }
    w.budget = m.budgetMs;
    w.cls = Attribute(segs[i], m.med, m.budgetMs);
    w.over = w.cls != kOnTime && w.cls != kNoData;
    if (w.cls >= 0 && w.cls < kCls) ++m.cls[w.cls];
    if (w.over) ++m.over;
    if (!r.pdt) ++m.unpaired;
    if (r.locN > 1) ++m.multiLocate;
    if (r.pdt) {
      w.slots = SlotsBetween(prevPdt, r.pdt, r.period);
      if (w.slots > 0) {
        ++m.slots[std::min(w.slots, 5) - 1];
        if (prevSlots && w.slots != prevSlots) ++m.cadenceChanges;
        prevSlots = w.slots;
      }
      prevPdt = r.pdt;
    }
    if (m.clockOk && r.pdt && r.e1) {
      w.hasMargin = true;
      w.margin = (r.pdt - QpcToNs(r.e1, qpcFreq)) / 1e6;
      margins.push_back(w.margin);
      if (w.margin < 0) ++m.late;
      if (w.margin < 0 && w.margin >= -2) ++m.nearLate;
      if (w.margin >= 0 && w.margin < 2) ++m.nearEarly;
      if (r.l1) {
        w.horizon = (r.pdt - QpcToNs(r.l1, qpcFreq)) / 1e6;
        horizons.push_back(w.horizon);
      }
    }
    if (r.l1 && r.e1 > r.l1) {
      w.age = (r.e1 - r.l1) * qpcToMs;
      ages.push_back(w.age);
      if (r.seq >= r.seqAtLoc) {
        w.depth = static_cast<int>(r.seq - r.seqAtLoc);
        ++m.depth[std::min(w.depth, 3)];
      }
    }
    if (r.hasRef && r.hasBeg) {
      w.rotBeg = RotDeg(r.ref.o, r.beg.o);
      w.trBeg = DistMm(r.ref.p, r.beg.p);
      rb.push_back(w.rotBeg);
      tb.push_back(w.trBeg);
    }
    if (r.hasRef && r.hasEnd) {
      w.rotEnd = RotDeg(r.ref.o, r.end.o);
      w.trEnd = DistMm(r.ref.p, r.end.p);
      re.push_back(w.rotEnd);
      te.push_back(w.trEnd);
    }
    // Our tasks overlapping this frame's interval.
    if (segs[i].valid)
      for (const Task& k : tasks) {
        const int64_t a = std::max(k.t0, r.prevE1), b = std::min(k.t1, r.e1);
        if (b > a) {
          w.taskMs += (b - a) * qpcToMs;
          w.taskMask |= 1u << k.kind;
        }
      }
    w.creates = r.creates;
    w.createMs = r.createTicks * qpcToMs;
    w.pageFaults = r.pageFaults;
    w.shLate = r.shLate;
    w.gbLate = r.gbLate;
    if (w.over) {
      if (w.taskMask) {
        ++m.overWithTask;
        m.taskOverMask |= w.taskMask;
      }
      if (w.creates) ++m.overWithCreate;
      if (w.pageFaults >= faultGate) ++m.overWithFaults;
      if (w.shLate + w.gbLate) ++m.overWithLate;
    }
    if (rowsOut) rowsOut->push_back(w);
  }
  if (recs.back().e1 > recs.front().e1) m.seconds = (recs.back().e1 - recs.front().e1) * qpcToMs / 1000.0;
  m.marginN = static_cast<int>(margins.size());
  if (!margins.empty()) {
    m.marginP5 = Pct(margins, 0.05);
    m.marginP50 = Pct(margins, 0.5);
    m.marginP95 = Pct(margins, 0.95);
  }
  if (!horizons.empty()) m.horizonP50 = Pct(horizons, 0.5);
  if (!ages.empty()) {
    m.ageP50 = Pct(ages, 0.5);
    m.ageP95 = Pct(ages, 0.95);
  }
  m.poseN = static_cast<int>(rb.size());
  if (!rb.empty()) {
    m.rotBegP50 = Pct(rb, 0.5);
    m.rotBegP95 = Pct(rb, 0.95);
    m.trBegP50 = Pct(tb, 0.5);
    m.trBegP95 = Pct(tb, 0.95);
  }
  if (!re.empty()) {
    m.rotEndP50 = Pct(re, 0.5);
    m.rotEndP95 = Pct(re, 0.95);
    m.trEndP50 = Pct(te, 0.5);
    m.trEndP95 = Pct(te, 0.95);
  }
  if (!mainCpu.empty()) m.mainCpuP50 = Pct(mainCpu, 0.5);
  if (!waits.empty()) m.waitP50 = Pct(waits, 0.5);
  if (!locates.empty()) m.locateP50 = Pct(locates, 0.5);
  return m;
}

inline std::string TaskList(uint32_t mask) {
  std::string s;
  for (int k = 0; k < kTasks; ++k)
    if (mask & (1u << k)) {
      if (!s.empty()) s += '+';
      s += kTaskName[k];
    }
  return s.empty() ? std::string("-") : s;
}

// The summary as log lines (without the "timeline:" prefix).
inline std::vector<std::string> FormatSummary(const Summary& m, bool pose, bool events) {
  std::vector<std::string> out;
  char b[600];
  if (!m.valid) {
    snprintf(b, sizeof(b), "%d frames, none complete (xrBeginFrame/xrEndFrame not seen in order)", m.frames);
    out.push_back(b);
    return out;
  }
  snprintf(b, sizeof(b),
           "%d frames in %.1f s | interval p50 %.2f p95 %.2f p99 %.2f max %.1f ms | display period %.2f ms, cadence "
           "%d refreshes (budget %.1f ms) | over budget %d (%.2f%%)",
           m.valid, m.seconds, m.ivP50, m.ivP95, m.ivP99, m.ivMax, m.periodMs, m.targetSlots, m.budgetMs, m.over,
           m.valid ? 100.0 * m.over / m.valid : 0.0);
  out.push_back(b);
  snprintf(b, sizeof(b),
           "medians: pre-begin %.2f, xrBeginFrame %.2f, render CPU %.2f, render blocked %.2f, xrEndFrame %.2f, Main "
           "CPU %.2f, xrWaitFrame %.2f, xrLocateViews %.3f ms",
           m.med.preBegin, m.med.begin, m.med.bodyCpu, m.med.bodyIdle, m.med.submit, m.mainCpuP50, m.waitP50,
           m.locateP50);
  out.push_back(b);
  std::string cls;
  for (int c = kSim; c <= kMixed; ++c) {
    snprintf(b, sizeof(b), "%s%s %d", cls.empty() ? "" : ", ", kClsShort[c], m.cls[c]);
    cls += b;
  }
  snprintf(b, sizeof(b), "over-budget frames by cause: %s", cls.c_str());
  out.push_back(b);
  const int ns = m.slots[0] + m.slots[1] + m.slots[2] + m.slots[3] + m.slots[4];
  snprintf(b, sizeof(b),
           "predicted display slots (refreshes between frames) 1/2/3/4/5+: %d/%d/%d/%d/%d, cadence changes %.1f/s",
           m.slots[0], m.slots[1], m.slots[2], m.slots[3], m.slots[4],
           m.seconds > 0 ? m.cadenceChanges / m.seconds : 0.0);
  out.push_back(b);
  (void)ns;
  if (m.clockOk)
    snprintf(b, sizeof(b),
             "deadline: predicted display - xrEndFrame return p5 %.2f p50 %.2f p95 %.2f ms; after the predicted time "
             "%d of %d, within 2 ms late %d, within 2 ms early %d | horizon (pose sample -> predicted display) p50 "
             "%.2f ms",
             m.marginP5, m.marginP50, m.marginP95, m.late, m.marginN, m.nearLate, m.nearEarly, m.horizonP50);
  else
    snprintf(b, sizeof(b), "deadline: n/a (the runtime's XrTime is not QPC nanoseconds)");
  out.push_back(b);
  snprintf(b, sizeof(b),
           "pose: DCS's xrLocateViews return -> xrEndFrame return p50 %.2f p95 %.2f ms; frames submitted in between "
           "0/1/2/3+: %d/%d/%d/%d | unpaired %d, several xrLocateViews for one display time %d",
           m.ageP50, m.ageP95, m.depth[0], m.depth[1], m.depth[2], m.depth[3], m.unpaired, m.multiLocate);
  out.push_back(b);
  if (pose) {
    snprintf(b, sizeof(b),
             "pose probe (%d frames): head pose for the same display time re-located at xrBeginFrame: rotation p50 "
             "%.3f p95 %.3f deg, translation p50 %.2f p95 %.2f mm; at xrEndFrame: rotation p50 %.3f p95 %.3f deg, "
             "translation p50 %.2f p95 %.2f mm",
             m.poseN, m.rotBegP50, m.rotBegP95, m.trBegP50, m.trBegP95, m.rotEndP50, m.rotEndP95, m.trEndP50,
             m.trEndP95);
    out.push_back(b);
  }
  if (events) {
    snprintf(b, sizeof(b),
             "over-budget frames overlapping: our worker tasks %d (%s), render-thread device creates %d, page faults "
             ">= gate %d, recorder late passes %d",
             m.overWithTask, TaskList(m.taskOverMask).c_str(), m.overWithCreate, m.overWithFaults, m.overWithLate);
    out.push_back(b);
  }
  return out;
}

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

using WaitFn = int32_t(__stdcall*)(uint64_t, const void*, FrameState*);
using BeginFn = int32_t(__stdcall*)(uint64_t, const void*);
using EndFn = int32_t(__stdcall*)(uint64_t, const FrameEndInfo*);
using LocateViewsFn = int32_t(__stdcall*)(uint64_t, const ViewLocateInfo*, void*, uint32_t, uint32_t*, void*);
using LocateSpaceFn = int32_t(__stdcall*)(uint64_t, uint64_t, int64_t, SpaceLocation*);
using CreateRefSpaceFn = int32_t(__stdcall*)(uint64_t, const RefSpaceCreateInfo*, uint64_t*);

constexpr int kSlotWait = 33, kSlotBegin = 34, kSlotEnd = 35, kSlotLocate = 36, kSlotLocateSpace = 18,
              kSlotCreateRefSpace = 15;
constexpr int kHooked = 4;
constexpr int kHookSlot[kHooked] = {kSlotWait, kSlotBegin, kSlotEnd, kSlotLocate};
const char* const kHookName[kHooked] = {"xrWaitFrame", "xrBeginFrame", "xrEndFrame", "xrLocateViews"};

std::atomic<bool> g_on{false};       // recording (hooks installed and FrameTimeline >= 1)
std::atomic<bool> g_poseOn{false};   // PoseProbe
std::atomic<bool> g_evOn{false};     // EventLog
void** g_disp = nullptr;             // the dispatch table the hooks are in
// R115 (L7): one read of g_disp for a hook-side user (Stop may clear it while a chained hook still runs): tested and
// indexed through the same copy, never re-read between the null check and the index.
inline void** DispOnce() { return *static_cast<void** const volatile*>(&g_disp); }
void* g_orig[kHooked] = {};
void* g_hook[kHooked] = {};
bool g_armed[kHooked] = {};
// R93: hooks that observe through the cockpit mask export's own hook instead of the slot (xrEndFrame 35 and
// xrLocateViews 36 while the export owns them; see ChainBehindMask).
bool g_chained[kHooked] = {};

SRWLOCK g_lock = SRWLOCK_INIT;
struct Wait {
  int64_t pdt = 0, period = 0, w0 = 0, w1 = 0;
  uint64_t mainCyc = 0;
};
std::deque<Wait> g_waits;  // waited, not begun (OpenXR: xrBeginFrame takes the oldest)
struct Pending {
  int64_t displayTime = 0;
  int64_t l0 = 0, l1 = 0;
  int n = 0;
  uint64_t seqAtLoc = 0;
  bool hasRef = false;
  XrPose ref{};
};
constexpr int kPending = 8;
Pending g_pending[kPending];
int g_pendingNext = 0;
std::vector<Rec> g_done;  // completed frames for the worker
constexpr size_t kMaxDone = 4096;
std::atomic<uint64_t> g_dropped{0};

// Main thread (xrWaitFrame) only.
uint64_t g_mainCycPrev = 0;
DWORD g_mainTid = 0;
// Render thread (xrBeginFrame / xrEndFrame) only.
Rec g_cur;
bool g_curOpen = false;
DWORD g_beginTid = 0;
int64_t g_prevE1 = 0;
uint64_t g_cycPrevE1 = 0;
uint32_t g_prevFaults = 0;
uint64_t g_prevLate[2] = {};
std::atomic<uint64_t> g_seq{0};          // xrEndFrame returns
std::atomic<DWORD> g_renderTid{0};       // the thread calling xrEndFrame
std::atomic<uint64_t> g_threadMismatch{0};

// Pose probe.
std::atomic<uint64_t> g_viewSpace{0};
std::atomic<uint64_t> g_baseSpace{0};
std::atomic<int> g_spaceState{0};  // 0 none, 1 created, -1 failed (not retried)
std::atomic<uint64_t> g_probeTicks{0}, g_probeCalls{0}, g_probeFails{0};

inline int64_t Now() {
  LARGE_INTEGER q;
  QueryPerformanceCounter(&q);
  return q.QuadPart;
}
inline uint64_t Cycles() {
  ULONG64 c = 0;
  QueryThreadCycleTime(GetCurrentThread(), &c);
  return c;
}

struct Lock {
  Lock() { AcquireSRWLockExclusive(&g_lock); }
  ~Lock() { ReleaseSRWLockExclusive(&g_lock); }
};

Pending* FindPending(int64_t t) {
  for (Pending& p : g_pending)
    if (p.displayTime == t && t) return &p;
  return nullptr;
}

// The head pose (VIEW space) in `base` at `time`; false when not located.
bool LocateHead(uint64_t base, int64_t time, XrPose* out) {
  const uint64_t view = g_viewSpace.load(std::memory_order_relaxed);
  void** const disp = DispOnce();  // R115 (L7)
  if (!disp || !view || !base || time <= 0) return false;
  auto fn = reinterpret_cast<LocateSpaceFn>(disp[kSlotLocateSpace]);
  if (!fn) return false;
  SpaceLocation loc{};
  loc.type = kTypeSpaceLocation;
  const int64_t t0 = Now();
  const int32_t r = fn(view, base, time, &loc);
  g_probeTicks.fetch_add(static_cast<uint64_t>(Now() - t0), std::memory_order_relaxed);
  g_probeCalls.fetch_add(1, std::memory_order_relaxed);
  if (r < 0 || (loc.locationFlags & (kLocOrientationValid | kLocPositionValid)) !=
                   (kLocOrientationValid | kLocPositionValid)) {
    g_probeFails.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  *out = loc.pose;
  return true;
}

// Main thread, inside DCS's xrLocateViews hook: the VIEW space, once.
void EnsureViewSpace(uint64_t session) {
  void** const disp = DispOnce();  // R115 (L7)
  if (g_spaceState.load(std::memory_order_relaxed) != 0 || !session || !disp) return;
  auto fn = reinterpret_cast<CreateRefSpaceFn>(disp[kSlotCreateRefSpace]);
  RefSpaceCreateInfo ci{};
  ci.type = kTypeRefSpaceCreateInfo;
  ci.referenceSpaceType = kRefSpaceView;
  ci.poseInReferenceSpace.o.w = 1.0f;
  uint64_t space = 0;
  const int32_t r = fn ? fn(session, &ci, &space) : -1;
  if (r >= 0 && space) {
    g_viewSpace = space;
    g_spaceState = 1;
  } else {
    g_spaceState = -1;
  }
}

// ---------------------------------------------------------------------------
// Event sources (render thread, EventLog)
// ---------------------------------------------------------------------------

std::atomic<uint32_t> g_rtCreates{0};
std::atomic<int64_t> g_rtCreateTicks{0};
struct Event {
  int slot = 0;
  int64_t t0 = 0, t1 = 0;
  int where = 0;  // 0 render thread, 1 Main (xrWaitFrame thread), 2 other
};
std::mutex g_evMutex;
std::vector<Event> g_events;  // creates >= kEventMinTicks, any thread
int64_t g_eventMinTicks = 0;

inline uint32_t PageFaults() {
  PROCESS_MEMORY_COUNTERS pmc{};
  pmc.cb = sizeof(pmc);
  return K32GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc)) ? pmc.PageFaultCount : 0;
}

// The recorders' late passes (cumulative; reset every 10 s by the census).
inline void LateTotals(uint64_t out[2]) {
  out[0] = out[1] = 0;
  const auto rl = std::memory_order_relaxed;
  if (shrec::g_state.load(rl) == 1)
    for (int c = 0; c < shrec::kSlots; ++c) out[0] += static_cast<uint64_t>(shrec::g_casc[c].st.passReason[shrec::kPLate].load(rl));
  if (gbrec::g_state.load(rl) == 1)
    for (int k = 0; k < gbrec::kSlots; ++k) out[1] += static_cast<uint64_t>(gbrec::g_slot[k].st.passReason[gbrec::kPLate].load(rl));
}
inline uint32_t Delta(uint64_t now, uint64_t prev) { return static_cast<uint32_t>(now >= prev ? now - prev : now); }

// ---------------------------------------------------------------------------
// OpenXR hooks
// ---------------------------------------------------------------------------

int32_t __stdcall H_Wait(uint64_t session, const void* info, FrameState* state) {
  if (!g_on.load(std::memory_order_relaxed)) return reinterpret_cast<WaitFn>(g_orig[0])(session, info, state);
  const uint64_t cyc = Cycles();
  const int64_t t0 = Now();
  const int32_t r = reinterpret_cast<WaitFn>(g_orig[0])(session, info, state);
  const int64_t t1 = Now();
  if (r >= 0 && state) {
    const DWORD tid = GetCurrentThreadId();
    Wait w;
    w.pdt = state->predictedDisplayTime;
    w.period = state->predictedDisplayPeriod;
    w.w0 = t0;
    w.w1 = t1;
    w.mainCyc = (tid == g_mainTid && g_mainCycPrev && cyc > g_mainCycPrev) ? cyc - g_mainCycPrev : 0;
    g_mainCycPrev = cyc;
    g_mainTid = tid;
    Lock l;
    g_waits.push_back(w);
    while (g_waits.size() > 8) g_waits.pop_front();
  }
  return r;
}

int32_t __stdcall H_Locate(uint64_t session, const ViewLocateInfo* info, void* state, uint32_t cap, uint32_t* count,
                           void* views) {
  if (!g_on.load(std::memory_order_relaxed))
    return reinterpret_cast<LocateViewsFn>(g_orig[3])(session, info, state, cap, count, views);
  const int64_t t0 = Now();
  const int32_t r = reinterpret_cast<LocateViewsFn>(g_orig[3])(session, info, state, cap, count, views);
  const int64_t t1 = Now();
  if (r < 0 || !info || cap == 0) return r;  // a size query or a failure: not a pose sample
  XrPose ref{};
  bool hasRef = false;
  if (g_poseOn.load(std::memory_order_relaxed)) {
    EnsureViewSpace(session);
    g_baseSpace.store(info->space, std::memory_order_relaxed);
    hasRef = LocateHead(info->space, info->displayTime, &ref);
  }
  Lock l;
  Pending* p = FindPending(info->displayTime);
  if (p) {
    ++p->n;
  } else {
    p = &g_pending[g_pendingNext];
    g_pendingNext = (g_pendingNext + 1) % kPending;
    *p = Pending{};
    p->displayTime = info->displayTime;
    p->l0 = t0;
    p->l1 = t1;
    p->n = 1;
    p->seqAtLoc = g_seq.load(std::memory_order_relaxed);
    p->hasRef = hasRef;
    p->ref = ref;
  }
  return r;
}

int32_t __stdcall H_Begin(uint64_t session, const void* info) {
  if (!g_on.load(std::memory_order_relaxed)) return reinterpret_cast<BeginFn>(g_orig[1])(session, info);
  const int64_t t0 = Now();
  const int32_t r = reinterpret_cast<BeginFn>(g_orig[1])(session, info);
  const int64_t t1 = Now();
  const uint64_t cyc = Cycles();
  if (r < 0) return r;
  Rec cur;
  {
    Lock l;
    if (!g_waits.empty()) {
      const Wait& w = g_waits.front();
      cur.pdt = w.pdt;
      cur.period = w.period;
      cur.w0 = w.w0;
      cur.w1 = w.w1;
      cur.mainCyc = w.mainCyc;
      g_waits.pop_front();
    }
  }
  cur.b0 = t0;
  cur.b1 = t1;
  cur.cycB1 = cyc;
  g_cur = cur;
  g_curOpen = true;
  g_beginTid = GetCurrentThreadId();
  if (g_poseOn.load(std::memory_order_relaxed) && cur.pdt)
    g_cur.hasBeg = LocateHead(g_baseSpace.load(std::memory_order_relaxed), cur.pdt, &g_cur.beg);
  return r;
}

int32_t __stdcall H_End(uint64_t session, const FrameEndInfo* info) {
  if (!g_on.load(std::memory_order_relaxed)) return reinterpret_cast<EndFn>(g_orig[2])(session, info);
  qvalog::NoteFrameThread();  // R93: the render thread (main.cpp's async log: its lines never wait)
  const DWORD tid = GetCurrentThreadId();
  const bool same = g_curOpen && tid == g_beginTid;
  if (g_curOpen && !same) g_threadMismatch.fetch_add(1, std::memory_order_relaxed);
  Rec rec = same ? g_cur : Rec{};
  if (g_poseOn.load(std::memory_order_relaxed) && info)
    rec.hasEnd = LocateHead(g_baseSpace.load(std::memory_order_relaxed), info->displayTime, &rec.end);
  rec.cycE0 = Cycles();
  rec.e0 = Now();
  const int32_t r = reinterpret_cast<EndFn>(g_orig[2])(session, info);
  rec.e1 = Now();
  rec.cycE1 = Cycles();
  g_curOpen = false;
  g_renderTid.store(tid, std::memory_order_relaxed);
  if (r < 0 || !info) {
    g_prevE1 = 0;  // the next frame's interval is not measured
    return r;
  }
  rec.displayTime = info->displayTime;
  rec.prevE1 = g_prevE1;
  rec.cycPrevE1 = g_cycPrevE1;
  if (rec.pdt && rec.pdt != rec.displayTime) {  // DCS submitted another time than the one waited: not paired
    rec.pdt = rec.period = 0;
    rec.w0 = rec.w1 = 0;
  }
  if (g_evOn.load(std::memory_order_relaxed)) {
    rec.creates = g_rtCreates.exchange(0, std::memory_order_relaxed);
    rec.createTicks = g_rtCreateTicks.exchange(0, std::memory_order_relaxed);
    const uint32_t pf = PageFaults();
    rec.pageFaults = g_prevFaults ? Delta(pf, g_prevFaults) : 0;
    g_prevFaults = pf;
    uint64_t late[2];
    LateTotals(late);
    rec.shLate = Delta(late[0], g_prevLate[0]);
    rec.gbLate = Delta(late[1], g_prevLate[1]);
    g_prevLate[0] = late[0];
    g_prevLate[1] = late[1];
  }
  rec.seq = g_seq.load(std::memory_order_relaxed);
  {
    Lock l;
    if (Pending* p = FindPending(rec.displayTime)) {
      rec.l0 = p->l0;
      rec.l1 = p->l1;
      rec.locN = p->n;
      rec.seqAtLoc = p->seqAtLoc;
      rec.hasRef = p->hasRef;
      rec.ref = p->ref;
      *p = Pending{};
    }
    if (g_done.size() < kMaxDone)
      g_done.push_back(rec);
    else
      g_dropped.fetch_add(1, std::memory_order_relaxed);
  }
  g_seq.fetch_add(1, std::memory_order_relaxed);
  g_prevE1 = rec.e1;
  g_cycPrevE1 = rec.cycE1;
  return r;
}

void* const kHookFn[kHooked] = {reinterpret_cast<void*>(&H_Wait), reinterpret_cast<void*>(&H_Begin),
                                reinterpret_cast<void*>(&H_End), reinterpret_cast<void*>(&H_Locate)};

// R93 (docs/research/R92_live_hitch_fg.md D2): one owner per OpenXR dispatch slot. The loader registry keeps one true
// original per slot, so two hooks of one slot never chain (R76: whichever hooked last bypassed the other), and the
// timeline used to wait while the cockpit mask export owned xrEndFrame (35) / xrLocateViews (36): with the FG profile
// it never recorded. Now the export stays the slot's only owner and the timeline observes behind it: the export's hook
// forwards to ours (cmask::g_xeNext, cmask::ctime::g_next) and ours calls the true original (the export's own copy).
// The export's work in its hook (the mask commit) runs before ours, so it falls in the timeline's render CPU, not in
// its xrEndFrame span, as when the export ran without the timeline.
enum ChainState { kChainNo = 0, kChainYes, kChainFail };
inline void* MaskHookFor(int i) {
  if (kHookSlot[i] == kSlotEnd) return reinterpret_cast<void*>(&fixdepth::cmask::H_EndFrame);
  if (kHookSlot[i] == kSlotLocate) return reinterpret_cast<void*>(&fixdepth::cmask::ctime::H_LocateViews);
  return nullptr;
}
inline void** MaskSlotFor(int i) {
  if (kHookSlot[i] == kSlotEnd) return fixdepth::cmask::g_xeSlot;
  if (kHookSlot[i] == kSlotLocate) return fixdepth::cmask::ctime::g_slot;
  return nullptr;
}
// The chained observer still in place for hook i (the export's hook in the slot, forwarding to ours).
inline bool ChainIntact(int i) {
  void** slot = &g_disp[kHookSlot[i]];
  if (MaskSlotFor(i) != slot || *slot != MaskHookFor(i)) return false;
  if (kHookSlot[i] == kSlotEnd)
    return reinterpret_cast<void*>(fixdepth::cmask::g_xeNext.load()) == kHookFn[i];
  return reinterpret_cast<void*>(fixdepth::cmask::ctime::g_next.load()) == kHookFn[i];
}
inline void Unchain(int i) {
  if (kHookSlot[i] == kSlotEnd) {
    fixdepth::cmask::XrEndFn ours = reinterpret_cast<fixdepth::cmask::XrEndFn>(kHookFn[i]);
    fixdepth::cmask::g_xeNext.compare_exchange_strong(ours, nullptr);
  } else if (kHookSlot[i] == kSlotLocate) {
    fixdepth::cmask::ctime::LocateFn ours = reinterpret_cast<fixdepth::cmask::ctime::LocateFn>(kHookFn[i]);
    fixdepth::cmask::ctime::g_next.compare_exchange_strong(ours, nullptr);
  }
}
// R115 (T1): an original that is one of our own hooks (or the export's hook itself): calling it from ours would recurse
// without end (outside the loader SlotOriginal is the slot's current value, which can be ours).
inline bool IsOwnHook(const void* fn, int i) {
  for (int k = 0; k < kHooked; ++k)
    if (fn == kHookFn[k]) return true;
  return fn == MaskHookFor(i);
}
// Hook i behind the export's hook when the export owns its slot.
ChainState ChainBehindMask(int i, void** slot) {
  if (!MaskHookFor(i) || MaskSlotFor(i) != slot || *slot != MaskHookFor(i)) return kChainNo;
  if (kHookSlot[i] == kSlotEnd) {
    if (!fixdepth::cmask::g_xeOrig || IsOwnHook(reinterpret_cast<void*>(fixdepth::cmask::g_xeOrig), i))
      return kChainFail;
    g_orig[i] = reinterpret_cast<void*>(fixdepth::cmask::g_xeOrig);  // the true original (the export's copy)
    fixdepth::cmask::g_xeNext.store(reinterpret_cast<fixdepth::cmask::XrEndFn>(kHookFn[i]), std::memory_order_release);
  } else {
    if (!fixdepth::cmask::ctime::g_orig || IsOwnHook(reinterpret_cast<void*>(fixdepth::cmask::ctime::g_orig), i))
      return kChainFail;
    g_orig[i] = reinterpret_cast<void*>(fixdepth::cmask::ctime::g_orig);
    fixdepth::cmask::ctime::g_next.store(reinterpret_cast<fixdepth::cmask::ctime::LocateFn>(kHookFn[i]),
                                         std::memory_order_release);
  }
  return kChainYes;
}

// Hooks the four slots of `disp` (a loader dispatch table, or a test's); behind the cockpit mask export's hook where it
// owns the slot (R93).
bool InstallOn(void** disp) {
  if (!disp) return false;
  g_disp = disp;
  for (int i = 0; i < kHooked; ++i) {
    void** slot = &disp[kHookSlot[i]];
    if (*slot == kHookFn[i]) {
      g_armed[i] = true;
      continue;
    }
    const ChainState cs = ChainBehindMask(i, slot);
    if (cs == kChainYes) {
      g_chained[i] = true;
      continue;
    }
    if (cs == kChainNo) g_orig[i] = SlotOriginal(slot);
    if (cs == kChainFail || !g_orig[i] || !HookSlot(slot, kHookFn[i], nullptr)) {
      for (int k = 0; k < i; ++k) {
        if (g_armed[k]) {
          UnhookSlot(&disp[kHookSlot[k]], g_orig[k]);
          g_armed[k] = false;
        }
        if (g_chained[k]) {
          Unchain(k);
          g_chained[k] = false;
        }
      }
      return false;
    }
    g_armed[i] = true;
  }
  return true;
}

void UninstallHooks() {
  g_on = false;
  if (!g_disp) return;
  for (int i = 0; i < kHooked; ++i) {
    if (g_armed[i]) {
      void** slot = &g_disp[kHookSlot[i]];
      if (*slot == kHookFn[i]) UnhookSlot(slot, g_orig[i]);
      g_armed[i] = false;
    }
    if (g_chained[i]) {  // R93: the export keeps its slot; only our link behind it goes
      Unchain(i);
      g_chained[i] = false;
    }
  }
}

// Every armed slot still holds our hook (another feature may have replaced it); every chained hook is still behind the
// export's hook (R93).
bool HooksIntact() {
  if (!g_disp) return false;
  for (int i = 0; i < kHooked; ++i) {
    if (g_chained[i]) {
      if (!ChainIntact(i)) return false;
      continue;
    }
    if (!g_armed[i] || g_disp[kHookSlot[i]] != kHookFn[i]) return false;
  }
  return true;
}
// R93: what broke HooksIntact, for the log line: 1 the export took a slot we hooked (re-armed behind it), 2 the export
// left a slot we observed through (re-armed on the slot), 0 another feature.
int BrokenBy() {
  if (!g_disp) return 0;
  for (int i = 0; i < kHooked; ++i) {
    if (g_chained[i] && !ChainIntact(i)) return 2;
    if (g_armed[i] && g_disp[kHookSlot[i]] != kHookFn[i] && MaskHookFor(i) &&
        g_disp[kHookSlot[i]] == MaskHookFor(i))
      return 1;
  }
  return 0;
}

// The analysed Visualizer.dll's dispatch table, its trampolines checked (per build: fstart::kVisBuilds; R147 2.9.31
// re-derived offline, the same bytes at .text +0x2c790).
struct Tramp {
  uint32_t rva[fstart::kVisBuildCount];
  uint8_t code[11];
};
const Tramp kTramp[] = {
    {{0x1b7840, 0x1e3fd0}, {0x4c, 0x8b, 0x48, 0x48, 0x49, 0x8b, 0x81, 0x08, 0x01, 0x00, 0x00}},  // xrWaitFrame
    {{0x1b648f, 0x1e2c1f}, {0x4c, 0x8b, 0x40, 0x48, 0x49, 0x8b, 0x80, 0x10, 0x01, 0x00, 0x00}},  // xrBeginFrame
    {{0x1b6abf, 0x1e324f}, {0x4c, 0x8b, 0x40, 0x48, 0x49, 0x8b, 0x80, 0x18, 0x01, 0x00, 0x00}},  // xrEndFrame
    {{0x1b7409, 0x1e3b99}, {0x4c, 0x8b, 0x50, 0x48, 0x49, 0x8b, 0x82, 0x20, 0x01, 0x00, 0x00}},  // xrLocateViews
    {{0x1b7365, 0x1e3af5}, {0x4c, 0x8b, 0x50, 0x48, 0x49, 0x8b, 0x82, 0x90, 0x00, 0x00, 0x00}},  // xrLocateSpace
    {{0x1b6710, 0x1e2ea0}, {0x4c, 0x8b, 0x48, 0x48, 0x49, 0x8b, 0x41, 0x78, 0x4c, 0x8b, 0xc6}},  // xrCreateReferenceSpace
};

void** FindDispatch(std::string* why) {
  auto* vis = reinterpret_cast<uint8_t*>(GetModuleHandleW(L"Visualizer.dll"));
  if (!vis) {
    *why = "Visualizer.dll not loaded";
    return nullptr;
  }
  const int build = fstart::VisBuildOf(vis);
  if (build < 0) {
    *why = "Visualizer.dll is not an analysed build";
    return nullptr;
  }
  for (const Tramp& t : kTramp)
    if (!gpt::CodeMatches(vis + t.rva[build], t.code, sizeof(t.code))) {
      char b[96];
      snprintf(b, sizeof(b), "OpenXR trampoline at Visualizer+0x%x differs", t.rva[build]);
      *why = b;
      return nullptr;
    }
  void** disp = fstart::DispatchGuarded(vis, build);
  if (!disp || !gpt::Readable(disp) || !gpt::Readable(&disp[40])) {
    *why = "OpenXR dispatch table not found (no VR session yet)";
    return nullptr;
  }
  char m[200];
  for (int s : {kSlotWait, kSlotBegin, kSlotEnd, kSlotLocate, kSlotLocateSpace, kSlotCreateRefSpace})
    if (!gpt::ModRva(SlotOriginal(&disp[s]), m, sizeof(m))) {
      *why = "a dispatch entry is not in a loaded image";
      return nullptr;
    }
  return disp;
}

// ---------------------------------------------------------------------------
// ID3D11Device create hooks (EventLog)
// ---------------------------------------------------------------------------

constexpr int kCreateSlots = 7;
constexpr int kCreateSlot[kCreateSlots] = {3, 5, 11, 12, 13, 15, 18};
const char* const kCreateName[kCreateSlots] = {"CreateBuffer", "CreateTexture2D", "CreateInputLayout",
                                               "CreateVertexShader", "CreateGeometryShader", "CreatePixelShader",
                                               "CreateComputeShader"};
void* g_cOrig[kCreateSlots] = {};
void** g_cSlot[kCreateSlots] = {};

inline const char* CreateName(int slot) {
  for (int i = 0; i < kCreateSlots; ++i)
    if (kCreateSlot[i] == slot) return kCreateName[i];
  return "?";
}

inline void NoteCreate(int slot, int64_t t0, int64_t t1) {
  const DWORD tid = GetCurrentThreadId();
  const bool rt = tid == g_renderTid.load(std::memory_order_relaxed);
  if (rt) {
    g_rtCreates.fetch_add(1, std::memory_order_relaxed);
    g_rtCreateTicks.fetch_add(t1 - t0, std::memory_order_relaxed);
  }
  if (t1 - t0 >= g_eventMinTicks || rt) {
    Event e;
    e.slot = slot;
    e.t0 = t0;
    e.t1 = t1;
    e.where = rt ? 0 : tid == g_mainTid ? 1 : 2;
    std::lock_guard<std::mutex> lock(g_evMutex);
    if (g_events.size() < 20000) g_events.push_back(e);
  }
}

template <int I, class... A>
HRESULT STDMETHODCALLTYPE HCreate(ID3D11Device* d, A... a) {
  using Fn = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, A...);
  const int64_t t0 = Now();
  const HRESULT r = reinterpret_cast<Fn>(g_cOrig[I])(d, a...);
  if (g_evOn.load(std::memory_order_relaxed)) NoteCreate(kCreateSlot[I], t0, Now());
  return r;
}

void* const kCreateHook[kCreateSlots] = {
    reinterpret_cast<void*>(&HCreate<0, const D3D11_BUFFER_DESC*, const D3D11_SUBRESOURCE_DATA*, ID3D11Buffer**>),
    reinterpret_cast<void*>(&HCreate<1, const D3D11_TEXTURE2D_DESC*, const D3D11_SUBRESOURCE_DATA*, ID3D11Texture2D**>),
    reinterpret_cast<void*>(&HCreate<2, const D3D11_INPUT_ELEMENT_DESC*, UINT, const void*, SIZE_T, ID3D11InputLayout**>),
    reinterpret_cast<void*>(&HCreate<3, const void*, SIZE_T, ID3D11ClassLinkage*, ID3D11VertexShader**>),
    reinterpret_cast<void*>(&HCreate<4, const void*, SIZE_T, ID3D11ClassLinkage*, ID3D11GeometryShader**>),
    reinterpret_cast<void*>(&HCreate<5, const void*, SIZE_T, ID3D11ClassLinkage*, ID3D11PixelShader**>),
    reinterpret_cast<void*>(&HCreate<6, const void*, SIZE_T, ID3D11ClassLinkage*, ID3D11ComputeShader**>)};

// d3d11's own entries only (a suite's counting hook or another layer's is left alone).
int InstallCreates(ID3D11Device* dev, std::string* skipped) {
  if (!dev) return 0;
  void** vt = *reinterpret_cast<void***>(dev);
  const HMODULE d3d = GetModuleHandleW(L"d3d11.dll");
  int n = 0;
  for (int i = 0; i < kCreateSlots; ++i) {
    if (g_cSlot[i]) {
      ++n;
      continue;
    }
    void** slot = &vt[kCreateSlot[i]];
    void* cur = SlotOriginal(slot);
    HMODULE owner = nullptr;
    if (*slot != cur || !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                            static_cast<LPCWSTR>(cur), &owner) ||
        owner != d3d) {
      if (skipped) *skipped += std::string(skipped->empty() ? "" : ", ") + kCreateName[i];
      continue;
    }
    g_cOrig[i] = cur;
    if (HookSlot(slot, kCreateHook[i], nullptr)) {
      g_cSlot[i] = slot;
      ++n;
    }
  }
  return n;
}

void UninstallCreates() {
  for (int i = 0; i < kCreateSlots; ++i)
    if (g_cSlot[i]) {
      if (*g_cSlot[i] == kCreateHook[i]) UnhookSlot(g_cSlot[i], g_cOrig[i]);
      g_cSlot[i] = nullptr;
    }
}

// ---------------------------------------------------------------------------
// Worker tasks (EventLog)
// ---------------------------------------------------------------------------

std::mutex g_taskMutex;
std::vector<Task> g_tasks;  // tasks since the last drain (worker thread)
int64_t g_taskMinTicks = 0;  // shorter tasks are not recorded (set at install: 20 us)

struct TaskScope {
  int kind;
  int64_t t0;
  explicit TaskScope(int k) : kind(k), t0(g_evOn.load(std::memory_order_relaxed) ? Now() : 0) {}
  ~TaskScope() {
    if (!t0) return;
    Task t;
    t.kind = kind;
    t.t0 = t0;
    t.t1 = Now();
    if (t.t1 - t.t0 < g_taskMinTicks) return;
    std::lock_guard<std::mutex> lock(g_taskMutex);
    if (g_tasks.size() < 50000) g_tasks.push_back(t);
  }
};

// ---------------------------------------------------------------------------
// Worker: install, drain, CSV, summaries
// ---------------------------------------------------------------------------

struct Cfg {
  int timeline = 0;  // 0 off, 1 summaries, 2 + CSV
  int windowSec = 10;
  bool pose = false;
  bool events = false;
};

std::vector<Rec> g_window;
std::vector<Task> g_windowTasks;
FILE* g_csv = nullptr;
FILE* g_evCsv = nullptr;
int64_t g_csvT0 = 0;
uint64_t g_lastSummaryMs = 0, g_lastTryMs = 0, g_lastDrainMs = 0;
std::string g_lastWhy;
bool g_pausedLogged = false;
uint64_t g_probeTicksPrev = 0, g_probeCallsPrev = 0;
int g_createHooks = 0;
Cfg g_cfgNow;

std::wstring CsvName(const wchar_t* prefix) {
  SYSTEMTIME st;
  GetLocalTime(&st);
  wchar_t name[96];
  swprintf_s(name, L"%ls_%04d%02d%02d_%02d%02d%02d.csv", prefix, st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute,
             st.wSecond);
  return name;
}

void CloseCsv() {
  if (g_csv) fclose(g_csv);
  if (g_evCsv) fclose(g_evCsv);
  g_csv = g_evCsv = nullptr;
  g_csvT0 = 0;
}

void OpenCsv(const std::wstring& dir) {
  if (g_csv) return;
  // R49: shared for readers (_wfopen_s denies them: the R52 live script could not copy the CSVs while DCS ran).
  g_csv = _wfsopen((dir + CsvName(L"timeline")).c_str(), L"w", _SH_DENYWR);
  if (g_csv)
    fprintf(g_csv,
            "t_s,interval_ms,pre_begin_ms,begin_ms,render_cpu_ms,render_blocked_ms,submit_ms,main_cpu_ms,wait_ms,"
            "locate_ms,budget_ms,class,over,slots,margin_ms,horizon_ms,pose_age_ms,depth,rot_begin_deg,tr_begin_mm,rot_end_deg,tr_end_mm,"
            "creates,create_ms,page_faults,sh_late,gb_late,task_ms,tasks\n");
  g_evCsv = _wfsopen((dir + CsvName(L"events")).c_str(), L"w", _SH_DENYWR);
  if (g_evCsv) fprintf(g_evCsv, "t_s,kind,name,thread,ms\n");
}

void WriteRows(const std::vector<Row>& rows) {
  if (!g_csv) return;
  for (const Row& w : rows)
    fprintf(g_csv,
            "%.4f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.4f,%.3f,%s,%d,%d,%.3f,%.3f,%.3f,%d,%.4f,%.3f,%.4f,%.3f,%u,%.3f,%u,%u,"
            "%u,%.3f,%s\n",
            w.t, w.s.valid ? w.s.interval : NAN, w.s.preBegin, w.s.begin, w.s.bodyCpu, w.s.bodyIdle, w.s.submit,
            w.mainCpu, w.waitMs, w.locateMs, w.budget, kClsShort[w.cls], w.over ? 1 : 0, w.slots, w.hasMargin ? w.margin : NAN, w.hasMargin ? w.horizon : NAN,
            w.age, w.depth, w.rotBeg, w.trBeg, w.rotEnd, w.trEnd, w.creates, w.createMs, w.pageFaults, w.shLate,
            w.gbLate, w.taskMs, TaskList(w.taskMask).c_str());
  fflush(g_csv);
}

void WriteEvents(const std::vector<Event>& ev, const std::vector<Task>& tasks, double qpcToMs) {
  if (!g_evCsv || !g_csvT0) return;
  static const char* const kWhere[3] = {"render", "main", "other"};
  for (const Event& e : ev)
    fprintf(g_evCsv, "%.4f,create,%s,%s,%.3f\n", (e.t0 - g_csvT0) * qpcToMs / 1000.0, CreateName(e.slot),
            kWhere[e.where < 0 || e.where > 2 ? 2 : e.where], (e.t1 - e.t0) * qpcToMs);
  for (const Task& t : tasks)
    fprintf(g_evCsv, "%.4f,task,%s,worker,%.3f\n", (t.t0 - g_csvT0) * qpcToMs / 1000.0, kTaskName[t.kind],
            (t.t1 - t.t0) * qpcToMs);
  fflush(g_evCsv);
}

void Stop(const char* why) {
  const bool was = g_on.load() || g_disp;
  UninstallHooks();
  UninstallCreates();
  g_poseOn = false;
  g_evOn = false;
  CloseCsv();
  {
    Lock l;
    g_waits.clear();
    for (Pending& p : g_pending) p = Pending{};
    g_done.clear();
  }
  g_window.clear();
  g_windowTasks.clear();
  g_curOpen = false;
  g_prevE1 = 0;
  g_disp = nullptr;
  g_createHooks = 0;
  if (was && why) Log("timeline: off (%s)", why);
}

// R76: the cockpit mask export's xrEndFrame / xrLocateViews hooks own slots 35 / 36 (its frame commit and content
// time). The loader registry keeps one true original per slot, so two hooks there never chain: whichever hooked last
// bypassed the other (frame generation lost its mask, depth and MVs). R76 made the timeline wait while they were in;
// R93: it observes behind them instead (ChainBehindMask), so this only says which way it is armed.
bool XrSlotsOwnedElsewhere() {
  return fixdepth::cmask::g_xeSlot != nullptr || fixdepth::cmask::ctime::g_slot != nullptr;
}

// Worker thread, every loop pass. dir: where the CSVs go.
void Tick(const Cfg& c, const std::wstring& dir, double qpcToUs, double tscHz, int64_t qpcFreq) {
  g_cfgNow = c;
  if (c.timeline <= 0) {
    if (g_on.load() || g_disp) Stop("[Perf] FrameTimeline=0");
    return;
  }
  const uint64_t nowMs = GetTickCount64();
  if (!g_on.load()) {
    if (nowMs - g_lastTryMs < 2000) return;
    g_lastTryMs = nowMs;
    if (fstart::g_sessionOn.load()) {  // frame_start.h owns slots 33/34 during its phase
      if (!g_pausedLogged) Log("timeline: waiting (FrameStartGap suite phase owns xrWaitFrame/xrBeginFrame)");
      g_pausedLogged = true;
      return;
    }
    std::string why;
    void** disp = FindDispatch(&why);
    if (!disp) {
      if (why != g_lastWhy) Log("timeline: not installed: %s", why.c_str());
      g_lastWhy = why;
      return;
    }
    if (!InstallOn(disp)) {
      Log("timeline: could not hook the OpenXR dispatch table");
      return;
    }
    g_lastWhy.clear();
    g_pausedLogged = false;
    g_eventMinTicks = static_cast<int64_t>(qpcFreq * 0.0003);  // creates >= 0.3 ms on any thread
    g_taskMinTicks = static_cast<int64_t>(qpcFreq * 0.00002);  // worker tasks >= 20 us
    g_on = true;
    char m[200];
    std::string where;
    for (int i = 0; i < kHooked; ++i) {
      gpt::ModRva(g_orig[i], m, sizeof(m));
      where += std::string(where.empty() ? "" : ", ") + kHookName[i] + " -> " + m +
               (g_chained[i] ? " (behind the cockpit mask export's hook, R93)" : "");
    }
    Log("timeline: on (FrameTimeline=%d, window %d s, PoseProbe=%d, EventLog=%d): %s", c.timeline, c.windowSec,
        c.pose ? 1 : 0, c.events ? 1 : 0, where.c_str());
    g_lastSummaryMs = nowMs;
  }
  if (nowMs - g_lastDrainMs < 1000) return;  // the rest once per second
  g_lastDrainMs = nowMs;
  if (!HooksIntact()) {  // replaced by another feature: off now, re-armed by a later pass
    const int by = BrokenBy();  // R93
    Stop(by == 1   ? "the cockpit mask export took xrEndFrame / xrLocateViews; re-arming behind its hooks"
         : by == 2 ? "the cockpit mask export's xrEndFrame / xrLocateViews hook went away; re-arming on the slot"
                   : "an OpenXR dispatch slot was replaced by another feature; re-arming when it is free");
    g_lastTryMs = 0;  // R93: re-armed at the next pass, not 2 s later
    return;
  }
  g_poseOn = c.pose;
  if (c.events != g_evOn.load()) {
    if (c.events) {
      ID3D11Device* dev = perfmon::DcsDevice();
      std::string skipped;
      g_createHooks = InstallCreates(dev, &skipped);
      if (dev) dev->Release();
      g_prevFaults = 0;
      Log("timeline: event log on: %d of %d device create entries hooked%s%s", g_createHooks, kCreateSlots,
          skipped.empty() ? "" : "; not d3d11's own (left alone): ", skipped.c_str());
    } else {
      UninstallCreates();
      g_createHooks = 0;
    }
    g_evOn = c.events;
  }
  if (c.timeline >= 2 && !g_csv) OpenCsv(dir);
  if (c.timeline < 2 && g_csv) CloseCsv();
  // Drain.
  std::vector<Rec> fresh;
  {
    Lock l;
    fresh.swap(g_done);
  }
  std::vector<Task> tasks;
  {
    std::lock_guard<std::mutex> lock(g_taskMutex);
    tasks.swap(g_tasks);
  }
  std::vector<Event> events;
  {
    std::lock_guard<std::mutex> lock(g_evMutex);
    events.swap(g_events);
  }
  const double qpcToMs = qpcToUs / 1000.0, cycToMs = tscHz > 0 ? 1000.0 / tscHz : 0;
  if (!fresh.empty() && !g_csvT0) g_csvT0 = fresh.front().e1;
  if (g_csv && !fresh.empty()) {
    std::vector<Row> rows;
    std::vector<Task> all = g_windowTasks;
    all.insert(all.end(), tasks.begin(), tasks.end());
    // Rows need the window's medians: written per drain with the medians of what is drained so far this window.
    std::vector<Rec> win = g_window;
    win.insert(win.end(), fresh.begin(), fresh.end());
    std::vector<Row> allRows;
    Summarize(win, all, qpcToMs, cycToMs, qpcFreq, &allRows, g_csvT0);
    rows.assign(allRows.end() - static_cast<std::ptrdiff_t>(fresh.size()), allRows.end());
    WriteRows(rows);
  }
  if (g_evCsv) WriteEvents(events, tasks, qpcToMs);
  g_window.insert(g_window.end(), fresh.begin(), fresh.end());
  g_windowTasks.insert(g_windowTasks.end(), tasks.begin(), tasks.end());
  if (nowMs - g_lastSummaryMs < static_cast<uint64_t>(c.windowSec) * 1000) return;
  g_lastSummaryMs = nowMs;
  TaskScope self(kTTimeline);
  if (g_window.empty()) {
    Log("timeline: no frames in the last %d s (menu, loading, paused or 2D)", c.windowSec);
    g_windowTasks.clear();
    return;
  }
  const Summary m = Summarize(g_window, g_windowTasks, qpcToMs, cycToMs, qpcFreq);
  for (const std::string& line : FormatSummary(m, c.pose, c.events)) Log("timeline: %s", line.c_str());
  const uint64_t pt = g_probeTicks.load(), pc = g_probeCalls.load();
  if (c.pose)
    Log("timeline: pose probe calls %.2f per frame, %.3f ms each, failed %llu, VIEW space %s", m.frames ? double(pc - g_probeCallsPrev) / m.frames : 0.0,
        pc > g_probeCallsPrev ? (pt - g_probeTicksPrev) * qpcToMs / double(pc - g_probeCallsPrev) : 0.0,
        static_cast<unsigned long long>(g_probeFails.load()),
        g_spaceState.load() == 1 ? "created" : g_spaceState.load() < 0 ? "FAILED" : "not yet");
  g_probeTicksPrev = pt;
  g_probeCallsPrev = pc;
  if (const uint64_t d = g_dropped.exchange(0)) Log("timeline: %llu frames dropped (worker too slow)", static_cast<unsigned long long>(d));
  if (const uint64_t d = g_threadMismatch.exchange(0))
    Log("timeline: %llu frames had xrBeginFrame and xrEndFrame on different threads (not split)", static_cast<unsigned long long>(d));
  g_window.clear();
  g_windowTasks.clear();
}

// Payload stop. The slots go back to their originals in every case (a hook
// already running finishes in this pinned module); the worker-owned state and
// the CSV files only when the worker has stopped.
void Shutdown(bool workerIdle) {
  if (workerIdle) {
    Stop(nullptr);
    return;
  }
  UninstallHooks();
  UninstallCreates();
  g_poseOn = false;
  g_evOn = false;
}

}  // namespace ftl
