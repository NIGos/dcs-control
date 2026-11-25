namespace DcsVr.Core;

/// <summary>
/// Live mode: the settings whose component re-reads its file while DCS runs, so the app can write that file at once
/// (ControlService.ApplyLive). Phase 1: the engine module's DcsQvCull.ini (re-read every second) and the pupil shift
/// layer's PupilShift.ini (about every 90 frames). Phase 2: Quad Views' settings.cfg (about every second), OFXR's
/// ofxr_bridge.ini (every 250 ms: NVIDIA flow keys; from 0.7 the headset panel's; with OFXR 0104/0123 the generated-frame
/// quality options) and Cheeky's CheekyFoveatedDLSS.ini
/// (about every second, DLSS 5 tuning). Phase 2 files are written only for a profile applied by an app whose bundled components reload them
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

    /// <summary>The keys of a live file that differ from the installed one because the profile changed. [Model]
    /// RecordersEmulatedLists also follows the module's log (<see cref="EngineOptimizations.EmulatedListsMatter(string?)"/>),
    /// which DCS rewrites while it runs: with Multi-threading without driver support itself unchanged, the key is not a
    /// change (the module reads it only when DCS starts), so it never holds back the live settings of that file.</summary>
    internal static string[] ProfileChanges(string file, IEnumerable<string> changed, VrProfile draft, VrProfile applied) =>
        [.. changed.Where(k => !(file == EngineFile && k.Equals("Model.RecordersEmulatedLists", StringComparison.OrdinalIgnoreCase)
            && draft.EngineRecordersEmulatedLists == applied.EngineRecordersEmulatedLists))];
    /// <summary>The first app version whose bundled Quad Views, OFXR, Cheeky and engine module (R144) reload their files.</summary>
    public const string ReloadVersion = "0.6.2-preview";
    /// <summary>The first app version whose bundled Quad Views (0026) and OFXR (0095) also reload the 0.7 live settings
    /// (<see cref="Live07"/>): the eye-tracking refinements, Far depth's strength and key, the headset panel.</summary>
    public const string Reload07Version = "0.7.0-preview";

    /// <summary>Engine: an engine part, live only when DCS started with Engine Optimizations on (PerceptionOnly=0).
    /// Visual: a VR image effect of the engine module. Pupil: the pupil shift layer (and, for some, the module too).
    /// Quad: Quad Views' post-processing, eye-tracking refinements and Far depth's strength and key. Flow: OFXR's NVIDIA
    /// optical flow (DCS started with NVIDIA frame generation). Panel: OFXR's headset panel (whenever OFXR runs, with frame
    /// generation or for the panel alone). Neural: Cheeky's DLSS 5 tuning. Audio: 3D headphone audio ([Audio] and its key in
    /// [Hotkeys]), live whenever the installed module has the audio part (<see cref="Binaural.PayloadHasAudio"/>), with or
    /// without Engine Optimizations, in VR or on a monitor.</summary>
    public enum Kind { Engine, Visual, Pupil, Quad, Flow, Neural, Panel, Audio }

    /// <summary>A live setting: its profile property (camelCase, as in profile.json and the interface's field ids) and the
    /// component files it lives in. A setting in two files is live only while both components run.</summary>
    public sealed record Field(string Name, Kind Kind, params string[] In);

    private static Field E(string name) => new(name, Kind.Engine, EngineFile);
    private static Field V(string name) => new(name, Kind.Visual, EngineFile);
    private static Field P(string name) => new(name, Kind.Pupil, PupilFile);
    private static Field Q(string name) => new(name, Kind.Quad, QuadFile);
    private static Field F(string name) => new(name, Kind.Flow, FlowFile);
    private static Field H(string name) => new(name, Kind.Panel, FlowFile);
    private static Field N(string name) => new(name, Kind.Neural, NeuralFile);
    private static Field A(string name) => new(name, Kind.Audio, EngineFile);

    /// <summary>Every live setting (docs: the live-reload map). Cost weights and Texture dedupe switch on and off live; the
    /// Model allocator too from R144 (before: off only); Diagnostic hooks only on (<see cref="IsLiveKey"/>). Quad Views'
    /// edge blend also sets Cheeky's DLSS 5 edge feather (live too); if it also turns the edge fade on or off, it waits.</summary>
    public static IReadOnlyList<Field> Fields { get; } =
    [
        E("engineShadowCache"), E("engineGBufferRecorder"), E("engineModelAllocator"), E("engineShaderTimeCache"),
        E("engineTaskQueueClock"), E("boostPrefetch"), E("engineShadowRecorder"), E("engineShadowInstancing"), E("engineCostWeights"),
        E("enginePartitionBoost"), E("engineTextureDedupe"), E("engineEffectBufferSkip"), E("enginePlainCounter"), E("engineFrameHeap"),
        E("engineStateFilter"), E("engineBeeps"), E("engineTimerRefreshUs"), E("engineDevPayloadPath"), E("engineDevIniPath"), E("engineToggleKey"),
        E("engineDiagnosticHooks"), E("engineFastShaderPaths"), E("engineWaterSkip"), E("engineIdleEffectSkip"),
        // R122 per-view DLSS presets: the module takes another letter at its next tick, and DCS's next mission load creates
        // its DLSS with it (DCS never recreates DLSS in a mission). Turning the preset on or off also changes the pupil shift
        // layer's [DlssPreset] (read at instance creation), and the focus preset also Cheeky's CenterPreset: those wait.
        E("peripheryDlssPreset"), E("focusDlssPreset"),
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
        // 0.7 (Quad Views 0026): the eye-tracking refinements, Far depth's strength and its key. Far depth on/off waits. The
        // key's name on OFXR's panel (DCSVR_FARDEPTH_HOTKEY, read at launch) keeps the old key until the next launch.
        Q("quadSaccadeLead"), Q("quadSaccadeTail"), Q("quadGazeStabilize"), Q("quadLostGazeEase"), Q("farDepthPreset"), Q("farDepthToggleKey"),
        // 0.7 (Quad Views 0035-0037): how the focus area follows a jump of the eyes and meets the periphery.
        Q("quadHideFocusEdge"), Q("quadGazeGlideMs"), Q("quadFocusFadeIn"), Q("quadKeepHistoryOnSaccade"), Q("quadBandMatch"), Q("quadBandMatchColor"),
        // 0.7: OFXR's headset panel. The FPS counter and the panel's corner are re-read with the panel (every 250 ms), the
        // VRAM counter by the live ini watcher (OFXR 0095). Not the panel's key (an environment variable read at launch)
        // or the smooth cursor (its pipelines and images are made before the first generated frame).
        H("showOverlay"), H("headsetPanelPosition"), H("diagnosticVram"),
        // R148: the flow quality preset is not here: OFXR re-initialises its NVIDIA flow for it (5.5 s), and DCS froze for
        // 6.8 s. Resolution and bidirectional flow cost a single frame of about 0.5 s.
        F("nvidiaFlowScale"), F("bidirectionalFlow"),
        // OFXR 0104 and 0123: the generated-frame quality options (set on each frame generator without a rebuild, no
        // hitch), taken by the same watcher; only where the installed OFXR has that live contract (FgQualityInstalled).
        F("frameGenSkyOcclusion"), F("frameGenHudGuard"), F("frameGenEdgeChoice"), F("frameGenMaskBand"),
        N("neuralIntensity"), N("neuralLocalTone"), N("neuralLocalStructure"), N("neuralSkinStructure"), N("neuralStyle"), N("neuralAutomaticMask"),
        N("neuralUiCorrection"), N("neuralColorStrength"), N("neuralTransferStrength"), N("neuralPaperWhiteScale"), N("neuralBrightnessMatch"), N("neuralDepth"),
        N("neuralMotionScaleX"), N("neuralMotionScaleY"),
        // 3D headphone audio: every [Audio] key and its in-flight key (binaural_config.h: all Live; a new HRTF set is loaded
        // on the module's control thread and crossfaded in). Turning it on or off live writes only [Audio] on a monitor; in
        // VR it also deploys or removes the pupil shift layer's head pose (PupilShift.ini [HeadPose], read when DCS starts
        // VR), which waits: the generic signature check in ControlService.ApplyLive finds that. A set imported since the
        // launch is a new file next to the module, so picking it waits too. binauralExcludeContexts (profile file only, no
        // interface field) is not in the table: an edit by hand waits for the next launch.
        A("binauralAudio"), A("binauralHrtf"), A("headCircumferenceCm"), A("binauralLateHeadTracking"), A("binauralNearField"), A("binauralMaxObjects"),
        A("binauralLevelTrimDb"), A("binauralCpuBudgetPercent"), A("binauralLogVoices"), A("audioToggleKey"),
    ];

    private static readonly Dictionary<string, Field> ByName = Fields.ToDictionary(f => f.Name, StringComparer.Ordinal);
    public static Field? Find(string name) => ByName.GetValueOrDefault(name);

    /// <summary>The settings live only with components installed by a 0.7 app (<see cref="Current07"/>).</summary>
    public static IReadOnlySet<string> Live07 { get; } = new HashSet<string>(StringComparer.Ordinal)
    {
        "quadSaccadeLead", "quadSaccadeTail", "quadGazeStabilize", "quadLostGazeEase", "farDepthPreset", "farDepthToggleKey",
        "quadHideFocusEdge", "quadGazeGlideMs", "quadFocusFadeIn", "quadKeepHistoryOnSaccade", "quadBandMatch", "quadBandMatchColor",
        "showOverlay", "headsetPanelPosition", "diagnosticVram",
    };

    /// <summary>The applied profile was installed by an app whose bundled components reload their files in flight (the
    /// journal records the app version since <see cref="ReloadVersion"/>; older journals have none).</summary>
    public static bool Current(TransactionJournal? journal) => InstalledSince(journal, ReloadVersion);
    /// <summary>The applied profile was installed by an app whose Quad Views and OFXR also reload <see cref="Live07"/>.</summary>
    public static bool Current07(TransactionJournal? journal) => InstalledSince(journal, Reload07Version);
    /// <summary>The engine payload the applied profile installed has the audio part (<see cref="Binaural.PayloadHasAudio"/>):
    /// only then does a live [Audio] write reach a module that reads it.</summary>
    public static bool AudioInstalled(TransactionJournal? journal) =>
        journal?.Entries.FirstOrDefault(e => Path.GetFileName(e.Path).Equals("DcsQvCullPayload.dll", StringComparison.OrdinalIgnoreCase)) is { } payload && Binaural.PayloadHasAudio(payload.Path);
    /// <summary>The names of the personal HRTF sets the applied profile installed (Scripts\DcsQvCull\hrtf\&lt;name&gt;.dcshrtf).</summary>
    public static IReadOnlySet<string> InstalledHrtfSets(TransactionJournal? journal) =>
        (journal?.Entries ?? []).Where(e => e.Path.EndsWith(Binaural.HrtfExtension, StringComparison.OrdinalIgnoreCase)
                && string.Equals(Path.GetFileName(Path.GetDirectoryName(e.Path)), Binaural.HrtfFolder, StringComparison.OrdinalIgnoreCase))
            .Select(e => Path.GetFileNameWithoutExtension(e.Path)).ToHashSet(StringComparer.OrdinalIgnoreCase);
    /// <summary>The settings OFXR 0104 and 0123 take in live (<see cref="FgQualityInstalled"/>).</summary>
    public static IReadOnlySet<string> FgQualityFields { get; } = new HashSet<string>(StringComparer.Ordinal)
    {
        "frameGenSkyOcclusion", "frameGenHudGuard", "frameGenEdgeChoice", "frameGenMaskBand",
    };
    /// <summary>The OFXR layer the applied profile installed takes the generated-frame quality keys
    /// in live with the contract this app writes them for (<see cref="OfxrReadsFgQuality"/>). Not a version check: this app
    /// version can be the one that installed an OFXR without them (0.7.0), whose live watcher would ignore them.</summary>
    public static bool FgQualityInstalled(TransactionJournal? journal) =>
        journal?.Entries.FirstOrDefault(e => Path.GetFileName(e.Path).Equals(OfxrLayerFile, StringComparison.OrdinalIgnoreCase)) is { } layer && OfxrReadsFgQuality(layer.Path);
    public const string OfxrLayerFile = "XR_APILAYER_XRFrameBridge_diagnostic.dll";
    /// <summary>The live contract marker an OFXR layer carries (fork patch 0123, src/common/embedded_control.cpp,
    /// <c>OFXR_LiveFgContract</c>): this text, then the contract's number in ASCII digits. 2 (0123): the quality keys are
    /// set on each frame generator without a rebuild and no unused swapchain is armed by the change. A layer without it
    /// (0104 only, of the 0.7.1 development builds, or 0.7.0 without the keys) does not count. A later contract raises the number and
    /// keeps the text; any number from <see cref="FgLiveContract"/> up counts.</summary>
    public const string FgLiveContractMarker = "DCSC-OFXR-LIVE-FG/";
    public const int FgLiveContract = 2;
    private static readonly byte[] FgLiveContractBytes = System.Text.Encoding.ASCII.GetBytes(FgLiveContractMarker);
    private static readonly System.Collections.Concurrent.ConcurrentDictionary<string, (long Length, DateTime Written, bool Has)> OfxrCache = new(StringComparer.OrdinalIgnoreCase);
    /// <summary>The OFXR layer at <paramref name="layerPath"/> carries the live contract marker with a number from
    /// <see cref="FgLiveContract"/> up (<see cref="FgLiveContractMarker"/>). Cached by size and time: the interface asks on
    /// every state refresh.</summary>
    public static bool OfxrReadsFgQuality(string? layerPath)
    {
        if (layerPath is null) return false;
        try
        {
            var info = new FileInfo(layerPath);
            if (!info.Exists || info.Length > 256L * 1024 * 1024) return false;
            if (OfxrCache.TryGetValue(info.FullName, out var cached) && cached.Length == info.Length && cached.Written == info.LastWriteTimeUtc) return cached.Has;
            var bytes = File.ReadAllBytes(info.FullName);
            var has = HasLiveContract(bytes);
            OfxrCache[info.FullName] = (info.Length, info.LastWriteTimeUtc, has);
            return has;
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException) { return false; }
    }
    /// <summary>Whether <paramref name="bytes"/> hold <see cref="FgLiveContractMarker"/> followed by a number from
    /// <see cref="FgLiveContract"/> up (every occurrence is looked at).</summary>
    public static bool HasLiveContract(ReadOnlySpan<byte> bytes)
    {
        for (int at; (at = bytes.IndexOf(FgLiveContractBytes)) >= 0; bytes = bytes[(at + 1)..])
        {
            var rest = bytes[(at + FgLiveContractBytes.Length)..];
            int digits = 0, value = 0;
            while (digits < rest.Length && digits < 6 && rest[digits] is >= (byte)'0' and <= (byte)'9') value = value * 10 + (rest[digits++] - '0');
            if (digits > 0 && value >= FgLiveContract) return true;
        }
        return false;
    }
    private static bool InstalledSince(TransactionJournal? journal, string version) =>
        ReleaseVersion.Parse(journal?.AppVersion) is { } installed && installed.CompareTo(ReleaseVersion.Parse(version)) >= 0;

    /// <summary>The component behind <paramref name="file"/> re-reads it in a DCS started from <paramref name="applied"/>:
    /// always for phase 1; Quad Views, OFXR and Cheeky only when installed by a current app (<see cref="Current"/>); OFXR
    /// when DCS started with NVIDIA frame generation (its flow keys), or, installed by a 0.7 app, whenever it runs (its
    /// headset panel keys; <see cref="IsLiveKey"/> then takes the flow keys only with NVIDIA frame generation).</summary>
    public static bool Reloads(string file, VrProfile applied, TransactionJournal journal)
    {
        if (file.Equals(EngineFile, StringComparison.OrdinalIgnoreCase) || file.Equals(PupilFile, StringComparison.OrdinalIgnoreCase)) return true;
        if (!Current(journal)) return false;
        if (!file.Equals(FlowFile, StringComparison.OrdinalIgnoreCase)) return true;
        var launch = applied.ForLaunch();
        return launch.FrameGen == FrameGeneration.Nvidia || Current07(journal) && launch.UsesHeadsetPanel;
    }

    /// <summary>The live settings for the applied profile: their components were installed with it (so they run in a DCS
    /// started from it) and reload their files, engine parts only with Engine Optimizations on, the image effects only in
    /// VR. Diagnostic hooks only while the applied profile has them off (turning them off waits for the next launch).</summary>
    public static IReadOnlyList<string> AvailableFields(VrProfile? applied, TransactionJournal? journal)
    {
        if (applied is null || journal is null) return [];
        var launch = applied.ForLaunch();
        var installed = journal.Entries.Select(e => Path.GetFileName(e.Path)).ToHashSet(StringComparer.OrdinalIgnoreCase);
        var current07 = Current07(journal);
        var audio = AudioInstalled(journal);
        var fgQuality = FgQualityInstalled(journal);
        return [.. Fields.Where(f => f.In.All(file => installed.Contains(file) && Reloads(file, applied, journal)) && (current07 || !Live07.Contains(f.Name)) && f.Kind switch
        {
            Kind.Audio => audio,
            Kind.Engine => launch.EngineOptimizations && (f.Name != "engineDiagnosticHooks" || Current(journal) && !launch.EngineDiagnosticHooks),
            Kind.Visual => !launch.Desktop,
            // With foveated frame generation, bidirectional flow also picks how the strip a fast eye move uncovers is
            // filled (foveated_uncovered_fill), which OFXR reads at start: the whole file waits.
            // The quality options only with an OFXR that reads them.
            Kind.Flow => launch.FrameGen == FrameGeneration.Nvidia && (f.Name != "bidirectionalFlow" || !launch.UsesFoveatedFrameGen) && (fgQuality || !FgQualityFields.Contains(f.Name)),
            Kind.Panel => launch.UsesHeadsetPanel,
            // With Soft focus edge on, Hide the focus edge also switches dcsvr_edge_match, which Quad Views reads at start.
            Kind.Quad => f.Name != "quadHideFocusEdge" || !launch.QuadEdgeMatch,
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
        "DLSS.ReleaseIdleSets", "DLSS.PeripheryPreset", "DLSS.FocusPreset", "Dev.PayloadPath", "Dev.IniPath",
        // 0.7: water no-submit (hot reload), early shaders (worker tick). [Views] ThreeViews is parked: the app no longer writes it.
        "Water.SkipHiddenWater", "D3D.EarlyShaders",
        // [D3D] ChainFast / ShaderFast: re-applied by the worker every second (worker.inc). Off takes effect at once. On: kinds
        // DCS creates afterwards are taken at creation, the ones it made earlier by the bind-time identification, which counts
        // only flight time (180 s) and, once ended with kinds missing, opens again at the next mission load (regfix 82c5345).
        // So live, and the interface says the full gain can take a few minutes of flight.
        "D3D.ChainFast", "D3D.ShaderFast",
        // 3D headphone audio (binaural_config.h: every key Live) and its in-flight key; only with an audio payload (audio).
        "Audio.*", "Hotkeys.AudioToggle",
    };
    private static bool IsAudioKey(string key) => key.StartsWith("Audio.", StringComparison.OrdinalIgnoreCase) || key.Equals("Hotkeys.AudioToggle", StringComparison.OrdinalIgnoreCase);
    // PupilShift.ini keys the layer re-reads (pupil_shift_layer.cpp:241-320). Not OpenXrEyeGaze, QuadViewsFixedFocus or [DlssPreset] (instance creation).
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
    // Quad Views 0026 (0.7): the eye-tracking filter, Far depth's curve and key. The engine module's mirror reads none of them.
    private static readonly HashSet<string> Quad07Keys = new(StringComparer.OrdinalIgnoreCase)
    {
        "dcsvr_saccade_widening", "dcsvr_saccade_speed", "dcsvr_saccade_lead_ms", "dcsvr_saccade_max_extend", "dcsvr_saccade_hold_ms",
        "dcsvr_saccade_end_speed", "dcsvr_saccade_tail", "dcsvr_gaze_deadzone", "dcsvr_lost_ease_ms",
        "dcsvr_temporal_saccade_reset", "dcsvr_gaze_glide_ms", "dcsvr_focus_fade_in", "dcsvr_band_match", "dcsvr_band_match_color",
        "dcsvr_far_depth_preset", "dcsvr_far_depth_world_ipd", "dcsvr_far_depth_z0_m", "dcsvr_far_depth_gamma", "dcsvr_far_depth_toggle",
    };
    // ofxr_bridge.ini keys of the headset panel (0.7): [overlay] position and panel_position, read with the panel every
    // 250 ms (openxr_fps_overlay.cpp); [ofxr] diag_vram, taken by the live ini watcher (OFXR 0095). Whenever OFXR runs.
    private static readonly HashSet<string> PanelKeys = new(StringComparer.OrdinalIgnoreCase)
    {
        "overlay.position", "overlay.panel_position", "ofxr.diag_vram",
    };
    // ofxr_bridge.ini keys OFXR polls (patch 0066): the NVIDIA flow, rebuilt with a short hitch. Not nvidia_preset, which
    // OFXR also polls: a preset change re-initialises the flow (R148: 5.5 s, a 6.8 s freeze of DCS), so it waits for the
    // next launch. The occlusion choice follows the preset and bidirectional flow: live with a bidirectional change, and
    // waiting with the preset that changes it (the whole file waits then).
    private static readonly HashSet<string> FlowKeys = new(StringComparer.OrdinalIgnoreCase)
    {
        "ofxr.nvidia_input_scale", "ofxr.nvidia_bidirectional", "ofxr.nvidia_occlusion_aware",
    };
    // ofxr_bridge.ini keys of OFXR 0104 (the generated-frame quality options: shader bits, set_fg_quality, no rebuild),
    // followed by the same watcher with NVIDIA frame generation; only where the installed OFXR reads them (fgQuality,
    // FgQualityInstalled).
    private static readonly HashSet<string> FgQualityKeys = new(StringComparer.OrdinalIgnoreCase)
    {
        "ofxr.dcs_motion_sky_occlusion", "ofxr.dcs_motion_hud_guard", "ofxr.nvidia_edge_choice", "ofxr.dcs_motion_mask_band",
    };
    // CheekyFoveatedDLSS.ini keys Cheeky applies while it runs (patches/cheeky/dcs_live_reload.hpp, dcs_live_keys). The first
    // nine without a hitch; the rest recreate the DLSS-NR feature (a short hitch).
    private static readonly HashSet<string> NeuralKeys = new(StringComparer.OrdinalIgnoreCase)
    {
        "CheekyFoveatedDLSS.NrPaperWhiteScale", "CheekyFoveatedDLSS.NrHdrTransferStrength", "CheekyFoveatedDLSS.NrColorStrength",
        "CheekyFoveatedDLSS.NrBrightnessMatch",
        "CheekyFoveatedDLSS.NrRoundness", "CheekyFoveatedDLSS.NrTransitionWidth", "CheekyFoveatedDLSS.NrMotionScaleXMultiplier",
        "CheekyFoveatedDLSS.NrMotionScaleYMultiplier", "CheekyFoveatedDLSS.NrDepthConvention",
        "CheekyFoveatedDLSS.NrIntensity", "CheekyFoveatedDLSS.NrLocalToneStrength", "CheekyFoveatedDLSS.NrLocalStructureStrength",
        "CheekyFoveatedDLSS.NrSkinStructureStrength", "CheekyFoveatedDLSS.NrStyle", "CheekyFoveatedDLSS.NrAutomaticMask", "CheekyFoveatedDLSS.NrUiCorrection",
    };

    /// <summary>True when the component applies this key (section-qualified, as <see cref="IniFile.Parse"/> names it) while
    /// DCS runs. <paramref name="value"/> is the new value (null: removed); <paramref name="atStart"/> what DCS started with
    /// (engine file); <paramref name="current"/> whether the profile was installed by a current app (<see cref="Current"/>):
    /// without it Quad Views, OFXR and Cheeky never reload, the Model allocator only switches off and Diagnostics waits.
    /// <paramref name="current07"/>: installed by a 0.7 app (<see cref="Current07"/>), for the 0.7 keys of Quad Views and
    /// OFXR's headset panel; <paramref name="nvidiaFlow"/>: DCS started with NVIDIA frame generation, for OFXR's flow keys.</summary>
    /// <paramref name="audio"/>: the installed engine payload has the audio part (<see cref="AudioInstalled"/>), for [Audio].
    /// <paramref name="hrtfSets"/>: the personal HRTF sets installed next to the module (<see cref="InstalledHrtfSets"/>);
    /// [Audio] Hrtf naming another one waits, since its file comes only with the next launch. Null: not checked.
    /// <paramref name="fgQuality"/>: the installed OFXR reads the quality keys (<see cref="FgQualityInstalled"/>).
    public static bool IsLiveKey(string file, string key, string? value, string? atStart, bool current = true, bool current07 = true, bool nvidiaFlow = true, bool audio = true, IReadOnlySet<string>? hrtfSets = null, bool audioOnLive = true, bool fgQuality = true)
    {
        if (file.Equals(EngineFile, StringComparison.OrdinalIgnoreCase) && IsAudioKey(key))
            return audio && !(key.Equals("Audio.Hrtf", StringComparison.OrdinalIgnoreCase) && hrtfSets is not null && value is not null
                && !value.Equals(Binaural.DefaultHrtf, StringComparison.OrdinalIgnoreCase) && !hrtfSets.Contains(value))
                // Turned on when DCS started with it off: live only when nothing it needs waits for the launch (audioOnLive:
                // no head pose to start in VR, DCS's speaker layout already Stereo). Back on after a live off is live.
                && !(key.Equals("Audio.Binaural", StringComparison.OrdinalIgnoreCase) && value == "1" && atStart != "1" && !audioOnLive);
        if (file.Equals(QuadFile, StringComparison.OrdinalIgnoreCase))
        {
            var name = key.Contains('.') ? key[(key.LastIndexOf('.') + 1)..] : key;
            return current && (QuadKeys.Contains(name) || current07 && Quad07Keys.Contains(name));
        }
        if (file.Equals(FlowFile, StringComparison.OrdinalIgnoreCase)) return current && (nvidiaFlow && (FlowKeys.Contains(key) || fgQuality && FgQualityKeys.Contains(key)) || current07 && PanelKeys.Contains(key));
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
