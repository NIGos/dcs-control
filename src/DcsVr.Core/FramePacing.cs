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
            FpsLimitMode.Preserve => cap is null ? "Keeps DCS's own limit (none set)" : "Keeps DCS's own limit",
            FpsLimitMode.RuntimeHeadroom => "Very high (300 FPS); the headset still sets the pace",
            FpsLimitMode.MatchRefresh => fg ? (profile.FrameGenFactor == VrProfile.FrameGenAuto ? "Half the refresh rate; a second generated frame while DCS falls behind" : Multiplier(profile) == 3 ? "A third of the refresh rate, for two generated frames" : "Half the refresh rate, for one generated frame") : "The full refresh rate; frame generation is off",
            _ => "Custom limit"
        };
        return new(profile.HeadsetRefreshHz, profile.HeadsetRefreshHz / Multiplier(profile), cap,
            cap is null ? null : Math.Min(profile.HeadsetRefreshHz, cap.Value * Multiplier(profile)), label, fg,
            "Calculated, not measured: CPU or GPU load and other frame limits can lower it." + (HeadsetRefresh.ReadFor(profile) is { } read ? " Refresh rate read from " + read.Source + "." : " The refresh rate is the one set in DCS Control, not read from the headset."));
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
            new("fps-cap", plan.DcsCap is null ? CheckState.Manual : CheckState.Pass, "DCS frame limit", plan.CapDescription + (plan.DcsCap is null ? "." : string.Create(CultureInfo.InvariantCulture, $": {plan.DcsCap:0.###} FPS.")) + " Your original value is backed up and comes back with Back to stock DCS."),
            new("refresh-rate", HeadsetRefresh.ReadFor(profile) is null ? CheckState.Manual : CheckState.Pass, "Refresh rate", (HeadsetRefresh.ReadFor(profile) is { } read ? string.Create(CultureInfo.InvariantCulture, $"{read.Source}: {profile.HeadsetRefreshHz:0.###} Hz.") : string.Create(CultureInfo.InvariantCulture, $"Make sure the headset runs at {profile.HeadsetRefreshHz:0.###} Hz in Pimax Play or SteamVR.")) + string.Create(CultureInfo.InvariantCulture, $" DCS needs to render {plan.RequiredRenderedFps:0.###} FPS") + (plan.FrameGeneration ? (profile.FrameGenFactor == VrProfile.FrameGenAuto ? ", plus one generated frame per rendered frame (two while DCS falls behind)." : Multiplier(profile) == 3 ? ", plus two generated frames per rendered frame (3×)." : ", plus one generated frame per rendered frame (2×).") : "; frame generation is off.") + " " + plan.Note)
        };
        if (plan.DcsCap is { } cap && cap + .01 < plan.RequiredRenderedFps)
            checks.Add(new("fps-under-target", CheckState.Warning, "Frame limit too low", string.Create(CultureInfo.InvariantCulture, $"A limit of {cap:0.###} FPS is below what DCS needs to render for {profile.HeadsetRefreshHz:0.###} Hz: at most {plan.ConfiguredOutputCeiling:0.###} frames per second reach the headset. Raise the limit or lower the refresh rate.")));
        if (inventory.AutoexecPath is not null)
            checks.Add(new("autoexec-pacing", inventory.AutoexecMaxFps is not null || inventory.AutoexecPacingUnknown ? CheckState.Warning : CheckState.Pass, "autoexec.cfg frame limit", inventory.AutoexecMaxFps is { } value
                ? "DCS's autoexec.cfg sets max_fps = " + value + ", a second frame limit that can hold DCS back. DCS Control leaves this file as it is: check that line yourself."
                : inventory.AutoexecPacingUnknown ? "DCS's autoexec.cfg has commands DCS Control cannot read, so their effect on the frame rate is unknown. DCS Control leaves this file as it is: check it yourself."
                : "DCS's autoexec.cfg sets no frame limit (max_fps). DCS Control leaves this file as it is."));
        var sync = inventory.DcsSettings.GetValueOrDefault("graphics.sync");
        checks.Add(new("dcs-vsync", !profile.DisableDcsVSync && sync == "true" ? CheckState.Warning : CheckState.Pass, "DCS desktop VSync",
            profile.DisableDcsVSync ? "Desktop VSync is turned off at launch; your original value is backed up. The headset keeps its own sync."
            : "Left as it is: " + (sync == "true" ? "on" : sync == "false" ? "off" : "not found") + ". VSync of the DCS window on the monitor can hold back the frame rate; Desktop VSync off on the Framegen page turns it off."));
        // Other limiters and runtime motion smoothing cannot be read or verified here, so they are always checks the
        // user does (an old profile's self-reported values are dropped when it loads).
        checks.Add(new("external-limiters", CheckState.Manual, "NVIDIA / RTSS / other FPS limits",
            "Check Max Frame Rate in the NVIDIA settings (global and for DCS), Background Application Max Frame Rate and RTSS: a second frame limit can fight with the DCS frame limit and frame generation. DCS Control does not read or change them."
            + (inventory.LimiterProcesses.Count == 0 ? " RTSS is not running, but other limits may still be on." : " Running: " + string.Join(", ", inventory.LimiterProcesses) + " (it may or may not limit the frame rate).")));
        if (plan.FrameGeneration)
            checks.Add(new("runtime-reprojection", CheckState.Manual, "Runtime motion smoothing / Smart Smoothing",
                "With frame generation, turn off Smart Smoothing in Pimax Play (or Motion Smoothing in SteamVR): two kinds of generated frames on top of each other add latency and stutter. DCS Control does not change this setting."));
        return checks;
    }

    // Accept only plain data assignments; executable autoexec Lua is never run.
    public static (string? MaxFps, bool Unknown) ReadAutoexec(string text)
    {
        try { return (new LuaOptions("options={\n" + text + "\n}").Get("max_fps"), false); }
        catch (InvalidDataException) { return (null, true); }
    }
}
