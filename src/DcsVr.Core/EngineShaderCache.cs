namespace DcsVr.Core;

/// <summary>
/// The engine module's shader-variant cache (native/dcsqvcull shadow_inst.h, *.qvc): the instanced shader variants it
/// compiles from DCS's own shader sources, about 200 per airfield session and two ~1.4 s compiles each. Without it the
/// first minutes of a mission run slower (measured 2026-11-11: render thread 16.4-16.9 ms instead of 12.1 ms for 4.5 to
/// 5 minutes). The module used to keep it inside its own folder in Saved Games, so Back to stock DCS deleted it and
/// every restore paid the compile again. It now lives in DCS Control's state folder (state root\engine-shader-cache),
/// named to the module by [Model] ShaderCacheDir in DcsQvCull.ini:
/// <list type="bullet">
/// <item>Back to stock DCS keeps it: nothing of it is in the game or in Saved Games, and it only holds derived data the
/// module checks before use (cache format, module version, compiler, the hash of every shader source), so entries a DCS
/// or app update made stale are rejected and recompiled.</item>
/// <item>Like the rest of the state folder (the record of original files, the profiles) it stays when DCS Control is
/// uninstalled; <c>DcsVr.Cli clear-shader-cache</c> deletes it (<see cref="Clear"/>), and the module caps it itself
/// (64 MB or 4000 entries, least recently used first).</item>
/// <item>Entries an earlier version left in the module's folder are moved over when a profile is applied
/// (<see cref="Adopt"/>); anything still there is removed by Back to stock DCS as before.</item>
/// </list>
/// </summary>
public static class EngineShaderCache
{
    public const string FolderName = "engine-shader-cache";
    /// <summary>The module refuses a longer folder (DCS.exe is not long-path aware; shader_cache_dir.h kMaxDirChars,
    /// counted with the trailing backslash it adds) and then uses its own folder as before.</summary>
    public const int MaxDirectoryLength = 260 - 48 - 1;

    public static string DirectoryIn(string stateRoot) => Path.Combine(Path.GetFullPath(stateRoot), FolderName);

    /// <summary>The folder to write as [Model] ShaderCacheDir, or null when the module could not use it (then the key
    /// is left out and the cache stays in the module's folder).</summary>
    public static string? ForIni(string? directory)
    {
        if (string.IsNullOrWhiteSpace(directory)) return null;
        var full = Path.GetFullPath(directory).TrimEnd(Path.DirectorySeparatorChar);
        // One line of an ini, read by GetPrivateProfileString: no line break, no leading or trailing blank or quote.
        return full.Length <= MaxDirectoryLength && Path.IsPathFullyQualified(full) && !full.StartsWith(@"\\?\", StringComparison.Ordinal)
            && full.IndexOfAny(['\r', '\n', '"']) < 0 && full == full.Trim() ? full : null;
    }

    /// <summary>Moves the cache entries the module wrote in its own folder (<paramref name="moduleCache"/>, Saved
    /// Games\DCS\Scripts\DcsQvCull\cache) to <paramref name="directory"/>, so an update does not start with an empty
    /// cache. An entry the state folder already has is the newer one and stays; the leftover is deleted. Unfinished
    /// writes (*.tmp) are deleted, and the emptied folder goes. Returns how many entries were moved. Best effort: a
    /// file in use or a folder that cannot be written is skipped (the module then compiles what is missing).</summary>
    public static int Adopt(string moduleCache, string directory)
    {
        var moved = 0;
        try
        {
            if (!Directory.Exists(moduleCache) || Path.GetFullPath(moduleCache).TrimEnd('\\').Equals(Path.GetFullPath(directory).TrimEnd('\\'), StringComparison.OrdinalIgnoreCase)) return 0;
            // A link in the place of either folder is never followed.
            if (new DirectoryInfo(moduleCache).Attributes.HasFlag(FileAttributes.ReparsePoint)) return 0;
            Directory.CreateDirectory(directory);
            if (new DirectoryInfo(directory).Attributes.HasFlag(FileAttributes.ReparsePoint)) return 0;
            foreach (var file in Directory.EnumerateFiles(moduleCache))
            {
                var name = Path.GetFileName(file);
                var entry = name.EndsWith(".qvc", StringComparison.OrdinalIgnoreCase);
                if (!entry && !(name.Contains(".qvc.", StringComparison.OrdinalIgnoreCase) && name.EndsWith(".tmp", StringComparison.OrdinalIgnoreCase))) continue;
                try
                {
                    var target = Path.Combine(directory, name);
                    File.SetAttributes(file, FileAttributes.Normal);
                    if (entry && !File.Exists(target)) { File.Move(file, target); moved++; }
                    else File.Delete(file);
                }
                catch (Exception e) when (e is IOException or UnauthorizedAccessException) { }
            }
            if (!Directory.EnumerateFileSystemEntries(moduleCache).Any()) Directory.Delete(moduleCache);
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException or System.Security.SecurityException) { }
        return moved;
    }

    /// <summary>Deletes the cache in <paramref name="stateRoot"/>: its entries and unfinished writes, then the folder if
    /// nothing else is in it. Returns the files deleted and their size. The module compiles the variants again.</summary>
    public static (int Files, long Bytes) Clear(string stateRoot)
    {
        var directory = DirectoryIn(stateRoot);
        var files = 0; var bytes = 0L;
        try
        {
            if (!Directory.Exists(directory) || new DirectoryInfo(directory).Attributes.HasFlag(FileAttributes.ReparsePoint)) return (0, 0);
            foreach (var file in new DirectoryInfo(directory).EnumerateFiles())
            {
                if (!file.Name.EndsWith(".qvc", StringComparison.OrdinalIgnoreCase) && !(file.Name.Contains(".qvc.", StringComparison.OrdinalIgnoreCase) && file.Name.EndsWith(".tmp", StringComparison.OrdinalIgnoreCase))) continue;
                try { var size = file.Length; file.Attributes = FileAttributes.Normal; file.Delete(); files++; bytes += size; }
                catch (Exception e) when (e is IOException or UnauthorizedAccessException) { }
            }
            if (!Directory.EnumerateFileSystemEntries(directory).Any()) Directory.Delete(directory);
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException or System.Security.SecurityException) { }
        return (files, bytes);
    }
}
