// R30 field diagnostics (section 5, L1-L7) and the GPU-bound governor's
// driver (R30 proposal 1). Worker thread only, about once per second; the
// render thread only feeds perfgov's frame sampler (gov.h) from the collect
// hook. Every line goes through Log (UTF-8, log_utf8.h).
//
// Lines (examples in R30 / the report):
//   env:        once at start, once when DCS's GPU is known, once when the
//               view layout is known; repeated every 30 min (8 MB tail)
//   perf:       every [Log] PerfIntervalSec (10 s): fps, frame p50/p95,
//               render-thread busy, GPU busy (D3DKMT, any vendor; NVML as a
//               fallback), verdict CPU|GPU|mixed|capped, engine, governor
//   features:   the 18 production optimizations on/idle/latched/off with a
//               short reason, on change (at most every 10 s) and every 60 s
//   warmup:     shader-variant compile start and end
//   toggle:     kill switch: the 10 s before and the 10 s after (2 s settle)
//   vram:       every [Log] VramIntervalSec (60 s): DXGI local budget/usage
//   governor:   every stage change, with its evidence
//
// GPU busy: the 3D engine nodes' running time (D3DKMTQueryStatistics, the
// counter Task Manager shows), whole adapter (DCS + compositor + streamer)
// and DCS's own share. No D3D call is made on DCS's device or context.
// Render-thread busy: QueryThreadCycleTime of DCS's render thread over the
// wall time: the thread our pass hooks run on (shadow batching's pass hook,
// else either recorder's render-entry thread); the collect call runs on pool
// threads and is not it. No busy % (no verdict) until one of them has run.
//
// Included once from main.cpp inside its anonymous namespace, after the
// recorders, shadow_inst.h and split_filter.h.
#pragma once

namespace perfmon {

// ---------------------------------------------------------------------------
// DCS's device and adapter
// ---------------------------------------------------------------------------
// AddRef'd, or null while no feature has seen DCS's device yet.
ID3D11Device* DcsDevice() {
  {
    std::lock_guard<std::mutex> lock(shadowinst::g_mutex);
    if (shadowinst::g_device) {
      shadowinst::g_device->AddRef();
      return shadowinst::g_device;
    }
  }
  if (sfilt::Ctx* c = sfilt::g_ctx.load()) {
    ID3D11Device* d = nullptr;
    c->GetDevice(&d);
    return d;
  }
  return pdev::Device();  // [General] PerceptionOnly (perception_device.h)
}

// DCS's render thread: the thread our pass hooks run on (0 = not seen yet).
DWORD RenderTid() {
  if (DWORD t = shadowbatch::g_renderThread) return t;
  if (DWORD t = gbrec::g_renderTid) return t;
  return shrec::g_renderTid;
}

struct Adapter {
  IDXGIAdapter3* a3 = nullptr;
  DXGI_ADAPTER_DESC1 desc{};
  bool haveDesc = false;
  bool fromDcs = false;    // DCS's device's adapter (else the first adapter)
  std::string driver;      // UMD version a.b.c.d
  int commandLists = -1;   // D3D11 THREADING.DriverCommandLists of DCS's device (-1 unknown)
  int featureLevel = 0;
};
Adapter g_ad;

void SetAdapter(IDXGIAdapter* a) {
  IDXGIAdapter1* a1 = nullptr;
  if (SUCCEEDED(a->QueryInterface(__uuidof(IDXGIAdapter1), reinterpret_cast<void**>(&a1))) && a1) {
    g_ad.haveDesc = SUCCEEDED(a1->GetDesc1(&g_ad.desc));
    a1->Release();
  }
  LARGE_INTEGER umd{};
  if (SUCCEEDED(a->CheckInterfaceSupport(__uuidof(IDXGIDevice), &umd))) {
    char b[48];
    snprintf(b, sizeof(b), "%u.%u.%u.%u", HIWORD(umd.HighPart), LOWORD(umd.HighPart), HIWORD(umd.LowPart),
             LOWORD(umd.LowPart));
    g_ad.driver = b;
  }
  if (g_ad.a3) g_ad.a3->Release();
  g_ad.a3 = nullptr;
  a->QueryInterface(__uuidof(IDXGIAdapter3), reinterpret_cast<void**>(&g_ad.a3));
}

bool OpenDcsAdapter() {
  ID3D11Device* dev = DcsDevice();
  if (!dev) return false;
  IDXGIDevice* dx = nullptr;
  IDXGIAdapter* a = nullptr;
  if (SUCCEEDED(dev->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&dx))) && dx) {
    dx->GetAdapter(&a);
    dx->Release();
  }
  D3D11_FEATURE_DATA_THREADING th{};
  if (SUCCEEDED(dev->CheckFeatureSupport(D3D11_FEATURE_THREADING, &th, sizeof(th))))
    g_ad.commandLists = th.DriverCommandLists ? 1 : 0;
  g_ad.featureLevel = static_cast<int>(dev->GetFeatureLevel());
  dev->Release();
  if (!a) return false;
  SetAdapter(a);
  a->Release();
  g_ad.fromDcs = true;
  return true;
}

// The first DXGI adapter (DCS's device not seen: no shadow instancing and no
// split filter). dxgi.dll is DCS's; nothing is linked.
bool OpenFirstAdapter() {
  HMODULE m = GetModuleHandleW(L"dxgi.dll");
  using Fn = HRESULT(WINAPI*)(REFIID, void**);
  auto create = m ? reinterpret_cast<Fn>(GetProcAddress(m, "CreateDXGIFactory1")) : nullptr;
  IDXGIFactory1* f = nullptr;
  if (!create || FAILED(create(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&f))) || !f) return false;
  IDXGIAdapter1* a = nullptr;
  const bool ok = SUCCEEDED(f->EnumAdapters1(0, &a)) && a;
  if (ok) {
    SetAdapter(a);
    a->Release();
  }
  f->Release();
  return ok;
}

struct Vram {
  bool ok = false;
  uint64_t budget = 0, usage = 0, nlUsage = 0;
};
Vram ReadVram() {
  Vram v;
  DXGI_QUERY_VIDEO_MEMORY_INFO i{};
  if (g_ad.a3 && SUCCEEDED(g_ad.a3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &i))) {
    v.ok = true;
    v.budget = i.Budget;
    v.usage = i.CurrentUsage;
  }
  if (g_ad.a3 && SUCCEEDED(g_ad.a3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &i)))
    v.nlUsage = i.CurrentUsage;
  return v;
}

// ---------------------------------------------------------------------------
// GPU busy (D3DKMT, gdi32 exports resolved at run time)
// ---------------------------------------------------------------------------
struct Kmt {
  using QsFn = NTSTATUS(APIENTRY*)(const D3DKMT_QUERYSTATISTICS*);
  using OpenFn = NTSTATUS(APIENTRY*)(D3DKMT_OPENADAPTERFROMLUID*);
  using InfoFn = NTSTATUS(APIENTRY*)(const D3DKMT_QUERYADAPTERINFO*);
  using CloseFn = NTSTATUS(APIENTRY*)(const D3DKMT_CLOSEADAPTER*);
  QsFn qs = nullptr;
  bool ok = false;
  LUID luid{};
  std::vector<ULONG> nodes;  // 3D engine nodes (node 0 when the metadata is not available)
  // The previous sample per node and whether it is there (a flag, not a sentinel: D3DKMT's RunningTime can be
  // negative; node 0 of the RTX 5090 reads about -3.0e9 x 100 ns and still counts up, so -1 as "none" made
  // every sample of that node invalid and the GPU busy line n/a).
  std::vector<LONGLONG> lastAll, lastProc;
  std::vector<char> haveAll, haveProc;
  // The process query never answered with either handle kind (STATUS_INVALID_PARAMETER for a process that has no
  // device on this adapter, or for a LIMITED handle): DCS's share stays n/a, no more switching / logging.
  bool procUnsupported = false, procEver = false;
  uint32_t procProbe = 0;  // while unsupported: samples since the last probe (one query per kKmtProbe samples)
  int64_t lastQpc = 0;
  // This process for D3DKMT_QUERYSTATISTICS_PROCESS_NODE ("(dcs N%)"). The
  // query needs PROCESS_QUERY_INFORMATION: a PROCESS_QUERY_LIMITED_INFORMATION
  // handle makes every process query fail with STATUS_INVALID_PARAMETER
  // (0xC000000D) [M 2026-10-12, Windows 11 26300, RTX 5090: the pseudo handle
  // and a QUERY_INFORMATION handle answer, LIMITED never], which was the
  // permanent "(dcs n/a)" since the switch away from the pseudo handle.
  // selfKind: 0 = that real handle, 1 = the pseudo handle; after kKmtRetry
  // seconds without one process answer the other kind is tried (each switch
  // logged with the status), so the share recovers whatever stops one kind.
  HANDLE self = nullptr;
  int selfKind = 0;
  int procFails = 0;  // consecutive samples without a process answer
  int allFails = 0;   // consecutive samples whose adapter query failed (KmtSample: false)
  NTSTATUS procStatus = 0;  // the last failed process query's status
  uint32_t switches = 0;
};
Kmt g_kmt;
constexpr int kKmtRetry = 5;
constexpr uint32_t kKmtProbe = 60;
std::atomic<uint64_t> g_kmtReopens{0};  // D3DKMT re-opened after its adapter queries failed (LUID change)
uint32_t g_kmtSwitchLogs = 0;

// The handle the process queries use now.
inline HANDLE KmtSelf() { return g_kmt.self ? g_kmt.self : GetCurrentProcess(); }
// Opens this process's handle of kind `kind` (0: real, PROCESS_QUERY_INFORMATION;
// 1: the pseudo handle). A failed OpenProcess leaves the pseudo handle.
void KmtSetSelf(int kind) {
  if (g_kmt.self) CloseHandle(g_kmt.self);
  g_kmt.self = kind == 0 ? OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, GetCurrentProcessId()) : nullptr;
  g_kmt.selfKind = g_kmt.self ? 0 : 1;
}

bool KmtOpen(LUID luid) {
  if (g_kmt.self) CloseHandle(g_kmt.self);
  g_kmt = Kmt();
  HMODULE g = GetModuleHandleW(L"gdi32.dll");
  if (!g) g = LoadLibraryW(L"gdi32.dll");
  if (!g) return false;
  g_kmt.qs = reinterpret_cast<Kmt::QsFn>(GetProcAddress(g, "D3DKMTQueryStatistics"));
  auto open = reinterpret_cast<Kmt::OpenFn>(GetProcAddress(g, "D3DKMTOpenAdapterFromLuid"));
  auto info = reinterpret_cast<Kmt::InfoFn>(GetProcAddress(g, "D3DKMTQueryAdapterInfo"));
  auto close = reinterpret_cast<Kmt::CloseFn>(GetProcAddress(g, "D3DKMTCloseAdapter"));
  if (!g_kmt.qs) return false;
  D3DKMT_QUERYSTATISTICS q{};
  q.Type = D3DKMT_QUERYSTATISTICS_ADAPTER;
  q.AdapterLuid = luid;
  if (g_kmt.qs(&q) != 0) return false;
  const ULONG n = q.QueryResult.AdapterInformation.NodeCount;
  if (!n || n > 256) return false;
  D3DKMT_OPENADAPTERFROMLUID o{};
  o.AdapterLuid = luid;
  if (open && info && open(&o) == 0) {
    for (ULONG i = 0; i < n; ++i) {
      D3DKMT_NODEMETADATA m{};
      m.NodeOrdinalAndAdapterIndex = i;
      D3DKMT_QUERYADAPTERINFO qi{};
      qi.hAdapter = o.hAdapter;
      qi.Type = KMTQAITYPE_NODEMETADATA;
      qi.pPrivateDriverData = &m;
      qi.PrivateDriverDataSize = sizeof(m);
      if (info(&qi) == 0 && m.NodeData.EngineType == DXGK_ENGINE_TYPE_3D) g_kmt.nodes.push_back(i);
    }
    if (close) {
      D3DKMT_CLOSEADAPTER c{};
      c.hAdapter = o.hAdapter;
      close(&c);
    }
  }
  if (g_kmt.nodes.empty()) g_kmt.nodes.push_back(0);
  g_kmt.luid = luid;
  KmtSetSelf(0);
  g_kmt.lastAll.assign(g_kmt.nodes.size(), 0);
  g_kmt.lastProc.assign(g_kmt.nodes.size(), 0);
  g_kmt.haveAll.assign(g_kmt.nodes.size(), 0);
  g_kmt.haveProc.assign(g_kmt.nodes.size(), 0);
  g_kmt.ok = true;
  return true;
}

// Busy percent of the busiest 3D node since the previous call, whole adapter
// and this process; false on the first call or a failed query.
bool KmtSample(double* all, double* proc) {
  if (!g_kmt.ok) return false;
  LARGE_INTEGER now;
  QueryPerformanceCounter(&now);
  LARGE_INTEGER fq;
  QueryPerformanceFrequency(&fq);
  const double wall100ns = g_kmt.lastQpc ? (now.QuadPart - g_kmt.lastQpc) * 1e7 / static_cast<double>(fq.QuadPart) : 0;
  g_kmt.lastQpc = now.QuadPart;
  double bestAll = -1, bestProc = -1;
  bool valid = wall100ns > 0;
  bool procAnswered = false;
  const bool probeNow = g_kmt.procUnsupported && ++g_kmt.procProbe >= kKmtProbe;
  if (probeNow) g_kmt.procProbe = 0;
  for (size_t k = 0; k < g_kmt.nodes.size(); ++k) {
    D3DKMT_QUERYSTATISTICS q{};
    q.Type = D3DKMT_QUERYSTATISTICS_NODE;
    q.AdapterLuid = g_kmt.luid;
    q.QueryNode.NodeId = g_kmt.nodes[k];
    if (g_kmt.qs(&q) != 0) {
      ++g_kmt.allFails;  // SampleSecond re-opens after kKmtRetry (the adapter's LUID may have changed)
      return false;
    }
    const LONGLONG a = q.QueryResult.NodeInformation.GlobalInformation.RunningTime.QuadPart;
    D3DKMT_QUERYSTATISTICS p{};
    p.Type = D3DKMT_QUERYSTATISTICS_PROCESS_NODE;
    p.AdapterLuid = g_kmt.luid;
    p.hProcess = KmtSelf();
    p.QueryProcessNode.NodeId = g_kmt.nodes[k];
    const bool askProc = !g_kmt.procUnsupported || probeNow;
    const NTSTATUS ps = askProc ? g_kmt.qs(&p) : static_cast<NTSTATUS>(0xC000000DL);
    if (ps != 0) g_kmt.procStatus = ps;
    procAnswered |= ps == 0;
    const LONGLONG b = ps == 0 ? p.QueryResult.ProcessNodeInformation.RunningTime.QuadPart : 0;
    if (!g_kmt.haveAll[k]) valid = false;
    if (valid) {
      // A counter that went back (reset / wrap) gives no busy value for this node in this sample.
      if (a >= g_kmt.lastAll[k])
        bestAll = (std::max)(bestAll, 100.0 * static_cast<double>(a - g_kmt.lastAll[k]) / wall100ns);
      if (ps == 0 && g_kmt.haveProc[k] && b >= g_kmt.lastProc[k])
        bestProc = (std::max)(bestProc, 100.0 * static_cast<double>(b - g_kmt.lastProc[k]) / wall100ns);
    }
    g_kmt.lastAll[k] = a;
    g_kmt.haveAll[k] = 1;
    g_kmt.lastProc[k] = b;
    g_kmt.haveProc[k] = ps == 0;
  }
  g_kmt.allFails = 0;
  if (procAnswered) {
    g_kmt.procFails = 0;
    g_kmt.procEver = true;
    if (g_kmt.procUnsupported) {
      g_kmt.procUnsupported = false;
      Log("perf: DCS's GPU share query answers again (%s handle)", g_kmt.selfKind == 0 ? "PROCESS_QUERY_INFORMATION" : "pseudo");
    }
  } else if (!g_kmt.procUnsupported && ++g_kmt.procFails >= kKmtRetry) {
    if (!g_kmt.procEver && g_kmt.switches >= 1) {
      // Neither handle kind ever answered (STATUS_INVALID_PARAMETER: the driver keeps no statistics for a
      // process without a device on this adapter). Degrade: the share n/a, probed once a minute, no switching.
      g_kmt.procUnsupported = true;
      g_kmt.procFails = 0;
      g_kmt.procProbe = 0;
      Log("perf: DCS's GPU share is not available (NTSTATUS 0x%08lx with both handle kinds; the driver answers "
          "per-process statistics only for a process with a device on this adapter); shown as n/a, probed every %u s",
          static_cast<unsigned long>(g_kmt.procStatus), kKmtProbe);
    } else {
      // No process answer for kKmtRetry samples: the other handle kind (its
      // first sample primes again).
      const int from = g_kmt.selfKind;
      KmtSetSelf(1 - from);
      g_kmt.procFails = 0;
      ++g_kmt.switches;
      g_kmt.haveProc.assign(g_kmt.nodes.size(), 0);
      if (g_kmtSwitchLogs < 8) {
        ++g_kmtSwitchLogs;
        Log("perf: DCS's GPU share query failed for %d s (NTSTATUS 0x%08lx with the %s handle); now using the %s "
            "handle",
            kKmtRetry, static_cast<unsigned long>(g_kmt.procStatus), from == 0 ? "PROCESS_QUERY_INFORMATION" : "pseudo",
            g_kmt.selfKind == 0 ? "PROCESS_QUERY_INFORMATION" : "pseudo");
      }
    }
  }
  if (!valid) return false;
  *all = (std::min)(100.0, (std::max)(0.0, bestAll));
  *proc = bestProc < 0 ? -1 : (std::min)(100.0, bestProc);
  return true;
}

// ---------------------------------------------------------------------------
// Per-second samples
// ---------------------------------------------------------------------------
struct Sec {
  double t = 0;  // seconds since the monitor started (QPC)
  uint32_t frames = 0;
  double fps = 0;  // frames / the second's exact length
  double renderPct = -1, gpuPct = -1, dcsPct = -1;
  bool engineOff = false;
  int verdict = perfgov::kNone;
  int stage = 0;
  perfgov::Cap cap;  // the frame-rate cap over the last 5 s (fps 0 = none)
};
constexpr int kSecs = 128;
Sec g_sec[kSecs];
int g_secCount = 0;  // total written

const Sec& SecAt(int i) { return g_sec[i % kSecs]; }

struct Agg {
  int n = 0;
  uint32_t frames = 0;
  double seconds = 0, rt = 0, gpu = 0, dcs = 0;
  int rtN = 0, gpuN = 0, dcsN = 0;
  int verdicts[5] = {};
  double Fps() const { return seconds > 0 ? frames / seconds : 0; }
  double Rt() const { return rtN ? rt / rtN : -1; }
  double Gpu() const { return gpuN ? gpu / gpuN : -1; }
  double Dcs() const { return dcsN ? dcs / dcsN : -1; }
};
// Samples with t in (t0, t1], optionally only those with engineOff == off.
Agg Aggregate(double t0, double t1, int off = -1) {
  Agg a;
  for (int i = (std::max)(0, g_secCount - kSecs); i < g_secCount; ++i) {
    const Sec& s = SecAt(i);
    if (s.t <= t0 || s.t > t1 || (off >= 0 && s.engineOff != (off != 0))) continue;
    ++a.n;
    a.frames += s.frames;
    a.seconds += 1.0;
    if (s.renderPct >= 0) a.rt += s.renderPct, ++a.rtN;
    if (s.gpuPct >= 0) a.gpu += s.gpuPct, ++a.gpuN;
    if (s.dcsPct >= 0) a.dcs += s.dcsPct, ++a.dcsN;
    if (s.verdict >= 0 && s.verdict < 5) a.verdicts[s.verdict]++;
  }
  return a;
}

// Most frequent verdict among those with frames; kNone when there are none.
int Majority(const Agg& a) {
  int best = perfgov::kNone, bestN = 0;
  for (int v = 1; v < 5; ++v)
    if (a.verdicts[v] > bestN) best = v, bestN = a.verdicts[v];
  return best;
}

std::string Pct(double v) {
  if (v < 0) return "n/a";
  char b[16];
  snprintf(b, sizeof(b), "%.0f%%", v);
  return b;
}

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------
int64_t g_qpc0 = 0;
double g_lastSecT = 0, g_lastPerfT = 0, g_lastVramT = 0, g_lastFeatT = -1e9, g_lastFeatFullT = -1e9;
double g_lastEnvT = -1e9, g_lastRollT = 0;
perfgov::CapTracker g_capTrack;
uint64_t g_framesCapAt = 0;
bool g_capHoldLogged = false;
uint64_t g_framesAt = 0, g_framesPerfAt = 0;
HANDLE g_rt = nullptr;
DWORD g_rtTid = 0;
ULONG64 g_rtCycles = 0;
bool g_rtHave = false;
int g_gpuSource = 0;  // 0 none, 1 D3DKMT, 2 NVML
bool g_kmtTried = false;
double g_kmtRetryT = -1e9;  // the last D3DKMT re-open attempt
bool g_gpuLogged = false, g_viewsLogged = false;
int g_viewsRecognised = -1;
double g_firstFrameT = -1;
bool g_noFramesLogged = false;
std::string g_lastFeatures;
// Warm-up.
bool g_warm = false;
double g_warmT0 = 0;
uint32_t g_warmHits0 = 0, g_warmCompiled0 = 0;
// Toggle quantification.
double g_toggleT = -1;
bool g_toggleOff = false;
// Governor.
perfgov::Governor g_gov;
bool g_govAnnounced = false;
uint32_t g_govUsable = 1;
std::string g_envLine, g_gpuLine, g_viewsLine;

double Now() {
  LARGE_INTEGER q;
  QueryPerformanceCounter(&q);
  return (q.QuadPart - g_qpc0) * g_qpcToUs / 1e6;
}

// ---------------------------------------------------------------------------
// env lines
// ---------------------------------------------------------------------------
void LogEnvStart() {
  const std::string ver = envinfo::ExeVersion();
  std::string head = envinfo::FirstLineWith(g_dir + L"..\\..\\Logs\\dcs.log", "DCS/");
  if (head.size() > 120) head.resize(120);
  SYSTEM_INFO si{};
  GetSystemInfo(&si);
  char b[1800];
  snprintf(b, sizeof(b),
           "env: DCS %s%s%s%s; CPU %s, %lu logical; OpenXR runtime %s; API layers: %s; streamers/runtimes running: %s",
           ver.empty() ? "?" : ver.c_str(), head.empty() ? "" : " (dcs.log: ", head.c_str(), head.empty() ? "" : ")",
           envinfo::CpuBrand().c_str(), si.dwNumberOfProcessors, envinfo::XrRuntime().c_str(),
           envinfo::XrLayers().c_str(), envinfo::Streamers().c_str());
  g_envLine = b;
  Log("%s", b);
}

void LogGpuEnv() {
  const Vram v = ReadVram();
  char b[700];
  snprintf(b, sizeof(b),
           "env: GPU %s (%s, vendor 0x%04x), driver %s, dedicated %.0f MB, VRAM budget %.0f MB, DCS uses %.0f MB; "
           "DriverCommandLists=%s; feature level %x; GPU busy from %s",
           g_ad.haveDesc ? envinfo::Utf8(g_ad.desc.Description).c_str() : "?",
           g_ad.fromDcs ? "DCS's device" : "first adapter, DCS's device not seen",
           g_ad.haveDesc ? g_ad.desc.VendorId : 0, g_ad.driver.empty() ? "?" : g_ad.driver.c_str(),
           g_ad.haveDesc ? g_ad.desc.DedicatedVideoMemory / 1048576.0 : 0.0, v.ok ? v.budget / 1048576.0 : 0.0,
           v.ok ? v.usage / 1048576.0 : 0.0,
           g_ad.commandLists < 0 ? "?" : g_ad.commandLists ? "1" : "0 (the recorders do not run)",
           static_cast<unsigned>(g_ad.featureLevel),
           g_gpuSource == 1 ? "D3DKMT 3D engine running time" : g_gpuSource == 2 ? "NVML" : "nothing (no GPU verdict)");
  g_gpuLine = b;
  Log("%s", b);
}

void LogViews(int views, int pairs) {
  char b[600];
  const bool quad = pairs >= 2;
  snprintf(b, sizeof(b),
           "env: views: %d per frame in DCS's main collect call (%u collections), %d focus/peripheral pairs: quad "
           "views recognised %s%s; render thread %s",
           views, perfgov::g_mainCount.load() ? perfgov::g_mainCount.load() : perfgov::g_maxCount.load(), pairs,
           quad ? "yes" : "NO",
           quad ? ""
                : " (stereo or an unrecognised layout: the A/B bench, the quad statistics and the G-buffer recorder's "
                  "cockpit filter need quad views)",
           RenderTid() ? "known (the pass hooks' thread; busy % is measured on it)"
                       : "not known yet (no pass hook ran: no busy %, no verdict)");
  g_viewsLine = b;
  Log("%s", b);
}

// ---------------------------------------------------------------------------
// features line
// ---------------------------------------------------------------------------
const char* const kFeatName[18] = {"AllocSlabs",  "BigPages",      "FrameHeap",   "TriCounter",   "TaskClock",
                                   "TimerCache",  "TexDedupe",     "CbSkip",      "CostWeights",  "PartitionBoost",
                                   "ShadowInst",  "ShadowBatch",   "PlanAsync",   "ShadowTexSkip", "ShadowRec",
                                   "GBufferRec",  "SplitFilter",   "Prefetch"};

std::string FeatureText() {
  int on = 0, conf = 0;
  std::string s;
  for (int i = 0; i < 18; ++i) {
    const qvstatus::Feature& f = g_featLast[i];
    conf += f.configured;
    const char* st;
    std::string why;
    if (!f.configured) {
      st = "off";
      why = "ini";
    } else if (f.latched) {
      st = "latched";
    } else if (g_engineOff.load()) {
      st = "off";
      why = "killswitch";
    } else if (f.active) {
      st = "on";
      ++on;
    } else {
      st = "idle";
      const bool rec = i == 14 || i == 15;
      const bool keys = i >= 10 && i <= 15;
      if (rec && g_govStage.load() >= 3)
        why = "governor";
      else if (g_benchRunningFlag.load())
        why = "bench";
      else if (keys && g_warming.load())
        why = "warming up";
      else if (rec && !(i == 14 ? shrec::Ready() : gbrec::Ready()))
        why = "starting";
      else if (rec)
        why = "no passes";
      else
        why = "waiting";
    }
    s += " ";
    s += kFeatName[i];
    s += "=";
    s += st;
    if (!why.empty()) s += "(" + why + ")";
  }
  char b[64];
  snprintf(b, sizeof(b), "features: %d of %d configured on;", on, conf);
  return b + s;
}

// ---------------------------------------------------------------------------
// Warm-up (shader variants)
// ---------------------------------------------------------------------------
void WarmTick(double t) {
  if (shadowinst::g_state.load() != 1) return;
  const shadowinst::Totals tot = shadowinst::Snap();
  const bool busy = tot.queued + tot.busy > 0;
  uint32_t hits = 0, compiled = 0;
  {
    std::lock_guard<std::mutex> lock(shadowinst::g_cacheMutex);
    hits = shadowinst::g_cacheHits;
    compiled = shadowinst::g_cacheCompiled;
  }
  if (busy && !g_warm) {
    g_warm = true;
    g_warmT0 = t;
    g_warmHits0 = hits;
    g_warmCompiled0 = compiled;
    g_warming = true;
    Log("warmup: shader variants compiling (%u keys queued, %u in progress); shadow instancing, batching, the texture "
        "skip and the recorders are only partly active until done",
        tot.queued, tot.busy);
  } else if (!busy && g_warm) {
    g_warm = false;
    g_warming = false;
    Log("warmup: done after %.0f s (cache: %u hits, %u compiled in this batch; %u hits, %u compiled since start)",
        t - g_warmT0, hits - g_warmHits0, compiled - g_warmCompiled0, hits, compiled);
  }
}

// ---------------------------------------------------------------------------
// Kill switch quantification
// ---------------------------------------------------------------------------
std::string AggText(const Agg& a) {
  char b[200];
  if (!a.n || !a.frames) return "no frames";
  snprintf(b, sizeof(b), "fps %.1f (%d s), render-thread busy %s, gpu %s, verdict %s", a.Fps(), a.n,
           Pct(a.Rt()).c_str(), Pct(a.Gpu()).c_str(), perfgov::VerdictName(Majority(a)));
  return b;
}

void OnToggle(bool off) {
  const double t = Now();
  if (g_toggleT >= 0) {  // the previous toggle's "after" line was not written yet
    const Agg a = Aggregate(g_toggleT + 2, t, g_toggleOff ? 1 : 0);
    Log("toggle: after %.0f s with the engine %s (toggled again early): %s", t - g_toggleT, g_toggleOff ? "OFF" : "ON",
        AggText(a).c_str());
  }
  const Agg before = Aggregate(t - 10.5, t, off ? 0 : 1);
  Log("toggle: engine %s; the 10 s before (engine %s): %s", off ? "OFF" : "ON", off ? "ON" : "OFF",
      AggText(before).c_str());
  g_toggleT = t;
  g_toggleOff = off;
  g_gov.Clear();  // the verdicts before the toggle describe the other state
}

void ToggleTick(double t) {
  if (g_toggleT < 0 || t < g_toggleT + 12.5) return;
  const Agg a = Aggregate(g_toggleT + 2, g_toggleT + 12.5, g_toggleOff ? 1 : 0);
  const Agg b = Aggregate(g_toggleT - 10.5, g_toggleT, g_toggleOff ? 0 : 1);
  char d[64] = "";
  if (a.frames && b.frames && b.Fps() > 0) snprintf(d, sizeof(d), " (fps %+.1f%%)", 100.0 * (a.Fps() / b.Fps() - 1));
  Log("toggle: the 10 s after (engine %s, 2 s settle): %s%s", g_toggleOff ? "OFF" : "ON", AggText(a).c_str(), d);
  g_toggleT = -1;
}

// ---------------------------------------------------------------------------
// Governor
// ---------------------------------------------------------------------------
bool ShadowRecGovernable() {
  return g_cfg.shadowRecorder && !(g_cfg.gbufferBatch && g_cfg.shadowInst) && !shrec::g_disabled.load() &&
         shrec::g_state.load() >= 0;
}
bool GBufferRecGovernable() {
  return g_cfg.gbufferRecorder && g_cfg.shadowInst && !g_cfg.gbufferBatch && !gbrec::g_disabled.load() &&
         gbrec::g_state.load() >= 0;
}

std::string StageText(int stage) {
  const perfgov::StageSettings s = perfgov::ForStage(stage, g_cfg.shadowRecorderSplit,
                                                     static_cast<uint32_t>(g_cfg.gbufferRecorderHelpers));
  char b[200];
  if (s.pause)
    snprintf(b, sizeof(b), "stage %d: both recorders paused (DCS draws shadows and the G-buffer itself)", stage);
  else
    snprintf(b, sizeof(b), "stage %d: ShadowRec split 0x%x, GBufferRec helpers %u%s", stage, s.shadowSplit, s.gbHelpers,
             stage == 0 ? " (as configured)" : "");
  return b;
}

void SetStage(int stage) {
  g_govStage = stage;
  g_gov.stage = stage;
  ApplyShadowRecorder();
  ApplyGBufferRecorder();
}

// StartBench (worker): the benchmark measures the configured features.
void GovForBench() {
  if (g_govStage.load() == 0) return;
  Log("governor: back to stage 0 for the benchmark");
  SetStage(0);
  g_gov.Reset(Now());
}

std::string RecheckText() {
  if (g_gov.evRecheck <= 0) return std::string();
  char b[48];
  snprintf(b, sizeof(b), "; recheck %.1f", g_gov.evRecheck);
  return b;
}

void GovTick(const Sec& s) {
  if (g_perceptionOnly.load()) {  // [General] PerceptionOnly: no recorder to govern, no governor action
    g_gov.Reset(s.t);
    return;
  }
  perfgov::GovConfig c;
  c.enabled = g_cfg.govEnabled;
  c.windowSec = g_cfg.govWindowSec;
  c.needSec = g_cfg.govNeedSec;
  c.dwellSec = g_cfg.govDwellSec;
  c.minGainPct = g_cfg.govMinGainPct;
  c.trialSec = g_cfg.govTrialSec;
  c.recheckSec = g_cfg.govRecheckSec;
  c.revertBackoffSec = g_cfg.govRevertBackoffSec;
  const bool sh = ShadowRecGovernable(), gb = GBufferRecGovernable();
  g_govUsable = perfgov::UsableStages(sh, gb, g_cfg.shadowRecorderSplit,
                                      static_cast<uint32_t>(g_cfg.gbufferRecorderHelpers));
  if (!c.enabled) {
    if (g_govStage.load() != 0) {
      Log("governor: off ([Governor] Enabled=0): %s", StageText(0).c_str());
      SetStage(0);
    }
    // A trial in progress is dropped with its stage: re-enabled, it would
    // otherwise decide on seconds from before the pause.
    g_gov.Reset(s.t);
    g_govAnnounced = false;
    return;
  }
  if (!g_govAnnounced && (sh || gb)) {
    g_govAnnounced = true;
    Log("governor: on (GPU-bound: gpu >= %d%%, or >= %d%% with the render thread <= %d%% busy, in %d of %d s; back "
        "up when gpu < %d%% "
        "or render thread >= %d%%; at least %d s per stage; each step a paired fps trial: %d s before, %d s after, %d s "
        "back at the old stage (2 s settle each), kept only beyond %.1f%%; a reverted stage blocked for %d s, "
        "doubling); usable stages 0x%x",
        g_cfg.govGpuFullPct, g_cfg.govGpuBusyPct, g_cfg.govRenderIdlePct, c.needSec, c.windowSec, g_cfg.govGpuFreePct,
        g_cfg.govCpuBusyPct, c.dwellSec, c.trialSec, c.trialSec, c.recheckSec, c.minGainPct, c.revertBackoffSec,
        g_govUsable);
  }
  // Capped frame rate: fps cannot change, so no fps trial means anything: hold the stage.
  if (s.cap.fps > 0) {
    if (!g_capHoldLogged)
      Log("governor: capped at %.1f fps (%d Hz / %d): governor holding %s", s.cap.fps, s.cap.hz, s.cap.div,
          StageText(g_gov.stage).c_str());
    g_capHoldLogged = true;
    g_gov.Reset(s.t);
    return;
  }
  if (g_capHoldLogged) {
    g_capHoldLogged = false;
    Log("governor: frame rate no longer capped; governing again");
  }
  // Frozen: kill switch, benchmark, nothing to govern. The window restarts afterwards.
  if (g_engineOff.load() || g_benchRunningFlag.load() || !(sh || gb)) {
    g_gov.Reset(s.t);
    return;
  }
  const int from = g_gov.stage;
  const double held = s.t - g_gov.lastChange;
  const double fps = s.frames ? s.fps : -1.0;
  const int d = g_gov.Tick(static_cast<perfgov::Verdict>(s.verdict), fps, s.t, c, g_govUsable);
  const int to = g_gov.stage;
  if (d) SetStage(to);
  const double gain = g_gov.evFpsBefore > 0 ? 100.0 * (g_gov.evFpsAfter / g_gov.evFpsBefore - 1.0) : 0.0;
  switch (g_gov.lastEvent) {
    case perfgov::kEvDown:
      Log("governor: GPU-bound in %d of the last %d s (gpu %s, render thread %s busy) -> %s%s", g_gov.lastGpu,
          g_gov.lastN, Pct(s.gpuPct).c_str(), Pct(s.renderPct).c_str(), StageText(to).c_str(),
          g_gov.trial ? "; trial: kept only if fps rises" : "");
      break;
    case perfgov::kEvUp:
      Log("governor: not GPU-bound in %d of the last %d s (gpu %s, render thread %s busy) -> %s (after %.0f s in stage "
          "%d%s%s)",
          g_gov.lastFree, g_gov.lastN, Pct(s.gpuPct).c_str(), Pct(s.renderPct).c_str(), StageText(to).c_str(), held,
          from, to > 0 && g_gov.UpDwell(c, to) > c.dwellSec ? "; the next step up waits longer after a bounce" : "",
          g_gov.trial ? "; trial: reverted if fps falls" : "");
      break;
    case perfgov::kEvUnusable:
      Log("governor: stage %d no longer applies (a recorder is off or the ini changed) -> %s", from,
          StageText(to).c_str());
      break;
    case perfgov::kEvRecheck:
      Log("governor: step %d trial: fps %.1f before, %.1f after; rechecking stage %d for %d s", g_gov.evStage,
          g_gov.evFpsBefore, g_gov.evFpsAfter, to, c.recheckSec);
      break;
    case perfgov::kEvUnsteady:
      Log("governor: step %d trial without a verdict: the scene changed during it (fps %.1f before, %.1f after, "
          "%.1f on the recheck) -> %s; stage %d waits %.0f s",
          g_gov.evStage, g_gov.evFpsBefore, g_gov.evFpsAfter, g_gov.evRecheck, StageText(to).c_str(),
          g_gov.evWaitStage, g_gov.evBlockSec);
      break;
    case perfgov::kEvKept:
      Log("governor: step %d kept: fps %.1f -> %.1f (%+.1f%% against the before/recheck reference%s) -> %s",
          g_gov.evStage, g_gov.evFpsBefore, g_gov.evFpsAfter, gain, RecheckText().c_str(), StageText(to).c_str());
      break;
    case perfgov::kEvInconclusive:
      Log("governor: step %d kept without an fps verdict (too few frames in its trial)", g_gov.evStage);
      break;
    case perfgov::kEvRevertDown:
    case perfgov::kEvRevertUp:
      Log("governor: step %d reverted: fps %.1f -> %.1f (%+.1f%%%s, %s%.1f%%) -> %s; stage %d blocked for %.0f s",
          g_gov.evStage, g_gov.evFpsBefore, g_gov.evFpsAfter, gain, RecheckText().c_str(),
          g_gov.lastEvent == perfgov::kEvRevertDown ? "a step down needs more than +" : "a step up may lose at most ",
          c.minGainPct, StageText(to).c_str(), g_gov.evStage, g_gov.evBlockSec);
      break;
    default:
      break;
  }
}

// ---------------------------------------------------------------------------
// Per-second tick (worker)
// ---------------------------------------------------------------------------
void SampleSecond(double t) {
  Sec s;
  s.t = t;
  const double dt = t - g_lastSecT;
  g_lastSecT = t;
  const uint64_t fr = perfgov::g_frames.load(std::memory_order_acquire);
  s.frames = static_cast<uint32_t>(fr - g_framesAt);
  g_framesAt = fr;
  // Render thread busy.
  const DWORD tid = RenderTid();
  if (tid && tid != g_rtTid) {
    if (g_rt) CloseHandle(g_rt);
    g_rt = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, tid);
    g_rtTid = tid;
    g_rtHave = false;
  }
  ULONG64 cyc = 0;
  if (g_rt && QueryThreadCycleTime(g_rt, &cyc)) {
    if (g_rtHave && dt > 0.2 && g_tscHz > 0 && cyc >= g_rtCycles)
      s.renderPct = (std::min)(100.0, 100.0 * (cyc - g_rtCycles) / g_tscHz / dt);
    g_rtCycles = cyc;
    g_rtHave = true;
  }
  // GPU busy.
  if (!g_kmtTried && g_ad.haveDesc) {
    g_kmtTried = true;
    if (KmtOpen(g_ad.desc.AdapterLuid))
      g_gpuSource = 1;
    else if (gpu::Init())
      g_gpuSource = 2;
  }
  if (g_gpuSource == 1) {
    double a = -1, p = -1;
    if (KmtSample(&a, &p)) {
      s.gpuPct = a;
      s.dcsPct = p;
    } else if ((!g_kmt.ok || g_kmt.allFails >= kKmtRetry) && t - g_kmtRetryT >= kKmtRetry) {
      // The adapter queries keep failing (or a re-open failed): DCS's adapter
      // again (a driver reset gives it a new LUID), D3DKMT opened for it;
      // tried every kKmtRetry seconds.
      g_kmtRetryT = t;
      const LUID was = g_kmt.luid;
      OpenDcsAdapter();
      if (KmtOpen(g_ad.desc.AdapterLuid)) {
        g_kmtReopens++;
        Log("perf: GPU statistics re-opened (adapter LUID %08lx:%08lx -> %08lx:%08lx)",
            static_cast<unsigned long>(was.HighPart), was.LowPart,
            static_cast<unsigned long>(g_ad.desc.AdapterLuid.HighPart), g_ad.desc.AdapterLuid.LowPart);
      }
    }
  } else if (g_gpuSource == 2) {
    const int u = gpu::Utilization();
    if (u >= 0) s.gpuPct = u;
  }
  s.engineOff = g_engineOff.load();
  perfgov::Thresholds th;
  th.gpuBusyPct = g_cfg.govGpuBusyPct;
  th.renderIdlePct = g_cfg.govRenderIdlePct;
  th.cpuBusyPct = g_cfg.govCpuBusyPct;
  th.gpuFreePct = g_cfg.govGpuFreePct;
  th.gpuFullPct = g_cfg.govGpuFullPct;
  perfgov::Second sec;
  s.fps = dt > 0 ? s.frames / dt : 0;
  sec.frames = static_cast<uint32_t>(s.fps + 0.5);
  sec.renderPct = s.renderPct;
  sec.gpuPct = s.gpuPct;
  s.verdict = perfgov::Classify(sec, th);
  // Frame-rate cap: pinned at refresh/N, neither the GPU nor the render thread decides fps (verdict capped).
  {
    std::vector<float> iv;
    g_framesCapAt = perfgov::ReadIntervals(g_framesCapAt, iv);
    g_capTrack.Add(iv);
    s.cap = g_capTrack.last;
    if (s.cap.fps > 0 && s.verdict != perfgov::kNone) s.verdict = perfgov::kCapped;
  }
  s.stage = g_govStage.load();
  g_sec[g_secCount % kSecs] = s;
  ++g_secCount;
  // The verdict the status word shows: majority of the last 8 s.
  g_verdictNow = Majority(Aggregate(t - 8.5, t));
  GovTick(s);
}

void LogVram() {
  const Vram v = ReadVram();
  if (v.ok && v.budget)
    Log("vram: DCS uses %.0f of %.0f MB local budget (%.0f%%), non-local %.0f MB%s", v.usage / 1048576.0, v.budget / 1048576.0,
        100.0 * v.usage / v.budget, v.nlUsage / 1048576.0,
        v.usage * 10 >= v.budget * 9 ? "   <-- near the budget (textures may be demoted or paged)" : "");
}

void PerfLine(double t) {
  const double secs = t - g_lastPerfT;
  g_lastPerfT = t;
  std::vector<float> iv;
  iv.reserve(4096);
  const uint64_t from = g_framesPerfAt;
  g_framesPerfAt = perfgov::ReadIntervals(g_framesPerfAt, iv);
  const uint64_t frames = g_framesPerfAt - from;  // fps from the frame counter itself (not the 1-s samples)
  const Agg a = Aggregate(t - secs, t);  // the seconds sampled since the last line (the same Tick wrote both)
  if (!a.frames || iv.empty()) {
    if (!g_noFramesLogged) Log("perf: no frames in the last %.0f s (menu, loading or the mission is paused)", secs);
    g_noFramesLogged = true;
    return;
  }
  g_noFramesLogged = false;
  std::sort(iv.begin(), iv.end());
  auto q = [&](double p) { return iv[(std::min)(iv.size() - 1, static_cast<size_t>(p * iv.size()))]; };
  const int v = Majority(a);
  // A frame-rate cap over the whole window wins over the thresholds (the GPU and the render thread have slack).
  const perfgov::Cap cap = perfgov::DetectCap(iv);
  char vtext[64];
  if (cap.fps > 0)
    snprintf(vtext, sizeof(vtext), "capped %.1f fps = %d/%d", cap.fps, cap.hz, cap.div);
  else
    snprintf(vtext, sizeof(vtext), "%s", perfgov::VerdictName(v));
  Log("perf: fps=%.1f ft p50/p95=%.1f/%.1f ms render-thread busy=%s gpu util=%s (dcs %s) verdict=%s (%d s GPU, %d CPU, "
      "%d capped, %d mixed of %d) engine=%s governor=%d views=%d pairs=%d",
      frames / (secs > 0 ? secs : 1.0), q(0.5), q(0.95), Pct(a.Rt()).c_str(), Pct(a.Gpu()).c_str(),
      Pct(a.Dcs()).c_str(), vtext, a.verdicts[perfgov::kGpu], a.verdicts[perfgov::kCpu],
      a.verdicts[perfgov::kCapped], a.verdicts[perfgov::kMixed], a.n, g_engineOff.load() ? "off" : "on",
      g_govStage.load(), perfgov::g_views.load(), perfgov::g_pairs.load());
}

void Start() {
  perfgov::Init();
  LARGE_INTEGER q;
  QueryPerformanceCounter(&q);
  g_qpc0 = q.QuadPart;
  LogEnvStart();
  g_lastEnvT = 0;
}

void Tick() {
  const double t = Now();
  if (t - g_lastSecT < 1.0) return;
  // GPU and adapter: DCS's device once a feature saw it; the first adapter after 30 s of frames without it.
  if (!g_gpuLogged) {
    const bool frames = perfgov::g_frames.load() > 0;
    if (frames && g_firstFrameT < 0) g_firstFrameT = t;
    if (OpenDcsAdapter() || (frames && t - g_firstFrameT > 30 && OpenFirstAdapter())) {
      g_gpuLogged = true;
      SampleSecond(t);  // opens D3DKMT, so the line names the GPU source
      LogGpuEnv();
      g_lastVramT = t;
      return;
    }
  }
  SampleSecond(t);
  WarmTick(t);
  ToggleTick(t);
  if (t - g_lastRollT >= 10) {  // the main-call size follows the scene (sampler window)
    g_lastRollT = t;
    perfgov::RollWindow();
  }
  // View layout: 5 s after the first frames, and when recognition changes.
  const int pairs = perfgov::g_pairs.load(), views = perfgov::g_views.load();
  if (g_firstFrameT >= 0 && t - g_firstFrameT > 5 && views >= 0) {
    const int rec = pairs >= 2 ? 1 : 0;
    if (!g_viewsLogged || rec != g_viewsRecognised) {
      g_viewsLogged = true;
      g_viewsRecognised = rec;
      LogViews(views, pairs);
    }
  }
  const int perfSec = g_cfg.perfIntervalSec;
  if (perfSec > 0 && t - g_lastPerfT >= perfSec - 0.01) PerfLine(t);
  // Features: on change (at most every 10 s) and every 60 s.
  if (perfSec > 0 && t - g_lastFeatT >= 10) {
    std::string f = FeatureText();
    if (f != g_lastFeatures || t - g_lastFeatFullT >= 60) {
      Log("%s", f.c_str());
      g_lastFeatures = f;
      g_lastFeatT = g_lastFeatFullT = t;
    }
  }
  const int vramSec = g_cfg.vramIntervalSec;
  if (vramSec > 0 && g_ad.a3 && t - g_lastVramT >= vramSec) {
    g_lastVramT = t;
    LogVram();
  }
  // Footer every 30 min: the env lines again, so a long log's tail still has them.
  if (t - g_lastEnvT >= 1800) {
    g_lastEnvT = t;
    if (!g_envLine.empty()) Log("%s (repeat)", g_envLine.c_str());
    if (!g_gpuLine.empty()) Log("%s (repeat)", g_gpuLine.c_str());
    if (!g_viewsLine.empty()) Log("%s (repeat)", g_viewsLine.c_str());
  }
}

void Shutdown() {
  if (g_rt) CloseHandle(g_rt);
  g_rt = nullptr;
  g_rtTid = 0;
  if (g_ad.a3) g_ad.a3->Release();
  g_ad.a3 = nullptr;
  if (g_kmt.self) CloseHandle(g_kmt.self);
  g_kmt.self = nullptr;
}

}  // namespace perfmon
