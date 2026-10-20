// R95: the symbology emission companion's configuration and shared block (contract dcsvr_symbology.h; design
// docs/research/R95_symbology_export.md). The block follows the motion companion's rules exactly: mapped at the
// install when [Perception] SymbologyExport=1 (with CockpitMaskExport=1), its own reader heartbeat (R67: produced only
// while it AND the mask's are fresh), the shared set's generation and the mask's fence, R76's records on a release,
// R85's held record (the same generation and handles, active clear) across an export switch, RetireUnmapped when the
// export is off. The render-thread work (snapshots, accumulation, the tone map's second evaluation, the commit) is in
// symbology.h. Included from cockpit_mask.h inside namespace fixdepth::cmask, after the motion block.
#pragma once

std::atomic<bool> g_symOn{false};     // [Perception] SymbologyExport (default 0)
std::atomic<bool> g_symAlpha{true};   // [Perception] SymbologyAlpha (default 1: the coverage alpha in E.a)
// R97: [Dev] SymbologyPublishAlways (default 0): the symbology block's reader heartbeat is not required (the mask's is)
std::atomic<bool> g_symPublishAlways{false};
std::atomic<int> g_symVramNote{0};  // R97: the export turned on (1) / off (2): the render thread logs DXGI's usage
std::wstring g_sBlockName = csy::block_name;  // + DCSVR_PERCEPTION_SHM_SUFFIX (Configure)
bool SymWanted() { return g_on.load(std::memory_order_relaxed) && g_symOn.load(std::memory_order_relaxed); }

// R97 (Dev): DXGI's local-segment CurrentUsage of the device's adapter (bytes; -1 unknown). Process-wide: DCS's and
// OFXR's allocations in the same interval are in a difference too, so the lines that log it say when it was read.
inline int64_t LocalUsageBytes(ID3D11Device* dev) {
  if (!dev) return -1;
  IDXGIDevice* dx = nullptr;
  IDXGIAdapter* ad = nullptr;
  IDXGIAdapter3* a3 = nullptr;
  int64_t r = -1;
  if (SUCCEEDED(dev->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&dx))) && dx &&
      SUCCEEDED(dx->GetAdapter(&ad)) && ad &&
      SUCCEEDED(ad->QueryInterface(__uuidof(IDXGIAdapter3), reinterpret_cast<void**>(&a3))) && a3) {
    DXGI_QUERY_VIDEO_MEMORY_INFO i{};
    if (SUCCEEDED(a3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &i))) r = static_cast<int64_t>(i.CurrentUsage);
  }
  if (a3) a3->Release();
  if (ad) ad->Release();
  if (dx) dx->Release();
  return r;
}
// R97: a texture's allocation estimate (bytes): D3D11 has no GetResourceAllocationInfo (D3D12 only); the size rounded up
// to 64 KiB, the placement granularity of textures this large (R97 doc: the 5090's D3D12 sizes for the same descs).
inline uint64_t TexAlloc64k(uint32_t w, uint32_t h, uint32_t bytesPerTexel) {
  const uint64_t s = uint64_t(w) * h * bytesPerTexel;
  return (s + 65535) & ~uint64_t(65535);
}

// The symbology block's own reader heartbeat (symbology only while it AND the mask's are fresh).
bool g_sIdle = true;              // render thread
std::atomic<bool> g_sIdleA{true};
std::atomic<uint64_t> g_sToIdle{0}, g_sToActive{0};

HANDLE g_smap = nullptr;
std::atomic<csy::Header*> g_sview{nullptr};
csy::Header g_shdr{};

void PublishSymHeader(bool final = false) {
  csy::Header* v = g_sview.load();
  if (!v) return;
  LARGE_INTEGER q, f;
  QueryPerformanceCounter(&q);
  QueryPerformanceFrequency(&f);
  constexpr uint32_t hbFlags = csy::header_flags::reader_heartbeat | csy::header_flags::idle;
  if (!final)
    g_shdr.flags = (g_shdr.flags & ~hbFlags) | csy::header_flags::reader_heartbeat |
                   ((g_idle || g_sIdle) ? csy::header_flags::idle : 0u);
  g_shdr.magic = csy::block_magic;
  g_shdr.version = csy::block_version;
  g_shdr.structSize = sizeof(csy::Header);
  g_shdr.slotCount = csy::slot_count;
  g_shdr.format = csy::format_rgba16f;
  g_shdr.ownerPid = GetCurrentProcessId();
  g_shdr.revision = csy::contract_revision;
  g_shdr.heartbeatQpc = q.QuadPart;
  g_shdr.qpcFrequency = f.QuadPart;
  csy::seqlock_write(v, g_shdr);
}

bool OpenSymBlock(bool existingOnly = false) {
  if (g_sview.load()) return true;
  g_smap = existingOnly ? OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, g_sBlockName.c_str())
                        : CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, 4096, g_sBlockName.c_str());
  if (!g_smap) return false;
  void* v = MapViewOfFile(g_smap, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(csy::Header));
  if (!v) {
    CloseHandle(g_smap);
    g_smap = nullptr;
    return false;
  }
  g_sview = static_cast<csy::Header*>(v);
  return true;
}

// The record's handles and set-describing fields cleared (R76's record keeps only the generation).
void ClearSymSet() {
  for (auto& h : g_shdr.textureHandle) h = 0;
  for (auto& h : g_shdr.classTextureHandle) h = 0;
  g_shdr.fenceHandle = 0;
  g_shdr.classFormat = 0;
  g_shdr.emissionFlags = 0;
  g_shdr.atlasW = g_shdr.atlasH = 0;
}

void CloseSymBlock(bool keep = false) {  // keep: R85 (CloseBlock)
  if (csy::Header* v = g_sview.load()) {
    if (keep) {
      g_shdr.flags &= ~csy::header_flags::active;
    } else {
      g_shdr.flags = 0;
      ClearSymSet();
    }
    for (csy::EmissionSlot& s : g_shdr.slot) s = csy::EmissionSlot{};
    PublishSymHeader(true);
    g_sview = nullptr;
    UnmapViewOfFile(v);
  }
  if (g_smap) CloseHandle(g_smap);
  g_smap = nullptr;
}

// R85 / R86 (as RetireMotionRec): R76's record of this generation (flags 0, no handles).
void RetireSymRec() {
  g_shdr.generation = g_gen;
  g_shdr.flags = 0;
  ClearSymSet();
  for (csy::EmissionSlot& s : g_shdr.slot) s = csy::EmissionSlot{};
}
