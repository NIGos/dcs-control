// Offline tests of the G-buffer part of shadow_inst.h (R13 stage 1): the
// model_vs source edit on the real sources, our compiles of the main-pass
// techniques with checks 1-4 and the reflection gate, the dataflow comparer
// and the gate on synthetic shaders, the technique/pass selection mirror, the
// runtime path on a fake DX11Shader, and (from sitest::Lifecycle) the
// observer on gb_count.h's hook. Sources are the temp copies made by
// sitest::Run (Bazar is only read by that copy).
// Included from test_main.cpp after shadow_inst_test.h.
#pragma once

namespace gbtest {
using namespace shadowinst;

// Main-pass define sets: def_material.fx variants (static, textured with
// tangents, alpha test, transparent, deferred decal, skinned, damage volume
// and RGBA masks, registration + damage + decal) and the other
// def_material.fx includers.
std::vector<SourceKey> GbKeys() {
  const std::vector<Define> base = {{"DIRECTX11", "true"}, {"NORMAL_SIZE", "3"}, {"POSITION_SIZE", "4"},
                                    {"USE_DCS_DEFERRED", "1"}};
  struct Set {
    const char* path;
    std::vector<Define> extra;
  };
  const std::vector<Set> sets = {
      {"model/def_material.fx", {}},
      {"model/def_material.fx",
       {{"DIFFUSE_UV", "tc0"}, {"NORMAL_MAP_UV", "tc0"}, {"SPECULAR_UV", "tc0"}, {"TANGENT_SIZE", "4"},
        {"TEXCOORD0_SIZE", "2"}}},
      {"model/def_material.fx", {{"BLEND_MODE", "BM_ALPHA_TEST"}, {"DIFFUSE_UV", "tc0"}, {"TEXCOORD0_SIZE", "2"}}},
      {"model/def_material.fx", {{"BLEND_MODE", "BM_TRANSPARENT"}, {"DIFFUSE_UV", "tc0"}, {"TEXCOORD0_SIZE", "2"}}},
      {"model/def_material.fx",
       {{"BLEND_MODE", "BM_DECAL_DEFERRED"}, {"DECAL_UV", "tc0"}, {"DIFFUSE_UV", "tc0"}, {"TEXCOORD0_SIZE", "2"}}},
      {"model/def_material.fx", {{"BONES_WEIGHTS_SIZE", "4"}, {"ENABLE_SKELETAL_ANIMATION", ""}}},
      {"model/def_material.fx",
       {{"BLEND_MODE", "BM_ALPHA_TEST"}, {"DAMAGE_UV", "tc1"}, {"DIFFUSE_UV", "tc0"}, {"ENABLE_DAMAGE_ARGUMENTS", ""},
        {"TEXCOORD0_SIZE", "2"}, {"TEXCOORD1_SIZE", "2"}}},
      {"model/def_material.fx",
       {{"BLEND_MODE", "BM_ALPHA_TEST"}, {"DAMAGE_RGBA_MASK", ""}, {"DAMAGE_UV", "tc1"}, {"DIFFUSE_UV", "tc0"},
        {"ENABLE_DAMAGE_ARGUMENTS", ""}, {"TEXCOORD0_SIZE", "2"}, {"TEXCOORD1_SIZE", "2"}}},
      {"model/def_material.fx",
       // registration excludes skinning (structs.hlsl: both declare bonesWeights)
       {{"AIRCRAFT_REGISTRATION", ""}, {"BONES_WEIGHTS_SIZE", "1"}, {"DAMAGE_UV", "tc1"}, {"DECAL_UV", "tc0"},
        {"DIFFUSE_UV", "tc0"}, {"ENABLE_DAMAGE_ARGUMENTS", ""}, {"NORMAL_MAP_UV", "tc0"}, {"TANGENT_SIZE", "4"},
        {"TEXCOORD0_SIZE", "2"}, {"TEXCOORD1_SIZE", "2"}}},
      {"model/building_material.fx", {{"DIFFUSE_UV", "tc0"}, {"TEXCOORD0_SIZE", "2"}}},
      {"model/forest_material.fx", {{"BLEND_MODE", "BM_ALPHA_TEST"}, {"DIFFUSE_UV", "tc0"}, {"TEXCOORD0_SIZE", "2"}}},
      {"model/self_illum_material.fx", {{"DIFFUSE_UV", "tc0"}, {"SELF_ILLUMINATION_UV", "tc0"}, {"TEXCOORD0_SIZE", "2"}}},
      {"model/chrome_material.fx", {{"DIFFUSE_UV", "tc0"}, {"TEXCOORD0_SIZE", "2"}}},
      {"model/color_material.fx", {}},
      {"model/bano_material.fx", {{"BLEND_MODE", "BM_ADDITIVE"}, {"DIFFUSE_UV", "tc0"}, {"TEXCOORD0_SIZE", "2"}}},
      {"model/def_with_rgba_dmg_material.fx",
       {{"BLEND_MODE", "BM_ALPHA_TEST"}, {"DAMAGE_UV", "tc1"}, {"DIFFUSE_UV", "tc0"}, {"ENABLE_DAMAGE_ARGUMENTS", ""},
        {"TEXCOORD0_SIZE", "2"}, {"TEXCOORD1_SIZE", "2"}}},
      {"model/transparent_self_illum_material.fx",
       {{"BLEND_MODE", "BM_TRANSPARENT"}, {"DIFFUSE_UV", "tc0"}, {"TEXCOORD0_SIZE", "2"}}},
      {"model/additive_self_illum_material.fx",
       {{"BLEND_MODE", "BM_ADDITIVE"}, {"DIFFUSE_UV", "tc0"}, {"TEXCOORD0_SIZE", "2"}}},
  };
  std::vector<SourceKey> keys;
  for (const Set& s : sets) {
    SourceKey k;
    k.path = s.path;
    k.defines = base;
    for (const Define& d : s.extra) k.defines.push_back(d);
    std::sort(k.defines.begin(), k.defines.end(), [](const Define& a, const Define& b) { return a.name < b.name; });
    for (Define& d : k.defines) d.keyValue = !d.value.empty();
    k.key = BuildKey(k.path, k.defines);
    keys.push_back(k);
  }
  return keys;
}

// ---- Synthetic shaders for the comparer and the gate ----

bool CompileSrc(const Compiler& cc, const char* src, const char* entry, const char* target, std::vector<uint8_t>& out,
                std::string* err = nullptr) {
  auto compile = reinterpret_cast<decltype(&D3DCompile)>(GetProcAddress(cc.dll, "D3DCompile"));
  ID3DBlob* code = nullptr;
  ID3DBlob* errs = nullptr;
  const bool ok = SUCCEEDED(compile(src, strlen(src), "synthetic", nullptr, nullptr, entry, target,
                                    D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errs)) &&
                  code;
  if (ok) out.assign(static_cast<uint8_t*>(code->GetBufferPointer()),
                     static_cast<uint8_t*>(code->GetBufferPointer()) + code->GetBufferSize());
  if (!ok && err && errs) *err = static_cast<const char*>(errs->GetBufferPointer());
  if (code) code->Release();
  if (errs) errs->Release();
  return ok;
}

// B0 / B1 pairs with the uniforms.hlsl layout trick (posStructOffset at
// cb0[15].w). bodyB is the (b) body; it may differ from (a) on purpose.
std::string SynthVs(bool instanced, const char* body) {
  std::string s =
      "cbuffer def_uniforms { float4 pad[15]; float3 pad2; uint ";
  s += instanced ? "qvPsoBase; };\nstatic uint posStructOffset;\n"
                   "StructuredBuffer<uint> qvInstOffsets : register(t127);\n"
                 : "posStructOffset; };\n";
  s += "StructuredBuffer<float4> sb : register(t0);\n";
  s += instanced ? "float4 main(float4 p : POSITION, uint vid : SV_VertexID, uint qvIID : SV_InstanceID) : SV_Position {\n"
                   "posStructOffset = qvInstOffsets[qvPsoBase + qvIID];\n"
                 : "float4 main(float4 p : POSITION, uint vid : SV_VertexID) : SV_Position {\n";
  s += body;
  s += "}\n";
  return s;
}

void Synthetic(const Compiler& cc) {
  const char* good = "float4 a = sb[posStructOffset + vid]; float4 b = sb[posStructOffset + (uint)p.w];\n"
                     "float4 q = p; q.w = 1; return a * pad[0] + b * dot(q, pad[1]);\n";
  const char* changedOp = "float4 a = sb[posStructOffset + vid]; float4 b = sb[posStructOffset + (uint)p.w];\n"
                          "float4 q = p; q.w = 1; return a * pad[0] - b * dot(q, pad[1]);\n";
  const char* changedOrder = "float4 a = sb[posStructOffset + vid]; float4 b = sb[posStructOffset + (uint)p.w];\n"
                             "float4 q = p; q.w = 1; return (a + b) * pad[0] * dot(q, pad[1]);\n";
  // The same inputs feeding the other load (signatures unchanged, dataflow not).
  const char* otherInput = "float4 a = sb[posStructOffset + (uint)p.w]; float4 b = sb[posStructOffset + vid];\n"
                           "float4 q = p; q.w = 1; return a * pad[0] + b * dot(q, pad[1]);\n";
  const char* otherOffset = "float4 a = sb[posStructOffset + vid]; float4 b = sb[posStructOffset + 1 + (uint)p.w];\n"
                            "float4 q = p; q.w = 1; return a * pad[0] + b * dot(q, pad[1]);\n";
  std::vector<uint8_t> a, b, bad[4];
  std::string err;
  bool built = CompileSrc(cc, SynthVs(false, good).c_str(), "main", "vs_5_0", a, &err) &&
               CompileSrc(cc, SynthVs(true, good).c_str(), "main", "vs_5_0", b, &err);
  const char* bads[4] = {changedOp, changedOrder, otherInput, otherOffset};
  for (int i = 0; i < 4 && built; ++i) built = CompileSrc(cc, SynthVs(true, bads[i]).c_str(), "main", "vs_5_0", bad[i], &err);
  if (!built) {
    printf("     synthetic compile: %s\n", err.c_str());
    Check(false, "gbuffer inst: synthetic VS pairs compile");
    return;
  }
  FxBlob fa{a.data(), static_cast<uint32_t>(a.size())}, fb{b.data(), static_cast<uint32_t>(b.size())};
  VariantDiff d;
  std::string detail;
  const char* w = CompareVariants(cc, fa, fb, d, detail);
  DisasmDiff dd;
  const char* w4 = w ? "checks 1-3 failed" : CompareDisasm(cc, fa, fb, d, MaterialCbSlot(cc, fa), dd, detail);
  printf("     synthetic: checks 1-3 %s, check 4 %s%s%s (%u->%u instr, %u outputs)\n", w ? w : "pass",
         w4 ? w4 : "pass", detail.empty() ? "" : " ", detail.c_str(), dd.instrA, dd.instrB, dd.outputs);
  Check(!w && !w4 && dd.outputs == 4 && dd.instrB == dd.instrA + 2,
        "gbuffer inst: dataflow compare accepts (a) + iadd + ld_structured t127 with renamed temps");
  int rejected = 0;
  for (int i = 0; i < 4; ++i) {
    FxBlob fx{bad[i].data(), static_cast<uint32_t>(bad[i].size())};
    VariantDiff d2;
    std::string det2;
    const char* v = CompareVariants(cc, fa, fx, d2, det2);
    DisasmDiff dd2;
    const char* v4 = v ? nullptr : CompareDisasm(cc, fa, fx, d2, MaterialCbSlot(cc, fa), dd2, det2);
    printf("     synthetic negative %d: checks 1-3 %s, check 4 %s %s\n", i, v ? v : "pass", v4 ? v4 : "pass",
           det2.c_str());
    rejected += !v && v4 != nullptr;
  }
  Check(rejected == 4,
        "gbuffer inst: dataflow compare rejects a changed float op, a changed op order, another input and another "
        "offset (each passes checks 1-3)");
  // (a) against itself: no t127, no SV_InstanceID (check 4 alone).
  DisasmDiff dd3;
  std::string det3;
  Check(CompareDisasm(cc, fa, fa, d, 0, dd3, det3) != nullptr, "gbuffer inst: dataflow compare rejects (a) vs (a)");

  // Gate on a synthetic effect: def_uniforms with uniforms.hlsl's offsets.
  const char* fx =
      "cbuffer def_uniforms { uint4 lightCount; float4 FlatShadowPlane; float4x4 prevFrameTransform; float4 flirCoeff;\n"
      "  float4 pad[8]; float3 pad2; uint posStructOffset; float4 tail[3]; };\n"
      "float4 vs(float4 p : POSITION) : SV_Position { return p * pad[0] + posStructOffset; }\n"
      "Texture2D texRead; Texture2D texUnread;\n"
      "float4 psGood() : SV_Target { return flirCoeff + tail[2] + texRead.Load(int3(0, 0, 0)); }\n"
      "float4 psPrev() : SV_Target { return prevFrameTransform[1]; }\n"
      "float4 psPso() : SV_Target { return posStructOffset; }\n"
      "VertexShader v = CompileShader(vs_5_0, vs());\n"
      "technique11 t {\n"
      " pass P0 { SetVertexShader(v); SetGeometryShader(NULL); SetPixelShader(CompileShader(ps_5_0, psGood())); }\n"
      " pass P1 { SetVertexShader(v); SetGeometryShader(NULL); SetPixelShader(CompileShader(ps_5_0, psPrev())); }\n"
      " pass P2 { SetVertexShader(v); SetGeometryShader(NULL); SetPixelShader(CompileShader(ps_5_0, psPso())); }\n"
      "}\n";
  ID3DBlob* code = nullptr;
  ID3DBlob* errs = nullptr;
  const HRESULT hr = cc.compile2(fx, strlen(fx), "gate.fx", nullptr, nullptr, nullptr, "fx_5_0",
                                 D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, 0, nullptr, 0, &code, &errs);
  FxEffect e;
  std::string perr;
  const bool parsed = SUCCEEDED(hr) && code &&
                      ParseFx5(static_cast<uint8_t*>(code->GetBufferPointer()), code->GetBufferSize(), e, perr);
  if (!parsed && errs) printf("     gate effect: %s\n", static_cast<const char*>(errs->GetBufferPointer()));
  const FxTechnique* t = parsed ? FindTechnique(e, "t") : nullptr;
  PassGate g[3];
  if (t && t->passes.size() == 3)
    for (int i = 0; i < 3; ++i) GatePass(cc, t->passes[i], g[i]);
  const uint64_t prevBits = 0xffffull << 8;  // dwords 8..23 = CB +0x20..+0x5f
  const bool gateOk = t && g[0].ok && g[0].prevUnused && g[0].psoVsOnly && !(g[0].used[0] & prevBits) &&
                      (g[0].used[0] >> 63 & 1) && (g[0].used[0] >> 24 & 0xf) == 0xf &&  // posStructOffset, flirCoeff
                      (g[0].used[1] >> (0x120 / 4 - 64) & 0xf) == 0xf &&                // tail[2]
                      g[1].ok && !g[1].prevUnused && g[1].psoVsOnly && g[2].ok && g[2].prevUnused &&
                      !g[2].psoVsOnly && JoinNames(g[0].names[0]) == "flirCoeff,tail" &&
                      JoinNames(g[0].names[1]) == "pad,posStructOffset";
  printf("     gate synthetic: P0 prev %s pso %s VS {%s} PS {%s}; P1 prev %s; P2 pso %s\n",
         g[0].prevUnused ? "unused" : "used", g[0].psoVsOnly ? "VS-only" : "elsewhere", JoinNames(g[0].names[1]).c_str(),
         JoinNames(g[0].names[0]).c_str(), g[1].prevUnused ? "unused" : "used", g[2].psoVsOnly ? "VS-only" : "elsewhere");
  Check(gateOk,
        "gbuffer inst: reflection gate finds prevFrameTransform and posStructOffset reads per stage (D3D_SVF_USED) "
        "and the used CB dwords");
  // Per-pass read sets (R14 lead 3 counter): every RDEF binding of the pass, any stage.
  for (int i = 0; t && t->passes.size() == 3 && i < 3; ++i) PassReads(t->passes[i], g[i]);
  const std::vector<std::string> p0 = {"def_uniforms", "texRead"}, p1 = {"def_uniforms"};
  printf("     pass reads synthetic: P0 {%s} P1 {%s}\n", JoinNames(g[0].reads).c_str(), JoinNames(g[1].reads).c_str());
  Check(t && g[0].readsOk && g[0].reads == p0 && g[1].readsOk && g[1].reads == p1 && g[2].reads == p1,
        "gbuffer inst: per-pass read sets list the resources each pass binds (texRead in P0 only, texUnread nowhere)");
  if (code) code->Release();
  if (errs) errs->Release();
}

// ---- Selection mirror ----

void Selection() {
  struct Case {
    uint32_t pass;
    uint8_t transparent, p12d, c0, c1, c4;
    float flir;
    bool radar;
    int slot, idx;
  } cases[] = {
      {1, 0, 0, 0, 0, 0, 0, true, 0, 0},  // G-buffer
      {1, 0, 0, 0, 1, 0, 0, true, 0, 0},  // opaque pass 1 ignores the context bits
      {1, 1, 0, 0, 0, 0, 0, true, 0, 1},  // transparent: forward
      {1, 1, 0, 0, 1, 0, 0, true, 0, 2},
      {2, 0, 0, 0, 0, 0, 0, true, 0, 1},
      {2, 0, 0, 0, 1, 0, 0, true, 0, 2},
      {2, 0, 1, 0, 1, 0, 0, true, 0, 0},  // props+0x12d outside the cockpit
      {1, 0, 0, 1, 0, 0, 0, true, 1, 0},  // cockpit G-buffer
      {2, 0, 0, 1, 0, 0, 0, true, 1, 1},
      {2, 0, 0, 1, 1, 0, 0, true, 1, 2},
      {2, 0, 0, 1, 0, 1, 0, true, 1, 3},
      {2, 0, 0, 1, 1, 1, 0, true, 1, 4},
      {3, 0, 1, 1, 0, 0, 0, true, 1, 2},  // props+0x12d in the cockpit, pass 3
      {0, 0, 1, 1, 0, 0, 0, true, 1, 1},
      {5, 0, 0, 0, 0, 0, 0, true, 0, 1},  // 5 -> 0x15e20
      {8, 0, 0, 0, 0, 0, 2.0f, true, 5, 0},
      {8, 0, 0, 0, 0, 0, 0.5f, true, 5, 1},
      {8, 1, 0, 1, 1, 1, 0.5f, true, 6, 1},
      {4, 0, 0, 0, 0, 0, 0, true, 4, 0},
      {6, 0, 0, 0, 0, 0, 0, true, 3, 0},
      {7, 0, 0, 0, 0, 0, 0, true, 2, 0},
      {9, 0, 0, 0, 0, 0, 0, true, 13, 0},
      {10, 0, 0, 0, 0, 0, 0, true, 9, 16},
      {10, 0, 0, 0, 0, 0, 0, false, 13, 0},
      {11, 0, 0, 0, 0, 0, 0, true, 10, 0},
      {13, 0, 0, 0, 0, 0, 0, true, 12, 0},
      {13, 1, 0, 0, 0, 0, 0, true, 13, 0},
  };
  int bad = 0;
  for (const Case& c : cases) {
    const shadowinst::Selection s =
        SelectTechPass(c.pass, c.transparent, c.p12d, c.c0, c.c1, c.c4, c.flir, 1.0f, c.radar);
    if (s.slot != c.slot || s.pass != c.idx) {
      printf("     selection: pass %u t%u p%u ctx %u%u%u -> %d P%d, expected %d P%d\n", c.pass, c.transparent, c.p12d,
             c.c0, c.c1, c.c4, s.slot, s.pass, c.slot, c.idx);
      ++bad;
    }
  }
  Check(bad == 0, "gbuffer inst: technique/pass selection mirror of 0x16140 / 0x15e20 (27 cases)");
}

// ---- Real sources: edits, compiles, checks 1-4, gate ----

struct KeyOut {
  bool okA = false, okB = false, okC = false, inert = false;
  int edits = 0;
  std::string err, line;
  int techs = 0, passes = 0, vsOk = 0, gateOk = 0, prevUnused = 0, psoVsOnly = 0, created = 0, vsBlobs = 0;
  std::vector<std::string> psNames, vsNames;
};

void RunKey(const Compiler& cc, const std::wstring& shaders, const SourceKey& k, ID3D11Device* dev, KeyOut& o) {
  std::vector<uint8_t> a, b, c;
  std::string ea, eb, ec;
  double msA = 0, msB = 0;
  o.okA = CompileEffect(cc, shaders, k, false, kVarRef, a, ea, &msA);
  o.okB = CompileEffect(cc, shaders, k, true, kVarModel, b, eb, &msB, &o.edits);
  o.okC = CompileEffect(cc, shaders, k, true, kVarRef, c, ec);
  if (!o.okA || !o.okB || !o.okC) {
    o.err = "compile failed: a " + ea + " | b " + eb + " | c " + ec;
    return;
  }
  o.inert = a == c;
  FxEffect fa, fb;
  std::string e;
  if (!ParseFx5(a.data(), a.size(), fa, e) || !ParseFx5(b.data(), b.size(), fb, e)) {
    o.err = "parse: " + e;
    return;
  }
  std::set<const uint8_t*> blobs;
  char buf[256];
  for (const char* tech : {"normal_cf", "normal_cf_db", "normal_cockpit_cf", "normal_cockpit_cf_db"}) {
    std::vector<FxBlob> va, vb;
    const FxTechnique* ta = FindTechnique(fa, tech);
    if (!ta || !TechniqueVs(fa, tech, va, e) || !TechniqueVs(fb, tech, vb, e) || va.size() != vb.size()) {
      o.err = std::string(tech) + ": " + e;
      return;
    }
    ++o.techs;
    for (size_t p = 0; p < va.size(); ++p) {
      ++o.passes;
      blobs.insert(vb[p].p);
      VariantDiff d;
      DisasmDiff dd;
      std::string detail;
      const char* w = CompareVariants(cc, va[p], vb[p], d, detail);
      if (!w && (d.cbName != "def_uniforms" || d.cbOffset != 0xfc || d.cbSize != 0x130)) w = "unexpected CB";
      if (!w) w = CompareDisasm(cc, va[p], vb[p], d, MaterialCbSlot(cc, va[p]), dd, detail);
      if (w) {
        if (o.err.empty()) o.err = std::string(tech) + " P" + std::to_string(p) + ": " + w + " " + detail;
      } else {
        ++o.vsOk;
      }
      if (p == 0 && !strcmp(tech, "normal_cf")) {
        snprintf(buf, sizeof(buf), "VS %u->%u instr, %u->%u temps, %u outputs identical; a %.0f ms, b %.0f ms",
                 dd.instrA, dd.instrB, d.tempsA, d.tempsB, dd.outputs, msA, msB);
        o.line = buf;
      }
      PassGate g;
      GatePass(cc, ta->passes[p], g);
      o.gateOk += g.ok;
      o.prevUnused += g.ok && g.prevUnused;
      o.psoVsOnly += g.ok && g.psoVsOnly;
      if (!g.ok && o.err.empty()) o.err = std::string(tech) + " P" + std::to_string(p) + " gate: " + g.why;
      for (const std::string& n : g.names[0]) o.psNames.push_back(n);
      for (const std::string& n : g.names[1]) o.vsNames.push_back(n);
    }
  }
  o.vsBlobs = static_cast<int>(blobs.size());
  if (dev) {
    std::vector<FxBlob> vb;
    if (TechniqueVs(fb, "normal_cf", vb, e)) {
      ID3D11VertexShader* vs = nullptr;
      if (SUCCEEDED(dev->CreateVertexShader(vb[0].p, vb[0].n, nullptr, &vs))) {
        ++o.created;
        vs->Release();
      }
    }
  }
}

void RealSources(const Compiler& cc, const std::wstring& shaders, ID3D11Device* dev) {
  // Edits on the real files.
  std::string vsrc, uni;
  const bool read = ReadWholeFile(shaders + L"model\\functions\\vertex_shader.hlsl", vsrc) &&
                    ReadWholeFile(shaders + L"model\\common\\uniforms.hlsl", uni);
  std::string v2 = vsrc, u2 = uni;
  const char* w1 = read ? EditVertexShader(v2) : "unreadable";
  const char* w2 = read ? EditUniforms(u2) : "unreadable";
  Check(!w1 && !w2 && CountOf(v2, "register(t127)") == 1 && CountOf(v2, "SV_InstanceID") == 1 &&
            CountOf(v2, "posStructOffset = qvInstOffsets[qvPsoBase + qvIID];") == 1 &&
            CountOf(v2, "#ifdef QV_MODEL_INSTANCED") == 2 && CountOf(u2, kUniformsGuard) == 2,
        "gbuffer inst: model_vs edit applies to the real vertex_shader.hlsl; uniforms.hlsl edit guarded by both "
        "defines");
  std::string bad = "VS_OUTPUT model_vs(VS_INPUT input)\n{}\nVS_OUTPUT model_vs(VS_INPUT input)\n{}\n";
  std::string bad2 = "VS_OUTPUT other_vs(VS_INPUT input) {}";
  Check(EditVertexShader(bad) != nullptr && EditVertexShader(bad2) != nullptr,
        "gbuffer inst: model_vs edit refuses files whose anchor is missing or repeated");

  // Compiles, in parallel (D3DCompile is thread-safe; each key is independent).
  const std::vector<SourceKey> keys = GbKeys();
  std::vector<KeyOut> out(keys.size());
  std::atomic<size_t> next{0};
  std::vector<std::thread> pool;
  for (int t = 0; t < 4; ++t)
    pool.emplace_back([&] {
      for (size_t i; (i = next.fetch_add(1)) < keys.size();) RunKey(cc, shaders, keys[i], dev, out[i]);
    });
  for (std::thread& t : pool) t.join();
  int compiled = 0, inert = 0, edits = 0, allVs = 0, allGate = 0, allPrev = 0, allPso = 0, created = 0, oneVs = 0;
  std::vector<std::string> ps, vs;
  for (size_t i = 0; i < keys.size(); ++i) {
    const KeyOut& o = out[i];
    compiled += o.okA && o.okB && o.okC;
    inert += o.inert;
    edits += o.edits == 5;
    const bool vsAll = o.passes == 28 && o.vsOk == o.passes;
    allVs += vsAll;
    allGate += o.passes == 28 && o.gateOk == o.passes;
    allPrev += o.passes == 28 && o.prevUnused == o.passes;
    allPso += o.passes == 28 && o.psoVsOnly == o.passes;
    created += o.created;
    oneVs += o.vsBlobs == 1;
    for (const std::string& n : o.psNames) ps.push_back(n);
    for (const std::string& n : o.vsNames) vs.push_back(n);
    printf("     gb %-62.62s: %s; %d techs %d passes: VS checks %d, gate %d, prevFrameTransform unused %d, "
           "posStructOffset VS-only %d, %d distinct (b) VS; PS reads %s%s%s\n",
           keys[i].key.c_str(), o.line.empty() ? "-" : o.line.c_str(), o.techs, o.passes, o.vsOk, o.gateOk,
           o.prevUnused, o.psoVsOnly, o.vsBlobs, JoinNames(o.psNames).c_str(), o.err.empty() ? "" : "; FIRST ISSUE: ",
           o.err.c_str());
  }
  const int n = static_cast<int>(keys.size());
  printf("     gb: %d keys, %d compiled, VS used members %s\n", n, compiled, JoinNames(vs).c_str());
  printf("     gb: PS used members (union) %s\n", JoinNames(ps).c_str());
  Check(compiled == n && inert == n && edits == n,
        "gbuffer inst: every main-pass key compiles as (a), (b) with QV_MODEL_INSTANCED (uniforms + vertex_shader "
        "edited) and inert-edited (identical to (a))");
  Check(allVs == n && oneVs == n,
        "gbuffer inst: every pass VS of normal_cf[_db] / normal_cockpit_cf[_db] passes checks 1-4 (signatures, CB, "
        "bindings, dataflow) and all 28 passes share one (b) VS");
  Check(allGate == n && allPrev == n,
        "gbuffer inst: gate: prevFrameTransform is unused in every stage of every main-pass technique pass");
  Check(allPso == n, "gbuffer inst: gate: posStructOffset is read by the VS only, in every pass");
  Check(dev && created == n, "gbuffer inst: instanced model_vs objects are created by a D3D11 device");
}

// ---- Runtime path on a fake DX11Shader (normal_cf 5 passes, normal_cockpit_cf 9) ----

ID3D11VertexShader* g_vs = nullptr;
ID3D11Device* g_dev = nullptr;
struct FakeTech {
  void* vt;
  uint32_t passes;
};
struct FakeObj2 {
  void* vt;
};
FakeObj2 g_pass, g_var;
FakeTech g_tech[3];
long __fastcall TechDesc(void* self, void* out) {
  struct D {
    const char* name;
    uint32_t passes, annotations;
  }* d = static_cast<D*>(out);
  d->name = "t";
  d->passes = static_cast<FakeTech*>(self)->passes;
  d->annotations = 0;
  return 0;
}
void* __fastcall GetPass(void* self, uint32_t i) { return i < static_cast<FakeTech*>(self)->passes ? &g_pass : nullptr; }
long __fastcall PassVsDesc(void*, void* out) {
  struct D {
    void* var;
    uint32_t index;
  }* d = static_cast<D*>(out);
  d->var = &g_var;
  d->index = 0;
  return 0;
}
long __fastcall GetVs(void*, uint32_t, ID3D11VertexShader** vs) {
  *vs = g_vs;
  if (g_vs) g_vs->AddRef();
  return g_vs ? 0 : E_FAIL;
}
long __fastcall GetDevice(void*, ID3D11Device** dev) {
  *dev = g_dev;
  if (g_dev) g_dev->AddRef();
  return g_dev ? 0 : E_FAIL;
}

struct Images {
  std::vector<uint8_t> dxImage, ngImage;
  uint8_t* dx = nullptr;
  uint8_t* ng = nullptr;
  Images() : dxImage(0xc1000), ngImage(0x60000) {
    dx = dxImage.data();
    ng = ngImage.data();
    auto vt = [&](uint32_t rva, int slot, void* fn) { reinterpret_cast<void**>(dx + rva)[slot] = fn; };
    vt(kTechVtbl, 4, reinterpret_cast<void*>(&TechDesc));
    vt(kTechVtbl, 7, reinterpret_cast<void*>(&GetPass));
    vt(kPassVtbl, 5, reinterpret_cast<void*>(&PassVsDesc));
    vt(kShaderVarVtbl, 32, reinterpret_cast<void*>(&GetVs));
    vt(kEffectVtbl, 4, reinterpret_cast<void*>(&GetDevice));
    g_pass.vt = dx + kPassVtbl;
    g_var.vt = dx + kShaderVarVtbl;
    for (FakeTech& t : g_tech) t.vt = dx + kTechVtbl;
    g_tech[0].passes = 5;
    g_tech[1].passes = 9;
    g_tech[2].passes = 1;
  }
};

// DX11Shader + ModelMaterialMT + item + SceneRenderable, laid out as DCS's.
struct Fake {
  uint8_t sh[0x248] = {};
  uint8_t mat[0x300] = {};
  uint8_t props[0x200] = {};
  uint8_t recs[0x50 * 3] = {};
  uint8_t item[0x100] = {};
  uint8_t flat[0x20] = {};
  uint8_t r[0x80] = {};
  FakeObj2 effect{};
  std::vector<uint8_t> defs;
  std::string* path = nullptr;
  std::string* key = nullptr;
  void Init(const Images& im, const SourceKey& k) {
    effect.vt = im.dx + kEffectVtbl;
    *reinterpret_cast<void**>(sh) = im.dx + kShaderVtbl;
    *reinterpret_cast<void**>(sh + 0x50) = &effect;
    path = new (sh + 0x58) std::string(k.path);
    key = new (sh + 0x78) std::string(k.key);
    defs.assign(0xa0 * k.defines.size(), 0);
    for (size_t i = 0; i < k.defines.size(); ++i) {
      uint8_t* e = defs.data() + i * 0xa0;
      strcpy_s(reinterpret_cast<char*>(e + 8), 0x48, k.defines[i].name.c_str());
      strcpy_s(reinterpret_cast<char*>(e + 0x58), 0x48, k.defines[i].value.c_str());
      *reinterpret_cast<uint64_t*>(e + 0x50) = k.defines[i].keyValue ? 1 : 0;
    }
    *reinterpret_cast<uint8_t**>(sh + 0x98) = defs.data();
    *reinterpret_cast<uint8_t**>(sh + 0xa0) = defs.data() + defs.size();
    *reinterpret_cast<uint8_t**>(sh + 0xa8) = defs.data() + defs.size();
    new (recs + 0x50 * 0) std::string("normal_cf");
    new (recs + 0x50 * 1) std::string("normal_cockpit_cf");
    new (recs + 0x50 * 2) std::string("lockon_shadows");
    for (int i = 0; i < 3; ++i) *reinterpret_cast<void**>(recs + 0x50 * i + 0x20) = &g_tech[i];
    *reinterpret_cast<uint8_t**>(sh + 0xb0) = recs;
    *reinterpret_cast<uint8_t**>(sh + 0xb8) = recs + sizeof(recs);
    *reinterpret_cast<void**>(mat) = im.ng + kModelMatVtbl;
    *reinterpret_cast<void**>(mat + 0x28) = props;
    *reinterpret_cast<void**>(mat + 0x30) = sh;
    *reinterpret_cast<uint64_t*>(mat + 0x1d8) = 1;
    *reinterpret_cast<uint64_t*>(mat + 0x1e0) = 2;
    *reinterpret_cast<int64_t*>(mat + 0x220) = -1;
    *reinterpret_cast<void**>(item + 0x10) = mat;
    *reinterpret_cast<void**>(item + 0x50) = flat;
    *reinterpret_cast<float*>(flat + 0x18) = 0.5f;
    *reinterpret_cast<void**>(r + 0x10) = item;
  }
  void Pass(uint32_t p) { *reinterpret_cast<uint32_t*>(r + 0x60) = p; }
  ~Fake() {
    if (path) path->~basic_string();
    if (key) key->~basic_string();
    if (path)
      for (int i = 0; i < 3; ++i) reinterpret_cast<std::string*>(recs + 0x50 * i)->~basic_string();
  }
};

bool MakeVs(const Compiler& cc, ID3D11Device* dev) {
  std::vector<uint8_t> b;
  if (!CompileSrc(cc, "float4 vs(float4 p : POSITION) : SV_Position { return p; }", "vs", "vs_5_0", b)) return false;
  g_dev = dev;
  return SUCCEEDED(dev->CreateVertexShader(b.data(), b.size(), nullptr, &g_vs));
}

void RuntimePath(const Compiler& cc, const std::wstring& shaders, ID3D11Device* dev) {
  if (!dev || !MakeVs(cc, dev)) {
    printf("SKIP gbuffer inst: no D3D11 device for the runtime path\n");
    return;
  }
  Images im;
  const SourceKey k = GbKeys()[1];
  auto* f = new Fake;
  f->Init(im, k);
  uint8_t* const savedDx = g_dx;
  const std::wstring savedRoot = g_root;
  const bool savedStop = shadowinst::g_stop.load();
  MapEntry* const savedMap = g_map;
  g_dx = im.dx;
  g_root = shaders;
  g_compiler = cc;
  shadowinst::g_stop = false;
  g_map = new MapEntry[kMapSize];
  g_mapUsed = 0;
  for (auto& u : g_mapUsedKind) u = 0;
  g_deviceChecked = false;
  g_device = nullptr;

  auto* s = new Snapshot;
  memset(s, 0, sizeof(*s));
  s->kind = kKindGb;
  const char* why = SnapshotGuarded(f->mat, f->sh, *s);
  Check(!why && s->tech[0] == 1 && s->tech[1] == 2 && !strcmp(s->techName[0], "normal_cf") &&
            !strcmp(s->techName[1], "normal_cockpit_cf") && s->passes[0] == 5 && s->passes[1] == 9 &&
            s->dcsVs[0][4] == g_vs && s->dcsVs[1][8] == g_vs,
        "gbuffer inst: snapshot reads normal (mat+0x1d8) and normal_cockpit (mat+0x1e0), 5 and 9 passes");
  if (why) printf("     snapshot: %s\n", why);
  CheckDevice(*s);
  KeyResult r;
  r.kind = kKindGb;
  CompileKey(*s, r);
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    PublishLocked(*s, r);
  }
  bool all = r.ok && r.kind == kKindGb && r.vs.size() == 1 && r.vs[0].vs && r.techs.size() == 2;
  for (uint32_t p = 0; p < 5; ++p) {
    void* dcs = nullptr;
    all &= FindVsEx(f->sh, 1, p, &dcs) == r.vs[0].vs && dcs == g_vs && PrevTransformUnused(f->sh, 1, p);
  }
  for (uint32_t p = 0; p < 9; ++p) all &= FindVs(f->sh, 2, p) == r.vs[0].vs && PrevTransformUnused(f->sh, 2, p);
  all &= !FindVs(f->sh, 1, 5) && !FindVs(f->sh, 2, 9) && !PrevTransformUnused(f->sh, 1, 5) &&
         g_mapUsedKind[kKindGb].load() == 14 && g_mapUsedKind[kKindShadow].load() == 0;
  if (!r.ok) printf("     compile: %s\n", r.why.c_str());
  Check(all,
        "gbuffer inst: key compiled, one instanced model_vs for all 14 passes, FindVsEx and PrevTransformUnused per "
        "(shader, tech, pass)");
  uint64_t m[2] = {};
  const bool got = CbUsedDwords(f->sh, 1, 0, m);
  // posStructOffset dword 63 (VS), specMapValue 0x9c (PS, SPECULAR_UV), nothing of the matrix.
  Check(got && (m[0] >> 63 & 1) && (m[0] >> (0x9c / 4) & 1) && !(m[0] & (0xffffull << 8)),
        "gbuffer inst: CbUsedDwords gives the CB dwords read by the pass (posStructOffset, specMapValue; not the "
        "matrix)");
  // R14 lead 3 counter: the per-pass read set query, then the per-draw count.
  {
    const int savedState = g_state.load();
    const bool savedSelOk = g_selOk;
    void* const savedShaderVtbl = g_shaderVtbl;
    void* const savedModelVtbl = g_modelVtbl;
    g_state = 1;
    g_selOk = false;
    g_shaderVtbl = im.dx + kShaderVtbl;
    g_modelVtbl = im.ng + kModelMatVtbl;
    std::vector<std::string> bound, vars;
    std::string rwhy, rkey;
    const int st = GbPassReadTextures(f->sh, 1, 0, &bound, &vars, &rwhy, &rkey);
    Check(st == shadowtex::kReadsReady && !r.techs.empty() && !r.techs[0].gates.empty() &&
              bound == r.techs[0].gates[0].reads && !bound.empty() && vars == r.fxVars && !vars.empty() &&
              rkey == k.key && GbPassReadTextures(f->sh, 1, 7, nullptr, nullptr, nullptr, nullptr) == shadowtex::kReadsPending,
          "gbuffer tex: GbPassReadTextures returns (a)'s bindings of one (shader, technique, pass) and the variables");
    // Live parameter records: every variable of (a), in order. Handles: one
    // read by normal_cf P0, one not, one -1.
    std::vector<uint8_t> recs(0x50 * (vars.size() ? vars.size() : 1));
    int readIdx = -1, unreadIdx = -1;
    for (size_t i = 0; i < vars.size(); ++i) {
      *reinterpret_cast<const char**>(recs.data() + i * 0x50 + 0x30) = vars[i].c_str();
      bool read = false;
      for (const std::string& b : bound) read |= shadowtex::NameRefers(vars[i].c_str(), b.c_str());
      if (read && readIdx < 0) readIdx = static_cast<int>(i);
      if (!read && unreadIdx < 0) unreadIdx = static_cast<int>(i);
    }
    *reinterpret_cast<uint8_t**>(f->sh + 0xc8) = recs.data();
    *reinterpret_cast<uint8_t**>(f->sh + 0xd0) = recs.data() + 0x50 * vars.size();
    *reinterpret_cast<int64_t*>(f->mat + 0x240) = readIdx;
    *reinterpret_cast<int64_t*>(f->mat + 0x248) = unreadIdx;
    *reinterpret_cast<int64_t*>(f->mat + 0x250) = -1;
    *reinterpret_cast<uint32_t*>(f->mat + 0x2d8) = 3;
    f->Pass(1);
    TexCountClear();
    TexCountNote(f->r);
    TexCountNote(f->r);
    const bool counted = readIdx >= 0 && unreadIdx >= 0 && g_tc.draws == 2 && g_tc.sets == 4 && g_tc.unread == 2 &&
                         g_tc.unknownSets == 0 && g_tcUnreadByName.size() == 1 &&
                         *g_tcUnreadByName.begin()->first == vars[unreadIdx] && g_tcUnreadByName.begin()->second == 2 &&
                         g_tcMasks.size() == 1;
    printf("     gb tex: read %s, unread %s; draws %llu, sets %llu, unread %llu, unknown %llu\n",
           readIdx >= 0 ? vars[readIdx].c_str() : "-", unreadIdx >= 0 ? vars[unreadIdx].c_str() : "-",
           static_cast<unsigned long long>(g_tc.draws), static_cast<unsigned long long>(g_tc.sets),
           static_cast<unsigned long long>(g_tc.unread), static_cast<unsigned long long>(g_tc.unknownSets));
    Check(counted, "gbuffer tex: a G-buffer draw counts its sets; only the variable P0 does not read is unread, by name");
    // Not counted: cockpit, pass 2, transparent.
    f->r[0x64] = 1;
    TexCountNote(f->r);
    f->r[0x64] = 0;
    f->Pass(2);
    TexCountNote(f->r);
    f->Pass(1);
    f->props[0x33] = 1;
    TexCountNote(f->r);
    f->props[0x33] = 0;
    Check(g_tc.draws == 2 && g_tc.cockpit == 1, "gbuffer tex: cockpit, pass-2 and transparent draws are not counted");
    // A shader without a published entry: its sets are unknown.
    auto* f2 = new Fake;
    f2->Init(im, k);
    *reinterpret_cast<int64_t*>(f2->mat + 0x240) = 0;
    *reinterpret_cast<uint32_t*>(f2->mat + 0x2d8) = 1;
    f2->Pass(1);
    TexCountNote(f2->r);
    // A live record that is not a variable of (a): the mask is unknown.
    std::vector<uint8_t> odd = recs;
    static const char kOdd[] = "qvNotInTheEffect";
    *reinterpret_cast<const char**>(odd.data() + 0x30) = kOdd;
    *reinterpret_cast<uint8_t**>(f->sh + 0xc8) = odd.data();
    *reinterpret_cast<uint8_t**>(f->sh + 0xd0) = odd.data() + 0x50 * vars.size();
    TexCountNote(f->r);
    Check(g_tc.draws == 4 && g_tc.sets == 7 && g_tc.unread == 2 && g_tc.unknownSets == 3 && g_tc.unknownDraws == 2 &&
              g_tcUnknownWhy.size() == 2 && g_tcOldMasks.size() == 1,
          "gbuffer tex: sets of an unpublished key, or of records that are not variables of (a), are unknown");
    TexCountClear();
    *reinterpret_cast<uint8_t**>(f->sh + 0xc8) = nullptr;
    *reinterpret_cast<uint8_t**>(f->sh + 0xd0) = nullptr;
    *reinterpret_cast<uint32_t*>(f->mat + 0x2d8) = 0;
    delete f2;
    g_state = savedState;
    g_selOk = savedSelOk;
    g_shaderVtbl = savedShaderVtbl;
    g_modelVtbl = savedModelVtbl;
  }
  LogResult(r, 1);
  Check(r.line.find("UNUSED") != std::string::npos && r.line.find("dataflow") != std::string::npos,
        "gbuffer inst: one log line per key with checks and gate");
  // No cockpit technique: only normal* is compiled.
  *reinterpret_cast<uint64_t*>(f->mat + 0x1e0) = 77;
  auto* s2 = new Snapshot;
  memset(s2, 0, sizeof(*s2));
  s2->kind = kKindGb;
  const char* why2 = SnapshotGuarded(f->mat, f->sh, *s2);
  KeyResult r2;
  if (!why2) CompileKey(*s2, r2);
  Check(!why2 && s2->passes[1] == 0 && r2.ok && r2.techs.size() == 1,
        "gbuffer inst: a material without a cockpit technique compiles normal* only");
  // A non-normal technique is rejected.
  *reinterpret_cast<uint64_t*>(f->mat + 0x1e0) = 2;
  *reinterpret_cast<uint64_t*>(f->mat + 0x1d8) = 3;
  auto* s3 = new Snapshot;
  memset(s3, 0, sizeof(*s3));
  s3->kind = kKindGb;
  KeyResult r3;
  if (!SnapshotGuarded(f->mat, f->sh, *s3)) CompileKey(*s3, r3);
  Check(!r3.ok && r3.why.find("not a normal*") != std::string::npos && r3.msA == 0,
        "gbuffer inst: techniques other than normal* / normal_cockpit* are rejected before compiling");
  for (PassVs& p : r.vs)
    if (p.vs) p.vs->Release();
  for (PassVs& p : r2.vs)
    if (p.vs) p.vs->Release();
  if (g_device) g_device->Release();
  g_device = nullptr;
  delete s;
  delete s2;
  delete s3;
  delete[] g_map;
  g_map = savedMap;
  for (auto& u : g_mapUsedKind) u = 0;
  g_dx = savedDx;
  g_root = savedRoot;
  shadowinst::g_stop = savedStop;
  delete f;
  g_vs->Release();
  g_vs = nullptr;
}

// Called by sitest::Lifecycle after its shadow checks and before Shutdown:
// InstallGb on the analysed binaries, the observer fed with fake
// SceneRenderables during SuitePhaseGb, totals.
void LifecycleGb(const Compiler& cc, const std::wstring& shaders, ID3D11Device* dev) {
  if (!dev || !MakeVs(cc, dev)) {
    printf("SKIP gbuffer inst: lifecycle needs a D3D11 device\n");
    return;
  }
  const bool installed = InstallGb();
  Check(installed && gbcount::g_observer.load() == &GbObserver && g_selOk && g_flirThreshold == 1.0f,
        "gbuffer inst: InstallGb registers the observer on gb_count's SceneRenderable hook; selection code verified");
  if (!installed) return;
  Images im;
  uint8_t* const dx0 = g_dx;
  void* const shv0 = g_shaderVtbl;
  void* const mv0 = g_modelVtbl;
  g_dx = im.dx;
  g_shaderVtbl = im.dx + kShaderVtbl;
  g_modelVtbl = im.ng + kModelMatVtbl;
  g_root = shaders;
  auto* a = new Fake;
  auto* b = new Fake;  // same key, another DX11Shader
  auto* c = new Fake;  // only seen in pass 8: not a G-buffer key
  a->Init(im, GbKeys()[0]);
  b->Init(im, GbKeys()[0]);
  c->Init(im, GbKeys()[2]);
  std::atomic<bool> abort{false};
  // The feed below must land inside the phase's collection window: 2 s, so a
  // test thread preempted on a loaded machine still feeds in time (the window
  // is a plain sleep in SuitePhaseGb; 400 ms was enough only when idle).
  std::thread phase([&] { SuitePhaseGb(2000, abort, nullptr); });
  for (const ULONGLONG until = GetTickCount64() + 30000; !g_collectGb.load() && GetTickCount64() < until;) Sleep(1);
  auto feed = [](Fake* f, uint32_t pass) {
    f->Pass(pass);
    gbcount::g_observer.load()(f->r, nullptr);
  };
  for (int rep = 0; rep < 2; ++rep) {
    feed(a, 1);
    feed(a, 2);
    feed(b, 1);
    feed(c, 8);
  }
  phase.join();
  const Totals t = Snap(kKindGb);
  const bool sel = g_sel[1][0][0].load() == 4 && g_sel[2][0][1].load() == 2 && g_sel[8][5][1].load() == 2;
  Check(sel, "gbuffer inst: observer counts the selected technique passes (pass 1 -> normal P0, 2 -> P1, 8 -> "
             "flat_shadow FLIR)");
  printf("     gb lifecycle: seen %u, keys %u, ok %u, map %u, VS created %u\n", t.seen, t.keys, t.ok, t.map, t.created);
  Check(t.seen == 2 && t.keys == 1 && t.ok == 1 && t.done == 1 && t.map == 28 && t.created == 1 &&
            FindVs(a->sh, 1, 0) && FindVs(a->sh, 1, 0) == FindVs(b->sh, 2, 8) && !FindVs(c->sh, 1, 0),
        "gbuffer inst: passes 1-2 queue each shader once, one compile per key, map has every (shader, tech, pass)");
  g_dx = dx0;
  g_shaderVtbl = shv0;
  g_modelVtbl = mv0;
  delete a;
  delete b;
  delete c;
  g_vs->Release();
  g_vs = nullptr;
}

void Run() {
  Compiler cc;
  if (!cc.Load() || !cc.disassemble) {
    printf("SKIP gbuffer inst: System32 d3dcompiler_47.dll (with D3DDisassemble) not available\n");
    return;
  }
  const std::wstring shaders = sitest::TempRoot() + L"shaders\\";
  Synthetic(cc);
  Selection();
  // Build checks on the analysed NGModel.dll: the mirrored selection code.
  {
    auto* ng = reinterpret_cast<uint8_t*>(dcsinstall::LoadBinary(L"NGModel.dll"));
    if (!dcsinstall::Found()) {
      dcsinstall::Skip("gbuffer inst build checks");
    } else if (!ng) {
      printf("SKIP gbuffer inst: NGModel.dll not found\n");
    } else {
      auto** matVt = reinterpret_cast<void**>(ng + kModelMatVtbl);
      float thr = 0;
      memcpy(&thr, ng + kFlirThreshold, 4);
      Check(reinterpret_cast<uint8_t*>(SlotOriginal(&matVt[4])) == ng + kModelDraw &&
                shadowtex::CodeIs(ng, kSceneRenderable, kSceneRenderableEnd, kSceneRenderableHash) &&
                shadowtex::CodeIs(ng, kSelCode, kSelCodeEnd, kSelCodeHash) &&
                shadowtex::CodeIs(ng, kDrawDispatch, kDrawDispatchEnd, kDrawDispatchHash) &&
                shadowtex::CodeIs(ng, kDrawTable, kDrawTableEnd, kDrawTableHash) && thr == 1.0f,
            "gbuffer inst: build checks pass (ModelMaterialMT vt[4], SceneRenderable vt[1], selection code, jump "
            "table, FLIR threshold)");
    }
  }
  if (GetFileAttributesW((shaders + L"model\\functions\\vertex_shader.hlsl").c_str()) == INVALID_FILE_ATTRIBUTES) {
    printf("SKIP gbuffer inst: shader source copy not found\n");
    return;
  }
  ID3D11Device* dev = nullptr;
  D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
  if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &fl, 1, D3D11_SDK_VERSION, &dev,
                               nullptr, nullptr)))
    D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, &fl, 1, D3D11_SDK_VERSION, &dev, nullptr, nullptr);
  RealSources(cc, shaders, dev);
  RuntimePath(cc, shaders, dev);
  if (dev) dev->Release();
}

}  // namespace gbtest
