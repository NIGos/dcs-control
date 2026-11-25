using System.Diagnostics;
using System.Globalization;
using System.IO.Compression;
using System.Text;
using System.Text.RegularExpressions;
using Microsoft.Win32;

namespace DcsVr.Core;

/// <summary>Why DLSS 5 did or did not reach the Quad Views focus area in the last DCS session, from Cheeky's log.</summary>
/// <param name="State">applied, not-applied, no-dlss (DCS sent no DLSS frame), no-session (no Cheeky log for a session).</param>
/// <param name="Reason">The dominant rejection, as Cheeky names it ("no-proof/output-not-focus-size"), when nothing was accepted.</param>
/// <param name="DlssSize">The size DCS ran DLSS at in the rejections ("816x804").</param>
/// <param name="FocusSize">The focus views' size Quad Views published ("1520x1480").</param>
public sealed record DlssFocusReport(string State, string Summary, string? Explanation, string? Reason, int Accepted, int Rejected,
    string? DlssSize, string? FocusSize, string? Version, IReadOnlyList<string> Evidence);

/// <summary>An entry of the diagnostic package: where it came from and what it holds.</summary>
public sealed record DiagnosticEntry(string Name, string Source, long Bytes, bool Truncated);

/// <summary>Where the diagnostic package reads from. Null members are skipped.</summary>
public sealed record DiagnosticSources(string? StateRoot, string? ManagedProfile, string? OptionsPath, string? DcsExecutable, RuntimeKind Runtime,
    string? LocalAppData = null, string? RoamingAppData = null, string? SteamVrSettings = null);

/// <summary>The Diagnostics page: DLSS 5 focus diagnosis and the diagnostic package (a ZIP of the logs and settings
/// needed to understand a problem, with the Windows user and PC names removed).</summary>
public static partial class Diagnostics
{
    // ---- DLSS 5 on the focus area -------------------------------------------------------------------------------

    [GeneratedRegex(@"DCS quad focus: reject handle=0x[0-9A-F]+ count=\d+ reason=(?<reason>[\w-]+)(?:.*?size_route=(?<route>[\w-]+))?(?:.*?ngx=\(\d+,\d+ (?<ngx>\d+x\d+)\))?")] private static partial Regex RejectRegex();
    [GeneratedRegex(@"focusL=\(-?\d+,-?\d+ (?<size>\d+x\d+)")] private static partial Regex FocusSizeRegex();

    /// <summary>Reads the last session of CheekyFoveatedDLSS-Standalone.log in <paramref name="dcsBin"/>\CheekyFoveatedDLSS.</summary>
    public static DlssFocusReport DiagnoseDlssFocus(string? dcsBin)
    {
        var log = dcsBin is null ? null : Path.Combine(dcsBin, "CheekyFoveatedDLSS", "CheekyFoveatedDLSS-Standalone.log");
        if (log is null || !File.Exists(log))
            return new("no-session", "No DLSS 5 session found yet. It appears after DCS starts with DLSS 5 on.", null, null, 0, 0, null, null, null, []);
        List<string> lines;
        try { lines = Tail(log, 16L * 1024 * 1024).Split('\n').Select(l => l.TrimEnd('\r')).ToList(); }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException) { return new("no-session", "DLSS 5's log could not be read: " + e.Message, null, null, 0, 0, null, null, null, []); }
        return DiagnoseDlssFocus(lines);
    }

    /// <summary>The diagnosis from the log lines; only the lines after the last session start count.</summary>
    public static DlssFocusReport DiagnoseDlssFocus(IReadOnlyList<string> all)
    {
        var start = -1;
        for (var i = all.Count - 1; i >= 0; i--)
            if (all[i].Contains(" initializing; runtime remains resident", StringComparison.Ordinal)) { start = i; break; }
        var lines = all.Skip(start + 1).ToList();
        int accepted = 0, rejected = 0;
        var reasons = new Dictionary<string, int>();
        var dlssSizes = new Dictionary<string, int>();
        string? focusSize = null, version = null;
        var evidence = new List<string>();
        foreach (var line in lines)
        {
            if (LastFlight.NeuralInitRegex().Match(line) is { Success: true } init) version ??= init.Groups[1].Value;
            if (line.Contains("DCS quad focus: mapped focus accepted", StringComparison.Ordinal)) { accepted++; if (evidence.Count < 12) evidence.Add(line.Trim()); continue; }
            if (FocusSizeRegex().Match(line) is { Success: true } f) focusSize = f.Groups["size"].Value;
            if (RejectRegex().Match(line) is not { Success: true } r) continue;
            rejected++;
            var key = r.Groups["reason"].Value + (r.Groups["route"].Success && r.Groups["reason"].Value == "no-proof" ? "/" + r.Groups["route"].Value : "");
            reasons[key] = reasons.GetValueOrDefault(key) + 1;
            if (r.Groups["ngx"].Success) dlssSizes[r.Groups["ngx"].Value] = dlssSizes.GetValueOrDefault(r.Groups["ngx"].Value) + 1;
            if (evidence.Count < 12) evidence.Add(line.Trim());
        }
        var dlssSize = dlssSizes.OrderByDescending(p => p.Value).Select(p => p.Key).FirstOrDefault();
        if (accepted > 0)
            // Rejections of the peripheral views are normal: DLSS 5 works on the focus views only.
            return new("applied", $"Applied: DCS's DLSS on the focus views was identified{(version is null ? "" : $" (DLSS 5 runtime {version})")}.", rejected > 0 ? "The rejections in the log are DCS's DLSS on the peripheral views, which DLSS 5 leaves alone. That is expected." : null,
                null, accepted, rejected, dlssSize, focusSize, version, evidence);
        if (rejected == 0)
            return new("no-dlss", "Not applied: DCS sent no DLSS frame in the last session.", "DLSS 5 works on DCS's own DLSS. In DCS, set Upscaling (or DLSS) to DLSS in the graphics or VR settings, and make sure the mission was loaded in VR (the main menu has no DLSS frames).",
                null, 0, 0, null, focusSize, version, evidence);
        var reason = reasons.OrderByDescending(p => p.Value).First().Key;
        return new("not-applied", $"Not applied: the focus views were not identified ({reason}, {rejected} times).", Explain(reason, dlssSize, focusSize), reason, 0, rejected, dlssSize, focusSize, version, evidence);
    }

    /// <summary>What a Cheeky focus rejection means for the pilot.</summary>
    public static string Explain(string reason, string? dlssSize, string? focusSize) => reason switch
    {
        "snapshot-unavailable" => "Cheeky did not find the focus adapter's gaze layer. DLSS 5 on the focus area needs the bundled Quad Views with the focus adapter: check that the profile uses the bundled provider and that no other Quad Views layer (for example a separately installed Quad-Views-Foveated) is enabled.",
        "snapshot-not-ready" => "Quad Views did not publish the focus views in time. It is normal for a moment after loading; if it lasts, check that eye tracking runs and the headset is awake.",
        "ambiguous-match" => "More than one image matched the focus views, so neither was used. Send the diagnostic package.",
        "no-proof/output-not-focus-size" => $"DCS ran its DLSS only on images that are not the size of the focus views{(dlssSize is null ? "" : $" (DLSS on {dlssSize.Replace('x', '×')}")}{(focusSize is null ? (dlssSize is null ? "" : ")") : $", focus views {focusSize.Replace('x', '×')})")}. DLSS 5 needs DCS's DLSS on the focus views themselves. This happens when DCS renders without Quad Views (only two views), or when another Quad Views layer or the headset's own Quad Views shapes the views instead of the bundled one. Check that Quad Views is on in the profile and that no other Quad Views layer is installed.",
        "no-proof/handle-census" => "DCS used DLSS on more images than the four Quad Views views (for example also on a mirror or a second view), so the focus views could not be told apart. Turn off DLSS anywhere else DCS uses it, or send the diagnostic package.",
        "no-proof/layout-unavailable" or "no-proof/layout-stale" or "no-proof/layout-session-mismatch" => "The bundled Quad Views did not share its view layout with DLSS 5. Check that the profile uses the bundled Quad Views with the focus adapter and that no other Quad Views layer is enabled.",
        "no-proof/eye-identity-required" => "Foveated processing needs to know which eye each image is for, and DCS's images could not be matched to an eye. Send the diagnostic package.",
        "no-proof/focus-pair-size-mismatch" or "no-proof/peripheral-pair-size-mismatch" or "no-proof/peripheral-equals-focus-size" => "The Quad Views layout is unusual: the two eyes' views differ in size, or the focus and peripheral views have the same size, so they cannot be told apart. Lower the peripheral resolution or raise the focus resolution.",
        "no-proof/snapshot-layout-mismatch" => "The gaze snapshot and the Quad Views layout disagree. Send the diagnostic package.",
        _ => "Send the diagnostic package so the log can be read in full.",
    };

    // ---- Diagnostic package --------------------------------------------------------------------------------------

    private const long TextLimit = 8L * 1024 * 1024;
    /// <summary>A long log keeps its start too (the engine optimizations log their setup there, then grow every 10 s).</summary>
    private const long HeadLimit = 512L * 1024;
    private static readonly string[] TextExtensions = [".log", ".txt", ".json", ".ini", ".lua", ".cfg", ".csv", ".vrsettings", ".xml"];

    /// <summary>Writes the package to <paramref name="zipPath"/>: <paramref name="report"/> (the app's own report as
    /// JSON) plus logs and settings from the sources. Text only, each file cut to its last 8 MB, user and PC names removed.</summary>
    public static IReadOnlyList<DiagnosticEntry> CreatePackage(DiagnosticSources sources, string report, string zipPath, HeadsetRuntimeInfo? headset = null)
    {
        var entries = new List<DiagnosticEntry>();
        // Serial numbers are learnt from every file first, then removed everywhere: a serial is labelled in one log
        // (Pimax's, written last) and bare in others.
        var serials = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        var pilot = Redactor(Environment.UserName, Environment.GetFolderPath(Environment.SpecialFolder.UserProfile), Environment.MachineName, serials);
        // The pilot's own HRTF sets are named by the pilot (often after a person): every name becomes "personal set N".
        var savedGamesForNames = sources.OptionsPath is { } namesOptions ? Path.GetDirectoryName(Path.GetDirectoryName(Path.GetFullPath(namesOptions))) : null;
        // Names the redaction patterns find in one file (an older log's "Hrtf=x", a removed set's path) go from every file.
        var learnedNames = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        var hrtfNames = sources.StateRoot is { } namesState ? HrtfNameRedactor(namesState, savedGamesForNames, learnedNames) : (text => text);
        Func<string, string> redactor = text => hrtfNames(pilot(text));
        var temp = zipPath + ".tmp";
        var learning = true;
        var scanned = new HashSet<string>(StringComparer.Ordinal);
        ZipArchive? zip = null;
        try
        {
            void Text(string name, string source, string content, bool truncated = false)
            {
                if (learning) { LearnSerials(content, serials); LearnHrtfNames(content, learnedNames); return; }
                var bytes = Encoding.UTF8.GetBytes(redactor(content));
                var entry = zip!.CreateEntry(name, CompressionLevel.Optimal);
                using (var output = entry.Open()) output.Write(bytes);
                entries.Add(new(name, redactor(source), bytes.Length, truncated));
            }
            void File_(string name, string? path, long head = HeadLimit, long tail = TextLimit)
            {
                if (path is null || !File.Exists(path)) return;
                if (learning ? !scanned.Add(name) : entries.Any(e => e.Name == name)) return;
                // The package stays small enough to attach: once 150 MB of text is in, the remaining (older) files are listed only.
                if (!learning && entries.Sum(e => e.Bytes) > 150L * 1024 * 1024) { entries.Add(new(name + " (left out: package size limit)", redactor(path), 0, true)); return; }
                try
                {
                    var length = new FileInfo(path).Length;
                    Text(name, path, HeadAndTail(path, head, tail), length > head + tail);
                }
                catch (Exception e) when (e is IOException or UnauthorizedAccessException) { Text(name + ".unreadable.txt", path, e.Message); }
            }
            void Folder(string prefix, string? folder, string pattern = "*", int newest = 20, SearchOption option = SearchOption.TopDirectoryOnly, string? except = null)
            {
                if (folder is null || !Directory.Exists(folder)) return;
                IEnumerable<FileInfo> files;
                try { files = new DirectoryInfo(folder).EnumerateFiles(pattern, option).Where(f => TextExtensions.Contains(f.Extension, StringComparer.OrdinalIgnoreCase) && !string.Equals(f.Name, except, StringComparison.OrdinalIgnoreCase)).OrderByDescending(f => f.LastWriteTimeUtc).Take(newest).ToList(); }
                catch (Exception e) when (e is IOException or UnauthorizedAccessException) { return; }
                foreach (var file in files) File_(prefix + "/" + Path.GetRelativePath(folder, file.FullName).Replace('\\', '/'), file.FullName);
            }

            // Two passes over the same sources: the first only learns serial numbers, the second writes the package.
            Collect();
            learning = false;
            using (var stream = new FileStream(temp, FileMode.Create, FileAccess.Write))
            using (zip = new ZipArchive(stream, ZipArchiveMode.Create))
                Collect();
            File.Move(temp, zipPath, true);

            void Collect()
            {
            Text("report.json", "DCS Control", report);
            var local = sources.LocalAppData ?? Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData);
            var roaming = sources.RoamingAppData ?? Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData);
            var savedGamesRoot = sources.OptionsPath is { } o ? Path.GetDirectoryName(Path.GetDirectoryName(Path.GetFullPath(o))) : null;
            Text("summary.txt", "DCS Control", Summary(headset, ToggleLines(
            [
                ("DcsQvCull.prev.log", savedGamesRoot is null ? null : Path.Combine(savedGamesRoot, "Scripts", "DcsQvCull", "DcsQvCull.prev.log")),
                ("DcsQvCull.log", savedGamesRoot is null ? null : Path.Combine(savedGamesRoot, "Scripts", "DcsQvCull", "DcsQvCull.log")),
                ("PupilShift.log", sources.ManagedProfile is null ? null : Path.Combine(sources.ManagedProfile, PupilShiftStatus.FolderName, PupilShiftStatus.LogName)),
                ("Quad-Views-Foveated.log", Path.Combine(local, "Quad-Views-Foveated", "Quad-Views-Foveated.log")),
            ])));

            // DCS Control: the applied profile's deployed settings and component logs, and the app's own state.
            Folder("dcs-control/applied-profile", sources.ManagedProfile, "*", 60, SearchOption.AllDirectories);
            if (sources.StateRoot is { } state)
            {
                foreach (var name in new[] { "draft.json", "app.log" }) File_("dcs-control/" + name, Path.Combine(state, name));
                // Never the record of programs Free VRAM closed: it holds their full command lines.
                Folder("dcs-control/boost", Path.Combine(state, "boost"), except: "closed.json");
                // What the app changed and how restoring went; never the backed-up files themselves.
                File_("dcs-control/originals/baseline.json", Path.Combine(state, "originals", "baseline.json"));
                File_("dcs-control/originals/originals.log", Path.Combine(state, "originals", "originals.log"));
            }

            // The Quad Views layer's own log: whether it ran or stepped aside ("layer will be bypassed"), and why (GitHub #20).
            File_("quad-views/Quad-Views-Foveated.log", Path.Combine(local, "Quad-Views-Foveated", "Quad-Views-Foveated.log"));
            // DCS: its log, settings, the engine optimizations' log and reports, the hooks it loads.
            if (sources.OptionsPath is { } options && Path.GetDirectoryName(Path.GetDirectoryName(Path.GetFullPath(options))) is { } savedGames)
            {
                File_("dcs/Logs/dcs.log", Path.Combine(savedGames, "Logs", "dcs.log"));
                File_("dcs/Logs/dcs.log.old", Path.Combine(savedGames, "Logs", "dcs.log.old"));
                File_("dcs/Config/options.lua", options);
                File_("dcs/Config/autoexec.cfg", Path.Combine(savedGames, "Config", "autoexec.cfg"));
                // The module's log of the session before the last DCS start (kept by it at each start), as for dcs.log.old.
                foreach (var name in new[] { "DcsQvCull.log", "DcsQvCull.prev.log", "DcsQvCullPrefetch.log", "DcsQvCullPrefetch.prev.log" })
                    File_("dcs/Scripts/DcsQvCull/" + name, Path.Combine(savedGames, "Scripts", "DcsQvCull", name));
                // A second DCS started while the first held the log writes DcsQvCull.<pid>.log.
                Folder("dcs/Scripts/DcsQvCull", Path.Combine(savedGames, "Scripts", "DcsQvCull"), "DcsQvCull.*.log", 4);
                Folder("dcs/Scripts/DcsQvCull", Path.Combine(savedGames, "Scripts", "DcsQvCull"), "*", 10);
                var hooks = Path.Combine(savedGames, "Scripts", "Hooks");
                try { if (Directory.Exists(hooks)) Text("dcs/Scripts/Hooks.txt", hooks, string.Join("\n", Directory.EnumerateFiles(hooks).Select(Path.GetFileName))); }
                catch (Exception e) when (e is IOException or UnauthorizedAccessException) { }
            }
            // 3D headphone audio: the HRTF sets in the library (names and facts; never a SOFA file or where it was) and the
            // module's audio lines of the last DCS start. The deployed .dcshrtf files are binary and stay out.
            if (sources.StateRoot is { } audioState) Text("dcs-control/audio.txt", "DCS Control", learning ? "" : AudioSummary(audioState, savedGamesRoot));
            if (sources.DcsExecutable is { } exe && Path.GetDirectoryName(exe) is { } bin)
            {
                Folder("dcs/bin/CheekyFoveatedDLSS", Path.Combine(bin, "CheekyFoveatedDLSS"));
                File_("dcs/bin/CheekyFoveatedDLSS-Loader.log", Path.Combine(bin, "CheekyFoveatedDLSS-Loader.log"));
                Text("dcs/bin/modules.txt", bin, learning ? "" : BinaryVersions(bin));
            }
            // Cheeky's crash log in DCS's %TEMP% (faults it contained, open item 7) and the copy it rolls over at 1 MiB:
            // a fault storm repeats one line thousands of times, so only its start and last 1 MB.
            var tempDir = sources.LocalAppData is { } fixedLocal ? Path.Combine(fixedLocal, "Temp") : Path.GetTempPath();
            foreach (var name in new[] { "CheekyFoveatedDLSS_crash.log", "CheekyFoveatedDLSS_crash.prev.log" })
                File_("temp/" + name, Path.Combine(tempDir, name), 64 * 1024, 1024 * 1024);

            // Headset software.
            if (sources.Runtime == RuntimeKind.SboysSteamVr)
            {
                File_("steamvr/steamvr.vrsettings", sources.SteamVrSettings);
                if (sources.SteamVrSettings is { } settings && Path.GetDirectoryName(Path.GetDirectoryName(settings)) is { } steam)
                    foreach (var name in new[] { "vrserver.txt", "vrcompositor.txt", "vrclient_DCS.txt" }) File_("steamvr/logs/" + name, Path.Combine(steam, "logs", name));
            }
            // Pimax's logs, except on the Quest 3 VD route: there they come from other sessions (a Pimax used another day)
            // and would mislead. Virtual Desktop's own logs are not collected: their location is not known here.
            if (sources.Runtime != RuntimeKind.VirtualDesktop)
            {
                Folder("pimax/runtime", Path.Combine(local, "Pimax", "runtime"), "pvr_srv_log_*", 1);
                File_("pimax/runtime/pvr_client_DCS_log.txt", Path.Combine(local, "Pimax", "runtime", "pvr_client_DCS_log.txt"));
                File_("pimax/runtime/profile.json", Path.Combine(local, "Pimax", "runtime", "profile.json"));
                Folder("pimax/PiService", Path.Combine(local, "Pimax", "PiService", "Log"), "PiService__*", 2);
                File_("pimax/AppConfig/global.json", Path.Combine(roaming, "Pimax", "AppConfig", "global.json"));
            }

            if (learning) return;
            Text("openxr.txt", "Windows registry", OpenXrRegistry());
            Text("system.txt", "Windows", SystemSummary());
            Text("README.txt", "DCS Control", Readme(entries));
            }
        }
        catch
        {
            // No half-written package next to the one the pilot asked for.
            try { File.Delete(temp); } catch (Exception e) when (e is IOException or UnauthorizedAccessException) { }
            throw;
        }
        return entries;
    }

    [GeneratedRegex(@"^(?<time>\d\d:\d\d:\d\d\.\d{3}) (?<what>(?:hotkey|toggle): (?!switch took).*?)\s*$")] private static partial Regex ToggleRegex();

    /// <summary>The in-flight switches in the logs, in order: the engine kill switch with the module's "the 10 s
    /// before / after" performance lines, the other engine keys, Pupil shift's keys and Quad Views' Far depth key.
    /// At most 200 lines, the newest kept.</summary>
    internal static IReadOnlyList<string> ToggleLines(IEnumerable<(string Name, string? Path)> logs)
    {
        var lines = new List<string>();
        foreach (var (name, path) in logs)
        {
            if (path is null || !File.Exists(path)) continue;
            string text;
            try { text = Tail(path, TextLimit); }
            catch (Exception e) when (e is IOException or UnauthorizedAccessException) { continue; }
            foreach (var raw in text.Split('\n'))
            {
                var line = raw.TrimEnd('\r');
                if (ToggleRegex().Match(line) is { Success: true } m) lines.Add($"[{name}] {m.Groups["time"].Value} {m.Groups["what"].Value}");
                else if (line.Contains("DCSVR Far depth: switched", StringComparison.Ordinal)) lines.Add($"[{name}] {line.Trim()}");
            }
        }
        return lines.Count > 200 ? lines[^200..] : lines;
    }

    /// <summary>summary.txt: the headset software and the in-flight switches, so a reader starts there.</summary>
    internal static string Summary(HeadsetRuntimeInfo? headset, IReadOnlyList<string> toggles) =>
        $"DCS Control {ProductInfo.Version} diagnostic summary\n"
        + (headset?.Line ?? "Headset/runtime: not read") + "\n\n"
        + "In-flight switches (DcsQvCull.prev.log is the DCS session before DcsQvCull.log):\n"
        + (toggles.Count == 0 ? "  none in these logs\n" : string.Concat(toggles.Select(t => "  " + t + "\n")));

    /// <summary>The package's audio.txt: the HRTF library (each set as "personal set N", never its name) and the "audio:"
    /// lines of the module's newest log.</summary>
    internal static string AudioSummary(string stateRoot, string? savedGamesRoot)
    {
        var sets = new HrtfLibrary(stateRoot).List();
        var text = new StringBuilder($"3D headphone audio\n\nHRTF sets in the library: {sets.Count} of the pilot's own (named \"personal set 1\" and so on here; the generic KU100 set is inside the engine module):\n");
        if (sets.Count == 0) text.Append("  none\n");
        for (var i = 0; i < sets.Count; i++)
        {
            var s = sets[i];
            text.Append(CultureInfo.InvariantCulture, $"  personal set {i + 1}: {s.Directions} directions, {s.SampleRate} Hz, {s.Taps} taps, head radius {s.HeadRadiusMm:0.0} mm, {s.Bytes} bytes{(s.Imported is { } at ? ", imported " + at.ToString("yyyy-MM-dd", CultureInfo.InvariantCulture) : "")}{(s.Warnings.Count > 0 ? "; " + string.Join(" ", s.Warnings) : "")}\n");
        }
        text.Append("\naudio: lines of the module's newest log (DcsQvCull.log):\n");
        if (savedGamesRoot is not null)
        {
            var log = EngineOptimizations.ModuleLog(Path.Combine(savedGamesRoot, "Scripts", EngineOptimizations.ModuleFolder));
            try
            {
                var lines = File.Exists(log) ? LastFlight.ReadLines(log, 8L * 1024 * 1024, l => l.Contains("audio: ", StringComparison.Ordinal) || l.Contains("loader started", StringComparison.Ordinal)) : [];
                var start = lines.FindLastIndex(l => l.Contains("loader started", StringComparison.Ordinal));
                var audio = lines.Skip(start + 1).ToList();
                foreach (var line in audio.Take(40).Concat(audio.Count > 80 ? ["  …"] : []).Concat(audio.Skip(Math.Max(40, audio.Count - 40)))) text.Append("  ").Append(line.Trim()).Append('\n');
                if (audio.Count == 0) text.Append("  none\n");
            }
            catch (Exception e) when (e is IOException or UnauthorizedAccessException) { text.Append("  unreadable: ").Append(e.Message).Append('\n'); }
        }
        return HrtfNameRedactor(stateRoot, savedGamesRoot)(text.ToString());
    }

    /// <summary>Replaces the names of the pilot's HRTF sets (the library's file names and the titles inside them, and the
    /// sets deployed next to the module) with "personal set N" (N: the set's place in the library, by name), and any other
    /// set name where a log, DcsQvCull.ini or a profile names one ("HRTF x: ...", "Hrtf=x", "binauralHrtf": "x", HRTF set
    /// "x", "HRTF imported: x (", hrtf\x.dcshrtf) with "personal set". "default" (the generic set) and "personal" (the
    /// module's own word for any of the pilot's sets) stay. The patterns that name a set run first; then the library's
    /// names, and the names the patterns found (<paramref name="learned"/>, filled from every file of a package first), are
    /// replaced as whole words elsewhere, except names under three characters and generic ones ("HRTF", "SOFA file",
    /// "Subject 3": every word a common term or a number), which would rewrite ordinary text.</summary>
    internal static Func<string, string> HrtfNameRedactor(string stateRoot, string? savedGamesRoot, HashSet<string>? learned = null)
    {
        var labels = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
        var sets = new HrtfLibrary(stateRoot).List();
        for (var i = 0; i < sets.Count; i++)
            foreach (var name in new[] { sets[i].Name, sets[i].Title }) if (name.Trim().Length > 0) labels.TryAdd(name.Trim(), "personal set " + (i + 1));
        if (savedGamesRoot is not null)
        {
            try
            {
                var deployed = Path.Combine(savedGamesRoot, "Scripts", EngineOptimizations.ModuleFolder, "hrtf");
                if (Directory.Exists(deployed))
                    foreach (var file in Directory.EnumerateFiles(deployed, "*" + Binaural.HrtfExtension)) labels.TryAdd(Path.GetFileNameWithoutExtension(file), "personal set");
            }
            catch (Exception e) when (e is IOException or UnauthorizedAccessException) { }
        }
        foreach (var kept in labels.Keys.Where(KeptHrtfName).ToArray()) labels.Remove(kept);
        var found = learned ?? new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        string Label(string name)
        {
            var key = name.Trim();
            if (KeptHrtfName(key)) return name;
            found.Add(key);
            return labels.TryGetValue(key, out var l) ? l : "personal set";
        }
        var cacheCount = -1;
        (Regex Regex, string Label)[] cache = [];
        (Regex Regex, string Label)[] Words()
        {
            if (cacheCount == found.Count) return cache;
            var all = new Dictionary<string, string>(labels, StringComparer.OrdinalIgnoreCase);
            foreach (var name in found) all.TryAdd(name, "personal set");
            cache = all.Where(p => p.Key.Length >= 3 && !KeptHrtfName(p.Key) && !GenericHrtfName(p.Key)).OrderByDescending(p => p.Key.Length)
                .Select(p => (new Regex(@"(?<![\w])" + Regex.Escape(p.Key) + @"(?![\w])", RegexOptions.IgnoreCase | RegexOptions.CultureInvariant), p.Value)).ToArray();
            cacheCount = found.Count;
            return cache;
        }
        return text =>
        {
            if (string.IsNullOrEmpty(text)) return text;
            // Where a set is named first (the free-text pass would otherwise rewrite part of a name these patterns need).
            text = HrtfLogNameRegex().Replace(text, m => m.Groups[1].Value + Label(m.Groups[2].Value));
            text = HrtfIniNameRegex().Replace(text, m => m.Groups[1].Value + Label(m.Groups[2].Value));
            text = HrtfJsonNameRegex().Replace(text, m => m.Groups[1].Value + Label(m.Groups[2].Value) + "\"");
            text = HrtfQuotedNameRegex().Replace(text, m => m.Groups[1].Value + Label(m.Groups[2].Value) + m.Groups[3].Value);
            text = HrtfImportedRegex().Replace(text, m => m.Groups[1].Value + Label(m.Groups[2].Value));
            text = HrtfPathRegex().Replace(text, m => m.Groups[1].Value + Label(m.Groups[2].Value) + m.Groups[3].Value);
            foreach (var (regex, label) in Words()) text = regex.Replace(text, label);
            return text;
        };
    }

    /// <summary>Collects the set names the redaction patterns find in <paramref name="text"/> (the package's first pass,
    /// like <see cref="LearnSerials"/>): a name seen where a pattern names it in one file goes from every file.</summary>
    internal static void LearnHrtfNames(string text, HashSet<string> names)
    {
        if (string.IsNullOrEmpty(text)) return;
        foreach (var regex in new[] { HrtfLogNameRegex(), HrtfIniNameRegex(), HrtfJsonNameRegex(), HrtfQuotedNameRegex(), HrtfImportedRegex(), HrtfPathRegex() })
            foreach (Match m in regex.Matches(text))
                if (m.Groups[2].Value.Trim() is { Length: > 0 } name && !KeptHrtfName(name)) names.Add(name);
    }

    /// <summary>The module's log calls each of the pilot's sets "personal", never by its name.</summary>
    internal const string ModulePersonalWord = "personal";
    private static bool KeptHrtfName(string name) =>
        name.Length == 0 || name.Equals(Binaural.DefaultHrtf, StringComparison.OrdinalIgnoreCase) || name.Equals(ModulePersonalWord, StringComparison.OrdinalIgnoreCase)
        || name.StartsWith("personal set", StringComparison.Ordinal);
    // Words a set's title often is (SOFA titles such as "HRTF", "SOFA file", "KEMAR subject 3"): a name made only of these
    // and numbers is replaced only where a pattern names it as a set, never as a free word.
    private static readonly HashSet<string> GenericHrtfWords = new(StringComparer.OrdinalIgnoreCase)
    {
        "hrtf", "hrtfs", "hrir", "hrirs", "sofa", "file", "files", "set", "sets", "audio", "binaural", "default", "generic", "personal",
        "kemar", "ku100", "neumann", "sadie", "ii", "dummy", "head", "measurement", "measurements", "measured", "subject", "database",
        "data", "dataset", "simplefreefieldhrir", "freefield", "free", "field", "test", "the", "of", "and", "a",
    };
    private static bool GenericHrtfName(string name) =>
        Regex.Split(name, @"[^\p{L}\p{N}]+").Where(w => w.Length > 0).All(w => GenericHrtfWords.Contains(w) || w.All(char.IsAsciiDigit));
    // "HRTF My ears: 2562 directions", "HRTF My ears (crossfading)", "HRTF My ears not usable (" (the module's log).
    [GeneratedRegex(@"(\bHRTF )([\w .-]+?)(?=: \d+ directions| \(crossfading\)| not usable \()")] private static partial Regex HrtfLogNameRegex();
    // DcsQvCull.ini "Hrtf=My ears" (to the line's end or a comment), and the module's config line of earlier builds,
    // "audio: config: Binaural=1 Hrtf=My ears HeadCircumferenceCm=57.0 ...", anywhere in a line.
    [GeneratedRegex(@"(?m)(\bHrtf\s*=\s*)([^\r\n;=]*?)(?=\s+HeadCircumferenceCm=|\s*;|\s*\r?$)")] private static partial Regex HrtfIniNameRegex();
    // A profile's JSON: "binauralHrtf": "My ears".
    [GeneratedRegex(@"(""binauralHrtf""\s*:\s*"")((?:[^""\\]|\\.)*)""")] private static partial Regex HrtfJsonNameRegex();
    // Checks and messages: HRTF set "My ears" (also JSON-escaped: HRTF set \"My ears\").
    [GeneratedRegex(@"(\bHRTF (?:set )?(?:\\)?"")([^""\\]*)((?:\\)?"")")] private static partial Regex HrtfQuotedNameRegex();
    // app.log of earlier builds: "HRTF imported: My ears (2562 directions, 48000 Hz)".
    [GeneratedRegex(@"(\bHRTF imported: )([^\r\n(]+?)(?= \(\d+ directions)")] private static partial Regex HrtfImportedRegex();
    // A set's file, in the library or next to the module: hrtf\My ears.dcshrtf (also / and JSON's \\).
    [GeneratedRegex(@"(\bhrtf(?:\\\\|[\\/]))([^\\/:*?""<>|\r\n]+?)(\.dcshrtf\b)", RegexOptions.IgnoreCase)] private static partial Regex HrtfPathRegex();

    private static string Readme(IReadOnlyList<DiagnosticEntry> entries) =>
        $"DCS Control {ProductInfo.Version} diagnostic package, {DateTimeOffset.Now:yyyy-MM-dd HH:mm zzz}.\n"
        + "Logs and settings of DCS, DCS Control's components and the headset software. The Windows user name, PC name and e-mail addresses are replaced by <user>, <pc> and <e-mail>; account tokens, account ids, Steam IDs and the headset's serial number by <token>, <id>, <steam id> and <serial>; network and MAC addresses by <ip> and <mac>; the names of your own HRTF sets by \"personal set 1\" and so on.\n"
        + "Start with summary.txt: the headset software and the in-flight switches of these logs.\n"
        + "Large logs are cut to their last 8 MB (marked below).\n\n"
        + string.Join("\n", entries.Select(e => string.Create(CultureInfo.InvariantCulture, $"{e.Name}  ({e.Bytes:N0} bytes{(e.Truncated ? ", last part" : "")})  from {e.Source}")));

    /// <summary>Writes a report the pilot may attach to an issue (Diagnostic report, the CLI's <c>diagnostic</c>) with the
    /// same names removed as in the diagnostic package.</summary>
    public static void WriteReport(string path, string text) => AtomicFile.WriteText(path, Redactor()(text));

    /// <summary>Replaces the Windows user name (also in the user's profile folder), the PC name and e-mail addresses.</summary>
    internal static Func<string, string> Redactor() => Redactor(Environment.UserName, Environment.GetFolderPath(Environment.SpecialFolder.UserProfile), Environment.MachineName);

    /// <summary>Removes what identifies the pilot: the user's profile folder in every spelling a log or JSON uses (\, \\,
    /// /), the user and profile folder names, the PC name, e-mail addresses, and what Pimax's service logs carry
    /// (session tokens, account ids, the headset's serial number).</summary>
    internal static Func<string, string> Redactor(string user, string profile, string machine, HashSet<string>? knownSerials = null)
    {
        var folder = Path.GetFileName(profile.TrimEnd('\\', '/'));
        var names = new[] { user, folder }.Where(n => n.Length >= 3).Distinct(StringComparer.OrdinalIgnoreCase)
            .SelectMany(n => new[] { n, System.Text.Json.JsonEncodedText.Encode(n).ToString() }).Distinct(StringComparer.Ordinal).ToArray();
        var serials = knownSerials ?? new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        var paths = profile.Length > 3 ?new[] { (profile, @"C:\Users\<user>"), (profile.Replace("\\", "\\\\"), @"C:\\Users\\<user>"), (profile.Replace('\\', '/'), "C:/Users/<user>") } : [];
        return text =>
        {
            if (string.IsNullOrEmpty(text)) return text;
            foreach (var (from, to) in paths) text = text.Replace(from, to, StringComparison.OrdinalIgnoreCase);
            // The PC name goes before the user names: a PC named after its user ("JOHN-PC") would otherwise become "<user>-PC".
            if (machine.Length >= 3) text = Regex.Replace(text, @"(?<![\w-])" + Regex.Escape(machine) + @"(?![\w-])", "<pc>", RegexOptions.IgnoreCase);
            // Short names are removed where they are a profile folder, anywhere else only from three letters up.
            if (folder.Length is > 0 and < 3) text = Regex.Replace(text, @"(?<=Users[\\/]{1,2})" + Regex.Escape(folder) + @"(?=[\\/""]|$)", "<user>", RegexOptions.IgnoreCase);
            foreach (var name in names) text = Regex.Replace(text, @"(?<![\w])" + Regex.Escape(name) + @"(?![\w])", "<user>", RegexOptions.IgnoreCase);
            // An 8.3 short name of the profile folder ("C:\Users\GIUSEP~1") starts with the name too.
            text = ShortProfileRegex().Replace(text, "<user>");
            text = TokenRegex().Replace(text, "${key}${sep}<token>");
            text = AccountRegex().Replace(text, "${key}${sep}<id>");
            // A serial number seen once (next to its label) is removed wherever else it appears, also in later files.
            // Next to its label always; elsewhere also glued to letters (Pimax writes "<serial>platMap", and a serial that
            // starts or ends with a letter may touch a word), but never inside a longer number: a digit edge must not touch
            // another digit. A learnt serial always holds a digit, so no plain word can contain it.
            LearnSerials(text, serials);
            text = SerialRegex().Replace(text, "${key}${sep}<serial>");
            text = SteamVrSerialRegex().Replace(text, m => m.Value[..(m.Groups["serial"].Index - m.Index)] + "<serial>");
            foreach (var serial in serials.OrderByDescending(s => s.Length))
                text = Regex.Replace(text, SerialEdge(serial[0], before: true) + Regex.Escape(serial) + SerialEdge(serial[^1], before: false), "<serial>", RegexOptions.IgnoreCase);
            // Steam IDs: what Steam's API prints into DCS's output ("Caching Steam ID: 7656119…"), in both spellings.
            text = SteamId64Regex().Replace(text, "<steam id>");
            text = SteamId3Regex().Replace(text, "[U:1:<id>]");
            // Network addresses: MAC addresses, and IPv4 addresses where they are one (never a version such as 2.9.31.12345).
            text = MacRegex().Replace(text, "<mac>");
            text = MacLabelledRegex().Replace(text, "${key}${sep}<mac>");
            text = Ipv4Regex().Replace(text, m => IsAddress(text, m) ? "<ip>" : m.Value);
            return EmailRegex().Replace(text, "<e-mail>");
        };
    }

    /// <summary>Adds the headset serial numbers <paramref name="text"/> names to <paramref name="serials"/>: next to a
    /// serial label (Pimax), as SteamVR's active HMD ("Active HMD set to lighthouse.LHR-…") and as its
    /// Prop_SerialNumber_String property. Only values with a digit, so a word ("Null Serial Number") is never learnt, and
    /// only ones that look like a serial (8 characters or more, not one repeated character): a placeholder such as
    /// "000000" would otherwise be removed from every number in every file. Shorter ones are still removed at their label.</summary>
    internal static void LearnSerials(string text, ISet<string> serials)
    {
        if (string.IsNullOrEmpty(text)) return;
        foreach (Match m in SerialRegex().Matches(text)) if (LooksLikeSerial(m.Groups["serial"].Value)) serials.Add(m.Groups["serial"].Value);
        foreach (Match m in SteamVrSerialRegex().Matches(text)) if (LooksLikeSerial(m.Groups["serial"].Value)) serials.Add(m.Groups["serial"].Value);
    }
    private static string SerialEdge(char edge, bool before) =>
        !char.IsAsciiDigit(edge) ? "" : before ? "(?<![0-9])" : "(?![0-9])";
    private static bool LooksLikeSerial(string value) =>
        value.Length >= 8 && value.Where(char.IsAsciiLetterOrDigit).Select(char.ToUpperInvariant).Distinct().Count() > 1;

    /// <summary>Whether a dotted quad is a network address rather than a file or DCS version ("3.7.10.0" in
    /// modules.txt, "2.9.31.12345"): one with a port, one after an address word ("ip", "host", "connect to"…), or one in
    /// a home or local network range (10.x, 172.16-31.x, 192.168.x, 169.254.x, 100.64-127.x) not after "version".
    /// Loopback and 0.0.0.0 stay: they are no one's address.</summary>
    private static bool IsAddress(string text, Match m)
    {
        var o = m.Groups["o"].Captures.Select(c => int.Parse(c.Value, CultureInfo.InvariantCulture)).ToArray();
        if (o[0] is 127 or 0 or 255) return false;
        var after = m.Index + m.Length;
        if (after + 1 < text.Length && text[after] == ':' && char.IsAsciiDigit(text[after + 1])) return true;
        var before = text[Math.Max(0, m.Index - 24)..m.Index];
        if (AddressWordRegex().IsMatch(before)) return true;
        var local = o[0] == 10 || (o[0] == 172 && o[1] is >= 16 and <= 31) || (o[0] == 192 && o[1] == 168) || (o[0] == 169 && o[1] == 254) || (o[0] == 100 && o[1] is >= 64 and <= 127);
        return local && !VersionWordRegex().IsMatch(before);
    }
    [GeneratedRegex(@"(?<!\d)7656119\d{10}(?!\d)")] private static partial Regex SteamId64Regex();
    [GeneratedRegex(@"\[U:1:\d+\]")] private static partial Regex SteamId3Regex();
    [GeneratedRegex(@"[\w.+-]+@[\w-]+(\.[\w-]+)+")] private static partial Regex EmailRegex();
    [GeneratedRegex(@"(?<=Users[\\/]{1,2})[^\\/\s]{1,6}~\d+", RegexOptions.IgnoreCase)] private static partial Regex ShortProfileRegex();
    [GeneratedRegex(@"(?<key>\b(?:access_?token|refresh_?token|token|authorization|bearer)\b)(?<sep>[""']?\s*[:=]\s*[""']?\s*)(?!<token>)[^\s,""'}]+", RegexOptions.IgnoreCase)] private static partial Regex TokenRegex();
    [GeneratedRegex(@"(?<key>\b(?:user_?id|account_?id)\b)(?<sep>[""']?\s*[:=]\s*[""']?)\w+", RegexOptions.IgnoreCase)] private static partial Regex AccountRegex();
    [GeneratedRegex(@"(?<key>\b(?:serial(?:[ _]?number)?|hmd_?sn|device_?sn|sn)\b)(?<sep>[""']?\s*[:=]\s*[""']?)(?<serial>(?=[A-Z0-9-]*\d)[A-Z0-9][A-Z0-9-]{5,})", RegexOptions.IgnoreCase)] private static partial Regex SerialRegex();
    [GeneratedRegex(@"(?:\bActive HMD set to [\w-]+\.|\bProp_SerialNumber_String[\s""':=]{1,40})(?<serial>(?=[\w-]*\d)[A-Za-z0-9][\w-]{5,})", RegexOptions.IgnoreCase)] private static partial Regex SteamVrSerialRegex();
    [GeneratedRegex(@"(?<![\w.])(?:(?<o>25[0-5]|2[0-4]\d|1\d\d|[1-9]?\d)\.){3}(?<o>25[0-5]|2[0-4]\d|1\d\d|[1-9]?\d)(?!\w|\.\d)")] private static partial Regex Ipv4Regex();
    [GeneratedRegex(@"(?:\b(?:ip|ipv4|addr|address|host|hostname|server|client|peer|remote|gateway|dns|connect(?:ed|ing)?\s+to)|@)\W{0,4}$", RegexOptions.IgnoreCase)] private static partial Regex AddressWordRegex();
    [GeneratedRegex(@"\b(?:version|ver|build)\W{0,3}$", RegexOptions.IgnoreCase)] private static partial Regex VersionWordRegex();
    [GeneratedRegex(@"(?<![\w:-])[0-9A-F]{2}(?<s>[:-])(?:[0-9A-F]{2}\k<s>){4}[0-9A-F]{2}(?![\w:-])", RegexOptions.IgnoreCase)] private static partial Regex MacRegex();
    [GeneratedRegex(@"(?<key>\bmac(?:[ _]?addr(?:ess)?)?\b)(?<sep>[""']?\s*[:=]\s*[""']?)[0-9A-F]{12}\b", RegexOptions.IgnoreCase)] private static partial Regex MacLabelledRegex();

    /// <summary>A file another program may be writing: whole up to <paramref name="head"/> + <paramref name="tail"/> bytes,
    /// otherwise its first <paramref name="head"/> and last <paramref name="tail"/> bytes with a line saying what was left out.</summary>
    internal static string HeadAndTail(string path, long head, long tail)
    {
        using var stream = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete);
        var length = stream.Length;
        if (length <= head + tail) { using var whole = new StreamReader(stream, Encoding.UTF8, true); return whole.ReadToEnd(); }
        var start = new byte[head];
        stream.ReadExactly(start);
        var cut = Array.LastIndexOf(start, (byte)'\n') is var newline and > 0 ? newline + 1 : start.Length;
        stream.Seek(-tail, SeekOrigin.End);
        using var reader = new StreamReader(stream, Encoding.UTF8, true);
        var end = reader.ReadToEnd();
        var resume = end.IndexOf('\n') is var first and >= 0 && first < end.Length - 1 ? end[(first + 1)..] : end;
        return Encoding.UTF8.GetString(start, 0, cut) + string.Create(CultureInfo.InvariantCulture, $"\n[... {length - head - tail:N0} bytes left out by the diagnostic package ...]\n") + resume;
    }

    /// <summary>The last <paramref name="limit"/> bytes of a file another program may be writing.</summary>
    internal static string Tail(string path, long limit)
    {
        using var stream = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete);
        if (stream.Length > limit) stream.Seek(-limit, SeekOrigin.End);
        using var reader = new StreamReader(stream, Encoding.UTF8, true);
        return reader.ReadToEnd();
    }

    /// <summary>DLSS, DirectX and loader DLLs in DCS's bin folder with their file versions (DCS's own DLSS version).</summary>
    private static string BinaryVersions(string bin)
    {
        var builder = new StringBuilder();
        try
        {
            foreach (var file in Directory.EnumerateFiles(bin, "*.dll").Where(f => Path.GetFileName(f) is var n && (n.StartsWith("nvngx", StringComparison.OrdinalIgnoreCase) || n.StartsWith("dxgi", StringComparison.OrdinalIgnoreCase) || n.StartsWith("d3d", StringComparison.OrdinalIgnoreCase) || n.StartsWith("openxr", StringComparison.OrdinalIgnoreCase) || n.StartsWith("sl.", StringComparison.OrdinalIgnoreCase))).Order())
            {
                var info = FileVersionInfo.GetVersionInfo(file);
                builder.Append(CultureInfo.InvariantCulture, $"{Path.GetFileName(file)}\t{(info.FileMajorPart + info.FileMinorPart + info.FileBuildPart + info.FilePrivatePart == 0 ? "-" : $"{info.FileMajorPart}.{info.FileMinorPart}.{info.FileBuildPart}.{info.FilePrivatePart}")}\t{new FileInfo(file).Length:N0} bytes\t{info.CompanyName}\n");
            }
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException) { builder.Append(e.Message); }
        return builder.ToString();
    }

    /// <summary>The active OpenXR runtime and every implicit API layer registered for all users and this user, with its
    /// state (0 = enabled): another Quad Views or overlay layer explains many problems.</summary>
    private static string OpenXrRegistry()
    {
        var builder = new StringBuilder();
        foreach (var (hive, name) in new[] { (Registry.LocalMachine, "HKLM"), (Registry.CurrentUser, "HKCU") })
        {
            try
            {
                using var root = hive.OpenSubKey(@"SOFTWARE\Khronos\OpenXR\1");
                if (root is null) continue;
                if (root.GetValue("ActiveRuntime") is string runtime) builder.Append(CultureInfo.InvariantCulture, $"{name} ActiveRuntime = {runtime}\n");
                foreach (var kind in new[] { "ApiLayers\\Implicit", "ApiLayers\\Explicit" })
                {
                    using var layers = root.OpenSubKey(kind);
                    if (layers is null) continue;
                    foreach (var manifest in layers.GetValueNames())
                        builder.Append(CultureInfo.InvariantCulture, $"{name} {kind} {(Equals(layers.GetValue(manifest), 0) ? "enabled " : "disabled")} {manifest}{(File.Exists(manifest) ? "" : "  (file missing)")}\n");
                }
            }
            catch (Exception e) when (e is System.Security.SecurityException or UnauthorizedAccessException or IOException) { builder.Append(name).Append(": ").Append(e.Message).Append('\n'); }
        }
        return builder.Length == 0 ? "No OpenXR registration found.\n" : builder.ToString();
    }

    /// <summary>Windows version, CPU, memory and display adapters with their driver versions (from the registry).</summary>
    private static string SystemSummary()
    {
        var builder = new StringBuilder();
        builder.Append(CultureInfo.InvariantCulture, $"DCS Control {ProductInfo.Version}\nWindows {Environment.OSVersion.Version} ({(Environment.Is64BitOperatingSystem ? "64-bit" : "32-bit")})\nLogical processors {Environment.ProcessorCount}\n");
        try
        {
            using var cpu = Registry.LocalMachine.OpenSubKey(@"HARDWARE\DESCRIPTION\System\CentralProcessor\0");
            if (cpu?.GetValue("ProcessorNameString") is string name) builder.Append("CPU ").Append(name.Trim()).Append('\n');
            using var build = Registry.LocalMachine.OpenSubKey(@"SOFTWARE\Microsoft\Windows NT\CurrentVersion");
            if (build?.GetValue("DisplayVersion") is string display) builder.Append("Windows release ").Append(display).Append(", build ").Append(build.GetValue("CurrentBuild")).Append('\n');
            using var adapters = Registry.LocalMachine.OpenSubKey(@"SYSTEM\CurrentControlSet\Control\Class\{4d36e968-e325-11ce-bfc1-08002be10318}");
            foreach (var key in adapters?.GetSubKeyNames().Where(k => k.Length == 4 && k.All(char.IsDigit)) ?? [])
            {
                using var adapter = adapters!.OpenSubKey(key);
                if (adapter?.GetValue("DriverDesc") is string desc) builder.Append("GPU ").Append(desc).Append(", driver ").Append(adapter.GetValue("DriverVersion")).Append('\n');
            }
        }
        catch (Exception e) when (e is System.Security.SecurityException or UnauthorizedAccessException or IOException) { builder.Append(e.Message).Append('\n'); }
        if (NvidiaSettings.ThreadedOptimizationForDcs() is { } threaded) builder.Append("NVIDIA Threaded optimization for DCS: ").Append(threaded.Value).Append(" (").Append(threaded.Where).Append(")\n");
        if (NvidiaSettings.MfaaForDcs() is { } mfaa) builder.Append("NVIDIA MFAA for DCS: ").Append(mfaa.On ? "On" : "Off").Append(" (").Append(mfaa.Where).Append(")\n");
        if (NvidiaSettings.DlssPresetForDcs() is { } dlssPreset) builder.Append("NVIDIA DLSS-SR preset for DCS: ").Append(dlssPreset.Forced ? "forces " + dlssPreset.Name : "not forced").Append(dlssPreset.FromDcsProfile ? " (DCS program profile)" : " (base profile)").Append(dlssPreset.LatestDll is { } latest ? latest ? ", latest DLL on" : ", latest DLL off" : "").Append('\n');
        builder.Append(CultureInfo.InvariantCulture, $"Physical memory about {GC.GetGCMemoryInfo().TotalAvailableMemoryBytes / (1024 * 1024 * 1024.0):0} GB\n");
        return builder.ToString();
    }
}
