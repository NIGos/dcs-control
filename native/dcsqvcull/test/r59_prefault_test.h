// R59 helper-prefault-chunk-uncounted: a helper whose stage-A read faulted
// (contained, the job marked recycled) never counts its chunk; the primary
// must not wait out kPreWaitUs for it.
#pragma once

namespace r59pre {

// R60: "returns at once" is a wall-clock bound (under half of kPreWaitUs). A test thread preempted between its two
// clock reads (a loaded machine) only makes an attempt longer, so the bound must hold in the best of 20 attempts;
// every attempt must return the right answer.
constexpr int kAttempts = 20;

void Run() {
  {
    using namespace gbrec;
    bool ok = true, r = true;
    double us = 1e30;
    for (int a = 0; a < kAttempts && ok && us >= kPreWaitUs / 2; ++a) {
      Job* j = NewJob();
      if (!j) {
        ok = false;
        break;
      }
      j->n = 3 * kPreChunk;
      j->preNext.store(static_cast<uint32_t>(j->n));  // every chunk taken
      j->preDone.store(2 * kPreChunk);                 // one helper faulted inside its chunk
      j->recycled.store(1);
      const int64_t t0 = defrec::Qpc();
      r = WaitPre(*j);
      us = std::min(us, (defrec::Qpc() - t0) * defrec::QpcToUs());
      ok = r;
      // Not recycled: a stalled helper still times out (false).
      if (a == 0) {
        j->recycled.store(0);
        ok = ok && !WaitPre(*j);
      }
      FreeJob(j);
    }
    ok = ok && us < kPreWaitUs / 2;
    if (!ok) printf("     gb WaitPre: %s after %.0f us\n", r ? "true" : "false", us);
    Check(ok, "R59 gb rec: WaitPre returns at once on a recycled job (a helper's contained fault left its chunk "
              "uncounted), still times out otherwise");
  }
  {
    using namespace shrec;
    bool ok = true, r = false;
    double us = 1e30;
    for (int a = 0; a < kAttempts && ok && us >= kPreWaitUs / 2; ++a) {
      Job* j = NewJob();
      if (!j) {
        ok = false;
        break;
      }
      j->n = 3 * kPreChunk;
      j->gen = 7;
      j->preNext.store(static_cast<uint32_t>(j->n));  // chunk 0 held by a helper that faulted: never published
      j->recycled.store(1);
      const int64_t t0 = defrec::Qpc();
      r = PreAndCommit(*j);
      us = std::min(us, (defrec::Qpc() - t0) * defrec::QpcToUs());
      ok = !r && j->result == kBuildNone;
      FreeJob(j);
    }
    ok = ok && us < kPreWaitUs / 2;
    if (!ok) printf("     shadow PreAndCommit: %s after %.0f us\n", r ? "true" : "false", us);
    Check(ok, "R59 shadow rec: PreAndCommit stops waiting for an unpublished chunk once the job is recycled");
  }
}

}  // namespace r59pre
