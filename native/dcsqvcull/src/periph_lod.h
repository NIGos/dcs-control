// Peripheral-only instancer LOD prototype ([Model] PeripheralLodScale, percent,
// default 0 = off; R26 §13 lead B). LOSSY by construction: trees, shrubs and
// buildings switch LOD and fade out closer to the camera in the two
// peripheral quad views only. The focus views, the height map and the shadow
// cascades keep DCS's own tables. With the key at 0 nothing is installed.
//
// Mechanism [V edterrainGraphics41.dll 2.9.30, static, R26 §10/§13]:
// InstanceManager2 vt[24] (ETG+0xa3700, slot ETG+0x14f5b8) rescales every
// object type's LOD switch and max distances with the instancer factors and
// uploads them, through DCS's buffer objects at IM2+0x290 / +0x2a0 / +0x2c0
// (each object's vt[2](this, 0, data, bytes)), into three GPU buffers. The
// scaled values exist only in temporaries and on the GPU: no CPU-side culling
// reads them. So the per-view effect of other factors is fully described by
// the contents of those three buffers.
//
// Prototype:
// 1. Capture (in the vt[24] hook, on the thread that calls it, which must be
//    the render thread): DCS's call is passed through unchanged; then, once
//    (and again whenever DCS's factors change), vt[24] is called with a copy
//    whose factors are scaled exactly as lod_probe.h does (+0x10, +0x4c, +0x50
//    times s, +0x48 times s clamped to [0.3, 1]); DCS uploads the scaled tables,
//    which are copied into our "scaled" buffers; then vt[24] gets DCS's own
//    values back, DCS uploads its tables again, and they are copied into our
//    "stock" buffers. The two extra calls write DCS's "instancer factors has
//    been changed" line to dcs.log twice per capture.
// 2. Swap (pass_timing.h's g_viewBoundary, render thread, top-level passes):
//    DCS renders the views as periphery L, periphery R, focus L, focus R
//    [V gdd census: CopyToHMDPass targets 816x804, 816x804, 2028x2004,
//    2028x2004], each starting with a LightsBuffersPass (the per-view culling
//    chain). At the first LightsBuffersPass of a frame our scaled tables are
//    copied into DCS's buffers, at the third the stock ones. HeightMapPass and
//    the cascades come before the views, so they always see stock tables
//    (a frame that ends early is put back at the next HeightMapPass/cascade).
//    CopyResource on DCS's immediate context: ordered with the dispatches.
//    The view order is checked at every CopyToHMDPass end (target width).
//
// Suite phases (measurement):
// - TimingPhase: GpuPassTiming 5 s each, OFF / ON / OFF / ON / OFF.
// - ImagePhase: the final per-view target written by CopyToHMDPass (the
//   image DCS hands to QVFR) read back for N consecutive frames per
//   configuration, OFF / ON / OFF, written raw to <dev>\plod\ for offline
//   comparison (tools in the R26 §13 notes).
//
// lod_probe.h hooks the same slot: never run its phase while this is on.
// Included once from main.cpp after gpu_pass_timing.h.
#pragma once

namespace plod {

constexpr uint32_t kSlotRva = 0x14f5b8;
constexpr uint32_t kFnRva = 0xa3700;
constexpr size_t kOptBytes = 0x60;
constexpr uint32_t kBufOff[3] = {0x290, 0x2a0, 0x2c0};

using Fn = uint64_t(__fastcall*)(void*, void*, void*, void*);
Fn g_orig = nullptr;
void** g_slot = nullptr;

std::atomic<int> g_pct{0};          // wanted scale in percent; 0 = off
std::atomic<bool> g_swapOn{false};  // swap at the view boundaries (bench toggles this)
std::atomic<bool> g_ready{false};   // our copies hold valid tables
std::atomic<int> g_capPct{0};
std::atomic<uint64_t> g_calls{0}, g_captures{0}, g_swaps{0}, g_restores{0}, g_orderBad{0}, g_orderOk{0},
    g_offThread{0}, g_capFail{0};
std::atomic<DWORD> g_hookTid{0};

ID3D11Device* g_dev = nullptr;
ID3D11DeviceContext* g_ctx = nullptr;
ID3D11Buffer* g_dcs[3] = {};     // DCS's buffers (AddRef'd)
ID3D11Buffer* g_stock[3] = {};   // our copies
ID3D11Buffer* g_scaled[3] = {};
bool g_dynamic[3] = {};          // DCS's buffer is DYNAMIC: written by Map + memcpy from the CPU copies
std::vector<uint8_t> g_cpuStock[3], g_cpuScaled[3];
double g_capKey[5] = {};         // DCS's +0x00, +0x10, +0x48, +0x4c, +0x50 at the capture
void* g_capSelf = nullptr;
void* g_bufVt = nullptr;         // d3d11's ID3D11Buffer vtable

// Render-thread state.
int g_state = 0;    // 0 = DCS's buffers hold stock tables, 1 = scaled
int g_lbOrd = 0;    // LightsBuffersPass ordinal in the frame
int g_hmdOrd = 0;   // CopyToHMDPass ordinal in the frame
std::unordered_map<void*, int> g_kind;

enum Kind { kOther, kLights, kHeight, kCascade, kHmd, kGBuffer };

int KindRaw(void* pass) {
  __try {
    const uint8_t* b = static_cast<const uint8_t*>(pass);
    const uint64_t n = *reinterpret_cast<const uint64_t*>(b + 8);
    if (n == 0 || n > 32) return kOther;
    const char* s = reinterpret_cast<const char*>(b + 0x10);
    auto is = [&](const char* w) { return n == strlen(w) && memcmp(s, w, n) == 0; };
    if (is("LightsBuffersPass")) return kLights;
    if (is("HeightMapPass")) return kHeight;
    if (is("CopyToHMDPass")) return kHmd;
    if (is("GBufferPass")) return kGBuffer;
    if (n == 18 && memcmp(s, "CascadeShadowPass", 17) == 0) return kCascade;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }
  return kOther;
}
int KindOf(void* pass) {
  auto it = g_kind.find(pass);
  if (it != g_kind.end()) return it->second;
  const int k = KindRaw(pass);
  if (g_kind.size() < 4096) g_kind.emplace(pass, k);
  return k;
}

// ---------------------------------------------------------------------------
// Capture
// ---------------------------------------------------------------------------

bool ReadKey(const void* opt, double key[5]) {
  __try {
    const uint8_t* o = static_cast<const uint8_t*>(opt);
    key[0] = *reinterpret_cast<const double*>(o + 0x00);
    key[1] = *reinterpret_cast<const double*>(o + 0x10);
    key[2] = *reinterpret_cast<const float*>(o + 0x48);
    key[3] = *reinterpret_cast<const float*>(o + 0x4c);
    key[4] = *reinterpret_cast<const double*>(o + 0x50);
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

// lod_probe.h's scaling, on a copy.
bool CopyScaled(const void* src, uint8_t* dst, double s) {
  __try {
    memcpy(dst, src, kOptBytes);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
  double d10, d50;
  float f48, f4c;
  memcpy(&d10, dst + 0x10, 8);
  memcpy(&f48, dst + 0x48, 4);
  memcpy(&f4c, dst + 0x4c, 4);
  memcpy(&d50, dst + 0x50, 8);
  d10 *= s, d50 *= s;
  f4c = static_cast<float>(f4c * s);
  f48 = static_cast<float>(std::min(1.0, std::max(0.3, f48 * s)));
  memcpy(dst + 0x10, &d10, 8);
  memcpy(dst + 0x48, &f48, 4);
  memcpy(dst + 0x4c, &f4c, 4);
  memcpy(dst + 0x50, &d50, 8);
  return true;
}

// The ID3D11Buffer inside one of DCS's buffer objects: a pointer within its
// first 0x100 bytes (or one level further) to an object with d3d11's buffer
// vtable. *where gets the path for the log.
ID3D11Buffer* FindBufferRaw(void* obj, char* where, size_t cap) {
  __try {
    void* const* q = static_cast<void* const*>(obj);
    for (int i = 0; i < 32; ++i) {
      void* p = q[i];
      if (!gpt::Readable(p)) continue;
      if (*static_cast<void**>(p) == g_bufVt) {
        snprintf(where, cap, "+0x%x", i * 8);
        return static_cast<ID3D11Buffer*>(p);
      }
    }
    for (int i = 0; i < 32; ++i) {
      void* p = q[i];
      if (!gpt::Readable(p) || !gpt::Readable(*static_cast<void**>(p))) continue;
      void* const* r = static_cast<void* const*>(p);
      for (int j = 0; j < 32; ++j) {
        void* t = r[j];
        if (!gpt::Readable(t)) continue;
        if (*static_cast<void**>(t) == g_bufVt) {
          snprintf(where, cap, "+0x%x->+0x%x", i * 8, j * 8);
          return static_cast<ID3D11Buffer*>(t);
        }
      }
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }
  return nullptr;
}

void* ReadPtr(const void* p) {
  __try {
    return *static_cast<void* const*>(p);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return nullptr;
  }
}
void DescribeObj(void* obj, char* vt, size_t vcap, char* upd, size_t ucap) {
  __try {
    void** v = *static_cast<void***>(obj);
    gpt::ModRva(v, vt, vcap);
    gpt::ModRva(v[2], upd, ucap);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }
}

void ReleaseCopies() {
  for (int i = 0; i < 3; ++i) {
    if (g_stock[i]) g_stock[i]->Release();
    if (g_scaled[i]) g_scaled[i]->Release();
    if (g_dcs[i]) g_dcs[i]->Release();
    g_stock[i] = g_scaled[i] = g_dcs[i] = nullptr;
    g_cpuStock[i].clear();
    g_cpuScaled[i].clear();
  }
  g_ready = false;
}

bool EnsureDevice() {
  if (g_ctx) return true;
  ID3D11Device* dev = shadowinst::g_device;
  if (!dev) return false;
  dev->AddRef();
  g_dev = dev;
  dev->GetImmediateContext(&g_ctx);
  D3D11_BUFFER_DESC d{};
  d.ByteWidth = 16;
  d.Usage = D3D11_USAGE_DEFAULT;
  d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  ID3D11Buffer* b = nullptr;
  if (SUCCEEDED(dev->CreateBuffer(&d, nullptr, &b)) && b) {
    g_bufVt = *reinterpret_cast<void**>(b);
    b->Release();
  }
  return g_ctx != nullptr;
}

// Reads a buffer through a staging copy (blocking; capture only).
bool ReadBack(ID3D11Buffer* src, std::vector<uint8_t>& out) {
  D3D11_BUFFER_DESC d{};
  src->GetDesc(&d);
  D3D11_BUFFER_DESC s{};
  s.ByteWidth = d.ByteWidth;
  s.Usage = D3D11_USAGE_STAGING;
  s.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  s.MiscFlags = d.MiscFlags & D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
  s.StructureByteStride = d.StructureByteStride;
  ID3D11Buffer* st = nullptr;
  if (FAILED(g_dev->CreateBuffer(&s, nullptr, &st)) || !st) return false;
  g_ctx->CopyResource(st, src);
  D3D11_MAPPED_SUBRESOURCE m{};
  bool ok = SUCCEEDED(g_ctx->Map(st, 0, D3D11_MAP_READ, 0, &m));
  if (ok) {
    out.assign(static_cast<uint8_t*>(m.pData), static_cast<uint8_t*>(m.pData) + d.ByteWidth);
    g_ctx->Unmap(st, 0);
  }
  st->Release();
  return ok;
}

// DCS's three buffers -> our copies (dst = g_stock or g_scaled).
bool Snapshot(ID3D11Buffer** dst, std::vector<uint8_t>* cpu) {
  for (int i = 0; i < 3; ++i) {
    if (g_dynamic[i]) {
      if (!ReadBack(g_dcs[i], cpu[i])) return false;
    } else {
      g_ctx->CopyResource(dst[i], g_dcs[i]);
    }
  }
  return true;
}

// Our copies -> DCS's three buffers (render thread).
void Put(bool scaled) {
  for (int i = 0; i < 3; ++i) {
    if (!g_dcs[i]) continue;
    if (g_dynamic[i]) {
      const std::vector<uint8_t>& v = scaled ? g_cpuScaled[i] : g_cpuStock[i];
      D3D11_MAPPED_SUBRESOURCE m{};
      if (!v.empty() && SUCCEEDED(g_ctx->Map(g_dcs[i], 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
        memcpy(m.pData, v.data(), v.size());
        g_ctx->Unmap(g_dcs[i], 0);
      }
    } else {
      g_ctx->CopyResource(g_dcs[i], scaled ? g_scaled[i] : g_stock[i]);
    }
  }
}

void Capture(void* self, void* opt, void* c, void* d, int pct, const double key[5]) {
  static int logs = 0;
  const bool log = logs++ < 8;
  // Our view swap must not be half way: DCS's buffers get rewritten below.
  ReleaseCopies();
  for (int i = 0; i < 3; ++i) {
    void* obj = ReadPtr(static_cast<uint8_t*>(self) + kBufOff[i]);
    char where[64] = "?", vt[128] = "?", upd[128] = "?";
    ID3D11Buffer* b = obj ? FindBufferRaw(obj, where, sizeof(where)) : nullptr;
    if (obj) DescribeObj(obj, vt, sizeof(vt), upd, sizeof(upd));
    if (!b) {
      Log("periph lod: IM2+0x%x object %p (vtable %s): no ID3D11Buffer found; prototype off", kBufOff[i], obj, vt);
      ++g_capFail;
      ReleaseCopies();
      return;
    }
    b->AddRef();
    g_dcs[i] = b;
    D3D11_BUFFER_DESC bd{};
    b->GetDesc(&bd);
    g_dynamic[i] = bd.Usage == D3D11_USAGE_DYNAMIC;
    if (log)
      Log("periph lod: IM2+0x%x object %p vtable %s update %s, buffer %s = %p: %u bytes, usage %d, bind 0x%x, "
          "misc 0x%x, stride %u",
          kBufOff[i], obj, vt, upd, where, static_cast<void*>(b), bd.ByteWidth, static_cast<int>(bd.Usage),
          bd.BindFlags, bd.MiscFlags, bd.StructureByteStride);
    if (!g_dynamic[i]) {
      D3D11_BUFFER_DESC od = bd;
      od.Usage = D3D11_USAGE_DEFAULT;
      od.CPUAccessFlags = 0;
      if (FAILED(g_dev->CreateBuffer(&od, nullptr, &g_stock[i])) ||
          FAILED(g_dev->CreateBuffer(&od, nullptr, &g_scaled[i]))) {
        Log("periph lod: CreateBuffer for the copies failed; prototype off");
        ++g_capFail;
        ReleaseCopies();
        return;
      }
    }
  }
  alignas(16) uint8_t scaledOpt[kOptBytes];
  if (!CopyScaled(opt, scaledOpt, pct / 100.0)) return;
  g_orig(self, scaledOpt, c, d);  // DCS: "factors changed", uploads the scaled tables
  if (!Snapshot(g_scaled, g_cpuScaled)) {
    ++g_capFail;
    ReleaseCopies();
    g_orig(self, opt, c, d);
    return;
  }
  g_orig(self, opt, c, d);  // and its own tables again
  if (!Snapshot(g_stock, g_cpuStock)) {
    ++g_capFail;
    ReleaseCopies();
    return;
  }
  g_state = 0;
  memcpy(g_capKey, key, sizeof(g_capKey));
  g_capSelf = self;
  g_capPct = pct;
  ++g_captures;
  // Check once per capture: the two sets differ, and how (first floats of each buffer).
  for (int i = 0; i < 3; ++i) {
    std::vector<uint8_t> a, b;
    if (g_dynamic[i]) {
      a = g_cpuStock[i];
      b = g_cpuScaled[i];
    } else if (!ReadBack(g_stock[i], a) || !ReadBack(g_scaled[i], b)) {
      continue;
    }
    size_t words = std::min(a.size(), b.size()) / 4, diff = 0;
    double ratioSum = 0;
    size_t ratioN = 0;
    for (size_t w = 0; w < words; ++w) {
      float x, y;
      memcpy(&x, a.data() + w * 4, 4);
      memcpy(&y, b.data() + w * 4, 4);
      if (memcmp(&x, &y, 4) != 0) {
        ++diff;
        if (std::isfinite(x) && std::isfinite(y) && x > 1.0f && x < 1e7f) {
          ratioSum += y / x;
          ++ratioN;
        }
      }
    }
    if (log)
      Log("periph lod: buffer IM2+0x%x stock vs x%.2f: %zu of %zu words differ, mean scaled/stock over the changed "
          "distances %.3f (%zu)",
          kBufOff[i], pct / 100.0, diff, words, ratioN ? ratioSum / ratioN : 0.0, ratioN);
  }
  g_ready = true;
  if (log)
    Log("periph lod: captured on thread %lu (render thread %lu); DCS factors %.4f %.4f %.4f %.4f %.4f, periphery x%.2f",
        GetCurrentThreadId(), static_cast<unsigned long>(ptiming::g_topTid.load()), key[0], key[1], key[2], key[3],
        key[4], pct / 100.0);
}

uint64_t __fastcall Hook(void* self, void* opt, void* c, void* d) {
  const uint64_t r = g_orig(self, opt, c, d);
  g_calls.fetch_add(1, std::memory_order_relaxed);
  g_hookTid.store(GetCurrentThreadId(), std::memory_order_relaxed);
  const int pct = g_pct.load(std::memory_order_relaxed);
  if (pct <= 0 || pct >= 100 || !opt) return r;
  double key[5];
  if (!ReadKey(opt, key)) return r;
  if (g_ready.load(std::memory_order_relaxed) && pct == g_capPct.load() && self == g_capSelf &&
      memcmp(key, g_capKey, sizeof(key)) == 0)
    return r;
  if (GetCurrentThreadId() != ptiming::g_topTid.load(std::memory_order_relaxed)) {
    if (g_offThread.fetch_add(1) == 0)
      Log("periph lod: InstanceManager2 vt[24] runs on thread %lu, not the render thread %lu: no capture",
          GetCurrentThreadId(), static_cast<unsigned long>(ptiming::g_topTid.load()));
    return r;
  }
  if (g_capFail.load() >= 3 || !EnsureDevice()) return r;
  Capture(self, opt, c, d, pct, key);
  return r;
}

// ---------------------------------------------------------------------------
// Image capture (suite)
// ---------------------------------------------------------------------------

struct Img {
  int cfg = 0, view = 0, seq = 0;
  UINT w = 0, h = 0, fmt = 0;
  std::vector<uint8_t> px;  // tight rows, 4 bytes per pixel
};
struct Pending {
  ID3D11Texture2D* st = nullptr;
  int cfg = 0, view = 0, seq = 0;
  UINT w = 0, h = 0, fmt = 0;
  int age = 0;
};
std::atomic<int> g_capLeft{0};      // frames still to capture
std::atomic<int> g_capCfg{0};
std::atomic<bool> g_capFocus{false};  // also the focus views (first frame of a configuration only)
std::atomic<uint64_t> g_capNoRtv{0}, g_capBusy{0};
int g_capSeq = 0;
std::vector<Pending> g_pending;     // render thread
std::vector<ID3D11Texture2D*> g_free;  // render thread
std::mutex g_imgMutex;
std::vector<Img> g_imgs;

void PollPending(bool wait) {
  for (size_t i = 0; i < g_pending.size();) {
    Pending& p = g_pending[i];
    ++p.age;
    D3D11_MAPPED_SUBRESOURCE m{};
    const HRESULT hr = g_ctx->Map(p.st, 0, D3D11_MAP_READ, wait ? 0 : D3D11_MAP_FLAG_DO_NOT_WAIT, &m);
    if (hr == DXGI_ERROR_WAS_STILL_DRAWING) {
      ++i;
      continue;
    }
    if (SUCCEEDED(hr)) {
      Img im;
      im.cfg = p.cfg, im.view = p.view, im.seq = p.seq, im.w = p.w, im.h = p.h, im.fmt = p.fmt;
      im.px.resize(static_cast<size_t>(p.w) * p.h * 4);
      for (UINT y = 0; y < p.h; ++y)
        memcpy(im.px.data() + static_cast<size_t>(y) * p.w * 4, static_cast<uint8_t*>(m.pData) + y * m.RowPitch,
               p.w * 4);
      g_ctx->Unmap(p.st, 0);
      std::lock_guard<std::mutex> lock(g_imgMutex);
      g_imgs.push_back(std::move(im));
    }
    g_free.push_back(p.st);
    g_pending.erase(g_pending.begin() + i);
  }
}

void CaptureTarget(int view) {
  ID3D11RenderTargetView* rtv = nullptr;
  g_ctx->OMGetRenderTargets(1, &rtv, nullptr);
  if (!rtv) {
    ++g_capNoRtv;
    return;
  }
  ID3D11Resource* res = nullptr;
  rtv->GetResource(&res);
  rtv->Release();
  ID3D11Texture2D* tex = nullptr;
  if (res) res->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&tex));
  if (res) res->Release();
  if (!tex) return;
  D3D11_TEXTURE2D_DESC d{};
  tex->GetDesc(&d);
  const bool fmt32 = d.Format == DXGI_FORMAT_R8G8B8A8_TYPELESS || d.Format == DXGI_FORMAT_R8G8B8A8_UNORM ||
                     d.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB || d.Format == DXGI_FORMAT_B8G8R8A8_TYPELESS ||
                     d.Format == DXGI_FORMAT_B8G8R8A8_UNORM || d.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB ||
                     d.Format == DXGI_FORMAT_R10G10B10A2_UNORM || d.Format == DXGI_FORMAT_R10G10B10A2_TYPELESS;
  if (!fmt32 || d.SampleDesc.Count != 1 || g_pending.size() >= 24) {
    ++g_capBusy;
    tex->Release();
    return;
  }
  ID3D11Texture2D* st = nullptr;
  for (size_t i = 0; i < g_free.size(); ++i) {
    D3D11_TEXTURE2D_DESC fd{};
    g_free[i]->GetDesc(&fd);
    if (fd.Width == d.Width && fd.Height == d.Height && fd.Format == d.Format) {
      st = g_free[i];
      g_free.erase(g_free.begin() + i);
      break;
    }
  }
  if (!st) {
    D3D11_TEXTURE2D_DESC sd = d;
    sd.MipLevels = 1;
    sd.ArraySize = 1;
    sd.Usage = D3D11_USAGE_STAGING;
    sd.BindFlags = 0;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    sd.MiscFlags = 0;
    if (FAILED(g_dev->CreateTexture2D(&sd, nullptr, &st)) || !st) {
      ++g_capBusy;
      tex->Release();
      return;
    }
  }
  g_ctx->CopySubresourceRegion(st, 0, 0, 0, 0, tex, 0, nullptr);
  tex->Release();
  Pending p;
  p.st = st, p.cfg = g_capCfg.load(), p.view = view, p.seq = g_capSeq, p.w = d.Width, p.h = d.Height,
  p.fmt = d.Format;
  g_pending.push_back(p);
}

// ---------------------------------------------------------------------------
// View boundary (render thread)
// ---------------------------------------------------------------------------

using BFn = void (*)(void*, bool, int);
std::atomic<BFn> g_probe{nullptr};  // uav_overlap.h's callback, chained here
std::atomic<bool> g_installedCb{false};

void SetState(int s) {
  if (s == g_state) return;
  Put(s == 1);
  g_state = s;
  (s ? g_swaps : g_restores).fetch_add(1, std::memory_order_relaxed);
}

// Draw slot 13 of DCS's immediate context, hooked only from a CopyToHMDPass
// begin to its end (that pass issues one Draw into the view's HMD image).
using DrawFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT);
constexpr int kDrawSlot = 13;
DrawFn g_drawOrig = nullptr;
void** g_drawTable = nullptr;
int g_armView = -1;
bool g_armCheck = false, g_armCap = false;
UINT g_w0 = 0;
std::atomic<uint64_t> g_drawMissed{0};

void CheckOrder(int o, const D3D11_TEXTURE2D_DESC& d) {
  static int logged = 0;
  if (logged < 4) {
    ++logged;
    Log("periph lod: view order check: CopyToHMDPass #%d draws into %ux%u format %d", o, d.Width, d.Height,
        static_cast<int>(d.Format));
  }
  if (o == 0) g_w0 = d.Width;
  if (o == 1 && d.Width != g_w0) ++g_orderBad;
  if (o == 2) (d.Width > g_w0 ? g_orderOk : g_orderBad).fetch_add(1);
}

void STDMETHODCALLTYPE DrawHook(ID3D11DeviceContext* c, UINT n, UINT start) {
  g_drawOrig(c, n, start);
  if (c != g_ctx || g_armView < 0) return;
  const int o = g_armView;
  g_armView = -1;
  if (g_armCheck) {
    ID3D11RenderTargetView* rtv = nullptr;
    c->OMGetRenderTargets(1, &rtv, nullptr);
    if (rtv) {
      ID3D11Resource* r = nullptr;
      rtv->GetResource(&r);
      rtv->Release();
      ID3D11Texture2D* t = nullptr;
      if (r) r->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&t));
      if (r) r->Release();
      if (t) {
        D3D11_TEXTURE2D_DESC d{};
        t->GetDesc(&d);
        t->Release();
        CheckOrder(o, d);
      }
    }
  }
  if (g_armCap) CaptureTarget(o);
}

void ArmDraw(int o, bool check, bool cap) {
  void** vt = *reinterpret_cast<void***>(g_ctx);
  if (vt[kDrawSlot] == reinterpret_cast<void*>(&DrawHook)) return;
  g_drawOrig = reinterpret_cast<DrawFn>(vt[kDrawSlot]);
  g_drawTable = vt;
  g_armView = o;
  g_armCheck = check;
  g_armCap = cap;
  d3ds::WriteTablePointer(&vt[kDrawSlot], reinterpret_cast<void*>(&DrawHook));
}

void DisarmDraw() {
  if (!g_drawTable) return;
  if (g_armView >= 0) ++g_drawMissed;  // d3d11 replaced the entry before the draw, or no Draw
  g_armView = -1;
  if (g_drawTable[kDrawSlot] == reinterpret_cast<void*>(&DrawHook))
    d3ds::WriteTablePointer(&g_drawTable[kDrawSlot], reinterpret_cast<void*>(g_drawOrig));
  g_drawTable = nullptr;
}

void OnBoundary(void* pass, bool begin, int depth) {
  if (GetCurrentThreadId() != ptiming::g_topTid.load(std::memory_order_relaxed)) return;
  if (BFn pr = g_probe.load(std::memory_order_relaxed)) pr(pass, begin, depth);
  if (depth != 0 || !g_ctx) return;
  const int k = KindOf(pass);
  const bool ready = g_ready.load(std::memory_order_relaxed);
  if (begin) {
    if (!g_pending.empty()) PollPending(false);
    if (k == kHeight || k == kCascade) {
      g_lbOrd = 0;
      g_hmdOrd = 0;
      if (g_state && ready) SetState(0);
    } else if (k == kLights) {
      const int o = g_lbOrd++;
      if (!ready) return;
      if (o == 0 && g_swapOn.load(std::memory_order_relaxed))
        SetState(1);
      else if (o >= 2 || !g_swapOn.load(std::memory_order_relaxed))
        SetState(0);
    }
  } else if (k == kHmd) {
    const int o = g_hmdOrd++;
    DisarmDraw();
    if (g_capLeft.load(std::memory_order_relaxed) > 0 && o == 3) {
      g_capFocus = false;
      ++g_capSeq;
      g_capLeft.fetch_sub(1);
    }
  }
  if (begin && k == kHmd) {
    // The target is bound only during the pass's draw: catch it there.
    static uint32_t n = 0;
    const int o = g_hmdOrd;
    if (o == 0) ++n;
    const bool check = (n & 31) == 0 && o < 4;
    const bool cap = g_capLeft.load(std::memory_order_relaxed) > 0 &&
                     (o < 2 || (o < 4 && g_capFocus.load(std::memory_order_relaxed)));
    if (check || cap) ArmDraw(o, check, cap);
  }
}

// ---------------------------------------------------------------------------
// Control (config thread / suite thread)
// ---------------------------------------------------------------------------

bool Install() {
  if (g_slot) return true;
  HMODULE m = GetModuleHandleW(L"edterrainGraphics41.dll");
  if (!m) return false;
  auto** slot = reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(m) + kSlotRva);
  void* cur = SlotOriginal(slot);
  if (cur != reinterpret_cast<uint8_t*>(m) + kFnRva) {
    static bool logged = false;
    if (!logged) Log("periph lod: ETG+0x%x holds %p, not ETG+0x%x (other DCS build); not installed", kSlotRva, cur, kFnRva);
    logged = true;
    return false;
  }
  g_orig = reinterpret_cast<Fn>(cur);
  if (!HookSlot(slot, reinterpret_cast<void*>(&Hook), nullptr)) return false;
  g_slot = slot;
  return true;
}

// The boundary callback (also used by uav_overlap.h).
bool InstallCallback() {
  ptiming::Install();
  if (!ptiming::g_orig) return false;
  EnsureDevice();
  if (!g_ctx) return false;
  ptiming::g_viewBoundary = &OnBoundary;
  g_installedCb = true;
  return true;
}

// [Model] PeripheralLodScale (percent; 0 = off). Called every second from
// the config loop; cheap when nothing changes.
void Apply(int pct) {
  if (pct <= 0 || pct >= 100) {
    g_swapOn = false;
    g_pct = 0;
    return;  // the hook and callback stay (if ever installed): they restore stock tables at the next frame
  }
  if (!Install() || !InstallCallback()) return;
  if (g_pct.load() != pct) {
    g_pct = pct;
    Log("periph lod: periphery instancer LOD factors x%.2f requested (LOSSY prototype)", pct / 100.0);
  }
  g_swapOn = true;
}

// Waits until the render thread captured the tables (suite).
bool WaitReady(int ms) {
  for (int t = 0; t < ms; t += 20) {
    if (g_ready.load() && g_capPct.load() == g_pct.load()) return true;
    Sleep(20);
  }
  return false;
}

void LogCounters(const char* what) {
  Log("periph lod %s: vt[24] calls %llu, captures %llu (failures %llu, off-thread %llu), swaps to scaled %llu, "
      "back to stock %llu, view order checks ok %llu / wrong %llu",
      what, static_cast<unsigned long long>(g_calls.load()), static_cast<unsigned long long>(g_captures.load()),
      static_cast<unsigned long long>(g_capFail.load()), static_cast<unsigned long long>(g_offThread.load()),
      static_cast<unsigned long long>(g_swaps.load()), static_cast<unsigned long long>(g_restores.load()),
      static_cast<unsigned long long>(g_orderOk.load()), static_cast<unsigned long long>(g_orderBad.load()));
  Log("periph lod %s: CopyToHMDPass draws missed by the hook %llu", what,
      static_cast<unsigned long long>(g_drawMissed.load()));
}

// Suite: GPU pass timing OFF / ON / OFF / ON / OFF (5 s each).
void TimingPhase(int pct, int cfgPct, std::atomic<uint64_t>& frameCounter) {
  Apply(pct);
  if (!WaitReady(3000)) {
    Log("  periph lod: tables not captured; phase skipped");
    LogCounters("(not ready)");
    Apply(cfgPct);
    return;
  }
  for (int i = 0; i < 5; ++i) {
    const bool on = (i & 1) != 0;
    g_swapOn = on;
    Sleep(1500);
    Log("  periph lod timing run %d: %s", i + 1, on ? "ON (periphery tables scaled)" : "OFF (stock tables)");
    gpt::Measure(5000, frameCounter);
  }
  LogCounters("after timing");
  Apply(cfgPct);
}

void WriteImgs(const std::wstring& dir) {
  std::vector<Img> v;
  {
    std::lock_guard<std::mutex> lock(g_imgMutex);
    v.swap(g_imgs);
  }
  for (const Img& im : v) {
    wchar_t name[96];
    swprintf_s(name, L"c%d_v%d_%03d.raw", im.cfg, im.view, im.seq);
    FILE* f = nullptr;
    _wfopen_s(&f, (dir + name).c_str(), L"wb");
    if (!f) continue;
    const uint32_t hdr[4] = {im.w, im.h, im.fmt, 4};
    fwrite(hdr, sizeof(hdr), 1, f);
    fwrite(im.px.data(), 1, im.px.size(), f);
    fclose(f);
  }
}

// Suite: final per-view images, OFF / ON / OFF, `frames` consecutive frames each.
void ImagePhase(int pct, int cfgPct, int frames, const std::wstring& devDir) {
  const std::wstring dir = devDir + L"plod\\";
  CreateDirectoryW(dir.c_str(), nullptr);
  Apply(pct);
  if (!WaitReady(3000)) {
    Log("  periph lod: tables not captured; image phase skipped");
    Apply(cfgPct);
    return;
  }
  for (int cfg = 0; cfg < 3; ++cfg) {
    const bool on = cfg == 1;
    g_swapOn = on;
    Sleep(2500);  // DLSS history settles
    g_capCfg = cfg;
    g_capFocus = true;
    g_capLeft = frames;
    for (int t = 0; t < 20000 && g_capLeft.load() > 0; t += 20) Sleep(20);
    g_capLeft = 0;
    Sleep(300);  // last readbacks
    WriteImgs(dir);
    Log("  periph lod images: configuration %d (%s) captured, %d frames requested (no target %llu, skipped %llu)",
        cfg, on ? "ON" : "OFF", frames, static_cast<unsigned long long>(g_capNoRtv.load()),
        static_cast<unsigned long long>(g_capBusy.load()));
  }
  LogCounters("after images");
  Apply(cfgPct);
}

void Shutdown() {
  g_swapOn = false;
  g_pct = 0;
  g_capLeft = 0;
  if (g_installedCb.load()) {
    for (int i = 0; i < 50 && g_state; ++i) Sleep(10);  // the render thread puts stock tables back
    ptiming::BoundaryFn me = &OnBoundary;
    ptiming::g_viewBoundary.compare_exchange_strong(me, nullptr);
    Sleep(50);
    if (g_state && g_ctx) {
      Put(false);  // nothing rendered since: safe from here
      g_state = 0;
    }
  }
  if (g_drawTable && g_drawTable[kDrawSlot] == reinterpret_cast<void*>(&DrawHook))
    d3ds::WriteTablePointer(&g_drawTable[kDrawSlot], reinterpret_cast<void*>(g_drawOrig));
  g_drawTable = nullptr;
  if (g_slot) {
    UnhookSlot(g_slot, reinterpret_cast<void*>(g_orig));
    g_slot = nullptr;
    Sleep(50);  // a Capture already inside the hook finishes before its copies go
  }
  ReleaseCopies();
  for (auto& p : g_pending) p.st->Release();
  g_pending.clear();
  for (auto* t : g_free) t->Release();
  g_free.clear();
  if (g_ctx) g_ctx->Release();
  if (g_dev) g_dev->Release();
  g_ctx = nullptr;
  g_dev = nullptr;
}

}  // namespace plod
