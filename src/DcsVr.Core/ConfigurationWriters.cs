using System.Globalization;
using System.Text.Json.Nodes;

namespace DcsVr.Core;

public static class ConfigurationWriters
{
    private static string Number(double value) => value.ToString("G9", CultureInfo.InvariantCulture);
    // Cheeky clamps these ratios to 0.20..1 and rewrites its INI with the clamped value, which restore would then
    // report as a user edit. Write the value Cheeky will actually use (src/settings.cpp).
    private static double CheekyRange(double value) => Math.Clamp(value, 0.2, 1);
    // Central DLSS 5 area of each focus view (fraction per axis). Cheeky centres it in the focus view, which Quad Views
    // already centres on the gaze, keeps the game's own DX11 DLSS and feathers the NR edge over NrTransitionWidth.
    private static double NeuralArea(VrProfile p) => p.UsesCentralNeuralArea ? p.NeuralFocusArea / 100.0 : 1;

    public static string Cheeky(VrProfile p) => $"""
        [CheekyFoveatedDLSS]
        SchemaVersion=1
        Enabled={(p.UsesFoveatedDlss ? 1 : 0)}
        D3D11D3D12Transport={(p.NeuralRendering ? 1 : 0)}
        D3D12LowerHook=1
        PeripheralDlaa={(p.UsesQuadFocus ? 0 : 1)}
        PeripheralDlaaScale={Number(CheekyRange(p.PeripheralScale))}
        Width={Number(p.UsesQuadFocus ? 1 : CheekyRange(p.FoveaWidth))}
        Height={Number(p.UsesQuadFocus ? 1 : CheekyRange(p.FoveaHeight))}
        CenterMode={(p.Gaze == GazeMode.EyeTracked && !p.UsesQuadFocus ? 1 : 0)}
        AutoStereoAlignment={(p.UsesQuadFocus ? 0 : 1)}
        EyeCalibrationContinuous=0
        GazeSmoothingMs=20
        GazeHoldMs=400
        TransitionWidth=0.04
        NrEnabled={(p.NeuralRendering ? 1 : 0)}
        NrFoveated={(p.UsesQuadFocus ? (p.UsesCentralNeuralArea || p.UsesNeuralEdgeFade ? 1 : 0) : 1)}
        NrUseSrFoveation={(p.UsesQuadFocus ? 0 : 1)}
        NrWidth={Number(p.UsesQuadFocus ? NeuralArea(p) : CheekyRange(p.FoveaWidth))}
        NrHeight={Number(p.UsesQuadFocus ? NeuralArea(p) : CheekyRange(p.FoveaHeight))}
        NrRoundness={((p.UsesNeuralEdgeFade || p.UsesCentralNeuralArea) && p.QuadRoundFocus ? 1 : 0)}
        CenterPreset={(p.UsesQuadFocus ? p.FocusDlssPreset : 0)}
        NrTransitionWidth={Number(p.UsesNeuralEdgeFade ? p.NeuralEdgeFeather : VrProfile.NeuralAreaFeather)}
        NrProcessingOrder={(p.NeuralBeforeUpscaling ? 1 : 0)}
        NrWorkingScale={Number(p.NeuralWorkingScale)}
        NrIntensity={Number(p.NeuralIntensity)}
        NrLocalToneStrength={Number(p.NeuralLocalTone)}
        NrLocalStructureStrength={Number(p.NeuralLocalStructure)}
        NrSkinStructureStrength={Number(p.NeuralSkinStructure)}
        NrUiCorrection={(p.NeuralUiCorrection ? 1 : 0)}
        NrColorStrength={Number(p.NeuralColorStrength)}
        NrHdrTransferStrength={Number(p.NeuralTransferStrength)}
        NrPaperWhiteScale={Number(p.NeuralPaperWhiteScale)}
        NrDepthConvention={(int)p.NeuralDepth}
        NrMotionScaleXMultiplier={Number(p.NeuralMotionScaleX)}
        NrMotionScaleYMultiplier={Number(p.NeuralMotionScaleY)}
        NrStyle={(int)p.NeuralStyle}
        NrAutomaticMask={(p.NeuralAutomaticMask ? 1 : 0)}

        """;

    /// <summary>[PupilShift] PublishGaze, written only while Visual realism needs the gaze (eye-focus blur, G vision
    /// following the eyes): with it off the layer's file stays as it was.</summary>
    private static string PublishGazeLines(VrProfile p) => p.UsesGazePublish ? """


        ; The gaze for the engine module's eye-focus blur and G vision, also without Depth boost.
        PublishGaze=1
        """ : "";

    /// <summary>[Color] Mode, written only while a color mode can run (a mode or its key is set): with everything off the
    /// file stays as it was before Visual realism existed.</summary>
    private static string ColorModeLine(VrProfile p) => p.UsesColorMode ? $"""

        Mode={(int)p.ColorMode}
        """ : "";

    /// <summary>Visual realism's sections after [Color]: each color mode's values while a color mode can run, [VrFx] while
    /// Gaze DoF or G vision runs. Every GUI key is written; the tone curve's tuning keys and the developer probes are not
    /// (the module's defaults).</summary>
    private static string VisualSections(VrProfile p) => (p.UsesColorMode ? $"""


        ; Color modes: Fix and Enhanced each have their own values; Night vision and glare are Enhanced only.
        [ColorFix]
        ExposureEV={Number(p.ColorFixExposureEv)}
        NightEV={Number(p.ColorFixNightEv)}
        Saturation={Number(p.ColorFixSaturation)}
        Warmth={Number(p.ColorFixWarmth)}
        AdaptBright={Number(p.ColorFixAdaptBright)}
        AdaptDark={Number(p.ColorFixAdaptDark)}

        [ColorEnhanced]
        ExposureEV={Number(p.ColorEnhancedExposureEv)}
        NightEV={Number(p.ColorEnhancedNightEv)}
        Saturation={Number(p.ColorEnhancedSaturation)}
        Warmth={Number(p.ColorEnhancedWarmth)}
        AdaptBright={Number(p.ColorEnhancedAdaptBright)}
        AdaptDark={Number(p.ColorEnhancedAdaptDark)}
        NightVision={(p.NightVision ? 1 : 0)}
        NightVisionStrength={Number(p.NightVisionStrength)}
        NightVisionBlue={Number(p.NightVisionBlue)}
        NightVisionBlur={Number(p.NightVisionBlur)}
        NightVisionScotopic={Number(p.NightVisionScotopic)}
        NightVisionPhotopic={Number(p.NightVisionPhotopic)}
        Glare={(p.Glare ? 1 : 0)}
        GlareStrength={Number(p.GlareStrength)}
        GlareThresholdEV={Number(p.GlareThresholdEv)}
        GlareRadius={Number(p.GlareRadius)}
        GlareStreak={Number(p.GlareStreak)}
        """ : "") + (p.UsesGazeDof || p.UsesGVision ? $"""


        ; Gaze DoF (needs the gaze, from the pupil shift layer) and G vision (G from DCS's export, through the module's Lua hook).
        [VrFx]
        GazeDof={(p.UsesGazeDof ? 1 : 0)}
        GazeDofStrength={Number(p.GazeDofStrength)}
        GazeDofMaxArcmin={Number(p.GazeDofMaxArcmin)}
        GazeDofSaccadeRelease={(p.GazeDofSaccadeRelease ? 1 : 0)}
        GazeDofSettleMs={p.GazeDofSettleMs.ToString(CultureInfo.InvariantCulture)}
        GazeDofFadeInMs={p.GazeDofFadeInMs.ToString(CultureInfo.InvariantCulture)}
        GazeDofToleranceD={Number(p.GazeDofToleranceD)}
        GazeDofFoveaGuardDeg={Number(p.GazeDofFoveaGuardDeg)}
        GazeDofPupilMm={Number(p.GazeDofPupilMm)}
        GVision={(p.UsesGVision ? 1 : 0)}
        GVisionStrength={Number(p.GVisionStrength)}
        GVisionUseGaze={(p.GVisionUseGaze ? 1 : 0)}
        GVisionRedout={(p.GVisionRedout ? 1 : 0)}
        GVisionTolerance={Number(p.GVisionTolerance)}
        GVisionMode={(int)p.GVisionMode}
        """ : "");

    /// <summary>DcsQvCull.ini for the DCS engine optimizations. Every key whose built-in default is not wanted is written: a missing key
    /// falls back to the module's built-in default, and several of those turn on experiments that measured no gain or a
    /// loss (peripheral exclusion, the D3D11 state meter, extra suite phases), which stay off here. Keys left out (developer probes) default to off. The test suite
    /// (run_suite.flag or Ctrl+Alt+F11) compares the two optimizations off and on.</summary>
    public static string DcsQvCull(VrProfile p) => $"""
        ; Written by DCS Control for the applied profile; edits here are replaced the next time it is applied.
        ; DcsQvCull re-reads this file every second while DCS runs.

        [General]
        ; Installed for Depth boost alone (Engine Optimizations off): only the perception features run, none of the
        ; optimizations, the test suite or the engine keys.
        PerceptionOnly={(p.EngineOptimizations ? 0 : 1)}
        Diagnostics={(p.EngineDiagnosticHooks ? 1 : 0)}
        Beeps={(p.EngineBeeps ? 1 : 0)}
        LowPowerPacer=0
        LowPowerPacerTimeoutUs=10
        SigScan=1

        [Timing]
        ShaderTimeCache={(p.EngineShaderTimeCache ? 1 : 0)}
        TaskQueueClock={(p.EngineTaskQueueClock && p.EngineShaderTimeCache ? 1 : 0)}
        CacheUs={Math.Clamp(p.EngineTimerRefreshUs, VrProfile.EngineTimerRefreshMin, VrProfile.EngineTimerRefreshMax)}

        [Hotkeys]
        ; Switches the optimizations above off and back on in flight ("virtual-key:modifiers", Ctrl 1, Alt 2, Shift 4; 0:0 off).
        Toggle={NeuralHotkeys.Environment(p.EngineToggleKey, NeuralHotkeys.EngineDefault)}
        ; The module's developer keys (peripheral exclusion, DebugHole, focus fraction) are off.
        DeveloperKeys=0

        [Scene]
        PartitionBoost={(p.EnginePartitionBoost ? 1 : 0)}
        CostWeights={(p.EngineCostWeights ? 1 : 0)}
        CostWeightsSanity=0
        CollectThreadsMax=0
        FineTimerResolution=0

        [Cull]
        Enabled=0
        DebugHole=0
        FocusKeepFraction=-1
        SafetyNdc=0.05
        NativeKeepFraction=0.35
        SaccadeDeg=1.0
        SaccadeHoldFrames=6
        FocusRatio=0.80
        ApexTolerance=0.02

        [D3D]
        Meter=0
        Filter=0
        SplitFilter={(p.EngineStateFilter ? 1 : 0)}
        SplitFilterOps=0x4ff
        ChainFast=7
        ShaderFast=3
        PeriphInstFilter=0
        FocusBroadcast=0

        [Shadows]
        CascadeCache={(p.EngineShadowCache ? 1 : 0)}

        [Shadow]
        TightCasters=0
        DebugInvert=0

        [Model]
        AllocSlabs={(p.EngineModelAllocator ? 1 : 0)}
        SlabBytes=4096
        PlainTriangleCounter={(p.EnginePlainCounter ? 1 : 0)}
        FrameHeapSlabs={(p.EngineFrameHeap ? 1 : 0)}
        BigModelPages={(p.EngineShadowInstancing ? 1 : 0)}
        BigPageBytes=4194304
        ShadowInstancing={(p.EngineShadowInstancing ? 1 : 0)}
        ShadowBatching={(p.EngineShadowInstancing ? 1 : 0)}
        ShadowPlanAsync={(p.EngineShadowInstancing ? 1 : 0)}
        ShadowTextureSkip={(p.EngineShadowInstancing ? 1 : 0)}
        GBufferBatching=0
        ParallelUpload=0
        DirectUpload=0
        ShadowRecorder={(p.EngineShadowRecorder ? 1 : 0)}
        ShadowRecorderScope=0x30f
        ShadowRecorderWaitUs=200
        ShadowRecorderPriority=0
        ShadowRecorderSplit=0xf
        ShadowRecorderInstancing=1
        ; Helper threads per shadow cascade: 1 on CPUs with 8 logical cores or fewer, so DCS keeps its own threads.
        ShadowRecorderHelpers={(Environment.ProcessorCount <= 8 ? 1 : 3)}
        GBufferRecorder={(p.EngineGBufferRecorder ? 1 : 0)}
        GBufferRecorderScope=0x10055
        GBufferRecorderMaxSegments=12
        GBufferRecorderHelpers=2
        GBufferRecorderWaitUs=300
        GBufferRecorderIsland=30
        GBufferRecorderRedo=1
        GBufferRecorderSwapAhead=1
        GBufferRecorderCockpit=0
        GBufferRecorderByOrdinal=1
        SrvTailTrim=0
        PassFlush=0

        [Texture]
        StreamDedupe={(p.EngineTextureDedupe ? 1 : 0)}

        [Effects]
        SkipSameConstantBuffer={(p.EngineEffectBufferSkip ? 1 : 0)}
        SkipSameConstantUpload=0

        [Prefetch]
        ; Terrain prefetch fix: skip (repeats of a range within WindowMs are skipped; a prefetch is only a hint), observe
        ; (only counted) or off. Statistics every 10 s in DcsQvCullPrefetch.log next to this file.
        Mode={(p.UsesPrefetchFix ? p.BoostPrefetch.ToString().ToLowerInvariant() : "off")}
        WindowMs=5000
        TableBits=20

        [Threads]
        Census=0
        WorkerPlacement=2
        PlaceRecheckSec=60

        [Color]{ColorModeLine(p)}
        ModeKey={(p.UsesColorModeKey ? NeuralHotkeys.Environment(p.ColorModeKey, NeuralHotkeys.ColorModeDefault) : "0:0")}
        ToneProbe=0
        SwapProbe=0{VisualSections(p)}

        [Log]
        StatsIntervalSec=2
        DumpViews=3
        ; A perf line every 10 s (fps, frame times, CPU- or GPU-bound), VRAM every 60 s; RecorderDetail=1 logs the
        ; recorders' full 10-s blocks instead of one summary line.
        RecorderDetail=0
        PerfIntervalSec=10
        VramIntervalSec=60

        ; When the GPU is the limit, the recorders' extra GPU work is stepped down (and back up when the CPU is the
        ; limit again); each step is kept only when it raises fps. The image never changes.
        [Governor]
        Enabled=1
        GpuBusyPct=92
        RenderIdlePct=85
        CpuBusyPct=92
        GpuFreePct=80
        GpuFullPct=98
        WindowSec=8
        NeedSec=6
        MinDwellSec=10
        MinGainPct=2.0
        TrialSec=20
        RecheckSec=10
        RevertBackoffSec=300

        [Perception]
        ; Depth boost's scene depth: DCS's depth buffer sampled around the gaze and published for the pupil shift layer
        ; (docs/FIXATION_DEPTH.md). On only while the profile uses Depth boost.
        FixationDepth={(p.UsesFixationDepth ? 1 : 0)}
        ; The predictive transition's depth grid: unused with the dual camera (always gradual), so not captured.
        DepthGrid=0
        ; A camera cut (F1/F2/F10, respawn, mission load) resets the bundled Quad Views' periphery history.
        SceneCutReset=1
        FixationDepthVerify=0
        FixationDepthDump=0
        ; Far depth (docs/FAR_DEPTH.md): the engine's depth for the bundled Quad Views, on with the profile's Far depth.
        FarDepthExport={(p.UsesFarDepth ? 1 : 0)}
        FarDepthExportSlots=2
        ; The image before DCS draws its HUD and symbology, so the bundled Quad Views keeps them out of Far depth.
        FarDepthPreOverlay=1
        ; Cockpit mask for Framegen (R65): the cockpit stays still in generated frames. The module computes it only while
        ; the frame generation layer reads it (its heartbeat), so it costs nothing otherwise.
        CockpitMaskExport={(p.FrameGen != FrameGeneration.Off && !p.Desktop ? 1 : 0)}
        CockpitMaskViews=3
        CockpitMaskDivisor=0
        CockpitMaskNearM=1.0
        CockpitMaskDilate=0
        CockpitMaskDump=0
        CockpitMaskLog=0
        ; Cockpit depth and DCS's motion vectors for Framegen (R69/R70): computed only while the frame generation layer reads
        ; them (its heartbeats), so they cost nothing otherwise.
        CockpitDepthExport={(p.FrameGen != FrameGeneration.Off && !p.Desktop ? 1 : 0)}
        CockpitDepthDump=0
        CockpitMotionExport={(p.FrameGen != FrameGeneration.Off && !p.Desktop ? 1 : 0)}
        CockpitMotionClass=1
        CockpitMotionDump=0
        CockpitFgDump=0
        ; HUD symbology in the cockpit mask (value 128, R74): not yet verified on lit symbols, off.
        CockpitMaskOverlay=0
        ; The HUD and helmet symbols as their own layer for frame generation (R95); needs the dual camera.
        SymbologyExport={(p.UsesHudSymbology ? 1 : 0)}
        SymbologyAlpha=1
        ; The HUD found as the change between copies before and after it in the HDR image (aircraft such as the F-16
        ; draw it after DLSS, before the tonemap); the module falls back by itself when that image is not found.
        FarDepthCoverageMode=hdr
        FarDepthLogProj=0
        ; Depth boost's dual camera: the cockpit, its lighting and its displays at the true eye distance while the world
        ; is rendered boosted; the HUD stays at DCS's camera. Its in-flight pause key is a developer key (off).
        CockpitTrueIpd={(p.UsesDepthBoost ? 1 : 0)}
        CockpitTrueIpdLighting=1
        CockpitTrueIpdForward=1
        CockpitTrueIpdHelmet=1
        CockpitTrueIpdOverlays=1
        CockpitTrueIpdOverlayPath=cpu
        CockpitTrueIpdOwnship={(p.UsesDepthBoost && p.OwnAircraftTrueDepth ? 2 : 0)}
        CockpitTrueIpdToggle=0:0

        [Bench]
        Blocks=24
        BlockSec=5
        SettleSec=1.0
        AutoStartSec=0
        ThreadsB=12

        ; Ctrl+Alt+F11 in the cockpit: about 3 minutes of A/B, all optimizations OFF (stock DCS) against ON, written to
        ; report_*.txt and bench_*.csv next to this file. Nothing runs until the key is pressed.
        [Suite]
        SelfTest=0
        Profile=0
        BenchCull=0
        BenchTimer=0
        BenchPartition=0
        BenchIsolation=0
        BenchThreads=0
        BenchTimerRes=0
        BenchFilter=0
        BenchShadow=0
        BenchAllocSlabs=0
        BenchCostWeights=0
        BenchCbUpload=0
        BenchPacer=0
        MotionSweep=0
        BenchFrameHeap=0
        MotionProfile=0
        MotionTaxi=0
        MotionCounters=1
        ShadowInstCompile=0
        ShadowInstVerify=0
        BenchShadowInst=0
        BenchBigPages=0
        BenchShadowTex=0
        BenchTexTable=0
        YawScan=0
        HoldYawDeg=-1
        GBufferInstCompile=0
        GBufferInstVerify=0
        BenchGBufferInst=0
        BenchParallelUpload=0
        BenchShadowPlanAsync=0
        DirectUploadCount=0
        DirectUploadVerify=0
        BenchDirectUpload=0
        SplitFilterVerify=0
        ShadowRecVerify=0
        ShadowRecVerifySec=5
        BenchShadowRecorder=0
        ShadowRecCount=0
        GBufferRecVerify=0
        GBufferRecVerifySec=6
        GBufferRecVerifyStride=0
        BenchGBufferRecorder=0
        GBufferRecCount=0
        GBufferRecStateDump=0
        GpuPassTiming=0
        YawProfile=0
        YawProfileStep=30
        RotationProfile=0
        RotationDegPerSec=60
        RotationSeconds=20
        FrameStartGap=0
        FrameStartGapSec=10
        RunnableThreads=0
        RunnableThreadsSec=10
        RunnableThreadsEvery=16
        VramCount=0
        VramCountSec=20
        VramCountCreates=1
        GpuPassStats=0
        ForwardRecCount=0
        BenchPassFlush=0
        BenchPassFlushMask=0
        SrvTailTrimVerify=0
        SrvTailTrimVerifySec=10
        BenchSrvTailTrim=0
        BenchSplitFilter=0
        CascadeCacheTiming=0
        ChainFastTiming=0
        ChainFastVerify=0
        ShaderFastTiming=0
        ShaderFastVerify=0
        GeoOnlyProbe=0
        PeriphCensus=0
        PeriphInstTiming=0
        PeriphInstVerify=0
        FocusGeomProbe=0
        FocusVsCensus=0
        FocusBcastProbe=0
        FocusBcastTiming=0
        FocusBcastVerify=0
        JoinTailCount=0
        SrvSpanCount=0
        FxApplyCount=0
        GBufferTexCount=0
        Terrain=0
        BenchTexDedupe=0
        BenchCbSkip=0
        BenchTriPlain=0
        BenchEngine=1
        BenchMicro=0
        BenchPrefetch=0
        Quick=1
        MotionSeconds=60

        [Dev]
        ; Developer mode: a payload built elsewhere, hot-reloaded, and an ini that replaces this one. Empty: off.
        PayloadPath={(p.EngineOptimizations && p.EngineDevMode ? p.EngineDevPayloadPath?.Trim() : null)}
        IniPath={(p.EngineOptimizations && p.EngineDevMode ? p.EngineDevIniPath?.Trim() : null)}

        """;

    /// <summary>OFXR's ini. <paramref name="cursorTemplates"/> is the folder holding DCS's Visualizer.dll (DCS.exe's own),
    /// where the smooth cursor finds the cursor images if the running game has not loaded them.</summary>
    public static string Ofxr(VrProfile p, string? cursorTemplates = null) => $"""
        [ofxr]
        enabled={(p.FrameGen == FrameGeneration.Off ? 0 : 1)}
        motion_vectors=off
        deep_pipeline={(p.FrameGenDeepPipeline ? 1 : 0)}
        pipelined_deep_hold={(p.FrameGenDeepPipeline && p.FrameGenPipelinedDeepHold ? 1 : 0)}
        async_arm={(p.FrameGenAsyncArm ? 1 : 0)}
        triple_frame_gen={(p.FrameGenFactor == 3 ? 1 : 0)}
        adaptive_frame_gen={(p.FrameGenFactor == VrProfile.FrameGenAuto ? 1 : 0)}
        ; Per-frame 2X/3X and a full-rate 1X (patch 0032): off until it has flown in DCS.
        adaptive_cadence=0
        ; Cheaper composition of still tiles (patch 0033, bit-identical output): off until measured in DCS.
        fast_composition=0
        ; Synthetic frames placed by DCS's frame-start clock (patch 0037): off until an A/B in DCS shows it helps.
        content_time_fraction=0
        ; Both eyes synthesized on their own queues, about 1 ms sooner with identical output (patch 0038): off until flown.
        parallel_eye_synthesis=0
        ; Synthetic frames placed by DCS's own pose clock (patch 0039): smoother on paper, but fixed halves match the real
        ; frames better; off until a headset A/B decides.
        slot_content_time=0
        d3d11_bridge={(p.FrameGen == FrameGeneration.Off ? 0 : 1)}
        backend={(p.FrameGen == FrameGeneration.FidelityFx ? "fidelityfx" : "nvidia")}
        nvidia_preset={p.FlowPreset.ToString().ToLowerInvariant()}
        nvidia_input_scale={p.NvidiaFlowScale}
        nvidia_bidirectional={(p.BidirectionalFlow ? 1 : 0)}
        ; Moving edges: the flow endpoint solved per pixel (level 1). Measured better with the Medium preset and one-way
        ; flow, the defaults; other combinations keep the plain sample (0).
        nvidia_occlusion_aware={(p.FlowPreset == NvidiaFlowPreset.Medium && !p.BidirectionalFlow ? 1 : 0)}
        ; The engine module's cockpit mask keeps the cockpit still in generated frames; without it (Engine Optimizations off)
        ; nothing changes and nothing is spent.
        cockpit_mask=1
        ; The engine module's HUD symbols: taken out before frame generation and put back on the generated frames.
        hud_symbology={(p.UsesHudSymbology ? 1 : 0)}
        ; With the engine module's cockpit depth too, the cockpit is reprojected exactly when the head moves (no block: no cost).
        cockpit_depth=1
        ; DCS's own motion vectors for terrain, aircraft, water and sky, checked against the colours (NVIDIA path only).
        dcs_motion={(p.FrameGen == FrameGeneration.Nvidia ? 1 : 0)}
        diag_vram={(p.DiagnosticVram ? 1 : 0)}
        ; Panel only (no frame generation): DCS's instance gets no extension it did not ask for.
        visibility_mask={(p.FrameGen == FrameGeneration.Off ? 0 : 1)}
        smooth_cursor={(p.SmoothCursor ? 1 : 0)}
        cursor_templates={cursorTemplates ?? ""}

        [diagnostics]
        logging_enabled={(p.DiagnosticRecorder && p.FrameGen != FrameGeneration.Off ? 1 : 0)}
        max_file_mb={(p.DiagnosticFrameRecords ? 1024 : 256)}
        flush_each_event=0{(p.DiagnosticFrameRecords ? "\nframe_records=1" : "")}

        [overlay]
        position={(p.ShowOverlay && p.FrameGen != FrameGeneration.Off ? "upper_right" : "off")}

        """;

    /// <summary>PupilShift.ini, next to the pupil shift layer, which re-reads it about once a second while DCS runs. The
    /// layer carries Pupil shift ([PupilShift] Enabled) and Depth boost ([DepthBoost] Enabled), each on its own.</summary>
    public static string PupilShift(VrProfile p, double? ipdMm = null) => $"""
        ; DCS Control pupil shift layer (Pupil shift and Depth boost). Changes apply live while DCS runs.
        ; [PupilShift] Enabled is the pupil correction; with 0 the layer still runs for Depth boost.
        [PupilShift]
        Enabled={(p.UsesPupilShift ? 1 : 0)}
        ; In-flight switch, virtual-key:modifiers (1 Ctrl, 2 Alt, 4 Shift); 0:0 off.
        Toggle={NeuralHotkeys.Environment(p.PupilShiftToggleKey, NeuralHotkeys.PupilDefault)}
        ; Pupil to eye rotation centre, millimetres.
        EyeRadiusMm={Number(p.PupilShiftEyeRadiusMm)}
        ; Lens virtual image distance, metres.
        VirtualImageM={Number(p.PupilShiftVirtualImageM)}
        ; Gaze beyond this angle is treated as this angle.
        MaxGazeDeg=35
        ; Eye distance in millimetres, 0 = the headset's own: DCS's custom IPD, which the layer applies itself (DCS would
        ; otherwise replace the eyes' sideways position after the layer and undo the pupil's sideways shift).
        Ipd={Number(ipdMm ?? 0)}
        ; Testing only: a fixed gaze instead of the eye tracker.
        SimulateGaze=0
        SimulateGazeXDeg=0
        SimulateGazeYDeg=0{PublishGazeLines(p)}

        ; Depth boost: the eye distance above (Ipd, or the headset's) times a scale that rises while the eyes fixate far
        ; and returns to 1 when they fixate nearer than NearM (the cockpit). docs/PERCEPTION.md, docs/FIXATION_DEPTH.md.
        [DepthBoost]
        Enabled={(p.UsesDepthBoost ? 1 : 0)}
        ; In-flight switch, virtual-key:modifiers (1 Ctrl, 2 Alt, 4 Shift); 0:0 off.
        Toggle={NeuralHotkeys.Environment(p.DepthBoostToggleKey, NeuralHotkeys.DepthBoostDefault)}
        ; realistic: scale 1 up to FarStartM, MaxScale from FarFullM (log ramp between); enhanced: the same from 3 m to 50 m.
        Mode={(p.DepthBoostMode == DepthBoostMode.Enhanced ? "enhanced" : "realistic")}
        MaxScale={Number(p.DepthBoostActiveMax)}
        FarStartM={Number(p.DepthBoostMode == DepthBoostMode.Enhanced ? VrProfile.DepthBoostEnhancedStartM : p.DepthBoostFarStartM)}
        FarFullM={Number(p.DepthBoostMode == DepthBoostMode.Enhanced ? VrProfile.DepthBoostEnhancedFullM : p.DepthBoostFarFullM)}
        NearM={Number(p.DepthBoostNearM)}
        ; Rates in millimetres of eye distance per second: towards MaxScale (below the disparity-change threshold for
        ; the cockpit at 0.5 m), back towards 1, and the extra change allowed during a saccade.
        UpMmPerS=5
        DownMmPerS=40
        SaccadeStepMm=4
        ; MaxScale is capped so the return to 1 takes at most this long.
        RecoverMs=800
        ; Without a fresh gaze (a blink) the scale holds this long, then returns to 1.
        HoldMs=500
        ; gradual: the rates above, confirmed after the eyes land. predictive: during a saccade the landing is predicted
        ; from Pimax PVR's gaze (sampled at PredictPollHz) and the engine's depth grid, and one step of at most
        ; PredictMaxStepMm (and PredictDisparityDeg of disparity at the landing) is made in flight; the rest follows the
        ; rates. Saccades predicted shorter than PredictMinDeg, or with less than PredictLeadFrames frames left, use gradual.
        ; With the dual camera the scale only moves smoothly: the transition is gradual and has no in-flight switch.
        Transition=gradual
        TransitionToggle=0:0
        PredictMinDeg=8
        PredictMaxStepMm=12
        PredictDisparityDeg=0.4
        PredictLeadFrames=0.5
        PredictPollHz=500

        """;

    public static string LayerManifest(string name, string library, int implementationVersion, string description) => JsonData.Serialize(new
    {
        file_format_version = "1.0.0",
        api_layer = new
        {
            name, library_path = Path.GetFullPath(library), api_version = "1.0",
            implementation_version = implementationVersion.ToString(CultureInfo.InvariantCulture), description
        }
    });

    public static string QuadViews(VrProfile p) => $"""
        [exe:DCS]
        peripheral_multiplier={Number(p.PeripheralScale)}
        focus_multiplier={Number(p.QuadFocusScale)}
        stereo_output_multiplier=1
        sharpen_focus_view={Number(p.QuadSharpening)}
        smoothen_focus_view_edges={Number(p.QuadEdgeBlend)}
        focus_view_shape={(p.QuadRoundFocus ? 2 : 8)}
        dcsvr_sharpen_taper={(p.QuadSharpenTaper ? 1 : 0)}
        dcsvr_periphery_sharpen={Number(p.QuadPeripheryContrast)}
        horizontal_fixed_section={Number(Math.Min(p.FoveaWidth, .9))}
        vertical_fixed_section={Number(Math.Min(p.FoveaHeight, .9))}
        horizontal_focus_section={Number(Math.Min(p.FoveaWidth, .9))}
        vertical_focus_section={Number(Math.Min(p.FoveaHeight, .9))}
        force_no_eye_tracking={(p.Gaze == GazeMode.Fixed ? 1 : 0)}
        turbo_mode={(p.QuadTurbo ? 1 : 0)}
        dcsvr_saccade_widening={(p.QuadSaccadeLead && p.Gaze == GazeMode.EyeTracked ? 1 : 0)}
        dcsvr_saccade_speed=120
        dcsvr_saccade_lead_ms={p.QuadSaccadeLeadMs}
        dcsvr_saccade_max_extend=0.35
        dcsvr_saccade_hold_ms=40
        dcsvr_gaze_deadzone={(p.QuadGazeStabilize && p.Gaze == GazeMode.EyeTracked ? "0.5" : "0")}
        dcsvr_temporal={(p.UsesPeripheryStability ? 1 : 0)}
        dcsvr_temporal_tau_ms={Number(Math.Round(VrProfile.PeripheryStabilityMaxTauMs * Math.Clamp(p.PeripheryStabilityStrength, 0, 1), 1))}
        dcsvr_temporal_ramp_deg=8
        dcsvr_temporal_clip=1
        dcsvr_temporal_debug=0
        dcsvr_far_depth={(p.UsesFarDepth ? 1 : 0)}
        dcsvr_far_depth_preset={p.FarDepthPreset.ToString().ToLowerInvariant()}
        dcsvr_far_depth_toggle={(p.UsesFarDepth ? NeuralHotkeys.Environment(p.FarDepthToggleKey, NeuralHotkeys.FarDepthDefault) : "0:0")}

        """;

    public static string ExplicitManifest(string existing, string manifestPath)
    {
        var root = JsonNode.Parse(existing)?.AsObject() ?? throw new InvalidDataException("Invalid OpenXR manifest.");
        var layer = root["api_layer"]?.AsObject() ?? throw new InvalidDataException("Manifest is missing api_layer.");
        var library = layer["library_path"]?.GetValue<string>() ?? throw new InvalidDataException("Manifest is missing library_path.");
        var absoluteLibrary = Path.GetFullPath(library, Path.GetDirectoryName(Path.GetFullPath(manifestPath))!);
        if (!File.Exists(absoluteLibrary)) throw new InvalidDataException("The layer library does not exist.");
        layer["library_path"] = absoluteLibrary;
        layer.Remove("enable_environment"); layer.Remove("disable_environment");
        return root.ToJsonString(JsonData.Options);
    }
}
