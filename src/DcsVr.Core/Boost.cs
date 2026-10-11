namespace DcsVr.Core;

/// <summary>Parsed <c>DcsVr.Cli boost</c> command line. Pure, so the parser is unit-testable.</summary>
/// <param name="ProfilePath">Explicit profile JSON to boost for; when null the applied profile under the state root is used.</param>
/// <param name="DcsPid">A specific DCS PID to attach to instead of discovering it.</param>
/// <param name="StateRoot">Override for the DcsControl state root (applied profile, status folder).</param>
/// <param name="DcsExecutable">The DCS.exe that was launched (launch.json). Used to find DCS again when the hinted PID
/// exits, e.g. when the Steam edition restarts itself.</param>
/// <param name="DcsLog">The dcs.log next to the applied options.lua, read for the CPU ranking.</param>
public sealed record BoostArguments(string? ProfilePath, int? DcsPid, string? StateRoot, bool Help, string? Error,
    string? DcsExecutable = null, string? DcsLog = null)
{
    public static BoostArguments Parse(IReadOnlyList<string> args)
    {
        string? profile = null, state = null, error = null, exe = null, log = null;
        int? pid = null;
        var help = false;
        for (var i = 0; i < args.Count; i++)
        {
            string? Next() => i + 1 < args.Count ? args[++i] : null;
            switch (args[i])
            {
                case "--profile": profile = Next(); if (profile is null) error ??= "--profile needs a file path."; break;
                case "--state": state = Next(); if (state is null) error ??= "--state needs a folder."; break;
                case "--dcs-exe": exe = Next(); if (string.IsNullOrWhiteSpace(exe)) error ??= "--dcs-exe needs the DCS.exe path."; break;
                case "--dcs-log": log = Next(); if (string.IsNullOrWhiteSpace(log)) error ??= "--dcs-log needs the dcs.log path."; break;
                case "--dcs-pid":
                    var raw = Next();
                    if (raw is null) error ??= "--dcs-pid needs a process id.";
                    else if (!int.TryParse(raw, out var value) || value <= 0) error ??= $"--dcs-pid must be a positive integer, not '{raw}'.";
                    else pid = value;
                    break;
                case "--help" or "-h" or "-?": help = true; break;
                default: error ??= $"Unknown boost option '{args[i]}'."; break;
            }
        }
        return new BoostArguments(profile, pid, state, help, error, exe, log);
    }

    public const string Usage = "Usage: DcsVr.Cli boost [--profile profile.json] [--dcs-pid N] [--dcs-exe DCS.exe] [--dcs-log dcs.log] [--state folder]\n" +
        "Runs the boost helper while DCS is alive: CPU Boost (DCS priority and all CPUs, VR runtime and background apps\n" +
        "confined), Free VRAM before flight (listed programs closed, optionally reopened) and the flight monitor mode, per\n" +
        "the profile; everything restored when DCS exits. Started automatically when such a profile launches DCS.\n" +
        "--dcs-pid is a hint: if that process exits (DCS restarting itself), the DCS started from --dcs-exe is used.\n" +
        "Gives up with status \"timeout\" when no DCS has run long enough within 5 minutes.";
}

/// <summary>A running DCS.exe as seen while waiting to attach. <paramref name="ImagePath"/> is canonical (or null when
/// it could not be read).</summary>
public sealed record DcsCandidate(int Pid, DateTime StartTime, string? ImagePath);

/// <summary>Which running DCS CPU Boost attaches to. Pure, so the Steam self-restart case is unit-testable.</summary>
public static class BoostAttach
{
    /// <summary>The hinted PID wins while it is alive. Once it has exited (the Steam edition restarts itself after a
    /// few seconds as <c>DCS.exe --restarted</c>), any DCS started from <paramref name="executable"/> is accepted;
    /// without a known executable any DCS is. A candidate must have run for <paramref name="minLifetime"/> so a
    /// process about to restart is never chosen. Returns null to keep waiting.</summary>
    public static int? Choose(IReadOnlyList<DcsCandidate> candidates, int? hintPid, string? executable, TimeSpan minLifetime, DateTime now)
    {
        var hintAlive = hintPid is { } hint && candidates.Any(c => c.Pid == hint);
        foreach (var candidate in candidates.OrderBy(c => c.StartTime))
        {
            if (now - candidate.StartTime < minLifetime) continue;
            if (hintAlive) { if (candidate.Pid == hintPid) return candidate.Pid; continue; }
            if (executable is not null && (candidate.ImagePath is null || !string.Equals(candidate.ImagePath, executable, StringComparison.OrdinalIgnoreCase))) continue;
            return candidate.Pid;
        }
        return null;
    }
}

/// <summary>One process state CPU Boost changed, so it can be put back exactly. Matched on restore by PID, name and
/// start time, so a recycled PID is never touched.</summary>
public sealed record BoostChange(
    int Pid, string Name, long StartTicks,
    ulong OriginalAffinity, string OriginalPriority,
    ulong AppliedAffinity, string AppliedPriority);

/// <summary>Bookkeeping of exactly what CPU Boost changed. Pure: it performs no process operations, so the
/// record/restore accounting is unit-testable without real processes.</summary>
public sealed class BoostLedger
{
    // Keyed by PID and start time: a recycled PID is a different process and gets its own record.
    private readonly Dictionary<(int Pid, long StartTicks), BoostChange> _changes = [];
    public IReadOnlyCollection<BoostChange> Changes => _changes.Values;

    /// <summary>True when this exact process (PID and start time) is already recorded.</summary>
    public bool Contains(int pid, long startTicks) => _changes.ContainsKey((pid, startTicks));

    /// <summary>Records a process once. A process started by an already-confined parent inherits one of our masks; that
    /// is not its real original state, so its original is taken as all CPUs (and below-normal as normal). A record for
    /// the same PID with another start time belongs to a process that has exited; it is dropped and the new process
    /// is recorded.</summary>
    public bool TryRecord(int pid, string name, long startTicks, ulong currentAffinity, string currentPriority,
        ulong targetAffinity, string targetPriority, ulong allCpusMask, IReadOnlyCollection<ulong> ourMasks, out BoostChange change)
    {
        if (_changes.TryGetValue((pid, startTicks), out var existing)) { change = existing; return false; }
        foreach (var stale in _changes.Keys.Where(k => k.Pid == pid).ToArray()) _changes.Remove(stale);
        var originalAffinity = currentAffinity;
        var originalPriority = currentPriority;
        if (ourMasks.Contains(currentAffinity))
        {
            originalAffinity = allCpusMask;
            if (string.Equals(originalPriority, "BelowNormal", StringComparison.OrdinalIgnoreCase)) originalPriority = "Normal";
        }
        change = new BoostChange(pid, name, startTicks, originalAffinity, originalPriority, targetAffinity, targetPriority);
        _changes[(pid, startTicks)] = change;
        return true;
    }

    /// <summary>A recorded change may be restored only onto the same process it was taken from.</summary>
    public static bool SameProcess(BoostChange change, string name, long startTicks) =>
        string.Equals(change.Name, name, StringComparison.OrdinalIgnoreCase) && change.StartTicks == startTicks;
}

/// <summary>Status CPU Boost writes to <c>%LOCALAPPDATA%\DcsControl\boost\status.json</c> for the app to read later.</summary>
public sealed record BoostStatus
{
    public int SchemaVersion { get; init; } = 1;
    public string State { get; init; } = "starting";
    public DateTimeOffset UpdatedAt { get; init; } = DateTimeOffset.UtcNow;
    public int? DcsPid { get; init; }
    public CoreRankingSource Source { get; init; }
    public string SourceDescription { get; init; } = "";
    public IReadOnlyList<int> RenderCores { get; init; } = [];
    public IReadOnlyList<int> VrRuntimeCores { get; init; } = [];
    public IReadOnlyList<int> BackgroundCores { get; init; } = [];
    public IReadOnlyList<string> Changed { get; init; } = [];
    public IReadOnlyList<string> Closed { get; init; } = [];
    /// <summary>Programs Free VRAM started again after the flight.</summary>
    public IReadOnlyList<string> Reopened { get; init; } = [];
    /// <summary>What Lower the monitor while flying did last, or null when it is off.</summary>
    public string? Monitor { get; init; }
    /// <summary>What Pause the desktop Tobii eye tracker did last, or null when it is off.</summary>
    public string? Tobii { get; init; }
    public int Restored { get; init; }
    public IReadOnlyList<string> Errors { get; init; } = [];
}
