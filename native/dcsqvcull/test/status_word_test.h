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

void Run() {
  Layout();
  Counting();
  Handover();
}

}  // namespace swtest
