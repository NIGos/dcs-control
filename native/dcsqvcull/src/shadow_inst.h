// Exact shadow-caster instancing (R12), stage 1: the compile pipeline and the
// variant map. No draw changes yet. [Model] ShadowInstancing,
// [Suite] ShadowInstCompile.
//
// Tags: [V] verified in the binary (dx11backend.dll / NGModel.dll of DCS
// 2.9.30) or the shader sources, [I] inferred, [M] measured.
//
// What a shader is [V dx11backend]:
//   DX11Shader (vtable 0xb43b8) is built by 0x2f4b0 -> ctor 0x1c750 (bytes at
//   0x1c883..0x1c8d8 hash-checked):
//     +0x50 effect (CEffect, vtable 0xbe700, set by 0x1ee40),
//     +0x58 std::string = the normalized source path (also the effect's
//           srcName), e.g. "model/def_material.fx",
//     +0x78 std::string = the shader key, 0xa1a60 (hash-checked):
//           path [":" {name ["=" value] ";"}...],
//     +0x98 std::vector of 0xa0-byte defines: name = char[] at +8, value =
//           char[] at +0x58 (the D3D_SHADER_MACRO built by 0xa19d0,
//           hash-checked); "=value" is in the key when qword +0x50 != 0.
//   The shader manager (0x2be50) merges its own global defines into the same
//   vector before building the key and compiling (the fxo keys show
//   DIRECTX11=true, USE_DCS_DEFERRED=1), so +0x98 is exactly the macro list
//   DCS compiled with. The key reconstructed from +0x58/+0x98 is compared to
//   +0x78 for every shader: a mismatch rejects the key.
// How DCS compiles [V 0x2e720]: D3DCompile2(src, len, name, macros, include,
//   NULL, "fx_5_0", 0x8000 (O3), 0, 0, NULL, 0). Its include handler
//   (IncludesProvider, vtable 0xb5288, Open 0xe3e0) looks up
//   "/shaders/" + folder(main path) + name first, then "/shaders/" + name.
//   Folders ending in "inter/" get a special rule (0x2e8f1), not reproduced:
//   such keys are rejected. We serve the same lookup order from
//   Bazar\shaders (shader mods in the VFS are not seen).
// The device [V]: CEffect vt[4] (0x66a40, hash-checked) is FX11's
//   GetDevice: *out = [effect+0x150], AddRef -- the device the effect created
//   every shader with. Cross-checked once with ID3D11DeviceChild::GetDevice
//   of DCS's own VS of a shadow pass (FX shader variable vt[32]
//   GetVertexShader, 0x5e790: [block+0x18], AddRef). If the device was
//   created SINGLETHREADED our VS objects are created on the render thread
//   (in the caster observer), otherwise on the compile worker.
// Shaders seen: inst_count.h's ShadowMapRenderable slot-1 hook calls our
//   observer for every caster (render thread). ModelMaterialMT materials
//   (vtable 0x592f0) with a DX11Shader at mat+0x30 are snapshotted once per
//   shader (path, key, defines, technique handles mat+0x210/0x218 and names,
//   live pass counts and VS pointers) and queued; the compile workers do the
//   rest. DCS memory is only read on the render thread.
//
// Per key, variants: (a) the unmodified sources, (b) the same with
// QV_SHADOW_INSTANCED defined and two included files edited in memory
// (R12 sec. 1.3):
//   model/common/uniforms.hlsl: posStructOffset in def_uniforms becomes
//     qvPsoBase (same type and offset), plus `static uint posStructOffset;`.
//   model/functions/lk_shadow.hlsl: StructuredBuffer<uint> qvInstOffsets :
//     register(t127); lk_shadow_vs gets `uint qvIID : SV_InstanceID` and
//     starts with posStructOffset = qvInstOffsets[qvPsoBase + qvIID].
// Static checks per technique pass (both shadow techniques), with D3DReflect:
//   output signature byte-identical; input signature = (a)'s + one
//   SV_InstanceID; every constant buffer identical (name, size, variables,
//   offsets, types, flags) except posStructOffset -> qvPsoBase at the same
//   offset; bindings = (a)'s + qvInstOffsets at t127.
//
// G-buffer instancing, stage 1 (R13 sec. 3, 5): the same pipeline for the
// main-pass techniques. ModelMaterialMT::draw (vt[4] 0x16140) -> 0x15e20
// selects, for model passes 1 and 2, normal_cf[_db] (handle mat+0x1d8) or,
// in the cockpit, normal_cockpit_cf[_db] (mat+0x1e0); opaque pass 1 = P0
// (deferred_ps_c, the G-buffer), else forward P1..P4 / cockpit P1..P8. Pass 8
// is flat_shadow / flat_shadow_transparent (flat_shadow_vs, alpha blended;
// not compiled here). Every normal* pass sets model_vs_c = model_vs of
// model/functions/vertex_shader.hlsl (forest.hlsl's model_vs is included by
// no effect). Variant (b) for these keys: QV_MODEL_INSTANCED, uniforms.hlsl
// edited as above (shared guard) and model_vs given the same three lines as
// lk_shadow_vs. Per pass: checks 1-3 above, check 4 (a disassembly dataflow
// compare, CompareDisasm) and the reflection gate on (a) (GatePass: which
// def_uniforms members each stage reads; prevFrameTransform at CB
// +0x20..+0x5f must be unused for the batching key to leave it out, and
// posStructOffset must be read by the VS only). Map entries carry the gate:
// FindVsEx, PrevTransformUnused, CbUsedDwords. Keys come from
// SceneRenderable vt[1] (gb_count.h's hook, observer GbObserver), collected
// by [Suite] GBufferInstCompile only. Each G-buffer pass also keeps the
// resources its stages bind in (a) (PassGate::reads, GbPassReadTextures),
// for the R14 lead 3 counter [Suite] GBufferTexCount (measurement only).
// Compile results persist in <module dir>\cache\ (see "Disk cache" below):
// later starts load them without D3DCompile.
// Included once from main.cpp inside its anonymous namespace, after
// inst_count.h, gb_count.h and shadow_tex.h.
#pragma once

namespace shadowinst {

// ---------------------------------------------------------------------------
// FX11 fx_5_0 binary parser (pure; unit tested on DCS's own .fxo files)
// ---------------------------------------------------------------------------
// Layout as in FX11's EffectBinaryFormat.h / EffectLoad.cpp: a 0x60-byte
// header, the unstructured block (names, types, initializers, shader blobs;
// every o* offset is relative to it), then the structured stream: constant
// buffers + numeric variables, object variables, interface variables, groups
// -> techniques -> passes -> assignments. Object types 25-30 are the SM5
// shader variables (PS, VS, GS, CS, HS, DS) [M: the 265 .fxo files].

constexpr uint32_t kFx5Tag = 0xFEFF2001;
constexpr uint32_t kStateVertexShader = 6;  // pass assignment state index of SetVertexShader [M]

struct FxBlob {
  const uint8_t* p = nullptr;
  uint32_t n = 0;
};
struct FxAssign {
  uint32_t state = 0, index = 0, type = 0, init = 0;
  FxBlob shader;     // resolved shader, if any
  bool dynamic = false;  // selected by a runtime index or expression
  bool unresolved = false;  // names no variable of the effect, or an element past a shader array's end
};
struct FxPass {
  std::string name;
  std::vector<FxAssign> assigns;
};
struct FxTechnique {
  std::string group, name;
  std::vector<FxPass> passes;
};
struct FxObjectVar {
  std::string name, typeName;
  uint32_t objType = 0, elements = 0;
  std::vector<FxBlob> shaders;  // shader variables: one per element (p == nullptr: NULL)
};
struct FxEffect {
  uint32_t cbs = 0, numeric = 0, objects = 0, techniques = 0, totalShaders = 0, inlineShaders = 0, groups = 0;
  std::vector<FxObjectVar> vars;
  std::vector<std::string> otherNames;  // constant buffers, numeric and interface variables
  std::vector<FxTechnique> techs;
  size_t size = 0;  // bytes consumed (header + unstructured + structured)
  uint32_t shaderBlobs = 0, inlineBlobs = 0;
};

class FxReader {
 public:
  FxReader(const uint8_t* p, size_t n) : p_(p), n_(n) {}
  bool ok = true;
  const char* why = nullptr;
  size_t pos = 0;
  uint32_t U32() {
    if (!ok || n_ - pos < 4 || pos > n_) return Fail("structured data truncated"), 0u;
    uint32_t v;
    memcpy(&v, p_ + pos, 4);
    pos += 4;
    return v;
  }
  void Skip(size_t words) {
    for (size_t i = 0; i < words && ok; ++i) U32();
  }
  void Fail(const char* w) {
    if (ok) why = w;
    ok = false;
  }

 private:
  const uint8_t* p_;
  size_t n_;
};

class FxParser {
 public:
  FxParser(const uint8_t* p, size_t n) : p_(p), n_(n), r_(p, n) {}

  bool Parse(FxEffect& fx, std::string& err) {
    if (!p_ || n_ < 0x60) return Err(err, "too small for an fx_5_0 header");
    uint32_t h[24];
    memcpy(h, p_, sizeof(h));
    if (h[0] != kFx5Tag) return Err(err, "not an fx_5_0 effect (tag)");
    fx.cbs = h[1];
    fx.numeric = h[2];
    fx.objects = h[3];
    if (h[4] || h[5] || h[6]) return Err(err, "effect pools are not supported");
    fx.techniques = h[7];
    cbU_ = h[8];
    fx.totalShaders = h[17];
    fx.inlineShaders = h[18];
    fx.groups = h[19];
    const uint32_t interfaces = h[21];
    if (cbU_ > n_ - 0x60) return Err(err, "unstructured block larger than the effect");
    u_ = p_ + 0x60;
    r_ = FxReader(p_, n_);
    r_.pos = 0x60 + cbU_;
    // Constant buffers and their numeric variables.
    // SBinaryConstantBuffer: oName, Size, Flags, cVariables, ExplicitBindPoint;
    // SBinaryNumericVariable: oName, oType, oSemantic, Offset, oDefaultValue, Flags.
    for (uint32_t i = 0; i < fx.cbs && r_.ok; ++i) {
      const uint32_t oCb = r_.U32();
      r_.Skip(2);
      const uint32_t vars = r_.U32();
      r_.Skip(1);
      if (r_.ok) fx.otherNames.push_back(Str(oCb));
      Annotations();
      if (vars > 100000) r_.Fail("implausible variable count");
      for (uint32_t v = 0; v < vars && r_.ok; ++v) {
        const uint32_t oVar = r_.U32();
        r_.Skip(5);
        if (r_.ok) fx.otherNames.push_back(Str(oVar));
        Annotations();
      }
    }
    // Object variables.
    for (uint32_t i = 0; i < fx.objects && r_.ok; ++i) {
      FxObjectVar var;
      const uint32_t oName = r_.U32(), oType = r_.U32();
      r_.Skip(2);
      Type t;
      if (!r_.ok || !ReadType(oType, t)) break;
      if (t.varType != 2) {
        r_.Fail("object variable without an object type");
        break;
      }
      var.name = Str(oName);
      var.typeName = t.name;
      var.objType = t.objType;
      var.elements = t.elements;
      const uint32_t n = t.elements ? t.elements : 1;
      if (n > 65536) {
        r_.Fail("implausible element count");
        break;
      }
      switch (t.objType) {
        case 2: case 3: case 4: case 21:  // blend, depth-stencil, rasterizer, sampler blocks
          for (uint32_t e = 0; e < n && r_.ok; ++e) {
            const uint32_t c = r_.U32();
            if (c > 4096) r_.Fail("implausible assignment count");
            r_.Skip(4ull * c);
          }
          break;
        case 5: case 6: case 7:  // SM4 shaders: oShader
          for (uint32_t e = 0; e < n && r_.ok; ++e) var.shaders.push_back(Blob(r_.U32(), false));
          break;
        case 8:  // GS with stream output: oShader, oSODecl
          for (uint32_t e = 0; e < n && r_.ok; ++e) {
            var.shaders.push_back(Blob(r_.U32(), false));
            r_.Skip(1);
          }
          break;
        case 25: case 26: case 27: case 28: case 29: case 30:  // SM5 shaders: SBinaryShaderData5
          for (uint32_t e = 0; e < n && r_.ok; ++e) {
            var.shaders.push_back(Blob(r_.U32(), false));
            r_.Skip(8);
          }
          break;
        case 1:  // strings
          r_.Skip(n);
          break;
        default:  // textures, buffers, views: no per-element data
          break;
      }
      Annotations();
      fx.vars.push_back(std::move(var));
    }
    // Interface variables.
    for (uint32_t i = 0; i < interfaces && r_.ok; ++i) {
      const uint32_t oName = r_.U32();
      r_.Skip(3);
      if (r_.ok) fx.otherNames.push_back(Str(oName));
      Annotations();
    }
    // Groups, techniques, passes.
    for (uint32_t g = 0; g < fx.groups && r_.ok; ++g) {
      const uint32_t oGroup = r_.U32(), techs = r_.U32();
      Annotations();
      if (techs > 65536) r_.Fail("implausible technique count");
      for (uint32_t t = 0; t < techs && r_.ok; ++t) {
        FxTechnique tech;
        tech.group = oGroup ? Str(oGroup) : std::string();
        const uint32_t oTech = r_.U32(), passes = r_.U32();
        tech.name = Str(oTech);
        Annotations();
        if (passes > 4096) r_.Fail("implausible pass count");
        for (uint32_t p = 0; p < passes && r_.ok; ++p) {
          FxPass pass;
          const uint32_t oPass = r_.U32(), assigns = r_.U32();
          pass.name = oPass ? Str(oPass) : std::string();
          Annotations();
          if (assigns > 4096) r_.Fail("implausible assignment count");
          for (uint32_t a = 0; a < assigns && r_.ok; ++a) {
            FxAssign as;
            as.state = r_.U32();
            as.index = r_.U32();
            as.type = r_.U32();
            as.init = r_.U32();
            if (r_.ok) Resolve(fx, as);
            pass.assigns.push_back(as);
          }
          tech.passes.push_back(std::move(pass));
        }
        fx.techs.push_back(std::move(tech));
      }
    }
    if (!r_.ok) return Err(err, r_.why ? r_.why : "malformed effect");
    fx.size = r_.pos;
    fx.shaderBlobs = blobs_;
    fx.inlineBlobs = inline_;
    return true;
  }

 private:
  struct Type {
    std::string name;
    uint32_t varType = 0, elements = 0, objType = 0;
  };

  static bool Err(std::string& err, const char* w) {
    err = w;
    return false;
  }

  std::string Str(uint32_t off) {
    if (off >= cbU_) {
      r_.Fail("string offset outside the unstructured block");
      return std::string();
    }
    const void* z = memchr(u_ + off, 0, cbU_ - off);
    if (!z) {
      r_.Fail("unterminated string");
      return std::string();
    }
    return std::string(reinterpret_cast<const char*>(u_ + off), static_cast<const uint8_t*>(z) - (u_ + off));
  }

  bool ReadType(uint32_t off, Type& t) {
    if (off > cbU_ || cbU_ - off < 28) {
      r_.Fail("type offset outside the unstructured block");
      return false;
    }
    uint32_t w[7];
    memcpy(w, u_ + off, sizeof(w));
    t.name = Str(w[0]);
    t.varType = w[1];
    t.elements = w[2];
    t.objType = w[6];
    return r_.ok;
  }

  void Annotations() {
    const uint32_t n = r_.U32();
    if (n > 4096) r_.Fail("implausible annotation count");
    for (uint32_t i = 0; i < n && r_.ok; ++i) {
      r_.U32();  // oName
      Type t;
      if (!ReadType(r_.U32(), t)) return;
      if (t.varType == 2 && t.objType == 1)
        r_.Skip(t.elements ? t.elements : 1);  // string offsets
      else
        r_.Skip(1);  // oDefaultValue
    }
  }

  FxBlob Blob(uint32_t off, bool isInline) {
    FxBlob b;
    if (off == 0) return b;  // NULL shader
    if (off > cbU_ || cbU_ - off < 4) {
      r_.Fail("shader offset outside the unstructured block");
      return b;
    }
    uint32_t n;
    memcpy(&n, u_ + off, 4);
    if (n > cbU_ - off - 4) {
      r_.Fail("shader blob outside the unstructured block");
      return b;
    }
    b.p = u_ + off + 4;
    b.n = n;
    ++blobs_;
    if (isInline) ++inline_;
    return b;
  }

  const FxObjectVar* FindVar(const FxEffect& fx, const std::string& name) {
    for (const FxObjectVar& v : fx.vars)
      if (v.name == name) return &v;
    return nullptr;
  }

  // Shader selected by one pass assignment (ECAT_* types of FX11).
  bool IsOtherName(const FxEffect& fx, const std::string& name) {
    for (const std::string& n : fx.otherNames)
      if (n == name) return true;
    return false;
  }

  void Resolve(const FxEffect& fx, FxAssign& a) {
    switch (a.type) {
      case 2: {  // variable
        const std::string name = Str(a.init);
        const FxObjectVar* v = FindVar(fx, name);
        if (v && !v->shaders.empty()) a.shader = v->shaders[0];
        a.unresolved = !v && !IsOtherName(fx, name);
        break;
      }
      case 3: {  // constant index into an array variable
        if (a.init > cbU_ || cbU_ - a.init < 8) return r_.Fail("initializer outside the unstructured block");
        uint32_t w[2];
        memcpy(w, u_ + a.init, 8);
        const std::string name = Str(w[0]);
        const FxObjectVar* v = FindVar(fx, name);
        if (v && w[1] < v->shaders.size()) a.shader = v->shaders[w[1]];
        a.unresolved = v ? (!v->shaders.empty() && w[1] >= v->shaders.size()) : !IsOtherName(fx, name);
        break;
      }
      case 7: case 8: {  // inline shader (SM4: oShader, oSODecl; SM5: SBinaryShaderData5)
        if (a.init > cbU_ || cbU_ - a.init < 4) return r_.Fail("initializer outside the unstructured block");
        uint32_t o;
        memcpy(&o, u_ + a.init, 4);
        a.shader = Blob(o, true);
        break;
      }
      case 4: case 5: case 6:
        a.dynamic = true;
        break;
      default:  // constants (e.g. SetGeometryShader(NULL))
        break;
    }
  }

  const uint8_t* p_;
  size_t n_;
  FxReader r_;
  const uint8_t* u_ = nullptr;
  uint32_t cbU_ = 0, blobs_ = 0, inline_ = 0;
};

inline bool ParseFx5(const uint8_t* p, size_t n, FxEffect& fx, std::string& err) {
  FxParser parser(p, n);
  return parser.Parse(fx, err);
}

// DXBC chunk by FourCC (nullptr if absent or malformed).
inline const uint8_t* DxbcChunk(const uint8_t* p, size_t n, const char* cc, uint32_t* size) {
  if (!p || n < 32 || memcmp(p, "DXBC", 4) != 0) return nullptr;
  uint32_t total, count;
  memcpy(&total, p + 24, 4);
  memcpy(&count, p + 28, 4);
  if (total > n || count > 64 || 32ull + 4ull * count > total) return nullptr;
  for (uint32_t c = 0; c < count; ++c) {
    uint32_t off, sz;
    memcpy(&off, p + 32 + 4 * c, 4);
    if (off > total || total - off < 8) return nullptr;
    memcpy(&sz, p + off + 4, 4);
    if (sz > total - off - 8) return nullptr;
    if (memcmp(p + off, cc, 4) == 0) {
      if (size) *size = sz;
      return p + off + 8;
    }
  }
  return nullptr;
}

// D3D10_SB program type of a DXBC shader (0 PS, 1 VS, 2 GS, 3 HS, 4 DS, 5 CS), -1 if unknown.
inline int DxbcProgramType(const uint8_t* p, size_t n) {
  uint32_t sz = 0;
  const uint8_t* c = DxbcChunk(p, n, "SHEX", &sz);
  if (!c) c = DxbcChunk(p, n, "SHDR", &sz);
  if (!c || sz < 4) return -1;
  uint32_t v;
  memcpy(&v, c, 4);
  return static_cast<int>(v >> 16);
}

// The VS of every pass of a technique (by name, any group). Returns false
// with a reason when the technique is missing or a pass has no static VS.
inline bool TechniqueVs(const FxEffect& fx, const char* tech, std::vector<FxBlob>& out, std::string& err) {
  out.clear();
  const FxTechnique* t = nullptr;
  for (const FxTechnique& x : fx.techs)
    if (x.name == tech) {
      if (t) {
        err = std::string("technique ") + tech + " exists in more than one group";
        return false;
      }
      t = &x;
    }
  if (!t) {
    err = std::string("technique ") + tech + " not in the compiled effect";
    return false;
  }
  for (size_t p = 0; p < t->passes.size(); ++p) {
    FxBlob vs;
    int seen = 0;
    for (const FxAssign& a : t->passes[p].assigns) {
      if (a.state != kStateVertexShader) continue;
      ++seen;
      if (a.dynamic) {
        err = std::string(tech) + ": vertex shader selected at run time";
        return false;
      }
      vs = a.shader;
    }
    if (seen != 1 || !vs.p) {
      err = std::string(tech) + ": pass " + std::to_string(p) + (seen ? " has a NULL vertex shader" : " sets no vertex shader");
      return false;
    }
    if (DxbcProgramType(vs.p, vs.n) != 1) {
      err = std::string(tech) + ": pass vertex shader slot holds a non-VS blob";
      return false;
    }
    out.push_back(vs);
  }
  if (out.empty()) {
    err = std::string(tech) + " has no passes";
    return false;
  }
  return true;
}

// ---------------------------------------------------------------------------
// Shadow texture skip (shadow_tex.h): the resources a shadow technique reads,
// from our unmodified (a) compile. DCS's live effects are optimized (no
// bytecode left to reflect), so the read mask is built from (a), which
// compiles the same source with the same key and defines.
// ---------------------------------------------------------------------------

struct TechReads {
  std::string name, why;  // why empty = analysed
  bool ok = false;
  uint32_t passes = 0, shaders = 0;
  std::vector<std::string> bound;  // RDEF binding names of every stage of every pass, sorted, unique
};

// Every resource bound by any shader of any pass of the technique (RDEF of
// each blob, the list FX11 builds its per-shader SRV dependencies from).
// Fails on anything that could hide a shader: a technique that is missing or
// in two groups, an assignment selected at run time or naming no variable, a
// blob without a program or a readable resource list.
inline bool TechniqueReads(const FxEffect& fx, const std::string& tech, TechReads& out) {
  out = TechReads();
  out.name = tech;
  const FxTechnique* t = nullptr;
  int found = 0;
  for (const FxTechnique& x : fx.techs)
    if (x.name == tech) {
      t = &x;
      ++found;
    }
  if (found != 1) {
    out.why = found ? "technique exists in more than one group" : "technique not in the compiled effect";
    return false;
  }
  if (t->passes.empty()) {
    out.why = "technique has no passes";
    return false;
  }
  const char* names[shadowtex::kMaxBindings];
  for (size_t p = 0; p < t->passes.size(); ++p) {
    const std::string at = "pass " + std::to_string(p) + ": ";
    for (const FxAssign& a : t->passes[p].assigns) {
      if (a.dynamic) {
        out.why = at + "a shader or state is selected at run time";
        return false;
      }
      if (a.unresolved) {
        out.why = at + "an assignment names no variable of the effect";
        return false;
      }
      if (!a.shader.p) continue;  // state assignment or NULL shader
      if (DxbcProgramType(a.shader.p, a.shader.n) < 0) {
        out.why = at + "a shader blob has no program";
        return false;
      }
      const int n = shadowtex::ParseDxbcBindings(a.shader.p, a.shader.n, names, shadowtex::kMaxBindings);
      if (n < 0) {
        out.why = at + "a shader blob has no readable resource list";
        return false;
      }
      ++out.shaders;
      for (int i = 0; i < n; ++i) out.bound.push_back(names[i]);
    }
  }
  std::sort(out.bound.begin(), out.bound.end());
  out.bound.erase(std::unique(out.bound.begin(), out.bound.end()), out.bound.end());
  out.passes = static_cast<uint32_t>(t->passes.size());
  out.ok = true;
  return true;
}

// Both shadow techniques of one key (livePasses: the live effect's pass
// counts, compared) and the names of every variable of the effect (the live
// parameter records must all be among them).
inline void AnalyseShadowReads(const FxEffect& fx, const char* const techs[2], const uint32_t livePasses[2],
                               std::vector<TechReads>& reads, std::vector<std::string>& vars);

// Every variable name of the effect (object, numeric, constant buffer,
// interface), sorted and unique: the live parameter records must be among them.
inline void EffectVarNames(const FxEffect& fx, std::vector<std::string>& vars) {
  vars.clear();
  for (const FxObjectVar& v : fx.vars) vars.push_back(v.name);
  for (const std::string& n : fx.otherNames) vars.push_back(n);
  std::sort(vars.begin(), vars.end());
  vars.erase(std::unique(vars.begin(), vars.end()), vars.end());
}

inline void AnalyseShadowReads(const FxEffect& fx, const char* const techs[2], const uint32_t livePasses[2],
                               std::vector<TechReads>& reads, std::vector<std::string>& vars) {
  reads.clear();
  EffectVarNames(fx, vars);
  for (int t = 0; t < 2; ++t) {
    bool dup = false;
    for (const TechReads& x : reads) dup |= x.name == techs[t];
    if (dup) continue;
    TechReads tr;
    if (TechniqueReads(fx, techs[t], tr) && tr.passes != livePasses[t]) {
      tr.ok = false;
      tr.why = "pass count differs from DCS's live effect";
    }
    reads.push_back(std::move(tr));
  }
}

// ---------------------------------------------------------------------------
// In-memory source edits (R12 sec. 1.3). Without QV_SHADOW_INSTANCED the edited
// text preprocesses to the original (verified offline: same effect bytes).
// ---------------------------------------------------------------------------

constexpr const char* kInstDefine = "QV_SHADOW_INSTANCED";
// G-buffer instancing (R13 stage 1): model_vs in vertex_shader.hlsl, the VS
// of every normal* / normal_cockpit* pass. The uniforms.hlsl edit is shared:
// it is active under either define.
constexpr const char* kModelInstDefine = "QV_MODEL_INSTANCED";
constexpr const char* kUniformsGuard = "#if defined(QV_SHADOW_INSTANCED) || defined(QV_MODEL_INSTANCED)";
constexpr const char* kUniformsFile = "model/common/uniforms.hlsl";
constexpr const char* kLkShadowFile = "model/functions/lk_shadow.hlsl";
constexpr const char* kVertexShaderFile = "model/functions/vertex_shader.hlsl";
// Variant selector of CompileEffect (bool true = the shadow variant).
enum : int { kVarRef = 0, kVarShadow = 1, kVarModel = 2 };
// Which files FxInclude edits.
enum : unsigned { kEditUniforms = 1, kEditLkShadow = 2, kEditVertexShader = 4, kEditAll = 7 };

inline size_t CountOf(const std::string& s, const char* what) {
  size_t n = 0;
  for (size_t at = s.find(what); at != std::string::npos; at = s.find(what, at + 1)) ++n;
  return n;
}

// Returns nullptr on success, else why the anchors were not found.
inline const char* EditUniforms(std::string& s) {
  const char* field = "uint posStructOffset;";
  const char* endMark = "// GENERATED CODE END ID: def_uniforms";
  const size_t cb = s.find("cbuffer def_uniforms");
  if (cb == std::string::npos || CountOf(s, field) != 1 || CountOf(s, endMark) != 1)
    return "uniforms.hlsl: anchors not found exactly once";
  const size_t f = s.find(field);
  const size_t end = s.find(endMark);
  if (f < cb || end < f) return "uniforms.hlsl: posStructOffset is not inside def_uniforms";
  const size_t ls = s.rfind('\n', f) + 1;  // npos + 1 == 0
  size_t le = s.find('\n', f);
  if (le == std::string::npos) return "uniforms.hlsl: unexpected end of file";
  // Insert after the end marker's line first (offsets before it stay valid).
  size_t endLine = s.find('\n', end);
  endLine = endLine == std::string::npos ? s.size() : endLine + 1;
  s.insert(endLine, std::string(kUniformsGuard) + "\nstatic uint posStructOffset;\n#endif\n");
  const std::string line = s.substr(ls, le - ls);
  std::string repl = std::string(kUniformsGuard) + "\n\tuint qvPsoBase;\n#else\n" + line + "\n#endif";
  s.replace(ls, le - ls, repl);
  return nullptr;
}

inline const char* EditLkShadow(std::string& s) {
  const char* sig = "VS_OUTPUT_SHADOWS lk_shadow_vs(const VS_INPUT_SHADOWS input)";
  if (CountOf(s, sig) != 1) return "lk_shadow.hlsl: lk_shadow_vs signature not found exactly once";
  const size_t at = s.find(sig);
  const size_t brace = s.find('{', at);
  if (brace == std::string::npos) return "lk_shadow.hlsl: lk_shadow_vs body not found";
  s.insert(brace + 1, std::string("\n#ifdef ") + kInstDefine +
                          "\n\tposStructOffset = qvInstOffsets[qvPsoBase + qvIID];\n#endif");
  std::string repl = std::string("#ifdef ") + kInstDefine +
                     "\nStructuredBuffer<uint> qvInstOffsets : register(t127);\n"
                     "VS_OUTPUT_SHADOWS lk_shadow_vs(const VS_INPUT_SHADOWS input, uint qvIID : SV_InstanceID)\n"
                     "#else\n" +
                     sig + "\n#endif";
  s.replace(at, strlen(sig), repl);
  return nullptr;
}

// model/functions/vertex_shader.hlsl (R13 sec. 3): the same three additions to
// model_vs, behind QV_MODEL_INSTANCED. forest.hlsl's model_vs is not included
// by any effect [V grep of Bazar\shaders], so it is not edited.
inline const char* EditVertexShader(std::string& s) {
  const char* sig = "VS_OUTPUT model_vs(VS_INPUT input)";
  if (CountOf(s, sig) != 1) return "vertex_shader.hlsl: model_vs signature not found exactly once";
  const size_t at = s.find(sig);
  const size_t brace = s.find('{', at);
  if (brace == std::string::npos) return "vertex_shader.hlsl: model_vs body not found";
  s.insert(brace + 1, std::string("\n#ifdef ") + kModelInstDefine +
                          "\n\tposStructOffset = qvInstOffsets[qvPsoBase + qvIID];\n#endif");
  std::string repl = std::string("#ifdef ") + kModelInstDefine +
                     "\nStructuredBuffer<uint> qvInstOffsets : register(t127);\n"
                     "VS_OUTPUT model_vs(VS_INPUT input, uint qvIID : SV_InstanceID)\n"
                     "#else\n" +
                     sig + "\n#endif";
  s.replace(at, strlen(sig), repl);
  return nullptr;
}

// ---------------------------------------------------------------------------
// Include handler: DCS's lookup order over Bazar\shaders, optional edits.
// ---------------------------------------------------------------------------

inline std::string LowerSlashes(std::string s) {
  for (char& c : s) {
    if (c == '\\') c = '/';
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  }
  return s;
}

inline bool ReadWholeFile(const std::wstring& path, std::string& out) {
  HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
  if (h == INVALID_HANDLE_VALUE) return false;
  LARGE_INTEGER sz;
  bool ok = GetFileSizeEx(h, &sz) && sz.QuadPart < (64ll << 20);
  if (ok) {
    out.resize(static_cast<size_t>(sz.QuadPart));
    DWORD got = 0;
    ok = out.empty() || (ReadFile(h, &out[0], static_cast<DWORD>(out.size()), &got, nullptr) && got == out.size());
  }
  CloseHandle(h);
  return ok;
}

inline std::wstring Widen(const std::string& s) {
  std::wstring w(s.size(), L'\0');
  for (size_t i = 0; i < s.size(); ++i) w[i] = static_cast<wchar_t>(static_cast<unsigned char>(s[i]));
  return w;
}

// FNV-1a 64 (cache checksums and source content hashes).
inline uint64_t Fnv64(const void* p, size_t n, uint64_t h = 0xcbf29ce484222325ull) {
  const auto* b = static_cast<const uint8_t*>(p);
  for (size_t i = 0; i < n; ++i) {
    h ^= b[i];
    h *= 0x100000001b3ull;
  }
  return h;
}

// What the compiles of one key read, for the disk cache: every source file
// read by CompileEffect / FxInclude (path relative to the shaders root as
// probed, raw content before our edits) and every include probe that found
// no file (a file appearing there would change the lookup). Set per worker
// thread around one key's compiles (t_sourceLog).
struct SourceFile {
  std::string rel;
  uint64_t size = 0, hash = 0;
};
struct SourceLog {
  std::vector<SourceFile> files;
  std::vector<std::string> missing;
  uint32_t compiles = 0;    // D3DCompile2 calls
  bool unreliable = false;  // a file changed between reads, or exists but was unreadable
  void AddFile(const std::string& relRaw, const std::string& text) {
    const std::string rel = LowerSlashes(relRaw);
    const uint64_t h = Fnv64(text.data(), text.size());
    for (const SourceFile& f : files)
      if (f.rel == rel) {
        if (f.size != text.size() || f.hash != h) unreliable = true;
        return;
      }
    files.push_back({rel, text.size(), h});
  }
  void AddMissing(const std::wstring& path, const std::string& relRaw) {
    if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) {
      unreliable = true;  // there but not readable: not a stable input
      return;
    }
    const std::string rel = LowerSlashes(relRaw);
    for (const std::string& m : missing)
      if (m == rel) return;
    missing.push_back(rel);
  }
};
thread_local SourceLog* t_sourceLog = nullptr;
std::atomic<uint32_t> g_d3dCompiles{0};  // every D3DCompile2 call of CompileEffect (tests: a cache hit adds none)

class FxInclude : public ID3DInclude {
 public:
  // root: ...\Bazar\shaders\ (trailing backslash); folder: the main file's
  // folder relative to root with '/' and trailing '/', e.g. "model/".
  // edit: kEdit* mask of the files to edit in memory.
  FxInclude(const std::wstring& root, const std::string& folder, unsigned edit)
      : root_(root), folder_(folder), edit_(edit) {
    wchar_t full[MAX_PATH];
    const DWORD n = GetFullPathNameW(root_.c_str(), MAX_PATH, full, nullptr);
    rootFull_ = n && n < MAX_PATH ? LowerSlashes(Narrow(full)) : LowerSlashes(Narrow(root_));
  }

  HRESULT __stdcall Open(D3D_INCLUDE_TYPE, LPCSTR name, LPCVOID, LPCVOID* data, UINT* bytes) override {
    *data = nullptr;
    *bytes = 0;
    std::string text, rel;
    bool found = false;
    for (int pass = 0; pass < 2 && !found; ++pass) {
      const std::string relTry = (pass == 0 ? folder_ : std::string()) + name;
      std::wstring path = root_ + Widen(relTry);
      for (wchar_t& c : path)
        if (c == L'/') c = L'\\';
      if (!ReadWholeFile(path, text)) {
        if (t_sourceLog) t_sourceLog->AddMissing(path, relTry);
      } else {
        found = true;
        if (t_sourceLog) t_sourceLog->AddFile(relTry, text);
        wchar_t full[MAX_PATH];
        const DWORD n = GetFullPathNameW(path.c_str(), MAX_PATH, full, nullptr);
        rel = LowerSlashes(n && n < MAX_PATH ? Narrow(full) : relTry);
        if (rel.compare(0, rootFull_.size(), rootFull_) == 0) rel = rel.substr(rootFull_.size());
      }
    }
    if (!found) {
      if (missing.empty()) missing = name;
      return E_FAIL;
    }
    ++opened;
    if ((edit_ & kEditUniforms) && rel == kUniformsFile) {
      if (const char* why = EditUniforms(text)) {
        if (error.empty()) error = why;
        return E_FAIL;
      }
      ++editedUniforms;
    } else if ((edit_ & kEditLkShadow) && rel == kLkShadowFile) {
      if (const char* why = EditLkShadow(text)) {
        if (error.empty()) error = why;
        return E_FAIL;
      }
      ++editedLkShadow;
    } else if ((edit_ & kEditVertexShader) && rel == kVertexShaderFile) {
      if (const char* why = EditVertexShader(text)) {
        if (error.empty()) error = why;
        return E_FAIL;
      }
      ++editedVertexShader;
    }
    char* buf = new char[text.size() + 1];
    memcpy(buf, text.data(), text.size());
    buf[text.size()] = 0;
    *data = buf;
    *bytes = static_cast<UINT>(text.size());
    return S_OK;
  }

  HRESULT __stdcall Close(LPCVOID data) override {
    delete[] static_cast<const char*>(data);
    return S_OK;
  }

  int opened = 0, editedUniforms = 0, editedLkShadow = 0, editedVertexShader = 0;
  std::string missing, error;

 private:
  static std::string Narrow(const std::wstring& w) {
    std::string s(w.size(), '\0');
    for (size_t i = 0; i < w.size(); ++i) s[i] = w[i] < 128 ? static_cast<char>(w[i]) : '?';
    return s;
  }
  std::wstring root_;
  std::string folder_, rootFull_;
  unsigned edit_;
};

// ---------------------------------------------------------------------------
// Compiler (d3dcompiler_47 from System32, as DCS uses) and the static checks
// ---------------------------------------------------------------------------

struct Compiler {
  HMODULE dll = nullptr;
  decltype(&D3DCompile2) compile2 = nullptr;
  decltype(&D3DReflect) reflect = nullptr;
  decltype(&D3DDisassemble) disassemble = nullptr;  // optional (G-buffer check 4)
  std::string path;

  bool Load() {
    if (compile2) return true;
    wchar_t sys[MAX_PATH];
    const UINT n = GetSystemDirectoryW(sys, MAX_PATH);
    if (!n || n >= MAX_PATH - 24) return false;
    std::wstring p = std::wstring(sys) + L"\\d3dcompiler_47.dll";
    dll = LoadLibraryW(p.c_str());
    if (!dll) return false;
    compile2 = reinterpret_cast<decltype(&D3DCompile2)>(GetProcAddress(dll, "D3DCompile2"));
    reflect = reinterpret_cast<decltype(&D3DReflect)>(GetProcAddress(dll, "D3DReflect"));
    disassemble = reinterpret_cast<decltype(&D3DDisassemble)>(GetProcAddress(dll, "D3DDisassemble"));
    char buf[MAX_PATH] = {};
    GetModuleFileNameA(dll, buf, MAX_PATH);
    path = buf;
    return compile2 && reflect;
  }
};

struct Define {
  std::string name, value;
  bool keyValue = true;  // "=value" appears in the shader key (DCS: qword +0x50 != 0)
};

// What one DX11Shader was compiled from.
struct SourceKey {
  std::string path;  // normalized, relative to the shaders root, e.g. "model/def_material.fx"
  std::string key;   // DCS's key string (+0x78)
  std::vector<Define> defines;
};

// DCS's key format (0xa1a60): path [":" {name ["=" value] ";"}...].
inline std::string BuildKey(const std::string& path, const std::vector<Define>& defines) {
  std::string k = path;
  if (!defines.empty()) k += ':';
  for (const Define& d : defines) {
    k += d.name;
    if (d.keyValue) {
      k += '=';
      k += d.value;
    }
    k += ';';
  }
  return k;
}

// Parses a DCS key back into path + defines (used by the offline tests on
// the .fxo cache headers).
inline bool ParseKey(const std::string& key, SourceKey& out) {
  const size_t colon = key.find(':');
  out.key = key;
  out.path = key.substr(0, colon);
  out.defines.clear();
  if (colon == std::string::npos) return !out.path.empty();
  size_t at = colon + 1;
  while (at < key.size()) {
    const size_t semi = key.find(';', at);
    if (semi == std::string::npos) return false;
    const std::string item = key.substr(at, semi - at);
    Define d;
    const size_t eq = item.find('=');
    d.name = item.substr(0, eq);
    d.keyValue = eq != std::string::npos;
    d.value = d.keyValue ? item.substr(eq + 1) : std::string();
    out.defines.push_back(d);
    at = semi + 1;
  }
  return BuildKey(out.path, out.defines) == key;
}

// Folder part of a normalized path, with trailing '/'.
inline std::string FolderOf(const std::string& path) {
  const size_t s = path.rfind('/');
  return s == std::string::npos ? std::string() : path.substr(0, s + 1);
}

// DCS's target and flags [V 0x2e720]; also part of the disk cache's environment.
constexpr const char* kFxTarget = "fx_5_0";
constexpr UINT kFxFlags = D3DCOMPILE_OPTIMIZATION_LEVEL3;

// Compiles one effect as DCS does. edit = serve the edited includes; with
// instanced = kVarShadow (or true) uniforms.hlsl + lk_shadow.hlsl are edited
// and QV_SHADOW_INSTANCED is added (shadow variant b); with kVarModel
// uniforms.hlsl + vertex_shader.hlsl are edited and QV_MODEL_INSTANCED is
// added (G-buffer variant b). edit without instanced edits all three files
// and adds no define: the inertness check of the edits. editsApplied: bit 0
// uniforms, bit 1 lk_shadow, bit 2 vertex_shader.
inline bool CompileEffect(const Compiler& c, const std::wstring& root, const SourceKey& k, bool edit, int instanced,
                          std::vector<uint8_t>& out, std::string& err, double* ms = nullptr,
                          int* editsApplied = nullptr) {
  out.clear();
  if (FolderOf(k.path).size() >= 6 && FolderOf(k.path).compare(FolderOf(k.path).size() - 6, 6, "inter/") == 0) {
    err = "folder ends in inter/ (DCS include rule not reproduced)";
    return false;
  }
  std::string src;
  std::wstring mainPath = root + Widen(k.path);
  for (wchar_t& ch : mainPath)
    if (ch == L'/') ch = L'\\';
  if (!ReadWholeFile(mainPath, src)) {
    if (t_sourceLog) t_sourceLog->AddMissing(mainPath, k.path);
    err = "source file not found under Bazar\\shaders";
    return false;
  }
  if (t_sourceLog) t_sourceLog->AddFile(k.path, src);
  std::vector<D3D_SHADER_MACRO> macros;
  for (const Define& d : k.defines) macros.push_back({d.name.c_str(), d.value.c_str()});
  if (instanced == kVarShadow) macros.push_back({kInstDefine, "1"});
  if (instanced == kVarModel) macros.push_back({kModelInstDefine, "1"});
  macros.push_back({nullptr, nullptr});
  const unsigned mask = !edit ? 0u
                        : instanced == kVarShadow ? kEditUniforms | kEditLkShadow
                        : instanced == kVarModel  ? kEditUniforms | kEditVertexShader
                                                  : kEditAll;
  FxInclude inc(root, FolderOf(k.path), mask);
  ID3DBlob* code = nullptr;
  ID3DBlob* errs = nullptr;
  LARGE_INTEGER t0, t1, f;
  g_d3dCompiles.fetch_add(1);
  if (t_sourceLog) ++t_sourceLog->compiles;
  QueryPerformanceCounter(&t0);
  const HRESULT hr = c.compile2(src.data(), src.size(), k.path.c_str(), macros.data(), &inc, nullptr, kFxTarget,
                                kFxFlags, 0, 0, nullptr, 0, &code, &errs);
  QueryPerformanceCounter(&t1);
  QueryPerformanceFrequency(&f);
  if (ms) *ms = 1000.0 * (t1.QuadPart - t0.QuadPart) / f.QuadPart;
  if (editsApplied)
    *editsApplied = (inc.editedUniforms ? 1 : 0) + (inc.editedLkShadow ? 2 : 0) + (inc.editedVertexShader ? 4 : 0);
  bool ok = SUCCEEDED(hr) && code;
  if (!ok) {
    if (!inc.error.empty()) {
      err = inc.error;
    } else if (errs && errs->GetBufferSize()) {
      const char* m = static_cast<const char*>(errs->GetBufferPointer());
      const char* e = strstr(m, "error");
      std::string line(e ? e : m);
      const size_t nl = line.find('\n');
      err = "compile failed: " + line.substr(0, std::min<size_t>(nl, 200));
    } else if (!inc.missing.empty()) {
      err = "include not found: " + inc.missing;
    } else {
      char b[64];
      snprintf(b, sizeof(b), "compile failed: hr 0x%08lx", static_cast<unsigned long>(hr));
      err = b;
    }
  } else if (edit && instanced == kVarShadow && (inc.editedUniforms != 1 || inc.editedLkShadow != 1)) {
    ok = false;
    err = inc.editedUniforms != 1 ? "effect does not include model/common/uniforms.hlsl"
                                  : "effect does not include model/functions/lk_shadow.hlsl";
  } else if (edit && instanced == kVarModel && (inc.editedUniforms != 1 || inc.editedVertexShader != 1)) {
    ok = false;
    err = inc.editedUniforms != 1 ? "effect does not include model/common/uniforms.hlsl"
                                  : "effect does not include model/functions/vertex_shader.hlsl exactly once";
  }
  if (ok) {
    const auto* p = static_cast<const uint8_t*>(code->GetBufferPointer());
    out.assign(p, p + code->GetBufferSize());
  }
  if (code) code->Release();
  if (errs) errs->Release();
  return ok;
}

// Result of comparing one pass's VS: (a) reference, (b) instanced.
struct VariantDiff {
  uint32_t instrA = 0, instrB = 0, tempsA = 0, tempsB = 0;
  uint32_t cbOffset = 0, cbSize = 0;  // posStructOffset/qvPsoBase offset, def_uniforms size
  uint32_t iidRegister = 0;
  std::string cbName;
};

// Static checks of R12 sec. 1.3 (1-3) with D3DReflect. Returns nullptr when (b)
// is (a) plus exactly the instancing additions, else the first difference.
inline const char* CompareVariants(const Compiler& c, const FxBlob& a, const FxBlob& b, VariantDiff& d,
                                   std::string& detail) {
  // 1. Output signature, byte for byte (PS linkage).
  uint32_t na = 0, nb = 0;
  const char* osgn = DxbcChunk(a.p, a.n, "OSGN", nullptr) ? "OSGN" : "OSG5";
  const uint8_t* oa = DxbcChunk(a.p, a.n, osgn, &na);
  const uint8_t* ob = DxbcChunk(b.p, b.n, osgn, &nb);
  if (!oa || !ob || na != nb || memcmp(oa, ob, na) != 0) return "output signature differs";
  ID3D11ShaderReflection* ra = nullptr;
  ID3D11ShaderReflection* rb = nullptr;
  if (FAILED(c.reflect(a.p, a.n, __uuidof(ID3D11ShaderReflection), reinterpret_cast<void**>(&ra))) ||
      FAILED(c.reflect(b.p, b.n, __uuidof(ID3D11ShaderReflection), reinterpret_cast<void**>(&rb)))) {
    if (ra) ra->Release();
    return "D3DReflect failed";
  }
  const char* why = nullptr;
  D3D11_SHADER_DESC da = {}, db = {};
  ra->GetDesc(&da);
  rb->GetDesc(&db);
  d.instrA = da.InstructionCount;
  d.instrB = db.InstructionCount;
  d.tempsA = da.TempRegisterCount;
  d.tempsB = db.TempRegisterCount;
  // 2. Input signature: (a)'s entries unchanged, plus one SV_InstanceID.
  if (db.InputParameters != da.InputParameters + 1) why = "input signature: expected exactly one added parameter";
  for (UINT i = 0, j = 0; !why && j < db.InputParameters; ++j) {
    D3D11_SIGNATURE_PARAMETER_DESC pb = {};
    rb->GetInputParameterDesc(j, &pb);
    if (pb.SystemValueType == D3D_NAME_INSTANCE_ID) {
      d.iidRegister = pb.Register;
      continue;
    }
    if (i >= da.InputParameters) {
      why = "input signature: unexpected added parameter";
      break;
    }
    D3D11_SIGNATURE_PARAMETER_DESC pa = {};
    ra->GetInputParameterDesc(i++, &pa);
    if (strcmp(pa.SemanticName, pb.SemanticName) != 0 || pa.SemanticIndex != pb.SemanticIndex ||
        pa.Register != pb.Register || pa.SystemValueType != pb.SystemValueType ||
        pa.ComponentType != pb.ComponentType || pa.Mask != pb.Mask || pa.ReadWriteMask != pb.ReadWriteMask)
      why = "input signature: an existing parameter changed";
  }
  // 3a. Constant buffers: (a)'s, plus the element-type record RDEF keeps for
  // the new structured buffer (D3D_CT_RESOURCE_BIND_INFO "qvInstOffsets").
  int renamed = 0;
  if (!why) {
    D3D11_SHADER_BUFFER_DESC q = {};
    if (db.ConstantBuffers != da.ConstantBuffers + 1 ||
        FAILED(rb->GetConstantBufferByName("qvInstOffsets")->GetDesc(&q)) || q.Type != D3D_CT_RESOURCE_BIND_INFO ||
        q.Size != 4)
      why = "constant buffers: expected (a)'s plus the qvInstOffsets element record";
  }
  for (UINT i = 0; !why && i < da.ConstantBuffers; ++i) {
    ID3D11ShaderReflectionConstantBuffer* ca = ra->GetConstantBufferByIndex(i);
    D3D11_SHADER_BUFFER_DESC ba = {}, bb = {};
    ca->GetDesc(&ba);
    ID3D11ShaderReflectionConstantBuffer* cbb = rb->GetConstantBufferByName(ba.Name);
    if (FAILED(cbb->GetDesc(&bb))) {
      why = "a constant buffer is missing in (b)";
      break;
    }
    if (ba.Type != bb.Type || ba.Size != bb.Size || ba.Variables != bb.Variables || ba.uFlags != bb.uFlags) {
      why = "a constant buffer's size or variable count differs";
      detail = ba.Name;
      break;
    }
    for (UINT v = 0; !why && v < ba.Variables; ++v) {
      ID3D11ShaderReflectionVariable* va = ca->GetVariableByIndex(v);
      ID3D11ShaderReflectionVariable* vb = cbb->GetVariableByIndex(v);
      D3D11_SHADER_VARIABLE_DESC xa = {}, xb = {};
      D3D11_SHADER_TYPE_DESC ta = {}, tb = {};
      va->GetDesc(&xa);
      vb->GetDesc(&xb);
      va->GetType()->GetDesc(&ta);
      vb->GetType()->GetDesc(&tb);
      if (xa.StartOffset != xb.StartOffset || xa.Size != xb.Size || xa.uFlags != xb.uFlags ||
          ta.Class != tb.Class || ta.Type != tb.Type || ta.Rows != tb.Rows || ta.Columns != tb.Columns ||
          ta.Elements != tb.Elements || ta.Members != tb.Members) {
        why = "a constant-buffer variable's offset, size or type differs";
        detail = xa.Name;
        break;
      }
      if (strcmp(xa.Name, xb.Name) != 0) {
        if (strcmp(xa.Name, "posStructOffset") == 0 && strcmp(xb.Name, "qvPsoBase") == 0) {
          ++renamed;
          d.cbOffset = xa.StartOffset;
          d.cbSize = ba.Size;
          d.cbName = ba.Name;
        } else {
          why = "a constant-buffer variable was renamed";
          detail = xa.Name;
        }
      }
    }
  }
  if (!why && renamed != 1) why = "posStructOffset -> qvPsoBase not found exactly once";
  // 3b. Bindings: (a)'s unchanged, plus qvInstOffsets at t127.
  if (!why && db.BoundResources != da.BoundResources + 1) why = "bindings: expected exactly one added binding";
  for (UINT i = 0; !why && i < da.BoundResources; ++i) {
    D3D11_SHADER_INPUT_BIND_DESC ba = {}, bb = {};
    ra->GetResourceBindingDesc(i, &ba);
    if (FAILED(rb->GetResourceBindingDescByName(ba.Name, &bb)) || ba.Type != bb.Type ||
        ba.BindPoint != bb.BindPoint || ba.BindCount != bb.BindCount || ba.uFlags != bb.uFlags ||
        ba.ReturnType != bb.ReturnType || ba.Dimension != bb.Dimension || ba.NumSamples != bb.NumSamples) {
      why = "bindings: an existing binding changed";
      detail = ba.Name;
    }
  }
  if (!why) {
    D3D11_SHADER_INPUT_BIND_DESC q = {};
    if (FAILED(rb->GetResourceBindingDescByName("qvInstOffsets", &q)) || q.Type != D3D_SIT_STRUCTURED ||
        q.BindPoint != 127 || q.BindCount != 1)
      why = "bindings: qvInstOffsets is not a structured buffer at t127";
  }
  ra->Release();
  rb->Release();
  return why;
}

// ---------------------------------------------------------------------------
// Check 4 (R12 sec. 1.3): disassembly diff of (a) and (b), as a dataflow
// compare. fxc reschedules independent instructions and renames temps when
// the two instructions are added (seen on model_vs: the input moves are
// emitted after the loads in (b)), so a line diff cannot pass. Instead:
//  - declarations: (b) = (a) + dcl_input_sgv v<iid>.x, instance_id +
//    dcl_resource_structured t127, 4 (dcl_temps may differ);
//  - opcodes: multiset (b) = (a) + one iadd + one ld_structured_indexable;
//  - dataflow: both programs are evaluated symbolically per component (value
//    numbering; temps are only names). (a)'s posStructOffset read
//    cb<slot>[reg].<comp> and (b)'s qvInstOffsets[qvPsoBase + SV_InstanceID]
//    stand for the same symbol. Every output component must end with the same
//    value: the same operations with the same modifiers on the same inputs, in
//    the same order along every dependency chain. Control flow, indexable
//    temps and unknown operand forms are rejected (not seen in model_vs or
//    lk_shadow_vs).
// ---------------------------------------------------------------------------

struct DisasmDiff {
  uint32_t instrA = 0, instrB = 0;  // instructions, without ret
  uint32_t outputs = 0;             // output components compared
  std::string extra;                // (b)-only opcodes
};

namespace dfa {

struct Operand {
  std::string text;    // as written, without modifiers and swizzle
  char kind = 0;       // 'r' temp, 'v' input, 'o' output, 'c' cb, 'l' literal, 't' resource, 's' sampler, 'n' null
  int index = -1;      // register number (cb: slot)
  int cbReg = -1;      // static cb register
  std::string cbName;  // "cb<N>" or "icb"
  std::string dynReg;  // dynamic cb index register ("r3"), its component and offset
  int dynComp = 0, dynOff = 0;
  int sw[4] = {0, 1, 2, 3};
  int swn = 0;  // swizzle/mask letters (0 = none)
  std::vector<std::string> lit;
  bool neg = false, abs = false;
};

inline std::string Trim(const std::string& s) {
  size_t a = 0, b = s.size();
  while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r')) ++a;
  while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r')) --b;
  return s.substr(a, b - a);
}

inline int CompOf(char c) { return c == 'x' ? 0 : c == 'y' ? 1 : c == 'z' ? 2 : c == 'w' ? 3 : -1; }

// Splits at commas outside () and [].
inline std::vector<std::string> SplitOperands(const std::string& s) {
  std::vector<std::string> out;
  int depth = 0;
  std::string cur;
  for (char c : s) {
    if (c == '(' || c == '[') ++depth;
    if (c == ')' || c == ']') --depth;
    if (c == ',' && depth == 0) {
      out.push_back(Trim(cur));
      cur.clear();
    } else {
      cur += c;
    }
  }
  if (!Trim(cur).empty()) out.push_back(Trim(cur));
  return out;
}

inline bool ParseOperand(std::string s, Operand& o) {
  s = Trim(s);
  if (!s.empty() && s[0] == '-') {
    o.neg = true;
    s = Trim(s.substr(1));
  }
  if (s.size() >= 2 && s[0] == '|') {
    const size_t e = s.find('|', 1);
    if (e == std::string::npos) return false;
    o.abs = true;
    s = s.substr(1, e - 1) + s.substr(e + 1);
  }
  if (s == "null") {
    o.kind = 'n';
    return true;
  }
  if (s.compare(0, 2, "l(") == 0) {
    const size_t e = s.rfind(')');
    if (e == std::string::npos) return false;
    o.kind = 'l';
    o.text = s;
    for (const std::string& x : SplitOperands(s.substr(2, e - 2))) o.lit.push_back(x);
    return !o.lit.empty() && o.lit.size() <= 4;
  }
  // Swizzle / mask after the last '.' that is not inside [] or a number.
  std::string base = s, sw;
  const size_t dot = s.rfind('.');
  const size_t br = s.rfind(']');
  if (dot != std::string::npos && (br == std::string::npos || dot > br)) {
    base = s.substr(0, dot);
    sw = s.substr(dot + 1);
  }
  if (sw.size() > 4) return false;
  for (size_t i = 0; i < sw.size(); ++i) {
    const int c = CompOf(sw[i]);
    if (c < 0) return false;
    o.sw[i] = c;
  }
  o.swn = static_cast<int>(sw.size());
  o.text = base;
  const bool icb = base.compare(0, 4, "icb[") == 0;
  if (base.compare(0, 2, "cb") == 0 || icb) {
    o.kind = 'c';
    const size_t lb = base.find('[');
    const size_t rb = base.rfind(']');
    if (lb == std::string::npos || rb == std::string::npos || rb < lb) return false;
    o.index = icb ? -1 : atoi(base.c_str() + 2);  // icb: the immediate constant buffer (declared, compared)
    o.cbName = base.substr(0, lb);
    const std::string idx = Trim(base.substr(lb + 1, rb - lb - 1));
    if (!idx.empty() && idx[0] >= '0' && idx[0] <= '9') {
      o.cbReg = atoi(idx.c_str());
      return true;
    }
    // r<N>.<c> [+ K]
    const size_t d = idx.find('.');
    if (idx.empty() || idx[0] != 'r' || d == std::string::npos || d + 1 >= idx.size()) return false;
    o.dynReg = idx.substr(0, d);
    o.dynComp = CompOf(idx[d + 1]);
    const size_t plus = idx.find('+');
    o.dynOff = plus == std::string::npos ? 0 : atoi(idx.c_str() + plus + 1);
    return o.dynComp >= 0;
  }
  const char k = base.empty() ? 0 : base[0];
  if ((k == 'r' || k == 'v' || k == 'o' || k == 't' || k == 's') && base.size() >= 2 && base[1] >= '0' &&
      base[1] <= '9' && base.find_first_not_of("0123456789", 1) == std::string::npos) {
    o.kind = k;
    o.index = atoi(base.c_str() + 1);
    return true;
  }
  return false;  // indexable temps, special registers: not handled
}

struct Program {
  std::vector<std::string> decls;
  struct Instr {
    std::string op;  // full opcode token (with _sat and resource modifiers)
    std::string name;  // opcode without modifiers, e.g. "ld_structured_indexable"
    std::vector<Operand> ops;
  };
  std::vector<Instr> code;
};

inline const char* Parse(const std::string& text, Program& p) {
  size_t at = 0;
  bool header = false;
  while (at < text.size()) {
    size_t nl = text.find('\n', at);
    if (nl == std::string::npos) nl = text.size();
    std::string line = Trim(text.substr(at, nl - at));
    at = nl + 1;
    if (line.empty() || line.compare(0, 2, "//") == 0) continue;
    if (!header) {
      header = true;  // "vs_5_0"
      continue;
    }
    if (line.compare(0, 4, "dcl_") == 0) {
      // dcl_immediateConstantBuffer { {..}, ... } spans lines: keep it as one.
      int braces = 0;
      for (char ch : line) braces += ch == '{' ? 1 : ch == '}' ? -1 : 0;
      while (braces > 0 && at < text.size()) {
        size_t e = text.find('\n', at);
        if (e == std::string::npos) e = text.size();
        const std::string more = Trim(text.substr(at, e - at));
        at = e + 1;
        for (char ch : more) braces += ch == '{' ? 1 : ch == '}' ? -1 : 0;
        line += ' ';
        line += more;
      }
      if (braces != 0) return "unterminated declaration";
      p.decls.push_back(line);
      continue;
    }
    if (line[0] == '[' || line[0] == '{') return "instruction prefix not handled";
    // Opcode: up to the first space outside parentheses.
    int depth = 0;
    size_t sp = std::string::npos;
    for (size_t i = 0; i < line.size(); ++i) {
      if (line[i] == '(') ++depth;
      if (line[i] == ')') --depth;
      if (line[i] == ' ' && depth == 0) {
        sp = i;
        break;
      }
    }
    Program::Instr in;
    in.op = sp == std::string::npos ? line : line.substr(0, sp);
    in.name = in.op.substr(0, in.op.find('('));
    if (in.name.size() > 4 && in.name.compare(in.name.size() - 4, 4, "_sat") == 0)
      in.name.resize(in.name.size() - 4);
    if (sp != std::string::npos)
      for (const std::string& s : SplitOperands(line.substr(sp + 1))) {
        Operand o;
        if (!ParseOperand(s, o)) return "operand form not handled";
        in.ops.push_back(o);
      }
    p.code.push_back(std::move(in));
  }
  return header ? nullptr : "empty disassembly";
}

// Value numbering shared by both programs.
class Values {
 public:
  int Id(const std::string& s) {
    auto it = ids_.find(s);
    if (it != ids_.end()) return it->second;
    const int id = static_cast<int>(ids_.size());
    ids_.emplace(s, id);
    return id;
  }
  int Apply(const std::string& op, const std::vector<int>& args) {
    std::string k = op;
    k += '(';
    for (int a : args) {
      k += std::to_string(a);
      k += ',';
    }
    k += ')';
    return Id(k);
  }

  // Debug: the expression a value id stands for.
  std::string Name(int id) const {
    for (auto& kv : ids_)
      if (kv.second == id) return kv.first;
    return "?";
  }

 private:
  std::unordered_map<std::string, int> ids_;
};

struct Special {
  int cbSlot = -1, cbReg = -1, cbComp = -1;  // posStructOffset / qvPsoBase location
  bool instanced = false;                    // (b)
  int iidReg = -1;                           // (b): SV_InstanceID input register (.x)
};

// Evaluates one program; outputs: "o<N>.<c>" -> value.
inline const char* Evaluate(const Program& p, const Special& sp, Values& vals,
                            std::map<std::string, int>& outputs) {
  std::unordered_map<int, std::array<int, 4>> temps;
  const int undef = vals.Id("undef");
  const int pso = vals.Id("PSO");
  const int base = vals.Id("BASE"), iid = vals.Id("IID");
  const int iadd1 = vals.Apply("iadd", {iid, base}), iadd2 = vals.Apply("iadd", {base, iid});
  auto readComp = [&](const Operand& o, int pos, int& out) -> const char* {
    const int c = o.swn == 0 ? pos : o.swn == 1 ? o.sw[0] : o.sw[pos < o.swn ? pos : o.swn - 1];
    int v;
    switch (o.kind) {
      case 'r': {
        auto it = temps.find(o.index);
        v = it == temps.end() ? undef : it->second[c];
        break;
      }
      case 'v':
        v = sp.instanced && o.index == sp.iidReg && c == 0 ? iid
                                                           : vals.Id("v" + std::to_string(o.index) + "." + "xyzw"[c]);
        break;
      case 'c':
        if (o.cbReg >= 0) {
          if (o.index == sp.cbSlot && o.cbReg == sp.cbReg && c == sp.cbComp)
            v = sp.instanced ? base : pso;
          else
            v = vals.Id(o.text + "." + "xyzw"[c]);
        } else {
          auto it = temps.find(atoi(o.dynReg.c_str() + 1));
          const int idx = it == temps.end() ? undef : it->second[o.dynComp];
          v = vals.Id(o.cbName + "[" + std::to_string(idx) + "+" + std::to_string(o.dynOff) +
                      "]." + "xyzw"[c]);
        }
        break;
      case 'l':
        v = vals.Id("l:" + o.lit[o.lit.size() == 1 ? 0 : (pos < static_cast<int>(o.lit.size()) ? pos : 0)]);
        break;
      default:
        return "unexpected source operand";
    }
    if (o.abs) v = vals.Apply("abs", {v});
    if (o.neg) v = vals.Apply("neg", {v});
    out = v;
    return nullptr;
  };
  auto write = [&](const Operand& d, int c, int v) -> const char* {
    if (d.kind == 'n') return nullptr;
    if (d.kind == 'r') {
      auto it = temps.find(d.index);
      if (it == temps.end()) it = temps.emplace(d.index, std::array<int, 4>{undef, undef, undef, undef}).first;
      it->second[c] = v;
      return nullptr;
    }
    if (d.kind == 'o') {
      outputs["o" + std::to_string(d.index) + "." + "xyzw"[c]] = v;
      return nullptr;
    }
    return "unexpected destination operand";
  };
  auto mask = [](const Operand& d, int* out) {
    if (d.swn == 0) {
      for (int i = 0; i < 4; ++i) out[i] = i;
      return 4;
    }
    for (int i = 0; i < d.swn; ++i) out[i] = d.sw[i];
    return d.swn;
  };
  static const char* const kCw[] = {
      "mov", "movc", "add", "mul", "mad", "div", "min", "max", "lt", "ge", "eq", "ne", "iadd", "imad", "ineg",
      "imin", "imax", "ilt", "ige", "ieq", "ine", "ishl", "ishr", "ushr", "and", "or", "xor", "not", "ftou", "ftoi",
      "utof", "itof", "rsq", "sqrt", "rcp", "exp", "log", "frc", "round_ne", "round_ni", "round_pi", "round_z",
      "f16tof32", "f32tof16", "ubfe", "ibfe", "bfi", "bfrev", "countbits", "firstbit_hi", "firstbit_lo",
      "firstbit_shi", "umin", "umax", "ult", "uge", "umad"};
  static const char* const kComm2[] = {"add", "mul", "min", "max", "eq", "ne", "iadd", "and", "or", "xor",
                                       "ieq", "ine", "imin", "imax", "umin", "umax"};
  static const char* const kTwoDst[] = {"sincos", "udiv", "imul", "umul", "uaddc", "usubb", "swapc"};
  static const char* const kFlow[] = {"if_z", "if_nz", "else", "endif", "loop", "endloop", "break", "breakc_z",
                                      "breakc_nz", "continue", "continuec_z", "continuec_nz", "switch", "case",
                                      "default", "endswitch", "call", "callc_z", "callc_nz", "retc_z", "retc_nz",
                                      "label", "discard_z", "discard_nz"};
  auto in = [](const std::string& n, const char* const* list, size_t count) {
    for (size_t i = 0; i < count; ++i)
      if (n == list[i]) return true;
    return false;
  };
  for (size_t i = 0; i < p.code.size(); ++i) {
    const Program::Instr& ins = p.code[i];
    const std::string& n = ins.name;
    if (n == "ret") {
      if (i + 1 != p.code.size()) return "ret before the end";
      continue;
    }
    if (in(n, kFlow, sizeof(kFlow) / sizeof(*kFlow))) return "control flow not handled";
    if (ins.ops.empty()) return "instruction without operands";
    int comps[4];
    const char* err = nullptr;
    // An instruction reads all its sources before it writes: destination
    // components are collected here and written at the end (mul r0, r0.xxxx).
    struct Pending {
      size_t dst;
      int comp, value;
    };
    std::vector<Pending> pend;
    if (n == "dp2" || n == "dp3" || n == "dp4") {
      if (ins.ops.size() != 3) return "dp: operand count";
      const int len = n[2] - '0';
      std::vector<int> args;
      for (int s = 1; s <= 2 && !err; ++s)
        for (int k = 0; k < len && !err; ++k) {
          int v;
          err = readComp(ins.ops[s], k, v);
          args.push_back(v);
        }
      if (err) return err;
      // dot(a, b) == dot(b, a) bit for bit (same products, same sum order).
      if (std::lexicographical_compare(args.begin() + len, args.end(), args.begin(), args.begin() + len))
        std::rotate(args.begin(), args.begin() + len, args.end());
      const int v = vals.Apply(ins.op, args);
      const int m = mask(ins.ops[0], comps);
      for (int k = 0; k < m; ++k) pend.push_back({0, comps[k], v});
    } else if (n.compare(0, 13, "ld_structured") == 0) {
      if (ins.ops.size() != 4 || ins.ops[3].kind != 't') return "ld_structured: operand form";
      int addr, off;
      if ((err = readComp(ins.ops[1], 0, addr)) || (err = readComp(ins.ops[2], 0, off))) return err;
      const Operand& res = ins.ops[3];
      const int m = mask(ins.ops[0], comps);
      for (int k = 0; k < m && !err; ++k) {
        int v;
        if (sp.instanced && res.index == 127)
          v = (addr == iadd1 || addr == iadd2) && off == vals.Id("l:0") ? pso : vals.Id("t127 misuse");
        else
          v = vals.Apply(ins.op + " " + res.text + "." + "xyzw"[res.sw[comps[k]]], {addr, off});
        pend.push_back({0, comps[k], v});
      }
    } else if (in(n, kTwoDst, sizeof(kTwoDst) / sizeof(*kTwoDst))) {
      if (ins.ops.size() < 3) return "two-destination op: operand count";
      for (int d = 0; d < 2 && !err; ++d) {
        if (ins.ops[d].kind == 'n') continue;
        const int m = mask(ins.ops[d], comps);
        for (int k = 0; k < m && !err; ++k) {
          std::vector<int> args;
          for (size_t s = 2; s < ins.ops.size() && !err; ++s) {
            int v;
            err = readComp(ins.ops[s], comps[k], v);
            args.push_back(v);
          }
          if (!err) pend.push_back({static_cast<size_t>(d), comps[k], vals.Apply(ins.op + "#" + std::to_string(d), args)});
        }
      }
    } else {
      // Component-wise ops read each source at the destination component's
      // position; any other op (resource loads, samples, unknown) is taken to
      // depend on all four positions of every source (conservative).
      const bool cw = in(n, kCw, sizeof(kCw) / sizeof(*kCw));
      const int m = mask(ins.ops[0], comps);
      for (int k = 0; k < m && !err; ++k) {
        std::vector<int> args;
        for (size_t s = 1; s < ins.ops.size() && !err; ++s) {
          const Operand& o = ins.ops[s];
          if (o.kind == 't' || o.kind == 's') {
            args.push_back(vals.Id(o.text + "." + "xyzw"[o.sw[o.swn ? comps[k] : 0]]));
            continue;
          }
          if (cw) {
            int v;
            err = readComp(o, comps[k], v);
            args.push_back(v);
          } else {
            for (int pos = 0; pos < 4 && !err; ++pos) {
              int v;
              err = readComp(o, pos, v);
              args.push_back(v);
            }
          }
        }
        // Operand order of commutative ops is not significant (IEEE and
        // integer a op b == b op a exactly); fxc swaps them when it reschedules.
        if (cw && args.size() >= 2 && (in(n, kComm2, sizeof(kComm2) / sizeof(*kComm2)) && args.size() == 2 ||
                                       (n == "mad" || n == "imad" || n == "umad") && args.size() == 3) &&
            args[1] < args[0])
          std::swap(args[0], args[1]);
        if (!err) pend.push_back({0, comps[k], vals.Apply(ins.op + (cw ? "" : "@" + std::to_string(comps[k])), args)});
      }
    }
    for (size_t w = 0; w < pend.size() && !err; ++w) err = write(ins.ops[pend[w].dst], pend[w].comp, pend[w].value);
    if (err) return err;
  }
  return nullptr;
}

inline bool Disassemble(const Compiler& c, const FxBlob& b, std::string& out) {
  if (!c.disassemble) return false;
  ID3DBlob* t = nullptr;
  if (FAILED(c.disassemble(b.p, b.n, 0, nullptr, &t)) || !t) return false;
  out.assign(static_cast<const char*>(t->GetBufferPointer()), t->GetBufferSize());
  t->Release();
  while (!out.empty() && out.back() == '\0') out.pop_back();
  return true;
}

}  // namespace dfa

// Check 4 for one VS pair. d is CompareVariants' result for the same pair
// (cbOffset, iidRegister); cbSlot is def_uniforms' VS bind point in (a).
inline const char* CompareDisasm(const Compiler& c, const FxBlob& a, const FxBlob& b, const VariantDiff& d,
                                 int cbSlot, DisasmDiff& out, std::string& detail) {
  std::string ta, tb;
  if (!dfa::Disassemble(c, a, ta) || !dfa::Disassemble(c, b, tb)) return "disassembly: D3DDisassemble failed";
  dfa::Program pa, pb;
  if (const char* w = dfa::Parse(ta, pa)) return detail = "(a)", w;
  if (const char* w = dfa::Parse(tb, pb)) return detail = "(b)", w;
  // Declarations.
  std::map<std::string, int> decl;
  for (const std::string& s : pa.decls)
    if (s.compare(0, 9, "dcl_temps") != 0) ++decl[s];
  for (const std::string& s : pb.decls)
    if (s.compare(0, 9, "dcl_temps") != 0) --decl[s];
  const std::string sgv = "dcl_input_sgv v" + std::to_string(d.iidRegister) + ".x, instance_id";
  const std::string t127 = "dcl_resource_structured t127, 4";
  for (auto& kv : decl) {
    const int want = kv.first == sgv || kv.first == t127 ? -1 : 0;
    if (kv.second != want) return detail = kv.first, "disassembly: declarations differ beyond the two additions";
  }
  if (decl.count(sgv) == 0 || decl.count(t127) == 0) return "disassembly: an expected declaration is missing in (b)";
  // Opcode multiset.
  std::map<std::string, int> ops;
  for (const auto& i : pa.code) ++ops[i.name];
  for (const auto& i : pb.code) --ops[i.name];
  for (auto& kv : ops) {
    const int want = kv.first == "iadd" || kv.first == "ld_structured_indexable" ? -1 : 0;
    if (kv.second != want) return detail = kv.first, "disassembly: opcodes differ beyond one iadd + one ld_structured";
  }
  out.extra = "iadd ld_structured_indexable";
  out.instrA = static_cast<uint32_t>(pa.code.size()) - (!pa.code.empty() && pa.code.back().name == "ret");
  out.instrB = static_cast<uint32_t>(pb.code.size()) - (!pb.code.empty() && pb.code.back().name == "ret");
  // Dataflow.
  dfa::Values vals;
  dfa::Special sa, sb;
  sa.cbSlot = sb.cbSlot = cbSlot;
  sa.cbReg = sb.cbReg = static_cast<int>(d.cbOffset / 16);
  sa.cbComp = sb.cbComp = static_cast<int>(d.cbOffset % 16 / 4);
  sb.instanced = true;
  sb.iidReg = static_cast<int>(d.iidRegister);
  std::map<std::string, int> oa, ob;
  if (const char* w = dfa::Evaluate(pa, sa, vals, oa)) return detail = "(a)", w;
  if (const char* w = dfa::Evaluate(pb, sb, vals, ob)) return detail = "(b)", w;
  if (oa.size() != ob.size()) return "disassembly: (b) writes other output components";
  for (auto& kv : oa) {
    auto it = ob.find(kv.first);
    if (it == ob.end() || it->second != kv.second) return detail = kv.first, "disassembly: dataflow differs";
  }
  out.outputs = static_cast<uint32_t>(oa.size());
  return nullptr;
}

// ---------------------------------------------------------------------------
// R13 stage-1 gate: which def_uniforms members each stage of a technique pass
// reads, from D3DReflect of our unmodified (a) compile (D3D_SVF_USED).
// prevFrameTransform = CB +0x20..+0x5f = mat+0xb0..0xef (the per-item matrix
// that 0x15e20 copies from item+0x60..0x9c). posStructOffset must be read by
// the VS only: in an instanced draw the leader's CB holds the group base.
// ---------------------------------------------------------------------------

constexpr const char* kMaterialCb = "def_uniforms";
constexpr uint32_t kMaterialCbSize = 0x130, kPrevOffset = 0x20, kPrevSize = 0x40, kPsoOffset = 0xfc;
constexpr int kStages = 6;  // PS VS GS HS DS CS (D3D10_SB program type)
constexpr const char* kStageName[kStages] = {"PS", "VS", "GS", "HS", "DS", "CS"};

struct PassGate {
  bool ok = false;          // every stage analysed
  bool prevUnused = false;  // prevFrameTransform unused in every stage
  bool psoVsOnly = false;   // posStructOffset read by the VS and by no other stage
  uint64_t used[2] = {};    // def_uniforms dwords read by any stage (bit i = bytes 4i..4i+3)
  uint32_t stages = 0;      // bit per program type present
  uint32_t psTargets = 0;   // SV_Target outputs of the pass's PS (G-buffer stage 2: the deferred PS writes 5-6)
  std::vector<std::string> names[kStages];  // used def_uniforms members per stage
  std::string why;
  // R14 lead 3 (G1) counter, from (a) independently of the CB gate above:
  // every resource the RDEF chunk of any stage of this pass binds (sorted,
  // unique), as TechniqueReads for one pass. readsOk false: readsWhy says why.
  bool readsOk = false;
  std::vector<std::string> reads;
  std::string readsWhy;
};

inline void MarkDwords(uint64_t m[2], uint32_t off, uint32_t size) {
  for (uint32_t dw = off / 4; dw < (off + size + 3) / 4 && dw < 128; ++dw) m[dw / 64] |= 1ull << (dw % 64);
}

// One shader blob: def_uniforms members it reads.
inline const char* ReflectMaterialCb(const Compiler& c, const FxBlob& b, int& stage, std::vector<std::string>& names,
                                     uint64_t used[2], bool& prev, bool& pso, uint32_t* targets = nullptr) {
  stage = DxbcProgramType(b.p, b.n);
  if (stage < 0 || stage >= kStages) return "unknown shader stage";
  ID3D11ShaderReflection* r = nullptr;
  if (FAILED(c.reflect(b.p, b.n, __uuidof(ID3D11ShaderReflection), reinterpret_cast<void**>(&r))) || !r)
    return "D3DReflect failed";
  const char* why = nullptr;
  D3D11_SHADER_DESC sd = {};
  r->GetDesc(&sd);
  if (targets && stage == 0) {
    *targets = 0;
    for (UINT i = 0; i < sd.OutputParameters; ++i) {
      D3D11_SIGNATURE_PARAMETER_DESC pd = {};
      if (SUCCEEDED(r->GetOutputParameterDesc(i, &pd)) && pd.SystemValueType == D3D_NAME_TARGET) ++*targets;
    }
  }
  for (UINT i = 0; !why && i < sd.ConstantBuffers; ++i) {
    ID3D11ShaderReflectionConstantBuffer* cb = r->GetConstantBufferByIndex(i);
    D3D11_SHADER_BUFFER_DESC bd = {};
    cb->GetDesc(&bd);
    if (bd.Type != D3D_CT_CBUFFER || strcmp(bd.Name, kMaterialCb) != 0) continue;
    if (bd.Size != kMaterialCbSize) {
      why = "def_uniforms has another size";
      break;
    }
    bool layoutPrev = false, layoutPso = false;
    for (UINT v = 0; v < bd.Variables; ++v) {
      D3D11_SHADER_VARIABLE_DESC vd = {};
      cb->GetVariableByIndex(v)->GetDesc(&vd);
      const bool isPrev = strcmp(vd.Name, "prevFrameTransform") == 0;
      const bool isPso = strcmp(vd.Name, "posStructOffset") == 0;
      layoutPrev |= isPrev && vd.StartOffset == kPrevOffset && vd.Size == kPrevSize;
      layoutPso |= isPso && vd.StartOffset == kPsoOffset && vd.Size == 4;
      if (!(vd.uFlags & D3D_SVF_USED)) continue;
      names.push_back(vd.Name);
      MarkDwords(used, vd.StartOffset, vd.Size);
      prev |= isPrev;
      pso |= isPso;
    }
    if (!layoutPrev || !layoutPso) why = "def_uniforms: prevFrameTransform or posStructOffset not at the analysed offset";
  }
  r->Release();
  return why;
}

inline void GatePass(const Compiler& c, const FxPass& pass, PassGate& g) {
  bool prev = false, psoOther = false, psoVs = false;
  for (const FxAssign& a : pass.assigns) {
    if (a.dynamic) {
      g.why = "a shader of the pass is selected at run time";
      return;
    }
    if (!a.shader.p) continue;
    int stage = -1;
    std::vector<std::string> names;
    bool p = false, s = false;
    uint32_t targets = 0;
    if (const char* w = ReflectMaterialCb(c, a.shader, stage, names, g.used, p, s, &targets)) {
      g.why = w;
      return;
    }
    g.stages |= 1u << stage;
    if (stage == 0) g.psTargets = targets;
    for (std::string& n : names) g.names[stage].push_back(std::move(n));
    prev |= p;
    (stage == 1 ? psoVs : psoOther) |= s;
  }
  if (!(g.stages & 2)) {
    g.why = "pass has no vertex shader";
    return;
  }
  g.ok = true;
  g.prevUnused = !prev;
  g.psoVsOnly = psoVs && !psoOther;
}

// The pass's read set for the G1 counter (PassGate::reads): the RDEF binding
// names of every shader the pass selects, any stage. Same rejections as
// TechniqueReads: a shader selected at run time, an assignment naming no
// variable, a blob without a program or a readable resource list.
inline void PassReads(const FxPass& pass, PassGate& g) {
  g.readsOk = false;
  g.reads.clear();
  g.readsWhy.clear();
  std::vector<std::string> reads;
  const char* names[shadowtex::kMaxBindings];
  for (const FxAssign& a : pass.assigns) {
    if (a.dynamic) {
      g.readsWhy = "a shader or state is selected at run time";
      return;
    }
    if (a.unresolved) {
      g.readsWhy = "an assignment names no variable of the effect";
      return;
    }
    if (!a.shader.p) continue;  // state assignment or NULL shader
    if (DxbcProgramType(a.shader.p, a.shader.n) < 0) {
      g.readsWhy = "a shader blob has no program";
      return;
    }
    const int n = shadowtex::ParseDxbcBindings(a.shader.p, a.shader.n, names, shadowtex::kMaxBindings);
    if (n < 0) {
      g.readsWhy = "a shader blob has no readable resource list";
      return;
    }
    for (int i = 0; i < n; ++i) reads.push_back(names[i]);
  }
  std::sort(reads.begin(), reads.end());
  reads.erase(std::unique(reads.begin(), reads.end()), reads.end());
  g.reads = std::move(reads);
  g.readsOk = true;
}

// ---------------------------------------------------------------------------
// Runtime: observed shaders, compile workers, the variant map
// ---------------------------------------------------------------------------

// dx11backend.dll
constexpr uint32_t kShaderVtbl = 0xb43b8;   // .?AVDX11Shader@RenderAPI@@
constexpr uint32_t kEffectVtbl = 0xbe700;   // .?AVCEffect@D3DX11Effects@@
constexpr uint32_t kEffectGetDevice = 0x66a40, kEffectGetDeviceEnd = 0x66a89;
constexpr uint64_t kEffectGetDeviceHash = 0x5b7086e6edd28963ull;
constexpr uint32_t kShaderVarVtbl = 0xbd320;  // .?AUSShaderGlobalVariable@D3DX11Effects@@
constexpr uint32_t kGetVertexShader = 0x5e790, kGetVertexShaderEnd = 0x5e7fa;  // shader variable vt[32]
constexpr uint64_t kGetVertexShaderHash = 0xc13a45cdca71d942ull;
constexpr uint32_t kCtorFields = 0x1c883, kCtorFieldsEnd = 0x1c8d8;  // +0x58, +0x78, +0x98 stores
constexpr uint64_t kCtorFieldsHash = 0xd87db5cfa62bb7b3ull;
constexpr uint32_t kKeyBuilder = 0xa1a60, kKeyBuilderEnd = 0xa1be8;
constexpr uint64_t kKeyBuilderHash = 0xdfc520925718cb4full;
constexpr uint32_t kMacroBuilder = 0xa19d0, kMacroBuilderEnd = 0xa1a55;
constexpr uint64_t kMacroBuilderHash = 0xefaa4847922b6b10ull;
constexpr uint32_t kTechVtbl = 0xb9608;     // .?AUSTechnique@D3DX11Effects@@
constexpr uint32_t kPassVtbl = 0xb96b0;     // .?AUSPassBlock@D3DX11Effects@@
// NGModel.dll
constexpr uint32_t kModelMatVtbl = 0x592f0;  // .?AVModelMaterialMT@model@@

// Where this build has them: the recorded RVAs above, or where reloc.h
// re-found them in another build (0 = not found). Set by VerifyBuild; until
// then the recorded RVAs (the tests' fake modules use those).
struct Addrs {
  uint32_t shader = kShaderVtbl, effect = kEffectVtbl, effectGetDevice = kEffectGetDevice;
  uint32_t shaderVar = kShaderVarVtbl, getVertexShader = kGetVertexShader, tech = kTechVtbl, pass = kPassVtbl;
  uint32_t model = kModelMatVtbl;
};
Addrs g_at;

constexpr int kMaxPasses = 16;  // normal_cockpit*: 9 passes
constexpr int kMaxDefines = 128;
constexpr int kWorkers = 2;

// What a snapshot / key / map entry is for: the shadow casters (R12, techniques
// mat+0x210/0x218) or the main passes (R13, techniques mat+0x1d8 normal* and
// mat+0x1e0 normal_cockpit*, selected by 0x15e20 for model passes 1 and 2).
enum : int { kKindShadow = 0, kKindGb = 1, kKinds = 2 };
constexpr uint32_t kTechOffset[kKinds][2] = {{0x210, 0x218}, {0x1d8, 0x1e0}};

// Raw snapshot of one DX11Shader, taken on the render thread (POD: built
// under SEH, no unwinding objects).
struct Snapshot {
  void* shader;
  void* effect;
  void* techBegin;
  int kind;                  // kKind*, set before the snapshot is taken
  uint64_t tech[2];          // kTechOffset[kind]
  char techName[2][64];
  uint32_t passes[2];        // 0: technique absent (G-buffer cockpit technique only)
  void* dcsVs[2][kMaxPasses];  // FX pass VS objects (not referenced)
  char path[260];
  char key[8192];
  uint32_t defineCount;
  struct {
    char name[0x48];
    char value[0x48];
    uint8_t keyValue;
  } defines[kMaxDefines];
  const char* why;  // set when the snapshot is unusable
  bool dead;        // the DX11Shader was destroyed meanwhile (ForgetShader): its texture reads are not published
  Snapshot* inboxNext;  // R90 H2: link in g_inbox (render thread -> workers, no lock)
};

// Compiled result of one key (shared by every DX11Shader with that key).
struct PassVs {
  std::vector<uint8_t> bytecode;  // (b)
  ID3D11VertexShader* vs = nullptr;
  VariantDiff diff;
  DisasmDiff disasm;  // G-buffer keys (check 4)
};
struct TechVs {
  std::string name;
  std::vector<int> passVs;       // index into KeyResult::vs per pass
  std::vector<PassGate> gates;   // G-buffer keys: per pass, from (a)
};
struct KeyResult {
  std::string key, path, why;  // why empty = OK
  std::string line;            // the logged one-line summary
  int kind = kKindShadow;
  uint32_t index = 0;          // order of completion
  bool ok = false, done = false;
  bool cached = false;         // loaded from the disk cache (no compile ran)
  double msA = 0, msB = 0;
  size_t defines = 0;
  std::vector<PassVs> vs;  // distinct (b) VS blobs
  std::vector<TechVs> techs;
  // Shadow keys, from (a): what each shadow technique binds (shadow_tex.h's
  // read mask) and every variable name of the effect. Empty when (a) did not
  // compile or parse; filled even when (b) fails.
  std::vector<TechReads> reads;
  std::vector<std::string> fxVars;  // sorted
  std::vector<Snapshot*> waiters;  // other DX11Shaders with this key, published when done
};

// Variant map: (DX11Shader*, technique handle, pass) -> our VS. Shadow and
// G-buffer entries share it (their technique handles differ). Lock-free
// lookups (for stage 2), inserts under g_mutex; entries are only dropped at
// unload. The fingerprint (effect, technique array) guards against a reused
// DX11Shader address until the destructor hook is wired (stage 2).
// kFlagBlendNone: the shader's BLEND_MODE define evaluates to BM_NONE (absent,
// "BM_NONE" or "0"; no shader source defines BLEND_MODE itself [V grep]), so
// its normal* passes use DISABLE_ALPHA_BLEND + ENABLE_DEPTH_BUFFER
// (shader_macroses.hlsl). kFlagDeferredP0: pass 0 of the normal* (not
// normal_cockpit*) technique, whose PS writes at least 5 render targets (the
// G-buffer, deferred/GBuffer.hlsl target0-4[5]).
enum : uint8_t { kFlagPrevUnused = 1, kFlagPsoVsOnly = 2, kFlagGate = 4, kFlagBlendNone = 8, kFlagDeferredP0 = 16 };
struct MapEntry {
  std::atomic<void*> shader{nullptr};
  uint64_t tech = 0;
  uint32_t pass = 0;
  uint8_t kind = 0;
  uint8_t flags = 0;          // kFlag*: G-buffer reflection gate of this pass
  uint64_t cbUsed[2] = {};    // def_uniforms dwords read by any stage of this pass
  void* effect = nullptr;
  void* techBegin = nullptr;
  void* dcsVs = nullptr;
  std::atomic<ID3D11VertexShader*> vs{nullptr};  // set once created
  // The finished key and this pass's gate (immutable once published; freed
  // only by Shutdown). GbPassReadTextures reads the pass's read set from it.
  const KeyResult* result = nullptr;
  const PassGate* gate = nullptr;
};
constexpr size_t kMapSize = 8192;  // power of two
MapEntry* g_map = nullptr;
// g_mapUsed: bumped by every publish and every forget (shadow_batch.h keys
// its plans on it: any change to the map invalidates them). g_mapOccupied:
// slots that are not empty (live entries and tombstones), the fill cap.
// g_mapUsedKind: live entries per kind.
std::atomic<uint32_t> g_mapUsed{0};
std::atomic<uint32_t> g_mapOccupied{0};
std::atomic<uint32_t> g_mapUsedKind[kKinds] = {};
std::atomic<uint32_t> g_mapForgotten{0};
std::atomic<bool> g_mapCapLogged{false};
// A forgotten entry (ForgetShader). Lookups probe past it; PublishLocked
// reuses it, so destroyed shaders (mission reloads) do not fill the map.
inline void* MapTomb() { return reinterpret_cast<void*>(uintptr_t{1}); }

inline size_t MapHash(const void* s, uint64_t tech, uint32_t pass) {
  uint64_t h = (reinterpret_cast<uintptr_t>(s) >> 4) * 0x9E3779B97F4A7C15ull;
  h ^= (tech * 0xC2B2AE3D27D4EB4Full) ^ (pass * 0x165667B19E3779F9ull);
  return static_cast<size_t>(h ^ (h >> 29)) & (kMapSize - 1);
}

// The published entry of (shader, tech, pass) whose fingerprint matches the
// shader's current effect and technique array; nullptr if none. A stale entry
// of a destroyed shader at the same address is skipped (PublishLocked adds the
// new one further along the probe chain).
const MapEntry* FindEntry(void* shader, uint64_t tech, uint32_t pass) {
  if (!g_map || !shader) return nullptr;
  uint8_t* s = static_cast<uint8_t*>(shader);
  void* effect = *reinterpret_cast<void**>(s + 0x50);
  void* techBegin = *reinterpret_cast<void**>(s + 0xb0);
  size_t i = MapHash(shader, tech, pass);
  for (size_t probe = 0; probe < kMapSize; ++probe, i = (i + 1) & (kMapSize - 1)) {
    void* k = g_map[i].shader.load(std::memory_order_acquire);
    if (!k) return nullptr;
    const MapEntry& e = g_map[i];
    // The re-load: the slot was not forgotten and reused while its fields were read.
    if (k == shader && e.tech == tech && e.pass == pass && e.effect == effect && e.techBegin == techBegin &&
        e.shader.load(std::memory_order_acquire) == k)
      return &e;
  }
  return nullptr;
}

// Stage 2 entry point: our instanced VS for this shader/technique/pass, or
// nullptr (not ready, rejected, or a different effect at that address).
// Works for the shadow techniques and for normal* / normal_cockpit*.
ID3D11VertexShader* FindVsEx(void* shader, uint64_t tech, uint32_t pass, void** dcsVs);
ID3D11VertexShader* FindVs(void* shader, uint64_t tech, uint32_t pass) { return FindVsEx(shader, tech, pass, nullptr); }

// Also returns the DCS VS the variant was built against (stage 2 checks it).
ID3D11VertexShader* FindVsEx(void* shader, uint64_t tech, uint32_t pass, void** dcsVs) {
  const MapEntry* e = FindEntry(shader, tech, pass);
  if (!e) return nullptr;
  if (dcsVs) *dcsVs = e->dcsVs;
  return e->vs.load(std::memory_order_acquire);
}

// R13 gate: true only when reflection of our unmodified (a) compile proves
// that no stage of this technique pass reads prevFrameTransform (CB
// +0x20..+0x5f = mat+0xb0..0xef), so a batching key may leave those bytes
// out. false: used, not analysed, or not published (then key on them).
bool PrevTransformUnused(void* shader, uint64_t tech, uint32_t pass) {
  const MapEntry* e = FindEntry(shader, tech, pass);
  return e && (e->flags & kFlagGate) && (e->flags & kFlagPrevUnused);
}

// def_uniforms dwords read by any stage of this pass (bit i = CB bytes
// 4i..4i+3 = mat+0x90+4i..), for a minimal exact batching key. false when
// the pass was not analysed.
bool CbUsedDwords(void* shader, uint64_t tech, uint32_t pass, uint64_t out[2]) {
  const MapEntry* e = FindEntry(shader, tech, pass);
  if (!e || !(e->flags & kFlagGate)) return false;
  out[0] = e->cbUsed[0];
  out[1] = e->cbUsed[1];
  return true;
}

// kFlag* of the published entry (0 when none). G-buffer stage 2 reads it.
uint8_t EntryFlags(void* shader, uint64_t tech, uint32_t pass) {
  const MapEntry* e = FindEntry(shader, tech, pass);
  return e ? e->flags : 0;
}

// True when the define list leaves BLEND_MODE at BM_NONE (see kFlagBlendNone):
// absent (#if treats an undefined name as 0 == BM_NONE), "BM_NONE" or "0".
inline bool BlendModeNone(const Snapshot& s) {
  for (uint32_t i = 0; i < s.defineCount && i < static_cast<uint32_t>(kMaxDefines); ++i) {
    if (strcmp(s.defines[i].name, "BLEND_MODE") != 0) continue;
    const char* v = s.defines[i].value;
    return strcmp(v, "BM_NONE") == 0 || strcmp(v, "0") == 0;
  }
  return true;
}

std::atomic<int> g_state{0};  // 0 = not tried, 1 = ready, -1 = unavailable, -2 = unloaded (final)
std::atomic<bool> g_on{false};          // [Model] ShadowInstancing (stage 1: compile + log)
std::atomic<bool> g_collect{false};     // observer queues new shaders (g_on or the suite phase)
std::atomic<bool> g_collectGb{false};   // G-buffer observer queues new shaders ([Suite] GBufferInstCompile)
std::atomic<bool> g_onGb{false};        // [Model] GBufferBatching wants the G-buffer keys collected
std::atomic<bool> g_gbInstalled{false};  // G-buffer observer registered on gb_count.h's hook
std::atomic<bool> g_stop{false};
std::atomic<int> g_inObserver{0};
uint8_t* g_dx = nullptr;
uint8_t* g_ng = nullptr;
void* g_shaderVtbl = nullptr;
void* g_modelVtbl = nullptr;
std::wstring g_root;  // ...\Bazar\shaders\ .
Compiler g_compiler;
std::mutex g_mutex;  // queue, results, device
std::condition_variable g_cv;
std::vector<Snapshot*> g_queue;
std::unordered_map<std::string, KeyResult*> g_results;  // by key
std::vector<std::pair<Snapshot*, KeyResult*>> g_pendingCreate;  // single-threaded device: created on the render thread
// R90 H2: new snapshots from the render thread (ObserveBody), a lock-free push-only stack. Moved into g_queue (in
// arrival order) under g_mutex by whoever holds it next: the pushing thread when try_lock succeeds, a worker
// before it waits (workers re-check every 100 ms), ForgetShader, Snap, Shutdown. Before, the render thread
// took g_mutex for every new shader, behind a worker holding it while the driver created VS objects.
std::atomic<Snapshot*> g_inbox{nullptr};
std::atomic<uint32_t> g_inboxCount{0};
std::atomic<uint32_t> g_pendingCount{0};
HANDLE g_workers[kWorkers] = {};
int g_busy = 0;  // workers inside a compile
ID3D11Device* g_device = nullptr;
bool g_deviceSingleThreaded = false;
std::atomic<bool> g_deviceChecked{false};
std::atomic<uint32_t> g_seenShaders{0}, g_rejectedSnapshots{0}, g_keysDone{0}, g_keysOk{0}, g_vsCreated{0};
std::atomic<uint32_t> g_seenGb{0}, g_rejectedGb{0}, g_keysDoneGb{0}, g_keysOkGb{0};  // G-buffer kind
std::atomic<uint32_t> g_vsCreatedKind[kKinds] = {};
std::atomic<uint32_t> g_logIndex{0};
std::vector<std::string> g_rejectLog;  // under g_mutex: why snapshots were unusable (first few)

// Seen-shader sets (lock-free, insert-only), one per kind: a shader already
// queued for its shadow techniques is queued again for its G-buffer ones.
constexpr size_t kSeenSize = 4096;
std::atomic<void*> g_seen[kSeenSize];
std::atomic<void*> g_seenSetGb[kSeenSize];

// Set when a whole scan found neither the shader nor a free or forgotten slot;
// cleared by ForgetShader. Observe runs for every caster of every frame:
// without it a full set cost a 4096-slot scan per caster on the render thread.
std::atomic<bool> g_seenFull[2] = {};

bool MarkSeen(void* shader, int kind = kKindShadow) {
  std::atomic<void*>* set = kind == kKindGb ? g_seenSetGb : g_seen;
  std::atomic<bool>& full = g_seenFull[kind == kKindGb ? 1 : 0];
  if (full.load(std::memory_order_relaxed)) return false;
  // Forgotten shaders (ForgetShader's tombstone, 1) are skipped by the
  // search and their slots reused by inserts: before, they were never
  // reused, so destroyed shaders (mission reloads) filled the set for good.
  void* const tomb = reinterpret_cast<void*>(uintptr_t{1});
  std::atomic<void*>* reuse = nullptr;
  size_t i = (reinterpret_cast<uintptr_t>(shader) >> 4) * 0x9E3779B97F4A7C15ull >> 52 & (kSeenSize - 1);
  for (size_t probe = 0; probe < kSeenSize; ++probe, i = (i + 1) & (kSeenSize - 1)) {
    void* k = set[i].load(std::memory_order_relaxed);
    if (k == shader) return false;
    if (k == tomb) {
      if (!reuse) reuse = &set[i];
      continue;
    }
    if (!k) {
      std::atomic<void*>* slot = reuse ? reuse : &set[i];
      void* expected = reuse ? tomb : nullptr;
      if (slot->compare_exchange_strong(expected, shader)) return true;
      if (expected == shader) return false;
      if (reuse) {  // the tombstone went meanwhile: retry with the free slot
        reuse = nullptr;
        expected = nullptr;
        if (set[i].compare_exchange_strong(expected, shader)) return true;
        if (expected == shader) return false;
      }
    }
  }
  if (reuse) {
    void* expected = tomb;
    if (reuse->compare_exchange_strong(expected, shader)) return true;
    if (expected == shader) return false;
  }
  full.store(true, std::memory_order_relaxed);
  return false;  // full: ignore
}

// Result-map key: the DCS key, prefixed for the G-buffer kind.
std::string ResultKey(int kind, const char* key) { return (kind == kKindGb ? "gb|" : "") + std::string(key); }

// MSVC std::string at p: copies at most cap-1 chars; false if implausible.
bool CopyStdString(const uint8_t* p, char* out, size_t cap) {
  const uint64_t size = *reinterpret_cast<const uint64_t*>(p + 0x10);
  const uint64_t res = *reinterpret_cast<const uint64_t*>(p + 0x18);
  if (res < 15 || size > res || size >= cap) return false;
  const char* s = res > 15 ? *reinterpret_cast<const char* const*>(p) : reinterpret_cast<const char*>(p);
  memcpy(out, s, size);
  out[size] = 0;
  return strlen(out) == size;
}

bool CopyCString(const uint8_t* p, size_t max, char* out) {
  const void* z = memchr(p, 0, max);
  if (!z) return false;
  const size_t n = static_cast<const uint8_t*>(z) - p;
  memcpy(out, p, n + 1);
  return true;
}

template <typename F>
F VSlot(void* obj, int slot) {
  return reinterpret_cast<F>((*static_cast<void***>(obj))[slot]);
}

// Reads one DX11Shader (render thread). Plain C: runs under __try.
const char* SnapshotRaw(uint8_t* mat, uint8_t* sh, Snapshot& s) {
  s.shader = sh;
  s.effect = *reinterpret_cast<void**>(sh + 0x50);
  s.techBegin = *reinterpret_cast<void**>(sh + 0xb0);
  if (!s.effect || *static_cast<void**>(s.effect) != g_dx + g_at.effect) return "effect at +0x50 is not a CEffect";
  if (!CopyStdString(sh + 0x58, s.path, sizeof(s.path))) return "+0x58 is not a plausible std::string";
  if (!CopyStdString(sh + 0x78, s.key, sizeof(s.key))) return "+0x78 is not a plausible std::string";
  const size_t pl = strlen(s.path);
  if (pl < 4 || !strchr(s.path, '.')) return "+0x58 does not name a shader file";
  if (strncmp(s.key, s.path, pl) != 0) return "+0x78 does not start with +0x58";
  uint8_t* db = *reinterpret_cast<uint8_t**>(sh + 0x98);
  uint8_t* de = *reinterpret_cast<uint8_t**>(sh + 0xa0);
  uint8_t* dc = *reinterpret_cast<uint8_t**>(sh + 0xa8);
  if (de < db || dc < de || (de - db) % 0xa0 != 0 || (de - db) / 0xa0 > kMaxDefines)
    return "+0x98 is not a plausible vector of 0xa0-byte defines";
  s.defineCount = static_cast<uint32_t>((de - db) / 0xa0);
  for (uint32_t i = 0; i < s.defineCount; ++i) {
    uint8_t* e = db + i * 0xa0;
    if (!CopyCString(e + 8, 0x48, s.defines[i].name) || !s.defines[i].name[0]) return "define name not terminated";
    if (!CopyCString(e + 0x58, 0x48, s.defines[i].value)) return "define value not terminated";
    s.defines[i].keyValue = *reinterpret_cast<uint64_t*>(e + 0x50) != 0;
  }
  // Techniques: records of 0x50 bytes at +0xb0, handles 1-based, name =
  // std::string at the record, object at +0x20 [V shadow_tex.h].
  uint8_t* techEnd = *reinterpret_cast<uint8_t**>(sh + 0xb8);
  const uint64_t techCount = (techEnd - static_cast<uint8_t*>(s.techBegin)) / 0x50;
  const int kind = s.kind == kKindGb ? kKindGb : kKindShadow;
  for (int t = 0; t < 2; ++t) {
    s.tech[t] = *reinterpret_cast<uint64_t*>(mat + kTechOffset[kind][t]);
    if (kind == kKindGb && t == 1 && (s.tech[t] < 1 || s.tech[t] > techCount)) {
      s.passes[t] = 0;  // no cockpit technique: only normal* is compiled
      continue;
    }
    if (s.tech[t] < 1 || s.tech[t] > techCount) return "technique handle out of range";
    uint8_t* rec = static_cast<uint8_t*>(s.techBegin) + (s.tech[t] - 1) * 0x50;
    if (!CopyStdString(rec, s.techName[t], sizeof(s.techName[t]))) return "technique name unreadable";
    void* tech = *reinterpret_cast<void**>(rec + 0x20);
    if (!tech || *static_cast<void**>(tech) != g_dx + g_at.tech) return "technique is not an FX technique";
    struct {
      const char* name;
      uint32_t passes, annotations;
    } td = {};
    if (VSlot<long(__fastcall*)(void*, void*)>(tech, 4)(tech, &td) < 0) return "technique GetDesc failed";
    if (td.passes == 0 || td.passes > kMaxPasses) return "unexpected pass count";
    s.passes[t] = td.passes;
    for (uint32_t p = 0; p < td.passes; ++p) {
      s.dcsVs[t][p] = nullptr;
      void* pass = VSlot<void*(__fastcall*)(void*, uint32_t)>(tech, 7)(tech, p);
      if (!pass || *static_cast<void**>(pass) != g_dx + g_at.pass) return "pass is not an FX pass block";
      struct {
        void* var;
        uint32_t index;
      } pd = {};
      if (VSlot<long(__fastcall*)(void*, void*)>(pass, 5)(pass, &pd) < 0 || !pd.var) return "pass VS desc failed";
      if (*static_cast<void**>(pd.var) != g_dx + g_at.shaderVar) continue;  // not a global shader variable
      ID3D11VertexShader* vs = nullptr;
      if (VSlot<long(__fastcall*)(void*, uint32_t, ID3D11VertexShader**)>(pd.var, 32)(pd.var, pd.index, &vs) >= 0 &&
          vs) {
        s.dcsVs[t][p] = vs;
        vs->Release();  // the effect keeps its own reference
      }
    }
  }
  return nullptr;
}

const char* SnapshotGuarded(uint8_t* mat, uint8_t* sh, Snapshot& s) {
  __try {
    return SnapshotRaw(mat, sh, s);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return "access violation while reading the shader";
  }
}

// DCS's device, from the first usable shader (render thread).
long GetDeviceGuarded(void* effect, ID3D11Device** dev) {
  __try {
    return VSlot<long(__fastcall*)(void*, ID3D11Device**)>(effect, 4)(effect, dev);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return E_FAIL;
  }
}

void CheckDevice(const Snapshot& s) {
  if (g_deviceChecked.exchange(true)) return;
  ID3D11Device* dev = nullptr;
  if (GetDeviceGuarded(s.effect, &dev) < 0 || !dev) {
    Log("shadow inst: CEffect::GetDevice failed; no VS objects will be created");
    return;
  }
  // Same device as DCS's own VS of this pass?
  ID3D11Device* vsDev = nullptr;
  void* dcsVs = s.dcsVs[0][0] ? s.dcsVs[0][0] : s.dcsVs[1][0];
  if (dcsVs) static_cast<ID3D11VertexShader*>(dcsVs)->GetDevice(&vsDev);
  const UINT flags = dev->GetCreationFlags();
  const D3D_FEATURE_LEVEL fl = dev->GetFeatureLevel();
  const bool match = !dcsVs || vsDev == dev;
  Log("shadow inst: device %p from CEffect::GetDevice (feature level %x, creation flags 0x%x%s); DCS's shadow VS "
      "belongs to %s",
      dev, static_cast<unsigned>(fl), flags, (flags & D3D11_CREATE_DEVICE_SINGLETHREADED) ? " SINGLETHREADED" : "",
      !dcsVs ? "(no global VS variable to check)" : match ? "the same device" : "ANOTHER device");
  if (vsDev) vsDev->Release();
  if (!match || fl < D3D_FEATURE_LEVEL_11_0) {
    dev->Release();
    Log("shadow inst: device rejected; no VS objects will be created");
    return;
  }
  std::lock_guard<std::mutex> lock(g_mutex);
  g_device = dev;
  g_deviceSingleThreaded = (flags & D3D11_CREATE_DEVICE_SINGLETHREADED) != 0;
}

// Creates the missing VS objects of one key on dev. R90 H2: a worker calls it without g_mutex for a key it is
// still compiling (r is that worker's alone until it marks it done, and the device is free threaded); the
// single-threaded device path calls it through CreateVsLocked.
void CreateVsOn(ID3D11Device* dev, KeyResult& r) {
  if (!r.ok || !dev) return;
  for (PassVs& p : r.vs) {
    if (p.vs) continue;
    if (SUCCEEDED(dev->CreateVertexShader(p.bytecode.data(), p.bytecode.size(), nullptr, &p.vs))) {
      g_vsCreated.fetch_add(1);
      g_vsCreatedKind[r.kind == kKindGb ? kKindGb : kKindShadow].fetch_add(1);
    }
  }
}
// Caller holds g_mutex; runs on a worker, or on the render thread for a single-threaded device.
void CreateVsLocked(KeyResult& r) { CreateVsOn(g_device, r); }

// R90 H2: g_inbox into g_queue, oldest first. Caller holds g_mutex.
void DrainInboxLocked() {
  Snapshot* h = g_inbox.exchange(nullptr, std::memory_order_acquire);
  Snapshot* fifo = nullptr;
  while (h) {  // the stack is newest first
    Snapshot* n = h->inboxNext;
    h->inboxNext = fifo;
    fifo = h;
    h = n;
  }
  while (fifo) {
    Snapshot* n = fifo->inboxNext;
    fifo->inboxNext = nullptr;
    g_queue.push_back(fifo);
    g_inboxCount.fetch_sub(1, std::memory_order_relaxed);
    fifo = n;
  }
}

// R90 H2: the render thread's queue push; never waits for g_mutex.
void InboxPush(Snapshot* s) {
  Snapshot* h = g_inbox.load(std::memory_order_relaxed);
  do {
    s->inboxNext = h;
  } while (!g_inbox.compare_exchange_weak(h, s, std::memory_order_release, std::memory_order_relaxed));
  g_inboxCount.fetch_add(1, std::memory_order_relaxed);
  std::unique_lock<std::mutex> lock(g_mutex, std::try_to_lock);
  if (!lock.owns_lock()) return;  // a worker takes it within 100 ms
  DrainInboxLocked();
  g_cv.notify_all();
}

// Creates the key's VS objects if needed and publishes the shader's map
// entries. Caller holds g_mutex.
void PublishLocked(const Snapshot& s, KeyResult& r) {
  if (!r.ok || !g_map) return;
  CreateVsLocked(r);
  if (s.dead) return;  // destroyed meanwhile (ForgetShader already ran): its entries would never match or be reclaimed
  for (int t = 0; t < 2; ++t) {
    const TechVs* tv = nullptr;
    for (const TechVs& x : r.techs)
      if (x.name == s.techName[t]) tv = &x;
    if (!tv) continue;
    for (uint32_t p = 0; p < s.passes[t] && p < tv->passVs.size(); ++p) {
      size_t i = MapHash(s.shader, s.tech[t], p);
      MapEntry* slot = nullptr;  // the first tombstone on the chain, else the empty slot that ends it
      for (size_t probe = 0; probe < kMapSize; ++probe, i = (i + 1) & (kMapSize - 1)) {
        MapEntry& e = g_map[i];
        void* k = e.shader.load(std::memory_order_relaxed);
        if (k == s.shader && e.tech == s.tech[t] && e.pass == p) {
          if (e.effect == s.effect && e.techBegin == s.techBegin) {  // already published
            slot = nullptr;
            break;
          }
          continue;  // stale entry of a destroyed shader at the same address: keep probing
        }
        if (k == MapTomb()) {
          if (!slot) slot = &e;
          continue;
        }
        if (k) continue;
        if (!slot) {
          if (g_mapOccupied.load() >= kMapSize * 3 / 4) {
            if (!g_mapCapLogged.exchange(true))
              Log("shadow inst: instancing map full (%u entries); shaders published later get no instanced VS",
                  g_mapOccupied.load());
            break;
          }
          slot = &e;
          g_mapOccupied.fetch_add(1);
        }
        break;
      }
      if (slot) {
        MapEntry& e = *slot;  // empty or a tombstone: lookups never match it while it is filled
        e.tech = s.tech[t];
        e.pass = p;
        e.effect = s.effect;
        e.techBegin = s.techBegin;
        e.dcsVs = s.dcsVs[t][p];
        e.kind = static_cast<uint8_t>(r.kind);
        e.flags = 0;
        e.cbUsed[0] = e.cbUsed[1] = 0;
        e.result = &r;
        e.gate = p < tv->gates.size() ? &tv->gates[p] : nullptr;
        bool useVs = true;
        if (p < tv->gates.size()) {
          const PassGate& g = tv->gates[p];
          if (g.ok) {
            const bool deferredP0 = r.kind == kKindGb && t == 0 && p == 0 && tv->name.compare(0, 6, "normal") == 0 &&
                                    tv->name.compare(0, 14, "normal_cockpit") != 0 && g.psTargets >= 5;
            e.flags = static_cast<uint8_t>(kFlagGate | (g.prevUnused ? kFlagPrevUnused : 0) |
                                           (g.psoVsOnly ? kFlagPsoVsOnly : 0) |
                                           (r.kind == kKindGb && BlendModeNone(s) ? kFlagBlendNone : 0) |
                                           (deferredP0 ? kFlagDeferredP0 : 0));
            e.cbUsed[0] = g.used[0];
            e.cbUsed[1] = g.used[1];
          }
          // Another stage reading posStructOffset would see the group base in
          // an instanced draw: no VS for that pass.
          useVs = g.ok && g.psoVsOnly;
        }
        e.vs.store(useVs ? r.vs[tv->passVs[p]].vs : nullptr, std::memory_order_release);
        e.shader.store(s.shader, std::memory_order_release);
        g_mapUsed.fetch_add(1);
        g_mapUsedKind[r.kind == kKindGb ? kKindGb : kKindShadow].fetch_add(1);
      }
    }
  }
}

// ---------------------------------------------------------------------------
// Texture-read map for shadow_tex.h: (DX11Shader*, shadow technique handle) ->
// the finished KeyResult and its TechReads. Published under g_mutex when a key
// finishes (whatever (b) did), lock-free lookups on the render thread. The
// DX11Shader destructor hook of shadow_tex.h calls ForgetShader, so a new
// shader at a reused address is snapshotted and published again.
// ---------------------------------------------------------------------------

struct TexEntry {
  std::atomic<void*> shader{nullptr};  // nullptr = empty, kTexTomb = forgotten
  uint64_t tech = 0;
  void* effect = nullptr;
  void* techBegin = nullptr;
  const KeyResult* r = nullptr;
  int reads = -1;  // index into r->reads; -1 = the key failed before (a) was analysed
};
constexpr size_t kTexMapSize = 4096;  // power of two
inline void* TexTomb() { return reinterpret_cast<void*>(uintptr_t{1}); }
TexEntry* g_texMap = nullptr;
std::atomic<uint32_t> g_texMapUsed{0}, g_texForgotten{0};  // used: slots not empty (live and tombstones)
std::atomic<bool> g_texMapCapLogged{false};
std::atomic<bool> g_onTex{false};  // [Model] ShadowTextureSkip wants the shadow keys collected
Snapshot* g_compiling[kWorkers] = {};  // under g_mutex: snapshots a worker is compiling

inline size_t TexHash(const void* s, uint64_t tech) {
  uint64_t h = (reinterpret_cast<uintptr_t>(s) >> 4) * 0x9E3779B97F4A7C15ull ^ tech * 0xC2B2AE3D27D4EB4Full;
  return static_cast<size_t>(h ^ (h >> 31)) & (kTexMapSize - 1);
}

const TexEntry* FindTex(void* shader, uint64_t tech) {
  if (!g_texMap || !shader) return nullptr;
  uint8_t* s = static_cast<uint8_t*>(shader);
  void* effect = *reinterpret_cast<void**>(s + 0x50);
  void* techBegin = *reinterpret_cast<void**>(s + 0xb0);
  size_t i = TexHash(shader, tech);
  for (size_t probe = 0; probe < kTexMapSize; ++probe, i = (i + 1) & (kTexMapSize - 1)) {
    void* k = g_texMap[i].shader.load(std::memory_order_acquire);
    if (!k) return nullptr;
    const TexEntry& e = g_texMap[i];
    // The re-load: the slot was not forgotten and reused while its fields were read.
    if (k == shader && e.tech == tech && e.effect == effect && e.techBegin == techBegin &&
        e.shader.load(std::memory_order_acquire) == k)
      return &e;
  }
  return nullptr;
}

// Caller holds g_mutex; r is done (immutable from here on but for its VS objects).
void PublishTexLocked(const Snapshot& s, const KeyResult& r) {
  if (!g_texMap || s.dead || s.kind != kKindShadow) return;
  for (int t = 0; t < 2; ++t) {
    int idx = -1;
    for (size_t i = 0; i < r.reads.size(); ++i)
      if (r.reads[i].name == s.techName[t]) idx = static_cast<int>(i);
    size_t i = TexHash(s.shader, s.tech[t]);
    // The first tombstone on the chain is reused (lock-free readers probing
    // past it see another key, or the tombstone, never a half-written match:
    // FindTex re-loads the key after reading the fields); else the empty slot.
    TexEntry* slot = nullptr;
    for (size_t probe = 0; probe < kTexMapSize; ++probe, i = (i + 1) & (kTexMapSize - 1)) {
      TexEntry& e = g_texMap[i];
      void* k = e.shader.load(std::memory_order_relaxed);
      if (k == s.shader && e.tech == s.tech[t] && e.effect == s.effect && e.techBegin == s.techBegin) {
        slot = nullptr;  // already published
        break;
      }
      if (k == TexTomb()) {
        if (!slot) slot = &e;
        continue;
      }
      if (k) continue;
      if (!slot) {
        if (g_texMapUsed.load() >= kTexMapSize * 3 / 4) {
          if (!g_texMapCapLogged.exchange(true))
            Log("shadow inst: texture-read map full (%u entries); shaders published later keep every texture set",
                g_texMapUsed.load());
          break;
        }
        slot = &e;
        g_texMapUsed.fetch_add(1);
      }
      break;
    }
    if (slot) {
      TexEntry& e = *slot;
      e.tech = s.tech[t];
      e.effect = s.effect;
      e.techBegin = s.techBegin;
      e.r = &r;
      e.reads = idx;
      e.shader.store(s.shader, std::memory_order_release);
    }
  }
}

// Render thread (shadow_tex.h, first sight of a shader). The resources bound
// by any stage of any pass of technique tech in our (a) compile of the
// shader's key, and (vars, optional) every variable name of that effect.
// shadowtex::kReadsPending: not compiled yet (or shadow_inst not running);
// kReadsFailed: compiled, but not usable (why says why); kReadsReady.
int ShadowReadTextures(void* shader, uint64_t tech, std::vector<std::string>* bound, std::vector<std::string>* vars,
                       std::string* why, std::string* key) {
  g_inObserver.fetch_add(1);  // seq_cst: Shutdown sets g_stop, then waits for 0
  int st = shadowtex::kReadsPending;
  if (!g_stop.load() && g_state.load() == 1) {
    if (const TexEntry* e = FindTex(shader, tech)) {
      const KeyResult& r = *e->r;
      if (key) *key = r.key;
      if (e->reads < 0) {
        st = shadowtex::kReadsFailed;
        if (why) *why = "shadow inst: " + (r.why.empty() ? std::string("technique not analysed") : r.why);
      } else if (!r.reads[e->reads].ok) {
        st = shadowtex::kReadsFailed;
        if (why) *why = "shadow inst: " + r.reads[e->reads].name + ": " + r.reads[e->reads].why;
      } else {
        st = shadowtex::kReadsReady;
        if (bound) *bound = r.reads[e->reads].bound;
        if (vars) *vars = r.fxVars;
      }
    }
  }
  g_inObserver.fetch_sub(1);
  return st;
}

// G-buffer counterpart (R14 lead 3 counter): the resources bound by any stage
// of pass `pass` of technique handle `tech` in our (a) compile of the
// shader's G-buffer key (PassGate::reads), and (vars, optional) every
// variable name of that effect. kReadsPending: no published map entry (key
// not compiled yet, failed, or never collected); kReadsFailed: the pass's
// read set is not usable (why says why); kReadsReady.
int GbPassReadTextures(void* shader, uint64_t tech, uint32_t pass, std::vector<std::string>* bound,
                       std::vector<std::string>* vars, std::string* why, std::string* key) {
  g_inObserver.fetch_add(1);  // seq_cst: Shutdown sets g_stop, then waits for 0
  int st = shadowtex::kReadsPending;
  if (!g_stop.load() && g_state.load() == 1) {
    const MapEntry* e = FindEntry(shader, tech, pass);
    if (e && e->kind == kKindGb && e->result) {
      if (key) *key = e->result->key;
      if (!e->gate) {
        st = shadowtex::kReadsFailed;
        if (why) *why = "gbuffer inst: pass not analysed";
      } else if (!e->gate->readsOk) {
        st = shadowtex::kReadsFailed;
        if (why) *why = "gbuffer inst: " + e->gate->readsWhy;
      } else {
        st = shadowtex::kReadsReady;
        if (bound) *bound = e->gate->reads;
        if (vars) *vars = e->result->fxVars;
      }
    }
  }
  g_inObserver.fetch_sub(1);
  return st;
}

// Linear-probing tombstones at the end of a cluster (the next slot empty)
// become empty again, walking back over the cluster's trailing tombstones:
// every key of the cluster lies before them, so no lookup needs to probe
// past them (lock-free lookups give the same answer either way). Returns the
// slots freed. Caller holds g_mutex (no insert meanwhile).
template <class E>
uint32_t TrimTombs(E* map, size_t size, void* tomb) {
  uint32_t freed = 0;
  for (size_t i = 0; i < size; ++i) {
    if (map[i].shader.load(std::memory_order_relaxed) != tomb ||
        map[(i + 1) & (size - 1)].shader.load(std::memory_order_relaxed) != nullptr)
      continue;
    for (size_t k = i; map[k].shader.load(std::memory_order_relaxed) == tomb; k = (k - 1) & (size - 1)) {
      map[k].shader.store(nullptr, std::memory_order_release);
      ++freed;
    }
  }
  return freed;
}

// The DX11Shader is being destroyed (shadow_tex.h's destructor hook): forget
// its texture-read and instancing entries and its seen mark, and keep queued
// or in-flight snapshots of it from publishing. Forgotten slots are reused by
// later publishes (tombstones on a probe chain) or emptied (TrimTombs), so
// destroyed shaders (mission reloads, slot changes) do not fill the maps:
// before, both maps stopped publishing for good at 3/4 full.
void ForgetShader(void* shader) {
  if (g_state.load() != 1 || !shader) return;
  std::lock_guard<std::mutex> lock(g_mutex);
  if (g_texMap) {
    bool any = false;
    for (size_t i = 0; i < kTexMapSize; ++i)
      if (g_texMap[i].shader.load(std::memory_order_relaxed) == shader) {
        g_texMap[i].shader.store(TexTomb(), std::memory_order_release);
        g_texForgotten.fetch_add(1);
        any = true;
      }
    if (any) g_texMapUsed.fetch_sub(TrimTombs(g_texMap, kTexMapSize, TexTomb()));
  }
  // Its instancing entries (their VS objects stay with the KeyResult).
  if (g_map) {
    bool any = false;
    for (size_t i = 0; i < kMapSize; ++i)
      if (g_map[i].shader.load(std::memory_order_relaxed) == shader) {
        g_map[i].shader.store(MapTomb(), std::memory_order_release);
        g_mapUsedKind[g_map[i].kind == kKindGb ? kKindGb : kKindShadow].fetch_sub(1);
        g_mapForgotten.fetch_add(1);
        any = true;
      }
    if (any) {
      g_mapOccupied.fetch_sub(TrimTombs(g_map, kMapSize, MapTomb()));
      g_mapUsed.fetch_add(1);  // shadow_batch.h plans keyed on the map are rebuilt
    }
  }
  for (size_t i = 0; i < kSeenSize; ++i) {
    if (g_seen[i].load(std::memory_order_relaxed) == shader) g_seen[i].store(TexTomb());
    // The G-buffer set too: a new shader at this address is collected again.
    if (g_seenSetGb[i].load(std::memory_order_relaxed) == shader) g_seenSetGb[i].store(TexTomb());
  }
  g_seenFull[0].store(false, std::memory_order_relaxed);
  g_seenFull[1].store(false, std::memory_order_relaxed);
  auto kill = [&](Snapshot* s) {
    if (s && s->shader == shader) s->dead = true;
  };
  DrainInboxLocked();  // R90 H2
  for (Snapshot* s : g_queue) kill(s);
  for (Snapshot* s : g_compiling) kill(s);
  for (auto& pc : g_pendingCreate) kill(pc.first);
  for (auto& kv : g_results)
    for (Snapshot* w : kv.second->waiters) kill(w);
}

void CompileGbKey(const Snapshot& s, KeyResult& r);

// Compiles a and b for one snapshot's key and fills r (worker thread).
void CompileKey(const Snapshot& s, KeyResult& r) {
  if (s.kind == kKindGb) {
    CompileGbKey(s, r);
    return;
  }
  SourceKey k;
  k.path = s.path;
  k.key = s.key;
  r.key = s.key;
  r.path = s.path;
  for (uint32_t i = 0; i < s.defineCount; ++i)
    k.defines.push_back({s.defines[i].name, s.defines[i].value, s.defines[i].keyValue != 0});
  r.defines = k.defines.size();
  if (BuildKey(k.path, k.defines) != k.key) {
    r.why = "defines at +0x98 do not reproduce the key at +0x78";
    return;
  }
  for (int t = 0; t < 2; ++t)
    if (strcmp(s.techName[t], "lockon_shadows") != 0 && strcmp(s.techName[t], "lockon_shadows_transparent") != 0) {
      r.why = std::string("technique ") + s.techName[t] + " is not a shadow technique";
      return;
    }
  std::vector<uint8_t> fxA, fxB;
  std::string err;
  if (!CompileEffect(g_compiler, g_root, k, false, kVarRef, fxA, err, &r.msA)) {
    r.why = "(a) " + err;
    return;
  }
  FxEffect ea, eb;
  if (!ParseFx5(fxA.data(), fxA.size(), ea, err)) {
    r.why = "effect parse: " + err;
    return;
  }
  // The texture skip's read sets come from (a) alone.
  const char* techs[2] = {s.techName[0], s.techName[1]};
  AnalyseShadowReads(ea, techs, s.passes, r.reads, r.fxVars);
  if (g_stop.load()) return;
  if (!CompileEffect(g_compiler, g_root, k, true, kVarShadow, fxB, err, &r.msB)) {
    r.why = "(b) " + err;
    return;
  }
  if (!ParseFx5(fxB.data(), fxB.size(), eb, err)) {
    r.why = "effect parse: " + err;
    return;
  }
  for (int t = 0; t < 2; ++t) {
    bool dup = false;
    for (const TechVs& x : r.techs) dup |= x.name == s.techName[t];
    if (dup) continue;
    std::vector<FxBlob> va, vb;
    if (!TechniqueVs(ea, s.techName[t], va, err) || !TechniqueVs(eb, s.techName[t], vb, err)) {
      r.why = err;
      return;
    }
    if (va.size() != vb.size() || va.size() != s.passes[t]) {
      r.why = std::string(s.techName[t]) + ": pass count differs from DCS's live effect";
      return;
    }
    TechVs tv;
    tv.name = s.techName[t];
    for (size_t p = 0; p < vb.size(); ++p) {
      VariantDiff d;
      std::string detail;
      if (const char* why = CompareVariants(g_compiler, va[p], vb[p], d, detail)) {
        r.why = std::string(s.techName[t]) + " pass " + std::to_string(p) + ": " + why +
                (detail.empty() ? "" : " (" + detail + ")");
        return;
      }
      int idx = -1;
      for (size_t i = 0; i < r.vs.size(); ++i)
        if (r.vs[i].bytecode.size() == vb[p].n && memcmp(r.vs[i].bytecode.data(), vb[p].p, vb[p].n) == 0)
          idx = static_cast<int>(i);
      if (idx < 0) {
        PassVs pv;
        pv.bytecode.assign(vb[p].p, vb[p].p + vb[p].n);
        pv.diff = d;
        r.vs.push_back(std::move(pv));
        idx = static_cast<int>(r.vs.size() - 1);
      }
      tv.passVs.push_back(idx);
    }
    r.techs.push_back(std::move(tv));
  }
  r.ok = true;
}

// ---------------------------------------------------------------------------
// G-buffer keys (R13 stage 1): normal* / normal_cockpit*, every pass
// ---------------------------------------------------------------------------

// The main-pass techniques of def_material.fx and its includers
// (def_material_techniques.hlsl: TECH_NAME_GEN(normal|normal_cockpit,
// _cf|_cf_db)). Every pass of them sets model_vs_c.
inline bool IsGbTechnique(const char* name) {
  for (const char* b : {"normal", "normal_cockpit"})
    for (const char* p : {"", "_cf", "_cf_db"})
      if (std::string(name) == std::string(b) + p) return true;
  return false;
}

inline const FxTechnique* FindTechnique(const FxEffect& fx, const std::string& name) {
  const FxTechnique* t = nullptr;
  for (const FxTechnique& x : fx.techs)
    if (x.name == name) {
      if (t) return nullptr;  // ambiguous (TechniqueVs reports it)
      t = &x;
    }
  return t;
}

// def_uniforms bind point of one shader blob (-1 if it binds none).
inline int MaterialCbSlot(const Compiler& c, const FxBlob& b) {
  ID3D11ShaderReflection* r = nullptr;
  if (FAILED(c.reflect(b.p, b.n, __uuidof(ID3D11ShaderReflection), reinterpret_cast<void**>(&r))) || !r) return -1;
  D3D11_SHADER_INPUT_BIND_DESC bd = {};
  const int slot = SUCCEEDED(r->GetResourceBindingDescByName(kMaterialCb, &bd)) && bd.Type == D3D_SIT_CBUFFER
                       ? static_cast<int>(bd.BindPoint)
                       : -1;
  r->Release();
  return slot;
}

// Compiles (a) and the G-buffer (b) for one snapshot's key, runs checks 1-4
// on every pass VS and the reflection gate on every pass (worker thread).
void CompileGbKey(const Snapshot& s, KeyResult& r) {
  r.kind = kKindGb;
  SourceKey k;
  k.path = s.path;
  k.key = s.key;
  r.key = s.key;
  r.path = s.path;
  for (uint32_t i = 0; i < s.defineCount; ++i)
    k.defines.push_back({s.defines[i].name, s.defines[i].value, s.defines[i].keyValue != 0});
  r.defines = k.defines.size();
  if (BuildKey(k.path, k.defines) != k.key) {
    r.why = "defines at +0x98 do not reproduce the key at +0x78";
    return;
  }
  for (int t = 0; t < 2; ++t)
    if (s.passes[t] && !IsGbTechnique(s.techName[t])) {
      r.why = std::string("technique ") + s.techName[t] + " is not a normal* / normal_cockpit* technique";
      return;
    }
  std::vector<uint8_t> fxA, fxB;
  std::string err;
  if (!CompileEffect(g_compiler, g_root, k, false, kVarRef, fxA, err, &r.msA)) {
    r.why = "(a) " + err;
    return;
  }
  if (g_stop.load()) return;
  if (!CompileEffect(g_compiler, g_root, k, true, kVarModel, fxB, err, &r.msB)) {
    r.why = "(b) " + err;
    return;
  }
  FxEffect ea, eb;
  if (!ParseFx5(fxA.data(), fxA.size(), ea, err) || !ParseFx5(fxB.data(), fxB.size(), eb, err)) {
    r.why = "effect parse: " + err;
    return;
  }
  // (a)'s variable names, for the G1 counter's record check (as the shadow keys).
  EffectVarNames(ea, r.fxVars);
  // Checks 1-4 once per distinct (a, b) VS blob pair (all passes share model_vs_c).
  std::vector<std::pair<const uint8_t*, int>> checked;
  for (int t = 0; t < 2; ++t) {
    if (!s.passes[t]) continue;
    bool dup = false;
    for (const TechVs& x : r.techs) dup |= x.name == s.techName[t];
    if (dup) continue;
    std::vector<FxBlob> va, vb;
    if (!TechniqueVs(ea, s.techName[t], va, err) || !TechniqueVs(eb, s.techName[t], vb, err)) {
      r.why = err;
      return;
    }
    const FxTechnique* ta = FindTechnique(ea, s.techName[t]);
    if (!ta || va.size() != vb.size() || va.size() != s.passes[t] || ta->passes.size() != va.size()) {
      r.why = std::string(s.techName[t]) + ": pass count differs from DCS's live effect";
      return;
    }
    TechVs tv;
    tv.name = s.techName[t];
    for (size_t p = 0; p < vb.size(); ++p) {
      int idx = -1;
      for (const auto& c : checked)
        if (c.first == va[p].p && r.vs[c.second].bytecode.size() == vb[p].n &&
            memcmp(r.vs[c.second].bytecode.data(), vb[p].p, vb[p].n) == 0)
          idx = c.second;
      if (idx < 0) {
        VariantDiff d;
        DisasmDiff dd;
        std::string detail;
        const char* why = CompareVariants(g_compiler, va[p], vb[p], d, detail);
        const int slot = why ? -1 : MaterialCbSlot(g_compiler, va[p]);
        if (!why && slot < 0) why = "def_uniforms is not bound in the VS";
        if (!why) why = CompareDisasm(g_compiler, va[p], vb[p], d, slot, dd, detail);
        if (why) {
          r.why = std::string(s.techName[t]) + " pass " + std::to_string(p) + ": " + why +
                  (detail.empty() ? "" : " (" + detail + ")");
          return;
        }
        for (size_t i = 0; i < r.vs.size(); ++i)
          if (r.vs[i].bytecode.size() == vb[p].n && memcmp(r.vs[i].bytecode.data(), vb[p].p, vb[p].n) == 0)
            idx = static_cast<int>(i);
        if (idx < 0) {
          PassVs pv;
          pv.bytecode.assign(vb[p].p, vb[p].p + vb[p].n);
          pv.diff = d;
          pv.disasm = dd;
          r.vs.push_back(std::move(pv));
          idx = static_cast<int>(r.vs.size() - 1);
        }
        checked.push_back({va[p].p, idx});
      }
      tv.passVs.push_back(idx);
      PassGate g;
      GatePass(g_compiler, ta->passes[p], g);
      PassReads(ta->passes[p], g);
      tv.gates.push_back(std::move(g));
    }
    r.techs.push_back(std::move(tv));
  }
  if (r.techs.empty()) {
    r.why = "no normal* technique";
    return;
  }
  r.ok = true;
}

// Sorted, de-duplicated, comma-separated.
inline std::string JoinNames(std::vector<std::string> v) {
  std::sort(v.begin(), v.end());
  v.erase(std::unique(v.begin(), v.end()), v.end());
  std::string s;
  for (const std::string& n : v) s += (s.empty() ? "" : ",") + n;
  return s.empty() ? "-" : s;
}

// Gate summary over every pass of a G-buffer key.
struct GbSummary {
  uint32_t passes = 0, analysed = 0, prevUnused = 0, psoVsOnly = 0;
  std::string prevUsedAt, notAnalysed;  // "tech Pn (PS)" lists
  std::vector<std::string> names[kStages];
  uint64_t used[2] = {};
};
inline GbSummary SummarizeGb(const KeyResult& r) {
  GbSummary s;
  for (const TechVs& t : r.techs)
    for (size_t p = 0; p < t.gates.size(); ++p) {
      const PassGate& g = t.gates[p];
      ++s.passes;
      const std::string at = t.name + " P" + std::to_string(p);
      if (!g.ok) {
        if (s.notAnalysed.size() < 200) s.notAnalysed += (s.notAnalysed.empty() ? "" : ", ") + at + ": " + g.why;
        continue;
      }
      ++s.analysed;
      s.prevUnused += g.prevUnused;
      s.psoVsOnly += g.psoVsOnly;
      s.used[0] |= g.used[0];
      s.used[1] |= g.used[1];
      std::string stages;
      for (int st = 0; st < kStages; ++st) {
        for (const std::string& n : g.names[st]) {
          s.names[st].push_back(n);
          if (n == "prevFrameTransform") stages += (stages.empty() ? "" : "+") + std::string(kStageName[st]);
        }
      }
      if (!stages.empty() && s.prevUsedAt.size() < 200)
        s.prevUsedAt += (s.prevUsedAt.empty() ? "" : ", ") + at + " (" + stages + ")";
    }
  return s;
}

// "a N ms, b M ms", or "cached" for a result loaded from the disk cache.
inline std::string Timing(const KeyResult& r) {
  if (r.cached) return "cached";
  char b[64];
  snprintf(b, sizeof(b), "a %.0f ms, b %.0f ms", r.msA, r.msB);
  return b;
}

void LogGbResult(KeyResult& r, uint32_t index) {
  char key[260];
  snprintf(key, sizeof(key), "%s", r.key.c_str());
  if (r.key.size() >= sizeof(key)) memcpy(key + sizeof(key) - 4, "...", 4);
  char b[1024];
  if (!r.ok) {
    snprintf(b, sizeof(b), "gbuffer inst: [%u] %s (%zu defines): FAILED, %s; %s; key %s", index, r.path.c_str(),
             r.defines, r.why.c_str(), Timing(r).c_str(), key);
    r.line = b;
    Log("%s", b);
    return;
  }
  std::string techs;
  for (const TechVs& t : r.techs)
    techs += (techs.empty() ? "" : ", ") + t.name + " " + std::to_string(t.passVs.size()) + " passes";
  const VariantDiff& d = r.vs[0].diff;
  const DisasmDiff& dd = r.vs[0].disasm;
  uint32_t created = 0;
  for (const PassVs& p : r.vs) created += p.vs != nullptr;
  const GbSummary g = SummarizeGb(r);
  std::string line;
  snprintf(b, sizeof(b),
           "gbuffer inst: [%u] %s (%zu defines): OK, %s; %s; %zu VS (%u created), VS %u->%u instr, "
           "%u->%u temps, +SV_InstanceID v%u, %s %u B posStructOffset->qvPsoBase @0x%x, +qvInstOffsets t127, "
           "dataflow: %u output components identical, (b) adds %s; ",
           index, r.path.c_str(), r.defines, Timing(r).c_str(), techs.c_str(), r.vs.size(), created, dd.instrA, dd.instrB,
           d.tempsA, d.tempsB, d.iidRegister, d.cbName.c_str(), d.cbSize, d.cbOffset, dd.outputs, dd.extra.c_str());
  line = b;
  snprintf(b, sizeof(b),
           "gate: prevFrameTransform (CB +0x20..+0x5f) read in %u of %u passes, any stage -> %s%s%s; "
           "posStructOffset VS-only in %u/%u%s%s; ",
           g.analysed - g.prevUnused, g.passes,
           g.prevUnused == g.passes ? "UNUSED, not part of the batching key" : "USED or not analysed, key on it",
           g.prevUsedAt.empty() ? "" : ": ", g.prevUsedAt.c_str(), g.psoVsOnly, g.passes,
           g.notAnalysed.empty() ? "" : "; not analysed: ", g.notAnalysed.c_str());
  line += b;
  line += "def_uniforms USED: VS " + JoinNames(g.names[1]) + "; PS " + JoinNames(g.names[0]);
  for (int st = 2; st < kStages; ++st)
    if (!g.names[st].empty()) line += std::string("; ") + kStageName[st] + " " + JoinNames(g.names[st]);
  line += std::string("; key ") + key;
  r.line = line;
  Log("%s", line.c_str());
}

// Shadow keys: what the texture skip learned from (a), for the key's log line.
inline std::string ReadsSummary(const KeyResult& r) {
  if (r.reads.empty()) return "; texture reads: not analysed";
  std::string s = "; texture reads:";
  for (const TechReads& t : r.reads) {
    s += " " + t.name + " ";
    if (!t.ok) {
      s += "NOT USABLE (" + t.why + ")";
      continue;
    }
    std::string names;
    for (const std::string& n : t.bound) names += (names.empty() ? "" : ",") + n;
    if (names.size() > 160) names = names.substr(0, 157) + "...";
    s += std::to_string(t.bound.size()) + " bound [" + names + "]";
  }
  s += ", " + std::to_string(r.fxVars.size()) + " variables";
  return s;
}

void LogResult(KeyResult& r, uint32_t index) {
  if (r.kind == kKindGb) {
    r.index = index;
    LogGbResult(r, index);
    return;
  }
  r.index = index;
  char line[1024];
  char key[260];
  snprintf(key, sizeof(key), "%s", r.key.c_str());
  if (r.key.size() >= sizeof(key)) memcpy(key + sizeof(key) - 4, "...", 4);
  if (!r.ok) {
    snprintf(line, sizeof(line), "shadow inst: [%u] %s (%zu defines): FAILED, %s; %s; key %s", index,
             r.path.c_str(), r.defines, r.why.c_str(), Timing(r).c_str(), key);
    r.line = line + ReadsSummary(r);
    Log("%s", r.line.c_str());
    return;
  }
  std::string techs;
  for (const TechVs& t : r.techs) {
    char b[96];
    snprintf(b, sizeof(b), "%s%s %zu pass%s", techs.empty() ? "" : ", ", t.name.c_str(), t.passVs.size(),
             t.passVs.size() == 1 ? "" : "es");
    techs += b;
  }
  const VariantDiff& d = r.vs[0].diff;
  uint32_t created = 0;
  for (const PassVs& p : r.vs) created += p.vs != nullptr;
  snprintf(line, sizeof(line),
           "shadow inst: [%u] %s (%zu defines): OK, %s; %s; %zu VS (%u created), VS %u->%u instr, "
           "%u->%u temps, +SV_InstanceID v%u, %s %u B posStructOffset->qvPsoBase @0x%x, +qvInstOffsets t127; key %s",
           index, r.path.c_str(), r.defines, Timing(r).c_str(), techs.c_str(), r.vs.size(), created, d.instrA, d.instrB,
           d.tempsA, d.tempsB, d.iidRegister, d.cbName.c_str(), d.cbSize, d.cbOffset, key);
  r.line = line + ReadsSummary(r);
  Log("%s", r.line.c_str());
}

// ---------------------------------------------------------------------------
// Disk cache of compiled keys: <module dir>\cache\ (Saved Games\DCS\Scripts\
// DcsQvCull\cache), one file per (kind, key, defines, techniques, live pass
// counts). A hit fills the KeyResult without D3DCompile; the VS objects are
// still created on DCS's device as after a compile. Read and written by the
// compile workers only (never the render thread); a missing or read-only
// folder just means no cache.
//
// File: 32-byte header
//   char magic[8] "QVSICACH", u32 format (kCacheFormat), u32 edit version
//   (kCacheEditVersion), u64 payload size, u64 FNV-1a 64 of the payload;
// payload (u32 lengths/counts, little endian):
//   identity: kind, key, path, defines (name, value, keyValue), both
//     technique names + live pass counts (byte-compared with the snapshot);
//   environment: target, flags, both instancing define names, d3dcompiler_47
//     size + FNV hash (byte-compared);
//   sources: every file read (rel path, size, hash), every include probe
//     that found no file (re-checked on load);
//   result: everything KeyResult needs but the VS objects and the timings:
//     why, ok, (b) VS bytecode + check results per distinct VS, techniques
//     (pass -> VS index, G-buffer gates and per-pass read sets), shadow
//     texture read sets, the effect's variable names.
// Invalidation: any header, identity, environment or source mismatch, a bad
// checksum or a malformed payload rejects the file; the key is compiled and
// the file rewritten (temp file + MoveFileEx, atomic).
// ---------------------------------------------------------------------------

constexpr char kCacheMagic[8] = {'Q', 'V', 'S', 'I', 'C', 'A', 'C', 'H'};
constexpr uint32_t kCacheFormat = 1;  // file layout
// Bump whenever anything that shapes a KeyResult changes: the in-memory source
// edits, the static checks (CompareVariants, CompareDisasm), the gate, the
// texture-read analysis, or the result fields.
// 2: per-pass G-buffer read sets (PassGate::reads*) and the G-buffer keys'
//    variable names (R14 lead 3 counter).
constexpr uint32_t kCacheEditVersion = 2;
constexpr size_t kCacheHeader = 32;

std::mutex g_cacheMutex;  // everything below
std::wstring g_cacheDir;  // trailing backslash; empty = no cache
bool g_cacheConfigured = false;
uint32_t g_cacheEditVersion = kCacheEditVersion;
uint64_t g_cacheCompilerSize = 0, g_cacheCompilerHash = 0;
bool g_cacheWritable = false;
uint32_t g_cacheHits = 0, g_cacheCompiled = 0, g_cacheRejected = 0, g_cacheWriteFails = 0;
uint32_t g_cacheLoggedAt = 0;  // hits + compiled at the last summary
std::map<std::string, std::pair<uint32_t, std::string>> g_cacheRejectWhy;  // reason -> count, first detail
struct SrcMemo {
  uint64_t size, hash;
  FILETIME written;
};
std::unordered_map<std::wstring, SrcMemo> g_cacheSrcMemo;  // full path -> content hash (this start)

// dir: the cache folder (trailing backslash), empty for none. Hashes the
// compiler DLL and creates the folder. nullptr when usable, else why not.
const char* CacheInit(const std::wstring& dir, const Compiler& c, uint32_t editVersion = kCacheEditVersion) {
  std::lock_guard<std::mutex> lock(g_cacheMutex);
  g_cacheConfigured = true;
  g_cacheDir.clear();
  g_cacheWritable = false;
  g_cacheEditVersion = editVersion;
  g_cacheHits = g_cacheCompiled = g_cacheRejected = g_cacheWriteFails = g_cacheLoggedAt = 0;
  g_cacheRejectWhy.clear();
  g_cacheSrcMemo.clear();
  if (dir.empty()) return "no module folder";
  wchar_t p[MAX_PATH];
  const DWORD n = c.dll ? GetModuleFileNameW(c.dll, p, MAX_PATH) : 0;
  std::string bytes;
  if (!n || n >= MAX_PATH || !ReadWholeFile(p, bytes)) return "d3dcompiler_47.dll not readable";
  g_cacheCompilerSize = bytes.size();
  g_cacheCompilerHash = Fnv64(bytes.data(), bytes.size());
  CreateDirectoryW(dir.c_str(), nullptr);
  const DWORD a = GetFileAttributesW(dir.c_str());
  if (a == INVALID_FILE_ATTRIBUTES || !(a & FILE_ATTRIBUTE_DIRECTORY)) return "folder missing and not creatable";
  g_cacheDir = dir;
  g_cacheWritable = true;
  return nullptr;
}

class CacheWriter {
 public:
  std::string b;
  void U8(uint8_t v) { b.push_back(static_cast<char>(v)); }
  void U32(uint32_t v) { b.append(reinterpret_cast<const char*>(&v), 4); }
  void U64(uint64_t v) { b.append(reinterpret_cast<const char*>(&v), 8); }
  void Str(const std::string& s) {
    U32(static_cast<uint32_t>(s.size()));
    b.append(s);
  }
  void Strs(const std::vector<std::string>& v) {
    U32(static_cast<uint32_t>(v.size()));
    for (const std::string& s : v) Str(s);
  }
  void Bytes(const std::vector<uint8_t>& v) {
    U32(static_cast<uint32_t>(v.size()));
    b.append(reinterpret_cast<const char*>(v.data()), v.size());
  }
};

class CacheReader {
 public:
  CacheReader(const uint8_t* p, size_t n) : p_(p), n_(n) {}
  bool ok = true;
  size_t pos = 0;
  bool Take(void* out, size_t k) {
    if (!ok || n_ - pos < k) return ok = false;
    memcpy(out, p_ + pos, k);
    pos += k;
    return true;
  }
  uint8_t U8() {
    uint8_t v = 0;
    Take(&v, 1);
    return v;
  }
  uint32_t U32() {
    uint32_t v = 0;
    Take(&v, 4);
    return v;
  }
  uint64_t U64() {
    uint64_t v = 0;
    Take(&v, 8);
    return v;
  }
  uint32_t Count(uint32_t max) {
    const uint32_t v = U32();
    if (v > max) ok = false;
    return ok ? v : 0;
  }
  std::string Str() {
    const uint32_t k = U32();
    if (!ok || n_ - pos < k) {
      ok = false;
      return std::string();
    }
    std::string s(reinterpret_cast<const char*>(p_ + pos), k);
    pos += k;
    return s;
  }
  std::vector<std::string> Strs() {
    std::vector<std::string> v(Count(1u << 16));
    for (std::string& s : v) s = Str();
    return v;
  }
  std::vector<uint8_t> Bytes() {
    const uint32_t k = U32();
    if (!ok || n_ - pos < k) {
      ok = false;
      return {};
    }
    std::vector<uint8_t> v(p_ + pos, p_ + pos + k);
    pos += k;
    return v;
  }
  bool AtEnd() const { return ok && pos == n_; }

 private:
  const uint8_t* p_;
  size_t n_;
};

inline int KindOf(const Snapshot& s) { return s.kind == kKindGb ? kKindGb : kKindShadow; }

inline void WriteIdentity(CacheWriter& w, const Snapshot& s) {
  w.U32(static_cast<uint32_t>(KindOf(s)));
  w.Str(s.key);
  w.Str(s.path);
  const uint32_t n = std::min<uint32_t>(s.defineCount, kMaxDefines);
  w.U32(n);
  for (uint32_t i = 0; i < n; ++i) {
    w.Str(s.defines[i].name);
    w.Str(s.defines[i].value);
    w.U8(s.defines[i].keyValue ? 1 : 0);
  }
  for (int t = 0; t < 2; ++t) {
    w.Str(s.passes[t] ? s.techName[t] : "");
    w.U32(s.passes[t]);
  }
}

// Caller holds g_cacheMutex (compiler identity).
inline void WriteEnvLocked(CacheWriter& w) {
  w.Str(kFxTarget);
  w.U32(kFxFlags);
  w.Str(kInstDefine);
  w.Str(kModelInstDefine);
  w.U64(g_cacheCompilerSize);
  w.U64(g_cacheCompilerHash);
}

inline void WriteResult(CacheWriter& w, const KeyResult& r) {
  w.Str(r.key);
  w.Str(r.path);
  w.Str(r.why);
  w.U32(static_cast<uint32_t>(r.kind));
  w.U8(r.ok ? 1 : 0);
  w.U64(r.defines);
  w.U32(static_cast<uint32_t>(r.vs.size()));
  for (const PassVs& p : r.vs) {
    w.Bytes(p.bytecode);
    const VariantDiff& d = p.diff;
    for (uint32_t v : {d.instrA, d.instrB, d.tempsA, d.tempsB, d.cbOffset, d.cbSize, d.iidRegister}) w.U32(v);
    w.Str(d.cbName);
    w.U32(p.disasm.instrA);
    w.U32(p.disasm.instrB);
    w.U32(p.disasm.outputs);
    w.Str(p.disasm.extra);
  }
  w.U32(static_cast<uint32_t>(r.techs.size()));
  for (const TechVs& t : r.techs) {
    w.Str(t.name);
    w.U32(static_cast<uint32_t>(t.passVs.size()));
    for (int i : t.passVs) w.U32(static_cast<uint32_t>(i));
    w.U32(static_cast<uint32_t>(t.gates.size()));
    for (const PassGate& g : t.gates) {
      w.U8(g.ok ? 1 : 0);
      w.U8(g.prevUnused ? 1 : 0);
      w.U8(g.psoVsOnly ? 1 : 0);
      w.U64(g.used[0]);
      w.U64(g.used[1]);
      w.U32(g.stages);
      w.U32(g.psTargets);
      for (int st = 0; st < kStages; ++st) w.Strs(g.names[st]);
      w.Str(g.why);
      w.U8(g.readsOk ? 1 : 0);
      w.Strs(g.reads);
      w.Str(g.readsWhy);
    }
  }
  w.U32(static_cast<uint32_t>(r.reads.size()));
  for (const TechReads& t : r.reads) {
    w.Str(t.name);
    w.Str(t.why);
    w.U8(t.ok ? 1 : 0);
    w.U32(t.passes);
    w.U32(t.shaders);
    w.Strs(t.bound);
  }
  w.Strs(r.fxVars);
}

// Reads a result and re-runs the cheap static checks on it: every stored VS
// is a vertex-shader DXBC blob, every pass index is in range, an OK result
// has a VS per pass. nullptr or what is wrong.
inline const char* ReadResult(CacheReader& rd, KeyResult& r) {
  r.key = rd.Str();
  r.path = rd.Str();
  r.why = rd.Str();
  r.kind = rd.U32() == kKindGb ? kKindGb : kKindShadow;
  r.ok = rd.U8() != 0;
  r.defines = static_cast<size_t>(rd.U64());
  r.vs.resize(rd.Count(256));
  for (PassVs& p : r.vs) {
    p.bytecode = rd.Bytes();
    VariantDiff& d = p.diff;
    for (uint32_t* v : {&d.instrA, &d.instrB, &d.tempsA, &d.tempsB, &d.cbOffset, &d.cbSize, &d.iidRegister})
      *v = rd.U32();
    d.cbName = rd.Str();
    p.disasm.instrA = rd.U32();
    p.disasm.instrB = rd.U32();
    p.disasm.outputs = rd.U32();
    p.disasm.extra = rd.Str();
    if (rd.ok && DxbcProgramType(p.bytecode.data(), p.bytecode.size()) != 1) return "a stored VS is not a VS blob";
  }
  r.techs.resize(rd.Count(16));
  for (TechVs& t : r.techs) {
    t.name = rd.Str();
    t.passVs.resize(rd.Count(kMaxPasses));
    for (int& i : t.passVs) {
      const uint32_t v = rd.U32();
      if (rd.ok && v >= r.vs.size()) return "a pass VS index is out of range";
      i = static_cast<int>(v);
    }
    t.gates.resize(rd.Count(kMaxPasses));
    for (PassGate& g : t.gates) {
      g.ok = rd.U8() != 0;
      g.prevUnused = rd.U8() != 0;
      g.psoVsOnly = rd.U8() != 0;
      g.used[0] = rd.U64();
      g.used[1] = rd.U64();
      g.stages = rd.U32();
      g.psTargets = rd.U32();
      for (int st = 0; st < kStages; ++st) g.names[st] = rd.Strs();
      g.why = rd.Str();
      g.readsOk = rd.U8() != 0;
      g.reads = rd.Strs();
      g.readsWhy = rd.Str();
    }
  }
  r.reads.resize(rd.Count(16));
  for (TechReads& t : r.reads) {
    t.name = rd.Str();
    t.why = rd.Str();
    t.ok = rd.U8() != 0;
    t.passes = rd.U32();
    t.shaders = rd.U32();
    t.bound = rd.Strs();
  }
  r.fxVars = rd.Strs();
  if (!rd.AtEnd()) return "payload malformed";
  if (r.ok && (r.vs.empty() || r.techs.empty())) return "OK result without VS";
  for (const TechVs& t : r.techs)
    if (r.ok && t.passVs.empty()) return "OK result without passes";
  return nullptr;
}

inline std::wstring CachePathLocked(const Snapshot& s) {
  CacheWriter id;
  WriteIdentity(id, s);
  char name[40];
  snprintf(name, sizeof(name), "%s%016llx.qvc", KindOf(s) == kKindGb ? "g_" : "s_",
           static_cast<unsigned long long>(Fnv64(id.b.data(), id.b.size())));
  return g_cacheDir + Widen(name);
}

// Size and content hash of a source file (memoized per start, revalidated by
// size and last-write time). false if unreadable.
inline bool SourceHash(const std::wstring& path, uint64_t& size, uint64_t& hash) {
  WIN32_FILE_ATTRIBUTE_DATA fa;
  if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fa)) return false;
  const uint64_t sz = (static_cast<uint64_t>(fa.nFileSizeHigh) << 32) | fa.nFileSizeLow;
  {
    std::lock_guard<std::mutex> lock(g_cacheMutex);
    auto it = g_cacheSrcMemo.find(path);
    if (it != g_cacheSrcMemo.end() && it->second.size == sz &&
        CompareFileTime(&it->second.written, &fa.ftLastWriteTime) == 0) {
      size = it->second.size;
      hash = it->second.hash;
      return true;
    }
  }
  std::string text;
  if (!ReadWholeFile(path, text)) return false;
  size = text.size();
  hash = Fnv64(text.data(), text.size());
  std::lock_guard<std::mutex> lock(g_cacheMutex);
  g_cacheSrcMemo[path] = {size, hash, fa.ftLastWriteTime};
  return true;
}

inline std::wstring SourcePath(const std::string& rel) {
  std::wstring p = g_root + Widen(rel);
  for (wchar_t& c : p)
    if (c == L'/') c = L'\\';
  return p;
}

// Loads the snapshot's cached result into out. nullptr on a hit; else the
// reject reason ("missing" when there is no file; detail: the first source
// that changed).
const char* CacheLoad(const Snapshot& s, KeyResult& out, std::string& detail) {
  std::wstring path;
  std::string id, env;
  {
    std::lock_guard<std::mutex> lock(g_cacheMutex);
    if (g_cacheDir.empty()) return "missing";
    path = CachePathLocked(s);
    CacheWriter w;
    WriteIdentity(w, s);
    id = std::move(w.b);
    CacheWriter e;
    WriteEnvLocked(e);
    env = std::move(e.b);
  }
  std::string file;
  if (!ReadWholeFile(path, file)) return GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES ? "missing" : "unreadable";
  const auto* b = reinterpret_cast<const uint8_t*>(file.data());
  uint32_t format = 0, edit = 0;
  uint64_t size = 0, sum = 0;
  if (file.size() < kCacheHeader || memcmp(b, kCacheMagic, 8) != 0) return "bad magic";
  memcpy(&format, b + 8, 4);
  memcpy(&edit, b + 12, 4);
  memcpy(&size, b + 16, 8);
  memcpy(&sum, b + 24, 8);
  if (format != kCacheFormat) return "format version";
  {
    std::lock_guard<std::mutex> lock(g_cacheMutex);
    if (edit != g_cacheEditVersion) return "edit version";
  }
  if (size != file.size() - kCacheHeader) return "truncated";
  if (Fnv64(b + kCacheHeader, static_cast<size_t>(size)) != sum) return "checksum";
  const uint8_t* p = b + kCacheHeader;
  if (size < id.size() || memcmp(p, id.data(), id.size()) != 0) return "key mismatch";
  if (size - id.size() < env.size() || memcmp(p + id.size(), env.data(), env.size()) != 0)
    return "compiler or flags changed";
  const size_t head = id.size() + env.size();
  CacheReader rd(p + head, static_cast<size_t>(size - head));
  const uint32_t files = rd.Count(4096);
  for (uint32_t i = 0; i < files && rd.ok; ++i) {
    const std::string rel = rd.Str();
    const uint64_t fsize = rd.U64(), fhash = rd.U64();
    uint64_t nsize = 0, nhash = 0;
    if (rd.ok && (!SourceHash(SourcePath(rel), nsize, nhash) || nsize != fsize || nhash != fhash)) {
      detail = rel;
      return "source changed";
    }
  }
  const uint32_t missing = rd.Count(4096);
  for (uint32_t i = 0; i < missing && rd.ok; ++i) {
    const std::string rel = rd.Str();
    if (rd.ok && GetFileAttributesW(SourcePath(rel).c_str()) != INVALID_FILE_ATTRIBUTES) {
      detail = rel + " appeared";
      return "source changed";
    }
  }
  if (!rd.ok) return "payload malformed";
  if (files == 0) return "no sources recorded";
  KeyResult r;
  if (const char* why = ReadResult(rd, r)) return why;
  if (r.key != s.key || r.kind != KindOf(s)) return "key mismatch";
  out.key = std::move(r.key);
  out.path = std::move(r.path);
  out.why = std::move(r.why);
  out.kind = r.kind;
  out.ok = r.ok;
  out.defines = r.defines;
  out.vs = std::move(r.vs);
  out.techs = std::move(r.techs);
  out.reads = std::move(r.reads);
  out.fxVars = std::move(r.fxVars);
  out.msA = out.msB = 0;
  out.cached = true;
  prune::TouchOnHit(path);  // R90: last use for the cache's LRU cap (disk_prune.h)
  return nullptr;
}

// Whether a finished compile is worth storing: the compiler ran, every input
// was recorded, the workers were not stopping, and the failure (if any) is a
// property of the inputs (not e.g. an out-of-memory HRESULT).
inline bool Cacheable(const KeyResult& r, const SourceLog& log) {
  return !g_stop.load() && log.compiles > 0 && !log.unreliable && !log.files.empty() &&
         r.why.find("compile failed: hr ") == std::string::npos;
}

// Writes the result atomically (temp file, then MoveFileEx). Worker thread.
void CacheSave(const Snapshot& s, const KeyResult& r, const SourceLog& log) {
  CacheWriter w;
  std::wstring path;
  uint32_t edit;
  {
    std::lock_guard<std::mutex> lock(g_cacheMutex);
    if (g_cacheDir.empty() || !g_cacheWritable) return;
    path = CachePathLocked(s);
    WriteIdentity(w, s);
    WriteEnvLocked(w);
    edit = g_cacheEditVersion;
  }
  w.U32(static_cast<uint32_t>(log.files.size()));
  for (const SourceFile& f : log.files) {
    w.Str(f.rel);
    w.U64(f.size);
    w.U64(f.hash);
  }
  w.Strs(log.missing);
  WriteResult(w, r);
  std::string file(kCacheMagic, 8);
  const uint64_t size = w.b.size(), sum = Fnv64(w.b.data(), w.b.size());
  file.append(reinterpret_cast<const char*>(&kCacheFormat), 4);
  file.append(reinterpret_cast<const char*>(&edit), 4);
  file.append(reinterpret_cast<const char*>(&size), 8);
  file.append(reinterpret_cast<const char*>(&sum), 8);
  file += w.b;
  const std::wstring tmp = path + L"." + std::to_wstring(GetCurrentThreadId()) + L".tmp";
  bool ok = false, tempOk = false;
  HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  DWORD err = GetLastError();
  if (h != INVALID_HANDLE_VALUE) {
    DWORD put = 0;
    ok = WriteFile(h, file.data(), static_cast<DWORD>(file.size()), &put, nullptr) && put == file.size();
    if (!ok) err = GetLastError();
    tempOk = ok;
    CloseHandle(h);
    bool clearedRo = false;
    // A scanner or indexer briefly holding the target (or the fresh temp
    // file) makes the rename fail with a sharing/lock/access error: retried
    // (compile worker thread, never the render thread). Before, one such hit
    // turned cache writes off for the whole session.
    for (int attempt = 0; ok; ++attempt) {
      if (MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) break;
      err = GetLastError();
      // R63: a read-only target (Saved Games restored from a backup or a sync tool) is not transient: its
      // attribute is cleared once, then the rename retried. Before, it failed every retry and turned saving off.
      if (err == ERROR_ACCESS_DENIED && !clearedRo) {
        clearedRo = true;
        const DWORD a = GetFileAttributesW(path.c_str());
        if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_READONLY) &&
            SetFileAttributesW(path.c_str(), a & ~FILE_ATTRIBUTE_READONLY))
          continue;
      }
      const bool transient =
          err == ERROR_SHARING_VIOLATION || err == ERROR_ACCESS_DENIED || err == ERROR_LOCK_VIOLATION;
      if (!transient || attempt >= 9) {
        ok = false;
        break;
      }
      Sleep(10);
    }
    if (!ok) DeleteFileW(tmp.c_str());
  }
  if (ok) return;
  bool first;
  {
    std::lock_guard<std::mutex> lock(g_cacheMutex);
    first = g_cacheWriteFails++ == 0;
    // R63: only a temp file that cannot be written means the folder is (read-only folder, full disk): stop
    // trying, keep reading. One target that cannot be replaced (a deny ACL) costs that entry only.
    if (!tempOk) g_cacheWritable = false;
  }
  if (first && !tempOk)
    Log("shadow inst: cache: cannot write %ls (error %lu); continuing without saving", path.c_str(),
        static_cast<unsigned long>(err));
  else if (first)
    Log("shadow inst: cache: cannot replace %ls (error %lu); that entry is not saved, other entries still are",
        path.c_str(), static_cast<unsigned long>(err));
}

// Worker: the snapshot's key from the disk cache, else compiled (at idle
// priority) and stored.
void CompileKeyCached(const Snapshot& s, KeyResult& r) {
  bool useCache;
  {
    std::lock_guard<std::mutex> lock(g_cacheMutex);
    useCache = !g_cacheDir.empty();
  }
  if (useCache) {
    std::string detail;
    const char* why = CacheLoad(s, r, detail);
    if (!why) {
      std::lock_guard<std::mutex> lock(g_cacheMutex);
      ++g_cacheHits;
      return;
    }
    if (strcmp(why, "missing") != 0) {
      std::lock_guard<std::mutex> lock(g_cacheMutex);
      ++g_cacheRejected;
      auto& e = g_cacheRejectWhy[why];
      if (e.first++ == 0) e.second = detail;
    }
  }
  SourceLog log;
  t_sourceLog = &log;
  const int prio = GetThreadPriority(GetCurrentThread());
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_IDLE);  // first start: leave the cores to DCS
  CompileKey(s, r);
  SetThreadPriority(GetCurrentThread(), prio == THREAD_PRIORITY_ERROR_RETURN ? THREAD_PRIORITY_NORMAL : prio);
  t_sourceLog = nullptr;
  if (!useCache) return;
  {
    std::lock_guard<std::mutex> lock(g_cacheMutex);
    ++g_cacheCompiled;
  }
  if (Cacheable(r, log)) CacheSave(s, r, log);
}

// "shadow inst: cache: N hits, M compiled, K rejected (reason n: detail, ...)";
// M counts the rejected keys too. Empty when the cache is off.
std::string CacheSummary() {
  std::lock_guard<std::mutex> lock(g_cacheMutex);
  if (g_cacheDir.empty()) return std::string();
  char b[160];
  snprintf(b, sizeof(b), "shadow inst: cache: %u hits, %u compiled, %u rejected", g_cacheHits, g_cacheCompiled,
           g_cacheRejected);
  std::string s = b;
  std::string why;
  for (auto& kv : g_cacheRejectWhy)
    why += (why.empty() ? "" : ", ") + kv.first + " " + std::to_string(kv.second.first) +
           (kv.second.second.empty() ? "" : ": " + kv.second.second);
  if (!why.empty()) s += " (" + why + ")";
  if (g_cacheWriteFails) s += "; " + std::to_string(g_cacheWriteFails) + " not written (folder not writable)";
  return s;
}

// Logs the summary when the workers went idle and something changed since
// the last one (once per start, plus once per later batch of new keys).
void LogCacheSummaryIfChanged() {
  {
    std::lock_guard<std::mutex> lock(g_cacheMutex);
    const uint32_t n = g_cacheHits + g_cacheCompiled;
    if (g_cacheDir.empty() || n == g_cacheLoggedAt) return;
    g_cacheLoggedAt = n;
  }
  const std::string s = CacheSummary();
  if (!s.empty()) Log("%s", s.c_str());
}

// Publishes one snapshot of a finished key (caller holds g_mutex). With a
// single-threaded device the render thread creates and publishes instead.
void FinishLocked(Snapshot* s, KeyResult* r) {
  PublishTexLocked(*s, *r);
  if (r->ok && g_device && g_deviceSingleThreaded) {
    g_pendingCreate.push_back({s, r});
    g_pendingCount = static_cast<uint32_t>(g_pendingCreate.size());
    return;
  }
  PublishLocked(*s, *r);
  delete s;
}

DWORD WINAPI Worker(void*) {
  // Below the game's threads; CompileKeyCached drops to idle while compiling,
  // so compiles only fill the idle cores.
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
  for (;;) {
    Snapshot* s = nullptr;
    KeyResult* r = nullptr;
    {
      std::unique_lock<std::mutex> lock(g_mutex);
      for (;;) {  // R90 H2: the render thread's pushes may arrive without a notify (its try_lock failed)
        DrainInboxLocked();
        if (g_stop.load() || !g_queue.empty()) break;
        g_cv.wait_for(lock, std::chrono::milliseconds(100));
      }
      if (g_stop.load()) return 0;
      s = g_queue.front();
      g_queue.erase(g_queue.begin());
      const std::string rk = ResultKey(s->kind, s->key);
      auto it = g_results.find(rk);
      if (it != g_results.end()) {
        // Same key through another DX11Shader: publish now, or when the
        // worker compiling it finishes.
        if (it->second->done)
          FinishLocked(s, it->second);
        else
          it->second->waiters.push_back(s);
        g_cv.notify_all();
        continue;
      }
      r = new KeyResult;
      r->key = s->key;
      r->kind = s->kind;
      g_results[rk] = r;
      ++g_busy;
      for (Snapshot*& c : g_compiling)
        if (!c) {
          c = s;  // ForgetShader can mark it dead while it compiles
          break;
        }
    }
    CompileKeyCached(*s, *r);
    ID3D11Device* dev = nullptr;
    {
      std::lock_guard<std::mutex> lock(g_mutex);
      for (Snapshot*& c : g_compiling)
        if (c == s) c = nullptr;
      if (!g_deviceSingleThreaded && g_device) {
        dev = g_device;
        dev->AddRef();
      }
    }
    // R90 H2: outside g_mutex (the driver may take milliseconds per VS; the render thread's ForgetShader and
    // the single-threaded drain wait on g_mutex). r is not done yet: no other thread reads its VS objects.
    if (dev) {
      CreateVsOn(dev, *r);
      dev->Release();
    }
    const uint32_t idx = g_logIndex.fetch_add(1) + 1;
    if (!g_stop.load()) LogResult(*r, idx);
    bool idle;
    {
      std::lock_guard<std::mutex> lock(g_mutex);
      r->done = true;
      (r->kind == kKindGb ? g_keysDoneGb : g_keysDone).fetch_add(1);
      if (r->ok) (r->kind == kKindGb ? g_keysOkGb : g_keysOk).fetch_add(1);
      if (g_stop.load()) {
        delete s;  // Shutdown frees the waiters with the result
        --g_busy;
        return 0;
      }
      FinishLocked(s, r);
      for (Snapshot* w : r->waiters) FinishLocked(w, r);
      r->waiters.clear();
      --g_busy;
      g_cv.notify_all();
      idle = g_queue.empty() && g_busy == 0;
    }
    if (idle) LogCacheSummaryIfChanged();
  }
}

// Render thread: first sight of a shader -> snapshot -> queue.
void ObserveBody(uint8_t* mat, int kind) {
  uint8_t* sh = *reinterpret_cast<uint8_t**>(mat + 0x30);
  if (!sh || *reinterpret_cast<void**>(sh) != g_shaderVtbl) return;
  if (!MarkSeen(sh, kind)) return;
  (kind == kKindGb ? g_seenGb : g_seenShaders).fetch_add(1, std::memory_order_relaxed);
  Snapshot* s = new Snapshot;
  memset(s, 0, sizeof(*s));
  s->kind = kind;
  s->why = SnapshotGuarded(mat, sh, *s);
  if (s->why) {
    (kind == kKindGb ? g_rejectedGb : g_rejectedSnapshots).fetch_add(1);
    std::unique_lock<std::mutex> lock(g_mutex, std::try_to_lock);  // R90 H2: a sample line only; never wait
    if (lock.owns_lock() && g_rejectLog.size() < 16) {
      char b[400];
      snprintf(b, sizeof(b), "%sshader %p (%s): %s", kind == kKindGb ? "g-buffer " : "", sh,
               s->path[0] ? s->path : "?", s->why);
      g_rejectLog.push_back(b);
    }
    delete s;
    return;
  }
  CheckDevice(*s);
  InboxPush(s);  // R90 H2
}

// DCS's caster loop calls this: a std::bad_alloc (the 27 KB snapshot, the
// queue) must not unwind into DCS's frames. The shader is then not compiled
// this session (it is marked seen), as for any rejected snapshot.
void Observe(uint8_t* mat, int kind = kKindShadow) {
  try {
    ObserveBody(mat, kind);
  } catch (...) {
  }
}

// Single-threaded device: create the VS objects here, a few per call.
void DrainCreates() {
  std::unique_lock<std::mutex> lock(g_mutex, std::try_to_lock);
  if (!lock.owns_lock()) return;
  for (int n = 0; n < 4 && !g_pendingCreate.empty(); ++n) {
    auto [s, r] = g_pendingCreate.back();
    g_pendingCreate.pop_back();
    PublishLocked(*s, *r);
    delete s;
  }
  g_pendingCount = static_cast<uint32_t>(g_pendingCreate.size());
  if (g_pendingCreate.empty()) g_cv.notify_all();
}

void Observer(void* self, void*) {
  g_inObserver.fetch_add(1, std::memory_order_acquire);
  if (!g_stop.load(std::memory_order_relaxed)) {
    if (g_collect.load(std::memory_order_relaxed)) {
      uint8_t* item = *reinterpret_cast<uint8_t**>(static_cast<uint8_t*>(self) + 0x10);
      uint8_t* mat = item ? *reinterpret_cast<uint8_t**>(item + 0x10) : nullptr;
      if (mat && *reinterpret_cast<void**>(mat) == g_modelVtbl) Observe(mat);
    }
    if (g_deviceSingleThreaded && g_pendingCount.load(std::memory_order_relaxed)) DrainCreates();
  }
  g_inObserver.fetch_sub(1, std::memory_order_release);
}

// ---------------------------------------------------------------------------
// G-buffer observer (R13): NGModel SceneRenderable (vtable 0x59810) vt[1]
// 0x44350, hooked once by gb_count.h, which calls this before the original.
// 0x44350 calls mat->vt[4] = ModelMaterialMT::draw 0x16140(mat, item, &vec,
// pass = [r+0x60], mesh [item+0xc0], mesh2 [item+0xc8], &ctx = r+0x64,
// &size = r+0x6c) [V]. The technique and pass index it then selects are
// mirrored below for the log (and for stage 2's planner) [V, hash-checked]:
//  - pass 4..13: jump table 0x1647c. 4 normal_ir P0, 6 normal_sat P0,
//    7 normal_map P0, 8 flat_shadow (flat_shadow_transparent when
//    props+0x33) with mesh2, pass index = 1.0f > [[item+0x50]+0x18] (FLIR),
//    10 radar (every pass, if mat+0x220 != -1), 11 impostor P0, 13
//    cockpit_cubemap P0 if opaque, 9 and 12 nothing; 5 goes to 0x15e20.
//  - other passes: 0x15e20. Opaque pass 1 = P0 (deferred_ps_c, the G-buffer);
//    otherwise ecx = ctx[1] ? e|1 : e with e = (ctx[0] && ctx[4]&1) ? 2 : 0,
//    and with props+0x12d: ecx = (pass == 3), or -1 outside the cockpit.
//    Cockpit (ctx[0]): normal_cockpit P(ecx+1); else normal P0/P1/P2 for
//    ecx -1/0/1.
// Technique slot i = handle at mat+0x1d8 + 8i (names: NGModel .data 0x70640).
// ---------------------------------------------------------------------------

constexpr uint32_t kSceneRenderable = 0x44350, kSceneRenderableEnd = 0x443dc;
constexpr uint64_t kSceneRenderableHash = 0x48801b542c9549beull;
constexpr uint32_t kModelDraw = 0x16140;  // ModelMaterialMT vt[4]
constexpr uint32_t kSelCode = 0x15fd1, kSelCodeEnd = 0x16134;  // 0x15e20's selection + its jump table
constexpr uint64_t kSelCodeHash = 0x1675bab468ed1984ull;
constexpr uint32_t kDrawDispatch = 0x161e5, kDrawDispatchEnd = 0x16301;  // 0x16140's switch, passes 4, 6, 7, 8
constexpr uint64_t kDrawDispatchHash = 0xeb2115bc10a89889ull;
constexpr uint32_t kDrawTable = 0x1647c, kDrawTableEnd = 0x164a4;
constexpr uint64_t kDrawTableHash = 0x24760084557e75b3ull;
constexpr uint32_t kFlirThreshold = 0x57df8;  // float (1.0)
constexpr int kSelSlots = 14;                 // technique slots 0..12, 13 = no draw
constexpr int kSelPassIdx = 17;               // pass index 0..15, 16 = every pass (radar)
constexpr int kSelModelPasses = 16;
constexpr const char* kTechSlotName[kSelSlots] = {"normal",
                                                  "normal_cockpit",
                                                  "normal_map",
                                                  "normal_sat",
                                                  "normal_ir",
                                                  "flat_shadow",
                                                  "flat_shadow_transparent",
                                                  "lockon_shadows",
                                                  "lockon_shadows_transparent",
                                                  "radar",
                                                  "impostor",
                                                  "cockpit_glass_uv",
                                                  "cockpit_cubemap",
                                                  "(no draw)"};

struct Selection {
  int slot, pass;
};
// Pure mirror of the selection (see above).
inline Selection SelectTechPass(uint32_t pass, uint8_t transparent, uint8_t p12d, uint8_t c0, uint8_t c1, uint8_t c4,
                                float flir, float threshold, bool radarValid) {
  switch (pass) {
    case 4: return {4, 0};
    case 6: return {3, 0};
    case 7: return {2, 0};
    case 8: return {transparent ? 6 : 5, threshold > flir ? 1 : 0};
    case 9: case 12: return {13, 0};
    case 10: return radarValid ? Selection{9, 16} : Selection{13, 0};
    case 11: return {10, 0};
    case 13: return transparent ? Selection{13, 0} : Selection{12, 0};
    default: break;
  }
  int ecx = -1;
  if (!(pass == 1 && !transparent)) {
    const int e = c0 && (c4 & 1) ? 2 : 0;
    ecx = c1 ? (e | 1) : e;
    if (p12d) {
      ecx = pass == 3 ? 1 : 0;
      if (!c0) ecx = -1;
    }
  }
  if (c0) {
    const unsigned idx = static_cast<unsigned>(ecx + 1);
    return {1, idx <= 8 ? static_cast<int>(idx) : 0};
  }
  return {0, ecx == 0 ? 1 : ecx == 1 ? 2 : 0};
}

std::atomic<uint32_t> g_sel[kSelModelPasses][kSelSlots][kSelPassIdx];
std::atomic<uint64_t> g_selOther{0};
bool g_selOk = false;  // the mirrored code matched its hashes
float g_flirThreshold = 1.0f;

Selection SelectionOf(uint8_t* r, uint8_t* item, uint8_t* mat) {
  const uint32_t pass = *reinterpret_cast<uint32_t*>(r + 0x60);
  const uint8_t* props = *reinterpret_cast<uint8_t**>(mat + 0x28);
  float flir = 0;
  if (pass == 8) {
    const uint8_t* q = *reinterpret_cast<uint8_t**>(item + 0x50);
    flir = q ? *reinterpret_cast<const float*>(q + 0x18) : 0.0f;
  }
  return SelectTechPass(pass, props[0x33], props[0x12d], r[0x64], r[0x65], r[0x68], flir, g_flirThreshold,
                        *reinterpret_cast<int64_t*>(mat + 0x220) != -1);
}

void NoteSelectionGuarded(uint8_t* r, uint8_t* item, uint8_t* mat) {
  __try {
    const uint32_t pass = *reinterpret_cast<uint32_t*>(r + 0x60);
    const Selection sel = SelectionOf(r, item, mat);
    if (pass < kSelModelPasses)
      g_sel[pass][sel.slot][sel.pass].fetch_add(1, std::memory_order_relaxed);
    else
      g_selOther.fetch_add(1, std::memory_order_relaxed);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }
}

// ---------------------------------------------------------------------------
// R14 lead 3 (G1) counter, measurement only ([Suite] GBufferTexCount).
// Per G-buffer ModelMaterialMT draw (model pass 1, opaque, outside the
// cockpit: technique [mat+0x1d8] P0, the deferred normal* pass [V 0x15e20];
// cross-checked with the selection mirror when it is verified): the
// material's bound texture handles, mat+0x240+8i for i < [mat+0x2d8] with
// handle != -1 (0xcf80 sets each through DX11Shader slot 26 on every slot-4
// call), and how many of them name an effect variable that no stage of that
// technique pass reads in our (a) compile of the shader's key
// (GbPassReadTextures). Live parameter records are matched by name exactly as
// shadow_tex.h's masks (MapRecords: every live record name must be a variable
// of (a); a record is read when a binding NameRefers to it). A draw whose key
// is not compiled/published yet, or whose records do not map, counts its sets
// as unknown. Runs in GbObserver, before the original draw (it reads the
// material and the shader's records only, which that draw does not change).
// One thread counts (the first that calls; the G-buffer items run on the
// render thread); items seen on other threads are only tallied. No DCS state
// is written.
// ---------------------------------------------------------------------------

constexpr uint32_t kTexCountMaxHandles = 64;

struct TexCountMask {
  shadowtex::MaskEntry e;          // read bit per live parameter record; state 1 usable, -1 unknown
  std::vector<std::string> names;  // live record names (copied), for the per-variable counts
  std::string why;                 // state -1: why
};
struct TexCountTotals {
  uint64_t draws = 0, sets = 0, unread = 0, unknownSets = 0, unknownDraws = 0;
  uint64_t cockpit = 0, selMismatch = 0;
};
std::atomic<bool> g_texCountOn{false};
std::atomic<int> g_inTexCount{0};
std::atomic<DWORD> g_texCountThread{0};
std::atomic<uint64_t> g_texCountOtherThread{0};
// Counting thread only while g_texCountOn; the suite phase reads and clears
// them after g_inTexCount dropped to 0.
TexCountTotals g_tc;
std::unordered_map<const MapEntry*, TexCountMask*> g_tcMasks;
std::vector<TexCountMask*> g_tcOldMasks;                            // replaced (records moved); freed with the rest
std::unordered_map<const std::string*, uint64_t> g_tcUnreadByName;  // key: an element of some TexCountMask::names
std::unordered_map<const char*, uint64_t> g_tcUnknownWhy;          // reason (literal or TexCountMask::why) -> sets

struct TexCountDraw {
  uint8_t* shader;
  uint64_t tech;
  uint32_t n, extra;  // handles != -1 kept in h; extra: those past kTexCountMaxHandles
  bool dx;
  int64_t h[kTexCountMaxHandles];
};

// 0: not a G-buffer ModelMaterialMT draw; 1: counted; 2: cockpit; 3: the
// selection mirror disagrees. Plain C: runs under __try.
int ReadTexCountDrawRaw(uint8_t* r, TexCountDraw& d) {
  if (*reinterpret_cast<uint32_t*>(r + 0x60) != 1) return 0;
  uint8_t* item = *reinterpret_cast<uint8_t**>(r + 0x10);
  uint8_t* mat = item ? *reinterpret_cast<uint8_t**>(item + 0x10) : nullptr;
  if (!mat || *reinterpret_cast<void**>(mat) != g_modelVtbl) return 0;
  const uint8_t* props = *reinterpret_cast<uint8_t**>(mat + 0x28);
  if (props[0x33]) return 0;  // transparent: forward passes of normal*
  if (r[0x64]) return 2;      // cockpit: normal_cockpit*
  if (g_selOk) {
    const Selection s = SelectionOf(r, item, mat);
    if (s.slot != 0 || s.pass != 0) return 3;
  }
  d.shader = *reinterpret_cast<uint8_t**>(mat + 0x30);
  d.dx = d.shader && *reinterpret_cast<void**>(d.shader) == g_shaderVtbl;
  d.tech = *reinterpret_cast<uint64_t*>(mat + 0x1d8);
  d.n = d.extra = 0;
  const uint32_t n = *reinterpret_cast<uint32_t*>(mat + 0x2d8);
  for (uint32_t i = 0; i < n; ++i) {
    const int64_t h = *reinterpret_cast<int64_t*>(mat + 0x240 + 8 * static_cast<uint64_t>(i));
    if (h == -1) continue;
    if (d.n < kTexCountMaxHandles)
      d.h[d.n++] = h;
    else
      ++d.extra;
  }
  return 1;
}

int ReadTexCountDrawGuarded(uint8_t* r, TexCountDraw& d) {
  __try {
    return ReadTexCountDrawRaw(r, d);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return 0;
  }
}

const char* MapTexCountGuarded(shadowtex::MaskEntry& e, const char* const* bound, int nBound, const char* const* vars,
                               int nVars, uint32_t* bad) {
  __try {
    return shadowtex::MapRecords(e, bound, nBound, vars, nVars, bad);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return "access violation while reading the parameter records";
  }
}

// Name of live parameter record i (0x50 bytes, name at +0x30), "" if none.
bool CopyRecordNameGuarded(const uint8_t* recBegin, uint32_t i, char* out, size_t cap) {
  __try {
    const char* n = *reinterpret_cast<const char* const*>(recBegin + static_cast<uint64_t>(i) * 0x50 + 0x30);
    size_t k = 0;
    if (n)
      for (; k + 1 < cap && n[k]; ++k) out[k] = n[k];
    out[k] = 0;
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    out[0] = 0;
    return false;
  }
}

// The read mask of (shader, normal* P0) for this map entry: cached, else
// built from GbPassReadTextures. nullptr while the key's read set is pending.
TexCountMask* TexCountMaskFor(const MapEntry* me, uint8_t* shader, uint64_t tech) {
  void* recBegin = *reinterpret_cast<void**>(shader + 0xc8);
  void* recEnd = *reinterpret_cast<void**>(shader + 0xd0);
  auto it = g_tcMasks.find(me);
  if (it != g_tcMasks.end() && it->second->e.recBegin == recBegin && it->second->e.recEnd == recEnd)
    return it->second;
  std::vector<std::string> bound, vars;
  std::string why;
  const int st = GbPassReadTextures(shader, tech, 0, &bound, &vars, &why, nullptr);
  if (st == shadowtex::kReadsPending) return nullptr;
  auto* m = new TexCountMask;
  shadowtex::MaskEntry& e = m->e;
  e.effect = *reinterpret_cast<void**>(shader + 0x50);
  e.recBegin = recBegin;
  e.recEnd = recEnd;
  e.techBegin = *reinterpret_cast<void**>(shader + 0xb0);
  e.techA = e.techB = tech;
  const uint64_t span = static_cast<uint8_t*>(recEnd) - static_cast<uint8_t*>(recBegin);
  e.recCount = recBegin && recEnd > recBegin ? static_cast<uint32_t>(span / 0x50) : 0;
  if (st != shadowtex::kReadsReady) {
    m->why = why.empty() ? "gbuffer inst: read set not usable" : why;
  } else if (!recBegin || recEnd < recBegin || span % 0x50 != 0) {
    m->why = "parameter records at +0xc8 are not plausible";
  } else if (e.recCount > shadowtex::kMaxRecords) {
    m->why = "more than 512 parameter records";
  } else {
    std::vector<const char*> b, v;
    for (const std::string& n : bound) b.push_back(n.c_str());
    for (const std::string& n : vars) v.push_back(n.c_str());  // sorted: std::string order is strcmp order
    uint32_t bad = ~0u;
    if (const char* w = MapTexCountGuarded(e, b.data(), static_cast<int>(b.size()), v.data(),
                                           static_cast<int>(v.size()), &bad))
      m->why = w;
  }
  e.state = m->why.empty() ? 1 : -1;
  if (e.state > 0) {
    m->names.resize(e.recCount);
    char buf[256];
    for (uint32_t i = 0; i < e.recCount; ++i)
      if (CopyRecordNameGuarded(static_cast<const uint8_t*>(recBegin), i, buf, sizeof(buf))) m->names[i] = buf;
  }
  if (it != g_tcMasks.end()) {
    g_tcOldMasks.push_back(it->second);  // its names may be keys of g_tcUnreadByName
    it->second = m;
  } else {
    g_tcMasks[me] = m;
  }
  return m;
}

// One SceneRenderable item on the counting thread.
void TexCountNote(uint8_t* r) {
  TexCountDraw d;
  const int k = ReadTexCountDrawGuarded(r, d);
  if (k == 0) return;
  if (k == 2) {
    ++g_tc.cockpit;
    return;
  }
  if (k == 3) {
    ++g_tc.selMismatch;
    return;
  }
  ++g_tc.draws;
  const uint64_t sets = static_cast<uint64_t>(d.n) + d.extra;
  g_tc.sets += sets;
  if (!sets) return;
  const char* unknown = nullptr;
  TexCountMask* m = nullptr;
  if (!d.dx) {
    unknown = "not a DX11Shader";
  } else if (d.extra) {
    unknown = "more than 64 texture handles";
  } else {
    const MapEntry* me = FindEntry(d.shader, d.tech, 0);
    m = me ? TexCountMaskFor(me, d.shader, d.tech) : nullptr;
    if (!m)
      unknown = "key not compiled yet or failed (no published normal* P0 entry)";
    else if (m->e.state <= 0)
      unknown = m->why.c_str();
  }
  if (unknown) {
    g_tc.unknownSets += sets;
    ++g_tc.unknownDraws;
    g_tcUnknownWhy[unknown] += sets;
    return;
  }
  bool anyUnknown = false;
  for (uint32_t i = 0; i < d.n; ++i) {
    const int64_t h = d.h[i];
    if (h < 0 || static_cast<uint64_t>(h) >= m->e.recCount) {
      ++g_tc.unknownSets;
      ++g_tcUnknownWhy["handle outside the parameter records"];
      anyUnknown = true;
      continue;
    }
    if (shadowtex::Skippable(m->e, h)) {
      ++g_tc.unread;
      ++g_tcUnreadByName[&m->names[static_cast<size_t>(h)]];
    }
  }
  g_tc.unknownDraws += anyUnknown;
}

void TexCountObserve(uint8_t* r) {
  g_inTexCount.fetch_add(1);  // seq_cst: the phase clears g_texCountOn, then waits for 0
  if (g_texCountOn.load()) {
    DWORD expected = 0;
    const DWORD me = GetCurrentThreadId();
    if (g_texCountThread.compare_exchange_strong(expected, me) || expected == me)
      TexCountNote(r);
    else
      g_texCountOtherThread.fetch_add(1, std::memory_order_relaxed);
  }
  g_inTexCount.fetch_sub(1);
}

void TexCountClear() {
  for (auto& kv : g_tcMasks) delete kv.second;
  for (TexCountMask* m : g_tcOldMasks) delete m;
  g_tcMasks.clear();
  g_tcOldMasks.clear();
  g_tcUnreadByName.clear();
  g_tcUnknownWhy.clear();
  g_tc = TexCountTotals();
  g_texCountThread = 0;
  g_texCountOtherThread = 0;
}

// Model passes whose shaders are G-buffer keys: 1 (G-buffer, opaque P0) and
// 2; both reach normal* / normal_cockpit* through 0x15e20.
void GbObserver(void* self, void*) {
  g_inObserver.fetch_add(1, std::memory_order_acquire);
  if (!g_stop.load(std::memory_order_relaxed)) {
    if (g_texCountOn.load(std::memory_order_relaxed)) TexCountObserve(static_cast<uint8_t*>(self));
    if (g_collectGb.load(std::memory_order_relaxed)) {
      auto* r = static_cast<uint8_t*>(self);
      uint8_t* item = *reinterpret_cast<uint8_t**>(r + 0x10);
      uint8_t* mat = item ? *reinterpret_cast<uint8_t**>(item + 0x10) : nullptr;
      if (mat && *reinterpret_cast<void**>(mat) == g_modelVtbl) {
        if (g_selOk) NoteSelectionGuarded(r, item, mat);
        const uint32_t pass = *reinterpret_cast<uint32_t*>(r + 0x60);
        if (pass == 1 || pass == 2) Observe(mat, kKindGb);
      }
    }
    if (g_deviceSingleThreaded && g_pendingCount.load(std::memory_order_relaxed)) DrainCreates();
  }
  g_inObserver.fetch_sub(1, std::memory_order_release);
}

// Shaders root from the NGModel.dll location: <DCS>\bin\NGModel.dll ->
// <DCS>\Bazar\shaders\ .
std::wstring FindShaderRoot(HMODULE ng) {
  wchar_t p[MAX_PATH];
  const DWORD n = GetModuleFileNameW(ng, p, MAX_PATH);
  if (!n || n >= MAX_PATH) return std::wstring();
  std::wstring s(p);
  for (int i = 0; i < 2; ++i) {
    const size_t slash = s.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return std::wstring();
    s.resize(slash);
  }
  s += L"\\Bazar\\shaders\\";
  const DWORD a = GetFileAttributesW((s + L"model\\common\\uniforms.hlsl").c_str());
  return a == INVALID_FILE_ATTRIBUTES ? std::wstring() : s;
}

// Returns nullptr when the binaries are the analysed build (or another build
// with the same code, see reloc.h). Sets g_at.
const char* VerifyBuild(uint8_t* ng, uint8_t* dx) {
  Addrs& a = g_at;
  a.model = reloc::Rva(hooksig::NG_ModelMaterialMT_vtbl, kModelMatVtbl);
  a.shader = reloc::Rva(hooksig::DX_DX11Shader_vtbl, kShaderVtbl);
  a.effect = reloc::Rva(hooksig::DX_CEffect_vtbl, kEffectVtbl);
  a.effectGetDevice = reloc::Rva(hooksig::DX_CEffect_GetDevice, kEffectGetDevice);
  a.shaderVar = reloc::Rva(hooksig::DX_SShaderGlobalVariable_vtbl, kShaderVarVtbl);
  a.getVertexShader = reloc::Rva(hooksig::DX_SShaderGlobalVariable_GetVertexShader, kGetVertexShader);
  a.tech = reloc::Rva(hooksig::DX_STechnique_vtbl, kTechVtbl);
  a.pass = reloc::Rva(hooksig::DX_SPassBlock_vtbl, kPassVtbl);
  if (!a.model || !allocslab::RttiIs(ng, reinterpret_cast<void**>(ng + a.model), ".?AVModelMaterialMT@model@@"))
    return "shadow inst: NGModel ModelMaterialMT does not match this build; skipped";
  if (!a.shader || !a.effect || !a.effectGetDevice || !a.shaderVar || !a.getVertexShader || !a.tech || !a.pass)
    return "shadow inst: dx11backend DX11Shader/effects code does not match this build; skipped";
  auto** effectVt = reinterpret_cast<void**>(dx + a.effect);
  auto** varVt = reinterpret_cast<void**>(dx + a.shaderVar);
  if (!allocslab::RttiIs(dx, reinterpret_cast<void**>(dx + a.shader), ".?AVDX11Shader@RenderAPI@@") ||
      !allocslab::RttiIs(dx, effectVt, ".?AVCEffect@D3DX11Effects@@") ||
      !allocslab::RttiIs(dx, varVt, ".?AUSShaderGlobalVariable@D3DX11Effects@@") ||
      !allocslab::RttiIs(dx, reinterpret_cast<void**>(dx + a.tech), ".?AUSTechnique@D3DX11Effects@@") ||
      !allocslab::RttiIs(dx, reinterpret_cast<void**>(dx + a.pass), ".?AUSPassBlock@D3DX11Effects@@") ||
      reinterpret_cast<uint8_t*>(effectVt[4]) != dx + a.effectGetDevice ||
      reinterpret_cast<uint8_t*>(varVt[32]) != dx + a.getVertexShader ||
      !reloc::CodeIs(hooksig::DX_CEffect_GetDevice, dx, kEffectGetDevice, kEffectGetDeviceEnd, kEffectGetDeviceHash) ||
      !reloc::CodeIs(hooksig::DX_SShaderGlobalVariable_GetVertexShader, dx, kGetVertexShader, kGetVertexShaderEnd, kGetVertexShaderHash) ||
      !reloc::CodeIs(hooksig::DX_DX11Shader_ctor_fieldStores, dx, kCtorFields, kCtorFieldsEnd, kCtorFieldsHash) ||
      !reloc::CodeIs(hooksig::DX_DX11Shader_keyBuilder, dx, kKeyBuilder, kKeyBuilderEnd, kKeyBuilderHash) ||
      !reloc::CodeIs(hooksig::DX_DX11Shader_macroBuilder, dx, kMacroBuilder, kMacroBuilderEnd, kMacroBuilderHash))
    return "shadow inst: dx11backend DX11Shader/effects code does not match this build; skipped";
  return nullptr;
}

bool Install() {
  // Claimed atomically (-3 while installing): the payload worker and a suite
  // phase can both call it, and two first calls each started compile workers
  // (only the last set's handles kept: Shutdown freed the results under the
  // others) and allocated the maps twice.
  int idle = 0;
  if (!g_state.compare_exchange_strong(idle, -3)) return g_state.load() > 0;
  auto* ng = reinterpret_cast<uint8_t*>(GetModuleHandleW(L"NGModel.dll"));
  auto* dx = reinterpret_cast<uint8_t*>(GetModuleHandleW(L"dx11backend.dll"));
  if (!ng || !dx) {
    g_state = 0;  // retried later
    return false;
  }
  g_state = -1;
  if (const char* why = VerifyBuild(ng, dx)) {
    Log("%s", why);
    return false;
  }
  g_root = FindShaderRoot(reinterpret_cast<HMODULE>(ng));
  if (g_root.empty()) {
    Log("shadow inst: Bazar\\shaders\\model\\common\\uniforms.hlsl not found next to bin\\; skipped");
    return false;
  }
  char already[MAX_PATH] = "not loaded";
  if (HMODULE m = GetModuleHandleW(L"d3dcompiler_47.dll")) GetModuleFileNameA(m, already, MAX_PATH);
  if (!g_compiler.Load()) {
    Log("shadow inst: System32 d3dcompiler_47.dll (D3DCompile2/D3DReflect) not available; skipped");
    return false;
  }
  if (!instcount::Install()) {
    Log("shadow inst: shadow caster hook unavailable; skipped");
    return false;
  }
  g_ng = ng;
  g_dx = dx;
  g_shaderVtbl = dx + g_at.shader;
  g_modelVtbl = ng + g_at.model;
  g_map = new MapEntry[kMapSize];
  g_mapUsed = 0;
  g_mapOccupied = 0;
  g_mapCapLogged = false;
  g_texMapCapLogged = false;
  for (auto& u : g_mapUsedKind) u = 0;
  g_texMap = new TexEntry[kTexMapSize];
  g_texMapUsed = 0;
  g_stop = false;
  // Disk cache in the module's folder (tests configure their own first).
  bool configured;
  {
    std::lock_guard<std::mutex> lock(g_cacheMutex);
    configured = g_cacheConfigured;
  }
  const char* cacheWhy = configured ? nullptr : CacheInit(g_dir.empty() ? std::wstring() : g_dir + L"cache\\", g_compiler);
  std::wstring cacheDir;
  {
    std::lock_guard<std::mutex> lock(g_cacheMutex);
    cacheDir = g_cacheDir;
  }
  for (int i = 0; i < kWorkers; ++i) g_workers[i] = CreateThread(nullptr, 0, qvexit::Thread<Worker>, nullptr, 0, nullptr);
  instcount::g_observer.store(&Observer);
  g_state = 1;
  Log("shadow inst: ready (compiler %s; DCS process had %s; sources %ls; %d compile workers)",
      g_compiler.path.c_str(), already, g_root.c_str(), kWorkers);
  if (!cacheDir.empty())
    Log("shadow inst: cache: %ls (format %u, edit version %u)", cacheDir.c_str(), kCacheFormat, kCacheEditVersion);
  else
    Log("shadow inst: cache: off (%s); every key is compiled", cacheWhy ? cacheWhy : "not configured");
  return true;
}

// G-buffer part: the shared pipeline plus gb_count.h's SceneRenderable hook
// with our observer (one HookSlot for both). The selection mirror is enabled
// only when the mirrored NGModel code matches its hashes.
bool InstallGb() {
  if (g_gbInstalled.load()) return true;
  if (!Install()) return false;
  if (!gbcount::Install()) {
    Log("gbuffer inst: SceneRenderable hook unavailable; skipped");
    return false;
  }
  auto** matVt = reinterpret_cast<void**>(g_ng + g_at.model);
  g_selOk = reinterpret_cast<uint8_t*>(SlotOriginal(&matVt[4])) == g_ng + kModelDraw &&
            shadowtex::CodeIs(g_ng, kSceneRenderable, kSceneRenderableEnd, kSceneRenderableHash) &&
            shadowtex::CodeIs(g_ng, kSelCode, kSelCodeEnd, kSelCodeHash) &&
            shadowtex::CodeIs(g_ng, kDrawDispatch, kDrawDispatchEnd, kDrawDispatchHash) &&
            shadowtex::CodeIs(g_ng, kDrawTable, kDrawTableEnd, kDrawTableHash);
  if (g_selOk) memcpy(&g_flirThreshold, g_ng + kFlirThreshold, 4);
  if (!g_selOk) Log("gbuffer inst: ModelMaterialMT::draw selection code differs from the analysed build; no selection log");
  gbcount::g_observer.store(&GbObserver);
  g_gbInstalled = true;
  Log("gbuffer inst: ready (SceneRenderable observer on gb_count's hook; selection mirror %s)",
      g_selOk ? "verified" : "off");
  return true;
}

// Stage-1 action of [Model] ShadowInstancing: collect and compile.
void SetOn(bool on) {
  g_on = on;
  if (on && !Install()) return;
  g_collect = on || g_onTex.load();
}

// [Model] ShadowTextureSkip: the masks come from the compiled shadow keys, so
// the shadow casters' keys are collected and compiled while it is on.
bool SetOnTex(bool on) {
  g_onTex = on;
  if (on && !Install()) return false;
  g_collect = on || g_on.load();
  return true;
}

// [Model] GBufferBatching: collect and compile the G-buffer keys while on.
bool SetOnGb(bool on) {
  g_onGb = on;
  if (on && !InstallGb()) {
    g_collectGb = false;
    return false;
  }
  g_collectGb = on;
  return true;
}

struct Totals {
  uint32_t seen = 0, rejected = 0, queued = 0, keys = 0, done = 0, ok = 0, busy = 0, created = 0, map = 0;
};
// queued and busy are for both kinds; the rest for the given kind.
Totals Snap(int kind = kKindShadow) {
  Totals t;
  std::lock_guard<std::mutex> lock(g_mutex);
  const bool gb = kind == kKindGb;
  t.seen = (gb ? g_seenGb : g_seenShaders).load();
  t.rejected = (gb ? g_rejectedGb : g_rejectedSnapshots).load();
  t.queued = static_cast<uint32_t>(g_queue.size() + g_pendingCreate.size() + g_inboxCount.load());
  for (auto& kv : g_results) t.keys += kv.second->kind == kind;
  t.done = (gb ? g_keysDoneGb : g_keysDone).load();
  t.ok = (gb ? g_keysOkGb : g_keysOk).load();
  t.busy = static_cast<uint32_t>(g_busy);
  t.created = g_vsCreatedKind[gb ? kKindGb : kKindShadow].load();
  t.map = g_mapUsedKind[gb ? kKindGb : kKindShadow].load();
  return t;
}

// Waits until every queued key has compiled (or abort / 30 min); seconds.
double WaitCompiles(const std::atomic<bool>& abort) {
  const ULONGLONG t0 = GetTickCount64();
  for (;;) {
    const Totals t = Snap();
    if ((t.queued == 0 && t.busy == 0) || abort.load() || GetTickCount64() - t0 > 30ull * 60 * 1000) break;
    Sleep(250);
  }
  return (GetTickCount64() - t0) / 1000.0;
}

// Per-key lines of one kind again from this thread (so they land in the
// report), compile-time and failure summaries. Caller holds g_mutex.
void LogKeysLocked(int kind, const char* tag) {
  std::vector<double> ms;
  std::map<std::string, std::vector<std::string>> failed;
  double total = 0;
  std::vector<const KeyResult*> done;
  for (auto& kv : g_results) {
    const KeyResult& r = *kv.second;
    if (r.kind != kind) continue;
    if (r.msA + r.msB > 0) {
      ms.push_back(r.msA + r.msB);
      total += r.msA + r.msB;
    }
    if (!r.ok && !r.why.empty()) failed[r.why].push_back(r.path);
    if (r.done && !r.line.empty()) done.push_back(&r);
  }
  std::sort(done.begin(), done.end(), [](const KeyResult* a, const KeyResult* b) { return a->index < b->index; });
  for (const KeyResult* r : done) Log("  %s", r->line.c_str());
  std::sort(ms.begin(), ms.end());
  if (!ms.empty())
    Log("  %s: compile time per key (a+b): min %.0f ms, median %.0f ms, max %.0f ms, total %.1f s on %d workers", tag,
        ms.front(), ms[ms.size() / 2], ms.back(), total / 1000.0, kWorkers);
  for (auto& kv : failed) {
    std::string paths;
    for (size_t i = 0; i < kv.second.size() && i < 6; ++i) paths += (i ? ", " : "") + kv.second[i];
    Log("  %s: failed x%zu: %s [%s%s]", tag, kv.second.size(), kv.first.c_str(), paths.c_str(),
        kv.second.size() > 6 ? ", ..." : "");
  }
}

// [Suite] ShadowInstCompile: collect for collectMs, wait for every compile,
// log the summary. abort is polled while waiting.
void SuitePhase(int collectMs, const std::atomic<bool>& abort) {
  if (!Install()) {
    Log("  shadow inst: not available (see above)");
    return;
  }
  g_collect = true;
  Sleep(collectMs);
  g_collect = g_on.load() || g_onTex.load();
  const double waitS = WaitCompiles(abort);
  const Totals t = Snap();
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    for (const std::string& l : g_rejectLog)
      if (l.compare(0, 9, "g-buffer ") != 0) Log("  shadow inst: unusable shader: %s", l.c_str());
    LogKeysLocked(kKindShadow, "shadow inst");
  }
  Log("  shadow inst: %u DX11Shader objects seen (%u unusable), %u keys: %u OK, %u failed, %u not finished; "
      "%u VS objects created, %u map entries; waited %.1f s",
      t.seen, t.rejected, t.keys, t.ok, t.done - t.ok, t.keys - t.done, t.created, t.map, waitS);
  const std::string cache = CacheSummary();
  if (!cache.empty()) Log("  %s", cache.c_str());
}

// [Suite] GBufferInstCompile (R13 stage 1): collect the G-buffer keys seen in
// model passes 1 and 2 for collectMs, log the technique/pass selection, wait
// for every compile, log one line per key (checks 1-4 and the reflection
// gate) and the totals. frames: the quad-frame counter (per-frame figures).
void SuitePhaseGb(int collectMs, const std::atomic<bool>& abort, const std::atomic<uint64_t>* frames) {
  if (!InstallGb()) {
    Log("  gbuffer inst: not available (see above)");
    return;
  }
  for (auto& a : g_sel)
    for (auto& b : a)
      for (auto& c : b) c.store(0);
  g_selOther = 0;
  const uint64_t f0 = frames ? frames->load() : 0;
  g_collectGb = true;
  Sleep(collectMs);
  g_collectGb = g_onGb.load();
  const uint64_t nf = frames ? frames->load() - f0 : 0;
  const double per = nf ? static_cast<double>(nf) : collectMs / 1000.0;
  if (g_selOk) {
    Log("  gbuffer inst: ModelMaterialMT draws by model pass -> technique pass, per %s (%llu frames):",
        nf ? "frame" : "s", static_cast<unsigned long long>(nf));
    for (int p = 0; p < kSelModelPasses; ++p) {
      std::string line;
      uint64_t sum = 0;
      for (int t = 0; t < kSelSlots; ++t)
        for (int i = 0; i < kSelPassIdx; ++i) {
          const uint32_t n = g_sel[p][t][i].load();
          if (!n) continue;
          sum += n;
          char b[96];
          if (i == 16)
            snprintf(b, sizeof(b), "%s%s all passes %.1f", line.empty() ? "" : ", ", kTechSlotName[t], n / per);
          else
            snprintf(b, sizeof(b), "%s%s P%d %.1f", line.empty() ? "" : ", ", kTechSlotName[t], i, n / per);
          line += b;
        }
      if (sum) Log("  gbuffer inst:   pass %d (%.1f): %s", p, sum / per, line.c_str());
    }
    if (g_selOther.load()) Log("  gbuffer inst:   passes >= 16: %.1f", g_selOther.load() / per);
  }
  const double waitS = WaitCompiles(abort);
  const Totals t = Snap(kKindGb);
  uint32_t keysPrevUnused = 0, keysPsoVsOnly = 0, passes = 0, passesPrevUnused = 0;
  std::map<std::string, uint32_t> usedBy[kStages];  // member -> keys
  std::string prevUsedKeys;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    for (const std::string& l : g_rejectLog)
      if (l.compare(0, 9, "g-buffer ") == 0) Log("  gbuffer inst: unusable shader: %s", l.c_str() + 9);
    LogKeysLocked(kKindGb, "gbuffer inst");
    for (auto& kv : g_results) {
      const KeyResult& r = *kv.second;
      if (r.kind != kKindGb || !r.ok) continue;
      const GbSummary g = SummarizeGb(r);
      passes += g.passes;
      passesPrevUnused += g.prevUnused;
      if (g.prevUnused == g.passes)
        ++keysPrevUnused;
      else if (prevUsedKeys.size() < 300)
        prevUsedKeys += (prevUsedKeys.empty() ? "" : "; ") + r.path + " " + g.prevUsedAt + g.notAnalysed;
      keysPsoVsOnly += g.psoVsOnly == g.passes;
      for (int st = 0; st < kStages; ++st) {
        std::vector<std::string> v = g.names[st];
        std::sort(v.begin(), v.end());
        v.erase(std::unique(v.begin(), v.end()), v.end());
        for (const std::string& n : v) ++usedBy[st][n];
      }
    }
  }
  Log("  gbuffer inst: %u DX11Shader objects seen in passes 1-2 (%u unusable), %u keys: %u OK, %u failed, %u not "
      "finished; %u VS objects created, %u map entries; waited %.1f s",
      t.seen, t.rejected, t.keys, t.ok, t.done - t.ok, t.keys - t.done, t.created, t.map, waitS);
  Log("  gbuffer inst: gate: prevFrameTransform unused in every stage of every pass for %u of %u OK keys (%u of %u "
      "technique passes); posStructOffset VS-only in %u keys%s%s",
      keysPrevUnused, t.ok, passesPrevUnused, passes, keysPsoVsOnly, prevUsedKeys.empty() ? "" : "; used: ",
      prevUsedKeys.c_str());
  for (int st = 0; st < kStages; ++st) {
    if (usedBy[st].empty()) continue;
    std::string line;
    for (auto& kv : usedBy[st]) line += (line.empty() ? "" : ", ") + kv.first + " " + std::to_string(kv.second);
    Log("  gbuffer inst: def_uniforms members USED by the %s of any pass (keys): %s", kStageName[st], line.c_str());
  }
}

// [Suite] GBufferTexCount (R14 lead 3 gate): collect the G-buffer keys for
// warmMs and wait for their compiles (disk cache hits are fast), then count
// for countMs (collection stays on) and log per-frame figures. frames: the
// quad-frame counter (per second without it).
void SuitePhaseGbTex(int warmMs, int countMs, const std::atomic<bool>& abort, const std::atomic<uint64_t>* frames) {
  if (!InstallGb()) {
    Log("  gbuffer tex: not available (see above)");
    return;
  }
  g_collectGb = true;
  Sleep(warmMs);
  const double waitS = WaitCompiles(abort);
  TexCountClear();
  const uint64_t f0 = frames ? frames->load() : 0;
  g_texCountOn = true;
  Sleep(countMs);
  g_texCountOn = false;
  for (int i = 0; i < 2000 && g_inTexCount.load() != 0; ++i) Sleep(1);
  const uint64_t nf = frames ? frames->load() - f0 : 0;
  g_collectGb = g_onGb.load();
  if (g_inTexCount.load() != 0) {
    Log("  gbuffer tex: the counting thread did not leave the counter within 2 s; no report");  // its data stays (leaked)
    return;
  }
  const Totals t = Snap(kKindGb);
  const double per = nf ? static_cast<double>(nf) : countMs / 1000.0;
  const char* unit = nf ? "frame" : "s";
  const TexCountTotals& c = g_tc;
  Log("  gbuffer tex: %llu frames counted; waited %.1f s for the G-buffer keys first (%u keys: %u OK, %u failed, %u not "
      "finished)",
      static_cast<unsigned long long>(nf), waitS, t.keys, t.ok, t.done - t.ok, t.keys - t.done);
  Log("  gbuffer tex: G-buffer ModelMaterialMT draws (model pass 1, opaque, normal* P0) %.1f/%s, bound texture sets "
      "(handles != -1) %.1f/%s, %.2f per draw",
      c.draws / per, unit, c.sets / per, unit, c.draws ? static_cast<double>(c.sets) / c.draws : 0.0);
  const uint64_t known = c.sets - c.unknownSets;
  Log("  gbuffer tex: sets whose variable no stage of that technique pass reads in our (a) compile: %.1f/%s = %.1f%% "
      "of all sets, %.1f%% of the %.1f/%s known sets; at ~19 ns per set (R10 C2 rate) about %.3f ms/%s",
      c.unread / per, unit, c.sets ? 100.0 * c.unread / c.sets : 0.0, known ? 100.0 * c.unread / known : 0.0,
      known / per, unit, c.unread * 19e-6 / per, unit);
  {
    std::map<std::string, uint64_t> why;
    for (auto& kv : g_tcUnknownWhy) why[kv.first] += kv.second;
    std::vector<std::pair<uint64_t, std::string>> v;
    for (auto& kv : why) v.push_back({kv.second, kv.first});
    std::sort(v.rbegin(), v.rend());
    std::string line;
    char b[64];
    for (size_t i = 0; i < v.size() && i < 8; ++i) {
      snprintf(b, sizeof(b), " %.1f", v[i].first / per);
      line += (i ? "; " : ": ") + v[i].second + b;
    }
    Log("  gbuffer tex: unknown sets %.1f/%s (%.1f%% of all sets) on %.1f draws/%s%s", c.unknownSets / per, unit,
        c.sets ? 100.0 * c.unknownSets / c.sets : 0.0, c.unknownDraws / per, unit, line.c_str());
  }
  {
    std::map<std::string, uint64_t> byName;
    for (auto& kv : g_tcUnreadByName) byName[kv.first->empty() ? std::string("(unnamed)") : *kv.first] += kv.second;
    std::vector<std::pair<uint64_t, std::string>> v;
    for (auto& kv : byName) v.push_back({kv.second, kv.first});
    std::sort(v.rbegin(), v.rend());
    std::string line;
    char b[64];
    for (size_t i = 0; i < v.size() && i < 10; ++i) {
      snprintf(b, sizeof(b), " %.1f", v[i].first / per);
      line += (i ? ", " : "") + v[i].second + b;
    }
    Log("  gbuffer tex: top unread variables (sets/%s, %zu distinct): %s", unit, v.size(),
        line.empty() ? "-" : line.c_str());
  }
  uint32_t unmappable = 0;
  for (auto& kv : g_tcMasks) unmappable += kv.second->e.state <= 0;
  Log("  gbuffer tex: not counted: pass-1 opaque cockpit draws %.1f/%s, selection mirror disagreed %.1f/%s, items on "
      "other threads %.1f/%s; %zu (shader, normal* P0) masks, %u unknown",
      c.cockpit / per, unit, c.selMismatch / per, unit, g_texCountOtherThread.load() / per, unit, g_tcMasks.size(),
      unmappable);
  TexCountClear();
}

// Payload unload: stop collecting, join the workers, then release.
// release = false: a user of the VS objects and compile results (shadow
// batching, G-buffer batching, a recorder) could not be drained, so they are
// left alive (the workers are still stopped). Before, they were released
// while such a pass could still read them.
void Shutdown(bool release = true) {
  if (g_state.load() <= 0) return;
  instcount::g_observer.store(nullptr);
  if (gbcount::g_observer.load() == &GbObserver) gbcount::g_observer.store(nullptr);
  g_texCountOn = false;
  g_collect = false;
  g_collectGb = false;
  g_stop = true;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_cv.notify_all();
  }
  bool joined = true;
  for (HANDLE& h : g_workers) {
    if (!h) continue;
    // D3DCompile cannot be interrupted: a worker finishes its current effect.
    if (WaitForSingleObject(h, 60000) != WAIT_OBJECT_0) joined = false;
    CloseHandle(h);
    h = nullptr;
  }
  for (int i = 0; i < 1000 && g_inObserver.load() != 0; ++i) Sleep(1);
  if (!joined || g_inObserver.load() != 0) {
    g_state = -2;
    Log("shadow inst: a compile worker did not stop within 60 s; its objects are leaked, not released");
    return;
  }
  if (!release) {
    g_state = -2;
    Log("shadow inst: a pass using its shaders was still running at unload; its objects are left alive");
    return;
  }
  std::lock_guard<std::mutex> lock(g_mutex);
  uint32_t released = 0;
  if (g_map) {
    for (size_t i = 0; i < kMapSize; ++i) g_map[i].shader.store(nullptr);
  }
  if (g_texMap) {
    for (size_t i = 0; i < kTexMapSize; ++i) g_texMap[i].shader.store(nullptr);
  }
  for (auto& kv : g_results) {
    for (Snapshot* w : kv.second->waiters) delete w;
    for (PassVs& p : kv.second->vs)
      if (p.vs) {
        p.vs->Release();
        p.vs = nullptr;
        ++released;
      }
    delete kv.second;
  }
  g_results.clear();
  DrainInboxLocked();  // R90 H2
  for (Snapshot* s : g_queue) delete s;
  g_queue.clear();
  for (auto& pc : g_pendingCreate) delete pc.first;
  g_pendingCreate.clear();
  if (g_device) g_device->Release();
  g_device = nullptr;
  g_gbInstalled = false;
  g_state = -2;  // never re-installed by this payload (it is being unloaded)
  Log("shadow inst: unloaded (%u VS objects released)", released);
}

}  // namespace shadowinst
