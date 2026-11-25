// Helper types of the tests in Program.cs (fakes, fixtures and frozen expected text): no test lives here.
using System.Diagnostics;
using System.IO.Compression;
using System.Text;
using DcsVr.Core;

/// <summary>One end-to-end fixture: the service on its own distribution and state, and the fake PC's DCS and runtime files.</summary>
sealed record E2eFixture(ControlService Service, string Base, string Exe, string Options, string Pimax, string SteamVr);

/// <summary>A primary 3840×2160 144 Hz display for the monitor mode tests; records every call.</summary>
sealed class FakeDisplays : IDisplayModes
{
    public SavedDisplayMode Mode = new(3840, 2160, 144, 32, 0, 0);
    public bool Refuse, RefuseExact;
    public int Sets, Resets;
    private readonly SavedDisplayMode _saved = new(3840, 2160, 144, 32, 0, 0);
    public string? PrimaryDevice() => @"\\.\DISPLAY1";
    public string Name(string device) => "Fixture monitor (" + device + ")";
    public SavedDisplayMode? Current(string device) => device == @"\\.\DISPLAY1" ? Mode : null;
    public IReadOnlyList<DisplayMode> Modes(string device) => [new(3840, 2160, 144), new(3840, 2160, 60), new(2560, 1440, 120), new(1920, 1080, 144), new(1920, 1080, 60), new(1280, 720, 60)];
    public bool Set(string device, SavedDisplayMode mode)
    {
        if (Refuse || RefuseExact && mode == _saved) return false;
        Sets++; Mode = mode; return true;
    }
    public bool Reset(string device) { if (Refuse) return false; Resets++; Mode = _saved; return true; }
}

sealed class FixtureHttpHandler(byte[] bytes) : HttpMessageHandler
{
    protected override Task<HttpResponseMessage> SendAsync(HttpRequestMessage request, CancellationToken cancellationToken)
        => Task.FromResult(new HttpResponseMessage(System.Net.HttpStatusCode.OK) { Content = new ByteArrayContent(bytes) });
}

/// <summary>Fixed answers by URL for the update tests; records each request and its User-Agent. Offline throws like a
/// PC without network.</summary>
sealed class UrlFixtureHandler(Dictionary<string, byte[]> answers) : HttpMessageHandler
{
    public Dictionary<string, byte[]> Answers { get; } = answers;
    public List<string> Requests { get; } = [];
    public List<string> UserAgents { get; } = [];
    public bool Offline;
    protected override Task<HttpResponseMessage> SendAsync(HttpRequestMessage request, CancellationToken cancellationToken)
    {
        if (Offline) throw new HttpRequestException("No network (test fixture).");
        var url = request.RequestUri!.AbsoluteUri; Requests.Add(url); UserAgents.Add(request.Headers.UserAgent.ToString());
        return Task.FromResult(Answers.TryGetValue(url, out var bytes) ? new HttpResponseMessage(System.Net.HttpStatusCode.OK) { Content = new ByteArrayContent(bytes) } : new HttpResponseMessage(System.Net.HttpStatusCode.NotFound));
    }
}

/// <summary>DcsQvCull.ini as DCS Control wrote it before the constant-key trim (2026-10-21; R107 F07): a frozen copy of
/// ConfigurationWriters.DcsQvCull, ColorModeLine and VisualSections, kept only so the trim test can prove the module reads
/// the same values from the trimmed file. Never update it.</summary>
static class OldDcsQvCullTemplate
{
    private static string Number(double value) => value.ToString("G9", System.Globalization.CultureInfo.InvariantCulture);
    private static string Int(int value) => value.ToString(System.Globalization.CultureInfo.InvariantCulture);
    private static string ColorModeLine(VrProfile p) => p.UsesColorMode ? $"""

        Mode={Int((int)p.ColorMode)}
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
        GazeDofSettleMs={p.GazeDofSettleMs.ToString(System.Globalization.CultureInfo.InvariantCulture)}
        GazeDofFadeInMs={p.GazeDofFadeInMs.ToString(System.Globalization.CultureInfo.InvariantCulture)}
        GazeDofToleranceD={Number(p.GazeDofToleranceD)}
        GazeDofFoveaGuardDeg={Number(p.GazeDofFoveaGuardDeg)}
        GazeDofPupilMm={Number(p.GazeDofPupilMm)}
        GVision={(p.UsesGVision ? 1 : 0)}
        GVisionStrength={Number(p.GVisionStrength)}
        GVisionUseGaze={(p.GVisionUseGaze ? 1 : 0)}
        GVisionRedout={(p.GVisionRedout ? 1 : 0)}
        GVisionTolerance={Number(p.GVisionTolerance)}
        GVisionMode={Int((int)p.GVisionMode)}
        """ : "");

    public static string Ini(VrProfile p) => $"""
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
        CacheUs={Int(Math.Clamp(p.EngineTimerRefreshUs, VrProfile.EngineTimerRefreshMin, VrProfile.EngineTimerRefreshMax))}

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
        FarDepthExportSlots=1
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
}
