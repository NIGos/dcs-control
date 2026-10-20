using System.Text;

namespace DcsVr.Core;

/// <summary>One path DCS Control has written, with what was there before it first touched it.</summary>
public sealed record OriginalFile
{
    public required string Path { get; init; }
    /// <summary>Folder the user authorized (bin, Config, managed state). Links below it are rejected at every write.</summary>
    public string? Root { get; init; }
    /// <summary>Hash of the original bytes; null when the path did not exist (Back to stock DCS removes it).</summary>
    public string? OriginalSha256 { get; init; }
    /// <summary>Copy of the original bytes in the originals folder; null when the original was absent.</summary>
    public string? BackupFile { get; init; }
    /// <summary>The original belonged to another program (another mod's dxgi.dll, ReShade): backed up and replaced.</summary>
    public bool Foreign { get; init; }
    /// <summary>A file identical to one of DCS Control's own components was there (left by an earlier install or
    /// another copy of the app's state): it counts as absent, so Back to stock DCS removes it.</summary>
    public bool Leftover { get; init; }
    /// <summary>options.lua: only these settings are owned. Setting path to its original literal (null: it was absent).
    /// Back to stock DCS sets back only these keys and keeps every other edit.</summary>
    public Dictionary<string, string?>? Settings { get; set; }
    /// <summary>Log files the installed component writes at runtime. Back to stock DCS removes them with it.</summary>
    public List<string>? RuntimeLogs { get; set; }
    public DateTimeOffset Since { get; init; } = DateTimeOffset.UtcNow;
}

/// <summary>The state of DCS Control's writes: the original of every path it changed, and the profile written last.</summary>
public sealed record OriginalsBaseline
{
    public int SchemaVersion { get; init; } = 2;
    public List<OriginalFile> Files { get; init; } = [];
    /// <summary>The profile written last, complete. Null while nothing is applied or after an interrupted apply.</summary>
    public TransactionJournal? Current { get; set; }
    /// <summary>clean, applying, applied or restoring.</summary>
    public string State { get; set; } = "clean";
    public DateTimeOffset? LastActionAt { get; set; }
    public string? LastAction { get; set; }
    /// <summary>What the last complete apply installed whole (path → hashes), kept while an apply or Back to stock DCS
    /// runs: a resumed run still tells the pilot's newer files from DCS Control's own.</summary>
    public Dictionary<string, HashSet<string>>? LastInstalled { get; set; }
}

/// <param name="Action">restore (the backup is put back), remove (DCS Control created it) or settings (owned
/// options.lua settings are set back, other edits kept).</param>
public sealed record OriginalItem(string Path, string Action, bool Foreign, string? Detail);
public sealed record OriginalsStatus(int Count, IReadOnlyList<OriginalItem> Files, string? ProfileId, string State, DateTimeOffset? LastActionAt, string? LastAction, string Folder, TransactionJournal? Current);
/// <param name="ReplacedForeign">Files of another program this apply backed up and replaced.</param>
/// <param name="PutBack">Paths an earlier profile wrote that this one does not use: set back to their originals.</param>
public sealed record OriginalsApply(TransactionJournal Current, IReadOnlyList<string> ReplacedForeign, IReadOnlyList<string> PutBack);

/// <summary>
/// One mental model for DCS Control's writes. The first time a path is written its original is backed up (or
/// recorded as absent); later applies simply overwrite whatever is there, including files of an earlier profile,
/// leftovers and other programs' files (backed up first). Back to stock DCS puts every path back to its original
/// whatever it holds now; options.lua is handled setting by setting. Replaces the per-apply journals (folder
/// "transactions"), which are converted once on first use.
/// </summary>
/// <param name="legacyFolders">Journal folders of earlier versions, oldest data first; read once to build the originals.</param>
/// <param name="ownHashes">Hashes of DCS Control's own components: an existing file with these bytes counts as absent.</param>
public sealed class OriginalsStore(string directory, Func<IEnumerable<string>>? legacyFolders = null, Func<IReadOnlySet<string>>? ownHashes = null)
{
    public const int Schema = 2;
    public string Directory { get; } = System.IO.Path.GetFullPath(directory);
    private string BaselinePath => System.IO.Path.Combine(Directory, "baseline.json");
    /// <summary>What conversions and restores did, appended (kept under 1 MB).</summary>
    public string LogPath => System.IO.Path.Combine(Directory, "originals.log");

    /// <summary>The originals (converting the journals of an earlier version on first use).</summary>
    public OriginalsBaseline Read()
    {
        using var exclusive = Lock();
        return LoadCore();
    }

    /// <summary>The applied profile without locking or converting anything: for the CPU Boost helper and quick checks.</summary>
    public TransactionJournal? ReadCurrent()
    {
        try { return File.Exists(BaselinePath) && JsonData.Deserialize<OriginalsBaseline>(File.ReadAllText(BaselinePath)) is { State: "applied", Current: { } current } ? current : null; }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException or System.Text.Json.JsonException or InvalidDataException) { return null; }
    }

    public OriginalsStatus Status()
    {
        var baseline = Read();
        var items = baseline.Files.AsEnumerable().Reverse().Select(f => new OriginalItem(f.Path,
            f.Settings is not null ? "settings" : f.BackupFile is not null ? "restore" : "remove", f.Foreign,
            f.Settings is { Count: > 0 } settings ? string.Join(", ", settings.Keys) : f.Settings is not null ? "no settings changed now"
            : f.Foreign ? "another program's file, backed up" : f.Leftover ? "left by DCS Control earlier" : null)).ToArray();
        return new(items.Length, items, baseline.Current?.ProfileId, baseline.State, baseline.LastActionAt, baseline.LastAction, Directory, baseline.Current);
    }

    /// <summary>The owned options.lua settings with their original values, for the planner (null when none).</summary>
    public IReadOnlyDictionary<string, string?>? OwnedSettings(string optionsPath)
    {
        // Matched like Apply matches (also under another spelling of the same file: a junction, subst drive, 8.3 name or
        // redirected Saved Games), so the planner sees the owned settings Apply will merge into.
        return Find(Read(), System.IO.Path.GetFullPath(optionsPath))?.Settings;
    }

    /// <summary>
    /// Writes <paramref name="plan"/>: originals of paths written for the first time are backed up (and saved) before
    /// anything is written, paths an earlier profile wrote that this plan does not use are set back to their originals,
    /// then every file is overwritten. options.lua gets only the plan's settings, applied to the file as it is now.
    /// An interrupted apply leaves every original recorded: launching again or Back to stock DCS finishes the job.
    /// </summary>
    public OriginalsApply Apply(ApplyPlan plan, Action<int>? beforeWrite = null)
    {
        using var exclusive = Lock();
        var baseline = LoadCore();
        if (plan.Files.Select(f => System.IO.Path.GetFullPath(f.Path)).Distinct(StringComparer.OrdinalIgnoreCase).Count() != plan.Files.Count)
            throw new InvalidDataException("The plan contains duplicate destinations.");
        if (plan.Files.Any(f => f.Delete)) throw new InvalidDataException("A profile plan never deletes files.");
        foreach (var file in plan.Files) PathPolicy.RejectReparsePoints(file.Path, file.Root);
        var own = ownHashes?.Invoke() ?? new HashSet<string>();
        var replaced = new List<string>();
        var originals = new List<OriginalFile>();
        var superseded = new List<OriginalFile>();
        // 1. The original of every path is recorded before anything is written.
        foreach (var file in plan.Files)
        {
            var full = System.IO.Path.GetFullPath(file.Path);
            var original = Find(baseline, full);
            if (original is null)
            {
                original = Capture(file, full, own);
                baseline.Files.Add(original);
                if (original.Foreign) replaced.Add(full);
                Log($"First write of {full}: " + (original.Foreign ? "another program's file, backed up as " + original.BackupFile : original.BackupFile is not null ? "original backed up as " + original.BackupFile : original.Leftover ? "a file identical to a DCS Control component, counted as absent" : "absent before"));
            }
            else if (ReplacedSinceInstall(baseline, original, full, own, file) is { } newer)
            {
                // The pilot or another program put a newer file there after DCS Control installed its own (a ReShade
                // update, for example): that file becomes the original Back to stock DCS puts back.
                var at = baseline.Files.IndexOf(original);
                superseded.Add(original);
                original = Capture(file, full, own) with { Since = original.Since };
                baseline.Files[at] = original;
                if (original.Foreign) replaced.Add(full);
                Log($"{full} was replaced after DCS Control installed it ({newer}); the new file is backed up as {original.BackupFile}.");
            }
            if (file.LuaChanges is { } changes)
            {
                original.Settings ??= [];
                foreach (var change in changes) original.Settings.TryAdd(change.Path, change.PreviousRaw);
            }
            if (file.RuntimeLogs is { Count: > 0 } logs) original.RuntimeLogs = [.. (original.RuntimeLogs ?? []).Union(logs.Select(System.IO.Path.GetFullPath), StringComparer.OrdinalIgnoreCase)];
            originals.Add(original);
        }
        // What the last apply installed, read before the record is cleared: a stale file replaced since is the pilot's.
        var installedBefore = InstalledHashes(baseline) ?? Recorded(baseline.LastInstalled);
        baseline.LastInstalled = installedBefore;
        baseline.State = "applying"; baseline.Current = null; Save(baseline);
        // The backups of originals a newer file took over from are no longer referenced once the record is saved.
        foreach (var old in superseded) DeleteBackup(old);
        var putBack = new List<string>();
        var index = -1;
        try
        {
            // 2. Paths an earlier profile wrote that this one does not use go back to their originals (the dxgi.dll
            // loader and dxgi2.dll prefetch fix that 0.5.4 and earlier put in bin, for example).
            var planned = new HashSet<OriginalFile>(originals, ReferenceEqualityComparer.Instance);
            foreach (var stale in baseline.Files.AsEnumerable().Reverse().Where(f => !planned.Contains(f)).ToArray())
            {
                if (RootGone(stale))
                {
                    Log($"{stale.Root} no longer exists; {stale.Path} is not put back (its backup stays as {stale.BackupFile ?? "none"}).");
                    baseline.Files.Remove(stale); Save(baseline);
                    continue;
                }
                try
                {
                    if (PilotsNewerFile(installedBefore, stale, own))
                        // Updated by its program after DCS Control installed its file (a ReShade update): the newer file
                        // stays, never the older original over it.
                        Log($"{stale.Path} was replaced after DCS Control installed it; the newer file is left as it is.");
                    else RestoreEntry(stale, out _);
                }
                catch (Exception error) when (error is InvalidDataException or FileNotFoundException or DirectoryNotFoundException or IOException or UnauthorizedAccessException)
                {
                    // A backup that is missing or damaged (quarantined by an antivirus, for example), or a stale file that
                    // stays locked or protected (Controlled Folder Access), must not block every later apply: the entry
                    // stays listed for Back to stock DCS, which reports it.
                    Log("Could not put back " + stale.Path + ": " + error.Message);
                    continue;
                }
                // The baseline forgets the entry before its backup goes: a crash in between leaves an unused backup,
                // never an entry pointing at a deleted one.
                baseline.Files.Remove(stale); Save(baseline); DeleteBackup(stale);
                putBack.Add(stale.Path);
            }
            // 3. Every file is written over whatever is there now.
            var entries = new List<JournalEntry>();
            for (index = 0; index < plan.Files.Count; index++)
            {
                beforeWrite?.Invoke(index);
                var file = plan.Files[index]; var original = originals[index];
                var full = System.IO.Path.GetFullPath(file.Path);
                PathPolicy.RejectReparsePoints(full, file.Root);
                var released = new List<string>();
                var bytes = file.LuaChanges is { } changes ? MergeSettings(full, original, changes, released) : file.Content;
                AtomicFile.Write(full, bytes);
                // Settings set back are forgotten only once the file says so: a failed write keeps their originals.
                foreach (var key in released) original.Settings!.Remove(key);
                entries.Add(new() { Path = full, InstalledSha256 = Hashing.BytesSha256(bytes), PreviousSha256 = original.OriginalSha256, LuaChanges = file.LuaChanges, IniValues = file.IniValues, RuntimeLogs = file.RuntimeLogs, Root = file.Root is null ? null : System.IO.Path.GetFullPath(file.Root), Applied = true });
            }
            baseline.Current = new() { Id = Guid.NewGuid().ToString("N"), ProfileId = plan.ProfileId, Status = "applied", Entries = entries };
            baseline.LastInstalled = null;
            baseline.State = "applied"; baseline.LastAction = "Applied " + plan.Description; baseline.LastActionAt = DateTimeOffset.UtcNow;
            Save(baseline);
            Log($"Applied {plan.ProfileId}: {entries.Count} files written, {putBack.Count} put back, {replaced.Count} other programs' files replaced.");
            return new(baseline.Current, replaced, putBack);
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException or InvalidDataException or DecoderFallbackException or ArgumentException)
        {
            baseline.LastAction = "Apply of " + plan.Description + " stopped"; baseline.LastActionAt = DateTimeOffset.UtcNow;
            try { Save(baseline); } catch (Exception e) when (e is IOException or UnauthorizedAccessException) { }
            var where = index >= 0 && index < plan.Files.Count ? " at " + plan.Files[index].Path : "";
            Log($"Apply of {plan.ProfileId} stopped{where}: {error.Message}");
            // A protected folder gets the permission advice (Launch then asks Windows itself); a file refused although its
            // folder is writable is something else holding it.
            var folder = index >= 0 && index < plan.Files.Count ? System.IO.Path.GetDirectoryName(plan.Files[index].Path) : null;
            var advice = error is not UnauthorizedAccessException ? ""
                : folder is not null && !LaunchSafety.CanWrite(folder) ? " " + LaunchSafety.WriteAdvice(folder)
                : " Windows refused this file although its folder is writable: an antivirus or Windows Security's Controlled folder access may be holding or blocking it.";
            throw new IOException($"Applying stopped{where}: {error.Message}{advice} The original files are still backed up: launch again, or use Back to stock DCS.", error);
        }
    }

    /// <summary>
    /// Puts every path back to its original whatever it holds now: the backup is written back, a file that did not
    /// exist is removed, and options.lua gets only its owned settings back (every other edit is kept; a file that can
    /// no longer be read gets the whole original back). Each path is forgotten once restored, so an interrupted
    /// restore resumes. Paths that could not be restored (a damaged backup, a locked file) stay listed.
    /// </summary>
    public RestoreResult RestoreOriginals()
    {
        using var exclusive = Lock();
        var baseline = LoadCore();
        // What the applied profile installed, read before the record is cleared: a file replaced since is the pilot's.
        var installed = InstalledHashes(baseline) ?? Recorded(baseline.LastInstalled);
        var own = ownHashes?.Invoke() ?? new HashSet<string>();
        baseline.LastInstalled = installed;
        baseline.State = "restoring"; baseline.Current = null; Save(baseline);
        var problems = new List<string>();
        var count = 0;
        foreach (var original in baseline.Files.AsEnumerable().Reverse().ToArray())
        {
            // A DCS install or Saved Games folder removed since (another drive, DCS.openbeta): nothing is rebuilt there;
            // the entry goes, its backup stays in the originals folder.
            if (RootGone(original))
            {
                Log($"{original.Root} no longer exists; {original.Path} is not put back (its backup stays as {original.BackupFile ?? "none"}).");
                baseline.Files.Remove(original); Save(baseline); count++;
                continue;
            }
            try
            {
                if (PilotsNewerFile(installed, original, own))
                {
                    // Replaced after DCS Control installed its file (by the pilot or another program, a ReShade update for
                    // example): the newer file is left where it is, neither overwritten with the older original nor removed.
                    Log($"{original.Path} was replaced after DCS Control installed it; the newer file is left as it is.");
                }
                else
                {
                    RestoreEntry(original, out var note);
                    if (note is not null) Log(note);
                }
                baseline.Files.Remove(original); Save(baseline); DeleteBackup(original); count++;
            }
            catch (Exception error) when (error is IOException or UnauthorizedAccessException or InvalidDataException)
            {
                problems.Add(original.Path + ": " + error.Message);
                Log("Could not restore " + original.Path + ": " + error.Message);
            }
        }
        if (problems.Count == 0) baseline.LastInstalled = null;
        baseline.State = problems.Count == 0 ? "clean" : "restoring";
        baseline.LastAction = problems.Count == 0 ? $"Restored {count} original files" : $"Restored {count} original files; {problems.Count} could not be restored";
        baseline.LastActionAt = DateTimeOffset.UtcNow;
        Save(baseline);
        Log(baseline.LastAction + ".");
        return new(problems.Count == 0, problems);
    }

    /// <summary>Overwrites files of the applied profile with new content and records the new bytes (Pimax Play's focus
    /// values read again at launch). Only whole-file entries: no DCS settings, component INI values or deletions.</summary>
    public void UpdateInstalled(IReadOnlyList<(string Path, byte[] Content)> files)
    {
        using var exclusive = Lock();
        var baseline = LoadCore();
        if (baseline is not { State: "applied", Current: { } current }) throw new InvalidOperationException("Only an applied profile can be updated in place.");
        if (files.Select(f => System.IO.Path.GetFullPath(f.Path)).Distinct(StringComparer.OrdinalIgnoreCase).Count() != files.Count)
            throw new InvalidDataException("The update contains duplicate destinations.");
        var targets = files.Select(f =>
        {
            var full = System.IO.Path.GetFullPath(f.Path);
            var entry = current.Entries.SingleOrDefault(e => e.Path.Equals(full, StringComparison.OrdinalIgnoreCase)) ?? throw new InvalidOperationException("The applied profile does not own " + full + ".");
            if (entry.LuaChanges is not null || entry.IniValues is not null || entry.Deleted) throw new InvalidOperationException("This file cannot be updated in place: " + full);
            return (Entry: entry, f.Content);
        }).ToArray();
        foreach (var (entry, content) in targets)
        {
            PathPolicy.RejectReparsePoints(entry.Path, entry.Root);
            AtomicFile.Write(entry.Path, content);
            entry.InstalledSha256 = Hashing.BytesSha256(content);
        }
        Save(baseline);
    }

    /// <summary>
    /// Sets the applied profile's own settings in options.lua back to the installed values when they differ (DCS
    /// rewrote Max FPS, for example); every other key and the file's formatting stay. Returns the "file: key" pairs
    /// written. A file that cannot be parsed is left alone.
    /// </summary>
    public IReadOnlyList<string> RealignLuaValues()
    {
        using var exclusive = Lock();
        var baseline = LoadCore();
        if (baseline is not { State: "applied", Current: { } current }) throw new InvalidOperationException("Only an applied profile can be updated in place.");
        var written = new List<string>();
        foreach (var entry in current.Entries)
        {
            if (entry.LuaChanges is not { Count: > 0 } || !File.Exists(entry.Path)) continue;
            PathPolicy.RejectReparsePoints(entry.Path, entry.Root);
            try
            {
                var utf8 = StrictUtf8.Encoding;
                var bytes = File.ReadAllBytes(entry.Path); var text = utf8.GetString(bytes);
                var drifted = entry.LuaChanges.Where(c => LuaOptions.NormalizeLiteral(new LuaOptions(text).Get(c.Path.Split('.'))) != LuaOptions.NormalizeLiteral(c.InstalledRaw)).ToArray();
                if (drifted.Length == 0) continue;
                foreach (var change in drifted) text = new LuaOptions(text).SetLiteral(change.Path.Split('.'), change.InstalledRaw);
                _ = new LuaOptions(text);
                // An edit while the new text was prepared wins: nothing is written.
                if (Hashing.FileSha256(entry.Path) != Hashing.BytesSha256(bytes)) continue;
                var merged = utf8.GetBytes(text);
                AtomicFile.Write(entry.Path, merged);
                entry.InstalledSha256 = Hashing.BytesSha256(merged);
                written.AddRange(drifted.Select(c => System.IO.Path.GetFileName(entry.Path) + ": " + c.Path));
            }
            catch (Exception error) when (error is InvalidDataException or DecoderFallbackException or ArgumentException) { }
        }
        if (written.Count > 0) Save(baseline);
        return written;
    }

    private OriginalFile Capture(FileMutation file, string full, IReadOnlySet<string> own)
    {
        var root = file.Root is null ? null : System.IO.Path.GetFullPath(file.Root);
        var settings = file.LuaChanges is null ? null : new Dictionary<string, string?>();
        if (!File.Exists(full)) return new() { Path = full, Root = root, Settings = settings };
        var bytes = File.ReadAllBytes(full);
        var hash = Hashing.BytesSha256(bytes);
        // Bytes DCS Control installs (this plan's or any of its components) are ours, whoever left them: absent.
        if (settings is null && (file.OwnedLocation || hash == Hashing.BytesSha256(file.Content) || own.Contains(hash) || file.OwnHashes?.Contains(hash) == true))
            return new() { Path = full, Root = root, Leftover = true };
        var backup = NextBackupName();
        AtomicFile.Write(System.IO.Path.Combine(Directory, backup), bytes);
        return new() { Path = full, Root = root, OriginalSha256 = hash, BackupFile = backup, Foreign = settings is null, Settings = settings };
    }

    /// <summary>options.lua as it is now, with owned settings this plan no longer changes set back to their originals and
    /// the plan's settings set to the installed values.</summary>
    private static byte[] MergeSettings(string path, OriginalFile original, IReadOnlyList<LuaValueChange> changes, List<string> released)
    {
        var utf8 = StrictUtf8.Encoding;
        var text = utf8.GetString(File.ReadAllBytes(path));
        var planned = changes.Select(c => c.Path).ToHashSet(StringComparer.Ordinal);
        foreach (var (key, value) in (original.Settings ?? []).Where(s => !planned.Contains(s.Key)).ToArray())
        {
            text = SetBack(text, key, value);
            released.Add(key);
        }
        foreach (var change in changes) text = new LuaOptions(text).SetLiteral(change.Path.Split('.'), change.InstalledRaw);
        _ = new LuaOptions(text);
        return utf8.GetBytes(text);
    }

    private static string SetBack(string text, string key, string? value)
    {
        var lua = new LuaOptions(text); var keys = key.Split('.');
        var now = lua.Get(keys);
        if (LuaOptions.NormalizeLiteral(now) == LuaOptions.NormalizeLiteral(value)) return text;
        return value is null ? lua.Remove(keys) : lua.SetLiteral(keys, value);
    }

    /// <summary>Puts one path back to its original. <paramref name="note"/> says when options.lua could not be read and the
    /// whole original file was written back.</summary>
    private void RestoreEntry(OriginalFile original, out string? note)
    {
        note = null;
        // Its folder is missing but not removed (RootGone said no: a drive or top folder that is not there now, an unmounted
        // volume): nothing is rebuilt there and the entry stays listed until the folder is back.
        if (original.Root is { } root && !System.IO.Directory.Exists(root))
            throw new IOException("Its folder " + root + " is not available now (an offline or unmounted drive); it stays listed for Back to stock DCS.");
        PathPolicy.RejectReparsePoints(original.Path, original.Root);
        byte[] Backup()
        {
            var bytes = File.ReadAllBytes(PathPolicy.UnderRoot(Directory, original.BackupFile!));
            if (Hashing.BytesSha256(bytes) != original.OriginalSha256) throw new InvalidDataException("The backup of this file is damaged; it was left as it is.");
            return bytes;
        }
        if (original.Settings is not null)
        {
            if (!File.Exists(original.Path)) { if (original.BackupFile is not null) AtomicFile.Write(original.Path, Backup()); return; }
            var utf8 = StrictUtf8.Encoding;
            try
            {
                var current = File.ReadAllBytes(original.Path); var text = utf8.GetString(current);
                // Nothing but the owned settings differs from the original: the original bytes go back exactly (a
                // setting DCS Control inserted leaves no trace of its line).
                if (OnlyOwnedSettingsDiffer(original, text) is { } exact) { AtomicFile.Write(original.Path, exact); return; }
                foreach (var (key, value) in original.Settings) text = SetBack(text, key, value);
                _ = new LuaOptions(text);
                var merged = utf8.GetBytes(text);
                if (!merged.AsSpan().SequenceEqual(current)) AtomicFile.Write(original.Path, merged);
            }
            catch (Exception error) when (error is InvalidDataException or DecoderFallbackException or ArgumentException)
            {
                if (original.BackupFile is null) throw new InvalidDataException("The file cannot be read and has no backup: " + error.Message);
                // The file as it is now (DCS's and the pilot's later changes) is kept next to the backups first.
                var kept = KeepReplaced(original.Path);
                AtomicFile.Write(original.Path, Backup());
                note = $"{original.Path} could not be read ({error.Message}); the whole original file was put back"
                    + (kept is null ? "." : $"; the file as it was is kept as {kept}.");
            }
            return;
        }
        if (original.BackupFile is not null) AtomicFile.Write(original.Path, Backup());
        else if (File.Exists(original.Path)) AtomicFile.Retry(() => { File.SetAttributes(original.Path, FileAttributes.Normal); File.Delete(original.Path); });
        // Runtime files first, so the folder a removed file leaves empty goes too.
        RuntimeFiles.Remove(original.Path, original.RuntimeLogs);
        if (original.BackupFile is null) RuntimeFiles.RemoveEmptyCreatedFolder(original.Path, original.Root);
    }

    /// <summary>The original options.lua bytes when the file now differs from them only in the owned settings (the
    /// original with the owned settings set to their current values is exactly the current text); null otherwise.</summary>
    private byte[]? OnlyOwnedSettingsDiffer(OriginalFile original, string current)
    {
        if (original.BackupFile is null || original.Settings is null) return null;
        try
        {
            var bytes = File.ReadAllBytes(PathPolicy.UnderRoot(Directory, original.BackupFile));
            if (Hashing.BytesSha256(bytes) != original.OriginalSha256) return null;
            var probe = StrictUtf8.Encoding.GetString(bytes);
            var now = new LuaOptions(current);
            foreach (var key in original.Settings.Keys)
            {
                var keys = key.Split('.'); var value = now.Get(keys); var lua = new LuaOptions(probe);
                probe = value is null ? lua.Remove(keys) : lua.Get(keys) == value ? probe : lua.SetLiteral(keys, value);
            }
            return probe == current ? bytes : null;
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException or InvalidDataException or DecoderFallbackException or ArgumentException) { return null; }
    }

    private OriginalsBaseline LoadCore()
    {
        if (File.Exists(BaselinePath))
        {
            var baseline = JsonData.Deserialize<OriginalsBaseline>(File.ReadAllText(BaselinePath));
            if (baseline.SchemaVersion != Schema) throw new InvalidDataException("The originals record has an unknown format: " + BaselinePath);
            return baseline;
        }
        return Convert();
    }

    /// <summary>
    /// Builds the originals from the journals of earlier versions (once). For each path the original is the backup (or
    /// absence) recorded by the oldest journal that touched it and was not restored; each owned options.lua setting
    /// likewise takes the oldest recorded value. Bytes another journal installed, or identical to a DCS Control
    /// component, count as absent. The newest applied journal becomes the applied profile. Backups are copied and
    /// checked before the baseline is written; only then is the old folder renamed (kept, never deleted).
    /// </summary>
    private OriginalsBaseline Convert()
    {
        var folders = (legacyFolders?.Invoke() ?? []).Select(System.IO.Path.GetFullPath).Distinct(StringComparer.OrdinalIgnoreCase).Where(System.IO.Directory.Exists).ToArray();
        var journals = new Dictionary<string, (TransactionJournal Journal, string Folder)>(StringComparer.OrdinalIgnoreCase);
        var notes = new List<string>();
        foreach (var folder in folders)
            foreach (var dir in System.IO.Directory.EnumerateDirectories(folder))
            {
                var path = System.IO.Path.Combine(dir, "journal.json");
                if (!File.Exists(path)) continue;
                try
                {
                    var journal = JsonData.Deserialize<TransactionJournal>(File.ReadAllText(path));
                    if (journal.ProfileId == "application") continue;
                    journals.TryAdd(journal.Id, (journal, dir));
                }
                catch (Exception e) when (e is IOException or UnauthorizedAccessException or System.Text.Json.JsonException or InvalidDataException or NotSupportedException)
                { notes.Add($"Skipped unreadable journal {path}: {e.Message}"); }
            }
        var baseline = new OriginalsBaseline();
        if (journals.Count == 0 && notes.Count == 0) return baseline;
        System.IO.Directory.CreateDirectory(Directory);
        var own = new HashSet<string>(ownHashes?.Invoke() ?? new HashSet<string>(), StringComparer.OrdinalIgnoreCase);
        // Whole files any journal installed are DCS Control's own bytes (an earlier profile's dxgi.dll, for example).
        foreach (var (journal, _) in journals.Values)
            foreach (var entry in journal.Entries.Where(e => e.LuaChanges is null && e.IniValues is null && !e.Deleted)) own.Add(entry.InstalledSha256);
        var active = journals.Values.Where(j => j.Journal.Status != "restored").OrderBy(j => j.Journal.CreatedAt).ToArray();
        notes.Add($"Converting {journals.Count} journals from {string.Join(", ", folders)}: {active.Length} not restored.");
        foreach (var (journal, folder) in active)
        {
            var live = journal.Entries.Where(e => e.Applied && !e.Reverted).ToArray();
            notes.Add($"Journal {journal.Id} ({journal.ProfileId}, {journal.CreatedAt:u}, {journal.Status}): {live.Length} files not restored.");
            foreach (var entry in live)
            {
                var existing = Find(baseline, entry.Path);
                if (existing is not null)
                {
                    if (entry.LuaChanges is not null && existing.Settings is not null)
                        foreach (var change in entry.LuaChanges)
                            if (existing.Settings.TryAdd(change.Path, change.PreviousRaw)) notes.Add($"  {entry.Path}: setting {change.Path} original {change.PreviousRaw ?? "absent"} (from this journal).");
                    if (entry.RuntimeLogs is { Count: > 0 } more) existing.RuntimeLogs = [.. (existing.RuntimeLogs ?? []).Union(more, StringComparer.OrdinalIgnoreCase)];
                    notes.Add($"  {entry.Path}: original already taken from an older journal.");
                    continue;
                }
                var settings = entry.LuaChanges?.ToDictionary(c => c.Path, c => c.PreviousRaw);
                var previous = entry.PreviousSha256;
                if (previous is not null && settings is null && own.Contains(previous))
                {
                    baseline.Files.Add(new() { Path = entry.Path, Root = entry.Root, Leftover = true, RuntimeLogs = entry.RuntimeLogs?.ToList(), Since = journal.CreatedAt });
                    notes.Add($"  {entry.Path}: its backup is a DCS Control file; original counted as absent.");
                    continue;
                }
                string? backup = null;
                if (previous is not null)
                {
                    var source = entry.BackupFile is null ? null : System.IO.Path.Combine(folder, entry.BackupFile);
                    var bytes = source is not null && File.Exists(source) ? File.ReadAllBytes(source) : null;
                    if (bytes is null || Hashing.BytesSha256(bytes) != previous)
                    {
                        notes.Add($"  {entry.Path}: the backup is missing or damaged; this path is left out (its file stays as it is).");
                        continue;
                    }
                    backup = NextBackupName();
                    AtomicFile.Write(System.IO.Path.Combine(Directory, backup), bytes);
                    if (Hashing.FileSha256(System.IO.Path.Combine(Directory, backup)) != previous) throw new IOException("A converted backup could not be verified: " + entry.Path);
                }
                baseline.Files.Add(new() { Path = entry.Path, Root = entry.Root, OriginalSha256 = previous, BackupFile = backup, Foreign = previous is not null && settings is null, Settings = settings, RuntimeLogs = entry.RuntimeLogs?.ToList(), Since = journal.CreatedAt });
                notes.Add($"  {entry.Path}: " + (settings is not null ? "settings " + string.Join(", ", settings.Select(s => s.Key + "=" + (s.Value ?? "absent"))) + (backup is null ? "" : ", whole file backed up as " + backup)
                    : backup is not null ? "original backed up as " + backup : "created by DCS Control (removed on restore)") + ".");
            }
        }
        var applied = active.Where(j => j.Journal.Status == "applied").ToArray();
        if (applied.Length > 1) notes.Add($"{applied.Length} journals were marked applied at once; the newest ({applied[^1].Journal.Id}) is the applied profile.");
        if (active.Any(j => j.Journal.Status != "applied")) notes.Add("Journals that were not cleanly restored are included: their originals come first.");
        if (applied.Length > 0)
        {
            var newest = applied[^1].Journal;
            baseline.Current = newest with { Entries = [.. newest.Entries.Where(e => e.Applied && !e.Reverted)] };
            baseline.State = "applied";
        }
        baseline.LastAction = $"Converted earlier backups: {baseline.Files.Count} original files"; baseline.LastActionAt = DateTimeOffset.UtcNow;
        Save(baseline);
        // The old folder is kept, renamed, so nothing is lost if the conversion missed something.
        var primary = folders.FirstOrDefault();
        if (primary is not null && System.IO.Path.GetDirectoryName(primary)!.Equals(System.IO.Path.GetDirectoryName(Directory), StringComparison.OrdinalIgnoreCase))
        {
            var target = primary + ".converted";
            if (System.IO.Directory.Exists(target)) target += "-" + DateTimeOffset.UtcNow.ToString("yyyyMMddHHmmss", System.Globalization.CultureInfo.InvariantCulture);
            try { System.IO.Directory.Move(primary, target); notes.Add("Kept the earlier journals as " + target + "."); }
            catch (Exception e) when (e is IOException or UnauthorizedAccessException) { notes.Add("The earlier journals stay in " + primary + " (" + e.Message + ")."); }
        }
        foreach (var other in folders.Skip(1)) notes.Add("Also read (left as it is): " + other);
        notes.Add($"Conversion done: {baseline.Files.Count} original files, applied profile {baseline.Current?.ProfileId ?? "none"}.");
        foreach (var note in notes) Log(note);
        return baseline;
    }

    /// <summary>The recorded original of a path, also when it was recorded under another spelling of the same file (a
    /// junction, subst drive or 8.3 name): two entries for one file would record DCS Control's own file as the original.</summary>
    private static OriginalFile? Find(OriginalsBaseline baseline, string path)
    {
        if (baseline.Files.FirstOrDefault(f => f.Path.Equals(path, StringComparison.OrdinalIgnoreCase)) is { } same) return same;
        var canonical = SystemPaths.Canonical(path);
        return baseline.Files.FirstOrDefault(f => SystemPaths.Canonical(f.Path).Equals(canonical, StringComparison.OrdinalIgnoreCase));
    }

    /// <summary>Copies the file at <paramref name="path"/> into the originals folder as &lt;n&gt;.replaced before it is
    /// overwritten or removed; returns the copy's path, or null when there was no file.</summary>
    private string? KeepReplaced(string path)
    {
        if (!File.Exists(path)) return null;
        System.IO.Directory.CreateDirectory(Directory);
        // Copies are kept 90 days: long enough to recover a file by hand, not forever.
        foreach (var stale in new DirectoryInfo(Directory).EnumerateFiles("*.replaced").Where(f => f.LastWriteTimeUtc < DateTime.UtcNow.AddDays(-90)))
            try { stale.Delete(); } catch (Exception e) when (e is IOException or UnauthorizedAccessException) { }
        var copy = System.IO.Path.Combine(Directory, $"{DateTime.UtcNow:yyyyMMdd-HHmmss}-{Guid.NewGuid():N}"[..24] + ".replaced");
        AtomicFile.Write(copy, File.ReadAllBytes(path));
        return copy;
    }

    /// <summary>The whole files the applied profile installed, by path, with the hashes it installed there (null when no
    /// complete apply is recorded, so a later change cannot be told from DCS Control's own write). Files that only get
    /// settings or values merged in (options.lua, component INIs) are not listed: they change legitimately.</summary>
    private static Dictionary<string, HashSet<string>>? InstalledHashes(OriginalsBaseline baseline)
    {
        if (baseline.Current is not { } journal) return null;
        var installed = new Dictionary<string, HashSet<string>>(StringComparer.OrdinalIgnoreCase);
        foreach (var entry in journal.Entries.Where(e => e.LuaChanges is null && e.IniValues is null && !e.Deleted))
        {
            var path = SystemPaths.Canonical(entry.Path);
            if (!installed.TryGetValue(path, out var hashes)) installed[path] = hashes = new(StringComparer.OrdinalIgnoreCase);
            hashes.Add(entry.InstalledSha256);
        }
        return installed;
    }

    /// <summary>True when the file now at a path DCS Control installed whole is something else: neither the original,
    /// nor what DCS Control installed there, nor any of its components. Used only for another program's file that DCS
    /// Control replaced (dxgi.dll of ReShade, for example): when that program puts a newer version back, the newer one
    /// is what the pilot has, so it is not overwritten with the older original. DCS Control's own files and the files
    /// it created keep the rule that Back to stock DCS puts back exactly what was there before.</summary>
    private static bool ReplacedAfterInstall(Dictionary<string, HashSet<string>> installed, string path, string now, string? original, IReadOnlySet<string> own)
        => installed.TryGetValue(SystemPaths.Canonical(path), out var hashes) && !hashes.Contains(now)
           && !string.Equals(now, original, StringComparison.OrdinalIgnoreCase) && !own.Contains(now) && !IsCheekyLoader(path);

    /// <summary>True for a Cheeky DLSS loader (dxgi.dll) of any DCS Control version or copy (a dev build, another state
    /// folder): it names its CheekyFoveatedDLSS folder. Never the pilot's own file, so never taken as a newer original.</summary>
    private static bool IsCheekyLoader(string path)
    {
        try
        {
            if (new FileInfo(path).Length > 64L * 1024 * 1024) return false;
            return File.ReadAllBytes(path).AsSpan().IndexOf("CheekyFoveatedDLSS"u8) >= 0;
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException) { return false; }
    }

    /// <summary>The hash of the file now at <paramref name="full"/> when it was replaced after DCS Control installed its
    /// own whole file there (see <see cref="ReplacedAfterInstall"/>); null otherwise, and whenever that cannot be told (no
    /// complete apply recorded, settings files, the app's own folder).</summary>
    private static string? ReplacedSinceInstall(OriginalsBaseline baseline, OriginalFile original, string full, IReadOnlySet<string> own, FileMutation file)
    {
        // Another program's original, or a path that was empty and now holds another program's library (ReShade
        // installed there while the profile was applied). A file DCS Control created and the pilot edited is simply
        // written again.
        if (!(original.Foreign || original.BackupFile is null && IsLibrary(full)) || original.Settings is not null || file.LuaChanges is not null || file.IniValues is not null || file.OwnedLocation || !File.Exists(full)) return null;
        // After an interrupted apply the record is cleared; what that apply's predecessor installed is kept for this.
        if ((InstalledHashes(baseline) ?? Recorded(baseline.LastInstalled)) is not { } installed) return null;
        var now = Hashing.FileSha256(full);
        if (now == Hashing.BytesSha256(file.Content) || file.OwnHashes?.Contains(now) == true) return null;
        return ReplacedAfterInstall(installed, full, now, original.OriginalSha256, own) ? now : null;
    }

    /// <summary>A Windows program library (starts with "MZ"): what another program (ReShade, an overlay) puts in DCS's bin.</summary>
    private static bool IsLibrary(string path)
    {
        try
        {
            if (!File.Exists(path)) return false;
            using var stream = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete);
            return stream.ReadByte() == 'M' && stream.ReadByte() == 'Z';
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException) { return false; }
    }

    /// <summary>The file now at the entry's path was put there by the pilot or another program after DCS Control installed
    /// its own: another program's newer version of its file (a ReShade update), or another program's library on a path
    /// that was empty (ReShade installed while the profile was applied). It stays where it is.</summary>
    /// <summary>The recorded install hashes with the case-insensitive comparers the live ones use (JSON loses them).</summary>
    private static Dictionary<string, HashSet<string>>? Recorded(Dictionary<string, HashSet<string>>? recorded) =>
        recorded?.ToDictionary(p => p.Key, p => new HashSet<string>(p.Value, StringComparer.OrdinalIgnoreCase), StringComparer.OrdinalIgnoreCase);

    private static bool PilotsNewerFile(Dictionary<string, HashSet<string>>? installed, OriginalFile original, IReadOnlySet<string> own)
    {
        if (installed is null || original.Settings is not null || !File.Exists(original.Path)) return false;
        if (!original.Foreign && !(original.BackupFile is null && IsLibrary(original.Path))) return false;
        return ReplacedAfterInstall(installed, original.Path, Hashing.FileSha256(original.Path), original.OriginalSha256, own);
    }

    /// <summary>The folder the entry was authorized under no longer exists (that DCS install or variant was removed). A
    /// drive that is only offline (an external disk, a locked volume, a network share) is not "removed": its entries stay.
    /// Neither is a root whose whole path is missing below the drive (the letter may now belong to another disk) or one
    /// that is only unreadable: removed means a parent folder still exists and lists no such folder.</summary>
    private static bool RootGone(OriginalFile original)
    {
        try
        {
            if (original.Root is not { } root || System.IO.Directory.Exists(root)) return false;
            var full = System.IO.Path.TrimEndingDirectorySeparator(System.IO.Path.GetFullPath(root));
            var volume = System.IO.Path.GetPathRoot(full);
            if (string.IsNullOrEmpty(volume) || !System.IO.Directory.Exists(volume)) return false;
            var missing = full;
            var parent = System.IO.Path.GetDirectoryName(missing);
            while (parent is not null && !System.IO.Directory.Exists(parent))
            {
                missing = parent;
                parent = System.IO.Path.GetDirectoryName(missing);
            }
            if (parent is null || string.Equals(System.IO.Path.TrimEndingDirectorySeparator(parent), System.IO.Path.TrimEndingDirectorySeparator(volume), StringComparison.OrdinalIgnoreCase)) return false;
            return !System.IO.Directory.EnumerateFileSystemEntries(parent, System.IO.Path.GetFileName(missing)).Any();
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException) { return false; }
    }

    private string NextBackupName()
    {
        System.IO.Directory.CreateDirectory(Directory);
        var next = System.IO.Directory.EnumerateFiles(Directory, "*.original").Select(p => int.TryParse(System.IO.Path.GetFileNameWithoutExtension(p), out var n) ? n : -1).DefaultIfEmpty(-1).Max() + 1;
        return next.ToString("D4", System.Globalization.CultureInfo.InvariantCulture) + ".original";
    }

    private void DeleteBackup(OriginalFile original)
    {
        if (original.BackupFile is null) return;
        try { File.Delete(PathPolicy.UnderRoot(Directory, original.BackupFile)); }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException) { }
    }

    private FileStream Lock()
    {
        System.IO.Directory.CreateDirectory(Directory);
        // The CPU Boost helper or the CLI may hold it for a moment: wait up to about 5 s before giving up.
        for (var attempt = 1; ; attempt++)
        {
            try { return new FileStream(System.IO.Path.Combine(Directory, "originals.lock"), FileMode.OpenOrCreate, FileAccess.ReadWrite, FileShare.None); }
            catch (IOException e) when (attempt < 25 && (e.HResult & 0xFFFF) is 32 or 33) { Thread.Sleep(200); }
        }
    }

    private void Save(OriginalsBaseline baseline)
    {
        System.IO.Directory.CreateDirectory(Directory);
        AtomicFile.WriteText(BaselinePath, JsonData.Serialize(baseline));
    }

    private void Log(string line)
    {
        try
        {
            System.IO.Directory.CreateDirectory(Directory);
            var info = new FileInfo(LogPath);
            if (info.Exists && info.Length > 1024 * 1024) File.Move(LogPath, LogPath + ".old", overwrite: true);
            File.AppendAllText(LogPath, $"{DateTimeOffset.Now:yyyy-MM-dd HH:mm:ss} {line}{Environment.NewLine}");
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException) { }
    }
}
