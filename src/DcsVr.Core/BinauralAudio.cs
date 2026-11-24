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
    /// '-', '_' and '.', not starting with '.'; ASCII only, no path separators. No space at either end: the module reads
    /// the ini value trimmed (GetPrivateProfileString), so such a name would ask it for another file.</summary>
    public static bool IsValidHrtfName(string? name) =>
        !string.IsNullOrEmpty(name) && name.Length <= MaxNameLength && name[0] != '.' && name[0] != ' ' && name[^1] != ' '
        && name.All(c => char.IsAsciiLetterOrDigit(c) || c is ' ' or '-' or '_' or '.');

    /// <summary>A profile's Hrtf value as the module reads it: null or blank is the generic set, "default" in any case is
    /// "default" (the module compares it exactly), anything else trimmed (the module reads the ini value trimmed).</summary>
    public static string NormalizeHrtfName(string? name)
    {
        var text = (name ?? "").Trim();
        return text.Length == 0 || text.Equals(DefaultHrtf, StringComparison.OrdinalIgnoreCase) ? DefaultHrtf : text;
    }

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

    /// <summary>The head radius of a circumference (binaural_config.h HeadRadiusFromCircumferenceCm), in mm. The module's
    /// time difference between the ears is the set's own scaled by circumference / 57 cm (PilotItdRadiusM).</summary>
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

    /// <summary>Launch DCS, with DCS closed: a <see cref="KillFlag"/> still next to the module was never taken (DCS ended
    /// within a second of the button, or the flag was made by hand) and would start the next session with 3D audio switched
    /// off. Removed; true when there was one.</summary>
    public static bool RemoveStaleKillFlag(string moduleDirectory)
    {
        var flag = Path.Combine(moduleDirectory, KillFlag);
        try
        {
            if (!File.Exists(flag)) return false;
            File.Delete(flag);
            return true;
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException) { return false; }
    }

    // ---- Does the installed engine module carry the audio part, and the generic set? -----------------------------------

    private static readonly ConcurrentDictionary<string, (long Length, DateTime Written, bool Has)> ResourceCache = new(StringComparer.OrdinalIgnoreCase);
    /// <summary>The payload embeds the generic HRTF set: an RCDATA resource named HRTF_DEFAULT with data in its PE resource
    /// directory (binaural_config.h kEmbeddedResourceName; native/dcsqvcull embeds native/binaural/data/sadie2_ku100.dcshrtf
    /// when the build has it). The name alone is also in the module's code, so this reads the resource table. Cached by
    /// size and time.</summary>
    public static bool PayloadHasDefaultHrtf(string? payloadPath)
    {
        if (payloadPath is null) return false;
        try
        {
            var info = new FileInfo(payloadPath);
            if (!info.Exists || info.Length > 256L * 1024 * 1024) return false;
            if (ResourceCache.TryGetValue(info.FullName, out var cached) && cached.Length == info.Length && cached.Written == info.LastWriteTimeUtc) return cached.Has;
            var has = PeResource.Find(File.ReadAllBytes(info.FullName), PeResource.RcData, "HRTF_DEFAULT") is { Length: > 0 };
            ResourceCache[info.FullName] = (info.Length, info.LastWriteTimeUtc, has);
            return has;
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException) { return false; }
    }

    /// <summary>3D audio has a set to render with: the generic one in this build's engine module, or one of the pilot's own.
    /// Without either, the module never patches DCS's sound engine, and Launch DCS does not deploy 3D audio at all.</summary>
    public static bool Usable(string? payloadPath, int personalSets) => personalSets > 0 || PayloadHasDefaultHrtf(payloadPath);
    public const string UnusableReason = "This DCS Control build has no generic HRTF set yet, and you have no set of your own.";

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
    ///   audio: on (... HRTF &lt;name&gt;: N directions, ...)   running, with that set
    ///   audio: HRTF &lt;name&gt; (crossfading)                 another set picked live
    ///   audio: off (&lt;reason&gt;)                             stayed or went off; DCS's own sound plays
    ///   audio: off (&lt;id&gt; not found (&lt;why&gt;))               a hook point not found in this DCS build
    ///   audio: kill switch (&lt;by&gt;): binaural OFF ... | ON   the kill switch (key or flag)
    ///   audio: running, N sounds, p99 X ms per block, ...; head pose on|off|unused   every 10 s while running
    ///   audio: CPU budget: at most N sounds now (... over budget)   CPU back-off (fewer 3D sounds to stay in the budget)
    ///   audio: CPU budget: at most N sounds now (10 s under half the budget)   back up
    ///   audio: on (re-enabled ..., HRTF &lt;name&gt;: N directions)   running again on the same binding
    ///   audio: HRTF &lt;name&gt; not usable (&lt;why&gt;); the default set is used   the pilot's set refused by the module
    /// A "running," line is written only while sounds were rendered in 3D, so it also means running after an "off".
    /// "off (binaural is off in the settings)" is the module following its settings (turned off live), not a failure:
    /// switched off. <paramref name="enabled"/>: the deployed [Audio] Binaural (false: the module is installed for
    /// other features and 3D audio is off in the profile); null when not known (Last flight reads a past session).
    /// </summary>
    public static AudioStatus ParseStatus(IEnumerable<string>? moduleLines, bool installed, bool dcsRunning, bool? enabled = null)
    {
        if (!installed) return new("not-installed", "Launch DCS installs it when 3D headphone audio is on.");
        var lines = new List<string>();
        foreach (var line in moduleLines ?? [])
        {
            if (line.Contains("loader started", StringComparison.Ordinal)) { lines.Clear(); continue; }
            if (line.Contains(" audio: ", StringComparison.Ordinal) || line.StartsWith("audio: ", StringComparison.Ordinal)) lines.Add(line);
        }
        if (enabled == false) return new("disabled", "Off in this profile. DCS's own sound plays.", Lines: lines.TakeLast(12).Select(l => l.Trim()).ToArray());
        string? state = null, reason = null, hrtf = null, fallback = null, fallbackWhy = null; int? sounds = null, maxNow = null; double? cost = null; bool? pose = null; var backoffs = 0; var switchedOff = false;
        foreach (var line in lines)
        {
            var text = line[(line.IndexOf("audio: ", StringComparison.Ordinal) + 7)..].Trim();
            if (text.StartsWith("off (", StringComparison.Ordinal))
            {
                state = "off"; reason = Inside(text[3..]);
                if (NotFoundRegex().Match(reason) is { Success: true } nf) reason = $"this DCS version's sound engine is not the one it was made for ({nf.Groups[1].Value})";
                continue;
            }
            if (text.StartsWith("on (", StringComparison.Ordinal)) { state = "running"; reason = null; switchedOff = false; }
            if (KillRegex().Match(text) is { Success: true } k) switchedOff = k.Groups[1].Value == "OFF";
            if (FallbackRegex().Match(text) is { Success: true } fb) { fallback = fb.Groups[1].Value; fallbackWhy = fb.Groups[2].Value; continue; }
            if (HrtfRegex().Match(text) is { Success: true } h)
            {
                hrtf = h.Groups[1].Value;
                // Another set picked live after a refused one: that one is in use now.
                if (!hrtf.Equals(DefaultHrtf, StringComparison.Ordinal)) fallback = fallbackWhy = null;
            }
            if (text.StartsWith("running,", StringComparison.Ordinal))
            {
                // Written only while sounds were rendered in 3D: running, also after an earlier "off" (turned back on live).
                if (state != "running") { state = "running"; reason = null; }
                if (SoundsRegex().Match(text) is { Success: true } s) sounds = int.Parse(s.Groups[1].Value, CultureInfo.InvariantCulture);
                if (P99Regex().Match(text) is { Success: true } c && double.TryParse(c.Groups[1].Value, NumberStyles.Float, CultureInfo.InvariantCulture, out var ms)) cost = ms;
                if (PoseRegex().Match(text) is { Success: true } p) pose = p.Groups[1].Value switch { "on" => true, "off" => false, _ => (bool?)null };
            }
            if (BudgetRegex().Match(text) is { Success: true } b)
            {
                maxNow = int.Parse(b.Groups[1].Value, CultureInfo.InvariantCulture);
                if (text.Contains("over budget", StringComparison.Ordinal)) backoffs++;
            }
        }
        var tail = lines.TakeLast(12).Select(l => l.Trim()).ToArray();
        if (state is null)
            return new("waiting", dcsRunning ? "Waiting for DCS's sound engine." : "Installed for the next DCS start.", Lines: tail);
        if (state == "off" && reason == SettingsOffReason)
            return new("switched-off", "Switched off in the settings. DCS's own sound plays.", null, Backoffs: backoffs, SwitchedOff: true, Hrtf: hrtf, Lines: tail) { SwitchedOffBy = "settings" };
        if (state == "off") return new("off", $"Off: {reason}. DCS's own sound plays.", reason, Backoffs: backoffs, Lines: tail) { HrtfFallback = fallback, HrtfFallbackReason = fallbackWhy };
        var parts = new List<string> { switchedOff ? "Switched off in flight" : dcsRunning ? "Running" : "Ran at the last DCS start" };
        if (sounds is { } n) parts.Add(n == 1 ? "1 sound" : n + " sounds");
        if (cost is { } t) parts.Add(t.ToString("0.0#", CultureInfo.InvariantCulture) + " ms");
        return new(switchedOff ? "switched-off" : "running", string.Join(" · ", parts), null, sounds, cost, backoffs, maxNow, switchedOff, hrtf, pose, tail)
            { HrtfFallback = fallback, HrtfFallbackReason = fallbackWhy, SwitchedOffBy = switchedOff ? "key" : null };

        static string Inside(string text)
        {
            var open = text.IndexOf('('); var close = text.LastIndexOf(')');
            return open >= 0 && close > open ? text[(open + 1)..close].Trim() : text.Trim('(', ')', ' ');
        }
    }

    /// <summary>The module's line when its settings turn 3D audio off (native/dcsqvcull/src/main/audio.inc).</summary>
    public const string SettingsOffReason = "binaural is off in the settings";

    /// <summary>The status of 3D audio from the module's newest log in the Saved Games of <paramref name="optionsPath"/>,
    /// and from the deployed DcsQvCull.ini: installed for other features with [Audio] Binaural not 1, it is "disabled".</summary>
    public static AudioStatus ReadStatus(string optionsPath, bool dcsRunning)
    {
        var module = EngineOptimizations.ModuleDirectory(optionsPath);
        var installed = File.Exists(Path.Combine(module, "DcsQvCull.dll"));
        bool? enabled = null;
        if (installed)
        {
            // The module's own default is Binaural=0: no file or no [Audio] is off.
            try
            {
                var ini = Path.Combine(module, LiveSettings.EngineFile);
                enabled = File.Exists(ini) && IniFile.Parse(IniFile.Decode(File.ReadAllBytes(ini))).TryGetValue("Audio.Binaural", out var on) && on.Trim() == "1";
            }
            catch (Exception e) when (e is IOException or UnauthorizedAccessException) { }
        }
        var log = EngineOptimizations.ModuleLog(module);
        List<string>? lines = null;
        try
        {
            if (installed && File.Exists(log))
                lines = LastFlight.ReadLines(log, 8L * 1024 * 1024, l => l.Contains("audio: ", StringComparison.Ordinal) || l.Contains("loader started", StringComparison.Ordinal));
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException) { }
        return ParseStatus(lines, installed, dcsRunning, enabled);
    }

    [GeneratedRegex(@"^([\w. -]+?) not found \(")] private static partial Regex NotFoundRegex();
    [GeneratedRegex(@"^(?:on \(.*?, )?HRTF ([\w .-]+?)(?=: \d+ directions| \(crossfading\))")] private static partial Regex HrtfRegex();
    [GeneratedRegex(@"; head pose (on|off|unused)\s*$")] private static partial Regex PoseRegex();
    [GeneratedRegex(@"^running, (\d+) sounds?\b")] private static partial Regex SoundsRegex();
    [GeneratedRegex(@"\bp99 (\d+(?:\.\d+)?) ms\b")] private static partial Regex P99Regex();
    [GeneratedRegex(@"^CPU budget: at most (\d+) sounds now\b")] private static partial Regex BudgetRegex();
    [GeneratedRegex(@"^kill switch \([^)]*\): binaural (ON|OFF)\b")] private static partial Regex KillRegex();
    [GeneratedRegex(@"^HRTF ([\w .-]+?) not usable \((.*)\); the default set is used\s*$")] private static partial Regex FallbackRegex();
}

/// <summary>3D audio as the module's log has it (<see cref="Binaural.ParseStatus"/>).</summary>
/// <param name="State">"not-installed", "disabled" (the module is installed for other features, 3D audio is off in the
/// deployed settings), "unavailable" (on in the profile, but there is no HRTF set to render with: nothing deployed),
/// "waiting" (installed, no audio line yet), "running", "switched-off" (the kill switch or the settings, live: see
/// <see cref="SwitchedOffBy"/>) or "off" (it stayed or went off: <paramref name="Reason"/>).</param>
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
    /// <summary>The pilot's own set the module refused at the last DCS start (it used the generic one), and why.</summary>
    public string? HrtfFallback { get; init; }
    /// <summary>With "switched-off": "key" (the in-flight key or toggle_audio.flag) or "settings" (turned off live).</summary>
    public string? SwitchedOffBy { get; init; }
    public string? HrtfFallbackReason { get; init; }
}

/// <summary>Reads one resource's data from a PE image's resource directory (no loading, no execution).</summary>
public static class PeResource
{
    public const int RcData = 10;
    /// <summary>The data of the first language of resource <paramref name="type"/> / <paramref name="name"/> (a string
    /// name, compared case-insensitively as Windows does), or null when absent or the image is malformed.</summary>
    public static byte[]? Find(byte[] image, int type, string name)
    {
        try
        {
            uint U32(int o) => System.Buffers.Binary.BinaryPrimitives.ReadUInt32LittleEndian(image.AsSpan(o, 4));
            ushort U16(int o) => System.Buffers.Binary.BinaryPrimitives.ReadUInt16LittleEndian(image.AsSpan(o, 2));
            if (image.Length < 0x40 || U16(0) != 0x5a4d) return null;
            var pe = (int)U32(0x3c);
            if (pe < 0 || pe + 24 > image.Length || U32(pe) != 0x4550) return null;
            var sections = U16(pe + 6); var optSize = U16(pe + 20); var opt = pe + 24;
            var magic = U16(opt);
            var dirs = opt + (magic == 0x20b ? 112 : magic == 0x10b ? 96 : -1);
            if (dirs < opt || dirs + 8 * 3 > image.Length) return null;
            var rsrcRva = U32(dirs + 8 * 2);
            if (rsrcRva == 0) return null;
            var table = opt + optSize;
            int Offset(uint rva)
            {
                for (var i = 0; i < sections; i++)
                {
                    var sh = table + 40 * i;
                    var va = U32(sh + 12); var size = Math.Max(U32(sh + 8), U32(sh + 16)); var raw = U32(sh + 20);
                    if (rva >= va && rva < va + size) return checked((int)(raw + (rva - va)));
                }
                return -1;
            }
            var root = Offset(rsrcRva);
            if (root < 0) return null;
            // A directory's entry matching an id (name == null) or a string name; returns the offset field.
            uint? Entry(int dir, int? id, string? text)
            {
                int named = U16(dir + 12), ids = U16(dir + 14);
                for (var i = 0; i < named + ids; i++)
                {
                    var e = dir + 16 + 8 * i;
                    var n = U32(e);
                    if ((n & 0x80000000) != 0)
                    {
                        if (text is null) continue;
                        var at = root + (int)(n & 0x7fffffff);
                        var len = U16(at);
                        if (string.Equals(System.Text.Encoding.Unicode.GetString(image.AsSpan(at + 2, len * 2)), text, StringComparison.OrdinalIgnoreCase)) return U32(e + 4);
                    }
                    else if (id is { } want && n == want) return U32(e + 4);
                }
                return null;
            }
            if (Entry(root, type, null) is not { } t || (t & 0x80000000) == 0) return null;
            var typeDir = root + (int)(t & 0x7fffffff);
            if (Entry(typeDir, null, name) is not { } nm || (nm & 0x80000000) == 0) return null;
            var nameDir = root + (int)(nm & 0x7fffffff);
            if (U16(nameDir + 12) + U16(nameDir + 14) == 0) return null;
            var lang = U32(nameDir + 16 + 4);
            if ((lang & 0x80000000) != 0) return null;
            var data = root + (int)lang;
            var dataRva = U32(data); var dataSize = (int)U32(data + 4);
            var at2 = Offset(dataRva);
            return at2 < 0 || at2 + dataSize > image.Length ? null : image.AsSpan(at2, dataSize).ToArray();
        }
        catch (Exception e) when (e is ArgumentOutOfRangeException or OverflowException or IndexOutOfRangeException) { return null; }
    }
}
