using System.Diagnostics;
using System.IO;
using System.Text.Json;
using System.Windows;
using DcsVr.Core;
using Microsoft.Win32;

namespace DcsVr.App;

/// <summary>One serialized command channel. Web content never owns an approved deployment plan.</summary>
internal sealed class WebBridge(ControlService service, Window? owner = null,
    Func<string?, string?, InventorySnapshot>? capture = null, bool offline = false, Func<string?, string?, FlightSources>? flightSources = null)
{
    private readonly SemaphoreSlim _commands = new(1);
    private InventorySnapshot _inventory = new();
    private VrProfile _profile = new();
    private bool _shownApplied;
    /// <summary>On opening, the editor shows the profile that is applied (its saved profile.json), not a preset, so the
    /// settings on screen are the ones DCS will launch with.</summary>
    private void ShowAppliedProfile()
    {
        var applied = service.ReadAppliedProfile();
        if (applied is not null)
        {
            _profile = applied.Profile;
            // The DCS files the applied profile was written for, not whatever detection would find first.
            if (applied.Executable is { } executable) _dcs = executable;
            if (applied.OptionsPath is { } options) _options = options;
            _status = "Showing the applied profile. Launch DCS uses it as it is.";
        }
        // The draft left on screen last time wins over the applied profile when it differs (settings, DCS install or
        // options.lua); Reset to applied is one click.
        if (Drafts.Load() is { } draft && (applied is null || !ControlService.SameDraft(draft.Profile, applied.Profile)
            || !ControlService.SamePath(draft.Dcs, applied.Executable) || !ControlService.SamePath(draft.Options, applied.OptionsPath)))
        {
            _profile = draft.Profile;
            if (draft.Dcs is { } dcs) _dcs = dcs;
            if (draft.Options is { } options) _options = options;
            _status = applied is null ? "Your last draft is back. Launch DCS applies it." : "Your last draft is back; it differs from the applied profile. Undo changes brings it back.";
        }
    }
    private DraftStore Drafts => new(service.StateRoot);
    private readonly Lock _draftLock = new();
    /// <summary>The page saves the draft a moment after each edit, outside the command lock (a launch may hold it).</summary>
    private string? SaveDraftRequest(string json)
    {
        if (json.Length > 128 * 1024 || !json.Contains("\"saveDraft\"", StringComparison.Ordinal)) return null;
        string? id = null;
        try
        {
            using var doc = JsonDocument.Parse(json);
            var root = doc.RootElement;
            if (root.GetProperty("action").GetString() != "saveDraft") return null;
            id = root.GetProperty("id").GetString();
            var data = root.GetProperty("data");
            var draft = JsonData.Deserialize<VrProfile>(data.GetProperty("profile").GetRawText());
            lock (_draftLock) Drafts.Save(draft, Empty(data.GetProperty("dcs").GetString()), Empty(data.GetProperty("options").GetString()));
            return Reply(id, new { saved = true });
        }
        catch (Exception e) when (e is JsonException or KeyNotFoundException or InvalidOperationException or IOException or UnauthorizedAccessException or NotSupportedException)
        { return JsonData.Serialize(new { id, ok = false, error = e.Message }); }
    }
    private readonly Lock _flightLock = new();
    private (string Key, DateTime At, object Reply)? _flightCache;
    /// <summary>Last flight: read-only, from the logs DCS, the headset runtime, OFXR, Cheeky, CPU Boost and the engine
    /// optimizations' prefetch fix leave behind; answered outside the command lock and reused for 10 s.</summary>
    private Task<string>? LastFlightRequest(string json)
    {
        if (json.Length > 1024 || !json.Contains("\"lastFlight\"", StringComparison.Ordinal)) return null;
        string? id = null;
        try
        {
            using var doc = JsonDocument.Parse(json);
            if (doc.RootElement.GetProperty("action").GetString() != "lastFlight") return null;
            id = doc.RootElement.GetProperty("id").GetString();
        }
        catch (Exception e) when (e is JsonException or KeyNotFoundException or InvalidOperationException) { return Task.FromResult(JsonData.Serialize(new { id, ok = false, error = e.Message })); }
        return Read();
        // Logs written by other programs are never trusted: anything unexpected while reading them is logged and the
        // page simply shows no last flight.
        async Task<string> Read()
        {
            try
            {
                if (DcsRunning()) return Reply(id, new { running = true });
                var sources = FlightSourcesNow();
                var key = JsonData.Serialize(sources);
                lock (_flightLock) if (_flightCache is { } hit && hit.Key == key && DateTime.UtcNow - hit.At < TimeSpan.FromSeconds(10)) return Reply(id, hit.Reply);
                var flight = await Task.Run(() => LastFlight.Read(sources));
                object reply = new { running = false, flight };
                lock (_flightLock) _flightCache = (key, DateTime.UtcNow, reply);
                return Reply(id, reply);
            }
            catch (Exception e)
            {
                AppLog.Error("Last flight", e);
                return Reply(id, new { running = false, flight = (FlightSummary?)null });
            }
        }
    }
    private FlightSources FlightSourcesNow()
    {
        var options = _options ?? _inventory.OptionsPath; var dcs = _dcs ?? _inventory.DcsExecutable;
        if (flightSources is not null) return flightSources(dcs, options);
        var local = Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData);
        return new(DcsLogPath(options), Path.Combine(local, "Pimax", "runtime"), Path.Combine(service.ManagedRoot, "profiles"),
            Path.Combine(service.StateRoot, "boost", "status.json"), dcs is null ? null : Path.GetDirectoryName(dcs));
    }
    /// <summary>"Pimax Play changed …" while the applied profile follows Pimax Play and its values changed since Apply;
    /// Launch DCS updates the focus area first.</summary>
    private static string? FoveaChanged(AppliedProfile? applied) =>
        applied is not null && PimaxFovea.UsesPimaxPlay(applied.Profile) && PimaxFovea.Resolve(applied.Profile) is { Pimax: { } pimax } resolved
            && !PimaxFovea.SameFovea(resolved.Profile, applied.Fovea ?? AppliedFovea.From(applied.Profile, null))
            ? $"Pimax Play changed: focus will be updated to {pimax.Play.Short} at launch." : null;
    /// <summary>A running DCS.exe; the offline verification host reports none.</summary>
    private bool DcsRunning()
    {
        if (offline) return false;
        var running = Process.GetProcessesByName("DCS");
        try { return running.Length > 0; } finally { foreach (var process in running) process.Dispose(); }
    }
    private readonly Lock _boostCacheLock = new();
    private (DateTime At, string Key, object Reply)? _boostCache;
    /// <summary>"What Boost will do" is read-only and slow (it inspects running processes and the DCS log), so it is
    /// answered outside the command lock and reused for 3 s for the same Boost settings and log.</summary>
    private Task<string>? BoostPlanRequest(string json)
    {
        if (json.Length > 128 * 1024 || !json.Contains("\"boostPlan\"", StringComparison.Ordinal)) return null;
        string? id = null;
        try
        {
            using var doc = JsonDocument.Parse(json);
            var root = doc.RootElement;
            if (root.GetProperty("action").GetString() != "boostPlan") return null;
            id = root.GetProperty("id").GetString();
            var data = root.GetProperty("data");
            var draftProfile = JsonData.Deserialize<VrProfile>(data.GetProperty("profile").GetRawText());
            var log = DcsLogPath(Empty(data.GetProperty("options").GetString()) ?? _options ?? _inventory.OptionsPath);
            var key = JsonData.Serialize(new { draftProfile.Desktop, draftProfile.CpuBoost, draftProfile.BoostDcsPriority, draftProfile.BoostMoveVrRuntime, draftProfile.BoostMoveBackgroundApps,
                draftProfile.BoostBackgroundApps, draftProfile.BoostElevated, draftProfile.Runtime, log,
                draftProfile.FreeVramApps, draftProfile.FlightDisplayWidth, draftProfile.FlightDisplayHeight, draftProfile.FlightDisplayRefresh });
            lock (_boostCacheLock)
                if (_boostCache is { } cached && cached.Key == key && DateTime.UtcNow - cached.At < TimeSpan.FromSeconds(3)) return Task.FromResult(Reply(id, cached.Reply));
            return Describe();
            async Task<string> Describe()
            {
                try
                {
                    var reply = new { plan = await Task.Run(() => BoostPlanner.Describe(draftProfile, log)), dcsLog = log };
                    lock (_boostCacheLock) _boostCache = (DateTime.UtcNow, key, reply);
                    return Reply(id, reply);
                }
                catch (Exception e) { return JsonData.Serialize(new { id, ok = false, error = e.Message }); }
            }
        }
        catch (Exception e) { return Task.FromResult(JsonData.Serialize(new { id, ok = false, error = e.Message })); }
    }
    private string? _previewFoveaStamp;
    private string? _dcs, _options, _sboys;
    private ApplyPlan? _plan;
    private ReadinessReport? _readiness;
    private Dictionary<string, string>? _sboysHashes;
    private string _status = "Ready. Launch DCS applies the profile shown here, then starts DCS.";
    private string _report = "Review files lists what Launch DCS will write.";
    public string StateRoot => service.StateRoot;
    public async Task<string> Handle(string json)
    {
        // Requests answered outside the command lock; every await sits inside the try, so nothing escapes to the window.
        try
        {
            if (BoostPlanRequest(json) is { } boostPlan) return await boostPlan;
            if (SaveDraftRequest(json) is { } savedDraft) return savedDraft;
            if (LastFlightRequest(json) is { } lastFlight) return await lastFlight;
        }
        catch (Exception e) { AppLog.Error("Request", e); return JsonData.Serialize(new { id = RequestId(json), ok = false, error = e.Message }); }
        // Read-only and cheap: polled by the page so Launch and Restore lock while DCS runs.
        if (json.Length < 512 && json.Contains("\"dcsStatus\"", StringComparison.Ordinal))
        {
            string? statusId = null;
            try
            {
                using var doc = JsonDocument.Parse(json);
                if (doc.RootElement.GetProperty("action").GetString() == "dcsStatus")
                { statusId = doc.RootElement.GetProperty("id").GetString(); return Reply(statusId, new { dcsRunning = DcsRunning() }); }
            }
            catch (Exception e) { return JsonData.Serialize(new { id = statusId, ok = false, error = e.Message }); }
        }
        await _commands.WaitAsync();
        string? id = null;
        try
        {
            if (json.Length > 128 * 1024) throw new InvalidDataException("The request is too large.");
            using var doc = JsonDocument.Parse(json);
            var root = doc.RootElement;
            id = root.GetProperty("id").GetString();
            var action = root.GetProperty("action").GetString();
            var data = root.GetProperty("data");
            if (action is "preview" or "apply" or "launch" or "resetNeural" or "save" or "export" or "refresh" or "checkReadiness" or "prepareSboys" or "importSboys" or "openSboys" or "detect" or "diagnosticPackage" or "grantWriteAccess") ReadDraft(data);
            switch (action)
            {
                case "ready":
                    if (!_shownApplied) { _shownApplied = true; ShowAppliedProfile(); }
                    await Capture(); break;
                case "changed": Invalidate(); return Reply(id, new { invalidated = true });
                case "detect":
                    EnsureInteractive();
                    // Proposes a profile from the running headset route and existing tool settings. Nothing is applied
                    // until the user accepts it and previews.
                    await Capture();
                    var detection = await Task.Run(() => SetupDetection.Detect(_profile, CurrentInventory, new DetectionSources { SavedRuntimes = service.SavedRuntime }));
                    _activeRoute = (DateTime.UtcNow, detection.Active);
                    return Reply(id, new { profile = detection.Profile, detected = detection.Detected, notes = detection.Notes, active = detection.Active.Summary });
                case "refresh": Invalidate(); await Capture(); _status = "PC rescanned."; break;
                case "pimaxFovea":
                    // Read-only: Pimax Play's Quad View values as they are on disk now.
                    await RefreshEyeResolution();
                    return Reply(id, new { pimax = PimaxView() });
                case "checkReadiness":
                    // The guided setup checks its candidate (replace=true): afterwards the page's own draft and paths
                    // come back, so a cancelled wizard leaves nothing behind for later commands.
                    var page = data.TryGetProperty("replace", out var replace) && replace.ValueKind == JsonValueKind.True ? (_profile, _dcs, _options) : ((VrProfile, string?, string?)?)null;
                    Invalidate(); await Capture();
                    _readiness = await Task.Run(() => Readiness.Check(_profile, CurrentInventory, service));
                    if (page is { } wizardBefore) { (_profile, _dcs, _options) = wizardBefore; _plan = null; } // the wizard reads this check's report
                    if (FoveaFact(_profile) is { } fact)
                        _readiness = _readiness with { Checks = [.. _readiness.Checks, new("pimax-fovea", fact.Severity == IssueSeverity.Warning ? CheckState.Warning : CheckState.Pass, "Focus area from Pimax Play", fact.Message)] };
                    _status = _readiness.CanPrepare ? "Automatic checks done. Confirm the items under Check yourself." : "Setup needs attention. Resolve the listed blockers, then check again.";
                    _report = JsonData.Serialize(_readiness); break;
                case "openGuide":
                    EnsureInteractive(); var guide = data.GetProperty("guide").GetString();
                    var url = guide switch {
                        "sboys" => "https://github.com/sboys3/CustomHeadsetOpenVR/releases/tag/1.3.0",
                        "gaze" => "https://github.com/mbucchia/Quad-Views-Foveated/wiki/Pimax-Crystal",
                        "pimax" => "https://pimax.com/pages/downloads",
                        "steamvr" => "https://store.steampowered.com/app/250820/SteamVR/",
                        "vcredist" => "https://learn.microsoft.com/en-us/cpp/windows/latest-supported-vc-redist",
                        "nvidia" => "https://www.nvidia.com/en-us/drivers/", _ => throw new InvalidDataException("Unknown setup guide.") };
                    using (var process = Process.Start(new ProcessStartInfo(url) { UseShellExecute = true })) { } break;
                case "preview":
                    // Review files: read-only. Lists every file a fresh apply of this draft writes; Launch DCS rebuilds
                    // and checks the plan itself, so nothing here has to be approved first.
                    Invalidate(); await Capture();
                    var fovea = PimaxFovea.Resolve(_profile);
                    _plan = await Task.Run(() => service.Preview(_profile, CurrentInventory, fovea));
                    _previewFoveaStamp = fovea.Pimax?.Stamp;
                    var current = service.ReadAppliedProfile();
                    _report = (fovea.Applies ? string.Join("\n", fovea.Notes.Prepend("Focus area from Pimax Play:")) + "\n\n" : "") + service.Describe(_plan);
                    _status = $"Review · {_plan.Files.Count} files. " + (current is null ? "Launch DCS backs up the originals, then writes them."
                        : ControlService.SameDraft(_profile, current.Profile) ? "This is the applied profile: Launch DCS writes nothing new" + (FoveaChanged(current) is null ? "." : " except Pimax Play's new focus values.")
                        : $"Launch DCS writes them over {current.Profile?.Name ?? current.Journal.ProfileId}; the original files stay backed up.")
                        + (fovea.Pimax is { } pimax ? " " + PimaxFovea.Summary(pimax) : fovea.Applies ? " " + fovea.Notes[0] : ""); break;
                case "grantWriteAccess":
                    EnsureInteractive(); await Capture();
                    _status = await EnsureWritable(LaunchSafety.ProfileFolders(_profile, CurrentInventory), askedByPilot: true)
                        ? "Write access granted: DCS Control can now write the profile's files." : "DCS Control can already write to every folder this profile uses.";
                    Invalidate(); break;
                case "apply":
                    // Apply without launching: the same steps as Launch DCS, with Pimax Play's values read now.
                    Invalidate(); await Capture();
                    var synced = await WithWriteAccess(() => service.SyncApplied(_profile, CurrentInventory), () => LaunchSafety.ProfileFolders(_profile, CurrentInventory));
                    _status = synced.Message + " Start DCS with Launch DCS so it receives the profile's OpenXR environment.";
                    _report = SyncReport(synced); break;
                case "restore":
                    // Back to stock DCS: every path DCS Control changed goes back to what was there before.
                    Invalidate();
                    var restored = await Task.Run(service.RestoreOriginals);
                    // A file left behind in a protected folder (DCS under Program Files): one Windows prompt, then the rest.
                    if (!restored.Complete && !offline)
                    {
                        var restoreFolders = await Task.Run(() => service.Originals.Read().Files.Select(f => Path.GetDirectoryName(f.Path)).OfType<string>().ToList());
                        if (await Task.Run(() => { LaunchSafety.ForgetWriteProbes(); return LaunchSafety.UnwritableFolders(restoreFolders); }) is { Count: > 0 }
                            && await EnsureWritable(restoreFolders, askedByPilot: false, throwWhenBlocked: false))
                            restored = await Task.Run(service.RestoreOriginals);
                    }
                    _status = restored.Complete ? "Back to stock DCS: every file and setting DCS Control changed is back to its original. DCS runs as if the app had never been used." : "Some original files could not be restored; they are still listed on the Stock DCS page.";
                    _report = restored.Complete ? "Every file DCS Control changed is back to its original; owned DCS settings in options.lua were set back, your other edits kept." : string.Join("\n", restored.Conflicts); break;
                case "engineStatus":
                    // Read-only: the DCS engine optimizations' own log, the newest test report and DCS's log.
                    var engineOptions = EngineOptionsPath();
                    return Reply(id, new { status = engineOptions is null ? null : await Task.Run(() => EngineOptimizations.Read(engineOptions, DcsLogPath(engineOptions), DcsRunning())) });
                case "dlssFocus":
                    // Read-only: why DLSS 5 did or did not reach the focus area in the last session (Cheeky's log).
                    var focusFallback = _dcs ?? _inventory.DcsExecutable;
                    return Reply(id, new { report = await Task.Run(() =>
                    {
                        var exe = service.ReadAppliedProfile()?.Executable ?? focusFallback;
                        return Diagnostics.DiagnoseDlssFocus(exe is null ? null : Path.GetDirectoryName(exe));
                    }) });
                case "diagnosticPackage":
                    EnsureInteractive();
                    var packageDialog = new SaveFileDialog { Filter = "Diagnostic package|*.zip", FileName = $"dcs-control-diagnostics-{DateTime.Now:yyyyMMdd-HHmm}.zip" };
                    if (packageDialog.ShowDialog(owner) != true) break;
                    await Capture(); _readiness = await Task.Run(() => Readiness.Check(_profile, CurrentInventory, service));
                    var packageApplied = await Task.Run(service.ReadAppliedProfile);
                    var packageDcs = packageApplied?.Executable ?? _dcs ?? CurrentInventory.DcsExecutable;
                    var packageOptions = packageApplied?.OptionsPath ?? _options ?? CurrentInventory.OptionsPath;
                    var packageSources = new DiagnosticSources(service.StateRoot, packageApplied is null ? null : Path.Combine(service.ManagedRoot, "profiles", packageApplied.Profile.Id),
                        packageOptions, packageDcs, _profile.Runtime, SteamVrSettings: HeadsetRefresh.SteamVrSettings);
                    var packageFocus = await Task.Run(() => Diagnostics.DiagnoseDlssFocus(packageDcs is null ? null : Path.GetDirectoryName(packageDcs)));
                    var packageRefresh = await Task.Run(() => HeadsetRefresh.Detect(_profile.Runtime));
                    var packageReport = JsonData.Serialize(new { schemaVersion = 1, version = ProductInfo.Version, created = DateTimeOffset.Now, dcsRunning = DcsRunning(),
                        draft = _profile, applied = packageApplied?.Profile, inventory = CurrentInventory, issues = ProfileValidation.Validate(_profile, CurrentInventory), readiness = _readiness,
                        lastFlight = await Task.Run(() => { try { return LastFlight.Read(FlightSourcesNow()); } catch (Exception e) when (e is IOException or UnauthorizedAccessException) { return null; } }),
                        dlssFocus = packageFocus, headsetRefresh = packageRefresh, originals = service.Originals.Status(), packages = PackageCatalog.All });
                    var packaged = await Task.Run(() => Diagnostics.CreatePackage(packageSources, packageReport, packageDialog.FileName));
                    _status = $"Diagnostic package saved: {Path.GetFileName(packageDialog.FileName)} ({packaged.Count} files). Attach it to your report.";
                    using (var process = Process.Start(new ProcessStartInfo("explorer.exe") { ArgumentList = { "/select,", packageDialog.FileName }, UseShellExecute = false })) { }
                    break;
                case "pupilStatus":
                    // Read-only: the pupil shift layer's log in the applied profile's folder.
                    var pupilApplied = await Task.Run(service.ReadAppliedProfile);
                    var pupilFolder = pupilApplied?.Profile.ForLaunch().UsesPupilShift == true ? PupilShiftStatus.Folder(service.ManagedRoot, pupilApplied.Profile.Id) : null;
                    return Reply(id, new { status = await Task.Run(() => PupilShiftStatus.Read(pupilFolder, DcsRunning())) });
                case "pupilOpenLog":
                    EnsureInteractive();
                    var pupilLog = (await Task.Run(service.ReadAppliedProfile)) is { } applied && applied.Profile.ForLaunch().UsesPupilShift
                        ? Path.Combine(PupilShiftStatus.Folder(service.ManagedRoot, applied.Profile.Id), PupilShiftStatus.LogName) : null;
                    if (pupilLog is null || !File.Exists(pupilLog)) throw new InvalidOperationException("No log yet: the layer writes it when DCS starts VR.");
                    using (var process = Process.Start(new ProcessStartInfo(pupilLog) { UseShellExecute = true })) { } break;
                case "engineSuite":
                    EnsureInteractive();
                    var suiteOptions = EngineOptionsPath() ?? throw new InvalidOperationException("Select DCS's options.lua first.");
                    return Reply(id, new { message = EngineOptimizations.StartSuite(suiteOptions, DcsRunning()) });
                case "engineOpen":
                    EnsureInteractive();
                    var openOptions = EngineOptionsPath() ?? throw new InvalidOperationException("Select DCS's options.lua first.");
                    var engineStatus = EngineOptimizations.Read(openOptions, DcsLogPath(openOptions), DcsRunning());
                    var engineTarget = data.GetProperty("target").GetString() switch
                    {
                        "report" => engineStatus.LastReport ?? throw new InvalidOperationException("No test report yet."),
                        "log" => engineStatus.LogPath ?? throw new InvalidOperationException("No log yet: DCS writes it when it loads the module."),
                        _ => throw new InvalidDataException("Unknown file.")
                    };
                    using (var process = Process.Start(new ProcessStartInfo(engineTarget) { UseShellExecute = true })) { } break;
                case "openBackups":
                    EnsureInteractive(); Directory.CreateDirectory(service.Originals.Directory);
                    using (var process = Process.Start(new ProcessStartInfo("explorer.exe") { ArgumentList = { service.Originals.Directory }, UseShellExecute = false })) { } break;
                case "launch":
                    // One click: bring the installed profile in line with this draft (nothing, Pimax Play's focus values
                    // in place, or restore + apply), verify the launch contract, then start DCS and CPU Boost.
                    Invalidate(); await Capture();
                    if (offline)
                    {
                        // The offline verification host never starts the game: everything up to the process start runs.
                        var (checkedSync, checkedStart) = await Task.Run(() => service.SyncAndPrepareLaunch(_profile, CurrentInventory));
                        _status = LaunchStatus(checkedSync, "Offline verification: launch contract verified, DCS was not started.");
                        _report = SyncReport(checkedSync) + "\nLaunch contract verified: " + checkedStart.FileName; break;
                    }
                    var (launched, dcsProcess) = await WithWriteAccess(() => service.SyncAndLaunch(_profile, CurrentInventory), () => LaunchSafety.ProfileFolders(_profile, CurrentInventory));
                    dcsProcess.Dispose();
                    _status = LaunchStatus(launched, "DCS launched."); _report = SyncReport(launched); break;
                case "launchCheck":
                    var start = await Task.Run(service.PrepareLaunchApplied); _report = "Launch contract verified: " + start.FileName; break;
                case "resetNeural": _profile = RenoSettings.ResetAdvanced(_profile); Invalidate(); _status = "Advanced DLSS 5 image controls reset to their defaults."; break;
                case "browse":
                    EnsureInteractive();
                    var target = data.GetProperty("target").GetString();
                    if (target is "quadViewsLayerDirectory")
                    {
                        var folder = new OpenFolderDialog { Title = "Select provider folder" };
                        return Reply(id, new { path = folder.ShowDialog(owner) == true ? folder.FolderName : null });
                    }
                    var file = new OpenFileDialog { Title = "Select component", Filter = target switch { "dcs" => "DCS executable|DCS.exe", "options" => "DCS settings|options.lua", "neuralRuntimePath" => "NVIDIA neural runtime|nvngx_dlssnr.dll", "engineDevPayloadPath" => "DcsQvCull payload|*.dll", "engineDevIniPath" => "DcsQvCull settings|*.ini", _ => "OpenXR manifest|*.json" } };
                    return Reply(id, new { path = file.ShowDialog(owner) == true ? file.FileName : null });
                case "rememberRuntime":
                    // The file picked with Browse (DLSS 5 page or guided setup): verified exactly like deployment and kept
                    // as the app's copy, so it is not asked for again. Nothing in the draft or the game changes here.
                    var remembered = await Task.Run(() => service.RememberNeuralRuntime(data.GetProperty("path").GetString() ?? throw new InvalidDataException("Select nvngx_dlssnr.dll.")));
                    Invalidate();
                    return Reply(id, new { savedRuntime = SavedRuntimeView(), retainedRuntime = RetainedRuntime(), message = "Saved a verified copy of nvngx_dlssnr.dll " + remembered.Version + " at " + remembered.Path + ". DLSS 5 uses it from now on." });
                case "forgetRuntime":
                    var kept = service.ForgetNeuralRuntime(); Invalidate();
                    return Reply(id, new { savedRuntime = SavedRuntimeView(), retainedRuntime = RetainedRuntime(), message = kept
                        ? "The saved runtime copy is no longer used for new drafts. The applied profile was installed from it, so the file stays until that profile is restored."
                        : "The saved runtime copy was deleted. Select nvngx_dlssnr.dll again to use DLSS 5." });
                case "save":
                    EnsureInteractive(); var save = new SaveFileDialog { Filter = "DCS Control profile|*.json", FileName = "dcs-control-profile.json" };
                    if (save.ShowDialog(owner) == true) { service.SaveProfile(_profile, save.FileName); _status = "Profile saved."; } break;
                case "import":
                    EnsureInteractive(); var import = new OpenFileDialog { Filter = "DCS Control profile|*.json" };
                    if (import.ShowDialog(owner) == true) { _profile = service.LoadProfile(import.FileName); Invalidate(); _status = "Profile imported."; } break;
                case "export":
                    EnsureInteractive(); var export = new SaveFileDialog { Filter = "Diagnostic report|*.json", FileName = "dcs-control-report.json" };
                    if (export.ShowDialog(owner) == true) { await Capture(); _readiness = await Task.Run(() => Readiness.Check(_profile, CurrentInventory, service));
                        AtomicFile.WriteText(export.FileName, JsonData.Serialize(new { schemaVersion = 1, version = ProductInfo.Version, inventory = CurrentInventory, profile = _profile, readiness = _readiness,
                            packages = PackageCatalog.All, originals = service.Originals.Status() })); _status = "Fresh diagnostic and readiness report exported."; } break;
                case "importSboys":
                    EnsureInteractive(); var sboysImport = new OpenFileDialog { Title = "Import official Sboys 1.3.0 ZIP", Filter = "Official Sboys archive|*.zip" };
                    if (sboysImport.ShowDialog(owner) != true) break;
                    await PackageAcquisition.Acquire(PackageCatalog.Get("sboys"), Path.Combine(service.StateRoot, "cache"), sboysImport.FileName);
                    goto case "prepareSboys";
                case "prepareSboys":
                    var sboysPackage = PackageCatalog.Get("sboys");
                    var bundled = Path.Combine(service.PackageRoot, sboysPackage.ArchiveName);
                    var cached = Path.Combine(service.StateRoot, "cache", sboysPackage.ArchiveName);
                    if (offline && !File.Exists(bundled) && !File.Exists(cached)) throw new InvalidOperationException("Network downloads are disabled during offline verification.");
                    var archive = await PackageAcquisition.Acquire(sboysPackage, Path.Combine(service.StateRoot, "cache"), File.Exists(bundled) ? bundled : null);
                    _sboys = await Task.Run(() => {
                        var folder = new ComponentCache(Path.Combine(service.StateRoot, "cache")).Import(sboysPackage, archive);
                        _sboysHashes = Directory.EnumerateFiles(folder, "*", SearchOption.AllDirectories).ToDictionary(p => p, Hashing.FileSha256, StringComparer.OrdinalIgnoreCase);
                        return Directory.EnumerateFiles(folder, "custom-headset-gui.exe", SearchOption.AllDirectories).Single();
                    });
                    _report = "Verified Sboys configuration tool:\n" + _sboys; _status = "Official Sboys package verified. Open its tool → About → Install driver, then configure tracking and refresh checks."; break;
                case "openSboys":
                    EnsureInteractive(); if (_sboys is null || !File.Exists(_sboys)) throw new InvalidOperationException("Prepare the Sboys package first.");
                    if (_sboysHashes is null || _sboysHashes.Any(f => !File.Exists(f.Key) || Hashing.FileSha256(f.Key) != f.Value))
                        throw new InvalidDataException("Prepared Sboys files changed. Prepare the official package again before opening it.");
                    using (var process = Process.Start(new ProcessStartInfo(_sboys) { UseShellExecute = true, WorkingDirectory = Path.GetDirectoryName(_sboys)! })) { } break;
                case "window":
                    // The page draws the title-bar buttons; the offline verification host has no window.
                    if (owner is not null)
                        switch (data.GetProperty("op").GetString())
                        {
                            case "minimize": owner.WindowState = WindowState.Minimized; break;
                            case "maximize": owner.WindowState = owner.WindowState == WindowState.Maximized ? WindowState.Normal : WindowState.Maximized; break;
                            case "close": owner.Close(); break;
                            default: throw new InvalidDataException("Unknown window command.");
                        }
                    return Reply(id, new { maximized = owner?.WindowState == WindowState.Maximized });
                default: throw new InvalidDataException("Unknown interface command.");
            }
            return Reply(id, State());
        }
        catch (Exception e) { return JsonData.Serialize(new { id, ok = false, error = e.Message }); }
        finally { _commands.Release(); }
    }
    /// <summary>The request's id, for an error reply; null when the message has none.</summary>
    internal static string? RequestId(string json)
    {
        try { using var doc = JsonDocument.Parse(json); return doc.RootElement.TryGetProperty("id", out var id) && id.ValueKind == JsonValueKind.String ? id.GetString() : null; }
        catch (JsonException) { return null; }
    }
    private InventorySnapshot CurrentInventory => _inventory with { DcsExecutable = _dcs, OptionsPath = _options };
    private void ReadDraft(JsonElement data)
    {
        _profile = JsonData.Deserialize<VrProfile>(data.GetProperty("profile").GetRawText());
        _dcs = Empty(data.GetProperty("dcs").GetString()); _options = Empty(data.GetProperty("options").GetString());
    }
    private static string? Empty(string? value) => string.IsNullOrWhiteSpace(value) ? null : value.Trim();
    private void Invalidate() { _plan = null; _readiness = null; }
    private string LaunchStatus(LaunchSync sync, string launched) => sync.Kind == LaunchSyncKind.Unchanged ? launched + " " + _profile.Name + "." : sync.Message + " " + launched;
    private static string SyncReport(LaunchSync sync) => sync.Message + "\nProfile: " + sync.Journal.ProfileId
        + (sync.ReplacedProfileId is { } replaced && replaced != sync.Journal.ProfileId && !sync.Message.Contains(" over " + replaced, StringComparison.Ordinal) ? "\nWritten over: " + replaced : "");
    private void EnsureInteractive() { if (offline) throw new InvalidOperationException("Interactive commands are disabled during offline verification."); }
    /// <summary>Runs a step that writes game files (Launch, Apply). Only when Windows actually refuses a write (a
    /// protected folder: DCS under Program Files) is it asked for the permission, once, and the step runs again; a
    /// launch that writes nothing never asks.</summary>
    private async Task<T> WithWriteAccess<T>(Func<T> step, Func<IReadOnlyList<string>> folders)
    {
        try { return await Task.Run(step); }
        catch (Exception e) when (!offline && (e is UnauthorizedAccessException || e.InnerException is UnauthorizedAccessException))
        {
            if (!await EnsureWritable(folders(), askedByPilot: false)) throw;
            return await Task.Run(step);
        }
    }

    /// <summary>Folders whose permission change was declined or did not help in this session: not asked again.</summary>
    private readonly HashSet<string> _grantRefused = new(StringComparer.OrdinalIgnoreCase);

    /// <summary>
    /// Makes <paramref name="folders"/> writable: one Windows prompt for the folders whose permissions keep this account
    /// out (local NTFS only). A folder blocked by something else (antivirus, Controlled folder access, a read-only or
    /// network drive) is never "fixed" with a prompt that cannot help: the pilot reads what blocks it instead. Returns
    /// whether permissions were changed.
    /// </summary>
    private async Task<bool> EnsureWritable(IReadOnlyList<string> folders, bool askedByPilot, bool throwWhenBlocked = true)
    {
        var blocked = await Task.Run(() => { LaunchSafety.ForgetWriteProbes(); return LaunchSafety.UnwritableFolders(folders); });
        if (blocked.Count == 0) return false;
        var grantable = await Task.Run(() => blocked.Where(f => LaunchSafety.PermissionsDenyWrite(f) && LaunchSafety.CanGrant(f) && (askedByPilot || !_grantRefused.Contains(f))).ToList());
        string Advice(IReadOnlyList<string> list) => string.Join(" ", list.Select(f => f + ": " + LaunchSafety.WriteAdvice(f)).Distinct());
        if (grantable.Count == 0)
        {
            if (!throwWhenBlocked) return false;
            throw new InvalidOperationException("DCS Control cannot write to " + string.Join("; ", blocked) + ". " + Advice(blocked));
        }
        _status = "Asking Windows for permission to write to " + string.Join(" and ", grantable) + "…";
        if (!await Task.Run(() => LaunchSafety.GrantWriteAccess(grantable)))
        {
            _grantRefused.UnionWith(grantable);
            if (!throwWhenBlocked) return false;
            throw new InvalidOperationException("DCS Control cannot write to " + string.Join("; ", grantable) + " and the Windows prompt was declined. " + LaunchSafety.WriteAccessAdvice);
        }
        if (await Task.Run(() => LaunchSafety.UnwritableFolders(folders)) is { Count: > 0 } still)
        {
            _grantRefused.UnionWith(still);
            if (!throwWhenBlocked) return true;
            throw new InvalidOperationException("DCS Control still cannot write to " + string.Join("; ", still) + ". " + Advice(still));
        }
        return true;
    }
    private async Task Capture()
    {
        _inventory = await Task.Run(() => capture is null ? new WindowsInventory().Capture(_dcs, _options) : capture(_dcs, _options));
        _dcs ??= _inventory.DcsExecutable; _options ??= _inventory.OptionsPath;
        await RefreshEyeResolution();
    }
    private (int Width, int Height)? _eyeResolution;
    /// <summary>The per-eye resolution from dcs.log, read off the UI thread (head of the file only, cached by length
    /// and time) whenever the inventory or Pimax Play's values are read again; replies reuse the last value.</summary>
    private async Task RefreshEyeResolution()
    {
        var log = DcsLogPath(_options ?? _inventory.OptionsPath);
        _eyeResolution = await Task.Run(() => PimaxFovea.ReadEyeResolution(log));
    }
    private (DateTime At, ActiveRouteState Value)? _activeRoute;
    /// <summary>Which route currently drives the headset, refreshed at most every five seconds.</summary>
    private ActiveRouteState ActiveRoute()
    {
        // The offline verification host chooses what is "running" (Readiness.ActiveRouteProvider); it never probes this PC.
        if (offline) return Readiness.ActiveRouteProvider();
        if (_activeRoute is { } cached && DateTime.UtcNow - cached.At < TimeSpan.FromSeconds(5)) return cached.Value;
        var value = SetupDetection.DetectActiveRoute(); _activeRoute = (DateTime.UtcNow, value); return value;
    }
    private object State()
    {
        var originals = service.Originals.Status();
        var applied = originals.State == "applied" ? service.ReadAppliedProfile() : null;
        var activeRoute = ActiveRoute();
        var issues = ProfileValidation.Validate(service.ResolveNeuralRuntime(_profile), CurrentInventory).ToList();
        if (!_profile.Desktop && activeRoute.Route is { } running && running != _profile.Runtime)
            issues.Insert(0, new("route-mismatch", IssueSeverity.Warning, activeRoute.Summary + " This profile uses " + (_profile.Runtime == RuntimeKind.Pimax ? "the Pimax runtime" : "Sboys through SteamVR") + ". Switch the route or use Detect my setup before applying."));
        if (FoveaFact(_profile) is { } fact) issues.Add(fact);
        var launcherBlocked = LaunchSafety.LauncherRestartBlocked;
        if (_profile.KeepDcsLauncher && launcherBlocked) issues.Insert(0, new("dcs-launcher", IssueSeverity.Error, LaunchSafety.LauncherBlockedMessage));
        else if (_profile.KeepDcsLauncher && LaunchSafety.LauncherRestartUnknown) issues.Insert(0, new("dcs-launcher", IssueSeverity.Warning, LaunchSafety.LauncherUnknownMessage));
        return new { version = ProductInfo.Version, profile = _profile, dcs = _dcs, options = _options, inventory = CurrentInventory, pimax = PimaxView(),
            activeRoute = new { route = activeRoute.Route?.ToString(), summary = activeRoute.Summary },
            issues, cadence = Enum.IsDefined(_profile.FpsLimit) ? FramePacing.Describe(HeadsetRefresh.Resolve(_profile), CurrentInventory) : null, headsetRefresh = HeadsetRefresh.Detect(_profile.Runtime), foreignDxgi = service.ForeignDxgi(_profile, CurrentInventory), dcsCustomIpd = CurrentInventory.DcsSettings.GetValueOrDefault("VR.custom_IPD_enable") == "true" ? CurrentInventory.DcsSettings.GetValueOrDefault("VR.custom_IPD")?.Trim('"') : null, gpu = new { nvidiaRtx = GpuInfo.Unknown || GpuInfo.HasNvidiaRtx, names = GpuInfo.Names }, status = _status, report = _report,
            plan = _plan is null ? null : new { foveaStamp = _previewFoveaStamp, files = _plan.Files.Select(f => new { f.Path, f.Purpose, bytes = f.Content.Length, replaces = f.ExpectedSha256 is not null, f.LuaChanges }), environment = _plan.LaunchEnvironment },
            // Recovery: one record of original files, restored with one action.
            originals = new { count = originals.Count, files = originals.Files, state = originals.State, lastAction = originals.LastAction, lastActionAt = originals.LastActionAt, folder = originals.Folder },
            launchReady = applied is not null, appliedProfile = applied?.Journal.ProfileId, appliedName = applied?.Profile?.Name ?? applied?.Journal.ProfileId,
            // The applied profile.json content, so the page labels a draft "Applied" only when it matches it.
            appliedDraft = applied?.Profile, appliedDcs = applied?.Executable, appliedOptions = applied?.OptionsPath, foveaChanged = FoveaChanged(applied), dcsRunning = DcsRunning(),
            savedRuntime = SavedRuntimeView(), retainedRuntime = RetainedRuntime(), sboysReady = _sboys is not null, launcherBlocked, launcherUnknown = LaunchSafety.LauncherRestartUnknown, dcsDefaultKeys = NeuralHotkeys.DcsDefaultsForCurrentLayout(), readiness = _readiness };
    }
    /// <summary>The app's saved DLSS 5 runtime copy, shown on the DLSS 5 page; null when there is none (or it was forgotten).</summary>
    private object? SavedRuntimeView() => service.SavedRuntime.Current() is { } s
        ? new { path = s.Path, version = s.Version, sha256 = s.Sha256, originalPath = s.OriginalPath, originalExists = File.Exists(s.OriginalPath), savedAt = s.SavedAt } : null;
    /// <summary>A forgotten copy kept because the applied profile was installed from it.</summary>
    private string? RetainedRuntime() => service.SavedRuntime.Read() is { Forgotten: true } s ? s.Path : null;
    private static string Reply(string? id, object data) => JsonData.Serialize(new { id, ok = true, data });
    /// <summary>Pimax Play's Quad View page as Pimax Play shows it, with the bundled Quad Views values it converts to.</summary>
    private object PimaxView()
    {
        if (PimaxFovea.Read() is not { } s) return new { found = false, path = PimaxFovea.SettingsPath, explanation = PimaxFovea.Explanation };
        // The per-eye resolution the headset runtime recommended in DCS's last session, for the focus size in pixels.
        var eye = _eyeResolution;
        var c = s.Converted;
        return new { found = true, path = s.Path, modified = s.Modified, mode = s.Play.Mode, controls = s.Play.Controls, @short = s.Play.Short, mismatch = s.Mismatch,
            capNote = PimaxFovea.CapNote(s), leftEye = s.LeftEye, rightEye = s.RightEye, fromQuickSliders = s.FromQuickSliders, gazeScale = s.GazeResolutionScale,
            peripheryScale = s.PeripheryResolutionScale, transitionMode = s.Play.TransitionMode, converted = c, stamp = s.Stamp, summary = PimaxFovea.Summary(s),
            notes = PimaxFovea.Notes(s).Skip(1), explanation = PimaxFovea.Explanation, sameLabelNote = PimaxFovea.SameLabelNote,
            eyeResolution = eye is { } e ? new { width = e.Width, height = e.Height,
                focusWidth = PimaxPlayUnits.FocusPixels(e.Width, c.Width, c.FocusScale), focusHeight = PimaxPlayUnits.FocusPixels(e.Height, c.Height, c.FocusScale),
                pimaxFocusWidth = PimaxPlayUnits.FocusPixels(e.Width, c.RequestedWidth, 1 + s.GazeResolutionScale * .5), pimaxFocusHeight = PimaxPlayUnits.FocusPixels(e.Height, c.RequestedHeight, 1 + s.GazeResolutionScale * .5) } : null };
    }
    /// <summary>The Pimax Play fact for checks and readiness, in Pimax Play's own units.</summary>
    private static ValidationIssue? FoveaFact(VrProfile profile)
    {
        if (PimaxFovea.UsesPimaxPlay(profile))
        {
            var resolved = PimaxFovea.Resolve(profile);
            return resolved.Pimax is { } pimax ? new("pimax-fovea", pimax.Mismatch is null ? IssueSeverity.Info : IssueSeverity.Warning, PimaxFovea.Summary(pimax) + " Bundled Quad Views renders the same focus size."
                    + (PimaxFovea.CapNote(pimax) is { } cap ? " " + cap : "") + (pimax.Mismatch is { } m ? " " + m : ""))
                : new("pimax-fovea", IssueSeverity.Warning, resolved.Notes[0]);
        }
        if (profile.QuadViews == QuadProvider.PimaxNative && PimaxFovea.Read() is { } native)
            return new("pimax-fovea", native.Mismatch is null ? IssueSeverity.Info : IssueSeverity.Warning, "Pimax native Quad Views applies Pimax Play's settings itself. " + PimaxFovea.Summary(native) + (native.Mismatch is { } m2 ? " " + m2 : ""));
        return null;
    }
    /// <summary>The options.lua the engine optimizations belong to: the applied profile's when one is applied (its files
    /// are what DCS runs with), otherwise the selected or detected one.</summary>
    private string? EngineOptionsPath() => service.ReadAppliedProfile()?.OptionsPath ?? _options ?? _inventory.OptionsPath;
    /// <summary>Saved Games\DCS\Logs\dcs.log next to the selected options.lua (Saved Games\DCS\Config\options.lua).</summary>
    private static string DcsLogPath(string? options) =>
        options is not null && Path.GetDirectoryName(Path.GetDirectoryName(Path.GetFullPath(options))) is { } profileRoot
            ? Path.Combine(profileRoot, "Logs", "dcs.log")
            : Path.Combine(SystemPaths.SavedGames(), "DCS", "Logs", "dcs.log");
}
