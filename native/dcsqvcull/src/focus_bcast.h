// R38 rank 2: intra-eye geometry broadcast (periphery + focus of one eye in
// one geometry pass). Offline building blocks, a live measure-only probe, and
// a readiness check. Nothing here changes DCS's output unless a suite probe
// phase runs (the probe is lossy for its 5 s ON runs, like LodProbe).
//
// 1. [Suite] FocusGeomProbe = view mask (bit v = HMD view v: 1/2 periphery
//    L/R, 4/8 focus L/R; 12 = both focus views; default 0 = off). GPU pass
//    timing OFF / ON / OFF / ON / OFF (5 s each; GpuPassStats adds the
//    pipeline counts). While ON, every RSSetViewports DCS issues inside the
//    selected views' main GBufferPass (the view's first GBufferPass, as R41's
//    GeoOnlyProbe) is replaced by a 16x16 viewport at (-64, -64): every
//    triangle is still fetched, vertex-shaded, clipped and set up, but no
//    pixel is written. The pass's ON time is the geometry ("front-end") part
//    a broadcast can share; stock minus ON is the per-view raster/PS part.
//    The viewports DCS asked for last are bound again at the pass end, so the
//    next pass sees stock state. Differences to R41's GeoOnlyProbe
//    (fixation_depth.h): its own RSSetViewports arm, independent of the depth
//    publisher's arm and of FixationDepth, and it runs inside the suite's GPU
//    pass timing (R41's was bypassed there, scratchpad r41 blockC2).
//    Limits: draws replayed from command lists (gb_rec's model draws) carry
//    their own viewports and stay stock: run with [Model] GBufferRecorder=0,
//    or read the remaining PS invocations in GpuPassStats (the probe's
//    coverage). The view index counts CopyToHMDPass ends from the frame's
//    HeightMapPass / first cascade (periph L, periph R, focus L, focus R).
//
// 2. Building blocks for FastGS broadcast (pure, tested offline; not bound
//    anywhere in this build):
//    - SolveAffine: focus clip = affine of periphery clip (x_f = ax x_p + bx w,
//      y_f = ay y_p + by w, z and w rows equal), from the two projection or
//      view-projection matrices (rows as in cPerView / the census CBs). The
//      DLSS jitter of both views is inside bx, by.
//    - BroadcastViewport: the viewport that maps periphery NDC onto the focus
//      target (Width = ax * focus width, TopLeftX = x0 + (bx + 1 - ax) w / 2).
//    - FixMv: the motion vector the focus PS would have written, from the one
//      the periphery's PS wrote (DCS's calcMotionVector: MV = 0.5 dims
//      (prevNDC - curNDC)), exact for a moving gaze (current and previous
//      affines); kMvFixHlsl is the same as a full-screen pass.
//    - ParseOutputSignature / FastGsHlsl / CheckGsMatches: a FastGS
//      passthrough generated from a VS output signature (DXBC OSGN/OSG5/OSG1),
//      checked by reflection to reproduce the VS's register layout.
//    - CreateFastGs: NvAPI_D3D11_CreateFastGeometryShaderExplicit with
//      NV_FASTGS_USE_VIEWPORT_MASK (+ OFFSET_RT_INDEX_BY_VP_INDEX).
//
// 3. [D3D] FocusBroadcast (default 0 = nothing). 1 = readiness check only:
//    once, on DCS's device, NVAPI is loaded and a passthrough FastGS (a
//    built-in signature) is created and released; the result is logged.
//    Nothing is bound and DCS's output is unchanged. The broadcast itself is
//    not built (README_R38R2.md: gates and the VS-signature blocker).
//
// Included once from main.cpp after fixation_depth.h (uses plod::, gpt::,
// ptiming::, fixdepth::VpHook / g_vpOrig).
#pragma once

#include "nvapi_fgs_min.h"

namespace fbc {

// ---------------------------------------------------------------------------
// Pure math
// ---------------------------------------------------------------------------

// A 4x4 clip transform as four rows: clip.c = dot(row[c], p).
struct Rows {
  double r[4][4];
};
struct Affine {
  double ax = 1, bx = 0, ay = 1, by = 0;
  double residual = 0;  // largest deviation from the affine model, relative to the row scale
};

// Focus rows = (ax rowX_p + bx rowW, ay rowY_p + by rowW, rowZ_p, rowW_p).
// False when z / w rows differ, a cross term is present, or the residual is above tol.
inline bool SolveAffine(const Rows& p, const Rows& f, Affine& out, double tol = 1e-4) {
  auto norm = [](const double* v) { return std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2] + v[3] * v[3]); };
  const double* w = p.r[3];
  const double ww = w[0] * w[0] + w[1] * w[1] + w[2] * w[2] + w[3] * w[3];
  if (ww <= 0) return false;
  double res = 0;
  for (int row = 2; row < 4; ++row) {
    const double sc = std::max(1e-12, norm(p.r[row]));
    for (int k = 0; k < 4; ++k) res = std::max(res, std::fabs(p.r[row][k] - f.r[row][k]) / sc);
  }
  double a[2], b[2];
  for (int row = 0; row < 2; ++row) {
    const double* pr = p.r[row];
    const double* fr = f.r[row];
    // a from the component where the periphery row is largest and w is smallest relative to it.
    int best = -1;
    double bestQ = 0;
    for (int k = 0; k < 4; ++k) {
      const double q = std::fabs(pr[k]) / (std::fabs(w[k]) + 1e-12 * (1 + std::fabs(pr[k])));
      if (std::fabs(pr[k]) > 1e-9 && q > bestQ) bestQ = q, best = k;
    }
    if (best < 0) return false;
    // Solve [pr w] [a b]^T = fr in least squares (2 unknowns).
    double s_pp = 0, s_pw = 0, s_ww = 0, s_pf = 0, s_wf = 0;
    for (int k = 0; k < 4; ++k) {
      s_pp += pr[k] * pr[k], s_pw += pr[k] * w[k], s_ww += w[k] * w[k];
      s_pf += pr[k] * fr[k], s_wf += w[k] * fr[k];
    }
    const double det = s_pp * s_ww - s_pw * s_pw;
    if (std::fabs(det) < 1e-18) return false;
    a[row] = (s_pf * s_ww - s_wf * s_pw) / det;
    b[row] = (s_wf * s_pp - s_pf * s_pw) / det;
    const double sc = std::max(1e-12, norm(fr));
    for (int k = 0; k < 4; ++k) res = std::max(res, std::fabs(a[row] * pr[k] + b[row] * w[k] - fr[k]) / sc);
  }
  out.ax = a[0], out.bx = b[0], out.ay = a[1], out.by = b[1];
  out.residual = res;
  return res <= tol && out.ax > 0 && out.ay > 0;
}

// The viewport that maps periphery NDC onto a focus target whose stock viewport is (x0, y0, w, h).
// False when it leaves D3D11's viewport bounds [-32768, 32767].
inline bool BroadcastViewport(const Affine& a, float x0, float y0, float w, float h, D3D11_VIEWPORT& v) {
  v.Width = static_cast<float>(a.ax * w);
  v.Height = static_cast<float>(a.ay * h);
  v.TopLeftX = static_cast<float>(x0 + (a.bx + 1.0 - a.ax) * w * 0.5);
  v.TopLeftY = static_cast<float>(y0 + (1.0 - a.by - a.ay) * h * 0.5);
  v.MinDepth = 0, v.MaxDepth = 1;
  return v.TopLeftX >= D3D11_VIEWPORT_BOUNDS_MIN && v.TopLeftY >= D3D11_VIEWPORT_BOUNDS_MIN &&
         v.TopLeftX + v.Width <= D3D11_VIEWPORT_BOUNDS_MAX && v.TopLeftY + v.Height <= D3D11_VIEWPORT_BOUNDS_MAX &&
         v.Width > 0 && v.Height > 0 && v.Width <= D3D11_VIEWPORT_BOUNDS_MAX && v.Height <= D3D11_VIEWPORT_BOUNDS_MAX;
}

// Motion-vector fixup for one focus pixel (centre px + 0.5, py + 0.5 of a dimsF target).
// mvP: what the periphery's PS wrote there (0.5 dimsP (prevNDC_p - curNDC_p), NDC y up).
// cur / prev: the focus-of-periphery affines of the matrices calcMotionVector uses (this frame's and the
// previous frame's). jitterF: the focus's current jitter in NDC when those matrices are unjittered (else 0).
// Returns 0.5 dimsF (prevNDC_f - curNDC_f), the focus PS's own value.
struct Mv {
  double x, y;
};
inline Mv FixMv(double px, double py, Mv mvP, const Affine& cur, const Affine& prev, double dimsPx, double dimsPy,
                double dimsFx, double dimsFy, double jitterFx = 0, double jitterFy = 0) {
  const double nfx = (px + 0.5) / dimsFx * 2.0 - 1.0 - jitterFx;  // focus NDC of the pixel centre (MV space)
  const double nfy = 1.0 - (py + 0.5) / dimsFy * 2.0 - jitterFy;
  const double cpx = (nfx - cur.bx) / cur.ax, cpy = (nfy - cur.by) / cur.ay;  // periphery NDC, current
  const double ppx = cpx + 2.0 * mvP.x / dimsPx, ppy = cpy + 2.0 * mvP.y / dimsPy;  // periphery NDC, previous
  const double pfx = prev.ax * ppx + prev.bx, pfy = prev.ay * ppy + prev.by;        // focus NDC, previous
  return Mv{0.5 * dimsFx * (pfx - nfx), 0.5 * dimsFy * (pfy - nfy)};
}

// The same as a full-screen pixel shader over the focus MV target (copied in from the broadcast): pixels whose
// depth is the clear value (reverse-Z: 0) keep their value.
constexpr char kMvFixHlsl[] = R"(
cbuffer Fix : register(b0) {
  float4 Cur;     // ax, bx, ay, by (this frame)
  float4 Prev;    // ax, bx, ay, by (previous frame)
  float4 Dims;    // periphery w, h, focus w, h
  float4 Jit;     // focus jitter x, y (NDC; 0 when the MV matrices are jittered)
};
Texture2D<float2> MvIn : register(t0);
Texture2D<float> Depth : register(t1);
float2 main(float4 pos : SV_Position) : SV_Target0 {
  int2 p = int2(pos.xy);
  float2 mv = MvIn.Load(int3(p, 0));
  if (Depth.Load(int3(p, 0)) == 0) return mv;
  float2 nf = float2(pos.x / Dims.z * 2 - 1, 1 - pos.y / Dims.w * 2) - Jit.xy;
  float2 cp = (nf - float2(Cur.y, Cur.w)) / float2(Cur.x, Cur.z);
  float2 pp = cp + 2 * mv / Dims.xy;
  float2 pf = float2(Prev.x, Prev.z) * pp + float2(Prev.y, Prev.w);
  return 0.5 * Dims.zw * (pf - nf);
}
)";

// ---------------------------------------------------------------------------
// VS output signature -> FastGS passthrough
// ---------------------------------------------------------------------------

struct SigElem {
  std::string name;
  uint32_t index = 0, sysValue = 0, compType = 0, reg = 0, stream = 0, minPrec = 0;
  uint8_t mask = 0;
};

// DXBC output signature (OSGN, OSG5 or OSG1). False when absent or malformed.
inline bool ParseOutputSignature(const void* code, size_t size, std::vector<SigElem>& out) {
  out.clear();
  const uint8_t* b = static_cast<const uint8_t*>(code);
  if (size < 32 || memcmp(b, "DXBC", 4) != 0) return false;
  uint32_t n = 0;
  memcpy(&n, b + 28, 4);
  if (n > 64 || 32 + 4ull * n > size) return false;
  for (uint32_t i = 0; i < n; ++i) {
    uint32_t off = 0;
    memcpy(&off, b + 32 + 4 * i, 4);
    if (off + 8ull > size) return false;
    const uint8_t* c = b + off;
    uint32_t len = 0;
    memcpy(&len, c + 4, 4);
    const bool osgn = memcmp(c, "OSGN", 4) == 0, osg5 = memcmp(c, "OSG5", 4) == 0, osg1 = memcmp(c, "OSG1", 4) == 0;
    if (!osgn && !osg5 && !osg1) continue;
    if (off + 8ull + len > size || len < 8) return false;
    const uint8_t* d = c + 8;
    uint32_t cnt = 0;
    memcpy(&cnt, d, 4);
    const size_t es = osg1 ? 32 : osg5 ? 28 : 24;
    if (8 + cnt * es > len) return false;
    for (uint32_t e = 0; e < cnt; ++e) {
      const uint8_t* p = d + 8 + e * es;
      SigElem s;
      size_t q = 0;
      if (osg5 || osg1) memcpy(&s.stream, p, 4), q = 4;
      uint32_t nameOff = 0;
      memcpy(&nameOff, p + q, 4);
      memcpy(&s.index, p + q + 4, 4);
      memcpy(&s.sysValue, p + q + 8, 4);
      memcpy(&s.compType, p + q + 12, 4);
      memcpy(&s.reg, p + q + 16, 4);
      s.mask = p[q + 20];
      if (osg1) memcpy(&s.minPrec, p + q + 24, 4);
      if (nameOff >= len) return false;
      const char* nm = reinterpret_cast<const char*>(d + nameOff);
      s.name.assign(nm, strnlen(nm, len - nameOff));
      out.push_back(s);
    }
    return true;
  }
  return false;
}

// HLSL of an explicit FastGS passthrough for that signature: input = the VS's outputs in register order (fxc's
// first-fit packing then reproduces an fxc-packed layout; CheckGsMatches proves it per signature), output
// = the same plus SV_ViewportArrayIndex (the viewport mask) and, with rtOffset, SV_RenderTargetArrayIndex = 0.
// Empty when the signature holds what a passthrough cannot reproduce (clip/cull distances, min precision,
// streams other than 0, an existing viewport / RT index).
inline std::string FastGsHlsl(const std::vector<SigElem>& sig, uint32_t mask, bool rtOffset) {
  std::vector<SigElem> s = sig;
  std::sort(s.begin(), s.end(), [](const SigElem& a, const SigElem& b) {
    if (a.reg != b.reg) return a.reg < b.reg;
    return (a.mask & -a.mask) < (b.mask & -b.mask);
  });
  std::string fields;
  int f = 0;
  for (const SigElem& e : s) {
    if (e.stream || e.minPrec) return {};
    if (e.sysValue == 2 || e.sysValue == 3 || e.sysValue == 4 || e.sysValue == 5) return {};  // clip/cull, RT, VP
    if (e.sysValue != 0 && e.sysValue != 1) return {};  // only SV_Position among system values
    const int comps = __popcnt(e.mask);
    if (comps < 1 || comps > 4) return {};
    // A mask with a hole (e.g. .xz) cannot be declared as one HLSL field.
    const int lo = static_cast<int>(_tzcnt_u32(e.mask));
    if (static_cast<uint32_t>(((1u << comps) - 1) << lo) != e.mask) return {};
    const char* base = e.compType == 1 ? "uint" : e.compType == 2 ? "int" : e.compType == 3 ? "float" : nullptr;
    if (!base) return {};
    char line[256];
    const std::string sem = e.sysValue == 1 ? std::string("SV_Position")
                                            : e.name + std::to_string(e.index);
    const char* interp = e.compType == 3 ? "" : "nointerpolation ";
    snprintf(line, sizeof(line), "  %s%s%s f%d : %s;\n", interp, base, comps > 1 ? std::to_string(comps).c_str() : "",
             f++, sem.c_str());
    fields += line;
  }
  std::string h = "struct V {\n" + fields + "};\nstruct G {\n" + fields +
                  "  uint fbcVp : SV_ViewportArrayIndex;\n" +
                  (rtOffset ? "  uint fbcRt : SV_RenderTargetArrayIndex;\n" : "") +
                  "};\n[maxvertexcount(1)] void main(triangle V i[3], inout TriangleStream<G> s) {\n  G o;\n";
  for (int k = 0; k < f; ++k) h += "  o.f" + std::to_string(k) + " = i[0].f" + std::to_string(k) + ";\n";
  h += "  o.fbcVp = " + std::to_string(mask) + "u;\n";
  if (rtOffset) h += "  o.fbcRt = 0u;\n";
  h += "  s.Append(o);\n}\n";
  return h;
}

// The compiled GS's input signature reproduces the VS's output signature (register, mask, type, semantic),
// and its output is the same plus the viewport index (and RT index).
inline bool CheckGsMatches(const std::vector<SigElem>& vsOut, const void* gs, size_t gsSize, bool rtOffset) {
  // The GS input signature is the ISGN chunk; parse it with the same layout (24-byte entries).
  const uint8_t* b = static_cast<const uint8_t*>(gs);
  if (gsSize < 32 || memcmp(b, "DXBC", 4) != 0) return false;
  uint32_t n = 0;
  memcpy(&n, b + 28, 4);
  std::vector<SigElem> in;
  for (uint32_t i = 0; i < n && 32 + 4ull * n <= gsSize; ++i) {
    uint32_t off = 0;
    memcpy(&off, b + 32 + 4 * i, 4);
    if (off + 8ull > gsSize || memcmp(b + off, "ISGN", 4) != 0) continue;
    // Re-label the chunk as OSGN in a copy and reuse the parser.
    std::vector<uint8_t> copy(b, b + gsSize);
    memcpy(&copy[off], "OSGN", 4);
    // Hide any real output chunk so the parser finds this one first: rename the others.
    for (uint32_t j = 0; j < n; ++j) {
      uint32_t o2 = 0;
      memcpy(&o2, b + 32 + 4 * j, 4);
      if (o2 != off && o2 + 4ull <= gsSize &&
          (memcmp(b + o2, "OSGN", 4) == 0 || memcmp(b + o2, "OSG5", 4) == 0 || memcmp(b + o2, "OSG1", 4) == 0))
        memcpy(&copy[o2], "XXXX", 4);
    }
    if (!ParseOutputSignature(copy.data(), copy.size(), in)) return false;
    break;
  }
  if (in.size() != vsOut.size()) return false;
  for (const SigElem& v : vsOut) {
    bool found = false;
    for (const SigElem& g : in)
      if (g.reg == v.reg && g.mask == v.mask && g.compType == v.compType && g.sysValue == v.sysValue &&
          (v.sysValue == 1 || (g.index == v.index && _stricmp(g.name.c_str(), v.name.c_str()) == 0)))
        found = true;
    if (!found) return false;
  }
  std::vector<SigElem> out;
  if (!ParseOutputSignature(gs, gsSize, out)) return false;
  size_t vp = 0, rt = 0;
  for (const SigElem& o : out) vp += o.sysValue == 5, rt += o.sysValue == 4;
  return out.size() == vsOut.size() + 1 + (rtOffset ? 1 : 0) && vp == 1 && rt == (rtOffset ? 1u : 0u);
}

inline ID3DBlob* CompileHlsl(const std::string& src, const char* entry, const char* target, std::string* err) {
  using CompileFn = HRESULT(WINAPI*)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*, LPCSTR, LPCSTR,
                                     UINT, UINT, ID3DBlob**, ID3DBlob**);
  static CompileFn compile = [] {
    HMODULE m = LoadLibraryW(L"d3dcompiler_47.dll");
    return m ? reinterpret_cast<CompileFn>(GetProcAddress(m, "D3DCompile")) : nullptr;
  }();
  if (!compile) {
    if (err) *err = "d3dcompiler_47.dll not available";
    return nullptr;
  }
  ID3DBlob *code = nullptr, *msg = nullptr;
  const HRESULT hr = compile(src.data(), src.size(), "fbc", nullptr, nullptr, entry, target, 1 << 15 /* OPT L3 */, 0,
                             &code, &msg);
  if (FAILED(hr) && err && msg) *err = static_cast<const char*>(msg->GetBufferPointer());
  if (msg) msg->Release();
  return SUCCEEDED(hr) ? code : nullptr;
}

// NvAPI FastGS (explicit) for a VS output signature. Status in *status (NvAPI_Status; -1000 no NVAPI,
// -1001 signature not reproducible, -1002 compile, -1003 layout check).
inline ID3D11GeometryShader* CreateFastGs(ID3D11Device* dev, const std::vector<SigElem>& sig, uint32_t mask,
                                          bool rtOffset, int* status) {
  static nvfgs::Api api;
  if (status) *status = -1000;
  if (!nvfgs::Load(api) || !api.createFastGs) return nullptr;
  const std::string h = FastGsHlsl(sig, mask, rtOffset);
  if (h.empty()) {
    if (status) *status = -1001;
    return nullptr;
  }
  ID3DBlob* b = CompileHlsl(h, "main", "gs_5_0", nullptr);
  if (!b) {
    if (status) *status = -1002;
    return nullptr;
  }
  if (!CheckGsMatches(sig, b->GetBufferPointer(), b->GetBufferSize(), rtOffset)) {
    b->Release();
    if (status) *status = -1003;
    return nullptr;
  }
  nvfgs::FastGsExplicitDesc d{nvfgs::kFastGsExplicitVer,
                              nvfgs::kFastGsUseViewportMask | (rtOffset ? nvfgs::kFastGsOffsetRtIndexByVpIndex : 0u),
                              nullptr};
  ID3D11GeometryShader* gs = nullptr;
  const int st = api.createFastGs(dev, b->GetBufferPointer(), b->GetBufferSize(), nullptr, &d, &gs);
  b->Release();
  if (status) *status = st;
  if (st != nvfgs::kNvapiOk) {
    if (gs) gs->Release();
    return nullptr;
  }
  return gs;
}

// ---------------------------------------------------------------------------
// [D3D] FocusBroadcast = 1: readiness check (once; nothing bound)
// ---------------------------------------------------------------------------

std::atomic<bool> g_readyDone{false};
bool (*g_readyGate)() = nullptr;  // R53: focus_bcast_run's start gate (frames flowing) when FocusBroadcast=2
// The signature of DCS's speedtree G-buffer VS family is not known at run time (DCS creates its shaders before
// the payload loads); this check uses a representative one (position + 5 interpolants).
inline std::vector<SigElem> ProbeSignature() {
  std::vector<SigElem> s(6);
  s[0] = {"SV_Position", 0, 1, 3, 0, 0, 0, 0xf};
  s[1] = {"TEXCOORD", 0, 0, 3, 1, 0, 0, 0x3};
  s[2] = {"TEXCOORD", 4, 0, 3, 1, 0, 0, 0x4};  // fxc packs a later float into register 1's free .z
  s[3] = {"TEXCOORD", 1, 0, 3, 2, 0, 0, 0x7};
  s[4] = {"TEXCOORD", 2, 0, 3, 3, 0, 0, 0xf};
  s[5] = {"TEXCOORD", 3, 0, 3, 4, 0, 0, 0xf};
  return s;
}
inline void Readiness(int mode) {
  if (mode <= 0 || g_readyDone.load()) return;
  if (g_readyGate && !g_readyGate()) return;  // retried by the next config tick
  if (!plod::EnsureDevice()) return;  // DCS's device not captured yet: retried by the next config tick
  g_readyDone = true;
  nvfgs::Api api;
  const bool nv = nvfgs::Load(api);
  int mv = -1, st = -1;
  nvfgs::QueryBits q{nvfgs::kQueryMultiViewVer, 0};
  if (nv && api.queryMultiView) mv = api.queryMultiView(plod::g_dev, &q);
  D3D11_FEATURE_DATA_D3D11_OPTIONS3 o3{};
  plod::g_dev->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS3, &o3, sizeof(o3));
  int st2 = -1;
  ID3D11GeometryShader* gs = CreateFastGs(plod::g_dev, ProbeSignature(), 3, false, &st);
  ID3D11GeometryShader* gs2 = CreateFastGs(plod::g_dev, ProbeSignature(), 3, true, &st2);
  Log("focus broadcast: readiness (nothing bound): NVAPI %s (init %d); MultiView query %d bits 0x%x; VPAndRTArrayIndex "
      "from any shader %d; FastGS viewport mask: status %d%s, with RT offset: status %d%s",
      nv ? "loaded" : "not available", api.initStatus, mv, q.bits,
      static_cast<int>(o3.VPAndRTArrayIndexFromAnyShaderFeedingRasterizer), st, gs ? " (created)" : "", st2,
      gs2 ? " (created)" : "");
  if (gs) gs->Release();
  if (gs2) gs2->Release();
}

// ---------------------------------------------------------------------------
// [Suite] FocusGeomProbe: geometry-only main G-buffer pass (measure only)
// ---------------------------------------------------------------------------

using VpFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, const D3D11_VIEWPORT*);
std::atomic<uint32_t> g_mask{0};   // views whose main GBufferPass is probed (0 = probe off)
// Render-thread state.
int g_view = 0;                    // CopyToHMDPass ends since the frame's HeightMap / first cascade
int g_gbInView = 0;                // GBufferPass begins in the current view
int g_gbCount[4] = {}, g_gbPrev[4] = {};  // GBufferPass runs per view this frame / last frame (R44)
// R44 live fix: the view's main GBufferPass is its second-to-last one (live 2.9.30: 3 per view, a small first
// pass, the main pass, a tiny last one; 2 per view in the R38 census: main, last). n = last frame's count.
inline int MainOrdinal(int n) { return n >= 3 ? n - 2 : 0; }
bool g_active = false;             // inside a probed main GBufferPass, hook armed
void** g_table = nullptr;          // the table our hook was written into
VpFn g_next = nullptr;             // what our hook forwards to
VpFn g_d3d = nullptr;              // the last entry seen that was neither ours nor the depth publisher's
D3D11_VIEWPORT g_req[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
UINT g_reqN = 0;                   // DCS's last requested viewports in the pass (restored at the pass end)
bool g_reqSeen = false;
thread_local int t_in = 0;
std::atomic<uint64_t> g_passes{0}, g_overrides{0}, g_passesMissed{0}, g_buried{0}, g_reentry{0}, g_viewSeen[4];

void STDMETHODCALLTYPE VpHook(ID3D11DeviceContext* c, UINT n, const D3D11_VIEWPORT* v) {
  VpFn next = g_next;
  if (t_in > 0) {  // a cycle through another hook that took ours as its original: go straight to d3d11
    ++g_reentry;
    next = g_d3d;
  }
  if (!next) return;
  if (!g_active || c != plod::g_ctx || GetCurrentThreadId() != ptiming::g_topTid.load(std::memory_order_relaxed) ||
      !n || !v) {
    ++t_in;
    next(c, n, v);
    --t_in;
    return;
  }
  const UINT m = std::min<UINT>(n, D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE);
  memcpy(g_req, v, m * sizeof(D3D11_VIEWPORT));
  g_reqN = m;
  g_reqSeen = true;
  D3D11_VIEWPORT off[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
  for (UINT i = 0; i < m; ++i) off[i] = D3D11_VIEWPORT{-64.0f, -64.0f, 16.0f, 16.0f, v[i].MinDepth, v[i].MaxDepth};
  ++g_overrides;
  ++t_in;
  next(c, m, off);
  --t_in;
}

int VpSlot() {
  static int s = DcsQv_ContextSlot("RSSetViewports");
  return s;
}

void Arm() {
  void** vt = *reinterpret_cast<void***>(plod::g_ctx);
  const int s = VpSlot();
  if (s < 0) return;
  void* cur = vt[s];
  if (cur != reinterpret_cast<void*>(&VpHook)) {
    // The depth publisher's hook on top whose original is ours already: we are in its chain (buried at an
    // earlier pass end); do not wrap it again.
    const bool chained = cur == reinterpret_cast<void*>(&fixdepth::VpHook) &&
                         reinterpret_cast<void*>(fixdepth::g_vpOrig) == reinterpret_cast<void*>(&VpHook);
    if (!chained) {
      if (cur != reinterpret_cast<void*>(&fixdepth::VpHook)) g_d3d = reinterpret_cast<VpFn>(cur);
      g_next = reinterpret_cast<VpFn>(cur);
      g_table = vt;
      d3ds::WriteTablePointer(&vt[s], reinterpret_cast<void*>(&VpHook));
    }
  }
  if (!g_d3d) g_d3d = g_next;
  // Viewports bound before the pass (DCS may set none inside it): saved, and replaced at once.
  UINT n = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
  plod::g_ctx->RSGetViewports(&n, g_req);
  g_reqN = n;
  g_reqSeen = false;
  g_active = true;
  if (n) {
    D3D11_VIEWPORT pre[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
    memcpy(pre, g_req, n * sizeof(D3D11_VIEWPORT));
    VpHook(plod::g_ctx, n, pre);  // through our hook: replaced, and remembered as DCS's request
  }
  g_reqSeen = false;
}

// Our hook out of the table, or out of the depth publisher's chain (suite thread, after the phase; the code
// stays callable either way: the payload module is pinned).
void Unwrap() {
  if (!plod::g_ctx || !g_next) return;
  const int s = VpSlot();
  if (s < 0) return;
  void** vt = *reinterpret_cast<void***>(plod::g_ctx);
  if (vt[s] == reinterpret_cast<void*>(&VpHook)) d3ds::WriteTablePointer(&vt[s], reinterpret_cast<void*>(g_next));
  if (reinterpret_cast<void*>(fixdepth::g_vpOrig) == reinterpret_cast<void*>(&VpHook))
    fixdepth::g_vpOrig = reinterpret_cast<fixdepth::VpFn>(g_next);
  g_table = nullptr;
}

void Disarm() {
  if (!g_active) return;
  g_active = false;
  // Stock state for whatever follows: DCS's last request (or what was bound at the pass begin).
  if (g_reqN && g_next) {
    ++t_in;
    g_next(plod::g_ctx, g_reqN, g_req);
    --t_in;
  }
  if (!g_reqSeen) ++g_passesMissed;
  const int s = VpSlot();
  if (g_table && s >= 0) {
    if (g_table[s] == reinterpret_cast<void*>(&VpHook)) {
      d3ds::WriteTablePointer(&g_table[s], reinterpret_cast<void*>(g_next));
      g_table = nullptr;
    } else {
      ++g_buried;  // another hook went on top: ours stays in its chain as a pass-through (g_active false)
    }
  }
}

// periph_lod.h's boundary chain (render thread; every pass, nested ones too).
void OnBoundary(void* pass, bool begin, int depth) {
  if (depth != 0 || !plod::g_ctx) return;
  const uint32_t mask = g_mask.load(std::memory_order_relaxed);
  const int k = plod::KindOf(pass);
  if (begin && (k == plod::kHeight || k == plod::kCascade)) {
    if (g_view != 0 || g_gbInView != 0) {
      for (int i = 0; i < 4; ++i) g_gbPrev[i] = g_gbCount[i], g_gbCount[i] = 0;
      g_view = 0, g_gbInView = 0;
    }
    return;
  }
  if (k == plod::kHmd && !begin) {
    ++g_view;
    g_gbInView = 0;
    return;
  }
  if (k != plod::kGBuffer) return;
  if (begin) {
    const int v = g_view;
    const int ord = g_gbInView++;
    if (v >= 0 && v < 4) g_gbCount[v] = g_gbInView;
    const bool main = ord == MainOrdinal(v >= 0 && v < 4 ? g_gbPrev[v] : 0);
    if (main && v >= 0 && v < 4) g_viewSeen[v].fetch_add(1, std::memory_order_relaxed);
    if (mask && main && v >= 0 && v < 4 && (mask >> v & 1)) {
      ++g_passes;
      Arm();
    }
  } else {
    Disarm();
  }
}

// GPU pass timing OFF / ON / OFF / ON / OFF, 5 s each.
void RunPhase(uint32_t mask, std::atomic<uint64_t>& frameCounter) {
  if (!plod::InstallCallback()) {
    Log("  focus geom probe: pass boundary callback not available; phase skipped");
    return;
  }
  if (VpSlot() < 0) {
    Log("  focus geom probe: RSSetViewports slot unknown; phase skipped");
    return;
  }
  for (auto& s : g_viewSeen) s = 0;
  plod::g_probe = &OnBoundary;
  for (int i = 0; i < 5 && !g_benchAbort.load() && !g_stop.load(); ++i) {
    const bool on = (i & 1) != 0;
    g_mask = on ? mask : 0;
    Sleep(1000);
    Log("  focus geom probe run %d: %s", i + 1,
        on ? "ON (selected views' main GBufferPass into a 16x16 viewport at (-64,-64))" : "OFF");
    const uint64_t p0 = g_passes.load(), o0 = g_overrides.load(), m0 = g_passesMissed.load();
    gpt::Measure(5000, frameCounter);
    if (on)
      Log("  focus geom probe: %llu passes probed, %llu viewports replaced, %llu passes with no viewport set inside "
          "(the pre-pass one replaced), %llu buried pass-throughs, %llu re-entries",
          static_cast<unsigned long long>(g_passes.load() - p0), static_cast<unsigned long long>(g_overrides.load() - o0),
          static_cast<unsigned long long>(g_passesMissed.load() - m0), static_cast<unsigned long long>(g_buried.load()),
          static_cast<unsigned long long>(g_reentry.load()));
  }
  g_mask = 0;
  Sleep(200);  // an armed pass ends at its pass end
  plod::BFn me = &OnBoundary;
  plod::g_probe.compare_exchange_strong(me, nullptr);
  Unwrap();
  Log("  focus geom probe: main GBufferPass seen per view %llu / %llu / %llu / %llu (expected equal)",
      static_cast<unsigned long long>(g_viewSeen[0].load()), static_cast<unsigned long long>(g_viewSeen[1].load()),
      static_cast<unsigned long long>(g_viewSeen[2].load()), static_cast<unsigned long long>(g_viewSeen[3].load()));
}

void Shutdown() {
  if (g_mask.exchange(0)) Sleep(100);
  plod::BFn me = &OnBoundary;
  plod::g_probe.compare_exchange_strong(me, nullptr);
  Unwrap();
}

}  // namespace fbc
