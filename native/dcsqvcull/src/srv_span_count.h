// setShaderResources span counter (R15 F2, measurement only; no behaviour change).
//
// dx11backend's exported DX11Renderer::setShaderResources (0x1ac20) keeps a
// 128-slot shadow per stage (this+0x118+stage*0x400, count this+0xf8+stage*4).
// It ignores the start argument, skips only the identical prefix, zero-fills
// the old tail when the new count is smaller, and binds
// [firstDiff, max(new, old)) through the context member pointer [V 0x1ac20-
// 0x1ad0a]. F2 would bind only [firstDiff, lastDiff]: the identical tail of
// the span is redundant. Its only callers are the two direct calls in FX
// ApplyShaderBlock (0x68546, 0x688b8) [V xref].
//
// While [Suite] SrvSpanCount runs (5 s), both call sites are redirected to a
// wrapper that calls the original function unchanged and, per call, compares
// the shadow before and after: passed span [firstDiff, end), changed span
// [first changed, last changed], the identical head and tail of the passed
// span, and rdtsc around the call. A least-squares fit of call time against
// the passed slot count gives the per-slot cost used for the estimate. The
// call sites are restored afterwards.
// Included once from main.cpp inside its anonymous namespace, after pacer.h
// (AllocNear), pose_sweep.h (PatchAllSuspended), shadow_tex.h (CodeIs) and
// pass_timing.h (ptiming::g_topTid).
#pragma once

namespace srvspan {

constexpr uint32_t kFn = 0x1ac20;                     // DX11Renderer::setShaderResources
constexpr uint32_t kFnEnd = 0x1ad0b;
constexpr uint64_t kFnHash = 0xce5d23c1a21ed3d1ull;   // FNV-1a 64 of [kFn, kFnEnd)
const uint32_t kSites[2] = {0x68546, 0x688b8};        // the two calls in ApplyShaderBlock 0x68470
constexpr uint32_t kCountOff = 0xf8, kShadowOff = 0x118, kStageStride = 0x400;
constexpr int kMaxSlots = 128, kStages = 6;
const char kExport[] =
    "?setShaderResources@DX11Renderer@RenderAPI@@QEAAXW4enShaderType@2@P8ID3D11DeviceContext@@EAAXIIPEBQEAUID3D11"
    "ShaderResourceView@@@ZII1@Z";

using Fn = void(__fastcall*)(uint8_t* self, int stage, void* setter, uint32_t start, uint32_t count,
                             void* const* views);
Fn g_fn = nullptr;
std::atomic<bool> g_on{false};
uint8_t* g_stub = nullptr;      // near page: mov rax, Hook; jmp rax
uint8_t g_saved[2][5] = {};     // original call bytes
std::atomic<bool> g_patched{false};
uint8_t* g_dx = nullptr;
DWORD g_renderTid = 0;

// Span-length buckets: 0, 1, 2, 3, 4, 5-8, 9-16, 17+.
constexpr int kBuckets = 8;
const char* const kBucketNames[kBuckets] = {"0", "1", "2", "3", "4", "5-8", "9-16", "17+"};
inline int Bucket(uint32_t n) {
  if (n <= 4) return static_cast<int>(n);
  if (n <= 8) return 5;
  if (n <= 16) return 6;
  return 7;
}

struct Counters {
  uint64_t calls = 0, renderThread = 0, noCall = 0, d3dCalls = 0, faults = 0;
  uint64_t passed = 0, changedSpan = 0, changedSlots = 0, head = 0, tail = 0, allSame = 0, allSameSlots = 0;
  uint64_t shrink = 0;  // calls whose count shrank (the old tail is nulled)
  uint64_t stageCalls[kStages] = {}, stagePassed[kStages] = {}, stageTail[kStages] = {};
  uint64_t hist[kBuckets][kBuckets] = {};  // [passed bucket][changed-span bucket]
  uint64_t bucketCalls[kBuckets] = {}, bucketCyc[kBuckets] = {};
  uint64_t cycNoCall = 0;
  // Least squares, cycles ~ a + b * passed (calls that reach the context).
  double n = 0, sx = 0, sxx = 0, sy = 0, sxy = 0, syy = 0;
};
Counters g_c;
SRWLOCK g_lock = SRWLOCK_INIT;

struct Sample {
  uint32_t oldCount, newCount, end, first;
  void* before[kMaxSlots];
};

bool Pre(uint8_t* self, int stage, uint32_t count, void* const* views, Sample& s) {
  __try {
    if (stage < 0 || stage >= kStages || count > kMaxSlots) return false;
    s.newCount = count;
    s.oldCount = *reinterpret_cast<uint32_t*>(self + kCountOff + stage * 4);
    if (s.oldCount > kMaxSlots) return false;
    s.end = count > s.oldCount ? count : s.oldCount;
    void* const* shadow = reinterpret_cast<void* const*>(self + kShadowOff + static_cast<size_t>(stage) * kStageStride);
    memcpy(s.before, shadow, s.end * sizeof(void*));
    uint32_t i = 0;
    while (i < count && views[i] == s.before[i]) ++i;  // the function's own identical-prefix rule
    s.first = i;
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

bool Post(uint8_t* self, int stage, const Sample& s, uint64_t cyc) {
  int firstCh = -1, lastCh = -1;
  uint32_t nCh = 0;
  __try {
    void* const* shadow = reinterpret_cast<void* const*>(self + kShadowOff + static_cast<size_t>(stage) * kStageStride);
    for (uint32_t i = s.first; i < s.end; ++i)
      if (shadow[i] != s.before[i]) {
        if (firstCh < 0) firstCh = static_cast<int>(i);
        lastCh = static_cast<int>(i);
        ++nCh;
      }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
  const uint32_t passed = s.end > s.first ? s.end - s.first : 0;
  const bool render = GetCurrentThreadId() == g_renderTid;
  AcquireSRWLockExclusive(&g_lock);
  Counters& c = g_c;
  c.calls++;
  if (render) c.renderThread++;
  c.stageCalls[stage]++;
  if (s.newCount < s.oldCount) c.shrink++;
  if (!passed) {
    c.noCall++;
    c.cycNoCall += cyc;
    c.hist[0][0]++;
  } else {
    c.d3dCalls++;
    c.passed += passed;
    c.stagePassed[stage] += passed;
    uint32_t span = 0, tail = passed, head = 0;
    if (nCh) {
      span = static_cast<uint32_t>(lastCh - firstCh + 1);
      tail = s.end - 1 - static_cast<uint32_t>(lastCh);
      head = static_cast<uint32_t>(firstCh) - s.first;
    } else {
      c.allSame++;
      c.allSameSlots += passed;
    }
    c.changedSpan += span;
    c.changedSlots += nCh;
    c.head += head;
    c.tail += tail;
    c.stageTail[stage] += tail;
    c.hist[Bucket(passed)][Bucket(span)]++;
    const int b = Bucket(passed);
    c.bucketCalls[b]++;
    c.bucketCyc[b] += cyc;
    const double x = passed, y = static_cast<double>(cyc);
    c.n += 1;
    c.sx += x;
    c.sxx += x * x;
    c.sy += y;
    c.sxy += x * y;
    c.syy += y * y;
  }
  ReleaseSRWLockExclusive(&g_lock);
  return true;
}

void __fastcall Hook(uint8_t* self, int stage, void* setter, uint32_t start, uint32_t count, void* const* views) {
  if (!g_on.load(std::memory_order_relaxed)) return g_fn(self, stage, setter, start, count, views);
  Sample s;
  const bool pre = Pre(self, stage, count, views, s);
  const uint64_t t0 = __rdtsc();
  g_fn(self, stage, setter, start, count, views);
  const uint64_t dt = __rdtsc() - t0;
  if (!pre || !Post(self, stage, s, dt)) {
    AcquireSRWLockExclusive(&g_lock);
    g_c.faults++;
    ReleaseSRWLockExclusive(&g_lock);
  }
}

// Checks the build: the export, the function bytes and both call sites.
const char* Check(uint8_t* dx) {
  if (reinterpret_cast<uint8_t*>(GetProcAddress(reinterpret_cast<HMODULE>(dx), kExport)) != dx + kFn)
    return "srv span counter: setShaderResources export is not at dx11backend+0x1ac20; skipped";
  if (!shadowtex::CodeIs(dx, kFn, kFnEnd, kFnHash))
    return "srv span counter: setShaderResources differs from the analysed build; skipped";
  for (int i = 0; i < 2; ++i) {
    uint8_t b[5];
    int32_t rel;
    if (!allocslab::ReadBytes(dx + kSites[i], b, 5) || b[0] != 0xE8)
      return "srv span counter: unexpected bytes at an ApplyShaderBlock call site; skipped";
    memcpy(&rel, b + 1, 4);
    if (dx + kSites[i] + 5 + rel != dx + kFn) return "srv span counter: call site does not call setShaderResources; skipped";
  }
  return nullptr;
}

bool PatchSites(bool on) {
  // Both originals saved before either site changes: when site 0 failed to
  // patch, the restore wrote site 1's never-saved (zero) bytes into
  // ApplyShaderBlock. Restoring an unpatched site now rewrites its own bytes.
  if (on)
    for (int i = 0; i < 2; ++i) memcpy(g_saved[i], g_dx + kSites[i], 5);
  for (int i = 0; i < 2; ++i) {
    uint8_t* site = g_dx + kSites[i];
    uint8_t b[5];
    if (on) {
      b[0] = 0xE8;
      const int32_t rel = static_cast<int32_t>(g_stub - (site + 5));
      memcpy(b + 1, &rel, 4);
    } else {
      memcpy(b, g_saved[i], 5);
    }
    if (!posesweep::PatchAllSuspended(site, b, 5)) return false;
  }
  return true;
}

// Restores the call sites if a measurement was interrupted (payload stop).
void Shutdown() {
  g_on = false;
  if (g_patched.exchange(false)) PatchSites(false);
}

void Measure(int ms, std::atomic<uint64_t>& frames, double tscHz) {
  auto* dx = reinterpret_cast<uint8_t*>(GetModuleHandleW(L"dx11backend.dll"));
  if (!dx || !tscHz) {
    Log("  srv span counter: dx11backend.dll not loaded");
    return;
  }
  if (const char* why = Check(dx)) {
    Log("  %s", why);
    return;
  }
  g_dx = dx;
  g_fn = reinterpret_cast<Fn>(dx + kFn);
  if (!g_stub) {
    uint8_t* page = pacer::AllocNear(dx);
    if (!page) {
      Log("  srv span counter: no memory within reach of dx11backend.dll");
      return;
    }
    memset(page, 0xCC, 4096);
    page[0] = 0x48;
    page[1] = 0xB8;  // mov rax, imm64
    void* hook = reinterpret_cast<void*>(&Hook);
    memcpy(page + 2, &hook, 8);
    page[10] = 0xFF;
    page[11] = 0xE0;  // jmp rax
    DWORD old;
    VirtualProtect(page, 4096, PAGE_EXECUTE_READ, &old);
    FlushInstructionCache(GetCurrentProcess(), page, 4096);
    g_stub = page;
  }
  AcquireSRWLockExclusive(&g_lock);
  g_c = Counters{};
  ReleaseSRWLockExclusive(&g_lock);
  g_renderTid = ptiming::g_topTid.load();
  if (!PatchSites(true)) {
    PatchSites(false);
    Log("  srv span counter: could not patch the ApplyShaderBlock call sites");
    return;
  }
  g_patched = true;
  Sleep(200);
  const uint64_t f0 = frames.load();
  g_on = true;
  Sleep(ms);
  g_on = false;
  const double f = static_cast<double>(frames.load() - f0);
  Sleep(100);
  if (g_patched.exchange(false) && !PatchSites(false)) Log("  srv span counter: WARNING could not restore a call site");
  AcquireSRWLockExclusive(&g_lock);
  const Counters c = g_c;
  ReleaseSRWLockExclusive(&g_lock);
  if (f <= 0 || c.calls == 0) {
    Log("  srv span counter: no setShaderResources calls counted (%.0f frames)", f);
    return;
  }
  const double nsPerCyc = 1e9 / tscHz;
  auto pct = [](double a, double b) { return b > 0 ? 100.0 * a / b : 0.0; };
  Log("  srv span: %.0f frames; setShaderResources %.0f calls/frame (%.1f%% on the render thread %lu), %.0f reach "
      "the context, %.0f return without a call; count shrank in %.0f/frame; %llu reads faulted%s",
      f, c.calls / f, pct(static_cast<double>(c.renderThread), static_cast<double>(c.calls)),
      static_cast<unsigned long>(g_renderTid), c.d3dCalls / f, c.noCall / f, c.shrink / f,
      static_cast<unsigned long long>(c.faults),
      d3ds::g_vtblCount.load() > 0 ? " (D3D11 meter hooks installed: call times include them)" : "");
  Log("  srv span: slots bound %.0f/frame (%.2f per call); changed span [firstDiff, lastDiff] %.0f/frame, changed "
      "slots %.0f/frame; identical tail %.0f/frame, identical head %.0f/frame; calls with no changed slot %.0f/frame "
      "(%.0f slots)",
      c.passed / f, c.d3dCalls ? static_cast<double>(c.passed) / c.d3dCalls : 0.0, c.changedSpan / f,
      c.changedSlots / f, c.tail / f, c.head / f, c.allSame / f, c.allSameSlots / f);
  for (int s = 0; s < kStages; ++s)
    if (c.stageCalls[s])
      Log("  srv span stage %d: %.0f calls/frame, %.0f slots bound/frame, identical tail %.0f/frame (%.1f%%)", s,
          c.stageCalls[s] / f, c.stagePassed[s] / f, c.stageTail[s] / f,
          pct(static_cast<double>(c.stageTail[s]), static_cast<double>(c.stagePassed[s])));
  Log("  srv span histogram, calls/frame by passed span (rows) and changed span (columns %s|%s|%s|%s|%s|%s|%s|%s):",
      kBucketNames[0], kBucketNames[1], kBucketNames[2], kBucketNames[3], kBucketNames[4], kBucketNames[5],
      kBucketNames[6], kBucketNames[7]);
  for (int r = 0; r < kBuckets; ++r) {
    uint64_t row = 0;
    for (int k = 0; k < kBuckets; ++k) row += c.hist[r][k];
    if (!row) continue;
    char line[512];
    int n = snprintf(line, sizeof(line), "    passed %-4s:", kBucketNames[r]);
    for (int k = 0; k < kBuckets && n < static_cast<int>(sizeof(line)) - 16; ++k)
      n += snprintf(line + n, sizeof(line) - n, " %8.1f", c.hist[r][k] / f);
    if (r > 0 && c.bucketCalls[r])
      snprintf(line + n, sizeof(line) - n, "   mean call %.0f ns", c.bucketCyc[r] * nsPerCyc / c.bucketCalls[r]);
    Log("%s", line);
  }
  // Per-slot cost: least squares over the calls that reach the context.
  const double det = c.n * c.sxx - c.sx * c.sx;
  double a = 0, b = 0, r2 = 0;
  if (c.n > 2 && det > 0) {
    b = (c.n * c.sxy - c.sx * c.sy) / det;
    a = (c.sy - b * c.sx) / c.n;
    const double sst = c.syy - c.sy * c.sy / c.n;
    const double sse = c.syy - a * c.sy - b * c.sxy;
    r2 = sst > 0 ? 1.0 - sse / sst : 0.0;
  }
  const double share = pct(static_cast<double>(c.tail), static_cast<double>(c.passed));
  const double slotNs = b > 0 ? b * nsPerCyc : 0.0;
  const double trimMs = c.tail * slotNs * 1e-6 / f;
  const double skipMs = (a > 0 ? a * nsPerCyc : 0.0) * c.allSame * 1e-6 / f;
  Log("  srv span cost model (render thread side, d3d11 runtime + driver producer): %.1f ns per call + %.2f ns per "
      "slot (R^2 %.2f); mean call %.0f ns, calls without a context call %.0f ns",
      a * nsPerCyc, slotNs, r2, c.n > 0 ? c.sy / c.n * nsPerCyc : 0.0,
      c.noCall ? c.cycNoCall * nsPerCyc / c.noCall : 0.0);
  Log("  srv span estimate: identical tail %.1f%% of slots bound (R15 F2 gate 30%%: %s); trimming it saves about "
      "%.3f ms/frame at %.2f ns/slot, plus %.3f ms/frame if calls with no changed slot were dropped",
      share, share > 30.0 ? "PASS" : "below", trimMs, slotNs, skipMs);
}

}  // namespace srvspan
