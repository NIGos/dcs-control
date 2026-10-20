using System.Globalization;
using System.Text;
using System.Text.Json;

namespace DcsVr.Core;

/// <param name="OfxrLayerDll">OFXR layer built from the djules75 fork (0.2.9.1, scripts/build-ofxr-djules75.ps1), used for
/// every framegen profile. Upstream 0.2.1 deadlocks DCS: DCS waits for the next frame on a second thread while OFXR's
/// second cycle calls the runtime's xrWaitFrame on the render thread (reproduced offline and in the headset on
/// 2026-10-03). The fork also creates private swapchains only for submitted swapchains, which replaces our earlier
/// defer-until-submitted patch for Quad Views.</param>
/// <param name="EngineDirectory">DCS engine optimizations (native/dcsqvcull): DcsQvCull.dll, DcsQvCullPayload.dll and DcsQvCull.lua.</param>
/// <param name="PupilShiftDll">Pupil shift OpenXR layer (native/pupil_shift): XR_APILAYER_DCSVR_pupil_shift.dll.</param>
public sealed record ComponentLocations(string? OfxrDirectory, string? CheekyDirectory, string? CheekyLayerDll, string? QuadViewsDirectory = null, string? QuadFocusDirectory = null, string? OfxrLayerDll = null, string? EngineDirectory = null, string? PupilShiftDll = null);

public sealed class DeploymentPlanner(ComponentLocations components)
{
    public const string DcsSteamAppId = "223750";
    /// <summary>16:9 as DCS writes it to options.lua (graphics.aspect = 1.7777777777778).</summary>
    public const double SmallWindowAspect = 1.7777777777778;
    /// <param name="savedProfile">Written to profile.json instead of <paramref name="profile"/>: the user's own draft when
    /// <paramref name="profile"/> carries values converted from Pimax Play.</param>
    /// <param name="appliedFovea">The converted Pimax Play values, saved as pimax-fovea.json in the profile folder.</param>
    /// <param name="ownedSettings">options.lua settings DCS Control already changed, with their original values: the
    /// plan starts from options.lua with these set back, so a setting the new profile no longer needs returns to the
    /// user's value and the recorded "before" values are the user's own.</param>
    /// <param name="neuralSha256">The hash recorded for the app's saved DLSS 5 runtime copy when the profile uses it; the
    /// deployed bytes must match it.</param>
    /// <remarks>Existing files are never a conflict: Apply backs up an original the first time it writes a path and
    /// overwrites afterwards (see <see cref="OriginalsStore"/>).</remarks>
    public ApplyPlan Build(VrProfile profile, InventorySnapshot inventory, string managedRoot, VrProfile? savedProfile = null, AppliedFovea? appliedFovea = null, IReadOnlyDictionary<string, string?>? ownedSettings = null, string? neuralSha256 = null, byte[]? chainedDxgi = null)
    {
        // Optimizations only: none of the VR settings the profile keeps for later is deployed.
        profile = profile.ForLaunch();
        var issues = ProfileValidation.Validate(profile, inventory);
        var errors = issues.Where(i => i.Severity == IssueSeverity.Error).ToArray();
        if (errors.Length > 0) throw new InvalidDataException(string.Join(Environment.NewLine, errors.Select(i => i.Message)));
        var exe = Path.GetFullPath(inventory.DcsExecutable!);
        if (!Path.GetFileName(exe).Equals("DCS.exe", StringComparison.OrdinalIgnoreCase)) throw new InvalidDataException("Select DCS.exe.");
        var bin = Path.GetDirectoryName(exe)!;
        var profileRoot = PathPolicy.UnderRoot(managedRoot, "profiles/" + profile.Id);
        var layersRoot = PathPolicy.UnderRoot(profileRoot, "layers");
        var files = new List<FileMutation>();
        var environment = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
        var layerNames = new List<string>();
        var dependencies = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
        // Optimizations only: DCS uses whatever runtime the PC has (another headset) or none (a monitor); the app sets
        // no OpenXR variable at all, so none is required or pinned.
        if (!profile.Desktop)
        {
            var runtime = profile.RuntimeManifestPath ?? (profile.Runtime == RuntimeKind.Pimax ? inventory.PimaxRuntime : inventory.SteamVrRuntime);
            if (runtime is null || !File.Exists(runtime)) throw new InvalidDataException("OpenXR runtime manifest unavailable.");
            using (var document = JsonDocument.Parse(File.ReadAllText(runtime)))
            {
                var library = document.RootElement.GetProperty("runtime").GetProperty("library_path").GetString()
                    ?? throw new InvalidDataException("Runtime manifest is missing its library.");
                var runtimeLibrary = Path.GetFullPath(library, Path.GetDirectoryName(Path.GetFullPath(runtime))!);
                if (!File.Exists(runtimeLibrary)) throw new InvalidDataException("The OpenXR runtime library does not exist.");
                dependencies[runtimeLibrary] = Hashing.FileSha256(runtimeLibrary);
            }
            dependencies[Path.GetFullPath(runtime)] = Hashing.FileSha256(runtime);
            environment["XR_RUNTIME_JSON"] = Path.GetFullPath(runtime);
        }
        environment["DCSVR_QUAD_FOCUS"] = profile.UsesQuadFocus ? "1" : "0";
        environment["DCSVR_NR_HOTKEY"] = profile.NeuralRendering ? NeuralHotkeys.Environment(profile.NeuralToggleKey) : "0:0";
        // The diagnostic panel is drawn by OFXR: with frame generation, or loaded for the panel alone (HeadsetPanel).
        var framegen = profile.UsesHeadsetPanel;
        environment["DCSVR_DIAG_HOTKEY"] = framegen ? NeuralHotkeys.Environment(profile.DiagnosticOverlayKey, NeuralHotkeys.DiagnosticDefault) : "0:0";
        environment["DCSVR_DIAG_OVERLAY"] = framegen && profile.DiagnosticOverlayAtStart ? "1" : "0";
        // The panel's Engine line names the in-flight switch of the engine optimizations (OFXR fork patch 0013).
        environment["DCSVR_ENGINE_HOTKEY"] = framegen && profile.EngineOptimizations ? NeuralHotkeys.Environment(profile.EngineToggleKey, NeuralHotkeys.EngineDefault) : "0:0";
        // And its Pupil shift line names the layer's switch (patch 0015).
        environment["DCSVR_PUPIL_HOTKEY"] = framegen && profile.UsesPupilShift ? NeuralHotkeys.Environment(profile.PupilShiftToggleKey, NeuralHotkeys.PupilDefault) : "0:0";
        // And its Depth boost line names Depth boost's switch (patch 0017).
        environment["DCSVR_DEPTH_HOTKEY"] = framegen && profile.UsesDepthBoost ? NeuralHotkeys.Environment(profile.DepthBoostToggleKey, NeuralHotkeys.DepthBoostDefault) : "0:0";
        // And its Far depth line names Far depth's switch (patch 0024).
        environment["DCSVR_FARDEPTH_HOTKEY"] = framegen && profile.UsesFarDepth ? NeuralHotkeys.Environment(profile.FarDepthToggleKey, NeuralHotkeys.FarDepthDefault) : "0:0";
        // The Steam edition restarts itself through Steam when started directly, and the new process loses this
        // environment (verified with DCS 2.9.30: the restarted process had no layers). Steam's own launch variables
        // tell the game it was already started by Steam, so the original process keeps the profile environment.
        if (exe.Contains(@"\steamapps\", StringComparison.OrdinalIgnoreCase)) { environment["SteamAppId"] = DcsSteamAppId; environment["SteamGameId"] = DcsSteamAppId; }

        var optionsPath = Path.GetFullPath(inventory.OptionsPath!);
        // Saved Games\DCS*\Scripts, next to Config: where the DCS engine optimizations' Lua hook and module go.
        var scripts = Path.Combine(Path.GetDirectoryName(Path.GetDirectoryName(optionsPath)!) ?? throw new InvalidDataException(@"options.lua must be in Saved Games\DCS\Config."), "Scripts");
        var optionsBytes = File.ReadAllBytes(optionsPath);
        var utf8 = StrictUtf8.Encoding;
        string text;
        try { text = utf8.GetString(optionsBytes); }
        // Hand-edited and saved in another encoding: nothing is written into it, and the pilot reads how to fix it.
        catch (DecoderFallbackException) { throw new InvalidDataException(optionsPath + " is not saved as UTF-8 (it was probably edited and saved in another encoding). Open it in Notepad, choose Save as → Encoding UTF-8, and launch again; DCS reads it either way."); }
        // Settings an earlier profile changed start from the user's own values.
        foreach (var (key, original) in ownedSettings ?? new Dictionary<string, string?>())
        {
            var lua = new LuaOptions(text); var keys = key.Split('.');
            if (LuaOptions.NormalizeLiteral(lua.Get(keys)) == LuaOptions.NormalizeLiteral(original)) continue;
            text = original is null ? lua.Remove(keys) : lua.SetLiteral(keys, original);
        }
        // Only what the selected pipeline needs, and only when the user's value differs: VR users already have VR,
        // quad views and DLSS set, and every other DCS option is left exactly as it is.
        var current = new LuaOptions(text);
        var optionChanges = new Dictionary<string, object>();
        void Require(string path, object value)
        {
            var literal = value switch { bool b => b ? "true" : "false", string text => "\"" + text + "\"", _ => Convert.ToString(value, CultureInfo.InvariantCulture)! };
            var existing = current.Get(path.Split('.'));
            bool same;
            try { same = existing is not null && LuaOptions.NormalizeLiteral(existing) == LuaOptions.NormalizeLiteral(literal); }
            catch (InvalidDataException) { same = false; }
            if (!same) optionChanges[path] = value;
        }
        // Optimizations only leaves DCS's VR setting as the user has it (another headset, or the monitor).
        if (!profile.Desktop) Require("VR.enable", true);
        // DCS asks for quad views only with this option on; stereo Cheeky needs it off, otherwise it is left alone.
        if (profile.QuadViews != QuadProvider.None) Require("VR.openxr_quadView", true);
        else if (profile.UsesCheeky && current.Get("VR", "openxr_quadView") is not null) Require("VR.openxr_quadView", false);
        // Cheeky and DLSS 5 work on DCS's DLSS Super Resolution.
        if (profile.UsesCheeky) Require("graphics.Upscaling", "DLSS");
        // Frame pacing is set for the headset; on a monitor DCS keeps the user's own frame cap and vertical sync.
        if (!profile.Desktop && FramePacing.RequestedCap(profile) is { } fpsCap) optionChanges["graphics.maxFPS"] = fpsCap;
        if (!profile.Desktop && profile.DisableDcsVSync) optionChanges["graphics.sync"] = false;
        // The DCS launcher restarts DCS.exe --restarted when Play is pressed, breaking away from any job object; inside a
        // job that forbids breakaway Windows refuses it (error 5, see LaunchSafety.CurrentJob). By default DCS goes
        // straight into the game; KeepDcsLauncher leaves the launcher alone (the restarted DCS inherits the profile
        // environment). Restored with the profile.
        if (!profile.KeepDcsLauncher && current.Get("miscellaneous") is not null) Require("miscellaneous.launcher", false);
        // DCS's custom IPD replaces each eye's sideways position after the OpenXR layers, which would undo Pupil shift's
        // sideways move and Depth boost's IPD scale: the layer applies the same IPD itself (Depth boost scales it) and
        // DCS's option is off while the profile is applied (owned like every key here: Back to stock DCS turns it on again).
        // Which eye distance DCS renders with (VrProfile.EyeDistance): the headset's own turns DCS's custom IPD off;
        // Manual sets it (through the layer when it runs, else through DCS's option); DcsCustom keeps DCS's value.
        double? pupilIpd = null;
        var dcsCustomOn = current.Get("VR", "custom_IPD_enable")?.Trim() == "true";
        if (!profile.Desktop)
            switch (profile.EyeDistance)
            {
                case EyeDistanceSource.Headset:
                    if (dcsCustomOn) Require("VR.custom_IPD_enable", false);
                    break;
                case EyeDistanceSource.Manual when profile.UsesLayerEyeDistance:
                    pupilIpd = profile.EyeDistanceMm;
                    if (dcsCustomOn) Require("VR.custom_IPD_enable", false);
                    break;
                case EyeDistanceSource.Manual:
                    Require("VR.custom_IPD_enable", true);
                    Require("VR.custom_IPD", profile.EyeDistanceMm.ToString("0.#", CultureInfo.InvariantCulture));
                    break;
                default:
                    // DCS's custom IPD would undo the layer's eye positions, so it is turned off whenever the layer runs; an
                    // IPD outside 40–90 mm (or missing) is not carried over and the layer keeps the headset's own.
                    if (profile.UsesLayerEyeDistance && dcsCustomOn)
                    {
                        pupilIpd = CustomIpd(current.Get("VR", "custom_IPD"));
                        Require("VR.custom_IPD_enable", false);
                    }
                    break;
            }
        // Small DCS window in VR: the desktop mirror becomes a 1280×720 window. The headset image is rendered at the
        // headset's own resolution either way; DCS and Windows only keep smaller desktop buffers. The aspect follows the
        // window when DCS stores one. Owned keys like every setting above: Back to stock DCS puts the user's values back.
        if (profile.UsesSmallDcsWindow && current.Get("graphics") is not null)
        {
            Require("graphics.width", VrProfile.SmallWindowWidth);
            Require("graphics.height", VrProfile.SmallWindowHeight);
            Require("graphics.fullScreen", false);
            if (current.Get("graphics", "aspect") is not null) Require("graphics.aspect", SmallWindowAspect);
        }
        // Existing autoexec commands remain untouched, but edits invalidate the saved launch contract.
        var autoexec = Path.Combine(Path.GetDirectoryName(optionsPath)!, "autoexec.cfg");
        if (File.Exists(autoexec)) dependencies[autoexec] = Hashing.FileSha256(autoexec);
        var patched = LuaOptions.Patch(text, optionChanges);
        var originalLua = new LuaOptions(text); var installedLua = new LuaOptions(patched);
        // Nothing to change: options.lua is not touched at all.
        if (optionChanges.Count > 0) Add(optionsPath, utf8.GetBytes(patched), "Profile DCS settings",
            luaChanges: optionChanges.Keys.Select(key => new LuaValueChange(key, originalLua.Get(key.Split('.')), installedLua.Get(key.Split('.'))!)).ToArray());

        if (profile.UsesCheeky)
        {
            var cheeky = RequireDirectory(profile.UsesQuadFocus ? components.QuadFocusDirectory : components.CheekyDirectory,
                profile.UsesQuadFocus ? "Verified focus adapter binaries are unavailable." : "Cheeky package has not been imported.");
            AddLoader(cheeky);
            foreach (var file in new[] { "CheekyFoveatedDLSSHost.dll", "CheekyFoveatedDLSSRuntime.dll" })
                Add(PathPolicy.UnderRoot(bin, "CheekyFoveatedDLSS/" + file), File.ReadAllBytes(PathPolicy.UnderRoot(cheeky, "CheekyFoveatedDLSS/" + file)), "Cheeky runtime", ownHashes: Variants("CheekyFoveatedDLSS/" + file));
            var cheekyIni = ConfigurationWriters.Cheeky(profile);
            Add(PathPolicy.UnderRoot(bin, "CheekyFoveatedDLSS/CheekyFoveatedDLSS.ini"), new UTF8Encoding(false).GetBytes(cheekyIni), "DLSS and gaze settings", iniValues: IniFile.Parse(cheekyIni));
            var layerDll = components.CheekyLayerDll ?? throw new InvalidDataException("Cheeky OpenXR layer unavailable.");
            var deployedDll = PathPolicy.UnderRoot(profileRoot, "cheeky/CheekyOpenXRLayer.dll");
            Add(deployedDll, File.ReadAllBytes(layerDll), "Cheeky OpenXR layer");
            AddText(PathPolicy.UnderRoot(layersRoot, "cheeky.json"), ConfigurationWriters.LayerManifest("XR_APILAYER_CHEEKY_foveated_dlss", deployedDll, 1, "DCS Control · Cheeky gaze"), "Profile Cheeky manifest");
            layerNames.Add("XR_APILAYER_CHEEKY_foveated_dlss");
            environment["CHEEKY_OPENXR_LAYER_DISABLE"] = "1";
            if (profile.NeuralRendering)
            {
                var neural = Path.GetFullPath(profile.NeuralRuntimePath!);
                if (!Path.GetFileName(neural).Equals("nvngx_dlssnr.dll", StringComparison.OrdinalIgnoreCase)) throw new InvalidDataException("The DLSS 5 runtime must be named nvngx_dlssnr.dll.");
                // Read once, then verify signature, architecture and version on a private copy of those bytes, so the
                // selected file cannot be swapped between the checks and the deployment.
                var neuralBytes = File.ReadAllBytes(neural);
                // The app's saved copy must still be the file that was verified and recorded when it was saved.
                if (neuralSha256 is not null && Hashing.BytesSha256(neuralBytes) != neuralSha256)
                    throw new InvalidDataException("The saved copy of the DLSS 5 runtime changed since it was saved. Select nvngx_dlssnr.dll again.");
                // A file the user chose (the saved copy or the path in the profile) may be NVIDIA's runtime patched for
                // RTX 20, 30 and 40 cards; Detect only ever picks validly signed files.
                NativeBinary.VerifyNeuralRuntime(neuralBytes, allowModified: true);
                Add(PathPolicy.UnderRoot(bin, "CheekyFoveatedDLSS/nvngx_dlssnr.dll"), neuralBytes, "User-supplied DLSS 5 runtime");
            }
        }

        // DCS engine optimizations: a Lua hook in Saved Games loads the module; nothing in the DCS install changes. The
        // loader and the hook are locked while DCS runs, like every file Apply writes (it refuses while DCS is open).
        // Depth boost or a Visual realism feature alone installs it too, in its Perception-only mode.
        if (profile.UsesEngineModule)
        {
            var engine = RequireDirectory(components.EngineDirectory, profile.EngineOptimizations ? "The DCS engine optimizations are unavailable." : profile.UsesDepthBoost ? "Depth boost's engine module is unavailable." : "Visual realism's engine module is unavailable.");
            var module = Path.Combine(scripts, "DcsQvCull");
            Add(Path.Combine(module, "DcsQvCull.dll"), File.ReadAllBytes(Path.Combine(engine, "DcsQvCull.dll")), "DCS engine optimizations loader",
                // What the module writes while DCS runs; all of it goes with the module on restore.
                runtimeLogs: [.. new[] { "DcsQvCull.log", "DcsQvCull.prev.log", "DcsQvCull.*.log", "DcsQvCullPrefetch.log", "DcsQvCullPrefetch.prev.log", "motion_*.csv", "rotation_*.csv", "timeline_*.csv", "events_*.csv", "fbc_*.csv", "run_profile.flag", "report_*.txt", "profile_*.txt", "bench_*.csv", "run_suite.flag", "run_motion.flag", "toggle_engine.flag", Path.Combine("payload", "active_*.dll"), Path.Combine("cache", "*.qvc"), Path.Combine("cache", "*.qvc.*.tmp"), "gbrec_states*.jsonl", Path.Combine("tone_probe", "*.f32") }.Select(name => Path.Combine(module, name))]);
            Add(Path.Combine(module, "payload", "DcsQvCullPayload.dll"), File.ReadAllBytes(Path.Combine(engine, "DcsQvCullPayload.dll")), "DCS engine optimizations");
            AddIni(Path.Combine(module, "DcsQvCull.ini"), ConfigurationWriters.DcsQvCull(profile), "DCS engine optimization settings");
            // Written last, removed first on restore: the hook never points at a module that is gone.
            Add(Path.Combine(scripts, "Hooks", "DcsQvCull.lua"), File.ReadAllBytes(Path.Combine(engine, "DcsQvCull.lua")), "DCS engine optimizations hook");
        }

        if (profile.QuadViews == QuadProvider.QuadViewsFoveated)
        {
            var useBundledProvider = string.IsNullOrWhiteSpace(profile.QuadViewsLayerDirectory);
            var provider = RequireDirectory(useBundledProvider ? components.QuadViewsDirectory : profile.QuadViewsLayerDirectory, "Quad Views provider unavailable.");
            var manifest = Directory.EnumerateFiles(provider, "*.json").FirstOrDefault(p => IsLayer(p, "XR_APILAYER_MBUCCHIA_quad_views_foveated"))
                ?? throw new InvalidDataException("Quad-Views-Foveated manifest not found.");
            var explicitManifest = ConfigurationWriters.ExplicitManifest(File.ReadAllText(manifest), manifest);
            if (!useBundledProvider)
            {
                using var alternative = JsonDocument.Parse(explicitManifest);
                var dll = alternative.RootElement.GetProperty("api_layer").GetProperty("library_path").GetString()!;
                dependencies[Path.GetFullPath(manifest)] = Hashing.FileSha256(manifest);
                dependencies[dll] = Hashing.FileSha256(dll);
            }
            if (useBundledProvider)
            {
                var node = System.Text.Json.Nodes.JsonNode.Parse(explicitManifest)!;
                var sourceDll = node["api_layer"]!["library_path"]!.GetValue<string>();
                var deployedDll = PathPolicy.UnderRoot(profileRoot, "quadviews/XR_APILAYER_MBUCCHIA_quad_views_foveated.dll");
                Add(deployedDll, File.ReadAllBytes(sourceDll), "Software Quad Views provider");
                node["api_layer"]!["library_path"] = deployedDll;
                explicitManifest = node.ToJsonString(JsonData.Options);
                var settings = PathPolicy.UnderRoot(profileRoot, "quadviews/settings.cfg");
                AddText(settings, File.ReadAllText(PathPolicy.UnderRoot(provider, "settings.cfg")) + "\n" + ConfigurationWriters.QuadViews(profile), "Profile Quad Views settings");
                environment["DCSVR_QUAD_SETTINGS"] = settings;
            }
            AddText(PathPolicy.UnderRoot(layersRoot, "quad-views.json"), explicitManifest, "Profile Quad Views manifest");
            layerNames.Add("XR_APILAYER_MBUCCHIA_quad_views_foveated");
            environment["DISABLE_XR_APILAYER_MBUCCHIA_quad_views_foveated"] = "1";
        }
        // Optimizations only keeps the user's own OpenXR layers (for example their own Quad Views) as they are.
        else if (!profile.Desktop) environment["DISABLE_XR_APILAYER_MBUCCHIA_quad_views_foveated"] = "1";

        if (profile.FrameGen != FrameGeneration.Off || profile.UsesHeadsetPanel)
        {
            // The fork build is used whenever it is available; the upstream package is only a fallback for stereo
            // profiles in development fixtures, because software Quad Views needs the fork's submitted-only swapchains.
            var forkDll = components.OfxrLayerDll is { } built && File.Exists(built) ? built : null;
            if (forkDll is null && profile.QuadViews == QuadProvider.QuadViewsFoveated)
                throw new InvalidDataException("Framegen needs the OFXR layer in components/ofxr. Rebuild the release with scripts/build-ofxr-djules75.ps1.");
            var ofxr = forkDll is null ? RequireDirectory(components.OfxrDirectory, "OFXR package has not been imported.") : null;
            var dll = PathPolicy.UnderRoot(profileRoot, "ofxr/XR_APILAYER_XRFrameBridge_diagnostic.dll");
            Add(dll, File.ReadAllBytes(forkDll ?? PathPolicy.UnderRoot(ofxr!, "ofxr/XR_APILAYER_XRFrameBridge_diagnostic.dll")), "OFXR framegen");
            AddIni(PathPolicy.UnderRoot(profileRoot, "ofxr/ofxr_bridge.ini"), ConfigurationWriters.Ofxr(profile, Path.GetDirectoryName(exe)), "Framegen settings");
            AddText(PathPolicy.UnderRoot(layersRoot, "ofxr.json"), ConfigurationWriters.LayerManifest("XR_APILAYER_XRFrameBridge_diagnostic", dll, forkDll is null ? 116 : 401, "DCS Control · OFXR Bridge"), "Profile OFXR manifest");
            layerNames.Add("XR_APILAYER_XRFrameBridge_diagnostic");
        }
        if (!profile.Desktop) environment["XRFG_DISABLE_OFXR_BRIDGE"] = "1";

        // The loader places implicit layers above every explicit one. An implicit gaze bridge would therefore sit
        // above Quad Views and Cheeky, and their eye-gaze calls would never reach it. Disable the implicit
        // registration for this process and load the same library explicitly, last, next to the runtime.
        if (GazeBridge.Required(profile) && GazeBridge.Find(inventory) is { } gaze)
        {
            var gazeText = File.ReadAllText(gaze.ManifestPath);
            var disable = GazeBridge.DisableVariable(gazeText)
                ?? throw new InvalidDataException("The gaze bridge manifest has no disable_environment, so it cannot be ordered below Quad Views: " + gaze.ManifestPath);
            var gazeManifest = ConfigurationWriters.ExplicitManifest(gazeText, gaze.ManifestPath);
            AddText(PathPolicy.UnderRoot(layersRoot, "gaze-bridge.json"), gazeManifest, "Profile gaze bridge manifest (below Quad Views)");
            using (var parsed = JsonDocument.Parse(gazeManifest))
            {
                var gazeDll = parsed.RootElement.GetProperty("api_layer").GetProperty("library_path").GetString()!;
                dependencies[gazeDll] = Hashing.FileSha256(gazeDll);
            }
            dependencies[Path.GetFullPath(gaze.ManifestPath)] = Hashing.FileSha256(gaze.ManifestPath);
            environment[disable] = "1";
            layerNames.Add(gaze.Name!);
        }
        // Pupil shift sits first, next to DCS: it changes only what DCS renders and puts the runtime's own poses and
        // FOVs back on the submitted views, so every layer below sees the frame as if it were not there. The same layer
        // carries Depth boost and publishes the gaze for Visual realism, so it is deployed for any of them.
        if (profile.UsesPerceptionLayer)
        {
            var source = components.PupilShiftDll is { } built && File.Exists(built) ? built : throw new InvalidDataException("The pupil shift layer is unavailable.");
            var folder = PathPolicy.UnderRoot(profileRoot, "pupilshift");
            var dll = PathPolicy.UnderRoot(folder, "XR_APILAYER_DCSVR_pupil_shift.dll");
            Add(dll, File.ReadAllBytes(source), "Pupil shift OpenXR layer", runtimeLogs: [PathPolicy.UnderRoot(folder, "PupilShift.log")]);
            var pupilIni = ConfigurationWriters.PupilShift(profile, pupilIpd);
            Add(PathPolicy.UnderRoot(folder, "PupilShift.ini"), new UTF8Encoding(false).GetBytes(pupilIni), "Pupil shift settings", iniValues: IniFile.Parse(pupilIni));
            AddText(PathPolicy.UnderRoot(layersRoot, "pupil-shift.json"), ConfigurationWriters.LayerManifest("XR_APILAYER_DCSVR_pupil_shift", dll, 1, "DCS Control · pupil shift"), "Profile pupil shift manifest");
            layerNames.Insert(0, "XR_APILAYER_DCSVR_pupil_shift");
        }
        // VectorXR is an implicit layer that sits above every add-on here: its frame generation, Quadviews and Depth
        // would stack on Framegen, Quad Views, Cheeky and Depth boost (two frame generators, two Quad Views providers). It
        // is turned off for DCS started with any VR profile; other games keep it.
        if (!profile.Desktop) environment[VectorXr.DisableVariable] = "1";
        if (layerNames.Count > 0)
        {
            environment["XR_API_LAYER_PATH"] = layersRoot;
            environment["XR_ENABLE_API_LAYERS"] = string.Join(';', layerNames);
        }
        else if (!profile.Desktop) { environment["XR_API_LAYER_PATH"] = ""; environment["XR_ENABLE_API_LAYERS"] = ""; }
        AddText(PathPolicy.UnderRoot(profileRoot, "profile.json"), JsonData.Serialize(savedProfile ?? profile), "Reproducible profile");
        if (appliedFovea is not null) AddText(PathPolicy.UnderRoot(profileRoot, AppliedFovea.FileName), JsonData.Serialize(appliedFovea), "Focus area values read from Pimax Play");
        AddText(PathPolicy.UnderRoot(profileRoot, "launch.json"), JsonData.Serialize(new LaunchConfiguration(exe, environment, dependencies, File.Exists(autoexec) ? [] : [autoexec])), "Profile launch environment");
        return new(profile.Id, profile.Name, files, environment, exe);

        // VR.custom_IPD in millimetres ("64" or 64); null outside 40-90 mm.
        static double? CustomIpd(string? raw) =>
            raw is not null && double.TryParse(raw.Trim().Trim('"'), System.Globalization.NumberStyles.Float, CultureInfo.InvariantCulture, out var mm) && mm is >= 40 and <= 90 ? mm : null;
        void AddText(string path, string content, string purpose) => Add(path, new UTF8Encoding(false).GetBytes(content), purpose);
        // Read by GetPrivateProfileString, which takes a file without a BOM as the ANSI code page: UTF-16 with a BOM keeps
        // paths with accented or Cyrillic letters (the DCS folder, Saved Games) intact.
        void AddIni(string path, string content, string purpose) => Add(path, [.. Encoding.Unicode.GetPreamble(), .. Encoding.Unicode.GetBytes(content)], purpose);
        // The same file in both Cheeky builds (upstream and focus adapter): either one already there is ours.
        string[] Variants(string relative) => new[] { components.CheekyDirectory, components.QuadFocusDirectory }
            .Where(d => d is not null && File.Exists(Path.Combine(d, relative))).Select(d => Hashing.FileSha256(Path.Combine(d!, relative))).ToArray();
        void AddLoader(string source)
        {
            // dxgi.dll is the only loader: the version.dll fallback is flagged by Microsoft Defender and is not distributed.
            // Another program's dxgi.dll (ReShade, an overlay) is backed up the first time and replaced; Restore
            // originals brings it back. Removed last on restore (reverse order), after Cheeky's own files.
            const string loader = "dxgi.dll";
            Add(PathPolicy.UnderRoot(bin, loader), File.ReadAllBytes(PathPolicy.UnderRoot(source, loader)), "Cheeky loader " + loader, ownHashes: Variants(loader),
                runtimeLogs: new[] { "CheekyFoveatedDLSS-Loader.log", "CheekyFoveatedDLSS/CheekyFoveatedDLSS-Host.log", "CheekyFoveatedDLSS/CheekyFoveatedDLSS-Standalone.log" }
                    .Select(log => PathPolicy.UnderRoot(bin, log)).ToArray());
            // The pilot's own dxgi.dll (ReShade, an overlay) keeps working: the loader chains to bin\dxgi2.dll, so it is
            // installed there; Back to stock DCS removes this copy and puts the original back as dxgi.dll.
            if (chainedDxgi is not null)
                Add(PathPolicy.UnderRoot(bin, "dxgi2.dll"), chainedDxgi, "Your own dxgi.dll (ReShade, an overlay), loaded after the DLSS 5 loader");
        }
        void Add(string path, byte[] content, string purpose, IReadOnlyList<LuaValueChange>? luaChanges = null, IReadOnlyDictionary<string, string>? iniValues = null, IReadOnlyList<string>? runtimeLogs = null, IReadOnlyCollection<string>? ownHashes = null)
        {
            // Every destination lives below a folder the user selected or the app owns; links below it are refused
            // now and again at apply/restore time, links above it (moved installs) are accepted.
            var managed = Path.GetFullPath(managedRoot);
            var root = new[] { bin, Path.GetDirectoryName(optionsPath)!, scripts, managed }
                .FirstOrDefault(r => Path.GetFullPath(path).StartsWith(r.TrimEnd(Path.DirectorySeparatorChar) + Path.DirectorySeparatorChar, StringComparison.OrdinalIgnoreCase));
            PathPolicy.RejectReparsePoints(path, root);
            // An existing file is never a conflict: Apply backs up its original the first time and overwrites it.
            files.Add(new(path, File.Exists(path) ? Hashing.FileSha256(path) : null, content, purpose, luaChanges, IniValues: iniValues, RuntimeLogs: runtimeLogs, Root: root,
                OwnHashes: ownHashes, OwnedLocation: root is not null && root.Equals(managed, StringComparison.OrdinalIgnoreCase)));
        }
    }

    internal static string RequireDirectory(string? path, string error) => path is not null && Directory.Exists(path) ? Path.GetFullPath(path) : throw new InvalidDataException(error);
    private static bool IsLayer(string path, string name)
    {
        try { using var document = JsonDocument.Parse(File.ReadAllText(path)); return document.RootElement.GetProperty("api_layer").GetProperty("name").GetString() == name; }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException or JsonException or KeyNotFoundException or InvalidOperationException or ArgumentException) { return false; }
    }
}

/// <summary>Locates an enabled implicit eye-gaze bridge (OpenXR-Eye-Trackers) for the SteamVR/Sboys route.</summary>
public static class GazeBridge
{
    public static bool Required(VrProfile profile) =>
        profile.Runtime == RuntimeKind.SboysSteamVr && profile.Gaze == GazeMode.EyeTracked && (profile.QuadViews != QuadProvider.None || profile.UsesCheeky);

    public static bool IsGazeLayer(LayerFact layer) =>
        layer.Name?.Contains("eye_trackers", StringComparison.OrdinalIgnoreCase) == true || layer.Name?.Contains("eyetrackers", StringComparison.OrdinalIgnoreCase) == true;

    /// <summary>
    /// Sboys 1.3 creates a SteamVR eye-tracking component, and SteamVR/OpenXR then reports
    /// XR_EXT_eye_gaze_interaction support (verified 2026-10-03 on a Crystal Super: supportsEyeGazeInteraction = 1).
    /// Quad Views reads gaze through that extension, so no separate bridge is needed. Major and minor only: "1.3" has no
    /// build number and would sort below 1.3.0.
    /// </summary>
    public static bool RuntimeProvidesGaze(InventorySnapshot inventory) =>
        inventory.Drivers.Any(d => d.Name.Contains("customheadset", StringComparison.OrdinalIgnoreCase) && !d.Blocked
            && Version.TryParse((d.Version ?? "").Split('-')[0], out var version) && (version.Major, version.Minor) is ( > 1, _) or (1, >= 3) && !(d.Version ?? "").Contains('-'));

    public static LayerFact? Find(InventorySnapshot inventory) =>
        inventory.Layers.FirstOrDefault(l => l.Enabled && l.LibraryExists && l.Name is not null && IsGazeLayer(l));

    public static string? DisableVariable(string manifest)
    {
        try
        {
            using var document = JsonDocument.Parse(manifest);
            if (!document.RootElement.GetProperty("api_layer").TryGetProperty("disable_environment", out var value)) return null;
            var name = value.GetString();
            return !string.IsNullOrWhiteSpace(name) && name.All(c => char.IsAsciiLetterOrDigit(c) || c == '_') ? name : null;
        }
        catch (Exception e) when (e is JsonException or KeyNotFoundException or InvalidOperationException) { return null; }
    }

    /// <summary>Implicit layers the profile controls. Any other enabled implicit layer loads above the profile layers.</summary>
    public static bool IsManaged(LayerFact layer, VrProfile profile) =>
        layer.Name is "XR_APILAYER_MBUCCHIA_quad_views_foveated" or "XR_APILAYER_CHEEKY_foveated_dlss" or "XR_APILAYER_XRFrameBridge_diagnostic"
        || (Required(profile) && IsGazeLayer(layer));
}

/// <summary>VectorXR (DienerTech), an implicit OpenXR layer with its own frame generation, Quadviews and Depth: turned
/// off for DCS started with a profile that loads DCS Control's own layers.</summary>
public static class VectorXr
{
    public const string LayerName = "XR_APILAYER_DIENERTECH_VECTORXR";
    public const string DisableVariable = "DISABLE_XR_APILAYER_DIENERTECH_VECTORXR";
    public static bool Is(LayerFact layer) => string.Equals(layer.Name, LayerName, StringComparison.OrdinalIgnoreCase);
}
