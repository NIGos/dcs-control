// Tighter shadow-caster culling for the cascade shadow maps.
//
// DCS fills each cascade with every object inside the cascade's light-space
// box. A caster at point p only darkens receivers p + t*L (t >= 0, L = the
// direction the light travels). For any plane with inward normal n and
// n.L <= 0, moving along L never increases n.x + d, so if every visible
// receiver lies inside that plane, a caster outside it cannot shadow anything
// visible. Adding such planes to the cascade's ClippingVolume removes only
// casters whose shadows fall outside every rendered view: the image does not
// change.
//
// Receivers = every volume culled in the same call except the shadow maps
// themselves (shading 14 cascades, 15 terrain shadow map) and the top-down
// height map (shading 20): the four quad views and all their passes, plus any
// other camera in the frame. Each plane is pushed out until all receiver
// vertices are inside, plus a margin per cascade for shadow filtering and
// normal offset.
//
// Included once from main.cpp inside its anonymous namespace, after Prepare.
#pragma once


struct ShadowStats {
  std::atomic<uint64_t> frames{0};       // calls with cascades handled
  std::atomic<uint64_t> planes{0};       // planes added, summed over cascades
  std::atomic<uint64_t> skipNoLight{0};  // light direction not determined
  std::atomic<uint64_t> skipReceiver{0}; // a receiver volume could not be read
  std::atomic<uint64_t> shadowRend{0};   // cascade renderables, summed (quad frames)
};
ShadowStats g_shadowStats;

std::mutex g_lightMutex;
Vec3 g_lightCached{};
bool g_lightValid = false;
Vec3 g_lightLogged{};

// Intersection point of three planes (n.x + d = 0).
bool Intersect3(const Plane& a, const Plane& b, const Plane& c, Vec3& out) {
  Vec3 bc = Cross(b.n, c.n);
  double det = Dot(a.n, bc);
  if (std::fabs(det) < 1e-9) return false;
  out = (bc * -a.d + Cross(c.n, a.n) * -b.d + Cross(a.n, b.n) * -c.d) * (1.0 / det);
  return std::isfinite(out.x) && std::isfinite(out.y) && std::isfinite(out.z);
}

// Vertices of the convex polytope {x : n.x + d >= 0 for all planes}.
bool PolyVertices(const std::vector<Plane>& pl, std::vector<Vec3>& out) {
  out.clear();
  const size_t n = pl.size();
  for (size_t i = 0; i < n; ++i)
    for (size_t j = i + 1; j < n; ++j)
      for (size_t k = j + 1; k < n; ++k) {
        Vec3 v;
        if (!Intersect3(pl[i], pl[j], pl[k], v)) continue;
        bool inside = true;
        for (const Plane& p : pl)
          if (Dot(p.n, v) + p.d < -0.01) {
            inside = false;
            break;
          }
        if (inside) out.push_back(v);
      }
  return out.size() >= 4;
}

// Unit axes of the opposing plane pairs of a box-like volume.
int BoxAxes(const std::vector<Plane>& pl, Vec3* axes, double* widths) {
  int n = 0;
  for (size_t i = 0; i < pl.size(); ++i)
    for (size_t j = i + 1; j < pl.size(); ++j)
      if (Dot(pl[i].n, pl[j].n) < -0.99999 && n < 3) {
        axes[n] = pl[i].n;
        // Inside both: n.x >= -d_i and -n.x >= -d_j  ->  width = d_i + d_j.
        widths[n] = pl[i].d + pl[j].d;
        ++n;
      }
  return n;
}

bool SameAxis(Vec3 a, Vec3 b) { return std::fabs(Dot(a, b)) > 0.99999; }

// Light direction candidates, each oriented downward (the light travels down).
// The cascade boxes have three axes: light-space X = cross(L, up) is always
// horizontal, Y and L are not. Exactly one of Y and L is the light:
//  - when the terrain shadow map (shading 15) is in the call, L is the axis it
//    shares with every cascade (its other two axes are rotated differently);
//  - afterwards the cascade axis closest to that L is followed as the sun moves;
//  - with no reference at all, both non-horizontal axes are returned and only
//    planes valid for both are used, which is safe whichever one is the light.
bool FindLight(const std::vector<std::vector<Plane>>& cascades, const std::vector<Plane>* terrain,
               std::vector<Vec3>& lights) {
  lights.clear();
  Vec3 axes[3];
  double w[3];
  if (cascades.empty() || BoxAxes(cascades[0], axes, w) != 3) return false;
  for (size_t c = 1; c < cascades.size(); ++c) {
    Vec3 ax[3];
    double ww[3];
    if (BoxAxes(cascades[c], ax, ww) != 3) return false;
    for (int a = 0; a < 3; ++a)
      if (!(SameAxis(axes[a], ax[0]) || SameAxis(axes[a], ax[1]) || SameAxis(axes[a], ax[2]))) return false;
  }
  auto down = [](Vec3 v) { return v.y > 0 ? v * -1.0 : v; };
  std::lock_guard<std::mutex> lock(g_lightMutex);
  if (terrain) {
    Vec3 tx[3];
    double ww[3];
    if (BoxAxes(*terrain, tx, ww) == 3) {
      int found = -1, matches = 0;
      for (int a = 0; a < 3; ++a)
        if (SameAxis(axes[a], tx[0]) || SameAxis(axes[a], tx[1]) || SameAxis(axes[a], tx[2])) {
          found = a;
          ++matches;
        }
      if (matches == 1) {
        g_lightCached = down(axes[found]);
        g_lightValid = true;
      }
    }
  }
  if (g_lightValid) {
    // Follow the sun: the cascade axis within 2.5 degrees of the last light.
    for (int a = 0; a < 3; ++a)
      if (std::fabs(Dot(axes[a], g_lightCached)) > 0.999) {
        g_lightCached = down(axes[a]);
        if (g_lightCached.y >= -0.05) return false;  // sun or moon near the horizon
        lights.push_back(g_lightCached);
        return true;
      }
    g_lightValid = false;  // lost track (big jump in time of day): fall back
  }
  int horizontal = -1, nh = 0;
  for (int a = 0; a < 3; ++a)
    if (std::fabs(axes[a].y) < 0.02) {
      horizontal = a;
      ++nh;
    }
  if (nh != 1) return false;  // not the expected basis, or the light is near the horizon
  for (int a = 0; a < 3; ++a) {
    if (a == horizontal) continue;
    Vec3 l = down(axes[a]);
    if (l.y >= -0.05) return false;
    lights.push_back(l);
  }
  return true;
}

// Copies a ClippingVolume (with its inline exclusion list) to dst.
bool CopyVolume(uint8_t* dst, const uint8_t* src) {
  uint64_t size = *reinterpret_cast<const uint64_t*>(src + kVolExclSize);
  const uint8_t* data = *reinterpret_cast<uint8_t* const*>(src + kVolExclPtr);
  if (size > static_cast<uint64_t>(kVolExclInlineCap) || (size > 0 && !data)) return false;
  memcpy(dst, src, kVolExclPtr);
  uint8_t* inl = dst + kVolExclInline;
  for (uint64_t i = 0; i < size; ++i) memcpy(inl + i * kPlaneBlockSize, data + i * kPlaneBlockSize, kPlaneBlockSize);
  *reinterpret_cast<uint8_t**>(dst + kVolExclPtr) = inl;
  *reinterpret_cast<uint64_t*>(dst + kVolExclCap) = kVolExclInlineCap;
  *reinterpret_cast<uint64_t*>(dst + kVolExclSize) = size;
  return true;
}

void PrepareShadows(uint32_t count, uint8_t* infos, Patch* patches, size_t maxPatches, int* patchedOut) {
  // Gather volumes.
  std::vector<const uint8_t*> cascadeVols, receiverVols;
  const uint8_t* terrainVol = nullptr;
  for (uint32_t i = 0; i < count; ++i) {
    const uint8_t* ci = infos + i * kCollectionInfoStride;
    uint16_t sm = *reinterpret_cast<const uint16_t*>(ci + kCiShadingModel);
    const uint8_t* vol = *reinterpret_cast<uint8_t* const*>(ci + kCiClipVolume);
    if (!vol) continue;
    auto addUnique = [](std::vector<const uint8_t*>& v, const uint8_t* p) {
      if (std::find(v.begin(), v.end(), p) == v.end()) v.push_back(p);
    };
    if (sm == 14) addUnique(cascadeVols, vol);
    else if (sm == 15) terrainVol = vol;
    else if (sm != 20) addUnique(receiverVols, vol);
  }
  if (cascadeVols.empty() || receiverVols.empty()) return;
  // A cascade volume shared with anything other than cascades is left alone.
  for (uint32_t i = 0; i < count; ++i) {
    const uint8_t* ci = infos + i * kCollectionInfoStride;
    if (*reinterpret_cast<const uint16_t*>(ci + kCiShadingModel) == 14) continue;
    const uint8_t* vol = *reinterpret_cast<uint8_t* const*>(ci + kCiClipVolume);
    cascadeVols.erase(std::remove(cascadeVols.begin(), cascadeVols.end(), vol), cascadeVols.end());
  }
  if (cascadeVols.empty()) return;

  // Receiver volumes as plane lists; candidate planes are their planes.
  std::vector<std::vector<Plane>> receivers;
  std::vector<Plane> candidates, pl;
  for (const uint8_t* vol : receiverVols) {
    if (*reinterpret_cast<const int32_t*>(vol + kPlaneExtraCount) != 0 || !ReadPlanes(vol, pl)) {
      g_shadowStats.skipReceiver++;
      return;  // unknown receiver shape: keep DCS's culling for this call
    }
    receivers.push_back(pl);
    for (const Plane& p : pl) {
      bool dup = false;
      for (const Plane& c : candidates)
        if (Dot(c.n, p.n) > 0.999999 && std::fabs(c.d - p.d) < 0.01) dup = true;
      if (!dup) candidates.push_back(p);
    }
  }

  std::vector<std::vector<Plane>> cascades;
  for (const uint8_t* vol : cascadeVols) {
    if (*reinterpret_cast<const int32_t*>(vol + kPlaneExtraCount) != 0 || !ReadPlanes(vol, pl) || pl.size() != 6)
      return;
    cascades.push_back(pl);
  }
  std::vector<Plane> terrain;
  bool haveTerrain = terrainVol && ReadPlanes(terrainVol, terrain) && terrain.size() == 6;
  std::vector<Vec3> lights;
  if (!FindLight(cascades, haveTerrain ? &terrain : nullptr, lights)) {
    g_shadowStats.skipNoLight++;
    return;
  }
  std::vector<const Plane*> away;  // candidates facing away from every possible light
  for (const Plane& c : candidates) {
    bool ok = true;
    for (const Vec3& l : lights)
      if (Dot(c.n, l) > 0) ok = false;
    if (ok) away.push_back(&c);
  }
  if (away.empty()) return;

  const bool invert = g_shadowDebugInvert.load();
  int& n = *patchedOut;
  uint64_t planesAdded = 0;
  std::vector<Vec3> verts, vtmp, boxVerts;
  for (size_t ci = 0; ci < cascadeVols.size(); ++ci) {
    const std::vector<Plane>& box = cascades[ci];
    Vec3 ax[3];
    double w[3];
    BoxAxes(box, ax, w);
    double extent = std::max(w[0], std::max(w[1], w[2]));
    double margin = std::max(g_cfg.shadowMarginMin, g_cfg.shadowMarginFrac * extent);
    if (!PolyVertices(box, boxVerts)) continue;

    // A receiver can only sample this cascade where it lies inside the shadow
    // map's footprint: the two box slabs across the light, widened by the
    // margin. With two light candidates each hypothesis has its own footprint;
    // the receiver set is the union, so a plane is kept only if it holds for
    // both.
    verts.clear();
    std::vector<int> vertOwner;
    size_t slabPlanes = 0;
    for (const Vec3& l : lights) {
      std::vector<Plane> slab;
      for (const Plane& p : box)
        if (std::fabs(Dot(p.n, l)) < 0.999) slab.push_back({p.n, p.d + margin});
      slabPlanes = slab.size();
      for (size_t ri = 0; ri < receivers.size(); ++ri) {
        std::vector<Plane> clipped = receivers[ri];
        clipped.insert(clipped.end(), slab.begin(), slab.end());
        // Nothing visible lies below the lowest ground on any DCS map (the Dead
        // Sea shore is at -430 m; world y is height above sea level): without
        // this floor the footprint prisms run hundreds of km underground.
        clipped.push_back({Vec3{0, 1, 0}, -g_cfg.shadowFloorY});
        if (PolyVertices(clipped, vtmp)) {
          verts.insert(verts.end(), vtmp.begin(), vtmp.end());
          vertOwner.insert(vertOwner.end(), vtmp.size(), static_cast<int>(ri));
        }
      }
    }
    // No visible receiver inside this cascade's footprint: leave it to DCS
    // rather than remove everything on the strength of the model alone.
    if (verts.empty()) continue;

    // Each candidate is pushed out to contain every receiver vertex plus the
    // margin; keep those that cut deepest into the cascade box.
    std::vector<std::pair<double, Plane>> ranked;
    for (const Plane* c : away) {
      double worst = -1e300;
      for (const Vec3& v : verts) worst = std::max(worst, -(Dot(c->n, v) + c->d));
      Plane p{c->n, c->d + worst + margin};
      double cut = 0;
      for (const Vec3& v : boxVerts) cut = std::max(cut, -(Dot(p.n, v) + p.d));
      if (cut > margin) ranked.push_back({cut, p});
    }
    std::sort(ranked.begin(), ranked.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    {
      static std::atomic<int> diag{0};
      if (diag.fetch_add(1) < 8) {
        double bestCut = -1e300;
        const Plane* best = nullptr;
        for (const Plane* c : away) {
          double worst = -1e300;
          for (const Vec3& v : verts) worst = std::max(worst, -(Dot(c->n, v) + c->d));
          double cut = -1e300;
          for (const Vec3& v : boxVerts) cut = std::max(cut, -(Dot(c->n, v) + c->d + worst + margin));
          if (cut > bestCut) {
            bestCut = cut;
            best = c;
          }
        }
        if (best && ci == 3) {
          // Which receivers push the best candidate out the most.
          std::vector<std::pair<double, int>> worstBy(receivers.size(), {-1e300, 0});
          for (size_t k = 0; k < verts.size(); ++k) {
            double o = -(Dot(best->n, verts[k]) + best->d);
            auto& wb = worstBy[vertOwner[k]];
            if (o > wb.first) wb = {o, static_cast<int>(k)};
          }
          for (size_t r = 0; r < receivers.size(); ++r) {
            const Vec3& v = verts[worstBy[r].second];
            Log("shadow tightening diag:   receiver %zu (vol %p) pushes %.0f m at (%.0f %.0f %.0f), plane0 n=(%+.3f %+.3f %+.3f)",
                r, static_cast<const void*>(receiverVols[r]), worstBy[r].first, v.x, v.y, v.z, receivers[r][0].n.x,
                receivers[r][0].n.y, receivers[r][0].n.z);
          }
        }
        Log("shadow tightening diag: cascade %zu extent %.0f m, slab planes %zu, receiver vertices %zu, best plane "
            "n=(%+.3f %+.3f %+.3f) cuts %.1f m (margin %.1f), planes chosen %zu",
            ci, extent, slabPlanes, verts.size(), best ? best->n.x : 0, best ? best->n.y : 0, best ? best->n.z : 0,
            bestCut, margin, ranked.size());
      }
    }
    std::vector<Plane> chosen;
    const int room = kMaxPlanes - static_cast<int>(box.size());
    for (const auto& r : ranked) {
      if (static_cast<int>(chosen.size()) >= room) break;
      bool dup = false;
      for (const Plane& c : chosen)
        if (Dot(c.n, r.second.n) > 0.999) dup = true;
      if (!dup) chosen.push_back(r.second);
    }
    if (chosen.empty()) continue;

    const uint8_t* src = cascadeVols[ci];
    uint64_t key = 0x5300000000ull | (static_cast<uint64_t>(ci) << 8);
    uint8_t* dst = AcquireVolume(key);
    if (!dst) continue;  // allocation failed (AcquireVolume)
    if (!invert) {
      if (!CopyVolume(dst, src)) continue;
      int idx = *reinterpret_cast<int32_t*>(dst + kPlaneCount);
      for (const Plane& p : chosen) WritePlane(dst, idx++, p);
      *reinterpret_cast<int32_t*>(dst + kPlaneCount) = idx;
    } else {
      // Debug: exclude everything fully inside box + planes, so only the
      // casters that would be removed (and those straddling a plane) remain.
      uint8_t block[kPlaneBlockSize];
      memset(block, 0, sizeof(block));
      int idx = 0;
      for (const Plane& p : box) WritePlane(block, idx++, p);
      for (const Plane& p : chosen) WritePlane(block, idx++, p);
      *reinterpret_cast<int32_t*>(block + kPlaneCount) = idx;
      if (!MakeVolumeWithExclusion(dst, src, block)) continue;
    }
    planesAdded += chosen.size();
    for (uint32_t i = 0; i < count && static_cast<size_t>(n) < maxPatches; ++i) {
      uint8_t* other = infos + i * kCollectionInfoStride;
      if (*reinterpret_cast<uint8_t**>(other + kCiClipVolume) != src) continue;
      patches[n].slot = other + kCiClipVolume;
      patches[n].original = const_cast<uint8_t*>(src);
      *reinterpret_cast<uint8_t**>(other + kCiClipVolume) = dst;
      ++n;
    }
  }
  g_shadowStats.frames++;
  g_shadowStats.planes += planesAdded;
  std::lock_guard<std::mutex> lock(g_lightMutex);
  static size_t loggedCount = 0;
  if (lights.size() != loggedCount || !SameAxis(g_lightLogged, lights[0])) {
    loggedCount = lights.size();
    g_lightLogged = lights[0];
    if (lights.size() == 1)
      Log("shadow tightening: light (%+.3f %+.3f %+.3f), %zu receiver volumes, %zu planes facing away", lights[0].x,
          lights[0].y, lights[0].z, receiverVols.size(), away.size());
    else
      Log("shadow tightening: light is one of (%+.3f %+.3f %+.3f) / (%+.3f %+.3f %+.3f), planes valid for both; "
          "%zu receiver volumes, %zu planes facing away",
          lights[0].x, lights[0].y, lights[0].z, lights[1].x, lights[1].y, lights[1].z, receiverVols.size(),
          away.size());
  }
}
