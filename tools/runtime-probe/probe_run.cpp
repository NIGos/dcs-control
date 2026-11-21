// Runtime probe, capture side. Submits test images as one or two projection layers to an OpenXR runtime (talking to the
// runtime DLL directly: no loader, no API layers), reads the Pimax compositor's mirrors and writes one PNG + one
// key=value file per case. The analysis is probe_analyze.cpp. D3D11 binding, or D3D12 with --d3d12 (what the runtime
// sees behind the frame-generation layer). The mirrors are always read through a D3D11 device.
//   probe_run <outdir> [--runtime <dll>] [--quick] [--d3d12]
// Chain mode (CHAIN.md): the probe plays the game and the loader. It loads OpenXR API layers itself, chains them in
// front of the runtime and submits ONE projection layer with FOUR views (XR_VARJO_quad_views), so the Quad Views layer
// (and the frame-generation layer under it) do the composition and the mirrors show their result.
//   probe_run <outdir> --quad --layer <manifest.json|dll> [--layer ...] [--quick] [--format 28|29]
//             [--motion | --motion-only] [--motion-phases M] [--motion-frames N] [--motion-grabs G] [--motion-warmup F]
//             [--motion-pattern lattice|scene] [--motion-diag] [--motion-seed S] [--motion-fg 1|2|3]
// Exit codes: 0 done, 2 runtime or session failure, 3 headset not tracked (the compositor shows no app then), 4 no mirror,
// 5 the runtime refused the session (start the probe again).
#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_D3D11
#define XR_USE_GRAPHICS_API_D3D12
#include "common.h"
#include "scene_pattern.h"
#include <d3d11_1.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <openxr/openxr_loader_negotiation.h>
#include <algorithm>
#include <array>
#include <functional>

static FILE* g_log = nullptr;
static void L(const char* fmt, ...) {
  va_list a; va_start(a, fmt); SYSTEMTIME t; GetLocalTime(&t);
  fprintf(g_log, "%02d:%02d:%02d.%03d ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds); vfprintf(g_log, fmt, a); fputc('\n', g_log); fflush(g_log);
  va_end(a);
}
static std::string NowStamp() { SYSTEMTIME t; GetLocalTime(&t); char b[64]; snprintf(b, sizeof(b), "%02d-%02d-%02d %02d:%02d:%02d.%03d", t.wYear % 100, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, t.wMilliseconds); return b; }

// ---- Pimax PVR client interface, in this process only: the runtime's own HMD handle, config keys, shared mirrors ----
struct PvrIface { void* slot[80]; };
static void* g_hmd = nullptr; static PvrIface* g_pvr = nullptr;
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
static int PvrGetInt(const char* k, int def) { return g_pvr && g_hmd ? ((int (*)(void*, const char*, int))g_pvr->slot[18])(g_hmd, k, def) : def; }
static int PvrSetInt(const char* k, int v) { return g_pvr && g_hmd ? ((int (*)(void*, const char*, int))g_pvr->slot[19])(g_hmd, k, v) : -1; }
static int64_t PvrGetInt64(const char* k, int64_t def) { return g_pvr && g_hmd ? ((int64_t (*)(void*, const char*, int64_t))g_pvr->slot[60])(g_hmd, k, def) : def; }

// ---- OpenXR, straight from the runtime DLL ----
static PFN_xrGetInstanceProcAddr g_gipa = nullptr; static XrInstance g_inst = XR_NULL_HANDLE;
template <typename T> static T fn(const char* name) { PFN_xrVoidFunction f = nullptr; g_gipa(g_inst, name, &f); if (!f) L("missing %s", name); return reinterpret_cast<T>(f); }
static PFN_xrPollEvent xrPollEvent_; static PFN_xrBeginSession xrBeginSession_; static PFN_xrEndSession xrEndSession_;
static PFN_xrWaitFrame xrWaitFrame_; static PFN_xrBeginFrame xrBeginFrame_; static PFN_xrEndFrame xrEndFrame_; static PFN_xrLocateViews xrLocateViews_;
static PFN_xrAcquireSwapchainImage xrAcquire; static PFN_xrWaitSwapchainImage xrWaitImg; static PFN_xrReleaseSwapchainImage xrRelease;
static PFN_xrCreateSwapchain xrCreateSwapchain_; static PFN_xrDestroySwapchain xrDestroySwapchain_; static PFN_xrEnumerateSwapchainImages xrEnumImages;

static ID3D11Device* dev = nullptr; static ID3D11DeviceContext* ctx = nullptr; static IDXGIAdapter3* adapter3 = nullptr;
static XrSession ses = XR_NULL_HANDLE; static XrSpace space = XR_NULL_HANDLE;
static XrSessionState g_state = XR_SESSION_STATE_UNKNOWN; static bool g_running = false, g_quit = false;

// D3D12 side (only with --d3d12): one queue shared with the runtime, one list recorded per frame.
static bool g_d3d12 = false; static ID3D12Device* dev12 = nullptr; static ID3D12CommandQueue* queue12 = nullptr; static ID3D12CommandAllocator* alloc12 = nullptr;
static ID3D12GraphicsCommandList* list12 = nullptr; static ID3D12Fence* fence12 = nullptr; static UINT64 g_fenceValue = 0; static HANDLE g_fenceEvent = nullptr; static bool g_listOpen = false;
static void EnsureList() { if (!g_listOpen) { alloc12->Reset(); list12->Reset(alloc12, nullptr); g_listOpen = true; } }
static void Flush12() {
  if (!g_listOpen) return;
  list12->Close(); ID3D12CommandList* l[] = {list12}; queue12->ExecuteCommandLists(1, l); g_listOpen = false;
  queue12->Signal(fence12, ++g_fenceValue);
  if (fence12->GetCompletedValue() < g_fenceValue) { fence12->SetEventOnCompletion(g_fenceValue, g_fenceEvent); WaitForSingleObject(g_fenceEvent, 5000); }
}
static void Barrier(ID3D12Resource* r, UINT sub, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b) {
  D3D12_RESOURCE_BARRIER bar{}; bar.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; bar.Transition.pResource = r; bar.Transition.Subresource = sub; bar.Transition.StateBefore = a; bar.Transition.StateAfter = b;
  list12->ResourceBarrier(1, &bar);
}

struct Chain { XrSwapchain h = XR_NULL_HANDLE; std::vector<XrSwapchainImageD3D11KHR> img; std::vector<XrSwapchainImageD3D12KHR> img12; int w = 0, hgt = 0, slices = 1; uint32_t cur = 0; };
static bool MakeChain(Chain& c, int w, int h, int slices, int64_t fmt) {
  XrSwapchainCreateInfo ci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
  ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
  ci.format = fmt; ci.sampleCount = 1; ci.width = w; ci.height = h; ci.faceCount = 1; ci.arraySize = slices; ci.mipCount = 1;
  XrResult r = xrCreateSwapchain_(ses, &ci, &c.h); if (XR_FAILED(r)) { L("xrCreateSwapchain %dx%d x%d fmt %lld failed %d", w, h, slices, (long long)fmt, r); return false; }
  uint32_t k = 0; xrEnumImages(c.h, 0, &k, nullptr); c.img.assign(k, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR}); c.img12.assign(k, {XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR});
  if (g_d3d12) xrEnumImages(c.h, k, &k, reinterpret_cast<XrSwapchainImageBaseHeader*>(c.img12.data()));
  else xrEnumImages(c.h, k, &k, reinterpret_cast<XrSwapchainImageBaseHeader*>(c.img.data()));
  c.w = w; c.hgt = h; c.slices = slices; return true;
}
static void FreeChain(Chain& c) { if (c.h) xrDestroySwapchain_(c.h); c = Chain(); }

struct Source { ID3D11Texture2D* tex = nullptr; ID3D12Resource* res = nullptr; int w = 0, h = 0; };
static void SetSource12(Source& s, const Image& im) {
  D3D12_RESOURCE_DESC td{}; td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; td.Width = im.w; td.Height = im.h; td.DepthOrArraySize = 1; td.MipLevels = 1; td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.SampleDesc.Count = 1;
  bool fresh = !s.res || s.w != im.w || s.h != im.h;
  if (fresh) { if (s.res) s.res->Release(); D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    dev12->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&s.res)); s.w = im.w; s.h = im.h; }
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{}; UINT rows = 0; UINT64 rowBytes = 0, total = 0; dev12->GetCopyableFootprints(&td, 0, 1, 0, &fp, &rows, &rowBytes, &total);
  D3D12_HEAP_PROPERTIES up{}; up.Type = D3D12_HEAP_TYPE_UPLOAD; D3D12_RESOURCE_DESC bd{}; bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; bd.Width = total; bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1; bd.SampleDesc.Count = 1; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  ID3D12Resource* upload = nullptr; if (FAILED(dev12->CreateCommittedResource(&up, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&upload)))) { L("upload buffer failed"); return; }
  void* p = nullptr; upload->Map(0, nullptr, &p);
  for (int y = 0; y < im.h; ++y) memcpy((BYTE*)p + fp.Offset + size_t(y) * fp.Footprint.RowPitch, im.at(0, y), size_t(im.w) * 4);
  upload->Unmap(0, nullptr);
  EnsureList(); if (!fresh) Barrier(s.res, 0, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
  D3D12_TEXTURE_COPY_LOCATION dst{}; dst.pResource = s.res; dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; D3D12_TEXTURE_COPY_LOCATION src{}; src.pResource = upload; src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; src.PlacedFootprint = fp;
  list12->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr); Barrier(s.res, 0, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
  Flush12(); upload->Release();
}
static void SetSource(Source& s, const Image& im) {
  if (g_d3d12) { SetSource12(s, im); return; }
  if (!s.tex || s.w != im.w || s.h != im.h) {
    if (s.tex) s.tex->Release();
    D3D11_TEXTURE2D_DESC td{}; td.Width = im.w; td.Height = im.h; td.MipLevels = 1; td.ArraySize = 1; td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    dev->CreateTexture2D(&td, nullptr, &s.tex); s.w = im.w; s.h = im.h;
  }
  ctx->UpdateSubresource(s.tex, 0, nullptr, im.px.data(), im.w * 4, 0);
}

static XrQuaternionf Mul(XrQuaternionf a, XrQuaternionf b) {
  return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
          a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}
static XrQuaternionf Rot(float yawDeg, float pitchDeg) {
  float hy = yawDeg * 3.14159265f / 360.f, hp = pitchDeg * 3.14159265f / 360.f;
  return Mul(XrQuaternionf{0, sinf(hy), 0, cosf(hy)}, XrQuaternionf{sinf(hp), 0, 0, cosf(hp)});
}

// What is being submitted right now.
struct Scene {
  int layers = 1;          // 1 = layer A only, 2 = A + B, 0 = the composed reference alone
  uint64_t flagsB = 0;
  float yawA = 0, yawB = 0, pitchA = 0, pitchB = 0;
  Fov fovA[2], fovB[2];    // submitted FOVs (tangents)
};
static Scene g_scene; static Chain chA[2], chB[2], chR[2]; static bool g_array = false; static Source srcA[2], srcB[2], srcR[2];
static Fov g_eyeFov[2]; static bool g_haveFov = false; static uint64_t g_viewFlags = 0; static XrPosef g_eyePose[2];
static int g_frames = 0, g_endFailures = 0; static double g_lastFps = 0;

// Chain mode: the view configuration the session runs with, and the frame function of the four-view submission.
static XrViewConfigurationType g_viewConfig = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
static bool g_quad = false; static bool FrameQuad();
static XrFovf ToXr(const Fov& f) { return {float(atan(f.l)), float(atan(f.r)), float(atan(f.u)), float(atan(f.d))}; }

static void PollEvents() {
  XrEventDataBuffer ev{XR_TYPE_EVENT_DATA_BUFFER};
  while (xrPollEvent_(g_inst, &ev) == XR_SUCCESS) {
    if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
      g_state = reinterpret_cast<XrEventDataSessionStateChanged*>(&ev)->state; L("session state %d", g_state);
      if (g_state == XR_SESSION_STATE_READY) { XrSessionBeginInfo bi{XR_TYPE_SESSION_BEGIN_INFO}; bi.primaryViewConfigurationType = g_viewConfig; XrResult br = xrBeginSession_(ses, &bi); if (XR_SUCCEEDED(br)) g_running = true; else L("xrBeginSession failed %d", br); }
      else if (g_state == XR_SESSION_STATE_STOPPING) { xrEndSession_(ses); g_running = false; }
      else if (g_state == XR_SESSION_STATE_EXITING || g_state == XR_SESSION_STATE_LOSS_PENDING) g_quit = true;
    }
    ev = {XR_TYPE_EVENT_DATA_BUFFER};
  }
}
static std::vector<Chain*> g_pendingRelease;
static void CopyTo(Chain& c, int slice, Source& s) {
  // An array swapchain is acquired at its first slice and released after its last one.
  if (slice == 0 || c.slices == 1) {
    XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO}; wi.timeout = XR_INFINITE_DURATION;
    xrAcquire(c.h, nullptr, &c.cur); xrWaitImg(c.h, &wi);
  }
  UINT sub = c.slices == 1 ? 0 : slice;
  if (g_d3d12) {
    if (s.res) { // OpenXR hands D3D12 colour images over in the render-target state and wants them back in it.
      EnsureList(); ID3D12Resource* img = c.img12[c.cur].texture;
      Barrier(img, sub, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_DEST);
      D3D12_TEXTURE_COPY_LOCATION dst{}; dst.pResource = img; dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; dst.SubresourceIndex = sub;
      D3D12_TEXTURE_COPY_LOCATION src{}; src.pResource = s.res; src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
      list12->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
      Barrier(img, sub, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_RENDER_TARGET);
    }
  } else if (s.tex) ctx->CopySubresourceRegion(c.img[c.cur].texture, sub, 0, 0, 0, s.tex, 0, nullptr);
  if (slice == c.slices - 1 || c.slices == 1) g_pendingRelease.push_back(&c);
}
// Submits the frame's copies, then gives the images back to the runtime.
static void FlushCopies() {
  if (g_d3d12) Flush12(); else ctx->Flush();
  for (Chain* c : g_pendingRelease) xrRelease(c->h, nullptr);
  g_pendingRelease.clear();
}
// One frame of the current scene. Returns false when the session is gone.
static bool Frame() {
  if (g_quad) return FrameQuad();
  PollEvents(); if (g_quit) return false;
  if (!g_running) { Sleep(20); return true; }
  XrFrameState fst{XR_TYPE_FRAME_STATE}; XrFrameWaitInfo fwi{XR_TYPE_FRAME_WAIT_INFO};
  if (XR_FAILED(xrWaitFrame_(ses, &fwi, &fst))) return false;
  XrFrameBeginInfo fbi{XR_TYPE_FRAME_BEGIN_INFO}; if (XR_FAILED(xrBeginFrame_(ses, &fbi))) return false;
  XrCompositionLayerProjectionView va[2], vb[2];
  XrCompositionLayerProjection la{XR_TYPE_COMPOSITION_LAYER_PROJECTION}, lb{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
  const XrCompositionLayerBaseHeader* layers[2]; uint32_t nl = 0;
  if (fst.shouldRender) {
    XrViewLocateInfo vli{XR_TYPE_VIEW_LOCATE_INFO}; vli.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO; vli.displayTime = fst.predictedDisplayTime; vli.space = space;
    XrViewState vs{XR_TYPE_VIEW_STATE}; XrView views[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}}; uint32_t vc = 0;
    if (XR_SUCCEEDED(xrLocateViews_(ses, &vli, &vs, 2, &vc, views))) {
      g_viewFlags = vs.viewStateFlags;
      for (int e = 0; e < 2; ++e) {
        g_eyePose[e] = views[e].pose;
        if (!g_haveFov && views[e].fov.angleRight > views[e].fov.angleLeft) g_eyeFov[e] = {tan(views[e].fov.angleLeft), tan(views[e].fov.angleRight), tan(views[e].fov.angleUp), tan(views[e].fov.angleDown)};
      }
      if (g_eyeFov[0].r > g_eyeFov[0].l && g_eyeFov[1].r > g_eyeFov[1].l) g_haveFov = true;
      const Scene& sc = g_scene; bool ref = sc.layers == 0, two = sc.layers == 2;
      for (int e = 0; e < 2; ++e) {
        Chain& ca = ref ? chR[e] : (g_array ? chA[0] : chA[e]);
        Source& sa = ref ? srcR[e] : srcA[e];
        if (ca.h) CopyTo(ca, (ref || !g_array) ? 0 : e, sa);
        va[e] = {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}; va[e].pose = views[e].pose; va[e].pose.orientation = Mul(views[e].pose.orientation, Rot(sc.yawA, sc.pitchA));
        va[e].fov = ToXr(sc.fovA[e]); va[e].subImage.swapchain = ca.h; va[e].subImage.imageRect = {{0, 0}, {ca.w, ca.hgt}};
        va[e].subImage.imageArrayIndex = (ref || !g_array) ? 0 : e;
        if (two) {
          Chain& cb = g_array ? chB[0] : chB[e];
          CopyTo(cb, g_array ? e : 0, srcB[e]);
          vb[e] = {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}; vb[e].pose = views[e].pose; vb[e].pose.orientation = Mul(views[e].pose.orientation, Rot(sc.yawB, sc.pitchB));
          vb[e].fov = ToXr(sc.fovB[e]); vb[e].subImage.swapchain = cb.h; vb[e].subImage.imageRect = {{0, 0}, {cb.w, cb.hgt}};
          vb[e].subImage.imageArrayIndex = g_array ? e : 0;
        }
      }
      FlushCopies();
      la.space = space; la.viewCount = 2; la.views = va; layers[nl++] = reinterpret_cast<XrCompositionLayerBaseHeader*>(&la);
      if (two) { lb.space = space; lb.viewCount = 2; lb.views = vb; lb.layerFlags = sc.flagsB; layers[nl++] = reinterpret_cast<XrCompositionLayerBaseHeader*>(&lb); }
    }
  }
  XrFrameEndInfo fei{XR_TYPE_FRAME_END_INFO}; fei.displayTime = fst.predictedDisplayTime; fei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE; fei.layerCount = nl; fei.layers = layers;
  if (XR_FAILED(xrEndFrame_(ses, &fei))) ++g_endFailures;
  ++g_frames;
  static LARGE_INTEGER qf, q0; static int n0 = 0; LARGE_INTEGER q1; if (!qf.QuadPart) { QueryPerformanceFrequency(&qf); QueryPerformanceCounter(&q0); }
  QueryPerformanceCounter(&q1); double dt = double(q1.QuadPart - q0.QuadPart) / qf.QuadPart;
  if (dt >= 1.0) { g_lastFps = (g_frames - n0) / dt; n0 = g_frames; q0 = q1; }
  return true;
}
static bool Frames(int n) { for (int i = 0; i < n; ++i) if (!Frame()) return false; return true; }
static bool Tracked() { return (g_viewFlags & (XR_VIEW_STATE_ORIENTATION_TRACKED_BIT | XR_VIEW_STATE_POSITION_TRACKED_BIT)) == (XR_VIEW_STATE_ORIENTATION_TRACKED_BIT | XR_VIEW_STATE_POSITION_TRACKED_BIT); }

// ---- mirrors ----
static bool GrabMirrorOnce(const char* handleKey, Image& out) {
  int64_t h = PvrGetInt64(handleKey, 0); if (!h) return false;
  ID3D11Texture2D* tex = nullptr; if (FAILED(dev->OpenSharedResource((HANDLE)h, __uuidof(ID3D11Texture2D), (void**)&tex)) || !tex) return false;
  D3D11_TEXTURE2D_DESC d; tex->GetDesc(&d); D3D11_TEXTURE2D_DESC s = d; s.Usage = D3D11_USAGE_STAGING; s.BindFlags = 0; s.CPUAccessFlags = D3D11_CPU_ACCESS_READ; s.MiscFlags = 0;
  ID3D11Texture2D* st = nullptr; bool ok = false;
  if (SUCCEEDED(dev->CreateTexture2D(&s, nullptr, &st))) {
    ctx->CopyResource(st, tex); D3D11_MAPPED_SUBRESOURCE m;
    if (SUCCEEDED(ctx->Map(st, 0, D3D11_MAP_READ, 0, &m))) {
      bool bgra = d.Format == 87 || d.Format == 91 || d.Format == 90 || d.Format == 88; out = Image(d.Width, d.Height);
      for (UINT y = 0; y < d.Height; ++y) { const BYTE* p = (const BYTE*)m.pData + y * m.RowPitch; uint8_t* o = out.at(0, y);
        for (UINT x = 0; x < d.Width; ++x) { o[x * 4] = bgra ? p[x * 4 + 2] : p[x * 4]; o[x * 4 + 1] = p[x * 4 + 1]; o[x * 4 + 2] = bgra ? p[x * 4] : p[x * 4 + 2]; o[x * 4 + 3] = 255; } }
      ctx->Unmap(st, 0); ok = true;
    }
    st->Release();
  }
  tex->Release(); return ok;
}
// Two grabs some frames apart must agree: the mirror is written at half rate into a ring of two buffers.
static bool GrabMirror(bool screen, Image& out, double* unstable) {
  const char* key = screen ? "screen_mirror_handle" : "undistorted_mirror_handle_0";
  for (int attempt = 0; attempt < 4; ++attempt) {
    if (!Frames(30)) return false;
    Image a, b; if (!GrabMirrorOnce(key, a)) { Frames(20); continue; }
    if (!Frames(8)) return false;
    if (!GrabMirrorOnce(key, b) || a.px.size() != b.px.size()) continue;
    size_t diff = 0, n = a.px.size() / 4;
    for (size_t i = 0; i < n; i += 7) if (memcmp(&a.px[i * 4], &b.px[i * 4], 3)) ++diff;
    *unstable = double(diff) / double(n / 7);
    out = std::move(b);
    if (*unstable < 0.002) return true;
  }
  return !out.px.empty();
}

// ---- test images ----
static double TanX(const Fov& f, int w, double x) { return f.l + (x + 0.5) / w * (f.r - f.l); }
static double TanY(const Fov& f, int h, double y) { return f.u - (y + 0.5) / h * (f.u - f.d); }
// Dots on a tangent-space lattice; the dot at index (0,0) of an un-offset lattice is white (the anchor).
static Image Lattice(int w, int h, const Fov& f, double step, double radTan, double off, uint8_t r, uint8_t g, uint8_t b, bool transparentBg, int premult, bool anchor, double sx = 0, double sy = 0) {
  // sx, sy: the whole lattice (anchor included) translated by that tangent amount (the --motion cases).
  Image im(w, h); double ppt = w / (f.r - f.l), radPx = radTan * ppt;
  for (int y = 0; y < h; ++y) { double ty = TanY(f, h, y) - sy; uint8_t* o = im.at(0, y);
    for (int x = 0; x < w; ++x) {
      double tx = TanX(f, w, x) - sx, ix = floor(tx / step - off + 0.5), iy = floor(ty / step - off + 0.5);
      double dx = tx - (ix + off) * step, dy = ty - (iy + off) * step, cov = radPx - sqrt(dx * dx + dy * dy) * ppt + 0.5;
      cov = cov < 0 ? 0 : (cov > 1 ? 1 : cov);
      bool white = anchor && ix == 0 && iy == 0;
      double cr = white ? 1 : r / 255.0, cg = white ? 1 : g / 255.0, cb = white ? 1 : b / 255.0;
      if (transparentBg) {
        if (premult == 1) { o[x * 4] = Enc(cr * cov); o[x * 4 + 1] = Enc(cg * cov); o[x * 4 + 2] = Enc(cb * cov); }
        else if (premult == 2) { o[x * 4] = uint8_t(Enc(cr) * cov + 0.5); o[x * 4 + 1] = uint8_t(Enc(cg) * cov + 0.5); o[x * 4 + 2] = uint8_t(Enc(cb) * cov + 0.5); }
        else { o[x * 4] = Enc(cr); o[x * 4 + 1] = Enc(cg); o[x * 4 + 2] = Enc(cb); }
        o[x * 4 + 3] = uint8_t(cov * 255 + 0.5);
      } else { o[x * 4] = Enc(cr * cov); o[x * 4 + 1] = Enc(cg * cov); o[x * 4 + 2] = Enc(cb * cov); o[x * 4 + 3] = 255; }
    } }
  return im;
}
static Image Flat(int w, int h, double level) { Image im(w, h); uint8_t v = Enc(level); for (size_t i = 0; i < im.px.size(); i += 4) { im.px[i] = im.px[i + 1] = im.px[i + 2] = v; im.px[i + 3] = 255; } return im; }
// Layer B at one grey level with the focus fade in alpha.
static Image Faded(int w, int h, double level, int premult) {
  Image im(w, h);
  for (int y = 0; y < h; ++y) { double ny = 1 - 2 * (y + 0.5) / h; uint8_t* o = im.at(0, y);
    for (int x = 0; x < w; ++x) { double nx = 2 * (x + 0.5) / w - 1, a = FocusAlpha(nx, ny); uint8_t v = premult == 1 ? Enc(level * a) : (premult == 2 ? uint8_t(Enc(level) * a + 0.5) : Enc(level));
      o[x * 4] = o[x * 4 + 1] = o[x * 4 + 2] = v; o[x * 4 + 3] = uint8_t(a * 255 + 0.5); } }
  return im;
}
// Sentinel patches in layer B (same bytes in every alpha variant; the analysis knows what each blend rule gives).
static Image Sentinel(int w, int h) {
  Image im(w, h);
  auto rect = [&](double cx, double cy, double hs, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    int x0 = int((cx - hs + 1) / 2 * w), x1 = int((cx + hs + 1) / 2 * w), y0 = int((1 - (cy + hs)) / 2 * h), y1 = int((1 - (cy - hs)) / 2 * h);
    for (int y = y0; y < y1; ++y) for (int x = x0; x < x1; ++x) { uint8_t* o = im.at(x, y); o[0] = r; o[1] = g; o[2] = b; o[3] = a; } };
  rect(0, 0, 0.9, 118, 118, 118, 255);     // opaque mid grey body
  rect(-0.5, 0.5, 0.18, 255, 0, 255, 0);   // P1: alpha 0, magenta colour
  rect(0.5, 0.5, 0.18, 128, 128, 128, 0);  // P2: alpha 0, grey colour
  rect(-0.5, -0.5, 0.18, 0, 0, 255, 255);  // P3: opaque blue
  rect(0.5, -0.5, 0.18, 255, 255, 0, 128); // P4: half-alpha yellow
  return im;
}
static Image Solid(int w, int h, uint8_t r, uint8_t g, uint8_t b) { Image im(w, h); for (size_t i = 0; i < im.px.size(); i += 4) { im.px[i] = r; im.px[i + 1] = g; im.px[i + 2] = b; im.px[i + 3] = 255; } return im; }
// A slanted edge (5 degrees from vertical) through tangent x0: black on the left, white on the right.
static Image Edge(int w, int h, const Fov& f, double x0Tan) {
  Image im(w, h); double ppt = w / (f.r - f.l), s = sin(5 * 3.14159265 / 180), c = cos(5 * 3.14159265 / 180);
  for (int y = 0; y < h; ++y) { uint8_t* o = im.at(0, y);
    for (int x = 0; x < w; ++x) { double px = (TanX(f, w, x) - x0Tan) * ppt, py = (y - h / 2.0), d = px * c + py * s, cov = d + 0.5; cov = cov < 0 ? 0 : (cov > 1 ? 1 : cov);
      uint8_t v = Enc(0.02 + 0.78 * cov); o[x * 4] = o[x * 4 + 1] = o[x * 4 + 2] = v; o[x * 4 + 3] = 255; } }
  return im;
}
// The composed reference: what Quad Views does today (bilinear periphery over the eye FOV, focus blended in linear light).
static Image Compose(int w, int h, const Image& A, const Fov& fa, const Image& B, const Fov& fb, bool fade, bool premult) {
  static double dec[256]; static uint8_t enc[4097]; static bool init = false;
  if (!init) { for (int i = 0; i < 256; ++i) dec[i] = SrgbToLinear(i / 255.0); for (int i = 0; i <= 4096; ++i) enc[i] = Enc(i / 4096.0); init = true; }
  auto sample = [&](const Image& im, double u, double v, double out[4]) {
    double x = u * im.w - 0.5, y = v * im.h - 0.5; int x0 = int(floor(x)), y0 = int(floor(y)); double fx = x - x0, fy = y - y0;
    auto cl = [](int v, int hi) { return v < 0 ? 0 : (v > hi ? hi : v); };
    const uint8_t* p00 = im.at(cl(x0, im.w - 1), cl(y0, im.h - 1)); const uint8_t* p10 = im.at(cl(x0 + 1, im.w - 1), cl(y0, im.h - 1));
    const uint8_t* p01 = im.at(cl(x0, im.w - 1), cl(y0 + 1, im.h - 1)); const uint8_t* p11 = im.at(cl(x0 + 1, im.w - 1), cl(y0 + 1, im.h - 1));
    for (int c = 0; c < 4; ++c) { auto v4 = [&](const uint8_t* p) { return c < 3 ? dec[p[c]] : p[c] / 255.0; };
      out[c] = (v4(p00) * (1 - fx) + v4(p10) * fx) * (1 - fy) + (v4(p01) * (1 - fx) + v4(p11) * fx) * fy; } };
  Image im(w, h);
  for (int y = 0; y < h; ++y) { double ty = TanY(fa, h, y), v = (y + 0.5) / h; uint8_t* o = im.at(0, y); double ny = (ty - (fb.u + fb.d) / 2) / ((fb.u - fb.d) / 2);
    for (int x = 0; x < w; ++x) {
      double tx = TanX(fa, w, x), u = (x + 0.5) / w, ca[4], col[3]; sample(A, u, v, ca);
      double nx = (tx - (fb.l + fb.r) / 2) / ((fb.r - fb.l) / 2);
      if (fabs(nx) < 1 && fabs(ny) < 1) {
        double cb[4]; sample(B, (nx + 1) / 2, (1 - ny) / 2, cb); double a = fade ? FocusAlpha(nx, ny) : cb[3];
        for (int c = 0; c < 3; ++c) { double bc = cb[c]; if (fade && premult && cb[3] > 1e-4) bc = cb[c] / cb[3]; col[c] = (fade ? bc * a : (premult ? bc : bc * a)) + ca[c] * (1 - a); }
      } else for (int c = 0; c < 3; ++c) col[c] = ca[c];
      for (int c = 0; c < 3; ++c) { double q = col[c] < 0 ? 0 : (col[c] > 1 ? 1 : col[c]); o[x * 4 + c] = enc[int(q * 4096 + 0.5)]; }
      o[x * 4 + 3] = 255;
    } }
  return im;
}

// The eye FOV scaled by k around its centre, and a focus box inside a FOV (Quad Views' section, clamped at the borders).
static Fov Scaled(const Fov& f, double k) { double cx = (f.l + f.r) / 2, cy = (f.u + f.d) / 2, hx = (f.r - f.l) / 2 * k, hy = (f.u - f.d) / 2 * k; return {cx - hx, cx + hx, cy + hy, cy - hy}; }
static Fov FocusBox(const Fov& f, double ox, double oy, double section) {
  double cx = (f.l + f.r) / 2 + ox * (f.r - f.l) / 2, cy = (f.u + f.d) / 2 + oy * (f.u - f.d) / 2, hx = section * (f.r - f.l) / 2, hy = section * (f.u - f.d) / 2;
  Fov b{cx - hx, cx + hx, cy + hy, cy - hy};
  if (b.l < f.l) b.l = f.l; if (b.r > f.r) b.r = f.r; if (b.u > f.u) b.u = f.u; if (b.d < f.d) b.d = f.d; return b;
}

// Alpha convention of layer B: 0 = unpremultiplied (flag set), 1 = premultiplied in linear light, 2 = premultiplied on
// the encoded values (what a compositor that blends in gamma space needs).
static std::string g_out; static int g_caseNo = 0; static int64_t g_fmt = 29; static int g_premult = 1;
static const int AW = 1040, AH = 1028, BW = 2028, BH = 2004, RW = 5424, RH = 5356;
// Image sizes written into the case: the constants above, or in the quad chain the sizes the layer recommends.
static int g_capAW = AW, g_capAH = AH, g_capBW = BW, g_capBH = BH;
static void QuadCaseFields(Case& c);
static void PutFov(Case& c, const char* name, const Fov f[2]) { for (int e = 0; e < 2; ++e) { std::string p = std::string(name) + std::to_string(e); c[p + "l"] = F(f[e].l); c[p + "r"] = F(f[e].r); c[p + "u"] = F(f[e].u); c[p + "d"] = F(f[e].d); } }
// Captures the current scene and writes the case.
static bool Capture(Case c, bool screen) {
  Image m; double unstable = 1; std::string begin = NowStamp();
  if (!GrabMirror(screen, m, &unstable)) { L("case %d: no mirror image", g_caseNo); return false; }
  char name[64]; snprintf(name, sizeof(name), "case%03d", g_caseNo++);
  c["id"] = name; c["mirror"] = screen ? "screen" : "undistorted"; c["unstable"] = F(unstable); c["time"] = begin; c["fps"] = F(g_lastFps);
  c["format"] = std::to_string(g_fmt); c["array"] = g_array ? "1" : "0"; c["premult"] = std::to_string(g_premult); c["layers"] = std::to_string(g_scene.layers);
  c["tracked"] = Tracked() ? "1" : "0"; c["state"] = std::to_string(g_state);
  c["yawA"] = F(g_scene.yawA); c["yawB"] = F(g_scene.yawB); c["pitchA"] = F(g_scene.pitchA); c["pitchB"] = F(g_scene.pitchB);
  PutFov(c, "eye", g_eyeFov); PutFov(c, "fovA", g_scene.fovA); PutFov(c, "fovB", g_scene.fovB);
  c["aw"] = std::to_string(g_capAW); c["ah"] = std::to_string(g_capAH); c["bw"] = std::to_string(g_capBW); c["bh"] = std::to_string(g_capBH);
  if (g_quad) QuadCaseFields(c);
  SavePng(Wide(g_out + "\\" + name + ".png"), m); SaveCase(g_out + "\\" + name + ".txt", c);
  L("%s kind %s mirror %s layers %d unstable %.4f", name, Str(c, "kind").c_str(), screen ? "screen" : "undist", g_scene.layers, unstable);
  return true;
}
static uint64_t BlendFlags() { return XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT | (g_premult ? 0 : XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT); }
static void SceneFovs(double k, double ox, double oy) { for (int e = 0; e < 2; ++e) { g_scene.fovA[e] = Scaled(g_eyeFov[e], k); g_scene.fovB[e] = FocusBox(g_scene.fovA[e], ox, oy, 0.34); } }
static void ResetPose() { g_scene.yawA = g_scene.yawB = g_scene.pitchA = g_scene.pitchB = 0; }

// Position: green dots in A, red dots in B on the same tangent lattice shifted by half a step.
static bool CaseLattice(double k, double ox, double oy, double step, bool screen, const char* tag) {
  SceneFovs(k, ox, oy); g_scene.layers = 2; g_scene.flagsB = BlendFlags();
  for (int e = 0; e < 2; ++e) {
    SetSource(srcA[e], Lattice(AW, AH, g_scene.fovA[e], step * k, 0.009 * k, 0, 0, 255, 0, false, 1, true));
    SetSource(srcB[e], Lattice(BW, BH, g_scene.fovB[e], step * k, 0.009 * k, 0.5, 255, 0, 0, true, g_premult, false));
  }
  Case c; c["kind"] = "lattice"; c["tag"] = tag; c["scale"] = F(k); c["ox"] = F(ox); c["oy"] = F(oy); c["step"] = F(step * k);
  return Capture(c, screen);
}
static void SetGrayScene(double la, double lb, bool reference) {
  SceneFovs(1, 0.1, 0.1);
  if (!reference) { g_scene.layers = 2; g_scene.flagsB = BlendFlags(); for (int e = 0; e < 2; ++e) { SetSource(srcA[e], Flat(AW, AH, la)); SetSource(srcB[e], Faded(BW, BH, lb, g_premult)); } }
  else { g_scene.layers = 0; for (int e = 0; e < 2; ++e) SetSource(srcR[e], Compose(RW, RH, Flat(AW, AH, la), g_scene.fovA[e], Faded(BW, BH, lb, 0), g_scene.fovB[e], true, false)); }
}
static bool CaseGray(double la, double lb, bool reference, const char* kind) {
  SetGrayScene(la, lb, reference);
  Case c; c["kind"] = kind; c["la"] = F(la); c["lb"] = F(lb); c["reference"] = reference ? "1" : "0";
  return Capture(c, false);
}
static bool CaseSentinel(bool blend) {
  SceneFovs(1, 0.1, 0.1); g_scene.layers = 2; g_scene.flagsB = blend ? BlendFlags() : 0;
  for (int e = 0; e < 2; ++e) { SetSource(srcA[e], Solid(AW, AH, 0, 200, 200)); SetSource(srcB[e], Sentinel(BW, BH)); }
  Case c; c["kind"] = "sentinel"; c["blend"] = blend ? "1" : "0"; return Capture(c, false);
}
// Sharpness on a scale model: FOVs enlarged by k so that the mirror samples the layers the way the panel would.
static bool CaseEdge(double k, bool reference, bool screen) {
  SceneFovs(k, 0.25, 0.0);
  for (int e = 0; e < 2; ++e) {
    const Fov& ey = g_eyeFov[e]; double c0 = (ey.l + ey.r) / 2, hw = (ey.r - ey.l) / 2;
    Image a = Edge(AW, AH, g_scene.fovA[e], c0 - 0.80 * hw), b = Edge(BW, BH, g_scene.fovB[e], c0 + 0.30 * hw);
    if (reference) SetSource(srcR[e], Compose(RW, RH, a, g_scene.fovA[e], b, g_scene.fovB[e], false, false)); else { SetSource(srcA[e], a); SetSource(srcB[e], b); }
  }
  g_scene.layers = reference ? 0 : 2; g_scene.flagsB = 0;
  Case c; c["kind"] = "edge"; c["scale"] = F(k); c["reference"] = reference ? "1" : "0"; return Capture(c, screen);
}
static bool CaseReproj(float yawA, float yawB, float pitch, const char* tag) {
  ResetPose(); g_scene.yawA = yawA; g_scene.yawB = yawB; g_scene.pitchA = g_scene.pitchB = pitch;
  bool ok = CaseLattice(1, 0.1, 0.1, 0.12, true, tag); ResetPose(); return ok;
}

// ================= chain mode =================
// ---- the loader's side of the API-layer protocol, by hand ----
struct LayerMod { std::string path, name; PFN_xrGetInstanceProcAddr gipa = nullptr; PFN_xrCreateApiLayerInstance create = nullptr; };
static std::vector<LayerMod> g_layers; // first = closest to the application
static PFN_xrGetInstanceProcAddr g_rtGipa = nullptr;
// What the loader puts under the last layer: creates the runtime's instance.
static XrResult XRAPI_CALL TermCreateApiLayerInstance(const XrInstanceCreateInfo* info, const XrApiLayerCreateInfo*, XrInstance* instance) {
  PFN_xrVoidFunction f = nullptr;
  if (XR_FAILED(g_rtGipa(XR_NULL_HANDLE, "xrCreateInstance", &f)) || !f) return XR_ERROR_INITIALIZATION_FAILED;
  return reinterpret_cast<PFN_xrCreateInstance>(f)(info, instance);
}
static std::string JsonString(const std::string& text, size_t from, const char* key) {
  std::string k = std::string("\"") + key + "\""; size_t p = text.find(k, from); if (p == std::string::npos) return "";
  p = text.find(':', p + k.size()); if (p == std::string::npos) return ""; p = text.find('"', p); if (p == std::string::npos) return "";
  std::string out; for (++p; p < text.size() && text[p] != '"'; ++p) { if (text[p] == '\\' && p + 1 < text.size()) ++p; out += text[p]; }
  return out;
}
static std::string FullPath(const std::string& p) { char b[1024]; DWORD n = GetFullPathNameA(p.c_str(), sizeof(b), b, nullptr); return n && n < sizeof(b) ? std::string(b) : p; }
// --layer takes a manifest (name and library_path are read from its "api_layer" object, a relative library_path is
// relative to the manifest) or a DLL (the layer's name is then the file name without extension, which is how both of
// our layers are named).
static bool AddLayer(const std::string& arg) {
  LayerMod m; std::string full = FullPath(arg); size_t dot = full.find_last_of('.'), slash = full.find_last_of("\\/");
  std::string ext = dot == std::string::npos ? "" : full.substr(dot); for (auto& ch : ext) ch = char(tolower((unsigned char)ch));
  std::string dir = slash == std::string::npos ? "." : full.substr(0, slash);
  if (ext == ".json") {
    FILE* f = fopen(full.c_str(), "rb"); if (!f) { L("layer manifest not found: %s", full.c_str()); return false; }
    std::string text; char buf[4096]; size_t n; while ((n = fread(buf, 1, sizeof(buf), f)) > 0) text.append(buf, n); fclose(f);
    size_t a = text.find("\"api_layer\""); if (a == std::string::npos) a = 0;
    m.name = JsonString(text, a, "name"); std::string lib = JsonString(text, a, "library_path");
    if (m.name.empty() || lib.empty()) { L("layer manifest without name or library_path: %s", full.c_str()); return false; }
    bool absolute = (lib.size() > 1 && lib[1] == ':') || lib[0] == '\\' || lib[0] == '/';
    m.path = FullPath(absolute ? lib : dir + "\\" + lib);
  } else { m.path = full; m.name = full.substr(slash == std::string::npos ? 0 : slash + 1, dot == std::string::npos ? std::string::npos : dot - (slash == std::string::npos ? 0 : slash + 1)); }
  g_layers.push_back(m); return true;
}
static bool NegotiateLayers() {
  for (auto& m : g_layers) {
    HMODULE h = LoadLibraryExA(m.path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!h) { L("cannot load layer %s (%lu)", m.path.c_str(), GetLastError()); return false; }
    auto nego = reinterpret_cast<XrResult(XRAPI_CALL*)(const XrNegotiateLoaderInfo*, const char*, XrNegotiateApiLayerRequest*)>(GetProcAddress(h, "xrNegotiateLoaderApiLayerInterface"));
    // Quad Views wants maxInterfaceVersion to be exactly the current one; both layers want the struct sizes below.
    XrNegotiateLoaderInfo li{XR_LOADER_INTERFACE_STRUCT_LOADER_INFO, XR_LOADER_INFO_STRUCT_VERSION, sizeof(XrNegotiateLoaderInfo), 1, XR_CURRENT_LOADER_API_LAYER_VERSION, XR_MAKE_VERSION(1, 0, 0), XR_MAKE_VERSION(1, 1, 0xfff)};
    XrNegotiateApiLayerRequest rq{XR_LOADER_INTERFACE_STRUCT_API_LAYER_REQUEST, XR_API_LAYER_INFO_STRUCT_VERSION, sizeof(XrNegotiateApiLayerRequest)};
    XrResult r = nego ? nego(&li, m.name.c_str(), &rq) : XR_ERROR_INITIALIZATION_FAILED;
    if (XR_FAILED(r) || !rq.getInstanceProcAddr || !rq.createApiLayerInstance) { L("layer %s (%s): negotiation failed %d%s", m.name.c_str(), m.path.c_str(), r, nego ? "" : " (no xrNegotiateLoaderApiLayerInterface export)"); return false; }
    m.gipa = rq.getInstanceProcAddr; m.create = rq.createApiLayerInstance;
    L("layer %s loaded from %s (interface %u, api %u.%u)", m.name.c_str(), m.path.c_str(), rq.layerInterfaceVersion, XR_VERSION_MAJOR(rq.layerApiVersion), XR_VERSION_MINOR(rq.layerApiVersion));
  }
  return true;
}
// The XrApiLayerNextInfo list: entry i is layer i's view of what is below it (layer i+1, or the runtime after the last).
static XrResult CreateInstanceChained(const XrInstanceCreateInfo& ici) {
  static std::vector<XrApiLayerNextInfo> next; next.assign(g_layers.size(), XrApiLayerNextInfo{});
  for (size_t i = 0; i < g_layers.size(); ++i) {
    XrApiLayerNextInfo& n = next[i]; bool last = i + 1 == g_layers.size();
    n.structType = XR_LOADER_INTERFACE_STRUCT_API_LAYER_NEXT_INFO; n.structVersion = XR_API_LAYER_NEXT_INFO_STRUCT_VERSION; n.structSize = sizeof(XrApiLayerNextInfo);
    strncpy(n.layerName, g_layers[i].name.c_str(), XR_MAX_API_LAYER_NAME_SIZE - 1);
    n.nextGetInstanceProcAddr = last ? g_rtGipa : g_layers[i + 1].gipa;
    n.nextCreateApiLayerInstance = last ? TermCreateApiLayerInstance : g_layers[i + 1].create;
    n.next = last ? nullptr : &next[i + 1];
  }
  static XrApiLayerCreateInfo ci; ci = XrApiLayerCreateInfo{};
  ci.structType = XR_LOADER_INTERFACE_STRUCT_API_LAYER_CREATE_INFO; ci.structVersion = XR_API_LAYER_CREATE_INFO_STRUCT_VERSION; ci.structSize = sizeof(XrApiLayerCreateInfo);
  ci.loaderInstance = nullptr; ci.nextInfo = next.data();
  XrResult r = g_layers[0].create(&ici, &ci, &g_inst);
  if (XR_SUCCEEDED(r)) g_gipa = g_layers[0].gipa; // from here on every function comes from the top of the chain
  return r;
}

// ---- four views in one projection layer ----
// View order of XR_VARJO_quad_views: 0 periphery left, 1 periphery right, 2 focus left, 3 focus right.
static Chain chQ[4]; static Source srcQ[4]; static int g_qw[4], g_qh[4];
static Fov g_focusFov[2]; static bool g_haveFocus = false;
// Motion: M pre-made phases of the four images, one phase per submitted frame.
static std::vector<std::array<Source, 4>> g_motionSrc; static bool g_motionOn = false; static long long g_motionCount = 0;
// Scene pattern: the phase being shown stays (the still case before the motion); and the phases are swept there and
// back (0 .. n-1 .. 0 ..) instead of looping, because that pattern does not repeat.
static bool g_motionHold = false, g_motionPingPong = false;
static size_t MotionIndex(long long count) { long long n = (long long)g_motionSrc.size(); if (!g_motionPingPong || n < 2) return size_t(count % n); long long T = 2 * (n - 1), m = count % T; return size_t(m < n ? m : T - m); }
static Fov FromXr(const XrFovf& f) { return {tan(f.angleLeft), tan(f.angleRight), tan(f.angleUp), tan(f.angleDown)}; }
static bool FrameQuad() {
  PollEvents(); if (g_quit) return false;
  if (!g_running) { Sleep(20); return true; }
  XrFrameState fst{XR_TYPE_FRAME_STATE}; XrFrameWaitInfo fwi{XR_TYPE_FRAME_WAIT_INFO};
  if (XR_FAILED(xrWaitFrame_(ses, &fwi, &fst))) return false;
  XrFrameBeginInfo fbi{XR_TYPE_FRAME_BEGIN_INFO}; if (XR_FAILED(xrBeginFrame_(ses, &fbi))) return false;
  XrCompositionLayerProjectionView pv[4]; XrCompositionLayerProjection lq{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
  const XrCompositionLayerBaseHeader* layers[1]; uint32_t nl = 0;
  if (fst.shouldRender && chQ[3].h) {
    XrViewLocateInfo vli{XR_TYPE_VIEW_LOCATE_INFO}; vli.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_QUAD_VARJO; vli.displayTime = fst.predictedDisplayTime; vli.space = space;
    XrViewState vs{XR_TYPE_VIEW_STATE}; XrView views[4] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}, {XR_TYPE_VIEW}, {XR_TYPE_VIEW}}; uint32_t vc = 0;
    if (XR_SUCCEEDED(xrLocateViews_(ses, &vli, &vs, 4, &vc, views)) && vc == 4) {
      g_viewFlags = vs.viewStateFlags; bool focusOk = true;
      for (int e = 0; e < 2; ++e) {
        g_eyePose[e] = views[e].pose;
        if (!g_haveFov && views[e].fov.angleRight > views[e].fov.angleLeft) g_eyeFov[e] = FromXr(views[e].fov);
        // The focus FOV is the layer's (the fixed section with no eye tracker); it is left untouched while the pose is not valid.
        const XrFovf& ff = views[2 + e].fov;
        if (ff.angleRight > ff.angleLeft && ff.angleUp > ff.angleDown) g_focusFov[e] = FromXr(ff); else focusOk = false;
      }
      if (g_eyeFov[0].r > g_eyeFov[0].l && g_eyeFov[1].r > g_eyeFov[1].l) g_haveFov = true;
      g_haveFocus = focusOk;
      if (focusOk) {
        std::array<Source, 4>* ms = (g_motionOn && !g_motionSrc.empty()) ? &g_motionSrc[MotionIndex(g_motionCount)] : nullptr;
        for (int v = 0; v < 4; ++v) {
          CopyTo(chQ[v], 0, ms ? (*ms)[v] : srcQ[v]);
          pv[v] = {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}; pv[v].pose = views[v].pose; pv[v].fov = views[v].fov; // exactly as located
          pv[v].subImage.swapchain = chQ[v].h; pv[v].subImage.imageRect = {{0, 0}, {chQ[v].w, chQ[v].hgt}}; pv[v].subImage.imageArrayIndex = 0;
        }
        FlushCopies(); if (ms && !g_motionHold) ++g_motionCount;
        lq.space = space; lq.viewCount = 4; lq.views = pv; layers[nl++] = reinterpret_cast<XrCompositionLayerBaseHeader*>(&lq);
      }
    }
  }
  XrFrameEndInfo fei{XR_TYPE_FRAME_END_INFO}; fei.displayTime = fst.predictedDisplayTime; fei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE; fei.layerCount = nl; fei.layers = layers;
  if (XR_FAILED(xrEndFrame_(ses, &fei))) ++g_endFailures;
  ++g_frames;
  static LARGE_INTEGER qf, q0; static int n0 = 0; LARGE_INTEGER q1; if (!qf.QuadPart) { QueryPerformanceFrequency(&qf); QueryPerformanceCounter(&q0); }
  QueryPerformanceCounter(&q1); double dt = double(q1.QuadPart - q0.QuadPart) / qf.QuadPart;
  if (dt >= 1.0) { g_lastFps = (g_frames - n0) / dt; n0 = g_frames; q0 = q1; }
  return true;
}
// The FOVs the images of a case are made for: periphery = the eye, focus = what the layer located.
static void QuadSceneFovs() { g_scene.layers = 1; g_scene.flagsB = 0; ResetPose(); for (int e = 0; e < 2; ++e) { g_scene.fovA[e] = g_eyeFov[e]; g_scene.fovB[e] = g_focusFov[e]; } }
static void QuadCaseFields(Case& c) {
  c["chain"] = "quad"; c["views"] = "4";
  // How far the located focus FOV is now from the one the images were made for (tangent). Not 0 = the focus moved
  // (eye tracking on): the red dots are then not where the analysis expects them.
  double drift = 0; for (int e = 0; e < 2; ++e) { const Fov& a = g_scene.fovB[e]; const Fov& b = g_focusFov[e]; drift = std::max(drift, std::max(std::max(fabs(a.l - b.l), fabs(a.r - b.r)), std::max(fabs(a.u - b.u), fabs(a.d - b.d)))); }
  c["focus_drift"] = F(drift);
}
// The focus image of the lattice cases: opaque (the layer makes the fade), red dots on the half-step lattice, and a white
// dot where the periphery lattice has its anchor (the focus covers it; the analysis starts its fit from the white dot).
static Image FocusLattice(int w, int h, const Fov& f, double step, double radTan, double sx) {
  Image im = Lattice(w, h, f, step, radTan, 0.5, 255, 0, 0, false, 1, false, sx, 0);
  double ppt = w / (f.r - f.l), radPx = radTan * ppt, cx = (sx - f.l) / (f.r - f.l) * w - 0.5, cy = (f.u - 0) / (f.u - f.d) * h - 0.5;
  for (int y = std::max(0, int(cy - radPx - 2)); y <= std::min(h - 1, int(cy + radPx + 2)); ++y) for (int x = std::max(0, int(cx - radPx - 2)); x <= std::min(w - 1, int(cx + radPx + 2)); ++x) {
    double dx = TanX(f, w, x) - sx, dy = TanY(f, h, y), cov = radPx - sqrt(dx * dx + dy * dy) * ppt + 0.5; cov = cov < 0 ? 0 : (cov > 1 ? 1 : cov);
    uint8_t v = Enc(cov), *o = im.at(x, y); for (int k = 0; k < 3; ++k) if (v > o[k]) o[k] = v; }
  return im;
}
static const double kQuadStep = 0.06, kQuadDot = 0.009;
static void QuadSetLattice(Source dst[4], double sx) {
  for (int e = 0; e < 2; ++e) {
    SetSource(dst[e], Lattice(g_qw[e], g_qh[e], g_scene.fovA[e], kQuadStep, kQuadDot, 0, 0, 255, 0, false, 1, true, sx, 0));
    SetSource(dst[2 + e], FocusLattice(g_qw[2 + e], g_qh[2 + e], g_scene.fovB[e], kQuadStep, kQuadDot, sx));
  }
}
static bool CaseQLattice(const char* tag) {
  QuadSceneFovs(); QuadSetLattice(srcQ, 0);
  Case c; c["kind"] = "lattice"; c["tag"] = tag; c["scale"] = "1"; c["ox"] = "0"; c["oy"] = "0"; c["step"] = F(kQuadStep);
  return Capture(c, false);
}
// gray: the same level in all four views; two: periphery one level, focus another.
static bool CaseQGray(double la, double lb) {
  QuadSceneFovs(); for (int e = 0; e < 2; ++e) { SetSource(srcQ[e], Flat(g_qw[e], g_qh[e], la)); SetSource(srcQ[2 + e], Flat(g_qw[2 + e], g_qh[2 + e], lb)); }
  Case c; c["kind"] = la == lb ? "gray" : "two"; c["la"] = F(la); c["lb"] = F(lb); c["reference"] = "0";
  return Capture(c, false);
}
// edge at scale 1: the periphery's edge half way between the eye's left border and the focus area, the focus' edge a
// little right of its centre (inside the part the fade leaves untouched).
static bool CaseQEdge() {
  QuadSceneFovs();
  for (int e = 0; e < 2; ++e) {
    const Fov& ey = g_scene.fovA[e]; const Fov& fb = g_scene.fovB[e];
    SetSource(srcQ[e], Edge(g_qw[e], g_qh[e], ey, (ey.l + fb.l) / 2));
    SetSource(srcQ[2 + e], Edge(g_qw[2 + e], g_qh[2 + e], fb, (fb.l + fb.r) / 2 + 0.2 * (fb.r - fb.l) / 2));
  }
  Case c; c["kind"] = "edge"; c["scale"] = "1"; c["reference"] = "0"; return Capture(c, false);
}
// Motion: the lattice (periphery and focus alike, same world motion) moves right by step / phases per submitted frame.
// The undistorted mirror is read `grabs` times per application frame from both of its buffers; every image that differs
// from the last one read from that buffer is one case (kind motion), until `frames` are kept. Each case carries the index
// of the last frame submitted before the read, so the analysis can place the dots it sees on the expected trajectory.
//
// Scene pattern (sm != null, --motion-pattern scene; scene_pattern.h): the same capture with a textured world that
// does not repeat and carries markers, instead of the lattice. It moves by (vx, vy) of tangent per submitted frame for
// phases - 1 frames, then back by the same step, and so on: frame n shows phase tri(n) = 0 .. phases-1 .. 0 ..
// One still case (kind scene, phase 0 held) is taken before the motion starts.
struct SceneMotion { uint32_t seed = 20261115; double vx = 0.0075, vy = 0; int fg = 0; };
static void SceneKeys(Case& c, const SceneMotion& sm, const ScenePattern& sp, int phases) {
  c["pattern"] = "scene"; c["seed"] = std::to_string(sm.seed); c["phases"] = std::to_string(phases); c["vtan"] = F(sm.vx); c["vtan_y"] = F(sm.vy); c["fg"] = std::to_string(sm.fg);
  c["sweep"] = "pingpong"; c["markers"] = std::to_string(sp.markers.size()); c["marker_r"] = F(kMkDisc); c["marker_min_dist"] = F(sp.minDist);
}
static bool CaseQMotion(int phases, int frames, int grabs, int warmup, const SceneMotion* sm = nullptr) {
  QuadSceneFovs();
  for (auto& set : g_motionSrc) for (auto& s : set) if (s.tex) s.tex->Release();
  ScenePattern sp; g_motionPingPong = sm != nullptr;
  if (sm) {
    sp.Build(sm->seed);
    L("scene pattern: seed %u, %d phases swept there and back, step %.6f %.6f of tangent per frame, %d markers in the world (least distance %.4f)", sm->seed, phases, sm->vx, sm->vy, (int)sp.markers.size(), sp.minDist);
  }
  g_motionSrc.assign(phases, {});
  for (int p = 0; p < phases; ++p) {
    if (sm) for (int e = 0; e < 2; ++e) {
      SetSource(g_motionSrc[p][e], SceneImage(sp, g_qw[e], g_qh[e], g_scene.fovA[e], p * sm->vx, p * sm->vy, false));
      SetSource(g_motionSrc[p][2 + e], SceneImage(sp, g_qw[2 + e], g_qh[2 + e], g_scene.fovB[e], p * sm->vx, p * sm->vy, true));
    } else QuadSetLattice(g_motionSrc[p].data(), kQuadStep * p / phases);
    if (!Frame()) return false;
  }
  if (sm) { // phase 0, not moving: the floor of the marker measurements, and a reference image
    g_motionCount = 0; g_motionHold = true; g_motionOn = true;
    Case c; c["kind"] = "scene"; c["tag"] = "still"; c["scale"] = "1"; c["step"] = F(kQuadStep); SceneKeys(c, *sm, sp, phases);
    bool ok = Capture(c, false); g_motionHold = false;
    if (!ok) { g_motionOn = false; return false; }
  }
  g_motionCount = 0; g_motionOn = true;
  if (!Frames(warmup)) { g_motionOn = false; return false; } // lets frame generation arm and settle on the moving content
  struct Grab { Image im; long long submitted; double ms; int buf, sub; double fps; std::string stamp; };
  std::vector<Grab> kept; Image last[2]; const char* keys[2] = {"undistorted_mirror_handle_0", "undistorted_mirror_handle_1"};
  LARGE_INTEGER qf, q0; QueryPerformanceFrequency(&qf); QueryPerformanceCounter(&q0);
  int waitMs = g_lastFps > 1 ? int(1000.0 / g_lastFps / grabs) : 8; waitMs = waitMs < 2 ? 2 : (waitMs > 20 ? 20 : waitMs);
  int appFrames = 0, reads = 0; bool alive = true;
  for (; alive && appFrames < frames * 12 && (int)kept.size() < frames; ++appFrames) {
    alive = Frame();
    for (int g = 0; alive && g < grabs && (int)kept.size() < frames; ++g) {
      if (g) Sleep(waitMs);
      for (int b = 0; b < 2 && (int)kept.size() < frames; ++b) {
        Image im; if (!GrabMirrorOnce(keys[b], im)) continue; ++reads;
        if (im.px.size() == last[b].px.size()) { size_t diff = 0, n = im.px.size() / 4; for (size_t i = 0; i < n; i += 7) if (memcmp(&im.px[i * 4], &last[b].px[i * 4], 3)) ++diff; if (double(diff) < 0.0005 * double(n / 7)) continue; }
        LARGE_INTEGER q1; QueryPerformanceCounter(&q1); last[b] = im;
        kept.push_back({std::move(im), g_motionCount - 1, double(q1.QuadPart - q0.QuadPart) * 1000.0 / qf.QuadPart, b, g, g_lastFps, NowStamp()});
      }
    }
  }
  g_motionOn = false;
  L("motion: %d images kept from %d reads over %d application frames (%.1f fps), %d phases, %d reads per frame %d ms apart", (int)kept.size(), reads, appFrames, g_lastFps, phases, grabs, waitMs);
  for (size_t i = 0; i < kept.size(); ++i) {
    const Grab& k = kept[i]; char name[64]; snprintf(name, sizeof(name), "case%03d", g_caseNo++); char tag[32]; snprintf(tag, sizeof(tag), "m%02d", (int)i);
    Case c; c["kind"] = "motion"; c["tag"] = tag; c["id"] = name; c["mirror"] = "undistorted"; c["unstable"] = "0"; c["time"] = k.stamp; c["fps"] = F(k.fps);
    c["format"] = std::to_string(g_fmt); c["array"] = "0"; c["premult"] = std::to_string(g_premult); c["layers"] = "1"; c["tracked"] = Tracked() ? "1" : "0"; c["state"] = std::to_string(g_state);
    c["scale"] = "1"; c["step"] = F(kQuadStep); c["phases"] = std::to_string(phases); c["vtan"] = F(kQuadStep / phases);
    c["seq"] = std::to_string(i); c["buf"] = std::to_string(k.buf); c["sub"] = std::to_string(k.sub); c["submitted"] = std::to_string(k.submitted); c["t_ms"] = F(k.ms);
    PutFov(c, "eye", g_eyeFov); PutFov(c, "fovA", g_scene.fovA); PutFov(c, "fovB", g_scene.fovB);
    c["aw"] = std::to_string(g_capAW); c["ah"] = std::to_string(g_capAH); c["bw"] = std::to_string(g_capBW); c["bh"] = std::to_string(g_capBH);
    QuadCaseFields(c);
    if (sm) SceneKeys(c, *sm, sp, phases);
    SavePng(Wide(g_out + "\\" + name + ".png"), k.im); SaveCase(g_out + "\\" + name + ".txt", c);
  }
  for (auto& set : g_motionSrc) for (auto& s : set) if (s.tex) s.tex->Release();
  g_motionSrc.clear();
  return alive && !kept.empty();
}

static bool SetMirror(const char* key, int on) { int r = PvrSetInt(key, on); L("set %s=%d -> %d", key, on, r); return r == 0; }
static uint64_t VramUsed() { DXGI_QUERY_VIDEO_MEMORY_INFO i{}; if (adapter3 && SUCCEEDED(adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &i))) return i.CurrentUsage; return 0; }

static bool RunVariant(bool full, bool quick) {
  L("variant format %lld array %d premult %d full %d", (long long)g_fmt, g_array, g_premult, full);
  ResetPose();
  struct P { double x, y; const char* tag; };
  std::vector<P> pos = {{0.1, 0.1, "centre"}};
  if (full) { pos.insert(pos.end(), {{-0.3, 0.2, "slow1"}, {0.0, -0.3, "slow2"}, {0.35, 0.15, "slow3"}, {0.2, 0.4, "slow4"}, {-0.5, -0.4, "jump1"}, {0.6, 0.5, "jump2"}, {-0.9, 0.0, "edge_left"}, {0.0, 0.95, "edge_top"}}); }
  else pos.push_back({-0.9, 0.3, "edge_left"});
  for (auto& p : pos) if (!CaseLattice(1, p.x, p.y, 0.06, false, p.tag)) return false;
  if (!CaseLattice(2.6, 0.1, 0.1, 0.06, false, "scaled")) return false;
  std::vector<double> levels = quick ? std::vector<double>{0.03, 0.18, 0.9} : std::vector<double>{0.01, 0.03, 0.10, 0.18, 0.5, 0.9};
  for (double l : levels) if (!CaseGray(l, l, false, "gray")) return false;
  if (!CaseGray(0.0, 1.0, false, "two") || !CaseGray(0.1, 0.5, false, "two") || !CaseGray(0.5, 0.1, false, "two")) return false;
  if (!CaseSentinel(true)) return false;
  if (full && !CaseSentinel(false)) return false;
  return true;
}

// Chain mode: four swapchains at the sizes the layer recommends, then the same wait for tracking as the direct mode.
static int QuadSetup(Case& info, XrSystemId sys, bool allowUntracked) {
  for (int i = 0; i < 300 && !g_running && !g_quit; ++i) { PollEvents(); Sleep(20); }
  XrViewConfigurationView v[4] = {{XR_TYPE_VIEW_CONFIGURATION_VIEW}, {XR_TYPE_VIEW_CONFIGURATION_VIEW}, {XR_TYPE_VIEW_CONFIGURATION_VIEW}, {XR_TYPE_VIEW_CONFIGURATION_VIEW}}; uint32_t n = 0;
  XrResult r = fn<PFN_xrEnumerateViewConfigurationViews>("xrEnumerateViewConfigurationViews")(g_inst, sys, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_QUAD_VARJO, 4, &n, v);
  if (XR_FAILED(r) || n != 4) { L("no quad view configuration (result %d, %u views): is the Quad Views layer in the chain, and not bypassed?", r, n); return 2; }
  uint64_t v0 = VramUsed(); std::string sizes;
  for (int i = 0; i < 4; ++i) {
    g_qw[i] = int(v[i].recommendedImageRectWidth); g_qh[i] = int(v[i].recommendedImageRectHeight); sizes += (i ? " " : "") + std::to_string(g_qw[i]) + "x" + std::to_string(g_qh[i]);
    if (!MakeChain(chQ[i], g_qw[i], g_qh[i], 1, g_fmt)) return 2;
    SetSource(srcQ[i], Solid(g_qw[i], g_qh[i], 0, 0, 0));
  }
  g_capAW = g_qw[0]; g_capAH = g_qh[0]; g_capBW = g_qw[2]; g_capBH = g_qh[2];
  info["recommended_quad"] = sizes; info["images_per_swapchain"] = std::to_string(chQ[0].img.size()); info["vram_start"] = std::to_string(v0);
  L("quad views: recommended %s, format %lld", sizes.c_str(), (long long)g_fmt);
  int waited = 0, good = 0;
  while (!g_quit && waited < 90 * (allowUntracked ? 15 : 150)) { Frames(90); waited += 90; bool ok = g_haveFov && g_haveFocus && Tracked() && g_state == XR_SESSION_STATE_FOCUSED; good = ok ? good + 1 : 0; if (good >= 3) break; }
  L("tracked %d state %d focus fov %d fps %.1f after %d frames", Tracked(), g_state, g_haveFocus, g_lastFps, waited);
  if (g_quit || !g_running) return 2;
  if ((!Tracked() && !allowUntracked) || g_state != XR_SESSION_STATE_FOCUSED || !g_haveFocus) return 3;
  info["focus_fov0"] = F(g_focusFov[0].l) + " " + F(g_focusFov[0].r) + " " + F(g_focusFov[0].u) + " " + F(g_focusFov[0].d);
  return 0;
}
struct QuadOptions { bool quick = false, motion = false, motionOnly = false; int phases = 8, frames = 24, grabs = 2, warmup = 300; bool scene = false, diag = false, phasesGiven = false; uint32_t seed = 20261115; int fg = 0; };
static int QuadRun(const QuadOptions& o) {
  if (!SetMirror("enable_undistorted_mirror", 1)) return 4;
  Frames(90); g_premult = 1;
  bool ok = CaseQLattice("centre"); // first: the analysis takes the mirror's geometry from it
  if (ok && !o.motionOnly) {
    std::vector<double> levels = o.quick ? std::vector<double>{0.03, 0.18, 0.9} : std::vector<double>{0.01, 0.03, 0.10, 0.18, 0.5, 0.9};
    for (double l : levels) ok = ok && CaseQGray(l, l);
    ok = ok && CaseQGray(0.0, 1.0) && CaseQGray(0.1, 0.5) && CaseQGray(0.5, 0.1) && CaseQEdge();
  }
  if (ok && (o.motion || o.motionOnly) && o.scene) {
    // Scene pattern: 0.0075 of tangent per frame along x (the lattice's default speed), and 0.6 of that upwards with
    // --motion-diag. The sweep (phases - 1 steps) must stay inside the part of the world that has markers, and the
    // pre-made images (phases * four views, 4 bytes per pixel) must fit in video memory.
    SceneMotion sm; sm.seed = o.seed; sm.fg = o.fg; sm.vx = kQuadStep / 8; sm.vy = o.diag ? 0.6 * sm.vx : 0;
    int phases = o.phasesGiven ? o.phases : 32, most = 1 + int(kSceneSweep / hypot(sm.vx, sm.vy)), least = 8;
    if (phases > most) { L("scene pattern: %d phases would sweep past the markers' area, using %d", phases, most); phases = most; }
    if (phases < least) phases = least;
    double per = 0; for (int i = 0; i < 4; ++i) per += double(g_qw[i]) * g_qh[i] * 4;
    DXGI_QUERY_VIDEO_MEMORY_INFO vm{};
    if (adapter3 && SUCCEEDED(adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &vm)) && vm.Budget > vm.CurrentUsage) {
      double room = 0.7 * double(vm.Budget - vm.CurrentUsage); L("scene pattern: %d phases need %.0f MB of video memory, %.0f MB usable", phases, phases * per / 1048576, room / 1048576);
      if (phases * per > room && int(room / per) >= least) { phases = int(room / per); L("scene pattern: reduced to %d phases to fit", phases); }
    }
    ok = CaseQMotion(phases, o.frames, o.grabs, o.warmup, &sm);
  } else
  if (ok && (o.motion || o.motionOnly)) ok = CaseQMotion(o.phases, o.frames, o.grabs, o.warmup);
  for (int i = 0; i < 4; ++i) SetSource(srcQ[i], Solid(g_qw[i], g_qh[i], 0, 0, 0));
  Frames(10);
  return ok ? 0 : (g_quit ? 2 : 4);
}

int main(int argc, char** argv) {
  if (argc < 2) { printf("usage: probe_run <outdir> [--runtime <dll>] [--quick] [--d3d12]\n       probe_run <outdir> --quad --layer <manifest.json|dll> [--layer ...] [--quick] [--format 28|29] [--motion|--motion-only] [--motion-phases M] [--motion-frames N] [--motion-grabs G] [--motion-warmup F] [--motion-pattern lattice|scene] [--motion-diag] [--motion-seed S] [--motion-fg 1|2|3]   (see CHAIN.md)\n"); return 2; }
  g_out = argv[1]; CreateDirectoryA(g_out.c_str(), nullptr);
  std::string rtPath = "C:\\Program Files\\Pimax\\Runtime\\PiOpenXR_64.dll"; bool quick = false, allowUntracked = false;
  for (int i = 2; i < argc; ++i) { if (!strcmp(argv[i], "--runtime") && i + 1 < argc) rtPath = argv[++i]; else if (!strcmp(argv[i], "--quick")) quick = true; else if (!strcmp(argv[i], "--d3d12")) g_d3d12 = true; else if (!strcmp(argv[i], "--allow-untracked")) allowUntracked = true; }
  // Chain mode options (a second pass, so the line above stays as it was).
  QuadOptions qo; std::vector<std::string> layerArgs; int64_t formatArg = 0; bool patternBad = false;
  for (int i = 2; i < argc; ++i) {
    auto val = [&](int def) { return i + 1 < argc ? atoi(argv[++i]) : def; };
    if (!strcmp(argv[i], "--layer") && i + 1 < argc) layerArgs.push_back(argv[++i]);
    else if (!strcmp(argv[i], "--runtime")) ++i;
    else if (!strcmp(argv[i], "--quad")) g_quad = true;
    else if (!strcmp(argv[i], "--motion")) qo.motion = true;
    else if (!strcmp(argv[i], "--motion-only")) qo.motionOnly = true;
    else if (!strcmp(argv[i], "--motion-phases")) { qo.phases = val(8); qo.phasesGiven = true; }
    else if (!strcmp(argv[i], "--motion-pattern") && i + 1 < argc) { ++i; if (!strcmp(argv[i], "scene")) qo.scene = true; else if (strcmp(argv[i], "lattice")) patternBad = true; }
    else if (!strcmp(argv[i], "--motion-diag")) qo.diag = true;
    else if (!strcmp(argv[i], "--motion-seed")) qo.seed = uint32_t(strtoul(i + 1 < argc ? argv[++i] : "20261115", nullptr, 10));
    else if (!strcmp(argv[i], "--motion-fg")) qo.fg = val(0);
    else if (!strcmp(argv[i], "--motion-frames")) qo.frames = val(24);
    else if (!strcmp(argv[i], "--motion-grabs")) qo.grabs = val(2);
    else if (!strcmp(argv[i], "--motion-warmup")) qo.warmup = val(300);
    else if (!strcmp(argv[i], "--format")) formatArg = val(0);
  }
  qo.quick = quick; qo.phases = qo.phases < 4 ? 4 : (qo.phases > 64 ? 64 : qo.phases); qo.frames = qo.frames < 1 ? 1 : (qo.frames > 120 ? 120 : qo.frames);
  qo.grabs = qo.grabs < 1 ? 1 : (qo.grabs > 4 ? 4 : qo.grabs); qo.warmup = qo.warmup < 0 ? 0 : qo.warmup;
  g_log = fopen((g_out + "\\probe_run.log").c_str(), "w"); if (!g_log) return 2;
  if (g_quad && g_d3d12) { L("--quad is D3D11 only (Quad Views bypasses itself for any other API)"); return 2; }
  if ((qo.motion || qo.motionOnly) && !g_quad) { L("--motion needs --quad"); return 2; }
  if (patternBad) { L("--motion-pattern: lattice or scene"); return 2; }
  if (qo.diag && !qo.scene) { L("--motion-diag needs --motion-pattern scene"); return 2; }
  qo.fg = qo.fg < 0 || qo.fg > 3 ? 0 : qo.fg;
  if (g_quad) { g_viewConfig = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_QUAD_VARJO; if (formatArg == 28 || formatArg == 29) g_fmt = formatArg; }
  for (auto& a : layerArgs) if (!AddLayer(a)) return 2;
  CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  InstallPvrHook();
  std::string dir = rtPath.substr(0, rtPath.find_last_of("\\/")); SetDllDirectoryA(dir.c_str());
  HMODULE rt = LoadLibraryExA(rtPath.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
  if (!rt) { L("cannot load runtime %s (%lu)", rtPath.c_str(), GetLastError()); return 2; }
  auto nego = reinterpret_cast<XrResult(XRAPI_CALL*)(const XrNegotiateLoaderInfo*, XrNegotiateRuntimeRequest*)>(GetProcAddress(rt, "xrNegotiateLoaderRuntimeInterface"));
  XrNegotiateLoaderInfo li{XR_LOADER_INTERFACE_STRUCT_LOADER_INFO, XR_LOADER_INFO_STRUCT_VERSION, sizeof(XrNegotiateLoaderInfo), 1, XR_CURRENT_LOADER_RUNTIME_VERSION, XR_MAKE_VERSION(1, 0, 0), XR_MAKE_VERSION(1, 1, 0xfff)};
  XrNegotiateRuntimeRequest rr{XR_LOADER_INTERFACE_STRUCT_RUNTIME_REQUEST, XR_RUNTIME_INFO_STRUCT_VERSION, sizeof(XrNegotiateRuntimeRequest)};
  if (!nego || XR_FAILED(nego(&li, &rr)) || !rr.getInstanceProcAddr) { L("runtime negotiation failed"); return 2; }
  g_gipa = rr.getInstanceProcAddr; g_rtGipa = rr.getInstanceProcAddr;
  if (!g_layers.empty() && !NegotiateLayers()) return 2;

  Case info;
  if (!g_layers.empty()) { std::string names, paths; for (auto& m : g_layers) { names += (names.empty() ? "" : " > ") + m.name; paths += (paths.empty() ? "" : " | ") + m.path; } info["layers"] = names; info["layer_paths"] = paths; }
  if (g_quad) info["chain"] = "quad"; info["runtime"] = rtPath; info["pid"] = std::to_string(GetCurrentProcessId()); info["start"] = NowStamp();
  { auto en = fn<PFN_xrEnumerateInstanceExtensionProperties>("xrEnumerateInstanceExtensionProperties"); uint32_t n = 0; en(nullptr, 0, &n, nullptr);
    std::vector<XrExtensionProperties> ext(n, {XR_TYPE_EXTENSION_PROPERTIES}); en(nullptr, n, &n, ext.data()); std::string all;
    for (auto& e : ext) { all += e.extensionName; all += " "; } info["extensions"] = all; }
  const char* exts[] = {g_d3d12 ? "XR_KHR_D3D12_enable" : "XR_KHR_D3D11_enable"}; info["api"] = g_d3d12 ? "D3D12" : "D3D11";
  XrInstanceCreateInfo ici{XR_TYPE_INSTANCE_CREATE_INFO}; strcpy(ici.applicationInfo.applicationName, "dcsvr_runtime_probe"); ici.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
  ici.enabledExtensionCount = 1; ici.enabledExtensionNames = exts;
  // Chain mode: quad views is asked for the way the game does (the Quad Views layer takes it out before the runtime sees
  // it), and the instance is created through the layers. The application name stays ours, not "DCS World".
  const char* quadExts[] = {"XR_KHR_D3D11_enable", "XR_VARJO_quad_views"};
  if (g_quad) { ici.enabledExtensionCount = 2; ici.enabledExtensionNames = quadExts; }
  if (!g_layers.empty()) { XrResult cr = CreateInstanceChained(ici); if (XR_FAILED(cr)) { L("xrCreateInstance through the layer chain failed %d", cr); return 2; } }
  else
  if (XR_FAILED(fn<PFN_xrCreateInstance>("xrCreateInstance")(&ici, &g_inst))) { L("xrCreateInstance failed"); return 2; }
  xrPollEvent_ = fn<PFN_xrPollEvent>("xrPollEvent"); xrBeginSession_ = fn<PFN_xrBeginSession>("xrBeginSession"); xrEndSession_ = fn<PFN_xrEndSession>("xrEndSession");
  xrWaitFrame_ = fn<PFN_xrWaitFrame>("xrWaitFrame"); xrBeginFrame_ = fn<PFN_xrBeginFrame>("xrBeginFrame"); xrEndFrame_ = fn<PFN_xrEndFrame>("xrEndFrame"); xrLocateViews_ = fn<PFN_xrLocateViews>("xrLocateViews");
  xrAcquire = fn<PFN_xrAcquireSwapchainImage>("xrAcquireSwapchainImage"); xrWaitImg = fn<PFN_xrWaitSwapchainImage>("xrWaitSwapchainImage"); xrRelease = fn<PFN_xrReleaseSwapchainImage>("xrReleaseSwapchainImage");
  xrCreateSwapchain_ = fn<PFN_xrCreateSwapchain>("xrCreateSwapchain"); xrDestroySwapchain_ = fn<PFN_xrDestroySwapchain>("xrDestroySwapchain"); xrEnumImages = fn<PFN_xrEnumerateSwapchainImages>("xrEnumerateSwapchainImages");

  XrSystemGetInfo sgi{XR_TYPE_SYSTEM_GET_INFO}; sgi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY; XrSystemId sys = 0;
  if (XR_FAILED(fn<PFN_xrGetSystem>("xrGetSystem")(g_inst, &sgi, &sys))) { L("xrGetSystem failed: headset not available"); return 2; }
  XrGraphicsRequirementsD3D11KHR req{XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR};
  if (g_d3d12) { XrGraphicsRequirementsD3D12KHR r12{XR_TYPE_GRAPHICS_REQUIREMENTS_D3D12_KHR}; fn<PFN_xrGetD3D12GraphicsRequirementsKHR>("xrGetD3D12GraphicsRequirementsKHR")(g_inst, sys, &r12); req.adapterLuid = r12.adapterLuid; }
  else fn<PFN_xrGetD3D11GraphicsRequirementsKHR>("xrGetD3D11GraphicsRequirementsKHR")(g_inst, sys, &req);
  IDXGIFactory1* fac = nullptr; CreateDXGIFactory1(IID_PPV_ARGS(&fac)); IDXGIAdapter1* ad = nullptr;
  for (UINT i = 0; fac->EnumAdapters1(i, &ad) != DXGI_ERROR_NOT_FOUND; ++i) { DXGI_ADAPTER_DESC1 d; ad->GetDesc1(&d); if (!memcmp(&d.AdapterLuid, &req.adapterLuid, sizeof(LUID))) break; ad->Release(); ad = nullptr; }
  D3D_FEATURE_LEVEL fl[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
  if (FAILED(D3D11CreateDevice(ad, ad ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, fl, 2, D3D11_SDK_VERSION, &dev, nullptr, &ctx))) { L("D3D11CreateDevice failed"); return 2; }
  if (ad) ad->QueryInterface(&adapter3);
  XrGraphicsBindingD3D11KHR gb{XR_TYPE_GRAPHICS_BINDING_D3D11_KHR}; gb.device = dev;
  XrGraphicsBindingD3D12KHR gb12{XR_TYPE_GRAPHICS_BINDING_D3D12_KHR};
  if (g_d3d12) {
    D3D12_COMMAND_QUEUE_DESC qd{}; qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (FAILED(D3D12CreateDevice(ad, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&dev12))) || FAILED(dev12->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue12))) ||
        FAILED(dev12->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc12))) ||
        FAILED(dev12->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc12, nullptr, IID_PPV_ARGS(&list12))) || FAILED(dev12->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence12)))) { L("D3D12 device setup failed"); return 2; }
    list12->Close(); g_fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr); gb12.device = dev12; gb12.queue = queue12;
  }
  XrSessionCreateInfo sci{XR_TYPE_SESSION_CREATE_INFO}; sci.next = g_d3d12 ? (void*)&gb12 : (void*)&gb; sci.systemId = sys;
  XrResult sr = XR_ERROR_RUNTIME_FAILURE;
  sr = fn<PFN_xrCreateSession>("xrCreateSession")(g_inst, &sci, &ses);
  // Right after a headset restart the runtime refuses the session for a while, and keeps refusing it in this process: a new process works.
  if (XR_FAILED(sr)) { L("xrCreateSession failed %d: start the probe again in a few seconds", sr); return 5; }
  XrReferenceSpaceCreateInfo rsi{XR_TYPE_REFERENCE_SPACE_CREATE_INFO}; rsi.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL; rsi.poseInReferenceSpace.orientation.w = 1;
  fn<PFN_xrCreateReferenceSpace>("xrCreateReferenceSpace")(ses, &rsi, &space);
  XrViewConfigurationView vcv[2] = {{XR_TYPE_VIEW_CONFIGURATION_VIEW}, {XR_TYPE_VIEW_CONFIGURATION_VIEW}}; uint32_t n = 0;
  fn<PFN_xrEnumerateViewConfigurationViews>("xrEnumerateViewConfigurationViews")(g_inst, sys, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 2, &n, vcv);
  info["recommended"] = std::to_string(vcv[0].recommendedImageRectWidth) + "x" + std::to_string(vcv[0].recommendedImageRectHeight);

  // Session running, FOV known, headset tracked: the Pimax compositor shows no application until the headset tracks.
  int exitCode = 0;
  if (g_quad) exitCode = QuadSetup(info, sys, allowUntracked);
  else { for (int i = 0; i < 300 && !g_running && !g_quit; ++i) { PollEvents(); Sleep(20); }
    uint64_t v0 = VramUsed();
    if (!MakeChain(chA[0], AW, AH, 1, g_fmt) || !MakeChain(chA[1], AW, AH, 1, g_fmt)) return 2;
    for (int e = 0; e < 2; ++e) { g_scene.fovA[e] = Fov{-1, 1, 1, -1}; SetSource(srcA[e], Solid(AW, AH, 0, 0, 0)); }
    g_scene.layers = 1; Frames(30);
    for (int e = 0; e < 2; ++e) g_scene.fovA[e] = g_eyeFov[e];
    int waited = 0; while (!g_quit && waited < 90 * (allowUntracked ? 15 : 150)) { Frames(90); waited += 90; if (g_haveFov) for (int e = 0; e < 2; ++e) g_scene.fovA[e] = g_eyeFov[e]; bool ok = g_haveFov && Tracked() && g_state == XR_SESSION_STATE_FOCUSED; static int good = 0; good = ok ? good + 1 : 0; if (good >= 3) break; }
    info["images_per_swapchain"] = std::to_string(chA[0].img.size()); info["vram_start"] = std::to_string(v0);
    L("tracked %d state %d fps %.1f after %d frames", Tracked(), g_state, g_lastFps, waited);
    if (g_quit || !g_running) exitCode = 2; else if ((!Tracked() && !allowUntracked) || g_state != XR_SESSION_STATE_FOCUSED) exitCode = 3; }
  info["tracked"] = Tracked() ? "1" : "0"; info["state"] = std::to_string(g_state);
  info["fov0"] = F(g_eyeFov[0].l) + " " + F(g_eyeFov[0].r) + " " + F(g_eyeFov[0].u) + " " + F(g_eyeFov[0].d);

  int mirrorBefore[2] = {PvrGetInt("enable_undistorted_mirror", -1), PvrGetInt("enable_screen_mirror", -1)};
  info["mirror_keys_before"] = std::to_string(mirrorBefore[0]) + " " + std::to_string(mirrorBefore[1]);
  if (exitCode == 0 && g_quad) exitCode = QuadRun(qo);
  else if (exitCode == 0) {
    if (!SetMirror("enable_undistorted_mirror", 1)) exitCode = 4;
    Frames(90);
    struct V { int64_t fmt; bool array; int premult; bool full; };
    std::vector<V> variants = {{29, false, 1, getenv("PROBE_NO_REFERENCE") == nullptr}, {29, false, 0, false}, {29, false, 2, false}, {28, false, 1, false}, {28, false, 0, false}, {28, false, 2, false}, {29, true, 2, false}, {29, true, 0, false}, {28, true, 0, false}, {28, true, 2, false}};
    if (quick) variants.resize(3);
    int64_t curFmt = -1; bool curArray = false; bool haveChains = false;
    for (size_t vi = 0; vi < variants.size() && exitCode == 0; ++vi) {
      const V& v = variants[vi];
      if (!haveChains || v.fmt != curFmt || v.array != curArray) {
        g_scene.layers = 1; for (int e = 0; e < 2; ++e) { FreeChain(chB[e]); }
        Chain oldA[2] = {chA[0], chA[1]};
        g_fmt = v.fmt; curFmt = v.fmt; curArray = v.array; uint64_t before = VramUsed();
        bool ok;
        if (v.array) { Chain na, nb; ok = MakeChain(na, AW, AH, 2, g_fmt) && MakeChain(nb, BW, BH, 2, g_fmt); chA[0] = na; chA[1] = Chain(); chB[0] = nb; }
        else { Chain a0, a1; ok = MakeChain(a0, AW, AH, 1, g_fmt) && MakeChain(a1, AW, AH, 1, g_fmt) && MakeChain(chB[0], BW, BH, 1, g_fmt) && MakeChain(chB[1], BW, BH, 1, g_fmt); chA[0] = a0; chA[1] = a1; }
        g_array = v.array;
        for (int e = 0; e < 2; ++e) if (oldA[e].h) xrDestroySwapchain_(oldA[e].h);
        if (!ok) { exitCode = 2; break; }
        Frames(20); info["vram_two_layer_" + std::to_string(vi)] = std::to_string((long long)VramUsed() - (long long)before); haveChains = true;
      }
      g_premult = v.premult;
      if (!RunVariant(v.full, quick)) { exitCode = g_quit ? 2 : 4; break; }
      if (vi == 0) {
        // The composed reference (one 5424x5356 layer), the scale-model sharpness test and reprojection, once.
        uint64_t before = VramUsed();
        if (MakeChain(chR[0], RW, RH, 1, g_fmt) && MakeChain(chR[1], RW, RH, 1, g_fmt)) {
          Frames(10); info["vram_reference"] = std::to_string((long long)VramUsed() - (long long)before);
          CaseGray(0.03, 0.03, true, "gray"); CaseGray(0.18, 0.18, true, "gray"); CaseGray(0.9, 0.9, true, "gray"); CaseGray(0.0, 1.0, true, "two"); CaseGray(0.1, 0.5, true, "two");
          CaseEdge(2.6, false, false); CaseEdge(2.6, true, false);
          SetMirror("enable_screen_mirror", 1); Frames(90);
          CaseEdge(2.6, false, true); CaseEdge(2.6, true, true);
          CaseReproj(0, 0, 0, "rest"); CaseReproj(0.5f, 0.5f, 0, "yaw0.5"); CaseReproj(1, 1, 0, "yaw1"); CaseReproj(2, 2, 0, "yaw2"); CaseReproj(0, 0, 2, "pitch2");
          CaseReproj(0, 2, 0, "only_b_yaw2");
          SetMirror("enable_screen_mirror", 0); Frames(30);
          // Compositor load: 20 s of the composed reference, 20 s of two layers; the analysis reads the Pimax server log.
          auto timing = [&](const char* tag, bool reference) {
            SetGrayScene(0.18, 0.18, reference); Frames(90);
            Case c; c["kind"] = "timing"; c["tag"] = tag; c["begin"] = NowStamp(); int f0 = g_frames, e0 = g_endFailures; Frames(1800); c["end"] = NowStamp();
            c["frames"] = std::to_string(g_frames - f0); c["end_failures"] = std::to_string(g_endFailures - e0); c["id"] = std::string("timing_") + tag; SaveCase(g_out + "\\timing_" + tag + ".txt", c); };
          SetMirror("enable_undistorted_mirror", 0); Frames(30);
          timing("reference", true); timing("two_layer", false);
          SetMirror("enable_undistorted_mirror", 1); Frames(60);
          g_scene.layers = 1; Frames(5); FreeChain(chR[0]); FreeChain(chR[1]);
        } else L("reference swapchain failed");
      }
    }
  }
  // Restore: mirror keys back to what they were (unset reads -1; 0 turns them off).
  SetMirror("enable_undistorted_mirror", mirrorBefore[0] > 0 ? 1 : 0); SetMirror("enable_screen_mirror", mirrorBefore[1] > 0 ? 1 : 0);
  info["mirror_keys_after"] = std::to_string(PvrGetInt("enable_undistorted_mirror", -1)) + " " + std::to_string(PvrGetInt("enable_screen_mirror", -1));
  info["end"] = NowStamp(); info["exit"] = std::to_string(exitCode); info["cases"] = std::to_string(g_caseNo); info["end_failures"] = std::to_string(g_endFailures);
  SaveCase(g_out + "\\run.txt", info);
  if (g_running) { fn<PFN_xrRequestExitSession>("xrRequestExitSession")(ses); for (int i = 0; i < 100 && g_running; ++i) { Frame(); } }
  fn<PFN_xrDestroySession>("xrDestroySession")(ses); fn<PFN_xrDestroyInstance>("xrDestroyInstance")(g_inst);
  L("exit %d, %d cases", exitCode, g_caseNo);
  return exitCode;
}
