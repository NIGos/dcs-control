// R146 (GitHub NIGos/dcs-control#8, "DCS Control interferes with ReShade"): DCS's D3D11 context behind a device
// proxy. A fake proxy wraps a real (hardware or WARP) immediate context the way ReShade's D3D11DeviceContext does:
// a class vtable in read-only image data of a module that is not the system d3d11.dll, each entry counting its
// call and forwarding to the real context. Checks: the detection (d3d_proxy.h), that the proxy sees and keeps every
// call, the split filter left stock under the proxy ([D3D] ProxyGuard=1) with its sites restored and the wrapper's
// table untouched, the wrapper's table hooked consistently and restored with ProxyGuard=0, the one log line, and
// the serialised protect / write / restore path of d3ds::WriteTablePointer on a read-only page.
#pragma once

// The test image exports ReShade's add-on registration as ReShade64.dll does (6.8.0): a wrapper whose code lives
// in this image is "ReShade" to the detection.
extern "C" __declspec(dllexport) int ReShadeRegisterAddon(void*, unsigned int) { return 0; }

namespace r146test {

namespace sf = sfilt;
using Ctx = ID3D11DeviceContext1;

constexpr int kPxSlots = 160;  // >= ID3D11DeviceContext1's 144 entries
constexpr int kStub = 32;

// Code and table of the wrapper class inside this image: the stubs in an executable section, the table in a
// read-only one (as a C++ class vtable in the proxy's .rdata).
#pragma section(".qvpxs", read, execute)
#pragma section(".qvpxv", read)
__declspec(allocate(".qvpxs")) uint8_t g_pxStubs[kPxSlots * kStub] = {0xCC};
__declspec(allocate(".qvpxv")) void* g_pxVt[kPxSlots] = {nullptr};
volatile LONG64 g_pxCalls[kPxSlots];

// The wrapper object: its table, then the real object (what each stub forwards to).
struct FakeProxy {
  void** vt;
  void* orig;
};

// Stub i: count, then this = orig, jump to orig's own entry i.
void FillStubs(uint8_t* stubs) {
  memset(stubs, 0xCC, kPxSlots * kStub);
  for (int i = 0; i < kPxSlots; ++i) {
    uint8_t* p = stubs + kStub * i;
    volatile LONG64* counter = &g_pxCalls[i];
    const int32_t disp = i * 8;
    p[0] = 0x48, p[1] = 0xB8;  // mov rax, imm64
    memcpy(p + 2, &counter, 8);
    p[10] = 0xF0, p[11] = 0x48, p[12] = 0xFF, p[13] = 0x00;  // lock inc qword ptr [rax]
    p[14] = 0x48, p[15] = 0x8B, p[16] = 0x49, p[17] = 0x08;  // mov rcx, [rcx+8]
    p[18] = 0x48, p[19] = 0x8B, p[20] = 0x01;                // mov rax, [rcx]
    p[21] = 0xFF, p[22] = 0xA0;                              // jmp qword ptr [rax+disp32]
    memcpy(p + 23, &disp, 4);
  }
  FlushInstructionCache(GetCurrentProcess(), stubs, kPxSlots * kStub);
}

bool BuildProxyClass() {
  DWORD old = 0;
  if (!VirtualProtect(g_pxStubs, sizeof(g_pxStubs), PAGE_EXECUTE_READWRITE, &old)) return false;
  FillStubs(g_pxStubs);
  VirtualProtect(g_pxStubs, sizeof(g_pxStubs), PAGE_EXECUTE_READ, &old);
  if (!VirtualProtect(g_pxVt, sizeof(g_pxVt), PAGE_READWRITE, &old)) return false;
  for (int i = 0; i < kPxSlots; ++i) g_pxVt[i] = g_pxStubs + kStub * i;
  VirtualProtect(g_pxVt, sizeof(g_pxVt), PAGE_READONLY, &old);
  return true;
}

// The same class outside every image (code and a read-only table on their own pages): the split filter's R48 guard
// never hooks over code of the payload, and in the test the payload is this image, so ProxyGuard=0 needs this one.
struct HeapClass {
  uint8_t* stubs = nullptr;
  void** vt = nullptr;
};
bool BuildHeapClass(HeapClass& h) {
  h.stubs = static_cast<uint8_t*>(VirtualAlloc(nullptr, kPxSlots * kStub, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
  h.vt = static_cast<void**>(VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
  if (!h.stubs || !h.vt) return false;
  FillStubs(h.stubs);
  DWORD old = 0;
  VirtualProtect(h.stubs, kPxSlots * kStub, PAGE_EXECUTE_READ, &old);
  for (int i = 0; i < kPxSlots; ++i) h.vt[i] = h.stubs + kStub * i;
  VirtualProtect(h.vt, 4096, PAGE_READONLY, &old);
  return true;
}
void FreeHeapClass(HeapClass& h) {
  if (h.stubs) VirtualFree(h.stubs, 0, MEM_RELEASE);
  if (h.vt) VirtualFree(h.vt, 0, MEM_RELEASE);
  h = HeapClass{};
}
bool HeapTableIsStock(const HeapClass& h) {
  for (int i = 0; i < kPxSlots; ++i)
    if (h.vt[i] != h.stubs + kStub * i) return false;
  return true;
}

bool TableIsStock() {
  for (int i = 0; i < kPxSlots; ++i)
    if (g_pxVt[i] != g_pxStubs + kStub * i) return false;
  return true;
}

LONG64 Calls(const char* method) { return g_pxCalls[DcsQv_ContextSlot(method)]; }

std::string Line() {
  std::lock_guard<std::mutex> lock(d3dproxy::g_lineMutex);
  return d3dproxy::g_line;
}

// Writers on two slots of one read-only page, through the protect / write / restore path.
void ProtectRace() {
  auto* page = static_cast<void**>(VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
  if (!page) {
    Check(false, "R146: test page");
    return;
  }
  DWORD old = 0;
  VirtualProtect(page, 4096, PAGE_READONLY, &old);
  const uint64_t w0 = d3ds::g_protectWrites.load();
  static constexpr int kWrites = 4000;
  auto writer = [page](int slot) {
    for (int i = 0; i < kWrites; ++i)
      d3ds::WriteTablePointer(&page[slot], reinterpret_cast<void*>(static_cast<uintptr_t>(slot * 0x100000 + i)));
  };
  std::thread a(writer, 1), b(writer, 2);
  a.join();
  b.join();
  MEMORY_BASIC_INFORMATION mbi{};
  VirtualQuery(page, &mbi, sizeof(mbi));
  Check(page[1] == reinterpret_cast<void*>(static_cast<uintptr_t>(0x100000 + kWrites - 1)) &&
            page[2] == reinterpret_cast<void*>(static_cast<uintptr_t>(0x200000 + kWrites - 1)) &&
            d3ds::g_protectWrites.load() - w0 >= static_cast<uint64_t>(kWrites) && mbi.Protect == PAGE_READONLY,
        "R146: two writers on one read-only table page (a proxy's vtable): every write lands, no fault, the page "
        "read-only again");
  VirtualFree(page, 0, MEM_RELEASE);
}

void Run() {
  ProtectRace();
  ID3D11Device* dev = nullptr;
  ID3D11DeviceContext* ic = nullptr;
  D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
  if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &fl, 1, D3D11_SDK_VERSION, &dev, nullptr,
                               &ic)) &&
      FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, &fl, 1, D3D11_SDK_VERSION, &dev, nullptr,
                               &ic))) {
    QvSkip("R146: no D3D11 device\n");
    return;
  }
  Ctx* real = nullptr;
  ic->QueryInterface(__uuidof(ID3D11DeviceContext1), reinterpret_cast<void**>(&real));
  if (!real || !BuildProxyClass()) {
    Check(false, "R146: fake proxy class");
    if (real) real->Release();
    ic->Release();
    dev->Release();
    return;
  }
  d3dproxy::g_testNoPayloadSkip = true;  // this image is the payload and the fake wrapper
  FakeProxy proxyObj{g_pxVt, real};
  FakeProxy devProxyObj{g_pxVt, dev};  // classification reads only the table head
  auto* proxy = reinterpret_cast<Ctx*>(&proxyObj);

  // ---- detection ----
  {
    const d3dproxy::Verdict r = d3dproxy::Classify(real), d = d3dproxy::Classify(dev);
    Check(r.kind == d3dproxy::kD3d11 && d.kind == d3dproxy::kD3d11 && !r.reshade,
          "R146: d3d11's own context and device are not a proxy (slots 0..3 in the system d3d11.dll)");
    const d3dproxy::Verdict p = d3dproxy::Classify(proxy);
    Check(p.kind == d3dproxy::kProxy && p.owner == GetModuleHandleW(nullptr) && p.reshade &&
              _wcsicmp(p.module.c_str(), d3dproxy::PathOf(GetModuleHandleW(nullptr)).c_str()) == 0,
          "R146: a wrapper context whose table is in another image is a proxy, ReShade by its exported add-on API");
    // The system d3d11.dll by its full path: an image of another path counts as a proxy even when its code is
    // d3d11's (a proxy installed as bin\d3d11.dll is not mistaken for d3d11 by its name).
    d3dproxy::g_testSystemD3d11 = d3dproxy::PathOf(GetModuleHandleW(nullptr));
    const d3dproxy::Verdict swapped = d3dproxy::Classify(real), swappedP = d3dproxy::Classify(proxy);
    d3dproxy::g_testSystemD3d11.clear();
    Check(swapped.kind == d3dproxy::kProxy && !swapped.reshade && swappedP.kind == d3dproxy::kD3d11,
          "R146: the system d3d11.dll is recognised by its full path, not its name");
    Check(d3dproxy::Classify(nullptr).kind == d3dproxy::kUnknown, "R146: no object, no verdict");
  }

  // ---- the proxy sees and keeps every call ----
  {
    const LONG64 t0 = Calls("IASetPrimitiveTopology"), o0 = Calls("OMSetRenderTargets");
    proxy->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_LINELIST);
    proxy->OMSetRenderTargets(0, nullptr, nullptr);
    D3D11_PRIMITIVE_TOPOLOGY t = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
    real->IAGetPrimitiveTopology(&t);
    Check(t == D3D11_PRIMITIVE_TOPOLOGY_LINELIST && Calls("IASetPrimitiveTopology") == t0 + 1 &&
              Calls("OMSetRenderTargets") == o0 + 1 && proxy->GetType() == D3D11_DEVICE_CONTEXT_IMMEDIATE,
          "R146: the fake proxy counts each call and forwards it to the real context");
  }

  // ---- split filter under the proxy, [D3D] ProxyGuard=1 (default) ----
  const bool guardWas = g_cfg.proxyGuard;
  auto freshFilter = [] {
    sf::g_ctx = nullptr;
    sf::g_state = 0;
    sf::g_sites = sftest::kSites;
    sf::g_siteCount = 5;
    sf::g_tables = sftest::kTables;
    sf::g_tableCount = 6;
    sf::g_opMask = sf::kAllOps;
    sf::ResetCounters();
  };
  {
    d3dproxy::ResetForTest();
    g_cfg.proxyGuard = true;
    freshFilter();
    uint8_t* m = sftest::MakeFake();
    uint8_t before[0x1000];
    if (m) memcpy(before, m, sizeof(before));
    Check(m && sf::InstallAt(m, false), "R146 guard: fake sites pass the split filter's checks");
    Check(m && !sf::Attach() && sf::g_sitesPatched.load(), "R146 guard: sites patched while DCS's context is unknown");
    const LONG64 t0 = Calls("IASetPrimitiveTopology");
    if (m) reinterpret_cast<sftest::TopoFn>(m + sftest::kFn[3])(proxy, D3D11_PRIMITIVE_TOPOLOGY_POINTLIST);
    Check(sf::g_ctx.load() == proxy && Calls("IASetPrimitiveTopology") == t0 + 1,
          "R146 guard: the wrapper captured as DCS's context; the capturing call reached the proxy");
    const bool attached = sf::Attach();
    Check(!attached && sf::g_state.load() == -1 && !sf::Live() && !sf::g_hooked.load() && !sf::g_sitesPatched.load() &&
              m && memcmp(before, m, sizeof(before)) == 0 && TableIsStock(),
          "R146 guard: under a proxy the split filter is left stock (sites restored, the wrapper's table untouched)");
    Check(!sf::Install(), "R146 guard: latched for the session (no retry from the worker)");
    const std::string line = Line();
    Check(line.find("d3d proxy: DCS's D3D11 context") != std::string::npos &&
              line.find("ReShade add-on API") != std::string::npos &&
              line.find("split filter left stock ([D3D] ProxyGuard=1)") != std::string::npos,
          "R146 guard: one 'd3d proxy:' line names the wrapper, ReShade and what the split filter did");
    // The app's "turned itself off ... does not recognise this DCS version" scan must not match it.
    Check(line.find("could not") == std::string::npos && line.find("off for this session") == std::string::npos &&
              line.find("not installed") == std::string::npos && line.find("latched") == std::string::npos &&
              line.find("DISABLED") == std::string::npos && line.find("does not match") == std::string::npos,
          "R146 guard: the line carries none of the phrases DCS Control reads as an unrecognised DCS build");
    const LONG64 t1 = Calls("IASetPrimitiveTopology");
    if (m) {
      reinterpret_cast<sftest::TopoFn>(m + sftest::kFn[3])(proxy, D3D11_PRIMITIVE_TOPOLOGY_LINELIST);
      reinterpret_cast<sftest::TopoFn>(m + sftest::kFn[3])(proxy, D3D11_PRIMITIVE_TOPOLOGY_LINELIST);
    }
    Check(Calls("IASetPrimitiveTopology") == t1 + 2, "R146 guard: every DCS call reaches the proxy (none skipped)");
    // Logged once per session: a second finder adds no line.
    d3dproxy::NoteDevice(reinterpret_cast<ID3D11Device*>(&devProxyObj));
    Check(Line() == line, "R146: the wrapper is logged once per session");
  }

  // ---- [D3D] ProxyGuard=0: the wrapper's table hooked consistently, restored on detach ----
  {
    d3dproxy::ResetForTest();
    g_cfg.proxyGuard = false;
    HeapClass hc;
    const bool built = BuildHeapClass(hc);
    FakeProxy heapObj{hc.vt, real};
    auto* hp = reinterpret_cast<Ctx*>(&heapObj);
    freshFilter();
    uint8_t* m = sftest::MakeFake();
    uint8_t before[0x1000];
    if (m) memcpy(before, m, sizeof(before));
    const uint64_t pw0 = d3ds::g_protectWrites.load();
    const bool ok = m && sf::InstallAt(m, false) && !sf::Attach();
    if (m) reinterpret_cast<sftest::TopoFn>(m + sftest::kFn[3])(hp, D3D11_PRIMITIVE_TOPOLOGY_POINTLIST);
    const bool live = built && ok && sf::Attach() && sf::Live();
    const int topoSlot = DcsQv_ContextSlot("IASetPrimitiveTopology");
    Check(live && hc.vt[topoSlot] == sf::kHooks[sf::kTopo] && d3ds::g_protectWrites.load() > pw0 &&
              Line().find("ProxyGuard=0") != std::string::npos &&
              Line().find("(code in no module)") != std::string::npos,
          "R146 ProxyGuard=0: the split filter hooks the wrapper's read-only table (protect / write / restore)");
    const LONG64 t0 = Calls("IASetPrimitiveTopology");
    if (m) {
      reinterpret_cast<sftest::TopoFn>(m + sftest::kFn[3])(hp, D3D11_PRIMITIVE_TOPOLOGY_LINELIST);
      reinterpret_cast<sftest::TopoFn>(m + sftest::kFn[3])(hp, D3D11_PRIMITIVE_TOPOLOGY_LINELIST);
      hp->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);  // a foreign caller through the table
    }
    D3D11_PRIMITIVE_TOPOLOGY t = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
    real->IAGetPrimitiveTopology(&t);
    Check(Calls("IASetPrimitiveTopology") == t0 + 2 && t == D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST,
          "R146 ProxyGuard=0: a changed bind and a foreign call reach the proxy, the redundant repeat does not "
          "(our hook sits above the proxy: what it skips the proxy never sees)");
    sf::Detach();
    Check(built && HeapTableIsStock(hc) && m && memcmp(before, m, sizeof(before)) == 0 && !sf::g_hooked.load(),
          "R146 ProxyGuard=0: detach restores the wrapper's table and the sites");
    FreeHeapClass(hc);
  }

  // ---- shadow instancing's device check names the wrapper ----
  {
    d3dproxy::ResetForTest();
    g_cfg.proxyGuard = true;
    d3dproxy::NoteDevice(reinterpret_cast<ID3D11Device*>(&devProxyObj));
    const std::string line = Line();
    Check(line.find("D3D11 device") != std::string::npos && line.find("found by shadow instancing") != std::string::npos,
          "R146: shadow instancing's refused device names the wrapper");
    d3dproxy::ResetForTest();
    d3dproxy::NoteDevice(dev);
    Check(Line().empty() && d3dproxy::g_seen.load() == d3dproxy::kD3d11, "R146: d3d11's own device logs nothing");
  }

  // Leave the split filter as a fresh payload would find it.
  g_cfg.proxyGuard = guardWas;
  d3dproxy::g_testNoPayloadSkip = false;
  d3dproxy::ResetForTest();
  sf::g_ctx = nullptr;
  sf::g_state = 0;
  sf::g_sites = sf::kSites;
  sf::g_siteCount = static_cast<int>(sizeof(sf::kSites) / sizeof(sf::kSites[0]));
  sf::g_tables = sf::kTables;
  sf::g_tableCount = static_cast<int>(sizeof(sf::kTables) / sizeof(sf::kTables[0]));
  sf::g_opMask = sf::kDefaultOps;
  sf::ResetCounters();
  real->Release();
  ic->Release();
  dev->Release();
}

}  // namespace r146test
