using DcsVr.Core;

// TEST ONLY: set by the test suite's CLI parity test for its child process alone, never by the app, the installer or a
// user. The child then sees what the test runner sees: no DCS running and the fixture GPU (an RTX 4090), not the PC's.
if (Environment.GetEnvironmentVariable("DCSVR_TEST_NO_DCS") == "1")
{
    DcsProcess.Provider = () => [];
    GpuInfo.Adapters = () => [new("NVIDIA GeForce RTX 4090", "NVIDIA", "32.0.15.0")];
}
try
{
    var command = args.FirstOrDefault() ?? "help";
    // A help flag anywhere only prints the commands: "launch --help" must never start DCS. The boost helper's own
    // arguments are its business.
    if (command != "boost" && args.Any(a => a is "--help" or "-h" or "/?" or "-?")) command = "help";
    // SOFA -> .dcshrtf (the app's Import SOFA and scripts/fetch-hrtf.ps1): needs no state folder or DCS, so it runs
    // before either is looked at. Its exit codes and output lines are a contract (DcsVr.Core.Hrtf.SofaConversion).
    if (command == "hrtf-convert") return DcsVr.Core.Hrtf.SofaConversion.RunCli(args[1..], Console.Out, Console.Error);
    string? Option(string name) { var i = Array.IndexOf(args, name); return i >= 0 && i + 1 < args.Length ? args[i + 1] : null; }
    // The release folder holds release-manifest.json next to files\, where this program lives.
    string ReleaseFolder() => File.Exists(Path.Combine(AppContext.BaseDirectory, "release-manifest.json")) ? AppContext.BaseDirectory : Path.GetDirectoryName(Path.TrimEndingDirectorySeparator(AppContext.BaseDirectory))!;
    var service = new ControlService(Option("--distribution") ?? AppContext.BaseDirectory, Option("--state") ?? ControlService.DefaultStateRoot);
    // Commands that read or write the record of original files refuse to run from a sandbox that redirects
    // %LOCALAPPDATA% (a second, private record would diverge from the app's); an explicit --state is the caller's choice.
    if (command is "readiness" or "preview" or "apply" or "live-apply" or "status" or "restore" or "launch-check" or "launch" or "diagnostic" or "audio-test"
        && Option("--state") is null && AppDataRedirection.Target() is { } redirected)
    { Console.Error.WriteLine(string.Format(System.Globalization.CultureInfo.InvariantCulture, AppDataRedirection.Message, redirected)); return 3; }
    switch (command)
    {
        case "app-verify":
            Console.WriteLine($"Verified {ApplicationInstaller.VerifySource(Option("--source") ?? ReleaseFolder()).Files.Count} release files."); break;
        case "app-install":
        {
            // The pilot's own installation (not a test one in a scratch folder, not from a sandbox with a private
            // %LOCALAPPDATA%): its settings are copied as the replaced version left them, for Go back.
            var installer = new ApplicationInstaller(Option("--destination") ?? ApplicationInstaller.DefaultDestination);
            var mayUsePilotState = !args.Contains("--no-registered-state") && AppDataRedirection.Target() is null;
            var installState = Option("--state") ?? (mayUsePilotState && installer.IsRegistered() ? ControlService.DefaultStateRoot : null);
            var journal = installer.Install(Option("--source") ?? ReleaseFolder(), stateRoot: installState,
                movedStateRoot: mayUsePilotState ? former => ApplicationInstaller.IsRegistered(former) ? ControlService.DefaultStateRoot : null : null);
            Console.WriteLine($"Installed {journal.Entries.Count} application files. Recovery transaction: {journal.Id}"); break;
        }
        case "app-uninstall":
        {
            // Incomplete (a file could not be restored): non-zero, so the installer keeps the shortcuts, the Apps entry and the journals.
            var result = new ApplicationInstaller(Option("--destination") ?? ApplicationInstaller.DefaultDestination).Uninstall(out var previousRoot);
            Console.WriteLine(args.Contains("--details")
                ? JsonData.Serialize(new { result.Complete, result.Conflicts, PreviousRoot = result.Complete ? previousRoot : null })
                : JsonData.Serialize(result));
            return result.Complete ? 0 : 2;
        }
        case "inventory":
            Console.WriteLine(JsonData.Serialize(new WindowsInventory().Capture(Option("--dcs"), Option("--options")))); break;
        case "readiness":
        {
            var profile = Option("--profile") is { } path ? service.LoadProfile(path) : new VrProfile();
            var report = Readiness.Check(profile, new WindowsInventory().Capture(Option("--dcs"), Option("--options")), service);
            var json = JsonData.Serialize(report); if (Option("--out") is { } output) AtomicFile.WriteText(output, json);
            Console.WriteLine(json); return report.CanPrepare ? 0 : 2;
        }
        case "presets": Console.WriteLine(JsonData.Serialize(ProfilePresets.All)); break;
        case "detect":
        {
            // Read-only: proposes a profile from the running headset route and existing Pimax/SteamVR settings.
            var start = Option("--profile") is { } source ? service.LoadProfile(source) : ProfilePresets.All.Single(p => p.Id == "pimax-combined");
            var result = SetupDetection.Detect(start, new WindowsInventory().Capture(Option("--dcs"), Option("--options")));
            var output = JsonData.Serialize(new { result.Active, result.Detected, result.Notes, result.Profile });
            if (Option("--out") is { } path) AtomicFile.WriteText(path, JsonData.Serialize(result.Profile));
            Console.WriteLine(output); break;
        }
        case "validate":
        {
            // Settled like the interface and LoadProfile, so the same file gives the same result from every entry point.
            var profile = ProfileValidation.ResolveFeatures(JsonData.Deserialize<VrProfile>(File.ReadAllText(Option("--profile") ?? throw new ArgumentException("--profile required"))));
            var issues = ProfileValidation.Validate(service.ResolveNeuralRuntime(profile)); Console.WriteLine(JsonData.Serialize(issues));
            return issues.Any(i => i.Severity == IssueSeverity.Error) ? 2 : 0;
        }
        case "preview":
        case "apply":
        {
            var p = ProfileValidation.ResolveFeatures(JsonData.Deserialize<VrProfile>(File.ReadAllText(Option("--profile") ?? throw new ArgumentException("--profile required"))));
            var inventory = new WindowsInventory().Capture(Option("--dcs"), Option("--options"));
            var plan = service.Preview(p, inventory);
            Console.WriteLine(service.Describe(plan));
            if (command == "apply") Console.WriteLine(JsonData.Serialize(service.Apply(plan)));
            break;
        }
        // Live mode, for scripted live tests: writes now (also while DCS runs) only the settings its components re-read in
        // flight; the rest waits for the next launch. JSON with the settings applied, those waiting and the files written.
        case "live-apply":
        {
            var p = ProfileValidation.ResolveFeatures(JsonData.Deserialize<VrProfile>(File.ReadAllText(Option("--profile") ?? throw new ArgumentException("--profile required"))));
            var applied = service.ReadAppliedProfile();
            var inventory = new WindowsInventory().Capture(Option("--dcs") ?? applied?.Executable, Option("--options") ?? applied?.OptionsPath);
            var result = service.ApplyLive(p, inventory);
            // Cheeky saves its INI itself: read again in a moment (written once more if its save dropped a value).
            var rechecks = service.RecheckLiveAsync(result.Rechecks).GetAwaiter().GetResult();
            Console.WriteLine(JsonData.Serialize(new { result.Applied, result.Waiting, result.Written, result.Message, Rechecks = rechecks.Select(o => o.ToString()).ToArray() }));
            break;
        }
        // The original files DCS Control changed and the applied profile ("current").
        case "status":
        {
            var status = service.Originals.Status();
            if (!args.Contains("--leftovers")) { Console.WriteLine(JsonData.Serialize(status)); break; }
            // --leftovers (the setup's uninstaller): also how many DCS Control files are in DCS without a record (a lost
            // or damaged record), searched like restore searches them, so Back to stock DCS is offered for them too.
            string? dcs = Option("--dcs"), options = Option("--options");
            if (dcs is null && options is null && Option("--state") is null)
                try { var found = new WindowsInventory().Capture(); (dcs, options) = (found.DcsExecutable, found.OptionsPath); }
                catch (Exception e) when (e is IOException or UnauthorizedAccessException or InvalidDataException) { }
            var node = System.Text.Json.JsonSerializer.SerializeToNode(status, JsonData.Options)!.AsObject();
            node["leftovers"] = service.LeftoverFiles(dcs, options).Count;
            Console.WriteLine(node.ToJsonString(JsonData.Options)); break;
        }
        case "restore":
        {
            // The uninstaller's Back to stock DCS also removes DCS Control's files the record does not list: in the DCS
            // given with --dcs/--options or, on the app's own state folder only, the DCS found on this PC. A test's
            // --state never reaches the pilot's real install.
            string? dcs = Option("--dcs"), options = Option("--options");
            if (dcs is null && options is null && Option("--state") is null)
                try { var found = new WindowsInventory().Capture(); (dcs, options) = (found.DcsExecutable, found.OptionsPath); }
                catch (Exception e) when (e is IOException or UnauthorizedAccessException or InvalidDataException) { }
            var result = service.RestoreOriginals(dcs, options); Console.WriteLine(JsonData.Serialize(result));
            return result.Complete ? 0 : 2;
        }
        case "clear-shader-cache":
        {
            // The engine module's compiled shader variants (state folder\engine-shader-cache\<Saved Games variant>), which
            // Back to stock DCS keeps on purpose; every variant's. Refused while DCS runs: the module is reading and writing them.
            ControlService.EnsureDcsClosed();
            var (files, bytes) = EngineShaderCache.ClearAll(service.StateRoot);
            Console.WriteLine(string.Format(System.Globalization.CultureInfo.InvariantCulture, "Deleted {0} compiled shader variants ({1:0.0} MB) in {2}. The engine module compiles them again in the first minutes of the next mission.", files, bytes / 1048576.0, service.ShaderCacheRoot));
            break;
        }
        case "launch-check": Console.WriteLine("Verified installed launch contract: " + service.PrepareLaunchApplied().FileName); break;
        case "diagnostic":
        {
            var inventory = new WindowsInventory().Capture(Option("--dcs"), Option("--options"));
            Console.WriteLine(service.ExportDiagnostic(inventory, new(), Option("--out") ?? "diagnostic.json")); break;
        }
        case "nvidia": { var read = NvidiaSettings.Fresh(); Console.WriteLine(JsonData.Serialize(new { threadedOptimization = read.Threaded, mfaa = read.Mfaa, dlssPreset = read.DlssPreset })); break; }
        case "inspect-binary": Console.WriteLine(JsonData.Serialize(NativeBinary.Inspect(Option("--file") ?? throw new ArgumentException("--file required")))); break;
        case "launch":
        {
            // Same as the app's Launch DCS for the applied profile: Pimax Play's focus values are brought up to date
            // (or the profile is applied again when its installed files changed), then DCS starts.
            var applied = service.ReadAppliedProfile();
            var inventory = new WindowsInventory().Capture(Option("--dcs") ?? applied?.Executable, Option("--options") ?? applied?.OptionsPath);
            var (sync, process) = service.SyncAndLaunch(null, inventory);
            using (process) Console.WriteLine(sync.Message + " DCS launched.");
            break;
        }
        // The cockpit space listening test (tools/listening-test): [AudioTest] in the applied DcsQvCull.ini, Part A's
        // sounds, Part B's mission and watcher, the analysis.
        case "audio-test": return CockpitSpaceTestCli.Run(args[1..], () => service, Console.Out, Console.Error);
        // Runs in its own process so it outlives the app; started automatically when a boosted profile launches DCS.
        case "boost": return BoostRuntime.Run(args[1..], Console.Out, Console.Error);
        // Started elevated by the app (Start Tobii) when an ended helper left the desktop Tobii services stopped.
        case "tobii-resume": return BoostRuntime.ResumeLeftoverTobii(service.StateRoot, Console.Out, Console.Error);
        // Started elevated by the app (End DCS) for a DCS with administrator rights it may not end itself (GitHub #21).
        case DcsProcess.EndCommand: return DcsProcess.EndFromCommand(Console.Out);
        default:
            // An unknown command is an error (a typo must not look like success to a script); "help" is not.
            if (command != "help") Console.Error.WriteLine("Unknown command: " + command);
            Console.WriteLine("DCS Control " + ProductInfo.Version + "\nCommands: inventory, readiness, detect, presets, validate, preview, apply, live-apply, status, restore, diagnostic, inspect-binary, launch-check, launch, boost, tobii-resume, end-dcs, nvidia, clear-shader-cache, app-verify, app-install, app-uninstall, hrtf-convert, audio-test\nOptions: --profile file.json --dcs DCS.exe --options options.lua --state folder --distribution folder --out report.json\npreview leaves DCS and registry unchanged. apply writes the profile over whatever is installed; the first time it writes a path it backs up the original. live-apply writes now, also while DCS runs, only the settings DCS Control applies live (the engine module's, pupil shift layer's, Quad Views', OFXR's and Cheeky's files); the rest waits for the next launch. status lists the original files and the applied profile (with --leftovers also the count of DCS Control files in DCS the record does not list). restore puts every original back (owned options.lua settings only). launch first brings the applied profile up to date (Pimax Play focus values). hrtf-convert <in.sofa> <out.dcshrtf> [--name N] [--source S] [--generic] converts a SOFA HRTF file for 3D audio. audio-test runs the cockpit space listening test (audio-test help)."); if (command != "help") return 1; break;
    }
    return 0;
}
catch (Exception e) when (e is IOException or InvalidDataException or UnauthorizedAccessException or ArgumentException or InvalidOperationException or System.Text.Json.JsonException or System.ComponentModel.Win32Exception or NotSupportedException or FormatException)
{ Console.Error.WriteLine(e.Message); return 1; }
