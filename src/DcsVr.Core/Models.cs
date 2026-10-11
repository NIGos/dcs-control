using System.Text.Json;
using System.Text.Json.Serialization;

namespace DcsVr.Core;

public enum RuntimeKind { Pimax, SboysSteamVr }
public enum QuadProvider { None, PimaxNative, QuadViewsFoveated }
public enum FrameGeneration { Off, Nvidia, FidelityFx }
public enum GazeMode { Fixed, EyeTracked }
public enum TrackingKind { Unknown, Slam, Lighthouse }
public enum NeuralDepthMode { Game, Normal, Reversed }
/// <summary>DLSS-NR model style (DLSSNR.Style 0..2), the same three modes RenoDX's DLSS 5 add-on offers.</summary>
public enum NeuralStyle { Standard, Natural, Cinematic }
public enum NvidiaFlowPreset { Fast, Medium, Slow }
public enum FpsLimitMode { Preserve, RuntimeHeadroom, MatchRefresh, Custom }
public enum IssueSeverity { Info, Warning, Error }
/// <summary>Where the bundled Quad Views focus area comes from: Pimax Play's own Quad Views settings, read (never
/// written) at preview, apply and launch, or the values stored in the profile.</summary>
public enum FoveaSource { PimaxPlay, Profile }
public enum BoostPriority { Normal, AboveNormal, High }
/// <summary>DCS's repeated PrefetchVirtualMemory calls (edterrain/edCore workers): leave them, only count them, or
/// skip a range already prefetched within the window ([Prefetch] in DcsQvCull.ini, an engine optimization).</summary>
public enum PrefetchFix { Off, Observe, Skip }

public sealed record VrProfile
{
    public int SchemaVersion { get; init; } = 1;
    public string Id { get; init; } = "pimax-baseline";
    public string Name { get; init; } = "Pimax · original configuration";
    public RuntimeKind Runtime { get; init; } = RuntimeKind.Pimax;
    /// <summary>Optimizations only, for another headset or a monitor: DCS runs exactly as the user set it up (VR or not,
    /// with the OpenXR runtime and layers of the PC), and the app adds only the features that are not VR (Engine
    /// Optimizations with the prefetch fix, CPU Boost, Free VRAM before flight): no runtime, layer or VR option of its
    /// own. The VR settings stay in the profile for the next Pimax or Sboys flight; <see cref="ForLaunch"/> is what
    /// runs.</summary>
    public bool Desktop { get; init; }
    public QuadProvider QuadViews { get; init; } = QuadProvider.PimaxNative;
    public FrameGeneration FrameGen { get; init; }
    /// <summary>Output frames per rendered frame: 2 (one generated frame), 3 (OFXR fork triple_frame_gen: two generated
    /// frames, renders at a third of the refresh rate, more latency and more artifacts in sideways motion) or
    /// <see cref="FrameGenAuto"/> (OFXR picks 2 while DCS can hold half the refresh rate, 3 otherwise).</summary>
    public int FrameGenFactor { get; init; } = FrameGenAuto;
    public const int FrameGenAuto = 0;
    /// <summary>OFXR deep_pipeline ("Prefer FPS over latency" in its tray): one more frame in flight ahead of the headset,
    /// which absorbs DCS frame-time spikes at the cost of about one refresh period of latency (11 ms at 90 Hz). Applies
    /// to 2×, 3× and Auto (fork patch 0007); in 3× the extra slot is taken only under sustained lateness.</summary>
    public bool FrameGenDeepPipeline { get; init; } = true;
    /// <summary>OFXR smooth_cursor (fork patch 0010): generated frames find DCS's mouse cursor in the two real frames
    /// and draw it at its in-between position, so it moves at the headset's rate instead of fading between two places.
    /// Only the generated frames change; where a click lands does not. Off by default until flown.</summary>
    public bool SmoothCursor { get; init; }
    public GazeMode Gaze { get; init; } = GazeMode.EyeTracked;
    /// <summary>Foveated Super Resolution (Cheeky's own foveated DLSS SR), for stereo without Quad Views only. With Quad Views
    /// it would make Cheeky redo DCS's DLSS of the focus views privately instead of building DLSS 5 on it, so it is ignored
    /// there (<see cref="UsesFoveatedDlss"/>) and cleared by <see cref="ProfileValidation.ResolveFeatures"/>.</summary>
    public bool FoveatedDlss { get; init; }
    public bool NeuralRendering { get; init; }
    public bool QuadFocusAdapter { get; init; }
    // Used when the focus area source is this profile (or Pimax Play's settings cannot be read). The defaults are
    // Pimax Play's Fine values on a Crystal Super (0.66 / 0.20 per side shares left out, gaze scale 0.25, periphery
    // 0.1919) converted the same way PimaxFovea converts them: 0.57 x 0.34 focus at 1.125x, periphery 0.19.
    public double FoveaWidth { get; init; } = 0.57;
    public double FoveaHeight { get; init; } = 0.34;
    public double PeripheralScale { get; init; } = 0.19;
    public double QuadFocusScale { get; init; } = 1.125;
    public double QuadSharpening { get; init; } = 0.7;
    /// <summary>Bundled Quad Views: CAS on the periphery at its own resolution before Quad Views enlarges it, restoring
    /// the local contrast the enlargement takes (dcsvr_periphery_sharpen; 0 = off).</summary>
    public double QuadPeripheryContrast { get; init; } = 0.3;
    public double QuadEdgeBlend { get; init; } = 0.2;
    /// <summary>Quad Views focus area as the circle inscribed in the focus view, fading out radially over QuadEdgeBlend down
    /// to nothing (focus_view_shape=2); off keeps a rectangle with slightly rounded corners (focus_view_shape=8) that fades
    /// out the same way, instead of upstream's rectangle whose fade stops at 50 % and leaves a visible step.</summary>
    public bool QuadRoundFocus { get; init; } = true;
    /// <summary>Bundled Quad Views: the focus sharpening fades out over the same edge band as the focus area, so detail
    /// steps down gradually into the periphery instead of ending at the edge (dcsvr_sharpen_taper).</summary>
    public bool QuadSharpenTaper { get; init; } = true;
    /// <summary>DLSS 5 over the whole focus area: its look (tone, colour, structure) fades out toward the focus edge with
    /// the focus area itself, so the focus area is not a disc of a different look. Cheeky's NR feather over the whole
    /// focus view (NrFoveated with NrWidth=1), as wide as the Quad Views edge band allows (Cheeky caps it at 0.3).</summary>
    public bool QuadNeuralEdgeFade { get; init; } = true;
    /// <summary>Eye-tracked bundled Quad Views: during a saccade the focus area reaches ahead along the eye's motion by the
    /// display latency (<see cref="QuadSaccadeLeadMs"/>), so the eye lands in the sharp area even with frame generation.
    /// No GPU cost: the focus view keeps its pixel count (native/quadviews/dcsvr_gaze.h).</summary>
    public bool QuadSaccadeLead { get; init; } = true;
    /// <summary>Eye-tracked bundled Quad Views: the focus stays put while the gaze moves less than 0.5 degrees, so tracker
    /// noise does not make its edge shimmer. Real eye movements are followed in the same frame.</summary>
    public bool QuadGazeStabilize { get; init; } = true;
    /// <summary>The focus area's reach during a saccade, as gaze-to-headset latency: about one rendered frame without
    /// frame generation, more with it (it waits for the next rendered frame), most in 3X.</summary>
    public int QuadSaccadeLeadMs => FrameGen == FrameGeneration.Off ? 25 : FrameGenFactor == 2 ? 55 : 85;
    /// <summary>Quad Views turbo: DCS starts the next frame while the runtime still holds the previous one. Quad Views itself
    /// enables it on most runtimes and disables it on SteamVR; off by default until measured in the headset.</summary>
    public bool QuadTurbo { get; init; }
    public double NeuralWorkingScale { get; init; } = 0.75;
    public double NeuralIntensity { get; init; } = 1;
    public bool NeuralBeforeUpscaling { get; init; } = true;
    public double NeuralLocalTone { get; init; } = 1;
    public double NeuralLocalStructure { get; init; } = 1;
    public double NeuralSkinStructure { get; init; } = 1;
    public bool NeuralAutomaticMask { get; init; }
    public bool NeuralUiCorrection { get; init; }
    public double NeuralColorStrength { get; init; } = 1;
    public double NeuralTransferStrength { get; init; } = 1;
    public double NeuralPaperWhiteScale { get; init; } = 1;
    public NeuralDepthMode NeuralDepth { get; init; }
    public NeuralStyle NeuralStyle { get; init; }
    /// <summary>With the Quad Views focus adapter: DLSS 5 covers the central part of each focus view, in percent per axis
    /// (100 = whole focus view, 80, 70 or 50). The NR edge fades out over <see cref="NeuralAreaFeather"/>. One of
    /// <see cref="NeuralAreas"/>.</summary>
    public int NeuralFocusArea { get; init; } = 100;
    public static IReadOnlyList<int> NeuralAreas { get; } = [100, 80, 70, 50];
    /// <summary>Cheeky NrTransitionWidth for a central DLSS 5 area: the fade from DLSS 5 to the plain DLSS image spans
    /// half of this fraction of the focus view on each side (6%), independent of the area size.</summary>
    public const double NeuralAreaFeather = 0.12;
    /// <summary>In-flight DLSS 5 toggle: "Off", "virtual-key:modifiers" (recorded by the key capture field) or a label of
    /// earlier versions (<see cref="NeuralHotkeys.All"/>).</summary>
    public string NeuralToggleKey { get; init; } = NeuralHotkeys.Default;
    /// <summary>Shows or hides the in-headset diagnostic panel (FPS, DLSS 5, framegen); drawn by OFXR, so framegen profiles only.</summary>
    public string DiagnosticOverlayKey { get; init; } = NeuralHotkeys.DiagnosticDefault;
    public bool DiagnosticOverlayAtStart { get; init; }
    /// <summary>OFXR's in-headset VRAM counter (diag_vram in its ini); off by default.</summary>
    public bool DiagnosticVram { get; init; }
    public double NeuralMotionScaleX { get; init; } = 1;
    public double NeuralMotionScaleY { get; init; } = 1;
    public int NvidiaFlowScale { get; init; } = 50;
    public NvidiaFlowPreset FlowPreset { get; init; } = NvidiaFlowPreset.Medium;
    public bool BidirectionalFlow { get; init; }
    public bool DiagnosticRecorder { get; init; } = true;
    /// <summary>OFXR's always-on FPS counter; the diagnostic panel (its own key) shows FPS on demand.</summary>
    public bool ShowOverlay { get; init; }
    public FpsLimitMode FpsLimit { get; init; }
    public double HeadsetRefreshHz { get; init; } = 90;
    /// <summary>The refresh rate is read from the headset software (<see cref="HeadsetRefresh"/>) whenever it can be;
    /// <see cref="HeadsetRefreshHz"/> is then only the fallback. Off: <see cref="HeadsetRefreshHz"/> as set.</summary>
    public bool RefreshFromHeadset { get; init; } = true;
    public double RenderedFpsCap { get; init; } = 45;
    public bool DisableDcsVSync { get; init; }
    /// <summary>Keep the DCS launcher window when launching from the app. Off: the profile sets
    /// miscellaneous.launcher = false (restored with the profile) and DCS goes straight into the game.</summary>
    public bool KeepDcsLauncher { get; init; }
    public string? NeuralRuntimePath { get; init; }
    public string? RuntimeManifestPath { get; init; }
    public string? QuadViewsLayerDirectory { get; init; }
    public TrackingKind Tracking { get; init; }
    /// <summary>New profiles (presets, the checklist, Detect my setup) follow Pimax Play. A saved profile without this
    /// property predates it and always used its own values, so <see cref="JsonData"/> loads it as <see cref="FoveaSource.Profile"/>.</summary>
    public FoveaSource FoveaSource { get; init; } = FoveaSource.PimaxPlay;

    /// <summary>CPU Boost while DCS runs (started with the launch, undone when DCS exits).</summary>
    public bool CpuBoost { get; init; }
    public BoostPriority BoostDcsPriority { get; init; } = BoostPriority.AboveNormal;
    /// <summary>Keep VR runtime and headset services off the CPU cores DCS uses for its main and render threads.</summary>
    public bool BoostMoveVrRuntime { get; init; } = true;
    /// <summary>Move <see cref="BoostBackgroundApps"/> to the slowest cores at below-normal priority.</summary>
    public bool BoostMoveBackgroundApps { get; init; } = true;
    public IReadOnlyList<string> BoostBackgroundApps { get => _boostBackgroundApps; init => _boostBackgroundApps = new(value); }
    private readonly NameList _boostBackgroundApps = new(["msedge", "chrome", "firefox", "brave", "opera", "OneDrive", "Teams", "ms-teams", "Spotify", "steamwebhelper"]);
    /// <summary>Replaced by <see cref="FreeVramApps"/>: a profile saved with this list is loaded with Free VRAM before
    /// flight on (when CPU Boost was on), the same names, ending apps that do not close and no reopening, which is what
    /// the list did. Kept so those profiles still load; always empty after loading.</summary>
    public IReadOnlyList<string> BoostCloseApps { get => _boostCloseApps; init => _boostCloseApps = new(value); }
    private readonly NameList _boostCloseApps = new([]);
    /// <summary>The terrain prefetch fix, an Engine Optimizations option run by the DCS engine optimizations module
    /// ([Prefetch] in DcsQvCull.ini). Named for CPU Boost, where it was a sub-option deployed as bin\dxgi2.dll until
    /// 0.5.4; the JSON name stays so saved profiles keep loading.</summary>
    public PrefetchFix BoostPrefetch { get; init; } = PrefetchFix.Skip;
    /// <summary>Another program's dxgi.dll in DCS's bin (ReShade, an overlay) keeps working with DLSS 5: it is loaded after
    /// the DLSS 5 loader as bin\dxgi2.dll (the loader chains to that file). Off: it is only set aside while applied.</summary>
    public bool KeepOtherDxgi { get; init; } = true;
    /// <summary>Ask for administrator rights (UAC) so services running under other accounts can be moved too.</summary>
    public bool BoostElevated { get; init; }

    /// <summary>Free VRAM before flight: the boost helper closes <see cref="FreeVramApps"/> when DCS starts.</summary>
    public bool FreeVram { get; init; }
    /// <summary>Process names (without .exe, * and ? allowed) closed by <see cref="FreeVram"/>. The defaults are programs
    /// seen holding video memory on a Windows gaming PC; voice chat and recording programs are deliberately absent.</summary>
    public IReadOnlyList<string> FreeVramApps { get => _freeVramApps; init => _freeVramApps = new(value); }
    private readonly NameList _freeVramApps = new(DefaultFreeVramApps);
    public static IReadOnlyList<string> DefaultFreeVramApps { get; } =
        ["NVIDIA Overlay", "msedge", "ChatGPT", "HueSync", "RazerCortex", "RazerAppEngine", "wallpaper64", "wallpaper32"];
    /// <summary>Start the programs Free VRAM closed again, as the signed-in user, when DCS exits.</summary>
    public bool FreeVramReopen { get; init; } = true;
    /// <summary>End a program that is still running 5 s after the close request. Off: it is left running.</summary>
    public bool FreeVramForce { get; init; }
    /// <summary>The profile sets DCS's desktop window to 1280×720, windowed (options.lua, restored with the profile).</summary>
    public bool SmallDcsWindow { get; init; }
    public const int SmallWindowWidth = 1280, SmallWindowHeight = 720;
    /// <summary>The boost helper switches the main monitor to <see cref="FlightDisplayWidth"/> ×
    /// <see cref="FlightDisplayHeight"/> at <see cref="FlightDisplayRefresh"/> Hz while DCS runs; never saved as a Windows setting.</summary>
    public bool LowerMonitor { get; init; }
    public int FlightDisplayWidth { get; init; } = 1920;
    public int FlightDisplayHeight { get; init; } = 1080;
    public int FlightDisplayRefresh { get; init; } = 60;

    /// <summary>DCS engine optimizations (DcsQvCull): a native module loaded by Saved Games\DCS\Scripts\Hooks\DcsQvCull.lua
    /// that removes CPU work from DCS's render thread without changing the image. Nothing in the DCS install is changed.</summary>
    public bool EngineOptimizations { get; init; }
    /// <summary>[Timing] ShaderTimeCache (the module's name; it is the streaming timer cache): the clock read DCS's DX11
    /// backend makes on every texture bind to timestamp texture-streaming use, answered from a value refreshed every
    /// <see cref="EngineTimerRefreshUs"/>. Measured +5 to +6 % FPS.</summary>
    public bool EngineShaderTimeCache { get; init; } = true;
    /// <summary>[Scene] PartitionBoost: culling's per-model work cut into 16 chunks instead of 12. Measured +2 % FPS.</summary>
    public bool EnginePartitionBoost { get; init; } = true;
    /// <summary>[Scene] CostWeights: culling's work split by each object's measured culling cost instead of a fixed
    /// estimate, so the tasks finish together. Measured +2.75 % FPS, p95 −4 %.</summary>
    public bool EngineCostWeights { get; init; } = true;
    /// <summary>[General] Beeps: the module's sound feedback (in-flight switch, test suite start and end).</summary>
    public bool EngineBeeps { get; init; } = true;
    /// <summary>[Model] ShadowInstancing, ShadowBatching and BigModelPages together: identical shadow casters in a
    /// cascade are drawn with one instanced draw, using instanced variants of DCS's own shadow vertex shader compiled in
    /// the background after DCS starts (about 2.5 min, nothing written to the DCS install). Verified texel-identical;
    /// turns itself off on any fault. Measured +2.8 % FPS. Also writes ShadowPlanAsync: the grouping is planned off the
    /// render thread (+1.9 % FPS, texel-identical). Also writes ShadowTextureSkip: shadow draws skip the texture sets
    /// their compiled shaders never read (masks from the same compile; depth-identical).</summary>
    public bool EngineShadowInstancing { get; init; } = true;
    /// <summary>[Model] AllocSlabs: per-thread slabs for the model data allocator (NGModel StructBufferManager), so
    /// culling threads stop contending for it. Measured +6 % FPS, p95 −9 %.</summary>
    public bool EngineModelAllocator { get; init; } = true;
    /// <summary>[Model] FrameHeapSlabs: per-thread blocks for DCS's per-frame memory heap (edCore TFrameMemoryHeap),
    /// falling back to the original near the end of the heap. Measured +1.5 % FPS, culling −9 %.</summary>
    public bool EngineFrameHeap { get; init; } = true;
    /// <summary>[Timing] TaskQueueClock: the frame-start drain of DCS's task queue (unlimited budget only) reads the
    /// cached clock; needs <see cref="EngineShaderTimeCache"/>. Helps while the camera moves (render-thread drain
    /// 2.9 → 1.6 ms per frame in a fast synthetic flight).</summary>
    public bool EngineTaskQueueClock { get; init; } = true;
    /// <summary>[Texture] StreamDedupe: a texture-streaming request repeated for the same texture and size within the
    /// same 1 ms tick is skipped. Measured +2 % FPS.</summary>
    public bool EngineTextureDedupe { get; init; } = true;
    /// <summary>[Effects] SkipSameConstantBuffer: the effect system's SetConstantBuffer is skipped when that buffer is
    /// already set. With <see cref="EnginePlainCounter"/>: measured +3.6 % FPS.</summary>
    public bool EngineEffectBufferSkip { get; init; } = true;
    /// <summary>[D3D] SplitFilter: DCS's renderer sets D3D11 state (shaders, buffers, textures, samplers) it already has
    /// set; the repeat is skipped at DCS's own call sites. Verified exact live (362 frames, 0 mismatches); measured
    /// about CPU −1.2 %, +0.5 to 1 % FPS. Turns itself off after a DCS update.</summary>
    public bool EngineStateFilter { get; init; } = true;
    /// <summary>[Model] ShadowRecorder: the shadow cascades' draws are recorded on 8 worker threads (D3D11 deferred
    /// contexts) instead of DCS's render thread; cascade 1 stays as DCS draws it. Verified texel-identical live;
    /// measured +3 to +5 % FPS, p95 −4 to −7 %, at 4-7 % more total CPU and about 30 MB of memory.</summary>
    public bool EngineShadowRecorder { get; init; } = true;
    /// <summary>[Model] GBufferRecorder: DCS's G-buffer model draws are recorded on worker threads (D3D11 deferred
    /// contexts) in DCS's exact order and replayed by DCS; segments whose textures changed are recorded again. Verified
    /// pixel-identical live; measured +22 to +24 % FPS on its own, at about 12 more worker threads.</summary>
    public bool EngineGBufferRecorder { get; init; } = true;
    /// <summary>[Model] PlainTriangleCounter: a statistics-only triangle counter updated without a locked instruction.</summary>
    public bool EnginePlainCounter { get; init; } = true;
    /// <summary>[Timing] CacheUs: how often the cached time is refreshed, in microseconds.</summary>
    public int EngineTimerRefreshUs { get; init; } = 1000;
    public const int EngineTimerRefreshMin = 250, EngineTimerRefreshMax = 2000;
    /// <summary>[Hotkeys] Toggle: switches the enabled engine optimizations off and back on in flight, to compare
    /// (one beep off, two beeps on). "virtual-key:modifiers" like the other in-flight keys; "Off" disables it.</summary>
    public string EngineToggleKey { get; init; } = NeuralHotkeys.EngineDefault;
    /// <summary>Developer mode ([Dev] in DcsQvCull.ini): the installed loader hot-reloads the payload from
    /// <see cref="EngineDevPayloadPath"/> and the module takes its settings from <see cref="EngineDevIniPath"/>, which
    /// replaces this profile's engine settings entirely. For working on the module; off by default.</summary>
    public bool EngineDevMode { get; init; }
    public string? EngineDevPayloadPath { get; init; }
    public string? EngineDevIniPath { get; init; }
    /// <summary>[General] Diagnostics: measurement hooks always installed instead of only while the test suite runs.</summary>
    public bool EngineDiagnosticHooks { get; init; }

    /// <summary>While DCS runs, the boost helper stops a desktop Tobii eye tracker's services (Tobii Experience and the
    /// Eye Tracker 5/4C runtime), which otherwise hold the Pimax headset's Tobii tracker, and starts them again when DCS
    /// exits (<see cref="TobiiDesktop"/>). Needs administrator rights (UAC at launch).</summary>
    public bool PauseTobiiDesktop { get; init; }

    /// <summary>Pupil shift (native/pupil_shift, an OpenXR layer at the top of the chain): each eye is rendered from where
    /// its pupil is for the current gaze instead of from one fixed eye point, which removes the perspective error on the
    /// cockpit and the image shift against the lens when the eyes turn. The gaze comes from the focus views of a
    /// foveated quad-view configuration, so it needs eye-tracked Quad Views.</summary>
    public bool PupilShift { get; init; }
    /// <summary>Distance from the pupil to the eye's rotation centre (anatomy: about 10 to 11 mm).</summary>
    public double PupilShiftEyeRadiusMm { get; init; } = 10.5;
    /// <summary>Distance of the lens's virtual image. Not published by Pimax; pancake designs for 1.3" micro-OLED
    /// panels sit at 1 to 2 m. Only affects distant objects, and by less than 0.1 degree across that range.</summary>
    public double PupilShiftVirtualImageM { get; init; } = 1.5;
    /// <summary>Turns the correction off and on in flight, to compare (PupilShift.ini Toggle; the panel names it).</summary>
    public string PupilShiftToggleKey { get; init; } = NeuralHotkeys.PupilDefault;
    public const double PupilShiftEyeRadiusMin = 5, PupilShiftEyeRadiusMax = 15, PupilShiftVirtualImageMin = 0.5, PupilShiftVirtualImageMax = 5;
    /// <summary>The pupil shift layer is deployed: only for a VR flight with eye-tracked Quad Views.</summary>
    public bool UsesPupilShift => PupilShift && !Desktop && QuadViews != QuadProvider.None && Gaze == GazeMode.EyeTracked;

    /// <summary>The engine optimizations module runs the prefetch fix ([Prefetch] Mode in DcsQvCull.ini), also in
    /// Optimizations only and when DCS starts from Steam or a shortcut.</summary>
    public bool UsesPrefetchFix => EngineOptimizations && BoostPrefetch != PrefetchFix.Off;
    /// <summary>The boost helper is started with DCS: for CPU Boost, Free VRAM before flight or the monitor mode.</summary>
    public bool UsesBoostHelper => CpuBoost || FreeVram || UsesLowerMonitor || UsesTobiiPause;
    /// <summary>The desktop Tobii pause is for a Pimax next to a desktop tracker, never with <see cref="Desktop"/>.</summary>
    public bool UsesTobiiPause => PauseTobiiDesktop && !Desktop;
    /// <summary>The monitor mode and the small DCS window are for the desktop mirror of a VR flight, never with
    /// <see cref="Desktop"/>, where they would shrink the game itself.</summary>
    public bool UsesLowerMonitor => LowerMonitor && !Desktop;
    public bool UsesSmallDcsWindow => SmallDcsWindow && !Desktop;
    /// <summary>The boost helper starts elevated: CPU Boost's administrator option, or pausing the desktop Tobii services.</summary>
    public bool BoostHelperElevated => (CpuBoost && BoostElevated) || UsesTobiiPause;
    /// <summary>Foveated Super Resolution actually runs: only in stereo, without Quad Views.</summary>
    public bool UsesFoveatedDlss => FoveatedDlss && QuadViews == QuadProvider.None;
    public bool UsesCheeky => UsesFoveatedDlss || NeuralRendering;
    public bool UsesQuadFocus => UsesCheeky && QuadViews == QuadProvider.QuadViewsFoveated && QuadFocusAdapter;
    /// <summary>Central DLSS 5 area actually written to Cheeky: only with the focus adapter and DLSS 5 on.</summary>
    public bool UsesCentralNeuralArea => UsesQuadFocus && NeuralRendering && NeuralFocusArea < 100;
    public bool UsesNeuralEdgeFade => UsesQuadFocus && NeuralRendering && NeuralFocusArea >= 100 && QuadNeuralEdgeFade && QuadEdgeBlend > 0;
    /// <summary>Cheeky NrTransitionWidth for <see cref="UsesNeuralEdgeFade"/>: the Quad Views fade band (2 x the edge
    /// blending, in focus radii), at most Cheeky's 0.3.</summary>
    public double NeuralEdgeFeather => Math.Min(0.3, 2 * QuadEdgeBlend);
    /// <summary>What a launch runs: with <see cref="Desktop"/> (Optimizations only) the VR features are off, whatever the
    /// profile keeps for the next VR flight; otherwise the profile itself.</summary>
    public VrProfile ForLaunch() => Desktop
        ? this with { QuadViews = QuadProvider.None, NeuralRendering = false, FoveatedDlss = false, FrameGen = FrameGeneration.Off, QuadFocusAdapter = false, PauseTobiiDesktop = false }
        : this;
    public override string ToString() => Name;
}

/// <summary>A read-only list of names that compares by its items, so profiles holding equal lists stay equal records
/// (after a JSON round trip, a copy or a fresh default).</summary>
public sealed class NameList(IEnumerable<string>? names) : IReadOnlyList<string>, IEquatable<NameList>
{
    private readonly string[] _names = names?.ToArray() ?? [];
    public string this[int index] => _names[index];
    public int Count => _names.Length;
    public IEnumerator<string> GetEnumerator() => ((IEnumerable<string>)_names).GetEnumerator();
    System.Collections.IEnumerator System.Collections.IEnumerable.GetEnumerator() => GetEnumerator();
    public bool Equals(NameList? other) => other is not null && _names.SequenceEqual(other._names, StringComparer.Ordinal);
    public override bool Equals(object? obj) => obj is NameList other && Equals(other);
    public override int GetHashCode() { var hash = new HashCode(); foreach (var name in _names) hash.Add(name, StringComparer.Ordinal); return hash.ToHashCode(); }
}
public sealed record ValidationIssue(string Code, IssueSeverity Severity, string Message);
/// <param name="ExcludedEntries">Archive entries never extracted. Cheeky's version.dll fallback loader is excluded:
/// Microsoft Defender flags it (Trojan:Win32/Posilod.CA!cl, cloud heuristic) and dxgi.dll alone is sufficient.</param>
public sealed record ComponentPackage(string Id, string Version, string ArchiveName, string Url, string Sha256, string License, string SourceUrl, IReadOnlyList<string>? ExcludedEntries = null);
public sealed record FileFact(string Path, bool Exists, string? Version, string? Sha256);
public sealed record LayerFact(string Scope, string ManifestPath, bool Enabled, string? Name, string? LibraryPath, bool LibraryExists);
public sealed record DriverFact(string Name, string Directory, bool LibraryExists, bool Blocked, string? Version = null);
public sealed record InventorySnapshot
{
    public DateTimeOffset CapturedAt { get; init; } = DateTimeOffset.UtcNow;
    public string? DcsDirectory { get; init; }
    public string? DcsExecutable { get; init; }
    public string? OptionsPath { get; init; }
    public string? ActiveRuntime { get; init; }
    public string? PimaxRuntime { get; init; }
    public string? SteamVrRuntime { get; init; }
    public string? PimaxVersion { get; init; }
    public string? SboysDirectory { get; init; }
    public string? SboysConfigPath { get; init; }
    public bool DcsRunning { get; init; }
    public TrackingKind Tracking { get; init; }
    public IReadOnlyList<FileFact> Files { get; init; } = [];
    public IReadOnlyList<LayerFact> Layers { get; init; } = [];
    public IReadOnlyList<DriverFact> Drivers { get; init; } = [];
    public IReadOnlyDictionary<string, string> DcsSettings { get; init; } = new Dictionary<string, string>();
    public IReadOnlyList<string> Observations { get; init; } = [];
    public IReadOnlyList<string> LimiterProcesses { get; init; } = [];
    public string? AutoexecPath { get; init; }
    public string? AutoexecMaxFps { get; init; }
    public bool AutoexecPacingUnknown { get; init; }
}

public static class JsonData
{
    public static JsonSerializerOptions Options { get; } = Create(withProfileConverter: true);

    private static JsonSerializerOptions Create(bool withProfileConverter)
    {
        var options = new JsonSerializerOptions
        {
            WriteIndented = true,
            PropertyNamingPolicy = JsonNamingPolicy.CamelCase,
            UnmappedMemberHandling = JsonUnmappedMemberHandling.Disallow,
            Converters = { new JsonStringEnumConverter(allowIntegerValues: false) }
        };
        if (withProfileConverter) options.Converters.Add(new VrProfileConverter(Create(withProfileConverter: false)));
        return options;
    }

    /// <summary>Settings earlier versions saved and this one no longer has: the self-reported limiter, its FPS and the
    /// runtime motion smoothing (preflight lists them as checks you do yourself), the experimental consent and the Sboys
    /// driver folder (the registered driver is used). Dropped when a profile loads, in any casing.</summary>
    private static readonly HashSet<string> ObsoleteProfileKeys = new(["externalLimiter", "externalLimiterFps", "runtimeReprojection",
        "experimentalAcknowledged", "sboysDriverDirectory"], StringComparer.OrdinalIgnoreCase);

    /// <summary>Reads a profile with the plain options, without the <see cref="ObsoleteProfileKeys"/> (unknown keys are
    /// refused), then marks a profile saved without "foveaSource" (written before the setting existed) as using its own
    /// focus values instead of silently switching it to Pimax Play.</summary>
    private sealed class VrProfileConverter(JsonSerializerOptions plain) : JsonConverter<VrProfile>
    {
        public override VrProfile Read(ref Utf8JsonReader reader, Type typeToConvert, JsonSerializerOptions options)
        {
            using var document = JsonDocument.ParseValue(ref reader);
            var profile = (document.RootElement.ValueKind == JsonValueKind.Object && document.RootElement.EnumerateObject().Any(p => ObsoleteProfileKeys.Contains(p.Name))
                ? JsonSerializer.Deserialize<VrProfile>(WithoutObsoleteKeys(document.RootElement), plain)
                : document.Deserialize<VrProfile>(plain)) ?? throw new JsonException("The profile is empty.");
            if (document.RootElement.ValueKind == JsonValueKind.Object && !document.RootElement.TryGetProperty("foveaSource", out _))
                profile = profile with { FoveaSource = FoveaSource.Profile };
            return MigrateCloseApps(profile, document.RootElement.ValueKind == JsonValueKind.Object && document.RootElement.TryGetProperty("freeVram", out _));
        }
        public override void Write(Utf8JsonWriter writer, VrProfile value, JsonSerializerOptions options) => JsonSerializer.Serialize(writer, value, plain);

        private static byte[] WithoutObsoleteKeys(JsonElement profile)
        {
            using var stream = new MemoryStream();
            using (var writer = new Utf8JsonWriter(stream))
            {
                writer.WriteStartObject();
                foreach (var property in profile.EnumerateObject())
                    if (!ObsoleteProfileKeys.Contains(property.Name)) property.WriteTo(writer);
                writer.WriteEndObject();
            }
            return stream.ToArray();
        }
    }

    /// <summary>The CPU Boost "Close when DCS starts" list became Free VRAM before flight. A profile saved before
    /// (<paramref name="hasFreeVram"/> false) keeps doing what it did: the same names are closed when CPU Boost was on,
    /// apps that do not close are ended, nothing is reopened. Names left in the old list of a newer profile join the
    /// Free VRAM list. The old list is always empty afterwards.</summary>
    public static VrProfile MigrateCloseApps(VrProfile profile, bool hasFreeVram)
    {
        var old = profile.BoostCloseApps.Where(n => !string.IsNullOrWhiteSpace(n)).Select(n => n.Trim()).ToArray();
        if (old.Length == 0) return profile.BoostCloseApps.Count == 0 ? profile : profile with { BoostCloseApps = [] };
        if (!hasFreeVram)
            return profile with { FreeVram = profile.CpuBoost, FreeVramApps = old.Distinct(StringComparer.OrdinalIgnoreCase).ToArray(), FreeVramForce = true, FreeVramReopen = false, BoostCloseApps = [] };
        return profile with { FreeVramApps = profile.FreeVramApps.Concat(old).Distinct(StringComparer.OrdinalIgnoreCase).ToArray(), BoostCloseApps = [] };
    }

    public static string Serialize<T>(T value) => JsonSerializer.Serialize(value, Options);
    public static T Deserialize<T>(string text) => JsonSerializer.Deserialize<T>(text, Options)
        ?? throw new InvalidDataException("The JSON document is empty.");
}

public static class ProfilePresets
{
    public static IReadOnlyList<VrProfile> All { get; } = [
        new(),
        new() { Id = "pimax-framegen", Name = "Pimax · Quad Views + framegen", FrameGen = FrameGeneration.Nvidia },
        new() { Id = "sboys-framegen", Name = "Sboys · Quad Views + framegen", Runtime = RuntimeKind.SboysSteamVr, QuadViews = QuadProvider.QuadViewsFoveated, FrameGen = FrameGeneration.Nvidia },
        new() { Id = "pimax-neural", Name = "Pimax · DLSS 5 + framegen (no Quad Views)", QuadViews = QuadProvider.None, FrameGen = FrameGeneration.Nvidia, FoveatedDlss = true, NeuralRendering = true },
        new() { Id = "sboys-neural", Name = "Sboys · DLSS 5 + framegen (no Quad Views)", Runtime = RuntimeKind.SboysSteamVr, QuadViews = QuadProvider.None, FrameGen = FrameGeneration.Nvidia, FoveatedDlss = true, NeuralRendering = true },
        new() { Id = "pimax-combined", Name = "Pimax · DLSS 5 + framegen + Quad Views", QuadViews = QuadProvider.QuadViewsFoveated, QuadFocusAdapter = true, FrameGen = FrameGeneration.Nvidia, NeuralRendering = true },
        new() { Id = "sboys-combined", Name = "Sboys · DLSS 5 + framegen + Quad Views", Runtime = RuntimeKind.SboysSteamVr, QuadViews = QuadProvider.QuadViewsFoveated, QuadFocusAdapter = true, FrameGen = FrameGeneration.Nvidia, NeuralRendering = true }
    ];
}
