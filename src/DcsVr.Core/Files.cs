using System.Security.Cryptography;
using System.Text;

namespace DcsVr.Core;

public static class Hashing
{
    public static string BytesSha256(ReadOnlySpan<byte> bytes) => Convert.ToHexStringLower(SHA256.HashData(bytes));
    public static string FileSha256(string path)
    {
        using var stream = File.OpenRead(path);
        return Convert.ToHexStringLower(SHA256.HashData(stream));
    }
}

public static class AtomicFile
{
    public static void Write(string path, byte[] bytes)
    {
        var directory = Path.GetDirectoryName(Path.GetFullPath(path))!;
        Directory.CreateDirectory(directory);
        var temporary = Path.Combine(directory, ".dcs-vr-" + Guid.NewGuid().ToString("N") + ".tmp");
        try
        {
            using (var stream = new FileStream(temporary, FileMode.CreateNew, FileAccess.Write, FileShare.None, 65536, FileOptions.WriteThrough))
            { stream.Write(bytes); stream.Flush(flushToDisk: true); }
            // A file the pilot made read-only (options.lua, typically) is replaced and stays read-only.
            var attributes = File.Exists(path) ? File.GetAttributes(path) : (FileAttributes?)null;
            var readOnly = attributes is { } a && a.HasFlag(FileAttributes.ReadOnly);
            if (readOnly) File.SetAttributes(path, attributes!.Value & ~FileAttributes.ReadOnly);
            try { Retry(() => File.Move(temporary, path, overwrite: true)); }
            finally { if (readOnly && File.Exists(path)) File.SetAttributes(path, File.GetAttributes(path) | FileAttributes.ReadOnly); }
            RemoveLeftovers(directory);
        }
        finally { if (File.Exists(temporary)) try { File.Delete(temporary); } catch (Exception e) when (e is IOException or UnauthorizedAccessException) { } }
    }

    /// <summary>Temporary files of a write that a crash or power loss interrupted (".dcs-vr-*.tmp", older than an hour)
    /// are removed the next time the folder is written; a current write's file is never touched.</summary>
    private static void RemoveLeftovers(string directory)
    {
        try
        {
            foreach (var file in new DirectoryInfo(directory).EnumerateFiles(".dcs-vr-*.tmp"))
                if (DateTime.UtcNow - file.LastWriteTimeUtc > TimeSpan.FromHours(1)) file.Delete();
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException) { }
    }

    /// <summary>Runs <paramref name="action"/> again for about 2 s while another program holds the file (an antivirus
    /// scanning a new DLL, Windows Search, OneDrive): sharing and lock violations, and access denied on a file that is
    /// being scanned. A real permission problem still fails after the last try.</summary>
    public static void Retry(Action action)
    {
        for (var attempt = 1; ; attempt++)
        {
            try { action(); return; }
            catch (Exception e) when (attempt < 8 && (e is UnauthorizedAccessException || (e is IOException io && (io.HResult & 0xFFFF) is 32 or 33)))
            { Thread.Sleep(50 * attempt); }
        }
    }
    public static void WriteText(string path, string content) => Write(path, new UTF8Encoding(false).GetBytes(content));
}

/// <summary>UTF-8 without a byte order mark that refuses invalid bytes instead of replacing them: options.lua is read
/// and written back with it, so a file saved in another encoding is reported, never silently changed.</summary>
internal static class StrictUtf8
{
    public static readonly UTF8Encoding Encoding = new(false, true);
}

public static class PathPolicy
{
    public static string UnderRoot(string root, string relative)
    {
        if (Path.IsPathFullyQualified(relative) || relative.Contains(':')) throw new InvalidDataException("The path must be relative and contain no NTFS streams.");
        foreach (var segment in relative.Replace('/', '\\').Split('\\'))
        {
            if (segment is "" or "." or ".." || segment.EndsWith('.') || segment.EndsWith(' ') || segment.IndexOfAny(Path.GetInvalidFileNameChars()) >= 0)
                throw new InvalidDataException("The path contains an invalid name.");
            var stem = segment.Split('.')[0].ToUpperInvariant();
            if (stem is "CON" or "PRN" or "AUX" or "NUL" || (stem.Length == 4 && (stem.StartsWith("COM") || stem.StartsWith("LPT")) && stem[3] is >= '1' and <= '9'))
                throw new InvalidDataException("The path contains a reserved Windows device name.");
        }
        var canonicalRoot = Path.GetFullPath(root).TrimEnd(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar);
        var full = Path.GetFullPath(Path.Combine(canonicalRoot, relative));
        if (!full.StartsWith(canonicalRoot + Path.DirectorySeparatorChar, StringComparison.OrdinalIgnoreCase))
            throw new InvalidDataException("The path escapes the authorized folder.");
        RejectReparsePoints(full, canonicalRoot);
        return full;
    }

    /// <summary>
    /// Rejects a symlink or junction at the destination and at every folder between it and <paramref name="root"/>.
    /// Links above the root are allowed: moving DCS or Saved Games to another drive with a junction is common,
    /// and the user selected that location explicitly.
    /// </summary>
    public static void RejectReparsePoints(string fullPath, string? root = null)
    {
        var current = Path.GetFullPath(fullPath).TrimEnd(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar);
        var stop = root is null ? null : Path.GetFullPath(root).TrimEnd(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar);
        while (!string.IsNullOrEmpty(current))
        {
            // Only real links count: OneDrive Files On-Demand and other cloud files carry a reparse point too, with no link target.
            if ((File.Exists(current) || Directory.Exists(current)) && (File.GetAttributes(current) & FileAttributes.ReparsePoint) != 0
                && (File.Exists(current) ? new FileInfo(current).LinkTarget : new DirectoryInfo(current).LinkTarget) is not null)
                throw new InvalidDataException("Symlink and junction destinations are unsupported: " + current);
            if (stop is null) return;
            current = Path.GetDirectoryName(current);
            if (current is null || current.Length <= stop.Length) return;
        }
    }
}
