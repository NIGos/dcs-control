namespace DcsVr.Core;

/// <summary>What the DCS engine optimizations (DcsQvCull) report about themselves: whether the module is installed and
/// loaded, which optimizations found what they need in this DCS build, and the newest test-suite report.</summary>
/// <param name="State">"not-installed", "installed" (DCS has not loaded it since it was written), "loaded" or "failed".</param>
/// <param name="Summary">One line for the page.</param>
/// <param name="Warnings">Conditions the user should act on (a DCS update the module does not recognise).</param>
/// <param name="NoDriverCommandLists">The latest module log says DCS's device has no driver D3D11 command lists
/// (DriverCommandLists=0): the option to run the recorders on emulated lists can matter.</param>
public sealed record EngineStatus(string State, string Summary, IReadOnlyList<string> Warnings, bool TimerCacheAvailable, bool TimerCacheActive,
    bool SceneHooked, bool SuiteRunning, string ModuleDirectory, string? LogPath, string? LastReport, DateTime? LastReportAt, string? LastStats,
    bool NoDriverCommandLists = false, IReadOnlyList<string>? VersionWarnings = null)
{
    /// <summary>The warnings that say a part stayed off because it does not know this DCS build (a subset of
    /// <see cref="Warnings"/>): the Checks page repeats them, as they matter also with Engine Optimizations off.</summary>
    public IReadOnlyList<string> VersionWarnings { get; init; } = VersionWarnings ?? [];
}

/// <summary>The module's shader warm-up (warmup.json next to its log, native/dcsqvcull/src/warmup_status.h).</summary>
/// <param name="State">"prewarm" (stale variants rechecked, usually in the main menu) or "warming" (the scene's variants
/// compiling).</param>
/// <param name="Done">Variants ready.</param>
/// <param name="Total">Ready plus still to do; it may grow while DCS finds more.</param>
public sealed record EngineWarmup(string State, int Done, int Total, int Hits, int Compiled, int Seconds);

public static class EngineOptimizations
{
    public const string ModuleFolder = "DcsQvCull";
    public const string SuiteFlag = "run_suite.flag";
    public const string WarmupFile = "warmup.json";
    /// <summary>The module rewrites the file every second while it warms up: an older one is from a DCS that has ended.</summary>
    public static readonly TimeSpan WarmupFresh = TimeSpan.FromSeconds(15);

    /// <summary>The warm-up in progress while DCS runs, or null: DCS not running, no file, idle, done, or a file the
    /// module has stopped updating.</summary>
    public static EngineWarmup? ReadWarmup(string module, bool dcsRunning, DateTime? utcNow = null)
    {
        if (!dcsRunning) return null;
        var path = Path.Combine(module, WarmupFile);
        try
        {
            var info = new FileInfo(path);
            if (!info.Exists || info.Length > 4096 || (utcNow ?? DateTime.UtcNow) - info.LastWriteTimeUtc > WarmupFresh) return null;
            using var stream = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete);
            using var doc = System.Text.Json.JsonDocument.Parse(stream);
            var root = doc.RootElement;
            var state = root.TryGetProperty("state", out var s) && s.ValueKind == System.Text.Json.JsonValueKind.String ? s.GetString() : null;
            if (state is not ("prewarm" or "warming")) return null;
            int Number(string name) => root.TryGetProperty(name, out var v) && v.TryGetInt32(out var n) && n >= 0 ? n : 0;
            var done = Number("done");
            return new(state, done, Math.Max(done, Number("total")), Number("hits"), Number("compiled"), Number("seconds"));
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException or System.Text.Json.JsonException or InvalidOperationException) { return null; }
    }

    /// <summary>Saved Games\DCS*\Scripts\DcsQvCull for the options.lua in Saved Games\DCS*\Config.</summary>
    public static string ModuleDirectory(string optionsPath) =>
        Path.Combine(Path.GetDirectoryName(Path.GetDirectoryName(Path.GetFullPath(optionsPath))!)!, "Scripts", ModuleFolder);

    /// <summary>The module's log of the latest DCS start: DcsQvCull.log, or DcsQvCull.&lt;pid&gt;.log written by a DCS started
    /// while another one held DcsQvCull.log, whichever was written last (never the kept .prev.log).</summary>
    public static string ModuleLog(string module)
    {
        var log = Path.Combine(module, "DcsQvCull.log");
        try
        {
            if (!Directory.Exists(module)) return log;
            var newest = new DirectoryInfo(module).EnumerateFiles("DcsQvCull.*.log")
                .Where(f => System.Text.RegularExpressions.Regex.IsMatch(f.Name, @"^DcsQvCull\.[0-9]+\.log$"))
                .Append(new FileInfo(log)).Where(f => f.Exists).MaxBy(f => f.LastWriteTimeUtc);
            return newest?.FullName ?? log;
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException) { return log; }
    }

    /// <summary>Reads DcsQvCull.log (rewritten at every DCS start) and the newest report. <paramref name="dcsLog"/> is
    /// DCS's own log, where a module that could not be loaded is reported.</summary>
    public static EngineStatus Read(string optionsPath, string? dcsLog, bool dcsRunning)
    {
        var module = ModuleDirectory(optionsPath);
        var loader = Path.Combine(module, "DcsQvCull.dll");
        var hook = Path.Combine(Path.GetDirectoryName(module)!, "Hooks", "DcsQvCull.lua");
        var log = ModuleLog(module);
        var (report, reportAt) = NewestReport(module);
        if (!File.Exists(loader) || !File.Exists(hook))
            return new("not-installed", "Launch DCS (or Apply without launching) installs it when Engine Optimizations is on.", [], false, false, false, false, module, null, report, reportAt, null);
        var warnings = new List<string>();
        var lines = ReadShared(log);
        var noLists = lines is not null && HasNoDriverCommandLists(lines);
        // The log is written when DCS starts the module; one older than the loader belongs to an earlier install.
        if (lines is null || File.GetLastWriteTimeUtc(log) < File.GetLastWriteTimeUtc(loader))
        {
            var failed = LoadFailure(dcsLog, File.GetLastWriteTimeUtc(loader));
            if (failed is not null) return new("failed", "DCS could not load the module: " + failed, [failed], false, false, false, false, module, null, report, reportAt, null, noLists);
            return new("installed", dcsRunning ? "Installed. DCS loads it when its interface starts." : "Installed. DCS loads it at its next start.", [], false, false, false, false, module, null, report, reportAt, null, noLists);
        }
        bool Has(string text) => lines.Any(l => l.Contains(text, StringComparison.Ordinal));
        // The loader's last outcome counts: a payload that failed to start after an earlier one ran is a failure now.
        var payloadStart = lines.FindLastIndex(l => l.Contains("loader: starting payload", StringComparison.Ordinal));
        var startFailed = lines.FindLastIndex(l => l.Contains("loader: payload start reported an error", StringComparison.Ordinal) || l.Contains("loader: payload exports missing", StringComparison.Ordinal) || l.Contains("loader: payload not loaded (", StringComparison.Ordinal));
        if (startFailed > payloadStart)
        {
            var why = lines[startFailed][(lines[startFailed].IndexOf("loader: ", StringComparison.Ordinal) + 8)..].Trim();
            return new("failed", "The module could not start: " + why + ". See DcsQvCull.log.", [why], false, false, false, false, module, log, report, reportAt, null, noLists);
        }
        var loaded = payloadStart >= 0;
        var timerAvailable = Has("timer cache: dx11backend!ED_get_time redirected");
        var lastStats = lines.LastOrDefault(l => l.Contains("stats: ", StringComparison.Ordinal));
        var timerActive = lastStats?.Contains("timer=cache", StringComparison.Ordinal) == true;
        // boost=off: the in-flight switch has the optimizations off (until it is pressed again or the ini changes).
        var switchedOff = lastStats?.Contains("boost=off", StringComparison.Ordinal) == true;
        var scene = Has("scene: DCSScene at");
        // Developer mode: the loader names a payload other than the installed one; the module names the settings it reads.
        var source = lines.LastOrDefault(l => l.Contains("loader: payload source ", StringComparison.Ordinal));
        var devPayload = source is not null && !source.TrimEnd().EndsWith(Path.Combine("payload", "DcsQvCullPayload.dll"), StringComparison.OrdinalIgnoreCase)
            ? source[(source.IndexOf("loader: payload source ", StringComparison.Ordinal) + 23)..].Trim() : null;
        var settings = lines.LastOrDefault(l => l.Contains("config: dev mode", StringComparison.Ordinal));
        var devIni = settings is not null && settings.Contains("dev mode, settings from ", StringComparison.Ordinal)
            ? settings[(settings.IndexOf("dev mode, settings from ", StringComparison.Ordinal) + 24)..].Trim() : null;
        var version = new List<string>();
        void Version(string warning) { warnings.Add(warning); version.Add(warning); }
        if (Has("partition boost: unexpected scene layout"))
            Version("This DCS version changed something Culling partition boost relies on, so it turned itself off. The other optimizations still run.");
        // A graphics driver without D3D11 command lists (common on AMD) cannot run the multi-threaded recorders: not an
        // error, DCS draws shadows and the G-buffer as usual.
        // The module's line names each capability, the device's creation flags and DCS's adapter (before 0.5.7: one
        // "the driver lacks command lists" line).
        if (lines.LastOrDefault(l => l.Contains("the driver lacks command lists", StringComparison.Ordinal) || l.Contains("the D3D11 device lacks what the recorder needs", StringComparison.Ordinal)) is { } missing)
            warnings.Add(NoCommandListsAdvice(missing));
        // The recorders ran on the lists Windows emulates (Multi-threading without driver support, the pilot's choice there): on a PC with an
        // NVIDIA card something takes the driver's own lists from DCS (integrated graphics, MFAA, a hooked program), so say
        // what gives the full gain. The GPU line names DCS's adapter; a recorder's line is the fallback.
        else if ((lines.LastOrDefault(l => l.Contains("DriverCommandLists=0 (recorders on emulated lists)", StringComparison.Ordinal))
                     ?? lines.LastOrDefault(l => l.Contains("recorder: no driver command lists", StringComparison.Ordinal))) is { } emulated
                 && EmulatedListsAdvice(emulated) is { } advice)
            warnings.Add(advice);
        // The newer optimizations check the DCS build first and stay off when it is not the one they were made for. Only
        // a build-match line (the code they patch does not look as on the analysed build) is a DCS version problem for
        // the Checks page; a safety check that latched, a contained exception or a failed allocation is said on the
        // Engine page only.
        foreach (var (prefix, name) in new[] { ("model allocator:", "model data allocator"), ("triangle counter:", "statistics counter"),
                     ("constant buffer skip:", "effect constant-buffer skip"), ("texture binds:", "texture streaming dedupe"), ("partition weights:", "culling partition by real cost"), ("frame heap:", "frame memory per thread"), ("task-queue clock:", "task queue clock cache"), ("shadow batching:", "shadow caster instancing"), ("shadow inst:", "shadow caster instancing"), ("big model pages:", "shadow caster instancing"), ("split filter", "redundant state filter"), ("shadow recorder", "multi-threaded shadow recorder"), ("gbuffer recorder", "multi-threaded G-buffer") })
        {
            var off = lines.Where(l => l.Contains(prefix, StringComparison.Ordinal) && !l.Contains("not recordable", StringComparison.Ordinal) && !l.Contains("texture classes the table refused", StringComparison.Ordinal) && !l.Contains("the driver lacks command lists", StringComparison.Ordinal) && !l.Contains("the D3D11 device lacks what the recorder needs", StringComparison.Ordinal) && !l.Contains("quad-view filter", StringComparison.Ordinal) && !l.Contains("execution identity", StringComparison.Ordinal) && (BuildMismatch(l) || l.Contains("unknown patch", StringComparison.Ordinal) || l.Contains("could not", StringComparison.Ordinal) || l.Contains("FAILED", StringComparison.Ordinal) || l.Contains("disabled for this session", StringComparison.Ordinal) || l.Contains("off for this session", StringComparison.Ordinal) || l.Contains("NOT SAFE", StringComparison.Ordinal) || l.Contains("latched", StringComparison.Ordinal) || (l.Contains("exception", StringComparison.OrdinalIgnoreCase) && l.Contains("contained", StringComparison.Ordinal)) || l.Contains("DISABLED", StringComparison.Ordinal) || l.EndsWith("; unavailable", StringComparison.Ordinal) || l.Contains("; jobs start at RenderGraph::render entry", StringComparison.Ordinal) || l.EndsWith("; off", StringComparison.Ordinal) || l.EndsWith("; skipped", StringComparison.Ordinal) || l.Contains("not installed", StringComparison.Ordinal) && !l.Contains("counters not installed", StringComparison.Ordinal))).ToList();
            var title = $"{char.ToUpperInvariant(name[0])}{name[1..]}";
            if (off.Any(BuildMismatch)) Version($"{title} turned itself off: it does not recognise this DCS version. The other optimizations still run.");
            else if (off.Count > 0) warnings.Add($"{title} turned itself off: one of its safety checks failed. The other optimizations still run.");
        }
        // Depth boost's dual camera finds the pilot's own aircraft through a table in NGModel.dll; a DCS build that moved it
        // leaves that matching off (the dual camera itself keeps running).
        if (Has("cockpit ipd: ownship matching off"))
            Version("This DCS version changed something the dual camera uses to recognise your own aircraft, so that part is off. Depth boost still runs.");
        // The frame-timing helpers (frame start, view profile, frame timeline, cockpit outline) know the DCS builds they were
        // analysed on; on another (a newer DCS) they stay off or fall back. One line, whichever said so.
        if (Has("is not an analysed build"))
            Version("DCS Control has not been checked against this DCS version yet, so some frame timing helpers are off. Everything else runs.");
        // Depth boost's dual camera hooks a G-buffer pass of a known build (cockpit_ipd.h Start); Sharp HUD hooks DCS's HUD
        // record render point (symbology.h). On another build each stays off, and said so only in the module's log.
        if (Has("cockpit ipd: build mismatch"))
            Version("This DCS version changed something Depth boost's dual camera needs, so Depth boost is off.");
        if (lines.LastOrDefault(l => l.Contains("symbology: record render point ", StringComparison.Ordinal)) is { } hud && hud.Contains("NOT hooked", StringComparison.Ordinal))
            Version("Sharp HUD did not find where this DCS version draws the HUD, so it is off. Frame generation still runs.");
        if (Has("ERROR: Scene.dll exports not found"))
            Version("This DCS version is not supported by the culling hook, so Culling partition boost is off. The other optimizations still run.");
        // A suite is running from its start line until its report line (statistics keep arriving after both).
        var started = lines.FindLastIndex(l => l.Contains("==== DcsQvCull test suite", StringComparison.Ordinal));
        var written = lines.FindLastIndex(l => l.Contains("report written to report_", StringComparison.Ordinal) || l.Contains("==== suite aborted", StringComparison.Ordinal));
        var running = dcsRunning && (File.Exists(Path.Combine(module, SuiteFlag)) || (started >= 0 && started > written));
        if (!loaded) return new("installed", "Installed, but DCS has not started it yet.", warnings, false, false, false, running, module, log, report, reportAt, lastStats, noLists, version);
        var parts = new List<string> { dcsRunning ? "Loaded in the running DCS" : "Loaded at the last DCS start" };
        if (devPayload is not null) parts.Add("developer payload " + devPayload);
        if (devIni is not null) parts.Add("developer settings " + devIni);
        if (dcsRunning && switchedOff) parts.Add("switched off in flight (press the in-flight switch again)");
        parts.Add(timerAvailable ? (timerActive || lastStats is null ? "streaming timer cache on" : "streaming timer cache off") : "streaming timer cache not available");
        parts.Add(scene ? "culling hook on" : "culling hook not seen yet");
        return new("loaded", string.Join(" · ", parts) + ".", warnings, timerAvailable, timerActive, scene, running, module, log, report, reportAt, lastStats, noLists, version);
    }

    /// <summary>A module line that says the code a part patches does not look as on the DCS build it was analysed on (as
    /// opposed to a safety check that latched in flight): the Checks page's "DCS version" line.</summary>
    internal static bool BuildMismatch(string line) =>
        line.Contains("does not match", StringComparison.Ordinal) || line.Contains("unexpected bytes", StringComparison.Ordinal)
        || line.Contains("is not an analysed build", StringComparison.Ordinal) || line.Contains("build mismatch", StringComparison.Ordinal)
        || line.Contains("NOT hooked", StringComparison.Ordinal) || line.Contains("exports not found", StringComparison.Ordinal);

    /// <summary>Starts the in-game test suite: DcsQvCull checks for run_suite.flag next to its DLL every second.</summary>
    public static string StartSuite(string optionsPath, bool dcsRunning)
    {
        if (!dcsRunning) throw new InvalidOperationException("Start DCS and get into a flight first: the test suite runs inside DCS (in VR, keep the headset on and awake).");
        var module = ModuleDirectory(optionsPath);
        if (!File.Exists(Path.Combine(module, "DcsQvCull.dll"))) throw new InvalidOperationException("Engine Optimizations are not installed yet. Turn them on and start DCS with Launch DCS first.");
        File.WriteAllBytes(Path.Combine(module, SuiteFlag), []);
        return "Test suite requested. It takes about 7 minutes; keep the headset on and still, in a busy scene. The report appears here when it is done.";
    }

    /// <summary>The module's log says DCS's device has no driver command lists: its GPU line or a recorder's line with
    /// DriverCommandLists=0, a recorder's line when it ran on emulated lists, or the line of a module before 0.5.7.</summary>
    public static bool HasNoDriverCommandLists(IEnumerable<string> lines) =>
        lines.Any(l => l.Contains("DriverCommandLists=0", StringComparison.Ordinal) || l.Contains("the driver lacks command lists", StringComparison.Ordinal)
            || l.Contains("recorder: no driver command lists", StringComparison.Ordinal));

    /// <summary>The PC's graphics are AMD only (no NVIDIA adapter), so DCS runs on an AMD driver, which has no D3D11 command
    /// lists. An AMD integrated GPU next to an NVIDIA card does not count: DCS runs on the NVIDIA card.</summary>
    public static bool AmdOnly(IReadOnlyList<GpuAdapter> adapters) =>
        adapters.Any(a => a.Vendor == "AMD") && !adapters.Any(a => a.Vendor == "NVIDIA");

    /// <summary>"Multi-threading without driver support" left automatic (VrProfile.EngineRecordersEmulatedLists null) is
    /// on: AMD-only graphics, where it was tested (Radeon RX 7900 XTX, GitHub #27). Off elsewhere, as in 0.7.0: on an
    /// NVIDIA PC whose device lacks command lists it has not been tested, so the Engine page gives the advice and the
    /// pilot can turn it on. Unknown adapters: off (the option is hidden there too).</summary>
    public static bool EmulatedListsAutoOn() => AmdOnly(GpuInfo.Adapters());

    /// <summary>The Engine page shows "Multi-threading without driver support" only where it can matter: the GPU is AMD,
    /// or the latest module log says DriverCommandLists=0. Hidden on NVIDIA with command lists, where the module ignores
    /// it (on by default, so showing it while on would show it on every PC). The interface applies the same rule
    /// (app.js, emulatedListsHidden).</summary>
    public static bool ShowsEmulatedListsOption(IReadOnlyList<GpuAdapter> adapters, bool logNoDriverCommandLists) =>
        logNoDriverCommandLists || AmdOnly(adapters);

    /// <summary>Whether DcsQvCull.ini gets [Model] RecordersEmulatedLists=1 when the option is on: where the option shows
    /// (<see cref="ShowsEmulatedListsOption"/>), or when the adapters could not be read. With driver command lists the
    /// module ignores the key (deferred_rec.h RecorderMode picks the offset ring whenever DriverCommandLists=1), so a PC
    /// with them keeps the file earlier versions wrote. An NVIDIA PC whose device lacks the lists (MFAA On, the integrated
    /// GPU, a hooking program) gets it, when the pilot turned the option on (it is not automatic there,
    /// <see cref="EmulatedListsAutoOn"/>), from the launch after the module first logs DriverCommandLists=0.</summary>
    public static bool EmulatedListsMatter(IReadOnlyList<GpuAdapter> adapters, bool logNoDriverCommandLists) =>
        adapters.Count == 0 || ShowsEmulatedListsOption(adapters, logNoDriverCommandLists);

    /// <summary><see cref="EmulatedListsMatter(IReadOnlyList{GpuAdapter}, bool)"/> for this PC's adapters and the module's
    /// latest log in <paramref name="module"/> (Scripts\DcsQvCull); null: no module folder known.</summary>
    public static bool EmulatedListsMatter(string? module)
    {
        var adapters = GpuInfo.Adapters();
        if (adapters.Count == 0 || AmdOnly(adapters)) return true;
        return module is not null && LogSaysNoDriverCommandLists(ModuleLog(module));
    }

    private static readonly Lock LogCacheLock = new();
    private static (string Path, DateTime Written, long Length, bool NoLists)? _logCache;
    /// <summary><see cref="HasNoDriverCommandLists"/> for a module log, read again only when it changed (every Launch and
    /// preview plans the ini).</summary>
    private static bool LogSaysNoDriverCommandLists(string log)
    {
        try
        {
            var info = new FileInfo(log);
            if (!info.Exists) return false;
            lock (LogCacheLock)
                if (_logCache is { } c && c.Path == info.FullName && c.Written == info.LastWriteTimeUtc && c.Length == info.Length) return c.NoLists;
            var noLists = ReadShared(log) is { } lines && HasNoDriverCommandLists(lines);
            lock (LogCacheLock) _logCache = (info.FullName, info.LastWriteTimeUtc, info.Length, noLists);
            return noLists;
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException or ArgumentException) { return false; }
    }

    /// <summary>Why DCS's device has no D3D11 command lists, and what to do. NVIDIA's Threaded optimization is not named:
    /// Off did not remove them in a live test (RTX 5090, driver 32.0.16.2114, the module logged DriverCommandLists=1).</summary>
    public static string NoCommandListsAdvice(string? line = null)
    {
        const string start = "Multi-threaded shadows and G-buffer cannot run: DCS's graphics device has no D3D11 command lists. DCS draws them as usual and the other optimizations still run. ";
        if (line is not null && !line.Contains("DriverCommandLists=0", StringComparison.Ordinal) && line.Contains("DriverCommandLists=1", StringComparison.Ordinal))
            return "Multi-threaded shadows and G-buffer cannot run: DCS's graphics device lacks a capability they need (" + Capabilities(line) + "). DCS draws them as usual and the other optimizations still run. Please report it with a diagnostic package.";
        // On NVIDIA it stays off unless the pilot turns it on (untested there, EmulatedListsAutoOn), as in 0.7.0.
        return start + (NvidiaCause(line) is { } cause
            ? cause + " Or turn on Multi-threading without driver support, on this page, to run them anyway, with a smaller gain (not tested on NVIDIA yet)."
            : "This graphics driver does not offer them (common on AMD cards). Turn on Multi-threading without driver support, on this page, to run them anyway, with a smaller gain.");
    }

    /// <summary>The recorders run on the command lists Windows emulates (Multi-threading without driver support) on a PC
    /// with an NVIDIA card, whose driver has its own lists: something takes them from DCS, and fixing it gives the full
    /// gain. <paramref name="line"/>: the module's GPU line or a recorder's "no driver command lists" line. Null on a PC
    /// without an NVIDIA card (AMD: emulated lists are as good as it gets).</summary>
    public static string? EmulatedListsAdvice(string? line = null) =>
        GpuInfo.Adapters().Any(a => a.Vendor == "NVIDIA")
            ? "Multi-threaded shadows and G-buffer run on command lists Windows emulates, so the gain is smaller than it can be: DCS's graphics device has no D3D11 command lists of its own. " + NvidiaCause(line)
            : null;

    /// <summary>Why a PC with an NVIDIA card gives DCS no command lists, and what to do; null without an NVIDIA card (and
    /// no other cause in <paramref name="line"/>).</summary>
    private static string? NvidiaCause(string? line)
    {
        if (line?.Contains("PREVENT_INTERNAL_THREADING_OPTIMIZATIONS", StringComparison.Ordinal) == true)
            return "The device was created with threading optimizations turned off, which a program hooked into DCS usually does (an overlay, ReShade-like tools, some OpenXR layers). Try once without them.";
        // DCS on integrated graphics while the PC has an NVIDIA card: the line names DCS's adapter (vendor 0x1002 AMD, 0x8086 Intel).
        if (line is not null && (line.Contains("vendor 0x1002", StringComparison.OrdinalIgnoreCase) || line.Contains("vendor 0x8086", StringComparison.OrdinalIgnoreCase)) && GpuInfo.Adapters().Any(a => a.Vendor == "NVIDIA"))
            return "DCS runs on " + Adapter(line) + ", not on the NVIDIA card. Make DCS use the NVIDIA card (Windows Settings → System → Display → Graphics → DCS: High performance), or disable the integrated graphics, then start DCS again.";
        if (NvidiaSettings.MfaaForDcs() is { On: true } mfaa)
            return $"The cause: NVIDIA's Antialiasing - MFAA is On in {mfaa.Where}. {mfaa.Fix}, then start DCS again.";
        if (GpuInfo.Adapters().Any(a => a.Vendor == "NVIDIA"))
            return "NVIDIA cards have them, so check in NVIDIA Control Panel (Global settings and the DCS World program) that Antialiasing - MFAA is Off, that DCS runs on the NVIDIA card and not on integrated graphics, and that no other program wraps DCS's graphics (ReShade, Special K, overlays).";
        return null;
    }
    private static string Capabilities(string line) =>
        string.Join(", ", System.Text.RegularExpressions.Regex.Matches(line, @"(DriverCommandLists|ConstantBufferOffsetting|ID3D11DeviceContext1)=0").Select(m => m.Groups[1].Value + " missing")) is { Length: > 0 } list ? list : "see DcsQvCull.log";
    // A recorder's line: "... on <name> (vendor 0x...)"; the module's GPU line: "env: GPU <name> (DCS's device, vendor 0x...)".
    private static string Adapter(string line) =>
        System.Text.RegularExpressions.Regex.Match(line, @" on (?<name>.+?) \(vendor ") is { Success: true } m ? m.Groups["name"].Value
        : System.Text.RegularExpressions.Regex.Match(line, @"GPU (?<name>.+?) \([^)]*vendor ") is { Success: true } g ? g.Groups["name"].Value : "integrated graphics";

    private static (string? Path, DateTime? At) NewestReport(string module)
    {
        try
        {
            if (!Directory.Exists(module)) return (null, null);
            var newest = new DirectoryInfo(module).EnumerateFiles("report_*.txt").OrderByDescending(f => f.LastWriteTimeUtc).FirstOrDefault();
            return newest is null ? (null, null) : (newest.FullName, newest.LastWriteTime);
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException) { return (null, null); }
    }

    /// <summary>The lines of a file DCS may still be writing; null when it is missing or unreadable.</summary>
    private static List<string>? ReadShared(string path)
    {
        try
        {
            if (!File.Exists(path)) return null;
            using var stream = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete);
            // Only the end matters for a long session (statistics every 2 s); the start lines are in the first part.
            const int Head = 256 * 1024, Tail = 1024 * 1024;
            using var reader = new StreamReader(stream);
            if (stream.Length <= Head + Tail) return Split(reader.ReadToEnd());
            var head = new char[Head]; var read = reader.Read(head, 0, Head);
            stream.Seek(-Tail, SeekOrigin.End); reader.DiscardBufferedData();
            return Split(new string(head, 0, read) + "\n" + reader.ReadToEnd());
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException) { return null; }
        static List<string> Split(string text) => [.. text.Split('\n').Select(l => l.TrimEnd('\r'))];
    }

    /// <summary>DCS's own report that the Lua hook could not load the module, from a dcs.log written after the install.</summary>
    private static string? LoadFailure(string? dcsLog, DateTime installedUtc)
    {
        if (dcsLog is null || !File.Exists(dcsLog) || File.GetLastWriteTimeUtc(dcsLog) < installedUtc) return null;
        var line = ReadShared(dcsLog)?.LastOrDefault(l => l.Contains("DcsQvCull", StringComparison.Ordinal) && l.Contains("failed to load native module", StringComparison.Ordinal));
        if (line is null) return null;
        var at = line.IndexOf("failed to load native module", StringComparison.Ordinal);
        return line[at..].Trim();
    }
}
