// R90 (docs/research/R90_hitch_hunting.md, F3 and F1): this process's threads without a system-wide
// snapshot, and code-site writes with every other thread of the process suspended.
//
// Why. Every call-site patch (split_filter.h, shadow_rec.h's Scene sort sites, pose_sweep.h's
// PatchAllSuspended used by big_pages.h, edtime.h, srv_span_count.h, srv_tail_trim.h, alloc_slab.h,
// join_tail_count.h) walked CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD): a snapshot of every thread
// of the system (~7,500 on the test PC; 60 ms p50 on the worker, R52), then opened each DCS thread by
// id and suspended it, reading its context before the next one (serial waits for each suspension).
// R82: every kill-switch flag (112 of 112) was followed by a render-thread spike of 78 ms p50, 181 ms max.
//
// [General] PatchMode=1 (default): the threads come from NtGetNextThread over this process only (a
// walk of the process's own thread list, ~220 entries; handles straight from the walk, no OpenThread
// by id). All of them are suspended first, then their contexts are read: the suspensions complete in
// parallel instead of one after the other. [General] PatchMode=0: the previous path, unchanged (kept
// for the A/B).
//
// Why still every thread (and not only the render thread or the threads whose IP is in the range).
// - The write is not atomic for the instruction fetch: a site is 5-6 bytes at any alignment (it often
//   straddles an 8-byte boundary), and the Intel/AMD manuals give no guarantee for code another core
//   executes while it is rewritten without the cross-modifying-code protocol (SDM vol. 3, 8.1.3: the
//   modifying and the executing processors must synchronise; an int3-and-IPI scheme like Linux's
//   text_poke_bp would need an exception handler emulating the call on every core).
// - Suspending only the threads whose IP is in the range is not enough: a thread left running can
//   enter the range during the write.
// - "Only the render thread executes it" cannot be proven offline for these sites: the dx11backend
//   sites (split_filter.h) are DCS's renderer code, which our recorder workers also run (deferred-
//   context record and replay), and the Scene sort sites are called from the collect path, which DCS
//   runs on its collect pool as well (Scene CollectThreadsMax).
// So the invariant stays the one the old code had, and that is what makes the write safe: at the
// moment of the write every other thread of the process is suspended (its context read, so the
// suspension has completed), and none has its instruction pointer strictly inside a patched range (a
// thread at the first byte, or returning to the byte after, sees a whole old or a whole new
// instruction). Only the cost changes: no system-wide snapshot (that was most of the kill switch's
// 78 ms) and no serial suspension waits.
//
// Nothing between the first SuspendThread and the last ResumeThread allocates, logs or takes a lock a
// suspended thread may hold (fixed arrays; VirtualProtect and FlushInstructionCache are system calls).
// Included once from main.cpp after exit_guard.h.
#pragma once

#include <tlhelp32.h>

namespace qvpatch {

// [General] PatchMode (LoadConfig): 1 = process-only enumeration and batched suspension (R90 default),
// 0 = the Toolhelp path. Read on every write.
std::atomic<int> g_mode{1};

// Statistics (worker reads them for its lines; the tests read them).
std::atomic<uint64_t> g_writes{0}, g_heldUsTotal{0}, g_heldUsMax{0}, g_lastHeldUs{0}, g_lastEnumUs{0};
std::atomic<uint32_t> g_lastThreads{0}, g_lastAttempts{0};

using NtGetNextThreadFn = LONG(NTAPI*)(HANDLE process, HANDLE thread, ACCESS_MASK access, ULONG attributes,
                                       ULONG flags, PHANDLE next);
inline NtGetNextThreadFn NextThreadFn() {
  static const NtGetNextThreadFn f = [] {
    HMODULE nt = GetModuleHandleW(L"ntdll.dll");
    return nt ? reinterpret_cast<NtGetNextThreadFn>(GetProcAddress(nt, "NtGetNextThread")) : nullptr;
  }();
  return f;
}

// R115 P5: test hook (tests only; null in the payload): replaces NtGetNextThread so a test can make the
// walk fail midway. Read once per walk (code-site writes and placement discovery), never per frame.
NtGetNextThreadFn g_testNextThread = nullptr;

constexpr LONG kStatusNoMoreEntries = static_cast<LONG>(0x8000001AL);  // STATUS_NO_MORE_ENTRIES

// Calls keep = f(HANDLE) for every thread of this process, the calling thread included (handles with
// `access`, from the process's own thread list: no system-wide snapshot). A handle f keeps (true) is
// the caller's to close; the others are closed here. Returns the number of threads visited, -1 when
// NtGetNextThread is unavailable or fails before the end of the list (R115 P5: only
// STATUS_NO_MORE_ENTRIES ends the walk; any other failure, e.g. access denied or handle quota, means
// threads may have been missed, and the handles f kept are still the caller's to close).
template <class F>
int ForEachProcessThread(ACCESS_MASK access, F f) {
  const NtGetNextThreadFn next = g_testNextThread ? g_testNextThread : NextThreadFn();
  if (!next) return -1;
  HANDLE cur = nullptr;
  bool keptCur = false;
  int n = 0;
  for (;;) {
    HANDLE h = nullptr;
    const LONG st = next(GetCurrentProcess(), cur, access, 0, 0, &h);
    if (cur && !keptCur) CloseHandle(cur);
    if (st == kStatusNoMoreEntries) break;  // the end of the list: done
    if (st < 0 || !h) return -1;            // R115 P5: an error mid-walk is not the end of the list
    ++n;
    cur = h;
    keptCur = f(h);
  }
  return n;
}

struct Site {
  uint8_t* addr;
  const uint8_t* bytes;
  size_t len;
};

inline uint64_t NowUs() {
  static const double toUs = [] {
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    return 1e6 / static_cast<double>(f.QuadPart);
  }();
  LARGE_INTEGER c;
  QueryPerformanceCounter(&c);
  return static_cast<uint64_t>(c.QuadPart * toUs);
}

inline bool InsideAny(uintptr_t rip, const Site* s, int n) {
  for (int i = 0; i < n; ++i) {
    const uintptr_t a = reinterpret_cast<uintptr_t>(s[i].addr);
    if (rip > a && rip < a + s[i].len) return true;
  }
  return false;
}

inline bool WriteAll(const Site* s, int n) {
  bool done = true;
  for (int i = 0; i < n; ++i) {
    DWORD old;
    if (!VirtualProtect(s[i].addr, s[i].len, PAGE_EXECUTE_READWRITE, &old)) {
      done = false;
      continue;
    }
    memcpy(s[i].addr, s[i].bytes, s[i].len);
    VirtualProtect(s[i].addr, s[i].len, old, &old);
    FlushInstructionCache(GetCurrentProcess(), s[i].addr, s[i].len);
  }
  return done;
}

inline void NoteWrite(uint64_t heldUs, uint64_t enumUs, uint32_t threads, uint32_t attempts) {
  g_writes.fetch_add(1, std::memory_order_relaxed);
  g_heldUsTotal.fetch_add(heldUs, std::memory_order_relaxed);
  uint64_t m = g_heldUsMax.load(std::memory_order_relaxed);
  while (heldUs > m && !g_heldUsMax.compare_exchange_weak(m, heldUs)) {
  }
  g_lastHeldUs.store(heldUs, std::memory_order_relaxed);
  g_lastEnumUs.store(enumUs, std::memory_order_relaxed);
  g_lastThreads.store(threads, std::memory_order_relaxed);
  g_lastAttempts.store(attempts, std::memory_order_relaxed);
}

constexpr int kMaxHeld = 2048;  // as before (DCS runs ~220 threads)

// PatchMode=0: the pre-R90 path (system-wide Toolhelp snapshot, open by id, suspend and read the context
// one thread at a time), as split_filter.h / shadow_rec.h / pose_sweep.h had it.
inline bool WriteSuspendedToolhelp(const Site* s, int n, int attempts) {
  for (int attempt = 0; attempt < attempts; ++attempt) {
    const uint64_t t0 = NowUs();
    HANDLE held[kMaxHeld];  // on the stack: no allocation while threads are suspended
    int nHeld = 0;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;
    const uint64_t t1 = NowUs();
    THREADENTRY32 te{sizeof(te)};
    bool clear = true;
    uint64_t firstSuspend = 0;
    for (BOOL ok = Thread32First(snap, &te); ok && nHeld < kMaxHeld; ok = Thread32Next(snap, &te)) {
      if (te.th32OwnerProcessID != GetCurrentProcessId() || te.th32ThreadID == GetCurrentThreadId()) continue;
      HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, te.th32ThreadID);
      if (!h) continue;
      if (SuspendThread(h) == static_cast<DWORD>(-1)) {
        CloseHandle(h);
        continue;
      }
      if (!firstSuspend) firstSuspend = NowUs();
      held[nHeld++] = h;
      CONTEXT ctx{};
      ctx.ContextFlags = CONTEXT_CONTROL;
      if (GetThreadContext(h, &ctx) && InsideAny(ctx.Rip, s, n)) clear = false;
    }
    CloseHandle(snap);
    const bool done = clear && WriteAll(s, n);
    for (int k = 0; k < nHeld; ++k) {
      ResumeThread(held[k]);
      CloseHandle(held[k]);
    }
    const uint64_t t2 = NowUs();
    if (clear) {
      NoteWrite(firstSuspend ? t2 - firstSuspend : 0, t1 - t0, static_cast<uint32_t>(nHeld),
                static_cast<uint32_t>(attempt + 1));
      return done;
    }
    Sleep(2);
  }
  return false;
}

// PatchMode=1 (R90): this process's threads from NtGetNextThread, all suspended first, then every
// context read (each GetThreadContext returns once that thread's suspension has completed), then the
// IP check and the write, then all resumed.
inline bool WriteSuspendedProcess(const Site* s, int n, int attempts) {
  const DWORD self = GetCurrentThreadId();
  for (int attempt = 0; attempt < attempts; ++attempt) {
    HANDLE held[kMaxHeld];  // on the stack: no allocation while threads are suspended
    int nHeld = 0;
    bool overflow = false;
    const uint64_t t0 = NowUs();
    const int visited = ForEachProcessThread(
        THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_LIMITED_INFORMATION, [&](HANDLE h) {
          if (GetThreadId(h) == self) return false;
          if (nHeld >= kMaxHeld) {
            overflow = true;
            return false;
          }
          if (SuspendThread(h) == static_cast<DWORD>(-1)) return false;  // exiting: it runs no code of ours
          held[nHeld++] = h;
          return true;
        });
    const uint64_t t1 = NowUs();
    bool clear = visited >= 0 && !overflow;
    for (int k = 0; k < nHeld && clear; ++k) {
      CONTEXT ctx{};
      ctx.ContextFlags = CONTEXT_CONTROL;
      if (GetThreadContext(held[k], &ctx) && InsideAny(ctx.Rip, s, n)) clear = false;
    }
    const bool done = clear && WriteAll(s, n);
    for (int k = 0; k < nHeld; ++k) {
      ResumeThread(held[k]);
      CloseHandle(held[k]);
    }
    const uint64_t t2 = NowUs();
    // No NtGetNextThread, a failed walk (R115 P5: threads may have been missed; nothing was written and every
    // thread held above is resumed) or too many threads: the Toolhelp path.
    if (visited < 0 || overflow) return WriteSuspendedToolhelp(s, n, attempts);
    if (clear) {
      NoteWrite(t2 - t0, t1 - t0, static_cast<uint32_t>(nHeld), static_cast<uint32_t>(attempt + 1));
      return done;
    }
    Sleep(2);
  }
  return false;
}

// Writes every site with all other threads of the process suspended and none inside a site, retrying
// (2 ms apart) while one is. False when no clear moment came within `attempts` or a page could not be
// unprotected.
inline bool WriteSuspended(const Site* s, int n, int attempts = 50) {
  if (n <= 0) return true;
  return g_mode.load(std::memory_order_relaxed) == 0 ? WriteSuspendedToolhelp(s, n, attempts)
                                                      : WriteSuspendedProcess(s, n, attempts);
}

}  // namespace qvpatch
