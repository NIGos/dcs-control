namespace DcsVr.Core;

/// <summary>
/// Live mode: the settings whose component re-reads its file while DCS runs, so the app can write that file at once
/// (ControlService.ApplyLive). Phase 1: the engine module's DcsQvCull.ini (re-read every second) and the pupil shift
/// layer's PupilShift.ini (about every 90 frames). Phase 2: Quad Views' settings.cfg (about every second), OFXR's
/// ofxr_bridge.ini (every 250 ms, NVIDIA flow keys only) and Cheeky's CheekyFoveatedDLSS.ini (about every second, DLSS 5
/// tuning). Phase 2 files are written only for a profile applied by an app whose bundled components reload them
/// (<see cref="Reloads"/>). Everything else (options.lua, layers, DLLs, other keys) waits for the next launch. One table
/// for the backend's whitelist and the interface's "Live" tags: the interface gets <see cref="AvailableFields"/>.
/// </summary>
public static class LiveSettings
{
    public const string EngineFile = "DcsQvCull.ini";
    public const string PupilFile = "PupilShift.ini";
    public const string QuadFile = "settings.cfg";
    public const string FlowFile = "ofxr_bridge.ini";
    public const string NeuralFile = "CheekyFoveatedDLSS.ini";
    public static IReadOnlyList<string> Files { get; } = [EngineFile, PupilFile, QuadFile, FlowFile, NeuralFile];
    /// <summary>The first app version whose bundled Quad Views, OFXR, Cheeky and engine module (R144) reload their files.</summary>
    public const string ReloadVersion = "0.6.2-preview";

    /// <summary>Engine: an engine part, live only when DCS started with Engine Optimizations on (PerceptionOnly=0).
    /// Visual: a VR image effect of the engine module. Pupil: the pupil shift layer (and, for some, the module too).
    /// Quad: Quad Views' post-processing. Flow: OFXR's NVIDIA optical flow (DCS started with NVIDIA frame generation).
    /// Neural: Cheeky's DLSS 5 tuning.</summary>
    public enum Kind { Engine, Visual, Pupil, Quad, Flow, Neural }

    /// <summary>A live setting: its profile property (camelCase, as in profile.json and the interface's field ids) and the
    /// component files it lives in. A setting in two files is live only while both components run.</summary>
    public sealed record Field(string Name, Kind Kind, params string[] In);

    private static Field E(string name) => new(name, Kind.Engine, EngineFile);
    private static Field V(string name) => new(name, Kind.Visual, EngineFile);
    private static Field P(string name) => new(name, Kind.Pupil, PupilFile);
    private static Field Q(string name) => new(name, Kind.Quad, QuadFile);
    private static Field F(string name) => new(name, Kind.Flow, FlowFile);
    private static Field N(string name) => new(name, Kind.Neural, NeuralFile);

    /// <summary>Every live setting (docs: the live-reload map). Cost weights and Texture dedupe switch on and off live; the
    /// Model allocator too from R144 (before: off only); Diagnostic hooks only on (<see cref="IsLiveKey"/>). Quad Views'
    /// edge blend also sets Cheeky's DLSS 5 edge feather (live too); if it also turns the edge fade on or off, it waits.</summary>
    public static IReadOnlyList<Field> Fields { get; } =
    [
        E("engineReleaseIdleDlss"), E("engineShadowCache"), E("engineGBufferRecorder"), E("engineModelAllocator"), E("engineShaderTimeCache"),
        E("engineTaskQueueClock"), E("boostPrefetch"), E("engineShadowRecorder"), E("engineShadowInstancing"), E("engineCostWeights"),
        E("enginePartitionBoost"), E("engineTextureDedupe"), E("engineEffectBufferSkip"), E("enginePlainCounter"), E("engineFrameHeap"),
        E("engineStateFilter"), E("engineBeeps"), E("engineTimerRefreshUs"), E("engineDevPayloadPath"), E("engineDevIniPath"), E("engineToggleKey"),
        E("engineDiagnosticHooks"),
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
        Q("quadSharpening"), Q("quadEdgeBlend"), Q("quadSharpenTaper"), Q("quadPeripheryContrast"), Q("peripheryStability"), Q("peripheryStabilityStrength"),
        // R148: the flow quality preset is not here: OFXR re-initialises its NVIDIA flow for it (5.5 s), and DCS froze for
        // 6.8 s. Resolution and bidirectional flow cost a single frame of about 0.5 s.
        F("nvidiaFlowScale"), F("bidirectionalFlow"),
        N("neuralIntensity"), N("neuralLocalTone"), N("neuralLocalStructure"), N("neuralSkinStructure"), N("neuralStyle"), N("neuralAutomaticMask"),
        N("neuralUiCorrection"), N("neuralColorStrength"), N("neuralTransferStrength"), N("neuralPaperWhiteScale"), N("neuralDepth"),
        N("neuralMotionScaleX"), N("neuralMotionScaleY"),
    ];

    private static readonly Dictionary<string, Field> ByName = Fields.ToDictionary(f => f.Name, StringComparer.Ordinal);
    public static Field? Find(string name) => ByName.GetValueOrDefault(name);

    /// <summary>The applied profile was installed by an app whose bundled components reload their files in flight (the
    /// journal records the app version since <see cref="ReloadVersion"/>; older journals have none).</summary>
    public static bool Current(TransactionJournal? journal) =>
        ReleaseVersion.Parse(journal?.AppVersion) is { } installed && installed.CompareTo(ReleaseVersion.Parse(ReloadVersion)) >= 0;

    /// <summary>The component behind <paramref name="file"/> re-reads it in a DCS started from <paramref name="applied"/>:
    /// always for phase 1; Quad Views, OFXR and Cheeky only when installed by a current app (<see cref="Current"/>); OFXR
    /// only when DCS started with NVIDIA frame generation (its flow keys mean nothing otherwise).</summary>
    public static bool Reloads(string file, VrProfile applied, TransactionJournal journal)
    {
        if (file.Equals(EngineFile, StringComparison.OrdinalIgnoreCase) || file.Equals(PupilFile, StringComparison.OrdinalIgnoreCase)) return true;
        if (!Current(journal)) return false;
        return !file.Equals(FlowFile, StringComparison.OrdinalIgnoreCase) || applied.ForLaunch().FrameGen == FrameGeneration.Nvidia;
    }

    /// <summary>The live settings for the applied profile: their components were installed with it (so they run in a DCS
    /// started from it) and reload their files, engine parts only with Engine Optimizations on, the image effects only in
    /// VR. Diagnostic hooks only while the applied profile has them off (turning them off waits for the next launch).</summary>
    public static IReadOnlyList<string> AvailableFields(VrProfile? applied, TransactionJournal? journal)
    {
        if (applied is null || journal is null) return [];
        var launch = applied.ForLaunch();
        var installed = journal.Entries.Select(e => Path.GetFileName(e.Path)).ToHashSet(StringComparer.OrdinalIgnoreCase);
        return [.. Fields.Where(f => f.In.All(file => installed.Contains(file) && Reloads(file, applied, journal)) && f.Kind switch
        {
            Kind.Engine => launch.EngineOptimizations && (f.Name != "engineDiagnosticHooks" || Current(journal) && !launch.EngineDiagnosticHooks),
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
    // PupilShift.ini keys the layer re-reads (pupil_shift_layer.cpp:241-320). Not OpenXrEyeGaze or [DlssPreset] (instance creation).
    private static readonly HashSet<string> PupilKeys = new(StringComparer.OrdinalIgnoreCase)
    {
        "PupilShift.Enabled", "PupilShift.Toggle", "PupilShift.EyeRadiusMm", "PupilShift.VirtualImageM", "PupilShift.MaxGazeDeg", "PupilShift.Ipd",
        "PupilShift.SimulateGaze", "PupilShift.SimulateGazeXDeg", "PupilShift.SimulateGazeYDeg", "PupilShift.PublishGaze", "DepthBoost.*",
    };
    // Quad Views' settings.cfg keys applied between two frames (native/quadviews/dcsvr_live.h), in any section. Exactly
    // these: any other key written live would also put the engine module's mirror of the file out of step.
    private static readonly HashSet<string> QuadKeys = new(StringComparer.OrdinalIgnoreCase)
    {
        "sharpen_focus_view", "smoothen_focus_view_edges", "dcsvr_sharpen_taper", "dcsvr_periphery_sharpen",
        "dcsvr_temporal", "dcsvr_temporal_tau_ms", "dcsvr_temporal_ramp_deg", "dcsvr_temporal_clip",
    };
    // ofxr_bridge.ini keys OFXR polls (patch 0066): the NVIDIA flow, rebuilt with a short hitch. Not nvidia_preset, which
    // OFXR also polls: a preset change re-initialises the flow (R148: 5.5 s, a 6.8 s freeze of DCS), so it waits for the
    // next launch. The occlusion choice follows the preset and bidirectional flow: live with a bidirectional change, and
    // waiting with the preset that changes it (the whole file waits then).
    private static readonly HashSet<string> FlowKeys = new(StringComparer.OrdinalIgnoreCase)
    {
        "ofxr.nvidia_input_scale", "ofxr.nvidia_bidirectional", "ofxr.nvidia_occlusion_aware",
    };
    // CheekyFoveatedDLSS.ini keys Cheeky applies while it runs (patches/cheeky/dcs_live_reload.hpp, dcs_live_keys). The first
    // eight without a hitch; the rest recreate the DLSS-NR feature (a short hitch).
    private static readonly HashSet<string> NeuralKeys = new(StringComparer.OrdinalIgnoreCase)
    {
        "CheekyFoveatedDLSS.NrPaperWhiteScale", "CheekyFoveatedDLSS.NrHdrTransferStrength", "CheekyFoveatedDLSS.NrColorStrength",
        "CheekyFoveatedDLSS.NrRoundness", "CheekyFoveatedDLSS.NrTransitionWidth", "CheekyFoveatedDLSS.NrMotionScaleXMultiplier",
        "CheekyFoveatedDLSS.NrMotionScaleYMultiplier", "CheekyFoveatedDLSS.NrDepthConvention",
        "CheekyFoveatedDLSS.NrIntensity", "CheekyFoveatedDLSS.NrLocalToneStrength", "CheekyFoveatedDLSS.NrLocalStructureStrength",
        "CheekyFoveatedDLSS.NrSkinStructureStrength", "CheekyFoveatedDLSS.NrStyle", "CheekyFoveatedDLSS.NrAutomaticMask", "CheekyFoveatedDLSS.NrUiCorrection",
    };

    /// <summary>True when the component applies this key (section-qualified, as <see cref="IniFile.Parse"/> names it) while
    /// DCS runs. <paramref name="value"/> is the new value (null: removed); <paramref name="atStart"/> what DCS started with
    /// (engine file); <paramref name="current"/> whether the profile was installed by a current app (<see cref="Current"/>):
    /// without it Quad Views, OFXR and Cheeky never reload, the Model allocator only switches off and Diagnostics waits.</summary>
    public static bool IsLiveKey(string file, string key, string? value, string? atStart, bool current = true)
    {
        if (file.Equals(QuadFile, StringComparison.OrdinalIgnoreCase)) return current && QuadKeys.Contains(key.Contains('.') ? key[(key.LastIndexOf('.') + 1)..] : key);
        if (file.Equals(FlowFile, StringComparison.OrdinalIgnoreCase)) return current && FlowKeys.Contains(key);
        if (file.Equals(NeuralFile, StringComparison.OrdinalIgnoreCase)) return current && NeuralKeys.Contains(key);
        var engine = file.Equals(EngineFile, StringComparison.OrdinalIgnoreCase);
        var keys = engine ? EngineKeys : file.Equals(PupilFile, StringComparison.OrdinalIgnoreCase) ? PupilKeys : null;
        if (keys is null) return false;
        // R144: the measurement hooks install live when turned on; once in, they stay until DCS restarts, so off waits.
        if (engine && key.Equals("General.Diagnostics", StringComparison.OrdinalIgnoreCase)) return current && value == "1";
        var section = key.Contains('.') ? key[..key.IndexOf('.')] : "";
        if (!keys.Contains(key) && !keys.Contains(section + ".*")) return false;
        // Before R144 the allocator was installed only when DCS started: off worked live, on only when DCS started with it on.
        if (engine && !current && key.Equals("Model.AllocSlabs", StringComparison.OrdinalIgnoreCase)) return value is null or "0" || atStart == "1";
        return true;
    }

    /// <summary>Whether the component writes this file itself while it runs (Cheeky saves its INI): then only the owned
    /// values decide whether the file is still the one DCS Control installed, and a live write changes only the keys that
    /// change, in the file as it is now.</summary>
    public static bool SelfWritten(string file) => file.Equals(NeuralFile, StringComparison.OrdinalIgnoreCase);

    /// <summary>How long after a live write to a self-written file it is read again (<see cref="SelfWritten"/>): Cheeky
    /// takes a changed file within about a second; a save of its own that raced the write would have dropped a value.</summary>
    public static readonly TimeSpan RecheckDelay = TimeSpan.FromSeconds(1.5);
}

/// <summary>A live write to a file its component also writes (Cheeky): the values DCS Control wrote, read again after
/// <see cref="LiveSettings.RecheckDelay"/> (<see cref="ControlService.RecheckLive"/>).</summary>
public sealed record LiveRecheck(string JournalId, string Path, IReadOnlyDictionary<string, string> Values);
/// <summary>A live write of one file (<see cref="OriginalsStore.UpdateInstalledLive(string, IReadOnlyList{LiveFileUpdate})"/>).
/// <paramref name="OwnedValues"/>: the file's component also writes it (Cheeky); these are the values DCS Control owns in it now.</summary>
public sealed record LiveFileUpdate(string Path, byte[] Content, string ExpectedSha256, IReadOnlyDictionary<string, string>? OwnedValues = null);
/// <summary>What a recheck found. Kept: the values are there (the record follows the file). Rewritten: a value was lost and
/// was written once more. Lost: lost again, left to the next launch. Superseded: a newer write or apply since.</summary>
public enum LiveRecheckOutcome { Kept, Rewritten, Lost, Superseded }

/// <summary>What a live apply did. <paramref name="Applied"/>: profile properties (camelCase) whose draft value is in effect
/// in the running DCS and differs from the applied profile.json or from what was in effect before (a live setting set
/// back). <paramref name="Waiting"/>: those whose draft value is not in effect now (its file or key waits, a one-way key
/// held back, a setting read at launch), counted against what is installed, not against profile.json.
/// <paramref name="Written"/> the files written now; <paramref name="Rechecks"/> the writes to self-written files to read
/// again shortly (<see cref="ControlService.RecheckLive"/>).</summary>
public sealed record LiveApplyResult(IReadOnlyList<string> Applied, IReadOnlyList<string> Waiting, IReadOnlyList<string> Written, string Message, IReadOnlyList<LiveRecheck>? Rechecks = null);
