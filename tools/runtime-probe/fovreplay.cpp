// Foveated replay (REPLAY.md): plays OFXR's foveated frame-generation submission to the runtime WITHOUT DCS, OFXR or
// Quad Views, and measures the Pimax compositor's mirrors while it does, to find what makes the feathered focus disc
// flicker. Talks to the runtime DLL directly (no loader, no API layers), D3D11.
//
// What it submits, per frame at the display rate (90 Hz on the Pimax), as OFXR does in foveated mode:
//   layer 0  periphery projection, flags 0, sRGB (29), 1076x1060 per eye, the eye's whole FOV
//   layer 1  focus projection, SOURCE_ALPHA | UNPREMULTIPLIED (alpha mode 0), UNORM (28), 1952x1928 per eye, a centred
//            sub-rectangle FOV, same pose as layer 0, Quad Views' rounded fade in alpha
//   layer 2  a small quad (OFXR's diagnostic panel), SOURCE_ALPHA, head-locked, low left
// Images: an array of two (slice 0 left, 1 right) or one swapchain per eye; one or four swapchain SETS; with four, real
// frames alternate sets 0/1 and generated frames sets 2/3 (OFXR keeps real and synthetic outputs on different sets).
// Cadence 1 (real only), 2 (R G R G) or 3 (R G G). Content is the same on every frame: one uniform bright sky colour in
// both layers, so a correct composite is uniform whatever the blend rule, and any disc is the runtime's.
// Every periphery image carries a 4-block tag (bit 0 generated, bits 1-2 set, bit 3 frame parity) and the variant's
// number in big digits, so each mirror grab says which submission it shows and a person in the headset can name it.
//
//   fovreplay <outdir> [--seconds S] [--loops N] [--settle S] [--track-wait S] [--only a,b,...] [--runtime <dll>]
//                      [--allow-untracked] [--submit-only] [--beep] [--selftest]
// Live control: <outdir>\replay_cmd.txt, read when it changes. Lines key=value:
//   variant=<name|number>   switch to a table variant (stops the cycle)
//   alpha=0|1|2 array=0|1 sets=1|2|4 cadence=1|2|3 quad=0|1 opaque=0|1 stall=<frames, 0 off> focus=0|1
//                           a custom variant from the current one (stops the cycle)
//   cycle=1                 resume the cycle where it stopped        quit=1  end now (summary is written)
// Output: replay.log, grabs.csv (one line per mirror grab), summary.csv, summary.md, PNGs (base, locate, worst grab of
// each variant). Exit codes as probe_run: 0 done, 2 session failure, 3 not tracked, 4 no mirror, 5 session refused.
#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_D3D11
#include "common.h"
#include <d3d11_1.h>
#include <dxgi1_4.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <openxr/openxr_loader_negotiation.h>
#include <algorithm>
#include <atomic>
#include <mutex>
#include <thread>

static FILE* g_log = nullptr; static std::mutex g_logMutex;
static void L(const char* fmt, ...) {
  std::lock_guard<std::mutex> lk(g_logMutex);
  va_list a; va_start(a, fmt); SYSTEMTIME t; GetLocalTime(&t);
  fprintf(g_log, "%02d:%02d:%02d.%03d ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds); vfprintf(g_log, fmt, a); fputc('\n', g_log); fflush(g_log);
  va_end(a);
}
static double Now() { static LARGE_INTEGER f{}, t0{}; if (!f.QuadPart) { QueryPerformanceFrequency(&f); QueryPerformanceCounter(&t0); } LARGE_INTEGER q; QueryPerformanceCounter(&q); return double(q.QuadPart - t0.QuadPart) / f.QuadPart; }

// ---- Pimax PVR client interface (as probe_run): the runtime's own HMD handle, config keys, shared mirrors ----
struct PvrIface { void* slot[80]; };
static std::atomic<void*> g_hmd{nullptr}; static PvrIface* g_pvr = nullptr;
static int (*g_origEnd)(void*, long long, const void* const*, unsigned) = nullptr;
static int (*g_origSubmit)(void*, long long, const void* const*, unsigned) = nullptr;
static int HookEnd(void* h, long long f, const void* const* l, unsigned c) { g_hmd = h; return g_origEnd(h, f, l, c); }
static int HookSubmit(void* h, long long f, const void* const* l, unsigned c) { g_hmd = h; return g_origSubmit(h, f, l, c); }
static void InstallPvrHook() {
  HMODULE m = LoadLibraryW(L"LibPVRClient64.dll"); if (!m) { L("no LibPVRClient64.dll: not a Pimax runtime, mirrors unavailable"); return; }
  auto gi = (PvrIface* (*)(uint32_t, uint32_t))GetProcAddress(m, "getPvrInterface"); if (!gi) return;
  for (uint32_t minor = 23; minor <= 40; ++minor) {
    PvrIface* t = gi(1, minor); if (!t) continue;
    if (!g_pvr || minor == 32) g_pvr = t;
    DWORD old; if (!VirtualProtect(t, sizeof(void*) * 40, PAGE_READWRITE, &old)) continue;
    if (t->slot[31] != (void*)HookEnd) { if (!g_origEnd) g_origEnd = (decltype(g_origEnd))t->slot[31]; if (t->slot[31] == (void*)g_origEnd) t->slot[31] = (void*)HookEnd; }
    if (t->slot[34] != (void*)HookSubmit) { if (!g_origSubmit) g_origSubmit = (decltype(g_origSubmit))t->slot[34]; if (t->slot[34] == (void*)g_origSubmit) t->slot[34] = (void*)HookSubmit; }
    VirtualProtect(t, sizeof(void*) * 40, old, &old);
  }
}
static int PvrGetInt(const char* k, int def) { void* h = g_hmd; return g_pvr && h ? ((int (*)(void*, const char*, int))g_pvr->slot[18])(h, k, def) : def; }
static int PvrSetInt(const char* k, int v) { void* h = g_hmd; return g_pvr && h ? ((int (*)(void*, const char*, int))g_pvr->slot[19])(h, k, v) : -1; }
static int64_t PvrGetInt64(const char* k, int64_t def) { void* h = g_hmd; return g_pvr && h ? ((int64_t (*)(void*, const char*, int64_t))g_pvr->slot[60])(h, k, def) : def; }

// ---- OpenXR, straight from the runtime DLL ----
static PFN_xrGetInstanceProcAddr g_gipa = nullptr; static XrInstance g_inst = XR_NULL_HANDLE;
template <typename T> static T fn(const char* name) { PFN_xrVoidFunction f = nullptr; g_gipa(g_inst, name, &f); if (!f) L("missing %s", name); return reinterpret_cast<T>(f); }
static PFN_xrPollEvent xrPollEvent_; static PFN_xrBeginSession xrBeginSession_; static PFN_xrEndSession xrEndSession_;
static PFN_xrWaitFrame xrWaitFrame_; static PFN_xrBeginFrame xrBeginFrame_; static PFN_xrEndFrame xrEndFrame_; static PFN_xrLocateViews xrLocateViews_;
static PFN_xrAcquireSwapchainImage xrAcquire; static PFN_xrWaitSwapchainImage xrWaitImg; static PFN_xrReleaseSwapchainImage xrRelease;
static PFN_xrCreateSwapchain xrCreateSwapchain_; static PFN_xrDestroySwapchain xrDestroySwapchain_; static PFN_xrEnumerateSwapchainImages xrEnumImages;

static ID3D11Device* dev = nullptr; static ID3D11DeviceContext* ctx = nullptr; static LUID g_luid{};
static XrSession ses = XR_NULL_HANDLE; static XrSpace space = XR_NULL_HANDLE, viewSpace = XR_NULL_HANDLE;
static XrSessionState g_state = XR_SESSION_STATE_UNKNOWN; static bool g_running = false, g_quit = false;

struct Chain { XrSwapchain h = XR_NULL_HANDLE; std::vector<XrSwapchainImageD3D11KHR> img; int w = 0, hgt = 0, slices = 1; int64_t fmt = 0; uint32_t cur = 0; };
static bool MakeChain(Chain& c, int w, int h, int slices, int64_t fmt) {
  XrSwapchainCreateInfo ci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
  ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
  ci.format = fmt; ci.sampleCount = 1; ci.width = w; ci.height = h; ci.faceCount = 1; ci.arraySize = slices; ci.mipCount = 1;
  XrResult r = xrCreateSwapchain_(ses, &ci, &c.h); if (XR_FAILED(r)) { L("xrCreateSwapchain %dx%d x%d fmt %lld failed %d", w, h, slices, (long long)fmt, r); return false; }
  uint32_t k = 0; xrEnumImages(c.h, 0, &k, nullptr); c.img.assign(k, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
  xrEnumImages(c.h, k, &k, reinterpret_cast<XrSwapchainImageBaseHeader*>(c.img.data()));
  c.w = w; c.hgt = h; c.slices = slices; c.fmt = fmt; return true;
}
static void FreeChain(Chain& c) { if (c.h) xrDestroySwapchain_(c.h); c = Chain(); }
static void Acquire(Chain& c) { XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO}; wi.timeout = XR_INFINITE_DURATION; xrAcquire(c.h, nullptr, &c.cur); xrWaitImg(c.h, &wi); }

static ID3D11Texture2D* MakeTex(const Image& im) {
  D3D11_TEXTURE2D_DESC td{}; td.Width = im.w; td.Height = im.h; td.MipLevels = 1; td.ArraySize = 1; td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  D3D11_SUBRESOURCE_DATA sd{im.px.data(), UINT(im.w * 4), 0}; ID3D11Texture2D* t = nullptr; dev->CreateTexture2D(&td, &sd, &t); return t;
}
static XrQuaternionf Mul(XrQuaternionf a, XrQuaternionf b) {
  return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
          a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}
static XrFovf ToXr(const Fov& f) { return {float(atan(f.l)), float(atan(f.r)), float(atan(f.u)), float(atan(f.d))}; }

// ---- what is submitted ----
static const int PW = 1076, PH = 1060, FW = 1952, FH = 1928, QW = 256, QH = 128;
static const double kSection = 0.36; // focus half-size as a fraction of the eye's tangent half-size
struct Variant { std::string name; int alpha = 0; bool array = true; int sets = 4; int cadence = 2; bool quad = true; bool opaque = false; int stall = 0; bool focus = true; };
static std::vector<Variant> Table() {
  std::vector<Variant> t;
  auto add = [&](const char* n, auto mod) { Variant v; v.name = n; mod(v); t.push_back(v); };
  add("control_periphery_only", [](Variant& v) { v.focus = false; });
  add("prod_2x", [](Variant&) {});
  add("prod_3x", [](Variant& v) { v.cadence = 3; });
  add("real_only_1x", [](Variant& v) { v.cadence = 1; });
  add("one_set_2x", [](Variant& v) { v.sets = 1; });
  add("per_eye_2x", [](Variant& v) { v.array = false; });
  add("alpha1_premult_2x", [](Variant& v) { v.alpha = 1; });
  add("alpha2_srgb_2x", [](Variant& v) { v.alpha = 2; });
  add("no_quad_2x", [](Variant& v) { v.quad = false; });
  add("opaque_focus_2x", [](Variant& v) { v.opaque = true; });
  add("stall_2x", [](Variant& v) { v.stall = 45; });
  add("prod_2x_again", [](Variant&) {});
  return t;
}
static std::string Describe(const Variant& v) {
  char b[200]; snprintf(b, sizeof(b), "focus %d alpha %d array %d sets %d cadence %d quad %d opaque %d stall %d", v.focus, v.alpha, v.array, v.sets, v.cadence, v.quad, v.opaque, v.stall); return b;
}
static Variant g_var; static std::atomic<int> g_varId{-1}; static std::atomic<bool> g_settled{false};
static Fov g_eyeFov[2], g_focFov[2]; static bool g_haveFov = false; static uint64_t g_viewFlags = 0;
static bool Tracked() { const uint64_t b = XR_VIEW_STATE_ORIENTATION_TRACKED_BIT | XR_VIEW_STATE_POSITION_TRACKED_BIT; return (g_viewFlags & b) == b; }

// Swapchain pools: periphery [array][set][eye], focus [array][srgb][set][eye]; with array only [..][0] is used.
static Chain chP[2][4][2], chF[2][2][4][2], chQ;
static ID3D11Texture2D *texSky = nullptr, *texFoc[3] = {}, *texLocate = nullptr, *texBlack = nullptr, *texWhite = nullptr, *texQuad = nullptr;
// Scene modes beyond the variant: calibration phases.
enum class Scene { measure, base, tag, locate };
static Scene g_scene = Scene::base; static int g_tagLit = -1;
static int g_frameNo = 0, g_realNo = 0, g_genNo = 0, g_endFailures = 0;

// Periphery pixel geometry: the focus box, the tag blocks above it and the digits below it.
struct Rect { int x0, y0, x1, y1; };
static Rect g_focusPx, g_tag[4]; static int g_digitX = 0, g_digitY = 0, g_digitH = 0;
static void Geometry() {
  const Fov& e = g_eyeFov[0];
  auto px = [&](double tx) { return int((tx - e.l) / (e.r - e.l) * PW); }; auto py = [&](double ty) { return int((e.u - ty) / (e.u - e.d) * PH); };
  g_focusPx = {px(g_focFov[0].l), py(g_focFov[0].u), px(g_focFov[0].r), py(g_focFov[0].d)};
  int fh = g_focusPx.y1 - g_focusPx.y0, bw = PW / 20, bh = PH / 20, gap = bw / 2;
  int ty = std::max(PH / 20, g_focusPx.y0 - fh * 45 / 100 - bh), tx = (g_focusPx.x0 + g_focusPx.x1) / 2 - (4 * bw + 3 * gap) / 2;
  for (int k = 0; k < 4; ++k) g_tag[k] = {tx + k * (bw + gap), ty, tx + k * (bw + gap) + bw, ty + bh};
  g_digitH = PH / 14; g_digitY = std::min(PH - PH / 20 - g_digitH, g_focusPx.y1 + fh * 45 / 100); g_digitX = (g_focusPx.x0 + g_focusPx.x1) / 2 - g_digitH * 6 / 10;
  L("geometry: focus px %d,%d-%d,%d tags y %d digits y %d", g_focusPx.x0, g_focusPx.y0, g_focusPx.x1, g_focusPx.y1, ty, g_digitY);
}
static void CopyBox(ID3D11Texture2D* dst, UINT sub, ID3D11Texture2D* src, int x0, int y0, int x1, int y1) {
  x0 = std::max(0, x0); y0 = std::max(0, y0); x1 = std::min(PW, x1); y1 = std::min(PH, y1);
  for (int y = y0; y < y1; y += 64) for (int x = x0; x < x1; x += 64) {
    D3D11_BOX b{0, 0, 0, UINT(std::min(64, x1 - x)), UINT(std::min(64, y1 - y)), 1}; ctx->CopySubresourceRegion(dst, sub, x, y, 0, src, 0, &b); }
}
// Seven-segment digits for the variant number (black on the sky).
static void Digits(ID3D11Texture2D* dst, UINT sub, int number) {
  static const uint8_t seg[10] = {0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F};
  std::string s = std::to_string(number); int h = g_digitH, w = h * 55 / 100, t = std::max(4, h / 9), x = g_digitX - int(s.size() - 1) * (w + t * 2) / 2;
  for (char ch : s) { uint8_t m = seg[ch - '0']; int y = g_digitY, hm = y + h / 2;
    if (m & 1) CopyBox(dst, sub, texBlack, x, y, x + w, y + t); if (m & 2) CopyBox(dst, sub, texBlack, x + w - t, y, x + w, hm);
    if (m & 4) CopyBox(dst, sub, texBlack, x + w - t, hm, x + w, y + h); if (m & 8) CopyBox(dst, sub, texBlack, x, y + h - t, x + w, y + h);
    if (m & 16) CopyBox(dst, sub, texBlack, x, hm, x + t, y + h); if (m & 32) CopyBox(dst, sub, texBlack, x, y, x + t, hm);
    if (m & 64) CopyBox(dst, sub, texBlack, x, hm - t / 2, x + w, hm + t / 2); x += w + t * 2; }
}

static bool EnsurePools(const Variant& v) {
  // Keep only what this variant uses: four sets of 1952x1928 arrays are ~360 MB.
  int srgb = v.alpha == 2 ? 1 : 0;
  for (int a = 0; a < 2; ++a) for (int s = 0; s < 4; ++s) for (int e = 0; e < 2; ++e) {
    bool needP = a == int(v.array) && s < v.sets && (!v.array || e == 0);
    if (!needP && chP[a][s][e].h) FreeChain(chP[a][s][e]);
    if (needP && !chP[a][s][e].h && !MakeChain(chP[a][s][e], PW, PH, v.array ? 2 : 1, 29)) return false;
    for (int f = 0; f < 2; ++f) { bool needF = needP && v.focus && f == srgb;
      if (!needF && chF[a][f][s][e].h) FreeChain(chF[a][f][s][e]);
      if (needF && !chF[a][f][s][e].h && !MakeChain(chF[a][f][s][e], FW, FH, v.array ? 2 : 1, f ? 29 : 28)) return false; }
  }
  if (!chQ.h && !MakeChain(chQ, QW, QH, 1, 28)) return false;
  return true;
}

static void PollEvents() {
  XrEventDataBuffer ev{XR_TYPE_EVENT_DATA_BUFFER};
  while (xrPollEvent_(g_inst, &ev) == XR_SUCCESS) {
    if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
      g_state = reinterpret_cast<XrEventDataSessionStateChanged*>(&ev)->state; L("session state %d", g_state);
      if (g_state == XR_SESSION_STATE_READY) { XrSessionBeginInfo bi{XR_TYPE_SESSION_BEGIN_INFO}; bi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO; if (XR_SUCCEEDED(xrBeginSession_(ses, &bi))) g_running = true; }
      else if (g_state == XR_SESSION_STATE_STOPPING) { xrEndSession_(ses); g_running = false; }
      else if (g_state == XR_SESSION_STATE_EXITING || g_state == XR_SESSION_STATE_LOSS_PENDING) g_quit = true;
    }
    ev = {XR_TYPE_EVENT_DATA_BUFFER};
  }
}

// One frame. The tag code of what it submitted goes to the log of submissions (sub.csv).
static FILE* g_sub = nullptr;
static bool Frame() {
  PollEvents(); if (g_quit) return false;
  if (!g_running) { Sleep(20); return true; }
  const Variant& v = g_var;
  if (g_scene == Scene::measure && v.stall > 0 && g_frameNo % v.stall == v.stall - 1) Sleep(30); // a missed frame: the runtime's extrapolator
  XrFrameState fst{XR_TYPE_FRAME_STATE}; XrFrameWaitInfo fwi{XR_TYPE_FRAME_WAIT_INFO};
  if (XR_FAILED(xrWaitFrame_(ses, &fwi, &fst))) return false;
  XrFrameBeginInfo fbi{XR_TYPE_FRAME_BEGIN_INFO}; if (XR_FAILED(xrBeginFrame_(ses, &fbi))) return false;
  XrCompositionLayerProjectionView vp[2], vf[2];
  XrCompositionLayerProjection lp{XR_TYPE_COMPOSITION_LAYER_PROJECTION}, lf{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
  XrCompositionLayerQuad lq{XR_TYPE_COMPOSITION_LAYER_QUAD};
  const XrCompositionLayerBaseHeader* layers[3]; uint32_t nl = 0;
  if (fst.shouldRender) {
    XrViewLocateInfo vli{XR_TYPE_VIEW_LOCATE_INFO}; vli.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO; vli.displayTime = fst.predictedDisplayTime; vli.space = space;
    XrViewState vs{XR_TYPE_VIEW_STATE}; XrView views[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}}; uint32_t vc = 0;
    if (XR_SUCCEEDED(xrLocateViews_(ses, &vli, &vs, 2, &vc, views))) {
      g_viewFlags = vs.viewStateFlags;
      if (!g_haveFov && views[0].fov.angleRight > views[0].fov.angleLeft && views[1].fov.angleRight > views[1].fov.angleLeft) {
        for (int e = 0; e < 2; ++e) { const XrFovf& f = views[e].fov; g_eyeFov[e] = {tan(f.angleLeft), tan(f.angleRight), tan(f.angleUp), tan(f.angleDown)};
          double hx = kSection * (g_eyeFov[e].r - g_eyeFov[e].l) / 2, hy = kSection * (g_eyeFov[e].u - g_eyeFov[e].d) / 2; g_focFov[e] = {-hx, hx, hy, -hy}; }
        g_haveFov = true; Geometry();
      }
      bool focus = g_scene == Scene::locate || (g_scene == Scene::measure && v.focus);
      bool measuring = g_scene == Scene::measure;
      int cadence = measuring ? v.cadence : 1, sets = measuring ? v.sets : 1;
      // The cadence slips one frame every 181: the mirror is written every other display frame, always on the same
      // parity, so without the slip a 2X run would show it only real or only generated frames.
      bool gen = cadence > 1 && (g_frameNo + g_frameNo / 181) % cadence != 0;
      int set = sets == 1 ? 0 : (sets == 2 ? (gen ? 1 : 0) : (gen ? 2 + g_genNo % 2 : g_realNo % 2));
      if (gen) ++g_genNo; else ++g_realNo;
      int tag = (gen ? 1 : 0) | ((set & 3) << 1) | ((g_frameNo & 1) << 3);
      bool arr = measuring ? v.array : true; int srgb = measuring && v.alpha == 2 ? 1 : 0;
      ID3D11Texture2D* foc = g_scene == Scene::locate ? texLocate : (v.opaque ? texFoc[2] : texFoc[v.alpha == 0 ? 0 : 1]);
      for (int e = 0; e < 2; ++e) {
        Chain& cp = chP[arr][set][arr ? 0 : e]; UINT sub = arr ? e : 0;
        if (!arr || e == 0) Acquire(cp);
        ID3D11Texture2D* img = cp.img[cp.cur].texture;
        ctx->CopySubresourceRegion(img, sub, 0, 0, 0, texSky, 0, nullptr);
        for (int k = 0; k < 4; ++k) {
          bool lit = g_scene == Scene::tag ? k == g_tagLit : (measuring && ((tag >> k) & 1));
          CopyBox(img, sub, lit ? texWhite : texBlack, g_tag[k].x0, g_tag[k].y0, g_tag[k].x1, g_tag[k].y1);
        }
        if (measuring && g_varId >= 0) Digits(img, sub, g_varId);
        vp[e] = {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}; vp[e].pose = views[e].pose; vp[e].fov = ToXr(g_eyeFov[e]);
        vp[e].subImage.swapchain = cp.h; vp[e].subImage.imageRect = {{0, 0}, {PW, PH}}; vp[e].subImage.imageArrayIndex = sub;
        if (focus) {
          Chain& cf = chF[arr][srgb][set][arr ? 0 : e];
          if (!arr || e == 0) Acquire(cf);
          ctx->CopySubresourceRegion(cf.img[cf.cur].texture, sub, 0, 0, 0, foc, 0, nullptr);
          vf[e] = {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}; vf[e].pose = views[e].pose; vf[e].fov = ToXr(g_focFov[e]);
          vf[e].subImage.swapchain = cf.h; vf[e].subImage.imageRect = {{0, 0}, {FW, FH}}; vf[e].subImage.imageArrayIndex = sub;
        }
      }
      bool quad = measuring && v.quad;
      if (quad) { Acquire(chQ); ctx->CopySubresourceRegion(chQ.img[chQ.cur].texture, 0, 0, 0, 0, texQuad, 0, nullptr); }
      ctx->Flush();
      for (int e = 0; e < 2; ++e) { if (!arr || e == 1) { xrRelease(chP[arr][set][arr ? 0 : e].h, nullptr); if (focus) xrRelease(chF[arr][srgb][set][arr ? 0 : e].h, nullptr); } }
      if (quad) xrRelease(chQ.h, nullptr);
      lp.space = space; lp.viewCount = 2; lp.views = vp; layers[nl++] = reinterpret_cast<XrCompositionLayerBaseHeader*>(&lp);
      uint64_t ff = 0;
      if (focus) {
        ff = (g_scene == Scene::measure && v.opaque) ? 0 : (XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT | (g_scene == Scene::locate || v.alpha == 0 ? XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT : 0));
        lf.space = space; lf.viewCount = 2; lf.views = vf; lf.layerFlags = ff; layers[nl++] = reinterpret_cast<XrCompositionLayerBaseHeader*>(&lf);
      }
      if (quad) {
        lq.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT; lq.space = viewSpace; lq.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
        lq.subImage.swapchain = chQ.h; lq.subImage.imageRect = {{0, 0}, {QW, QH}}; lq.pose.orientation.w = 1; lq.pose.position = {-0.9f, -0.75f, -1.0f}; lq.size = {0.25f, 0.125f};
        layers[nl++] = reinterpret_cast<XrCompositionLayerBaseHeader*>(&lq);
      }
      if (g_sub && measuring) fprintf(g_sub, "%.4f,%d,%d,%d,%d,%d,%d,%llu,%lld\n", Now(), g_varId.load(), g_frameNo, gen ? 1 : 0, set, tag, int(nl), (unsigned long long)ff, (long long)fst.predictedDisplayTime);
    }
  }
  XrFrameEndInfo fei{XR_TYPE_FRAME_END_INFO}; fei.displayTime = fst.predictedDisplayTime; fei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE; fei.layerCount = nl; fei.layers = layers;
  if (XR_FAILED(xrEndFrame_(ses, &fei))) ++g_endFailures;
  ++g_frameNo;
  return true;
}
static bool Frames(int n) { for (int i = 0; i < n; ++i) if (!Frame()) return false; return true; }

// ---- mirror grabbing and the metric (its own thread and D3D11 device) ----
// Kinds: 0 undistorted (after composition, before reprojection; buffers _0 and _1), 1 screen (distorted, after
// reprojection). Work at half resolution, luma of the stored codes (Rec. 709 weights).
enum Label : uint8_t { kNone = 0, kInside = 1, kBand = 2, kRing = 3, kTag0 = 4 };
struct KindState {
  int w = 0, h = 0; std::vector<float> acc; int accN = 0;          // accumulation for calibration
  std::vector<float> base; std::vector<uint8_t> label;              // after calibration
  float tagOff[2][4] = {}, tagOn[2][4] = {}; bool ready = false;
  std::vector<double> tsum, tsq; int tn = 0;                        // temporal per-pixel stats of the current variant
  std::vector<uint8_t> worstRgba; int worstW = 0, worstH = 0; double worst = -1; int worstVar = -2;
};
struct Rec { double t; int var; int kind; int tag; float in[2], band[2], ring[2], blk[2], ringAbs[2]; };
struct VarStat { int var; int kind; double bandTstd[2], ringTstd[2]; int n; };
static KindState g_k[2]; static std::vector<Rec> g_recs; static std::vector<VarStat> g_vstats; static std::mutex g_grabMutex;
enum class GrabMode { idle, accumulate, measure, stop };
static std::atomic<GrabMode> g_grabMode{GrabMode::idle}; static std::atomic<int> g_grabs{0}, g_grabFail{0};
static std::atomic<int> g_measVar{-2};
static std::string g_out;

static void FinishVariant(KindState& k, int kind, int var) {
  if (k.tn < 2 || var < -1) return;
  VarStat s{var, kind, {0, 0}, {0, 0}, k.tn};
  for (int e = 0; e < 2; ++e) { double sb = 0, sr = 0; int nb = 0, nr = 0;
    for (int y = 0; y < k.h; ++y) for (int x = e * k.w / 2; x < (e + 1) * k.w / 2; ++x) { size_t i = size_t(y) * k.w + x; uint8_t lb = k.label[i]; if (lb != kBand && lb != kRing) continue;
      double m = k.tsum[i] / k.tn, var2 = k.tsq[i] / k.tn - m * m, sd = var2 > 0 ? sqrt(var2) : 0; if (lb == kBand) { sb += sd; ++nb; } else { sr += sd; ++nr; } }
    s.bandTstd[e] = nb ? sb / nb : 0; s.ringTstd[e] = nr ? sr / nr : 0; }
  g_vstats.push_back(s);
  if (k.worst >= 0 && !k.worstRgba.empty()) { Image im(k.worstW, k.worstH); im.px = k.worstRgba; char n[96]; snprintf(n, sizeof(n), "\\worst_v%02d_%s.png", var, kind ? "screen" : "undist"); SavePng(Wide(g_out + n), im); }
  k.worst = -1; k.worstRgba.clear();
}

// One mirror image (full resolution, RGBA or BGRA rows): accumulated for calibration or measured.
static void ProcessImage(int kind, const BYTE* data, UINT pitch, UINT width, UINT height, bool bgra, double t, int var, bool settled, GrabMode mode) {
  static thread_local std::vector<float> lum;
  int W = int(width) / 2, H = int(height) / 2; lum.resize(size_t(W) * H);
  for (int y = 0; y < H; ++y) for (int x = 0; x < W; ++x) { double acc = 0;
    for (int dy = 0; dy < 2; ++dy) for (int dx = 0; dx < 2; ++dx) { const BYTE* p = data + (2 * y + dy) * pitch + (2 * x + dx) * 4;
      double r = bgra ? p[2] : p[0], g = p[1], b = bgra ? p[0] : p[2]; acc += 0.2126 * r + 0.7152 * g + 0.0722 * b; }
    lum[size_t(y) * W + x] = float(acc / 4); }
  KindState& k = g_k[kind];
  std::lock_guard<std::mutex> lk(g_grabMutex);
  if (mode == GrabMode::accumulate) {
    if (k.w != W || k.h != H) { k.w = W; k.h = H; k.acc.assign(size_t(W) * H, 0); k.accN = 0; }
    for (size_t i = 0; i < lum.size(); ++i) k.acc[i] += lum[i]; ++k.accN;
    return;
  }
  if (mode != GrabMode::measure || !k.ready || k.w != W || k.h != H) return;
  if (var != k.worstVar) { FinishVariant(k, kind, k.worstVar); k.worstVar = var; k.tn = 0; std::fill(k.tsum.begin(), k.tsum.end(), 0.0); std::fill(k.tsq.begin(), k.tsq.end(), 0.0); }
  if (!settled) return;
    Rec r{t, var, kind, 0, {}, {}, {}, {}, {}};
    double sIn[2] = {}, sB[2] = {}, sR[2] = {}, sRa[2] = {}; int nIn[2] = {}, nB[2] = {}, nR[2] = {}; double tg[2][4] = {}; int ntg[2][4] = {};
    const int B = 12; int bw = (W / 2 + B - 1) / B, bh = (H + B - 1) / B; std::vector<double> bs(size_t(2) * bw * bh, 0); std::vector<int> bn(bs.size(), 0), ball(bs.size(), 0);
    for (int y = 0; y < H; ++y) for (int x = 0; x < W; ++x) { size_t i = size_t(y) * W + x; int e = x >= W / 2 ? 1 : 0; uint8_t lb = k.label[i]; if (!lb) continue;
      double dv = lum[i] - k.base[i];
      if (lb >= kTag0) { tg[e][lb - kTag0] += lum[i]; ++ntg[e][lb - kTag0]; continue; }
      if (lb == kInside) { sIn[e] += dv; ++nIn[e]; } else if (lb == kBand) { sB[e] += dv; ++nB[e]; } else { sR[e] += dv; sRa[e] += fabs(dv); ++nR[e]; }
      size_t bi = (size_t(e) * bh + y / B) * bw + (x - e * (W / 2)) / B; ++ball[bi]; if (lb == kBand) { bs[bi] += dv; ++bn[bi]; }
      k.tsum[i] += dv; k.tsq[i] += dv * dv; }
    ++k.tn;
    int tagE[2] = {0, 0};
    for (int e = 0; e < 2; ++e) {
      r.in[e] = nIn[e] ? float(sIn[e] / nIn[e]) : 0; r.band[e] = nB[e] ? float(sB[e] / nB[e]) : 0; r.ring[e] = nR[e] ? float(sR[e] / nR[e]) : 0; r.ringAbs[e] = nR[e] ? float(sRa[e] / nR[e]) : 0;
      double mx = 0; for (int by = 0; by < bh; ++by) for (int bx = 0; bx < bw; ++bx) { size_t bi = (size_t(e) * bh + by) * bw + bx; if (bn[bi] * 2 >= ball[bi] && bn[bi] > 8) mx = std::max(mx, fabs(bs[bi] / bn[bi] - r.ring[e])); }
      r.blk[e] = float(mx);
      for (int b = 0; b < 4; ++b) if (ntg[e][b] && tg[e][b] / ntg[e][b] > (k.tagOff[e][b] + k.tagOn[e][b]) / 2) tagE[e] |= 1 << b;
    }
    r.tag = tagE[0] | (tagE[1] << 4);
    g_recs.push_back(r);
    double score = std::max(r.blk[0], r.blk[1]);
    if (score > k.worst) { k.worst = score; k.worstW = int(width); k.worstH = int(height); k.worstRgba.resize(size_t(width) * height * 4);
      for (UINT y = 0; y < height; ++y) { const BYTE* p = data + y * pitch; uint8_t* o = &k.worstRgba[size_t(y) * width * 4];
        for (UINT x = 0; x < width; ++x) { o[x * 4] = bgra ? p[x * 4 + 2] : p[x * 4]; o[x * 4 + 1] = p[x * 4 + 1]; o[x * 4 + 2] = bgra ? p[x * 4] : p[x * 4 + 2]; o[x * 4 + 3] = 255; } } }
}

static void GrabThread() {
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
  IDXGIFactory1* fac = nullptr; CreateDXGIFactory1(IID_PPV_ARGS(&fac)); IDXGIAdapter1* ad = nullptr;
  for (UINT i = 0; fac && fac->EnumAdapters1(i, &ad) != DXGI_ERROR_NOT_FOUND; ++i) { DXGI_ADAPTER_DESC1 d; ad->GetDesc1(&d); if (!memcmp(&d.AdapterLuid, &g_luid, sizeof(LUID))) break; ad->Release(); ad = nullptr; }
  ID3D11Device* gd = nullptr; ID3D11DeviceContext* gc = nullptr;
  D3D11CreateDevice(ad, ad ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &gd, nullptr, &gc);
  if (!gd) { L("grab thread: no device"); return; }
  const char* keys[3] = {"undistorted_mirror_handle_0", "undistorted_mirror_handle_1", "screen_mirror_handle"};
  ID3D11Texture2D* tex[3] = {}; int64_t open[3] = {}; ID3D11Texture2D* st[3] = {};
  while (g_grabMode != GrabMode::stop) {
    GrabMode mode = g_grabMode; if (mode == GrabMode::idle) { Sleep(5); continue; }
    for (int s = 0; s < 3 && g_grabMode != GrabMode::stop; ++s) {
      int kind = s == 2 ? 1 : 0;
      int64_t hnd = PvrGetInt64(keys[s], 0); if (!hnd) hnd = (unsigned)PvrGetInt(keys[s], 0);
      if (hnd && hnd != open[s]) { if (tex[s]) tex[s]->Release(); if (st[s]) st[s]->Release(); tex[s] = nullptr; st[s] = nullptr; open[s] = 0;
        if (SUCCEEDED(gd->OpenSharedResource((HANDLE)hnd, __uuidof(ID3D11Texture2D), (void**)&tex[s])) && tex[s]) open[s] = hnd; }
      if (!tex[s]) { ++g_grabFail; Sleep(2); continue; }
      D3D11_TEXTURE2D_DESC d; tex[s]->GetDesc(&d);
      if (!st[s]) { D3D11_TEXTURE2D_DESC sd = d; sd.Usage = D3D11_USAGE_STAGING; sd.BindFlags = 0; sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ; sd.MiscFlags = 0; sd.ArraySize = 1; sd.MipLevels = 1; sd.SampleDesc.Count = 1; gd->CreateTexture2D(&sd, nullptr, &st[s]); }
      if (!st[s]) { ++g_grabFail; continue; }
      gc->CopySubresourceRegion(st[s], 0, 0, 0, 0, tex[s], 0, nullptr);
      D3D11_MAPPED_SUBRESOURCE m; if (FAILED(gc->Map(st[s], 0, D3D11_MAP_READ, 0, &m))) { ++g_grabFail; continue; }
      bool bgra = d.Format == 87 || d.Format == 91 || d.Format == 90 || d.Format == 88;
      ProcessImage(kind, (const BYTE*)m.pData, m.RowPitch, d.Width, d.Height, bgra, Now(), g_measVar, g_settled, mode);
      gc->Unmap(st[s], 0); ++g_grabs;
    }
  }
  { std::lock_guard<std::mutex> lk(g_grabMutex); for (int kind = 0; kind < 2; ++kind) FinishVariant(g_k[kind], kind, g_k[kind].worstVar); }
  for (int s = 0; s < 3; ++s) { if (tex[s]) tex[s]->Release(); if (st[s]) st[s]->Release(); }
  gc->Release(); gd->Release();
}

// Accumulates n grabs of each kind of the current scene; returns the means (empty when the mirror gave nothing).
static bool Accumulate(int n, std::vector<float> out[2], int timeoutFrames = 900) {
  { std::lock_guard<std::mutex> lk(g_grabMutex); for (auto& k : g_k) { k.accN = 0; std::fill(k.acc.begin(), k.acc.end(), 0.f); } }
  g_grabMode = GrabMode::accumulate;
  for (int f = 0; f < timeoutFrames; f += 10) {
    if (!Frames(10)) return false;
    std::lock_guard<std::mutex> lk(g_grabMutex); if (g_k[0].accN >= n && g_k[1].accN >= n / 2) break;
  }
  g_grabMode = GrabMode::idle; Sleep(60);
  std::lock_guard<std::mutex> lk(g_grabMutex);
  for (int kind = 0; kind < 2; ++kind) { KindState& k = g_k[kind]; out[kind].clear(); if (!k.accN) continue; out[kind].resize(k.acc.size()); for (size_t i = 0; i < k.acc.size(); ++i) out[kind][i] = k.acc[i] / k.accN; }
  return !out[0].empty();
}
static void SaveLum(const std::vector<float>& l, int w, int h, const char* name) {
  Image im(w, h); for (size_t i = 0; i < l.size(); ++i) { uint8_t v = uint8_t(std::min(255.f, std::max(0.f, l[i]))); im.px[i * 4] = im.px[i * 4 + 1] = im.px[i * 4 + 2] = v; im.px[i * 4 + 3] = 255; }
  SavePng(Wide(g_out + "\\" + name), im);
}

static int CalibBase(std::vector<float> base[2]) {
  for (int kind = 0; kind < 2; ++kind) { if (base[kind].empty()) continue; g_k[kind].base = base[kind]; g_k[kind].label.assign(base[kind].size(), kNone);
    SaveLum(base[kind], g_k[kind].w, g_k[kind].h, kind ? "base_screen.png" : "base_undist.png"); }
  // Flat dark mirror = the compositor shows no application (headset untracked).
  double m = 0; for (float v : base[0]) m += v; m /= base[0].size(); L("base mean luma %.1f (undistorted)", m);
  if (m < 20) { L("mirror is dark: the compositor shows no application"); return 3; }
  return 0;
}
static void CalibTag(int t, std::vector<float> lit[2]) {
  for (int kind = 0; kind < 2; ++kind) { KindState& k = g_k[kind]; if (lit[kind].empty() || k.base.empty()) continue; int n[2] = {0, 0};
    for (size_t i = 0; i < k.base.size(); ++i) if (lit[kind][i] - k.base[i] > 60) { k.label[i] = uint8_t(kTag0 + t); int e = int(i % k.w) >= k.w / 2; ++n[e]; k.tagOn[e][t] += lit[kind][i]; k.tagOff[e][t] += k.base[i]; }
    for (int e = 0; e < 2; ++e) { if (n[e]) { k.tagOn[e][t] /= n[e]; k.tagOff[e][t] /= n[e]; } }
    L("tag %d %s: %d / %d pixels, off %.0f on %.0f", t, kind ? "screen" : "undist", n[0], n[1], k.tagOff[0][t], k.tagOn[0][t]); }
}
static int CalibLocate(std::vector<float> loc[2]) {
  for (int kind = 0; kind < 2; ++kind) {
    KindState& k = g_k[kind]; if (loc[kind].empty() || k.base.empty()) continue;
    SaveLum(loc[kind], k.w, k.h, kind ? "locate_screen.png" : "locate_undist.png");
    for (int e = 0; e < 2; ++e) {
      int x0 = e * k.w / 2, x1 = (e + 1) * k.w / 2; double dmax = 0;
      for (int y = 0; y < k.h; ++y) for (int x = x0; x < x1; ++x) { size_t i = size_t(y) * k.w + x; dmax = std::max(dmax, double(k.base[i] - loc[kind][i])); }
      int bx0 = x1, bx1 = x0, by0 = k.h, by1 = 0, nIn = 0, nB = 0, nR = 0;
      auto weight = [&](size_t i) { return dmax > 10 ? (k.base[i] - loc[kind][i]) / dmax : 0.0; };
      for (int y = 0; y < k.h; ++y) for (int x = x0; x < x1; ++x) { size_t i = size_t(y) * k.w + x; if (k.label[i] >= kTag0) continue; double w = weight(i);
        if (w > 0.9) { k.label[i] = kInside; ++nIn; } else if (w > 0.1) { k.label[i] = kBand; ++nB; }
        if (w > 0.1) { bx0 = std::min(bx0, x); bx1 = std::max(bx1, x); by0 = std::min(by0, y); by1 = std::max(by1, y); } }
      int mx = (bx1 - bx0) / 4, my = (by1 - by0) / 4;
      for (int y = std::max(0, by0 - my); y <= std::min(k.h - 1, by1 + my); ++y) for (int x = std::max(x0, bx0 - mx); x <= std::min(x1 - 1, bx1 + mx); ++x) {
        size_t i = size_t(y) * k.w + x; if (k.label[i] == kNone && weight(i) < 0.02) { k.label[i] = kRing; ++nR; } }
      L("locate %s eye %d: focus contrast %.0f, box %d,%d-%d,%d, inside %d band %d ring %d px", kind ? "screen" : "undist", e, dmax, bx0, by0, bx1, by1, nIn, nB, nR);
    }
    k.tsum.assign(k.base.size(), 0); k.tsq.assign(k.base.size(), 0); k.ready = true;
  }
  return g_k[0].ready ? 0 : 4;
}
// Calibration: the base (periphery only, tags dark), each tag block lit alone, and the focus located by a dark focus
// layer (its weight map gives inside / fade band / ring around it).
static int Calibrate() {
  g_scene = Scene::base; Frames(45);
  // The runtime shows its own panels for a while after a session starts ("Controller not detected"): the base is taken
  // only once the centre of both eyes is a flat sky (luma spread below 3 codes), up to a minute.
  std::vector<float> base[2];
  for (int tries = 0;; ++tries) {
    if (!Accumulate(8, base)) { L("calibration: no mirror image"); return 4; }
    KindState& k = g_k[0]; double worst = 0;
    for (int e = 0; e < 2; ++e) { double s = 0, s2 = 0; int n = 0;
      for (int y = k.h / 5; y < k.h * 4 / 5; ++y) for (int x = e * k.w / 2 + k.w / 10; x < (e + 1) * k.w / 2 - k.w / 10; ++x) { double v = base[0][size_t(y) * k.w + x]; s += v; s2 += v * v; ++n; }
      double m = s / n, sd = sqrt(std::max(0.0, s2 / n - m * m)); worst = std::max(worst, sd); }
    L("base try %d: centre luma spread %.2f", tries, worst);
    if (worst < 3 || tries >= 30) break;
    Frames(90);
  }
  if (int r = CalibBase(base)) return r;
  for (int t = 0; t < 4; ++t) { g_scene = Scene::tag; g_tagLit = t; Frames(30); std::vector<float> lit[2]; if (!Accumulate(6, lit)) return 4; CalibTag(t, lit); }
  g_tagLit = -1; g_scene = Scene::locate; Frames(30);
  std::vector<float> loc[2]; if (!Accumulate(12, loc)) return 4;
  return CalibLocate(loc);
}

// ---- command file ----
static FILETIME g_cmdTime{}; static bool g_cycle = true, g_beep = false;
static int ApplyCommands(std::vector<Variant>& table, Variant& custom, bool& quit) {
  // Returns the variant id to switch to (-1 none, -2 custom).
  std::string path = g_out + "\\replay_cmd.txt"; WIN32_FILE_ATTRIBUTE_DATA fa{};
  if (!GetFileAttributesExA(path.c_str(), GetFileExInfoStandard, &fa) || CompareFileTime(&fa.ftLastWriteTime, &g_cmdTime) == 0) return -1;
  g_cmdTime = fa.ftLastWriteTime; Case c; if (!LoadCase(path, c)) return -1;
  int result = -1;
  if (c.count("quit") && Num(c, "quit") != 0) quit = true;
  if (c.count("cycle") && Num(c, "cycle") != 0) g_cycle = true;
  if (c.count("variant")) { std::string v = Str(c, "variant"); g_cycle = false;
    for (size_t i = 0; i < table.size(); ++i) if (table[i].name == v || std::to_string(i) == v) result = int(i); }
  bool any = false; Variant n = g_var;
  auto take = [&](const char* key, auto& field) { if (c.count(key)) { field = std::remove_reference_t<decltype(field)>(Num(c, key)); any = true; } };
  take("alpha", n.alpha); take("array", n.array); take("sets", n.sets); take("cadence", n.cadence); take("quad", n.quad); take("opaque", n.opaque); take("stall", n.stall); take("focus", n.focus);
  if (any) { n.alpha = std::clamp(n.alpha, 0, 2); n.sets = n.sets >= 4 ? 4 : (n.sets >= 2 ? 2 : 1); n.cadence = std::clamp(n.cadence, 1, 3); n.name = "custom"; custom = n; g_cycle = false; result = -2; }
  L("command file: %s", any ? Describe(n).c_str() : (result >= 0 ? table[result].name.c_str() : "no change"));
  return result;
}

// ---- summary ----
struct Agg { int n = 0; double s = 0, s2 = 0, mx = 0; void add(double v) { ++n; s += v; s2 += v * v; mx = std::max(mx, fabs(v)); } double mean() const { return n ? s / n : 0; } double sd() const { if (n < 2) return 0; double m = mean(), v = s2 / n - m * m; return v > 0 ? sqrt(v) : 0; } };
static void WriteSummary(const std::vector<Variant>& names, const Variant& custom) {
  std::lock_guard<std::mutex> lk(g_grabMutex);
  FILE* g = fopen((g_out + "\\grabs.csv").c_str(), "w");
  if (g) { fprintf(g, "t,variant,mirror,tag_left,tag_right,inside_l,band_l,ring_l,block_l,ringabs_l,inside_r,band_r,ring_r,block_r,ringabs_r\n");
    for (auto& r : g_recs) fprintf(g, "%.4f,%d,%s,%d,%d,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f\n", r.t, r.var, r.kind ? "screen" : "undist", r.tag & 15, r.tag >> 4, r.in[0], r.band[0], r.ring[0], r.blk[0], r.ringAbs[0], r.in[1], r.band[1], r.ring[1], r.blk[1], r.ringAbs[1]);
    fclose(g); }
  auto vname = [&](int v) { return v >= 0 && v < int(names.size()) ? names[v].name : (v == 99 ? custom.name : std::string("?")); };
  // Control noise: variant 0 (periphery only) gives the spread of the metric with no focus layer at all.
  std::vector<int> vars; for (auto& r : g_recs) if (std::find(vars.begin(), vars.end(), r.var) == vars.end()) vars.push_back(r.var);
  double ctrl[2][2] = {{0, 0}, {0, 0}}, ctrlBlk[2][2] = {{0, 0}, {0, 0}};
  auto spread = [](std::vector<float> b) { if (b.empty()) return 0.0; std::sort(b.begin(), b.end()); return double(b[std::min(b.size() - 1, size_t(b.size() * 0.99))] - b[b.size() / 2]); };
  for (int kind = 0; kind < 2; ++kind) for (int e = 0; e < 2; ++e) { Agg a; std::vector<float> b; for (auto& r : g_recs) if (r.var == 0 && r.kind == kind) { a.add(r.band[e] - r.ring[e]); b.push_back(r.blk[e]); } ctrl[kind][e] = a.sd(); ctrlBlk[kind][e] = spread(b); }
  FILE* s = fopen((g_out + "\\summary.csv").c_str(), "w"); FILE* md = fopen((g_out + "\\summary.md").c_str(), "w");
  if (!s || !md) { if (s) fclose(s); if (md) fclose(md); return; }
  fprintf(s, "variant,name,mirror,eye,grabs,disc_mean,flicker_sd,block_max,block_p99,real_minus_gen,band_tstd,ring_tstd,verdict\n");
  fprintf(md, "# Replay foveato: riepilogo\n\nValori in codici a 8 bit di luminanza, banda di sfumatura meno anello esterno (0 = composizione corretta).\n"
              "`disco` = media, `sfarfallio` = deviazione standard fra le acquisizioni, `blocco` = il peggior blocco della banda, `R-G` = frame reali meno generati (dal tag).\n"
              "Controllo = solo periferia: il rumore della misura.\n\n| # | Variante | Specchio | Occhio | Acq. | Disco | Sfarfallio | Blocco max / p99 | R-G | Esito |\n|---|---|---|---|---|---|---|---|---|---|\n");
  std::sort(vars.begin(), vars.end());
  for (int v : vars) for (int kind = 0; kind < 2; ++kind) for (int e = 0; e < 2; ++e) {
    Agg d, bl, rr, gg; std::vector<float> blocks;
    for (auto& r : g_recs) if (r.var == v && r.kind == kind) { double x = r.band[e] - r.ring[e]; d.add(x); bl.add(r.blk[e]); blocks.push_back(r.blk[e]);
      int tg = e ? r.tag >> 4 : r.tag & 15; if (tg & 1) gg.add(x); else rr.add(x); }
    if (!d.n) continue;
    std::sort(blocks.begin(), blocks.end()); double p99 = blocks[std::min(blocks.size() - 1, size_t(blocks.size() * 0.99))];
    double bt = 0, rt = 0; for (auto& vs : g_vstats) if (vs.var == v && vs.kind == kind) { bt = vs.bandTstd[e]; rt = vs.ringTstd[e]; }
    double noise = std::max(0.5, 3 * ctrl[kind][e]);
    // Flicker: the band moves between grabs, as a whole (sd) or in a part of it (the worst block above its median).
    bool flick = v != 0 && (d.sd() > noise || spread(blocks) > std::max(2.0, 3 * ctrlBlk[kind][e]));
    bool disc = v != 0 && fabs(d.mean()) > std::max(1.5, noise);
    bool rg = rr.n > 3 && gg.n > 3 && fabs(rr.mean() - gg.mean()) > std::max(1.0, noise);
    std::string verdict = v == 0 ? "control" : (flick ? "FLICKER" : (disc ? "DISC" : (rg ? "REAL!=GEN" : "clean")));
    std::string rgs = (rr.n > 3 && gg.n > 3) ? F(rr.mean() - gg.mean()) : std::string("");
    fprintf(s, "%d,%s,%s,%s,%d,%.3f,%.3f,%.2f,%.2f,%s,%.3f,%.3f,%s\n", v, vname(v).c_str(), kind ? "screen" : "undist", e ? "right" : "left", d.n, d.mean(), d.sd(), bl.mx, p99, rgs.c_str(), bt, rt, verdict.c_str());
    char rgbuf[32] = ""; if (!rgs.empty()) snprintf(rgbuf, sizeof(rgbuf), "%.2f", rr.mean() - gg.mean());
    fprintf(md, "| %d | %s | %s | %s | %d | %.2f | %.2f | %.1f / %.1f | %s | %s |\n", v, vname(v).c_str(), kind ? "distorto" : "non distorto", e ? "destro" : "sinistro", d.n, d.mean(), d.sd(), bl.mx, p99, rgbuf, verdict.c_str());
  }
  fprintf(md, "\nSoglie: sfarfallio > max(0.5, 3 x controllo); disco > max(1.5, la stessa soglia); blocco: p99 meno mediana > max(2, 3 x lo stesso del controllo).\n"
              "Lo specchio e scritto a circa meta della frequenza del display: una differenza che alterna a ogni frame puo sfuggire (il tag dice quali frame sono stati visti).\n");
  fclose(s); fclose(md);
}

// --selftest: the calibration, the metric and the verdicts on synthetic mirror images (no runtime, no headset). A
// fault is planted in two variants: prod_2x gets a bright fade ring in the right eye on generated frames only (the
// flicker under hunt), alpha1_premult_2x a steady one in both eyes; every other variant is clean. Exit 0 when the
// summary says exactly that.
static int SelfTest() {
  const int W = 2880, H = 1440, E = W / 2; std::vector<uint8_t> img(size_t(W) * H * 4); uint32_t rng = 12345;
  auto synth = [&](int scene, int tagLit, int tag, const double fault[2]) {
    for (int y = 0; y < H; ++y) for (int x = 0; x < W; ++x) {
      int e = x >= E, lx = x - e * E; double c[3] = {150, 190, 235};
      for (int k = 0; k < 4; ++k) if (lx >= 560 + k * 90 && lx < 620 + k * 90 && y >= 300 && y < 360) { bool lit = scene == 1 ? k == tagLit : (scene == 3 && ((tag >> k) & 1)); c[0] = c[1] = c[2] = lit ? 255 : 0; }
      double nx = (lx - 720) / 260.0, ny = (700 - y) / 260.0, a = FocusAlpha(nx, ny);
      if (scene == 2) for (int i = 0; i < 3; ++i) c[i] = a * 20 + (1 - a) * c[i];
      if (scene == 3) for (int i = 0; i < 3; ++i) c[i] += fault[e] * 4 * a * (1 - a);
      uint8_t* o = &img[(size_t(y) * W + x) * 4];
      for (int i = 0; i < 3; ++i) { rng = rng * 1664525u + 1013904223u; double v = c[i] + double(int(rng >> 30) - 1); o[i] = uint8_t(std::clamp(v, 0.0, 255.0) + 0.5); }
      o[3] = 255; } };
  auto feed = [&](int n, std::vector<float> out[2], int scene, int tagLit) {
    for (auto& k : g_k) { k.accN = 0; std::fill(k.acc.begin(), k.acc.end(), 0.f); }
    const double none[2] = {0, 0};
    for (int i = 0; i < n; ++i) { synth(scene, tagLit, 0, none); for (int kind = 0; kind < 2; ++kind) ProcessImage(kind, img.data(), W * 4, W, H, false, Now(), -2, false, GrabMode::accumulate); }
    for (int kind = 0; kind < 2; ++kind) { KindState& k = g_k[kind]; out[kind].resize(k.acc.size()); for (size_t i = 0; i < k.acc.size(); ++i) out[kind][i] = k.acc[i] / k.accN; } };
  std::vector<float> base[2], lit[2], loc[2];
  feed(4, base, 0, -1); if (CalibBase(base)) return 1;
  for (int t = 0; t < 4; ++t) { feed(2, lit, 1, t); CalibTag(t, lit); }
  feed(2, loc, 2, -1); if (CalibLocate(loc)) return 1;
  std::vector<Variant> table = Table();
  for (int v = 0; v < int(table.size()); ++v) {
    const Variant& var = table[v]; int realNo = 0, genNo = 0;
    for (int g = 0; g < 36; ++g) {
      bool gen = var.cadence > 1 && g % var.cadence != 0; int sets = var.sets;
      int set = sets == 1 ? 0 : (sets == 2 ? (gen ? 1 : 0) : (gen ? 2 + genNo % 2 : realNo % 2)); if (gen) ++genNo; else ++realNo;
      int tag = (gen ? 1 : 0) | ((set & 3) << 1) | ((g & 1) << 3);
      double fault[2] = {0, 0};
      if (var.name == "prod_2x" && gen) fault[1] = 25; if (var.name == "alpha1_premult_2x") fault[0] = fault[1] = 10;
      synth(3, -1, tag, fault);
      for (int kind = 0; kind < 2; ++kind) ProcessImage(kind, img.data(), W * 4, W, H, false, Now(), v, true, GrabMode::measure);
    }
  }
  for (int kind = 0; kind < 2; ++kind) FinishVariant(g_k[kind], kind, g_k[kind].worstVar);
  WriteSummary(table, Variant());
  // Check the verdicts and the decoded tags.
  int bad = 0; FILE* f = fopen((g_out + "\\summary.csv").c_str(), "r"); if (!f) return 1;
  char line[512]; fgets(line, sizeof(line), f);
  while (fgets(line, sizeof(line), f)) {
    std::vector<std::string> col; std::string cur; for (char* p = line; *p && *p != '\n'; ++p) { if (*p == ',') { col.push_back(cur); cur.clear(); } else cur += *p; } col.push_back(cur);
    if (col.size() < 13) continue; int v = atoi(col[0].c_str()); std::string eye = col[3], verdict = col[12];
    std::string want = v == 0 ? "control" : (v == 1 && eye == "right" ? "FLICKER" : (v == 6 ? "DISC" : "clean"));
    if (verdict != want) { L("selftest: variant %d %s %s eye: %s, expected %s", v, col[2].c_str(), eye.c_str(), verdict.c_str(), want.c_str()); ++bad; }
  }
  fclose(f);
  int tagBad = 0; for (size_t i = 0; i < g_recs.size(); ++i) { int g = int(i / 2) % 36; if (((g_recs[i].tag >> 3) & 1) != (g & 1)) ++tagBad; }
  if (tagBad) { L("selftest: %d grabs with a wrong parity bit in the decoded tag", tagBad); ++bad; }
  L("selftest: %s (%d records)", bad ? "FAIL" : "PASS", int(g_recs.size()));
  return bad ? 1 : 0;
}

int main(int argc, char** argv) {
  if (argc < 2) { printf("usage: fovreplay <outdir> [--seconds S] [--loops N] [--settle S] [--track-wait S] [--only a,b] [--runtime <dll>] [--allow-untracked]\n"); return 2; }
  g_out = argv[1]; CreateDirectoryA(g_out.c_str(), nullptr);
  std::string rtPath = "C:\\Program Files\\Pimax\\Runtime\\PiOpenXR_64.dll", only; double seconds = 15, settle = 2.5; int loops = 1, trackWait = 240; bool allowUntracked = false, submitOnly = false;
  for (int i = 2; i < argc; ++i) {
    auto next = [&]() { return i + 1 < argc ? argv[++i] : ""; };
    if (!strcmp(argv[i], "--runtime")) rtPath = next(); else if (!strcmp(argv[i], "--seconds")) seconds = atof(next()); else if (!strcmp(argv[i], "--loops")) loops = atoi(next());
    else if (!strcmp(argv[i], "--settle")) settle = atof(next()); else if (!strcmp(argv[i], "--track-wait")) trackWait = atoi(next()); else if (!strcmp(argv[i], "--only")) only = next();
    else if (!strcmp(argv[i], "--allow-untracked")) allowUntracked = true; else if (!strcmp(argv[i], "--beep")) g_beep = true; else if (!strcmp(argv[i], "--submit-only")) submitOnly = true;
  }
  g_log = fopen((g_out + "\\replay.log").c_str(), "w"); if (!g_log) return 2;
  for (int i = 2; i < argc; ++i) if (!strcmp(argv[i], "--selftest")) { CoInitializeEx(nullptr, COINIT_MULTITHREADED); int r = SelfTest(); printf("selftest %s\n", r ? "FAIL" : "PASS"); return r; }
  g_sub = fopen((g_out + "\\submissions.csv").c_str(), "w"); if (g_sub) fprintf(g_sub, "t,variant,frame,generated,set,tag,layers,focus_flags,display_time\n");
  CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  InstallPvrHook();
  std::string dir = rtPath.substr(0, rtPath.find_last_of("\\/")); SetDllDirectoryA(dir.c_str());
  HMODULE rt = LoadLibraryExA(rtPath.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH); if (!rt) { L("cannot load runtime %s", rtPath.c_str()); return 2; }
  auto nego = reinterpret_cast<XrResult(XRAPI_CALL*)(const XrNegotiateLoaderInfo*, XrNegotiateRuntimeRequest*)>(GetProcAddress(rt, "xrNegotiateLoaderRuntimeInterface"));
  XrNegotiateLoaderInfo li{XR_LOADER_INTERFACE_STRUCT_LOADER_INFO, XR_LOADER_INFO_STRUCT_VERSION, sizeof(XrNegotiateLoaderInfo), 1, XR_CURRENT_LOADER_RUNTIME_VERSION, XR_MAKE_VERSION(1, 0, 0), XR_MAKE_VERSION(1, 1, 0xfff)};
  XrNegotiateRuntimeRequest rr{XR_LOADER_INTERFACE_STRUCT_RUNTIME_REQUEST, XR_RUNTIME_INFO_STRUCT_VERSION, sizeof(XrNegotiateRuntimeRequest)};
  if (!nego || XR_FAILED(nego(&li, &rr)) || !rr.getInstanceProcAddr) { L("runtime negotiation failed"); return 2; }
  g_gipa = rr.getInstanceProcAddr;
  const char* exts[] = {"XR_KHR_D3D11_enable"};
  XrInstanceCreateInfo ici{XR_TYPE_INSTANCE_CREATE_INFO}; strcpy(ici.applicationInfo.applicationName, "dcsvr_fovreplay"); ici.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
  ici.enabledExtensionCount = 1; ici.enabledExtensionNames = exts;
  if (XR_FAILED(fn<PFN_xrCreateInstance>("xrCreateInstance")(&ici, &g_inst))) { L("xrCreateInstance failed"); return 2; }
  xrPollEvent_ = fn<PFN_xrPollEvent>("xrPollEvent"); xrBeginSession_ = fn<PFN_xrBeginSession>("xrBeginSession"); xrEndSession_ = fn<PFN_xrEndSession>("xrEndSession");
  xrWaitFrame_ = fn<PFN_xrWaitFrame>("xrWaitFrame"); xrBeginFrame_ = fn<PFN_xrBeginFrame>("xrBeginFrame"); xrEndFrame_ = fn<PFN_xrEndFrame>("xrEndFrame"); xrLocateViews_ = fn<PFN_xrLocateViews>("xrLocateViews");
  xrAcquire = fn<PFN_xrAcquireSwapchainImage>("xrAcquireSwapchainImage"); xrWaitImg = fn<PFN_xrWaitSwapchainImage>("xrWaitSwapchainImage"); xrRelease = fn<PFN_xrReleaseSwapchainImage>("xrReleaseSwapchainImage");
  xrCreateSwapchain_ = fn<PFN_xrCreateSwapchain>("xrCreateSwapchain"); xrDestroySwapchain_ = fn<PFN_xrDestroySwapchain>("xrDestroySwapchain"); xrEnumImages = fn<PFN_xrEnumerateSwapchainImages>("xrEnumerateSwapchainImages");
  XrSystemGetInfo sgi{XR_TYPE_SYSTEM_GET_INFO}; sgi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY; XrSystemId sys = 0;
  if (XR_FAILED(fn<PFN_xrGetSystem>("xrGetSystem")(g_inst, &sgi, &sys))) { L("xrGetSystem failed: headset not available (yet)"); return 5; }
  XrGraphicsRequirementsD3D11KHR req{XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR}; fn<PFN_xrGetD3D11GraphicsRequirementsKHR>("xrGetD3D11GraphicsRequirementsKHR")(g_inst, sys, &req); g_luid = req.adapterLuid;
  IDXGIFactory1* fac = nullptr; CreateDXGIFactory1(IID_PPV_ARGS(&fac)); IDXGIAdapter1* ad = nullptr;
  for (UINT i = 0; fac->EnumAdapters1(i, &ad) != DXGI_ERROR_NOT_FOUND; ++i) { DXGI_ADAPTER_DESC1 d; ad->GetDesc1(&d); if (!memcmp(&d.AdapterLuid, &req.adapterLuid, sizeof(LUID))) break; ad->Release(); ad = nullptr; }
  D3D_FEATURE_LEVEL fl[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
  if (FAILED(D3D11CreateDevice(ad, ad ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, fl, 2, D3D11_SDK_VERSION, &dev, nullptr, &ctx))) { L("D3D11CreateDevice failed"); return 2; }
  XrGraphicsBindingD3D11KHR gb{XR_TYPE_GRAPHICS_BINDING_D3D11_KHR}; gb.device = dev;
  XrSessionCreateInfo sci{XR_TYPE_SESSION_CREATE_INFO}; sci.next = &gb; sci.systemId = sys;
  if (XR_FAILED(fn<PFN_xrCreateSession>("xrCreateSession")(g_inst, &sci, &ses))) { L("xrCreateSession failed: start again in a few seconds"); return 5; }
  XrReferenceSpaceCreateInfo rsi{XR_TYPE_REFERENCE_SPACE_CREATE_INFO}; rsi.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL; rsi.poseInReferenceSpace.orientation.w = 1;
  fn<PFN_xrCreateReferenceSpace>("xrCreateReferenceSpace")(ses, &rsi, &space);
  rsi.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW; fn<PFN_xrCreateReferenceSpace>("xrCreateReferenceSpace")(ses, &rsi, &viewSpace);

  // Images: a bright sky in both layers (the colour a white disc shows on in flight), the focus with Quad Views' fade.
  const uint8_t SR = 150, SG = 190, SB = 235;
  { Image sky(PW, PH); for (size_t i = 0; i < sky.px.size(); i += 4) { sky.px[i] = SR; sky.px[i + 1] = SG; sky.px[i + 2] = SB; sky.px[i + 3] = 255; } texSky = MakeTex(sky); }
  for (int mode = 0; mode < 3; ++mode) {
    // 0 unpremultiplied (colour, black where alpha is 0), 1 premultiplied on the encoded bytes (modes 1 and 2), 2 opaque.
    Image f(FW, FH);
    for (int y = 0; y < FH; ++y) { double ny = 1 - 2 * (y + 0.5) / FH; for (int x = 0; x < FW; ++x) { double nx = 2 * (x + 0.5) / FW - 1, a = mode == 2 ? 1.0 : FocusAlpha(nx, ny); uint8_t* o = f.at(x, y);
      double k = mode == 1 ? a : (a > 0 ? 1.0 : 0.0); o[0] = uint8_t(SR * k + 0.5); o[1] = uint8_t(SG * k + 0.5); o[2] = uint8_t(SB * k + 0.5); o[3] = uint8_t(a * 255 + 0.5); } }
    texFoc[mode] = MakeTex(f);
  }
  { Image f(FW, FH); for (int y = 0; y < FH; ++y) { double ny = 1 - 2 * (y + 0.5) / FH; for (int x = 0; x < FW; ++x) { double nx = 2 * (x + 0.5) / FW - 1, a = FocusAlpha(nx, ny); uint8_t* o = f.at(x, y); o[0] = o[1] = o[2] = a > 0 ? 20 : 0; o[3] = uint8_t(a * 255 + 0.5); } } texLocate = MakeTex(f); }
  { Image b(64, 64), w(64, 64); for (size_t i = 0; i < b.px.size(); i += 4) { b.px[i] = b.px[i + 1] = b.px[i + 2] = 0; b.px[i + 3] = 255; w.px[i] = w.px[i + 1] = w.px[i + 2] = 255; w.px[i + 3] = 255; } texBlack = MakeTex(b); texWhite = MakeTex(w); }
  { Image q(QW, QH); for (size_t i = 0; i < q.px.size(); i += 4) { q.px[i] = q.px[i + 1] = q.px[i + 2] = 12; q.px[i + 3] = 160; } texQuad = MakeTex(q); }

  // Session running, FOV known, headset tracked (the compositor shows no application until it tracks).
  int exitCode = 0;
  for (int i = 0; i < 300 && !g_running && !g_quit; ++i) { PollEvents(); Sleep(20); }
  { Variant cal; cal.array = true; cal.sets = 1; g_var = cal; if (!EnsurePools(cal)) exitCode = 2; }
  g_scene = Scene::base;
  int waited = 0, good = 0;
  while (exitCode == 0 && !g_quit && waited < 90 * (allowUntracked ? 15 : trackWait)) { if (!Frames(90)) break; waited += 90; bool shown = g_state == XR_SESSION_STATE_FOCUSED || (g_state == XR_SESSION_STATE_VISIBLE && waited >= 90 * 60); bool ok = g_haveFov && Tracked() && shown; good = ok ? good + 1 : 0; if (good >= 3) break; }
  L("tracked %d state %d after %d frames", Tracked(), g_state, waited);
  if (exitCode == 0) { if (g_quit || !g_running) exitCode = 2; else if ((!Tracked() && !allowUntracked) || (g_state != XR_SESSION_STATE_FOCUSED && g_state != XR_SESSION_STATE_VISIBLE)) exitCode = 3; }
  int mirrorBefore = PvrGetInt("enable_undistorted_mirror", -1), screenBefore = PvrGetInt("enable_screen_mirror", -1);
  std::thread grabber; bool mirrorsSet = false;
  if (exitCode == 0 && !submitOnly) {
    mirrorsSet = true;
    L("mirror keys before: %d %d; set -> %d %d", mirrorBefore, screenBefore, PvrSetInt("enable_undistorted_mirror", 1), PvrSetInt("enable_screen_mirror", 1));
    Frames(60);
    grabber = std::thread(GrabThread);
    exitCode = Calibrate();
  }
  std::vector<Variant> table = Table(); Variant custom;
  if (exitCode == 0) {
    std::vector<int> order; for (size_t i = 0; i < table.size(); ++i) if (only.empty() || ("," + only + ",").find("," + table[i].name + ",") != std::string::npos || ("," + only + ",").find("," + std::to_string(i) + ",") != std::string::npos) order.push_back(int(i));
    if (order.empty() || order[0] != 0) order.insert(order.begin(), 0); // the control first, always
    int pos = 0, loop = 0; bool quit = false; double started = 0; int current = -3;
    g_scene = Scene::measure; g_grabMode = GrabMode::measure;
    if (g_beep) { Beep(880, 300); Beep(1175, 300); }
    while (!g_quit && !quit) {
      int want = -1, cmd = ApplyCommands(table, custom, quit);
      if (cmd == -2) want = 99; else if (cmd >= 0) want = cmd;
      if (want < 0 && g_cycle) { if (current == -3 || Now() - started >= seconds) { if (current != -3) { if (++pos >= int(order.size())) { pos = 0; if (++loop >= loops) break; } } want = order[pos]; } }
      if (want >= 0 && (want != current || cmd != -1)) {
        g_settled = false; Variant v = want == 99 ? custom : table[want];
        if (!EnsurePools(v)) { exitCode = 2; break; }
        g_var = v; current = want; g_varId = want; g_measVar = want; started = Now(); g_realNo = g_genNo = 0;
        { static int f0 = 0; static double t0 = 0; double now = Now(); if (t0 > 0) L("previous variant: %.1f fps", (g_frameNo - f0) / (now - t0)); f0 = g_frameNo; t0 = now; }
        L("variant %d %s: %s (end-frame failures so far %d)", want, v.name.c_str(), Describe(v).c_str(), g_endFailures);
      }
      if (!g_settled && Now() - started >= settle) g_settled = true;
      if (!Frames(9)) { exitCode = 2; break; }
    }
    if (g_beep) { Beep(1175, 300); Beep(880, 300); Beep(660, 500); }
    L("measurement done: %d grabs (%d failed), %d end-frame failures, %d frames", g_grabs.load(), g_grabFail.load(), g_endFailures, g_frameNo);
  }
  g_grabMode = GrabMode::stop; if (grabber.joinable()) grabber.join();
  if (exitCode == 0 && !submitOnly) WriteSummary(table, custom);
  if (mirrorsSet) { PvrSetInt("enable_undistorted_mirror", mirrorBefore > 0 ? 1 : 0); PvrSetInt("enable_screen_mirror", screenBefore > 0 ? 1 : 0);
    L("mirror keys after: %d %d", PvrGetInt("enable_undistorted_mirror", -1), PvrGetInt("enable_screen_mirror", -1)); }
  if (g_sub) fclose(g_sub);
  if (g_running) { fn<PFN_xrRequestExitSession>("xrRequestExitSession")(ses); for (int i = 0; i < 100 && g_running; ++i) Frame(); }
  fn<PFN_xrDestroySession>("xrDestroySession")(ses); fn<PFN_xrDestroyInstance>("xrDestroyInstance")(g_inst);
  L("exit %d", exitCode);
  return exitCode;
}
