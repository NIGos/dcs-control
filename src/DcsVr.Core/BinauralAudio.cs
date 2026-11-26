using System.Collections.Concurrent;
using System.Globalization;
using System.Text;
using System.Text.RegularExpressions;

namespace DcsVr.Core;

/// <summary>
/// 3D headphone audio, the app side (docs/BINAURAL_AUDIO.md): the engine module's [Audio] section, its key names
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

    // ---- Cockpit space (docs/COCKPIT_SPACE.md section 7) -------------------------------------------------------------

    /// <summary>[Audio] CockpitSpace: 0 Off, 1 Subtle (default), 2 Normal, 3 Strong.</summary>
    public const int CockpitSpaceMin = 0, CockpitSpaceMax = 3, CockpitSpaceDefault = 1;
    /// <summary>The level names, as the module logs them ("audio: cockpit space: Subtle (...)") and the page shows them.</summary>
    public static IReadOnlyList<string> CockpitSpaceLevels { get; } = ["Off", "Subtle", "Normal", "Strong"];
    /// <summary>[Audio] CockpitSpaceFamily: "auto" (from the aircraft flown) or a family id (cockpit_space.h).</summary>
    public const string CockpitFamilyAuto = "auto";
    /// <summary>The families (id, name on the page), in the page's order; "auto" first.</summary>
    public static IReadOnlyList<(string Id, string Title)> CockpitFamilies { get; } =
    [
        (CockpitFamilyAuto, "Automatic"), ("bubble", "Bubble canopy"), ("framed", "Framed canopy"), ("helicopter", "Helicopter cabin"),
        ("warbird", "Warbird"), ("transport", "Transport flight deck"), ("generic", "Generic"),
    ];
    /// <summary>A family id the module knows (case-insensitive), "auto" included.</summary>
    public static bool IsCockpitFamily(string? id) => CockpitFamilies.Any(f => f.Id.Equals((id ?? "").Trim(), StringComparison.OrdinalIgnoreCase));
    /// <summary>A profile's family as written: trimmed and lower case; null or blank is "auto". An unknown name is kept
    /// (Checks names it; the writer writes "auto" for it).</summary>
    public static string NormalizeCockpitFamily(string? id) => string.IsNullOrWhiteSpace(id) ? CockpitFamilyAuto : id.Trim().ToLowerInvariant();
    /// <summary>The CockpitSpaceFamily value written: the family when known, else "auto".</summary>
    public static string CockpitFamilyValue(string? id) => IsCockpitFamily(id) ? NormalizeCockpitFamily(id) : CockpitFamilyAuto;
    /// <summary>The page's name of a family id ("bubble" → "Bubble canopy"); the id itself when unknown.</summary>
    public static string CockpitFamilyTitle(string? id) =>
        CockpitFamilies.FirstOrDefault(f => f.Id.Equals((id ?? "").Trim(), StringComparison.OrdinalIgnoreCase)).Title ?? (id ?? "");
    /// <summary>The level's name ("Subtle"); out of range clamped.</summary>
    public static string CockpitSpaceName(int level) => CockpitSpaceLevels[Math.Clamp(level, CockpitSpaceMin, CockpitSpaceMax)];

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

    /// <summary>What the payload file carries, read once per size and time: the audio part (the UTF-16 name HRTF_DEFAULT
    /// anywhere in the image) and the generic set (the RCDATA resource HRTF_DEFAULT with data).</summary>
    private static readonly ConcurrentDictionary<string, (long Length, DateTime Written, bool Audio, bool DefaultHrtf, bool CockpitSpace)> PayloadCache = new(StringComparer.OrdinalIgnoreCase);
    private static readonly byte[] EmbeddedMarker = Encoding.Unicode.GetBytes("HRTF_DEFAULT");
    /// <summary>The cockpit space key in binaural_config.h's table (wide strings, as the module reads the ini).</summary>
    private static readonly byte[] CockpitSpaceMarker = Encoding.Unicode.GetBytes("CockpitSpaceFamily");
    private static (bool Audio, bool DefaultHrtf, bool CockpitSpace) PayloadFacts(string? payloadPath)
    {
        if (payloadPath is null) return default;
        try
        {
            var info = new FileInfo(payloadPath);
            if (!info.Exists || info.Length > 256L * 1024 * 1024) return default;
            if (PayloadCache.TryGetValue(info.FullName, out var cached) && cached.Length == info.Length && cached.Written == info.LastWriteTimeUtc) return (cached.Audio, cached.DefaultHrtf, cached.CockpitSpace);
            var bytes = File.ReadAllBytes(info.FullName);
            var audio = bytes.AsSpan().IndexOf(EmbeddedMarker) >= 0;
            var facts = (Audio: audio, DefaultHrtf: PeResource.Find(bytes, PeResource.RcData, "HRTF_DEFAULT") is { Length: > 0 }, CockpitSpace: audio && bytes.AsSpan().IndexOf(CockpitSpaceMarker) >= 0);
            PayloadCache[info.FullName] = (info.Length, info.LastWriteTimeUtc, facts.Audio, facts.DefaultHrtf, facts.CockpitSpace);
            return facts;
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException) { return default; }
    }

    /// <summary>The installed payload's audio part has cockpit space (it reads [Audio] CockpitSpace*: the key name is in
    /// its key table). An earlier one ignores those keys, so Live mode must not report a change there as applied.</summary>
    public static bool PayloadHasCockpitSpace(string? payloadPath) => PayloadFacts(payloadPath).CockpitSpace;

    /// <summary>The payload embeds the generic HRTF set: an RCDATA resource named HRTF_DEFAULT with data in its PE resource
    /// directory (binaural_config.h kEmbeddedResourceName; native/dcsqvcull embeds native/binaural/data/sadie2_ku100.dcshrtf
    /// when the build has it). The name alone is also in the module's code, so this reads the resource table. Cached by
    /// size and time.</summary>
    public static bool PayloadHasDefaultHrtf(string? payloadPath) => PayloadFacts(payloadPath).DefaultHrtf;

    /// <summary>3D audio has a set to render with: the generic one in this build's engine module, or one of the pilot's own.
    /// A release always has the generic one (build-release.ps1 refuses to build without it); a -LocalOnly build without
    /// it and without a set of the pilot's own leaves 3D audio off at Launch DCS (the module would refuse it anyway).</summary>
    public static bool Usable(string? payloadPath, int personalSets) => personalSets > 0 || PayloadHasDefaultHrtf(payloadPath);

    /// <summary>The installed payload has the audio module: its generic HRTF is embedded as the resource HRTF_DEFAULT
    /// (docs/BINAURAL_AUDIO.md), whose name is in the PE resource directory as UTF-16. An earlier payload ignores
    /// [Audio], so Live mode must not report a change there as applied. Cached by size and time.</summary>
    public static bool PayloadHasAudio(string? payloadPath) => PayloadFacts(payloadPath).Audio;

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
    /// other features and 3D audio is off in the profile; while DCS runs and the module has said it followed that, it is
    /// switched off in the settings, live); null when not known (Last flight reads a past session).
    /// The module calls each of the pilot's sets "personal" (never by its name): <paramref name="deployedHrtf"/>, the
    /// deployed [Audio] Hrtf, names it; without it the status says "personal". Logs of earlier builds name the set.
    /// </summary>
    public static AudioStatus ParseStatus(IEnumerable<string>? moduleLines, bool installed, bool dcsRunning, bool? enabled = null, string? deployedHrtf = null)
    {
        if (!installed) return new("not-installed", "Launch DCS installs it when 3D headphone audio is on.");
        var lines = new List<string>();
        foreach (var line in moduleLines ?? [])
        {
            if (line.Contains("loader started", StringComparison.Ordinal)) { lines.Clear(); continue; }
            if (line.Contains(" audio: ", StringComparison.Ordinal) || line.StartsWith("audio: ", StringComparison.Ordinal)) lines.Add(line);
        }
        string? state = null, reason = null, hrtf = null, fallback = null, fallbackWhy = null; int? sounds = null, maxNow = null; double? cost = null; bool? pose = null; var backoffs = 0; var switchedOff = false; var started = false;
        var space = new CockpitSpaceStatus();
        foreach (var line in lines)
        {
            var text = line[(line.IndexOf("audio: ", StringComparison.Ordinal) + 7)..].Trim();
            if (text.StartsWith(CockpitSpacePrefix, StringComparison.Ordinal)) { space = space.With(text[CockpitSpacePrefix.Length..]); continue; }
            if (text.StartsWith("off (", StringComparison.Ordinal))
            {
                state = "off"; reason = Inside(text[3..]);
                if (NotFoundRegex().Match(reason) is { Success: true } nf) reason = $"this DCS version's sound engine is not the one it was made for ({nf.Groups[1].Value})";
                continue;
            }
            if (text.StartsWith("on (", StringComparison.Ordinal)) { state = "running"; reason = null; switchedOff = false; started = true; }
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
                space = space.WithRunning(text);
            }
            if (BudgetRegex().Match(text) is { Success: true } b)
            {
                maxNow = int.Parse(b.Groups[1].Value, CultureInfo.InvariantCulture);
                if (text.Contains("over budget", StringComparison.Ordinal)) backoffs++;
            }
        }
        var tail = lines.TakeLast(12).Select(l => l.Trim()).ToArray();
        hrtf = Named(hrtf); fallback = Named(fallback);
        var settingsOff = state == "off" && reason == SettingsOffReason;
        // Off in the deployed settings: off in this profile, unless DCS runs and the module said it switched off live.
        var reported = space.Level is null && space.EarlyOffs == 0 ? null : space;
        if (enabled == false && !(dcsRunning && settingsOff)) return new("disabled", "Off in this profile. DCS's own sound plays.", Lines: tail);
        if (state is null)
            return new("waiting", dcsRunning ? "Waiting for DCS's sound engine." : "Installed for the next DCS start.", Lines: tail) { CockpitSpace = reported };
        if (settingsOff)
            return new("switched-off", "Switched off in the settings. DCS's own sound plays.", null, Backoffs: backoffs, SwitchedOff: true, Hrtf: hrtf, Lines: tail) { SwitchedOffBy = "settings", Started = started, CockpitSpace = reported };
        if (state == "off") return new("off", $"Off: {reason}. DCS's own sound plays.", reason, Backoffs: backoffs, Lines: tail) { HrtfFallback = fallback, HrtfFallbackReason = fallbackWhy, Started = started, CockpitSpace = reported };
        var parts = new List<string> { switchedOff ? "Switched off in flight" : dcsRunning ? "Running" : "Ran at the last DCS start" };
        if (sounds is { } n) parts.Add(n == 1 ? "1 sound" : n + " sounds");
        if (cost is { } t) parts.Add(t.ToString("0.0#", CultureInfo.InvariantCulture) + " ms");
        return new(switchedOff ? "switched-off" : "running", string.Join(" · ", parts), null, sounds, cost, backoffs, maxNow, switchedOff, hrtf, pose, tail)
            { HrtfFallback = fallback, HrtfFallbackReason = fallbackWhy, SwitchedOffBy = switchedOff ? "key" : null, Started = started, CockpitSpace = reported };

        // The module's "personal": the set Launch DCS deployed (the ini's Hrtf) when that is one of the pilot's.
        string? Named(string? name) =>
            name == ModulePersonalSet && NormalizeHrtfName(deployedHrtf) is var d && d != DefaultHrtf && IsValidHrtfName(d) ? d : name;
        static string Inside(string text)
        {
            var open = text.IndexOf('('); var close = text.LastIndexOf(')');
            return open >= 0 && close > open ? text[(open + 1)..close].Trim() : text.Trim('(', ')', ' ');
        }
    }

    /// <summary>Cockpit space's log lines start with this after "audio: " (docs/COCKPIT_SPACE.md section 8.5).</summary>
    public const string CockpitSpacePrefix = "cockpit space: ";

    /// <summary>The module's line when its settings turn 3D audio off (native/dcsqvcull/src/main/audio.inc).</summary>
    public const string SettingsOffReason = "binaural is off in the settings";
    /// <summary>The module's log word for any of the pilot's own sets (audio.inc LogSetName): it never logs a set's name.</summary>
    public const string ModulePersonalSet = "personal";


    /// <summary>The status of 3D audio from the module's newest log in the Saved Games of <paramref name="optionsPath"/>,
    /// and from the deployed DcsQvCull.ini: installed for other features with [Audio] Binaural not 1, it is "disabled".</summary>
    public static AudioStatus ReadStatus(string optionsPath, bool dcsRunning)
    {
        var module = EngineOptimizations.ModuleDirectory(optionsPath);
        var installed = File.Exists(Path.Combine(module, "DcsQvCull.dll"));
        bool? enabled = null;
        string? deployedHrtf = null;
        if (installed)
        {
            // The module's own default is Binaural=0: no file or no [Audio] is off.
            try
            {
                var ini = Path.Combine(module, LiveSettings.EngineFile);
                var values = File.Exists(ini) ? IniFile.Parse(IniFile.Decode(File.ReadAllBytes(ini))) : null;
                enabled = values is not null && values.TryGetValue("Audio.Binaural", out var on) && on.Trim() == "1";
                if (values is not null && values.TryGetValue("Audio.Hrtf", out var set)) deployedHrtf = set.Trim();
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
        return ParseStatus(lines, installed, dcsRunning, enabled, deployedHrtf);
    }

    [GeneratedRegex(@"^([\w. -]+?) not found \(")] private static partial Regex NotFoundRegex();
    [GeneratedRegex(@"^(?:on \(.*?, )?HRTF ([\w .-]+?)(?=: \d+ directions| \(crossfading\))")] private static partial Regex HrtfRegex();
    // Not anchored at the end: cockpit space appends its own part to the running line.
    [GeneratedRegex(@"; head pose (on|off|unused)\b")] private static partial Regex PoseRegex();
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
    /// <summary>The module started binaural audio in this session (an "on (" line after the last "loader started").</summary>
    public bool Started { get; init; }
    /// <summary>Cockpit space as the module's log has it; null when it logged nothing about it (off, or a module without it).</summary>
    public CockpitSpaceStatus? CockpitSpace { get; init; }
}

/// <summary>
/// Cockpit space in the module's log (docs/COCKPIT_SPACE.md section 8.5):
///   audio: cockpit space: &lt;Off|Subtle|Normal|Strong|Test&gt; (&lt;family&gt; &lt;auto|setting&gt;, aircraft &lt;type|unknown&gt;)
///   audio: cockpit space: inside &lt;0|1&gt;, canopy &lt;0.00..1.00|unknown&gt; (&lt;mech|arg38|none&gt;)
///   audio: cockpit space: early reflections off (CPU budget) | early reflections on again
///   audio: running, ...; space &lt;level&gt; &lt;family&gt; inside &lt;0|1&gt; wet voices &lt;n&gt; early &lt;on|off&gt; room p99 &lt;ms&gt; ms, direct &lt;dB&gt; dB
/// </summary>
public sealed partial record CockpitSpaceStatus
{
    /// <summary>"Off", "Subtle", "Normal", "Strong", or "Test" while the listening test plays a level it does not name.</summary>
    public string? Level { get; init; }
    /// <summary>The family in use (cockpit_space.h id: bubble, framed, helicopter, warbird, transport, generic).</summary>
    public string? Family { get; init; }
    /// <summary>The family is the pilot's choice (Cockpit type), not picked from the aircraft.</summary>
    public bool FamilyFromSetting { get; init; }
    /// <summary>The aircraft's unit type as DCS reports it; null when unknown.</summary>
    public string? Aircraft { get; init; }
    /// <summary>The listener is in the cockpit (not an outside view); null when not said.</summary>
    public bool? Inside { get; init; }
    /// <summary>Canopy 0 closed .. 1 open; null when unknown.</summary>
    public double? Canopy { get; init; }
    /// <summary>Times the early reflections went off to stay within the CPU budget.</summary>
    public int EarlyOffs { get; init; }
    /// <summary>The early reflections are off now (CPU budget).</summary>
    public bool EarlyOff { get; init; }
    /// <summary>Sounds with the space at the last running line, and the room's p99 per block there.</summary>
    public int? WetVoices { get; init; }
    public double? RoomCostMs { get; init; }

    /// <summary>The page's line: "In the cockpit: F-16C_50, bubble canopy." Null when the aircraft is unknown or the space is off.</summary>
    public string? Summary => Aircraft is null || Level is null || Level == "Off" || Family is null ? null
        : $"In the cockpit: {Aircraft}, {Binaural.CockpitFamilyTitle(Family).ToLowerInvariant()}{(FamilyFromSetting ? " (your choice)" : "")}.";

    /// <summary>This status with one "cockpit space: " line (the text after the prefix) taken in.</summary>
    public CockpitSpaceStatus With(string text)
    {
        if (LevelRegex().Match(text) is { Success: true } m)
            return this with { Level = m.Groups[1].Value, Family = m.Groups[2].Value, FamilyFromSetting = m.Groups[3].Value == "setting", Aircraft = m.Groups[4].Value.Trim() is var a && a.Length > 0 && a != "unknown" ? a : null };
        if (StateRegex().Match(text) is { Success: true } s)
            return this with { Inside = s.Groups[1].Value == "1", Canopy = double.TryParse(s.Groups[2].Value, NumberStyles.Float, CultureInfo.InvariantCulture, out var c) && double.IsFinite(c) ? Math.Clamp(c, 0, 1) : null };
        if (text.StartsWith("early reflections off", StringComparison.Ordinal)) return this with { EarlyOffs = EarlyOffs + 1, EarlyOff = true };
        if (text.StartsWith("early reflections on", StringComparison.Ordinal)) return this with { EarlyOff = false };
        return this;
    }

    /// <summary>This status with the space part of a "running," line taken in (the text after "audio: ").</summary>
    public CockpitSpaceStatus WithRunning(string text) => RunningRegex().Match(text) is { Success: true } r
        ? this with
        {
            Level = r.Groups[1].Value, Family = r.Groups[2].Value, Inside = r.Groups[3].Value == "1", WetVoices = int.Parse(r.Groups[4].Value, CultureInfo.InvariantCulture),
            EarlyOff = r.Groups[5].Value == "off", RoomCostMs = double.Parse(r.Groups[6].Value, CultureInfo.InvariantCulture),
        }
        : this;

    [GeneratedRegex(@"^(Off|Subtle|Normal|Strong|Test) \((\w+) (auto|setting), aircraft (.*)\)\s*$")] private static partial Regex LevelRegex();
    [GeneratedRegex(@"^inside ([01]), canopy (\d+(?:\.\d+)?|unknown) \((?:mech|arg38|none)\)")] private static partial Regex StateRegex();
    [GeneratedRegex(@"\bspace (Off|Subtle|Normal|Strong) (\w+) inside ([01]) wet voices (\d+) early (on|off) room p99 (\d+(?:\.\d+)?) ms")] private static partial Regex RunningRegex();
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
