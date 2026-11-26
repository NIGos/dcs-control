using System.Diagnostics;
using System.Globalization;
using System.Security.Cryptography;
using System.Text;

namespace DcsVr.Core;

/// <summary>
/// DcsVr.Cli audio-test: the cockpit space listening test (tools/listening-test/README.md).
///   audio-test start [--seed N]            [AudioTest] on, the pilot's own level until a trial sets one
///   audio-test level 0..3                  the level for the next sounds (keeps the seed)
///   audio-test stop                        [AudioTest] removed: the pilot's own level again
///   audio-test watch --folder F [--log L]  Part B: follows the test mission's lines in dcs.log and sets each trial's
///                                          level; stops at the mission's "End the test" or Ctrl+C (removes [AudioTest])
///   audio-test stimuli --folder F [--tool binaural_room_render.exe] [--hrtf file] [--dry-run]   Part A's sounds
///   audio-test mission --source in.miz --out out.miz                                         Part B's mission
///   audio-test analyze --folder F          the report (summary.md, summary.json)
/// A results folder holds one session: seed.txt (shared by both parts), Part A's trials.json, key.json, stimuli\ and
/// answers.csv, Part B's part-b.log. The level of a trial is never printed while the test runs.
/// </summary>
public static class CockpitSpaceTestCli
{
    public static int Run(string[] args, Func<ControlService> service, TextWriter output, TextWriter error, CancellationToken cancel = default)
    {
        string? Option(string name) { var i = Array.IndexOf(args, name); return i >= 0 && i + 1 < args.Length ? args[i + 1] : null; }
        var verb = args.FirstOrDefault() ?? "help";
        try
        {
            switch (verb)
            {
                case "start":
                {
                    var seed = Option("--seed") is { } s ? uint.Parse(s, CultureInfo.InvariantCulture) : NewSeed();
                    output.WriteLine(service().SetAudioTest(new(seed, CockpitSpaceTest.CockpitTrials + CockpitSpaceTest.AbxTrials)));
                    return 0;
                }
                case "level":
                {
                    if (args.Length < 2 || !int.TryParse(args[1], NumberStyles.None, CultureInfo.InvariantCulture, out var level) || level is < Binaural.CockpitSpaceMin or > Binaural.CockpitSpaceMax)
                    { error.WriteLine("audio-test level takes 0 (Off) to 3 (Strong)."); return 1; }
                    var current = service().ReadAudioTest() ?? throw new InvalidOperationException("No listening test is on. Run audio-test start first.");
                    output.WriteLine(service().SetAudioTest(current with { Level = level }));
                    return 0;
                }
                case "stop": output.WriteLine(service().SetAudioTest(null)); return 0;
                case "watch":
                {
                    var folder = Folder(Option("--folder"));
                    var seed = Seed(folder, create: true);
                    var svc = service();
                    var log = Option("--log") ?? DcsLog(svc) ?? throw new InvalidOperationException("DCS's log was not found. Give it with --log <Saved Games>\\DCS\\Logs\\dcs.log.");
                    var trials = CockpitSpaceTest.CockpitTrials + CockpitSpaceTest.AbxTrials;
                    using var stop = CancellationTokenSource.CreateLinkedTokenSource(cancel);
                    ConsoleCancelEventHandler onCancel = (_, e) => { e.Cancel = true; stop.Cancel(); };
                    Console.CancelKeyPress += onCancel;
                    try
                    {
                        output.WriteLine(svc.SetAudioTest(new(seed, trials)));
                        output.WriteLine("Fly the test mission. F10 radio menu, Other, Cockpit space test. Ctrl+C here ends the test.");
                        Watch(log, seed, level => svc.SetAudioTest(new(seed, trials, level)), Path.Combine(folder, "part-b.log"), output, stop.Token, TimeSpan.FromMilliseconds(250));
                    }
                    finally
                    {
                        Console.CancelKeyPress -= onCancel;
                        output.WriteLine(svc.SetAudioTest(null));
                    }
                    output.WriteLine("Part B done. Run analyze.ps1 (or audio-test analyze --folder ...) for the results.");
                    return 0;
                }
                case "stimuli":
                {
                    var folder = Folder(Option("--folder"));
                    var seed = Seed(folder, create: true);
                    var dryRun = args.Contains("--dry-run");
                    var lines = Stimuli(folder, seed, Option("--tool"), Option("--hrtf"), dryRun, output);
                    if (dryRun) foreach (var l in lines) output.WriteLine(l);
                    return 0;
                }
                case "mission":
                {
                    var source = Option("--source") ?? throw new ArgumentException("--source <mission.miz> required");
                    var target = Option("--out") ?? throw new ArgumentException("--out <test mission.miz> required");
                    if (Path.GetFullPath(source).Equals(Path.GetFullPath(target), StringComparison.OrdinalIgnoreCase)) throw new ArgumentException("--out must be a new file, not the source mission.");
                    var bytes = CockpitSpaceTest.BuildMission(File.ReadAllBytes(source), NewSeed());
                    Directory.CreateDirectory(Path.GetDirectoryName(Path.GetFullPath(target))!);
                    AtomicFile.Write(target, bytes);
                    output.WriteLine("Test mission written: " + Path.GetFileName(target) + ". Copy it to your Missions folder yourself, or open it from here in DCS.");
                    return 0;
                }
                case "analyze":
                {
                    var folder = Folder(Option("--folder"), create: false);
                    var seed = Seed(folder, create: false);
                    var answers = Path.Combine(folder, "answers.csv");
                    var partB = Path.Combine(folder, "part-b.log");
                    var analysis = CockpitSpaceTest.Analyze(seed, File.Exists(answers) ? CockpitSpaceTest.ReadAnswers(File.ReadAllLines(answers)) : null, File.Exists(partB) ? File.ReadAllLines(partB) : null);
                    var report = CockpitSpaceTest.Report(analysis);
                    AtomicFile.WriteText(Path.Combine(folder, "summary.md"), report);
                    AtomicFile.WriteText(Path.Combine(folder, "summary.json"), JsonData.Serialize(analysis));
                    output.Write(report);
                    return 0;
                }
                default:
                    if (verb != "help") error.WriteLine("Unknown audio-test command: " + verb);
                    output.WriteLine("audio-test start [--seed N] | level 0..3 | stop | watch --folder F [--log dcs.log] | stimuli --folder F [--tool exe] [--hrtf file] [--dry-run] | mission --source in.miz --out out.miz | analyze --folder F");
                    return verb == "help" ? 0 : 1;
            }
        }
        catch (Exception e) when (e is IOException or InvalidDataException or UnauthorizedAccessException or ArgumentException or InvalidOperationException or FormatException or OverflowException)
        { error.WriteLine(e.Message); return 1; }
    }

    private static uint NewSeed() => BitConverter.ToUInt32(RandomNumberGenerator.GetBytes(4));

    private static string Folder(string? folder, bool create = true)
    {
        if (string.IsNullOrWhiteSpace(folder)) throw new ArgumentException("--folder <results folder> required");
        var full = Path.GetFullPath(folder);
        if (create) Directory.CreateDirectory(full);
        else if (!Directory.Exists(full)) throw new IOException("No results folder " + full + ".");
        return full;
    }

    /// <summary>The session's seed (seed.txt), made once.</summary>
    public static uint Seed(string folder, bool create)
    {
        var file = Path.Combine(folder, "seed.txt");
        if (File.Exists(file) && uint.TryParse(File.ReadAllText(file).Trim(), NumberStyles.None, CultureInfo.InvariantCulture, out var seed)) return seed;
        if (!create) throw new IOException("No seed.txt in " + folder + ": not a listening test folder.");
        seed = NewSeed();
        AtomicFile.WriteText(file, seed.ToString(CultureInfo.InvariantCulture) + "\n");
        return seed;
    }

    /// <summary>dcs.log next to the applied profile's options.lua (&lt;Saved Games&gt;\DCS\Config\options.lua → ..\Logs\dcs.log).</summary>
    public static string? DcsLog(ControlService service) =>
        service.ReadAppliedProfile()?.OptionsPath is { } options && Path.GetDirectoryName(Path.GetDirectoryName(options)) is { } dcs ? Path.Combine(dcs, "Logs", "dcs.log") : null;

    /// <summary>
    /// Part A's sounds: seed.txt, trials.json (what the runner plays, ids only), key.json (the unblinding), dry\*.wav and,
    /// unless a dry run, stimuli\&lt;id&gt;.wav rendered by the render tool (render\&lt;id&gt;\render.wav copied). Returns the
    /// tool's command lines.
    /// </summary>
    public static IReadOnlyList<string> Stimuli(string folder, uint seed, string? tool, string? hrtf, bool dryRun, TextWriter output)
    {
        var (trials, key) = CockpitSpaceTest.PartAPlan(seed);
        AtomicFile.WriteText(Path.Combine(folder, "trials.json"), JsonData.Serialize(trials));
        AtomicFile.WriteText(Path.Combine(folder, "key.json"), JsonData.Serialize(key));
        var dry = Path.Combine(folder, "dry"); Directory.CreateDirectory(dry);
        foreach (var signal in key.Stimuli.Select(s => s.Signal).Distinct())
            AtomicFile.Write(Path.Combine(dry, signal + ".wav"), CockpitSpaceTest.Wav(CockpitSpaceTest.DrySignal(signal, seed)));
        var exe = tool ?? "binaural_room_render.exe";
        var lines = new List<string>();
        if (!dryRun && (tool is null || !File.Exists(tool))) throw new IOException("The render tool was not found" + (tool is null ? "" : ": " + tool) + ". Give it with --tool (native/binaural build output, binaural_room_render.exe).");
        var stimuli = Path.Combine(folder, "stimuli"); if (!dryRun) Directory.CreateDirectory(stimuli);
        var done = 0;
        foreach (var s in key.Stimuli)
        {
            var outDir = Path.Combine(folder, "render", s.Id);
            var arguments = CockpitSpaceTest.RenderArguments(s, Path.Combine(dry, s.Signal + ".wav"), outDir, hrtf);
            lines.Add(Quote(exe) + " " + string.Join(" ", arguments.Select(Quote)));
            if (dryRun) continue;
            Directory.CreateDirectory(outDir);
            var start = new ProcessStartInfo(exe) { UseShellExecute = false, RedirectStandardOutput = true, RedirectStandardError = true, CreateNoWindow = true };
            foreach (var a in arguments) start.ArgumentList.Add(a);
            using var process = Process.Start(start) ?? throw new IOException("The render tool did not start.");
            try { process.PriorityClass = ProcessPriorityClass.BelowNormal; } catch (Exception e) when (e is InvalidOperationException or System.ComponentModel.Win32Exception) { }
            var stdout = process.StandardOutput.ReadToEndAsync(); var stderr = process.StandardError.ReadToEnd();
            process.WaitForExit(); _ = stdout.Result;
            var rendered = Path.Combine(outDir, "render.wav");
            if (process.ExitCode != 0 || !File.Exists(rendered)) throw new IOException($"The render tool failed for {s.Id} (exit {process.ExitCode}): {stderr.Trim()}");
            File.Copy(rendered, Path.Combine(stimuli, s.Id + ".wav"), overwrite: true);
            output.WriteLine($"Rendered {++done} of {key.Stimuli.Count}.");
        }
        return lines;
        static string Quote(string a) => a.Contains(' ', StringComparison.Ordinal) || a.Length == 0 ? "\"" + a + "\"" : a;
    }

    /// <summary>
    /// Part B's watcher: follows <paramref name="logPath"/> (dcs.log, which DCS writes while it runs and starts anew at
    /// each start) from its end, copies every test mission line to <paramref name="copyPath"/>, and calls
    /// <paramref name="setLevel"/> with the level of each trial event (<see cref="CockpitSpaceTest.LevelFor"/>). Ends at
    /// the mission's "end" line or on <paramref name="cancel"/>. Prints only "ready" lines, never a level.
    /// </summary>
    public static void Watch(string logPath, uint seed, Action<int?> setLevel, string copyPath, TextWriter output, CancellationToken cancel, TimeSpan poll)
    {
        long position = File.Exists(logPath) ? new FileInfo(logPath).Length : 0;
        var pending = new StringBuilder();
        while (!cancel.IsCancellationRequested)
        {
            try
            {
                if (File.Exists(logPath))
                {
                    using var stream = new FileStream(logPath, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete);
                    if (stream.Length < position) { position = 0; pending.Clear(); }
                    if (stream.Length > position)
                    {
                        stream.Position = position;
                        var buffer = new byte[stream.Length - position];
                        var read = stream.Read(buffer, 0, buffer.Length);
                        position += read;
                        pending.Append(Encoding.UTF8.GetString(buffer, 0, read));
                        var text = pending.ToString(); var cut = text.LastIndexOf('\n');
                        if (cut >= 0)
                        {
                            pending.Clear().Append(text[(cut + 1)..]);
                            foreach (var line in text[..cut].Split('\n'))
                            {
                                if (CockpitSpaceTest.ParseLine(line) is not { } e) continue;
                                File.AppendAllText(copyPath, line.TrimEnd('\r') + "\n");
                                if (CockpitSpaceTest.LevelFor(e, seed) is { } level) { setLevel(level); output.WriteLine($"{Describe(e)}: set."); }
                                else if (e.Kind == "start") output.WriteLine("Test mission ready.");
                                else if (e.Kind == "end") { output.WriteLine("The mission ended the test."); return; }
                                else output.WriteLine($"{Describe(e)}: noted.");
                            }
                        }
                    }
                }
            }
            catch (IOException) { }
            cancel.WaitHandle.WaitOne(poll);
        }
        static string Describe(CockpitSpaceTest.MissionEvent e) => e.Kind switch
        {
            "trial" => "Cockpit trial " + e.Number, "outside" => "Outside sound " + e.Number, "abx" => $"A/B/X trial {e.Number} {e.Text}",
            "rate" => "Rating for trial " + e.Number, "heard" => "Answer for outside sound " + e.Number, "abxtrial" => "A/B/X trial " + e.Number, "abxanswer" => "A/B/X answer " + e.Number,
            _ => e.Kind,
        };
    }
}
