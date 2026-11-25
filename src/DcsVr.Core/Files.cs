using System.Security.Cryptography;
using System.Text;

namespace DcsVr.Core;

public static class Hashing
{
    public static string BytesSha256(ReadOnlySpan<byte> bytes) => Convert.ToHexStringLower(SHA256.HashData(bytes));

    /// <summary>The file's SHA-256, read now. Inside a <see cref="Scope"/> a file whose size and times did not change since
    /// it was hashed in the same scope is not read again (one launch verifies, plans and applies the same files).</summary>
    public static string FileSha256(string path)
    {
        if (ScopeMemo.Value is not { } memo || Identity(path) is not { } before) return HashFile(path);
        if (memo.TryGetValue(before, out var known)) return known;
        var hash = HashFile(path);
        if (Identity(path) == before) memo[before] = hash;
        return hash;
    }

    /// <summary>The file's SHA-256, kept for the life of the process and computed again when its size, write time or
    /// creation time changed (a file written in the last two seconds is always hashed). For planning, the identical-file
    /// check of an apply and display: the tamper checks before a launch use <see cref="FileSha256"/>.</summary>
    public static string FileSha256Cached(string path)
    {
        if (Identity(path) is not { } before) return HashFile(path);
        if (Known.TryGetValue(before, out var known)) return known;
        var hash = FileSha256(path);
        if (Identity(path) == before) Known[before] = hash;
        return hash;
    }

    /// <summary>The bytes of a file whose content goes into a plan. Their hash (<see cref="ContentSha256"/>) comes with them:
    /// the cached one when the file did not change while it was read, otherwise one pass over the bytes read.</summary>
    public static byte[] ReadAllBytes(string path)
    {
        var before = Identity(path);
        var bytes = File.ReadAllBytes(path);
        if (before is { } id && Identity(path) == id)
        {
            if (!Known.TryGetValue(id, out var hash)) Known[id] = hash = BytesSha256(bytes);
            Contents.AddOrUpdate(bytes, hash);
        }
        return bytes;
    }

    /// <summary>SHA-256 of planned content, computed once per array. Plan contents are never changed after they are made.</summary>
    public static string ContentSha256(byte[] bytes)
    {
        if (Contents.TryGetValue(bytes, out var known)) return known;
        var hash = BytesSha256(bytes);
        Contents.AddOrUpdate(bytes, hash);
        return hash;
    }

    /// <summary>Within the returned scope (one launch, one apply) <see cref="FileSha256"/> reads each unchanged file once.
    /// Nested scopes share the outer one.</summary>
    public static IDisposable Scope()
    {
        if (ScopeMemo.Value is not null) return NoScope.Instance;
        ScopeMemo.Value = new();
        return new ScopeEnd();
    }

    private static string HashFile(string path)
    {
        using var stream = File.OpenRead(path);
        return Convert.ToHexStringLower(SHA256.HashData(stream));
    }
    private readonly record struct FileIdentity(string Path, long Length, DateTime Written, DateTime Created);
    // A file written in the last two seconds (or dated in the future) is never cached: two writes within one tick of the
    // file system's clock can leave the same size and times (git's "racy" entries), so its hash is always computed.
    private static readonly TimeSpan Settle = TimeSpan.FromSeconds(2);
    private static FileIdentity? Identity(string path)
    {
        try
        {
            var info = new FileInfo(path);
            if (!info.Exists) return null;
            var now = DateTime.UtcNow;
            if (now - info.LastWriteTimeUtc < Settle || now - info.CreationTimeUtc < Settle) return null;
            return new(info.FullName.ToUpperInvariant(), info.Length, info.LastWriteTimeUtc, info.CreationTimeUtc);
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException or ArgumentException or NotSupportedException) { return null; }
    }
    private static readonly System.Collections.Concurrent.ConcurrentDictionary<FileIdentity, string> Known = new();
    private static readonly System.Runtime.CompilerServices.ConditionalWeakTable<byte[], string> Contents = new();
    private static readonly AsyncLocal<System.Collections.Concurrent.ConcurrentDictionary<FileIdentity, string>?> ScopeMemo = new();
    private sealed class ScopeEnd : IDisposable { public void Dispose() => ScopeMemo.Value = null; }
    private sealed class NoScope : IDisposable { public static readonly NoScope Instance = new(); public void Dispose() { } }
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

public static class FileProblem
{
    /// <summary>A file that could not be changed, as a refusal names it: Windows' own messages usually quote the path
    /// already, so it is put in front only when they do not.</summary>
    public static string Describe(string path, Exception error) =>
        Plain(error.Message).Contains(Plain(path), StringComparison.OrdinalIgnoreCase) ? error.Message : path + ": " + error.Message;
    private static string Plain(string text) => text.Replace('/', '\\');
}
