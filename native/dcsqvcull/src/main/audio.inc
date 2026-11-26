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

// Cockpit space (docs/COCKPIT_SPACE.md): the sim's SpaceState (audio_space.h), the control thread's settings for the
// render side's room (audio_render.h SpaceCtl), and what the Lua bridge reports (loader exports DcsQvCull_Audio*: the
// Lua thread writes, the control thread reads).
audspace::SpaceSlot g_space;
audr::SpaceCtlSlot g_spaceCtl;
audr::SpaceCtl g_spaceCtlNow;               // control thread: the last published copy
std::atomic<bool> g_luaWanted{false};       // DcsQvCull_AudioWanted: binaural and the space on (control thread sets)
SRWLOCK g_luaLock = SRWLOCK_INIT;
char g_luaType[64] = {};                    // under g_luaLock: the player's unit type ("" unknown)
std::atomic<float> g_luaCanopy{-1.0f};      // 0..1, -1 unknown
std::atomic<int> g_luaCanopySrc{0};         // 1 LoGetMechInfo, 2 draw argument 38, 0 none
std::atomic<int> g_luaInside{-1};           // the camera check: 1 cockpit, 0 external, -1 unknown
std::atomic<uint64_t> g_luaCalls{0};        // bridge calls received (tests)
std::atomic<uint64_t> g_luaStateGen{0};     // DcsQvCull_AudioState calls (a fresh camera check)

void LuaAircraft(const char* type) {
  g_luaCalls.fetch_add(1, std::memory_order_relaxed);
  char t[64] = {};
  if (type) {
    // A unit type name: printable ASCII only (it reaches the log and the app's status line).
    size_t n = 0;
    for (; type[n] && n < sizeof(t) - 1; ++n) t[n] = type[n] >= 0x20 && type[n] < 0x7f ? type[n] : '?';
  }
  AcquireSRWLockExclusive(&g_luaLock);
  memcpy(g_luaType, t, sizeof(t));
  ReleaseSRWLockExclusive(&g_luaLock);
}
void LuaState(double canopy, int src, int inside) {
  g_luaCalls.fetch_add(1, std::memory_order_relaxed);
  const bool known = std::isfinite(canopy) && canopy >= 0.0;
  g_luaCanopy.store(known ? static_cast<float>(canopy > 1.0 ? 1.0 : canopy) : -1.0f, std::memory_order_relaxed);
  g_luaCanopySrc.store(known ? (src == 2 ? 2 : 1) : 0, std::memory_order_relaxed);
  g_luaInside.store(inside > 0 ? 1 : (inside == 0 ? 0 : -1), std::memory_order_relaxed);
  g_luaStateGen.fetch_add(1, std::memory_order_release);
}
std::string LuaType() {
  AcquireSRWLockShared(&g_luaLock);
  std::string s(g_luaType);
  ReleaseSRWLockShared(&g_luaLock);
  return s;
}

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
  int spaceLevel = 0;  // CockpitSpace (the sim reads ed_sound's space fields only when > 0)
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
// Tests: the room renders without its level match (the direct sound added as it is), for the e2e scenarios that
// separate the direct sound from the room by d alone (DRR, IACC).
std::atomic<bool> g_testRoomNoMatch{false};
// The same for a live bisection: [Dev] AudioRoomNoMatch=1 in the (dev) ini (read with the config, Live).
std::atomic<bool> g_devRoomNoMatch{false};
void RoomFlush(audr::Block& blk, float* out, uint32_t frames, bool inMix);
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
  // Cockpit space: ed_sound's space fields are read only while it is wanted and their code matched (audio_hooks.h).
  g_sim.space.gainReads = on && rt.spaceLevel > 0 && g_res.spaceGain;
  g_sim.space.orientReads = on && rt.spaceLevel > 0 && g_res.spaceOrient;
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
// Cockpit space: the room's buses and tail, and the owned voices' direct sound (blk.dry), into the block. ed_sound's mix
// renders the voices of renderer+0x20 (a list, voice+0 the next one) and THEN runs its master processing over `out`
// (the limiter and the global gain ramp, docs/BINAURAL_AUDIO_HOOKS.md 2.3). So this runs inside the mix, like every
// other voice's output, at the render of the list's last node (found by walking the list at the block's first call:
// new voices may come in anywhere). When the last node is not rendered (ed_sound passes voices that do not play, as
// during a mission load), at the voice that was rendered last in the previous block, if no node came in after it.
// A voice rendered after the flush plays its direct sound straight into `out`, without the room (counted: late calls).
// The dry path is used only after a block that flushed inside the mix (so never in a binding's first block); a block
// that did not: direct + room go in after the mix at the master gain measured at the flushes (`out` after the mix over
// `out` at the flush, every block without a late call), and the room alone (nothing in `dry`) the same way, or
// dropped while that gain is not known yet.
// (Run after the mix, the room and the direct sound skipped DCS's master gain: +9 dB on the F-16 in the second flight;
// in the first flight the room alone came out that much too loud against the direct sound.)
// The voice is the last of the mix's list (its next pointer, voice+0, is null).
inline bool LastVoice(void* voice) { return *static_cast<void* const*>(voice) == nullptr; }
// The list from renderer+0x20: its last node, and how many nodes follow `after` (kNotInList when it is not in it).
// Bounded (4096 nodes) and fault-safe; false when the walk failed.
constexpr uint32_t kNotInList = 0xffffffffu;
bool WalkList(void* renderer, const void* after, void** tail, uint32_t* following) {
  __try {
    void* v = *reinterpret_cast<void* const*>(static_cast<uint8_t*>(renderer) + 0x20);
    void* last = nullptr;
    uint32_t n = 0, at = kNotInList;
    for (; v && n < 4096; ++n) {
      if (v == after) at = n;
      last = v;
      v = *static_cast<void* const*>(v);
    }
    if (v) return false;  // longer than any list ed_sound keeps: not trusted
    *tail = last;
    *following = at == kNotInList ? kNotInList : n - 1 - at;
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}
inline void AddDouble(std::atomic<double>& a, double v) {  // the render thread adds, the running line exchanges
  double cur = a.load(std::memory_order_relaxed);
  while (!a.compare_exchange_weak(cur, cur + v, std::memory_order_relaxed)) {
  }
}
void RoomFlush(audr::Block& blk, float* out, uint32_t frames, bool inMix) {
  if (!blk.roomOn || blk.roomFlushed) return;
  blk.roomFlushed = true;
  blk.flushedInMix = inMix;
  audr::Binding* b = blk.b;
  const int64_t r0 = Qpc();
  {
    audr::MxcsrScope fz;
    // The energies (a diagnosis on the running line) every 8th room block: they cost three passes over the block.
    const bool measure = (g_cnt.roomBlocks.load(std::memory_order_relaxed) & 7u) == 0;
    double eDry = 0, eBefore = 0, eAfter = 0;
    if (measure)
      for (uint32_t i = 0; i < 2 * frames; ++i) {
        eBefore += double(out[i]) * out[i];
        if (blk.dry) eDry += double(blk.dry[i]) * blk.dry[i];
      }
    if (blk.dry && (g_testRoomNoMatch.load(std::memory_order_relaxed) || g_devRoomNoMatch.load(std::memory_order_relaxed))) {
      for (uint32_t i = 0; i < 2 * frames; ++i) out[i] += blk.dry[i];
      audr::RoomRender(b->room, out, frames);
    } else {
      audr::RoomRender(b->room, out, frames, blk.dry);
    }
    audr::RoomEnd(b->room);
    // `out` here: what the master stage gets, if nothing is rendered after (the master gain's measure).
    double e = 0;
    for (uint32_t i = 0; i < 2 * frames; ++i) e += double(out[i]) * out[i];
    blk.ePreMaster = e;
    if (measure) {
      eAfter = e;
      AddDouble(g_cnt.roomEDry, eDry), AddDouble(g_cnt.roomEOutBefore, eBefore), AddDouble(g_cnt.roomEOutAfter, eAfter);
      g_cnt.roomEBlocks.fetch_add(1, std::memory_order_relaxed);
    }
  }
  blk.roomRenderTicks += Qpc() - r0;
  blk.dry = nullptr;  // voices rendered after this point (inside the mix) go straight into `out`
  g_cnt.roomRenders.fetch_add(1, std::memory_order_relaxed);
  (inMix ? g_cnt.roomFlushInMix : g_cnt.roomFlushAfterMix).fetch_add(1, std::memory_order_relaxed);
}
// After a voice-render call inside the mix: count it and flush the room when this is the block's last call (see above).
inline void AfterVoiceCall(audr::Block& blk, void* voice, float* out, uint32_t frames) {
  if (blk.calls == 0 && blk.roomOn)
    blk.walked = WalkList(blk.b->renderer, blk.b->lastCaller, &blk.tail, &blk.prevLastFollowing);
  ++blk.calls;
  blk.lastCaller = voice;
  if (blk.roomFlushed) {
    ++blk.callsAfterFlush;
    if (blk.roomOn) g_cnt.roomLateCalls.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  if (!blk.roomOn) return;
  const audr::Binding& b = *blk.b;
  const bool atTail = blk.walked ? voice == blk.tail : LastVoice(voice);
  // The list's last node not rendered in the last block either: the voice rendered last then, unless a node came in
  // after it (it would be rendered after the flush).
  const bool fallback = !atTail && !b.prevFlushAtTail && b.lastCaller == voice && blk.walked &&
                        b.lastFollowing != kNotInList && blk.prevLastFollowing == b.lastFollowing;
  if (atTail || fallback) {
    RoomFlush(blk, out, frames, true);
    blk.flushAtTail = atTail;
  }
}

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
  if (slot < 0) {  // not a candidate: DCS's own call, untouched
    const int r = g_res.voiceRender(voice, out, frames);
    AfterVoiceCall(*blk, voice, out, frames);
    return r;
  }
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
    AfterVoiceCall(*blk, voice, out, frames);
    return r;
  }
  {
    audr::MxcsrScope fz;
    audr::ProcessVoice(*blk, g_cnt, slot, voice, scratch, snap, ok, md, r == 0, g_meta.ackGen);
  }
  g_cnt.candidateCalls.fetch_add(1, std::memory_order_relaxed);
  blk->dspTicks += (t1 - t0) + (Qpc() - t2);
  AfterVoiceCall(*blk, voice, out, frames);
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
  // Cockpit space: the room's block-end targets and its BeginBlock (skipped, no call at all, while it is off and idle).
  int64_t roomTicks = 0;
  if (b->room) {
    audspace::SpaceData sd;
    const bool haveSd = audspace::SpaceRead(g_space, &sd);
    static thread_local audr::SpaceCtl ctl;  // this thread's last good copy (a busy writer keeps it)
    audr::ReadCtl(g_spaceCtl, &ctl);
    const int64_t r0 = Qpc();
    {
      audr::MxcsrScope fz;
      audr::RoomBlockBegin(blk, haveSd ? &sd : nullptr, ctl, now, freq);
    }
    roomTicks += Qpc() - r0;
  }
  t_block = &blk;
  g_res.mix(rend, out, frames);
  t_block = nullptr;
  if (blk.coreBegun) {
    audr::MxcsrScope fz;
    audr::CoreEnd(b->core);
  }
  if (blk.roomOn) {
    // Normally done inside the mix (AfterVoiceCall). Not flushed: direct + room (or the room alone) of this block go in
    // after the mix at the master gain measured at the flushes; the room alone is dropped while that is not known.
    if (!blk.roomFlushed) {
      float* hold = b->scratch;
      memset(hold, 0, sizeof(float) * 2 * frames);
      const bool dry = blk.dryUsed && blk.dry;
      RoomFlush(blk, hold, frames, false);
      if (b->masterKnown || dry) {  // the dry path runs only after a flush inside the mix: the gain is known then
        const float g = b->masterGain;
        for (uint32_t i = 0; i < 2 * frames; ++i) out[i] += g * hold[i];
        if (b->wet > 0.0f) g_cnt.roomAfterWet.fetch_add(1, std::memory_order_relaxed);
      } else {
        g_cnt.roomDropped.fetch_add(1, std::memory_order_relaxed);
      }
    } else if (blk.flushedInMix && blk.callsAfterFlush == 0 && blk.ePreMaster > 1e-12) {
      // ed_sound's master gain: `out` now over `out` at the flush (nothing rendered in between). The first measure as
      // it is, then smoothed over a few blocks (DCS's gain ramps).
      double e = 0;
      for (uint32_t i = 0; i < 2 * frames; ++i) e += double(out[i]) * out[i];
      const float g = static_cast<float>(std::sqrt(e / blk.ePreMaster));
      if (std::isfinite(g) && g > 0.01f && g < 4.0f) {
        b->masterGain = b->masterKnown ? b->masterGain + 0.3f * (g - b->masterGain) : g;
        b->masterKnown = true;
        g_cnt.masterGainDb.store(20.0f * std::log10(b->masterGain), std::memory_order_relaxed);
      }
    }
    roomTicks += blk.roomRenderTicks;
    blk.dspTicks += roomTicks;
  }
  b->prevFlushInMix = blk.roomOn && blk.roomFlushed && blk.flushedInMix;
  b->prevFlushAtTail = blk.flushAtTail;
  b->lastCalls = blk.calls;
  b->lastCaller = blk.lastCaller;
  {
    void* tail = nullptr;
    uint32_t following = kNotInList;
    b->lastFollowing = blk.roomOn && blk.lastCaller && WalkList(rend, blk.lastCaller, &tail, &following) ? following : kNotInList;
  }
  if (b->room) audr::RoomBlockEnd(blk, g_cnt, roomTicks, freq);
  b->lastUseQpc.store(now, std::memory_order_relaxed);
  g_cnt.armedBlocks.fetch_add(1, std::memory_order_relaxed);
  g_cnt.activeLastBlock.store(blk.hrtfVoices, std::memory_order_relaxed);
  g_cnt.ownedWideNow.store(blk.wideVoices, std::memory_order_relaxed);
  audr::BudgetTick(*b, g_cnt, g_ev, {rt.cpuBudget, rt.maxObjects}, frames, blk.dspTicks, now, freq,
                   blk.roomOn && blk.roomEarly && blk.roomFrameValid);
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
  g_sim.spaceOut = &g_space;
  g_sim.Reset();
  audspace::SpaceWrite(g_space, audspace::SpaceData{});
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
  if (!r.spaceGain || !r.spaceOrient)
    Log("audio: cockpit space: ed_sound's cockpit fields not found (%s); %s", r.spaceWhy,
        !r.spaceGain && !r.spaceOrient ? "inside from the camera, no through-canopy class, no aircraft frame"
        : !r.spaceGain                 ? "inside from the camera, no through-canopy class"
                                       : "no aircraft frame (late reflections only)");
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
std::vector<const binaural::HrtfSet*> g_retiredByCore, g_retiredByRoom;  // sets given back, waiting for the other one
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
  b->given.push_back(set);
  b->renderer = rend;
  b->rate = static_cast<uint32_t>(rate);
  b->core = core;
  // One allocation: the voice's two channels (2 x kMaxFrames), the onset buffer and the delayed side's input
  // (kMaxFrames each), then every slot's side history (kSlots x kSideHist; each slot zeroes its own before its first use).
  b->scratch = static_cast<float*>(
      _aligned_malloc(sizeof(float) * (6 * audr::kMaxFrames + static_cast<size_t>(audv::kSlots) * audr::kSideHist), 64));
  b->onset = b->scratch ? b->scratch + 2 * audr::kMaxFrames : nullptr;
  b->side = b->scratch ? b->scratch + 3 * audr::kMaxFrames : nullptr;
  b->sideHist = b->scratch ? b->scratch + 4 * audr::kMaxFrames : nullptr;
  // Cockpit space: the block's direct sound of the owned voices (2 x kMaxFrames), handed to the room's level match.
  b->dry = b->scratch ? b->scratch + 4 * audr::kMaxFrames + static_cast<size_t>(audv::kSlots) * audr::kSideHist : nullptr;
  b->overrun = reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(rend) + audv::off::kROverrun);
  if (!b->scratch) {
    audr::CoreDelete(core);
    delete b;
    LogOff("no memory for the binaural renderer");
    return;
  }
  // Cockpit space: the room renders with the voices' set and head; it exists only in builds with the core's Room.
  if (audr::kHaveRoom) {
    binaural::Room* room = audr::RoomNew();
    char rerr[160] = {};
    if (room && audr::RoomInit(room, b->rate, binaural::kMaxSources, rerr, sizeof(rerr))) {
      audr::RoomSetHrtf(room, set);
      audr::RoomSetHeadRadius(room, cc.headRadiusM);
      if (g_spaceCtlNow.haveParams) audr::RoomSetParams(room, g_spaceCtlNow.params);
      b->room = room;
    } else {
      audr::RoomDelete(room);
      Log("audio: cockpit space: off (the room did not start: %s)", rerr[0] ? rerr : "no memory");
    }
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
  audr::RoomDelete(b->room);
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
  r.spaceLevel = c.binaural ? g_cfgRead.space.Effective() : 0;
  WriteRt(r);
  g_luaWanted.store(c.binaural && g_cfgRead.space.Effective() > 0, std::memory_order_relaxed);
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
  {
    const bool noMatch = GetPrivateProfileIntW(L"Dev", L"AudioRoomNoMatch", 0, ini.c_str()) != 0;
    if (noMatch != g_devRoomNoMatch.load()) {
      g_devRoomNoMatch.store(noMatch);
      Log("audio: cockpit space: level match %s ([Dev] AudioRoomNoMatch=%d)", noMatch ? "off" : "on", noMatch ? 1 : 0);
    }
  }
  audcfg::Read r = audcfg::ReadIni(ini.c_str());
  for (int i = 0; i < r.noteCount; ++i) Log("%s", r.notes[i]);
  const bool first = !g_haveCfg;
  const binaural_config::AudioConfig prev = g_cfgRead.cfg;
  const audcfg::SpaceConfig prevSpace = g_cfgRead.space;
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
  if (first || !audcfg::SameSpace(prevSpace, r.space))
    Log("audio: config: CockpitSpace=%d CockpitSpaceFamily=%ls CockpitSpaceCanopy=%d%s", r.space.level,
        r.space.familyAuto ? L"auto" : cockpit_space::FamilyKey(r.space.family), r.space.canopy ? 1 : 0,
        r.space.Blind() ? "; listening test level in use (not named)" : "");
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
        if (std::find(b->given.begin(), b->given.end(), set) == b->given.end()) b->given.push_back(set);
        if (b->room) {  // the room's buses use the voices' set (retired by both; an idle room is shown it once)
          audr::RoomSetHrtf(b->room, set);
          b->roomSetGen.fetch_add(1, std::memory_order_release);
        }
        g_curSet = set;
        newSet = true;
        g_hrtfName = used;
        Log("audio: HRTF %ls (crossfading)", LogSetName(used));
      }
    }
    // The head size is relative to the set's own measured head (binaural_config.h PilotItdRadiusM): set again for a new
    // circumference or a new set.
    // The near-field ear positions use the real head radius (C / 2 pi), never the set's fitted one.
    if ((first || newSet || prev.headCircumferenceCm != c.headCircumferenceCm) && g_curSet) {
      const float itd = binaural_config::PilotItdRadiusM(audr::CoreDescribe(g_curSet).datasetHeadRadiusM, c.headCircumferenceCm);
      audr::CoreSetHeadRadius(b->core, itd);
      if (b->room) audr::RoomSetHeadRadius(b->room, itd);
    }
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
      case audr::kEvEarlyOff:
        Log("audio: cockpit space: early reflections off (CPU budget)");
        break;
      case audr::kEvEarlyOn:
        Log("audio: cockpit space: early reflections on again");
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
  // flags: meta_flags (audio_voice.h), added: 1 owned, 2 owned from its creation, 4 head pose valid, 8 near (under
  // 1.4 m, until past 1.6 m), 16 head position valid, 32 a stereo wave, 64 auto_width (informational).
  // Then the render side's view of that voice in its last block (Counters::VoiceView; empty when we did not render it):
  // comp the loudness factor, diffuse 0/1, wide 0/1 (DCS's side kept: near or diffuse), panLevel DCS's level for the
  // direction, power the smoothed per-frame power of m L', m R' (the point / wide energy share: sum power by wide).
  // Cockpit space (after power): room 1 when sent to the room in its last block, send_db its send gain, through 1 heard
  // through the canopy, cgain DCS's canopy damping when applied (empty otherwise), ax..az its aircraft-frame position,
  // dcomp_db the direct gain d. Empty when not sent.
  if (!exists)
    fprintf(f, "time,slot,gen,ctx,owned,x,y,z,dist,m,gL,gR,flags,comp,diffuse,wide,panLevel,power,room,send_db,through,"
               "cgain,ax,ay,az,dcomp_db\r\n");
  SYSTEMTIME st;
  GetLocalTime(&st);
  for (int s = 0; s < audv::kSlots; ++s) {
    if (!g_meta.voice[s].load()) continue;
    audv::MetaData d;
    if (!audv::MetaRead(g_meta.slot[s], &d)) continue;
    fprintf(f, "%02d:%02d:%02d.%03d,%d,%u,%u,%d,%.3f,%.3f,%.3f,%.3f,%.6g,%.6g,%.6g,%u", st.wHour, st.wMinute, st.wSecond,
            st.wMilliseconds, s, d.gen, d.ctx, (d.flags & audv::meta_flags::kOwned) ? 1 : 0, d.pos[0], d.pos[1], d.pos[2],
            d.dist, d.mTag, d.gL, d.gR, d.flags);
    const audr::Counters::VoiceView& vv = g_cnt.view[s];
    const uint32_t vs = vv.state.load(std::memory_order_relaxed);
    const bool viewed = (vs & audr::Counters::kViewTagged) && vv.gen.load(std::memory_order_relaxed) == d.gen;
    if (viewed)
      fprintf(f, ",%.4f,%d,%d,%.4f,%.4g", vv.comp.load(std::memory_order_relaxed),
              (vs & audr::Counters::kViewDiffuse) ? 1 : 0, (vs & audr::Counters::kViewWide) ? 1 : 0,
              vv.panLevel.load(std::memory_order_relaxed), vv.power.load(std::memory_order_relaxed));
    else
      fprintf(f, ",,,,,");
    const bool through = (d.flags & audv::meta_flags::kThroughCanopy) != 0;
    char cg[24] = "";
    if (through) snprintf(cg, sizeof(cg), "%.4g", d.cockpitGain);
    if (viewed && vv.room.load(std::memory_order_relaxed))
      fprintf(f, ",1,%.2f,%d,%s,%.3f,%.3f,%.3f,%.2f\r\n", vv.sendDb.load(std::memory_order_relaxed), through ? 1 : 0, cg,
              vv.ax.load(std::memory_order_relaxed), vv.ay.load(std::memory_order_relaxed),
              vv.az.load(std::memory_order_relaxed), vv.dcompDb.load(std::memory_order_relaxed));
    else
      fprintf(f, ",0,,%d,%s,,,,\r\n", through ? 1 : 0, cg);
  }
  fclose(f);
}

// ---- control thread: cockpit space ----------------------------------------------------------------------------------
const char* SpaceLevelName(int l) { return l == 1 ? "Subtle" : l == 2 ? "Normal" : l == 3 ? "Strong" : "Off"; }

// The room's own time per block: p50 / p99 of the last 1024 room blocks, microseconds.
void RoomTimes(double* p50Us, double* p99Us) {
  *p50Us = *p99Us = 0;
  const uint32_t h = g_cnt.roomHead.load(std::memory_order_acquire);
  const uint32_t n = h < 1024 ? h : 1024;
  if (!n) return;
  std::vector<uint32_t> v(n);
  for (uint32_t i = 0; i < n; ++i) v[i] = g_cnt.roomRing[(h - 1 - i) & 1023].load(std::memory_order_relaxed);
  std::sort(v.begin(), v.end());
  *p50Us = v[n / 2] / 10.0;
  *p99Us = v[std::min<size_t>(n - 1, static_cast<size_t>(n * 0.99))] / 10.0;
}

struct SpaceState {
  bool havePub = false;
  int level = 0;
  bool blind = false;           // a listening test level: the level is never named
  cockpit_space::Family family = cockpit_space::Family::Generic;
  bool familyAuto = true;
  std::string type;
  // log line state
  bool logged = false;
  int inside = -2, canopySrc = -2;
  float canopy = -2;
  int frameSrc = -1, cockpitCtx = -1;
  uint32_t unreliable = 0;
  ULONGLONG unreliableAt = 0;
  int probeCtx = -1;
  float probe[3] = {};
  bool stubLogged = false;
  int insideNow = 1;           // what the render side uses (for the running line and audio_space.csv)
  int sigDcs = -2, sigCam = -2;  // the two inside signals last logged (ed_sound's flag, the camera check; -1 unknown)
  ULONGLONG lastCsv = 0;
  // The space switched on: the camera check the bridge reported before is stale (the bridge is quiet while the space is
  // off), so the space stays out until a fresh one arrives (g_luaHoldMs at most: a payload without the bridge).
  bool wanted = false;
  uint64_t holdGen = 0;
  ULONGLONG holdUntil = 0;
};
std::atomic<uint32_t> g_luaHoldMs{1500};  // tests set 0 (audio_rig.h RigUp); the hold test sets it back
SpaceState g_spaceSt;

// Every 250 ms: the family (setting, else the aircraft's type, else generic), the preset for the room, the canopy and
// the camera check for the render side; the log lines of docs/COCKPIT_SPACE.md 8.5.
void SpaceTick() {
  SpaceState& S = g_spaceSt;
  const audcfg::SpaceConfig& sc = g_cfgRead.space;
  const int level = g_cfgRead.cfg.binaural ? sc.Effective() : 0;
  const std::string type = LuaType();
  const cockpit_space::Family fam = sc.familyAuto ? cockpit_space::FamilyForType(type.c_str()) : sc.family;
  audr::SpaceCtl& ctl = g_spaceCtlNow;
  const bool changed = !S.havePub || level != S.level || sc.Blind() != S.blind || fam != S.family ||
                       sc.familyAuto != S.familyAuto || type != S.type;
  if (changed) {
    // A new preset for every room (crossfaded inside the room); level 0 publishes none: the room fades out and idles.
    if (level > 0) {
      binaural::RoomParams p{};
      if (cockpit_space::Preset(fam, level, &p)) {
        ctl.params = p;
        ctl.haveParams = true;
        if (audr::Binding* b = g_binding.load())
          if (b->room) audr::RoomSetParams(b->room, p);
      }
    }
    ctl.level = level;
    // A listening test's level is never named (the pilot rates blind; the app shows what the log says).
    Log("audio: cockpit space: %s (%ls %s, aircraft %s)", sc.Blind() ? "Test" : SpaceLevelName(level),
        cockpit_space::FamilyKey(fam), sc.familyAuto ? "auto" : "setting", type.empty() ? "unknown" : type.c_str());
    if (level > 0 && !audr::kHaveRoom && !S.stubLogged) {
      S.stubLogged = true;
      Log("audio: cockpit space: this build has no room renderer yet; the sound is unchanged");
    }
    S.havePub = true;
    S.level = level, S.blind = sc.Blind(), S.family = fam, S.familyAuto = sc.familyAuto, S.type = type;
  }
  const float luaCanopy = g_luaCanopy.load(std::memory_order_relaxed);
  ctl.canopy = sc.canopy ? luaCanopy : -1.0f;
  {
    const bool wantedNow = g_luaWanted.load(std::memory_order_relaxed);
    const uint64_t gen = g_luaStateGen.load(std::memory_order_acquire);
    if (wantedNow && !S.wanted) S.holdGen = gen, S.holdUntil = GetTickCount64() + g_luaHoldMs.load(std::memory_order_relaxed);
    S.wanted = wantedNow;
    const bool hold = wantedNow && gen == S.holdGen && GetTickCount64() < S.holdUntil;
    ctl.luaInside = hold ? 0 : g_luaInside.load(std::memory_order_relaxed);  // held: as outside (no wet)
  }
  audr::WriteCtl(g_spaceCtl, ctl);
  if (!g_installed.load() || (level == 0 && !S.blind)) return;  // a test's Off keeps its lines (blind)
  // inside / canopy (on change; canopy steps of 0.1)
  audspace::SpaceData sd{};
  const bool haveSd = audspace::SpaceRead(g_space, &sd);
  const bool reads = haveSd && (sd.flags & audspace::flags::kReads);
  const int inside = audspace::Inside(haveSd ? &sd : nullptr, ctl.luaInside) ? 1 : 0;
  S.insideNow = inside;
  // Both signals, on change: ed_sound's flag (once seen) and the camera check (until the flag is verified live, an
  // outside camera wins; docs/COCKPIT_SPACE.md 10.1).
  const int sigDcs = reads && (sd.flags & audspace::flags::kInsideKnown) ? ((sd.flags & audspace::flags::kInside) ? 1 : 0) : -1;
  if (sigDcs != S.sigDcs || ctl.luaInside != S.sigCam) {
    auto name = [](int v) { return v < 0 ? "unknown" : v ? "inside" : "outside"; };
    Log("audio: cockpit space: inside signals: ed_sound %s, camera %s", name(sigDcs), name(ctl.luaInside));
    S.sigDcs = sigDcs, S.sigCam = ctl.luaInside;
  }
  const int csrc = ctl.canopy >= 0 ? g_luaCanopySrc.load(std::memory_order_relaxed) : 0;
  if (inside != S.inside || csrc != S.canopySrc || (ctl.canopy >= 0) != (S.canopy >= 0) ||
      (ctl.canopy >= 0 && std::fabs(ctl.canopy - S.canopy) >= 0.1f)) {
    if (ctl.canopy >= 0)
      Log("audio: cockpit space: inside %d, canopy %.2f (%s)", inside, ctl.canopy, csrc == 2 ? "arg38" : "mech");
    else
      Log("audio: cockpit space: inside %d, canopy unknown (none)", inside);
    S.inside = inside, S.canopySrc = csrc, S.canopy = ctl.canopy;
  }
  // the aircraft frame (on change; once a minute while frames are refused)
  if (haveSd) {
    const LONGLONG ageMs = sd.frameQpc ? (Qpc() - sd.frameQpc) * 1000 / QpcFreq() : -1;
    const int src = (sd.flags & audspace::flags::kFrameValid) && ageMs >= 0 && ageMs < 1000 ? 1 : 0;
    const ULONGLONG now = GetTickCount64();
    const bool refusing = sd.unreliable != S.unreliable && now - S.unreliableAt >= 60000;
    if (src != S.frameSrc || sd.cockpitCtx != S.cockpitCtx || refusing) {
      Log("audio: cockpit space: frame from %s, cockpit ctx %d, spread %.2f deg, unreliable frames %u, outlier sounds %u",
          src ? "sound" : "none", sd.cockpitCtx, sd.spreadDeg, sd.unreliable, sd.outliers);
      S.frameSrc = src, S.cockpitCtx = sd.cockpitCtx;
      if (refusing || sd.unreliable != S.unreliable) S.unreliableAt = now;
      S.unreliable = sd.unreliable;
    }
    // The listener-parameter probe (docs/COCKPIT_SPACE.md 3.1): at enable and on any change over 1 %.
    if (reads && sd.worldCtx < 8) {
      const float v[3] = {sd.lgGain, sd.lgPitch, sd.lgLowpass};
      bool moved = S.probeCtx != sd.worldCtx;
      for (int k = 0; k < 3 && !moved; ++k)
        moved = std::fabs(v[k] - S.probe[k]) > 0.01f * std::fmax(std::fabs(S.probe[k]), 1e-6f);
      if (moved) {
        Log("audio: ctx %d listener gain=%.4g pitch=%.4g lowpass=%.4g", sd.worldCtx, v[0], v[1], v[2]);
        S.probeCtx = sd.worldCtx;
        memcpy(S.probe, v, sizeof(v));
      }
    }
  }
}

// audio_space.csv (LogVoices): one row a second while the space is on.
std::wstring g_spaceCsv;
void SpaceCsvTick() {
  if (!g_cfgRead.cfg.logVoices || !g_installed.load() || g_spaceSt.level == 0) return;
  if (g_spaceCsv.empty()) g_spaceCsv = g_dir + L"audio_space.csv";
  WIN32_FILE_ATTRIBUTE_DATA fa{};
  const bool exists = GetFileAttributesExW(g_spaceCsv.c_str(), GetFileExInfoStandard, &fa) != 0;
  if (exists && (static_cast<uint64_t>(fa.nFileSizeHigh) << 32 | fa.nFileSizeLow) > (8ull << 20)) return;
  FILE* f = nullptr;
  if (_wfopen_s(&f, g_spaceCsv.c_str(), L"ab") || !f) return;
  if (!exists)
    fprintf(f, "time,level,family,type,inside,canopy,canopy_src,frame_src,cockpit_ctx,yaw_deg,pitch_deg,roll_deg,"
               "spread_deg,wet,early,wet_voices,room_p50_us,room_p99_us,dcomp_mean_db,lg_gain,lg_lowpass,match_lo_db,"
               "match_mid_db,match_hi_db\r\n");
  SYSTEMTIME st;
  GetLocalTime(&st);
  audspace::SpaceData sd{};
  audspace::SpaceRead(g_space, &sd);
  const float c[4] = {g_cnt.roomC[0].load(), g_cnt.roomC[1].load(), g_cnt.roomC[2].load(), g_cnt.roomC[3].load()};
  float yaw = 0, pitch = 0, roll = 0;
  const bool frame = g_cnt.roomFrameValid.load() != 0;
  if (frame) audspace::YawPitchRoll(c, &yaw, &pitch, &roll);
  double p50 = 0, p99 = 0;
  RoomTimes(&p50, &p99);
  const float canopy = g_spaceCtlNow.canopy;
  char canopyText[16] = "";
  if (canopy >= 0) snprintf(canopyText, sizeof(canopyText), "%.2f", canopy);
  const int csrc = canopy >= 0 ? g_luaCanopySrc.load() : 0;
  const bool probe = (sd.flags & audspace::flags::kReads) && sd.worldCtx < 8;
  char lg[48] = ",";
  if (probe) snprintf(lg, sizeof(lg), "%.4g,%.4g", sd.lgGain, sd.lgLowpass);
  fprintf(f, "%02d:%02d:%02d.%03d,%s,%ls,%s,%d,%s,%s,%s,%d,", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
          SpaceLevelName(g_spaceSt.level), cockpit_space::FamilyKey(g_spaceSt.family),
          g_spaceSt.type.empty() ? "unknown" : g_spaceSt.type.c_str(), g_spaceSt.insideNow, canopyText,
          csrc == 2 ? "arg38" : csrc == 1 ? "mech" : "none", frame ? "sound" : "none", sd.cockpitCtx);
  if (frame) fprintf(f, "%.1f,%.1f,%.1f,", yaw, pitch, roll);
  else fprintf(f, ",,,");
  fprintf(f, "%.2f,%.3f,%d,%u,%.1f,%.1f,%.2f,%s,%.2f,%.2f,%.2f\r\n", sd.spreadDeg, g_cnt.roomWet.load(),
          g_cnt.roomEarly.load() ? 1 : 0, g_cnt.roomVoicesNow.load(), p50, p99, g_cnt.dcompMeanDb.load(), lg,
          g_cnt.roomMatchDb[0].load(), g_cnt.roomMatchDb[1].load(), g_cnt.roomMatchDb[2].load());
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
  SpaceTick();
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
  // Retired bindings: freed 2 s after their last block; then every set no live binding holds is released (the old
  // binding's sets at another rate, or ones it retired that the current binding never had).
  bool freed = false;
  for (size_t i = 0; i < g_retiredBindings.size();) {
    audr::Binding* b = g_retiredBindings[i];
    if (Qpc() - b->lastUseQpc.load() > 2 * QpcFreq()) {
      FreeBinding(b);
      g_retiredBindings.erase(g_retiredBindings.begin() + static_cast<ptrdiff_t>(i));
      freed = true;
    } else {
      ++i;
    }
  }
  if (freed) {
    for (size_t i = 0; i < g_sets.size();) {
      const binaural::HrtfSet* s = g_sets[i].set;
      if (audr::SetStillHeld(s, g_curSet, g_binding.load(), g_retiredBindings)) {
        ++i;
        continue;
      }
      g_retiredByCore.erase(std::remove(g_retiredByCore.begin(), g_retiredByCore.end(), s), g_retiredByCore.end());
      g_retiredByRoom.erase(std::remove(g_retiredByRoom.begin(), g_retiredByRoom.end(), s), g_retiredByRoom.end());
      audr::CoreReleaseHrtf(g_sets[i].set);
      g_sets.erase(g_sets.begin() + static_cast<ptrdiff_t>(i));
      g_cnt.setsReleasedAfterRebind.fetch_add(1, std::memory_order_relaxed);
    }
  }
  if (audr::Binding* b = g_binding.load()) {
    // A set is released only once both renderers gave it back: the voices' core and, with cockpit space, the room.
    while (const binaural::HrtfSet* s = audr::CoreTakeRetired(b->core)) g_retiredByCore.push_back(s);
    if (b->room)
      while (const binaural::HrtfSet* s = audr::RoomTakeRetired(b->room)) g_retiredByRoom.push_back(s);
    for (size_t k = 0; k < g_retiredByCore.size();) {
      const binaural::HrtfSet* s = g_retiredByCore[k];
      const auto inRoom = std::find(g_retiredByRoom.begin(), g_retiredByRoom.end(), s);
      if (b->room && inRoom == g_retiredByRoom.end()) {
        ++k;
        continue;
      }
      if (inRoom != g_retiredByRoom.end()) g_retiredByRoom.erase(inRoom);
      g_retiredByCore.erase(g_retiredByCore.begin() + static_cast<ptrdiff_t>(k));
      for (size_t i = 0; i < g_sets.size(); ++i)
        if (g_sets[i].set == s) {
          audr::CoreReleaseHrtf(g_sets[i].set);
          g_sets.erase(g_sets.begin() + static_cast<ptrdiff_t>(i));
          b->given.erase(std::remove(b->given.begin(), b->given.end(), s), b->given.end());
          break;
        }
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
      // The sounds' make-up: mono waves, stereo ones, and the voices kept wide in the last block (DCS's own side around
      // the binaural mid: under 1.4 / 1.6 m, or a diffuse bed by its own signal; mono or stereo waves alike).
      const uint32_t owned = g_cnt.ownedNow.load(), st = std::min(owned, g_cnt.ownedStereoNow.load());
      // Cockpit space: its level and family, inside, the voices sent to the room in the last block, the early
      // reflections, the room's own p99 and the mean direct-gain compensation (before "; head pose", which ends the line).
      char space[720] = "Off";
      if (g_spaceSt.blind) {
        snprintf(space, sizeof(space), "Test");  // a listening test: neither the level nor its room figures
      } else if (g_spaceSt.level > 0) {
        double p50 = 0, p99 = 0;
        RoomTimes(&p50, &p99);
        // The block energies around the room's Render since the last line (mean per block, dB): the owned voices' dry
        // direct sound, `out` before the Render and after it; where the Render ran (inside the mix / after it).
        const double eb = static_cast<double>(std::max<uint64_t>(1, g_cnt.roomEBlocks.exchange(0)));
        auto edb = [eb](double e) { return e > 0 ? 10.0 * std::log10(e / eb) : -999.0; };
        const double eDry = edb(g_cnt.roomEDry.exchange(0.0)), eBefore = edb(g_cnt.roomEOutBefore.exchange(0.0)),
                     eAfter = edb(g_cnt.roomEOutAfter.exchange(0.0));
        snprintf(space, sizeof(space),
                 "%s %ls inside %d wet voices %u early %s room p99 %.2f ms, direct %.1f dB, match %+.1f/%+.1f/%+.1f dB",
                 SpaceLevelName(g_spaceSt.level), cockpit_space::FamilyKey(g_spaceSt.family), g_spaceSt.insideNow,
                 g_cnt.roomVoicesNow.load(), g_cnt.roomEarly.load() ? "on" : "off", p99 / 1000.0, g_cnt.dcompMeanDb.load(),
                 g_cnt.roomMatchDb[0].load(), g_cnt.roomMatchDb[1].load(), g_cnt.roomMatchDb[2].load());
        size_t used = strlen(space);
        snprintf(space + used, sizeof(space) - used,
                 ", energy dry %.1f out %.1f -> %.1f dB, renders %llu in mix %llu after %llu (dropped %llu, added %llu, "
                 "late %llu), blocks %llu, master %+.1f dB", eDry, eBefore,
                 eAfter, static_cast<unsigned long long>(g_cnt.roomRenders.load()),
                 static_cast<unsigned long long>(g_cnt.roomFlushInMix.load()),
                 static_cast<unsigned long long>(g_cnt.roomFlushAfterMix.load()),
                 static_cast<unsigned long long>(g_cnt.roomDropped.load()),
                 static_cast<unsigned long long>(g_cnt.roomAfterWet.load()),
                 static_cast<unsigned long long>(g_cnt.roomLateCalls.load()),
                 static_cast<unsigned long long>(g_cnt.roomBlocks.load()), g_cnt.masterGainDb.load());
        // Where the room's time goes since the last line, mean per room block (us): the per-voice sends, the wall
        // filters, the bus HRTF renders, the tail, the output with the level match.
        {
          static double prevSec[binaural::kRoomSections] = {}, prevSend = 0;
          static uint64_t prevBlocks = 0;
          double sec[binaural::kRoomSections] = {}, mx[binaural::kRoomSections] = {};
          if (audr::Binding* bb = g_binding.load(); bb && bb->room) {
            audr::RoomSectionsUs(bb->room, sec, mx);
            audr::RoomResetSectionMax(bb->room);  // the maxima's window: one running line
          }
          const double sendMax = g_cnt.roomSendMaxUs.exchange(0.0), blockMax = g_cnt.roomBlockMaxUs.exchange(0.0);
          const uint64_t blocksNow = g_cnt.roomBlocks.load();
          const double nb = static_cast<double>(blocksNow > prevBlocks ? blocksNow - prevBlocks : 1);
          const double sendNow = g_cnt.roomSendUs.load();
          double d[binaural::kRoomSections];
          for (uint32_t k = 0; k < binaural::kRoomSections; ++k)
            d[k] = sec[k] >= prevSec[k] ? (sec[k] - prevSec[k]) / nb : 0.0, prevSec[k] = sec[k];
          const double sends = sendNow >= prevSend ? (sendNow - prevSend) / nb : 0.0;
          prevSend = sendNow, prevBlocks = blocksNow;
          used = strlen(space);
          snprintf(space + used, sizeof(space) - used,
                   ", us per block: sends %.0f walls %.0f buses %.0f early hp %.0f tail %.0f match %.0f", sends, d[1],
                   d[2], d[3], d[4], d[5]);
          // and the longest block of each since the last line: in the room's sections or outside them
          used = strlen(space);
          snprintf(space + used, sizeof(space) - used,
                   ", us max: sends %.0f drain %.0f walls %.0f buses %.0f early hp %.0f tail %.0f match %.0f room %.0f",
                   sendMax, mx[0], mx[1], mx[2], mx[3], mx[4], mx[5], blockMax);
        }
      }
      Log("audio: running, %u sounds, p99 %.2f ms per block (budget %.0f%% of a block, about %.2f ms at 10 ms), limit "
          "%u; mono %u, stereo %u; wide %u (near or diffuse); fades in/out %llu/%llu, fallbacks %llu, refused %llu, torn %llu (snapshots %llu), "
          "pan solves %llu (median %llu, mismatch "
          "%llu), overruns %llu; space %s; head pose %s",
          owned, P99Ms(), g_cfgRead.cfg.cpuBudgetPercent, budgetMs,
          std::min<uint32_t>(g_cnt.limitNow.load(), static_cast<uint32_t>(g_cfgRead.cfg.maxObjects)), owned - st, st,
          std::min(owned, g_cnt.ownedWideNow.load()),
          static_cast<unsigned long long>(g_cnt.fadeIns.load()), static_cast<unsigned long long>(g_cnt.fadeOuts.load()),
          static_cast<unsigned long long>(g_cnt.repanCalls.load()), static_cast<unsigned long long>(g_cnt.coreRejected.load()),
          static_cast<unsigned long long>(g_meta.torn.load()), static_cast<unsigned long long>(g_cnt.tornSnaps.load()),
          static_cast<unsigned long long>(g_cnt.panSolves.load()),
          static_cast<unsigned long long>(g_cnt.panMedian.load()), static_cast<unsigned long long>(g_cnt.panMismatch.load()),
          static_cast<unsigned long long>(g_cnt.overruns.load()), space, pose);
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
          "%llu, limit %llu, no wave yet %llu",
          u(blocks - L.idleBlocks), u(armed - L.idleArmed), u(pan - L.idlePan), u(refused[audr::kRefuseOff]),
          u(refused[audr::kRefuseOutput]), u(refused[audr::kRefuseNot3D]), u(refused[audr::kRefuseOffline]),
          u(refused[audr::kRefuseChannels]), u(refused[audr::kRefuseContext]), u(refused[audr::kRefuseDistance]),
          u(refused[audr::kRefuseStereoOut]), u(refused[audr::kRefuseGains]), u(refused[audr::kRefusePanMismatch]),
          u(refused[audr::kRefuseNotCreation]), u(refused[audr::kRefuseLimit]), u(refused[audr::kRefuseNoWave]));
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
    SpaceCsvTick();
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
    g_retiredByCore.clear();
    g_retiredByRoom.clear();
  }
  audr::g_status.store(0);
}

// DCS is shutting down: the thread stops; hooks and bindings stay (DCS may still call them while it tears down).
void Quiesce(DWORD ms) { JoinThread(ms); }

}  // namespace audio
