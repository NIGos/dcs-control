// R84 (R1b): CockpitDepthExport off/on churn must not grow anything on DCS's device.
//
// Live (R82b arm C) PDH dedicated usage rose about 12-14 MB per depth-export off/on. Every flip of the export goes
// through fixation_depth.h Tick -> Uninstall -> Install (DepthWanted() is part of the install key), so this probe drives
// the same path on a test device with an OFXR-like reader (its own device, at most one opened set per block, the
// release fence signalled) and counts, per object type, every object our module creates on the device that is still
// alive (an IUnknown tag put on each object with SetPrivateDataInterface; D3D11 releases the tag when the object is
// destroyed: no debug layer needed). Plus the process handle count and the device's local video memory
// (QueryVideoMemoryInfo, the per-process figure PDH "GPU Process Memory\Dedicated Usage" reports).
//
// Included from cockpit_mask_test.h's pipeline (it needs that test's synthetic frame). QV_R84_N=<n> (default 6) sets
// the cycle count; QV_R84_REAL_GRACE=1 waits out kHandleGraceMs for real between flips (else the graves are closed at
// once after the reader let go).
#pragma once

#include <dxgi1_4.h>

#ifdef QV_R84_HW  // the investigation build: DCS's GPU and driver (driver-side video memory)
#define QV_R84_DRIVER D3D_DRIVER_TYPE_HARDWARE
#else
#define QV_R84_DRIVER D3D_DRIVER_TYPE_WARP
#endif

namespace r84 {

void __fastcall NoExec(void*, uint64_t) {}

enum Kind { kBuf, kTex, kSrv, kUav, kRtv, kDsv, kCs, kQuery, kFence, kKinds };
const char* const kNames[kKinds] = {"buffer", "texture2d", "srv", "uav", "rtv", "dsv", "cs", "query", "fence"};
std::atomic<long> g_live[kKinds];
std::atomic<long> g_made[kKinds];
ID3D11Device* g_dev = nullptr;  // only objects created on this device are counted
// {5B0E3A71-84B1-4C1B-9E3D-2A6F0C84D101}
const GUID kTagGuid = {0x5b0e3a71, 0x84b1, 0x4c1b, {0x9e, 0x3d, 0x2a, 0x6f, 0x0c, 0x84, 0xd1, 0x01}};

struct Tag final : IUnknown {
  std::atomic<ULONG> ref{1};
  int kind;
  explicit Tag(int k) : kind(k) {}
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** out) override {
    if (!out) return E_POINTER;
    if (id == __uuidof(IUnknown)) {
      *out = static_cast<IUnknown*>(this);
      AddRef();
      return S_OK;
    }
    *out = nullptr;
    return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return ++ref; }
  ULONG STDMETHODCALLTYPE Release() override {
    const ULONG n = --ref;
    if (n == 0) {
      g_live[kind]--;
      delete this;
    }
    return n;
  }
};

void Track(ID3D11DeviceChild* o, int kind) {
  if (!o) return;
  g_live[kind]++;
  g_made[kind]++;
  Tag* t = new Tag(kind);
  o->SetPrivateDataInterface(kTagGuid, t);  // the object holds the tag until it is destroyed
  t->Release();                             // failed: this was the last reference (counted back down)
}

// Device vtable slots (offsetof in ID3D11Device5Vtbl, checked with the SDK's C interface).
constexpr int kSlotBuffer = 3, kSlotTex2D = 5, kSlotSrv = 7, kSlotUav = 8, kSlotRtv = 9, kSlotDsv = 10, kSlotCs = 18,
              kSlotQuery = 24, kSlotFence = 68;
void** g_vt = nullptr;
void* g_orig[80] = {};

template <int Slot, int K, class Desc, class Out>
HRESULT STDMETHODCALLTYPE Create2(ID3D11Device* self, const Desc* d, Out** out) {
  using Fn = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, const Desc*, Out**);
  const HRESULT hr = reinterpret_cast<Fn>(g_orig[Slot])(self, d, out);
  if (SUCCEEDED(hr) && self == g_dev && out && *out) Track(*out, K);
  return hr;
}
template <int Slot, int K, class A, class Desc, class Out>
HRESULT STDMETHODCALLTYPE Create3(ID3D11Device* self, A a, const Desc* d, Out** out) {
  using Fn = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, A, const Desc*, Out**);
  const HRESULT hr = reinterpret_cast<Fn>(g_orig[Slot])(self, a, d, out);
  if (SUCCEEDED(hr) && self == g_dev && out && *out) Track(*out, K);
  return hr;
}
HRESULT STDMETHODCALLTYPE CreateCs(ID3D11Device* self, const void* code, SIZE_T n, ID3D11ClassLinkage* l,
                                   ID3D11ComputeShader** out) {
  using Fn = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, const void*, SIZE_T, ID3D11ClassLinkage*, ID3D11ComputeShader**);
  const HRESULT hr = reinterpret_cast<Fn>(g_orig[kSlotCs])(self, code, n, l, out);
  if (SUCCEEDED(hr) && self == g_dev && out && *out) Track(*out, kCs);
  return hr;
}
HRESULT STDMETHODCALLTYPE CreateFence(ID3D11Device* self, UINT64 v, D3D11_FENCE_FLAG f, REFIID id, void** out) {
  using Fn = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, UINT64, D3D11_FENCE_FLAG, REFIID, void**);
  const HRESULT hr = reinterpret_cast<Fn>(g_orig[kSlotFence])(self, v, f, id, out);
  if (SUCCEEDED(hr) && self == g_dev && out && *out) Track(static_cast<ID3D11Fence*>(*out), kFence);
  return hr;
}

void Patch(int slot, void* fn) {
  DWORD old = 0;
  VirtualProtect(&g_vt[slot], sizeof(void*), PAGE_READWRITE, &old);
  g_orig[slot] = g_vt[slot];
  g_vt[slot] = fn;
  VirtualProtect(&g_vt[slot], sizeof(void*), old, &old);
}
void Unpatch(int slot) {
  if (!g_orig[slot]) return;
  DWORD old = 0;
  VirtualProtect(&g_vt[slot], sizeof(void*), PAGE_READWRITE, &old);
  g_vt[slot] = g_orig[slot];
  VirtualProtect(&g_vt[slot], sizeof(void*), old, &old);
  g_orig[slot] = nullptr;
}

void Begin(ID3D11Device* dev) {
  g_dev = dev;
  g_vt = *reinterpret_cast<void***>(dev);
  Patch(kSlotBuffer, reinterpret_cast<void*>(&Create3<kSlotBuffer, kBuf, const D3D11_BUFFER_DESC*,
                                                      D3D11_SUBRESOURCE_DATA, ID3D11Buffer>));
  Patch(kSlotTex2D, reinterpret_cast<void*>(&Create3<kSlotTex2D, kTex, const D3D11_TEXTURE2D_DESC*,
                                                     D3D11_SUBRESOURCE_DATA, ID3D11Texture2D>));
  Patch(kSlotSrv, reinterpret_cast<void*>(&Create3<kSlotSrv, kSrv, ID3D11Resource*, D3D11_SHADER_RESOURCE_VIEW_DESC,
                                                   ID3D11ShaderResourceView>));
  Patch(kSlotUav, reinterpret_cast<void*>(&Create3<kSlotUav, kUav, ID3D11Resource*, D3D11_UNORDERED_ACCESS_VIEW_DESC,
                                                   ID3D11UnorderedAccessView>));
  Patch(kSlotRtv, reinterpret_cast<void*>(&Create3<kSlotRtv, kRtv, ID3D11Resource*, D3D11_RENDER_TARGET_VIEW_DESC,
                                                   ID3D11RenderTargetView>));
  Patch(kSlotDsv, reinterpret_cast<void*>(&Create3<kSlotDsv, kDsv, ID3D11Resource*, D3D11_DEPTH_STENCIL_VIEW_DESC,
                                                   ID3D11DepthStencilView>));
  Patch(kSlotCs, reinterpret_cast<void*>(&CreateCs));
  Patch(kSlotQuery, reinterpret_cast<void*>(&Create2<kSlotQuery, kQuery, D3D11_QUERY_DESC, ID3D11Query>));
  Patch(kSlotFence, reinterpret_cast<void*>(&CreateFence));
}
void End() {
  for (int s : {kSlotBuffer, kSlotTex2D, kSlotSrv, kSlotUav, kSlotRtv, kSlotDsv, kSlotCs, kSlotQuery, kSlotFence})
    Unpatch(s);
  g_dev = nullptr;
}

struct Snap {
  long live[kKinds] = {};
  long made[kKinds] = {};
  DWORD handles = 0;
  uint64_t local = 0;  // QueryVideoMemoryInfo LOCAL CurrentUsage (this process, the device's adapter)
};
uint64_t LocalUsage(ID3D11Device* dev) {
  uint64_t v = 0;
  IDXGIDevice* dx = nullptr;
  if (SUCCEEDED(dev->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&dx))) && dx) {
    IDXGIAdapter* a = nullptr;
    if (SUCCEEDED(dx->GetAdapter(&a)) && a) {
      IDXGIAdapter3* a3 = nullptr;
      if (SUCCEEDED(a->QueryInterface(__uuidof(IDXGIAdapter3), reinterpret_cast<void**>(&a3))) && a3) {
        DXGI_QUERY_VIDEO_MEMORY_INFO i{};
        if (SUCCEEDED(a3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &i))) v = i.CurrentUsage;
        a3->Release();
      }
      a->Release();
    }
    dx->Release();
  }
  return v;
}
Snap Take(ID3D11Device* dev) {
  Snap s;
  for (int k = 0; k < kKinds; ++k) {
    s.live[k] = g_live[k].load();
    s.made[k] = g_made[k].load();
  }
  GetProcessHandleCount(GetCurrentProcess(), &s.handles);
  s.local = LocalUsage(dev);
  return s;
}
std::string Diff(const Snap& a, const Snap& b) {
  std::string t;
  char x[96];
  for (int k = 0; k < kKinds; ++k) {
    sprintf_s(x, "%s%s %ld->%ld (made %ld)", t.empty() ? "" : ", ", kNames[k], a.live[k], b.live[k], b.made[k] - a.made[k]);
    t += x;
  }
  sprintf_s(x, "; handles %lu->%lu; local VRAM %+.2f MB", a.handles, b.handles,
            (static_cast<double>(b.local) - static_cast<double>(a.local)) / 1048576.0);
  t += x;
  return t;
}

// The OFXR-like reader of one block: its own mapping (kept across our reinstalls), on its own device at most one
// opened set of textures; a new generation releases the old set first, an inactive header releases it.
template <class H>
struct Reader {
  HANDLE map = nullptr;
  H* view = nullptr;
  uint32_t gen = 0;
  std::vector<ID3D11Texture2D*> tex;
  ID3D11Fence* fence = nullptr;  // the mask's release fence (mask block only)
  void Open(const wchar_t* name) {
    map = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, 4096, name);
    if (map) view = static_cast<H*>(MapViewOfFile(map, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(H)));
  }
  void Drop() {
    for (ID3D11Texture2D* t : tex)
      if (t) t->Release();
    tex.clear();
    if (fence) fence->Release();
    fence = nullptr;
    gen = 0;
  }
  void Close() {
    Drop();
    if (view) UnmapViewOfFile(view);
    if (map) CloseHandle(map);
    view = nullptr;
    map = nullptr;
  }
};

}  // namespace r84
