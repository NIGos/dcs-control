// Periphery instance filter (R38 rank 1). [D3D] PeriphInstFilter, a bitmask, default 0 = nothing installed.
//   1 (variant b, BIT-exact): in the two periphery views, the G-buffer passes do not draw the forest / building
//          instances (IM2 instancer) whose conservative screen box holds no pixel centre.
//   2 (variant a, NEAR, opt-in): nor the instances wholly inside the focus view's fully opaque core (QVFR
//          composite alpha 1) shrunk by a margin. Hidden in the composite; the auto-exposure histogram (it reads
//          the whole left periphery image, R38 §2.5) and DLSS temporal history across saccades still see it.
//   3 = both.
// Measure only: [Suite] PeriphCensus=N (seconds) classifies every periphery instance without changing anything.
//
// Where [V census gdd_*.csv 2026-10-11, R34 §2-§3, R35 §3, VS disassembly of the meta2 copies 2026-10-16]:
// - Each view's IM2 chain (LightsBuffersPass) leaves in DCS's buffers: mainBuffer (64-byte OutputInstanceData),
//   counts[bucket], offsets = exclusive scan of counts, indices[offsets[b] + rank] = instance, and the indirect
//   arguments (8 dwords per entry: fillindirect writes [1] = counts[static[7]] and [7] = static[7] = bucket).
// - Instancer draws (edterrainGraphics41+0xb4518, DrawIndexedInstancedIndirect) read, in every one of the 176
//   instancer VS of the archive: i = indices[offsets[cb0.offsetsBufferIndex] + SV_InstanceID], then
//   mainBuffer[i]; an instance id >= counts[bucket] collapses to a point. The same buffers feed later passes
//   (SimpleFlatShadowPass, TransparentDecalPass), so only the G-buffer may see a filtered list (R35 §3).
// - The view's GBufferPass run (cockpit / main / decal) follows its chain; nothing else runs in between that
//   reads the indices or the arguments.
// Mechanism (in place): at the first instancer draw (on the identified lists) of periphery view v's G-buffer
// passes (LightsBuffersPass ordinal 0 / 1), before DCS's draw, on DCS's immediate context: snapshot the
// TerrainContext CB that draw binds at VS b11 (R45: R44's live census took it at the GBufferPass begin, before
// DCS's own UpdateSubresource of b11 inside the pass [V gdd trace 2026-10-11: one 2176-byte DEFAULT CB, one
// upload as the first event of each GBufferPass], so it classified with whatever camera the previous upload
// left: error bits 0x900 in every run); copy the used index range aside; classify every listed instance; stable
// partition each bucket's range (kept instances first, in order, then the dropped ones, in order) and copy it
// into DCS's index buffer; set InstanceCount = kept in DCS's arguments in place (a UAV on an indirect-arguments
// buffer unbinds nothing). At the end of that G-buffer pass the stock index range is copied back and InstanceCount
// = counts[bucket] written back (the value fillindirect wrote; checked for every entry before the commit); later
// G-buffer passes of the view draw stock lists. Any
// inconsistency found on the GPU (a slot outside its bucket, arguments that do not match counts, the list
// larger than the range, another camera) makes the commit a copy of the stock lists: the frame renders stock.
// Same-frame checks after the run: the TerrainContext the G-buffer used has the VP we classified with and the
// render size we assumed; at the focus view's run end, DCS's own focus projection gives the true opaque core,
// compared with the one used. A failed check latches the bit off and is logged.
// Exactness of (b): an instance whose triangles cover no sample writes nothing (no RT, depth or stencil write,
// no UAV in these PS); removing it from a draw and keeping the others' relative order (stable partition) gives
// the same rasterisation order and bit-identical targets (SV_InstanceID only indexes the list and guards the
// count in all 176 instancer VS; no PS sees it). The conservative bound (Bounds below) covers the
// instance transform, LOD cross-fade, SpeedTree wind bend and leaf shake for ANY wind, airflow and noise input
// (R38R1 report): R = |C.y| + 3 |C.xz| + 4 s r about the pivot (C = the transformed boundsphere centre,
// r its radius, s the transform's norm bound), sqrt(10) s (|c| + r) when the blend flag scales axes. The data
// assumption is DCS's own: the mesh lies in the object's boundsphere (Parse frustum-culls with it).
// [Suite] PeriphInstVerify=N: N seconds of a same-frame proof per periphery instancer draw: the dropped
// instances are drawn again with DCS's VS/PS/state into a scratch depth target (no colour, depth test only
// against "outside the core") under an occlusion query; every query must count 0 samples.
// [Suite] PeriphInstTiming=1: GPU pass timing OFF/ON/OFF/ON/OFF, 5 s each.
//
// Per run (R45): the DrawIndexedInstancedIndirect entry is armed from the periphery run's first GBufferPass begin
// to its end (and in the focus runs until their first instancer draw), re-armed from VSSetShaderResources after
// ExecuteCommandList toggles; never with another module's re-arming hooks in the table (then the run is stock).
// Identification: two one-shot table arms, never with another module's re-arming hooks in the table:
// DrawIndexedInstancedIndirect in a periphery G-buffer run (the arguments buffer, the VS SRVs mainBuffer /
// indices / offsets / counts by the slot order every instancer VS uses, the TerrainContext at VS b11), and
// Dispatch in a LightsBuffersPass (IM2 Parse: u1 = mainBuffer, t3 objects (64 B), t4 objectLods (48 B)).
//
// Included once from main.cpp after fixation_depth.h (uses ptiming, plod, chainfast, gpt, gdd, d3ds, fixdepth).
#pragma once

#include "perception_suffix.h"  // R79: DCSVR_PERCEPTION_SHM_SUFFIX (already in through fixation_depth.h)

namespace pifilt {

// ---------------------------------------------------------------------------
// Configuration (config / suite threads write, render thread reads)
// ---------------------------------------------------------------------------
constexpr uint32_t kBitZero = 1, kBitCore = 2, kBits = 3;
std::atomic<uint32_t> g_cfgMask{0};    // [D3D] PeriphInstFilter (ini)
std::atomic<int> g_override{-1};       // a suite phase's mask (-1: the ini's)
std::atomic<uint32_t> g_mask{0};       // live mask (suite phases toggle it)
std::atomic<uint32_t> g_latched{0};    // bits switched off by a failed check (until the key changes)
std::atomic<bool> g_census{false};     // classify and count (measure only, or together with the filter)
std::atomic<bool> g_verify{false};     // [Suite] PeriphInstVerify running
std::atomic<int> g_marginPx{16};       // [D3D] PeriphInstCoreMargin: core shrink, periphery render pixels
std::atomic<int> g_edgePermille{200};  // [D3D] PeriphInstCoreEdge: QVFR smoothen_focus_view_edges x 1000
std::atomic<int> g_shape{8};           // [D3D] PeriphInstCoreShape: QVFR focus_view_shape (superellipse exponent)
std::atomic<int> g_suiteCensus{0}, g_suiteVerify{0};
std::atomic<bool> g_suiteTiming{false};
std::atomic<bool> g_fault{false};
std::atomic<bool> g_stopping{false};   // Shutdown asks the render thread for stock lists and no arms

// ---------------------------------------------------------------------------
// GPU kernels (one source, several entry points; cs_5_0, vs/ps not needed)
// ---------------------------------------------------------------------------
// Error bits in stats[0]: 0x1f and 0x20 block the commit (same frame); 0x100.. are the after-run checks.
constexpr uint32_t kErrTotal = 0x1, kErrIndex = 0x2, kErrBucket = 0x4, kErrSlot = 0x8, kErrArgs = 0x10,
                   kErrView = 0x20, kErrCommitMask = 0x3f, kErrVp = 0x100, kErrDims = 0x200, kErrCore = 0x400,
                   kErrEye = 0x800;
// stats layout (uint32 x 64, per eye, cleared at the run begin)
enum Stat : int {
  kSErr = 0, kSTotal = 1, kSInst = 2, kSTri = 3,  // 3,4 = 64-bit
  kSZeroI = 5, kSZeroT = 6,                       // 6,7
  kSCoreI = 8, kSCoreT = 9,                       // 9,10 (core and not zero)
  kSZeroNI = 11, kSZeroNT = 12,                   // nominal bound
  kSCoreNI = 14, kSCoreNT = 15,
  kSNear = 17, kSUnmapped = 18, kSKept = 19, kSDropI = 20, kSDropT = 21,  // 21,22
  kSCoreOn = 23, kSCheckRun = 24, kSDeltaG = 25, kSDeltaP = 26, kSCheckFocus = 27, kSZw0 = 28, kSZw1 = 29,
  kSGbW = 30, kSGbH = 31, kSFocusW = 32, kSFocusH = 33,
  // Diagnostics (R45): what exactly a check saw (floats stored as their bits).
  kSDRows = 34,      // KCheckRun: bit q-16 set = row q (16..23) of the G-buffer's TerrainContext differs
  kSDMaxD = 35,      // largest |live - snapshot| over rows 16..23 (float)
  kSDRow = 36,       // its row * 4 + component
  kSDSnapV = 37,     // the snapshot's value there (float)
  kSDLiveV = 38,     // the live value there (float)
  kSDOrtho = 39,     // live row 44.x (orthoCamera; must be 0)
  kSDSnapP00 = 40,   // snapshot P[0][0] (row 16.x; periphery ~0.8, focus ~2.3)
  kSDLiveP00 = 41,   // the G-buffer's (KCheckRun) P[0][0]
  kSDSnapCam = 42,   // 42..44 snapshot cameraPos (row 38.xyz)
  kSDLiveCam = 45,   // 45..47 live cameraPos (KCheckRun)
  kSDFocFlags = 48,  // KCheckFocus: 1 snapshot P ok, 2 focus P ok, 4 row 22 differs, 8 row 23 differs
  kSDFocRel = 49,    // largest relative difference over rows 22..23 (float)
  kSDFocP00 = 50,    // focus P[0][0]
  kSDFocCam = 51,    // 51..53 focus cameraPos
  kSDFirstSlot = 54, // ~(first slot j with error 0x2 / 0x4 / 0x8), 0 = none
  kSDFirstArg = 55,  // ~(first argument entry with error 0x10), 0 = none
  kSDBadPrim = 56,   // instances whose LOD's gPrimCount is implausible (> 4 M or negative): weight 0
  kSArgTriK = 57,    // 57,58: sum over argument entries of IndexCount / 3 x InstanceCount (64-bit)
  kSCount = 64
};

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
RWBuffer<uint> flags : register(u0);
RWBuffer<uint> stats : register(u1);
RWStructuredBuffer<float4> st : register(u2);
RWBuffer<uint> blk : register(u3);
RWBuffer<uint> K : register(u4);
RWBuffer<uint> outA : register(u5);
RWBuffer<uint> outB : register(u6);

#define M_ZERO 1u
#define M_CORE 2u
#define M_CENSUS 4u
#define M_COMMIT 8u
#define M_VERIFY 16u

void Add64(uint at, uint v) {
  if (v == 0) return;
  uint o;
  InterlockedAdd(stats[at], v, o);
  if (o > 0xffffffffu - v) InterlockedAdd(stats[at + 1], 1u);
}

// Rigorous radius about the pivot (see the header comment and the R38R1 report):
// - transform: Y = normalize(cross(Z, X)) * |X| (every instancer VS); s bounds |M v| / |v|;
// - LOD cross-fade: a convex combination of two vertex positions of the mesh (both in the boundsphere);
//   the blend<0 path scales chosen axes by |blend| <= 1;
// - SpeedTree bend: p' = arc(h) + R (p - h y), |arc(h)| <= |h|, R a rotation: |p'| <= |h| + |p.xz|;
// - leaf shake: + (w - p.xz), |w| = |p.xz|: <= 2 |p.xz|;
// - so |final| <= |p.y| + 3 |p.xz|, over the sphere B(C, s r): |C.y| + 3 |C.xz| + 4 s r.
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

)" R"(
groupshared uint gsI[24];

[numthreads(256, 1, 1)]
void KClassify(uint3 dt : SV_DispatchThreadID, uint gi : SV_GroupIndex) {
  if (gi < 24) gsI[gi] = 0;
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
    if (ab < nb && ni != cnts[ab]) {
      err |= 0x10;
      InterlockedMax(stats[55], ~j);
    }
    // the triangles the arguments list (every entry; the G-buffer draws those of its techniques): exact 64-bit
    // (ic / 3) x ni from 16-bit halves
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
  }
  uint keep = 1;
  if (j < total && (err & 0x1) == 0) {
    uint i = idxIn[j];
    if (i >= mainCap) {
      err |= 0x2;
      InterlockedMax(stats[54], ~j);
    } else {
      Inst r = mainB[i];
      uint b = r.a.x & 0xffffffu;
      if (b >= nb) {
        err |= 0x4;
        InterlockedMax(stats[54], ~j);
      } else {
        uint o = offs[b], n = cnts[b];
        if (j < o || j - o >= n) {
          err |= 0x8;
          InterlockedMax(stats[54], ~j);
        }
      }
      uint lodI = r.a.y & 0xffffffu;
      uint tri = 0, ob = 0xffffffffu;
      if (lodI < nLod) {
        // InstancerObjectLod.gPrimCount (int at byte 28). R44 live: a few LODs hold -1 or similar (4.4e12
        // triangles per run); such a weight is counted apart and taken as 0.
        tri = lods[lodI].q1.w;
        if (tri > 4000000u) {
          InterlockedAdd(gsI[14], 1u);
          tri = 0;
        }
        ob = l2o[lodI];
      }
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
        bool drop = ((mode & M_ZERO) != 0 && zR) || ((mode & M_CORE) != 0 && cR);
        if (drop) {
          keep = 0;
          InterlockedAdd(gsI[12], 1u);
          InterlockedAdd(gsI[13], tri);
        }
      }
    }
  }
  if (j < idxCap) flags[j] = j < total ? keep : 0;
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
    InterlockedAdd(stats[20], gsI[12]);
    Add64(21, gsI[13]);
    InterlockedAdd(stats[56], gsI[14]);
  }
}

)" R"(
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

groupshared uint sh[1024];
[numthreads(1024, 1, 1)]
void KScan1(uint3 g : SV_GroupID, uint t : SV_GroupIndex) {
  uint j = g.x * 1024 + t;
  sh[t] = j < idxCap ? flags[j] : 0;
  GroupMemoryBarrierWithGroupSync();
  [unroll] for (uint s = 512; s > 0; s >>= 1) {
    if (t < s) sh[t] += sh[t + s];
    GroupMemoryBarrierWithGroupSync();
  }
  if (t == 0) blk[g.x] = sh[0];
}
[numthreads(1024, 1, 1)]
void KScan2(uint t : SV_GroupIndex) {
  uint nBlk = (idxCap + 1023) / 1024;
  uint v = t < nBlk ? blk[t] : 0;
  sh[t] = v;
  GroupMemoryBarrierWithGroupSync();
  [unroll] for (uint s = 1; s < 1024; s <<= 1) {
    uint a = t >= s ? sh[t - s] : 0;
    GroupMemoryBarrierWithGroupSync();
    sh[t] += a;
    GroupMemoryBarrierWithGroupSync();
  }
  if (t < nBlk) blk[t] = sh[t] - v;
  if (t == 1023) blk[nBlk] = sh[1023];
}
[numthreads(1024, 1, 1)]
void KScan3(uint3 g : SV_GroupID, uint t : SV_GroupIndex) {
  uint j = g.x * 1024 + t;
  uint v = j < idxCap ? flags[j] : 0;
  sh[t] = v;
  GroupMemoryBarrierWithGroupSync();
  [unroll] for (uint s = 1; s < 1024; s <<= 1) {
    uint a = t >= s ? sh[t - s] : 0;
    GroupMemoryBarrierWithGroupSync();
    sh[t] += a;
    GroupMemoryBarrierWithGroupSync();
  }
  if (j < idxCap) K[j] = blk[g.x] + sh[t] - v;
  if (g.x == 0 && t == 0) {
    uint nBlk = (idxCap + 1023) / 1024;
    K[idxCap] = blk[nBlk];
    stats[19] = blk[nBlk];
  }
}
// Stable partition per bucket: kept first, then dropped (outA), and dropped first (outB, verify only).
[numthreads(256, 1, 1)]
void KScatter(uint3 dt : SV_DispatchThreadID) {
  uint j = dt.x;
  if (j >= idxCap) return;
  uint total = offs[nb - 1] + cnts[nb - 1];
  bool bad = (stats[0] & 0x3fu) != 0 || (mode & M_COMMIT) == 0;
  uint v = idxIn[j];
  if (bad || j >= total) {
    outA[j] = v;
    if ((mode & M_VERIFY) != 0) outB[j] = v;
    return;
  }
  uint b = mainB[v].a.x & 0xffffffu;
  uint o = offs[b], n = cnts[b];
  uint kb = K[j] - K[o], kt = K[o + n] - K[o], keep = K[j + 1] - K[j];
  uint dst = keep != 0 ? o + kb : o + kt + (j - o - kb);
  outA[dst] = v;
  if ((mode & M_VERIFY) != 0) {
    uint dr = keep != 0 ? o + (n - kt) + kb : o + (j - o - kb);
    outB[dr] = v;
  }
}
// Arguments, in place in DCS's buffer (outA; an indirect-arguments buffer is never bound as an SRV, so this UAV
// unbinds nothing of DCS's): InstanceCount = kept; the dropped-set copy (outB, verify only).
[numthreads(256, 1, 1)]
void KArgs(uint3 dt : SV_DispatchThreadID) {
  uint e = dt.x;
  if (e >= nEntries) return;
  bool bad = (stats[0] & 0x3fu) != 0 || (mode & M_COMMIT) == 0;
  uint b = outA[e * 8 + 7];
  if ((mode & M_VERIFY) != 0) {
    uint w;
    [unroll] for (w = 0; w < 8; ++w) outB[e * 8 + w] = w == 1 ? 0 : outA[e * 8 + w];
  }
  if (bad || b >= nb) return;
  uint o = offs[b], n = cnts[b];
  uint kt = K[o + n] - K[o];
  outA[e * 8 + 1] = kt;
  if ((mode & M_VERIFY) != 0) outB[e * 8 + 1] = n - kt;
}
// The run end: DCS's InstanceCount back (= counts[bucket], which the run checked for every entry), only when the
// run's commit changed it (no error bit in this eye's stats).
[numthreads(256, 1, 1)]
void KArgsRestore(uint3 dt : SV_DispatchThreadID) {
  uint e = dt.x;
  if (e >= nEntries || (stats[0] & 0x3fu) != 0) return;
  uint b = outA[e * 8 + 7];
  if (b < nb) outA[e * 8 + 1] = cnts[b];
}
)" R"(
// After a periphery run: the TerrainContext the G-buffer used (b2, live) against the snapshot (b1).
[numthreads(1, 1, 1)]
void KCheckRun() {
  uint e = 0, rows = 0, at = 0;
  float md = 0, sv = 0, lv = 0;
  [unroll] for (uint q = 16; q < 24; ++q) {
    if (any(asuint(tl[q]) != asuint(tc[q]))) {
      e |= 0x100;
      rows |= 1u << (q - 16);
    }
    [unroll] for (uint k = 0; k < 4; ++k) {
      float d = abs(tl[q][k] - tc[q][k]);
      if (asuint(tl[q][k]) != asuint(tc[q][k]) && !(d <= md)) {  // NaN counts as the largest
        md = d;
        at = q * 4 + k;
        sv = tc[q][k];
        lv = tl[q][k];
      }
    }
  }
  if (asint(tl[44].x) != 0 || (W > 0 && (tl[44].z != W || tl[44].w != H))) e |= 0x200;
  if (e != 0) InterlockedOr(stats[0], e);
  stats[24] = 1;
  stats[28] = asuint(tc[44].z);
  stats[29] = asuint(tc[44].w);
  stats[30] = asuint(tl[44].z);
  stats[31] = asuint(tl[44].w);
  stats[34] = rows;
  stats[35] = asuint(md);
  stats[36] = at;
  stats[37] = asuint(sv);
  stats[38] = asuint(lv);
  stats[39] = asuint(tl[44].x);
  stats[40] = asuint(tc[16].x);
  stats[41] = asuint(tl[16].x);
  stats[42] = asuint(tc[38].x);
  stats[43] = asuint(tc[38].y);
  stats[44] = asuint(tc[38].z);
  stats[45] = asuint(tl[38].x);
  stats[46] = asuint(tl[38].y);
  stats[47] = asuint(tl[38].z);
}
// After the same eye's focus run: the true core rect from DCS's focus projection (b2) and the periphery
// snapshot (b1); kept for the next frame and compared with the rects used.
[numthreads(1, 1, 1)]
void KCheckFocus() {
  uint e = 0;
  float4 rect = float4(0, 0, 0, 0);
  bool pS = ProjOk(tc[16], tc[17], tc[19]), pF = ProjOk(tl[16], tl[17], tl[19]);
  bool ok = pS && pF;
  uint fl = (pS ? 1u : 0u) | (pF ? 2u : 0u);
  float mr = 0;
  [unroll] for (uint q = 22; q < 24; ++q) {
    float4 d = abs(tl[q] - tc[q]), m = max(abs(tl[q]), abs(tc[q]));
    float4 rel = d / max(m, float4(1, 1, 1, 1));
    mr = max(mr, max(max(rel.x, rel.y), max(rel.z, rel.w)));
    if (any(d > 1e-5 * max(m, float4(1, 1, 1, 1)))) {
      ok = false;
      fl |= q == 22 ? 4u : 8u;
    }
  }
  stats[48] = fl;
  stats[49] = asuint(mr);
  stats[50] = asuint(tl[16].x);
  stats[51] = asuint(tl[38].x);
  stats[52] = asuint(tl[38].y);
  stats[53] = asuint(tl[38].z);
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
  float4 info = st[eye * 6 + 4];
  uint used = (uint)info.x;
  bool coreOn = info.y > 0.5;
  float sx = 0.5 * W, sy = 0.5 * H;
  float4 g = st[eye * 6 + 2], p = st[eye * 6 + 3];
  float dg = max(max(abs(g.x - rect.x), abs(g.y - rect.y)) * sx, max(abs(g.z - rect.z), abs(g.w - rect.w)) * sy);
  float dp = max(max(abs(p.x - rect.x), abs(p.y - rect.y)) * sx, max(abs(p.z - rect.z), abs(p.w - rect.w)) * sy);
  bool fine = (((used & 1u) != 0) && dg <= marginPx - 1) || (((used & 2u) != 0) && dp <= marginPx - 1);
  if (coreOn && (mode & M_CORE) != 0 && (!ok || !fine)) e |= 0x400;
  if (e != 0) InterlockedOr(stats[0], e);
  stats[25] = (used & 1u) != 0 ? (uint)min(dg * 1000, 4e9) : 0xffffffffu;
  stats[26] = (used & 2u) != 0 ? (uint)min(dp * 1000, 4e9) : 0xffffffffu;
  stats[27] = 1;
  stats[32] = asuint(tl[44].z);
  stats[33] = asuint(tl[44].w);
}
)";

// Verify: depth texture filled 2.0 (outside the core: every fragment passes LESS_EQUAL) or -1 (inside).
const char kHlslFill[] = R"(
cbuffer Params : register(b0) {
  uint mode; uint nb; uint idxCap; uint nEntries;
  uint mainCap; uint nObj; uint nLod; uint eye;
  float W; float H; float epsPx; float marginPx;
  float coreR; float shapeP; float tlx; float tly;
};
StructuredBuffer<float4> st : register(t0);
RWTexture2D<float> outT : register(u0);
bool InRect(float x, float y, float4 rect) {
  float hw = 0.5 * (rect.y - rect.x), hh = 0.5 * (rect.w - rect.z);
  if (!(hw > 0 && hh > 0)) return false;
  float u = abs(x - 0.5 * (rect.x + rect.y)) / hw, v = abs(y - 0.5 * (rect.z + rect.w)) / hh;
  if (!(u < coreR && v < coreR)) return false;
  return pow(u / coreR, shapeP) + pow(v / coreR, shapeP) < 1.0;
}
[numthreads(8, 8, 1)]
void KFill(uint3 p : SV_DispatchThreadID) {
  if (p.x >= (uint)W || p.y >= (uint)H) return;
  float x = ((p.x + 0.5) / W) * 2 - 1, y = 1 - ((p.y + 0.5) / H) * 2;
  float4 info = st[eye * 6 + 4];
  uint used = (uint)info.x;
  bool core = (mode & 2u) != 0 && info.y > 0.5 && used != 0;
  if (core && (used & 1u) != 0) core = InRect(x, y, st[eye * 6 + 2]);
  if (core && (used & 2u) != 0) core = InRect(x, y, st[eye * 6 + 3]);
  outT[p.xy] = core ? -1.0 : 2.0;
}
)";

enum Kern { kKClassify, kKLod2Obj, kKScan1, kKScan2, kKScan3, kKScatter, kKArgs, kKArgsRestore, kKCheckRun, kKCheckFocus, kKFill,
            kKernels };
const char* const kKernName[kKernels] = {"KClassify", "KLod2Obj", "KScan1", "KScan2", "KScan3", "KScatter",
                                         "KArgs", "KArgsRestore", "KCheckRun", "KCheckFocus", "KFill"};

bool CompileEntry(const char* src, const char* entry, std::vector<uint8_t>& out, std::string* err) {
  if (!chainfast::LoadCompiler()) {
    if (err) *err = "d3dcompiler_47.dll / D3DCompile not available";
    return false;
  }
  ID3DBlob* code = nullptr;
  ID3DBlob* e = nullptr;
  const HRESULT hr = chainfast::g_compile(src, strlen(src), entry, nullptr, nullptr, entry, "cs_5_0",
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
    if (!CompileEntry(k == kKFill ? kHlslFill : kHlsl, kKernName[k], g_code[k], &g_codeErr)) {
      g_codeErr = std::string(kKernName[k]) + ": " + g_codeErr;
      return false;
    }
  g_codeReady = true;
  return true;
}

// ---------------------------------------------------------------------------
// CPU mirror of the classification (tests, and the census report's reference)
// ---------------------------------------------------------------------------
struct F3 {
  float x, y, z;
};
inline F3 Sub3(F3 a, F3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline float Dot3(F3 a, F3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline float Len3(F3 a) { return std::sqrt(Dot3(a, a)); }
inline F3 Cross3(F3 a, F3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
inline F3 Mul3(F3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
inline F3 Add3(F3 a, F3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }

// Rigorous / nominal radius about the pivot (mirror of Bounds in kHlsl).
inline void BoundsCpu(F3 X, F3 Z, F3 c, float rad, bool scaled, float* rrig, float* rnom) {
  const float lx = Len3(X), lz = Len3(Z);
  const F3 yd = Cross3(Z, X);
  const float ly = Len3(yd);
  const F3 Y = ly > 0 ? Mul3(yd, lx / ly) : F3{0, 0, 0};
  const float cosXZ = (lx > 0 && lz > 0) ? std::fabs(Dot3(X, Z)) / (lx * lz) : 0.f;
  const float s = cosXZ < 1e-3f ? std::max(lx, lz) * 1.002f : std::sqrt(2 * lx * lx + lz * lz) * 1.002f;
  const F3 C = Add3(Add3(Mul3(X, c.x), Mul3(Y, c.y)), Mul3(Z, c.z));
  const float rw = s * std::fabs(rad);
  const float tree = std::fabs(C.y) + 3 * std::sqrt(C.x * C.x + C.z * C.z) + 4 * rw;
  const float flat = 3.1623f * s * (Len3(c) + std::fabs(rad));
  *rrig = scaled ? std::max(tree, flat) : tree;
  *rnom = Len3(C) + rw;
}

// ---------------------------------------------------------------------------
// DCS's resources (identified at run time) and ours
// ---------------------------------------------------------------------------
struct Dcs {
  ID3D11Buffer* args = nullptr;  // indirect arguments (R289)
  ID3D11Buffer* mainB = nullptr; // OutputInstanceData (R277)
  ID3D11Buffer* idx = nullptr;   // indices (R287)
  ID3D11Buffer* offs = nullptr;  // offsets (R279)
  ID3D11Buffer* cnts = nullptr;  // counts (R280)
  ID3D11Buffer* tc = nullptr;    // TerrainContext CB (VS b11)
  ID3D11Buffer* objs = nullptr;  // instancer2_objects (Parse t3)
  ID3D11Buffer* lods = nullptr;  // instancer2_objectLods (Parse t4)
  D3D11_SHADER_RESOURCE_VIEW_DESC idxDesc{};  // DCS's view of the indices (the verify draw's substitute)
  UINT nArgs = 0, mainCap = 0, idxCap = 0, nb = 0, nObj = 0, nLod = 0, tcBytes = 0;
};

struct Ours {
  ID3D11ComputeShader* cs[kKernels] = {};
  ID3D11Buffer* params = nullptr;  // dynamic CB
  ID3D11Buffer* snap[2] = {};      // TerrainContext snapshots per eye (default CB), taken at the first instancer draw
  ID3D11Buffer* fsnap[2] = {};     // the same eye's focus TerrainContext, at the focus view's first instancer draw
  ID3D11ShaderResourceView *srvMain = nullptr, *srvIdx = nullptr, *srvOffs = nullptr, *srvCnts = nullptr,
                           *srvObjs = nullptr, *srvLods = nullptr, *srvArgs = nullptr;
  chainfast::TBuf l2o, flags, K, blk, stats[2], bakIdx, newIdx, rejIdx, rejArgs, state;
  ID3D11UnorderedAccessView* uavArgs = nullptr;  // DCS's arguments, typed R32_UINT (InstanceCount in place)
  ID3D11Buffer* staging[8] = {};
  int stagingEye[8] = {-1, -1, -1, -1, -1, -1, -1, -1};
  uint64_t stagingFrame[8] = {};
  ID3D11ShaderResourceView* srvRejVs = nullptr;  // rejIdx with DCS's view description (verify draw)
  // verify target
  ID3D11Texture2D* vDepth = nullptr;
  ID3D11DepthStencilView* vDsv = nullptr;
  ID3D11Texture2D* vFill = nullptr;
  ID3D11UnorderedAccessView* vFillUav = nullptr;
  ID3D11DepthStencilState* vDs = nullptr;
  UINT vW = 0, vH = 0;
  std::vector<ID3D11Query*> queries;
  std::vector<std::pair<ID3D11Query*, UINT>> qPending;  // query, argument offset
  std::vector<ID3D11Query*> qFree;
};

// Render-thread state.
struct Rt {
  int lbOrd = 0;
  int view = -1;         // LightsBuffersPass ordinal of the current view
  bool inRun = false;    // inside a GBufferPass run
  int runView = -1;
  bool dirty = false;    // DCS's indices / arguments hold our lists
  int dirtyEye = 0;
  bool committed = false;
  uint32_t runMode = 0;  // the current periphery run's mode
  uint32_t eyeMode[2] = {};  // the mode of each eye's run this frame (its focus check uses it)
  float W[2] = {}, H[2] = {}, tlx[2] = {}, tly[2] = {};
  float focusW = 0;
  uint32_t totalEst[2] = {};  // listed instances (the after-run readback), for the slot range processed
  uint32_t range = 0;         // slots processed / copied in the current run
  UINT samples[2] = {};  // the periphery G-buffer depth target's sample count (1 expected)
  bool zwKnown[2] = {};
  float zw[2][2] = {};
  uint64_t frame = 0;
  uint64_t l2oRuns = 0;
  bool l2oValid = false;
  int armDraw = 0;       // kArm* bits: learn, verify, begin (periphery commit), focus (snapshot), count
  bool armDispatch = false;
  int qNext = 0;
  // R45: the run's GPU work happens at its first instancer draw on the identified lists, with the TerrainContext
  // that draw has bound (DCS uploads b11 inside the G-buffer pass, after the pass begin: R44's 0x900).
  bool pending = false;      // a periphery run waits for its first instancer draw
  bool passActive = false;   // the G-buffer pass holding this run's commit / snapshot is open
  int passEye = -1;
  int gbInRun = 0;           // G-buffer passes begun in the current run
  ID3D11Buffer* runCam = nullptr;  // the b11 buffer of the commit draw (AddRef'd until the pass end)
  bool focusPending[2] = {}, focusTaken[2] = {};
};
constexpr int kArmLearn = 1, kArmVerify = 2, kArmBegin = 4, kArmFocus = 8, kArmCount = 16, kArmVsc = 32;
// R38 rank 2 G4 probe (vs_sig_probe.h): while on, every instancer draw of a periphery run on the identified lists is
// reported (view, argument offset) before DCS's draw, and the run end (stock arguments) after.
std::atomic<bool> g_vscOn{false};
void (*g_vscDraw)(ID3D11DeviceContext* c, int view, UINT off) = nullptr;
void (*g_vscEnd)(int view) = nullptr;
// R53 (focus_bcast_run.h, R38 rank 2 broadcast): while g_fbcOn, the instancer draw entries are armed in every view's
// G-buffer run (periphery and focus), DrawInstancedIndirect as well (kind 1; DrawIndexedInstancedIndirect is kind 0),
// on any arguments buffer. g_fbcPre runs before DCS's draw and returns true when the draw must not be issued (a focus
// draw the broadcast already rendered); g_fbcPost runs after it (also after a skipped one). g_fbcBoundary sees every
// top-level pass boundary pifilt sees, after pifilt's own handling, with the view (LightsBuffersPass ordinal, -1
// before the first) and the G-buffer passes begun in the current run.
constexpr int kArmFbc = 64;
std::atomic<bool> g_fbcOn{false};
bool (*g_fbcPre)(ID3D11DeviceContext* c, int kind, ID3D11Buffer* args, UINT off, int view) = nullptr;
void (*g_fbcPost)(ID3D11DeviceContext* c, int kind, ID3D11Buffer* args, UINT off, int view) = nullptr;
void (*g_fbcBoundary)(void* pass, bool begin, int kind, int view, int gbInRun) = nullptr;
std::atomic<uint64_t> g_fbcFaults{0};

ID3D11Device* g_dev = nullptr;
ID3D11DeviceContext* g_ctx = nullptr;
Dcs g_d;
Ours g_o;
Rt g_rt;
std::mutex g_mu;  // g_d / g_o creation vs Shutdown (render thread vs worker)
std::atomic<bool> g_drawLearned{false}, g_dispatchLearned{false}, g_resReady{false}, g_installed{false};
std::atomic<int> g_resFail{0};

// Counters (render thread writes, worker reads)
std::atomic<uint64_t> g_runs{0}, g_commits{0}, g_aborts{0}, g_skipped{0}, g_verDraws{0}, g_verSamples{0},
    g_verQueries{0}, g_verNonZero{0}, g_verLost{0}, g_verSkip{0}, g_learnDraws{0}, g_learnDisp{0}, g_orderBad{0},
    g_armBlocked{0};
// R45 run-timing counters: periphery runs whose passes ended with no instancer draw on the identified lists
// (nothing done: stock), instancer draws on other lists / with no usable b11 (skipped), our lists drawn in a later
// G-buffer pass of the run (stock by then), commit draws whose b11 was not the identified TerrainContext buffer
// (the bound one is used), focus runs with no instancer draw (no focus check).
std::atomic<uint64_t> g_noDraw{0}, g_otherLists{0}, g_noCam{0}, g_lateDraws{0}, g_camOther{0}, g_focusNoDraw{0},
    g_detailLogged{0};
constexpr int kErrBitN = 12;  // per-bit run counts, bits 0x1 .. 0x800
struct Census {
  uint64_t runs = 0, inst = 0, tri = 0, zeroI = 0, zeroT = 0, coreI = 0, coreT = 0, zeroNI = 0, zeroNT = 0,
           coreNI = 0, coreNT = 0, nearI = 0, unmapped = 0, dropI = 0, dropT = 0, coreOn = 0, errRuns = 0;
  uint32_t errBits = 0;
  uint64_t dgSum = 0, dgN = 0, dpSum = 0, dpN = 0;
  uint64_t bitRuns[kErrBitN] = {};
  uint64_t badPrim = 0, argTriK = 0;
};
Census g_cen[2];
std::mutex g_cenMu;

// Gaze (Perception's layer): read on the render thread at a periphery run begin.
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
// A default typed R32_UINT buffer with SRV + UAV.
inline bool MakeTyped(UINT elems, chainfast::TBuf& r) { return chainfast::MakeBuf(g_dev, elems, 0, nullptr, true, true, r); }
// A copy target / source for DCS's buffer: the same size and misc flags (indirect arguments), default usage,
// SRV + UAV as typed R32_UINT.
inline bool MakeLike(ID3D11Buffer* dcs, chainfast::TBuf& r) {
  D3D11_BUFFER_DESC d{};
  dcs->GetDesc(&d);
  D3D11_BUFFER_DESC bd{};
  bd.ByteWidth = d.ByteWidth;
  bd.Usage = D3D11_USAGE_DEFAULT;
  bd.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
  bd.MiscFlags = d.MiscFlags & D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS;
  if (FAILED(g_dev->CreateBuffer(&bd, nullptr, &r.b))) return false;
  r.bytes = d.ByteWidth;
  D3D11_SHADER_RESOURCE_VIEW_DESC v{};
  v.Format = DXGI_FORMAT_R32_UINT;
  v.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
  v.Buffer.NumElements = d.ByteWidth / 4;
  D3D11_UNORDERED_ACCESS_VIEW_DESC u{};
  u.Format = DXGI_FORMAT_R32_UINT;
  u.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
  u.Buffer.NumElements = d.ByteWidth / 4;
  return SUCCEEDED(g_dev->CreateShaderResourceView(r.b, &v, &r.srv)) &&
         SUCCEEDED(g_dev->CreateUnorderedAccessView(r.b, &u, &r.uav));
}

// Foreign hooks that re-arm by taking whatever entry they find (fixation_depth.h's list) or a draw arm of
// fixation depth's own: then the one-shot arms stay out.
inline bool ForeignHooks() {
  return gpt::g_opsHooked.load(std::memory_order_relaxed) || gdd::g_hooked.load(std::memory_order_relaxed) ||
         d3ds::g_vtblCount.load(std::memory_order_relaxed) > 0 || fixdepth::g_drawArmed;
}

// ---------------------------------------------------------------------------
// CS state save / restore around our dispatches (DCS's state cache must find what it set)
// ---------------------------------------------------------------------------
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
  Rel(g_o.srvRejVs);
  Rel(g_o.uavArgs);
  for (chainfast::TBuf* b : {&g_o.l2o, &g_o.flags, &g_o.K, &g_o.blk, &g_o.stats[0], &g_o.stats[1], &g_o.bakIdx,
                             &g_o.newIdx, &g_o.rejIdx, &g_o.rejArgs, &g_o.state})
    b->Release();
  for (auto& s : g_o.staging) Rel(s);
  Rel(g_o.vDepth);
  Rel(g_o.vDsv);
  Rel(g_o.vFill);
  Rel(g_o.vFillUav);
  Rel(g_o.vDs);
  for (auto* q : g_o.queries)
    if (q) q->Release();
  g_o.queries.clear();
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
  ok = ok && MakeTyped(d.nLod, g_o.l2o) && MakeTyped(d.idxCap, g_o.flags) && MakeTyped(d.idxCap + 1, g_o.K) &&
       MakeTyped(1025, g_o.blk) && MakeTyped(kSCount, g_o.stats[0]) && MakeTyped(kSCount, g_o.stats[1]) &&
       MakeLike(d.idx, g_o.bakIdx) && MakeLike(d.idx, g_o.newIdx) && MakeLike(d.idx, g_o.rejIdx) &&
       MakeLike(d.args, g_o.rejArgs) &&
       chainfast::MakeBuf(g_dev, 12, 16, nullptr, true, true, g_o.state);
  // newIdx / bakIdx are copied to / from DCS's buffer: same byte size.
  ok = ok && BufBytes(g_o.newIdx.b) == BufBytes(d.idx);
  if (ok) {
    D3D11_UNORDERED_ACCESS_VIEW_DESC u{};
    u.Format = DXGI_FORMAT_R32_UINT;
    u.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    u.Buffer.NumElements = d.nArgs * 8;
    ok = SUCCEEDED(g_dev->CreateUnorderedAccessView(d.args, &u, &g_o.uavArgs));
  }
  if (ok) {
    D3D11_SHADER_RESOURCE_VIEW_DESC vd = d.idxDesc;
    ok = SUCCEEDED(g_dev->CreateShaderResourceView(g_o.rejIdx.b, &vd, &g_o.srvRejVs));
  }
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
    Log("periph inst filter: creating our buffers / views / kernels failed (%d of 3)%s", f,
        f >= 3 ? "; feature off for the session" : "");
    return false;
  }
  const UINT zero[4] = {};
  g_ctx->ClearUnorderedAccessViewUint(g_o.state.uav, zero);
  g_rt.l2oValid = false;
  g_resReady = true;
  Log("periph inst filter: ready: %u argument entries, %u buckets, indices %u, mainBuffer %u, objects %u, LODs %u, "
      "TerrainContext %u bytes",
      d.nArgs, d.nb, d.idxCap, d.mainCap, d.nObj, d.nLod, d.tcBytes);
  return true;
}

// ---------------------------------------------------------------------------
// One-shot arms (identification) and the verify draw hook
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
// R53: DrawInstancedIndirect (focus broadcast only).
using DrawInstIndFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Buffer*, UINT);
int g_slotDraw2 = -1;
void* volatile g_origDraw2 = nullptr;
std::atomic<uint64_t> g_rearmRefused{0};  // R53: a re-arm that found another hook of this payload in a draw slot

void LearnDraw(ID3D11DeviceContext* c, ID3D11Buffer* args);
void VerifyDraw(ID3D11DeviceContext* c, ID3D11Buffer* args, UINT off);
void LearnDispatch(ID3D11DeviceContext* c);
void PreDraw(ID3D11DeviceContext* c, UINT off);

// Before DCS's draw: a pending periphery run's GPU work (with the TerrainContext this draw binds), the focus
// snapshot, or counting. After it: identification, or the verify redraw.
// R53: the focus broadcast's callbacks around one DCS instancer draw (SEH: a fault turns the callbacks off and the
// draw goes through as DCS's).
bool FbcPre(ID3D11DeviceContext* c, int kind, ID3D11Buffer* b, UINT off) {
  bool skip = false;
  __try {
    skip = g_fbcPre(c, kind, b, off, g_rt.inRun ? g_rt.runView : g_rt.view);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    g_fbcFaults.fetch_add(1, std::memory_order_relaxed);
    g_fbcOn = false;
    skip = false;
  }
  return skip;
}
void FbcPost(ID3D11DeviceContext* c, int kind, ID3D11Buffer* b, UINT off) {
  __try {
    g_fbcPost(c, kind, b, off, g_rt.inRun ? g_rt.runView : g_rt.view);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    g_fbcFaults.fetch_add(1, std::memory_order_relaxed);
    g_fbcOn = false;
  }
}
inline bool FbcArmed(ID3D11DeviceContext* c) {
  return c == g_ctx && !t_inside && (g_rt.armDraw & kArmFbc) && g_fbcPre && g_fbcOn.load(std::memory_order_relaxed);
}
void STDMETHODCALLTYPE H_Draw(ID3D11DeviceContext* c, ID3D11Buffer* b, UINT off) {
  if (c == g_ctx && !t_inside && b && b == g_d.args && (g_rt.armDraw & (kArmBegin | kArmFocus | kArmCount | kArmVsc)) &&
      !g_fault.load(std::memory_order_relaxed)) {
    t_inside = true;
    __try {
      PreDraw(c, off);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
      g_fault = true;
    }
    t_inside = false;
  }
  if (FbcArmed(c)) {  // R53: the focus broadcast (DCS's draw may be skipped: a focus draw already broadcast)
    t_inside = true;
    const bool skip = FbcPre(c, 0, b, off);
    if (!skip) reinterpret_cast<DrawIndFn>(g_origDraw)(c, b, off);
    if (g_fbcPost) FbcPost(c, 0, b, off);
    t_inside = false;
  } else {
    reinterpret_cast<DrawIndFn>(g_origDraw)(c, b, off);
  }
  if (c != g_ctx || t_inside || g_fault.load(std::memory_order_relaxed)) return;
  t_inside = true;
  __try {
    if ((g_rt.armDraw & kArmLearn) && !g_drawLearned.load(std::memory_order_relaxed))
      LearnDraw(c, b);
    else if ((g_rt.armDraw & kArmVerify) && b == g_d.args && g_rt.committed)
      VerifyDraw(c, b, off);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    g_fault = true;
  }
  t_inside = false;
}
// R53: DrawInstancedIndirect, armed only for the focus broadcast.
void STDMETHODCALLTYPE H_Draw2(ID3D11DeviceContext* c, ID3D11Buffer* b, UINT off) {
  if (FbcArmed(c)) {
    t_inside = true;
    const bool skip = FbcPre(c, 1, b, off);
    if (!skip) reinterpret_cast<DrawInstIndFn>(g_origDraw2)(c, b, off);
    if (g_fbcPost) FbcPost(c, 1, b, off);
    t_inside = false;
    return;
  }
  reinterpret_cast<DrawInstIndFn>(g_origDraw2)(c, b, off);
}
// R53: the broadcast's own calls of DCS's draw (its verify redraws), past our hooks: d3d11's entry as last adopted.
void CallOrigDraw(ID3D11DeviceContext* c, int kind, ID3D11Buffer* b, UINT off) {
  if (kind == 0)
    reinterpret_cast<DrawIndFn>(g_origDraw)(c, b, off);
  else
    reinterpret_cast<DrawInstIndFn>(g_origDraw2)(c, b, off);
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
// Verify only: ExecuteCommandList / Flush toggle d3d11's draw entries; the SRV setter (not toggled) puts the
// draw hook back with d3d11's new entry as the original.
void STDMETHODCALLTYPE H_VsSrv(ID3D11DeviceContext* c, UINT s, UINT n, ID3D11ShaderResourceView* const* v) {
  reinterpret_cast<VsSrvFn>(g_origVsSrv)(c, s, n, v);
  if (c != g_ctx || !(g_rt.armDraw & (kArmVerify | kArmBegin | kArmFocus | kArmCount | kArmVsc | kArmFbc)) || !g_vt)
    return;
  void* cur = g_vt[g_slotDraw];
  if (cur != reinterpret_cast<void*>(&H_Draw)) {
    if (d3ds::InPayload(cur)) {  // R53 (R48 rule): never adopt another hook of this payload as d3d11's entry
      g_rearmRefused.fetch_add(1, std::memory_order_relaxed);
    } else {
      g_origDraw = cur;
      d3ds::WriteTablePointer(&g_vt[g_slotDraw], reinterpret_cast<void*>(&H_Draw));
      g_verLost.fetch_add(1, std::memory_order_relaxed);
    }
  }
  if ((g_rt.armDraw & kArmFbc) && g_slotDraw2 >= 0) {  // R53
    void* cur2 = g_vt[g_slotDraw2];
    if (cur2 != reinterpret_cast<void*>(&H_Draw2)) {
      if (d3ds::InPayload(cur2)) {
        g_rearmRefused.fetch_add(1, std::memory_order_relaxed);
      } else {
        g_origDraw2 = cur2;
        d3ds::WriteTablePointer(&g_vt[g_slotDraw2], reinterpret_cast<void*>(&H_Draw2));
      }
    }
  }
}

inline bool SlotsKnown() {
  if (g_slotDraw < 0) {
    g_slotDraw = DcsQv_ContextSlot("DrawIndexedInstancedIndirect");
    g_slotDisp = DcsQv_ContextSlot("Dispatch");
    g_slotVsSrv = DcsQv_ContextSlot("VSSetShaderResources");
    g_slotDraw2 = DcsQv_ContextSlot("DrawInstancedIndirect");  // R53 (optional: only the broadcast arms it)
  }
  return g_slotDraw >= 0 && g_slotDisp >= 0 && g_slotVsSrv >= 0;
}
// The VS SRV setter puts the draw hook back after ExecuteCommandList / Flush toggled d3d11's entry (every arm
// but the one-shot identification).
inline void ArmVsSrv() {
  void* cs = g_vt[g_slotVsSrv];
  if (cs != reinterpret_cast<void*>(&H_VsSrv)) {
    g_origVsSrv = cs;
    d3ds::WriteTablePointer(&g_vt[g_slotVsSrv], reinterpret_cast<void*>(&H_VsSrv));
  }
}
// why: kArm* bits. An arm already in the table takes the new bits.
// R53: DrawInstancedIndirect for the broadcast arm (never over another hook of this payload).
inline void ArmDraw2() {
  if (g_slotDraw2 < 0 || !g_vt) return;
  void* cur = g_vt[g_slotDraw2];
  if (cur == reinterpret_cast<void*>(&H_Draw2)) return;
  if (d3ds::InPayload(cur)) {
    g_rearmRefused.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  g_origDraw2 = cur;
  d3ds::WriteTablePointer(&g_vt[g_slotDraw2], reinterpret_cast<void*>(&H_Draw2));
}
bool ArmDraw(int why) {
  if (!SlotsKnown()) return false;
  if (g_rt.armDraw) {
    g_rt.armDraw |= why;
    if ((why & ~kArmLearn) && g_vt) ArmVsSrv();
    if ((why & kArmFbc) && g_vt) ArmDraw2();
    return true;
  }
  if (ForeignHooks()) {
    g_armBlocked.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  g_vt = *reinterpret_cast<void***>(g_ctx);
  void* cur = g_vt[g_slotDraw];
  if (cur == reinterpret_cast<void*>(&H_Draw)) {  // still ours from an arm whose bits were all served
    g_rt.armDraw = why;
    if (why & ~kArmLearn) ArmVsSrv();
    if (why & kArmFbc) ArmDraw2();
    return true;
  }
  if ((why & kArmFbc) && d3ds::InPayload(cur)) {  // R53 (R48 rule): another hook of this payload holds the slot
    g_rearmRefused.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  g_origDraw = cur;
  g_rt.armDraw = why;
  d3ds::WriteTablePointer(&g_vt[g_slotDraw], reinterpret_cast<void*>(&H_Draw));
  if (why & ~kArmLearn) ArmVsSrv();
  if (why & kArmFbc) ArmDraw2();
  return true;
}
void DisarmDraw() {
  // (the arm's bits may already be 0 while the hook is in the table: PreDraw clears the bit it served)
  if (!g_vt || g_slotDraw < 0) return;
  if (g_vt[g_slotDraw] == reinterpret_cast<void*>(&H_Draw)) d3ds::WriteTablePointer(&g_vt[g_slotDraw], g_origDraw);
  if (g_vt[g_slotVsSrv] == reinterpret_cast<void*>(&H_VsSrv))
    d3ds::WriteTablePointer(&g_vt[g_slotVsSrv], g_origVsSrv);
  if (g_slotDraw2 >= 0 && g_vt[g_slotDraw2] == reinterpret_cast<void*>(&H_Draw2))  // R53
    d3ds::WriteTablePointer(&g_vt[g_slotDraw2], g_origDraw2);
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
// A hook of ours left in the table after its arm ended (another module re-armed over it and then put
// "its original" back): the saved entry goes back.
void Sweep() {
  if (!g_vt || !SlotsKnown()) return;
  if (!g_rt.armDraw && g_vt[g_slotDraw] == reinterpret_cast<void*>(&H_Draw))
    d3ds::WriteTablePointer(&g_vt[g_slotDraw], g_origDraw);
  if (!g_rt.armDraw && g_vt[g_slotVsSrv] == reinterpret_cast<void*>(&H_VsSrv))
    d3ds::WriteTablePointer(&g_vt[g_slotVsSrv], g_origVsSrv);
  if (!g_rt.armDispatch && g_vt[g_slotDisp] == reinterpret_cast<void*>(&H_Dispatch))
    d3ds::WriteTablePointer(&g_vt[g_slotDisp], g_origDisp);
  if (!g_rt.armDraw && g_slotDraw2 >= 0 && g_vt[g_slotDraw2] == reinterpret_cast<void*>(&H_Draw2))  // R53
    d3ds::WriteTablePointer(&g_vt[g_slotDraw2], g_origDraw2);
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
    v[at + 1]->GetDesc(&g_d.idxDesc);
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
    Log("periph inst filter: instancer draw identified: arguments %p (%u entries), VS t%d mainBuffer %p (%u), t%d "
        "indices %p (%u), t%d offsets %p, t%d counts %p (%u buckets), b11 TerrainContext %p (%u bytes)",
        static_cast<void*>(g_d.args), g_d.nArgs, at, static_cast<void*>(g_d.mainB), g_d.mainCap, at + 1,
        static_cast<void*>(g_d.idx), g_d.idxCap, at + 2, static_cast<void*>(g_d.offs), cs,
        static_cast<void*>(g_d.cnts), g_d.nb, static_cast<void*>(g_d.tc), g_d.tcBytes);
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
    Log("periph inst filter: IM2 Parse identified: objects %p (%u), object LODs %p (%u)",
        static_cast<void*>(g_d.objs), g_d.nObj, static_cast<void*>(g_d.lods), g_d.nLod);
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
constexpr uint32_t kMZero = 1, kMCore = 2, kMCensus = 4, kMCommit = 8, kMVerify = 16, kMProbe = 32;

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
  out[0] = f[0];  // left
  out[1] = f[1];  // right
  out[2] = f[2];  // up
  out[3] = f[3];  // down
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
  p.marginPx = static_cast<float>(g_marginPx.load(std::memory_order_relaxed));
  p.coreR = 1.0f - 2.0f * g_edgePermille.load(std::memory_order_relaxed) / 1000.0f;
  p.shapeP = static_cast<float>(g_shape.load(std::memory_order_relaxed));
  p.tlx = g_rt.tlx[eye];
  p.tly = g_rt.tly[eye];
  p.expZwOk = g_rt.zwKnown[eye] ? 1 : 0;
  p.expZw0 = g_rt.zw[eye][0];
  p.expZw1 = g_rt.zw[eye][1];
  return p;
}

inline void Dispatch(ID3D11DeviceContext* c, int k, UINT gx) {
  c->CSSetShader(g_o.cs[k], nullptr, 0);
  c->Dispatch(gx, 1, 1);
}

// The run mode for periphery view `eye` now (0: nothing to do).
uint32_t ModeFor(int eye, uint32_t* maskOut) {
  uint32_t m = g_mask.load(std::memory_order_relaxed) & ~g_latched.load(std::memory_order_relaxed) & kBits;
  // Zero coverage needs one sample per pixel at the centre (no MSAA target seen at the last run's end).
  if (g_rt.samples[eye] != 1) m &= ~kBitZero;
  // The core needs the view order periphery L, R, focus L, R (the focus view renders wider than this one);
  // zero coverage is view-local and needs no order.
  if (!(g_rt.focusW > g_rt.W[eye]) || g_orderBad.load(std::memory_order_relaxed)) m &= ~kBitCore;
  if (maskOut) *maskOut = m;
  const bool census = g_census.load(std::memory_order_relaxed);
  if (!m && !census) return 0;
  uint32_t mode = m | (census ? kMCensus : 0);
  if (m) mode |= kMCommit;
  if (m && g_verify.load(std::memory_order_relaxed)) mode |= kMVerify;
  // Until the after-run check has read back this view's render size (its G-buffer TerrainContext, which is also
  // the snapshot's: R45) once, only the snapshot and that check run: the commit and the classification rely on it.
  if (!g_rt.zwKnown[eye] || !(g_rt.W[eye] > 0) || !(g_rt.H[eye] > 0)) mode = kMProbe;
  return mode;
}

// Periphery view `eye`, at its first instancer draw on the identified lists (R45; R44 took it at the first
// GBufferPass begin, before DCS's own TerrainContext upload of the pass): `cam` is the TerrainContext CB that
// draw has bound at VS b11, so the classification uses exactly the camera DCS's instancer VS will use.
void PeriphBeginAt(int eye, ID3D11Buffer* cam) {
  g_rt.committed = false;
  g_rt.runMode = 0;
  g_rt.eyeMode[eye] = 0;
  if (!cam || !CreateOurs()) return;
  uint32_t m = 0;
  const uint32_t mode = ModeFor(eye, &m);
  if (!mode) return;
  const bool census = (mode & kMCensus) != 0;
  Params p = BaseParams(eye, mode);
  if ((m & kBitCore) || census) p.gazeOk = GazeTangents(eye, p.gazeTan) ? 1 : 0;
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
    g_rt.runMode = g_rt.eyeMode[eye] = mode;
    return;
  }
  // Only the slots in use are processed and copied: the readback's total + 25 % + 8192, whole 1024-slot
  // blocks (the kernels refuse the commit when the list outgrew it; the next run takes the new total).
  {
    uint64_t r = g_rt.totalEst[eye] ? uint64_t(g_rt.totalEst[eye]) * 5 / 4 + 8192 : g_d.idxCap;
    r = (r + 1023) & ~uint64_t(1023);
    g_rt.range = static_cast<uint32_t>(std::min<uint64_t>(r, g_d.idxCap));
    p.idxCap = g_rt.range;
    PutParams(c, p);
  }
  const UINT range = g_rt.range;
  const D3D11_BOX box{0, 0, 0, range * 4, 1, 1};
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
  const bool commit = (mode & kMCommit) != 0;
  if (commit) {
    c->CopySubresourceRegion(g_o.bakIdx.b, 0, 0, 0, 0, g_d.idx, 0, &box);
  }
  ID3D11ShaderResourceView* idxIn = commit ? g_o.bakIdx.srv : g_o.srvIdx;
  ID3D11ShaderResourceView* argsIn = g_o.srvArgs;
  {
    ID3D11ShaderResourceView* s[8] = {g_o.srvMain, idxIn, g_o.srvOffs, g_o.srvCnts, g_o.srvObjs, g_o.srvLods,
                                      g_o.l2o.srv, argsIn};
    c->CSSetShaderResources(0, 8, s);
    ID3D11UnorderedAccessView* u[8] = {g_o.flags.uav, g_o.stats[eye].uav, g_o.state.uav, nullptr, nullptr, nullptr,
                                       nullptr, nullptr};
    c->CSSetUnorderedAccessViews(0, 8, u, nullptr);
    Dispatch(c, kKClassify, (std::max(range, g_d.nArgs) + 255) / 256);
  }
  if (commit) {
    ID3D11UnorderedAccessView* u[8] = {g_o.flags.uav, g_o.stats[eye].uav, g_o.state.uav, g_o.blk.uav, g_o.K.uav,
                                       nullptr, nullptr, nullptr};
    c->CSSetUnorderedAccessViews(0, 8, u, nullptr);
    const UINT nBlk = (range + 1023) / 1024;
    Dispatch(c, kKScan1, nBlk);
    Dispatch(c, kKScan2, 1);
    Dispatch(c, kKScan3, nBlk);
    const bool ver = (mode & kMVerify) != 0;
    ID3D11UnorderedAccessView* u2[8] = {nullptr, g_o.stats[eye].uav, nullptr, nullptr, g_o.K.uav, g_o.newIdx.uav,
                                        ver ? g_o.rejIdx.uav : nullptr, nullptr};
    c->CSSetUnorderedAccessViews(0, 8, nulU, nullptr);
    c->CSSetUnorderedAccessViews(0, 8, u2, nullptr);
    Dispatch(c, kKScatter, (range + 255) / 256);
    c->CSSetShaderResources(7, 1, nul);  // our SRV of the arguments out before their UAV goes in
    ID3D11UnorderedAccessView* u3[8] = {nullptr, g_o.stats[eye].uav, nullptr, nullptr, g_o.K.uav, g_o.uavArgs,
                                        ver ? g_o.rejArgs.uav : nullptr, nullptr};
    c->CSSetUnorderedAccessViews(0, 8, nulU, nullptr);
    c->CSSetUnorderedAccessViews(0, 8, u3, nullptr);
    Dispatch(c, kKArgs, (g_d.nArgs + 255) / 256);
  }
  c->CSSetShaderResources(0, 8, nul);
  c->CSSetUnorderedAccessViews(0, 8, nulU, nullptr);
  if (commit) {
    // Our lists into DCS's buffers (identical copies of stock when the GPU found anything inconsistent).
    c->CopySubresourceRegion(g_d.idx, 0, 0, 0, 0, g_o.newIdx.b, 0, &box);
    g_rt.dirty = true;
    g_rt.dirtyEye = eye;
    g_rt.committed = true;
    g_commits.fetch_add(1, std::memory_order_relaxed);
  }
  saved.Restore(c);
  g_rt.runMode = mode;
  g_rt.eyeMode[eye] = mode;
  g_runs.fetch_add(1, std::memory_order_relaxed);
}

// Tests / direct callers: a run on the identified TerrainContext buffer as it is now; the run's pass is open
// until EndRun / PassEnd.
void PeriphBegin(int eye) {
  PeriphBeginAt(eye, g_d.tc);
  Rel(g_rt.runCam);
  g_rt.runCam = g_d.tc;
  if (g_rt.runCam) g_rt.runCam->AddRef();
  g_rt.passActive = true;
  g_rt.passEye = eye;
}

void RestoreStock() {
  if (!g_rt.dirty) return;
  ID3D11DeviceContext* c = g_ctx;
  const int eye = g_rt.dirtyEye;
  const D3D11_BOX box{0, 0, 0, g_rt.range * 4, 1, 1};
  c->CopySubresourceRegion(g_d.idx, 0, 0, 0, 0, g_o.bakIdx.b, 0, &box);
  CsState saved;
  saved.Save(c);
  ID3D11ShaderResourceView* nul[8] = {};
  ID3D11UnorderedAccessView* nulU[8] = {};
  c->CSSetShaderResources(0, 8, nul);
  c->CSSetUnorderedAccessViews(0, 8, nulU, nullptr);
  PutParams(c, BaseParams(eye, g_rt.eyeMode[eye]));
  c->CSSetConstantBuffers(0, 1, &g_o.params);
  ID3D11ShaderResourceView* s[4] = {nullptr, nullptr, nullptr, g_o.srvCnts};
  c->CSSetShaderResources(0, 4, s);
  ID3D11UnorderedAccessView* u[6] = {nullptr, g_o.stats[eye].uav, nullptr, nullptr, nullptr, g_o.uavArgs};
  c->CSSetUnorderedAccessViews(0, 6, u, nullptr);
  Dispatch(c, kKArgsRestore, (g_d.nArgs + 255) / 256);
  c->CSSetShaderResources(0, 8, nul);
  c->CSSetUnorderedAccessViews(0, 8, nulU, nullptr);
  saved.Restore(c);
  g_rt.dirty = false;
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

void LogLatch(uint32_t bits, uint32_t err, int eye) {
  const uint32_t before = g_latched.fetch_or(bits);
  if ((before & bits) != bits)
    Log("periph inst filter: check failed on eye %d (error bits 0x%x): bit(s) 0x%x switched off until the key "
        "changes",
        eye, err, bits & ~before);
}

inline float F(uint32_t u) {
  float f;
  memcpy(&f, &u, 4);
  return f;
}
// One run's error bits in words, with the values each failed check saw (stats as read back; W/H/zw the values
// the run used). Pure: tested offline.
std::string DescribeErr(const uint32_t* s, float W, float H, bool zwKnown, float zw0, float zw1, uint32_t range,
                        int marginPx) {
  const uint32_t e = s[kSErr];
  std::string o;
  char b[640];
  auto add = [&](const char* t) {
    if (!o.empty()) o += "; ";
    o += t;
  };
  if (e & kErrTotal) {
    sprintf_s(b, "0x1 listed instances %u > slots processed %u", s[kSTotal], range);
    add(b);
  }
  if (e & (kErrIndex | kErrBucket | kErrSlot)) {
    sprintf_s(b, "0x%x (index past mainBuffer 0x2 / bucket past counts 0x4 / slot outside its bucket 0x8) first at slot %u",
              e & (kErrIndex | kErrBucket | kErrSlot), s[kSDFirstSlot] ? ~s[kSDFirstSlot] : 0xffffffffu);
    add(b);
  }
  if (e & kErrArgs) {
    sprintf_s(b, "0x10 InstanceCount != counts[bucket], first at argument entry %u",
              s[kSDFirstArg] ? ~s[kSDFirstArg] : 0xffffffffu);
    add(b);
  }
  if (e & kErrView) {
    sprintf_s(b, "0x20 snapshot renderTargetSize %.0fx%.0f, learned for this view %.0fx%.0f%s", F(s[kSZw0]), F(s[kSZw1]),
              zw0, zw1, zwKnown ? "" : " (not learned)");
    add(b);
  }
  if (e & kErrVp) {
    static const char kC[] = "xyzw";
    const uint32_t at = s[kSDRow];
    sprintf_s(b,
              "0x100 G-buffer TerrainContext rows 16..23 differ from the snapshot (rows mask 0x%02x = bit q-16), largest "
              "|d| %.6g at row %u.%c: snapshot %.6g, G-buffer %.6g; P00 snapshot %.5f / G-buffer %.5f; cameraPos "
              "snapshot (%.3f %.3f %.3f) / G-buffer (%.3f %.3f %.3f)",
              s[kSDRows], F(s[kSDMaxD]), at / 4, kC[at & 3], F(s[kSDSnapV]), F(s[kSDLiveV]), F(s[kSDSnapP00]),
              F(s[kSDLiveP00]), F(s[kSDSnapCam]), F(s[kSDSnapCam + 1]), F(s[kSDSnapCam + 2]), F(s[kSDLiveCam]),
              F(s[kSDLiveCam + 1]), F(s[kSDLiveCam + 2]));
    add(b);
  }
  if (e & kErrDims) {
    sprintf_s(b, "0x200 G-buffer renderTargetSize %.0fx%.0f, classified with %.0fx%.0f; orthoCamera %d", F(s[kSGbW]),
              F(s[kSGbH]), W, H, static_cast<int>(s[kSDOrtho]));
    add(b);
  }
  if (e & kErrEye) {
    const uint32_t f = s[kSDFocFlags];
    sprintf_s(b,
              "0x800 focus view does not match the snapshot's eye: projection form ok snapshot %d / focus %d, row 22 "
              "differs %d, row 23 differs %d (largest relative difference %.3g); P00 snapshot %.5f / focus %.5f; "
              "cameraPos snapshot (%.3f %.3f %.3f) / focus (%.3f %.3f %.3f)",
              (f & 1) ? 1 : 0, (f & 2) ? 1 : 0, (f & 4) ? 1 : 0, (f & 8) ? 1 : 0, F(s[kSDFocRel]), F(s[kSDSnapP00]),
              F(s[kSDFocP00]), F(s[kSDSnapCam]), F(s[kSDSnapCam + 1]), F(s[kSDSnapCam + 2]), F(s[kSDFocCam]),
              F(s[kSDFocCam + 1]), F(s[kSDFocCam + 2]));
    add(b);
  }
  if (e & kErrCore) {
    sprintf_s(b, "0x400 core rect error gaze %.2f px / previous frame %.2f px, limit %d px (margin - 1)",
              s[kSDeltaG] == 0xffffffffu ? -1.0 : s[kSDeltaG] / 1000.0,
              s[kSDeltaP] == 0xffffffffu ? -1.0 : s[kSDeltaP] / 1000.0, marginPx - 1);
    add(b);
  }
  return o;
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
    auto q = [&](int at) { return static_cast<uint64_t>(s[at]) | (static_cast<uint64_t>(s[at + 1]) << 32); };
    if (s[kSCheckRun]) {
      // the chain camera's output size (the view guard) and the G-buffer's render size (DCS's own
      // TerrainContext renderTargetSize; census: equal to the G-buffer viewport), used from the next runs
      float zw0, zw1, gw, gh;
      memcpy(&zw0, &s[kSZw0], 4);
      memcpy(&zw1, &s[kSZw1], 4);
      memcpy(&gw, &s[kSGbW], 4);
      memcpy(&gh, &s[kSGbH], 4);
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
    if (s[kSCheckFocus]) {
      float fw;
      memcpy(&fw, &s[kSFocusW], 4);
      if (eye == 0) {
        g_rt.focusW = fw;
        if (g_rt.W[0] > 0 && !(fw > g_rt.W[0])) g_orderBad.fetch_add(1, std::memory_order_relaxed);
      }
    }
    if (s[kSTotal]) {
      const uint32_t t = s[kSTotal], e = g_rt.totalEst[eye];
      g_rt.totalEst[eye] = std::max(t, e - e / 1024);  // follows growth at once, shrinks slowly
    }
    const uint32_t err = s[kSErr];
    {
      std::lock_guard<std::mutex> lock(g_cenMu);
      Census& c = g_cen[eye];
      ++c.runs;
      c.inst += s[kSInst];
      c.tri += q(kSTri);
      c.zeroI += s[kSZeroI];
      c.zeroT += q(kSZeroT);
      c.coreI += s[kSCoreI];
      c.coreT += q(kSCoreT);
      c.zeroNI += s[kSZeroNI];
      c.zeroNT += q(kSZeroNT);
      c.coreNI += s[kSCoreNI];
      c.coreNT += q(kSCoreNT);
      c.nearI += s[kSNear];
      c.unmapped += s[kSUnmapped];
      c.dropI += s[kSDropI];
      c.dropT += q(kSDropT);
      c.coreOn += s[kSCoreOn] ? 1 : 0;
      if (err) {
        ++c.errRuns;
        c.errBits |= err;
        for (int k = 0; k < kErrBitN; ++k)
          if (err & (1u << k)) ++c.bitRuns[k];
      }
      c.badPrim += s[kSDBadPrim];
      c.argTriK += q(kSArgTriK);
      if (s[kSCheckFocus] && s[kSDeltaG] != 0xffffffffu) {
        c.dgSum += s[kSDeltaG];
        ++c.dgN;
      }
      if (s[kSCheckFocus] && s[kSDeltaP] != 0xffffffffu) {
        c.dpSum += s[kSDeltaP];
        ++c.dpN;
      }
    }
    if (err & kErrCommitMask) g_aborts.fetch_add(1, std::memory_order_relaxed);
    // The first failing runs after a key change, each check with the values it saw.
    if (err && g_detailLogged.fetch_add(1, std::memory_order_relaxed) < 6)
      Log("periph inst filter: eye %d run with error bits 0x%x: %s", eye, err,
          DescribeErr(s, g_rt.W[eye], g_rt.H[eye], g_rt.zwKnown[eye], g_rt.zw[eye][0], g_rt.zw[eye][1], g_rt.range,
                      g_marginPx.load(std::memory_order_relaxed))
              .c_str());
    // After-run checks: the classification ran on a camera / size other than the G-buffer's, or the core was
    // not where the composite puts it. (kErrView alone is the guard doing its job: no latch.)
    if (err & (kErrVp | kErrDims)) LogLatch(kBits, err, eye);
    if (err & (kErrCore | kErrEye)) LogLatch(kBitCore, err, eye);
  }
}

// ---------------------------------------------------------------------------
// Verify (suite phase only): the dropped instances drawn again into a scratch depth target
// ---------------------------------------------------------------------------
bool EnsureVerifyTarget(ID3D11DepthStencilView* dcsDsv) {
  if (!dcsDsv) return false;
  ID3D11Resource* r = nullptr;
  dcsDsv->GetResource(&r);
  ID3D11Texture2D* t = nullptr;
  if (r) r->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&t));
  if (r) r->Release();
  if (!t) return false;
  D3D11_TEXTURE2D_DESC d{};
  t->GetDesc(&d);
  t->Release();
  if (d.SampleDesc.Count != 1) return false;
  if (g_o.vDsv && g_o.vW == d.Width && g_o.vH == d.Height) return true;
  Rel(g_o.vDsv);
  Rel(g_o.vDepth);
  Rel(g_o.vFillUav);
  Rel(g_o.vFill);
  D3D11_TEXTURE2D_DESC td{};
  td.Width = d.Width;
  td.Height = d.Height;
  td.MipLevels = 1;
  td.ArraySize = 1;
  td.Format = DXGI_FORMAT_R32_TYPELESS;
  td.SampleDesc.Count = 1;
  td.Usage = D3D11_USAGE_DEFAULT;
  td.BindFlags = D3D11_BIND_DEPTH_STENCIL;
  if (FAILED(g_dev->CreateTexture2D(&td, nullptr, &g_o.vDepth))) return false;
  D3D11_DEPTH_STENCIL_VIEW_DESC dv{};
  dv.Format = DXGI_FORMAT_D32_FLOAT;
  dv.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
  if (FAILED(g_dev->CreateDepthStencilView(g_o.vDepth, &dv, &g_o.vDsv))) return false;
  td.Format = DXGI_FORMAT_R32_FLOAT;
  td.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
  if (FAILED(g_dev->CreateTexture2D(&td, nullptr, &g_o.vFill))) return false;
  if (FAILED(g_dev->CreateUnorderedAccessView(g_o.vFill, nullptr, &g_o.vFillUav))) return false;
  if (!g_o.vDs) {
    D3D11_DEPTH_STENCIL_DESC ds{};
    ds.DepthEnable = TRUE;
    ds.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    ds.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
    if (FAILED(g_dev->CreateDepthStencilState(&ds, &g_o.vDs))) return false;
  }
  if (g_o.queries.empty())
    for (int i = 0; i < 2048; ++i) {
      D3D11_QUERY_DESC qd{D3D11_QUERY_OCCLUSION, 0};
      ID3D11Query* q = nullptr;
      if (FAILED(g_dev->CreateQuery(&qd, &q))) break;
      g_o.queries.push_back(q);
      g_o.qFree.push_back(q);
    }
  g_o.vW = d.Width;
  g_o.vH = d.Height;
  return !g_o.queries.empty();
}

bool g_fillDone = false;

// The scratch depth: 2.0 where a fragment counts (outside the core), -1 inside it.
void FillVerifyDepth(ID3D11DeviceContext* c, int eye) {
  CsState saved;
  saved.Save(c);
  ID3D11ShaderResourceView* nul[8] = {};
  ID3D11UnorderedAccessView* nulU[8] = {};
  c->CSSetShaderResources(0, 8, nul);
  c->CSSetUnorderedAccessViews(0, 8, nulU, nullptr);
  ID3D11Buffer* cb = g_o.params;
  Params p = BaseParams(eye, g_rt.eyeMode[eye]);
  p.W = static_cast<float>(g_o.vW);
  p.H = static_cast<float>(g_o.vH);
  PutParams(c, p);
  c->CSSetConstantBuffers(0, 1, &cb);
  c->CSSetShaderResources(0, 1, &g_o.state.srv);
  c->CSSetUnorderedAccessViews(0, 1, &g_o.vFillUav, nullptr);
  c->CSSetShader(g_o.cs[kKFill], nullptr, 0);
  c->Dispatch((g_o.vW + 7) / 8, (g_o.vH + 7) / 8, 1);
  c->CSSetShaderResources(0, 8, nul);
  c->CSSetUnorderedAccessViews(0, 8, nulU, nullptr);
  saved.Restore(c);
  c->CopyResource(g_o.vDepth, g_o.vFill);
}

void VerifyDraw(ID3D11DeviceContext* c, ID3D11Buffer* args, UINT off) {
  (void)args;
  if (off % 32) {
    g_verSkip.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  ID3D11ShaderResourceView* v[16] = {};
  c->VSGetShaderResources(0, 16, v);
  int slot = -1;
  for (int i = 0; i < 16 && slot < 0; ++i)
    if (v[i] && BufOf(v[i]) == g_d.idx) slot = i;
  ID3D11RenderTargetView* rtv[8] = {};
  ID3D11DepthStencilView* dsv = nullptr;
  ID3D11DepthStencilState* ds = nullptr;
  UINT ref = 0;
  c->OMGetRenderTargets(8, rtv, &dsv);
  c->OMGetDepthStencilState(&ds, &ref);
  if (slot >= 0 && EnsureVerifyTarget(dsv) && !g_o.qFree.empty()) {
    if (!g_fillDone) {
      FillVerifyDepth(c, g_rt.runView);
      g_fillDone = true;
    }
    ID3D11Query* q = g_o.qFree.back();
    g_o.qFree.pop_back();
    c->VSSetShaderResources(static_cast<UINT>(slot), 1, &g_o.srvRejVs);
    c->OMSetRenderTargetsAndUnorderedAccessViews(0, nullptr, g_o.vDsv, 0, D3D11_KEEP_UNORDERED_ACCESS_VIEWS, nullptr,
                                                 nullptr);
    c->OMSetDepthStencilState(g_o.vDs, 0);
    c->Begin(q);
    reinterpret_cast<DrawIndFn>(g_origDraw)(c, g_o.rejArgs.b, off);
    c->End(q);
    g_o.qPending.emplace_back(q, off);
    // DCS's state back
    c->OMSetDepthStencilState(ds, ref);
    UINT nr = 0;
    for (UINT i = 0; i < 8; ++i)
      if (rtv[i]) nr = i + 1;
    c->OMSetRenderTargetsAndUnorderedAccessViews(nr, rtv, dsv, nr, D3D11_KEEP_UNORDERED_ACCESS_VIEWS, nullptr, nullptr);
    c->VSSetShaderResources(static_cast<UINT>(slot), 1, &v[slot]);
    g_verDraws.fetch_add(1, std::memory_order_relaxed);
  } else {
    g_verSkip.fetch_add(1, std::memory_order_relaxed);
  }
  for (auto& p : rtv) Rel(p);
  Rel(dsv);
  Rel(ds);
  for (auto& p : v) Rel(p);
}

void PollQueries() {
  for (size_t i = 0; i < g_o.qPending.size();) {
    UINT64 n = 0;
    const HRESULT hr = g_ctx->GetData(g_o.qPending[i].first, &n, sizeof(n), D3D11_ASYNC_GETDATA_DONOTFLUSH);
    if (hr == S_FALSE) {
      ++i;
      continue;
    }
    if (hr == S_OK) {
      g_verQueries.fetch_add(1, std::memory_order_relaxed);
      g_verSamples.fetch_add(n, std::memory_order_relaxed);
      if (n) {
        const uint64_t k = g_verNonZero.fetch_add(1, std::memory_order_relaxed);
        if (k < 16)
          Log("periph inst filter verify: dropped instances of the draw at argument offset %u produced %llu samples",
              g_o.qPending[i].second, static_cast<unsigned long long>(n));
      }
    }
    g_o.qFree.push_back(g_o.qPending[i].first);
    g_o.qPending[i] = g_o.qPending.back();
    g_o.qPending.pop_back();
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
void PreDraw(ID3D11DeviceContext* c, UINT off) {
  const int v = g_rt.runView;
  if ((g_rt.armDraw & kArmVsc) && (v == 0 || v == 1) && g_vscDraw) g_vscDraw(c, v, off);
  if ((g_rt.armDraw & kArmBegin) && g_rt.pending && (v == 0 || v == 1)) {
    if (!ListsBound(c)) {
      g_otherLists.fetch_add(1, std::memory_order_relaxed);
      return;  // keep waiting for a draw on the identified lists
    }
    g_rt.pending = false;
    g_rt.armDraw = (g_rt.armDraw & ~kArmBegin) | kArmCount;  // counts our lists drawn after this pass
    ID3D11Buffer* cam = BoundCam(c);
    if (!cam) return;
    if (cam != g_d.tc) g_camOther.fetch_add(1, std::memory_order_relaxed);
    PeriphBeginAt(v, cam);
    Rel(g_rt.runCam);
    g_rt.runCam = cam;  // the reference BoundCam took
    g_rt.passActive = true;
    g_rt.passEye = v;
    if (g_rt.committed && (g_rt.runMode & kMVerify)) ArmDraw(kArmVerify);
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

// The end of the G-buffer pass that holds this run's commit: stock lists back, then the after-run check on the
// TerrainContext buffer the commit draw had bound (its content now = what every draw of this pass used, unless
// DCS uploaded another camera inside the pass: 0x100). Later G-buffer passes of the run draw stock lists.
void PassEnd() {
  if (!g_rt.passActive) return;
  const int eye = g_rt.passEye;
  g_rt.passActive = false;
  g_rt.passEye = -1;
  RestoreStock();
  if (g_resReady.load() && (eye == 0 || eye == 1) && g_rt.eyeMode[eye] && g_rt.runCam)
    RunCheck(kKCheckRun, eye, g_rt.runCam);
  Rel(g_rt.runCam);
  if (g_rt.armDraw) g_rt.armDraw = (g_rt.armDraw & ~(kArmVerify | kArmBegin)) | kArmCount;
}

void EndRun() {
  const int v = g_rt.runView;
  g_rt.inRun = false;
  g_rt.runView = -1;
  if (v == 0 || v == 1) {
    PassEnd();  // no pass end seen (direct callers)
    DisarmDraw();
    RestoreStock();
    if (g_rt.pending) {
      g_rt.pending = false;
      g_noDraw.fetch_add(1, std::memory_order_relaxed);
    }
    if (g_vscOn.load(std::memory_order_relaxed) && g_vscEnd) g_vscEnd(v);  // DCS's arguments are stock again
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
  } else if (v > 3) {
    DisarmDraw();  // R53
  }
}

void BeginRun(int v) {
  g_rt.inRun = true;
  g_rt.runView = v;
  g_rt.gbInRun = 1;
  if (v == 2 || v == 3) {
    const int e = v - 2;
    g_rt.focusTaken[e] = false;
    g_rt.focusPending[e] = g_resReady.load() && g_rt.eyeMode[e] != 0;
    if (g_rt.focusPending[e] && !ArmDraw(kArmFocus)) g_rt.focusPending[e] = false;
    if (g_fbcOn.load(std::memory_order_relaxed)) ArmDraw(kArmFbc);  // R53
    return;
  }
  if (v > 3) {  // R53: the broadcast classifies views itself (sensor views shift the ordinals in F1)
    if (g_fbcOn.load(std::memory_order_relaxed)) ArmDraw(kArmFbc);
    return;
  }
  if (v != 0 && v != 1) return;
  if (g_fbcOn.load(std::memory_order_relaxed)) ArmDraw(kArmFbc);  // R53 (with the learning arm, before it)
  g_fillDone = false;
  g_rt.committed = false;
  g_rt.runMode = 0;
  g_rt.eyeMode[v] = 0;
  g_rt.pending = false;
  if (!g_drawLearned.load()) {
    ArmDraw(kArmLearn);
    return;
  }
  if (g_vscOn.load(std::memory_order_relaxed)) ArmDraw(kArmVsc);
  // The GPU work waits for the run's first instancer draw (PreDraw): DCS uploads the view's TerrainContext
  // inside the G-buffer pass, after its begin.
  if (!CreateOurs() || !ModeFor(v, nullptr)) return;
  g_rt.pending = ArmDraw(kArmBegin);
}

void BoundaryBody(void* pass, bool begin) {
  if (g_stopping.load(std::memory_order_acquire)) {  // Shutdown: stock lists back, arms out, nothing new
    DisarmDraw();
    DisarmDispatch();
    RestoreStock();
    g_rt.inRun = false;
    g_rt.pending = g_rt.passActive = false;
    Rel(g_rt.runCam);
    return;
  }
  const int k = plod::KindOf(pass);
  if (begin) {
    if (k != plod::kGBuffer && g_rt.inRun) EndRun();
    if (k == plod::kHeight || k == plod::kCascade) {
      RestoreStock();
      if (g_rt.lbOrd) {
        ++g_rt.frame;
        Sweep();
        if (g_resReady.load()) {
          PollReadback();
          PollQueries();
        }
      }
      g_rt.lbOrd = 0;
      g_rt.view = -1;
    } else if (k == plod::kLights) {
      RestoreStock();
      g_rt.view = g_rt.lbOrd++;
      if (g_drawLearned.load() && !g_dispatchLearned.load()) ArmDispatch();
    } else if (k == plod::kGBuffer && !g_rt.inRun && g_rt.view >= 0) {
      BeginRun(g_rt.view);
    } else if (k == plod::kGBuffer && g_rt.inRun) {
      ++g_rt.gbInRun;
    }
  } else {
    if (k == plod::kLights) DisarmDispatch();
    if (k == plod::kGBuffer && g_rt.inRun && g_rt.passActive) PassEnd();
  }
  // R53: the focus broadcast's boundary (after pifilt's own: the run's arms are in at a G-buffer begin). Called while
  // the callback is set, also after g_fbcOn went off: an open block closes at its pass end.
  if (auto fb = g_fbcBoundary)
    fb(pass, begin, k, g_rt.inRun ? g_rt.runView : g_rt.view, g_rt.inRun ? g_rt.gbInRun : 0);
}
// SEH around the body (C++ objects inside need their own frame).
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
// Control (config / suite thread)
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
  const std::wstring n = perc_sfx::Name(::dcsvr_perception::gaze_block_name);  // R79: shared rule (perception_suffix.h)
  HANDLE h = OpenFileMappingW(FILE_MAP_READ, FALSE, n.c_str());
  if (!h) return;
  void* v = MapViewOfFile(h, FILE_MAP_READ, 0, 0, sizeof(::dcsvr_perception::GazeOut));
  if (!v) {
    CloseHandle(h);
    return;
  }
  g_gazeMap = h;
  g_gazeView = v;
  Log("periph inst filter: %ls opened (focus FOV per frame for the core hole)", n.c_str());
}

// [D3D] PeriphInstFilter / PeriphInstCoreMargin / PeriphInstCoreEdge / PeriphInstCoreShape and the [Suite] keys.
void ReadConfig(const wchar_t* ini) {
  wchar_t b[32];
  GetPrivateProfileStringW(L"D3D", L"PeriphInstFilter", L"0", b, 32, ini);
  const uint32_t m = static_cast<uint32_t>(wcstoul(b, nullptr, 0)) & kBits;
  if (m != g_cfgMask.load()) {
    g_latched = 0;
    g_detailLogged = 0;
    Log("periph inst filter: [D3D] PeriphInstFilter=%u (1 = zero coverage, BIT; 2 = focus core hole, NEAR; 3 = both)",
        m);
  }
  g_cfgMask = m;
  g_marginPx = std::clamp(static_cast<int>(GetPrivateProfileIntW(L"D3D", L"PeriphInstCoreMargin", 16, ini)), 2, 256);
  g_edgePermille = std::clamp(static_cast<int>(GetPrivateProfileIntW(L"D3D", L"PeriphInstCoreEdge", 200, ini)), 0, 499);
  g_shape = std::clamp(static_cast<int>(GetPrivateProfileIntW(L"D3D", L"PeriphInstCoreShape", 8, ini)), 0, 64);
  g_suiteCensus = std::clamp(static_cast<int>(GetPrivateProfileIntW(L"Suite", L"PeriphCensus", 0, ini)), 0, 600);
  g_suiteTiming = GetPrivateProfileIntW(L"Suite", L"PeriphInstTiming", 0, ini) != 0;
  g_suiteVerify = std::clamp(static_cast<int>(GetPrivateProfileIntW(L"Suite", L"PeriphInstVerify", 0, ini)), 0, 600);
}

void LogCensus(const char* tag);
void LogCounters(const char* tag);
void ResetCensus();
// Every second from the config loop (not during a benchmark). Nothing is installed while the key is 0 and no
// phase runs.
void Apply(bool engineOff) {
  const int ov = g_override.load();
  const uint32_t m = ov >= 0 ? static_cast<uint32_t>(ov) : (engineOff ? 0 : g_cfgMask.load());
  const bool want = m || g_census.load() || g_verify.load() || g_vscOn.load() || g_fbcOn.load();
  if (!g_installed.load()) {
    if (!want) {
      g_mask = 0;
      return;
    }
    if (!EnsureDevice()) return;
    if (!CompileAll()) {
      static bool logged = false;
      if (!logged) Log("periph inst filter: kernel compile failed: %s", g_codeErr.c_str());
      logged = true;
      return;
    }
    ptiming::Install();
    if (!ptiming::g_orig) return;
    ptiming::g_pifBoundary = &OnBoundary;
    g_installed = true;
    Log("periph inst filter: pass boundary callback installed; waiting for the periphery views");
  }
  g_mask = m;
  if ((m & kBitCore) || g_census.load()) OpenGaze();
  // While the key is on outside a suite: one state line and the dropped shares every 60 s.
  static ULONGLONG last = 0;
  const ULONGLONG now = GetTickCount64();
  if (m && ov < 0 && now - last >= 60000) {
    if (last) {
      LogCounters("60 s");
      LogCensus("last 60 s");
      ResetCensus();
    }
    last = now;
  }
}

void LogCensus(const char* tag) {
  std::lock_guard<std::mutex> lock(g_cenMu);
  for (int e = 0; e < 2; ++e) {
    const Census& c = g_cen[e];
    if (!c.runs) {
      Log("  periph inst %s eye %d: no runs read back", tag, e);
      continue;
    }
    const double r = static_cast<double>(c.runs), t = c.tri ? static_cast<double>(c.tri) : 1.0;
    Log("  periph inst %s eye %d: %llu runs, %.0f instances / %.2f M triangles per run; zero coverage %.1f %% of "
        "triangles (%.1f %% inst), core %.1f %% (%.1f %%), with the nominal bound: zero %.1f %%, core %.1f %%; "
        "near-plane %.0f, unmapped %.0f per run; dropped %.1f %% of triangles; core on in %.0f %% of runs; "
        "error runs %llu (bits 0x%x); core rect error gaze %.2f px (%llu), previous frame %.2f px (%llu)",
        tag, e, static_cast<unsigned long long>(c.runs), c.inst / r, c.tri / r / 1e6, 100.0 * c.zeroT / t,
        c.inst ? 100.0 * c.zeroI / c.inst : 0.0, 100.0 * c.coreT / t, c.inst ? 100.0 * c.coreI / c.inst : 0.0,
        100.0 * c.zeroNT / t, 100.0 * c.coreNT / t, c.nearI / r, c.unmapped / r, 100.0 * c.dropT / t,
        100.0 * c.coreOn / r, static_cast<unsigned long long>(c.errRuns), c.errBits,
        c.dgN ? c.dgSum / 1000.0 / c.dgN : -1.0, static_cast<unsigned long long>(c.dgN),
        c.dpN ? c.dpSum / 1000.0 / c.dpN : -1.0, static_cast<unsigned long long>(c.dpN));
    std::string bits;
    for (int k = 0; k < kErrBitN; ++k)
      if (c.bitRuns[k]) {
        char b[48];
        sprintf_s(b, "%s0x%x in %llu", bits.empty() ? "" : ", ", 1u << k, static_cast<unsigned long long>(c.bitRuns[k]));
        bits += b;
      }
    Log("  periph inst %s eye %d: runs per error bit: %s; LODs with an implausible gPrimCount %.1f per run (weight 0); "
        "argument entries list %.3f M triangles per run (IndexCount/3 x InstanceCount, every entry; compare the "
        "periphery GBufferPass IA)",
        tag, e, bits.empty() ? "none" : bits.c_str(), c.badPrim / r, c.argTriK / r / 1e6);
  }
}
void ResetCensus() {
  std::lock_guard<std::mutex> lock(g_cenMu);
  g_cen[0] = g_cen[1] = Census{};
}
void LogCounters(const char* tag) {
  Log("  periph inst filter (%s): mask 0x%x latched 0x%x, identified draw %d dispatch %d ready %d, runs %llu, "
      "commits %llu, aborted by the GPU checks %llu, view order bad %llu, identification arms blocked by other "
      "table hooks %llu (instancer draws seen %llu, Parse dispatches seen %llu), render size %.0fx%.0f / %.0fx%.0f, "
      "focus width %.0f, samples %u, fault %d",
      tag, g_mask.load(), g_latched.load(), g_drawLearned.load() ? 1 : 0, g_dispatchLearned.load() ? 1 : 0,
      g_resReady.load() ? 1 : 0, static_cast<unsigned long long>(g_runs.load()),
      static_cast<unsigned long long>(g_commits.load()), static_cast<unsigned long long>(g_aborts.load()),
      static_cast<unsigned long long>(g_orderBad.load()), static_cast<unsigned long long>(g_armBlocked.load()),
      static_cast<unsigned long long>(g_learnDraws.load()), static_cast<unsigned long long>(g_learnDisp.load()),
      g_rt.W[0], g_rt.H[0], g_rt.W[1], g_rt.H[1], g_rt.focusW, g_rt.samples[0], g_fault.load() ? 1 : 0);
  Log("  periph inst filter (%s): run timing: periphery runs with no instancer draw on the identified lists %llu, "
      "draws skipped (other lists bound %llu, no usable b11 %llu), commit draws whose b11 was not the identified "
      "buffer %llu, our lists drawn in a later G-buffer pass of the run (stock) %llu, focus runs with no instancer "
      "draw %llu",
      tag, static_cast<unsigned long long>(g_noDraw.load()), static_cast<unsigned long long>(g_otherLists.load()),
      static_cast<unsigned long long>(g_noCam.load()), static_cast<unsigned long long>(g_camOther.load()),
      static_cast<unsigned long long>(g_lateDraws.load()), static_cast<unsigned long long>(g_focusNoDraw.load()));
}

// Waits until the identification and the first runs happened (ms).
bool WaitReady(int ms) {
  for (int t = 0; t < ms; t += 100) {
    if (g_resReady.load() && g_rt.zwKnown[0] && g_rt.zwKnown[1]) return true;
    Sleep(100);
  }
  return g_resReady.load();
}

// [Suite] phases, in this order: census (measure only), timing (OFF/ON/OFF/ON/OFF), verify. While a phase
// runs, its mask overrides the ini's (g_override); the config loop's Apply keeps it.
void SuitePhases(std::atomic<uint64_t>& frameCounter) {
  const int cen = g_suiteCensus.load(), ver = g_suiteVerify.load();
  const bool tim = g_suiteTiming.load();
  if (!cen && !ver && !tim) return;
  g_detailLogged = 0;
  g_noDraw = g_otherLists = g_noCam = g_lateDraws = g_camOther = g_focusNoDraw = 0;
  const uint32_t cfg = g_cfgMask.load();
  const uint32_t want = cfg ? cfg : kBitZero;
  if (cen > 0) {
    Log("-- R38 rank 1: periphery instance census, measure only (periph_inst_filter.h, %d s) --", cen);
    g_override = 0;  // nothing filtered while counting
    g_census = true;
    Apply(false);
    if (!WaitReady(15000)) {
      Log("  periph inst census: the instancer buffers were not identified in 15 s; phase skipped");
    } else {
      Sleep(1000);
      ResetCensus();
      Sleep(static_cast<DWORD>(cen) * 1000);
      Sleep(300);
      LogCensus("census");
    }
    LogCounters("census");
    g_census = false;
  }
  if (tim) {
    Log("-- R38 rank 1: periphery instance filter, GPU timing OFF/ON/OFF/ON/OFF (mask 0x%x, 5 x 5 s) --", want);
    g_override = 0;
    g_census = true;
    Apply(false);
    if (!WaitReady(15000)) {
      Log("  periph inst timing: not ready; phase skipped");
    } else {
      for (int i = 0; i < 5; ++i) {
        const bool on = (i & 1) != 0;
        g_override = on ? static_cast<int>(want) : 0;
        g_mask = on ? want : 0;
        Sleep(1500);
        ResetCensus();
        Log("  periph inst timing run %d: %s", i + 1, on ? "ON (filtered lists in the periphery G-buffer)" : "OFF (stock)");
        // light: timestamps only, no context-op hooks (the filter's draw arm stays out of a table other hooks hold)
        gpt::Measure(5000, frameCounter, true);
        LogCensus(on ? "timing ON" : "timing OFF");
      }
    }
    g_census = false;
    LogCounters("timing");
  }
  if (ver > 0) {
    Log("-- R38 rank 1: periphery instance filter, same-frame verify of the dropped set (mask 0x%x, %d s) --", want,
        ver);
    g_override = static_cast<int>(want);
    g_mask = want;
    g_verify = true;
    g_census = true;
    Apply(false);
    if (!WaitReady(15000)) {
      Log("  periph inst verify: not ready; phase skipped");
    } else {
      Sleep(1000);
      g_verDraws = g_verSamples = g_verQueries = g_verNonZero = g_verLost = g_verSkip = 0;
      ResetCensus();
      Sleep(static_cast<DWORD>(ver) * 1000);
      g_verify = false;
      Sleep(1000);  // the last queries come back
      Log("  periph inst verify: %llu dropped-set draws, %llu queries read, %llu samples in total, %llu draws with "
          "samples (must be 0), %llu draws skipped, draw hook re-armed %llu times; verdict %s",
          static_cast<unsigned long long>(g_verDraws.load()), static_cast<unsigned long long>(g_verQueries.load()),
          static_cast<unsigned long long>(g_verSamples.load()), static_cast<unsigned long long>(g_verNonZero.load()),
          static_cast<unsigned long long>(g_verSkip.load()), static_cast<unsigned long long>(g_verLost.load()),
          g_verQueries.load() && !g_verNonZero.load() ? "PASS" : (g_verNonZero.load() ? "FAIL" : "NO DATA"));
      LogCensus("verify");
    }
    g_verify = false;
    g_census = false;
    LogCounters("verify");
  }
  g_override = -1;
  Apply(false);
}

void Shutdown() {
  g_mask = 0;
  g_census = false;
  g_verify = false;
  g_override = -1;
  if (g_installed.load()) {
    // The render thread puts DCS's stock lists back and withdraws its arms at its next pass boundary.
    g_stopping = true;
    for (int i = 0; i < 50 && (g_rt.dirty || g_rt.armDraw || g_rt.armDispatch); ++i) Sleep(10);
    g_installed = false;
    ptiming::BoundaryFn me = &OnBoundary;
    ptiming::g_pifBoundary.compare_exchange_strong(me, nullptr);
    Sleep(50);  // a boundary already inside finishes
    g_stopping = false;
  }
  if (g_vt && SlotsKnown()) {
    if (g_vt[g_slotDraw] == reinterpret_cast<void*>(&H_Draw)) d3ds::WriteTablePointer(&g_vt[g_slotDraw], g_origDraw);
    if (g_vt[g_slotVsSrv] == reinterpret_cast<void*>(&H_VsSrv))
      d3ds::WriteTablePointer(&g_vt[g_slotVsSrv], g_origVsSrv);
    if (g_vt[g_slotDisp] == reinterpret_cast<void*>(&H_Dispatch)) d3ds::WriteTablePointer(&g_vt[g_slotDisp], g_origDisp);
    if (g_slotDraw2 >= 0 && g_vt[g_slotDraw2] == reinterpret_cast<void*>(&H_Draw2))  // R53
      d3ds::WriteTablePointer(&g_vt[g_slotDraw2], g_origDraw2);
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

}  // namespace pifilt
