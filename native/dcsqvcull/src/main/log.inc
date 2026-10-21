namespace {

// ---------------------------------------------------------------------------
// Logging and configuration
// ---------------------------------------------------------------------------

std::wstring g_dir;
std::mutex g_logMutex;
FILE* g_log = nullptr;                   // standalone builds (tests) only
const DcsQvLoaderApi* g_api = nullptr;   // set when running under the loader
std::atomic<bool> g_stop{false};         // payload is being replaced
qvstatus::Sink g_statusSink;             // loader-owned status word (status_word.h)
qvstatus::Sink g_dualStatusSink;         // loader-owned dual camera status word (DcsQvCull_DualCameraStatus)
bool g_reportCapture = false;  // guarded by g_logMutex
DWORD g_reportThread = 0;      // only lines logged by the suite thread go to the report
std::string g_reportBuf;

// R90 (docs/research/R90_hitch_hunting.md F2): asynchronous log path for DCS's threads. Log() used to format,
// write and flush (WriteFile) on the calling thread under two mutexes (this one, then the loader's): a render-
// thread line could wait behind a worker's burst (the census, the perception block) or a write stalled by a
// reader of the file (DCS Control and the harness read it) or a filter driver. Now:
//   - a thread that did not start through qvexit (DCS's render, collect and Main threads) formats the line,
//     stamps the local time and puts it into a bounded lock-free ring (Vyukov MPMC, kAsyncSlots lines); the
//     payload's log writer thread writes the lines in ring order, so each thread's lines keep their order, with
//     the time stamped at the call (through the loader's DcsQvCull_LogAt; a loader without it stamps the write
//     time, at most one writer period later);
//   - a full ring drops the line and counts it; the writer then logs "log: N lines from DCS threads dropped";
//     R93 (docs/research/R92_live_hitch_fg.md D1): the ring is 4096 lines (was 256: ~950 lines of the first mission
//     load's burst were dropped at every DCS start), and only a frame thread drops: a DCS thread that never ran one
//     of our per-frame hooks (the loading / Main threads; qvalog::t_frameThread false) waits for ring space instead
//     (the writer is woken; at most kFullWaitMs per line, then the line is dropped and counted). It still never
//     writes the file itself. R115 (L1): after such a timeout, while the writer has not moved (g_deq unchanged),
//     the next non-frame lines that find the ring full are dropped at once (counted) instead of waiting again;
//     once the writer moves, waiting resumes. A frame thread (the render thread: HookCollect, the pass hook, the xrEndFrame hooks)
//     never waits and never writes: a full ring drops its line, as before;
//   - the payload's own threads (worker, recorders, suite) write synchronously as before; a synchronous line
//     first drains the ring (a fault line of ours comes after every queued line), and Stop / Quiesce drain it.
// [Log] Async=0 writes every line synchronously (the old path). The line format and the .prev roll are the
// loader's, unchanged.
thread_local bool g_tOurThread = false;  // set by qvexit::NoteStart: a payload thread (Run / RunVoid)

namespace qvalog {
// Power of two (R93: was 256); 8 MiB of zero-initialised (demand-zero) memory: R115 (L2) nothing touches a page of
// it before a line goes through it (no initialiser writes the slots; see Slot::seq).
constexpr uint32_t kAsyncSlots = 4096;
constexpr uint32_t kFullWaitMs = 250;   // R93: a non-frame DCS thread waits at most this long per line for ring space
// R93: set on a DCS thread by our per-frame hooks (HookCollect, the top-level pass hook, the xrEndFrame hooks): the
// render thread. Its lines never wait for ring space (a full ring drops them). Every other DCS thread may wait.
thread_local bool t_frameThread = false;
inline void NoteFrameThread() { t_frameThread = true; }
// R115 (L2): a trivial type (no member initialiser, no std::atomic: MSVC runs a load-time constructor loop over an
// array of such class objects, writing every slot), so the ring is plain zero-initialised (demand-zero) data. seq is
// Vyukov's sequence minus the slot's index, read / written only through SeqOf / SetSeq (acquire / release): the
// initial seq[i] = i is a stored 0, so the zero ring is the empty ring.
struct Slot {
  LONG seq;
  SYSTEMTIME st;
  char text[2048];
};
Slot g_ring[kAsyncSlots];
inline uint32_t SeqOf(const Slot& s, uint32_t idx) { return static_cast<uint32_t>(ReadAcquire(&s.seq)) + idx; }
inline void SetSeq(Slot& s, uint32_t idx, uint32_t seq) { WriteRelease(&s.seq, static_cast<LONG>(seq - idx)); }
std::atomic<uint32_t> g_enq{0};
// Written only under g_drainMutex (the next line to write); R115 (L1): atomic so the stall latch can read it.
std::atomic<uint32_t> g_deq{0};
// R115 (L1): g_deq when a non-frame line last timed out waiting for ring space (-1: none). While g_deq is still this
// value (the writer has not moved), a non-frame line finding the ring full is dropped at once instead of waiting.
std::atomic<int64_t> g_stallDeq{-1};
std::mutex g_drainMutex;            // one consumer at a time (the writer, or a synchronous line)
std::atomic<bool> g_on{false};      // writer running and [Log] Async=1: DCS threads enqueue
std::atomic<bool> g_want{true};     // [Log] Async
std::atomic<bool> g_testHold{false};  // tests: the writer leaves the ring alone
std::atomic<uint64_t> g_queued{0}, g_dropped{0}, g_written{0};
uint64_t g_droppedLogged = 0;       // under g_drainMutex
// R93: lines a non-frame DCS thread queued after waiting for ring space, the longest wait (us), the drops by frame
// threads (never waited) and the writer's wake event (set by a waiting thread; null: the writer polls every 10 ms).
std::atomic<uint64_t> g_waited{0}, g_waitMaxUs{0}, g_frameDropped{0};
uint64_t g_waitedLogged = 0;        // under g_drainMutex
uint64_t g_frameDroppedLogged = 0;  // under g_drainMutex (R115 L5: the frame-thread share of the dropped report)
HANDLE g_wake = nullptr;
using LogAtFn = void (*)(const char* line, const SYSTEMTIME* st);
LogAtFn g_logAt = nullptr;          // the loader's DcsQvCull_LogAt (resolved at start; null: g_api->log)

// Any thread. False when the ring is full (the caller counts the drop).
bool Push(const char* line, const SYSTEMTIME& st) {
  uint32_t pos = g_enq.load(std::memory_order_relaxed);
  Slot* s;
  uint32_t idx;
  for (;;) {
    idx = pos & (kAsyncSlots - 1);
    s = &g_ring[idx];
    const uint32_t seq = SeqOf(*s, idx);
    const int32_t dif = static_cast<int32_t>(seq - pos);
    if (dif == 0) {
      if (g_enq.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) break;
    } else if (dif < 0) {
      return false;  // full
    } else {
      pos = g_enq.load(std::memory_order_relaxed);
    }
  }
  s->st = st;
  strncpy_s(s->text, sizeof(s->text), line, _TRUNCATE);
  SetSeq(*s, idx, pos + 1);
  g_queued.fetch_add(1, std::memory_order_relaxed);
  return true;
}

// R93: a non-frame DCS thread with a full ring: wake the writer and retry for up to kFullWaitMs. 1: queued; 0: still
// full (the caller counts the drop); -1: the async path went off meanwhile (the caller writes synchronously, as with
// [Log] Async=0). Never touches the file or a log lock.
int WaitPush(const char* line, const SYSTEMTIME& st) {
  LARGE_INTEGER f, t0, t1;
  QueryPerformanceFrequency(&f);
  QueryPerformanceCounter(&t0);
  const int64_t limit = static_cast<int64_t>(f.QuadPart) * kFullWaitMs / 1000;
  for (;;) {
    if (!g_on.load(std::memory_order_acquire)) return -1;
    if (g_wake) SetEvent(g_wake);
    Sleep(1);
    const bool ok = Push(line, st);
    QueryPerformanceCounter(&t1);
    if (ok) {
      g_waited.fetch_add(1, std::memory_order_relaxed);
      const uint64_t us = static_cast<uint64_t>((t1.QuadPart - t0.QuadPart) * 1000000 / f.QuadPart);
      uint64_t m = g_waitMaxUs.load(std::memory_order_relaxed);
      while (us > m && !g_waitMaxUs.compare_exchange_weak(m, us, std::memory_order_relaxed)) {
      }
      return 1;
    }
    if (t1.QuadPart - t0.QuadPart >= limit) {
      g_stallDeq.store(g_deq.load(std::memory_order_acquire), std::memory_order_release);  // R115 (L1): latched
      return 0;
    }
  }
}

// R115 (L1): true while the writer has not moved since a non-frame line last timed out waiting for ring space (the
// caller drops its line at once, counted). Clears the latch once the writer moved: the next full ring waits again.
bool WriterStalled() {
  int64_t at = g_stallDeq.load(std::memory_order_acquire);
  if (at < 0) return false;
  if (static_cast<uint32_t>(at) == g_deq.load(std::memory_order_acquire)) return true;
  g_stallDeq.compare_exchange_strong(at, -1, std::memory_order_acq_rel);
  return false;
}

// One line to the log file (caller holds g_logMutex). st: the time to stamp (nullptr = now).
void Emit(const char* line, const SYSTEMTIME* st) {
  if (g_api) {
    if (st && g_logAt)
      g_logAt(line, st);
    else
      g_api->log(line);
    return;
  }
  if (!g_log) return;
  SYSTEMTIME now;
  if (!st) {
    GetLocalTime(&now);
    st = &now;
  }
  fprintf(g_log, "%02d:%02d:%02d.%03d %s\n", st->wHour, st->wMinute, st->wSecond, st->wMilliseconds, line);
  fflush(g_log);
}

// Set while this thread drains: a contained fault inside a drain (the SEH unwind skips the lock's destructor)
// must not make that thread's fault line wait for the drain lock it still holds.
thread_local bool t_inDrain = false;

bool Pending() {
  if (t_inDrain) return false;
  std::lock_guard<std::mutex> d(g_drainMutex);
  const uint32_t deq = g_deq.load(std::memory_order_relaxed);
  const uint32_t idx = deq & (kAsyncSlots - 1);
  return SeqOf(g_ring[idx], idx) == deq + 1;
}

// Writes every queued line in ring order (writer thread, a synchronous line, Stop). Returns the lines written.
uint32_t Drain() {
  if (t_inDrain) return 0;
  std::lock_guard<std::mutex> d(g_drainMutex);
  t_inDrain = true;
  uint32_t n = 0;
  uint32_t deq = g_deq.load(std::memory_order_relaxed);
  for (;;) {
    const uint32_t idx = deq & (kAsyncSlots - 1);
    Slot& s = g_ring[idx];
    if (SeqOf(s, idx) != deq + 1) break;  // empty, or the next slot not written yet
    {
      std::lock_guard<std::mutex> lock(g_logMutex);
      Emit(s.text, &s.st);
    }
    SetSeq(s, idx, deq + kAsyncSlots);
    ++deq;
    g_deq.store(deq, std::memory_order_release);
    ++n;
  }
  g_written.fetch_add(n, std::memory_order_relaxed);
  const uint64_t dropped = g_dropped.load(std::memory_order_relaxed);
  if (dropped != g_droppedLogged) {
    // R115 (L5): the frame threads' share (dropped without waiting) appended after the unchanged phrase.
    const uint64_t frameDropped = g_frameDropped.load(std::memory_order_relaxed);
    const uint64_t frameNew = frameDropped >= g_frameDroppedLogged ? frameDropped - g_frameDroppedLogged : frameDropped;
    char line[240];
    snprintf(line, sizeof(line),
             "log: %llu lines from DCS threads dropped (async log ring of %u lines full); %llu of them from frame "
             "threads (the render thread)",
             static_cast<unsigned long long>(dropped - g_droppedLogged), kAsyncSlots,
             static_cast<unsigned long long>(frameNew));
    g_droppedLogged = dropped;
    g_frameDroppedLogged = frameDropped;
    std::lock_guard<std::mutex> lock(g_logMutex);
    Emit(line, nullptr);
  }
  const uint64_t waited = g_waited.load(std::memory_order_relaxed);
  if (waited != g_waitedLogged) {  // R93: a burst a loading thread outran the ring with (nothing lost)
    char line[200];
    snprintf(line, sizeof(line),
             "log: %llu lines from DCS non-frame threads waited for ring space (async log ring of %u lines full; "
             "longest wait %.1f ms); none of them dropped",
             static_cast<unsigned long long>(waited - g_waitedLogged), kAsyncSlots,
             g_waitMaxUs.exchange(0, std::memory_order_relaxed) / 1000.0);  // R115 (L5): this report's waits
    g_waitedLogged = waited;
    std::lock_guard<std::mutex> lock(g_logMutex);
    Emit(line, nullptr);
  }
  t_inDrain = false;
  return n;
}
}  // namespace qvalog

void Log(const char* fmt, ...) {
  if (!g_api && !g_log) return;
  char line[2048];
  va_list ap;
  va_start(ap, fmt);
  qvlog::FormatV(line, sizeof(line), fmt, ap);  // %ls as UTF-8 (log_utf8.h)
  va_end(ap);
  if (!g_tOurThread && qvalog::g_on.load(std::memory_order_acquire)) {  // R90: DCS's threads never write
    SYSTEMTIME st;
    GetLocalTime(&st);
    if (qvalog::Push(line, st)) return;
    if (qvalog::t_frameThread) {  // R93: the render thread never waits: dropped and counted
      qvalog::g_frameDropped.fetch_add(1, std::memory_order_relaxed);
      qvalog::g_dropped.fetch_add(1, std::memory_order_relaxed);
      return;
    }
    // R93: a loading / Main thread waits for ring space (bounded). R115 (L1): not again while the writer is stalled.
    const int w = qvalog::WriterStalled() ? 0 : qvalog::WaitPush(line, st);
    if (w > 0) return;
    if (w == 0) {
      qvalog::g_dropped.fetch_add(1, std::memory_order_relaxed);
      return;
    }
    // w < 0: the async path went off while it waited: the synchronous path below (as with [Log] Async=0)
  }
  if (qvalog::Pending()) qvalog::Drain();  // lines queued before this one first
  std::lock_guard<std::mutex> lock(g_logMutex);
  if (!g_api && !g_log) return;
  qvalog::Emit(line, nullptr);
  if (g_reportCapture && GetCurrentThreadId() == g_reportThread) {
    g_reportBuf += line;
    g_reportBuf += '\n';
  }
}

// Hook slots go through the loader's registry so a reloaded payload always
// receives the true original function, never an older payload's hook.
bool HookSlot(void** slot, void* hook, void** original) {
  if (g_api) return g_api->patch(slot, hook, original);
  DWORD old;
  if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) return false;
  void* prev = InterlockedExchangePointer(slot, hook);
  if (original) *original = prev;
  VirtualProtect(slot, sizeof(void*), old, &old);
  return true;
}

void* SlotOriginal(void** slot) { return g_api ? g_api->original(slot) : *slot; }

void UnhookSlot(void** slot, void* original) {
  if (g_api) {
    g_api->restore(slot);
    return;
  }
  DWORD old;
  if (VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) {
    InterlockedExchangePointer(slot, original);
    VirtualProtect(slot, sizeof(void*), old, &old);
  }
}

void StartReportCapture() {
  std::lock_guard<std::mutex> lock(g_logMutex);
  g_reportBuf.clear();
  g_reportThread = GetCurrentThreadId();
  g_reportCapture = true;
}

std::string StopReportCapture() {
  std::lock_guard<std::mutex> lock(g_logMutex);
  g_reportCapture = false;
  return std::move(g_reportBuf);
}

