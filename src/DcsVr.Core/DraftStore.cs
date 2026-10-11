namespace DcsVr.Core;

/// <summary>The draft on screen, kept across app restarts (state root\draft.json). Written by the interface a moment
/// after each edit; read at start in preference to the applied profile. Named profiles still go through Save/Import.</summary>
public sealed class DraftStore(string stateRoot)
{
    public string Path { get; } = System.IO.Path.Combine(stateRoot, "draft.json");

    public void Save(VrProfile profile, string? dcs, string? options) =>
        AtomicFile.WriteText(Path, JsonData.Serialize(new SavedDraft(profile, dcs, options, DateTimeOffset.UtcNow)));

    /// <summary>The saved draft settled like the checklist settles it, or null when there is none or it cannot be read.</summary>
    public SavedDraft? Load()
    {
        try
        {
            if (!File.Exists(Path)) return null;
            var saved = JsonData.Deserialize<SavedDraft>(File.ReadAllText(Path));
            return saved?.Profile is null || saved.Profile.SchemaVersion != 1 ? null : saved with { Profile = ProfileValidation.ResolveFeatures(saved.Profile) };
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException or System.Text.Json.JsonException or InvalidDataException or NotSupportedException) { return null; }
    }
}

/// <param name="Dcs">DCS.exe selected with the draft; null when detected.</param>
/// <param name="Options">options.lua selected with the draft; null when detected.</param>
public sealed record SavedDraft(VrProfile Profile, string? Dcs, string? Options, DateTimeOffset SavedAt);
