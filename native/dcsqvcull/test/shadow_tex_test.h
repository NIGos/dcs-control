// Offline tests of the shadow texture skip's masks (shadow_tex.h) built from
// shadow_inst.h's (a) compiles: def_material.fx's shadow techniques compiled
// from the Bazar copies in the temp folder (shadow_inst_test.h's copy), the
// read set checked against D3DReflect of every shader of every pass, the
// render-thread query and mask build on a fake DX11Shader, and how the skip
// composes with shadow batching's grouping.
// Included from test_main.cpp after shadow_inst_test.h.
#pragma once

namespace sttest {
using namespace shadowinst;

const char* const kShadowTechs[2] = {"lockon_shadows", "lockon_shadows_transparent"};

// Live parameter records as DCS lays them out (0x50 bytes, name at +0x30),
// one per variable name.
struct Records {
  std::vector<std::string> names;
  std::vector<uint8_t> bytes;
  void Build(const std::vector<std::string>& n) {
    names = n;
    bytes.assign(names.size() * 0x50 + 0x50, 0);
    for (size_t i = 0; i < names.size(); ++i)
      *reinterpret_cast<const char**>(bytes.data() + i * 0x50 + 0x30) = names[i].c_str();
  }
  uint8_t* begin() { return bytes.data(); }
  uint8_t* end() { return bytes.data() + names.size() * 0x50; }
};

// D3DReflect's bound resources of every shader of every pass of a technique,
// with the stages seen (bit = D3D11_SHVER type).
bool OracleBound(const Compiler& cc, const FxEffect& fx, const char* tech, std::set<std::string>& out,
                 uint32_t& stages) {
  const FxTechnique* t = FindTechnique(fx, tech);
  if (!t) return false;
  for (const FxPass& p : t->passes)
    for (const FxAssign& a : p.assigns) {
      if (!a.shader.p) continue;
      ID3D11ShaderReflection* r = nullptr;
      if (FAILED(cc.reflect(a.shader.p, a.shader.n, __uuidof(ID3D11ShaderReflection), reinterpret_cast<void**>(&r))))
        return false;
      D3D11_SHADER_DESC sd = {};
      r->GetDesc(&sd);
      stages |= 1u << D3D11_SHVER_GET_TYPE(sd.Version);
      for (UINT i = 0; i < sd.BoundResources; ++i) {
        D3D11_SHADER_INPUT_BIND_DESC bd = {};
        r->GetResourceBindingDesc(i, &bd);
        out.insert(bd.Name);
      }
      r->Release();
    }
  return true;
}

bool IsTexture(const FxObjectVar& v) { return v.typeName.compare(0, 7, "Texture") == 0; }

// ---- Fake live objects for shadowtex::Lookup (vtables at their dx11backend
// offsets in a fake image, like shadow_inst_test.h's) ----
struct LivePass {
  void* vt;
  uint8_t pad[8];
  uint32_t assignments;  // +0x10
  uint32_t pad2;
  uint8_t* assign;       // +0x18
};
LivePass g_livePass;
uint8_t g_liveAssign[0x38 * 2];
struct LiveTech {
  void* vt;
};
LiveTech g_liveTech;
long __fastcall LiveTechDesc(void*, void* out) {
  struct D {
    const char* name;
    uint32_t passes, annotations;
  }* d = static_cast<D*>(out);
  d->name = "t";
  d->passes = 1;
  d->annotations = 0;
  return 0;
}
void* __fastcall LiveGetPass(void*, uint32_t i) { return i == 0 ? &g_livePass : nullptr; }
bool __fastcall LivePassValid(void*) { return true; }

// A DX11Shader: +0x50 effect, +0xb0 technique records (std::string name,
// +0x20 object), +0xc8 parameter records.
struct LiveShader {
  uint8_t sh[0x100] = {};
  uint8_t techs[0x50 * 2] = {};
  uint8_t effect[16] = {};
  void Init(Records& recs) {
    *reinterpret_cast<void**>(sh + 0x50) = effect;
    new (techs) std::string(kShadowTechs[0]);
    new (techs + 0x50) std::string(kShadowTechs[1]);
    *reinterpret_cast<void**>(techs + 0x20) = &g_liveTech;
    *reinterpret_cast<void**>(techs + 0x50 + 0x20) = &g_liveTech;
    *reinterpret_cast<uint8_t**>(sh + 0xb0) = techs;
    *reinterpret_cast<uint8_t**>(sh + 0xb8) = techs + sizeof(techs);
    *reinterpret_cast<uint8_t**>(sh + 0xc8) = recs.begin();
    *reinterpret_cast<uint8_t**>(sh + 0xd0) = recs.end();
  }
  ~LiveShader() {
    reinterpret_cast<std::string*>(techs)->~basic_string();
    reinterpret_cast<std::string*>(techs + 0x50)->~basic_string();
  }
};

void Run() {
  Compiler cc;
  if (!cc.Load()) {
    QvSkip("shadow tex masks: System32 d3dcompiler_47.dll not available\n");
    return;
  }
  // Bazar copies (shadow_inst_test.h's temp folder; copied again if absent).
  const std::wstring root = sitest::TempRoot();
  const std::wstring shaders = root + L"shaders\\";
  if (GetFileAttributesW((shaders + L"model\\def_material.fx").c_str()) == INVALID_FILE_ATTRIBUTES) {
    CreateDirectoryW(root.c_str(), nullptr);
    sitest::CopyTree(sitest::kShaderDir, shaders, {L".fx", L".hlsl", L".h"}, L"fxo");
  }
  if (GetFileAttributesW((shaders + L"model\\def_material.fx").c_str()) == INVALID_FILE_ATTRIBUTES) {
    QvSkip("shadow tex masks: shader sources not found\n");
    return;
  }

  // ---- 1. Read set of (a) vs D3DReflect; mask vs the textures the passes bind ----
  const std::vector<SourceKey> keys = sitest::ModelKeys();
  int analysed = 0, exact = 0, withSkips = 0, withKeeps = 0, parserSame = 0;
  for (const SourceKey& k : keys) {
    std::vector<uint8_t> fxA;
    std::string err;
    FxEffect fx;
    if (!CompileEffect(cc, shaders, k, false, kVarRef, fxA, err) || !ParseFx5(fxA.data(), fxA.size(), fx, err)) {
      printf("     %s: (a) failed: %s\n", k.key.c_str(), err.c_str());
      continue;
    }
    uint32_t live[2] = {};
    for (int t = 0; t < 2; ++t) {
      const FxTechnique* ft = FindTechnique(fx, kShadowTechs[t]);
      live[t] = ft ? static_cast<uint32_t>(ft->passes.size()) : 0;
    }
    std::vector<TechReads> reads;
    std::vector<std::string> vars;
    AnalyseShadowReads(fx, kShadowTechs, live, reads, vars);
    if (reads.size() != 2 || !reads[0].ok || !reads[1].ok) {
      printf("     %s: reads not analysed: %s\n", k.key.c_str(),
             reads.empty() ? "?" : (!reads[0].ok ? reads[0].why : reads[1].why).c_str());
      continue;
    }
    ++analysed;
    // Our RDEF names per technique == D3DReflect's.
    std::set<std::string> oracle;
    uint32_t stages = 0;
    bool same = true;
    for (int t = 0; t < 2; ++t) {
      std::set<std::string> o;
      if (!OracleBound(cc, fx, kShadowTechs[t], o, stages)) same = false;
      same &= std::set<std::string>(reads[t].bound.begin(), reads[t].bound.end()) == o;
      oracle.insert(o.begin(), o.end());
    }
    parserSame += same;
    // Records for every variable of the effect, as DCS's DX11Shader has them.
    Records recs;
    recs.Build(vars);
    shadowtex::MaskEntry e;
    e.recBegin = recs.begin();
    e.recEnd = recs.end();
    e.recCount = static_cast<uint32_t>(recs.names.size());
    e.state = 1;
    std::vector<const char*> b, v;
    for (const TechReads& tr : reads)
      for (const std::string& n : tr.bound) b.push_back(n.c_str());
    for (const std::string& n : vars) v.push_back(n.c_str());
    uint32_t bad = ~0u;
    const char* why = shadowtex::MapRecords(e, b.data(), static_cast<int>(b.size()), v.data(), static_cast<int>(v.size()),
                                            &bad);
    // Every texture variable: skipped exactly when no shader of either shadow
    // technique binds it (D3DReflect, NameRefers rules).
    bool keyExact = !why;
    std::string keptList, skipList;
    int skips = 0, keeps = 0;
    for (const FxObjectVar& ov : fx.vars) {
      if (!IsTexture(ov)) continue;
      uint32_t idx = 0;
      while (idx < recs.names.size() && recs.names[idx] != ov.name) ++idx;
      bool bound = false;
      for (const std::string& o : oracle) bound |= shadowtex::NameRefers(ov.name.c_str(), o.c_str());
      const bool skip = shadowtex::Skippable(e, idx);
      keyExact &= idx < recs.names.size() && skip == !bound;
      (skip ? skipList : keptList) += (skip ? skipList : keptList).empty() ? ov.name : " " + ov.name;
      (skip ? skips : keeps) += 1;
    }
    exact += keyExact;
    withSkips += skips > 0;
    withKeeps += keeps > 0;
    printf("     %-58.58s stages 0x%x, %zu vars: textures kept [%s], skipped [%s]%s\n", k.key.c_str(), stages,
           vars.size(), keptList.c_str(), skipList.c_str(), keyExact ? "" : "  <-- MISMATCH");
  }
  Check(analysed == static_cast<int>(keys.size()) && parserSame == analysed,
        "shadow tex masks: (a)'s shadow technique read sets equal D3DReflect's bindings of every pass shader "
        "(5 def_material keys)");
  Check(analysed > 0 && exact == analysed,
        "shadow tex masks: the mask skips exactly the texture variables no shadow PS/VS binds");
  Check(withSkips == analysed && withKeeps > 0, "shadow tex masks: every key skips some textures, some keep textures");

  // ---- 2. Runtime: CompileKey -> texture-read map -> shadowtex::Lookup ----
  {
    const int savedState = shadowinst::g_state.load();
    const bool savedStop = shadowinst::g_stop.load();
    TexEntry* const savedTexMap = g_texMap;
    const std::wstring savedRoot = g_root;
    const Compiler savedCompiler = g_compiler;
    uint8_t* const savedTexDx = shadowtex::g_dx;
    shadowtex::MaskCache* const savedCache = shadowtex::g_cache;
    shadowinst::g_state = 1;
    shadowinst::g_stop = false;
    g_texMap = new TexEntry[kTexMapSize];
    g_texMapUsed = 0;
    g_root = shaders;
    g_compiler = cc;
    std::vector<uint8_t> image(0xc1000);
    uint8_t* dx = image.data();
    reinterpret_cast<void**>(dx + shadowtex::kTechVtbl)[4] = reinterpret_cast<void*>(&LiveTechDesc);
    reinterpret_cast<void**>(dx + shadowtex::kTechVtbl)[7] = reinterpret_cast<void*>(&LiveGetPass);
    reinterpret_cast<void**>(dx + shadowtex::kPassVtbl)[3] = reinterpret_cast<void*>(&LivePassValid);
    g_liveTech.vt = dx + shadowtex::kTechVtbl;
    g_livePass.vt = dx + shadowtex::kPassVtbl;
    g_livePass.assignments = 1;
    g_livePass.assign = g_liveAssign;
    *reinterpret_cast<uint32_t*>(g_liveAssign + 4) = 1;  // numeric
    shadowtex::g_dx = dx;
    shadowtex::g_cache = new shadowtex::MaskCache;

    const SourceKey& k = keys[1];  // alpha-tested
    auto* s = new Snapshot;
    memset(s, 0, sizeof(*s));
    s->kind = kKindShadow;
    strcpy_s(s->path, k.path.c_str());
    strcpy_s(s->key, k.key.c_str());
    s->defineCount = static_cast<uint32_t>(k.defines.size());
    for (size_t i = 0; i < k.defines.size(); ++i) {
      strcpy_s(s->defines[i].name, k.defines[i].name.c_str());
      strcpy_s(s->defines[i].value, k.defines[i].value.c_str());
      s->defines[i].keyValue = k.defines[i].keyValue;
    }
    for (int t = 0; t < 2; ++t) {
      strcpy_s(s->techName[t], kShadowTechs[t]);
      s->tech[t] = static_cast<uint64_t>(t + 1);
      s->passes[t] = 1;
    }
    auto* r = new KeyResult;
    CompileKey(*s, *r);
    // The fake DX11Shader: records for every variable of (a).
    Records recs;
    recs.Build(r->fxVars);
    auto* live = new LiveShader;
    live->Init(recs);
    s->shader = live->sh;
    s->effect = *reinterpret_cast<void**>(live->sh + 0x50);
    s->techBegin = *reinterpret_cast<void**>(live->sh + 0xb0);
    uint8_t* sh = live->sh;

    bool pending = false;
    const size_t used0 = shadowtex::g_cache->Used();
    const shadowtex::MaskEntry* m0 = shadowtex::Lookup(sh, 1, 2, &pending);
    Check(r->reads.size() == 2 && r->reads[0].ok && r->reads[1].ok && !m0 && pending &&
              shadowtex::g_cache->Used() == used0,
          "shadow tex masks: before the key is published the shader has no mask (pending, nothing cached)");
    {
      std::lock_guard<std::mutex> lock(g_mutex);
      r->done = true;
      PublishTexLocked(*s, *r);
    }
    std::vector<std::string> bound, vars;
    std::string why, key;
    const int st = ShadowReadTextures(sh, 2, &bound, &vars, &why, &key);
    Check(st == shadowtex::kReadsReady && bound == r->reads[1].bound && vars == r->fxVars && key == k.key &&
              ShadowReadTextures(sh, 3, nullptr, nullptr, nullptr, nullptr) == shadowtex::kReadsPending,
          "shadow tex masks: ShadowReadTextures returns (a)'s bindings per (shader, technique handle)");
    const shadowtex::MaskEntry* m = shadowtex::Lookup(sh, 1, 2, &pending);
    // Expected: a texture record is read iff some binding of either technique names it.
    bool maskOk = m && m->state == 1 && !pending;
    int skipped = 0;
    for (uint32_t i = 0; m && i < recs.names.size(); ++i) {
      bool read = false;
      for (const TechReads& tr : r->reads)
        for (const std::string& n : tr.bound) read |= shadowtex::NameRefers(recs.names[i].c_str(), n.c_str());
      maskOk &= shadowtex::Skippable(*m, i) == !read;
      skipped += !read;
    }
    Check(maskOk && skipped > 0 && shadowtex::Lookup(sh, 1, 2, &pending) == m,
          "shadow tex masks: Lookup builds the mask from the published reads, live records matched by name, "
          "then serves it from the cache");
    // A live record that is not a variable of (a): the shader keeps every set.
    {
      Records odd;
      std::vector<std::string> n = r->fxVars;
      n.push_back("qvNotInTheEffect");
      odd.Build(n);
      *reinterpret_cast<uint8_t**>(sh + 0xc8) = odd.begin();
      *reinterpret_cast<uint8_t**>(sh + 0xd0) = odd.end();
      const shadowtex::MaskEntry* mo = shadowtex::Lookup(sh, 1, 2, &pending);
      // A live pass selecting an object by variable: kept as well.
      Records two;
      two.Build(r->fxVars);
      two.bytes.push_back(0);  // a different record array (new fingerprint)
      *reinterpret_cast<uint8_t**>(sh + 0xc8) = two.begin();
      *reinterpret_cast<uint8_t**>(sh + 0xd0) = two.end();
      *reinterpret_cast<uint32_t*>(g_liveAssign + 4) = 5;
      const shadowtex::MaskEntry* mt = shadowtex::Lookup(sh, 1, 2, &pending);
      *reinterpret_cast<uint32_t*>(g_liveAssign + 4) = 1;
      Check(mo && mo->state == -1 && !shadowtex::Skippable(*mo, 0) && mt && mt->state == -1,
            "shadow tex masks: a live parameter missing from (a), or a live pass selecting objects, keeps every set");
      *reinterpret_cast<uint8_t**>(sh + 0xc8) = recs.begin();
      *reinterpret_cast<uint8_t**>(sh + 0xd0) = recs.end();
    }
    // A failed key is published as failed: kept whole, not asked again.
    {
      auto* s2 = new Snapshot(*s);
      LiveShader* live2 = new LiveShader;
      live2->Init(recs);
      s2->shader = live2->sh;
      s2->effect = *reinterpret_cast<void**>(live2->sh + 0x50);
      s2->techBegin = *reinterpret_cast<void**>(live2->sh + 0xb0);
      KeyResult failed;
      failed.why = "(a) compile failed: test";
      failed.done = true;
      {
        std::lock_guard<std::mutex> lock(g_mutex);
        PublishTexLocked(*s2, failed);
      }
      std::string w2;
      const int st2 = ShadowReadTextures(live2->sh, 1, nullptr, nullptr, &w2, nullptr);
      const shadowtex::MaskEntry* mf = shadowtex::Lookup(live2->sh, 1, 2, &pending);
      Check(st2 == shadowtex::kReadsFailed && w2.find("(a) compile failed") != std::string::npos && mf &&
                mf->state == -1 && !pending,
            "shadow tex masks: a key whose (a) failed maps its shaders to 'keep every set'");
      ForgetShader(live2->sh);
      delete live2;
      delete s2;
    }
    // Destructor hook path: forgotten shaders are pending again (and the
    // snapshot of a destroyed shader does not publish).
    ForgetShader(sh);
    const int afterForget = ShadowReadTextures(sh, 1, nullptr, nullptr, nullptr, nullptr);
    s->dead = true;
    {
      std::lock_guard<std::mutex> lock(g_mutex);
      PublishTexLocked(*s, *r);
    }
    const int deadPublish = ShadowReadTextures(sh, 1, nullptr, nullptr, nullptr, nullptr);
    Check(afterForget == shadowtex::kReadsPending && deadPublish == shadowtex::kReadsPending &&
              g_texForgotten.load() >= 2,
          "shadow tex masks: ForgetShader drops the shader's entries; a dead snapshot publishes nothing");
    // The query is off once shadow_inst stops.
    s->dead = false;
    {
      std::lock_guard<std::mutex> lock(g_mutex);
      PublishTexLocked(*s, *r);
    }
    shadowinst::g_stop = true;
    const int stopped = ShadowReadTextures(sh, 1, nullptr, nullptr, nullptr, nullptr);
    shadowinst::g_stop = false;
    Check(stopped == shadowtex::kReadsPending && ShadowReadTextures(sh, 1, nullptr, nullptr, nullptr, nullptr) ==
                                                     shadowtex::kReadsReady,
          "shadow tex masks: no reads are served while shadow_inst is stopping");
    for (PassVs& p : r->vs)
      if (p.vs) p.vs->Release();
    delete r;
    delete s;
    delete live;
    delete shadowtex::g_cache;
    shadowtex::g_cache = savedCache;
    shadowtex::g_dx = savedTexDx;
    delete[] g_texMap;
    g_texMap = savedTexMap;
    g_texMapUsed = 0;
    g_root = savedRoot;
    g_compiler = savedCompiler;
    shadowinst::g_state = savedState;
    shadowinst::g_stop = savedStop;
  }

  // ---- 3. Composition with shadow batching's groups ----
  // The planner groups casters by instcount::TextureKey, which hashes the
  // (aux, texture) pair of every handle != -1, read or skipped: two casters
  // that differ only in a texture the mask skips are not grouped, so a
  // skipped member's sets always equal its leader's.
  {
    using namespace stx;
    auto* a = new Scene;
    auto* c = new Scene;
    const int64_t handles[4] = {0, 1, -1, 2};
    for (bool& f : g_validFlag) f = false;
    Build(*a, 4, handles, 1, 0, 0);
    Build(*c, 4, handles, 1, 0, 0);
    // Build points both scenes' entries at their own textures; make them equal.
    memcpy(c->entries, a->entries, sizeof(a->entries));
    const auto savedGet = instcount::g_getTex;
    const auto savedValid = instcount::g_valid;
    instcount::g_getTex = &FakeGetTexture;
    instcount::g_valid = &FakeValid;
    const uint64_t ka = instcount::TextureKey(a->mat, a->item);
    const uint64_t kc = instcount::TextureKey(c->mat, c->item);
    // Entry 1 (handle 1) gets another texture; entry 2 (handle -1) too.
    *reinterpret_cast<void**>(c->entries + 1 * 24 + 8) = c->tex[7];
    const uint64_t kc1 = instcount::TextureKey(c->mat, c->item);
    memcpy(c->entries, a->entries, sizeof(a->entries));
    *reinterpret_cast<void**>(c->entries + 2 * 24 + 8) = c->tex[7];
    const uint64_t kc2 = instcount::TextureKey(c->mat, c->item);
    instcount::g_getTex = savedGet;
    instcount::g_valid = savedValid;
    Check(ka == kc && kc1 != ka && kc2 == ka,
          "shadow tex + batching: the group key covers every texture entry slot 5 sets (skipped ones too)");
    delete a;
    delete c;
  }
}

}  // namespace sttest
