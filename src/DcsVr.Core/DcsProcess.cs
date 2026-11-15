using System.Diagnostics;

namespace DcsVr.Core;

/// <summary>One running DCS.exe: its process id and full image path (null when it cannot be read).</summary>
public sealed record RunningDcs(int Pid, string? ImagePath);

public enum DcsHealth { NotRunning, Healthy, NoWindow, NotResponding }

/// <summary>Says when a running DCS has looked stuck for long enough to offer ending it. DCS has no window for a few
/// seconds while it starts and stops answering Windows for minutes while a mission loads, so each state has its own
/// patience; any healthy reading starts again. So does a pause in the readings: the page asks every few seconds, but not
/// while it is hidden or busy, and DCS may have answered, closed or started again in between.</summary>
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
