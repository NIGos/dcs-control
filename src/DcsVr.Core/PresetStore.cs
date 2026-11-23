namespace DcsVr.Core;

/// <summary>The user's own presets: named copies of the settings, kept by the app (state root\presets\&lt;name&gt;.json).
/// Each file is an ordinary profile, the same as Export writes, so it can be shared or imported elsewhere. One preset can
/// be active: the interface then saves every edit into it, a moment after the draft.</summary>
/// <param name="Summary">What the preset runs, in the profile's words; null when its file cannot be read.</param>
public sealed record PresetInfo(string Name, string? Summary, DateTime Modified);

public sealed class PresetStore(string stateRoot)
{
    public const int MaxNameLength = 40;
    public string Root { get; } = Path.Combine(stateRoot, "presets");
    private string ActivePath => Path.Combine(Root, "active.txt");

    /// <summary>A name that is also a file name: trimmed, single spaces, no characters Windows refuses.</summary>
    public static string Clean(string? name)
    {
        var invalid = Path.GetInvalidFileNameChars();
        var cleaned = string.Join(' ', new string((name ?? "").Select(c => invalid.Contains(c) || char.IsControl(c) ? ' ' : c).ToArray())
            .Split(' ', StringSplitOptions.RemoveEmptyEntries)).Trim('.', ' ');
        if (cleaned.Length == 0) throw new InvalidDataException("Give the preset a name.");
        if (cleaned.Length > MaxNameLength) throw new InvalidDataException($"A preset name has at most {MaxNameLength} characters.");
        if (cleaned.Equals("active", StringComparison.OrdinalIgnoreCase) || Reserved.Contains(cleaned.Split('.')[0], StringComparer.OrdinalIgnoreCase))
            throw new InvalidDataException("Windows does not allow this name. Choose another.");
        return cleaned;
    }
    private static readonly string[] Reserved = ["CON", "PRN", "AUX", "NUL", "COM1", "COM2", "COM3", "COM4", "COM5", "COM6", "COM7", "COM8", "COM9", "LPT1", "LPT2", "LPT3", "LPT4", "LPT5", "LPT6", "LPT7", "LPT8", "LPT9"];

    public string PathOf(string name) => Path.Combine(Root, Clean(name) + ".json");
    public bool Exists(string name) => File.Exists(PathOf(name));

    /// <summary>The presets by name, as their files are spelled. A file whose name no preset can have (copied in by hand:
    /// too long, double spaces) is left out: every use of the name would refuse it, the page's state included.</summary>
    public IReadOnlyList<string> List() => !Directory.Exists(Root) ? [] :
        [.. Directory.EnumerateFiles(Root, "*.json").Select(Path.GetFileNameWithoutExtension).OfType<string>().Where(IsCleanName).Order(StringComparer.CurrentCultureIgnoreCase)];
    private static bool IsCleanName(string name)
    {
        try { return Clean(name) == name; }
        catch (InvalidDataException) { return false; }
    }

    /// <summary>The preset edits are saved into, or null: none chosen, or its file is gone.</summary>
    public string? Active
    {
        get
        {
            try
            {
                if (!File.Exists(ActivePath)) return null;
                var name = File.ReadAllText(ActivePath).Trim();
                return List().FirstOrDefault(n => n.Equals(name, StringComparison.OrdinalIgnoreCase));
            }
            catch (Exception e) when (e is IOException or UnauthorizedAccessException) { return null; }
        }
    }
    public void SetActive(string? name)
    {
        if (name is null) { if (File.Exists(ActivePath)) File.Delete(ActivePath); return; }
        Directory.CreateDirectory(Root); AtomicFile.WriteText(ActivePath, Clean(name));
    }

    public void Rename(string from, string to)
    {
        string source = PathOf(from), target = PathOf(to);
        if (!File.Exists(source)) throw new InvalidDataException($"The preset {Clean(from)} no longer exists.");
        var sameFile = string.Equals(source, target, StringComparison.OrdinalIgnoreCase);
        if (!sameFile && File.Exists(target)) throw new InvalidDataException($"A preset named {Clean(to)} already exists.");
        var wasActive = string.Equals(Active, Clean(from), StringComparison.OrdinalIgnoreCase);
        if (sameFile) { var temporary = source + ".renaming"; File.Move(source, temporary, true); File.Move(temporary, target); }
        else File.Move(source, target);
        if (wasActive) SetActive(to);
    }

    /// <summary>A copy under another name; the active preset does not change.</summary>
    public void Duplicate(string from, string to)
    {
        string source = PathOf(from), target = PathOf(to);
        if (!File.Exists(source)) throw new InvalidDataException($"The preset {Clean(from)} no longer exists.");
        if (File.Exists(target)) throw new InvalidDataException($"A preset named {Clean(to)} already exists.");
        File.Copy(source, target);
    }

    /// <summary>What the Presets page shows for one preset: what it runs (the profile's own name) and when it last changed.
    /// A file that cannot be read is still listed, so it can be deleted.</summary>
    public IReadOnlyList<PresetInfo> Details() => [.. List().Select(name =>
    {
        var path = PathOf(name); string? summary = null;
        try
        {
            using var doc = System.Text.Json.JsonDocument.Parse(File.ReadAllText(path));
            if (doc.RootElement.ValueKind == System.Text.Json.JsonValueKind.Object && doc.RootElement.TryGetProperty("name", out var value) && value.ValueKind == System.Text.Json.JsonValueKind.String) summary = value.GetString();
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException or System.Text.Json.JsonException) { }
        return new PresetInfo(name, summary, File.GetLastWriteTimeUtc(path));
    })];

    public void Delete(string name)
    {
        var wasActive = string.Equals(Active, Clean(name), StringComparison.OrdinalIgnoreCase);
        var path = PathOf(name); if (File.Exists(path)) File.Delete(path);
        if (wasActive) SetActive(null);
    }
}
