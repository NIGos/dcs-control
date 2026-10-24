namespace DcsVr.Core;

public static class ProfileValidation
{
    /// <summary>Processes that start DCS (Steam, Explorer, Epic, the DCS launcher and updater, this app and its CPU Boost
    /// helper), DCS itself, and core Windows processes: CPU Boost never moves or closes them, whatever pattern matches.</summary>
    public static IReadOnlySet<string> BoostProtectedProcesses { get; } = new HashSet<string>(
    [
        "steam", "steamservice", "explorer", "EpicGamesLauncher", "DCS", "DCS_updater", "DcsControl", "DcsVrControl", "DcsVr.Cli",
        "cmd", "powershell", "pwsh", "conhost", "dllhost", "devenv",
        "csrss", "winlogon", "dwm", "svchost", "lsass", "services", "wininit", "smss", "System", "Idle",
        "System Idle Process", "Registry", "Memory Compression", "fontdrvhost", "audiodg"
    ], StringComparer.OrdinalIgnoreCase);

    /// <summary>Settles feature combinations that cannot run, exactly like the interface's checklist (resolveFeatures in
    /// ui/app.js): Foveated Super Resolution is for stereo only, so Quad Views turns it off (with Quad Views it would redo
    /// DCS's DLSS of the focus views instead of DLSS 5 building on it); Pimax native Quad Views exists only on the Pimax
    /// route and cannot host the focus adapter DLSS 5 needs, so it becomes bundled Quad Views there; with bundled Quad
    /// Views, Cheeky always runs through the focus adapter.</summary>
    public static VrProfile ResolveFeatures(VrProfile profile)
    {
        // Optimizations only keeps the VR settings for the next VR flight; VrProfile.ForLaunch is what runs.
        if (profile.FoveatedDlss && profile.QuadViews != QuadProvider.None) profile = profile with { FoveatedDlss = false };
        if (profile.QuadViews == QuadProvider.PimaxNative && (profile.Runtime == RuntimeKind.SboysSteamVr || profile.UsesCheeky))
            profile = profile with { QuadViews = QuadProvider.QuadViewsFoveated };
        return profile with { QuadFocusAdapter = profile.UsesCheeky && profile.QuadViews == QuadProvider.QuadViewsFoveated };
    }

    /// <summary>options.lua inside Saved Games\DCS*\Config, the only place DCS reads it from.</summary>
    public static bool IsSavedGamesOptions(string path)
    {
        var config = Path.GetDirectoryName(Path.GetFullPath(path));
        var profileRoot = config is null ? null : Path.GetDirectoryName(config);
        return Path.GetFileName(path).Equals("options.lua", StringComparison.OrdinalIgnoreCase) && config is not null && profileRoot is not null
            && Path.GetFileName(config).Equals("Config", StringComparison.OrdinalIgnoreCase) && Path.GetFileName(profileRoot).StartsWith("DCS", StringComparison.OrdinalIgnoreCase);
    }
    public static IReadOnlyList<ValidationIssue> Validate(VrProfile profile, InventorySnapshot? inventory = null)
    {
        // What will run: Optimizations only is never held back by the VR settings it keeps for later.
        profile = profile.ForLaunch();
        var issues = new List<ValidationIssue>();
        void Error(string code, string text) => issues.Add(new(code, IssueSeverity.Error, text));
        void Range(string code, double value, double min, double max)
        {
            if (!double.IsFinite(value) || value < min || value > max) Error(code, $"Allowed range: {min} to {max}.");
        }
        if (profile.SchemaVersion != 1) Error("schema", "Unsupported profile version.");
        if (string.IsNullOrWhiteSpace(profile.Id) || profile.Id.Length > 80 || profile.Id.Any(c => !char.IsAsciiLetterOrDigit(c) && c != '-'))
            Error("profile-id", "Profile ID must contain letters, numbers, or hyphens.");
        if (string.IsNullOrWhiteSpace(profile.Name) || profile.Name.Length > 120) Error("profile-name", "Enter a name of at most 120 characters.");
        if (!Enum.IsDefined(profile.Runtime) || !Enum.IsDefined(profile.QuadViews) || !Enum.IsDefined(profile.FrameGen) || !Enum.IsDefined(profile.Gaze) || !Enum.IsDefined(profile.Tracking))
            Error("enum", "The profile contains an unrecognized choice.");
        Range("fovea-width", profile.FoveaWidth, .1, 1);
        Range("fovea-height", profile.FoveaHeight, .1, 1);
        Range("peripheral-scale", profile.PeripheralScale, .15, 1);
        Range("quad-focus-scale", profile.QuadFocusScale, .5, 2);
        Range("quad-sharpening", profile.QuadSharpening, 0, 1);
        Range("quad-periphery-contrast", profile.QuadPeripheryContrast, 0, 1);
        Range("quad-edge-blend", profile.QuadEdgeBlend, 0, .5);
        Range("periphery-stability-strength", profile.PeripheryStabilityStrength, 0, 1);
        Range("neural-scale", profile.NeuralWorkingScale, .1, 1);
        Range("neural-intensity", profile.NeuralIntensity, 0, 1);
        Range("neural-local-tone", profile.NeuralLocalTone, 0, 2);
        Range("neural-local-structure", profile.NeuralLocalStructure, 0, 2);
        Range("neural-skin-structure", profile.NeuralSkinStructure, 0, 2);
        Range("neural-color", profile.NeuralColorStrength, 0, 2);
        Range("neural-transfer", profile.NeuralTransferStrength, 0, 2);
        Range("neural-paper-white", profile.NeuralPaperWhiteScale, .01, 8);
        Range("neural-motion-x", profile.NeuralMotionScaleX, -4, 4);
        Range("neural-motion-y", profile.NeuralMotionScaleY, -4, 4);
        if (!Enum.IsDefined(profile.NeuralDepth)) Error("neural-depth", "Select game, normal, or reversed depth.");
        if (!VrProfile.FocusDlssPresets.Contains(profile.FocusDlssPreset)) Error("focus-preset", "Select DCS's own preset or E, K, L or M for the focus DLSS preset.");
        if (!VrProfile.NeuralAreas.Contains(profile.NeuralFocusArea)) Error("neural-area", "DLSS 5 area must be the whole focus area or its central 90%, 80%, 70% or 50%.");
        if (profile.NvidiaFlowScale is not (50 or 75 or 100)) Error("flow-scale", "Optical flow resolution must be 50%, 75%, or 100%.");
        if (!Enum.IsDefined(profile.FlowPreset)) Error("flow-preset", "Select fast, medium, or slow optical flow.");
        if (!Enum.IsDefined(profile.FpsLimit)) Error("fps-mode", "Select a recognized DCS frame limit.");
        Range("refresh-rate", profile.HeadsetRefreshHz, 50, 240);
        Range("rendered-fps-cap", profile.RenderedFpsCap, 30, 300);
        if (profile.Runtime == RuntimeKind.SboysSteamVr && profile.QuadViews == QuadProvider.PimaxNative)
            Error("quad-runtime", "Pimax native Quad Views works only on the Pimax route. Select Bundled Quad Views as the Quad Views provider, or switch the route to Pimax.");
        if (profile.UsesCheeky && profile.QuadViews != QuadProvider.None && !profile.UsesQuadFocus)
            Error("quad-cheeky", "DLSS 5 with Quad Views needs Bundled Quad Views as the Quad Views provider.");
        if (profile.UsesQuadFocus && !string.IsNullOrWhiteSpace(profile.QuadViewsLayerDirectory))
            Error("quad-adapter-provider", "DLSS 5 with Quad Views needs the bundled Quad Views provider. Clear the Alternative Quad Views provider folder.");
        if (profile.UsesQuadFocus)
            issues.Add(new("quad-focus", IssueSeverity.Info, "DLSS 5 runs only on the focus area of Quad Views, on top of DCS's own DLSS. The periphery keeps DCS's normal image."));
        if (profile.Runtime == RuntimeKind.Pimax && profile.QuadViews == QuadProvider.QuadViewsFoveated)
            issues.Add(new("pimax-native-replaced", IssueSeverity.Info, "Bundled Quad Views takes over from Pimax's own Quad Views for DCS. Keep Quad Views turned on in Pimax Play (the tested setup)." + (PimaxFovea.UsesPimaxPlay(profile)
                ? "" : " The focus area comes from this profile, not from Pimax Play's Quad View settings.")));
        if (!Enum.IsDefined(profile.FoveaSource)) Error("fovea-source", "Select Pimax Play or this profile as the focus area source.");
        if (profile.CpuBoost || profile.FreeVram)
        {
            // Names are process names without ".exe". Moving or closing a process that starts DCS (or DCS itself) would
            // slow down or end the game it is meant to help.
            static string Normalize(string name) => name.Trim() is var n && n.EndsWith(".exe", StringComparison.OrdinalIgnoreCase) ? n[..^4] : n.Trim();
            var lists = new List<(string Feature, string Verb, string List, IReadOnlyList<string>? Names)>();
            if (profile.FreeVram) lists.Add(("Free VRAM", "close", "Programs to close", profile.FreeVramApps));
            if (profile.CpuBoost && profile.BoostMoveBackgroundApps) lists.Add(("CPU Boost", "move", "Background apps", profile.BoostBackgroundApps));
            foreach (var (feature, label, list, names) in lists)
            {
                if (names is null) continue;
                if (names.Any(string.IsNullOrWhiteSpace)) Error("boost-app-name", $"{list} has an empty line. Remove it.");
                foreach (var name in names.Where(n => !string.IsNullOrWhiteSpace(n)).Select(Normalize).Distinct(StringComparer.OrdinalIgnoreCase))
                {
                    // A pattern of wildcards only ("*", "?*", "*.exe") would match every process.
                    if (name.All(c => c is '*' or '?'))
                    { Error("boost-app-wildcard", $"{feature} cannot {label} '{name}': it would match every program. Enter a program name; * and ? work only as part of a name."); continue; }
                    // Wildcards are matched exactly as at run time, so "D*" or "s*" is refused like "DCS" or "steam".
                    var hit = BoostProtectedProcesses.FirstOrDefault(p => BoostPlanner.MatchesProcessName(name, p));
                    if (hit is not null)
                        Error("boost-launcher", (string.Equals(hit, name, StringComparison.OrdinalIgnoreCase)
                            ? $"{feature} cannot {label} {name}: it "
                            : $"{feature} cannot {label} '{name}': it matches {hit}, which ") + $"starts DCS, is DCS itself or is a protected Windows process. Remove it from {list}.");
                }
            }
        }
        if (profile.CpuBoost)
        {
            if (!Enum.IsDefined(profile.BoostDcsPriority)) Error("boost-choice", "Select a recognized DCS priority.");
            if (profile.BoostDcsPriority == BoostPriority.High)
                issues.Add(new("boost-high-priority", IssueSeverity.Warning, "High priority lets DCS take CPU time ahead of the VR compositor, headset services, audio and input handling. When DCS saturates the CPU this can cause stutter, tracking or input lag, or audio dropouts. Above normal is recommended; use High only if you have tested it."));
        }
        if (profile.PupilShift && profile.PupilShiftEyeRadiusMm is < VrProfile.PupilShiftEyeRadiusMin or > VrProfile.PupilShiftEyeRadiusMax)
            Error("pupil-radius", $"Eye rotation radius must be {VrProfile.PupilShiftEyeRadiusMin}–{VrProfile.PupilShiftEyeRadiusMax} mm.");
        if (profile.PupilShift && profile.PupilShiftVirtualImageM is < VrProfile.PupilShiftVirtualImageMin or > VrProfile.PupilShiftVirtualImageMax)
            Error("pupil-image", $"Lens image distance must be {VrProfile.PupilShiftVirtualImageMin}–{VrProfile.PupilShiftVirtualImageMax} m.");
        if (profile.PupilShift && !profile.Desktop && !profile.UsesPupilShift)
            issues.Add(new("pupil-gaze", IssueSeverity.Warning, "Pupil shift needs Quad Views with an eye-tracked focus area, because it reads your gaze from it. With these settings it does not run."));
        if (profile.PeripheryStability && !profile.Desktop && !profile.UsesPeripheryStability)
            issues.Add(new("periphery-stability-provider", IssueSeverity.Warning, "Periphery stability runs in the bundled Quad Views only. With Pimax native Quad Views or with Quad Views off, it does not run."));
        if (profile.FarDepth && !profile.Desktop && !profile.UsesFarDepth)
            issues.Add(new("far-depth-needs", IssueSeverity.Warning, "Far depth needs the bundled Quad Views and Engine Optimizations (they measure the depth). With these settings it does not run."));
        if (profile.FarDepth && !Enum.IsDefined(profile.FarDepthPreset)) Error("far-depth-preset", "Select Subtle, Default or Strong for Far depth.");
        if (!Enum.IsDefined(profile.PeripheryDlssPreset)) Error("periphery-preset", "Select Off, E, F, K or M for the periphery DLSS preset.");
        else if (profile.PeripheryDlssPreset != PeripheryDlssPreset.Off && !profile.Desktop && !profile.UsesPeripheryDlssPreset)
            issues.Add(new("periphery-preset-needs", IssueSeverity.Warning, "Periphery DLSS preset needs bundled Quad Views (not Pimax native) and Engine Optimizations. Until then it does not run."));
        if (profile.DepthBoost && !Enum.IsDefined(profile.DepthBoostMode)) Error("depth-mode", "Select Realistic or Enhanced for Depth boost.");
        if (profile.DepthBoost && profile.DepthBoostMode == DepthBoostMode.Enhanced && profile.DepthBoostMax is < VrProfile.DepthBoostMaxMin or > VrProfile.DepthBoostMaxMax)
            Error("depth-max", $"Depth boost's Enhanced strength must be ×{VrProfile.DepthBoostMaxMin}–×{VrProfile.DepthBoostMaxMax}.");
        if (profile.DepthBoost && profile.DepthBoostMode == DepthBoostMode.Realistic && profile.DepthBoostRealisticMax is < VrProfile.DepthBoostMaxMin or > VrProfile.DepthBoostRealisticMaxMax)
            Error("depth-realistic-max", $"Depth boost's Realistic strength must be ×{VrProfile.DepthBoostMaxMin}–×{VrProfile.DepthBoostRealisticMaxMax}.");
        if (profile.DepthBoost && profile.DepthBoostMode == DepthBoostMode.Realistic &&
            (profile.DepthBoostFarStartM is < VrProfile.DepthBoostFarStartMin or > VrProfile.DepthBoostFarStartMax || !double.IsFinite(profile.DepthBoostFarFullM) ||
             profile.DepthBoostFarFullM < profile.DepthBoostFarStartM * 1.2 || profile.DepthBoostFarFullM > VrProfile.DepthBoostFarFullMax))
            Error("depth-far", $"Depth boost's Realistic distances: normal depth up to {VrProfile.DepthBoostFarStartMin}–{VrProfile.DepthBoostFarStartMax} m, the full strength from at least 1.2 times that and at most {VrProfile.DepthBoostFarFullMax} m.");
        if (profile.DepthBoost && profile.DepthBoostNearM is < VrProfile.DepthBoostNearMin or > VrProfile.DepthBoostNearMax)
            Error("depth-near", $"Depth boost's cockpit distance must be {VrProfile.DepthBoostNearMin}–{VrProfile.DepthBoostNearMax} m.");
        if (profile.DepthBoost && !profile.Desktop && !profile.UsesDepthBoost)
            issues.Add(new("depth-gaze", IssueSeverity.Warning, "Depth boost needs Quad Views with an eye-tracked focus area, because it reads where each eye looks from it. With these settings it does not run."));
        if (profile.FrameGenHudSymbology && profile.FrameGen != FrameGeneration.Off && !profile.Desktop && !profile.UsesDepthBoost)
            issues.Add(new("hud-symbology-needs", IssueSeverity.Warning, "Sharp HUD needs Depth boost (Perception page). Until then it does not run."));
        // Visual realism: the module clamps out-of-range values itself; the profile refuses them, like the other settings.
        // Codes are "visual-" + the profile property (camelCase), so the Checks page can open the field.
        void Visual(string property, double value, double min, double max, string unit = "")
        {
            if (!double.IsFinite(value) || value < min || value > max) Error("visual-" + char.ToLowerInvariant(property[0]) + property[1..], $"Allowed range: {min}{unit} to {max}{unit}.");
        }
        if (!Enum.IsDefined(profile.ColorMode)) Error("visual-colorMode", "Select Off, Fix or Enhanced for the color mode.");
        if (!Enum.IsDefined(profile.GVisionMode)) Error("visual-gVisionMode", "Select Auto, Complement DCS or Full for G vision.");
        Visual(nameof(VrProfile.ColorFixExposureEv), profile.ColorFixExposureEv, -2, 2, " EV");
        Visual(nameof(VrProfile.ColorFixNightEv), profile.ColorFixNightEv, -2, 2, " EV");
        Visual(nameof(VrProfile.ColorFixSaturation), profile.ColorFixSaturation, .5, 1.5);
        Visual(nameof(VrProfile.ColorFixWarmth), profile.ColorFixWarmth, 0, 1);
        Visual(nameof(VrProfile.ColorFixAdaptBright), profile.ColorFixAdaptBright, 0, 10, " s");
        Visual(nameof(VrProfile.ColorFixAdaptDark), profile.ColorFixAdaptDark, 0, 30, " s");
        Visual(nameof(VrProfile.ColorEnhancedExposureEv), profile.ColorEnhancedExposureEv, -2, 2, " EV");
        Visual(nameof(VrProfile.ColorEnhancedNightEv), profile.ColorEnhancedNightEv, -2, 2, " EV");
        Visual(nameof(VrProfile.ColorEnhancedSaturation), profile.ColorEnhancedSaturation, .5, 1.5);
        Visual(nameof(VrProfile.PanelGamut), profile.PanelGamut, 0, 2);
        Visual(nameof(VrProfile.ColorEnhancedWarmth), profile.ColorEnhancedWarmth, 0, 1);
        Visual(nameof(VrProfile.ColorEnhancedAdaptBright), profile.ColorEnhancedAdaptBright, 0, 10, " s");
        Visual(nameof(VrProfile.ColorEnhancedAdaptDark), profile.ColorEnhancedAdaptDark, 0, 30, " s");
        Visual(nameof(VrProfile.NightVisionStrength), profile.NightVisionStrength, 0, 1);
        Visual(nameof(VrProfile.NightVisionBlue), profile.NightVisionBlue, 0, 1);
        Visual(nameof(VrProfile.NightVisionBlur), profile.NightVisionBlur, 0, 1);
        Visual(nameof(VrProfile.NightVisionScotopic), profile.NightVisionScotopic, .0005, .5, " cd/m²");
        Visual(nameof(VrProfile.NightVisionPhotopic), profile.NightVisionPhotopic, .05, 100, " cd/m²");
        if (double.IsFinite(profile.NightVisionScotopic) && double.IsFinite(profile.NightVisionPhotopic) && profile.NightVisionPhotopic < 4 * profile.NightVisionScotopic)
            Error("visual-nightVisionPhotopic", "The daylight level must be at least 4 times the full-night level.");
        Visual(nameof(VrProfile.GlareStrength), profile.GlareStrength, 0, 1);
        Visual(nameof(VrProfile.GlareThresholdEv), profile.GlareThresholdEv, 0, 8, " EV");
        Visual(nameof(VrProfile.GlareRadius), profile.GlareRadius, 1, 20, "°");
        Visual(nameof(VrProfile.GlareStreak), profile.GlareStreak, 0, 1);
        Visual(nameof(VrProfile.GazeDofStrength), profile.GazeDofStrength, 0, 1);
        Visual(nameof(VrProfile.GazeDofMaxArcmin), profile.GazeDofMaxArcmin, 1, 20, " arcmin");
        Visual(nameof(VrProfile.GazeDofSettleMs), profile.GazeDofSettleMs, 0, 500, " ms");
        Visual(nameof(VrProfile.GazeDofFadeInMs), profile.GazeDofFadeInMs, 50, 1000, " ms");
        Visual(nameof(VrProfile.GazeDofToleranceD), profile.GazeDofToleranceD, .1, 1, " D");
        Visual(nameof(VrProfile.GazeDofFoveaGuardDeg), profile.GazeDofFoveaGuardDeg, 0, 5, "°");
        if (!double.IsFinite(profile.GazeDofPupilMm) || (profile.GazeDofPupilMm != 0 && profile.GazeDofPupilMm is < 2 or > 8))
            Error("visual-gazeDofPupilMm", "Pupil size must be 0 (automatic) or 2 to 8 mm.");
        Visual(nameof(VrProfile.GVisionStrength), profile.GVisionStrength, 0, 1);
        Visual(nameof(VrProfile.GVisionTolerance), profile.GVisionTolerance, 4, 10, " G");
        if (profile.GazeDof && !profile.Desktop && !profile.UsesGazeDof)
            issues.Add(new("visual-gaze", IssueSeverity.Warning, "Eye-focus blur needs Quad Views with an eye-tracked focus area, because it reads your gaze from it. With these settings it does not run."));
        if (profile.EngineOptimizations && !Enum.IsDefined(profile.BoostPrefetch)) Error("engine-prefetch", "Select a recognized terrain prefetch fix mode.");
        if (profile.EngineOptimizations && profile.EngineTimerRefreshUs is < VrProfile.EngineTimerRefreshMin or > VrProfile.EngineTimerRefreshMax)
            Error("engine-timer", $"Streaming timer refresh must be {VrProfile.EngineTimerRefreshMin}–{VrProfile.EngineTimerRefreshMax} µs.");
        if (profile.EngineOptimizations && profile.EngineDevMode)
        {
            void DevPath(string? path, string extension, string what)
            {
                if (string.IsNullOrWhiteSpace(path)) { Error("engine-dev", $"Developer mode needs the {what}."); return; }
                if (!Path.IsPathFullyQualified(path.Trim()) || !path.Trim().EndsWith(extension, StringComparison.OrdinalIgnoreCase) || path.Any(char.IsControl) || path.Contains(';'))
                    Error("engine-dev", $"The {what} must be a full path to a {extension} file.");
                // DCS is not long-path aware: the module cannot open a path of 260 characters or more.
                else if (path.Trim().Length >= 260)
                    Error("engine-dev", $"The {what} path is too long for DCS (260 characters or more); use a shorter folder.");
                else if (!File.Exists(path.Trim()))
                    issues.Add(new("engine-dev-missing", IssueSeverity.Warning, $"The {what} does not exist yet ({path.Trim()}); until it does, the module uses its installed copy."));
            }
            DevPath(profile.EngineDevPayloadPath, ".dll", "developer payload");
            DevPath(profile.EngineDevIniPath, ".ini", "developer settings file");
        }
        if (profile.UsesLowerMonitor && (profile.FlightDisplayWidth is < 640 or > 15360 || profile.FlightDisplayHeight is < 480 or > 8640 || profile.FlightDisplayRefresh is < 24 or > 1000))
            Error("monitor-mode", "Select a monitor mode from the list for Lower the monitor while flying.");
        if (profile.QuadViews == QuadProvider.QuadViewsFoveated && string.IsNullOrWhiteSpace(profile.QuadViewsLayerDirectory) && (profile.FoveaWidth > .9 || profile.FoveaHeight > .9))
            issues.Add(new("quad-coverage-cap", IssueSeverity.Warning, "Bundled Quad Views renders a focus area of at most 90% of the view in each direction, which is Horizontal and Vertical FOV 10% in Pimax Play's Quick units. Lower values stay in the profile but are rendered as 10%."));
        if (profile.FrameGenFactor is not (VrProfile.FrameGenAuto or 2 or 3))
            Error("framegen-factor", "Select Auto, 2× or 3× for Generated frames.");
        // In-flight keys: any key with Ctrl/Alt/Shift (or an F-key, Pause, Scroll Lock … alone). A key DCS binds by
        // default still works, so it is a warning; the two keys must differ when both are in use.
        void Key(string? value, string fallback, bool used, string code, string what)
        {
            if (!NeuralHotkeys.TryParse(value ?? fallback, out var key)) { Error(code, $"The {what} key is not a recognized key. Record it again."); return; }
            if (NeuralHotkeys.Problem(key) is { } problem) { Error(code, $"{char.ToUpperInvariant(what[0])}{what[1..]} key: {problem}"); return; }
            if (used && NeuralHotkeys.IsDcsDefault(key))
                issues.Add(new(code + "-dcs", IssueSeverity.Warning, $"{NeuralHotkeys.Label(key)} ({what}) is a DCS default key binding: DCS reacts to it too. Choose another combination unless you have unbound it in DCS."));
        }
        Key(profile.NeuralToggleKey, NeuralHotkeys.Default, profile.NeuralRendering, "neural-hotkey", "DLSS 5 toggle");
        Key(profile.DiagnosticOverlayKey, NeuralHotkeys.DiagnosticDefault, profile.UsesHeadsetPanel, "diagnostic-hotkey", "in-headset panel");
        if (profile.NeuralRendering && profile.UsesHeadsetPanel && NeuralHotkeys.TryParse(profile.NeuralToggleKey ?? NeuralHotkeys.Default, out var nrKey) && !nrKey.IsOff
            && NeuralHotkeys.Same(profile.NeuralToggleKey ?? NeuralHotkeys.Default, profile.DiagnosticOverlayKey ?? NeuralHotkeys.DiagnosticDefault))
            Error("hotkey-conflict", "The DLSS 5 toggle and the in-headset panel need different keys.");
        // The module starts its 3-minute test suite on Ctrl+Alt+F11 with any other modifier too: no in-flight key may use
        // its keys while it runs.
        if (profile.EngineOptimizations && profile.NeuralRendering && NeuralHotkeys.TryParse(profile.NeuralToggleKey ?? NeuralHotkeys.Default, out var nrSuite) && !nrSuite.IsOff && NeuralHotkeys.EngineModuleKey(nrSuite))
            Error("neural-hotkey", $"{NeuralHotkeys.Label(nrSuite)} is reserved by Engine Optimizations (Ctrl+Alt+F8 to F12, Page Up and Page Down). Choose another key.");
        if (profile.EngineOptimizations && profile.UsesHeadsetPanel && NeuralHotkeys.TryParse(profile.DiagnosticOverlayKey ?? NeuralHotkeys.DiagnosticDefault, out var panelSuite) && !panelSuite.IsOff && NeuralHotkeys.EngineModuleKey(panelSuite))
            Error("diagnostic-hotkey", $"{NeuralHotkeys.Label(panelSuite)} is reserved by Engine Optimizations (Ctrl+Alt+F8 to F12, Page Up and Page Down). Choose another key.");
        Key(profile.EngineToggleKey, NeuralHotkeys.EngineDefault, profile.EngineOptimizations, "engine-hotkey", "Engine Optimizations switch");
        // The module's own keys (Ctrl+Alt+F8 to F12, Ctrl+Alt+Page Up/Down) stay reserved for it.
        if (profile.EngineOptimizations && NeuralHotkeys.TryParse(profile.EngineToggleKey ?? NeuralHotkeys.EngineDefault, out var engineKey) && !engineKey.IsOff)
        {
            if (NeuralHotkeys.EngineModuleKey(engineKey))
                Error("engine-hotkey", $"{NeuralHotkeys.Label(engineKey)} is reserved by Engine Optimizations (Ctrl+Alt+F8 to F12, Page Up and Page Down). Choose another key.");
            if ((profile.NeuralRendering && NeuralHotkeys.Same(profile.EngineToggleKey ?? NeuralHotkeys.EngineDefault, profile.NeuralToggleKey ?? NeuralHotkeys.Default))
                || (profile.UsesHeadsetPanel && NeuralHotkeys.Same(profile.EngineToggleKey ?? NeuralHotkeys.EngineDefault, profile.DiagnosticOverlayKey ?? NeuralHotkeys.DiagnosticDefault)))
                Error("hotkey-conflict", "The Engine Optimizations switch needs a key of its own: the DLSS 5 toggle or the in-headset panel uses it.");
        }
        // Pupil shift's switch must differ from every other in-flight key in use.
        Key(profile.PupilShiftToggleKey, NeuralHotkeys.PupilDefault, profile.UsesPupilShift, "pupil-hotkey", "Pupil shift switch");
        if (profile.UsesPupilShift && NeuralHotkeys.TryParse(profile.PupilShiftToggleKey ?? NeuralHotkeys.PupilDefault, out var pupilKey) && !pupilKey.IsOff)
        {
            var pupil = profile.PupilShiftToggleKey ?? NeuralHotkeys.PupilDefault;
            if (profile.EngineOptimizations && NeuralHotkeys.EngineModuleKey(pupilKey))
                Error("pupil-hotkey", $"{NeuralHotkeys.Label(pupilKey)} is reserved by Engine Optimizations (Ctrl+Alt+F8 to F12, Page Up and Page Down). Choose another key.");
            if ((profile.NeuralRendering && NeuralHotkeys.Same(pupil, profile.NeuralToggleKey ?? NeuralHotkeys.Default))
                || (profile.UsesHeadsetPanel && NeuralHotkeys.Same(pupil, profile.DiagnosticOverlayKey ?? NeuralHotkeys.DiagnosticDefault))
                || (profile.EngineOptimizations && NeuralHotkeys.Same(pupil, profile.EngineToggleKey ?? NeuralHotkeys.EngineDefault)))
                Error("hotkey-conflict", "The Pupil shift switch needs a key of its own: DLSS 5, the in-headset panel or Engine Optimizations use it.");
        }
        // Depth boost's switch likewise, Pupil shift's included.
        Key(profile.DepthBoostToggleKey, NeuralHotkeys.DepthBoostDefault, profile.UsesDepthBoost, "depth-hotkey", "Depth boost switch");
        if (profile.UsesDepthBoost && NeuralHotkeys.TryParse(profile.DepthBoostToggleKey ?? NeuralHotkeys.DepthBoostDefault, out var depthKey) && !depthKey.IsOff)
        {
            var depth = profile.DepthBoostToggleKey ?? NeuralHotkeys.DepthBoostDefault;
            if (profile.EngineOptimizations && NeuralHotkeys.EngineModuleKey(depthKey))
                Error("depth-hotkey", $"{NeuralHotkeys.Label(depthKey)} is reserved by Engine Optimizations (Ctrl+Alt+F8 to F12, Page Up and Page Down). Choose another key.");
            if ((profile.NeuralRendering && NeuralHotkeys.Same(depth, profile.NeuralToggleKey ?? NeuralHotkeys.Default))
                || (profile.UsesHeadsetPanel && NeuralHotkeys.Same(depth, profile.DiagnosticOverlayKey ?? NeuralHotkeys.DiagnosticDefault))
                || (profile.EngineOptimizations && NeuralHotkeys.Same(depth, profile.EngineToggleKey ?? NeuralHotkeys.EngineDefault))
                || (profile.UsesPupilShift && NeuralHotkeys.Same(depth, profile.PupilShiftToggleKey ?? NeuralHotkeys.PupilDefault)))
                Error("hotkey-conflict", "The Depth boost switch needs a key of its own: DLSS 5, the in-headset panel, Engine Optimizations or Pupil shift use it.");
        }
        // Far depth's switch, read by the bundled Quad Views: a key of its own as well.
        Key(profile.FarDepthToggleKey, NeuralHotkeys.FarDepthDefault, profile.UsesFarDepth, "far-depth-hotkey", "Far depth switch");
        if (profile.UsesFarDepth && NeuralHotkeys.TryParse(profile.FarDepthToggleKey ?? NeuralHotkeys.FarDepthDefault, out var farKey) && !farKey.IsOff)
        {
            var far = profile.FarDepthToggleKey ?? NeuralHotkeys.FarDepthDefault;
            if (profile.EngineOptimizations && NeuralHotkeys.EngineModuleKey(farKey))
                Error("far-depth-hotkey", $"{NeuralHotkeys.Label(farKey)} is reserved by Engine Optimizations (Ctrl+Alt+F8 to F12, Page Up and Page Down). Choose another key.");
            if ((profile.NeuralRendering && NeuralHotkeys.Same(far, profile.NeuralToggleKey ?? NeuralHotkeys.Default))
                || (profile.UsesHeadsetPanel && NeuralHotkeys.Same(far, profile.DiagnosticOverlayKey ?? NeuralHotkeys.DiagnosticDefault))
                || (profile.EngineOptimizations && NeuralHotkeys.Same(far, profile.EngineToggleKey ?? NeuralHotkeys.EngineDefault))
                || (profile.UsesPupilShift && NeuralHotkeys.Same(far, profile.PupilShiftToggleKey ?? NeuralHotkeys.PupilDefault))
                || (profile.UsesDepthBoost && NeuralHotkeys.Same(far, profile.DepthBoostToggleKey ?? NeuralHotkeys.DepthBoostDefault)))
                Error("hotkey-conflict", "The Far depth switch needs a key of its own: another in-flight switch uses it.");
        }
        // The color mode key (Visual realism, off by default) likewise.
        Key(profile.ColorModeKey, NeuralHotkeys.ColorModeDefault, profile.UsesColorModeKey, "color-hotkey", "color mode");
        if (profile.UsesColorModeKey && NeuralHotkeys.TryParse(profile.ColorModeKey, out var colorKey))
        {
            var color = profile.ColorModeKey;
            if (profile.EngineOptimizations && NeuralHotkeys.EngineModuleKey(colorKey))
                Error("color-hotkey", $"{NeuralHotkeys.Label(colorKey)} is reserved by Engine Optimizations (Ctrl+Alt+F8 to F12, Page Up and Page Down). Choose another key.");
            if ((profile.NeuralRendering && NeuralHotkeys.Same(color, profile.NeuralToggleKey ?? NeuralHotkeys.Default))
                || (profile.UsesHeadsetPanel && NeuralHotkeys.Same(color, profile.DiagnosticOverlayKey ?? NeuralHotkeys.DiagnosticDefault))
                || (profile.EngineOptimizations && NeuralHotkeys.Same(color, profile.EngineToggleKey ?? NeuralHotkeys.EngineDefault))
                || (profile.UsesPupilShift && NeuralHotkeys.Same(color, profile.PupilShiftToggleKey ?? NeuralHotkeys.PupilDefault))
                || (profile.UsesDepthBoost && NeuralHotkeys.Same(color, profile.DepthBoostToggleKey ?? NeuralHotkeys.DepthBoostDefault))
                || (profile.UsesFarDepth && NeuralHotkeys.Same(color, profile.FarDepthToggleKey ?? NeuralHotkeys.FarDepthDefault)))
                Error("hotkey-conflict", "The color mode key needs a key of its own: another in-flight switch uses it.");
        }
        // NVIDIA-only processing on a PC without an NVIDIA RTX card would install files that cannot run.
        if (!profile.Desktop && !GpuInfo.Unknown && !GpuInfo.HasNvidiaRtx)
        {
            if (profile.UsesCheeky)
                Error("dlss-gpu", $"DLSS 5 and Foveated Super Resolution need an NVIDIA RTX graphics card; this PC has {GpuInfo.Names}. Turn them off on the DLSS 5 page.");
            if (profile.FrameGen == FrameGeneration.Nvidia)
                Error("framegen-gpu", $"NVIDIA optical-flow frame generation needs an NVIDIA RTX graphics card; this PC has {GpuInfo.Names}. Choose FidelityFX on the Framegen page: it runs on any graphics card.");
        }
        // NVIDIA's Threaded optimization Off (a common DCS VR tweak) makes the driver report no D3D11 command lists to
        // DCS, so the multi-threaded shadows and G-buffer cannot run. Said before the flight; the app never changes it.
        if (profile.EngineOptimizations && (profile.EngineShadowRecorder || profile.EngineGBufferRecorder)
            && NvidiaSettings.ThreadedOptimizationForDcs() is { Value: ThreadedOptimization.Off } threaded)
            issues.Add(new("nvidia-threaded-optimization", IssueSeverity.Warning, $"NVIDIA's Threaded optimization is Off in {threaded.Where}. With it off the driver gives DCS no D3D11 command lists, so Multi-threaded shadows and G-buffer (the biggest engine gains) cannot run; everything else still does. {threaded.Fix}, then launch DCS again."));
        // MFAA on has the same effect (found by a pilot, GitHub issue #1).
        if (profile.EngineOptimizations && (profile.EngineShadowRecorder || profile.EngineGBufferRecorder)
            && NvidiaSettings.MfaaForDcs() is { On: true } mfaa)
            issues.Add(new("nvidia-mfaa", IssueSeverity.Warning, $"NVIDIA's Antialiasing - MFAA is On in {mfaa.Where}. With it on the driver gives DCS no D3D11 command lists, so Multi-threaded shadows and G-buffer (the biggest engine gains) cannot run; everything else still does. {mfaa.Fix}, then launch DCS again."));
        if (!profile.Desktop && profile.EyeDistance == EyeDistanceSource.Manual && !(profile.EyeDistanceMm is >= VrProfile.EyeDistanceMin and <= VrProfile.EyeDistanceMax))
            Error("eye-distance", $"Eye distance must be between {VrProfile.EyeDistanceMin:0} and {VrProfile.EyeDistanceMax:0} mm.");
        if (profile.NeuralRendering && string.IsNullOrWhiteSpace(profile.NeuralRuntimePath))
            Error("neural-runtime", "DLSS 5 needs NVIDIA's runtime file, nvngx_dlssnr.dll. Select it on the DLSS 5 page: DCS Control keeps a verified copy, so you select it only once.");
        if (inventory is not null)
        {
            if (inventory.DcsRunning) Error("dcs-running", "Close DCS before applying or restoring a profile.");
            // Checked before anything is written, so Launch never stops half way on a protected folder.
            foreach (var folder in LaunchSafety.UnwritableFolders(profile, inventory))
                // Not blocking: Launch DCS asks Windows for the permission itself (one prompt) before writing anything.
                issues.Add(LaunchSafety.PermissionsDenyWrite(folder) && LaunchSafety.CanGrant(folder)
                    ? new("write-access", IssueSeverity.Warning, $"DCS Control cannot write to {folder} yet (usual for DCS under Program Files). Launch DCS asks Windows for permission with one prompt, or press Grant access now.")
                    : new("write-blocked", IssueSeverity.Error, $"DCS Control cannot write to {folder}. {LaunchSafety.BlockedAdvice}"));
            // Not blocking: without a bridge Quad Views reports no eye tracking and keeps a fixed focus region.
            if (GazeBridge.Required(profile) && GazeBridge.Find(inventory) is null && !GazeBridge.RuntimeProvidesGaze(inventory))
                issues.Add(new("gaze-bridge-missing", IssueSeverity.Warning, "No eye-tracking bridge for SteamVR was found, so on the Sboys route the focus area stays fixed in the center. Install OpenXR-Eye-Trackers for an eye-tracked focus area."));
            if (inventory.DcsExecutable is null) Error("dcs-not-found", "DCS was not found. Select DCS.exe in the bin folder of your DCS installation.");
            else if (profile.SmoothCursor && profile.FrameGen != FrameGeneration.Off &&
                     !File.Exists(Path.Combine(Path.GetDirectoryName(inventory.DcsExecutable) ?? "", "Visualizer.dll")))
                issues.Add(new("smooth-cursor-images", IssueSeverity.Info, "Smooth mouse cursor: DCS's cursor images (Visualizer.dll) were not found next to DCS.exe. Frame generation then takes them from the running game, or draws a plain arrow."));
            if (inventory.OptionsPath is null) Error("options-not-found", @"DCS's settings file (Saved Games\DCS\Config\options.lua) was not found." + " If DCS was never started on this PC, start it once to the main menu and close it, then press Rescan this PC; otherwise select the options.lua DCS uses.");
            // The Saved Games folder (Scripts, Logs, the engine optimizations) is found from options.lua's place: one
            // picked elsewhere would put them in a folder DCS never reads.
            else if (!IsSavedGamesOptions(inventory.OptionsPath))
                issues.Add(new("options-location", IssueSeverity.Warning, @"The selected options.lua is not in a DCS Saved Games folder (Saved Games\DCS\Config\options.lua). DCS reads only that one, and the engine optimizations and logs are found next to it: select the options.lua DCS uses."));
            if (profile.Runtime == RuntimeKind.SboysSteamVr && !profile.Desktop && inventory.SteamVrRuntime is null && profile.RuntimeManifestPath is null)
                Error("steamvr-not-found", "SteamVR was not found on this PC, and the Sboys route needs it. Install SteamVR and start it once.");
            // The Periphery DLSS preset changes DCS's own DLSS: with another upscaler it does nothing (DCS Control does not
            // change DCS's Upscaling for it).
            var dlss = DcsUsesDlss(profile, inventory);
            if (profile.UsesPeripheryDlssPreset && dlss == false)
                issues.Add(new("periphery-preset-dlss", IssueSeverity.Warning, "Periphery DLSS preset only works when DCS upscales with DLSS. Set Upscaling to DLSS in DCS's settings."));
            // Read only: the preset the NVIDIA App forces on DCS, worth knowing at Quad Views sizes. Shown only while DCS
            // uses DLSS in VR and the driver forces a preset; the app never changes NVIDIA settings.
            if (!profile.Desktop && dlss == true && NvidiaSettings.DlssPresetForDcs() is { Forced: true } forced)
                issues.Add(new("nvidia-dlss-preset", IssueSeverity.Info, DlssPresetNote(profile, forced)));
        }
        return issues;
    }

    /// <summary>DCS upscales with DLSS: options.lua's graphics.Upscaling, or DLSS 5 / Foveated SR (Launch DCS sets it for
    /// them); null when options.lua does not say.</summary>
    public static bool? DcsUsesDlss(VrProfile profile, InventorySnapshot inventory) =>
        profile.UsesCheeky ? true
        : inventory.DcsSettings.TryGetValue("graphics.Upscaling", out var raw) ? raw.Trim().Trim('"').Equals("DLSS", StringComparison.OrdinalIgnoreCase)
        : null;

    /// <summary>The Checks note for a DLSS preset the NVIDIA App forces on DCS.</summary>
    public static string DlssPresetNote(VrProfile profile, DlssPresetSetting forced)
    {
        var name = forced.Name == "latest" ? "the latest DLSS preset" : "DLSS preset " + forced.Name;
        var option = profile.UsesPeripheryDlssPreset
            ? "the Periphery DLSS preset you turned on lightens the periphery without changing NVIDIA settings."
            : "the Periphery DLSS preset option (DLSS 5 page) can lighten the periphery without changing NVIDIA settings.";
        return forced.Name == "M"
            ? $"The NVIDIA App forces {name} for DCS. At Quad Views sizes, K costs about 0.6 ms less; {option}"
            : $"The NVIDIA App forces {name} for DCS; {option}";
    }
}
