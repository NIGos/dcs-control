using System.IO;
using System.Windows;
using System.Windows.Threading;

namespace DcsVr.App;

public partial class App : Application
{
    private System.Threading.Mutex? _single;
    [System.Runtime.InteropServices.DllImport("user32.dll")] private static extern bool SetForegroundWindow(IntPtr window);
    [System.Runtime.InteropServices.DllImport("user32.dll")] private static extern bool ShowWindow(IntPtr window, int command);
    protected override void OnStartup(StartupEventArgs e)
    {
        base.OnStartup(e);
        // Unexpected errors are logged (%LOCALAPPDATA%\DcsControl\app.log) and reported without closing the app where
        // that is safe: a failed UI event or an unobserved background task leaves the profile and the game files as the
        // record of original files describes them.
        DispatcherUnhandledException += OnDispatcherUnhandledException;
        TaskScheduler.UnobservedTaskException += (_, args) => { AppLog.Error("Unobserved task", args.Exception); args.SetObserved(); };
        AppDomain.CurrentDomain.UnhandledException += (_, args) => { if (args.ExceptionObject is Exception error) AppLog.Error(args.IsTerminating ? "Fatal" : "Unhandled", error); };
        if (e.Args.Length == 2 && e.Args[0] is "--web-smoke" or "--render-smoke")
        {
            ShutdownMode = ShutdownMode.OnExplicitShutdown;
            _ = RunWebSmoke(e.Args[1]); return;
        }
        // One window at a time: a second start brings the open one forward (two copies would apply and restore the same
        // game files against each other).
        _single = new System.Threading.Mutex(true, @"Local\DcsControl.App", out var first);
        if (!first)
        {
            try
            {
                foreach (var other in System.Diagnostics.Process.GetProcessesByName(System.Diagnostics.Process.GetCurrentProcess().ProcessName))
                    using (other) if (other.Id != Environment.ProcessId && other.MainWindowHandle != IntPtr.Zero) { ShowWindow(other.MainWindowHandle, 9); SetForegroundWindow(other.MainWindowHandle); }
            }
            catch (Exception error) when (error is InvalidOperationException or System.ComponentModel.Win32Exception) { }
            Shutdown(0); return;
        }
        // Started inside another app's sandbox, Windows would keep the app's record of original files in a private copy
        // (see AppDataRedirection): the app starts again outside it, through Explorer, and this copy closes.
        if (e.Args.Length == 0 && DcsVr.Core.AppDataRedirection.Target() is { } redirected)
        {
            try
            {
                _single?.ReleaseMutex(); _single?.Dispose(); _single = null;
                using var outside = System.Diagnostics.Process.Start(new System.Diagnostics.ProcessStartInfo("explorer.exe") { ArgumentList = { Environment.ProcessPath! }, UseShellExecute = false });
                Shutdown(0); return;
            }
            catch (Exception error) when (error is System.ComponentModel.Win32Exception or InvalidOperationException)
            {
                AppLog.Error("Restart outside the sandbox", error);
                MessageBox.Show(string.Format(System.Globalization.CultureInfo.InvariantCulture, DcsVr.Core.AppDataRedirection.Message, redirected), "DCS Control", MessageBoxButton.OK, MessageBoxImage.Warning);
                Shutdown(1); return;
            }
        }
        // A monitor mode the boost helper changed for a flight and could not set back (it was ended) is set back now.
        try { if (DcsVr.Core.BoostRuntime.RestoreLeftoverDisplay(DcsVr.Core.ControlService.DefaultStateRoot) is { } restored) AppLog.Info("Monitor mode: " + restored); }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException or System.ComponentModel.Win32Exception) { AppLog.Error("Monitor mode restore", error); }
        new WebWindow().Show();
    }
    private DateTime _lastErrorShown;
    private void OnDispatcherUnhandledException(object sender, DispatcherUnhandledExceptionEventArgs e)
    {
        AppLog.Error("UI", e.Exception);
        // Running out of memory or a broken stack is not survivable; everything else is reported and the app goes on.
        if (e.Exception is OutOfMemoryException or StackOverflowException or InvalidProgramException) return;
        e.Handled = true;
        if (ShutdownMode == ShutdownMode.OnExplicitShutdown || DateTime.UtcNow - _lastErrorShown < TimeSpan.FromSeconds(10)) return;
        _lastErrorShown = DateTime.UtcNow;
        MessageBox.Show("DCS Control hit an unexpected error and kept running:\n\n" + e.Exception.Message + "\n\nDetails were saved to " + AppLog.Path + ".",
            "DCS Control", MessageBoxButton.OK, MessageBoxImage.Warning);
    }
    private async Task RunWebSmoke(string path)
    {
        try { await WebVerification.Run(path); Shutdown(0); }
        catch (Exception e) { Directory.CreateDirectory(Path.GetDirectoryName(Path.GetFullPath(path))!); File.WriteAllText(path + "-failure.txt", e.ToString()); Shutdown(1); }
    }
}
