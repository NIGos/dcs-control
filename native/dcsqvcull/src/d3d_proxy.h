// R146: DCS's D3D11 objects behind a device proxy (ReShade, Special K, a debug layer). GitHub NIGos/dcs-control#8.
//
// A proxy such as ReShade (dxgi.dll / d3d11.dll in DCS's bin, or any name: it inline-hooks d3d11's
// D3D11CreateDevice* exports) hands DCS a wrapper device and a wrapper immediate context. Their tables are C++
// class vtables in the proxy's image (read-only data, shared by every context the proxy wraps, never rewritten),
// and each method forwards to the real d3d11 object after the proxy's own tracking (ReShade's add-on events:
// render-target / depth-stencil binds, draws, clears). The proxy's own work (ReShade's effects at Present, its
// overlay, its add-ons) runs on the real context underneath, where hooks on the wrapper's table see nothing.
// D3D11 child objects (shaders, resources) are not wrapped: their GetDevice reports the real device.
//
// What follows from it in this payload (docs/research/R146_reshade_compat.md):
//  - Every context-table hook patches the table of the context it is handed, i.e. the wrapper's: calls go DCS ->
//    our hook -> wrapper -> d3d11, so the proxy still sees every call we forward. Nothing bypasses it to d3d11's
//    own table.
//  - The split filter skips Set* calls whose arguments equal its shadow of DCS's context. Calls the proxy makes on
//    the real context (its state block around effects and overlay, an add-on that binds state mid-frame) never
//    reach the shadow; a skip after an incomplete restore leaves the proxy's state bound for DCS's draws. It is not
//    installed under a proxy ([D3D] ProxyGuard=1, the default; 0 hooks the wrapper's table as before).
//  - Shadow instancing compares CEffect's device (the wrapper) with its shaders' device (the real one) and refuses
//    it; the recorders and batching need that device and stay off. The tone-map toggle points take d3d11's own
//    entries only (fixation_depth.h InD3d11), PerceptionOnly's context check needs d3d11's IUnknown entries
//    (perception_device.h): under a wrapper those stay off, as before R146.
//  - Writes into a read-only wrapper table (protect / write / restore) are serialised (d3dstate.h).
//
// Detection: the IUnknown and GetDevice entries (slots 0..3) of an object's table. d3d11 keeps a context's table
// in the context object, but these four entries point into the system d3d11.dll; a wrapper's point into its own
// image. The system d3d11.dll is named by its full path: a proxy may itself be loaded as bin\d3d11.dll.
// ReShade: its image exports ReShadeRegisterAddon (ReShade 5.0+; checked on ReShade 6.8.0's ReShade64.dll).
// One log line per session ("d3d proxy: ..."), whichever feature finds the wrapper first.
// Included once from main.cpp (main/features.inc) after d3dstate.h and the globals it uses (Log, g_cfg).
#pragma once

namespace d3dproxy {

enum Kind { kUnknown = 0, kD3d11 = 1, kProxy = 2 };

struct Verdict {
  int kind = kUnknown;
  bool reshade = false;      // the owner exports ReShade's add-on API
  HMODULE owner = nullptr;   // the wrapper's module (kProxy; null when its code is in no module)
  std::wstring module;       // its path
  std::string version;       // its file version "a.b.c.d" (empty when it has none)
};

// Tests: this path counts as the system d3d11.dll (empty = GetSystemDirectory\d3d11.dll).
std::wstring g_testSystemD3d11;
// Tests: the test image is both the payload and the fake wrapper, so its entries are not skipped as ours.
bool g_testNoPayloadSkip = false;

inline HMODULE OwnerOf(const void* p) {
  HMODULE m = nullptr;
  if (!p || !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                static_cast<LPCWSTR>(p), &m))
    return nullptr;
  return m;
}

inline std::wstring PathOf(HMODULE m) {
  wchar_t path[MAX_PATH * 2] = {};
  const DWORD n = m ? GetModuleFileNameW(m, path, MAX_PATH * 2) : 0;
  return n && n < MAX_PATH * 2 ? std::wstring(path, n) : std::wstring();
}

inline std::wstring SystemD3d11Path() {
  if (!g_testSystemD3d11.empty()) return g_testSystemD3d11;
  wchar_t sys[MAX_PATH] = {};
  const UINT n = GetSystemDirectoryW(sys, MAX_PATH);
  return n && n < MAX_PATH ? std::wstring(sys, n) + L"\\d3d11.dll" : std::wstring();
}

inline bool IsSystemD3d11(HMODULE m) {
  if (!m) return false;
  const std::wstring p = PathOf(m), s = SystemD3d11Path();
  return !p.empty() && !s.empty() && _wcsicmp(p.c_str(), s.c_str()) == 0;
}

inline bool ExportsReShadeApi(HMODULE m) {
  return m && (GetProcAddress(m, "ReShadeRegisterAddon") != nullptr || GetProcAddress(m, "ReShadeRegisterEvent") != nullptr);
}

// File version from the module's own version resource (no version.dll).
inline std::string ModuleVersion(HMODULE m) {
  if (!m) return std::string();
  HRSRC r = FindResourceW(m, MAKEINTRESOURCEW(1), MAKEINTRESOURCEW(16));  // VS_VERSION_INFO, RT_VERSION
  HGLOBAL g = r ? LoadResource(m, r) : nullptr;
  const auto* p = g ? static_cast<const uint8_t*>(LockResource(g)) : nullptr;
  const DWORD size = r ? SizeofResource(m, r) : 0;
  if (!p || size < sizeof(VS_FIXEDFILEINFO)) return std::string();
  for (DWORD at = 0; at + sizeof(VS_FIXEDFILEINFO) <= size; at += 4) {
    VS_FIXEDFILEINFO fi;
    memcpy(&fi, p + at, sizeof(fi));
    if (fi.dwSignature != 0xFEEF04BD) continue;
    char b[64];
    snprintf(b, sizeof(b), "%u.%u.%u.%u", HIWORD(fi.dwFileVersionMS), LOWORD(fi.dwFileVersionMS),
             HIWORD(fi.dwFileVersionLS), LOWORD(fi.dwFileVersionLS));
    return b;
  }
  return std::string();
}

// Slots 0..3 (QueryInterface, AddRef, Release, GetDevice) of obj's table; false when unreadable.
inline bool ReadHead(const void* obj, void* out[4]) {
  __try {
    if (!obj || (reinterpret_cast<uintptr_t>(obj) & 7)) return false;
    void* const* vt = *static_cast<void* const* const*>(obj);
    if (!vt || (reinterpret_cast<uintptr_t>(vt) & 7)) return false;
    for (int i = 0; i < 4; ++i) out[i] = vt[i];
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

// A D3D11 device or context (any COM object whose slots 0..3 are IUnknown + GetDevice / d3d11's own).
// Entries of this payload are ignored (none of its hooks sits in these slots; a stale one would not say "proxy").
inline Verdict Classify(const void* obj) {
  Verdict v;
  void* head[4] = {};
  if (!ReadHead(obj, head)) return v;
  int own = 0;
  for (int i = 0; i < 4; ++i) {
    if (!g_testNoPayloadSkip && d3ds::InPayload(head[i])) continue;
    const HMODULE m = OwnerOf(head[i]);
    if (IsSystemD3d11(m)) {
      ++own;
      continue;
    }
    v.kind = kProxy;
    v.owner = m;
    v.module = m ? PathOf(m) : std::wstring(L"(code in no module)");
    v.reshade = ExportsReShadeApi(m);
    v.version = ModuleVersion(m);
    return v;
  }
  v.kind = own ? kD3d11 : kUnknown;
  return v;
}

// ---- session state and the one log line ----
std::atomic<int> g_seen{kUnknown};  // the first verdict a feature reported (kProxy latches)
std::mutex g_lineMutex;
std::string g_line;                 // the logged line (tests)

inline bool GuardOn() { return g_cfg.proxyGuard; }

// Logs the wrapper once per session. noun: "context" / "device"; who: the feature that found it; action: what
// that feature does about it.
inline void Report(const Verdict& v, const void* obj, const char* noun, const char* who, const char* action) {
  if (v.kind != kProxy) {
    int none = kUnknown;
    if (v.kind == kD3d11) g_seen.compare_exchange_strong(none, kD3d11);
    return;
  }
  if (g_seen.exchange(kProxy) == kProxy) return;
  std::string api;
  if (v.reshade) api = v.version.empty() ? " (ReShade add-on API)" : " (ReShade add-on API, version " + v.version + ")";
  else if (!v.version.empty()) api = " (version " + v.version + ")";
  char line[1024];
  qvlog::Format(line, sizeof(line),
                "d3d proxy: DCS's D3D11 %s %p is wrapped by %ls%s, found by %s; %s. Shadow instancing and the "
                "recorders refuse its device, the tone-map toggle points take d3d11's own entries only",
                noun, obj, v.module.c_str(), api.c_str(), who, action);
  {
    std::lock_guard<std::mutex> lock(g_lineMutex);
    g_line = line;
  }
  Log("%s", line);
}

// Split filter, before it hooks DCS's context table: true = leave it stock (a wrapper and [D3D] ProxyGuard=1).
inline bool RefuseSplitFilter(const void* ctx) {
  const Verdict v = Classify(ctx);
  const bool refuse = v.kind == kProxy && GuardOn();
  Report(v, ctx, "context", "the split filter",
         refuse ? "split filter left stock ([D3D] ProxyGuard=1)"
                : "split filter hooks the wrapper's table ([D3D] ProxyGuard=0)");
  return refuse;
}

// Shadow instancing found CEffect's device on another device than its shaders: names the wrapper, if it is one.
inline void NoteDevice(ID3D11Device* dev) {
  if (!dev || g_seen.load() == kProxy) return;
  Report(Classify(dev), dev, "device", "shadow instancing",
         g_cfg.splitFilter && GuardOn() ? "split filter left stock ([D3D] ProxyGuard=1)"
                                        : "context-table features follow the wrapper's table");
}

// Tests: a fresh session.
inline void ResetForTest() {
  g_seen = kUnknown;
  std::lock_guard<std::mutex> lock(g_lineMutex);
  g_line.clear();
}

}  // namespace d3dproxy
