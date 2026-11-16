// Headset-free test of the rig capture layer: this program is the loader, the runtime and the layer above (it plays
// OFXR's part: an application swapchain, a "current" ring of two and a "synthetic" ring of one per projection layer,
// array swapchains of two slices, real and synthetic submissions alternating, one repeated submission).
// D3D12 on the WARP software device (default) or the hardware adapter (--hw). Every image holds a pattern that is a
// formula of (layer, eye, frame, x, y); check_harness.py recomputes it and compares the captured bytes.
//   rigtap_harness <layer dll> <out dir> [--hw] [--live]
// --live: the live image sizes (periphery 1076x1060, focus 1952x1928, two slices each) instead of a quarter of them.
#define XR_USE_GRAPHICS_API_D3D12
#define NOMINMAX
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <openxr/openxr_loader_negotiation.h>

static ID3D12Device* g_dev; static ID3D12CommandQueue* g_queue; static ID3D12CommandAllocator* g_alloc; static ID3D12GraphicsCommandList* g_list; static ID3D12Fence* g_fence; static HANDLE g_event; static UINT64 g_fv;
static void Flush() { g_queue->Signal(g_fence, ++g_fv); g_fence->SetEventOnCompletion(g_fv, g_event); WaitForSingleObject(g_event, INFINITE); }

// ---- the fake runtime --------------------------------------------------------------------------------------------
struct RtSc { uint32_t w, h, array; DXGI_FORMAT fmt; std::vector<ID3D12Resource*> img; uint32_t next = 0; };
static std::map<uint64_t, RtSc> g_rt; static uint64_t g_handle = 0x1000; static unsigned g_rtEnd = 0, g_rtEndBad = 0; static const XrFrameEndInfo* g_expectInfo = nullptr;
static XrResult XRAPI_CALL RtCreateSession(XrInstance, const XrSessionCreateInfo*, XrSession* s) { *s = (XrSession)0x51; return XR_SUCCESS; }
static XrResult XRAPI_CALL RtDestroySession(XrSession) { return XR_SUCCESS; }
static XrResult XRAPI_CALL RtCreateSc(XrSession, const XrSwapchainCreateInfo* ci, XrSwapchain* out) {
  RtSc sc{ci->width, ci->height, ci->arraySize, ci->format == 28 ? DXGI_FORMAT_R8G8B8A8_TYPELESS : DXGI_FORMAT_R8G8B8A8_UNORM_SRGB};   // format 28 comes back TYPELESS (as a runtime may do), 29 typed
  for (int i = 0; i < 3; ++i) { D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_DEFAULT; D3D12_RESOURCE_DESC d{}; d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; d.Width = sc.w; d.Height = sc.h; d.DepthOrArraySize = (UINT16)sc.array; d.MipLevels = 1; d.Format = sc.fmt; d.SampleDesc.Count = 1; d.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    ID3D12Resource* r = nullptr; if (FAILED(g_dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&r)))) return XR_ERROR_RUNTIME_FAILURE; sc.img.push_back(r); }
  uint64_t h = g_handle++; g_rt[h] = sc; *out = (XrSwapchain)h; return XR_SUCCESS;
}
static XrResult XRAPI_CALL RtDestroySc(XrSwapchain h) { auto it = g_rt.find((uint64_t)h); if (it != g_rt.end()) { for (auto* r : it->second.img) r->Release(); g_rt.erase(it); } return XR_SUCCESS; }
static XrResult XRAPI_CALL RtEnumImages(XrSwapchain h, uint32_t cap, uint32_t* count, XrSwapchainImageBaseHeader* images) {
  auto& sc = g_rt[(uint64_t)h]; *count = (uint32_t)sc.img.size(); if (cap == 0) return XR_SUCCESS; if (cap < *count) return XR_ERROR_SIZE_INSUFFICIENT;
  auto* im = (XrSwapchainImageD3D12KHR*)images; for (uint32_t i = 0; i < *count; ++i) im[i].texture = sc.img[i]; return XR_SUCCESS;
}
static XrResult XRAPI_CALL RtAcquire(XrSwapchain h, const XrSwapchainImageAcquireInfo*, uint32_t* index) { auto& sc = g_rt[(uint64_t)h]; *index = sc.next; sc.next = (sc.next + 1) % (uint32_t)sc.img.size(); return XR_SUCCESS; }
static XrResult XRAPI_CALL RtRelease(XrSwapchain, const XrSwapchainImageReleaseInfo*) { return XR_SUCCESS; }
static XrResult XRAPI_CALL RtCreateRef(XrSession, const XrReferenceSpaceCreateInfo*, XrSpace* sp) { *sp = (XrSpace)0x77; return XR_SUCCESS; }
static XrResult XRAPI_CALL RtEnd(XrSession, const XrFrameEndInfo* info) { ++g_rtEnd; if (info != g_expectInfo) ++g_rtEndBad; return XR_SUCCESS; }
static XrResult XRAPI_CALL RtDestroyInstance(XrInstance) { return XR_SUCCESS; }
static XrResult XRAPI_CALL RtGipa(XrInstance, const char* name, PFN_xrVoidFunction* fn) {
#define F(n, f) if (!strcmp(name, n)) { *fn = (PFN_xrVoidFunction)f; return XR_SUCCESS; }
  F("xrCreateSession", RtCreateSession) F("xrDestroySession", RtDestroySession) F("xrCreateSwapchain", RtCreateSc) F("xrDestroySwapchain", RtDestroySc) F("xrEnumerateSwapchainImages", RtEnumImages)
  F("xrAcquireSwapchainImage", RtAcquire) F("xrReleaseSwapchainImage", RtRelease) F("xrCreateReferenceSpace", RtCreateRef) F("xrEndFrame", RtEnd) F("xrDestroyInstance", RtDestroyInstance)
#undef F
  *fn = nullptr; return XR_ERROR_FUNCTION_UNSUPPORTED;
}
static XrResult XRAPI_CALL RtCreateApiLayerInstance(const XrInstanceCreateInfo*, const XrApiLayerCreateInfo*, XrInstance* inst) { *inst = (XrInstance)0x11; return XR_SUCCESS; }

// ---- the pattern (the same formula is in check_harness.py) -------------------------------------------------------
static inline void Pattern(int layer, int eye, int frame, int x, int y, uint8_t* o) {
  o[0] = uint8_t(x * 3 + frame * 5 + eye * 40); o[1] = uint8_t(y * 7 + layer * 90 + frame); o[2] = uint8_t((x ^ y) + frame * 11); o[3] = layer == 1 ? uint8_t(x + 2 * y + frame * 3) : 255;
}
static ID3D12Resource* g_upload; static UINT64 g_uploadSize;
static void Fill(ID3D12Resource* tex, int layer, int frame) {
  D3D12_RESOURCE_DESC d = tex->GetDesc(); D3D12_RESOURCE_DESC bd = d; bd.DepthOrArraySize = 1; bd.Format = d.Format == DXGI_FORMAT_R8G8B8A8_TYPELESS ? DXGI_FORMAT_R8G8B8A8_UNORM : d.Format; bd.Flags = D3D12_RESOURCE_FLAG_NONE;
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp[2]; UINT64 total = 0, off = 0;
  for (int e = 0; e < 2; ++e) { UINT64 t = 0; g_dev->GetCopyableFootprints(&bd, 0, 1, off, &fp[e], nullptr, nullptr, &t); off = (fp[e].Offset + t + 511) & ~511ull; total = off; }
  if (total > g_uploadSize) { if (g_upload) g_upload->Release(); D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_UPLOAD; D3D12_RESOURCE_DESC b{}; b.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; b.Width = total; b.Height = 1; b.DepthOrArraySize = 1; b.MipLevels = 1; b.SampleDesc.Count = 1; b.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    g_dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &b, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&g_upload)); g_uploadSize = total; }
  uint8_t* base = nullptr; g_upload->Map(0, nullptr, (void**)&base);
  for (int e = 0; e < 2; ++e) for (UINT y = 0; y < d.Height; ++y) { uint8_t* row = base + fp[e].Offset + size_t(y) * fp[e].Footprint.RowPitch; for (UINT x = 0; x < d.Width; ++x) Pattern(layer, e, frame, (int)x, (int)y, row + x * 4); }
  g_upload->Unmap(0, nullptr);
  g_alloc->Reset(); g_list->Reset(g_alloc, nullptr);
  D3D12_RESOURCE_BARRIER b{}; b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; b.Transition.pResource = tex; b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES; b.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET; b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST; g_list->ResourceBarrier(1, &b);
  for (int e = 0; e < 2; ++e) { D3D12_TEXTURE_COPY_LOCATION dst{}; dst.pResource = tex; dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; dst.SubresourceIndex = e; D3D12_TEXTURE_COPY_LOCATION src{}; src.pResource = g_upload; src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; src.PlacedFootprint = fp[e]; g_list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr); }
  b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST; b.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET; g_list->ResourceBarrier(1, &b);
  g_list->Close(); ID3D12CommandList* l[] = {g_list}; g_queue->ExecuteCommandLists(1, l);   // not flushed: the capture layer's copy must be ordered behind this on the queue
}

static bool WriteText(const std::string& path, const std::string& text) { FILE* f = fopen(path.c_str(), "wb"); if (!f) return false; fwrite(text.data(), 1, text.size(), f); fclose(f); return true; }
static bool Exists(const std::string& p) { return GetFileAttributesA(p.c_str()) != INVALID_FILE_ATTRIBUTES; }

int main(int argc, char** argv) {
  if (argc < 3) { fprintf(stderr, "usage: rigtap_harness <layer dll> <out dir> [--hw] [--live]\n"); return 2; }
  bool hw = false, live = false; for (int i = 3; i < argc; ++i) { if (!strcmp(argv[i], "--hw")) hw = true; if (!strcmp(argv[i], "--live")) live = true; } std::string out = argv[2]; CreateDirectoryA(out.c_str(), nullptr);
  IDXGIFactory4* fac = nullptr; CreateDXGIFactory1(IID_PPV_ARGS(&fac)); IDXGIAdapter* ad = nullptr; if (!hw) fac->EnumWarpAdapter(IID_PPV_ARGS(&ad));
  if (FAILED(D3D12CreateDevice(ad, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&g_dev)))) { fprintf(stderr, "no D3D12 device\n"); return 2; }
  D3D12_COMMAND_QUEUE_DESC qd{}; g_dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&g_queue)); g_dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g_alloc));
  g_dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_alloc, nullptr, IID_PPV_ARGS(&g_list)); g_list->Close(); g_dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_fence)); g_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);

  SetEnvironmentVariableA("RIGTAP_DIR", out.c_str()); SetEnvironmentVariableA("RIGTAP_EXTENSIONS", "XR_KHR_D3D12_enable;XR_TEST_one");
  HMODULE mod = LoadLibraryA(argv[1]); if (!mod) { fprintf(stderr, "LoadLibrary failed %lu\n", GetLastError()); return 2; }
  auto negotiate = (PFN_xrNegotiateLoaderApiLayerInterface)GetProcAddress(mod, "xrNegotiateLoaderApiLayerInterface"); if (!negotiate) return 2;
  XrNegotiateLoaderInfo li{}; li.structType = XR_LOADER_INTERFACE_STRUCT_LOADER_INFO; li.structVersion = XR_LOADER_INFO_STRUCT_VERSION; li.structSize = sizeof(li); li.minInterfaceVersion = 1; li.maxInterfaceVersion = XR_CURRENT_LOADER_API_LAYER_VERSION; li.minApiVersion = XR_MAKE_VERSION(1, 0, 0); li.maxApiVersion = XR_CURRENT_API_VERSION;
  XrNegotiateApiLayerRequest req{}; req.structType = XR_LOADER_INTERFACE_STRUCT_API_LAYER_REQUEST; req.structVersion = XR_API_LAYER_INFO_STRUCT_VERSION; req.structSize = sizeof(req);
  if (XR_FAILED(negotiate(&li, "XR_APILAYER_RIG_capture", &req))) return 2;
  int fails = 0; auto check = [&](bool ok, const char* what) { printf("%s: %s\n", ok ? "ok  " : "FAIL", what); if (!ok) ++fails; };
  // null-instance question of the layer above (no loader in this process: the RIGTAP_EXTENSIONS answer)
  { PFN_xrVoidFunction p = nullptr; XrResult r = req.getInstanceProcAddr(XR_NULL_HANDLE, "xrEnumerateInstanceExtensionProperties", &p); uint32_t n = 0; XrExtensionProperties props[4]{};
    bool ok = XR_SUCCEEDED(r) && p && XR_SUCCEEDED(((PFN_xrEnumerateInstanceExtensionProperties)p)(nullptr, 0, &n, nullptr)) && n == 2 && XR_SUCCEEDED(((PFN_xrEnumerateInstanceExtensionProperties)p)(nullptr, 4, &n, props)) && !strcmp(props[0].extensionName, "XR_KHR_D3D12_enable");
    check(ok, "null-instance extension list answered before the instance exists");
    p = (PFN_xrVoidFunction)1; check(req.getInstanceProcAddr(XR_NULL_HANDLE, "xrCreateSession", &p) == XR_ERROR_FUNCTION_UNSUPPORTED && p == nullptr, "null-instance query of an instance function stays unsupported"); }
  XrApiLayerNextInfo ni{}; ni.structType = XR_LOADER_INTERFACE_STRUCT_API_LAYER_NEXT_INFO; ni.structVersion = XR_API_LAYER_NEXT_INFO_STRUCT_VERSION; ni.structSize = sizeof(ni); strcpy_s(ni.layerName, "XR_APILAYER_RIG_capture"); ni.nextGetInstanceProcAddr = RtGipa; ni.nextCreateApiLayerInstance = RtCreateApiLayerInstance;
  XrApiLayerCreateInfo lci{}; lci.structType = XR_LOADER_INTERFACE_STRUCT_API_LAYER_CREATE_INFO; lci.structVersion = XR_API_LAYER_CREATE_INFO_STRUCT_VERSION; lci.structSize = sizeof(lci); lci.nextInfo = &ni;
  XrInstanceCreateInfo ici{XR_TYPE_INSTANCE_CREATE_INFO}; XrInstance inst = XR_NULL_HANDLE;
  if (XR_FAILED(req.createApiLayerInstance(&ici, &lci, &inst))) return 2;
  auto get = [&](const char* n) { PFN_xrVoidFunction p = nullptr; req.getInstanceProcAddr(inst, n, &p); return p; };
  auto createSession = (PFN_xrCreateSession)get("xrCreateSession"); auto destroySession = (PFN_xrDestroySession)get("xrDestroySession"); auto createSc = (PFN_xrCreateSwapchain)get("xrCreateSwapchain"); auto destroySc = (PFN_xrDestroySwapchain)get("xrDestroySwapchain");
  auto acquire = (PFN_xrAcquireSwapchainImage)get("xrAcquireSwapchainImage"); auto release = (PFN_xrReleaseSwapchainImage)get("xrReleaseSwapchainImage"); auto endFrame = (PFN_xrEndFrame)get("xrEndFrame"); auto createRef = (PFN_xrCreateReferenceSpace)get("xrCreateReferenceSpace");
  { PFN_xrVoidFunction p = (PFN_xrVoidFunction)1; check(req.getInstanceProcAddr(inst, "xrLocateViews", &p) == XR_ERROR_FUNCTION_UNSUPPORTED, "a function the runtime does not have is passed through as unsupported"); }
  XrGraphicsBindingD3D12KHR bind{XR_TYPE_GRAPHICS_BINDING_D3D12_KHR}; bind.device = g_dev; bind.queue = g_queue; XrSessionCreateInfo sci{XR_TYPE_SESSION_CREATE_INFO}; sci.next = &bind; XrSession session = XR_NULL_HANDLE; createSession(inst, &sci, &session);
  XrReferenceSpaceCreateInfo rci{XR_TYPE_REFERENCE_SPACE_CREATE_INFO}; rci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL; rci.poseInReferenceSpace.orientation.w = 1; XrSpace space = XR_NULL_HANDLE; createRef(session, &rci, &space);
  // As OFXR does per projection layer: the application's swapchain, a current ring (2), a synthetic ring (1).
  const int W[2] = {live ? 1076 : 269, 0}, H[2] = {live ? 1060 : 265, 0}; const int FW = live ? 1952 : 487, FH = live ? 1928 : 482;   // periphery, focus (the small ones are odd: row pitch)
  XrSwapchain sc[2][4];
  for (int L = 0; L < 2; ++L) for (int k = 0; k < 4; ++k) { XrSwapchainCreateInfo ci{XR_TYPE_SWAPCHAIN_CREATE_INFO}; ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT; ci.format = L == 0 ? 29 : 28; ci.sampleCount = 1; ci.width = L == 0 ? W[0] : FW; ci.height = L == 0 ? H[0] : FH; ci.faceCount = 1; ci.arraySize = 2; ci.mipCount = 1;
    if (XR_FAILED(createSc(session, &ci, &sc[L][k]))) return 2; }
  (void)W; (void)H;
  // rects: not the whole image, so offsets are exercised
  XrRect2Di rect[2] = {{{3, 2}, {W[0] - 5, H[0] - 4}}, {{0, 0}, {FW, FH}}};
  LARGE_INTEGER qf; QueryPerformanceFrequency(&qf); double idleNs = 0, directNs = 0; unsigned idleN = 0;
  const int kFrames = 500; int curSlot = 0, lastKind = -1; std::string expected = "{\"frames\":[";
  for (int f = 0; f < kFrames; ++f) {
    // arm from the trigger file, three times: full; scale 2 + crop; a request above the hard cap
    if (f == 8) WriteText(out + "\\arm.txt", "frames=12\ntag=full\n");
    if (f == 40) WriteText(out + "\\arm.txt", "frames=6\nscale=2\ncrop=0.25,0.25,0.75,0.75\ntag=small\ndelay_ms=30\n");
    if (f == 70) WriteText(out + "\\arm.txt", live ? "frames=500\nscale=4\ntag=cap\n" : "frames=500\ntag=cap\n");
    // a tangent window per eye + a second stream of the whole image at scale 3; then a budget that forces a scale
    if (f == 200) WriteText(out + "\\arm.txt", "frames=6\nwindow0=-0.3,0.3,0.2,-0.2\nwindow1=-0.21,0.35,0.22,-0.2\nscale2=3\ntag=win\n");
    if (f == 260) WriteText(out + "\\arm.txt", live ? "frames=20\nbudget_mb=64\ntag=budget\n" : "frames=20\nbudget_mb=16\ntag=budget\n");
    bool repeat = f > 3 && f % 7 == 3;               // repeated submissions (nothing acquired or released): one or more in every capture window
    bool synthetic = repeat ? (lastKind == 1) : (f % 2 == 1); int k = synthetic ? 3 : 1 + curSlot;
    if (!repeat) { for (int L = 0; L < 2; ++L) { uint32_t idx = 0; XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO}; acquire(sc[L][k], &ai, &idx); Fill(g_rt[(uint64_t)sc[L][k]].img[idx], L, f); if (L == 0) Flush(); XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO}; release(sc[L][k], &ri); }
      lastKind = synthetic ? 1 : 0; if (!synthetic) curSlot ^= 1; }
    static int shownFrame = 0, shownK = 1; if (!repeat) { shownFrame = f; shownK = k; }
    XrCompositionLayerProjectionView views[2][2]; XrCompositionLayerProjection proj[2];
    for (int L = 0; L < 2; ++L) { for (int e = 0; e < 2; ++e) { views[L][e] = {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}; views[L][e].pose.orientation.w = 1; views[L][e].pose.position.x = e ? 0.032f : -0.032f; views[L][e].pose.position.y = 0.001f * shownFrame;
        float half = L == 0 ? 0.9f : 0.25f; views[L][e].fov = {-half + 0.01f * L * (shownFrame % 5), half + 0.01f * L * (shownFrame % 5), half, -half}; views[L][e].subImage.swapchain = sc[L][shownK]; views[L][e].subImage.imageRect = rect[L]; views[L][e].subImage.imageArrayIndex = e; }
      proj[L] = {XR_TYPE_COMPOSITION_LAYER_PROJECTION}; proj[L].layerFlags = L == 1 ? (XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT | XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT) : 0; proj[L].space = space; proj[L].viewCount = 2; proj[L].views = views[L]; }
    XrCompositionLayerQuad quad{XR_TYPE_COMPOSITION_LAYER_QUAD}; quad.space = space; quad.pose.orientation.w = 1; quad.size = {0.3f, 0.2f}; quad.subImage.swapchain = (XrSwapchain)0xDEAD; quad.subImage.imageRect = {{0, 0}, {16, 16}};   // a swapchain the layer never saw
    const XrCompositionLayerBaseHeader* layers[3] = {(XrCompositionLayerBaseHeader*)&proj[0], (XrCompositionLayerBaseHeader*)&proj[1], (XrCompositionLayerBaseHeader*)&quad};
    XrFrameEndInfo info{XR_TYPE_FRAME_END_INFO}; info.displayTime = 1000000000ll + 11111111ll * f; info.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE; info.layerCount = 3; info.layers = layers;
    g_expectInfo = &info; LARGE_INTEGER a, b, c; QueryPerformanceCounter(&a); XrResult r = endFrame(session, &info); QueryPerformanceCounter(&b); RtEnd(session, &info); QueryPerformanceCounter(&c); --g_rtEnd;
    if (XR_FAILED(r)) { check(false, "xrEndFrame returned an error"); break; }
    if (f >= 400) { idleNs += double(b.QuadPart - a.QuadPart) * 1e9 / qf.QuadPart; directNs += double(c.QuadPart - b.QuadPart) * 1e9 / qf.QuadPart; ++idleN; }
    char e[200]; snprintf(e, sizeof(e), "%s{\"f\":%d,\"displayTime\":%lld,\"shownFrame\":%d,\"synthetic\":%s,\"repeat\":%s,\"ring\":%d}", f ? "," : "", f, (long long)info.displayTime, shownFrame, (shownK == 3) ? "true" : "false", repeat ? "true" : "false", shownK); expected += e;
    Flush(); Sleep(f < 400 ? 6 : 0);
  }
  expected += std::string("],\"live\":") + (live ? "true" : "false") + ",\"rects\":[[3,2," + std::to_string(W[0] - 5) + "," + std::to_string(H[0] - 4) + "],[0,0," + std::to_string(FW) + "," + std::to_string(FH) + "]]}";
  WriteText(out + "\\expected.json", expected);
  check(g_rtEnd == (unsigned)kFrames && g_rtEndBad == 0, "every xrEndFrame reached the runtime once with the application's own pointer");
  printf("idle xrEndFrame through the layer: %.0f ns mean over %u calls (the fake runtime's own call: %.0f ns)\n", idleNs / idleN, idleN, directNs / idleN);
  // the writer thread finishes on its own time
  for (int i = 0; i < 300 && !(Exists(out + "\\cap00_full\\done.txt") && Exists(out + "\\cap01_small\\done.txt") && Exists(out + "\\cap02_cap\\done.txt") && Exists(out + "\\cap03_win\\done.txt") && Exists(out + "\\cap04_budget\\done.txt")); ++i) Sleep(100);
  check(Exists(out + "\\cap00_full\\done.txt") && Exists(out + "\\cap01_small\\done.txt") && Exists(out + "\\cap02_cap\\done.txt") && Exists(out + "\\cap03_win\\done.txt") && Exists(out + "\\cap04_budget\\done.txt"), "five captures written to the end (done.txt)");
  for (int L = 0; L < 2; ++L) for (int k = 0; k < 4; ++k) destroySc(sc[L][k]);
  destroySession(session);
  check(g_rt.empty(), "every swapchain destroyed in the runtime");
  printf(fails ? "HARNESS FAILED (%d)\n" : "harness ok\n", fails); return fails ? 1 : 0;
}
