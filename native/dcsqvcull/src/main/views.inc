#include "gpu.h"
#include "scene_cut.h"  // [Perception] SceneCutReset: Quad Views history reset on camera cuts
#include "bench.h"
#include "profiler.h"
#include "timer_cache.h"
#include "d3dstate.h"
#include "collect_timing.h"
#include "task_timing.h"
#include "model_timing.h"
#include "shadow_pass.h"
// Quad-view frames (UpdateGazeAndCheckHold below), declared ahead of pass_timing.h: its top-level frame id.
std::atomic<uint64_t> g_quadFrame{0};
#include "pass_timing.h"
#include "thread_tuning.h"
#include <condition_variable>
#include <functional>
#include <thread>
#include <d3dcompiler.h>
#include <d3d11shader.h>
#include <dxgi1_4.h>
#include <d3d11_4.h>  // R65: ID3D11Device5 / ID3D11Fence (cockpit mask: shared fence)
#include "deferred_rec.h"  // R15 A1: deferred-context worker pool of the shadow and G-buffer recorders
namespace {

// 0.7 three views ([Views] ThreeViews, three_views.h): the state the collect hook (Prepare, below), fixation depth's
// render-thread capture of the periphery depth (fixation_depth.h) and the worker share. The worker decides; the
// collect hook and the render thread only read, except g_layerMask (render thread, once per frame).
namespace tvc {
std::atomic<bool> g_cfgOn{false};      // [Views] ThreeViews: the module runs the capture and asks Quad Views each frame
std::atomic<bool> g_engaged{false};    // the right periphery is rendered as strip + cockpit (collect and terrain chain)
std::atomic<uint32_t> g_layerMask{0};  // Quad Views' DcsVr_QuadViewsDepthWanted() of the last frame (render thread)
std::atomic<double> g_stripNdc{0.55};  // [Views] ThreeViewsStripNdc: the right periphery's world is drawn for NDC x >= it
std::atomic<uint64_t> g_patched{0}, g_refused{0}, g_ckKept{0}, g_notRight{0};
std::atomic<int> g_logLeft{0};         // collect log lines left (set on each engage)
// [Views] ThreeViewsIsolate (dev, default 0, hot): parts of the mode left out to isolate a fault. 1 no post-transparent
// depth copy / push, 2 no fill inside the engine (Quad Views' fill only), 4 no terrain chain narrowing, 8 no motion
// target recorded for the engine fill, 16 the narrowed terrain planes in a constant buffer of ours bound in place of
// DCS's (three_views.h vstrip::OwnMode) instead of written into DCS's buffer.
std::atomic<uint32_t> g_isolate{0};
// Fixation depth's pass boundary (render thread): begin / end of a top-level pass, inRight = the pass belongs to the
// right periphery (HMD view 1 by the CopyToHMDPass ordinal, four HMD views in the last frame). three_views.h.
std::atomic<void (*)(bool begin, bool inRight)> g_onPass{nullptr};
}  // namespace tvc

void ApplyTimerConfig() {
  if (!timercache::g_orig) return;
  timercache::g_periodUs = g_cfg.shaderTimeCacheUs;
  timercache::g_on = g_cfg.shaderTimeCache && g_cfg.shaderTimeCacheUs > 0 && !g_engineOff.load();
}

// ---------------------------------------------------------------------------
// Hooks
// ---------------------------------------------------------------------------

using CollectFn = void(__fastcall*)(void* self, uint32_t count, uint8_t* infos, uint8_t* outs,
                                    void* taskQueue, uint32_t flags);
CollectFn g_origCollect = nullptr;
std::atomic<void*> g_scene{nullptr};
std::atomic<uint32_t> g_lastCollectFlags{0};  // last argument of collectSceneObjectsRenderables
// Finer culling partition: Scene.dll splits object parsing into
// min((min(threadsMax, arg) + 1) * 2, 16) contiguous chunks weighted by object
// count. Raising threadsMax to 8 and the argument to 7 yields the maximum of
// 16 chunks, so one chunk of heavy models delays the render thread less.
std::atomic<uint32_t> g_defaultThreadsMax{0};
// R63 U2: 1 = the running Scene.dll's collect function reads scene+0x40 and
// takes min(it, its last argument) (scenecheck::FindThreadsMaxUse, checked by
// Install); -1 = it does not: nothing writes +0x40 or raises the argument.
std::atomic<int> g_sceneLayout{0};
std::atomic<bool> g_threadsApplied{false};

struct ViewInfo {
  int index;
  Frustum fr;
  int role;     // 0 = other, 1 = peripheral, 2 = focus
  int partner;  // index of the paired focus view (peripheral only)
};

struct Patch {
  uint8_t* slot;    // address of CollectionInfo::clipVolume
  uint8_t* original;
};

size_t VectorCount(const uint8_t* vec) {
  auto b = *reinterpret_cast<uint8_t* const*>(vec);
  auto e = *reinterpret_cast<uint8_t* const*>(vec + 8);
  return (b && e > b) ? static_cast<size_t>(e - b) / sizeof(void*) : 0;
}

void DumpViews(uint32_t count, const uint8_t* infos, const std::vector<ViewInfo>& views) {
  Log("---- view dump: %u infos ----", count);
  for (uint32_t i = 0; i < count; ++i) {
    const uint8_t* ci = infos + i * kCollectionInfoStride;
    uint16_t shading = *reinterpret_cast<const uint16_t*>(ci + kCiShadingModel);
    uint32_t tag = *reinterpret_cast<const uint32_t*>(ci + kCiViewportTag);
    const void* aux = *reinterpret_cast<void* const*>(ci + kCiAuxCallback);
    const uint8_t* vol = *reinterpret_cast<uint8_t* const*>(ci + kCiClipVolume);
    Log("[%u] shading=%u tag=%u aux=%p vol=%p", i, shading, tag, aux, vol);
    if (!vol) continue;
    int np = *reinterpret_cast<const int32_t*>(vol + kPlaneCount);
    Log("     planes=%d extra=%d distMax=%.1f excl=%llu", np,
        *reinterpret_cast<const int32_t*>(vol + kPlaneExtraCount),
        *reinterpret_cast<const double*>(vol + kVolDistanceMax),
        static_cast<unsigned long long>(*reinterpret_cast<const uint64_t*>(vol + kVolExclSize)));
    for (int p = 0; p < np && p < kMaxPlanes; ++p) {
      const double* d = reinterpret_cast<const double*>(vol + p * kPlaneStride);
      Log("       n=(%+.4f %+.4f %+.4f) d=%+.2f", d[0], d[1], d[2], d[3]);
    }
    const float* m = reinterpret_cast<const float*>(ci + kCiViewProj);
    Log("     vp row0 %+.3f %+.3f %+.3f %+.3f | row3 %+.3f %+.3f %+.3f %+.3f", m[0], m[1], m[2],
        m[3], m[12], m[13], m[14], m[15]);
    for (const ViewInfo& v : views) {
      if (v.index != static_cast<int>(i)) continue;
      if (!v.fr.valid) {
        Log("     frustum: not decoded");
        break;
      }
      const double r2d = 57.29577951308232;
      Log("     apex=(%.3f %.3f %.3f) fwd=(%+.3f %+.3f %+.3f) half=[%.1f %.1f %.1f %.1f] role=%d "
          "partner=%d",
          v.fr.apex.x, v.fr.apex.y, v.fr.apex.z, v.fr.forward.x, v.fr.forward.y, v.fr.forward.z,
          v.fr.halfAngle[0] * r2d, v.fr.halfAngle[1] * r2d, v.fr.halfAngle[2] * r2d,
          v.fr.halfAngle[3] * r2d, v.role, v.partner);
    }
  }
}

// Collections per collect call we classify and patch. It was 64: an F-4 cockpit
// scene has 68 (R24 12), so no call was classified, no quad frame counted, and
// every feature keyed on quad frames stood still. This is only a sanity bound.
constexpr uint32_t kMaxCollectInfos = 4096;

// Classifies the views of one call into peripheral/focus pairs.
int Classify(uint32_t count, const uint8_t* infos, std::vector<ViewInfo>& views) {
  views.clear();
  for (uint32_t i = 0; i < count; ++i) {
    const uint8_t* ci = infos + i * kCollectionInfoStride;
    if (*reinterpret_cast<const uint16_t*>(ci + kCiShadingModel) != 0) continue;
    if (*reinterpret_cast<void* const*>(ci + kCiAuxCallback)) continue;
    const uint8_t* vol = *reinterpret_cast<uint8_t* const*>(ci + kCiClipVolume);
    if (!vol) continue;
    ViewInfo v{static_cast<int>(i), DecodeFrustum(vol), 0, -1};
    views.push_back(v);
  }
  if (views.size() < 4) return 0;

  double widest = 0;
  for (auto& v : views)
    if (v.fr.valid && v.fr.meanHalfAngle > widest) widest = v.fr.meanHalfAngle;
  for (auto& v : views)
    if (v.fr.valid) v.role = (v.fr.meanHalfAngle < widest * g_cfg.focusRatio) ? 2 : 1;

  int pairs = 0;
  for (auto& p : views) {
    if (p.role != 1) continue;
    double bestDist = g_cfg.apexTolerance;
    int best = -1;
    for (size_t k = 0; k < views.size(); ++k) {
      const ViewInfo& f = views[k];
      if (f.role != 2) continue;
      double dist = Len(f.fr.apex - p.fr.apex);
      // The focus view must look somewhere inside the peripheral view.
      if (Dot(f.fr.centerDir, p.fr.centerDir) < std::cos(p.fr.meanHalfAngle)) continue;
      if (dist <= bestDist) {
        bestDist = dist;
        best = static_cast<int>(k);
      }
    }
    if (best >= 0) {
      p.partner = best;
      ++pairs;
    }
  }
  return pairs;
}

// Quad-Views-Foveated blends the focus layer over the periphery with
// alpha = max(0.5, s.x * s.y), s = smoothstep ramps of width `smoothing` in
// focus texture coordinates. The focus is fully opaque only where both
// texcoords are in [smoothing, 1 - smoothing], i.e. |ndc| < 1 - 2 * smoothing.
std::atomic<double> g_qvSmoothing{-1.0};  // <0 = unknown
std::atomic<bool> g_qvfrLoaded{false};       // Quad-Views-Foveated layer present in DCS
std::atomic<double> g_keepOverride{-1.0};    // set live with Ctrl+Alt+PgUp/PgDn

double ReadQvfrSmoothing() {
  wchar_t base[MAX_PATH];
  if (!GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH)) return -1;
  std::wstring path = std::wstring(base) + L"\\Quad-Views-Foveated\\Quad-Views-Foveated.log";
  // QVFR keeps its log open for writing: _wfopen_s would demand exclusive
  // read access and fail with a sharing violation.
  FILE* f = _wfsopen(path.c_str(), L"rb", _SH_DENYNO);
  if (!f) return -1;
  std::string text;
  char buf[65536];
  size_t r;
  while ((r = fread(buf, 1, sizeof(buf), f)) > 0) text.append(buf, r);
  fclose(f);
  size_t session = text.rfind("Session is using quad views");
  if (session == std::string::npos) return -1;
  size_t pos = text.find("Edge smoothing: ", session);
  size_t next = text.find("Session is using quad views", session + 1);
  if (pos == std::string::npos || (next != std::string::npos && pos > next)) return 0.0;  // not logged = off
  return atof(text.c_str() + pos + 16);
}

double EffectiveKeep() {
  if (g_keepOverride.load() >= 0) return Clamp(g_keepOverride.load(), 0.0, 1.0);
  if (g_cfg.keepFraction > 0) return Clamp(g_cfg.keepFraction, 0.0, 1.0);
  // Without QVFR the runtime composites the quad views itself (Pimax native)
  // and its edge blend is unknown: use the configured conservative value.
  if (!g_qvfrLoaded.load()) return Clamp(g_cfg.nativeKeepFraction, 0.0, 1.0);
  double sm = g_qvSmoothing.load();
  if (sm < 0) sm = 0.30;  // conservative until the log is read
  return Clamp(1.0 - 2.0 * sm - g_cfg.safetyNdc, 0.0, 1.0);
}

// Saccade detection. Gaze is measured relative to the head (dot products of
// the focus centre with the peripheral side planes), so head rotation alone
// does not count as a saccade.
std::mutex g_gazeMutex;
double g_prevGaze[2][4] = {};
bool g_havePrevGaze[2] = {};
std::atomic<uint64_t> g_holdUntil{0};
std::atomic<uint64_t> g_saccades{0};

bool UpdateGazeAndCheckHold(const std::vector<ViewInfo>& views) {
  uint64_t frame = ++g_quadFrame;
  double threshold = std::sin(g_cfg.saccadeDeg * 0.017453292519943295);
  bool saccade = false;
  {
    std::lock_guard<std::mutex> lock(g_gazeMutex);
    int eye = 0;
    for (const ViewInfo& v : views) {
      if (v.role != 1 || v.partner < 0 || eye >= 2) continue;
      const Frustum& fo = views[v.partner].fr;
      double g[4];
      for (int i = 0; i < 4; ++i) g[i] = Dot(fo.centerDir, v.fr.sides[i].n);
      if (g_havePrevGaze[eye])
        for (int i = 0; i < 4; ++i)
          if (std::fabs(g[i] - g_prevGaze[eye][i]) > threshold) saccade = true;
      memcpy(g_prevGaze[eye], g, sizeof(g));
      g_havePrevGaze[eye] = true;
      ++eye;
    }
  }
  if (saccade && g_cfg.saccadeHoldFrames > 0) {
    g_saccades++;
    g_holdUntil = frame + static_cast<uint64_t>(g_cfg.saccadeHoldFrames) - 1;
  }
  return frame <= g_holdUntil.load();
}

// R123 (focus_twin.h, [Model] FocusTwin): the classified views of each collect call (this frame's cameras, before
// the render graph runs). nullptr unless the focus twin is installed.
using CollectTapFn = void (*)(uint32_t count, const uint8_t* infos, const std::vector<ViewInfo>& views);
std::atomic<CollectTapFn> g_collectTap{nullptr};

// Three views (tvc above, three_views.h): a copy of the right periphery's ClippingVolume whose MAIN frustum is its
// outer strip (NDC x >= g_stripNdc; outer = away from the left periphery's apex). Two places change:
//   - the plane block at +0 (near, far, 4 sides): Scene.dll's object culling;
//   - the pyramid at +0x1b0 (byte perspective flag, apex Vec3d +0x1b8, near +0x1e8, far +0x1f0, corner count +0x1f8,
//     corners Vec3d[] +0x200, stride 0x18): edterrainGraphics41 rebuilds the terrain parse's culling planes from it
//     whenever it is a 3..8-corner pyramid with valid distances; Visualizer's TerrainParser copies only the 0x1b0-byte
//     plane block (never the exclusion list), which is why an exclusion volume leaves the terrain patches whole.
// The two inner corners move towards the outer ones along the image width; the outer edge, the top and bottom edges,
// the apex and the distances stay DCS's own. The exclusion list is dropped (the strip is outside the focus area).
// nullptr = not applicable (counted, logged while g_logLeft lasts). Measured 2026-11-19 (scratch view_drop.h, mask
// bit 1024): the right periphery's G-buffer primitives 19.06 M -> 9.57 M, image inside the strip unchanged.
constexpr size_t kVolPyrFlag = 0x1b0, kVolPyrApex = 0x1b8, kVolPyrNear = 0x1e8, kVolPyrCount = 0x1f8,
                 kVolPyrCorners = 0x200;
uint8_t* TvStripVolume(const ViewInfo& v, const ViewInfo& other, const uint8_t* src, const uint8_t* ci0) {
  const bool logIt = tvc::g_logLeft.load(std::memory_order_relaxed) > 0 && tvc::g_logLeft.fetch_sub(1) > 0;
  if (!v.fr.valid || !other.fr.valid) return nullptr;
  const Frustum& f = v.fr;
  Vec3 r = f.apex - other.fr.apex;
  r = r - f.forward * Dot(r, f.forward);
  if (Len(r) < 0.01) {  // one apex (no IPD): the centre directions of the mirrored frusta
    r = f.centerDir - other.fr.centerDir;
    r = r - f.forward * Dot(r, f.forward);
  }
  if (Len(r) < 1e-6) return nullptr;
  r = Normalize(r);
  const uint32_t n = *reinterpret_cast<const uint32_t*>(src + kVolPyrCount);
  const bool persp = src[kVolPyrFlag] != 0;
  const double* ap = reinterpret_cast<const double*>(src + kVolPyrApex);
  const Vec3 A{ap[0], ap[1], ap[2]};
  const int extra = *reinterpret_cast<const int32_t*>(src + kPlaneExtraCount);
  if (n != 4 || !persp || extra != 0 || Len(A - f.apex) > 0.05) {
    if (logIt)
      Log("three views: strip volume refused: corners %u perspective %d extra %d apex (%.3f %.3f %.3f) vs planes' "
          "(%.3f %.3f %.3f)", n, persp ? 1 : 0, extra, A.x, A.y, A.z, f.apex.x, f.apex.y, f.apex.z);
    return nullptr;
  }
  Vec3 e[4];
  double depth[4], u[4];
  Vec3 c{0, 0, 0};
  for (int k = 0; k < 4; ++k) {
    const double* p = reinterpret_cast<const double*>(src + kVolPyrCorners + k * 0x18);
    const Vec3 d = Vec3{p[0], p[1], p[2]} - A;
    depth[k] = Dot(d, f.forward);
    if (!(depth[k] > 1e-6)) return nullptr;
    e[k] = d * (1.0 / depth[k]);
    c = c + e[k] * 0.25;
  }
  int inner = 0;
  for (int k = 0; k < 4; ++k) u[k] = Dot(e[k] - c, r), inner += u[k] < 0 ? 1 : 0;
  if (inner != 2) return nullptr;
  const double w = Clamp((1.0 - tvc::g_stripNdc.load(std::memory_order_relaxed)) * 0.5, 0.02, 1.0);  // kept width
  Vec3 e2[4];
  for (int k = 0; k < 4; ++k) {
    e2[k] = e[k];
    if (u[k] >= 0) continue;
    const int a = (k + 1) & 3, b = (k + 3) & 3;
    const int q = u[a] > 0 ? a : b;  // the corner across the width (outer side)
    if (!(u[q] > 0)) return nullptr;
    e2[k] = e[q] + (e[k] - e[q]) * w;
  }
  const uint64_t key = 0xE000000000000000ull ^
                       ((static_cast<uint64_t>(*reinterpret_cast<const uint32_t*>(ci0 + kCiViewportTag)) << 8) |
                        static_cast<uint64_t>(v.index));
  uint8_t* dst = AcquireVolume(key);
  if (!dst) return nullptr;
  memcpy(dst, src, kVolExclPtr);
  *reinterpret_cast<uint8_t**>(dst + kVolExclPtr) = dst + kVolExclInline;
  *reinterpret_cast<uint64_t*>(dst + kVolExclCap) = kVolExclInlineCap;
  *reinterpret_cast<uint64_t*>(dst + kVolExclSize) = 0;
  for (int k = 0; k < 4; ++k) {
    double* p = reinterpret_cast<double*>(dst + kVolPyrCorners + k * 0x18);
    const Vec3 q = A + e2[k] * depth[k];
    p[0] = q.x, p[1] = q.y, p[2] = q.z;
  }
  // Plane block: DCS's near / far, sides through the apex and consecutive corners, normals towards the centre.
  uint8_t block[kPlaneBlockSize];
  memset(block, 0, kPlaneBlockSize);
  int idx = 0;
  WritePlane(block, idx++, f.nearP);
  if (f.hasFar) WritePlane(block, idx++, f.farP);
  const Vec3 pc = (e2[0] + e2[1] + e2[2] + e2[3]) * 0.25;
  for (int k = 0; k < 4; ++k) {
    Vec3 nn = Normalize(Cross(e2[k], e2[(k + 1) & 3]));
    if (Dot(nn, pc) < 0) nn = nn * -1.0;
    WritePlane(block, idx++, Plane{nn, -Dot(nn, f.apex)});
  }
  *reinterpret_cast<int32_t*>(block + kPlaneCount) = idx;
  *reinterpret_cast<int32_t*>(block + kPlaneExtraCount) = 0;
  memcpy(dst, block, kPlaneBlockSize);
  if (logIt) {
    const double nearD = *reinterpret_cast<const double*>(src + kVolPyrNear),
                 farD = *reinterpret_cast<const double*>(src + kVolPyrNear + 8);
    Log("three views: strip volume for collect view %d: width kept %.3f, outer dir (%+.3f %+.3f %+.3f), apex offset "
        "to the other periphery %.4f m, pyramid near %.3f far %.1f, %d planes (DCS's %d)",
        v.index, w, r.x, r.y, r.z, Len(f.apex - other.fr.apex), nearD, farD, idx,
        *reinterpret_cast<const int32_t*>(src + kPlaneCount));
  }
  return dst;
}

// Three views, engaged: every collection of the right periphery (the second periphery view in collect order) except
// the cockpit's (shading model 4 or viewport tag 600: they keep DCS's full volume) culls with the strip volume. Returns the right
// periphery's view index (R3's exclusion leaves it alone), or -1 when nothing was patched.
int TvPatch(uint32_t count, uint8_t* infos, Patch* patches, size_t maxPatches, int* patchedOut,
            const std::vector<ViewInfo>& views) {
  const ViewInfo* p[2] = {};
  int np = 0;
  for (const ViewInfo& v : views)
    if (v.role == 1 && np < 2) p[np++] = &v;
  if (np != 2) {
    tvc::g_refused++;
    return -1;
  }
  const ViewInfo& v = *p[1];
  uint8_t* ci0 = infos + v.index * kCollectionInfoStride;
  uint8_t* src = *reinterpret_cast<uint8_t**>(ci0 + kCiClipVolume);
  if (!src) {
    tvc::g_refused++;
    return -1;
  }
  for (const ViewInfo& o : views)  // a volume shared with another view would cull that view too
    if (o.index != v.index && *reinterpret_cast<uint8_t**>(infos + o.index * kCollectionInfoStride + kCiClipVolume) == src) {
      tvc::g_refused++;
      return -1;
    }
  uint8_t* dst = TvStripVolume(v, *p[0], src, ci0);
  if (!dst) {
    tvc::g_refused++;
    return -1;
  }
  int& n = *patchedOut;
  for (uint32_t i = 0; i < count && static_cast<size_t>(n) < maxPatches; ++i) {
    uint8_t* o = infos + i * kCollectionInfoStride;
    if (*reinterpret_cast<uint8_t**>(o + kCiClipVolume) != src) continue;
    // The cockpit keeps DCS's full volume: shading model 4 (its opaque G-buffer) and every collection under the
    // cockpit viewport tag 600 (shading models 9 / 10 there: the cockpit's transparent materials, the HUD combiner
    // glass and its frame among them; culled to the strip they went missing from the right eye's view).
    if (*reinterpret_cast<const uint16_t*>(o + kCiShadingModel) == 4 || *reinterpret_cast<const uint32_t*>(o + kCiViewportTag) == 600) {
      tvc::g_ckKept++;
      continue;
    }
    patches[n].slot = o + kCiClipVolume;
    patches[n].original = src;
    *reinterpret_cast<uint8_t**>(o + kCiClipVolume) = dst;
    ++n;
  }
  tvc::g_patched++;
  return v.index;
}

// Swaps in replacement volumes. Returns the number of patched views.
void Prepare(uint32_t count, uint8_t* infos, Patch* patches, size_t maxPatches, int* pairsOut,
             int* patchedOut, std::vector<ViewInfo>& views) {
  int pairs = Classify(count, infos, views);
  *pairsOut = pairs;
  if (const CollectTapFn tap = g_collectTap.load(std::memory_order_acquire)) tap(count, infos, views);
  if (g_dumpsLeft.load() > 0 && views.size() >= 4) {
    if (g_dumpsLeft.fetch_sub(1) > 0) DumpViews(count, infos, views);
  }
  if (pairs < 1) return;
  bool hold = UpdateGazeAndCheckHold(views);
  // Camera cut detection (scene_cut.h): the left periphery's camera.
  for (const ViewInfo& v : views)
    if (v.role == 1 && v.fr.valid) {
      const double pos[3] = {v.fr.apex.x, v.fr.apex.y, v.fr.apex.z};
      const double fwd[3] = {v.fr.forward.x, v.fr.forward.y, v.fr.forward.z};
      scenecut::OnCamera(pos, fwd);
      break;
    }
  // Three views (engaged by the worker only with the engine on): the right periphery's strip, before R3's exclusion.
  const int tvRight = tvc::g_engaged.load(std::memory_order_relaxed) && !g_faulted.load()
                          ? TvPatch(count, infos, patches, maxPatches, patchedOut, views)
                          : -1;
  if (!g_enabled.load() || g_faulted.load() || g_perceptionOnly.load(std::memory_order_relaxed)) return;
  const bool hole = g_debugHole.load();
  if (hold && !hole) return;
  const double keep = EffectiveKeep();
  if (keep <= 0.0 && !hole) return;

  int& n = *patchedOut;
  for (const ViewInfo& v : views) {
    if (static_cast<size_t>(n) >= maxPatches) break;
    if (v.index == tvRight) continue;  // three views: its volumes are the strip's
    const Frustum* excl = nullptr;
    bool shrink = true;
    if (v.role == 1 && v.partner >= 0) {
      excl = &views[v.partner].fr;
    } else if (hole && v.role == 2) {
      excl = &v.fr;  // debug: hide everything fully inside the focus frustum
      shrink = false;
    }
    if (!excl) continue;

    uint8_t* ci = infos + v.index * kCollectionInfoStride;
    uint8_t* src = *reinterpret_cast<uint8_t**>(ci + kCiClipVolume);
    uint8_t block[kPlaneBlockSize];
    BuildExclusionBlock(block, *excl, shrink ? keep : 1.0);

    uint64_t key = (static_cast<uint64_t>(*reinterpret_cast<uint32_t*>(ci + kCiViewportTag)) << 8) |
                   static_cast<uint64_t>(v.index);
    uint8_t* dst = AcquireVolume(key);
    if (!dst || !MakeVolumeWithExclusion(dst, src, block)) continue;
    // DCS culls every pass of a view (depth prepass, G-buffer, forward,
    // transparent...) in the same call, and passes of one view share one
    // ClippingVolume so Scene.dll culls them once. Swap the volume of all of
    // them: consistent results across passes and the sharing is preserved.
    for (uint32_t i = 0; i < count && static_cast<size_t>(n) < maxPatches; ++i) {
      uint8_t* other = infos + i * kCollectionInfoStride;
      if (*reinterpret_cast<uint8_t**>(other + kCiClipVolume) != src) continue;
      patches[n].slot = other + kCiClipVolume;
      patches[n].original = src;
      *reinterpret_cast<uint8_t**>(other + kCiClipVolume) = dst;
      ++n;
    }
  }
}

