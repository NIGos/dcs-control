using System.Diagnostics;

namespace DcsVr.Core;

/// <summary>One running DCS.exe: its process id and full image path (null when it cannot be read).</summary>
public sealed record RunningDcs(int Pid, string? ImagePath);

/// <summary>The running DCS processes, in one place: every "is DCS running" check asks here.</summary>
public static class DcsProcess
{
    /// <summary>Replaceable so tests never depend on a DCS running on the build machine.</summary>
    public static Func<IReadOnlyList<RunningDcs>> Provider { get; set; } = ReadRunning;

    public static IReadOnlyList<RunningDcs> Running() => Provider();

    public static bool AnyRunning() => Running().Count > 0;

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
