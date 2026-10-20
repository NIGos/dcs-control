using System.Collections.Concurrent;
using System.Globalization;
using System.Text.Json;
using System.Text.RegularExpressions;

namespace DcsVr.Core;

/// <summary>Where the last DCS session left its traces. Every source is optional.</summary>
/// <param name="DcsLog">Saved Games\DCS\Logs\dcs.log: the session's start and end (UTC). The prefetch fix's
/// DcsQvCullPrefetch.log is read from the engine optimizations module next to it (Saved Games\DCS\Scripts\DcsQvCull).</param>
/// <param name="PimaxLogDirectory">%LOCALAPPDATA%\Pimax\runtime with pvr_srv_log_*.txt: frames the headset received.</param>
/// <param name="ManagedProfiles">The app's managed\profiles folder: OFXR's flight logs in each profile's ofxr folder.</param>
/// <param name="BoostStatus">%LOCALAPPDATA%\DcsControl\boost\status.json written by the CPU Boost helper.</param>
/// <param name="DcsBin">DCS's bin folder: Cheeky's CheekyFoveatedDLSS logs.</param>
/// <param name="QuadViewsLog">%LOCALAPPDATA%\Quad-Views-Foveated\Quad-Views-Foveated.log, rewritten at each start: whether
/// eye tracking reached the bundled Quad Views.</param>
public sealed record FlightSources(string? DcsLog, string? PimaxLogDirectory = null, string? ManagedProfiles = null, string? BoostStatus = null, string? DcsBin = null,
    string? QuadViewsLog = null);

/// <summary>A summary of the last DCS session, read from logs after DCS exits. Parts without a source are null.</summary>
public sealed record FlightSummary(DateTimeOffset Start, DateTimeOffset End, bool Closed, int? DcsPid, HeadsetFrames? Headset, FramegenShare? Framegen,
    FrameTimes? FrameTime, NeuralState? Dlss5, BoostResult? Boost, PrefetchResult? Prefetch, EyeTrackingState? EyeTracking = null, FrameCap? Cap = null)
{
    public TimeSpan Duration => End - Start;
}
/// <param name="AverageFps">Frames per second DCS (or OFXR) delivered to the Pimax runtime, averaged over the session.</param>
/// <param name="AtRefreshShare">Share of seconds at 97% of the compositor rate or better.</param>
/// <param name="Missed">Frames the runtime had to show without a new application frame.</param>
public sealed record HeadsetFrames(double AverageFps, double CompositorFps, double AtRefreshShare, long Missed, long Discarded, int Seconds);
/// <summary>Seconds OFXR ran at each frame multiplier (from its once-a-second latency_status), and its switches.</summary>
/// <param name="Seconds1x">Seconds at full rate with generation paused (adaptive cadence's 1X).</param>
public sealed record FramegenShare(int Seconds2x, int Seconds3x, int Switches, bool Adaptive, int Seconds1x = 0)
{
    public double Share3x => Seconds2x + Seconds3x == 0 ? 0 : (double)Seconds3x / (Seconds2x + Seconds3x);
}
/// <summary>DCS frame time from OFXR's adaptive control: each second's p90 of max(CPU, GPU) per DCS frame; the median
/// and p90 of those over the session, in milliseconds.</summary>
public sealed record FrameTimes(double P50Ms, double P90Ms, int Seconds);
/// <param name="State">"ran", "not-started" or "failed".</param>
public sealed record NeuralState(string State, string? Version, int Toggles, bool? OnAtExit, string? Error);
public sealed record BoostResult(string State, int Moved, int Restored, IReadOnlyList<string> Errors);
/// <param name="AverageCallsPerSecond">Average over the 10 s reports with any calls.</param>
/// <param name="SkippedPercent">Share of all calls the fix skipped, weighted by calls.</param>
/// <summary>Quad Views' "Eye tracking is supported" or "is not supported" for the last session.</summary>
public sealed record EyeTrackingState(bool Supported);
public sealed record PrefetchResult(double AverageCallsPerSecond, double PeakCallsPerSecond, double SkippedPercent, int Reports);
/// <summary>How long the frame rate was held by the headset runtime (Virtual Desktop's or Meta's motion smoothing at
/// half the refresh rate, for example), from the engine optimizations module's 10 s performance reports. While held,
/// neither CPU nor GPU is the limit and no optimization can raise the frame rate.</summary>
/// <param name="Fps">The rate it was held at, when the module named it (36 for 72 Hz / 2); null otherwise.</param>
/// <param name="RefreshHz">The headset refresh rate it was derived from; null when not named.</param>
/// <param name="Divider">Refresh rate / held rate (2 for 72 Hz / 2); 1 is DCS at the full refresh rate, not a hold.</param>
/// <param name="CappedReports">Reports the module judged capped.</param>
/// <param name="Reports">All reports of the session.</param>
public sealed record FrameCap(double? Fps, double? RefreshHz, int CappedReports, int Reports, int? Divider = null)
{
    public double Share => Reports == 0 ? 0 : (double)CappedReports / Reports;
}

/// <summary>Reads <see cref="FlightSummary"/> defensively and cheaply: only the start and tail of dcs.log, only the
/// lines that matter in the other logs, and each parsed file is cached by size and timestamp.</summary>
public static partial class LastFlight
{
    private static readonly ConcurrentDictionary<string, (long Length, DateTime Written, object? Value)> Cache = new(StringComparer.OrdinalIgnoreCase);
    private const long MaxTail = 64L * 1024 * 1024;

    /// <summary>The last session, or null when dcs.log is missing or unreadable (no flight recorded yet).</summary>
    public static FlightSummary? Read(FlightSources sources)
    {
        if (sources.DcsLog is not { } log || Session(log) is not { } session) return null;
        var (start, end, closed) = session;
        var (ofxrPath, ofxrPid) = FindOfxrLog(sources.ManagedProfiles, start, end);
        // A log still being written or replaced by its program is skipped, never the whole summary.
        var ofxr = Safe(() => ofxrPath is null ? null : ParseOfxr(OfxrLines(ofxrPath, ofxrPid)));
        var boost = Safe(() => sources.BoostStatus is { } status && File.Exists(status) ? Cached(status, () => ParseBoostStatus(File.ReadAllText(status))) : null);
        if (boost is not null && !(boost.UpdatedAt >= start.AddMinutes(-1) && boost.UpdatedAt <= end.AddMinutes(10))) boost = null;
        var pid = ofxrPid ?? boost?.DcsPid;
        return new(start, end, closed, pid,
            ReadPimax(sources.PimaxLogDirectory, start, end, pid),
            ofxr?.Share, ofxr?.Times,
            ReadCheeky(sources.DcsBin, start, end),
            boost?.Result,
            ReadPrefetch(PrefetchLog(log), start, end),
            ReadEyeTracking(sources.QuadViewsLog, start, end),
            ReadFrameCap(ModuleLog(log), start, end));
    }

    // ---- Quad Views --------------------------------------------------------------------------------------------------

    private static EyeTrackingState? ReadEyeTracking(string? path, DateTimeOffset start, DateTimeOffset end)
    {
        try
        {
            if (path is null || !File.Exists(path)) return null;
            return ParseEyeTracking(Cached(path, () => ReadLines(path, 8L * 1024 * 1024, l => l.Contains("Eye tracking is", StringComparison.Ordinal) || l.Contains(" is active", StringComparison.Ordinal))) ?? [], start, end);
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException) { return null; }
    }

    /// <summary>"2026-10-13 15:19:14 +0200: Eye tracking is not supported", once per OpenXR instance: the last one after
    /// the last "layer (v…) is active", within the session (a file can hold several DCS starts, or an older one).</summary>
    public static EyeTrackingState? ParseEyeTracking(IEnumerable<string> lines, DateTimeOffset start, DateTimeOffset end)
    {
        EyeTrackingState? state = null;
        foreach (var line in lines)
        {
            var m = QuadViewsLineRegex().Match(line);
            if (!m.Success || !DateTimeOffset.TryParseExact(m.Groups[1].Value + m.Groups[2].Value.Insert(3, ":"), "yyyy-MM-dd HH:mm:sszzz", CultureInfo.InvariantCulture, DateTimeStyles.None, out var at)) continue;
            if (at < start.AddMinutes(-1) || at > end.AddMinutes(1)) continue;
            var text = m.Groups[3].Value;
            if (text.Contains(" is active", StringComparison.Ordinal) && text.Contains("layer (v", StringComparison.Ordinal)) state = null;
            else if (text.StartsWith("Eye tracking is not supported", StringComparison.Ordinal)) state = new(false);
            else if (text.StartsWith("Eye tracking is supported", StringComparison.Ordinal)) state = new(true);
        }
        return state;
    }
    [GeneratedRegex(@"^(\d{4}-\d\d-\d\d \d\d:\d\d:\d\d) ([+-]\d{4}): (.*)$")] private static partial Regex QuadViewsLineRegex();

    // ---- dcs.log ---------------------------------------------------------------------------------------------------

    /// <summary>The session in dcs.log: "=== Log opened UTC …", the last timestamped line, and whether it closed.</summary>
    private static (DateTimeOffset, DateTimeOffset, bool)? Session(string path)
    {
        try
        {
            if (!File.Exists(path)) return null;
            var info = new FileInfo(path);
            return Cached(path, () =>
            {
                var head = ReadHead(path, 4096); var tail = ReadTail(path, 64 * 1024);
                return ParseDcsSession(head, tail, new DateTimeOffset(info.LastWriteTimeUtc, TimeSpan.Zero));
            });
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException) { return null; }
    }

    /// <summary>Start from the "Log opened UTC" header, end from the last timestamped line (or the file time).</summary>
    public static (DateTimeOffset Start, DateTimeOffset End, bool Closed)? ParseDcsSession(string head, string tail, DateTimeOffset written)
    {
        var opened = OpenedRegex().Match(head);
        if (!opened.Success || !DateTime.TryParseExact(opened.Groups[1].Value, "yyyy-MM-dd HH:mm:ss", CultureInfo.InvariantCulture, DateTimeStyles.AssumeUniversal | DateTimeStyles.AdjustToUniversal, out var startUtc)) return null;
        var start = new DateTimeOffset(startUtc, TimeSpan.Zero);
        var end = written;
        foreach (Match m in StampRegex().Matches(tail))
            if (DateTime.TryParseExact(m.Groups[1].Value, "yyyy-MM-dd HH:mm:ss.fff", CultureInfo.InvariantCulture, DateTimeStyles.AssumeUniversal | DateTimeStyles.AdjustToUniversal, out var t)) end = new DateTimeOffset(t, TimeSpan.Zero);
        if (end < start) end = start;
        return (start, end, tail.Contains("=== Log closed.", StringComparison.Ordinal));
    }
    [GeneratedRegex(@"=== Log opened UTC (\d{4}-\d\d-\d\d \d\d:\d\d:\d\d)")] private static partial Regex OpenedRegex();
    [GeneratedRegex(@"(?m)^(\d{4}-\d\d-\d\d \d\d:\d\d:\d\d\.\d{3}) ")] private static partial Regex StampRegex();

    // ---- Pimax runtime -------------------------------------------------------------------------------------------

    private static HeadsetFrames? ReadPimax(string? directory, DateTimeOffset start, DateTimeOffset end, int? pid)
    {
        try
        {
            if (directory is null || !Directory.Exists(directory)) return null;
            // pvr_srv_log_YY-MM-DD-HH-MM-SS.txt starts at that local time; the one(s) open during the session.
            var files = Directory.EnumerateFiles(directory, "pvr_srv_log_*.txt").Select(f => new FileInfo(f))
                .Where(f => f.LastWriteTimeUtc >= start.UtcDateTime && PimaxFileStart(f.Name) is { } s && s <= end).OrderBy(f => f.Name).ToArray();
            var lines = new List<string>();
            foreach (var file in files.TakeLast(2))
                lines.AddRange(Cached(file.FullName, () => ReadLines(file.FullName, MaxTail, l => l.Contains("rendering fps:", StringComparison.Ordinal))) ?? []);
            return ParsePimax(lines, start, end, pid);
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException) { return null; }
    }
    private static DateTimeOffset? PimaxFileStart(string name)
    {
        var m = PimaxFileRegex().Match(name);
        return m.Success && DateTime.TryParseExact(m.Groups[1].Value, "yy-MM-dd-HH-mm-ss", CultureInfo.InvariantCulture, DateTimeStyles.AssumeLocal, out var t) ? new DateTimeOffset(t) : null;
    }
    [GeneratedRegex(@"^pvr_srv_log_(\d\d-\d\d-\d\d-\d\d-\d\d-\d\d)\.txt$", RegexOptions.IgnoreCase)] private static partial Regex PimaxFileRegex();

    /// <summary>Pimax's once-a-second "rendering fps:(a:…,c:…) Missed:(a:…) … Discard:(a:…)" lines of the DCS process
    /// (the number after [PSRV]; any non-zero process when the PID is unknown) within the session, skipping seconds
    /// below 1 FPS (loading). Times are local.</summary>
    public static HeadsetFrames? ParsePimax(IEnumerable<string> lines, DateTimeOffset start, DateTimeOffset end, int? pid)
    {
        double app = 0, compositor = 0; long missed = 0, discarded = 0; int seconds = 0, atRefresh = 0;
        foreach (var line in lines)
        {
            var m = PimaxLineRegex().Match(line);
            if (!m.Success || !DateTime.TryParseExact(m.Groups[1].Value, "yy-MM-dd HH:mm:ss.fff", CultureInfo.InvariantCulture, DateTimeStyles.AssumeLocal, out var local)) continue;
            var at = new DateTimeOffset(local);
            if (at < start || at > end) continue;
            if (!int.TryParse(m.Groups[2].Value, NumberStyles.None, CultureInfo.InvariantCulture, out var process)) continue;
            if (pid is { } p ? process != p : process == 0) continue;
            if (Number(m.Groups[3].Value) is not { } a || Number(m.Groups[4].Value) is not { } c
                || !long.TryParse(m.Groups[5].Value, NumberStyles.None, CultureInfo.InvariantCulture, out var missedNow)
                || !long.TryParse(m.Groups[6].Value, NumberStyles.None, CultureInfo.InvariantCulture, out var discardedNow)) continue;
            // Loading screens submit next to nothing; those seconds are not flight.
            if (a < 1) continue;
            seconds++; app += a; compositor += c;
            if (c > 0 && a >= c * .97) atRefresh++;
            missed += missedNow; discarded += discardedNow;
        }
        return seconds == 0 ? null : new(app / seconds, compositor / seconds, (double)atRefresh / seconds, missed, discarded, seconds);
    }
    [GeneratedRegex(@"^\[(\d\d-\d\d-\d\d \d\d:\d\d:\d\d\.\d{3})\].*?\[PSRV\]\s+(\d+)\s+\w+\s+rendering fps:\(a:([\d.]+),c:([\d.]+)\)\s+Missed:\(a:(\d+)[^)]*\).*?Discard:\(a:(\d+)\)")]
    private static partial Regex PimaxLineRegex();

    // ---- OFXR flight log -------------------------------------------------------------------------------------------

    /// <summary>ofxr-bridge-flight-YYYYMMDD-HHMMSS-pidN.log (local start time) started during the session.</summary>
    private static (string?, int?) FindOfxrLog(string? profiles, DateTimeOffset start, DateTimeOffset end)
    {
        try
        {
            if (profiles is null || !Directory.Exists(profiles)) return (null, null);
            var best = Directory.EnumerateDirectories(profiles).Select(d => Path.Combine(d, "ofxr")).Where(Directory.Exists)
                .SelectMany(d => Directory.EnumerateFiles(d, "ofxr-bridge-flight-*.log"))
                .Select(f => (Path: f, Match: OfxrFileRegex().Match(Path.GetFileName(f))))
                .Where(x => x.Match.Success && DateTime.TryParseExact(x.Match.Groups[1].Value, "yyyyMMdd-HHmmss", CultureInfo.InvariantCulture, DateTimeStyles.AssumeLocal, out var t)
                    && new DateTimeOffset(t) >= start.AddMinutes(-2) && new DateTimeOffset(t) <= end)
                .OrderBy(x => x.Match.Groups[1].Value, StringComparer.Ordinal).LastOrDefault();
            return best.Path is null ? (null, null) : (best.Path, int.TryParse(best.Match.Groups[2].Value, NumberStyles.None, CultureInfo.InvariantCulture, out var pid) ? pid : null);
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException) { return (null, null); }
    }
    [GeneratedRegex(@"^ofxr-bridge-flight-(\d{8}-\d{6})-pid(\d+)\.log$", RegexOptions.IgnoreCase)] private static partial Regex OfxrFileRegex();

    /// <summary>Deletes all but the newest <paramref name="keep"/> OFXR flight logs (with their .prev segments) across
    /// the profiles: every DCS start writes one, up to twice its size cap, and only the last flight is read. A log in
    /// use is left alone. Returns how many files were deleted.</summary>
    public static int PruneOfxrLogs(string? profiles, int keep = 8)
    {
        var deleted = 0;
        try
        {
            if (profiles is null || !Directory.Exists(profiles)) return 0;
            var old = Directory.EnumerateDirectories(profiles).Select(d => Path.Combine(d, "ofxr")).Where(Directory.Exists)
                .SelectMany(d => Directory.EnumerateFiles(d, "ofxr-bridge-flight-*.log"))
                .Select(f => (Path: f, Match: OfxrFileRegex().Match(Path.GetFileName(f))))
                .Where(x => x.Match.Success)
                .OrderByDescending(x => x.Match.Groups[1].Value, StringComparer.Ordinal).ThenByDescending(x => x.Path, StringComparer.OrdinalIgnoreCase)
                .Skip(keep).Select(x => x.Path).ToList();
            foreach (var log in old)
                foreach (var file in new[] { OfxrPreviousSegment(log), log })
                    try { if (File.Exists(file)) { File.Delete(file); deleted++; } }
                    catch (Exception e) when (e is IOException or UnauthorizedAccessException) { }
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException) { }
        return deleted;
    }

    /// <summary>Deletes the smooth cursor's diagnostic captures (cursor-capture-&lt;pid&gt;-&lt;n&gt;-*.bmp and -info.txt, OFXR
    /// patch 0011) of all but the newest <paramref name="keep"/> DCS processes across the profiles: with the flight log on,
    /// every DCS start adds up to 8 sets of about 3 MB, and nothing in the app reads them. The newest process's captures
    /// always stay; a file in use is left alone. Returns how many files were deleted.</summary>
    public static int PruneCursorCaptures(string? profiles, int keep = 4)
    {
        var deleted = 0;
        try
        {
            if (profiles is null || !Directory.Exists(profiles)) return 0;
            var old = Directory.EnumerateDirectories(profiles).Select(d => Path.Combine(d, "ofxr")).Where(Directory.Exists)
                .SelectMany(d => new DirectoryInfo(d).EnumerateFiles("cursor-capture-*"))
                .Select(f => (File: f, Match: CursorCaptureRegex().Match(f.Name)))
                .Where(x => x.Match.Success)
                .GroupBy(x => x.File.DirectoryName + "|" + x.Match.Groups[1].Value, StringComparer.OrdinalIgnoreCase)
                .OrderByDescending(g => g.Max(x => x.File.LastWriteTimeUtc)).ThenByDescending(g => g.Key, StringComparer.OrdinalIgnoreCase)
                .Skip(Math.Max(1, keep)).SelectMany(g => g.Select(x => x.File)).ToList();
            foreach (var file in old)
                try { file.Delete(); deleted++; }
                catch (Exception e) when (e is IOException or UnauthorizedAccessException) { }
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException) { }
        return deleted;
    }
    [GeneratedRegex(@"^cursor-capture-(\d+)-\d+-(?:previous\.bmp|current\.bmp|overview\.bmp|info\.txt)$", RegexOptions.IgnoreCase)] private static partial Regex CursorCaptureRegex();

    /// <summary>The flight log's summary lines, after those of its .prev segment when that belongs to the same session:
    /// at its size cap OFXR renames the log to &lt;name&gt;.prev.log and starts it again (fork patch 0046), so a long
    /// flight is in both. The segment counts only when its header record names the same process.</summary>
    private static List<string>? OfxrLines(string path, int? pid)
    {
        static bool Keep(string l) => l.Contains("op=latency_status ", StringComparison.Ordinal) || l.Contains("op=adaptive_switch ", StringComparison.Ordinal);
        var current = Cached(path, () => ReadLines(path, MaxTail, Keep));
        var previous = OfxrPreviousSegment(path);
        // The earlier segment is optional: unreadable or another process's, the summary covers the current one.
        List<string>? earlier = null;
        if (pid is { } p && File.Exists(previous))
            earlier = Safe(() => OfxrHeaderPid(ReadHead(previous, 512)) == p ? Cached(previous, () => ReadLines(previous, MaxTail, Keep)) : null);
        return earlier is null ? current : current is null ? earlier : [.. earlier, .. current];
    }

    /// <summary>ofxr-bridge-flight-…-pidN.log → ofxr-bridge-flight-…-pidN.prev.log, the segment OFXR rotated out last.</summary>
    public static string OfxrPreviousSegment(string path) => Path.Combine(Path.GetDirectoryName(path) ?? "", Path.GetFileNameWithoutExtension(path) + ".prev" + Path.GetExtension(path));

    /// <summary>The process id in a flight log's header record (its first line: op=logger, c = the process id), or null.</summary>
    public static int? OfxrHeaderPid(string head)
    {
        var end = head.IndexOf('\n');
        var m = OfxrHeaderRegex().Match(end < 0 ? head : head[..end]);
        return m.Success && int.TryParse(m.Groups[1].Value, NumberStyles.None, CultureInfo.InvariantCulture, out var pid) ? pid : null;
    }
    [GeneratedRegex(@"\bphase=I op=logger result=-?\d+ dur_us=\d+ a=\d+ b=\d+ c=(\d+)\r?$")] private static partial Regex OfxrHeaderRegex();

    public sealed record OfxrResult(FramegenShare? Share, FrameTimes? Times);

    /// <summary>latency_status (once a second: result = multiplier, +10 under adaptive control; a = that second's p90 of
    /// max(CPU, GPU) per DCS frame in microseconds) and adaptive_switch (a multiplier change, or adaptive turned on/off).</summary>
    public static OfxrResult? ParseOfxr(IEnumerable<string>? lines)
    {
        if (lines is null) return null;
        int one = 0, two = 0, three = 0, switches = 0; var adaptive = false; var frames = new List<double>();
        foreach (var line in lines)
        {
            var m = OfxrLineRegex().Match(line);
            if (!m.Success) continue;
            if (!long.TryParse(m.Groups[2].Value, NumberStyles.AllowLeadingSign, CultureInfo.InvariantCulture, out var result)
                || !ulong.TryParse(m.Groups[3].Value, NumberStyles.None, CultureInfo.InvariantCulture, out var a)
                || !ulong.TryParse(m.Groups[4].Value, NumberStyles.None, CultureInfo.InvariantCulture, out var b)) continue;
            if (m.Groups[1].Value == "latency_status")
            {
                adaptive |= result >= 10;
                // 11 is adaptive cadence at 1X (generation paused at full rate).
                if (result == 11) one++; else if (result % 10 == 2) two++; else if (result % 10 == 3) three++;
                if (a > 0 && a < 1_000_000) frames.Add(a / 1000.0);
            }
            // Reason 5 is adaptive control turned on or off; the others change the multiplier between 2 and 3. Adaptive
            // cadence: 8 enters 1X, 9-12 leave it; 13 turns cadence on or off.
            else if (result is >= 8 and <= 12) switches++;
            // 13 with a and b both 2 or 3 is cadence moving the session between 3X and 2X; with 0 it only turns cadence on or off.
            else if (result != 5 && a != b && a is 2 or 3 && b is 2 or 3) switches++;
        }
        if (one + two + three == 0 && frames.Count == 0) return null;
        frames.Sort();
        return new(one + two + three == 0 ? null : new(two, three, switches, adaptive, one), frames.Count == 0 ? null : new(Percentile(frames, .5), Percentile(frames, .9), frames.Count));
    }
    [GeneratedRegex(@"\bop=(latency_status|adaptive_switch) result=(-?\d+) dur_us=\d+ a=(\d+) b=(\d+)")] private static partial Regex OfxrLineRegex();
    private static double Percentile(List<double> sorted, double q) => Math.Round(sorted[Math.Clamp((int)Math.Ceiling(q * sorted.Count) - 1, 0, sorted.Count - 1)], 1);

    // ---- Cheeky (DLSS 5) -------------------------------------------------------------------------------------------

    private static NeuralState? ReadCheeky(string? bin, DateTimeOffset start, DateTimeOffset end)
    {
        try
        {
            if (bin is null) return null;
            var folder = Path.Combine(bin, "CheekyFoveatedDLSS");
            var standalone = Path.Combine(folder, "CheekyFoveatedDLSS-Standalone.log"); var host = Path.Combine(folder, "CheekyFoveatedDLSS-Host.log");
            bool During(string f) => File.Exists(f) && File.GetLastWriteTimeUtc(f) >= start.UtcDateTime.AddMinutes(-1) && File.GetLastWriteTimeUtc(f) <= end.UtcDateTime.AddMinutes(10);
            if (!During(standalone)) return null;
            // Every session appends to these logs, with the time of day only: the startup markers are kept so only the
            // last session's lines count.
            var runtime = Cached(standalone, () => ReadLines(standalone, 16L * 1024 * 1024, l => IsRuntimeStart(l) || (l.Contains("DLSS-NR", StringComparison.Ordinal) && (l.Contains("runtime initialized", StringComparison.Ordinal) || NeuralFailedRegex().IsMatch(l) || NeuralErrorRegex().IsMatch(l)))));
            var toggles = During(host) ? Cached(host, () => ReadLines(host, 4L * 1024 * 1024, l => IsHostStart(l) || l.Contains("hotkey toggled", StringComparison.Ordinal))) : null;
            return ParseCheeky(runtime ?? [], toggles ?? []);
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException) { return null; }
    }

    /// <summary>"DLSS-NR 310.8 feature-18 runtime initialized", DLSS-NR failures, and "DLSS-NR hotkey toggled enabled=yes|no",
    /// counted only after the last startup marker of each log ("Cheeky 0.5.3 Standalone initializing; …" in the runtime
    /// log, "Initializing standalone host" in the host log): every DCS session appends to both files.</summary>
    public static NeuralState ParseCheeky(IEnumerable<string> runtime, IEnumerable<string> host)
    {
        string? version = null, error = null;
        runtime = LastSession(runtime, IsRuntimeStart); host = LastSession(host, IsHostStart);
        foreach (var line in runtime)
        {
            if (NeuralInitRegex().Match(line) is { Success: true } init) version ??= init.Groups[1].Value;
            else if (error is null && line.Contains("DLSS-NR", StringComparison.Ordinal) && (NeuralFailedRegex().IsMatch(line) || NeuralErrorRegex().IsMatch(line)))
                error = line.Length > 40 && line.IndexOf("DLSS-NR", StringComparison.Ordinal) is var at and >= 0 ? line[at..].Trim() : line.Trim();
        }
        int toggles = 0; bool? on = null;
        foreach (var line in host)
            if (line.Contains("hotkey toggled enabled=", StringComparison.Ordinal)) { toggles++; on = line.TrimEnd().EndsWith("enabled=yes", StringComparison.Ordinal); }
        if (error is { Length: > 160 }) error = error[..157] + "…";
        return new(version is not null && error is null ? "ran" : error is not null ? "failed" : "not-started", version, toggles, on, error);
    }
    /// <summary>Cheeky's line when the DLSS 5 runtime starts; group 1 is the runtime version. Also read by <see cref="Diagnostics"/>.</summary>
    [GeneratedRegex(@"DLSS-NR (\S+) feature-\d+ runtime initialized")] internal static partial Regex NeuralInitRegex();
    /// <summary>The lines after the last line that matches <paramref name="start"/>; all lines when none does (a log
    /// from before the markers, or one whose start is beyond the read limit).</summary>
    private static List<string> LastSession(IEnumerable<string> lines, Func<string, bool> start)
    {
        var list = lines as List<string> ?? lines.ToList();
        var last = list.FindLastIndex(l => start(l));
        return last < 0 ? list : list.GetRange(last + 1, list.Count - last - 1);
    }
    private static bool IsRuntimeStart(string line) => line.Contains("Cheeky ", StringComparison.Ordinal) && line.Contains(" initializing; runtime remains resident", StringComparison.Ordinal);
    private static bool IsHostStart(string line) => line.Contains("Initializing standalone host", StringComparison.Ordinal) || line.Contains("Initializing OptiScaler host", StringComparison.Ordinal);
    [GeneratedRegex(@"\berror=(?!0\b)-?\d+")] private static partial Regex NeuralErrorRegex();
    [GeneratedRegex(@"\b(failed|failure)\b", RegexOptions.IgnoreCase)] private static partial Regex NeuralFailedRegex();

    // ---- CPU Boost ---------------------------------------------------------------------------------------------------

    public sealed record BoostStatus(DateTimeOffset UpdatedAt, int? DcsPid, BoostResult Result);

    /// <summary>status.json of the CPU Boost helper: state, what it changed and restored, its errors.</summary>
    public static BoostStatus? ParseBoostStatus(string json)
    {
        try
        {
            using var doc = JsonDocument.Parse(json);
            var r = doc.RootElement;
            if (!r.TryGetProperty("updatedAt", out var updated) || !updated.TryGetDateTimeOffset(out var at)) return null;
            static string[] List(JsonElement root, string name) => root.TryGetProperty(name, out var e) && e.ValueKind == JsonValueKind.Array ? [.. e.EnumerateArray().Select(x => x.GetString() ?? "")] : [];
            var changed = List(r, "changed");
            var restored = r.TryGetProperty("restored", out var re) && re.ValueKind == JsonValueKind.Number && re.TryGetInt32(out var count) ? count : 0;
            var state = r.TryGetProperty("state", out var st) ? st.GetString() ?? "" : "";
            var pid = r.TryGetProperty("dcsPid", out var p) && p.ValueKind == JsonValueKind.Number && p.TryGetInt32(out var number) ? number : (int?)null;
            // DCS's own priority and affinity are listed first as "DCS (PID n): …"; the rest are other processes moved.
            var moved = changed.Count(c => !c.StartsWith("DCS (PID", StringComparison.Ordinal));
            return new(at, pid, new(state, moved, restored, List(r, "errors")));
        }
        catch (Exception e) when (e is JsonException or InvalidOperationException or FormatException) { return null; }
    }

    // ---- Prefetch fix ------------------------------------------------------------------------------------------------

    /// <summary>The engine optimizations module's DcsQvCullPrefetch.log for the Saved Games folder of
    /// <paramref name="dcsLog"/> (Saved Games\DCS*\Logs\dcs.log → Saved Games\DCS*\Scripts\DcsQvCull).</summary>
    public static string? PrefetchLog(string dcsLog) =>
        Path.GetDirectoryName(Path.GetDirectoryName(Path.GetFullPath(dcsLog))) is { } savedGames ? Path.Combine(savedGames, "Scripts", "DcsQvCull", "DcsQvCullPrefetch.log") : null;

    private static PrefetchResult? ReadPrefetch(string? path, DateTimeOffset start, DateTimeOffset end)
    {
        try { return path is not null && File.Exists(path) ? ParsePrefetch(Cached(path, () => ReadLines(path, 8L * 1024 * 1024, l => l.Contains("calls/s=", StringComparison.Ordinal))) ?? [], start, end) : null; }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException) { return null; }
    }

    /// <summary>"2026-10-05 19:41:45 calls/s=22239 skipped=84.2% …" every 10 s, local time, within the session.</summary>
    public static PrefetchResult? ParsePrefetch(IEnumerable<string> lines, DateTimeOffset start, DateTimeOffset end)
    {
        double total = 0, skipped = 0, peak = 0; int reports = 0, active = 0;
        foreach (var line in lines)
        {
            var m = PrefetchRegex().Match(line);
            if (!m.Success || !DateTime.TryParseExact(m.Groups[1].Value, "yyyy-MM-dd HH:mm:ss", CultureInfo.InvariantCulture, DateTimeStyles.AssumeLocal, out var local)) continue;
            var at = new DateTimeOffset(local);
            if (at < start.AddMinutes(-1) || at > end.AddMinutes(1)) continue;
            if (Number(m.Groups[2].Value) is not { } calls || Number(m.Groups[3].Value) is not { } share) continue;
            reports++;
            if (calls <= 0) continue;
            active++; total += calls; skipped += calls * share / 100; peak = Math.Max(peak, calls);
        }
        return reports == 0 ? null : new(active == 0 ? 0 : total / active, peak, total == 0 ? 0 : Math.Round(skipped / total * 100, 1), reports);
    }
    [GeneratedRegex(@"^(\d{4}-\d\d-\d\d \d\d:\d\d:\d\d) calls/s=(\d+(?:\.\d+)?) skipped=(\d+(?:\.\d+)?)%")] private static partial Regex PrefetchRegex();

    // ---- Frame rate held by the runtime ------------------------------------------------------------------------------

    /// <summary>The engine optimizations module's DcsQvCull.log next to <paramref name="dcsLog"/>'s Saved Games folder.</summary>
    public static string? ModuleLog(string dcsLog) =>
        Path.GetDirectoryName(Path.GetDirectoryName(Path.GetFullPath(dcsLog))) is { } savedGames ? EngineOptimizations.ModuleLog(Path.Combine(savedGames, "Scripts", "DcsQvCull")) : null;

    private static FrameCap? ReadFrameCap(string? path, DateTimeOffset start, DateTimeOffset end)
    {
        try
        {
            if (path is null || !File.Exists(path)) return null;
            // The module starts its log afresh with each DCS start and stamps lines with the time of day only, so the
            // log belongs to this session when it was last written during it.
            var written = new DateTimeOffset(File.GetLastWriteTimeUtc(path), TimeSpan.Zero);
            if (written < start.AddMinutes(-1) || written > end.AddMinutes(10)) return null;
            return ParseFrameCap(Cached(path, () => ReadLines(path, 8L * 1024 * 1024, l => l.Contains(" perf: fps=", StringComparison.Ordinal) || l.Contains("governor: capped at", StringComparison.Ordinal) || l.Contains("loader started", StringComparison.Ordinal))) ?? []);
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException) { return null; }
    }

    /// <summary>"perf: fps=35.4 … verdict=capped 36.0 fps = 72/2 (…)" every 10 s, and "governor: capped at 36.0 fps
    /// (72 Hz / 2)"; only the lines after the last loader start count (one DCS session). Null without reports.</summary>
    public static FrameCap? ParseFrameCap(IEnumerable<string> lines)
    {
        int reports = 0, capped = 0; double? fps = null, hz = null; int? divider = null;
        foreach (var line in lines)
        {
            if (line.Contains("loader started", StringComparison.Ordinal)) { reports = 0; capped = 0; fps = null; hz = null; divider = null; continue; }
            if (GovernorCapRegex().Match(line) is { Success: true } g)
            {
                if (Number(Dot(g.Groups[1].Value)) is { } held && Number(Dot(g.Groups[2].Value)) is { } headset) { fps = held; hz = headset; divider = Divide(g.Groups[3].Value); }
                continue;
            }
            var m = PerfRegex().Match(line);
            if (!m.Success) continue;
            reports++;
            if (!m.Groups[1].Value.Equals("capped", StringComparison.OrdinalIgnoreCase)) continue;
            capped++;
            if (Number(Dot(m.Groups[2].Value)) is { } rate && Number(Dot(m.Groups[3].Value)) is { } refresh) { fps = rate; hz = refresh; divider = Divide(m.Groups[4].Value); }
        }
        return reports == 0 ? null : new(fps, hz, capped, reports, divider);
        // Payloads before R62 wrote numbers in the user's locale ("36,0" in comma-decimal regions).
        static string Dot(string text) => text.Replace(',', '.');
        static int? Divide(string text) => int.TryParse(text, NumberStyles.None, CultureInfo.InvariantCulture, out var n) && n is >= 1 and <= 8 ? n : null;
    }
    [GeneratedRegex(@" perf: fps=\S+ .* verdict=([A-Za-z]+)(?: (\d+(?:[.,]\d+)?) fps = (\d+(?:[.,]\d+)?)/(\d+))?")] private static partial Regex PerfRegex();
    [GeneratedRegex(@"governor: capped at (\d+(?:[.,]\d+)?) fps \((\d+(?:[.,]\d+)?) Hz / (\d+)\)")] private static partial Regex GovernorCapRegex();

    // ---- Reading ---------------------------------------------------------------------------------------------------

    private static T? Safe<T>(Func<T?> read) where T : class
    {
        try { return read(); }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException) { return null; }
    }

    /// <summary>A finite number, or null: log lines are never trusted to be well formed.</summary>
    private static double? Number(string text) => double.TryParse(text, NumberStyles.Float, CultureInfo.InvariantCulture, out var value) && double.IsFinite(value) ? value : null;

    /// <summary>The parsed value for this file's size and timestamp; parsed again only when the file changes.</summary>
    private static T? Cached<T>(string path, Func<T?> parse)
    {
        var info = new FileInfo(path);
        if (!info.Exists) return default;
        if (Cache.TryGetValue(path + "|" + typeof(T).Name, out var hit) && hit.Length == info.Length && hit.Written == info.LastWriteTimeUtc) return (T?)hit.Value;
        var value = parse();
        Cache[path + "|" + typeof(T).Name] = (info.Length, info.LastWriteTimeUtc, value);
        return value;
    }

    /// <summary>Lines of the last <paramref name="maxBytes"/> of a file that pass <paramref name="keep"/>; the file may
    /// still be written by another process.</summary>
    private static List<string> ReadLines(string path, long maxBytes, Func<string, bool> keep)
    {
        var lines = new List<string>();
        using var stream = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete, 1 << 16, FileOptions.SequentialScan);
        var skipFirst = stream.Length > maxBytes;
        if (skipFirst) stream.Seek(-maxBytes, SeekOrigin.End);
        using var reader = new StreamReader(stream, System.Text.Encoding.UTF8, detectEncodingFromByteOrderMarks: true, 1 << 16);
        if (skipFirst) reader.ReadLine();
        while (reader.ReadLine() is { } line) if (keep(line)) lines.Add(line);
        return lines;
    }
    private static string ReadHead(string path, int bytes)
    {
        using var stream = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete);
        var buffer = new byte[Math.Min(bytes, stream.Length)]; stream.ReadExactly(buffer);
        return System.Text.Encoding.UTF8.GetString(buffer);
    }
    private static string ReadTail(string path, int bytes)
    {
        using var stream = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete);
        var length = (int)Math.Min(bytes, stream.Length); stream.Seek(-length, SeekOrigin.End);
        var buffer = new byte[length]; stream.ReadExactly(buffer);
        return System.Text.Encoding.UTF8.GetString(buffer);
    }
}
