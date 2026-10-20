using System.Diagnostics;
using Microsoft.Win32;

namespace DcsVr.Core;

/// <summary>A desktop Tobii eye tracker (Tobii Eye Tracker 5 or 4C with Tobii Experience) next to a Pimax Crystal.
/// Tobii Experience discovers every Tobii device on the PC, the headset's eye chip included (its log lists
/// tobii-prp://XR5DA-… next to tobii-prp://IS50F-…), and while it holds the headset's tracker Pimax's eye tracking does
/// not work. While DCS runs, the boost helper (elevated) stops the desktop tracker's services and starts them again when
/// DCS exits. The headset's own runtimes (installed by Pimax: VR4PIMAX…, XR5EYECHIP…) are never touched.</summary>
public static class TobiiDesktop
{
    /// <summary>Tobii Experience's service; Tobii.EyeX.Engine and Tobii.EyeX.Interaction are its children.</summary>
    public const string ExperienceService = "Tobii Service";
    public const string MarkerFile = "tobii-paused.json";

    /// <summary>A service of the desktop tracker: Tobii Experience, or a desktop tracker's platform runtime installed by its
    /// Windows driver (platform_runtime_IS…, e.g. IS5LEYETRACKER5). Never a service installed by Pimax or a headset's
    /// runtime (VR…, XR…).</summary>
    public static bool IsDesktopService(string name, string? imagePath)
    {
        var image = imagePath ?? "";
        if (image.Contains(@"\Pimax\", StringComparison.OrdinalIgnoreCase) || name.Contains("PIMAX", StringComparison.OrdinalIgnoreCase)
            || image.Contains("platform_runtime_VR", StringComparison.OrdinalIgnoreCase) || image.Contains("platform_runtime_XR", StringComparison.OrdinalIgnoreCase))
            return false;
        if (name.Equals(ExperienceService, StringComparison.OrdinalIgnoreCase)) return image.Contains("Tobii.Service.exe", StringComparison.OrdinalIgnoreCase);
        return name.StartsWith("Tobii", StringComparison.OrdinalIgnoreCase) && Path.GetFileName(image.Trim('"')).StartsWith("platform_runtime_IS", StringComparison.OrdinalIgnoreCase);
    }

    /// <summary>The desktop tracker's installed services (registry), Tobii Experience first.</summary>
    public static IReadOnlyList<string> InstalledServices()
    {
        var found = new List<string>();
        try
        {
            using var services = Registry.LocalMachine.OpenSubKey(@"SYSTEM\CurrentControlSet\Services");
            if (services is null) return found;
            foreach (var name in services.GetSubKeyNames().Where(n => n.StartsWith("Tobii", StringComparison.OrdinalIgnoreCase)))
            {
                using var key = services.OpenSubKey(name);
                if (IsDesktopService(name, key?.GetValue("ImagePath") as string)) found.Add(name);
            }
        }
        catch (Exception e) when (e is System.Security.SecurityException or UnauthorizedAccessException or IOException) { }
        return [.. found.OrderBy(n => n.Equals(ExperienceService, StringComparison.OrdinalIgnoreCase) ? 0 : 1).ThenBy(n => n, StringComparer.OrdinalIgnoreCase)];
    }

    /// <summary>Stops the running desktop services; returns the ones it stopped (to start again) and what went wrong.</summary>
    public static (IReadOnlyList<string> Stopped, IReadOnlyList<string> Errors) Pause()
    {
        var stopped = new List<string>(); var errors = new List<string>();
        foreach (var service in InstalledServices())
        {
            if (State(service) != "RUNNING") continue;
            var (ok, message) = Control("stop", service, "STOPPED");
            if (ok) stopped.Add(service); else errors.Add($"Stop {service}: {message}");
        }
        // Tobii Experience's user-session processes end with its service; any left over would keep the device open.
        if (stopped.Any(s => s.Equals(ExperienceService, StringComparison.OrdinalIgnoreCase)))
            foreach (var name in new[] { "Tobii.EyeX.Engine", "Tobii.EyeX.Interaction" })
                foreach (var process in SafeGetProcesses(name))
                    using (process) try { process.Kill(); process.WaitForExit(5000); } catch (Exception e) when (e is InvalidOperationException or System.ComponentModel.Win32Exception) { }
        return (stopped, errors);
    }

    /// <summary>Starts the services <see cref="Pause"/> stopped; returns what went wrong. Without <paramref name="wait"/>
    /// the start is only requested (logoff and shutdown leave a few seconds for everything).</summary>
    public static IReadOnlyList<string> Resume(IEnumerable<string> services, bool wait = true)
    {
        var errors = new List<string>();
        foreach (var service in services)
        {
            if (State(service) == "RUNNING") continue;
            var (ok, message) = Control("start", service, "RUNNING", wait);
            if (!ok) errors.Add($"Start {service}: {message}");
        }
        return errors;
    }

    /// <summary>The service's state as sc.exe reports it ("RUNNING", "STOPPED", …); null when it cannot be queried.</summary>
    public static string? State(string service)
    {
        var (exit, output) = Sc("query", service);
        if (exit != 0) return null;
        var line = output.Split('\n').FirstOrDefault(l => l.TrimStart().StartsWith("STATE", StringComparison.OrdinalIgnoreCase));
        return line?.Split(' ', StringSplitOptions.RemoveEmptyEntries).ElementAtOrDefault(3);
    }

    private static (bool Ok, string Message) Control(string verb, string service, string target, bool wait = true)
    {
        var (exit, output) = Sc(verb, service);
        // 1062: not started (stop), 1056: already running (start).
        if (exit != 0 && exit != 1062 && exit != 1056) return (false, output.Trim().Split('\n').LastOrDefault()?.Trim() ?? $"sc {verb} failed ({exit})");
        if (!wait) return (true, "requested");
        var deadline = DateTime.UtcNow + TimeSpan.FromSeconds(20);
        while (DateTime.UtcNow < deadline)
        {
            if (State(service) == target) return (true, target);
            Thread.Sleep(250);
        }
        return (false, $"still not {target} after 20 s");
    }

    private static (int Exit, string Output) Sc(string verb, string service)
    {
        var start = new ProcessStartInfo(Path.Combine(Environment.SystemDirectory, "sc.exe")) { UseShellExecute = false, RedirectStandardOutput = true, RedirectStandardError = true, CreateNoWindow = true };
        start.ArgumentList.Add(verb); start.ArgumentList.Add(service);
        try
        {
            using var process = Process.Start(start) ?? throw new IOException("sc.exe could not be started.");
            var error = process.StandardError.ReadToEndAsync();
            var output = process.StandardOutput.ReadToEnd();
            if (!process.WaitForExit(30000)) { try { process.Kill(); } catch (InvalidOperationException) { } return (-1, "sc.exe did not answer within 30 s."); }
            return (process.ExitCode, output + error.Result);
        }
        catch (Exception e) when (e is System.ComponentModel.Win32Exception or IOException or InvalidOperationException) { return (-1, "sc.exe: " + e.Message); }
    }

    private static Process[] SafeGetProcesses(string name)
    {
        try { return Process.GetProcessesByName(name); }
        catch (Exception e) when (e is InvalidOperationException or System.ComponentModel.Win32Exception) { return []; }
    }
}
