// Binaural (HRTF) audio module (docs/BINAURAL_AUDIO.md, docs/BINAURAL_AUDIO_HOOKS.md): the wrappers ed_sound calls,
// the install / restore of its four call sites, the binding of the WASAPI renderer to a BinauralCore, and the module's
// own control thread (config and Live reload, kill switch flag and hotkey, HRTF loading, log lines, status bits).
//
// Independent of the engine: its own thread (the worker may wait minutes for Scene.dll, and audio also runs with the
// engine optimizations off or in PerceptionOnly); the engine kill switch never touches it, and its own kill switch
// (toggle_audio.flag, [Hotkeys] AudioToggle) never touches the engine. Nothing is patched unless [Audio] Binaural=1,
// and the control thread itself starts only once the ini says Binaural=1 (at payload start, or a Live change the
// worker's ini reload hands to StartIfWanted): with 3D audio off the module polls nothing and logs nothing.
//
// Threads:
//   - DCS simulation thread: PanWrapA / PanWrapB (P3a / P3b) -> audsim (ownership, tag, VoiceMeta);
//   - DCS render thread: MixWrap (P5) arms one device block, RenderWrap (P4) renders candidate voices (audr);
//   - the module's control thread (AudioThread): everything that may allocate, log or touch files.
// The real-time threads never log: they count (audr::Counters) and push events (audr::EvRing); this thread logs.
namespace audio {

using audr::kHaveCore;

// ---- state ---------------------------------------------------------------------------------------------------------
audhk::Resolved g_res;                    // written before any site points at us, then read-only
std::atomic<bool> g_installed{false};     // the four sites call our thunks
std::atomic<int> g_hookState{0};          // 0 not tried / waiting for ed_sound, 1 installed, -1 refused for the session
audv::MetaTable g_meta;
audr::Counters g_cnt;
audr::EvRing g_ev;
audsim::Sim g_sim;
std::atomic<audr::Binding*> g_binding{nullptr};
std::atomic<uint32_t> g_epoch{0};         // bumped when the binding changes (the sim drops every slot)
std::atomic<int> g_inflight{0};           // wrapper calls in progress
std::atomic<void*> g_newRenderer{nullptr};  // render thread -> control thread: an unknown renderer called P5
std::atomic<bool> g_userOff{false};       // kill switch (toggle_audio.flag / [Hotkeys] AudioToggle)
std::atomic<bool> g_stopping{false};      // Stop is fading everything back before the sites are restored
enum OffReason : int { kOffNone = 0, kOffOverruns = 1, kOffPanLaw = 2 };
std::atomic<int> g_offReason{kOffNone};   // binaural off for the rest of the DCS session (overruns: until re-armed)
std::atomic<uint32_t> g_overrunGen{0};    // bumped when the overrun stop is re-armed (bindings drop their overrun times)
// The renderer the world's 3D sounds play on (control thread picks it from audsim's counts, MixWrap reads it).
std::atomic<void*> g_worldRenderer{nullptr};
constexpr double kLiveBindingS = 0.2;     // a binding whose renderer delivered a block within this is in use
// The bound renderer's blocks pass through (render thread sets, the sim and the control thread read): the frame count
// of a block too long for the core, 1 for a block whose layout changed, 0 while blocks are armed. The sim releases every
// voice meanwhile (they would play centred: tagged but not rendered).
std::atomic<uint32_t> g_passFrames{0};
// The binding is moving to another live renderer (control thread): the sim releases every voice first, so the old
// renderer's blocks fade them back to DCS's panning before its binding is retired (no centred voices, no step).
std::atomic<bool> g_handover{false};
void* g_handoverTo = nullptr;             // control thread
ULONGLONG g_handoverSince = 0;
constexpr ULONGLONG kHandoverMaxMs = 500;
int g_stopGraceMs = 50;                   // Stop: quiet time before freeing (a wrapper not yet counted in g_inflight)
std::atomic<dcsvr_headpose::Block*> g_pose{nullptr};
std::atomic<uint32_t> g_selfPid{0};       // the pose block's writer must be this process (set by Start)
thread_local audr::Block* t_block = nullptr;

// The head pose readers' limits (head_pose_reader.h): 150 ms staleness, 50 ms horizon, speed guards, this process.
inline dcsvr_headpose::ReaderLimits PoseLimits() {
  dcsvr_headpose::ReaderLimits l;
  l.expectedPid = g_selfPid.load(std::memory_order_relaxed);
  return l;
}

// Runtime settings the real-time threads read (control writes, seqlock).
struct Rt {
  bool binaural = false;
  bool late = true;
  float trim = 1.0f;
  int maxObjects = 48;
  int cpuBudget = 25;
  uint32_t exclude = 0;
};
struct RtSlot {
  std::atomic<uint32_t> seq{0};
  Rt d;
};
RtSlot g_rt;
inline void WriteRt(const Rt& r) {
  const uint32_t q = g_rt.seq.load(std::memory_order_relaxed);
  g_rt.seq.store(q + 1, std::memory_order_relaxed);
  std::atomic_thread_fence(std::memory_order_release);
  g_rt.d = r;
  std::atomic_thread_fence(std::memory_order_release);
  g_rt.seq.store(q + 2, std::memory_order_release);
}
// Each reading thread keeps its last good copy: a writer preempted between its two stores must not switch binaural
// off for a frame (that would release every owned voice and fade it back in with a new generation).
thread_local Rt t_rtLast;
inline Rt ReadRt() {
  for (int i = 0; i < 64; ++i) {
    const uint32_t a = g_rt.seq.load(std::memory_order_acquire);
    if (a & 1u) continue;
    Rt r = g_rt.d;
    std::atomic_thread_fence(std::memory_order_acquire);
    if (g_rt.seq.load(std::memory_order_relaxed) == a) return t_rtLast = r;
  }
  return t_rtLast;  // a writer kept the slot busy: this thread's last good copy
}

// Test overrides (production: null).
const uint8_t* (*g_testEdBase)() = nullptr;
audhk::ImportFn g_testImports = nullptr;
const audhk::SigTable* g_testTable = nullptr;
bool (*g_testHrtf)(const wchar_t* name, std::vector<uint8_t>* out) = nullptr;
std::atomic<bool> g_testNoThread{false};  // tests drive ControlTick themselves
void (*g_testAfterRestore)() = nullptr;   // Stop: right after the sites are restored

inline int64_t Qpc() {
  LARGE_INTEGER c;
  QueryPerformanceCounter(&c);
  return c.QuadPart;
}
inline int64_t QpcFreq() {
  static const int64_t f = [] {
    LARGE_INTEGER x;
    QueryPerformanceFrequency(&x);
    return x.QuadPart;
  }();
  return f;
}

struct InflightScope {
  InflightScope() { g_inflight.fetch_add(1, std::memory_order_acq_rel); }
  ~InflightScope() { g_inflight.fetch_sub(1, std::memory_order_acq_rel); }
};

// ---- simulation thread ----------------------------------------------------------------------------------------------
audsim::Cfg g_simCfg;  // sim thread only
audsim::Env g_simEnv;
uint32_t g_simEpoch = 0;
// listener-vs-pose diagnostic: per frame, the listener's and the head's turn since the previous frame (radians)
struct DiagRing {
  std::atomic<uint32_t> head{0};
  float listener[256] = {};
  float headTurn[256] = {};
} g_diag;
double g_lastListenerQ[4] = {0, 0, 0, 1};
float g_lastHeadQ[4] = {0, 0, 0, 1};
bool g_diagHave = false;
// The context whose listener the diagnostic reads: the first 3D one (context 0 is DCS's 2D one, whose listener never
// turns with the head; the world's 3D sounds play on 2 or 3). -1: none yet. Sim thread writes, control thread logs it.
std::atomic<int> g_diagCtx{-1};

inline double QuatAngle(const double a[4], const double b[4]) {
  const double d = std::fabs(a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3]);
  return 2.0 * std::acos(d > 1.0 ? 1.0 : d);
}

void SimFrame(uint32_t f) {
  const Rt rt = ReadRt();
  audr::Binding* b = g_binding.load(std::memory_order_acquire);
  const uint32_t ep = g_epoch.load(std::memory_order_acquire);
  const bool epochReset = ep != g_simEpoch;
  g_simEpoch = ep;
  const bool on = rt.binaural && b && b->core && !b->retired.load(std::memory_order_relaxed) &&
                  !g_userOff.load(std::memory_order_relaxed) && g_offReason.load(std::memory_order_relaxed) == kOffNone &&
                  !g_stopping.load(std::memory_order_relaxed) && !g_sim.panLawTripped &&
                  g_passFrames.load(std::memory_order_relaxed) == 0 && !g_handover.load(std::memory_order_relaxed);
  g_simCfg.on = on;
  g_simCfg.limit = std::min<int>(rt.maxObjects, static_cast<int>(g_cnt.limitNow.load(std::memory_order_relaxed)));
  g_simCfg.excludeMask = rt.exclude;
  audsim::Env& e = g_simEnv;
  e.boundRenderer = b ? b->renderer : nullptr;
  e.ctxArray = g_res.ctxArray;
  e.ctxCount = g_res.ctxCount ? *g_res.ctxCount : 0;
  e.engineState = g_res.engineState ? *g_res.engineState : 3;
  e.now = Qpc();
  e.freq = QpcFreq();
  e.poseValid = false;
  e.posValid = false;
  dcsvr_headpose::Block* pb = rt.late ? g_pose.load(std::memory_order_acquire) : nullptr;
  dcsvr_headpose::PredictedPose pp;
  // The pose DCS used for this frame: the newest checked entry as it is (its extrapolation is not used here).
  if (pb && dcsvr_headpose::ReadPredicted(pb, e.now, e.now, PoseLimits(), pp) == dcsvr_headpose::PoseStatus::kOk) {
    const dcsvr_headpose::Entry& pe = pp.entry;
    e.poseValid = true;
    e.posValid = (pe.flags & dcsvr_headpose::entry_flags::position_valid) != 0;
    memcpy(e.q, pe.orientation, sizeof(e.q));
    memcpy(e.t, pe.position, sizeof(e.t));
    e.poseBase = pe.baseSpace;
  }
  // Diagnostic: the listener of the first 3D context against the head pose, per frame.
  int diagCtx = -1;
  for (int i = 0; e.ctxArray && i < e.ctxCount && i < audv::off::kMaxContexts; ++i)
    if (audv::Rd<uint8_t>(e.ctxArray + audv::off::kCtxStride * i, audv::off::kCtxIs2D) == 0) {
      diagCtx = i;
      break;
    }
  if (diagCtx != g_diagCtx.load(std::memory_order_relaxed)) {
    g_diagCtx.store(diagCtx, std::memory_order_relaxed);
    g_diagHave = false;  // another listener: no turn across the change
  }
  if (diagCtx >= 0) {
    double lq[4];
    memcpy(lq, e.ctxArray + audv::off::kCtxStride * diagCtx + audv::off::kCtxQuat, sizeof(lq));
    const double hq[4] = {e.q[0], e.q[1], e.q[2], e.q[3]};
    const double lh[4] = {g_lastHeadQ[0], g_lastHeadQ[1], g_lastHeadQ[2], g_lastHeadQ[3]};
    if (g_diagHave && e.poseValid) {
      const uint32_t h = g_diag.head.load(std::memory_order_relaxed);
      g_diag.listener[h & 255] = static_cast<float>(QuatAngle(lq, g_lastListenerQ));
      g_diag.headTurn[h & 255] = static_cast<float>(QuatAngle(hq, lh));
      g_diag.head.store(h + 1, std::memory_order_release);
    }
    memcpy(g_lastListenerQ, lq, sizeof(lq));
    memcpy(g_lastHeadQ, e.q, sizeof(g_lastHeadQ));
    g_diagHave = e.poseValid;
  }
  audsim::BeginFrame(g_sim, g_simCfg, e, f, epochReset);
}

void PanWrapImpl(void* voice, bool initial) {
  InflightScope in;
  g_res.panner(voice);  // DCS's own panner first: doppler, filters, its state exactly stock
  if (!g_installed.load(std::memory_order_acquire) || qvexit::Exiting()) return;
  const uint32_t f = *g_res.frameCounter;
  if (!g_sim.own.haveFrame || f != g_sim.own.frame) SimFrame(f);
  audsim::PanVoice(g_sim, g_simCfg, g_simEnv, voice, initial);
}
void PanWrapA(void* voice) { PanWrapImpl(voice, false); }
void PanWrapB(void* voice) { PanWrapImpl(voice, true); }

// ---- render thread --------------------------------------------------------------------------------------------------
int RenderWrap(void* voice, float* out, uint32_t frames) {
  InflightScope in;
  audr::Block* blk = t_block;
  if (!blk || blk->out != out || blk->frames != frames) {
    g_cnt.unarmedCalls.fetch_add(1, std::memory_order_relaxed);
    return g_res.voiceRender(voice, out, frames);
  }
  int slot = -1;
  for (int s = 0; s < audv::kSlots; ++s)
    if (g_meta.voice[s].load(std::memory_order_acquire) == voice) {
      slot = s;
      break;
    }
  if (slot < 0) return g_res.voiceRender(voice, out, frames);  // not a candidate: DCS's own call, untouched
  audr::Binding& b = *blk->b;
  float* scratch = b.scratch;
  const int64_t t0 = Qpc();
  memset(scratch, 0, sizeof(float) * 2 * frames);
  const int64_t t1 = Qpc();
  const int r = g_res.voiceRender(voice, scratch, frames);
  const int64_t t2 = Qpc();
  float snap[6];  // channels 0..2: the gains just rendered and the stereo tag's mark (audio_voice.h IsStereoTag)
  memcpy(snap, static_cast<const uint8_t*>(voice) + audv::off::kVSnapGain, sizeof(snap));
  audv::MetaData md;
  const bool ok = audv::MetaRead(g_meta.slot[slot], &md);
  if (!ok) g_meta.torn.fetch_add(1, std::memory_order_relaxed);
  if (ok && md.voice != voice) {  // the slot was just given to another voice: this one is DCS's
    for (uint32_t i = 0; i < 2 * frames; ++i) out[i] += scratch[i];
    return r;
  }
  {
    audr::MxcsrScope fz;
    audr::ProcessVoice(*blk, g_cnt, slot, voice, scratch, snap, ok, md, r == 0, g_meta.ackGen);
  }
  g_cnt.candidateCalls.fetch_add(1, std::memory_order_relaxed);
  blk->dspTicks += (t1 - t0) + (Qpc() - t2);
  return r;
}

void MixWrap(void* rend, float* out, uint32_t frames) {
  InflightScope in;
  g_cnt.blocks.fetch_add(1, std::memory_order_relaxed);
  audr::Binding* b = g_binding.load(std::memory_order_acquire);
  if (!b || b->renderer != rend || b->retired.load(std::memory_order_relaxed)) {
    // Another renderer (DCS's second output, options.lua sound.hp_output, has its own thread calling here): the control
    // thread is told only when it could be the one to bind: nothing bound, the bound one stopped delivering blocks, or
    // this one carries the world's 3D sounds. Two live outputs never take turns.
    if (g_installed.load(std::memory_order_relaxed) && (!b || b->renderer != rend)) {
      bool ask = !b || b->retired.load(std::memory_order_relaxed) || rend == g_worldRenderer.load(std::memory_order_relaxed);
      if (!ask)
        ask = static_cast<double>(Qpc() - b->lastUseQpc.load(std::memory_order_relaxed)) >
              kLiveBindingS * static_cast<double>(QpcFreq());
      if (ask) g_newRenderer.store(rend, std::memory_order_release);
    }
    g_res.mix(rend, out, frames);
    return;
  }
  if (frames > audr::kMaxFrames || audv::Rd<uint32_t>(rend, audv::off::kRChannels) != 2 ||
      audv::Rd<double>(rend, audv::off::kRRate) != static_cast<double>(b->rate)) {
    if (frames > audr::kMaxFrames) g_cnt.framesTooLong.fetch_add(1, std::memory_order_relaxed);
    else g_newRenderer.store(rend, std::memory_order_release);  // its layout changed: validate again
    // The owned voices are not rendered in this block (they play centred): the sim releases them (and the control
    // thread says why when the blocks are too long).
    g_passFrames.store(frames > audr::kMaxFrames ? frames : 1u, std::memory_order_relaxed);
    g_cnt.passBlocks.fetch_add(1, std::memory_order_relaxed);
    g_res.mix(rend, out, frames);
    return;
  }
  if (g_passFrames.load(std::memory_order_relaxed)) g_passFrames.store(0, std::memory_order_relaxed);
  const Rt rt = ReadRt();
  audr::Block blk;
  blk.b = b;
  blk.out = out;
  blk.frames = frames;
  blk.on = rt.binaural && !g_userOff.load(std::memory_order_relaxed) &&
           g_offReason.load(std::memory_order_relaxed) == kOffNone && !g_stopping.load(std::memory_order_relaxed) &&
           !qvexit::Exiting();
  blk.late = rt.late;
  blk.trim = rt.trim;
  const int64_t now = Qpc(), freq = QpcFreq();
  if (rt.late) {
    dcsvr_headpose::Block* pb = g_pose.load(std::memory_order_acquire);
    // The newest head pose, predicted to the end of this block (when its last sample leaves our hands): checked for
    // staleness, writer and plausible speeds, extrapolated at most 50 ms (head_pose_reader.h).
    const int64_t target = now + static_cast<int64_t>(static_cast<double>(frames) / b->rate * static_cast<double>(freq));
    dcsvr_headpose::PredictedPose pp;
    if (pb && dcsvr_headpose::ReadPredicted(pb, now, target, PoseLimits(), pp) == dcsvr_headpose::PoseStatus::kOk) {
      blk.pose.valid = true;
      blk.pose.posValid = (pp.entry.flags & dcsvr_headpose::entry_flags::position_valid) != 0;
      blk.pose.q = pp.orientation;
      blk.pose.t = pp.position;
      blk.pose.base = pp.entry.baseSpace;
      g_cnt.poseBlocks.fetch_add(1, std::memory_order_relaxed);
    }
  }
  t_block = &blk;
  g_res.mix(rend, out, frames);
  t_block = nullptr;
  if (blk.coreBegun) {
    audr::MxcsrScope fz;
    audr::CoreEnd(b->core);
  }
  b->lastUseQpc.store(now, std::memory_order_relaxed);
  g_cnt.armedBlocks.fetch_add(1, std::memory_order_relaxed);
  g_cnt.activeLastBlock.store(blk.hrtfVoices, std::memory_order_relaxed);
  audr::BudgetTick(*b, g_cnt, g_ev, {rt.cpuBudget, rt.maxObjects}, frames, blk.dspTicks, now, freq);
  // ED's overrun counter counts any slow block, ours or not (a system or DCS hitch). An overrun is held against binaural
  // only when our own work took a real share of this block or the one before (half the CPU budget or more); our usual
  // cost (a few percent of a block) never stops it for somebody else's hitch.
  const double share =
      static_cast<double>(blk.dspTicks) / static_cast<double>(freq) / (static_cast<double>(frames) / static_cast<double>(b->rate));
  const double ours = 0.5 * rt.cpuBudget / 100.0;
  const bool attributable = blk.hrtfVoices > 0 && (share >= ours || b->prevShare >= ours);
  b->prevShare = share;
  const uint32_t og = g_overrunGen.load(std::memory_order_relaxed);
  if (b->overrunGen != og) {  // re-armed (kill switch, Live Binaural): earlier overruns no longer count
    b->overrunGen = og;
    memset(b->overrunTimes, 0, sizeof(b->overrunTimes));
  }
  if (audr::OverrunTick(*b, attributable, now, freq)) {
    g_cnt.overruns.fetch_add(1, std::memory_order_relaxed);
    int none = kOffNone;
    if (g_offReason.compare_exchange_strong(none, kOffOverruns)) g_ev.Push({audr::kEvOverrunsOff, 3, 0, 0});
  }
}

// ---- control thread: install / restore -------------------------------------------------------------------------------
const uint8_t* EdBase() {
  if (g_testEdBase) return g_testEdBase();
  return sigscan::ImageBase(GetModuleHandleW(L"ed_sound.dll"));
}

std::string g_offText;  // the last "audio: off (...)" reason logged (control thread)
void LogOff(const std::string& why) {
  if (why == g_offText) return;
  g_offText = why;
  Log("audio: off (%s)", why.c_str());
}
void LogOn() { g_offText.clear(); }

// The binaural stop for DCS's overruns is for the session, unless the pilot asks for it again (kill switch back on, or
// Binaural back to 1 in the settings). The pan-law stop stays (the output does not follow the pan law it measures).
bool RearmOverruns() {
  int over = kOffOverruns;
  if (!g_offReason.compare_exchange_strong(over, kOffNone)) return false;
  g_overrunGen.fetch_add(1, std::memory_order_acq_rel);
  return true;
}

bool Install() {
  const uint8_t* base = EdBase();
  if (!base) return false;  // not loaded yet: tried again
  audhk::Resolved r;
  if (!audhk::Resolve(base, g_testTable ? *g_testTable : audhk::Generated(), g_testImports ? g_testImports : &audhk::DefaultImport, &r)) {
    g_hookState = -1;
    LogOff(r.why);
    return false;
  }
  // ed_sound's engine not started yet (state 0: its mutex and globals may not be set up): nothing is locked or patched
  // now; tried again.
  if (!r.engineState || *r.engineState < 1) return false;
  void* const wrappers[audhk::kSiteCount] = {reinterpret_cast<void*>(&PanWrapA), reinterpret_cast<void*>(&PanWrapB),
                                              reinterpret_cast<void*>(&RenderWrap), reinterpret_cast<void*>(&MixWrap)};
  const char* thunkWhy = nullptr;
  uint8_t* page = audhk::MakeThunks(r.base, r.expected, wrappers, &thunkWhy);
  if (!page) {
    g_hookState = -1;
    LogOff(thunkWhy ? thunkWhy : "no memory within reach of ed_sound.dll");
    return false;
  }
  g_res = r;
  g_stopping.store(false);
  g_passFrames.store(0);
  g_handover.store(false);
  g_handoverTo = nullptr;
  g_sim.meta = &g_meta;
  g_sim.cnt = &g_cnt;
  g_sim.ev = &g_ev;
  g_sim.Reset();
  g_installed.store(true, std::memory_order_release);
  // The render side first (it passes through until a block is armed), then the panner calls under ED's mutex.
  bool ok = audhk::WriteCall(r.site[audhk::kP4], audhk::ThunkOf(page, audhk::kP4), false) &&
            audhk::WriteCall(r.site[audhk::kP5], audhk::ThunkOf(page, audhk::kP5), false);
  if (ok) {
    r.lock(r.mutex);
    ok = audhk::WriteCall(r.site[audhk::kP3a], audhk::ThunkOf(page, audhk::kP3a), true) &&
         audhk::WriteCall(r.site[audhk::kP3b], audhk::ThunkOf(page, audhk::kP3b), true);
    r.unlock(r.mutex);
  }
  if (!ok) {
    r.lock(r.mutex);
    audhk::WriteCall(r.site[audhk::kP3a], r.expected[audhk::kP3a], true);
    audhk::WriteCall(r.site[audhk::kP3b], r.expected[audhk::kP3b], true);
    r.unlock(r.mutex);
    audhk::WriteCall(r.site[audhk::kP5], r.expected[audhk::kP5], false);
    audhk::WriteCall(r.site[audhk::kP4], r.expected[audhk::kP4], false);
    g_installed.store(false, std::memory_order_release);
    g_hookState = -1;
    LogOff("could not write ed_sound.dll's call sites");
    return false;
  }
  g_hookState = 1;
  const IMAGE_NT_HEADERS64* nt = sigscan::NtHeaders(base);
  Log("audio: hooks in (ed_sound.dll 0x%08x, %s)", nt ? nt->FileHeader.TimeDateStamp : 0u,
      r.analysedBuild ? "the analysed build" : "located by pattern");
  return true;
}

// Fades every owned voice back to DCS's panning, then puts the four call sites back. Bounded waits.
void Uninstall(DWORD waitMs) {
  if (!g_installed.load()) return;
  g_stopping.store(true);
  const ULONGLONG until = GetTickCount64() + waitMs;
  for (;;) {
    bool any = false;
    for (int s = 0; s < audv::kSlots; ++s) any |= g_meta.voice[s].load() != nullptr;
    if (!any || GetTickCount64() >= until) break;
    Sleep(5);
  }
  const audhk::Resolved& r = g_res;
  r.lock(r.mutex);
  audhk::WriteCall(r.site[audhk::kP3a], r.expected[audhk::kP3a], true);
  audhk::WriteCall(r.site[audhk::kP3b], r.expected[audhk::kP3b], true);
  r.unlock(r.mutex);
  audhk::WriteCall(r.site[audhk::kP5], r.expected[audhk::kP5], false);
  audhk::WriteCall(r.site[audhk::kP4], r.expected[audhk::kP4], false);
  g_installed.store(false, std::memory_order_release);
  const ULONGLONG drainUntil = GetTickCount64() + 1000;
  while (g_inflight.load() > 0 && GetTickCount64() < drainUntil) Sleep(1);
  g_hookState = 0;
  g_handover.store(false);
  g_handoverTo = nullptr;
  g_passFrames.store(0);
  Log("audio: hooks out (ed_sound.dll's call sites restored%s)", g_inflight.load() ? "; a call was still in flight" : "");
}

// ---- control thread: HRTF sets and the binding -----------------------------------------------------------------------
struct LoadedSet {
  binaural::HrtfSet* set = nullptr;
  uint32_t rate = 0;
  std::wstring name;
};
std::vector<LoadedSet> g_sets;                 // every set ever loaded and not yet released
std::vector<audr::Binding*> g_retiredBindings; // freed 2 s after their last block
audcfg::Read g_cfgRead;
bool g_haveCfg = false;
std::wstring g_hrtfName;                       // the set the current binding uses
std::wstring g_hrtfRequested;                  // the last [Audio] Hrtf value acted on
// A set as the log names it: "default" (the generic set) or "personal" (any of the pilot's own). The pilot names those
// sets, often after a person, and the log reaches diagnostic packages and issues: a set's name is never logged. The app
// knows which of the pilot's sets it deployed (DcsQvCull.ini Hrtf=).
constexpr const wchar_t* kLogPersonalSet = L"personal";
const wchar_t* LogSetName(const std::wstring& name) {
  return name == binaural_config::kDefaultHrtfName ? binaural_config::kDefaultHrtfName : kLogPersonalSet;
}
std::wstring g_cfgIni;
FILETIME g_cfgTime{}, g_cfgDevTime{};  // the effective ini's and the deployed ini's write times
void* g_rejectedRenderer = nullptr;
const binaural::HrtfSet* g_curSet = nullptr;   // the set the current binding's core was last given
// The renderer choice (WorldTick): the busiest renderer of the world's 3D sounds, adopted after 2 s in a row.
void* g_worldCandidate = nullptr;
int g_worldSeconds = 0;

bool HrtfBytes(const std::wstring& name, std::vector<uint8_t>* out, std::string* why) {
  if (g_testHrtf) {
    if (g_testHrtf(name.c_str(), out)) return true;
    *why = "test set missing";
    return false;
  }
  if (name == binaural_config::kDefaultHrtfName) {
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&HrtfBytes), &self);
    HRSRC res = self ? FindResourceW(self, binaural_config::kEmbeddedResourceName, RT_RCDATA) : nullptr;
    HGLOBAL h = res ? LoadResource(self, res) : nullptr;
    const void* p = h ? LockResource(h) : nullptr;
    const DWORD n = res ? SizeofResource(self, res) : 0;
    if (!p || !n) {
      *why = "this build has no default HRTF set";
      return false;
    }
    out->assign(static_cast<const uint8_t*>(p), static_cast<const uint8_t*>(p) + n);
    return true;
  }
  const std::wstring path = g_dir + binaural_config::kHrtfFolder + L"\\" + name + L".dcshrtf";
  HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
  if (f == INVALID_HANDLE_VALUE) {
    *why = "file not found";
    return false;
  }
  LARGE_INTEGER sz{};
  GetFileSizeEx(f, &sz);
  if (sz.QuadPart <= 0 || sz.QuadPart > (64ll << 20)) {
    CloseHandle(f);
    *why = "file size out of range";
    return false;
  }
  out->resize(static_cast<size_t>(sz.QuadPart));
  DWORD got = 0;
  const BOOL ok = ReadFile(f, out->data(), static_cast<DWORD>(out->size()), &got, nullptr);
  CloseHandle(f);
  if (!ok || got != out->size()) {
    *why = "file could not be read";
    return false;
  }
  return true;
}

// The set `name` at `rate` (loaded once; an unusable personal set falls back to the default, logged). *used: the
// name of the set returned.
binaural::HrtfSet* SetFor(std::wstring name, uint32_t rate, std::string* why, std::wstring* used) {
  for (int attempt = 0; attempt < 2; ++attempt) {
    *used = name;
    for (const LoadedSet& s : g_sets)
      if (s.rate == rate && s.name == name) return s.set;
    std::vector<uint8_t> bytes;
    std::string w;
    binaural::HrtfSet* set = nullptr;
    if (HrtfBytes(name, &bytes, &w)) {
      // 4-byte aligned copy for the core (vector storage is aligned for new)
      char err[160] = {};
      set = audr::CoreLoadHrtf(bytes.data(), bytes.size(), rate, err, sizeof(err));
      if (!set) w = err[0] ? err : "the set was refused";
    }
    if (set) {
      g_sets.push_back({set, rate, name});
      return set;
    }
    if (name == binaural_config::kDefaultHrtfName) {
      *why = w;
      return nullptr;
    }
    Log("audio: HRTF %ls not usable (%s); the default set is used", LogSetName(name), w.c_str());
    name = binaural_config::kDefaultHrtfName;
  }
  return nullptr;
}

// A set to render with exists (the named one, or the generic one it falls back to). Checked before ed_sound is patched.
bool HrtfReady(const std::wstring& name, std::string* why) {
  std::vector<uint8_t> bytes;
  std::string w;
  if (name != binaural_config::kDefaultHrtfName && HrtfBytes(name, &bytes, &w)) return true;
  if (HrtfBytes(binaural_config::kDefaultHrtfName, &bytes, &w)) return true;
  *why = w;
  return false;
}

// Once a second: the renderer most of the world's 3D sounds played on in the last second (audsim's counts), adopted as
// g_worldRenderer when it led two seconds in a row. Entries unseen for a second are dropped.
void WorldTick() {
  void* best = nullptr;
  uint32_t bestN = 0;
  for (audsim::Sim::RendCount& rc : g_sim.world) {
    const void* r = rc.rend.load(std::memory_order_relaxed);
    const uint32_t n = rc.n.exchange(0, std::memory_order_relaxed);
    if (!r) continue;
    if (!n) {
      rc.rend.store(nullptr, std::memory_order_relaxed);
      continue;
    }
    if (n > bestN) best = const_cast<void*>(r), bestN = n;
  }
  if (!best) return;
  g_worldSeconds = best == g_worldCandidate ? g_worldSeconds + 1 : 1;
  g_worldCandidate = best;
  if (g_worldSeconds >= 2 && g_worldRenderer.load() != best) {
    g_worldRenderer.store(best, std::memory_order_release);
    audr::Binding* b = g_binding.load();
    if (b && b->renderer != best) g_newRenderer.store(best, std::memory_order_release);  // bind it (it may be quiet now)
  }
}

bool ReadRenderer(void* rend, double* rate, uint32_t* ch, double z[2], bool lfe[2], float w3d[2][2]) {
  __try {
    const uint8_t* r = static_cast<const uint8_t*>(rend);
    *rate = audv::Rd<double>(r, audv::off::kRRate);
    *ch = audv::Rd<uint32_t>(r, audv::off::kRChannels);
    for (int k = 0; k < 2; ++k) {
      z[k] = audv::Rd<double>(r, audv::off::kRSpk + audv::off::kRSpkStride * k + audv::off::kSpkDir + 16);
      lfe[k] = audv::Rd<uint8_t>(r, audv::off::kRSpk + audv::off::kRSpkStride * k + audv::off::kSpkLfe) != 0;
      w3d[k][0] = audv::Rd<float>(r, audv::off::kRSpk + audv::off::kRSpkStride * k + audv::off::kSpkW3dA);
      w3d[k][1] = audv::Rd<float>(r, audv::off::kRSpk + audv::off::kRSpkStride * k + audv::off::kSpkW3dB);
    }
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

// Whether binaural can run on `rend` (DCS's stereo pair at a supported rate); why: the reason when not (may be null).
bool CheckRenderer(void* rend, double* rateOut, char* why, size_t whyCap) {
  double rate = 0, z[2] = {};
  uint32_t ch = 0;
  bool lfe[2] = {};
  float w3d[2][2] = {};
  if (why && whyCap) why[0] = 0;
  if (!ReadRenderer(rend, &rate, &ch, z, lfe, w3d)) return false;
  char w[160];
  // DCS's stereo pair (hook spec section 3): left speaker z < 0 with 3D weights (1, 0), right z > 0 with (0, 1).
  const bool pair = w3d[0][0] == 1.0f && w3d[0][1] == 0.0f && w3d[1][0] == 0.0f && w3d[1][1] == 1.0f;
  if (ch != 2 || !(z[0] < -0.5) || !(z[1] > 0.5) || lfe[0] || lfe[1] || !pair) {
    snprintf(w, sizeof(w), "DCS's audio output is not stereo (%u channels); set Headphones or Stereo in DCS", ch);
  } else if (!(rate >= binaural::kMinSampleRate && rate <= binaural::kMaxSampleRate)) {
    snprintf(w, sizeof(w), "output rate %.0f Hz is not supported", rate);
  } else if (!kHaveCore) {
    snprintf(w, sizeof(w), "this build has no binaural renderer");
  } else {
    w[0] = 0;
  }
  if (why && whyCap) strncpy_s(why, whyCap, w, _TRUNCATE);
  if (rateOut) *rateOut = rate;
  return w[0] == 0;
}
bool RendererUsable(void* rend) { return CheckRenderer(rend, nullptr, nullptr, 0); }

bool AnySlotInUse() {
  for (int s = 0; s < audv::kSlots; ++s)
    if (g_meta.voice[s].load() != nullptr) return true;
  return false;
}

// The binding of `rend`. how: why this renderer (logged with its address, so two DCS outputs can be told apart).
void Bind(void* rend, const char* how) {
  double rate = 0;
  char why[160];
  if (!CheckRenderer(rend, &rate, why, sizeof(why)) && !why[0]) return;  // unreadable: tried again later
  if (why[0]) {
    if (rend != g_rejectedRenderer) LogOff(why);
    g_rejectedRenderer = rend;
    return;
  }
  const binaural_config::AudioConfig& c = g_cfgRead.cfg;
  std::string w;
  std::wstring used;
  binaural::HrtfSet* set = SetFor(c.hrtf, static_cast<uint32_t>(rate), &w, &used);
  if (!set) {
    if (rend != g_rejectedRenderer) LogOff("no HRTF set: " + w);
    g_rejectedRenderer = rend;
    return;
  }
  binaural::Renderer* core = audr::CoreNew();
  binaural::Config cc;
  cc.sampleRate = static_cast<uint32_t>(rate);
  cc.maxSources = binaural::kMaxSources;
  cc.headRadiusM = binaural_config::PilotItdRadiusM(audr::CoreDescribe(set).datasetHeadRadiusM, c.headCircumferenceCm);
  cc.earRadiusM = binaural_config::HeadRadiusFromCircumferenceCm(c.headCircumferenceCm);  // the ears sit on the real head
  cc.nearField = c.nearField;
  char err[160] = {};
  if (!core || !audr::CoreInit(core, cc, err, sizeof(err))) {
    audr::CoreDelete(core);
    if (rend != g_rejectedRenderer) LogOff(std::string("the binaural renderer did not start: ") + err);
    g_rejectedRenderer = rend;
    return;
  }
  audr::CoreSetHrtf(core, set);
  g_curSet = set;
  auto* b = new audr::Binding();
  b->renderer = rend;
  b->rate = static_cast<uint32_t>(rate);
  b->core = core;
  // 3 x kMaxFrames: the voice's two channels, then the onset buffer (audr::Binding::onset)
  b->scratch = static_cast<float*>(_aligned_malloc(sizeof(float) * 3 * audr::kMaxFrames, 64));
  b->onset = b->scratch ? b->scratch + 2 * audr::kMaxFrames : nullptr;
  b->overrun = reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(rend) + audv::off::kROverrun);
  if (!b->scratch) {
    audr::CoreDelete(core);
    delete b;
    LogOff("no memory for the binaural renderer");
    return;
  }
  b->lastUseQpc.store(Qpc());
  audr::Binding* old = g_binding.exchange(b, std::memory_order_acq_rel);
  if (old) {
    old->retired.store(true);
    old->lastUseQpc.store(Qpc());
    g_retiredBindings.push_back(old);
  }
  g_epoch.fetch_add(1, std::memory_order_acq_rel);
  g_rejectedRenderer = nullptr;
  g_hrtfName = used;
  g_hrtfRequested = c.hrtf;
  const binaural::HrtfInfo info = audr::CoreDescribe(set);
  LogOn();
  Log("audio: output %p bound (%s%s)", rend, how, old ? (old->renderer == rend ? "; validated again" : "; moved from another output") : "");
  Log("audio: on (%.0f Hz stereo, HRTF %ls: %u directions, head radius %.1f cm, near field %s, at most %d sounds)", rate,
      LogSetName(used), info.directions, cc.headRadiusM * 100.0, c.nearField ? "on" : "off", c.maxObjects);
}

// "audio: on (<why>, HRTF <name>: <n> directions)" when binaural works again on the same binding (the app reads any
// "on (" line as running again).
void LogOnAgain(const char* why) {
  if (!g_binding.load() || !g_curSet) return;
  LogOn();
  Log("audio: on (%s, HRTF %ls: %u directions)", why, LogSetName(g_hrtfName), audr::CoreDescribe(g_curSet).directions);
}

void FreeBinding(audr::Binding* b) {
  audr::CoreDelete(b->core);
  _aligned_free(b->scratch);
  delete b;
}

// ---- control thread: configuration ----------------------------------------------------------------------------------
std::wstring EffectiveIni(std::wstring* devDir) {
  const std::wstring deployed = g_dir + L"DcsQvCull.ini";
  const qvdev::DevPath dp = qvdev::ReadDevPath(L"IniPath", deployed);
  if (dp.state == qvdev::kDevOk) {
    *devDir = dp.path.substr(0, dp.path.find_last_of(L"\\/") + 1);
    return dp.path;
  }
  devDir->clear();
  return deployed;
}

void PublishRt() {
  const binaural_config::AudioConfig& c = g_cfgRead.cfg;
  Rt r;
  r.binaural = c.binaural;
  r.late = c.lateHeadTracking;
  r.trim = binaural_config::TrimGain(c.levelTrimDb);
  r.maxObjects = c.maxObjects;
  r.cpuBudget = c.cpuBudgetPercent;
  r.exclude = c.excludeContextMask;
  WriteRt(r);
}

std::wstring g_devDirAudio;
void ReadConfig(bool force) {
  std::wstring devDir;
  const std::wstring ini = EffectiveIni(&devDir);
  WIN32_FILE_ATTRIBUTE_DATA a{}, d{};
  GetFileAttributesExW(ini.c_str(), GetFileExInfoStandard, &a);
  GetFileAttributesExW((g_dir + L"DcsQvCull.ini").c_str(), GetFileExInfoStandard, &d);
  if (!force && ini == g_cfgIni && CompareFileTime(&a.ftLastWriteTime, &g_cfgTime) == 0 &&
      CompareFileTime(&d.ftLastWriteTime, &g_cfgDevTime) == 0)
    return;
  g_cfgIni = ini;
  g_cfgTime = a.ftLastWriteTime;
  g_cfgDevTime = d.ftLastWriteTime;
  g_devDirAudio = devDir;
  audcfg::Read r = audcfg::ReadIni(ini.c_str());
  for (int i = 0; i < r.noteCount; ++i) Log("%s", r.notes[i]);
  const bool first = !g_haveCfg;
  const binaural_config::AudioConfig prev = g_cfgRead.cfg;
  g_cfgRead = r;
  g_haveCfg = true;
  g_rejectedRenderer = nullptr;  // a refused output is looked at again with the new settings
  const binaural_config::AudioConfig& c = r.cfg;
  PublishRt();
  if (first || !audcfg::Same(prev, c))
    Log("audio: config: Binaural=%d Hrtf=%ls HeadCircumferenceCm=%.1f LateHeadTracking=%d NearField=%d MaxObjects=%d "
        "LevelTrimDb=%.1f CpuBudgetPercent=%d ExcludeContexts=0x%02x LogVoices=%d",
        c.binaural ? 1 : 0, LogSetName(c.hrtf), c.headCircumferenceCm, c.lateHeadTracking ? 1 : 0, c.nearField ? 1 : 0, c.maxObjects,
        c.levelTrimDb, c.cpuBudgetPercent, c.excludeContextMask, c.logVoices ? 1 : 0);
  // Live changes the core takes directly (each ramps inside the next block).
  if (audr::Binding* b = g_binding.load()) {
    bool newSet = false;
    if (c.hrtf != g_hrtfRequested) {
      g_hrtfRequested = c.hrtf;
      std::string why;
      std::wstring used;
      binaural::HrtfSet* set = SetFor(c.hrtf, b->rate, &why, &used);
      if (set && used != g_hrtfName) {
        audr::CoreSetHrtf(b->core, set);
        g_curSet = set;
        newSet = true;
        g_hrtfName = used;
        Log("audio: HRTF %ls (crossfading)", LogSetName(used));
      }
    }
    // The head size is relative to the set's own measured head (binaural_config.h PilotItdRadiusM): set again for a new
    // circumference or a new set.
    // The near-field ear positions use the real head radius (C / 2 pi), never the set's fitted one.
    if ((first || newSet || prev.headCircumferenceCm != c.headCircumferenceCm) && g_curSet)
      audr::CoreSetHeadRadius(b->core, binaural_config::PilotItdRadiusM(audr::CoreDescribe(g_curSet).datasetHeadRadiusM, c.headCircumferenceCm));
    if (first || prev.headCircumferenceCm != c.headCircumferenceCm)
      audr::CoreSetEarRadius(b->core, binaural_config::HeadRadiusFromCircumferenceCm(c.headCircumferenceCm));
    if (first || prev.nearField != c.nearField) audr::CoreSetNearField(b->core, c.nearField);
  }
  if (!c.binaural && !first && prev.binaural) LogOff("binaural is off in the settings");
  if (c.binaural && !prev.binaural && !first) {
    RearmOverruns();
    LogOnAgain("re-enabled in the settings");
  }
}

// ---- control thread: the loop ----------------------------------------------------------------------------------------
uint64_t StatusBitsNow() {
  uint64_t s = 0;
  if (g_haveCfg && g_cfgRead.cfg.binaural) s |= qvstatus::kAudioConfigured;
  const bool off = g_hookState.load() < 0 || g_offReason.load() != kOffNone || !g_offText.empty();
  if (g_installed.load() && g_binding.load() && g_cnt.ownedNow.load() > 0 && !off && !g_userOff.load())
    s |= qvstatus::kAudioActive;
  if (off && g_haveCfg && g_cfgRead.cfg.binaural) s |= qvstatus::kAudioOff;
  return s;
}

void HandleEvents() {
  audr::Ev e;
  while (g_ev.Pop(&e)) {
    switch (e.code) {
      case audr::kEvBudgetDown:
        Log("audio: CPU budget: at most %u sounds now (%.1f%% of the last second's blocks over budget)", e.a, e.b / 10.0);
        break;
      case audr::kEvBudgetUp:
        Log("audio: CPU budget: at most %u sounds now (10 s under half the budget)", e.a);
        break;
      case audr::kEvOverrunsOff:
        LogOff("DCS reported 3 audio overruns within 10 s; binaural off for this DCS session");
        break;
      case audr::kEvPanLawOff: {
        int none = kOffNone;
        g_offReason.compare_exchange_strong(none, kOffPanLaw);
        char b[160];
        snprintf(b, sizeof(b), "pan law mismatch: %u of %u voices in 10 s; binaural off for this DCS session", e.a, e.b);
        LogOff(b);
        break;
      }
      default:
        break;
    }
  }
}

double P99Ms() {
  const uint32_t h = g_cnt.timeHead.load(std::memory_order_acquire);
  const uint32_t n = h < 1024 ? h : 1024;
  if (!n) return 0;
  std::vector<uint32_t> v(n);
  for (uint32_t i = 0; i < n; ++i) v[i] = g_cnt.timeRing[(h - 1 - i) & 1023].load(std::memory_order_relaxed);
  const size_t k = std::min<size_t>(n - 1, static_cast<size_t>(n * 0.99));
  std::nth_element(v.begin(), v.begin() + k, v.end());
  return v[k] / 10000.0;
}

void LogContexts() {
  if (!g_res.ctxArray || !g_res.ctxCount) return;
  static uint64_t lastKey[audv::off::kMaxContexts] = {};
  int n = 0;
  __try {
    n = *g_res.ctxCount;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return;
  }
  for (int i = 0; i < n && i < audv::off::kMaxContexts; ++i) {
    uint8_t is2d = 0, cg = 0;
    int32_t outIdx = 0;
    float thr = 0, ex = 0;
    __try {
      const uint8_t* c = g_res.ctxArray + audv::off::kCtxStride * i;
      is2d = audv::Rd<uint8_t>(c, audv::off::kCtxIs2D);
      cg = audv::Rd<uint8_t>(c, audv::off::kCtxCockpitGain);
      outIdx = audv::Rd<int32_t>(c, audv::off::kCtxOutput);
      thr = audv::Rd<float>(c, audv::off::kCtxCtThreshold);
      ex = audv::Rd<float>(c, audv::off::kCtxCtExponent);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
      return;
    }
    uint32_t tb, eb;
    memcpy(&tb, &thr, 4);
    memcpy(&eb, &ex, 4);
    const uint64_t key = 1ull << 63 | static_cast<uint64_t>(is2d) << 56 | static_cast<uint64_t>(cg) << 48 |
                         static_cast<uint64_t>(outIdx & 0xff) << 40 | (static_cast<uint64_t>(tb ^ eb) & 0xffffffffffull);
    if (key == lastKey[i]) continue;
    lastKey[i] = key;
    Log("audio: ctx %d 2d=%d cockpitGain=%d output=%d crosstalk=%.3f/%.3f", i, is2d, cg, outIdx, thr, ex);
  }
}

void LogListenerVsPose() {
  const uint32_t h = g_diag.head.load(std::memory_order_acquire);
  const uint32_t n = h < 256 ? h : 256;
  if (n < 64) return;
  double sl = 0, sh = 0;
  double best = -2;
  int bestLag = 0;
  for (int lag = -3; lag <= 3; ++lag) {
    double sxy = 0, sxx = 0, syy = 0;
    for (uint32_t i = 8; i + 8 < n; ++i) {
      const uint32_t a = (h - 1 - i) & 255, b = (h - 1 - i + lag) & 255;
      const double x = g_diag.listener[a], y = g_diag.headTurn[b];
      sxy += x * y, sxx += x * x, syy += y * y;
    }
    const double c = sxx > 0 && syy > 0 ? sxy / std::sqrt(sxx * syy) : 0;
    if (c > best) best = c, bestLag = lag;
  }
  for (uint32_t i = 0; i < n; ++i) {
    sl += g_diag.listener[(h - 1 - i) & 255];
    sh += g_diag.headTurn[(h - 1 - i) & 255];
  }
  Log("audio: listener-vs-pose: %u frames, ctx %d, listener turn %.2f deg/frame, head turn %.2f deg/frame, best match "
      "at %d frame(s) (correlation %.2f)",
      n, g_diagCtx.load(std::memory_order_relaxed), sl / n * 57.29578, sh / n * 57.29578, bestLag, best);
}

std::wstring g_voicesCsv;
void LogVoicesTick() {
  if (!g_cfgRead.cfg.logVoices || !g_installed.load()) return;
  if (g_voicesCsv.empty()) g_voicesCsv = g_dir + L"audio_voices.csv";
  WIN32_FILE_ATTRIBUTE_DATA fa{};
  const bool exists = GetFileAttributesExW(g_voicesCsv.c_str(), GetFileExInfoStandard, &fa) != 0;
  if (exists && (static_cast<uint64_t>(fa.nFileSizeHigh) << 32 | fa.nFileSizeLow) > (8ull << 20)) return;
  FILE* f = nullptr;
  if (_wfopen_s(&f, g_voicesCsv.c_str(), L"ab") || !f) return;
  if (!exists) fprintf(f, "time,slot,gen,ctx,owned,x,y,z,dist,m,gL,gR\r\n");
  SYSTEMTIME st;
  GetLocalTime(&st);
  for (int s = 0; s < audv::kSlots; ++s) {
    if (!g_meta.voice[s].load()) continue;
    audv::MetaData d;
    if (!audv::MetaRead(g_meta.slot[s], &d)) continue;
    fprintf(f, "%02d:%02d:%02d.%03d,%d,%u,%u,%d,%.3f,%.3f,%.3f,%.3f,%.6g,%.6g,%.6g\r\n", st.wHour, st.wMinute, st.wSecond,
            st.wMilliseconds, s, d.gen, d.ctx, (d.flags & audv::meta_flags::kOwned) ? 1 : 0, d.pos[0], d.pos[1], d.pos[2],
            d.dist, d.mTag, d.gL, d.gR);
  }
  fclose(f);
}

// Opens or drops the head pose block as the settings ask (never unmapped while the process runs).
dcsvr_headpose::SharedBlock g_poseMap;
void PoseTick() {
  const bool want = g_cfgRead.cfg.binaural && g_cfgRead.cfg.lateHeadTracking;
  if (!want) {
    g_pose.store(nullptr, std::memory_order_release);
    return;
  }
  if (!g_poseMap.get()) {
    const std::wstring sfx = perc_sfx::SuffixW();
    if (!g_poseMap.open(sfx.c_str())) return;
  }
  g_pose.store(g_poseMap.get(), std::memory_order_release);
}

struct Loop {
  ULONGLONG lastFlag = 0, lastStats = 0, lastCtx = 0, lastDiag = 0, lastVoices = 0, lastInstall = 0, lastCfg = 0, lastWorld = 0;
  bool keyWas = false;
  uint64_t statsHrtf = 0, statsPose = 0;
  // The "audio: idle" line's previous counts (blocks, armed blocks, pan calls, refusals by reason).
  uint64_t idleBlocks = 0, idleArmed = 0, idlePan = 0, idleRefused[audr::kRefuseCount] = {};
  std::string passText;  // the "audio: off" reason logged for blocks too long (empty: none)
};
Loop g_loop;

void ToggleUser(const char* by) {
  const bool off = !g_userOff.load();
  g_userOff.store(off);
  Log("audio: kill switch (%s): binaural %s", by, off ? "OFF (DCS's own panning)" : "ON");
  if (!off && RearmOverruns()) LogOnAgain("re-armed by the kill switch after DCS's overruns");
}

void ControlTick() {
  Loop& L = g_loop;
  const ULONGLONG now = GetTickCount64();
  if (now - L.lastCfg >= 250) {  // the ini every 250 ms (Live): two attribute reads, a parse only when it changed
    L.lastCfg = now;
    ReadConfig(false);
  }
  const binaural_config::AudioConfig& c = g_cfgRead.cfg;
  if (c.binaural && g_hookState.load() == 0 && now - L.lastInstall >= 2000) {
    L.lastInstall = now;
    // ed_sound is patched only when there is a set to render with: the generic one in this build, or the pilot's own.
    std::string why;
    if (!HrtfReady(c.hrtf, &why)) LogOff("no HRTF set: " + why);
    else if (Install()) LogContexts();
  }
  PoseTick();
  if (now - L.lastWorld >= 1000) {
    L.lastWorld = now;
    WorldTick();
  }
  if (void* rend = g_newRenderer.exchange(nullptr)) {
    // A renderer refused once is looked at again every 5 s (or after a settings change), not at every block. While the
    // bound renderer delivers blocks, another one is bound only when the world's 3D sounds play on it (it is then the
    // main output and the bound one the headphones / radio output), or to validate the bound one again.
    static ULONGLONG lastRejectedTry = 0;
    const bool retry = rend != g_rejectedRenderer || now - lastRejectedTry >= 5000;
    audr::Binding* cur = g_binding.load();
    const bool curLive = cur && !cur->retired.load() &&
                         static_cast<double>(Qpc() - cur->lastUseQpc.load()) <= kLiveBindingS * static_cast<double>(QpcFreq());
    const bool wanted = !curLive || rend == cur->renderer || rend == g_worldRenderer.load();
    if (g_installed.load() && c.binaural && retry && wanted && !g_handover.load()) {
      if (rend == g_rejectedRenderer) lastRejectedTry = now;
      const char* how = !cur ? "the first output with 3D sounds' blocks"
                        : rend == cur->renderer ? "the bound output, its layout changed"
                        : rend == g_worldRenderer.load() ? "the world's 3D sounds play on it"
                                                          : "the bound output stopped delivering blocks";
      if (curLive && rend != cur->renderer && AnySlotInUse() && RendererUsable(rend)) {
        // Another live output takes over: the voices the bound one renders binaurally fade back to DCS's panning there
        // first (the sim releases them), then the binding moves. Never more than kHandoverMaxMs.
        g_handoverTo = rend;
        g_handoverSince = now;
        g_handover.store(true);
        Log("audio: output %p takes over from %p (%s); fading its 3D sounds back first", rend, cur->renderer, how);
      } else {
        Bind(rend, how);
      }
    }
  }
  if (g_handover.load()) {
    audr::Binding* cur = g_binding.load();
    const bool curLive = cur && !cur->retired.load() &&
                         static_cast<double>(Qpc() - cur->lastUseQpc.load()) <= kLiveBindingS * static_cast<double>(QpcFreq());
    if (!AnySlotInUse() || !curLive || now - g_handoverSince >= kHandoverMaxMs) {
      if (g_installed.load() && c.binaural) Bind(g_handoverTo, "the world's 3D sounds play on it");
      g_handoverTo = nullptr;
      g_handover.store(false);  // after the binding moved (a new epoch): the sim owns voices of the new output only
    }
  }
  // The bound output's blocks pass through: too long for the core (said once), or a layout change (validated again).
  {
    const uint32_t pf = g_passFrames.load();
    if (pf > 1 && L.passText.empty()) {
      char b[160];
      snprintf(b, sizeof(b), "DCS's audio blocks are %u frames, more than binaural takes (%u); DCS's own panning", pf, audr::kMaxFrames);
      L.passText = b;
      LogOff(L.passText);
    } else if (pf == 0 && !L.passText.empty()) {
      if (g_offText == L.passText) LogOnAgain("DCS's audio blocks fit again");
      L.passText.clear();
    }
  }
  // Retired bindings: freed 2 s after their last block.
  for (size_t i = 0; i < g_retiredBindings.size();) {
    audr::Binding* b = g_retiredBindings[i];
    if (Qpc() - b->lastUseQpc.load() > 2 * QpcFreq()) {
      FreeBinding(b);
      g_retiredBindings.erase(g_retiredBindings.begin() + static_cast<ptrdiff_t>(i));
    } else {
      ++i;
    }
  }
  if (audr::Binding* b = g_binding.load())
    while (const binaural::HrtfSet* s = audr::CoreTakeRetired(b->core)) {
      for (size_t i = 0; i < g_sets.size(); ++i)
        if (g_sets[i].set == s) {
          audr::CoreReleaseHrtf(g_sets[i].set);
          g_sets.erase(g_sets.begin() + static_cast<ptrdiff_t>(i));
          break;
        }
    }
  HandleEvents();
  if (now - L.lastFlag >= 1000) {  // kill switch: toggle_audio.flag next to the payload or the dev ini
    L.lastFlag = now;
    for (const std::wstring& tf : {g_dir + binaural_config::kKillFlagFile,
                                   g_devDirAudio.empty() ? std::wstring() : g_devDirAudio + binaural_config::kKillFlagFile}) {
      if (!tf.empty() && GetFileAttributesW(tf.c_str()) != INVALID_FILE_ATTRIBUTES) {
        DeleteFileW(tf.c_str());
        ToggleUser("toggle_audio.flag");
      }
    }
  }
  if (g_cfgRead.toggleVk > 0) {
    const bool k = KeyDown(g_cfgRead.toggleVk) && ModifiersMatch(g_cfgRead.toggleMods);
    if (k && !L.keyWas) ToggleUser("[Hotkeys] AudioToggle");
    L.keyWas = k;
  }
  if (g_installed.load() && now - L.lastCtx >= 5000) {
    L.lastCtx = now;
    LogContexts();
  }
  if (g_installed.load() && now - L.lastStats >= 10000) {
    L.lastStats = now;
    const uint64_t hc = g_cnt.hrtfCalls.load();
    if (hc != L.statsHrtf) {
      audr::Binding* b = g_binding.load();
      const double budgetMs = b && b->rate ? 10.0 * g_cfgRead.cfg.cpuBudgetPercent / 100.0 : 0;
      // Head pose: "on" when blocks since the last line had the headset's pose (LateHeadTracking), "off" when it is
      // asked for and none came, "unused" when LateHeadTracking=0. The app shows it next to "Follow the head".
      const uint64_t pc = g_cnt.poseBlocks.load();
      const char* pose = !g_cfgRead.cfg.lateHeadTracking ? "unused" : pc != L.statsPose ? "on" : "off";
      L.statsPose = pc;
      // The sounds' make-up: mono waves, stereo ones (a point source at the stereo's loudness), and of those the ones
      // whose sdef asks for width (auto_width: DCS's own side around a binaural mid).
      const uint32_t owned = g_cnt.ownedNow.load(), st = std::min(owned, g_cnt.ownedStereoNow.load());
      Log("audio: running, %u sounds, p99 %.2f ms per block (budget %.0f%% of a block, about %.2f ms at 10 ms), limit "
          "%u; mono %u, stereo %u (wide %u); fades in/out %llu/%llu, fallbacks %llu, refused %llu, torn %llu (snapshots %llu), "
          "pan solves %llu (median %llu, mismatch "
          "%llu), overruns %llu; head pose %s",
          owned, P99Ms(), g_cfgRead.cfg.cpuBudgetPercent, budgetMs,
          std::min<uint32_t>(g_cnt.limitNow.load(), static_cast<uint32_t>(g_cfgRead.cfg.maxObjects)), owned - st, st,
          std::min(st, g_cnt.ownedWideNow.load()),
          static_cast<unsigned long long>(g_cnt.fadeIns.load()), static_cast<unsigned long long>(g_cnt.fadeOuts.load()),
          static_cast<unsigned long long>(g_cnt.repanCalls.load()), static_cast<unsigned long long>(g_cnt.coreRejected.load()),
          static_cast<unsigned long long>(g_meta.torn.load()), static_cast<unsigned long long>(g_cnt.tornSnaps.load()),
          static_cast<unsigned long long>(g_cnt.panSolves.load()),
          static_cast<unsigned long long>(g_cnt.panMedian.load()), static_cast<unsigned long long>(g_cnt.panMismatch.load()),
          static_cast<unsigned long long>(g_cnt.overruns.load()), pose);
      L.statsHrtf = hc;
    } else if (g_cfgRead.cfg.binaural && !g_userOff.load() && g_offReason.load() == kOffNone) {
      // Binaural on and nothing rendered in 3D for 10 s: what reached the module meanwhile, so a silent session can be
      // told apart (no blocks: not DCS's output; blocks but none armed: no binding; pan calls all refused: why).
      const uint64_t blocks = g_cnt.blocks.load(), armed = g_cnt.armedBlocks.load(), pan = g_cnt.panCalls.load();
      uint64_t refused[audr::kRefuseCount];
      for (int i = 0; i < audr::kRefuseCount; ++i) refused[i] = g_cnt.refused[i].load() - L.idleRefused[i];
      const auto u = [](uint64_t v) { return static_cast<unsigned long long>(v); };
      Log("audio: idle, blocks %llu (armed %llu), pan calls %llu; refused: off %llu, output %llu, not 3D %llu, offline %llu, "
          "channels %llu, context %llu, distance %llu, output not stereo %llu, gains %llu, pan law %llu, not at start "
          "%llu, limit %llu",
          u(blocks - L.idleBlocks), u(armed - L.idleArmed), u(pan - L.idlePan), u(refused[audr::kRefuseOff]),
          u(refused[audr::kRefuseOutput]), u(refused[audr::kRefuseNot3D]), u(refused[audr::kRefuseOffline]),
          u(refused[audr::kRefuseChannels]), u(refused[audr::kRefuseContext]), u(refused[audr::kRefuseDistance]),
          u(refused[audr::kRefuseStereoOut]), u(refused[audr::kRefuseGains]), u(refused[audr::kRefusePanMismatch]),
          u(refused[audr::kRefuseNotCreation]), u(refused[audr::kRefuseLimit]));
    }
    L.idleBlocks = g_cnt.blocks.load();
    L.idleArmed = g_cnt.armedBlocks.load();
    L.idlePan = g_cnt.panCalls.load();
    for (int i = 0; i < audr::kRefuseCount; ++i) L.idleRefused[i] = g_cnt.refused[i].load();
  }
  if (g_installed.load() && now - L.lastDiag >= 60000) {
    L.lastDiag = now;
    LogListenerVsPose();
  }
  if (now - L.lastVoices >= 1000) {
    L.lastVoices = now;
    LogVoicesTick();
  }
  audr::g_status.store(StatusBitsNow(), std::memory_order_release);
}

HANDLE g_thread = nullptr;
std::atomic<bool> g_quit{false};

DWORD WINAPI AudioThread(void*) {
  ReadConfig(true);
  if (!g_cfgRead.cfg.binaural) Log("audio: binaural off in the settings; ed_sound.dll untouched");
  while (!g_quit.load() && !qvexit::Exiting()) {
    ControlTick();
    Sleep(20);
  }
  return 0;
}

// The control thread exists only once [Audio] Binaural=1 was seen: with 3D audio off (the default, and every profile
// without it) the module starts no thread, polls nothing and logs nothing, as before binaural audio existed. Start
// looks once at payload start; the worker's ini reload (Live) calls StartIfWanted again when the ini changed.
SRWLOCK g_threadLock = SRWLOCK_INIT;
bool g_threadStarted = false;               // under g_threadLock: this payload's thread was started (or, in tests, counted)
std::atomic<int> g_testThreadStarts{0};     // tests (g_testNoThread): the threads StartIfWanted would have created

// [Audio] Binaural in the effective ini (the deployed one, or [Dev] IniPath's), read without a log line.
bool WantedInIni() {
  std::wstring devDir;
  const std::wstring ini = EffectiveIni(&devDir);
  return audcfg::ReadIni(ini.c_str()).cfg.binaural;
}

// Starts the control thread when the ini asks for binaural audio and this payload has not started it yet. True when it
// started it now. Never after Stop / Quiesce began (g_quit).
bool StartIfWanted() {
  if (!WantedInIni()) return false;
  AcquireSRWLockExclusive(&g_threadLock);
  const bool start = !g_threadStarted && !g_quit.load();
  if (start) {
    g_threadStarted = true;
    if (g_testNoThread.load())
      g_testThreadStarts.fetch_add(1);
    else
      g_thread = CreateThread(nullptr, 0, qvexit::Thread<AudioThread>, nullptr, 0, nullptr);
  }
  ReleaseSRWLockExclusive(&g_threadLock);
  return start;
}

void Start() {
  AcquireSRWLockExclusive(&g_threadLock);
  g_quit = false;
  g_threadStarted = false;
  ReleaseSRWLockExclusive(&g_threadLock);
  g_stopping = false;
  g_selfPid.store(GetCurrentProcessId());
  StartIfWanted();
}

bool JoinThread(DWORD ms) {
  AcquireSRWLockExclusive(&g_threadLock);
  g_quit = true;  // no StartIfWanted creates one from here on
  ReleaseSRWLockExclusive(&g_threadLock);
  if (g_thread) {
    if (WaitForSingleObject(g_thread, ms) == WAIT_OBJECT_0) {
      CloseHandle(g_thread);
      g_thread = nullptr;
    } else {
      Log("audio: control thread did not stop within %lu ms", ms);
      return false;
    }
  }
  return true;
}

// Payload stop (hot reload, Stop): fade back, restore the sites, stop the thread. Bindings are freed only when no
// wrapper call is in flight (an old payload stays mapped, so its code stays callable either way).
void Stop() {
  if (!JoinThread(3000)) {
    // Still inside a load, a Bind or Install (it may hold ED's mutex or be using g_sets): nothing here is touched or
    // freed (a leak, never a use after free). The sites keep calling this payload, which stays mapped; the next
    // payload's Resolve accepts these thunks and takes the sites over.
    audr::g_status.store(0);
    return;
  }
  if (g_installed.load()) Uninstall(400);
  if (g_testAfterRestore) g_testAfterRestore();
  // A wrapper that passed a call site just before it was restored counts itself in g_inflight only once inside it: the
  // memory is freed after a grace period with nothing in flight (checked again after it), else left in place (a leak,
  // never a use after free).
  bool quiet = !g_installed.load();
  if (quiet && (g_binding.load() || !g_retiredBindings.empty() || !g_sets.empty())) {
    quiet = false;
    for (int round = 0; round < 3 && !quiet; ++round) {
      const ULONGLONG until = GetTickCount64() + 1000;
      while (g_inflight.load() > 0 && GetTickCount64() < until) Sleep(1);
      Sleep(static_cast<DWORD>(g_stopGraceMs));
      quiet = g_inflight.load() == 0;
    }
    if (!quiet) Log("audio: a call was still running at stop; the binaural renderer's memory is left in place");
  }
  if (quiet) {
    if (audr::Binding* b = g_binding.exchange(nullptr)) FreeBinding(b);
    for (audr::Binding* b : g_retiredBindings) FreeBinding(b);
    g_retiredBindings.clear();
    for (LoadedSet& s : g_sets) audr::CoreReleaseHrtf(s.set);
    g_sets.clear();
  }
  audr::g_status.store(0);
}

// DCS is shutting down: the thread stops; hooks and bindings stay (DCS may still call them while it tears down).
void Quiesce(DWORD ms) { JoinThread(ms); }

}  // namespace audio
