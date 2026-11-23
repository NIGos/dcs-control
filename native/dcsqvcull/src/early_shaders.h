// 0.7 early shaders: shaders DCS creates before this payload loads (DcsQvCull.lua runs 40-70 s into a DCS start;
// DX11ShaderBinaries::loadCache has created the deferred / post / sky / water / cloud shaders by then, so no creation
// hook ever sees their bytes). Three parts, all default off:
//
// 1. Identification at bind time, by output ([D3D] EarlyShaders bit 0, or a non-zero census key: then only the
//    census's identity column, nothing swapped). DCS's bound shader object is run on synthetic
//    inputs next to shaders this payload compiles itself from DCS's own installed source (Bazar\shaders), on DCS's
//    device (recorded on a deferred context by the worker, executed on the render thread at a pass boundary with
//    ExecuteCommandList(.., TRUE), read back without waiting) - the R34 chain fast route, for pixel shaders. A DCS
//    object is taken as kernel K only when its output equals our build of K bit for bit in every case and differs
//    from every other candidate (DecideIdentity). Our builds themselves are pinned: their SHEX code chunk must hash
//    to the value proven offline for that kernel (kSsaoProven; the stock build reproduces DCS 2.9.31's archive blob
//    byte for byte in its code chunk, 4f6d6b19 / 72edc447). A different DCS version, another source text, another
//    compiler or a mismatch on any case: the shader stays DCS's (logged once).
//    Limits: only kernels with a registered recipe (synthetic inputs + source + proven hashes) can be identified;
//    today the two SSAO kernels. A DCS shader created before load with no recipe has no identity beyond its pass,
//    bindings and constant buffers (the census below).
//
// 2. SSAO water skip ([D3D] EarlyShaders bit 0, default 0). deferred/SSAO.fx SSAO_Value tests the stencil of the
//    centre pixel (loop-invariant) inside its 64 / 128-sample loop and adds nothing on water, so the stock result on
//    water is exactly 1.0 (acc stays 0; bytecode 4f6d6b19 / 72edc447: movc keeps r1.x, then mad -0 * 1/64 + 1).
//    The variant is the same source with one line added before the loop:
//        [branch] if ((SampleMap(StencilMap, pix, 0).g & STENCIL_COMPOSITION_MASK) != STENCIL_COMPOSITION_WATER)
//    (PatchSsaoSource). Proven offline bit-identical on water and non-water pixels (tools\shader_exact, `pair`,
//    rules\ssao.txt, WARP and GPU; negative controls detected). At each SSAOPassData draw whose pixel shader is an
//    identified SSAO kernel, our variant is bound for that draw and DCS's put back right after it. A draw hook that
//    d3d11 has toggled away just lets DCS's stock shader draw (same picture, no gain).
//
// 3. Census ([Suite] EarlyShaderCensus=N, measure only; each new non-zero N runs one census of one frame). Every
//    draw / dispatch DCS issues on its immediate context inside a top-level render pass of that frame is classed by
//    (pass occurrence, VS, PS, GS, CS objects); per class: draw / dispatch counts, vertex / index totals, the bound
//    state at its first draw (shader resource slots with dimension / format / size, constant buffer slots and sizes,
//    render targets, viewport), whether each shader was seen created (with the creation hook in from payload start:
//    its DXBC checksum = the `hash` of artifacts\gpu-review\fxo-sweep\blob_table.csv) or is early, which recipe
//    identified it, and, for pixel-shader classes whose bindings fit one of the idle-pass hints (canopy icing /
//    fogging / droplet glass, droplet blur / copy, G-effect, hot air, cirrus), the constant values the idle tests
//    read (copied from the bound buffers to staging at the first draw, read back without waiting) and the verdict.
//    Written to early_census_<N>.csv next to the log, with one summary line in the log. Nothing DCS draws changes.
//    The creation record (PS / VS / GS creation hooks in d3d11's shared device table) is installed at payload start
//    only when the key is non-zero then: a census armed later reports shaders created before the key as early.
//
// Idle-pass skeletons (EarlyIdle* below): only the detection of the idle condition from constants the module can
// read at bind time, unit-tested. The live census (2026-11-18) showed canopy fogging, droplets and G-effect are not
// drawn when idle; hot air is: bit 1 of [D3D] EarlyShaders is the hot-air idle skip (below). Bits 2-4 are refused and
// logged.
//
// Included once from main.cpp (main/features.inc) after periph_census.h (uses ptiming, fixdepth, chainfast, gpt,
// d3ds, sfilt, shadowinst, HookSlot, Log).
#pragma once

#include <d3dcompiler.h>

#include <array>
#include <limits>

namespace eshd {

// ===========================================================================
// Pure parts (unit-tested)
// ===========================================================================

// --- SSAO variant source ----------------------------------------------------
constexpr char kSsaoLoop[] = "for (uint k = 0; k < KERNEL; ++k) {";
constexpr char kSsaoInnerTest[] = "if (stv != STENCIL_COMPOSITION_WATER) {";
constexpr char kSsaoInnerSample[] = "uint stv = SampleMap(StencilMap, pix, 0).g & STENCIL_COMPOSITION_MASK;";
constexpr char kSsaoBranch[] =
    "[branch] if ((SampleMap(StencilMap, pix, 0).g & STENCIL_COMPOSITION_MASK) != STENCIL_COMPOSITION_WATER)";
constexpr char kSsaoAccEnd[] = "acc = 1 - acc / KERNEL;";
// Entry points appended to the source (DCS's technique passes SSAO_1 / SSAO_2: PS(64 / 128, true, 3.0)).
constexpr char kSsaoEntries[] =
    "\nPS_OUTPUT QvSsao64(const VS_OUTPUT i) { return PS(i, 64, true, 3.0); }\n"
    "PS_OUTPUT QvSsao128(const VS_OUTPUT i) { return PS(i, 128, true, 3.0); }\n";
constexpr const char* kSsaoEntry[2] = {"QvSsao64", "QvSsao128"};
constexpr const char* kSsaoName[2] = {"SSAO 64 (High: SSAO_1)", "SSAO 128 (SSAO_2)"};

// SHEX code chunk (FNV-1a 64 of the chunk payload, chainfast::ShexChunk) of the builds proven offline, DCS 2.9.31,
// d3dcompiler_47 (HLSL compiler 10.1), ps_5_0, flags 0, DIRECTX11=true USE_DCS_DEFERRED=1. stock = DCS's own archive
// blob's code chunk (4f6d6b19 / 72edc447, stock and ATMOSX archives alike).
struct Proven {
  uint64_t stock, variant;
  uint32_t stockBytes, variantBytes;
};
constexpr Proven kSsaoProven[2] = {{0xb4a9ca6c1e9f1850ull, 0x8ec58ec13015becfull, 5644, 5668},
                                   {0xaf68824576c2e990ull, 0x396393d113f7cc0full, 5644, 5668}};

inline std::string TrimWs(const std::string& s) {
  size_t a = 0, b = s.size();
  while (a < b && (s[a] == ' ' || s[a] == '\t')) ++a;
  while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r' || s[b - 1] == '\n')) --b;
  return s.substr(a, b - a);
}

// The variant source: DCS's SSAO.fx with kSsaoBranch inserted before the sample loop (same indentation and line
// ending). Refused (false, why) unless the text is the one the offline proof read: exactly one loop line, after
// "float2 SSAO_Value(", and before the loop's end ("acc = 1 - acc / KERNEL;") exactly one stencil sample at the
// centre pixel `pix` and one water test. The compiled result is pinned again by kSsaoProven.
inline bool PatchSsaoSource(const std::string& src, std::string* out, std::string* why) {
  std::vector<std::string> lines;
  for (size_t p = 0; p < src.size();) {
    size_t e = src.find('\n', p);
    e = e == std::string::npos ? src.size() : e + 1;
    lines.push_back(src.substr(p, e - p));
    p = e;
  }
  int fn = -1, loop = -1, loops = 0, end = -1;
  for (int i = 0; i < static_cast<int>(lines.size()); ++i) {
    const std::string t = TrimWs(lines[i]);
    if (fn < 0 && t.compare(0, 18, "float2 SSAO_Value(") == 0) fn = i;
    if (t == kSsaoLoop) ++loops, loop = i;
  }
  if (fn < 0 || loops != 1 || loop < fn) {
    if (why) *why = fn < 0 ? "SSAO_Value not found" : loops != 1 ? "sample loop line not found exactly once" : "loop before SSAO_Value";
    return false;
  }
  int samples = 0, otherUses = 0, tests = 0;
  for (int i = loop + 1; i < static_cast<int>(lines.size()); ++i) {
    const std::string t = TrimWs(lines[i]);
    if (t == kSsaoAccEnd) {
      end = i;
      break;
    }
    if (t == kSsaoInnerSample) ++samples;
    else if (t.find("StencilMap") != std::string::npos) ++otherUses;  // any other stencil read in the loop
    if (t == kSsaoInnerTest) ++tests;
  }
  if (end < 0 || samples != 1 || otherUses != 0 || tests != 1) {
    if (why) *why = end < 0 ? "end of the loop not found" : "the loop's water test is not the expected one";
    return false;
  }
  const std::string& l = lines[loop];
  const size_t ind = l.find_first_not_of(" \t");
  std::string eol = "\n";
  if (l.size() >= 2 && l[l.size() - 2] == '\r') eol = "\r\n";
  if (l.empty() || l.back() != '\n') eol = "\n";
  std::string o;
  o.reserve(src.size() + 160);
  for (int i = 0; i < static_cast<int>(lines.size()); ++i) {
    if (i == loop) o += l.substr(0, ind) + kSsaoBranch + eol;
    o += lines[i];
  }
  *out = std::move(o);
  return true;
}

// --- identity by output -----------------------------------------------------
// out[c] = one case's read-back words. Returns the index of the only candidate whose every case equals DCS's bit
// for bit, -1 when none does, -2 when several do (not discriminated: never taken).
inline int DecideIdentity(const std::vector<std::vector<uint32_t>>& dcs,
                          const std::vector<std::vector<std::vector<uint32_t>>>& cands) {
  int found = -1;
  for (size_t k = 0; k < cands.size(); ++k) {
    bool eq = cands[k].size() == dcs.size() && !dcs.empty();
    for (size_t c = 0; eq && c < dcs.size(); ++c) eq = cands[k][c] == dcs[c];
    if (!eq) continue;
    if (found >= 0) return -2;
    found = static_cast<int>(k);
  }
  return found;
}

// --- synthetic SSAO cases (fingerprint; the offline rules\ssao.txt, smaller) -----------------------------------
struct XRng {
  uint64_t s;
  uint32_t operator()() {
    s ^= s << 13, s ^= s >> 7, s ^= s << 17;
    return static_cast<uint32_t>(s >> 11);
  }
  float F(float a, float b) { return a + (b - a) * static_cast<float>((*this)() % 1000000u) / 1000000.0f; }
};
inline uint32_t FBits(float f) {
  uint32_t u;
  memcpy(&u, &f, 4);
  return u;
}
constexpr UINT kFpW = 64, kFpH = 48, kFpCases = 6, kFpSlices = 6;
constexpr UINT kCb7Bytes = 2416;  // DCS's cPerView (R120b)
struct SsaoCase {
  std::vector<float> gbuf;       // kFpSlices x W x H x 4
  std::vector<float> depth;      // W x H
  std::vector<uint32_t> stencil; // W x H x 2
  uint32_t cb0[8];
  std::vector<uint32_t> cb7;     // kCb7Bytes / 4
};
inline void StoreCols(const double m[4][4], uint32_t* w) {
  for (int c = 0; c < 4; ++c)
    for (int r = 0; r < 4; ++r) w[4 * c + r] = FBits(static_cast<float>(m[r][c]));
}
inline bool Inv4(const double a[4][4], double inv[4][4]) {
  double m[4][8];
  for (int r = 0; r < 4; ++r)
    for (int c = 0; c < 8; ++c) m[r][c] = c < 4 ? a[r][c] : (c - 4 == r ? 1.0 : 0.0);
  for (int c = 0; c < 4; ++c) {
    int p = c;
    for (int r = c + 1; r < 4; ++r)
      if (fabs(m[r][c]) > fabs(m[p][c])) p = r;
    if (fabs(m[p][c]) < 1e-30) return false;
    for (int k = 0; k < 8; ++k) std::swap(m[p][k], m[c][k]);
    const double d = m[c][c];
    for (int k = 0; k < 8; ++k) m[c][k] /= d;
    for (int r = 0; r < 4; ++r)
      if (r != c) {
        const double f = m[r][c];
        for (int k = 0; k < 8; ++k) m[r][k] -= f * m[c][k];
      }
  }
  for (int r = 0; r < 4; ++r)
    for (int c = 0; c < 4; ++c) inv[r][c] = m[r][c + 4];
  return true;
}
inline SsaoCase MakeSsaoCase(uint32_t seed) {
  XRng g{0x9e3779b97f4a7c15ull ^ (static_cast<uint64_t>(seed) * 0x100000001b3ull)};
  for (int i = 0; i < 4; ++i) g();
  SsaoCase k;
  k.gbuf.resize(static_cast<size_t>(kFpSlices) * kFpW * kFpH * 4);
  for (float& v : k.gbuf) v = g.F(0, 1);
  k.depth.resize(static_cast<size_t>(kFpW) * kFpH);
  for (float& v : k.depth) {
    const uint32_t r = g() % 100;
    v = r < 10 ? 0.0f : r < 20 ? 1.0f : g.F(0, 1);
  }
  k.stencil.resize(static_cast<size_t>(kFpW) * kFpH * 2);
  static const uint32_t kOther[6] = {0, 8, 24, 32, 48, 56};
  for (size_t i = 0; i < k.stencil.size(); i += 2) {
    const uint32_t r = g() % 100, type = r < 35 ? 16u : r < 50 ? 40u : kOther[g() % 6];
    k.stencil[i] = g() & 0xff;
    k.stencil[i + 1] = (g() & 0xC7u) | type;
  }
  memset(k.cb0, 0, sizeof(k.cb0));
  k.cb0[2] = FBits(g.F(0.05f, 4.0f));  // radius
  k.cb0[6] = kFpW, k.cb0[7] = kFpH;    // viewport (0, 0, W, H)
  k.cb7.assign(kCb7Bytes / 4, 0);
  for (uint32_t& w : k.cb7) w = FBits(g.F(-2, 2));
  // gView (word 32): a rotation; gProj (64) and gProjInv (80): a perspective pair (asymmetric, reversed in 1 of 3)
  {
    double q[4], s = 0;
    for (double& x : q) x = g.F(-1, 1), s += x * x;
    s = sqrt(s > 1e-12 ? s : 1);
    for (double& x : q) x /= s;
    const double w = q[0], x = q[1], y = q[2], z = q[3];
    const double m[4][4] = {{1 - 2 * (y * y + z * z), 2 * (x * y - w * z), 2 * (x * z + w * y), 0},
                            {2 * (x * y + w * z), 1 - 2 * (x * x + z * z), 2 * (y * z - w * x), 0},
                            {2 * (x * z - w * y), 2 * (y * z + w * x), 1 - 2 * (x * x + y * y), 0},
                            {0, 0, 0, 1}};
    StoreCols(m, &k.cb7[32]);
  }
  {
    const double tx = g.F(0.3f, 1.6f), ty = g.F(0.3f, 1.6f), ox = g.F(-0.4f, 0.4f), oy = g.F(-0.4f, 0.4f);
    const double n = g.F(0.05f, 1.0f), f = g.F(1000.0f, 100000.0f);
    double p[4][4] = {{1 / tx, 0, 0, 0}, {0, 1 / ty, 0, 0}, {ox, oy, 0, 1}, {0, 0, 0, 0}};
    if (g() % 3 == 0) p[2][2] = n / (n - f), p[3][2] = -f * n / (n - f);
    else p[2][2] = f / (f - n), p[3][2] = -n * f / (f - n);
    double inv[4][4];
    if (Inv4(p, inv)) {
      StoreCols(p, &k.cb7[64]);
      StoreCols(inv, &k.cb7[80]);
    }
  }
  return k;
}

// --- census: bound state of a class's first draw ----------------------------
struct SrvDesc {
  uint8_t slot = 0, dim = 0;  // D3D11_SRV_DIMENSION
  uint16_t fmt = 0;           // DXGI_FORMAT
  uint32_t w = 0, h = 0, d = 0;  // width, height, depth or array size
};
struct BindSig {
  std::vector<SrvDesc> srv;   // pixel stage (draws) or compute stage (dispatches)
  uint16_t cbMask = 0;
  uint32_t cbBytes[14] = {};
  uint8_t nrt = 0;
  uint16_t rtFmt[8] = {};
  uint32_t rtW = 0, rtH = 0;
  bool dsv = false;
  float vpW = 0, vpH = 0;
};
inline const SrvDesc* SrvAt(const BindSig& s, int slot) {
  for (const SrvDesc& d : s.srv)
    if (d.slot == slot) return &d;
  return nullptr;
}
inline std::string SigText(const BindSig& s) {
  std::string o;
  char b[96];
  for (const SrvDesc& d : s.srv) {
    snprintf(b, sizeof(b), "t%u:%s%u/%ux%u%s%u ", d.slot,
             d.dim == D3D11_SRV_DIMENSION_BUFFER || d.dim == D3D11_SRV_DIMENSION_BUFFEREX ? "buf"
             : d.dim == D3D11_SRV_DIMENSION_TEXTURE3D                                 ? "3d"
             : d.dim == D3D11_SRV_DIMENSION_TEXTURE2DARRAY                            ? "2da"
             : d.dim == D3D11_SRV_DIMENSION_TEXTURECUBE                               ? "cube"
                                                                                      : "2d",
             d.fmt, d.w, d.h, d.d > 1 ? "x" : "", d.d > 1 ? d.d : 0u);
    o += b;
  }
  for (int i = 0; i < 14; ++i)
    if (s.cbMask >> i & 1) {
      snprintf(b, sizeof(b), "b%d:%u ", i, s.cbBytes[i]);
      o += b;
    }
  for (int i = 0; i < s.nrt; ++i) {
    snprintf(b, sizeof(b), "rt%d:%u ", i, s.rtFmt[i]);
    o += b;
  }
  snprintf(b, sizeof(b), "%ux%u%s vp %.0fx%.0f", s.rtW, s.rtH, s.dsv ? " dsv" : "", s.vpW, s.vpH);
  o += b;
  return o;
}

// --- idle-pass hints and tests ------------------------------------------------
// A hint names a pixel shader family by the bindings its reflection declares (fxo-sweep blob_table.csv,
// DCS 2.9.31): a class "fits" when every required slot holds a view of the required dimension and every required
// constant buffer is bound. A fit is a pointer for the reader, not an identity (any shader with the same bindings
// fits too); the census prints it with the class's pass.
enum Need : uint8_t { kAny2d = 1, kNeed3d = 2, kNeed2da = 3 };
struct Hint {
  const char* name;
  const char* source;
  struct {
    int slot;
    Need need;
  } srv[4];
  uint16_t cbMask;
};
constexpr Hint kHints[] = {
    // glass_material.fx forward_ps_pass_icing / _fogging / _droplets (model/functions/glass.hlsl): CockpitRefraction
    // t103, RainDroplets t104 (2.9.31 reflection of every IcingMap variant), cPerFrame b6
    {"canopy glass (icing / fogging / droplets)", "model/glass_material.fx", {{103, kAny2d}, {104, kAny2d}, {-1}, {-1}}, 1u << 6},
    // enlight/droplets.fx BLUR_PS / COPY_PS: src t0, surface t1 at 2048 x 2048 (the canopy droplet map)
    {"canopy droplet blur", "enlight/droplets.fx", {{0, kAny2d}, {1, kAny2d}, {-1}, {-1}}, 1u << 0},
    // PostEffects/overload.fx PS: DiffuseMap t0, depthMap t1, $Globals b0, cPerView b7
    {"G-effect (overload)", "PostEffects/overload.fx", {{0, kAny2d}, {1, kAny2d}, {-1}, {-1}}, (1u << 0) | (1u << 7)},
    // enlight/hotair.fx psHeatAir: HeatMap t0, HeatTex t1 (3D), $Globals b0
    {"hot air", "enlight/hotair.fx", {{0, kAny2d}, {1, kNeed3d}, {-1}, {-1}}, 1u << 0},
    // enlight/cirrus.fx psCirrus: DiffTex t0, DiffTex2 t1, cPerFrame b6, cPerView b7, no $Globals
    {"cirrus", "enlight/cirrus.fx", {{0, kAny2d}, {1, kAny2d}, {-1}, {-1}}, (1u << 6) | (1u << 7)},
};
constexpr int kHintCount = static_cast<int>(sizeof(kHints) / sizeof(kHints[0]));
enum HintId { kHGlass = 0, kHDroplets, kHOverload, kHHotAir, kHCirrus };

inline bool HintFits(const Hint& h, const BindSig& s) {
  if ((s.cbMask & h.cbMask) != h.cbMask) return false;
  for (const auto& r : h.srv) {
    if (r.slot < 0) break;
    const SrvDesc* d = SrvAt(s, r.slot);
    if (!d) return false;
    if (r.need == kNeed3d && d->dim != D3D11_SRV_DIMENSION_TEXTURE3D) return false;
    if (r.need == kNeed2da && d->dim != D3D11_SRV_DIMENSION_TEXTURE2DARRAY) return false;
    if (r.need == kAny2d && d->dim != D3D11_SRV_DIMENSION_TEXTURE2D && d->dim != D3D11_SRV_DIMENSION_TEXTURE2DARRAY &&
        d->dim != D3D11_SRV_DIMENSION_TEXTURE2DMS)
      return false;
  }
  if (&h == &kHints[kHDroplets]) {  // the canopy droplet map is 2048 x 2048
    const SrvDesc* d = SrvAt(s, 0);
    if (!d || d->w != 2048 || d->h != 2048) return false;
  }
  if (&h == &kHints[kHCirrus] && (s.cbMask & 1u)) return false;  // cirrus binds no $Globals
  return true;
}

// Idle tests: the constants each reads (constant buffer slot, byte offset; offsets from fxc reflection of DCS's
// installed sources, 2.9.31) and the condition under which the pass changes nothing on screen or writes a
// constant result. "Identical picture" for a skip needs more than the test, see kIdleNeeds.
inline bool Nan(float f) { return f != f; }
// glass.hlsl forward_ps_pass_icing / _fogging: value = saturate(gCockpitIcing.x * saturate(1 + (1 - aisMask.w) * 10)
// - ais), ais >= 0. gCockpitIcing.x <= 0 (or NaN: saturate(NaN) = 0) makes value 0 for every pixel: no ice normal,
// no environment blend, fogging's 63-tap blur has radius 0. cPerFrame (b6) +496 (gCockpitIcing).
constexpr int kIcingCb = 6;
constexpr uint32_t kIcingOff = 496;
inline bool IcingIdle(const float icing[4]) { return Nan(icing[0]) || icing[0] <= 0.0f; }
// PostEffects/overload.fx PS: defocus <= 0 skips the blur; blackwhitening == 0 and overload == 0 make both lerps and
// the darkening (pow(1, 0.5) = 1, pow(0, 4) = 0) identities: the output is DiffuseMap sampled at the pixel.
// $Globals (b0): defocus +36, blackwhitening +40, overload +44.
constexpr int kOverloadCb = 0;
constexpr uint32_t kOverloadOff = 36;
inline bool OverloadIdle(float defocus, float blackwhitening, float overload) {
  return !(defocus > 0.0f) && blackwhitening == 0.0f && overload == 0.0f;
}
// enlight/droplets.fx BLUR_PS (stock and ATMOSX): the canopy map relaxes towards the neutral normal (HALF, HALF, 1)
// by lerps of timeDelta / windForce / effectClear; DROPS passes add droplets. CPU-visible part of "nothing to do":
// windForce * timeDelta == 0 (no streaking) - the map being already neutral is GPU state (see kIdleNeeds).
// $Globals (b0): timeDelta +0, windForce +16, effectClear +20.
constexpr int kDropletsCb = 0;
inline bool DropletsCpuIdle(float timeDelta, float windForce) { return windForce * timeDelta == 0.0f; }
// enlight/hotair.fx psHeatAir discards every pixel whose heat-map opacity is < 0.01; with no heat particle drawn in
// the view's SimplePassData:HotAirPass (draw count 0 in the census) and the heat map cleared, every pixel discards.
inline bool HotAirIdle(uint64_t heatDrawsThisView) { return heatDrawsThisView == 0; }
// What a skip needs besides the test (printed with the census, read by the live agent).
constexpr const char* kIdleNeeds[kHintCount] = {
    "glass: the icing / fogging / droplet glass pass is the canopy's only glass draw (blend off): it cannot be skipped; "
    "identical picture needs an exact cheaper equivalent at value 0 (icing: the 3 refraction taps at the unbent "
    "position; fogging: NOT exact - 63 equal-weight taps at one position are not bitwise the single tap) or proof that "
    "DCS draws the plain glass pass P0/P1 instead when icing is 0",
    "droplets: skip blur + copy only while no droplet is added (DROPS passes not drawn, countAdd 0) AND the map is "
    "already the blur's fixed point (needs a GPU check of the 2048x2048 map: every texel neutral), else not identical",
    "G-effect: idle output = DiffuseMap at the pixel centre: a skip needs the consumer to read DiffuseMap (redirect) "
    "or a CopyResource when target and source match in size and format; never a plain skip",
    "hot air: skip heat-air only when the view's HotAirPass drew nothing AND the heat map was cleared to < 0.01; the "
    "following copy pass must then be shown to be a round trip (census: its source and target views)",
    "cirrus: no constant gates it (density comes from textures and time); only the census can show draws in clear "
    "weather; a skip would need every pixel to discard (opacity < 0.001), not decidable on the CPU",
};

// ===========================================================================
// D3D part
// ===========================================================================
using Ctx = ID3D11DeviceContext;
std::atomic<uint32_t> g_mask{0};           // [D3D] EarlyShaders as applied (bit 0 SSAO water skip, bit 1 hot-air idle skip)
std::atomic<bool> g_identWanted{false};    // SSAO identification runs (the skip, or a census key: identity column only)
std::atomic<bool> g_installed{false};      // boundary callback in
std::atomic<bool> g_stopping{false};
std::atomic<bool> g_fault{false};
Ctx* g_ctx = nullptr;                      // DCS's immediate context (AddRef)
ID3D11Device* g_dev = nullptr;             // its device (AddRef)
std::wstring g_logDir;                     // census files next to the log

// --- pass labels (render thread) ---
std::unordered_map<void*, std::string> g_passLabel;
std::unordered_map<void*, bool> g_passSsao;
const std::string& LabelOf(void* pass) {
  auto it = g_passLabel.find(pass);
  if (it != g_passLabel.end()) return it->second;
  std::string l = ptiming::RttiName(pass);
  char inl[48];
  fixdepth::PassName(pass, inl, sizeof(inl));
  bool printable = inl[0] != 0;
  for (const char* p = inl; *p; ++p) printable = printable && *p >= 0x20 && *p < 0x7f;
  if (printable && l.find(inl) == std::string::npos) l += std::string(":") + inl;
  g_passSsao[pass] = l.compare(0, 12, "SSAOPassData") == 0;
  return g_passLabel.emplace(pass, std::move(l)).first->second;
}

// --- creation record (census) ---
std::mutex g_createMu;
std::unordered_map<void*, std::array<uint8_t, 16>> g_created;  // shader object -> DXBC checksum
std::atomic<bool> g_createHooked{false};
using CreatePsFn = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, const void*, SIZE_T, ID3D11ClassLinkage*, ID3D11PixelShader**);
using CreateVsFn = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, const void*, SIZE_T, ID3D11ClassLinkage*, ID3D11VertexShader**);
using CreateGsFn = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, const void*, SIZE_T, ID3D11ClassLinkage*, ID3D11GeometryShader**);
CreatePsFn g_createPs = nullptr;
CreateVsFn g_createVs = nullptr;
CreateGsFn g_createGs = nullptr;
void** g_createSlot[3] = {};
void NoteCreated(void* obj, const void* code, SIZE_T n) {
  if (!obj || !code || n < 20 || memcmp(code, "DXBC", 4) != 0) return;
  std::array<uint8_t, 16> h;
  memcpy(h.data(), static_cast<const uint8_t*>(code) + 4, 16);
  std::lock_guard<std::mutex> lock(g_createMu);
  if (g_created.size() < 200000) g_created[obj] = h;
}
HRESULT STDMETHODCALLTYPE H_CreatePs(ID3D11Device* d, const void* c, SIZE_T n, ID3D11ClassLinkage* l, ID3D11PixelShader** o) {
  const HRESULT hr = g_createPs(d, c, n, l, o);
  if (SUCCEEDED(hr) && o && *o) NoteCreated(*o, c, n);
  return hr;
}
HRESULT STDMETHODCALLTYPE H_CreateVs(ID3D11Device* d, const void* c, SIZE_T n, ID3D11ClassLinkage* l, ID3D11VertexShader** o) {
  const HRESULT hr = g_createVs(d, c, n, l, o);
  if (SUCCEEDED(hr) && o && *o) NoteCreated(*o, c, n);
  return hr;
}
HRESULT STDMETHODCALLTYPE H_CreateGs(ID3D11Device* d, const void* c, SIZE_T n, ID3D11ClassLinkage* l, ID3D11GeometryShader** o) {
  const HRESULT hr = g_createGs(d, c, n, l, o);
  if (SUCCEEDED(hr) && o && *o) NoteCreated(*o, c, n);
  return hr;
}
// In d3d11's shared device table, through chain fast's private device (every device of the process shares it).
bool InstallCreateRecord() {
  if (g_createHooked.load()) return true;
  ID3D11Device* dev = nullptr;
  if (!chainfast::AcquirePrivate(&dev) || !dev) return false;
  void** vt = *reinterpret_cast<void***>(dev);
  const char* names[3] = {"CreatePixelShader", "CreateVertexShader", "CreateGeometryShader"};
  void* hooks[3] = {reinterpret_cast<void*>(&H_CreatePs), reinterpret_cast<void*>(&H_CreateVs),
                    reinterpret_cast<void*>(&H_CreateGs)};
  void* origs[3] = {};
  for (int i = 0; i < 3; ++i) {
    const int s = DcsQv_DeviceSlot(names[i]);
    if (s < 0 || !HookSlot(&vt[s], hooks[i], &origs[i]) || !origs[i]) {
      for (int j = 0; j < i; ++j) UnhookSlot(g_createSlot[j], origs[j]);
      return false;
    }
    g_createSlot[i] = &vt[s];
  }
  g_createPs = reinterpret_cast<CreatePsFn>(origs[0]);
  g_createVs = reinterpret_cast<CreateVsFn>(origs[1]);
  g_createGs = reinterpret_cast<CreateGsFn>(origs[2]);
  g_createHooked = true;
  return true;
}
bool CreatedHash(void* obj, std::array<uint8_t, 16>* h) {
  if (!obj) return false;
  std::lock_guard<std::mutex> lock(g_createMu);
  auto it = g_created.find(obj);
  if (it == g_created.end()) return false;
  *h = it->second;
  return true;
}

// --- SSAO recipe: our builds, the fingerprint job, the identified objects ---
struct SsaoBuild {
  std::vector<uint8_t> stock[2], variant[2];
  std::string why;  // empty when built and pinned
};
// The include handler for DCS's shader tree: "x.hlsl" next to the including file first, then the tree's root (fxc's
// order with /I <root>). The main file is given as text (the patched or plain SSAO.fx).
struct TreeInclude : ID3DInclude {
  std::wstring root;  // ...\Bazar\shaders\ (with the slash)
  std::vector<std::string> keep;
  std::unordered_map<const void*, std::string> dirOf;  // buffer -> its directory relative to root ("deferred/")
  std::string mainDir = "deferred/";
  static bool ReadAll(const std::wstring& p, std::string* out) {
    FILE* f = nullptr;
    if (_wfopen_s(&f, p.c_str(), L"rb") != 0 || !f) return false;
    char buf[65536];
    size_t n;
    out->clear();
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) out->append(buf, n);
    fclose(f);
    return true;
  }
  HRESULT STDMETHODCALLTYPE Open(D3D_INCLUDE_TYPE, LPCSTR name, LPCVOID parent, LPCVOID* data, UINT* bytes) override {
    std::string rel = name ? name : "";
    for (char& ch : rel)
      if (ch == '\\') ch = '/';
    const auto pit = dirOf.find(parent);
    const std::string pdir = pit != dirOf.end() ? pit->second : mainDir;
    std::string text, found;
    for (const std::string& cand : {pdir + rel, rel}) {
      // normalise "a/../b"
      std::vector<std::string> parts;
      size_t p = 0;
      while (p <= cand.size()) {
        size_t e = cand.find('/', p);
        if (e == std::string::npos) e = cand.size();
        const std::string seg = cand.substr(p, e - p);
        if (seg == "..") {
          if (!parts.empty()) parts.pop_back();
        } else if (!seg.empty() && seg != ".") {
          parts.push_back(seg);
        }
        p = e + 1;
      }
      std::string norm;
      for (size_t i = 0; i < parts.size(); ++i) norm += (i ? "/" : "") + parts[i];
      std::wstring w(norm.begin(), norm.end());
      if (ReadAll(root + w, &text)) {
        found = norm;
        break;
      }
    }
    if (found.empty()) return E_FAIL;
    keep.push_back(std::move(text));
    const std::string& t = keep.back();
    *data = t.data();
    *bytes = static_cast<UINT>(t.size());
    const size_t slash = found.find_last_of('/');
    dirOf[t.data()] = slash == std::string::npos ? std::string() : found.substr(0, slash + 1);
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE Close(LPCVOID) override { return S_OK; }
};
// Compiles one entry of `text` (DCS's SSAO.fx, plain or patched, plus kSsaoEntries) as DCS's archive was built.
bool CompileSsao(const std::string& text, const std::wstring& shaderRoot, const char* entry, std::vector<uint8_t>* out,
                 std::string* err) {
  if (!chainfast::LoadCompiler()) {
    if (err) *err = "d3dcompiler_47.dll not available";
    return false;
  }
  TreeInclude inc;
  inc.root = shaderRoot;
  const D3D_SHADER_MACRO defs[] = {{"DIRECTX11", "true"}, {"USE_DCS_DEFERRED", "1"}, {nullptr, nullptr}};
  ID3DBlob* code = nullptr;
  ID3DBlob* e = nullptr;
  const HRESULT hr = chainfast::g_compile(text.data(), text.size(), "deferred/SSAO.fx", defs, &inc, entry, "ps_5_0", 0, 0,
                                          &code, &e);
  if (FAILED(hr) || !code) {
    if (err) *err = e ? std::string(static_cast<const char*>(e->GetBufferPointer()), e->GetBufferSize()) : "D3DCompile failed";
    if (e) e->Release();
    if (code) code->Release();
    return false;
  }
  if (e) e->Release();
  const uint8_t* p = static_cast<const uint8_t*>(code->GetBufferPointer());
  out->assign(p, p + code->GetBufferSize());
  code->Release();
  return true;
}
inline uint64_t ShexHash(const std::vector<uint8_t>& b, uint32_t* len) {
  const uint8_t* p = nullptr;
  *len = 0;
  if (!chainfast::ShexChunk(b.data(), b.size(), &p, len)) return 0;
  return chainfast::Fnv64(p, *len);
}
// shaderRoot: DCS's Bazar\shaders\ (with the slash). Fills b->why on any refusal.
bool BuildSsao(const std::wstring& shaderRoot, SsaoBuild* b) {
  std::string src;
  if (!TreeInclude::ReadAll(shaderRoot + L"deferred\\SSAO.fx", &src)) {
    b->why = "deferred\\SSAO.fx not readable";
    return false;
  }
  std::string var, why;
  if (!PatchSsaoSource(src, &var, &why)) {
    b->why = "SSAO.fx is not the proven text (" + why + ")";
    return false;
  }
  src += kSsaoEntries;
  var += kSsaoEntries;
  for (int k = 0; k < 2; ++k) {
    std::string err;
    if (!CompileSsao(src, shaderRoot, kSsaoEntry[k], &b->stock[k], &err) ||
        !CompileSsao(var, shaderRoot, kSsaoEntry[k], &b->variant[k], &err)) {
      b->why = "compile failed: " + err.substr(0, 200);
      return false;
    }
    uint32_t ls = 0, lv = 0;
    const uint64_t hs = ShexHash(b->stock[k], &ls), hv = ShexHash(b->variant[k], &lv);
    if (hs != kSsaoProven[k].stock || hv != kSsaoProven[k].variant) {
      char t[200];
      snprintf(t, sizeof(t), "%s: code %016llx / %016llx (%u / %u bytes) is not the proven %016llx / %016llx",
               kSsaoName[k], static_cast<unsigned long long>(hs), static_cast<unsigned long long>(hv), ls, lv,
               static_cast<unsigned long long>(kSsaoProven[k].stock),
               static_cast<unsigned long long>(kSsaoProven[k].variant));
      b->why = t;
      return false;
    }
  }
  b->why.clear();
  return true;
}

// The fingerprint VS: a full-screen strip, projPos = position (DCS's own SSAO VS does the same).
constexpr char kFpVs[] = R"(
struct VO { float4 pos : SV_POSITION; float4 projPos : TEXCOORD0; };
VO main(uint vid : SV_VertexID) {
  float2 q = float2((vid & 1) ? 1 : -1, (vid & 2) ? 1 : -1);
  VO o; o.pos = float4(q, 0, 1); o.projPos = o.pos; return o;
}
)";
struct FpCaseGpu {
  ID3D11Texture2D* tex[3] = {};
  ID3D11ShaderResourceView* srv[3] = {};
  ID3D11Buffer* cb[2] = {};
  void Release() {
    for (auto*& p : tex)
      if (p) p->Release(), p = nullptr;
    for (auto*& p : srv)
      if (p) p->Release(), p = nullptr;
    for (auto*& p : cb)
      if (p) p->Release(), p = nullptr;
  }
};
bool MakeFpCase(ID3D11Device* d, const SsaoCase& k, FpCaseGpu* g) {
  D3D11_TEXTURE2D_DESC td{kFpW, kFpH, 1, kFpSlices, DXGI_FORMAT_R32G32B32A32_FLOAT, {1, 0}, D3D11_USAGE_DEFAULT,
                          D3D11_BIND_SHADER_RESOURCE};
  std::vector<D3D11_SUBRESOURCE_DATA> sd(kFpSlices);
  for (UINT s = 0; s < kFpSlices; ++s) sd[s] = {k.gbuf.data() + static_cast<size_t>(s) * kFpW * kFpH * 4, kFpW * 16, 0};
  if (FAILED(d->CreateTexture2D(&td, sd.data(), &g->tex[0]))) return false;
  td.ArraySize = 1, td.Format = DXGI_FORMAT_R32_FLOAT;
  D3D11_SUBRESOURCE_DATA s1{k.depth.data(), kFpW * 4, 0};
  if (FAILED(d->CreateTexture2D(&td, &s1, &g->tex[1]))) return false;
  td.Format = DXGI_FORMAT_R32G32_UINT;
  D3D11_SUBRESOURCE_DATA s2{k.stencil.data(), kFpW * 8, 0};
  if (FAILED(d->CreateTexture2D(&td, &s2, &g->tex[2]))) return false;
  for (int i = 0; i < 3; ++i)
    if (FAILED(d->CreateShaderResourceView(g->tex[i], nullptr, &g->srv[i]))) return false;
  D3D11_BUFFER_DESC bd{32, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER};
  D3D11_SUBRESOURCE_DATA c0{k.cb0};
  if (FAILED(d->CreateBuffer(&bd, &c0, &g->cb[0]))) return false;
  bd.ByteWidth = kCb7Bytes;
  D3D11_SUBRESOURCE_DATA c7{k.cb7.data()};
  return SUCCEEDED(d->CreateBuffer(&bd, &c7, &g->cb[1]));
}

// One fingerprint job: DCS's object and our stock builds (64, 128) on kFpCases cases.
constexpr int kFpShaders = 3;  // 0 DCS's, 1 our stock 64, 2 our stock 128
struct FpJob {
  ID3D11PixelShader* dcs = nullptr;  // AddRef
  ID3D11CommandList* cl = nullptr;
  std::vector<ID3D11Texture2D*> stage;  // [case][shader][rt]
  std::vector<std::vector<uint32_t>> data;
  size_t mapped = 0;          // render thread
  std::atomic<int> state{0};  // 1 recorded, 2 executed, 3 read back, -1 failed
  ULONGLONG t0 = 0;
  // When the render thread executed the job (0: not yet). The 20 s read-back limit counts from here: a job that waits
  // for its first pass through a long mission-load stall is not given up (0.7 small levers fix).
  std::atomic<ULONGLONG> tExec{0};
  void ReleaseGpu() {
    for (auto*& s : stage)
      if (s) s->Release(), s = nullptr;
    if (cl) cl->Release(), cl = nullptr;
  }
};
struct SsaoState {
  std::atomic<int> build{0};  // 0 not tried, 1 ready, -1 refused
  SsaoBuild blobs;
  ID3D11PixelShader* ours[2] = {};     // our stock builds (fingerprint references)
  ID3D11PixelShader* variant[2] = {};  // the water-skip variants
  ID3D11VertexShader* fpVs = nullptr;
  // identified DCS objects (render thread reads; worker appends before publishing g_nIdent)
  ID3D11PixelShader* ident[8] = {};
  int identKind[8] = {};
  std::atomic<int> nIdent{0};
  // objects seen in SSAO passes and not identified yet (render thread -> worker)
  std::mutex candMu;
  std::vector<ID3D11PixelShader*> cands;  // AddRef
  std::vector<void*> probed;              // render thread: never offered twice
  std::atomic<FpJob*> job{nullptr};
  std::vector<FpJob*> done;
  int tested = 0, accepted = 0, rejected = 0, retried = 0;
  std::unordered_map<void*, int> retries;  // worker: fingerprint attempts that failed to run or read back, per object
};
SsaoState g_ssao;
std::atomic<uint64_t> g_swaps{0}, g_ssaoDraws{0}, g_ssaoPasses{0};

// Worker: records the job (deferred context of DCS's device).
bool FpRecord(FpJob& j, ID3D11Device* d) {
  ID3D11DeviceContext* dc = nullptr;
  if (FAILED(d->CreateDeferredContext(0, &dc)) || !dc) return false;
  bool ok = true;
  D3D11_TEXTURE2D_DESC rd{kFpW, kFpH, 1, 1, DXGI_FORMAT_R32G32B32A32_FLOAT, {1, 0}, D3D11_USAGE_DEFAULT,
                          D3D11_BIND_RENDER_TARGET};
  ID3D11Texture2D* rt[2] = {};
  ID3D11RenderTargetView* rtv[2] = {};
  for (int i = 0; i < 2 && ok; ++i)
    ok = SUCCEEDED(d->CreateTexture2D(&rd, nullptr, &rt[i])) && SUCCEEDED(d->CreateRenderTargetView(rt[i], nullptr, &rtv[i]));
  D3D11_TEXTURE2D_DESC sd = rd;
  sd.Usage = D3D11_USAGE_STAGING, sd.BindFlags = 0, sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  ID3D11PixelShader* ps[kFpShaders] = {j.dcs, g_ssao.ours[0], g_ssao.ours[1]};
  ID3D11RasterizerState* rs = nullptr;  // DCS's SSAO pass: cullNone
  {
    D3D11_RASTERIZER_DESC rsd{D3D11_FILL_SOLID, D3D11_CULL_NONE};
    rsd.DepthClipEnable = TRUE;
    ok = SUCCEEDED(d->CreateRasterizerState(&rsd, &rs));
  }
  for (UINT c = 0; c < kFpCases && ok; ++c) {
    const SsaoCase k = MakeSsaoCase(0x55A0 + c);
    FpCaseGpu g;
    ok = MakeFpCase(d, k, &g);
    for (int s = 0; s < kFpShaders && ok; ++s) {
      const float clr[4] = {-7, -7, -7, -7};
      dc->ClearRenderTargetView(rtv[0], clr);
      dc->ClearRenderTargetView(rtv[1], clr);
      dc->OMSetRenderTargets(2, rtv, nullptr);
      D3D11_VIEWPORT vp{0, 0, static_cast<float>(kFpW), static_cast<float>(kFpH), 0, 1};
      dc->RSSetViewports(1, &vp);
      dc->RSSetState(rs);
      dc->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
      dc->IASetInputLayout(nullptr);
      dc->VSSetShader(g_ssao.fpVs, nullptr, 0);
      dc->PSSetShader(ps[s], nullptr, 0);
      dc->PSSetShaderResources(0, 3, g.srv);
      ID3D11Buffer* cbs[8] = {g.cb[0], nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, g.cb[1]};
      dc->PSSetConstantBuffers(0, 8, cbs);
      dc->Draw(4, 0);
      for (int r = 0; r < 2 && ok; ++r) {
        ID3D11Texture2D* st = nullptr;
        ok = SUCCEEDED(d->CreateTexture2D(&sd, nullptr, &st));
        if (ok) dc->CopyResource(st, rt[r]);
        j.stage.push_back(st);
      }
    }
    g.Release();  // the command list keeps what it uses
  }
  dc->ClearState();
  if (ok) ok = SUCCEEDED(dc->FinishCommandList(FALSE, &j.cl)) && j.cl;
  dc->Release();
  if (rs) rs->Release();
  for (int i = 0; i < 2; ++i) {
    if (rtv[i]) rtv[i]->Release();
    if (rt[i]) rt[i]->Release();
  }
  return ok;
}
// Render thread (a top-level pass begin, no arm in the table): execute, then read back without waiting.
void FpRenderStep(Ctx* c) {
  FpJob* j = g_ssao.job.load(std::memory_order_acquire);
  if (!j) return;
  const int st = j->state.load(std::memory_order_acquire);
  if (st == 1) {
    j->tExec.store(GetTickCount64(), std::memory_order_relaxed);
    j->state.store(2, std::memory_order_release);
    c->ExecuteCommandList(j->cl, TRUE);  // TRUE: DCS's context state is restored afterwards
    return;
  }
  if (st != 2) return;
  while (j->mapped < j->stage.size()) {
    D3D11_MAPPED_SUBRESOURCE m{};
    const HRESULT hr = c->Map(j->stage[j->mapped], 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m);
    if (hr == DXGI_ERROR_WAS_STILL_DRAWING) return;
    if (FAILED(hr)) {
      j->state.store(-1, std::memory_order_release);
      return;
    }
    std::vector<uint32_t> w(static_cast<size_t>(kFpW) * kFpH * 4);
    for (UINT y = 0; y < kFpH; ++y)
      memcpy(&w[static_cast<size_t>(y) * kFpW * 4], static_cast<const uint8_t*>(m.pData) + static_cast<size_t>(y) * m.RowPitch,
             kFpW * 16);
    c->Unmap(j->stage[j->mapped], 0);
    j->data.push_back(std::move(w));
    ++j->mapped;
  }
  j->state.store(3, std::memory_order_release);
}
// Worker: the job's verdict. -1 / -2 as DecideIdentity, -3 our two builds not told apart by the cases.
int FpVerdict(const FpJob& j) {
  if (j.data.size() != static_cast<size_t>(kFpCases) * kFpShaders * 2) return -1;
  std::vector<std::vector<uint32_t>> dcs;
  std::vector<std::vector<std::vector<uint32_t>>> cand(2);
  for (UINT c = 0; c < kFpCases; ++c) {
    auto at = [&](int s) {
      std::vector<uint32_t> v = j.data[(c * kFpShaders + s) * 2];
      const auto& b = j.data[(c * kFpShaders + s) * 2 + 1];
      v.insert(v.end(), b.begin(), b.end());
      return v;
    };
    dcs.push_back(at(0));
    cand[0].push_back(at(1));
    cand[1].push_back(at(2));
  }
  if (cand[0] == cand[1]) return -3;
  return DecideIdentity(dcs, cand);
}

// --- draw hooks (render thread, armed per top-level pass) ---
enum Dk : int { kDDraw, kDIdx, kDInst, kDIdxInst, kDAuto, kDIdxInd, kDInstInd, kDDisp, kDDispInd, kDkCount };
const char* const kDkNames[kDkCount] = {"Draw", "DrawIndexed", "DrawInstanced", "DrawIndexedInstanced", "DrawAuto",
                                        "DrawIndexedInstancedIndirect", "DrawInstancedIndirect", "Dispatch",
                                        "DispatchIndirect"};
int g_slot[kDkCount];
void* volatile g_orig[kDkCount] = {};
bool g_armed[kDkCount] = {};
void** g_vt = nullptr;
int g_armDepth = -1;  // depth of the pass that armed (render thread), -1 none
std::atomic<uint64_t> g_armRefused{0}, g_armLost{0};

// census state (render thread while running; worker after g_cState == 3)
struct ClassRec {
  std::string pass;
  int occ = 0;                    // occurrence of the pass label in the frame (0 = first)
  void *vs = nullptr, *ps = nullptr, *gs = nullptr, *cs = nullptr;
  uint64_t draws = 0, dispatches = 0, verts = 0, insts = 0;
  BindSig sig;
  ID3D11Buffer* cbStage[14] = {};  // staging copies of the first draw's PS constant buffers (hint fits only)
  std::vector<uint32_t> cbData[14];
  int hint = -1;
};
std::atomic<int> g_cState{0};  // 0 idle, 1 armed (waiting for a frame start), 2 recording, 3 reading back, 4 done
std::atomic<int> g_cKey{0};
uint64_t g_cFrame = 0;
std::vector<ClassRec> g_classes;
std::unordered_map<std::string, size_t> g_classIdx;
std::unordered_map<std::string, int> g_passOcc;  // label -> occurrences so far this frame
std::string g_curPass;
int g_curOcc = 0;
uint64_t g_cPasses = 0, g_cDrawsTotal = 0, g_cLostPasses = 0, g_cRefusedPasses = 0;
size_t g_cMapped = 0;
uint64_t g_cWaitFrames = 0;
constexpr size_t kMaxClasses = 4000, kMaxCbSnap = 600;
size_t g_cbSnaps = 0;

void DescribeSrvs(ID3D11ShaderResourceView* const* v, int n, BindSig* s) {
  for (int i = 0; i < n; ++i) {
    if (!v[i]) continue;
    SrvDesc d;
    d.slot = static_cast<uint8_t>(i);
    D3D11_SHADER_RESOURCE_VIEW_DESC vd{};
    v[i]->GetDesc(&vd);
    d.dim = static_cast<uint8_t>(vd.ViewDimension);
    d.fmt = static_cast<uint16_t>(vd.Format);
    ID3D11Resource* r = nullptr;
    v[i]->GetResource(&r);
    if (r) {
      D3D11_RESOURCE_DIMENSION rd;
      r->GetType(&rd);
      if (rd == D3D11_RESOURCE_DIMENSION_TEXTURE2D) {
        D3D11_TEXTURE2D_DESC t{};
        static_cast<ID3D11Texture2D*>(r)->GetDesc(&t);
        d.w = t.Width, d.h = t.Height, d.d = t.ArraySize;
      } else if (rd == D3D11_RESOURCE_DIMENSION_TEXTURE3D) {
        D3D11_TEXTURE3D_DESC t{};
        static_cast<ID3D11Texture3D*>(r)->GetDesc(&t);
        d.w = t.Width, d.h = t.Height, d.d = t.Depth;
      } else if (rd == D3D11_RESOURCE_DIMENSION_BUFFER) {
        D3D11_BUFFER_DESC b{};
        static_cast<ID3D11Buffer*>(r)->GetDesc(&b);
        d.w = b.ByteWidth;
      }
      r->Release();
    }
    s->srv.push_back(d);
  }
}
void CaptureSig(Ctx* c, bool compute, ClassRec* rec) {
  BindSig& s = rec->sig;
  ID3D11ShaderResourceView* v[128] = {};
  ID3D11Buffer* cb[14] = {};
  if (compute) {
    c->CSGetShaderResources(0, 128, v);
    c->CSGetConstantBuffers(0, 14, cb);
  } else {
    c->PSGetShaderResources(0, 128, v);
    c->PSGetConstantBuffers(0, 14, cb);
  }
  DescribeSrvs(v, 128, &s);
  for (auto* p : v)
    if (p) p->Release();
  for (int i = 0; i < 14; ++i)
    if (cb[i]) {
      D3D11_BUFFER_DESC b{};
      cb[i]->GetDesc(&b);
      s.cbMask |= static_cast<uint16_t>(1u << i);
      s.cbBytes[i] = b.ByteWidth;
    }
  if (!compute) {
    ID3D11RenderTargetView* rt[8] = {};
    ID3D11DepthStencilView* ds = nullptr;
    c->OMGetRenderTargets(8, rt, &ds);
    for (int i = 0; i < 8; ++i) {
      if (!rt[i]) continue;
      D3D11_RENDER_TARGET_VIEW_DESC rd{};
      rt[i]->GetDesc(&rd);
      s.nrt = static_cast<uint8_t>(i + 1);
      s.rtFmt[i] = static_cast<uint16_t>(rd.Format);
      if (!s.rtW) {
        ID3D11Resource* r = nullptr;
        rt[i]->GetResource(&r);
        D3D11_RESOURCE_DIMENSION dim;
        if (r) {
          r->GetType(&dim);
          if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D) {
            D3D11_TEXTURE2D_DESC t{};
            static_cast<ID3D11Texture2D*>(r)->GetDesc(&t);
            s.rtW = t.Width, s.rtH = t.Height;
          }
          r->Release();
        }
      }
      rt[i]->Release();
    }
    if (ds) s.dsv = true, ds->Release();
    UINT nv = 1;
    D3D11_VIEWPORT vp{};
    c->RSGetViewports(&nv, &vp);
    s.vpW = nv ? vp.Width : 0, s.vpH = nv ? vp.Height : 0;
    for (int h = 0; h < kHintCount; ++h)
      if (HintFits(kHints[h], s)) {
        rec->hint = h;
        break;
      }
    if (rec->hint >= 0 && g_cbSnaps < kMaxCbSnap)
      for (int i : {0, 6})
        if (cb[i] && s.cbBytes[i] <= 65536) {
          D3D11_BUFFER_DESC b{s.cbBytes[i], D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ};
          if (SUCCEEDED(g_dev->CreateBuffer(&b, nullptr, &rec->cbStage[i]))) {
            c->CopyResource(rec->cbStage[i], cb[i]);
            ++g_cbSnaps;
          }
        }
  }
  for (auto* p : cb)
    if (p) p->Release();
}
void Record(Ctx* c, int kind, UINT count, UINT inst) {
  const bool compute = kind >= kDDisp;
  ID3D11VertexShader* vs = nullptr;
  ID3D11PixelShader* ps = nullptr;
  ID3D11GeometryShader* gs = nullptr;
  ID3D11ComputeShader* cs = nullptr;
  if (compute) {
    c->CSGetShader(&cs, nullptr, nullptr);
  } else {
    c->VSGetShader(&vs, nullptr, nullptr);
    c->PSGetShader(&ps, nullptr, nullptr);
    c->GSGetShader(&gs, nullptr, nullptr);
  }
  char key[160];
  snprintf(key, sizeof(key), "%s#%d|%p|%p|%p|%p", g_curPass.c_str(), g_curOcc, static_cast<void*>(vs),
           static_cast<void*>(ps), static_cast<void*>(gs), static_cast<void*>(cs));
  auto it = g_classIdx.find(key);
  size_t i;
  if (it == g_classIdx.end()) {
    if (g_classes.size() >= kMaxClasses) goto out;
    i = g_classes.size();
    g_classIdx.emplace(key, i);
    g_classes.emplace_back();
    ClassRec& r = g_classes.back();
    r.pass = g_curPass, r.occ = g_curOcc, r.vs = vs, r.ps = ps, r.gs = gs, r.cs = cs;
    CaptureSig(c, compute, &r);
  } else {
    i = it->second;
  }
  {
    ClassRec& r = g_classes[i];
    if (compute) ++r.dispatches;
    else ++r.draws;
    r.verts += count;
    r.insts += inst;
    ++g_cDrawsTotal;
  }
out:
  if (vs) vs->Release();
  if (ps) ps->Release();
  if (gs) gs->Release();
  if (cs) cs->Release();
}

thread_local bool t_inHook = false;
bool g_swapPass = false;  // render thread: this top-level pass is an SSAO pass with the swap armed

// --- hot-air idle skip ([D3D] EarlyShaders bit 1, 0.7 small levers) ---
// Per view DCS runs SimplePassData:HotAirPass (heat particles into a quarter-resolution heat map), then two top-level
// passes of the HotAirModule: HotAirCopySourcePass (the scene copied to a scratch target) and HotAirComposePass
// (enlight/hotair.fx psHeatAir: every pixel whose heat-map opacity is < 0.01 discards, so with an empty heat map the
// scene target is left as it was and the scratch copy is read by nobody else). Live census 2026-11-18: no heat draw in
// any view with no engine running nearby, and the module still costs ~0.08 ms per frame (6.6 M pixel-shader
// invocations, 3.3 M samples written: the copy).
// Rule: the copy and compose passes of a frame are not executed (their execute is replaced by an empty one through
// pass_timing.h's second intercept) only when every HotAirPass of that frame so far was watched completely (draw
// entries armed for the whole pass and still ours at its end; ExecuteCommandList on the context goes through
// split_filter.h's hook, which counts it) and drew nothing, and at least as many HotAirPasses as compose passes were
// seen. Anything else (a heat draw in any view, a command list executed inside the pass, an arm refused or lost, an
// unknown order) runs DCS's passes: stock picture, no gain.
bool g_heatPass = false;              // render thread: the current top-level pass is a watched HotAirPass
uint64_t g_heatDraws = 0;             // render thread: draws seen in the current HotAirPass
uint64_t g_heatEcl0 = 0;              // render thread: split filter's ExecuteCommandList count at its begin
bool g_heatEclWatched = false;        // render thread: that count covers the context's ExecuteCommandList
uint64_t g_heatFrame = ~0ull;         // render thread: frame of the counters below
int g_heatSeen = 0, g_composeSeen = 0;
bool g_heatAllIdle = true;            // every HotAirPass of g_heatFrame watched and empty
std::atomic<uint64_t> g_heatSkipped{0}, g_heatRun{0}, g_heatBusy{0}, g_heatUnwatched{0}, g_heatOrder{0};
std::unordered_map<void*, int> g_heatKind;  // render thread: pass -> 0 other, 1 HotAirPass, 2 copy, 3 compose
// Pass label (RTTI name, ":" inline name) -> 0 other, 1 heat particles, 2 copy, 3 compose.
inline int HeatKindOf(const std::string& label) {
  if (label.compare(0, 14, "SimplePassData") == 0 && label.find(":HotAirPass") != std::string::npos) return 1;
  if (label.compare(0, 12, "HotAirModule") != 0) return 0;
  if (label.find(":HotAirCopySourcePass") != std::string::npos) return 2;
  if (label.find(":HotAirComposePass") != std::string::npos) return 3;
  return 0;
}
// Copy (2) or compose (3) pass: skip when at least one heat pass ran this frame, all of them were watched and empty,
// and there have been at least as many heat passes as composes including this view's (composeSeen counts this
// compose already; a copy precedes its view's compose).
inline bool HeatSkipOk(int kind, int heatSeen, int composeSeen, bool allIdle) {
  if (kind != 2 && kind != 3) return false;
  return heatSeen > 0 && allIdle && heatSeen >= composeSeen + (kind == 2 ? 1 : 0);
}
// A mission event (main/exports.inc): the pass objects are rebuilt and new ones may reuse old addresses, so every
// per-pass cache (labels, SSAO flag, hot-air kind) is dropped by the render thread at its next top-level pass.
std::atomic<bool> g_cacheReset{false};
inline void NoteMissionEvent() { g_cacheReset.store(true, std::memory_order_release); }
inline void TakeCacheReset() {
  if (!g_cacheReset.load(std::memory_order_relaxed) || !g_cacheReset.exchange(false, std::memory_order_acq_rel)) return;
  g_heatKind.clear();
  g_passLabel.clear();
  g_passSsao.clear();
}
int HeatKind(void* pass, const std::string& label) {
  auto it = g_heatKind.find(pass);
  if (it != g_heatKind.end()) return it->second;
  const int k = HeatKindOf(label);
  g_heatKind.emplace(pass, k);
  return k;
}
void HeatFrame(uint64_t f) {
  if (f == g_heatFrame) return;
  g_heatFrame = f;
  g_heatSeen = g_composeSeen = 0;
  g_heatAllIdle = true;
}
void __fastcall HeatNoExecute(void*, uint64_t) {}
const std::string& LabelOf(void* pass);
// pass_timing.h second intercept (render thread, top-level passes, before any boundary callback).
ptiming::ExecFn HeatIntercept(void* pass, uint64_t) {
  if (!(g_mask.load(std::memory_order_relaxed) & 2u) || g_fault.load(std::memory_order_relaxed)) return nullptr;
  ptiming::ExecFn r = nullptr;
  __try {
    TakeCacheReset();
    int k = HeatKind(pass, LabelOf(pass));
    // The kind is cached per pass object at its first sight. An object freed and another one made at its address
    // (passes rebuilt on a mission load) would keep a copy / compose kind and not be executed: those two kinds are
    // checked by name again, and a mismatch classes the pass afresh (a mission event also drops every cache).
    if ((k == 2 || k == 3) && !fixdepth::PassIs(pass, k == 2 ? "HotAirCopySourcePass" : "HotAirComposePass")) {
      g_heatKind.erase(pass);
      g_passLabel.erase(pass);
      g_passSsao.erase(pass);
      k = HeatKind(pass, LabelOf(pass));
    }
    if (k == 2 || k == 3) {
      HeatFrame(ptiming::g_execFrame.load(std::memory_order_relaxed));
      if (k == 3) ++g_composeSeen;
      if (HeatSkipOk(k, g_heatSeen, g_composeSeen, g_heatAllIdle)) {
        r = &HeatNoExecute;
        g_heatSkipped.fetch_add(1, std::memory_order_relaxed);
      } else {
        g_heatRun.fetch_add(1, std::memory_order_relaxed);
        if (g_heatAllIdle && g_heatSeen > 0) g_heatOrder.fetch_add(1, std::memory_order_relaxed);
      }
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    g_fault = true;
    r = nullptr;
  }
  return r;
}
// The SSAO part of Pre: swap an identified kernel, offer an unknown one to the worker (each object once).
ID3D11PixelShader* SsaoPre(Ctx* c) {
  g_ssaoDraws.fetch_add(1, std::memory_order_relaxed);
  ID3D11PixelShader* ps = nullptr;
  c->PSGetShader(&ps, nullptr, nullptr);
  if (!ps) return nullptr;
  const int n = g_ssao.nIdent.load(std::memory_order_acquire);
  int kindIdx = -1;
  for (int i = 0; i < n; ++i)
    if (g_ssao.ident[i] == ps) kindIdx = g_ssao.identKind[i];
  if (kindIdx >= 0 && (g_mask.load(std::memory_order_relaxed) & 1u) && g_ssao.variant[kindIdx]) {
    c->PSSetShader(g_ssao.variant[kindIdx], nullptr, 0);
    g_swaps.fetch_add(1, std::memory_order_relaxed);
    return ps;  // keeps the reference until Post
  }
  if (kindIdx < 0 && g_ssao.probed.size() < 16 &&
      std::find(g_ssao.probed.begin(), g_ssao.probed.end(), static_cast<void*>(ps)) == g_ssao.probed.end()) {
    ps->AddRef();  // the probed list's reference
    g_ssao.probed.push_back(ps);
    ps->AddRef();  // the candidate's (the worker takes it over)
    std::lock_guard<std::mutex> lock(g_ssao.candMu);
    g_ssao.cands.push_back(ps);
  }
  ps->Release();
  return nullptr;
}
ID3D11PixelShader* PreBody(Ctx* c, int kind, UINT count, UINT inst) {
  if (g_cState.load(std::memory_order_relaxed) == 2) Record(c, kind, count, inst);
  if (g_heatPass && kind < kDDisp) ++g_heatDraws;
  return g_swapPass && kind < kDDisp ? SsaoPre(c) : nullptr;
}
// Before DCS's draw: census record, SSAO swap. Returns DCS's PS to restore (AddRef'd) or null.
ID3D11PixelShader* Pre(Ctx* c, int kind, UINT count, UINT inst) {
  // Stopping: no swap to a variant Shutdown is about to release.
  if (c != g_ctx || t_inHook || g_fault.load(std::memory_order_relaxed) || g_stopping.load(std::memory_order_relaxed))
    return nullptr;
  t_inHook = true;
  ID3D11PixelShader* restore = nullptr;
  __try {
    restore = PreBody(c, kind, count, inst);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    g_fault = true;
  }
  t_inHook = false;
  return restore;
}
// d3d11 may submit inside DCS's draw or a state call and toggle the table ("after flush" entries over ours); each
// hook puts the arm back after forwarding, adopting d3d11's current entry (never another hook of ours: that one keeps
// the slot, ours stays out of the chain for the rest of the pass - DCS's stock path, no gain, nothing wrong).
std::atomic<uint64_t> g_rearms{0};
void Rearm(Ctx* c);
void Post(Ctx* c, ID3D11PixelShader* restore) {
  if (restore) {
    t_inHook = true;
    c->PSSetShader(restore, nullptr, 0);
    restore->Release();
    t_inHook = false;
  }
  Rearm(c);
}
using DrawFn = void(STDMETHODCALLTYPE*)(Ctx*, UINT, UINT);
using DrawIdxFn = void(STDMETHODCALLTYPE*)(Ctx*, UINT, UINT, INT);
using DrawInstFn = void(STDMETHODCALLTYPE*)(Ctx*, UINT, UINT, UINT, UINT);
using DrawIdxInstFn = void(STDMETHODCALLTYPE*)(Ctx*, UINT, UINT, UINT, INT, UINT);
using DrawAutoFn = void(STDMETHODCALLTYPE*)(Ctx*);
using IndFn = void(STDMETHODCALLTYPE*)(Ctx*, ID3D11Buffer*, UINT);
using DispFn = void(STDMETHODCALLTYPE*)(Ctx*, UINT, UINT, UINT);
#define ES_BODY(K, FN, COUNT, INST, ...)                    \
  ID3D11PixelShader* r = Pre(c, K, COUNT, INST);            \
  reinterpret_cast<FN>(g_orig[K])(__VA_ARGS__);             \
  Post(c, r);
void STDMETHODCALLTYPE E_Draw(Ctx* c, UINT n, UINT sv) { ES_BODY(kDDraw, DrawFn, n, 1, c, n, sv) }
void STDMETHODCALLTYPE E_DrawIndexed(Ctx* c, UINT n, UINT si, INT bv) { ES_BODY(kDIdx, DrawIdxFn, n, 1, c, n, si, bv) }
void STDMETHODCALLTYPE E_DrawInstanced(Ctx* c, UINT n, UINT k, UINT sv, UINT si) {
  ES_BODY(kDInst, DrawInstFn, n, k, c, n, k, sv, si)
}
void STDMETHODCALLTYPE E_DrawIndexedInstanced(Ctx* c, UINT n, UINT k, UINT si, INT bv, UINT sk) {
  ES_BODY(kDIdxInst, DrawIdxInstFn, n, k, c, n, k, si, bv, sk)
}
void STDMETHODCALLTYPE E_DrawAuto(Ctx* c) { ES_BODY(kDAuto, DrawAutoFn, 0, 0, c) }
void STDMETHODCALLTYPE E_DrawIdxInd(Ctx* c, ID3D11Buffer* b, UINT o) { ES_BODY(kDIdxInd, IndFn, 0, 0, c, b, o) }
void STDMETHODCALLTYPE E_DrawInstInd(Ctx* c, ID3D11Buffer* b, UINT o) { ES_BODY(kDInstInd, IndFn, 0, 0, c, b, o) }
void STDMETHODCALLTYPE E_Dispatch(Ctx* c, UINT x, UINT y, UINT z) { ES_BODY(kDDisp, DispFn, x * y * z, 1, c, x, y, z) }
void STDMETHODCALLTYPE E_DispatchInd(Ctx* c, ID3D11Buffer* b, UINT o) { ES_BODY(kDDispInd, IndFn, 0, 0, c, b, o) }
#undef ES_BODY
void* const kHooks[kDkCount] = {
    reinterpret_cast<void*>(&E_Draw),          reinterpret_cast<void*>(&E_DrawIndexed),
    reinterpret_cast<void*>(&E_DrawInstanced), reinterpret_cast<void*>(&E_DrawIndexedInstanced),
    reinterpret_cast<void*>(&E_DrawAuto),      reinterpret_cast<void*>(&E_DrawIdxInd),
    reinterpret_cast<void*>(&E_DrawInstInd),   reinterpret_cast<void*>(&E_Dispatch),
    reinterpret_cast<void*>(&E_DispatchInd)};
void Rearm(Ctx* c) {
  if (c != g_ctx || g_armDepth < 0 || !g_vt || *reinterpret_cast<void***>(c) != g_vt) return;
  // Stopping (Shutdown restores the slots) or faulted (the pass end takes the arms out): never armed again.
  if (g_stopping.load(std::memory_order_relaxed) || g_fault.load(std::memory_order_relaxed)) return;
  for (int k = 0; k < kDkCount; ++k) {
    if (!g_armed[k]) continue;
    void* cur = g_vt[g_slot[k]];
    if (cur == kHooks[k] || d3ds::InPayload(cur) || !fixdepth::InD3d11(cur)) continue;
    g_orig[k] = cur;
    d3ds::WriteTablePointer(&g_vt[g_slot[k]], kHooks[k]);
    g_rearms.fetch_add(1, std::memory_order_relaxed);
  }
}

// Another table hook of this payload that re-arms over whatever it finds (gpu_pass_timing.h, the D3D meter) is in:
// stay out (two such hooks adopt each other as the original: a call cycle).
bool ForeignTable() {
  return gpt::g_opsHooked.load(std::memory_order_relaxed) || d3ds::g_vtblCount.load(std::memory_order_relaxed) > 0;
}
// d3d11 toggles 24 draw / copy / update / clear entries of the immediate context's table to "after flush" variants
// on Flush, ExecuteCommandList, GetData or a waiting Map; the first call through one of them writes the normal
// entries back (fixation_depth.h, R47 / R32 4.5). An arm taken on a toggled table would vanish at DCS's first draw.
// So the table is normalized first by one private 16-byte upload through d3d11's current entry (render thread).
ID3D11Buffer* g_tiny = nullptr;
void Normalize() {
  if (!g_tiny && g_dev) {
    D3D11_BUFFER_DESC bd{16, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER};
    if (FAILED(g_dev->CreateBuffer(&bd, nullptr, &g_tiny))) g_tiny = nullptr;
  }
  if (!g_tiny) return;
  static const uint32_t zero[4] = {};
  g_ctx->UpdateSubresource(g_tiny, 0, nullptr, zero, 0, 0);
}
// Arms the slots that hold d3d11's own entry (never over another hook, ours or a proxy's). k0..k1: slot range.
bool Arm(int k0, int k1) {
  if (!g_ctx || ForeignTable()) return false;
  Normalize();
  void** vt = *reinterpret_cast<void***>(g_ctx);
  g_vt = vt;
  bool any = false;
  for (int k = k0; k < k1; ++k) {
    if (g_slot[k] < 0 || g_armed[k]) continue;
    void* cur = vt[g_slot[k]];
    if (cur == kHooks[k] || d3ds::InPayload(cur) || !fixdepth::InD3d11(cur)) {
      g_armRefused.fetch_add(1, std::memory_order_relaxed);
      continue;
    }
    g_orig[k] = cur;
    d3ds::WriteTablePointer(&vt[g_slot[k]], kHooks[k]);
    g_armed[k] = true;
    any = true;
  }
  return any;
}
// Returns true when every armed slot still held our hook (none lost to a d3d11 toggle).
bool Disarm() {
  bool kept = true;
  for (int k = 0; k < kDkCount; ++k) {
    if (!g_armed[k]) continue;
    g_armed[k] = false;
    if (g_vt && g_vt[g_slot[k]] == kHooks[k])
      d3ds::WriteTablePointer(&g_vt[g_slot[k]], g_orig[k]);
    else
      kept = false;
  }
  if (!kept) g_armLost.fetch_add(1, std::memory_order_relaxed);
  return kept;
}
bool AnyArmed() {
  for (bool a : g_armed)
    if (a) return true;
  return false;
}
bool ArmedAll(int k0, int k1) {
  for (int k = k0; k < k1; ++k)
    if (!g_armed[k]) return false;
  return true;
}

// Census frame boundaries and read-back (render thread, at top-level pass begins).
void CensusFrameStep(Ctx* c, uint64_t frame) {
  const int st = g_cState.load(std::memory_order_acquire);
  if (st == 1) {
    g_cFrame = frame;
    g_classes.clear();
    g_classIdx.clear();
    g_passOcc.clear();
    g_cPasses = g_cDrawsTotal = g_cLostPasses = g_cRefusedPasses = 0;
    g_cbSnaps = 0;
    g_cMapped = 0;
    g_cWaitFrames = 0;
    g_cState.store(2, std::memory_order_release);
    return;
  }
  if (st == 2 && frame != g_cFrame) {
    g_cState.store(3, std::memory_order_release);
    return;
  }
  if (st != 3) return;
  // read back the constant-buffer snapshots without waiting
  for (; g_cMapped < g_classes.size(); ++g_cMapped) {
    ClassRec& r = g_classes[g_cMapped];
    for (int i = 0; i < 14; ++i) {
      if (!r.cbStage[i] || !r.cbData[i].empty()) continue;
      D3D11_MAPPED_SUBRESOURCE m{};
      const HRESULT hr = c->Map(r.cbStage[i], 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m);
      if (hr == DXGI_ERROR_WAS_STILL_DRAWING && ++g_cWaitFrames < 2000) return;
      if (SUCCEEDED(hr)) {
        r.cbData[i].resize(r.sig.cbBytes[i] / 4);
        memcpy(r.cbData[i].data(), m.pData, r.cbData[i].size() * 4);
        c->Unmap(r.cbStage[i], 0);
      }
      r.cbStage[i]->Release();
      r.cbStage[i] = nullptr;
    }
  }
  g_cState.store(4, std::memory_order_release);
}

void OnBoundaryBody(void* pass, bool begin, int depth) {
  {
    if (begin) {
      if (depth != 0) return;
      // between passes, nothing of ours in the table: the fingerprint's command list and the census frame steps
      if (!AnyArmed()) {
        FpRenderStep(g_ctx);
        CensusFrameStep(g_ctx, ptiming::g_execFrame.load(std::memory_order_relaxed));
      }
      const bool census = g_cState.load(std::memory_order_relaxed) == 2;
      TakeCacheReset();
      const std::string& label = LabelOf(pass);
      const bool ssao = g_passSsao[pass] && g_identWanted.load(std::memory_order_relaxed) && g_ssao.build.load() == 1;
      if (census) {
        g_curPass = label;
        g_curOcc = g_passOcc[label]++;
        ++g_cPasses;
      }
      g_swapPass = false;
      if (ssao) g_ssaoPasses.fetch_add(1, std::memory_order_relaxed);
      const bool heat = (g_mask.load(std::memory_order_relaxed) & 2u) && HeatKind(pass, label) == 1;
      if (heat) {
        HeatFrame(ptiming::g_execFrame.load(std::memory_order_relaxed));
        ++g_heatSeen;
        g_heatDraws = 0;
        static const int eclSlot = DcsQv_ContextSlot("ExecuteCommandList");
        void** vt = *reinterpret_cast<void***>(g_ctx);
        g_heatEclWatched = eclSlot >= 0 && vt[eclSlot] == reinterpret_cast<void*>(&sfilt::H_ExecuteCommandList);
        g_heatEcl0 = sfilt::g_eclCount.load(std::memory_order_relaxed);
      }
      if (census || ssao || heat) {
        const bool armed = Arm(0, census ? kDkCount : kDDisp);
        if (census && !armed) ++g_cRefusedPasses;
        g_swapPass = ssao && armed;
        g_armDepth = armed ? 0 : -1;
        // A heat pass is watched only with every draw entry armed: a draw through a refused slot (another hook) would
        // not be counted, and the frame would read idle while heat is drawn.
        const bool allDraws = armed && ArmedAll(0, kDDisp);
        g_heatPass = heat && allDraws;
        if (heat && !allDraws) {
          g_heatAllIdle = false;
          g_heatUnwatched.fetch_add(1, std::memory_order_relaxed);
        }
      }
      return;
    }
    if (depth != g_armDepth) return;
    g_armDepth = -1;
    g_swapPass = false;
    const bool wasHeat = g_heatPass;
    g_heatPass = false;
    const bool kept = Disarm();
    if (wasHeat) {
      const bool ecl = !g_heatEclWatched || sfilt::g_eclCount.load(std::memory_order_relaxed) != g_heatEcl0;
      if (!kept || ecl) {
        g_heatAllIdle = false;
        g_heatUnwatched.fetch_add(1, std::memory_order_relaxed);
      } else if (g_heatDraws) {
        g_heatAllIdle = false;
        g_heatBusy.fetch_add(1, std::memory_order_relaxed);
      }
    }
    if (!kept && g_cState.load(std::memory_order_relaxed) == 2) ++g_cLostPasses;
  }
}
void OnBoundary(void* pass, bool begin, int depth) {
  if (g_stopping.load(std::memory_order_relaxed)) return;
  if (GetCurrentThreadId() != ptiming::g_topTid.load(std::memory_order_relaxed) || !g_ctx) return;
  if (g_fault.load(std::memory_order_relaxed)) {
    // A fault in a draw hook (Pre) mid-pass: the arms of that pass come out at its end, as without the fault (they
    // would otherwise stay in DCS's table for the session and every other feature's per-pass arm be refused).
    if (!begin && depth == g_armDepth) {
      g_armDepth = -1;
      g_swapPass = g_heatPass = false;
      __try {
        Disarm();
      } __except (EXCEPTION_EXECUTE_HANDLER) {
      }
    }
    return;
  }
  __try {
    OnBoundaryBody(pass, begin, depth);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    g_fault = true;
    g_armDepth = -1;
    g_swapPass = g_heatPass = false;
    if (AnyArmed()) Disarm();
  }
}

// --- census report (worker) ---
std::string HexHash(const std::array<uint8_t, 16>& h) {
  char b[40];
  for (int i = 0; i < 16; ++i) snprintf(b + 2 * i, 3, "%02x", h[i]);
  return b;
}
float CbFloat(const std::vector<uint32_t>& d, uint32_t off) {
  if (off / 4 >= d.size()) return std::numeric_limits<float>::quiet_NaN();
  float f;
  memcpy(&f, &d[off / 4], 4);
  return f;
}
// The idle columns of a class (hint fits only): values and verdict text.
std::string IdleText(const ClassRec& r, const std::unordered_map<std::string, uint64_t>& heatDrawsByOcc) {
  char b[256];
  switch (r.hint) {
    case kHGlass: {
      const auto& d = r.cbData[kIcingCb];
      if (d.empty()) return "no cPerFrame snapshot";
      float v[4];
      for (int i = 0; i < 4; ++i) v[i] = CbFloat(d, kIcingOff + 4 * i);
      snprintf(b, sizeof(b), "gCockpitIcing %g %g %g %g: %s", v[0], v[1], v[2], v[3], IcingIdle(v) ? "IDLE" : "active");
      return b;
    }
    case kHOverload: {
      const auto& d = r.cbData[kOverloadCb];
      if (d.empty()) return "no $Globals snapshot";
      const float df = CbFloat(d, 36), bw = CbFloat(d, 40), ov = CbFloat(d, 44);
      snprintf(b, sizeof(b), "defocus %g blackwhitening %g overload %g: %s", df, bw, ov, OverloadIdle(df, bw, ov) ? "IDLE" : "active");
      return b;
    }
    case kHDroplets: {
      const auto& d = r.cbData[kDropletsCb];
      if (d.empty()) return "no $Globals snapshot";
      const float td = CbFloat(d, 0), wf = CbFloat(d, 16), ec = CbFloat(d, 20);
      snprintf(b, sizeof(b), "timeDelta %g windForce %g effectClear %g: CPU part %s (map neutrality not checked)", td, wf, ec,
               DropletsCpuIdle(td, wf) ? "idle" : "active");
      return b;
    }
    case kHHotAir: {
      const auto it = heatDrawsByOcc.find(std::to_string(r.occ));
      const uint64_t n = it == heatDrawsByOcc.end() ? 0 : it->second;
      snprintf(b, sizeof(b), "heat particle draws in HotAirPass #%d: %llu: %s", r.occ, static_cast<unsigned long long>(n),
               HotAirIdle(n) ? "IDLE (if the heat map was cleared)" : "active");
      return b;
    }
    case kHCirrus:
      return "drawn (no constant gate; see the weather)";
    default:
      return "";
  }
}
// The per-class rows. heat draws: occurrence index -> draws of the SimplePassData:HotAirPass classes.
bool WriteCensus(int key, std::string* summary) {
  std::unordered_map<std::string, uint64_t> heat;
  for (const ClassRec& r : g_classes)
    if (r.pass.find("HotAirPass") != std::string::npos) heat[std::to_string(r.occ)] += r.draws;
  wchar_t name[64];
  swprintf_s(name, L"early_census_%d.csv", key);
  const std::wstring path = g_logDir + name;
  FILE* f = nullptr;
  if (_wfopen_s(&f, path.c_str(), L"w") != 0 || !f) return false;
  fprintf(f, "pass,occurrence,draws,dispatches,verts_or_groups,instances,stage_objects,created,identity,hint,idle,bindings\n");
  int early = 0, known = 0, hinted = 0;
  for (const ClassRec& r : g_classes) {
    std::string created, ident;
    void* objs[4] = {r.vs, r.ps, r.gs, r.cs};
    const char* tags[4] = {"vs", "ps", "gs", "cs"};
    char ob[200] = {};
    snprintf(ob, sizeof(ob), "vs %p ps %p gs %p cs %p", r.vs, r.ps, r.gs, r.cs);
    bool anyEarly = false;
    for (int i = 0; i < 4; ++i) {
      if (!objs[i]) continue;
      std::array<uint8_t, 16> h;
      if (i == 3) {
        created += std::string(created.empty() ? "" : " ") + "cs:not recorded";
        continue;
      }
      if (CreatedHash(objs[i], &h)) {
        created += std::string(created.empty() ? "" : " ") + tags[i] + ":" + HexHash(h);
      } else {
        created += std::string(created.empty() ? "" : " ") + tags[i] + ":early";
        anyEarly = true;
      }
    }
    const int n = g_ssao.nIdent.load();
    for (int i = 0; i < n; ++i)
      if (g_ssao.ident[i] == r.ps) ident = kSsaoName[g_ssao.identKind[i]];
    anyEarly ? ++early : ++known;
    if (r.hint >= 0) ++hinted;
    fprintf(f, "\"%s\",%d,%llu,%llu,%llu,%llu,%s,%s,%s,%s,\"%s\",\"%s\"\n", r.pass.c_str(), r.occ,
            static_cast<unsigned long long>(r.draws), static_cast<unsigned long long>(r.dispatches),
            static_cast<unsigned long long>(r.verts), static_cast<unsigned long long>(r.insts), ob, created.c_str(),
            ident.c_str(), r.hint >= 0 ? kHints[r.hint].name : "", r.hint >= 0 ? IdleText(r, heat).c_str() : "",
            SigText(r.sig).c_str());
  }
  fclose(f);
  char s[512];
  snprintf(s, sizeof(s),
           "early census %d: frame %llu, %llu top-level passes (%llu refused an arm, %llu lost it to a d3d11 toggle), "
           "%llu draws / dispatches seen, %zu classes (%d with a shader created before the record, %d all recorded), "
           "%d fit an idle-pass hint; creation record %s; %ls",
           key, static_cast<unsigned long long>(g_cFrame), static_cast<unsigned long long>(g_cPasses),
           static_cast<unsigned long long>(g_cRefusedPasses), static_cast<unsigned long long>(g_cLostPasses),
           static_cast<unsigned long long>(g_cDrawsTotal), g_classes.size(), early, known, hinted,
           g_createHooked.load() ? "in since payload start" : "NOT in (every shader counts as early)", path.c_str());
  *summary = s;
  return true;
}

// --- lifecycle ---
bool AcquireDcs() {
  if (g_ctx) return true;
  ID3D11Device* dev = nullptr;
  ID3D11DeviceContext* ctx = nullptr;
  if (chainfast::g_ctx && chainfast::g_dcsDev) {
    dev = chainfast::g_dcsDev;
    dev->AddRef();
  } else if (shadowinst::g_device) {
    dev = shadowinst::g_device;
    dev->AddRef();
  } else if (sfilt::Ctx* c = sfilt::g_ctx.load()) {
    c->GetDevice(&dev);
  }
  if (!dev) return false;
  dev->GetImmediateContext(&ctx);
  if (!ctx) {
    dev->Release();
    return false;
  }
  g_dev = dev;
  g_ctx = ctx;
  return true;
}
std::wstring ShaderRoot() {
  wchar_t exe[MAX_PATH * 2] = {};
  const DWORD n = GetModuleFileNameW(nullptr, exe, MAX_PATH * 2);
  std::wstring p(exe, n);
  for (int up = 0; up < 2; ++up) {  // ...\bin\DCS.exe (or bin-mt) -> install root
    const size_t s = p.find_last_of(L"\\/");
    if (s == std::wstring::npos) return L"";
    p.resize(s);
  }
  return p + L"\\Bazar\\shaders\\";
}
bool CreateSsaoObjects() {
  for (int k = 0; k < 2; ++k) {
    if (FAILED(g_dev->CreatePixelShader(g_ssao.blobs.stock[k].data(), g_ssao.blobs.stock[k].size(), nullptr, &g_ssao.ours[k])) ||
        FAILED(g_dev->CreatePixelShader(g_ssao.blobs.variant[k].data(), g_ssao.blobs.variant[k].size(), nullptr,
                                        &g_ssao.variant[k])))
      return false;
  }
  std::vector<uint8_t> vs;
  std::string err;
  if (!chainfast::LoadCompiler()) return false;
  ID3DBlob* code = nullptr;
  ID3DBlob* e = nullptr;
  if (FAILED(chainfast::g_compile(kFpVs, sizeof(kFpVs) - 1, "qv_fp_vs", nullptr, nullptr, "main", "vs_5_0", 0, 0, &code, &e)) ||
      !code) {
    if (e) e->Release();
    return false;
  }
  if (e) e->Release();
  const HRESULT hr = g_dev->CreateVertexShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &g_ssao.fpVs);
  code->Release();
  return SUCCEEDED(hr);
}
void ReleaseSsaoObjects() {
  for (int k = 0; k < 2; ++k) {
    if (g_ssao.ours[k]) g_ssao.ours[k]->Release(), g_ssao.ours[k] = nullptr;
    if (g_ssao.variant[k]) g_ssao.variant[k]->Release(), g_ssao.variant[k] = nullptr;
  }
  if (g_ssao.fpVs) g_ssao.fpVs->Release(), g_ssao.fpVs = nullptr;
}

bool Install() {
  if (g_installed.load()) return true;
  if (!AcquireDcs()) return false;
  for (int k = 0; k < kDkCount; ++k) g_slot[k] = DcsQv_ContextSlot(kDkNames[k]);
  for (int k = 0; k < kDkCount; ++k)
    if (g_slot[k] < 0) return false;
  ptiming::Install();
  g_stopping = false;
  ptiming::g_earlyBoundary = &OnBoundary;
  g_installed = true;
  return true;
}

// Worker, every second. mask: [D3D] EarlyShaders (kill switch / benchmark: 0); censusKey: [Suite] EarlyShaderCensus.
void Tick(uint32_t mask, int censusKey, const std::wstring& logDir) {
  g_logDir = logDir;
  static uint32_t lastMask = 0xffffffffu;
  const uint32_t refused = mask & 0x1Cu;
  if (mask != lastMask) {
    if (lastMask != 0xffffffffu || mask)
      Log("early shaders: [D3D] EarlyShaders=0x%x (SSAO water skip %s, hot-air idle skip %s)%s", mask,
          (mask & 1u) ? "on" : "off", (mask & 2u) ? "on" : "off",
          refused ? "; bits 2-4 (other idle-pass skips) are not built: refused" : "");
    lastMask = mask;
  }
  g_mask = mask & 3u;
  // With a census key set, DCS's SSAO shaders are identified too (the census's identity column); nothing is swapped
  // unless bit 0 is on.
  g_identWanted = (mask & 1u) || censusKey != 0;
  const bool wantCensus = censusKey != 0 && censusKey != g_cKey.load();
  if (!g_identWanted.load() && !wantCensus && !(mask & 2u) && !g_installed.load() && g_cState.load() == 0) return;
  if (!Install()) return;
  if (mask & 2u) ptiming::g_intercept2.store(&HeatIntercept);  // stays in once set; it checks the bit itself
  // SSAO build (once per payload)
  if (g_identWanted.load() && g_ssao.build.load() == 0) {
    const std::wstring root = ShaderRoot();
    if (BuildSsao(root, &g_ssao.blobs) && CreateSsaoObjects()) {
      g_ssao.build = 1;
      Log("early shaders: SSAO recipe ready: our builds of DCS's deferred\\SSAO.fx reproduce the proven code "
          "(64 %016llx, 128 %016llx), variants pinned; DCS's SSAO shaders are identified by output at their first "
          "SSAOPassData draw (water skip %s)",
          static_cast<unsigned long long>(kSsaoProven[0].stock), static_cast<unsigned long long>(kSsaoProven[1].stock),
          (mask & 1u) ? "on" : "off: identification only");
    } else {
      ReleaseSsaoObjects();
      g_ssao.build = -1;
      Log("early shaders: SSAO recipe not available (DCS's SSAO stays stock, not identified): %s",
          g_ssao.blobs.why.empty() ? "objects not created" : g_ssao.blobs.why.c_str());
    }
  }
  // fingerprint jobs
  const ULONGLONG now = GetTickCount64();
  if (FpJob* j = g_ssao.job.load()) {
    const int st = j->state.load();
    // A job still waiting for the render thread (state 1: no top-level pass yet, e.g. a mission-load stall) is never
    // given up; one executed but not read back within 20 s, or whose read-back failed, is retried (up to 3 attempts
    // per object) instead of leaving DCS's shader unidentified for the whole session.
    const ULONGLONG te = j->tExec.load(std::memory_order_relaxed);
    const bool stuck = st == 2 && te != 0 && now - te > 20000;
    if (st == 3 || st == -1 || stuck) {
      g_ssao.job = nullptr;
      const int v = st == 3 ? FpVerdict(*j) : -1;
      ++g_ssao.tested;
      // A stuck job (state 2) may still be read back by the render thread, which loaded it before the store above:
      // its objects stay until Shutdown (the done list). States 3 / -1 are the render thread's last touch.
      if (!stuck) j->ReleaseGpu();
      if (st != 3 && ++g_ssao.retries[j->dcs] < 3) {
        ++g_ssao.retried;
        Log("early shaders: fingerprint of SSAO-pass pixel shader %p %s: retried (attempt %d of 3)",
            static_cast<void*>(j->dcs), st == -1 ? "failed to read back" : "not read back in 20 s",
            g_ssao.retries[j->dcs] + 1);
        {
          std::lock_guard<std::mutex> lock(g_ssao.candMu);
          g_ssao.cands.push_back(j->dcs);  // the candidate's reference moves back to the queue
        }
      } else if (v >= 0 && g_ssao.nIdent.load() < 8) {
        const int i = g_ssao.nIdent.load();
        g_ssao.ident[i] = j->dcs;  // keeps the reference
        g_ssao.identKind[i] = v;
        g_ssao.nIdent.store(i + 1, std::memory_order_release);
        ++g_ssao.accepted;
        Log("early shaders: DCS's pixel shader %p equals our %s in %u of %u synthetic cases (and not the other "
            "kernel): taken as that kernel",
            static_cast<void*>(j->dcs), kSsaoName[v], kFpCases, kFpCases);
      } else {
        ++g_ssao.rejected;
        if (g_ssao.rejected <= 8)
          Log("early shaders: SSAO-pass pixel shader %p not identified (%s): left stock", static_cast<void*>(j->dcs),
              st != 3 ? (st == -1 ? "read back failed 3 times" : "not read back in 20 s, 3 times")
              : v == -2 ? "equals both kernels" : v == -3 ? "cases do not tell the kernels apart" : "output differs from both kernels");
        j->dcs->Release();
      }
      j->dcs = nullptr;
      g_ssao.done.push_back(j);
    }
  } else if (g_ssao.build.load() == 1) {
    ID3D11PixelShader* cand = nullptr;
    {
      std::lock_guard<std::mutex> lock(g_ssao.candMu);
      if (!g_ssao.cands.empty()) {
        cand = g_ssao.cands.front();
        g_ssao.cands.erase(g_ssao.cands.begin());
      }
    }
    if (cand) {
      FpJob* nj = new FpJob();
      nj->dcs = cand;
      nj->t0 = now;
      if (FpRecord(*nj, g_dev)) {
        nj->state = 1;
        g_ssao.job = nj;
      } else {
        Log("early shaders: fingerprint job for %p not recorded: left stock", static_cast<void*>(cand));
        nj->ReleaseGpu();
        cand->Release();
        nj->dcs = nullptr;
        g_ssao.done.push_back(nj);
      }
    }
  }
  // census: a finished one is written (and its buffers released) before a new key arms the next
  if (g_cState.load() == 4) {
    std::string s;
    if (WriteCensus(g_cKey.load(), &s)) Log("%s", s.c_str());
    else Log("early census %d: file not written", g_cKey.load());
    for (ClassRec& r : g_classes)
      for (auto*& p : r.cbStage)
        if (p) p->Release(), p = nullptr;
    g_classes.clear();
    g_classIdx.clear();
    g_cState = 0;
  }
  if (wantCensus && g_cState.load() == 0) {
    g_cKey = censusKey;
    g_cState = 1;
    Log("early shaders: census %d armed (next frame)", censusKey);
  }
  static ULONGLONG lastHeat = 0;
  if ((mask & 2u) && now - lastHeat >= 60000) {
    lastHeat = now;
    Log("early shaders: hot air: copy / compose passes skipped %llu, run %llu (heat drawn %llu, pass not watched %llu, "
        "pass order %llu)",
        static_cast<unsigned long long>(g_heatSkipped.load()), static_cast<unsigned long long>(g_heatRun.load()),
        static_cast<unsigned long long>(g_heatBusy.load()), static_cast<unsigned long long>(g_heatUnwatched.load()),
        static_cast<unsigned long long>(g_heatOrder.load()));
  }
  static ULONGLONG lastLine = 0;
  if ((mask & 1u) && now - lastLine >= 60000) {
    lastLine = now;
    Log("early shaders: SSAO passes %llu, draws seen %llu, variant draws %llu; identified %d, rejected %d; arms refused "
        "%llu, lost to a d3d11 toggle %llu",
        static_cast<unsigned long long>(g_ssaoPasses.load()), static_cast<unsigned long long>(g_ssaoDraws.load()),
        static_cast<unsigned long long>(g_swaps.load()), g_ssao.accepted, g_ssao.rejected,
        static_cast<unsigned long long>(g_armRefused.load()), static_cast<unsigned long long>(g_armLost.load()));
  }
}

// Payload start: the creation record goes in only when the census key is already set (dev ini).
void AtStart(int censusKey) {
  if (censusKey == 0 || g_createHooked.load()) return;
  if (InstallCreateRecord())
    Log("early shaders: shader creation record in (census key %d set at payload start)", censusKey);
  else
    Log("early shaders: shader creation record NOT installed (a census will count every shader as early)");
}

void Shutdown() {
  g_stopping = true;
  g_mask = 0;  // the hot-air intercept passes everything through from now on
  {
    ptiming::InterceptFn me = &HeatIntercept;
    ptiming::g_intercept2.compare_exchange_strong(me, nullptr);
  }
  if (g_installed.exchange(false)) {
    ptiming::BoundaryFn me = &OnBoundary;
    ptiming::g_earlyBoundary.compare_exchange_strong(me, nullptr);
    Sleep(50);  // a pass boundary already inside OnBoundary finishes (and a draw inside Pre: g_stopping is set)
    g_armDepth = -1;  // Rearm stays out (it also checks g_stopping)
    g_swapPass = g_heatPass = false;
    for (int k = 0; k < kDkCount; ++k)
      if (g_armed[k] && g_vt && g_vt[g_slot[k]] == kHooks[k]) d3ds::WriteTablePointer(&g_vt[g_slot[k]], g_orig[k]);
    for (bool& a : g_armed) a = false;
    Sleep(20);
  }
  if (g_createHooked.exchange(false)) {
    void* o[3] = {reinterpret_cast<void*>(g_createPs), reinterpret_cast<void*>(g_createVs), reinterpret_cast<void*>(g_createGs)};
    for (int i = 0; i < 3; ++i)
      if (g_createSlot[i]) UnhookSlot(g_createSlot[i], o[i]);
  }
  if (FpJob* j = g_ssao.job.exchange(nullptr)) g_ssao.done.push_back(j);
  for (FpJob* j : g_ssao.done) {
    j->ReleaseGpu();
    if (j->dcs) j->dcs->Release();
    delete j;
  }
  g_ssao.done.clear();
  {
    std::lock_guard<std::mutex> lock(g_ssao.candMu);
    for (auto* p : g_ssao.cands) p->Release();
    g_ssao.cands.clear();
  }
  for (int i = 0; i < g_ssao.nIdent.load(); ++i)
    if (g_ssao.ident[i]) g_ssao.ident[i]->Release(), g_ssao.ident[i] = nullptr;
  g_ssao.nIdent = 0;
  for (void* p : g_ssao.probed) static_cast<ID3D11PixelShader*>(p)->Release();
  g_ssao.probed.clear();
  ReleaseSsaoObjects();
  g_ssao.build = 0;
  for (ClassRec& r : g_classes)
    for (auto*& p : r.cbStage)
      if (p) p->Release(), p = nullptr;
  g_classes.clear();
  g_cState = 0;
  if (g_tiny) g_tiny->Release(), g_tiny = nullptr;
  if (g_ctx) g_ctx->Release(), g_ctx = nullptr;
  if (g_dev) g_dev->Release(), g_dev = nullptr;
}

}  // namespace eshd
