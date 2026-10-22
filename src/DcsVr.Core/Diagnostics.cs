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
    public static IReadOnlyList<DiagnosticEntry> CreatePackage(DiagnosticSources sources, string report, string zipPath)
    {
        var entries = new List<DiagnosticEntry>();
        var redactor = Redactor();
        var temp = zipPath + ".tmp";
        try
        {
        using (var stream = new FileStream(temp, FileMode.Create, FileAccess.Write))
        using (var zip = new ZipArchive(stream, ZipArchiveMode.Create))
        {
            void Text(string name, string source, string content, bool truncated = false)
            {
                var bytes = Encoding.UTF8.GetBytes(redactor(content));
                var entry = zip.CreateEntry(name, CompressionLevel.Optimal);
                using (var output = entry.Open()) output.Write(bytes);
                entries.Add(new(name, redactor(source), bytes.Length, truncated));
            }
            void File_(string name, string? path)
            {
                if (path is null || !File.Exists(path) || entries.Any(e => e.Name == name)) return;
                // The package stays small enough to attach: once 150 MB of text is in, the remaining (older) files are listed only.
                if (entries.Sum(e => e.Bytes) > 150L * 1024 * 1024) { entries.Add(new(name + " (left out: package size limit)", redactor(path), 0, true)); return; }
                try
                {
                    var length = new FileInfo(path).Length;
                    Text(name, path, HeadAndTail(path, HeadLimit, TextLimit), length > HeadLimit + TextLimit);
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

            Text("report.json", "DCS Control", report);
            var local = sources.LocalAppData ?? Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData);
            var roaming = sources.RoamingAppData ?? Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData);

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
            if (sources.DcsExecutable is { } exe && Path.GetDirectoryName(exe) is { } bin)
            {
                Folder("dcs/bin/CheekyFoveatedDLSS", Path.Combine(bin, "CheekyFoveatedDLSS"));
                File_("dcs/bin/CheekyFoveatedDLSS-Loader.log", Path.Combine(bin, "CheekyFoveatedDLSS-Loader.log"));
                Text("dcs/bin/modules.txt", bin, BinaryVersions(bin));
            }

            // Headset software.
            if (sources.Runtime == RuntimeKind.SboysSteamVr)
            {
                File_("steamvr/steamvr.vrsettings", sources.SteamVrSettings);
                if (sources.SteamVrSettings is { } settings && Path.GetDirectoryName(Path.GetDirectoryName(settings)) is { } steam)
                    foreach (var name in new[] { "vrserver.txt", "vrcompositor.txt", "vrclient_DCS.txt" }) File_("steamvr/logs/" + name, Path.Combine(steam, "logs", name));
            }
            Folder("pimax/runtime", Path.Combine(local, "Pimax", "runtime"), "pvr_srv_log_*", 1);
            File_("pimax/runtime/pvr_client_DCS_log.txt", Path.Combine(local, "Pimax", "runtime", "pvr_client_DCS_log.txt"));
            File_("pimax/runtime/profile.json", Path.Combine(local, "Pimax", "runtime", "profile.json"));
            Folder("pimax/PiService", Path.Combine(local, "Pimax", "PiService", "Log"), "PiService__*", 2);
            File_("pimax/AppConfig/global.json", Path.Combine(roaming, "Pimax", "AppConfig", "global.json"));

            Text("openxr.txt", "Windows registry", OpenXrRegistry());
            Text("system.txt", "Windows", SystemSummary());
            Text("README.txt", "DCS Control", Readme(entries));
        }
        File.Move(temp, zipPath, true);
        }
        catch
        {
            // No half-written package next to the one the pilot asked for.
            try { File.Delete(temp); } catch (Exception e) when (e is IOException or UnauthorizedAccessException) { }
            throw;
        }
        return entries;
    }

    private static string Readme(IReadOnlyList<DiagnosticEntry> entries) =>
        $"DCS Control {ProductInfo.Version} diagnostic package, {DateTimeOffset.Now:yyyy-MM-dd HH:mm zzz}.\n"
        + "Logs and settings of DCS, DCS Control's components and the headset software. The Windows user name, PC name and e-mail addresses are replaced by <user>, <pc> and <e-mail>; account tokens, account ids and the headset's serial number by <token>, <id> and <serial>.\n"
        + "Large logs are cut to their last 8 MB (marked below).\n\n"
        + string.Join("\n", entries.Select(e => string.Create(CultureInfo.InvariantCulture, $"{e.Name}  ({e.Bytes:N0} bytes{(e.Truncated ? ", last part" : "")})  from {e.Source}")));

    /// <summary>Replaces the Windows user name (also in the user's profile folder), the PC name and e-mail addresses.</summary>
    internal static Func<string, string> Redactor() => Redactor(Environment.UserName, Environment.GetFolderPath(Environment.SpecialFolder.UserProfile), Environment.MachineName);

    /// <summary>Removes what identifies the pilot: the user's profile folder in every spelling a log or JSON uses (\, \\,
    /// /), the user and profile folder names, the PC name, e-mail addresses, and what Pimax's service logs carry
    /// (session tokens, account ids, the headset's serial number).</summary>
    internal static Func<string, string> Redactor(string user, string profile, string machine)
    {
        var folder = Path.GetFileName(profile.TrimEnd('\\', '/'));
        var names = new[] { user, folder }.Where(n => n.Length >= 3).Distinct(StringComparer.OrdinalIgnoreCase)
            .SelectMany(n => new[] { n, System.Text.Json.JsonEncodedText.Encode(n).ToString() }).Distinct(StringComparer.Ordinal).ToArray();
        var serials = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        var paths = profile.Length > 3 ?new[] { (profile, @"C:\Users\<user>"), (profile.Replace("\\", "\\\\"), @"C:\\Users\\<user>"), (profile.Replace('\\', '/'), "C:/Users/<user>") } : [];
        return text =>
        {
            if (string.IsNullOrEmpty(text)) return text;
            foreach (var (from, to) in paths) text = text.Replace(from, to, StringComparison.OrdinalIgnoreCase);
            // Short names are removed where they are a profile folder, anywhere else only from three letters up.
            if (folder.Length is > 0 and < 3) text = Regex.Replace(text, @"(?<=Users[\\/]{1,2})" + Regex.Escape(folder) + @"(?=[\\/""]|$)", "<user>", RegexOptions.IgnoreCase);
            foreach (var name in names) text = Regex.Replace(text, @"(?<![\w])" + Regex.Escape(name) + @"(?![\w])", "<user>", RegexOptions.IgnoreCase);
            // An 8.3 short name of the profile folder ("C:\Users\GIUSEP~1") starts with the name too.
            text = ShortProfileRegex().Replace(text, "<user>");
            if (machine.Length >= 3) text = Regex.Replace(text, @"(?<![\w-])" + Regex.Escape(machine) + @"(?![\w-])", "<pc>", RegexOptions.IgnoreCase);
            text = TokenRegex().Replace(text, "${key}${sep}<token>");
            text = AccountRegex().Replace(text, "${key}${sep}<id>");
            // A serial number seen once (next to its label) is removed wherever else it appears, also in later files.
            foreach (Match m in SerialRegex().Matches(text)) serials.Add(m.Value[(m.Groups["key"].Length + m.Groups["sep"].Length)..]);
            foreach (var serial in serials) text = text.Replace(serial, "<serial>", StringComparison.OrdinalIgnoreCase);
            return EmailRegex().Replace(text, "<e-mail>");
        };
    }
    [GeneratedRegex(@"[\w.+-]+@[\w-]+(\.[\w-]+)+")] private static partial Regex EmailRegex();
    [GeneratedRegex(@"(?<=Users[\\/]{1,2})[^\\/\s]{1,6}~\d+", RegexOptions.IgnoreCase)] private static partial Regex ShortProfileRegex();
    [GeneratedRegex(@"(?<key>\b(?:access_?token|refresh_?token|token|authorization|bearer)\b)(?<sep>[""']?\s*[:=]\s*[""']?\s*)(?!<token>)[^\s,""'}]+", RegexOptions.IgnoreCase)] private static partial Regex TokenRegex();
    [GeneratedRegex(@"(?<key>\b(?:user_?id|account_?id)\b)(?<sep>[""']?\s*[:=]\s*[""']?)\w+", RegexOptions.IgnoreCase)] private static partial Regex AccountRegex();
    [GeneratedRegex(@"(?<key>\b(?:serial(?:[ _]?number)?|hmd_?sn|device_?sn|sn)\b)(?<sep>[""']?\s*[:=]\s*[""']?)[A-Z0-9][A-Z0-9-]{5,}", RegexOptions.IgnoreCase)] private static partial Regex SerialRegex();

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
