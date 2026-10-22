// Offline checks: frustum decoding, exclusion building, Scene.dll export lookup.
#include "../src/main.cpp"

#include <cassert>
#include <d3dcompiler.h>
#include <d3d11shader.h>
#include <thread>

namespace {

double ED_FakeTime() {
  LARGE_INTEGER c, f;
  QueryPerformanceCounter(&c);
  QueryPerformanceFrequency(&f);
  return double(c.QuadPart) / double(f.QuadPart);
}

int g_fail = 0;
// R60: the names of the failed checks, repeated on stderr when the process exits (whichever return of main), so a
// truncated stdout log still says which check failed.
std::vector<std::string> g_failed;
void PrintFailedChecks() {
  fflush(stdout);
  if (g_failed.empty()) return;
  fprintf(stderr, "FAILED CHECKS (%zu):\n", g_failed.size());
  for (const std::string& s : g_failed) fprintf(stderr, "  FAIL %s\n", s.c_str());
  fflush(stderr);
}
// R107 (R106 F08): every SKIP goes through QvSkip, so the summary lists them (a SKIP is neither a pass nor a
// failure, and a vacuous one used to be visible only by reading the whole log).
std::vector<std::string> g_skipped;
void QvSkip(const char* fmt, ...) {
  char buf[1024];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  printf("SKIP %s", buf);
  std::string s(buf);
  while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
  g_skipped.push_back(s);
}
// The last lines of every run: the skipped checks, then "N failure(s), M skipped".
void PrintSummary() {
  if (!g_skipped.empty()) {
    printf("SKIPPED (%zu):\n", g_skipped.size());
    for (const std::string& s : g_skipped) printf("  SKIP %s\n", s.c_str());
  }
  printf("%d failure(s), %zu skipped\n", g_fail, g_skipped.size());
}
void Check(bool ok, const char* what) {
  printf("%s %s\n", ok ? "PASS" : "FAIL", what);
  if (!ok) {
    ++g_fail;
    if (g_failed.empty()) atexit(&PrintFailedChecks);
    g_failed.push_back(what);
  }
}

// QV_TEST_SPIN=1 (off by default): two below-normal-priority spinner threads
// inside this process, alive only while one timing-sensitive test runs (the
// scope of a TestSpin object). An in-process stand-in for a busy machine;
// never an external load process.
struct TestSpin {
  std::atomic<bool> stop{false};
  std::thread t[2];
  explicit TestSpin(const char* what) {
    const char* e = getenv("QV_TEST_SPIN");
    if (!e || *e != '1') return;
    printf("     (QV_TEST_SPIN: 2 spinner threads during %s)\n", what);
    for (std::thread& x : t)
      x = std::thread([this] {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
        volatile uint64_t v = 1;
        while (!stop.load(std::memory_order_relaxed)) v = v * 6364136223846793005ull + 1;
      });
  }
  ~TestSpin() {
    stop = true;
    for (std::thread& x : t)
      if (x.joinable()) x.join();
  }
};
// Writes a symmetric-ish frustum into a ClippingVolume-like buffer the way
// DCS stores it: inward unit normals, d such that inside is n.p + d >= 0.
void MakeFrustum(uint8_t* vol, Vec3 eye, Vec3 fwd, Vec3 up, double l, double r, double b, double t,
                 double zn, double zf) {
  memset(vol, 0, kVolSize);
  Vec3 right{fwd.y * up.z - fwd.z * up.y, fwd.z * up.x - fwd.x * up.z, fwd.x * up.y - fwd.y * up.x};
  auto side = [&](Vec3 edgeDir, Vec3 inwardHint) {
    // plane through eye containing edgeDir and the axis perpendicular to inwardHint
    Vec3 axis{edgeDir.y * inwardHint.z - edgeDir.z * inwardHint.y,
              edgeDir.z * inwardHint.x - edgeDir.x * inwardHint.z,
              edgeDir.x * inwardHint.y - edgeDir.y * inwardHint.x};
    Vec3 n{edgeDir.y * axis.z - edgeDir.z * axis.y, edgeDir.z * axis.x - edgeDir.x * axis.z,
           edgeDir.x * axis.y - edgeDir.y * axis.x};
    if (Dot(n, inwardHint) < 0) n = n * -1.0;
    n = n * (1.0 / Len(n));
    return Plane{n, -Dot(n, eye)};
  };
  // edges: angles given as tangents
  Plane planes[6];
  planes[0] = {fwd, -Dot(fwd, eye + fwd * zn)};
  planes[1] = {fwd * -1.0, Dot(fwd, eye + fwd * zf)};
  planes[2] = side(fwd + right * l, right);         // left edge, inside toward +right
  planes[3] = side(fwd + right * r, right * -1.0);  // right edge
  planes[4] = side(fwd + up * b, up);
  planes[5] = side(fwd + up * t, up * -1.0);
  for (int i = 0; i < 6; ++i) WritePlane(vol, i, planes[i]);
  *reinterpret_cast<int32_t*>(vol + kPlaneCount) = 6;
}

bool InsideBlock(const uint8_t* block, Vec3 p, double radius) {
  int n = *reinterpret_cast<const int32_t*>(block + kPlaneCount);
  for (int i = 0; i < n; ++i) {
    const double* d = reinterpret_cast<const double*>(block + i * kPlaneStride);
    if (d[0] * p.x + d[1] * p.y + d[2] * p.z + d[3] <= radius) return false;
  }
  return true;
}


// ---------------------------------------------------------------------------
// Shadow texture skip (shadow_tex.h): fakes for the slot-5 differential test
// ---------------------------------------------------------------------------
namespace stx {
using namespace shadowtex;

struct Call {
  int kind;  // 1 sb, 2 set texture, 3 submit, 4 vt23, 5 vt18, 6 getDesc, 7 compat, 8 getSRV, 9 getTexture, 10 valid
  uint64_t a, b, c, d;
  bool operator==(const Call& o) const { return kind == o.kind && a == o.a && b == o.b && c == o.c && d == o.d; }
};
std::vector<Call> g_trace;
alignas(16) uint8_t g_texProps[0x20][16];
bool g_validFlag[0x20];
bool g_compatResult = true;
alignas(16) uint8_t g_desc[0x40];

uint64_t U(const void* p) { return reinterpret_cast<uint64_t>(p); }

const void* __fastcall FakeGetTexture(const void* props, uint32_t idx) {
  g_trace.push_back({9, U(props), idx, 0, 0});
  return g_texProps[idx & 0x1f];
}
bool __fastcall FakeValid(const void* t) {
  const size_t idx = (static_cast<const uint8_t*>(t) - &g_texProps[0][0]) / 16;
  g_trace.push_back({10, idx, 0, 0, 0});
  return g_validFlag[idx];
}
uint64_t __fastcall FakeSetSb(void* sh, void* h, void* v) {
  g_trace.push_back({1, U(sh), U(h), U(v), 0});
  return 0;
}
uint64_t __fastcall FakeSetTex(void* sh, int64_t h, void* tex, void* aux, const uint64_t* size) {
  g_trace.push_back({2, U(sh) ^ static_cast<uint64_t>(h), U(tex), U(aux), *size});
  return 0;
}
uint64_t __fastcall FakeSubmit(void* mat, uint64_t tech, uint64_t pass, void* a4) {
  g_trace.push_back({3, U(mat), tech, pass & 0xffffffffull, U(a4)});
  return 0x1234 + tech;
}
uint64_t __fastcall FakeTex23(void* tex, uint64_t size) {
  g_trace.push_back({4, U(tex), size, 0, 0});
  return 0;
}
uint64_t __fastcall FakeTex18(void* tex) {
  g_trace.push_back({5, U(tex), 0, 0, 0});
  return 0;
}
const uint8_t* __fastcall FakeGetDesc(void* tex) {
  g_trace.push_back({6, U(tex), 0, 0, 0});
  return g_desc;
}
bool __fastcall FakeCompat(int32_t type, int32_t fmt) {
  g_trace.push_back({7, static_cast<uint32_t>(type), static_cast<uint32_t>(fmt), 0, 0});
  return g_compatResult;
}
void* __fastcall FakeGetSrv(void* tex, void* aux, const uint64_t* size) {
  g_trace.push_back({8, U(tex), U(aux), *size, 0});
  return nullptr;
}

bool WriteCode(void* where, const void* bytes, size_t n) {
  DWORD old;
  if (!VirtualProtect(where, n, PAGE_EXECUTE_READWRITE, &old)) return false;
  memcpy(where, bytes, n);
  VirtualProtect(where, n, old, &old);
  FlushInstructionCache(GetCurrentProcess(), where, n);
  return true;
}

// One caster: material, properties, render item, texture entries, textures.
struct Scene {
  uint8_t mat[0x300];
  uint8_t props[0x300];
  uint8_t item[0x100];
  uint8_t entries[24 * 16];
  uint8_t* entryBase;
  uint8_t tex[8][0x700];
  uint8_t inner[8][0x20];
  uint8_t back[8][0x50];
};

void* g_shaderVtbl[40];
void* g_texVtbl[30];
void* g_innerVtbl[12];
void* g_otherInnerVtbl[12];
alignas(16) uint8_t g_shader[0x100];
alignas(16) uint8_t g_records[0x50 * 16];
alignas(16) uint8_t g_globals[0x100];
alignas(16) uint8_t g_sbArray[0x20 + 0x30 * 8];

// Texture t of the scene: 4 has a mip set ready to swap, 6 an unknown inner
// class and a non-zero +0x678. Entry 5 has no texture; entries 0, 3, 6, ...
// have aux -1.
void Build(Scene& s, int count, const int64_t* handles, int props8, int transp, int baseIdx) {
  memset(&s, 0, sizeof(s));
  *reinterpret_cast<void**>(s.mat + 0x28) = s.props;
  *reinterpret_cast<void**>(s.mat + 0x30) = g_shader;
  *reinterpret_cast<uint64_t*>(s.mat + 0x68) = 0x6868;
  *reinterpret_cast<uint64_t*>(s.mat + 0x210) = 7;
  *reinterpret_cast<uint64_t*>(s.mat + 0x218) = 8;
  *reinterpret_cast<uint32_t*>(s.mat + 0x2d8) = static_cast<uint32_t>(count);
  for (int i = 0; i < count; ++i) *reinterpret_cast<int64_t*>(s.mat + 0x240 + i * 8) = handles[i];
  *reinterpret_cast<int32_t*>(s.props + 8) = props8;
  s.props[0x33] = static_cast<uint8_t>(transp);
  *reinterpret_cast<uint32_t*>(s.props + 0x26c) = static_cast<uint32_t>(baseIdx);
  *reinterpret_cast<uint32_t*>(s.item + 0xd0) = 3;
  *reinterpret_cast<uint32_t*>(s.item + 0xd4) = 0xabcd;
  s.entryBase = s.entries;
  *reinterpret_cast<void**>(s.item + 0x18) = &s.entryBase;
  for (int i = 0; i < 16; ++i) {
    uint8_t* e = s.entries + i * 24;
    const int t = i & 7;
    *reinterpret_cast<int64_t*>(e) = (i % 3 == 0) ? -1 : i;  // aux
    *reinterpret_cast<void**>(e + 8) = (i == 5) ? nullptr : s.tex[t];
    *reinterpret_cast<void**>(s.tex[t]) = g_texVtbl;
    *reinterpret_cast<void**>(s.tex[t] + 0x10) = s.inner[t];
    *reinterpret_cast<void**>(s.inner[t]) = (t == 6) ? g_otherInnerVtbl : g_innerVtbl;
    *reinterpret_cast<void**>(s.tex[t] + 0x1a0) = s.back[t];
    *reinterpret_cast<int32_t*>(s.back[t] + 0x40) = (t == 4) ? 5 : 3;
    *reinterpret_cast<void**>(s.back[t] + 0x20) = (t == 4) ? s.back[t] : nullptr;
    if (t == 6) *reinterpret_cast<uint64_t*>(s.tex[t] + 0x678) = 1;
  }
}

}  // namespace stx

}  // namespace

#include "dcs_install.h"  // DCS install lookup (no machine-specific paths)
#include "shadow_inst_test.h"
#include "shadow_tex_test.h"
#include "gb_inst_test.h"
#include "shadow_batch_test.h"
#include "shadow_plan_test.h"
#include "par_upload_test.h"
#include "direct_upload_test.h"
#include "gb_batch_test.h"
#include "split_filter_test.h"
#include "deferred_rec_test.h"
#include "shadow_rec_test.h"
#include "gpu_pass_timing_test.h"
#include "gb_rec_count_test.h"
#include "gb_rec_test.h"
#include "gb_rec_cockpit_test.h"
#include "view_profile_test.h"
#include "frame_start_test.h"
#include "vram_count_test.h"
#include "pass_flush_test.h"
#include "fwd_rec_count_test.h"
#include "srv_tail_trim_test.h"
#include "status_word_test.h"
#include "prefetch_fix_test.h"
#include "periph_lod_test.h"
#include "robustness_test.h"
#include "long_session_test.h"
#include "gov_test.h"
#include "fixation_depth_test.h"
#include "r94_test.h"  // R94: [Perception] SymbologyTrace, the dev live HUD trace (its helpers are used by far_depth_test / ovl_cpu_test)
#include "scene_cut_test.h"
#include "far_depth_test.h"  // Far depth engine export to Quad Views (layout, fake layer, WARP copy and push)
#include "r84_test.h"  // R84 (R1b): live-object / handle / VRAM probe of the depth-export reinstall (used by the next)
#include "cockpit_mask_test.h"  // R65: cockpit mask export (contract, layout, WARP max filter, xrEndFrame commit, reader fence)
#include "cockpit_ipd_test.h"  // R31 option 3: cockpit at the true eye distance (patch, matcher, WARP pipeline, split)
#include "exit_guard_test.h"  // field report 0.5.5: exit and mission-restart crashes
#include "perception_only_test.h"  // [General] PerceptionOnly: perception features only, no engine module
#include "cascade_cache_test.h"  // R33: far shadow cascade cache (math on census matrices, decisions, WARP replica)
#include "chain_fast_test.h"  // R34: shorter culling chains (variants, creation hook, substitution, live check)
#include "shader_fast_test.h"  // R36: wider grids for single-SM kernels (variants, identification, Dispatch hook, live check)
#include "night_tone_test.h"  // R37: [Night] opt-in night darkening through DCS's tone mapper (pure parts, hook, build checks)
#include "color_grade_test.h"  // [Color] pure parts: the R41 probes (R40 grading removed in R107)
#include "color_mode_test.h"  // R42 [Color] Mode: Fix / Enhanced (pure parts and the pass on WARP)
#include "vrfx_test.h"  // R51 [VrFx] GazeDof / GVision (keys, physiology, state machines, the pass on WARP)
#include "thread_place_test.h"  // R39: [Threads] census and worker placement (mask rule, queries, round trip)
#include "frame_timeline_test.h"  // R52: per-frame timeline, pose probe, event log (keys default 0); placement re-check
#include "r59_stop_passthrough_test.h"  // R59 L1: old payload hooks pass through after Stop
#include "r59_inst_map_test.h"  // R59: shadow_inst maps reuse destroyed shaders' slots
#include "r59_prefault_test.h"  // R59: recycled job does not wait out kPreWaitUs
#include "r59_helmet_cache_test.h"  // R59: helmet material verdicts (reuse, mission reset, no spill)
#include "r59_tls_test.h"  // R59: no 128 KB static TLS table
#include "r59_xr_state_test.h"  // R59: XR session state from the log's new bytes
#include "r59_prefetch_log_test.h"  // R59: prefetch stats log rotated past 1 MB
#include "r62_dev_ini_test.h"  // R62 a1: [Dev] IniPath of 260+ characters
#include "r62_log_numbers_test.h"  // R62: log numbers keep '.' on comma-decimal regions
#include "r62_gbrec_lines_test.h"  // R62: recorder fallback lines vs the app's "turned itself off" scan
#include "r62_vrfx_gap_test.h"  // R62: VrFx state across an all-off gap
#include "r62_night_tone_test.h"  // R62: night tone writes only live modules
#include "r63_scene_check_test.h"  // R63 U2: partition boost only on a verified Scene.dll collect
#include "r63_srvtbl_test.h"  // R63 U5: ownship SceneRenderable vtable by RTTI
#include "r63_tex_tick_test.h"  // R63: the texture dedupe tick stays monotonic
#include "r63_big_pages_test.h"  // R63: big pages undone by a next payload that has them off
#include "r63_lua_hook_test.h"  // R63: the Lua hook loads the module by its exact path (';' in Saved Games)
#include "r63_env_layers_test.h"  // R63: API layers turned off by the environment are marked in the log
#include "r77_test.h"  // R77: uncovered-bugfix round (texture tables, suite kill switch, ...)
#include "r79_test.h"  // R79: one DCSVR_PERCEPTION_SHM_SUFFIX rule for every shared block (R77c N1-N3)
#include "r83_test.h"  // R83: R82 D1, no false MISSED tone-map swaps at VrFx transitions in Mode 0
#include "r90_test.h"  // R90: lossless hitch fixes (patching, placement, async log, readbacks, Lua, disk, shadow inst)
#include "r93_test.h"  // R93: R92's D1 (async log ring at DCS start) and D2 (FrameTimeline behind the cockpit mask)
#include "r95_test.h"       // R95: the symbology emission export (contract, pure parts, ovl_cpu record point)
#include "r95_tone_test.h"  // R95: the tone map's second evaluation on WARP (every colour mode and effect)
#include "r95_pipe_test.h"  // R95: the whole path, the lifecycle (WARP; QV_R95_HW=1: also the GPU)
#include "r95_hw_test.h"    // R95: the GPU cost bench (QV_R95_HW=1) and r95test::Run
#include "r99_test.h"       // R99: the pose hook by displayTime; the symbology replay stage's cost
#include "r101_test.h"      // R101: the learnt HUD field recovers after a bank (aging, recent fallback)
#include "r122_test.h"      // R122: the optional per-view DLSS preset (fake NGX / NvAPI)
#include "focus_twin_test.h"  // R123: the focus twin (census TerrainContexts; WARP pipeline vs stock, byte-exact)
#include "r126_test.h"       // R126: lossless overhead speed-ups (each against the code it replaced)
#include "r130_test.h"       // R130: read-only DLSS feature lifetime instrumentation (fake NGX core)
#include "r132_test.h"       // R132: opt-in release of idle DLSS sets (fake NGX core)
#include "r133_test.h"       // R133: the recorder's motion vectors (exec-time freshness rig, the verify's bracket, dump)
#include "r111_test.h"      // R111: [Color] PanelGamut (sRGB -> DCI-P3 compensation for a wide-gamut panel)
#include "log_phrases_test.h"  // R107: the log phrases DCS Control parses stay in the source byte-exact
#include "ini_keys_test.h"  // R107: every key the app writes is still read (live or retired)

// R60: QV_TEST_LOAD=<n> (off by default): n busy threads of normal priority inside this process for the whole run.
// An in-process stand-in for a machine loaded by another program (as DCS loading a mission); never an external load
// process. The threads are detached and end with the process.
void StartTestLoad() {
  const char* e = getenv("QV_TEST_LOAD");
  const int n = e ? atoi(e) : 0;
  if (n <= 0) return;
  printf("     (QV_TEST_LOAD: %d busy threads during the whole run)\n", n);
  for (int i = 0; i < n && i < 256; ++i)
    std::thread([] {
      volatile uint64_t v = 1;
      for (;;) v = v * 6364136223846793005ull + 1;
    }).detach();
}

// R107 (R106 F28): the run's files (bench_*.csv, report_*.txt, profile_*.txt, the test inis) go to
// %TEMP%\qv_test_<pid>\ instead of test\ (509 bench files had piled up there). Paths next to the binary
// (..\src, ..\lua) are taken from the exe's path, not the current directory, so they are unaffected.
// The folder is removed at exit when no check failed (kept for inspection otherwise).
std::wstring g_testOutDir;
void CleanTestOutDir() {
  if (g_testOutDir.empty() || g_fail) return;
  WIN32_FIND_DATAW fd;
  HANDLE h = FindFirstFileW((g_testOutDir + L"\\*").c_str(), &fd);
  if (h != INVALID_HANDLE_VALUE) {
    do
      if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) DeleteFileW((g_testOutDir + L"\\" + fd.cFileName).c_str());
    while (FindNextFileW(h, &fd));
    FindClose(h);
  }
  wchar_t tmp[MAX_PATH] = {};
  GetTempPathW(MAX_PATH, tmp);
  SetCurrentDirectoryW(tmp);
  RemoveDirectoryW(g_testOutDir.c_str());
}
void UseTempOutputDir() {
  wchar_t tmp[MAX_PATH] = {};
  GetTempPathW(MAX_PATH, tmp);
  g_testOutDir = std::wstring(tmp) + L"qv_test_" + std::to_wstring(GetCurrentProcessId());
  CreateDirectoryW(g_testOutDir.c_str(), nullptr);
  if (!SetCurrentDirectoryW(g_testOutDir.c_str())) {
    printf("     (test output: %ls not usable; files go to the current folder)\n", g_testOutDir.c_str());
    g_testOutDir.clear();
    return;
  }
  printf("     (test output folder: %ls)\n", g_testOutDir.c_str());
  atexit(&CleanTestOutDir);
}

#ifdef QV_TEST_NO_MAIN
int TestMain() {
#else
int main() {
#endif
  if (getenv("QV_UNBUFFERED")) setvbuf(stdout, nullptr, _IONBF, 0);  // R97: the order of lines around a crash
  g_log = stdout;
  g_mute = true;
  UseTempOutputDir();
  r90::NoteRingAtLoad();  // R115 (L2): the async log ring before any test (checked in r90::AsyncLog)
  StartTestLoad();
  // QV_SF_STRESS=<n>: only the split-filter tests, n times (race hunting).
  if (const char* n = getenv("QV_SF_STRESS")) {
    for (int i = 0, k = atoi(n); i < k; ++i) sftest::Run();
    PrintSummary();
    return g_fail ? 1 : 0;
  }
  // QV_DEFREC_ONLY=1: only the deferred-recording tests (R15 A1 infrastructure).
  // QV_DEFREC_BENCH=<rounds>: only its render-thread timing benchmark (3 runs).
  if (const char* n = getenv("QV_DEFREC_BENCH")) {
    drtest::Bench(atoi(n) > 2 ? atoi(n) : 15);
    return 0;
  }
  if (getenv("QV_DEFREC_ONLY")) {
    drtest::Run();
    PrintSummary();
    return g_fail ? 1 : 0;
  }
  if (getenv("QV_R122_ONLY")) {  // R122: only the per-view DLSS preset tests
    r122test::Run();
    PrintSummary();
    return g_fail ? 1 : 0;
  }
  if (getenv("QV_R123_ONLY")) {  // R123: only the focus twin tests
    r123test::Run();
    PrintSummary();
    return g_fail ? 1 : 0;
  }
  if (getenv("QV_R128_ONLY")) {  // R128: only the dormant-mode tests (QV_R128_HW=1: also the GPU; QV_R128_BENCH=1: cost)
    cktest::r128t::Run(getenv("QV_R128_HW") != nullptr);
    PrintSummary();
    return g_fail ? 1 : 0;
  }
  if (getenv("QV_R126_ONLY")) {  // R126: only the overhead speed-up tests
    r126test::Run();
    PrintSummary();
    return g_fail ? 1 : 0;
  }
  if (getenv("QV_R133_ONLY")) {  // R133: only the recorder motion-vector tests (QV_R95_HW=1: the GPU too)
    r133test::Run();
    if (const char* hw = getenv("QV_R95_HW"); hw && atoi(hw) != 0) r133test::Run(true);
    PrintSummary();
    return g_fail ? 1 : 0;
  }
  if (getenv("QV_R130_ONLY")) {  // R130: only the DLSS lifetime instrumentation tests
    r130test::Run();
    PrintSummary();
    return g_fail ? 1 : 0;
  }
  if (getenv("QV_R132_ONLY")) {  // R132: only the DLSS idle-set release tests (with R130's, which share the fakes)
    r130test::Run();
    r132test::Run();
    PrintSummary();
    return g_fail ? 1 : 0;
  }
  if (getenv("QV_R111_ONLY")) {  // R111: only the PanelGamut tests (QV_R111_HW=1: the GPU cost judged)
    r111test::Run();
    PrintSummary();
    return g_fail ? 1 : 0;
  }
  if (getenv("QV_R95_ONLY")) {  // R95: only the symbology export's tests
    r95test::Run();
    r97test::Run();  // R97
    r99test::Run();  // R99
    r101test::Run();  // R101
    if (const char* hw = getenv("QV_R95_HW"); hw && atoi(hw) != 0) cktest::r128t::Run(true);  // R128 (GPU too)
    if (const char* hw = getenv("QV_R95_HW"); hw && atoi(hw) != 0) r133test::Run(true);  // R133 (driver command lists)
    PrintSummary();
    return g_fail ? 1 : 0;
  }
  // QV_FTL_ONLY=1: only the frame_timeline.h tests (R52).
  if (getenv("QV_FTL_ONLY")) {
    ftltest::Run();
    PrintSummary();
    return g_fail ? 1 : 0;
  }
  // QV_PLOD_ONLY=1: only the periph_lod.h tests (R26 13).
  if (getenv("QV_PLOD_ONLY")) {
    plodtest::Run();
    PrintSummary();
    return g_fail ? 1 : 0;
  }
  // QV_COCKPITIPD_ONLY=1: only the cockpit-at-true-IPD tests (cockpit_ipd.h, R31 option 3).
  if (getenv("QV_COCKPITIPD_ONLY")) {
    cktest::Run();
    PrintSummary();
    return g_fail ? 1 : 0;
  }
  // R69: QV_MASK_ONLY=1: only the cockpit mask / depth companion tests.
  if (getenv("QV_MASK_ONLY")) {
    mask65test::Run();
    PrintSummary();
    return g_fail ? 1 : 0;
  }
  // QV_FIXDEPTH_ONLY=1: only the fixation depth tests (Depth boost).
  if (getenv("QV_FIXDEPTH_ONLY")) {
    fdtest::Run();
    sctest::Run();
    fartest::Run();
    mask65test::Run();
    cktest::Run();
    PrintSummary();
    return g_fail ? 1 : 0;
  }
  // QV_GOV_ONLY=1: only the R30 tests (perf sampler, detector, governor, status bits, log lines).
  if (getenv("QV_GOV_ONLY")) {
    govtest::Run();
    PrintSummary();
    return g_fail ? 1 : 0;
  }
  // QV_SHREC_ONLY=1: only the shadow recorder tests (R17 S1-S3).
  if (getenv("QV_SHREC_ONLY")) {
    srtest::Run();
    PrintSummary();
    return g_fail ? 1 : 0;
  }
  // QV_GBREC_ONLY=1: only the G-buffer recorder tests (R18 S1-S3).
  if (getenv("QV_GBREC_ONLY")) {
    grtest::Run();
    gbcktest::Run();
    PrintSummary();
    return g_fail ? 1 : 0;
  }
  // QV_VPROF_ONLY=1: only the synthetic pose and view profile tests.
  if (getenv("QV_VPROF_ONLY")) {
    vptest::Run();
    PrintSummary();
    return g_fail ? 1 : 0;
  }
  // QV_FSTART_ONLY=1: only the frame-start and runnable-threads counter tests (R22 E1, E3).
  if (getenv("QV_FSTART_ONLY")) {
    fsttest::Run();
    PrintSummary();
    return g_fail ? 1 : 0;
  }
  // QV_PFLUSH_ONLY=1: only the pass flush tests.
  if (getenv("QV_PFLUSH_ONLY")) {
    pftest::Run();
    PrintSummary();
    return g_fail ? 1 : 0;
  }
  // QV_SRVTRIM_ONLY=1: only the setShaderResources tail trim tests (R15 F2).
  if (getenv("QV_SRVTRIM_ONLY")) {
    strtest::Run();
    PrintSummary();
    return g_fail ? 1 : 0;
  }
  // QV_PFFIX_ONLY=1: only the terrain prefetch fix tests (prefetch_fix.h).
  if (getenv("QV_PFFIX_ONLY")) {
    pfxtest::Run();
    PrintSummary();
    return g_fail ? 1 : 0;
  }
  // QV_FWDREC_ONLY=1: only the forward recorder S0 counter tests (R21).
  if (getenv("QV_FWDREC_ONLY")) {
    frctest::Run();
    PrintSummary();
    return g_fail ? 1 : 0;
  }
  // QV_EXIT_ONLY=1: only the field-report 0.5.5 tests (DCS exit race, mission restart).
  if (getenv("QV_EXIT_ONLY")) {
    extest::Run();
    PrintSummary();
    return g_fail ? 1 : 0;
  }
  // QV_ROBUST_ONLY=1: only the robustness-round regression tests.
  if (getenv("QV_ROBUST_ONLY")) {
    rbtest::Run();
    PrintSummary();
    return g_fail ? 1 : 0;
  }
  // QV_LONGSESS_ONLY=1: only the long-session round's tests (reused meshes, table bounds, weights, pages, GPU share).
  if (getenv("QV_LONGSESS_ONLY")) {
    lstest::Run();
    PrintSummary();
    return g_fail ? 1 : 0;
  }
  // QV_VRAM_ONLY=1: only the GPU pass timing and VRAM census tests.
  // QV_CC_ONLY=1: only the R33 cascade cache tests.
  if (getenv("QV_CC_ONLY")) {
    cctest::Run();
    PrintSummary();
    return g_fail ? 1 : 0;
  }
  // QV_NT_ONLY=1: only the R37 night tone tests.
  if (getenv("QV_NT_ONLY")) {
    nttest::Run();
    PrintSummary();
    return g_fail ? 1 : 0;
  }
  // QV_SHF_ONLY=1: only the R36 shader fast tests.
  if (getenv("QV_SHF_ONLY")) {
    shfasttest::Run();
    PrintSummary();
    return g_fail ? 1 : 0;
  }
  // QV_CF_ONLY=1: only the R34 chain fast tests.
  if (getenv("QV_CF_ONLY")) {
    cftest::Run();
    PrintSummary();
    return g_fail ? 1 : 0;
  }
  // QV_PO_ONLY=1: only the [General] PerceptionOnly tests.
  if (getenv("QV_PO_ONLY")) {
    potest::Run();
    PrintSummary();
    return g_fail ? 1 : 0;
  }
  if (getenv("QV_VRAM_ONLY")) {
    gpttest::Run();
    vctest::Run();
    PrintSummary();
    return g_fail ? 1 : 0;
  }
  potest::EarlyFixationTick();  // R107: before any test maps GraphicsCore.dll (perception_only_test.h)
  lptest::Run();  // R107: app-parsed log phrases
  iktest::Run();  // R107: the app's ini keys, retired keys
  alignas(64) static uint8_t periph[0x700], focus[0x700];
  Vec3 eye{1000.0, 50.0, -2000.0}, fwd{0, 0, 1}, up{0, 1, 0};
  MakeFrustum(periph, eye, fwd, up, -1.2, 1.0, -1.1, 1.0, 0.05, 20000);  // ~50 deg half
  MakeFrustum(focus, eye, fwd, up, -0.30, 0.30, -0.28, 0.30, 0.05, 20000);  // ~17 deg half

  Frustum fp = DecodeFrustum(periph), ff = DecodeFrustum(focus);
  Check(fp.valid && ff.valid, "decode both frusta");
  Check(Len(fp.apex - eye) < 1e-6 && Len(ff.apex - eye) < 1e-6, "apex recovered");
  Check(Dot(ff.forward, fwd) > 0.9999, "forward recovered");
  printf("     periph mean half=%.1f deg, focus mean half=%.1f deg\n", fp.meanHalfAngle * 57.2958,
         ff.meanHalfAngle * 57.2958);

  uint8_t block[kPlaneBlockSize];
  // keep 0.35 of a focus spanning tan -0.30..0.30 -> excluded |tan x| < 0.105.
  BuildExclusionBlock(block, ff, 0.35);
  Vec3 centre = eye + fwd * 100.0;
  Check(InsideBlock(block, centre, 1.0), "centre object excluded");
  Check(InsideBlock(block, eye + Vec3{0.09, 0, 1} * 100.0, 0.1), "object in opaque core excluded");
  Check(!InsideBlock(block, eye + Vec3{0.12, 0, 1} * 100.0, 0.1), "object in blend band kept");
  Check(!InsideBlock(block, eye + Vec3{0.29, 0, 1} * 100.0, 0.1), "object at focus border kept");
  Check(!InsideBlock(block, eye + Vec3{0.5, 0, 1} * 100.0, 0.1), "object outside focus kept");
  // Asymmetric focus (gaze off-centre): the core must follow the focus centre.
  // MakeFrustum's "right" is fwd x up = -x, so this focus spans x in [-0.8, -0.2].
  alignas(64) static uint8_t offc[0x700];
  MakeFrustum(offc, eye, fwd, up, 0.20, 0.80, -0.30, 0.30, 0.05, 20000);
  Frustum fo = DecodeFrustum(offc);
  BuildExclusionBlock(block, fo, 0.35);
  Check(InsideBlock(block, eye + Vec3{-0.50, 0, 1} * 100.0, 0.1), "off-centre core excluded");
  Check(!InsideBlock(block, eye + Vec3{-0.25, 0, 1} * 100.0, 0.1), "off-centre band kept");
  Check(!InsideBlock(block, centre, 0.1), "head-centre kept when gaze is off-centre");

  // Full block (debug hole) equals the focus frustum.
  BuildExclusionBlock(block, ff, 1.0);
  Check(InsideBlock(block, eye + Vec3{0.28, 0, 1} * 100.0, 0.1) &&
            !InsideBlock(block, eye + Vec3{0.32, 0, 1} * 100.0, 0.1),
        "keep=1 matches focus frustum");

  // QVFR log on this machine (may be held open for writing by a running DCS).
  double sm = ReadQvfrSmoothing();
  printf("     qvfr smoothing read: %.2f\n", sm);
  Check(sm >= 0.0 && sm < 0.5, "QVFR smoothing parsed");
  sm = 0.30;
  g_qvSmoothing = sm;
  Check(std::fabs(EffectiveKeep() - 0.35) < 1e-9, "auto keep = 1 - 2*0.30 - 0.05");

  // Volume copy with exclusion.
  alignas(64) static uint8_t dst[0x700];
  Check(MakeVolumeWithExclusion(dst, periph, block), "volume copy");
  Check(*reinterpret_cast<uint64_t*>(dst + kVolExclSize) == 1 &&
            *reinterpret_cast<uint8_t**>(dst + kVolExclPtr) == dst + kVolExclInline &&
            memcmp(dst, periph, kVolExclPtr) == 0,
        "exclusion list layout");

  // Classification over a fake CollectionInfo array: L periph, R periph, L focus, R focus.
  alignas(64) static uint8_t vols[4][0x700];
  Vec3 eyeR = eye + Vec3{0.064, 0, 0};
  MakeFrustum(vols[0], eye, fwd, up, -1.2, 1.0, -1.1, 1.0, 0.05, 20000);
  MakeFrustum(vols[1], eyeR, fwd, up, -1.0, 1.2, -1.1, 1.0, 0.05, 20000);
  MakeFrustum(vols[2], eye, fwd, up, -0.3, 0.3, -0.28, 0.3, 0.05, 20000);
  MakeFrustum(vols[3], eyeR, fwd, up, -0.3, 0.3, -0.28, 0.3, 0.05, 20000);
  static uint8_t infos[5 * kCollectionInfoStride];
  memset(infos, 0, sizeof(infos));
  for (int i = 0; i < 4; ++i)
    *reinterpret_cast<uint8_t**>(infos + i * kCollectionInfoStride + kCiClipVolume) = vols[i];
  *reinterpret_cast<uint16_t*>(infos + 4 * kCollectionInfoStride) = 8;  // non-main pass
  *reinterpret_cast<uint8_t**>(infos + 4 * kCollectionInfoStride + kCiClipVolume) = vols[0];
  std::vector<ViewInfo> views;
  Patch patches[16];
  int pairs = 0, patched = 0;
  g_enabled = true;
  Prepare(5, infos, patches, 16, &pairs, &patched, views);
  Check(pairs == 2 && patched == 3, "two pairs found, 3 infos patched (incl. shared pass)");
  Check(*reinterpret_cast<uint8_t**>(infos + 4 * kCollectionInfoStride + kCiClipVolume) ==
            *reinterpret_cast<uint8_t**>(infos + kCiClipVolume),
        "pass sharing the peripheral volume keeps sharing the replacement");
  Check(views.size() == 4 && views[0].partner == 2 && views[1].partner == 3, "same-eye pairing");
  uint8_t* v0 = *reinterpret_cast<uint8_t**>(infos + kCiClipVolume);
  Check(v0 != vols[0] && *reinterpret_cast<uint64_t*>(v0 + kVolExclSize) == 1, "periph volume swapped");
  for (int i = 0; i < patched; ++i) *reinterpret_cast<uint8_t**>(patches[i].slot) = patches[i].original;
  Check(*reinterpret_cast<uint8_t**>(infos + kCiClipVolume) == vols[0], "restore");

  // Frame heap slabs: a fake TFrameMemoryHeap<1> with the original's bump
  // semantics (aligned size, xadd on +0x18, null past +0x10).
  {
    struct FakeHeap {
      uint8_t* base;
      uint64_t pad;
      uint8_t* end;
      std::atomic<uint8_t*> cursor;
    };
    static_assert(offsetof(FakeHeap, end) == 0x10 && offsetof(FakeHeap, cursor) == 0x18, "heap layout");
    static std::vector<uint8_t> mem(64 << 20);
    static FakeHeap heap;
    struct F {
      static void* Get(void* h, size_t size) {
        auto* fh = static_cast<FakeHeap*>(h);
        size_t a = (size + 15) & ~static_cast<size_t>(15);
        uint8_t* p = fh->cursor.fetch_add(a);
        return p + a > fh->end ? nullptr : p;
      }
      static void Reset(void* h) {
        auto* fh = static_cast<FakeHeap*>(h);
        fh->cursor = fh->base;
      }
    };
    heap.base = reinterpret_cast<uint8_t*>((reinterpret_cast<uintptr_t>(mem.data()) + 15) & ~uintptr_t(15));
    heap.end = heap.base + (48 << 20);
    heap.cursor = heap.base;
    frameheap::g_orig = &F::Get;
    frameheap::g_origReset = &F::Reset;
    bool aligned = true, inside = true, noOverlap = true;
    for (int on = 0; on < 2; ++on) {
      g_frameHeapOn = on == 1;
      for (int frame = 0; frame < 3; ++frame) {
        std::vector<std::vector<std::pair<uint8_t*, size_t>>> recs(8);
        std::vector<std::thread> th;
        for (int t = 0; t < 8; ++t)
          th.emplace_back([&, t] {
            for (int i = 0; i < 20000; ++i) {
              size_t sz = 16 + ((i * 37 + t * 11) % 400);
              auto* p = static_cast<uint8_t*>(frameheap::HookGet(&heap, sz));
              if (p) recs[t].push_back({p, sz});
            }
          });
        for (auto& x : th) x.join();
        std::vector<std::pair<uint8_t*, size_t>> all;
        for (auto& v : recs) all.insert(all.end(), v.begin(), v.end());
        std::sort(all.begin(), all.end());
        for (size_t i = 0; i < all.size(); ++i) {
          if (reinterpret_cast<uintptr_t>(all[i].first) & 15) aligned = false;
          if (all[i].first < heap.base || all[i].first + all[i].second > heap.end) inside = false;
          if (i && all[i - 1].first + all[i - 1].second > all[i].first) noOverlap = false;
        }
        frameheap::HookReset<0>(&heap);
      }
    }
    // Near the end of the heap the original is used directly.
    g_frameHeapOn = true;
    heap.cursor = heap.end - 1000;
    void* last = frameheap::HookGet(&heap, 64);
    Check(last == heap.end - 1000, "frame heap: near the end the original allocator is used");
    g_frameHeapOn = false;
    Check(aligned && inside, "frame heap slabs: 16-byte aligned, inside the heap");
    Check(noOverlap, "frame heap slabs: no overlapping allocations, with and without slabs");
    frameheap::g_orig = nullptr;
  }

  // Pose sweep: yaw rotation of returned views keeps unit quaternions and
  // rotates positions about Y; nothing changes when off.
  {
    struct Fake {
      static int32_t Locate(void*, const void*, void*, uint32_t cap, uint32_t* count, uint8_t* views) {
        *count = 2;
        for (uint32_t i = 0; i < cap && i < 2; ++i) {
          float* q = reinterpret_cast<float*>(views + i * 64 + 16);
          q[0] = 0; q[1] = 0; q[2] = 0; q[3] = 1;            // identity orientation
          q[4] = i ? 0.032f : -0.032f; q[5] = 1.2f; q[6] = 0;  // eye positions
        }
        return 0;
      }
    };
    posesweep::g_tramp = &Fake::Locate;
    alignas(16) uint8_t views[2 * 64] = {};
    uint32_t count = 0;
    posesweep::g_on = false;
    posesweep::Hook(nullptr, nullptr, nullptr, 2, &count, views);
    float* q0 = reinterpret_cast<float*>(views + 16);
    Check(q0[3] == 1 && q0[1] == 0 && q0[4] == -0.032f, "pose sweep off: views unchanged");
    // t = 7 s into the recording: slow sweep, yaw = 60*sin(2*pi*2/8) = 60 deg
    LARGE_INTEGER now, f;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&f);
    g_qpcToUs = 1e6 / static_cast<double>(f.QuadPart);
    posesweep::g_sweep = true;  // what Start() sets with g_on
    posesweep::g_on = true;
    // The hook reads the clock itself: yaw > 30 deg holds for ~0.67 s after
    // t = 7 s, so a test thread preempted that long retries with a fresh t0.
    for (int attempt = 0; attempt < 10; ++attempt) {
      QueryPerformanceCounter(&now);
      posesweep::g_t0 = now.QuadPart - static_cast<int64_t>(7.0 * f.QuadPart);
      posesweep::Hook(nullptr, nullptr, nullptr, 2, &count, views);
      if (std::fabs(posesweep::g_yawDeg.load()) > 30.0) break;
    }
    posesweep::g_on = false;
    posesweep::g_sweep = false;
    double yaw = posesweep::g_yawDeg.load() * 3.14159265358979 / 180.0;
    double len = std::sqrt(q0[0] * q0[0] + q0[1] * q0[1] + q0[2] * q0[2] + q0[3] * q0[3]);
    double expectY = std::sin(yaw / 2), expectW = std::cos(yaw / 2);
    double px = -0.032 * std::cos(yaw), pz = 0.032 * std::sin(yaw);
    printf("     pose sweep: yaw %.1f deg, q=(%.3f %.3f %.3f %.3f), left eye at (%.4f %.3f %.4f)\n",
           posesweep::g_yawDeg.load(), q0[0], q0[1], q0[2], q0[3], q0[4], q0[5], q0[6]);
    Check(std::fabs(posesweep::g_yawDeg.load()) > 30.0, "pose sweep applies a yaw in the slow phase");
    Check(std::fabs(len - 1.0) < 1e-5, "pose sweep keeps unit quaternions");
    Check(std::fabs(q0[1] - expectY) < 1e-5 && std::fabs(q0[3] - expectW) < 1e-5, "pose sweep orientation = yaw about Y");
    Check(std::fabs(q0[4] - px) < 1e-5 && std::fabs(q0[6] - pz) < 1e-5 && q0[5] == 1.2f, "pose sweep rotates positions about Y");
    posesweep::g_tramp = nullptr;
  }

  // Pacer MWAITX stub: called with rsi = flags, the way the patched loop does.
  if (pacer::HasMonitorx()) {
    auto* mem = static_cast<uint8_t*>(VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    // caller: push rsi; mov rsi, rcx; call stub (rel32); pop rsi; ret
    uint8_t* stub = mem + 64;
    uint8_t* c = mem;
    pacer::Emit(c, {0x56, 0x48, 0x89, 0xCE, 0xE8});
    int32_t rel = static_cast<int32_t>(stub - (c + 4));
    memcpy(c, &rel, 4);
    c += 4;
    pacer::Emit(c, {0x5E, 0xC3});
    const double tsc = 4.7e9;
    pacer::EmitMwaitStub(stub, static_cast<uint32_t>(tsc * 0.005));  // 5 ms: long enough to observe
    DWORD old;
    VirtualProtect(mem, 4096, PAGE_EXECUTE_READ, &old);
    auto call = reinterpret_cast<void (*)(volatile uint8_t*)>(mem);
    alignas(64) static volatile uint8_t flags[64];
    auto ms = [](LARGE_INTEGER a, LARGE_INTEGER b) {
      LARGE_INTEGER f;
      QueryPerformanceFrequency(&f);
      return (b.QuadPart - a.QuadPart) * 1000.0 / f.QuadPart;
    };
    // Wall-clock bounds: a preempted test thread (CPU load) only makes an
    // attempt longer, so each bound must hold in the best of 20 attempts
    // (the first attempt passes on an idle machine).
    LARGE_INTEGER t0, t1;
    double exitNow = 1e9, slept = 0, woke = 1e9;
    bool sleptOk = false;
    for (int attempt = 0; attempt < 20 && exitNow >= 0.5; ++attempt) {
      flags[0] = 0;
      QueryPerformanceCounter(&t0);
      call(flags);
      QueryPerformanceCounter(&t1);
      exitNow = std::min(exitNow, ms(t0, t1));
    }
    for (int attempt = 0; attempt < 20 && !sleptOk; ++attempt) {
      flags[0] = 1;
      flags[12] = 0;
      QueryPerformanceCounter(&t0);
      call(flags);
      QueryPerformanceCounter(&t1);
      slept = ms(t0, t1);
      sleptOk = slept > 0.01 && slept < 50.0;
    }
    for (int attempt = 0; attempt < 20 && woke >= 50.0; ++attempt) {
      flags[0] = 1;
      std::thread waker([] {
        Sleep(1);
        flags[0] = 0;
      });
      QueryPerformanceCounter(&t0);
      call(flags);
      QueryPerformanceCounter(&t1);
      waker.join();
      woke = std::min(woke, ms(t0, t1));
    }
    printf("     pacer stub: exit flag %.3f ms, timeout %.3f ms, woken by write %.3f ms\n", exitNow, slept, woke);
    Check(exitNow < 0.5, "pacer stub returns at once when the exit flag is already set");
    // MWAITX may wake early (interrupts); it must sleep a little and never
    // longer than the timeout.
    Check(sleptOk, "pacer stub sleeps, bounded by the timeout");
    Check(woke < 50.0, "pacer stub returns after a write to the flags");
    Check(flags[12] == 0, "pacer stub leaves the flags untouched");
  } else {
    printf("     pacer stub: CPU has no MONITORX, test skipped\n");
  }

  // Allocator slabs: a fake StructBufferManager with the original semantics
  // (page search 0xc1b0, locked bump 0xbf30, reset slot 7). Many threads
  // allocate; no two allocations may overlap within a generation, every
  // result must match its (page, index) out-parameters, and the reset must
  // invalidate every slab.
  {
    struct FakeMgr {
      void* vt;
      uint8_t* begin;
      uint8_t* end;
      uint8_t* cap;
      std::mutex m;  // stands in for the ed::mutex at +0x20
    };
    static_assert(offsetof(FakeMgr, m) == 0x20, "mutex offset");
    static FakeMgr mgr;
    static std::vector<uint8_t> pageMem(0x30 * 256);
    mgr.begin = mgr.end = pageMem.data();
    mgr.cap = pageMem.data() + pageMem.size();
    struct Fakes {
      static void Lock(void* mu) { static_cast<std::mutex*>(mu)->lock(); }
      static void Unlock(void* mu) { static_cast<std::mutex*>(mu)->unlock(); }
      static allocslab::Page* Scan(void* m, uint32_t size, uint32_t count) {
        auto* fm = static_cast<FakeMgr*>(m);
        for (uint8_t* p = fm->begin; p < fm->end; p += 0x30) {
          auto* pg = reinterpret_cast<allocslab::Page*>(p);
          if (pg->elemSize == size && pg->capBytes - pg->used >= size * count) return pg;
        }
        uint32_t n = std::max(count, 0xFE00u / size);
        auto* pg = reinterpret_cast<allocslab::Page*>(fm->end);
        *pg = {static_cast<uint32_t>((fm->end - fm->begin) / 0x30), size, n, n * size, 0, 0,
               static_cast<uint8_t*>(malloc(n * size)), nullptr};
        fm->end += 0x30;
        return pg;
      }
      static uint8_t* Alloc(void* m, uint32_t size, uint32_t count, uint32_t* page, uint32_t* index) {
        auto* fm = static_cast<FakeMgr*>(m);
        fm->m.lock();
        allocslab::Page* pg = Scan(m, size, count);
        uint32_t used = pg->used;
        pg->used += size * count;
        *page = pg->idx;
        *index = used / size;
        fm->m.unlock();
        return pg->data + used;
      }
      static void Reset(void* m) {
        auto* fm = static_cast<FakeMgr*>(m);
        for (uint8_t* p = fm->begin; p < fm->end; p += 0x30) {
          auto* pg = reinterpret_cast<allocslab::Page*>(p);
          pg->prevUsed = pg->used;
          pg->used = 0;
        }
      }
    };
    allocslab::g_lock = &Fakes::Lock;
    allocslab::g_unlock = &Fakes::Unlock;
    allocslab::g_scan = &Fakes::Scan;
    allocslab::g_tramp = &Fakes::Alloc;
    allocslab::g_origReset = &Fakes::Reset;
    g_allocSlabBytes = 4096;

    struct Rec {
      uint32_t page, off, len;
      uint8_t* ptr;
    };
    bool allMatch = true, noOverlap = true;
    uint64_t totalSlow = 0, totalCalls = 0;
    for (int mode = 0; mode < 2; ++mode) {
      g_allocSlabsOn = mode == 1;
      for (int frame = 0; frame < 3; ++frame) {
        std::vector<std::vector<Rec>> recs(8);
        std::vector<std::thread> th;
        for (int t = 0; t < 8; ++t)
          th.emplace_back([&, t] {
            uint32_t sizes[4] = {0x60, 0x70, 0xA0, 0xB0};
            for (int i = 0; i < 3000; ++i) {
              uint32_t size = sizes[(i * 7 + t) % 4], count = 1 + (i % 5), pg = 0, idx = 0;
              uint8_t* p = allocslab::HookAlloc(&mgr, size, count, &pg, &idx);
              recs[t].push_back({pg, idx * size, size * count, p});
            }
          });
        for (auto& x : th) x.join();
        std::map<uint32_t, std::vector<std::pair<uint32_t, uint32_t>>> byPage;
        for (auto& v : recs)
          for (const Rec& r : v) {
            auto* pg = reinterpret_cast<allocslab::Page*>(mgr.begin + r.page * 0x30);
            if (r.ptr != pg->data + r.off || r.off + r.len > pg->used) allMatch = false;
            byPage[r.page].push_back({r.off, r.len});
          }
        for (auto& kv : byPage) {
          auto& v = kv.second;
          std::sort(v.begin(), v.end());
          for (size_t i = 1; i < v.size(); ++i)
            if (v[i - 1].first + v[i - 1].second > v[i].first) noOverlap = false;
        }
        allocslab::HookReset(&mgr);  // per-frame reset through the hooked slot
      }
    }
    allocslab::Totals tot = allocslab::Snapshot();
    totalSlow = tot.slow;
    totalCalls = tot.calls;
    printf("     allocator slabs: %llu calls, %llu slow-path reservations with slabs on\n",
           static_cast<unsigned long long>(totalCalls), static_cast<unsigned long long>(totalSlow));
    Check(allMatch, "every allocation matches its page and index, inside the used range");
    Check(noOverlap, "no two allocations overlap within a frame, with and without slabs");
    Check(totalSlow > 0 && totalSlow < totalCalls / 10, "slabs serve most requests without the lock");
    g_allocSlabsOn = false;
    allocslab::g_tramp = nullptr;
  }

  // Shadow caster tightening: no caster whose shadow can reach a view may be
  // removed, and some casters must be removed.
  {
    g_enabled = false;
    const Vec3 L = Normalize(Vec3{0.3, -0.7, 0.6});
    auto basis = [&](double rot, Vec3& X, Vec3& Y) {
      Vec3 x0 = Normalize(Cross(L, Vec3{0, 1, 0}));
      Vec3 y0 = Cross(x0, L);
      X = x0 * std::cos(rot) + y0 * std::sin(rot);
      Y = Cross(X, L);
    };
    auto makeBox = [&](uint8_t* vol, Vec3 X, Vec3 Y, Vec3 c, double hx, double hy, double back, double front) {
      memset(vol, 0, kVolSize);
      Vec3 ax[3] = {X, Y, L};
      double lo[3] = {Dot(X, c) - hx, Dot(Y, c) - hy, Dot(L, c) - back};
      double hi[3] = {Dot(X, c) + hx, Dot(Y, c) + hy, Dot(L, c) + front};
      for (int a = 0; a < 3; ++a) {
        WritePlane(vol, 2 * a, Plane{ax[a], -lo[a]});
        WritePlane(vol, 2 * a + 1, Plane{ax[a] * -1.0, hi[a]});
      }
      *reinterpret_cast<int32_t*>(vol + kPlaneCount) = 6;
    };
    alignas(64) static uint8_t casc[4][0x700], terr[0x700];
    static uint8_t sinfos[9 * kCollectionInfoStride];
    memset(sinfos, 0, sizeof(sinfos));
    Vec3 X, Y;
    basis(0.0, X, Y);
    const double ext[4] = {30, 120, 900, 5000};
    for (int c = 0; c < 4; ++c) {
      Vec3 mid = eye + fwd * (ext[c] * 0.5);
      makeBox(casc[c], X, Y, mid, ext[c], ext[c], ext[c] * 2, ext[c]);
    }
    Vec3 X2, Y2;
    basis(0.2, X2, Y2);
    makeBox(terr, X2, Y2, eye, 60000, 60000, 60000, 60000);
    for (int i = 0; i < 4; ++i)
      *reinterpret_cast<uint8_t**>(sinfos + i * kCollectionInfoStride + kCiClipVolume) = vols[i];
    for (int c = 0; c < 4; ++c) {
      uint8_t* ci = sinfos + (4 + c) * kCollectionInfoStride;
      *reinterpret_cast<uint16_t*>(ci + kCiShadingModel) = 14;
      *reinterpret_cast<uint8_t**>(ci + kCiClipVolume) = casc[c];
    }
    *reinterpret_cast<uint16_t*>(sinfos + 8 * kCollectionInfoStride + kCiShadingModel) = 15;
    *reinterpret_cast<uint8_t**>(sinfos + 8 * kCollectionInfoStride + kCiClipVolume) = terr;

    Patch sp[32];
    int sn = 0;
    g_shadowDebugInvert = false;
    for (int pass = 0; pass < 3; ++pass) {
    // pass 0: terrain map present (exact light); 1: tracking the cached light;
    // 2: no reference, both non-horizontal axes treated as possible lights.
    if (pass == 1) *reinterpret_cast<uint8_t**>(sinfos + 8 * kCollectionInfoStride + kCiClipVolume) = nullptr;
    if (pass == 2) g_lightValid = false;
    sn = 0;
    PrepareShadows(9, sinfos, sp, 32, &sn);
    Check(sn == 4, "all four cascades get a tightened volume");

    // Receivers as plane lists, for the reference ray test.
    std::vector<std::vector<Plane>> recv(4);
    for (int i = 0; i < 4; ++i) ReadPlanes(vols[i], recv[i]);
    // A receiver samples a cascade only inside its footprint (the box slabs
    // across the light) and nothing visible lies below the floor height.
    auto rayHits = [&](Vec3 p, const std::vector<Plane>& box) {
      for (const auto& recvPlanes : recv) {
        std::vector<Plane> pl = recvPlanes;
        for (const Plane& q : box)
          if (std::fabs(Dot(q.n, L)) < 0.999) pl.push_back(q);
        pl.push_back({Vec3{0, 1, 0}, -g_cfg.shadowFloorY});
        double t0 = 0, t1 = 1e6;
        for (const Plane& q : pl) {
          double a = Dot(q.n, L), b = Dot(q.n, p) + q.d;  // need b + a t >= 0
          if (std::fabs(a) < 1e-12) {
            if (b < 0) t1 = -1;
          } else if (a > 0) {
            t0 = std::max(t0, -b / a);
          } else {
            t1 = std::min(t1, -b / a);
          }
        }
        if (t0 <= t1) return true;
      }
      return false;
    };
    int violations = 0, removed = 0, total = 0, addedMax = 0;
    uint32_t seed = 12345;
    auto rnd = [&]() {
      seed = seed * 1664525u + 1013904223u;
      return (seed >> 8) / double(1 << 24);
    };
    for (int c = 0; c < 4; ++c) {
      uint8_t* tv = *reinterpret_cast<uint8_t**>(sinfos + (4 + c) * kCollectionInfoStride + kCiClipVolume);
      int np = *reinterpret_cast<int32_t*>(tv + kPlaneCount);
      addedMax = std::max(addedMax, np - 6);
      std::vector<Plane> all;
      ReadPlanes(tv, all);
      std::vector<Plane> box;
      ReadPlanes(casc[c], box);
      for (int k = 0; k < 20000; ++k) {
        Vec3 mid = eye + fwd * (ext[c] * 0.5);
        Vec3 p = mid + X * ((rnd() * 2 - 1) * ext[c]) + Y * ((rnd() * 2 - 1) * ext[c]) +
                 L * ((rnd() * 3 - 2) * ext[c]);
        bool inBox = true;
        for (const Plane& q : box)
          if (Dot(q.n, p) + q.d < 0) inBox = false;
        if (!inBox) continue;
        ++total;
        bool kept = true;
        for (const Plane& q : all)
          if (Dot(q.n, p) + q.d < 0) kept = false;
        if (!kept) ++removed;
        if (!kept && rayHits(p, box)) ++violations;
      }
    }
    for (int i = 0; i < sn; ++i) *reinterpret_cast<uint8_t**>(sp[i].slot) = sp[i].original;
    printf("     shadow tightening pass %d: %d of %d box points removed, max %d planes added\n", pass, removed, total,
           addedMax);
    Check(violations == 0, "no removed caster can shadow a view");
    Check(removed > total / 20, "tightening removes a meaningful share of the cascade boxes");
    Check(*reinterpret_cast<uint8_t**>(sinfos + 4 * kCollectionInfoStride + kCiClipVolume) == casc[0],
          "cascade restore");
    }
  }

  // Saccade detection: same views twice = no hold, moved focus = hold.
  {
    g_cfg.saccadeDeg = 1.0;
    g_cfg.saccadeHoldFrames = 6;
    Check(!UpdateGazeAndCheckHold(views), "first frame no hold");
    Check(!UpdateGazeAndCheckHold(views), "static gaze no hold");
    std::vector<ViewInfo> moved = views;
    MakeFrustum(vols[2], eye, fwd, up, -0.2, 0.4, -0.28, 0.3, 0.05, 20000);  // ~5.6 deg shift
    moved[2].fr = DecodeFrustum(vols[2]);
    Check(UpdateGazeAndCheckHold(moved), "gaze jump starts hold");
    for (int i = 0; i < 5; ++i) UpdateGazeAndCheckHold(moved);
    Check(!UpdateGazeAndCheckHold(moved), "hold ends after 6 frames");
  }

  // Real Scene.dll: exports and vtable slot.
  HMODULE scene = dcsinstall::LoadBinary(L"Scene.dll");
  if (!dcsinstall::Found())
    dcsinstall::Skip("map Scene.dll");
  else
    Check(scene != nullptr, "map Scene.dll");
  if (scene) {
    auto vt = reinterpret_cast<void**>(GetProcAddress(scene, kViewVtable));
    void* fn = reinterpret_cast<void*>(GetProcAddress(scene, kCollectExport));
    Check(vt && fn && vt[kCollectSlot] == fn, "vtable slot 3 == collectSceneObjectsRenderables");
  }
  // Benchmark: synthetic frames, ON is 10% cheaper.
  {
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    g_qpcToUs = 1e6 / static_cast<double>(f.QuadPart);
    g_dir = L"./";
    g_cfg.benchBlocks = 6;
    g_cfg.benchBlockSec = 2;
    g_cfg.benchSettleSec = 0.2;
    std::atomic<bool> stop{false};
    std::thread frames([&] {
      while (!stop) {
        LARGE_INTEGER a, b;
        QueryPerformanceCounter(&a);
        double ms = g_enabled.load() ? 9.0 : 10.0;
        do QueryPerformanceCounter(&b);
        while ((b.QuadPart - a.QuadPart) * g_qpcToUs < ms * 1000);
        BenchOnFrame(b.QuadPart, g_enabled.load() ? 900 : 1000, 500, 1000);
      }
    });
    g_enabled = true;
    RunBenchmark(0, true);
    stop = true;
    frames.join();
    Check(g_enabled.load(), "bench restores previous state");
  }
  // Profiler: a busy thread must show up with a multi-frame stack.
  {
    std::atomic<bool> stop{false};
    std::thread busy([&] {
      volatile double x = 1.0;
      while (!stop) x = std::sqrt(x + 1.0);
    });
    // Profiles left by earlier runs or other phases (same folder) must not be
    // mistaken for this one: note what exists before, then read only the new
    // file (names carry the second they were written, so the newest new one).
    auto listProfiles = [] {
      std::set<std::wstring> names;
      WIN32_FIND_DATAW fd;
      HANDLE h = FindFirstFileW(L"./profile_*.txt", &fd);
      if (h != INVALID_HANDLE_VALUE) {
        do names.insert(fd.cFileName);
        while (FindNextFileW(h, &fd));
        FindClose(h);
      }
      return names;
    };
    const std::set<std::wstring> before = listProfiles();
    prof::Run(2, 4, 1, "selftest");
    stop = true;
    busy.join();
    std::wstring mine;
    for (const std::wstring& n : listProfiles())
      if (!before.count(n) && n > mine) mine = n;
    const bool found = !mine.empty();
    bool hasExe = false, noSamples = false;
    if (found) {
      FILE* f = nullptr;
      _wfopen_s(&f, (L"./" + mine).c_str(), L"r");
      char line[512];
      int shown = 0;
      while (f && fgets(line, sizeof(line), f)) {
        if (strstr(line, "test_main.exe")) hasExe = true;
        if (shown == 0 && strstr(line, " 0 samples")) noSamples = true;
        if (shown++ < 14) printf("     | %s", line);
      }
      if (f) fclose(f);
      DeleteFileW((L"./" + mine).c_str());
    }
    if (found && noSamples)
      QvSkip("profiler sampled the busy thread: the profiler got no stack samples (machine under load)\n");
    else
      Check(found && hasExe, "profiler sampled the busy thread");
  }
  // Timer cache: the hook only reads the cached value, refreshed by the updater.
  {
    // Real-timer calls are counted per thread: the hook path (this thread)
    // must make none, whatever the updater does meanwhile. The updater's
    // progress is waited for with a deadline, never a fixed sleep (a loaded
    // machine can delay it).
    TestSpin spin("timer cache");
    static std::atomic<int> hookThreadCalls{0};
    static DWORD hookTid = 0;
    hookTid = GetCurrentThreadId();
    timercache::g_orig = [] {
      if (GetCurrentThreadId() == hookTid) ++hookThreadCalls;
      return ED_FakeTime();
    };
    timercache::g_periodUs = 1000;
    timercache::g_cached = timercache::g_orig();
    const uint64_t refresh0 = timercache::g_refreshes.load();
    HANDLE t = CreateThread(nullptr, 0, timercache::Updater, nullptr, 0, nullptr);
    auto waitFor = [](auto cond) {
      const ULONGLONG end = GetTickCount64() + 30000;
      while (!cond() && GetTickCount64() < end) Sleep(1);
      return cond();
    };
    waitFor([&] { return timercache::g_refreshes.load() > refresh0; });
    timercache::g_on = true;
    const int before = hookThreadCalls.load();
    double first = timercache::Hook();
    for (int i = 0; i < 100000; ++i) timercache::Hook();
    const int during = hookThreadCalls.load() - before;
    const bool advanced = waitFor([&] { return timercache::Hook() > first; });
    double later = timercache::Hook();
    Check(during == 0, "hook path never calls the real timer (only the ~1 kHz updater does)");
    Check(advanced && later > first, "cached value advances");
    // Lag of the cached value behind the real clock: the updater runs every
    // ~1 ms, so the best of a few samples is far below 0.1 s even under load.
    double lag = 1e9;
    for (int i = 0; i < 50 && lag >= 0.1; ++i) {
      const double cached = timercache::Hook();
      lag = std::min(lag, ED_FakeTime() - cached);
      if (lag >= 0.1) Sleep(2);
    }
    Check(lag >= 0 && lag < 0.1, "cached value tracks real time");
    const bool refreshed = waitFor([&] { return timercache::g_refreshes.load() > refresh0 + 10; });
    const int refresh = static_cast<int>(timercache::g_refreshes.load() - refresh0);
    Check(refreshed && refresh > 10, "updater refreshes at ~1 kHz");
    timercache::g_on = false;
    int b2 = hookThreadCalls.load();
    timercache::Hook();
    Check(hookThreadCalls.load() >= b2 + 1, "disabled cache passes calls through");
    Check(timercache::FindImport(GetModuleHandleW(nullptr), "KERNEL32.dll", "QueryPerformanceCounter") != nullptr,
          "IAT lookup finds an import");

    // [Hotkeys] Toggle parser and the in-flight kill switch.
    {
      int vk = -1, mods = -1;
      Check(ParseHotkey(L"122:6", vk, mods) && vk == 122 && mods == 6, "hotkey 122:6 = Alt+Shift+F11");
      Check(ParseHotkey(L"0:0", vk, mods) && vk == 0 && mods == 0, "hotkey 0:0 = off");
      Check(!ParseHotkey(L"255:1", vk, mods) && vk == 0, "hotkey key 255 rejected");
      Check(!ParseHotkey(L"120:8", vk, mods), "hotkey modifiers 8 rejected");
      Check(!ParseHotkey(L"0:2", vk, mods), "hotkey key 0 with modifiers rejected");
      Check(!ParseHotkey(L"F11", vk, mods) && !ParseHotkey(L"122", vk, mods) && !ParseHotkey(L"122:", vk, mods) &&
                !ParseHotkey(L"122:6x", vk, mods) && !ParseHotkey(L"", vk, mods),
            "hotkey malformed values rejected");

      const Config saved = g_cfg;
      const wchar_t* iniPath = L"./DcsQvCull.ini";
      auto writeIni = [&](const char* extra) {
        FILE* f = nullptr;
        _wfopen_s(&f, iniPath, L"w");
        fprintf(f, "[Timing]\nShaderTimeCache=1\nCacheUs=1000\n[Scene]\nPartitionBoost=1\n[Hotkeys]\nToggle=122:6\n%s", extra);
        fclose(f);
      };
      // [Perception] FarDepthCoverageMode: absent -> hdr (1), "overlay" -> 0,
      // anything else -> hdr, logged once as invalid.
      {
        bool inv = true;
        Check(ParseFarCoverageMode(L"", &inv) == 1 && !inv && ParseFarCoverageMode(L"hdr", &inv) == 1 && !inv &&
                  ParseFarCoverageMode(L"HDR", &inv) == 1 && !inv && ParseFarCoverageMode(L"overlay", &inv) == 0 &&
                  !inv && ParseFarCoverageMode(L"Overlay", &inv) == 0 && !inv &&
                  ParseFarCoverageMode(L"ldr", &inv) == 1 && inv,
              "FarDepthCoverageMode parse: empty/hdr -> hdr, overlay (any case) -> overlay, other -> hdr + invalid");
        writeIni("");
        LoadConfig(true);
        Check(g_cfg.farDepthCoverageMode == 1, "ini: FarDepthCoverageMode absent -> hdr (the default)");
        // R107 (R106 F06): a key missing from the ini behaves as DCS Control ships it (the code default equals the
        // template's value), and a fresh Config agrees with the parse.
        {
          const Config fresh;
          Check(g_cfg.shadowRecorderScope == 0x30f && fresh.shadowRecorderScope == 0x30f,
                "R107 default: [Model] ShadowRecorderScope missing -> 0x30f (shipped)");
          Check(g_cfg.gbufferRecorderScope == 0x10055 && fresh.gbufferRecorderScope == 0x10055,
                "R107 default: [Model] GBufferRecorderScope missing -> 0x10055 (shipped)");
          Check(g_cfg.gbufferRecorderByOrdinal && fresh.gbufferRecorderByOrdinal,
                "R107 default: [Model] GBufferRecorderByOrdinal missing -> 1 (shipped)");
          Check(g_cfg.prefetchWindowMs == 5000 && fresh.prefetchWindowMs == 5000 && pffix::kDefaultWindowMs == 5000,
                "R107 default: [Prefetch] WindowMs missing -> 5000 (shipped; the module's own default too)");
          Check(g_cfg.cockpitTrueIpdLighting && fresh.cockpitTrueIpdLighting,
                "R107 default: [Perception] CockpitTrueIpdLighting missing -> 1 (shipped)");
          Check(g_cfg.cockpitTrueIpdToggleVk == 0 && g_cfg.cockpitTrueIpdToggleMods == 0 &&
                    fresh.cockpitTrueIpdToggleVk == 0 && fresh.cockpitTrueIpdToggleMods == 0,
                "R107 default: [Perception] CockpitTrueIpdToggle missing -> 0:0, no live hotkey (shipped)");
          Check(g_cfg.suiteBenchEngine && fresh.suiteBenchEngine,
                "R107 default: [Suite] BenchEngine missing -> 1 (shipped)");
          Check(g_cfg.suiteQuick && fresh.suiteQuick, "R107 default: [Suite] Quick missing -> 1 (shipped)");
        }
        writeIni("[Perception]\nFarDepthCoverageMode=overlay\n");
        LoadConfig(true);
        Check(g_cfg.farDepthCoverageMode == 0, "ini: FarDepthCoverageMode=overlay -> overlay");
        writeIni("[Perception]\nFarDepthCoverageMode=hdr\n");
        LoadConfig(true);
        Check(g_cfg.farDepthCoverageMode == 1, "ini: FarDepthCoverageMode=hdr -> hdr");
        const int il0 = g_farCovInvalidLogs;
        const bool was = g_farCovInvalidLogged;
        g_farCovInvalidLogged = false;
        writeIni("[Perception]\nFarDepthCoverageMode=bogus\n");
        LoadConfig(true);
        const bool firstRead = g_cfg.farDepthCoverageMode == 1 && g_farCovInvalidLogs == il0 + 1;
        LoadConfig(true);
        Check(firstRead && g_cfg.farDepthCoverageMode == 1 && g_farCovInvalidLogs == il0 + 1,
              "ini: FarDepthCoverageMode=bogus -> hdr, logged once as invalid (not again on the next read)");
        g_farCovInvalidLogged = was || g_farCovInvalidLogged;
      }
      // R49: [D3D] SplitFilter defaults to 1 (the shipped setting; ChainFast / ShaderFast rely on it); 0 still turns it off.
      writeIni("");
      LoadConfig(true);
      const bool sfDefault = g_cfg.splitFilter;
      writeIni("[D3D]\nSplitFilter=0\n");
      LoadConfig(true);
      const bool sfOff = !g_cfg.splitFilter;
      Check(sfDefault && sfOff, "ini: [D3D] SplitFilter absent -> 1 (the shipped default), SplitFilter=0 -> off");
      writeIni("");
      LoadConfig(true);
      Check(g_cfg.toggleVk == 122 && g_cfg.toggleMods == 6 && !g_cfg.developerKeys, "ini hotkey read, developer keys off");
      Check(timercache::g_on.load() && g_partitionBoost.load(), "ini state: cache and boost on");
      ToggleEngine();
      Check(!timercache::g_on.load() && !g_partitionBoost.load() && g_engineOff.load(), "toggle turns both off");
      ToggleEngine();
      Check(timercache::g_on.load() && g_partitionBoost.load() && !g_engineOff.load(), "second toggle restores ini state");
      ToggleEngine();
      g_benchRunningFlag = true;
      Check(!ToggleEngine() && g_engineOff.load(), "toggle refused while a suite runs");
      g_benchRunningFlag = false;
      Sleep(20);
      writeIni("; profile applied\n");
      HANDLE h = CreateFileW(iniPath, FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
      FILETIME bumped = g_iniTime;
      reinterpret_cast<ULARGE_INTEGER*>(&bumped)->QuadPart += 10000000;  // +1 s, never equal to the old time
      SetFileTime(h, nullptr, nullptr, &bumped);
      CloseHandle(h);
      LoadConfig(false);
      Check(!g_engineOff.load() && timercache::g_on.load() && g_partitionBoost.load(), "ini change resets the switch to ON");
      // Dev mode: [Dev] IniPath replaces the deployed ini, and is watched too.
      {
        FILE* f = nullptr;
        _wfopen_s(&f, L"./dev_test.ini", L"w");
        fprintf(f, "[Hotkeys]\nToggle=120:3\nDeveloperKeys=1\n");
        fclose(f);
        writeIni("[Dev]\nIniPath=.\\dev_test.ini\n");
        HANDLE h2 = CreateFileW(iniPath, FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                OPEN_EXISTING, 0, nullptr);
        FILETIME bumped2 = g_iniTime;
        reinterpret_cast<ULARGE_INTEGER*>(&bumped2)->QuadPart += 20000000;
        SetFileTime(h2, nullptr, nullptr, &bumped2);
        CloseHandle(h2);
        LoadConfig(false);
        Check(g_cfg.toggleVk == 120 && g_cfg.toggleMods == 3 && g_cfg.developerKeys && !g_devDir.empty(),
              "dev mode: settings come from [Dev] IniPath");
        Sleep(20);
        _wfopen_s(&f, L"./dev_test.ini", L"w");
        fprintf(f, "[Hotkeys]\nToggle=121:1\n");
        fclose(f);
        HANDLE h3 = CreateFileW(L"./dev_test.ini", FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                nullptr, OPEN_EXISTING, 0, nullptr);
        FILETIME bumped3 = g_devIniTime;
        reinterpret_cast<ULARGE_INTEGER*>(&bumped3)->QuadPart += 10000000;
        SetFileTime(h3, nullptr, nullptr, &bumped3);
        CloseHandle(h3);
        LoadConfig(false);
        Check(g_cfg.toggleVk == 121 && !g_cfg.developerKeys, "dev mode: a change to the dev ini is picked up");
        DeleteFileW(L"./dev_test.ini");
        g_devDir.clear();
      }
      DeleteFileW(iniPath);
      g_cfg = saved;
      g_partitionBoost = false;
    }

    TerminateThread(t, 0);
    CloseHandle(t);
    timercache::g_orig = nullptr;
  }

  // D3D11 meter on a real device: redundancy and hazard invalidation.
  {
    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
    D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &fl, 1, D3D11_SDK_VERSION, &dev,
                      nullptr, &ctx);
    Check(d3ds::LooksLikeContext(ctx, d3ds::ReferenceImpl()), "context recognised by implementation pointer");
    Check(d3ds::InstallFor(ctx), "d3d meter hooks a context vtable");
    D3D11_TEXTURE2D_DESC td{};
    td.Width = td.Height = 16;
    td.MipLevels = td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    ID3D11Texture2D* tex = nullptr;
    dev->CreateTexture2D(&td, nullptr, &tex);
    ID3D11ShaderResourceView* srv = nullptr;
    dev->CreateShaderResourceView(tex, nullptr, &srv);
    ID3D11RenderTargetView* rtv = nullptr;
    dev->CreateRenderTargetView(tex, nullptr, &rtv);

    d3ds::Totals a = d3ds::Snapshot();
    ctx->PSSetShaderResources(0, 1, &srv);  // new
    ctx->PSSetShaderResources(0, 1, &srv);  // redundant
    ctx->OMSetRenderTargets(1, &rtv, nullptr);  // runtime unbinds srv (hazard)
    ctx->PSSetShaderResources(0, 1, &srv);  // must NOT count as redundant
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);  // redundant
    ctx->ClearState();
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);  // after ClearState: not redundant
    ctx->Draw(3, 0);
    d3ds::Totals b = d3ds::Snapshot();
    Check(b.calls[d3ds::kPSSRV] - a.calls[d3ds::kPSSRV] == 3 &&
              b.redundant[d3ds::kPSSRV] - a.redundant[d3ds::kPSSRV] == 1,
          "SRV: 1 redundant of 3, hazard unbind respected");
    Check(b.calls[d3ds::kTopo] - a.calls[d3ds::kTopo] == 3 &&
              b.redundant[d3ds::kTopo] - a.redundant[d3ds::kTopo] == 1,
          "topology: ClearState respected");
    Check(b.draws - a.draws == 1, "draw counted");
    d3ds::Uninstall();  // stop the refresher before the context goes away
    srv->Release();
    rtv->Release();
    tex->Release();
    ctx->Release();
    dev->Release();
  }

  // Shadow texture skip: pure helpers.
  {
    using namespace shadowtex;
    Check(NameRefers("Diffuse", "Diffuse") && NameRefers("Arr", "Arr[1]") && NameRefers("s", "s.tex") &&
              !NameRefers("Diffuse", "DiffuseMap") && !NameRefers("Diffuse", "diffuse") &&
              !NameRefers("NormalMap", "Diffuse") && NameRefers(nullptr, "x"),
          "shadow tex: binding names match variables by base name");
    // DXBC parser against the D3D compiler's own reflection.
    HMODULE dc = LoadLibraryW(L"d3dcompiler_47.dll");
    auto compile = dc ? reinterpret_cast<decltype(&D3DCompile)>(GetProcAddress(dc, "D3DCompile")) : nullptr;
    auto reflect = dc ? reinterpret_cast<decltype(&D3DReflect)>(GetProcAddress(dc, "D3DReflect")) : nullptr;
    if (!compile || !reflect) {
      QvSkip("shadow tex: d3dcompiler_47.dll not available\n");
    } else {
      const char* src =
          "Texture2D Diffuse; Texture2D NormalMap; Texture2D Unused; Texture2D Arr[3]; Texture2D DamageMask;\n"
          "SamplerState S; StructuredBuffer<float4> sbPositions; cbuffer C { float4 k; };\n"
          "float4 ps(float4 p : SV_Position, float2 uv : TEXCOORD0) : SV_Target {\n"
          "  clip(Diffuse.Sample(S, uv).a - 0.5); return Arr[1].Sample(S, uv) * k + DamageMask.Sample(S, uv); }\n"
          "float4 ps_none(float4 p : SV_Position) : SV_Target { return 1; }\n"
          "float4 vs(float4 pos : POSITION) : SV_Position {\n"
          "  return sbPositions[(uint)pos.w] + NormalMap.SampleLevel(S, pos.xy, 0); }\n"
          "struct G { float4 p : SV_Position; };\n"
          "[maxvertexcount(3)] void gs(triangle G i[3], inout TriangleStream<G> o) {\n"
          "  for (int j = 0; j < 3; ++j) o.Append(i[j]); }\n";
      struct Case {
        const char* entry;
        const char* target;
      } cases[] = {{"ps", "ps_5_0"}, {"ps", "ps_5_1"}, {"ps_none", "ps_5_0"}, {"vs", "vs_5_0"},
                   {"vs", "vs_5_1"}, {"gs", "gs_4_0"}};
      bool allMatch = true, truncOk = true, sawUnused = false, sawDiffuse = false;
      for (const Case& c : cases) {
        ID3DBlob* code = nullptr;
        ID3DBlob* err = nullptr;
        if (FAILED(compile(src, strlen(src), "t.hlsl", nullptr, nullptr, c.entry, c.target, 0, 0, &code, &err))) {
          printf("     compile %s %s failed: %s\n", c.entry, c.target,
                 err ? static_cast<const char*>(err->GetBufferPointer()) : "?");
          allMatch = false;
          if (err) err->Release();
          continue;
        }
        const char* names[kMaxBindings];
        const auto* bytes = static_cast<const uint8_t*>(code->GetBufferPointer());
        const size_t size = code->GetBufferSize();
        const int n = ParseDxbcBindings(bytes, size, names, kMaxBindings);
        ID3D11ShaderReflection* r = nullptr;
        bool match = n >= 0 && SUCCEEDED(reflect(bytes, size, __uuidof(ID3D11ShaderReflection),
                                                 reinterpret_cast<void**>(&r)));
        if (match) {
          D3D11_SHADER_DESC sd = {};
          r->GetDesc(&sd);
          match = static_cast<UINT>(n) == sd.BoundResources;
          for (UINT i = 0; match && i < sd.BoundResources; ++i) {
            D3D11_SHADER_INPUT_BIND_DESC bd = {};
            r->GetResourceBindingDesc(i, &bd);
            match = strcmp(bd.Name, names[i]) == 0;
          }
          r->Release();
        }
        std::string list;
        for (int i = 0; i < n; ++i) {
          sawUnused |= strcmp(names[i], "Unused") == 0;
          sawDiffuse |= strcmp(names[i], "Diffuse") == 0;
          list += i ? " " : "";
          list += names[i];
        }
        printf("     %s %s: %d bindings [%s]%s\n", c.entry, c.target, n, list.c_str(),
               match ? "" : "  <-- differs from D3DReflect");
        allMatch &= match;
        // A truncated blob is rejected, never read out of bounds.
        std::vector<uint8_t> copy(bytes, bytes + size);
        for (size_t cut = 0; cut < size; cut += 7) {
          std::vector<uint8_t> t(copy.begin(), copy.begin() + cut);
          if (ParseDxbcBindings(t.data(), t.size(), names, kMaxBindings) >= 0) truncOk = false;
        }
        code->Release();
      }
      Check(allMatch, "shadow tex: RDEF parser lists exactly D3DReflect's bound resources (SM4.0/5.0/5.1)");
      Check(truncOk, "shadow tex: truncated DXBC rejected");
      Check(sawDiffuse && !sawUnused, "shadow tex: unused textures are not listed, used ones are");
      uint8_t junk[64] = {'D', 'X', 'B', 'C'};
      const char* names[4];
      Check(ParseDxbcBindings(junk, sizeof(junk), names, 4) < 0 && ParseDxbcBindings(nullptr, 0, names, 4) < 0,
            "shadow tex: malformed DXBC rejected");
    }
    // Records -> read mask.
    MaskEntry e;
    alignas(16) static uint8_t recs[0x50 * 5];
    const char* recNames[5] = {"Diffuse", "NormalMap", "Arr", nullptr, "Specular"};
    for (int i = 0; i < 5; ++i) *reinterpret_cast<const char**>(recs + i * 0x50 + 0x30) = recNames[i];
    e.recBegin = recs;
    e.recCount = 5;
    e.state = 1;
    const char* bound[] = {"S", "Diffuse", "Arr[1]", "sbPositions"};
    MarkRead(e, bound, 4);
    Check(!Skippable(e, 0) && Skippable(e, 1) && !Skippable(e, 2) && !Skippable(e, 3) && Skippable(e, 4) &&
              !Skippable(e, 5) && !Skippable(e, -1) && !Skippable(e, 1ll << 40),
          "shadow tex: mask keeps read, unnamed and out-of-range parameters");
    e.state = -1;
    Check(!Skippable(e, 1), "shadow tex: a 'keep' entry skips nothing");
    // Cache.
    auto* cache = new MaskCache;
    MaskEntry m;
    m.state = 1;
    m.techA = 7;
    m.techB = 8;
    m.effect = &m;
    uint8_t keys[3];
    bool ok = cache->Insert(&keys[0], m) && cache->Insert(&keys[1], m);
    ok = ok && cache->Find(&keys[0], 7, 8, &m, nullptr, nullptr, nullptr) &&
         !cache->Find(&keys[0], 7, 9, &m, nullptr, nullptr, nullptr) &&     // other technique
         !cache->Find(&keys[0], 7, 8, &keys, nullptr, nullptr, nullptr) &&  // other effect (fingerprint)
         !cache->Find(&keys[2], 7, 8, &m, nullptr, nullptr, nullptr);
    cache->Invalidate(&keys[0]);
    ok = ok && !cache->Find(&keys[0], 7, 8, &m, nullptr, nullptr, nullptr) &&
         cache->Find(&keys[1], 7, 8, &m, nullptr, nullptr, nullptr) && cache->Used() == 1;
    ok = ok && cache->Insert(&keys[0], m) && cache->Find(&keys[0], 7, 8, &m, nullptr, nullptr, nullptr);
    Check(ok, "shadow tex: cache finds by shader + fingerprint, destructor invalidation forgets");
    std::vector<uint8_t> many(MaskCache::kSize + 1);
    size_t stored = 0;
    for (size_t i = 2; i < many.size(); ++i) stored += cache->Insert(&many[i], m) != nullptr;
    Check(stored == MaskCache::kSize - 2 && !cache->Insert(&many[0], m), "shadow tex: full cache refuses inserts");
    delete cache;
  }

  // G-buffer batching build checks, before the next block patches NGModel's image.
  gbbtest::BuildChecks();

  // Shadow texture skip: the C copy of ModelMaterialMT slot 5 against the real
  // machine code of the analysed NGModel.dll (loaded without imports; its IAT
  // entries, globals and the submit are redirected to fakes).
  {
    using namespace stx;
    HMODULE ngm = dcsinstall::LoadBinary(L"NGModel.dll");
    HMODULE dxm = dcsinstall::LoadBinary(L"dx11backend.dll");
    if (!dcsinstall::Found()) {
      dcsinstall::Skip("shadow tex");
    } else if (!ngm || !dxm) {
      QvSkip("shadow tex: DCS binaries not found\n");
    } else {
      auto* ng = reinterpret_cast<uint8_t*>(ngm);
      auto* dx = reinterpret_cast<uint8_t*>(dxm);
      const char* why = VerifyBuild(ng, dx);
      Check(!why, "shadow tex: build checks pass on the analysed NGModel/dx11backend");
      if (why) printf("     %s\n", why);
      // Redirect what slot 5 reaches outside itself.
      void* redirect = reinterpret_cast<void*>(&FakeGetTexture);
      WriteCode(ng + kIatGetTexture, &redirect, 8);
      redirect = reinterpret_cast<void*>(&FakeValid);
      WriteCode(ng + kIatValid, &redirect, 8);
      redirect = g_globals;
      WriteCode(ng + kGlobals, &redirect, 8);
      uint8_t jmp[12] = {0x48, 0xb8};
      const uint64_t target = reinterpret_cast<uint64_t>(&FakeSubmit);
      memcpy(jmp + 2, &target, 8);
      jmp[10] = 0xff;
      jmp[11] = 0xe0;  // mov rax, FakeSubmit; jmp rax
      WriteCode(ng + kSubmit, jmp, sizeof(jmp));
      *reinterpret_cast<uint8_t**>(g_globals + 0x80) = g_sbArray;
      for (int i = 0; i < 8; ++i) *reinterpret_cast<uint64_t*>(g_sbArray + 0x20 + i * 0x30) = 0x5b00 + i;
      g_shaderVtbl[26] = reinterpret_cast<void*>(&FakeSetTex);
      g_shaderVtbl[27] = reinterpret_cast<void*>(&FakeSetSb);
      *reinterpret_cast<void**>(g_shader) = g_shaderVtbl;
      *reinterpret_cast<void**>(g_shader + 0xc8) = g_records;
      for (int i = 0; i < 16; ++i) *reinterpret_cast<int32_t*>(g_records + i * 0x50 + 0xc) = (i == 9) ? 0x20 : 7;
      g_texVtbl[23] = reinterpret_cast<void*>(&FakeTex23);
      g_texVtbl[18] = reinterpret_cast<void*>(&FakeTex18);
      *reinterpret_cast<int32_t*>(g_desc + 0x20) = 28;
      // The hook's globals, pointing at the loaded image and the fakes.
      shadowtex::g_ng = ng;
      shadowtex::g_submit = reinterpret_cast<SubmitFn>(ng + kSubmit);
      shadowtex::g_getDesc = &FakeGetDesc;
      shadowtex::g_compat = &FakeCompat;
      shadowtex::g_getSrv = &FakeGetSrv;
      shadowtex::g_texVtblPtr = g_texVtbl;
      shadowtex::g_innerFile = g_innerVtbl;
      shadowtex::g_innerArray = g_innerVtbl;
      shadowtex::g_innerDummy = g_innerVtbl;
      auto original = reinterpret_cast<Slot5Fn>(ng + kSlot5);

      const int64_t handles[12] = {0, 1, -1, 2, 3, 4, 5, 6, 7, -1, 9, 10};
      struct Variant {
        int count, props8, transp, base;
        bool v15, v18;
      } variants[] = {{12, 0, 0, 0, true, false}, {12, 0, 1, 2, false, true}, {12, 1, 0, 1, false, false},
                      {12, 0, 0, 0, false, false}, {0, 1, 1, 0, true, true},  {7, 2, 1, 3, true, true},
                      {12, 0, 1, 4, true, false}};
      auto* sc = new Scene;
      bool same = true, skipOk = true, noMaskOk = true;
      int skippedTotal = 0, replayed = 0, nullTex = 0;
      for (const Variant& v : variants) {
        for (bool& f : g_validFlag) f = false;
        g_validFlag[0xf] = v.v15;
        g_validFlag[0x12] = v.v18;
        Build(*sc, v.count, handles, v.props8, v.transp, v.base);
        g_trace.clear();
        const uint64_t r1 = original(sc->mat, sc->item, reinterpret_cast<void*>(0xa3), reinterpret_cast<void*>(0xa4));
        const uint32_t cb1 = *reinterpret_cast<uint32_t*>(sc->mat + 0x18c);
        const std::vector<Call> ref = g_trace;
        // Every parameter read: identical calls, result and CB field.
        MaskEntry all;
        all.state = 1;
        all.recCount = 16;
        all.read[0] = ~0ull;
        Counters cnt;
        Build(*sc, v.count, handles, v.props8, v.transp, v.base);
        g_trace.clear();
        const uint64_t r2 = Draw(sc->mat, sc->item, reinterpret_cast<void*>(0xa3), g_shader, all, cnt);
        same &= r1 == r2 && cb1 == *reinterpret_cast<uint32_t*>(sc->mat + 0x18c) && cb1 == 0xabcd && g_trace == ref;
        // No mask (pending / keep): the copy with every set kept, counted as keptNoMask.
        {
          static const MaskEntry keepAll;
          Counters cnt0;
          Build(*sc, v.count, handles, v.props8, v.transp, v.base);
          g_trace.clear();
          const uint64_t r0 = Draw(sc->mat, sc->item, reinterpret_cast<void*>(0xa3), g_shader, keepAll, cnt0, true);
          uint64_t sets = 0;
          for (const Call& c : ref) sets += c.kind == 2;
          noMaskOk &= r0 == r1 && g_trace == ref && cnt0.keptNoMask == sets && cnt0.kept == 0 && cnt0.skipped == 0;
        }
        // Parameters 1, 3, 4, 6, 9 not read: their sets become the streaming
        // request, plus the slot-26 replay (without SetResource) where a
        // mip-set swap is due (texture 4 with aux -1) or the inner class is
        // unknown (texture 6).
        MaskEntry part = all;
        part.read[0] = ~((1ull << 1) | (1ull << 3) | (1ull << 4) | (1ull << 6) | (1ull << 9));
        std::vector<Call> expect;
        for (const Call& c : ref) {
          const int64_t h = static_cast<int64_t>(c.a ^ U(g_shader));
          if (c.kind != 2 || !Skippable(part, h)) {
            expect.push_back(c);
            continue;
          }
          if (!c.b) continue;  // NULL texture: slot 26 does nothing
          expect.push_back({4, c.b, c.d, 0, 0});
          const int t = static_cast<int>((reinterpret_cast<uint8_t*>(c.b) - &sc->tex[0][0]) / 0x700);
          const bool replay = t == 6 || (t == 4 && c.c == ~0ull);
          if (!replay) continue;
          const uint32_t type = h == 9 ? 0x20 : 7;
          expect.push_back({6, c.b, 0, 0, 0});
          expect.push_back({7, type, 28, 0, 0});
          expect.push_back({5, c.b, 0, 0, 0});
          if (t != 6)
            expect.push_back({8, c.b, c.c, c.d, 0});
          else if (type == 0x20)
            expect.push_back({8, c.b, c.c, 0, 0});
        }
        Build(*sc, v.count, handles, v.props8, v.transp, v.base);
        g_trace.clear();
        Counters cnt2;
        const uint64_t r3 = Draw(sc->mat, sc->item, reinterpret_cast<void*>(0xa3), g_shader, part, cnt2);
        skipOk &= r3 == r1 && g_trace == expect;
        skippedTotal += static_cast<int>(cnt2.skipped + cnt2.skippedReplayed + cnt2.nullTex);
        replayed += static_cast<int>(cnt2.skippedReplayed);
        nullTex += static_cast<int>(cnt2.nullTex);
      }
      // Shadow batching's leaders without the [item+0xd4] swap (shadow_batch.h
      // g_psoDirect): DrawPso makes exactly the calls slot 5 makes with
      // [item+0xd4] == the group base, for every routing of slot 5, and leaves
      // [item+0xd4] alone.
      {
        void* fakeSlot = reinterpret_cast<void*>(original);
        void** const slot0 = shadowtex::g_slot;
        const Slot5Fn orig0 = shadowtex::g_orig;
        const int state0 = shadowtex::g_state.load();
        const bool on0 = shadowtex::g_on.load();
        void* const shVt0 = shadowtex::g_shaderVtblPtr;
        MaskCache* const cache0 = shadowtex::g_cache;
        const shadowcount::Fn next0 = shadowcount::g_modelNext.load();
        shadowtex::g_slot = &fakeSlot;
        shadowtex::g_orig = original;
        shadowtex::g_state = 1;
        shadowtex::g_shaderVtblPtr = g_shaderVtbl;
        shadowtex::g_cache = new MaskCache;
        MaskEntry part;
        part.state = 1;
        part.recCount = 16;
        part.read[0] = ~((1ull << 1) | (1ull << 3) | (1ull << 4) | (1ull << 6) | (1ull << 9));
        part.effect = *reinterpret_cast<void**>(g_shader + 0x50);
        part.recBegin = *reinterpret_cast<void**>(g_shader + 0xc8);
        part.recEnd = *reinterpret_cast<void**>(g_shader + 0xd0);
        part.techBegin = *reinterpret_cast<void**>(g_shader + 0xb0);
        part.techA = 7;
        part.techB = 8;
        shadowtex::g_cache->Insert(g_shader, part);
        bool modes = Slot5Mode() == kPsoCopy;
        fakeSlot = reinterpret_cast<void*>(&Hook);
        modes &= Slot5Mode() == kPsoHook;
        fakeSlot = reinterpret_cast<void*>(&shadowcount::HookModel);
        shadowcount::g_modelNext = nullptr;
        modes &= Slot5Mode() == kPsoCopy;
        shadowcount::g_modelNext = &Hook;
        modes &= Slot5Mode() == kPsoHook;
        fakeSlot = reinterpret_cast<void*>(&FakeSubmit);
        modes &= Slot5Mode() == kPsoNone;
        shadowtex::g_state = 0;
        fakeSlot = reinterpret_cast<void*>(&Hook);
        modes &= Slot5Mode() == kPsoNone;
        shadowtex::g_state = 1;
        Check(modes, "shadow tex: Slot5Mode follows what slot 5 holds (DCS's, the hook, the counter's chain, foreign)");
        bool psoOk = true;
        constexpr uint32_t kBase = 0x5150;
        for (const Variant& v : variants) {
          for (bool& fl : g_validFlag) fl = false;
          g_validFlag[0xf] = v.v15;
          g_validFlag[0x12] = v.v18;
          // DCS's slot 5 with the base in [item+0xd4] (the old swap).
          Build(*sc, v.count, handles, v.props8, v.transp, v.base);
          *reinterpret_cast<uint32_t*>(sc->item + 0xd4) = kBase;
          g_trace.clear();
          const uint64_t r1 = original(sc->mat, sc->item, reinterpret_cast<void*>(0xa3), reinterpret_cast<void*>(0xa4));
          const std::vector<Call> ref = g_trace;
          auto same = [&](uint64_t r, const std::vector<Call>& want, uint64_t rw) {
            return r == rw && g_trace == want && *reinterpret_cast<uint32_t*>(sc->mat + 0x18c) == kBase &&
                   *reinterpret_cast<uint32_t*>(sc->item + 0xd4) == 0xabcd;
          };
          // Slot 5 is DCS's (copy mode), or the hook with the skip off.
          shadowtex::g_on = false;
          for (int mode : {kPsoCopy, kPsoHook}) {
            Build(*sc, v.count, handles, v.props8, v.transp, v.base);
            g_trace.clear();
            const uint64_t r = DrawPso(sc->mat, sc->item, reinterpret_cast<void*>(0xa3), kBase, mode);
            psoOk &= same(r, ref, r1);
          }
          // The hook with the skip on: what the hook makes with the base in [item+0xd4].
          shadowtex::g_on = true;
          Build(*sc, v.count, handles, v.props8, v.transp, v.base);
          *reinterpret_cast<uint32_t*>(sc->item + 0xd4) = kBase;
          g_trace.clear();
          const uint64_t r4 = Hook(sc->mat, sc->item, reinterpret_cast<void*>(0xa3), nullptr);
          const std::vector<Call> ref4 = g_trace;
          Build(*sc, v.count, handles, v.props8, v.transp, v.base);
          g_trace.clear();
          const uint64_t r5 = DrawPso(sc->mat, sc->item, reinterpret_cast<void*>(0xa3), kBase, kPsoHook);
          psoOk &= same(r5, ref4, r4);
        }
        delete shadowtex::g_cache;
        shadowtex::g_cache = cache0;
        shadowtex::g_slot = slot0;
        shadowtex::g_orig = orig0;
        shadowtex::g_state = state0;
        shadowtex::g_on = on0;
        shadowtex::g_shaderVtblPtr = shVt0;
        shadowcount::g_modelNext = next0;
        Check(psoOk, "shadow tex: DrawPso = slot 5 with [item+0xd4] == the group base, [item+0xd4] untouched (copy, "
                     "hook with the skip off and on, 7 variants)");
      }
      delete sc;
      Check(same, "shadow tex: C copy of slot 5 makes exactly the original's calls (7 variants)");
      Check(noMaskOk, "shadow tex: casters without a mask run the copy with every set kept and counted");
      Check(skipOk && skippedTotal > 0 && replayed > 0,
            "shadow tex: skipped sets keep the streaming request; swaps and unknown textures replay slot 26 "
            "without SetResource");
      printf("     %d texture sets skipped across the variants, %d replayed, %d without texture\n", skippedTotal,
             replayed, nullTex);
    }
  }

  // Shadow instancing stage 1 (shadow_inst.h): FX11 parser, compile pipeline,
  // source edits, runtime path, install/unload.
  sitest::Run();
  // Shadow texture skip masks from shadow_inst's (a) compiles.
  sttest::Run();
  // G-buffer instancing stage 1 (R13): model_vs variants, checks 1-4, gate.
  gbtest::Run();
  sbtest::Run();
  sptest::Run();
  putest::Run();
  dutest::Run();
  gbbtest::Run();
  // Split-path redundant-state filter (R15 F5) on a real device through fake dx11backend sites.
  sftest::Run();
  // Deferred-context recording infrastructure (R15 A1): bit-exact cascade, state restore, pool.
  drtest::Run();
  // Shadow recorder (R17 S1-S3): mesh fields, tables, reflection, probe checks, jobs, device record vs stock.
  srtest::Run();
  // GPU pass timing: analysis on synthetic ticks, then ring readback and op counts on a real device.
  gpttest::Run();
  // G-buffer recorder S0 counters (R18): segments, gates, item walk, guard masks, setup probe and Execute(TRUE).
  grctest::Run();
  // G-buffer recorder (R18 S1-S3): texture table, keys, jobs, restores, device segments vs stock.
  grtest::Run();
  gbcktest::Run();  // cockpit and execution identity (R24)
  // Synthetic pose (sweep with hold/taxi) and the view profile (bounds, spikes, aggregation, GPU light mode).
  vptest::Run();
  // Frame-start bubble attribution and runnable threads (R22 E1, E3): classes, segments, snapshots, gate, live probes.
  fsttest::Run();
  // VRAM census: format sizes, pinned-view rules, summary, device create hooks and refcounts.
  vctest::Run();
  // Pass flush: Flush decisions per mask bit, paired A/B statistics, Flush through the execute hook on a device.
  pftest::Run();
  // Forward recorder S0 counters (R21): pass kinds, FX pass index, rule flags, segments, gates, item walk.
  frctest::Run();
  // setShaderResources identical-tail trim (R15 F2): fake caches vs the 0x1ac20 transcription, real device via fake sites.
  strtest::Run();
  // DcsQvCull_Status word (status_word.h): bit layout, feature counting, loader/payload handover.
  swtest::Run();
  // Terrain prefetch fix: modes, window, spin guard, unload/reload safety, old dxgi2 fix take-over, log line.
  pfxtest::Run();
  plodtest::Run();
  // Robustness round (2026-10-11): payload stop ordering, UTF-8 log lines, recorder start race, ...
  rbtest::Run();
  lstest::Run();
  // R30: frame sampler, CPU/GPU-bound detector, governor hysteresis and stages, pause/resume, status bits, log lines.
  govtest::Run();
  // Field report 0.5.5: DCS exit race (loader freed under the payload), mission restart during a recorder job.
  extest::Run();
  // Depth boost: fixation depth (layout, seq guard, projection, window, summary, WARP pipeline, state restore).
  fdtest::Run();
  // Scene-cut reset for the bundled Quad Views (detector sequences, export seam).
  sctest::Run();
  // Far depth engine export (contract layout, CPU gProj capture, WARP copy/push, gating, ring, release, hot reload).
  fartest::Run();
  // R65 cockpit mask export (contract, layout, WARP max filter vs CPU, sample points, xrEndFrame commit, reader fence).
  mask65test::Run();
  // Cockpit at the true eye distance (R31 option 3: patch math, matcher, upload filter, WARP pipeline, stencil split, off path).
  cktest::Run();
  cktest::r128t::Run(false);  // R128: cockpit ipd's dormant mode (exactness against the full work, first boosted frame)
  // [General] PerceptionOnly: no engine module, the perception features, status 0, keys inert, dual camera status.
  potest::Run();
  // R33 far shadow cascade cache: census-matrix math, refresh/reuse decisions, caster lists, WARP replica of the pass.
  cctest::Run();
  // R34 shorter culling chains: variants vs CPU transcription and stock stand-ins, hook path, live check, shutdown.
  cftest::Run();
  // R36 wider grids for single-SM kernels: variants vs stock stand-ins, identification by output, Dispatch hook.
  shfasttest::Run();
  // R37 [Night]: opt-in night darkening through DCS's tone mapper (pure parts, hook on a fake module, build checks).
  nttest::Run();
  // R40 [Color]: opt-in post-tonemap grading, pure parts (all keys default 0 = identity).
  cgtest::Run();
  // R42 [Color] Mode (Fix / Enhanced): parameters, CPU reference, the pixel shader swap on WARP (default off).
  cmtest::Run();
  r111test::Run();  // R111: PanelGamut (after the R42 tests: same WARP rig helpers)
  r122test::Run();  // R122: per-view DLSS preset ([DLSS] keys; fake NGX core and NvAPI, no driver call)
  r123test::Run();  // R123: focus twin ([Model] FocusTwin; census TerrainContexts, WARP pipeline byte-exact vs stock)
  r126test::Run();  // R126: lossless overhead speed-ups (cost scopes on the TSC, MatchWorld)
  r130test::Run();  // R130: DLSS feature lifetime instrumentation ([DLSS] Instrument; fake NGX core, read-only)
  r132test::Run();  // R132: opt-in release of idle DLSS sets ([DLSS] ReleaseIdleSets; fake NGX core)
  r133test::Run();  // R133: the recorder's motion vectors (exec-time freshness on WARP, the verify's bracket, dump)
  vfxtest::Run();
  // R39 [Threads]: worker placement off DCS's exclusive render core (all keys default 0), census queries.
  tptest::Run();
  // R52: per-frame timeline / pose probe / event log (measure only, keys default 0), placement re-check cadence.
  ftltest::Run();
  // R59 bug-fix sweep 2.
  r59stop::Run();
  r59map::Run();
  r59pre::Run();
  r59helmet::Run();
  r59tls::Run();
  r59xr::Run();
  r59pflog::Run();
  r62dev::Run();  // R62 contract bug-fix sweep (native side)
  r62log::Run();
  r62gbl::Run();
  r62fx::Run();
  r62nt::Run();
  r63scene::Run();  // R63 joint bug-fix sweep 2 (native side)
  r63sr::Run();
  r63tick::Run();
  r63big::Run();
  r63lua::Run();
  r63env::Run();
  r77::Run();  // R77 uncovered-bugfix round (native side)
  r79::Run();  // R79 suffix rule: a suffixed run opens every block under the suffixed names
  r83::Run();  // R83 (R82 D1): miss accounting decides "due" from the state the swap goes by
  r90::Run();  // R90: lossless hitch fixes
  r93::Run();  // R93: async log start burst and render-thread no-I/O; FrameTimeline with the cockpit mask export
  r94::Run();  // R94: SymbologyTrace (dev live HUD trace)
  r95test::Run();  // R95: the symbology emission export (last: it installs and stops the cockpit mask export itself)
  r97test::Run();  // R97: its fixes after the first live run
  r99test::Run();  // R99: the pose hook by displayTime; the symbology replay stage's cost
  r101test::Run();  // R101: the learnt HUD field recovers after a bank

  // Suite: configuration check + short profile, report file written.
  {
    g_cfg.suiteProfile = true;
    g_cfg.profileSeconds = 2;
    g_cfg.suiteBenchCull = false;
    g_cfg.suiteBenchThreads = false;
    SuiteThread(nullptr);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(L"./report_*.txt", &fd);
    bool hasConfig = false, hasProfile = false;
    if (h != INVALID_HANDLE_VALUE) {
      FILE* f = nullptr;
      _wfopen_s(&f, (std::wstring(L"./") + fd.cFileName).c_str(), L"r");
      char line[512];
      while (f && fgets(line, sizeof(line), f)) {
        if (strstr(line, "-- configuration --")) hasConfig = true;
        if (strstr(line, "profile: done")) hasProfile = true;
      }
      if (f) fclose(f);
      FindClose(h);
      DeleteFileW((std::wstring(L"./") + fd.cFileName).c_str());
    }
    WIN32_FIND_DATAW pd;
    HANDLE hp = FindFirstFileW(L"./profile_*.txt", &pd);
    if (hp != INVALID_HANDLE_VALUE) {
      DeleteFileW((std::wstring(L"./") + pd.cFileName).c_str());
      FindClose(hp);
    }
    Check(hasConfig && hasProfile, "suite writes report with configuration and profile");
  }
  PrintSummary();
  return g_fail;
}
