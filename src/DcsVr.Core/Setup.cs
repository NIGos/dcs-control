using System.Text.Json;

namespace DcsVr.Core;

public enum CheckState { Pass, Error, Warning, Manual }
public sealed record ReadinessCheck(string Id, CheckState State, string Title, string Detail, string? Guide = null);
public sealed record ReadinessReport(DateTimeOffset CapturedAt, string ProfileId, IReadOnlyList<ReadinessCheck> Checks)
{
    public bool CanPrepare => Checks.All(c => c.State != CheckState.Error);
}

public static class Readiness
{
    /// <summary>Running headset route; replaceable so tests never depend on what runs on the build machine.</summary>
    public static Func<ActiveRouteState> ActiveRouteProvider { get; set; } = () => SetupDetection.DetectActiveRoute();

    /// <remarks>An applied profile is never a blocker: Launch DCS writes the new profile over it.</remarks>
    public static ReadinessReport Check(VrProfile profile, InventorySnapshot inventory, ControlService service)
    {
        // A draft without its own DLSS 5 runtime file is checked with the app's saved copy, exactly as Preview deploys it.
        // What will run: the VR settings Optimizations only keeps for later are not checked.
        profile = HeadsetRefresh.Resolve(service.ResolveNeuralRuntime(profile).ForLaunch());
        var checks = new List<ReadinessCheck>();
        void Add(string id, CheckState state, string title, string detail, string? guide = null) => checks.Add(new(id, state, title, detail, guide));
        foreach (var issue in ProfileValidation.Validate(profile, inventory))
            Add(issue.Code, issue.Severity switch { IssueSeverity.Error => CheckState.Error, IssueSeverity.Info => CheckState.Manual, _ => CheckState.Warning }, "Settings", issue.Message);
        if (Enum.IsDefined(profile.FpsLimit) && !profile.Desktop) checks.AddRange(FramePacing.Checks(profile, inventory));
        Add("game", File.Exists(inventory.DcsExecutable) && File.Exists(inventory.OptionsPath) ? CheckState.Pass : CheckState.Error,
            "DCS and Saved Games", "Select DCS.exe and the options.lua for the Saved Games profile you will use.", "paths");
        Add("closed", inventory.DcsRunning ? CheckState.Error : CheckState.Pass, "DCS is closed", "Close DCS before applying or restoring files.");
        try
        {
            if (inventory.DcsExecutable is { } exe && File.Exists(exe)) LaunchSafety.EnsureNotElevated(exe);
            else if (LaunchSafety.CurrentProcessElevated) throw new InvalidOperationException("DCS Control is running as administrator. Reopen it without administrator rights.");
            Add("elevation", CheckState.Pass, "Runs without admin rights", "DCS starts without administrator rights, so it loads the headset software and add-ons this profile sets up.");
        }
        catch (InvalidOperationException e) { Add("elevation", CheckState.Error, "Runs without admin rights", e.Message); }
        // DCS's bin folder receives only the Cheeky loader and its files (DLSS 5, Foveated SR); everything else goes to Saved Games.
        LaunchSafety.ForgetWriteProbes();
        var writable = LaunchSafety.UnwritableFolders(profile, inventory);
        // Folders Launch DCS can get access to itself (one Windows prompt) are a warning, as in ProfileValidation.
        var grantable = writable.All(f => LaunchSafety.PermissionsDenyWrite(f) && LaunchSafety.CanGrant(f));
        Add("write-access", writable.Count == 0 ? CheckState.Pass : grantable ? CheckState.Warning : CheckState.Error, "Write access to game folders",
            writable.Count == 0 ? "DCS Control can write every file it needs." : "Not writable for your Windows user: " + string.Join(" ", writable.Select(f => f + ": " + LaunchSafety.WriteAdvice(f))));
        if (service.ForeignDxgi(profile, inventory) is { } dxgi)
            Add("foreign-dxgi", CheckState.Warning, "Another program's dxgi.dll", profile.KeepOtherDxgi
                ? $"DCS's bin folder has a dxgi.dll from {dxgi.Name} ({dxgi.Path}). DLSS 5 needs that file name, so Launch DCS backs it up and loads it together with DLSS 5: {dxgi.Name} keeps working. If DCS misbehaves with both, turn off Keep another program's dxgi.dll working on the DLSS 5 page."
                : $"DCS's bin folder has a dxgi.dll from {dxgi.Name} ({dxgi.Path}). DLSS 5 needs that file name, so Launch DCS backs it up and sets it aside while this profile is applied: {dxgi.Name} is off in DCS until Back to stock DCS puts it back.");
        var running = profile.Desktop ? new ActiveRouteState(null, "", default) : ActiveRouteProvider();
        if (running.Route is { } activeRoute && activeRoute != profile.Runtime)
            Add("route-mismatch", CheckState.Error, "Headset route", running.Summary + " This profile uses " + (profile.Runtime == RuntimeKind.Pimax ? "the Pimax runtime" : "Sboys through SteamVR") + ". Change How you fly in the right panel, or close the other headset software, before launching.");
        else if (running.Route is not null) Add("route-mismatch", CheckState.Pass, "Headset route", running.Summary);
        if (inventory.DcsExecutable?.Contains(@"\steamapps\", StringComparison.OrdinalIgnoreCase) == true)
            Add("steam-relaunch", CheckState.Manual, "Steam edition launch", "Keep Steam running and start DCS with Launch DCS in this app. DCS then starts with this profile instead of restarting through Steam without it.");
        if (profile.KeepDcsLauncher && LaunchSafety.LauncherRestartBlocked)
            Add("dcs-launcher", CheckState.Error, "DCS launcher", LaunchSafety.LauncherBlockedMessage);
        else if (profile.KeepDcsLauncher && LaunchSafety.LauncherRestartUnknown)
            Add("dcs-launcher", CheckState.Warning, "DCS launcher", LaunchSafety.LauncherUnknownMessage);
        var foreign = inventory.Layers.Where(l => l.Enabled && !GazeBridge.IsManaged(l, profile)).Select(l => l.Name ?? Path.GetFileName(l.ManifestPath)).ToArray();
        if (foreign.Length > 0 && !profile.Desktop)
            Add("implicit-layers", CheckState.Warning, "Other VR add-ons (OpenXR layers)", "Other OpenXR add-ons are installed: " + string.Join(", ", foreign) + ". They load before this profile's add-ons and can change the image, eye tracking or frame timing. If something looks wrong, turn them off (for example with OpenXR API Layers GUI).");
        var manifest = profile.RuntimeManifestPath ?? (profile.Runtime == RuntimeKind.Pimax ? inventory.PimaxRuntime : inventory.SteamVrRuntime);
        if (!profile.Desktop) try
        {
            using var json = JsonDocument.Parse(File.ReadAllText(manifest ?? throw new InvalidDataException("Runtime manifest was not detected.")));
            var relative = json.RootElement.GetProperty("runtime").GetProperty("library_path").GetString();
            var dll = Path.GetFullPath(relative ?? throw new InvalidDataException("Runtime library path is empty."), Path.GetDirectoryName(manifest)!);
            if (!NativeBinary.IsX64(dll)) throw new InvalidDataException("The selected runtime library is missing or is not x64: " + dll);
            Add("runtime", CheckState.Pass, "Selected OpenXR runtime", manifest! + " → " + dll);
        }
        catch (Exception e) when (e is IOException or InvalidDataException or UnauthorizedAccessException or JsonException or KeyNotFoundException or ArgumentException or InvalidOperationException)
        { Add("runtime", CheckState.Error, "Selected OpenXR runtime", e.Message, profile.Runtime == RuntimeKind.Pimax ? "pimax" : "steamvr"); }
        var missingVc = new[] { "vcruntime140.dll", "vcruntime140_1.dll", "msvcp140.dll" }.Where(n => !NativeBinary.IsX64(Path.Combine(Environment.SystemDirectory, n))).ToArray();
        var vcFile = Path.Combine(Environment.SystemDirectory, "vcruntime140.dll");
        var vcVersion = File.Exists(vcFile) ? System.Diagnostics.FileVersionInfo.GetVersionInfo(vcFile).FileVersion : null;
        var currentVc = Version.TryParse(vcVersion, out var parsedVc) && parsedVc >= new Version(14, 50);
        Add("vc-runtime", missingVc.Length == 0 && currentVc ? CheckState.Pass : CheckState.Error, "Microsoft C++ x64 runtime",
            missingVc.Length == 0 && currentVc ? "Required x64 C++ runtime libraries are present. Version: " + vcVersion : "Install Microsoft C++ x64 runtime 14.50 or newer. Missing libraries: " + string.Join(", ", missingVc), "vcredist");
        if (profile.FrameGen == FrameGeneration.Nvidia)
            Add("optical-flow-driver", NativeBinary.IsX64(Path.Combine(Environment.SystemDirectory, "nvofapi64.dll")) ? CheckState.Pass : CheckState.Error,
                "NVIDIA optical-flow driver", "NVIDIA Optical Flow frame generation needs NVIDIA's optical-flow driver, part of the NVIDIA graphics driver for RTX cards. Update the NVIDIA driver, or choose FidelityFX on the Framegen page: it runs on any graphics card.", "nvidia");
        if (profile.Runtime == RuntimeKind.SboysSteamVr && !profile.Desktop)
        {
            var driver = inventory.Drivers.FirstOrDefault(d => d.Name.Contains("customheadset", StringComparison.OrdinalIgnoreCase));
            Add("sboys-registration", driver is { LibraryExists: true, Blocked: false } ? CheckState.Pass : CheckState.Error,
                "Sboys SteamVR driver", driver is null ? "Prepare Sboys, open its tool, and install the driver from About. Refresh checks afterward." : driver.Blocked ? "SteamVR has disabled or blocked this driver. Enable it in SteamVR Manage Add-ons." : driver.LibraryExists ? driver.Directory : "The registered driver DLL is missing or is not x64.", "sboys");
            if (driver is not null)
                Add("sboys-version", GazeBridge.RuntimeProvidesGaze(inventory) ? CheckState.Pass : driver.Version is null ? CheckState.Warning : CheckState.Error,
                    "Sboys driver version", "Detected " + (driver.Version ?? "unversioned driver") + ". The managed official package is 1.3.0. Install/reinstall it from the prepared Sboys tool's About page before this test.", "sboys");
            if (profile.Gaze == GazeMode.EyeTracked && (profile.QuadViews != QuadProvider.None || profile.UsesCheeky))
            {
                var gaze = GazeBridge.Find(inventory);
                string? orderable = null;
                try { orderable = gaze is null ? null : GazeBridge.DisableVariable(File.ReadAllText(gaze.ManifestPath)); }
                catch (Exception e) when (e is IOException or UnauthorizedAccessException) { }
                if (gaze is null && GazeBridge.RuntimeProvidesGaze(inventory))
                    Add("gaze-bridge", CheckState.Manual, "SteamVR OpenXR eye gaze",
                        "Sboys " + driver?.Version + " provides eye tracking to SteamVR, which exposes XR_EXT_eye_gaze_interaction to Quad Views. No separate bridge is needed. Calibrate eye tracking and verify in the headset that the focus region follows your gaze.", "gaze");
                else
                Add("gaze-bridge", gaze is not null && orderable is not null && NativeBinary.IsX64(gaze.LibraryPath!) ? CheckState.Pass : CheckState.Error, "SteamVR OpenXR gaze bridge",
                    gaze is not null && orderable is null ? "The bridge manifest has no disable_environment, so it cannot be loaded below Quad Views. Update OpenXR-Eye-Trackers or choose Fixed focus." :
                    gaze is null ? "Dynamic OpenXR gaze needs a bridge such as OpenXR-Eye-Trackers. Its archived Crystal guide does not establish Crystal Super compatibility. Validate its gaze test app; choose Fixed focus explicitly if unavailable." : "Enabled bridge detected. The profile loads it explicitly below Quad Views. Valid eye data and Crystal Super support still need a headset test.", "gaze");
            }
        }
        if (profile.Desktop)
            Add("desktop", CheckState.Manual, "Optimizations only", "DCS runs as you set it up, on the monitor or with another headset, with your own VR settings and headset software. DCS Control adds only Engine Optimizations, CPU Boost and the flight helpers you turned on.");
        else
        Add("headset", CheckState.Manual, "Headset and tracking", "Connect your headset. Before launching DCS, check the image, IPD, tracking and resolution in Pimax Play (or SteamVR).", profile.Runtime == RuntimeKind.Pimax ? "pimax" : "sboys");
        if (profile.Gaze == GazeMode.EyeTracked && profile.QuadViews != QuadProvider.None)
            Add("gaze-live", CheckState.Manual, "Eye calibration and focus motion", "Turn on and calibrate eye tracking in Pimax Play. Then check in the headset that the focus area follows your eyes.", "gaze");
        if (!profile.Desktop)
        Add("pipeline-live", CheckState.Manual, "Image quality in the headset", "In the first flight, check both eyes, cockpit text, the HUD, head motion, latency and the edge of the focus area. If something looks wrong, turn the features on one at a time to find which one causes it.");
        try
        {
            if (service.Originals.Read() is { State: "applied", Current: { } current })
                Add("active-backup", CheckState.Pass, "Applied settings", (service.ReadAppliedProfile()?.Profile?.Name ?? current.ProfileId) + " is applied. Launch DCS writes the new settings over it; the original files stay backed up.");
            var plan = service.Preview(profile, inventory);
            Add("deployment", CheckState.Pass, "Files and components", $"All components are verified and fit together; Launch DCS will write {plan.Files.Count} files. Nothing has been written yet.");
        }
        catch (Exception e) when (e is IOException or InvalidDataException or UnauthorizedAccessException or InvalidOperationException or ArgumentException or JsonException)
        { Add("deployment", CheckState.Error, "Files and components", e.Message); }
        return new(DateTimeOffset.UtcNow, profile.Id, checks);
    }
}
