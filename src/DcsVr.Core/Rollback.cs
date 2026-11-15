using System.Text.Json;

namespace DcsVr.Core;

/// <summary>What this installation replaced (install folder\.installer\previous.json), written by the installer of the
/// version that replaced it: the setup and the CLI's app-install both go through <see cref="ApplicationInstaller.Install"/>.</summary>
public sealed record PreviousVersion(string Version, string ReplacedBy, DateTimeOffset At);

/// <summary>A copy of the profile state (state root\versions\&lt;version&gt;\info.json) as <paramref name="Version"/> left it.
/// <paramref name="SetAside"/>: taken when the pilot went back from that version, so it holds settings the earlier
/// version may not read; never deleted. <paramref name="Returned"/>: its presets are back in place after a later update.</summary>
public sealed record StateCopy(string Version, DateTimeOffset At, bool SetAside, bool Returned = false);

/// <summary>A going back that was started (state root\versions\going-back.json): read at the next start to tell whether
/// it happened. <paramref name="SetAside"/> is the folder name of the state set aside, null when there was none.</summary>
public sealed record PendingRollback(string From, string To, DateTimeOffset At, string? LastSeenVersion, string? SetAside);

/// <summary>The version one step back, or why there is none. Both null: not on record, the latest earlier release on
/// GitHub is used.</summary>
public sealed record RollbackTarget(string? Version, string? Reason);

/// <summary>What the start found: a going back that did not happen was undone, or presets set aside came back.</summary>
public sealed record RollbackReconciled(bool Undone, IReadOnlyList<string> ReturnedPresets);

/// <summary>
/// Going back to the version installed before this one, one step only. The earlier version's own signed setup does the
/// installing (kept from its update, or downloaded again: <see cref="Updates.KeptSetup"/>, <see cref="Updates.Earlier"/>);
/// this class decides which version that is and keeps the settings readable for it. An earlier build refuses profile
/// settings it does not know, so the draft and the presets go back to the copy taken when that version was replaced
/// (<see cref="Snapshot"/>, by the installer), and the current ones are set aside in state root\versions\&lt;this
/// version&gt;, where they stay; presets saved since return when a later version starts (<see cref="Reconcile"/>).
/// settings.json is read leniently by every version and is never swapped. The record of originals, the applied profile
/// and the managed folder describe DCS's files and are never copied: DCS goes back to stock first, with this version's
/// own restore (the bridge does that).
/// </summary>
public sealed class Rollback
{
    /// <summary>The installer's journals are read strictly, and their format last changed in 0.6.3 (the consolidated
    /// baseline): an earlier setup cannot update an installation of this version. Raise this when the journal changes.</summary>
    public static readonly (int Major, int Minor, int Patch) Oldest = (0, 6, 3);
    /// <summary>An earlier version's setup is kept this long after the update; then going back downloads it again.</summary>
    public static readonly TimeSpan KeepSetupFor = TimeSpan.FromDays(30);

    private static readonly JsonSerializerOptions Lenient = new() { PropertyNamingPolicy = JsonNamingPolicy.CamelCase, WriteIndented = true };
    private readonly string _state;
    private readonly string? _install;
    private readonly Func<DateTimeOffset> _clock;

    /// <param name="installRoot">The installation (the folder with installation.json); null for an unzipped copy.</param>
    public Rollback(string stateRoot, string? installRoot, string? currentVersion = null, Func<DateTimeOffset>? clock = null)
    {
        _state = Path.GetFullPath(stateRoot); _install = installRoot is null ? null : Path.GetFullPath(installRoot);
        Current = currentVersion ?? ProductInfo.Version; _clock = clock ?? (() => DateTimeOffset.UtcNow);
    }
    public string Current { get; }
    public string VersionsRoot => VersionsIn(_state);
    private string PendingPath => Path.Combine(VersionsRoot, "going-back.json");
    private static string VersionsIn(string stateRoot) => Path.Combine(stateRoot, "versions");
    public static string RecordPath(string installRoot) => Path.Combine(installRoot, ".installer", "previous.json");
    private static bool Same(string? a, string? b) => ReleaseVersion.Parse(a) is { } x && ReleaseVersion.Parse(b) is { } y && x.CompareTo(y) == 0;
    private static bool Reachable(ReleaseVersion v) => (v.Major, v.Minor, v.Patch).CompareTo(Oldest) >= 0;

    /// <summary>The installer's note of the version it replaced. Never fails an installation.</summary>
    public static void Record(string installRoot, string replaced, string installed, DateTimeOffset now)
    {
        if (ReleaseVersion.Parse(replaced) is not { } from || ReleaseVersion.Parse(installed) is not { } to || from.CompareTo(to) == 0) return;
        try { AtomicFile.WriteText(RecordPath(installRoot), JsonSerializer.Serialize(new PreviousVersion(from.ToString(), to.ToString(), now), Lenient)); }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException) { }
    }

    /// <summary>What this installation replaced; null when not on record (or for an unzipped copy).</summary>
    public PreviousVersion? Recorded => _install is null ? null : Read<PreviousVersion>(RecordPath(_install)) is { Version: not null, ReplacedBy: not null } r && ReleaseVersion.Parse(r.Version) is not null ? r : null;

    /// <summary>The version one step back, without asking the network.</summary>
    public RollbackTarget Target()
    {
        if (_install is null) return new(null, "This copy is not installed, so it replaced nothing. Earlier versions are on the Releases page.");
        if (Recorded is not { } record) return new(null, null);
        var previous = ReleaseVersion.Parse(record.Version)!; var current = ReleaseVersion.Parse(Current);
        if (current is null || previous.CompareTo(current) >= 0) return new(null, "You already went back one version.");
        if (!Reachable(previous)) return new(null, $"DCS Control {previous.Short} is too old to go back to from here. Its setup is on the Releases page.");
        return new(previous.ToString(), null);
    }

    /// <summary>Whether a copy of the settings as <paramref name="version"/> left them exists.</summary>
    public bool HasSnapshot(string? version) => ReleaseVersion.Parse(version) is { } v && Info(Path.Combine(VersionsRoot, v.ToString())) is not null;

    /// <summary>The newest settings set aside by going back that have not returned yet: (version, folder), or null.</summary>
    public (string Version, string Folder)? SetAside()
    {
        try
        {
            return Copies(_state).Where(c => c.Info is { SetAside: true, Returned: false }).OrderByDescending(c => c.Info.At).Select(c => ((string, string)?)(c.Info.Version, c.Folder)).FirstOrDefault();
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException) { return null; }
    }

    /// <summary>
    /// Copies the draft and the presets into state root\versions\&lt;version&gt;\ as <paramref name="version"/> left them.
    /// The installer calls it before the first start of the version that replaces it; going back calls it with
    /// <paramref name="setAside"/> for the version being left. A copy set aside earlier for the same version is renamed
    /// ("… kept &lt;time&gt;"), never deleted. Returns the folder, or null when there is no draft and no preset.
    /// </summary>
    public static string? Snapshot(string stateRoot, string version, bool setAside, DateTimeOffset now)
    {
        if (ReleaseVersion.Parse(version) is not { } v) return null;
        stateRoot = Path.GetFullPath(stateRoot);
        if (!ProfileFiles(stateRoot).Any()) return null;
        var folder = Path.Combine(VersionsIn(stateRoot), v.ToString()); var building = folder + ".new";
        if (Directory.Exists(building)) Directory.Delete(building, true);
        CopyProfile(stateRoot, building);
        AtomicFile.WriteText(Path.Combine(building, "info.json"), JsonSerializer.Serialize(new StateCopy(v.ToString(), now, setAside), Lenient));
        if (Directory.Exists(folder))
        {
            if (Info(folder) is { SetAside: true })
            {
                var kept = folder + " kept " + now.UtcDateTime.ToString("yyyyMMdd-HHmmss", System.Globalization.CultureInfo.InvariantCulture);
                Directory.Move(folder, Directory.Exists(kept) ? kept + "-" + Guid.NewGuid().ToString("N")[..6] : kept);
            }
            else Directory.Delete(folder, true);
        }
        Directory.Move(building, folder);
        return folder;
    }

    /// <summary>
    /// Just before the earlier version's setup starts: sets the current draft and presets aside, puts back the copy taken
    /// when <paramref name="to"/> was replaced (none: the earlier version starts from its defaults), and notes in
    /// settings.json the version being left, so no update check offers it again by itself, and the earlier version as
    /// seen, so its What's new does not open. The earlier version's update cache is written as checked now: a build
    /// that does not know about skipped versions stays quiet for its 6 hours. Anything that fails is undone.
    /// </summary>
    public PendingRollback Prepare(string to, AppSettingsStore settings)
    {
        var target = (ReleaseVersion.Parse(to) ?? throw new InvalidDataException("The earlier version is not a version.")).ToString();
        var saved = settings.Read() ?? throw new IOException("DCS Control's settings file could not be read just now. Try again.");
        var now = _clock();
        var aside = Snapshot(_state, Current, setAside: true, now);
        var pending = new PendingRollback(Current, target, now, saved.LastSeenVersion, aside is null ? null : Path.GetFileName(aside));
        AtomicFile.WriteText(PendingPath, JsonSerializer.Serialize(pending, Lenient));
        try
        {
            var snapshot = Path.Combine(VersionsRoot, target);
            ClearProfile(_state);
            if (Info(snapshot) is not null) CopyProfile(snapshot, _state);
            settings.Save(saved with { SkippedVersion = Current, LastSeenVersion = target });
            try { AtomicFile.WriteText(Path.Combine(_state, "updates.json"), JsonData.Serialize(new UpdateCheck(now, target, null, null))); }
            catch (Exception e) when (e is IOException or UnauthorizedAccessException) { }
            return pending;
        }
        catch { try { Undo(pending, settings); } catch (Exception e) when (e is IOException or UnauthorizedAccessException) { } throw; }
    }

    /// <summary>Puts everything back as it was before <see cref="Prepare"/>: the setup could not start, or it did not install.</summary>
    public void Undo(PendingRollback pending, AppSettingsStore settings)
    {
        if (pending.SetAside is null) ClearProfile(_state);
        else if (Path.Combine(VersionsRoot, Path.GetFileName(pending.SetAside)) is var folder && Info(folder) is not null)
        {
            ClearProfile(_state); CopyProfile(folder, _state); Directory.Delete(folder, true);
        }
        if (settings.Read() is { } now) settings.Save(now with { SkippedVersion = null, LastSeenVersion = pending.LastSeenVersion });
        File.Delete(Path.Combine(_state, "updates.json"));
        File.Delete(PendingPath);
    }

    /// <summary>
    /// At every start, before the draft is read. A going back that did not happen (this is still the version that was
    /// being left, and nothing was installed since) is undone. Otherwise the skipped version is forgotten once this
    /// version is not older than it, and presets set aside by a version not newer than this one come back: only names
    /// that do not exist now, nothing is overwritten. Never throws.
    /// </summary>
    public RollbackReconciled Reconcile(AppSettingsStore settings)
    {
        try
        {
            // Only the installation decides whether its going back happened: an unzipped copy of the version that was
            // left, started while the earlier version is installed, shares this state folder and would undo it (and
            // delete the draft and presets the earlier version has saved since). It changes nothing until the installation
            // has looked.
            if (Read<PendingRollback>(PendingPath) is { From: not null, To: not null } pending)
            {
                if (_install is null) return new(false, []);
                var installedSince = Recorded is { } record && record.At > pending.At && Same(record.ReplacedBy, Current);
                if (Same(pending.From, Current) && !installedSince) { KeepSince(pending); Undo(pending, settings); return new(true, []); }
                File.Delete(PendingPath);
            }
            if (settings.Read() is { SkippedVersion: { } skipped } saved && !ReleaseVersion.IsNewer(skipped, Current)) settings.Save(saved with { SkippedVersion = null });
            var returned = new List<string>();
            if (ReleaseVersion.Parse(Current) is { } current)
                foreach (var (folder, info) in Copies(_state).Where(c => c.Info is { SetAside: true, Returned: false }).OrderBy(c => c.Info.At))
                {
                    if (ReleaseVersion.Parse(info.Version) is not { } version || version.CompareTo(current) > 0) continue;
                    var presets = Path.Combine(folder, "presets");
                    if (Directory.Exists(presets))
                        foreach (var file in Directory.EnumerateFiles(presets, "*.json"))
                        {
                            var destination = Path.Combine(_state, "presets", Path.GetFileName(file));
                            if (File.Exists(destination)) continue;
                            Directory.CreateDirectory(Path.GetDirectoryName(destination)!); File.Copy(file, destination);
                            returned.Add(Path.GetFileNameWithoutExtension(file));
                        }
                    AtomicFile.WriteText(Path.Combine(folder, "info.json"), JsonSerializer.Serialize(info with { Returned = true }, Lenient));
                }
            return new(false, returned);
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException or JsonException or NotSupportedException) { return new(false, []); }
    }

    /// <summary>
    /// Before a going back is undone at a later start. The earlier version may have run after all: it was uninstalled
    /// and this version installed afresh, so no note says an installation happened. What it saved since (every draft
    /// or preset file that is not the copy put in place for it) goes to state root\versions\&lt;earlier&gt; kept &lt;time&gt;,
    /// set aside like any other: its presets return at the next start, nothing is deleted. Nothing to keep when the
    /// setup never ran.
    /// </summary>
    private void KeepSince(PendingRollback pending)
    {
        if (ReleaseVersion.Parse(pending.To) is not { } to) return;
        var placed = Path.Combine(VersionsRoot, to.ToString());
        var changed = ProfileFiles(_state).Where(f => !File.Exists(Path.Combine(placed, f)) || !File.ReadAllBytes(Path.Combine(placed, f)).AsSpan().SequenceEqual(File.ReadAllBytes(Path.Combine(_state, f)))).ToList();
        if (changed.Count == 0) return;
        var now = _clock();
        var folder = placed + " kept " + now.UtcDateTime.ToString("yyyyMMdd-HHmmss", System.Globalization.CultureInfo.InvariantCulture);
        if (Directory.Exists(folder)) folder += "-" + Guid.NewGuid().ToString("N")[..6];
        foreach (var file in changed)
        {
            var destination = Path.Combine(folder, file);
            Directory.CreateDirectory(Path.GetDirectoryName(destination)!); File.Copy(Path.Combine(_state, file), destination);
        }
        AtomicFile.WriteText(Path.Combine(folder, "info.json"), JsonSerializer.Serialize(new StateCopy(to.ToString(), now, SetAside: true), Lenient));
    }

    /// <summary>The profile state an earlier build must be able to read: the draft and the files in presets\.</summary>
    private static IEnumerable<string> ProfileFiles(string root)
    {
        if (File.Exists(Path.Combine(root, "draft.json"))) yield return "draft.json";
        var presets = Path.Combine(root, "presets");
        if (!Directory.Exists(presets)) yield break;
        foreach (var file in Directory.EnumerateFiles(presets))
            if (Path.GetFileName(file) is { } name && !name.EndsWith(".tmp", StringComparison.OrdinalIgnoreCase)) yield return Path.Combine("presets", name);
    }
    private static void CopyProfile(string from, string to)
    {
        Directory.CreateDirectory(to);
        foreach (var file in ProfileFiles(from).ToList())
        {
            var destination = Path.Combine(to, file);
            Directory.CreateDirectory(Path.GetDirectoryName(destination)!); File.Copy(Path.Combine(from, file), destination, overwrite: true);
        }
    }
    private static void ClearProfile(string root) { foreach (var file in ProfileFiles(root).ToList()) File.Delete(Path.Combine(root, file)); }
    private static StateCopy? Info(string folder) => Read<StateCopy>(Path.Combine(folder, "info.json")) is { Version: not null } info ? info : null;
    private static IEnumerable<(string Folder, StateCopy Info)> Copies(string stateRoot)
    {
        var root = VersionsIn(stateRoot);
        if (!Directory.Exists(root)) yield break;
        foreach (var folder in Directory.EnumerateDirectories(root))
            if (!folder.EndsWith(".new", StringComparison.OrdinalIgnoreCase) && Info(folder) is { } info) yield return (folder, info);
    }
    private static T? Read<T>(string path) where T : class
    {
        try { return File.Exists(path) ? JsonSerializer.Deserialize<T>(File.ReadAllText(path), Lenient) : null; }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException or JsonException or NotSupportedException) { return null; }
    }
}
