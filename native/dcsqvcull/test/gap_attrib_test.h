// Offline test for gap_attrib.h's analysis: clock calibration (fit and the
// causality fallback), the per-boundary split gap = ops + starve + rest, the
// GPU lag, the boundary keys, the gap statistics match and the exclusion of
// calibration frames.
#pragma once

namespace gatest {

void* const kA = reinterpret_cast<void*>(0x1000);
void* const kB = reinterpret_cast<void*>(0x2000);
void* const kC = reinterpret_cast<void*>(0x3000);
void* const kN = reinterpret_cast<void*>(0x4000);

std::string NameOf(void* p) {
  if (p == kA) return "A";
  if (p == kB) return "B";
  if (p == kC) return "C";
  return "N";
}

constexpr double kQpc = 1e7;  // QPC ticks per second
constexpr uint64_t kFreq = 1000000000ull;

struct Builder {
  gpt::Done d;
  explicit Builder(uint64_t serial) {
    d.serial = serial;
    d.freq = kFreq;
  }
  // gpu and cpu in ms
  Builder& E(gpt::EvType t, void* pass, uint8_t depth, double gpuMs, double cpuMs) {
    d.ev.push_back({pass, static_cast<uint8_t>(t), depth});
    d.ticks.push_back(static_cast<uint64_t>(std::llround(gpuMs * 1e6)));
    d.cpu.push_back(static_cast<int64_t>(std::llround(cpuMs * kQpc / 1000.0)));
    return *this;
  }
};

bool Near(double a, double b, double eps = 1e-6) { return std::fabs(a - b) < eps; }

// A calibration frame: the stamp runs 0.01 ms after c0, the Flush returns 0.02 ms after c0, offset 1000 ms.
gpt::Done CalFrame(uint64_t serial, double c0) {
  Builder b(serial);
  b.E(gpt::kCal, nullptr, 0, c0 + 1000.0 + 0.01, c0);
  b.E(gpt::kBegin, kA, 0, c0 + 1000.1, c0 + 0.05).E(gpt::kEnd, kA, 0, c0 + 1000.2, c0 + 0.06);
  b.d.calAfter = static_cast<int64_t>(std::llround((c0 + 0.02) * kQpc / 1000.0));
  return b.d;
}

void Split() {
  std::vector<gpt::Done> done;
  done.push_back(CalFrame(0, 100.0));
  {
    // Excluded (follows a calibration frame).
    Builder b(1);
    b.E(gpt::kBegin, kA, 0, 1200.0, 150.0).E(gpt::kEnd, kA, 0, 1201.0, 151.0);
    b.E(gpt::kBegin, kB, 0, 1209.0, 152.0).E(gpt::kEnd, kB, 0, 1210.0, 153.0);
    done.push_back(b.d);
  }
  {
    Builder b(3);
    // A: GPU 5000-5002, its end issued at cpu 3991.
    b.E(gpt::kBegin, kA, 0, 5000.0, 3990.0).E(gpt::kEnd, kA, 0, 5002.0, 3991.0);
    // An outside-pass dispatch, GPU 5002.1-5002.4.
    b.E(gpt::kOpBegin, reinterpret_cast<void*>(static_cast<intptr_t>(gpt::kDispatch)), 0, 5002.1, 3991.5);
    b.E(gpt::kOpEnd, reinterpret_cast<void*>(static_cast<intptr_t>(gpt::kDispatch)), 0, 5002.4, 3991.6);
    // B issued at cpu 4002.6 (GPU clock 5002.6), runs at 5003: gap 1.0 = ops 0.3 + starve 0.3 + rest 0.4, lag 0.4.
    b.E(gpt::kBegin, kB, 0, 5003.0, 4002.6);
    b.E(gpt::kBegin, kN, 1, 5003.1, 4002.6).E(gpt::kEnd, kN, 1, 5003.5, 4002.7);  // nested: no boundary
    b.E(gpt::kEnd, kB, 0, 5004.0, 4003.0);
    // C issued long before (cpu 3995 -> 4995): gap 0.2, all rest, lag 9.2.
    b.E(gpt::kBegin, kC, 0, 5004.2, 3995.0).E(gpt::kEnd, kC, 0, 5005.0, 3995.1);
    // A again (A#1): issued after B ended on the GPU... gap 0.5 with no ops: all starved (tB 5005.6 > 5005.5).
    b.E(gpt::kBegin, kA, 0, 5005.5, 4005.6).E(gpt::kEnd, kA, 0, 5006.0, 4005.7);
    b.d.gapEv = {1};  // the gap after A#0's end stamp
    b.d.gapStats = {{nullptr, 0, 0, 0, 0, 2000000, 0}};
    done.push_back(b.d);
  }
  done.push_back(CalFrame(10, 9000.0));
  gapat::Result r = gapat::Analyze(done, &NameOf, kQpc);
  Check(r.cal.fromCal && r.cal.n == 2, "gapat: two calibration samples used");
  Check(Near(r.cal.Off(4000.0), 1000.0, 1e-4), "gapat: fitted offset is 1000 ms");
  Check(Near(r.cal.halfMs, 0.01, 1e-4), "gapat: Flush window half-width");
  Check(r.frames == 1 && r.excluded == 3, "gapat: calibration frames and the next two left out");
  Check(r.by.size() == 3, "gapat: three boundaries (nested pass ignored)");
  const gapat::Agg& ab = r.by["A#0 > B#0"];
  Check(ab.n == 1 && Near(ab.gap, 1.0, 1e-4) && Near(ab.ops, 0.3, 1e-4) && Near(ab.starve, 0.3, 1e-4) &&
            Near(ab.rest, 0.4, 1e-4),
        "gapat: A > B gap = ops 0.3 + starve 0.3 + rest 0.4");
  Check(Near(ab.lag[0], 0.4, 1e-4), "gapat: A > B lag 0.4 ms");
  Check(ab.opN[gpt::kDispatch] == 1 && Near(ab.opMs[gpt::kDispatch], 0.3, 1e-4), "gapat: dispatch counted in its boundary");
  Check(ab.nSt == 1 && ab.st.cs == 2000000, "gapat: gap statistics matched to their boundary");
  Check(Near(ab.cpuGap, 11.6, 1e-4), "gapat: CPU time between the two issues");
  const gapat::Agg& bc = r.by["B#0 > C#0"];
  Check(bc.n == 1 && Near(bc.gap, 0.2, 1e-4) && Near(bc.starve, 0.0) && Near(bc.rest, 0.2, 1e-4) &&
            Near(bc.lag[0], 9.2, 1e-4) && bc.nSt == 0,
        "gapat: B > C issued early: no starvation, all rest");
  const gapat::Agg& ca = r.by["C#0 > A#1"];
  Check(ca.n == 1 && Near(ca.gap, 0.5, 1e-4) && Near(ca.starve, 0.5, 1e-4) && Near(ca.rest, 0.0, 1e-4),
        "gapat: C > A#1 issued after the GPU got there: all starved");
  Check(Near(r.fGap[0], 1.7, 1e-4) && Near(r.fOps[0], 0.3, 1e-4) && Near(r.fStarve[0], 0.8, 1e-4) &&
            Near(r.fRest[0], 0.6, 1e-4) && Near(r.fRestLowLag[0], 0.4, 1e-4),
        "gapat: per-frame sums");
}

void Causality() {
  // No calibration: the offset is the smallest GPU - issue difference, so the tightest stamp has lag 0.
  std::vector<gpt::Done> done;
  Builder b(5);
  b.E(gpt::kBegin, kA, 0, 2000.5, 1000.0).E(gpt::kEnd, kA, 0, 2001.0, 1000.5);
  b.E(gpt::kBegin, kB, 0, 2001.4, 1001.3).E(gpt::kEnd, kB, 0, 2002.0, 1001.4);  // diff 1000.1 (min)
  done.push_back(b.d);
  gapat::Result r = gapat::Analyze(done, &NameOf, kQpc);
  Check(!r.cal.fromCal && Near(r.cal.a, 1000.1, 1e-4), "gapat: causality offset = min(GPU - issue)");
  const gapat::Agg& ab = r.by["A#0 > B#0"];
  Check(Near(ab.lag[0], 0.0, 1e-4) && Near(ab.starve, 0.4, 1e-4), "gapat: causality bound marks the tight gap starved");
  // Drift: three samples with offset 1000 + 1e-5 * t.
  std::vector<gpt::Done> cal;
  for (int i = 0; i < 3; ++i) {
    const double c0 = 1000.0 * (i + 1);
    Builder c(static_cast<uint64_t>(10 * i));
    c.E(gpt::kCal, nullptr, 0, c0 + 1000.0 + 1e-5 * (c0 - 1000.0) + 0.01, c0);
    c.d.calAfter = static_cast<int64_t>(std::llround((c0 + 0.02) * kQpc / 1000.0));
    cal.push_back(c.d);
  }
  gapat::Result rc = gapat::Analyze(cal, &NameOf, kQpc);
  Check(rc.cal.n == 3 && Near(rc.cal.b, 1e-5, 1e-8) && rc.cal.residMs < 1e-4, "gapat: drift fitted over 3 samples");
}

void Run() {
  Split();
  Causality();
}

}  // namespace gatest
