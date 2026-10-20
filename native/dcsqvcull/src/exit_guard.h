// DCS shutdown and late faults on the payload's own threads.
//
// Field report (DCS Control 0.5.5, GitHub NIGos/dcs-control#1): on quitting
// DCS, right after "render thread has stopped", the payload's worker thread
// faulted (AV at payload+0x136D1A, WorkerThread -> qvstatus::Sink::Publish)
// writing the status word that lives in the loader's data. DCS had closed the
// hooks' Lua state, whose loadlib finalizer FreeLibrary'd the loader
// (DcsQvCull.dll) while the payload, loaded by the loader, stayed mapped and
// kept running. The loader now pins itself (loader.cpp), and the Lua hook
// signals the exit before that finalizer runs (DcsQvCull_Shutdown ->
// DcsQvPayload_Quiesce: g_exiting, every periodic thread leaves its loop).
//
// As a last line, each thread the payload creates runs its body under
// Run/Thread<Body>: a hardware fault that escapes the body (DCS freed memory
// or unloaded a module under it during teardown) is logged and ends that
// thread only, instead of terminating DCS. Nothing here takes a lock in the
// filter; the log line is written from the handler on the faulting thread,
// never under the loader lock (these are not DllMain paths).
// Included once from main.cpp after Log.
#pragma once

namespace qvexit {

std::atomic<bool> g_exiting{false};       // DCS is shutting down (DcsQvPayload_Quiesce)
std::atomic<uint64_t> g_contained{0};     // thread-ending faults contained (all threads)
std::atomic<uint64_t> g_containedExit{0}; // of which after g_exiting was set
// R52: thread bodies started through Run / RunVoid (every payload thread). The
// worker placement pass (thread_place.h) re-runs soon after it changes,
// instead of polling with a system-wide thread snapshot.
std::atomic<uint32_t> g_threadStarts{0};
// R90: the ids of those threads, in start order, for thread_place.h's tracked placement (PlaceRecheckMode=1):
// it adopts them from here instead of finding them in a thread snapshot. A bounded ring: entry i holds
// (i + 1) << 32 | thread id once written; a reader that falls more than kStartRing behind re-discovers.
constexpr uint32_t kStartRing = 256;
std::atomic<uint64_t> g_startRing[kStartRing] = {};
std::atomic<uint32_t> g_startHead{0};
inline void NoteStart() {
  g_threadStarts.fetch_add(1, std::memory_order_relaxed);
  g_tOurThread = true;  // R90: Log() writes this thread's lines synchronously (main.cpp)
  const uint32_t i = g_startHead.fetch_add(1, std::memory_order_acq_rel);
  g_startRing[i % kStartRing].store((static_cast<uint64_t>(i + 1) << 32) | GetCurrentThreadId(),
                                    std::memory_order_release);
}

inline bool Exiting() { return g_exiting.load(std::memory_order_acquire); }
// True the first time (the caller does the one-time quiesce).
inline bool Begin() { return !g_exiting.exchange(true, std::memory_order_acq_rel); }

// The hardware faults a late read or write of freed DCS memory (or a call into
// an unloaded module) raises. C++ exceptions, breakpoints and stack overflow
// are left to the normal handlers.
inline bool Containable(unsigned long code) {
  switch (code) {
    case EXCEPTION_ACCESS_VIOLATION:
    case EXCEPTION_IN_PAGE_ERROR:
    case EXCEPTION_ILLEGAL_INSTRUCTION:
    case EXCEPTION_PRIV_INSTRUCTION:
    case EXCEPTION_DATATYPE_MISALIGNMENT:
      return true;
    default:
      return false;
  }
}

struct Fault {
  unsigned long code = 0;
  const void* at = nullptr;      // faulting instruction
  uintptr_t target = 0;          // AV / in-page: the address read or written
};

inline int Filter(const EXCEPTION_POINTERS* ep, Fault* f) {
  const EXCEPTION_RECORD* r = ep->ExceptionRecord;
  if (!Containable(r->ExceptionCode)) return EXCEPTION_CONTINUE_SEARCH;
  f->code = r->ExceptionCode;
  f->at = r->ExceptionAddress;
  if ((r->ExceptionCode == EXCEPTION_ACCESS_VIOLATION || r->ExceptionCode == EXCEPTION_IN_PAGE_ERROR) &&
      r->NumberParameters >= 2)
    f->target = static_cast<uintptr_t>(r->ExceptionInformation[1]);
  return EXCEPTION_EXECUTE_HANDLER;
}

// "payload+0x1234" or "module.dll+0x1234" or "0x..." (unmapped: an unloaded module).
inline void Where(const void* p, char* out, size_t n) {
  HMODULE m = nullptr;
  wchar_t path[MAX_PATH] = {};
  HMODULE self = nullptr;
  GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                     reinterpret_cast<LPCWSTR>(&Where), &self);
  if (p && GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                              static_cast<LPCWSTR>(p), &m) && m) {
    const unsigned long long off = static_cast<unsigned long long>(static_cast<const uint8_t*>(p) -
                                                                   reinterpret_cast<const uint8_t*>(m));
    if (m == self) {
      snprintf(out, n, "payload+0x%llX", off);
      return;
    }
    GetModuleFileNameW(m, path, MAX_PATH);
    const wchar_t* base = wcsrchr(path, L'\\');
    snprintf(out, n, "%ls+0x%llX", base ? base + 1 : path, off);
    return;
  }
  snprintf(out, n, "%p (no module)", p);
}

inline void Report(const void* body, const Fault& f) {
  const bool exiting = Exiting();
  g_contained.fetch_add(1, std::memory_order_relaxed);
  if (exiting) g_containedExit.fetch_add(1, std::memory_order_relaxed);
  // The log goes through the loader (g_api->log). A loader older than the
  // pin may already be unmapped here (that is the 0.5.5 exit crash itself):
  // then there is nowhere to write, and logging would fault again.
  HMODULE loader = nullptr;
  if (g_api && !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                   reinterpret_cast<LPCWSTR>(g_api), &loader))
    return;
  char at[160], fn[160];
  Where(f.at, at, sizeof(at));
  Where(body, fn, sizeof(fn));
  Log("thread %s: exception 0x%08lx at %s (address 0x%llX)%s; this thread exits, DCS keeps running", fn, f.code, at,
      static_cast<unsigned long long>(f.target),
      exiting ? " during DCS shutdown" : "");
}

constexpr DWORD kFaultExit = 0xDEADu;

// Runs body(arg); a containable fault that escapes it ends the thread (kFaultExit).
inline DWORD Run(LPTHREAD_START_ROUTINE body, void* arg) {
  NoteStart();
  Fault f;
  __try {
    return body(arg);
  } __except (Filter(GetExceptionInformation(), &f)) {
  }
  Report(reinterpret_cast<const void*>(body), f);
  return kFaultExit;
}

// Thread entry wrapper: CreateThread(nullptr, 0, qvexit::Thread<Body>, arg, ...).
template <DWORD(WINAPI* Body)(void*)>
DWORD WINAPI Thread(void* arg) {
  return Run(Body, arg);
}

// std::thread bodies (no argument).
inline void RunVoid(void (*body)()) {
  NoteStart();
  Fault f;
  __try {
    body();
    return;
  } __except (Filter(GetExceptionInformation(), &f)) {
  }
  Report(reinterpret_cast<const void*>(body), f);
}

}  // namespace qvexit
