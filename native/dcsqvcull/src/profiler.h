// In-process sampling profiler. Periodically suspends the busiest DCS
// threads, walks their stacks with the modules' own .pdata unwind tables and
// aggregates exclusive/inclusive samples per function. Included once from
// main.cpp after the globals it uses (Log, g_dir).
//
// While a thread is suspended nothing here may take a lock (heap, loader,
// logging): the target could be holding it. Module tables are snapshotted
// beforehand and samples go into preallocated buffers.
#pragma once

#include <psapi.h>
#include <tlhelp32.h>

#include <algorithm>
#include <array>
#include <map>

#pragma comment(lib, "psapi.lib")

namespace prof {

struct Module {
  uintptr_t base = 0, end = 0;
  std::string name;
  const RUNTIME_FUNCTION* pdata = nullptr;
  uint32_t pdataCount = 0;
  std::vector<std::pair<uint32_t, std::string>> exports;  // sorted by RVA
};

std::vector<Module> g_modules;  // sorted by base, read-only while sampling

void SnapshotModules() {
  g_modules.clear();
  HMODULE mods[2048];
  DWORD needed = 0;
  if (!EnumProcessModules(GetCurrentProcess(), mods, sizeof(mods), &needed)) return;
  size_t n = std::min<size_t>(needed / sizeof(HMODULE), 2048);
  for (size_t i = 0; i < n; ++i) {
    MODULEINFO mi{};
    if (!GetModuleInformation(GetCurrentProcess(), mods[i], &mi, sizeof(mi))) continue;
    Module m;
    m.base = reinterpret_cast<uintptr_t>(mi.lpBaseOfDll);
    m.end = m.base + mi.SizeOfImage;
    char name[MAX_PATH];
    GetModuleBaseNameA(GetCurrentProcess(), mods[i], name, MAX_PATH);
    m.name = name;
    auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(m.base);
    auto nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(m.base + dos->e_lfanew);
    const auto& exc = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
    if (exc.VirtualAddress && exc.Size) {
      m.pdata = reinterpret_cast<const RUNTIME_FUNCTION*>(m.base + exc.VirtualAddress);
      m.pdataCount = exc.Size / sizeof(RUNTIME_FUNCTION);
    }
    const auto& ed = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (ed.VirtualAddress && ed.Size) {
      auto dir = reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*>(m.base + ed.VirtualAddress);
      auto funcs = reinterpret_cast<const DWORD*>(m.base + dir->AddressOfFunctions);
      auto names = reinterpret_cast<const DWORD*>(m.base + dir->AddressOfNames);
      auto ords = reinterpret_cast<const WORD*>(m.base + dir->AddressOfNameOrdinals);
      for (DWORD k = 0; k < dir->NumberOfNames; ++k) {
        DWORD rva = funcs[ords[k]];
        if (rva >= ed.VirtualAddress && rva < ed.VirtualAddress + ed.Size) continue;  // forwarder
        m.exports.emplace_back(rva, reinterpret_cast<const char*>(m.base + names[k]));
      }
      std::sort(m.exports.begin(), m.exports.end());
    }
    g_modules.push_back(std::move(m));
  }
  std::sort(g_modules.begin(), g_modules.end(),
            [](const Module& a, const Module& b) { return a.base < b.base; });
}

int FindModule(uintptr_t pc) {
  size_t lo = 0, hi = g_modules.size();
  while (lo < hi) {
    size_t mid = (lo + hi) / 2;
    if (g_modules[mid].end <= pc)
      lo = mid + 1;
    else
      hi = mid;
  }
  if (lo < g_modules.size() && pc >= g_modules[lo].base && pc < g_modules[lo].end)
    return static_cast<int>(lo);
  return -1;
}

const RUNTIME_FUNCTION* FindEntry(const Module& m, uint32_t rva) {
  size_t lo = 0, hi = m.pdataCount;
  while (lo < hi) {
    size_t mid = (lo + hi) / 2;
    if (m.pdata[mid].EndAddress <= rva)
      lo = mid + 1;
    else
      hi = mid;
  }
  if (lo < m.pdataCount && rva >= m.pdata[lo].BeginAddress && rva < m.pdata[lo].EndAddress)
    return &m.pdata[lo];
  return nullptr;
}

// Follows chained unwind info back to the entry describing the function start.
uint32_t PrimaryBegin(const Module& m, const RUNTIME_FUNCTION* e) {
  for (int guard = 0; guard < 8; ++guard) {
    auto ui = reinterpret_cast<const uint8_t*>(m.base + e->UnwindData);
    uint8_t flags = ui[0] >> 3;
    if (!(flags & UNW_FLAG_CHAININFO)) break;
    uint8_t codes = ui[2];
    e = reinterpret_cast<const RUNTIME_FUNCTION*>(ui + 4 + ((codes + 1) & ~1) * 2);
  }
  return e->BeginAddress;
}

constexpr int kMaxDepth = 40;

struct Sample {
  uint16_t thread;
  uint8_t depth;
  uint64_t key[kMaxDepth];  // (module index << 32) | function begin RVA; ~0 = unknown
};

// Walks one suspended thread. No locks, no allocation.
int Walk(CONTEXT& ctx, uint64_t* keys) {
  int depth = 0;
  __try {
    for (; depth < kMaxDepth; ++depth) {
      uintptr_t pc = ctx.Rip;
      if (!pc) break;
      int mi = FindModule(pc);
      if (mi < 0) {
        keys[depth] = ~0ull;
        break;  // JIT or unknown code: cannot unwind further
      }
      const Module& m = g_modules[mi];
      uint32_t rva = static_cast<uint32_t>(pc - m.base);
      const RUNTIME_FUNCTION* e = m.pdata ? FindEntry(m, rva) : nullptr;
      if (!e) {
        keys[depth] = (static_cast<uint64_t>(mi) << 32) | rva;
        // Leaf function without unwind data: return address is at [rsp].
        ctx.Rip = *reinterpret_cast<uint64_t*>(ctx.Rsp);
        ctx.Rsp += 8;
        continue;
      }
      keys[depth] = (static_cast<uint64_t>(mi) << 32) | PrimaryBegin(m, e);
      void* handlerData = nullptr;
      DWORD64 establisher = 0;
      RtlVirtualUnwind(UNW_FLAG_NHANDLER, m.base, pc, const_cast<PRUNTIME_FUNCTION>(e), &ctx,
                       &handlerData, &establisher, nullptr);
      if (ctx.Rip == pc) break;
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }
  return depth;
}

struct ThreadInfo {
  DWORD tid;
  HANDLE h;
  std::string name;
  uint64_t cycles = 0;  // over the profile
  uint64_t samples = 0;
};

std::string ThreadName(HANDLE h) {
  using Fn = HRESULT(WINAPI*)(HANDLE, PWSTR*);
  static Fn fn = reinterpret_cast<Fn>(
      GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetThreadDescription"));
  if (!fn) return {};
  PWSTR w = nullptr;
  std::string out;
  if (SUCCEEDED(fn(h, &w)) && w) {
    char buf[256];
    WideCharToMultiByte(CP_UTF8, 0, w, -1, buf, sizeof(buf), nullptr, nullptr);
    out = buf;
    LocalFree(w);
  }
  return out;
}

std::string Symbol(uint64_t key) {
  if (key == ~0ull) return "<unknown>";
  const Module& m = g_modules[key >> 32];
  uint32_t rva = static_cast<uint32_t>(key);
  auto it = std::upper_bound(m.exports.begin(), m.exports.end(),
                             std::make_pair(rva, std::string("\xff")));
  char buf[64];
  snprintf(buf, sizeof(buf), "+0x%x", rva);
  std::string s = m.name + buf;
  if (it != m.exports.begin()) {
    --it;
    if (it->first == rva) return m.name + "!" + it->second;
    // Internal function: name the closest export below it as a hint.
    if (rva - it->first < 0x4000) s += " (after " + it->second.substr(0, 100) + ")";
  }
  return s;
}

std::atomic<bool> g_running{false};

// Profiles the `maxThreads` busiest threads for `seconds`. Blocking.
void RunBody(int seconds, int maxThreads, int periodMs, const char* label) {
  if (g_running.exchange(true)) return;
  Log("profile: start %d s (%s)", seconds, label);
  SnapshotModules();

  // Pick the busiest threads over half a second.
  std::vector<ThreadInfo> all;
  DWORD self = GetCurrentThreadId();
  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
  if (snap != INVALID_HANDLE_VALUE) {
    THREADENTRY32 te{sizeof(te)};
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
      if (te.th32OwnerProcessID != GetCurrentProcessId() || te.th32ThreadID == self) continue;
      HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION,
                            FALSE, te.th32ThreadID);
      if (h) all.push_back({te.th32ThreadID, h, {}, 0, 0});
    }
    CloseHandle(snap);
  }
  std::vector<uint64_t> c0(all.size());
  for (size_t i = 0; i < all.size(); ++i) QueryThreadCycleTime(all[i].h, &c0[i]);
  Sleep(500);
  for (size_t i = 0; i < all.size(); ++i) {
    ULONG64 c1 = 0;
    QueryThreadCycleTime(all[i].h, &c1);
    all[i].cycles = c1 - c0[i];
  }
  std::sort(all.begin(), all.end(),
            [](const ThreadInfo& a, const ThreadInfo& b) { return a.cycles > b.cycles; });
  std::vector<ThreadInfo> threads;
  for (auto& t : all) {
    if (static_cast<int>(threads.size()) < maxThreads && t.cycles > 0) {
      t.name = ThreadName(t.h);
      t.cycles = 0;
      threads.push_back(t);
    } else {
      CloseHandle(t.h);
    }
  }

  std::vector<uint64_t> start(threads.size());
  for (size_t i = 0; i < threads.size(); ++i) QueryThreadCycleTime(threads[i].h, &start[i]);
  // Capped (about 330 MB of samples): [Profile] Seconds/PeriodMs/Threads are
  // not bounded in the ini, and a several-GB request threw std::bad_alloc on
  // this thread, which ended DCS.
  const size_t maxSamples = (std::min)(
      static_cast<size_t>(seconds) * 1000 / std::max(1, periodMs) * threads.size() + 16, size_t{1} << 20);
  std::vector<Sample> samples(maxSamples);
  size_t ns = 0;
  // Windows 11 ignores timeBeginPeriod for many processes; a high resolution
  // waitable timer gives a real ~1 ms period.
  HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                        TIMER_ALL_ACCESS);
  LARGE_INTEGER f, t0, t;
  QueryPerformanceFrequency(&f);
  QueryPerformanceCounter(&t0);
  do {
    for (size_t i = 0; i < threads.size() && ns < maxSamples; ++i) {
      if (SuspendThread(threads[i].h) == static_cast<DWORD>(-1)) continue;
      CONTEXT ctx{};
      ctx.ContextFlags = CONTEXT_FULL;
      if (GetThreadContext(threads[i].h, &ctx)) {
        Sample& s = samples[ns];
        s.thread = static_cast<uint16_t>(i);
        s.depth = static_cast<uint8_t>(Walk(ctx, s.key));
        if (s.depth) ++ns;
      }
      ResumeThread(threads[i].h);
    }
    if (timer) {
      LARGE_INTEGER due;
      due.QuadPart = -10000LL * periodMs;
      SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE);
      WaitForSingleObject(timer, 1000);
    } else {
      Sleep(periodMs);
    }
    QueryPerformanceCounter(&t);
  } while ((t.QuadPart - t0.QuadPart) < seconds * f.QuadPart && ns < maxSamples);
  if (timer) CloseHandle(timer);
  double wall = double(t.QuadPart - t0.QuadPart) / f.QuadPart;
  for (size_t i = 0; i < threads.size(); ++i) {
    ULONG64 c1 = 0;
    QueryThreadCycleTime(threads[i].h, &c1);
    threads[i].cycles = c1 - start[i];
  }

  // Aggregate.
  struct Counts {
    uint64_t excl = 0, incl = 0;
  };
  std::vector<std::unordered_map<uint64_t, Counts>> perThread(threads.size());
  std::unordered_map<uint64_t, Counts> total;
  std::vector<std::map<std::string, uint64_t>> perModule(threads.size());
  // Leaf call paths (5 innermost frames) per thread: which callers the hot
  // exclusive functions are reached from.
  constexpr int kPathDepth = 5;
  using Path = std::array<uint64_t, kPathDepth>;
  std::vector<std::map<Path, uint64_t>> perPath(threads.size());
  for (size_t k = 0; k < ns; ++k) {
    const Sample& s = samples[k];
    threads[s.thread].samples++;
    auto& m = perThread[s.thread];
    m[s.key[0]].excl++;
    total[s.key[0]].excl++;
    uint64_t seen[kMaxDepth];
    int nseen = 0;
    for (int d = 0; d < s.depth; ++d) {
      bool dup = false;
      for (int j = 0; j < nseen; ++j) dup |= seen[j] == s.key[d];
      if (dup) continue;
      seen[nseen++] = s.key[d];
      m[s.key[d]].incl++;
      total[s.key[d]].incl++;
    }
    if (s.key[0] != ~0ull) perModule[s.thread][g_modules[s.key[0] >> 32].name]++;
    Path path;
    path.fill(0);
    for (int d = 0; d < kPathDepth && d < s.depth; ++d) path[d] = s.key[d];
    perPath[s.thread][path]++;
  }

  SYSTEMTIME st;
  GetLocalTime(&st);
  wchar_t name[96];
  swprintf_s(name, L"profile_%04d%02d%02d_%02d%02d%02d.txt", st.wYear, st.wMonth, st.wDay, st.wHour,
             st.wMinute, st.wSecond);
  FILE* out = nullptr;
  _wfopen_s(&out, (g_dir + name).c_str(), L"w");
  if (out) {
    uint64_t tscStart = __rdtsc();
    Sleep(100);
    double tscHz = (__rdtsc() - tscStart) * 10.0;
    fprintf(out, "DcsQvCull profile: %s, %.1f s, %zu threads, %zu samples, period %d ms\n\n", label,
            wall, threads.size(), ns, periodMs);
    auto dumpTop = [&](const std::unordered_map<uint64_t, Counts>& m, uint64_t n, bool incl, int top) {
      std::vector<std::pair<uint64_t, uint64_t>> v;
      for (auto& kv : m) v.emplace_back(incl ? kv.second.incl : kv.second.excl, kv.first);
      std::sort(v.rbegin(), v.rend());
      for (int i = 0; i < top && i < static_cast<int>(v.size()); ++i)
        fprintf(out, "  %6.2f%%  %s\n", n ? 100.0 * v[i].first / n : 0.0, Symbol(v[i].second).c_str());
    };
    for (size_t i = 0; i < threads.size(); ++i) {
      const ThreadInfo& th = threads[i];
      fprintf(out, "==== thread %lu \"%s\": cpu %.0f%% of one core, %llu samples ====\n", th.tid,
              th.name.c_str(), th.cycles / tscHz / wall * 100.0,
              static_cast<unsigned long long>(th.samples));
      fprintf(out, " modules (exclusive):\n");
      std::vector<std::pair<uint64_t, std::string>> mods;
      for (auto& kv : perModule[i]) mods.emplace_back(kv.second, kv.first);
      std::sort(mods.rbegin(), mods.rend());
      for (size_t k = 0; k < mods.size() && k < 12; ++k)
        fprintf(out, "  %6.2f%%  %s\n", 100.0 * mods[k].first / std::max<uint64_t>(1, th.samples),
                mods[k].second.c_str());
      fprintf(out, " top exclusive:\n");
      dumpTop(perThread[i], th.samples, false, 30);
      fprintf(out, " top inclusive:\n");
      dumpTop(perThread[i], th.samples, true, 40);
      fprintf(out, " hot paths (leaf <- caller <- ...):\n");
      std::vector<std::pair<uint64_t, Path>> paths;
      for (auto& kv : perPath[i]) paths.emplace_back(kv.second, kv.first);
      std::sort(paths.begin(), paths.end(),
                [](const auto& a, const auto& b) { return a.first > b.first; });
      for (size_t k = 0; k < paths.size() && k < 25; ++k) {
        fprintf(out, "  %6.2f%%  ", 100.0 * paths[k].first / std::max<uint64_t>(1, th.samples));
        for (int d = 0; d < kPathDepth && paths[k].second[d]; ++d)
          fprintf(out, "%s%s", d ? "\n             <- " : "", Symbol(paths[k].second[d]).c_str());
        fprintf(out, "\n");
      }
      fprintf(out, "\n");
    }
    fprintf(out, "==== all sampled threads, top inclusive ====\n");
    dumpTop(total, ns, true, 60);
    fclose(out);
  }
  for (auto& th : threads) CloseHandle(th.h);
  Log("profile: done, %zu samples -> %ls", ns, name);
  g_running = false;
}

// A thread of its own (ProfileThread): an exception here would end DCS.
void Run(int seconds, int maxThreads, int periodMs, const char* label) {
  try {
    RunBody(seconds, maxThreads, periodMs, label);
  } catch (...) {
    Log("profile: out of memory; aborted");
    g_running = false;
  }
}

}  // namespace prof
