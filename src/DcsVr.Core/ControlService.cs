using System.Diagnostics;
using System.Text;

namespace DcsVr.Core;

public sealed class ControlService(string distributionRoot, string stateRoot)
{
    public string DistributionRoot { get; } = Path.GetFullPath(distributionRoot);
    public string StateRoot { get; } = Path.GetFullPath(stateRoot);
    /// <summary>Originals of every path DCS Control wrote, and the applied profile. Journals of earlier versions
    /// (state root\transactions, and copies Windows kept for the app when it ran inside another app's package) are
    /// converted on first use.</summary>
    public OriginalsStore Originals => new(Path.Combine(StateRoot, "originals"), LegacyJournalFolders, OwnHashes);
    public string ManagedRoot => Path.Combine(StateRoot, "managed");
    public string PackageRoot => Path.Combine(DistributionRoot, "packages");
    /// <summary>%LOCALAPPDATA%\DcsControl; an installation that already keeps its state in the folder of the app's earlier
    /// name (%LOCALAPPDATA%\DcsVrControl, before it was renamed DCS Control) goes on using it, so its record of original
    /// files and its applied profile stay exactly where Back to stock DCS expects them.</summary>
    public static string DefaultStateRoot => StateRootIn(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData));
    public static string StateRootIn(string localAppData)
    {
        var current = Path.Combine(localAppData, "DcsControl"); var earlier = Path.Combine(localAppData, LegacyStateFolder);
        return !Directory.Exists(current) && Directory.Exists(earlier) ? earlier : current;
    }
    /// <summary>The state folder's name before the app was renamed DCS Control.</summary>
    public const string LegacyStateFolder = "DcsVrControl";
    /// <summary>The app's saved copy of the user's DLSS 5 runtime (state root\runtimes).</summary>
    public NeuralRuntimeStore SavedRuntime => new(Path.Combine(StateRoot, "runtimes"));

    /// <summary>
    /// Journal folders of earlier versions: state root\transactions and, for the default state root, the private copies
    /// Windows keeps when the app was started from inside a packaged app (MSIX virtualizes %LOCALAPPDATA% writes to
    /// %LOCALAPPDATA%\Packages\&lt;package&gt;\LocalCache\Local). Such a copy is a second, divergent set of journals
    /// for the same DCS files, the cause of files "already existing" that no visible journal owned.
    /// </summary>
    private IEnumerable<string> LegacyJournalFolders()
    {
        yield return Path.Combine(StateRoot, "transactions");
        if (!StateRoot.Equals(Path.GetFullPath(DefaultStateRoot), StringComparison.OrdinalIgnoreCase)) yield break;
        var packages = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "Packages");
        string[] candidates;
        try { candidates = Directory.Exists(packages) ? Directory.GetDirectories(packages) : []; }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException) { yield break; }
        foreach (var package in candidates)
        {
            foreach (var name in new[] { "DcsControl", LegacyStateFolder })
            {
                var copy = Path.Combine(package, "LocalCache", "Local", name, "transactions");
                if (Directory.Exists(copy)) yield return copy;
            }
        }
    }

    /// <summary>Hashes of the components this distribution installs and of the saved DLSS 5 runtime: an existing file
    /// with these bytes is DCS Control's own, never another program's.</summary>
    private IReadOnlySet<string> OwnHashes()
    {
        var hashes = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        try
        {
            var components = Path.Combine(DistributionRoot, "components");
            if (Directory.Exists(components))
                foreach (var file in Directory.EnumerateFiles(components, "*.sha256", SearchOption.AllDirectories))
                    if (File.ReadAllText(file).Trim() is { Length: 64 } hash) hashes.Add(hash.ToLowerInvariant());
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException) { }
        if (SavedRuntime.Read()?.Sha256 is { } saved) hashes.Add(saved);
        return hashes;
    }

    /// <summary>True when DLSS 5 is on and the profile names no runtime file that exists, so the saved copy is used.</summary>
    public static bool UsesSavedRuntime(VrProfile profile) =>
        profile.NeuralRendering && (string.IsNullOrWhiteSpace(profile.NeuralRuntimePath) || !File.Exists(profile.NeuralRuntimePath));

    /// <summary>The profile with the saved runtime copy filled in when it has no runtime path of its own (or one that no
    /// longer exists). Profiles keep a null path, meaning "use the saved copy"; only checks and deployment see it filled.</summary>
    public VrProfile ResolveNeuralRuntime(VrProfile profile) => ResolveNeuralRuntime(profile, out _);
    /// <param name="savedSha256">The recorded hash when the resolved file is the saved copy, so deployment can check it.</param>
    public VrProfile ResolveNeuralRuntime(VrProfile profile, out string? savedSha256)
    {
        savedSha256 = null;
        if (!profile.NeuralRendering) return profile;
        var store = SavedRuntime;
        if (UsesSavedRuntime(profile) && store.Current() is { } saved) profile = profile with { NeuralRuntimePath = saved.Path };
        if (!string.IsNullOrWhiteSpace(profile.NeuralRuntimePath) && Path.GetFullPath(profile.NeuralRuntimePath).Equals(store.DllPath, StringComparison.OrdinalIgnoreCase))
            savedSha256 = store.Read()?.Sha256;
        return profile;
    }

    /// <summary>Verifies and saves the selected DLSS 5 runtime as the app's copy (only the latest is kept).</summary>
    public SavedNeuralRuntime RememberNeuralRuntime(string path) => SavedRuntime.Save(path);

    /// <summary>Forgets the saved copy. When the applied profile was installed from it, the file is kept (no longer used
    /// for new drafts) and removed when that profile is restored. Returns true when the file was kept.</summary>
    public bool ForgetNeuralRuntime()
    {
        var inUse = AppliedUsesSavedRuntime();
        SavedRuntime.Forget(keepFile: inUse);
        return inUse;
    }

    /// <summary>The applied profile installed the saved copy's exact bytes as its DLSS 5 runtime.</summary>
    public bool AppliedUsesSavedRuntime()
    {
        if (SavedRuntime.Read() is not { } saved) return false;
        return (Originals.Read() is { State: "applied", Current: { } current } ? current.Entries : null)?
            .Any(e => Path.GetFileName(e.Path).Equals(NeuralRuntimeStore.FileName, StringComparison.OrdinalIgnoreCase) && e.InstalledSha256 == saved.Sha256) == true;
    }

    /// <summary>The plan Launch DCS (or Apply without launching) writes for <paramref name="profile"/>. Read-only. An
    /// applied profile, its files or another program's files never stop it: Apply overwrites them (originals are
    /// backed up the first time a path is written).</summary>
    /// <param name="fovea">Pimax Play's values as already read by the caller (so a stamp it keeps matches the plan);
    /// read now when omitted.</param>
    /// <summary>
    /// Another program's dxgi.dll in DCS's bin folder (ReShade, an overlay) that the profile's loader would set aside
    /// while it is applied: not one of this app's loaders and not the one the applied profile installed. Null when the
    /// profile needs no loader or the file is ours. The pilot is told before Launch, not after.
    /// </summary>
    public ForeignDxgi? ForeignDxgi(VrProfile profile, InventorySnapshot inventory)
    {
        var launch = profile.ForLaunch();
        if (!launch.UsesCheeky || inventory.DcsExecutable is not { } exe || Path.GetDirectoryName(exe) is not { } bin) return null;
        var path = Path.Combine(bin, "dxgi.dll");
        try
        {
            if (!File.Exists(path)) return null;
            var hash = Hashing.FileSha256(path);
            var ours = new[] { "components/cheeky/dxgi.dll", "components/cheeky-focus/dxgi.dll" }.Select(p => Path.Combine(DistributionRoot, p)).Where(File.Exists).Select(Hashing.FileSha256);
            var installed = ReadAppliedProfile()?.Journal.Entries.Any(e => string.Equals(e.Path, path, StringComparison.OrdinalIgnoreCase) && e.InstalledSha256 == hash) == true;
            if (installed || ours.Contains(hash, StringComparer.OrdinalIgnoreCase)) return null;
            var info = FileVersionInfo.GetVersionInfo(path);
            var product = new[] { info.ProductName, info.FileDescription, info.CompanyName }.FirstOrDefault(n => !string.IsNullOrWhiteSpace(n))?.Trim();
            return new(path, product, hash);
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException) { return null; }
    }

    /// <summary>
    /// The pilot's own dxgi.dll to chain after the DLSS 5 loader as bin\dxgi2.dll: the one in bin now, or the one an
    /// earlier apply set aside (its backup). Null when there is none, when chaining is off, or when bin\dxgi2.dll
    /// belongs to another program (it is never replaced for this).
    /// </summary>
    public byte[]? ChainedDxgi(VrProfile profile, InventorySnapshot inventory)
    {
        if (!profile.KeepOtherDxgi || !profile.ForLaunch().UsesCheeky || inventory.DcsExecutable is not { } exe || Path.GetDirectoryName(exe) is not { } bin) return null;
        try
        {
            var originals = Originals.Read().Files;
            OriginalFile? Recorded(string name) => originals.FirstOrDefault(f => string.Equals(Path.GetFullPath(f.Path), Path.GetFullPath(Path.Combine(bin, name)), StringComparison.OrdinalIgnoreCase));
            var second = Path.Combine(bin, "dxgi2.dll");
            if (File.Exists(second) && Recorded("dxgi2.dll") is not { Foreign: false }) return null;
            if (ForeignDxgi(profile, inventory) is { } now) return File.ReadAllBytes(now.Path);
            return Recorded("dxgi.dll") is { Foreign: true, BackupFile: { } backup } ? File.ReadAllBytes(PathPolicy.UnderRoot(Originals.Directory, backup)) : null;
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException or InvalidDataException) { return null; }
    }

    public ApplyPlan Preview(VrProfile profile, InventorySnapshot inventory, FoveaResolution? fovea = null)
    {
        // A focus area that follows Pimax Play is read now, so the plan holds exactly the values that will be written.
        // profile.json keeps the user's own draft; the converted values go to the provider configuration and pimax-fovea.json.
        var draft = profile;
        // Optimizations only runs none of the VR settings the draft keeps (profile.json still holds them); the refresh
        // rate is the headset's own when it can be read.
        profile = HeadsetRefresh.Resolve(profile.ForLaunch());
        fovea ??= PimaxFovea.Resolve(profile);
        // A fovea the caller resolved from the raw draft carries the draft's other settings: they are made launch-ready
        // again (both steps change nothing a second time).
        profile = HeadsetRefresh.Resolve(fovea.Profile.ForLaunch());
        // Checked and deployed with the saved runtime copy when the draft has no runtime file of its own; profile.json
        // keeps the draft's own (empty) path.
        profile = ResolveNeuralRuntime(profile, out var savedNeuralSha);
        var appliedFovea = fovea.Applies ? AppliedFovea.From(profile, fovea.Pimax?.Stamp) : null;
        var errors = ProfileValidation.Validate(profile, inventory).Where(i => i.Severity == IssueSeverity.Error).ToArray();
        if (errors.Length > 0) throw new InvalidDataException(string.Join(Environment.NewLine, errors.Select(i => i.Message)));
        var ownedSettings = inventory.OptionsPath is { } optionsPath ? Originals.OwnedSettings(optionsPath) : null;
        var cache = new ComponentCache(Path.Combine(StateRoot, "cache"));
        cache.RemoveStaleStaging(TimeSpan.FromHours(1));
        var staged = new List<string>();
        try { return Build(); }
        // Payload bytes are already copied into the plan, so the verified extraction is no longer needed.
        finally { foreach (var directory in staged) cache.RemoveStaging(directory); }

        ApplyPlan Build()
        {
            string? ofxr = null, cheeky = null, layer = null;
            if (profile.UsesCheeky)
            {
                if (!profile.UsesQuadFocus)
                {
                    // Unmodified upstream Cheeky 0.5.4 binaries, extracted at build time from the pinned archive without version.dll.
                    cheeky = Path.Combine(DistributionRoot, "components/cheeky");
                    foreach (var filename in new[] { "dxgi.dll", "CheekyFoveatedDLSS/CheekyFoveatedDLSSHost.dll", "CheekyFoveatedDLSS/CheekyFoveatedDLSSRuntime.dll" })
                    {
                        var path = PathPolicy.UnderRoot(cheeky, filename);
                        if (!File.Exists(path) || Hashing.FileSha256(path) != File.ReadAllText(path + ".sha256").Trim())
                            throw new InvalidDataException("The Cheeky components are modified or incomplete.");
                    }
                }
                layer = Path.Combine(DistributionRoot, "components/CheekyOpenXRLayer.dll");
                var expected = File.ReadAllText(layer + ".sha256").Trim();
                if (Hashing.FileSha256(layer) != expected) throw new InvalidDataException("Cheeky layer is modified or incomplete.");
            }
            var quad = Path.Combine(DistributionRoot, "components/quadviews");
            if (profile.QuadViews == QuadProvider.QuadViewsFoveated && string.IsNullOrWhiteSpace(profile.QuadViewsLayerDirectory))
            {
                foreach (var filename in new[] { "XR_APILAYER_MBUCCHIA_quad_views_foveated.dll", "openxr-api-layer.json", "settings.cfg" })
                {
                    var path = Path.Combine(quad, filename);
                    if (Hashing.FileSha256(path) != File.ReadAllText(path + ".sha256").Trim()) throw new InvalidDataException("Quad Views provider is modified or incomplete.");
                }
            }
            string? focus = null;
            if (profile.UsesQuadFocus)
            {
                focus = Path.Combine(DistributionRoot, "components/cheeky-focus");
                foreach (var filename in new[] { "dxgi.dll", "CheekyFoveatedDLSS/CheekyFoveatedDLSSHost.dll", "CheekyFoveatedDLSS/CheekyFoveatedDLSSRuntime.dll" })
                {
                    var path = PathPolicy.UnderRoot(focus, filename);
                    if (Hashing.FileSha256(path) != File.ReadAllText(path + ".sha256").Trim())
                        throw new InvalidDataException("The focus adapter is modified or incomplete.");
                }
            }
            string? ofxrLayer = null;
            if (profile.FrameGen != FrameGeneration.Off)
            {
                // OFXR from the djules75 fork (DCS-compatible); see ComponentLocations.
                ofxrLayer = Path.Combine(DistributionRoot, "components/ofxr/XR_APILAYER_XRFrameBridge_diagnostic.dll");
                if (!File.Exists(ofxrLayer) && profile.QuadViews != QuadProvider.QuadViewsFoveated && File.Exists(Path.Combine(PackageRoot, PackageCatalog.Get("ofxr").ArchiveName)))
                {
                    // Development distributions without the fork build: upstream package for stereo profiles only.
                    ofxrLayer = null; ofxr = Import("ofxr");
                }
                else if (!File.Exists(ofxrLayer) || Hashing.FileSha256(ofxrLayer) != File.ReadAllText(ofxrLayer + ".sha256").Trim())
                    throw new InvalidDataException("The OFXR layer is modified or incomplete.");
            }
            string? engine = null;
            if (profile.EngineOptimizations)
            {
                // DCS engine optimizations (native/dcsqvcull), copied to Saved Games\DCS\Scripts.
                engine = Path.Combine(DistributionRoot, "components/dcsqvcull");
                foreach (var filename in EngineFiles)
                {
                    var path = Path.Combine(engine, filename);
                    if (!File.Exists(path) || !File.Exists(path + ".sha256") || Hashing.FileSha256(path) != File.ReadAllText(path + ".sha256").Trim())
                        throw new InvalidDataException("The DCS engine optimizations are modified or incomplete.");
                }
            }
            string? pupilShift = null;
            if (profile.ForLaunch().UsesPupilShift)
            {
                pupilShift = Path.Combine(DistributionRoot, "components/pupilshift/XR_APILAYER_DCSVR_pupil_shift.dll");
                if (!File.Exists(pupilShift) || !File.Exists(pupilShift + ".sha256") || Hashing.FileSha256(pupilShift) != File.ReadAllText(pupilShift + ".sha256").Trim())
                    throw new InvalidDataException("The pupil shift layer is modified or incomplete.");
            }
            return new DeploymentPlanner(new(ofxr, cheeky, layer, quad, focus, ofxrLayer, engine, pupilShift)).Build(profile, inventory, ManagedRoot, draft, appliedFovea, ownedSettings, savedNeuralSha, ChainedDxgi(profile, inventory));
        }
        string Import(string id)
        {
            var package = PackageCatalog.Get(id);
            var directory = cache.Import(package, Path.Combine(PackageRoot, package.ArchiveName));
            staged.Add(directory); return directory;
        }
    }

    internal static readonly string[] EngineFiles = ["DcsQvCull.dll", "DcsQvCullPayload.dll", "DcsQvCull.lua"];

    /// <summary>Writes the plan over whatever is installed (see <see cref="OriginalsStore.Apply"/>). Refused only while a
    /// running DCS could use the files.</summary>
    public OriginalsApply Apply(ApplyPlan plan)
    {
        var originals = Originals;
        EnsureDcsClosed(plan.Files.Select(f => f.Path).Append(plan.Executable).Concat(originals.Read().Files.Select(f => f.Path)), ManagedRoot);
        return originals.Apply(plan);
    }
    /// <summary>Back to stock DCS: every path DCS Control changed goes back to what was there before it first wrote
    /// it, whatever it holds now (owned options.lua settings only, other edits kept). Refused only while DCS runs.</summary>
    public RestoreResult RestoreOriginals()
    {
        var originals = Originals;
        EnsureDcsClosed(originals.Read().Files.Select(f => f.Path).Append(ManagedRoot), ManagedRoot);
        var result = originals.RestoreOriginals();
        // A saved runtime copy forgotten while this profile was applied is deleted once nothing installed uses it.
        if (result.Complete && !AppliedUsesSavedRuntime())
            try { SavedRuntime.RemoveForgotten(); } catch (Exception e) when (e is IOException or UnauthorizedAccessException) { }
        return result;
    }
    /// <summary>Refuses while a running DCS could use the files involved: anything in that DCS's install, in the
    /// Saved Games DCS folders (the known folder, which may be redirected, and the default location), or in the
    /// app's managed folder under %LOCALAPPDATA%. Paths are compared in canonical form, so junctions, symbolic
    /// links, 8.3 names and moved installs are seen through. Without paths, or when a running DCS's location can't be
    /// read, any DCS blocks.</summary>
    public static void EnsureDcsClosed(IEnumerable<string>? paths = null, string? managedRoot = null)
    {
        var running = Process.GetProcessesByName("DCS");
        try
        {
            if (running.Length == 0) return;
            var targets = paths?.Where(p => !string.IsNullOrEmpty(p)).Select(SystemPaths.Canonical).ToArray();
            var savedGames = SystemPaths.SavedGamesCandidates();
            var managed = managedRoot ?? Path.Combine(DefaultStateRoot, "managed");
            foreach (var process in running)
            {
                string? root = null;
                try { if (SystemPaths.ProcessImagePath(process.Id) is { } image) root = Path.GetDirectoryName(Path.GetDirectoryName(SystemPaths.Canonical(image))); }
                catch (Exception e) when (e is System.ComponentModel.Win32Exception or InvalidOperationException or ArgumentException) { }
                if (targets is null || root is null || targets.Any(t => UsedByRunningDcs(t, root, savedGames, managed)))
                    throw new InvalidOperationException("Close DCS before applying or restoring the profile.");
            }
        }
        finally { foreach (var process in running) process.Dispose(); }
    }

    /// <summary>True when <paramref name="target"/> may be in use by a DCS installed at <paramref name="dcsRoot"/>: inside
    /// that install, inside a Saved Games\DCS* folder, or inside the managed component folder. Compares canonical paths.</summary>
    internal static bool UsedByRunningDcs(string target, string dcsRoot, IReadOnlyList<string> savedGames, string managedRoot)
    {
        var path = SystemPaths.Canonical(target);
        if (SystemPaths.IsUnderOrSame(path, dcsRoot) || SystemPaths.IsUnderOrSame(path, managedRoot)) return true;
        foreach (var folder in savedGames)
        {
            var canonical = SystemPaths.Canonical(folder);
            if (SystemPaths.IsUnderOrSame(path, canonical) && Path.GetRelativePath(canonical, path).StartsWith("DCS", StringComparison.OrdinalIgnoreCase)) return true;
        }
        return false;
    }

    public string Describe(ApplyPlan plan) => string.Join(Environment.NewLine, new[]
    {
        plan.Description, $"Launch: {plan.Executable}", "", "Files to apply (originals will be backed up):"
    }.Concat(plan.Files.Select(f => $"{(f.ExpectedSha256 is null ? "+" : "~")} {f.Path}\n    {f.Purpose} · {f.Content.Length.ToString("N0", System.Globalization.CultureInfo.InvariantCulture)} byte"))
     .Concat(new[] { "", "DCS settings to apply:" }).Concat(plan.Files.SelectMany(f => f.LuaChanges ?? []).Select(c => $"{c.Path}: {c.PreviousRaw ?? "absent"} -> {c.InstalledRaw}"))
     .Concat(new[] { "", "Environment for the DCS process only:" }).Concat(plan.LaunchEnvironment.Select(p => $"{p.Key}={p.Value}")));

    public string ExportDiagnostic(InventorySnapshot inventory, VrProfile profile, string output)
    {
        var report = new { schemaVersion = 1, version = ProductInfo.Version, inventory, profile, cadence = FramePacing.Describe(profile, inventory), pacingChecks = FramePacing.Checks(profile, inventory), pipeline = profile.UsesQuadFocus ? "DCS quad views -> mapped focus processing -> software stereo composition -> optional OFXR -> selected runtime" : "Standard profile", issues = ProfileValidation.Validate(ResolveNeuralRuntime(profile), inventory), originals = Originals.Status(), packages = PackageCatalog.All };
        AtomicFile.WriteText(output, JsonData.Serialize(report));
        return Path.GetFullPath(output);
    }

    private Process Start(ProcessStartInfo start)
    {
        var process = Process.Start(start) ?? throw new IOException("DCS could not be launched.");
        TryStartCpuBoost(process.Id);
        return process;
    }

    /// <summary>The applied profile's saved draft (profile.json), the DCS paths it was applied to and the Pimax Play
    /// values written with it. Null when no single profile is applied or its files cannot be read.</summary>
    public AppliedProfile? ReadAppliedProfile()
    {
        try
        {
            if (Originals.Read() is not { State: "applied", Current: { } journal }) return null;
            var live = journal.Entries;
            JournalEntry? Entry(string name) => live.FirstOrDefault(e => Path.GetFileName(e.Path).Equals(name, StringComparison.OrdinalIgnoreCase));
            if (Entry("profile.json") is not { } saved || !File.Exists(saved.Path)) return null;
            var profile = JsonData.Deserialize<VrProfile>(File.ReadAllText(saved.Path));
            string? executable = null;
            if (Entry("launch.json") is { } launch && File.Exists(launch.Path))
                executable = JsonData.Deserialize<LaunchConfiguration>(File.ReadAllText(launch.Path)).Executable;
            var options = live.FirstOrDefault(e => e.LuaChanges is not null && Path.GetFileName(e.Path).Equals("options.lua", StringComparison.OrdinalIgnoreCase))?.Path;
            return new(journal, profile, saved.Path, string.IsNullOrWhiteSpace(executable) ? null : executable, options, PimaxFovea.ReadApplied(saved.Path));
        }
        catch (Exception e) when (e is IOException or System.Text.Json.JsonException or UnauthorizedAccessException or InvalidDataException) { return null; }
    }

    /// <summary>Same settings as the applied draft: id and name only follow the features, as in the interface.</summary>
    public static bool SameDraft(VrProfile draft, VrProfile applied) =>
        JsonData.Serialize(draft with { Id = "", Name = "" }) == JsonData.Serialize(applied with { Id = "", Name = "" });

    /// <summary>
    /// Brings the installed configuration in line with <paramref name="draft"/> (the applied profile itself when null),
    /// for Launch DCS and "Apply without launching". Nothing is written when the draft is the applied profile and its
    /// files are intact. When only Pimax Play's Quad View values changed, the bundled Quad Views settings and
    /// pimax-fovea.json are updated in place. When DCS changed only the profile's own settings in options.lua, those
    /// keys are set back in place. Otherwise the draft is written over whatever is installed: no restore first, no
    /// conflicts (originals are backed up the first time a path is written; another program's files are reported).
    /// </summary>
    public LaunchSync SyncApplied(VrProfile? draft, InventorySnapshot inventory)
    {
        var originals = Originals;
        var baseline = originals.Read();
        var applied = baseline.State == "applied" ? ReadAppliedProfile() : null;
        draft ??= applied?.Profile ?? throw new InvalidOperationException("No profile is applied yet. Open DCS Control and launch from there once.");
        // Every file involved: everything DCS Control wrote, the selected DCS install and options, the managed folder.
        EnsureDcsClosed(baseline.Files.Select(f => f.Path).Concat(new[] { inventory.DcsExecutable, inventory.OptionsPath, ManagedRoot }.OfType<string>()), ManagedRoot);
        string? reason = null;
        if (applied is not null && SameDraft(draft, applied.Profile) && SamePaths(inventory, applied))
        {
            if (!TryUpdatePimaxFovea(applied, out var fovea)) reason = "Pimax Play's focus values could not be updated in place";
            else if (SavedRuntimeReplaced(draft, applied.Journal)) reason = "the saved DLSS 5 runtime copy was replaced";
            else
            {
                var journal = originals.ReadCurrent() ?? applied.Journal;
                try
                {
                    VerifyInstalled(journal);
                    return new(fovea is null ? LaunchSyncKind.Unchanged : LaunchSyncKind.FoveaUpdated, fovea ?? "The applied profile is up to date.", journal);
                }
                catch (IOException e)
                {
                    reason = e.Message.TrimEnd('.');
                    // Only the profile's own DCS settings changed (DCS rewrote Max FPS, for example): they are set back in
                    // options.lua in place, keeping everything else DCS changed.
                    if (LuaDriftOnly(journal) && originals.RealignLuaValues() is { Count: > 0 } realigned)
                    {
                        try
                        {
                            journal = originals.ReadCurrent() ?? journal;
                            VerifyInstalled(journal);
                            var settings = $"DCS had changed the profile's settings in options.lua ({string.Join(", ", realigned.Select(r => r[(r.IndexOf(": ", StringComparison.Ordinal) + 2)..]))}); they were set back.";
                            return new(LaunchSyncKind.SettingsRealigned, fovea is null ? settings : fovea + " " + settings, journal);
                        }
                        catch (IOException again) { reason = again.Message.TrimEnd('.'); }
                    }
                }
            }
        }
        var plan = Preview(draft, inventory);
        var result = Apply(plan);
        var replacedProfile = applied?.Journal.ProfileId ?? baseline.Current?.ProfileId;
        // Pilots read the profile's name; the id stays in LaunchSync for the report's "Written over" line and tests.
        var replacedName = applied?.Profile?.Name ?? replacedProfile;
        var message = reason is not null ? $"Applied {plan.Description} again ({reason})."
            : replacedName is not null ? $"Applied {plan.Description} over {replacedName}." : $"Applied {plan.Description}; the original files are backed up.";
        var notes = result.ReplacedForeign.Select(path => $"Replaced another {Path.GetFileName(path)} ({path}); it is backed up and Back to stock DCS brings it back.").ToArray();
        if (notes.Length > 0) message += " " + string.Join(" ", notes);
        return new(LaunchSyncKind.Applied, message, result.Current, replacedProfile, notes);
    }

    /// <summary><see cref="SyncApplied"/>, then the launch contract check. Starts nothing.</summary>
    public (LaunchSync Sync, ProcessStartInfo Start) SyncAndPrepareLaunch(VrProfile? draft, InventorySnapshot inventory)
    {
        // Refused before anything is written: with the launcher kept, the game could not start (see LaunchSafety.CurrentJob).
        if (draft?.KeepDcsLauncher == true && LaunchSafety.LauncherRestartBlocked) throw new InvalidOperationException(LaunchSafety.LauncherBlockedMessage);
        var sync = SyncApplied(draft, inventory);
        return (sync, PrepareLaunchApplied());
    }

    /// <summary>Launch DCS: <see cref="SyncApplied"/>, the launch contract check, then DCS and the CPU Boost helper.</summary>
    public (LaunchSync Sync, Process Process) SyncAndLaunch(VrProfile? draft, InventorySnapshot inventory)
    {
        var (sync, start) = SyncAndPrepareLaunch(draft, inventory);
        return (sync, Start(start));
    }

    /// <summary>The installed profile differs only in owned DCS settings of options.lua: every other check of
    /// <see cref="VerifyInstalled"/> passes.</summary>
    private static bool LuaDriftOnly(TransactionJournal journal)
    {
        try { VerifyInstalled(journal, ignoreLuaValues: true); return true; }
        catch (Exception e) when (e is IOException or InvalidDataException or System.Text.Json.JsonException or UnauthorizedAccessException) { return false; }
    }

    private static bool SamePaths(InventorySnapshot inventory, AppliedProfile applied) =>
        SamePath(inventory.DcsExecutable, applied.Executable) && SamePath(inventory.OptionsPath, applied.OptionsPath);

    /// <summary>The same file; an empty side (detected automatically, or a profile that leaves options.lua untouched)
    /// matches anything. The interface compares the DCS paths the same way.</summary>
    public static bool SamePath(string? selected, string? used)
    {
        if (string.IsNullOrWhiteSpace(selected) || string.IsNullOrWhiteSpace(used)) return true;
        try { return Path.GetFullPath(selected.Trim()).TrimEnd('\\').Equals(Path.GetFullPath(used.Trim()).TrimEnd('\\'), StringComparison.OrdinalIgnoreCase); }
        catch (Exception e) when (e is ArgumentException or NotSupportedException or PathTooLongException) { return false; }
    }

    /// <summary>The draft uses the app's saved DLSS 5 runtime copy and that copy is no longer the installed one.</summary>
    private bool SavedRuntimeReplaced(VrProfile draft, TransactionJournal journal)
    {
        if (!UsesSavedRuntime(draft) || SavedRuntime.Current() is not { } saved) return false;
        var installed = journal.Entries.FirstOrDefault(e => Path.GetFileName(e.Path).Equals(NeuralRuntimeStore.FileName, StringComparison.OrdinalIgnoreCase));
        return installed is not null && installed.InstalledSha256 != saved.Sha256;
    }

    /// <summary>
    /// Pimax Play's Quad View values changed since Apply: rewrites only the bundled Quad Views settings.cfg and
    /// pimax-fovea.json of the applied profile, through <see cref="OriginalsStore.UpdateInstalled"/>. Returns false when a full apply is needed instead: the profile was applied without pimax-fovea.json, other
    /// installed files depend on the values (Cheeky's INI), the installed settings are not exactly what Apply wrote, or
    /// a file changed. <paramref name="message"/> says what was updated; null when nothing changed.
    /// </summary>
    private bool TryUpdatePimaxFovea(AppliedProfile applied, out string? message)
    {
        message = null;
        var profile = applied.Profile;
        if (!PimaxFovea.UsesPimaxPlay(profile)) return true;
        var resolved = PimaxFovea.Resolve(profile);
        // Pimax Play's settings cannot be read now: the applied values stay.
        if (resolved.Pimax is not { } pimax) return true;
        if (applied.Fovea is not { } written) return PimaxFovea.ChangedSinceApply(profile) is null;
        var now = resolved.Profile;
        if (PimaxFovea.SameFovea(now, written)) return true;
        var before = profile with { FoveaWidth = written.FoveaWidth, FoveaHeight = written.FoveaHeight, QuadFocusScale = written.QuadFocusScale, PeripheralScale = written.PeripheralScale, QuadEdgeBlend = written.QuadEdgeBlend };
        if (profile.UsesCheeky && ConfigurationWriters.Cheeky(before) != ConfigurationWriters.Cheeky(now)) return false;
        if (ProfileValidation.Validate(ResolveNeuralRuntime(now)).Any(i => i.Severity == IssueSeverity.Error)) return false;
        var source = Path.Combine(DistributionRoot, "components/quadviews/settings.cfg");
        if (!File.Exists(source) || !File.Exists(source + ".sha256") || Hashing.FileSha256(source) != File.ReadAllText(source + ".sha256").Trim()) return false;
        var folder = Path.GetDirectoryName(applied.ProfilePath)!;
        var settings = Path.Combine(folder, "quadviews", "settings.cfg"); var foveaFile = Path.Combine(folder, AppliedFovea.FileName);
        var utf8 = new UTF8Encoding(false); var provider = File.ReadAllText(source);
        // Exactly what Apply wrote for the old values, so the update changes nothing but the focus values.
        if (!File.Exists(settings) || !File.ReadAllBytes(settings).AsSpan().SequenceEqual(utf8.GetBytes(provider + "\n" + ConfigurationWriters.QuadViews(before)))) return false;
        EnsureDcsClosed([settings, foveaFile], ManagedRoot);
        try
        {
            Originals.UpdateInstalled([(settings, utf8.GetBytes(provider + "\n" + ConfigurationWriters.QuadViews(now))),
                (foveaFile, utf8.GetBytes(JsonData.Serialize(AppliedFovea.From(now, pimax.Stamp))))]);
        }
        catch (Exception e) when (e is IOException or InvalidOperationException or InvalidDataException or UnauthorizedAccessException) { return false; }
        message = $"Pimax Play changed: focus updated to {pimax.Play.Short}.";
        return true;
    }

    /// <summary>Starts the CPU Boost helper as its own process (so it survives the app closing) when the applied
    /// profile asks for it. Failure to start it never fails the launch.</summary>
    private void TryStartCpuBoost(int dcsPid)
    {
        try
        {
            var start = BuildCpuBoostStart(dcsPid);
            if (start is not null) Process.Start(start)?.Dispose();
        }
        catch (Exception e) when (e is IOException or InvalidDataException or UnauthorizedAccessException or System.Text.Json.JsonException or System.ComponentModel.Win32Exception) { }
    }

    /// <summary>The boost helper command for the applied profile, or null when the profile uses neither CPU Boost, Free VRAM before flight nor the flight monitor mode, or the
    /// helper is missing. Passes the launched DCS PID (a hint: the Steam edition restarts itself), the launched
    /// executable (to find DCS again after such a restart) and the dcs.log next to the applied options.lua. Starts
    /// nothing.</summary>
    internal ProcessStartInfo? BuildCpuBoostStart(int dcsPid, string? cliDirectory = null)
    {
        var journal = Originals.ReadCurrent();
        JournalEntry? Entry(string file) => journal?.Entries.FirstOrDefault(e => Path.GetFileName(e.Path).Equals(file, StringComparison.OrdinalIgnoreCase));
        var profileEntry = Entry("profile.json");
        if (profileEntry is null || !File.Exists(profileEntry.Path)) return null;
        var profile = JsonData.Deserialize<VrProfile>(File.ReadAllText(profileEntry.Path));
        if (!profile.UsesBoostHelper) return null;
        var directory = cliDirectory ?? AppContext.BaseDirectory;
        var cli = new[] { "DcsVr.Cli.exe", "DcsVr.Cli" }.Select(name => Path.Combine(directory, name)).FirstOrDefault(File.Exists);
        if (cli is null) return null;
        var start = new ProcessStartInfo(cli) { WorkingDirectory = Path.GetDirectoryName(cli)! };
        var arguments = new List<string> { "boost", "--state", StateRoot, "--profile", profileEntry.Path, "--dcs-pid", dcsPid.ToString(System.Globalization.CultureInfo.InvariantCulture) };
        if (Entry("launch.json") is { } launchEntry && File.Exists(launchEntry.Path))
            arguments.AddRange(["--dcs-exe", JsonData.Deserialize<LaunchConfiguration>(File.ReadAllText(launchEntry.Path)).Executable]);
        if (BoostRuntime.DcsLogFromOptions(Entry("options.lua")?.Path) is { } log) arguments.AddRange(["--dcs-log", log]);
        foreach (var argument in arguments) start.ArgumentList.Add(argument);
        // Elevation is requested only when the profile opts in, so services under other accounts can be moved. The
        // elevated helper is started hidden: closing a visible console would end it before it restores.
        if (profile.BoostHelperElevated) { start.UseShellExecute = true; start.Verb = "runas"; start.WindowStyle = ProcessWindowStyle.Hidden; }
        else { start.UseShellExecute = false; start.CreateNoWindow = true; }
        return start;
    }

    /// <summary>Verifies the complete installed launch contract without executing the game.</summary>
    public ProcessStartInfo PrepareLaunchApplied()
    {
        var current = Originals.Read() is { State: "applied", Current: { } applied } ? applied : null;
        EnsureDcsClosed(current?.Entries.Select(e => e.Path), ManagedRoot);
        if (current is null) throw new InvalidOperationException("No profile is applied. Launch DCS from DCS Control to apply one.");
        var launch = VerifyInstalled(current);
        if (!File.Exists(launch.Executable)) throw new IOException("DCS executable is missing: " + launch.Executable);
        if (LaunchSafety.LauncherRestartBlocked && KeepsLauncher(current)) throw new InvalidOperationException(LaunchSafety.LauncherBlockedMessage);
        LaunchSafety.EnsureNotElevated(launch.Executable);
        // Steam's launch identifiers stop the Steam edition from restarting itself, so Steam itself must already run.
        if (launch.Environment.ContainsKey("SteamAppId"))
        {
            var steam = Process.GetProcessesByName("steam");
            try { if (steam.Length == 0) throw new InvalidOperationException("Start Steam before launching the Steam edition of DCS."); }
            finally { foreach (var process in steam) process.Dispose(); }
        }
        return BuildStartInfo(launch);
    }

    /// <summary>The applied profile shows the DCS launcher (its saved draft has <see cref="VrProfile.KeepDcsLauncher"/>).</summary>
    private static bool KeepsLauncher(TransactionJournal journal)
    {
        var saved = journal.Entries.FirstOrDefault(e => Path.GetFileName(e.Path).Equals("profile.json", StringComparison.OrdinalIgnoreCase));
        try { return saved is not null && File.Exists(saved.Path) && JsonData.Deserialize<VrProfile>(File.ReadAllText(saved.Path)).KeepDcsLauncher; }
        catch (Exception e) when (e is IOException or System.Text.Json.JsonException or UnauthorizedAccessException) { return false; }
    }

    /// <summary>Every installed file still holds what the profile installed and every external dependency is unchanged;
    /// returns the saved launch configuration. Throws <see cref="IOException"/> naming what changed.</summary>
    /// <param name="ignoreLuaValues">Skip the owned DCS settings of options.lua (the file must still exist).</param>
    private static LaunchConfiguration VerifyInstalled(TransactionJournal journal, bool ignoreLuaValues = false)
    {
        foreach (var entry in journal.Entries)
        {
            if (!File.Exists(entry.Path)) throw new IOException("An installed file is missing: " + entry.Path + ". If an antivirus removed it (check its quarantine), restore it there and add an exclusion for that folder; Launch DCS writes it again otherwise.");
            if (entry.LuaChanges is { Count: > 0 })
            {
                if (ignoreLuaValues) continue;
                var lua = new LuaOptions(File.ReadAllText(entry.Path));
                if (entry.LuaChanges.Any(change => LuaOptions.NormalizeLiteral(lua.Get(change.Path.Split('.'))) != LuaOptions.NormalizeLiteral(change.InstalledRaw)))
                    throw new IOException("DCS changed the profile's settings in options.lua");
            }
            else if (entry.IniValues is { Count: > 0 })
            {
                // Cheeky rewrites its INI on first start (defaults, float formatting); only owned values matter.
                if (!IniFile.Matches(File.ReadAllText(entry.Path), entry.IniValues))
                    throw new IOException("The profile's component settings changed: " + entry.Path);
            }
            else if (Hashing.FileSha256(entry.Path) != entry.InstalledSha256) throw new IOException("An installed file changed: " + entry.Path);
        }
        var launchEntry = journal.Entries.FirstOrDefault(e => Path.GetFileName(e.Path).Equals("launch.json", StringComparison.OrdinalIgnoreCase))
            ?? throw new IOException("The applied profile has no launch.json.");
        var launch = JsonData.Deserialize<LaunchConfiguration>(File.ReadAllText(launchEntry.Path));
        foreach (var dependency in launch.Dependencies ?? [])
            if (!File.Exists(dependency.Key) || Hashing.FileSha256(dependency.Key) != dependency.Value)
                throw new IOException("An external runtime, provider or pacing configuration changed: " + dependency.Key);
        foreach (var absent in launch.AbsentDependencies ?? [])
            if (File.Exists(absent)) throw new IOException("A new pacing configuration appeared: " + absent);
        return launch;
    }

    public static ProcessStartInfo BuildStartInfo(LaunchConfiguration launch)
    {
        var start = new ProcessStartInfo(launch.Executable) { UseShellExecute = false, WorkingDirectory = Path.GetDirectoryName(launch.Executable)! };
        foreach (var value in launch.Environment) start.Environment[value.Key] = value.Value;
        return start;
    }
    public void SaveProfile(VrProfile profile, string path) => AtomicFile.WriteText(path, JsonData.Serialize(profile));
    public VrProfile LoadProfile(string path)
    {
        // Settled exactly like the interface's feature checklist, so a combination it fixes by itself is not an error here.
        var p = ProfileValidation.ResolveFeatures(JsonData.Deserialize<VrProfile>(File.ReadAllText(path)));
        var errors = ProfileValidation.Validate(p).Where(i => i.Severity == IssueSeverity.Error && i.Code != "neural-runtime").ToArray();
        if (errors.Length > 0) throw new InvalidDataException(string.Join("\n", errors.Select(e => e.Message)));
        return p;
    }
}
/// <param name="Journal">The applied profile's record (installed files and hashes).</param>
/// <param name="Executable">DCS.exe from the applied launch.json.</param>
/// <param name="OptionsPath">The options.lua the profile changed; null when it left options.lua untouched.</param>
/// <param name="Fovea">pimax-fovea.json: Pimax Play's converted values written at Apply; null when not used.</param>
/// <summary>Another program's dxgi.dll in DCS's bin folder; Product is its name from the file's version information, if any.</summary>
public sealed record ForeignDxgi(string Path, string? Product, string Sha256)
{
    public string Name => Product ?? "another program";
}

public sealed record AppliedProfile(TransactionJournal Journal, VrProfile Profile, string ProfilePath, string? Executable, string? OptionsPath, AppliedFovea? Fovea);
/// <summary>What Launch DCS (or Apply without launching) did to the installed configuration. SettingsRealigned: the
/// profile's own DCS settings in options.lua were set back in place.</summary>
public enum LaunchSyncKind { Unchanged, FoveaUpdated, Applied, SettingsRealigned }
/// <param name="ReplacedProfileId">The profile this one was written over.</param>
/// <param name="Notes">Other programs' files replaced (backed up) by this apply, one sentence each.</param>
public sealed record LaunchSync(LaunchSyncKind Kind, string Message, TransactionJournal Journal, string? ReplacedProfileId = null, IReadOnlyList<string>? Notes = null);
public sealed record LaunchConfiguration(string Executable, Dictionary<string, string> Environment, Dictionary<string, string>? Dependencies = null, IReadOnlyList<string>? AbsentDependencies = null);
