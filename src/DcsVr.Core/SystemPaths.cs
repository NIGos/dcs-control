using System.Runtime.InteropServices;
using Microsoft.Win32.SafeHandles;

namespace DcsVr.Core;

/// <summary>Windows path facts CPU Boost and the DCS-running guard need: the real (possibly redirected) Saved Games
/// folder, a canonical form of a path that sees through junctions, symbolic links and 8.3 short names, and the image
/// path of a running process. Read-only.</summary>
public static class SystemPaths
{
    /// <summary>The Saved Games known folder (FOLDERID_SavedGames), which may be redirected to another drive; falls
    /// back to %USERPROFILE%\Saved Games.</summary>
    public static string SavedGames()
    {
        var id = new Guid("4C5C32FF-BB9D-43B0-B5B4-2D72E54EAAA4");
        var pointer = IntPtr.Zero;
        try { if (SHGetKnownFolderPath(ref id, 0x4000 /* KF_FLAG_DONT_VERIFY */, IntPtr.Zero, out pointer) == 0 && Marshal.PtrToStringUni(pointer) is { Length: > 0 } path) return path; }
        catch (Exception e) when (e is DllNotFoundException or EntryPointNotFoundException) { }
        finally { if (pointer != IntPtr.Zero) Marshal.FreeCoTaskMem(pointer); }
        return DefaultSavedGames;
    }

    /// <summary>%USERPROFILE%\Saved Games, the location when the known folder is not redirected.</summary>
    public static string DefaultSavedGames => Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.UserProfile), "Saved Games");

    /// <summary>Both Saved Games locations (known folder first), distinct after canonicalization.</summary>
    public static IReadOnlyList<string> SavedGamesCandidates() =>
        new[] { SavedGames(), DefaultSavedGames }.Select(Canonical).Distinct(StringComparer.OrdinalIgnoreCase).ToArray();

    /// <summary>Canonical full path: the longest existing prefix is resolved through the file system (junctions,
    /// symbolic links, 8.3 names, case) and the not-yet-existing remainder is appended. Never throws for a
    /// well-formed path; falls back to <see cref="Path.GetFullPath(string)"/>.</summary>
    public static string Canonical(string path)
    {
        string full;
        try { full = Path.GetFullPath(path); } catch (Exception e) when (e is ArgumentException or NotSupportedException or PathTooLongException) { return path; }
        var remainder = new Stack<string>();
        var current = Path.TrimEndingDirectorySeparator(full);
        for (var guard = 0; guard < 64 && !string.IsNullOrEmpty(current); guard++)
        {
            if (FinalPath(current) is { } resolved)
            {
                var result = resolved;
                while (remainder.Count > 0) result = Path.Combine(result, remainder.Pop());
                return result;
            }
            var parent = Path.GetDirectoryName(current);
            if (parent is null) break;
            remainder.Push(Path.GetFileName(current));
            current = parent;
        }
        return full;
    }

    /// <summary>True when <paramref name="path"/> is <paramref name="root"/> itself or inside it, comparing canonical
    /// forms of both.</summary>
    public static bool IsUnderOrSame(string path, string root)
    {
        var p = Path.TrimEndingDirectorySeparator(Canonical(path));
        var r = Path.TrimEndingDirectorySeparator(Canonical(root));
        return p.Equals(r, StringComparison.OrdinalIgnoreCase) || p.StartsWith(r + Path.DirectorySeparatorChar, StringComparison.OrdinalIgnoreCase);
    }

    /// <summary>Full image path of a running process, readable with PROCESS_QUERY_LIMITED_INFORMATION (works for
    /// processes of the same user without reading their memory); null when it cannot be read.</summary>
    public static string? ProcessImagePath(int pid)
    {
        var handle = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, false, (uint)pid);
        if (handle == IntPtr.Zero) return null;
        try
        {
            var buffer = new char[32768];
            var size = (uint)buffer.Length;
            return QueryFullProcessImageName(handle, 0, buffer, ref size) ? new string(buffer, 0, (int)size) : null;
        }
        finally { CloseHandle(handle); }
    }

    static string? FinalPath(string path)
    {
        using var handle = CreateFile(path, 0, FILE_SHARE_ALL, IntPtr.Zero, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, IntPtr.Zero);
        if (handle.IsInvalid) return null;
        var buffer = new char[32768];
        var length = GetFinalPathNameByHandle(handle, buffer, (uint)buffer.Length, 0 /* FILE_NAME_NORMALIZED | VOLUME_NAME_DOS */);
        if (length == 0 || length >= buffer.Length) return null;
        var result = new string(buffer, 0, (int)length);
        if (result.StartsWith(@"\\?\UNC\", StringComparison.OrdinalIgnoreCase)) return @"\\" + result[8..];
        if (result.StartsWith(@"\\?\", StringComparison.Ordinal)) return result[4..];
        return result;
    }

    const uint FILE_SHARE_ALL = 0x1 | 0x2 | 0x4, OPEN_EXISTING = 3, FILE_FLAG_BACKUP_SEMANTICS = 0x02000000;
    const uint PROCESS_QUERY_LIMITED_INFORMATION = 0x1000;

    [DllImport("shell32.dll")] private static extern int SHGetKnownFolderPath(ref Guid id, uint flags, IntPtr token, out IntPtr path);
    [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    private static extern SafeFileHandle CreateFile(string name, uint access, uint share, IntPtr security, uint disposition, uint flags, IntPtr template);
    [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    private static extern uint GetFinalPathNameByHandle(SafeFileHandle file, [Out] char[] path, uint length, uint flags);
    [DllImport("kernel32.dll", SetLastError = true)] private static extern IntPtr OpenProcess(uint access, bool inherit, uint processId);
    [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    private static extern bool QueryFullProcessImageName(IntPtr process, uint flags, [Out] char[] name, ref uint size);
    [DllImport("kernel32.dll", SetLastError = true)] private static extern bool CloseHandle(IntPtr handle);
}

/// <summary>
/// Detects a process whose writes under %LOCALAPPDATA% Windows redirects to a packaged app's private folder
/// (%LOCALAPPDATA%\Packages\&lt;package&gt;\LocalCache\Local): DCS Control started from inside such an app (or its
/// sandboxed shell) would keep its originals in a private copy that the normal app never sees, while the DCS files
/// themselves are shared. That split is how two "applied" profiles and files "already existing" without an owner came
/// about. The check writes and removes one empty probe file.
/// </summary>
public static class AppDataRedirection
{
    /// <summary>The private folder that receives this process's %LOCALAPPDATA% writes, or null when they are not
    /// redirected (or the check cannot run).</summary>
    public static string? Target()
    {
        var local = Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData);
        if (string.IsNullOrEmpty(local)) return null;
        var name = ".dcsvr-probe-" + Guid.NewGuid().ToString("N");
        var probe = Path.Combine(local, name);
        try
        {
            File.WriteAllBytes(probe, []);
            var packages = Path.Combine(local, "Packages");
            if (!Directory.Exists(packages)) return null;
            foreach (var package in Directory.EnumerateDirectories(packages))
            {
                var redirected = Path.Combine(package, "LocalCache", "Local");
                if (File.Exists(Path.Combine(redirected, name))) return redirected;
            }
            return null;
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException) { return null; }
        finally { try { File.Delete(probe); } catch (Exception e) when (e is IOException or UnauthorizedAccessException) { } }
    }

    public const string Message = "DCS Control was started inside another app's sandbox (Windows redirects its %LOCALAPPDATA% to {0}). Its record of original files would go to a private copy that the normal app never sees, while the DCS files are shared. Start DCS Control from the Start menu or Explorer instead.";
}
