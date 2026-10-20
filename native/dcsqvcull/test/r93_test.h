// R93 (docs/research/R92_live_hitch_fg.md, "R93"): the two defects of the R92 live session.
//   D1  [Log] Async=1 dropped ~950 lines at every DCS start (256-line ring): a 4096-line ring, a non-frame DCS thread
//       waits (bounded) for ring space, a frame thread (the render thread) never waits and never writes the file.
//   D2  [Perf] FrameTimeline never recorded while the cockpit mask export owned xrEndFrame / xrLocateViews (R76 wait):
//       the timeline now observes behind the export's hooks (one owner per OpenXR dispatch slot).
#pragma once

namespace r93 {

// ---------------------------------------------------------------------------
// D1: async log ring
// ---------------------------------------------------------------------------

struct LogFile {
  std::wstring path;
  FILE* f = nullptr;
  FILE* saved = nullptr;
  bool Open() {
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    path = std::wstring(tmp) + L"r93_log_" + std::to_wstring(GetCurrentProcessId()) + L".log";
    f = _wfsopen(path.c_str(), L"w", _SH_DENYWR);
    if (!f) return false;
    std::lock_guard<std::mutex> lock(g_logMutex);
    saved = g_log;
    g_log = f;
    return true;
  }
  std::vector<std::string> Close() {
    {
      std::lock_guard<std::mutex> lock(g_logMutex);
      g_log = saved;
    }
    fclose(f);
    std::vector<std::string> lines = r90::ReadLines(path);
    DeleteFileW(path.c_str());
    return lines;
  }
};

void WaitWritten() {  // the writer has written every queued line
  for (int i = 0; i < 2500 && qvalog::g_written.load() < qvalog::g_queued.load(); ++i) Sleep(2);
}

constexpr int kLt = 4, kLn = 500;   // a DCS start: 4 loading threads, 2000 lines
constexpr int kBt = 3, kBn = 4000;  // 3x the ring
constexpr int kFn = 6000;           // frame-thread lines with the file locked

void AsyncLogStart() {
  LogFile lf;
  if (!lf.Open()) {
    Check(false, "R93 D1: temp log file");
    return;
  }
  const bool want0 = qvalog::g_want.load();
  qvalog::g_want = true;
  qvalog::g_dropped = 0;
  qvalog::g_queued = 0;
  qvalog::g_written = 0;
  qvalog::g_waited = 0;
  qvalog::g_frameDropped = 0;
  {
    std::lock_guard<std::mutex> d(qvalog::g_drainMutex);  // the reports count from here (an earlier test's left)
    qvalog::g_droppedLogged = 0;
    qvalog::g_waitedLogged = 0;
  }
  StartAsyncLog();
  const bool on = qvalog::g_on.load();

  // 1. A DCS start: a mission load's burst of 2000 lines from 4 loading threads (not frame threads), unpaced.
  {
    std::vector<std::thread> th;
    for (int t = 0; t < kLt; ++t)
      th.emplace_back([t] {
        for (int i = 0; i < kLn; ++i) Log("r93 start t=%d n=%d", t, i);
      });
    for (auto& x : th) x.join();
  }
  WaitWritten();
  const uint64_t startDropped = qvalog::g_dropped.load();
  // 2. A burst 3x the ring from 3 loading threads (12000 lines): they wait for ring space, nothing is dropped.
  uint64_t burstUs = 0;
  {
    const uint64_t a = qvpatch::NowUs();
    std::vector<std::thread> th;
    for (int t = 0; t < kBt; ++t)
      th.emplace_back([t] {
        for (int i = 0; i < kBn; ++i) Log("r93 big t=%d n=%d", t, i);
      });
    for (auto& x : th) x.join();
    burstUs = qvpatch::NowUs() - a;
  }
  WaitWritten();
  const uint64_t bigDropped = qvalog::g_dropped.load() - startDropped, waited = qvalog::g_waited.load();

  // 3. The render thread (a frame thread) with nobody able to write the file: the test holds the drain lock (the
  //    writer and any synchronous line block) and the log mutex (every file write). The frame thread's 6000 lines
  //    must neither block nor write: 4096 fit, the rest are dropped and counted. Then a loading thread's line with
  //    the ring still full: it waits at most kFullWaitMs and is dropped.
  uint64_t frameUs = 0, frameMaxUs = 0, loadUs = 0;
  uint64_t frameDropped = 0, dropAfterLoad = 0;
  {
    std::unique_lock<std::mutex> drain(qvalog::g_drainMutex);
    std::unique_lock<std::mutex> file(g_logMutex);
    const uint64_t q0 = qvalog::g_queued.load(), d0 = qvalog::g_dropped.load();
    std::thread([&frameUs, &frameMaxUs] {
      qvalog::NoteFrameThread();  // as HookCollect / the pass hook / the xrEndFrame hooks do on DCS's render thread
      const uint64_t a = qvpatch::NowUs();
      for (int i = 0; i < kFn; ++i) {
        const uint64_t b = qvpatch::NowUs();
        Log("r93 frame n=%d", i);
        frameMaxUs = std::max<uint64_t>(frameMaxUs, qvpatch::NowUs() - b);
      }
      frameUs = qvpatch::NowUs() - a;
    }).join();
    frameDropped = qvalog::g_dropped.load() - d0;
    const uint64_t frameQueued = qvalog::g_queued.load() - q0;
    std::thread([&loadUs] {
      const uint64_t a = qvpatch::NowUs();
      Log("r93 loading line with the ring full");
      loadUs = qvpatch::NowUs() - a;
    }).join();
    dropAfterLoad = qvalog::g_dropped.load() - d0 - frameDropped;
    printf("     (R93 D1: frame thread %d lines with the file locked: %llu queued, %llu dropped, %.1f ms total, max "
           "%.1f ms per line; a loading thread's line with the ring full returned after %.1f ms)\n",
           kFn, static_cast<unsigned long long>(frameQueued), static_cast<unsigned long long>(frameDropped),
           frameUs / 1000.0, frameMaxUs / 1000.0, loadUs / 1000.0);
    Check(frameQueued == qvalog::kAsyncSlots && frameDropped == kFn - qvalog::kAsyncSlots &&
              qvalog::g_frameDropped.load() == frameDropped && frameUs < 500000 && frameMaxUs < 50000,
          "R93 D1: a frame thread (the render thread) never waits and never writes the file: with the file and the "
          "drain locked elsewhere its 6000 lines return at once, 4096 queued, the rest dropped and counted");
    Check(dropAfterLoad == 1 && loadUs >= qvalog::kFullWaitMs * 1000 - 20000 && loadUs < 2000000,
          "R93 D1: a loading thread's wait for ring space is bounded (about 250 ms), then its line is dropped and "
          "counted");
  }
  WaitWritten();
  qvalog::g_testHold = false;
  StopAsyncLog();
  const uint64_t queued = qvalog::g_queued.load(), dropped = qvalog::g_dropped.load(), written = qvalog::g_written.load();
  const std::vector<std::string> lines = lf.Close();
  qvalog::g_want = want0;

  int start = 0, big = 0, frame = 0, dropLines = 0, waitLines = 0;
  bool ordered = true;
  int lastS[kLt], lastB[kBt], lastF = -1;
  for (int& x : lastS) x = -1;
  for (int& x : lastB) x = -1;
  uint64_t droppedReported = 0;
  for (const std::string& l : lines) {
    if (l.size() < 13) continue;
    const char* text = l.c_str() + 13;
    int t = 0, n = 0;
    if (sscanf_s(text, "r93 start t=%d n=%d", &t, &n) == 2 && t >= 0 && t < kLt) {
      ordered &= n == lastS[t] + 1;
      lastS[t] = n;
      ++start;
    } else if (sscanf_s(text, "r93 big t=%d n=%d", &t, &n) == 2 && t >= 0 && t < kBt) {
      ordered &= n == lastB[t] + 1;
      lastB[t] = n;
      ++big;
    } else if (sscanf_s(text, "r93 frame n=%d", &n) == 1) {
      ordered &= n == lastF + 1;
      lastF = n;
      ++frame;
    } else if (!strncmp(text, "log: ", 5) && strstr(text, "dropped (async")) {
      ++dropLines;
      unsigned long long k = 0;
      if (sscanf_s(text, "log: %llu", &k) == 1) droppedReported += k;
    } else if (!strncmp(text, "log: ", 5) && strstr(text, "waited for ring space")) {
      ++waitLines;
    }
  }
  printf("     (R93 D1: start burst %d of %d lines written, %llu dropped; 12000-line burst in %.1f ms: %d written, "
         "%llu dropped, %llu lines waited for ring space (%d report lines))\n",
         start, kLt * kLn, static_cast<unsigned long long>(startDropped), burstUs / 1000.0, big,
         static_cast<unsigned long long>(bigDropped), static_cast<unsigned long long>(waited), waitLines);
  Check(on && startDropped == 0 && start == kLt * kLn && ordered,
        "R93 D1: a DCS start's burst of 2000 lines from 4 loading threads: no line dropped, every line written, each "
        "thread's lines in order");
  Check(bigDropped == 0 && big == kBt * kBn && waited > 0 && waitLines > 0,
        "R93 D1: a 12000-line burst (3x the ring) from loading threads: they wait for ring space, nothing dropped, the "
        "waits reported");
  printf("     (R93 D1: %d frame lines written; queued %llu, written %llu, dropped %llu, reported %llu in %d lines)\n", frame,
         static_cast<unsigned long long>(queued), static_cast<unsigned long long>(written),
         static_cast<unsigned long long>(dropped), static_cast<unsigned long long>(droppedReported), dropLines);
  Check(frame == static_cast<int>(qvalog::kAsyncSlots) && written == queued && droppedReported == dropped &&
            dropLines > 0,
        "R93 D1: the frame thread's queued lines are written by the writer in order; every drop is reported");
}

// ---------------------------------------------------------------------------
// D2: FrameTimeline behind the cockpit mask export's hooks
// ---------------------------------------------------------------------------

namespace cmask = fixdepth::cmask;

void TimelineBehindMask() {
  using namespace ftltest;
  ResetFake();
  ftl::Stop(nullptr);
  void** const disp = g_fakeDisp;
  void* const fEnd = disp[ftl::kSlotEnd];
  void* const fLoc = disp[ftl::kSlotLocate];
  void* const maskEnd = reinterpret_cast<void*>(&cmask::H_EndFrame);
  void* const maskLoc = reinterpret_cast<void*>(&cmask::ctime::H_LocateViews);
  // The export owns 35 / 36 (as its InstallXr leaves them), counting its own hook calls.
  auto maskInstall = [&] {
    cmask::g_xeOrig = reinterpret_cast<cmask::XrEndFn>(fEnd);
    cmask::ctime::g_orig = reinterpret_cast<cmask::ctime::LocateFn>(fLoc);
    disp[ftl::kSlotEnd] = maskEnd;
    disp[ftl::kSlotLocate] = maskLoc;
    cmask::g_xeSlot = &disp[ftl::kSlotEnd];
    cmask::ctime::g_slot = &disp[ftl::kSlotLocate];
    cmask::g_xeOn = true;
    cmask::g_xrHooked = true;
    cmask::ctime::g_hooked = true;
    cmask::ctime::g_on = true;
    cmask::ctime::g_testRecordAlways = true;
  };
  maskInstall();
  const uint64_t xe0 = cmask::g_xeCalls.load(), lv0 = cmask::ctime::g_calls.load();
  const bool installed = ftl::InstallOn(disp);
  const bool chained = installed && ftl::g_chained[2] && ftl::g_chained[3] && !ftl::g_armed[2] && !ftl::g_armed[3] &&
                       ftl::g_armed[0] && ftl::g_armed[1] && disp[ftl::kSlotEnd] == maskEnd &&
                       disp[ftl::kSlotLocate] == maskLoc && ftl::HooksIntact() && ftl::XrSlotsOwnedElsewhere();
  ftl::g_on = true;
  std::vector<ftl::Rec> recs;
  int ends0 = g_rt.ends;
  // Off the render thread id the tests may have set (the export's commit stays out of it: only its counters move).
  std::thread([&recs] { recs = DrivePipelined(12, 0.5); }).join();
  int paired = 0;
  for (const ftl::Rec& r : recs) paired += r.pdt && r.pdt == r.displayTime && r.l1 && r.e1 > r.e0 ? 1 : 0;
  const uint64_t xeCalls = cmask::g_xeCalls.load() - xe0, lvCalls = cmask::ctime::g_calls.load() - lv0;
  cmask::CheckXrSlots();
  const bool maskKept = cmask::g_xrHooked.load() && cmask::ctime::g_hooked.load() && !cmask::g_xeLost.load();
  printf("     (R93 D2: behind the mask's hooks: %zu frames recorded (%d paired), runtime xrEndFrame %d calls, the "
         "mask's xrEndFrame hook %llu / xrLocateViews hook %llu calls)\n",
         recs.size(), paired, g_rt.ends - ends0, static_cast<unsigned long long>(xeCalls),
         static_cast<unsigned long long>(lvCalls));
  Check(chained, "R93 D2: with the cockpit mask export owning xrEndFrame / xrLocateViews the timeline installs behind "
                 "its hooks: the export keeps both slots, the timeline hooks only xrWaitFrame / xrBeginFrame there");
  Check(recs.size() == 12 && paired == 12 && g_rt.ends - ends0 == 12 && xeCalls == 12 && lvCalls == 13 && maskKept,
        "R93 D2: both run: every frame recorded and paired by the timeline, every xrEndFrame / xrLocateViews passes "
        "through the export's hook once and reaches the runtime once; the export still sees its slots as its own");

  // Stop: the export keeps its slots; only the link behind it goes; its hooks forward to the runtime again.
  ftl::Stop(nullptr);
  const bool unlinked = !cmask::g_xeNext.load() && !cmask::ctime::g_next.load() && disp[ftl::kSlotEnd] == maskEnd &&
                        disp[ftl::kSlotLocate] == maskLoc && disp[ftl::kSlotWait] == reinterpret_cast<void*>(&F_Wait) &&
                        !ftl::g_chained[2] && !ftl::g_chained[3];
  ends0 = g_rt.ends;
  CallEnd(1);
  const bool forwards = g_rt.ends == ends0 + 1;
  Check(unlinked && forwards, "R93 D2: the timeline's stop unlinks it from the export's hooks and leaves the export's "
                              "slots alone; the export's hook forwards to the runtime again");

  // The export goes away while the timeline is behind it: detected (re-armed on the slot), then the export comes back
  // over the timeline's own hooks: detected (re-armed behind it).
  const bool again = ftl::InstallOn(disp) && ftl::HooksIntact();
  cmask::UninstallXr();
  const bool left = disp[ftl::kSlotEnd] == fEnd && disp[ftl::kSlotLocate] == fLoc && !cmask::g_xeNext.load() &&
                    !ftl::HooksIntact() && ftl::BrokenBy() == 2;
  ftl::Stop(nullptr);
  const bool direct = ftl::InstallOn(disp) && ftl::g_armed[2] && ftl::g_armed[3] && !ftl::g_chained[2] &&
                      disp[ftl::kSlotEnd] == ftl::kHookFn[2] && ftl::HooksIntact();
  maskInstall();
  const bool took = !ftl::HooksIntact() && ftl::BrokenBy() == 1;
  ftl::Stop(nullptr);
  const bool rechained = disp[ftl::kSlotEnd] == maskEnd && ftl::InstallOn(disp) && ftl::g_chained[2] &&
                         ftl::g_chained[3] && ftl::HooksIntact();
  Check(again && left && direct && took && rechained,
        "R93 D2: the export leaving its slots breaks the chain (seen, re-armed on the slots), the export taking the "
        "timeline's slots is seen too (re-armed behind its hooks)");

  // Clean up: as the tests found it.
  ftl::Stop(nullptr);
  cmask::ctime::g_testRecordAlways = false;
  cmask::UninstallXr();
  cmask::g_xeOrig = nullptr;
  cmask::ctime::g_orig = nullptr;
  ResetFake();
}

void Run() {
  AsyncLogStart();
  TimelineBehindMask();
}

}  // namespace r93
