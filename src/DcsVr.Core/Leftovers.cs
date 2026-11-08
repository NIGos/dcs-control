namespace DcsVr.Core;

/// <summary>A file of DCS Control's in DCS's folders that the record of originals does not list. <paramref name="RenameTo"/>
/// set: the file is the pilot's own dxgi.dll (ReShade, an overlay) that the DLSS 5 loader chained as dxgi2.dll; it goes
/// back to that name instead of being removed.</summary>
public sealed record LeftoverFile(string Path, string Detail, string? RenameTo = null);

/// <summary>
/// DCS Control's files left in DCS's bin folder and in Saved Games without a record of originals listing them: the record
/// lives in %LOCALAPPDATA%\DcsControl, so a reinstalled Windows, a cleaned or restored profile folder, or a copy of the
/// app with another state folder leaves DCS with the DLSS 5 loader and the engine hook still installed, and Back to
/// stock DCS with nothing to put back. A file counts only when its bytes are exactly one of the components DCS Control
/// has ever shipped (KnownComponents.txt plus this build's). Some of those are byte-identical to Cheeky's public release
/// (the dxgi.dll loader, and the stereo route's Host and Runtime): those count only next to proof that DCS Control put
/// them there, a Cheeky runtime DCS Control built itself (the focus route) or its <see cref="OwnerMark"/>, so a Cheeky
/// the pilot installed by hand is never matched; nor is ReShade or any other mod. The files that only come with those
/// components (Cheeky's INI, the copy of the pilot's DLSS 5 runtime, the logs, the engine INI) go with them, never on
/// their own.
/// </summary>
public static class Leftovers
{
    /// <summary>Hashes of every component file DCS Control has shipped (lower case).</summary>
    public static IReadOnlySet<string> KnownComponentHashes => Known.All;
    /// <summary>The shipped hashes that are byte-identical to Cheeky's public release: the components/cheeky files and
    /// cheeky-focus's loaders (dxgi.dll, the old version.dll) come straight from the pinned archive.</summary>
    public static IReadOnlySet<string> UpstreamHashes => Known.Upstream;
    /// <summary>The file DCS Control writes next to Cheeky's runtime in bin\CheekyFoveatedDLSS (Cheeky rewrites its own INI,
    /// so a mark there would not last): proof that the upstream-identical files beside it are DCS Control's.</summary>
    public const string OwnerMark = "DcsControl.txt";
    public const string OwnerMarkText = "Installed by DCS Control with its DLSS 5 files. Back to stock DCS removes this folder's DCS Control files, this one too.\n";

    private static readonly (IReadOnlySet<string> All, IReadOnlySet<string> Upstream) Known = LoadKnown();
    private static (IReadOnlySet<string>, IReadOnlySet<string>) LoadKnown()
    {
        var hashes = new HashSet<string>(StringComparer.OrdinalIgnoreCase); var upstream = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        using var stream = typeof(Leftovers).Assembly.GetManifestResourceStream("DcsVr.Core.KnownComponents.txt")
            ?? throw new InvalidOperationException("KnownComponents.txt is not embedded in DcsVr.Core.");
        using var reader = new StreamReader(stream);
        while (reader.ReadLine() is { } line)
        {
            var fields = line.Trim().Split(' ', 3);
            if (fields.Length < 2 || fields[0].Length != 64 || !fields[0].All(Uri.IsHexDigit)) continue;
            var hash = fields[0].ToLowerInvariant(); hashes.Add(hash);
            if (IsUpstreamComponent(fields[1])) upstream.Add(hash);
        }
        return (hashes, upstream);
    }
    /// <summary>A components/ path whose file is Cheeky's own release binary, shipped unmodified.</summary>
    public static bool IsUpstreamComponent(string component) =>
        component.StartsWith("cheeky/", StringComparison.OrdinalIgnoreCase)
        || component.Equals("cheeky-focus/dxgi.dll", StringComparison.OrdinalIgnoreCase) || component.Equals("cheeky-focus/version.dll", StringComparison.OrdinalIgnoreCase);

    private static readonly string[] CheekyAnchors = ["CheekyFoveatedDLSSHost.dll", "CheekyFoveatedDLSSRuntime.dll"];
    private static readonly string[] CheekyCompanions = ["CheekyFoveatedDLSS.ini", "nvngx_dlssnr.dll", "CheekyFoveatedDLSS-Host.log", "CheekyFoveatedDLSS-Standalone.log", OwnerMark];
    /// <summary>dxgi.dll is the loader DCS Control installs; version.dll was the Foveated SR loader of 0.2-0.3.</summary>
    private static readonly string[] Loaders = ["dxgi.dll", "version.dll"];
    /// <summary>Folders only DCS Control creates: removed when a leftover sweep leaves them empty.</summary>
    private static readonly string[] OwnFolders = ["CheekyFoveatedDLSS", "payload", "DcsQvCull"];

    /// <summary>The leftovers in the given bin folders (bin, bin-mt) and Saved Games DCS folders (Saved Games\DCS,
    /// DCS.openbeta), in the order <see cref="Remove"/> handles them. Paths in <paramref name="recorded"/> are the record's
    /// and never listed. <paramref name="upstream"/>: the hashes of <paramref name="own"/> that Cheeky's public release
    /// also has (default <see cref="UpstreamHashes"/>).</summary>
    public static IReadOnlyList<LeftoverFile> Find(IEnumerable<string> binFolders, IEnumerable<string> savedGamesDcsFolders, IReadOnlySet<string> own, IEnumerable<string> recorded, IReadOnlySet<string>? upstream = null)
    {
        upstream ??= UpstreamHashes;
        var listed = new HashSet<string>(recorded.Select(SystemPaths.Canonical), StringComparer.OrdinalIgnoreCase);
        var found = new List<LeftoverFile>();
        bool Free(string path) => File.Exists(path) && !listed.Contains(SystemPaths.Canonical(path)) && !found.Any(f => f.Path.Equals(path, StringComparison.OrdinalIgnoreCase));
        bool Own(string path) => Free(path) && HashOf(path) is { } hash && own.Contains(hash);
        // Bytes only DCS Control ever shipped (not also in Cheeky's public release).
        bool Built(string path) => Own(path) && !upstream.Contains(HashOf(path)!);
        void Add(string path, string detail, string? renameTo = null) => found.Add(new(path, detail, renameTo));

        foreach (var bin in Existing(binFolders))
        {
            var cheeky = Path.Combine(bin, "CheekyFoveatedDLSS");
            // Proof that the upstream-identical files here are DCS Control's: a runtime it built, or its mark.
            var proof = CheekyAnchors.Any(f => Built(Path.Combine(cheeky, f))) || Free(Path.Combine(cheeky, OwnerMark));
            var anchors = proof ? CheekyAnchors.Select(f => Path.Combine(cheeky, f)).Where(Own).ToArray() : [];
            foreach (var anchor in anchors) Add(anchor, "DLSS 5 runtime (Cheeky), installed by DCS Control");
            if (proof)
                foreach (var companion in CheekyCompanions.Select(f => Path.Combine(cheeky, f)).Where(Free))
                    Add(companion, companion.EndsWith(".log", StringComparison.OrdinalIgnoreCase) ? "DLSS 5 log" : companion.EndsWith(".ini", StringComparison.OrdinalIgnoreCase) ? "DLSS 5 settings"
                        : companion.EndsWith(OwnerMark, StringComparison.OrdinalIgnoreCase) ? "DCS Control's mark on its DLSS 5 files" : "DCS Control's copy of your DLSS 5 runtime");
            var loaders = Loaders.Select(f => Path.Combine(bin, f)).Where(f => Built(f) || proof && Own(f)).ToArray();
            foreach (var loader in loaders) Add(loader, "DLSS 5 loader, installed by DCS Control");
            var dxgi = Path.Combine(bin, "dxgi.dll"); var chained = Path.Combine(bin, "dxgi2.dll");
            // The pilot's own dxgi.dll, chained after the loader: back to its name once the loader is gone.
            if (loaders.Contains(dxgi, StringComparer.OrdinalIgnoreCase) && Free(chained) && HashOf(chained) is { } second && !own.Contains(second))
                Add(chained, "your own dxgi.dll (ReShade, an overlay), put back as dxgi.dll", dxgi);
            var loaderLog = Path.Combine(bin, "CheekyFoveatedDLSS-Loader.log");
            if ((anchors.Length > 0 || loaders.Length > 0) && Free(loaderLog)) Add(loaderLog, "DLSS 5 loader log");
        }
        foreach (var dcs in Existing(savedGamesDcsFolders))
        {
            var module = Path.Combine(dcs, "Scripts", "DcsQvCull");
            var parts = new[] { Path.Combine(dcs, "Scripts", "Hooks", "DcsQvCull.lua"), Path.Combine(module, "DcsQvCull.dll"), Path.Combine(module, "payload", "DcsQvCullPayload.dll") }.Where(Own).ToArray();
            foreach (var part in parts) Add(part, part.EndsWith(".lua", StringComparison.OrdinalIgnoreCase) ? "Engine Optimizations hook, installed by DCS Control" : "Engine Optimizations module, installed by DCS Control");
            var ini = Path.Combine(module, "DcsQvCull.ini");
            if (parts.Length > 0 && Free(ini)) Add(ini, "Engine Optimizations settings");
        }
        return found;
    }

    /// <summary>Removes the leftovers (or puts the chained dxgi.dll back), then the folders only DCS Control creates when
    /// that leaves them empty. Returns what could not be done; <paramref name="log"/> gets one line per file.</summary>
    public static IReadOnlyList<string> Remove(IReadOnlyList<LeftoverFile> files, Action<string> log)
    {
        var problems = new List<string>();
        foreach (var file in files)
        {
            try
            {
                if (!File.Exists(file.Path)) continue;
                if (file.RenameTo is { } target)
                {
                    if (File.Exists(target)) { problems.Add(file.Path + ": " + Path.GetFileName(target) + " is still there, so it was not put back under that name."); continue; }
                    AtomicFile.Retry(() => File.Move(file.Path, target));
                    log($"Leftover without a record: {file.Path} put back as {target} ({file.Detail}).");
                }
                else
                {
                    AtomicFile.Retry(() => { File.SetAttributes(file.Path, FileAttributes.Normal); File.Delete(file.Path); });
                    log($"Leftover without a record: removed {file.Path} ({file.Detail}).");
                }
            }
            catch (Exception error) when (error is IOException or UnauthorizedAccessException)
            {
                problems.Add(file.Path + ": " + error.Message);
                log("Could not remove leftover " + file.Path + ": " + error.Message);
            }
        }
        // Deepest first, so DcsQvCull\payload goes before DcsQvCull.
        foreach (var folder in files.Select(f => Path.GetDirectoryName(f.Path)).OfType<string>().Distinct(StringComparer.OrdinalIgnoreCase).OrderByDescending(f => f.Length))
            for (var dir = folder; dir is not null && OwnFolders.Contains(Path.GetFileName(dir), StringComparer.OrdinalIgnoreCase); dir = Path.GetDirectoryName(dir))
            {
                try
                {
                    if (!Directory.Exists(dir) || Directory.EnumerateFileSystemEntries(dir).Any()) break;
                    Directory.Delete(dir);
                }
                catch (Exception e) when (e is IOException or UnauthorizedAccessException) { break; }
            }
        return problems;
    }

    private static IEnumerable<string> Existing(IEnumerable<string> folders) =>
        folders.Where(f => !string.IsNullOrWhiteSpace(f)).Select(Path.GetFullPath).Distinct(StringComparer.OrdinalIgnoreCase).Where(Directory.Exists);

    // The Stock DCS page asks on every refresh: a file is hashed again only when its size or time changed.
    private static readonly Dictionary<string, (long Length, DateTime Written, string Hash)> Hashes = new(StringComparer.OrdinalIgnoreCase);
    private static string? HashOf(string path)
    {
        try
        {
            var info = new FileInfo(path);
            if (!info.Exists || info.Length > 64L * 1024 * 1024) return null;
            lock (Hashes)
                if (Hashes.TryGetValue(path, out var known) && known.Length == info.Length && known.Written == info.LastWriteTimeUtc) return known.Hash;
            var hash = Hashing.FileSha256(path).ToLowerInvariant();
            lock (Hashes) Hashes[path] = (info.Length, info.LastWriteTimeUtc, hash);
            return hash;
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException) { return null; }
    }
}
