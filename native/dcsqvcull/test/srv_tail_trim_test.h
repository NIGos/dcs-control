// Offline test for srv_tail_trim.h (R15 F2): the trimmed setShaderResources
// against a C++ transcription of dx11backend 0x1ac20 (and, when DCS is
// installed (dcs_install.h), against the real function bytes copied from dx11backend.dll):
//  1. fake cache arrays and a fake device: named grow / shrink / prefix / tail
//     cases with the exact bound ranges, then a seeded random sequence; after
//     every call the renderer bytes (counts and caches of all stages) and the
//     fake device slots must be identical to stock's, the trim never binds more;
//  2. a fake dx11backend (two call sites with ApplyShaderBlock's call shape, a
//     function entry, vcall thunks) on a real D3D11 device (hardware, WARP
//     fallback): stock run, then the sites patched to the trim; after every
//     call the VS/PS SRV slots read back with Get* must equal stock's. Then the
//     verify route (0 mismatches), a d3d11 hazard unbind that makes the trim
//     differ (verify latches, the stock state is in place), the byte checks,
//     and detach restoring both sites byte for byte.
#pragma once

namespace strtest {

namespace tr = srvtrim;
using Setter = tr::Setter;

constexpr size_t kSelfBytes = 0x118 + 6 * 0x400;  // through the CS cache

// dx11backend 0x1ac20, instruction for instruction [V 0x1ac20-0x1ad0a].
void __fastcall RefStock(uint8_t* self, int stage, void* setter, uint32_t /*start*/, uint32_t count,
                         void* const* views) {
  const int64_t st = stage;                                                 // movsxd rax, edx
  void** const sh = reinterpret_cast<void**>(self + 0x118 + (st << 10));    // r14
  uint32_t* const cnt = reinterpret_cast<uint32_t*>(self + 0xf8 + st * 4);  // r15 + 0xf8
  uint32_t end = count;
  if (count < *cnt) end = *cnt;  // cmp edi, [r15+0xf8]; cmovb esi, [r15+0xf8]
  uint32_t first = 0;
  for (uint32_t i = 0; i < count; ++i) {
    if (i == first && sh[i] == views[i]) first = i + 1;
    sh[i] = views[i];
  }
  if (*cnt > count) memset(sh + count, 0, static_cast<size_t>(*cnt - count) * 8);
  *cnt = count;
  if (end && end > first) reinterpret_cast<Setter>(setter)(*reinterpret_cast<void**>(self + 0x30), first, end - first, sh + first);
}

// ---------------------------------------------------------------------------
// 1. Fake arrays and a fake device
// ---------------------------------------------------------------------------

struct FakeDev {
  void* slots[6][128];
  uint64_t calls, bound;
  uint32_t lastFirst, lastN;
};

template <int K>
void __fastcall FakeSet(void* ctx, UINT first, UINT n, void* const* v) {
  auto* d = static_cast<FakeDev*>(ctx);
  for (UINT i = 0; i < n; ++i) d->slots[K][first + i] = v[i];
  d->calls++;
  d->bound += n;
  d->lastFirst = first;
  d->lastN = n;
}
void* const kFakeSetters[6] = {reinterpret_cast<void*>(&FakeSet<0>), reinterpret_cast<void*>(&FakeSet<1>),
                               reinterpret_cast<void*>(&FakeSet<2>), reinterpret_cast<void*>(&FakeSet<3>),
                               reinterpret_cast<void*>(&FakeSet<4>), reinterpret_cast<void*>(&FakeSet<5>)};

struct Side {
  alignas(16) uint8_t self[kSelfBytes];
  FakeDev dev;
  void Reset() {
    memset(this, 0, sizeof(*this));
    void* d = &dev;
    memcpy(self + 0x30, &d, 8);
  }
};

void* V(int i) { return i ? reinterpret_cast<void*>(static_cast<uintptr_t>(0x1000 + 0x10 * i)) : nullptr; }

uint64_t g_rng = 0x9e3779b97f4a7c15ull;
uint32_t Rand() {
  g_rng ^= g_rng << 13;
  g_rng ^= g_rng >> 7;
  g_rng ^= g_rng << 17;
  return static_cast<uint32_t>(g_rng >> 11);
}

// One call on both sides; true when the renderer bytes and device slots agree.
bool Both(Side& s, Side& t, srvspan::Fn stock, int stage, uint32_t count, void* const* views) {
  stock(s.self, stage, kFakeSetters[stage], 0, count, views);
  tr::Trim(t.self, stage, kFakeSetters[stage], 0, count, views);
  return memcmp(s.self + 0x38, t.self + 0x38, kSelfBytes - 0x38) == 0 &&
         memcmp(s.dev.slots, t.dev.slots, sizeof(s.dev.slots)) == 0 && t.dev.bound <= s.dev.bound;
}

void Logic() {
  // Update() alone: the named cases and their ranges.
  {
    void* cache[8] = {};
    uint32_t cnt = 0;
    void* a[5] = {V(1), V(2), V(3), V(4), V(5)};
    tr::Range r = tr::Update(cache, &cnt, 5, a);
    Check(r.first == 0 && r.end == 5 && r.stockEnd == 5 && cnt == 5, "srv trim: first bind binds all 5");
    r = tr::Update(cache, &cnt, 5, a);
    Check(r.first == 5 && r.end == 5 && r.stockEnd == 5, "srv trim: identical call binds nothing (as stock)");
    void* b[5] = {V(1), V(9), V(3), V(4), V(5)};
    r = tr::Update(cache, &cnt, 5, b);
    Check(r.first == 1 && r.end == 2 && r.stockEnd == 5, "srv trim: one changed slot: [1,2) instead of stock's [1,5)");
    void* c[3] = {V(1), V(9), V(7)};
    r = tr::Update(cache, &cnt, 3, c);
    Check(r.first == 2 && r.end == 5 && r.stockEnd == 5 && cnt == 3 && !cache[3] && !cache[4],
          "srv trim: shrink binds through the old count (nulled slots bound as stock)");
    void* d[2] = {V(1), V(9)};
    r = tr::Update(cache, &cnt, 2, d);
    Check(r.first == 2 && r.end == 3 && r.stockEnd == 3 && !cache[2],
          "srv trim: shrink with an identical prefix binds only the nulled slots");
    void* e[6] = {V(1), V(9), nullptr, nullptr, nullptr, V(6)};
    r = tr::Update(cache, &cnt, 6, e);
    Check(r.first == 5 && r.end == 6 && r.stockEnd == 6, "srv trim: grow over null slots binds from the first non-null");
    void* f[6] = {V(1), V(8), nullptr, nullptr, nullptr, V(6)};
    r = tr::Update(cache, &cnt, 6, f);
    Check(r.first == 1 && r.end == 2 && r.stockEnd == 6, "srv trim: identical tail of 4 left out");
    void* g[6] = {V(2), V(8), nullptr, V(3), nullptr, V(6)};
    r = tr::Update(cache, &cnt, 6, g);
    Check(r.first == 0 && r.end == 4 && r.stockEnd == 6, "srv trim: identical slots between two changes are bound");
    r = tr::Update(cache, &cnt, 0, nullptr);
    Check(r.first == 0 && r.end == 6 && r.stockEnd == 6 && cnt == 0, "srv trim: count 0 nulls the old range");
    r = tr::Update(cache, &cnt, 0, nullptr);
    Check(r.first == 0 && r.end == 0 && r.stockEnd == 0, "srv trim: count 0 twice binds nothing");
    void* h[4] = {nullptr, nullptr, nullptr, nullptr};
    r = tr::Update(cache, &cnt, 4, h);
    Check(r.first == 4 && r.end == 4 && r.stockEnd == 4 && cnt == 4,
          "srv trim: growing with nulls over null slots binds nothing (identical prefix, as stock)");
  }
  // The trimmed function against the stock transcription.
  static Side s, t;
  s.Reset();
  t.Reset();
  tr::g_fn = &RefStock;  // the guard's fallback
  bool ok = true;
  void* a[8] = {V(1), V(2), V(3), V(4), V(5), V(6), V(7), V(8)};
  ok &= Both(s, t, &RefStock, 4, 8, a);
  a[2] = V(9);
  ok &= Both(s, t, &RefStock, 4, 8, a);
  Check(ok && t.dev.lastFirst == 2 && t.dev.lastN == 1 && s.dev.lastFirst == 2 && s.dev.lastN == 6,
        "srv trim: real call binds [2,3), stock [2,8); caches and device equal");
  ok &= Both(s, t, &RefStock, 4, 3, a);
  Check(ok && t.dev.lastFirst == 3 && t.dev.lastN == 5, "srv trim: shrink 8 -> 3 binds the 5 nulled slots");
  const uint64_t c0 = t.dev.calls;
  ok &= Both(s, t, &RefStock, 4, 3, a);
  Check(ok && t.dev.calls == c0, "srv trim: no call when nothing differs");
  void* big[129] = {};
  ok &= Both(s, t, &RefStock, 0, 129, big);  // outside the analysed shape: both run stock
  Check(ok, "srv trim: count 129 falls back to the original function");
  s.Reset();
  t.Reset();

  // Random: few distinct views and long identical prefixes/tails, as in ApplyShaderBlock.
  ok = true;
  void* views[6][128] = {};
  uint64_t sb = 0, tb = 0;
  for (int step = 0; step < 300000 && ok; ++step) {
    const int stage = static_cast<int>(Rand() % 6);
    void** v = views[stage];
    const uint32_t mode = Rand() % 16;
    uint32_t count = mode == 0 ? Rand() % 129 : Rand() % 24;
    const uint32_t changes = Rand() % 4;
    for (uint32_t k = 0; k < changes && count; ++k) v[Rand() % count] = V(static_cast<int>(Rand() % 7));
    if (mode == 1) memset(v, 0, sizeof(views[0]));
    ok = Both(s, t, &RefStock, stage, count, v);
    sb = s.dev.bound;
    tb = t.dev.bound;
  }
  Check(ok, "srv trim: 300k random calls, renderer bytes and device slots identical to stock after each");
  printf("     random: stock bound %llu slots, trim %llu (%.1f%% fewer)\n", static_cast<unsigned long long>(sb),
         static_cast<unsigned long long>(tb), sb ? 100.0 * (sb - tb) / sb : 0.0);
}

// Optional: the transcription against the real bytes of the analysed build.
void* __cdecl MemsetShim(void* d, int c, size_t n) { return memset(d, c, n); }

void RealBytes() {
  if (!dcsinstall::Found()) {  // DCS_BIN / DCS_ROOT, Steam, registry (dcs_install.h)
    dcsinstall::Skip("srv trim (real setShaderResources bytes)");
    return;
  }
  const std::wstring path = dcsinstall::Bin() + L"dx11backend.dll";
  HMODULE m = LoadLibraryExW(path.c_str(), nullptr, LOAD_LIBRARY_AS_IMAGE_RESOURCE | LOAD_LIBRARY_AS_DATAFILE);
  if (!m) {
    printf("SKIP srv trim: %ls not mapped\n", path.c_str());
    return;
  }
  const uint8_t* base = sigscan::ImageBase(m);
  const uint32_t len = srvspan::kFnEnd - srvspan::kFn;
  if (shadowtex::Fnv(base + srvspan::kFn, len) != srvspan::kFnHash) {
    printf("SKIP srv trim: dx11backend.dll is not the analysed build\n");
    FreeLibrary(m);
    return;
  }
  auto* page = static_cast<uint8_t*>(VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
  memset(page, 0xCC, 4096);
  memcpy(page, base + srvspan::kFn, len);
  FreeLibrary(m);
  // The one rel32 out of the function: call memset at 0x1acc3 -> a shim in the page.
  const uint32_t call = 0x1acc3 - srvspan::kFn;
  uint8_t* shim = page + 0x200;
  shim[0] = 0x48;
  shim[1] = 0xB8;
  void* fn = reinterpret_cast<void*>(&MemsetShim);
  memcpy(shim + 2, &fn, 8);
  shim[10] = 0xFF;
  shim[11] = 0xE0;
  const int32_t rel = static_cast<int32_t>(shim - (page + call + 5));
  Check(page[call] == 0xE8, "srv trim: real bytes: memset call where the analysis has it");
  memcpy(page + call + 1, &rel, 4);
  DWORD old;
  VirtualProtect(page, 4096, PAGE_EXECUTE_READ, &old);
  FlushInstructionCache(GetCurrentProcess(), page, 4096);
  const auto real = reinterpret_cast<srvspan::Fn>(page);
  static Side s, t;
  s.Reset();
  t.Reset();
  bool ok = true;
  void* views[6][128] = {};
  for (int step = 0; step < 200000 && ok; ++step) {
    const int stage = static_cast<int>(Rand() % 6);
    void** v = views[stage];
    const uint32_t count = Rand() % 16 == 0 ? Rand() % 129 : Rand() % 24;
    for (uint32_t k = 0, n = Rand() % 4; k < n && count; ++k) v[Rand() % count] = V(static_cast<int>(Rand() % 7));
    real(s.self, stage, kFakeSetters[stage], 0, count, v);
    RefStock(t.self, stage, kFakeSetters[stage], 0, count, v);
    ok = memcmp(s.self + 0x38, t.self + 0x38, kSelfBytes - 0x38) == 0 &&
         memcmp(&s.dev, &t.dev, sizeof(FakeDev)) == 0;
  }
  Check(ok, "srv trim: transcription = dx11backend's real bytes (200k random calls, every bind call identical)");
  s.Reset();
  t.Reset();
  ok = true;
  for (int step = 0; step < 200000 && ok; ++step) {
    const int stage = static_cast<int>(Rand() % 6);
    void** v = views[stage];
    const uint32_t count = Rand() % 24;
    for (uint32_t k = 0, n = Rand() % 4; k < n && count; ++k) v[Rand() % count] = V(static_cast<int>(Rand() % 7));
    ok = Both(s, t, real, stage, count, v);
  }
  Check(ok, "srv trim: trim = dx11backend's real bytes in final cache and device state (200k random calls)");
  VirtualFree(page, 0, MEM_RELEASE);
}

// ---------------------------------------------------------------------------
// 2. Fake dx11backend on a real device
// ---------------------------------------------------------------------------

constexpr uint32_t kFnRva = 0x100, kCaller[2] = {0x200, 0x300}, kThunkPs = 0x400, kThunkVs = 0x410;
constexpr uint32_t kSiteOff = 0x18;
const uint32_t kSites[2] = {kCaller[0] + kSiteOff, kCaller[1] + kSiteOff};

alignas(4096) uint8_t g_fakeImage[0x1000];

void Thunk(uint8_t* t, const char* method) {
  const int off = DcsQv_ContextSlot(method) * 8;
  t[0] = 0x48, t[1] = 0x8B, t[2] = 0x01, t[3] = 0xFF;  // mov rax,[rcx]; jmp [rax+off]
  if (off < 0x80) {
    t[4] = 0x60, t[5] = static_cast<uint8_t>(off);
  } else {
    t[4] = 0xA0;
    memcpy(t + 5, &off, 4);
  }
}

uint8_t* MakeFake() {
  uint8_t* m = g_fakeImage;
  DWORD old = 0;
  if (!VirtualProtect(m, sizeof(g_fakeImage), PAGE_EXECUTE_READWRITE, &old)) return nullptr;
  memset(m, 0xCC, sizeof(g_fakeImage));
  // The "function": mov rax, RefStock; jmp rax.
  uint8_t* f = m + kFnRva;
  f[0] = 0x48, f[1] = 0xB8;
  void* ref = reinterpret_cast<void*>(&RefStock);
  memcpy(f + 2, &ref, 8);
  f[10] = 0xFF, f[11] = 0xE0;
  // Two callers passing their 6 arguments on (5th and 6th on the stack) via `call rel32`.
  const uint8_t body[] = {0x48, 0x83, 0xEC, 0x38,                    // sub rsp, 38h
                          0x48, 0x8B, 0x44, 0x24, 0x60,              // mov rax, [rsp+60h]
                          0x48, 0x89, 0x44, 0x24, 0x20,              // mov [rsp+20h], rax
                          0x48, 0x8B, 0x44, 0x24, 0x68,              // mov rax, [rsp+68h]
                          0x48, 0x89, 0x44, 0x24, 0x28,              // mov [rsp+28h], rax
                          0xE8, 0, 0, 0, 0,                          // call fn (site, +0x18)
                          0x48, 0x83, 0xC4, 0x38, 0xC3};             // add rsp, 38h; ret
  for (uint32_t c : kCaller) {
    memcpy(m + c, body, sizeof(body));
    const int32_t rel = static_cast<int32_t>(kFnRva - (c + kSiteOff + 5));
    memcpy(m + c + kSiteOff + 1, &rel, 4);
  }
  Thunk(m + kThunkPs, "PSSetShaderResources");
  Thunk(m + kThunkVs, "VSSetShaderResources");
  FlushInstructionCache(GetCurrentProcess(), m, sizeof(g_fakeImage));
  return m;
}

void ResetState() {
  tr::g_state = 0;
  tr::g_patched = 0;
  tr::g_disabled = false;
  tr::g_base = nullptr;
  tr::ResetCounters();
}

struct Bound {
  void* ps[128];
  void* vs[128];
  bool operator==(const Bound& o) const { return memcmp(this, &o, sizeof(*this)) == 0; }
};
Bound Read(ID3D11DeviceContext* c) {
  Bound b;
  ID3D11ShaderResourceView* v[128];
  c->PSGetShaderResources(0, 128, v);
  for (int i = 0; i < 128; ++i) {
    b.ps[i] = v[i];
    if (v[i]) v[i]->Release();
  }
  c->VSGetShaderResources(0, 128, v);
  for (int i = 0; i < 128; ++i) {
    b.vs[i] = v[i];
    if (v[i]) v[i]->Release();
  }
  return b;
}

void Device() {
  ID3D11Device* dev = nullptr;
  ID3D11DeviceContext* ic = nullptr;
  D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
  if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &fl, 1, D3D11_SDK_VERSION, &dev, nullptr,
                               &ic)) &&
      FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, &fl, 1, D3D11_SDK_VERSION, &dev, nullptr,
                               &ic))) {
    printf("SKIP srv trim: no D3D11 device\n");
    return;
  }
  uint8_t* m = MakeFake();
  if (!m) {
    printf("SKIP srv trim: fake module not writable\n");
    ic->Release();
    dev->Release();
    return;
  }
  // Views: 1x1 textures; the last one can also be a render target (hazard case).
  constexpr int kViews = 9;
  ID3D11Texture2D* tex[kViews] = {};
  ID3D11ShaderResourceView* srv[kViews] = {};
  ID3D11RenderTargetView* rtv = nullptr;
  for (int i = 0; i < kViews; ++i) {
    D3D11_TEXTURE2D_DESC d{};
    d.Width = d.Height = 1;
    d.MipLevels = d.ArraySize = 1;
    d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE | (i == kViews - 1 ? D3D11_BIND_RENDER_TARGET : 0);
    dev->CreateTexture2D(&d, nullptr, &tex[i]);
    if (tex[i]) dev->CreateShaderResourceView(tex[i], nullptr, &srv[i]);
  }
  if (tex[kViews - 1]) dev->CreateRenderTargetView(tex[kViews - 1], nullptr, &rtv);
  bool made = rtv != nullptr;
  for (auto* v : srv) made &= v != nullptr;
  if (!made) {
    printf("SKIP srv trim: views not created\n");
  } else {
    void* const setters[2] = {m + kThunkVs, m + kThunkPs};  // stage 0 VS, stage 4 PS
    static uint8_t self[kSelfBytes];
    auto resetSelf = [&]() {
      ic->ClearState();
      memset(self, 0, sizeof(self));
      void* c = ic;
      memcpy(self + 0x30, &c, 8);
    };
    // A fixed random sequence through the two caller functions.
    struct Step {
      int caller, stage;
      uint32_t count;
      void* v[24];
    };
    std::vector<Step> seq;
    {
      void* cur[2][24] = {};
      for (int i = 0; i < 4000; ++i) {
        Step s{};
        s.caller = static_cast<int>(Rand() % 2);
        const int k = static_cast<int>(Rand() % 2);
        s.stage = k ? 4 : 0;
        s.count = Rand() % 24;
        for (uint32_t c = 0, n = Rand() % 4; c < n && s.count; ++c)
          cur[k][Rand() % s.count] = Rand() % 5 ? srv[Rand() % (kViews - 1)] : nullptr;
        memcpy(s.v, cur[k], sizeof(s.v));
        seq.push_back(s);
      }
    }
    auto run = [&](std::vector<Bound>* out) {
      for (const Step& s : seq) {
        auto f = reinterpret_cast<srvspan::Fn>(m + kCaller[s.caller]);
        f(self, s.stage, setters[s.stage == 4], 0, s.count, s.v);
        out->push_back(Read(ic));
      }
    };
    ResetState();
    resetSelf();
    std::vector<Bound> stock, trim;
    run(&stock);
    const std::vector<uint8_t> stockSelf(self + 0x38, self + sizeof(self));
    uint8_t orig[2][5];
    for (int i = 0; i < 2; ++i) memcpy(orig[i], m + kSites[i], 5);

    // Byte checks: a site that does not call the function is refused.
    m[kSites[1]] = 0x90;
    Check(!tr::InstallAt(m, kSites, kFnRva, false) && tr::g_state.load() == -1,
          "srv trim: a call site with other bytes is refused (state -1)");
    memcpy(m + kSites[1], orig[1], 5);
    ResetState();
    Check(tr::InstallAt(m, kSites, kFnRva, false) && tr::g_state.load() == 1, "srv trim: fake module checked");
    Check(tr::Attach(false) && tr::g_patched.load() == 1 && tr::SiteCalls(m + kSites[0], tr::g_stub) &&
              tr::SiteCalls(m + kSites[1], tr::g_stub),
          "srv trim: both call sites go to the trim stub");
    resetSelf();
    run(&trim);
    bool same = trim.size() == stock.size();
    for (size_t i = 0; same && i < trim.size(); ++i) same = trim[i] == stock[i];
    Check(same, "srv trim: real device, 4000 calls through the patched sites: VS/PS SRV slots = stock after each");
    Check(memcmp(stockSelf.data(), self + 0x38, stockSelf.size()) == 0,
          "srv trim: renderer cache bytes after the run = stock's");

    // Verify route: same sequence, no mismatch, and the trims are counted.
    Check(tr::Attach(true) && tr::SiteCalls(m + kSites[0], tr::g_stub + 16), "srv trim: verify stub attached");
    tr::ResetCounters();
    resetSelf();
    std::vector<Bound> ver;
    run(&ver);
    same = ver.size() == stock.size();
    for (size_t i = 0; same && i < ver.size(); ++i) same = ver[i] == stock[i];
    Check(same && !tr::g_disabled.load() && tr::g_c.mismatches == 0 && tr::g_c.trimmed > 0 &&
              tr::g_c.verified == tr::g_c.trimmed && tr::g_c.slotsBound < tr::g_c.slotsStock,
          "srv trim: verify on the clean sequence: 0 mismatches, every trimmed call verified");
    printf("     verify: %llu calls, %llu trimmed, stock %llu slots, trim %llu\n",
           static_cast<unsigned long long>(tr::g_c.calls), static_cast<unsigned long long>(tr::g_c.trimmed),
           static_cast<unsigned long long>(tr::g_c.slotsStock), static_cast<unsigned long long>(tr::g_c.slotsBound));

    // Hazard: d3d11 unbinds an SRV whose resource becomes a render target; the
    // cache still holds it. Stock's next call rebinds it as part of the tail,
    // the trim leaves it out: verify must catch it and leave stock's state.
    tr::ResetCounters();
    resetSelf();
    auto call = [&](void* const* v, uint32_t n) {
      reinterpret_cast<srvspan::Fn>(m + kCaller[0])(self, 4, setters[1], 0, n, v);
    };
    void* va[3] = {srv[0], srv[1], srv[kViews - 1]};
    call(va, 3);
    ic->OMSetRenderTargets(1, &rtv, nullptr);  // unbinds PS slot 2
    ID3D11RenderTargetView* none = nullptr;
    ic->OMSetRenderTargets(1, &none, nullptr);
    void* vb[3] = {srv[2], srv[1], srv[kViews - 1]};
    call(vb, 3);  // trim binds [0,1); stock would bind [0,3) and restore slot 2
    ID3D11ShaderResourceView* got[3] = {};
    ic->PSGetShaderResources(0, 3, got);
    const bool stockState = got[0] == srv[2] && got[1] == srv[1] && got[2] == srv[kViews - 1];
    for (auto* g : got)
      if (g) g->Release();
    Check(tr::g_disabled.load() && tr::g_c.mismatches == 1 && stockState,
          "srv trim: a hazard-unbound tail slot is a verify mismatch: latched off, stock's binding in place");
    tr::LogCounters("test", 1);
    Check(!tr::Attach(false), "srv trim: latched off refuses to attach");

    tr::Detach();
    Check(tr::g_patched.load() == 0 && memcmp(m + kSites[0], orig[0], 5) == 0 && memcmp(m + kSites[1], orig[1], 5) == 0,
          "srv trim: detach restores both call sites byte for byte");
    // A site changed between install and attach is refused.
    ResetState();
    Check(tr::InstallAt(m, kSites, kFnRva, false), "srv trim: reinstall");
    m[kSites[0]] = 0x90;
    Check(!tr::Attach(false) && tr::g_state.load() == -1 && m[kSites[0]] == 0x90,
          "srv trim: a call site changed after the install is not patched (off)");
    memcpy(m + kSites[0], orig[0], 5);
    ResetState();
  }
  if (rtv) rtv->Release();
  for (auto* v : srv)
    if (v) v->Release();
  for (auto* t : tex)
    if (t) t->Release();
  ic->ClearState();
  ic->Release();
  dev->Release();
}

void Run() {
  Logic();
  RealBytes();
  Device();
}

}  // namespace strtest
