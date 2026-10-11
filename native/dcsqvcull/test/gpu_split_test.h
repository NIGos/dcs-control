// Offline test for gpu_split.h's pure parts: the vertex cache model
// (meshoptimizer's analyzeVertexCache), the Forsyth reorder (a permutation of
// the triangles that keeps each triangle's winding) and the vertex merge.
#pragma once

namespace gstest {

using namespace gsplit;

// An n x n quad grid, rows in order (2 triangles per quad).
std::vector<uint32_t> Grid(uint32_t n, uint32_t* nv) {
  std::vector<uint32_t> ix;
  for (uint32_t y = 0; y < n; ++y)
    for (uint32_t x = 0; x < n; ++x) {
      const uint32_t a = y * (n + 1) + x, b = a + 1, c = a + n + 1, d = c + 1;
      ix.insert(ix.end(), {a, b, c, b, d, c});
    }
  *nv = (n + 1) * (n + 1);
  return ix;
}

// Triangles as rotation-normalised tuples (winding kept), sorted.
std::vector<std::tuple<uint32_t, uint32_t, uint32_t>> Tris(const std::vector<uint32_t>& ix) {
  std::vector<std::tuple<uint32_t, uint32_t, uint32_t>> t;
  for (size_t i = 0; i < ix.size(); i += 3) {
    uint32_t a = ix[i], b = ix[i + 1], c = ix[i + 2];
    while (a > b || a > c) {
      const uint32_t s = a;
      a = b, b = c, c = s;
    }
    t.emplace_back(a, b, c);
  }
  std::sort(t.begin(), t.end());
  return t;
}

void Model() {
  std::vector<unsigned> ts;
  // One triangle: 3 vertices; the same triangle twice: still 3 (cached).
  const std::vector<uint32_t> one = {0, 1, 2, 0, 1, 2};
  Check(VertsShaded(one.data(), 3, 3, 32, 32, 32, ts) == 3, "gsplit: one triangle shades 3 vertices");
  Check(VertsShaded(one.data(), 6, 3, 32, 32, 32, ts) == 3, "gsplit: a repeated triangle hits the cache");
  // Primitive group 1: every triangle is its own batch, 3 per triangle.
  Check(VertsShaded(one.data(), 6, 3, 32, 32, 1, ts) == 6, "gsplit: a primitive group of 1 re-shades per triangle");
  // Disjoint triangles: 3 per triangle in any model.
  std::vector<uint32_t> dis;
  for (uint32_t t = 0; t < 100; ++t) dis.insert(dis.end(), {3 * t, 3 * t + 1, 3 * t + 2});
  Check(ShadedNv(dis, 300, ts) == 300 && ShadedFifo16(dis, 300, ts) == 300, "gsplit: disjoint triangles shade 3 each");
  // A warp of 32 holds at most 32 distinct vertices: 11 disjoint triangles = 33 > 32, so 2 batches; count unchanged.
  // Grid: the NV model shades fewer than 3 per triangle and at least every vertex once.
  uint32_t nv = 0;
  const std::vector<uint32_t> g = Grid(64, &nv);
  const uint64_t s = ShadedNv(g, nv, ts);
  Check(s >= nv && s < g.size(), "gsplit: a grid shades between its vertex count and 3 per triangle");
}

void Reorder() {
  uint32_t nv = 0;
  const std::vector<uint32_t> g = Grid(48, &nv);
  // Scramble the triangle order (deterministic).
  const size_t nt = g.size() / 3;
  std::vector<uint32_t> perm(nt);
  for (size_t t = 0; t < nt; ++t) perm[t] = static_cast<uint32_t>(t);
  uint32_t r = 12345;
  for (size_t t = nt - 1; t > 0; --t) {
    r = r * 1103515245u + 12345u;
    std::swap(perm[t], perm[(r >> 8) % (t + 1)]);
  }
  std::vector<uint32_t> sc;
  for (uint32_t t : perm) sc.insert(sc.end(), {g[t * 3], g[t * 3 + 1], g[t * 3 + 2]});
  std::vector<unsigned> ts;
  for (int cs : {16, 32}) {
    const std::vector<uint32_t> o = Forsyth(sc, nv, cs);
    Check(o.size() == sc.size() && Tris(o) == Tris(sc), "gsplit: Forsyth is a permutation of the triangles, winding kept");
    Check(ShadedNv(o, nv, ts) < ShadedNv(sc, nv, ts) * 7 / 10, "gsplit: Forsyth cuts a scrambled grid's shading by > 30%");
  }
  uint64_t best = 0;
  const std::vector<uint32_t> b = BestOrder(sc, nv, ts, &best);
  Check(best <= ShadedNv(sc, nv, ts) && best == ShadedNv(b, nv, ts), "gsplit: BestOrder is never worse and reports its order");
  // Degenerate and tiny inputs.
  Check(Forsyth({}, 0, 32).empty(), "gsplit: Forsyth of nothing is nothing");
  const std::vector<uint32_t> deg = {0, 0, 1, 1, 2, 2};
  Check(Tris(Forsyth(deg, 3, 32)) == Tris(deg), "gsplit: degenerate triangles are kept");
}

void Merge() {
  // 4 vertices, stride 16: v2 equals v0 in all bytes, v3 equals v1 in the first 12 only.
  float vb[4][4] = {{0, 0, 0, 1}, {1, 0, 0, 1}, {0, 0, 0, 1}, {1, 0, 0, 2}};
  const std::vector<uint32_t> ix = {0, 1, 2, 2, 3, 1};
  const std::vector<uint32_t> e = MergeIndices(ix, reinterpret_cast<uint8_t*>(vb), 4, 16, 0);
  Check(e == std::vector<uint32_t>({0, 1, 0, 0, 3, 1}), "gsplit: merge of equal vertices (all bytes)");
  const std::vector<uint32_t> p = MergeIndices(ix, reinterpret_cast<uint8_t*>(vb), 4, 16, 12);
  Check(p == std::vector<uint32_t>({0, 1, 0, 0, 1, 1}), "gsplit: merge by the first 12 bytes");
}

void Analysis() {
  MeshRec m;
  uint32_t nv = 0;
  const std::vector<uint32_t> g = Grid(20, &nv);
  m.key.fmt = DXGI_FORMAT_R16_UINT;
  m.key.count = static_cast<UINT>(g.size());
  m.ibData.resize(g.size() * 2);
  for (size_t i = 0; i < g.size(); ++i) reinterpret_cast<uint16_t*>(m.ibData.data())[i] = static_cast<uint16_t>(g[i]);
  const MeshResult r = Analyse(m);
  Check(r.ok && r.tris == g.size() / 3 && r.uniq == nv && r.nvOpt <= r.nvOrig && !r.merged,
        "gsplit: Analyse reads R16 indices, counts triangles and unique vertices");
  m.ibFail = true;
  Check(!Analyse(m).ok, "gsplit: a failed copy is not analysed");
}

void Run() {
  Model();
  Reorder();
  Merge();
  Analysis();
}

}  // namespace gstest
