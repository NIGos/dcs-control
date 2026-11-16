using System.Diagnostics;

namespace DcsVr.Core;

/// <summary>One running DCS.exe: its process id and full image path (null when it cannot be read).</summary>
public sealed record RunningDcs(int Pid, string? ImagePath);

public enum DcsHealth { NotRunning, Healthy, NoWindow, NotResponding }

/// <summary>Says when a running DCS has looked stuck for long enough to offer ending it. DCS has no window for a few
/// seconds while it starts and stops answering Windows for minutes while a mission loads, so each state has its own
/// patience; any healthy reading starts again. So does a pause in the readings: DCS may have answered, closed or started
/// again in between. <see cref="DcsStuckWatch"/> keeps the readings coming while the page is hidden or busy.</summary>
public sealed class DcsStuckTracker
{
    public static readonly TimeSpan NoWindowPatience = TimeSpan.FromSeconds(30), NotRespondingPatience = TimeSpan.FromMinutes(4);
    /// <summary>Readings further apart than this are not one stretch of the same state.</summary>
    public static readonly TimeSpan MaxGap = TimeSpan.FromSeconds(20);
    private DcsHealth _state = DcsHealth.NotRunning; private DateTimeOffset _since, _last;

    /// <summary>Null while DCS is fine or not yet stuck for long enough; otherwise why it is offered.</summary>
    public DcsHealth? Update(DcsHealth health, DateTimeOffset now)
    {
        if (health != _state || now - _last > MaxGap) { _state = health; _since = now; }
        _last = now;
        return health switch
        {
            DcsHealth.NoWindow when now - _since >= NoWindowPatience => DcsHealth.NoWindow,
            DcsHealth.NotResponding when now - _since >= NotRespondingPatience => DcsHealth.NotResponding,
            _ => null
        };
    }
}

/// <summary>Reads how DCS looks on its own clock, for as long as the app runs. The page asks every few seconds, but not
/// while the window is minimised (the page is suspended) or a command runs: left to the page alone, the patience would
/// start again each time the window comes back, and a pilot who looks now and then would never be told (GitHub #21).</summary>
public sealed class DcsStuckWatch : IDisposable
{
    /// <summary>Well inside <see cref="DcsStuckTracker.MaxGap"/>, also when one reading waits for a DCS that does not answer.</summary>
    public static readonly TimeSpan Interval = TimeSpan.FromSeconds(8);
    private readonly DcsStuckTracker _tracker = new(); private readonly Func<DateTimeOffset> _now; private readonly Timer? _timer;
    private DcsHealth? _last; private int _reading;

    /// <param name="interval">Between the watch's own readings; <see cref="Timeout.InfiniteTimeSpan"/> for none (a test calls <see cref="Read"/> itself).</param>
    /// <param name="now">The clock, replaceable for tests.</param>
    public DcsStuckWatch(TimeSpan? interval = null, Func<DateTimeOffset>? now = null)
    {
        _now = now ?? (() => DateTimeOffset.UtcNow);
        var every = interval ?? Interval;
        if (every != Timeout.InfiniteTimeSpan) _timer = new Timer(_ => Tick(), null, every, every);
    }

    /// <summary>What the last reading said: null while DCS is fine, gone or not yet stuck for long enough.</summary>
    public DcsHealth? Last { get { lock (_tracker) return _last; } }

    /// <summary>Reads DCS now. <paramref name="running"/>: what the caller already knows, so the processes are not listed twice.</summary>
    public DcsHealth? Read(bool? running = null)
    {
        var health = (running ?? DcsProcess.AnyRunning()) ? DcsProcess.Health() : DcsHealth.NotRunning;
        lock (_tracker) return _last = _tracker.Update(health, _now());
    }

    private void Tick()
    {
        // One reading at a time: asking a DCS that does not answer can take seconds.
        if (Interlocked.Exchange(ref _reading, 1) != 0) return;
        // A timer thread has nobody to report to: a reading that fails is missed, and the next one counts.
        try { Read(); }
        catch (Exception e) when (e is System.ComponentModel.Win32Exception or InvalidOperationException or NotSupportedException or UnauthorizedAccessException) { }
        finally { Volatile.Write(ref _reading, 0); }
    }

    public void Dispose() => _timer?.Dispose();
}

/// <summary>The running DCS processes, in one place: every "is DCS running" check asks here.</summary>
public static class DcsProcess
{
    /// <summary>Replaceable so tests never depend on a DCS running on the build machine.</summary>
    public static Func<IReadOnlyList<RunningDcs>> Provider { get; set; } = ReadRunning;

    public static IReadOnlyList<RunningDcs> Running() => Provider();

    public static bool AnyRunning() => Running().Count > 0;

    /// <summary>How the running DCS looks from outside. Replaceable for tests, like <see cref="Provider"/>.</summary>
    public static Func<DcsHealth> HealthProvider { get; set; } = ReadHealth;
    public static DcsHealth Health() => HealthProvider();

    /// <summary>Ends every DCS.exe (the user asked for it, for a DCS that is stuck) and waits a moment for them to go.
    /// Returns how many were ended.</summary>
    public static int EndAll()
    {
        var processes = Process.GetProcessesByName("DCS"); var ended = 0;
        try
        {
            foreach (var process in processes)
                try { process.Kill(); if (process.WaitForExit(5000)) ended++; }
                catch (Exception e) when (e is System.ComponentModel.Win32Exception or InvalidOperationException or NotSupportedException) { }
            return ended;
        }
        finally { foreach (var process in processes) process.Dispose(); }
    }

    /// <summary>Ends DCS as <see cref="EndAll"/> does, unless it looks healthy now: it was offered because it looked stuck,
    /// and a DCS that only took long to load a mission may answer again before the pilot confirms. Null: left running.</summary>
    public static int? EndStuck() => Health() == DcsHealth.Healthy ? null : EndAll();

    /// <summary>A DCS with no window at all (what ending it from Task Manager's app list can leave behind, GitHub #21),
    /// or whose window does not answer Windows. One healthy DCS among several counts as healthy.</summary>
    private static DcsHealth ReadHealth()
    {
        var processes = Process.GetProcessesByName("DCS");
        try
        {
            if (processes.Length == 0) return DcsHealth.NotRunning;
            var worst = DcsHealth.NoWindow;
            foreach (var process in processes)
            {
                DcsHealth one;
                try { one = process.MainWindowHandle == IntPtr.Zero ? DcsHealth.NoWindow : process.Responding ? DcsHealth.Healthy : DcsHealth.NotResponding; }
                catch (Exception e) when (e is System.ComponentModel.Win32Exception or InvalidOperationException or NotSupportedException) { one = DcsHealth.Healthy; }
                if (one == DcsHealth.Healthy) return DcsHealth.Healthy;
                if (one == DcsHealth.NotResponding) worst = DcsHealth.NotResponding;
            }
            return worst;
        }
        finally { foreach (var process in processes) process.Dispose(); }
    }

    private static IReadOnlyList<RunningDcs> ReadRunning()
    {
        var processes = Process.GetProcessesByName("DCS");
        try
        {
            var running = new List<RunningDcs>(processes.Length);
            foreach (var process in processes)
            {
                string? image = null;
                try { image = SystemPaths.ProcessImagePath(process.Id); }
                catch (Exception e) when (e is System.ComponentModel.Win32Exception or InvalidOperationException or ArgumentException) { }
                running.Add(new(process.Id, image));
            }
            return running;
        }
        finally { foreach (var process in processes) process.Dispose(); }
    }
}
