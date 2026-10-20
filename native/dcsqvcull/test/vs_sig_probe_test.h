// R38 rank 2 gate G4 probe (vs_sig_probe.h): DXBC analysis (VS / output signature / FastGS / structured slots),
// the CreateVertexShader / CreateInputLayout hooks on a WARP device, the in-memory reference scan (pointer and
// inline blobs, the mainBuffer slot check, decoys), the draw census's triangle readback, the coverage summary, and
// the key off by default.
#pragma once

namespace vsigtest {

bool CompileStage(const char* src, const char* target, std::vector<uint8_t>& out) {
  if (!chainfast::LoadCompiler()) return false;
  ID3DBlob *code = nullptr, *err = nullptr;
  const HRESULT hr = chainfast::g_compile(src, strlen(src), "vsig", nullptr, nullptr, "main", target,
                                          D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &err);
  if (err) err->Release();
  if (FAILED(hr) || !code) return false;
  const uint8_t* p = static_cast<const uint8_t*>(code->GetBufferPointer());
  out.assign(p, p + code->GetBufferSize());
  code->Release();
  return true;
}

// An instancer-like VS: mainBuffer (structured, 64 bytes) at t3, indices at t4, five outputs.
const char kVs[] = R"(
struct Inst { uint4 a, b, c, d; };
StructuredBuffer<Inst> mainB : register(t3);
Buffer<uint> idx : register(t4);
struct O { float4 pos : SV_Position; float4 n : NORMAL; float2 uv : TEXCOORD0; float3 w : TEXCOORD1; uint id : TEXCOORD2; };
O main(uint v : SV_VertexID, uint i : SV_InstanceID) {
  Inst r = mainB[idx[i]];
  O o;
  o.pos = float4(asfloat(r.a.zw), asfloat(r.b.x), 1) + v;
  o.n = asfloat(r.c);
  o.uv = asfloat(r.d.xy);
  o.w = asfloat(r.b.yzw);
  o.id = r.a.x;
  return o;
}
)";
// Another VS reading a 64-byte structured buffer at t0 (a decoy for slot 3).
const char kVsOther[] = R"(
struct Inst { uint4 a, b, c, d; };
StructuredBuffer<Inst> m : register(t0);
float4 main(uint v : SV_VertexID) : SV_Position { return asfloat(m[v].a); }
)";
const char kPs[] = R"(float4 main(float4 p : SV_Position) : SV_Target { return p; })";

void Run() {
  using namespace vsig;
  std::vector<uint8_t> vs, vs2, ps;
  const bool built = CompileStage(kVs, "vs_5_0", vs) && CompileStage(kVsOther, "vs_5_0", vs2) && CompileStage(kPs, "ps_5_0", ps);
  Check(built, "vsig: test shaders compile");
  if (!built) return;
  // 1. analysis
  {
    Blob b, b2, bp;
    const bool a = Analyze(vs.data(), vs.size(), b), a2 = Analyze(vs2.data(), vs2.size(), b2),
               ap = Analyze(ps.data(), ps.size(), bp);
    std::vector<uint8_t> bad = vs;
    bad[24] ^= 0x40;  // declared size beyond the buffer
    Blob bb;
    const bool abad = Analyze(bad.data(), bad.size(), bb);
    char what[300];
    sprintf_s(what, "vsig: DXBC analysis: VS ok %d (signature %d elements, FastGS %d, structured t3 stride 64 %d, t0 %d), "
                    "other VS t0 %d, PS refused %d, a bad size refused %d",
              a ? 1 : 0, b.elems, b.gsOk ? 1 : 0, DeclaresMain(b, 3) ? 1 : 0, DeclaresMain(b, 0) ? 1 : 0,
              DeclaresMain(b2, 0) ? 1 : 0, ap ? 0 : 1, abad ? 0 : 1);
    Check(a && b.elems == 5 && b.gsOk && DeclaresMain(b, 3) && !DeclaresMain(b, 0) && a2 && DeclaresMain(b2, 0) && !ap &&
              !abad && memcmp(b.sum, vs.data() + 4, 16) == 0,
          what);
  }
  // 2. device hooks on WARP
  ID3D11Device* d = nullptr;
  ID3D11DeviceContext* c = nullptr;
  D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
  if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, &fl, 1, D3D11_SDK_VERSION, &d, nullptr, &c))) {
    printf("SKIP vsig: no WARP device\n");
    return;
  }
  ID3D11VertexShader *obj = nullptr, *obj2 = nullptr;
  {
    void** vt = *reinterpret_cast<void***>(d);
    void* before = vt[kSlotCvs];
    const uint64_t n0 = g_createdVs.load(), l0 = g_createdIl.load();
    InstallDeviceHooks(d);
    d->CreateVertexShader(vs.data(), vs.size(), nullptr, &obj);
    d->CreateVertexShader(vs2.data(), vs2.size(), nullptr, &obj2);
    ID3D11InputLayout* il = nullptr;
    D3D11_INPUT_ELEMENT_DESC e{"POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0};
    const char kIlVs[] = "float4 main(float4 p : POSITION) : SV_Position { return p; }";
    std::vector<uint8_t> ilvs;
    if (CompileStage(kIlVs, "vs_5_0", ilvs)) d->CreateInputLayout(&e, 1, ilvs.data(), ilvs.size(), &il);
    if (il) il->Release();
    bool rec = false;
    {
      std::lock_guard<std::mutex> lock(g_mu);
      auto it = g_created.find(obj);
      rec = it != g_created.end() && memcmp(it->second.sum, vs.data() + 4, 16) == 0 && it->second.elems == 5;
    }
    RemoveDeviceHooks();
    char what[260];
    sprintf_s(what, "vsig: CreateVertexShader hook records object -> bytecode (%d), %llu VS / %llu layouts counted, table "
                    "restored %d",
              rec ? 1 : 0, (unsigned long long)(g_createdVs.load() - n0), (unsigned long long)(g_createdIl.load() - l0),
              vt[kSlotCvs] == before ? 1 : 0);
    Check(rec && g_createdVs.load() - n0 == 2 && g_createdIl.load() - l0 == 1 && vt[kSlotCvs] == before, what);
  }
  // 3. memory scan: a DCS-like shader record {object, pad, blob pointer, size}, a decoy record (another object
  // with a VS that does not read slot 3) and an inline copy after a third object
  {
    struct ShaderRec {
      void* obj;
      uint64_t pad[3];
      const uint8_t* blob;
      uint64_t size;
    };
    std::vector<uint8_t>* heapBlob = new std::vector<uint8_t>(vs);
    std::vector<uint8_t>* heapBlob2 = new std::vector<uint8_t>(vs2);
    ShaderRec* r1 = new ShaderRec{obj, {}, heapBlob->data(), heapBlob->size()};
    ShaderRec* r2 = new ShaderRec{obj2, {}, heapBlob2->data(), heapBlob2->size()};
    ID3D11VertexShader* obj3 = nullptr;
    d->CreateVertexShader(vs.data(), vs.size(), nullptr, &obj3);
    uint8_t* inl = new uint8_t[64 + vs.size()];
    memset(inl, 0, 64);
    memcpy(inl, &obj3, 8);
    memcpy(inl + 64, vs.data(), vs.size());
    std::vector<std::pair<uintptr_t, int>> objs = {{reinterpret_cast<uintptr_t>(obj), 3},
                                                   {reinterpret_cast<uintptr_t>(obj2), 3},
                                                   {reinterpret_cast<uintptr_t>(obj3), 3}};
    std::sort(objs.begin(), objs.end());
    uint64_t scanned = 0;
    uint32_t ms = 0;
    auto out = ScanMemory(objs, 0, &scanned, &ms);
    const ScanOut &o1 = out[reinterpret_cast<uintptr_t>(obj)], &o2 = out[reinterpret_cast<uintptr_t>(obj2)],
                  &o3 = out[reinterpret_cast<uintptr_t>(obj3)];
    char what[400];
    sprintf_s(what, "vsig: memory scan (%.2f GB, %u ms): pointed-to blob found and declares t3 %d (refs %d, blobs %d, own %d at %lld); "
                    "decoy object's blob found but refused for t3 %d; inline copy found %d",
              scanned / 1073741824.0, ms, o1.ok ? 1 : 0, o1.refs, o1.blobs, o1.own ? 1 : 0, (long long)o1.dist, (o2.blobs > 0 && !o2.ok) ? 1 : 0, o3.ok ? 1 : 0);
    auto has = [&](const ScanOut& o, const std::vector<uint8_t>& code) {
      for (auto& x : o.tests)
        if (x == code) return true;
      return false;
    };
    // proximity alone can pick a heap neighbour's blob (the decoy record sits next to the first): the candidates
    // to test by output must hold the right program, and the decoy's own t0 program is never one
    Check(has(o1, vs) && has(o3, vs) && !has(o2, vs2) && o2.blobs > 0, what);
    delete r1;
    delete r2;
    delete heapBlob;
    delete heapBlob2;
    delete[] inl;
    obj3->Release();
  }
  // 3b. identification by output: DCS's object vs candidate programs through one stream-output GS
  {
    const char kVsDecoy[] = R"(
struct Inst { uint4 a, b, c, d; };
StructuredBuffer<Inst> mainB : register(t3);
Buffer<uint> idx : register(t4);
struct O { float4 pos : SV_Position; float4 n : NORMAL; float2 uv : TEXCOORD0; float3 w : TEXCOORD1; uint id : TEXCOORD2; };
O main(uint v : SV_VertexID, uint i : SV_InstanceID) {
  Inst r = mainB[idx[i]];
  O o;
  o.pos = float4(asfloat(r.a.zw), asfloat(r.b.x), 1) + v;
  o.n = asfloat(r.c) * 2;
  o.uv = asfloat(r.d.xy);
  o.w = asfloat(r.b.yzw);
  o.id = r.a.x;
  return o;
}
)";
    std::vector<uint8_t> decoy;
    CompileStage(kVsDecoy, "vs_5_0", decoy);
    std::vector<float> inst(64 * 16);
    for (size_t i = 0; i < inst.size(); ++i) inst[i] = 0.25f * float(i % 97) + 1.0f;
    std::vector<uint32_t> ix(64);
    for (uint32_t i = 0; i < 64; ++i) ix[i] = (i * 7) % 64;
    ID3D11Buffer* mb = piftest::MkBuf(d, 64 * 64, D3D11_BIND_SHADER_RESOURCE, D3D11_RESOURCE_MISC_BUFFER_STRUCTURED, 64, inst.data());
    ID3D11Buffer* ib = piftest::MkBuf(d, 64 * 4, D3D11_BIND_SHADER_RESOURCE, 0, 0, ix.data());
    ID3D11ShaderResourceView* sv[2] = {piftest::MkSrv(d, mb, 64, true), piftest::MkSrv(d, ib, 64, false)};
    c->VSSetShaderResources(3, 2, sv);
    c->VSSetShader(obj, nullptr, 0);
    c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    IdTest a, b;
    const bool pa = PrepareTest(vs, a), pb = PrepareTest(decoy, b);
    const bool ra = pa && RunTest(c, d, obj, a), rb = pb && RunTest(c, d, obj, b);
    c->Flush();
    for (int w = 0; w < 200 && (a.state == 1 || b.state == 1); ++w) {
      Sleep(5);
      PollTest(c, a);
      PollTest(c, b);
    }
    ID3D11VertexShader* bound = nullptr;
    c->VSGetShader(&bound, nullptr, nullptr);
    D3D11_PRIMITIVE_TOPOLOGY topo{};
    c->IAGetPrimitiveTopology(&topo);
    char what[260];
    sprintf_s(what, "vsig: identification by output: the object's own program MATCH %d (state %d), a program differing in "
                    "one output differs %d (state %d), DCS's VS and topology rebound %d",
              a.state == 2 ? 1 : 0, a.state, b.state == 3 ? 1 : 0, b.state, (bound == obj && topo == D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST) ? 1 : 0);
    Check(ra && rb && a.state == 2 && b.state == 3 && bound == obj && topo == D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST, what);
    pifilt::Rel(bound);
    a.Release();
    b.Release();
    ID3D11ShaderResourceView* ns[2] = {};
    c->VSSetShaderResources(3, 2, ns);
    c->VSSetShader(nullptr, nullptr, 0);
    sv[0]->Release();
    sv[1]->Release();
    mb->Release();
    ib->Release();
  }
  // 4. the draw census: triangles from the arguments read back at the run end
  {
    pifilt::SetDeviceForTest(d);
    std::vector<uint32_t> args(64 * 8, 0);
    args[2 * 8 + 0] = 300;  // entry 2: 100 triangles x 7 instances
    args[2 * 8 + 1] = 7;
    args[5 * 8 + 0] = 60;   // entry 5: 20 x 11
    args[5 * 8 + 1] = 11;
    ID3D11Buffer* ab = piftest::MkBuf(d, 64 * 32, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS,
                                      D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS, 0, args.data());
    ID3D11Buffer* mb = piftest::MkBuf(d, 64 * 64, D3D11_BIND_SHADER_RESOURCE, D3D11_RESOURCE_MISC_BUFFER_STRUCTURED, 64, nullptr);
    ID3D11ShaderResourceView* ms = piftest::MkSrv(d, mb, 64, true);
    pifilt::g_d.args = ab;
    pifilt::g_d.mainB = mb;
    c->VSSetShader(obj, nullptr, 0);
    c->VSSetShaderResources(3, 1, &ms);
    g_record = true;
    g_sample[0] = true;
    OnDraw(c, 0, 2 * 32);
    OnDraw(c, 0, 5 * 32);
    OnEnd(0);
    c->Flush();
    for (int t = 0; t < 100 && g_runsRead.load() == 0; ++t) {
      Sleep(5);
      PollPending(c);
    }
    uint64_t tri = 0, draws = 0;
    int slot = -2;
    {
      std::lock_guard<std::mutex> lock(g_mu);
      auto it = g_vs.find(obj);
      if (it != g_vs.end()) {
        tri = it->second.tri;
        draws = it->second.draws;
        slot = it->second.mainSlot;
      }
    }
    char what[200];
    sprintf_s(what, "vsig: draw census: %llu draws, %llu triangles from the read-back arguments (920 expected), mainBuffer "
                    "slot t%d",
              (unsigned long long)draws, (unsigned long long)tri, slot);
    Check(draws == 2 && tri == 100 * 7 + 20 * 11 && slot == 3, what);
    g_record = false;
    pifilt::g_d.args = nullptr;
    pifilt::g_d.mainB = nullptr;
    ID3D11ShaderResourceView* nul = nullptr;
    c->VSSetShaderResources(3, 1, &nul);
    c->VSSetShader(nullptr, nullptr, 0);
    Shutdown();
    ms->Release();
    mb->Release();
    ab->Release();
    pifilt::SetDeviceForTest(nullptr);
  }
  // 5. summary arithmetic and the key's default
  {
    std::map<ID3D11VertexShader*, Info> m;
    Info a, b, e;
    a.tri = 900;
    a.created = true;
    a.blob.elems = 5;
    a.blob.gsOk = true;
    b.tri = 60;
    b.idMatch = true;
    b.blob.elems = 4;
    b.blob.gsOk = true;
    e.tri = 40;  // no source
    m[reinterpret_cast<ID3D11VertexShader*>(0x10)] = a;
    m[reinterpret_cast<ID3D11VertexShader*>(0x20)] = b;
    m[reinterpret_cast<ID3D11VertexShader*>(0x30)] = e;
    const Report r = Summarise(m);
    wchar_t ini[MAX_PATH];
    GetTempPathW(MAX_PATH, ini);
    wcscat_s(ini, L"qv_vsig_none.ini");
    ReadConfig(ini);
    Check(r.triTotal == 1000 && r.triCovered == 960 && r.triCreated == 900 && r.triScan == 60 && r.objsCovered == 2 &&
              g_suiteSec.load() == 0,
          "vsig: coverage summary (96 % of 1000 triangles, 2 of 3 objects) and [Suite] FocusVsCensus default 0");
  }
  // 6. R49 archive source: folder loading, candidates by mainBuffer slot, exclusion by DCS's bound strides, the
  //    tally (a match, ambiguity when matches disagree in signature) and the summary without ambiguous objects.
  {
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    std::wstring dir = std::wstring(tmp) + L"qv_vsig_arc_" + std::to_wstring(GetCurrentProcessId());
    CreateDirectoryW(dir.c_str(), nullptr);
    auto put = [&](const wchar_t* n, const std::vector<uint8_t>& b) {
      const HANDLE f = CreateFileW((dir + L"\\" + n).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
      DWORD w = 0;
      if (f != INVALID_HANDLE_VALUE) WriteFile(f, b.data(), static_cast<DWORD>(b.size()), &w, nullptr), CloseHandle(f);
    };
    std::vector<uint8_t> junk(100, 7);
    put(L"a.dxbc", vs);
    put(L"b.dxbc", vs2);
    put(L"c.dxbc", ps);
    put(L"d.dxbc", junk);  // not a container
    put(L"e.bin", vs);     // not *.dxbc
    const auto arc = LoadArchive(dir);
    const auto c3 = ArchiveCandidates(arc, 3), c0 = ArchiveCandidates(arc, 0), c5 = ArchiveCandidates(arc, 5);
    const bool loadOk = arc.size() == 3 && c3.size() == 1 && c3[0] == vs && c0.size() == 1 && c0[0] == vs2 && c5.empty() &&
                        LoadArchive(L"").empty() && LoadArchive(dir + L"\\none").empty();
    for (const wchar_t* n : {L"a.dxbc", L"b.dxbc", L"c.dxbc", L"d.dxbc", L"e.bin"}) DeleteFileW((dir + L"\\" + n).c_str());
    RemoveDirectoryW(dir.c_str());
    // strides: mainBuffer bound at t3 with a 64-byte structure stride matches; 32 does not; nothing bound does not
    Blob b;
    Analyze(vs.data(), vs.size(), b);
    std::vector<float> zero(64 * 16, 0.0f);
    ID3D11Buffer* m64 = piftest::MkBuf(d, 64 * 64, D3D11_BIND_SHADER_RESOURCE, D3D11_RESOURCE_MISC_BUFFER_STRUCTURED, 64, zero.data());
    ID3D11Buffer* m32 = piftest::MkBuf(d, 64 * 64, D3D11_BIND_SHADER_RESOURCE, D3D11_RESOURCE_MISC_BUFFER_STRUCTURED, 32, zero.data());
    ID3D11ShaderResourceView* s64 = piftest::MkSrv(d, m64, 64, true);
    ID3D11ShaderResourceView* s32 = piftest::MkSrv(d, m32, 128, true);
    ID3D11ShaderResourceView* nul = nullptr;
    c->VSSetShaderResources(3, 1, &nul);
    const bool none = StridesMatch(c, b);
    c->VSSetShaderResources(3, 1, &s64);
    const bool m = StridesMatch(c, b);
    c->VSSetShaderResources(3, 1, &s32);
    const bool w = StridesMatch(c, b);
    c->VSSetShaderResources(3, 1, &nul);
    if (s64) s64->Release();
    if (s32) s32->Release();
    m64->Release();
    m32->Release();
    // the tally
    auto mk = [&](int state, const char* src) {
      IdTest t;
      PrepareTest(vs, t);
      t.state = state;
      t.soSrc = src;
      return t;
    };
    std::vector<IdTest> one, two, amb;
    one.push_back(mk(3, "s1"));
    one.push_back(mk(5, "s1"));
    one.push_back(mk(2, "s1"));
    one.push_back(mk(4, "s1"));
    two.push_back(mk(2, "s1"));
    two.push_back(mk(2, "s1"));
    amb.push_back(mk(2, "s1"));
    amb.push_back(mk(2, "s2"));
    Info i1, i2, i3;
    i1.tri = 800, i2.tri = 150, i3.tri = 50;
    TallyTests(one, i1);
    TallyTests(two, i2);
    TallyTests(amb, i3);
    std::map<ID3D11VertexShader*, Info> mm;
    mm[reinterpret_cast<ID3D11VertexShader*>(0x10)] = i1;
    mm[reinterpret_cast<ID3D11VertexShader*>(0x20)] = i2;
    mm[reinterpret_cast<ID3D11VertexShader*>(0x30)] = i3;
    const Report r = Summarise(mm);
    const bool tallyOk = i1.idMatch && !i1.idAmbig && i1.idTried == 4 && i1.idDiff == 1 && i1.idExcluded == 1 &&
                         i1.idFail == 1 && i1.idMatches == 1 && i1.matchSum == SumHex(vs.data() + 4) && i2.idMatch &&
                         !i2.idAmbig && i2.idMatches == 2 && i3.idMatch && i3.idAmbig && r.triCovered == 950 &&
                         r.objsCovered == 2;
    // The prefix rule: a shorter signature that is a prefix of the longest match is not ambiguous (the longest is
    // taken) when every element of the longest carried data; an all-zero element, or a non-prefix, is AMBIGUOUS.
    auto withSig = [&](int state, size_t n, bool nz, const char* src) {
      IdTest t = mk(state, src);
      t.sig.resize(n);
      for (size_t i = 0; i < n; ++i) t.sig[i].name = "TEXCOORD", t.sig[i].index = uint32_t(i), t.sig[i].reg = uint32_t(i), t.sig[i].mask = 0xf;
      t.colsNonZero = nz;
      return t;
    };
    std::vector<IdTest> pre, preZero, notPre;
    pre.push_back(withSig(2, 11, true, "a"));
    pre.push_back(withSig(2, 13, true, "c"));
    pre.push_back(withSig(2, 12, true, "b"));
    pre.push_back(withSig(3, 14, true, "d"));
    preZero.push_back(withSig(2, 11, true, "a"));
    preZero.push_back(withSig(2, 12, false, "b"));
    notPre.push_back(withSig(2, 11, true, "a"));
    notPre.push_back(withSig(2, 12, true, "b"));
    notPre.back().sig[3].index = 99;  // differs inside the shorter one's range
    Info p1, p2, p3;
    TallyTests(pre, p1);
    TallyTests(preZero, p2);
    TallyTests(notPre, p3);
    const bool prefixOk = p1.idMatch && !p1.idAmbig && p1.idPrefix == 2 && p1.idLongest == 13 && p1.idDiff == 1 &&
                          p2.idMatch && p2.idAmbig && p3.idMatch && p3.idAmbig;
    char what[300];
    sprintf_s(what, "vsig R49 archive: prefix rule (longest of 11/12/13 taken %d, all-zero element ambiguous %d, "
                    "non-prefix ambiguous %d)", p1.idLongest, p2.idAmbig, p3.idAmbig);
    Check(prefixOk, what);
    sprintf_s(what, "vsig R49 archive: folder loads %zu of 5 files (only *.dxbc containers), candidates by slot t3 %zu / t0 "
                    "%zu / t5 %zu; strides: none bound %d, 64 %d, 32 %d; tally and summary (ambiguous not covered) %d",
              arc.size(), c3.size(), c0.size(), c5.size(), none, m, w, tallyOk);
    Check(loadOk && !none && m && !w && tallyOk, what);
  }
  obj->Release();
  obj2->Release();
  c->Release();
  d->Release();
}

}  // namespace vsigtest
