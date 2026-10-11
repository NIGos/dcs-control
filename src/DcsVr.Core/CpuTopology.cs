using System.Numerics;
using System.Runtime.InteropServices;
using System.Text;
using System.Text.RegularExpressions;

namespace DcsVr.Core;

/// <summary>
/// The logical-CPU ranking CPU Boost uses, derived from DCS's own core classification in dcs.log, or from the
/// Windows scheduler's CPU sets when the log has none. Building it reads files and the OS only; it changes nothing.
/// </summary>
/// <param name="RenderCores">Logical CPUs DCS prefers for its main and render threads (left entirely to DCS).</param>
/// <param name="VrRuntimeCores">Logical CPUs the VR runtime and headset services are confined to (the non-render,
/// "common" cores).</param>
/// <param name="BackgroundCores">The slowest logical CPUs, where background apps are confined: the lowest performance
/// classes, at least two logical CPUs, and never every CPU.</param>
public sealed partial record CpuTopology(
    CoreRankingSource Source,
    string Description,
    int LogicalCount,
    IReadOnlyList<int> RenderCores,
    IReadOnlyList<int> VrRuntimeCores,
    IReadOnlyList<int> BackgroundCores)
{
    /// <summary>Bytes of dcs.log read: DCS writes its "CPU info" block within the first few kilobytes.</summary>
    public const int LogHeadBytes = 64 * 1024;

    /// <summary>The system affinity mask ("all CPUs") this PC really has, when known; 0 when not read (parsed-only
    /// topologies in tests). CPU Boost gives DCS this mask, never one built from the log's highest CPU index.</summary>
    public ulong AllCpusMask { get; init; }

    /// <summary>True when there is a usable ranking: without one, CPU Boost moves nothing by affinity.</summary>
    public bool HasRanking => Source != CoreRankingSource.None && RenderCores.Count > 0 && BackgroundCores.Count > 0;

    public static CpuTopology None(string description) =>
        new(CoreRankingSource.None, description, Environment.ProcessorCount, [], [], []);

    /// <summary>Reads the given dcs.log (then the newest dcs.log under Saved Games); falls back to the Windows CPU
    /// sets; else no ranking. The result is limited to this PC's system affinity mask, and on PCs with several
    /// processor groups (more than 64 logical CPUs) it never ranks, so only DCS priority changes.</summary>
    public static CpuTopology Detect(string? dcsLogPath = null)
    {
        var system = ReadSystemAffinity();
        return LimitToSystem(DetectUnlimited(dcsLogPath), system.Groups, system.Mask, Environment.ProcessorCount);
    }

    static CpuTopology DetectUnlimited(string? dcsLogPath)
    {
        foreach (var log in CandidateLogs(dcsLogPath))
        {
            try
            {
                var head = ReadLogHead(log);
                if (ParseDcsLog(head) is { } fromLog) return fromLog with { Description = fromLog.Description + " (" + log + ")" };
                // The newest log describes this CPU even when it gives no ranking (one performance class): an older log
                // may come from the CPU before an upgrade, so it is not used.
                if (PerformanceClassLine().IsMatch(head) || CoreSetLine().IsMatch(head)) break;
            }
            catch (Exception e) when (e is IOException or UnauthorizedAccessException) { }
        }
        return FromWindows();
    }

    /// <summary>Reads at most <paramref name="maxBytes"/> from the start of a log that DCS may be writing (shared
    /// read/write/delete, so a running DCS never causes a sharing violation).</summary>
    public static string ReadLogHead(string path, int maxBytes = LogHeadBytes)
    {
        using var stream = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete, 4096, FileOptions.SequentialScan);
        var buffer = new byte[maxBytes];
        var total = 0;
        while (total < buffer.Length)
        {
            var read = stream.Read(buffer, total, buffer.Length - total);
            if (read <= 0) break;
            total += read;
        }
        return Encoding.UTF8.GetString(buffer, 0, total);
    }

    static IEnumerable<string> CandidateLogs(string? explicitPath)
    {
        var seen = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        if (!string.IsNullOrWhiteSpace(explicitPath) && seen.Add(Path.GetFullPath(explicitPath))) yield return explicitPath;
        // The CPU classification is per machine, so any recent DCS log on this PC is valid when the selected one
        // is missing. Saved Games may be redirected: use the known folder, then the default location.
        var candidates = new List<string>();
        foreach (var savedGames in SystemPaths.SavedGamesCandidates())
            foreach (var variant in new[] { "DCS", "DCS.openbeta" })
            {
                var path = Path.Combine(savedGames, variant, "Logs", "dcs.log");
                try { if (File.Exists(path) && seen.Add(Path.GetFullPath(path))) candidates.Add(path); } catch (IOException) { }
            }
        // Newest first: the most recent launch classified the CPU this machine actually has.
        foreach (var path in candidates.OrderByDescending(p => { try { return File.GetLastWriteTimeUtc(p); } catch (IOException) { return DateTime.MinValue; } }))
            yield return path;
    }

    [GeneratedRegex(@"logical cores with performance class (\d+): \{([\d, ]*)\}", RegexOptions.IgnoreCase)]
    private static partial Regex PerformanceClassLine();
    [GeneratedRegex(@"(common|render|IO|unavailable) cores: \{([\d, ]*)\}", RegexOptions.IgnoreCase)]
    private static partial Regex CoreSetLine();

    static IReadOnlyList<int> ParseSet(string body) =>
        body.Split(',', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries)
            .Select(token => int.TryParse(token, out var value) ? value : -1).Where(value => value >= 0).Distinct().ToArray();

    /// <summary>Parses the "CPU info" block DCS writes; returns null when the log has no usable ranking.</summary>
    public static CpuTopology? ParseDcsLog(string text)
    {
        var classes = new SortedDictionary<int, IReadOnlyList<int>>();
        foreach (Match match in PerformanceClassLine().Matches(text))
        {
            var cpus = ParseSet(match.Groups[2].Value);
            if (cpus.Count > 0) classes[int.Parse(match.Groups[1].Value)] = cpus;
        }
        IReadOnlyList<int> render = [], common = [];
        foreach (Match match in CoreSetLine().Matches(text))
        {
            var kind = match.Groups[1].Value.ToLowerInvariant();
            var cpus = ParseSet(match.Groups[2].Value);
            if (kind == "render") render = cpus;
            else if (kind == "common") common = cpus;
        }

        var all = classes.Values.SelectMany(v => v).Concat(render).Concat(common).Distinct().OrderBy(v => v).ToArray();
        if (all.Length == 0) return null;
        var logicalCount = all[^1] + 1;

        // A usable ranking needs the CPU to be heterogeneous (more than one performance class) and DCS's own render
        // set to be a real subset. A homogeneous CPU (one class, or render covering everything) gives no ranking.
        if (classes.Count < 2 || render.Count == 0 || render.Count >= all.Length)
            return null;

        // Background: accumulate the lowest performance classes until at least two logical CPUs, never all of them.
        var background = new List<int>();
        foreach (var level in classes.Keys)
        {
            foreach (var cpu in classes[level]) if (!background.Contains(cpu)) background.Add(cpu);
            if (background.Count >= 2) break;
        }
        if (background.Count >= all.Length) background = background.Take(Math.Max(2, all.Length - 1)).ToList();

        // VR runtime: DCS's "common" (non-render) cores; fall back to everything that is not a render core.
        var vrRuntime = common.Count > 0 ? common : all.Where(c => !render.Contains(c)).ToArray();
        return new CpuTopology(CoreRankingSource.DcsLog,
            $"DCS classified {render.Count} render and {vrRuntime.Count} common logical CPUs across {classes.Count} performance classes",
            logicalCount, render, vrRuntime, background);
    }

    /// <summary>Restricts a ranking to what affinity masks can express on this PC. Several processor groups (or more
    /// than 64 logical CPUs) cannot be expressed by a single-group affinity mask, so no ranking is used there and only
    /// DCS priority changes. Otherwise the CPU lists are limited to <paramref name="systemMask"/>, which also becomes
    /// <see cref="AllCpusMask"/>. Pure, so it is unit-testable.</summary>
    internal static CpuTopology LimitToSystem(CpuTopology topology, int processorGroups, ulong systemMask, int processorCount)
    {
        if (processorGroups > 1 || processorCount > 64)
            return None($"This PC has {processorCount} logical CPUs in {Math.Max(1, processorGroups)} processor groups; CPU affinity is not changed on PCs with more than 64 logical CPUs or several processor groups, so only DCS priority changes")
                with { LogicalCount = processorCount };
        if (systemMask == 0) return topology;
        bool Inside(int cpu) => cpu is >= 0 and < 64 && (systemMask & (1UL << cpu)) != 0;
        var count = BitOperations.PopCount(systemMask);
        if (!topology.HasRanking) return topology with { LogicalCount = count, AllCpusMask = systemMask };
        var render = topology.RenderCores.Where(Inside).ToArray();
        var vr = topology.VrRuntimeCores.Where(Inside).ToArray();
        // Background apps never share DCS's render cores (a small CPU's lowest class can reach them), and the headset's
        // services are not squeezed onto fewer than four logical CPUs: then they stay where Windows puts them.
        var background = topology.BackgroundCores.Where(Inside).Where(c => !render.Contains(c)).ToArray();
        if (vr.Length < 4) vr = [];
        if (render.Length == 0 || background.Length == 0 || background.Length >= count)
            return None($"The CPU ranking ({topology.Description}) does not fit this PC's {count} available logical CPUs; CPU affinity will not be changed")
                with { LogicalCount = count, AllCpusMask = systemMask };
        return topology with { LogicalCount = count, AllCpusMask = systemMask, RenderCores = render, VrRuntimeCores = vr, BackgroundCores = background };
    }

    static (int Groups, ulong Mask) ReadSystemAffinity()
    {
        var groups = 1;
        ulong mask = 0;
        try { groups = Math.Max(1, (int)GetActiveProcessorGroupCount()); } catch (Exception e) when (e is DllNotFoundException or EntryPointNotFoundException) { }
        try { if (GetProcessAffinityMask(GetCurrentProcess(), out _, out var system)) mask = (ulong)system; }
        catch (Exception e) when (e is DllNotFoundException or EntryPointNotFoundException) { }
        return (groups, mask);
    }

    /// <summary>Windows fallback: separate logical CPUs by scheduler efficiency class (Intel P/E split). AMD X3D parts
    /// report a single efficiency class, so this yields no ranking there, matching dcs.log's CPPC-only classification.</summary>
    public static CpuTopology FromWindows()
    {
        try
        {
            var sets = ReadCpuSets();
            if (sets.Count == 0) return None("No Windows CPU-set information was available; CPU affinity will not be changed.");
            // Logical processor indexes are per processor group; a single affinity mask covers group 0 only.
            if (sets.Any(s => s.Group != 0))
                return None("Windows reports several processor groups; CPU affinity will not be changed.");
            var byEfficiency = sets.GroupBy(s => s.EfficiencyClass).OrderByDescending(g => g.Key).ToArray();
            var logicalCount = sets.Max(s => s.LogicalProcessor) + 1;
            if (byEfficiency.Length < 2)
                return None($"This CPU reports a single scheduling class across {logicalCount} logical CPUs; CPU affinity will not be changed.");
            var render = byEfficiency[0].Select(s => s.LogicalProcessor).OrderBy(v => v).ToArray();
            var background = new List<int>();
            foreach (var group in byEfficiency.Reverse())
            {
                foreach (var cpu in group.Select(s => s.LogicalProcessor).OrderBy(v => v)) if (!background.Contains(cpu)) background.Add(cpu);
                if (background.Count >= 2) break;
            }
            var all = sets.Select(s => s.LogicalProcessor).Distinct().ToArray();
            if (background.Count >= all.Length) background = background.Take(Math.Max(2, all.Length - 1)).ToList();
            var vrRuntime = all.Where(c => !render.Contains(c)).OrderBy(v => v).ToArray();
            return new CpuTopology(CoreRankingSource.Windows,
                $"Windows reports {byEfficiency.Length} scheduling classes; {render.Length} performance CPUs reserved for DCS",
                logicalCount, render, vrRuntime, background);
        }
        catch (Exception e) when (e is DllNotFoundException or EntryPointNotFoundException or OutOfMemoryException)
        {
            return None("Windows CPU-set information could not be read; CPU affinity will not be changed.");
        }
    }

    private readonly record struct CpuSet(int Group, int LogicalProcessor, int EfficiencyClass);

    static IReadOnlyList<CpuSet> ReadCpuSets()
    {
        GetSystemCpuSetInformation(IntPtr.Zero, 0, out var length, IntPtr.Zero, 0);
        if (length == 0) return [];
        var buffer = Marshal.AllocHGlobal((int)length);
        try
        {
            if (!GetSystemCpuSetInformation(buffer, length, out length, IntPtr.Zero, 0)) return [];
            var result = new List<CpuSet>();
            var offset = 0;
            while (offset + 8 <= length)
            {
                // SYSTEM_CPU_SET_INFORMATION: Size@0 (DWORD), Type@4 (DWORD); for Type==0 (CpuSetInformation) the
                // CPU_SET union follows: Id@8, Group@12 (WORD), LogicalProcessorIndex@14, CoreIndex@15,
                // LastLevelCacheIndex@16, NumaNodeIndex@17, EfficiencyClass@18.
                var size = Marshal.ReadInt32(buffer, offset);
                if (size <= 0 || offset + size > length) break;
                var type = Marshal.ReadInt32(buffer, offset + 4);
                if (type == 0 && size >= 19)
                    result.Add(new CpuSet(Marshal.ReadInt16(buffer, offset + 12), Marshal.ReadByte(buffer, offset + 14), Marshal.ReadByte(buffer, offset + 18)));
                offset += size;
            }
            return result;
        }
        finally { Marshal.FreeHGlobal(buffer); }
    }

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool GetSystemCpuSetInformation(IntPtr information, uint bufferLength, out uint returnedLength, IntPtr process, uint flags);
    [DllImport("kernel32.dll")] private static extern ushort GetActiveProcessorGroupCount();
    [DllImport("kernel32.dll")] private static extern IntPtr GetCurrentProcess();
    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool GetProcessAffinityMask(IntPtr process, out UIntPtr processMask, out UIntPtr systemMask);
}
