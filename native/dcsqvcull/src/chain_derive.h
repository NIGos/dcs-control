// Focus culling chain = periphery chain filtered by the focus planes? A live census ([Suite] ChainDeriveCensus,
// measure only, default 0 = nothing installed). Audit A2/B4, R38 §3.2 / §7 rank 3.
//
// Claim under test [R38 §3.2, from the Parse disassembly]: the IM2 Parse kernel picks the LOD from the camera
// distance only and culls with the six planes TerrainContext rows 28..33; every other register it reads is the same
// for a periphery view and the focus view of the same eye. If so, every instance of the focus chain's output is in
// the periphery chain's output with the same LOD and the same record, and the focus list could be derived by
// filtering the periphery list (saving the focus chains).
//
// What runs (render thread, DCS's immediate context; nothing DCS sees is changed):
// - Buffers: chain_fast.h's CSSetShader hook calls OnSet before each bind. When the shader being bound or the one
//   bound before is the identified instancemanager2.calccounters (stock or variant), the bound CS views are matched
//   against calccounters' signature (t0 sums typed, u0 mainBuffer structured 64 B, u1 ranks typed, u2 counts typed)
//   and, once matched, mainBuffer / counts / sums are kept (AddRef). The census needs [D3D] ChainFast != 0 (its
//   hook) and the calccounters pair identified.
// - Views: DCS renders periphery L, periphery R, focus L, focus R, each starting with a top-level LightsBuffersPass
//   that holds its culling chain (periph_lod.h, R34 §2). A HeightMapPass / CascadeShadowPass start resets the order.
// - Capture (one frame every kCaptureMs, never while the last one is being read or analysed): at the end of each of
//   the four LightsBuffersPasses that ran calccounters, CopySubresourceRegion of sums (first bytes), counts (whole),
//   mainBuffer (first [Suite] ChainDeriveCensusMB MB) and the TerrainContext (CS b11 as bound at calccounters) into
//   staging buffers of ours. Read back with D3D11_MAP_FLAG_DO_NOT_WAIT, one view per frame (at the first
//   LightsBuffersPass start), copying only the sums[0] records in use; a capture not readable within kMaxWait
//   frames is dropped (counted). The render thread never waits for the GPU.
// - GPU time (every frame while the key is on): D3D11 timestamp + disjoint queries around each of the four
//   LightsBuffersPasses and their first kNest nested passes (per name), read back without waiting (ring of kRing
//   frames; a slot not ready when it comes round is dropped and counted).
// - Analysis (own thread): pairs (0, 2) and (1, 3) after a camera check (same projection rows 18-19 and camera
//   position, row 44.x == 0, periphery |P00| < focus |P00|). Records match by words 2..10 (pivot, X and Z axes);
//   duplicates are matched one to one. Kinds, per pair:
//     matched     focus record found in the periphery list;
//     lod         matched, but bucket (word 0 & 0xffffff) or LOD (word 1 & 0xffffff) differs;
//     payload     matched, same bucket and LOD, another word differs (word mask and per-word counts logged);
//     extra_out   focus record absent from the periphery list, its pivot outside a periphery plane (the conservative
//                 sphere test's corner/behind-the-eye case: no G-buffer pixel, but the flat-shadow and decal passes
//                 read the same lists, so a derived list would differ from stock there);
//     extra_in    absent, pivot inside every periphery plane (unexplained: would be lost by a derivation);
//     dropped_out periphery record absent from the focus list, pivot outside a focus plane (expected);
//     dropped_in  absent, pivot inside every focus plane (the boundsphere centre is not the pivot, so not
//                 necessarily wrong; expected near 0);
//     counts_bad  buckets whose counts[] value differs from the records found (identification sanity);
//     order       buckets whose focus records are in the periphery's relative order or not (append order comes
//                 from atomics: informative only, the stock chain's own order varies run to run, R34 §2).
//   The plane sign convention is taken from the data (the sign under which most of the view's own pivots are
//   inside). Pivot tests are diagnostics; the derivability verdict rests on exact record matching.
// - Log (every [Suite] ChainDeriveCensus seconds): one "chain derive census:" line (counts per pair, GPU ms per
//   view, verdict) and one "chain derive census gpu:" line with the nested passes.
// Included once from main.cpp after chain_fast.h and periph_lod.h.
#pragma once

#include <algorithm>

namespace chainderive {

// ---------------------------------------------------------------------------
// Pure part (unit-tested)
// ---------------------------------------------------------------------------
constexpr int kWords = 16;               // 64-byte OutputInstanceData
constexpr int kKeyFirst = 2, kKeyLast = 10;  // pivot (2..4), X axis (5..7), Z axis (8..10)
constexpr int kRowP = 16, kRowPlanes = 28, kRowCam = 38, kRowFlags = 44, kRowsNeeded = 45;

struct ViewData {
  std::vector<uint32_t> rec;     // n * 16 words
  uint32_t n = 0;                // records (sums[0], clamped to what was captured)
  uint32_t sums0 = 0;            // sums[0] as read
  bool trunc = false;            // sums[0] records did not fit the captured range
  std::vector<uint32_t> counts;  // counts[bucket]
  std::vector<float> cb;         // TerrainContext floats (row r at 4 r)
};

struct CensusStats {
  uint64_t pairs = 0, camBad = 0, trunc = 0, signBad = 0;
  uint64_t periph = 0, focus = 0, matched = 0, lod = 0, payload = 0, extraOut = 0, extraIn = 0;
  uint64_t droppedOut = 0, droppedIn = 0, invalid = 0, countsBad = 0, orderSame = 0, orderDiff = 0;
  uint64_t wordDiff[kWords] = {};
  uint32_t planesEqAnd = 0x3f, planesEqOr = 0;  // planes bitwise equal between periphery and focus (bit = row - 28)
  void Add(const CensusStats& o) {
    pairs += o.pairs, camBad += o.camBad, trunc += o.trunc, signBad += o.signBad;
    periph += o.periph, focus += o.focus, matched += o.matched, lod += o.lod, payload += o.payload;
    extraOut += o.extraOut, extraIn += o.extraIn, droppedOut += o.droppedOut, droppedIn += o.droppedIn;
    invalid += o.invalid, countsBad += o.countsBad, orderSame += o.orderSame, orderDiff += o.orderDiff;
    for (int i = 0; i < kWords; ++i) wordDiff[i] += o.wordDiff[i];
    if (o.pairs && !o.camBad) {
      planesEqAnd &= o.planesEqAnd;
      planesEqOr |= o.planesEqOr;
    }
  }
  uint32_t WordMask() const {
    uint32_t m = 0;
    for (int i = 0; i < kWords; ++i)
      if (wordDiff[i]) m |= 1u << i;
    return m;
  }
  // The focus list is the periphery list filtered: every focus record found with the same LOD and payload.
  bool Exact() const { return pairs > camBad + trunc && !camBad && !extraIn && !extraOut && !lod && !payload; }
};

inline const float* Row(const ViewData& v, int r) { return v.cb.data() + 4 * r; }
inline uint32_t Bits(float f) {
  uint32_t u;
  memcpy(&u, &f, 4);
  return u;
}
inline bool RowEq(const ViewData& a, const ViewData& b, int r) {
  return memcmp(Row(a, r), Row(b, r), 16) == 0;
}

// Periphery p and focus f of one eye: same camera, a narrower focus projection. *planesEq: bit i = plane row 28+i
// bitwise equal.
inline bool CameraPair(const ViewData& p, const ViewData& f, uint32_t* planesEq) {
  *planesEq = 0;
  if (p.cb.size() < 4 * kRowsNeeded || f.cb.size() < 4 * kRowsNeeded) return false;
  for (int i = 0; i < 6; ++i)
    if (RowEq(p, f, kRowPlanes + i)) *planesEq |= 1u << i;
  const bool cam = memcmp(Row(p, kRowCam), Row(f, kRowCam), 12) == 0;
  const bool zw = RowEq(p, f, kRowP + 2) && RowEq(p, f, kRowP + 3);
  const bool flags = Bits(Row(p, kRowFlags)[0]) == 0 && Bits(Row(f, kRowFlags)[0]) == 0;
  const float pp = std::fabs(Row(p, kRowP)[0]), fp = std::fabs(Row(f, kRowP)[0]);
  return cam && zw && flags && pp > 0 && fp > pp;
}

inline void Pivot(const uint32_t* r, float out[3]) { memcpy(out, r + 2, 12); }

// Signed distances of a point to the six planes (normalised), with the inside sign s.
inline float MinDist(const ViewData& v, int s, const float p[3]) {
  float m = 3.4e38f;
  for (int i = 0; i < 6; ++i) {
    const float* q = Row(v, kRowPlanes + i);
    const float len = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2]);
    const float d = len > 0 ? s * (q[0] * p[0] + q[1] * p[1] + q[2] * p[2] + q[3]) / len : 0.f;
    if (!(d >= m)) m = d;  // NaN counts as outside
  }
  return m;
}

// The inside sign of v's planes: the one under which more of v's own pivots are inside all six. *frac: that share.
inline int PlaneSign(const ViewData& v, double* frac) {
  uint64_t pos = 0, neg = 0;
  for (uint32_t i = 0; i < v.n; ++i) {
    float p[3];
    Pivot(&v.rec[16ull * i], p);
    if (MinDist(v, 1, p) >= 0) ++pos;
    if (MinDist(v, -1, p) >= 0) ++neg;
  }
  *frac = v.n ? static_cast<double>((std::max)(pos, neg)) / v.n : 0.0;
  return neg > pos ? -1 : 1;
}

inline bool KeyLess(const uint32_t* a, const uint32_t* b) {
  for (int k = kKeyFirst; k <= kKeyLast; ++k)
    if (a[k] != b[k]) return a[k] < b[k];
  return false;
}

// counts[] against the records: buckets whose count differs. Records with a bucket past counts are *invalid.
inline uint64_t CountsBad(const ViewData& v, uint64_t* invalid) {
  if (v.counts.empty()) return 0;
  std::vector<uint32_t> h(v.counts.size(), 0);
  size_t top = 0;  // buckets past the highest one in use are not compared (the counts buffer may be longer)
  for (uint32_t i = 0; i < v.n; ++i) {
    const uint32_t b = v.rec[16ull * i] & 0xffffffu;
    if (b < h.size()) {
      ++h[b];
      top = (std::max)(top, static_cast<size_t>(b) + 1);
    } else {
      ++*invalid;
    }
  }
  uint64_t bad = 0;
  for (size_t b = 0; b < top; ++b)
    if (h[b] != v.counts[b]) ++bad;
  return bad;
}

inline CensusStats ComparePair(const ViewData& P, const ViewData& F) {
  CensusStats s;
  s.pairs = 1;
  uint32_t eq = 0;
  if (!CameraPair(P, F, &eq)) {
    s.camBad = 1;
    return s;
  }
  s.planesEqAnd = s.planesEqOr = eq;
  if (P.trunc || F.trunc) {
    s.trunc = 1;
    return s;
  }
  double fp = 0, ff = 0;
  const int sp = PlaneSign(P, &fp), sf = PlaneSign(F, &ff);
  if ((P.n && fp < 0.5) || (F.n && ff < 0.5)) s.signBad = 1;
  s.periph = P.n;
  s.focus = F.n;
  s.countsBad = CountsBad(P, &s.invalid) + CountsBad(F, &s.invalid);
  std::vector<uint32_t> ord(P.n);
  for (uint32_t i = 0; i < P.n; ++i) ord[i] = i;
  const uint32_t* pr = P.rec.data();
  std::sort(ord.begin(), ord.end(), [&](uint32_t a, uint32_t b) {
    return KeyLess(pr + 16ull * a, pr + 16ull * b) || (!KeyLess(pr + 16ull * b, pr + 16ull * a) && a < b);
  });
  std::vector<uint8_t> used(P.n, 0);
  std::unordered_map<uint32_t, std::pair<uint32_t, bool>> lastByBucket;  // bucket -> (last periphery index + 1, inverted)
  for (uint32_t j = 0; j < F.n; ++j) {
    const uint32_t* fr = F.rec.data() + 16ull * j;
    auto lo = std::lower_bound(ord.begin(), ord.end(), j, [&](uint32_t a, uint32_t) { return KeyLess(pr + 16ull * a, fr); });
    uint32_t hit = UINT32_MAX;
    for (auto it = lo; it != ord.end() && !KeyLess(fr, pr + 16ull * *it); ++it)
      if (!used[*it]) {
        hit = *it;
        break;
      }
    if (hit == UINT32_MAX) {
      float p[3];
      Pivot(fr, p);
      if (MinDist(P, sp, p) < 0)
        ++s.extraOut;
      else
        ++s.extraIn;
      continue;
    }
    used[hit] = 1;
    ++s.matched;
    const uint32_t* q = pr + 16ull * hit;
    if ((q[0] & 0xffffffu) != (fr[0] & 0xffffffu) || (q[1] & 0xffffffu) != (fr[1] & 0xffffffu)) {
      ++s.lod;
    } else {
      bool any = false;
      for (int k = 0; k < kWords; ++k) {
        if (k >= kKeyFirst && k <= kKeyLast) continue;
        if (q[k] != fr[k]) {
          ++s.wordDiff[k];
          any = true;
        }
      }
      if (any) ++s.payload;
    }
    auto& lb = lastByBucket[fr[0] & 0xffffffu];
    if (hit + 1 < lb.first) lb.second = true;
    lb.first = hit + 1;
  }
  for (const auto& kv : lastByBucket)
    if (kv.second.second)
      ++s.orderDiff;
    else
      ++s.orderSame;
  for (uint32_t i = 0; i < P.n; ++i) {
    if (used[i]) continue;
    float p[3];
    Pivot(pr + 16ull * i, p);
    if (MinDist(F, sf, p) < 0)
      ++s.droppedOut;
    else
      ++s.droppedIn;
  }
  return s;
}

struct GpuView {
  double us = 0;
  uint64_t n = 0;
  std::vector<std::pair<std::string, std::pair<double, uint64_t>>> nest;  // name -> (us sum, samples)
};

// The census line (greppable prefix "chain derive census:").
inline std::string FormatLine(int sec, uint64_t captures, uint64_t timeouts, const CensusStats& s, const GpuView g[4],
                              uint64_t gpuDropped, const char* state) {
  char b[1400];
  auto ms = [&](int v) { return g[v].n ? g[v].us / g[v].n / 1000.0 : -1.0; };
  const uint64_t used = s.pairs - s.camBad - s.trunc;
  const double perPair = used ? 1.0 / used : 0.0;
  const char* verdict = !captures ? "NO DATA" : !used ? "NO PAIRS" : s.Exact() ? "EXACT (focus = filtered periphery)" : "NOT EXACT";
  snprintf(b, sizeof(b),
           "chain derive census: %d s, %llu captures (%llu timed out or incomplete),%llu pairs (%llu camera mismatch, %llu "
           "truncated, %llu plane-sign unclear); per pair: periphery %.0f, focus %.0f records; matched %llu, extra_in "
           "%llu, extra_out %llu, lod %llu, payload %llu (words 0x%04x), dropped_out %llu, dropped_in %llu, "
           "counts_bad %llu, invalid %llu, order same %llu / differs %llu buckets, planes equal all 0x%02x any 0x%02x; "
           "GPU LightsBuffersPass ms periphery L %.3f R %.3f, focus L %.3f R %.3f (%llu samples dropped); %s; %s",
           sec, (unsigned long long)captures, (unsigned long long)timeouts, (unsigned long long)s.pairs,
           (unsigned long long)s.camBad, (unsigned long long)s.trunc, (unsigned long long)s.signBad, s.periph * perPair,
           s.focus * perPair, (unsigned long long)s.matched, (unsigned long long)s.extraIn,
           (unsigned long long)s.extraOut, (unsigned long long)s.lod, (unsigned long long)s.payload, s.WordMask(),
           (unsigned long long)s.droppedOut, (unsigned long long)s.droppedIn, (unsigned long long)s.countsBad,
           (unsigned long long)s.invalid, (unsigned long long)s.orderSame, (unsigned long long)s.orderDiff,
           used ? s.planesEqAnd : 0u, s.planesEqOr, ms(0), ms(1), ms(2), ms(3), (unsigned long long)gpuDropped,
           verdict, state);
  return b;
}

inline std::string FormatGpuLine(const GpuView g[4]) {
  std::string out = "chain derive census gpu: nested passes, ms per frame:";
  const char* names[4] = {"periphery L", "periphery R", "focus L", "focus R"};
  for (int v = 0; v < 4; ++v) {
    out += " | ";
    out += names[v];
    for (const auto& e : g[v].nest) {
      char b[96];
      snprintf(b, sizeof(b), " %s %.3f", e.first.c_str(), e.second.second ? e.second.first / e.second.second / 1000.0 : 0.0);
      out += b;
    }
  }
  return out;
}

// ---------------------------------------------------------------------------
// Live part
// ---------------------------------------------------------------------------
constexpr int kRing = 8, kNest = 6, kMaxWait = 120;
constexpr ULONGLONG kCaptureMs = 500;

std::atomic<int> g_sec{0};  // [Suite] ChainDeriveCensus (seconds per line; 0 = off)
std::atomic<int> g_mb{24};  // [Suite] ChainDeriveCensusMB (mainBuffer bytes captured per view)
std::atomic<int> g_state{0};  // 0 off, 1 callbacks in
bool g_waitLogged = false;

// Render-thread state.
ID3D11Buffer* g_main = nullptr;  // learnt (AddRef)
ID3D11Buffer* g_counts = nullptr;
ID3D11Buffer* g_sums = nullptr;
std::atomic<bool> g_learnt{false};
int g_lbOrd = 0;         // LightsBuffersPass ordinal in the frame
int g_inLb = -1;         // view of the running LightsBuffersPass, -1 outside
void* g_lbPass = nullptr;
bool g_ccSeen = false;   // calccounters seen in the running LightsBuffersPass
ID3D11Buffer* g_passCb = nullptr;  // CS b11 at calccounters in the running pass (AddRef)

struct Stage {
  ID3D11Buffer* b = nullptr;
  UINT bytes = 0;
  void Release() {
    if (b) b->Release();
    b = nullptr;
    bytes = 0;
  }
};
Stage g_stMain[4], g_stCounts[4], g_stSums[4], g_stCb[4];
int g_capState = 0;  // 0 idle, 1 copying this frame, 2 reading back
bool g_capOk[4] = {};
int g_capRead = 0, g_capWait = 0;
ULONGLONG g_nextCap = 0;
ViewData g_capOut[4];

// GPU timing.
struct TSlot {
  ID3D11Query* dj = nullptr;
  ID3D11Query* ts[2 + 2 * kNest] = {};
  char names[kNest][40] = {};
  int nNest = 0;
  bool pending = false;
};
TSlot g_ts[kRing][4];
int g_ring = 0;
void* g_nestPass = nullptr;
int g_nestIdx = -1;

// Shared with the analysis thread.
std::mutex g_mu;
GpuView g_gpu[4];
uint64_t g_gpuDropped = 0, g_captures = 0, g_timeouts = 0;
CensusStats g_stats;
std::atomic<bool> g_jobReady{false};
ViewData g_job[4];
HANDLE g_thread = nullptr, g_wake = nullptr;
std::atomic<bool> g_stop{false};
std::string g_note;  // last state note (log)

bool IsCalcCounters(ID3D11ComputeShader* s) {
  if (!s) return false;
  const int n = chainfast::g_n.load(std::memory_order_acquire);
  for (int i = 0; i < n; ++i) {
    const chainfast::Entry& e = chainfast::g_e[i];
    if (e.kind == chainfast::kCalcCounters && (e.stock == s || e.variant == s)) return true;
  }
  return false;
}

ID3D11Buffer* BufOf(ID3D11View* v, D3D11_BUFFER_DESC* bd) {
  if (!v) return nullptr;
  ID3D11Resource* r = nullptr;
  v->GetResource(&r);
  if (!r) return nullptr;
  D3D11_RESOURCE_DIMENSION dim{};
  r->GetType(&dim);
  if (dim != D3D11_RESOURCE_DIMENSION_BUFFER) {
    r->Release();
    return nullptr;
  }
  ID3D11Buffer* b = static_cast<ID3D11Buffer*>(r);
  b->GetDesc(bd);
  return b;  // AddRef'd by GetResource
}

// calccounters' bindings: t0 sums (typed), u0 mainBuffer (structured 64 B), u1 ranks (typed), u2 counts (typed).
void TryLearn(ID3D11DeviceContext* c) {
  ID3D11UnorderedAccessView* u[3] = {};
  ID3D11ShaderResourceView* t[1] = {};
  c->CSGetUnorderedAccessViews(0, 3, u);
  c->CSGetShaderResources(0, 1, t);
  D3D11_BUFFER_DESC d0{}, d1{}, d2{}, dt{};
  ID3D11Buffer* b0 = BufOf(u[0], &d0);
  ID3D11Buffer* b1 = BufOf(u[1], &d1);
  ID3D11Buffer* b2 = BufOf(u[2], &d2);
  ID3D11Buffer* bt = BufOf(t[0], &dt);
  const bool ok = b0 && b1 && b2 && bt && (d0.MiscFlags & D3D11_RESOURCE_MISC_BUFFER_STRUCTURED) &&
                  d0.StructureByteStride == 64 && d0.ByteWidth >= 64 * 4096 &&
                  !(d2.MiscFlags & D3D11_RESOURCE_MISC_BUFFER_STRUCTURED) && d2.ByteWidth >= 4 * 1024 &&
                  !(dt.MiscFlags & D3D11_RESOURCE_MISC_BUFFER_STRUCTURED) && dt.ByteWidth >= 16 && b0 != b2 &&
                  b2 != bt && b0 != bt;
  if (ok) {
    g_main = b0;
    g_counts = b2;
    g_sums = bt;
    b0 = b2 = bt = nullptr;
    g_learnt = true;
    Log("chain derive census: buffers identified at calccounters: mainBuffer %u B, counts %u B, sums %u B", d0.ByteWidth,
        d2.ByteWidth, dt.ByteWidth);
  }
  for (ID3D11Buffer* b : {b0, b1, b2, bt})
    if (b) b->Release();
  for (auto* v : u)
    if (v) v->Release();
  if (t[0]) t[0]->Release();
}

void OnSet(ID3D11DeviceContext* c, ID3D11ComputeShader* s) {
  if (GetCurrentThreadId() != ptiming::g_topTid.load(std::memory_order_relaxed)) return;
  if (!IsCalcCounters(s) && !IsCalcCounters(chainfast::g_curCs)) return;
  if (!g_learnt.load(std::memory_order_relaxed)) TryLearn(c);
  if (g_inLb >= 0) {
    g_ccSeen = true;
    if (!g_passCb) c->CSGetConstantBuffers(11, 1, &g_passCb);
  }
}

bool EnsureStage(Stage& st, ID3D11Buffer* like, UINT bytes) {
  if (st.b && st.bytes == bytes) return true;
  st.Release();
  D3D11_BUFFER_DESC sd{};
  like->GetDesc(&sd);
  D3D11_BUFFER_DESC bd{};
  bd.ByteWidth = bytes;
  bd.Usage = D3D11_USAGE_STAGING;
  bd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  if (sd.MiscFlags & D3D11_RESOURCE_MISC_BUFFER_STRUCTURED) {
    bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    bd.StructureByteStride = sd.StructureByteStride;
  }
  if (FAILED(chainfast::g_dcsDev->CreateBuffer(&bd, nullptr, &st.b))) {
    st.b = nullptr;
    return false;
  }
  st.bytes = bytes;
  return true;
}

void CopyHead(ID3D11DeviceContext* c, Stage& st, ID3D11Buffer* src) {
  D3D11_BOX box{0, 0, 0, st.bytes, 1, 1};
  c->CopySubresourceRegion(st.b, 0, 0, 0, 0, src, 0, &box);
}

// End of view v's LightsBuffersPass on a capture frame.
void IssueCopies(ID3D11DeviceContext* c, int v) {
  g_capOk[v] = false;
  if (!g_ccSeen || !g_learnt.load(std::memory_order_relaxed)) return;
  ID3D11Buffer* cb = g_passCb;
  if (cb)
    cb->AddRef();
  else
    c->CSGetConstantBuffers(11, 1, &cb);
  if (!cb) return;
  D3D11_BUFFER_DESC dm{}, dc{}, ds{}, dcb{};
  g_main->GetDesc(&dm);
  g_counts->GetDesc(&dc);
  g_sums->GetDesc(&ds);
  cb->GetDesc(&dcb);
  const UINT cap = static_cast<UINT>((std::max)(1, (std::min)(256, g_mb.load()))) << 20;
  const UINT mainBytes = (std::min)(dm.ByteWidth, cap) / 64 * 64;
  const bool ok = dcb.ByteWidth >= 16 * kRowsNeeded && EnsureStage(g_stMain[v], g_main, mainBytes) &&
                  EnsureStage(g_stCounts[v], g_counts, dc.ByteWidth) &&
                  EnsureStage(g_stSums[v], g_sums, (std::min)(ds.ByteWidth, 64u)) &&
                  EnsureStage(g_stCb[v], cb, dcb.ByteWidth);
  if (ok) {
    CopyHead(c, g_stMain[v], g_main);
    CopyHead(c, g_stCounts[v], g_counts);
    CopyHead(c, g_stSums[v], g_sums);
    CopyHead(c, g_stCb[v], cb);
    g_capOk[v] = true;
  }
  cb->Release();
}

// One view per call; true when the whole capture is read (or dropped).
bool ReadOne(ID3D11DeviceContext* c) {
  while (g_capRead < 4 && !g_capOk[g_capRead]) ++g_capRead;
  if (g_capRead >= 4) return true;
  const int v = g_capRead;
  Stage* st[4] = {&g_stSums[v], &g_stCounts[v], &g_stCb[v], &g_stMain[v]};
  D3D11_MAPPED_SUBRESOURCE m[4] = {};
  int mapped = 0;
  for (; mapped < 4; ++mapped)
    if (c->Map(st[mapped]->b, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m[mapped]) != S_OK) break;
  if (mapped == 4) {
    ViewData& o = g_capOut[v];
    o.sums0 = *static_cast<const uint32_t*>(m[0].pData);
    const uint32_t cap = g_stMain[v].bytes / 64;
    o.n = (std::min)(o.sums0, cap);
    o.trunc = o.sums0 > cap;
    o.counts.assign(static_cast<const uint32_t*>(m[1].pData), static_cast<const uint32_t*>(m[1].pData) + g_stCounts[v].bytes / 4);
    o.cb.assign(static_cast<const float*>(m[2].pData), static_cast<const float*>(m[2].pData) + g_stCb[v].bytes / 4);
    o.rec.assign(static_cast<const uint32_t*>(m[3].pData), static_cast<const uint32_t*>(m[3].pData) + 16ull * o.n);
    ++g_capRead;
  }
  for (int i = 0; i < mapped; ++i) c->Unmap(st[i]->b, 0);
  return g_capRead >= 4;
}

void PollTiming(ID3D11DeviceContext* c, bool all) {
  for (int f = 0; f < kRing; ++f)
    for (int v = 0; v < 4; ++v) {
      TSlot& t = g_ts[f][v];
      if (!t.pending || (!all && f == g_ring)) continue;
      D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj{};
      if (c->GetData(t.dj, &dj, sizeof(dj), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) continue;
      uint64_t ts[2 + 2 * kNest] = {};
      bool ready = true;
      for (int i = 0; i < 2 + 2 * t.nNest && ready; ++i)
        ready = c->GetData(t.ts[i], &ts[i], sizeof(uint64_t), D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK;
      if (!ready) continue;
      t.pending = false;
      if (dj.Disjoint || !dj.Frequency) continue;
      const double k = 1e6 / static_cast<double>(dj.Frequency);
      std::lock_guard<std::mutex> lock(g_mu);
      GpuView& g = g_gpu[v];
      g.us += (ts[1] - ts[0]) * k;
      ++g.n;
      for (int i = 0; i < t.nNest; ++i) {
        const double us = (ts[3 + 2 * i] - ts[2 + 2 * i]) * k;
        auto it = std::find_if(g.nest.begin(), g.nest.end(), [&](const auto& e) { return e.first == t.names[i]; });
        if (it == g.nest.end()) {
          if (g.nest.size() >= 12) continue;
          g.nest.push_back({t.names[i], {0.0, 0}});
          it = g.nest.end() - 1;
        }
        it->second.first += us;
        ++it->second.second;
      }
    }
}

bool MakeQuery(D3D11_QUERY q, ID3D11Query** out) {
  if (*out) return true;
  D3D11_QUERY_DESC d{q, 0};
  return SUCCEEDED(chainfast::g_dcsDev->CreateQuery(&d, out));
}

void PassName(void* pass, char out[40]) {
  out[0] = 0;
  __try {
    const uint8_t* b = static_cast<const uint8_t*>(pass);
    const uint64_t n = *reinterpret_cast<const uint64_t*>(b + 8);
    if (n == 0 || n >= 40) return;
    memcpy(out, b + 0x10, static_cast<size_t>(n));
    out[n] = 0;
    for (uint64_t i = 0; i < n; ++i)
      if (out[i] < 0x20 || out[i] > 0x7e) out[i] = '?';
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    out[0] = 0;
  }
}

void OnBoundary(void* pass, bool begin, int depth) {
  if (GetCurrentThreadId() != ptiming::g_topTid.load(std::memory_order_relaxed)) return;
  ID3D11DeviceContext* c = chainfast::g_ctx;
  if (!c || !chainfast::g_dcsDev) return;
  if (depth == 1 && g_inLb >= 0) {  // nested pass inside a LightsBuffersPass
    TSlot& t = g_ts[g_ring][g_inLb];
    if (begin && !g_nestPass && t.nNest < kNest && MakeQuery(D3D11_QUERY_TIMESTAMP, &t.ts[2 + 2 * t.nNest]) &&
        MakeQuery(D3D11_QUERY_TIMESTAMP, &t.ts[3 + 2 * t.nNest])) {
      PassName(pass, t.names[t.nNest]);
      if (!t.names[t.nNest][0]) strcpy_s(t.names[t.nNest], "?");
      c->End(t.ts[2 + 2 * t.nNest]);
      g_nestPass = pass;
      g_nestIdx = t.nNest;
    } else if (!begin && pass == g_nestPass) {
      c->End(t.ts[3 + 2 * g_nestIdx]);
      t.nNest = g_nestIdx + 1;
      g_nestPass = nullptr;
    }
    return;
  }
  if (depth != 0) return;
  const int k = plod::KindOf(pass);
  if (begin && (k == plod::kHeight || k == plod::kCascade)) {
    g_lbOrd = 0;
    return;
  }
  if (k != plod::kLights) return;
  if (begin) {
    const int v = g_lbOrd++;
    if (v >= 4) return;
    if (v == 0) {
      g_ring = (g_ring + 1) % kRing;
      PollTiming(c, true);
      if (g_capState == 1) g_capState = 2;  // a capture frame that ended before the fourth view
      if (g_capState == 2) {
        if (ReadOne(c)) {
          int ok = 0;
          for (int i = 0; i < 4; ++i) ok += g_capOk[i];
          if (ok == 4 && !g_jobReady.load()) {
            for (int i = 0; i < 4; ++i) std::swap(g_job[i], g_capOut[i]);
            g_jobReady = true;
            SetEvent(g_wake);
          } else {  // a view without calccounters or without a TerrainContext at b11
            std::lock_guard<std::mutex> lock(g_mu);
            ++g_timeouts;
          }
          g_capState = 0;
          g_nextCap = GetTickCount64() + kCaptureMs;
        } else if (++g_capWait > kMaxWait) {
          std::lock_guard<std::mutex> lock(g_mu);
          ++g_timeouts;
          g_capState = 0;
          g_nextCap = GetTickCount64() + kCaptureMs;
        }
      } else if (g_capState == 0 && g_learnt.load(std::memory_order_relaxed) && !g_jobReady.load() &&
                 GetTickCount64() >= g_nextCap) {
        g_capState = 1;
        g_capRead = 0;
        g_capWait = 0;
        for (bool& b : g_capOk) b = false;
      }
    }
    TSlot& t = g_ts[g_ring][v];
    if (t.pending) {
      std::lock_guard<std::mutex> lock(g_mu);
      ++g_gpuDropped;
    }
    t.pending = false;
    t.nNest = 0;
    g_nestPass = nullptr;
    if (MakeQuery(D3D11_QUERY_TIMESTAMP_DISJOINT, &t.dj) && MakeQuery(D3D11_QUERY_TIMESTAMP, &t.ts[0]) &&
        MakeQuery(D3D11_QUERY_TIMESTAMP, &t.ts[1])) {
      c->Begin(t.dj);
      c->End(t.ts[0]);
      g_inLb = v;
      g_lbPass = pass;
    }
    g_ccSeen = false;
    if (g_passCb) g_passCb->Release();
    g_passCb = nullptr;
    return;
  }
  if (pass != g_lbPass || g_inLb < 0) return;
  const int v = g_inLb;
  TSlot& t = g_ts[g_ring][v];
  if (g_nestPass) {  // a nested pass that never ended (should not happen): drop it
    g_nestPass = nullptr;
  }
  c->End(t.ts[1]);
  c->End(t.dj);
  t.pending = true;
  if (g_capState == 1) {
    IssueCopies(c, v);
    if (v == 3) g_capState = 2;
  }
  if (g_passCb) g_passCb->Release();
  g_passCb = nullptr;
  g_inLb = -1;
  g_lbPass = nullptr;
}

DWORD WINAPI AnalysisThread(void*) {
  ULONGLONG last = GetTickCount64();
  while (!g_stop.load()) {
    WaitForSingleObject(g_wake, 1000);
    if (g_stop.load()) break;
    if (g_jobReady.load()) {
      CensusStats s = ComparePair(g_job[0], g_job[2]);
      s.Add(ComparePair(g_job[1], g_job[3]));
      {
        std::lock_guard<std::mutex> lock(g_mu);
        g_stats.Add(s);
        ++g_captures;
      }
      g_jobReady = false;
    }
    const int sec = g_sec.load();
    const ULONGLONG now = GetTickCount64();
    if (sec > 0 && now - last >= static_cast<ULONGLONG>(sec) * 1000) {
      std::string line, gl;
      {
        std::lock_guard<std::mutex> lock(g_mu);
        line = FormatLine(static_cast<int>((now - last) / 1000), g_captures, g_timeouts, g_stats, g_gpu, g_gpuDropped,
                          g_learnt.load() ? "buffers identified" : "buffers NOT identified (needs [D3D] ChainFast != 0 and calccounters seen)");
        gl = FormatGpuLine(g_gpu);
        g_stats = CensusStats{};
        g_captures = g_timeouts = g_gpuDropped = 0;
        for (GpuView& g : g_gpu) g = GpuView{};
      }
      Log("%s", line.c_str());
      Log("%s", gl.c_str());
      last = now;
    }
  }
  return 0;
}

void ReleaseGpu() {
  for (int v = 0; v < 4; ++v) {
    g_stMain[v].Release();
    g_stCounts[v].Release();
    g_stSums[v].Release();
    g_stCb[v].Release();
  }
  for (auto& row : g_ts)
    for (TSlot& t : row) {
      if (t.dj) t.dj->Release();
      for (ID3D11Query*& q : t.ts)
        if (q) {
          q->Release();
          q = nullptr;
        }
      t = TSlot{};
    }
  for (ID3D11Buffer** b : {&g_main, &g_counts, &g_sums, &g_passCb})
    if (*b) {
      (*b)->Release();
      *b = nullptr;
    }
  g_learnt = false;
}

void Stop() {
  if (g_state.load() != 1) return;
  void (*me)(void*, bool, int) = &OnBoundary;
  ptiming::g_censusBoundary.compare_exchange_strong(me, nullptr);
  void (*os)(ID3D11DeviceContext*, ID3D11ComputeShader*) = &OnSet;
  chainfast::g_onSetCensus.compare_exchange_strong(os, nullptr);
  Sleep(300);  // the render thread leaves the callbacks (each runs for microseconds)
  g_stop = true;
  if (g_wake) SetEvent(g_wake);
  if (g_thread) {
    WaitForSingleObject(g_thread, 5000);
    CloseHandle(g_thread);
    g_thread = nullptr;
  }
  ReleaseGpu();
  g_capState = 0;
  g_inLb = -1;
  g_lbOrd = 0;
  g_jobReady = false;
  g_state = 0;
  Log("chain derive census: off");
}

// Worker (1 s loop): sec = [Suite] ChainDeriveCensus, mb = [Suite] ChainDeriveCensusMB.
void Tick(int sec, int mb) {
  g_sec = sec;
  g_mb = mb;
  if (sec <= 0) {
    Stop();
    g_waitLogged = false;
    return;
  }
  if (g_state.load() == 1) return;
  if (!chainfast::g_ctx || !chainfast::g_dcsDev || !chainfast::g_setSlot) {
    if (!g_waitLogged) Log("chain derive census: waiting for chain fast's CSSetShader hook ([D3D] ChainFast != 0)");
    g_waitLogged = true;
    return;
  }
  if (!g_wake) g_wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  g_stop = false;
  g_jobReady = false;
  {
    std::lock_guard<std::mutex> lock(g_mu);
    g_stats = CensusStats{};
    g_captures = g_timeouts = g_gpuDropped = 0;
    for (GpuView& g : g_gpu) g = GpuView{};
  }
  g_thread = CreateThread(nullptr, 0, &AnalysisThread, nullptr, 0, nullptr);
  if (!g_thread) return;
  g_nextCap = GetTickCount64() + 2000;
  chainfast::g_onSetCensus = &OnSet;
  ptiming::g_censusBoundary = &OnBoundary;
  g_state = 1;
  Log("chain derive census: on (line every %d s, mainBuffer capture up to %d MB per view, one frame every %llu ms); "
      "measure only",
      sec, mb, static_cast<unsigned long long>(kCaptureMs));
}

void Shutdown() {
  Stop();
  if (g_wake) {
    CloseHandle(g_wake);
    g_wake = nullptr;
  }
}

}  // namespace chainderive
