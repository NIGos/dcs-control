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
/// <summary>Depth boost's curve from the distance looked at to the eye-distance scale (docs/FIXATION_DEPTH.md, "Modes").
/// Realistic: normal depth up to <see cref="VrProfile.DepthBoostFarStartM"/> (150 m), where a headset's stereo is
/// already as good as real stereo, then more up to <see cref="VrProfile.DepthBoostFarFullM"/> (400 m), where real stereo
/// still works and the headset's pixels no longer resolve it. Enhanced: more depth from 3 m, the maximum from 50 m;
/// not realistic (distant objects look nearer and the world smaller).</summary>
public enum DepthBoostMode { Realistic, Enhanced }
/// <summary>Far depth's strength (docs/FAR_DEPTH.md): how much depth is added to distant scenery.</summary>
public enum FarDepthPreset { Subtle, Default, Strong }
/// <summary>The eye distance DCS renders with: the headset's own (measured or set in Pimax Play / SteamVR), DCS's custom
/// IPD from its VR options, or a value set here.</summary>
public enum EyeDistanceSource { Headset, DcsCustom, Manual }
/// <summary>Where the bundled Quad Views focus area comes from: Pimax Play's own Quad Views settings, read (never
/// written) at preview, apply and launch, or the values stored in the profile.</summary>
public enum FoveaSource { PimaxPlay, Profile }
public enum BoostPriority { Normal, AboveNormal, High }
/// <summary>The engine module's color modes ([Color] Mode in DcsQvCull.ini, docs/USER_GUIDE.md "Visual realism"): Off is
/// DCS's own image; Fix corrects DCS's VR color faults (lifted blacks, the left eye's exposure seam) and otherwise looks
/// like DCS; Enhanced adds a filmic tone curve, richer color, golden-hour warmth, eye-like adaptation, night vision and
/// glare.</summary>
public enum ColorMode { Off, Fix, Enhanced }
/// <summary>G vision's relation to DCS's own G-EFFECT ([VrFx] GVisionMode): Auto follows DCS's option; Complement leaves
/// the darkening to DCS and adds the eye-centred tunnel and color loss; Full does all of it (for G-EFFECT None).</summary>
public enum GVisionMode { Auto, Complement, Full }
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
    /// <summary>OFXR pipelined_deep_hold (fork patch 0050): with the smoothness buffer, DCS's split-thread presenter also
    /// gets the held slot, so a DCS frame up to about a refresh late is absorbed (R92 live: late first synthetics 32/min
    /// without it, about 2/min with it). On with the smoothness buffer, whose ~11 ms it costs; profile file only (no control: it follows the Smoothness buffer).</summary>
    public bool FrameGenPipelinedDeepHold { get; init; } = true;
    /// <summary>OFXR async_arm (fork patch 0050): frame generation for a new swapchain (leaving the F10 map) is prepared on
    /// a worker thread; frames pass through meanwhile. Off until flown; "Prepare in the background" in the Framegen page's Advanced fold.</summary>
    public bool FrameGenAsyncArm { get; init; }
    /// <summary>OFXR adaptive_cadence (fork patch 0032): frame generation only when DCS falls below the refresh rate (a
    /// full-rate 1X otherwise); DCS's frame limit must then be the refresh rate. Profile file only, off until flown.</summary>
    public bool FrameGenAdaptiveCadence { get; init; }
    /// <summary>HUD symbology for frame generation (engine module R95, OFXR fork patch 0049): the HUD and helmet symbols
    /// are taken out of the real frames, the generated frames are made from the world alone and the symbols put back, so
    /// they stop smearing. Needs Depth boost's dual camera (the overlay render point). "Sharp HUD" on the Framegen page, off by default.</summary>
    public bool FrameGenHudSymbology { get; init; }
    public bool UsesHudSymbology => FrameGenHudSymbology && FrameGen != FrameGeneration.Off && !Desktop && UsesDepthBoost;
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
    /// <summary>DLSS preset for the focus views only (Cheeky CenterPreset): 0 keeps DCS's own preset; 5 = E, 11 = K,
    /// 12 = L, 13 = M. The periphery keeps DCS's preset either way.</summary>
    public int FocusDlssPreset { get; init; }
    public static IReadOnlyList<int> FocusDlssPresets { get; } = [0, 5, 11, 12, 13];
    public static IReadOnlyList<int> NeuralAreas { get; } = [100, 90, 80, 70, 50];
    /// <summary>Cheeky NrTransitionWidth for a central DLSS 5 area: the fade from DLSS 5 to the plain DLSS image spans
    /// half of this fraction of the focus view on each side (6%), independent of the area size.</summary>
    public const double NeuralAreaFeather = 0.12;
    /// <summary>In-flight DLSS 5 toggle: "Off", "virtual-key:modifiers" (recorded by the key capture field) or a label of
    /// earlier versions (<see cref="NeuralHotkeys.All"/>).</summary>
    public string NeuralToggleKey { get; init; } = NeuralHotkeys.Default;
    /// <summary>Shows or hides the in-headset diagnostic panel (FPS, DLSS 5, framegen); drawn by OFXR, so framegen profiles only.</summary>
    public string DiagnosticOverlayKey { get; init; } = NeuralHotkeys.DiagnosticDefault;
    public bool DiagnosticOverlayAtStart { get; init; }
    /// <summary>The in-headset panel without frame generation: OFXR is loaded only to draw it (no generation, no D3D11
    /// bridge, DCS's frames pass straight through). With frame generation the panel is always there.</summary>
    public bool HeadsetPanel { get; init; }
    /// <summary>The in-headset panel can be shown in this flight (its key works).</summary>
    /// Without frame generation OFXR is loaded for the panel only when it can be shown: its key is set, or it is visible at start.
    public bool UsesHeadsetPanel => !Desktop && (FrameGen != FrameGeneration.Off || (HeadsetPanel && (DiagnosticOverlayAtStart || (NeuralHotkeys.TryParse(DiagnosticOverlayKey ?? NeuralHotkeys.DiagnosticDefault, out var key) && !key.IsOff))));
    /// <summary>OFXR's in-headset VRAM counter (diag_vram in its ini); off by default.</summary>
    public bool DiagnosticVram { get; init; }
    public double NeuralMotionScaleX { get; init; } = 1;
    public double NeuralMotionScaleY { get; init; } = 1;
    public int NvidiaFlowScale { get; init; } = 50;
    public NvidiaFlowPreset FlowPreset { get; init; } = NvidiaFlowPreset.Medium;
    public bool BidirectionalFlow { get; init; }
    public bool DiagnosticRecorder { get; init; } = true;
    /// <summary>OFXR's per-frame records (frame_records, fork patch 0048) for hitch analysis; "Per-frame records" in Frame generation diagnostics (Framegen page), off by
    /// default. On, the flight log holds 1 GB before it rotates.</summary>
    public bool DiagnosticFrameRecords { get; init; }
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
    /// <summary>[Shadows] CascadeCache: the far shadow cascades (about 70 to 1500 m) are drawn every few frames and reused
    /// in between. Measured −0.72 ms of GPU, 0.16 % of shadow texels differ; distant tree shadows update about 11 times a
    /// second, so their wind sway can look stepped. Off by default; forced off with the engine switched off or in the
    /// Perception-only mode.</summary>
    public bool EngineShadowCache { get; init; }
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
    /// <summary>The pupil correction runs: only for a VR flight with eye-tracked Quad Views.</summary>
    public bool UsesPupilShift => PupilShift && !Desktop && QuadViews != QuadProvider.None && Gaze == GazeMode.EyeTracked;

    /// <summary>Periphery stability (bundled Quad Views; native/quadviews/dcsvr_perception.h, docs/PERCEPTION.md): a
    /// temporal low-pass on the periphery outside the focus area, against the shimmer its enlargement makes visible.
    /// The history follows the head's rotation and is clipped to the current neighbourhood against trails; saccades
    /// and gaps start it over. Works with a fixed or an eye-tracked focus.</summary>
    public bool PeripheryStability { get; init; }
    /// <summary>0 to 1: the filter's time constant as a share of <see cref="PeripheryStabilityMaxTauMs"/> (0.5 = 40 ms).
    /// More steadies more; on content that moves on its own (outside world while the aircraft rolls) it can leave a
    /// faint trail.</summary>
    public double PeripheryStabilityStrength { get; init; } = 0.5;
    public const double PeripheryStabilityMaxTauMs = 80;
    /// <summary>The pass runs: a VR flight with the bundled Quad Views (dcsvr_temporal); Pimax's own quad views have no
    /// such pass.</summary>
    public bool UsesPeripheryStability => PeripheryStability && !Desktop && UsesBundledQuadViews;
    /// <summary>The bundled Quad Views provider (no alternative provider folder): only it has DCS Control's passes.</summary>
    public bool UsesBundledQuadViews => QuadViews == QuadProvider.QuadViewsFoveated && string.IsNullOrWhiteSpace(QuadViewsLayerDirectory);
    /// <summary>Far depth (docs/FAR_DEPTH.md): more depth on distant scenery (about 50 m to 2 km), the cockpit exact. The
    /// bundled Quad Views shifts each eye's image with the depth DCS drew, which the engine module hands it. Off by default.</summary>
    public bool FarDepth { get; init; }
    public FarDepthPreset FarDepthPreset { get; init; } = FarDepthPreset.Default;
    /// <summary>Turns Far depth off and on in flight, to compare (bundled Quad Views' dcsvr_far_depth_toggle).</summary>
    public string FarDepthToggleKey { get; init; } = NeuralHotkeys.FarDepthDefault;
    /// <summary>It runs: a VR flight with the bundled Quad Views and Engine Optimizations (the depth comes from the engine module).</summary>
    public bool UsesFarDepth => FarDepth && !Desktop && UsesBundledQuadViews && EngineOptimizations;
    /// <summary>Depth boost (docs/PERCEPTION.md, docs/FIXATION_DEPTH.md), the dual camera: the world outside is rendered
    /// at a larger eye distance (<see cref="DepthBoostMode"/>), following where the eyes look and changing slowly, while
    /// the engine module draws the cockpit at the true eye distance (DcsQvCull.ini [Perception] CockpitTrueIpd=1). The
    /// fixation depth comes from DCS's depth buffer at the gaze (the engine module; with Engine Optimizations off it runs
    /// in its Perception-only mode) and from the two eyes' vergence, in the pupil shift layer. Off by default.</summary>
    public bool DepthBoost { get; init; }
    /// <summary>With Depth boost, the own aircraft's exterior seen from the cockpit (a helicopter's other crew station,
    /// a two-seater's front seat) is drawn at the true eye distance too ([Perception] CockpitTrueIpdOwnship=2); without
    /// it, it sits closer than it is. On by default.</summary>
    public bool OwnAircraftTrueDepth { get; init; } = true;
    /// <summary>Realistic (default) or Enhanced; each has its own strength.</summary>
    public DepthBoostMode DepthBoostMode { get; init; } = DepthBoostMode.Realistic;
    /// <summary>Enhanced's render IPD scale while fixating far (from 50 m): 1.4 renders a 64 mm IPD as 89.6 mm.</summary>
    public double DepthBoostMax { get; init; } = 1.4;
    /// <summary>Realistic's render IPD scale from <see cref="DepthBoostFarFullM"/> on: 1.25 renders 64 mm as 80 mm.</summary>
    public double DepthBoostRealisticMax { get; init; } = 1.25;
    /// <summary>Realistic: normal depth up to this distance looked at (metres).</summary>
    public double DepthBoostFarStartM { get; init; } = 150;
    /// <summary>Realistic: the full strength from this distance looked at (metres); a log ramp between.</summary>
    public double DepthBoostFarFullM { get; init; } = 400;
    /// <summary>A fixation nearer than this is the cockpit (normal IPD); beyond twice this it is far.</summary>
    public double DepthBoostNearM { get; init; } = 1.5;
    /// <summary>Turns Depth boost off and on in flight ([DepthBoost] Toggle; the panel names it).</summary>
    public string DepthBoostToggleKey { get; init; } = NeuralHotkeys.DepthBoostDefault;
    public const double DepthBoostMaxMin = 1, DepthBoostMaxMax = 2, DepthBoostNearMin = 0.5, DepthBoostNearMax = 5;
    public const double DepthBoostRealisticMaxMax = 1.5, DepthBoostFarStartMin = 50, DepthBoostFarStartMax = 1000, DepthBoostFarFullMax = 3000;
    /// <summary>Enhanced's fixed curve: normal depth at 3 m, the full strength from 50 m.</summary>
    public const double DepthBoostEnhancedStartM = 3, DepthBoostEnhancedFullM = 50;
    /// <summary>The strength of the selected mode.</summary>
    public double DepthBoostActiveMax => DepthBoostMode == DepthBoostMode.Enhanced ? DepthBoostMax : DepthBoostRealisticMax;
    /// <summary>Depth boost runs: like Pupil shift, only for a VR flight with eye-tracked Quad Views.</summary>
    public bool UsesDepthBoost => DepthBoost && !Desktop && QuadViews != QuadProvider.None && Gaze == GazeMode.EyeTracked;
    /// <summary>The pupil shift layer (it carries Pupil shift and Depth boost, and publishes the gaze for Visual realism)
    /// is deployed when any of them runs.</summary>
    public bool UsesPerceptionLayer => UsesPupilShift || UsesDepthBoost || UsesGazePublish;
    /// <summary>The layer moves the eyes (Pupil shift or Depth boost), so it takes over DCS's custom IPD. Publishing the
    /// gaze alone moves nothing and leaves DCS's IPD as it is.</summary>
    public bool UsesLayerEyeDistance => UsesPupilShift || UsesDepthBoost;
    /// <summary>The engine module measures the scene depth at the gaze (DcsQvCull.ini [Perception] FixationDepth=1)
    /// whenever Depth boost runs, with or without the engine optimizations.</summary>
    public bool UsesFixationDepth => UsesDepthBoost;
    /// <summary>The engine module is installed: for Engine Optimizations, or for Depth boost's dual camera and scene
    /// depth or a Visual realism feature alone, in its Perception-only mode ([General] PerceptionOnly=1: none of the
    /// engine optimizations, no test suite or engine keys).</summary>
    public bool UsesEngineModule => EngineOptimizations || UsesDepthBoost || UsesVisualRealism;
    /// <summary>The eye distance DCS renders with. Headset (default): the IPD the headset measured or was set to, the
    /// realistic one; DCS's custom IPD is turned off while the profile is applied. DcsCustom: DCS's own custom IPD as it
    /// is (the pupil shift layer applies it itself when it runs). Manual: <see cref="EyeDistanceMm"/>. VR flights only;
    /// Back to stock DCS puts DCS's options back.</summary>
    public EyeDistanceSource EyeDistance { get; init; } = EyeDistanceSource.Headset;
    /// <summary>Manual's eye distance in millimetres.</summary>
    public double EyeDistanceMm { get; init; } = 64;
    public const double EyeDistanceMin = 50, EyeDistanceMax = 80;

    // Visual realism (the engine module's [Color], [ColorFix], [ColorEnhanced] and [VrFx], re-read by DCS about every
    // second). Every default is the module's own, and everything is off: a profile without these properties loads as off.
    /// <summary>[Color] Mode. Off by default.</summary>
    public ColorMode ColorMode { get; init; }
    /// <summary>[Color] ModeKey: cycles Off, Fix and Enhanced in flight (1, 2 or 3 beeps). Off by default.</summary>
    public string ColorModeKey { get; init; } = NeuralHotkeys.ColorModeDefault;
    /// <summary>[ColorFix]: exposure offset (EV), extra exposure at night only (EV), scene-linear saturation, golden-hour
    /// warmth (0 to 1) and the eye-like adaptation's time constants towards bright and dark (s; 0 = DCS's own only).</summary>
    public double ColorFixExposureEv { get; init; }
    public double ColorFixNightEv { get; init; }
    public double ColorFixSaturation { get; init; } = 1;
    public double ColorFixWarmth { get; init; }
    public double ColorFixAdaptBright { get; init; }
    public double ColorFixAdaptDark { get; init; }
    /// <summary>[ColorEnhanced]: the same keys with Enhanced's own defaults.</summary>
    public double ColorEnhancedExposureEv { get; init; }
    public double ColorEnhancedNightEv { get; init; }
    public double ColorEnhancedSaturation { get; init; } = 1.15;
    public double ColorEnhancedWarmth { get; init; } = 0.35;
    public double ColorEnhancedAdaptBright { get; init; } = 0.4;
    public double ColorEnhancedAdaptDark { get; init; } = 1.5;
    /// <summary>[ColorEnhanced] NightVision (Enhanced only): at night dim areas lose color and turn blue-grey, as rod vision
    /// does; lights and lit symbology keep their color. On inside Enhanced by default.</summary>
    public bool NightVision { get; init; } = true;
    public double NightVisionStrength { get; init; } = 0.8;
    public double NightVisionBlue { get; init; } = 0.4;
    public double NightVisionBlur { get; init; } = 0.25;
    /// <summary>Light levels (cd/m²) of full night and full day vision; day at least 4 times night.</summary>
    public double NightVisionScotopic { get; init; } = 0.005;
    public double NightVisionPhotopic { get; init; } = 5;
    /// <summary>[ColorEnhanced] Glare (Enhanced only): a soft halo around very bright lights. On inside Enhanced by default.</summary>
    public bool Glare { get; init; } = true;
    public double GlareStrength { get; init; } = 0.35;
    public double GlareThresholdEv { get; init; } = 3.5;
    /// <summary>The wide halo's size in degrees of view.</summary>
    public double GlareRadius { get; init; } = 5;
    public double GlareStreak { get; init; } = 0.3;
    /// <summary>[VrFx] GazeDof: things nearer or farther than where you look go slightly soft. It needs the gaze the pupil
    /// shift layer publishes from the eye-tracked Quad Views focus (<see cref="UsesGazeDof"/>). Off by default.</summary>
    public bool GazeDof { get; init; }
    public double GazeDofStrength { get; init; } = 0.5;
    public double GazeDofMaxArcmin { get; init; } = 6;
    public bool GazeDofSaccadeRelease { get; init; } = true;
    public int GazeDofSettleMs { get; init; } = 120;
    public int GazeDofFadeInMs { get; init; } = 250;
    public double GazeDofToleranceD { get; init; } = 0.3;
    public double GazeDofFoveaGuardDeg { get; init; } = 2;
    /// <summary>0 = estimated from the scene's brightness, else 2 to 8 mm.</summary>
    public double GazeDofPupilMm { get; init; }
    /// <summary>[VrFx] GVision: grey-out, tunnel vision, blackout and red-out under G (the G comes from DCS's export
    /// through the module's Lua hook). Off by default.</summary>
    public bool GVision { get; init; }
    public double GVisionStrength { get; init; } = 1;
    public bool GVisionUseGaze { get; init; } = true;
    public bool GVisionRedout { get; init; } = true;
    /// <summary>The sustained G of a full blackout (7.5: g-suit and straining).</summary>
    public double GVisionTolerance { get; init; } = 7.5;
    public GVisionMode GVisionMode { get; init; }
    /// <summary>The color mode hotkey is set (it can switch to any mode in flight).</summary>
    public bool UsesColorModeKey => !Desktop && NeuralHotkeys.TryParse(ColorModeKey ?? NeuralHotkeys.ColorModeDefault, out var key) && !key.IsOff;
    /// <summary>The module's color modes run, or can be switched on in flight: VR flights only (the module replaces the
    /// tone map of the headset's views).</summary>
    public bool UsesColorMode => !Desktop && (ColorMode != ColorMode.Off || UsesColorModeKey);
    /// <summary>A VR flight with an eye-tracked Quad Views focus: the pupil shift layer has a gaze to work with.</summary>
    public bool UsesEyeTrackedVr => !Desktop && QuadViews != QuadProvider.None && Gaze == GazeMode.EyeTracked;
    /// <summary>Gaze DoF runs: it needs the gaze, which the pupil shift layer publishes for the module (with Depth boost,
    /// or its [PupilShift] PublishGaze for Visual realism alone).</summary>
    public bool UsesGazeDof => GazeDof && UsesEyeTrackedVr;
    public bool UsesGVision => GVision && !Desktop;
    /// <summary>The pupil shift layer publishes the gaze for the engine module ([PupilShift] PublishGaze=1): for Gaze DoF,
    /// and for G vision's tunnel following the eyes.</summary>
    public bool UsesGazePublish => UsesGazeDof || (UsesGVision && GVisionUseGaze && UsesEyeTrackedVr);
    /// <summary>Any Visual realism feature runs, so the engine module is installed for it (<see cref="UsesEngineModule"/>).</summary>
    public bool UsesVisualRealism => UsesColorMode || UsesGazeDof || UsesGVision;

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
    /// blending, in focus radii), at most Cheeky's 0.45 (wide enough for Quad Views' own band).</summary>
    public double NeuralEdgeFeather => Math.Min(0.45, 2 * QuadEdgeBlend);
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
    public string? DcsExecutable { get; init; }
    public string? OptionsPath { get; init; }
    public string? ActiveRuntime { get; init; }
    public string? PimaxRuntime { get; init; }
    public string? SteamVrRuntime { get; init; }
    public string? PimaxVersion { get; init; }
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
    /// <summary>autoexec.cfg sets HUD_MFD_after_DLSS = true (DCS draws the HUD and MFDs after DLSS, so DLSS 5 never
    /// smears them); null when there is no autoexec.cfg.</summary>
    public bool? AutoexecHudAfterDlss { get; init; }
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
    /// driver folder (the registered driver is used), and Depth boost's transition choice and its key (the dual camera
    /// only moves the depth gradually). Dropped when a profile loads, in any casing.</summary>
    private static readonly HashSet<string> ObsoleteProfileKeys = new(["externalLimiter", "externalLimiterFps", "runtimeReprojection",
        "experimentalAcknowledged", "sboysDriverDirectory", "depthBoostTransition", "depthBoostTransitionKey"], StringComparer.OrdinalIgnoreCase);

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
