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
    string? Option(string name) { var i = Array.IndexOf(args, name); return i >= 0 && i + 1 < args.Length ? args[i + 1] : null; }
    // The release folder holds release-manifest.json next to files\, where this program lives.
    string ReleaseFolder() => File.Exists(Path.Combine(AppContext.BaseDirectory, "release-manifest.json")) ? AppContext.BaseDirectory : Path.GetDirectoryName(Path.TrimEndingDirectorySeparator(AppContext.BaseDirectory))!;
    var service = new ControlService(Option("--distribution") ?? AppContext.BaseDirectory, Option("--state") ?? ControlService.DefaultStateRoot);
    // Commands that read or write the record of original files refuse to run from a sandbox that redirects
    // %LOCALAPPDATA% (a second, private record would diverge from the app's); an explicit --state is the caller's choice.
    if (command is "readiness" or "preview" or "apply" or "status" or "restore" or "launch-check" or "launch" or "diagnostic"
        && Option("--state") is null && AppDataRedirection.Target() is { } redirected)
    { Console.Error.WriteLine(string.Format(System.Globalization.CultureInfo.InvariantCulture, AppDataRedirection.Message, redirected)); return 3; }
    switch (command)
    {
        case "app-verify":
            Console.WriteLine($"Verified {ApplicationInstaller.VerifySource(Option("--source") ?? ReleaseFolder()).Files.Count} release files."); break;
        case "app-install":
        {
            var journal = new ApplicationInstaller(Option("--destination") ?? ApplicationInstaller.DefaultDestination).Install(Option("--source") ?? ReleaseFolder());
            Console.WriteLine($"Installed {journal.Entries.Count} application files. Recovery transaction: {journal.Id}"); break;
        }
        case "app-uninstall":
        {
            // Incomplete (a file could not be restored): non-zero, so the installer keeps the shortcuts, the Apps entry and the journals.
            var result = new ApplicationInstaller(Option("--destination") ?? ApplicationInstaller.DefaultDestination).Uninstall();
            Console.WriteLine(JsonData.Serialize(result));
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
        // The original files DCS Control changed and the applied profile ("current").
        case "status": Console.WriteLine(JsonData.Serialize(service.Originals.Status())); break;
        case "restore":
        {
            var result = service.RestoreOriginals(); Console.WriteLine(JsonData.Serialize(result));
            return result.Complete ? 0 : 2;
        }
        case "launch-check": Console.WriteLine("Verified installed launch contract: " + service.PrepareLaunchApplied().FileName); break;
        case "diagnostic":
        {
            var inventory = new WindowsInventory().Capture(Option("--dcs"), Option("--options"));
            Console.WriteLine(service.ExportDiagnostic(inventory, new(), Option("--out") ?? "diagnostic.json")); break;
        }
        case "nvidia": { var (threaded, mfaa) = NvidiaSettings.Fresh(); Console.WriteLine(JsonData.Serialize(new { threadedOptimization = threaded, mfaa })); break; }
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
        // Runs in its own process so it outlives the app; started automatically when a boosted profile launches DCS.
        case "boost": return BoostRuntime.Run(args[1..], Console.Out, Console.Error);
        // Started elevated by the app (Start Tobii) when an ended helper left the desktop Tobii services stopped.
        case "tobii-resume": return BoostRuntime.ResumeLeftoverTobii(service.StateRoot, Console.Out, Console.Error);
        default:
            // An unknown command is an error (a typo must not look like success to a script); "help" is not.
            if (command != "help") Console.Error.WriteLine("Unknown command: " + command);
            Console.WriteLine("DCS Control " + ProductInfo.Version + "\nCommands: inventory, readiness, detect, presets, validate, preview, apply, status, restore, diagnostic, inspect-binary, launch-check, launch, boost, tobii-resume, nvidia, app-verify, app-install, app-uninstall\nOptions: --profile file.json --dcs DCS.exe --options options.lua --state folder --distribution folder --out report.json\npreview leaves DCS and registry unchanged. apply writes the profile over whatever is installed; the first time it writes a path it backs up the original. status lists the original files and the applied profile. restore puts every original back (owned options.lua settings only). launch first brings the applied profile up to date (Pimax Play focus values)."); if (command != "help") return 1; break;
    }
    return 0;
}
catch (Exception e) when (e is IOException or InvalidDataException or UnauthorizedAccessException or ArgumentException or InvalidOperationException or System.Text.Json.JsonException or System.ComponentModel.Win32Exception or NotSupportedException or FormatException)
{ Console.Error.WriteLine(e.Message); return 1; }
