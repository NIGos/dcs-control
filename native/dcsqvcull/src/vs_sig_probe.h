// R38 rank 2, gate G4 probe (measure only): can the output signature of every vertex shader DCS uses for the
// periphery G-buffer's instancer draws (forest and buildings, the draws a focus broadcast would carry) be obtained?
// A FastGS passthrough must reproduce that signature (R38R2 blocker 7). [Suite] FocusVsCensus=N (seconds, default
// 0 = nothing installed).
//
// What it records (render thread, through periph_inst_filter.h's per-run draw arm on the identified instancer
// lists; DCS's arguments are never changed by this probe):
// - per instancer draw of the two periphery views: the bound VS object and the argument offset; on sampled runs
//   the arguments are copied at the run end (stock again) and read back without waiting: triangles per draw =
//   IndexCountPerInstance / 3 x InstanceCount;
// - the VS t-slot where mainBuffer is bound (the identity check below).
// Signature sources, per VS object:
// (1) CreateVertexShader on DCS's device seen by our hook (installed from the config tick as soon as the device is
//     known, while the key is non-zero: start DCS with the key set to see what DCS creates after the payload
//     loads). Exact: the object and its bytecode come together. CreateInputLayout calls are counted too.
// (2) In-memory blobs: every reference to the VS object in DCS's private read/write memory is looked up; within
//     +-0x400 bytes of each reference, a pointer to (or an inline copy of) a DXBC vertex shader is a candidate.
//     A candidate counts only when it declares a 64-byte structured resource at the t-slot where the draw had
//     mainBuffer bound (every instancer VS reads mainBuffer through it).
// (3) fxo.edcz / metashaders2 (copies, offline): the DXBC checksum of every blob found is logged for an offline
//     match.
// (4) R49: archive candidates. [Suite] FocusVsArchive = a folder of *.dxbc files (the instancer VS extracted
//     offline from COPIES of Saved Games\DCS\metashaders2, dev only; the memory scan (2) is skipped then). Every
//     blob that declares a 64-byte structured resource at the object's mainBuffer slot is a candidate; one whose
//     structured resources do not match the strides DCS has bound is excluded without running; the rest are
//     identified by output (below). A MATCH counts only when every matching candidate has the same signature
//     (else AMBIGUOUS, not covered).
// Gate G4: the triangles of VS objects with a known signature from which a FastGS passthrough can be generated
// (fbc::FastGsHlsl non-empty) are >= 90 % of the periphery instancer triangles.
//
// Included once from main.cpp after periph_inst_filter.h (uses pifilt, fbc, d3ds).
#pragma once
// (std map / set / unordered_map come from main.cpp's includes: this file is included inside its namespace)

namespace vsig {

std::atomic<int> g_suiteSec{0};  // [Suite] FocusVsCensus
std::wstring g_archiveDir;       // [Suite] FocusVsArchive (R49, dev only; empty: no archive source)

// ---------------------------------------------------------------------------
// DXBC analysis (pure)
// ---------------------------------------------------------------------------
struct Blob {
  uint8_t sum[16] = {};
  uint32_t size = 0;
  int elems = -1;     // output signature elements (-1: none parsed)
  bool gsOk = false;  // a FastGS passthrough can be generated for that signature
  std::vector<std::pair<uint32_t, uint32_t>> structs;  // dcl_resource_structured (t-slot, stride)
};

// The DXBC container's declared size (0 when the header is not one).
inline uint32_t DxbcSize(const uint8_t* b, size_t avail) {
  if (avail < 32 || memcmp(b, "DXBC", 4) != 0) return 0;
  uint32_t one = 0, size = 0, n = 0;
  memcpy(&one, b + 20, 4);
  memcpy(&size, b + 24, 4);
  memcpy(&n, b + 28, 4);
  if (one != 1 || size < 32 || size > (4u << 20) || n == 0 || n > 64 || 32 + 4ull * n > size) return 0;
  return size;
}
// A vertex shader blob: its output signature, whether a FastGS can mirror it, its structured resources.
inline bool Analyze(const void* code, size_t n, Blob& out) {
  const uint8_t* b = static_cast<const uint8_t*>(code);
  const uint32_t size = DxbcSize(b, n);
  if (!size || size > n) return false;
  uint32_t cnt = 0;
  memcpy(&cnt, b + 28, 4);
  bool vs = false;
  out = Blob{};
  memcpy(out.sum, b + 4, 16);
  out.size = size;
  for (uint32_t i = 0; i < cnt; ++i) {
    uint32_t off = 0, len = 0;
    memcpy(&off, b + 32 + 4 * i, 4);
    if (off + 8ull > size) return false;
    memcpy(&len, b + off + 4, 4);
    if (off + 8ull + len > size) return false;
    if (memcmp(b + off, "SHEX", 4) != 0 && memcmp(b + off, "SHDR", 4) != 0) continue;
    if (len < 8) return false;
    const uint32_t* t = reinterpret_cast<const uint32_t*>(b + off + 8);
    const uint32_t nt = len / 4;
    vs = (t[0] >> 16) == 1;
    uint32_t total = std::min(t[1], nt);
    for (uint32_t k = 2; k < total;) {
      const uint32_t tok = t[k], op = tok & 0x7ff;
      uint32_t il = (tok >> 24) & 0x7f;
      if (op == 0x35) il = k + 1 < total ? t[k + 1] : 0;  // customdata: the length is in the next dword
      if (il == 0) break;
      // dcl_resource_structured: [opcode] [operand token] [register] [stride] (no extended operand)
      if (op == 162 && il >= 4 && k + 3 < total && !(t[k + 1] & 0x80000000u)) out.structs.emplace_back(t[k + 2], t[k + 3]);
      k += il;
    }
  }
  if (!vs) return false;
  std::vector<fbc::SigElem> sig;
  if (fbc::ParseOutputSignature(b, size, sig)) {
    out.elems = static_cast<int>(sig.size());
    out.gsOk = !fbc::FastGsHlsl(sig, 3, false).empty();
  }
  return true;
}
inline bool DeclaresMain(const Blob& b, int slot) {
  for (auto& s : b.structs)
    if (static_cast<int>(s.first) == slot && s.second == 64) return true;
  return false;
}
// SEH-guarded copy (DCS may free what we read).
inline bool SafeCopy(void* dst, const void* src, size_t n) {
  __try {
    memcpy(dst, src, n);
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}
inline std::string SumHex(const uint8_t* s) {
  char h[40];
  for (int i = 0; i < 16; ++i) sprintf_s(h + 2 * i, 3, "%02x", s[i]);
  return h;
}

// ---------------------------------------------------------------------------
// Device hooks: CreateVertexShader / CreateInputLayout (exact source (1))
// ---------------------------------------------------------------------------
using CvsFn = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, const void*, SIZE_T, ID3D11ClassLinkage*,
                                          ID3D11VertexShader**);
using CilFn = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, const D3D11_INPUT_ELEMENT_DESC*, UINT, const void*, SIZE_T,
                                          ID3D11InputLayout**);
constexpr int kSlotCil = 11, kSlotCvs = 12;  // ID3D11Device vtable
void** g_devVt = nullptr;
void* volatile g_origCvs = nullptr;
void* volatile g_origCil = nullptr;
ID3D11Device* g_hookDev = nullptr;  // the device whose calls are recorded
std::mutex g_mu;
std::unordered_map<void*, Blob> g_created;  // VS object -> its bytecode (objects created since the hook)
std::atomic<uint64_t> g_createdVs{0}, g_createdIl{0};
ULONGLONG g_hookTick = 0;

HRESULT STDMETHODCALLTYPE H_Cvs(ID3D11Device* d, const void* code, SIZE_T n, ID3D11ClassLinkage* l,
                                ID3D11VertexShader** pp) {
  const HRESULT hr = reinterpret_cast<CvsFn>(g_origCvs)(d, code, n, l, pp);
  if (SUCCEEDED(hr) && pp && *pp && d == g_hookDev && code) {
    g_createdVs.fetch_add(1, std::memory_order_relaxed);
    Blob b;
    std::vector<uint8_t> copy(n);
    if (!n || n > (4u << 20) || !SafeCopy(copy.data(), code, n) || !Analyze(copy.data(), n, b)) b = Blob{};
    std::lock_guard<std::mutex> lock(g_mu);
    g_created[*pp] = b;
  }
  return hr;
}
HRESULT STDMETHODCALLTYPE H_Cil(ID3D11Device* d, const D3D11_INPUT_ELEMENT_DESC* e, UINT ne, const void* code,
                                SIZE_T n, ID3D11InputLayout** pp) {
  const HRESULT hr = reinterpret_cast<CilFn>(g_origCil)(d, e, ne, code, n, pp);
  if (SUCCEEDED(hr) && d == g_hookDev) g_createdIl.fetch_add(1, std::memory_order_relaxed);
  return hr;
}
bool InstallDeviceHooks(ID3D11Device* dev) {
  if (g_devVt || !dev) return g_devVt != nullptr;
  void** vt = *reinterpret_cast<void***>(dev);
  g_origCvs = vt[kSlotCvs];
  g_origCil = vt[kSlotCil];
  g_hookDev = dev;
  d3ds::WriteTablePointer(&vt[kSlotCvs], reinterpret_cast<void*>(&H_Cvs));
  d3ds::WriteTablePointer(&vt[kSlotCil], reinterpret_cast<void*>(&H_Cil));
  g_devVt = vt;
  g_hookTick = GetTickCount64();
  return true;
}
void RemoveDeviceHooks() {
  if (!g_devVt) return;
  if (g_devVt[kSlotCvs] == reinterpret_cast<void*>(&H_Cvs)) d3ds::WriteTablePointer(&g_devVt[kSlotCvs], g_origCvs);
  if (g_devVt[kSlotCil] == reinterpret_cast<void*>(&H_Cil)) d3ds::WriteTablePointer(&g_devVt[kSlotCil], g_origCil);
  g_devVt = nullptr;
  g_hookDev = nullptr;
}

// ---------------------------------------------------------------------------
// Identification by output (render thread, before a DCS instancer draw with the object bound)
// ---------------------------------------------------------------------------
// DCS's bound VS object and our VS made from a candidate blob run in turn with DCS's state, as K points of instance
// 0, through one stream-output GS built from the candidate's output signature (nothing rasterised). Equal bytes
// = the object runs the candidate's program: its signature is the object's. A wrong candidate differs (a wrong
// signature only reads registers differently for both runs; it can cause a miss, not a false match).
constexpr UINT kIdVerts = 64;
struct IdTest {
  std::vector<uint8_t> code, gsCode;
  Blob b;
  std::vector<std::string> names;
  std::vector<D3D11_SO_DECLARATION_ENTRY> decl;
  UINT stride = 0;
  ID3D11VertexShader* vs = nullptr;
  ID3D11GeometryShader* gs = nullptr;
  ID3D11Buffer *soA = nullptr, *soB = nullptr, *stA = nullptr, *stB = nullptr;
  std::string soSrc;  // the stream-output GS source (one per output signature: equal sources = equal signatures)
  std::vector<fbc::SigElem> sig;  // R49: the output signature in register order (as the stream-output record)
  bool colsNonZero = false;       // R49: every element of the record carries a non-zero byte in DCS's run
  int state = 0;  // 0 untested, 1 in flight, 2 MATCH, 3 differs, 4 could not run, 5 excluded (bound strides differ)
  int age = 0;
  void Release() {
    pifilt::Rel(vs);
    pifilt::Rel(gs);
    pifilt::Rel(soA);
    pifilt::Rel(soB);
    pifilt::Rel(stA);
    pifilt::Rel(stB);
  }
};
// The stream-output GS for a signature (register order; SV_Position as SV_Position). False when it holds system
// values other than the position or components a passthrough cannot carry.
bool SoGs(const std::vector<fbc::SigElem>& sig0, IdTest& t) {
  std::vector<fbc::SigElem> sig = sig0;
  std::sort(sig.begin(), sig.end(), [](const fbc::SigElem& a, const fbc::SigElem& b) {
    if (a.reg != b.reg) return a.reg < b.reg;
    return (a.mask & -a.mask) < (b.mask & -b.mask);
  });
  t.sig = sig;  // R49
  std::string fields;
  t.names.clear();
  t.decl.clear();
  t.stride = 0;
  for (size_t i = 0; i < sig.size(); ++i) {
    const fbc::SigElem& e = sig[i];
    if (e.stream || e.minPrec || (e.sysValue != 0 && e.sysValue != 1)) return false;
    const int n = __popcnt(e.mask);
    if (n < 1 || n > 4) return false;
    const char* ty = e.compType == 1 ? "uint" : e.compType == 2 ? "int" : "float";
    char line[160];
    const std::string sem = e.sysValue == 1 ? std::string("SV_Position") : e.name + std::to_string(e.index);
    sprintf_s(line, "  %s%s%s m%zu : %s;\n", (e.compType == 1 || e.compType == 2) ? "nointerpolation " : "", ty,
              n > 1 ? std::to_string(n).c_str() : "", i, sem.c_str());
    fields += line;
    t.names.push_back(e.sysValue == 1 ? "SV_Position" : e.name);
  }
  for (size_t i = 0; i < sig.size(); ++i) {
    const fbc::SigElem& e = sig[i];
    D3D11_SO_DECLARATION_ENTRY d{};
    d.Stream = 0;
    d.SemanticName = t.names[i].c_str();
    d.SemanticIndex = sig[i].sysValue == 1 ? 0 : e.index;
    d.StartComponent = 0;
    d.ComponentCount = static_cast<BYTE>(__popcnt(e.mask));
    d.OutputSlot = 0;
    t.decl.push_back(d);
    t.stride += 4 * d.ComponentCount;
  }
  if (t.decl.empty() || t.stride > 2048) return false;
  const std::string src = "struct V {\n" + fields +
                          "};\n[maxvertexcount(1)] void main(point V i[1], inout PointStream<V> s) { s.Append(i[0]); }\n";
  t.soSrc = src;
  static std::map<std::string, std::vector<uint8_t>> s_gsCache;  // R49: hundreds of archive candidates, few signatures
  static std::mutex s_gsMu;
  {
    std::lock_guard<std::mutex> lock(s_gsMu);
    auto it = s_gsCache.find(src);
    if (it != s_gsCache.end()) {
      t.gsCode = it->second;
      return true;
    }
  }
  if (!chainfast::LoadCompiler()) return false;
  ID3DBlob *code = nullptr, *err = nullptr;
  const HRESULT hr = chainfast::g_compile(src.c_str(), src.size(), "vsig_so", nullptr, nullptr, "main", "gs_5_0",
                                          D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &err);
  if (err) err->Release();
  if (FAILED(hr) || !code) return false;
  const uint8_t* p = static_cast<const uint8_t*>(code->GetBufferPointer());
  t.gsCode.assign(p, p + code->GetBufferSize());
  code->Release();
  std::lock_guard<std::mutex> lock(s_gsMu);
  s_gsCache[src] = t.gsCode;
  return true;
}
// R49: a candidate whose structured resources (t-slot, stride) are not all bound with that structure stride in DCS's
// state cannot be the bound program (DCS binds what its VS reads): excluded without creating anything.
bool StridesMatch(ID3D11DeviceContext* c, const Blob& b) {
  for (auto& s : b.structs) {
    if (s.first >= D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT) return false;
    ID3D11ShaderResourceView* v = nullptr;
    c->VSGetShaderResources(s.first, 1, &v);
    if (!v) return false;
    ID3D11Buffer* buf = pifilt::BufOf(v);
    UINT stride = 0;
    if (buf) {
      D3D11_BUFFER_DESC d{};
      buf->GetDesc(&d);
      stride = (d.MiscFlags & D3D11_RESOURCE_MISC_BUFFER_STRUCTURED) ? d.StructureByteStride : 0;
    }
    v->Release();
    if (stride != s.second) return false;
  }
  return true;
}
// R49: the archive folder's *.dxbc files (bytes; at most 4096 files of <= 4 MB).
std::vector<std::vector<uint8_t>> LoadArchive(const std::wstring& dir) {
  std::vector<std::vector<uint8_t>> out;
  if (dir.empty()) return out;
  WIN32_FIND_DATAW fd{};
  const HANDLE h = FindFirstFileW((dir + L"\\*.dxbc").c_str(), &fd);
  if (h == INVALID_HANDLE_VALUE) return out;
  do {
    if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.nFileSizeHigh || fd.nFileSizeLow > (4u << 20)) continue;
    const HANDLE f = CreateFileW((dir + L"\\" + fd.cFileName).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                 OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) continue;
    std::vector<uint8_t> b(fd.nFileSizeLow);
    DWORD got = 0;
    if (ReadFile(f, b.data(), fd.nFileSizeLow, &got, nullptr) && got == fd.nFileSizeLow && DxbcSize(b.data(), b.size()))
      out.push_back(std::move(b));
    CloseHandle(f);
  } while (out.size() < 4096 && FindNextFileW(h, &fd));
  FindClose(h);
  return out;
}
// R49: the archive blobs that can be the program of an object with mainBuffer at `slot` (a vertex shader declaring a
// 64-byte structured resource there, with an output signature a FastGS passthrough can carry); distinct checksums.
std::vector<std::vector<uint8_t>> ArchiveCandidates(const std::vector<std::vector<uint8_t>>& arc, int slot) {
  std::vector<std::vector<uint8_t>> out;
  std::set<std::string> taken;
  for (auto& code : arc) {
    Blob b;
    if (Analyze(code.data(), code.size(), b) && DeclaresMain(b, slot) && b.gsOk && taken.insert(SumHex(b.sum)).second)
      out.push_back(code);
  }
  return out;
}
// Prepares one candidate (suite thread: compile only).
bool PrepareTest(const std::vector<uint8_t>& blob, IdTest& t) {
  t = IdTest{};
  t.code = blob;
  if (!Analyze(blob.data(), blob.size(), t.b)) return false;
  std::vector<fbc::SigElem> sig;
  return fbc::ParseOutputSignature(blob.data(), blob.size(), sig) && SoGs(sig, t);
}
// Runs one test on the context right now (DCS's object `x` bound with its draw's state). False: could not run.
bool RunTest(ID3D11DeviceContext* c, ID3D11Device* d, ID3D11VertexShader* x, IdTest& t) {
  if (!t.vs && FAILED(d->CreateVertexShader(t.code.data(), t.code.size(), nullptr, &t.vs))) return false;
  if (!t.gs && FAILED(d->CreateGeometryShaderWithStreamOutput(t.gsCode.data(), t.gsCode.size(), t.decl.data(),
                                                              static_cast<UINT>(t.decl.size()), &t.stride, 1,
                                                              D3D11_SO_NO_RASTERIZED_STREAM, nullptr, &t.gs)))
    return false;
  const UINT bytes = t.stride * kIdVerts;
  auto mk = [&](ID3D11Buffer** b, bool staging) {
    if (*b) return true;
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = bytes;
    bd.Usage = staging ? D3D11_USAGE_STAGING : D3D11_USAGE_DEFAULT;
    bd.BindFlags = staging ? 0 : D3D11_BIND_STREAM_OUTPUT;
    bd.CPUAccessFlags = staging ? D3D11_CPU_ACCESS_READ : 0;
    return SUCCEEDED(d->CreateBuffer(&bd, nullptr, b));
  };
  if (!mk(&t.soA, false) || !mk(&t.soB, false) || !mk(&t.stA, true) || !mk(&t.stB, true)) return false;
  ID3D11HullShader* hs = nullptr;
  c->HSGetShader(&hs, nullptr, nullptr);
  if (hs) {
    hs->Release();
    return false;
  }
  // DCS's state
  D3D11_PRIMITIVE_TOPOLOGY topo{};
  c->IAGetPrimitiveTopology(&topo);
  ID3D11GeometryShader* oldGs = nullptr;
  ID3D11ClassInstance* gci[16] = {};
  UINT ngci = 16;
  c->GSGetShader(&oldGs, gci, &ngci);
  ID3D11VertexShader* oldVs = nullptr;
  ID3D11ClassInstance* vci[16] = {};
  UINT nvci = 16;
  c->VSGetShader(&oldVs, vci, &nvci);
  ID3D11Buffer* oldSo[4] = {};
  c->SOGetTargets(4, oldSo);
  // the two runs
  const UINT fill[4] = {0xcdcdcdcdu, 0xcdcdcdcdu, 0xcdcdcdcdu, 0xcdcdcdcdu};
  (void)fill;
  c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_POINTLIST);
  c->GSSetShader(t.gs, nullptr, 0);
  const UINT zero = 0;
  c->VSSetShader(x, nullptr, 0);
  c->SOSetTargets(1, &t.soA, &zero);
  c->Draw(kIdVerts, 0);
  c->VSSetShader(t.vs, nullptr, 0);
  c->SOSetTargets(1, &t.soB, &zero);
  c->Draw(kIdVerts, 0);
  // DCS's state back
  ID3D11Buffer* nul[1] = {};
  c->SOSetTargets(1, nul, &zero);
  UINT offs[4] = {UINT(-1), UINT(-1), UINT(-1), UINT(-1)};
  c->SOSetTargets(4, oldSo, offs);
  c->VSSetShader(oldVs, nvci ? vci : nullptr, nvci);
  c->GSSetShader(oldGs, ngci ? gci : nullptr, ngci);
  c->IASetPrimitiveTopology(topo);
  for (auto& p : oldSo) pifilt::Rel(p);
  pifilt::Rel(oldVs);
  pifilt::Rel(oldGs);
  for (auto& p : vci) pifilt::Rel(p);
  for (auto& p : gci) pifilt::Rel(p);
  c->CopyResource(t.stA, t.soA);
  c->CopyResource(t.stB, t.soB);
  t.state = 1;
  t.age = 0;
  return true;
}
// Reads a test back without waiting: 2 MATCH, 3 differs, 1 still in flight.
int PollTest(ID3D11DeviceContext* c, IdTest& t) {
  if (t.state != 1) return t.state;
  D3D11_MAPPED_SUBRESOURCE a{}, b{};
  HRESULT ha = c->Map(t.stA, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &a);
  if (ha == DXGI_ERROR_WAS_STILL_DRAWING) {
    if (++t.age > 240) t.state = 4;
    return t.state;
  }
  if (FAILED(ha)) return t.state = 4;
  HRESULT hb = c->Map(t.stB, 0, D3D11_MAP_READ, 0, &b);
  if (FAILED(hb)) {
    c->Unmap(t.stA, 0);
    return t.state = 4;
  }
  const UINT bytes = t.stride * kIdVerts;
  const bool eq = memcmp(a.pData, b.pData, bytes) == 0;
  // R49: per element, any non-zero byte over the K records of DCS's run (an all-zero trailing element cannot tell a
  // longer signature from a shorter one that matches as well).
  {
    const uint8_t* pa = static_cast<const uint8_t*>(a.pData);
    bool all = !t.sig.empty();
    UINT off = 0;
    for (const fbc::SigElem& e : t.sig) {
      const UINT w = 4 * __popcnt(e.mask);
      bool nz = false;
      for (UINT v = 0; v < kIdVerts && !nz; ++v)
        for (UINT i = 0; i < w && !nz; ++i) nz = pa[v * t.stride + off + i] != 0;
      all = all && nz;
      off += w;
    }
    t.colsNonZero = all;
  }
  bool nonzero = false;
  for (UINT i = 0; i < bytes && !nonzero; ++i) nonzero = static_cast<const uint8_t*>(a.pData)[i] != 0;
  c->Unmap(t.stA, 0);
  c->Unmap(t.stB, 0);
  return t.state = (eq && nonzero) ? 2 : 3;
}

// ---------------------------------------------------------------------------
// Draw census (render thread)
// ---------------------------------------------------------------------------
struct Info {
  uint64_t draws = 0, tri = 0, triDraws = 0;
  int mainSlot = -1;
  bool created = false;
  Blob blob;            // from (1) or (2)
  int scanBlobs = 0;    // distinct DXBC VS candidates near its references
  int scanRefs = 0;     // references to the object found
  bool scanOk = false;  // the blob taken declares mainBuffer's slot (and is unambiguous)
  bool scanOwn = false, scanAmb = false;
  int64_t scanDist = -1;
  int idTried = 0, idDiff = 0, idFail = 0;
  bool idMatch = false;  // a candidate's program confirmed by output
  // R49 (archive source)
  int idExcluded = 0;      // candidates excluded by the bound strides
  int idMatches = 0;       // candidates that matched
  bool idAmbig = false;    // matches with different signatures: not covered
  bool fromArchive = false;
  std::string matchSum;    // the chosen match's DXBC checksum
  int idPrefix = 0;        // matches whose signature is a prefix of the chosen one
  int idLongest = 0;       // elements of the chosen (longest) matching signature
};
std::map<ID3D11VertexShader*, Info> g_vs;  // keys AddRef'd while the phase runs
struct Rec {
  ID3D11VertexShader* vs;
  UINT off;
};
std::vector<Rec> g_runRecs[2];
bool g_sample[2] = {};
uint64_t g_runN[2] = {};
struct Pend {
  ID3D11Buffer* st = nullptr;
  std::vector<Rec> recs;
  bool busy = false;
  int age = 0;
};
Pend g_pend[4];
UINT g_argsBytes = 0;
std::atomic<uint64_t> g_runsSampled{0}, g_runsRead{0}, g_drawsSeen{0};
std::atomic<bool> g_record{false};
// identification phase (render thread runs the tests; the suite thread prepares and reads them between phases)
std::map<ID3D11VertexShader*, std::vector<IdTest>> g_tests;
std::set<ID3D11VertexShader*> g_filtered;  // R49: objects whose candidates were checked against the bound strides
std::atomic<bool> g_identify{false}, g_cleanup{false};
std::atomic<uint64_t> g_testsRun{0};

void PollPending(ID3D11DeviceContext* c) {
  for (Pend& p : g_pend) {
    if (!p.busy) continue;
    D3D11_MAPPED_SUBRESOURCE m{};
    const HRESULT hr = c->Map(p.st, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m);
    if (hr == DXGI_ERROR_WAS_STILL_DRAWING) {
      if (++p.age > 240) p.busy = false;
      continue;
    }
    p.busy = false;
    if (FAILED(hr)) continue;
    const uint32_t* a = static_cast<const uint32_t*>(m.pData);
    {
      std::lock_guard<std::mutex> lock(g_mu);
      for (const Rec& r : p.recs) {
        if (r.off % 4 || r.off + 8ull > g_argsBytes) continue;
        const uint64_t t = uint64_t(a[r.off / 4] / 3) * a[r.off / 4 + 1];
        Info& in = g_vs[r.vs];
        in.tri += t;
        ++in.triDraws;
      }
    }
    c->Unmap(p.st, 0);
    g_runsRead.fetch_add(1, std::memory_order_relaxed);
  }
}

void OnDraw(ID3D11DeviceContext* c, int view, UINT off) {
  const bool rec = g_record.load(std::memory_order_relaxed), idf = g_identify.load(std::memory_order_relaxed);
  if (!rec && !idf) return;
  ID3D11VertexShader* vs = nullptr;
  c->VSGetShader(&vs, nullptr, nullptr);
  if (!vs) return;
  bool fresh = false;
  if (rec) {
    g_drawsSeen.fetch_add(1, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lock(g_mu);
    auto it = g_vs.find(vs);
    if (it == g_vs.end()) {
      it = g_vs.emplace(vs, Info{}).first;
      vs->AddRef();  // kept until the report
      fresh = true;
    }
    ++it->second.draws;
  }
  if (fresh) {
    ID3D11ShaderResourceView* v[16] = {};
    c->VSGetShaderResources(0, 16, v);
    int slot = -1;
    for (int i = 0; i < 16; ++i)
      if (v[i] && slot < 0 && pifilt::BufOf(v[i]) == pifilt::g_d.mainB) slot = i;
    for (auto& p : v) pifilt::Rel(p);
    std::lock_guard<std::mutex> lock(g_mu);
    g_vs[vs].mainSlot = slot;
  }
  if (rec && g_sample[view]) g_runRecs[view].push_back({vs, off});
  if (idf) {
    auto it = g_tests.find(vs);
    if (it != g_tests.end() && g_filtered.insert(vs).second)  // R49: once per object, DCS's bound strides
      for (IdTest& t : it->second)
        if (t.state == 0 && !StridesMatch(c, t.b)) t.state = 5;
    if (it != g_tests.end())
      for (IdTest& t : it->second)
        if (t.state == 0) {
          if (!RunTest(c, pifilt::g_dev, vs, t)) t.state = 4;
          g_testsRun.fetch_add(1, std::memory_order_relaxed);
          break;  // one test per DCS draw
        }
  }
  vs->Release();  // the map's reference keeps it
}

void OnEnd(int view) {
  ID3D11DeviceContext* c = pifilt::g_ctx;
  if (!c || !pifilt::g_d.args) return;
  PollPending(c);
  for (auto& kv : g_tests)
    for (IdTest& t : kv.second) PollTest(c, t);
  if (g_cleanup.load(std::memory_order_relaxed)) {  // the tests' objects go on the render thread
    for (auto& kv : g_tests)
      for (IdTest& t : kv.second) t.Release();
    g_cleanup = false;
  }
  if (g_sample[view] && !g_runRecs[view].empty()) {
    if (!g_argsBytes) g_argsBytes = pifilt::BufBytes(pifilt::g_d.args);
    for (Pend& p : g_pend) {
      if (p.busy) continue;
      if (!p.st) {
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = g_argsBytes;
        bd.Usage = D3D11_USAGE_STAGING;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(pifilt::g_dev->CreateBuffer(&bd, nullptr, &p.st))) break;
      }
      c->CopyResource(p.st, pifilt::g_d.args);
      p.recs.swap(g_runRecs[view]);
      p.busy = true;
      p.age = 0;
      g_runsSampled.fetch_add(1, std::memory_order_relaxed);
      break;
    }
  }
  g_runRecs[view].clear();
  g_sample[view] = g_record.load(std::memory_order_relaxed) && (++g_runN[view] % 4) == 0;  // the next run
}

// ---------------------------------------------------------------------------
// In-memory scan (source (2); suite thread, after the census)
// ---------------------------------------------------------------------------
struct Region {
  uintptr_t a, b;
};
inline bool Readable(DWORD p) {
  if (p & (PAGE_GUARD | PAGE_NOACCESS)) return false;
  return (p & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE |
               PAGE_EXECUTE_WRITECOPY)) != 0;
}
inline bool InRegions(const std::vector<Region>& rs, uintptr_t p, size_t n) {
  auto it = std::upper_bound(rs.begin(), rs.end(), p, [](uintptr_t v, const Region& r) { return v < r.a; });
  if (it == rs.begin()) return false;
  --it;
  return p >= it->a && p + n <= it->b;
}
// One region's qwords that equal a wanted pointer (SEH: DCS may free the region meanwhile).
void ScanRegion(uintptr_t a, uintptr_t b, uintptr_t lo, uintptr_t hi, const std::unordered_map<uintptr_t, int>* want,
                std::vector<std::pair<uintptr_t, uintptr_t>>* hits) {
  const uintptr_t* p = reinterpret_cast<const uintptr_t*>(a);
  const size_t n = (b - a) / 8;
  __try {
    for (size_t i = 0; i < n; ++i) {
      const uintptr_t v = p[i];
      if (v < lo || v > hi) continue;
      if (want->count(v)) hits->emplace_back(a + i * 8, v);
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }
}
// Per object: the DXBC VS candidates found (a) in the object's own memory (the D3D11 runtime's shader object, from
// 0x40 before the interface pointer to 0x400 after it) and (b) around each reference to the object elsewhere. The
// blob taken is the nearest own candidate, else the nearest reference-window candidate (by the distance between the
// pointer / inline copy and the object or the reference); `ok` when it declares mainBuffer's slot.
struct ScanOut {
  int refs = 0, blobs = 0;
  bool own = false;      // the blob taken came from the object's own memory
  int64_t dist = -1;     // its distance in bytes
  bool ambiguous = false;  // another distinct blob at the same smallest distance
  bool ok = false;
  Blob best;
  std::vector<std::vector<uint8_t>> tests;  // distinct candidates that declare mainBuffer's slot (<= 16), nearest first
};
struct Cand {
  int64_t dist;
  bool own;
  Blob b;
  std::vector<uint8_t> code;  // the blob's bytes (identification by output)
};
// The DXBC VS candidates in [w0, w1): pointers to blobs and inline copies, with |position - origin|.
void Candidates(const std::vector<Region>& readable, uintptr_t w0, uintptr_t w1, uintptr_t origin, bool own,
                std::vector<Cand>& out) {
  std::vector<uint8_t> buf;
  auto tryAt = [&](uintptr_t at, uintptr_t pos) {  // a DXBC container at `at`, copied before it is parsed
    uint8_t hd[32];
    if (!InRegions(readable, at, 32) || !SafeCopy(hd, reinterpret_cast<const void*>(at), 32) || memcmp(hd, "DXBC", 4) != 0)
      return;
    uint32_t full = 0;
    memcpy(&full, hd + 24, 4);
    if (full < 32 || full > (4u << 20) || !InRegions(readable, at, full)) return;
    buf.resize(full);
    Blob b;
    if (!SafeCopy(buf.data(), reinterpret_cast<const void*>(at), full) || !Analyze(buf.data(), full, b)) return;
    const int64_t d = pos > origin ? int64_t(pos - origin) : int64_t(origin - pos);
    out.push_back({d, own, b, buf});
  };
  for (uintptr_t q = w0 & ~uintptr_t(7); q < w1; q += 8) {
    uintptr_t v = 0;
    if (!InRegions(readable, q, 8) || !SafeCopy(&v, reinterpret_cast<const void*>(q), 8)) continue;
    if (v >= 0x10000 && v < 0x7fffffff0000ull && !(v & 3)) tryAt(v, q);  // a pointer to a blob
    tryAt(q, q);                                                          // an inline copy
  }
}
ScanOut Choose(std::vector<Cand>& c, int refs, int slot) {
  ScanOut so;
  so.refs = refs;
  std::set<std::string> distinct;
  for (auto& x : c) distinct.insert(SumHex(x.b.sum));
  so.blobs = static_cast<int>(distinct.size());
  if (c.empty()) return so;
  std::stable_sort(c.begin(), c.end(), [](const Cand& a, const Cand& b) {
    if (a.own != b.own) return a.own;  // the object's own memory first
    return a.dist < b.dist;
  });
  const Cand& k = c[0];
  for (size_t i = 1; i < c.size() && c[i].own == k.own && c[i].dist == k.dist; ++i)
    if (memcmp(c[i].b.sum, k.b.sum, 16) != 0) so.ambiguous = true;
  so.own = k.own;
  so.dist = k.dist;
  so.best = k.b;
  so.ok = !so.ambiguous && DeclaresMain(k.b, slot);
  std::set<std::string> taken;
  for (auto& x : c)
    if (DeclaresMain(x.b, slot) && x.b.gsOk && so.tests.size() < 16 && taken.insert(SumHex(x.b.sum)).second)
      so.tests.push_back(x.code);
  return so;
}
// Scans every committed private read/write region for the given pointers, then the candidates per object.
// limitBytes caps the reference scan (0: no cap).
std::map<uintptr_t, ScanOut> ScanMemory(const std::vector<std::pair<uintptr_t, int>>& objs, uint64_t limitBytes,
                                        uint64_t* scanned, uint32_t* ms) {
  std::map<uintptr_t, ScanOut> out;
  std::vector<Region> readable, rw;
  const ULONGLONG t0 = GetTickCount64();
  MEMORY_BASIC_INFORMATION mbi{};
  for (uintptr_t a = 0x10000; a < 0x7fffffff0000ull && VirtualQuery(reinterpret_cast<void*>(a), &mbi, sizeof(mbi));) {
    const uintptr_t base = reinterpret_cast<uintptr_t>(mbi.BaseAddress), end = base + mbi.RegionSize;
    if (mbi.State == MEM_COMMIT && Readable(mbi.Protect)) {
      if (!readable.empty() && readable.back().b == base) readable.back().b = end;
      else readable.push_back({base, end});
      if (mbi.Type == MEM_PRIVATE && (mbi.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE))) rw.push_back({base, end});
    }
    if (end <= a) break;
    a = end;
  }
  if (objs.empty()) return out;
  std::unordered_map<uintptr_t, int> want;
  uintptr_t lo = ~uintptr_t(0), hi = 0;
  for (auto& o : objs) {
    want[o.first] = o.second;
    lo = std::min(lo, o.first);
    hi = std::max(hi, o.first);
  }
  uint64_t done = 0;
  std::vector<std::pair<uintptr_t, uintptr_t>> hits;  // (address of the reference, object)
  for (const Region& r : rw) {
    if (limitBytes && done >= limitBytes) break;
    ScanRegion(r.a, r.b, lo, hi, &want, &hits);
    done += r.b - r.a;
  }
  std::map<uintptr_t, std::vector<Cand>> cands;
  std::map<uintptr_t, int> refs;
  for (auto& o : objs) {
    std::vector<Cand>& oc = cands[o.first];
    const uintptr_t w0 = o.first >= 0x40 ? o.first - 0x40 : 0, w1 = o.first + 0x400;
    Candidates(readable, w0, w1, o.first, true, oc);
    // one level down: objects the shader object points to (the runtime's bytecode / signature holders)
    for (uintptr_t q = w0 & ~uintptr_t(7); q < w1; q += 8) {
      uintptr_t v = 0;
      if (!InRegions(readable, q, 8) || !SafeCopy(&v, reinterpret_cast<const void*>(q), 8)) continue;
      if (v < 0x10000 || v >= 0x7fffffff0000ull || (v & 7) || !InRegions(readable, v, 8)) continue;
      const size_t before = oc.size();
      Candidates(readable, v, v + 0x200, v, true, oc);
      for (size_t i = before; i < oc.size(); ++i) oc[i].dist += 0x1000 + int64_t(q > o.first ? q - o.first : o.first - q);
    }
  }
  for (auto& h : hits) {
    ++refs[h.second];
    Candidates(readable, h.first >= 0x400 ? h.first - 0x400 : 0, h.first + 0x400, h.first, false, cands[h.second]);
  }
  for (auto& o : objs) out[o.first] = Choose(cands[o.first], refs[o.first], o.second);
  if (scanned) *scanned = done;
  if (ms) *ms = static_cast<uint32_t>(GetTickCount64() - t0);
  return out;
}
// ---------------------------------------------------------------------------
// Config, suite phase, report
// ---------------------------------------------------------------------------
void ReadConfig(const wchar_t* ini) {
  g_suiteSec = std::clamp(static_cast<int>(GetPrivateProfileIntW(L"Suite", L"FocusVsCensus", 0, ini)), 0, 600);
  wchar_t b[MAX_PATH] = {};
  GetPrivateProfileStringW(L"Suite", L"FocusVsArchive", L"", b, MAX_PATH, ini);
  g_archiveDir = b;
}
// R49: the outcome of one object's tests (pure; tested). Matches can differ in signature in one way only: a candidate
// whose output signature is a PREFIX of the object's (fewer trailing elements, e.g. the SpeedTree LOD/wind variants
// with 11-14 TEXCOORD outputs) reads only the registers it declares, and DCS's object writes the same values there,
// so it matches too. The object's signature is then the LONGEST matching one, provided (a) every other match is a
// prefix of it (else AMBIGUOUS) and (b) each of its elements carried a non-zero byte in DCS's run (an all-zero
// trailing element cannot tell it from a shorter signature: AMBIGUOUS).
inline bool SigElemEq(const fbc::SigElem& a, const fbc::SigElem& b) {
  return a.name == b.name && a.index == b.index && a.sysValue == b.sysValue && a.compType == b.compType &&
         a.reg == b.reg && a.mask == b.mask && a.stream == b.stream && a.minPrec == b.minPrec;
}
inline bool SigIsPrefix(const std::vector<fbc::SigElem>& a, const std::vector<fbc::SigElem>& b) {
  if (a.size() > b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i)
    if (!SigElemEq(a[i], b[i])) return false;
  return true;
}
void TallyTests(std::vector<IdTest>& tests, Info& in) {
  const IdTest* best = nullptr;
  std::vector<const IdTest*> matches;
  for (IdTest& t : tests) {
    ++in.idTried;
    if (t.state == 2) {
      ++in.idMatches;
      matches.push_back(&t);
      if (!best || t.sig.size() > best->sig.size()) best = &t;
    } else if (t.state == 3) {
      ++in.idDiff;
    } else if (t.state == 5) {
      ++in.idExcluded;
    } else {
      ++in.idFail;
    }
  }
  if (!best) return;
  in.idMatch = true;
  in.blob = best->b;
  in.matchSum = SumHex(best->b.sum);
  for (const IdTest* m : matches) {
    if (m->soSrc == best->soSrc) continue;  // the same signature
    if (!SigIsPrefix(m->sig, best->sig) || m->sig.size() == best->sig.size()) in.idAmbig = true;
    else ++in.idPrefix;
  }
  if (in.idPrefix && !best->colsNonZero) in.idAmbig = true;
  in.idLongest = static_cast<int>(best->sig.size());
}
// Config tick: the device hooks go in as soon as DCS's device is known while the key is set (source (1)).
void Apply() {
  if (g_suiteSec.load() <= 0 || g_devVt) return;
  if (!pifilt::EnsureDevice()) return;
  if (InstallDeviceHooks(pifilt::g_dev))
    Log("vs signature probe: CreateVertexShader / CreateInputLayout hooks on DCS's device (records what DCS creates "
        "from now on)");
}

struct Report {
  uint64_t triTotal = 0, triCovered = 0, triCreated = 0, triScan = 0;
  int objs = 0, objsCovered = 0;
};
// Coverage over the recorded objects (pure on the map; tested).
Report Summarise(const std::map<ID3D11VertexShader*, Info>& m) {
  Report r;
  for (auto& kv : m) {
    const Info& in = kv.second;
    ++r.objs;
    r.triTotal += in.tri;
    const bool sigKnown = (in.created || (in.idMatch && !in.idAmbig)) && in.blob.elems > 0 && in.blob.gsOk;
    if (in.created) r.triCreated += in.tri;
    if (in.idMatch && !in.idAmbig) r.triScan += in.tri;
    if (sigKnown) {
      r.triCovered += in.tri;
      ++r.objsCovered;
    }
  }
  return r;
}

void SuitePhase(std::atomic<uint64_t>& frameCounter) {
  const int sec = g_suiteSec.load();
  if (sec <= 0) return;
  (void)frameCounter;
  Log("-- R38 rank 2 gate G4: VS output-signature sources for the periphery G-buffer instancer draws, measure only "
      "(vs_sig_probe.h, %d s) --",
      sec);
  Apply();
  pifilt::g_vscDraw = &OnDraw;
  pifilt::g_vscEnd = &OnEnd;
  g_record = true;
  pifilt::g_vscOn = true;
  pifilt::Apply(false);
  for (int t = 0; t < 15000 && !pifilt::g_drawLearned.load(); t += 100) Sleep(100);
  if (!pifilt::g_drawLearned.load()) Log("  vs signature probe: the instancer draw was not identified in 15 s");
  Sleep(static_cast<DWORD>(sec) * 1000);
  g_record = false;  // the last sampled runs are read back at the next run ends
  Sleep(600);
  pifilt::g_vscOn = false;
  Sleep(200);
  pifilt::g_vscDraw = nullptr;
  pifilt::g_vscEnd = nullptr;
  // sources
  std::map<ID3D11VertexShader*, Info> m;
  {
    std::lock_guard<std::mutex> lock(g_mu);
    for (auto& kv : g_vs) {
      auto it = g_created.find(kv.first);
      if (it != g_created.end()) {
        kv.second.created = true;
        kv.second.blob = it->second;
      }
    }
    m = g_vs;
  }
  std::vector<std::pair<uintptr_t, int>> objs;
  for (auto& kv : m) objs.emplace_back(reinterpret_cast<uintptr_t>(kv.first), kv.second.mainSlot);
  uint64_t scanned = 0;
  uint32_t ms = 0;
  // R49: with an archive folder the memory scan (R45: nothing found, 48 s of scanning) is skipped.
  const std::vector<std::vector<uint8_t>> arc = LoadArchive(g_archiveDir);
  if (!g_archiveDir.empty())
    Log("  vs signature probe: archive source: %zu DXBC files in [Suite] FocusVsArchive; memory scan skipped", arc.size());
  std::map<uintptr_t, ScanOut> sc = g_archiveDir.empty() ? ScanMemory(objs, 0, &scanned, &ms) : std::map<uintptr_t, ScanOut>{};
  for (auto& kv : m) {
    auto it = sc.find(reinterpret_cast<uintptr_t>(kv.first));
    if (it == sc.end()) continue;
    kv.second.scanRefs = it->second.refs;
    kv.second.scanBlobs = it->second.blobs;
    kv.second.scanOk = it->second.ok;
    kv.second.scanOwn = it->second.own;
    kv.second.scanAmb = it->second.ambiguous;
    kv.second.scanDist = it->second.dist;
    if (!kv.second.created && it->second.best.size) kv.second.blob = it->second.best;
  }
  // Identification by output of the memory candidates (objects without a creation-hook blob)
  {
    size_t prepared = 0;
    for (auto& kv : m) {
      if (kv.second.created) continue;
      std::vector<std::vector<uint8_t>> codes;
      auto it = sc.find(reinterpret_cast<uintptr_t>(kv.first));
      if (it != sc.end()) codes = it->second.tests;
      if (!arc.empty() && kv.second.mainSlot >= 0) {
        std::vector<std::vector<uint8_t>> a = ArchiveCandidates(arc, kv.second.mainSlot);
        if (!a.empty()) kv.second.fromArchive = true;
        for (auto& x : a) codes.push_back(std::move(x));
      }
      std::vector<IdTest> v;
      for (auto& code : codes) {
        IdTest t;
        if (PrepareTest(code, t)) v.push_back(std::move(t));
      }
      prepared += v.size();
      if (!v.empty()) g_tests[kv.first] = std::move(v);
    }
    if (prepared) {
      g_filtered.clear();
      g_identify = true;
      pifilt::g_vscDraw = &OnDraw;
      pifilt::g_vscEnd = &OnEnd;
      pifilt::g_vscOn = true;
      const int waits = arc.empty() ? 100 : 400;  // R49: up to 40 s for hundreds of archive candidates
      for (int w = 0; w < waits; ++w) {  // every test run and read back
        Sleep(100);
        bool open = false;
        for (auto& kv : g_tests)
          for (IdTest& t : kv.second) open = open || t.state < 2;
        if (!open) break;
      }
      g_identify = false;
      g_cleanup = true;
      for (int w = 0; w < 30 && g_cleanup.load(); ++w) Sleep(100);
      pifilt::g_vscOn = false;
      Sleep(200);
      pifilt::g_vscDraw = nullptr;
      pifilt::g_vscEnd = nullptr;
    }
    for (auto& kv : g_tests) TallyTests(kv.second, m[kv.first]);
    Log("  vs signature probe: identification by output: %zu candidate programs prepared, %llu test runs", prepared,
        static_cast<unsigned long long>(g_testsRun.load()));
    if (!g_cleanup.load()) g_tests.clear();  // (left for Shutdown when the render thread did not release them)
  }
  const Report r = Summarise(m);
  Log("  vs signature probe: %llu instancer draws seen, %llu runs sampled, %llu read back; %d VS objects; hook since "
      "%.0f s: %llu vertex shaders and %llu input layouts created on DCS's device; memory scanned %.2f GB in %u ms",
      static_cast<unsigned long long>(g_drawsSeen.load()), static_cast<unsigned long long>(g_runsSampled.load()),
      static_cast<unsigned long long>(g_runsRead.load()), r.objs,
      g_hookTick ? (GetTickCount64() - g_hookTick) / 1000.0 : -1.0, static_cast<unsigned long long>(g_createdVs.load()),
      static_cast<unsigned long long>(g_createdIl.load()), scanned / 1073741824.0, ms);
  std::vector<std::pair<ID3D11VertexShader*, Info>> v(m.begin(), m.end());
  std::sort(v.begin(), v.end(), [](auto& a, auto& b) { return a.second.tri > b.second.tri; });
  const double tot = r.triTotal ? static_cast<double>(r.triTotal) : 1.0;
  for (auto& kv : v) {
    const Info& in = kv.second;
    Log("    VS %p: %5.1f %% of triangles (%llu draws, %llu with arguments read), mainBuffer at t%d; created after the "
        "hook %d; memory: %d references, %d distinct DXBC VS nearby, taken from %s at %lld bytes%s, declares mainBuffer's slot %d; by output%s: %d tried, %d excluded by strides, MATCH %d (%d matching, %d shorter prefix signatures, longest %d elements%s), %d differ, %d not run; signature %d "
        "elements, FastGS %s; DXBC checksum %s",
        static_cast<void*>(kv.first), 100.0 * in.tri / tot, static_cast<unsigned long long>(in.draws),
        static_cast<unsigned long long>(in.triDraws), in.mainSlot, in.created ? 1 : 0, in.scanRefs, in.scanBlobs,
        in.scanDist < 0 ? "-" : (in.scanOwn ? "the object" : "a reference"), static_cast<long long>(in.scanDist),
        in.scanAmb ? " (AMBIGUOUS)" : "", in.scanOk ? 1 : 0, in.fromArchive ? " (archive)" : "", in.idTried,
        in.idExcluded, in.idMatch ? 1 : 0, in.idMatches, in.idPrefix, in.idLongest, in.idAmbig ? ", signatures differ: AMBIGUOUS" : "", in.idDiff,
        in.idFail, in.blob.elems, in.blob.gsOk ? "ok" : "no", in.blob.size ? SumHex(in.blob.sum).c_str() : "-");
  }
  const double cov = 100.0 * r.triCovered / tot;
  Log("  vs signature probe: triangles with a usable signature %.1f %% (from the creation hook %.1f %%, from memory confirmed by output "
      "%.1f %%), %d of %d objects; periphery instancer triangles sampled %.2f M per sampled run; gate G4 (>= 90 %%): "
      "%s",
      cov, 100.0 * r.triCreated / tot, 100.0 * r.triScan / tot, r.objsCovered, r.objs,
      g_runsRead.load() ? r.triTotal / 1e6 / g_runsRead.load() : 0.0,
      !r.triTotal ? "NO DATA" : (cov >= 90.0 ? "PASS" : "FAIL"));
  // release
  {
    std::lock_guard<std::mutex> lock(g_mu);
    for (auto& kv : g_vs) kv.first->Release();
    g_vs.clear();
  }
  g_drawsSeen = g_runsSampled = g_runsRead = g_testsRun = 0;
}

void Shutdown() {
  pifilt::g_vscOn = false;
  g_record = false;
  pifilt::g_vscDraw = nullptr;
  pifilt::g_vscEnd = nullptr;
  Sleep(20);
  RemoveDeviceHooks();
  for (Pend& p : g_pend) {
    pifilt::Rel(p.st);
    p.busy = false;
    p.recs.clear();
  }
  std::lock_guard<std::mutex> lock(g_mu);
  for (auto& kv : g_vs) kv.first->Release();
  g_vs.clear();
  g_created.clear();
  for (auto& kv : g_tests)
    for (IdTest& t : kv.second) t.Release();
  g_tests.clear();
  g_filtered.clear();
  g_argsBytes = 0;
}

}  // namespace vsig
