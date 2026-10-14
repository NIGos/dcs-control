namespace DcsVr.Core;

/// <summary>What the DCS engine optimizations (DcsQvCull) report about themselves: whether the module is installed and
/// loaded, which optimizations found what they need in this DCS build, and the newest test-suite report.</summary>
/// <param name="State">"not-installed", "installed" (DCS has not loaded it since it was written), "loaded" or "failed".</param>
/// <param name="Summary">One line for the page.</param>
/// <param name="Warnings">Conditions the user should act on (a DCS update the module does not recognise).</param>
public sealed record EngineStatus(string State, string Summary, IReadOnlyList<string> Warnings, bool TimerCacheAvailable, bool TimerCacheActive,
    bool SceneHooked, bool SuiteRunning, string ModuleDirectory, string? LogPath, string? LastReport, DateTime? LastReportAt, string? LastStats);

public static class EngineOptimizations
{
    public const string ModuleFolder = "DcsQvCull";
    public const string SuiteFlag = "run_suite.flag";

    /// <summary>Saved Games\DCS*\Scripts\DcsQvCull for the options.lua in Saved Games\DCS*\Config.</summary>
    public static string ModuleDirectory(string optionsPath) =>
        Path.Combine(Path.GetDirectoryName(Path.GetDirectoryName(Path.GetFullPath(optionsPath))!)!, "Scripts", ModuleFolder);

    /// <summary>Reads DcsQvCull.log (rewritten at every DCS start) and the newest report. <paramref name="dcsLog"/> is
    /// DCS's own log, where a module that could not be loaded is reported.</summary>
    public static EngineStatus Read(string optionsPath, string? dcsLog, bool dcsRunning)
    {
        var module = ModuleDirectory(optionsPath);
        var loader = Path.Combine(module, "DcsQvCull.dll");
        var hook = Path.Combine(Path.GetDirectoryName(module)!, "Hooks", "DcsQvCull.lua");
        var log = Path.Combine(module, "DcsQvCull.log");
        var (report, reportAt) = NewestReport(module);
        if (!File.Exists(loader) || !File.Exists(hook))
            return new("not-installed", "Launch DCS (or Apply without launching) installs it when Engine Optimizations is on.", [], false, false, false, false, module, null, report, reportAt, null);
        var warnings = new List<string>();
        var lines = ReadShared(log);
        // The log is written when DCS starts the module; one older than the loader belongs to an earlier install.
        if (lines is null || File.GetLastWriteTimeUtc(log) < File.GetLastWriteTimeUtc(loader))
        {
            var failed = LoadFailure(dcsLog, File.GetLastWriteTimeUtc(loader));
            if (failed is not null) return new("failed", "DCS could not load the module: " + failed, [failed], false, false, false, false, module, null, report, reportAt, null);
            return new("installed", dcsRunning ? "Installed. DCS loads it when its interface starts." : "Installed. DCS loads it at its next start.", [], false, false, false, false, module, null, report, reportAt, null);
        }
        bool Has(string text) => lines.Any(l => l.Contains(text, StringComparison.Ordinal));
        var loaded = Has("loader: starting payload");
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
        if (Has("partition boost: unexpected scene layout"))
            warnings.Add("This DCS version changed something Culling partition boost relies on, so it turned itself off. The other optimizations still run.");
        // A graphics driver without D3D11 command lists (common on AMD) cannot run the multi-threaded recorders: not an
        // error, DCS draws shadows and the G-buffer as usual.
        // The module's line names each capability, the device's creation flags and DCS's adapter (before 0.5.7: one
        // "the driver lacks command lists" line).
        if (lines.LastOrDefault(l => l.Contains("the driver lacks command lists", StringComparison.Ordinal) || l.Contains("the D3D11 device lacks what the recorder needs", StringComparison.Ordinal)) is { } missing)
            warnings.Add(NoCommandListsAdvice(missing));
        // The newer optimizations check the DCS build first and stay off when it is not the one they were made for.
        foreach (var (prefix, name) in new[] { ("model allocator:", "model data allocator"), ("triangle counter:", "statistics counter"),
                     ("constant buffer skip:", "effect constant-buffer skip"), ("texture binds:", "texture streaming dedupe"), ("partition weights:", "culling partition by real cost"), ("frame heap:", "frame memory per thread"), ("task-queue clock:", "task queue clock cache"), ("shadow batching:", "shadow caster instancing"), ("shadow inst:", "shadow caster instancing"), ("big model pages:", "shadow caster instancing"), ("split filter", "redundant state filter"), ("shadow recorder", "multi-threaded shadow recorder"), ("gbuffer recorder", "multi-threaded G-buffer") })
            if (lines.Any(l => l.Contains(prefix, StringComparison.Ordinal) && !l.Contains("not recordable", StringComparison.Ordinal) && !l.Contains("texture classes the table refused", StringComparison.Ordinal) && !l.Contains("the driver lacks command lists", StringComparison.Ordinal) && !l.Contains("the D3D11 device lacks what the recorder needs", StringComparison.Ordinal) && (l.Contains("does not match", StringComparison.Ordinal) || l.Contains("unexpected bytes", StringComparison.Ordinal) || l.Contains("unknown patch", StringComparison.Ordinal) || l.Contains("could not", StringComparison.Ordinal) || l.Contains("FAILED", StringComparison.Ordinal) || l.Contains("disabled for this session", StringComparison.Ordinal) || l.Contains("off for this session", StringComparison.Ordinal) || l.Contains("NOT SAFE", StringComparison.Ordinal) || l.Contains("latched", StringComparison.Ordinal) || (l.Contains("exception", StringComparison.OrdinalIgnoreCase) && l.Contains("contained", StringComparison.Ordinal)) || l.Contains("DISABLED", StringComparison.Ordinal) || l.EndsWith("; unavailable", StringComparison.Ordinal) || l.Contains("; jobs start at RenderGraph::render entry", StringComparison.Ordinal) || l.EndsWith("; off", StringComparison.Ordinal) || l.EndsWith("; skipped", StringComparison.Ordinal) || l.Contains("not installed", StringComparison.Ordinal) && !l.Contains("counters not installed", StringComparison.Ordinal))))
                warnings.Add($"{char.ToUpperInvariant(name[0])}{name[1..]} turned itself off: it does not recognise this DCS version, or one of its safety checks failed. The other optimizations still run.");
        if (Has("ERROR: Scene.dll exports not found"))
            warnings.Add("This DCS version is not supported by the culling hook, so Culling partition boost is off. The other optimizations still run.");
        // A suite is running from its start line until its report line (statistics keep arriving after both).
        var started = lines.FindLastIndex(l => l.Contains("==== DcsQvCull test suite", StringComparison.Ordinal));
        var written = lines.FindLastIndex(l => l.Contains("report written to report_", StringComparison.Ordinal) || l.Contains("==== suite aborted", StringComparison.Ordinal));
        var running = dcsRunning && (File.Exists(Path.Combine(module, SuiteFlag)) || (started >= 0 && started > written));
        if (!loaded) return new("installed", "Installed, but DCS has not started it yet.", warnings, false, false, false, running, module, log, report, reportAt, lastStats);
        var parts = new List<string> { dcsRunning ? "Loaded in the running DCS" : "Loaded at the last DCS start" };
        if (devPayload is not null) parts.Add("developer payload " + devPayload);
        if (devIni is not null) parts.Add("developer settings " + devIni);
        if (dcsRunning && switchedOff) parts.Add("switched off in flight (press the in-flight switch again)");
        parts.Add(timerAvailable ? (timerActive || lastStats is null ? "streaming timer cache on" : "streaming timer cache off") : "streaming timer cache not available");
        parts.Add(scene ? "culling hook on" : "culling hook not seen yet");
        return new("loaded", string.Join(" · ", parts) + ".", warnings, timerAvailable, timerActive, scene, running, module, log, report, reportAt, lastStats);
    }

    /// <summary>Starts the in-game test suite: DcsQvCull checks for run_suite.flag next to its DLL every second.</summary>
    public static string StartSuite(string optionsPath, bool dcsRunning)
    {
        if (!dcsRunning) throw new InvalidOperationException("Start DCS and get into a flight first: the test suite runs inside DCS (in VR, keep the headset on and awake).");
        var module = ModuleDirectory(optionsPath);
        if (!File.Exists(Path.Combine(module, "DcsQvCull.dll"))) throw new InvalidOperationException("Engine Optimizations are not installed yet. Turn them on and start DCS with Launch DCS first.");
        File.WriteAllBytes(Path.Combine(module, SuiteFlag), []);
        return "Test suite requested. It takes about 7 minutes; keep the headset on and still, in a busy scene. The report appears here when it is done.";
    }

    /// <summary>Why DCS's device has no D3D11 command lists, and what to do: on NVIDIA almost always Threaded optimization Off.</summary>
    public static string NoCommandListsAdvice(string? line = null)
    {
        const string start = "Multi-threaded shadows and G-buffer cannot run: DCS's graphics device has no D3D11 command lists. DCS draws them as usual and the other optimizations still run. ";
        if (line is not null && !line.Contains("DriverCommandLists=0", StringComparison.Ordinal) && line.Contains("DriverCommandLists=1", StringComparison.Ordinal))
            return "Multi-threaded shadows and G-buffer cannot run: DCS's graphics device lacks a capability they need (" + Capabilities(line) + "). DCS draws them as usual and the other optimizations still run. Please report it with a diagnostic package.";
        if (line?.Contains("PREVENT_INTERNAL_THREADING_OPTIMIZATIONS", StringComparison.Ordinal) == true)
            return start + "The device was created with threading optimizations turned off, which a program hooked into DCS usually does (an overlay, ReShade-like tools, some OpenXR layers). Try once without them.";
        // DCS on integrated graphics while the PC has an NVIDIA card: the line names DCS's adapter (vendor 0x1002 AMD, 0x8086 Intel).
        if (line is not null && (line.Contains("vendor 0x1002", StringComparison.OrdinalIgnoreCase) || line.Contains("vendor 0x8086", StringComparison.OrdinalIgnoreCase)) && GpuInfo.Adapters().Any(a => a.Vendor == "NVIDIA"))
            return start + "DCS runs on " + Adapter(line) + ", not on the NVIDIA card. Make DCS use the NVIDIA card (Windows Settings → System → Display → Graphics → DCS: High performance), or disable the integrated graphics, then start DCS again.";
        if (NvidiaSettings.MfaaForDcs() is { On: true } mfaa)
            return start + $"The cause: NVIDIA's Antialiasing - MFAA is On in {mfaa.Where}. {mfaa.Fix}, then start DCS again.";
        if (NvidiaSettings.ThreadedOptimizationForDcs() is { Value: ThreadedOptimization.Off } threaded)
            return start + $"The cause: NVIDIA's Threaded optimization is Off in {threaded.Where}. {threaded.Fix}, then start DCS again.";
        if (GpuInfo.Adapters().Any(a => a.Vendor == "NVIDIA"))
            return start + "NVIDIA cards have them, so check in NVIDIA Control Panel (Global settings and the DCS World program) that Threaded optimization is not Off and Antialiasing - MFAA is Off, that DCS runs on the NVIDIA card and not on integrated graphics, and that no other program wraps DCS's graphics (ReShade, Special K, overlays).";
        return start + "This graphics driver does not offer them (common on AMD cards).";
    }
    private static string Capabilities(string line) =>
        string.Join(", ", System.Text.RegularExpressions.Regex.Matches(line, @"(DriverCommandLists|ConstantBufferOffsetting|ID3D11DeviceContext1)=0").Select(m => m.Groups[1].Value + " missing")) is { Length: > 0 } list ? list : "see DcsQvCull.log";
    private static string Adapter(string line) =>
        System.Text.RegularExpressions.Regex.Match(line, @" on (?<name>.+?) \(vendor ") is { Success: true } m ? m.Groups["name"].Value : "integrated graphics";

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
