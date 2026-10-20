using System.Diagnostics;
using System.Runtime.InteropServices;
using Microsoft.Win32.SafeHandles;

namespace DcsVr.Core;

/// <summary>
/// The separate <c>DcsVr.Cli boost</c> process. It survives the app closing: it waits for the real DCS (following the
/// Steam edition's self-restart), applies the CPU Boost plan (DCS priority and all CPUs; VR runtime and background
/// apps confined), rescans for late starters, and restores exactly what it changed when DCS exits, on Ctrl+C, when its
/// console is closed, and at logoff or shutdown. It never touches DCS's affinity beyond all CPUs, DCS's ancestors, this
/// helper, its parent or a protected process. All process work is guarded; the pure parts live in
/// <see cref="BoostLedger"/>, <see cref="BoostAttach"/> and <see cref="BoostPlanner"/>, which are unit-tested.
/// </summary>
public sealed class BoostRuntime
{
    // Per session: a boost for one signed-in user never blocks another user's.
    private const string MutexName = @"Local\DcsVrControl.CpuBoost";
    private static readonly TimeSpan MinDcsLifetime = TimeSpan.FromSeconds(20);
    private static readonly TimeSpan RescanInterval = TimeSpan.FromSeconds(15);
    private static readonly TimeSpan AttachTimeout = TimeSpan.FromMinutes(5);
    private static readonly TimeSpan CloseGrace = TimeSpan.FromSeconds(5);
    // Windows ends a process a few seconds after a console close, logoff or shutdown event; restore must fit.
    private static readonly TimeSpan SignalRestoreBudget = TimeSpan.FromSeconds(4);

    private readonly VrProfile _profile;
    private readonly string? _dcsExecutable;
    private readonly int? _dcsPid;
    private readonly string? _dcsLog;
    private readonly string _boostDir;
    private readonly BoostLedger _ledger = new();
    private readonly List<string> _errors = [];
    private readonly List<string> _closed = [];
    private readonly List<string> _dcsChanges = [];
    private readonly HashSet<(int Pid, string Name)> _failed = [];
    private readonly object _restoreLock = new();
    private bool _restoreDone;
    private int _restoredCount;
    private int? _attachedPid;
    private CpuTopology _topology = CpuTopology.None("not yet detected");
    private ulong _vrMask, _bgMask, _allCpus;

    private readonly List<ClosedProcess> _closedForVram = [];
    private readonly List<string> _reopened = [];
    private string? _displayStatus;
    private IReadOnlyList<string> _tobiiPaused = [];
    private string? _tobiiStatus;
    private FlightDisplay? _display;
    private volatile bool _sessionEnding;

    private BoostRuntime(VrProfile profile, string stateRoot, string? dcsExecutable, int? dcsPid, string? dcsLog)
    {
        // The VR helpers (desktop Tobii pause) never run in Optimizations only.
        _profile = profile.ForLaunch();
        _dcsExecutable = string.IsNullOrWhiteSpace(dcsExecutable) ? null : SystemPaths.Canonical(dcsExecutable);
        _dcsPid = dcsPid;
        _dcsLog = dcsLog;
        _boostDir = Path.Combine(stateRoot, "boost");
    }

    /// <summary>The marker of a monitor mode the helper changed and has not yet set back.</summary>
    public static string DisplayMarkerPath(string stateRoot) => Path.Combine(stateRoot, "boost", "display-mode.json");
    /// <summary>The processes CPU Boost changed and has not yet set back, written after every change, so a helper that
    /// is ended (Task Manager, a crash) leaves them for the next app or helper start to restore.</summary>
    public static string LedgerPath(string stateRoot) => Path.Combine(stateRoot, "boost", "ledger.json");
    /// <summary>The programs Free VRAM closed and has not yet reopened (with "Reopen after the flight"), written after
    /// every close, so a helper that is ended leaves them for the next app or helper start to reopen.</summary>
    public static string ClosedPath(string stateRoot) => Path.Combine(stateRoot, "boost", "closed.json");
    /// <summary>The marker of desktop Tobii services a helper stopped and has not yet started again.</summary>
    public static string TobiiMarkerPath(string stateRoot) => Path.Combine(stateRoot, "boost", TobiiDesktop.MarkerFile);

    /// <summary>At app start: true when a helper stopped the desktop Tobii services and could not start them again (it
    /// was ended), and no helper runs now. After a restart the services start with Windows again: a marker whose services
    /// all run asks for nothing and is removed.</summary>
    public static bool TobiiLeftPaused(string stateRoot)
    {
        var marker = TobiiMarkerPath(stateRoot);
        if (!File.Exists(marker) || HelperRunning()) return false;
        try
        {
            if (JsonData.Deserialize<string[]>(ReadRecord(marker)).Any(service => TobiiDesktop.State(service) != "RUNNING")) return true;
            File.Delete(marker);
            return false;
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException or System.Text.Json.JsonException or InvalidDataException) { return true; }
    }

    /// <summary>At app start: programs Free VRAM closed for a flight whose helper was ended before reopening them are
    /// reopened now, once, as they were started (path, arguments, the program's folder). Nothing happens while a helper
    /// runs, or while DCS runs (its helper was ended mid-flight: the programs would take its video memory back).
    /// Returns what was done, or null when there was nothing to do.</summary>
    public static IReadOnlyList<string>? ReopenLeftoverPrograms(string stateRoot) => HelperRunning() || DcsRunning() ? null : ReopenClosedFile(ClosedPath(stateRoot));

    private static bool DcsRunning()
    {
        var running = Process.GetProcessesByName("DCS");
        foreach (var process in running) process.Dispose();
        return running.Length > 0;
    }

    internal static IReadOnlyList<string>? ReopenClosedFile(string path, Func<ReopenTarget, string?>? start = null)
    {
        if (TakeClosedFile(path) is not { } closed) return null;
        var done = new List<string>();
        foreach (var target in FreeVram.ReopenTargets(closed, RunningImages(closed)))
            done.Add((start ?? FreeVram.StartAsUser)(target) is { } problem ? $"Could not reopen {target.Name}: {problem}" : $"Reopened {target.Name} ({target.ImagePath}).");
        return done;
    }

    /// <summary>The closed programs a record names, taken from it once: it is renamed first, so an app start and a helper
    /// start reading it together never both reopen them. Null when there is none, or when it is held by another program
    /// for a moment (an antivirus scan): then it stays for the next start, never removed unread.</summary>
    internal static ClosedProcess[]? TakeClosedFile(string path)
    {
        if (!File.Exists(path)) return null;
        var claimed = path + "." + Guid.NewGuid().ToString("N") + ".taken";
        try { File.Move(path, claimed); }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException) { return null; }
        ClosedProcess[] closed;
        try { closed = JsonData.Deserialize<ClosedProcess[]>(ReadRecord(claimed)); }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException)
        {
            try { File.Move(claimed, path); } catch (Exception again) when (again is IOException or UnauthorizedAccessException) { }
            return null;
        }
        catch (Exception e) when (e is System.Text.Json.JsonException or InvalidDataException) { closed = []; }
        // Once: the record goes before anything starts, so a program that cannot start is not tried at every start.
        try { File.Delete(claimed); } catch (Exception e) when (e is IOException or UnauthorizedAccessException) { }
        return closed;
    }

    /// <summary>Executables of the closed programs that run again (their own service restarted them).</summary>
    private static List<string> RunningImages(IReadOnlyList<ClosedProcess> closed)
    {
        var running = new List<string>();
        if (closed.Count == 0) return running;
        foreach (var fact in BoostPlanner.CaptureProcesses(testAccess: false).Processes)
            if (closed.Any(c => BoostPlanner.MatchesProcessName(c.Name, fact.Name)) && SystemPaths.ProcessImagePath(fact.Pid) is { } image) running.Add(image);
        return running;
    }

    /// <summary>The <c>tobii-resume</c> command (elevated, started by the app): starts the desktop Tobii services a helper
    /// stopped and could not start again. Returns a process exit code.</summary>
    public static int ResumeLeftoverTobii(string stateRoot, TextWriter output, TextWriter error)
    {
        if (HelperRunning()) { error.WriteLine("A boost helper is running; it starts the desktop Tobii services again itself when DCS exits."); return 1; }
        if (!FreeVram.IsElevated()) { error.WriteLine("Starting the desktop Tobii services needs administrator rights."); return 1; }
        var (done, message) = ResumeTobiiMarker(TobiiMarkerPath(stateRoot));
        (done ? output : error).WriteLine("Desktop Tobii: " + message);
        return done ? 0 : 1;
    }

    /// <summary>Starts what a Tobii marker names (elevated only) and removes the marker once everything started.</summary>
    private static (bool Done, string Message) ResumeTobiiMarker(string marker)
    {
        try
        {
            if (!File.Exists(marker)) return (true, "nothing is paused.");
            // Only names that are still desktop Tobii services on this PC; the marker never names anything else to start.
            var installed = TobiiDesktop.InstalledServices();
            var leftover = JsonData.Deserialize<string[]>(File.ReadAllText(marker)).Where(s => installed.Contains(s, StringComparer.OrdinalIgnoreCase)).ToArray();
            var errors = TobiiDesktop.Resume(leftover);
            if (errors.Count > 0) return (false, string.Join("; ", errors));
            File.Delete(marker);
            return (true, leftover.Length == 0 ? "nothing left to start." : "started again " + string.Join(", ", leftover));
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException or System.Text.Json.JsonException or InvalidDataException) { return (false, e.Message); }
    }

    /// <summary>The elevated <c>DcsVr.Cli tobii-resume</c> the app starts when the pilot asks; null without the CLI. The
    /// state folder is passed: the elevated process may run as another administrator, with another LOCALAPPDATA.</summary>
    public static ProcessStartInfo? TobiiResumeStart(string stateRoot, string? cliDirectory = null)
    {
        var directory = cliDirectory ?? AppContext.BaseDirectory;
        var cli = new[] { "DcsVr.Cli.exe", "DcsVr.Cli" }.Select(name => Path.Combine(directory, name)).FirstOrDefault(File.Exists);
        if (cli is null) return null;
        var start = new ProcessStartInfo(cli) { WorkingDirectory = Path.GetDirectoryName(cli)!, UseShellExecute = true, Verb = "runas", WindowStyle = ProcessWindowStyle.Hidden };
        foreach (var argument in new[] { "tobii-resume", "--state", stateRoot }) start.ArgumentList.Add(argument);
        return start;
    }

    /// <summary>At app start: processes a helper changed and could not set back (it was ended) get their original
    /// priority and CPUs back, when they are still the same processes (PID, name and start time). Nothing happens while
    /// a helper runs. Returns how many were restored, or null when there was nothing to do.</summary>
    public static int? RestoreLeftoverProcesses(string stateRoot) => HelperRunning() ? null : RestoreLedgerFile(LedgerPath(stateRoot));

    internal static int? RestoreLedgerFile(string path)
    {
        if (!File.Exists(path)) return null;
        BoostChange[] changes;
        try { changes = JsonData.Deserialize<BoostChange[]>(ReadRecord(path)); }
        // Held by another program for a moment (an antivirus scan): kept for the next start, never deleted unread.
        catch (Exception e) when (e is IOException or UnauthorizedAccessException) { return null; }
        catch (Exception e) when (e is System.Text.Json.JsonException or InvalidDataException) { changes = []; }
        var results = changes.Select(change => (change, result: RestoreChange(change))).ToList();
        // A process this start may not change (one an elevated helper moved) stays recorded for a helper with rights.
        var denied = results.Where(r => r.result is null).Select(r => r.change).ToArray();
        try
        {
            if (denied.Length == 0) File.Delete(path);
            else AtomicFile.WriteText(path, JsonData.Serialize(denied));
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException) { }
        return results.Count(r => r.result == true);
    }

    /// <summary>Puts one recorded process back, only onto the same process it was taken from: true when restored, false
    /// when that process is gone, null when it may not be changed from here (access denied).</summary>
    private static bool? RestoreChange(BoostChange change)
    {
        try
        {
            using var process = Process.GetProcessById(change.Pid);
            if (!BoostLedger.SameProcess(change, SafeName(process), process.StartTime.Ticks)) return false;
            if (change.OriginalAffinity != 0) process.ProcessorAffinity = (nint)change.OriginalAffinity;
            if (Enum.TryParse<ProcessPriorityClass>(change.OriginalPriority, out var priority) && !string.Equals(change.OriginalPriority, change.AppliedPriority, StringComparison.Ordinal))
                process.PriorityClass = priority;
            return true;
        }
        catch (Exception e) when (e is ArgumentException or InvalidOperationException) { return false; } // gone (or exited)
        catch (Exception e) when (IsProcessAccess(e)) { return null; }
    }

    private BoostChange[] _leftover = [];

    /// <summary>A helper record, read again for a moment while another program (an antivirus scan) holds it.</summary>
    private static string ReadRecord(string path)
    {
        var text = "";
        AtomicFile.Retry(() => text = File.ReadAllText(path));
        return text;
    }

    private static BoostChange[] ReadLedger(string path)
    {
        try { return File.Exists(path) ? JsonData.Deserialize<BoostChange[]>(ReadRecord(path)) : []; }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException or System.Text.Json.JsonException or InvalidDataException) { return []; }
    }

    private void SaveLedger()
    {
        try { AtomicFile.WriteText(LedgerPath(Path.GetDirectoryName(_boostDir)!), JsonData.Serialize(_ledger.Changes.Concat(_leftover).ToArray())); }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException) { Log("Could not record the changed processes: " + e.Message); }
    }

    /// <summary>True while a boost helper runs in this session (it holds the helper mutex).</summary>
    public static bool HelperRunning()
    {
        try { if (Mutex.TryOpenExisting(MutexName, out var existing)) { existing.Dispose(); return true; } return false; }
        catch (UnauthorizedAccessException) { return true; } // an elevated helper's mutex
    }

    /// <summary>At app start: a monitor mode a helper changed and could not set back (it was ended) is set back now.
    /// Nothing happens while a helper runs. Returns what was done, or null.</summary>
    public static string? RestoreLeftoverDisplay(string stateRoot, IDisplayModes? displays = null)
    {
        var marker = DisplayMarkerPath(stateRoot);
        if (!File.Exists(marker) || HelperRunning()) return null;
        return FlightDisplay.RestoreLeftover(displays ?? new WindowsDisplayModes(), marker);
    }

    /// <summary>dcs.log next to an options.lua (Saved Games\DCS\Config\options.lua -> Saved Games\DCS\Logs\dcs.log).</summary>
    public static string? DcsLogFromOptions(string? optionsPath)
    {
        if (string.IsNullOrWhiteSpace(optionsPath)) return null;
        try { return Path.GetDirectoryName(Path.GetDirectoryName(Path.GetFullPath(optionsPath))) is { } profileRoot ? Path.Combine(profileRoot, "Logs", "dcs.log") : null; }
        catch (Exception e) when (e is ArgumentException or NotSupportedException or PathTooLongException) { return null; }
    }

    /// <summary>CLI entry point for the <c>boost</c> command. Returns a process exit code.</summary>
    public static int Run(IReadOnlyList<string> args, TextWriter output, TextWriter error)
    {
        var parsed = BoostArguments.Parse(args);
        if (parsed.Error is not null) { error.WriteLine(parsed.Error); error.WriteLine(BoostArguments.Usage); return 1; }
        if (parsed.Help) { output.WriteLine(BoostArguments.Usage); return 0; }

        var stateRoot = Path.GetFullPath(parsed.StateRoot ?? ControlService.DefaultStateRoot);
        VrProfile profile;
        string? dcsExecutable = parsed.DcsExecutable, dcsLog = parsed.DcsLog;
        try
        {
            var applied = ResolveApplied(stateRoot);
            if (parsed.ProfilePath is { } path) profile = JsonData.Deserialize<VrProfile>(File.ReadAllText(path));
            else if (applied is not null) profile = applied.Value.Profile;
            else { error.WriteLine("No profile was given and no applied profile was found under " + stateRoot); return 1; }
            dcsExecutable ??= applied?.Executable;
            dcsLog ??= DcsLogFromOptions(applied?.Options);
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException or System.Text.Json.JsonException or InvalidDataException)
        { error.WriteLine("Could not read the boost profile: " + e.Message); return 1; }

        if (!profile.UsesBoostHelper) { output.WriteLine("CPU Boost, Free VRAM before flight and Lower the monitor are off in this profile; nothing to do."); return 0; }

        Mutex mutex;
        try { mutex = new Mutex(initiallyOwned: false, MutexName, out _); }
        catch (UnauthorizedAccessException) { error.WriteLine("CPU Boost is already running (as administrator)."); return 0; }
        using var _mutex = mutex;
        var held = false;
        try { held = mutex.WaitOne(TimeSpan.Zero); } catch (AbandonedMutexException) { held = true; }
        if (!held) { error.WriteLine("CPU Boost is already running."); return 0; }
        try { return new BoostRuntime(profile, stateRoot, dcsExecutable, parsed.DcsPid, dcsLog).Execute(output); }
        finally { try { mutex.ReleaseMutex(); } catch (ApplicationException) { } }
    }

    private static (VrProfile Profile, string? Executable, string? Options)? ResolveApplied(string stateRoot)
    {
        var journal = new OriginalsStore(Path.Combine(stateRoot, "originals")).ReadCurrent();
        if (journal is null) return null;
        JournalEntry? Entry(string file) => journal.Entries.FirstOrDefault(e => Path.GetFileName(e.Path).Equals(file, StringComparison.OrdinalIgnoreCase));
        var profileEntry = Entry("profile.json");
        if (profileEntry is null || !File.Exists(profileEntry.Path)) return null;
        var profile = JsonData.Deserialize<VrProfile>(File.ReadAllText(profileEntry.Path));
        var launchEntry = Entry("launch.json");
        string? executable = null;
        if (launchEntry is not null && File.Exists(launchEntry.Path))
            try { executable = JsonData.Deserialize<LaunchConfiguration>(File.ReadAllText(launchEntry.Path)).Executable; } catch (Exception e) when (e is IOException or System.Text.Json.JsonException) { }
        return (profile, executable, Entry("options.lua")?.Path);
    }

    private int Execute(TextWriter output)
    {
        Directory.CreateDirectory(_boostDir);
        // An earlier helper that was ended before it could restore: its processes are put back first.
        if (RestoreLedgerFile(LedgerPath(Path.GetDirectoryName(_boostDir)!)) is int earlier) Log($"Processes changed by an earlier flight: {earlier} restored.");
        // Those this helper may not change (an elevated helper moved them) stay recorded alongside its own changes.
        _leftover = ReadLedger(LedgerPath(Path.GetDirectoryName(_boostDir)!));
        // Programs an earlier helper closed for Free VRAM and was ended before reopening. With Free VRAM and Reopen on
        // for this flight they stay closed and are reopened with this flight's own; otherwise they are reopened now.
        if (_profile.FreeVram && _profile.FreeVramReopen)
        {
            if (TakeClosedFile(ClosedPath(Path.GetDirectoryName(_boostDir)!)) is { Length: > 0 } earlierClosed)
            {
                _closedForVram.AddRange(earlierClosed);
                SaveClosedRecord();
                Log($"Free VRAM (earlier flight): {earlierClosed.Length} closed programs are reopened after this flight.");
            }
        }
        else if (ReopenClosedFile(ClosedPath(Path.GetDirectoryName(_boostDir)!)) is { } reopened) foreach (var line in reopened) Log("Free VRAM (earlier flight): " + line);
        // Not disposed: a signal handler thread may still be waiting on them when Execute returns.
        var stopping = new ManualResetEventSlim(false);
        var finished = new ManualResetEventSlim(false);
        void RequestStop(string reason)
        {
            if (!stopping.IsSet) Log(reason + "; restoring.");
            stopping.Set();
        }
        ConsoleCancelEventHandler onCancel = (_, e) => { e.Cancel = true; RequestStop("Ctrl+C"); };
        // Console close (SIGHUP), logoff/shutdown (SIGTERM) and Ctrl+C (SIGINT). Windows ends the process once a close,
        // logoff or shutdown handler returns, so the handler waits for the restore below to finish.
        var registrations = new List<PosixSignalRegistration>();
        foreach (var (signal, reason) in new[] { (PosixSignal.SIGTERM, "Logoff or shutdown"), (PosixSignal.SIGHUP, "Console closed"), (PosixSignal.SIGINT, "Ctrl+C") })
            try
            {
                registrations.Add(PosixSignalRegistration.Create(signal, context =>
                {
                    context.Cancel = true;
                    if (context.Signal == PosixSignal.SIGTERM) _sessionEnding = true;
                    RequestStop(reason);
                    finished.Wait(SignalRestoreBudget);
                }));
            }
            catch (Exception e) when (e is PlatformNotSupportedException or IOException or System.ComponentModel.Win32Exception) { }
        // Logoff is not one of the signals above on every .NET version; services moved by an elevated helper outlive the
        // session, so logoff and shutdown are also handled directly.
        ConsoleCtrlHandler onSessionEnd = type =>
        {
            if (type is not (CTRL_CLOSE_EVENT or CTRL_LOGOFF_EVENT or CTRL_SHUTDOWN_EVENT)) return false;
            if (type != CTRL_CLOSE_EVENT) _sessionEnding = true;
            RequestStop(type == CTRL_CLOSE_EVENT ? "Console closed" : "Logoff or shutdown");
            finished.Wait(SignalRestoreBudget);
            return true;
        };
        var sessionHandler = false;
        try { sessionHandler = SetConsoleCtrlHandler(onSessionEnd, true); } catch (Exception e) when (e is DllNotFoundException or EntryPointNotFoundException) { }
        try { Console.CancelKeyPress += onCancel; } catch (Exception e) when (e is IOException or PlatformNotSupportedException) { }
        // Any other way out of this process (an unhandled error on another thread, a normal exit) still sets the
        // monitor back; a hard kill is covered by the marker file at the next app or helper start.
        UnhandledExceptionEventHandler onCrash = (_, _) => RestoreDisplay("Unexpected error");
        EventHandler onExit = (_, _) => RestoreDisplay("Helper exit");
        AppDomain.CurrentDomain.UnhandledException += onCrash;
        AppDomain.CurrentDomain.ProcessExit += onExit;

        try
        {
            Log($"Boost helper started (PID {Environment.ProcessId}). CPU Boost {(_profile.CpuBoost ? "on" : "off")}, Free VRAM {(_profile.FreeVram ? "on" : "off")}, desktop Tobii pause {(_profile.PauseTobiiDesktop ? "on" : "off")}, elevated {FreeVram.IsElevated()}, monitor mode {(_profile.UsesLowerMonitor ? FlightDisplay.Target(_profile).ToString() : "off")}. DCS hint PID {(_dcsPid?.ToString() ?? "none")}, executable {_dcsExecutable ?? "any"}, log {_dcsLog ?? "auto"}.");
            if (_profile.UsesLowerMonitor) ApplyDisplay();
            // Before DCS creates its OpenXR session, so the headset's eye tracker is free when Quad Views asks for gaze.
            ResumeLeftoverTobii();
            if (_profile.UsesTobiiPause) PauseTobii();
            WriteStatus("waiting");
            output.WriteLine("CPU Boost waiting for DCS...");

            var (dcs, timedOut) = WaitForDcs(stopping);
            if (dcs is null)
            {
                Log(timedOut ? $"No DCS ran for {MinDcsLifetime.TotalSeconds:0} s within {AttachTimeout.TotalMinutes:0} minutes; giving up." : "Stopped before DCS appeared.");
                WriteStatus(timedOut ? "timeout" : "stopped");
                output.WriteLine(timedOut ? "CPU Boost gave up waiting for DCS." : "CPU Boost stopped.");
                return timedOut ? 2 : 0;
            }

            // DCS's own launcher (and the Steam edition) exits and starts DCS again as DCS.exe --restarted: the attached
            // process ending is only DCS's exit when no DCS from the same executable follows within RestartGrace.
            var first = true;
            while (dcs is not null)
            {
                Process? next = null;
                using (dcs)
                {
                    _attachedPid = dcs.Id;
                    if (first)
                    {
                        _topology = CpuTopology.Detect(_dcsLog);
                        _allCpus = _topology.AllCpusMask != 0 ? _topology.AllCpusMask : Mask(Enumerable.Range(0, Math.Min(64, _topology.LogicalCount)));
                        _vrMask = Mask(_topology.VrRuntimeCores) & _allCpus;
                        _bgMask = Mask(_topology.BackgroundCores) & _allCpus;
                        Log($"DCS found (PID {dcs.Id}). {_topology.Description}." + (_topology.HasRanking ? "" : " No process is moved between cores; only DCS priority changes."));
                    }

                    if (_profile.CpuBoost) TuneDcs(dcs);
                    if (_profile.FreeVram && first) CloseForVram(dcs.Id);
                    if (_profile.CpuBoost) ApplyToOthers(dcs.Id);
                    WriteStatus("active");
                    output.WriteLine($"CPU Boost active for DCS PID {dcs.Id}.");

                    // One handle, opened at attach, tells when this DCS exits; no PID lookups that a recycled PID could fool.
                    using var exited = new ProcessExitWaitHandle(dcs.SafeHandle);
                    var handles = new[] { stopping.WaitHandle, exited };
                    var dcsExited = false;
                    while (true)
                    {
                        var signaled = WaitHandle.WaitAny(handles, RescanInterval);
                        if (signaled == 0) break;
                        if (signaled == 1) { dcsExited = true; break; }
                        if (_profile.CpuBoost) ApplyToOthers(dcs.Id);
                        WriteStatus("active");
                    }
                    if (!dcsExited) break;
                    next = WaitForRestart(dcs.Id, stopping);
                    if (next is null) { Log("DCS exited; restoring."); break; }
                    Log($"DCS PID {dcs.Id} ended and DCS started again (PID {next.Id}), as DCS's launcher does: CPU Boost moves to it; nothing is restored.");
                }
                dcs = next;
                first = false;
            }
            return 0;
        }
        finally
        {
            // At logoff or shutdown the desktop Tobii services go first: they outlive the session, and Windows ends this
            // process within seconds. Their start is only requested, so the restores below still fit.
            if (_sessionEnding) ResumeTobii(wait: false);
            var restored = RestoreOnce();
            // The monitor first: it is what the user sees, and a logoff leaves little time.
            RestoreDisplay("Restore");
            ResumeTobii();
            if (_profile.FreeVram && _profile.FreeVramReopen && !_sessionEnding) ReopenClosed();
            // Nothing is reopened at logoff or shutdown, then or at the next start.
            else DeleteClosedRecord();
            if (_attachedPid is not null)
            {
                Log($"Restored {restored} of {_ledger.Changes.Count} changed processes.");
                WriteStatus("restored", restored);
                try { output.WriteLine($"CPU Boost restored {restored} processes."); } catch (IOException) { }
            }
            finished.Set();
            AppDomain.CurrentDomain.UnhandledException -= onCrash;
            AppDomain.CurrentDomain.ProcessExit -= onExit;
            try { Console.CancelKeyPress -= onCancel; } catch (Exception e) when (e is IOException or PlatformNotSupportedException) { }
            foreach (var registration in registrations) registration.Dispose();
            if (sessionHandler) SetConsoleCtrlHandler(onSessionEnd, false);
            GC.KeepAlive(onSessionEnd);
        }
    }

    /// <summary>Waits for a DCS that has run for <see cref="MinDcsLifetime"/>. The PID from the launch is only a hint:
    /// the Steam edition exits after a few seconds and starts again as <c>DCS.exe --restarted</c>, so once the hinted
    /// process is gone, the DCS started from the launched executable is used. Gives up after
    /// <see cref="AttachTimeout"/>.</summary>
    private (Process? Dcs, bool TimedOut) WaitForDcs(ManualResetEventSlim stopping)
    {
        var deadline = DateTime.UtcNow + AttachTimeout;
        var hintGoneLogged = false;
        while (!stopping.IsSet)
        {
            var candidates = DcsCandidates();
            if (_dcsPid is { } hint && !hintGoneLogged && !candidates.Any(c => c.Pid == hint))
            {
                hintGoneLogged = true;
                Log($"DCS PID {hint} is not running (DCS may have restarted itself); waiting for DCS from {_dcsExecutable ?? "any location"}.");
            }
            if (BoostAttach.Choose(candidates, _dcsPid, _dcsExecutable, MinDcsLifetime, DateTime.UtcNow) is { } pid)
            {
                var expected = candidates.First(c => c.Pid == pid).StartTime;
                Process? process = null;
                try
                {
                    process = Process.GetProcessById(pid);
                    _ = process.SafeHandle; // opens and keeps the one handle used for the whole session
                    if (!process.HasExited && process.StartTime == expected) { var attached = process; process = null; return (attached, false); }
                }
                catch (Exception e) when (e is ArgumentException || IsProcessAccess(e)) { Log($"Could not open DCS PID {pid}: {e.Message}"); }
                finally { process?.Dispose(); }
            }
            if (DateTime.UtcNow >= deadline) return (null, true);
            if (stopping.Wait(TimeSpan.FromSeconds(2))) break;
        }
        return (null, false);
    }

    /// <summary>How long a DCS that ended is given to start again (its launcher's Play, the Steam edition's restart)
    /// before CPU Boost restores everything.</summary>
    private static readonly TimeSpan RestartGrace = TimeSpan.FromSeconds(30);

    /// <summary>A DCS from the same executable (any one when the launch named none) that runs within
    /// <see cref="RestartGrace"/> after <paramref name="endedPid"/> ended, opened for the session; null when none does.</summary>
    private Process? WaitForRestart(int endedPid, ManualResetEventSlim stopping)
    {
        var deadline = DateTime.UtcNow + RestartGrace;
        while (!stopping.IsSet && DateTime.UtcNow < deadline)
        {
            foreach (var candidate in DcsCandidates())
            {
                if (candidate.Pid == endedPid) continue;
                if (_dcsExecutable is not null && (candidate.ImagePath is null || !candidate.ImagePath.Equals(_dcsExecutable, StringComparison.OrdinalIgnoreCase))) continue;
                Process? process = null;
                try
                {
                    process = Process.GetProcessById(candidate.Pid);
                    _ = process.SafeHandle; // the one handle for the rest of the session
                    if (!process.HasExited && process.StartTime == candidate.StartTime) { var attached = process; process = null; return attached; }
                }
                catch (Exception e) when (e is ArgumentException || IsProcessAccess(e)) { }
                finally { process?.Dispose(); }
            }
            if (stopping.Wait(TimeSpan.FromSeconds(1))) break;
        }
        return null;
    }

    private static IReadOnlyList<DcsCandidate> DcsCandidates()
    {
        var result = new List<DcsCandidate>();
        foreach (var process in SafeGetProcesses(BoostPlanner.DcsProcessName))
            using (process)
                try
                {
                    var image = SystemPaths.ProcessImagePath(process.Id);
                    result.Add(new DcsCandidate(process.Id, process.StartTime, image is null ? null : SystemPaths.Canonical(image)));
                }
                catch (Exception e) when (IsProcessAccess(e)) { }
        return result;
    }

    private void TuneDcs(Process dcs)
    {
        // DCS always keeps every CPU, whatever it inherited from the process that started it. Not recorded for
        // restore: DCS exits before restore runs.
        var priority = ToPriorityClass(_profile.BoostDcsPriority);
        try { dcs.PriorityClass = priority; _dcsChanges.Add($"DCS (PID {dcs.Id}): priority {priority}"); }
        catch (Exception e) when (IsProcessAccess(e)) { _errors.Add("DCS priority: " + e.Message); }
        if (_topology.HasRanking && _allCpus != 0)
            try
            {
                // Widened only from a mask CPU Boost itself set (inherited from a confined launcher): a pilot who pins DCS
                // on purpose (Process Lasso on the V-cache CCD, P-cores only) keeps that pinning.
                var current = (ulong)dcs.ProcessorAffinity & _allCpus;
                if (current == _allCpus) { }
                else if (current == _vrMask || current == _bgMask) { dcs.ProcessorAffinity = (nint)_allCpus; _dcsChanges.Add($"DCS (PID {dcs.Id}): affinity all CPUs 0x{_allCpus:X}"); }
                else _dcsChanges.Add($"DCS (PID {dcs.Id}): affinity 0x{current:X} kept (set outside CPU Boost)");
            }
            catch (Exception e) when (IsProcessAccess(e)) { _errors.Add("DCS affinity: " + e.Message); }
    }

    /// <summary>PIDs never touched whatever their name: DCS, its ancestors (children inherit affinity), this helper
    /// and its parent.</summary>
    private static HashSet<int> ExcludedPids(BoostSnapshot snapshot, int dcsPid)
    {
        var excluded = new HashSet<int>(BoostPlanner.AncestorPids(snapshot)) { dcsPid, Environment.ProcessId, 0 };
        if (snapshot.Processes.FirstOrDefault(p => p.Pid == Environment.ProcessId) is { } self) excluded.Add(self.ParentPid);
        return excluded;
    }

    private void ApplyToOthers(int dcsPid)
    {
        if (!_topology.HasRanking) return;
        var moveVr = _profile.BoostMoveVrRuntime && !_profile.Desktop && _vrMask != 0;
        var moveBackground = _profile.BoostMoveBackgroundApps && _bgMask != 0;
        if (!moveVr && !moveBackground) return;
        var vrPatterns = moveVr ? BoostPlanner.VrRuntimeServices(_profile) : [];
        var backgroundPatterns = moveBackground ? _profile.BoostBackgroundApps.Where(n => !string.IsNullOrWhiteSpace(n)).ToArray() : [];
        var ourMasks = new[] { _vrMask, _bgMask };

        // One snapshot per tick, with real parent PIDs rooted at the DCS being boosted.
        var snapshot = BoostPlanner.CaptureProcesses(testAccess: false, trackedDcsPid: dcsPid);
        var excluded = ExcludedPids(snapshot, dcsPid);
        foreach (var fact in snapshot.Processes)
        {
            // Another signed-in user's programs are never moved; services (session 0) are, for the VR runtime.
            if (excluded.Contains(fact.Pid) || BoostPlanner.IsProtectedName(fact.Name) || snapshot.OtherSession(fact, services: true)) continue;
            if (vrPatterns.Any(p => BoostPlanner.MatchesProcessName(p, fact.Name))) Confine(fact, _vrMask, priority: null, ourMasks);
            else if (backgroundPatterns.Any(p => BoostPlanner.MatchesProcessName(p, fact.Name))) Confine(fact, _bgMask, ProcessPriorityClass.BelowNormal, ourMasks);
        }
        RecordInherited(snapshot, excluded);
    }

    /// <summary>Processes a confined program started inherit its CPUs (and the priority CPU Boost gave it): they are
    /// recorded too, so the restore gives them all CPUs again. Nothing is changed here.</summary>
    private void RecordInherited(BoostSnapshot snapshot, HashSet<int> excluded)
    {
        var recorded = 0;
        // Each pass adds the next generation (a child's own children).
        for (var pass = 0; pass < 4; pass++)
        {
            var parents = _ledger.Changes.Select(c => c.Pid).ToHashSet();
            var added = false;
            foreach (var fact in snapshot.Processes)
            {
                if (!parents.Contains(fact.ParentPid) || fact.ParentPid == fact.Pid || excluded.Contains(fact.Pid) || BoostPlanner.IsProtectedName(fact.Name)
                    || snapshot.OtherSession(fact, services: true) || _failed.Contains((fact.Pid, fact.Name))) continue;
                Process? process = null;
                try
                {
                    process = Process.GetProcessById(fact.Pid);
                    var start = process.StartTime.Ticks;
                    if (_ledger.Contains(fact.Pid, start)) continue;
                    if (_ledger.TryRecordInherited(fact.Pid, fact.Name, start, fact.ParentPid, (ulong)(long)process.ProcessorAffinity, process.PriorityClass.ToString(), _allCpus)) { added = true; recorded++; }
                }
                catch (ArgumentException) { /* exited between the snapshot and now */ }
                catch (Exception e) when (IsProcessAccess(e)) { _failed.Add((fact.Pid, fact.Name)); }
                finally { process?.Dispose(); }
            }
            if (!added) break;
        }
        if (recorded == 0) return;
        SaveLedger();
        Log($"{recorded} process(es) started by confined programs recorded: the restore gives them all CPUs again.");
    }

    private void Confine(BoostProcessFact fact, ulong mask, ProcessPriorityClass? priority, IReadOnlyCollection<ulong> ourMasks)
    {
        if (_failed.Contains((fact.Pid, fact.Name))) return;
        Process? process = null;
        try
        {
            process = Process.GetProcessById(fact.Pid);
            var start = process.StartTime.Ticks;
            if (_ledger.Contains(fact.Pid, start)) return; // already confined
            var currentAffinity = (ulong)(long)process.ProcessorAffinity;
            var currentPriority = process.PriorityClass;
            process.ProcessorAffinity = (nint)mask;
            var appliedPriority = currentPriority;
            if (priority is { } p && p != currentPriority)
                try { process.PriorityClass = p; appliedPriority = p; }
                catch (Exception e) when (IsProcessAccess(e)) { _errors.Add($"{fact.Name} (PID {fact.Pid}) priority: {e.Message}"); }
            // Recorded only after the change succeeded, so status and restore list real changes only.
            if (_ledger.TryRecord(fact.Pid, fact.Name, start, currentAffinity, currentPriority.ToString(), mask, appliedPriority.ToString(), _allCpus, ourMasks, out _)) SaveLedger();
        }
        catch (ArgumentException) { /* exited between the snapshot and now */ }
        catch (Exception e) when (IsProcessAccess(e))
        {
            // Typically access denied: owned by another account or session, needs administrator rights.
            if (_failed.Add((fact.Pid, fact.Name)))
                _errors.Add($"{fact.Name} (PID {fact.Pid}): {e.Message}" + (_profile.BoostElevated ? "" : " (it may need the administrator rights option)"));
        }
        finally { process?.Dispose(); }
    }

    /// <summary>Free VRAM before flight: closes the listed programs. Each process's path and command line are recorded
    /// first (for reopening), then a graceful close request (main window, or WM_CLOSE to every top-level window of a
    /// process without a main window), then up to <see cref="CloseGrace"/> to exit. What is still running afterwards
    /// is ended only with "End programs that do not close"; otherwise it is left running. Nothing else is changed:
    /// no program setting, no Windows setting. Every outcome is logged.</summary>
    private void CloseForVram(int dcsPid)
    {
        var snapshot = BoostPlanner.CaptureProcesses(testAccess: false, trackedDcsPid: dcsPid);
        var targets = FreeVram.Targets(snapshot, _profile.FreeVramApps, ExcludedPids(snapshot, dcsPid));
        if (targets.Count == 0) { Log("Free VRAM: none of the listed programs is running."); return; }
        var memory = FreeVram.DedicatedVideoMemory();
        if (memory is not null) Log($"Free VRAM: {targets.Count} processes hold about {FreeVram.Megabytes(targets.Sum(t => memory.GetValueOrDefault(t.Pid)))} of video memory (approx.).");
        var targetPids = targets.Select(t => t.Pid).ToHashSet();
        var pending = new List<(Process Process, string Label, ClosedProcess Record)>();
        foreach (var fact in targets)
        {
            var label = $"{fact.Name} (PID {fact.Pid})";
            Process process;
            try { process = Process.GetProcessById(fact.Pid); }
            catch (ArgumentException) { continue; }
            var record = new ClosedProcess(fact.Pid, fact.ParentPid, fact.Name, SystemPaths.ProcessImagePath(fact.Pid), FreeVram.CommandLine(fact.Pid), HasVisibleWindow(fact.Pid));
            try
            {
                // A helper process of a program in the list (a browser's renderer) closes with its program.
                if (targetPids.Contains(fact.ParentPid)) { pending.Add((process, label, record)); continue; }
                var asked = process.MainWindowHandle != IntPtr.Zero && process.CloseMainWindow();
                var posted = asked ? 0 : PostCloseToWindows(fact.Pid);
                Log(asked ? $"Asked {label} to close its main window." : posted > 0 ? $"Sent a close request to {posted} window(s) of {label}." : $"{label} has no window to close" + (_profile.FreeVramForce ? $"; it is ended if still running after {CloseGrace.TotalSeconds:0} s." : "."));
                pending.Add((process, label, record));
            }
            catch (Exception e) when (IsProcessAccess(e)) { _errors.Add($"Close {label}: {e.Message}"); process.Dispose(); }
        }

        var deadline = DateTime.UtcNow + CloseGrace;
        foreach (var (process, label, record) in pending)
            using (process)
                try
                {
                    var remaining = deadline - DateTime.UtcNow;
                    if (process.WaitForExit(remaining > TimeSpan.Zero ? remaining : TimeSpan.Zero)) { _closed.Add(label); _closedForVram.Add(record); SaveClosedRecord(); Log("Closed " + label + "."); continue; }
                    if (!_profile.FreeVramForce) { Log($"{label} is still running after {CloseGrace.TotalSeconds:0} s; left running (End programs that do not close is off)."); continue; }
                    process.Kill();
                    process.WaitForExit(TimeSpan.FromSeconds(2));
                    _closed.Add(label + " (ended)");
                    _closedForVram.Add(record);
                    SaveClosedRecord();
                    Log($"Ended {label}: it did not close within {CloseGrace.TotalSeconds:0} s.");
                }
                catch (Exception e) when (IsProcessAccess(e))
                {
                    _errors.Add($"Close {label}: {e.Message}" + (_profile.BoostElevated ? "" : " (it may need administrator rights)"));
                    Log($"Could not close {label}: {e.Message}");
                }
    }

    /// <summary>Reopen after the flight: starts each closed program once, as the signed-in user, unless it is already
    /// running again.</summary>
    private void ReopenClosed()
    {
        if (_closedForVram.Count == 0) return;
        // The record goes first: each program is reopened once, here, never again at the next start.
        DeleteClosedRecord();
        foreach (var target in FreeVram.ReopenTargets(_closedForVram, RunningImages(_closedForVram)))
        {
            var problem = FreeVram.StartAsUser(target);
            // Never its arguments: boost.log goes into the diagnostics package.
            if (problem is null) { _reopened.Add(target.Name); Log($"Reopened {target.Name} ({target.ImagePath})."); }
            else { _errors.Add($"Reopen {target.Name}: {problem}"); Log($"Could not reopen {target.Name}: {problem}"); }
        }
    }

    /// <summary>Records what Free VRAM closed, for the next app or helper start when this helper is ended before it
    /// reopens them. Only with "Reopen after the flight".</summary>
    private void SaveClosedRecord()
    {
        if (!_profile.FreeVramReopen) return;
        try { AtomicFile.WriteText(ClosedPath(Path.GetDirectoryName(_boostDir)!), JsonData.Serialize(_closedForVram.ToArray())); }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException) { Log("Could not record the closed programs: " + e.Message); }
    }

    private void DeleteClosedRecord()
    {
        try { File.Delete(ClosedPath(Path.GetDirectoryName(_boostDir)!)); }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException) { }
    }

    private void ApplyDisplay()
    {
        try
        {
            var marker = DisplayMarkerPath(Path.GetDirectoryName(_boostDir)!);
            // A change an earlier helper could not undo is set back first, so the recorded mode is the user's own.
            if (FlightDisplay.RestoreLeftover(new WindowsDisplayModes(), marker) is { } leftover) Log("Monitor (earlier flight): " + leftover);
            _display = new FlightDisplay(new WindowsDisplayModes(), marker);
            _displayStatus = _display.Apply(FlightDisplay.Target(_profile));
            Log("Monitor: " + _displayStatus);
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException or System.ComponentModel.Win32Exception or EntryPointNotFoundException)
        { _errors.Add("Monitor mode: " + e.Message); }
    }

    private string TobiiMarker => Path.Combine(_boostDir, TobiiDesktop.MarkerFile);

    /// <summary>Stops the desktop Tobii services (elevated only) and records them, so a helper that dies before DCS exits
    /// leaves a marker the next helper starts them from.</summary>
    private void PauseTobii()
    {
        if (!FreeVram.IsElevated()) { _tobiiStatus = "not paused: needs administrator rights"; _errors.Add("Desktop Tobii: " + _tobiiStatus + " (the UAC prompt was declined?)."); Log("Desktop Tobii: " + _tobiiStatus); return; }
        var (stopped, errors) = TobiiDesktop.Pause();
        _tobiiPaused = stopped;
        try { if (stopped.Count > 0) AtomicFile.WriteText(TobiiMarker, JsonData.Serialize(stopped)); } catch (Exception e) when (e is IOException or UnauthorizedAccessException) { }
        _errors.AddRange(errors.Select(e => "Desktop Tobii: " + e));
        _tobiiStatus = stopped.Count > 0 ? "paused " + string.Join(", ", stopped) : TobiiDesktop.InstalledServices().Count == 0 ? "no desktop Tobii eye tracker installed" : "nothing running to pause";
        Log("Desktop Tobii: " + _tobiiStatus + (errors.Count > 0 ? "; " + string.Join("; ", errors) : ""));
    }

    private void ResumeTobii(bool wait = true)
    {
        if (_tobiiPaused.Count == 0) return;
        var errors = TobiiDesktop.Resume(_tobiiPaused, wait);
        _errors.AddRange(errors.Select(e => "Desktop Tobii: " + e));
        _tobiiStatus = errors.Count == 0 ? "started again " + string.Join(", ", _tobiiPaused) : "could not start everything again: " + string.Join("; ", errors);
        Log("Desktop Tobii: " + _tobiiStatus);
        if (errors.Count == 0) try { File.Delete(TobiiMarker); } catch (Exception e) when (e is IOException or UnauthorizedAccessException) { }
        _tobiiPaused = [];
    }

    /// <summary>Services an earlier helper paused and could not start again (it was ended before DCS exited).</summary>
    private void ResumeLeftoverTobii()
    {
        if (!File.Exists(TobiiMarker)) return;
        if (!FreeVram.IsElevated()) { Log("Desktop Tobii (earlier flight): services still paused; they start again with the next elevated helper, Start Tobii in DCS Control or a restart."); return; }
        Log("Desktop Tobii (earlier flight): " + ResumeTobiiMarker(TobiiMarker).Message);
    }

    private void RestoreDisplay(string reason)
    {
        try
        {
            if (_display?.Restore() is { } result) { _displayStatus = result; Log($"Monitor ({reason}): {result}"); }
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException or System.ComponentModel.Win32Exception or EntryPointNotFoundException)
        { _errors.Add("Monitor mode: " + e.Message); }
    }

    private static bool HasVisibleWindow(int pid)
    {
        var found = false;
        EnumWindowsProc callback = (hwnd, _) =>
        {
            if (GetWindowThreadProcessId(hwnd, out var owner) != 0 && owner == (uint)pid && IsWindowVisible(hwnd)) { found = true; return false; }
            return true;
        };
        EnumWindows(callback, IntPtr.Zero);
        GC.KeepAlive(callback);
        return found;
    }

    private static int PostCloseToWindows(int pid)
    {
        var windows = new List<IntPtr>();
        EnumWindowsProc callback = (hwnd, _) =>
        {
            if (GetWindowThreadProcessId(hwnd, out var owner) != 0 && owner == (uint)pid) windows.Add(hwnd);
            return true;
        };
        EnumWindows(callback, IntPtr.Zero);
        GC.KeepAlive(callback);
        return windows.Count(hwnd => PostMessage(hwnd, WM_CLOSE, IntPtr.Zero, IntPtr.Zero));
    }

    /// <summary>Restores everything recorded, once, whichever path (DCS exit, signal, error) gets here first.</summary>
    private int RestoreOnce()
    {
        lock (_restoreLock)
        {
            if (_restoreDone) return _restoredCount;
            _restoreDone = true;
            foreach (var change in _ledger.Changes) if (RestoreChange(change) == true) _restoredCount++;
            var path = LedgerPath(Path.GetDirectoryName(_boostDir)!);
            try
            {
                // Leftovers of an earlier elevated helper wait for one with rights; nothing else stays recorded.
                if (_leftover.Length == 0) File.Delete(path);
                else AtomicFile.WriteText(path, JsonData.Serialize(_leftover));
            }
            catch (Exception e) when (e is IOException or UnauthorizedAccessException) { }
            return _restoredCount;
        }
    }

    private static IEnumerable<Process> SafeGetProcesses(string name)
    {
        try { return Process.GetProcessesByName(name); }
        catch (Exception e) when (e is InvalidOperationException or System.ComponentModel.Win32Exception) { return []; }
    }

    private static string SafeName(Process process) => process.ProcessName;
    private static bool IsProcessAccess(Exception e) =>
        e is InvalidOperationException or System.ComponentModel.Win32Exception or NotSupportedException;

    private static ProcessPriorityClass ToPriorityClass(BoostPriority priority) => priority switch
    {
        BoostPriority.AboveNormal => ProcessPriorityClass.AboveNormal,
        BoostPriority.High => ProcessPriorityClass.High,
        _ => ProcessPriorityClass.Normal
    };

    private static ulong Mask(IEnumerable<int> cores)
    {
        ulong mask = 0;
        foreach (var core in cores) if (core is >= 0 and < 64) mask |= 1UL << core;
        return mask;
    }

    private void Log(string message)
    {
        var log = Path.Combine(_boostDir, "boost.log");
        try { if (new FileInfo(log) is { Exists: true, Length: > 1024 * 1024 }) File.Move(log, log + ".old", overwrite: true); } catch (Exception e) when (e is IOException or UnauthorizedAccessException) { }
        try { File.AppendAllText(log, $"{DateTimeOffset.Now:yyyy-MM-dd HH:mm:ss} {message}{Environment.NewLine}"); }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException) { }
    }

    private void WriteStatus(string state, int restored = 0)
    {
        var changed = _dcsChanges.Concat(_ledger.Changes.Select(c => $"{c.Name} (PID {c.Pid}): {c.AppliedPriority}, affinity 0x{c.AppliedAffinity:X}")).ToArray();
        var status = new BoostStatus
        {
            State = state,
            DcsPid = _attachedPid ?? _dcsPid,
            Source = _topology.Source,
            SourceDescription = _topology.Description,
            RenderCores = _topology.RenderCores,
            VrRuntimeCores = _topology.VrRuntimeCores,
            BackgroundCores = _topology.BackgroundCores,
            Changed = changed,
            Closed = _closed.ToArray(),
            Reopened = _reopened.ToArray(),
            Monitor = _displayStatus,
            Tobii = _tobiiStatus,
            Restored = restored,
            Errors = _errors.ToArray()
        };
        try { AtomicFile.WriteText(Path.Combine(_boostDir, "status.json"), JsonData.Serialize(status)); }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException) { }
    }

    /// <summary>Waits on a process handle owned by a <see cref="Process"/>; does not own or close it.</summary>
    private sealed class ProcessExitWaitHandle : WaitHandle
    {
        public ProcessExitWaitHandle(SafeProcessHandle process) => SafeWaitHandle = new SafeWaitHandle(process.DangerousGetHandle(), ownsHandle: false);
    }

    private delegate bool ConsoleCtrlHandler(uint type);
    private const uint CTRL_CLOSE_EVENT = 2, CTRL_LOGOFF_EVENT = 5, CTRL_SHUTDOWN_EVENT = 6;
    [DllImport("kernel32.dll", SetLastError = true)] private static extern bool SetConsoleCtrlHandler(ConsoleCtrlHandler handler, bool add);

    private delegate bool EnumWindowsProc(IntPtr hwnd, IntPtr parameter);
    private const uint WM_CLOSE = 0x0010;
    [DllImport("user32.dll")] private static extern bool EnumWindows(EnumWindowsProc callback, IntPtr parameter);
    [DllImport("user32.dll")] private static extern uint GetWindowThreadProcessId(IntPtr hwnd, out uint processId);
    [DllImport("user32.dll")] private static extern bool IsWindowVisible(IntPtr hwnd);
    [DllImport("user32.dll", SetLastError = true)] private static extern bool PostMessage(IntPtr hwnd, uint message, IntPtr wParam, IntPtr lParam);
}
