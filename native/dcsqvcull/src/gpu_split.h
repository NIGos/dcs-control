// GPU split of the recorders' replays ([Suite] GpuSplit, default 0). Measure
// only: nothing DCS draws is skipped, reordered or changed, and with the key at
// 0 none of this runs (the recorders test one relaxed load per pass).
//
// R26 follow-up (2026-10-11): how much of the cascade and G-buffer GPU work is
// the recorders' model draws (shadow_rec.h's casters, gb_rec.h's items) and
// how much is the rest of the pass (terrain, GPU-driven trees and instancers,
// residual items), and how much vertex-shader work a vertex-cache reorder of
// the model meshes' index buffers could save.
//
// Mode 1 (brackets, run with gpu_pass_timing.h's phase): around each
// ExecuteCommandList group (shadow_rec: one per cascade pass; gb_rec: one per
// executed segment) the render thread ends a D3D11_QUERY_TIMESTAMP and begins
// a D3D11_QUERY_PIPELINE_STATISTICS and a D3D11_QUERY_OCCLUSION, and after
// the lists ends them and another timestamp. A D3D11_QUERY_TIMESTAMP_DISJOINT
// spans each frame (from its first bracket to the next frame's first). kRing
// frames in flight, read back with DONOTFLUSH, never waiting; late frames are
// dropped (counted). Queries set no pipeline binding, so the recorders' state
// restore and the split filter's shadow are unaffected. The pairs nest inside
// gpu_pass_timing.h's per-pass statistics pair (D3D11 allows several active
// queries of one type).
//
// Mode 2 (census, its own phase): for every draw in the executed lists
// (shadow: groups, weight = casters; G-buffer: alive candidates, weight 1)
// the mesh's index buffer (offset 0, start 0, its index count: the
// recorders' only draw form) is counted per cascade / execution slot. Each
// distinct (buffer, format, count) is copied once into a staging buffer
// (render thread, CopySubresourceRegion right after the pass's lists; shadow
// meshes also copy their vertex buffer when it is at most kMaxVbBytes), mapped
// a few frames later with DO_NOT_WAIT and copied out; then, on the suite
// thread (and helper threads), per mesh:
//  - post-transform vertex reuse under meshoptimizer's analyzeVertexCache
//    model with NVIDIA's parameters (cache 32, warp 32, primitive group 32:
//    a batch holds at most 32 distinct vertices and 32 triangles; a vertex is
//    shaded once per batch it is used in), and the FIFO-16 model;
//  - the same after a Forsyth reorder (cache 16 and 32, the better one, never
//    worse than the original);
//  - shadow meshes: also with vertices merged when all their bytes are equal
//    (always exact for any VS) and when their first 12 bytes are equal
//    ([I] the position at offset 0; an upper bound for E2's merge).
// Weighted by draws (instances) per frame, summed per cascade / slot, and
// compared with mode 1's measured VS invocations (the model's calibration).
//
// Included once from main.cpp inside its anonymous namespace, after
// shadow_rec.h and gb_rec.h (whose hooks call Bracket / MeshDraw when g_on).
#pragma once

namespace gsplit {

using shrec::SafeRel;

constexpr int kSites = 2;      // 0 shadow cascade slot (0-3), 1 G-buffer recorder slot (0-7)
constexpr int kIdx = 8;
constexpr int kRing = 8;       // frames in flight
constexpr int kMaxBr = 160;    // brackets per frame
constexpr UINT kChunk = 16u << 20;          // staging chunk
constexpr uint64_t kMaxStaging = 768ull << 20;
constexpr UINT kMaxVbBytes = 8u << 20;
constexpr uint64_t kCopyPerCall = 48ull << 20;  // bytes copied per hook call
const char* const kSiteName[kSites] = {"shadow cascade slot", "gbuffer recorder slot"};

// ---------------------------------------------------------------------------
// Mode 1: brackets
// ---------------------------------------------------------------------------
struct Acc {
  uint64_t frames = 0;    // frames read in which this (site, idx) had a bracket
  uint64_t brackets = 0;
  double ms = 0;
  uint64_t ia = 0, vs = 0, raster = 0, ps = 0, samples = 0;
};
struct FrameQ {
  ID3D11Query* dis = nullptr;
  ID3D11Query* t0[kMaxBr] = {};
  ID3D11Query* t1[kMaxBr] = {};
  ID3D11Query* pq[kMaxBr] = {};
  ID3D11Query* oq[kMaxBr] = {};
  uint8_t site[kMaxBr] = {}, idx[kMaxBr] = {};
  int n = 0;
  bool pending = false;
};

ID3D11Device* g_dev = nullptr;
ID3D11DeviceContext* g_ctx = nullptr;
FrameQ* g_ring = nullptr;
int g_cur = -1;           // open frame slot (render thread)
int g_open = -1;          // open bracket in the current frame
int g_lastSite = -1, g_lastIdx = -1;
uint32_t g_shadowSeen = 0;
std::atomic<int> g_want{0};    // 1 brackets, 2 census; 0 stop
std::atomic<bool> g_done{false};
int g_drain = 0;
Acc g_acc[kSites][kIdx];
uint64_t g_framesRead = 0, g_framesOpened = 0, g_dropped = 0, g_disjoint = 0, g_overflow = 0, g_createFail = 0;
Acc g_last[kSites][kIdx];      // the last mode-1 result (calibration for mode 2)
uint64_t g_lastFrames = 0;

void ReleaseRing() {
  if (!g_ring) return;
  for (int f = 0; f < kRing; ++f) {
    FrameQ& q = g_ring[f];
    SafeRel(q.dis);
    for (int i = 0; i < kMaxBr; ++i) SafeRel(q.t0[i]), SafeRel(q.t1[i]), SafeRel(q.pq[i]), SafeRel(q.oq[i]);
  }
  delete[] g_ring;
  g_ring = nullptr;
}

bool CreateRing() {
  g_ring = new FrameQ[kRing];
  D3D11_QUERY_DESC d = {};
  for (int f = 0; f < kRing; ++f) {
    FrameQ& q = g_ring[f];
    d.Query = D3D11_QUERY_TIMESTAMP_DISJOINT;
    if (FAILED(g_dev->CreateQuery(&d, &q.dis))) return false;
    for (int i = 0; i < kMaxBr; ++i) {
      d.Query = D3D11_QUERY_TIMESTAMP;
      if (FAILED(g_dev->CreateQuery(&d, &q.t0[i])) || FAILED(g_dev->CreateQuery(&d, &q.t1[i]))) return false;
      d.Query = D3D11_QUERY_PIPELINE_STATISTICS;
      if (FAILED(g_dev->CreateQuery(&d, &q.pq[i]))) return false;
      d.Query = D3D11_QUERY_OCCLUSION;
      if (FAILED(g_dev->CreateQuery(&d, &q.oq[i]))) return false;
    }
  }
  return true;
}

// Reads a pending frame; false while its data is not there yet.
bool TryRead(FrameQ& q) {
  D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj{};
  if (g_ctx->GetData(q.dis, &dj, sizeof(dj), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) return false;
  struct Row {
    uint64_t a, b;
    D3D11_QUERY_DATA_PIPELINE_STATISTICS p;
    uint64_t s;
  };
  Row rows[kMaxBr];
  for (int i = 0; i < q.n; ++i) {
    if (g_ctx->GetData(q.t0[i], &rows[i].a, 8, D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK ||
        g_ctx->GetData(q.t1[i], &rows[i].b, 8, D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK ||
        g_ctx->GetData(q.pq[i], &rows[i].p, sizeof(rows[i].p), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK ||
        g_ctx->GetData(q.oq[i], &rows[i].s, 8, D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK)
      return false;
  }
  q.pending = false;
  if (dj.Disjoint || !dj.Frequency) {
    ++g_disjoint;
    return true;
  }
  ++g_framesRead;
  bool seen[kSites][kIdx] = {};
  for (int i = 0; i < q.n; ++i) {
    Acc& a = g_acc[q.site[i]][q.idx[i]];
    seen[q.site[i]][q.idx[i]] = true;
    a.brackets++;
    if (rows[i].b > rows[i].a) a.ms += (rows[i].b - rows[i].a) * 1000.0 / dj.Frequency;
    a.ia += rows[i].p.IAPrimitives;
    a.vs += rows[i].p.VSInvocations;
    a.raster += rows[i].p.CPrimitives;
    a.ps += rows[i].p.PSInvocations;
    a.samples += rows[i].s;
  }
  for (int s = 0; s < kSites; ++s)
    for (int k = 0; k < kIdx; ++k)
      if (seen[s][k]) g_acc[s][k].frames++;
  return true;
}

void PollAll() {
  for (int f = 0; f < kRing; ++f)
    if (g_ring[f].pending && f != g_cur) TryRead(g_ring[f]);
}

void CloseFrame() {
  if (g_cur < 0) return;
  FrameQ& q = g_ring[g_cur];
  g_ctx->End(q.dis);
  q.pending = true;
  g_cur = -1;
}

void OpenFrame() {
  const int next = static_cast<int>(g_framesOpened % kRing);
  FrameQ& q = g_ring[next];
  if (q.pending && !TryRead(q)) {
    q.pending = false;
    ++g_dropped;
  }
  q.n = 0;
  g_ctx->Begin(q.dis);
  g_cur = next;
  ++g_framesOpened;
}

// ---------------------------------------------------------------------------
// Mode 2: census of the recorded meshes
// ---------------------------------------------------------------------------
struct MeshKey {
  ID3D11Buffer* ib;
  UINT count;
  DXGI_FORMAT fmt;
  bool operator==(const MeshKey& o) const { return ib == o.ib && count == o.count && fmt == o.fmt; }
};
struct MeshKeyHash {
  size_t operator()(const MeshKey& k) const {
    return std::hash<uintptr_t>()(reinterpret_cast<uintptr_t>(k.ib)) ^ (static_cast<size_t>(k.count) * 0x9E3779B97F4A7C15ull) ^
           static_cast<size_t>(k.fmt);
  }
};
struct Loc {
  int chunk = -1;
  UINT off = 0, bytes = 0;
};
struct MeshRec {
  MeshKey key{};
  ID3D11Buffer* vb = nullptr;  // reference while queued
  UINT stride = 0;
  bool wantVb = false;
  double w[kSites][kIdx] = {};  // draws (instances) summed over the census frames
  double wTex[kSites][kIdx] = {};  // of those, textured (alpha-tested) shadow casters
  Loc ibLoc, vbLoc;
  bool ibFail = false, vbFail = false, queued = false;
  std::vector<uint8_t> ibData, vbData;
};
struct Chunk {
  ID3D11Buffer* buf = nullptr;
  UINT used = 0, size = 0;
  bool mapped = false;
  std::vector<uint8_t> data;
};
std::unordered_map<MeshKey, uint32_t, MeshKeyHash> g_meshIdx;
std::vector<MeshRec> g_meshes;
std::vector<uint32_t> g_copyQueue;  // mesh indexes to copy
size_t g_copyPos = 0;
std::vector<Chunk> g_chunks;
uint64_t g_stagingBytes = 0;
int g_censusFramesWanted = 60;
int g_censusFrames = 0;
enum { kCsCollect, kCsCopy, kCsMap, kCsDone };
int g_cs = kCsCollect;
uint64_t g_skipBig = 0, g_copyFail = 0;

bool AllocIn(UINT bytes, Loc& loc) {
  bytes = (bytes + 15u) & ~15u;
  if (!g_chunks.empty()) {
    Chunk& c = g_chunks.back();
    if (c.size - c.used >= bytes) {
      loc.chunk = static_cast<int>(g_chunks.size() - 1);
      loc.off = c.used;
      c.used += bytes;
      return true;
    }
  }
  const UINT size = (std::max)(bytes, kChunk);
  if (g_stagingBytes + size > kMaxStaging) return false;
  D3D11_BUFFER_DESC d = {};
  d.ByteWidth = size;
  d.Usage = D3D11_USAGE_STAGING;
  d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  Chunk c;
  if (FAILED(g_dev->CreateBuffer(&d, nullptr, &c.buf))) return false;
  c.size = size;
  c.used = bytes;
  g_stagingBytes += size;
  g_chunks.push_back(std::move(c));
  loc.chunk = static_cast<int>(g_chunks.size() - 1);
  loc.off = 0;
  return true;
}

bool CopyBuf(ID3D11Buffer* src, UINT bytes, Loc& loc) {
  if (!src || !bytes) return false;
  D3D11_BUFFER_DESC sd;
  src->GetDesc(&sd);
  if (sd.ByteWidth < bytes || sd.Usage == D3D11_USAGE_STAGING) return false;
  if (!AllocIn(bytes, loc)) return false;
  loc.bytes = bytes;
  D3D11_BOX box = {0, 0, 0, bytes, 1, 1};
  g_ctx->CopySubresourceRegion(g_chunks[loc.chunk].buf, 0, loc.off, 0, 0, src, 0, &box);
  return true;
}

// Render thread, after a pass's lists: copies queued buffers (bounded per call).
void PumpCopies() {
  uint64_t done = 0;
  while (g_copyPos < g_copyQueue.size() && done < kCopyPerCall) {
    MeshRec& m = g_meshes[g_copyQueue[g_copyPos++]];
    const UINT ibBytes = m.key.count * (m.key.fmt == DXGI_FORMAT_R32_UINT ? 4u : 2u);
    if (!CopyBuf(m.key.ib, ibBytes, m.ibLoc)) m.ibFail = true, ++g_copyFail;
    done += ibBytes;
    if (m.wantVb && m.vb) {
      D3D11_BUFFER_DESC vd;
      m.vb->GetDesc(&vd);
      if (vd.ByteWidth > kMaxVbBytes) {
        m.vbFail = true;
        ++g_skipBig;
      } else if (!CopyBuf(m.vb, vd.ByteWidth, m.vbLoc)) {
        m.vbFail = true;
        ++g_copyFail;
      }
      done += vd.ByteWidth;
    }
    SafeRel(m.key.ib);  // the copies are queued on the context; the source may go now
    SafeRel(m.vb);
  }
}

bool PumpMaps() {
  bool all = true;
  for (Chunk& c : g_chunks) {
    if (c.mapped) continue;
    D3D11_MAPPED_SUBRESOURCE ms;
    const HRESULT hr = g_ctx->Map(c.buf, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &ms);
    if (hr == DXGI_ERROR_WAS_STILL_DRAWING) {
      all = false;
      break;  // in order: the later ones are not done either
    }
    if (SUCCEEDED(hr)) {
      c.data.assign(static_cast<uint8_t*>(ms.pData), static_cast<uint8_t*>(ms.pData) + c.used);
      g_ctx->Unmap(c.buf, 0);
    }
    c.mapped = true;
    SafeRel(c.buf);
  }
  return all;
}

void FinishCensusData() {
  for (MeshRec& m : g_meshes) {
    if (!m.ibFail && m.ibLoc.chunk >= 0 && g_chunks[m.ibLoc.chunk].data.size() >= m.ibLoc.off + m.ibLoc.bytes) {
      const uint8_t* p = g_chunks[m.ibLoc.chunk].data.data() + m.ibLoc.off;
      m.ibData.assign(p, p + m.ibLoc.bytes);
    } else {
      m.ibFail = true;
    }
    if (m.wantVb && !m.vbFail && m.vbLoc.chunk >= 0 &&
        g_chunks[m.vbLoc.chunk].data.size() >= m.vbLoc.off + m.vbLoc.bytes) {
      const uint8_t* p = g_chunks[m.vbLoc.chunk].data.data() + m.vbLoc.off;
      m.vbData.assign(p, p + m.vbLoc.bytes);
    }
  }
  for (Chunk& c : g_chunks) std::vector<uint8_t>().swap(c.data);
}

// ---------------------------------------------------------------------------
// Render-thread entry points (called by shadow_rec.h / gb_rec.h when g_on)
// ---------------------------------------------------------------------------

// Frame detection: the G-buffer executions precede the cascades in a frame
// (R17 3), so a G-buffer bracket after a cascade one, a cascade seen twice, or
// a G-buffer slot lower than the previous one starts a new frame.
bool NewFrame(int site, int idx) {
  if (g_lastSite < 0) return true;
  if (site == 1 && g_lastSite == 0) return true;
  if (site == 0 && ((g_shadowSeen >> idx) & 1)) return true;
  if (site == 1 && g_lastSite == 1 && idx < g_lastIdx) return true;
  return false;
}

void FrameStep(int site, int idx) {
  if (NewFrame(site, idx)) {
    g_shadowSeen = 0;
    if (g_want.load(std::memory_order_relaxed) == 1 && g_ring) {
      CloseFrame();
      PollAll();
      OpenFrame();
    } else if (g_want.load(std::memory_order_relaxed) == 2 && g_cs == kCsCollect && g_lastSite >= 0) {
      // The first (partial) frame is not counted: collection starts at the first full one.
      if (++g_censusFrames > g_censusFramesWanted) g_cs = kCsCopy;
    }
  }
  if (site == 0) g_shadowSeen |= 1u << idx;
  g_lastSite = site;
  g_lastIdx = idx;
}

void Bracket(int site, int idx, bool begin) {
  if (site < 0 || site >= kSites || idx < 0 || idx >= kIdx) return;
  const int want = g_want.load(std::memory_order_acquire);
  if (want == 0) {
    // Stopping: close the open frame once, then read the rest back.
    if (!begin || g_done.load(std::memory_order_relaxed)) return;
    if (g_ring) {
      CloseFrame();
      PollAll();
      bool any = false;
      for (int f = 0; f < kRing; ++f) any |= g_ring[f].pending;
      if (any && ++g_drain < 240) return;
      for (int f = 0; f < kRing; ++f)
        if (g_ring[f].pending) ++g_dropped, g_ring[f].pending = false;
    }
    g_on.store(0, std::memory_order_relaxed);
    g_done.store(true, std::memory_order_release);
    return;
  }
  if (want == 1) {
    if (!g_ring) return;
    if (begin) {
      FrameStep(site, idx);
      FrameQ& q = g_ring[g_cur];
      if (q.n >= kMaxBr) {
        ++g_overflow;
        g_open = -1;
        return;
      }
      g_open = q.n;
      q.site[g_open] = static_cast<uint8_t>(site);
      q.idx[g_open] = static_cast<uint8_t>(idx);
      g_ctx->End(q.t0[g_open]);
      g_ctx->Begin(q.pq[g_open]);
      g_ctx->Begin(q.oq[g_open]);
    } else if (g_open >= 0 && g_cur >= 0) {
      FrameQ& q = g_ring[g_cur];
      g_ctx->End(q.oq[g_open]);
      g_ctx->End(q.pq[g_open]);
      g_ctx->End(q.t1[g_open]);
      q.n = g_open + 1;
      g_open = -1;
    }
    return;
  }
  // want == 2: census.
  if (begin) {
    FrameStep(site, idx);
    return;
  }
  if (g_cs == kCsCopy) {
    PumpCopies();
    if (g_copyPos >= g_copyQueue.size()) g_cs = kCsMap;
  } else if (g_cs == kCsMap) {
    if (PumpMaps()) {
      g_cs = kCsDone;
      g_on.store(0, std::memory_order_relaxed);
      g_done.store(true, std::memory_order_release);
    }
  }
}

// One recorded draw of the pass whose lists just ran (census, collect stage).
void MeshDraw(int site, int idx, ID3D11Buffer* ib, DXGI_FORMAT fmt, UINT count, UINT weight, bool textured,
              ID3D11Buffer* vb, UINT stride) {
  if (g_want.load(std::memory_order_relaxed) != 2 || g_cs != kCsCollect || g_censusFrames < 1) return;
  if (!ib || count < 3 || idx < 0 || idx >= kIdx) return;
  const MeshKey k{ib, count, fmt};
  auto it = g_meshIdx.find(k);
  uint32_t mi;
  if (it == g_meshIdx.end()) {
    mi = static_cast<uint32_t>(g_meshes.size());
    g_meshIdx.emplace(k, mi);
    g_meshes.emplace_back();
    MeshRec& m = g_meshes.back();
    m.key = k;
    ib->AddRef();
    if (site == 0 && vb && stride >= 12) {
      m.vb = vb;
      vb->AddRef();
      m.stride = stride;
      m.wantVb = true;
    }
    g_copyQueue.push_back(mi);
  } else {
    mi = it->second;
    MeshRec& m = g_meshes[mi];
    if (site == 0 && !m.wantVb && vb && stride >= 12) {
      m.vb = vb;
      vb->AddRef();
      m.stride = stride;
      m.wantVb = true;
    }
  }
  MeshRec& m = g_meshes[mi];
  m.w[site][idx] += weight;
  if (textured) m.wTex[site][idx] += weight;
}

// ---------------------------------------------------------------------------
// Vertex cache models and reorder (pure; test/gpu_split_test.h)
// ---------------------------------------------------------------------------

// meshoptimizer's analyzeVertexCache (MIT): vertices shaded for the index
// order under a cache of `cache` entries, warps of `warp` distinct vertices and
// primitive groups of `pg` triangles (0 = unlimited).
uint64_t VertsShaded(const uint32_t* idx, size_t n, uint32_t nv, unsigned cache, unsigned warp, unsigned pg,
                     std::vector<unsigned>& ts) {
  ts.assign(nv, 0);
  unsigned stamp = cache + 1, warpOff = 0, pgOff = 0;
  uint64_t shaded = 0;
  for (size_t i = 0; i + 2 < n; i += 3) {
    const uint32_t a = idx[i], b = idx[i + 1], c = idx[i + 2];
    const unsigned ac = (stamp - ts[a]) > cache, bc = (stamp - ts[b]) > cache, cc = (stamp - ts[c]) > cache;
    if ((pg && pgOff == pg) || (warp && warpOff + ac + bc + cc > warp)) {
      warpOff = 0;
      pgOff = 0;
      stamp += cache + 1;
    }
    for (int j = 0; j < 3; ++j) {
      const uint32_t v = idx[i + j];
      if (stamp - ts[v] > cache) {
        ts[v] = stamp++;
        ++shaded;
        ++warpOff;
      }
    }
    ++pgOff;
  }
  return shaded;
}
uint64_t ShadedNv(const std::vector<uint32_t>& ix, uint32_t nv, std::vector<unsigned>& ts) {
  return VertsShaded(ix.data(), ix.size(), nv, 32, 32, 32, ts);
}
uint64_t ShadedFifo16(const std::vector<uint32_t>& ix, uint32_t nv, std::vector<unsigned>& ts) {
  return VertsShaded(ix.data(), ix.size(), nv, 16, 0, 0, ts);
}

// Tom Forsyth's linear-speed vertex cache optimisation (cache `cs` <= 32).
std::vector<uint32_t> Forsyth(const std::vector<uint32_t>& ix, uint32_t nv, int cs) {
  const size_t nt = ix.size() / 3;
  std::vector<uint32_t> out;
  out.reserve(nt * 3);
  if (!nt) return out;
  constexpr int kMaxCache = 32;
  float posScore[kMaxCache + 3];
  for (int p = 0; p < cs + 3; ++p)
    posScore[p] = p < 3 ? 0.75f : powf(1.0f - (p - 3) * (1.0f / cs), 1.5f);
  float valScore[64];
  for (int v = 0; v < 64; ++v) valScore[v] = v ? 2.0f / sqrtf(static_cast<float>(v)) : 0.0f;
  std::vector<uint32_t> live(nv, 0), start(nv + 1, 0), adj(nt * 3);
  for (size_t i = 0; i < nt * 3; ++i) live[ix[i]]++;
  for (uint32_t v = 0; v < nv; ++v) start[v + 1] = start[v] + live[v];
  {
    std::vector<uint32_t> fill(start.begin(), start.end() - 1);
    for (size_t t = 0; t < nt; ++t)
      for (int j = 0; j < 3; ++j) adj[fill[ix[t * 3 + j]]++] = static_cast<uint32_t>(t);
  }
  std::vector<int8_t> cpos(nv, -1);
  std::vector<float> vs(nv), ts(nt);
  std::vector<uint8_t> added(nt, 0);
  auto vscore = [&](uint32_t v) -> float {
    const uint32_t l = live[v];
    if (!l) return -1.0f;
    float s = l < 64 ? valScore[l] : 2.0f / sqrtf(static_cast<float>(l));
    if (cpos[v] >= 0) s += posScore[cpos[v]];
    return s;
  };
  for (uint32_t v = 0; v < nv; ++v) vs[v] = vscore(v);
  for (size_t t = 0; t < nt; ++t) ts[t] = vs[ix[t * 3]] + vs[ix[t * 3 + 1]] + vs[ix[t * 3 + 2]];
  uint32_t cache[kMaxCache + 6];
  int csize = 0;
  size_t scan = 0;
  int64_t best = -1;
  {
    float bs = -1e30f;
    for (size_t t = 0; t < nt; ++t)
      if (ts[t] > bs) bs = ts[t], best = static_cast<int64_t>(t);
  }
  for (size_t emitted = 0; emitted < nt; ++emitted) {
    if (best < 0) {
      while (scan < nt && added[scan]) ++scan;
      best = static_cast<int64_t>(scan);
    }
    const size_t t = static_cast<size_t>(best);
    added[t] = 1;
    uint32_t tv[3] = {ix[t * 3], ix[t * 3 + 1], ix[t * 3 + 2]};
    out.insert(out.end(), tv, tv + 3);
    // Remove t from its vertices' live lists.
    for (int j = 0; j < 3; ++j) {
      const uint32_t v = tv[j];
      uint32_t* a = &adj[start[v]];
      const uint32_t l = live[v];
      for (uint32_t q = 0; q < l; ++q)
        if (a[q] == t) {
          a[q] = a[l - 1];
          live[v] = l - 1;
          break;
        }
    }
    // New cache: the triangle's vertices first, then the old order.
    uint32_t nc[kMaxCache + 6];
    int nn = 0;
    for (int j = 0; j < 3; ++j) {
      bool dup = false;
      for (int q = 0; q < nn; ++q) dup |= nc[q] == tv[j];
      if (!dup) nc[nn++] = tv[j];
    }
    for (int q = 0; q < csize; ++q) {
      const uint32_t v = cache[q];
      if (v != tv[0] && v != tv[1] && v != tv[2]) nc[nn++] = v;
    }
    // Positions; entries past cs + 3 drop out.
    for (int q = 0; q < nn; ++q) cpos[nc[q]] = q < cs + 3 ? static_cast<int8_t>(q) : static_cast<int8_t>(-1);
    // Scores of the vertices touched, then of their live triangles; the best one next.
    for (int q = 0; q < nn; ++q) vs[nc[q]] = vscore(nc[q]);
    best = -1;
    float bs = -1e30f;
    for (int q = 0; q < nn; ++q) {
      const uint32_t v = nc[q];
      const uint32_t* a = &adj[start[v]];
      for (uint32_t r = 0; r < live[v]; ++r) {
        const uint32_t u = a[r];
        const float s = vs[ix[u * 3]] + vs[ix[u * 3 + 1]] + vs[ix[u * 3 + 2]];
        ts[u] = s;
        if (s > bs) bs = s, best = static_cast<int64_t>(u);
      }
    }
    csize = (std::min)(nn, cs + 3);
    memcpy(cache, nc, csize * sizeof(uint32_t));
  }
  return out;
}

// Remap: vertices whose first `cmpBytes` bytes (all when 0) are equal share
// the first one's index.
std::vector<uint32_t> MergeIndices(const std::vector<uint32_t>& ix, const uint8_t* vb, uint32_t nv, uint32_t stride,
                                   uint32_t cmpBytes) {
  const uint32_t len = cmpBytes ? (std::min)(cmpBytes, stride) : stride;
  std::unordered_map<std::string, uint32_t> first;
  first.reserve(nv * 2);
  std::vector<uint32_t> remap(nv);
  for (uint32_t v = 0; v < nv; ++v) {
    std::string key(reinterpret_cast<const char*>(vb + static_cast<size_t>(v) * stride), len);
    auto it = first.emplace(std::move(key), v).first;
    remap[v] = it->second;
  }
  std::vector<uint32_t> out(ix.size());
  for (size_t i = 0; i < ix.size(); ++i) out[i] = remap[ix[i]];
  return out;
}

struct MeshResult {
  bool ok = false;
  uint64_t tris = 0, uniq = 0;
  uint64_t nvOrig = 0, nvOpt = 0, fifoOrig = 0, fifoOpt = 0;
  bool merged = false;
  uint64_t uniqExact = 0, uniqPos = 0;
  uint64_t nvExactOpt = 0, nvPosOpt = 0;  // merged + reordered (shadow)
};

std::vector<uint32_t> BestOrder(const std::vector<uint32_t>& ix, uint32_t nv, std::vector<unsigned>& ts,
                                uint64_t* shaded) {
  uint64_t best = ShadedNv(ix, nv, ts);
  std::vector<uint32_t> bestIx = ix;
  for (int cs : {16, 32}) {
    std::vector<uint32_t> o = Forsyth(ix, nv, cs);
    const uint64_t s = ShadedNv(o, nv, ts);
    if (s < best) best = s, bestIx.swap(o);
  }
  *shaded = best;
  return bestIx;
}

MeshResult Analyse(const MeshRec& m) {
  MeshResult r;
  if (m.ibFail || m.ibData.empty()) return r;
  const bool w32 = m.key.fmt == DXGI_FORMAT_R32_UINT;
  const size_t n = m.key.count - m.key.count % 3;
  if (m.ibData.size() < n * (w32 ? 4 : 2)) return r;  // the read-back is shorter than the draw
  std::vector<uint32_t> ix(n);
  uint32_t nv = 0;
  for (size_t i = 0; i < n; ++i) {
    ix[i] = w32 ? reinterpret_cast<const uint32_t*>(m.ibData.data())[i] : reinterpret_cast<const uint16_t*>(m.ibData.data())[i];
    // 0xFFFFFFFF (a strip cut, or unwritten staging) made ix + 1 wrap to 0,
    // nv stayed small and the arrays below were indexed out of bounds.
    if (ix[i] >= (1u << 26)) return r;
    nv = (std::max)(nv, ix[i] + 1);
  }
  if (!nv || nv > (1u << 26)) return r;
  std::vector<unsigned> ts;
  r.tris = n / 3;
  {
    std::vector<uint8_t> seen(nv, 0);
    for (uint32_t v : ix) r.uniq += !seen[v], seen[v] = 1;
  }
  r.nvOrig = ShadedNv(ix, nv, ts);
  r.fifoOrig = ShadedFifo16(ix, nv, ts);
  std::vector<uint32_t> opt = BestOrder(ix, nv, ts, &r.nvOpt);
  r.fifoOpt = ShadedFifo16(opt, nv, ts);
  if (!m.vbData.empty() && m.stride >= 12 && static_cast<uint64_t>(nv) * m.stride <= m.vbData.size()) {
    r.merged = true;
    for (int pass = 0; pass < 2; ++pass) {
      std::vector<uint32_t> mx = MergeIndices(ix, m.vbData.data(), nv, m.stride, pass ? 12 : 0);
      std::vector<uint8_t> seen(nv, 0);
      uint64_t u = 0;
      for (uint32_t v : mx) u += !seen[v], seen[v] = 1;
      uint64_t s = 0;
      BestOrder(mx, nv, ts, &s);
      (pass ? r.uniqPos : r.uniqExact) = u;
      (pass ? r.nvPosOpt : r.nvExactOpt) = s;
    }
  }
  r.ok = true;
  return r;
}

// ---------------------------------------------------------------------------
// Suite thread
// ---------------------------------------------------------------------------
bool Prepare() {
  if (!shrec::g_ctx) return false;
  if (!g_ctx) {
    g_ctx = shrec::g_ctx;
    g_ctx->AddRef();
    g_ctx->GetDevice(&g_dev);
  }
  return g_dev != nullptr;
}

void ResetCommon() {
  g_cur = -1;
  g_open = -1;
  g_lastSite = g_lastIdx = -1;
  g_shadowSeen = 0;
  g_drain = 0;
  g_done = false;
}

bool WaitDone(int ms) {
  for (int t = 0; t < ms && !g_done.load(std::memory_order_acquire); t += 10) Sleep(10);
  return g_done.load(std::memory_order_acquire);
}

// Mode 1, armed around gpu_pass_timing.h's phase by the suite.
bool BeginBrackets() {
  if (!Prepare()) return false;
  ResetCommon();
  for (auto& row : g_acc)
    for (Acc& a : row) a = Acc();
  g_framesRead = g_framesOpened = g_dropped = g_disjoint = g_overflow = 0;
  if (!g_ring && !CreateRing()) {
    ReleaseRing();
    ++g_createFail;
    Log("  gpu split: query creation failed");
    return false;
  }
  g_want.store(1, std::memory_order_release);
  g_on.store(1, std::memory_order_release);
  return true;
}

void ReportBrackets(const char* what) {
  const double fr = g_framesRead ? static_cast<double>(g_framesRead) : 1.0;
  Log("  gpu split (%s): %llu frames read of %llu opened (dropped %llu, disjoint %llu, bracket overflow %llu)", what,
      static_cast<unsigned long long>(g_framesRead), static_cast<unsigned long long>(g_framesOpened),
      static_cast<unsigned long long>(g_dropped), static_cast<unsigned long long>(g_disjoint),
      static_cast<unsigned long long>(g_overflow));
  Log("  gpu split: recorders' replays per frame read (millions; ms = sum of bracket intervals; executed = frames "
      "with a bracket):");
  Log("    %-24s %8s %9s %8s %8s %8s %8s %8s %7s", "where", "ms", "brackets", "IA", "VS", "raster", "PS", "samples",
      "exec%");
  double tot[kSites][6] = {};
  for (int s = 0; s < kSites; ++s)
    for (int k = 0; k < kIdx; ++k) {
      const Acc& a = g_acc[s][k];
      if (!a.brackets) continue;
      Log("    %-21s %2d %8.3f %9.2f %8.3f %8.3f %8.3f %8.3f %8.3f %6.1f%%", kSiteName[s], k, a.ms / fr,
          a.brackets / fr, a.ia / fr / 1e6, a.vs / fr / 1e6, a.raster / fr / 1e6, a.ps / fr / 1e6,
          a.samples / fr / 1e6, 100.0 * a.frames / fr);
      tot[s][0] += a.ms / fr, tot[s][1] += a.ia / fr / 1e6, tot[s][2] += a.vs / fr / 1e6;
      tot[s][3] += a.raster / fr / 1e6, tot[s][4] += a.ps / fr / 1e6, tot[s][5] += a.samples / fr / 1e6;
    }
  for (int s = 0; s < kSites; ++s)
    Log("    %-21s all %8.3f %9s %8.3f %8.3f %8.3f %8.3f %8.3f", kSiteName[s], tot[s][0], "", tot[s][1], tot[s][2],
        tot[s][3], tot[s][4], tot[s][5]);
}

void EndBrackets() {
  g_want.store(0, std::memory_order_release);
  if (!WaitDone(3000)) {
    Log("  gpu split: the render thread did not reach a recorder pass to stop (results below may be partial)");
    g_on.store(0, std::memory_order_release);
    Sleep(200);
  }
  memcpy(g_last, g_acc, sizeof(g_acc));
  g_lastFrames = g_framesRead;
  ReportBrackets("brackets");
  ReleaseRing();
}

// Mode 2: census, its own phase.
void Census(int frames) {
  if (!Prepare()) {
    Log("  gpu split census: no device context (shadow recorder not prepared)");
    return;
  }
  ResetCommon();
  g_meshIdx.clear();
  g_meshes.clear();
  g_copyQueue.clear();
  g_copyPos = 0;
  g_chunks.clear();
  g_stagingBytes = 0;
  g_censusFramesWanted = frames;
  g_censusFrames = 0;
  g_cs = kCsCollect;
  g_skipBig = g_copyFail = 0;
  g_want.store(2, std::memory_order_release);
  g_on.store(1, std::memory_order_release);
  const bool done = WaitDone(30000);
  g_want.store(0, std::memory_order_release);
  g_on.store(0, std::memory_order_release);
  Sleep(100);  // a hook call in flight
  if (!done) {
    Log("  gpu split census: not finished in 30 s (state %d, %d frames, %zu meshes, %zu of %zu copied)", g_cs,
        g_censusFrames, g_meshes.size(), g_copyPos, g_copyQueue.size());
    for (MeshRec& m : g_meshes) SafeRel(m.key.ib), SafeRel(m.vb);
    for (Chunk& c : g_chunks) {
      if (c.buf && !c.mapped) SafeRel(c.buf);
    }
    g_chunks.clear();
    return;
  }
  FinishCensusData();
  g_chunks.clear();
  const double cf = g_censusFramesWanted;
  Log("  gpu split census: %d frames, %zu distinct meshes (index buffer, format, count), staging %.0f MB, copy "
      "failures %llu, vertex buffers over %u MB skipped %llu",
      g_censusFramesWanted, g_meshes.size(), g_stagingBytes / 1048576.0, static_cast<unsigned long long>(g_copyFail),
      kMaxVbBytes >> 20, static_cast<unsigned long long>(g_skipBig));
  // Analyse on a few threads.
  std::vector<MeshResult> res(g_meshes.size());
  std::atomic<size_t> next{0};
  const int nt = 6;
  std::vector<std::thread> th;
  const int64_t t0 = defrec::Qpc();
  for (int t = 0; t < nt; ++t)
    th.emplace_back([&] {
      SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
      for (size_t i; (i = next.fetch_add(1)) < g_meshes.size();) res[i] = Analyse(g_meshes[i]);
    });
  for (auto& t : th) t.join();
  Log("  gpu split census: analysis %.1f s on %d threads", (defrec::Qpc() - t0) * defrec::QpcToUs() / 1e6, nt);
  // Per (site, idx) and per site, weighted per frame.
  struct Sum {
    double draws = 0, tris = 0, uniq = 0, nvO = 0, nvR = 0, fO = 0, fR = 0, drawsTex = 0, trisTex = 0, nvOTex = 0,
           nvRTex = 0;
    double mTris = 0, mNvO = 0, mUniq = 0, uExact = 0, uPos = 0, nvExact = 0, nvPos = 0;  // merged subset
    double failDraws = 0;
  };
  Sum sum[kSites][kIdx + 1];
  uint64_t meshesOk = 0, merged = 0;
  for (size_t i = 0; i < g_meshes.size(); ++i) {
    const MeshRec& m = g_meshes[i];
    const MeshResult& r = res[i];
    meshesOk += r.ok;
    merged += r.merged;
    for (int s = 0; s < kSites; ++s)
      for (int k = 0; k < kIdx; ++k) {
        const double w = m.w[s][k] / cf, wt = m.wTex[s][k] / cf;
        if (w <= 0) continue;
        for (Sum* x : {&sum[s][k], &sum[s][kIdx]}) {
          if (!r.ok) {
            x->failDraws += w;
            continue;
          }
          x->draws += w;
          x->tris += w * r.tris;
          x->uniq += w * r.uniq;
          x->nvO += w * r.nvOrig;
          x->nvR += w * r.nvOpt;
          x->fO += w * r.fifoOrig;
          x->fR += w * r.fifoOpt;
          x->drawsTex += wt;
          x->trisTex += wt * r.tris;
          x->nvOTex += wt * r.nvOrig;
          x->nvRTex += wt * r.nvOpt;
          if (r.merged) {
            x->mTris += w * r.tris;
            x->mNvO += w * r.nvOrig;
            x->mUniq += w * r.uniq;
            x->uExact += w * r.uniqExact;
            x->uPos += w * r.uniqPos;
            x->nvExact += w * r.nvExactOpt;
            x->nvPos += w * r.nvPosOpt;
          }
        }
      }
  }
  Log("  gpu split census: %llu meshes analysed, %llu with vertex data (shadow merge)",
      static_cast<unsigned long long>(meshesOk), static_cast<unsigned long long>(merged));
  Log("  vertex shading per frame (millions) under the NVIDIA model (cache 32, warp 32, prim group 32); 'reorder' = "
      "Forsyth 16/32, the better, never worse; FIFO-16 for reference; 'meas VS' = the last bracket run's measured VS "
      "invocations of the same lists:");
  Log("    %-24s %7s %7s %7s %7s %7s %6s %7s %7s %6s %7s %6s %7s", "where", "draws", "tris", "uniqV", "NV", "NV-re",
      "save%", "FIFO", "FIFO-re", "VS/tri", "measVS", "meas/NV", "unread");
  for (int s = 0; s < kSites; ++s)
    for (int k = 0; k <= kIdx; ++k) {
      const Sum& x = sum[s][k];
      if (x.draws <= 0 && x.failDraws <= 0) continue;
      double meas = 0;
      if (g_lastFrames) {
        if (k < kIdx)
          meas = g_last[s][k].vs / static_cast<double>(g_lastFrames);
        else
          for (int q = 0; q < kIdx; ++q) meas += g_last[s][q].vs / static_cast<double>(g_lastFrames);
      }
      char where[40];
      if (k < kIdx)
        snprintf(where, sizeof(where), "%s %d", kSiteName[s], k);
      else
        snprintf(where, sizeof(where), "%s all", s ? "gbuffer" : "shadow");
      Log("    %-24s %7.0f %7.3f %7.3f %7.3f %7.3f %5.1f%% %7.3f %7.3f %6.2f %7.3f %6.2f %7.0f", where, x.draws,
          x.tris / 1e6, x.uniq / 1e6, x.nvO / 1e6, x.nvR / 1e6, x.nvO > 0 ? 100.0 * (1 - x.nvR / x.nvO) : 0.0,
          x.fO / 1e6, x.fR / 1e6, x.tris > 0 ? x.nvO / x.tris : 0.0, meas / 1e6, x.nvO > 0 ? meas / x.nvO : 0.0,
          x.failDraws);
    }
  for (int s = 0; s < kSites; ++s) {
    const Sum& x = sum[s][kIdx];
    if (x.drawsTex > 0)
      Log("    %s textured (alpha-tested) casters: %.0f draws, %.3f M tris, NV %.3f -> %.3f M", s ? "gbuffer" : "shadow",
          x.drawsTex, x.trisTex / 1e6, x.nvOTex / 1e6, x.nvRTex / 1e6);
  }
  for (int k = 0; k <= kIdx; ++k) {
    const Sum& x = sum[0][k];
    if (x.mTris <= 0) continue;
    Log("    shadow merge %s: covers %.3f M tris (NV %.3f M); unique verts as drawn %.3f -> merged equal bytes %.3f, "
        "equal first 12 bytes %.3f; NV after merge + reorder: exact %.3f M (%.1f%%), position %.3f M (%.1f%%)",
        k < kIdx ? (std::string("cascade slot ") + std::to_string(k)).c_str() : "all", x.mTris / 1e6, x.mNvO / 1e6,
        x.mUniq / 1e6, x.uExact / 1e6, x.uPos / 1e6, x.nvExact / 1e6,
        x.mNvO > 0 ? 100.0 * (1 - x.nvExact / x.mNvO) : 0.0, x.nvPos / 1e6, x.mNvO > 0 ? 100.0 * (1 - x.nvPos / x.mNvO) : 0.0);
  }
  // Distribution: the heaviest meshes by NV vertices per frame.
  std::vector<std::pair<double, size_t>> top;
  for (size_t i = 0; i < g_meshes.size(); ++i) {
    if (!res[i].ok) continue;
    double w = 0;
    for (int s = 0; s < kSites; ++s)
      for (int k = 0; k < kIdx; ++k) w += g_meshes[i].w[s][k];
    top.emplace_back(w / cf * res[i].nvOrig, i);
  }
  std::sort(top.begin(), top.end(), std::greater<>());
  Log("  heaviest meshes (NV vertices per frame): tris, draws/frame (shadow / gbuffer), NV/tri as drawn -> reordered");
  for (size_t q = 0; q < top.size() && q < 15; ++q) {
    const MeshRec& m = g_meshes[top[q].second];
    const MeshResult& r = res[top[q].second];
    double ws = 0, wg = 0;
    for (int k = 0; k < kIdx; ++k) ws += m.w[0][k] / cf, wg += m.w[1][k] / cf;
    Log("    %2zu: %.3f M verts/frame, %llu tris, %.1f / %.1f draws, %.2f -> %.2f, unique/tri %.2f", q + 1,
        top[q].first / 1e6, static_cast<unsigned long long>(r.tris), ws, wg,
        r.tris ? static_cast<double>(r.nvOrig) / r.tris : 0.0, r.tris ? static_cast<double>(r.nvOpt) / r.tris : 0.0,
        r.tris ? static_cast<double>(r.uniq) / r.tris : 0.0);
  }
  g_meshIdx.clear();
  std::vector<MeshRec>().swap(g_meshes);
}

}  // namespace gsplit
