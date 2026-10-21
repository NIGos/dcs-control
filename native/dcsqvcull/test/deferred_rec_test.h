// Offline test for deferred_rec.h (R15 A1 infrastructure) on a real device
// (WARP when there is none):
//  - a synthetic depth-only "cascade" (thousands of small indexed draws,
//    per-draw VS constants through the CB ring, instanced draws reading a
//    t127 offset buffer, a discard PS on some draws, depth GREATER, a depth
//    bias rasterizer state) drawn on the immediate context, then recorded by
//    the worker pool on deferred contexts: 4 quarters into 4 targets, the
//    whole cascade as one list, and 4 quarter lists into one target in
//    order; depth compared bit for bit after a staging readback, for every
//    CB strategy;
//  - ExecuteCommandList(TRUE) leaves the immediate context's state as it was
//    (PassState Get* snapshot before and after, with a ranged CB binding);
//  - Apply reproduces a captured state on a deferred context;
//  - a faulting job latches the pool off, a failing job does not, a job past
//    the wait timeout is dropped and the worker refused until it finished;
//    start/stop cycles release every device reference.
// QV_DEFREC_BENCH=<rounds>: render-thread timing (CB strategies, immediate
// 4 x 5,000 draws vs pool record + 4 ExecuteCommandList), 3 runs.
#pragma once

namespace drtest {
using namespace defrec;

constexpr UINT kSize = 1024;
constexpr int kInstances = 8;

struct DrawData {
  float m[16];
  float params[4];  // x = first instance offset (instanced draws)
  bool inst;
  bool discard;
};

struct Scene {
  ID3D11Device* dev = nullptr;
  ID3D11DeviceContext* imm = nullptr;
  ID3D11VertexShader* vs = nullptr;
  ID3D11VertexShader* vsInst = nullptr;
  ID3D11PixelShader* psDiscard = nullptr;
  ID3D11PixelShader* psColor = nullptr;
  ID3D11InputLayout* il = nullptr;
  ID3D11Buffer* vb = nullptr;
  ID3D11Buffer* ib = nullptr;
  ID3D11Buffer* refCb = nullptr;     // dynamic 80 B, Map DISCARD per draw (the stock way)
  ID3D11Buffer* passCb[4] = {};      // per-cascade transform (b1)
  ID3D11Buffer* instBuf = nullptr;   // t0: float4 per instance
  ID3D11Buffer* offBuf = nullptr;    // t127: uint offsets
  ID3D11ShaderResourceView* instSrv = nullptr;
  ID3D11ShaderResourceView* offSrv = nullptr;
  ID3D11RasterizerState* rs = nullptr;
  ID3D11DepthStencilState* dss = nullptr;
  ID3D11BlendState* bs = nullptr;
  ID3D11SamplerState* samp = nullptr;
  ID3D11Texture2D* depth[4] = {};
  ID3D11DepthStencilView* dsv[4] = {};
  ID3D11Texture2D* staging = nullptr;
  // A different ("foreign", DCS-like) immediate state for the restore check.
  ID3D11Texture2D* color = nullptr;
  ID3D11RenderTargetView* rtv = nullptr;
  ID3D11Buffer* bigCb = nullptr;  // 4 KB, bound ranged with *SetConstantBuffers1
  ID3D11RasterizerState* rs2 = nullptr;
  ID3D11DepthStencilState* dss2 = nullptr;
  ID3D11BlendState* bs2 = nullptr;
  std::vector<DrawData> draws;
};

const char kHlsl[] = R"(
cbuffer DrawCb : register(b0) { float4x4 m; float4 params; };
cbuffer PassCb : register(b1) { float4 cascade; };
StructuredBuffer<float4> inst : register(t0);
StructuredBuffer<uint> offs : register(t127);
float4 Out(float3 p) {
  float4 w = mul(float4(p, 1), m);
  return float4(w.xy * cascade.xy + cascade.zw, w.z, w.w);
}
float4 vs(float3 p : POSITION) : SV_Position { return Out(p); }
float4 vsInst(float3 p : POSITION, uint id : SV_InstanceID) : SV_Position {
  float4 o = inst[offs[(uint)params.x + id]];
  return Out(p * o.w + o.xyz);
}
void psDiscard(float4 pos : SV_Position) { if (frac(pos.x * 0.37 + pos.y * 0.23) > 0.8) discard; }
float4 psColor(float4 pos : SV_Position) : SV_Target { return pos.z; }
)";

template <class T>
void Rel(T*& p) {
  if (p) p->Release();
  p = nullptr;
}

uint32_t g_rng = 1;
float Rnd(float a, float b) {
  g_rng = g_rng * 1664525u + 1013904223u;
  return a + (b - a) * static_cast<float>(g_rng >> 8) / 16777216.0f;
}

bool Create(Scene& s, int drawCount) {
  ID3D11Device* dev = s.dev;
  HMODULE dll = LoadLibraryW(L"d3dcompiler_47.dll");
  auto compile = dll ? reinterpret_cast<decltype(&D3DCompile)>(GetProcAddress(dll, "D3DCompile")) : nullptr;
  if (!compile) return false;
  auto build = [&](const char* entry, const char* target, ID3DBlob** out) {
    ID3DBlob* err = nullptr;
    HRESULT hr = compile(kHlsl, sizeof(kHlsl) - 1, "defrec", nullptr, nullptr, entry, target,
                         D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, out, &err);
    if (FAILED(hr)) printf("     compile %s: %s\n", entry, err ? static_cast<const char*>(err->GetBufferPointer()) : "?");
    Rel(err);
    return SUCCEEDED(hr);
  };
  ID3DBlob* b = nullptr;
  if (!build("vs", "vs_5_0", &b)) return false;
  dev->CreateVertexShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &s.vs);
  const D3D11_INPUT_ELEMENT_DESC el[] = {{"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0}};
  dev->CreateInputLayout(el, 1, b->GetBufferPointer(), b->GetBufferSize(), &s.il);
  Rel(b);
  if (!build("vsInst", "vs_5_0", &b)) return false;
  dev->CreateVertexShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &s.vsInst);
  Rel(b);
  if (!build("psDiscard", "ps_5_0", &b)) return false;
  dev->CreatePixelShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &s.psDiscard);
  Rel(b);
  if (!build("psColor", "ps_5_0", &b)) return false;
  dev->CreatePixelShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &s.psColor);
  Rel(b);

  const float v[8][3] = {{-1, -1, -1}, {1, -1, -1}, {1, 1, -1}, {-1, 1, -1},
                         {-1, -1, 1},  {1, -1, 1},  {1, 1, 1},  {-1, 1, 1}};
  const uint16_t idx[36] = {0, 1, 2, 0, 2, 3, 4, 6, 5, 4, 7, 6, 0, 4, 5, 0, 5, 1,
                            3, 2, 6, 3, 6, 7, 1, 5, 6, 1, 6, 2, 0, 3, 7, 0, 7, 4};
  D3D11_BUFFER_DESC bd{};
  D3D11_SUBRESOURCE_DATA sd{};
  bd.Usage = D3D11_USAGE_IMMUTABLE;
  bd.ByteWidth = sizeof(v);
  bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
  sd.pSysMem = v;
  dev->CreateBuffer(&bd, &sd, &s.vb);
  bd.ByteWidth = sizeof(idx);
  bd.BindFlags = D3D11_BIND_INDEX_BUFFER;
  sd.pSysMem = idx;
  dev->CreateBuffer(&bd, &sd, &s.ib);

  bd = {};
  bd.ByteWidth = 80;
  bd.Usage = D3D11_USAGE_DYNAMIC;
  bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
  bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
  dev->CreateBuffer(&bd, nullptr, &s.refCb);
  for (int k = 0; k < 4; ++k) {
    const float c[4] = {1.0f - 0.15f * k, 0.9f + 0.05f * k, 0.03f * k, -0.02f * k};
    bd = {};
    bd.ByteWidth = 16;
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    sd.pSysMem = c;
    dev->CreateBuffer(&bd, &sd, &s.passCb[k]);
  }
  bd = {};
  bd.ByteWidth = 4096;
  bd.Usage = D3D11_USAGE_DEFAULT;
  bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
  dev->CreateBuffer(&bd, nullptr, &s.bigCb);

  // Draws: every 10th instanced (8 instances through t127), every 7th with the discard PS.
  g_rng = 12345;
  s.draws.resize(drawCount);
  int instDraws = 0;
  for (int i = 0; i < drawCount; ++i) {
    DrawData& d = s.draws[i];
    memset(&d, 0, sizeof(d));
    const float sc = Rnd(0.004f, 0.03f);
    // Column-major cbuffer: float 4c..4c+3 are the coefficients of output c.
    const float m[16] = {sc, Rnd(-0.01f, 0.01f), 0, Rnd(-1.0f, 1.0f),
                         0,  sc,                  0, Rnd(-1.0f, 1.0f),
                         Rnd(-0.05f, 0.05f), Rnd(-0.05f, 0.05f), Rnd(0.005f, 0.02f), Rnd(0.1f, 0.9f),
                         0,  0,                   0, 1};
    memcpy(d.m, m, sizeof(m));
    d.inst = i % 10 == 9;
    d.discard = i % 7 == 3;
    if (d.inst) d.params[0] = static_cast<float>(instDraws++ * kInstances);
    d.params[1] = Rnd(0, 1);
  }
  const int instCount = (instDraws > 0 ? instDraws : 1) * kInstances;
  std::vector<float> inst(instCount * 4);
  std::vector<uint32_t> off(instCount);
  for (int i = 0; i < instCount; ++i) {
    inst[i * 4 + 0] = Rnd(-2.0f, 2.0f);
    inst[i * 4 + 1] = Rnd(-2.0f, 2.0f);
    inst[i * 4 + 2] = Rnd(-1.0f, 1.0f);
    inst[i * 4 + 3] = Rnd(0.3f, 1.0f);
    off[i] = static_cast<uint32_t>(instCount - 1 - i);  // reversed: the offset buffer matters
  }
  bd = {};
  bd.Usage = D3D11_USAGE_IMMUTABLE;
  bd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
  bd.ByteWidth = instCount * 16;
  bd.StructureByteStride = 16;
  sd.pSysMem = inst.data();
  dev->CreateBuffer(&bd, &sd, &s.instBuf);
  bd.ByteWidth = instCount * 4;
  bd.StructureByteStride = 4;
  sd.pSysMem = off.data();
  dev->CreateBuffer(&bd, &sd, &s.offBuf);
  if (!s.instBuf || !s.offBuf) return false;
  dev->CreateShaderResourceView(s.instBuf, nullptr, &s.instSrv);
  dev->CreateShaderResourceView(s.offBuf, nullptr, &s.offSrv);

  D3D11_RASTERIZER_DESC rd{};
  rd.FillMode = D3D11_FILL_SOLID;
  rd.CullMode = D3D11_CULL_NONE;
  rd.DepthBias = 50;
  rd.SlopeScaledDepthBias = 2.0f;
  rd.DepthBiasClamp = 0.0f;
  rd.DepthClipEnable = TRUE;
  dev->CreateRasterizerState(&rd, &s.rs);
  rd.CullMode = D3D11_CULL_FRONT;
  rd.DepthBias = 0;
  rd.SlopeScaledDepthBias = 0;
  rd.ScissorEnable = TRUE;
  dev->CreateRasterizerState(&rd, &s.rs2);
  D3D11_DEPTH_STENCIL_DESC dd{};
  dd.DepthEnable = TRUE;
  dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
  dd.DepthFunc = D3D11_COMPARISON_GREATER;
  dev->CreateDepthStencilState(&dd, &s.dss);
  dd.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
  dd.StencilEnable = TRUE;
  dd.StencilReadMask = dd.StencilWriteMask = 0xff;
  dd.FrontFace = {D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_REPLACE, D3D11_COMPARISON_ALWAYS};
  dd.BackFace = dd.FrontFace;
  dev->CreateDepthStencilState(&dd, &s.dss2);
  D3D11_BLEND_DESC bl{};
  bl.RenderTarget[0].RenderTargetWriteMask = 0;  // depth only
  dev->CreateBlendState(&bl, &s.bs);
  bl.RenderTarget[0].BlendEnable = TRUE;
  bl.RenderTarget[0].SrcBlend = D3D11_BLEND_BLEND_FACTOR;
  bl.RenderTarget[0].DestBlend = D3D11_BLEND_INV_BLEND_FACTOR;
  bl.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
  bl.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
  bl.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
  bl.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
  bl.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
  dev->CreateBlendState(&bl, &s.bs2);
  D3D11_SAMPLER_DESC smp{};
  smp.Filter = D3D11_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;
  smp.AddressU = smp.AddressV = smp.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
  smp.ComparisonFunc = D3D11_COMPARISON_GREATER_EQUAL;
  smp.MaxLOD = D3D11_FLOAT32_MAX;
  dev->CreateSamplerState(&smp, &s.samp);

  D3D11_TEXTURE2D_DESC td{};
  td.Width = td.Height = kSize;
  td.MipLevels = td.ArraySize = 1;
  td.Format = DXGI_FORMAT_R32_TYPELESS;
  td.SampleDesc.Count = 1;
  td.Usage = D3D11_USAGE_DEFAULT;
  td.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
  D3D11_DEPTH_STENCIL_VIEW_DESC dvd{};
  dvd.Format = DXGI_FORMAT_D32_FLOAT;
  dvd.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
  for (int k = 0; k < 4; ++k) {
    dev->CreateTexture2D(&td, nullptr, &s.depth[k]);
    if (!s.depth[k]) return false;
    dev->CreateDepthStencilView(s.depth[k], &dvd, &s.dsv[k]);
  }
  td.BindFlags = 0;
  td.Usage = D3D11_USAGE_STAGING;
  td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  dev->CreateTexture2D(&td, nullptr, &s.staging);
  td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  td.Usage = D3D11_USAGE_DEFAULT;
  td.CPUAccessFlags = 0;
  td.BindFlags = D3D11_BIND_RENDER_TARGET;
  dev->CreateTexture2D(&td, nullptr, &s.color);
  if (s.color) dev->CreateRenderTargetView(s.color, nullptr, &s.rtv);
  return s.vs && s.vsInst && s.psDiscard && s.psColor && s.il && s.vb && s.ib && s.refCb && s.passCb[3] && s.bigCb &&
         s.instSrv && s.offSrv && s.rs && s.rs2 && s.dss && s.dss2 && s.bs && s.bs2 && s.samp && s.dsv[3] &&
         s.staging && s.rtv;
}

void Destroy(Scene& s) {
  Rel(s.vs), Rel(s.vsInst), Rel(s.psDiscard), Rel(s.psColor), Rel(s.il), Rel(s.vb), Rel(s.ib), Rel(s.refCb);
  for (auto*& p : s.passCb) Rel(p);
  Rel(s.instBuf), Rel(s.offBuf), Rel(s.instSrv), Rel(s.offSrv), Rel(s.rs), Rel(s.rs2), Rel(s.dss), Rel(s.dss2);
  Rel(s.bs), Rel(s.bs2), Rel(s.samp);
  for (auto*& p : s.depth) Rel(p);
  for (auto*& p : s.dsv) Rel(p);
  Rel(s.staging), Rel(s.color), Rel(s.rtv), Rel(s.bigCb);
}

// The pass state of cascade `cascade` drawing into target `target`, set
// directly (not through defrec::Apply) on `c`.
void SetPass(Scene& s, ID3D11DeviceContext* c, int target, int cascade) {
  c->ClearState();
  c->OMSetRenderTargets(0, nullptr, s.dsv[target]);
  D3D11_VIEWPORT vp{0, 0, static_cast<float>(kSize), static_cast<float>(kSize), 0, 1};
  c->RSSetViewports(1, &vp);
  c->RSSetState(s.rs);
  c->OMSetDepthStencilState(s.dss, 0);
  c->OMSetBlendState(s.bs, nullptr, 0xffffffff);
  c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  c->IASetInputLayout(s.il);
  const UINT stride = 12, offset = 0;
  c->IASetVertexBuffers(0, 1, &s.vb, &stride, &offset);
  c->IASetIndexBuffer(s.ib, DXGI_FORMAT_R16_UINT, 0);
  c->VSSetShader(s.vs, nullptr, 0);
  c->PSSetShader(nullptr, nullptr, 0);
  ID3D11Buffer* cbs[2] = {s.refCb, s.passCb[cascade]};
  c->VSSetConstantBuffers(0, 2, cbs);
  c->VSSetShaderResources(0, 1, &s.instSrv);
  c->VSSetShaderResources(127, 1, &s.offSrv);
  c->PSSetSamplers(0, 1, &s.samp);
}

// A different state, like DCS's at the point of the pass.
void SetForeign(Scene& s, ID3D11DeviceContext* c, ID3D11DeviceContext1* c1) {
  c->ClearState();
  c->OMSetRenderTargets(1, &s.rtv, s.dsv[3]);
  D3D11_VIEWPORT vp[2] = {{3, 5, 200, 100, 0.1f, 0.9f}, {0, 0, 64, 64, 0, 1}};
  c->RSSetViewports(2, vp);
  D3D11_RECT sc[2] = {{1, 2, 100, 90}, {0, 0, 10, 10}};
  c->RSSetScissorRects(2, sc);
  c->RSSetState(s.rs2);
  c->OMSetDepthStencilState(s.dss2, 0x5a);
  const float bf[4] = {0.25f, 0.5f, 0.75f, 1.0f};
  c->OMSetBlendState(s.bs2, bf, 0x0000ffffu);
  c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
  c->IASetInputLayout(s.il);
  const UINT stride[2] = {12, 16}, offset[2] = {24, 0};
  ID3D11Buffer* vbs[2] = {s.vb, s.vb};
  c->IASetVertexBuffers(2, 2, vbs, stride, offset);
  c->IASetIndexBuffer(s.ib, DXGI_FORMAT_R16_UINT, 12);
  c->VSSetShader(s.vsInst, nullptr, 0);
  c->PSSetShader(s.psColor, nullptr, 0);
  c->VSSetConstantBuffers(3, 1, &s.passCb[2]);
  if (c1) {
    const UINT first = 16, num = 16;  // a 256-byte window at 256 B
    c1->VSSetConstantBuffers1(0, 1, &s.bigCb, &first, &num);
    c1->PSSetConstantBuffers1(5, 1, &s.bigCb, &first, &num);
  }
  c->PSSetConstantBuffers(1, 1, &s.refCb);
  c->VSSetShaderResources(5, 1, &s.offSrv);
  c->PSSetShaderResources(0, 1, &s.instSrv);
  c->PSSetShaderResources(126, 1, &s.offSrv);
  c->PSSetSamplers(3, 1, &s.samp);
  c->VSSetSamplers(0, 1, &s.samp);
}

// Records draws [b, e). ring == null: the stock way (refCb, Map DISCARD per
// draw, already bound at b0). batched: all CBs first (one Close), then the draws.
bool RecordRange(Scene& s, ID3D11DeviceContext* c, CbRing* ring, bool batched, int b, int e) {
  ID3D11VertexShader* curVs = s.vs;
  ID3D11PixelShader* curPs = nullptr;
  std::vector<CbSlice> slices;
  if (ring && batched) {
    slices.resize(e - b);
    for (int i = b; i < e; ++i) {
      slices[i - b] = ring->Upload(s.draws[i].m, 80);
      if (!slices[i - b]) return false;
    }
    ring->Close();
  }
  for (int i = b; i < e; ++i) {
    const DrawData& d = s.draws[i];
    ID3D11VertexShader* v = d.inst ? s.vsInst : s.vs;
    ID3D11PixelShader* p = d.discard ? s.psDiscard : nullptr;
    if (v != curVs) c->VSSetShader(curVs = v, nullptr, 0);
    if (p != curPs) c->PSSetShader(curPs = p, nullptr, 0);
    if (ring) {
      CbSlice sl;
      if (batched) {
        sl = slices[i - b];
      } else {
        sl = ring->Upload(d.m, 80);
        if (!sl) return false;
        ring->Close();
      }
      ring->BindVS(0, sl);
    } else {
      D3D11_MAPPED_SUBRESOURCE m;
      if (FAILED(c->Map(s.refCb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) return false;
      memcpy(m.pData, d.m, 80);
      c->Unmap(s.refCb, 0);
    }
    if (d.inst)
      c->DrawIndexedInstanced(36, kInstances, 0, 0, 0);
    else
      c->DrawIndexed(36, 0, 0);
  }
  return true;
}

struct JobArgs {
  Scene* s;
  int begin, end;
  bool batched;
};
bool RecordJob(Worker& w, void* u) {
  auto* a = static_cast<JobArgs*>(u);
  return RecordRange(*a->s, w.dc, &w.ring, a->batched, a->begin, a->end);
}

std::vector<uint32_t> Read(Scene& s, int target) {
  std::vector<uint32_t> out(static_cast<size_t>(kSize) * kSize, 0xdeadbeef);
  s.imm->CopyResource(s.staging, s.depth[target]);
  D3D11_MAPPED_SUBRESOURCE m;
  if (FAILED(s.imm->Map(s.staging, 0, D3D11_MAP_READ, 0, &m))) return {};
  for (UINT y = 0; y < kSize; ++y)
    memcpy(&out[static_cast<size_t>(y) * kSize], static_cast<uint8_t*>(m.pData) + static_cast<size_t>(y) * m.RowPitch,
           kSize * 4);
  s.imm->Unmap(s.staging, 0);
  return out;
}

size_t Diff(const std::vector<uint32_t>& a, const std::vector<uint32_t>& b) {
  if (a.size() != b.size() || a.empty()) return SIZE_MAX;
  size_t n = 0;
  for (size_t i = 0; i < a.size(); ++i) n += a[i] != b[i];
  return n;
}

size_t Written(const std::vector<uint32_t>& a) {
  size_t n = 0;
  for (uint32_t v : a) n += v != 0;
  return n;
}

void Clear(Scene& s, int target) { s.imm->ClearDepthStencilView(s.dsv[target], D3D11_CLEAR_DEPTH, 0.0f, 0); }

ULONG RefCount(IUnknown* p) {
  p->AddRef();
  return p->Release();
}

// Waits that must complete (they only time out on a hung worker): generous,
// so a loaded machine never turns a slow recording into a failure.
constexpr DWORD kLongWaitMs = 60000;

bool SleepJob(Worker& w, void* u) {
  Sleep(*static_cast<DWORD*>(u));
  w.dc->ClearState();
  return true;
}
// Blocks until the test opens the gate: a job that is still running exactly as
// long as the test needs, whatever the scheduler does (a timed sleep is not).
bool GateJob(Worker& w, void* u) {
  WaitForSingleObject(*static_cast<HANDLE*>(u), INFINITE);
  w.dc->ClearState();
  return true;
}
bool FailJob(Worker&, void*) { return false; }
bool FaultJob(Worker& w, void* u) {
  w.dc->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_POINTLIST);
  return *static_cast<volatile int*>(u) == 1;  // u is null
}

bool CreateDevice(Scene& s, bool* warp) {
  const D3D_FEATURE_LEVEL want[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
  *warp = false;
  if (SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, want, 2, D3D11_SDK_VERSION, &s.dev,
                                  nullptr, &s.imm)))
    return true;
  *warp = true;
  return SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, want, 2, D3D11_SDK_VERSION, &s.dev,
                                     nullptr, &s.imm));
}

// The "unavailable" text names every capability, the flags and the adapter.
void CapsFailureText() {
  ID3D11Device* dev = nullptr;
  if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &dev,
                               nullptr, nullptr)) || !dev) {
    QvSkip("deferred rec caps text: no WARP device\n");
    return;
  }
  Caps c = QueryCaps(dev);
  c.commandLists = false;  // as a device without driver command lists reports it
  const char* t = DescribeCapsFailure(dev, c);
  printf("     | %s\n", t);
  Check(strstr(t, "DriverCommandLists=0") && strstr(t, "ConstantBufferOffsetting=") &&
            strstr(t, "ID3D11DeviceContext1=") && strstr(t, "creation flags 0x") && strstr(t, "vendor 0x1414") &&
            strstr(t, "LUID ") && strstr(t, "native command lists"),
        "deferred rec: the unavailable line lists each capability, the creation flags and DCS's adapter");
  dev->Release();
}

void Run() {
  CapsFailureText();
  Scene s;
  bool warp = false;
  if (!CreateDevice(s, &warp)) {
    QvSkip("deferred rec: no D3D11 device\n");
    return;
  }
  constexpr int kDraws = 6000;
  if (!Create(s, kDraws)) {
    Check(false, "deferred rec: test device objects");
    Destroy(s);
    Rel(s.imm), Rel(s.dev);
    return;
  }
  ID3D11DeviceContext1* imm1 = nullptr;
  s.imm->QueryInterface(__uuidof(ID3D11DeviceContext1), reinterpret_cast<void**>(&imm1));
  const Caps caps = QueryCaps(s.dev);
  printf("     %s device: DriverCommandLists=%d ConcurrentCreates=%d ConstantBufferOffsetting=%d "
         "MapNoOverwriteOnDynamicConstantBuffer=%d\n",
         warp ? "WARP" : "hardware", caps.commandLists, caps.concurrentCreates, caps.cbOffsetting, caps.noOverwriteCb);
  const ULONG devRefs0 = RefCount(s.dev);

  // Reference: the immediate context, quarters into 4 targets and the whole cascade into one.
  std::vector<uint32_t> refQ[4], refFull;
  for (int k = 0; k < 4; ++k) {
    SetPass(s, s.imm, k, k);
    Clear(s, k);
    RecordRange(s, s.imm, nullptr, false, k * kDraws / 4, (k + 1) * kDraws / 4);
  }
  for (int k = 0; k < 4; ++k) refQ[k] = Read(s, k);
  SetPass(s, s.imm, 0, 0);
  Clear(s, 0);
  RecordRange(s, s.imm, nullptr, false, 0, kDraws);
  refFull = Read(s, 0);
  // Determinism of the reference itself (a second immediate run).
  SetPass(s, s.imm, 0, 0);
  Clear(s, 0);
  RecordRange(s, s.imm, nullptr, false, 0, kDraws);
  const size_t selfDiff = Diff(Read(s, 0), refFull);
  printf("     reference: %zu of %u texels written (full cascade), quarter 0 %zu\n", Written(refFull), kSize * kSize,
         Written(refQ[0]));
  Check(selfDiff == 0 && Written(refFull) > kSize * kSize / 20 && Written(refQ[3]) > 0,
        "deferred rec: immediate reference cascade is deterministic and draws something");

  // Get*1 for a buffer bound without offsets (Capture's 'plain' rule).
  if (imm1) {
    s.imm->ClearState();
    s.imm->VSSetConstantBuffers(0, 1, &s.bigCb);
    ID3D11Buffer* got = nullptr;
    UINT first = 77, num = 77;
    imm1->VSGetConstantBuffers1(0, 1, &got, &first, &num);
    printf("     VSGetConstantBuffers1 after a plain bind of a 4 KB buffer: first=%u num=%u\n", first, num);
    Rel(got);
    PassState p{};
    Capture(s.imm, &p);
    Check(p.vsStage.cbRanged == 0 && p.vsStage.cbCount == 1, "deferred rec: plain CB binding captured as not ranged");
    Release(p);
  }

  // Apply reproduces a captured state on a deferred context (Get* on the deferred context).
  {
    PassState a{}, b{};
    SetForeign(s, s.imm, imm1);
    Capture(s.imm, &a);
    ID3D11DeviceContext* dc = nullptr;
    s.dev->CreateDeferredContext(0, &dc);
    ID3D11DeviceContext1* dc1 = nullptr;
    if (dc) dc->QueryInterface(__uuidof(ID3D11DeviceContext1), reinterpret_cast<void**>(&dc1));
    if (dc) {
      Apply(dc, dc1, a);
      Capture(dc, &b);
      ID3D11CommandList* cl = nullptr;
      dc->FinishCommandList(FALSE, &cl);
      Rel(cl);
    }
    Check(dc && a.valid && a.unsupported == 0 && (a.vsStage.cbRanged != 0 || !imm1) && Equal(a, b),
          "deferred rec: Apply on a deferred context reproduces the captured state (Get* identical)");
    if (!Equal(a, b)) {
      const auto* pa = reinterpret_cast<const uint8_t*>(&a);
      const auto* pb = reinterpret_cast<const uint8_t*>(&b);
      for (size_t i = 0; i < sizeof(PassState); ++i)
        if (pa[i] != pb[i]) {
          printf("     first difference at PassState+0x%zx\n", i);
          break;
        }
    }
    Release(a);
    Release(b);
    Rel(dc1);
    Rel(dc);
  }

  // The pool, every CB strategy.
  struct Variant {
    CbMode mode;
    bool batched;
    const char* name;
  };
  const Variant variants[] = {{CbMode::kDiscard, false, "discard per draw"},
                              {CbMode::kOffsets, false, "offsets per draw (NO_OVERWRITE)"},
                              {CbMode::kOffsets, true, "offsets batched"}};
  for (const Variant& var : variants) {
    if (var.mode == CbMode::kOffsets && !caps.cbOffsetting) {
      printf("     (CB offsets not supported: '%s' skipped)\n", var.name);
      continue;
    }
    Pool pool;
    PoolConfig pc;
    pc.cb.mode = var.mode;
    pc.name = "defrec test";
    if (!pool.Start(s.dev, pc)) {
      Check(false, "deferred rec: pool start");
      continue;
    }
    char what[256];
    // (a) 4 cascades on 4 workers into 4 targets.
    PassState ps[4] = {};
    for (int k = 0; k < 4; ++k) {
      SetPass(s, s.imm, k, k);
      Capture(s.imm, &ps[k]);
    }
    JobArgs args[4];
    bool sub = true;
    for (int k = 0; k < 4; ++k) {
      args[k] = {&s, k * kDraws / 4, (k + 1) * kDraws / 4, var.batched};
      sub = pool.Submit(k, &RecordJob, &args[k], &ps[k]) && sub;
    }
    const bool waited = pool.Wait(kLongWaitMs);
    SetForeign(s, s.imm, imm1);
    for (int k = 0; k < 4; ++k) Clear(s, k);
    PassState before{}, after{};
    Capture(s.imm, &before);
    const int executed = pool.ExecuteAll(s.imm, TRUE);
    Capture(s.imm, &after);
    const bool restored = Equal(before, after);
    Release(before);
    Release(after);
    size_t bad = 0;
    for (int k = 0; k < 4; ++k) bad += Diff(Read(s, k), refQ[k]);
    snprintf(what, sizeof(what),
             "deferred rec [%s, %s]: 4 cascades recorded on 4 workers match the immediate depth bit for bit",
             ModeName(pool.At(0).ring.Mode()), var.name);
    Check(sub && waited && executed == 4 && bad == 0, what);
    if (bad) printf("     %zu texels differ\n", bad);
    snprintf(what, sizeof(what), "deferred rec [%s]: ExecuteCommandList(TRUE) x4 leaves the immediate state unchanged",
             var.name);
    Check(restored, what);
    for (auto& p : ps) Release(p);

    // (b) The whole cascade as one list on one worker.
    PassState full{};
    SetPass(s, s.imm, 0, 0);
    Capture(s.imm, &full);
    JobArgs all{&s, 0, kDraws, var.batched};
    sub = pool.Submit(1, &RecordJob, &all, &full);
    bool ok = sub && pool.Wait(kLongWaitMs);
    Clear(s, 0);
    ok = ok && pool.ExecuteAll(s.imm) == 1;
    bad = Diff(Read(s, 0), refFull);
    snprintf(what, sizeof(what), "deferred rec [%s]: the whole cascade as one deferred list matches bit for bit",
             var.name);
    Check(ok && bad == 0, what);
    if (bad) printf("     %zu texels differ\n", bad);

    // (c) 4 quarter lists into one target, executed in order.
    for (int k = 0; k < 4; ++k) {
      args[k] = {&s, k * kDraws / 4, (k + 1) * kDraws / 4, var.batched};
      sub = pool.Submit(k, &RecordJob, &args[k], &full) && sub;
    }
    ok = sub && pool.Wait(kLongWaitMs);
    Clear(s, 0);
    ok = ok && pool.ExecuteAll(s.imm) == 4;
    bad = Diff(Read(s, 0), refFull);
    snprintf(what, sizeof(what),
             "deferred rec [%s]: one cascade split into 4 lists (4 workers, executed in order) matches bit for bit",
             var.name);
    Check(ok && bad == 0, what);
    if (bad) printf("     %zu texels differ\n", bad);
    // Negative control for the restore check: with FALSE the state is reset.
    if (var.mode == CbMode::kDiscard) {
      JobArgs one{&s, 0, 10, false};
      sub = pool.Submit(0, &RecordJob, &one, &full) && pool.Wait(kLongWaitMs);
      SetForeign(s, s.imm, imm1);
      PassState b0{}, a0{};
      Capture(s.imm, &b0);
      ID3D11CommandList* l = pool.TakeList(0);
      if (l) s.imm->ExecuteCommandList(l, FALSE);
      Rel(l);
      Capture(s.imm, &a0);
      Check(sub && !Equal(b0, a0) && a0.dsv == nullptr && a0.vs == nullptr,
            "deferred rec: control: ExecuteCommandList(FALSE) leaves the immediate context cleared (the check sees it)");
      Release(b0);
      Release(a0);
    }
    Release(full);
    const CbRing& r = pool.At(0).ring;
    printf("     ring worker 0: %llu maps (%llu DISCARD), %llu buffers created, %llu overflows\n",
           static_cast<unsigned long long>(r.maps), static_cast<unsigned long long>(r.discards),
           static_cast<unsigned long long>(r.created), static_cast<unsigned long long>(r.overflows));
    Check(pool.Stop(), "deferred rec: pool stops (workers joined)");
  }

  // Failure paths.
  {
    TestSpin spin("deferred rec failure paths");
    Pool pool;
    PoolConfig pc;
    pc.workers = 2;
    pc.name = "defrec test";
    bool ok = pool.Start(s.dev, pc);
    // A job that gives up: failure reported, pool stays on.
    ok = ok && pool.Submit(0, &FailJob, nullptr, nullptr) && !pool.Wait(kLongWaitMs) && pool.Status(0) == kIdle &&
         !pool.Disabled() && pool.At(0).failed == 1;
    Check(ok, "deferred rec: a job returning false reports failure, the pool stays on");
    // A job past the wait timeout: refused until it finished, its late list dropped.
    // The job blocks on a gate the test opens after the refusal (no timed sleeps).
    HANDLE gate = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    const bool sub = pool.Submit(1, &GateJob, &gate, nullptr);
    const bool waitedLate = pool.Wait(20);
    const bool refused = !pool.Submit(1, &GateJob, &gate, nullptr);
    SetEvent(gate);
    const ULONGLONG until = GetTickCount64() + 60000;
    while (pool.Busy(1) && GetTickCount64() < until) Sleep(1);
    DWORD ms0 = 0;
    const bool resubmit = !pool.Busy(1) && pool.Status(1) == kOk && pool.Submit(1, &SleepJob, &ms0, nullptr);
    const bool waited = pool.Wait(kLongWaitMs);
    CloseHandle(gate);
    ID3D11CommandList* l = pool.TakeList(1);
    Check(sub && !waitedLate && pool.timeouts() == 1 && refused && pool.refused() == 1 && resubmit && waited && l,
          "deferred rec: a job past the timeout is not waited for, the worker is refused until it finished");
    Rel(l);
    // R75: a worker sets its done event after publishing the job's final state, so the previous job's SetEvent can
    // land after the next Submit reset the event (gb_rec_test's view-change re-record: Wait returned while the
    // re-record still ran, about 1 run in 150). Replayed deterministically: that late signal set by hand while a
    // gated job runs. Wait must still wait for the job (here until its own timeout), then the job finishes normally.
    {
      HANDLE g2 = CreateEventW(nullptr, TRUE, FALSE, nullptr);
      const uint64_t to0 = pool.timeouts();
      const bool sub2 = pool.Submit(1, &GateJob, &g2, nullptr);
      SetEvent(pool.At(1).done);  // the previous job's late SetEvent
      const int64_t w0 = Qpc();
      const bool waited2 = pool.Wait(200);
      const double waitedMs = (Qpc() - w0) * QpcToUs() / 1000.0;
      const bool busy2 = pool.Busy(1);
      SetEvent(g2);
      const ULONGLONG until2 = GetTickCount64() + 60000;
      while (pool.Busy(1) && GetTickCount64() < until2) Sleep(1);
      ID3D11CommandList* l2 = pool.TakeList(1);
      CloseHandle(g2);
      printf("     stale done signal: Wait returned after %.1f ms (timeout 200)\n", waitedMs);
      Check(sub2 && !waited2 && busy2 && waitedMs >= 150.0 && pool.timeouts() == to0 + 1 && l2,
            "deferred rec: a stale done signal (the previous job's late SetEvent) does not end Wait while the job runs");
      Rel(l2);
    }
    // A faulting job: caught, reported, pool latched off.
    const bool fsub = pool.Submit(0, &FaultJob, nullptr, nullptr);
    const bool fwait = pool.Wait(kLongWaitMs);
    printf("     fault: worker 0 code 0x%08lx\n", static_cast<unsigned long>(pool.At(0).faultCode));
    Check(fsub && !fwait && pool.At(0).faults == 1 && pool.At(0).faultCode == EXCEPTION_ACCESS_VIOLATION &&
              pool.Disabled() && !pool.Submit(1, &FailJob, nullptr, nullptr),
          "deferred rec: a faulting job is caught (SEH), reported, and latches the pool off");
    Check(pool.Stop(), "deferred rec: pool with a faulted worker stops cleanly");
  }

  // Start/stop cycles with jobs in flight at Stop: every reference released.
  {
    bool ok = true;
    for (int i = 0; i < 20 && ok; ++i) {
      Pool pool;
      PoolConfig pc;
      pc.name = "defrec test";
      ok = pool.Start(s.dev, pc);
      PassState p{};
      SetPass(s, s.imm, 0, 0);
      Capture(s.imm, &p);
      JobArgs a{&s, 0, 500, false};
      for (int k = 0; k < 4 && ok; ++k) ok = pool.Submit(k, &RecordJob, &a, &p);
      if (i & 1) pool.Wait(5000);  // odd: lists left untaken at Stop; even: Stop while recording
      ok = pool.Stop() && ok;
      Release(p);
    }
    s.imm->ClearState();
    Check(ok && RefCount(s.dev) == devRefs0,
          "deferred rec: 20 start/stop cycles (Stop during recording too) release every device reference");
    if (RefCount(s.dev) != devRefs0)
      printf("     device refs %lu, expected %lu\n", RefCount(s.dev), devRefs0);
  }

  s.imm->ClearState();
  Rel(imm1);
  Destroy(s);
  Rel(s.imm);
  Rel(s.dev);
}

// ---------------------------------------------------------------------------
// QV_DEFREC_BENCH: render-thread timing
// ---------------------------------------------------------------------------
double Ms(int64_t a, int64_t b) { return (b - a) * QpcToUs() / 1000.0; }

double Median(std::vector<double> v) {
  if (v.empty()) return 0;
  std::sort(v.begin(), v.end());
  return v[v.size() / 2];
}

void Drain(Scene& s, ID3D11Query* q) {
  s.imm->End(q);
  s.imm->Flush();
  while (s.imm->GetData(q, nullptr, 0, 0) == S_FALSE) Sleep(0);
}

void Bench(int rounds) {
  Scene s;
  bool warp = false;
  constexpr int kPer = 5000, kDraws = 4 * kPer;
  if (!CreateDevice(s, &warp) || !Create(s, kDraws)) {
    printf("bench: no device\n");
    return;
  }
  const Caps caps = QueryCaps(s.dev);
  printf("bench on %s device; DriverCommandLists=%d offsets=%d noOverwriteCb=%d; %d rounds per run, median per run\n",
         warp ? "WARP" : "hardware", caps.commandLists, caps.cbOffsetting, caps.noOverwriteCb, rounds);
  ID3D11Query* q = nullptr;
  D3D11_QUERY_DESC qd{D3D11_QUERY_EVENT, 0};
  s.dev->CreateQuery(&qd, &q);

  struct Variant {
    CbMode mode;
    bool batched;
    const char* name;
  };
  const Variant variants[] = {{CbMode::kDiscard, false, "discard/draw"},
                              {CbMode::kOffsets, false, "offsets/draw"},
                              {CbMode::kOffsets, true, "offsets batched"}};
  constexpr int kV = 3, kRuns = 3;
  double med[16][kRuns] = {};

  Pool pools[kV];
  for (int v = 0; v < kV; ++v) {
    PoolConfig pc;
    pc.cb.mode = variants[v].mode;
    pc.name = "defrec bench";
    pools[v].Start(s.dev, pc);
  }
  ID3D11DeviceContext* dc = nullptr;
  s.dev->CreateDeferredContext(0, &dc);
  CbRing rings[kV];
  for (int v = 0; v < kV; ++v) {
    CbConfig cc;
    cc.mode = variants[v].mode;
    rings[v].Init(s.dev, dc, caps, cc);
  }

  for (int run = 0; run < kRuns; ++run) {
    std::vector<double> imm, immF, rec1[kV], fin1[kV], tot[kV], busy[kV], wait[kV], exec[kV], cap[kV];
    for (int r = 0; r < rounds + 2; ++r) {
      const bool keep = r >= 2;
      // Immediate: 4 cascades x 5,000 draws, stock CB path.
      Drain(s, q);
      int64_t t0 = Qpc();
      for (int k = 0; k < 4; ++k) {
        SetPass(s, s.imm, k, k);
        RecordRange(s, s.imm, nullptr, false, k * kPer, (k + 1) * kPer);
      }
      int64_t t1 = Qpc();
      s.imm->Flush();
      int64_t t2 = Qpc();
      if (keep) {
        imm.push_back(Ms(t0, t1));
        immF.push_back(Ms(t0, t2));
      }
      for (int v = 0; v < kV; ++v) {
        if (variants[v].mode == CbMode::kOffsets && !caps.cbOffsetting) continue;
        // One thread, one deferred context, 5,000 draws: CB strategy cost.
        Drain(s, q);
        PassState p{};
        SetPass(s, s.imm, 0, 0);
        Capture(s.imm, &p);
        rings[v].Reset();
        t0 = Qpc();
        Apply(dc, nullptr, p);
        RecordRange(s, dc, &rings[v], variants[v].batched, 0, kPer);
        rings[v].Close();
        t1 = Qpc();
        ID3D11CommandList* cl = nullptr;
        dc->FinishCommandList(FALSE, &cl);
        t2 = Qpc();
        Release(p);
        s.imm->ExecuteCommandList(cl, TRUE);
        Rel(cl);
        if (keep) {
          rec1[v].push_back(Ms(t0, t1));
          fin1[v].push_back(Ms(t1, t2));
        }
        // End to end on the render thread: capture 4 pass states, submit 4
        // jobs, wait, 4 x ExecuteCommandList(TRUE).
        Drain(s, q);
        Pool& pool = pools[v];
        PassState ps[4] = {};
        JobArgs args[4];
        t0 = Qpc();
        for (int k = 0; k < 4; ++k) {
          SetPass(s, s.imm, k, k);
          Capture(s.imm, &ps[k]);
        }
        const int64_t tc = Qpc();
        for (int k = 0; k < 4; ++k) {
          args[k] = {&s, k * kPer, (k + 1) * kPer, variants[v].batched};
          pool.Submit(k, &RecordJob, &args[k], &ps[k]);
        }
        const int64_t ts = Qpc();
        const bool ok = pool.Wait(1000);
        const int64_t tw = Qpc();
        const int n = pool.ExecuteAll(s.imm, TRUE);
        const int64_t te = Qpc();
        for (auto& x : ps) Release(x);
        const int64_t tr = Qpc();
        if (!ok || n != 4) printf("bench: pool round failed (%d lists)\n", n);
        if (keep) {
          tot[v].push_back(Ms(t0, tr));
          wait[v].push_back(Ms(ts, tw));
          busy[v].push_back(Ms(t0, tr) - Ms(ts, tw));
          exec[v].push_back(Ms(tw, te));
          cap[v].push_back(Ms(t0, tc));
        }
      }
    }
    Drain(s, q);
    med[0][run] = Median(imm);
    med[1][run] = Median(immF);
    printf("run %d: immediate 4x%d draws: issue %.3f ms, + Flush %.3f ms\n", run + 1, kPer, med[0][run], med[1][run]);
    for (int v = 0; v < kV; ++v) {
      if (rec1[v].empty()) continue;
      med[2 + v * 4][run] = Median(rec1[v]) + Median(fin1[v]);
      med[3 + v * 4][run] = Median(tot[v]);
      med[4 + v * 4][run] = Median(busy[v]);
      med[5 + v * 4][run] = Median(wait[v]);
      printf("run %d: [%-15s] 1 thread record %.3f + Finish %.3f ms | pool end-to-end %.3f ms (capture %.3f, wait "
             "%.3f, 4x Execute %.3f; render thread busy w/o wait %.3f)\n",
             run + 1, variants[v].name, Median(rec1[v]), Median(fin1[v]), Median(tot[v]), Median(cap[v]),
             Median(wait[v]), Median(exec[v]), Median(busy[v]));
    }
  }
  auto minMed = [&](int row, double* mn, double* md) {
    std::vector<double> v(med[row], med[row] + kRuns);
    std::sort(v.begin(), v.end());
    *mn = v[0];
    *md = v[kRuns / 2];
  };
  double a, b;
  minMed(0, &a, &b);
  printf("RESULT_DEFREC imm_issue min=%.3f med=%.3f\n", a, b);
  minMed(1, &a, &b);
  printf("RESULT_DEFREC imm_issue_flush min=%.3f med=%.3f\n", a, b);
  for (int v = 0; v < kV; ++v) {
    const char* rows[4] = {"rec1+fin", "pool_total", "pool_busy_no_wait", "pool_wait"};
    for (int k = 0; k < 4; ++k) {
      minMed(2 + v * 4 + k, &a, &b);
      printf("RESULT_DEFREC [%s] %s min=%.3f med=%.3f\n", variants[v].name, rows[k], a, b);
    }
  }
  for (auto& r : rings) r.Release();
  Rel(dc);
  for (auto& p : pools) p.Stop();
  Rel(q);
  s.imm->ClearState();
  Destroy(s);
  Rel(s.imm);
  Rel(s.dev);
}

}  // namespace drtest
