// Audit A2/B4 census (chain_derive.h): the pure compare logic on synthetic views. A periphery list, the focus list
// built as that list filtered by narrower planes, then one injected defect per case (each kind must be counted
// exactly and nothing else), the plane sign convention flipped, duplicates, order, camera checks, the log line.
#pragma once

namespace cdtest {
using namespace chainderive;

struct Rng32 {
  uint32_t s;
  uint32_t Next() {
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    return s;
  }
  float F(float a, float b) { return a + (b - a) * (Next() & 0xffffff) / 16777216.0f; }
};

void SetRow(ViewData& v, int r, float a, float b, float c, float d) {
  float* p = v.cb.data() + 4 * r;
  p[0] = a, p[1] = b, p[2] = c, p[3] = d;
}

// Planes x + t z >= 0 style around +z (inside sign s), near 0.1, far 1000.
void SetCamera(ViewData& v, float tanHalf, float pScale, float s) {
  v.cb.assign(4 * 48, 0.0f);
  SetRow(v, kRowP + 0, pScale, 0, 0.1f, 0);
  SetRow(v, kRowP + 1, 0, pScale, 0.001f, 0);
  SetRow(v, kRowP + 2, 0, 0, 1.0001f, -0.1f);
  SetRow(v, kRowP + 3, 0, 0, 1, 0);
  SetRow(v, kRowPlanes + 0, s * 1, 0, s * tanHalf, 0);
  SetRow(v, kRowPlanes + 1, s * -1, 0, s * tanHalf, 0);
  SetRow(v, kRowPlanes + 2, 0, s * 1, s * tanHalf, 0);
  SetRow(v, kRowPlanes + 3, 0, s * -1, s * tanHalf, 0);
  SetRow(v, kRowPlanes + 4, 0, 0, s * 1, s * -0.1f);
  SetRow(v, kRowPlanes + 5, 0, 0, s * -1, s * 1000);
  SetRow(v, kRowCam, -6.26f, -8.61f, -13.58f, 0);
}

void Push(ViewData& v, const uint32_t* r) {
  v.rec.insert(v.rec.end(), r, r + 16);
  ++v.n;
}

void Recount(ViewData& v, size_t buckets) {
  v.counts.assign(buckets, 0);
  for (uint32_t i = 0; i < v.n; ++i) ++v.counts[v.rec[16ull * i] & 0xffffffu];
  v.sums0 = v.n;
}

void MakeRec(Rng32& g, uint32_t r[16], float x, float y, float z) {
  memset(r, 0, 64);
  r[0] = (g.Next() % 50) | 0x01000000u;  // bucket, a flag byte
  r[1] = g.Next() % 4;                    // LOD
  memcpy(&r[2], &x, 4);
  memcpy(&r[3], &y, 4);
  memcpy(&r[4], &z, 4);
  for (int k = 5; k <= 10; ++k) {
    const float f = g.F(-1, 1);
    memcpy(&r[k], &f, 4);
  }
  const float blend = 1.0f;
  memcpy(&r[12], &blend, 4);
  r[13] = g.Next() & 0xffff;
}

// Periphery (tan 1) and focus (tan 0.3) of one eye; focus = periphery filtered by the focus planes (pivot test).
void Build(ViewData& P, ViewData& F, float sign, uint32_t seed = 7) {
  P = ViewData{};
  F = ViewData{};
  SetCamera(P, 1.0f, 0.78f, sign);
  SetCamera(F, 0.3f, 2.29f, sign);
  Rng32 g{seed};
  for (int i = 0; i < 4000; ++i) {
    uint32_t r[16];
    const float z = g.F(1, 300);
    MakeRec(g, r, g.F(-1.2f, 1.2f) * z, g.F(-1.2f, 1.2f) * z, z);
    float p[3];
    Pivot(r, p);
    if (MinDist(P, static_cast<int>(sign), p) < 0) continue;
    Push(P, r);
    if (MinDist(F, static_cast<int>(sign), p) >= 0) Push(F, r);
  }
  Recount(P, 64);
  Recount(F, 64);
}

void Run() {
  printf("chain derive census (audit A2/B4)\n");
  ViewData P, F;
  Build(P, F, 1);
  const uint32_t nP = P.n, nF = F.n;
  CensusStats s = ComparePair(P, F);
  Check(nF > 100 && nP > nF + 100 && s.pairs == 1 && !s.camBad && !s.trunc && !s.signBad && s.matched == nF &&
            s.droppedOut == nP - nF && !s.droppedIn && !s.extraIn && !s.extraOut && !s.lod && !s.payload &&
            !s.countsBad && !s.invalid && !s.orderDiff && s.orderSame > 0 && s.planesEqAnd == 0x30 && s.Exact(),
        "A2 census: focus = filtered periphery is EXACT (all matched, dropped only outside the focus planes, "
        "near/far planes equal)");

  Build(P, F, -1);
  s = ComparePair(P, F);
  Check(s.matched == F.n && s.droppedOut == P.n - F.n && !s.droppedIn && !s.signBad && s.Exact(),
        "A2 census: the plane sign convention is taken from the data (negated planes, same result)");

  // Each defect alone.
  Build(P, F, 1);
  {
    Rng32 g{99};
    uint32_t r[16];
    MakeRec(g, r, 0.5f, 0.5f, 50.f);  // inside every plane, not in the periphery list
    Push(F, r);
    Recount(F, 64);
    s = ComparePair(P, F);
    Check(s.extraIn == 1 && !s.extraOut && s.matched == F.n - 1 && !s.Exact(), "A2 census: extra_in counted");
  }
  Build(P, F, 1);
  {
    Rng32 g{98};
    uint32_t r[16];
    MakeRec(g, r, -4.f, 0.f, -2.f);  // beside / behind the eye: outside a periphery plane
    Push(F, r);
    Recount(F, 64);
    s = ComparePair(P, F);
    Check(s.extraOut == 1 && !s.extraIn && !s.Exact(), "A2 census: extra_out counted (outside a periphery plane)");
  }
  Build(P, F, 1);
  F.rec[16 * 3 + 1] ^= 1;  // LOD of one focus record
  s = ComparePair(P, F);
  Check(s.lod == 1 && !s.payload && s.matched == F.n && !s.Exact(), "A2 census: a LOD difference counted as lod");
  Build(P, F, 1);
  F.rec[16 * 5 + 13] ^= 0x10000;  // a payload word
  F.rec[16 * 5 + 0] ^= 0x02000000u;  // the flag byte of word 0 (bucket unchanged)
  s = ComparePair(P, F);
  Check(s.payload == 1 && !s.lod && s.wordDiff[13] == 1 && s.wordDiff[0] == 1 && s.WordMask() == 0x2001 && !s.Exact(),
        "A2 census: a payload difference counted with its words");

  // Duplicates: two identical periphery records, one in the focus list.
  Build(P, F, 1);
  {
    const uint32_t* r = F.rec.data();  // a record that is in both lists
    std::vector<uint32_t> copy(r, r + 16);
    Push(P, copy.data());
    Recount(P, 64);
    s = ComparePair(P, F);
    Check(s.matched == F.n && s.droppedIn == 1 && !s.extraIn && !s.extraOut && s.Exact(),
          "A2 census: duplicate records matched one to one (the spare one is dropped_in)");
  }

  // Order: two focus records of one bucket swapped.
  Build(P, F, 1);
  {
    int a = -1, b = -1;
    for (uint32_t i = 0; i < F.n && b < 0; ++i)
      for (uint32_t j = i + 1; j < F.n; ++j)
        if ((F.rec[16ull * i] & 0xffffff) == (F.rec[16ull * j] & 0xffffff)) {
          a = static_cast<int>(i), b = static_cast<int>(j);
          break;
        }
    for (int k = 0; k < 16; ++k) std::swap(F.rec[16 * a + k], F.rec[16 * b + k]);
    s = ComparePair(P, F);
    Check(a >= 0 && s.orderDiff == 1 && s.Exact(), "A2 census: order inside a bucket is reported, not a mismatch");
  }

  // Camera, counts, truncation.
  Build(P, F, 1);
  SetRow(F, kRowP + 3, 0, 0, 1, 0.001f);
  s = ComparePair(P, F);
  Check(s.camBad == 1 && !s.matched && !s.Exact(), "A2 census: a focus view of another camera is not compared");
  Build(P, F, 1);
  SetRow(F, kRowP, 0.5f, 0, 0.1f, 0);  // focus wider than periphery: not a focus view
  s = ComparePair(P, F);
  Check(s.camBad == 1, "A2 census: pair order checked (periphery |P00| < focus |P00|)");
  Build(P, F, 1);
  ++P.counts[F.rec[0] & 0xffffff];
  s = ComparePair(P, F);
  Check(s.countsBad == 1 && s.Exact(), "A2 census: counts[] against the records (identification sanity)");
  Build(P, F, 1);
  P.trunc = true;
  s = ComparePair(P, F);
  Check(s.trunc == 1 && !s.matched && !s.Exact(), "A2 census: a truncated capture is not compared");

  // Accumulation and the log line.
  Build(P, F, 1);
  CensusStats tot;
  tot.Add(ComparePair(P, F));
  tot.Add(ComparePair(P, F));
  GpuView gv[4];
  gv[2].us = 2 * 170.0, gv[2].n = 2;
  gv[2].nest.push_back({"TerrainModule", {300.0, 2}});
  const std::string line = FormatLine(30, 2, 0, tot, gv, 0, "buffers identified");
  const std::string gl = FormatGpuLine(gv);
  Check(tot.pairs == 2 && tot.matched == 2ull * F.n && tot.Exact() && line.rfind("chain derive census: 30 s", 0) == 0 &&
            line.find("EXACT (focus = filtered periphery)") != std::string::npos &&
            line.find("focus L 0.170") != std::string::npos && line.find("NOT EXACT") == std::string::npos &&
            gl.find("focus L TerrainModule 0.150") != std::string::npos,
        "A2 census: totals add up; the log line carries the verdict and the focus GPU ms");
  CensusStats none;
  Check(FormatLine(30, 0, 1, none, gv, 0, "x").find("NO DATA") != std::string::npos,
        "A2 census: no capture gives NO DATA, never EXACT");
}

}  // namespace cdtest
