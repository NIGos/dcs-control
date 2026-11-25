using System.Text.Json;
using System.Text.Json.Nodes;

namespace DcsVr.Core;

/// <summary>
/// One-time changes to the profiles kept on this PC (the draft and the presets) when a new version changes a default
/// that earlier versions stored explicitly. Each runs once per state folder: state root\migrations.json lists the ones
/// done. Nothing in the profile format changes, so the files stay readable by the version that wrote them (Go back gives
/// an earlier version its own snapshot anyway, <see cref="Rollback"/>). A value the pilot sets afterwards is kept: the
/// migration never runs again. The file is outside what Go back swaps, so going back and updating again does not
/// repeat it either.
/// </summary>
public static class ProfileMigrations
{
    /// <summary>"Multi-threading without driver support" ([Model] RecordersEmulatedLists) became on by default after 0.7.0
    /// (tested on a Radeon, GitHub #27). Profiles saved by 0.7.0 and earlier store "engineRecordersEmulatedLists": false
    /// (every value is written), which would keep it off.</summary>
    public const string EmulatedListsOn = "recorders-emulated-lists-on";

    public static string MarkerPath(string stateRoot) => Path.Combine(stateRoot, "migrations.json");

    /// <summary>Runs the migrations not yet done in <paramref name="stateRoot"/>; returns the ones that ran now. A file
    /// that cannot be read as JSON is left as it is (the app reports it when it loads it). When a file cannot be written
    /// now, the migration is not recorded and runs again at the next start.</summary>
    public static IReadOnlyList<string> Run(string stateRoot, Action<string>? log = null)
    {
        var done = Done(stateRoot);
        if (done.Contains(EmulatedListsOn)) return [];
        var changed = 0;
        try
        {
            foreach (var (path, inDraft) in ProfileFiles(stateRoot))
                if (TurnOnEmulatedLists(path, inDraft)) changed++;
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException)
        {
            log?.Invoke("Profile update (multi-threading without driver support on by default) could not finish: " + e.Message + ". It runs again at the next start.");
            return [];
        }
        Record(stateRoot, done.Append(EmulatedListsOn));
        if (changed > 0) log?.Invoke($"Multi-threading without driver support is now on by default: turned on in {changed} saved profile file(s).");
        return [EmulatedListsOn];
    }

    /// <summary>The draft (its "profile" object) and every preset file (a profile at the root).</summary>
    private static IEnumerable<(string Path, bool InDraft)> ProfileFiles(string stateRoot)
    {
        var draft = Path.Combine(stateRoot, "draft.json");
        if (File.Exists(draft)) yield return (draft, true);
        var presets = Path.Combine(stateRoot, "presets");
        if (Directory.Exists(presets))
            foreach (var file in Directory.EnumerateFiles(presets, "*.json"))
                yield return (file, false);
    }

    /// <summary>Sets "engineRecordersEmulatedLists" from false to true, changing nothing else; a profile without the key
    /// already loads with the new default. True when the file was rewritten.</summary>
    internal static bool TurnOnEmulatedLists(string path, bool inDraft)
    {
        JsonNode? root;
        try { root = JsonNode.Parse(File.ReadAllText(path)); }
        catch (JsonException) { return false; }
        var profile = inDraft ? root?["profile"] : root;
        if (profile is not JsonObject obj) return false;
        var key = obj.Select(p => p.Key).FirstOrDefault(k => k.Equals("engineRecordersEmulatedLists", StringComparison.OrdinalIgnoreCase));
        if (key is null || obj[key] is not JsonValue value || value.GetValueKind() != JsonValueKind.False) return false;
        obj[key] = true;
        AtomicFile.WriteText(path, root!.ToJsonString(Indented));
        return true;
    }

    private static readonly JsonSerializerOptions Indented = new() { WriteIndented = true };

    private static HashSet<string> Done(string stateRoot)
    {
        try
        {
            var path = MarkerPath(stateRoot);
            if (!File.Exists(path)) return [];
            using var document = JsonDocument.Parse(File.ReadAllText(path));
            return document.RootElement.ValueKind == JsonValueKind.Object && document.RootElement.TryGetProperty("done", out var list) && list.ValueKind == JsonValueKind.Array
                ? [.. list.EnumerateArray().Where(e => e.ValueKind == JsonValueKind.String).Select(e => e.GetString()!)]
                : [];
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException or JsonException) { return []; }
    }

    private static void Record(string stateRoot, IEnumerable<string> done)
    {
        try
        {
            Directory.CreateDirectory(stateRoot);
            AtomicFile.WriteText(MarkerPath(stateRoot), JsonSerializer.Serialize(new { done = done.Distinct().ToArray() }, Indented));
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException) { }
    }
}
