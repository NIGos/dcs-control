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
std::atomic<uint64_t> g_quadFrame{0};
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

// Swaps in replacement volumes. Returns the number of patched views.
void Prepare(uint32_t count, uint8_t* infos, Patch* patches, size_t maxPatches, int* pairsOut,
             int* patchedOut, std::vector<ViewInfo>& views) {
  int pairs = Classify(count, infos, views);
  *pairsOut = pairs;
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
  if (!g_enabled.load() || g_faulted.load() || g_perceptionOnly.load(std::memory_order_relaxed)) return;
  const bool hole = g_debugHole.load();
  if (hold && !hole) return;
  const double keep = EffectiveKeep();
  if (keep <= 0.0 && !hole) return;

  int& n = *patchedOut;
  for (const ViewInfo& v : views) {
    if (static_cast<size_t>(n) >= maxPatches) break;
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

