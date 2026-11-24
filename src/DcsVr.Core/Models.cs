using System.Text.Json;
using System.Text.Json.Serialization;

namespace DcsVr.Core;

/// <summary>The headset route. VirtualDesktop is a Quest (or other standalone headset) streamed by Virtual Desktop
/// through its own OpenXR runtime, VDXR; no eye tracking, so the Quad Views focus is fixed.</summary>
public enum RuntimeKind { Pimax, SboysSteamVr, VirtualDesktop }
public enum QuadProvider { None, PimaxNative, QuadViewsFoveated }
/// <summary>The corner of the view the in-headset panel is drawn in.</summary>
public enum PanelPosition { UpperRight, UpperLeft, LowerRight, LowerLeft }
public enum FrameGeneration { Off, Nvidia, FidelityFx }
public enum GazeMode { Fixed, EyeTracked }
public enum TrackingKind { Unknown, Slam, Lighthouse }
public enum NeuralDepthMode { Game, Normal, Reversed }
/// <summary>DLSS-NR model style (DLSSNR.Style 0..2), the same three modes RenoDX's DLSS 5 add-on offers.</summary>
public enum NeuralStyle { Standard, Natural, Cinematic }
public enum NvidiaFlowPreset { Fast, Medium, Slow }
/// <summary>How readily frame generation only when needed passes DCS's frames through ungenerated
/// (<see cref="VrProfile.FrameGenPassThrough"/>; OFXR <c>cadence_preference</c> 0, 1, 2, fork patch 0070).</summary>
public enum PassThroughPreference { Steady, Balanced, PreferRefresh }
/// <summary>The DLSS preset of the Quad Views periphery (<see cref="VrProfile.PeripheryDlssPreset"/>); Off keeps DCS's.
/// The app offers Off, E and F; K and M (offered until 0.7, slower than E: R122, E 0.138 ms against M 0.261 ms per view)
/// stay so a saved one parses, and a profile that has one loads as E (JsonData).</summary>
public enum PeripheryDlssPreset { Off, E, F, K, M }
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
    /// <summary>Developer options (0.7): shows the settings that are for working on DCS Control, not for flying: Turbo mode,
    /// the Alternative Quad Views provider, DLSS 5's Depth convention and Motion scale X/Y, the Observe choice of the terrain
    /// prefetch fix and the Engine page's Developer options. Not in the interface: set "developerOptions": true in the
    /// profile by hand. Without it those settings load at their safe values (<see cref="ForPilots"/>), whatever a draft or
    /// preset says. Written only when on, so a profile without it is what earlier versions read.</summary>
    [JsonIgnore(Condition = JsonIgnoreCondition.WhenWritingDefault)] public bool DeveloperOptions { get; init; }
    /// <summary>The profile with the developer-only settings at their safe values (<see cref="DeveloperOptions"/> off):
    /// Turbo off, the bundled Quad Views, DLSS 5 depth and motion as the game gives them, the terrain prefetch fix Observe
    /// read as Off, and the Engine page's Developer options at their defaults (Developer mode off, measurement hooks off,
    /// timer refresh 1000 us). The developer paths are kept: they do nothing without Developer mode.</summary>
    public VrProfile ForPilots() => DeveloperOptions ? this : this with
    {
        QuadTurbo = false, QuadViewsLayerDirectory = null, NeuralDepth = NeuralDepthMode.Game, NeuralMotionScaleX = 1, NeuralMotionScaleY = 1,
        BoostPrefetch = BoostPrefetch == PrefetchFix.Observe ? PrefetchFix.Off : BoostPrefetch,
        EngineDevMode = false, EngineDiagnosticHooks = false, EngineTimerRefreshUs = 1000,
    };
    /// <summary>A profile or preset file opened in the interface: unless the pilot's current profile already has Developer
    /// options, it comes without them and without the developer paths (a shared file could otherwise make DCS load any
    /// library). The CLI's --profile keeps them: rigs and developers set the file by hand.</summary>
    public VrProfile ForImport(bool keepDeveloper) => keepDeveloper || !DeveloperOptions && EngineDevPayloadPath is null && EngineDevIniPath is null ? this
        : (this with { DeveloperOptions = false, EngineDevPayloadPath = null, EngineDevIniPath = null }).ForPilots();
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
    /// <summary>OFXR adaptive_cadence (fork patches 0032, 0059, 0060): frame generation only when needed. DCS renders at
    /// the full refresh rate; frames pass through (1X, less latency) while it holds that, and 2X/3X Auto takes over while
    /// it falls behind. Every session and scene starts at 2X (DCS paced at half the refresh rate by OFXR, which the
    /// engine module logs as "capped at 45 fps (90 Hz / 2)"); 1X is entered once the p90 CPU and GPU time per frame have
    /// both stayed at most 80% of the refresh period for 5 s without a missed slot (CadenceConfig, issue #17), and left
    /// above 95% or on missed slots; that is Steady, and <see cref="FrameGenPassThrough"/> widens it. <see cref="RenderedFpsCap"/> plays no part unless the frame limit is Custom.
    /// Needs DCS's frame limit at the refresh rate. "Only when needed" on the Framegen page, on by
    /// default; it applies to Auto only (<see cref="UsesAdaptiveCadence"/>).</summary>
    public bool FrameGenAdaptiveCadence { get; init; } = true;
    /// <summary>When Only when needed stops generating (OFXR cadence_preference, fork patch 0070, issue #17). OFXR
    /// estimates DCS's frame time at the full rate while it runs at half of it, and that estimate reads a fifth to a
    /// quarter high. Steady is the rule of 0.6.0 to 0.6.3: pass through under 80% of a refresh, generate again over 95%
    /// (a PC that just holds the refresh rate never passes through). Balanced, the default (a profile saved before the option loads as Balanced too), tries passing through when
    /// the GPU estimate is within 110% of a refresh (the CPU time within 95%, fork patch 0075) and keeps it while frames arrive on time (generating again after 3 late
    /// episodes in a second or 5 in ten; misses within 250 ms are one episode, fork patch 0071). PreferRefresh tries up
    /// to 125% and generates again after 3 in a second or 10 in ten. A try that fails is not repeated for 30 s, doubling
    /// up to 4 minutes, and after 3 failed in a row (PreferRefresh: 6) not until the scene changes or 2X has run clean for three minutes (0075). "Stop generating" on the Framegen
    /// page; applies at the next DCS start.</summary>
    public PassThroughPreference FrameGenPassThrough { get; init; } = PassThroughPreference.Balanced;
    /// <summary>The share of one refresh period OFXR's frame-time estimate has to stay under before frames pass through
    /// (CadenceConfig::one_enter_fraction per preference).</summary>
    public static double PassThroughShare(PassThroughPreference preference) => preference switch { PassThroughPreference.Balanced => 1.10, PassThroughPreference.PreferRefresh => 1.25, _ => 0.80 };
    /// <summary>Frame generation only when needed is in use: frame generation on, a VR flight, Generated frames Auto. A
    /// pinned 2× or 3× always generates.</summary>
    public bool UsesAdaptiveCadence => FrameGen != FrameGeneration.Off && !Desktop && FrameGenAdaptiveCadence && FrameGenFactor == FrameGenAuto;
    /// <summary>HUD symbology for frame generation (engine module R95, OFXR fork patch 0049): the HUD and helmet symbols
    /// are taken out of the real frames, the generated frames are made from the world alone and the symbols put back, so
    /// they stop smearing. 0.7: no longer needs Depth boost; without its dual camera the engine module hooks the overlay
    /// render point on its own, passing DCS's records through. "Sharp HUD" on the Framegen page, experimental, off by default.</summary>
    public bool FrameGenHudSymbology { get; init; }
    public bool UsesHudSymbology => FrameGenHudSymbology && FrameGen != FrameGeneration.Off && !Desktop;
    /// <summary>Foveated frame generation: the bundled Quad Views submits the focus area and the rest of the view as two
    /// projection layers (dcsvr_two_layers) and OFXR generates each on its own (foveated_layers), instead of generating
    /// the composed full image; OFXR then joins them into one image per eye for the runtime (foveated_single_layer,
    /// patches 0099/0101; a refused join sends that frame as two layers). Measured 2026-11-15, before the join: about half the GPU time per generated pair and 4-5 GB less video
    /// memory. Probed and flown on the Pimax runtime with NVIDIA Optical Flow only; also offered on Quest 3 VD (VDXR takes
    /// projection layers like any OpenXR runtime), not yet flown there. The Smoothness buffer keeps the pilot's choice
    /// (fork patch 0080 tests it with the buffer at fixed 2x, 3x and Auto between them). Experimental, off by default.</summary>
    public bool FrameGenFoveated { get; init; }
    /// <summary>The two halves (Quad Views and OFXR) are written together or not at all: two layers without OFXR's rule
    /// give a wrong image on generated frames.</summary>
    public bool UsesFoveatedFrameGen => FrameGenFoveated && FrameGen == FrameGeneration.Nvidia && !Desktop && (Runtime is RuntimeKind.Pimax or RuntimeKind.VirtualDesktop) && UsesBundledQuadViews;
    /// <summary>OFXR smooth_cursor (fork patch 0010): generated frames find DCS's mouse cursor in the two real frames
    /// and draw it at its in-between position, so it moves at the headset's rate instead of fading between two places.
    /// Only the generated frames change; where a click lands does not. Off by default until flown.</summary>
    public bool SmoothCursor { get; init; }
    /// <summary>OFXR parallel_eye_synthesis=1 (fork patch 0038): each eye's generated frame on a GPU queue of its own, so
    /// one eye no longer waits for the other: about 1 ms sooner, the same pixels (the fork's byte-for-byte test on NVIDIA).
    /// Not flown yet; off by default. "Both eyes at once" on the Framegen page.</summary>
    public bool FrameGenParallelEyes { get; init; }
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
    /// <summary>Quad Views' vertical_focus_offset (eye-tracked) and vertical_fixed_offset (fixed focus), in the layer's own
    /// units: added to the gaze point (or the resting point) in the eye view's normalised coordinates, where the whole
    /// view is 2 high and up is positive. So a negative value moves the focus area down, and -0.1 is 5% of the view's
    /// height. The layer takes -0.5 to 0.5 (a quarter of the view either way); Pimax Play shows the same value as its
    /// Vertical Offset, -100 to 100 (<see cref="PimaxPlayUnits.OffsetFromStored"/>). One of the profile's own focus
    /// values ("Set here"); 0, the default, writes nothing and is the layer as it was (GitHub #24).</summary>
    public double QuadVerticalOffset { get; init; }
    /// <summary>How far from the focus area's centre, up or down, the image is still fully the focus view's, in the units
    /// of <see cref="QuadVerticalOffset"/>: the focus half-height (its share of the view, capped at 0.9 by the layer)
    /// less the edge band, which Quad Views' composition fades over 2 × edge blending of the half-height (0.01 to 0.95).</summary>
    public static double QuadSharpHalfHeight(VrProfile p) => Math.Min(p.FoveaHeight, .9) * (1 - Math.Clamp(2 * p.QuadEdgeBlend, .01, .95));
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
    /// <summary>Bundled Quad Views: inside the focus area's edge band its detail first tapers to the periphery's own
    /// resolution, then fades out (dcsvr_edge_match; round and rounded-rectangle shapes). Without it the band crossfades
    /// two images of very different sharpness, which reads as a ring around where you look. Off by default until seen in
    /// the headset. With <see cref="UsesFoveatedFrameGen"/> the focus layer's output pass does the same taper, with the
    /// periphery's resolution computed from the two views' sizes and fields of view (patch 0021). Part of
    /// <see cref="QuadHideFocusEdge"/>: written only while it is on.</summary>
    public bool QuadEdgeMatch { get; init; }
    /// <summary>Bundled Quad Views: hide the focus edge when the eyes move (on by default). Turns on the five settings
    /// (<see cref="QuadEdgeMatch"/> above and the four below) with their own values; off writes what Quad Views did
    /// before them (no edge match, no glide, no fade-in, the history starts over at a saccade, no band match). Flown by the pilot 2026-11-24 with the defaults below, DLSS 5 Local tone 1.</summary>
    public bool QuadHideFocusEdge { get; init; } = true;
    /// <summary>Eye-tracked bundled Quad Views: small focus moves outside saccades (the deadzone's steps, drift, slow
    /// pursuit) approach the gaze with this time constant instead of in one frame (dcsvr_gaze_glide_ms, Quad Views 0035;
    /// 0 = off). Saccades and moves over 2 degrees still jump (native/quadviews/dcsvr_gaze.h, GlideMaxDeg). Rig
    /// 2026-11-23, 10-degree saccades with DLSS 5: 40 ms halved the border flash; no effect on 20-degree ones.</summary>
    public int QuadGazeGlideMs { get; init; } = 40;
    /// <summary>Bundled Quad Views: a strip the focus area newly covers fades in over this many frames instead of
    /// appearing at once (dcsvr_focus_fade_in, Quad Views 0036; 0 = off, up to 3). Rig: 2 frames halved the 10-degree
    /// saccade border flash with DLSS 5.</summary>
    public double QuadFocusFadeIn { get; init; } = 2;
    /// <summary>Bundled Quad Views: an eye jump does not start the focus area's temporal history over
    /// (dcsvr_temporal_saccade_reset=0, Quad Views 0035). Rig: halved the 10-degree saccade border flash with DLSS 5.</summary>
    public bool QuadKeepHistoryOnSaccade { get; init; } = true;
    /// <summary>Bundled Quad Views: the focus area's brightness is matched to the periphery where the two meet
    /// (dcsvr_band_match strength 0..1, Quad Views 0037; 0 = off). Rig: 1 cut the 10-degree border flash by a third.</summary>
    public double QuadBandMatch { get; init; } = 1;
    /// <summary>With <see cref="QuadBandMatch"/>: the colour is matched too, not only the brightness (dcsvr_band_match_color).</summary>
    public bool QuadBandMatchColor { get; init; }
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
    /// frame generation, more with it (it waits for the next rendered frame), most in 3X. With the saccade tail (no frame
    /// generation only) 40 ms: flown in GitHub PR #26 on a Crystal Super at 72 FPS, 25 ms still showed the periphery at
    /// landings; tracker + one rendered frame + compositor is about 30-45 ms. With frame generation the values flown with
    /// it, with or without the tail: they are the latency, which the tail does not change.</summary>
    public int QuadSaccadeLeadMs => FrameGen == FrameGeneration.Off ? (UsesQuadSaccadeTail ? 40 : 25) : FrameGenFactor == 2 ? 55 : 85;
    /// <summary>Eye-tracked bundled Quad Views with <see cref="QuadSaccadeLead"/>: the focus area
    /// stays ahead of the eyes until they land (dcsvr_saccade_tail; native/quadviews/dcsvr_gaze.h). The lead's saccade
    /// ends below 50 deg/s instead of 120 (saccades decelerate sharply just before they land), its leading edge never
    /// retreats during one saccade, repeated tracker samples are not taken for the eye stopping, the lead is 40 ms instead
    /// of 25 with up to 0.5 instead of 0.35 (with frame generation the lead stays 55 / 85 ms), and nothing is held after
    /// the landing (<see cref="QuadSaccadeHoldMs"/>). Off: the filter exactly as without these keys (tests/quadviews-gaze
    /// checks it). With frame generation tests/quadviews-gaze places the focus once per rendered frame (2X, 3X) with
    /// the 55 / 85 ms leads: the tail never leaves the eye outside the sharp part more often than without it, and widens
    /// about a quarter fewer rendered frames (no hold). Not flown with frame generation yet.</summary>
    public bool QuadSaccadeTail { get; init; } = true;
    [JsonIgnore] public bool UsesQuadSaccadeTail => QuadSaccadeTail && QuadSaccadeLead && Gaze == GazeMode.EyeTracked;
    /// <summary>How long the focus area stays widened after a saccade, fading out (dcsvr_saccade_hold_ms). Without the tail
    /// 40 ms: the single threshold ends the saccade while the eye still decelerates. With it 0: the saccade already lasts
    /// until the gaze samples show the landing, and every frame located after that is centred on the landed gaze. In
    /// tests/quadviews-gaze (90 Hz, 40 ms lead and latency, 20-degree saccade) a hold adds widened frames after the landing
    /// (4 without, 8 with 55 ms) and never moves the landed eye inside the box: it was inside on every frame either way.</summary>
    [JsonIgnore] public int QuadSaccadeHoldMs => UsesQuadSaccadeTail ? 0 : 40;
    /// <summary>The farthest the focus area reaches during a saccade (NDC, per axis): 0.5 with the tail (its 40 ms lead
    /// covers more angle than 25 ms; with frame generation too: 0.35 and 0.6 landed the same in an offline sweep),
    /// otherwise 0.35.</summary>
    [JsonIgnore] public double QuadSaccadeMaxExtend => UsesQuadSaccadeTail ? 0.5 : 0.35;
    /// <summary>Eye-tracked bundled Quad Views: when the tracker loses the eyes (past Quad Views'
    /// 600 ms cache), the focus area slides to the fixed one over 150 ms instead of jumping (dcsvr_lost_ease_ms). A gaze
    /// found again is followed at once. With frame generation the focus moves once per rendered frame, so the slide is
    /// 4-5 steps in 3X (the generated frames in between blend them). Off: dcsvr_lost_ease_ms=0, the jump.</summary>
    public bool QuadLostGazeEase { get; init; } = true;
    [JsonIgnore] public bool UsesQuadLostGazeEase => QuadLostGazeEase && Gaze == GazeMode.EyeTracked;
    /// <summary>Quad Views turbo: DCS starts the next frame while the runtime still holds the previous one. Quad Views itself
    /// enables it on most runtimes and disables it on SteamVR; off by default until measured in the headset.</summary>
    public bool QuadTurbo { get; init; }
    /// <summary>Turbo runs only without frame generation: Turbo starts DCS's next frame without waiting for the headset,
    /// and frame generation paces those same waits. Together, on the Pimax runtime, frames stalled for up to seconds
    /// (forum report, 2026-11-13; Quad Views' own settings already turn Turbo off on SteamVR). The saved choice is kept.</summary>
    public bool UsesQuadTurbo => QuadTurbo && FrameGen == FrameGeneration.Off;
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
    /// <summary>Cheeky NrBrightnessMatch=1: the DLSS 5 area keeps the brightness of the game's own image under it, so
    /// the focus does not show as a slightly darker disc. Off by default.</summary>
    public bool NeuralBrightnessMatch { get; init; }
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
    /// <summary>A lighter DLSS preset for the Quad Views periphery (the engine module's [DLSS] PeripheryPreset, R122):
    /// the focus keeps M, or <see cref="FocusDlssPreset"/> when one is set ([DLSS] FocusPreset). With it the pupil shift
    /// layer hides the NVIDIA driver profile's forced preset from DLSS inside DCS ([DlssPreset] IgnoreDriverPreset):
    /// read-only, nothing in the NVIDIA settings changes. Applies at the next mission load. Off by default.</summary>
    public PeripheryDlssPreset PeripheryDlssPreset { get; init; }
    /// <summary>It runs: a VR flight with Quad Views-Foveated (the module tells periphery from focus by its view sizes)
    /// and Engine Optimizations (the per-view preset is one of the module's engine features, not in its Perception-only
    /// mode). It does something only while DCS upscales with DLSS (ProfileValidation says so otherwise).</summary>
    public bool UsesPeripheryDlssPreset => PeripheryDlssPreset != PeripheryDlssPreset.Off && Enum.IsDefined(PeripheryDlssPreset) && !Desktop && QuadViews == QuadProvider.QuadViewsFoveated && EngineOptimizations;
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
    /// <summary>Where the in-headset panel sits (OFXR fork patch 0076, [overlay] panel_position). Upper right, as before, by default.</summary>
    public PanelPosition HeadsetPanelPosition { get; init; }
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
    /// <summary>The engine module's frame ring around each ini reload ([Log] ReloadFrameTimes, R142), for live-reload hitch checks. Profile only, off.</summary>
    public bool DiagnosticReloadFrameTimes { get; init; }
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
    /// <summary>New profiles (presets, the checklist, Read settings from this PC) follow Pimax Play. A saved profile without this
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
    /// <summary>[D3D] ChainFast and ShaderFast (module defaults 7 and 3: faster terrain compute chains and wider shader
    /// grids, the same image). On writes nothing, so the module's defaults apply; off writes ChainFast=0 and
    /// ShaderFast=0, so someone isolating a problem can turn them off too (GitHub #28). Re-read every second.</summary>
    public bool EngineFastShaderPaths { get; init; } = true;
    /// <summary>[Shadows] CascadeCache: the far shadow cascades (about 70 to 1500 m) are drawn every few frames and reused
    /// in between. Measured −0.72 ms of GPU, 0.16 % of shadow texels differ; distant tree shadows update about 11 times a
    /// second, so their wind sway can look stepped. Off by default; forced off with the engine switched off or in the
    /// Perception-only mode.</summary>
    public bool EngineShadowCache { get; init; }
    /// <summary>Ignored since 0.7: Three views ([Views] ThreeViews, the right periphery reused from the left eye) was parked
    /// after the headset test on 2026-11-21 (too many artefacts). Kept only so profiles saved with it still load; the app
    /// never writes the key (a developer can still set it by hand in DcsQvCull.ini).</summary>
    public bool EngineThreeViews { get; init; }
    /// <summary>[Water] SkipHiddenWater=3 (0.7, experimental, no-submit): DCS's water passes are not submitted for a view
    /// that earlier frames found dry (probed one frame in four). Not exact for a few frames when water comes into view.
    /// Off by default.</summary>
    public bool EngineWaterSkip { get; init; }
    [JsonIgnore] public bool UsesWaterSkip => EngineWaterSkip && EngineOptimizations;
    /// <summary>[D3D] EarlyShaders=3 (0.7, experimental): bit 0 SSAO water skip (SSAO not computed on water pixels, where
    /// it adds nothing: proven bit-identical) and bit 1 hot-air idle skip (the heat-haze copy and compose passes skipped
    /// when no heat was drawn that frame: the stock picture). Small gains (about 0.23 ms on open sea, 0.02-0.08 ms for
    /// hot air), so one switch for both. Off by default.</summary>
    public bool EngineIdleEffectSkip { get; init; }
    [JsonIgnore] public bool UsesIdleEffectSkip => EngineIdleEffectSkip && EngineOptimizations;
    /// <summary>[Model] ShadowRecorder: the shadow cascades' draws are recorded on 8 worker threads (D3D11 deferred
    /// contexts) instead of DCS's render thread; cascade 1 stays as DCS draws it. Verified texel-identical live;
    /// measured +3 to +5 % FPS, p95 −4 to −7 %, at 4-7 % more total CPU and about 30 MB of memory.</summary>
    public bool EngineShadowRecorder { get; init; } = true;
    /// <summary>[Model] GBufferRecorder: DCS's G-buffer model draws are recorded on worker threads (D3D11 deferred
    /// contexts) in DCS's exact order and replayed by DCS; segments whose textures changed are recorded again. Verified
    /// pixel-identical live; measured +22 to +24 % FPS on its own, at about 12 more worker threads.</summary>
    public bool EngineGBufferRecorder { get; init; } = true;
    /// <summary>[Model] RecordersEmulatedLists: the shadow and G-buffer recorders also run when the graphics driver has no
    /// D3D11 command lists (typically AMD Radeon), on the lists the D3D11 runtime emulates, with copy-based constant
    /// uploads. A smaller gain than with driver lists; not yet tested on a Radeon. Read when a recorder installs (DCS
    /// start), so it waits for the next launch. Off by default; written only when on.</summary>
    public bool EngineRecordersEmulatedLists { get; init; }
    /// <summary>It matters: <see cref="EngineRecordersEmulatedLists"/> with Engine Optimizations and a recorder on.</summary>
    public bool UsesRecordersEmulatedLists => EngineRecordersEmulatedLists && EngineOptimizations && (EngineShadowRecorder || EngineGBufferRecorder);
    /// <summary>[Model] PlainTriangleCounter: a statistics-only triangle counter updated without a locked instruction.</summary>
    public bool EnginePlainCounter { get; init; } = true;
    /// <summary>[Timing] CacheUs: how often the cached time is refreshed, in microseconds.</summary>
    public int EngineTimerRefreshUs { get; init; } = 1000;
    public const int EngineTimerRefreshMin = 250, EngineTimerRefreshMax = 2000;
    /// <summary>[Model] ShaderCachePrewarm: shader variants a DCS or app update made stale are compiled again from DCS's
    /// start on (usually still in the main menu), not only when a mission first draws them. Read once at payload start.</summary>
    public bool EngineShaderCachePrewarm { get; init; } = true;
    /// <summary>[Model] ShaderCompileWorkers: threads compiling shader variants; 0 = automatic (a quarter of the logical
    /// processors, 2 to 6). The page offers Auto, 2, 4 and 6. Read once at payload start.</summary>
    public int EngineShaderCompileWorkers { get; init; }
    public static readonly int[] EngineShaderCompileWorkerChoices = [0, 2, 4, 6];
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
    /// cockpit and the image shift against the lens when the eyes turn. The gaze comes from the eye-tracked Quad Views,
    /// or, without Quad Views or with a fixed Quad Views focus, from the runtime's OpenXR eye gaze, which the layer reads
    /// itself (<see cref="UsesLayerOwnGaze"/>).</summary>
    public bool PupilShift { get; init; }
    /// <summary>Distance from the pupil to the eye's rotation centre (anatomy: about 10 to 11 mm).</summary>
    public double PupilShiftEyeRadiusMm { get; init; } = 10.5;
    /// <summary>Distance of the lens's virtual image. Not published by Pimax; pancake designs for 1.3" micro-OLED
    /// panels sit at 1 to 2 m. Only affects distant objects, and by less than 0.1 degree across that range.</summary>
    public double PupilShiftVirtualImageM { get; init; } = 1.5;
    /// <summary>Turns the correction off and on in flight, to compare (PupilShift.ini Toggle; the panel names it).</summary>
    public string PupilShiftToggleKey { get; init; } = NeuralHotkeys.PupilDefault;
    public const double PupilShiftEyeRadiusMin = 5, PupilShiftEyeRadiusMax = 15, PupilShiftVirtualImageMin = 0.5, PupilShiftVirtualImageMax = 5;
    /// <summary>The pupil correction runs: a VR flight in which the layer has a gaze (<see cref="UsesLayerGaze"/>).</summary>
    public bool UsesPupilShift => PupilShift && UsesLayerGaze;

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
    /// <summary>It runs: a VR flight with the bundled Quad Views. The depth comes from the engine module, installed for it
    /// in its Perception-only mode when Engine Optimizations are off (<see cref="UsesEngineModule"/>).</summary>
    public bool UsesFarDepth => FarDepth && !Desktop && UsesBundledQuadViews;
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
    /// is deployed when any of them runs, for the Periphery DLSS preset (it hides the driver's forced preset from
    /// DLSS: it loads before DCS starts its renderer, the engine module only after), and for the head pose of 3D audio's
    /// late head tracking (<see cref="UsesHeadPosePublish"/>; without the component, 3D audio runs without it).</summary>
    public bool UsesPerceptionLayer => PerceptionLayerRequired || UsesHeadPosePublish;
    /// <summary>A feature that cannot run without the layer wants it. The head pose alone does not: a distribution without
    /// the layer flies 3D audio with DCS's own head direction.</summary>
    [JsonIgnore] public bool PerceptionLayerRequired => UsesPupilShift || UsesDepthBoost || UsesGazePublish || UsesPeripheryDlssPreset;
    /// <summary>The layer moves the eyes (Pupil shift or Depth boost), so it takes over DCS's custom IPD. Publishing the
    /// gaze alone moves nothing and leaves DCS's IPD as it is.</summary>
    public bool UsesLayerEyeDistance => UsesPupilShift || UsesDepthBoost;
    /// <summary>The engine module measures the scene depth at the gaze (DcsQvCull.ini [Perception] FixationDepth=1)
    /// whenever Depth boost runs, with or without the engine optimizations.</summary>
    public bool UsesFixationDepth => UsesDepthBoost;
    /// <summary>The engine module is installed: for Engine Optimizations, or for Depth boost's dual camera and scene
    /// depth, a Visual realism feature, Far depth's depth or frame generation's cockpit exports (mask, depth, DCS's
    /// motion vectors, Sharp HUD) alone, in its Perception-only mode ([General] PerceptionOnly=1: none of the engine
    /// optimizations, no test suite or engine keys). 3D headphone audio is part of the module too.</summary>
    public bool UsesEngineModule => EngineModuleRequired || (FrameGen != FrameGeneration.Off && !Desktop);
    /// <summary>A feature that cannot run without the module wants it. Frame generation's exports alone do not: without
    /// the component (a development distribution) frame generation runs without them, as before 0.7.</summary>
    [JsonIgnore] public bool EngineModuleRequired => EngineOptimizations || UsesDepthBoost || UsesVisualRealism || UsesFarDepth || UsesBinauralAudio;
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
    /// <summary>[Color] PanelGamut (engine module R111): 0 off, 1 sRGB mapped to the Crystal Super's wider panel gamut
    /// (about DCI-P3), 2 half of that. Works with Fix and Enhanced; HUD and MFD symbology are not affected. Off by default.</summary>
    public int PanelGamut { get; init; }
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

    // 3D headphone audio (docs/BINAURAL_AUDIO.md): the engine module's [Audio] section, every key re-read in flight. Off
    // by default; a profile without these properties loads as off with the defaults below (binaural_config.h kKeys).
    /// <summary>[Audio] Binaural: DCS's left/right panning of the mono 3D sounds of the world and the cockpit is replaced
    /// by HRTF rendering (direction per ear: time, level and the ear's own colouring), so sounds come from front, back,
    /// above. Radio and intercom stay as DCS plays them. For headphones; on a monitor too (with head tracking). Off by
    /// default; Launch DCS installs the engine module for it, in its Perception-only mode when Engine Optimizations are off.</summary>
    public bool BinauralAudio { get; init; }
    /// <summary>[Audio] Hrtf: "default" (the generic SADIE II KU100 set embedded in the module) or the name of a set
    /// imported from the pilot's own SOFA file (<see cref="HrtfLibrary"/>).</summary>
    public string BinauralHrtf { get; init; } = Binaural.DefaultHrtf;
    /// <summary>[Audio] HeadCircumferenceCm: the head size the time difference between the ears is scaled to (48 to 66;
    /// 54, 57 and 60 are Small, Medium and Large).</summary>
    public double HeadCircumferenceCm { get; init; } = Binaural.HeadCircumferenceDefault;
    /// <summary>[Audio] LateHeadTracking: the head followed at audio rate between DCS frames, from the head pose the pupil
    /// shift layer publishes (PupilShift.ini [HeadPose] Publish=1, VR only). On by default.</summary>
    public bool BinauralLateHeadTracking { get; init; } = true;
    /// <summary>[Audio] NearField: sources nearer than about 2 m (the cockpit) rendered per ear. On by default.</summary>
    public bool BinauralNearField { get; init; } = true;
    /// <summary>[Audio] MaxObjects: the most sounds rendered in 3D at once (the loudest); the rest keep DCS's panning.</summary>
    public int BinauralMaxObjects { get; init; } = Binaural.MaxObjectsDefault;
    /// <summary>[Audio] LevelTrimDb: level of the 3D sounds only, against DCS's own mix.</summary>
    public double BinauralLevelTrimDb { get; init; }
    /// <summary>[Audio] CpuBudgetPercent: the share of each audio block 3D audio may use before it renders fewer sounds.</summary>
    public int BinauralCpuBudgetPercent { get; init; } = Binaural.CpuBudgetDefault;
    /// <summary>[Audio] ExcludeContexts: DCS sound context ids (0 to 7, comma separated) left to DCS's panning. Profile file
    /// only (for diagnosis: which id is which is only known from a live log); written only when set.</summary>
    public string? BinauralExcludeContexts { get; init; }
    /// <summary>[Audio] LogVoices: a per-sound CSV next to the module's log, for diagnosis. Off by default.</summary>
    public bool BinauralLogVoices { get; init; }
    /// <summary>[Hotkeys] AudioToggle: switches 3D audio off and back on in flight, to compare (DCS's own panning while
    /// off). "virtual-key:modifiers"; "Off" disables it.</summary>
    public string AudioToggleKey { get; init; } = NeuralHotkeys.AudioDefault;
    /// <summary>3D audio runs: on, in VR or on a monitor (it needs no headset).</summary>
    [JsonIgnore] public bool UsesBinauralAudio => BinauralAudio;
    /// <summary>The pupil shift layer publishes the head pose for 3D audio's late head tracking: a VR flight with both on.</summary>
    [JsonIgnore] public bool UsesHeadPosePublish => UsesBinauralAudio && BinauralLateHeadTracking && !Desktop;
    /// <summary>The audio in-flight key is in use.</summary>
    [JsonIgnore] public bool UsesAudioToggleKey => UsesBinauralAudio && NeuralHotkeys.TryParse(AudioToggleKey ?? NeuralHotkeys.AudioDefault, out var key) && !key.IsOff;

    /// <summary>The color mode hotkey is set (it can switch to any mode in flight).</summary>
    public bool UsesColorModeKey => !Desktop && NeuralHotkeys.TryParse(ColorModeKey ?? NeuralHotkeys.ColorModeDefault, out var key) && !key.IsOff;
    /// <summary>The module's color modes run, or can be switched on in flight: VR flights only (the module replaces the
    /// tone map of the headset's views).</summary>
    public bool UsesColorMode => !Desktop && (ColorMode != ColorMode.Off || UsesColorModeKey);
    /// <summary>A VR flight with an eye-tracked Quad Views focus: the pupil shift layer has a gaze to work with.</summary>
    public bool UsesEyeTrackedVr => !Desktop && QuadViews != QuadProvider.None && Gaze == GazeMode.EyeTracked;
    /// <summary>The pupil shift layer reads the gaze itself, with no eye-tracked Quad Views focus to take it from: without
    /// Quad Views, or with a fixed Quad Views focus ([PupilShift] QuadViewsFixedFocus=1), on the Pimax runtime, whose
    /// OpenXR eye gaze (XR_EXT_eye_gaze_interaction) it reads (docs/PUPIL_SHIFT.md, harness scenarios xr_gaze and
    /// qv_fixed_xr_gaze). Without a gaze from it the eyes stay at rest. Not flown yet. Depth boost keeps needing
    /// eye-tracked Quad Views.</summary>
    [JsonIgnore] public bool UsesLayerOwnGaze => !Desktop && Runtime == RuntimeKind.Pimax && (QuadViews == QuadProvider.None || Gaze == GazeMode.Fixed);
    /// <summary>A VR flight in which the pupil shift layer has a gaze: eye-tracked Quad Views, or its own.</summary>
    [JsonIgnore] public bool UsesLayerGaze => UsesEyeTrackedVr || UsesLayerOwnGaze;
    /// <summary>Gaze DoF runs: it needs the gaze, which the pupil shift layer publishes for the module (with Depth boost,
    /// or its [PupilShift] PublishGaze for Visual realism alone).</summary>
    public bool UsesGazeDof => GazeDof && UsesLayerGaze;
    public bool UsesGVision => GVision && !Desktop;
    /// <summary>The pupil shift layer publishes the gaze for the engine module ([PupilShift] PublishGaze=1): for Gaze DoF,
    /// and for G vision's tunnel following the eyes.</summary>
    public bool UsesGazePublish => UsesGazeDof || (UsesGVision && GVisionUseGaze && UsesLayerGaze);
    /// <summary>Any Visual realism feature runs, so the engine module is installed for it (<see cref="UsesEngineModule"/>).</summary>
    public bool UsesVisualRealism => UsesColorMode || UsesGazeDof || UsesGVision;

    /// <summary>The engine optimizations module runs the prefetch fix ([Prefetch] Mode in DcsQvCull.ini), also in
    /// Optimizations only and when DCS starts from Steam or a shortcut.</summary>
    public bool UsesPrefetchFix => EngineOptimizations && BoostPrefetch != PrefetchFix.Off;
    /// <summary>The boost helper is started with DCS: for CPU Boost, Free VRAM before flight or the monitor mode.</summary>
    public bool UsesBoostHelper => CpuBoost || FreeVram || UsesLowerMonitor || UsesTobiiPause;
    /// <summary>The desktop Tobii pause is for a Pimax next to a desktop tracker, never with <see cref="Desktop"/>.</summary>
    public bool UsesTobiiPause => PauseTobiiDesktop && !Desktop && Runtime != RuntimeKind.VirtualDesktop;
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
    /// <summary>Virtual Desktop's OpenXR runtime manifest (VDXR, virtualdesktop-openxr.json); null when not installed.</summary>
    public string? VirtualDesktopRuntime { get; init; }
    public string? PimaxVersion { get; init; }
    /// <summary>The active OpenXR runtime's name when it is neither Pimax's nor SteamVR's (Varjo, Meta, Virtual Desktop,
    /// Windows Mixed Reality…); null for those two or when none is registered.</summary>
    public string? OtherOpenXrRuntime => OpenXrRuntimeNames.Other(ActiveRuntime);
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
    /// only moves the depth gradually), the headset tracking (detected, never a setting: it lives on the inventory), and
    /// the derived "usesThreeViews" an earlier 0.7 build saved before Three views was parked, and the derived values earlier 0.7
    /// builds saved and no longer write ([JsonIgnore]: 0.6.6, the release before 0.7, refuses a key it does not know, and these were new in 0.7).
    /// "frameGenFocusLayer", the Focus layer choice of earlier 0.7 builds: foveated frame generation now always sends the
    /// focus as Opaque focus (ConfigurationWriters.QuadViews), whatever an older draft or preset chose. "engineReleaseIdleDlss"
    /// (Free unused DLSS memory, removed in 0.7: on DCS 2.9.31 it released nothing) and its derived "usesReleaseIdleDlss".
    /// Dropped when a profile loads, in any casing.</summary>
    private static readonly HashSet<string> ObsoleteProfileKeys = new(["externalLimiter", "externalLimiterFps", "runtimeReprojection",
        "experimentalAcknowledged", "sboysDriverDirectory", "depthBoostTransition", "depthBoostTransitionKey", "tracking",
        "usesThreeViews", "usesQuadSaccadeTail", "usesQuadLostGazeEase", "usesWaterSkip", "usesIdleEffectSkip", "engineModuleRequired",
        "usesLayerOwnGaze", "usesLayerGaze", "quadSaccadeHoldMs", "quadSaccadeMaxExtend", "frameGenFocusLayer",
        "engineReleaseIdleDlss", "usesReleaseIdleDlss"], StringComparer.OrdinalIgnoreCase);

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
            // Periphery DLSS preset K or M (no longer offered: heavier than E, which the periphery is for) loads as E.
            if (profile.PeripheryDlssPreset is PeripheryDlssPreset.K or PeripheryDlssPreset.M) profile = profile with { PeripheryDlssPreset = PeripheryDlssPreset.E };
            // Without Developer options the developer-only settings load at their safe values (VrProfile.ForPilots).
            return MigrateCloseApps(profile.ForPilots(), document.RootElement.ValueKind == JsonValueKind.Object && document.RootElement.TryGetProperty("freeVram", out _));
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
        new() { Id = "sboys-combined", Name = "Sboys · DLSS 5 + framegen + Quad Views", Runtime = RuntimeKind.SboysSteamVr, QuadViews = QuadProvider.QuadViewsFoveated, QuadFocusAdapter = true, FrameGen = FrameGeneration.Nvidia, NeuralRendering = true },
        HeadsetRoute.WithQuest3Focus(new() { Id = "vd-quadviews", Name = "Quest 3 VD · Quad Views (fixed focus)", Runtime = RuntimeKind.VirtualDesktop, QuadViews = QuadProvider.QuadViewsFoveated })
    ];
}
