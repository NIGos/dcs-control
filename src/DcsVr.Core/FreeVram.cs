using System.Runtime.InteropServices;
using System.Security.Principal;
using System.Text.RegularExpressions;

namespace DcsVr.Core;

/// <summary>One program Free VRAM before flight would close, as shown before launch.</summary>
/// <param name="Name">The name or pattern from the list.</param>
/// <param name="Processes">Running processes that match and may be closed.</param>
/// <param name="DedicatedBytes">Their dedicated video memory now (GPU Process Memory\Dedicated Usage, summed per
/// process: approximate, shared allocations can be counted twice). Null when the counters cannot be read.</param>
/// <param name="NeedsAdministrator">A matching process runs under another account or elevated: it cannot be closed
/// without administrator rights.</param>
/// <param name="Skipped">A matching process is DCS, an ancestor of DCS or a protected process and is never closed.</param>
public sealed record FreeVramApp(string Name, int Processes, long? DedicatedBytes, bool NeedsAdministrator, bool Skipped);

/// <summary>A process Free VRAM closed, recorded before the close request so it can be started again. HadWindow: it showed
/// a visible top-level window (a browser left running in the background has none).</summary>
public sealed record ClosedProcess(int Pid, int ParentPid, string Name, string? ImagePath, string? CommandLine, bool HadWindow = true);

/// <summary>A program to start again after the flight: its executable and the arguments it was started with.</summary>
public sealed record ReopenTarget(string Name, string ImagePath, string Arguments);

/// <summary>Free VRAM before flight. The selection and reopen bookkeeping is pure (unit-tested); the process, counter and
/// start helpers below it only read, except <see cref="StartAsUser"/>.</summary>
public static partial class FreeVram
{
    /// <summary>Processes the list closes: a name matches a pattern (<see cref="BoostPlanner.MatchesProcessName"/>), and
    /// the process is not in <paramref name="excluded"/> (DCS, its ancestors, the helper and its parent) and not a
    /// protected or launcher process, and runs in this session (another signed-in user's programs are never closed).</summary>
    public static IReadOnlyList<BoostProcessFact> Targets(BoostSnapshot snapshot, IReadOnlyList<string> patterns, IReadOnlySet<int> excluded)
    {
        var names = patterns.Where(p => !string.IsNullOrWhiteSpace(p)).ToArray();
        return snapshot.Processes.Where(p => !excluded.Contains(p.Pid) && !BoostPlanner.IsProtectedName(p.Name) && !snapshot.OtherSession(p)
            && names.Any(n => BoostPlanner.MatchesProcessName(n, p.Name))).ToArray();
    }

    /// <summary>Per pattern: what it would close now and the video memory those processes hold.</summary>
    public static IReadOnlyList<FreeVramApp> Describe(BoostSnapshot snapshot, IReadOnlyList<string> patterns, IReadOnlySet<int> excluded, IReadOnlyDictionary<int, long>? dedicated)
    {
        var result = new List<FreeVramApp>();
        foreach (var pattern in patterns.Where(p => !string.IsNullOrWhiteSpace(p)).Distinct(StringComparer.OrdinalIgnoreCase))
        {
            var matching = snapshot.Processes.Where(p => BoostPlanner.MatchesProcessName(pattern, p.Name) && !snapshot.OtherSession(p)).ToArray();
            var closable = matching.Where(p => !excluded.Contains(p.Pid) && !BoostPlanner.IsProtectedName(p.Name)).ToArray();
            long? bytes = dedicated is null ? null : closable.Sum(p => dedicated.TryGetValue(p.Pid, out var b) ? b : 0);
            result.Add(new(pattern.Trim(), closable.Length, bytes, closable.Any(p => !p.AccessibleForSetInfo), closable.Length < matching.Length));
        }
        return result;
    }

    /// <summary>What to start again after the flight, from the processes that were closed. Only a program's first
    /// process is started: a closed process whose parent was also closed is a helper of that program, and a Chromium or
    /// CEF helper (<c>--type=</c>) is never started on its own. A program is started once per executable and arguments,
    /// never when that executable is already running again (its own service restarted it), and never without a path.
    /// A Chromium browser that had no visible window (Edge or Chrome kept in the background) starts again without one
    /// (<c>--no-startup-window</c>), instead of opening a browser window the user had closed.</summary>
    public static IReadOnlyList<ReopenTarget> ReopenTargets(IReadOnlyList<ClosedProcess> closed, IReadOnlyCollection<string> runningImages)
    {
        var closedPids = closed.Select(c => c.Pid).ToHashSet();
        var running = new HashSet<string>(runningImages.Where(p => !string.IsNullOrWhiteSpace(p)), StringComparer.OrdinalIgnoreCase);
        var seen = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        var result = new List<ReopenTarget>();
        foreach (var process in closed)
        {
            if (string.IsNullOrWhiteSpace(process.ImagePath) || closedPids.Contains(process.ParentPid) && process.ParentPid != process.Pid) continue;
            var arguments = Arguments(process.CommandLine, process.ImagePath);
            if (IsHelperProcess(arguments) || running.Contains(process.ImagePath)) continue;
            if (!process.HadWindow && !arguments.Contains(NoStartupWindow, StringComparison.OrdinalIgnoreCase)
                && closed.Any(c => c.ParentPid == process.Pid && c.Pid != process.Pid && IsHelperProcess(Arguments(c.CommandLine, c.ImagePath ?? ""))))
                arguments = (arguments + " " + NoStartupWindow).Trim();
            if (seen.Add(process.ImagePath + "\0" + arguments)) result.Add(new(process.Name, process.ImagePath, arguments));
        }
        return result;
    }

    private const string NoStartupWindow = "--no-startup-window";

    /// <summary>A Chromium/CEF/Electron child process: started by its browser process, never on its own.</summary>
    public static bool IsHelperProcess(string arguments) => HelperType().IsMatch(arguments);
    [GeneratedRegex(@"(^|\s)""?--type=", RegexOptions.IgnoreCase)] private static partial Regex HelperType();

    /// <summary>The arguments of a command line: everything after the program token when the line starts with the
    /// program (quoted or not, by full path or by file name); otherwise the whole line, as some programs start
    /// themselves with arguments only.</summary>
    public static string Arguments(string? commandLine, string imagePath)
    {
        var line = (commandLine ?? "").Trim();
        if (line.Length == 0) return "";
        // An unquoted path with spaces (C:\Program Files\App\app.exe --x) is the program when it is the image path.
        if (line.StartsWith(imagePath, StringComparison.OrdinalIgnoreCase) && (line.Length == imagePath.Length || char.IsWhiteSpace(line[imagePath.Length])))
            return line[imagePath.Length..].Trim();
        string first; int end;
        if (line[0] == '"') { var close = line.IndexOf('"', 1); end = close < 0 ? line.Length : close + 1; first = line[1..(close < 0 ? line.Length : close)]; }
        else { var space = line.IndexOfAny([' ', '\t']); end = space < 0 ? line.Length : space; first = line[..end]; }
        var file = Path.GetFileName(imagePath);
        var stem = Path.GetFileNameWithoutExtension(imagePath);
        var isProgram = first.Equals(imagePath, StringComparison.OrdinalIgnoreCase)
            || SafeFileName(first) is { } named && (named.Equals(file, StringComparison.OrdinalIgnoreCase) || named.Equals(stem, StringComparison.OrdinalIgnoreCase));
        return isProgram ? line[end..].Trim() : line;
    }
    static string? SafeFileName(string token) { try { return Path.GetFileName(token); } catch (ArgumentException) { return null; } }

    /// <summary>Megabytes for display, rounded.</summary>
    public static string Megabytes(long bytes) => (bytes / (1024.0 * 1024.0)).ToString("N0", System.Globalization.CultureInfo.InvariantCulture) + " MB";

    // --- Read-only process facts ---

    /// <summary>The command line of a process (PROCESS_QUERY_LIMITED_INFORMATION; null when it cannot be read).</summary>
    public static string? CommandLine(int pid)
    {
        var handle = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, false, (uint)pid);
        if (handle == IntPtr.Zero) return null;
        try
        {
            var size = 4096;
            for (var attempt = 0; attempt < 4; attempt++)
            {
                var buffer = Marshal.AllocHGlobal(size);
                try
                {
                    var status = NtQueryInformationProcess(handle, ProcessCommandLineInformation, buffer, size, out var needed);
                    if (status == StatusInfoLengthMismatch || status == StatusBufferTooSmall) { size = Math.Max(needed, size * 2); continue; }
                    if (status != 0) return null;
                    var length = (ushort)Marshal.ReadInt16(buffer);
                    var text = Marshal.ReadIntPtr(buffer, IntPtr.Size); // UNICODE_STRING.Buffer, after Length/MaximumLength and padding
                    return text == IntPtr.Zero ? "" : Marshal.PtrToStringUni(text, length / 2);
                }
                finally { Marshal.FreeHGlobal(buffer); }
            }
            return null;
        }
        catch (Exception e) when (e is EntryPointNotFoundException or DllNotFoundException) { return null; }
        finally { CloseHandle(handle); }
    }

    /// <summary>Dedicated video memory per process from the GPU Process Memory performance counters, summed over
    /// adapters. Empty when the counters are unavailable. Read-only.</summary>
    public static IReadOnlyDictionary<int, long>? DedicatedVideoMemory()
    {
        try
        {
            if (PdhOpenQueryW(null, IntPtr.Zero, out var query) != 0) return null;
            try
            {
                if (PdhAddEnglishCounterW(query, @"\GPU Process Memory(*)\Dedicated Usage", IntPtr.Zero, out var counter) != 0) return null;
                if (PdhCollectQueryData(query) != 0) return null;
                uint size = 0;
                var status = PdhGetFormattedCounterArrayW(counter, PDH_FMT_LARGE | PDH_FMT_NOCAP100, ref size, out _, IntPtr.Zero);
                if (status != PDH_MORE_DATA || size == 0) return null;
                var buffer = Marshal.AllocHGlobal((int)size);
                try
                {
                    if (PdhGetFormattedCounterArrayW(counter, PDH_FMT_LARGE | PDH_FMT_NOCAP100, ref size, out var count, buffer) != 0) return null;
                    var result = new Dictionary<int, long>();
                    var itemSize = IntPtr.Size + 16; // PDH_FMT_COUNTERVALUE_ITEM_W: name pointer, then status (padded) and the 8-byte value
                    for (var i = 0; i < count; i++)
                    {
                        var item = buffer + i * itemSize;
                        var name = Marshal.PtrToStringUni(Marshal.ReadIntPtr(item));
                        if (Marshal.ReadInt32(item, IntPtr.Size) != 0 || name is null) continue;
                        var match = PidInstance().Match(name);
                        if (!match.Success || !int.TryParse(match.Groups[1].Value, out var pid)) continue;
                        var value = Marshal.ReadInt64(item, IntPtr.Size + 8);
                        if (value > 0) result[pid] = result.GetValueOrDefault(pid) + value;
                    }
                    return result;
                }
                finally { Marshal.FreeHGlobal(buffer); }
            }
            finally { PdhCloseQuery(query); }
        }
        catch (Exception e) when (e is DllNotFoundException or EntryPointNotFoundException) { return null; }
    }
    [GeneratedRegex(@"^pid_(\d+)_", RegexOptions.IgnoreCase)] private static partial Regex PidInstance();

    // --- Starting a program again as the signed-in user ---

    /// <summary>True when this process runs elevated (an administrator token with UAC's filter lifted).</summary>
    public static bool IsElevated()
    {
        using var identity = WindowsIdentity.GetCurrent();
        return new WindowsPrincipal(identity).IsInRole(WindowsBuiltInRole.Administrator);
    }

    /// <summary>Starts a program as the signed-in user, never elevated: through the shell when this process is not
    /// elevated, otherwise with the token of the desktop shell (Explorer). Returns null on success, otherwise why not.</summary>
    public static string? StartAsUser(ReopenTarget target)
    {
        if (!File.Exists(target.ImagePath)) return "the program is no longer at " + target.ImagePath;
        var directory = Path.GetDirectoryName(target.ImagePath);
        try
        {
            if (!IsElevated())
            {
                using var process = System.Diagnostics.Process.Start(new System.Diagnostics.ProcessStartInfo(target.ImagePath)
                { Arguments = target.Arguments, UseShellExecute = true, WorkingDirectory = directory ?? "" });
                return null;
            }
            return StartWithShellToken(target.ImagePath, target.Arguments, directory);
        }
        catch (Exception e) when (e is System.ComponentModel.Win32Exception or InvalidOperationException or IOException) { return e.Message; }
    }

    private static string? StartWithShellToken(string image, string arguments, string? directory)
    {
        var shell = GetShellWindow();
        if (shell == IntPtr.Zero || GetWindowThreadProcessId(shell, out var shellPid) == 0) return "no desktop shell to start it from";
        IntPtr process = IntPtr.Zero, token = IntPtr.Zero, primary = IntPtr.Zero;
        try
        {
            process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, false, shellPid);
            if (process == IntPtr.Zero || !OpenProcessToken(process, TOKEN_DUPLICATE, out token)) return "the desktop shell's user could not be read";
            if (!DuplicateTokenEx(token, TOKEN_QUERY | TOKEN_ASSIGN_PRIMARY | TOKEN_DUPLICATE | TOKEN_ADJUST_DEFAULT | TOKEN_ADJUST_SESSIONID, IntPtr.Zero, 2, 1, out primary))
                return "the desktop shell's user could not be used";
            var startup = new STARTUPINFO { cb = Marshal.SizeOf<STARTUPINFO>() };
            var commandLine = "\"" + image + "\"" + (arguments.Length > 0 ? " " + arguments : "");
            if (!CreateProcessWithTokenW(primary, 0, image, new System.Text.StringBuilder(commandLine), 0, IntPtr.Zero, directory, ref startup, out var info))
                return new System.ComponentModel.Win32Exception(Marshal.GetLastWin32Error()).Message;
            CloseHandle(info.hThread); CloseHandle(info.hProcess);
            return null;
        }
        finally
        {
            if (primary != IntPtr.Zero) CloseHandle(primary);
            if (token != IntPtr.Zero) CloseHandle(token);
            if (process != IntPtr.Zero) CloseHandle(process);
        }
    }

    const uint PROCESS_QUERY_LIMITED_INFORMATION = 0x1000;
    const int ProcessCommandLineInformation = 60;
    const int StatusInfoLengthMismatch = unchecked((int)0xC0000004), StatusBufferTooSmall = unchecked((int)0xC0000023);
    const uint PDH_FMT_LARGE = 0x00000400, PDH_FMT_NOCAP100 = 0x00008000, PDH_MORE_DATA = 0x800007D2;
    const uint TOKEN_ASSIGN_PRIMARY = 0x0001, TOKEN_DUPLICATE = 0x0002, TOKEN_QUERY = 0x0008, TOKEN_ADJUST_DEFAULT = 0x0080, TOKEN_ADJUST_SESSIONID = 0x0100;

    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    private struct STARTUPINFO
    {
        public int cb; public string? lpReserved, lpDesktop, lpTitle;
        public int dwX, dwY, dwXSize, dwYSize, dwXCountChars, dwYCountChars, dwFillAttribute, dwFlags;
        public short wShowWindow, cbReserved2; public IntPtr lpReserved2, hStdInput, hStdOutput, hStdError;
    }
    [StructLayout(LayoutKind.Sequential)] private struct PROCESS_INFORMATION { public IntPtr hProcess, hThread; public int dwProcessId, dwThreadId; }

    [DllImport("kernel32.dll", SetLastError = true)] private static extern IntPtr OpenProcess(uint access, bool inherit, uint processId);
    [DllImport("kernel32.dll", SetLastError = true)] private static extern bool CloseHandle(IntPtr handle);
    [DllImport("ntdll.dll")] private static extern int NtQueryInformationProcess(IntPtr process, int informationClass, IntPtr information, int length, out int returnLength);
    [DllImport("pdh.dll", CharSet = CharSet.Unicode)] private static extern uint PdhOpenQueryW(string? source, IntPtr user, out IntPtr query);
    [DllImport("pdh.dll", CharSet = CharSet.Unicode)] private static extern uint PdhAddEnglishCounterW(IntPtr query, string path, IntPtr user, out IntPtr counter);
    [DllImport("pdh.dll")] private static extern uint PdhCollectQueryData(IntPtr query);
    [DllImport("pdh.dll", CharSet = CharSet.Unicode)] private static extern uint PdhGetFormattedCounterArrayW(IntPtr counter, uint format, ref uint bufferSize, out uint itemCount, IntPtr buffer);
    [DllImport("pdh.dll")] private static extern uint PdhCloseQuery(IntPtr query);
    [DllImport("user32.dll")] private static extern IntPtr GetShellWindow();
    [DllImport("user32.dll")] private static extern uint GetWindowThreadProcessId(IntPtr hwnd, out uint processId);
    [DllImport("advapi32.dll", SetLastError = true)] private static extern bool OpenProcessToken(IntPtr process, uint access, out IntPtr token);
    [DllImport("advapi32.dll", SetLastError = true)] private static extern bool DuplicateTokenEx(IntPtr token, uint access, IntPtr attributes, int impersonationLevel, int tokenType, out IntPtr newToken);
    [DllImport("advapi32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    private static extern bool CreateProcessWithTokenW(IntPtr token, uint logonFlags, string application, System.Text.StringBuilder commandLine, uint creationFlags, IntPtr environment, string? currentDirectory, ref STARTUPINFO startup, out PROCESS_INFORMATION information);
}
