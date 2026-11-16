namespace DcsVr.Core;

/// <summary>The draft on screen, kept across app restarts (state root\draft.json). Written by the interface a moment
/// after each edit; read at start in preference to the applied profile. Named profiles still go through Save/Import.</summary>
public sealed class DraftStore(string stateRoot)
{
    public string Path { get; } = System.IO.Path.Combine(stateRoot, "draft.json");

    public void Save(VrProfile profile, string? dcs, string? options) =>
        AtomicFile.WriteText(Path, JsonData.Serialize(new SavedDraft(profile, dcs, options, DateTimeOffset.UtcNow)));

    /// <summary>The saved draft settled like the checklist settles it, or null when there is none or it cannot be used.
    /// A file another program holds for a moment is read again (<see cref="AtomicFile.Retry"/>). A draft that is there
    /// and cannot be used (damaged, or written by a version whose settings this one does not know) is copied next to it
    /// as draft.unreadable-&lt;time&gt;.json before the next edit writes a new one: once per content, and
    /// <paramref name="log"/> says where.</summary>
    public SavedDraft? Load(Action<string>? log = null)
    {
        byte[] bytes;
        try
        {
            if (!File.Exists(Path)) return null;
            byte[]? read = null;
            AtomicFile.Retry(() => read = File.ReadAllBytes(Path));
            bytes = read!;
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException)
        {
            log?.Invoke("The draft could not be read (" + e.Message + "); the app starts without it.");
            return null;
        }
        try
        {
            var saved = JsonData.Deserialize<SavedDraft>(new System.Text.UTF8Encoding(false, true).GetString(bytes));
            if (saved?.Profile is not null && saved.Profile.SchemaVersion == 1) return saved with { Profile = ProfileValidation.ResolveFeatures(saved.Profile) };
        }
        catch (Exception e) when (e is System.Text.Json.JsonException or InvalidDataException or NotSupportedException or ArgumentException) { }
        KeepUnreadable(bytes, log);
        return null;
    }

    private void KeepUnreadable(byte[] bytes, Action<string>? log)
    {
        try
        {
            var folder = System.IO.Path.GetDirectoryName(Path)!;
            foreach (var kept in Directory.EnumerateFiles(folder, "draft.unreadable-*.json"))
                if (new FileInfo(kept).Length == bytes.LongLength && File.ReadAllBytes(kept).AsSpan().SequenceEqual(bytes)) return;
            var copy = System.IO.Path.Combine(folder, "draft.unreadable-" + DateTime.UtcNow.ToString("yyyyMMdd-HHmmss", System.Globalization.CultureInfo.InvariantCulture) + ".json");
            if (File.Exists(copy)) copy = copy[..^5] + "-" + Guid.NewGuid().ToString("N")[..6] + ".json";
            File.WriteAllBytes(copy, bytes);
            log?.Invoke("The saved draft could not be used and was kept as " + copy + "; the app starts without it.");
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException) { log?.Invoke("The saved draft could not be used, and no copy of it could be kept (" + e.Message + ")."); }
    }
}

/// <param name="Dcs">DCS.exe selected with the draft; null when detected.</param>
/// <param name="Options">options.lua selected with the draft; null when detected.</param>
public sealed record SavedDraft(VrProfile Profile, string? Dcs, string? Options, DateTimeOffset SavedAt);
