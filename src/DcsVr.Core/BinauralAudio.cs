using System.Collections.Concurrent;
using System.Globalization;
using System.Text;
using System.Text.RegularExpressions;

namespace DcsVr.Core;

/// <summary>
/// 3D headphone audio, the app side (docs/BINAURAL_AUDIO.md, task D): the engine module's [Audio] section, its key names
/// and ranges (the same as native/binaural/include/binaural_config.h, which a test reads and compares), the kill switch
/// flag, DCS's speaker layout, and what the module's log says about it.
/// </summary>
public static partial class Binaural
{
    public const string Section = "Audio";
    /// <summary>The kill switch: created next to DcsQvCull.dll, the module's worker flips 3D audio off or on for the
    /// session and deletes it (binaural_config.h kKillFlagFile).</summary>
    public const string KillFlag = "toggle_audio.flag";
    /// <summary>Personal HRTF sets in the module's folder: Scripts\DcsQvCull\hrtf\&lt;name&gt;.dcshrtf.</summary>
    public const string HrtfFolder = "hrtf";
    public const string HrtfExtension = ".dcshrtf";
    /// <summary>The generic set embedded in the module (SADIE II, Neumann KU100 dummy head).</summary>
    public const string DefaultHrtf = "default";
    public const string DefaultHrtfTitle = "Generic (KU100)";
    /// <summary>PupilShift.ini: the layer publishes the head pose for late head tracking only with Publish=1.</summary>
    public const string PoseSection = "HeadPose", PoseKey = "Publish";

    // The [Audio] keys' ranges, as binaural_config.h kKeys has them.
    public const double HeadCircumferenceMin = 48, HeadCircumferenceMax = 66, HeadCircumferenceDefault = 57;
    /// <summary>The Head size presets (head circumference in cm), as in docs/BINAURAL_AUDIO.md section 1.</summary>
    public const double HeadSmall = 54, HeadMedium = 57, HeadLarge = 60;
    public const int MaxObjectsMin = 8, MaxObjectsMax = 64, MaxObjectsDefault = 48;
    public const double LevelTrimMin = -12, LevelTrimMax = 6;
    public const int CpuBudgetMin = 5, CpuBudgetMax = 50, CpuBudgetDefault = 25;
    public const int MaxNameLength = 64;
    /// <summary>Context ids the module knows (ExcludeContexts takes 0 to 7).</summary>
    public const int MaxContextId = 7;

    /// <summary>A valid Hrtf value (binaural_config.h IsValidHrtfName): "default", or 1 to 64 letters, digits, spaces,
    /// '-', '_' and '.', not starting with '.'; ASCII only, no path separators.</summary>
    public static bool IsValidHrtfName(string? name) =>
        !string.IsNullOrEmpty(name) && name.Length <= MaxNameLength && name[0] != '.' && name.All(c => char.IsAsciiLetterOrDigit(c) || c is ' ' or '-' or '_' or '.');

    /// <summary>A name for an imported set from a file name: the characters <see cref="IsValidHrtfName"/> refuses become
    /// '_', spaces at the ends and leading dots go, at most 64 characters; "personal" when nothing is left, and never
    /// "default" (the generic set's name).</summary>
    public static string NameFrom(string? raw)
    {
        var text = new string((raw ?? "").Select(c => char.IsAsciiLetterOrDigit(c) || c is ' ' or '-' or '_' or '.' ? c : '_').ToArray()).Trim().TrimStart('.').Trim();
        if (text.Length > MaxNameLength) text = text[..MaxNameLength].TrimEnd();
        if (text.Length == 0) return "personal";
        return text.Equals(DefaultHrtf, StringComparison.OrdinalIgnoreCase) ? "default-personal" : text;
    }

    /// <summary>"1, 3,5" → bits 1, 3 and 5 (binaural_config.h ParseExcludeContexts): integers 0 to 7, commas, spaces.
    /// Empty or null is no context.</summary>
    public static bool TryParseExcludeContexts(string? text, out uint mask)
    {
        mask = 0;
        if (string.IsNullOrWhiteSpace(text)) return true;
        foreach (var raw in text.Split(','))
        {
            var token = raw.Trim(' ', '\t');
            if (token.Length == 0 || token.Length > 1 || token[0] is < '0' or > '7') { mask = 0; return false; }
            mask |= 1u << (token[0] - '0');
        }
        return true;
    }

    /// <summary>The ExcludeContexts value written: the ids in order ("1,3"), or null for none (the key is left out).</summary>
    public static string? ExcludeContextsValue(string? text) =>
        TryParseExcludeContexts(text, out var mask) && mask != 0 ? string.Join(",", Enumerable.Range(0, MaxContextId + 1).Where(i => (mask & (1u << i)) != 0)) : null;

    /// <summary>The head radius the module derives (binaural_config.h HeadRadiusFromCircumferenceCm), in mm.</summary>
    public static double HeadRadiusMm(double circumferenceCm) =>
        Math.Clamp(Math.Clamp(double.IsFinite(circumferenceCm) ? circumferenceCm : HeadCircumferenceDefault, HeadCircumferenceMin, HeadCircumferenceMax) / 100 / (2 * Math.PI), .06, .12) * 1000;

    // ---- DCS's speaker layout (options.lua sound.main_layout) --------------------------------------------------------

    /// <summary>The layout the module needs: DCS's two-channel output (Options → Audio → Speaker layout: Stereo).</summary>
    public const string StereoLayout = "stereo";
    /// <summary>options.lua's own value without quotes; "" is DCS's Auto (the Windows device's layout).</summary>
    public static string LayoutValue(string? raw) => (raw ?? "").Trim().Trim('"').Trim();
    /// <summary>DCS's name for a layout value (the ids ed_sound lists: stereo, stereo_2_1, quadraphonic, suround_5_1, ...).</summary>
    public static string LayoutName(string? raw) => LayoutValue(raw).ToLowerInvariant() switch
    {
        "" => "Auto", "stereo" => "Stereo", "mono" => "Mono", "stereo_2_1" => "2.1", "quadraphonic" => "Quadraphonic",
        "suround_5_1" or "surround_5_1" => "5.1", "suround_7_1" or "surround_7_1" => "7.1", "spatial_8_1_4_4" => "Spatial 8.1.4.4", var other => other,
    };
    /// <summary>Windows spatial sound through DCS (options.lua sound.driver_name = "spatial", a hand edit): it gives DCS a
    /// speaker bed, not two channels, so the module stays off.</summary>
    public static bool SpatialDriver(string? raw) => LayoutValue(raw).Equals("spatial", StringComparison.OrdinalIgnoreCase);

    // ---- The kill switch ----------------------------------------------------------------------------------------------

    /// <summary>Asks the running module to switch 3D audio off, or back on (the same as the in-flight key): creates
    /// <see cref="KillFlag"/> next to DcsQvCull.dll. The module's worker takes it within a second and deletes it.</summary>
    public static string ToggleNow(string optionsPath, bool dcsRunning)
    {
        if (!dcsRunning) throw new InvalidOperationException("DCS is not running. 3D audio starts with DCS as set here.");
        var module = EngineOptimizations.ModuleDirectory(optionsPath);
        if (!File.Exists(Path.Combine(module, "DcsQvCull.dll"))) throw new InvalidOperationException("3D headphone audio is not installed yet. Turn it on and start DCS with Launch DCS.");
        File.WriteAllBytes(Path.Combine(module, KillFlag), []);
        return "Asked DCS to switch 3D audio off, or back on. It takes about a second.";
    }

    // ---- Does the installed engine module carry the audio part? ------------------------------------------------------

    private static readonly ConcurrentDictionary<string, (long Length, DateTime Written, bool Has)> PayloadCache = new(StringComparer.OrdinalIgnoreCase);
    private static readonly byte[] EmbeddedMarker = Encoding.Unicode.GetBytes("HRTF_DEFAULT");
    /// <summary>The installed payload has the audio module: its generic HRTF is embedded as the resource HRTF_DEFAULT
    /// (docs/BINAURAL_AUDIO.md, task B), whose name is in the PE resource directory as UTF-16. An earlier payload ignores
    /// [Audio], so Live mode must not report a change there as applied. Cached by size and time.</summary>
    public static bool PayloadHasAudio(string? payloadPath)
    {
        if (payloadPath is null) return false;
        try
        {
            var info = new FileInfo(payloadPath);
            if (!info.Exists || info.Length > 256L * 1024 * 1024) return false;
            if (PayloadCache.TryGetValue(info.FullName, out var cached) && cached.Length == info.Length && cached.Written == info.LastWriteTimeUtc) return cached.Has;
            var has = File.ReadAllBytes(info.FullName).AsSpan().IndexOf(EmbeddedMarker) >= 0;
            PayloadCache[info.FullName] = (info.Length, info.LastWriteTimeUtc, has);
            return has;
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException) { return false; }
    }

    // ---- What the module's log says ---------------------------------------------------------------------------------

    /// <summary>
    /// 3D audio in the module's log (DcsQvCull.log): the "audio:" lines of the last DCS start (after the last "loader
    /// started"). The lines this reads (docs/BINAURAL_AUDIO.md, "Log lines the app reads"):
    ///   audio: on (...)                     running (hrtf &lt;name&gt; and head pose on/off may be named inside)
    ///   audio: off (&lt;reason&gt;)               stayed or went off; DCS's own sound plays
    ///   audio: &lt;id&gt; not found (&lt;why&gt;)        a hook point not found in this DCS build: off
    ///   audio: switched off | switched on   the kill switch (key or flag)
    ///   audio: stats ... sounds=N ... p99=X ms   every few seconds while running
    ///   audio: budget ... MaxObjects A -&gt; B   CPU back-off (fewer 3D sounds to stay in the budget)
    /// </summary>
    public static AudioStatus ParseStatus(IEnumerable<string>? moduleLines, bool installed, bool dcsRunning)
    {
        if (!installed) return new("not-installed", "Launch DCS installs it when 3D headphone audio is on.");
        var lines = new List<string>();
        foreach (var line in moduleLines ?? [])
        {
            if (line.Contains("loader started", StringComparison.Ordinal)) { lines.Clear(); continue; }
            if (line.Contains(" audio: ", StringComparison.Ordinal) || line.StartsWith("audio: ", StringComparison.Ordinal)) lines.Add(line);
        }
        string? state = null, reason = null, hrtf = null; int? sounds = null, maxNow = null; double? cost = null; bool? pose = null; var backoffs = 0; var switchedOff = false;
        foreach (var line in lines)
        {
            var text = line[(line.IndexOf("audio: ", StringComparison.Ordinal) + 7)..].Trim();
            if (NotFoundRegex().Match(text) is { Success: true } nf) { state = "off"; reason = $"this DCS version's sound engine is not the one it was made for ({nf.Groups[1].Value})"; continue; }
            if (text.StartsWith("off (", StringComparison.Ordinal)) { state = "off"; reason = Inside(text[3..]); continue; }
            if (text.StartsWith("on", StringComparison.Ordinal) && (text.Length == 2 || text[2] is ' ' or '(' or ':')) { state = "running"; reason = null; switchedOff = false; }
            if (text.StartsWith("switched off", StringComparison.Ordinal)) switchedOff = true;
            else if (text.StartsWith("switched on", StringComparison.Ordinal)) switchedOff = false;
            if (HrtfRegex().Match(text) is { Success: true } h) hrtf = h.Groups[1].Value;
            if (PoseRegex().Match(text) is { Success: true } p) pose = p.Groups[1].Value is "on" or "yes" or "1";
            if (text.StartsWith("stats", StringComparison.Ordinal))
            {
                if (state is null) state = "running";
                if (SoundsRegex().Match(text) is { Success: true } s) sounds = int.Parse(s.Groups[1].Value, CultureInfo.InvariantCulture);
                if (P99Regex().Match(text) is { Success: true } c && double.TryParse(c.Groups[1].Value.Replace(',', '.'), NumberStyles.Float, CultureInfo.InvariantCulture, out var ms)) cost = ms;
            }
            if (text.StartsWith("budget", StringComparison.Ordinal) || text.Contains("back-off", StringComparison.Ordinal) || text.Contains("backoff", StringComparison.Ordinal))
            {
                backoffs++;
                if (BudgetRegex().Match(text) is { Success: true } b) maxNow = int.Parse(b.Groups[1].Value, CultureInfo.InvariantCulture);
            }
        }
        var tail = lines.TakeLast(12).Select(l => l.Trim()).ToArray();
        if (state is null)
            return new("waiting", dcsRunning ? "Waiting for DCS's sound engine." : "Installed for the next DCS start.", Lines: tail);
        if (state == "off") return new("off", $"Off: {reason}. DCS's own sound plays.", reason, Backoffs: backoffs, Lines: tail);
        var parts = new List<string> { switchedOff ? "Switched off in flight" : dcsRunning ? "Running" : "Ran at the last DCS start" };
        if (sounds is { } n) parts.Add(n == 1 ? "1 sound" : n + " sounds");
        if (cost is { } t) parts.Add(t.ToString("0.0#", CultureInfo.InvariantCulture) + " ms");
        return new(switchedOff ? "switched-off" : "running", string.Join(" · ", parts), null, sounds, cost, backoffs, maxNow, switchedOff, hrtf, pose, tail);

        static string Inside(string text)
        {
            var open = text.IndexOf('('); var close = text.LastIndexOf(')');
            return open >= 0 && close > open ? text[(open + 1)..close].Trim() : text.Trim('(', ')', ' ');
        }
    }

    /// <summary>The status of 3D audio from the module's newest log in the Saved Games of <paramref name="optionsPath"/>.</summary>
    public static AudioStatus ReadStatus(string optionsPath, bool dcsRunning)
    {
        var module = EngineOptimizations.ModuleDirectory(optionsPath);
        var installed = File.Exists(Path.Combine(module, "DcsQvCull.dll"));
        var log = EngineOptimizations.ModuleLog(module);
        List<string>? lines = null;
        try
        {
            if (installed && File.Exists(log))
                lines = LastFlight.ReadLines(log, 8L * 1024 * 1024, l => l.Contains("audio: ", StringComparison.Ordinal) || l.Contains("loader started", StringComparison.Ordinal));
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException) { }
        return ParseStatus(lines, installed, dcsRunning);
    }

    [GeneratedRegex(@"^([\w.-]+) not found\b")] private static partial Regex NotFoundRegex();
    [GeneratedRegex(@"\bhrtf[= ]([\w .-]+?)(?=[,;)]|\s{2}|$| \()")] private static partial Regex HrtfRegex();
    [GeneratedRegex(@"\b(?:head pose|pose)[=: ]+(on|off|yes|no|1|0)\b")] private static partial Regex PoseRegex();
    [GeneratedRegex(@"\b(?:sounds|owned|voices)=(\d+)")] private static partial Regex SoundsRegex();
    [GeneratedRegex(@"\bp99=([\d.,]+)\s*ms")] private static partial Regex P99Regex();
    [GeneratedRegex(@"(?:->|→|MaxObjects=)\s*(\d+)\s*$|(?:->|→)\s*(\d+)\b")] private static partial Regex BudgetRegex();
}

/// <summary>3D audio as the module's log has it (<see cref="Binaural.ParseStatus"/>).</summary>
/// <param name="State">"not-installed", "waiting" (installed, no audio line yet), "running", "switched-off" (the kill
/// switch) or "off" (it stayed or went off: <paramref name="Reason"/>).</param>
/// <param name="Summary">One line for the page ("Running · 32 sounds · 0.2 ms", "Off: … DCS's own sound plays.").</param>
/// <param name="Sounds">Sounds rendered in 3D at the last stats line.</param>
/// <param name="CostMs">The 99th percentile of its time per audio block at the last stats line.</param>
/// <param name="Backoffs">Times it lowered the number of 3D sounds to stay within its CPU budget.</param>
/// <param name="MaxObjectsNow">The number it lowered to last.</param>
/// <param name="HeadTracking">Late head tracking from the headset's pose: on, off (no pose), or not said.</param>
/// <param name="Lines">The last audio lines, for the page's More.</param>
public sealed record AudioStatus(string State, string Summary, string? Reason = null, int? Sounds = null, double? CostMs = null, int Backoffs = 0,
    int? MaxObjectsNow = null, bool SwitchedOff = false, string? Hrtf = null, bool? HeadTracking = null, IReadOnlyList<string>? Lines = null)
{
    public IReadOnlyList<string> Lines { get; init; } = Lines ?? [];
}
