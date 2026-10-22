// R130: READ-ONLY DLSS feature lifetime instrumentation ([DLSS] Instrument=1, src\dlss_views.h namespace life). A fake
// NGX core: the R122 fake CreateFeature, plus a fake EvaluateFeature and ReleaseFeature in executable memory that start
// with the real prologues of _nvngx.dll 32.0.16.1742, so the real detour code patches them. A test clock, fake memory
// readings (DXGI local usage, private bytes) and a fake capability block whose DLSSGetStatsCallback reports DLSS's VRAM.
// Checks: nothing installed with both keys 0; Instrument alone installs the three hooks; two sets of P,P,F,F in one
// load with only the second evaluated -> the summary flags set 1 IDLE and the release rule calls it eligible; the
// per-create and per-set memory deltas; a late evaluation of an old handle -> STILL EVALUATED and the guard refuses;
// a second mission load (summary at the load, supersede); a release; untracked handles; kill switch, key 0 and stop
// pass-through; the idle-summary skip; a bad context pointer is contained.
#pragma once

namespace r130test {

using dlssv::NgxParam;

// A fake NGX handle (NVSDK_NGX_Handle starts with its unsigned id). The fake CreateFeature returns ctx as the handle,
// so each creation passes its own FakeHandle as the context.
struct FakeHandle {
  uint32_t id;
  uint32_t pad[3];
};

int64_t g_clk = 1000000;
int64_t Clock() { return g_clk; }
void Advance(double s) { g_clk += dlssv::life::Ticks(s); }
uint64_t g_vram = 8ull << 30, g_priv = 4ull << 30, g_dlssBytes = 0;
bool Vram(void*, uint64_t* v) {
  *v = g_vram;
  return true;
}
bool Priv(uint64_t* v) {
  *v = g_priv;
  return true;
}
// The capability block NGX would return: DLSSGetStatsCallback, and SizeInBytes written by the callback.
int StatsCallback(NgxParam* p);
struct CapParam : NgxParam {
  unsigned long long size = 0;
  void Set(const char* n, unsigned long long v) override {
    if (!strcmp(n, "SizeInBytes")) size = v;
  }
  void Set(const char*, float) override {}
  void Set(const char*, double) override {}
  void Set(const char*, unsigned int) override {}
  void Set(const char*, int) override {}
  void Set(const char*, dlssv::D3D11Res*) override {}
  void Set(const char*, dlssv::D3D12Res*) override {}
  void Set(const char*, void*) override {}
  int Get(const char* n, unsigned long long* v) const override {
    if (strcmp(n, "SizeInBytes")) return static_cast<int>(0xBAD00005);
    *v = size;
    return dlssv::kNgxSuccess;
  }
  int Get(const char*, float*) const override { return static_cast<int>(0xBAD00005); }
  int Get(const char*, double*) const override { return static_cast<int>(0xBAD00005); }
  int Get(const char*, unsigned int*) const override { return static_cast<int>(0xBAD00005); }
  int Get(const char*, int*) const override { return static_cast<int>(0xBAD00005); }
  int Get(const char*, dlssv::D3D11Res**) const override { return static_cast<int>(0xBAD00005); }
  int Get(const char*, dlssv::D3D12Res**) const override { return static_cast<int>(0xBAD00005); }
  int Get(const char* n, void** v) const override {
    if (strcmp(n, "DLSSGetStatsCallback")) return static_cast<int>(0xBAD00005);
    *v = reinterpret_cast<void*>(&StatsCallback);
    return dlssv::kNgxSuccess;
  }
  void Reset() override {}
};
CapParam g_capBlock;
int g_capCalls = 0, g_statsCalls = 0;
int StatsCallback(NgxParam* p) {
  ++g_statsCalls;
  p->Set("SizeInBytes", static_cast<unsigned long long>(g_dlssBytes));
  return dlssv::kNgxSuccess;
}
int __cdecl CapParams(NgxParam** out) {
  ++g_capCalls;
  *out = &g_capBlock;
  return dlssv::kNgxSuccess;
}

// The fake core's creation cost: what one DLSS feature allocates (the fake create body runs through r122test's).
constexpr uint64_t kFeatVram = 100ull << 20, kFeatPriv = 50ull << 20, kFeatDlss = 80ull << 20;
int __cdecl CostlyCreateBody(void* ctx, int feature, NgxParam* params, void** handle) {
  g_vram += kFeatVram, g_priv += kFeatPriv, g_dlssBytes += kFeatDlss;
  return r122test::FakeCreateBody(ctx, feature, params, handle);
}
int g_evalCalls = 0, g_relCalls = 0;
int __cdecl FakeEvalBody(void*, const void* h, const NgxParam*, void*) {
  ++g_evalCalls;
  return h ? dlssv::kNgxSuccess : static_cast<int>(0xBAD00005);
}
int __cdecl FakeReleaseBody(void* h) {
  ++g_relCalls;
  return h ? dlssv::kNgxSuccess : static_cast<int>(0xBAD00005);
}
// _nvngx.dll 32.0.16.1742 prologues: EvaluateFeature (15 bytes of home-area stores; the decoder copies those) and
// ReleaseFeature (mov [rsp+10h],rbx; mov [rsp+18h],rsi; push rdi; sub rsp,20h).
const uint8_t kEvalPrologue[15] = {0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x6c, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18};
const uint8_t kRelPrologue[15] = {0x48, 0x89, 0x5c, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57, 0x48, 0x83, 0xec, 0x20};

struct Rig {
  uint8_t* create = nullptr;
  uint8_t* eval = nullptr;
  uint8_t* rel = nullptr;
  bool ok() const { return create && eval && rel; }
};
Rig MakeRig() {
  Rig r;
  const uint8_t undoCreate[] = {0x48, 0x83, 0xC4, 0x40, 0x41, 0x5E, 0x5F, 0x5E, 0x48, 0x8B, 0x6C, 0x24, 0x18};
  r.create = r122test::MakeFake(r122test::kCreatePrologue, sizeof(r122test::kCreatePrologue), undoCreate,
                                sizeof(undoCreate), reinterpret_cast<void*>(&CostlyCreateBody));
  r.eval = r122test::MakeFake(kEvalPrologue, sizeof(kEvalPrologue), nullptr, 0, reinterpret_cast<void*>(&FakeEvalBody));
  const uint8_t undoRel[] = {0x48, 0x83, 0xC4, 0x20, 0x5F};  // add rsp,20h; pop rdi
  r.rel = r122test::MakeFake(kRelPrologue, sizeof(kRelPrologue), undoRel, sizeof(undoRel),
                             reinterpret_cast<void*>(&FakeReleaseBody));
  return r;
}

void Fresh(const Rig& rig) {
  r122test::NewGeneration();
  dlssv::life::ResetForTest();
  dlssv::g_testCreateTarget = rig.create;
  dlssv::life::g_testEvalTarget = rig.eval;
  dlssv::life::g_testReleaseTarget = rig.rel;
  dlssv::life::g_testClock = &Clock;
  dlssv::life::g_testVram = &Vram;
  dlssv::life::g_testPriv = &Priv;
  dlssv::life::g_testCapParams = &CapParams;
  dlssv::g_testQvfrLog = r122test::WriteQvfrLog(r122test::kQvfrText);
  g_capBlock.size = 0;
}

bool Has(const std::string& s, const char* what) { return s.find(what) != std::string::npos; }
// The log lines that contain `key`, joined.
std::string Lines(const std::string& log, const char* key) {
  std::string out;
  size_t at = 0;
  while (at < log.size()) {
    size_t e = log.find('\n', at);
    if (e == std::string::npos) e = log.size();
    const std::string l = log.substr(at, e - at);
    if (l.find(key) != std::string::npos) out += l + "\n";
    at = e + 1;
  }
  return out;
}

FakeHandle g_h[32];
// One DCS-style set: P, P, F, F, 7 ms apart on the test clock; handles from g_h[first..first+3] with ids first+1...
void CreateSet(const Rig& rig, int first) {
  const auto create = reinterpret_cast<dlssv::CreateFn>(rig.create);
  r122test::FakeParam p;
  for (int i = 0; i < 4; ++i) {
    FakeHandle& h = g_h[first + i];
    h.id = static_cast<uint32_t>(first + i + 1);
    void* out = nullptr;
    if (i < 2)
      r122test::DcsFeature(p, 604, 596, 1040, 1028);
    else
      r122test::DcsFeature(p, 1176, 1164, 2028, 2004);
    create(&h, dlssv::kFeatureSuperSampling, &p, &out);
    Advance(0.007);
  }
}
void EvalHandles(const Rig& rig, int first, int count, int times) {
  const auto eval = reinterpret_cast<dlssv::EvalFn>(rig.eval);
  r122test::FakeParam p;
  for (int t = 0; t < times; ++t) {
    for (int i = 0; i < count; ++i) eval(nullptr, &g_h[first + i], &p, nullptr);
    Advance(1.0 / 90);
  }
}

void KeysOff(const Rig& rig) {
  using namespace dlssv;
  Fresh(rig);
  StartReportCapture();
  Tick(0, 0, false, false, false);
  const std::string log = StopReportCapture();
  Check(g_createState.load() == 0 && life::g_evalState.load() == 0 && life::g_releaseState.load() == 0 &&
            memcmp(rig.create, r122test::kCreatePrologue, 16) == 0 && memcmp(rig.eval, kEvalPrologue, 15) == 0 &&
            memcmp(rig.rel, kRelPrologue, 15) == 0 && log.find("dlss") == std::string::npos,
        "R130 Instrument=0 and no preset: nothing installed, no code touched, nothing logged");
}

void TwoSets(const Rig& rig) {
  using namespace dlssv;
  using namespace dlssv::life;
  Fresh(rig);
  StartReportCapture();
  Tick(0, 0, false, false, true);  // Instrument=1, no [DLSS] preset keys
  const std::string inst = StopReportCapture();
  Check(g_createState.load() == 1 && g_evalState.load() == 1 && g_releaseState.load() == 1 && rig.eval[0] == 0xFF &&
            rig.rel[0] == 0xFF && Has(inst, "dlss life: on ([DLSS] Instrument=1") &&
            Has(inst, "NVSDK_NGX_D3D11_EvaluateFeature hooked (patched; observation only") &&
            Has(inst, "NVSDK_NGX_D3D11_ReleaseFeature hooked (patched; observation only"),
        "R130 Instrument=1 without preset keys: CreateFeature, EvaluateFeature and ReleaseFeature hooked");

  // A mission load: two sets of P,P,F,F, 7 ms apart (R127), every creation costing the fake core 100 / 50 / 80 MB.
  g_evalCalls = 0;
  StartReportCapture();
  CreateSet(rig, 0);
  CreateSet(rig, 4);
  const std::string creates = StopReportCapture();
  bool sets = g_n.load() == 8;
  for (int i = 0; i < 8 && sets; ++i)
    sets = g_f[i].set == static_cast<uint32_t>(i < 4 ? 1 : 2) && g_f[i].load == 1 && g_f[i].id == static_cast<uint32_t>(i + 1) &&
           g_f[i].cls == ((i & 3) < 2 ? kPeriph : kFocus) && (i < 4 ? g_f[i].superBy.load() == 2 && g_f[i].superQpc.load() != 0
                                                                  : g_f[i].superQpc.load() == 0);
  Check(sets, "R130 sets: P,P,F,F,P,P,F,F in one load -> set 1 (ids 1-4, superseded by set 2) and set 2 (ids 5-8)");
  Check(Has(creates, "create #1 periphery 604x596->1040x1028: unchanged, off ([DLSS] PeripheryPreset=0)") &&
            Has(creates, "; life: set 1, load 1; VRAM local +100.0 MB, private +50.0 MB, DLSS stats +80.0 MB") &&
            Has(creates, "create #8 focus 1176x1164->2028x2004") && Has(creates, "; life: set 2, load 1; VRAM local +100.0 MB"),
        "R130 VRAM delta: each create line carries the DXGI local, private-bytes and DLSS-stats deltas of its creation");
  Check(g_capCalls >= 1 && g_statsCalls == 16, "R130 DLSS stats: our own capability block, the stats callback twice per creation");

  // Only the second set is evaluated (400 frames).
  EvalHandles(rig, 4, 4, 400);
  bool counts = g_evalCalls == 1600;
  for (int i = 0; i < 8; ++i)
    counts = counts && g_f[i].evals.load() == (i < 4 ? 0u : 400u) && g_f[i].evalsAfterSuper.load() == 0;
  Check(counts && g_untracked.load() == 0, "R130 counters: 400 evaluations on each set-2 handle, 0 on set 1, all passed through");

  // The load closes 2 s after its last creation: per-set totals.
  Advance(2.5);
  StartReportCapture();
  Tick(0, 0, false, false, true);
  const std::string totals = StopReportCapture();
  Check(Has(totals, "dlss life: load 1 done: 8 DLSS creations") &&
            Has(totals, "set 1 ids 1-4 (P,P,F,F): VRAM +400.0 MB, private +200.0 MB, DLSS stats +320.0 MB;") &&
            Has(totals, "set 2 ids 5-8 (P,P,F,F): VRAM +400.0 MB, private +200.0 MB, DLSS stats +320.0 MB;") &&
            Has(totals, "load total VRAM +800.0 MB, private +400.0 MB, DLSS stats +640.0 MB"),
        "R130 VRAM delta: the per-set and per-load totals are logged when the load closes");

  // The 10 s summary flags set 1 as idle and the release rule as eligible.
  Advance(10.0);
  StartReportCapture();
  Tick(0, 0, false, false, true);
  const std::string sum = StopReportCapture();
  Check(Has(sum, "dlss life: summary (periodic") && Has(sum, "8 live DLSS handles in sets 1-2 (newest set 2, load 1)") &&
            Has(sum, "1600 evaluations since the last summary (0 of untracked handles, 0 failed)") &&
            Has(sum, "dlss life:   set 2 (newest, load 1): id 5 P +400 (total 400) last") && Has(sum, "id 8 F +400") &&
            Has(sum, "IDLE older sets (no evaluation since superseded): set 1 (ids 1-4, load 1, superseded by set 2") &&
            Has(sum, "never evaluated);") && Has(sum, "[4 handle(s)]") && !Has(sum, "STILL EVALUATED"),
        "R130 summary: the old set (never evaluated after set 2 was created) is flagged IDLE; set 2 per handle");
  Check(Has(sum, "release rule (observation only, N=300; newest set 2 min 400 evals per handle): set 1 eligible;"),
        "R130 release rule: set 1 eligible (idle since superseded, set 2 evaluated >= N times)");

  // A quiet period: one idle summary, then skipped until something happens.
  Advance(10.0);
  StartReportCapture();
  Tick(0, 0, false, false, true);
  Advance(10.0);
  Tick(0, 0, false, false, true);
  const std::string quiet = StopReportCapture();
  size_t nSum = 0;
  for (size_t at = quiet.find("dlss life: summary"); at != std::string::npos; at = quiet.find("dlss life: summary", at + 1)) ++nSum;
  Check(nSum == 1 && Has(quiet, "0 evaluations since the last summary"),
        "R130 rate limit: one summary with nothing new, then none until something changes");

  // The guard: an old handle evaluated again.
  EvalHandles(rig, 0, 1, 1);
  EvalHandles(rig, 4, 4, 10);
  Advance(10.0);
  StartReportCapture();
  Tick(0, 0, false, false, true);
  const std::string guard = StopReportCapture();
  Check(g_f[0].evalsAfterSuper.load() == 1 &&
            Has(guard, "dlss life:   set 1 (load 1, superseded by set 2") && Has(guard, "STILL EVALUATED (1 since superseded): id 1 P +1 (total 1)") &&
            Has(guard, "id 2 P never evaluated;") && Has(guard, "set 1 REFUSED (guard: evaluated after superseded);") &&
            !Has(guard, "IDLE older sets"),
        "R130 guard: an old-set handle evaluated after it was superseded -> STILL EVALUATED, the rule refuses that set");

  // A second mission load: the summary is logged at the load, before its first creation; set 2 superseded by set 3.
  Advance(5.0);
  StartReportCapture();
  CreateSet(rig, 8);
  const std::string load2 = StopReportCapture();
  const size_t sumAt = load2.find("dlss life: summary (mission load"), createAt = load2.find("create #9 ");
  Check(sumAt != std::string::npos && createAt != std::string::npos && sumAt < createAt && g_f[8].set == 3 &&
            g_f[8].load == 2 && g_f[4].superBy.load() == 3 && g_f[0].superBy.load() == 2,
        "R130 mission load: a summary before the new load's first creation; the new set supersedes set 2 (set 1 keeps 'by set 2')");
  CreateSet(rig, 12);  // DCS's second set of that load
  Check(g_f[12].set == 4 && g_f[8].superBy.load() == 4 && g_load == 2, "R130 mission load: its second set is set 4");

  // Release (DCS is not expected to release; if it does, the entry leaves the live set).
  const auto rel = reinterpret_cast<ReleaseFn>(rig.rel);
  StartReportCapture();
  const int rr = rel(&g_h[4]);
  const std::string rlog = StopReportCapture();
  Check(rr == kNgxSuccess && g_relCalls == 1 && g_f[4].released.load() && g_releases.load() == 1 &&
            Has(rlog, "dlss life: DCS released id 5 (set 2, load 1, P): evaluated 410 times, last"),
        "R130 release: passed through, the entry marked released and logged");
  const uint64_t before = g_f[4].evals.load();
  EvalHandles(rig, 4, 1, 3);  // the released handle's memory: untracked from now on
  Check(g_f[4].evals.load() == before && g_untracked.load() == 3, "R130 a released or unknown handle's evaluations count as untracked");

  // Kill switch and key 0: pass-through, nothing counted.
  Tick(0, 0, false, true, true);
  EvalHandles(rig, 12, 4, 5);
  const bool killOk = g_f[12].evals.load() == 0 && g_evalCalls > 0;
  Tick(0, 0, false, false, false);
  EvalHandles(rig, 12, 4, 5);
  Check(killOk && g_f[12].evals.load() == 0 && !g_on.load(), "R130 kill switch and Instrument=0: evaluations pass through, uncounted");
  Tick(0, 0, false, false, true);
  EvalHandles(rig, 12, 4, 5);
  Check(g_f[12].evals.load() == 5, "R130 Instrument=1 again: counting resumes on the same detours");

  // The install instant: the slot already points at the hook, g_evalOrig / g_releaseOrig / g_createOrig not stored yet.
  {
    const EvalFn savedE = g_evalOrig.load();
    const ReleaseFn savedR = g_releaseOrig.load();
    const CreateFn savedC = dlssv::g_createOrig.load();
    g_evalOrig = nullptr, g_releaseOrig = nullptr, dlssv::g_createOrig = nullptr;
    const int e0 = g_evalCalls, r0 = g_relCalls, c0 = r122test::g_createCalls;
    EvalHandles(rig, 12, 1, 1);
    const int rr2 = reinterpret_cast<ReleaseFn>(rig.rel)(&g_h[15]);
    r122test::FakeParam fp;
    r122test::DcsFeature(fp, 604, 596, 1040, 1028);
    void* out = nullptr;
    g_h[20].id = 21;
    reinterpret_cast<CreateFn>(rig.create)(&g_h[20], kFeatureSuperSampling, &fp, &out);
    g_evalOrig = savedE, g_releaseOrig = savedR, dlssv::g_createOrig = savedC;
    Check(g_evalCalls == e0 + 1 && g_relCalls == r0 + 1 && rr2 == kNgxSuccess && r122test::g_createCalls == c0 + 1 &&
              out == &g_h[20],
          "R130 hooks called before their original pointer is stored fall back to the detour's trampoline (no null call)");
  }

  // Stop: pass-through.
  Stop();
  const int callsBefore = g_evalCalls;
  const uint64_t evalsBefore = g_f[12].evals.load(), totalBefore = g_evalTotal.load();
  EvalHandles(rig, 12, 4, 2);
  Check(g_evalCalls == callsBefore + 8 && g_f[12].evals.load() == evalsBefore && g_evalTotal.load() == totalBefore,
        "R130 stop: the evaluate hook passes through");
  Check(memcmp(rig.eval, "\xFF\x25", 2) == 0, "R130 stop: the patch stays for the next payload (as R122)");

  // A context pointer that is not a device context: contained, no reading.
  g_testVram = nullptr;
  uint64_t v = 0;
  Check(!LocalUsage(reinterpret_cast<void*>(0x10), &v), "R130 a bad context pointer for the VRAM reading is contained (SEH)");
}

// The release rule's verdicts (observation only) on a hand-built table: eligible only when every old handle has 0
// evaluations since superseded and every newest-set handle has >= N; refused when any old handle was evaluated after.
void RuleVerdicts() {
  using namespace dlssv::life;
  ResetForTest();
  for (int i = 0; i < 8; ++i) {
    g_f[i].handle = &g_h[i], g_f[i].id = i + 1, g_f[i].set = i < 4 ? 1 : 2;
    if (i < 4) g_f[i].superQpc = 5, g_f[i].superBy = 2;
    g_f[i].evals = i < 4 ? 7 : 400;  // set 1 evaluated before it was superseded: still eligible
  }
  g_n = 8;
  Rule nr = NewestMin(8, 2);
  const int v1 = SetVerdict(8, 1, nr, 300), v2 = SetVerdict(8, 2, nr, 300);
  g_f[7].evals = 299;
  nr = NewestMin(8, 2);
  const int notYet = SetVerdict(8, 1, nr, 300);
  g_f[7].evals = 400;
  g_f[2].evalsAfterSuper = 1;
  nr = NewestMin(8, 2);
  const int refused = SetVerdict(8, 1, nr, 300);
  g_f[2].evalsAfterSuper = 0;
  g_f[6].released = true;  // a released newest handle does not count
  g_f[7].evals = 10;
  g_f[6].evals = 0;
  nr = NewestMin(8, 2);
  Check(v1 == 1 && v2 == -2 && notYet == 0 && refused == -1 && nr.newestMin == 10,
        "R130 release rule: eligible / not yet (a newest handle below N) / refused by the guard / newest set not a candidate");
  ResetForTest();
}

void Run() {
  printf("-- R130 DLSS feature lifetime instrumentation (read-only) --\n");
  const Rig rig = MakeRig();
  if (!rig.ok()) {
    QvSkip("R130: no executable memory\n");
    return;
  }
  KeysOff(rig);
  TwoSets(rig);
  RuleVerdicts();
  dlssv::life::ResetForTest();
  r122test::NewGeneration();
}

}  // namespace r130test
