namespace DcsVr.Core;

/// <summary>Where the CPU ranking used by CPU Boost came from.</summary>
public enum CoreRankingSource { None, DcsLog, Windows }

/// <summary>One process CPU Boost would change, as shown before launch.</summary>
/// <param name="Category">"DCS", "VR runtime" or "Background".</param>
/// <param name="Action">Plain-language change, e.g. "Priority Above normal, all CPUs".</param>
/// <param name="NeedsAdministrator">Runs under another account (a service): changeable only with administrator rights.</param>
public sealed record BoostProcess(string Name, string Category, string Action, bool Running, bool NeedsAdministrator);

/// <summary>What CPU Boost will do for a profile on this PC. Read-only: building it changes nothing.</summary>
/// <param name="RenderCores">Logical CPUs DCS prefers for its main and render threads.</param>
/// <param name="VrRuntimeCores">Logical CPUs the VR runtime and headset services are confined to.</param>
/// <param name="BackgroundCores">Logical CPUs background apps are confined to (the slowest ones).</param>
/// <param name="FreeVram">Free VRAM before flight: each listed program, what it would close now and the video memory it holds.</param>
/// <param name="FreeVramBytes">Approximate total dedicated video memory of the programs it would close; null when the
/// GPU counters cannot be read.</param>
/// <param name="Monitor">Lower the monitor while flying: the primary display, its current mode, the flight mode and
/// the modes it reports. Null when no display can be read.</param>
/// <param name="SmallWindow">Small DCS window in VR: the options.lua values the profile sets.</param>
public sealed record BoostPlan(
    bool Enabled,
    CoreRankingSource Source,
    string SourceDescription,
    IReadOnlyList<int> RenderCores,
    IReadOnlyList<int> VrRuntimeCores,
    IReadOnlyList<int> BackgroundCores,
    IReadOnlyList<BoostProcess> Processes,
    IReadOnlyList<string> Notes,
    IReadOnlyList<FreeVramApp>? FreeVram = null,
    long? FreeVramBytes = null,
    MonitorPlan? Monitor = null,
    string? SmallWindow = null,
    IReadOnlyList<string>? TobiiServices = null);

public static partial class BoostPlanner
{
    /// <summary>Describes what CPU Boost would do; never changes processes.</summary>
    public static BoostPlan Describe(VrProfile profile, string? dcsLogPath = null) =>
        Compose(profile, CpuTopology.Detect(dcsLogPath), CaptureProcesses(), FreeVram.DedicatedVideoMemory(), new WindowsDisplayModes());
}
