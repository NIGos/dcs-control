using System.Text.Json;
using System.Text.RegularExpressions;

namespace DcsVr.Core;

/// <summary>The headset software of this PC: Pimax Play's version (its uninstall entry), the Pimax runtime's build
/// (the newest pvr_srv_log_*.txt header), the headset model and the active OpenXR runtime, with what the last DCS
/// launch from the app saw. A new runtime explained a white-disc flight only after its logs were read by hand.</summary>
public sealed record HeadsetRuntimeInfo(string? PimaxPlay, string? RuntimeBuild, string? RuntimeId, string? Headset, string? OpenXrRuntime,
    string? PreviousPimaxPlay = null, string? PreviousRuntimeBuild = null)
{
    /// <summary>Pimax Play or the runtime build differs from the last DCS launch from the app (each compared only when
    /// both are known).</summary>
    public bool Changed => Differs(PreviousPimaxPlay, PimaxPlay) || Differs(PreviousRuntimeBuild, RuntimeBuild);
    private static bool Differs(string? before, string? now) => before is not null && now is not null && before != now;

    /// <summary>One line for the report, README.txt and app.log.</summary>
    public string Line
    {
        get
        {
            var parts = new List<string>();
            if (Headset is not null) parts.Add(Headset);
            if (PimaxPlay is not null) parts.Add("Pimax Play " + PimaxPlay);
            if (RuntimeBuild is not null) parts.Add("Pimax runtime built " + RuntimeBuild + (RuntimeId is null ? "" : " (" + RuntimeId + ")"));
            if (OpenXrRuntime is not null) parts.Add("OpenXR runtime " + OpenXrRuntime);
            var line = "Headset/runtime: " + (parts.Count == 0 ? "not detected" : string.Join(" · ", parts));
            if (Changed)
                line += " · CHANGED since the last flight (was " + string.Join(", ", new[]
                {
                    Differs(PreviousPimaxPlay, PimaxPlay) ? "Pimax Play " + PreviousPimaxPlay : null,
                    Differs(PreviousRuntimeBuild, RuntimeBuild) ? "runtime built " + PreviousRuntimeBuild : null,
                }.OfType<string>()) + ")";
            return line;
        }
    }
}

public static partial class HeadsetRuntime
{
    /// <summary>In the app's state folder: what the last DCS launch from the app saw.</summary>
    public const string StateFile = "headset-runtime.json";

    [GeneratedRegex(@"\[PSRV\] build time:\s*(?<build>.+?)\s*$", RegexOptions.Multiline)] private static partial Regex BuildRegex();
    [GeneratedRegex(@"\[PSRV\] version detail:\s*(?<id>\S+)", RegexOptions.Multiline)] private static partial Regex IdRegex();
    // Pimax's USB vendor id (13476 = 0x34A4) only: the server also adds other hid devices, e.g. a Razer headset first.
    [GeneratedRegex(@"added hid device:13476(?::\d+)*-(?<name>[^\r\n]+?)\s*$", RegexOptions.Multiline)] private static partial Regex HeadsetRegex();

    private sealed record Seen(string? PimaxPlay, string? RuntimeBuild);

    /// <summary>The headset software now, compared with the state folder's record of the last flight (none: no flag).</summary>
    public static HeadsetRuntimeInfo Read(string? pimaxPlay, string? activeRuntime, string runtimeLogFolder, string? stateRoot)
    {
        var (build, id, headset) = ReadServerLog(runtimeLogFolder);
        Seen? before = null;
        try
        {
            if (stateRoot is not null && File.Exists(Path.Combine(stateRoot, StateFile)))
                before = JsonSerializer.Deserialize<Seen>(File.ReadAllText(Path.Combine(stateRoot, StateFile)), JsonData.Options);
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException or JsonException) { }
        return new(pimaxPlay, build, id, headset, string.IsNullOrWhiteSpace(activeRuntime) ? null : Path.GetFileName(activeRuntime), before?.PimaxPlay, before?.RuntimeBuild);
    }

    /// <summary>Records what this DCS launch sees, so the next report can say whether it changed. Fields not known now
    /// keep their earlier value.</summary>
    public static void RememberFlight(HeadsetRuntimeInfo info, string stateRoot)
    {
        var seen = new Seen(info.PimaxPlay ?? info.PreviousPimaxPlay, info.RuntimeBuild ?? info.PreviousRuntimeBuild);
        if (seen.PimaxPlay is null && seen.RuntimeBuild is null) return;
        Directory.CreateDirectory(stateRoot);
        File.WriteAllText(Path.Combine(stateRoot, StateFile), JsonSerializer.Serialize(seen, JsonData.Options));
    }

    /// <summary>The build date, build id and headset model from the start of the newest Pimax server log.</summary>
    internal static (string? Build, string? Id, string? Headset) ReadServerLog(string folder)
    {
        try
        {
            var log = Directory.Exists(folder) ? new DirectoryInfo(folder).GetFiles("pvr_srv_log_*.txt").OrderByDescending(f => f.LastWriteTimeUtc).FirstOrDefault() : null;
            if (log is null) return (null, null, null);
            using var stream = new FileStream(log.FullName, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete);
            var head = new byte[Math.Min(stream.Length, 512 * 1024)];
            stream.ReadExactly(head);
            return ParseServerLog(System.Text.Encoding.UTF8.GetString(head));
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException) { return (null, null, null); }
    }

    internal static (string? Build, string? Id, string? Headset) ParseServerLog(string text)
    {
        // "Nov  6 2026 20:19:38" (the C __DATE__ pads the day): one space.
        var build = BuildRegex().Match(text) is { Success: true } b ? Regex.Replace(b.Groups["build"].Value, @"\s+", " ") : null;
        var id = IdRegex().Match(text) is { Success: true } i ? i.Groups["id"].Value : null;
        var headset = HeadsetRegex().Match(text) is { Success: true } h ? h.Groups["name"].Value : null;
        return (build, id, headset);
    }
}

/// <summary>Names the registered OpenXR runtime from its manifest path.</summary>
public static class OpenXrRuntimeNames
{
    private static readonly (string Part, string Name)[] Known =
    [
        ("PiOpenXR", "Pimax"), ("steamxr", "SteamVR"), ("Varjo", "Varjo"), ("oculus", "Meta (Oculus)"), ("virtualdesktop", "Virtual Desktop (VDXR)"),
        ("MixedRealityRuntime", "Windows Mixed Reality"), ("pico", "PICO"), ("vive", "VIVE"),
    ];

    /// <summary>The runtime's name, or the manifest's file name when it is not one of the known runtimes.</summary>
    public static string? Name(string? manifest)
    {
        if (string.IsNullOrWhiteSpace(manifest)) return null;
        var normalized = manifest.Replace('\\', '/');
        foreach (var (part, name) in Known) if (normalized.Contains(part, StringComparison.OrdinalIgnoreCase)) return name;
        return Path.GetFileNameWithoutExtension(normalized);
    }

    /// <summary>The name when the runtime is neither Pimax's nor SteamVR's, the two the VR features are made for.</summary>
    public static string? Other(string? manifest) => Name(manifest) is { } name && name is not "Pimax" and not "SteamVR" ? name : null;
}