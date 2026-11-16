// Rig capture layer ("rig tap"): TEST-ONLY OpenXR API layer. Not a shipped component, never registered or installed.
// Loaded only through the environment of a dev launch, LAST in XR_ENABLE_API_LAYERS (closest to the runtime), so it
// sees exactly what the frame-generation layer (OFXR) hands to the runtime.
//
// Graphics binding below OFXR: D3D12 (OFXR's D3D11 bridge opens the runtime session with an XrGraphicsBindingD3D12KHR
// on its own device, external\ofxr-djules75\src\layer\openxr_layer.cpp 6530-6536). Pixels are read back on D3D12
// only; on any other binding the layer records the metadata and says "no pixels".
//
// Idle: every call is forwarded; xrEndFrame costs one atomic load and, four times a second, one file-attribute query.
// Armed (trigger file <RIGTAP_DIR>\arm.txt, or RIGTAP_FRAMES / RIGTAP_AFTER_S): for N consecutive xrEndFrame calls
// (default 40, hard cap 120) it writes one JSON line per submission and the RGBA bytes of every projection view's
// sub-image. The application's call is never failed: any error ends the capture and is logged.
//
//   RIGTAP_DIR        output folder (without it the layer only forwards)
//   RIGTAP_FRAMES     arm once from the environment: number of submissions
//   RIGTAP_AFTER_S    ... starting this many seconds after the session's first xrEndFrame (default 0)
//   RIGTAP_SCALE      default integer downscale (box average), 1 = none
//   RIGTAP_CROP       default crop of every sub-image, fractions x0,y0,x1,y1 (0,0,1,1 = all)
//   RIGTAP_MAX_MB     disk budget of the whole process (default 6144)
//   RIGTAP_EXTENSIONS answer to a null-instance xrEnumerateInstanceExtensionProperties when no loader is found
//                     (names separated by ';'; default: the Pimax runtime's list)
// arm.txt (key=value per line, all optional): frames, delay_ms, start_display_time (ns, XrTime), scale, crop, tag,
//   window0 / window1 = tangents l,r,u,d kept of view 0 / 1 of every projection layer (instead of crop: the same
//   angular window whatever the image size), scale2 (+ crop2) = a second file per view (whole sub-image at that scale),
//   budget_mb (default 2048) = size of the whole capture: the first stream's scale is raised until it fits.
// A scale is a box average done on the GPU when the texture takes a raw (UNORM) shader view, so only the small image
// is read back; else the region is read at full size and averaged on the writer thread.
// The file is renamed to arm.taken-<n>.txt when read. Each capture goes to <RIGTAP_DIR>\cap<n>[_tag]\ with
// capture.jsonl, the .rgba files and, when everything is on disk, done.txt.
#define XR_USE_GRAPHICS_API_D3D11
#define XR_USE_GRAPHICS_API_D3D12
#define NOMINMAX
#include <windows.h>
#include <psapi.h>
#include <d3d11.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <cmath>
#include <atomic>
#include <condition_variable>
#include <cstdarg>
#include <cstdlib>
#include <cwctype>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <openxr/openxr_loader_negotiation.h>

namespace {
const uint32_t kDefaultFrames = 40, kHardCap = 120;
PFN_xrGetInstanceProcAddr g_nextGipa = nullptr;
PFN_xrEndFrame g_nextEnd = nullptr;
PFN_xrCreateSession g_nextCreateSession = nullptr;
PFN_xrDestroySession g_nextDestroySession = nullptr;
PFN_xrCreateSwapchain g_nextCreateSc = nullptr;
PFN_xrDestroySwapchain g_nextDestroySc = nullptr;
PFN_xrEnumerateSwapchainImages g_nextEnumImages = nullptr;
PFN_xrAcquireSwapchainImage g_nextAcquire = nullptr;
PFN_xrReleaseSwapchainImage g_nextRelease = nullptr;
PFN_xrCreateReferenceSpace g_nextCreateRef = nullptr;
// RIGTAP_INVALID_MS=N (0089 confirmation, 2026-11-16): for N ms from the session's first xrLocateViews on a space that is
// not a VIEW reference space, the view state's VALID and TRACKED bits are cleared on the way up, as the runtime
// itself answers while the headset is not tracked yet. Nothing else of the answer is touched. Off without the key.
PFN_xrLocateViews g_nextLocate = nullptr; double g_invalidMs = 0; long long g_invalidFrom = 0; std::atomic<unsigned> g_invalidCalls{0}; std::map<XrSpace, int> g_spaceTypeOf; std::mutex g_spaceMu;

struct Sc {
  uint64_t id = 0; XrSession session = XR_NULL_HANDLE; uint32_t w = 0, h = 0, array = 0, mips = 0, samples = 0, faces = 0; int64_t fmt = 0; uint64_t usage = 0, createFlags = 0;
  long long createdQpc = 0; int dxgiFormat = 0; std::vector<ID3D12Resource*> images; std::deque<uint32_t> acquired; int lastReleased = -1; uint64_t releaseSeq = 0; bool alive = true;
};
std::mutex g_mu;                                   // swapchain table, session, capture state
std::map<XrSwapchain, Sc> g_sc; std::map<XrSpace, int> g_space; uint64_t g_scNext = 0;
int g_api = 0;                                     // 0 unknown, 11, 12, -1 other
XrSession g_session = XR_NULL_HANDLE; ID3D12Device* g_dev = nullptr; ID3D12CommandQueue* g_queue = nullptr;
ID3D12CommandAllocator* g_alloc = nullptr; ID3D12GraphicsCommandList* g_list = nullptr; ID3D12Fence* g_fence = nullptr; HANDLE g_fenceEvent = nullptr; uint64_t g_fenceValue = 0;
ID3D12Resource* g_readback = nullptr; uint64_t g_readbackSize = 0;

std::wstring g_dir; bool g_enabled = false; FILE* g_log = nullptr; long long g_qpf = 1;
enum : int { Idle = 0, Armed = 1, Capturing = 2 };
std::atomic<int> g_state{Idle}; std::atomic<uint64_t> g_endFrames{0};
ULONGLONG g_lastPoll = 0; long long g_firstEndQpc = 0; bool g_envArmPending = false; uint32_t g_envFrames = 0; double g_envAfterS = 0;
uint32_t g_defScale = 1; double g_defCrop[4] = {0, 0, 1, 1}; uint64_t g_maxBytes = 6144ull << 20; std::atomic<uint64_t> g_diskBytes{0};
struct Arm { uint32_t frames = kDefaultFrames, scale = 1, scale2 = 0; double crop[4] = {0, 0, 1, 1}, crop2[4] = {0, 0, 1, 1}, window[2][4] = {}; bool hasWindow[2] = {false, false}; uint64_t budgetBytes = 2048ull << 20; long long startQpc = 0; XrTime startDisplayTime = 0; std::string tag; } g_arm;
uint32_t g_capIndex = 0, g_capDone = 0; std::wstring g_capDir; std::string g_endReason;

long long Qpc() { LARGE_INTEGER q; QueryPerformanceCounter(&q); return q.QuadPart; }
void Log(const char* fmt, ...) {
  if (!g_log) return; SYSTEMTIME t; GetLocalTime(&t); fprintf(g_log, "%02d:%02d:%02d.%03d ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
  va_list a; va_start(a, fmt); vfprintf(g_log, fmt, a); va_end(a); fputc('\n', g_log); fflush(g_log);
}
std::wstring Widen(const std::string& s) { int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0); std::wstring w(n > 0 ? n - 1 : 0, L'\0'); if (n > 1) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &w[0], n); return w; }
std::string EnvA(const char* name) { char b[2048]; DWORD n = GetEnvironmentVariableA(name, b, sizeof(b)); return (n > 0 && n < sizeof(b)) ? std::string(b, n) : std::string(); }
bool ParseCrop(const std::string& s, double c[4]) { double v[4]; if (sscanf(s.c_str(), "%lf,%lf,%lf,%lf", &v[0], &v[1], &v[2], &v[3]) != 4) return false; if (!(v[0] >= 0 && v[1] >= 0 && v[2] <= 1 && v[3] <= 1 && v[2] > v[0] && v[3] > v[1])) return false; memcpy(c, v, sizeof(v)); return true; }

// ---- writer thread: everything that touches the disk during a capture --------------------------------------------
struct Job { int kind = 0; std::wstring path; std::vector<uint8_t> data; uint32_t w = 0, h = 0, scale = 1; std::string text; };   // 0 pixels, 1 json line, 2 end of capture
std::mutex g_jobMu; std::condition_variable g_jobCv; std::deque<std::unique_ptr<Job>> g_jobs; std::atomic<uint64_t> g_queuedBytes{0}; std::thread g_writer; bool g_writerOn = false; std::atomic<bool> g_writeFailed{false};
const uint64_t kMaxQueuedBytes = 3072ull << 20;
void WriterMain() {
  FILE* jsonl = nullptr; std::wstring jsonlPath;
  for (;;) {
    std::unique_ptr<Job> j;
    { std::unique_lock<std::mutex> l(g_jobMu); g_jobCv.wait(l, [] { return !g_jobs.empty(); }); j = std::move(g_jobs.front()); g_jobs.pop_front(); }
    if (j->kind == 0) {
      const uint8_t* src = j->data.data(); std::vector<uint8_t> shrunk; uint32_t w = j->w, h = j->h;
      if (j->scale > 1) { uint32_t s = j->scale, ow = w / s, oh = h / s; shrunk.resize(size_t(ow) * oh * 4);
        for (uint32_t y = 0; y < oh; ++y) for (uint32_t x = 0; x < ow; ++x) { uint32_t acc[4] = {0, 0, 0, 0};
          for (uint32_t yy = 0; yy < s; ++yy) { const uint8_t* p = src + (size_t(y * s + yy) * w + x * s) * 4; for (uint32_t xx = 0; xx < s; ++xx, p += 4) { acc[0] += p[0]; acc[1] += p[1]; acc[2] += p[2]; acc[3] += p[3]; } }
          uint8_t* o = &shrunk[(size_t(y) * ow + x) * 4]; uint32_t n = s * s; for (int c = 0; c < 4; ++c) o[c] = uint8_t((acc[c] + n / 2) / n); }
        src = shrunk.data(); w = ow; h = oh; }
      size_t bytes = size_t(w) * h * 4; FILE* f = _wfopen(j->path.c_str(), L"wb");
      if (!f || fwrite(src, 1, bytes, f) != bytes) g_writeFailed = true;
      if (f) fclose(f); g_diskBytes += bytes; g_queuedBytes -= j->data.size();
    } else if (j->kind == 1) {
      if (jsonlPath != j->path) { if (jsonl) fclose(jsonl); jsonl = _wfopen(j->path.c_str(), L"ab"); jsonlPath = j->path; }
      if (jsonl) { fwrite(j->text.data(), 1, j->text.size(), jsonl); fputc('\n', jsonl); fflush(jsonl); } else g_writeFailed = true;
    } else if (j->kind == 2) {
      if (jsonl) { fclose(jsonl); jsonl = nullptr; jsonlPath.clear(); }
      FILE* f = _wfopen(j->path.c_str(), L"wb"); if (f) { fwrite(j->text.data(), 1, j->text.size(), f); fclose(f); }
    } else return;
  }
}
void Push(std::unique_ptr<Job> j) {
  if (!g_writerOn) { g_writer = std::thread(WriterMain); g_writer.detach(); g_writerOn = true; }
  g_queuedBytes += j->data.size(); { std::lock_guard<std::mutex> l(g_jobMu); g_jobs.push_back(std::move(j)); } g_jobCv.notify_one();
}
void PushText(int kind, const std::wstring& path, const std::string& text) { auto j = std::make_unique<Job>(); j->kind = kind; j->path = path; j->text = text; Push(std::move(j)); }

// ---- arming ------------------------------------------------------------------------------------------------------
std::string SwapchainTableJson() {
  std::vector<const Sc*> v; std::vector<XrSwapchain> hs; for (auto& kv : g_sc) { v.push_back(&kv.second); hs.push_back(kv.first); }
  std::string s = "["; char b[512]; bool first = true;
  for (uint64_t id = 0; id < g_scNext; ++id) for (size_t i = 0; i < v.size(); ++i) if (v[i]->id == id) {
    snprintf(b, sizeof(b), "%s{\"id\":%llu,\"handle\":%llu,\"w\":%u,\"h\":%u,\"array\":%u,\"mips\":%u,\"samples\":%u,\"format\":%lld,\"dxgiFormat\":%d,\"usage\":%llu,\"createFlags\":%llu,\"images\":%zu,\"createdQpc\":%lld,\"alive\":%s}",
      first ? "" : ",", (unsigned long long)v[i]->id, (unsigned long long)hs[i], v[i]->w, v[i]->h, v[i]->array, v[i]->mips, v[i]->samples, (long long)v[i]->fmt, v[i]->dxgiFormat, (unsigned long long)v[i]->usage, (unsigned long long)v[i]->createFlags, v[i]->images.size(), v[i]->createdQpc, v[i]->alive ? "true" : "false");
    s += b; first = false; }
  return s + "]";
}
void BeginCapture() {   // g_mu held
  wchar_t name[160]; std::wstring tag = Widen(g_arm.tag); for (auto& c : tag) if (!(iswalnum(c) || c == L'-' || c == L'_')) c = L'_';
  swprintf(name, 160, L"\\cap%02u%s%s", g_capIndex, tag.empty() ? L"" : L"_", tag.substr(0, 60).c_str());
  g_capDir = g_dir + name; CreateDirectoryW(g_capDir.c_str(), nullptr); g_capDone = 0; g_endReason.clear();
  char b[900]; snprintf(b, sizeof(b), "{\"header\":1,\"version\":1,\"pid\":%lu,\"api\":%d,\"frames\":%u,\"scale\":%u,\"crop\":[%.6f,%.6f,%.6f,%.6f],\"qpcFreq\":%lld,\"armQpc\":%lld,\"startDisplayTime\":%lld,\"endFramesBefore\":%llu,\"pixelLayout\":\"rgba8 rows top-down, w*h*4 bytes per file (bgra:true = bytes are B,G,R,A)\",\"swapchains\":",
    GetCurrentProcessId(), g_api, g_arm.frames, g_arm.scale, g_arm.crop[0], g_arm.crop[1], g_arm.crop[2], g_arm.crop[3], g_qpf, Qpc(), (long long)g_arm.startDisplayTime, (unsigned long long)g_endFrames.load());
  PushText(1, g_capDir + L"\\capture.jsonl", std::string(b) + SwapchainTableJson() + "}");
  Log("capture %u begins: %u submissions, scale %u, crop %.3f,%.3f,%.3f,%.3f -> %ls", g_capIndex, g_arm.frames, g_arm.scale, g_arm.crop[0], g_arm.crop[1], g_arm.crop[2], g_arm.crop[3], g_capDir.c_str());
  g_state = Capturing;
}
void EndCapture(const char* reason) {   // g_mu held
  char b[400]; snprintf(b, sizeof(b), "{\"end\":1,\"reason\":\"%s\",\"submissions\":%u,\"asked\":%u}", reason, g_capDone, g_arm.frames);
  PushText(1, g_capDir + L"\\capture.jsonl", b);
  PushText(2, g_capDir + L"\\done.txt", std::string(reason) + "\n");
  Log("capture %u ends after %u of %u submissions: %s", g_capIndex, g_capDone, g_arm.frames, reason);
  ++g_capIndex; g_state = Idle;
}
void ArmWith(const Arm& a) { g_arm = a; if (g_arm.frames < 1) g_arm.frames = 1; if (g_arm.frames > kHardCap) g_arm.frames = kHardCap; if (g_arm.scale < 1) g_arm.scale = 1; if (g_arm.scale > 16) g_arm.scale = 16; if (g_arm.scale2 > 16) g_arm.scale2 = 16; if (g_arm.budgetBytes < (16ull << 20)) g_arm.budgetBytes = 16ull << 20; g_state = Armed; Log("armed: %u submissions, start qpc %lld, start display time %lld, tag '%s'", g_arm.frames, g_arm.startQpc, (long long)g_arm.startDisplayTime, g_arm.tag.c_str()); }
void PollTrigger() {   // idle only; at most four times a second
  ULONGLONG now = GetTickCount64(); if (now - g_lastPoll < 250) return; g_lastPoll = now;
  std::lock_guard<std::mutex> l(g_mu);
  if (g_state != Idle) return;
  if (g_envArmPending) { if (!g_firstEndQpc) g_firstEndQpc = Qpc(); g_envArmPending = false; Arm a; a.frames = g_envFrames; a.scale = g_defScale; memcpy(a.crop, g_defCrop, sizeof(a.crop)); a.startQpc = g_firstEndQpc + (long long)(g_envAfterS * g_qpf); a.tag = "env"; ArmWith(a); return; }
  std::wstring trig = g_dir + L"\\arm.txt"; if (GetFileAttributesW(trig.c_str()) == INVALID_FILE_ATTRIBUTES) return;
  wchar_t taken[64]; swprintf(taken, 64, L"\\arm.taken-%u.txt", g_capIndex); std::wstring to = g_dir + taken;
  if (!MoveFileExW(trig.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING)) return;   // still being written: next poll
  Arm a; a.scale = g_defScale; memcpy(a.crop, g_defCrop, sizeof(a.crop)); long long delayMs = 0;
  if (FILE* f = _wfopen(to.c_str(), L"rb")) { char line[512];
    while (fgets(line, sizeof(line), f)) { char* eq = strchr(line, '='); if (!eq) continue; *eq = 0; std::string k = line, v = eq + 1; while (!v.empty() && (v.back() == '\n' || v.back() == '\r' || v.back() == ' ')) v.pop_back();
      while (!k.empty() && (k[0] == ' ' || (unsigned char)k[0] >= 0x80)) k.erase(0, 1);
      if (k == "frames") a.frames = (uint32_t)strtoul(v.c_str(), nullptr, 10); else if (k == "delay_ms") delayMs = strtoll(v.c_str(), nullptr, 10); else if (k == "start_display_time") a.startDisplayTime = strtoll(v.c_str(), nullptr, 10);
      else if (k == "scale") a.scale = (uint32_t)strtoul(v.c_str(), nullptr, 10); else if (k == "crop") { if (!ParseCrop(v, a.crop)) Log("arm.txt: crop '%s' not understood, default kept", v.c_str()); } else if (k == "tag") a.tag = v;
      else if (k == "scale2") a.scale2 = (uint32_t)strtoul(v.c_str(), nullptr, 10); else if (k == "crop2") { if (!ParseCrop(v, a.crop2)) Log("arm.txt: crop2 '%s' not understood", v.c_str()); }
      else if (k == "budget_mb") a.budgetBytes = strtoull(v.c_str(), nullptr, 10) << 20;
      else if (k == "window0" || k == "window1") { int e = k[6] - '0'; double w[4]; if (sscanf(v.c_str(), "%lf,%lf,%lf,%lf", &w[0], &w[1], &w[2], &w[3]) == 4 && w[1] > w[0] && w[2] > w[3]) { memcpy(a.window[e], w, sizeof(w)); a.hasWindow[e] = true; } else Log("arm.txt: %s '%s' not understood (tangents l,r,u,d)", k.c_str(), v.c_str()); } }
    fclose(f); }
  a.startQpc = Qpc() + delayMs * g_qpf / 1000; ArmWith(a);
}

// ---- D3D12 readback ----------------------------------------------------------------------------------------------
bool Is8888(DXGI_FORMAT f, bool* bgra, DXGI_FORMAT* typed) {
  switch (f) {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: case DXGI_FORMAT_R8G8B8A8_UNORM: *bgra = false; *typed = DXGI_FORMAT_R8G8B8A8_UNORM; return true;
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: *bgra = false; *typed = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB; return true;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS: case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_B8G8R8X8_UNORM: case DXGI_FORMAT_B8G8R8X8_TYPELESS: *bgra = true; *typed = DXGI_FORMAT_B8G8R8A8_UNORM; return true;
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB: *bgra = true; *typed = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB; return true;
    default: return false;
  }
}
// A shader view that reads the bytes as stored (UNORM): possible on a typeless or UNORM texture, not on a fully typed
// sRGB one. Only then is a scale done on the GPU; else the full region is read back and averaged on the writer thread.
bool RawView(DXGI_FORMAT f, DXGI_FORMAT* view) {
  switch (f) {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: case DXGI_FORMAT_R8G8B8A8_UNORM: *view = DXGI_FORMAT_R8G8B8A8_UNORM; return true;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS: case DXGI_FORMAT_B8G8R8A8_UNORM: *view = DXGI_FORMAT_B8G8R8A8_UNORM; return true;
    default: return false;
  }
}
bool EnsureD3D12() {
  if (!g_dev || !g_queue) return false;
  if (g_list) return true;
  if (FAILED(g_dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g_alloc)))) return false;
  if (FAILED(g_dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_alloc, nullptr, IID_PPV_ARGS(&g_list)))) return false;
  g_list->Close();
  if (FAILED(g_dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_fence)))) return false;
  g_fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr); return g_fenceEvent != nullptr;
}
// GPU box average (exact: integer sum of the s x s texels, rounded as the CPU path rounds) into a small render target.
ID3D12RootSignature* g_rs = nullptr; ID3D12PipelineState* g_pso = nullptr; ID3D12DescriptorHeap* g_srvHeap = nullptr; ID3D12DescriptorHeap* g_rtvHeap = nullptr;
UINT g_srvNext = 0, g_srvStep = 0, g_rtvStep = 0; int g_gpuScale = 0;   // 0 not tried, 1 ready, -1 not available
struct Rt { ID3D12Resource* tex; UINT rtv; }; std::map<uint64_t, Rt> g_rts; const UINT kSrvSlots = 64, kRtvSlots = 16;
const char* kScaleShader =
  "Texture2DArray<float4> t : register(t0);\n"
  "cbuffer C : register(b0) { uint ox; uint oy; uint s; uint slice; };\n"
  "float4 vs(uint id : SV_VertexID) : SV_Position { return float4(id == 1 ? 3.0 : -1.0, id == 2 ? -3.0 : 1.0, 0.0, 1.0); }\n"
  "float4 ps(float4 p : SV_Position) : SV_Target {\n"
  "  uint2 q = uint2(p.xy); uint4 acc = uint4(0, 0, 0, 0);\n"
  "  for (uint y = 0; y < s; ++y) for (uint x = 0; x < s; ++x) acc += uint4(t.Load(int4(ox + q.x * s + x, oy + q.y * s + y, slice, 0)) * 255.0 + 0.5);\n"
  "  uint n = s * s; return float4((acc + n / 2) / n) / 255.0; }\n";
bool EnsureGpuScale() {
  if (g_gpuScale) return g_gpuScale > 0;
  g_gpuScale = -1;
  ID3DBlob *vs = nullptr, *ps = nullptr, *err = nullptr, *sig = nullptr; bool ok = false;
  do {
    if (FAILED(D3DCompile(kScaleShader, strlen(kScaleShader), "rigtap", nullptr, nullptr, "vs", "vs_5_0", 0, 0, &vs, &err))) break;
    if (FAILED(D3DCompile(kScaleShader, strlen(kScaleShader), "rigtap", nullptr, nullptr, "ps", "ps_5_0", 0, 0, &ps, &err))) break;
    D3D12_DESCRIPTOR_RANGE range{}; range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV; range.NumDescriptors = 1;
    D3D12_ROOT_PARAMETER par[2]{}; par[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE; par[0].DescriptorTable.NumDescriptorRanges = 1; par[0].DescriptorTable.pDescriptorRanges = &range; par[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    par[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS; par[1].Constants.Num32BitValues = 4; par[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_ROOT_SIGNATURE_DESC rsd{}; rsd.NumParameters = 2; rsd.pParameters = par;
    if (FAILED(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err))) break;
    if (FAILED(g_dev->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(), IID_PPV_ARGS(&g_rs)))) break;
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{}; pd.pRootSignature = g_rs; pd.VS = {vs->GetBufferPointer(), vs->GetBufferSize()}; pd.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
    pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL; pd.SampleMask = UINT_MAX; pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID; pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE; pd.NumRenderTargets = 1; pd.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM; pd.SampleDesc.Count = 1;
    if (FAILED(g_dev->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&g_pso)))) break;
    D3D12_DESCRIPTOR_HEAP_DESC hd{}; hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV; hd.NumDescriptors = kSrvSlots; hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(g_dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g_srvHeap)))) break;
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV; hd.NumDescriptors = kRtvSlots; hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    if (FAILED(g_dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g_rtvHeap)))) break;
    g_srvStep = g_dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV); g_rtvStep = g_dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    ok = true;
  } while (false);
  if (err) { Log("GPU scale not available: %.300s", (const char*)err->GetBufferPointer()); err->Release(); } else if (!ok) Log("GPU scale not available (pipeline objects)");
  if (vs) vs->Release(); if (ps) ps->Release(); if (sig) sig->Release();
  if (ok) g_gpuScale = 1;
  return ok;
}
Rt* GetRt(uint32_t w, uint32_t h) {
  uint64_t key = (uint64_t(w) << 32) | h; auto it = g_rts.find(key); if (it != g_rts.end()) return &it->second;
  if (g_rts.size() >= kRtvSlots) return nullptr;
  D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_DEFAULT; D3D12_RESOURCE_DESC d{}; d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; d.Width = w; d.Height = h; d.DepthOrArraySize = 1; d.MipLevels = 1; d.Format = DXGI_FORMAT_R8G8B8A8_UNORM; d.SampleDesc.Count = 1; d.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
  Rt rt{}; if (FAILED(g_dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_COPY_SOURCE, nullptr, IID_PPV_ARGS(&rt.tex)))) return nullptr;
  rt.rtv = (UINT)g_rts.size(); D3D12_CPU_DESCRIPTOR_HANDLE hnd = g_rtvHeap->GetCPUDescriptorHandleForHeapStart(); hnd.ptr += SIZE_T(rt.rtv) * g_rtvStep; g_dev->CreateRenderTargetView(rt.tex, nullptr, hnd);
  return &(g_rts[key] = rt);
}
void ReleaseD3D12() {
  for (auto& kv : g_rts) kv.second.tex->Release(); g_rts.clear();
  if (g_pso) { g_pso->Release(); g_pso = nullptr; } if (g_rs) { g_rs->Release(); g_rs = nullptr; } if (g_srvHeap) { g_srvHeap->Release(); g_srvHeap = nullptr; } if (g_rtvHeap) { g_rtvHeap->Release(); g_rtvHeap = nullptr; } g_gpuScale = 0;
  if (g_readback) { g_readback->Release(); g_readback = nullptr; g_readbackSize = 0; }
  if (g_list) { g_list->Release(); g_list = nullptr; } if (g_alloc) { g_alloc->Release(); g_alloc = nullptr; } if (g_fence) { g_fence->Release(); g_fence = nullptr; }
  if (g_fenceEvent) { CloseHandle(g_fenceEvent); g_fenceEvent = nullptr; }
  if (g_queue) { g_queue->Release(); g_queue = nullptr; } if (g_dev) { g_dev->Release(); g_dev = nullptr; }
}
// One file: a region of one array slice, copied as it is (gpu false; a scale then runs on the writer thread) or
// averaged on the GPU first (gpu true).
struct Item { ID3D12Resource* res; UINT sub, slice; D3D12_BOX box; D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp; uint32_t w, h, scale; bool gpu; DXGI_FORMAT view; Rt* rt; std::wstring path; };

// The texture a view shows, or why there are no pixels.
const char* ViewTexture(const XrSwapchainSubImage& si, ID3D12Resource** res, D3D12_RESOURCE_DESC* rd, bool* bgra, DXGI_FORMAT* typed) {
  auto it = g_sc.find(si.swapchain);
  if (it == g_sc.end()) return "swapchain not seen at creation";
  if (g_api != 12) return "session is not D3D12";
  if (it->second.lastReleased < 0 || (size_t)it->second.lastReleased >= it->second.images.size()) return "no released image known";
  if (!EnsureD3D12()) return "no D3D12 command objects";
  *res = it->second.images[it->second.lastReleased]; *rd = (*res)->GetDesc(); const XrRect2Di& r = si.imageRect;
  if (rd->Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || rd->SampleDesc.Count != 1) return "not a single-sample 2D texture";
  if (!Is8888(rd->Format, bgra, typed)) return "format is not 8 bits x 4";
  if (si.imageArrayIndex >= rd->DepthOrArraySize) return "array index outside the texture";
  if (r.offset.x < 0 || r.offset.y < 0 || r.extent.width <= 0 || r.extent.height <= 0 || uint64_t(r.offset.x) + r.extent.width > rd->Width || uint32_t(r.offset.y) + r.extent.height > rd->Height) return "rect outside the texture";
  return nullptr;
}
// The pixel region of one stream of one view. Stream 0: the tangent window of that view index when one was given,
// else the crop, at `scale`. Stream 1 (only with scale2 > 0): crop2 at scale2. False = nothing to store.
bool Region(const XrCompositionLayerProjectionView& w, uint32_t viewIndex, int stream, uint32_t scale, int* x0, int* y0, uint32_t* cw, uint32_t* ch) {
  const XrRect2Di& r = w.subImage.imageRect; double f[4];
  if (stream == 0 && viewIndex < 2 && g_arm.hasWindow[viewIndex]) {
    double tl = tan(w.fov.angleLeft), tr = tan(w.fov.angleRight), tu = tan(w.fov.angleUp), td = tan(w.fov.angleDown); const double* win = g_arm.window[viewIndex];
    if (!(tr > tl) || !(tu > td)) return false;      // an inverted field of view is not handled
    f[0] = (win[0] - tl) / (tr - tl); f[2] = (win[1] - tl) / (tr - tl); f[1] = (tu - win[2]) / (tu - td); f[3] = (tu - win[3]) / (tu - td);
    for (double& x : f) x = x < 0 ? 0 : (x > 1 ? 1 : x);
  } else memcpy(f, stream == 0 ? g_arm.crop : g_arm.crop2, sizeof(f));
  if (!(f[2] > f[0]) || !(f[3] > f[1])) return false;
  int ax = r.offset.x + int(f[0] * r.extent.width), ay = r.offset.y + int(f[1] * r.extent.height), bx = r.offset.x + int(f[2] * r.extent.width + 0.5), by = r.offset.y + int(f[3] * r.extent.height + 0.5);
  *x0 = ax; *y0 = ay; *cw = uint32_t(bx - ax) / scale * scale; *ch = uint32_t(by - ay) / scale * scale;
  return *cw > 0 && *ch > 0;
}
// Bytes one submission puts on disk with stream 0 at scaleA (for the budget).
uint64_t SubmissionBytes(const XrFrameEndInfo* info, uint32_t scaleA) {
  uint64_t total = 0;
  for (uint32_t i = 0; i < info->layerCount; ++i) { const XrCompositionLayerBaseHeader* h = info->layers ? info->layers[i] : nullptr; if (!h || h->type != XR_TYPE_COMPOSITION_LAYER_PROJECTION) continue;
    auto* p = (const XrCompositionLayerProjection*)h;
    for (uint32_t v = 0; v < p->viewCount; ++v) { ID3D12Resource* res; D3D12_RESOURCE_DESC rd; bool bgra; DXGI_FORMAT typed; if (ViewTexture(p->views[v].subImage, &res, &rd, &bgra, &typed)) continue;
      int x0, y0; uint32_t cw, ch;
      if (Region(p->views[v], v, 0, scaleA, &x0, &y0, &cw, &ch)) total += uint64_t(cw / scaleA) * (ch / scaleA) * 4;
      if (g_arm.scale2 && Region(p->views[v], v, 1, g_arm.scale2, &x0, &y0, &cw, &ch)) total += uint64_t(cw / g_arm.scale2) * (ch / g_arm.scale2) * 4; } }
  return total;
}
void AppendSub(std::string& s, const XrSwapchainSubImage& si) {
  auto it = g_sc.find(si.swapchain); const Sc* sc = it == g_sc.end() ? nullptr : &it->second; char b[700];
  snprintf(b, sizeof(b), "\"sc\":%llu,\"scId\":%lld,\"format\":%lld,\"dxgiFormat\":%d,\"usage\":%llu,\"scW\":%u,\"scH\":%u,\"scArray\":%u,\"rect\":[%d,%d,%d,%d],\"array\":%u,\"imageIndex\":%d,\"releaseSeq\":%llu",
    (unsigned long long)si.swapchain, sc ? (long long)sc->id : -1LL, sc ? (long long)sc->fmt : 0LL, sc ? sc->dxgiFormat : 0, sc ? (unsigned long long)sc->usage : 0ull, sc ? sc->w : 0, sc ? sc->h : 0, sc ? sc->array : 0,
    si.imageRect.offset.x, si.imageRect.offset.y, si.imageRect.extent.width, si.imageRect.extent.height, si.imageArrayIndex, sc ? sc->lastReleased : -1, sc ? (unsigned long long)sc->releaseSeq : 0ull);
  s += b;
}
// One submission. g_mu held. Returns false to end the capture (reason in *why).
bool CaptureFrame(const XrFrameEndInfo* info, const char** why) {
  long long q0 = Qpc(); std::string js; js.reserve(4096); char b[700]; std::vector<Item> items; uint64_t offset = 0; uint32_t autoScale = 0;
  if (g_capDone == 0) {   // the budget: stream 0's scale goes up until the whole capture fits
    uint32_t s = g_arm.scale; uint64_t per = SubmissionBytes(info, s);
    while (per * g_arm.frames > g_arm.budgetBytes && s < 16) per = SubmissionBytes(info, ++s);
    if (s != g_arm.scale) { Log("capture %u: scale %u -> %u so that %u submissions fit %llu MB (%.1f MB each now)", g_capIndex, g_arm.scale, s, g_arm.frames, (unsigned long long)(g_arm.budgetBytes >> 20), per / 1048576.0); g_arm.scale = s; autoScale = s; }
    else Log("capture %u: %.1f MB per submission, %.0f MB in all", g_capIndex, per / 1048576.0, per * g_arm.frames / 1048576.0);
  }
  snprintf(b, sizeof(b), "{\"n\":%u,\"qpc\":%lld,\"endFrame\":%llu,\"displayTime\":%lld,\"blendMode\":%d,\"layerCount\":%u,", g_capDone, q0, (unsigned long long)g_endFrames.load(), (long long)info->displayTime, (int)info->environmentBlendMode, info->layerCount);
  js += b; if (autoScale) { snprintf(b, sizeof(b), "\"autoScale\":%u,", autoScale); js += b; } js += "\"layers\":[";
  for (uint32_t i = 0; i < info->layerCount; ++i) {
    const XrCompositionLayerBaseHeader* h = info->layers ? info->layers[i] : nullptr; if (i) js += ",";
    if (!h) { js += "{\"type\":\"null\"}"; continue; }
    auto sp = g_space.find(h->space); int st = sp == g_space.end() ? -1 : sp->second;
    if (h->type == XR_TYPE_COMPOSITION_LAYER_PROJECTION) {
      auto* p = (const XrCompositionLayerProjection*)h;
      snprintf(b, sizeof(b), "{\"type\":\"projection\",\"flags\":%llu,\"space\":%llu,\"spaceType\":%d,\"viewCount\":%u,\"views\":[", (unsigned long long)p->layerFlags, (unsigned long long)p->space, st, p->viewCount); js += b;
      for (uint32_t v = 0; v < p->viewCount; ++v) {
        const XrCompositionLayerProjectionView& w = p->views[v]; bool depth = false;
        for (auto* n = (const XrBaseInStructure*)w.next; n; n = n->next) if (n->type == XR_TYPE_COMPOSITION_LAYER_DEPTH_INFO_KHR) depth = true;
        snprintf(b, sizeof(b), "%s{\"pose\":[%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g],\"fov\":[%.9g,%.9g,%.9g,%.9g],\"depthInfo\":%s,", v ? "," : "", w.pose.position.x, w.pose.position.y, w.pose.position.z, w.pose.orientation.x, w.pose.orientation.y, w.pose.orientation.z, w.pose.orientation.w,
          w.fov.angleLeft, w.fov.angleRight, w.fov.angleUp, w.fov.angleDown, depth ? "true" : "false"); js += b;
        AppendSub(js, w.subImage);
        ID3D12Resource* res = nullptr; D3D12_RESOURCE_DESC rd{}; bool bgra = false; DXGI_FORMAT typed = DXGI_FORMAT_UNKNOWN;
        const char* note = ViewTexture(w.subImage, &res, &rd, &bgra, &typed);
        for (int stream = 0; stream < 2 && !note; ++stream) {
          uint32_t s = stream == 0 ? g_arm.scale : g_arm.scale2; if (!s) continue;
          int x0, y0; uint32_t cw, ch; const char* sfx = stream ? "2" : "";
          if (!Region(w, v, stream, s, &x0, &y0, &cw, &ch)) { if (stream == 0) note = "crop or window is empty in this view"; continue; }
          Item itx{}; itx.res = res; itx.slice = w.subImage.imageArrayIndex; itx.sub = itx.slice * rd.MipLevels; itx.scale = s; itx.w = cw; itx.h = ch;
          itx.gpu = s > 1 && RawView(rd.Format, &itx.view) && EnsureGpuScale() && (itx.rt = GetRt(cw / s, ch / s)) != nullptr;
          D3D12_RESOURCE_DESC bd = rd; bd.DepthOrArraySize = 1; bd.MipLevels = 1; bd.Alignment = 0; bd.Flags = D3D12_RESOURCE_FLAG_NONE; UINT64 total = 0;
          if (itx.gpu) { bd.Width = cw / s; bd.Height = ch / s; bd.Format = DXGI_FORMAT_R8G8B8A8_UNORM; itx.box = {UINT(x0), UINT(y0), 0, 0, 0, 0}; }
          else { bd.Width = cw; bd.Height = ch; bd.Format = typed; itx.box = {UINT(x0), UINT(y0), 0, UINT(x0) + cw, UINT(y0) + ch, 1}; }
          g_dev->GetCopyableFootprints(&bd, 0, 1, offset, &itx.fp, nullptr, nullptr, &total);
          if (total == 0 || total == UINT64_MAX) { if (stream == 0) note = "no copy footprint"; continue; }
          offset = (itx.fp.Offset + total + 511) & ~uint64_t(511);
          wchar_t wf[64]; swprintf(wf, 64, L"\\s%04u_L%u_v%u%s.rgba", g_capDone, i, v, stream ? L"b" : L""); itx.path = g_capDir + wf; items.push_back(itx);
          snprintf(b, sizeof(b), ",\"file%s\":\"s%04u_L%u_v%u%s.rgba\",\"w%s\":%u,\"h%s\":%u,\"cropOffset%s\":[%d,%d],\"scale%s\":%u,\"bgra%s\":%s,\"gpuScaled%s\":%s", sfx, g_capDone, i, v, stream ? "b" : "", sfx, cw / s, sfx, ch / s, sfx,
            x0 - w.subImage.imageRect.offset.x, y0 - w.subImage.imageRect.offset.y, sfx, s, sfx, (bgra && !itx.gpu) ? "true" : "false", sfx, itx.gpu ? "true" : "false"); js += b;
        }
        if (note) { js += ",\"noPixels\":\""; js += note; js += "\""; }
        js += "}";
      }
      js += "]}";
    } else if (h->type == XR_TYPE_COMPOSITION_LAYER_QUAD) {
      auto* q = (const XrCompositionLayerQuad*)h;
      snprintf(b, sizeof(b), "{\"type\":\"quad\",\"flags\":%llu,\"space\":%llu,\"spaceType\":%d,\"eye\":%d,\"pose\":[%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g],\"size\":[%.9g,%.9g],", (unsigned long long)q->layerFlags, (unsigned long long)q->space, st, (int)q->eyeVisibility,
        q->pose.position.x, q->pose.position.y, q->pose.position.z, q->pose.orientation.x, q->pose.orientation.y, q->pose.orientation.z, q->pose.orientation.w, q->size.width, q->size.height); js += b;
      AppendSub(js, q->subImage); js += "}";
    } else { snprintf(b, sizeof(b), "{\"type\":\"other\",\"xrType\":%d,\"flags\":%llu}", (int)h->type, (unsigned long long)h->layerFlags); js += b; }
  }
  js += "]";
  if (!items.empty()) {
    uint64_t mem = 0, disk = 0; for (auto& it : items) { uint64_t o = uint64_t(it.w / it.scale) * (it.h / it.scale) * 4; disk += o; mem += it.gpu ? o : uint64_t(it.w) * it.h * 4; }
    if (g_queuedBytes.load() + mem > kMaxQueuedBytes) { *why = "writer queue over 3 GB (the region is read at full size: the texture takes no raw shader view, or the budget is too high)"; return false; }
    if (g_diskBytes.load() + g_queuedBytes.load() + disk > g_maxBytes) { *why = "disk budget RIGTAP_MAX_MB reached"; return false; }
    if (g_writeFailed.load()) { *why = "a file could not be written"; return false; }
    if (offset > g_readbackSize) {
      if (g_readback) { g_readback->Release(); g_readback = nullptr; g_readbackSize = 0; }
      D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_READBACK; D3D12_RESOURCE_DESC d{}; d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; d.Width = offset; d.Height = 1; d.DepthOrArraySize = 1; d.MipLevels = 1; d.SampleDesc.Count = 1; d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
      if (FAILED(g_dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&g_readback)))) { *why = "readback buffer not created"; return false; }
      g_readbackSize = offset;
    }
    if (FAILED(g_alloc->Reset()) || FAILED(g_list->Reset(g_alloc, nullptr))) { *why = "command list reset failed"; return false; }
    // OpenXR D3D12: a released colour image is in RENDER_TARGET state. Each sub-resource is moved once, to a state
    // that serves both the copy and the shader read.
    const D3D12_RESOURCE_STATES kRead = D3D12_RESOURCE_STATE_COPY_SOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    std::vector<std::pair<ID3D12Resource*, UINT>> moved;
    auto barrier = [&](ID3D12Resource* r, UINT sub, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES c) { D3D12_RESOURCE_BARRIER x{}; x.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; x.Transition.pResource = r; x.Transition.Subresource = sub; x.Transition.StateBefore = a; x.Transition.StateAfter = c; g_list->ResourceBarrier(1, &x); };
    for (auto& it : items) { bool seen = false; for (auto& m : moved) if (m.first == it.res && m.second == it.sub) seen = true; if (!seen) { moved.push_back({it.res, it.sub}); barrier(it.res, it.sub, D3D12_RESOURCE_STATE_RENDER_TARGET, kRead); } }
    bool pipeline = false;
    for (auto& it : items) {
      D3D12_TEXTURE_COPY_LOCATION dst{}; dst.pResource = g_readback; dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; dst.PlacedFootprint = it.fp; D3D12_TEXTURE_COPY_LOCATION src{}; src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
      if (!it.gpu) { src.pResource = it.res; src.SubresourceIndex = it.sub; g_list->CopyTextureRegion(&dst, 0, 0, 0, &src, &it.box); continue; }
      if (!pipeline) { g_list->SetGraphicsRootSignature(g_rs); g_list->SetPipelineState(g_pso); g_list->SetDescriptorHeaps(1, &g_srvHeap); g_list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST); pipeline = true; }
      UINT slot = g_srvNext++ % kSrvSlots; D3D12_RESOURCE_DESC rd = it.res->GetDesc();
      D3D12_SHADER_RESOURCE_VIEW_DESC sd{}; sd.Format = it.view; sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY; sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; sd.Texture2DArray.MipLevels = 1; sd.Texture2DArray.ArraySize = rd.DepthOrArraySize;
      D3D12_CPU_DESCRIPTOR_HANDLE ch = g_srvHeap->GetCPUDescriptorHandleForHeapStart(); ch.ptr += SIZE_T(slot) * g_srvStep; g_dev->CreateShaderResourceView(it.res, &sd, ch);
      D3D12_GPU_DESCRIPTOR_HANDLE gh = g_srvHeap->GetGPUDescriptorHandleForHeapStart(); gh.ptr += UINT64(slot) * g_srvStep;
      D3D12_CPU_DESCRIPTOR_HANDLE rh = g_rtvHeap->GetCPUDescriptorHandleForHeapStart(); rh.ptr += SIZE_T(it.rt->rtv) * g_rtvStep;
      uint32_t ow = it.w / it.scale, oh = it.h / it.scale; UINT consts[4] = {it.box.left, it.box.top, it.scale, it.slice};
      barrier(it.rt->tex, 0, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
      g_list->SetGraphicsRootDescriptorTable(0, gh); g_list->SetGraphicsRoot32BitConstants(1, 4, consts, 0); g_list->OMSetRenderTargets(1, &rh, FALSE, nullptr);
      D3D12_VIEWPORT vp{0, 0, float(ow), float(oh), 0, 1}; D3D12_RECT sr{0, 0, LONG(ow), LONG(oh)}; g_list->RSSetViewports(1, &vp); g_list->RSSetScissorRects(1, &sr); g_list->DrawInstanced(3, 1, 0, 0);
      barrier(it.rt->tex, 0, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
      src.pResource = it.rt->tex; src.SubresourceIndex = 0; g_list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    }
    for (auto& m : moved) barrier(m.first, m.second, kRead, D3D12_RESOURCE_STATE_RENDER_TARGET);
    if (FAILED(g_list->Close())) { *why = "command list close failed"; return false; }
    ID3D12CommandList* lists[] = {g_list}; g_queue->ExecuteCommandLists(1, lists);
    if (FAILED(g_queue->Signal(g_fence, ++g_fenceValue)) || FAILED(g_fence->SetEventOnCompletion(g_fenceValue, g_fenceEvent))) { *why = "fence signal failed"; return false; }
    if (WaitForSingleObject(g_fenceEvent, 2000) != WAIT_OBJECT_0) { *why = "GPU copy not finished in 2 s"; return false; }   // the list is left to finish; nothing is reset before the next capture's wait
    if (FAILED(g_dev->GetDeviceRemovedReason())) { *why = "device removed"; return false; }
    D3D12_RANGE rr{0, SIZE_T(offset)}; void* base = nullptr;
    if (FAILED(g_readback->Map(0, &rr, &base)) || !base) { *why = "readback map failed"; return false; }
    for (auto& it : items) { auto j = std::make_unique<Job>(); j->kind = 0; j->path = it.path; uint32_t rw = it.gpu ? it.w / it.scale : it.w, rh = it.gpu ? it.h / it.scale : it.h; j->w = rw; j->h = rh; j->scale = it.gpu ? 1 : it.scale; j->data.resize(size_t(rw) * rh * 4);
      const uint8_t* src = (const uint8_t*)base + it.fp.Offset; for (uint32_t y = 0; y < rh; ++y) memcpy(&j->data[size_t(y) * rw * 4], src + size_t(y) * it.fp.Footprint.RowPitch, size_t(rw) * 4);
      Push(std::move(j)); }
    D3D12_RANGE none{0, 0}; g_readback->Unmap(0, &none);
  }
  snprintf(b, sizeof(b), ",\"copyUs\":%lld}", (Qpc() - q0) * 1000000 / g_qpf); js += b;
  PushText(1, g_capDir + L"\\capture.jsonl", js);
  return true;
}

// ---- hooks -------------------------------------------------------------------------------------------------------
XrResult XRAPI_CALL MyEnd(XrSession s, const XrFrameEndInfo* info) {
  g_endFrames.fetch_add(1, std::memory_order_relaxed);
  int st = g_state.load(std::memory_order_relaxed);
  if (st == Idle) { if (g_enabled) PollTrigger(); return g_nextEnd(s, info); }
  if (info) {
    try {
      std::lock_guard<std::mutex> l(g_mu);
      if (g_state == Armed && Qpc() >= g_arm.startQpc && info->displayTime >= g_arm.startDisplayTime) BeginCapture();
      if (g_state == Capturing) {
        const char* why = nullptr;
        if (s != g_session && g_session != XR_NULL_HANDLE) EndCapture("xrEndFrame of another session");
        else if (!CaptureFrame(info, &why)) EndCapture(why ? why : "error");
        else if (++g_capDone >= g_arm.frames) EndCapture("complete");
      }
    } catch (...) { g_state = Idle; Log("capture stopped: exception"); }
  }
  return g_nextEnd(s, info);
}
XrResult XRAPI_CALL MyCreateSession(XrInstance inst, const XrSessionCreateInfo* ci, XrSession* out) {
  XrResult r = g_nextCreateSession(inst, ci, out);
  if (XR_SUCCEEDED(r) && ci && out) { try { std::lock_guard<std::mutex> l(g_mu); ReleaseD3D12(); g_api = -1; g_session = *out; g_firstEndQpc = 0;
    for (auto* n = (const XrBaseInStructure*)ci->next; n; n = n->next) {
      if (n->type == XR_TYPE_GRAPHICS_BINDING_D3D12_KHR) { auto* bnd = (const XrGraphicsBindingD3D12KHR*)n; g_api = 12; g_dev = bnd->device; g_queue = bnd->queue; if (g_dev) g_dev->AddRef(); if (g_queue) g_queue->AddRef(); }
      else if (n->type == XR_TYPE_GRAPHICS_BINDING_D3D11_KHR) g_api = 11;
    }
    Log("session %llx created: graphics binding %s", (unsigned long long)*out, g_api == 12 ? "D3D12 (pixels can be read)" : g_api == 11 ? "D3D11 (metadata only)" : "other (metadata only)"); } catch (...) {} }
  return r;
}
XrResult XRAPI_CALL MyDestroySession(XrSession s) {
  try { std::lock_guard<std::mutex> l(g_mu); if (s == g_session) { if (g_state == Capturing) EndCapture("session destroyed"); else if (g_state == Armed) g_state = Idle;
    // the queue is idle for our list: every capture waited for its own copy
    for (auto& kv : g_sc) if (kv.second.session == s) { for (auto* r : kv.second.images) r->Release(); kv.second.images.clear(); kv.second.alive = false; }
    ReleaseD3D12(); g_session = XR_NULL_HANDLE; g_api = 0; Log("session %llx destroyed", (unsigned long long)s); } } catch (...) {}
  return g_nextDestroySession(s);
}
XrResult XRAPI_CALL MyCreateSc(XrSession s, const XrSwapchainCreateInfo* ci, XrSwapchain* out) {
  XrResult r = g_nextCreateSc(s, ci, out);
  if (XR_SUCCEEDED(r) && ci && out) { try {
    Sc sc; sc.session = s; sc.w = ci->width; sc.h = ci->height; sc.array = ci->arraySize; sc.mips = ci->mipCount; sc.samples = ci->sampleCount; sc.faces = ci->faceCount; sc.fmt = ci->format; sc.usage = ci->usageFlags; sc.createFlags = ci->createFlags; sc.createdQpc = Qpc();
    bool d3d12; { std::lock_guard<std::mutex> l(g_mu); d3d12 = g_api == 12 && s == g_session; }
    if (d3d12 && g_nextEnumImages) { uint32_t n = 0;
      if (XR_SUCCEEDED(g_nextEnumImages(*out, 0, &n, nullptr)) && n > 0 && n < 64) { XrSwapchainImageD3D12KHR proto{XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR}; std::vector<XrSwapchainImageD3D12KHR> im(n, proto);
        if (XR_SUCCEEDED(g_nextEnumImages(*out, n, &n, (XrSwapchainImageBaseHeader*)im.data()))) for (uint32_t i = 0; i < n; ++i) if (im[i].texture) { im[i].texture->AddRef(); sc.images.push_back(im[i].texture); }
        if (!sc.images.empty()) sc.dxgiFormat = (int)sc.images[0]->GetDesc().Format; } }
    std::lock_guard<std::mutex> l(g_mu); sc.id = g_scNext++; g_sc[*out] = sc;
    Log("swapchain #%llu %llx %ux%u array %u mips %u samples %u format %lld (texture format %d) usage %llu images %zu", (unsigned long long)sc.id, (unsigned long long)*out, sc.w, sc.h, sc.array, sc.mips, sc.samples, (long long)sc.fmt, sc.dxgiFormat, (unsigned long long)sc.usage, sc.images.size());
  } catch (...) {} }
  return r;
}
XrResult XRAPI_CALL MyDestroySc(XrSwapchain h) {
  try { std::lock_guard<std::mutex> l(g_mu); auto it = g_sc.find(h); if (it != g_sc.end()) { for (auto* r : it->second.images) r->Release(); it->second.images.clear(); it->second.alive = false; } } catch (...) {}
  return g_nextDestroySc(h);
}
XrResult XRAPI_CALL MyAcquire(XrSwapchain h, const XrSwapchainImageAcquireInfo* ai, uint32_t* index) {
  XrResult r = g_nextAcquire(h, ai, index);
  if (XR_SUCCEEDED(r) && index) { try { std::lock_guard<std::mutex> l(g_mu); auto it = g_sc.find(h); if (it != g_sc.end()) it->second.acquired.push_back(*index); } catch (...) {} }
  return r;
}
XrResult XRAPI_CALL MyRelease(XrSwapchain h, const XrSwapchainImageReleaseInfo* ri) {
  XrResult r = g_nextRelease(h, ri);
  if (XR_SUCCEEDED(r)) { try { std::lock_guard<std::mutex> l(g_mu); auto it = g_sc.find(h); if (it != g_sc.end() && !it->second.acquired.empty()) { it->second.lastReleased = (int)it->second.acquired.front(); it->second.acquired.pop_front(); ++it->second.releaseSeq; } } catch (...) {} }
  return r;
}
XrResult XRAPI_CALL MyLocateViews(XrSession s, const XrViewLocateInfo* li, XrViewState* vs, uint32_t cap, uint32_t* count, XrView* views) {
  XrResult r = g_nextLocate(s, li, vs, cap, count, views);
  if (g_invalidMs > 0 && XR_SUCCEEDED(r) && li && vs && cap) {
    int type = 0; { std::lock_guard<std::mutex> l(g_spaceMu); auto it = g_spaceTypeOf.find(li->space); if (it != g_spaceTypeOf.end()) type = it->second; }
    if (type != (int)XR_REFERENCE_SPACE_TYPE_VIEW) {
      LARGE_INTEGER q; QueryPerformanceCounter(&q); if (!g_invalidFrom) { g_invalidFrom = q.QuadPart; Log("RIGTAP_INVALID_MS: first xrLocateViews on a space of type %d, flags %llu as the runtime gave them", type, (unsigned long long)vs->viewStateFlags); }
      if (double(q.QuadPart - g_invalidFrom) * 1000.0 / double(g_qpf) <= g_invalidMs) {
        vs->viewStateFlags &= ~(XR_VIEW_STATE_ORIENTATION_VALID_BIT | XR_VIEW_STATE_POSITION_VALID_BIT | XR_VIEW_STATE_ORIENTATION_TRACKED_BIT | XR_VIEW_STATE_POSITION_TRACKED_BIT);
        unsigned n = ++g_invalidCalls; if (n <= 8) Log("RIGTAP_INVALID_MS: xrLocateViews %u answered without a valid pose (display time %lld)", n, (long long)li->displayTime);
      }
    }
  }
  return r;
}
XrResult XRAPI_CALL MyCreateRef(XrSession s, const XrReferenceSpaceCreateInfo* ci, XrSpace* sp) {
  XrResult r = g_nextCreateRef(s, ci, sp);
  if (XR_SUCCEEDED(r) && ci && sp) { std::lock_guard<std::mutex> l(g_spaceMu); g_spaceTypeOf[*sp] = (int)ci->referenceSpaceType; }
  if (XR_SUCCEEDED(r) && ci && sp) { try { std::lock_guard<std::mutex> l(g_mu); g_space[*sp] = (int)ci->referenceSpaceType; } catch (...) {} }
  return r;
}

// Before the instance exists the layer above (OFXR) asks, with a null instance, for
// xrEnumerateInstanceExtensionProperties (does the runtime list XR_KHR_D3D12_enable, XR_KHR_visibility_mask). There is
// no next entry yet, so the answer comes from the loader in the process (as OFXR itself does for the layer above it);
// without a loader: RIGTAP_EXTENSIONS, else the Pimax runtime's list (tools\runtime-probe, night-d3d12b\run.txt).
PFN_xrGetInstanceProcAddr FindLoader() {
  auto entry = [](HMODULE m) -> PFN_xrGetInstanceProcAddr { if (!m || GetProcAddress(m, "xrNegotiateLoaderApiLayerInterface") || GetProcAddress(m, "xrNegotiateLoaderRuntimeInterface") || !GetProcAddress(m, "xrEnumerateApiLayerProperties")) return nullptr; return (PFN_xrGetInstanceProcAddr)GetProcAddress(m, "xrGetInstanceProcAddr"); };
  if (auto e = entry(GetModuleHandleW(L"openxr_loader.dll"))) return e;
  std::vector<HMODULE> mods(1024); DWORD bytes = 0; if (!K32EnumProcessModules(GetCurrentProcess(), mods.data(), DWORD(mods.size() * sizeof(HMODULE)), &bytes)) return nullptr;
  size_t n = bytes / sizeof(HMODULE); if (n > mods.size()) n = mods.size();
  for (size_t i = 0; i < n; ++i) if (auto e = entry(mods[i])) return e;
  return nullptr;
}
std::vector<std::string> g_fallbackExt;
XrResult XRAPI_CALL FallbackEnumerate(const char*, uint32_t cap, uint32_t* count, XrExtensionProperties* props) {
  const uint32_t n = (uint32_t)g_fallbackExt.size(); if (!count) return XR_ERROR_VALIDATION_FAILURE; *count = n; if (cap == 0) return XR_SUCCESS; if (cap < n || !props) return XR_ERROR_SIZE_INSUFFICIENT;
  for (uint32_t i = 0; i < n; ++i) { strncpy(props[i].extensionName, g_fallbackExt[i].c_str(), XR_MAX_EXTENSION_NAME_SIZE - 1); props[i].extensionName[XR_MAX_EXTENSION_NAME_SIZE - 1] = 0; props[i].extensionVersion = 1; }
  return XR_SUCCESS;
}
XrResult XRAPI_CALL MyGipa(XrInstance inst, const char* name, PFN_xrVoidFunction* fn) {
  if (!fn || !name) return XR_ERROR_VALIDATION_FAILURE;
  if (!g_nextGipa || inst == XR_NULL_HANDLE) {
    *fn = nullptr;
    if (strcmp(name, "xrEnumerateInstanceExtensionProperties") && strcmp(name, "xrEnumerateApiLayerProperties")) return XR_ERROR_FUNCTION_UNSUPPORTED;
    if (auto loader = FindLoader()) { if (XR_SUCCEEDED(loader(XR_NULL_HANDLE, name, fn)) && *fn) return XR_SUCCESS; *fn = nullptr; }
    if (!strcmp(name, "xrEnumerateInstanceExtensionProperties")) {
      if (g_fallbackExt.empty()) { std::string e = EnvA("RIGTAP_EXTENSIONS");
        if (e.empty()) e = "XR_KHR_D3D11_enable;XR_KHR_D3D12_enable;XR_KHR_vulkan_enable;XR_KHR_vulkan_enable2;XR_KHR_opengl_enable;XR_KHR_composition_layer_depth;XR_KHR_win32_convert_performance_counter_time;XR_KHR_visibility_mask;XR_FB_display_refresh_rate;XR_EXT_hand_tracking;XR_EXT_hand_joints_motion_range;XR_EXT_eye_gaze_interaction;XR_EXT_win32_appcontainer_compatible;XR_VARJO_quad_views;XR_VARJO_foveated_rendering";
        size_t a = 0; while (a <= e.size()) { size_t z = e.find(';', a); if (z == std::string::npos) z = e.size(); if (z > a) g_fallbackExt.push_back(e.substr(a, z - a)); a = z + 1; } }
      *fn = (PFN_xrVoidFunction)FallbackEnumerate; return XR_SUCCESS; }
    return XR_ERROR_FUNCTION_UNSUPPORTED;
  }
  XrResult r = g_nextGipa(inst, name, fn); if (XR_FAILED(r) || !*fn) return r;
#define HOOK(n, next, mine) if (!strcmp(name, n)) { if (next) *fn = (PFN_xrVoidFunction)mine; return r; }
  HOOK("xrEndFrame", g_nextEnd, MyEnd) HOOK("xrCreateSession", g_nextCreateSession, MyCreateSession) HOOK("xrDestroySession", g_nextDestroySession, MyDestroySession)
  HOOK("xrCreateSwapchain", g_nextCreateSc, MyCreateSc) HOOK("xrDestroySwapchain", g_nextDestroySc, MyDestroySc) HOOK("xrAcquireSwapchainImage", g_nextAcquire, MyAcquire)
  HOOK("xrReleaseSwapchainImage", g_nextRelease, MyRelease) HOOK("xrCreateReferenceSpace", g_nextCreateRef, MyCreateRef) HOOK("xrLocateViews", (g_invalidMs > 0 ? g_nextLocate : nullptr), MyLocateViews)
#undef HOOK
  return r;
}
XrResult XRAPI_CALL MyCreateInstance(const XrInstanceCreateInfo* info, const XrApiLayerCreateInfo* li, XrInstance* inst) {
  if (!li || !li->nextInfo || !li->nextInfo->nextCreateApiLayerInstance || !li->nextInfo->nextGetInstanceProcAddr) return XR_ERROR_INITIALIZATION_FAILED;
  PFN_xrGetInstanceProcAddr gipa = li->nextInfo->nextGetInstanceProcAddr; XrApiLayerCreateInfo next = *li; next.nextInfo = li->nextInfo->next;
  XrResult r = li->nextInfo->nextCreateApiLayerInstance(info, &next, inst);
  if (XR_FAILED(r) || !inst) return r;
  LARGE_INTEGER f; QueryPerformanceFrequency(&f); g_qpf = f.QuadPart;
  // Every next function is resolved here, once: a hook is only handed out when its next exists.
  auto get = [&](const char* n) { PFN_xrVoidFunction p = nullptr; return XR_SUCCEEDED(gipa(*inst, n, &p)) ? p : nullptr; };
  g_nextEnd = (PFN_xrEndFrame)get("xrEndFrame"); g_nextCreateSession = (PFN_xrCreateSession)get("xrCreateSession"); g_nextDestroySession = (PFN_xrDestroySession)get("xrDestroySession");
  g_nextCreateSc = (PFN_xrCreateSwapchain)get("xrCreateSwapchain"); g_nextDestroySc = (PFN_xrDestroySwapchain)get("xrDestroySwapchain"); g_nextEnumImages = (PFN_xrEnumerateSwapchainImages)get("xrEnumerateSwapchainImages");
  g_nextAcquire = (PFN_xrAcquireSwapchainImage)get("xrAcquireSwapchainImage"); g_nextRelease = (PFN_xrReleaseSwapchainImage)get("xrReleaseSwapchainImage"); g_nextCreateRef = (PFN_xrCreateReferenceSpace)get("xrCreateReferenceSpace");
  g_nextLocate = (PFN_xrLocateViews)get("xrLocateViews"); g_invalidMs = atof(EnvA("RIGTAP_INVALID_MS").c_str()); g_invalidFrom = 0;
  g_nextGipa = gipa;
  std::string dir = EnvA("RIGTAP_DIR");
  if (!dir.empty()) {
    g_dir = Widen(dir); while (!g_dir.empty() && (g_dir.back() == L'\\' || g_dir.back() == L'/')) g_dir.pop_back(); CreateDirectoryW(g_dir.c_str(), nullptr);
    if (!g_log) g_log = _wfopen((g_dir + L"\\rigtap.log").c_str(), L"a");
    g_enabled = g_log != nullptr;
    std::string v = EnvA("RIGTAP_SCALE"); if (!v.empty()) { g_defScale = (uint32_t)strtoul(v.c_str(), nullptr, 10); if (g_defScale < 1) g_defScale = 1; }
    v = EnvA("RIGTAP_CROP"); if (!v.empty() && !ParseCrop(v, g_defCrop)) Log("RIGTAP_CROP '%s' not understood", v.c_str());
    v = EnvA("RIGTAP_MAX_MB"); if (!v.empty()) g_maxBytes = strtoull(v.c_str(), nullptr, 10) << 20;
    v = EnvA("RIGTAP_FRAMES"); if (!v.empty()) { g_envFrames = (uint32_t)strtoul(v.c_str(), nullptr, 10); g_envArmPending = g_envFrames > 0; g_envAfterS = atof(EnvA("RIGTAP_AFTER_S").c_str()); }
    Log("instance created (result %d) pid %lu; hooks: end %d session %d swapchain %d images %d acquire %d release %d; env arm %u after %.1f s; disk budget %llu MB",
      (int)r, GetCurrentProcessId(), g_nextEnd != nullptr, g_nextCreateSession != nullptr, g_nextCreateSc != nullptr, g_nextEnumImages != nullptr, g_nextAcquire != nullptr, g_nextRelease != nullptr, g_envFrames, g_envAfterS, (unsigned long long)(g_maxBytes >> 20));
  }
  return r;
}
}  // namespace
extern "C" __declspec(dllexport) XrResult XRAPI_CALL xrNegotiateLoaderApiLayerInterface(const XrNegotiateLoaderInfo* li, const char*, XrNegotiateApiLayerRequest* req) {
  if (!li || !req) return XR_ERROR_INITIALIZATION_FAILED;
  req->layerInterfaceVersion = XR_CURRENT_LOADER_API_LAYER_VERSION; req->layerApiVersion = XR_CURRENT_API_VERSION; req->getInstanceProcAddr = MyGipa; req->createApiLayerInstance = MyCreateInstance; return XR_SUCCESS;
}
