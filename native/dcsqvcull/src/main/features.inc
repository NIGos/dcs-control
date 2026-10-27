#include "d3d_proxy.h"  // R146: DCS's D3D11 objects behind a device proxy (ReShade): detection, one log line
#include "shadow_tight.h"
#include "alloc_slab.h"
#include "tex_bind.h"
#include "part_weights.h"
#include "terrain_count.h"
#include "binder_count.h"
#include "inst_count.h"
#include "gb_count.h"
#include "shadow_tex.h"
#include "shadow_inst.h"
#include "shadow_batch.h"
#include "gb_batch.h"
#include "fx_apply_count.h"
#include "pacer.h"
#include "pose_sweep.h"
#include "edtime.h"
#include "srv_span_count.h"
#include "join_tail_count.h"
#include "shadow_rec_count.h"
#include "split_filter.h"
#include "srv_tail_trim.h"
#include "pass_flush.h"
#include "shadow_rec.h"
#include "gb_rec_count.h"
#include "gb_rec.h"
#include "fwd_rec_count.h"
#include "gpu_pass_timing.h"
#include "periph_lod.h"
#include "cascade_cache.h"  // R33: far shadow cascade static depth cache ([Shadows] CascadeCache, prototype)
#include "chain_fast.h"  // R34: shorter terrain/forest culling chains ([D3D] ChainFast, default 7)
#include "shader_fast.h"  // R36: wider grids for two single-SM kernels ([D3D] ShaderFast, default 0)
#include "dlss_views.h"  // R122: per-view DLSS preset ([DLSS] PeripheryPreset / FocusPreset / IgnoreDriverPreset, default 0)
#include "night_tone.h"   // R37: opt-in night darkening through DCS's tone mapper ([Night] NightTone, default 0)
#include "run_threads.h"
#include "frame_start.h"
#include "frame_heap.h"
#include "big_pages.h"
#include "par_upload.h"
#include "du_count.h"
#include "direct_upload.h"
#include "motion_probes.h"
#include "motion.h"
#include "view_profile.h"
#include "vram_count.h"
#include "prefetch_fix.h"
#include "perception_device.h"  // [General] PerceptionOnly: DCS's D3D11 context without the engine modules
#include "fixation_depth.h"  // Depth boost: fixation depth publisher ([Perception] FixationDepth)
#include "perf_mon.h"  // R30: env/perf/features/warmup/toggle/vram lines and the governor's driver
#include "thread_place.h"  // R39: [Threads] Census (default off) / WorkerPlacement (R44: default 2, selective)
#include "frame_timeline.h"  // R52: [Perf] FrameTimeline / PoseProbe / EventLog (measure only, default 0)
#include "focus_twin.h"  // R123: [Model] FocusTwin (opt-in, default 0): focus terrain patches replayed from the periphery
#include "vrs_probe.h"  // R139: [Suite] VrsProbe (measure only, default 0): NvAPI variable rate shading A/B probe
