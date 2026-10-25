namespace DcsVr.Core;

public sealed record LuaValueChange(string Path, string? PreviousRaw, string InstalledRaw);
/// <param name="IniValues">Owned keys of a configuration file that its component rewrites at runtime (adding defaults,
/// reformatting numbers). Launch checks compare these values instead of the file hash.</param>
/// <param name="OwnHashes">Other bytes of DCS Control's own for this path (the other Cheeky loader variant, for
/// example): an existing file with them is a leftover of ours, not another program's file.</param>
/// <param name="OwnedLocation">The path lies in DCS Control's own folder: whatever is there is ours.</param>
public sealed record FileMutation(string Path, string? ExpectedSha256, byte[] Content, string Purpose, IReadOnlyList<LuaValueChange>? LuaChanges = null, bool Delete = false, IReadOnlyDictionary<string, string>? IniValues = null, IReadOnlyList<string>? RuntimeLogs = null, string? Root = null, IReadOnlyCollection<string>? OwnHashes = null, bool OwnedLocation = false);
public sealed record ApplyPlan(string ProfileId, string Description, IReadOnlyList<FileMutation> Files, IReadOnlyDictionary<string, string> LaunchEnvironment, string Executable);
public sealed record JournalEntry
{
    public required string Path { get; init; }
    /// <summary>Hash of the bytes this transaction installed (for the applied profile: updated by in-place updates).</summary>
    public required string InstalledSha256 { get; set; }
    /// <summary>Journals of earlier versions: set while an in-place update replaced the file (the hash of the new bytes).
    /// A file holding them is the journal's own, never a user edit.</summary>
    [System.Text.Json.Serialization.JsonIgnore(Condition = System.Text.Json.Serialization.JsonIgnoreCondition.WhenWritingNull)]
    public string? PendingSha256 { get; set; }
    public string? PreviousSha256 { get; init; }
    public string? BackupFile { get; init; }
    public IReadOnlyList<LuaValueChange>? LuaChanges { get; init; }
    /// <summary>The owned component INI values (Cheeky, pupil shift); a live update records the new ones.</summary>
    public IReadOnlyDictionary<string, string>? IniValues { get; set; }
    /// <summary>Log files the installed component writes at runtime. Restore removes them with the component.</summary>
    public IReadOnlyList<string>? RuntimeLogs { get; init; }
    /// <summary>Folder the user authorized (bin, Config, managed state). Links below it are rejected at every write.</summary>
    public string? Root { get; init; }
    public bool Applied { get; set; }
    public bool Reverted { get; set; }
    public bool Deleted { get; init; }
}
public sealed record TransactionJournal
{
    public int SchemaVersion { get; init; } = 1;
    public required string Id { get; init; }
    public required string ProfileId { get; init; }
    public DateTimeOffset CreatedAt { get; init; } = DateTimeOffset.UtcNow;
    public string Status { get; set; } = "prepared";
    public List<JournalEntry> Entries { get; init; } = [];
}
public sealed record RestoreResult(bool Complete, IReadOnlyList<string> Conflicts);

/// <summary>Journals every write, detects concurrent edits, and preserves user changes on restore. Used by the application
/// installer; DCS files go through <see cref="OriginalsStore"/>, which also reads journals of earlier versions.</summary>
public sealed class TransactionStore(string stateDirectory)
{
    public string StateDirectory { get; } = Path.GetFullPath(stateDirectory);

    public TransactionJournal Apply(ApplyPlan plan, Action<int>? beforeWrite = null)
    {
        Directory.CreateDirectory(StateDirectory);
        using var exclusive = new FileStream(Path.Combine(StateDirectory, "transaction.lock"), FileMode.OpenOrCreate, FileAccess.ReadWrite, FileShare.None);
        if (plan.Files.Select(f => Path.GetFullPath(f.Path)).Distinct(StringComparer.OrdinalIgnoreCase).Count() != plan.Files.Count)
            throw new InvalidDataException("The plan contains duplicate destinations.");
        foreach (var file in plan.Files) VerifyExpected(file);
        if (plan.Files.Any(f => f.Delete && (f.ExpectedSha256 is null || f.Content.Length != 0 || f.LuaChanges is not null)))
            throw new InvalidDataException("Deletion requires a verified existing file and an empty payload.");
        var journal = new TransactionJournal { Id = Guid.NewGuid().ToString("N"), ProfileId = plan.ProfileId };
        var root = PathPolicy.UnderRoot(StateDirectory, journal.Id);
        Directory.CreateDirectory(root);
        for (var index = 0; index < plan.Files.Count; index++)
        {
            var file = plan.Files[index];
            string? backup = null;
            if (file.ExpectedSha256 is not null)
            {
                backup = $"{index:D4}.backup";
                var original = File.ReadAllBytes(file.Path);
                if (Hashing.BytesSha256(original) != file.ExpectedSha256) throw new IOException("The file changed while preparing its backup.");
                AtomicFile.Write(PathPolicy.UnderRoot(root, backup), original);
            }
            journal.Entries.Add(new() { Path = Path.GetFullPath(file.Path), PreviousSha256 = file.ExpectedSha256, InstalledSha256 = Hashing.BytesSha256(file.Content), BackupFile = backup, LuaChanges = file.LuaChanges, IniValues = file.IniValues, RuntimeLogs = file.RuntimeLogs, Root = file.Root is null ? null : Path.GetFullPath(file.Root), Deleted = file.Delete });
        }
        Save(root, journal);
        try
        {
            journal.Status = "applying"; Save(root, journal);
            for (var index = 0; index < plan.Files.Count; index++)
            {
                beforeWrite?.Invoke(index);
                var file = plan.Files[index];
                VerifyExpected(file);
                // Intent is persisted before replacement so an interrupted write is recoverable.
                journal.Entries[index].Applied = true; Save(root, journal);
                if (file.Delete) File.Delete(file.Path); else AtomicFile.Write(file.Path, file.Content);
            }
            journal.Status = "applied"; Save(root, journal);
            return journal;
        }
        catch (Exception applyError)
        {
            try
            {
                if (!RestoreCore(root, journal).Complete)
                    throw new IOException($"Apply failed ({applyError.Message}) and rollback left files changed by another program. Restore transaction {journal.Id} from Recovery.", applyError);
            }
            catch (IOException rolled) when (rolled.InnerException == applyError) { throw; }
            catch (Exception rollbackError)
            {
                // Keep the original failure visible; the journal remains for a manual restore.
                throw new IOException($"Apply failed ({applyError.Message}) and automatic rollback also failed ({rollbackError.Message}). Restore transaction {journal.Id} from Recovery.",
                    new AggregateException(applyError, rollbackError));
            }
            throw;
        }
    }

    public RestoreResult Restore(string transactionId)
    {
        ValidateId(transactionId);
        Directory.CreateDirectory(StateDirectory);
        using var exclusive = new FileStream(Path.Combine(StateDirectory, "transaction.lock"), FileMode.OpenOrCreate, FileAccess.ReadWrite, FileShare.None);
        var root = PathPolicy.UnderRoot(StateDirectory, transactionId);
        var journal = JsonData.Deserialize<TransactionJournal>(File.ReadAllText(PathPolicy.UnderRoot(root, "journal.json")));
        if (journal.Id != transactionId || journal.SchemaVersion != 1) throw new InvalidDataException("Invalid restore ledger.");
        return RestoreCore(root, journal);
    }

    public IReadOnlyList<TransactionJournal> List()
    {
        if (!Directory.Exists(StateDirectory)) return [];
        var result = new List<TransactionJournal>();
        foreach (var dir in Directory.EnumerateDirectories(StateDirectory))
        {
            var path = Path.Combine(dir, "journal.json");
            if (TryRead(path) is { } journal) result.Add(journal);
        }
        return result.OrderByDescending(j => j.CreatedAt).ToArray();
    }

    private static TransactionJournal? TryRead(string path)
    {
        try { return JsonData.Deserialize<TransactionJournal>(File.ReadAllText(path)); }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException or System.Text.Json.JsonException or InvalidDataException or NotSupportedException) { return null; }
    }

    private enum RestoreStep { Original, Replace, MergeLua, Conflict }

    /// <summary>What Restore does with one applied entry; reads only.</summary>
    private static RestoreStep Classify(string root, JournalEntry entry)
    {
        PathPolicy.RejectReparsePoints(entry.Path, entry.Root);
        var current = File.Exists(entry.Path) ? Hashing.FileSha256(entry.Path) : null;
        if (current == entry.PreviousSha256) return RestoreStep.Original;
        // The installed file is gone (an antivirus quarantined it, or it was deleted) and its original is backed up: the
        // original goes back; nothing of anyone else's is at that path to protect.
        if (current is null && !entry.Deleted && entry.BackupFile is not null) return RestoreStep.Replace;
        // A component that normalized its own configuration still holds exactly the owned values: restore it as installed.
        var normalized = current is not null && current != entry.InstalledSha256 && entry.IniValues is { Count: > 0 } && IniFile.Matches(File.ReadAllText(entry.Path), entry.IniValues);
        // Bytes of an interrupted in-place update are the transaction's own, not a user edit.
        var pending = current is not null && current == entry.PendingSha256;
        if (current != (entry.Deleted ? null : entry.InstalledSha256) && !normalized && !pending)
            return current is not null && entry.LuaChanges is { Count: > 0 } && PrepareLuaRestore(root, entry) is not null ? RestoreStep.MergeLua : RestoreStep.Conflict;
        return RestoreStep.Replace;
    }

    private static RestoreResult RestoreCore(string root, TransactionJournal journal)
    {
        var conflicts = new List<string>();
        foreach (var entry in journal.Entries.AsEnumerable().Reverse())
        {
            if (!entry.Applied || entry.Reverted) continue;
            switch (Classify(root, entry))
            {
                case RestoreStep.Original:
                    // A created file the user already deleted still leaves its runtime logs behind.
                    if (!File.Exists(entry.Path)) RuntimeFiles.Remove(entry.Path, entry.RuntimeLogs);
                    entry.Reverted = true; Save(root, journal); continue;
                case RestoreStep.MergeLua:
                    if (TryRestoreLua(root, entry)) { entry.Reverted = true; Save(root, journal); } else conflicts.Add(entry.Path);
                    continue;
                case RestoreStep.Conflict: conflicts.Add(entry.Path); continue;
            }
            if (entry.BackupFile is not null)
            {
                var backup = PathPolicy.UnderRoot(root, entry.BackupFile);
                var bytes = File.ReadAllBytes(backup);
                if (Hashing.BytesSha256(bytes) != entry.PreviousSha256) throw new InvalidDataException("Backup integrity failed: restore stopped.");
                AtomicFile.Write(entry.Path, bytes);
            }
            else File.Delete(entry.Path);
            // Runtime files first, so the folder a removed file leaves empty goes too.
            RuntimeFiles.Remove(entry.Path, entry.RuntimeLogs);
            if (entry.BackupFile is null) RuntimeFiles.RemoveEmptyCreatedFolder(entry.Path, entry.Root);
            entry.Reverted = true; Save(root, journal);
        }
        journal.Status = conflicts.Count == 0 ? "restored" : "restore-conflicts"; Save(root, journal);
        return new(conflicts.Count == 0, conflicts);
    }

    private static bool TryRestoreLua(string root, JournalEntry entry)
    {
        if (PrepareLuaRestore(root, entry) is not { } prepared) return false;
        // Detect an edit during merge preparation before writing.
        if (Hashing.FileSha256(entry.Path) != Hashing.BytesSha256(prepared.Current)) return false;
        AtomicFile.Write(entry.Path, prepared.Merged); return true;
    }

    /// <summary>options.lua with the owned values set back to the originals, when every owned value is still the installed
    /// or the original one; null otherwise. Reads only.</summary>
    private static (byte[] Current, byte[] Merged)? PrepareLuaRestore(string root, JournalEntry entry)
    {
        if (entry.BackupFile is null) return null;
        var backup = File.ReadAllBytes(PathPolicy.UnderRoot(root, entry.BackupFile));
        if (Hashing.BytesSha256(backup) != entry.PreviousSha256) throw new InvalidDataException("Backup integrity failed: restore stopped.");
        try
        {
            var utf8 = StrictUtf8.Encoding;
            var original = new LuaOptions(utf8.GetString(backup));
            var currentBytes = File.ReadAllBytes(entry.Path); var merged = utf8.GetString(currentBytes); var current = new LuaOptions(merged);
            foreach (var change in entry.LuaChanges!)
            {
                var keys = change.Path.Split('.'); var actual = current.Get(keys);
                if (original.Get(keys) != change.PreviousRaw) throw new InvalidDataException("Lua restore metadata does not match its backup.");
                var normalized = LuaOptions.NormalizeLiteral(actual);
                if (normalized != LuaOptions.NormalizeLiteral(change.InstalledRaw) && normalized != LuaOptions.NormalizeLiteral(change.PreviousRaw)) return null;
            }
            foreach (var change in entry.LuaChanges!)
            {
                var lua = new LuaOptions(merged); var keys = change.Path.Split('.');
                merged = change.PreviousRaw is null ? lua.Remove(keys) : lua.SetLiteral(keys, change.PreviousRaw);
            }
            return (currentBytes, utf8.GetBytes(merged));
        }
        catch (Exception error) when (error is InvalidDataException or System.Text.DecoderFallbackException) { return null; }
    }

    private static void VerifyExpected(FileMutation file)
    {
        PathPolicy.RejectReparsePoints(file.Path, file.Root);
        var actual = File.Exists(file.Path) ? Hashing.FileSha256(file.Path) : null;
        if (actual != file.ExpectedSha256) throw new IOException($"File changed since preview: {file.Path}");
    }

    private static void ValidateId(string id)
    {
        if (id.Length != 32 || id.Any(c => !char.IsAsciiHexDigit(c))) throw new InvalidDataException("Invalid restore ID.");
    }
    private static void Save(string root, TransactionJournal journal) => AtomicFile.WriteText(PathPolicy.UnderRoot(root, "journal.json"), JsonData.Serialize(journal));
}

/// <summary>Files a component writes while DCS runs (logs, reports, its own working copies), removed with the file that
/// declared them. A declared name may hold a * in its file name; it then stands for the matching files of that folder.</summary>
internal static class RuntimeFiles
{
    private static readonly string[] Named = [".log", ".flag"], Patterns = [".log", ".txt", ".csv", ".dll", ".qvc", ".tmp", ".jsonl", ".f32"];

    /// <summary>Declared inside the owner's folder tree. A single named file only as a log or a trigger flag; reports
    /// and the component's copies of its own module (e.g. payload\active_*.dll) only as a pattern, so a named library
    /// or document is never deleted.</summary>
    public static bool Allowed(string full, string owner)
    {
        full = Path.GetFullPath(full);
        if (!full.StartsWith(owner.TrimEnd(Path.DirectorySeparatorChar) + Path.DirectorySeparatorChar, StringComparison.OrdinalIgnoreCase)) return false;
        var name = Path.GetFileName(full);
        if (name.Length == 0 || name.Contains('?') || Path.GetDirectoryName(full)!.Contains('*')) return false;
        var extension = Path.GetExtension(name);
        if (extension.Contains('*')) return false;
        return (name.Contains('*') ? Patterns : Named).Contains(extension, StringComparer.OrdinalIgnoreCase);
    }

    /// <summary>The existing files <paramref name="full"/> names (several for a pattern).</summary>
    public static IEnumerable<string> Matching(string full)
    {
        var folder = Path.GetDirectoryName(full)!; var name = Path.GetFileName(full);
        if (!name.Contains('*')) return File.Exists(full) ? [full] : [];
        if (!Directory.Exists(folder)) return [];
        // EnumerateFiles also matches 8.3 names and longer extensions ("*.dll" matches "x.dllx"): keep exact matches only.
        var regex = new System.Text.RegularExpressions.Regex("^" + System.Text.RegularExpressions.Regex.Escape(name).Replace(@"\*", ".*") + "$", System.Text.RegularExpressions.RegexOptions.IgnoreCase);
        return Directory.EnumerateFiles(folder, name).Where(f => regex.IsMatch(Path.GetFileName(f))).ToArray();
    }

    /// <summary>Removes the runtime files <paramref name="path"/> declared, with the folders below its own that they
    /// leave empty.</summary>
    public static void Remove(string path, IEnumerable<string>? declared)
    {
        if (declared is null) return;
        var owner = Path.GetDirectoryName(path)!;
        foreach (var log in declared)
        {
            // Only declared runtime files inside the component's own folder tree; never anything that was not declared.
            var full = Path.GetFullPath(log);
            if (!Allowed(full, owner)) continue;
            // Best effort: a log held open by a viewer must never leave the restore half done.
            try
            {
                PathPolicy.RejectReparsePoints(full, owner);
                foreach (var file in Matching(full)) AtomicFile.Retry(() => File.Delete(file));
                var folder = Path.GetDirectoryName(full)!;
                if (!folder.Equals(owner, StringComparison.OrdinalIgnoreCase) && Directory.Exists(folder) && !Directory.EnumerateFileSystemEntries(folder).Any()) Directory.Delete(folder);
            }
            catch (Exception e) when (e is IOException or UnauthorizedAccessException or InvalidDataException) { }
        }
    }

    /// <summary>A file DCS Control created was removed: the empty folders left behind below the authorized root (for
    /// example bin\CheekyFoveatedDLSS, or managed\profiles\&lt;id&gt; once its last subfolder went) go too. The root
    /// itself is never removed.</summary>
    public static void RemoveEmptyCreatedFolder(string path, string? root)
    {
        try
        {
            if (root is not { } authorized) return;
            var stop = Path.GetFullPath(authorized).TrimEnd(Path.DirectorySeparatorChar) + Path.DirectorySeparatorChar;
            for (var folder = Path.GetDirectoryName(path); folder is not null && folder.StartsWith(stop, StringComparison.OrdinalIgnoreCase); folder = Path.GetDirectoryName(folder))
            {
                PathPolicy.RejectReparsePoints(folder, authorized);
                if (!Directory.Exists(folder) || Directory.EnumerateFileSystemEntries(folder).Any()) return;
                Directory.Delete(folder);
            }
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException or InvalidDataException) { }
    }
}
