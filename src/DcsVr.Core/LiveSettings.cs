namespace DcsVr.Core;

/// <summary>
/// Live mode: the settings whose component re-reads its file while DCS runs, so the app can write that file at once
/// (ControlService.ApplyLive). Only two files qualify: the engine module's DcsQvCull.ini (re-read every second) and the
/// pupil shift layer's PupilShift.ini (re-read about every 90 frames). Everything else (Quad Views, OFXR, Cheeky,
/// options.lua, layers, DLLs) waits for the next launch. One table for the backend's whitelist and the interface's
/// "Live" tags: the interface gets <see cref="AvailableFields"/> in its state.
/// </summary>
public static class LiveSettings
{
    public const string EngineFile = "DcsQvCull.ini";
    public const string PupilFile = "PupilShift.ini";
    public static IReadOnlyList<string> Files { get; } = [EngineFile, PupilFile];

    /// <summary>Engine: an engine part, live only when DCS started with Engine Optimizations on (PerceptionOnly=0).
    /// Visual: a VR image effect of the engine module. Pupil: the pupil shift layer (and, for some, the module too).</summary>
    public enum Kind { Engine, Visual, Pupil }

    /// <summary>A live setting: its profile property (camelCase, as in profile.json and the interface's field ids) and the
    /// component files it lives in. A setting in two files is live only while both components run.</summary>
    public sealed record Field(string Name, Kind Kind, params string[] In);

    private static Field E(string name) => new(name, Kind.Engine, EngineFile);
    private static Field V(string name) => new(name, Kind.Visual, EngineFile);
    private static Field P(string name) => new(name, Kind.Pupil, PupilFile);

    /// <summary>Every live setting (docs: the live-reload map). The Model allocator, Cost weights and Texture dedupe
    /// switch off live; switching them on live works only when DCS started with them on (<see cref="IsLiveKey"/>).</summary>
    public static IReadOnlyList<Field> Fields { get; } =
    [
        E("engineReleaseIdleDlss"), E("engineShadowCache"), E("engineGBufferRecorder"), E("engineModelAllocator"), E("engineShaderTimeCache"),
        E("engineTaskQueueClock"), E("boostPrefetch"), E("engineShadowRecorder"), E("engineShadowInstancing"), E("engineCostWeights"),
        E("enginePartitionBoost"), E("engineTextureDedupe"), E("engineEffectBufferSkip"), E("enginePlainCounter"), E("engineFrameHeap"),
        E("engineStateFilter"), E("engineBeeps"), E("engineTimerRefreshUs"), E("engineDevPayloadPath"), E("engineDevIniPath"), E("engineToggleKey"),
        V("colorMode"), V("panelGamut"), V("colorModeKey"),
        V("colorFixExposureEv"), V("colorFixNightEv"), V("colorFixSaturation"), V("colorFixWarmth"), V("colorFixAdaptBright"), V("colorFixAdaptDark"),
        V("colorEnhancedExposureEv"), V("colorEnhancedNightEv"), V("colorEnhancedSaturation"), V("colorEnhancedWarmth"), V("colorEnhancedAdaptBright"), V("colorEnhancedAdaptDark"),
        V("nightVision"), V("nightVisionStrength"), V("nightVisionBlue"), V("nightVisionBlur"), V("nightVisionScotopic"), V("nightVisionPhotopic"),
        V("glare"), V("glareStrength"), V("glareThresholdEv"), V("glareRadius"), V("glareStreak"),
        V("gVision"), V("gVisionStrength"), V("gVisionMode"), V("gVisionTolerance"), V("gVisionRedout"),
        new("gVisionUseGaze", Kind.Visual, EngineFile, PupilFile), new("gazeDof", Kind.Visual, EngineFile, PupilFile),
        V("gazeDofStrength"), V("gazeDofMaxArcmin"), V("gazeDofSaccadeRelease"), V("gazeDofSettleMs"), V("gazeDofFadeInMs"), V("gazeDofToleranceD"),
        V("gazeDofFoveaGuardDeg"), V("gazeDofPupilMm"), V("ownAircraftTrueDepth"),
        P("pupilShift"), P("pupilShiftEyeRadiusMm"), P("pupilShiftVirtualImageM"), P("pupilShiftToggleKey"), P("eyeDistanceMm"),
        new("depthBoost", Kind.Pupil, PupilFile, EngineFile), P("depthBoostMode"), P("depthBoostRealisticMax"), P("depthBoostMax"),
        P("depthBoostFarStartM"), P("depthBoostFarFullM"), P("depthBoostNearM"), P("depthBoostToggleKey"),
    ];

    private static readonly Dictionary<string, Field> ByName = Fields.ToDictionary(f => f.Name, StringComparer.Ordinal);
    public static Field? Find(string name) => ByName.GetValueOrDefault(name);

    /// <summary>The live settings for the applied profile: their components were installed with it (so they run in a DCS
    /// started from it), engine parts only with Engine Optimizations on, the image effects only in VR.</summary>
    public static IReadOnlyList<string> AvailableFields(VrProfile? applied, TransactionJournal? journal)
    {
        if (applied is null || journal is null) return [];
        var launch = applied.ForLaunch();
        var installed = journal.Entries.Select(e => Path.GetFileName(e.Path)).ToHashSet(StringComparer.OrdinalIgnoreCase);
        return [.. Fields.Where(f => f.In.All(installed.Contains) && f.Kind switch
        {
            Kind.Engine => launch.EngineOptimizations,
            Kind.Visual => !launch.Desktop,
            _ => true
        }).Select(f => f.Name)];
    }

    // DcsQvCull.ini keys the module applies within a second (main/config_load.inc); a section ending in ".*" takes every key.
    private static readonly HashSet<string> EngineKeys = new(StringComparer.OrdinalIgnoreCase)
    {
        "General.Beeps", "Timing.ShaderTimeCache", "Timing.TaskQueueClock", "Timing.CacheUs", "Hotkeys.Toggle",
        "Scene.PartitionBoost", "Scene.CostWeights", "D3D.SplitFilter", "Shadows.CascadeCache",
        "Model.AllocSlabs", "Model.PlainTriangleCounter", "Model.FrameHeapSlabs", "Model.BigModelPages", "Model.ShadowInstancing", "Model.ShadowBatching",
        "Model.ShadowPlanAsync", "Model.ShadowTextureSkip", "Model.ShadowRecorder", "Model.GBufferRecorder",
        "Texture.StreamDedupe", "Effects.SkipSameConstantBuffer", "Prefetch.Mode",
        "Color.Mode", "Color.PanelGamut", "Color.ModeKey", "ColorFix.*", "ColorEnhanced.*", "VrFx.*",
        "Perception.FixationDepth", "Perception.CockpitTrueIpd", "Perception.CockpitTrueIpdOwnship",
        "DLSS.ReleaseIdleSets", "Dev.PayloadPath", "Dev.IniPath",
    };
    /// <summary>Installed only when DCS starts (worker.inc:58-60): off works live, on only when DCS started with it on.</summary>
    private static readonly HashSet<string> OneWayKeys = new(StringComparer.OrdinalIgnoreCase) { "Scene.CostWeights", "Model.AllocSlabs", "Texture.StreamDedupe" };
    // PupilShift.ini keys the layer re-reads (pupil_shift_layer.cpp:241-320). Not OpenXrEyeGaze or [DlssPreset] (instance creation).
    private static readonly HashSet<string> PupilKeys = new(StringComparer.OrdinalIgnoreCase)
    {
        "PupilShift.Enabled", "PupilShift.Toggle", "PupilShift.EyeRadiusMm", "PupilShift.VirtualImageM", "PupilShift.MaxGazeDeg", "PupilShift.Ipd",
        "PupilShift.SimulateGaze", "PupilShift.SimulateGazeXDeg", "PupilShift.SimulateGazeYDeg", "PupilShift.PublishGaze", "DepthBoost.*",
    };

    /// <summary>True when the component applies this key (section-qualified, as <see cref="IniFile.Parse"/> names it) while
    /// DCS runs. <paramref name="value"/> is the new value (null: removed); <paramref name="atStart"/> what DCS started with.</summary>
    public static bool IsLiveKey(string file, string key, string? value, string? atStart)
    {
        var keys = file.Equals(EngineFile, StringComparison.OrdinalIgnoreCase) ? EngineKeys : file.Equals(PupilFile, StringComparison.OrdinalIgnoreCase) ? PupilKeys : null;
        if (keys is null) return false;
        var section = key.Contains('.') ? key[..key.IndexOf('.')] : "";
        if (!keys.Contains(key) && !keys.Contains(section + ".*")) return false;
        if (keys == EngineKeys && OneWayKeys.Contains(key)) return value is null or "0" || atStart == "1";
        return true;
    }
}

/// <summary>What a live apply did. <paramref name="Applied"/> and <paramref name="Waiting"/> are profile properties
/// (camelCase) that differ from the applied profile: in effect now, or waiting for the next launch.
/// <paramref name="Written"/> the files written now.</summary>
public sealed record LiveApplyResult(IReadOnlyList<string> Applied, IReadOnlyList<string> Waiting, IReadOnlyList<string> Written, string Message);
