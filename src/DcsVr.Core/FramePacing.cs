using System.Globalization;

namespace DcsVr.Core;

public sealed record CadencePlan(double RefreshHz, double RequiredRenderedFps, double? DcsCap, double? ConfiguredOutputCeiling,
    string CapDescription, bool FrameGeneration, string Note);

public static class FramePacing
{
    // DCS's installed optionsDb.lua exposes graphics.maxFPS as 30..300.
    // OFXR (the djules75 fork, 0.2.9.1) paces at run time but has no FPS limiter INI key.
    public static double? RequestedCap(VrProfile profile) => profile.FpsLimit switch
    {
        FpsLimitMode.Preserve => null,
        FpsLimitMode.RuntimeHeadroom => 300,
        // Never below DCS's own minimum (30): 3× at 72 or 75 Hz would ask for 24-25 FPS.
        FpsLimitMode.MatchRefresh => Math.Max(30, profile.HeadsetRefreshHz / Multiplier(profile)),
        FpsLimitMode.Custom => profile.RenderedFpsCap,
        _ => throw new InvalidDataException("Unknown FPS limit mode.")
    };

    public static CadencePlan Describe(VrProfile profile, InventorySnapshot? inventory = null)
    {
        var fg = profile.FrameGen != FrameGeneration.Off;
        var cap = RequestedCap(profile);
        if (profile.FpsLimit == FpsLimitMode.Preserve && inventory?.DcsSettings.TryGetValue("graphics.maxFPS", out var raw) == true)
            cap = PositiveNumber(raw);
        var label = profile.FpsLimit switch
        {
            FpsLimitMode.Preserve => cap is null ? "Preserve existing DCS limit (not detected)" : "Preserve detected DCS limit",
            FpsLimitMode.RuntimeHeadroom => "300 FPS headroom; OpenXR still paces frames",
            FpsLimitMode.MatchRefresh => fg ? (profile.FrameGenFactor == VrProfile.FrameGenAuto ? "Half refresh; OFXR switches to two generated frames when DCS falls below it" : Multiplier(profile) == 3 ? "A third of the refresh for OFXR's two generated frames" : "Half refresh for OFXR's generated frame (2×)") : "Full refresh; frame generation is off",
            _ => "Custom rendered-frame cap"
        };
        return new(profile.HeadsetRefreshHz, profile.HeadsetRefreshHz / Multiplier(profile), cap,
            cap is null ? null : Math.Min(profile.HeadsetRefreshHz, cap.Value * Multiplier(profile)), label, fg,
            "Calculated cap ceiling, not measured FPS. GPU/CPU load, skipped synthesis, other limiters and runtime presentation can lower it." + (HeadsetRefresh.ReadFor(profile) is { } read ? " Refresh rate read from " + read.Source + "." : " Refresh rate is the value set here, not read from the headset."));
    }

    /// <summary>Displayed frames per rendered frame for the profile's frame generation. Auto targets 2 (half the
    /// refresh rate); OFXR drops to 3 only while DCS cannot hold that.</summary>
    public static int Multiplier(VrProfile profile) => profile.FrameGen == FrameGeneration.Off ? 1 : profile.FrameGenFactor == 3 ? 3 : 2;

    public static double? PositiveNumber(string? raw) => double.TryParse(raw, NumberStyles.Float, CultureInfo.InvariantCulture, out var n) && double.IsFinite(n) && n > 0 ? n : null;

    public static IReadOnlyList<ReadinessCheck> Checks(VrProfile profile, InventorySnapshot inventory)
    {
        var plan = Describe(profile, inventory);
        var checks = new List<ReadinessCheck>
        {
            new("fps-cap", plan.DcsCap is null ? CheckState.Manual : CheckState.Pass, "DCS frame limit", plan.CapDescription + (plan.DcsCap is null ? "." : string.Create(CultureInfo.InvariantCulture, $": {plan.DcsCap:0.###} FPS.")) + " Changes are previewed, backed up and restored with the profile."),
            new("refresh-rate", HeadsetRefresh.ReadFor(profile) is null ? CheckState.Manual : CheckState.Pass, "Refresh rate", (HeadsetRefresh.ReadFor(profile) is { } read ? string.Create(CultureInfo.InvariantCulture, $"{read.Source}: {profile.HeadsetRefreshHz:0.###} Hz.") : string.Create(CultureInfo.InvariantCulture, $"Confirm {profile.HeadsetRefreshHz:0.###} Hz in Pimax Play or SteamVR.")) + string.Create(CultureInfo.InvariantCulture, $" Target: {plan.RequiredRenderedFps:0.###} rendered FPS") + (plan.FrameGeneration ? (profile.FrameGenFactor == VrProfile.FrameGenAuto ? " + generated frames: 1 (2×), or 2 (3×) when DCS falls behind." : Multiplier(profile) == 3 ? " + 2 generated frames per rendered frame (3×)." : " + 1 generated frame per rendered frame (2×).") : "; frame generation is off.") + " " + plan.Note)
        };
        if (plan.DcsCap is { } cap && cap + .01 < plan.RequiredRenderedFps)
            checks.Add(new("fps-under-target", CheckState.Warning, "Frame limit too low", string.Create(CultureInfo.InvariantCulture, $"{cap:0.###} rendered FPS cannot supply {profile.HeadsetRefreshHz:0.###} fresh frames/s with this pipeline. Its calculated ceiling is {plan.ConfiguredOutputCeiling:0.###} FPS before other bottlenecks.")));
        if (inventory.AutoexecPath is not null)
            checks.Add(new("autoexec-pacing", inventory.AutoexecMaxFps is not null || inventory.AutoexecPacingUnknown ? CheckState.Warning : CheckState.Pass, "autoexec.cfg frame limit", inventory.AutoexecMaxFps is { } value
                ? "Detected max_fps = " + value + ". This separate legacy cap may affect cadence. Review it before testing; the tool preserves autoexec.cfg."
                : inventory.AutoexecPacingUnknown ? "autoexec.cfg contains unsupported commands; its pacing effect is unknown. Review custom caps and commands before testing. The tool preserves this file."
                : "No max_fps assignment was found in the data-only autoexec.cfg. Review other custom pacing settings before testing. This file is preserved."));
        var sync = inventory.DcsSettings.GetValueOrDefault("graphics.sync");
        checks.Add(new("dcs-vsync", !profile.DisableDcsVSync && sync == "true" ? CheckState.Warning : CheckState.Pass, "DCS desktop VSync",
            profile.DisableDcsVSync ? "The profile will set graphics.sync = false. The original value is backed up. OpenXR runtime synchronization remains active."
            : "Preserved: " + (sync ?? "not detected") + ". Desktop VSync is separate from OpenXR synchronization; verify its effect in DCS."));
        // Other limiters and runtime motion smoothing cannot be read or verified here, so they are always checks the
        // user does (an old profile's self-reported values are dropped when it loads).
        checks.Add(new("external-limiters", CheckState.Manual, "NVIDIA / RTSS / other FPS limits",
            "Check NVIDIA Max Frame Rate (global and for DCS), Background Application Max Frame Rate and RTSS: a second limiter can fight with the DCS frame limit and frame generation. This app does not read or change them."
            + (inventory.LimiterProcesses.Count == 0 ? " No RTSS process was detected; that does not prove every limit is off." : " Running: " + string.Join(", ", inventory.LimiterProcesses) + "; a running process does not prove an active limit.")));
        if (plan.FrameGeneration)
            checks.Add(new("runtime-reprojection", CheckState.Manual, "Runtime motion smoothing / Smart Smoothing",
                "Turn off Pimax Smart Smoothing or SteamVR motion smoothing with frame generation: two stages that synthesize frames change latency and cadence. Rotational timewarp is separate. Set this in Pimax Play or SteamVR; this app does not change it."));
        return checks;
    }

    // Accept only plain data assignments; executable autoexec Lua is never run.
    public static (string? MaxFps, bool Unknown) ReadAutoexec(string text)
    {
        try { return (new LuaOptions("options={\n" + text + "\n}").Get("max_fps"), false); }
        catch (InvalidDataException) { return (null, true); }
    }
}
