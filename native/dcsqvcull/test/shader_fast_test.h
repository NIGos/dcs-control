// R36 wider grids for single-SM kernels (shader_fast.h): the grass cull and IM2 clear variants against HLSL
// transcriptions of the stock kernels on WARP; identification at dispatch time by output (stock objects created
// before the payload, decoys of the same shape rejected); substitution through the Dispatch hook, its re-arm
// after d3d11 puts its own entry back, the run-time mask, the live same-frame check (clean and with a spoiled
// result) and shutdown. When QV_R36_STOCK_DIR names a folder with DCS's own .dxbc files (extracted from
// Bazar\shaders\fxo\fxo.edcz, file name = md5), the variants are also checked against them; otherwise SKIP.
#pragma once

namespace shfasttest {
using namespace sfast;

// Stock grass2 grassPrepareCullDataTech (e837e87e), transcribed from the 2.9.30 disassembly: one 32x32 group,
// the 5x5 sample loops (y outer, x inner), the distance cut and the 6-plane box test, then the append.
const char kStockGrassHlsl[] = R"(
cbuffer CB0 : register(b0) { float4 c0[56]; }
cbuffer CB1 : register(b1) { float4 c1[39]; }
SamplerState s8 : register(s8);
Texture2D<float4> t0 : register(t0);
RWBuffer<uint> u0 : register(u0);
RWBuffer<uint> u1 : register(u1);
[numthreads(32, 32, 1)]
void main(uint3 tid : SV_GroupThreadID) {
  uint2 base = tid.xy << 5;
  float3 mn = float3(1e30, 1e30, 1e30);
  float3 mx = float3(-1e30, -1e30, -1e30);
  float dmin = 100000.0;
  [loop] for (uint y = 0; y <= 32; y += 8) {
    [loop] for (uint x = 0; x <= 32; x += 8) {
      float2 uv = (float2)(base + uint2(x, y));
      uv = uv + 0.5;
      uv = uv * c0[55].z;
      float4 r;
      r.xy = uv * 2.0 - 1.0;
      r.z = -r.y;
      r.w = 1.0;
      float h = t0.SampleLevel(s8, uv, 0).x;
      float3 p = float3(dot(r.xzw, c0[32].xyw), h - c1[35].y, dot(r.xzw, c0[34].xyw));
      mn = min(mn, p);
      mx = max(mx, p);
      float3 d = p - c1[38].xyz;
      dmin = min(dmin, sqrt(dot(d, d)));
    }
  }
  if (c0[55].x < dmin) return;
  float3 ctr = mn + mx;
  float4 ext = float4(mx, mx.x) - float4(mn, mn.x);
  float4 h4 = ext * float4(0.5, 0.5, 0.5, -0.5);
  float2 hn = ext.yz * -0.5;
  [loop] for (uint pl = 0; pl < 6; ++pl) {
    float4 P = c0[49 + pl];
    float3 q;
    q.x = P.x < 0 ? h4.w : h4.x;
    q.y = P.y < 0 ? hn.x : h4.y;
    q.z = P.z < 0 ? hn.y : h4.z;
    float3 a = ctr * 0.5 - q;
    float3 b = ctr * 0.5 + q;
    if (!(dot(P, float4(a, 1.0)) >= 0 || dot(P, float4(b, 1.0)) >= 0)) return;
  }
  uint at;
  InterlockedAdd(u1[8], 1, at);
  u0[at] = tid.y * 0x200000 + (tid.x << 5);
}
)";
// Same shape and bindings, another function (no culling): must be rejected by its output.
const char kDecoyGrassHlsl[] = R"(
cbuffer CB0 : register(b0) { float4 c0[56]; }
cbuffer CB1 : register(b1) { float4 c1[39]; }
SamplerState s8 : register(s8);
Texture2D<float4> t0 : register(t0);
RWBuffer<uint> u0 : register(u0);
RWBuffer<uint> u1 : register(u1);
[numthreads(32, 32, 1)]
void main(uint3 tid : SV_GroupThreadID) {
  if (t0.SampleLevel(s8, float2(0.5, 0.5), 0).x + c0[55].x + c1[38].x < -1e30) return;
  uint at;
  InterlockedAdd(u1[8], 1, at);
  u0[at] = tid.y * 0x200000 + (tid.x << 5);
}
)";
const char kStockClearHlsl[] =
    "RWBuffer<uint> u0 : register(u0);\n"
    "[numthreads(1, 1, 1)] void main(uint3 g : SV_GroupID) { u0[g.x] = 0; }\n";
const char kDecoyClearHlsl[] =
    "RWBuffer<uint> u0 : register(u0);\n"
    "[numthreads(1, 1, 1)] void main(uint3 g : SV_GroupID) { u0[g.x] = 1; }\n";

ID3D11ComputeShader* Make(ID3D11Device* d, const char* src, const char* name) {
  std::vector<uint8_t> code;
  ID3D11ComputeShader* cs = nullptr;
  if (chainfast::Compile(src, name, code, nullptr)) d->CreateComputeShader(code.data(), code.size(), nullptr, &cs);
  return cs;
}
ID3D11ComputeShader* MakeCode(ID3D11Device* d, const std::vector<uint8_t>& code) {
  ID3D11ComputeShader* cs = nullptr;
  if (!code.empty()) d->CreateComputeShader(code.data(), code.size(), nullptr, &cs);
  return cs;
}

struct GrassInputs {
  ID3D11ShaderResourceView* map[3] = {};
  ID3D11SamplerState* samp = nullptr;
  std::vector<std::pair<ID3D11Buffer*, ID3D11Buffer*>> cbs;
  std::vector<GrassCase> cases;
  void Make(ID3D11Device* d) {
    cases = GrassCases();
    for (int m = 0; m < 3; ++m) {
      std::vector<float> h;
      GrassMap(m, h);
      D3D11_TEXTURE2D_DESC td{1024, 1024, 1, 1, DXGI_FORMAT_R32_FLOAT, {1, 0}, D3D11_USAGE_DEFAULT,
                              D3D11_BIND_SHADER_RESOURCE, 0, 0};
      D3D11_SUBRESOURCE_DATA sd{h.data(), 1024 * 4, 0};
      ID3D11Texture2D* t = nullptr;
      d->CreateTexture2D(&td, &sd, &t);
      if (t) d->CreateShaderResourceView(t, nullptr, &map[m]), t->Release();
    }
    D3D11_SAMPLER_DESC s{D3D11_FILTER_MIN_MAG_MIP_LINEAR, D3D11_TEXTURE_ADDRESS_CLAMP, D3D11_TEXTURE_ADDRESS_CLAMP,
                         D3D11_TEXTURE_ADDRESS_CLAMP, 0, 1, D3D11_COMPARISON_NEVER, {0, 0, 0, 0}, 0, D3D11_FLOAT32_MAX};
    d->CreateSamplerState(&s, &samp);
    for (const GrassCase& g : cases) {
      std::vector<uint8_t> c0, c1;
      GrassCbs(g, c0, c1);
      cbs.emplace_back(MakeCbBytes(d, c0), MakeCbBytes(d, c1));
    }
  }
  void Release() {
    for (auto* m : map)
      if (m) m->Release();
    if (samp) samp->Release();
    for (auto& p : cbs) p.first->Release(), p.second->Release();
    *this = GrassInputs{};
  }
};
struct GrassOut {
  chainfast::TBuf u0, u1;
  void Release() { u0.Release(), u1.Release(); }
};
GrassOut GrassBufs(ID3D11Device* d, uint32_t start) {
  std::vector<uint32_t> s0(1024), s1(32);
  for (uint32_t i = 0; i < 1024; ++i) s0[i] = Sentinel(i);
  for (uint32_t i = 0; i < 32; ++i) s1[i] = 0x1000u + i;
  s1[8] = start;
  GrassOut o;
  chainfast::MakeBuf(d, 1024, 0, s0.data(), false, true, o.u0);
  chainfast::MakeBuf(d, 32, 0, s1.data(), false, true, o.u1);
  return o;
}
// Binds a grass case on the context (through its table, as DCS does) and dispatches gx groups.
void BindGrass(ID3D11DeviceContext* c, ID3D11ComputeShader* cs, const GrassInputs& in, size_t i, const GrassOut& o) {
  c->CSSetShader(cs, nullptr, 0);
  ID3D11Buffer* cb[2] = {in.cbs[i].first, in.cbs[i].second};
  c->CSSetConstantBuffers(0, 2, cb);
  c->CSSetShaderResources(0, 1, &in.map[in.cases[i].map]);
  c->CSSetSamplers(8, 1, &in.samp);
  ID3D11UnorderedAccessView* uu[3] = {o.u0.uav, o.u1.uav, nullptr};
  c->CSSetUnorderedAccessViews(0, 3, uu, nullptr);
}
bool Read2(ID3D11Device* d, ID3D11DeviceContext* c, const GrassOut& o, std::vector<uint32_t>& a, std::vector<uint32_t>& b) {
  return chainfast::ReadBuf(d, c, o.u0.b, o.u0.bytes, a) && chainfast::ReadBuf(d, c, o.u1.b, o.u1.bytes, b);
}
// Stock object (gx = 1) vs variant (gx = 32) on every case: same count, same set, rest untouched.
int GrassAgree(ID3D11Device* d, ID3D11DeviceContext* c, ID3D11ComputeShader* stock, ID3D11ComputeShader* var,
               const GrassInputs& in, int* nonTrivial) {
  int same = 0;
  *nonTrivial = 0;
  for (size_t i = 0; i < in.cases.size(); ++i) {
    GrassOut a = GrassBufs(d, in.cases[i].start), b = GrassBufs(d, in.cases[i].start);
    BindGrass(c, stock, in, i, a);
    c->Dispatch(1, 1, 1);
    BindGrass(c, var, in, i, b);
    c->Dispatch(kGrassRows, 1, 1);
    std::vector<uint32_t> a0, a1, b0, b1;
    if (Read2(d, c, a, a0, a1) && Read2(d, c, b, b0, b1) && SameGrass(a0, a1, b0, b1, in.cases[i].start)) {
      ++same;
      const uint32_t n = a1[8] - in.cases[i].start;
      if (n > 0 && n < 1024) ++*nonTrivial;
    }
    a.Release();
    b.Release();
  }
  ID3D11UnorderedAccessView* nul[3] = {};
  c->CSSetUnorderedAccessViews(0, 3, nul, nullptr);
  return same;
}

void Run() {
  ID3D11Device* dev = nullptr;
  ID3D11DeviceContext* ctx = nullptr;
  D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
  if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, &fl, 1, D3D11_SDK_VERSION, &dev, nullptr, &ctx))) {
    Check(false, "shader fast: WARP device");
    return;
  }
  // --- variants compile; grass variant vs the stock transcription; clear variants vs the CPU reference
  std::vector<uint8_t> gcode;
  std::string err;
  Check(chainfast::Compile(kSrcGrass, "qv_sf_grass", gcode, &err), "shader fast: grass variant compiles (cs_5_0, O3)");
  ID3D11ComputeShader* gvar = MakeCode(dev, gcode);
  ID3D11ComputeShader* gstock = Make(dev, kStockGrassHlsl, "stock_grass");
  ID3D11ComputeShader* gdecoy = Make(dev, kDecoyGrassHlsl, "decoy_grass");
  ID3D11ComputeShader* cstock = Make(dev, kStockClearHlsl, "stock_clear");
  ID3D11ComputeShader* cdecoy = Make(dev, kDecoyClearHlsl, "decoy_clear");
  Check(gvar && gstock && gdecoy && cstock && cdecoy, "shader fast: transcriptions and decoys compile");
  GrassInputs in;
  in.Make(dev);
  int nt = 0;
  const int agree = GrassAgree(dev, ctx, gstock, gvar, in, &nt);
  char what[256];
  sprintf_s(what, "shader fast: grass variant (32 groups) vs the stock transcription (1 group) on WARP: %d of %zu cases "
            "identical, %d with a partial cell set", agree, in.cases.size(), nt);
  Check(agree == static_cast<int>(in.cases.size()) && nt >= 3, what);
  {
    int ok = 0;
    for (UINT n : {256u, 1000u, 19527u}) {
      std::vector<uint8_t> code;
      if (chainfast::Compile(ClearSrc(n).c_str(), "qv_sf_clear", code, nullptr) && ClearSelfTest(dev, ctx, code, n)) ++ok;
    }
    Check(ok == 3, "shader fast: clear variants (N = 256, 1000, 19527) equal the CPU reference, tail untouched");
  }
  // --- DCS's own stock bytecode (optional)
  {
    const wchar_t* dir = _wgetenv(L"QV_R36_STOCK_DIR");
    if (!dir || !*dir) {
      QvSkip("shader fast: DCS's stock bytecode (set QV_R36_STOCK_DIR to a folder of extracted .dxbc)\n");
    } else {
      const std::vector<uint8_t> g = cftest::ReadAll(std::wstring(dir) + L"\\e837e87ef7fd45cada7f58d459f1d775.dxbc");
      const std::vector<uint8_t> c = cftest::ReadAll(std::wstring(dir) + L"\\8d918e57ed41b3c3389fee74bb5de7ce.dxbc");
      ID3D11ComputeShader* dg = MakeCode(dev, g);
      ID3D11ComputeShader* dc = MakeCode(dev, c);
      int nt2 = 0;
      const int a2 = dg ? GrassAgree(dev, ctx, dg, gvar, in, &nt2) : -1;
      sprintf_s(what, "shader fast: grass variant vs DCS's e837e87e on WARP: %d of %zu cases identical", a2,
                in.cases.size());
      Check(a2 == static_cast<int>(in.cases.size()) && nt2 >= 3, what);
      bool cok = dc != nullptr;
      for (UINT n : {300u, 19527u}) {
        if (!cok) break;
        std::vector<uint32_t> s(n + 77), r1, r2;
        for (uint32_t i = 0; i < s.size(); ++i) s[i] = Sentinel(i);
        chainfast::TBuf b1, b2;
        chainfast::MakeBuf(dev, n + 77, 0, s.data(), false, true, b1);
        chainfast::MakeBuf(dev, n + 77, 0, s.data(), false, true, b2);
        std::vector<uint8_t> code;
        chainfast::Compile(ClearSrc(n).c_str(), "qv_sf_clear", code, nullptr);
        ID3D11ComputeShader* v = MakeCode(dev, code);
        ctx->CSSetShader(dc, nullptr, 0);
        ctx->CSSetUnorderedAccessViews(0, 1, &b1.uav, nullptr);
        ctx->Dispatch(n, 1, 1);
        ctx->CSSetShader(v, nullptr, 0);
        ctx->CSSetUnorderedAccessViews(0, 1, &b2.uav, nullptr);
        ctx->Dispatch((n + 63) / 64, 1, 1);
        cok = v && chainfast::ReadBuf(dev, ctx, b1.b, b1.bytes, r1) && chainfast::ReadBuf(dev, ctx, b2.b, b2.bytes, r2) &&
              r1 == r2;
        if (v) v->Release();
        b1.Release();
        b2.Release();
      }
      Check(cok, "shader fast: clear variant vs DCS's 8d918e57 on WARP: every word equal (N = 300, 19527)");
      if (dg) dg->Release();
      if (dc) dc->Release();
    }
  }

  // --- identification at dispatch time on a WARP "DCS" device (objects created before the payload)
  ID3D11Device* saveDev = shadowinst::g_device;
  sfilt::Ctx* saveSf = sfilt::g_ctx.load();
  chainfast::Shutdown();
  shadowinst::g_device = dev;
  sfilt::g_ctx = nullptr;
  std::vector<uint32_t> cz(19527);
  for (uint32_t i = 0; i < cz.size(); ++i) cz[i] = Sentinel(i);
  chainfast::TBuf cbuf, sbuf;
  chainfast::MakeBuf(dev, 19527, 0, cz.data(), false, true, cbuf);
  chainfast::MakeBuf(dev, 8, 0, cz.data(), false, true, sbuf);
  auto clearDispatch = [&](ID3D11ComputeShader* cs, UINT n, ID3D11UnorderedAccessView* u) {
    ID3D11UnorderedAccessView* uu[2] = {u, nullptr};
    ctx->CSSetShader(cs, nullptr, 0);
    ctx->CSSetUnorderedAccessViews(0, 2, uu, nullptr);
    ctx->Dispatch(n, 1, 1);
  };
  auto frame = [&]() {
    GrassOut o = GrassBufs(dev, 0);
    BindGrass(ctx, gdecoy, in, 0, o);
    ctx->Dispatch(1, 1, 1);
    BindGrass(ctx, gstock, in, 1, o);
    ctx->Dispatch(1, 1, 1);
    clearDispatch(cdecoy, 19527, cbuf.uav);
    clearDispatch(cstock, 8, sbuf.uav);
    clearDispatch(cstock, 19527, cbuf.uav);
    ID3D11UnorderedAccessView* nul[3] = {};
    ctx->CSSetUnorderedAccessViews(0, 3, nul, nullptr);
    ctx->CSSetShader(nullptr, nullptr, 0);
    ctx->Flush();
    o.Release();
  };
  Apply(0x3);
  Check(g_vt && chainfast::g_ctx == ctx && chainfast::g_onSet.load() == &OnSet && g_grassVar.load() && g_identOn.load(),
        "shader fast: CSSetShader hook and re-arm callback in place, grass variant on the device, identification on");
  for (int i = 0; i < 600 && (g_found.load() != 0x3 || g_candTested < 4); ++i) {
    frame();
    Apply(0x3);
    Sleep(2);
  }
  sprintf_s(what, "shader fast id: both stock objects taken by their output, both decoys rejected (found 0x%x, %d "
            "tested, %d rejected)", g_found.load(), g_candTested, g_candRejected);
  Check(g_found.load() == 0x3 && g_candTested == 4 && g_candRejected == 2 && KindOf(gstock) == kGrassCull &&
            KindOf(cstock) == kImClear && KindOf(gdecoy) < 0 && KindOf(cdecoy) < 0,
        what);

  // --- substitution: grass
  auto runGrass = [&](size_t i, std::vector<uint32_t>& a0, std::vector<uint32_t>& a1) {
    GrassOut o = GrassBufs(dev, in.cases[i].start);
    BindGrass(ctx, gstock, in, i, o);
    ctx->Dispatch(1, 1, 1);
    Read2(dev, ctx, o, a0, a1);
    o.Release();
  };
  {
    const uint64_t s0 = g_subs[kGrassCull].load();
    std::vector<uint32_t> on0, on1, off0, off1;
    int same = 0;
    for (size_t i = 0; i < in.cases.size(); ++i) {
      Apply(0x3);
      runGrass(i, on0, on1);
      Apply(0);
      runGrass(i, off0, off1);
      same += SameGrass(off0, off1, on0, on1, in.cases[i].start);
    }
    Apply(0x3);
    sprintf_s(what, "shader fast: identified grass dispatches run the 32-group variant (%llu substitutions), results "
              "equal stock in %d of %zu cases", static_cast<unsigned long long>(g_subs[kGrassCull].load() - s0), same,
              in.cases.size());
    Check(g_subs[kGrassCull].load() - s0 == in.cases.size() && same == static_cast<int>(in.cases.size()), what);
    ID3D11ComputeShader* b = nullptr;
    ctx->CSGetShader(&b, nullptr, nullptr);
    if (b) b->Release();
    Check(b == gstock, "shader fast: DCS's own object is bound again after the substituted dispatch");
  }
  // --- substitution: clear (variant for N compiled on demand), small clears stay stock
  {
    for (int i = 0; i < 50 && !ClearVariant(19527); ++i) {
      clearDispatch(cstock, 19527, cbuf.uav);
      Apply(0x3);
    }
    const uint64_t s0 = g_subs[kImClear].load();
    const UINT junk[4] = {9, 9, 9, 9};
    ctx->ClearUnorderedAccessViewUint(cbuf.uav, junk);
    clearDispatch(cstock, 19527, cbuf.uav);
    clearDispatch(cstock, 8, sbuf.uav);
    std::vector<uint32_t> r;
    chainfast::ReadBuf(dev, ctx, cbuf.b, cbuf.bytes, r);
    bool zeros = r.size() == 19527;
    for (uint32_t v : r) zeros = zeros && v == 0;
    Check(ClearVariant(19527) && g_subs[kImClear].load() - s0 == 1 && zeros,
          "shader fast: the 19,527-group clear runs the 64-wide variant (all words 0); the 8-group clear stays stock");
  }
  // --- d3d11 puts its own Dispatch entry back (ExecuteCommandList/Flush): stock until the next bind re-arms
  {
    void** vt = *reinterpret_cast<void***>(ctx);
    d3ds::WriteTablePointer(&vt[g_slot], g_orig);
    const uint64_t s0 = g_subs[kGrassCull].load(), r0 = g_rearms.load();
    std::vector<uint32_t> a0, a1;
    GrassOut o = GrassBufs(dev, 0);
    BindGrass(ctx, gstock, in, 0, o);  // the bind re-arms the hook
    ctx->Dispatch(1, 1, 1);
    o.Release();
    Check(g_subs[kGrassCull].load() == s0 + 1 && g_rearms.load() > r0 &&
              vt[g_slot] == reinterpret_cast<void*>(&H_Dispatch),
          "shader fast: after d3d11 rewrites the Dispatch entry, the next bind of a kernel re-arms the hook");
  }
  // --- mask at run time
  {
    Apply(0x2);
    const uint64_t g0 = g_subs[kGrassCull].load(), c0 = g_subs[kImClear].load();
    std::vector<uint32_t> a0, a1;
    runGrass(0, a0, a1);
    clearDispatch(cstock, 19527, cbuf.uav);
    Check(g_subs[kGrassCull].load() == g0 && g_subs[kImClear].load() == c0 + 1,
          "shader fast: mask 0x2 substitutes the clear only");
    Apply(0x3);
  }
  // --- live same-frame check
  {
    auto drain = [&]() {
      for (int i = 0; i < 200 && g_pending.load() > 0; ++i) {
        ctx->Flush();
        ctx->CSSetShader(nullptr, nullptr, 0);  // the bind reads the slots back
        Sleep(1);
      }
    };
    g_verify = true;
    for (int k = 0; k < kKinds; ++k) g_checks[k] = g_diff[k] = 0;
    std::vector<uint32_t> a0, a1;
    for (size_t i = 0; i < in.cases.size(); ++i) runGrass(i, a0, a1);
    clearDispatch(cstock, 19527, cbuf.uav);
    drain();
    sprintf_s(what, "shader fast verify: stock re-run on the pre-dispatch copies equals the variants (grass %llu checks, "
              "%llu differ; clear %llu, %llu differ)", static_cast<unsigned long long>(g_checks[0].load()),
              static_cast<unsigned long long>(g_diff[0].load()), static_cast<unsigned long long>(g_checks[1].load()),
              static_cast<unsigned long long>(g_diff[1].load()));
    Check(g_checks[0].load() == in.cases.size() && g_diff[0].load() == 0 && g_checks[1].load() == 1 &&
              g_diff[1].load() == 0,
          what);
    g_testSpoil = true;
    runGrass(1, a0, a1);
    clearDispatch(cstock, 19527, cbuf.uav);
    drain();
    g_testSpoil = false;
    g_verify = false;
    Check(g_diff[0].load() == 1 && g_diff[1].load() == 1, "shader fast verify: a spoiled result is reported (both kinds)");
  }
  // --- shutdown
  {
    void** vt = *reinterpret_cast<void***>(ctx);
    const int slot = g_slot;
    Shutdown();
    Check(vt[slot] != reinterpret_cast<void*>(&H_Dispatch) && g_found.load() == 0 && g_nStock[0].load() == 0 &&
              !g_grassVar.load() && chainfast::g_onSet.load() == nullptr && KindOf(gstock) < 0,
          "shader fast: shutdown restores the Dispatch entry and releases kernels and variants");
    chainfast::Shutdown();
  }
  shadowinst::g_device = saveDev;
  sfilt::g_ctx = saveSf;
  cbuf.Release();
  sbuf.Release();
  in.Release();
  for (auto* s : {gvar, gstock, gdecoy, cstock, cdecoy})
    if (s) s->Release();
  ctx->ClearState();
  ctx->Release();
  dev->Release();
}

}  // namespace shfasttest
