// Offline tests for far_depth.h (the Far depth engine export to Quad Views):
// the DcsVrDepthView layout (264 bytes, the header's offsets), the descriptor
// fill, the CPU capture of cPerView's matrices from DCS's uploads, and the
// whole path on a WARP device through fixation_depth.h's boundary callback:
// four HMD views (focus L an R32G8X24 array slice, focus R R24G8 with
// *SetConstantBuffers1 offsets and its DSV still bound at the copy), a fake
// Quad Views exported by this executable (the module/export seam), the
// WantedMask gating, the copied texels against DCS's depth, this frame's
// jittered gProj in the push, the generation ring, DCS's bindings identical
// around every copy, identity skips (FOV mismatch, HMD offset), the layer
// unloaded mid-session, the 120-frame release, and the hot-reload release on
// the render thread (and from the uninstall when the render thread is idle).
#pragma once

// Layout per dcsvr_depth_export.h (pack 8).
static_assert(sizeof(DcsVrDepthView) == 264, "DcsVrDepthView 264 bytes");
static_assert(offsetof(DcsVrDepthView, structSize) == 0 && offsetof(DcsVrDepthView, version) == 4 &&
                  offsetof(DcsVrDepthView, hmdView) == 8 && offsetof(DcsVrDepthView, flags) == 12 &&
                  offsetof(DcsVrDepthView, engineFrame) == 16 && offsetof(DcsVrDepthView, displayTime) == 24 &&
                  offsetof(DcsVrDepthView, generation) == 32 && offsetof(DcsVrDepthView, format) == 36 &&
                  offsetof(DcsVrDepthView, texture) == 40 && offsetof(DcsVrDepthView, texWidth) == 48 &&
                  offsetof(DcsVrDepthView, texHeight) == 52 && offsetof(DcsVrDepthView, arraySlice) == 56 &&
                  offsetof(DcsVrDepthView, sampleCount) == 60 && offsetof(DcsVrDepthView, viewport) == 64 &&
                  offsetof(DcsVrDepthView, proj) == 88 && offsetof(DcsVrDepthView, viewInv) == 152 &&
                  offsetof(DcsVrDepthView, jitterPx) == 216 && offsetof(DcsVrDepthView, fovTan) == 224 &&
                  offsetof(DcsVrDepthView, outputWidth) == 240 && offsetof(DcsVrDepthView, outputHeight) == 244 &&
                  offsetof(DcsVrDepthView, preOverlay) == 248 && offsetof(DcsVrDepthView, preOverlayFormat) == 256 &&
                  offsetof(DcsVrDepthView, preOverlaySize) == 260,
              "DcsVrDepthView offsets");

// ---------------------------------------------------------------------------
// The fake Quad Views (exports of this executable; far_depth.h's seam)
// ---------------------------------------------------------------------------

namespace fartest {
struct Push {
  DcsVrDepthView d{};
  uint32_t result = 0;
  DWORD thread = 0;
  D3D11_TEXTURE2D_DESC td{};
};
struct RelRec {
  uint32_t gen = 0;
  DWORD thread = 0;
  int heldWithOurs = 0;  // held textures whose refcount showed a second owner (ours) at the release
  int held = 0;
};
std::atomic<uint32_t> g_mask{0}, g_api{1};
std::atomic<int> g_wantedCalls{0};
DWORD g_renderThread = 0;
ID3D11Device* g_qvDevice = nullptr;  // the "application device" (identity)
std::mutex g_m;
std::vector<Push> g_pushes;
std::vector<RelRec> g_rels;
ID3D11Texture2D* g_held[4] = {};
ID3D11Texture2D* g_heldPre[4] = {};
uint32_t g_heldGen[4] = {};

ULONG Refs(IUnknown* u) {
  u->AddRef();
  return u->Release();
}

// As dcsvr_depth_import.h Push: the checks, AddRef, the previous one dropped.
uint32_t FakePush(const DcsVrDepthView* d) {
  Push p;
  p.thread = GetCurrentThreadId();
  if (!d || d->structSize < sizeof(DcsVrDepthView) || d->version < 1 || !d->texture || d->hmdView > 3) return 0;
  p.d = *d;
  if (g_renderThread && p.thread != g_renderThread) p.result = DCSVR_DEPTH_PUSH_NOT_RENDER_THREAD;
  else if (!(g_mask.load() & (1u << d->hmdView))) p.result = DCSVR_DEPTH_PUSH_NOT_WANTED;
  else if (d->sampleCount != 1) p.result = DCSVR_DEPTH_PUSH_MSAA;
  if (!p.result) {
    d->texture->GetDesc(&p.td);
    ID3D11Device* owner = nullptr;
    d->texture->GetDevice(&owner);
    if (owner) owner->Release();
    if (p.td.SampleDesc.Count != 1) p.result = DCSVR_DEPTH_PUSH_MSAA;
    else if (p.td.Format != static_cast<DXGI_FORMAT>(d->format) || !(p.td.BindFlags & D3D11_BIND_SHADER_RESOURCE))
      p.result = DCSVR_DEPTH_PUSH_FORMAT;
    else if (d->arraySlice != 0 || p.td.ArraySize != 1 || p.td.MipLevels != 1) p.result = DCSVR_DEPTH_PUSH_LAYOUT;
    else if (owner != g_qvDevice) p.result = DCSVR_DEPTH_PUSH_FOREIGN_DEVICE;
    else p.result = DCSVR_DEPTH_PUSH_ACCEPTED;
  }
  // v1.1 pre-overlay: present with the flag, single-sample, SRV, one slice and
  // mip, at least the copied box (2w wide with PRE_POST_OVERLAY, which needs
  // HAS_PRE_OVERLAY).
  if (p.result == DCSVR_DEPTH_PUSH_ACCEPTED && (d->flags & DCSVR_DEPTH_PRE_POST_OVERLAY) &&
      !(d->flags & DCSVR_DEPTH_HAS_PRE_OVERLAY))
    p.result = DCSVR_DEPTH_PUSH_LAYOUT;
  if (p.result == DCSVR_DEPTH_PUSH_ACCEPTED && (d->flags & DCSVR_DEPTH_HAS_PRE_OVERLAY)) {
    D3D11_TEXTURE2D_DESC pd{};
    if (!d->preOverlay) {
      p.result = DCSVR_DEPTH_PUSH_NULL_OR_SIZE;
    } else {
      d->preOverlay->GetDesc(&pd);
      if (pd.SampleDesc.Count != 1) p.result = DCSVR_DEPTH_PUSH_MSAA;
      else if (!(pd.BindFlags & D3D11_BIND_SHADER_RESOURCE)) p.result = DCSVR_DEPTH_PUSH_FORMAT;
      else if (pd.ArraySize != 1 || pd.MipLevels != 1 || pd.Width < (d->preOverlaySize & 0xffff) ||
               pd.Height < (d->preOverlaySize >> 16))
        p.result = DCSVR_DEPTH_PUSH_LAYOUT;
      else if ((d->flags & DCSVR_DEPTH_PRE_POST_OVERLAY) && pd.Width < 2 * (d->preOverlaySize & 0xffff))
        p.result = DCSVR_DEPTH_PUSH_LAYOUT;
    }
  }
  std::lock_guard<std::mutex> lock(g_m);
  if (p.result == DCSVR_DEPTH_PUSH_ACCEPTED) {
    d->texture->AddRef();
    if (g_held[d->hmdView]) g_held[d->hmdView]->Release();
    g_held[d->hmdView] = d->texture;
    g_heldGen[d->hmdView] = d->generation;
    ID3D11Texture2D* pre = (d->flags & DCSVR_DEPTH_HAS_PRE_OVERLAY) ? d->preOverlay : nullptr;
    if (pre) pre->AddRef();
    if (g_heldPre[d->hmdView]) g_heldPre[d->hmdView]->Release();
    g_heldPre[d->hmdView] = pre;
  }
  g_pushes.push_back(p);
  return p.result;
}

void FakeRelease(uint32_t gen) {
  RelRec r;
  r.gen = gen;
  r.thread = GetCurrentThreadId();
  std::lock_guard<std::mutex> lock(g_m);
  for (int v = 0; v < 4; ++v)
    if (g_held[v] && g_heldGen[v] == gen) {
      ++r.held;
      if (Refs(g_held[v]) >= 2) ++r.heldWithOurs;
      g_held[v]->Release();
      g_held[v] = nullptr;
      if (g_heldPre[v]) g_heldPre[v]->Release();
      g_heldPre[v] = nullptr;
    }
  g_rels.push_back(r);
}
}  // namespace fartest

extern "C" __declspec(dllexport) uint32_t DcsQvCullTest_QVDepthApi() { return fartest::g_api.load(); }
extern "C" __declspec(dllexport) uint32_t DcsQvCullTest_QVDepthWanted() {
  fartest::g_wantedCalls++;
  return fartest::g_mask.load();
}
extern "C" __declspec(dllexport) uint32_t DcsQvCullTest_QVDepth(const DcsVrDepthView* d) { return fartest::FakePush(d); }
extern "C" __declspec(dllexport) void DcsQvCullTest_QVDepthRelease(uint32_t g) { fartest::FakeRelease(g); }

namespace fartest {
using UsFnT = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, UINT, const D3D11_BOX*, const void*,
                                       UINT, UINT);
UsFnT g_realUs = nullptr;
std::atomic<int> g_variantCalls{0};
// Stands in for the other variant d3d11 switches the slot to.
void STDMETHODCALLTYPE VariantUs(ID3D11DeviceContext* c, ID3D11Resource* r, UINT sub, const D3D11_BOX* b,
                                 const void* src, UINT rp, UINT dp) {
  g_variantCalls++;
  g_realUs(c, r, sub, b, src, rp, dp);
}
}  // namespace fartest

namespace fartest {

using namespace fixdepth;
using fdtest::FakePass;
using fdtest::MakePass;
using fdtest::Rel;
using fdtest::View;

// ---------------------------------------------------------------------------
// Pure parts
// ---------------------------------------------------------------------------

void TestFill() {
  fdexport::CaptureInfo c;
  c.hmdView = 3;
  c.engineFrame = 77;
  c.generation = 9;
  c.format = DXGI_FORMAT_R32G8X24_TYPELESS;
  c.texture = reinterpret_cast<ID3D11Texture2D*>(static_cast<uintptr_t>(0x1000));
  c.texW = 1176;
  c.texH = 1164;
  c.vp = D3D11_VIEWPORT{0.25f, 0, 1176, 1164, 0, 1};
  fdtest::MakeProj(c.proj, -0.6, 0.7, 0.65, -0.7, 0.02, 0, true);
  for (int i = 0; i < 16; ++i) c.viewInv[i] = static_cast<float>(i);
  c.viewInvValid = true;
  c.afterWater = true;
  DcsVrDepthView d;
  const bool ok = fdexport::FillView(c, d);
  const uint32_t want = DCSVR_DEPTH_VALID | DCSVR_DEPTH_REVERSE_Z | DCSVR_DEPTH_INFINITE_FAR | DCSVR_DEPTH_AFTER_WATER |
                        DCSVR_DEPTH_HAS_STENCIL | DCSVR_DEPTH_VIEWINV_VALID | DCSVR_DEPTH_IDENTITY_BY_COPYTOHMD;
  Check(ok && d.structSize == 264 && d.version == 1 && d.hmdView == 3 && d.flags == want && d.engineFrame == 77 &&
            d.displayTime == 0 && d.generation == 9 && d.format == DXGI_FORMAT_R32G8X24_TYPELESS &&
            d.texture == c.texture && d.texWidth == 1176 && d.texHeight == 1164 && d.arraySlice == 0 &&
            d.sampleCount == 1,
        "far depth: descriptor header fields and flags (reverse-Z infinite, after water, stencil, viewInv, ordinal)");
  Check(d.viewport[0] == 0.25f && d.viewport[2] == 1176 && d.viewport[3] == 1164 && d.viewport[4] == 0 &&
            d.viewport[5] == 1 && !memcmp(d.proj, c.proj, sizeof(d.proj)) && d.viewInv[13] == 13.0f &&
            d.jitterPx[0] == 0 && d.jitterPx[1] == 0 && d.outputWidth == 0 && d.outputHeight == 0 && !d.preOverlay &&
            d.preOverlayFormat == 0 && d.preOverlaySize == 0,
        "far depth: viewport (incl. depth range), proj, viewInv; no jitter, output size, pre-overlay (set later)");
  Check(fdtest::Near(d.fovTan[0], -0.6, 1e-5) && fdtest::Near(d.fovTan[1], 0.7, 1e-5) &&
            fdtest::Near(d.fovTan[2], 0.65, 1e-5) && fdtest::Near(d.fovTan[3], -0.7, 1e-5),
        "far depth: fovTan = DcsFov of gProj (left, right, up, down)");
  c.format = DXGI_FORMAT_R32_TYPELESS;
  c.afterWater = false;
  c.fallback = true;
  c.viewInvValid = false;
  fdtest::MakeProj(c.proj, -0.6, 0.7, 0.65, -0.7, 0.02, 300, true);
  Check(fdexport::FillView(c, d) && (d.flags & DCSVR_DEPTH_FALLBACK_CAPTURE) && !(d.flags & DCSVR_DEPTH_AFTER_WATER) &&
            !(d.flags & DCSVR_DEPTH_HAS_STENCIL) && !(d.flags & DCSVR_DEPTH_INFINITE_FAR) &&
            !(d.flags & DCSVR_DEPTH_VIEWINV_VALID) && !(d.flags & DCSVR_DEPTH_JITTER_KNOWN) &&
            !(d.flags & DCSVR_DEPTH_HAS_PRE_OVERLAY) && d.viewInv[13] == 0,
        "far depth: R32 (no stencil), finite far, fallback capture; JITTER_KNOWN and HAS_PRE_OVERLAY not set by the fill");
  float bad[16] = {};
  memcpy(c.proj, bad, sizeof(bad));
  Check(!fdexport::FillView(c, d), "far depth: a non-perspective projection is not pushed");
}

// The CPU capture of cPerView's matrices from DCS's uploads.
void TestUploads() {
  fdexport::ResetFrameState();
  float P[16], V[16];
  fdtest::MakeProj(P, -0.5, 0.5, 0.5, -0.5, 0.02, 0, true);
  P[8] += 0.0031f;  // a jitter
  for (int i = 0; i < 16; ++i) V[i] = 100.0f + i;
  // A cPerView struct at constant 16 (byte 256) of the buffer, column-major.
  std::vector<float> buf(16 * 4 + 512 / 4, 0.0f);
  float* base = buf.data() + 16 * 4;
  for (int r = 0; r < 4; ++r)
    for (int c = 0; c < 4; ++c) {
      base[192 / 4 + c * 4 + r] = V[r * 4 + c];
      base[256 / 4 + c * 4 + r] = P[r * 4 + c];
    }
  ID3D11Buffer* cb = reinterpret_cast<ID3D11Buffer*>(static_cast<uintptr_t>(0x5000));
  float p[16], v[16];
  fdexport::BeginGBufferPass();
  fdexport::NoteUpload(cb, 0, nullptr, buf.data());
  Check(!fdexport::TakeProj(cb, 16, 32, p, v), "far depth uploads: an unknown buffer is learned, not used");
  fdexport::BeginGBufferPass();
  fdexport::NoteUpload(cb, 0, nullptr, buf.data());
  Check(fdexport::TakeProj(cb, 16, 32, p, v) && !memcmp(p, P, sizeof(p)) && !memcmp(v, V, sizeof(v)),
        "far depth uploads: gProj/gViewInv of a known cPerView (offset 16 constants), transposed to P[r][c]");
  fdexport::BeginGBufferPass();
  Check(!fdexport::TakeProj(cb, 16, 32, p, v), "far depth uploads: no upload in this pass -> no projection");
  D3D11_BOX part{0, 0, 0, 16 * 16 + 200, 1, 1};
  fdexport::NoteUpload(cb, 0, &part, buf.data());
  Check(!fdexport::TakeProj(cb, 16, 32, p, v), "far depth uploads: a partial update without the matrices is ignored");
  D3D11_BOX cover{16 * 16 + 192, 0, 0, 16 * 16 + 320, 1, 1};
  fdexport::NoteUpload(cb, 0, &cover, base + 192 / 4);
  Check(fdexport::TakeProj(cb, 16, 32, p, v) && !memcmp(p, P, sizeof(p)),
        "far depth uploads: a boxed update covering the matrices is used (source at the box's left)");
  Check(!fdexport::TakeProj(cb, 0, 4096, p, v), "far depth uploads: the same buffer at another offset is another entry");
  fdexport::ResetFrameState();
}

// ---------------------------------------------------------------------------
// The device path
// ---------------------------------------------------------------------------

// cPerView bytes as fdtest::MakeCb lays them out (gViewInv 192, gProj 256).
std::vector<float> CbData(const View& v, const float P[16]) {
  std::vector<float> f((v.cbFirst * 16 + 512) / 4 + 64, 0.0f);
  float* base = f.data() + v.cbFirst * 4;
  auto put = [&](int byteOff, const float m[16]) {
    for (int r = 0; r < 4; ++r)
      for (int c = 0; c < 4; ++c) base[byteOff / 4 + c * 4 + r] = m[r * 4 + c];
  };
  const float viewInv[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, static_cast<float>(v.eyeX), 1.5f, 0, 1};
  put(192, viewInv);
  put(256, P);
  f.resize((f.size() * 4 + 15) / 16 * 4);
  return f;
}

// v1.1 copy point (far_depth.h 5b): DCS-like overlay draws. A full-screen
// triangle that composites another texture into the target (the tone-mapped
// scene), and a 6-vertex "HUD" rectangle over target pixel (120, 110).
struct OvlShaders {
  ID3D11VertexShader* fsVs = nullptr;
  ID3D11VertexShader* hudVs = nullptr;
  ID3D11PixelShader* copyPs = nullptr;
  ID3D11PixelShader* hudPs = nullptr;
  ID3D11RasterizerState* rs = nullptr;
  void Release() { Rel(fsVs), Rel(hudVs), Rel(copyPs), Rel(hudPs), Rel(rs); }
};
bool MakeOvlShaders(ID3D11Device* dev, OvlShaders& o) {
  HMODULE dll = LoadLibraryW(L"d3dcompiler_47.dll");
  auto compile = dll ? reinterpret_cast<decltype(&D3DCompile)>(GetProcAddress(dll, "D3DCompile")) : nullptr;
  if (!compile) return false;
  static const char kSrc[] =
      "float4 fs(uint id : SV_VertexID) : SV_Position {\n"
      "  float2 uv = float2((id << 1) & 2, id & 2);\n"
      "  return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);\n"
      "}\n"
      "float4 hud(uint id : SV_VertexID) : SV_Position {\n"
      "  const float2 p[6] = {float2(-0.4, 0.4), float2(0, 0.4), float2(-0.4, 0), float2(-0.4, 0), float2(0, 0.4),\n"
      "                       float2(0, 0)};\n"
      "  return float4(p[id], 0, 1);\n"
      "}\n"
      "Texture2D<float4> src : register(t0);\n"
      "float4 cp(float4 pos : SV_Position) : SV_Target { return src.Load(int3(pos.xy, 0)); }\n"
      "float4 hp(float4 pos : SV_Position) : SV_Target { return float4(0, 1, 0, 1); }\n";
  auto blob = [&](const char* entry, const char* target) -> ID3DBlob* {
    ID3DBlob* b = nullptr;
    ID3DBlob* err = nullptr;
    compile(kSrc, sizeof(kSrc) - 1, "ovl", nullptr, nullptr, entry, target, 0, 0, &b, &err);
    if (err) {
      printf("     overlay shader %s: %s\n", entry, static_cast<const char*>(err->GetBufferPointer()));
      err->Release();
    }
    return b;
  };
  ID3DBlob* b[4] = {blob("fs", "vs_5_0"), blob("hud", "vs_5_0"), blob("cp", "ps_5_0"), blob("hp", "ps_5_0")};
  const bool ok = b[0] && b[1] && b[2] && b[3];
  if (ok) {
    dev->CreateVertexShader(b[0]->GetBufferPointer(), b[0]->GetBufferSize(), nullptr, &o.fsVs);
    dev->CreateVertexShader(b[1]->GetBufferPointer(), b[1]->GetBufferSize(), nullptr, &o.hudVs);
    dev->CreatePixelShader(b[2]->GetBufferPointer(), b[2]->GetBufferSize(), nullptr, &o.copyPs);
    dev->CreatePixelShader(b[3]->GetBufferPointer(), b[3]->GetBufferSize(), nullptr, &o.hudPs);
    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    dev->CreateRasterizerState(&rd, &o.rs);
  }
  for (ID3DBlob* x : b)
    if (x) x->Release();
  return ok && o.fsVs && o.hudVs && o.copyPs && o.hudPs && o.rs;
}

// True when one of our draw hooks is in the context's table.
bool DrawHooksIn(void** table) {
  for (int k = 0; k < fdexport::kDkCount; ++k) {
    const int slot = DcsQv_ContextSlot(fdexport::kDrawNames[k]);
    if (slot >= 0 && table[slot] == DrawHookOf(k)) return true;
  }
  return false;
}

// Our module set up as Install does, minus the execute hook (no GraphicsCore).
void FakeInstall() {
  g_fault = false;
  g_workerStop = false;
  fdexport::g_stopping = false;
  fdexport::g_releaseReq = false;
  fdexport::SeedGeneration();
  fdexport::ResetFrameState();
  g_worker = std::thread(WorkerLoop);
  OpenMappings();
  g_installed = true;
}

int HeldNow() {
  std::lock_guard<std::mutex> lock(g_m);
  int n = 0;
  for (auto* t : g_held) n += t != nullptr;
  return n;
}

void TestDebugLayer() {
  ID3D11Device* dev = nullptr;
  ID3D11DeviceContext* c = nullptr;
  const D3D_FEATURE_LEVEL want[] = {D3D_FEATURE_LEVEL_11_1};
  if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_DEBUG, want, 1,
                               D3D11_SDK_VERSION, &dev, nullptr, &c))) {
    printf("SKIP far depth debug layer: D3D11 SDK layers not installed (Graphics Tools)\n");
    return;
  }
  ID3D11InfoQueue* q = nullptr;
  dev->QueryInterface(__uuidof(ID3D11InfoQueue), reinterpret_cast<void**>(&q));
  View v;
  v.w = 64;
  v.h = 64;
  fdtest::MakeProj(v.P, -0.5, 0.5, 0.5, -0.5, 0.02, 0, true);
  const bool made = fdtest::MakeDepth(dev, c, v, DXGI_FORMAT_R32G8X24_TYPELESS, 2, 1);
  D3D11_TEXTURE2D_DESC d{};
  if (made) v.tex->GetDesc(&d);
  d.ArraySize = 1;
  d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  ID3D11Texture2D* dst = nullptr;
  if (made) dev->CreateTexture2D(&d, nullptr, &dst);
  if (q) q->ClearStoredMessages();
  const UINT64 n0 = q ? q->GetNumStoredMessages() : 0;
  if (dst) {
    c->OMSetRenderTargets(0, nullptr, v.dsv);  // DCS's DSV still bound on the source
    c->CopySubresourceRegion(dst, 0, 0, 0, 0, v.tex, D3D11CalcSubresource(0, 1, 1), nullptr);
    c->Flush();
  }
  const UINT64 n1 = q ? q->GetNumStoredMessages() : 0;
  Check(made && dst && q && n1 == n0,
        "far depth (debug layer): whole-subresource copy from a bound DSV's array slice: no debug-layer message");
  c->ClearState();
  if (dst) dst->Release();
  Rel(v.dsv), Rel(v.tex);
  if (q) q->Release();
  c->Release();
  dev->Release();
}

void TestPipeline() {
  ID3D11Device* dev = nullptr;
  ID3D11DeviceContext* imm = nullptr;
  const D3D_FEATURE_LEVEL want[] = {D3D_FEATURE_LEVEL_11_1};
  if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, want, 1, D3D11_SDK_VERSION, &dev, nullptr,
                               &imm))) {
    printf("SKIP far depth pipeline: no WARP 11.1 device\n");
    return;
  }
  ID3D11DeviceContext1* c = nullptr;
  imm->QueryInterface(__uuidof(ID3D11DeviceContext1), reinterpret_cast<void**>(&c));
  if (!c || !CompileShaders()) {
    Rel(c), Rel(imm), Rel(dev);
    return;
  }
  // HMD views: periphery L/R 96x90 (R32), focus L 256x250 (R32G8X24 array,
  // slice 1), focus R 256x250 (R24G8, cPerView at constant 16).
  View v[4];
  const double fovs[4][4] = {{-1.4, 1.1, 1.2, -1.3}, {-1.1, 1.4, 1.2, -1.3}, {-0.65, 0.55, 0.6, -0.62},
                             {-0.55, 0.65, 0.6, -0.62}};
  float baseP[4][16];
  bool made = true;
  for (int i = 0; i < 4; ++i) {
    v[i].w = i < 2 ? 96 : 256;
    v[i].h = i < 2 ? 90 : 250;
    v[i].eyeX = (i & 1) ? 0.032 : -0.032;
    fdtest::MakeProj(v[i].P, fovs[i][0], fovs[i][1], fovs[i][2], fovs[i][3], 0.02, 0, true);
    memcpy(baseP[i], v[i].P, sizeof(baseP[i]));
    const DXGI_FORMAT f = i == 2 ? DXGI_FORMAT_R32G8X24_TYPELESS
                          : i == 3 ? DXGI_FORMAT_R24G8_TYPELESS
                                   : DXGI_FORMAT_R32_TYPELESS;
    made = made && fdtest::MakeDepth(dev, c, v[i], f, i == 2 ? 2 : 1, i == 2 ? 1 : 0) &&
           fdtest::MakeCb(dev, v[i], i == 3 ? 16 : 0);
  }
  View def;  // DCS's default frame buffer between passes
  def.w = def.h = 64;
  fdtest::MakeProj(def.P, -1, 1, 1, -1, 0.05, 0, true);
  made = made && fdtest::MakeDepth(dev, c, def, DXGI_FORMAT_R32_TYPELESS, 1, 0);
  // DCS-like CS state that must survive.
  ID3D11ComputeShader* dcsCs = nullptr;
  dev->CreateComputeShader(g_blob[0].data(), g_blob[0].size(), nullptr, &dcsCs);
  D3D11_BUFFER_DESC cd{};
  cd.ByteWidth = 1024;
  cd.Usage = D3D11_USAGE_DEFAULT;
  cd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
  ID3D11Buffer* cbA = nullptr;
  dev->CreateBuffer(&cd, nullptr, &cbA);
  made = made && dcsCs && cbA;
  Check(made, "far depth: synthetic HMD views (R32G8X24 array slice, R24G8, R32) and DCS-like state created");
  if (!made) return;

  // The module: far depth only (no cone, no grid), the fake Quad Views.
  const std::wstring oldMod = fdexport::g_module;
  fdexport::g_module = L"test_main.exe";
  fdexport::SetExportPrefix("DcsQvCullTest_QV");
  wchar_t gname[64];
  swprintf_s(gname, L"Local\\FarTestGaze_%lu", GetCurrentProcessId());
  g_gazeName = gname;
  HANDLE gm = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, 4096, gname);
  void* gview = gm ? MapViewOfFile(gm, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(GazeOut)) : nullptr;
  GazeOut g{};
  g.flags = kGazeValid;
  g.viewCount = 4;
  for (int i = 0; i < 4; ++i)
    for (int k = 0; k < 4; ++k) g.fovTan[i][k] = static_cast<float>(fovs[i][k]);
  if (gview) SeqWrite(gview, g);
  g_coneOn = false;
  g_gridOn = false;
  fdexport::g_on = true;
  fdexport::g_slots = 2;
  // FarDepthCoverageMode: hdr is the module default (the atomic's initial
  // value and Tick's); the pipeline below runs explicitly in overlay mode
  // until 6f, exactly as before the default changed.
  Check(fdexport::g_covMode.load() == fdexport::kCovHdr && fdexport::HdrMode() && fdexport::ViewHdr(2) &&
            fdexport::ViewHdr(3),
        "far depth coverage: hdr is the module default (initial value / Tick default), both focus views in hdr");
  fdexport::g_covMode = fdexport::kCovOverlay;
  Check(!fdexport::ViewHdr(2) && !fdexport::ViewHdr(3), "far depth coverage: explicit overlay -> no view in hdr");
  g_hmdOffset = 0;
  g_verify = 1;  // the module's own live state check around each copy
  ID3D11Device* oldDev = shadowinst::g_device;
  shadowinst::g_device = dev;
  ptiming::g_topTid = GetCurrentThreadId();
  g_renderThread = GetCurrentThreadId();
  g_qvDevice = dev;
  g_mask = 0;
  FakeInstall();

  FakePass casc = MakePass("CascadeShadowPass0"), lights = MakePass("LightsBuffersPass"),
           gbuf = MakePass("GBufferPass"), resolve = MakePass("DepthResolverPass"),
           seaPass = MakePass("WaterSeaPass"), hmdPass = MakePass("CopyToHMDPass");
  bool stateSame = true;
  int stateChecks = 0;
  auto ob = [&](FakePass* p, bool begin) {
    const uint64_t n0 = fdexport::g_copies.load();
    const fdtest::Snap before = fdtest::TakeSnap(c);
    OnBoundary(p, begin, 0);
    if (fdexport::g_copies.load() != n0) {
      ++stateChecks;
      if (!(before == fdtest::TakeSnap(c))) stateSame = false;
    }
  };
  auto bindDefault = [&] {
    ID3D11RenderTargetView* none = nullptr;
    c->OMSetRenderTargets(1, &none, def.dsv);
    D3D11_VIEWPORT vp{0, 0, 64, 64, 0, 1};
    c->RSSetViewports(1, &vp);
  };
  float frameP[4][16];  // the gProj DCS uploaded in each view's G-buffer this frame
  int jit = 0;
  bool sea = true, upload = true;
  // v1.1 pre-overlay: the focus views' colour targets (after DLSS, output
  // size): focus L R8G8B8A8_TYPELESS drawn through an sRGB RTV, viewport
  // inside it; focus R R11G11B10_FLOAT (no typeless family); an MSAA one.
  FakePass tone = MakePass("ToneMappingPass"), ovl = MakePass("OverlayLDRPass"), gui = MakePass("GUIPass");
  FakePass dlss = MakePass("DLSSPass"), ohdr = MakePass("OverlayHDRPass"), hist = MakePass("HistogramPass"),
           res2 = MakePass("DepthResolverPass2");
  ID3D11Texture2D* colTex[2] = {};
  ID3D11RenderTargetView* colRtv[2] = {};
  ID3D11Texture2D* msTex = nullptr;
  ID3D11RenderTargetView* msRtv = nullptr;
  {
    D3D11_TEXTURE2D_DESC td{};
    td.Width = 300;
    td.Height = 280;
    td.MipLevels = td.ArraySize = 1;
    td.SampleDesc.Count = 1;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    td.Format = DXGI_FORMAT_R8G8B8A8_TYPELESS;
    dev->CreateTexture2D(&td, nullptr, &colTex[0]);
    D3D11_RENDER_TARGET_VIEW_DESC rd{};
    rd.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    if (colTex[0]) dev->CreateRenderTargetView(colTex[0], &rd, &colRtv[0]);
    td.Format = DXGI_FORMAT_R11G11B10_FLOAT;
    dev->CreateTexture2D(&td, nullptr, &colTex[1]);
    if (colTex[1]) dev->CreateRenderTargetView(colTex[1], nullptr, &colRtv[1]);
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 4;
    td.BindFlags = D3D11_BIND_RENDER_TARGET;
    dev->CreateTexture2D(&td, nullptr, &msTex);
    if (msTex) dev->CreateRenderTargetView(msTex, nullptr, &msRtv);
  }
  Check(colRtv[0] && colRtv[1] && msRtv, "far depth pre-overlay: colour targets (R8G8B8A8 typeless + sRGB RTV, "
                                         "R11G11B10, 4x MSAA) created");
  // 5b: the tone-mapped scene in its own texture (composited into the target
  // by OverlayLDRPass's first draw), the overlay shaders.
  ID3D11Texture2D* srcTex[2] = {};
  ID3D11RenderTargetView* srcRtv[2] = {};
  ID3D11ShaderResourceView* srcSrv[2] = {};
  OvlShaders osh;
  {
    D3D11_TEXTURE2D_DESC td{};
    td.Width = 300;
    td.Height = 280;
    td.MipLevels = td.ArraySize = 1;
    td.SampleDesc.Count = 1;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    for (int k = 0; k < 2; ++k) {
      dev->CreateTexture2D(&td, nullptr, &srcTex[k]);
      if (srcTex[k]) {
        dev->CreateRenderTargetView(srcTex[k], nullptr, &srcRtv[k]);
        dev->CreateShaderResourceView(srcTex[k], nullptr, &srcSrv[k]);
      }
    }
  }
  // 5d: the focus views' HDR buffers (DLSS output, output size), R16G16B16A16_FLOAT.
  ID3D11Texture2D* hdrTex[2] = {};
  ID3D11RenderTargetView* hdrRtv[2] = {};
  ID3D11ShaderResourceView* hdrSrv[2] = {};
  {
    D3D11_TEXTURE2D_DESC td{};
    td.Width = 300;
    td.Height = 280;
    td.MipLevels = td.ArraySize = 1;
    td.SampleDesc.Count = 1;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    td.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    for (int k = 0; k < 2; ++k) {
      dev->CreateTexture2D(&td, nullptr, &hdrTex[k]);
      if (hdrTex[k]) {
        dev->CreateRenderTargetView(hdrTex[k], nullptr, &hdrRtv[k]);
        dev->CreateShaderResourceView(hdrTex[k], nullptr, &hdrSrv[k]);
      }
    }
  }
  const bool oshOk = MakeOvlShaders(dev, osh) && srcSrv[0] && srcSrv[1] && srcRtv[0] && srcRtv[1] && hdrSrv[0] &&
                     hdrSrv[1] && hdrRtv[0] && hdrRtv[1];
  Check(oshOk, "far depth copy point: overlay shaders (full-screen composite, HUD rectangle) and scene sources");
  const D3D11_VIEWPORT colVp[2] = {{6, 4, 288, 270, 0, 1}, {0, 0, 300, 280, 0, 1}};
  const D3D11_RECT hudRect{100, 100, 140, 120}, guiRect{20, 200, 60, 230};
  bool overlay = false, guiFirst = false, guiStart = false, msaa3 = false, switchUs = false, flushProbe = false;
  bool ovlStateOk = true, variantAfterPass = false;
  // ovlMode 0: the HUD by ClearView, the scene cleared into the target by
  // ToneMappingPass (no draw); 1: composite first (ToneMappingPass writes its
  // own texture, OverlayLDRPass draws it full-screen into the target, then the
  // HUD); 2: the scene in the target from ToneMappingPass, OverlayLDRPass
  // draws only the HUD (6 vertices).
  int ovlMode = 0;
  bool guiAfter = false;
  // hdrSeq: DLSSPass (writes the HDR buffer) -> OverlayHDRPass (HUD into HDR)
  // -> HistogramPass -> ToneMappingPass (full-screen draw reading HDR into the
  // LDR target) -> DepthResolverPass2 -> OverlayLDRPass (no draw, ovlMode 3).
  bool toneOn = true;  // outside hdrSeq: a ToneMappingPass around the scene (5e: a module / setting without one)
  bool hdrSeq = false, dlssOn = true;  // a GUIPass drawing into the view's target after its OverlayLDRPass
  bool cmpStateOk = true, hooksInPass = false;
  int flushSwitches = 0;
  const int usSlot = DcsQv_ContextSlot("UpdateSubresource");
  void** ctxTable = *reinterpret_cast<void***>(c);
  auto sceneColour = [&](int view, float out[4]) {
    out[0] = static_cast<float>((jit * 37 + view * 11) % 97) / 97.0f;
    out[1] = 0.5f;
    out[2] = view == 2 ? 0.25f : 0.75f;
    out[3] = 1.0f;
  };
  auto frame = [&](const int order[4]) {
    ++jit;
    ob(&casc, true);
    ob(&casc, false);
    if (overlay && guiStart) {  // a GUIPass at the frame start on focus L's target: before the view rendered
      ob(&gui, true);
      c->OMSetRenderTargets(1, &colRtv[0], nullptr);
      c->RSSetViewports(1, &colVp[0]);
      const float g[4] = {1, 0, 1, 1};
      c->ClearView(colRtv[0], g, &guiRect, 1);
      ob(&gui, false);
    }
    for (int k = 0; k < 4; ++k) {
      const int i = order[k];
      // Halton-like jitter in gProj's P20/P21 (+-0.5 texel).
      float P[16];
      memcpy(P, baseP[i], sizeof(P));
      const double jx = ((jit * 7) % 16) / 16.0 - 0.5, jy = ((jit * 11) % 16) / 16.0 - 0.5;
      P[8] += static_cast<float>(2.0 * jx / v[i].w);
      P[9] += static_cast<float>(2.0 * jy / v[i].h);
      memcpy(frameP[k], P, sizeof(P));
      const std::vector<float> data = CbData(v[i], P);
      ob(&lights, true);
      ob(&lights, false);
      bindDefault();
      for (int pass = 0; pass < 3; ++pass) {  // cockpit, main, decal
        ob(&gbuf, true);
        c->CSSetShader(dcsCs, nullptr, 0);
        c->CSSetConstantBuffers(0, 1, &cbA);
        ID3D11RenderTargetView* none = nullptr;
        c->OMSetRenderTargets(1, &none, v[i].dsv);  // pushFrameBuffer
        D3D11_VIEWPORT vp{0, 0, static_cast<float>(v[i].w), static_cast<float>(v[i].h), 0, 1};
        c->RSSetViewports(1, &vp);
        if (upload) c->UpdateSubresource(v[i].cb, 0, nullptr, data.data(), 0, 0);  // the binder: cPerView upload
        if (flushProbe && pass == 1) {  // a Flush inside the pass, then a copy-class call (d3d11 may switch slots)
          void* before = ctxTable[usSlot];
          c->Flush();
          c->UpdateSubresource(v[i].cb, 0, nullptr, data.data(), 0, 0);
          if (ctxTable[usSlot] != before) ++flushSwitches;
        }
        if (switchUs && pass == 1 && i == 2) {  // d3d11 switches the entry to another variant mid-pass
          if (!g_realUs) g_realUs = reinterpret_cast<UsFnT>(fixdepth::g_usOrig);
          ctxTable[usSlot] = reinterpret_cast<void*>(&VariantUs);
        }
        if (v[i].cbNum)
          c->VSSetConstantBuffers1(7, 1, &v[i].cb, &v[i].cbFirst, &v[i].cbNum);
        else
          c->VSSetConstantBuffers(7, 1, &v[i].cb);
        if (i != 3) bindDefault();  // popFrameBuffer; focus R keeps its DSV bound (copy while bound)
        ob(&gbuf, false);
        if (switchUs && i == 2 && pass == 2) variantAfterPass = ctxTable[usSlot] == reinterpret_cast<void*>(&VariantUs);
      }
      ob(&resolve, true);
      ob(&resolve, false);
      if (sea) {
        ob(&seaPass, true);
        ob(&seaPass, false);
      }
      if (overlay && i >= 2) {
        ID3D11RenderTargetView* rt = (i == 3 && msaa3) ? msRtv : colRtv[i - 2];
        const D3D11_VIEWPORT& cv = colVp[i - 2];
        float sc[4];
        sceneColour(i, sc);
        if (hdrSeq) {
          if (dlssOn) {
            ob(&dlss, true);  // the DLSS output (this frame's scene, HDR)
            c->ClearRenderTargetView(hdrRtv[i - 2], sc);
            ob(&dlss, false);
          } else {
            c->ClearRenderTargetView(hdrRtv[i - 2], sc);  // no upscaler: the scene already in HDR
          }
          ob(&ohdr, true);  // HUD_MFD_after_DLSS: the HUD into the HDR buffer
          c->OMSetRenderTargets(1, &hdrRtv[i - 2], nullptr);
          c->RSSetViewports(1, &cv);
          const float hh[4] = {0, 4, 0, 1};
          c->ClearView(hdrRtv[i - 2], hh, &hudRect, 1);
          ob(&ohdr, false);
          ob(&hist, true);
          ob(&hist, false);
          ob(&tone, true);  // tone mapping: HDR -> the LDR target, one full-screen draw
          c->OMSetRenderTargets(1, &rt, nullptr);
          c->RSSetViewports(1, &cv);
          c->IASetInputLayout(nullptr);
          c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
          c->RSSetState(osh.rs);
          c->VSSetShader(osh.fsVs, nullptr, 0);
          c->PSSetShader(osh.copyPs, nullptr, 0);
          c->PSSetShaderResources(0, 1, &hdrSrv[i - 2]);
          c->Draw(4 - 1, 0);
          ID3D11ShaderResourceView* none = nullptr;
          c->PSSetShaderResources(0, 1, &none);
          c->RSSetState(nullptr);
          ob(&tone, false);
          ob(&res2, true);
          ob(&res2, false);
        } else {
          if (toneOn) ob(&tone, true);  // the view's tone-mapped (post-DLSS) image
          c->ClearRenderTargetView(ovlMode == 1 ? srcRtv[i - 2] : rt, sc);
          if (toneOn) ob(&tone, false);
        }
        if (guiFirst) {  // a GUIPass after the view rendered, before its OverlayLDRPass
          ob(&gui, true);
          c->OMSetRenderTargets(1, &rt, nullptr);
          c->RSSetViewports(1, &cv);
          const float g[4] = {1, 0, 1, 1};
          c->ClearView(rt, g, &guiRect, 1);
          ob(&gui, false);
        }
        ob(&ovl, true);
        c->OMSetRenderTargets(1, &rt, nullptr);
        c->RSSetViewports(1, &cv);
        {  // DCS's own bindings went through unchanged
          ID3D11RenderTargetView* got = nullptr;
          c->OMGetRenderTargets(1, &got, nullptr);
          D3D11_VIEWPORT gv{};
          UINT nv = 1;
          c->RSGetViewports(&nv, &gv);
          ID3D11ComputeShader* cs = nullptr;
          c->CSGetShader(&cs, nullptr, nullptr);
          ovlStateOk = ovlStateOk && got == rt && nv == 1 && !memcmp(&gv, &cv, sizeof(gv)) && cs == dcsCs;
          if (got) got->Release();
          if (cs) cs->Release();
        }
        if (ovlMode == 3) {
          // nothing drawn in the LDR overlay pass (the HUD went into HDR)
        } else if (ovlMode == 0) {
          const float hud[4] = {0, 1, 0, 1};
          c->ClearView(rt, hud, &hudRect, 1);  // HUD symbology
        } else {
          c->IASetInputLayout(nullptr);
          c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
          c->RSSetState(osh.rs);
          if (ovlMode == 1) {  // the scene composite: full-screen, from the tone-mapped texture
            c->VSSetShader(osh.fsVs, nullptr, 0);
            c->PSSetShader(osh.copyPs, nullptr, 0);
            c->PSSetShaderResources(0, 1, &srcSrv[i - 2]);
            const fdtest::Snap sb = fdtest::TakeSnap(c);
            ID3D11PixelShader* psb = nullptr;
            c->PSGetShader(&psb, nullptr, nullptr);
            hooksInPass = hooksInPass || DrawHooksIn(ctxTable);
            c->Draw(3, 0);
            ID3D11PixelShader* psa = nullptr;
            c->PSGetShader(&psa, nullptr, nullptr);
            ID3D11ShaderResourceView* srva = nullptr;
            c->PSGetShaderResources(0, 1, &srva);
            D3D11_PRIMITIVE_TOPOLOGY topo{};
            c->IAGetPrimitiveTopology(&topo);
            cmpStateOk = cmpStateOk && sb == fdtest::TakeSnap(c) && psa == psb && srva == srcSrv[i - 2] &&
                         topo == D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
            Rel(psb), Rel(psa), Rel(srva);
          }
          c->VSSetShader(osh.hudVs, nullptr, 0);
          c->PSSetShader(osh.hudPs, nullptr, 0);
          c->Draw(6, 0);  // HUD symbology
          ID3D11ShaderResourceView* none = nullptr;
          c->PSSetShaderResources(0, 1, &none);
          c->RSSetState(nullptr);
        }
        ob(&ovl, false);
        if (guiAfter) {
          ob(&gui, true);
          c->OMSetRenderTargets(1, &rt, nullptr);
          c->RSSetViewports(1, &cv);
          const float g[4] = {1, 0, 1, 1};
          c->ClearView(rt, g, &guiRect, 1);
          ob(&gui, false);
        }
      }
      ob(&hmdPass, true);
      ob(&hmdPass, false);
    }
    c->Flush();
  };
  const int order[4] = {0, 1, 2, 3};
  auto pushesNow = [&] {
    std::lock_guard<std::mutex> lock(g_m);
    return g_pushes.size();
  };
  auto lastPush = [&](uint32_t view) {
    std::lock_guard<std::mutex> lock(g_m);
    for (size_t k = g_pushes.size(); k-- > 0;)
      if (g_pushes[k].d.hmdView == view) return g_pushes[k];
    return Push{};
  };

  // 1. Quad Views wants nothing: nothing runs.
  const uint64_t up0 = fdexport::g_uploadsSeen.load(), np0 = fdexport::g_skipNoProj.load();
  for (int f = 0; f < 6; ++f) frame(order);
  Check(g_wantedCalls.load() >= 5 && fdexport::g_copies.load() == 0 && fdexport::g_texCreates.load() == 0 &&
            pushesNow() == 0 && fdexport::g_uploadsSeen.load() == up0 && !fdexport::HaveTextures() &&
            fdexport::g_skipNoProj.load() == np0,
        "far depth: Wanted() = 0 -> no copy, no texture, no push, no upload hook (one Wanted call per frame)");
  const int wc0 = g_wantedCalls.load();
  for (int f = 0; f < 3; ++f) frame(order);
  Check(g_wantedCalls.load() - wc0 == 3, "far depth: exactly one Wanted() call per frame");

  // 2. Both focus views wanted, depth after WaterSeaPass.
  g_mask = fdexport::kFocusMask;
  const std::vector<uint8_t> src2 = fdtest::ReadTex(dev, c, v[2].tex), src3 = fdtest::ReadTex(dev, c, v[3].tex);
  frame(order);  // learns the cPerView buffers at the first GBufferPass end of each view
  const size_t p0 = pushesNow();
  Check(fdexport::g_skipNoProj.load() == np0 && p0 == 2 && fdexport::g_uploadsSeen.load() > up0,
        "far depth: cPerView learned at a view's first GBufferPass end, its gProj taken from the next pass's upload");
  std::vector<ID3D11Texture2D*> ring2;
  bool sameFrame = true, sameGen = true, projOk = true, framesStep = true;
  uint64_t lastFrame = 0;
  for (int f = 0; f < 6; ++f) {
    const size_t before = pushesNow();
    frame(order);
    const Push a = lastPush(2), b = lastPush(3);
    if (pushesNow() != before + 2) projOk = false;
    sameFrame = sameFrame && a.d.engineFrame == b.d.engineFrame;
    sameGen = sameGen && a.d.generation == b.d.generation;
    projOk = projOk && !memcmp(a.d.proj, frameP[2], sizeof(a.d.proj)) && !memcmp(b.d.proj, frameP[3], sizeof(b.d.proj));
    if (f) framesStep = framesStep && a.d.engineFrame == lastFrame + 1;
    lastFrame = a.d.engineFrame;
    ring2.push_back(a.d.texture);
  }
  Push a = lastPush(2), b = lastPush(3);
  printf("     far depth: pushes %zu, accepted %llu, copies %llu, uploads seen %llu, flags 0x%x/0x%x, gen %u, "
         "P20 %+.6f\n",
         pushesNow(), static_cast<unsigned long long>(fdexport::g_accepted.load()),
         static_cast<unsigned long long>(fdexport::g_copies.load()),
         static_cast<unsigned long long>(fdexport::g_uploadsSeen.load()), a.d.flags, b.d.flags, a.d.generation,
         a.d.proj[8]);
  Check(projOk && a.result == DCSVR_DEPTH_PUSH_ACCEPTED && b.result == DCSVR_DEPTH_PUSH_ACCEPTED &&
            a.thread == g_renderThread,
        "far depth: views 2 and 3 pushed every frame on the render thread, accepted, with THIS frame's jittered gProj");
  Check(sameFrame && sameGen && framesStep,
        "far depth: both focus views carry the same engineFrame and generation; engineFrame steps by one");
  const uint32_t flagsWant = DCSVR_DEPTH_VALID | DCSVR_DEPTH_REVERSE_Z | DCSVR_DEPTH_INFINITE_FAR |
                             DCSVR_DEPTH_AFTER_WATER | DCSVR_DEPTH_HAS_STENCIL | DCSVR_DEPTH_VIEWINV_VALID |
                             DCSVR_DEPTH_IDENTITY_BY_COPYTOHMD;
  Check(a.d.flags == flagsWant && b.d.flags == flagsWant && a.d.displayTime == 0 && a.d.structSize == 264 &&
            a.d.format == DXGI_FORMAT_R32G8X24_TYPELESS && b.d.format == DXGI_FORMAT_R24G8_TYPELESS &&
            a.d.texWidth == 256 && a.d.texHeight == 250 && a.d.arraySlice == 0 && a.d.sampleCount == 1 &&
            a.d.viewport[2] == 256 && a.d.viewport[3] == 250 && a.d.viewport[5] == 1 && !a.d.preOverlay,
        "far depth: flags, formats (R32G8X24 / R24G8 typeless), size, viewport incl. depth range, displayTime 0");
  Check(a.d.viewInv[12] < 0 && b.d.viewInv[12] > 0 && fdtest::Near(a.d.fovTan[0], -0.65, 0.01) &&
            fdtest::Near(b.d.fovTan[1], 0.65, 0.01),
        "far depth: gViewInv (left camera left of right) and fovTan from this frame's gProj");
  Check(a.td.ArraySize == 1 && a.td.MipLevels == 1 && a.td.SampleDesc.Count == 1 &&
            a.td.Usage == D3D11_USAGE_DEFAULT && a.td.BindFlags == D3D11_BIND_SHADER_RESOURCE,
        "far depth: our texture: ArraySize 1, MipLevels 1, single-sample, DEFAULT, BIND_SHADER_RESOURCE");
  {
    ID3D11Texture2D* t2 = nullptr;
    ID3D11Texture2D* t3 = nullptr;
    {
      std::lock_guard<std::mutex> lock(g_m);
      t2 = g_held[2];
      t3 = g_held[3];
      if (t2) t2->AddRef();
      if (t3) t3->AddRef();
    }
    const std::vector<uint8_t> got2 = t2 ? fdtest::ReadTex(dev, c, t2) : std::vector<uint8_t>{};
    const std::vector<uint8_t> got3 = t3 ? fdtest::ReadTex(dev, c, t3) : std::vector<uint8_t>{};
    const size_t sliceBytes = static_cast<size_t>(256) * 250 * 8;
    const bool eq2 = src2.size() == 2 * sliceBytes && got2.size() == sliceBytes &&
                     !memcmp(got2.data(), src2.data() + sliceBytes, sliceBytes);
    const bool eq3 = !got3.empty() && got3 == src3;
    Check(eq2 && eq3, "far depth: copied texels (depth and stencil bytes) equal DCS's: array slice 1 -> our "
                      "subresource 0 (R32G8X24), R24G8 copied while its DSV is bound");
    Rel(t2), Rel(t3);
  }
  Check(ring2.size() == 6 && ring2[0] != ring2[1] && ring2[0] == ring2[2] && ring2[1] == ring2[3] &&
            fdexport::g_texCreates.load() == 4,
        "far depth: 2-slot ring per view alternating by engine frame (4 textures)");
  Check(stateChecks >= 12 && stateSame && fdexport::g_stateChecks.load() >= 12 && fdexport::g_stateDiffs.load() == 0,
        "far depth: DCS's CS/OM/VS b7/viewport bindings identical across every copy (test snapshot and module check)");
  Check(fdtest::ReadTex(dev, c, v[2].tex) == src2 && fdtest::ReadTex(dev, c, v[3].tex) == src3,
        "far depth: DCS's depth targets bit-identical after the copies");

  // 3. Mask gating: left focus only.
  g_mask = 0b0100;
  const size_t pm = pushesNow();
  frame(order);
  frame(order);
  {
    std::lock_guard<std::mutex> lock(g_m);
    bool only2 = g_pushes.size() == pm + 2;
    for (size_t k = pm; k < g_pushes.size(); ++k) only2 = only2 && g_pushes[k].d.hmdView == 2;
    Check(only2, "far depth: WantedMask 0b0100 -> only HMD view 2 copied and pushed");
  }
  g_mask = fdexport::kFocusMask;

  // 4. No WaterSeaPass: fallback capture (at the CopyToHMDPass begin).
  sea = false;
  frame(order);
  frame(order);
  a = lastPush(2);
  Check((a.d.flags & DCSVR_DEPTH_FALLBACK_CAPTURE) && !(a.d.flags & DCSVR_DEPTH_AFTER_WATER) &&
            a.result == DCSVR_DEPTH_PUSH_ACCEPTED,
        "far depth: without WaterSeaPass -> FALLBACK_CAPTURE, no AFTER_WATER");
  sea = true;

  // 5. Identity: views rendered in another order (FOV mismatch), a learned HMD offset.
  const int swapped[4] = {0, 1, 3, 2};
  const uint64_t mm0 = fdexport::g_skipMismatch.load();
  size_t pi = pushesNow();
  frame(swapped);
  frame(swapped);
  Check(fdexport::g_skipMismatch.load() >= mm0 + 4 && pushesNow() == pi,
        "far depth: a view whose FOV matches the other focus view (GazeOut) is never pushed");
  g_hmdOffset = 1;
  const uint64_t id0 = fdexport::g_skipIdentity.load();
  pi = pushesNow();
  frame(order);
  Check(fdexport::g_skipIdentity.load() >= id0 + 2 && pushesNow() == pi,
        "far depth: a learned HMD-view offset (ordinal uncertain) -> not pushed");
  g_hmdOffset = 0;

  // 5b. No cPerView upload inside the pass (its content is not known on the CPU): not pushed.
  upload = false;
  const uint64_t nu0 = fdexport::g_skipNoProj.load();
  pi = pushesNow();
  frame(order);
  Check(fdexport::g_skipNoProj.load() == nu0 + 2 && pushesNow() == pi,
        "far depth: cPerView not uploaded in the pass -> no CPU gProj -> not pushed");
  upload = true;

  // 6. One slot (VRAM pressure): the same texture every frame.
  fdexport::g_slots = 1;
  frame(order);
  const Push s1 = lastPush(2);
  frame(order);
  Check(lastPush(2).d.texture == s1.d.texture && s1.result == DCSVR_DEPTH_PUSH_ACCEPTED,
        "far depth: FarDepthExportSlots=1 -> one texture per view");
  fdexport::g_slots = 2;

  // 6b. v1.1 pre-overlay: each focus view's colour copied at its first
  // overlay-class pass, before the HUD; pushed with the depth of that frame.
  overlay = true;
  guiStart = true;
  const uint64_t prc0 = fdexport::g_preCopies.load();
  for (int f = 0; f < 3; ++f) frame(order);
  auto heldPre = [&](int view) {
    std::lock_guard<std::mutex> lock(g_m);
    ID3D11Texture2D* t = g_heldPre[view];
    if (t) t->AddRef();
    return t;
  };
  // All texels of `pre`'s pre-overlay half (x < w; the whole texture when it
  // has no post half) equal the target's pixel at (x, y) (outside the HUD).
  auto preIsScene = [&](ID3D11Texture2D* pre, int view, int x, int y, bool* hudSeen) {
    const std::vector<uint8_t> rt = fdtest::ReadTex(dev, c, colTex[view - 2]);
    const std::vector<uint8_t> got = pre ? fdtest::ReadTex(dev, c, pre) : std::vector<uint8_t>{};
    if (rt.empty() || got.empty()) return false;
    D3D11_TEXTURE2D_DESC pd{};
    pre->GetDesc(&pd);
    const UINT boxW = view == 2 ? 288 : 300;
    const UINT w = pd.Width >= 2 * boxW ? boxW : pd.Width;
    uint32_t ref, hudPx;
    memcpy(&ref, rt.data() + (static_cast<size_t>(y) * 300 + x) * 4, 4);
    memcpy(&hudPx, rt.data() + (static_cast<size_t>(110) * 300 + 120) * 4, 4);
    *hudSeen = hudPx != ref;
    for (UINT yy = 0; yy < pd.Height; ++yy)
      for (UINT xx = 0; xx < w; ++xx) {
        uint32_t px;
        memcpy(&px, got.data() + (static_cast<size_t>(yy) * pd.Width + xx) * 4, 4);
        if (px != ref) return false;
      }
    return true;
  };
  // The post-overlay half of `pre` (at (w, 0)) equals the target's box as it
  // is now (after the overlays; nothing draws into it until the next frame).
  auto postIsTarget = [&](ID3D11Texture2D* pre, int view) {
    const std::vector<uint8_t> rt = fdtest::ReadTex(dev, c, colTex[view - 2]);
    const std::vector<uint8_t> got = pre ? fdtest::ReadTex(dev, c, pre) : std::vector<uint8_t>{};
    if (rt.empty() || got.empty()) return false;
    D3D11_TEXTURE2D_DESC pd{};
    pre->GetDesc(&pd);
    const D3D11_VIEWPORT& cv = colVp[view - 2];
    const UINT w = static_cast<UINT>(cv.Width), h = static_cast<UINT>(cv.Height);
    const UINT x0 = static_cast<UINT>(cv.TopLeftX), y0 = static_cast<UINT>(cv.TopLeftY);
    if (pd.Width < 2 * w || pd.Height < h) return false;
    for (UINT yy = 0; yy < h; ++yy)
      if (memcmp(got.data() + (static_cast<size_t>(yy) * pd.Width + w) * 4,
                 rt.data() + (static_cast<size_t>(yy + y0) * 300 + x0) * 4, static_cast<size_t>(w) * 4))
        return false;
    return true;
  };
  {
    const Push a2 = lastPush(2), a3 = lastPush(3);
    printf("     pre-overlay: copies %llu, flags 0x%x/0x%x, formats %u/%u, sizes 0x%x/0x%x\n",
           static_cast<unsigned long long>(fdexport::g_preCopies.load() - prc0), a2.d.flags, a3.d.flags,
           a2.d.preOverlayFormat, a3.d.preOverlayFormat, a2.d.preOverlaySize, a3.d.preOverlaySize);
    Check(fdexport::g_preCopies.load() - prc0 == 6 && (a2.d.flags & DCSVR_DEPTH_HAS_PRE_OVERLAY) &&
              (a3.d.flags & DCSVR_DEPTH_HAS_PRE_OVERLAY) && a2.result == DCSVR_DEPTH_PUSH_ACCEPTED &&
              a3.result == DCSVR_DEPTH_PUSH_ACCEPTED && a2.d.engineFrame == a3.d.engineFrame,
          "far depth pre-overlay: one copy per focus view per frame at its OverlayLDRPass, pushed with that frame's "
          "depth (a frame-start GUIPass on the target before the view rendered is ignored)");
    Check(a2.d.preOverlayFormat == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB && a2.d.preOverlaySize == (288u | (270u << 16)) &&
              a3.d.preOverlayFormat == DXGI_FORMAT_R11G11B10_FLOAT && a3.d.preOverlaySize == (300u | (280u << 16)),
          "far depth pre-overlay: preOverlayFormat = the RTV's format, preOverlaySize = the viewport box w | h << 16");
    ID3D11Texture2D* p2 = heldPre(2);
    ID3D11Texture2D* p3 = heldPre(3);
    D3D11_TEXTURE2D_DESC d2{}, d3{};
    if (p2) p2->GetDesc(&d2);
    if (p3) p3->GetDesc(&d3);
    Check(p2 && p3 && d2.Format == DXGI_FORMAT_R8G8B8A8_TYPELESS && d2.Width == 2 * 288 && d2.Height == 270 &&
              d3.Format == DXGI_FORMAT_R11G11B10_FLOAT && d3.Width == 2 * 300 && d3.Height == 280 && d2.ArraySize == 1 &&
              d2.MipLevels == 1 && d2.SampleDesc.Count == 1 && d2.Usage == D3D11_USAGE_DEFAULT &&
              d2.BindFlags == D3D11_BIND_SHADER_RESOURCE && d3.BindFlags == D3D11_BIND_SHADER_RESOURCE,
          "far depth pre-overlay: our texture: the typeless family (R8G8B8A8) or the target's own format "
          "(R11G11B10_FLOAT), exactly 2w x h (pre and post), one slice and mip, single-sample, DEFAULT, "
          "SHADER_RESOURCE");
    bool hud2 = false, hud3 = false;
    const bool s2 = preIsScene(p2, 2, 7, 5, &hud2), s3 = preIsScene(p3, 3, 1, 1, &hud3);
    Check(s2 && s3 && hud2 && hud3,
          "far depth pre-overlay: copied texels = this frame's scene colour over the whole box (sRGB target and "
          "R11G11B10), without the HUD the overlay pass drew after the copy");
    Rel(p2), Rel(p3);
  }
  Check(ovlStateOk && fdexport::g_stateDiffs.load() == 0,
        "far depth pre-overlay: DCS's RTV, viewport and CS state as DCS set them; module state check no difference");
  // A GUIPass that draws into the target after the view rendered: the copy is
  // made there, before the GUI (the GUI is not background).
  guiFirst = true;
  frame(order);
  frame(order);
  {
    ID3D11Texture2D* p2 = heldPre(2);
    bool hud = false;
    const std::vector<uint8_t> rt = fdtest::ReadTex(dev, c, colTex[0]);
    uint32_t guiPx = 0, ref = 0;
    memcpy(&guiPx, rt.data() + (static_cast<size_t>(210) * 300 + 30) * 4, 4);
    memcpy(&ref, rt.data() + (static_cast<size_t>(5) * 300 + 7) * 4, 4);
    Check(preIsScene(p2, 2, 7, 5, &hud) && guiPx != ref && (lastPush(2).d.flags & DCSVR_DEPTH_HAS_PRE_OVERLAY),
          "far depth pre-overlay: a GUIPass first on the view's target -> copied at the GUIPass, no GUI, no HUD");
    Rel(p2);
  }
  guiFirst = false;
  // MSAA target: no pre-overlay, the depth alone.
  msaa3 = true;
  const uint64_t ms0 = fdexport::g_preSkipMsaa.load();
  frame(order);
  Check(fdexport::g_preSkipMsaa.load() > ms0 && !(lastPush(3).d.flags & DCSVR_DEPTH_HAS_PRE_OVERLAY) &&
            lastPush(3).result == DCSVR_DEPTH_PUSH_ACCEPTED && (lastPush(2).d.flags & DCSVR_DEPTH_HAS_PRE_OVERLAY),
        "far depth pre-overlay: an MSAA target -> no HAS_PRE_OVERLAY, the depth pushed alone (accepted)");
  msaa3 = false;
  // Pre-overlay off: pushed right after the depth copy, no flag.
  fdexport::g_preOn = false;
  frame(order);
  Check(!(lastPush(2).d.flags & DCSVR_DEPTH_HAS_PRE_OVERLAY) && lastPush(2).result == DCSVR_DEPTH_PUSH_ACCEPTED,
        "far depth: FarDepthPreOverlay=0 -> pushed at the depth copy without the pre-overlay");
  fdexport::g_preOn = true;
  frame(order);
  Check((lastPush(2).d.flags & DCSVR_DEPTH_HAS_PRE_OVERLAY) != 0, "far depth pre-overlay: back on");

  // 6c. "arm missed": d3d11 switches UpdateSubresource's entry mid-pass. The
  // hook takes the new entry and re-arms; the view is still pushed and the
  // switched variant is what stays in the table.
  {
    const uint64_t ra0 = fdexport::g_usRearms.load(), sa0 = fdexport::g_skipArm.load();
    const size_t pp0 = pushesNow();
    switchUs = true;
    frame(order);
    switchUs = false;
    const bool variantKept = variantAfterPass;  // the frame-end Flush may switch it again (d3d11's own)
    if (g_realUs && ctxTable[usSlot] == reinterpret_cast<void*>(&VariantUs))
      ctxTable[usSlot] = reinterpret_cast<void*>(g_realUs);  // back to d3d11's entry
    Check(fdexport::g_usRearms.load() > ra0 && g_variantCalls.load() > 0 && variantKept &&
              fdexport::g_skipArm.load() == sa0 && pushesNow() == pp0 + 2 &&
              lastPush(2).result == DCSVR_DEPTH_PUSH_ACCEPTED,
          "far depth: upload entry switched mid-pass -> re-armed onto the new variant, view still pushed (no 'arm "
          "missed'), the variant left in the table at the pass end");
    flushProbe = true;
    const uint64_t sb0 = fdexport::g_skipArm.load();
    frame(order);
    flushProbe = false;
    printf("     Flush inside a GBufferPass: UpdateSubresource entry switched %d time(s) on WARP\n", flushSwitches);
    Check(fdexport::g_skipArm.load() == sb0 && lastPush(2).result == DCSVR_DEPTH_PUSH_ACCEPTED,
          "far depth: a Flush inside the G-buffer pass costs no push");
  }

  // 6f. HDR coverage (far_depth.h 5d): the HUD drawn into the HDR buffer
  // after DLSS; hdr mode brackets DLSSPass end .. ToneMappingPass begin.
  if (oshOk) {
    // Pixel (x, y) of a tightly packed texture read back by ReadTex.
    auto px = [](const std::vector<uint8_t>& v, UINT rowW, UINT bpp, UINT x, UINT y) {
      uint64_t r = 0;
      memcpy(&r, v.data() + (static_cast<size_t>(y) * rowW + x) * bpp, bpp);
      return r;
    };
    // view's texture: (0,0) half uniform = the HDR scene (no HUD), (w,0) half =
    // the HDR box now (with the HUD); bpp 8.
    auto checkHdr = [&](int view, bool* preNoHud, bool* postHud, bool* sizeFmt) {
      ID3D11Texture2D* p = heldPre(view);
      const std::vector<uint8_t> got = p ? fdtest::ReadTex(dev, c, p) : std::vector<uint8_t>{};
      const std::vector<uint8_t> hdr = fdtest::ReadTex(dev, c, hdrTex[view - 2]);
      D3D11_TEXTURE2D_DESC pd{};
      if (p) p->GetDesc(&pd);
      const D3D11_VIEWPORT& cv = colVp[view - 2];
      const UINT w = static_cast<UINT>(cv.Width), h = static_cast<UINT>(cv.Height);
      const UINT x0 = static_cast<UINT>(cv.TopLeftX), y0 = static_cast<UINT>(cv.TopLeftY);
      const Push a = lastPush(view);
      *sizeFmt = p && pd.Format == DXGI_FORMAT_R16G16B16A16_TYPELESS && pd.Width == 2 * w && pd.Height == h &&
                 a.d.preOverlayFormat == DXGI_FORMAT_R16G16B16A16_FLOAT && a.d.preOverlaySize == (w | (h << 16)) &&
                 (a.d.flags & DCSVR_DEPTH_HAS_PRE_OVERLAY) && (a.d.flags & DCSVR_DEPTH_PRE_POST_OVERLAY) &&
                 a.result == DCSVR_DEPTH_PUSH_ACCEPTED;
      *preNoHud = *postHud = false;
      if (got.empty() || hdr.empty() || pd.Width < 2 * w) {
        Rel(p);
        return;
      }
      const uint64_t scene = px(hdr, 300, 8, 1 + x0, 1 + y0), hudPx = px(hdr, 300, 8, 120, 110);
      bool pre = hudPx != scene, post = true;
      for (UINT y = 0; y < h; ++y)
        for (UINT x = 0; x < w; ++x) {
          pre = pre && px(got, pd.Width, 8, x, y) == scene;
          post = post && px(got, pd.Width, 8, w + x, y) == px(hdr, 300, 8, x0 + x, y0 + y);
        }
      *preNoHud = pre;
      *postHud = post && px(got, pd.Width, 8, w + 120 - x0, 110 - y0) == hudPx;
      Rel(p);
    };
    fdexport::ResetCopyPoint();
    hdrSeq = true;
    ovlMode = 3;
    fdexport::g_covMode = fdexport::kCovHdr;
    const uint64_t hp0 = fdexport::g_hdrPre.load(), hq0 = fdexport::g_hdrPost.load(),
                   sd0 = fdexport::g_stateDiffs.load(), sc0 = fdexport::g_stateChecks.load();
    frame(order);  // learns the HDR source (ToneMappingPass PS SRV0)
    // The pre textures change format (LDR -> R16G16B16A16): each slot is
    // recreated under a new generation, which releases the layer's other view
    // too; two frames until both slots are HDR.
    frame(order);
    frame(order);
    bool ok = true, sz = true;
    for (int f = 0; f < 3; ++f) {
      frame(order);
      for (int view = 2; view <= 3; ++view) {
        bool hp = false, hq = false, hz = false;
        checkHdr(view, &hp, &hq, &hz);
        ok = ok && hp && hq;
        sz = sz && hz;
      }
    }
    printf("     hdr coverage: pre %llu post %llu, flags 0x%x, format %u\n",
           static_cast<unsigned long long>(fdexport::g_hdrPre.load() - hp0),
           static_cast<unsigned long long>(fdexport::g_hdrPost.load() - hq0), lastPush(2).d.flags,
           lastPush(2).d.preOverlayFormat);
    Check(ok, "far depth hdr coverage: (0,0) = the DLSS output box without the HUD, (w,0) = the same box at the "
              "ToneMappingPass begin with the HUD drawn into HDR");
    Check(sz && fdexport::g_hdrPre.load() - hp0 == 10 && fdexport::g_hdrPost.load() - hq0 == 10,
          "far depth hdr coverage: source learnt from ToneMappingPass PS SRV0 in the first frame; then texture 2w x h "
          "R16G16B16A16_TYPELESS, preOverlayFormat R16G16B16A16_FLOAT, HAS_PRE | PRE_POST, accepted");
    Check(fdexport::g_stateChecks.load() >= sc0 + 12 && fdexport::g_stateDiffs.load() == sd0 &&
              !DrawHooksIn(ctxTable),
          "far depth hdr coverage: DCS's bindings identical around both copies; no draw hook left");
    // No DLSSPass (DLAA / no upscaler): the pre copy at the OverlayHDRPass begin.
    dlssOn = false;
    const uint64_t fb0 = fdexport::g_hdrFallback.load();
    frame(order);
    {
      bool hp = false, hq = false, hz = false;
      checkHdr(2, &hp, &hq, &hz);
      Check(hp && hq && hz && fdexport::g_hdrFallback.load() == fb0 + 2,
            "far depth hdr coverage: no DLSSPass -> pre at the OverlayHDRPass begin, same result");
    }
    dlssOn = true;
    // Overlay mode on the same sequence: both copies already have the HUD.
    fdexport::g_covMode = fdexport::kCovOverlay;
    for (int f = 0; f < 3; ++f) frame(order);  // back to LDR textures (recreated, see above)
    {
      ID3D11Texture2D* p = heldPre(2);
      bool hud = false;
      const bool preClean = preIsScene(p, 2, 7, 5, &hud);
      Check(p && !preClean && postIsTarget(p, 2) && (lastPush(2).d.flags & DCSVR_DEPTH_PRE_POST_OVERLAY) &&
                lastPush(2).d.preOverlayFormat == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,
            "far depth coverage overlay mode, HUD drawn into HDR: the LDR pre copy already has the HUD (live)");
      Rel(p);
    }
    // The trace window: DLSSPass end .. CopyToHMDPass begin.
    {
      std::lock_guard<std::mutex> lock(fdexport::g_statMutex);
      fdexport::g_ovTrace.clear();
    }
    fdexport::g_ovTraceLeft = 1;
    frame(order);
    std::vector<std::string> tl;
    {
      std::lock_guard<std::mutex> lock(fdexport::g_statMutex);
      tl.swap(fdexport::g_ovTrace);
    }
    auto has = [&](const char* a, const char* b2) {
      for (const std::string& l : tl)
        if (l.find(a) != std::string::npos && (!b2 || l.find(b2) != std::string::npos)) return true;
      return false;
    };
    for (const std::string& l : tl) printf("     trace: %s\n", l.c_str());
    Check(has("view 2: window opens at DLSSPass end", "draw hooks in") &&
              has("view 2 OverlayHDRPass end: draws 0", nullptr) && has("view 2 HistogramPass end", nullptr) &&
              has("view 2 ToneMappingPass PS SRV0: res", "format 10") &&
              has("view 2 ToneMappingPass draw 1: Draw(3) x1", "format 29") &&
              has("view 3 OverlayLDRPass end", nullptr) && !DrawHooksIn(ctxTable),
          "far depth overlay trace: DLSSPass end .. CopyToHMDPass begin, every pass, draws with RTV format, "
          "ToneMappingPass's PS SRV0");

    // 6g. HDR fallback (far_depth.h 5e) on a sequence without a
    // ToneMappingPass (and so no source and no pre point): the scene in the
    // LDR target, OverlayLDRPass draws the HUD.
    hdrSeq = false;
    toneOn = false;
    ovlMode = 2;
    auto capture = [&](bool on) {
      std::lock_guard<std::mutex> lock(g_logMutex);
      if (on) g_reportBuf.clear();
      g_reportThread = GetCurrentThreadId();
      g_reportCapture = on;
    };
    auto captured = [&] {
      std::lock_guard<std::mutex> lock(g_logMutex);
      std::string r;
      r.swap(g_reportBuf);
      return r;
    };
    auto countOf = [](const std::string& s, const char* what) {
      int n = 0;
      for (size_t at = s.find(what); at != std::string::npos; at = s.find(what, at + 1)) ++n;
      return n;
    };
    auto noPairSum = [] {
      uint64_t n = 0;
      for (const auto& a : fdexport::g_hdrNoPair) n += a.load();
      return n;
    };
    // The overlay-mode copies of both focus views in the last frame: pre =
    // the scene without the HUD, post = the target with it, LDR formats.
    auto overlayCopies = [&] {
      bool ok = true;
      for (int view = 2; view <= 3; ++view) {
        ID3D11Texture2D* p = heldPre(view);
        bool hud = false;
        ok = ok && preIsScene(p, view, view == 2 ? 7 : 1, view == 2 ? 5 : 1, &hud) && hud && postIsTarget(p, view);
        const Push pv = lastPush(view);
        ok = ok && (pv.d.flags & DCSVR_DEPTH_HAS_PRE_OVERLAY) && (pv.d.flags & DCSVR_DEPTH_PRE_POST_OVERLAY) &&
             pv.result == DCSVR_DEPTH_PUSH_ACCEPTED &&
             pv.d.preOverlayFormat ==
                 static_cast<uint32_t>(view == 2 ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : DXGI_FORMAT_R11G11B10_FLOAT);
        Rel(p);
      }
      return ok;
    };
    {
      // Overlay mode set explicitly: never evaluated, never switched, the
      // overlay copies every frame, however long the HDR chain is missing.
      fdexport::ResetCopyPoint();
      const uint64_t to0 = fdexport::g_hdrToOverlay.load(), npS0 = noPairSum(),
                     of0 = fdexport::g_hdrOverlayFrames.load();
      bool okO = true;
      for (int f = 0; f < fdexport::kHdrMissFallback + 5; ++f) {
        frame(order);
        if (f >= 3 && (f % 20 == 0 || f >= fdexport::kHdrMissFallback)) okO = okO && overlayCopies();
      }
      Check(okO && fdexport::g_hdrToOverlay.load() == to0 && noPairSum() == npS0 &&
                fdexport::g_hdrOverlayFrames.load() == of0 && !fdexport::g_hw[0].fb && !fdexport::g_hw[1].fb,
            "far depth coverage overlay (explicit): no ToneMappingPass for 125 frames -> overlay copies every frame, "
            "no HDR evaluation, no switch");
    }
    {
      // hdr mode, a fresh state (no ToneMappingPass source learnt yet).
      fdexport::ResetCopyPoint();
      fdexport::g_covMode = fdexport::kCovHdr;
      fdexport::ResetHdrWatch();
      HdrRelease();
      const uint64_t to0 = fdexport::g_hdrToOverlay.load(), th0 = fdexport::g_hdrToHdr.load(),
                     ns0 = fdexport::g_hdrNoPair[0].load(), rel0 = fdexport::g_releases.load(),
                     of0 = fdexport::g_hdrOverlayFrames.load();
      capture(true);
      bool noColour = true;
      // A frame's end is evaluated at the next frame's first pass boundary
      // (fixation_depth.h EndFrame): after kHdrMissFallback frames,
      // kHdrMissFallback - 1 are evaluated.
      for (int f = 0; f < fdexport::kHdrMissFallback; ++f) {
        frame(order);
        noColour = noColour && !(lastPush(2).d.flags & DCSVR_DEPTH_HAS_PRE_OVERLAY) &&
                   !(lastPush(3).d.flags & DCSVR_DEPTH_HAS_PRE_OVERLAY) &&
                   lastPush(2).result == DCSVR_DEPTH_PUSH_ACCEPTED;
      }
      const bool notYet = fdexport::ViewHdr(2) && fdexport::ViewHdr(3) && fdexport::g_hdrToOverlay.load() == to0;
      const uint32_t genBefore = lastPush(2).d.generation;
      frame(order);  // its first boundary ends frame kHdrMissFallback: the switch; then overlay copies
      const bool switched = fdexport::HdrMode() && !fdexport::ViewHdr(2) && !fdexport::ViewHdr(3) &&
                            fdexport::g_hdrToOverlay.load() == to0 + 2 &&
                            fdexport::g_hdrNoPair[0].load() - ns0 == 2u * fdexport::kHdrMissFallback &&
                            fdexport::g_releases.load() > rel0;
      const bool firstOverlay = overlayCopies();
      printf("     hdr fallback: no colour %d, not yet %d, to overlay %llu, no source %llu, releases %llu, view hdr "
             "%d/%d\n",
             noColour, notYet, static_cast<unsigned long long>(fdexport::g_hdrToOverlay.load() - to0),
             static_cast<unsigned long long>(fdexport::g_hdrNoPair[0].load() - ns0),
             static_cast<unsigned long long>(fdexport::g_releases.load() - rel0), fdexport::ViewHdr(2),
             fdexport::ViewHdr(3));
      Check(noColour && notYet && switched,
            "far depth hdr fallback: no ToneMappingPass -> no colour (hdr, no source) for 120 frames, then both focus "
            "views switch to overlay (counted per view, reason 'no source', textures released)");
      bool okF = firstOverlay;
      for (int f = 0; f < 2; ++f) {
        frame(order);
        okF = okF && overlayCopies();
      }
      Check(okF && lastPush(2).d.generation != genBefore && fdexport::g_hdrOverlayFrames.load() - of0 == 4,
            "far depth hdr fallback: the next frames carry the overlay-mode pre/post copies (LDR formats, HUD only in "
            "post) under a new generation; view-frames on overlay counted");
      FarLogStats();
      capture(false);
      const std::string rep = captured();
      Check(countOf(rep, "no usable HDR pair for 120 frames (no ToneMappingPass source learnt") == 2 &&
                rep.find("HMD view 2: no usable HDR pair") != std::string::npos &&
                rep.find("HMD view 3: no usable HDR pair") != std::string::npos &&
                rep.find("fallback to overlay: view 2 yes, view 3 yes, switches to overlay") != std::string::npos,
            "far depth hdr fallback: logged once per view with the reason; the coverage stats line shows it");
      // More frames without a pair: no second switch, no second log.
      capture(true);
      for (int f = 0; f < 5; ++f) frame(order);
      capture(false);
      Check(fdexport::g_hdrToOverlay.load() == to0 + 2 && captured().find("no usable HDR pair") == std::string::npos,
            "far depth hdr fallback: stays on overlay without another switch or log");

      // The HDR chain appears (DLSSPass -> OverlayHDRPass -> HistogramPass ->
      // ToneMappingPass reading R16G16B16A16_FLOAT): hdr again after
      // kHdrOkReturn frames with a pair (hysteresis), the overlay copies until then.
      hdrSeq = true;
      ovlMode = 3;
      capture(true);
      for (int f = 0; f < fdexport::kHdrOkReturn; ++f) frame(order);  // kHdrOkReturn - 1 evaluated (see above)
      const bool held = !fdexport::ViewHdr(2) && !fdexport::ViewHdr(3) && fdexport::g_hdrToHdr.load() == th0 &&
                        g_hdr[0].res == hdrTex[0] && g_hdr[1].res == hdrTex[1] &&
                        lastPush(2).d.preOverlayFormat == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
      frame(order);  // its first boundary ends frame kHdrOkReturn with a pair: hdr again, this frame's copies HDR
      const bool back = fdexport::ViewHdr(2) && fdexport::ViewHdr(3) && fdexport::g_hdrToHdr.load() == th0 + 2 &&
                        lastPush(2).d.preOverlayFormat == DXGI_FORMAT_R16G16B16A16_FLOAT;
      capture(false);
      const std::string rep2 = captured();
      Check(held && back && countOf(rep2, "HDR pair back for 60 frames") == 2,
            "far depth hdr fallback: the HDR source appears -> learnt while on overlay, still overlay for 60 frames, "
            "then hdr again (hysteresis; logged per view)");
      const uint64_t hpB0 = fdexport::g_hdrPre.load();
      bool okH = true;
      for (int f = 0; f < 3; ++f) {
        frame(order);
        if (f < 1) continue;  // both slots recreated in HDR format
        for (int view = 2; view <= 3; ++view) {
          bool hp = false, hq = false, hz = false;
          checkHdr(view, &hp, &hq, &hz);
          okH = okH && hp && hq && hz;
        }
      }
      Check(okH && fdexport::g_hdrPre.load() - hpB0 == 6 && fdexport::g_stateDiffs.load() == 0 && !DrawHooksIn(ctxTable),
            "far depth hdr fallback: back in hdr, the HDR pre/post copies again (R16G16B16A16, HUD only in post)");
    }
    fdexport::g_covMode = fdexport::kCovOverlay;
    fdexport::ResetHdrWatch();
    toneOn = true;
    hdrSeq = false;
    ovlMode = 0;
    for (int f = 0; f < 3; ++f) frame(order);  // back to LDR textures in both slots (recreated)
  }

  // 6e. v1.1 post-overlay (far_depth.h 5c): the tone-mapped scene in the
  // target, the HUD drawn by OverlayLDRPass; the same box copied again at the
  // view's CopyToHMDPass begin to (w, 0) of the same texture.
  if (oshOk) {
    fdexport::ResetCopyPoint();
    ovlMode = 2;
    const uint64_t pc0 = fdexport::g_postCopies.load(), sd0 = fdexport::g_stateDiffs.load(),
                   sc0 = fdexport::g_stateChecks.load();
    const size_t pp0 = pushesNow();
    bool okPre = true, okPost = true, flags = true, sizes = true;
    for (int f = 0; f < 3; ++f) {
      frame(order);
      for (int view = 2; view <= 3; ++view) {
        ID3D11Texture2D* p = heldPre(view);
        bool hud = false;
        okPre = okPre && preIsScene(p, view, view == 2 ? 7 : 1, view == 2 ? 5 : 1, &hud) && hud;
        okPost = okPost && postIsTarget(p, view);
        D3D11_TEXTURE2D_DESC pd{};
        if (p) p->GetDesc(&pd);
        const Push pv = lastPush(view);
        const UINT w = view == 2 ? 288 : 300, h = view == 2 ? 270 : 280;
        sizes = sizes && p && pd.Width == 2 * w && pd.Height == h && pv.d.preOverlaySize == (w | (h << 16));
        flags = flags && (pv.d.flags & DCSVR_DEPTH_HAS_PRE_OVERLAY) && (pv.d.flags & DCSVR_DEPTH_PRE_POST_OVERLAY) &&
                pv.result == DCSVR_DEPTH_PUSH_ACCEPTED && pv.d.engineFrame == lastPush(2).d.engineFrame;
        Rel(p);
      }
    }
    printf("     post-overlay: copies %llu, pushes %zu, flags 0x%x\n",
           static_cast<unsigned long long>(fdexport::g_postCopies.load() - pc0), pushesNow() - pp0,
           lastPush(2).d.flags);
    Check(okPre && okPost, "far depth post-overlay: (0,0) = this frame's scene without the HUD, (w,0) = the same box "
                           "after the overlays (scene with the HUD), sRGB target with an inner viewport and R11G11B10");
    Check(sizes && flags && fdexport::g_postCopies.load() - pc0 == 6 && pushesNow() - pp0 == 6,
          "far depth post-overlay: texture 2w x h, preOverlaySize w | h << 16, HAS_PRE_OVERLAY | PRE_POST_OVERLAY, "
          "one push per view after the post copy, accepted, same engineFrame");
    Check(fdexport::g_stateChecks.load() >= sc0 + 12 && fdexport::g_stateDiffs.load() == sd0 && ovlStateOk,
          "far depth post-overlay: DCS's bindings identical around both copies (module state check)");
    // A GUIPass into the target after OverlayLDRPass: the post copy has it.
    guiAfter = true;
    frame(order);
    {
      ID3D11Texture2D* p = heldPre(2);
      const std::vector<uint8_t> got = p ? fdtest::ReadTex(dev, c, p) : std::vector<uint8_t>{};
      const std::vector<uint8_t> rt = fdtest::ReadTex(dev, c, colTex[0]);
      uint32_t guiPost = 0, guiRt = 0;
      if (!got.empty() && !rt.empty()) {  // target pixel (30, 210) -> box (24, 206) -> post (288 + 24, 206)
        memcpy(&guiPost, got.data() + (static_cast<size_t>(206) * 576 + 288 + 24) * 4, 4);
        memcpy(&guiRt, rt.data() + (static_cast<size_t>(210) * 300 + 30) * 4, 4);
      }
      bool hud = false;
      Check(p && postIsTarget(p, 2) && guiPost == guiRt && preIsScene(p, 2, 7, 5, &hud) &&
                (lastPush(2).d.flags & DCSVR_DEPTH_PRE_POST_OVERLAY),
            "far depth post-overlay: a GUIPass after OverlayLDRPass -> the post copy is after it (GUI in (w,0) half)");
      Rel(p);
    }
    guiAfter = false;
    // FarDepthPostOverlay=0: w x h, no flag (v1.1 as before).
    fdexport::g_postOn = false;
    frame(order);
    frame(order);
    {
      ID3D11Texture2D* p = heldPre(3);
      D3D11_TEXTURE2D_DESC pd{};
      if (p) p->GetDesc(&pd);
      bool hud = false;
      const Push p3 = lastPush(3);
      Check(p && pd.Width == 300 && preIsScene(p, 3, 1, 1, &hud) && hud &&
                (p3.d.flags & DCSVR_DEPTH_HAS_PRE_OVERLAY) && !(p3.d.flags & DCSVR_DEPTH_PRE_POST_OVERLAY) &&
                p3.result == DCSVR_DEPTH_PUSH_ACCEPTED,
            "far depth post-overlay off: texture w x h, HAS_PRE_OVERLAY only, pushed at the overlay pass end");
      Rel(p);
    }
    fdexport::g_postOn = true;
    // MSAA target (no pre copy): no post copy, no flag.
    msaa3 = true;
    frame(order);
    Check(!(lastPush(3).d.flags & (DCSVR_DEPTH_HAS_PRE_OVERLAY | DCSVR_DEPTH_PRE_POST_OVERLAY)) &&
              lastPush(3).result == DCSVR_DEPTH_PUSH_ACCEPTED,
          "far depth post-overlay: no pre-overlay copy (MSAA) -> no post copy, no flag");
    msaa3 = false;
    frame(order);
    ovlMode = 0;
  }

  // 6d. v1.1 copy point (far_depth.h 5b): OverlayLDRPass composites the
  // tone-mapped scene with one full-screen draw, then draws the HUD.
  if (oshOk) {
    auto preOk = [&](int view, bool* hud) {
      ID3D11Texture2D* p = heldPre(view);
      const bool ok = preIsScene(p, view, view == 2 ? 7 : 1, view == 2 ? 5 : 1, hud);
      Rel(p);
      return ok;
    };
    auto modes = [&](int m) { return fdexport::g_preMode[0] == m && fdexport::g_preMode[1] == m; };
    // The live symptom, reproduced: the viewport point copies the target
    // before the composite, i.e. last frame's image with its HUD.
    fdexport::ResetCopyPoint();
    fdexport::g_preMode[0] = fdexport::g_preMode[1] = fdexport::kPreAtViewport;
    ovlMode = 1;
    frame(order);
    frame(order);
    bool hud = false;
    Check(!preOk(2, &hud) && !preOk(3, &hud),
          "far depth copy point: composite-first pass copied at the first viewport -> stale (last frame + HUD)");
    // Learning: the first draw into the target is full-screen in each of the
    // first three passes -> "after the composite"; those frames are already
    // copied after it.
    fdexport::ResetCopyPoint();
    const uint64_t cc0 = fdexport::g_cmpCopies.load(), lc0 = fdexport::g_learnCopies.load(),
                   pc0 = fdexport::g_preCopies.load(), sd0 = fdexport::g_stateDiffs.load(),
                   sc0 = fdexport::g_stateChecks.load();
    hooksInPass = false;
    bool learnOk = true;
    for (int f = 0; f < 3; ++f) {
      frame(order);
      bool h2 = false, h3 = false;
      learnOk = learnOk && preOk(2, &h2) && preOk(3, &h3) && h2 && h3;
    }
    printf("     copy point: modes %d/%d, learning %d/%d full %d/%d, composite copies %llu (learning %llu)\n",
           fdexport::g_preMode[0], fdexport::g_preMode[1], fdexport::g_learnSeen[0], fdexport::g_learnSeen[1],
           fdexport::g_learnFull[0], fdexport::g_learnFull[1],
           static_cast<unsigned long long>(fdexport::g_cmpCopies.load() - cc0),
           static_cast<unsigned long long>(fdexport::g_learnCopies.load() - lc0));
    Check(modes(fdexport::kPreAfterComposite) && fdexport::g_learnSeen[0] == 3 && fdexport::g_learnFull[1] == 3 &&
              fdexport::g_learnCopies.load() - lc0 == 6 && learnOk && hooksInPass,
          "far depth copy point: composite-first OverlayLDRPass -> decided 'after the scene composite draw' per "
          "view from 3 passes; learning frames already copied after it (this frame's scene, no HUD)");
    // Decided: cached, copies after the composite only.
    const int seen0 = fdexport::g_learnSeen[0];
    const size_t pp0 = pushesNow();
    bool okAll = true, flagsOk = true, tableClean = true;
    for (int f = 0; f < 4; ++f) {
      frame(order);
      bool h2 = false, h3 = false;
      okAll = okAll && preOk(2, &h2) && preOk(3, &h3) && h2 && h3;
      const Push a2 = lastPush(2), a3 = lastPush(3);
      flagsOk = flagsOk && (a2.d.flags & DCSVR_DEPTH_HAS_PRE_OVERLAY) && (a3.d.flags & DCSVR_DEPTH_HAS_PRE_OVERLAY) &&
                a2.result == DCSVR_DEPTH_PUSH_ACCEPTED && a3.result == DCSVR_DEPTH_PUSH_ACCEPTED;
      tableClean = tableClean && !DrawHooksIn(ctxTable);
    }
    Check(okAll && flagsOk && pushesNow() == pp0 + 8,
          "far depth copy point: after the composite -> each push's pre-overlay = this frame's scene, without the HUD "
          "drawn after it (sRGB target with an inner viewport, R11G11B10)");
    Check(fdexport::g_learnSeen[0] == seen0 && modes(fdexport::kPreAfterComposite) &&
              fdexport::g_learnCopies.load() - lc0 == 6 && fdexport::g_cmpCopies.load() - cc0 == 14 &&
              fdexport::g_preCopies.load() - pc0 == 14,
          "far depth copy point: the decision is cached (no more learning), one copy per view and frame");
    Check(cmpStateOk && fdexport::g_stateChecks.load() > sc0 && fdexport::g_stateDiffs.load() == sd0 && tableClean &&
              fdexport::g_drawLost.load() == 0,
          "far depth copy point: DCS's bindings identical across the hooked composite draw and the copy after it; "
          "no draw hook left in the context table after the pass");
    // Decided "after the composite", one frame without it (HUD first): the
    // copy is made just before that draw (no HUD), counted.
    const uint64_t miss0 = fdexport::g_cmpMiss.load();
    ovlMode = 2;
    frame(order);
    bool h2m = false;
    Check(fdexport::g_cmpMiss.load() == miss0 + 2 && preOk(2, &h2m) && h2m && modes(fdexport::kPreAfterComposite),
          "far depth copy point: decided 'after the composite', a pass whose first draw is not full-screen -> "
          "copied just before it (no HUD), decision kept");
    // HUD-first pattern from the start (the scene already in the target): the
    // viewport point is kept and the draw hooks are not armed any more.
    fdexport::ResetCopyPoint();
    for (int f = 0; f < 3; ++f) frame(order);
    const uint64_t arms0 = fdexport::g_drawArms.load();
    bool okV = true;
    for (int f = 0; f < 3; ++f) {
      frame(order);
      bool h2v = false, h3v = false;
      okV = okV && preOk(2, &h2v) && preOk(3, &h3v) && h2v && h3v;
    }
    Check(modes(fdexport::kPreAtViewport) && fdexport::g_learnFull[0] == 0 && okV &&
              fdexport::g_drawArms.load() == arms0 && !DrawHooksIn(ctxTable),
          "far depth copy point: HUD drawn first -> the first viewport after the bind kept (copy = scene, no HUD), "
          "no draw hook armed after the decision");
    // No draw at all (ClearView HUD): undecided, the viewport copy as before.
    fdexport::ResetCopyPoint();
    ovlMode = 0;
    bool ok0 = true;
    for (int f = 0; f < 2; ++f) {
      frame(order);
      bool h20 = false, h30 = false;
      ok0 = ok0 && preOk(2, &h20) && preOk(3, &h30) && h20 && h30;
    }
    Check(ok0 && modes(fdexport::kPreLearn) && fdexport::g_learnEmpty[0] == 2,
          "far depth copy point: no draw into the target -> the old viewport copy, still learning");
    // The overlay trace (FarDepthOverlayTrace=1): one frame, from
    // ToneMappingPass to OverlayLDRPass of each focus view.
    {
      std::lock_guard<std::mutex> lock(fdexport::g_statMutex);
      fdexport::g_ovTrace.clear();
    }
    fdexport::ResetCopyPoint();
    ovlMode = 1;
    fdexport::g_ovTraceLeft = 1;
    frame(order);
    std::vector<std::string> tl;
    {
      std::lock_guard<std::mutex> lock(fdexport::g_statMutex);
      tl.swap(fdexport::g_ovTrace);
    }
    frame(order);
    size_t after = 0;
    {
      std::lock_guard<std::mutex> lock(fdexport::g_statMutex);
      after = fdexport::g_ovTrace.size();
    }
    auto has = [&](const char* a, const char* b2) {
      for (const std::string& l : tl)
        if (l.find(a) != std::string::npos && (!b2 || l.find(b2) != std::string::npos)) return true;
      return false;
    };
    for (const std::string& l : tl) printf("     trace: %s\n", l.c_str());
    Check(has("view 2: window opens at ToneMappingPass", "draw hooks in") &&
              has("view 2 OverlayLDRPass draw 1: Draw(3) x1", "full-screen yes; pre-overlay target YES") &&
              has("view 2 OverlayLDRPass draw 2: Draw(6)", "full-screen no; pre-overlay target YES") &&
              has("view 3 OverlayLDRPass draw 1: Draw(3)", "pre-overlay target YES") &&
              has("view 2 ToneMappingPass end: draws 0, into the pre-overlay target 0", nullptr) &&
              has("view 3 OverlayLDRPass end: draws 2, into the pre-overlay target 2", nullptr) && after == 0 &&
              fdexport::g_ovTraceLeft.load() == 0 && !DrawHooksIn(ctxTable),
          "far depth overlay trace: one frame of ToneMappingPass..OverlayLDRPass draws per focus view (call, "
          "count, full-screen, pre-overlay target), then off; no hook left");
    ovlMode = 0;
    // GPU pass timing holds the table (its re-arm adopts any foreign entry as
    // d3d11's): neither the upload nor the draw entries are taken.
    fdexport::ResetCopyPoint();
    const uint64_t us0 = fdexport::g_usSkipForeign.load(), ds0 = fdexport::g_drawSkipForeign.load();
    ovlMode = 1;
    gpt::g_opsHooked = true;
    fdexport::g_ovTraceLeft = 1;  // the trace window asks for the draw entries in any case
    frame(order);
    gpt::g_opsHooked = false;
    printf("     foreign: us skip %llu draw skip %llu hooks %d usHook %d\n",
           static_cast<unsigned long long>(fdexport::g_usSkipForeign.load() - us0),
           static_cast<unsigned long long>(fdexport::g_drawSkipForeign.load() - ds0), DrawHooksIn(ctxTable) ? 1 : 0,
           ctxTable[usSlot] == reinterpret_cast<void*>(&UsHook) ? 1 : 0);
    Check(fdexport::g_usSkipForeign.load() > us0 && fdexport::g_drawSkipForeign.load() > ds0 &&
              !DrawHooksIn(ctxTable) && ctxTable[usSlot] != reinterpret_cast<void*>(&UsHook),
          "far depth: with GPU pass timing's table hooks in, no upload or draw hook is armed (no re-arm cycle)");
    frame(order);
    {
      std::lock_guard<std::mutex> lock(fdexport::g_statMutex);
      fdexport::g_ovTrace.clear();
    }
    ovlMode = 0;
  }

  // 7. The layer unloads mid-session, then comes back.
  fdexport::g_module = L"no_such_quad_views_layer.dll";
  const uint64_t nl0 = fdexport::g_noLayer.load(), cp0 = fdexport::g_copies.load();
  pi = pushesNow();
  for (int f = 0; f < 3; ++f) frame(order);
  Check(fdexport::g_noLayer.load() >= nl0 + 3 && pushesNow() == pi && fdexport::g_copies.load() == cp0,
        "far depth: layer unloaded -> Wanted unresolved, no copy, no push, nothing called through a stale pointer");
  fdexport::g_module = L"test_main.exe";
  g_api = 2;
  frame(order);
  Check(pushesNow() == pi && fdexport::g_badApi.load() >= 1, "far depth: a layer with another DepthApi() version is ignored");
  g_api = 1;
  frame(order);
  frame(order);
  Check(pushesNow() >= pi + 4, "far depth: the layer back -> pushes resume");

  // 8. 120 frames unwanted: Release(generation) first, then our copies; a new generation.
  const uint32_t gen0 = fdexport::g_gen;
  size_t r0;
  {
    std::lock_guard<std::mutex> lock(g_m);
    r0 = g_rels.size();
  }
  g_mask = 0;
  for (int f = 0; f < 125; ++f) frame(order);
  {
    std::lock_guard<std::mutex> lock(g_m);
    bool ok = g_rels.size() > r0;
    for (size_t k = r0; k < g_rels.size(); ++k)
      ok = ok && g_rels[k].thread == g_renderThread && g_rels[k].held == g_rels[k].heldWithOurs;
    const bool released = ok && g_rels[r0].gen == gen0 && g_rels[r0].held == 2;
    Check(released && !fdexport::HaveTextures() && fdexport::g_gen != gen0 && !g_held[2] && !g_held[3],
          "far depth: 120 unwanted frames -> Release(generation) on the render thread while ours were alive, then "
          "our copies freed, new generation");
  }
  g_mask = fdexport::kFocusMask;
  frame(order);
  frame(order);
  Check(lastPush(2).d.generation == fdexport::g_gen && lastPush(2).result == DCSVR_DEPTH_PUSH_ACCEPTED,
        "far depth: wanted again -> new textures under the new generation");

  // 9. Hot reload while Quad Views holds the textures: the uninstall asks the
  // render thread (this thread keeps rendering) to release them first.
  ID3D11Texture2D* kept = nullptr;  // a layer that keeps a reference anyway
  {
    std::lock_guard<std::mutex> lock(g_m);
    kept = g_held[2];
    if (kept) kept->AddRef();
  }
  const uint32_t genU = fdexport::g_gen;
  {
    std::lock_guard<std::mutex> lock(g_m);
    r0 = g_rels.size();
  }
  std::atomic<bool> done{false};
  std::thread un([&] {
    Uninstall();
    done = true;
  });
  for (int f = 0; f < 400 && !done.load(); ++f) {
    frame(order);
    Sleep(1);
  }
  un.join();
  {
    std::lock_guard<std::mutex> lock(g_m);
    bool onRender = g_rels.size() > r0;
    for (size_t k = r0; k < g_rels.size(); ++k)
      onRender = onRender && g_rels[k].thread == g_renderThread && g_rels[k].held == g_rels[k].heldWithOurs;
    Check(onRender && g_rels[r0].gen == genU && g_rels[r0].held == 2 && !g_held[2] && !g_held[3],
          "far depth hot reload: Release(generation) on the render thread before our textures were freed");
  }
  D3D11_TEXTURE2D_DESC kd{};
  if (kept) kept->GetDesc(&kd);
  Check(kept && Refs(kept) == 1 && kd.Width == 256 && !fdexport::HaveTextures() && !g_installed.load() && !g_s.dev,
        "far depth hot reload: a reference the layer kept is still valid (not dangling), ours are gone");
  Rel(kept);

  // 10. Reinstalled together with the fixation-depth cone (both share the
  // view's run); then an uninstall with the render thread idle.
  wchar_t dname[64];
  swprintf_s(dname, L"Local\\FarTestDepth_%lu", GetCurrentProcessId());
  g_depthName = dname;
  g_coneOn = true;
  FakeInstall();
  const uint64_t disp0 = g_dispatches.load();
  const size_t pc0 = pushesNow();
  for (int f = 0; f < 4; ++f) {
    g.displayTime = 5000 + f;
    for (int vv = 0; vv < 4; ++vv) g.gazeTan[vv][0] = 0.05f, g.gazeTan[vv][1] = 0.1f;
    if (gview) SeqWrite(gview, g);
    frame(order);
  }
  {
    std::lock_guard<std::mutex> lock(g_m);
    r0 = g_rels.size();
  }
  Check(HeldNow() == 2 && g_dispatches.load() > disp0 && pushesNow() >= pc0 + 4 && stateSame &&
            fdexport::g_stateDiffs.load() == 0,
        "far depth: reinstalled with FixationDepth on too: cone dispatches and both focus pushes, bindings identical");
  Uninstall();
  {
    std::lock_guard<std::mutex> lock(g_m);
    Check(g_rels.size() > r0 && g_rels[r0].held == 2 && g_rels[r0].held == g_rels[r0].heldWithOurs &&
              !fdexport::HaveTextures() && !g_held[2] && !g_held[3],
          "far depth: render thread idle at uninstall -> released from the uninstall, before our textures");
  }
  Check(!g_s.dev && !g_worker.joinable() && !g_installed.load(), "far depth: uninstalled (worker joined, device released)");

  // Teardown.
  g_coneOn = false;
  g_depthName = L"Local\\DcsVrFixationDepth_v1";
  fdexport::g_on = false;
  fdexport::g_module = oldMod;
  fdexport::SetExportPrefix("DcsVr_QuadViews");
  g_verify = 0;
  shadowinst::g_device = oldDev;
  g_gazeName = L"Local\\DcsVrPerceptionGaze_v1";
  {
    std::lock_guard<std::mutex> lock(g_m);
    for (auto*& t : g_held) Rel(t);
    for (auto*& t : g_heldPre) Rel(t);
  }
  for (int k = 0; k < 2; ++k) Rel(colRtv[k]), Rel(colTex[k]), Rel(srcRtv[k]), Rel(srcSrv[k]), Rel(srcTex[k]);
  for (int k = 0; k < 2; ++k) Rel(hdrRtv[k]), Rel(hdrSrv[k]), Rel(hdrTex[k]);
  osh.Release();
  Rel(msRtv), Rel(msTex);
  for (View& x : v) Rel(x.dsv), Rel(x.tex), Rel(x.cb);
  Rel(def.dsv), Rel(def.tex);
  c->ClearState();
  Rel(dcsCs), Rel(cbA);
  if (gview) UnmapViewOfFile(gview);
  if (gm) CloseHandle(gm);
  g_qvDevice = nullptr;
  Rel(c), Rel(imm);
  const ULONG last = dev->Release();
  Check(last == 0, "far depth: no D3D reference leaked (device released to 0)");
}

// Integration with the real Quad Views layer, when its build is available
// (the DLL named by DCSVR_QV_DLL; skipped when it is not set).
// A copy in our own temp folder is loaded as a plain DLL: its DllMain only
// registers a trace provider, and without an OpenXR session its import has no
// device and wants nothing, so the round trips check the contract surface:
// DepthApi 1, Wanted 0 (our module then copies nothing), Push rejecting a
// null / short descriptor (0) and a valid one as not wanted (5) without a
// reference kept, Release of any generation harmless, and our seam resolving
// the layer by module name. Accepted pushes need the layer's session (the live
// battery).
void TestRealLayer() {
  wchar_t src[MAX_PATH] = {};
  if (!GetEnvironmentVariableW(L"DCSVR_QV_DLL", src, MAX_PATH)) {
    printf("SKIP far depth real layer: DCSVR_QV_DLL (path of a Quad Views build) is not set\n");
    return;
  }
  if (GetFileAttributesW(src) == INVALID_FILE_ATTRIBUTES) {
    printf("SKIP far depth real layer: Quad Views build not found\n");
    return;
  }
  if (GetModuleHandleW(L"XR_APILAYER_MBUCCHIA_quad_views_foveated.dll")) {
    printf("SKIP far depth real layer: a Quad Views layer is already loaded in this process\n");
    return;
  }
  wchar_t tmp[MAX_PATH] = {}, dir[MAX_PATH] = {}, dst[MAX_PATH] = {};
  GetTempPathW(MAX_PATH, tmp);
  swprintf_s(dir, L"%lsdcsqvcull_qvlayer_%lu", tmp, GetCurrentProcessId());
  CreateDirectoryW(dir, nullptr);
  swprintf_s(dst, L"%ls\\XR_APILAYER_MBUCCHIA_quad_views_foveated.dll", dir);
  if (!CopyFileW(src, dst, FALSE)) {
    printf("SKIP far depth real layer: copy failed (%lu)\n", GetLastError());
    RemoveDirectoryW(dir);
    return;
  }
  HMODULE m = LoadLibraryW(dst);
  if (!m) {
    printf("SKIP far depth real layer: LoadLibrary failed (%lu); it may need the OpenXR loader\n", GetLastError());
    DeleteFileW(dst);
    RemoveDirectoryW(dir);
    return;
  }
  const auto api = reinterpret_cast<PFN_DcsVr_QuadViewsDepthApi>(GetProcAddress(m, "DcsVr_QuadViewsDepthApi"));
  const auto wanted = reinterpret_cast<PFN_DcsVr_QuadViewsDepthWanted>(GetProcAddress(m, "DcsVr_QuadViewsDepthWanted"));
  const auto push = reinterpret_cast<PFN_DcsVr_QuadViewsDepth>(GetProcAddress(m, "DcsVr_QuadViewsDepth"));
  const auto rel = reinterpret_cast<PFN_DcsVr_QuadViewsDepthRelease>(GetProcAddress(m, "DcsVr_QuadViewsDepthRelease"));
  const auto status = reinterpret_cast<uint32_t (*)()>(GetProcAddress(m, "DcsVr_QuadViewsDisparityStatus"));
  Check(api && wanted && push && rel && status, "far depth real layer: the four depth exports and DisparityStatus resolve");
  if (api && wanted && push && rel && status) {
    Check(api() == 1 && wanted() == 0 && status() == 0,
          "far depth real layer (no OpenXR session): DepthApi 1, Wanted 0, DisparityStatus 0");
    // Our seam against the real layer (by module name, as in DCS).
    const std::wstring oldMod = fdexport::g_module;
    fdexport::g_module = L"XR_APILAYER_MBUCCHIA_quad_views_foveated.dll";
    fdexport::SetExportPrefix("DcsVr_QuadViews");
    const uint64_t bad0 = fdexport::g_badApi.load(), nx0 = fdexport::g_noExport.load(), nl0 = fdexport::g_noLayer.load();
    const uint32_t mask = fdexport::QvWanted();
    Check(mask == 0 && fdexport::g_badApi.load() == bad0 && fdexport::g_noExport.load() == nx0 &&
              fdexport::g_noLayer.load() == nl0,
          "far depth real layer: our per-call resolve finds it by module name; mask 0 -> nothing would be copied");
    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* c = nullptr;
    const D3D_FEATURE_LEVEL fl[] = {D3D_FEATURE_LEVEL_11_1};
    if (SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, fl, 1, D3D11_SDK_VERSION, &dev, nullptr,
                                    &c))) {
      D3D11_TEXTURE2D_DESC d{};
      d.Width = 64;
      d.Height = 62;
      d.MipLevels = d.ArraySize = 1;
      d.Format = DXGI_FORMAT_R32G8X24_TYPELESS;
      d.SampleDesc.Count = 1;
      d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
      ID3D11Texture2D* t = nullptr;
      dev->CreateTexture2D(&d, nullptr, &t);
      fdexport::CaptureInfo ci;
      ci.hmdView = 2;
      ci.engineFrame = 1;
      ci.generation = 7;
      ci.format = d.Format;
      ci.texture = t;
      ci.texW = 64;
      ci.texH = 62;
      ci.vp = D3D11_VIEWPORT{0, 0, 64, 62, 0, 1};
      fdtest::MakeProj(ci.proj, -0.6, 0.6, 0.6, -0.6, 0.02, 0, true);
      ci.afterWater = true;
      DcsVrDepthView view{};
      const bool filled = t && fdexport::FillView(ci, view);
      const ULONG r0 = t ? Refs(t) : 0;
      DcsVrDepthView shortView = view;
      shortView.structSize = 200;
      const uint32_t rNull = push(nullptr), rShort = push(&shortView);
      const uint32_t rValid = filled ? fdexport::QvPush(view) : 99;
      rel(7);
      rel(12345);
      const ULONG r1 = t ? Refs(t) : 0;
      printf("     real layer: push null %u, short %u, valid %u; texture references %lu -> %lu\n", rNull, rShort, rValid,
             r0, r1);
      Check(filled && rNull == DCSVR_DEPTH_PUSH_NULL_OR_SIZE && rShort == DCSVR_DEPTH_PUSH_NULL_OR_SIZE &&
                rValid == DCSVR_DEPTH_PUSH_NOT_WANTED && r0 == r1,
            "far depth real layer: push round trips: null/short -> 0, valid without a session -> 5 (not wanted), no "
            "reference kept; Release(any generation) harmless");
      Rel(t);
      c->Release();
      Check(dev->Release() == 0, "far depth real layer: no reference to our device left in the layer");
    }
    fdexport::g_module = oldMod;
  }
  FreeLibrary(m);
  Check(!GetModuleHandleW(L"XR_APILAYER_MBUCCHIA_quad_views_foveated.dll"),
        "far depth real layer: unloaded again; our resolve then finds nothing");
  Check(fdexport::QvWanted() == 0, "far depth real layer: after the unload, Wanted resolves to 0 (layer missing)");
  DeleteFileW(dst);
  RemoveDirectoryW(dir);
}

void Run() {
  printf("-- far_depth.h --\n");
  TestRealLayer();
  TestFill();
  TestUploads();
  TestDebugLayer();
  TestPipeline();
}

}  // namespace fartest
