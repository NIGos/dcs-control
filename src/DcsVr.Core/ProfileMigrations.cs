using System.Text.Json;

namespace DcsVr.Core;

/// <summary>
/// One-time changes to the profiles kept on this PC (the draft and the presets) when a new version changes a default
/// that earlier versions stored explicitly. Each runs once per state folder: state root\migrations.json records it.
/// Nothing in the profile format changes, so the files stay readable by the version that wrote them. A value the pilot
/// sets afterwards is kept: the migration does not run again.
/// The record belongs to the profile files: Go back sets it aside with them and puts back the earlier version's set
/// (<see cref="Rollback"/>), which has none, so after going back and updating again the files the earlier version
/// left are migrated too; presets that come back from the set-aside copy are skipped (this version saved them).
/// It is written first, as pending, and marked done when every file is handled: a start that cannot write it changes
/// nothing, and a file that could not be written is tried again at the next start, alone. A record that cannot be read
/// counts as done (an earlier choice is never undone on a guess); a record that is gone means a new state folder.
/// </summary>
public static class ProfileMigrations
{
    /// <summary>"Multi-threading without driver support" ([Model] RecordersEmulatedLists) became automatic in 0.7.1: on for
    /// AMD-only graphics (tested on a Radeon, GitHub #27), off elsewhere as before. Profiles saved by 0.7.0 and earlier
    /// store "engineRecordersEmulatedLists": false (every value is written, and false was the default), which would keep
    /// it off on a Radeon: the key is taken out, so the profile is automatic (0.7.0 reads a missing key as false).</summary>
    public const string EmulatedListsOn = "recorders-emulated-lists-on";
    public const string FileName = "migrations.json";
    private const string Key = "engineRecordersEmulatedLists";

    public static string MarkerPath(string stateRoot) => Path.Combine(stateRoot, FileName);

    /// <summary>Runs the migration if it is not done in <paramref name="stateRoot"/>; returns it when it completed now.
    /// <paramref name="skipPresets"/>: preset names Go back returned at this start (saved by this version). Never throws
    /// for one file: a file that cannot be read as JSON is left as it is (the app reports it when it loads it).</summary>
    public static IReadOnlyList<string> Run(string stateRoot, Action<string>? log = null, IReadOnlyCollection<string>? skipPresets = null)
    {
        var record = Read(stateRoot, log);
        if (record is null || record.Done.Contains(EmulatedListsOn)) return [];
        // Relative paths of the files still to do: those recorded as pending, else every profile file now.
        List<string> todo = record.Pending.TryGetValue(EmulatedListsOn, out var pending) ? pending
            : [.. ProfileFiles(stateRoot).Where(f => skipPresets is null || !IsPreset(f, skipPresets))];
        if (!Write(stateRoot, record with { Pending = new(record.Pending) { [EmulatedListsOn] = todo } }, log)) return [];
        var left = new List<string>(); var changed = 0;
        foreach (var file in todo)
        {
            try { if (MakeEmulatedListsAutomatic(Path.Combine(stateRoot, file), file == "draft.json")) changed++; }
            catch (Exception e) when (e is IOException or UnauthorizedAccessException)
            {
                left.Add(file);
                log?.Invoke($"Profile update (multi-threading without driver support automatic): {file} could not be written now ({e.Message}); it is tried again at the next start.");
            }
        }
        if (changed > 0) log?.Invoke($"Multi-threading without driver support is now automatic (on for AMD-only graphics) in {changed} saved profile file(s).");
        var rest = new Dictionary<string, List<string>>(record.Pending);
        if (left.Count > 0) { rest[EmulatedListsOn] = left; Write(stateRoot, record with { Pending = rest }, log); return []; }
        rest.Remove(EmulatedListsOn);
        return Write(stateRoot, new Record([.. record.Done, EmulatedListsOn], rest), log) ? [EmulatedListsOn] : [];
    }

    private static bool IsPreset(string file, IReadOnlyCollection<string> names) =>
        file.StartsWith("presets" + Path.DirectorySeparatorChar, StringComparison.OrdinalIgnoreCase)
        && names.Contains(Path.GetFileNameWithoutExtension(file), StringComparer.OrdinalIgnoreCase);

    /// <summary>The draft and every preset file, relative to the state root.</summary>
    private static IEnumerable<string> ProfileFiles(string stateRoot)
    {
        if (File.Exists(Path.Combine(stateRoot, "draft.json"))) yield return "draft.json";
        var presets = Path.Combine(stateRoot, "presets");
        if (Directory.Exists(presets))
            foreach (var file in Directory.EnumerateFiles(presets, "*.json"))
                yield return Path.Combine("presets", Path.GetFileName(file));
    }

    /// <summary>Takes "engineRecordersEmulatedLists": false out of the profile (the draft's "profile" object, or a preset's
    /// root), every occurrence when a hand-edited file repeats it, so the profile is automatic; changes nothing else. A
    /// profile without the key already is, true stays; a file that is gone or not JSON is left alone. True when it was
    /// rewritten.</summary>
    internal static bool MakeEmulatedListsAutomatic(string path, bool inDraft)
    {
        if (!File.Exists(path)) return false;
        JsonDocument document;
        // JsonDocument keeps repeated keys (JsonNode refuses them), so a hand-edited file is handled like any other.
        try { document = JsonDocument.Parse(File.ReadAllText(path)); }
        catch (JsonException) { return false; }
        using (document)
        {
            var root = document.RootElement;
            if (root.ValueKind != JsonValueKind.Object) return false;
            var profile = inDraft ? root.EnumerateObject().Where(p => p.NameEquals("profile")).Select(p => p.Value).LastOrDefault() : root;
            if (profile.ValueKind != JsonValueKind.Object || !profile.EnumerateObject().Any(IsOff)) return false;
            using var stream = new MemoryStream();
            using (var writer = new Utf8JsonWriter(stream, new JsonWriterOptions { Indented = true }))
            {
                if (inDraft)
                {
                    writer.WriteStartObject();
                    foreach (var property in root.EnumerateObject())
                        if (property.NameEquals("profile") && property.Value.ValueKind == JsonValueKind.Object) { writer.WritePropertyName(property.Name); WriteProfile(writer, property.Value); }
                        else property.WriteTo(writer);
                    writer.WriteEndObject();
                }
                else WriteProfile(writer, root);
            }
            AtomicFile.Write(path, stream.ToArray());
            return true;
        }
    }
    private static bool IsOff(JsonProperty p) => p.Name.Equals(Key, StringComparison.OrdinalIgnoreCase) && p.Value.ValueKind == JsonValueKind.False;
    private static void WriteProfile(Utf8JsonWriter writer, JsonElement profile)
    {
        writer.WriteStartObject();
        foreach (var property in profile.EnumerateObject())
            if (!IsOff(property)) property.WriteTo(writer);
        writer.WriteEndObject();
    }

    private sealed record Record(IReadOnlyList<string> Done, Dictionary<string, List<string>> Pending);

    /// <summary>The record; an empty one when there is none; null when it cannot be read now (locked: nothing runs this
    /// start) or is damaged (counts as done, said in the log).</summary>
    private static Record? Read(string stateRoot, Action<string>? log)
    {
        var path = MarkerPath(stateRoot);
        try
        {
            if (!File.Exists(path)) return new([], []);
            using var document = JsonDocument.Parse(File.ReadAllText(path));
            var root = document.RootElement;
            if (root.ValueKind != JsonValueKind.Object) throw new JsonException("not an object");
            var done = root.TryGetProperty("done", out var d) && d.ValueKind == JsonValueKind.Array
                ? d.EnumerateArray().Where(e => e.ValueKind == JsonValueKind.String).Select(e => e.GetString()!).ToList() : [];
            var pending = new Dictionary<string, List<string>>(StringComparer.Ordinal);
            if (root.TryGetProperty("pending", out var p) && p.ValueKind == JsonValueKind.Object)
                foreach (var item in p.EnumerateObject().Where(i => i.Value.ValueKind == JsonValueKind.Array))
                    pending[item.Name] = [.. item.Value.EnumerateArray().Where(e => e.ValueKind == JsonValueKind.String).Select(e => e.GetString()!)];
            return new(done, pending);
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException)
        {
            log?.Invoke($"The profile update record ({path}) could not be read now ({e.Message}); the update waits for the next start.");
            return null;
        }
        catch (JsonException e)
        {
            log?.Invoke($"The profile update record ({path}) is damaged ({e.Message}); saved profiles are left as they are.");
            return null;
        }
    }

    private static bool Write(string stateRoot, Record record, Action<string>? log)
    {
        try
        {
            Directory.CreateDirectory(stateRoot);
            AtomicFile.WriteText(MarkerPath(stateRoot), JsonSerializer.Serialize(new { done = record.Done.Distinct().ToArray(), pending = record.Pending }, Indented));
            return true;
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException)
        {
            log?.Invoke($"The profile update record ({MarkerPath(stateRoot)}) could not be written ({e.Message}); the update waits for the next start.");
            return false;
        }
    }
    private static readonly JsonSerializerOptions Indented = new() { WriteIndented = true };
}

/// <summary>What the editor shows when the app opens: <see cref="Status"/> for the page; <see cref="Dcs"/> and
/// <see cref="Options"/> replace the selected DCS files when not null; <see cref="SaveDraft"/>: keep it as the draft.</summary>
public sealed record StartProfile(VrProfile Profile, string? Dcs, string? Options, string Status, bool SaveDraft = false)
{
    public const string AppliedStatus = "Showing the applied profile. Launch DCS uses it as it is.";
    public const string EmulatedListsStatus = "Multi-threading without driver support is now on for AMD graphics; Launch DCS applies it.";

    /// <summary>The applied profile (its saved profile.json, with the DCS files it was written for), unless the draft left
    /// on screen last time differs (settings, DCS install or options.lua). A draft that differs from the applied profile
    /// only by Multi-threading without driver support made automatic (the one-time update of profiles from 0.7.0 and
    /// earlier, <see cref="ProfileMigrations"/>) says so instead of "differs" (on AMD-only graphics, where that turns it
    /// on; elsewhere nothing changes, and the applied line is shown). Without a usable draft, when that update ran at this
    /// start, the applied profile gets the same change and is kept as the draft. Null: nothing saved, the defaults.</summary>
    public static StartProfile? Choose(VrProfile? applied, string? appliedDcs, string? appliedOptions, SavedDraft? draft, bool emulatedListsMigrated)
    {
        if (applied is null)
            return draft is null ? null : new(draft.Profile, draft.Dcs, draft.Options, "Your last draft is back. Launch DCS applies it.");
        var migratedApplied = applied with { EngineRecordersEmulatedLists = null };
        var status = migratedApplied.RecordersEmulatedListsOn ? EmulatedListsStatus : AppliedStatus;
        if (draft is null)
            return emulatedListsMigrated && applied.EngineRecordersEmulatedLists == false
                ? new(migratedApplied, appliedDcs, appliedOptions, status, SaveDraft: true)
                : new(applied, appliedDcs, appliedOptions, AppliedStatus);
        var samePaths = ControlService.SamePath(draft.Dcs, appliedDcs) && ControlService.SamePath(draft.Options, appliedOptions);
        if (samePaths && ControlService.SameDraft(draft.Profile, applied)) return new(applied, appliedDcs, appliedOptions, AppliedStatus);
        var onlyEmulatedLists = samePaths && applied.EngineRecordersEmulatedLists == false && draft.Profile.EngineRecordersEmulatedLists is null && ControlService.SameDraft(draft.Profile, migratedApplied);
        return new(draft.Profile, draft.Dcs ?? appliedDcs, draft.Options ?? appliedOptions,
            onlyEmulatedLists ? status : "Your last draft is back; it differs from the applied profile. Undo changes brings it back.");
    }
}
