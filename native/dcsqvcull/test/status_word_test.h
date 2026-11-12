// Offline test for status_word.h: the DcsQvCull_Status bit layout, feature
// counting, and the loader/payload handover of the loader-owned word (a
// stopped payload never writes again, 0 during a swap).
#pragma once

namespace swtest {

using namespace qvstatus;

void Layout() {
  Check(Pack(false, true, 5, 3) == 0, "status word: not running packs to 0");
  const uint64_t w = Pack(true, true, 17, 0);
  Check(w == (1ull | 2ull | (17ull << 8)), "status word: running + active + 17 active, nothing latched");
  Check(Running(w) && Optimizing(w) && !AnyLatched(w) && ActiveCount(w) == 17 && LatchedCount(w) == 0,
        "status word: accessors read the packed fields");
  const uint64_t l = Pack(true, false, 2, 3);
  Check(l == (1ull | 4ull | (2ull << 8) | (3ull << 16)), "status word: kill switch clears bit 1, latched sets bit 2");
  Check(Pack(true, true, 0, 0) == 3ull, "status word: running with nothing active is 3");
  const uint64_t c = Pack(true, true, 1000, 300);
  Check(ActiveCount(c) == 255 && LatchedCount(c) == 255 && (c >> 24) == 0,
        "status word: counts saturate at 255 and stay within bits 8-23");
  const uint64_t g = Pack(true, true, 12, 1, 17);
  Check(g == (1ull | 2ull | 4ull | (12ull << 8) | (1ull << 16) | (17ull << 24)) && ConfiguredCount(g) == 17,
        "status word: configured count in bits 24-31");
  Check(ConfiguredCount(Pack(true, true, 0, 0, 1000)) == 255 && (Pack(true, true, 0, 0, 1000) >> 32) == 0,
        "status word: configured saturates at 255 and stays within bits 24-31");
  Check(Pack(false, true, 0, 0, 17) == 0, "status word: not running packs to 0 even with features configured");
}

void Counting() {
  const Feature f[] = {{true, false}, {true, false}, {false, true}, {true, true}, {false, false}};
  const uint64_t w = Compose(f, 5, true, false);
  Check(ActiveCount(w) == 2 && LatchedCount(w) == 2 && AnyLatched(w) && Optimizing(w),
        "status word: a latched feature counts as latched, never as active");
  Check(!Optimizing(Compose(f, 5, true, true)) && Running(Compose(f, 5, true, true)),
        "status word: kill switch keeps bit 0, clears bit 1");
  Check(Compose(f, 5, false, false) == 0, "status word: not running composes to 0");
  Check(Compose(f, 0, true, false) == 3ull, "status word: no features, running and active");
  Check(ConfiguredCount(w) == 0, "status word: features without a configured flag count 0 configured");
  // Configured counts every ini-enabled feature, whether active, idle or latched.
  const Feature g[] = {{true, false, true}, {false, false, true}, {false, true, true}, {false, false, false}};
  const uint64_t gw = Compose(g, 4, true, false);
  Check(ConfiguredCount(gw) == 3 && ActiveCount(gw) == 1 && LatchedCount(gw) == 1,
        "status word: configured includes idle and latched features");
  Check(ConfiguredCount(Compose(g, 4, true, true)) == 3, "status word: kill switch leaves the configured count");
  // The payload's real composition (no DCS modules here): running, within the 18 features.
  const uint64_t real = ComputeStatus();
  Check(Running(real) && ActiveCount(real) + LatchedCount(real) <= 18 && ConfiguredCount(real) <= 18,
        "status word: ComputeStatus reports running and at most 18 production features");
  // A feature latched off this session stays latched under the kill switch
  // (CostWeights: configured, its hook never installed). It read "latched"
  // only while the kill switch was off, so bit 2 and the latched count
  // dropped when the pilot pressed it.
  {
    const bool cfg0 = g_cfg.costWeights, on0 = g_costWeightsOn.load(), off0 = g_engineOff.load();
    auto* orig0 = partw::g_orig;
    partw::g_orig = nullptr;
    g_cfg.costWeights = true;
    g_costWeightsOn = true;
    g_engineOff = false;
    const uint64_t live = ComputeStatus();
    const bool latchedLive = g_featLast[8].latched;
    g_costWeightsOn = false;  // what SetEngineOff(true) does to it
    g_engineOff = true;
    const uint64_t killed = ComputeStatus();
    const bool latchedKilled = g_featLast[8].latched;
    Check(latchedLive && latchedKilled && LatchedCount(killed) == LatchedCount(live) && AnyLatched(killed) &&
              !Optimizing(killed),
          "status word: a latched feature stays latched under the kill switch (bit 2, latched count)");
    partw::g_orig = orig0;
    g_cfg.costWeights = cfg0;
    g_costWeightsOn = on0;
    g_engineOff = off0;
  }
}

void Handover() {
  std::atomic<uint64_t> word{0};  // the loader's
  Sink a, b;                      // two payload generations
  a.Publish(0x123);
  Check(word.load() == 0, "status handover: a payload never attached does not write");
  a.Attach(&word);
  a.Publish(Pack(true, true, 4, 0));
  Check(ActiveCount(word.load()) == 4, "status handover: the attached payload publishes");
  // Loader swap: 0, stop previous (detach), attach the new one, start.
  word.store(0);
  a.Detach();
  a.Publish(Pack(true, true, 9, 0));
  Check(word.load() == 0, "status handover: a stopped payload never writes again");
  b.Attach(&word);
  Check(word.load() == 0, "status handover: 0 until the new payload publishes");
  b.Publish(Pack(true, false, 1, 1));
  a.Publish(Pack(true, true, 9, 0));
  Check(word.load() == Pack(true, false, 1, 1), "status handover: only the new payload's value is seen");
  b.Detach();
  Check(word.load() == 0, "status handover: stop stores 0");

  // Concurrency: a worker publishing as fast as it can while Stop detaches.
  for (int round = 0; round < 50; ++round) {
    Sink s;
    word.store(0);
    s.Attach(&word);
    std::atomic<bool> go{true};
    std::thread t([&] {
      while (go.load()) s.Publish(Pack(true, true, 7, 0));
    });
    while (word.load() == 0) std::this_thread::yield();
    s.Detach();
    bool clean = true;
    for (int i = 0; i < 2000; ++i)
      if (word.load(std::memory_order_acquire) != 0) clean = false;
    go = false;
    t.join();
    if (!clean || word.load() != 0) {
      Check(false, "status handover: no write after detach under concurrent publishing");
      return;
    }
  }
  Check(true, "status handover: no write after detach under concurrent publishing (50 rounds)");
}

// Warm-up progress: bits 44-52 and warmup.json (warmup_status.h).
void WarmProgress() {
  const uint64_t base = Pack(true, true, 12, 0, 17) | kWarming;
  const uint64_t w = WithWarmProgress(base, 60, false);
  Check(WarmValid(w) && WarmPercent(w) == 60 && !Prewarming(w) && (w & ~(kWarmValid | (0x7full << kWarmPercentShift))) == base,
        "warm progress: 60% valid in bits 44-51, the rest of the word unchanged");
  Check(WithWarmProgress(base, -1, false) == base, "warm progress: none adds nothing (an old panel sees the same word)");
  Check(WarmPercent(WithWarmProgress(base, 250, true)) == 100 && Prewarming(WithWarmProgress(base, 250, true)),
        "warm progress: clamped to 100, prewarm bit 52");
  Check(WithWarmProgress(0, 50, true) == 0, "warm progress: not running stays 0");
  Check((WithWarmProgress(base, 100, true) >> 53) == 0 && kWarmPercentShift == 44 && kWarmValid == (1ull << 51) &&
            kPrewarm == (1ull << 52) && kCockpitSymbology == (1ull << 43),
        "warm progress: bits 44-52 only, above bit 43 (R95)");
  Check(ProgressPercent(120, 80) == 60 && ProgressPercent(0, 5) == 0 && ProgressPercent(0, 0) == 100 &&
            ProgressPercent(999, 1) == 99 && ProgressPercent(7, 0) == 100,
        "warm progress: done over done + pending, 99 at most while pending");
  qvwarm::State s;
  s.state = "warming";
  s.done = 120;
  s.total = 200;
  s.hits = 80;
  s.compiled = 40;
  s.seconds = 34.6;
  Check(qvwarm::Json(s) ==
            "{\"version\":1,\"state\":\"warming\",\"done\":120,\"total\":200,\"hits\":80,\"compiled\":40,\"seconds\":35}\n",
        "warmup.json: the documented layout");
  s.total = 3;
  s.seconds = -1;
  Check(qvwarm::Json(s).find("\"total\":120,") != std::string::npos && qvwarm::Json(s).find("\"seconds\":0}") != std::string::npos,
        "warmup.json: total never below done, no negative seconds");
  wchar_t tmp[MAX_PATH];
  GetTempPathW(MAX_PATH, tmp);
  const std::wstring dir = std::wstring(tmp) + L"qv_warmup_test_" + std::to_wstring(GetCurrentProcessId()) + L"\\";
  CreateDirectoryW(dir.c_str(), nullptr);
  const bool w1 = qvwarm::Write(dir, qvwarm::Json(s));
  s.state = "done";
  const bool w2 = qvwarm::Write(dir, qvwarm::Json(s));
  std::string read;
  if (FILE* f = _wfopen((dir + L"warmup.json").c_str(), L"rb")) {
    char b[512];
    const size_t n = fread(b, 1, sizeof(b), f);
    read.assign(b, n);
    fclose(f);
  }
  Check(w1 && w2 && read == qvwarm::Json(s) && GetFileAttributesW((dir + L"warmup.json.tmp").c_str()) == INVALID_FILE_ATTRIBUTES,
        "warmup.json: replaced atomically, no temp file left");
  Check(!qvwarm::Write(L"", "{}"), "warmup.json: no folder, nothing written");
  // Publish: a failed final write is not remembered as written, so the next tick tries it again.
  {
    std::string written = "warming";
    qvwarm::State idle;
    const bool failed = !qvwarm::Publish(written, L"", idle, false) && written == "warming";
    const bool retried = qvwarm::Publish(written, dir, idle, false) && written == "idle";
    const bool once = !qvwarm::Publish(written, dir, idle, false);
    qvwarm::State moving;
    moving.state = "prewarm";
    const bool every = qvwarm::Publish(written, dir, moving, true) && qvwarm::Publish(written, dir, moving, true) &&
                       written == "prewarm";
    Check(failed && retried && once && every,
          "warmup.json: a final state is written once, retried after a failed write; a moving one every tick");
  }
  DeleteFileW((dir + L"warmup.json").c_str());
  RemoveDirectoryW(dir.c_str());
}

void Run() {
  WarmProgress();
  Layout();
  Counting();
  Handover();
}

}  // namespace swtest
