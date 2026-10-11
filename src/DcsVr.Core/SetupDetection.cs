using System.Diagnostics;
using System.Globalization;
using System.Text.Json;

namespace DcsVr.Core;

/// <summary>One value the detector proposes, with where it came from so the user can judge it before accepting.</summary>
public sealed record DetectedSetting(string Field, string Label, string Value, string Source);
public sealed record ActiveRouteState(RuntimeKind? Route, string Summary, TrackingKind Tracking);
public sealed record SetupDetectionResult(VrProfile Profile, IReadOnlyList<DetectedSetting> Detected, IReadOnlyList<string> Notes, ActiveRouteState Active);

/// <summary>Where detection reads from. Tests point these at fixtures; nothing here is ever written.</summary>
public sealed record DetectionSources
{
    public string SteamVrServerLog { get; init; } = Path.Combine(OpenVrPath("log"), "vrserver.txt");
    public string SteamVrSettings { get; init; } = Path.Combine(OpenVrPath("config"), "steamvr.vrsettings");

    /// <summary>SteamVR's log and config folders as registered in openvrpaths.vrpath; %LOCALAPPDATA%\openvr otherwise.</summary>
    private static string OpenVrPath(string kind)
    {
        var fallback = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "openvr");
        try
        {
            using var document = JsonDocument.Parse(File.ReadAllText(Path.Combine(fallback, "openvrpaths.vrpath")));
            return document.RootElement.TryGetProperty(kind, out var list) && list.ValueKind == JsonValueKind.Array && list.GetArrayLength() > 0
                && list[0].GetString() is { Length: > 0 } path && Directory.Exists(path) ? path : fallback;
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException or JsonException or InvalidOperationException) { return fallback; }
    }
    public string PimaxPlaySettings { get; init; } = PimaxFovea.SettingsPath;
    public IReadOnlyList<string> NeuralRuntimeFolders { get; init; } = [
        Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.UserProfile), "Downloads"),
        Environment.GetFolderPath(Environment.SpecialFolder.MyDocuments),
        Environment.GetFolderPath(Environment.SpecialFolder.DesktopDirectory)];
    /// <summary>The app's saved DLSS 5 runtime copy: used when present, and a signed runtime found on this PC is saved
    /// there. Null leaves detection read-only (the runtime path is proposed instead).</summary>
    public NeuralRuntimeStore? SavedRuntimes { get; init; }
    public Func<string, bool> ProcessRunning { get; init; } = name =>
    {
        var processes = Process.GetProcessesByName(name);
        try { return processes.Length > 0; } finally { foreach (var process in processes) process.Dispose(); }
    };
}

/// <summary>
/// Proposes a profile from what is running and configured on this PC: the headset route (Sboys/SteamVR or Pimax),
/// tracking, eye tracking, refresh rate, Pimax Play Quad View values and a valid DLSS-NR runtime.
/// </summary>
public static class SetupDetection
{
    public static ActiveRouteState DetectActiveRoute(DetectionSources? sources = null)
    {
        sources ??= new();
        // Only SteamVR's own log is positive evidence. Pimax Play (pi_server) also runs underneath Sboys, so it
        // never proves the Pimax route on its own.
        if (sources.ProcessRunning("vrserver"))
        {
            if (LastActiveHmd(sources.SteamVrServerLog) is not { } hmd)
                return new(null, "SteamVR is running, but its log does not name the active headset driver.", TrackingKind.Unknown);
            var tracking = hmd.Contains("Slam", StringComparison.OrdinalIgnoreCase) ? TrackingKind.Slam : TrackingKind.Unknown;
            if (hmd.StartsWith("CustomHeadsetOpenVR.", StringComparison.OrdinalIgnoreCase))
                return new(RuntimeKind.SboysSteamVr, "SteamVR is running the headset through Sboys (" + hmd + ").", tracking);
            if (hmd.StartsWith("aapvr.", StringComparison.OrdinalIgnoreCase) || hmd.StartsWith("pimax", StringComparison.OrdinalIgnoreCase))
                return new(null, "SteamVR is running the headset through Pimax's SteamVR driver (" + hmd + ").", tracking);
            return new(null, "SteamVR is running a headset through " + hmd.Split('.')[0] + ".", tracking);
        }
        if (sources.ProcessRunning("pi_server"))
            return new(null, "Pimax Play is running and SteamVR is not. Use the Pimax route, or start SteamVR first for Sboys.", TrackingKind.Unknown);
        return new(null, "No running headset software was detected. Start Pimax Play, or SteamVR with Sboys, then detect again.", TrackingKind.Unknown);
    }

    public static SetupDetectionResult Detect(VrProfile current, InventorySnapshot inventory, DetectionSources? sources = null)
    {
        sources ??= new();
        var detected = new List<DetectedSetting>();
        var notes = new List<string>();
        var profile = current;
        void Set(string field, string label, string value, string source) => detected.Add(new(field, label, value, source));

        var active = DetectActiveRoute(sources);
        if (active.Route is { } route)
        {
            // A running Pimax or SteamVR route is what this PC flies with: it replaces Optimizations only, whose VR settings
            // were kept in the profile and come back with it.
            profile = profile with { Desktop = false, Runtime = route, RuntimeManifestPath = route == profile.Runtime ? profile.RuntimeManifestPath : null };
            Set("runtime", "How you fly", (route == RuntimeKind.SboysSteamVr ? "Sboys · SteamVR OpenXR" : "Pimax OpenXR") + (current.Desktop ? " (instead of Optimizations only)" : ""), active.Summary);
        }
        else
        {
            notes.Add(active.Summary);
            if (current.Desktop) notes.Add("Optimizations only stays selected: no Pimax or SteamVR runtime is running.");
        }

        var tracking = active.Tracking != TrackingKind.Unknown ? active.Tracking : inventory.Tracking;
        if (tracking != TrackingKind.Unknown)
        {
            profile = profile with { Tracking = tracking };
            Set("tracking", "Headset tracking", tracking == TrackingKind.Slam ? "SLAM (Pimax EVO)" : "Lighthouse",
                active.Tracking != TrackingKind.Unknown ? "Sboys headset serial in the SteamVR log" : "Detected installation");
        }

        if (profile.QuadViews != QuadProvider.None)
        {
            if (profile.Runtime == RuntimeKind.SboysSteamVr && GazeBridge.RuntimeProvidesGaze(inventory))
            {
                profile = profile with { Gaze = GazeMode.EyeTracked };
                Set("gaze", "Focus movement", "Eye tracked", "Sboys 1.3 or newer exposes eye gaze through SteamVR OpenXR");
            }
            // Pimax's own eye-tracking services (Crystal family). Tobii.EyeX.Engine is the desktop Tobii Eye Tracker's
            // and platform_runtime_IS5LEYET is the Tobii Eye Tracker 5: neither says anything about the headset.
            else if (profile.Runtime == RuntimeKind.Pimax && (sources.ProcessRunning("platform_runtime_XR5EYECH") || sources.ProcessRunning("platform_runtime_VR4PIMAX")))
            {
                profile = profile with { Gaze = GazeMode.EyeTracked };
                Set("gaze", "Focus movement", "Eye tracked", "Pimax eye-tracking service is running");
            }
            // Pimax Play's service runs but no Pimax eye-tracking service does: a headset without eye tracking (Crystal Light, 8KX…).
            else if (profile.Runtime == RuntimeKind.Pimax && (sources.ProcessRunning("PiService") || sources.ProcessRunning("PiServiceLauncher")))
            {
                profile = profile with { Gaze = GazeMode.Fixed };
                Set("gaze", "Focus movement", "Fixed", "Pimax Play runs without an eye-tracking service (a headset without eye tracking)");
            }
            else if (profile.Runtime == RuntimeKind.SboysSteamVr && GazeBridge.Find(inventory) is null)
            {
                profile = profile with { Gaze = GazeMode.Fixed };
                Set("gaze", "Focus movement", "Fixed", "No eye-gaze source for this Sboys version");
            }
        }

        if (profile.Runtime == RuntimeKind.SboysSteamVr && ReadSteamVrRefresh(sources.SteamVrSettings) is { } refresh)
        {
            profile = profile with { HeadsetRefreshHz = refresh };
            Set("headsetRefreshHz", "Headset refresh rate", refresh.ToString("0.#", CultureInfo.InvariantCulture) + " Hz", "SteamVR preferred refresh rate (confirm it matches the headset)");
        }

        if (profile.QuadViews == QuadProvider.QuadViewsFoveated && string.IsNullOrWhiteSpace(profile.QuadViewsLayerDirectory) && PimaxFovea.Read(sources.PimaxPlaySettings) is { } quad)
        {
            // The focus area follows Pimax Play (read again at every launch); the converted values are also stored so
            // the profile keeps them if the source is later switched to "This profile".
            var c = quad.Converted;
            profile = profile with
            {
                FoveaSource = FoveaSource.PimaxPlay, QuadFocusScale = c.FocusScale, PeripheralScale = c.PeripheralScale,
                FoveaWidth = c.Width, FoveaHeight = c.Height, QuadEdgeBlend = c.Transition ? profile.QuadEdgeBlend : 0
            };
            // Shown in Pimax Play's own units and labels; the converted values are in the Quad Views page's details.
            var source = "Pimax Play " + quad.Play.Mode + " mode (" + sources.PimaxPlaySettings + ")";
            Set("foveaSource", "Focus area source", "Pimax Play (read again at every launch)", sources.PimaxPlaySettings);
            foreach (var control in quad.Play.Controls)
                Set("pimax:" + control.Label, control.Label, control.Value, source);
            Set("quadEdgeBlend", "Edge blending", Format(profile.QuadEdgeBlend), c.Transition ? "Pimax Transition Mode is " + quad.Play.TransitionMode + "; this profile's blending is kept" : "Pimax Transition Mode is Off");
            notes.AddRange(PimaxFovea.Notes(quad));
            if (profile.Gaze == GazeMode.Fixed && (profile.FoveaWidth < .45 || profile.FoveaHeight < .4))
                notes.Add("Pimax's focus region assumes eye tracking. With a fixed focus, about Horizontal FOV 45% and Vertical FOV 55% (Pimax Quick units, a focus of 0.55 × 0.45 of the view) keeps instruments sharp.");
        }

        if (ControlService.UsesSavedRuntime(profile))
        {
            // The app's saved copy is used by a profile without a runtime file of its own; a found file is saved as that copy.
            var saved = sources.SavedRuntimes?.Current();
            if (saved is null && FindNeuralRuntime(sources.NeuralRuntimeFolders) is { } neural)
            {
                if (sources.SavedRuntimes is { } store)
                {
                    try { saved = store.Save(neural); }
                    catch (Exception e) when (e is IOException or UnauthorizedAccessException or InvalidDataException) { notes.Add("The DLSS 5 runtime found at " + neural + " could not be saved: " + e.Message); }
                }
                if (saved is null)
                {
                    profile = profile with { NeuralRuntimePath = neural };
                    Set("neuralRuntimePath", "DLSS 5 runtime", neural, "Signed NVIDIA nvngx_dlssnr.dll 310.8 found on this PC");
                }
                else
                {
                    profile = profile with { NeuralRuntimePath = null };
                    Set("neuralRuntimePath", "DLSS 5 runtime", "Saved copy · version " + saved.Version, "Signed NVIDIA nvngx_dlssnr.dll found at " + neural + "; a verified copy is kept at " + saved.Path);
                }
            }
            else if (saved is not null)
            {
                if (!string.IsNullOrWhiteSpace(profile.NeuralRuntimePath))
                {
                    profile = profile with { NeuralRuntimePath = null };
                    Set("neuralRuntimePath", "DLSS 5 runtime", "Saved copy · version " + saved.Version, "The profile's runtime file no longer exists; the saved copy from " + saved.OriginalPath + " is used");
                }
                else notes.Add("DLSS 5 uses the saved copy of nvngx_dlssnr.dll (version " + saved.Version + ", from " + saved.OriginalPath + ").");
            }
            else notes.Add("No signed NVIDIA nvngx_dlssnr.dll 310.8 was found in Downloads, Documents or Desktop. Select it on the DLSS 5 page; the app keeps a verified copy.");
        }

        return new(profile, detected, notes, active);
    }

    private static string Format(double value) => value.ToString("0.##", CultureInfo.InvariantCulture);

    /// <summary>Reads the newest "Active HMD set to driver.serial" line from the end of the SteamVR server log.</summary>
    public static string? LastActiveHmd(string log)
    {
        try
        {
            if (!File.Exists(log)) return null;
            using var stream = new FileStream(log, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete);
            // The line is written once per SteamVR start, so a long session can push it out of a tail window:
            // read the tail first, then the whole log.
            const string marker = "Active HMD set to ";
            foreach (var window in new long[] { 512 * 1024, long.MaxValue })
            {
                stream.Seek(Math.Max(0, stream.Length - Math.Min(window, stream.Length)), SeekOrigin.Begin);
                using var reader = new StreamReader(stream, leaveOpen: true);
                string? last = null;
                for (var line = reader.ReadLine(); line is not null; line = reader.ReadLine())
                    if (line.IndexOf(marker, StringComparison.Ordinal) is var at and >= 0) last = line[(at + marker.Length)..].Trim();
                if (last is not null || window >= stream.Length) return last;
            }
            return null;
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException) { return null; }
    }

    internal static double? ReadSteamVrRefresh(string settings)
    {
        try
        {
            using var document = JsonDocument.Parse(File.ReadAllText(settings));
            return document.RootElement.TryGetProperty("steamvr", out var steamvr) && steamvr.TryGetProperty("preferredRefreshRate", out var rate)
                && rate.TryGetDouble(out var hz) && hz is >= 50 and <= 240 ? hz : null;
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException or JsonException or InvalidOperationException) { return null; }
    }

    private static string? FindNeuralRuntime(IReadOnlyList<string> folders)
    {
        foreach (var folder in folders.Where(Directory.Exists))
        {
            IEnumerable<string> candidates;
            try
            {
                candidates = Directory.EnumerateFiles(folder, "nvngx_dlssnr.dll", new EnumerationOptions { RecurseSubdirectories = true, MaxRecursionDepth = 3, IgnoreInaccessible = true })
                    .Take(200).OrderByDescending(File.GetLastWriteTimeUtc).Take(5).ToArray();
            }
            catch (Exception e) when (e is IOException or UnauthorizedAccessException) { continue; }
            foreach (var candidate in candidates)
            {
                try
                {
                    // Version and architecture first (cheap); the signature check only for a plausible candidate.
                    if (!NativeBinary.SupportsNeuralContract(System.Diagnostics.FileVersionInfo.GetVersionInfo(candidate).FileVersion) || !NativeBinary.IsX64(candidate)) continue;
                    var inspection = NativeBinary.Inspect(candidate);
                    if (inspection.IsX64 && inspection.TrustedSignature && NativeBinary.IsNvidiaSigner(inspection.Signer) && NativeBinary.SupportsNeuralContract(inspection.Version))
                        return candidate;
                }
                catch (Exception e) when (e is IOException or UnauthorizedAccessException) { }
            }
        }
        return null;
    }
}
