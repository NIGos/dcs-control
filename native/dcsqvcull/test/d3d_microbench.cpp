// Offline D3D11 microbenchmarks for R15 F5 and A1 (no DCS involved).
//
// F5: ns per redundant immediate-context call (identical arguments to what is
//     already bound) for the state calls the d3dstate.h meter found redundant,
//     against an empty forwarding thunk called through a function pointer
//     (what a split-path filter pays when it skips). Real draws with a
//     Map(DISCARD) constant-buffer update are interleaved so the driver is not
//     in a trivial state. Also the per-slot cost of redundant
//     PSSetShaderResources (R15 F2: cost of an identical tail slot).
// A1: CheckFeatureSupport(THREADING); 5,000 simple draws (VS+PS, one VB/IB,
//     a CB per draw via Map DISCARD, depth-only target) recorded on a deferred
//     context, FinishCommandList, ExecuteCommandList(TRUE) on the immediate
//     context, against the same draws issued on the immediate context; then 4
//     threads recording 4 x 5,000 in parallel.
//
// Hardware device on the default adapter, no debug layer. Every timing is the
// median of several rounds; the GPU is drained (event query) between rounds,
// outside the timed region.
// Usage: d3d_microbench.exe [rounds]   (default 15)
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <thread>
#include <intrin.h>
#include <vector>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")

namespace {

double g_qpcToNs = 0;

inline int64_t Now() {
  LARGE_INTEGER t;
  QueryPerformanceCounter(&t);
  return t.QuadPart;
}
inline double Ns(int64_t a, int64_t b) { return (b - a) * g_qpcToNs; }

double Median(std::vector<double> v) {
  if (v.empty()) return 0;
  std::sort(v.begin(), v.end());
  return v[v.size() / 2];
}

template <class T>
void Release(T*& p) {
  if (p) p->Release();
  p = nullptr;
}

#define CHECK(x)                                                                         \
  do {                                                                                   \
    HRESULT hr_ = (x);                                                                   \
    if (FAILED(hr_)) {                                                                   \
      printf("FAILED 0x%08lx: %s (line %d)\n", static_cast<unsigned long>(hr_), #x, __LINE__); \
      exit(1);                                                                           \
    }                                                                                    \
  } while (0)

const char kShader[] = R"(
cbuffer PerDraw : register(b0) { float4x4 m; float4 tint; };
struct VI { float3 p : POSITION; };
struct VO { float4 p : SV_Position; float4 c : COLOR; };
VO vs(VI i) { VO o; o.p = mul(float4(i.p, 1), m); o.c = tint; return o; }
float4 ps(VO i) : SV_Target { return i.c; }
)";

struct Gpu {
  ID3D11Device* dev = nullptr;
  ID3D11DeviceContext* ctx = nullptr;
  ID3D11VertexShader* vs = nullptr;
  ID3D11PixelShader* ps = nullptr;
  ID3D11InputLayout* il = nullptr;
  ID3D11Buffer* vb = nullptr;
  ID3D11Buffer* ib = nullptr;
  ID3D11Buffer* cb = nullptr;  // dynamic, Map DISCARD per draw (immediate context)
  ID3D11Texture2D* depth = nullptr;
  ID3D11DepthStencilView* dsv = nullptr;
  ID3D11BlendState* blend = nullptr;
  ID3D11DepthStencilState* ds = nullptr;
  ID3D11RasterizerState* rs = nullptr;
  ID3D11SamplerState* samp = nullptr;
  ID3D11Texture2D* tex[16] = {};
  ID3D11ShaderResourceView* srv[16] = {};
  ID3D11Query* evt = nullptr;
};

ID3DBlob* Compile(const char* entry, const char* target) {
  ID3DBlob* code = nullptr;
  ID3DBlob* err = nullptr;
  HRESULT hr = D3DCompile(kShader, sizeof(kShader) - 1, "bench", nullptr, nullptr, entry, target,
                          D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &err);
  if (FAILED(hr)) {
    printf("shader compile failed: %s\n", err ? static_cast<const char*>(err->GetBufferPointer()) : "?");
    exit(1);
  }
  Release(err);
  return code;
}

ID3D11Buffer* MakeCb(ID3D11Device* dev) {
  D3D11_BUFFER_DESC d{};
  d.ByteWidth = 80;
  d.Usage = D3D11_USAGE_DYNAMIC;
  d.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
  d.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
  ID3D11Buffer* b = nullptr;
  CHECK(dev->CreateBuffer(&d, nullptr, &b));
  return b;
}

void Init(Gpu& g) {
  UINT flags = 0;
  D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
  const D3D_FEATURE_LEVEL want[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
  CHECK(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, want, 2, D3D11_SDK_VERSION, &g.dev, &fl,
                          &g.ctx));
  IDXGIDevice* xd = nullptr;
  IDXGIAdapter* ad = nullptr;
  if (SUCCEEDED(g.dev->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&xd))) &&
      SUCCEEDED(xd->GetAdapter(&ad))) {
    DXGI_ADAPTER_DESC desc{};
    ad->GetDesc(&desc);
    LARGE_INTEGER umd{};
    ad->CheckInterfaceSupport(__uuidof(IDXGIDevice), &umd);
    printf("adapter: %ls, UMD %u.%u.%u.%u, feature level %x\n", desc.Description, HIWORD(umd.HighPart),
           LOWORD(umd.HighPart), HIWORD(umd.LowPart), LOWORD(umd.LowPart), static_cast<unsigned>(fl));
  }
  Release(ad);
  Release(xd);

  ID3DBlob* vsb = Compile("vs", "vs_5_0");
  ID3DBlob* psb = Compile("ps", "ps_5_0");
  CHECK(g.dev->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &g.vs));
  CHECK(g.dev->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &g.ps));
  const D3D11_INPUT_ELEMENT_DESC ied[] = {{"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0}};
  CHECK(g.dev->CreateInputLayout(ied, 1, vsb->GetBufferPointer(), vsb->GetBufferSize(), &g.il));
  Release(vsb);
  Release(psb);

  // A small cube: 8 vertices, 36 indices.
  const float v[8][3] = {{-1, -1, -1}, {1, -1, -1}, {1, 1, -1}, {-1, 1, -1},
                         {-1, -1, 1},  {1, -1, 1},  {1, 1, 1},  {-1, 1, 1}};
  const uint16_t idx[36] = {0, 1, 2, 0, 2, 3, 4, 6, 5, 4, 7, 6, 0, 4, 5, 0, 5, 1,
                            3, 2, 6, 3, 6, 7, 1, 5, 6, 1, 6, 2, 0, 3, 7, 0, 7, 4};
  D3D11_BUFFER_DESC bd{};
  D3D11_SUBRESOURCE_DATA sd{};
  bd.ByteWidth = sizeof(v);
  bd.Usage = D3D11_USAGE_IMMUTABLE;
  bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
  sd.pSysMem = v;
  CHECK(g.dev->CreateBuffer(&bd, &sd, &g.vb));
  bd.ByteWidth = sizeof(idx);
  bd.BindFlags = D3D11_BIND_INDEX_BUFFER;
  sd.pSysMem = idx;
  CHECK(g.dev->CreateBuffer(&bd, &sd, &g.ib));
  g.cb = MakeCb(g.dev);

  D3D11_TEXTURE2D_DESC td{};
  td.Width = td.Height = 512;
  td.MipLevels = td.ArraySize = 1;
  td.Format = DXGI_FORMAT_D32_FLOAT;
  td.SampleDesc.Count = 1;
  td.Usage = D3D11_USAGE_DEFAULT;
  td.BindFlags = D3D11_BIND_DEPTH_STENCIL;
  CHECK(g.dev->CreateTexture2D(&td, nullptr, &g.depth));
  CHECK(g.dev->CreateDepthStencilView(g.depth, nullptr, &g.dsv));

  D3D11_BLEND_DESC blend{};
  blend.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
  CHECK(g.dev->CreateBlendState(&blend, &g.blend));
  D3D11_DEPTH_STENCIL_DESC dsd{};
  dsd.DepthEnable = TRUE;
  dsd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
  dsd.DepthFunc = D3D11_COMPARISON_GREATER;  // reversed depth, like DCS's shadow passes
  CHECK(g.dev->CreateDepthStencilState(&dsd, &g.ds));
  D3D11_RASTERIZER_DESC rd{};
  rd.FillMode = D3D11_FILL_SOLID;
  rd.CullMode = D3D11_CULL_BACK;
  rd.DepthClipEnable = TRUE;
  CHECK(g.dev->CreateRasterizerState(&rd, &g.rs));
  D3D11_SAMPLER_DESC smp{};
  smp.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
  smp.AddressU = smp.AddressV = smp.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
  smp.MaxLOD = D3D11_FLOAT32_MAX;
  CHECK(g.dev->CreateSamplerState(&smp, &g.samp));

  td.Width = td.Height = 64;
  td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  for (int i = 0; i < 16; ++i) {
    CHECK(g.dev->CreateTexture2D(&td, nullptr, &g.tex[i]));
    CHECK(g.dev->CreateShaderResourceView(g.tex[i], nullptr, &g.srv[i]));
  }
  D3D11_QUERY_DESC qd{D3D11_QUERY_EVENT, 0};
  CHECK(g.dev->CreateQuery(&qd, &g.evt));
}

// Binds the whole draw state (immediate or deferred context).
void BindDrawState(const Gpu& g, ID3D11DeviceContext* c, ID3D11Buffer* cb) {
  const UINT stride = 12, offset = 0;
  c->IASetInputLayout(g.il);
  c->IASetVertexBuffers(0, 1, &g.vb, &stride, &offset);
  c->IASetIndexBuffer(g.ib, DXGI_FORMAT_R16_UINT, 0);
  c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  c->VSSetShader(g.vs, nullptr, 0);
  c->PSSetShader(g.ps, nullptr, 0);
  c->VSSetConstantBuffers(0, 1, &cb);
  c->PSSetConstantBuffers(0, 1, &cb);
  c->PSSetSamplers(0, 1, &g.samp);
  c->PSSetShaderResources(0, 16, g.srv);
  c->OMSetBlendState(g.blend, nullptr, 0xffffffff);
  c->OMSetDepthStencilState(g.ds, 0);
  c->RSSetState(g.rs);
  c->OMSetRenderTargets(0, nullptr, g.dsv);
  D3D11_VIEWPORT vp{0, 0, 512, 512, 0, 1};
  c->RSSetViewports(1, &vp);
}

// One draw with fresh CB contents (Map DISCARD).
inline void Draw(ID3D11DeviceContext* c, ID3D11Buffer* cb, int i) {
  D3D11_MAPPED_SUBRESOURCE m;
  if (SUCCEEDED(c->Map(cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
    float* f = static_cast<float*>(m.pData);
    const float s = 0.02f, x = ((i * 37) % 97) / 48.5f - 1.0f, y = ((i * 53) % 89) / 44.5f - 1.0f;
    const float mat[20] = {s, 0, 0, 0, 0, s, 0, 0, 0, 0, s, 0, x, y, 0.5f, 1, 1, 0, 0, 1};
    memcpy(f, mat, sizeof(mat));
    c->Unmap(cb, 0);
  }
  c->DrawIndexed(36, 0, 0);
}

void Drain(Gpu& g) {
  g.ctx->End(g.evt);
  g.ctx->Flush();
  while (g.ctx->GetData(g.evt, nullptr, 0, 0) == S_FALSE) Sleep(0);
}

// ---------------------------------------------------------------- F5 -------

// The skip path of a split-path filter: called through a pointer, does nothing.
using ThunkFn = void (*)(ID3D11DeviceContext*, const void*);
__declspec(noinline) void EmptyThunk(ID3D11DeviceContext*, const void*) {}
ThunkFn volatile g_thunk = &EmptyThunk;

enum Call { kBlend, kDepth, kRaster, kPSCB, kVSCB, kPSSamp, kTopo, kVSShader, kPSShader, kLayout, kThunk, kNone, kCalls };
const char* const kCallNames[kCalls] = {"OMSetBlendState",       "OMSetDepthStencilState", "RSSetState",
                                        "PSSetConstantBuffers(1)", "VSSetConstantBuffers(1)", "PSSetSamplers(1)",
                                        "IASetPrimitiveTopology", "VSSetShader",            "PSSetShader",
                                        "IASetInputLayout",       "empty thunk",            "(draws only)"};

inline void Redundant(const Gpu& g, ID3D11DeviceContext* c, int call) {
  switch (call) {
    case kBlend: c->OMSetBlendState(g.blend, nullptr, 0xffffffff); break;
    case kDepth: c->OMSetDepthStencilState(g.ds, 0); break;
    case kRaster: c->RSSetState(g.rs); break;
    case kPSCB: c->PSSetConstantBuffers(0, 1, &g.cb); break;
    case kVSCB: c->VSSetConstantBuffers(0, 1, &g.cb); break;
    case kPSSamp: c->PSSetSamplers(0, 1, &g.samp); break;
    case kTopo: c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST); break;
    case kVSShader: c->VSSetShader(g.vs, nullptr, 0); break;
    case kPSShader: c->PSSetShader(g.ps, nullptr, 0); break;
    case kLayout: c->IASetInputLayout(g.il); break;
    case kThunk: g_thunk(c, &g); break;
    default: break;
  }
}

// ns per call: `draws` draws, each followed by `perDraw` identical calls.
// Returns total ns of the loop.
double RunF5(Gpu& g, int call, int draws, int perDraw) {
  ID3D11DeviceContext* c = g.ctx;
  BindDrawState(g, c, g.cb);
  Drain(g);
  const int64_t t0 = Now();
  for (int i = 0; i < draws; ++i) {
    Draw(c, g.cb, i);
    for (int k = 0; k < perDraw; ++k) Redundant(g, c, call);
  }
  const int64_t t1 = Now();
  return Ns(t0, t1);
}

// Redundant PSSetShaderResources with n identical slots, once per draw.
double RunSrv(Gpu& g, int n, int draws, int perDraw) {
  ID3D11DeviceContext* c = g.ctx;
  BindDrawState(g, c, g.cb);
  Drain(g);
  const int64_t t0 = Now();
  for (int i = 0; i < draws; ++i) {
    Draw(c, g.cb, i);
    for (int k = 0; k < perDraw; ++k) c->PSSetShaderResources(0, n, g.srv);
  }
  const int64_t t1 = Now();
  return Ns(t0, t1);
}

void BenchF5(Gpu& g, int rounds) {
  const int kDraws = 2000, kPer = 32;
  printf("\n== F5: redundant immediate-context calls (%d draws per round, %d identical calls after each draw, "
         "median of %d rounds) ==\n",
         kDraws, kPer, rounds);
  // Warm up.
  for (int w = 0; w < 3; ++w) RunF5(g, kNone, kDraws, 0);
  double base = 0;
  {
    std::vector<double> v;
    for (int r = 0; r < rounds; ++r) v.push_back(RunF5(g, kNone, kDraws, 0));
    base = Median(v);
    printf("  draws only (Map DISCARD + DrawIndexed): %.1f ns per draw\n", base / kDraws);
  }
  double ns[kCalls] = {};
  for (int call = 0; call < kNone; ++call) {
    std::vector<double> v;
    for (int r = 0; r < rounds; ++r) {
      // Interleave a baseline run so drift (DCS load on the GPU) cancels.
      const double b = RunF5(g, kNone, kDraws, 0);
      const double t = RunF5(g, call, kDraws, kPer);
      v.push_back((t - b) / (static_cast<double>(kDraws) * kPer));
    }
    ns[call] = Median(v);
    printf("  %-26s %6.2f ns per redundant call\n", kCallNames[call], ns[call]);
  }
  printf("RESULT_F5");
  for (int call = 0; call < kNone; ++call) printf(" %s=%.2f", kCallNames[call], ns[call]);
  printf("\n");

  printf("  redundant PSSetShaderResources(0, n) with n identical SRVs (F2 per-slot cost):\n");
  double srvNs[17] = {};
  const int ns_[] = {1, 2, 4, 8, 16};
  for (int n : ns_) {
    std::vector<double> v;
    for (int r = 0; r < rounds; ++r) {
      const double b = RunF5(g, kNone, kDraws, 0);
      const double t = RunSrv(g, n, kDraws, kPer);
      v.push_back((t - b) / (static_cast<double>(kDraws) * kPer));
    }
    srvNs[n] = Median(v);
    printf("    n=%2d: %6.2f ns per call\n", n, srvNs[n]);
  }
  const double slope = (srvNs[16] - srvNs[1]) / 15.0;
  printf("RESULT_SRV per_call_1=%.2f per_slot=%.2f\n", srvNs[1], slope);
}

// ---------------------------------------------------------------- A1 -------

void Record(const Gpu& g, ID3D11DeviceContext* c, ID3D11Buffer* cb, int draws) {
  BindDrawState(g, c, cb);
  for (int i = 0; i < draws; ++i) Draw(c, cb, i);
}

void BenchA1(Gpu& g, int rounds) {
  const int kDraws = 5000, kThreads = 4;
  printf("\n== A1: %d draws, deferred context vs immediate (median of %d rounds) ==\n", kDraws, rounds);
  D3D11_FEATURE_DATA_THREADING th{};
  CHECK(g.dev->CheckFeatureSupport(D3D11_FEATURE_THREADING, &th, sizeof(th)));
  printf("  D3D11_FEATURE_THREADING: DriverConcurrentCreates=%d DriverCommandLists=%d\n", th.DriverConcurrentCreates,
         th.DriverCommandLists);
  printf("RESULT_THREADING concurrent_creates=%d command_lists=%d\n", th.DriverConcurrentCreates,
         th.DriverCommandLists);

  ID3D11DeviceContext* def[kThreads] = {};
  ID3D11Buffer* defCb[kThreads] = {};
  for (int t = 0; t < kThreads; ++t) {
    CHECK(g.dev->CreateDeferredContext(0, &def[t]));
    defCb[t] = MakeCb(g.dev);
  }

  std::vector<double> imm, immFlush, rec, fin, exe, par, parFin, parExe;
  for (int r = 0; r < rounds + 2; ++r) {
    const bool keep = r >= 2;  // two warm-up rounds
    // Immediate: issue only, then with the Flush that hands it to the driver.
    Drain(g);
    int64_t t0 = Now();
    Record(g, g.ctx, g.cb, kDraws);
    int64_t t1 = Now();
    g.ctx->Flush();
    int64_t t2 = Now();
    if (keep) {
      imm.push_back(Ns(t0, t1) / 1e6);
      immFlush.push_back(Ns(t0, t2) / 1e6);
    }
    // Deferred, one thread.
    Drain(g);
    ID3D11CommandList* cl = nullptr;
    t0 = Now();
    Record(g, def[0], defCb[0], kDraws);
    t1 = Now();
    CHECK(def[0]->FinishCommandList(FALSE, &cl));
    t2 = Now();
    g.ctx->ExecuteCommandList(cl, TRUE);
    const int64_t t3 = Now();
    Release(cl);
    if (keep) {
      rec.push_back(Ns(t0, t1) / 1e6);
      fin.push_back(Ns(t1, t2) / 1e6);
      exe.push_back(Ns(t2, t3) / 1e6);
    }
    // Deferred, 4 threads in parallel (4 x kDraws).
    Drain(g);
    ID3D11CommandList* cls[kThreads] = {};
    std::atomic<int> ready{0};
    std::atomic<bool> go{false};
    int64_t tEnd[kThreads] = {}, tRecEnd[kThreads] = {};
    HRESULT finishHr[kThreads] = {};  // CHECK exits, so the workers only store the result
    std::vector<std::thread> pool;
    for (int t = 0; t < kThreads; ++t)
      pool.emplace_back([&, t] {
        ready++;
        while (!go.load()) _mm_pause();
        Record(g, def[t], defCb[t], kDraws);
        tRecEnd[t] = Now();
        finishHr[t] = def[t]->FinishCommandList(FALSE, &cls[t]);
        tEnd[t] = Now();
      });
    while (ready.load() < kThreads) _mm_pause();
    t0 = Now();
    go = true;
    for (auto& th_ : pool) th_.join();
    for (int t = 0; t < kThreads; ++t) CHECK(finishHr[t]);
    const int64_t recEnd = *std::max_element(tRecEnd, tRecEnd + kThreads);
    const int64_t finEnd = *std::max_element(tEnd, tEnd + kThreads);
    t2 = Now();
    for (int t = 0; t < kThreads; ++t) g.ctx->ExecuteCommandList(cls[t], TRUE);
    const int64_t t4 = Now();
    for (auto& c : cls) Release(c);
    if (keep) {
      par.push_back(Ns(t0, recEnd) / 1e6);
      parFin.push_back(Ns(t0, finEnd) / 1e6);
      parExe.push_back(Ns(t2, t4) / 1e6);
    }
  }
  Drain(g);
  const double mImm = Median(imm), mImmF = Median(immFlush), mRec = Median(rec), mFin = Median(fin),
               mExe = Median(exe), mPar = Median(par), mParFin = Median(parFin), mParExe = Median(parExe);
  printf("  immediate: issue %.3f ms (%.0f ns/draw), issue + Flush %.3f ms\n", mImm, mImm * 1e6 / kDraws, mImmF);
  printf("  deferred, 1 thread: record %.3f ms (%.0f ns/draw), FinishCommandList %.3f ms, ExecuteCommandList(TRUE) "
         "%.3f ms on the immediate context\n",
         mRec, mRec * 1e6 / kDraws, mFin, mExe);
  printf("  deferred, %d threads x %d draws: record wall %.3f ms, record+Finish wall %.3f ms, %d x "
         "ExecuteCommandList(TRUE) %.3f ms\n",
         kThreads, kDraws, mPar, mParFin, kThreads, mParExe);
  printf("RESULT_A1 imm=%.3f imm_flush=%.3f rec1=%.3f fin1=%.3f exec1=%.3f rec4_wall=%.3f recfin4_wall=%.3f "
         "exec4=%.3f\n",
         mImm, mImmF, mRec, mFin, mExe, mPar, mParFin, mParExe);
  for (int t = 0; t < kThreads; ++t) {
    Release(def[t]);
    Release(defCb[t]);
  }
}

}  // namespace

int main(int argc, char** argv) {
  LARGE_INTEGER f;
  QueryPerformanceFrequency(&f);
  g_qpcToNs = 1e9 / static_cast<double>(f.QuadPart);
  const int rounds = argc > 1 ? std::max(3, atoi(argv[1])) : 15;
  Gpu g;
  Init(g);
  BenchF5(g, rounds);
  BenchA1(g, rounds);
  return 0;
}
