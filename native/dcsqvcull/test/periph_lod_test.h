// Offline tests for periph_lod.h and uav_overlap.h (R26 §13): pass kinds from
// the inline pass name (and the fault guard), the factor scaling (same as
// lod_probe.h, +0x48 clamped), nothing installed with the key at 0, and on a
// WARP device the table swap (scaled tables in DCS's buffers after SetState(1),
// stock after SetState(0)) and the renamed duplicate's shadow view (same
// description, a different buffer, cached).
#pragma once

namespace plodtest {

struct FakePass {
  void* vt = nullptr;
  uint64_t len = 0;
  char name[40] = {};
};
FakePass MakePass(const char* n) {
  FakePass p;
  p.len = strlen(n);
  strcpy_s(p.name, n);
  return p;
}

ID3D11Buffer* Buf(ID3D11Device* dev, const uint32_t* data, UINT words) {
  D3D11_BUFFER_DESC d{};
  d.ByteWidth = words * 4;
  d.Usage = D3D11_USAGE_DEFAULT;
  d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
  d.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
  d.StructureByteStride = 16;
  D3D11_SUBRESOURCE_DATA s{data, 0, 0};
  ID3D11Buffer* b = nullptr;
  dev->CreateBuffer(&d, data ? &s : nullptr, &b);
  return b;
}

uint32_t FirstWord(ID3D11Buffer* b) {
  std::vector<uint8_t> v;
  if (!plod::ReadBack(b, v) || v.size() < 4) return 0xdeadbeef;
  uint32_t w;
  memcpy(&w, v.data(), 4);
  return w;
}

void Run() {
  printf("-- periph_lod.h / uav_overlap.h --\n");
  // Pass kinds.
  {
    FakePass a = MakePass("LightsBuffersPass"), b = MakePass("HeightMapPass"), c = MakePass("CascadeShadowPass3"),
             d = MakePass("CopyToHMDPass"), e = MakePass("GBufferPass"), f = MakePass("GBufferPassX"),
             g = MakePass("CascadeShadowPass");
    Check(plod::KindRaw(&a) == plod::kLights, "plod: LightsBuffersPass kind");
    Check(plod::KindRaw(&b) == plod::kHeight, "plod: HeightMapPass kind");
    Check(plod::KindRaw(&c) == plod::kCascade, "plod: CascadeShadowPass3 kind");
    Check(plod::KindRaw(&d) == plod::kHmd, "plod: CopyToHMDPass kind");
    Check(plod::KindRaw(&e) == plod::kGBuffer, "plod: GBufferPass kind");
    Check(plod::KindRaw(&f) == plod::kOther && plod::KindRaw(&g) == plod::kOther, "plod: near names are other");
    Check(plod::KindRaw(reinterpret_cast<void*>(0x10)) == plod::kOther, "plod: unreadable pass object is other");
    Check(uovl::Wanted(&a, 1) && uovl::Wanted(&c, 1) && uovl::Wanted(&e, 3) && !uovl::Wanted(&d, 1) &&
              uovl::Wanted(&d, 2),
          "uovl: chain passes for modes 1/3/4, every pass for mode 2");
  }
  // Factor scaling.
  {
    alignas(16) uint8_t src[plod::kOptBytes], dst[plod::kOptBytes];
    for (size_t i = 0; i < sizeof(src); ++i) src[i] = static_cast<uint8_t>(i * 7 + 1);
    const double d0 = 1.5, d10 = 0.6667, d50 = 1.0;
    const float f48 = 0.4f, f4c = 0.6667f;
    memcpy(src + 0x00, &d0, 8);
    memcpy(src + 0x10, &d10, 8);
    memcpy(src + 0x48, &f48, 4);
    memcpy(src + 0x4c, &f4c, 4);
    memcpy(src + 0x50, &d50, 8);
    Check(plod::CopyScaled(src, dst, 0.5), "plod: scaled copy made");
    double r10, r50, r0;
    float r48, r4c;
    memcpy(&r0, dst, 8);
    memcpy(&r10, dst + 0x10, 8);
    memcpy(&r48, dst + 0x48, 4);
    memcpy(&r4c, dst + 0x4c, 4);
    memcpy(&r50, dst + 0x50, 8);
    Check(r0 == 1.5 && r10 == d10 * 0.5 && r50 == 0.5 && r4c == static_cast<float>(f4c * 0.5) && r48 == 0.3f,
          "plod: +0x10/+0x4c/+0x50 halved, +0x48 clamped to 0.3, distance factor kept");
    bool restSame = memcmp(src + 0x18, dst + 0x18, 0x48 - 0x18) == 0 && memcmp(src + 0x58, dst + 0x58, 8) == 0;
    Check(restSame, "plod: other option bytes unchanged");
    Check(!plod::CopyScaled(reinterpret_cast<void*>(0x10), dst, 0.5), "plod: unreadable options refused");
    double key[5];
    Check(plod::ReadKey(src, key) && key[0] == 1.5 && key[2] == static_cast<double>(f48), "plod: factor key read");
  }
  // Key 0: nothing installed.
  {
    plod::Apply(0);
    Check(!plod::g_slot && !plod::g_swapOn.load() && plod::g_pct.load() == 0 &&
              ptiming::g_viewBoundary.load() == nullptr,
          "plod: PeripheralLodScale 0 installs nothing");
    Check(uovl::g_dup.load() == 0 && uovl::g_mode.load() == 0, "uovl: off by default");
  }
  // Table swap on WARP.
  ID3D11Device* dev = nullptr;
  ID3D11DeviceContext* ctx = nullptr;
  D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
  if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, &fl, 1, D3D11_SDK_VERSION, &dev, nullptr,
                               &ctx))) {
    Check(false, "plod: WARP device");
    return;
  }
  plod::g_dev = dev;
  plod::g_ctx = ctx;
  uint32_t stock[16], scaled[16], dcsv[16];
  for (int i = 0; i < 16; ++i) stock[i] = 100 + i, scaled[i] = 200 + i, dcsv[i] = 100 + i;
  for (int i = 0; i < 3; ++i) {
    plod::g_dcs[i] = Buf(dev, dcsv, 16);
    plod::g_stock[i] = Buf(dev, stock, 16);
    plod::g_scaled[i] = Buf(dev, scaled, 16);
    plod::g_dynamic[i] = false;
  }
  plod::g_state = 0;
  plod::SetState(1);
  Check(FirstWord(plod::g_dcs[0]) == 200 && FirstWord(plod::g_dcs[2]) == 200 && plod::g_state == 1,
        "plod: SetState(1) puts the scaled tables in DCS's buffers");
  plod::SetState(0);
  Check(FirstWord(plod::g_dcs[1]) == 100 && plod::g_state == 0, "plod: SetState(0) puts the stock tables back");
  // Snapshot: DCS's buffers -> our copies.
  ctx->UpdateSubresource(plod::g_dcs[0], 0, nullptr, scaled, 0, 0);
  ID3D11Buffer* tmp[3] = {Buf(dev, nullptr, 16), Buf(dev, nullptr, 16), Buf(dev, nullptr, 16)};
  std::vector<uint8_t> cpu[3];
  Check(plod::Snapshot(tmp, cpu) && FirstWord(tmp[0]) == 200 && FirstWord(tmp[1]) == 100,
        "plod: snapshot copies DCS's buffers");
  for (auto* t : tmp) t->Release();
  // Dynamic path: Map + memcpy from the CPU copies.
  {
    D3D11_BUFFER_DESC d{};
    d.ByteWidth = 64;
    d.Usage = D3D11_USAGE_DYNAMIC;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    ID3D11Buffer* dyn = nullptr;
    dev->CreateBuffer(&d, nullptr, &dyn);
    plod::g_dcs[2]->Release();
    plod::g_dcs[2] = dyn;
    plod::g_dynamic[2] = true;
    plod::g_cpuScaled[2].assign(reinterpret_cast<uint8_t*>(scaled), reinterpret_cast<uint8_t*>(scaled) + 64);
    plod::g_cpuStock[2].assign(reinterpret_cast<uint8_t*>(stock), reinterpret_cast<uint8_t*>(stock) + 64);
    plod::SetState(1);
    Check(dyn && FirstWord(dyn) == 200, "plod: dynamic buffer gets the scaled CPU copy");
    plod::SetState(0);
    Check(dyn && FirstWord(dyn) == 100, "plod: dynamic buffer gets the stock CPU copy");
  }
  // Renamed duplicate's shadow view.
  {
    ID3D11Buffer* b = Buf(dev, stock, 16);
    D3D11_UNORDERED_ACCESS_VIEW_DESC u{};
    u.Format = DXGI_FORMAT_UNKNOWN;
    u.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    u.Buffer.NumElements = 4;
    u.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_APPEND;
    ID3D11UnorderedAccessView* v = nullptr;
    dev->CreateUnorderedAccessView(b, &u, &v);
    ID3D11UnorderedAccessView* s0 = v ? uovl::ShadowOf(v, 0) : nullptr;
    ID3D11UnorderedAccessView* s0b = v ? uovl::ShadowOf(v, 0) : nullptr;
    ID3D11UnorderedAccessView* s1 = v ? uovl::ShadowOf(v, 1) : nullptr;
    bool same = false;
    if (s0) {
      D3D11_UNORDERED_ACCESS_VIEW_DESC sd{};
      s0->GetDesc(&sd);
      ID3D11Resource* r = nullptr;
      s0->GetResource(&r);
      same = memcmp(&sd, &u, sizeof(u)) == 0 && r != b;
      if (r) r->Release();
    }
    Check(s0 && same && s0 == s0b && s1 && s1 != s0, "uovl: shadow view has DCS's description, its own buffer, cached");
    uovl::ReleaseShadows();
    if (v) v->Release();
    b->Release();
  }
  plod::ReleaseCopies();
  plod::g_ctx = nullptr;
  plod::g_dev = nullptr;
  ctx->Release();
  dev->Release();
}

}  // namespace plodtest
