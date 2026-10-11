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
        NrRoundness={(p.UsesNeuralEdgeFade && p.QuadRoundFocus ? 1 : 0)}
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

    /// <summary>DcsQvCull.ini for the DCS engine optimizations. Every key the module reads is written: a missing key
    /// falls back to the module's built-in default, and several of those turn on experiments that measured no gain or a
    /// loss (peripheral exclusion, the D3D11 state meter, extra suite phases), which stay off here. The test suite
    /// (run_suite.flag or Ctrl+Alt+F11) compares the two optimizations off and on.</summary>
    public static string DcsQvCull(VrProfile p) => $"""
        ; Written by DCS Control for the applied profile; edits here are replaced the next time it is applied.
        ; DcsQvCull re-reads this file every second while DCS runs.

        [General]
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

        [Log]
        StatsIntervalSec=2
        DumpViews=3

        [Bench]
        Blocks=24
        BlockSec=5
        SettleSec=1.0
        AutoStartSec=0
        ThreadsB=12

        [Suite]
        SelfTest=0
        Profile=0
        BenchCull=0
        BenchTimer=1
        BenchPartition=1
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
        JoinTailCount=0
        SrvSpanCount=0
        FxApplyCount=0
        GBufferTexCount=0
        Terrain=0
        BenchTexDedupe=0
        BenchCbSkip=0
        BenchTriPlain=0
        BenchEngine=0
        BenchMicro=0
        BenchPrefetch=0
        Quick=0
        MotionSeconds=60

        [Dev]
        ; Developer mode: a payload built elsewhere, hot-reloaded, and an ini that replaces this one. Empty: off.
        PayloadPath={(p.EngineDevMode ? p.EngineDevPayloadPath?.Trim() : null)}
        IniPath={(p.EngineDevMode ? p.EngineDevIniPath?.Trim() : null)}

        """;

    /// <summary>OFXR's ini. <paramref name="cursorTemplates"/> is the folder holding DCS's Visualizer.dll (DCS.exe's own),
    /// where the smooth cursor finds the cursor images if the running game has not loaded them.</summary>
    public static string Ofxr(VrProfile p, string? cursorTemplates = null) => $"""
        [ofxr]
        enabled={(p.FrameGen == FrameGeneration.Off ? 0 : 1)}
        motion_vectors=off
        deep_pipeline={(p.FrameGenDeepPipeline ? 1 : 0)}
        triple_frame_gen={(p.FrameGenFactor == 3 ? 1 : 0)}
        adaptive_frame_gen={(p.FrameGenFactor == VrProfile.FrameGenAuto ? 1 : 0)}
        d3d11_bridge=1
        backend={(p.FrameGen == FrameGeneration.FidelityFx ? "fidelityfx" : "nvidia")}
        nvidia_preset={p.FlowPreset.ToString().ToLowerInvariant()}
        nvidia_input_scale={p.NvidiaFlowScale}
        nvidia_bidirectional={(p.BidirectionalFlow ? 1 : 0)}
        diag_vram={(p.DiagnosticVram ? 1 : 0)}
        smooth_cursor={(p.SmoothCursor ? 1 : 0)}
        cursor_templates={cursorTemplates ?? ""}

        [diagnostics]
        logging_enabled={(p.DiagnosticRecorder ? 1 : 0)}
        max_file_mb=256
        flush_each_event=0

        [overlay]
        position={(p.ShowOverlay ? "upper_right" : "off")}

        """;

    /// <summary>PupilShift.ini, next to the pupil shift layer, which re-reads it about once a second while DCS runs.</summary>
    public static string PupilShift(VrProfile p, double? ipdMm = null) => $"""
        ; DCS Control pupil shift layer. Changes apply live while DCS runs.
        [PupilShift]
        Enabled=1
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
        SimulateGazeYDeg=0

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
