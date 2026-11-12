namespace DcsVr.Core;

/// <summary>
/// The engine module's shader-variant cache (native/dcsqvcull shadow_inst.h, *.qvc): the instanced shader variants it
/// compiles from DCS's own shader sources, about 200 per airfield session and two ~1.4 s compiles each. Without it the
/// first minutes of a mission run slower (measured 2026-11-11: render thread 16.4-16.9 ms instead of 12.1 ms for 4.5 to
/// 5 minutes). The module used to keep it inside its own folder in Saved Games, so Back to stock DCS deleted it and
/// every restore paid the compile again. It now lives in DCS Control's state folder, one folder per Saved Games
/// variant (state root\engine-shader-cache\DCS, ...\DCS.openbeta), named to the module by [Model] ShaderCacheDir in
/// DcsQvCull.ini:
/// <list type="bullet">
/// <item>Back to stock DCS keeps it: nothing of it is in the game or in Saved Games, and it only holds derived data the
/// module checks before use (cache format, module version, compiler, the hash of every shader source), so entries a DCS
/// or app update made stale are rejected and recompiled.</item>
/// <item>DCS stable and Open Beta each have their own folder: entry names come from the variant's identity only (the
/// sources are checked on load), so a shared folder would make each recompile and overwrite the other's entries.</item>
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
    private const int MaxVariantLength = 40;

    /// <summary>The folder that holds every variant's cache.</summary>
    public static string RootIn(string stateRoot) => Path.Combine(Path.GetFullPath(stateRoot), FolderName);

    /// <summary>The Saved Games folder name of <paramref name="optionsPath"/> (Saved Games\DCS.openbeta\Config\options.lua
    /// gives "DCS.openbeta"), made safe as one folder name; null when there is none.</summary>
    public static string? Variant(string? optionsPath)
    {
        if (string.IsNullOrWhiteSpace(optionsPath)) return null;
        string? name;
        try { name = Path.GetFileName(Path.GetDirectoryName(Path.GetDirectoryName(Path.GetFullPath(optionsPath)))); }
        catch (Exception e) when (e is ArgumentException or NotSupportedException or PathTooLongException) { return null; }
        if (string.IsNullOrEmpty(name)) return null;
        var safe = new string([.. name.Select(c => char.IsAsciiLetterOrDigit(c) || c is '.' or '_' or '-' ? c : '_')]).Trim('.', '_');
        if (safe.Length > MaxVariantLength) safe = safe[..MaxVariantLength].TrimEnd('.');
        return safe.Length == 0 ? null : safe;
    }

    /// <summary>The cache of the DCS whose options.lua is <paramref name="optionsPath"/>: its own folder under
    /// <see cref="RootIn"/>, or null when the variant is unknown (then no cache folder is written to the ini).</summary>
    public static string? DirectoryIn(string stateRoot, string? optionsPath) =>
        Variant(optionsPath) is { } variant ? Path.Combine(RootIn(stateRoot), variant) : null;

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

    /// <summary>The module's own cache folder (Saved Games\DCS*\Scripts\DcsQvCull\cache), where it keeps the cache when
    /// it cannot use [Model] ShaderCacheDir.</summary>
    public static string ModuleCache(string moduleDirectory) => Path.Combine(Path.GetFullPath(moduleDirectory), "cache");

    /// <summary>Why the module did not use [Model] ShaderCacheDir at its latest start (its log line "shadow inst: cache:
    /// [Model] ShaderCacheDir=... not used (why)"), or null when it did or did not say. With <paramref name="directory"/>
    /// only a refusal of that folder counts.</summary>
    public static string? ModuleRefused(IEnumerable<string>? lines, string? directory = null)
    {
        const string Marker = "shadow inst: cache: [Model] ShaderCacheDir=";
        var line = lines?.LastOrDefault(l => l.Contains(Marker, StringComparison.Ordinal) && l.Contains(" not used (", StringComparison.Ordinal));
        if (line is null) return null;
        var rest = line[(line.IndexOf(Marker, StringComparison.Ordinal) + Marker.Length)..];
        var at = rest.LastIndexOf(" not used (", StringComparison.Ordinal);
        var named = rest[..at].Trim().Trim('"').Replace('/', '\\').TrimEnd('\\');
        if (directory is not null && !named.Equals(Path.GetFullPath(directory).TrimEnd('\\'), StringComparison.OrdinalIgnoreCase)) return null;
        var why = rest[(at + 11)..];
        var close = why.IndexOf(')');
        return close > 0 ? why[..close] : why.TrimEnd(')', ';', ' ');
    }

    /// <summary><see cref="ModuleRefused"/> from the module's newest log in <paramref name="moduleDirectory"/>.</summary>
    public static string? ModuleRefusedIn(string moduleDirectory, string? directory = null)
    {
        var log = EngineOptimizations.ModuleLog(moduleDirectory);
        try
        {
            if (!File.Exists(log)) return null;
            using var stream = new FileStream(log, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete);
            using var reader = new StreamReader(stream);
            // The line is written once at payload start: the first 256 KB hold it.
            var head = new char[256 * 1024];
            var read = reader.ReadBlock(head, 0, head.Length);
            return ModuleRefused(new string(head, 0, read).Split('\n').Select(l => l.TrimEnd('\r')), directory);
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException) { return null; }
    }

    /// <summary>The cache folder the module uses for the DCS whose options.lua is <paramref name="optionsPath"/>, for
    /// the Engine page's row and its Clear: this variant's folder in <paramref name="stateRoot"/>, or the module's own
    /// folder when its latest start refused that one (<paramref name="Refused"/> then says why).</summary>
    public static (string? Directory, string? Refused) InUse(string stateRoot, string? optionsPath)
    {
        if (string.IsNullOrWhiteSpace(optionsPath)) return (null, null);
        var configured = DirectoryIn(stateRoot, optionsPath);
        var module = EngineOptimizations.ModuleDirectory(optionsPath);
        if (configured is null) return (ModuleCache(module), null);
        return ModuleRefusedIn(module, configured) is { } why ? (ModuleCache(module), why) : (configured, null);
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
                if (!entry && !IsUnfinished(name)) continue;
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

    /// <summary>How many compiled variants <paramref name="directory"/> holds, and their size (0, 0 when there is
    /// none). For the DCS Engine page.</summary>
    public static (int Files, long Bytes) Stats(string? directory)
    {
        var files = 0; var bytes = 0L;
        try
        {
            if (directory is null || !Directory.Exists(directory) || new DirectoryInfo(directory).Attributes.HasFlag(FileAttributes.ReparsePoint)) return (0, 0);
            foreach (var file in new DirectoryInfo(directory).EnumerateFiles("*.qvc")) { files++; bytes += file.Length; }
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException or System.Security.SecurityException) { }
        return (files, bytes);
    }

    /// <summary>Deletes the cache in <paramref name="directory"/>: its entries and unfinished writes, then the folder if
    /// nothing else is in it. Returns the files deleted and their size. The module compiles the variants again.</summary>
    public static (int Files, long Bytes) Clear(string? directory)
    {
        var files = 0; var bytes = 0L;
        try
        {
            if (directory is null || !Directory.Exists(directory) || new DirectoryInfo(directory).Attributes.HasFlag(FileAttributes.ReparsePoint)) return (0, 0);
            foreach (var file in new DirectoryInfo(directory).EnumerateFiles())
            {
                if (!file.Name.EndsWith(".qvc", StringComparison.OrdinalIgnoreCase) && !IsUnfinished(file.Name)) continue;
                try { var size = file.Length; file.Attributes = FileAttributes.Normal; file.Delete(); files++; bytes += size; }
                catch (Exception e) when (e is IOException or UnauthorizedAccessException) { }
            }
            if (!Directory.EnumerateFileSystemEntries(directory).Any()) Directory.Delete(directory);
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException or System.Security.SecurityException) { }
        return (files, bytes);
    }

    /// <summary>Deletes every variant's cache in <paramref name="stateRoot"/> (and entries an earlier build left directly
    /// in <see cref="RootIn"/>), then the root folder if nothing else is in it. For <c>DcsVr.Cli clear-shader-cache</c>.</summary>
    public static (int Files, long Bytes) ClearAll(string stateRoot)
    {
        var root = RootIn(stateRoot);
        var files = 0; var bytes = 0L;
        try
        {
            if (!Directory.Exists(root) || new DirectoryInfo(root).Attributes.HasFlag(FileAttributes.ReparsePoint)) return (0, 0);
            foreach (var folder in Directory.EnumerateDirectories(root))
            {
                var (f, b) = Clear(folder); files += f; bytes += b;
            }
            var (rf, rb) = Clear(root); files += rf; bytes += rb;
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException or System.Security.SecurityException) { }
        return (files, bytes);
    }

    private static bool IsUnfinished(string name) =>
        name.Contains(".qvc.", StringComparison.OrdinalIgnoreCase) && name.EndsWith(".tmp", StringComparison.OrdinalIgnoreCase);
}
