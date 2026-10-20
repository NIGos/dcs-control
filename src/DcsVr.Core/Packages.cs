using System.IO.Compression;

namespace DcsVr.Core;

public static class PackageCatalog
{
    public static IReadOnlyList<ComponentPackage> All { get; } = [
        new("ofxr", "0.2.1", "OFXR-Bridge-v0.2.1-V116.zip", "https://github.com/tig3rmast3r/OFXR-Bridge/releases/download/0.2.1/OFXR-Bridge-v0.2.1-V116.zip", "db224491cbc9553a51fe55d08623d25864c4ec0a3d49bd76e9d6c4d9b2410c67", "LGPL-3.0-or-later", "https://github.com/tig3rmast3r/OFXR-Bridge/tree/0.2.1"),
        new("cheeky", "0.5.4", "CheekyFoveatedDLSS-0.5.4-Standalone-release.zip", "https://github.com/ClarkCheekyKent/CheekyFoveatedDLSS/releases/download/v0.5.4/CheekyFoveatedDLSS-0.5.4-Standalone-release.zip", "d119a8edaacf0924fa748f6b7a2d13486baa39313e965a3eed6e385234f5116c", "GPL-3.0", "https://github.com/ClarkCheekyKent/CheekyFoveatedDLSS/tree/v0.5.4", ["version.dll"]),
        new("sboys", "1.3.0", "CustomHeadset-1.3.0-Pimax-UI.zip", "https://github.com/sboys3/CustomHeadsetOpenVR/releases/download/1.3.0/CustomHeadset-1.3.0-Pimax-UI.zip", "7e86492148f74b3e6c799bf758979c763c59cd170f59349e6a52cac3f38e99d3", "GPL-2.0 and upstream binary terms", "https://github.com/sboys3/CustomHeadsetOpenVR/tree/1.3.0")
    ];
    public static ComponentPackage Get(string id) => All.Single(p => p.Id == id);
}

public sealed class ComponentCache(string directory)
{
    public string DirectoryPath { get; } = Path.GetFullPath(directory);

    public string Import(ComponentPackage package, string archivePath)
    {
        if (Hashing.FileSha256(archivePath) != package.Sha256) throw new InvalidDataException($"Invalid digest for package {package.Id}.");
        var cachedArchive = PathPolicy.UnderRoot(DirectoryPath, package.ArchiveName);
        if (!Path.GetFullPath(archivePath).Equals(cachedArchive, StringComparison.OrdinalIgnoreCase))
            AtomicFile.Write(cachedArchive, File.ReadAllBytes(archivePath));
        // Each plan gets a fresh verified extraction, so edited cache files cannot enter a deployment.
        var staging = PathPolicy.UnderRoot(DirectoryPath, "staging/" + package.Id + "-" + Guid.NewGuid().ToString("N"));
        ExtractZip(cachedArchive, staging, package.ExcludedEntries);
        return staging;
    }

    /// <summary>Deletes one extraction created by <see cref="Import"/>. Paths outside the staging folder are ignored.</summary>
    public void RemoveStaging(string directory)
    {
        var staging = Path.Combine(DirectoryPath, "staging") + Path.DirectorySeparatorChar;
        var full = Path.GetFullPath(directory);
        if (!full.StartsWith(staging, StringComparison.OrdinalIgnoreCase) || !Directory.Exists(full)) return;
        try { PathPolicy.RejectReparsePoints(full, DirectoryPath); Directory.Delete(full, recursive: true); }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException or InvalidDataException) { /* Retried by RemoveStaleStaging. */ }
    }

    /// <summary>Cleans extractions left by interrupted previews. Prepared Sboys tools (one per Prepare) stay for
    /// <paramref name="preparedAge"/> (7 days by default), the newest always, and one whose tool is running is never
    /// touched: the user may have it open.</summary>
    public void RemoveStaleStaging(TimeSpan age, TimeSpan? preparedAge = null)
    {
        var staging = Path.Combine(DirectoryPath, "staging");
        if (!Directory.Exists(staging)) return;
        var prepared = new List<string>();
        foreach (var directory in Directory.EnumerateDirectories(staging))
            if (Path.GetFileName(directory).StartsWith("sboys-", StringComparison.OrdinalIgnoreCase)) prepared.Add(directory);
            else if (DateTime.UtcNow - Directory.GetCreationTimeUtc(directory) > age) RemoveStaging(directory);
        foreach (var directory in prepared.OrderByDescending(Directory.GetCreationTimeUtc).Skip(1))
            if (DateTime.UtcNow - Directory.GetCreationTimeUtc(directory) > (preparedAge ?? TimeSpan.FromDays(7)) && !ProgramInUse(directory))
                RemoveStaging(directory);
    }

    /// <summary>True when a program or library in <paramref name="directory"/> cannot be opened exclusively: a running
    /// executable's image is locked, so the folder of an open tool is never half deleted.</summary>
    private static bool ProgramInUse(string directory)
    {
        try
        {
            foreach (var file in Directory.EnumerateFiles(directory, "*", SearchOption.AllDirectories)
                .Where(f => f.EndsWith(".exe", StringComparison.OrdinalIgnoreCase) || f.EndsWith(".dll", StringComparison.OrdinalIgnoreCase)))
                try { using var _ = new FileStream(file, FileMode.Open, FileAccess.ReadWrite, FileShare.None); }
                catch (Exception e) when (e is IOException or UnauthorizedAccessException) { return true; }
            return false;
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException) { return true; }
    }

    public static void ExtractZip(string archivePath, string destination, IReadOnlyList<string>? excludedEntries = null)
    {
        bool Excluded(ZipArchiveEntry entry) => excludedEntries?.Contains(entry.FullName.Replace('\\', '/').TrimStart('/'), StringComparer.OrdinalIgnoreCase) == true;
        using var archive = ZipFile.OpenRead(archivePath);
        if (archive.Entries.Count > 10000) throw new InvalidDataException("The package contains too many files.");
        long total = 0;
        var names = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        foreach (var entry in archive.Entries)
        {
            total = checked(total + entry.Length);
            if (entry.Length > 64 * 1024 * 1024 || total > 256 * 1024 * 1024) throw new InvalidDataException("The package is too large.");
            if (((entry.ExternalAttributes >> 16) & 0xF000) == 0xA000) throw new InvalidDataException("The package contains a symbolic link.");
            var target = PathPolicy.UnderRoot(destination, entry.FullName.TrimEnd('/').Replace('/', Path.DirectorySeparatorChar));
            if (!names.Add(target)) throw new InvalidDataException("The package contains duplicate paths.");
            if (entry.FullName.EndsWith('/')) continue;
        }
        Directory.CreateDirectory(destination);
        foreach (var entry in archive.Entries)
        {
            if (entry.FullName.EndsWith('/') || Excluded(entry)) continue;
            var target = PathPolicy.UnderRoot(destination, entry.FullName.Replace('/', Path.DirectorySeparatorChar));
            using var input = entry.Open();
            using var memory = new MemoryStream();
            input.CopyTo(memory);
            if (memory.Length != entry.Length) throw new InvalidDataException("Extracted file size does not match.");
            AtomicFile.Write(target, memory.ToArray());
        }
    }
}
