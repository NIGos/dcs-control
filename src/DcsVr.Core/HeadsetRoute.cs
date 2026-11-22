namespace DcsVr.Core;

/// <summary>Per-route facts in one place: the OpenXR runtime a route uses, how messages name it and its setup guide.</summary>
public static class HeadsetRoute
{
    /// <summary>The OpenXR runtime manifest DCS is pointed at: the profile's own file, else the route's runtime found on this PC.</summary>
    public static string? Manifest(VrProfile profile, InventorySnapshot inventory) => profile.RuntimeManifestPath ?? profile.Runtime switch
    {
        RuntimeKind.SboysSteamVr => inventory.SteamVrRuntime,
        RuntimeKind.VirtualDesktop => inventory.VirtualDesktopRuntime,
        _ => inventory.PimaxRuntime
    };

    /// <summary>"This profile uses ..." in route messages.</summary>
    public static string Describe(RuntimeKind route) => route switch
    {
        RuntimeKind.SboysSteamVr => "Sboys through SteamVR",
        RuntimeKind.VirtualDesktop => "Quest 3 VD (Virtual Desktop, VDXR)",
        _ => "the Pimax runtime"
    };

    /// <summary>The setup guide for the route's runtime software (WebBridge openGuide).</summary>
    public static string RuntimeGuide(RuntimeKind route) => route switch
    {
        RuntimeKind.SboysSteamVr => "steamvr",
        RuntimeKind.VirtualDesktop => "virtualdesktop",
        _ => "pimax"
    };

    /// <summary>The setup guide for the headset itself.</summary>
    public static string HeadsetGuide(RuntimeKind route) => route switch
    {
        RuntimeKind.SboysSteamVr => "sboys",
        RuntimeKind.VirtualDesktop => "virtualdesktop",
        _ => "pimax"
    };

    /// <summary>
    /// Starting focus values for Quest 3 on Virtual Desktop, which has no Pimax Play to read and no eye tracking. A fixed
    /// focus of 0.55 x 0.75 (tall enough to reach the instruments below the view centre, tested in the headset) keeps them sharp; the periphery is kept at 0.40 (not Pimax's 0.19) because with a
    /// fixed focus the eyes look into it; the focus runs at 1.0x because VD's own quality setting already sets the
    /// resolution. Starting points only, to be tuned in the headset.
    /// </summary>
    public static VrProfile WithQuest3Focus(VrProfile profile) => profile with
    {
        FoveaSource = FoveaSource.Profile, Gaze = GazeMode.Fixed,
        FoveaWidth = 0.55, FoveaHeight = 0.75, PeripheralScale = 0.40, QuadFocusScale = 1.0, QuadEdgeBlend = 0.2
    };

    /// <summary>The Quest 3 rate (72, 80, 90, 120 Hz) nearest to <paramref name="hz"/>; the lower one on a tie.</summary>
    public static double NearestQuest3RefreshHz(double hz) =>
        ProfileValidation.Quest3RefreshHz.OrderBy(rate => Math.Abs(rate - hz)).ThenBy(rate => rate).First();

    /// <summary>What to do when the route's runtime manifest was not found.</summary>
    public static string RuntimeMissing(RuntimeKind route) => route switch
    {
        RuntimeKind.VirtualDesktop => "Virtual Desktop's OpenXR runtime (VDXR) was not found. Install Virtual Desktop Streamer, then press Check this PC.",
        _ => "The headset's OpenXR runtime was not found. Start Pimax Play (or SteamVR) once, then press Check this PC."
    };
}
