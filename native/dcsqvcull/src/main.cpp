// DcsQvCull: CPU culling optimisation for DCS World in quad-view VR.
//
// The four quad views (two wide peripheral views, two narrow focus views) are
// culled independently by Graphics::SceneBase. Every object inside a focus
// frustum is drawn twice: once at high resolution in the focus view and once in
// the peripheral view, where the compositor then covers it with the focus
// layer. This DLL hands the peripheral views an extra "exclusion volume" equal
// to the (slightly shrunk) focus frustum of the same eye. Scene.dll already
// supports exclusion volumes natively: an object whose OBB lies entirely inside
// one is skipped, so it never reaches LOD selection, sorting, batching or the
// draw call submission of the peripheral view.
//
// Hooking is done by patching the DCSScene vtable exported by Scene.dll, so no
// code bytes are modified.

#include <windows.h>
#include <share.h>
#include <winternl.h>   // NTSTATUS for d3dkmthk.h
#include <d3dkmthk.h>   // D3DKMTQueryStatistics: GPU engine busy for the perf line and the governor (perf_mon.h)

#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <set>
#include <mutex>
#include <tuple>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "loader_api.h"
#include "log_utf8.h"
#include "dev_path.h"  // R62: [Dev] IniPath read whole, an unusable one logged
#include "reloc.h"
#include "status_word.h"
#include "scene_layout.h"
#include "scene_check.h"  // R63 U2: partition boost only on a Scene.dll whose collect code uses +0x40 as verified
#include "gov.h"       // R30: frame sampler, CPU/GPU-bound detector, governor stages (pure)
#include "ini_diff.h"  // R142: the keys an ini reload changed, the engine key set, frame times around a reload (pure)
#include "env_info.h"  // R30 L3: DCS version, OpenXR runtime and layers, streamers
#include "dcsvr_depth_export.h"  // Far depth export contract with Quad Views (verbatim copy; far_depth.h)
#include "dcsvr_cockpit_mask.h"  // R65: cockpit mask export for a frame-generation layer (cockpit_mask.h writes it)
#include "dcsvr_cockpit_depth.h"  // R69: its depth companion (R68 Stage 1; cockpit_mask.h writes it)
#include "dcsvr_motion.h"  // R70: its motion-vector companion (R68 Stage 2; cockpit_mask.h writes it)
#include "dcsvr_symbology.h"  // R95: its symbology emission companion (R89; cockpit_mask.h's symbology.h writes it)
#include "dcsvr_pupil_fov.h"  // R95: the pupil shift layer's FOV pairs (Dev's header, verbatim; the focus layout)
#include "dcsvr_perception_shm.h"  // Perception layer's shared blocks (verbatim copy of perception_shm.h; cockpit_ipd.h reads BoostOut)
#include "color_grade.h"  // R40: [Color] opt-in post-tonemap grading keys (default 0; pure parts, pass not wired yet)
#include "color_pass.h"   // R42: [Color] Mode 0/1/2 (off / Fix / Enhanced): tone-map pixel shader swap (default 0)
#include "sym_trace_cfg.h"  // R94: [Perception] SymbologyTrace keys (dev, default off; sym_trace.h does the trace)

using namespace layout;

// R107 (R106 F11): main.cpp's body is split into src\main\*.inc parts, included below in the original order
// (one unity build: every feature header uses main.cpp's globals and anonymous namespace, and the test TU
// includes this file). Each part is a byte-exact line range of the former main.cpp; nothing was reordered.
#include "main/log.inc"                       // async log queue (qvalog), Log, HookSlot / UnhookSlot, report capture
#include "main/config.inc"                    // Config, globals, chime, hotkey parse, timer resolution, ReadDouble, PerceptionOnlyConfig
#include "main/config_load.inc"               // LoadConfig: every ini key
#include "main/geometry.inc"                  // frustum decoding, exclusion volumes (R3 peripheral exclusion), stats globals
#include "main/log_writer.inc"                // exit guard / patch threads / disk prune includes, the async log writer thread
#include "main/views.inc"                     // core includes, timer config, view dump and classification, gaze
#include "main/features.inc"                  // the feature headers, in their include order
#include "main/wiring.inc"                    // Apply helpers, census, Quad Views publishing, Install, Apply* wiring
#include "main/passflush_bench.inc"           // bench mode 30, pass flush (R-class, F18)
#include "main/engine.inc"                    // shadow texture skip, engine switch, status word, colour tick, toggle, key reading
#include "main/xr_state.inc"                  // profile thread, XR state from dcs.log, CheckConfiguration
#include "main/suite.inc"                     // suite helpers and SuiteThread
#include "main/perception_tick.inc"           // StartBench, the perception configuration and tick
#include "main/worker.inc"                    // WorkerThread
#include "main/bench_variant.inc"             // SetBenchVariant and the engine-off bench switch
#include "main/exports.inc"                   // Stop, mission and shutdown, the DcsQvPayload_* exports
