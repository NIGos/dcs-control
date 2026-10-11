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

    public static IReadOnlyList<ValidationIssue> Validate(VrProfile profile, InventorySnapshot? inventory = null)
    {
        // What will run: Optimizations only is never held back by the VR settings it keeps for later.
        profile = profile.ForLaunch();
        var issues = new List<ValidationIssue>();
        void Error(string code, string text) => issues.Add(new(code, IssueSeverity.Error, text));
        void Range(string code, double value, double min, double max)
        {
            if (!double.IsFinite(value) || value < min || value > max) Error(code, $"{code}: allowed range is {min} to {max}.");
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
        if (!VrProfile.NeuralAreas.Contains(profile.NeuralFocusArea)) Error("neural-area", "DLSS 5 area must be the whole focus area or its central 80%, 70% or 50%.");
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
            issues.Add(new("quad-focus", IssueSeverity.Info, "DLSS 5 runs on the two Quad Views focus views through the focus adapter, on DCS's own DLSS; the periphery keeps DCS's own image."));
        if (profile.Runtime == RuntimeKind.Pimax && profile.QuadViews == QuadProvider.QuadViewsFoveated)
            issues.Add(new("pimax-native-replaced", IssueSeverity.Info, "Bundled Quad Views replaces the Pimax runtime's own Quad Views for DCS; keep Quad Views on in Pimax Play (the tested setup)." + (PimaxFovea.UsesPimaxPlay(profile)
                ? "" : " Its focus area comes from this profile; Pimax Play's Quad View settings do not apply.")));
        if (!Enum.IsDefined(profile.FoveaSource)) Error("fovea-source", "Select Pimax Play or this profile as the focus area source.");
        if (profile.CpuBoost || profile.FreeVram)
        {
            // Names are process names without ".exe". Moving or closing a process that starts DCS (or DCS itself) would
            // slow down or end the game it is meant to help.
            static string Normalize(string name) => name.Trim() is var n && n.EndsWith(".exe", StringComparison.OrdinalIgnoreCase) ? n[..^4] : n.Trim();
            var lists = new List<(string Label, IReadOnlyList<string>? Names)>();
            if (profile.FreeVram) lists.Add(("close", profile.FreeVramApps));
            if (profile.CpuBoost && profile.BoostMoveBackgroundApps) lists.Add(("move", profile.BoostBackgroundApps));
            foreach (var (label, names) in lists)
            {
                if (names is null) continue;
                if (names.Any(string.IsNullOrWhiteSpace)) Error("boost-app-name", "CPU Boost app names cannot be empty. Remove the empty entry from the " + label + " list.");
                foreach (var name in names.Where(n => !string.IsNullOrWhiteSpace(n)).Select(Normalize).Distinct(StringComparer.OrdinalIgnoreCase))
                {
                    // A pattern of wildcards only ("*", "?*", "*.exe") would match every process.
                    if (name.All(c => c is '*' or '?'))
                    { Error("boost-app-wildcard", $"CPU Boost cannot {label} '{name}': it would match every process. Enter an app name; * and ? are allowed only as part of a name."); continue; }
                    // Wildcards are matched exactly as at run time, so "D*" or "s*" is refused like "DCS" or "steam".
                    var hit = BoostProtectedProcesses.FirstOrDefault(p => BoostPlanner.MatchesProcessName(name, p));
                    if (hit is not null)
                        Error("boost-launcher", (string.Equals(hit, name, StringComparison.OrdinalIgnoreCase)
                            ? $"CPU Boost cannot {label} {name}: it "
                            : $"CPU Boost cannot {label} '{name}': it matches {hit}, which ") + $"starts DCS, is DCS itself or is a protected Windows process. Remove it from the {label} list.");
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
            Error("pupil-image", $"Virtual image distance must be {VrProfile.PupilShiftVirtualImageMin}–{VrProfile.PupilShiftVirtualImageMax} m.");
        if (profile.PupilShift && !profile.Desktop && !profile.UsesPupilShift)
            issues.Add(new("pupil-gaze", IssueSeverity.Warning, "Pupil shift needs eye-tracked Quad Views: it reads the gaze from the focus views. With this profile it is not installed."));
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
                else if (!File.Exists(path.Trim()))
                    issues.Add(new("engine-dev-missing", IssueSeverity.Warning, $"The {what} does not exist yet ({path.Trim()}); until it does, the module uses its installed copy."));
            }
            DevPath(profile.EngineDevPayloadPath, ".dll", "developer payload");
            DevPath(profile.EngineDevIniPath, ".ini", "developer settings file");
        }
        if (profile.UsesLowerMonitor && (profile.FlightDisplayWidth is < 640 or > 15360 || profile.FlightDisplayHeight is < 480 or > 8640 || profile.FlightDisplayRefresh is < 24 or > 1000))
            Error("monitor-mode", "Select a monitor mode from the list for Lower the monitor while flying.");
        if (profile.QuadViews == QuadProvider.QuadViewsFoveated && string.IsNullOrWhiteSpace(profile.QuadViewsLayerDirectory) && (profile.FoveaWidth > .9 || profile.FoveaHeight > .9))
            issues.Add(new("quad-coverage-cap", IssueSeverity.Warning, "Bundled Quad Views renders a focus of at most 90% of the view per axis, which is Horizontal and Vertical FOV 10% in Pimax Play's Quick units. Lower values are kept in the profile and rendered as 10%."));
        if (profile.FrameGenFactor is not (VrProfile.FrameGenAuto or 2 or 3))
            Error("framegen-factor", "Frame generation produces 2 or 3 frames per rendered frame, or picks between them automatically.");
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
        Key(profile.DiagnosticOverlayKey, NeuralHotkeys.DiagnosticDefault, profile.FrameGen != FrameGeneration.Off, "diagnostic-hotkey", "diagnostic panel");
        if (profile.NeuralRendering && profile.FrameGen != FrameGeneration.Off && NeuralHotkeys.TryParse(profile.NeuralToggleKey ?? NeuralHotkeys.Default, out var nrKey) && !nrKey.IsOff
            && NeuralHotkeys.Same(profile.NeuralToggleKey ?? NeuralHotkeys.Default, profile.DiagnosticOverlayKey ?? NeuralHotkeys.DiagnosticDefault))
            Error("hotkey-conflict", "The DLSS 5 toggle and the diagnostic panel need different keys.");
        Key(profile.EngineToggleKey, NeuralHotkeys.EngineDefault, profile.EngineOptimizations, "engine-hotkey", "engine optimizations switch");
        // The module's own keys (Ctrl+Alt+F8 to F12, Ctrl+Alt+Page Up/Down) stay reserved for it.
        if (profile.EngineOptimizations && NeuralHotkeys.TryParse(profile.EngineToggleKey ?? NeuralHotkeys.EngineDefault, out var engineKey) && !engineKey.IsOff)
        {
            if (engineKey.Modifiers == 3 && engineKey.VirtualKey is >= 0x77 and <= 0x7B or 0x21 or 0x22)
                Error("engine-hotkey", $"{NeuralHotkeys.Label(engineKey)} is used by the engine optimizations' test suite. Choose another key.");
            if ((profile.NeuralRendering && NeuralHotkeys.Same(profile.EngineToggleKey ?? NeuralHotkeys.EngineDefault, profile.NeuralToggleKey ?? NeuralHotkeys.Default))
                || (profile.FrameGen != FrameGeneration.Off && NeuralHotkeys.Same(profile.EngineToggleKey ?? NeuralHotkeys.EngineDefault, profile.DiagnosticOverlayKey ?? NeuralHotkeys.DiagnosticDefault)))
                Error("hotkey-conflict", "The engine optimizations switch needs a key of its own (the DLSS 5 toggle or the diagnostic panel uses it).");
        }
        // Pupil shift's switch must differ from every other in-flight key in use.
        Key(profile.PupilShiftToggleKey, NeuralHotkeys.PupilDefault, profile.UsesPupilShift, "pupil-hotkey", "pupil shift switch");
        if (profile.UsesPupilShift && NeuralHotkeys.TryParse(profile.PupilShiftToggleKey ?? NeuralHotkeys.PupilDefault, out var pupilKey) && !pupilKey.IsOff)
        {
            var pupil = profile.PupilShiftToggleKey ?? NeuralHotkeys.PupilDefault;
            if (profile.EngineOptimizations && pupilKey.Modifiers == 3 && pupilKey.VirtualKey is >= 0x77 and <= 0x7B or 0x21 or 0x22)
                Error("pupil-hotkey", $"{NeuralHotkeys.Label(pupilKey)} is used by the engine optimizations' test suite. Choose another key.");
            if ((profile.NeuralRendering && NeuralHotkeys.Same(pupil, profile.NeuralToggleKey ?? NeuralHotkeys.Default))
                || (profile.FrameGen != FrameGeneration.Off && NeuralHotkeys.Same(pupil, profile.DiagnosticOverlayKey ?? NeuralHotkeys.DiagnosticDefault))
                || (profile.EngineOptimizations && NeuralHotkeys.Same(pupil, profile.EngineToggleKey ?? NeuralHotkeys.EngineDefault)))
                Error("hotkey-conflict", "The pupil shift switch needs a key of its own (DLSS 5, the diagnostic panel or the engine optimizations use it).");
        }
        // NVIDIA-only processing on a PC without an NVIDIA RTX card would install files that cannot run.
        if (!profile.Desktop && !GpuInfo.Unknown && !GpuInfo.HasNvidiaRtx)
        {
            if (profile.UsesCheeky)
                Error("dlss-gpu", $"DLSS 5 and Foveated Super Resolution need an NVIDIA RTX graphics card; this PC has {GpuInfo.Names}. Turn them off on the DLSS 5 page.");
            if (profile.FrameGen == FrameGeneration.Nvidia)
                Error("framegen-gpu", $"NVIDIA optical-flow frame generation needs an NVIDIA RTX graphics card; this PC has {GpuInfo.Names}. Choose FidelityFX on the Framegen page: it runs on any graphics card.");
        }
        if (profile.NeuralRendering && string.IsNullOrWhiteSpace(profile.NeuralRuntimePath))
            Error("neural-runtime", "Select a compatible NVIDIA nvngx_dlssnr.dll for DLSS 5. The app keeps a verified copy, so it is asked for once.");
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
                issues.Add(new("gaze-bridge-missing", IssueSeverity.Warning, "No enabled OpenXR gaze bridge was found. Eye-tracked focus on the Sboys route falls back to a fixed focus region until OpenXR-Eye-Trackers is installed."));
            if (inventory.DcsExecutable is null) Error("dcs-not-found", "Select the DCS executable you use for rendering.");
            else if (profile.SmoothCursor && profile.FrameGen != FrameGeneration.Off &&
                     !File.Exists(Path.Combine(Path.GetDirectoryName(inventory.DcsExecutable) ?? "", "Visualizer.dll")))
                issues.Add(new("smooth-cursor-images", IssueSeverity.Info, "Smooth mouse cursor: DCS's cursor images (Visualizer.dll) are not next to DCS.exe. Frame generation reads them from the running game, or uses a plain arrow."));
            if (inventory.OptionsPath is null) Error("options-not-found", @"DCS's settings file (Saved Games\DCS\Config\options.lua) was not found." + " If DCS was never started on this PC, start it once to the main menu and close it, then press Rescan this PC; otherwise select the options.lua DCS uses.");
            if (profile.Runtime == RuntimeKind.SboysSteamVr && !profile.Desktop && inventory.SteamVrRuntime is null && profile.RuntimeManifestPath is null)
                Error("steamvr-not-found", "SteamVR OpenXR manifest not found.");
        }
        return issues;
    }
}
