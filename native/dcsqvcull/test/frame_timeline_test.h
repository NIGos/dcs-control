// Offline tests for frame_timeline.h (R52): pure parts (clock, cadence,
// segments, attribution, summary), the OpenXR hooks on a fake dispatch table
// driven like DCS (Main one frame ahead of the render thread), the pose probe
// on a fake runtime, the device create hooks on WARP, the worker task log,
// the keys' defaults; plus thread_place.h's R52 re-check cadence.
#pragma once

namespace ftltest {

using namespace ftl;

// ---------------------------------------------------------------------------
// A fake runtime behind a fake loader dispatch table
// ---------------------------------------------------------------------------
struct FakeRt {
  int64_t freq = 0;
  int64_t periodNs = 11111111;
  int64_t nextPdt = 0;
  int waits = 0, begins = 0, ends = 0, locates = 0, locSpace = 0, created = 0;
  int64_t lastEndTime = 0;
  int64_t yawRadPerS1000 = 0;  // head yaw rate (mrad/s), the pose depends on the call's QPC time
  double mmPerS = 0;           // head x speed
  int failEnd = 0;
};
FakeRt g_rt;
void* g_fakeDisp[64] = {};

inline int64_t NowNs() { return QpcToNs(Now(), g_rt.freq); }

void Spin(double ms) {
  const int64_t end = Now() + static_cast<int64_t>(ms * g_rt.freq / 1000.0);
  volatile uint64_t v = 1;
  while (Now() < end) v = v * 6364136223846793005ull + 1;
}

int32_t __stdcall F_Wait(uint64_t, const void*, FrameState* s) {
  ++g_rt.waits;
  // Display slots: the next vsync at least 25 ms ahead of now (a runtime predicting for a pipelined app).
  const int64_t now = NowNs();
  int64_t t = g_rt.nextPdt ? g_rt.nextPdt : now;
  while (t < now + 25000000) t += g_rt.periodNs;
  g_rt.nextPdt = t + g_rt.periodNs;
  s->predictedDisplayTime = t;
  s->predictedDisplayPeriod = g_rt.periodNs;
  s->shouldRender = 1;
  return 0;
}
int32_t __stdcall F_Begin(uint64_t, const void*) {
  ++g_rt.begins;
  return 0;
}
int32_t __stdcall F_End(uint64_t, const FrameEndInfo* i) {
  ++g_rt.ends;
  g_rt.lastEndTime = i ? i->displayTime : 0;
  Spin(1.0);  // a driver drain (not Sleep: its length depends on the timer resolution)
  return g_rt.failEnd ? -1 : 0;
}
int32_t __stdcall F_Locate(uint64_t, const ViewLocateInfo*, void*, uint32_t cap, uint32_t* count, void*) {
  ++g_rt.locates;
  if (count) *count = 4;
  (void)cap;
  return 0;
}
int32_t __stdcall F_LocSpace(uint64_t space, uint64_t base, int64_t, SpaceLocation* loc) {
  ++g_rt.locSpace;
  if (space != 0x5150 || base != 0xBA5E || !loc || loc->type != kTypeSpaceLocation) return -1;
  // The newest pose (what a runtime returns when asked again later): yaw and x grow with QPC time.
  const double t = Now() / static_cast<double>(g_rt.freq);
  const double yaw = g_rt.yawRadPerS1000 / 1000.0 * t;
  loc->locationFlags = kLocOrientationValid | kLocPositionValid;
  loc->pose.o = {0.0f, static_cast<float>(std::sin(yaw / 2)), 0.0f, static_cast<float>(std::cos(yaw / 2))};
  loc->pose.p = {static_cast<float>(std::fmod(g_rt.mmPerS * t, 1000.0) / 1000.0), 1.2f, 0.0f};
  return 0;
}
int32_t __stdcall F_CreateSpace(uint64_t session, const RefSpaceCreateInfo* ci, uint64_t* out) {
  ++g_rt.created;
  if (session != 0x5E55 || !ci || ci->type != kTypeRefSpaceCreateInfo || ci->referenceSpaceType != kRefSpaceView ||
      ci->poseInReferenceSpace.o.w != 1.0f)
    return -1;
  *out = 0x5150;
  return 0;
}

void ResetFake() {
  g_rt = FakeRt{};
  LARGE_INTEGER f;
  QueryPerformanceFrequency(&f);
  g_rt.freq = f.QuadPart;
  for (auto& p : g_fakeDisp) p = nullptr;
  g_fakeDisp[kSlotWait] = reinterpret_cast<void*>(&F_Wait);
  g_fakeDisp[kSlotBegin] = reinterpret_cast<void*>(&F_Begin);
  g_fakeDisp[kSlotEnd] = reinterpret_cast<void*>(&F_End);
  g_fakeDisp[kSlotLocate] = reinterpret_cast<void*>(&F_Locate);
  g_fakeDisp[kSlotLocateSpace] = reinterpret_cast<void*>(&F_LocSpace);
  g_fakeDisp[kSlotCreateRefSpace] = reinterpret_cast<void*>(&F_CreateSpace);
}

// DCS's calls through the table (what Visualizer's trampolines do).
int32_t CallWait(FrameState* s) { return reinterpret_cast<WaitFn>(g_fakeDisp[kSlotWait])(0x5E55, nullptr, s); }
int32_t CallLocate(int64_t t) {
  ViewLocateInfo li{};
  li.type = 6;
  li.viewConfigurationType = 1000037000;  // XR_VIEW_CONFIGURATION_TYPE_PRIMARY_QUAD_VARJO
  li.displayTime = t;
  li.space = 0xBA5E;
  uint32_t n = 0;
  char views[4 * 64] = {};
  char state[24] = {};
  return reinterpret_cast<LocateViewsFn>(g_fakeDisp[kSlotLocate])(0x5E55, &li, state, 4, &n, views);
}
int32_t CallBegin() { return reinterpret_cast<BeginFn>(g_fakeDisp[kSlotBegin])(0x5E55, nullptr); }
int32_t CallEnd(int64_t t) {
  FrameEndInfo e{};
  e.type = 12;
  e.displayTime = t;
  return reinterpret_cast<EndFn>(g_fakeDisp[kSlotEnd])(0x5E55, &e);
}

// DCS's order: Main waits and locates frame k+1 right after the render thread began frame k.
std::vector<Rec> DrivePipelined(int frames, double renderMs, int slowFrame = -1, double slowMs = 0) {
  FrameState s{};
  CallWait(&s);
  int64_t next = s.predictedDisplayTime;
  CallLocate(next);
  for (int k = 0; k < frames; ++k) {
    const int64_t mine = next;
    CallBegin();
    FrameState s2{};
    CallWait(&s2);  // Main, frame k+1
    next = s2.predictedDisplayTime;
    CallLocate(next);
    Spin(k == slowFrame ? slowMs : renderMs);  // render thread CPU
    CallEnd(mine);
  }
  std::vector<Rec> out;
  Lock l;
  out.swap(g_done);
  return out;
}

void Pure() {
  Check(QpcToNs(10000000, 10000000) == 1000000000LL && QpcToNs(15, 10) == 1500000000LL &&
            QpcToNs(0x7fffffffffffLL, 10000000) == 0x7fffffffffffLL * 100 && QpcToNs(5, 0) == 0,
        "timeline: QPC -> ns without overflow");
  Check(ClockPlausible(30000000) && ClockPlausible(-10000000) && !ClockPlausible(-60000000) &&
            !ClockPlausible(600000000) && !ClockPlausible(-1700000000000000000LL),
        "timeline: XrTime as QPC ns plausible only from -50 ms to +500 ms");
  const int64_t p = 11111111;
  Check(SlotsBetween(1000, 1000 + 2 * p, p) == 2 && SlotsBetween(1000, 1000 + 3 * p + 100, p) == 3 &&
            SlotsBetween(0, 5 * p, p) == 0 && SlotsBetween(5, 5, p) == 0 && SlotsBetween(1, 2, 0) == 0,
        "timeline: refreshes between predicted display times");
  Check(TargetSlots(21.2, 11.111) == 2 && TargetSlots(11.7, 11.111) == 1 && TargetSlots(23.0, 11.111) == 2 &&
            TargetSlots(28.0, 11.111) == 3 && TargetSlots(10.0, 11.111) == 1 && TargetSlots(0, 11.1) == 0 &&
            TargetSlots(16.4, 13.889) == 1,
        "timeline: cadence target = median / refresh period, rounded (21.2 ms at 90 Hz = 2, 11.7 ms = 1)");
  XrQuat a{0, 0, 0, 1}, b{0, static_cast<float>(std::sin(0.5 * 0.01745329)), 0, static_cast<float>(std::cos(0.5 * 0.01745329))};
  Check(std::fabs(RotDeg(a, b) - 1.0) < 1e-3 && std::fabs(RotDeg(a, a)) < 1e-6 &&
            std::fabs(RotDeg(a, XrQuat{0, 0, 0, -1})) < 1e-6,
        "timeline: rotation between orientations (deg, q and -q equal)");
  Check(std::fabs(DistMm({0, 0, 0}, {0.003f, 0.004f, 0}) - 5.0) < 1e-3, "timeline: translation in mm");
  // Split: a frame of 20 ms: pre-begin 0.5, begin 0.2, body 18 (15 CPU), submit 1.3.
  Rec r;
  r.prevE1 = 1000;
  r.b0 = 1005;
  r.b1 = 1007;
  r.e0 = 1187;
  r.e1 = 1200;
  r.cycB1 = 100;
  r.cycE0 = 100 + 150;
  Segs s = Split(r, 0.1, 0.1);
  Check(s.valid && std::fabs(s.interval - 20) < 1e-9 && std::fabs(s.preBegin - 0.5) < 1e-9 &&
            std::fabs(s.begin - 0.2) < 1e-9 && std::fabs(s.bodyCpu - 15) < 1e-9 && std::fabs(s.bodyIdle - 3) < 1e-9 &&
            std::fabs(s.submit - 1.3) < 1e-9,
        "timeline: frame split into pre-begin / begin / render CPU / blocked / submit");
  Rec bad = r;
  bad.b0 = 900;
  Rec noPrev = r;
  noPrev.prevE1 = 0;
  Rec cyc = r;
  cyc.cycE0 = 100 + 500;  // more cycles than wall time: capped
  Check(!Split(bad, 0.1, 0.1).valid && !Split(noPrev, 0.1, 0.1).valid && Split(cyc, 0.1, 0.1).bodyIdle == 0,
        "timeline: out-of-order or first frames are not split; CPU capped at the wall time");
  // Attribution against medians.
  Segs med;
  med.valid = true;
  med.interval = 21;
  med.preBegin = 0.5;
  med.begin = 0.2;
  med.bodyCpu = 18;
  med.bodyIdle = 1;
  med.submit = 1.3;
  auto with = [&](double pre, double beg, double cpu, double idle, double sub) {
    Segs x;
    x.valid = true;
    x.preBegin = pre;
    x.begin = beg;
    x.bodyCpu = cpu;
    x.bodyIdle = idle;
    x.submit = sub;
    x.interval = pre + beg + cpu + idle + sub;
    return Attribute(x, med, 22.222);
  };
  Check(with(0.5, 0.2, 18, 1, 1.3) == kOnTime && with(9, 0.2, 18, 1, 1.3) == kSim && with(0.5, 6, 18, 1, 1.3) == kRuntime &&
            with(0.5, 0.2, 27, 1, 1.3) == kRenderCpu && with(0.5, 0.2, 18, 9, 1.3) == kRenderBlocked &&
            with(0.5, 0.2, 18, 1, 11) == kSubmit && with(3, 0.2, 21, 1, 4) == kMixed &&
            Attribute(Segs{}, med, 22.2) == kNoData && with(0.5, 0.2, 18, 1, 2.9) == kOnTime,
        "timeline: over-budget class = the segment with at least half of the excess, else mixed");
  // Summarize: 6 synthetic pipelined frames, 1 slow, one task overlapping it.
  std::vector<Rec> recs;
  const int64_t f = 10000000;  // QPC 10 MHz: 1 tick = 0.1 us
  const double toMs = 1e-4;
  int64_t e1 = 1000 * f;      // QPC ticks
  const int64_t per = 11111111;
  int64_t pdt = QpcToNs(e1, f) + 30000000;
  for (int k = 0; k < 6; ++k) {
    Rec x;
    const bool slow = k == 3;
    const int64_t iv = static_cast<int64_t>((slow ? 30.0 : 21.0) / toMs);
    x.prevE1 = e1;
    x.b0 = e1 + 5000;
    x.b1 = x.b0 + 2000;
    x.e1 = e1 + iv;
    x.e0 = x.e1 - 13000;
    x.cycB1 = 1;
    x.cycE0 = 1 + static_cast<uint64_t>((x.e0 - x.b1) * 0.9);
    x.pdt = pdt;
    x.period = per;
    x.displayTime = pdt;
    x.w0 = e1 - 100000;
    x.w1 = e1 - 90000;
    x.l0 = x.w1 + 1000;
    x.l1 = x.w1 + 2000;
    x.seqAtLoc = k;
    x.seq = k + 1;
    x.mainCyc = 190000;
    x.hasRef = x.hasBeg = x.hasEnd = true;
    x.ref.o = {0, 0, 0, 1};
    x.beg.o = b;
    x.end.o = b;
    x.end.p = {0.002f, 0, 0};
    if (slow) {
      x.creates = 2;
      x.createTicks = 50000;
      x.shLate = 1;
    }
    recs.push_back(x);
    e1 = x.e1;
    pdt += 2 * per;
  }
  std::vector<Task> tasks = {{kTPlace, recs[3].prevE1 + 1000, recs[3].prevE1 + 101000}, {kTStats, 0, 10}};
  std::vector<Row> rows;
  Summary m = Summarize(recs, tasks, toMs, toMs, f, &rows);
  Check(m.frames == 6 && m.valid == 6 && std::fabs(m.ivP50 - 21) < 1e-6 && m.targetSlots == 2 &&
            std::fabs(m.budgetMs - 22.222222) < 1e-3 && m.over == 1 && m.cls[kRenderCpu] == 1 && rows.size() == 6 &&
            rows[3].over && rows[3].cls == kRenderCpu,
        "timeline: summary finds the one slow frame and blames render CPU");
  std::vector<double> hz;
  for (const Rec& x : recs) hz.push_back((x.pdt - QpcToNs(x.l1, f)) / 1e6);
  Check(m.slots[1] == 5 && m.cadenceChanges == 0 && m.clockOk && m.marginN == 6 && m.late == 0 &&
            std::fabs(m.horizonP50 - Pct(hz, 0.5)) < 1e-6 && std::fabs(hz[0] - 38.8) < 1e-6 && m.depth[1] == 6 &&
            std::fabs(m.ageP50 - 29.8) < 1e-6,
        "timeline: slots 2, XrTime clock accepted, horizon and pose age, one frame pipelined");
  Check(m.poseN == 6 && std::fabs(m.rotBegP50 - 1.0) < 1e-3 && std::fabs(m.trEndP50 - 2.0) < 1e-3 &&
            m.overWithTask == 1 && (m.taskOverMask & (1u << kTPlace)) && m.overWithCreate == 1 && m.overWithLate == 1 &&
            rows[3].taskMs > 9.9 && rows[3].taskMs < 10.1 && rows[2].taskMask == 0,
        "timeline: pose deltas and the events overlapping the over-budget frame");
  // A clock that is not QPC ns: no deadline numbers.
  std::vector<Rec> other = recs;
  for (Rec& x : other) x.pdt += 1000000000000LL;
  Summary mo = Summarize(other, {}, toMs, toMs, f);
  Check(!mo.clockOk && mo.marginN == 0, "timeline: deadline metrics only with a QPC-ns XrTime");
  const std::vector<std::string> lines = FormatSummary(m, true, true);
  bool hasOver = false, hasPose = false, hasEv = false, hasDead = false;
  for (const std::string& l : lines) {
    hasOver |= l.find("over budget 1") != std::string::npos;
    hasPose |= l.find("pose probe (6 frames)") != std::string::npos;
    hasEv |= l.find("placement") != std::string::npos;
    hasDead |= l.find("deadline: predicted display") != std::string::npos;
  }
  Check(lines.size() == 8 && hasOver && hasPose && hasEv && hasDead && FormatSummary(Summary{}, false, false).size() == 1,
        "timeline: summary lines (budget, causes, slots, deadline, pose, probe, events)");
  Check(TaskList(0) == "-" && TaskList((1u << kTPerf) | (1u << kTCensus)) == "perf_mon+census", "timeline: task names");
  Check(std::fabs(m.locateP50 - 0.1) < 1e-9 && std::fabs(rows[0].budget - m.budgetMs) < 1e-12,
        "timeline: xrLocateViews duration and the budget per row");
  // CSV: header and every row have the same number of columns; the events file too.
  wchar_t tmp[MAX_PATH];
  GetTempPathW(MAX_PATH, tmp);
  const std::wstring dir = std::wstring(tmp) + L"qv_ftl_csv_test\\";
  CreateDirectoryW(dir.c_str(), nullptr);
  CloseCsv();
  OpenCsv(dir);
  g_csvT0 = recs.front().e1;
  WriteRows(rows);
  WriteEvents({Event{3, recs[2].e1, recs[2].e1 + 5000, 0}}, {Task{kTPlace, recs[3].e1, recs[3].e1 + 2000}}, toMs);
  CloseCsv();
  int files = 0, badCols = 0, rowLines = 0, evLines = 0;
  WIN32_FIND_DATAW fd;
  HANDLE h = FindFirstFileW((dir + L"*.csv").c_str(), &fd);
  if (h != INVALID_HANDLE_VALUE) {
    do {
      ++files;
      const std::wstring path = dir + fd.cFileName;
      FILE* fp = nullptr;
      _wfopen_s(&fp, path.c_str(), L"r");
      char line[2048];
      int cols0 = -1;
      const bool isEv = wcsncmp(fd.cFileName, L"events", 6) == 0;
      while (fp && fgets(line, sizeof(line), fp)) {
        int cols = 1;
        for (const char* c = line; *c; ++c) cols += *c == ',';
        if (cols0 < 0) cols0 = cols;
        else if (cols != cols0) ++badCols;
        if (isEv) ++evLines; else ++rowLines;
      }
      if (fp) fclose(fp);
      DeleteFileW(path.c_str());
    } while (FindNextFileW(h, &fd));
    FindClose(h);
  }
  RemoveDirectoryW(dir.c_str());
  Check(files == 2 && badCols == 0 && rowLines == 7 && evLines == 3, "timeline: CSV header and rows agree (frames and events files)");
}

void Hooks() {
  ResetFake();
  Stop(nullptr);
  void* origEnd = g_fakeDisp[kSlotEnd];
  Check(InstallOn(g_fakeDisp) && g_fakeDisp[kSlotEnd] != origEnd && HooksIntact(), "timeline: hooks on the dispatch table");
  // Off: pass-through, nothing recorded.
  g_on = false;
  DrivePipelined(3, 1.0);
  {
    Lock l;
    Check(g_done.empty() && g_rt.ends == 3 && g_rt.waits == 4, "timeline: with the key off the hooks only forward");
  }
  g_on = true;
  g_poseOn = true;
  g_rt.yawRadPerS1000 = 1745;  // 100 deg/s
  g_rt.mmPerS = 200;
  g_prevE1 = 0;
  const std::vector<Rec> recs = DrivePipelined(40, 4.0, 20, 15.0);
  LARGE_INTEGER f;
  QueryPerformanceFrequency(&f);
  const double toMs = 1000.0 / f.QuadPart;
  std::vector<Row> rows;
  const Summary m = Summarize(recs, {}, toMs, g_tscHz > 0 ? 1000.0 / g_tscHz : toMs, f.QuadPart, &rows);
  int paired = 0;
  for (const Rec& r : recs) paired += r.pdt && r.pdt == r.displayTime && r.l1 && r.hasRef && r.hasBeg && r.hasEnd ? 1 : 0;
  Check(recs.size() == 40 && paired == 40 && g_spaceState.load() == 1 && g_rt.created == 1,
        "timeline: every frame paired by display time (wait, locate, begin, end), VIEW space created once");
  Check(m.depth[0] == 1 && m.depth[1] == 39 && m.clockOk && m.unpaired == 0 && m.over >= 1 && rows[20].over &&
            rows[20].cls == kRenderCpu,
        "timeline: DCS-like order measured as one frame pipelined; the slow frame is over budget");
  // Pose deltas grow with time since the sample: begin < end, ~100 deg/s.
  Check(m.poseN == 40 && m.rotEndP50 > m.rotBegP50 && m.rotEndP50 > 0.2 && m.rotEndP50 < 3.0 && m.trEndP50 > m.trBegP50,
        "timeline: pose probe deltas (ref -> xrBeginFrame -> xrEndFrame) grow with the head's motion");
  // A frame submitted with another display time than the one waited: not paired.
  FrameState s{};
  CallBegin();
  CallEnd(12345);
  {
    Lock l;
    Check(!g_done.empty() && g_done.back().pdt == 0 && g_done.back().displayTime == 12345,
          "timeline: an xrEndFrame display time that was not waited is left unpaired");
    g_done.clear();
  }
  (void)s;
  // A failed xrEndFrame: forwarded, not recorded, the next interval not measured.
  g_rt.failEnd = 1;
  CallBegin();
  const int32_t fr = CallEnd(1);
  g_rt.failEnd = 0;
  {
    Lock l;
    Check(fr < 0 && g_done.empty() && g_prevE1 == 0, "timeline: a failed xrEndFrame is forwarded and not recorded");
  }
  // Another feature replaces a slot: not intact; Stop puts back only our own entries.
  void* foreign = reinterpret_cast<void*>(&F_Begin);
  g_fakeDisp[kSlotWait] = foreign;
  Check(!HooksIntact(), "timeline: a replaced slot is detected");
  Stop(nullptr);
  Check(g_fakeDisp[kSlotWait] == foreign && g_fakeDisp[kSlotEnd] == origEnd &&
            g_fakeDisp[kSlotLocate] == reinterpret_cast<void*>(&F_Locate) && !g_on.load(),
        "timeline: stop restores our slots and leaves a foreign one alone");
  g_spaceState = 0;
  g_viewSpace = 0;
}

void Creates() {
  ID3D11Device* dev = nullptr;
  D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
  if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, &fl, 1, D3D11_SDK_VERSION, &dev, nullptr,
                               nullptr))) {
    printf("SKIP timeline: no WARP device\n");
    return;
  }
  void** vt = *reinterpret_cast<void***>(dev);
  void* before = vt[3];
  std::string skipped;
  const int n = InstallCreates(dev, &skipped);
  const bool bufferHooked = g_cSlot[0] != nullptr;
  g_evOn = true;
  g_renderTid = GetCurrentThreadId();
  g_rtCreates = 0;
  D3D11_BUFFER_DESC bd{};
  bd.ByteWidth = 256;
  bd.Usage = D3D11_USAGE_DEFAULT;
  bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
  ID3D11Buffer* buf = nullptr;
  const HRESULT hr = dev->CreateBuffer(&bd, nullptr, &buf);
  const uint32_t counted = g_rtCreates.load();
  size_t evs = 0;
  {
    std::lock_guard<std::mutex> lock(g_evMutex);
    evs = g_events.size();
    g_events.clear();
  }
  if (buf) buf->Release();
  UninstallCreates();
  g_evOn = false;
  g_renderTid = 0;
  if (!skipped.empty()) printf("     (create entries already hooked by another test, left alone: %s)\n", skipped.c_str());
  Check(n >= 1 && bufferHooked && g_cSlot[0] == nullptr && SUCCEEDED(hr) && counted == 1 && evs == 1 && vt[3] == before,
        "timeline: device creates counted on the render thread (d3d11's entries), hooks removed again");
  dev->Release();
}

void TasksAndKeys() {
  LARGE_INTEGER f;
  QueryPerformanceFrequency(&f);
  g_taskMinTicks = static_cast<int64_t>(f.QuadPart * 0.00002);
  {
    std::lock_guard<std::mutex> lock(g_taskMutex);
    g_tasks.clear();
  }
  { TaskScope off(kTPerf); Spin(0.1); }  // event log off: nothing
  g_evOn = true;
  { TaskScope shortOne(kTConfig); }      // below 20 us: nothing
  { TaskScope t(kTCensus); Spin(0.2); }
  g_evOn = false;
  size_t n = 0;
  int kind = -1;
  {
    std::lock_guard<std::mutex> lock(g_taskMutex);
    n = g_tasks.size();
    if (n) kind = g_tasks[0].kind;
    g_tasks.clear();
  }
  Check(n == 1 && kind == kTCensus, "timeline: worker tasks recorded only with the event log on and >= 20 us");
  // Keys: defaults off; the timeline's Tick with FrameTimeline=0 installs nothing.
  Config c;
  Check(c.frameTimeline == 0 && !c.poseProbe && !c.eventLog && c.timelineSec == 10 && c.placeRecheckSec == 60,
        "timeline: [Perf] keys default off, [Threads] PlaceRecheckSec 60");
  Cfg off;
  Tick(off, L".\\", 0.1, 1e9, 10000000);
  Check(!g_on.load() && g_disp == nullptr, "timeline: FrameTimeline=0 installs nothing");
}

DWORD WINAPI NopThread(void*) { return 0; }
void NopBody() {}

void Placement() {
  Check(tplace::PlaceEveryMs(0, 60000) == 60000 && tplace::PlaceEveryMs(0) == 10000 && tplace::PlaceEveryMs(2, 60000) == 1000 &&
            tplace::PlaceEveryMs(-1, 60000) == 1000 && tplace::RecheckMs(60) == 60000 && tplace::RecheckMs(1) == 10000 &&
            tplace::RecheckMs(99999) == 3600000,
        "threads: R52 steady-state re-check 60 s (10-3600), 1 s while threads move");
  const uint32_t s0 = qvexit::g_threadStarts.load();
  HANDLE t = CreateThread(nullptr, 0, qvexit::Thread<&NopThread>, nullptr, 0, nullptr);
  if (t) {
    WaitForSingleObject(t, 5000);
    CloseHandle(t);
  }
  std::thread th([] { qvexit::RunVoid(&NopBody); });
  th.join();
  Check(qvexit::g_threadStarts.load() >= s0 + 2, "threads: every payload thread start is counted (Run and RunVoid)");
}

// R49: the CSVs stay readable while the timeline writes them (the R52 live script copies them with DCS running).
void CsvShared() {
  wchar_t tmp[MAX_PATH];
  GetTempPathW(MAX_PATH, tmp);
  std::wstring dir = std::wstring(tmp) + L"qv_ftl_csv_" + std::to_wstring(GetCurrentProcessId()) + L"\\";
  CreateDirectoryW(dir.c_str(), nullptr);
  ftl::CloseCsv();
  ftl::OpenCsv(dir);
  bool opened = ftl::g_csv && ftl::g_evCsv, readable = true;
  WIN32_FIND_DATAW fd{};
  int files = 0;
  const HANDLE h = FindFirstFileW((dir + L"*.csv").c_str(), &fd);
  if (h != INVALID_HANDLE_VALUE) {
    do {
      ++files;
      FILE* r = _wfsopen((dir + fd.cFileName).c_str(), L"rb", _SH_DENYNO);
      if (r) fclose(r); else readable = false;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
  }
  ftl::CloseCsv();
  if (HANDLE h2 = FindFirstFileW((dir + L"*.csv").c_str(), &fd); h2 != INVALID_HANDLE_VALUE) {
    do DeleteFileW((dir + fd.cFileName).c_str());
    while (FindNextFileW(h2, &fd));
    FindClose(h2);
  }
  RemoveDirectoryW(dir.c_str());
  Check(opened && files == 2 && readable, "timeline R49: timeline and events CSVs can be read while they are open for writing");
}

void Run() {
  Pure();
  CsvShared();
  Hooks();
  Creates();
  TasksAndKeys();
  Placement();
}

}  // namespace ftltest
