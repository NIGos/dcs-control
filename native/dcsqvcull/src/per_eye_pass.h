// One geometry pass per eye (periphery + focus together): stage 0, the offline building blocks.
// Design, evidence, gates and the live captures: native/dcsqvcull/PER_EYE_PASS_DESIGN.md.
//
// Nothing here touches DCS's draw path. No hook, no device object, no NvAPI call. What is here:
//  - The exact focus rows. A merged vertex shader needs the focus view's TerrainContext rows 20 / 21 (view-projection
//    x / y) while the PERIPHERY pass runs, before DCS uploads them. They are float(float(s * V) + float(c * W)) of
//    rows DCS shares between both views of an eye and the four projection scalars (sx, cx, sy, cy): byte-exact on
//    every TerrainContext of the R26 census (BuildVpRows; checked against census bytes in the tests).
//  - The DXBC vertex-shader patcher of route S (R120a): a clone that writes a second, full clip position computed
//    with the focus rows by a copy of the shader's own instructions (same opcodes, same order: the arithmetic a
//    separate focus pass would run), plus a viewport-mask output. Outputs get the names NV_XYZW_RIGHT /
//    NV_VIEWPORT_MASK for NvAPI_D3D11_CreateVertexShaderEx's custom semantics. Any shader the patcher cannot prove
//    safe stays stock (a reason per shader).
//  - The census of DCS's own shader cache (Saved Games\DCS\metashaders2\*.meta2, read-only): which vertex shaders
//    read the per-view rows, which can be cloned exactly, which hand a view-dependent value to the pixel shader.
//    [Suite] PerEyePassCensus=1 (default 0: nothing runs, nothing changes) logs it from the suite thread: file reads
//    and CPU work only.
//  - Small pure rules the later stages need: the viewport-mask packing, the two-slice target plan, the merge order.
//  - Stage 1 addition: a computed row of the per-view buffer is accepted when its index is the result of imin / imax
//    with literals in the same straight-line run and the whole range holds no per-view row (DCS: every terrain-family
//    shader reads TerrainContext_surfaceLod[clamp(i, 0, 3)], rows 45-48). Anything else computed stays refused.
//
// Included once from main.cpp (features.inc), last; uses Log and g_dir only (SuitePhase). Header-only, C++17.
#pragma once

namespace pep {

// ---------------------------------------------------------------------------
// The exact focus rows
// ---------------------------------------------------------------------------
// TerrainContext (b11, 2176 B) rows used here: 0-2 = the view matrix's x / y / z(w) rows as DCS multiplies them
// (row 2 is byte-equal to row 23, the clip w row), 16 / 17 = P x / y = (sx, 0, cx, 0) / (0, sy, cy, 0),
// 20 / 21 = VP x / y. Rows 0-2 are byte-equal in both views of an eye (ftwin::SharedRowsEqual).
constexpr int kTcBytes = 2176;
constexpr int kRowV0 = 0, kRowV1 = 1, kRowV2 = 2, kRowPx = 16, kRowPy = 17, kRowVx = 20, kRowVy = 21;

struct FocusP {
  float sx = 0, cx = 0, sy = 0, cy = 0;
};

// float(float(a * b) + float(c * d)): two rounded products, one rounded sum, never fused (the stores are volatile).
inline float MulAdd2(float a, float b, float c, float d) {
  volatile float p = a * b;
  volatile float q = c * d;
  volatile float r = p + q;
  return r;
}
// VP rows x / y from the shared rows and the projection scalars. Census: 128 of 128 components byte-equal over 16
// distinct TerrainContexts (4 views, 2 jitter phases, 3 render sizes); a fused multiply-add matches only 88-118.
inline void BuildVpRows(const float* v0, const float* v1, const float* v2, const FocusP& p, float* x, float* y) {
  for (int k = 0; k < 4; ++k) {
    x[k] = MulAdd2(p.sx, v0[k], p.cx, v2[k]);
    y[k] = MulAdd2(p.sy, v1[k], p.cy, v2[k]);
  }
}
// A jittered projection centre: c0 + 2 j / size (j in pixels, the same in both views of an eye). Census, x axis:
// byte-equal in 8 of 8 (j = -1/16 and 7/16 px, both view types, both eyes). The y axis uses a base-3 sequence whose
// float form the census cannot pin down (2 phases): capture C5 settles it.
inline float JitterCentre(float c0, float jitterPx, float size) {
  volatile float n = 2.0f * jitterPx;
  volatile float d = n / size;
  volatile float r = c0 + d;
  return r;
}
// The four rows a merged shader may read in the focus view (16, 17, 20, 21), 64 bytes, from the periphery's upload.
inline void PredictFocusRows(const uint8_t* tcPeriph, const FocusP& p, uint8_t* out64) {
  float r[16] = {p.sx, 0, p.cx, 0, 0, p.sy, p.cy, 0};
  float v[12];
  memcpy(v, tcPeriph, sizeof(v));
  BuildVpRows(v, v + 4, v + 8, p, r + 8, r + 12);
  memcpy(out64, r, 64);
}
// The prediction against DCS's own focus upload, bytes only (no tolerance: a slice drawn with other bytes is thrown
// away and the focus renders stock). 0 = equal, else 1 + the first row that differs.
inline int VerifyFocusRows(const uint8_t* pred64, const uint8_t* tcFocus) {
  static const int rows[4] = {kRowPx, kRowPy, kRowVx, kRowVy};
  for (int i = 0; i < 4; ++i)
    if (memcmp(pred64 + 16 * i, tcFocus + 16 * rows[i], 16) != 0) return 1 + rows[i];
  return 0;
}

// ---------------------------------------------------------------------------
// Small rules of the later stages
// ---------------------------------------------------------------------------
// Single-pass stereo with independent viewport masks: ONE uint, the low 16 bits = view 0's viewports, the high 16
// bits = view 1's (R120a T5d; the public SDK does not document it).
inline uint32_t PackViewportMasks(uint32_t view0, uint32_t view1) { return (view0 & 0xffffu) | (view1 << 16); }

// The two-slice targets of one eye: slice 0 = periphery (in the top-left corner), slice 1 = focus, both viewports at
// the origin. Arrays keep rasterisation bit-equal; an atlas does not (R120a 3.5: 97 k periphery depth pixels moved).
enum PlanWhy : int { kPlOk = 0, kPlSize, kPlSamples, kPlFormat, kPlCount };
struct SlicePlan {
  int why = kPlSize;
  uint32_t w = 0, h = 0;                // array size
  uint32_t vp0w = 0, vp0h = 0;          // viewport 0, slice 0
  uint32_t vp1w = 0, vp1h = 0;          // viewport 1, slice 1
  uint32_t mask = 0;                    // PackViewportMasks(1, 2)
};
inline SlicePlan PlanSlices(uint32_t wp, uint32_t hp, uint32_t wf, uint32_t hf, uint32_t samplesP, uint32_t samplesF,
                            bool sameFormats) {
  SlicePlan s;
  if (!wp || !hp || !wf || !hf || wp > 16384 || hp > 16384 || wf > 16384 || hf > 16384) return s;
  if (samplesP != 1 || samplesF != 1) return s.why = kPlSamples, s;  // DLSS path only (MSAA: a different pipeline)
  if (!sameFormats) return s.why = kPlFormat, s;
  s.w = wp > wf ? wp : wf, s.h = hp > hf ? hp : hf;
  s.vp0w = wp, s.vp0h = hp, s.vp1w = wf, s.vp1h = hf;
  s.mask = PackViewportMasks(1, 2);
  s.why = kPlOk;
  return s;
}

// Where the merged families sit among the items of DCS's focus main pass decides how the slice comes back without
// changing any depth tie (depth func GREATER_EQUAL: of two equal depths the later draw wins):
//  - all merged items before every other item: copy the slice in, then DCS draws the rest (stock order);
//  - all merged items after every other item: DCS draws the rest, then the slice is composited with the pass's own
//    depth test (a slice texel wins on greater or equal depth, exactly as its draw would have);
//  - mixed: no exact composite exists; the focus pass stays stock.
enum MergeOrder : int { kMoNone = 0, kMoFirst, kMoLast, kMoMixed };
inline MergeOrder ClassifyOrder(const uint8_t* merged, size_t n) {
  size_t first = n, last = 0, count = 0;
  for (size_t i = 0; i < n; ++i)
    if (merged[i]) {
      if (first == n) first = i;
      last = i, ++count;
    }
  if (!count) return kMoNone;
  if (last - first + 1 != count) return kMoMixed;  // other items inside the run
  if (first == 0) return kMoFirst;                 // (all items merged: copy-in, nothing else to draw)
  if (last == n - 1) return kMoLast;
  return kMoMixed;
}

// ---------------------------------------------------------------------------
// DXBC container
// ---------------------------------------------------------------------------
constexpr uint32_t FourCc(char a, char b, char c, char d) {
  return static_cast<uint32_t>(static_cast<uint8_t>(a)) | static_cast<uint32_t>(static_cast<uint8_t>(b)) << 8 |
         static_cast<uint32_t>(static_cast<uint8_t>(c)) << 16 | static_cast<uint32_t>(static_cast<uint8_t>(d)) << 24;
}
inline uint32_t Rd32(const uint8_t* p) {
  uint32_t v;
  memcpy(&v, p, 4);
  return v;
}
inline void Wr32(uint8_t* p, uint32_t v) { memcpy(p, &v, 4); }

inline void Md5Block(uint32_t st[4], const uint8_t* blk) {
  static const uint32_t K[64] = {
      0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
      0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
      0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
      0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
      0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
      0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
      0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
      0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391};
  static const uint8_t S[64] = {7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 5, 9,  14, 20, 5, 9,
                                14, 20, 5, 9,  14, 20, 5, 9,  14, 20, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
                                4, 11, 16, 23, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};
  uint32_t m[16];
  memcpy(m, blk, 64);
  uint32_t a = st[0], b = st[1], c = st[2], d = st[3];
  for (uint32_t i = 0; i < 64; ++i) {
    uint32_t f, g;
    if (i < 16)
      f = (b & c) | (~b & d), g = i;
    else if (i < 32)
      f = (d & b) | (~d & c), g = (5 * i + 1) & 15;
    else if (i < 48)
      f = b ^ c ^ d, g = (3 * i + 5) & 15;
    else
      f = c ^ (b | ~d), g = (7 * i) & 15;
    const uint32_t x = a + f + K[i] + m[g];
    a = d, d = c, c = b;
    b = b + ((x << S[i]) | (x >> (32 - S[i])));
  }
  st[0] += a, st[1] += b, st[2] += c, st[3] += d;
}
// The container's checksum: MD5 over the bytes after the checksum field, with DXBC's own final block (the bit
// count first, `bits / 4 | 1` last, instead of MD5's length suffix).
inline void DxbcChecksum(const uint8_t* blob, size_t size, uint8_t out[16]) {
  uint32_t st[4] = {0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476};
  if (size < 20) {
    memset(out, 0, 16);
    return;
  }
  const uint8_t* p = blob + 20;
  const size_t n = size - 20;
  const size_t full = n / 64, left = n % 64;
  for (size_t i = 0; i < full; ++i) Md5Block(st, p + 64 * i);
  const uint32_t bits = static_cast<uint32_t>(n * 8), bits2 = (bits >> 2) | 1;
  uint8_t blk[64] = {};
  if (left >= 56) {
    memcpy(blk, p + 64 * full, left);
    blk[left] = 0x80;
    Md5Block(st, blk);
    memset(blk, 0, 64);
    Wr32(blk, bits);
    Wr32(blk + 60, bits2);
    Md5Block(st, blk);
  } else {
    Wr32(blk, bits);
    if (left) memcpy(blk + 4, p + 64 * full, left);
    blk[4 + left] = 0x80;
    Wr32(blk + 60, bits2);
    Md5Block(st, blk);
  }
  memcpy(out, st, 16);
}

struct Chunk {
  uint32_t fourcc = 0, off = 0, size = 0;  // off: the chunk's data
};
// The declared size of a DXBC container at b (0: not one).
inline uint32_t ContainerSize(const uint8_t* b, size_t avail) {
  if (avail < 32 || memcmp(b, "DXBC", 4) != 0) return 0;
  const uint32_t one = Rd32(b + 20), size = Rd32(b + 24), n = Rd32(b + 28);
  if (one != 1 || size < 32 || size > (8u << 20) || size > avail || n == 0 || n > 64 || 32 + 4ull * n > size) return 0;
  return size;
}
inline bool ParseContainer(const uint8_t* b, size_t avail, std::vector<Chunk>& out) {
  out.clear();
  const uint32_t size = ContainerSize(b, avail);
  if (!size) return false;
  const uint32_t n = Rd32(b + 28);
  for (uint32_t i = 0; i < n; ++i) {
    const uint32_t o = Rd32(b + 32 + 4 * i);
    if (o < 32 + 4 * n || o + 8ull > size) return false;
    Chunk c;
    c.fourcc = Rd32(b + o), c.size = Rd32(b + o + 4), c.off = o + 8;
    if (c.off + static_cast<uint64_t>(c.size) > size) return false;
    out.push_back(c);
  }
  return true;
}
inline bool ChecksumOk(const uint8_t* b, size_t size) {
  uint8_t sum[16];
  DxbcChecksum(b, size, sum);
  return memcmp(sum, b + 4, 16) == 0;
}
struct ChunkData {
  uint32_t fourcc = 0;
  std::vector<uint8_t> data;
};
inline std::vector<uint8_t> BuildContainer(const std::vector<ChunkData>& chunks) {
  size_t size = 32 + 4 * chunks.size();
  for (const ChunkData& c : chunks) size += 8 + ((c.data.size() + 3) & ~size_t{3});
  std::vector<uint8_t> b(size, 0);
  memcpy(b.data(), "DXBC", 4);
  Wr32(b.data() + 20, 1);
  Wr32(b.data() + 24, static_cast<uint32_t>(size));
  Wr32(b.data() + 28, static_cast<uint32_t>(chunks.size()));
  size_t o = 32 + 4 * chunks.size();
  for (size_t i = 0; i < chunks.size(); ++i) {
    const size_t padded = (chunks[i].data.size() + 3) & ~size_t{3};
    Wr32(b.data() + 32 + 4 * i, static_cast<uint32_t>(o));
    Wr32(b.data() + o, chunks[i].fourcc);
    Wr32(b.data() + o + 4, static_cast<uint32_t>(padded));
    if (!chunks[i].data.empty()) memcpy(b.data() + o + 8, chunks[i].data.data(), chunks[i].data.size());
    o += 8 + padded;
  }
  DxbcChecksum(b.data(), b.size(), b.data() + 4);
  return b;
}

// ---------------------------------------------------------------------------
// Shader tokens (d3d11TokenizedProgramFormat)
// ---------------------------------------------------------------------------
enum Op : uint32_t {
  kOpBreak = 2, kOpBreakc = 3, kOpCall = 4, kOpCallc = 5, kOpCase = 6, kOpContinue = 7, kOpContinuec = 8, kOpCut = 9,
  kOpDefault = 10, kOpDiscard = 13, kOpElse = 18, kOpEmit = 19, kOpEmitThenCut = 20, kOpEndif = 21, kOpEndloop = 22,
  kOpEndswitch = 23, kOpIf = 31, kOpImax = 36, kOpImin = 37, kOpImul = 38, kOpLabel = 44, kOpLoop = 48, kOpCustomData = 53, kOpMov = 54,
  kOpNop = 58, kOpRet = 62, kOpRetc = 63, kOpSwitch = 76, kOpSincos = 77, kOpUdiv = 78, kOpUmul = 81,
  kOpDclResource = 88, kOpDclConstantBuffer = 89, kOpDclOutput = 101, kOpDclOutputSgv = 102, kOpDclOutputSiv = 103,
  kOpDclTemps = 104, kOpDclIndexableTemp = 105, kOpDclGlobalFlags = 106, kOpLod = 108, kOpReserved1 = 112,
  kOpHsDecls = 113, kOpHsJoinPhase = 116, kOpEmitStream = 117, kOpEmitThenCutStream = 119, kOpInterfaceCall = 120,
  kOpUaddc = 132, kOpUsubb = 133, kOpSwapc = 142, kOpDclStream = 143, kOpDclResourceStructured = 162,
  kOpLdUavTyped = 163, kOpStoreUavTyped = 164, kOpLdRaw = 165, kOpStoreRaw = 166, kOpLdStructured = 167,
  kOpStoreStructured = 168, kOpAtomicAnd = 169, kOpSync = 187, kOpDadd = 188,
};
enum OperandType : uint32_t {
  kOtTemp = 0, kOtInput = 1, kOtOutput = 2, kOtIndexableTemp = 3, kOtImm32 = 4, kOtImm64 = 5, kOtConstantBuffer = 8,
  kOtNull = 13,
};
inline bool IsDeclaration(uint32_t op) {
  return (op >= kOpDclResource && op <= kOpDclGlobalFlags) || (op >= kOpDclStream && op <= kOpDclResourceStructured) ||
         op == kOpCustomData;
}
// Destination operands of a code instruction a vertex shader may hold (they come first). -1: not handled (the shader
// stays stock): subroutines and interfaces, geometry / hull / compute-only instructions, writes to memory, doubles.
inline int DestCount(uint32_t op) {
  switch (op) {
    case kOpBreak: case kOpBreakc: case kOpCase: case kOpContinue: case kOpContinuec: case kOpDefault:
    case kOpDiscard: case kOpElse: case kOpEndif: case kOpEndloop: case kOpEndswitch: case kOpIf: case kOpLoop:
    case kOpNop: case kOpRet: case kOpRetc: case kOpSwitch:
      return 0;
    case kOpImul: case kOpSincos: case kOpUdiv: case kOpUmul: case kOpUaddc: case kOpUsubb: case kOpSwapc:
      return 2;
    case kOpCall: case kOpCallc: case kOpLabel: case kOpCut: case kOpEmit: case kOpEmitThenCut: case kOpCustomData:
    case kOpInterfaceCall: case kOpStoreUavTyped: case kOpStoreRaw: case kOpStoreStructured: case kOpReserved1:
      return -1;
    default:
      break;
  }
  if (op <= 87) return 1;                               // add .. xor (the rest of the D3D10 arithmetic)
  if (op >= kOpLod && op <= 111) return 1;              // lod, gather4, sample_pos, sample_info
  if (op >= 121 && op <= 131) return 1;                 // bufinfo, deriv_*, gather4_*, rcp, f32tof16, f16tof32
  if (op >= 134 && op <= 141) return 1;                 // countbits .. bfrev
  if (op == kOpLdUavTyped || op == kOpLdRaw || op == kOpLdStructured) return 1;
  return -1;                                            // hull phases, streams, atomics, sync, doubles, D3D11.1+
}

struct Opnd {
  uint32_t len = 0, type = 0, dim = 0, ncomp = 0, selMode = 0, sel = 0;
  uint32_t rep[3] = {}, imm[3] = {}, sub[3] = {};  // per index: representation, the immediate's / nested operand's offset
};
inline bool DecodeOperand(const uint32_t* t, uint32_t n, Opnd& o, int depth = 0) {
  o = Opnd();
  if (!n || depth > 3) return false;
  const uint32_t tok = t[0];
  uint32_t p = 1;
  o.ncomp = tok & 3, o.selMode = (tok >> 2) & 3, o.sel = (tok >> 4) & 0xff, o.type = (tok >> 12) & 0xff;
  o.dim = (tok >> 20) & 3;
  for (bool ext = (tok >> 31) != 0; ext; ++p) {
    if (p >= n) return false;
    ext = (t[p] >> 31) != 0;
  }
  if (o.type == kOtImm32 || o.type == kOtImm64) {
    uint32_t c = o.ncomp == 1 ? 1u : o.ncomp == 2 ? 4u : 0u;
    if (!c || o.dim) return false;
    if (o.type == kOtImm64) c *= 2;
    p += c;
    if (p > n) return false;
    o.len = p;
    return true;
  }
  for (uint32_t d = 0; d < o.dim; ++d) {
    const uint32_t rep = (tok >> (22 + 3 * d)) & 7;
    o.rep[d] = rep;
    if (rep == 0 || rep == 3) o.imm[d] = p, p += 1;
    if (rep == 1 || rep == 4) o.imm[d] = p, p += 2;
    if (rep > 4 || p > n) return false;
    if (rep >= 2) {
      Opnd s;
      if (p >= n || !DecodeOperand(t + p, n - p, s, depth + 1)) return false;
      o.sub[d] = p, p += s.len;
    }
  }
  if (p > n) return false;
  o.len = p;
  return true;
}
// Components a source operand reads (conservative: every component a swizzle names).
inline uint32_t SrcComps(const Opnd& o) {
  if (o.ncomp != 2) return 0xf;
  if (o.selMode == 0) return o.sel & 0xf ? o.sel & 0xf : 0xf;
  if (o.selMode == 1) return 1u << (o.sel & 3) | 1u << ((o.sel >> 2) & 3) | 1u << ((o.sel >> 4) & 3) | 1u << ((o.sel >> 6) & 3);
  if (o.selMode == 2) return 1u << (o.sel & 3);
  return 0xf;
}
struct Ins {
  uint32_t pos = 0, len = 0, op = 0, first = 0;  // first: the first operand's offset
};
inline bool DecodeIns(const uint32_t* t, uint32_t n, uint32_t pos, Ins& i) {
  if (pos >= n) return false;
  const uint32_t tok = t[pos];
  i.pos = pos, i.op = tok & 0x7ff;
  if (i.op == kOpCustomData) {
    if (pos + 1 >= n) return false;
    i.len = t[pos + 1], i.first = i.len;
    return i.len >= 2 && i.len <= n - pos;
  }
  i.len = (tok >> 24) & 0x7f, i.first = 1;
  if (!i.len || i.len > n - pos) return false;
  for (bool ext = (tok >> 31) != 0; ext; ++i.first) {
    if (i.first >= i.len) return false;
    ext = (t[pos + i.first] >> 31) != 0;
  }
  return true;
}

// ---------------------------------------------------------------------------
// The vertex-shader clone (route S, exact variant)
// ---------------------------------------------------------------------------
// A per-view row of the constant buffer and the components that differ between the views of an eye.
struct ViewRow {
  uint32_t row = 0, comps = 0xf;
};
// TerrainContext (R123 census): P x / y, VP x / y, inverse P x / y, the four side planes, the render size (z, w of
// row 44; x is the fixed-distance flag every instancer shader tests, y its distance: shared), row 132's pair.
constexpr ViewRow kTcViewRows[] = {{16, 0xf}, {17, 0xf}, {20, 0xf}, {21, 0xf}, {24, 0xf}, {25, 0xf},
                                   {28, 0xf}, {29, 0xf}, {30, 0xf}, {31, 0xf}, {44, 0xc}, {132, 0x3}};
struct PatchOpts {
  uint32_t cb = 11;                      // the buffer that holds the per-view rows
  const ViewRow* rows = kTcViewRows;
  uint32_t nRows = sizeof(kTcViewRows) / sizeof(kTcViewRows[0]);
  uint32_t preferSlot = 13;              // where the focus rows are injected (first free slot when declared)
  const char* posName = "NV_XYZW_RIGHT";
  const char* maskName = "NV_VIEWPORT_MASK";
};
enum PatchWhy : int {
  kPwOk = 0,
  kPwContainer,     // not a DXBC container, or its chunks are malformed
  kPwNotVs,         // no SHDR / SHEX of a vertex shader
  kPwSignature,     // no usable output signature (OSGN), or no SV_Position in it
  kPwNoViewRows,    // the position does not depend on a per-view row (nothing to clone; drawn once per view as now)
  kPwOpcode,        // an instruction this patcher does not handle (PatchResult::badOp)
  kPwDynamicRow,    // the per-view buffer is indexed by a computed value
  kPwFlow,          // a branch, loop exit or return depends on a per-view value
  kPwDest,          // a per-view value is written somewhere a clone cannot mirror (indexable temp, depth, ...)
  kPwNoSlot,        // no free constant-buffer slot / output register
  kPwTokens,        // malformed instruction stream
  kPwCount
};
inline const char* PatchWhyName(int w) {
  static const char* const n[kPwCount] = {"cloned",        "not a container", "not a vertex shader", "output signature",
                                          "no per-view row", "instruction",   "computed row index",  "per-view branch",
                                          "per-view write", "no free slot",   "malformed tokens"};
  return w >= 0 && w < kPwCount ? n[w] : "?";
}
struct PatchResult {
  int why = kPwContainer;
  uint32_t badOp = 0;
  std::vector<uint8_t> blob;         // the clone (why == kPwOk)
  // Outputs other than the position that depend on a per-view row (bit = register). Single-pass stereo shares every
  // attribute but the position between its views: a clone with such outputs is exact only when the pixel shader
  // rebuilds them for its own view (design 4.3). 0 = the clone alone is exact.
  uint32_t viewVaryings = 0;
  uint32_t posReg = 0, pos2Reg = 0, maskReg = 0, slot = 0;
  std::vector<uint32_t> rows;        // injected buffer: row i = the focus view's row rows[i]; row rows.size() = the mask (x)
  uint32_t ins = 0, twinned = 0, twinTemps = 0;
  uint32_t boundedRows = 0;          // computed row reads proven to stay on rows both views share
};

struct Sig {
  bool ok = false;
  uint32_t count = 0, posReg = 0xffffffff, maxReg = 0;
};
inline Sig ReadOsgn(const uint8_t* d, uint32_t size, const PatchOpts& o) {
  Sig s;
  if (size < 8) return s;
  s.count = Rd32(d);
  if (s.count > 64 || 8 + 24ull * s.count > size) return s;
  for (uint32_t i = 0; i < s.count; ++i) {
    const uint8_t* e = d + 8 + 24 * i;
    const uint32_t name = Rd32(e), sysval = Rd32(e + 8), reg = Rd32(e + 16);
    if (name >= size) return s;
    const size_t room = size - name;
    const char* str = reinterpret_cast<const char*>(d + name);
    if (!memchr(str, 0, room)) return s;
    if (!strcmp(str, o.posName) || !strcmp(str, o.maskName)) return s;  // already a clone
    if (sysval == 1) s.posReg = reg;  // D3D_NAME_POSITION
    if (reg != 0xffffffff && reg > s.maxReg) s.maxReg = reg;
  }
  s.ok = s.posReg < 32;
  return s;
}
// The output signature with the two new elements appended (float4 pos2 at pos2Reg, uint mask.x at maskReg).
inline std::vector<uint8_t> AppendOsgn(const uint8_t* d, uint32_t size, uint32_t count, uint32_t pos2Reg,
                                       uint32_t maskReg, const PatchOpts& o) {
  const uint32_t oldTable = 8 + 24 * count, newTable = oldTable + 48, shift = 48;
  // Strings end at the last NUL (the chunk is padded with 0xAB bytes after it).
  uint32_t strEnd = size;
  while (strEnd > oldTable && d[strEnd - 1] != 0) --strEnd;
  const size_t n1 = strlen(o.posName) + 1, n2 = strlen(o.maskName) + 1;
  std::vector<uint8_t> out(newTable + (strEnd - oldTable) + n1 + n2, 0);
  Wr32(out.data(), count + 2);
  Wr32(out.data() + 4, 8);
  for (uint32_t i = 0; i < count; ++i) {
    memcpy(out.data() + 8 + 24 * i, d + 8 + 24 * i, 24);
    Wr32(out.data() + 8 + 24 * i, Rd32(d + 8 + 24 * i) + shift);
  }
  memcpy(out.data() + newTable, d + oldTable, strEnd - oldTable);
  const uint32_t name1 = newTable + (strEnd - oldTable), name2 = name1 + static_cast<uint32_t>(n1);
  memcpy(out.data() + name1, o.posName, n1);
  memcpy(out.data() + name2, o.maskName, n2);
  uint8_t* e = out.data() + 8 + 24 * count;
  Wr32(e, name1), Wr32(e + 4, 0), Wr32(e + 8, 0), Wr32(e + 12, 3 /*float32*/), Wr32(e + 16, pos2Reg);
  e[20] = 0xf, e[21] = 0x0;   // mask xyzw, every component written
  e += 24;
  Wr32(e, name2), Wr32(e + 4, 0), Wr32(e + 8, 0), Wr32(e + 12, 1 /*uint32*/), Wr32(e + 16, maskReg);
  e[20] = 0x1, e[21] = 0xe;   // mask x; y, z, w never written
  return out;
}

// Two things are tracked. `state`: which components of which temp register may hold a per-view value AT THIS POINT
// of the program (the compiler reuses registers for unrelated values, so this must follow the program order and its
// branches; it decides whether an instruction reads a per-view value). `tainted`: the registers that receive a
// per-view value anywhere; every write to one of those is repeated on its twin register, so a twin always mirrors
// its register (equal where the value is shared, the focus view's where it is per view) on every path.
struct Scan {
  const PatchOpts* o = nullptr;
  uint32_t posReg = 0;
  std::vector<uint8_t> state;    // per temp register: component bits
  std::vector<uint8_t> tainted;  // per temp register
  uint32_t outTaint = 0;
  int fail = kPwOk;
  uint32_t badOp = 0;
  // The pixel-shader clone selects per-view rows instead of repeating instructions: a branch on a per-view value or
  // a per-view write anywhere is fine there. Only the computed-row proof and the instruction walk are wanted.
  bool select = false;
  // Per temp component (register * 4 + component): a proven range of its signed integer value. Set only by imin /
  // imax with a literal and forgotten at every other write and at every flow-control instruction except the entry
  // of an `if` arm (an `if` writes nothing: what holds before it holds at the start of both arms). So a range is
  // only ever used on a straight run from the instruction that made it, possibly into nested `if` arms.
  std::vector<int32_t> lo, hi;
  uint32_t boundedRows = 0;      // computed row reads accepted because their range holds no per-view row
  void ForgetRanges() {
    lo.assign(state.size() * 4, INT32_MIN);
    hi.assign(state.size() * 4, INT32_MAX);
  }
  uint32_t RowComps(uint32_t row) const {
    for (uint32_t i = 0; i < o->nRows; ++i)
      if (o->rows[i].row == row) return o->rows[i].comps;
    return 0;
  }
  uint32_t Temp(uint32_t r) const { return r < state.size() ? state[r] : 0u; }
};
// Does the operand (a source, or any operand's index expression) read a per-view value?
inline bool ReadsView(Scan& s, const uint32_t* t, const Opnd& o, bool asSource) {
  bool v = false;
  for (uint32_t d = 0; d < o.dim; ++d)
    if (o.sub[d]) {
      Opnd sub;
      if (!DecodeOperand(t + o.sub[d], 64, sub)) {
        s.fail = kPwTokens;
        return false;
      }
      v |= ReadsView(s, t + o.sub[d], sub, true);
    }
  if (!asSource) return v;
  if (o.type == kOtTemp && o.dim == 1 && o.rep[0] == 0) return v || (s.Temp(t[o.imm[0]]) & SrcComps(o)) != 0;
  if (o.type == kOtConstantBuffer && o.dim == 2 && o.rep[0] == 0 && t[o.imm[0]] == s.o->cb) {
    if (o.rep[1] != 0) {
      // A computed row. Exact only when the index provably stays on rows both views share (DCS: the four
      // TerrainContext_surfaceLod rows, indexed by a value clamped to 0..3) and is not itself a per-view value.
      bool shared = false;
      Opnd sub;
      if ((o.rep[1] == 2 || o.rep[1] == 3) && !v && o.sub[1] && DecodeOperand(t + o.sub[1], 64, sub) &&
          sub.type == kOtTemp && sub.dim == 1 && sub.rep[0] == 0 && sub.ncomp == 2 && sub.selMode == 2 &&
          !(t[o.sub[1]] >> 31)) {
        const size_t at = static_cast<size_t>(t[o.sub[1] + sub.imm[0]]) * 4 + (sub.sel & 3);
        const uint32_t base = o.rep[1] == 3 ? t[o.imm[1]] : 0u;
        if (at < s.lo.size() && s.lo[at] >= 0 && s.hi[at] >= s.lo[at] && s.hi[at] <= 4096 && base <= 4096) {
          shared = true;
          for (int32_t r = s.lo[at]; r <= s.hi[at]; ++r)
            if (s.RowComps(base + static_cast<uint32_t>(r))) shared = false;
        }
      }
      if (shared)
        ++s.boundedRows;
      else
        s.fail = kPwDynamicRow;
      return v;
    }
    return v || (s.RowComps(t[o.imm[1]]) & SrcComps(o)) != 0;
  }
  return v;
}

struct Code {
  std::vector<uint32_t> tok;   // the SHDR / SHEX chunk as tokens
  std::vector<Ins> ins;
  uint32_t firstCode = 0;      // index in ins of the first non-declaration
  uint32_t temps = 0;
  int dclTemps = -1;           // index in ins
  uint32_t cbSlots = 0;        // declared constant-buffer slots (bits)
  uint32_t maxOut = 0;
};
inline int ReadCode(const uint8_t* d, uint32_t size, Code& c, uint32_t program = 1) {
  if (size < 8 || size % 4) return kPwTokens;
  c.tok.resize(size / 4);
  memcpy(c.tok.data(), d, size);
  if ((c.tok[0] >> 16) != program) return kPwNotVs;  // program type: 1 vertex shader, 0 pixel shader
  const uint32_t n = c.tok[1];
  if (n < 2 || n > c.tok.size()) return kPwTokens;
  c.tok.resize(n);
  bool code = false;
  for (uint32_t p = 2; p < n;) {
    Ins i;
    if (!DecodeIns(c.tok.data(), n, p, i)) return kPwTokens;
    const uint32_t* t = c.tok.data() + p;
    if (IsDeclaration(i.op)) {
      if (code && i.op != kOpCustomData) return kPwTokens;  // a declaration after code: not fxc's layout
      Opnd o;
      if (i.op == kOpDclTemps && i.len == 2) c.dclTemps = static_cast<int>(c.ins.size()), c.temps = t[1];
      if (i.op == kOpDclConstantBuffer) {
        if (!DecodeOperand(t + i.first, i.len - i.first, o) || o.dim != 2 || o.rep[0] != 0) return kPwTokens;
        const uint32_t slot = t[i.first + o.imm[0]];
        if (slot < 32) c.cbSlots |= 1u << slot;
      }
      if (i.op == kOpDclOutput || i.op == kOpDclOutputSgv || i.op == kOpDclOutputSiv) {
        if (!DecodeOperand(t + i.first, i.len - i.first, o)) return kPwTokens;
        if (o.type == kOtOutput && o.dim == 1 && o.rep[0] == 0 && t[i.first + o.imm[0]] > c.maxOut)
          c.maxOut = t[i.first + o.imm[0]];
      }
      if (code) return kPwOpcode;  // an immediate constant buffer between instructions
    } else if (!code) {
      code = true;
      c.firstCode = static_cast<uint32_t>(c.ins.size());
    }
    c.ins.push_back(i);
    p += i.len;
  }
  if (!code) return kPwTokens;
  return kPwOk;
}

// The per-view analysis, in program order. Branches: both arms start from the state at the `if`, the state after
// `endif` is their union. Loops and switches: inside, a write never clears a component (the state only grows) and a
// loop body is repeated until the state at its end stops growing, so the state at every point covers every path
// that reaches it. A branch, loop exit or return that depends on a per-view value: no clone (the two views would
// take different paths).
inline void Analyze(const Code& c, Scan& s) {
  enum { kIf = 1, kLoop = 2, kSwitch = 3 };
  struct Frame {
    int kind = 0;
    size_t start = 0;
    bool hasElse = false;
    std::vector<uint8_t> saved, thenEnd;
    std::vector<int32_t> loAtIf, hiAtIf;  // the proven ranges at the `if` (they hold at the start of both arms)
  };
  std::vector<Frame> stack;
  int weak = 0;
  uint32_t steps = 0;
  s.ForgetRanges();
  // The range of one component of a source operand of imin / imax: a literal, or a temp whose range is known.
  const auto srcRange = [&s](const uint32_t* t, const Opnd& o, uint32_t comp, int32_t* lo, int32_t* hi) {
    *lo = INT32_MIN, *hi = INT32_MAX;
    if (t[0] >> 31) return;  // a modifier (negate, absolute value)
    if (o.type == kOtImm32) {
      const uint32_t n = o.ncomp == 1 ? 1u : 4u;
      *lo = *hi = static_cast<int32_t>(t[o.len - n + (n == 1 ? 0 : comp)]);
    } else if (o.type == kOtTemp && o.dim == 1 && o.rep[0] == 0 && o.ncomp == 2) {
      const uint32_t c2 = o.selMode == 1 ? (o.sel >> (2 * comp)) & 3 : o.selMode == 2 ? o.sel & 3 : comp;
      const size_t at = static_cast<size_t>(t[o.imm[0]]) * 4 + c2;
      if (at < s.lo.size()) *lo = s.lo[at], *hi = s.hi[at];
    }
  };
  for (size_t k = c.firstCode; k < c.ins.size() && s.fail == kPwOk; ++k) {
    if (++steps > 4000000) {
      s.fail = kPwTokens;
      return;
    }
    const Ins& i = c.ins[k];
    const uint32_t* t = c.tok.data() + i.pos;
    const int dests = DestCount(i.op);
    if (dests < 0) {
      s.fail = kPwOpcode, s.badOp = i.op;
      return;
    }
    bool reads = false;
    Opnd o;
    int idx = 0;
    for (uint32_t p = i.first; p < i.len; p += o.len, ++idx) {
      if (!DecodeOperand(t + p, i.len - p, o)) {
        s.fail = kPwTokens;
        return;
      }
      reads |= ReadsView(s, t + p, o, idx >= dests);
    }
    if (s.fail != kPwOk) return;
    if (dests == 0) {
      if (reads && !s.select) {
        s.fail = kPwFlow;
        return;
      }
      // Ranges: an `if` does not write, so what is proven before it holds at the start of its `then` arm and of
      // its `else` arm. Every other flow instruction forgets them (after `endif`, in and after loops, switches).
      const std::vector<int32_t> loBefore = i.op == kOpIf ? s.lo : std::vector<int32_t>();
      const std::vector<int32_t> hiBefore = i.op == kOpIf ? s.hi : std::vector<int32_t>();
      if (i.op != kOpIf) s.ForgetRanges();
      const bool top = !stack.empty();
      switch (i.op) {
        case kOpIf:
          stack.emplace_back();
          stack.back().kind = kIf, stack.back().saved = s.state;
          stack.back().loAtIf = loBefore, stack.back().hiAtIf = hiBefore;
          break;
        case kOpElse:
          if (!top || stack.back().kind != kIf || stack.back().hasElse) {
            s.fail = kPwTokens;
            return;
          }
          stack.back().hasElse = true, stack.back().thenEnd = s.state;
          s.state = stack.back().saved;
          s.lo = stack.back().loAtIf, s.hi = stack.back().hiAtIf;
          break;
        case kOpEndif: {
          if (!top || stack.back().kind != kIf) {
            s.fail = kPwTokens;
            return;
          }
          const std::vector<uint8_t>& other = stack.back().hasElse ? stack.back().thenEnd : stack.back().saved;
          for (size_t r = 0; r < s.state.size(); ++r) s.state[r] |= other[r];
          stack.pop_back();
          break;
        }
        case kOpLoop:
          stack.emplace_back();
          stack.back().kind = kLoop, stack.back().start = k, stack.back().saved = s.state;
          ++weak;
          break;
        case kOpEndloop:
          if (!top || stack.back().kind != kLoop) {
            s.fail = kPwTokens;
            return;
          }
          if (s.state != stack.back().saved) {  // it grew inside: once more from the loop's first instruction
            stack.back().saved = s.state;
            k = stack.back().start;
          } else {
            stack.pop_back();
            --weak;
          }
          break;
        case kOpSwitch:
          stack.emplace_back();
          stack.back().kind = kSwitch;
          ++weak;
          break;
        case kOpEndswitch:
          if (!top || stack.back().kind != kSwitch) {
            s.fail = kPwTokens;
            return;
          }
          stack.pop_back();
          --weak;
          break;
        default:
          break;
      }
      continue;
    }
    // imin / imax: the result's range per component, from the sources as they are before this instruction writes.
    int32_t nlo[4], nhi[4];
    for (int q = 0; q < 4; ++q) nlo[q] = INT32_MIN, nhi[q] = INT32_MAX;
    if ((i.op == kOpImin || i.op == kOpImax) && !(c.tok[i.pos] >> 31)) {
      Opnd od, oa, ob;
      const uint32_t pa = i.first + (DecodeOperand(t + i.first, i.len - i.first, od) ? od.len : i.len);
      if (pa < i.len && DecodeOperand(t + pa, i.len - pa, oa) && pa + oa.len < i.len &&
          DecodeOperand(t + pa + oa.len, i.len - pa - oa.len, ob))
        for (uint32_t q = 0; q < 4; ++q) {
          int32_t al, ah, bl, bh;
          srcRange(t + pa, oa, q, &al, &ah);
          srcRange(t + pa + oa.len, ob, q, &bl, &bh);
          const bool mn = i.op == kOpImin;
          nlo[q] = mn ? (al < bl ? al : bl) : (al > bl ? al : bl);
          nhi[q] = mn ? (ah < bh ? ah : bh) : (ah > bh ? ah : bh);
        }
    }
    idx = 0;
    for (uint32_t p = i.first; p < i.len && idx < dests; p += o.len, ++idx) {
      DecodeOperand(t + p, i.len - p, o);
      if (o.type == kOtNull) continue;
      if (o.type == kOtTemp && o.dim == 1 && o.rep[0] == 0) {
        const uint32_t r = t[p + o.imm[0]];
        if (r >= s.state.size()) {
          s.fail = kPwTokens;
          return;
        }
        const bool masked = o.ncomp == 2 && o.selMode == 0;
        const uint8_t m = static_cast<uint8_t>(masked ? o.sel & 0xf : 0xf);
        for (uint32_t q = 0; q < 4; ++q)
          if (m >> q & 1) s.lo[r * 4 + q] = masked ? nlo[q] : INT32_MIN, s.hi[r * 4 + q] = masked ? nhi[q] : INT32_MAX;
        if (reads)
          s.state[r] |= m, s.tainted[r] = 1;
        else if (!weak && masked)
          s.state[r] &= static_cast<uint8_t>(~m);
      } else if (o.type == kOtOutput && o.dim == 1 && o.rep[0] == 0 && t[p + o.imm[0]] < 32) {
        if (reads) s.outTaint |= 1u << t[p + o.imm[0]];
      } else if (reads && !s.select) {
        s.fail = kPwDest;
        return;
      }
    }
  }
  if (s.fail == kPwOk && !stack.empty()) s.fail = kPwTokens;
}

struct Twin {
  const Scan* s = nullptr;
  std::vector<uint32_t> twinOf;  // temp register -> its twin (0 = none)
  std::vector<uint32_t> rowIndex;  // (row -> injected index + 1) pairs: row, index
  uint32_t scratch = 0, slot = 0, pos2Reg = 0;
  uint32_t InjectedRow(uint32_t row) {
    for (size_t i = 0; i + 1 < rowIndex.size(); i += 2)
      if (rowIndex[i] == row) return rowIndex[i + 1];
    rowIndex.push_back(row);
    rowIndex.push_back(static_cast<uint32_t>(rowIndex.size() / 2));
    return rowIndex.back();
  }
};
// Rewrites one operand of a twin instruction in place.
inline bool TwinOperand(Twin& w, uint32_t* t, const Opnd& o, bool isDest) {
  for (uint32_t d = 0; d < o.dim; ++d)
    if (o.sub[d]) {
      Opnd sub;
      if (!DecodeOperand(t + o.sub[d], 64, sub) || !TwinOperand(w, t + o.sub[d], sub, false)) return false;
    }
  if (o.type == kOtTemp && o.dim == 1 && o.rep[0] == 0) {
    const uint32_t r = t[o.imm[0]];
    if (r < w.twinOf.size() && w.twinOf[r])
      t[o.imm[0]] = w.twinOf[r];
    else if (isDest)
      t[o.imm[0]] = w.scratch;  // a second destination that holds no per-view value: the original writes it
    return true;
  }
  if (isDest) {
    if (o.type == kOtNull) return true;
    if (o.type != kOtOutput || o.dim != 1 || o.rep[0] != 0) return false;
    if (t[o.imm[0]] == w.s->posReg) {
      t[o.imm[0]] = w.pos2Reg;
    } else {
      t[0] &= ~(0xffu << 12);  // another output of a two-destination instruction: into the scratch temp
      t[o.imm[0]] = w.scratch;
    }
    return true;
  }
  if (o.type == kOtConstantBuffer && o.dim == 2 && o.rep[0] == 0 && o.rep[1] == 0 && t[o.imm[0]] == w.s->o->cb &&
      (w.s->RowComps(t[o.imm[1]]) & SrcComps(o)) != 0) {
    t[o.imm[1]] = w.InjectedRow(t[o.imm[1]]);
    t[o.imm[0]] = w.slot;
  }
  return true;
}

// The clone of a vertex shader: the same shader, plus a second position (the focus view's, from the injected rows)
// and the viewport mask. On any doubt: why != kPwOk and no blob.
inline PatchResult PatchVsDualPosition(const uint8_t* blob, size_t size, const PatchOpts& opts = PatchOpts()) {
  PatchResult r;
  std::vector<Chunk> chunks;
  if (!ParseContainer(blob, size, chunks)) return r;
  int shex = -1, osgn = -1;
  for (size_t i = 0; i < chunks.size(); ++i) {
    if (chunks[i].fourcc == FourCc('S', 'H', 'D', 'R') || chunks[i].fourcc == FourCc('S', 'H', 'E', 'X')) shex = static_cast<int>(i);
    if (chunks[i].fourcc == FourCc('O', 'S', 'G', 'N')) osgn = static_cast<int>(i);
  }
  if (shex < 0) return r.why = kPwNotVs, r;
  Code c;
  r.why = ReadCode(blob + chunks[shex].off, chunks[shex].size, c);
  if (r.why != kPwOk) return r;
  if (osgn < 0) return r.why = kPwSignature, r;
  const Sig sig = ReadOsgn(blob + chunks[osgn].off, chunks[osgn].size, opts);
  if (!sig.ok) return r.why = kPwSignature, r;

  Scan s;
  s.o = &opts, s.posReg = sig.posReg;
  s.tainted.assign(c.temps, 0);
  s.state.assign(c.temps, 0);
  Analyze(c, s);
  r.ins = static_cast<uint32_t>(c.ins.size() - c.firstCode);
  r.boundedRows = s.boundedRows;
  if (s.fail != kPwOk) return r.why = s.fail, r.badOp = s.badOp, r;
  r.posReg = sig.posReg;
  if (!(s.outTaint >> sig.posReg & 1)) return r.why = kPwNoViewRows, r;
  r.viewVaryings = s.outTaint & ~(1u << sig.posReg);

  const uint32_t maxOut = c.maxOut > sig.maxReg ? c.maxOut : sig.maxReg;
  if (maxOut + 2 > 31) return r.why = kPwNoSlot, r;
  Twin w;
  w.s = &s;
  w.pos2Reg = r.pos2Reg = maxOut + 1;
  r.maskReg = maxOut + 2;
  w.slot = opts.preferSlot;
  if (w.slot > 13 || (c.cbSlots >> w.slot & 1)) {
    for (w.slot = 13; w.slot < 14 && (c.cbSlots >> w.slot & 1); --w.slot) {
    }
    if (w.slot > 13) return r.why = kPwNoSlot, r;
  }
  r.slot = w.slot;
  w.twinOf.assign(c.temps, 0);
  uint32_t next = c.temps;
  for (uint32_t i = 0; i < c.temps; ++i)
    if (s.tainted[i]) w.twinOf[i] = next++;
  r.twinTemps = next - c.temps;
  w.scratch = next++;
  if (next > 4096) return r.why = kPwNoSlot, r;

  // Twins first (they also fix the injected rows), then the stream is assembled.
  std::vector<std::vector<uint32_t>> twins(c.ins.size());
  for (size_t k = c.firstCode; k < c.ins.size(); ++k) {
    const Ins& i = c.ins[k];
    const uint32_t* t = c.tok.data() + i.pos;
    const int dests = DestCount(i.op);
    bool twin = false;
    Opnd o;
    int idx = 0;
    for (uint32_t p = i.first; p < i.len && idx < dests; p += o.len, ++idx) {
      DecodeOperand(t + p, i.len - p, o);
      if (o.dim != 1 || o.rep[0] != 0) continue;
      const uint32_t reg = t[p + o.imm[0]];
      twin |= (o.type == kOtTemp && reg < s.tainted.size() && s.tainted[reg]) || (o.type == kOtOutput && reg == sig.posReg);
    }
    if (!twin) continue;
    std::vector<uint32_t>& tw = twins[k];
    tw.assign(t, t + i.len);
    idx = 0;
    for (uint32_t p = i.first; p < i.len; p += o.len, ++idx) {
      if (!DecodeOperand(tw.data() + p, i.len - p, o) || !TwinOperand(w, tw.data() + p, o, idx < dests))
        return r.why = kPwDest, r;
    }
    ++r.twinned;
  }
  const uint32_t nRows = static_cast<uint32_t>(w.rowIndex.size() / 2);
  r.rows.resize(nRows);
  for (uint32_t i = 0; i < nRows; ++i) r.rows[w.rowIndex[2 * i + 1]] = w.rowIndex[2 * i];

  std::vector<uint32_t> out;
  out.reserve(c.tok.size() * 2 + 32);
  out.push_back(c.tok[0]);
  out.push_back(0);
  for (size_t k = 0; k < c.ins.size(); ++k) {
    const Ins& i = c.ins[k];
    if (k == c.firstCode) {
      const uint32_t dcl[] = {
          0x04000059, 0x00208e46, w.slot, nRows + 1,  // dcl_constantbuffer cb<slot>[nRows + 1], immediateIndexed
          0x03000065, 0x001020f2, w.pos2Reg,          // dcl_output o<pos2>.xyzw
          0x03000065, 0x00102012, r.maskReg,          // dcl_output o<mask>.x
      };
      out.insert(out.end(), dcl, dcl + sizeof(dcl) / sizeof(dcl[0]));
      if (c.dclTemps < 0) out.push_back(0x02000068), out.push_back(next);  // dcl_temps
      const uint32_t mask[] = {0x06000036, 0x00102012, r.maskReg, 0x0020800a, w.slot, nRows};  // mov o<mask>.x, cb<slot>[nRows].x
      out.insert(out.end(), mask, mask + sizeof(mask) / sizeof(mask[0]));
    }
    if (!twins[k].empty()) out.insert(out.end(), twins[k].begin(), twins[k].end());
    const size_t at = out.size();
    out.insert(out.end(), c.tok.begin() + i.pos, c.tok.begin() + i.pos + i.len);
    if (static_cast<int>(k) == c.dclTemps) out[at + 1] = next;
  }
  out[1] = static_cast<uint32_t>(out.size());

  std::vector<ChunkData> cd(chunks.size());
  for (size_t i = 0; i < chunks.size(); ++i) {
    cd[i].fourcc = chunks[i].fourcc;
    if (static_cast<int>(i) == shex) {
      cd[i].data.resize(out.size() * 4);
      memcpy(cd[i].data.data(), out.data(), cd[i].data.size());
    } else if (static_cast<int>(i) == osgn) {
      cd[i].data = AppendOsgn(blob + chunks[i].off, chunks[i].size, sig.count, w.pos2Reg, r.maskReg, opts);
    } else {
      cd[i].data.assign(blob + chunks[i].off, blob + chunks[i].off + chunks[i].size);
    }
  }
  r.blob = BuildContainer(cd);
  r.why = kPwOk;
  return r;
}

// ---------------------------------------------------------------------------
// The pixel-shader clone (design 4.3; probe P1: the pixel shader of a stereo draw reads its view as
// SV_RenderTargetArrayIndex, also when no earlier stage writes it)
// ---------------------------------------------------------------------------
// A merged draw runs ONE pixel shader for both views, with the periphery's constant buffers bound. The clone reads
// every per-view row from an injected buffer instead, picked by the view index: injected row 2 * i + view = that
// view's bytes of rows[i]. Nothing else changes: the same instructions on the same values a separate pass would
// read, so the clone's output for view v equals the stock shader's with view v's buffers, by construction (and
// checked: WARP in the tests, the GPU under stereo in test/pep_p1_probe.cpp).
// What it adds: one input (uint SV_RenderTargetArrayIndex, constant interpolation, the next free register), one
// constant buffer (dynamically indexed), one temp holding the index.
// Refused, with the vertex patcher's reasons: a computed row of a per-view buffer that is not proven to stay on
// shared rows (the same imin / imax rule), an instruction the walker does not know, a shader that already reads the
// index, no free slot. kPwNoViewRows: the shader reads no per-view row and can be used as it is in a merged draw.
struct PsViewBuffer {
  uint32_t cb = 0;
  const ViewRow* rows = nullptr;
  uint32_t nRows = 0;
};
constexpr PsViewBuffer kPsTerrainContext[] = {{11, kTcViewRows, sizeof(kTcViewRows) / sizeof(kTcViewRows[0])}};
struct PsPatchOpts {
  // The per-view rows of every buffer the G-buffer pixel shaders read. Default: the TerrainContext only; the lists
  // of cPerView (b7) and b10 come from a live capture (design 12.3) before a clone is trusted.
  const PsViewBuffer* bufs = kPsTerrainContext;
  uint32_t nBufs = 1;
  uint32_t preferSlot = 13;
  const char* indexName = "SV_RenderTargetArrayIndex";
};
struct PsPatchResult {
  int why = kPwContainer;
  uint32_t badOp = 0;
  std::vector<uint8_t> blob;
  uint32_t slot = 0, indexReg = 0, indexTemp = 0;
  std::vector<std::pair<uint32_t, uint32_t>> rows;  // (buffer slot, row): injected rows 2 * i (view 0) and 2 * i + 1 (view 1)
  uint32_t ins = 0, rewritten = 0, boundedRows = 0;
};
inline PsPatchResult PatchPsViewSelect(const uint8_t* blob, size_t size, const PsPatchOpts& opts = PsPatchOpts()) {
  PsPatchResult r;
  std::vector<Chunk> chunks;
  if (!ParseContainer(blob, size, chunks)) return r;
  int shex = -1, isgn = -1;
  for (size_t i = 0; i < chunks.size(); ++i) {
    if (chunks[i].fourcc == FourCc('S', 'H', 'D', 'R') || chunks[i].fourcc == FourCc('S', 'H', 'E', 'X')) shex = static_cast<int>(i);
    if (chunks[i].fourcc == FourCc('I', 'S', 'G', 'N')) isgn = static_cast<int>(i);
  }
  if (shex < 0) return r.why = kPwNotVs, r;
  Code c;
  r.why = ReadCode(blob + chunks[shex].off, chunks[shex].size, c, 0);
  if (r.why != kPwOk) return r;
  r.ins = static_cast<uint32_t>(c.ins.size() - c.firstCode);
  if (isgn < 0) return r.why = kPwSignature, r;
  // The input signature: the next free register; a shader that already takes the index is not touched.
  const uint8_t* sg = blob + chunks[isgn].off;
  const uint32_t sgSize = chunks[isgn].size;
  if (sgSize < 8) return r.why = kPwSignature, r;
  const uint32_t sgCount = Rd32(sg);
  if (sgCount > 64 || 8 + 24ull * sgCount > sgSize) return r.why = kPwSignature, r;
  uint32_t maxIn = 0;
  bool anyIn = false;
  for (uint32_t i = 0; i < sgCount; ++i) {
    const uint8_t* e = sg + 8 + 24 * i;
    if (Rd32(e) >= sgSize || !memchr(sg + Rd32(e), 0, sgSize - Rd32(e))) return r.why = kPwSignature, r;
    if (Rd32(e + 8) == 4) return r.why = kPwSignature, r;  // D3D_NAME_RENDER_TARGET_ARRAY_INDEX
    const uint32_t reg = Rd32(e + 16);
    if (reg != 0xffffffff && (!anyIn || reg > maxIn)) maxIn = reg, anyIn = true;
  }
  for (size_t k = 0; k < c.firstCode; ++k) {  // registers the code declares beyond the signature (never seen; safe)
    const Ins& i = c.ins[k];
    if (i.op < 95 || i.op > 100) continue;  // dcl_input .. dcl_input_ps_siv
    Opnd o;
    const uint32_t* t = c.tok.data() + i.pos;
    if (!DecodeOperand(t + i.first, i.len - i.first, o)) return r.why = kPwTokens, r;
    if (o.type == kOtInput && o.dim == 1 && o.rep[0] == 0 && (!anyIn || t[i.first + o.imm[0]] > maxIn))
      maxIn = t[i.first + o.imm[0]], anyIn = true;
  }
  r.indexReg = anyIn ? maxIn + 1 : 0;
  if (r.indexReg > 31) return r.why = kPwNoSlot, r;

  // Computed rows of each per-view buffer: proven shared, or no clone.
  for (uint32_t b = 0; b < opts.nBufs; ++b) {
    PatchOpts po;
    po.cb = opts.bufs[b].cb, po.rows = opts.bufs[b].rows, po.nRows = opts.bufs[b].nRows;
    Scan s;
    s.o = &po, s.select = true;
    s.tainted.assign(c.temps, 0);
    s.state.assign(c.temps, 0);
    Analyze(c, s);
    if (s.fail != kPwOk) return r.why = s.fail, r.badOp = s.badOp, r;
    r.boundedRows += s.boundedRows;
  }
  auto comps = [&](uint32_t cb, uint32_t row) -> uint32_t {
    for (uint32_t b = 0; b < opts.nBufs; ++b)
      if (opts.bufs[b].cb == cb)
        for (uint32_t i = 0; i < opts.bufs[b].nRows; ++i)
          if (opts.bufs[b].rows[i].row == row) return opts.bufs[b].rows[i].comps;
    return 0;
  };
  r.slot = opts.preferSlot;
  if (r.slot > 13 || (c.cbSlots >> r.slot & 1)) {
    for (r.slot = 13; r.slot < 14 && (c.cbSlots >> r.slot & 1); --r.slot) {
    }
    if (r.slot > 13) return r.why = kPwNoSlot, r;
  }
  r.indexTemp = c.temps;
  if (r.indexTemp + 1 > 4096) return r.why = kPwNoSlot, r;

  // Every immediate read of a per-view row becomes cb<slot>[r<indexTemp>.x + 2 * i].
  std::vector<std::vector<uint32_t>> code(c.ins.size());
  for (size_t k = c.firstCode; k < c.ins.size(); ++k) {
    const Ins& i = c.ins[k];
    const uint32_t* t = c.tok.data() + i.pos;
    std::vector<uint32_t>& out = code[k];
    out.assign(t, t + i.first);
    Opnd o;
    for (uint32_t p = i.first; p < i.len; p += o.len) {
      if (!DecodeOperand(t + p, i.len - p, o)) return r.why = kPwTokens, r;
      const bool view = o.type == kOtConstantBuffer && o.dim == 2 && o.rep[0] == 0 && o.rep[1] == 0 &&
                        (comps(t[p + o.imm[0]], t[p + o.imm[1]]) & SrcComps(o)) != 0;
      if (!view) {
        out.insert(out.end(), t + p, t + p + o.len);
        continue;
      }
      const std::pair<uint32_t, uint32_t> key(t[p + o.imm[0]], t[p + o.imm[1]]);
      uint32_t idx = 0;
      while (idx < r.rows.size() && r.rows[idx] != key) ++idx;
      if (idx == r.rows.size()) r.rows.push_back(key);
      const size_t at = out.size();
      out.insert(out.end(), t + p, t + p + o.len);
      out[at] = (out[at] & ~(7u << 25)) | (3u << 25);  // second index: immediate + relative
      out[at + o.imm[0]] = r.slot;
      out[at + o.imm[1]] = 2 * idx;
      out.push_back(0x0010000a);  // r<indexTemp>.x
      out.push_back(r.indexTemp);
      ++r.rewritten;
    }
    if (out.size() > 127) return r.why = kPwTokens, r;
    if (i.op != kOpCustomData) out[0] = (out[0] & ~(0x7fu << 24)) | (static_cast<uint32_t>(out.size()) << 24);
  }
  if (!r.rewritten) return r.rows.clear(), r.why = kPwNoViewRows, r;

  std::vector<uint32_t> out;
  out.reserve(c.tok.size() + 4 * r.rewritten + 32);
  out.push_back(c.tok[0]);
  out.push_back(0);
  const uint32_t nRows = static_cast<uint32_t>(r.rows.size());
  for (size_t k = 0; k < c.ins.size(); ++k) {
    const Ins& i = c.ins[k];
    if (k == c.firstCode) {
      const uint32_t dcl[] = {
          0x04000859, 0x00208e46, r.slot, 2 * nRows,  // dcl_constantbuffer cb<slot>[2 n], dynamicIndexed
          0x04000864, 0x00101012, r.indexReg, 4,      // dcl_input_ps_siv constant v<reg>.x, rendertarget_array_index
      };
      out.insert(out.end(), dcl, dcl + sizeof(dcl) / sizeof(dcl[0]));
      if (c.dclTemps < 0) out.push_back(0x02000068), out.push_back(r.indexTemp + 1);  // dcl_temps
      const uint32_t mov[] = {0x05000036, 0x00100012, r.indexTemp, 0x0010100a, r.indexReg};  // mov r<t>.x, v<reg>.x
      out.insert(out.end(), mov, mov + sizeof(mov) / sizeof(mov[0]));
    }
    const size_t at = out.size();
    if (!code[k].empty())
      out.insert(out.end(), code[k].begin(), code[k].end());
    else
      out.insert(out.end(), c.tok.begin() + i.pos, c.tok.begin() + i.pos + i.len);
    if (static_cast<int>(k) == c.dclTemps) out[at + 1] = r.indexTemp + 1;
  }
  out[1] = static_cast<uint32_t>(out.size());

  // The input signature with the index appended.
  const uint32_t oldTable = 8 + 24 * sgCount, shift = 24;
  uint32_t strEnd = sgSize;
  while (strEnd > oldTable && sg[strEnd - 1] != 0) --strEnd;
  const size_t nName = strlen(opts.indexName) + 1;
  std::vector<uint8_t> nsg(oldTable + shift + (strEnd - oldTable) + nName, 0);
  Wr32(nsg.data(), sgCount + 1);
  Wr32(nsg.data() + 4, 8);
  for (uint32_t i = 0; i < sgCount; ++i) {
    memcpy(nsg.data() + 8 + 24 * i, sg + 8 + 24 * i, 24);
    Wr32(nsg.data() + 8 + 24 * i, Rd32(sg + 8 + 24 * i) + shift);
  }
  memcpy(nsg.data() + oldTable + shift, sg + oldTable, strEnd - oldTable);
  const uint32_t name = oldTable + shift + (strEnd - oldTable);
  memcpy(nsg.data() + name, opts.indexName, nName);
  uint8_t* e = nsg.data() + oldTable;
  Wr32(e, name), Wr32(e + 4, 0), Wr32(e + 8, 4 /*render target array index*/), Wr32(e + 12, 1 /*uint32*/), Wr32(e + 16, r.indexReg);
  e[20] = 0x1, e[21] = 0x1;  // mask x, read

  std::vector<ChunkData> cd(chunks.size());
  for (size_t i = 0; i < chunks.size(); ++i) {
    cd[i].fourcc = chunks[i].fourcc;
    if (static_cast<int>(i) == shex) {
      cd[i].data.resize(out.size() * 4);
      memcpy(cd[i].data.data(), out.data(), cd[i].data.size());
    } else if (static_cast<int>(i) == isgn) {
      cd[i].data = nsg;
    } else {
      cd[i].data.assign(blob + chunks[i].off, blob + chunks[i].off + chunks[i].size);
    }
  }
  r.blob = BuildContainer(cd);
  r.why = kPwOk;
  return r;
}
// The injected buffer of a pixel-shader clone: row 2 * i + view = `tcs[view]`'s bytes of the clone's row i. `read`
// returns the 16 bytes of (buffer slot, row) of a view, or nullptr.
template <class Read>
inline bool BuildPsInject(const PsPatchResult& r, Read read, std::vector<uint8_t>& out) {
  out.assign(r.rows.size() * 32, 0);
  for (size_t i = 0; i < r.rows.size(); ++i)
    for (int v = 0; v < 2; ++v) {
      const uint8_t* p = read(v, r.rows[i].first, r.rows[i].second);
      if (!p) return false;
      memcpy(&out[i * 32 + static_cast<size_t>(v) * 16], p, 16);
    }
  return true;
}

// ---------------------------------------------------------------------------
// Census of a shader cache
// ---------------------------------------------------------------------------
struct Census {
  uint32_t files = 0, skippedFiles = 0, blobs = 0, badSum = 0, duplicates = 0;
  uint32_t vs = 0;             // distinct vertex shaders
  uint32_t declares = 0;       // ... that declare the per-view buffer
  uint32_t why[kPwCount] = {};  // of those
  uint32_t exact = 0;          // cloned, position only
  uint32_t varyings = 0;       // cloned, but a view-dependent value goes to the pixel shader
  uint64_t ins = 0, twinned = 0;  // over the cloned shaders
  uint32_t bounded = 0;        // clones that read a computed row proven to stay on rows both views share
  uint32_t badOps[256] = {};
  // Pixel shaders, against the TerrainContext's per-view rows only (PsPatchOpts' default).
  uint32_t ps = 0;               // distinct pixel shaders
  uint32_t psWhy[kPwCount] = {};  // kPwOk: view-select clone made; kPwNoViewRows: usable as it is
  uint64_t psIns = 0, psRewritten = 0;
};
inline bool DeclaresCb(const uint8_t* blob, const std::vector<Chunk>& chunks, uint32_t slot, bool* vs) {
  *vs = false;
  for (const Chunk& ch : chunks) {
    if (ch.fourcc != FourCc('S', 'H', 'D', 'R') && ch.fourcc != FourCc('S', 'H', 'E', 'X')) continue;
    Code c;
    const int why = ReadCode(blob + ch.off, ch.size, c);
    if (why == kPwNotVs) return false;
    *vs = true;
    return why != kPwOk || (slot < 32 && (c.cbSlots >> slot & 1));  // unreadable: counted, the patcher names the reason
  }
  return false;
}
// Every DXBC container in a file's bytes (DCS's cache files hold them back to back with their own headers).
inline void CensusBytes(const uint8_t* f, size_t n, const PatchOpts& o, Census& c, std::unordered_set<uint64_t>& seen) {
  for (size_t i = 0; i + 32 <= n;) {
    const void* hit = memchr(f + i, 'D', n - i - 3);
    if (!hit) break;
    i = static_cast<size_t>(static_cast<const uint8_t*>(hit) - f);
    const uint32_t size = ContainerSize(f + i, n - i);
    if (!size) {
      ++i;
      continue;
    }
    const uint8_t* b = f + i;
    i += size;
    ++c.blobs;
    if (!ChecksumOk(b, size)) {
      ++c.badSum;
      continue;
    }
    uint64_t key;
    memcpy(&key, b + 4, 8);
    uint64_t key2;
    memcpy(&key2, b + 12, 8);
    if (!seen.insert(key ^ (key2 * 0x9e3779b97f4a7c15ull)).second) {
      ++c.duplicates;
      continue;
    }
    std::vector<Chunk> chunks;
    bool vs = false;
    if (!ParseContainer(b, size, chunks)) continue;
    const bool declares = DeclaresCb(b, chunks, o.cb, &vs);
    if (!vs) {
      const PsPatchResult pr = PatchPsViewSelect(b, size);
      if (pr.why != kPwNotVs) {  // a pixel shader
        ++c.ps;
        ++c.psWhy[pr.why];
        if (pr.why == kPwOk) c.psIns += pr.ins, c.psRewritten += pr.rewritten;
      }
      continue;
    }
    ++c.vs;
    if (!declares) continue;
    ++c.declares;
    const PatchResult r = PatchVsDualPosition(b, size, o);
    ++c.why[r.why];
    if (r.why == kPwOpcode) ++c.badOps[r.badOp & 255];
    if (r.why != kPwOk) continue;
    r.viewVaryings ? ++c.varyings : ++c.exact;
    if (r.boundedRows) ++c.bounded;
    c.ins += r.ins, c.twinned += r.twinned;
  }
}
inline bool ReadFileBytes(const std::wstring& path, std::vector<uint8_t>& out, uint64_t maxBytes) {
  HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                         OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
  if (h == INVALID_HANDLE_VALUE) return false;
  LARGE_INTEGER sz{};
  bool ok = GetFileSizeEx(h, &sz) && sz.QuadPart >= 0 && static_cast<uint64_t>(sz.QuadPart) <= maxBytes;
  if (ok) {
    out.resize(static_cast<size_t>(sz.QuadPart));
    size_t got = 0;
    while (ok && got < out.size()) {
      DWORD n = 0;
      const DWORD want = static_cast<DWORD>(out.size() - got > (16u << 20) ? (16u << 20) : out.size() - got);
      ok = ReadFile(h, out.data() + got, want, &n, nullptr) && n > 0;
      got += n;
    }
  }
  CloseHandle(h);
  return ok;
}
// *.meta2 (DCS's cache) and *.dxbc (dev extracts) of one folder, read-only. False: the folder holds neither.
inline bool CensusFolder(const std::wstring& dir, const PatchOpts& o, Census& c, const std::atomic<bool>* abort = nullptr) {
  std::unordered_set<uint64_t> seen;
  bool any = false;
  for (const wchar_t* pat : {L"\\*.meta2", L"\\*.dxbc"}) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + pat).c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) continue;
    do {
      if (abort && abort->load(std::memory_order_relaxed)) break;
      if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
      any = true;
      std::vector<uint8_t> bytes;
      if (!ReadFileBytes(dir + L"\\" + fd.cFileName, bytes, 256ull << 20)) {
        ++c.skippedFiles;
        continue;
      }
      ++c.files;
      CensusBytes(bytes.data(), bytes.size(), o, c, seen);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
  }
  return any;
}
// The report, one statement per line.
inline std::string CensusText(const Census& c) {
  char b[512];
  std::string s;
  snprintf(b, sizeof(b),
           "per-eye pass census: %u files read (%u unreadable or above 256 MB), %u shader blobs (%u with a wrong "
           "checksum, %u duplicates), %u distinct vertex shaders, %u of them declare the per-view buffer\n",
           c.files, c.skippedFiles, c.blobs, c.badSum, c.duplicates, c.vs, c.declares);
  s += b;
  const uint32_t view = c.declares - c.why[kPwNoViewRows];
  snprintf(b, sizeof(b),
           "per-eye pass census: position depends on a per-view row in %u; cloned exactly %u (%.1f %%), cloned but a "
           "view-dependent value goes to the pixel shader %u (%.1f %%), not cloned %u\n",
           view, c.exact, view ? 100.0 * c.exact / view : 0.0, c.varyings, view ? 100.0 * c.varyings / view : 0.0,
           view - c.exact - c.varyings);
  s += b;
  for (int w = kPwContainer; w < kPwCount; ++w) {
    if (!c.why[w] || w == kPwNoViewRows) continue;
    snprintf(b, sizeof(b), "per-eye pass census: not cloned, %s: %u\n", PatchWhyName(w), c.why[w]);
    s += b;
  }
  for (int op = 0; op < 256; ++op) {
    if (!c.badOps[op]) continue;
    snprintf(b, sizeof(b), "per-eye pass census: unhandled instruction opcode %d in %u shaders\n", op, c.badOps[op]);
    s += b;
  }
  if (c.bounded) {
    snprintf(b, sizeof(b),
             "per-eye pass census: %u clones read a computed row of the per-view buffer; its index is clamped to rows "
             "both views share\n",
             c.bounded);
    s += b;
  }
  if (c.ps) {
    snprintf(b, sizeof(b),
             "per-eye pass census: %u distinct pixel shaders: %u read no per-view TerrainContext row (usable as they "
             "are), %u get a view-select clone (%llu operands rewritten), %u refused\n",
             c.ps, c.psWhy[kPwNoViewRows], c.psWhy[kPwOk], static_cast<unsigned long long>(c.psRewritten),
             c.ps - c.psWhy[kPwNoViewRows] - c.psWhy[kPwOk]);
    s += b;
    for (int w = kPwContainer; w < kPwCount; ++w) {
      if (!c.psWhy[w] || w == kPwNoViewRows) continue;
      snprintf(b, sizeof(b), "per-eye pass census: pixel shaders refused, %s: %u\n", PatchWhyName(w), c.psWhy[w]);
      s += b;
    }
  }
  if (c.exact + c.varyings) {
    snprintf(b, sizeof(b), "per-eye pass census: clones repeat %.1f %% of their instructions (%llu of %llu)\n",
             c.ins ? 100.0 * static_cast<double>(c.twinned) / static_cast<double>(c.ins) : 0.0,
             static_cast<unsigned long long>(c.twinned), static_cast<unsigned long long>(c.ins));
    s += b;
  }
  return s;
}

#ifndef DCSQV_PEP_NO_RUNTIME
// [Suite] PerEyePassCensus=1: the census of DCS's own shader cache, from the suite thread. Reads files, logs lines.
inline void SuitePhase(const std::atomic<bool>& abort) {
  const std::wstring dir = g_dir + L"..\\..\\metashaders2";
  Census c;
  const ULONGLONG t0 = GetTickCount64();
  if (!CensusFolder(dir, PatchOpts(), c, &abort)) {
    Log("per-eye pass census: no shader cache files in <Saved Games>\\DCS\\metashaders2 (DCS writes them while a "
        "mission runs); nothing counted");
    return;
  }
  const std::string text = CensusText(c);
  for (size_t a = 0; a < text.size();) {
    const size_t e = text.find('\n', a);
    Log("%s", text.substr(a, e - a).c_str());
    a = e == std::string::npos ? text.size() : e + 1;
  }
  Log("per-eye pass census: done in %llu ms; nothing was changed", static_cast<unsigned long long>(GetTickCount64() - t0));
}
#endif

}  // namespace pep
