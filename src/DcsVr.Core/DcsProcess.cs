using System.Diagnostics;
using System.Runtime.InteropServices;

namespace DcsVr.Core;

/// <summary>One running DCS.exe: its process id and full image path (null when it cannot be read).</summary>
/// <param name="SessionId">Its Windows session; -1 when unknown (counted as this session's).</param>
public sealed record RunningDcs(int Pid, string? ImagePath, int SessionId = -1);

/// <summary>Crashed: DCS shows its own crash window in place of the game window (it answers Windows, so it is not NotResponding).</summary>
public enum DcsHealth { NotRunning, Healthy, NoWindow, NotResponding, Crashed }

/// <summary>Says when a running DCS has looked stuck for long enough to offer ending it. DCS has no window for a few
/// seconds while it starts, and for longer while it closes after a large mission (a DCS that exits normally but slowly
/// must not be offered: the notice is on every page and counted while the app is minimised), and it stops answering
/// Windows for minutes while a mission loads, so each state has its own patience; any healthy reading starts again. So does a pause in the readings: DCS may have answered, closed or started
/// again in between. <see cref="DcsStuckWatch"/> keeps the readings coming while the page is hidden or busy.</summary>
public sealed class DcsStuckTracker
{
    public static readonly TimeSpan NoWindowPatience = TimeSpan.FromSeconds(60), NotRespondingPatience = TimeSpan.FromMinutes(4);
    /// <summary>A crash window is not a passing state: two readings in a row are enough.</summary>
    public static readonly TimeSpan CrashedPatience = TimeSpan.FromSeconds(8);
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
            DcsHealth.Crashed when now - _since >= CrashedPatience => DcsHealth.Crashed,
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

/// <summary>What ending DCS did (<see cref="DcsProcess.EndAll"/>): how many DCS.exe ended, how many Windows refused to end
/// (access denied: a DCS with administrator rights, GitHub #21) and how many were told to end and were still there 5 s later.</summary>
public sealed record DcsEndResult(int Ended, int Denied, int Lingering);

/// <summary>The running DCS processes, in one place: every "is DCS running" check asks here.</summary>
public static class DcsProcess
{
    /// <summary>Replaceable so tests never depend on a DCS running on the build machine.</summary>
    public static Func<IReadOnlyList<RunningDcs>> Provider { get; set; } = ReadRunning;

    public static IReadOnlyList<RunningDcs> Running() => Provider();

    public static bool AnyRunning() => Running().Count > 0;

    /// <summary>A DCS of this Windows session runs (one whose session is unknown counts): what End DCS can end, so what
    /// says whether it ended. Another signed-in user's DCS is left alone (<see cref="InThisSession(int)"/>).</summary>
    public static bool AnyRunningHere() => Running().Any(r => r.SessionId < 0 || InThisSession(r.SessionId));

    /// <summary>How the running DCS looks from outside. Replaceable for tests, like <see cref="Provider"/>.</summary>
    public static Func<DcsHealth> HealthProvider { get; set; } = ReadHealth;
    public static DcsHealth Health() => HealthProvider();

    /// <summary>Ends every DCS.exe (the user asked for it, for a DCS that is stuck) and waits a moment for them to go.
    /// Replaceable for tests, like <see cref="Provider"/>.</summary>
    public static Func<DcsEndResult> Ender { get; set; } = EndProcesses;
    public static DcsEndResult EndAll() => Ender();

    /// <summary>Whether a process runs in this Windows session. Another signed-in user's DCS (fast user switching) has no
    /// window here, so it would look stuck, and the elevated End DCS could end it: it is neither read nor ended, as Free
    /// VRAM never closes another session's programs.</summary>
    internal static bool InThisSession(int sessionId) => sessionId == CurrentSession.Value;
    private static readonly Lazy<int> CurrentSession = new(() => { using var self = Process.GetCurrentProcess(); return self.SessionId; });
    private static bool InThisSession(Process process)
    {
        try { return InThisSession(process.SessionId); }
        catch (Exception e) when (e is InvalidOperationException or System.ComponentModel.Win32Exception) { return false; } // exited meanwhile
    }

    private static DcsEndResult EndProcesses()
    {
        var processes = Process.GetProcessesByName("DCS"); int ended = 0, denied = 0, lingering = 0;
        try
        {
            foreach (var process in processes.Where(InThisSession))
                try { process.Kill(); if (process.WaitForExit(5000)) ended++; else lingering++; }
                // Access denied (GitHub #21): DCS has administrator rights this app does not have (started elevated, or set
                // to run as administrator), or it is already ending and hangs there.
                catch (System.ComponentModel.Win32Exception e) when (e.NativeErrorCode == ErrorAccessDenied) { denied++; }
                catch (Exception e) when (e is System.ComponentModel.Win32Exception or InvalidOperationException or NotSupportedException) { }
            return new(ended, denied, lingering);
        }
        finally { foreach (var process in processes) process.Dispose(); }
    }

    /// <summary>Ends DCS as <see cref="EndAll"/> does, unless it looks healthy now: it was offered because it looked stuck,
    /// and a DCS that only took long to load a mission may answer again before the pilot confirms. Null: left running.</summary>
    public static DcsEndResult? EndStuck() => Health() == DcsHealth.Healthy ? null : EndAll();

    /// <summary>The CLI command <see cref="ElevatedEndStart"/> runs, and its exit codes.</summary>
    public const string EndCommand = "end-dcs";
    public const int EndedCode = 0, NotEndedCode = 1, HealthyCode = 2;

    /// <summary>The elevated <c>DcsVr.Cli end-dcs</c> (one Windows prompt) for a DCS this app may not end itself
    /// (<see cref="DcsEndResult.Denied"/>); null without the CLI. It ends DCS only if it still looks stuck from there.</summary>
    public static ProcessStartInfo? ElevatedEndStart(string? cliDirectory = null)
    {
        var directory = cliDirectory ?? AppContext.BaseDirectory;
        var cli = new[] { "DcsVr.Cli.exe", "DcsVr.Cli" }.Select(name => Path.Combine(directory, name)).FirstOrDefault(File.Exists);
        if (cli is null) return null;
        var start = new ProcessStartInfo(cli) { WorkingDirectory = Path.GetDirectoryName(cli)!, UseShellExecute = true, Verb = "runas", WindowStyle = ProcessWindowStyle.Hidden };
        start.ArgumentList.Add(EndCommand);
        return start;
    }

    /// <summary>The <see cref="EndCommand"/> command: ends a DCS that still looks stuck; the exit code says how it went.</summary>
    public static int EndFromCommand(TextWriter output)
    {
        if (EndStuck() is not { } result) { output.WriteLine("DCS is responding, so it was not ended."); return HealthyCode; }
        output.WriteLine($"DCS.exe ended: {result.Ended}, refused: {result.Denied}, still closing: {result.Lingering}.");
        return AnyRunningHere() ? NotEndedCode : EndedCode;
    }

    /// <summary>What End DCS tells the pilot when DCS is still running after it. <paramref name="result"/>: this app's own
    /// try; <paramref name="elevatedExit"/>: the exit code of the elevated try (<see cref="ElevatedEndStart"/>), null when
    /// none ran; <paramref name="declined"/>: the Windows prompt for it was declined. Null: nothing to tell (it ended).</summary>
    public static string? EndFailure(DcsEndResult result, bool stillRunning, int? elevatedExit = null, bool declined = false)
    {
        const string manual = " End DCS.exe in Task Manager's Details tab.";
        if (elevatedExit == HealthyCode) return "DCS is responding again, so it was not ended.";
        if (!stillRunning) return null;
        if (declined) return "DCS runs with administrator rights, so ending it needs your permission, and the Windows prompt was declined." + manual;
        if (elevatedExit is not null) return "Windows did not let DCS end, even with administrator rights. It may be hanging while it closes: wait a minute, or restart Windows if it stays." + manual;
        if (result.Denied > 0) return "DCS runs with administrator rights, so Windows does not let DCS Control end it." + manual;
        if (result.Lingering > 0) return "DCS was told to end but is still closing. Wait a minute; if it stays, end DCS.exe in Task Manager's Details tab.";
        return "Windows did not let DCS Control end DCS." + manual;
    }

    private const int ErrorAccessDenied = 5;

    /// <summary>A DCS with no window at all (what ending it from Task Manager's app list can leave behind, GitHub #21),
    /// whose window does not answer Windows, or that crashed and still shows its crash window. One healthy DCS among
    /// several counts as healthy; of the others the most telling state wins (crashed, then not responding, then no window).</summary>
    private static DcsHealth ReadHealth()
    {
        var processes = Process.GetProcessesByName("DCS");
        try
        {
            // Another session's DCS is running, but not this pilot's to judge (it has no window here): healthy, never offered.
            var mine = processes.Where(InThisSession).ToArray();
            if (processes.Length == 0) return DcsHealth.NotRunning;
            if (mine.Length == 0) return DcsHealth.Healthy;
            var worst = DcsHealth.NoWindow;
            static int Rank(DcsHealth health) => health switch { DcsHealth.Crashed => 2, DcsHealth.NotResponding => 1, _ => 0 };
            foreach (var process in mine)
            {
                DcsHealth one;
                try
                {
                    var window = process.MainWindowHandle;
                    one = window == IntPtr.Zero ? DcsHealth.NoWindow : Classify(WindowClass(window), process.MainWindowTitle, Responds(window));
                }
                catch (Exception e) when (e is System.ComponentModel.Win32Exception or InvalidOperationException or NotSupportedException) { one = DcsHealth.Healthy; }
                if (one == DcsHealth.Healthy) return DcsHealth.Healthy;
                if (Rank(one) > Rank(worst)) worst = one;
            }
            return worst;
        }
        finally { foreach (var process in processes) process.Dispose(); }
    }

    /// <summary>What a DCS with a main window is, from that window. When DCS crashes the game window (class "DCS") goes
    /// and a standard dialog (class "#32770") titled "DCS Crash" takes its place, then "DCS failure" once the report is
    /// declined; it answers Windows and stays until someone closes it (measured on DCS 2.9.31, 2026-11-17). Any other
    /// dialog, such as a message box at start-up, is not called a crash.</summary>
    public static DcsHealth Classify(string? windowClass, string? title, bool responding) =>
        windowClass == "#32770" && title is not null && CrashTitles.Any(t => title.StartsWith(t, StringComparison.OrdinalIgnoreCase)) ? DcsHealth.Crashed
        : responding ? DcsHealth.Healthy : DcsHealth.NotResponding;

    private static readonly string[] CrashTitles = ["DCS Crash", "DCS failure"];

    /// <summary>Whether DCS's window answers, asked as <see cref="Process.Responding"/> asks (WM_NULL, 5 s). A DCS with
    /// administrator rights may refuse that message from this app (Windows' privilege isolation: access denied), which
    /// Process.Responding takes for "not responding", so a healthy elevated DCS would be offered for ending. Then
    /// Windows' own hung-window check, which works across that boundary, decides.</summary>
    private static bool Responds(IntPtr window)
    {
        if (SendMessageTimeout(window, 0 /* WM_NULL */, IntPtr.Zero, IntPtr.Zero, 0x0002 /* SMTO_ABORTIFHUNG */, 5000, out _) != IntPtr.Zero) return true;
        return Marshal.GetLastWin32Error() == ErrorAccessDenied && !IsHungAppWindow(window);
    }

    [DllImport("user32.dll", SetLastError = true)] private static extern IntPtr SendMessageTimeout(IntPtr window, uint message, IntPtr wParam, IntPtr lParam, uint flags, uint timeoutMs, out IntPtr result);
    [DllImport("user32.dll")] private static extern bool IsHungAppWindow(IntPtr window);

    private static string? WindowClass(IntPtr window)
    {
        var name = new char[64]; var length = GetClassName(window, name, name.Length);
        return length > 0 ? new string(name, 0, length) : null;
    }

    [DllImport("user32.dll", CharSet = CharSet.Unicode)] private static extern int GetClassName(IntPtr window, [Out] char[] name, int count);

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
                var session = -1;
                try { session = process.SessionId; }
                catch (Exception e) when (e is System.ComponentModel.Win32Exception or InvalidOperationException) { }
                running.Add(new(process.Id, image, session));
            }
            return running;
        }
        finally { foreach (var process in processes) process.Dispose(); }
    }
}
