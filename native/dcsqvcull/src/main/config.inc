struct Config {
  // Defaults below equal LoadConfig's ini fallbacks (an empty or missing ini
  // gives the same configuration as a fresh Config).
  bool enabled = false;          // [Cull] Enabled (peripheral exclusion: opt-in)
  bool debugHole = false;
  bool perceptionOnly = false;   // [General] PerceptionOnly (see g_perceptionOnly)
  // Fraction of the focus image (per axis, about its centre) that may be
  // excluded from the periphery. <0 = derive from Quad-Views-Foveated edge
  // smoothing: only the area where the focus layer is fully opaque.
  double keepFraction = -1.0;
  double safetyNdc = 0.05;       // extra NDC border kept when deriving it
  double nativeKeepFraction = 0.35;  // used when QVFR is not loaded (runtime-native quad views)
  double saccadeDeg = 1.0;       // gaze-in-head motion per frame treated as a saccade
  int saccadeHoldFrames = 6;     // frames without exclusion after a saccade
  double focusRatio = 0.80;      // focus half-angle must be below ratio * peripheral
  double apexTolerance = 0.02;   // metres between focus and peripheral eye position
  int collectThreadsMax = 0;     // 0 = leave DCS default
  int statsIntervalSec = 2;
  int dumpViews = 3;             // number of view dumps written to the log
  int benchBlocks = 24;          // A/B benchmark: number of blocks (even)
  int benchBlockSec = 5;
  double benchSettleSec = 1.0;   // ignored time at the start of each block
  int benchAutoStartSec = 0;     // >0: start automatically this long after quad views appear
  int benchThreadsB = 12;
  int profileSeconds = 20;
  int profileThreads = 24;
  int profilePeriodMs = 2;
  bool suiteProfile = false;
  bool suiteSelfTest = false;
  bool suiteBenchPartition = false;
  bool suiteBenchIsolation = false;
  bool suiteBenchTimerRes = false;
  bool diagnostics = false;      // measurement hooks always on
  int toggleVk = 122;            // [Hotkeys] Toggle, default Alt+Shift+F11
  int toggleMods = 6;
  bool developerKeys = false;    // Ctrl+Alt+F8/F9/F10/F12/PgUp/PgDn
  bool shadowTight = false;      // tighter cascade caster culling (shadow_tight.h)
  bool shadowDebugInvert = false;
  double shadowMarginFrac = 0.03;
  double shadowMarginMin = 2.0;
  double shadowFloorY = -1000.0;  // lowest visible receiver height (m)
  bool suiteBenchShadow = false;
  bool allocSlabs = false;       // [Model] AllocSlabs, per-thread slabs for NGModel's instance-data allocator
  int slabBytes = 4096;
  bool suiteBenchAllocSlabs = false;
  bool texDedupe = false;        // [Texture] StreamDedupe
  bool suiteBenchTexDedupe = false;
  bool suiteBenchEngine = true;   // all measured optimizations vs stock DCS (R107: default 1, as shipped)
  int motionSeconds = 60;         // run_motion.flag recording length
  bool suiteQuick = true;  // R107: default 1, as shipped
  bool triPlain = false;          // [Model] PlainTriangleCounter
  bool suiteBenchTriPlain = false;
  bool cbSkip = false;            // [Effects] SkipSameConstantBuffer
  bool suiteBenchCbSkip = false;
  bool suiteBenchMicro = false;
  bool costWeights = false;       // [Scene] CostWeights
  bool suiteBenchCostWeights = false;
  bool suiteTerrain = false;
  bool cbUploadSkip = false;      // [Effects] SkipSameConstantUpload
  bool suiteBenchCbUpload = false;
  bool pacerLowPower = false;     // [General] LowPowerPacer
  int pacerTimeoutUs = 10;
  bool suiteBenchPacer = false;
  bool motionSweep = false;       // [Suite] MotionSweep: synthetic head yaw during run_motion.flag
  bool motionProfile = false;
  bool motionTaxi = false;        // [Suite] MotionTaxi
  bool taskClock = false;         // [Timing] TaskQueueClock
  bool frameHeapSlabs = false;    // [Model] FrameHeapSlabs
  bool shadowTexSkip = true;      // [Model] ShadowTextureSkip (shadow_tex.h; masks from shadow_inst compiles)
  bool suiteBenchShadowTex = false;  // [Suite] BenchShadowTex
  bool suiteBenchTexTable = false;   // [Suite] BenchTexTable (texture dedupe table layout)
  bool suiteBenchBigPages = false;   // [Suite] BenchBigPages
  bool bigPages = true;              // [Model] BigModelPages (big_pages.h)
  bool parUpload = false;            // [Model] ParallelUpload (par_upload.h)
  bool suiteBenchParUpload = false;  // [Suite] BenchParallelUpload
  bool directUpload = false;            // [Model] DirectUpload (direct_upload.h, R16)
  bool suiteDirectUploadVerify = false;  // [Suite] DirectUploadVerify
  bool suiteBenchDirectUpload = false;   // [Suite] BenchDirectUpload
  bool motionCounters = true;        // [Suite] MotionCounters
  bool shadowInst = true;            // [Model] ShadowInstancing (shadow_inst.h: compiles the instanced shadow VS variants)
  bool suiteShadowInstCompile = false;  // [Suite] ShadowInstCompile
  bool suiteGBufferInstCompile = false;  // [Suite] GBufferInstCompile (R13 stage 1: main-pass model_vs variants)
  bool suiteGBufferTexCount = false;     // [Suite] GBufferTexCount (R14 lead 3: unread G-buffer texture sets)
  bool suiteFxApplyCount = false;        // [Suite] FxApplyCount (R14 lead 4: same-pass FX Apply counter)
  bool suiteDirectUploadCount = false;   // [Suite] DirectUploadCount (R16 gates G1-G3 for direct page upload)
  bool suiteSrvSpanCount = false;        // [Suite] SrvSpanCount (R15 F2: setShaderResources identical-tail counter)
  bool suiteJoinTailCount = false;       // [Suite] JoinTailCount (R15 F3: render-thread chunk tail at the culling join)
  bool suiteShadowRecCount = false;      // [Suite] ShadowRecCount (R17 S0: shadow recorder counters)
  bool suiteGBufferRecCount = false;     // [Suite] GBufferRecCount (R18 S0: G-buffer recorder counters and gates)
  bool suiteForwardRecCount = false;     // [Suite] ForwardRecCount (R21 S0: SimplePassData recorder counters and gates)
  bool suiteGpuPassTiming = false;       // [Suite] GpuPassTiming (gpu_pass_timing.h: GPU ms and context ops per pass)
  bool suiteGpuPassStats = false;        // [Suite] GpuPassStats (with GpuPassTiming: pipeline statistics and samples passed per pass)
  int peripheralLodScale = 0;            // [Model] PeripheralLodScale (periph_lod.h: LOSSY prototype, periphery instancer LOD factors x percent/100; 0 = off)
  int suitePeriphLodTiming = 0;          // [Suite] PeriphLodTiming (periph_lod.h: GPU timing OFF/ON x percent; 0 = off)
  int suitePeriphLodImage = 0;           // [Suite] PeriphLodImage (periph_lod.h: frames of final per-view images per configuration; 0 = off)
  bool suiteBenchPeripheralLod = false;  // [Suite] BenchPeripheralLod (bench mode 32, scale = PeriphLodTiming or 50)
  std::wstring dlssPeripheryPreset;      // [DLSS] PeripheryPreset (dlss_views.h, R122: LOSSY opt-in; "" / 0 = off, nothing installed; A-F, J-M or the NGX number)
  std::wstring dlssFocusPreset;          // [DLSS] FocusPreset (R122: 0 = the focus keeps DCS's / the driver's preset; only with PeripheryPreset)
  bool dlssIgnoreDriverPreset = false;   // [DLSS] IgnoreDriverPreset (R122: hide the driver profile's forced preset from DLSS; only before NGX starts)
  bool dlssInstrument = false;           // [DLSS] Instrument (R130, dev: read-only DLSS feature lifetime counters and summaries; 0 = nothing installed)
  bool dlssReleaseIdleSets = false;      // [DLSS] ReleaseIdleSets (R132, opt-in: release DCS's never-again-evaluated DLSS sets; 0 = nothing installed)
  int dlssReleaseAfterEvals = 300;       // [DLSS] ReleaseAfterEvals (R132: N evaluations of each newest-set handle before a release; 100..1000000)
  bool cascadeCache = false;             // [Shadows] CascadeCache (cascade_cache.h, R33: PROTOTYPE far-cascade static depth cache; 0 = off, nothing installed)
  uint32_t cascadeCacheMask = 0xc;       // [Shadows] CascadeCacheMask: cascades by CascadeShadowPass number (bit 2 = about 70-370 m, bit 3 = about 370-1500 m; bit 0 always refreshes: its unsnapped box is never seen)
  int cascadeCacheMaxAge = 4;            // [Shadows] CascadeCacheMaxAge: frames one capture may serve (1 = refresh every frame)
  int cascadeCacheMarginPct = 3;         // [Shadows] CascadeCacheMarginPct: window margin per side, percent of DCS's cascade extent
  int cascadeCacheVerify = 0;            // [Shadows] CascadeCacheVerify: compare every Nth reuse with a full redraw (test only; 0 = off)
  bool suiteCascadeCacheTiming = false;  // [Suite] CascadeCacheTiming (cascade_cache.h: GPU timing OFF/ON/OFF/ON/OFF, 5 s each)
  uint32_t chainFast = 7;                // [D3D] ChainFast (chain_fast.h, R34: shorter culling chains; bit 0 calcoffsets, bit 1 calccounters, bit 2 airflow offsets; default 7; 0 = off, nothing installed)
  bool suiteChainFastTiming = false;     // [Suite] ChainFastTiming (chain_fast.h: GPU timing OFF/ON/OFF/ON/OFF, 5 s each)
  int suiteChainFastVerify = 0;          // [Suite] ChainFastVerify: seconds of same-frame checks against the stock kernels (0 = off)
  uint32_t shaderFast = 3;               // [D3D] ShaderFast (shader_fast.h, R36: wider grids for single-SM kernels; bit 0 grass cull, bit 1 IM2 counts clear; default 3; 0 = off)
  bool suiteShaderFastTiming = false;    // [Suite] ShaderFastTiming (shader_fast.h: GPU timing OFF/ON/OFF/ON/OFF, 5 s each)
  int suiteShaderFastVerify = 0;         // [Suite] ShaderFastVerify: seconds of same-frame checks against the stock kernels (0 = off)
  int nightTone = 0;                     // [Night] NightTone (night_tone.h, R37: opt-in night darkening; 0 = off, nothing installed; 1 apply; 2 observe)
  double nightFloor = 0, nightToe = 0, nightEv = 0;  // [Night] NightExposureFloor, NightToe, NightExposureEV (0 = DCS's value)
  bool nightGate = true;                 // [Night] NightGate: scale by the night weight from the sun elevation
  bool suiteVramCount = false;           // [Suite] VramCount (vram_count.h: DXGI budget/usage, our memory, texture tables)
  int suiteVramCountSec = 20;            // [Suite] VramCountSec
  bool suiteVramCountCreates = true;     // [Suite] VramCountCreates: count DCS's buffer/texture creates during the phase
  // R49: default 1 = the shipped setting (DCS Control writes SplitFilter=1; ChainFast and ShaderFast rely on the split
  // filter's hooks). Before R49 the code default was 0, so an ini without the key ran without them.
  bool splitFilter = true;               // [D3D] SplitFilter (split_filter.h, R15 F5)
  uint32_t splitFilterOps = 0x4ff;       // [D3D] SplitFilterOps (sfilt::kDefaultOps: no blend, no depth)
  bool suiteSplitFilterVerify = false;   // [Suite] SplitFilterVerify
  bool suiteBenchSplitFilter = false;    // [Suite] BenchSplitFilter (bench mode 27)
  bool shadowBatch = true;           // [Model] ShadowBatching (shadow_batch.h; needs ShadowInstancing)
  bool suiteShadowInstVerify = false;   // [Suite] ShadowInstVerify
  bool suiteBenchShadowInst = false;    // [Suite] BenchShadowInst
  bool shadowPlanAsync = true;          // [Model] ShadowPlanAsync (shadow batching plans on planner threads)
  bool suiteBenchShadowPlanAsync = false;  // [Suite] BenchShadowPlanAsync
  bool shadowRecorder = false;          // [Model] ShadowRecorder (shadow_rec.h, R17 S3)
  uint32_t shadowRecorderScope = 0x30f; // R107: default as shipped; [Model] ShadowRecorderScope: bits 0-3 cascades, 8 untextured, 9 textured (0x30f: all)
  int shadowRecorderWaitUs = 200;       // [Model] ShadowRecorderWaitUs: render-thread wait for a job at the pass
  int shadowRecorderPriority = 0;       // [Model] ShadowRecorderPriority: worker THREAD_PRIORITY_* (-2..2)
  uint32_t shadowRecorderSplit = 0xf;   // [Model] ShadowRecorderSplit: cascades with helper workers (bits 0-3)
  int shadowRecorderHelpers = 3;        // [Model] ShadowRecorderHelpers: at most this many helpers per job (0-3)
  bool shadowRecorderInstancing = true;  // [Model] ShadowRecorderInstancing
  bool suiteShadowRecVerify = false;    // [Suite] ShadowRecVerify
  int suiteShadowRecVerifySec = 5;      // [Suite] ShadowRecVerifySec
  bool suiteBenchShadowRecorder = false;  // [Suite] BenchShadowRecorder (bench mode 28)
  bool gbufferBatch = false;            // [Model] GBufferBatching (gb_batch.h; needs ShadowInstancing)
  bool suiteGBufferInstVerify = false;  // [Suite] GBufferInstVerify
  bool suiteBenchGBufferInst = false;   // [Suite] BenchGBufferInst
  bool gbufferRecorder = false;            // [Model] GBufferRecorder (gb_rec.h, R18 S1-S3)
  // R123 focus twin (focus_twin.h; opt-in, default 0: nothing installed). The [Suite] keys tune / measure it.
  int focusTwin = 0;                       // [Model] FocusTwin (0 / 1)
  bool suiteFocusTwinShadow = false;       // [Suite] FocusTwinShadow: audits only, never skips (measure)
  int suiteFocusTwinAuditEvery = 64;       // [Suite] FocusTwinAuditEvery: focus passes per eye between audits (0 = trust audits only)
  int suiteFocusTwinTrust = 4;             // [Suite] FocusTwinTrust: passed audits per eye before the twin draws
  int suiteFocusTwinIsland = 16;           // [Suite] FocusTwinIsland: shortest run of replaced items (last frame)
  int suiteFocusTwinChunk = 64;            // [Suite] FocusTwinChunk: records per command list
  double suiteFocusTwinPredTol = 5e-5;     // [Suite] FocusTwinPredTol: prediction tolerance (relative)
  int focusTwinInstancer = 0;              // R125 [Model] FocusTwinInstancer (0 / 1; needs FocusTwin = 1): instanced trees and buildings too
  bool suiteFocusTwinPatches = true;       // R125 [Suite] FocusTwinPatches: 0 = terrain patch items stay DCS's (instancer items only)
  int suiteFocusTwinVerify = 0;            // [Suite] FocusTwinVerify: one same-frame stock-vs-twin compare per N focus passes per eye (0 = off)
  uint32_t gbufferRecorderScope = 0x10055; // R107: default as shipped; [Model] GBufferRecorderScope: bits 0-15 executions by ordinal, 16 A2C
  int gbufferRecorderWaitUs = 300;         // [Model] GBufferRecorderWaitUs: render-thread wait for a job at the pass
  int gbufferRecorderIsland = 30;          // [Model] GBufferRecorderIsland: shortest recorded run
  int gbufferRecorderMaxSegments = 12;     // [Model] GBufferRecorderMaxSegments: lists per execution (1-16)
  int gbufferRecorderHelpers = 2;          // [Model] GBufferRecorderHelpers: helper workers per job (0-2)
  bool gbufferRecorderCockpit = false;     // [Model] GBufferRecorderCockpit: cockpit draws and executions (R24)
  bool gbufferRecorderByOrdinal = true;    // [Model] GBufferRecorderByOrdinal: slots by plain ordinal (R107: default 1, shipped)
  bool suiteGBufferRecVerify = false;      // [Suite] GBufferRecVerify
  int suiteGBufferRecVerifySec = 6;        // [Suite] GBufferRecVerifySec
  bool suiteGBufferRecVerifyLive = false;  // [Suite] GBufferRecVerifyLive: test only, the recorder's verify while flying
  bool suiteGBufferRecVerifyNoLatch = false;  // [Suite] GBufferRecVerifyNoLatch: test only, differences logged only
  int suiteGBufferRecVerifyDump = 0;       // [Suite] GBufferRecVerifyDump: test only, R133 raw dumps of N compares
  bool suiteGBufferRecVerifyDumpAll = false;  // [Suite] GBufferRecVerifyDumpAll: test only, dump any difference
  int suiteGBufferRecVerifyStride = 0;     // [Suite] GBufferRecVerifyStride: every N-th draw residual (R18 T10)
  bool suiteBenchGBufferRecorder = false;  // [Suite] BenchGBufferRecorder (bench mode 29)
  uint32_t passFlush = 0;                  // [Model] PassFlush: pass_flush.h mask (0 = off)
  bool suiteBenchPassFlush = false;        // [Suite] BenchPassFlush (bench mode 30)
  uint32_t suiteBenchPassFlushMask = 0;    // [Suite] BenchPassFlushMask: the ON mask (0 = [Model] PassFlush)
  bool srvTailTrim = false;                // [Model] SrvTailTrim (srv_tail_trim.h, R15 F2)
  bool suiteSrvTailTrimVerify = false;     // [Suite] SrvTailTrimVerify
  int suiteSrvTailTrimVerifySec = 10;      // [Suite] SrvTailTrimVerifySec
  bool suiteBenchSrvTailTrim = false;      // [Suite] BenchSrvTailTrim (bench mode 31)
  int prefetchMode = 0;                    // [Prefetch] Mode: off (0) | observe (1) | skip (2), prefetch_fix.h
  uint32_t prefetchWindowMs = 5000;        // [Prefetch] WindowMs: repeat window (1-60000; R107: default as shipped)
  bool suiteBenchPrefetch = false;         // [Suite] BenchPrefetch (bench mode 33: observe vs skip)
  int prefetchTableBits = 20;              // [Prefetch] TableBits: repeat table 2^n entries (16-22, default 20; R39)
  bool threadCensus = false;               // [Threads] Census: per-thread CPU/affinity lines (thread_place.h, R39)
  int threadCensusSec = 60;                // [Threads] CensusSec (10-3600)
  int placeRecheckSec = 60;                // [Threads] PlaceRecheckSec: placement re-check once nothing moved (R52; R44 used 10 s)
  int placeRecheckMode = 1;                // [Threads] PlaceRecheckMode: 1 tracked handles, no system-wide snapshot (R90 default); 0 the snapshot pass
  int patchMode = 1;                       // [General] PatchMode: 1 code-site writes without the system-wide snapshot (R90 default); 0 the Toolhelp path
  bool asyncLog = true;                    // [Log] Async: DCS threads' log lines through the async ring (R90 default); 0 synchronous
  int shadowPlanWaitUs = 300;              // [Model] ShadowPlanWaitUs: render-thread wait for a planner's plan before the inline one (R90 H1; 20000 = pre-R90)
  int frameTimeline = 0;                   // [Perf] FrameTimeline: 0 off, 1 summaries, 2 + per-frame CSV (frame_timeline.h, R52)
  int timelineSec = 10;                    // [Perf] TimelineSec: summary window (2-600 s)
  bool poseProbe = false;                  // [Perf] PoseProbe: head pose re-located at xrBeginFrame / xrEndFrame (R52)
  bool eventLog = false;                   // [Perf] EventLog: worker tasks, device creates, page faults per frame (R52)
  int workerPlacement = 2;                 // [Threads] WorkerPlacement: 0 off, 1 all our threads off DCS's render core (R39; R41 §9 measured it slower), 2 selective (R44 default): threads off the render path moved, recorder workers / planners / upload helpers on the whole process mask
  int holdYawDeg = -1;               // [Suite] HoldYawDeg: constant view yaw for unattended runs (-1 = off)
  double holdPitchDeg = 0;           // [Suite] HoldPitchDeg: test only, constant view pitch with the held yaw (down +)
  bool holdPose = false;             // [Suite] HoldPose: test only, the head pose frozen (pose_sweep.h)
  bool poseWallClock = false;        // [Suite] PoseWallClock: test only, the synthetic yaw from the wall clock at each locate (pre-R99) instead of its displayTime
  double poseRotateDps = 0;          // [Suite] PoseRotateDps: test only, constant-rate synthetic yaw turn (0 = off)
  bool suiteYawScan = false;         // [Suite] YawScan: fps per held yaw, 0..345 in 15 deg steps
  bool suiteYawProfile = false;      // [Suite] YawProfile: cost split and bound per held yaw (view_profile.h)
  int suiteYawProfileStep = 30;      // [Suite] YawProfileStep: degrees between profiled yaws
  bool suiteRotationProfile = false;  // [Suite] RotationProfile: cost split, transients and spikes while turning
  int suiteRotationDegPerSec = 60;    // [Suite] RotationDegPerSec
  int suiteRotationSeconds = 20;      // [Suite] RotationSeconds
  bool suiteFrameStartGap = false;    // [Suite] FrameStartGap: frame-start GPU bubble attribution (frame_start.h, R22 E1)
  int suiteFrameStartGapSec = 10;     // [Suite] FrameStartGapSec: standalone phase (0 = only inside Yaw/RotationProfile)
  bool suiteRunnableThreads = false;  // [Suite] RunnableThreads: runnable-waiting DCS threads (run_threads.h, R22 E3)
  int suiteRunnableThreadsSec = 10;   // [Suite] RunnableThreadsSec: standalone phase (0 = only inside the profiles)
  int suiteRunnableThreadsEvery = 16; // [Suite] RunnableThreadsEvery: frames between snapshots
  uint32_t bigPageBytes = 4u << 20;  // [Model] BigPageBytes
  bool suiteBenchFrameHeap = false;     // [Suite] MotionProfile: CPU profile of the sweep's translation phase        // [Suite] Quick: configuration check + A/B only
  int suiteBenchFilter = 0;  // 1 = end-to-end A/B, 2 = also filter vs hooks
  bool d3dFilter = false;        // skip redundant D3D11 state calls
  bool fineTimer = false;
  bool partitionBoost = false;
  bool suiteBenchCull = false;
  bool suiteBenchThreads = false;
  bool suiteBenchTimer = false;
  bool d3dMeter = false;         // [D3D] Meter: count redundant D3D11 state calls (diagnostic)
  bool shaderTimeCache = true;   // cache dx11backend's per-draw ED_get_time
  int shaderTimeCacheUs = 1000;
  // R30 field diagnostics (perf_mon.h) and the GPU-bound governor (gov.h).
  bool recorderDetail = false;   // [Log] RecorderDetail: the recorders' full 10-s blocks (else one line each)
  int perfIntervalSec = 10;      // [Log] PerfIntervalSec: perf/features lines (0 = off)
  int vramIntervalSec = 60;      // [Log] VramIntervalSec: vram line (0 = off)
  bool govEnabled = true;        // [Governor] Enabled
  int govGpuBusyPct = 92;        // [Governor] GpuBusyPct: GPU busy at or above = the GPU is full
  int govRenderIdlePct = 85;     // [Governor] RenderIdlePct: render thread busy at or below = it waits
  int govCpuBusyPct = 92;        // [Governor] CpuBusyPct: render thread busy at or above = CPU-bound
  int govGpuFreePct = 80;        // [Governor] GpuFreePct: GPU busy below = the GPU has headroom
  int govGpuFullPct = 98;        // [Governor] GpuFullPct: GPU busy at or above = GPU-bound (render thread may spin-wait)
  int govWindowSec = 8;          // [Governor] WindowSec: verdicts considered
  int govNeedSec = 6;            // [Governor] NeedSec: of them needed for a step
  int govDwellSec = 10;          // [Governor] MinDwellSec: minimum time per stage
  double govMinGainPct = 2.0;    // [Governor] MinGainPct: fps gain a step down must show (and a step up may lose at most)
  int govTrialSec = 20;          // [Governor] TrialSec: fps window before and after a step (after a 2 s settle)
  int govRecheckSec = 10;        // [Governor] RecheckSec: the old stage again for this long (A-B-A; 0 = before/after only)
  int govRevertBackoffSec = 300; // [Governor] RevertBackoffSec: a reverted stage is blocked this long (doubling)
  // Depth boost's fixation depth (fixation_depth.h). Not an optimization:
  // outside the kill switch and the status word's features.
  bool fixationDepth = false;     // [Perception] FixationDepth: publish Local\DcsVrFixationDepth_v1
  bool sceneCutReset = true;      // [Perception] SceneCutReset: reset Quad Views' temporal history on camera cuts
  bool depthGrid = false;         // [Perception] DepthGrid: publish Local\DcsVrDepthGrid_v1 (32 x 32 grid of the left periphery)
  int fixationDepthVerify = 0;    // [Perception] FixationDepthVerify: sample every view run in turn, log the view map
  bool fixationDepthDump = false; // [Perception] FixationDepthDump: one window as a PGM in the dev folder
  std::wstring fixationTestGaze;  // [Perception] FixationDepthTestGaze: "x,y" tangents written as GazeOut if absent
  bool farDepthExport = false;    // [Perception] FarDepthExport: focus views' depth to Quad Views (when it wants them)
  int farDepthSlots = 2;          // [Perception] FarDepthExportSlots: copies per focus view (1 or 2)
  int farDepthLogProj = 0;        // [Perception] FarDepthLogProj: log the focus views' P20/P21 for N frames
  bool farDepthPreOverlay = true; // [Perception] FarDepthPreOverlay: each focus view's colour before the overlays
  int farDepthCoverageMode = 1;   // [Perception] FarDepthCoverageMode: hdr (1, the default; per-view fallback to overlay) or overlay (0)
  bool farDepthPostOverlay = true; // [Perception] FarDepthPostOverlay: also each focus view's colour after the overlays (at (w, 0))
  int farDepthOverlayTrace = 0;   // [Perception] FarDepthOverlayTrace: log the focus views' tone-map..overlay draws for N frames
  // R65 cockpit mask export (cockpit_mask.h, contract dcsvr_cockpit_mask.h): per HMD view, where DCS drew cockpit
  // geometry, for a frame-generation layer. Off by default.
  bool cockpitMaskExport = false;  // [Perception] CockpitMaskExport
  int cockpitMaskDivisor = 0;      // [Perception] CockpitMaskDivisor: source texels per mask texel (1-8; 0 auto: <= 512 wide)
  int cockpitMaskViews = 3;        // [Perception] CockpitMaskViews: HMD view bit mask (3 periphery, 15 + focus)
  double cockpitMaskNearM = 1.0;   // [Perception] CockpitMaskNearM: also nearer than this (m; 0 = stencil only)
  int cockpitMaskDilate = 0;       // [Perception] CockpitMaskDilate: extra mask texels of max filter (0-4)
  int cockpitMaskDump = 0;         // [Perception] CockpitMaskDump: write N committed masks as PGM (debug)
  int cockpitMaskLog = 0;          // [Perception] CockpitMaskLog: log N commits (displayTime pairing check)
  bool cockpitDepthExport = false; // [Perception] CockpitDepthExport (R69): the mask's depth companion (needs the mask)
  int cockpitDepthDump = 0;        // [Perception] CockpitDepthDump: write N committed depth atlases (debug)
  bool cockpitMotionExport = false; // [Perception] CockpitMotionExport (R70): DCS's MVs of views 0/1 (needs the mask)
  bool cockpitMotionClass = true;   // [Perception] CockpitMotionClass: the class byte atlas with the MVs
  int cockpitMotionDump = 0;        // [Perception] CockpitMotionDump: write N committed motion slots (debug)
  int cockpitFgDump = 0;            // [Perception] CockpitFgDump (R71): N consecutive frames of MV / class / depth /
                                    // colour + JSON for frame-generation fixtures (dev; needs mask, depth, motion)
  bool symbologyExport = false;     // [Perception] SymbologyExport (R95): HUD / helmet emission companion (needs the mask)
  bool symbologyAlpha = true;       // [Perception] SymbologyAlpha (R95): its coverage alpha (default 1)
  bool symbologyPublishAlways = false;  // [Dev] SymbologyPublishAlways (R97): without the symbology reader's heartbeat
  // [Perception] CockpitTrueIpd* (cockpit_ipd.h, R31 option 3): cockpit passes at the true eye distance while
  // Depth boost renders the world at the boosted one. CockpitTrueIpd defaults off (the app's ini turns it on);
  // Forward, Helmet, Overlays and OverlayMvp default on (they act only with CockpitTrueIpd), the rest off / neutral.
  int cockpitTrueIpd = 0;          // CockpitTrueIpd: 0 off, 1 on, 2 observe (armed, nothing modified, logs only)
  bool cockpitTrueIpdLighting = true;   // CockpitTrueIpdLighting (R107: default 1, shipped): phase 2, cockpit-stencil pixels lit from the true eye
  bool cockpitTrueIpdForward = true;    // CockpitTrueIpdForward: also TransparentCockpitPass / ...DecalPass
  bool cockpitTrueIpdHelmet = true;     // CockpitTrueIpdHelmet: helmet models drawn with their eye's camera
  int cockpitTrueIpdOwnship = 0;        // CockpitTrueIpdOwnship: the own aircraft's exterior model at the true eye
  bool cockpitIpdDormant = true;        // CockpitIpdDormant (R128): frames at boost scale 1 skip the work unused at h 0
  bool cockpitTrueIpdOverlays = true;   // CockpitTrueIpdOverlays: also the per-view overlay passes (HUD / MFD symbology)
  bool cockpitTrueIpdOverlayMvp = true; // CockpitTrueIpdOverlayMvp: in them, the effect buffers' CPU-built MVPs too
  int cockpitTrueIpdLog = 0;       // CockpitTrueIpdLog: per-upload trace for the next N frames
  int cockpitTrueIpdPrevLog = 0;   // CockpitTrueIpdPrevLog: gPrevFrameViewProj trace for the next N frames
  int cockpitTrueIpdFrameDump = 0;  // CockpitTrueIpdFrameDump: dev only, N frames of HMD views 2 / 3 as BMP + CSV
  int cockpitTrueIpdMvpDump = 0;    // CockpitTrueIpdMvpDump: dev only, the first N overlay matrices, one line each
  int cockpitTrueIpdFrameDumpViews = 0x0C;  // CockpitTrueIpdFrameDumpViews: digits 0-3 and m (mirror), default 23
  bool cockpitTrueIpdOverlayRestore = false;  // CockpitTrueIpdOverlayRestore: effect buffers back to DCS's bytes per pass
  int cockpitTrueIpdOverlayPath = 0;  // CockpitTrueIpdOverlayPath: cpu (0, default, R32) or d3d11 (1, A/B only)
  double cockpitTrueIpdTestShiftMm = 0;  // CockpitTrueIpdTestShiftMm: dev only, shift per eye without the layer
  // CockpitTrueIpdToggle: in-flight A/B key. R107: default 0:0 (off, as shipped); 118:6 = Alt+Shift+F7 (dev only).
  int cockpitTrueIpdToggleVk = 0;
  int cockpitTrueIpdToggleMods = 0;
};

Config g_cfg;
bool g_mute = false;  // set by the offline tests

// Audible cue for the user in the headset (Beep is synchronous).
std::atomic<bool> g_beeps{true};  // [General] Beeps
void Chime(DWORD freq, DWORD ms) {
  if (!g_mute && g_beeps.load(std::memory_order_relaxed)) Beep(freq, ms);
}
double g_tscHz = 0;
void ApplyTimerConfig();
void BenchUseCompactTexTable(bool on);
void ApplyBigPages(bool on);
void ApplyParUpload(bool on);
void ApplyDirectUpload(bool on);
void ApplySplitFilter(bool on);
void ApplyTextureHook();
void ApplyTriCounter();
void ApplyCbSkip();
void ApplyCbUpload();
void ApplyPacer();
void ApplyTaskClock();
void ApplyFrameHeap();
void ApplyShadowTexSkip();
void ApplyShadowInst();
void ApplyShadowBatch();
void ApplyGBufferBatch();
void ApplyShadowRecorder();
void ApplyGBufferRecorder();
void ApplyPassFlush();
void ApplySrvTailTrim(bool on);
void ApplyPrefetch();
void ApplyHoldYaw(int deg);
void ApplyPoseTest(double pitchDeg, double rateDps, bool holdPose, bool wallClock);
void ApplyR90Config();  // R90: PatchMode, [Log] Async (main.cpp, after patch_threads.h)
std::atomic<bool> g_enabled{true};
std::atomic<bool> g_debugHole{false};
std::atomic<bool> g_faulted{false};
FILETIME g_iniTime{};
std::atomic<bool> g_benchRunningFlag{false};
std::atomic<bool> g_partitionBoost{false};  // see HookCollect
// In-flight kill switch ([Hotkeys] Toggle): true turns off, together, the
// measured optimizations the ini enabled. Reset whenever the ini changes.
std::atomic<bool> g_engineOff{false};
// [General] PerceptionOnly=1 (default 0): the app installs DcsQvCull for Depth
// boost alone (Engine Optimizations off). Latched by the initial config read
// (a later ini change takes effect at the next DCS start). While set, only the
// perception features are installed: fixation depth / DepthGrid, the far
// depth export, CockpitTrueIpd (option 3, its R32 CPU path, StereoAck,
// BoostOut) and CockpitTrueIpdOwnship (stage 1: the G-buffer recorder is off).
// No engine optimization, recorder, benchmark, suite, diagnostics hook,
// governor action, engine toggle or Ctrl+Alt key; DcsQvCull_Status reports
// running + PerceptionOnly only (R62, status_word.h bit 38).
// The hooks it keeps, all pass-through or observing:
//   - Scene.dll collectSceneObjectsRenderables (HookCollect): view
//     classification only, for the eye separation (NoteCullSeparation,
//     option 3's measured-separation record pick) and the camera cut
//     (SceneCutReset); no volume patched, no thread limit touched.
//   - GraphicsCore BaseRenderingPass::execute (pass_timing.h): fixation
//     depth's pass-boundary callback; no timing recorded.
//   - G-buffer pass execute (gb_batch.h gbpass): option 3's observer, no
//     batching wrapper.
//   - DX11Renderer::draw (perception_device.h): one call, to learn DCS's
//     D3D11 context (shadow instancing / the split filter found it before);
//     the slot is restored at once.
//   - The perception modules' own hooks (fixation_depth.h, cockpit_ipd.h,
//     ovl_cpu.h, far_depth.h), as with PerceptionOnly=0.
std::atomic<bool> g_perceptionOnly{false};
// R30 governor (gov.h, perf_mon.h): the recorders' stage (0 = as configured,
// 1 = shadow split & 0x8, 2 = no helper lists, 3 = recorders paused), read by
// ApplyShadowRecorder/ApplyGBufferRecorder; the last verdict and the
// shader-variant warm-up for the status word.
std::atomic<int> g_govStage{0};
std::atomic<int> g_verdictNow{0};
std::atomic<bool> g_warming{false};
qvstatus::Feature g_featLast[18] = {};  // ComputeStatus's last states (the features line)
void PerfOnToggle(bool off);

// Parses "virtual-key:modifiers" (decimal; modifiers Ctrl 1, Alt 2, Shift 4).
// "0:0" disables the key. Returns false for anything else malformed or out of
// range (key 1-254, modifiers 0-7), leaving vk = mods = 0.
bool ParseHotkey(const wchar_t* text, int& vk, int& mods) {
  vk = mods = 0;
  if (!text) return false;
  wchar_t* end = nullptr;
  long k = wcstol(text, &end, 10);
  if (end == text || *end != L':') return false;
  const wchar_t* m0 = end + 1;
  long m = wcstol(m0, &end, 10);
  if (end == m0) return false;
  while (*end == L' ' || *end == L'\t') ++end;
  if (*end != 0) return false;
  if (k == 0 && m == 0) return true;  // explicitly off
  if (k < 1 || k > 254 || m < 0 || m > 7) return false;
  vk = static_cast<int>(k);
  mods = static_cast<int>(m);
  return true;
}
// R42 colour modes (color_pass.h): the ini's [Color] values (config thread) and the ModeKey's choice
// (-1 = none; it cycles 0 -> 1 -> 2 -> 0 until the ini's Mode changes).
cpass::Cfg g_colorCfg;
int g_colorKeyVk = 0, g_colorKeyMods = 0;  // [Color] ModeKey, off unless set
std::atomic<int> g_colorKeyMode{-1};
std::atomic<int> g_d3dMode{0};  // D3D11 filter mode, see d3dstate.h
std::atomic<bool> g_shadowTight{false};        // shadow_tight.h
std::atomic<bool> g_shadowDebugInvert{false};  // draw only the casters that would be removed
std::atomic<uint64_t> g_frameShadowRend{0};    // cascade renderables of the last quad frame (benchmark)
std::atomic<bool> g_allocSlabsOn{false};       // alloc_slab.h
std::atomic<uint32_t> g_allocSlabBytes{4096};
std::atomic<bool> g_texDedupeOn{false};        // tex_bind.h
std::atomic<bool> g_hookMeasure{false};
std::atomic<bool> g_triPlainOn{false};         // tricount (alloc_slab.h)
std::atomic<bool> g_cbSkipOn{false};           // cbskip (tex_bind.h)
std::atomic<bool> g_costWeightsOn{false};      // part_weights.h
std::atomic<bool> g_costWeightsSanity{false};
std::atomic<bool> g_cbUploadSkipOn{false};     // cbupload (tex_bind.h)
std::atomic<bool> g_pacerLowPowerOn{false};    // pacer.h
std::atomic<bool> g_taskClockOn{false};        // edtime.h
std::atomic<bool> g_frameHeapOn{false};        // frame_heap.h
// Frame-heap generation: +1 at every TFrameMemoryHeap<1>::reset_memory (frame_heap.h's
// hook). The renderables the recorders' workers read live in that heap: a job whose
// generation changed reads recycled memory (gb_rec.h HeapRecycled).
std::atomic<uint64_t> g_frameHeapGen{1};
// QPC of the last scene change: the G-buffer executions moved to another
// render graph (gb_rec.h), or the Lua hook reported a simulation stop, a
// mission load or a local player slot change. DCS frees the previous scene's
// objects around it while frames still render (no heap reset in between), so
// a recorder fault in a job started near it is contained per job
// (shrec::SceneChanging) instead of latching the recorder off.
std::atomic<int64_t> g_sceneChangeQpc{0};
std::atomic<bool> g_shadowTexSkipOn{false};    // shadow_tex.h        // time the allocator/texture hooks (suite counters only)

// System timer resolution for the DCS process (NtSetTimerResolution, 100 ns
// units). DCS reports 15.5 ms; Sleep(1) then lasts up to a full tick.
ULONG g_timerResCurrent = 0;
bool SetTimerResolution(bool fine) {
  using Fn = LONG(NTAPI*)(ULONG, BOOLEAN, PULONG);
  static Fn fn = reinterpret_cast<Fn>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtSetTimerResolution"));
  if (!fn) return false;
  ULONG actual = 0;
  LONG st = fn(5000, fine ? TRUE : FALSE, &actual);
  g_timerResCurrent = actual;
  return st == 0;
}
// OpenXR session state tracked from dcs.log by the worker thread.
std::atomic<bool> g_xrFocused{true};
std::atomic<uint64_t> g_focusLosses{0};

// R107: qvini::Double (ini_read.h): missing, empty or unparsable -> the default (was _wtof: "abc" or an empty value
// read as 0, unlike color_grade.h's IniD); an unparsable value is logged.
double ReadDouble(const wchar_t* sec, const wchar_t* key, double def, const std::wstring& ini) {
  bool bad = false;
  const double v = qvini::Double(sec, key, def, ini.c_str(), &bad);
  if (bad) Log("config: [%ls] %ls is not a number; using its default %g", sec, key, def);
  return v;
}

// Dev mode: [Dev] IniPath in the deployed ini names a developer ini that
// replaces it entirely (both are watched); run_suite.flag is also accepted
// next to that file.
std::wstring g_devDir;
FILETIME g_devIniTime{};

// [Perception] FarDepthCoverageMode: "overlay" -> 0; absent, "hdr" or anything
// else -> 1 (hdr, far_depth.h 5d/5e); "anything else" sets *invalid.
int ParseFarCoverageMode(const wchar_t* m, bool* invalid) {
  *invalid = false;
  if (_wcsicmp(m, L"overlay") == 0) return 0;
  if (m[0] && _wcsicmp(m, L"hdr") != 0) *invalid = true;
  return 1;
}
bool g_farCovInvalidLogged = false;  // the invalid value is logged once per process
int g_farCovInvalidLogs = 0;

// [General] PerceptionOnly=1: the configuration that is applied. A fresh
// Config (every suite, benchmark and diagnostics key off) with the engine
// optimizations whose defaults are on switched off, the engine keys off, and
// the [Perception] keys, the view classification thresholds and the log
// cadence taken from the ini. A new engine feature whose Config default is
// on must be added to the list below.
Config PerceptionOnlyConfig(const Config& c) {
  Config p;
  p.perceptionOnly = true;
  // Engine optimizations on by default.
  p.shadowTexSkip = p.bigPages = p.shadowInst = p.shadowBatch = p.shadowPlanAsync = false;
  p.shaderTimeCache = false;
  p.govEnabled = false;
  p.motionCounters = false;
  // R48: the R34 / R36 / R44 defaults that are on (ChainFast=7, ShaderFast=3, WorkerPlacement=2) were missing from
  // this list. ChainFast / ShaderFast were held off by the worker's PerceptionOnly guard only, and WorkerPlacement
  // (tplace::Tick, not guarded) only by the render thread staying unknown without the engine recorders.
  p.chainFast = p.shaderFast = 0;
  p.workerPlacement = 0;
  p.splitFilter = false;  // R49: its Config default is now on (the shipped setting)
  p.suiteBenchEngine = p.suiteQuick = false;  // R107: their Config defaults are now on (shipped); no suite here
  // The engine toggle and the Ctrl+Alt keys (inert in the worker regardless).
  p.toggleVk = p.toggleMods = 0;
  p.developerKeys = false;
  // View classification (HookCollect: eye separation, camera cuts); [Cull] Enabled stays off.
  p.keepFraction = c.keepFraction;
  p.safetyNdc = c.safetyNdc;
  p.nativeKeepFraction = c.nativeKeepFraction;
  p.saccadeDeg = c.saccadeDeg;
  p.saccadeHoldFrames = c.saccadeHoldFrames;
  p.focusRatio = c.focusRatio;
  p.apexTolerance = c.apexTolerance;
  // Log lines.
  p.statsIntervalSec = c.statsIntervalSec;
  p.dumpViews = c.dumpViews;
  p.perfIntervalSec = c.perfIntervalSec;
  p.vramIntervalSec = c.vramIntervalSec;
  // [Perception]
  p.fixationDepth = c.fixationDepth;
  p.sceneCutReset = c.sceneCutReset;
  p.depthGrid = c.depthGrid;
  p.fixationDepthVerify = c.fixationDepthVerify;
  p.fixationDepthDump = c.fixationDepthDump;
  p.fixationTestGaze = c.fixationTestGaze;
  p.farDepthExport = c.farDepthExport;
  p.farDepthSlots = c.farDepthSlots;
  p.farDepthLogProj = c.farDepthLogProj;
  p.farDepthPreOverlay = c.farDepthPreOverlay;
  p.farDepthCoverageMode = c.farDepthCoverageMode;
  p.farDepthPostOverlay = c.farDepthPostOverlay;
  p.farDepthOverlayTrace = c.farDepthOverlayTrace;
  p.cockpitMaskExport = c.cockpitMaskExport;  // R65
  p.cockpitMaskDivisor = c.cockpitMaskDivisor;
  p.cockpitMaskViews = c.cockpitMaskViews;
  p.cockpitMaskNearM = c.cockpitMaskNearM;
  p.cockpitMaskDilate = c.cockpitMaskDilate;
  p.cockpitMaskDump = c.cockpitMaskDump;
  p.cockpitMaskLog = c.cockpitMaskLog;
  p.cockpitDepthExport = c.cockpitDepthExport;  // R69
  p.cockpitDepthDump = c.cockpitDepthDump;
  p.cockpitMotionExport = c.cockpitMotionExport;  // R70
  p.cockpitMotionClass = c.cockpitMotionClass;
  p.cockpitMotionDump = c.cockpitMotionDump;
  p.cockpitTrueIpd = c.cockpitTrueIpd;
  p.cockpitTrueIpdLighting = c.cockpitTrueIpdLighting;
  p.cockpitTrueIpdForward = c.cockpitTrueIpdForward;
  p.cockpitTrueIpdHelmet = c.cockpitTrueIpdHelmet;
  p.cockpitTrueIpdOwnship = c.cockpitTrueIpdOwnship;  // stage 2 runs as stage 1 (no G-buffer recorder)
  p.cockpitIpdDormant = c.cockpitIpdDormant;  // R128
  p.cockpitTrueIpdOverlays = c.cockpitTrueIpdOverlays;
  p.cockpitTrueIpdOverlayMvp = c.cockpitTrueIpdOverlayMvp;
  p.cockpitTrueIpdLog = c.cockpitTrueIpdLog;
  p.cockpitTrueIpdPrevLog = c.cockpitTrueIpdPrevLog;
  p.cockpitTrueIpdFrameDump = c.cockpitTrueIpdFrameDump;
  p.cockpitTrueIpdMvpDump = c.cockpitTrueIpdMvpDump;
  p.cockpitTrueIpdFrameDumpViews = c.cockpitTrueIpdFrameDumpViews;
  p.cockpitTrueIpdOverlayRestore = c.cockpitTrueIpdOverlayRestore;
  p.cockpitTrueIpdOverlayPath = c.cockpitTrueIpdOverlayPath;
  p.cockpitTrueIpdTestShiftMm = c.cockpitTrueIpdTestShiftMm;
  p.cockpitTrueIpdToggleVk = c.cockpitTrueIpdToggleVk;
  p.cockpitTrueIpdToggleMods = c.cockpitTrueIpdToggleMods;
  return p;
}

// [General] PerceptionOnly: every engine-side runtime flag off (LoadConfig
// applies nothing else in this mode).
void PerceptionOnlyFlags() {
  g_enabled = false;
  g_debugHole = false;
  g_partitionBoost = false;
  g_engineOff = false;
  g_allocSlabsOn = false;
  g_texDedupeOn = false;
  g_triPlainOn = false;
  g_cbSkipOn = false;
  g_costWeightsOn = false;
  g_cbUploadSkipOn = false;
  g_pacerLowPowerOn = false;
  g_taskClockOn = false;
  g_frameHeapOn = false;
  g_shadowTexSkipOn = false;
  g_shadowTight = false;
  g_shadowDebugInvert = false;
  g_d3dMode = 0;
}

// R107: keys of research features removed from src (archived in DcsQvCull/research_archive, one folder each, with a
// README on how to restore it). An ini may still carry them: DCS Control's template wrote several as constants (0),
// and a dev ini may hold others. They do nothing now. A switch key (kSwitch: non-zero turned the feature or probe on)
// with a non-zero value is logged once per key and value in the session (a changed value logs again), so a stale dev
// ini that still expects the feature is visible in the log; 0 / empty / non-numeric stays silent. A tuning key
// (kTuning: a parameter of the feature, often with a non-zero old default such as GpuDedupWarm=30, or a path) is never
// logged: it did nothing without its switch, and the dev ini still carries the old defaults (R115 I3). Nothing else
// reads these keys.
constexpr bool kSwitch = true, kTuning = false;
struct RetiredKey {
  const wchar_t* sec;
  const wchar_t* key;
  const char* what;  // the removed feature and its archive folder
  bool sw;           // kSwitch: logged when non-zero; kTuning: never logged
};
constexpr const char* kRetFbc = "R53 / R38 rank 2 focus broadcast, research_archive\\R53_R38_focus_broadcast";
constexpr const char* kRetPif = "R38 rank 1 periph inst filter, research_archive\\R38_periph_inst_filter";
constexpr const char* kRetR40 = "R40 [Color] grading (superseded by R42 Mode), research_archive\\R40_color_grading";
constexpr const char* kRetGpu = "R26-R29 closed GPU probes, research_archive\\R26_R29_closed_gpu_probes";
constexpr const char* kRetOvl = "R74b cockpit mask overlay (value 128, producer retired in R107; 128 stays reserved)";
const RetiredKey kRetiredKeys[] = {
    {L"D3D", L"FocusBroadcast", kRetFbc, kSwitch},
    {L"D3D", L"FocusBroadcastArchive", kRetFbc, kTuning},
    {L"D3D", L"FocusBroadcastKinds", kRetFbc, kTuning},
    {L"D3D", L"FocusBroadcastRelearnFrames", kRetFbc, kTuning},
    {L"D3D", L"FocusBroadcastStartDelay", kRetFbc, kTuning},
    {L"D3D", L"FocusBroadcastTestsPerFrame", kRetFbc, kTuning},
    {L"Suite", L"FocusGeomProbe", kRetFbc, kSwitch},
    {L"Suite", L"FocusBcastProbe", kRetFbc, kSwitch},
    {L"Suite", L"FocusBcastTiming", kRetFbc, kSwitch},
    {L"Suite", L"FocusBcastVerify", kRetFbc, kSwitch},
    {L"Suite", L"FocusVsCensus", kRetFbc, kSwitch},
    {L"Suite", L"FocusVsArchive", kRetFbc, kTuning},
    {L"D3D", L"PeriphInstFilter", kRetPif, kSwitch},
    {L"D3D", L"PeriphInstCoreMargin", kRetPif, kTuning},
    {L"D3D", L"PeriphInstCoreEdge", kRetPif, kTuning},
    {L"D3D", L"PeriphInstCoreShape", kRetPif, kTuning},
    {L"Suite", L"PeriphCensus", kRetPif, kSwitch},
    {L"Suite", L"PeriphInstTiming", kRetPif, kSwitch},
    {L"Suite", L"PeriphInstVerify", kRetPif, kSwitch},
    {L"Color", L"SrgbMatch", kRetR40, kSwitch},
    {L"Color", L"BlackFloor", kRetR40, kSwitch},
    {L"Color", L"Contrast", kRetR40, kSwitch},
    {L"Color", L"Saturation", kRetR40, kSwitch},
    {L"Color", L"OutputGamma", kRetR40, kSwitch},
    {L"Suite", L"GpuSplit", kRetGpu, kSwitch},
    {L"Suite", L"GpuSplitFrames", kRetGpu, kTuning},
    {L"Suite", L"GapAttrib", kRetGpu, kSwitch},
    {L"Suite", L"GapAttribCalEvery", kRetGpu, kTuning},
    {L"Suite", L"GapAttribDump", kRetGpu, kSwitch},
    {L"Suite", L"LodProbe", kRetGpu, kSwitch},
    {L"Suite", L"GpuDedup", kRetGpu, kSwitch},
    {L"Suite", L"GpuDedupWarm", kRetGpu, kTuning},
    {L"Suite", L"GpuDedupHashCap", kRetGpu, kTuning},
    {L"Suite", L"GpuDedupSnap", kRetGpu, kSwitch},
    {L"Suite", L"UavOverlap", kRetGpu, kSwitch},
    {L"Perception", L"CockpitMaskOverlay", kRetOvl, kSwitch},
};
// The retired switch keys of `ini` with a non-zero value not logged before (same key and value) in this session:
// logged now. Returns how many.
int NoteRetiredKeys(const std::wstring& ini) {
  static std::set<std::wstring> logged;
  int n = 0;
  for (const RetiredKey& r : kRetiredKeys) {
    if (!r.sw) continue;  // R115 I3: tuning keys are never logged
    wchar_t v[64] = {};
    GetPrivateProfileStringW(r.sec, r.key, L"", v, 64, ini.c_str());
    if (!v[0] || (wcstol(v, nullptr, 0) == 0 && wcstod(v, nullptr) == 0.0)) continue;
    if (!logged.insert(std::wstring(r.sec) + L"]" + r.key + L"=" + v).second) continue;
    Log("config: [%ls] %ls=%ls is a retired key (%s); ignored", r.sec, r.key, v, r.what);
    ++n;
  }
  return n;
}

