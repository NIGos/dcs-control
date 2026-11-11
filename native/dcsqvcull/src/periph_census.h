// Periphery instance census (audit 2026-11-10 B6; the census half of R38 rank 1, research_archive\
// R38_periph_inst_filter, restored without the filter). [Suite] PeriphCensus=N (seconds, 0 = off, default): measure
// only. Nothing DCS draws changes: DCS's lists and arguments are only read, our work runs on DCS's immediate context
// with its compute state saved and restored, and one "periph census:" line is logged every N s.
//
// What it counts: in the two periphery views' G-buffer, the forest / building instances (IM2 instancer) whose
// conservative screen box holds no pixel centre ("zero coverage": they draw nothing), and those wholly inside the
// focus view's opaque core (gaze rect or the previous frame's focus projection). Both as a share of the listed
// instances and as a share of the listed triangles.
//
// Triangle weight (B6 flaw: R45 gated in triangles but counted instance shares, because the LOD's gPrimCount is not a
// triangle count in 2.9.30): every instance of bucket b is drawn by the argument entries e with args[e][7] == b
// (fillindirect writes [7] = bucket, [1] = counts[bucket]), so its triangles are sum over those entries of
// IndexCountPerInstance / 3 (KBucketTri, per run). Cross-check in the line: the sum of the instance weights equals
// the argument entries' own IndexCount / 3 x InstanceCount total when the arguments match counts (error 0x10 if not).
// A bucket weight over 4 M triangles is counted 0 and reported.
//
// Mechanism (from R38 / R45, unchanged where kept): identification by two one-shot table arms (the first instancer
// DrawIndexedInstancedIndirect of a periphery G-buffer run: arguments, VS SRVs mainBuffer / indices / offsets /
// counts, VS b11 TerrainContext; the IM2 Parse Dispatch: objects, object LODs). Per periphery run, at its first
// instancer draw on the identified lists and before DCS's draw: snapshot the TerrainContext that draw binds,
// classify every listed instance into a statistics buffer. At that pass's end the TerrainContext is compared with the
// snapshot (0x100 / 0x200: another camera or size, the run is left out); at the same eye's focus run its projection
// gives the true core (0x800: left out of the core share). Statistics are read back without waiting.
// Removed from R38: the commit (partition / argument rewrite / restore), the verify redraw, the timing phase, the
// R53 broadcast and R45 G4 probe callbacks, the [D3D] PeriphInst* keys (retired, still logged if set).
//
// Included once from main.cpp after measure_kit.h (uses ptiming, plod, chainfast, gpt, d3ds, fixdepth, shadowinst,
// sfilt, perc_sfx, ::dcsvr_perception).
#pragma once

namespace pcensus {

// ---------------------------------------------------------------------------
// State (worker writes the switch, render thread reads)
// ---------------------------------------------------------------------------
std::atomic<bool> g_installed{false};
std::atomic<bool> g_fault{false};
std::atomic<bool> g_stopping{false};  // Shutdown asks the render thread for its arms out
// Core-rect constants of R38 (QVFR defaults; were [D3D] PeriphInstCoreMargin / Edge / Shape, retired).
constexpr int kMarginPx = 16;
constexpr float kCoreR = 0.6f;  // 1 - 2 x smoothen_focus_view_edges 0.2
constexpr float kShapeP = 8.0f;
constexpr uint32_t kTriCap = 4000000u;  // a bucket weight above it is implausible: weight 0, counted

// Error bits in stats[0]: 0x1-0x20 at classification, 0x100.. the after-run checks.
constexpr uint32_t kErrTotal = 0x1, kErrIndex = 0x2, kErrBucket = 0x4, kErrSlot = 0x8, kErrArgs = 0x10,
                   kErrView = 0x20, kErrVp = 0x100, kErrDims = 0x200, kErrEye = 0x800;
// A run with one of these is left out of every share; kErrEye only of the core share.
constexpr uint32_t kErrDrop = kErrTotal | kErrIndex | kErrBucket | kErrSlot | kErrArgs | kErrView | kErrVp | kErrDims;
// stats layout (uint32 x 64, per eye, cleared at the run begin)
enum Stat : int {
  kSErr = 0, kSTotal = 1, kSInst = 2, kSTri = 3,  // 3,4 = 64-bit
  kSZeroI = 5, kSZeroT = 6,                       // 6,7
  kSCoreI = 8, kSCoreT = 9,                       // 9,10 (core and not zero)
  kSZeroNI = 11, kSZeroNT = 12,                   // nominal bound
  kSCoreNI = 14, kSCoreNT = 15,
  kSNear = 17, kSUnmapped = 18, kSClassified = 19,  // 19: KClassify ran (a census run, not a render-size probe)
  kSCoreOn = 23, kSCheckRun = 24, kSCheckFocus = 27, kSZw0 = 28, kSZw1 = 29,
  kSGbW = 30, kSGbH = 31, kSFocusW = 32, kSFocusH = 33,
  kSDSnapP00 = 40, kSDLiveP00 = 41,
  kSBadTri = 56,   // instances whose bucket weight was over kTriCap (weight 0)
  kSArgTriK = 57,  // 57,58: sum over argument entries of IndexCount / 3 x InstanceCount (64-bit)
  kSCount = 64
};

// ---------------------------------------------------------------------------
// GPU kernels (cs_5_0)
// ---------------------------------------------------------------------------
const char kHlsl[] = R"(
cbuffer Params : register(b0) {
  uint mode; uint nb; uint idxCap; uint nEntries;
  uint mainCap; uint nObj; uint nLod; uint eye;
  float W; float H; float epsPx; float marginPx;
  float coreR; float shapeP; float tlx; float tly;
  float4 gazeTan;
  uint gazeOk; uint expZwOk; float expZw0; float expZw1;
};
cbuffer TerrainSnap : register(b1) { float4 tc[136]; };
cbuffer TerrainLive : register(b2) { float4 tl[136]; };
struct Inst { uint4 a, b, c, d; };
struct Obj { uint4 q0, q1, q2, q3; };
struct Lod { uint4 q0, q1, q2; };
StructuredBuffer<Inst> mainB : register(t0);
Buffer<uint> idxIn : register(t1);
Buffer<uint> offs : register(t2);
Buffer<uint> cnts : register(t3);
StructuredBuffer<Obj> objs : register(t4);
StructuredBuffer<Lod> lods : register(t5);
Buffer<uint> l2o : register(t6);
Buffer<uint> argsIn : register(t7);
RWBuffer<uint> stats : register(u1);
RWStructuredBuffer<float4> st : register(u2);
RWBuffer<uint> btri : register(u3);
RWBuffer<uint> outA : register(u5);

void Add64(uint at, uint v) {
  if (v == 0) return;
  uint o;
  InterlockedAdd(stats[at], v, o);
  if (o > 0xffffffffu - v) InterlockedAdd(stats[at + 1], 1u);
}

// Rigorous radius about the pivot (R38R1 report): transform, LOD cross-fade, the blend<0 axis scaling, SpeedTree
// bend and leaf shake for any wind: |C.y| + 3 |C.xz| + 4 s r; sqrt(10) s (|c| + r) when the blend flag scales axes.
void Bounds(Inst r, Obj ob, out float Rrig, out float Rnom) {
  float3 X = asfloat(r.b.yzw);
  float3 Z = asfloat(r.c.xyz);
  float3 c = asfloat(uint3(ob.q0.w, ob.q1.x, ob.q1.y));
  float rad = abs(asfloat(ob.q1.z));
  float lx = length(X), lz = length(Z);
  float3 Yd = cross(Z, X);
  float ly = length(Yd);
  float3 Y = ly > 0 ? Yd * (lx / ly) : float3(0, 0, 0);
  float cosXZ = (lx > 0 && lz > 0) ? abs(dot(X, Z)) / (lx * lz) : 0;
  float s = cosXZ < 1e-3 ? max(lx, lz) * 1.002 : sqrt(2 * lx * lx + lz * lz) * 1.002;
  float3 C = X * c.x + Y * c.y + Z * c.z;
  float rw = s * rad;
  bool scaled = asfloat(r.d.x) < 0;
  float tree = abs(C.y) + 3 * length(C.xz) + 4 * rw;
  float flat = 3.1623 * s * (length(c) + rad);
  Rrig = scaled ? max(tree, flat) : tree;
  Rnom = length(C) + rw;
}

// Conservative NDC box of the sphere (pos, R) under the snapshot VP (rows 20..23). false: w may be <= 0.
bool NdcBox(float3 p, float R, out float4 box) {
  float4 r0 = tc[20], r1 = tc[21], r3 = tc[23];
  float cx = dot(r0.xyz, p) + r0.w, cy = dot(r1.xyz, p) + r1.w, cw = dot(r3.xyz, p) + r3.w;
  float ax = dot(abs(r0.xyz), abs(p)) + abs(r0.w), ay = dot(abs(r1.xyz), abs(p)) + abs(r1.w),
        aw = dot(abs(r3.xyz), abs(p)) + abs(r3.w);
  float ex = R * length(r0.xyz) + 1e-5 * ax, ey = R * length(r1.xyz) + 1e-5 * ay, ew = R * length(r3.xyz) + 1e-5 * aw;
  float w0 = cw - ew, w1 = cw + ew;
  box = float4(0, 0, 0, 0);
  if (!(w0 > 1e-6)) return false;
  float xa = cx - ex, xb = cx + ex, ya = cy - ey, yb = cy + ey;
  box.x = min(min(xa / w0, xa / w1), min(xb / w0, xb / w1));
  box.y = max(max(xa / w0, xa / w1), max(xb / w0, xb / w1));
  box.z = min(min(ya / w0, ya / w1), min(yb / w0, yb / w1));
  box.w = max(max(ya / w0, ya / w1), max(yb / w0, yb / w1));
  return true;
}
// A pixel centre k + 0.5 (k in [0, n)) in [a, b].
bool HasCentre(float a, float b, float n) {
  a = clamp(a, -1e7, 1e7);
  b = clamp(b, -1e7, 1e7);
  float k0 = max(ceil(a - 0.5), 0.0), k1 = min(floor(b - 0.5), n - 1);
  return k0 <= k1;
}
bool ZeroCoverage(float4 box) {
  float X0 = (box.x * 0.5 + 0.5) * W + tlx, X1 = (box.y * 0.5 + 0.5) * W + tlx;
  float Y0 = (0.5 - box.w * 0.5) * H + tly, Y1 = (0.5 - box.z * 0.5) * H + tly;
  return !HasCentre(X0 - epsPx - tlx, X1 + epsPx - tlx, W) || !HasCentre(Y0 - epsPx - tly, Y1 + epsPx - tly, H);
}
// Box (NDC, grown by the margin) inside the superellipse core of rect (l, r, b, t in periphery NDC).
bool InCore(float4 box, float4 rect) {
  float mx = 2 * marginPx / W, my = 2 * marginPx / H;
  float hw = 0.5 * (rect.y - rect.x), hh = 0.5 * (rect.w - rect.z);
  if (!(hw > 0 && hh > 0) || !(coreR > 0) || !(shapeP >= 1)) return false;
  float cx = 0.5 * (rect.x + rect.y), cy = 0.5 * (rect.z + rect.w);
  float um = max(abs(box.x - mx - cx), abs(box.y + mx - cx)) / hw;
  float vm = max(abs(box.z - my - cy), abs(box.w + my - cy)) / hh;
  if (!(um < coreR && vm < coreR)) return false;
  return pow(um / coreR, shapeP) + pow(vm / coreR, shapeP) <= 0.99999;
}
bool ProjOk(float4 r0, float4 r1, float4 r3) {
  return r0.y == 0 && r0.w == 0 && r1.x == 0 && r1.w == 0 && all(r3 == float4(0, 0, 1, 0)) && r0.x > 0 && r1.y > 0;
}
// The focus core rect from the gaze tangents and the periphery projection (snapshot rows 16, 17, 19).
bool GazeRect(out float4 rect) {
  rect = float4(0, 0, 0, 0);
  if (gazeOk == 0 || !ProjOk(tc[16], tc[17], tc[19])) return false;
  rect = float4(tc[16].x * gazeTan.x + tc[16].z, tc[16].x * gazeTan.y + tc[16].z,
                tc[17].y * gazeTan.w + tc[17].z, tc[17].y * gazeTan.z + tc[17].z);
  return rect.y > rect.x && rect.w > rect.z;
}

// Per-bucket triangle weight: sum over the argument entries of that bucket of IndexCountPerInstance / 3
// (btri cleared to 0 before).
[numthreads(256, 1, 1)]
void KBucketTri(uint3 dt : SV_DispatchThreadID) {
  uint e = dt.x;
  if (e >= nEntries) return;
  uint b = argsIn[e * 8 + 7];
  if (b < nb) InterlockedAdd(btri[b], argsIn[e * 8 + 0] / 3u);
}
)" R"(
groupshared uint gsI[16];

[numthreads(256, 1, 1)]
void KClassify(uint3 dt : SV_DispatchThreadID, uint gi : SV_GroupIndex) {
  if (gi < 16) gsI[gi] = 0;
  GroupMemoryBarrierWithGroupSync();
  uint j = dt.x;
  uint total = offs[nb - 1] + cnts[nb - 1];
  uint err = 0;
  if (total > idxCap) err |= 0x1;
  bool viewOk = expZwOk == 0 || (tc[44].z == expZw0 && tc[44].w == expZw1);
  if (!viewOk) err |= 0x20;
  if (j < nEntries) {
    uint ab = argsIn[j * 8 + 7];
    uint ic = argsIn[j * 8 + 0], ni = argsIn[j * 8 + 1];
    if (ab < nb && ni != cnts[ab]) err |= 0x10;
    // the triangles the arguments list (every entry): exact 64-bit (ic / 3) x ni from 16-bit halves
    uint ta = ic / 3u, a0 = ta & 0xffffu, a1 = ta >> 16, b0 = ni & 0xffffu, b1 = ni >> 16;
    Add64(57, a0 * b0);
    uint m1 = a0 * b1, m2 = a1 * b0;
    Add64(57, m1 << 16);
    Add64(57, m2 << 16);
    InterlockedAdd(stats[58], (m1 >> 16) + (m2 >> 16) + a1 * b1);
  }
  float4 grect, prect;
  bool gOk = GazeRect(grect);
  bool pOk = st[eye * 6 + 1].x > 0.5;
  prect = st[eye * 6 + 0];
  bool coreOn = (gOk || pOk) && viewOk;
  if (j == 0) {
    st[eye * 6 + 2] = grect;
    st[eye * 6 + 3] = prect;
    st[eye * 6 + 4] = float4((gOk ? 1 : 0) + (pOk ? 2 : 0), coreOn ? 1 : 0, 0, 0);
    stats[23] = coreOn ? 1 : 0;
    stats[1] = total;
    stats[19] = 1;
  }
  if (j < total && (err & 0x1) == 0) {
    uint i = idxIn[j];
    if (i >= mainCap) {
      err |= 0x2;
    } else {
      Inst r = mainB[i];
      uint b = r.a.x & 0xffffffu;
      uint tri = 0;
      if (b >= nb) {
        err |= 0x4;
      } else {
        uint o = offs[b], n = cnts[b];
        if (j < o || j - o >= n) err |= 0x8;
        tri = btri[b];
        if (tri > 4000000u) {
          InterlockedAdd(gsI[12], 1u);
          tri = 0;
        }
      }
      uint lodI = r.a.y & 0xffffffu;
      uint ob = lodI < nLod ? l2o[lodI] : 0xffffffffu;
      InterlockedAdd(gsI[0], 1u);
      InterlockedAdd(gsI[1], tri);
      if (ob >= nObj) {
        InterlockedAdd(gsI[11], 1u);
      } else {
        float Rrig, Rnom;
        Bounds(r, objs[ob], Rrig, Rnom);
        float3 p = asfloat(uint3(r.a.z, r.a.w, r.b.x));
        float fp = 4e-6 * length(p) + 1e-3;
        float4 bR, bN;
        bool okR = NdcBox(p, Rrig + fp, bR), okN = NdcBox(p, Rnom + fp, bN);
        if (!okR) InterlockedAdd(gsI[10], 1u);
        bool zR = okR && ZeroCoverage(bR), zN = okN && ZeroCoverage(bN);
        bool cR = okR && !zR && coreOn && (!gOk || InCore(bR, grect)) && (!pOk || InCore(bR, prect));
        bool cN = okN && !zN && coreOn && (!gOk || InCore(bN, grect)) && (!pOk || InCore(bN, prect));
        if (zR) { InterlockedAdd(gsI[2], 1u); InterlockedAdd(gsI[3], tri); }
        if (cR) { InterlockedAdd(gsI[4], 1u); InterlockedAdd(gsI[5], tri); }
        if (zN) { InterlockedAdd(gsI[6], 1u); InterlockedAdd(gsI[7], tri); }
        if (cN) { InterlockedAdd(gsI[8], 1u); InterlockedAdd(gsI[9], tri); }
      }
    }
  }
  if (err != 0) InterlockedOr(stats[0], err);
  GroupMemoryBarrierWithGroupSync();
  if (gi == 0) {
    InterlockedAdd(stats[2], gsI[0]);
    Add64(3, gsI[1]);
    InterlockedAdd(stats[5], gsI[2]);
    Add64(6, gsI[3]);
    InterlockedAdd(stats[8], gsI[4]);
    Add64(9, gsI[5]);
    InterlockedAdd(stats[11], gsI[6]);
    Add64(12, gsI[7]);
    InterlockedAdd(stats[14], gsI[8]);
    Add64(15, gsI[9]);
    InterlockedAdd(stats[17], gsI[10]);
    InterlockedAdd(stats[18], gsI[11]);
    InterlockedAdd(stats[56], gsI[12]);
  }
}

// lod -> object (objectLods index -> object index), map cleared to ~0 before.
[numthreads(64, 1, 1)]
void KLod2Obj(uint3 dt : SV_DispatchThreadID) {
  uint o = dt.x;
  if (o >= nObj) return;
  Obj ob = objs[o];
  int cnt = (int)ob.q2.x, start = (int)ob.q2.y;
  if (cnt <= 0 || cnt > 64 || start < 0) return;
  for (int k = 0; k < cnt; ++k) {
    uint l = (uint)(start + k);
    if (l < nLod) outA[l] = o;
  }
}

// After a periphery run's pass: the TerrainContext the G-buffer used (b2, live) against the snapshot (b1).
[numthreads(1, 1, 1)]
void KCheckRun() {
  uint e = 0;
  [unroll] for (uint q = 16; q < 24; ++q)
    if (any(asuint(tl[q]) != asuint(tc[q]))) e |= 0x100;
  if (asint(tl[44].x) != 0 || (W > 0 && (tl[44].z != W || tl[44].w != H))) e |= 0x200;
  if (e != 0) InterlockedOr(stats[0], e);
  stats[24] = 1;
  stats[28] = asuint(tc[44].z);
  stats[29] = asuint(tc[44].w);
  stats[30] = asuint(tl[44].z);
  stats[31] = asuint(tl[44].w);
  stats[40] = asuint(tc[16].x);
  stats[41] = asuint(tl[16].x);
}
// After the same eye's focus run: the true core rect from DCS's focus projection (b2) and the periphery
// snapshot (b1); kept for the next frame.
[numthreads(1, 1, 1)]
void KCheckFocus() {
  uint e = 0;
  float4 rect = float4(0, 0, 0, 0);
  bool ok = ProjOk(tc[16], tc[17], tc[19]) && ProjOk(tl[16], tl[17], tl[19]);
  [unroll] for (uint q = 22; q < 24; ++q) {
    float4 d = abs(tl[q] - tc[q]), m = max(abs(tl[q]), abs(tc[q]));
    if (any(d > 1e-5 * max(m, float4(1, 1, 1, 1)))) ok = false;
  }
  if (ok) {
    rect.x = tc[16].x * ((-1 - tl[16].z) / tl[16].x) + tc[16].z;
    rect.y = tc[16].x * ((1 - tl[16].z) / tl[16].x) + tc[16].z;
    rect.z = tc[17].y * ((-1 - tl[17].z) / tl[17].y) + tc[17].z;
    rect.w = tc[17].y * ((1 - tl[17].z) / tl[17].y) + tc[17].z;
  } else {
    e |= 0x800;
  }
  st[eye * 6 + 0] = rect;
  st[eye * 6 + 1] = float4(ok ? 1 : 0, 0, 0, 0);
  if (e != 0) InterlockedOr(stats[0], e);
  stats[27] = 1;
  stats[32] = asuint(tl[44].z);
  stats[33] = asuint(tl[44].w);
}
)";

enum Kern { kKBucketTri, kKClassify, kKLod2Obj, kKCheckRun, kKCheckFocus, kKernels };
const char* const kKernName[kKernels] = {"KBucketTri", "KClassify", "KLod2Obj", "KCheckRun", "KCheckFocus"};

bool CompileEntry(const char* entry, std::vector<uint8_t>& out, std::string* err) {
  if (!chainfast::LoadCompiler()) {
    if (err) *err = "d3dcompiler_47.dll / D3DCompile not available";
    return false;
  }
  ID3DBlob* code = nullptr;
  ID3DBlob* e = nullptr;
  const HRESULT hr = chainfast::g_compile(kHlsl, strlen(kHlsl), entry, nullptr, nullptr, entry, "cs_5_0",
                                          D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &e);
  if (FAILED(hr) || !code) {
    if (err) *err = e ? std::string(static_cast<const char*>(e->GetBufferPointer()), e->GetBufferSize()) : "D3DCompile failed";
    if (e) e->Release();
    if (code) code->Release();
    return false;
  }
  if (e) e->Release();
  const uint8_t* p = static_cast<const uint8_t*>(code->GetBufferPointer());
  out.assign(p, p + code->GetBufferSize());
  code->Release();
  return true;
}

std::vector<uint8_t> g_code[kKernels];
std::atomic<bool> g_codeReady{false};
std::string g_codeErr;
bool CompileAll() {
  if (g_codeReady.load()) return true;
  for (int k = 0; k < kKernels; ++k)
    if (!CompileEntry(kKernName[k], g_code[k], &g_codeErr)) {
      g_codeErr = std::string(kKernName[k]) + ": " + g_codeErr;
      return false;
    }
  g_codeReady = true;
  return true;
}

// ---------------------------------------------------------------------------
// Pure parts (offline tested): the per-bucket triangle weight, one run into the totals, the log line
// ---------------------------------------------------------------------------
// CPU mirror of KBucketTri (argument entries of 8 words).
inline std::vector<uint32_t> BucketTriCpu(const std::vector<uint32_t>& args, uint32_t nb) {
  std::vector<uint32_t> w(nb, 0);
  for (size_t e = 0; e + 8 <= args.size(); e += 8)
    if (args[e + 7] < nb) w[args[e + 7]] += args[e] / 3u;
  return w;
}

constexpr int kErrBitN = 12;  // per-bit run counts, bits 0x1 .. 0x800
struct Census {
  uint64_t runs = 0, okRuns = 0, coreRuns = 0, inst = 0, tri = 0, zeroI = 0, zeroT = 0, zeroNI = 0, zeroNT = 0,
           coreI = 0, coreT = 0, coreInst = 0, coreTri = 0, nearI = 0, unmapped = 0, coreOn = 0, badTri = 0,
           argTri = 0;
  uint32_t errBits = 0;
  uint64_t bitRuns[kErrBitN] = {};
};

// One run's statistics (as read back) into the totals. A run with a kErrDrop bit is counted with its bits only; the
// core share also leaves out the runs whose focus check failed (kErrEye).
inline void Accumulate(Census& c, const uint32_t* s) {
  auto q = [&](int at) { return static_cast<uint64_t>(s[at]) | (static_cast<uint64_t>(s[at + 1]) << 32); };
  const uint32_t err = s[kSErr];
  ++c.runs;
  c.errBits |= err;
  for (int k = 0; k < kErrBitN; ++k)
    if (err & (1u << k)) ++c.bitRuns[k];
  if (err & kErrDrop) return;
  ++c.okRuns;
  c.inst += s[kSInst];
  c.tri += q(kSTri);
  c.zeroI += s[kSZeroI];
  c.zeroT += q(kSZeroT);
  c.zeroNI += s[kSZeroNI];
  c.zeroNT += q(kSZeroNT);
  c.nearI += s[kSNear];
  c.unmapped += s[kSUnmapped];
  c.badTri += s[kSBadTri];
  c.argTri += q(kSArgTriK);
  c.coreOn += s[kSCoreOn] ? 1 : 0;
  if (!(err & kErrEye)) {
    ++c.coreRuns;
    c.coreI += s[kSCoreI];
    c.coreT += q(kSCoreT);
    c.coreInst += s[kSInst];
    c.coreTri += q(kSTri);
  }
}
inline Census Sum(const Census& a, const Census& b) {
  Census s = a;
  s.runs += b.runs;
  s.okRuns += b.okRuns;
  s.coreRuns += b.coreRuns;
  s.inst += b.inst;
  s.tri += b.tri;
  s.zeroI += b.zeroI;
  s.zeroT += b.zeroT;
  s.zeroNI += b.zeroNI;
  s.zeroNT += b.zeroNT;
  s.coreI += b.coreI;
  s.coreT += b.coreT;
  s.coreInst += b.coreInst;
  s.coreTri += b.coreTri;
  s.nearI += b.nearI;
  s.unmapped += b.unmapped;
  s.coreOn += b.coreOn;
  s.badTri += b.badTri;
  s.argTri += b.argTri;
  s.errBits |= b.errBits;
  for (int k = 0; k < kErrBitN; ++k) s.bitRuns[k] += b.bitRuns[k];
  return s;
}
inline double Pct(uint64_t a, uint64_t b) { return b ? 100.0 * static_cast<double>(a) / static_cast<double>(b) : 0.0; }

// The greppable line ("periph census: ..."); B6 gate: both zero-coverage shares under 5 % closes the item.
inline std::string Line(const Census& l, const Census& r, double sec) {
  const Census t = Sum(l, r);
  char b[1400];
  if (!t.okRuns) {
    sprintf_s(b, "periph census: %.0f s, no usable run (%llu runs, error bits 0x%x)", sec,
              static_cast<unsigned long long>(t.runs), t.errBits);
    return b;
  }
  const double n = static_cast<double>(t.okRuns);
  const double zi = Pct(t.zeroI, t.inst), zt = Pct(t.zeroT, t.tri);
  std::string bits;
  for (int k = 0; k < kErrBitN; ++k)
    if (t.bitRuns[k]) {
      char x[40];
      sprintf_s(x, "%s0x%x %llu", bits.empty() ? "" : ", ", 1u << k, static_cast<unsigned long long>(t.bitRuns[k]));
      bits += x;
    }
  sprintf_s(b,
            "periph census: %.0f s, %llu runs (%llu used, L %llu R %llu); zero coverage %.2f %% of instances, %.2f %% of "
            "triangles (L %.2f / %.2f, R %.2f / %.2f); nominal bound %.2f / %.2f %%; focus core %.2f / %.2f %% "
            "(core known in %.0f %% of runs); per run %.0f instances, %.3f M triangles (argument entries %.3f M), "
            "near-plane %.0f, unmapped %.0f, buckets over 4 M %.1f; error runs: %s; B6 gate (both zero shares < 5 %%): "
            "%s",
            sec, static_cast<unsigned long long>(t.runs), static_cast<unsigned long long>(t.okRuns),
            static_cast<unsigned long long>(l.okRuns), static_cast<unsigned long long>(r.okRuns), zi, zt,
            Pct(l.zeroI, l.inst), Pct(l.zeroT, l.tri), Pct(r.zeroI, r.inst), Pct(r.zeroT, r.tri), Pct(t.zeroNI, t.inst),
            Pct(t.zeroNT, t.tri), Pct(t.coreI, t.coreInst), Pct(t.coreT, t.coreTri), Pct(t.coreOn, t.okRuns),
            t.inst / n, t.tri / n / 1e6, t.argTri / n / 1e6, t.nearI / n, t.unmapped / n, t.badTri / n,
            bits.empty() ? "none" : bits.c_str(), zi < 5.0 && zt < 5.0 ? "below, close B6" : "reached, reopen B6");
  return b;
}

// ---------------------------------------------------------------------------
// DCS's resources (identified at run time) and ours
// ---------------------------------------------------------------------------
struct Dcs {
  ID3D11Buffer* args = nullptr;  // indirect arguments
  ID3D11Buffer* mainB = nullptr; // OutputInstanceData
  ID3D11Buffer* idx = nullptr;   // indices
  ID3D11Buffer* offs = nullptr;  // offsets
  ID3D11Buffer* cnts = nullptr;  // counts
  ID3D11Buffer* tc = nullptr;    // TerrainContext CB (VS b11)
  ID3D11Buffer* objs = nullptr;  // instancer2_objects (Parse t3)
  ID3D11Buffer* lods = nullptr;  // instancer2_objectLods (Parse t4)
  UINT nArgs = 0, mainCap = 0, idxCap = 0, nb = 0, nObj = 0, nLod = 0, tcBytes = 0;
};

struct Ours {
  ID3D11ComputeShader* cs[kKernels] = {};
  ID3D11Buffer* params = nullptr;  // dynamic CB
  ID3D11Buffer* snap[2] = {};      // TerrainContext snapshots per eye, taken at the first instancer draw
  ID3D11Buffer* fsnap[2] = {};     // the same eye's focus TerrainContext, at the focus view's first instancer draw
  ID3D11ShaderResourceView *srvMain = nullptr, *srvIdx = nullptr, *srvOffs = nullptr, *srvCnts = nullptr,
                           *srvObjs = nullptr, *srvLods = nullptr, *srvArgs = nullptr;
  chainfast::TBuf l2o, btri, stats[2], state;
  ID3D11Buffer* staging[8] = {};
  int stagingEye[8] = {-1, -1, -1, -1, -1, -1, -1, -1};
  uint64_t stagingFrame[8] = {};
};

// Render-thread state.
struct Rt {
  int lbOrd = 0;
  int view = -1;         // LightsBuffersPass ordinal of the current view
  bool inRun = false;    // inside a GBufferPass run
  int runView = -1;
  uint32_t eyeMode[2] = {};
  float W[2] = {}, H[2] = {};
  float focusW = 0;
  uint32_t totalEst[2] = {};  // listed instances (the last readback), for the slot range processed
  uint32_t range = 0;
  UINT samples[2] = {};  // the periphery G-buffer depth target's sample count (1 expected)
  bool zwKnown[2] = {};
  float zw[2][2] = {};
  uint64_t frame = 0;
  uint64_t l2oRuns = 0;
  bool l2oValid = false;
  int armDraw = 0;
  bool armDispatch = false;
  bool pending = false;      // a periphery run waits for its first instancer draw
  bool passActive = false;   // the G-buffer pass holding this run's snapshot is open
  int passEye = -1;
  ID3D11Buffer* runCam = nullptr;  // the b11 buffer of the snapshot draw (AddRef'd until the pass end)
  bool focusPending[2] = {}, focusTaken[2] = {};
};
constexpr int kArmLearn = 1, kArmBegin = 4, kArmFocus = 8, kArmCount = 16;
constexpr uint32_t kMCensus = 4, kMProbe = 32;

ID3D11Device* g_dev = nullptr;
ID3D11DeviceContext* g_ctx = nullptr;
Dcs g_d;
Ours g_o;
Rt g_rt;
std::mutex g_mu;  // g_d / g_o creation vs Shutdown (render thread vs worker)
std::atomic<bool> g_drawLearned{false}, g_dispatchLearned{false}, g_resReady{false};
std::atomic<int> g_resFail{0};
std::atomic<uint64_t> g_runs{0}, g_learnDraws{0}, g_learnDisp{0}, g_orderBad{0}, g_armBlocked{0}, g_noDraw{0},
    g_otherLists{0}, g_noCam{0}, g_lateDraws{0}, g_focusNoDraw{0}, g_detailLogged{0}, g_rearmRefused{0};
Census g_cen[2];
std::mutex g_cenMu;

// Gaze (Perception's layer): read on the render thread at a periphery run.
std::atomic<void*> g_gazeView{nullptr};
HANDLE g_gazeMap = nullptr;
ULONGLONG g_gazeTry = 0;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
template <typename T>
inline void Rel(T*& p) {
  if (p) p->Release();
  p = nullptr;
}
inline UINT BufBytes(ID3D11Buffer* b) {
  D3D11_BUFFER_DESC d{};
  if (b) b->GetDesc(&d);
  return d.ByteWidth;
}
inline ID3D11Buffer* BufOf(ID3D11View* v) {
  if (!v) return nullptr;
  ID3D11Resource* r = nullptr;
  v->GetResource(&r);
  if (!r) return nullptr;
  ID3D11Buffer* b = nullptr;
  r->QueryInterface(__uuidof(ID3D11Buffer), reinterpret_cast<void**>(&b));
  r->Release();
  if (b) b->Release();  // the view keeps it alive; we only compare / AddRef later
  return b;
}
// Typed R32_UINT buffer view (or a structured one of the given stride when stride != 0).
inline bool ViewIs(ID3D11ShaderResourceView* v, UINT stride, UINT* elems) {
  if (!v) return false;
  D3D11_SHADER_RESOURCE_VIEW_DESC d{};
  v->GetDesc(&d);
  if (d.ViewDimension != D3D11_SRV_DIMENSION_BUFFER && d.ViewDimension != D3D11_SRV_DIMENSION_BUFFEREX) return false;
  ID3D11Buffer* b = BufOf(v);
  if (!b) return false;
  D3D11_BUFFER_DESC bd{};
  b->GetDesc(&bd);
  if (stride) {
    if (d.Format != DXGI_FORMAT_UNKNOWN || !(bd.MiscFlags & D3D11_RESOURCE_MISC_BUFFER_STRUCTURED) ||
        bd.StructureByteStride != stride)
      return false;
  } else if (d.Format != DXGI_FORMAT_R32_UINT) {
    return false;
  }
  if (elems) *elems = d.Buffer.NumElements;
  return true;
}
inline bool MakeTypedSrv(ID3D11Buffer* b, UINT elems, ID3D11ShaderResourceView** out) {
  D3D11_SHADER_RESOURCE_VIEW_DESC v{};
  v.Format = DXGI_FORMAT_R32_UINT;
  v.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
  v.Buffer.NumElements = elems;
  return SUCCEEDED(g_dev->CreateShaderResourceView(b, &v, out));
}
inline bool MakeStructSrv(ID3D11Buffer* b, UINT elems, ID3D11ShaderResourceView** out) {
  D3D11_SHADER_RESOURCE_VIEW_DESC v{};
  v.Format = DXGI_FORMAT_UNKNOWN;
  v.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
  v.Buffer.NumElements = elems;
  return SUCCEEDED(g_dev->CreateShaderResourceView(b, &v, out));
}
inline bool MakeTyped(UINT elems, chainfast::TBuf& r) { return chainfast::MakeBuf(g_dev, elems, 0, nullptr, true, true, r); }

// Foreign hooks that re-arm by taking whatever entry they find: then the one-shot arms stay out.
inline bool ForeignHooks() {
  return gpt::g_opsHooked.load(std::memory_order_relaxed) || d3ds::g_vtblCount.load(std::memory_order_relaxed) > 0 ||
         fixdepth::g_drawArmed;
}

// CS state save / restore around our dispatches (DCS's state cache must find what it set).
struct CsState {
  ID3D11ComputeShader* cs = nullptr;
  ID3D11ClassInstance* ci[8] = {};
  UINT nci = 8;
  ID3D11Buffer* cb[3] = {};
  ID3D11ShaderResourceView* srv[8] = {};
  ID3D11UnorderedAccessView* uav[8] = {};
  void Save(ID3D11DeviceContext* c) {
    c->CSGetShader(&cs, ci, &nci);
    c->CSGetConstantBuffers(0, 3, cb);
    c->CSGetShaderResources(0, 8, srv);
    c->CSGetUnorderedAccessViews(0, 8, uav);
  }
  void Restore(ID3D11DeviceContext* c) {
    ID3D11ShaderResourceView* ns[8] = {};
    ID3D11UnorderedAccessView* nu[8] = {};
    c->CSSetShaderResources(0, 8, ns);
    c->CSSetUnorderedAccessViews(0, 8, nu, nullptr);
    const UINT keep[8] = {UINT(-1), UINT(-1), UINT(-1), UINT(-1), UINT(-1), UINT(-1), UINT(-1), UINT(-1)};
    c->CSSetUnorderedAccessViews(0, 8, uav, keep);
    c->CSSetShaderResources(0, 8, srv);
    c->CSSetConstantBuffers(0, 3, cb);
    c->CSSetShader(cs, nci ? ci : nullptr, nci);
    Rel(cs);
    for (auto& p : ci) Rel(p);
    for (auto& p : cb) Rel(p);
    for (auto& p : srv) Rel(p);
    for (auto& p : uav) Rel(p);
  }
};

// ---------------------------------------------------------------------------
// Resource creation (render thread, once the DCS buffers are known)
// ---------------------------------------------------------------------------
void ReleaseOurs() {
  for (auto& s : g_o.cs) Rel(s);
  Rel(g_o.params);
  for (auto& s : g_o.snap) Rel(s);
  for (auto& s : g_o.fsnap) Rel(s);
  Rel(g_rt.runCam);
  Rel(g_o.srvMain);
  Rel(g_o.srvIdx);
  Rel(g_o.srvOffs);
  Rel(g_o.srvCnts);
  Rel(g_o.srvObjs);
  Rel(g_o.srvLods);
  Rel(g_o.srvArgs);
  for (chainfast::TBuf* b : {&g_o.l2o, &g_o.btri, &g_o.stats[0], &g_o.stats[1], &g_o.state}) b->Release();
  for (auto& s : g_o.staging) Rel(s);
  g_o = Ours{};
}
void ReleaseDcs() {
  Rel(g_d.args);
  Rel(g_d.mainB);
  Rel(g_d.idx);
  Rel(g_d.offs);
  Rel(g_d.cnts);
  Rel(g_d.tc);
  Rel(g_d.objs);
  Rel(g_d.lods);
  g_d = Dcs{};
}

bool CreateOurs() {
  if (g_resReady.load()) return true;
  if (g_resFail.load() >= 3 || !g_codeReady.load() || !g_drawLearned.load() || !g_dispatchLearned.load()) return false;
  const Dcs& d = g_d;
  bool ok = d.idxCap > 0 && d.idxCap <= 1024u * 1024u && d.nb > 0 && d.nArgs > 0 && d.nObj > 0 && d.nLod > 0 &&
            d.tcBytes >= 136 * 16;
  for (int k = 0; ok && k < kKernels; ++k)
    ok = SUCCEEDED(g_dev->CreateComputeShader(g_code[k].data(), g_code[k].size(), nullptr, &g_o.cs[k]));
  if (ok) {
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = 128;
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    ok = SUCCEEDED(g_dev->CreateBuffer(&bd, nullptr, &g_o.params));
    D3D11_BUFFER_DESC sd{};
    sd.ByteWidth = d.tcBytes;
    sd.Usage = D3D11_USAGE_DEFAULT;
    sd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    for (int e = 0; ok && e < 2; ++e)
      ok = SUCCEEDED(g_dev->CreateBuffer(&sd, nullptr, &g_o.snap[e])) &&
           SUCCEEDED(g_dev->CreateBuffer(&sd, nullptr, &g_o.fsnap[e]));
  }
  ok = ok && MakeStructSrv(d.mainB, d.mainCap, &g_o.srvMain) && MakeTypedSrv(d.idx, d.idxCap, &g_o.srvIdx) &&
       MakeTypedSrv(d.offs, d.nb, &g_o.srvOffs) && MakeTypedSrv(d.cnts, d.nb, &g_o.srvCnts) &&
       MakeStructSrv(d.objs, d.nObj, &g_o.srvObjs) && MakeStructSrv(d.lods, d.nLod, &g_o.srvLods) &&
       MakeTypedSrv(d.args, d.nArgs * 8, &g_o.srvArgs);
  ok = ok && MakeTyped(d.nLod, g_o.l2o) && MakeTyped(d.nb, g_o.btri) && MakeTyped(kSCount, g_o.stats[0]) &&
       MakeTyped(kSCount, g_o.stats[1]) && chainfast::MakeBuf(g_dev, 12, 16, nullptr, true, true, g_o.state);
  if (ok) {
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = kSCount * 4;
    bd.Usage = D3D11_USAGE_STAGING;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    for (int i = 0; ok && i < 8; ++i) ok = SUCCEEDED(g_dev->CreateBuffer(&bd, nullptr, &g_o.staging[i]));
  }
  if (!ok) {
    ReleaseOurs();
    const int f = ++g_resFail;
    Log("periph census: creating our buffers / views / kernels failed (%d of 3)%s", f,
        f >= 3 ? "; census off for the session" : "");
    return false;
  }
  const UINT zero[4] = {};
  g_ctx->ClearUnorderedAccessViewUint(g_o.state.uav, zero);
  g_rt.l2oValid = false;
  g_resReady = true;
  Log("periph census: ready: %u argument entries, %u buckets, indices %u, mainBuffer %u, objects %u, LODs %u, "
      "TerrainContext %u bytes",
      d.nArgs, d.nb, d.idxCap, d.mainCap, d.nObj, d.nLod, d.tcBytes);
  return true;
}

// ---------------------------------------------------------------------------
// Table arms: identification (one-shot), the periphery snapshot and the focus snapshot (per run)
// ---------------------------------------------------------------------------
using DrawIndFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Buffer*, UINT);
using DispFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, UINT);
using VsSrvFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, ID3D11ShaderResourceView* const*);
void** g_vt = nullptr;
int g_slotDraw = -1, g_slotDisp = -1, g_slotVsSrv = -1;
void* volatile g_origDraw = nullptr;
void* volatile g_origDisp = nullptr;
void* volatile g_origVsSrv = nullptr;
thread_local bool t_inside = false;

void LearnDraw(ID3D11DeviceContext* c, ID3D11Buffer* args);
void LearnDispatch(ID3D11DeviceContext* c);
void PreDraw(ID3D11DeviceContext* c);

void STDMETHODCALLTYPE H_Draw(ID3D11DeviceContext* c, ID3D11Buffer* b, UINT off) {
  if (c == g_ctx && !t_inside && b && b == g_d.args && (g_rt.armDraw & (kArmBegin | kArmFocus | kArmCount)) &&
      !g_fault.load(std::memory_order_relaxed)) {
    t_inside = true;
    __try {
      PreDraw(c);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
      g_fault = true;
    }
    t_inside = false;
  }
  reinterpret_cast<DrawIndFn>(g_origDraw)(c, b, off);
  if (c != g_ctx || t_inside || g_fault.load(std::memory_order_relaxed)) return;
  if ((g_rt.armDraw & kArmLearn) && !g_drawLearned.load(std::memory_order_relaxed)) {
    t_inside = true;
    __try {
      LearnDraw(c, b);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
      g_fault = true;
    }
    t_inside = false;
  }
}
void STDMETHODCALLTYPE H_Dispatch(ID3D11DeviceContext* c, UINT x, UINT y, UINT z) {
  if (c == g_ctx && g_rt.armDispatch && !t_inside && !g_dispatchLearned.load(std::memory_order_relaxed)) {
    t_inside = true;
    __try {
      LearnDispatch(c);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
      g_fault = true;
    }
    t_inside = false;
  }
  reinterpret_cast<DispFn>(g_origDisp)(c, x, y, z);
}
// ExecuteCommandList / Flush toggle d3d11's draw entries; the SRV setter (not toggled) puts the draw hook back with
// d3d11's new entry as the original (never another hook of this payload: R48 rule).
void STDMETHODCALLTYPE H_VsSrv(ID3D11DeviceContext* c, UINT s, UINT n, ID3D11ShaderResourceView* const* v) {
  reinterpret_cast<VsSrvFn>(g_origVsSrv)(c, s, n, v);
  if (c != g_ctx || !(g_rt.armDraw & (kArmBegin | kArmFocus | kArmCount)) || !g_vt) return;
  void* cur = g_vt[g_slotDraw];
  if (cur != reinterpret_cast<void*>(&H_Draw)) {
    if (d3ds::InPayload(cur)) {
      g_rearmRefused.fetch_add(1, std::memory_order_relaxed);
    } else {
      g_origDraw = cur;
      d3ds::WriteTablePointer(&g_vt[g_slotDraw], reinterpret_cast<void*>(&H_Draw));
    }
  }
}

inline bool SlotsKnown() {
  if (g_slotDraw < 0) {
    g_slotDraw = DcsQv_ContextSlot("DrawIndexedInstancedIndirect");
    g_slotDisp = DcsQv_ContextSlot("Dispatch");
    g_slotVsSrv = DcsQv_ContextSlot("VSSetShaderResources");
  }
  return g_slotDraw >= 0 && g_slotDisp >= 0 && g_slotVsSrv >= 0;
}
inline void ArmVsSrv() {
  void* cs = g_vt[g_slotVsSrv];
  if (cs != reinterpret_cast<void*>(&H_VsSrv)) {
    g_origVsSrv = cs;
    d3ds::WriteTablePointer(&g_vt[g_slotVsSrv], reinterpret_cast<void*>(&H_VsSrv));
  }
}
// why: kArm* bits. An arm already in the table takes the new bits.
bool ArmDraw(int why) {
  if (!SlotsKnown()) return false;
  if (g_rt.armDraw) {
    g_rt.armDraw |= why;
    if ((why & ~kArmLearn) && g_vt) ArmVsSrv();
    return true;
  }
  if (ForeignHooks()) {
    g_armBlocked.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  g_vt = *reinterpret_cast<void***>(g_ctx);
  void* cur = g_vt[g_slotDraw];
  g_rt.armDraw = why;
  if (cur != reinterpret_cast<void*>(&H_Draw)) {  // else still ours from an arm whose bits were all served
    g_origDraw = cur;
    d3ds::WriteTablePointer(&g_vt[g_slotDraw], reinterpret_cast<void*>(&H_Draw));
  }
  if (why & ~kArmLearn) ArmVsSrv();
  return true;
}
void DisarmDraw() {
  if (!g_vt || g_slotDraw < 0) return;
  if (g_vt[g_slotDraw] == reinterpret_cast<void*>(&H_Draw)) d3ds::WriteTablePointer(&g_vt[g_slotDraw], g_origDraw);
  if (g_vt[g_slotVsSrv] == reinterpret_cast<void*>(&H_VsSrv)) d3ds::WriteTablePointer(&g_vt[g_slotVsSrv], g_origVsSrv);
  g_rt.armDraw = 0;
}
bool ArmDispatch() {
  if (g_rt.armDispatch || !SlotsKnown()) return false;
  if (ForeignHooks()) {
    g_armBlocked.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  g_vt = *reinterpret_cast<void***>(g_ctx);
  void* cur = g_vt[g_slotDisp];
  if (cur == reinterpret_cast<void*>(&H_Dispatch)) return false;
  g_origDisp = cur;
  g_rt.armDispatch = true;
  d3ds::WriteTablePointer(&g_vt[g_slotDisp], reinterpret_cast<void*>(&H_Dispatch));
  return true;
}
void DisarmDispatch() {
  if (!g_rt.armDispatch || !g_vt) return;
  if (g_vt[g_slotDisp] == reinterpret_cast<void*>(&H_Dispatch)) d3ds::WriteTablePointer(&g_vt[g_slotDisp], g_origDisp);
  g_rt.armDispatch = false;
}
// A hook of ours left in the table after its arm ended (another module re-armed over it and then put "its
// original" back): the saved entry goes back.
void Sweep() {
  if (!g_vt || !SlotsKnown()) return;
  if (!g_rt.armDraw && g_vt[g_slotDraw] == reinterpret_cast<void*>(&H_Draw))
    d3ds::WriteTablePointer(&g_vt[g_slotDraw], g_origDraw);
  if (!g_rt.armDraw && g_vt[g_slotVsSrv] == reinterpret_cast<void*>(&H_VsSrv))
    d3ds::WriteTablePointer(&g_vt[g_slotVsSrv], g_origVsSrv);
  if (!g_rt.armDispatch && g_vt[g_slotDisp] == reinterpret_cast<void*>(&H_Dispatch))
    d3ds::WriteTablePointer(&g_vt[g_slotDisp], g_origDisp);
}

// An instancer draw: the arguments buffer and the VS SRVs mainBuffer / indices / offsets / [prev] / counts.
void LearnDraw(ID3D11DeviceContext* c, ID3D11Buffer* args) {
  g_learnDraws.fetch_add(1, std::memory_order_relaxed);
  if (!args) return;
  D3D11_BUFFER_DESC ad{};
  args->GetDesc(&ad);
  if (!(ad.MiscFlags & D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS) || ad.ByteWidth % 32 || !ad.ByteWidth ||
      !(ad.BindFlags & D3D11_BIND_SHADER_RESOURCE))
    return;
  ID3D11ShaderResourceView* v[16] = {};
  c->VSGetShaderResources(0, 16, v);
  int at = -1, cs = -1;
  UINT nMain = 0, nIdx = 0, nOff = 0, nCnt = 0;
  for (int i = 0; i + 3 < 16 && at < 0; ++i) {
    if (!ViewIs(v[i], 64, &nMain) || !ViewIs(v[i + 1], 0, &nIdx) || !ViewIs(v[i + 2], 0, &nOff)) continue;
    for (int k = i + 3; k <= i + 4 && k < 16; ++k)
      if (ViewIs(v[k], 0, &nCnt) && nCnt == nOff && BufOf(v[k]) != BufOf(v[i + 2])) {
        at = i;
        cs = k;
        break;
      }
  }
  ID3D11Buffer* tc = nullptr;
  c->VSGetConstantBuffers(11, 1, &tc);
  if (at >= 0 && tc && BufBytes(tc) >= 136 * 16) {
    std::lock_guard<std::mutex> lock(g_mu);
    ReleaseDcs();
    g_d.args = args;
    args->AddRef();
    auto take = [](ID3D11ShaderResourceView* sv) {
      ID3D11Buffer* b = BufOf(sv);
      if (b) b->AddRef();
      return b;
    };
    g_d.mainB = take(v[at]);
    g_d.idx = take(v[at + 1]);
    g_d.offs = take(v[at + 2]);
    g_d.cnts = take(v[cs]);
    g_d.tc = tc;
    tc = nullptr;
    g_d.nArgs = ad.ByteWidth / 32;
    g_d.mainCap = BufBytes(g_d.mainB) / 64;
    g_d.idxCap = BufBytes(g_d.idx) / 4;
    g_d.nb = nOff;
    g_d.tcBytes = BufBytes(g_d.tc);
    // the G-buffer depth target of this draw: one sample per pixel expected (zero coverage tests centres)
    ID3D11DepthStencilView* dsv = nullptr;
    c->OMGetRenderTargets(0, nullptr, &dsv);
    UINT samples = 0;
    if (dsv) {
      ID3D11Resource* r = nullptr;
      dsv->GetResource(&r);
      ID3D11Texture2D* t = nullptr;
      if (r) r->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&t));
      if (t) {
        D3D11_TEXTURE2D_DESC td{};
        t->GetDesc(&td);
        samples = td.SampleDesc.Count;
        t->Release();
      }
      if (r) r->Release();
      dsv->Release();
    }
    g_rt.samples[0] = g_rt.samples[1] = samples;
    g_drawLearned = true;
    Log("periph census: instancer draw identified: %u argument entries, VS t%d mainBuffer (%u), t%d indices (%u), "
        "t%d offsets, t%d counts (%u buckets), b11 TerrainContext (%u bytes), depth samples %u",
        g_d.nArgs, at, g_d.mainCap, at + 1, g_d.idxCap, at + 2, cs, g_d.nb, g_d.tcBytes, samples);
  }
  if (tc) tc->Release();
  for (auto& p : v) Rel(p);
}

// IM2 Parse: u1 is the learned mainBuffer, t3 the objects (64 B), t4 the object LODs (48 B).
void LearnDispatch(ID3D11DeviceContext* c) {
  if (!g_drawLearned.load()) return;
  ID3D11UnorderedAccessView* u = nullptr;
  c->CSGetUnorderedAccessViews(1, 1, &u);
  if (!u) return;
  ID3D11Buffer* ub = BufOf(u);
  u->Release();
  if (ub != g_d.mainB) return;
  g_learnDisp.fetch_add(1, std::memory_order_relaxed);
  ID3D11ShaderResourceView* v[2] = {};
  c->CSGetShaderResources(3, 2, v);
  UINT no = 0, nl = 0;
  if (ViewIs(v[0], 64, &no) && ViewIs(v[1], 48, &nl)) {
    std::lock_guard<std::mutex> lock(g_mu);
    Rel(g_d.objs);
    Rel(g_d.lods);
    g_d.objs = BufOf(v[0]);
    g_d.lods = BufOf(v[1]);
    g_d.objs->AddRef();
    g_d.lods->AddRef();
    g_d.nObj = BufBytes(g_d.objs) / 64;
    g_d.nLod = BufBytes(g_d.lods) / 48;
    g_dispatchLearned = true;
    Log("periph census: IM2 Parse identified: objects (%u), object LODs (%u)", g_d.nObj, g_d.nLod);
  }
  for (auto& p : v) Rel(p);
}

// ---------------------------------------------------------------------------
// Per-run GPU work (render thread, DCS's immediate context)
// ---------------------------------------------------------------------------
#pragma pack(push, 4)
struct Params {
  uint32_t mode, nb, idxCap, nEntries;
  uint32_t mainCap, nObj, nLod, eye;
  float W, H, epsPx, marginPx;
  float coreR, shapeP, tlx, tly;
  float gazeTan[4];
  uint32_t gazeOk, expZwOk;
  float expZw0, expZw1;
};
#pragma pack(pop)
static_assert(sizeof(Params) == 96, "Params layout");

bool PutParams(ID3D11DeviceContext* c, const Params& p) {
  D3D11_MAPPED_SUBRESOURCE m{};
  if (FAILED(c->Map(g_o.params, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) return false;
  memset(m.pData, 0, 128);
  memcpy(m.pData, &p, sizeof(p));
  c->Unmap(g_o.params, 0);
  return true;
}

bool g_testGaze = false;  // tests: these tangents instead of the GazeOut block
float g_testGazeTan[4] = {};
// The focus tangents of this eye's focus view from Perception's GazeOut (newest record).
bool GazeTangents(int eye, float out[4]) {
  if (g_testGaze) {
    memcpy(out, g_testGazeTan, sizeof(g_testGazeTan));
    return true;
  }
  void* v = g_gazeView.load(std::memory_order_acquire);
  if (!v) return false;
  ::dcsvr_perception::GazeOut g{};
  if (!::dcsvr_perception::seqlock_read(static_cast<const ::dcsvr_perception::GazeOut*>(v), g, 3)) return false;
  if (g.viewCount != 4) return false;
  const float* f = g.fovTan[2 + eye];
  if (!(f[1] > f[0]) || !(f[2] > f[3])) return false;
  memcpy(out, f, 4 * sizeof(float));  // left, right, up, down
  return true;
}

Params BaseParams(int eye, uint32_t mode) {
  Params p{};
  p.mode = mode;
  p.nb = g_d.nb;
  p.idxCap = g_d.idxCap;
  p.nEntries = g_d.nArgs;
  p.mainCap = g_d.mainCap;
  p.nObj = g_d.nObj;
  p.nLod = g_d.nLod;
  p.eye = static_cast<uint32_t>(eye);
  p.W = g_rt.W[eye];
  p.H = g_rt.H[eye];
  p.epsPx = 1.0f / 16;
  p.marginPx = static_cast<float>(kMarginPx);
  p.coreR = kCoreR;
  p.shapeP = kShapeP;
  p.expZwOk = g_rt.zwKnown[eye] ? 1 : 0;
  p.expZw0 = g_rt.zw[eye][0];
  p.expZw1 = g_rt.zw[eye][1];
  return p;
}

inline void Dispatch(ID3D11DeviceContext* c, int k, UINT gx) {
  c->CSSetShader(g_o.cs[k], nullptr, 0);
  c->Dispatch(gx, 1, 1);
}

// Until the after-run check has read back this view's render size once, only the snapshot and that check run.
uint32_t ModeFor(int eye) {
  if (!g_rt.zwKnown[eye] || !(g_rt.W[eye] > 0) || !(g_rt.H[eye] > 0)) return kMProbe;
  return kMCensus;
}

// Periphery view `eye`, at its first instancer draw on the identified lists: `cam` is the TerrainContext CB that
// draw has bound at VS b11 (R45), so the classification uses exactly the camera DCS's instancer VS will use.
void PeriphBeginAt(int eye, ID3D11Buffer* cam) {
  g_rt.eyeMode[eye] = 0;
  if (!cam || !CreateOurs()) return;
  const uint32_t mode = ModeFor(eye);
  Params p = BaseParams(eye, mode);
  p.gazeOk = GazeTangents(eye, p.gazeTan) ? 1 : 0;
  ID3D11DeviceContext* c = g_ctx;
  if (!PutParams(c, p)) return;
  CsState saved;
  saved.Save(c);
  c->CopyResource(g_o.snap[eye], cam);
  const UINT zero[4] = {};
  c->ClearUnorderedAccessViewUint(g_o.stats[eye].uav, zero);
  ID3D11ShaderResourceView* nul[8] = {};
  ID3D11UnorderedAccessView* nulU[8] = {};
  c->CSSetUnorderedAccessViews(0, 8, nulU, nullptr);
  if (mode == kMProbe) {
    saved.Restore(c);
    g_rt.eyeMode[eye] = mode;
    return;
  }
  // Only the slots in use: the readback's total + 25 % + 8192, whole 1024-slot blocks (error 0x1 when the list
  // outgrew it; the next run takes the new total).
  {
    uint64_t r = g_rt.totalEst[eye] ? uint64_t(g_rt.totalEst[eye]) * 5 / 4 + 8192 : g_d.idxCap;
    r = (r + 1023) & ~uint64_t(1023);
    g_rt.range = static_cast<uint32_t>(std::min<uint64_t>(r, g_d.idxCap));
    p.idxCap = g_rt.range;
    PutParams(c, p);
  }
  const UINT range = g_rt.range;
  ID3D11Buffer* cbs[3] = {g_o.params, g_o.snap[eye], cam};
  c->CSSetConstantBuffers(0, 3, cbs);
  // lod -> object map, rebuilt every 64 runs (the object table is static; periph_lod rewrites distances only).
  if (!g_rt.l2oValid || (g_rt.l2oRuns++ & 63) == 0) {
    const UINT ff[4] = {0xffffffffu, 0xffffffffu, 0xffffffffu, 0xffffffffu};
    c->ClearUnorderedAccessViewUint(g_o.l2o.uav, ff);
    ID3D11ShaderResourceView* s[8] = {nullptr, nullptr, nullptr, nullptr, g_o.srvObjs, nullptr, nullptr, nullptr};
    c->CSSetShaderResources(0, 8, s);
    ID3D11UnorderedAccessView* u[8] = {nullptr, nullptr, nullptr, nullptr, nullptr, g_o.l2o.uav, nullptr, nullptr};
    c->CSSetUnorderedAccessViews(0, 8, u, nullptr);
    Dispatch(c, kKLod2Obj, (g_d.nObj + 63) / 64);
    c->CSSetUnorderedAccessViews(0, 8, nulU, nullptr);
    g_rt.l2oValid = true;
  }
  // per-bucket triangle weight from this run's arguments
  c->ClearUnorderedAccessViewUint(g_o.btri.uav, zero);
  {
    ID3D11ShaderResourceView* s[8] = {nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, g_o.srvArgs};
    c->CSSetShaderResources(0, 8, s);
    ID3D11UnorderedAccessView* u[8] = {nullptr, nullptr, nullptr, g_o.btri.uav, nullptr, nullptr, nullptr, nullptr};
    c->CSSetUnorderedAccessViews(0, 8, u, nullptr);
    Dispatch(c, kKBucketTri, (g_d.nArgs + 255) / 256);
    c->CSSetUnorderedAccessViews(0, 8, nulU, nullptr);
  }
  {
    ID3D11ShaderResourceView* s[8] = {g_o.srvMain, g_o.srvIdx, g_o.srvOffs, g_o.srvCnts, g_o.srvObjs, g_o.srvLods,
                                      g_o.l2o.srv, g_o.srvArgs};
    c->CSSetShaderResources(0, 8, s);
    ID3D11UnorderedAccessView* u[8] = {nullptr, g_o.stats[eye].uav, g_o.state.uav, g_o.btri.uav, nullptr, nullptr,
                                       nullptr, nullptr};
    c->CSSetUnorderedAccessViews(0, 8, u, nullptr);
    Dispatch(c, kKClassify, (std::max(range, g_d.nArgs) + 255) / 256);
  }
  c->CSSetShaderResources(0, 8, nul);
  c->CSSetUnorderedAccessViews(0, 8, nulU, nullptr);
  saved.Restore(c);
  g_rt.eyeMode[eye] = mode;
  g_runs.fetch_add(1, std::memory_order_relaxed);
}

void RunCheck(int k, int eye, ID3D11Buffer* live) {
  if (!live) return;
  ID3D11DeviceContext* c = g_ctx;
  Params p = BaseParams(eye, g_rt.eyeMode[eye]);
  if (!PutParams(c, p)) return;
  CsState saved;
  saved.Save(c);
  ID3D11ShaderResourceView* nul[8] = {};
  ID3D11UnorderedAccessView* nulU[8] = {};
  c->CSSetShaderResources(0, 8, nul);
  ID3D11Buffer* cbs[3] = {g_o.params, g_o.snap[eye], live};
  c->CSSetConstantBuffers(0, 3, cbs);
  ID3D11UnorderedAccessView* u[8] = {nullptr, g_o.stats[eye].uav, g_o.state.uav, nullptr, nullptr, nullptr, nullptr,
                                     nullptr};
  c->CSSetUnorderedAccessViews(0, 8, u, nullptr);
  Dispatch(c, k, 1);
  c->CSSetUnorderedAccessViews(0, 8, nulU, nullptr);
  saved.Restore(c);
}

// The per-eye statistics into a staging slot (read without waiting later).
void QueueReadback(int eye) {
  for (int i = 0; i < 8; ++i)
    if (g_o.stagingEye[i] < 0) {
      g_ctx->CopyResource(g_o.staging[i], g_o.stats[eye].b);
      g_o.stagingEye[i] = eye;
      g_o.stagingFrame[i] = g_rt.frame;
      return;
    }
}

inline float F(uint32_t u) {
  float f;
  memcpy(&f, &u, 4);
  return f;
}

void PollReadback() {
  for (int i = 0; i < 8; ++i) {
    if (g_o.stagingEye[i] < 0) continue;
    D3D11_MAPPED_SUBRESOURCE m{};
    const HRESULT hr = g_ctx->Map(g_o.staging[i], 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m);
    if (hr == DXGI_ERROR_WAS_STILL_DRAWING) {
      if (g_rt.frame - g_o.stagingFrame[i] > 240) g_o.stagingEye[i] = -1;  // give up on this one
      continue;
    }
    const int eye = g_o.stagingEye[i];
    g_o.stagingEye[i] = -1;
    if (FAILED(hr)) continue;
    uint32_t s[kSCount];
    memcpy(s, m.pData, sizeof(s));
    g_ctx->Unmap(g_o.staging[i], 0);
    if (s[kSCheckRun]) {
      // the chain camera's output size (the view guard) and the G-buffer's render size, used from the next runs
      const float zw0 = F(s[kSZw0]), zw1 = F(s[kSZw1]), gw = F(s[kSGbW]), gh = F(s[kSGbH]);
      if (!g_rt.zwKnown[eye] || g_rt.zw[eye][0] != zw0 || g_rt.zw[eye][1] != zw1) {
        g_rt.zw[eye][0] = zw0;
        g_rt.zw[eye][1] = zw1;
        g_rt.zwKnown[eye] = zw0 > 0 && zw1 > 0;
      }
      if (gw >= 16 && gh >= 16 && gw <= 16384 && gh <= 16384) {
        g_rt.W[eye] = gw;
        g_rt.H[eye] = gh;
      }
    }
    if (s[kSCheckFocus] && eye == 0) {
      g_rt.focusW = F(s[kSFocusW]);
      if (g_rt.W[0] > 0 && !(g_rt.focusW > g_rt.W[0])) g_orderBad.fetch_add(1, std::memory_order_relaxed);
    }
    if (s[kSTotal]) {
      const uint32_t t = s[kSTotal], e = g_rt.totalEst[eye];
      g_rt.totalEst[eye] = std::max(t, e - e / 1024);  // follows growth at once, shrinks slowly
    }
    if (!s[kSClassified]) continue;  // a probe run (render size only): not a census run
    {
      std::lock_guard<std::mutex> lock(g_cenMu);
      Accumulate(g_cen[eye], s);
    }
    const uint32_t err = s[kSErr];
    if ((err & kErrDrop) && g_detailLogged.fetch_add(1, std::memory_order_relaxed) < 6)
      Log("periph census: eye %d run left out, error bits 0x%x (listed %u, range %u; P00 snapshot %.5f / G-buffer "
          "%.5f; render size G-buffer %.0fx%.0f / classified %.0fx%.0f)",
          eye, err, s[kSTotal], g_rt.range, F(s[kSDSnapP00]), F(s[kSDLiveP00]), F(s[kSGbW]), F(s[kSGbH]), g_rt.W[eye],
          g_rt.H[eye]);
  }
}

// ---------------------------------------------------------------------------
// Pass boundaries (render thread, top level)
// ---------------------------------------------------------------------------
// The draw binds DCS's identified lists at VS (mainBuffer and indices among its SRVs).
bool ListsBound(ID3D11DeviceContext* c) {
  ID3D11ShaderResourceView* v[16] = {};
  c->VSGetShaderResources(0, 16, v);
  bool main = false, idx = false;
  for (auto* p : v) {
    if (!p) continue;
    ID3D11Buffer* b = BufOf(p);
    main = main || b == g_d.mainB;
    idx = idx || b == g_d.idx;
  }
  for (auto& p : v) Rel(p);
  return main && idx;
}
// The TerrainContext CB this draw binds (VS b11), AddRef'd; nullptr (counted) when absent or not the identified size.
ID3D11Buffer* BoundCam(ID3D11DeviceContext* c) {
  ID3D11Buffer* cam = nullptr;
  c->VSGetConstantBuffers(11, 1, &cam);
  if (!cam || BufBytes(cam) != g_d.tcBytes) {
    Rel(cam);
    g_noCam.fetch_add(1, std::memory_order_relaxed);
    return nullptr;
  }
  return cam;
}

// Before an instancer draw on the identified arguments (render thread, inside H_Draw).
void PreDraw(ID3D11DeviceContext* c) {
  const int v = g_rt.runView;
  if ((g_rt.armDraw & kArmBegin) && g_rt.pending && (v == 0 || v == 1)) {
    if (!ListsBound(c)) {
      g_otherLists.fetch_add(1, std::memory_order_relaxed);
      return;  // keep waiting for a draw on the identified lists
    }
    g_rt.pending = false;
    g_rt.armDraw = (g_rt.armDraw & ~kArmBegin) | kArmCount;
    ID3D11Buffer* cam = BoundCam(c);
    if (!cam) return;
    PeriphBeginAt(v, cam);
    Rel(g_rt.runCam);
    g_rt.runCam = cam;  // the reference BoundCam took
    g_rt.passActive = true;
    g_rt.passEye = v;
    return;
  }
  if ((g_rt.armDraw & kArmFocus) && (v == 2 || v == 3) && g_rt.focusPending[v - 2]) {
    const int e = v - 2;
    if (!ListsBound(c)) {
      g_otherLists.fetch_add(1, std::memory_order_relaxed);
      return;
    }
    g_rt.focusPending[e] = false;
    g_rt.armDraw &= ~kArmFocus;
    ID3D11Buffer* cam = BoundCam(c);
    if (!cam) return;
    c->CopyResource(g_o.fsnap[e], cam);
    cam->Release();
    g_rt.focusTaken[e] = true;
    return;
  }
  if ((g_rt.armDraw & kArmCount) && !g_rt.passActive) g_lateDraws.fetch_add(1, std::memory_order_relaxed);
}

// The end of the G-buffer pass that holds this run's snapshot: the after-run check on the TerrainContext buffer the
// snapshot draw had bound (its content now = what every draw of this pass used, unless DCS uploaded another camera
// inside the pass: 0x100).
void PassEnd() {
  if (!g_rt.passActive) return;
  const int eye = g_rt.passEye;
  g_rt.passActive = false;
  g_rt.passEye = -1;
  if (g_resReady.load() && (eye == 0 || eye == 1) && g_rt.eyeMode[eye] && g_rt.runCam)
    RunCheck(kKCheckRun, eye, g_rt.runCam);
  Rel(g_rt.runCam);
  if (g_rt.armDraw) g_rt.armDraw = (g_rt.armDraw & ~kArmBegin) | kArmCount;
}

void EndRun() {
  const int v = g_rt.runView;
  g_rt.inRun = false;
  g_rt.runView = -1;
  if (v == 0 || v == 1) {
    PassEnd();  // no pass end seen (direct callers)
    DisarmDraw();
    if (g_rt.pending) {
      g_rt.pending = false;
      g_noDraw.fetch_add(1, std::memory_order_relaxed);
    }
  } else if (v == 2 || v == 3) {
    DisarmDraw();
    const int e = v - 2;
    if (g_resReady.load() && g_rt.eyeMode[e]) {
      // the same eye's focus: DCS's focus projection as its instancer draws bound it
      if (g_rt.focusTaken[e])
        RunCheck(kKCheckFocus, e, g_o.fsnap[e]);
      else
        g_focusNoDraw.fetch_add(1, std::memory_order_relaxed);
      QueueReadback(e);
      g_rt.eyeMode[e] = 0;
    }
    g_rt.focusPending[e] = g_rt.focusTaken[e] = false;
  } else {
    DisarmDraw();
  }
}

void BeginRun(int v) {
  g_rt.inRun = true;
  g_rt.runView = v;
  if (v == 2 || v == 3) {
    const int e = v - 2;
    g_rt.focusTaken[e] = false;
    g_rt.focusPending[e] = g_resReady.load() && g_rt.eyeMode[e] != 0;
    if (g_rt.focusPending[e] && !ArmDraw(kArmFocus)) g_rt.focusPending[e] = false;
    return;
  }
  if (v != 0 && v != 1) return;
  g_rt.eyeMode[v] = 0;
  g_rt.pending = false;
  if (!g_drawLearned.load()) {
    ArmDraw(kArmLearn);
    return;
  }
  // The GPU work waits for the run's first instancer draw (PreDraw): DCS uploads the view's TerrainContext inside
  // the G-buffer pass, after its begin.
  if (!CreateOurs()) return;
  g_rt.pending = ArmDraw(kArmBegin);
}

void BoundaryBody(void* pass, bool begin) {
  if (g_stopping.load(std::memory_order_acquire)) {  // Shutdown: arms out, nothing new
    DisarmDraw();
    DisarmDispatch();
    g_rt.inRun = false;
    g_rt.pending = g_rt.passActive = false;
    Rel(g_rt.runCam);
    return;
  }
  const int k = plod::KindOf(pass);
  if (begin) {
    if (k != plod::kGBuffer && g_rt.inRun) EndRun();
    if (k == plod::kHeight || k == plod::kCascade) {
      if (g_rt.lbOrd) {
        ++g_rt.frame;
        Sweep();
        if (g_resReady.load()) PollReadback();
      }
      g_rt.lbOrd = 0;
      g_rt.view = -1;
    } else if (k == plod::kLights) {
      g_rt.view = g_rt.lbOrd++;
      if (g_drawLearned.load() && !g_dispatchLearned.load()) ArmDispatch();
    } else if (k == plod::kGBuffer && !g_rt.inRun && g_rt.view >= 0) {
      BeginRun(g_rt.view);
    }
  } else {
    if (k == plod::kLights) DisarmDispatch();
    if (k == plod::kGBuffer && g_rt.inRun && g_rt.passActive) PassEnd();
  }
}
void BoundarySeh(void* pass, bool begin) {
  __try {
    BoundaryBody(pass, begin);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    g_fault = true;
  }
}
void OnBoundary(void* pass, bool begin, int depth) {
  if (depth != 0 || !g_ctx || g_fault.load(std::memory_order_relaxed)) return;
  if (GetCurrentThreadId() != ptiming::g_topTid.load(std::memory_order_relaxed)) return;
  BoundarySeh(pass, begin);
}

// ---------------------------------------------------------------------------
// Control (worker thread)
// ---------------------------------------------------------------------------
bool EnsureDevice() {
  if (g_ctx) return true;
  ID3D11Device* dev = shadowinst::g_device;
  if (dev) dev->AddRef();
  else if (sfilt::Ctx* c = sfilt::g_ctx.load()) c->GetDevice(&dev);
  if (!dev) return false;
  g_dev = dev;
  dev->GetImmediateContext(&g_ctx);
  return g_ctx != nullptr;
}
// Tests: a device of their own stands in for DCS's.
void SetDeviceForTest(ID3D11Device* d) {
  if (g_ctx) g_ctx->Release();
  if (g_dev) g_dev->Release();
  g_dev = d;
  g_ctx = nullptr;
  if (d) {
    d->AddRef();
    d->GetImmediateContext(&g_ctx);
  }
}

void OpenGaze() {
  if (g_gazeView.load()) return;
  const ULONGLONG now = GetTickCount64();
  if (g_gazeTry && now - g_gazeTry < 1000) return;
  g_gazeTry = now;
  const std::wstring n = perc_sfx::Name(::dcsvr_perception::gaze_block_name);
  HANDLE h = OpenFileMappingW(FILE_MAP_READ, FALSE, n.c_str());
  if (!h) return;
  void* v = MapViewOfFile(h, FILE_MAP_READ, 0, 0, sizeof(::dcsvr_perception::GazeOut));
  if (!v) {
    CloseHandle(h);
    return;
  }
  g_gazeMap = h;
  g_gazeView = v;
}

void ResetCensus() {
  std::lock_guard<std::mutex> lock(g_cenMu);
  g_cen[0] = g_cen[1] = Census{};
}
void LogCensus(double sec) {
  Census l, r;
  {
    std::lock_guard<std::mutex> lock(g_cenMu);
    l = g_cen[0];
    r = g_cen[1];
    g_cen[0] = g_cen[1] = Census{};
  }
  Log("%s", Line(l, r, sec).c_str());
  if (!l.okRuns && !r.okRuns)
    Log("periph census: state: identified draw %d dispatch %d ready %d, render size %.0fx%.0f / %.0fx%.0f, depth "
        "samples %u, periphery runs %llu (with no instancer draw %llu), draws on other lists %llu, no usable b11 %llu, "
        "arms blocked by other table hooks %llu, re-arms refused %llu, view order bad %llu, fault %d",
        g_drawLearned.load() ? 1 : 0, g_dispatchLearned.load() ? 1 : 0, g_resReady.load() ? 1 : 0, g_rt.W[0],
        g_rt.H[0], g_rt.W[1], g_rt.H[1], g_rt.samples[0], static_cast<unsigned long long>(g_runs.load()),
        static_cast<unsigned long long>(g_noDraw.load()), static_cast<unsigned long long>(g_otherLists.load()),
        static_cast<unsigned long long>(g_noCam.load()), static_cast<unsigned long long>(g_armBlocked.load()),
        static_cast<unsigned long long>(g_rearmRefused.load()), static_cast<unsigned long long>(g_orderBad.load()),
        g_fault.load() ? 1 : 0);
}

void Shutdown() {
  if (g_installed.load()) {
    // The render thread withdraws its arms at its next pass boundary.
    g_stopping = true;
    for (int i = 0; i < 50 && (g_rt.armDraw || g_rt.armDispatch); ++i) Sleep(10);
    g_installed = false;
    ptiming::BoundaryFn me = &OnBoundary;
    ptiming::g_pcBoundary.compare_exchange_strong(me, nullptr);
    Sleep(50);  // a boundary already inside finishes
    g_stopping = false;
  }
  if (g_vt && SlotsKnown()) {
    if (g_vt[g_slotDraw] == reinterpret_cast<void*>(&H_Draw)) d3ds::WriteTablePointer(&g_vt[g_slotDraw], g_origDraw);
    if (g_vt[g_slotVsSrv] == reinterpret_cast<void*>(&H_VsSrv))
      d3ds::WriteTablePointer(&g_vt[g_slotVsSrv], g_origVsSrv);
    if (g_vt[g_slotDisp] == reinterpret_cast<void*>(&H_Dispatch)) d3ds::WriteTablePointer(&g_vt[g_slotDisp], g_origDisp);
  }
  g_rt.armDraw = 0;
  g_rt.armDispatch = false;
  std::lock_guard<std::mutex> lock(g_mu);
  ReleaseOurs();
  ReleaseDcs();
  g_resReady = false;
  g_drawLearned = false;
  g_dispatchLearned = false;
  g_rt = Rt{};
  if (void* v = g_gazeView.exchange(nullptr)) UnmapViewOfFile(v);
  if (g_gazeMap) CloseHandle(g_gazeMap);
  g_gazeMap = nullptr;
  g_gazeTry = 0;
  if (g_ctx) g_ctx->Release();
  if (g_dev) g_dev->Release();
  g_ctx = nullptr;
  g_dev = nullptr;
}

// Every second from the worker: [Suite] PeriphCensus=N (0 off). Installs the boundary callback while N > 0 (not
// during a benchmark), one line every N s; at 0 the last partial window is logged and everything is taken out.
void Tick(int sec, bool benchRunning) {
  static ULONGLONG last = 0;
  if (sec <= 0) {
    if (g_installed.load()) {
      if (last) LogCensus((GetTickCount64() - last) / 1000.0);
      Shutdown();
      Log("periph census: off");
    }
    last = 0;
    return;
  }
  if (!g_installed.load()) {
    if (benchRunning || !EnsureDevice()) return;
    if (!CompileAll()) {
      static bool logged = false;
      if (!logged) Log("periph census: kernel compile failed: %s", g_codeErr.c_str());
      logged = true;
      return;
    }
    ptiming::Install();
    if (!ptiming::g_orig) return;
    ResetCensus();
    g_fault = false;
    ptiming::g_pcBoundary = &OnBoundary;
    g_installed = true;
    last = GetTickCount64();
    Log("periph census: on, one line every %d s (measure only: DCS's lists are read, nothing drawn differently)", sec);
  }
  OpenGaze();
  const ULONGLONG now = GetTickCount64();
  if (now - last >= static_cast<ULONGLONG>(sec) * 1000) {
    LogCensus((now - last) / 1000.0);
    last = now;
  }
}

}  // namespace pcensus
