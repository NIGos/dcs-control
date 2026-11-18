// Offline equivalence harness for exact shader replacements: the bytecode rewrites of src/dxbc_patch.h
// ([ShaderExact], exact-shader-batch worktree; those commands are built only where that header exists) and, in this
// tree, the 0.7 early-shader variants built from DCS's own source (early_shaders.h, `pair`).
//
//   sx_equiv scan <dir|file> <outdir> [want]     rewrite every .dxbc; write <id>.sx.dxbc, <id>.stock.asm, <id>.sx.asm
//   sx_equiv run  <dir|file> <rules> [options]   run stock and rewritten shader on the same inputs, compare bits
//        options: -dev warp|hw|both   -cases N   -seed S   -want MASK   -csv FILE   -v
//   sx_equiv pair <stock.dxbc> <variant.dxbc> <rules> [-dev warp|hw|both] [-cases N] [-seed S] [-texsize WxH]
//        [-csv FILE] [-expect same|differ] [-id NAME]
//        run DCS's stock blob and a variant on the same inputs, compare every output bit. -expect differ is a
//        negative control (a variant broken on purpose must be reported). Pixel-shader runs also count the stock
//        render target 0 .x values equal to 1.0 ("trivial" pixels) and the others, so a rules file can be shown to
//        reach both sides of a branch.
//
// Vertex shaders: every output register of every vertex, captured by stream output. Pixel shaders: every render
// target (as 32-bit float or integer) and the depth buffer (coverage: a discarded pixel keeps the cleared depth).
// Inputs are random but shaped by the rules file so that index chains stay in range and flag words take every
// combination that matters (see rules\*.txt). The same rewrite code as the engine module is used.
//
// Not part of the shipped module. Build: scripts in artifacts are not needed; one translation unit:
//   cl /O2 /EHsc /std:c++17 sx_equiv.cpp d3d11.lib d3dcompiler.lib dxguid.lib
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#if defined(__has_include)
#if __has_include("../../src/dxbc_patch.h")
#include "../../src/dxbc_patch.h"
#define SX_PATCH 1
#endif
#endif
#ifndef SX_PATCH
#define SX_PATCH 0
#endif

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "dxguid.lib")

template <class T>
struct Com {
  T* p = nullptr;
  Com() = default;
  Com(const Com&) = delete;
  Com& operator=(const Com&) = delete;
  Com(Com&& o) noexcept : p(o.p) { o.p = nullptr; }
  ~Com() { if (p) p->Release(); }
  T* operator->() const { return p; }
  T** operator&() { if (p) p->Release(), p = nullptr; return &p; }
  operator T*() const { return p; }
};

static std::vector<uint8_t> ReadFile(const std::string& f) {
  std::vector<uint8_t> b;
  FILE* fp = nullptr;
  if (fopen_s(&fp, f.c_str(), "rb") || !fp) return b;
  fseek(fp, 0, SEEK_END);
  const long n = ftell(fp);
  fseek(fp, 0, SEEK_SET);
  b.resize(n > 0 ? n : 0);
  if (n > 0 && fread(b.data(), 1, n, fp) != static_cast<size_t>(n)) b.clear();
  fclose(fp);
  return b;
}
static void WriteFile(const std::string& f, const void* d, size_t n) {
  FILE* fp = nullptr;
  if (fopen_s(&fp, f.c_str(), "wb") || !fp) return;
  fwrite(d, 1, n, fp);
  fclose(fp);
}
static std::vector<std::string> ListBlobs(const std::string& path) {
  std::vector<std::string> v;
  const DWORD a = GetFileAttributesA(path.c_str());
  if (a == INVALID_FILE_ATTRIBUTES) return v;
  if (!(a & FILE_ATTRIBUTE_DIRECTORY)) {
    if (path.size() > 4 && path.substr(path.size() - 4) == ".txt") {  // a list of files, one per line
      const std::vector<uint8_t> b = ReadFile(path);
      std::istringstream is(std::string(b.begin(), b.end()));
      std::string l;
      while (std::getline(is, l)) {
        while (!l.empty() && (l.back() == '\r' || l.back() == ' ')) l.pop_back();
        if (!l.empty()) v.push_back(l);
      }
    } else {
      v.push_back(path);
    }
    return v;
  }
  WIN32_FIND_DATAA fd;
  const HANDLE h = FindFirstFileA((path + "\\*.dxbc").c_str(), &fd);
  if (h == INVALID_HANDLE_VALUE) return v;
  do v.push_back(path + "\\" + fd.cFileName);
  while (FindNextFileA(h, &fd));
  FindClose(h);
  std::sort(v.begin(), v.end());
  return v;
}
static std::string BaseId(const std::string& f) {
  size_t s = f.find_last_of("\\/");
  s = s == std::string::npos ? 0 : s + 1;
  size_t e = f.find('.', s);
  return f.substr(s, e == std::string::npos ? std::string::npos : e - s);
}
static std::string Disasm(const std::vector<uint8_t>& b) {
  Com<ID3DBlob> t;
  if (FAILED(D3DDisassemble(b.data(), b.size(), 0, nullptr, &t)) || !t.p) return "";
  return std::string(static_cast<const char*>(t->GetBufferPointer()));
}

// ---------------------------------------------------------------------------
// Rules: how random inputs are shaped
// ---------------------------------------------------------------------------
struct Rule {
  char kind = 'r';       // 'r' resource / constant buffer word, 'a' attribute (vertex input or pixel input)
  std::string name;      // substring of the resource name, or SEMANTIC+index ("TEXCOORD1")
  int word = -1;         // word in the element (resource), component (attribute); -1 = every word
  std::string gen;
  double a = 0, b = 0;
  bool perTri = false;   // attribute: one value for the three vertices of a triangle
};
static std::vector<Rule> g_rules;
static bool LoadRules(const std::string& f) {
  const std::vector<uint8_t> b = ReadFile(f);
  if (b.empty()) return false;
  std::istringstream is(std::string(b.begin(), b.end()));
  std::string l;
  while (std::getline(is, l)) {
    if (l.empty() || l[0] == '#') continue;
    std::istringstream ls(l);
    std::string k, w;
    Rule r;
    if (!(ls >> k >> r.name >> w >> r.gen)) continue;
    r.kind = k == "attr" || k == "tri" ? 'a' : 'r';
    r.perTri = k == "tri";
    r.word = w == "*" ? -1 : atoi(w.c_str());
    ls >> r.a >> r.b;
    g_rules.push_back(r);
  }
  return true;
}
using Rng = std::mt19937;
static uint32_t FBits(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
static float RandF(Rng& g, double lo, double hi) {
  return static_cast<float>(lo + (hi - lo) * (g() / 4294967296.0));
}
static uint32_t DefaultWord(Rng& g) {
  const uint32_t rare = g() % 400;
  if (rare == 0) return 0x7f800000u;  // +inf
  if (rare == 1) return 0x7fc00000u;  // NaN
  if (rare == 2) return 0x80000000u;  // -0
  const uint32_t k = g() % 16;
  if (k == 0) return FBits(0.0f);
  if (k == 1) return FBits(1.0f);
  if (k == 2) return FBits(-1.0f);
  if (k == 3) return FBits(RandF(g, -1e-3, 1e-3));
  if (k == 4) return FBits(RandF(g, -3000, 3000));
  return FBits(RandF(g, -8, 8));
}
// The building material flag word: every bit the vertex shaders test, the two sway bits jointly absent half the time.
static uint32_t FlagsWord(Rng& g, int mode) {
  uint32_t f = g() & (1u | 2u | 4u | 8u | 16u | 32u | 256u | 512u | 0x20000u | 0x10000000u);
  const uint32_t r = g() % 8;
  const uint32_t sway = mode == 1 ? 0 : mode == 2 ? (1 + r % 3) : (r < 4 ? 0 : r - 4 + (r == 4 ? 1 : 0));
  // sway: 0 none, 1 = 64, 2 = 128, 3 = both
  if (sway & 1) f |= 64;
  if (sway & 2) f |= 128;
  return f;
}
static bool GenWord(const Rule& r, Rng& g, int mode, uint32_t* out) {
  if (r.gen == "idx") *out = r.a > 0 ? g() % static_cast<uint32_t>(r.a) : 0;
  else if (r.gen == "f") *out = FBits(RandF(g, r.a, r.b));
  else if (r.gen == "zf") *out = (g() & 1) ? FBits(0.0f) : FBits(RandF(g, r.a, r.b));
  else if (r.gen == "flags") *out = FlagsWord(g, mode);
  else if (r.gen == "bits") *out = g() & static_cast<uint32_t>(r.a);
  else if (r.gen == "u32") *out = g();
  else if (r.gen == "const") *out = FBits(static_cast<float>(r.a));
  else if (r.gen == "constu") *out = static_cast<uint32_t>(r.a);
  else if (r.gen == "mostly") *out = (g() % 100 < static_cast<uint32_t>(r.b)) ? FBits(static_cast<float>(r.a)) : DefaultWord(g);
  else if (r.gen == "cnt") {  // an instance count: 1..8, 9..200, or far above any id
    const uint32_t k = g() % 5;
    *out = k < 2 ? 1 + g() % 8 : k < 4 ? 9 + g() % 192 : (g() & 1) ? 0x7fffffffu : 0u;
  }
  else if (r.gen == "fz") {  // floats in [a, b], 15 % exactly a, 15 % exactly b
    const uint32_t k = g() % 100;
    *out = FBits(k < 15 ? static_cast<float>(r.a) : k < 30 ? static_cast<float>(r.b) : RandF(g, r.a, r.b));
  }
  else if (r.gen == "stencil") {  // DCS's stencil byte (common/stencil.hlsl): composition type in bits 3..5 (& 0x38),
    // other bits random. Type 2 (16) water at a% (default 35), 5 (40) cockpit 15 %, the other six types the rest.
    static const uint32_t kOther[6] = {0, 8, 24, 32, 48, 56};
    const uint32_t k = g() % 100, water = r.a > 0 ? static_cast<uint32_t>(r.a) : 35u;
    const uint32_t type = k < water ? 16u : k < water + 15 ? 40u : kOther[g() % 6];
    *out = (g() & 0xC7u) | type;
  }
  else if (r.gen == "persp" || r.gen == "rot") return false;  // blocks, written by FillBlocks
  else return false;
  return true;
}

// Blocks of a constant buffer written as a whole (after the per-word rules), in DCS's cbuffer layout: a float4x4
// used as mul(v, M) with HLSL's default column-major packing stores M's columns as the consecutive float4 rows.
//   res <cb> <word> persp   a perspective projection at <word> (16 words) and its inverse right after (16 words):
//                           random field of view, asymmetric centre (quad-view focus), near 0.05..1, far 1e3..1e5,
//                           reversed depth in a third of the cases
//   res <cb> <word> rot     a random rotation (3x3 in a 4x4, w row / column 0 0 0 1) at <word>
static void StoreColumns(const double m[4][4], uint32_t* w) {
  for (int c = 0; c < 4; ++c)
    for (int r = 0; r < 4; ++r) w[4 * c + r] = FBits(static_cast<float>(m[r][c]));
}
static bool Invert4(const double a[4][4], double inv[4][4]) {
  double m[4][8];
  for (int r = 0; r < 4; ++r)
    for (int c = 0; c < 8; ++c) m[r][c] = c < 4 ? a[r][c] : (c - 4 == r ? 1.0 : 0.0);
  for (int c = 0; c < 4; ++c) {
    int p = c;
    for (int r = c + 1; r < 4; ++r)
      if (fabs(m[r][c]) > fabs(m[p][c])) p = r;
    if (fabs(m[p][c]) < 1e-30) return false;
    if (p != c)
      for (int k = 0; k < 8; ++k) std::swap(m[p][k], m[c][k]);
    const double d = m[c][c];
    for (int k = 0; k < 8; ++k) m[c][k] /= d;
    for (int r = 0; r < 4; ++r) {
      if (r == c) continue;
      const double f = m[r][c];
      for (int k = 0; k < 8; ++k) m[r][k] -= f * m[c][k];
    }
  }
  for (int r = 0; r < 4; ++r)
    for (int c = 0; c < 4; ++c) inv[r][c] = m[r][c + 4];
  return true;
}
static void FillBlocks(const std::string& name, std::vector<uint32_t>& data, Rng& g) {
  for (const Rule& r : g_rules) {
    if (r.kind != 'r' || name.find(r.name) == std::string::npos || r.word < 0) continue;
    const size_t at = static_cast<size_t>(r.word);
    if (r.gen == "persp" && at + 32 <= data.size()) {
      const double tx = RandF(g, 0.3, 1.6), ty = RandF(g, 0.3, 1.6), ox = RandF(g, -0.4, 0.4), oy = RandF(g, -0.4, 0.4);
      const double n = RandF(g, 0.05, 1.0), f = RandF(g, 1000.0, 100000.0);
      const bool rev = g() % 3 == 0;
      double p[4][4] = {{1 / tx, 0, 0, 0}, {0, 1 / ty, 0, 0}, {ox, oy, 0, 1}, {0, 0, 0, 0}};
      if (rev) p[2][2] = n / (n - f), p[3][2] = -f * n / (n - f);
      else p[2][2] = f / (f - n), p[3][2] = -n * f / (f - n);
      double inv[4][4];
      if (!Invert4(p, inv)) continue;
      StoreColumns(p, &data[at]);
      StoreColumns(inv, &data[at + 16]);
    } else if (r.gen == "rot" && at + 16 <= data.size()) {
      // random unit quaternion -> rotation matrix
      double q[4], s = 0;
      for (double& x : q) x = RandF(g, -1, 1), s += x * x;
      s = sqrt(s > 1e-12 ? s : 1);
      for (double& x : q) x /= s;
      const double w = q[0], x = q[1], y = q[2], z = q[3];
      const double m[4][4] = {{1 - 2 * (y * y + z * z), 2 * (x * y - w * z), 2 * (x * z + w * y), 0},
                              {2 * (x * y + w * z), 1 - 2 * (x * x + z * z), 2 * (y * z - w * x), 0},
                              {2 * (x * z - w * y), 2 * (y * z + w * x), 1 - 2 * (x * x + y * y), 0},
                              {0, 0, 0, 1}};
      StoreColumns(m, &data[at]);
    }
  }
}
static void FillResource(const std::string& name, int strideWords, std::vector<uint32_t>& data, Rng& g, int mode) {
  for (size_t i = 0; i < data.size(); ++i) data[i] = DefaultWord(g);
  for (const Rule& r : g_rules) {
    if (r.kind != 'r' || name.find(r.name) == std::string::npos) continue;
    for (size_t i = 0; i < data.size(); ++i) {
      const int w = strideWords > 0 ? static_cast<int>(i % strideWords) : static_cast<int>(i);
      if (r.word >= 0 && r.word != w) continue;
      GenWord(r, g, mode, &data[i]);
    }
  }
  if (strideWords == 0) FillBlocks(name, data, g);
}
static bool NamedByRule(const std::string& name) {
  for (const Rule& r : g_rules)
    if (r.kind == 'r' && name.find(r.name) != std::string::npos) return true;
  return false;
}
static UINT g_texW = 0, g_texH = 0;  // -texsize: every 2D texture this size (0: random 32..128)
// A sorted table for the merged-instance search: word 1 of each element ascending (the first vertex of the item).
static void SortField(std::vector<uint32_t>& data, int strideWords, int word, Rng& g, uint32_t step) {
  uint32_t acc = 0;
  for (size_t e = 0; e * strideWords + word < data.size(); ++e) {
    data[e * strideWords + word] = acc;
    acc += g() % (step + 1);
  }
}

// ---------------------------------------------------------------------------
// D3D helpers
// ---------------------------------------------------------------------------
struct Dev {
  Com<ID3D11Device> d;
  Com<ID3D11DeviceContext> c;
  std::string name;
};
static bool MakeDevice(bool warp, Dev* out) {
  const D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
  if (FAILED(D3D11CreateDevice(nullptr, warp ? D3D_DRIVER_TYPE_WARP : D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &fl, 1,
                               D3D11_SDK_VERSION, &out->d, nullptr, &out->c)))
    return false;
  out->name = warp ? "warp" : "hw";
  return true;
}
struct Sig {
  std::string sem;
  UINT index, reg, mask;
  D3D_REGISTER_COMPONENT_TYPE type;
  D3D_NAME sv;
};
struct Bind {
  std::string name;
  D3D_SHADER_INPUT_TYPE type;
  UINT slot, stride, cbBytes;
  D3D_SRV_DIMENSION dim;
  D3D_RESOURCE_RETURN_TYPE ret;
};
struct Refl {
  std::vector<Sig> in, out;
  std::vector<Bind> binds;
};
static bool Reflect(const std::vector<uint8_t>& b, Refl* r) {
  Com<ID3D11ShaderReflection> rf;
  if (FAILED(D3DReflect(b.data(), b.size(), IID_ID3D11ShaderReflection, reinterpret_cast<void**>(&rf)))) return false;
  D3D11_SHADER_DESC sd{};
  rf->GetDesc(&sd);
  for (UINT i = 0; i < sd.InputParameters; ++i) {
    D3D11_SIGNATURE_PARAMETER_DESC p{};
    rf->GetInputParameterDesc(i, &p);
    r->in.push_back({p.SemanticName, p.SemanticIndex, p.Register, p.Mask, p.ComponentType, p.SystemValueType});
  }
  for (UINT i = 0; i < sd.OutputParameters; ++i) {
    D3D11_SIGNATURE_PARAMETER_DESC p{};
    rf->GetOutputParameterDesc(i, &p);
    r->out.push_back({p.SemanticName, p.SemanticIndex, p.Register, p.Mask, p.ComponentType, p.SystemValueType});
  }
  for (UINT i = 0; i < sd.BoundResources; ++i) {
    D3D11_SHADER_INPUT_BIND_DESC bd{};
    rf->GetResourceBindingDesc(i, &bd);
    Bind bi{bd.Name, bd.Type, bd.BindPoint, bd.NumSamples, 0, bd.Dimension, bd.ReturnType};
    if (bd.Type == D3D_SIT_CBUFFER) {
      D3D11_SHADER_BUFFER_DESC cd{};
      rf->GetConstantBufferByName(bd.Name)->GetDesc(&cd);
      bi.cbBytes = cd.Size;
    }
    r->binds.push_back(bi);
  }
  return true;
}

// Resources of one case, shared by the stock and the rewritten shader.
struct Bound {
  std::vector<Com<ID3D11Buffer>> bufs;
  std::vector<Com<ID3D11Texture2D>> texs;
  ID3D11Buffer* cb[14] = {};
  ID3D11ShaderResourceView* srv[128] = {};
  ID3D11SamplerState* smp[16] = {};
  std::vector<ID3D11ShaderResourceView*> ownSrv;
  std::vector<ID3D11SamplerState*> ownSmp;
  ~Bound() {
    for (auto* s : ownSrv) s->Release();
    for (auto* s : ownSmp) s->Release();
  }
};
static bool MakeBound(Dev& dv, const Refl& rf, Rng& g, int mode, int caseNo, Bound* b) {
  b->bufs.reserve(64);
  b->texs.reserve(64);
  for (const Bind& bi : rf.binds) {
    if (bi.type == D3D_SIT_CBUFFER) {
      std::vector<uint32_t> w((bi.cbBytes + 15) / 16 * 4);
      FillResource(bi.name, 0, w, g, mode);
      D3D11_BUFFER_DESC bd{static_cast<UINT>(w.size() * 4), D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER};
      D3D11_SUBRESOURCE_DATA sd{w.data()};
      b->bufs.emplace_back();
      if (bi.slot >= 14 || FAILED(dv.d->CreateBuffer(&bd, &sd, &b->bufs.back()))) return false;
      b->cb[bi.slot] = b->bufs.back();
    } else if (bi.type == D3D_SIT_SAMPLER) {
      D3D11_SAMPLER_DESC sd{};
      const int k = caseNo % 4;
      sd.Filter = k == 3 ? D3D11_FILTER_MIN_MAG_MIP_POINT : k == 2 ? D3D11_FILTER_MIN_MAG_MIP_LINEAR : D3D11_FILTER_ANISOTROPIC;
      sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
      sd.MaxAnisotropy = k == 0 ? 16 : 4;
      sd.MaxLOD = D3D11_FLOAT32_MAX;
      sd.ComparisonFunc = D3D11_COMPARISON_LESS_EQUAL;
      ID3D11SamplerState* s = nullptr;
      if (bi.slot >= 16 || FAILED(dv.d->CreateSamplerState(&sd, &s))) return false;
      b->ownSmp.push_back(s);
      b->smp[bi.slot] = s;
    } else if (bi.type == D3D_SIT_STRUCTURED || (bi.type == D3D_SIT_TEXTURE && bi.dim == D3D_SRV_DIMENSION_BUFFER)) {
      const bool st = bi.type == D3D_SIT_STRUCTURED;
      const UINT stride = st ? bi.stride : 4;
      const UINT elems = st ? 256 : 512;
      std::vector<uint32_t> w(static_cast<size_t>(stride / 4) * elems);
      FillResource(bi.name, stride / 4, w, g, mode);
      for (const Rule& r : g_rules)
        if (r.kind == 'r' && r.gen == "sorted" && bi.name.find(r.name) != std::string::npos)
          SortField(w, stride / 4, r.word, g, static_cast<uint32_t>(r.a));
      D3D11_BUFFER_DESC bd{static_cast<UINT>(w.size() * 4), D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, 0,
                           st ? static_cast<UINT>(D3D11_RESOURCE_MISC_BUFFER_STRUCTURED) : 0u, st ? stride : 0u};
      D3D11_SUBRESOURCE_DATA sd{w.data()};
      b->bufs.emplace_back();
      if (FAILED(dv.d->CreateBuffer(&bd, &sd, &b->bufs.back()))) return false;
      D3D11_SHADER_RESOURCE_VIEW_DESC vd{};
      vd.Format = st ? DXGI_FORMAT_UNKNOWN
                     : bi.ret == D3D_RETURN_TYPE_UINT ? DXGI_FORMAT_R32_UINT
                     : bi.ret == D3D_RETURN_TYPE_SINT ? DXGI_FORMAT_R32_SINT : DXGI_FORMAT_R32_FLOAT;
      vd.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
      vd.Buffer.NumElements = elems;
      ID3D11ShaderResourceView* v = nullptr;
      if (bi.slot >= 128 || FAILED(dv.d->CreateShaderResourceView(b->bufs.back(), &vd, &v))) return false;
      b->ownSrv.push_back(v);
      b->srv[bi.slot] = v;
    } else if (bi.type == D3D_SIT_TEXTURE && (bi.dim == D3D_SRV_DIMENSION_TEXTURE2D || bi.dim == D3D_SRV_DIMENSION_TEXTURE2DARRAY) &&
               (NamedByRule(bi.name) || bi.ret == D3D_RETURN_TYPE_UINT || bi.ret == D3D_RETURN_TYPE_SINT ||
                bi.dim == D3D_SRV_DIMENSION_TEXTURE2DARRAY)) {
      // Typed 32-bit texture (4 words per texel, filled by the rules like a buffer of stride 4), one mip; arrays have
      // 6 slices. Integer textures always take this path (an 8-bit UNORM view is not valid for them).
      const bool arr = bi.dim == D3D_SRV_DIMENSION_TEXTURE2DARRAY;
      const UINT w = g_texW ? g_texW : 32u << (g() % 3), h = g_texH ? g_texH : w, slices = arr ? 6 : 1;
      const DXGI_FORMAT f = bi.ret == D3D_RETURN_TYPE_UINT   ? DXGI_FORMAT_R32G32B32A32_UINT
                            : bi.ret == D3D_RETURN_TYPE_SINT ? DXGI_FORMAT_R32G32B32A32_SINT
                                                             : DXGI_FORMAT_R32G32B32A32_FLOAT;
      D3D11_TEXTURE2D_DESC td{w, h, 1, slices, f, {1, 0}, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE};
      std::vector<std::vector<uint32_t>> px(slices);
      std::vector<D3D11_SUBRESOURCE_DATA> sd(slices);
      for (UINT s = 0; s < slices; ++s) {
        px[s].resize(static_cast<size_t>(w) * h * 4);
        FillResource(bi.name, 4, px[s], g, mode);
        sd[s] = {px[s].data(), w * 16, 0};
      }
      b->texs.emplace_back();
      if (FAILED(dv.d->CreateTexture2D(&td, sd.data(), &b->texs.back()))) return false;
      ID3D11ShaderResourceView* v = nullptr;
      if (bi.slot >= 128 || FAILED(dv.d->CreateShaderResourceView(b->texs.back(), nullptr, &v))) return false;
      b->ownSrv.push_back(v);
      b->srv[bi.slot] = v;
    } else if (bi.type == D3D_SIT_TEXTURE && bi.dim == D3D_SRV_DIMENSION_TEXTURE2D) {
      const UINT size = g_texW ? g_texW : 32u << (g() % 3), mips = 6;
      D3D11_TEXTURE2D_DESC td{g_texW ? g_texW : size * (1 + g() % 2), g_texH ? g_texH : size, mips, 1, DXGI_FORMAT_R8G8B8A8_UNORM, {1, 0}, D3D11_USAGE_DEFAULT,
                              D3D11_BIND_SHADER_RESOURCE};
      std::vector<std::vector<uint8_t>> px(mips);
      std::vector<D3D11_SUBRESOURCE_DATA> sd(mips);
      for (UINT m = 0; m < mips; ++m) {
        const UINT w = std::max(1u, td.Width >> m), h = std::max(1u, td.Height >> m);
        px[m].resize(static_cast<size_t>(w) * h * 4);
        for (auto& x : px[m]) x = static_cast<uint8_t>(g());
        sd[m] = {px[m].data(), w * 4, 0};
      }
      b->texs.emplace_back();
      if (FAILED(dv.d->CreateTexture2D(&td, sd.data(), &b->texs.back()))) return false;
      ID3D11ShaderResourceView* v = nullptr;
      if (bi.slot >= 128 || FAILED(dv.d->CreateShaderResourceView(b->texs.back(), nullptr, &v))) return false;
      b->ownSrv.push_back(v);
      b->srv[bi.slot] = v;
    } else {
      fprintf(stderr, "    unsupported binding %s (type %d dim %d)\n", bi.name.c_str(), bi.type, bi.dim);
      return false;
    }
  }
  return true;
}
static bool IsNan(uint32_t w) { return (w & 0x7f800000u) == 0x7f800000u && (w & 0x007fffffu) != 0; }
// 0 equal, 1 only NaN-for-NaN differences in float words, 2 different. `isFloat` per word, or null = all float.
static int CompareWords(const uint8_t* a, const uint8_t* b, size_t words, const std::vector<char>* isFloat, size_t* firstWord) {
  int r = 0;
  for (size_t i = 0; i < words; ++i) {
    uint32_t x, y;
    memcpy(&x, a + 4 * i, 4);
    memcpy(&y, b + 4 * i, 4);
    if (x == y) continue;
    if ((!isFloat || (*isFloat)[i]) && IsNan(x) && IsNan(y)) {
      if (r == 0) r = 1;
      continue;
    }
    if (r != 2) *firstWord = i;
    r = 2;
  }
  return r;
}
static int Pop(UINT m) { int n = 0; for (; m; m &= m - 1) ++n; return n; }
static int Low(UINT m) { int n = 0; while (m && !(m & 1)) m >>= 1, ++n; return n; }

static bool g_posOnly = false;  // ablation check: compare SV_Position only
struct Stats {
  uint64_t cases = 0, items = 0, words = 0, diffItems = 0, swayOn = 0;
  uint64_t nanItems = 0;  // items whose only difference is which NaN a float holds (both sides NaN)
  uint64_t rt0One = 0, rt0Other = 0;  // pixel shaders: covered pixels whose stock RT0.x is exactly 1.0 / anything else
  std::string first;
};

// ---------------------------------------------------------------------------
// Vertex shaders
// ---------------------------------------------------------------------------
static bool RunVs(Dev& dv, const std::vector<uint8_t>& stock, const std::vector<uint8_t>& sx, int cases, uint32_t seed,
                  Stats* st) {
  Refl rf;
  if (!Reflect(stock, &rf)) return false;
  std::vector<D3D11_SO_DECLARATION_ENTRY> so;
  UINT stride = 0;
  for (const Sig& s : rf.out) {
    so.push_back({0, s.sem.c_str(), s.index, 0, static_cast<BYTE>(Pop(s.mask)), 0});
    stride += 4 * Pop(s.mask);
  }
  Com<ID3D11VertexShader> vs[2];
  Com<ID3D11GeometryShader> gs[2];
  const std::vector<uint8_t>* code[2] = {&stock, &sx};
  for (int k = 0; k < 2; ++k) {
    if (FAILED(dv.d->CreateVertexShader(code[k]->data(), code[k]->size(), nullptr, &vs[k]))) {
      st->first = k ? "the rewritten shader was refused by CreateVertexShader" : "stock shader refused";
      return false;
    }
    if (FAILED(dv.d->CreateGeometryShaderWithStreamOutput(code[k]->data(), code[k]->size(), so.data(),
                                                          static_cast<UINT>(so.size()), &stride, 1,
                                                          D3D11_SO_NO_RASTERIZED_STREAM, nullptr, &gs[k]))) {
      st->first = "stream output shader refused";
      return false;
    }
  }
  // vertex inputs that are not system generated
  std::vector<D3D11_INPUT_ELEMENT_DESC> il;
  for (const Sig& s : rf.in) {
    if (s.sv == D3D_NAME_VERTEX_ID || s.sv == D3D_NAME_INSTANCE_ID) continue;
    const DXGI_FORMAT f = s.type == D3D_REGISTER_COMPONENT_UINT32 ? DXGI_FORMAT_R32G32B32A32_UINT
                        : s.type == D3D_REGISTER_COMPONENT_SINT32 ? DXGI_FORMAT_R32G32B32A32_SINT
                        : DXGI_FORMAT_R32G32B32A32_FLOAT;
    il.push_back({s.sem.c_str(), s.index, f, static_cast<UINT>(il.size()), 0, D3D11_INPUT_PER_VERTEX_DATA, 0});
  }
  Com<ID3D11InputLayout> layout;
  if (!il.empty() && FAILED(dv.d->CreateInputLayout(il.data(), static_cast<UINT>(il.size()), stock.data(), stock.size(), &layout)))
    return false;
  const UINT nv = 192, ni = 24;
  D3D11_BUFFER_DESC sbd{stride * nv * ni, D3D11_USAGE_DEFAULT, D3D11_BIND_STREAM_OUTPUT};
  D3D11_BUFFER_DESC rbd{stride * nv * ni, D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ};
  Com<ID3D11Buffer> sob, rb;
  if (FAILED(dv.d->CreateBuffer(&sbd, nullptr, &sob)) || FAILED(dv.d->CreateBuffer(&rbd, nullptr, &rb))) return false;
  for (int cs = 0; cs < cases; ++cs) {
    Rng g(seed * 7919u + cs);
    const int mode = cs % 5 == 3 ? 1 : cs % 5 == 4 ? 2 : 0;  // 1: no sway anywhere, 2: sway everywhere
    Bound b;
    if (!MakeBound(dv, rf, g, mode, cs, &b)) return false;
    std::vector<Com<ID3D11Buffer>> vbs(il.size());
    std::vector<ID3D11Buffer*> vbp;
    std::vector<UINT> strides, offs;
    for (size_t e = 0; e < il.size(); ++e) {
      std::vector<uint32_t> w(4 * nv);
      for (auto& x : w) x = DefaultWord(g);
      for (const Rule& r : g_rules) {
        if (r.kind != 'a' || r.name != std::string(il[e].SemanticName) + std::to_string(il[e].SemanticIndex)) continue;
        for (size_t i = 0; i < w.size(); ++i)
          if (r.word < 0 || r.word == static_cast<int>(i % 4)) GenWord(r, g, mode, &w[i]);
      }
      D3D11_BUFFER_DESC bd{static_cast<UINT>(w.size() * 4), D3D11_USAGE_DEFAULT, D3D11_BIND_VERTEX_BUFFER};
      D3D11_SUBRESOURCE_DATA sd{w.data()};
      if (FAILED(dv.d->CreateBuffer(&bd, &sd, &vbs[e]))) return false;
      vbp.push_back(vbs[e]);
      strides.push_back(16);
      offs.push_back(0);
    }
    std::vector<uint8_t> res[2];
    for (int k = 0; k < 2; ++k) {
      dv.c->ClearState();
      dv.c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_POINTLIST);
      dv.c->IASetInputLayout(layout);
      if (!vbp.empty()) dv.c->IASetVertexBuffers(0, static_cast<UINT>(vbp.size()), vbp.data(), strides.data(), offs.data());
      dv.c->VSSetShader(vs[k], nullptr, 0);
      dv.c->GSSetShader(gs[k], nullptr, 0);
      dv.c->VSSetConstantBuffers(0, 14, b.cb);
      dv.c->VSSetShaderResources(0, 128, b.srv);
      dv.c->VSSetSamplers(0, 16, b.smp);
      ID3D11Buffer* t = sob;
      UINT o = 0;
      dv.c->SOSetTargets(1, &t, &o);
      dv.c->DrawInstanced(nv, ni, 0, 0);
      dv.c->SOSetTargets(0, nullptr, nullptr);
      dv.c->CopyResource(rb, sob);
      D3D11_MAPPED_SUBRESOURCE m{};
      if (FAILED(dv.c->Map(rb, 0, D3D11_MAP_READ, 0, &m))) return false;
      res[k].assign(static_cast<const uint8_t*>(m.pData), static_cast<const uint8_t*>(m.pData) + stride * nv * ni);
      dv.c->Unmap(rb, 0);
    }
    ++st->cases;
    std::vector<char> isFloat;
    UINT posOff = 0, posWords = stride / 4;
    for (const Sig& s : rf.out) {
      if (g_posOnly && s.sv == D3D_NAME_POSITION) posOff = static_cast<UINT>(isFloat.size()), posWords = Pop(s.mask);
      for (int k = 0; k < Pop(s.mask); ++k) isFloat.push_back(s.type == D3D_REGISTER_COMPONENT_FLOAT32);
    }
    if (!g_posOnly) posOff = 0;
    for (UINT v = 0; v < nv * ni; ++v) {
      ++st->items;
      st->words += stride / 4;
      size_t fw = 0;
      const std::vector<char> posFloat(posWords, 1);
      const int cmp = CompareWords(&res[0][v * stride + 4 * posOff], &res[1][v * stride + 4 * posOff], posWords,
                                   g_posOnly ? &posFloat : &isFloat, &fw);
      fw += posOff;
      if (cmp == 1) ++st->nanItems;
      if (cmp == 2) {
        ++st->diffItems;
        if (st->first.empty()) {
          UINT off = static_cast<UINT>(fw * 4);
          UINT acc = 0;
          std::string where = "?";
          for (const Sig& s : rf.out) {
            if (off < acc + 4u * Pop(s.mask)) {
              where = s.sem + std::to_string(s.index) + " comp " + std::to_string((off - acc) / 4);
              break;
            }
            acc += 4 * Pop(s.mask);
          }
          uint32_t x, y;
          memcpy(&x, &res[0][v * stride + off / 4 * 4], 4);
          memcpy(&y, &res[1][v * stride + off / 4 * 4], 4);
          char t[200];
          sprintf_s(t, "case %d vertex %u instance %u %s: stock %08x rewritten %08x", cs, v % nv, v / nv, where.c_str(), x, y);
          st->first = t;
        }
      }
    }
  }
  return true;
}

// ---------------------------------------------------------------------------
// Pixel shaders
// ---------------------------------------------------------------------------
struct PsIn {
  Sig s;
  int interp = 2;  // 1 constant, 2 linear, ...
};
static bool RunPs(Dev& dv, const std::vector<uint8_t>& stock, const std::vector<uint8_t>& sx, int cases, uint32_t seed,
                  Stats* st) {
  Refl rf;
  if (!Reflect(stock, &rf)) return false;
  // interpolation modes from the declarations
  // (from the disassembly: "dcl_input_ps constant v2.x" = 1, any linear mode = 2)
  std::map<UINT, int> interp;
  {
    std::istringstream is(Disasm(stock));
    std::string l;
    while (std::getline(is, l)) {
      if (l.compare(0, 13, "dcl_input_ps ") != 0) continue;
      const size_t v = l.find(" v");
      if (v == std::string::npos) continue;
      interp[static_cast<UINT>(atoi(l.c_str() + v + 2))] = l.find("constant") != std::string::npos ? 1 : 2;
    }
  }
  // feeder vertex shader: one float4/uint4/int4 attribute per pixel-shader input, passed through
  std::string src = "struct VI { float4 pos : POSITION;\n";
  std::string vo = "struct VO {\n", body;
  std::vector<PsIn> ins;
  bool hasPos = false;
  for (const Sig& s : rf.in) {
    PsIn pi{s, interp.count(s.reg) ? interp[s.reg] : 2};
    const char* ty = s.type == D3D_REGISTER_COMPONENT_UINT32 ? "uint" : s.type == D3D_REGISTER_COMPONENT_SINT32 ? "int" : "float";
    const int n = Pop(s.mask);
    char t[256];
    if (s.sv == D3D_NAME_POSITION) {
      vo += "  float4 svpos : SV_Position;\n";
      body += "  o.svpos = i.pos;\n";
      hasPos = true;
    } else if (s.sv != D3D_NAME_UNDEFINED) {
      st->first = "pixel input system value not supported: " + s.sem;
      return false;
    } else {
      const size_t k = ins.size();
      sprintf_s(t, "  %s4 a%zu : ATTR%zu;\n", ty, k, k);
      src += t;
      sprintf_s(t, "  %s %s%d v%zu : %s%u;\n", pi.interp == 1 ? "nointerpolation" : "", ty, n, k, s.sem.c_str(), s.index);
      vo += t;
      static const char* sw[5] = {"", "x", "xy", "xyz", "xyzw"};
      sprintf_s(t, "  o.v%zu = i.a%zu.%s;\n", k, k, sw[n]);
      body += t;
      ins.push_back(pi);
    }
  }
  if (!hasPos) vo += "  float4 svpos : SV_Position;\n", body += "  o.svpos = i.pos;\n";
  src += "};\n" + vo + "};\nVO main(VI i) { VO o;\n" + body + "  return o; }\n";
  Com<ID3DBlob> vsb, err;
  if (FAILED(D3DCompile(src.data(), src.size(), "feeder", nullptr, nullptr, "main", "vs_5_0", 0, 0, &vsb, &err))) {
    st->first = std::string("feeder vertex shader: ") + (err.p ? static_cast<const char*>(err->GetBufferPointer()) : "?");
    return false;
  }
  {  // the feeder's outputs must sit exactly where the pixel shader reads its inputs
    std::vector<uint8_t> vb(static_cast<uint8_t*>(vsb->GetBufferPointer()), static_cast<uint8_t*>(vsb->GetBufferPointer()) + vsb->GetBufferSize());
    Refl vr;
    Reflect(vb, &vr);
    for (const Sig& s : rf.in) {
      bool ok = false;
      for (const Sig& o : vr.out)
        ok = ok || (o.sem == s.sem && o.index == s.index && o.reg == s.reg && (o.mask & s.mask) == s.mask);
      if (!ok && _stricmp(s.sem.c_str(), "SV_Position") != 0) {
        st->first = "feeder signature does not match the pixel shader's at " + s.sem + std::to_string(s.index);
        return false;
      }
    }
  }
  Com<ID3D11VertexShader> vs;
  if (FAILED(dv.d->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &vs))) return false;
  std::vector<D3D11_INPUT_ELEMENT_DESC> il = {{"POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0}};
  for (size_t k = 0; k < ins.size(); ++k) {
    const DXGI_FORMAT f = ins[k].s.type == D3D_REGISTER_COMPONENT_UINT32 ? DXGI_FORMAT_R32G32B32A32_UINT
                        : ins[k].s.type == D3D_REGISTER_COMPONENT_SINT32 ? DXGI_FORMAT_R32G32B32A32_SINT
                        : DXGI_FORMAT_R32G32B32A32_FLOAT;
    il.push_back({"ATTR", static_cast<UINT>(k), f, static_cast<UINT>(k + 1), 0, D3D11_INPUT_PER_VERTEX_DATA, 0});
  }
  Com<ID3D11InputLayout> layout;
  if (FAILED(dv.d->CreateInputLayout(il.data(), static_cast<UINT>(il.size()), vsb->GetBufferPointer(), vsb->GetBufferSize(), &layout)))
    return false;
  Com<ID3D11PixelShader> ps[2];
  const std::vector<uint8_t>* code[2] = {&stock, &sx};
  for (int k = 0; k < 2; ++k)
    if (FAILED(dv.d->CreatePixelShader(code[k]->data(), code[k]->size(), nullptr, &ps[k]))) {
      st->first = k ? "the rewritten shader was refused by CreatePixelShader" : "stock shader refused";
      return false;
    }
  // targets
  const UINT W = 192, H = 160;
  struct Rt { Com<ID3D11Texture2D> t, r; Com<ID3D11RenderTargetView> v; };
  std::vector<Rt> rts;
  bool depthOut = false;
  for (const Sig& s : rf.out) {
    if (_stricmp(s.sem.c_str(), "SV_Target") != 0) { depthOut = true; continue; }
    if (rts.size() <= s.index) rts.resize(s.index + 1);
    const DXGI_FORMAT f = s.type == D3D_REGISTER_COMPONENT_UINT32 ? DXGI_FORMAT_R32G32B32A32_UINT
                        : s.type == D3D_REGISTER_COMPONENT_SINT32 ? DXGI_FORMAT_R32G32B32A32_SINT
                        : DXGI_FORMAT_R32G32B32A32_FLOAT;
    D3D11_TEXTURE2D_DESC td{W, H, 1, 1, f, {1, 0}, D3D11_USAGE_DEFAULT, D3D11_BIND_RENDER_TARGET};
    Rt& r = rts[s.index];
    if (FAILED(dv.d->CreateTexture2D(&td, nullptr, &r.t)) || FAILED(dv.d->CreateRenderTargetView(r.t, nullptr, &r.v))) return false;
    td.Usage = D3D11_USAGE_STAGING, td.BindFlags = 0, td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (FAILED(dv.d->CreateTexture2D(&td, nullptr, &r.r))) return false;
  }
  (void)depthOut;
  Com<ID3D11Texture2D> dt, dr;
  Com<ID3D11DepthStencilView> dsv;
  {
    D3D11_TEXTURE2D_DESC td{W, H, 1, 1, DXGI_FORMAT_D32_FLOAT, {1, 0}, D3D11_USAGE_DEFAULT, D3D11_BIND_DEPTH_STENCIL};
    if (FAILED(dv.d->CreateTexture2D(&td, nullptr, &dt)) || FAILED(dv.d->CreateDepthStencilView(dt, nullptr, &dsv))) return false;
    td.Usage = D3D11_USAGE_STAGING, td.BindFlags = 0, td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (FAILED(dv.d->CreateTexture2D(&td, nullptr, &dr))) return false;
  }
  D3D11_RASTERIZER_DESC rd{D3D11_FILL_SOLID, D3D11_CULL_NONE};
  rd.DepthClipEnable = TRUE;
  Com<ID3D11RasterizerState> rs;
  dv.d->CreateRasterizerState(&rd, &rs);
  const UINT ntri = 160, nv = ntri * 3;
  for (int cs = 0; cs < cases; ++cs) {
    Rng g(seed * 104729u + cs);
    const int mode = cs % 5 == 3 ? 1 : cs % 5 == 4 ? 2 : 0;
    Bound b;
    if (!MakeBound(dv, rf, g, mode, cs, &b)) return false;
    std::vector<std::vector<uint32_t>> vd(il.size(), std::vector<uint32_t>(4 * nv));
    for (UINT t = 0; t < ntri; ++t) {  // triangles of mixed size; later ones cover earlier ones
      const float cx = RandF(g, -1, 1), cy = RandF(g, -1, 1), sz = (t % 4 == 0) ? 1.5f : (t % 4 == 1) ? 0.5f : 0.12f;
      for (int k = 0; k < 3; ++k) {
        const float wv = (cs % 2) ? RandF(g, 0.4, 2.5) : 1.0f;
        float p[4] = {(cx + RandF(g, -sz, sz)) * wv, (cy + RandF(g, -sz, sz)) * wv, RandF(g, 0.05, 0.95) * wv, wv};
        memcpy(&vd[0][4 * (3 * t + k)], p, 16);
      }
    }
    for (size_t e = 1; e < il.size(); ++e) {
      const PsIn& pi = ins[e - 1];
      const float uvScale = static_cast<float>(1 << (g() % 6));
      for (UINT i = 0; i < 4 * nv; ++i)
        vd[e][i] = pi.s.type == D3D_REGISTER_COMPONENT_FLOAT32 ? FBits(RandF(g, -1, 1) * uvScale) : g();
      for (const Rule& r : g_rules) {
        if (r.kind != 'a' || r.name != pi.s.sem + std::to_string(pi.s.index)) continue;
        for (UINT t = 0; t < ntri; ++t) {
          for (int c = 0; c < 4; ++c) {
            if (r.word >= 0 && r.word != c) continue;
            uint32_t tv = 0;
            GenWord(r, g, mode, &tv);
            for (int k = 0; k < 3; ++k) {
              uint32_t x = tv;
              if (!r.perTri || cs % 3 == 2) GenWord(r, g, mode, &x);  // every third case: per vertex
              vd[e][4 * (3 * t + k) + c] = x;
            }
          }
        }
      }
    }
    std::vector<Com<ID3D11Buffer>> vbs(il.size());
    std::vector<ID3D11Buffer*> vbp;
    std::vector<UINT> strides(il.size(), 16), offs(il.size(), 0);
    for (size_t e = 0; e < il.size(); ++e) {
      D3D11_BUFFER_DESC bd{static_cast<UINT>(vd[e].size() * 4), D3D11_USAGE_DEFAULT, D3D11_BIND_VERTEX_BUFFER};
      D3D11_SUBRESOURCE_DATA sd{vd[e].data()};
      if (FAILED(dv.d->CreateBuffer(&bd, &sd, &vbs[e]))) return false;
      vbp.push_back(vbs[e]);
    }
    std::vector<std::vector<uint8_t>> res[2];
    for (int k = 0; k < 2; ++k) {
      dv.c->ClearState();
      std::vector<ID3D11RenderTargetView*> rv;
      const float clr[4] = {-7.0f, -7.0f, -7.0f, -7.0f};
      for (Rt& r : rts) {
        rv.push_back(r.v);
        if (r.v) dv.c->ClearRenderTargetView(r.v, clr);
      }
      dv.c->ClearDepthStencilView(dsv, D3D11_CLEAR_DEPTH, 1.0f, 0);
      dv.c->OMSetRenderTargets(static_cast<UINT>(rv.size()), rv.empty() ? nullptr : rv.data(), dsv);
      D3D11_VIEWPORT vp{0, 0, static_cast<float>(W), static_cast<float>(H), 0, 1};
      dv.c->RSSetViewports(1, &vp);
      dv.c->RSSetState(rs);
      dv.c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
      dv.c->IASetInputLayout(layout);
      dv.c->IASetVertexBuffers(0, static_cast<UINT>(vbp.size()), vbp.data(), strides.data(), offs.data());
      dv.c->VSSetShader(vs, nullptr, 0);
      dv.c->PSSetShader(ps[k], nullptr, 0);
      dv.c->PSSetConstantBuffers(0, 14, b.cb);
      dv.c->PSSetShaderResources(0, 128, b.srv);
      dv.c->PSSetSamplers(0, 16, b.smp);
      dv.c->Draw(nv, 0);
      auto grab = [&](ID3D11Texture2D* from, ID3D11Texture2D* stg, UINT bpp) {
        dv.c->CopyResource(stg, from);
        D3D11_MAPPED_SUBRESOURCE m{};
        std::vector<uint8_t> o(static_cast<size_t>(W) * H * bpp);
        if (SUCCEEDED(dv.c->Map(stg, 0, D3D11_MAP_READ, 0, &m))) {
          for (UINT y = 0; y < H; ++y) memcpy(&o[static_cast<size_t>(y) * W * bpp], static_cast<const uint8_t*>(m.pData) + static_cast<size_t>(y) * m.RowPitch, static_cast<size_t>(W) * bpp);
          dv.c->Unmap(stg, 0);
        }
        return o;
      };
      res[k].push_back(grab(dt, dr, 4));
      for (Rt& r : rts)
        if (r.t) res[k].push_back(grab(r.t, r.r, 16));
    }
    ++st->cases;
    uint64_t covered = 0;
    for (UINT i = 0; i < W * H; ++i) {
      float z;
      memcpy(&z, &res[0][0][i * 4], 4);
      if (z != 1.0f) ++covered;
    }
    st->swayOn += covered;  // pixels the stock shader kept
    if (res[0].size() > 1) {  // render target 0 .x of the stock shader on covered pixels: exactly 1.0 or not
      for (UINT i = 0; i < W * H; ++i) {
        float z, x;
        memcpy(&z, &res[0][0][i * 4], 4);
        memcpy(&x, &res[0][1][i * 16], 4);
        if (z == 1.0f) continue;
        if (x == 1.0f) ++st->rt0One;
        else ++st->rt0Other;
      }
    }
    for (size_t t = 0; t < res[0].size(); ++t) {
      const UINT bpp = t == 0 ? 4 : 16;
      for (UINT i = 0; i < W * H; ++i) {
        if (t == 0) ++st->items;
        st->words += bpp / 4;
        size_t fw = 0;
        static const std::vector<char> none(4, 0);
        const int cmp = CompareWords(&res[0][t][i * bpp], &res[1][t][i * bpp], bpp / 4, t == 0 ? &none : nullptr, &fw);
        if (cmp == 1) ++st->nanItems;
        if (cmp == 2) {
          ++st->diffItems;
          if (st->first.empty()) {
            char s[160];
            sprintf_s(s, "case %d pixel (%u,%u) %s", cs, i % W, i / W, t == 0 ? "depth/coverage" : "render target");
            st->first = s;
          }
        }
      }
    }
  }
  return true;
}

#if SX_PATCH
// Negative controls: a rewrite broken on purpose must be reported as different, or the cases do not reach the
// rewritten path. 1: the inserted branches test the opposite (sway chains, shadow sample); 2: the unswayed
// position is taken from the wrong components; 3: the search loop stops early (hi - lo <= 16).
static bool Mutate(const std::vector<uint8_t>& stock, std::vector<uint8_t>& sx, int kind) {
  dxbcp::Program p;
  dxbcp::Container c;
  if (!dxbcp::Load(sx.data(), sx.size(), &p, &c)) return false;
  const int td = dxbcp::FindTemps(p);
  dxbcp::Program p0;
  dxbcp::Container c0;
  dxbcp::Load(stock.data(), stock.size(), &p0, &c0);
  const int td0 = dxbcp::FindTemps(p0);
  const uint32_t added = td >= 0 && td0 >= 0 ? p0.ins[td0].tok[1] : 0xffffffffu;  // first temp the rewrite added
  const bool ps = (p.version >> 16) == 0;
  int n = 0;
  for (size_t i = 0; i < p.ins.size(); ++i) {
    dxbcp::Instr& in = p.ins[i];
    if (kind == 1 && in.op == dxbcp::kIf && in.ops.size() == 1 && in.ops[0].type == dxbcp::kTemp &&
        (ps ? i + 6 == p.ins.size() : in.ops[0].idx[0] >= added))
      in.tok[0] ^= 1u << 18, ++n;
    if (kind == 2 && in.op == dxbcp::kMov && i > 0 && p.ins[i - 1].op == dxbcp::kElse && in.tok.size() == 5)
      in.tok[3] = (in.tok[3] & ~0xff0u) | ((((in.tok[3] >> 4) & 0xff) ^ 0x55) << 4), ++n;
    if (kind == 3 && in.op == dxbcp::kUge && in.ops.size() == 3 && in.ops[0].idx[0] >= added && in.tok.size() == 7)
      in.tok[4] = 16, ++n;
  }
  if (!n) return false;
  dxbcp::Build(sx.data(), c, p, &sx);
  return true;
}
#endif  // SX_PATCH

static uint32_t ShaderVersion(const std::vector<uint8_t>& b) {
  if (b.size() < 32) return 0;
  uint32_t nch;
  memcpy(&nch, b.data() + 28, 4);
  for (uint32_t i = 0; i < nch && 32 + 4 * i + 4 <= b.size(); ++i) {
    uint32_t off;
    memcpy(&off, b.data() + 32 + 4 * i, 4);
    if (off + 12 > b.size()) break;
    if (!memcmp(b.data() + off, "SHDR", 4) || !memcmp(b.data() + off, "SHEX", 4)) {
      uint32_t v;
      memcpy(&v, b.data() + off + 8, 4);
      return v;
    }
  }
  return 0;
}

// sx_equiv pair <stock.dxbc> <variant.dxbc> <rules> [options]: see the header.
static int Pair(int argc, char** argv) {
  if (argc < 5 || !LoadRules(argv[4])) {
    fprintf(stderr, "pair: <stock.dxbc> <variant.dxbc> <rules> needed (rules readable)\n");
    return 2;
  }
  const std::vector<uint8_t> stock = ReadFile(argv[2]), var = ReadFile(argv[3]);
  if (stock.size() < 32 || var.size() < 32) {
    fprintf(stderr, "pair: blob not readable\n");
    return 2;
  }
  std::string devs = "both", csvName, expect = "same", id = BaseId(argv[3]);
  int cases = 40;
  uint32_t seed = 1;
  for (int i = 5; i + 1 < argc; i += 2) {
    const std::string k = argv[i];
    if (k == "-dev") devs = argv[i + 1];
    else if (k == "-cases") cases = atoi(argv[i + 1]);
    else if (k == "-seed") seed = strtoul(argv[i + 1], nullptr, 0);
    else if (k == "-csv") csvName = argv[i + 1];
    else if (k == "-expect") expect = argv[i + 1];
    else if (k == "-id") id = argv[i + 1];
    else if (k == "-texsize") sscanf_s(argv[i + 1], "%ux%u", &g_texW, &g_texH);
  }
  const bool control = expect == "differ";
  const bool isVs = (ShaderVersion(stock) >> 16) == 1;
  FILE* csv = nullptr;
  if (!csvName.empty()) {
    const bool existed = GetFileAttributesA(csvName.c_str()) != INVALID_FILE_ATTRIBUTES;
    fopen_s(&csv, csvName.c_str(), "a");
    if (csv && !existed)
      fprintf(csv, "id,stage,device,expect,seed,cases,items,words,rt0_one,rt0_other,diff_items,nan_for_nan_items,result,first_difference\n");
  }
  int fails = 0;
  for (int w = 0; w < 2; ++w) {
    if ((w == 0 && devs == "hw") || (w == 1 && devs == "warp")) continue;
    Dev dv;
    if (!MakeDevice(w == 0, &dv)) {
      fprintf(stderr, "device %s not available\n", w == 0 ? "warp" : "hw");
      ++fails;
      continue;
    }
    Stats st;
    const bool ok = isVs ? RunVs(dv, stock, var, cases, seed, &st) : RunPs(dv, stock, var, cases, seed, &st);
    const bool pass = ok && st.cases > 0 && (control ? st.diffItems != 0 : st.diffItems == 0);
    if (!pass) ++fails;
    const char* res = !ok ? "NOT RUN" : control ? (pass ? "CONTROL DETECTED" : "CONTROL MISSED") : pass ? "IDENTICAL" : "DIFFERENT";
    printf("%s %s %s seed %u: %s  cases %llu, %s %llu, words %llu, stock RT0.x == 1: %llu, other: %llu, differing %llu, "
           "NaN-for-NaN only %llu%s%s\n",
           id.c_str(), isVs ? "VS" : "PS", dv.name.c_str(), seed, res, static_cast<unsigned long long>(st.cases),
           isVs ? "vertices" : "pixels", static_cast<unsigned long long>(st.items), static_cast<unsigned long long>(st.words),
           static_cast<unsigned long long>(st.rt0One), static_cast<unsigned long long>(st.rt0Other),
           static_cast<unsigned long long>(st.diffItems), static_cast<unsigned long long>(st.nanItems),
           st.first.empty() ? "" : "  first: ", st.first.c_str());
    if (csv)
      fprintf(csv, "%s,%s,%s,%s,%u,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%s,%s\n", id.c_str(), isVs ? "VS" : "PS",
              dv.name.c_str(), expect.c_str(), seed, static_cast<unsigned long long>(st.cases),
              static_cast<unsigned long long>(st.items), static_cast<unsigned long long>(st.words),
              static_cast<unsigned long long>(st.rt0One), static_cast<unsigned long long>(st.rt0Other),
              static_cast<unsigned long long>(st.diffItems), static_cast<unsigned long long>(st.nanItems),
              !ok ? "not-run" : control ? (pass ? "control-detected" : "control-missed") : pass ? "identical" : "different",
              st.first.c_str());
    fflush(stdout);
  }
  if (csv) fclose(csv);
  return fails ? 1 : 0;
}

// ---------------------------------------------------------------------------
int main(int argc, char** argv) {
  if (argc < 4) {
    fprintf(stderr, "usage: sx_equiv pair <stock.dxbc> <variant.dxbc> <rules> [-dev warp|hw|both] [-cases N] [-seed S] "
                    "[-texsize WxH] [-csv F] [-expect same|differ] [-id NAME]\n"
#if SX_PATCH
                    "       sx_equiv scan <dir|file|list.txt> <outdir> [want]\n"
                    "       sx_equiv run <dir|file|list.txt> <rules> [-dev warp|hw|both] [-cases N] [-seed S] [-want M] [-csv F]\n"
#endif
    );
    return 2;
  }
  const std::string cmd = argv[1];
  if (cmd == "pair") return Pair(argc, argv);
#if !SX_PATCH
  fprintf(stderr, "%s: needs src/dxbc_patch.h (exact-shader-batch tree); only `pair` is built here\n", cmd.c_str());
  return 2;
#else
  const std::vector<std::string> files = ListBlobs(argv[2]);
  if (cmd == "scan") {
    const std::string out = argv[3];
    const uint32_t want = argc > 4 ? strtoul(argv[4], nullptr, 0) : 7;
    CreateDirectoryA(out.c_str(), nullptr);
    int n = 0, badSum = 0, applied = 0;
    FILE* csv = nullptr;
    fopen_s(&csv, (out + "\\scan.csv").c_str(), "w");
    if (csv) fprintf(csv, "id,bytes,checksum_ok,applied,sway_chains,probes_folded,bytes_after,note\n");
    for (const std::string& f : files) {
      const std::vector<uint8_t> b = ReadFile(f);
      if (b.size() < 32) continue;
      ++n;
      uint8_t sum[16];
      dxbcp::Checksum(b.data(), b.size(), sum);
      const bool sumOk = memcmp(sum, b.data() + 4, 16) == 0;
      if (!sumOk) ++badSum;
      std::vector<uint8_t> o;
      dxbcp::Result r;
      const bool ok = dxbcp::Rewrite(b.data(), b.size(), want, &o, &r);
      if (ok) {
        ++applied;
        const std::string id = BaseId(f);
        WriteFile(out + "\\" + id + ".sx.dxbc", o.data(), o.size());
        const std::string a0 = Disasm(b), a1 = Disasm(o);
        WriteFile(out + "\\" + id + ".stock.asm", a0.data(), a0.size());
        WriteFile(out + "\\" + id + ".sx.asm", a1.data(), a1.size());
        if (a1.empty()) printf("%s: the rewritten blob does not disassemble\n", id.c_str());
      }
      if (csv && (ok || !r.note.empty()))
        fprintf(csv, "%s,%zu,%d,%u,%d,%d,%zu,%s\n", BaseId(f).c_str(), b.size(), sumOk ? 1 : 0, r.applied, r.swayChains,
                r.probesFolded, ok ? o.size() : 0, r.note.c_str());
    }
    if (csv) fclose(csv);
    printf("scan: %d blobs, %d with a checksum my code does not reproduce, %d rewritten\n", n, badSum, applied);
    return 0;
  }
  if (cmd == "abl") {  // sx_equiv abl <files> <rules> <mode 1 cull | 2 ps | 3 vspos> : the ablations create; vspos keeps positions
    if (argc < 5 || !LoadRules(argv[3])) return 2;
    const int mode = atoi(argv[4]);
    int bad = 0, n = 0, skipped = 0;
    for (int w = 0; w < 2; ++w) {
      Dev dv;
      if (!MakeDevice(w == 0, &dv)) return 1;
      for (const std::string& f : files) {
        const std::vector<uint8_t> b = ReadFile(f);
        std::vector<uint8_t> o;
        int kept = -1;
        if (b.size() < 32 || !dxbcp::Ablate(b.data(), b.size(), mode, &o, &kept)) { ++skipped; continue; }
        ++n;
        Stats st;
        bool ok;
        if (mode == 3) {
          g_posOnly = true;
          ok = RunVs(dv, b, o, 12, 1, &st) && st.diffItems == 0 && st.cases > 0;
        } else if (mode == 1) {
          Com<ID3D11VertexShader> s;
          ok = SUCCEEDED(dv.d->CreateVertexShader(o.data(), o.size(), nullptr, &s)) && !Disasm(o).empty();
        } else {
          Com<ID3D11PixelShader> s;
          ok = SUCCEEDED(dv.d->CreatePixelShader(o.data(), o.size(), nullptr, &s)) && !Disasm(o).empty();
        }
        if (!ok) ++bad;
        printf("%s %s mode %d: %s%s kept %d %% of the code; %llu vertices, %llu positions different%s%s\n", BaseId(f).c_str(),
               dv.name.c_str(), mode, ok ? "OK" : "FAILED", mode == 3 ? " (positions identical)" : " (creates)", kept,
               static_cast<unsigned long long>(st.items), static_cast<unsigned long long>(st.diffItems),
               st.first.empty() ? "" : " first: ", st.first.c_str());
      }
    }
    printf("abl: %d shader/device checks, %d failed, %d not applicable\n", n, bad, skipped);
    return bad ? 1 : 0;
  }
  if (cmd != "run") return 2;
  if (!LoadRules(argv[3])) {
    fprintf(stderr, "rules file not readable: %s\n", argv[3]);
    return 2;
  }
  std::string devs = "both", csvName;
  int cases = 40, mutate = 0;
  uint32_t seed = 1, want = 7;
  for (int i = 4; i + 1 < argc; i += 2) {
    const std::string k = argv[i];
    if (k == "-dev") devs = argv[i + 1];
    else if (k == "-cases") cases = atoi(argv[i + 1]);
    else if (k == "-seed") seed = strtoul(argv[i + 1], nullptr, 0);
    else if (k == "-want") want = strtoul(argv[i + 1], nullptr, 0);
    else if (k == "-csv") csvName = argv[i + 1];
    else if (k == "-mutate") mutate = atoi(argv[i + 1]);
  }
  FILE* csv = nullptr;
  if (!csvName.empty()) {
    const bool existed = GetFileAttributesA(csvName.c_str()) != INVALID_FILE_ATTRIBUTES;
    fopen_s(&csv, csvName.c_str(), "a");
    if (csv && !existed) fprintf(csv, "id,stage,applied,device,mutation,cases,items,words,kept_pixels,diff_items,nan_for_nan_items,result,first_difference\n");
  }
  int fails = 0, ran = 0;
  for (int w = 0; w < 2; ++w) {
    if ((w == 0 && devs == "hw") || (w == 1 && devs == "warp")) continue;
    Dev dv;
    if (!MakeDevice(w == 0, &dv)) {
      fprintf(stderr, "device %s not available\n", w == 0 ? "warp" : "hw");
      ++fails;
      continue;
    }
    for (const std::string& f : files) {
      const std::vector<uint8_t> b = ReadFile(f);
      std::vector<uint8_t> o;
      dxbcp::Result r;
      if (b.size() < 32 || !dxbcp::Rewrite(b.data(), b.size(), want, &o, &r)) continue;
      if (mutate && !Mutate(b, o, mutate)) continue;
      uint32_t ver = 0;
      {
        const uint32_t nch = dxbcp::Rd32(b.data() + 28);
        for (uint32_t i = 0; i < nch; ++i) {
          const uint32_t off = dxbcp::Rd32(b.data() + 32 + 4 * i);
          if (!memcmp(b.data() + off, "SHDR", 4) || !memcmp(b.data() + off, "SHEX", 4)) ver = dxbcp::Rd32(b.data() + off + 8);
        }
      }
      const bool isVs = (ver >> 16) == 1;
      Stats st;
      const bool ok = isVs ? RunVs(dv, b, o, cases, seed, &st) : RunPs(dv, b, o, cases, seed, &st);
      const bool pass = ok && st.cases > 0 && (mutate ? st.diffItems != 0 : st.diffItems == 0);
      if (!pass) ++fails;
      ++ran;
      printf("%s %s fix %u %s: %s  cases %llu, %s %llu, words %llu, differing %llu, NaN-for-NaN only %llu%s%s\n", BaseId(f).c_str(),
             isVs ? "VS" : "PS", r.applied, dv.name.c_str(),
             !ok ? "NOT RUN" : mutate ? (pass ? "CONTROL DETECTED" : "CONTROL MISSED") : pass ? "IDENTICAL" : "DIFFERENT",
             static_cast<unsigned long long>(st.cases), isVs ? "vertices" : "pixels",
             static_cast<unsigned long long>(st.items), static_cast<unsigned long long>(st.words),
             static_cast<unsigned long long>(st.diffItems), static_cast<unsigned long long>(st.nanItems),
             st.first.empty() ? "" : "  first: ", st.first.c_str());
      if (csv)
        fprintf(csv, "%s,%s,%u,%s,%d,%llu,%llu,%llu,%llu,%llu,%llu,%s,%s\n", BaseId(f).c_str(), isVs ? "VS" : "PS", r.applied,
                dv.name.c_str(), mutate, static_cast<unsigned long long>(st.cases), static_cast<unsigned long long>(st.items),
                static_cast<unsigned long long>(st.words), static_cast<unsigned long long>(st.swayOn),
                static_cast<unsigned long long>(st.diffItems), static_cast<unsigned long long>(st.nanItems),
                !ok ? "not-run" : mutate ? (pass ? "control-detected" : "control-missed") : pass ? "identical" : "different",
                st.first.c_str());
      fflush(stdout);
    }
  }
  if (csv) fclose(csv);
  printf("run: %d shader/device runs, %d not identical or not run\n", ran, fails);
  return fails ? 1 : 0;
#endif  // SX_PATCH
}
