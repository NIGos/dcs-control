// Offline tests for gpu_dedup.h (R26 11): byte hashing and its fault guard,
// update sizes, and on a WARP device the content-id rules (equal uploads give
// equal ids, copies carry the source id, full clears are content-based), the
// dispatch signature (same inputs -> same signature, any input change -> a
// different one, outputs re-numbered after the call, hidden counters), and the
// capture record.
#pragma once

namespace gddtest {

using D3DCompileFn = HRESULT(WINAPI*)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*, LPCSTR, LPCSTR,
                                      UINT, UINT, ID3DBlob**, ID3DBlob**);

ID3D11Buffer* MakeBuffer(ID3D11Device* dev, UINT bytes, UINT bind, UINT misc = 0, UINT stride = 0) {
  D3D11_BUFFER_DESC d{};
  d.ByteWidth = bytes;
  d.Usage = D3D11_USAGE_DEFAULT;
  d.BindFlags = bind;
  d.MiscFlags = misc;
  d.StructureByteStride = stride;
  ID3D11Buffer* b = nullptr;
  dev->CreateBuffer(&d, nullptr, &b);
  return b;
}

ID3D11UnorderedAccessView* MakeUav(ID3D11Device* dev, ID3D11Buffer* b, UINT elems) {
  D3D11_UNORDERED_ACCESS_VIEW_DESC u{};
  u.Format = DXGI_FORMAT_R32_UINT;
  u.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
  u.Buffer.NumElements = elems;
  ID3D11UnorderedAccessView* v = nullptr;
  dev->CreateUnorderedAccessView(b, &u, &v);
  return v;
}

void Run() {
  // Hashing.
  {
    const uint8_t a[13] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13};
    uint8_t b[13];
    memcpy(b, a, sizeof(b));
    Check(gdd::HashBytes(a, sizeof(a)) == gdd::HashBytes(b, sizeof(b)) && gdd::HashBytes(a, sizeof(a)) != 0,
          "gdd: equal bytes hash equal (tail bytes included)");
    b[12] ^= 1;
    Check(gdd::HashBytes(a, sizeof(a)) != gdd::HashBytes(b, sizeof(b)), "gdd: a changed tail byte changes the hash");
    Check(gdd::HashBytes(reinterpret_cast<const void*>(0x10), 64) == 0, "gdd: unreadable memory hashes to 0 (no fault)");
    Check(gdd::Mix(gdd::Mix(1, 2), 3) != gdd::Mix(gdd::Mix(1, 3), 2), "gdd: Mix is order-sensitive");
  }
  // Update sizes.
  {
    gdd::ResInfo buf;
    buf.dim = D3D11_RESOURCE_DIMENSION_BUFFER;
    buf.bytes = 4096;
    D3D11_BOX box{16, 0, 0, 80, 1, 1};
    Check(gdd::UpdateBytes(buf, 0, nullptr, 0, 0) == 4096 && gdd::UpdateBytes(buf, 0, &box, 0, 0) == 64,
          "gdd: buffer update size, whole and boxed");
    gdd::ResInfo tex;
    tex.dim = D3D11_RESOURCE_DIMENSION_TEXTURE2D;
    tex.w = 256, tex.h = 128, tex.mips = 3, tex.fmt = DXGI_FORMAT_R8G8B8A8_UNORM;
    Check(gdd::UpdateBytes(tex, 1, nullptr, 512, 0) == 512u * 64, "gdd: texture update size uses the mip height");
    tex.fmt = DXGI_FORMAT_BC1_UNORM;
    Check(gdd::UpdateBytes(tex, 0, nullptr, 512, 0) == 512u * 32, "gdd: block-compressed rows are 4 texels high");
  }
  // WARP device.
  ID3D11Device* dev = nullptr;
  ID3D11DeviceContext* ctx = nullptr;
  D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
  if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, &fl, 1, D3D11_SDK_VERSION, &dev, nullptr,
                               &ctx))) {
    Check(false, "gdd: WARP device");
    return;
  }
  HMODULE dc = LoadLibraryW(L"d3dcompiler_47.dll");
  auto compile = dc ? reinterpret_cast<D3DCompileFn>(GetProcAddress(dc, "D3DCompile")) : nullptr;
  ID3D11ComputeShader* cs = nullptr;
  if (compile) {
    const char* src =
        "cbuffer C : register(b0) { uint4 k; };\n"
        "RWBuffer<uint> u : register(u0);\n"
        "[numthreads(4,1,1)] void main(uint3 t : SV_DispatchThreadID) { u[t.x] = u[t.x] + k.x; }\n";
    ID3DBlob* code = nullptr;
    ID3DBlob* err = nullptr;
    if (SUCCEEDED(compile(src, strlen(src), "gdd_test", nullptr, nullptr, "main", "cs_5_0", 0, 0, &code, &err)))
      dev->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &cs);
    if (code) code->Release();
    if (err) err->Release();
  }
  Check(cs != nullptr, "gdd: test compute shader compiled");
  // The probe's view of the session.
  gpt::g_ctx = ctx;
  gpt::g_dev = dev;
  gpt::g_rt = GetCurrentThreadId();
  gdd::Reset();
  gdd::g_hashCap = 16384;
  gdd::g_ctx1 = nullptr;
  ctx->QueryInterface(__uuidof(ID3D11DeviceContext1), reinterpret_cast<void**>(&gdd::g_ctx1));
  if (gdd::g_ctx1) gdd::g_ctx1->Release();
  gdd::g_phase = 1;

  ID3D11Buffer* cbA = MakeBuffer(dev, 16, D3D11_BIND_CONSTANT_BUFFER);
  ID3D11Buffer* cbB = MakeBuffer(dev, 16, D3D11_BIND_CONSTANT_BUFFER);
  ID3D11Buffer* ub = MakeBuffer(dev, 64, D3D11_BIND_UNORDERED_ACCESS);
  ID3D11Buffer* ub2 = MakeBuffer(dev, 64, D3D11_BIND_UNORDERED_ACCESS);
  ID3D11Buffer* cp = MakeBuffer(dev, 16, D3D11_BIND_CONSTANT_BUFFER);
  ID3D11UnorderedAccessView* uav = ub ? MakeUav(dev, ub, 16) : nullptr;
  ID3D11UnorderedAccessView* uav2 = ub2 ? MakeUav(dev, ub2, 16) : nullptr;
  const uint32_t k1[4] = {1, 2, 3, 4}, k2[4] = {5, 6, 7, 8};
  if (cbA && cbB && ub && ub2 && cp && uav && uav2 && cs) {
    // Content ids of uploads and copies.
    gdd::PreUpdate(ctx, cbA, 0, nullptr, k1, 0, 0);
    ctx->UpdateSubresource(cbA, 0, nullptr, k1, 0, 0);
    gdd::PreUpdate(ctx, cbB, 0, nullptr, k1, 0, 0);
    ctx->UpdateSubresource(cbB, 0, nullptr, k1, 0, 0);
    Check(gdd::Res(cbA).cid == gdd::Res(cbB).cid, "gdd: two uploads of equal bytes get equal content ids");
    gdd::PreCopy(ctx, cp, cbA);
    Check(gdd::Res(cp).cid == gdd::Res(cbA).cid, "gdd: CopyResource carries the source's content id");
    // Full clears are content-based: the same value on equal views gives equal ids.
    const UINT zero[4] = {0, 0, 0, 0};
    gdd::PreClearUAV(ctx, uav, zero);
    gdd::PreClearUAV(ctx, uav2, zero);
    Check(gdd::Res(ub).cid == gdd::Res(ub2).cid, "gdd: full clears with the same value give equal ids");
    const uint64_t cleared = gdd::Res(ub).cid;
    // Signature of a dispatch.
    ctx->CSSetShader(cs, nullptr, 0);
    ctx->CSSetConstantBuffers(0, 1, &cbA);
    ctx->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
    const uint32_t dims[3] = {4, 1, 1};
    gdd::Pre(ctx, gdd::kDispatch, dims, 3, nullptr, 0);
    const uint64_t sig1 = gdd::t_p.sig;
    Check(gdd::t_p.on && gdd::t_p.nOut == 1, "gdd: a dispatch's UAV is recorded as its output");
    ctx->Dispatch(4, 1, 1);
    gdd::Post();
    Check(gdd::Res(ub).cid != cleared && gdd::Res(ub).cid == gdd::Mix(sig1, ('C' << 8) | 0),
          "gdd: the output is re-numbered from the signature after the call");
    // Same inputs again (other CB with equal bytes, output cleared back to the same content).
    gdd::PreClearUAV(ctx, uav, zero);
    ctx->CSSetConstantBuffers(0, 1, &cbB);
    gdd::Pre(ctx, gdd::kDispatch, dims, 3, nullptr, 0);
    const uint64_t sig2 = gdd::t_p.sig;
    gdd::Post();
    Check(sig1 == sig2, "gdd: equal shader, CB bytes, dims and output content give equal signatures");
    // Any input change changes the signature.
    gdd::PreClearUAV(ctx, uav, zero);
    gdd::PreUpdate(ctx, cbB, 0, nullptr, k2, 0, 0);
    ctx->UpdateSubresource(cbB, 0, nullptr, k2, 0, 0);
    gdd::Pre(ctx, gdd::kDispatch, dims, 3, nullptr, 0);
    const uint64_t sig3 = gdd::t_p.sig, in3 = 0;
    (void)in3;
    gdd::Post();
    Check(sig3 != sig1, "gdd: changed CB bytes change the signature");
    gdd::PreClearUAV(ctx, uav, zero);
    const uint32_t dims2[3] = {2, 1, 1};
    ctx->CSSetConstantBuffers(0, 1, &cbA);
    gdd::Pre(ctx, gdd::kDispatch, dims2, 3, nullptr, 0);
    Check(gdd::t_p.sig != sig1, "gdd: changed dispatch size changes the signature");
    gdd::Post();
    // Prior output content is an input of the full signature.
    gdd::Pre(ctx, gdd::kDispatch, dims, 3, nullptr, 0);
    Check(gdd::t_p.sig != sig1, "gdd: an output not cleared back changes the full signature");
    gdd::Post();
    // Hidden counters: an initial count sets the counter id.
    const UINT cnt = 5;
    gdd::NoteCounts(ctx, 1, &uav, &cnt);
    Check(gdd::g_ctr.count(uav) && gdd::g_ctr[uav] == gdd::Mix(0x1c7ull, 5), "gdd: initial UAV counts are recorded");
    // Capture record.
    gdd::g_frames = new gdd::Frame[gdd::kMaxCap];
    gdd::g_capFrames = 1;
    gdd::g_cur = 0;
    gdd::g_phase = 2;
    gdd::OnPass(reinterpret_cast<void*>(0x1234), true, 0);
    gdd::Pre(ctx, gdd::kDispatch, dims, 3, nullptr, 0);
    gdd::Post();
    gdd::OnPass(reinterpret_cast<void*>(0x1234), false, 0);
    const gdd::Frame& f = gdd::g_frames[0];
    Check(f.ev.size() == 1 && f.ev[0].top == reinterpret_cast<void*>(0x1234) && f.ev[0].a[0] == 4 &&
              f.ev[0].binds.find("CS=S1") != std::string::npos && f.ev[0].binds.find("uC0=R") != std::string::npos &&
              f.ev[0].binds.find("cC0=R") != std::string::npos,
          "gdd: the capture record has the pass, arguments and bindings");
    Check(f.ev.size() == 1 && f.ev[0].stamp < 0, "gdd: no GPU stamp without queries");
    gdd::g_phase = 0;
    gdd::g_cur = -1;
    gdd::FreeFrames();
    // Off: nothing is tracked.
    const uint64_t before = gdd::Res(cbA).cid;
    gdd::PreUpdate(ctx, cbA, 0, nullptr, k2, 0, 0);
    Check(gdd::Res(cbA).cid == before, "gdd: with the phase off the hooks track nothing");
    ctx->ClearState();
  }
  gdd::Reset();
  gpt::g_ctx = nullptr;
  gpt::g_dev = nullptr;
  gpt::g_rt = 0;
  for (IUnknown* u : std::initializer_list<IUnknown*>{cbA, cbB, ub, ub2, cp, uav, uav2, cs})
    if (u) u->Release();
  ctx->Release();
  dev->Release();
}

}  // namespace gddtest
