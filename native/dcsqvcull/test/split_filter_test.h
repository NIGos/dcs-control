// Offline test for split_filter.h (R15 F5): shadow compare logic, then a real
// D3D11 device (hardware, WARP fallback) driven through a fake dx11backend:
// code with the original call shapes (`mov rax,[rcx]; call [rax+off]`) and
// FX call tables of vcall thunks. A seeded random sequence of site calls,
// foreign calls through the context table, invalidators (ClearState,
// SwapDeviceContextState, ExecuteCommandList, *SetConstantBuffers1),
// simulated and real d3d11 table rewrites (SetMultithreadProtected) and
// deferred-context site calls runs twice: stock, then filtered. After every
// step the full bound state is read back with Get* and must be identical.
// Then the verify latch, and detach restoring every byte, entry and slot.
#pragma once

#include <d3d11_4.h>  // ID3D11Multithread

namespace sftest {

namespace sf = sfilt;
using Ctx = sf::Ctx;

// R48: f(arg) on a thread with a 256 KB stack. True when it did not return: a stack overflow (two hooks forwarding
// to each other) or still running after 10 s (the same cycle compiled to tail jumps). The thread is abandoned then.
struct R48Call {
  void (*fn)(void*);
  void* arg;
  volatile bool overflow;
};
DWORD WINAPI R48Thread(void* p) {
  R48Call* c = static_cast<R48Call*>(p);
  ULONG guarantee = 64 * 1024;  // room for the handler once the stack is gone
  SetThreadStackGuarantee(&guarantee);
  __try {
    c->fn(c->arg);
  } __except (GetExceptionCode() == EXCEPTION_STACK_OVERFLOW ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
    c->overflow = true;
  }
  return 0;
}
bool R48Cycles(void (*fn)(void*), void* arg) {
  static R48Call c;  // outlives an abandoned thread
  c = R48Call{fn, arg, false};
  HANDLE t = CreateThread(nullptr, 256 * 1024, &R48Thread, &c, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
  if (!t) return false;
  const bool done = WaitForSingleObject(t, 10000) == WAIT_OBJECT_0;
  CloseHandle(t);
  return !done || c.overflow;
}

void ShadowLogic() {
  void* a[4];
  sf::Shadow::Forget(a);
  void* x = reinterpret_cast<void*>(0x1000);
  void* y = reinterpret_cast<void*>(0x2000);
  const void* xy[2] = {x, y};
  Check(!sf::SameRange(a, 0, 2, xy), "split filter: unknown slots are never redundant");
  sf::StoreRange(a, 1, 2, xy);
  Check(sf::SameRange(a, 1, 2, xy) && a[0] == sf::kUnknown && a[3] == sf::kUnknown,
        "split filter: a stored range is redundant, neighbours stay unknown");
  const void* yx[2] = {y, x};
  Check(!sf::SameRange(a, 1, 2, yx), "split filter: one differing slot of a range is not redundant");
  Check(sf::SameRange(a, 2, 1, &xy[1]), "split filter: a sub-range of bound slots is redundant");
  Check(!sf::SameRange(a, 3, 2, xy) && !sf::SameRange(a, 4, 1, xy) && !sf::SameRange(a, 1, 0, xy) &&
            !sf::SameRange(a, 1, 2, nullptr),
        "split filter: out of range, empty and null arrays are not redundant");
  sf::StoreRange(a, 3, 2, xy);  // out of range: d3d11 ignores it, the stage becomes unknown
  Check(a[1] == sf::kUnknown && a[2] == sf::kUnknown, "split filter: an out-of-range set forgets the stage");
  sf::StoreRange(a, 0, 2, xy);
  sf::StoreRange(a, 0, 1, nullptr);
  Check(a[1] == sf::kUnknown, "split filter: a null array forgets the stage");
  const void* nulls[2] = {nullptr, nullptr};
  sf::StoreRange(a, 0, 2, nulls);
  Check(sf::SameRange(a, 0, 2, nulls), "split filter: null bindings are tracked like any value");
  float f[4];
  sf::Factor(nullptr, f);
  Check(f[0] == 1 && f[3] == 1, "split filter: null blend factor = {1,1,1,1}");
}

// Fake module layout.
constexpr uint32_t kFn[] = {0x100, 0x140, 0x180, 0x1c0, 0x200};  // blend, depth, raster, topo, layout
constexpr uint8_t kFnOp[] = {sf::kBlend, sf::kDepth, sf::kRS, sf::kTopo, sf::kIL};
constexpr uint32_t kThunks = 0x400, kTable = 0x800;
const sf::Site kSites[] = {{0x107, sf::kBlend}, {0x147, sf::kDepth}, {0x187, sf::kRS}, {0x1c7, sf::kTopo},
                           {0x207, sf::kIL}};
const sf::TableEntry kTables[] = {{kTable, sf::kVS},      {kTable + 8, sf::kVSCB},  {kTable + 16, sf::kVSSamp},
                                  {kTable + 24, sf::kPS}, {kTable + 32, sf::kPSCB}, {kTable + 40, sf::kPSSamp}};

// The fake module lives inside the test image, as a DLL's code does, so the
// filter's stubs always find memory within rel32 reach (a VirtualAlloc'd page
// at a random address failed InstallAt about once in 100 runs).
alignas(4096) uint8_t g_fakeImage[0x1000];
uint8_t* MakeFake() {
  auto* m = g_fakeImage;
  DWORD old = 0;
  if (!VirtualProtect(m, sizeof(g_fakeImage), PAGE_EXECUTE_READWRITE, &old)) return nullptr;
  memset(m, 0xCC, 0x1000);
  for (int i = 0; i < 5; ++i) {
    uint8_t* p = m + kFn[i];
    const uint8_t pre[] = {0x48, 0x83, 0xEC, 0x28, 0x48, 0x8B, 0x01, 0xFF, 0x90};  // sub rsp,28; mov rax,[rcx]; call [rax+
    memcpy(p, pre, sizeof(pre));
    const int32_t off = sf::MethodOffset(kFnOp[i]);
    memcpy(p + 9, &off, 4);
    const uint8_t post[] = {0x48, 0x83, 0xC4, 0x28, 0xC3};  // add rsp,28; ret
    memcpy(p + 13, post, sizeof(post));
  }
  for (int i = 0; i < 6; ++i) {
    uint8_t* t = m + kThunks + 16 * i;
    const int off = sf::MethodOffset(kTables[i].op);
    t[0] = 0x48, t[1] = 0x8B, t[2] = 0x01, t[3] = 0xFF;
    if (off < 0x80) {
      t[4] = 0x60, t[5] = static_cast<uint8_t>(off);
    } else {
      t[4] = 0xA0;
      memcpy(t + 5, &off, 4);
    }
    void* p = t;
    memcpy(m + kTables[i].rva, &p, 8);
  }
  FlushInstructionCache(GetCurrentProcess(), m, 0x1000);
  return m;
}

struct Snap {
  void *vs, *ps, *il, *blend, *depth, *rs;
  UINT nvs, nps, topo, mask, ref;
  float bf[4];
  void* vsCb[sf::kCb];
  void* psCb[sf::kCb];
  void* vsSamp[sf::kSamp];
  void* psSamp[sf::kSamp];
  bool operator==(const Snap& o) const { return memcmp(this, &o, sizeof(Snap)) == 0; }
};

template <class T>
void* Rel(T* p) {
  if (p) p->Release();
  return p;
}

Snap Take(Ctx* c) {
  Snap s;
  memset(&s, 0, sizeof(s));
  ID3D11VertexShader* vs = nullptr;
  c->VSGetShader(&vs, nullptr, &s.nvs);
  s.vs = Rel(vs);
  ID3D11PixelShader* ps = nullptr;
  c->PSGetShader(&ps, nullptr, &s.nps);
  s.ps = Rel(ps);
  ID3D11InputLayout* il = nullptr;
  c->IAGetInputLayout(&il);
  s.il = Rel(il);
  D3D11_PRIMITIVE_TOPOLOGY t = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
  c->IAGetPrimitiveTopology(&t);
  s.topo = t;
  ID3D11BlendState* b = nullptr;
  c->OMGetBlendState(&b, s.bf, &s.mask);
  s.blend = Rel(b);
  ID3D11DepthStencilState* d = nullptr;
  c->OMGetDepthStencilState(&d, &s.ref);
  s.depth = Rel(d);
  ID3D11RasterizerState* r = nullptr;
  c->RSGetState(&r);
  s.rs = Rel(r);
  ID3D11Buffer* cb[sf::kCb] = {};
  c->VSGetConstantBuffers(0, sf::kCb, cb);
  for (UINT i = 0; i < sf::kCb; ++i) s.vsCb[i] = Rel(cb[i]);
  c->PSGetConstantBuffers(0, sf::kCb, cb);
  for (UINT i = 0; i < sf::kCb; ++i) s.psCb[i] = Rel(cb[i]);
  ID3D11SamplerState* sm[sf::kSamp] = {};
  c->VSGetSamplers(0, sf::kSamp, sm);
  for (UINT i = 0; i < sf::kSamp; ++i) s.vsSamp[i] = Rel(sm[i]);
  c->PSGetSamplers(0, sf::kSamp, sm);
  for (UINT i = 0; i < sf::kSamp; ++i) s.psSamp[i] = Rel(sm[i]);
  return s;
}

struct Objects {
  ID3D11BlendState* blend[3] = {};
  ID3D11DepthStencilState* depth[3] = {};
  ID3D11RasterizerState* rs[3] = {};
  ID3D11SamplerState* samp[3] = {};
  ID3D11Buffer* cb[4] = {};
  ID3D11VertexShader* vs[2] = {};
  ID3D11PixelShader* ps[2] = {};
  ID3D11InputLayout* il[2] = {};
  ID3D11DeviceContext* def = nullptr;
  ID3D11CommandList* cl = nullptr;
  ID3DDeviceContextState* st = nullptr;
  ID3D11Multithread* mt = nullptr;
  bool cbOffsets = false;
};

bool Create(ID3D11Device* dev, Ctx* ctx, Objects& o) {
  for (int i = 0; i < 3; ++i) {
    D3D11_BLEND_DESC bd{};
    bd.RenderTarget[0].BlendEnable = i > 0;
    bd.RenderTarget[0].SrcBlend = i == 2 ? D3D11_BLEND_ONE : D3D11_BLEND_SRC_ALPHA;
    bd.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
    bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    dev->CreateBlendState(&bd, &o.blend[i]);
    D3D11_DEPTH_STENCIL_DESC dd{};
    dd.DepthEnable = i != 1;
    dd.DepthWriteMask = i == 2 ? D3D11_DEPTH_WRITE_MASK_ZERO : D3D11_DEPTH_WRITE_MASK_ALL;
    dd.DepthFunc = D3D11_COMPARISON_LESS;
    dd.StencilEnable = i == 2;
    dd.StencilReadMask = dd.StencilWriteMask = 0xFF;
    dd.FrontFace = dd.BackFace = {D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_REPLACE,
                                  D3D11_COMPARISON_ALWAYS};
    dev->CreateDepthStencilState(&dd, &o.depth[i]);
    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode = i == 2 ? D3D11_FILL_WIREFRAME : D3D11_FILL_SOLID;
    rd.CullMode = i == 1 ? D3D11_CULL_NONE : D3D11_CULL_BACK;
    rd.DepthClipEnable = TRUE;
    dev->CreateRasterizerState(&rd, &o.rs[i]);
    D3D11_SAMPLER_DESC sd{};
    sd.Filter = i == 0 ? D3D11_FILTER_MIN_MAG_MIP_LINEAR : i == 1 ? D3D11_FILTER_MIN_MAG_MIP_POINT : D3D11_FILTER_ANISOTROPIC;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
    sd.MaxAnisotropy = 4;
    sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    dev->CreateSamplerState(&sd, &o.samp[i]);
  }
  for (int i = 0; i < 4; ++i) {
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = 1024;  // 64 constants: room for *SetConstantBuffers1 offsets
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    dev->CreateBuffer(&bd, nullptr, &o.cb[i]);
  }
  HMODULE dll = LoadLibraryW(L"d3dcompiler_47.dll");
  auto compile = dll ? reinterpret_cast<decltype(&D3DCompile)>(GetProcAddress(dll, "D3DCompile")) : nullptr;
  if (!compile) return false;
  const char* vsSrc[2] = {"float4 main(float3 p : POSITION) : SV_Position { return float4(p, 1); }",
                          "cbuffer C : register(b0) { float4 k; }\n"
                          "float4 main(float3 p : POSITION, float2 t : TEXCOORD) : SV_Position "
                          "{ return float4(p, 1) + k * t.x; }"};
  const char* psSrc[2] = {"float4 main() : SV_Target { return 1; }", "float4 main() : SV_Target { return 0.5; }"};
  const D3D11_INPUT_ELEMENT_DESC el[2] = {
      {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
      {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0}};
  for (int i = 0; i < 2; ++i) {
    ID3DBlob* b = nullptr;
    if (FAILED(compile(vsSrc[i], strlen(vsSrc[i]), nullptr, nullptr, nullptr, "main", "vs_5_0", 0, 0, &b, nullptr)))
      return false;
    dev->CreateVertexShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &o.vs[i]);
    dev->CreateInputLayout(el, i + 1, b->GetBufferPointer(), b->GetBufferSize(), &o.il[i]);
    b->Release();
    if (FAILED(compile(psSrc[i], strlen(psSrc[i]), nullptr, nullptr, nullptr, "main", "ps_5_0", 0, 0, &b, nullptr)))
      return false;
    dev->CreatePixelShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &o.ps[i]);
    b->Release();
  }
  dev->CreateDeferredContext(0, &o.def);
  if (o.def) {
    o.def->RSSetState(o.rs[1]);
    o.def->FinishCommandList(FALSE, &o.cl);
  }
  ID3D11Device1* d1 = nullptr;
  if (SUCCEEDED(dev->QueryInterface(__uuidof(ID3D11Device1), reinterpret_cast<void**>(&d1)))) {
    D3D_FEATURE_LEVEL fl = dev->GetFeatureLevel();
    d1->CreateDeviceContextState(0, &fl, 1, D3D11_SDK_VERSION, __uuidof(ID3D11Device1), nullptr, &o.st);
    d1->Release();
  }
  D3D11_FEATURE_DATA_D3D11_OPTIONS opt{};
  o.cbOffsets = SUCCEEDED(dev->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS, &opt, sizeof(opt))) &&
                opt.ConstantBufferOffsetting;
  ctx->QueryInterface(__uuidof(ID3D11Multithread), reinterpret_cast<void**>(&o.mt));
  for (auto* p : o.blend)
    if (!p) return false;
  for (auto* p : o.cb)
    if (!p) return false;
  return o.vs[1] && o.ps[1] && o.il[1] && o.samp[2] && o.rs[2] && o.depth[2];
}

void Release(Objects& o) {
  auto rel = [](auto* p) {
    if (p) p->Release();
  };
  for (auto* p : o.blend) rel(p);
  for (auto* p : o.depth) rel(p);
  for (auto* p : o.rs) rel(p);
  for (auto* p : o.samp) rel(p);
  for (auto* p : o.cb) rel(p);
  for (auto* p : o.vs) rel(p);
  for (auto* p : o.ps) rel(p);
  for (auto* p : o.il) rel(p);
  rel(o.cl);
  rel(o.def);
  rel(o.st);
  rel(o.mt);
}

using BlendFn = void (*)(Ctx*, ID3D11BlendState*, const FLOAT*, UINT);
using DepthFn = void (*)(Ctx*, ID3D11DepthStencilState*, UINT);
using RsFn = void (*)(Ctx*, ID3D11RasterizerState*);
using TopoFn = void (*)(Ctx*, D3D11_PRIMITIVE_TOPOLOGY);
using IlFn = void (*)(Ctx*, ID3D11InputLayout*);
using VsFn = void(STDMETHODCALLTYPE*)(Ctx*, ID3D11VertexShader*, ID3D11ClassInstance* const*, UINT);
using PsFn = void(STDMETHODCALLTYPE*)(Ctx*, ID3D11PixelShader*, ID3D11ClassInstance* const*, UINT);
using CbFn = void(STDMETHODCALLTYPE*)(Ctx*, UINT, UINT, ID3D11Buffer* const*);
using SampFn = void(STDMETHODCALLTYPE*)(Ctx*, UINT, UINT, ID3D11SamplerState* const*);
template <class F>
F Entry2(uint8_t* m, int i) {
  return *reinterpret_cast<F*>(m + kTables[i].rva);
}

// One step of the script; `filtered` only changes the simulated table rewrite.
struct Script {
  uint8_t* m;
  Ctx* c;
  Objects* o;
  uint32_t rng;
  uint32_t R(uint32_t n) {
    rng = rng * 1664525u + 1013904223u;
    return (rng >> 8) % n;
  }
  template <class F>
  F Entry(int i) {
    return *reinterpret_cast<F*>(m + kTables[i].rva);  // read at call time, as ApplyShaderBlock does
  }
  void Step(bool filtered) {
    static const float kF[2][4] = {{0.5f, 0.25f, 1.0f, 0.0f}, {1, 1, 1, 1}};
    const D3D11_PRIMITIVE_TOPOLOGY topo[3] = {D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST,
                                              D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP,
                                              D3D11_PRIMITIVE_TOPOLOGY_LINELIST};
    const UINT refs[3] = {0, 1, 0x1FF};
    auto blend = [&] { return R(4) == 3 ? nullptr : o->blend[R(3)]; };
    ID3D11Buffer* cbs[3];
    ID3D11SamplerState* sms[3];
    const UINT start = R(3), n = 1 + R(3);
    for (UINT i = 0; i < 3; ++i) {
      cbs[i] = R(6) == 0 ? nullptr : o->cb[R(2) + (i & 1) * 2];
      sms[i] = R(6) == 0 ? nullptr : o->samp[R(3)];
    }
    uint32_t a = R(40);
    if (a >= 32 && a <= 38 && R(4)) a = R(32);  // invalidations and rewrites 4x rarer than state sets
    switch (a) {
      case 0: case 1: case 2: reinterpret_cast<BlendFn>(m + kFn[0])(c, blend(), R(3) ? nullptr : kF[R(2)], R(5) ? ~0u : 0xF); break;
      case 3: c->OMSetBlendState(blend(), R(2) ? nullptr : kF[0], ~0u); break;
      case 4: case 5: reinterpret_cast<DepthFn>(m + kFn[1])(c, R(4) ? o->depth[R(3)] : nullptr, refs[R(3)]); break;
      case 6: c->OMSetDepthStencilState(o->depth[R(3)], refs[R(3)]); break;
      case 7: case 8: reinterpret_cast<RsFn>(m + kFn[2])(c, R(4) ? o->rs[R(3)] : nullptr); break;
      case 9: c->RSSetState(o->rs[R(3)]); break;
      case 10: case 11: case 12: reinterpret_cast<TopoFn>(m + kFn[3])(c, topo[R(3)]); break;
      case 13: c->IASetPrimitiveTopology(topo[R(3)]); break;
      case 14: case 15: reinterpret_cast<IlFn>(m + kFn[4])(c, R(5) ? o->il[R(2)] : nullptr); break;
      case 16: c->IASetInputLayout(o->il[R(2)]); break;
      case 17: case 18: Entry<VsFn>(0)(c, R(5) ? o->vs[R(2)] : nullptr, nullptr, 0); break;
      case 19: c->VSSetShader(o->vs[R(2)], nullptr, 0); break;
      case 20: case 21: Entry<PsFn>(3)(c, R(5) ? o->ps[R(2)] : nullptr, nullptr, 0); break;
      case 22: c->PSSetShader(o->ps[R(2)], nullptr, 0); break;
      case 23: case 24: Entry<CbFn>(1)(c, start, n, cbs); break;
      case 25: c->VSSetConstantBuffers(start, n, cbs); break;
      case 26: case 27: Entry<CbFn>(4)(c, start, n, cbs); break;
      case 28: c->PSSetConstantBuffers(start, n, cbs); break;
      case 29: Entry<SampFn>(2)(c, start, n, sms); break;
      case 30: Entry<SampFn>(5)(c, start, n, sms); break;
      case 31: c->PSSetSamplers(start, n, sms); break;
      case 32:
        if (R(4) == 0) c->ClearState();
        break;
      case 33:  // another context state for one foreign call, then back
        if (o->st && R(3) == 0) {
          ID3DDeviceContextState* prev = nullptr;
          c->SwapDeviceContextState(o->st, &prev);
          c->RSSetState(o->rs[R(3)]);
          c->SwapDeviceContextState(prev, nullptr);
          if (prev) prev->Release();
        }
        break;
      case 34:
        if (o->cl && R(4) == 0) c->ExecuteCommandList(o->cl, R(2));
        break;
      case 35:
        if (o->cbOffsets) {
          const UINT first[1] = {16}, num[1] = {16};
          if (R(2))
            c->VSSetConstantBuffers1(R(2), 1, &o->cb[R(4)], first, num);
          else
            c->PSSetConstantBuffers1(R(2), 1, &o->cb[R(4)], first, num);
        }
        break;
      case 36:  // d3d11 rewrites the table (simulated): the next foreign call is not seen
        if (filtered) {
          for (int i = 0; i < sf::kHookCount; ++i)
            d3ds::WriteTablePointer(&sf::g_table[sf::g_slot[i]], sf::g_orig[i]);
        }
        c->RSSetState(o->rs[R(3)]);
        c->IASetPrimitiveTopology(topo[R(3)]);
        c->VSSetConstantBuffers(0, 1, &o->cb[R(4)]);
        break;
      case 37:  // a real full rewrite
        if (o->mt && R(3) == 0) {
          o->mt->SetMultithreadProtected(TRUE);
          c->PSSetShader(o->ps[R(2)], nullptr, 0);
          o->mt->SetMultithreadProtected(FALSE);
        }
        break;
      case 38:  // a deferred context through the sites: never filtered, never in the shadow
        if (o->def) {
          reinterpret_cast<RsFn>(m + kFn[2])(reinterpret_cast<Ctx*>(o->def), o->rs[R(3)]);
          Entry<CbFn>(1)(reinterpret_cast<Ctx*>(o->def), 0, 1, &o->cb[R(4)]);
        }
        break;
      default: c->VSSetSamplers(start, n, sms); break;
    }
  }
};

void Run() {
  ShadowLogic();
  ID3D11Device* dev = nullptr;
  ID3D11DeviceContext* ic = nullptr;
  D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
  if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &fl, 1, D3D11_SDK_VERSION, &dev, nullptr,
                               &ic)) &&
      FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, &fl, 1, D3D11_SDK_VERSION, &dev, nullptr,
                               &ic))) {
    QvSkip("split filter: no D3D11 device\n");
    return;
  }
  Ctx* c = nullptr;
  ic->QueryInterface(__uuidof(ID3D11DeviceContext1), reinterpret_cast<void**>(&c));
  Objects o;
  uint8_t* m = MakeFake();
  if (!c || !m || !Create(dev, c, o)) {
    Check(false, "split filter: test device objects");
    return;
  }
  uint8_t before[0x1000];
  memcpy(before, m, sizeof(before));

  sf::g_sites = kSites;
  sf::g_siteCount = 5;
  sf::g_tables = kTables;
  sf::g_tableCount = 6;
  Check(sf::InstallAt(m, false), "split filter: fake sites and call tables pass the checks");

  constexpr int kSteps = 6000;
  std::vector<Snap> stock(kSteps);
  Script s{m, c, &o, 12345};
  c->ClearState();
  for (int i = 0; i < kSteps; ++i) {
    s.Step(false);
    stock[i] = Take(c);
  }

  // Attach with every op filtered: sites patched, then the first topology site call hands over the context.
  sf::g_opMask = sf::kAllOps;
  Check(!sf::Attach() && sf::g_sitesPatched.load() && !sf::Live(), "split filter: sites patched, context not known yet");
  Check(m[0x1c7] == 0x90 && m[0x1c8] == 0xE8 && *reinterpret_cast<void**>(m + kTable + 8) == sf::kFilters[sf::kVSCB],
        "split filter: site is nop + call rel32, call-table entry points at the filter");
  reinterpret_cast<TopoFn>(m + kFn[3])(c, D3D11_PRIMITIVE_TOPOLOGY_POINTLIST);
  Check(sf::g_ctx.load() == c, "split filter: immediate context captured at a renderer site");
  Check(sf::Attach() && sf::Live(), "split filter: context table hooked, filter live");

  sf::ResetCounters();
  sf::g_count = true;
  s = Script{m, c, &o, 12345};
  c->ClearState();
  int bad = 0, firstBad = -1;
  for (int i = 0; i < kSteps; ++i) {
    s.Step(true);
    if (!(Take(c) == stock[i])) {
      if (firstBad < 0) firstBad = i;
      ++bad;
    }
  }
  sf::g_count = false;
  uint64_t calls = 0, skipped = 0, foreign = 0;
  for (int i = 0; i < sf::kOpCount; ++i) {
    calls += sf::g_c.calls[i];
    skipped += sf::g_c.skipped[i];
    foreign += sf::g_c.foreign[i];
  }
  printf("  split filter: %d steps, %llu site calls, %llu skipped, %llu foreign, %llu invalidations, %llu table "
         "rewrites found at a site, %llu re-hooks, first differing step %d\n",
         kSteps, static_cast<unsigned long long>(calls), static_cast<unsigned long long>(skipped),
         static_cast<unsigned long long>(foreign), static_cast<unsigned long long>(sf::g_c.resets),
         static_cast<unsigned long long>(sf::g_c.lost), static_cast<unsigned long long>(sf::g_rewrites.load()),
         firstBad);
  Check(bad == 0, "split filter: bound state after every step equals the stock run (Get* read-back)");
  Check(skipped > calls / 10 && foreign > 0 && sf::g_c.resets > 0 && sf::g_c.lost > 0,
        "split filter: skips, foreign calls, invalidations and table rewrites all exercised");
  Check(sf::g_c.site[3] > 0 && sf::g_c.site[sf::g_siteCount + 1] > 0, "split filter: per-site counters");

  // Cost of a redundant call through a site, filtered (here) and stock (after
  // detach): informational, printed only.
  auto timeRedundant = [&](double out[5]) {
    const int kN = 1 << 20;
    LARGE_INTEGER f, t0, t1;
    QueryPerformanceFrequency(&f);
    auto rs = reinterpret_cast<RsFn>(m + kFn[2]);
    auto topoFn = reinterpret_cast<TopoFn>(m + kFn[3]);
    rs(c, o.rs[0]);
    QueryPerformanceCounter(&t0);
    for (int i = 0; i < kN; ++i) rs(c, o.rs[0]);
    QueryPerformanceCounter(&t1);
    out[0] = (t1.QuadPart - t0.QuadPart) * 1e9 / f.QuadPart / kN;
    topoFn(c, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    QueryPerformanceCounter(&t0);
    for (int i = 0; i < kN; ++i) topoFn(c, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    QueryPerformanceCounter(&t1);
    out[1] = (t1.QuadPart - t0.QuadPart) * 1e9 / f.QuadPart / kN;
    ID3D11Buffer* cb2[2] = {o.cb[0], o.cb[2]};
    Entry2<CbFn>(m, 1)(c, 0, 2, cb2);
    QueryPerformanceCounter(&t0);
    for (int i = 0; i < kN; ++i) Entry2<CbFn>(m, 1)(c, 0, 2, cb2);
    QueryPerformanceCounter(&t1);
    out[2] = (t1.QuadPart - t0.QuadPart) * 1e9 / f.QuadPart / kN;
    auto bl = reinterpret_cast<BlendFn>(m + kFn[0]);
    bl(c, o.blend[1], nullptr, ~0u);
    QueryPerformanceCounter(&t0);
    for (int i = 0; i < kN; ++i) bl(c, o.blend[1], nullptr, ~0u);
    QueryPerformanceCounter(&t1);
    out[3] = (t1.QuadPart - t0.QuadPart) * 1e9 / f.QuadPart / kN;
    auto dp = reinterpret_cast<DepthFn>(m + kFn[1]);
    dp(c, o.depth[1], 0);
    QueryPerformanceCounter(&t0);
    for (int i = 0; i < kN; ++i) dp(c, o.depth[1], 0);
    QueryPerformanceCounter(&t1);
    out[4] = (t1.QuadPart - t0.QuadPart) * 1e9 / f.QuadPart / kN;
  };
  double filteredNs[5], stockNs[5];
  timeRedundant(filteredNs);

  // Verify: a shadow that lies is caught by the read-back, the call goes through, the filter latches off.
  reinterpret_cast<RsFn>(m + kFn[2])(c, o.rs[1]);
  sf::g_sh.rs = o.rs[2];  // pretend rs[2] is bound
  sf::g_verify = true;
  sf::g_count = true;
  reinterpret_cast<RsFn>(m + kFn[2])(c, o.rs[2]);
  sf::g_verify = false;
  sf::g_count = false;
  ID3D11RasterizerState* got = nullptr;
  c->RSGetState(&got);
  Check(got == o.rs[2] && sf::g_c.mismatch[sf::kRS] == 1 && sf::g_disabled.load() && !sf::Live(),
        "split filter: verify mismatch forwards the call and latches the filter off");
  if (got) got->Release();
  reinterpret_cast<RsFn>(m + kFn[2])(c, o.rs[0]);  // latched: forwarded through the table
  c->RSGetState(&got);
  Check(got == o.rs[0], "split filter: latched off, calls still reach d3d11");
  if (got) got->Release();

  sf::Detach();
  timeRedundant(stockNs);
  printf("  split filter: redundant call through a site, ns filtered/stock: raster %.1f/%.1f, topology %.1f/%.1f, "
         "VS CB x2 %.1f/%.1f, blend %.1f/%.1f, depth %.1f/%.1f\n",
         filteredNs[0], stockNs[0], filteredNs[1], stockNs[1], filteredNs[2], stockNs[2], filteredNs[3], stockNs[3],
         filteredNs[4], stockNs[4]);
  bool slotsClean = true;
  void** vt = *reinterpret_cast<void***>(c);
  for (int i = 0; i < sf::kHookCount; ++i)
    if (vt[sf::g_slot[i]] == sf::kHooks[i]) slotsClean = false;
  Check(memcmp(before, m, sizeof(before)) == 0 && slotsClean && !sf::g_sitesPatched.load() && !sf::g_hooked.load(),
        "split filter: detach restores every site byte, call-table entry and context slot");

  // Default ops: blend and depth keep DCS's own call (no site patch, no hook).
  sf::g_disabled = false;
  sf::g_opMask = sf::kDefaultOps;
  const bool live = sf::Attach() && sf::Live();
  vt = *reinterpret_cast<void***>(c);
  Check(live && m[0x107] == 0xFF && m[0x147] == 0xFF && m[0x187] == 0x90 && m[0x1c7] == 0x90 &&
            vt[sf::g_slot[sf::kBlend]] != sf::kHooks[sf::kBlend] && vt[sf::g_slot[sf::kRS]] == sf::kHooks[sf::kRS],
        "split filter: default ops leave the blend and depth sites and slots alone");
  reinterpret_cast<BlendFn>(m + kFn[0])(c, o.blend[2], nullptr, ~0u);
  reinterpret_cast<RsFn>(m + kFn[2])(c, o.rs[2]);
  Snap after = Take(c);
  Check(after.blend == o.blend[2] && after.rs == o.rs[2], "split filter: default ops, calls reach d3d11");
  {
    // R48 (the R47 crash): another hook of this payload in one of our slots that took ours as its original
    // (fixation_depth.h's RSSetState toggle point, armed over our hook). The next site call finds the slot not ours;
    // before R48 the re-hook took that hook as d3d11's entry: ours -> it -> ours, a stack overflow on that call.
    void** vtc = *reinterpret_cast<void***>(c);
    const int rs = sf::g_slot[sf::kRS];
    void* const real = sf::g_orig[sf::kRS];
    void* const tpOrig0 = fixdepth::g_tpOrig[fixdepth::kTpRSSetState];
    fixdepth::g_tpOrig[fixdepth::kTpRSSetState] = sf::kHooks[sf::kRS];
    d3ds::WriteTablePointer(&vtc[rs], fixdepth::TogglePointHook(fixdepth::kTpRSSetState));
    struct Arg {
      uint8_t* m;
      Ctx* c;
      ID3D11RasterizerState* r;
    } a{m, c, o.rs[1]};
    const bool cycles = R48Cycles([](void* p) {
      Arg* x = static_cast<Arg*>(p);
      reinterpret_cast<RsFn>(x->m + kFn[2])(x->c, x->r);  // the raster site
    }, &a);
    ID3D11RasterizerState* gotRs = nullptr;
    c->RSGetState(&gotRs);
    if (gotRs) gotRs->Release();
    const bool kept = vtc[rs] == fixdepth::TogglePointHook(fixdepth::kTpRSSetState) && sf::g_orig[sf::kRS] == real;
#ifndef R48_PREFIX
    const bool counted = sf::g_oursOnTop.load() >= 1;
#else
    const bool counted = true;
#endif
    Check(!cycles && kept && gotRs == o.rs[1] && counted,
          "R48 split filter: no re-hook over another hook of ours in its slot (no call cycle; the call reaches d3d11)");
    sf::g_orig[sf::kRS] = real;
    d3ds::WriteTablePointer(&vtc[rs], sf::kHooks[sf::kRS]);
    fixdepth::g_tpOrig[fixdepth::kTpRSSetState] = tpOrig0;
    reinterpret_cast<RsFn>(m + kFn[2])(c, o.rs[2]);  // ours again: the call forgets nothing it should keep
    c->RSGetState(&gotRs);
    if (gotRs) gotRs->Release();
    Check(gotRs == o.rs[2], "R48 split filter: hook back after the other hook left, calls reach d3d11");
  }
  sf::Detach();
  Check(memcmp(before, m, sizeof(before)) == 0, "split filter: detach (default ops) restores the module");

  // Leave the module as a fresh payload would find it.
  sf::g_ctx = nullptr;
  sf::g_state = 0;
  sf::g_sites = sf::kSites;
  sf::g_siteCount = static_cast<int>(sizeof(sf::kSites) / sizeof(sf::kSites[0]));
  sf::g_tables = sf::kTables;
  sf::g_tableCount = static_cast<int>(sizeof(sf::kTables) / sizeof(sf::kTables[0]));
  sf::ResetCounters();
  Release(o);
  c->Release();
  ic->Release();
  dev->Release();
}

}  // namespace sftest
