using System.Globalization;
using System.IO.Compression;
using System.Text;
using System.Text.RegularExpressions;

namespace DcsVr.Core;

/// <summary>[AudioTest] in DcsQvCull.ini while the pilot's listening test runs (docs/COCKPIT_SPACE.md section 11).
/// <paramref name="Level"/>: the cockpit space level the module plays instead of [Audio] CockpitSpace (0..3); null: the
/// pilot's own level (between trials).</summary>
public sealed record AudioTestSettings(uint Seed, int Trials, int? Level = null);

/// <summary>
/// The cockpit space listening test (docs/COCKPIT_SPACE.md section 11, tools/listening-test): the [AudioTest] section,
/// the blind orders from a seed, Part A's stimuli plan and dry signals, the DCS test mission (Part B: sounds and answers
/// through DCS's own F10 radio menu, no key of ours), the mission's dcs.log lines and the analysis. No DCS file is
/// written here: the mission is a copy, made next to the results.
/// </summary>
public static partial class CockpitSpaceTest
{
    public const string Section = "AudioTest";
    /// <summary>The mission's dcs.log lines carry this word (env.info).</summary>
    public const string LogTag = "CSPACE";
    /// <summary>Part B: rating trials (blocks of Off, Subtle, Normal), outside sounds (blocks of Off, Subtle) and A/B/X trials.</summary>
    public const int CockpitTrials = 9, OutsideTrials = 16, AbxTrials = 3;
    /// <summary>Part A: A1 rates 8 directions at 3 levels; A2 is 8 A/B/X trials, Off against Subtle.</summary>
    public const int PartADirections = 8, PartAAbx = 8;
    public const int SampleRate = 48000;
    /// <summary>The resource keys and file names the test mission adds (l10n/DEFAULT).</summary>
    public const string ScriptKey = "ResKey_CockpitSpaceTest", ScriptFile = "cockpit_space_test.lua", TickKey = "ResKey_CockpitSpaceTick", TickFile = "cockpit_space_tick.wav";

    // ---- [AudioTest] ----------------------------------------------------------------------------------------------------

    /// <summary>The ini text with [AudioTest] for <paramref name="test"/> (null: without it). Any [AudioTest] already
    /// there is removed first; the rest of the file is kept as it is.</summary>
    public static string WithSection(string ini, AudioTestSettings? test)
    {
        var newline = ini.Contains("\r\n", StringComparison.Ordinal) ? "\r\n" : "\n";
        var lines = ini.ReplaceLineEndings("\n").Split('\n').ToList();
        var kept = new List<string>(); var inTest = false;
        foreach (var line in lines)
        {
            var t = line.Trim();
            if (t.Length > 2 && t[0] == '[' && t[^1] == ']') inTest = t[1..^1].Trim().Equals(Section, StringComparison.OrdinalIgnoreCase);
            // The section's own comment line goes with it.
            if (!inTest && t == SectionComment) continue;
            if (!inTest) kept.Add(line);
        }
        var text = string.Join("\n", kept).TrimEnd('\n', ' ') + "\n";
        if (test is not null)
        {
            text += "\n" + SectionComment + "\n[" + Section + "]\nActive=1\nSeed=" + test.Seed.ToString(CultureInfo.InvariantCulture)
                + "\nTrials=" + Math.Clamp(test.Trials, 1, 99).ToString(CultureInfo.InvariantCulture)
                + (test.Level is { } level ? "\nLevel=" + Math.Clamp(level, Binaural.CockpitSpaceMin, Binaural.CockpitSpaceMax).ToString(CultureInfo.InvariantCulture) : "") + "\n";
        }
        return text.Replace("\n", newline, StringComparison.Ordinal);
    }
    private const string SectionComment = "; Cockpit space listening test (DcsVr.Cli audio-test). Removed when the test stops and by Back to stock DCS.";

    /// <summary>The [AudioTest] values in an ini text; null without the section or with Active not 1.</summary>
    public static AudioTestSettings? Read(string ini)
    {
        var values = IniFile.Parse(ini.ReplaceLineEndings("\n"));
        if (values.GetValueOrDefault(Section + ".Active")?.Trim() != "1") return null;
        uint.TryParse(values.GetValueOrDefault(Section + ".Seed"), NumberStyles.None, CultureInfo.InvariantCulture, out var seed);
        int.TryParse(values.GetValueOrDefault(Section + ".Trials"), NumberStyles.None, CultureInfo.InvariantCulture, out var trials);
        int? level = int.TryParse(values.GetValueOrDefault(Section + ".Level"), NumberStyles.None, CultureInfo.InvariantCulture, out var l) ? l : null;
        return new(seed, trials, level);
    }

    // ---- Blind orders ---------------------------------------------------------------------------------------------------

    /// <summary>A small fixed generator (SplitMix64), so an order made today is the same in any later .NET.</summary>
    private sealed class Rng(ulong seed)
    {
        private ulong _state = seed;
        public ulong Next()
        {
            var z = _state += 0x9E3779B97F4A7C15UL;
            z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9UL; z = (z ^ (z >> 27)) * 0x94D049BB133111EBUL;
            return z ^ (z >> 31);
        }
        public int Below(int n) => (int)(Next() % (ulong)n);
        public void Shuffle<T>(IList<T> items) { for (var i = items.Count - 1; i > 0; i--) { var j = Below(i + 1); (items[i], items[j]) = (items[j], items[i]); } }
        public double Unit() => (Next() >> 11) * (1.0 / (1UL << 53));
    }
    private static Rng For(uint seed, string what) => new(seed * 0x100000001B3UL ^ what.Aggregate(2166136261u, (h, c) => unchecked((h ^ c) * 16777619u)));

    /// <summary>Levels in blocks: each block holds every level of <paramref name="levels"/> once, in a shuffled order.</summary>
    private static int[] Blocks(uint seed, string what, int count, int[] levels)
    {
        var rng = For(seed, what); var order = new List<int>();
        while (order.Count < count) { var block = levels.ToList(); rng.Shuffle(block); order.AddRange(block); }
        return [.. order.Take(count)];
    }
    /// <summary>Part B's rating trials 1..<see cref="CockpitTrials"/>: Off, Subtle, Normal in blocks of three.</summary>
    public static int[] CockpitOrder(uint seed) => Blocks(seed, "cockpit", CockpitTrials, [0, 1, 2]);
    /// <summary>Part B's outside sounds 1..<see cref="OutsideTrials"/>: Off and Subtle in blocks of two.</summary>
    public static int[] OutsideOrder(uint seed) => Blocks(seed, "outside", OutsideTrials, [0, 1]);
    /// <summary>A/B/X trials (A = Off, B = Subtle): true where X is A.</summary>
    public static bool[] AbxXIsA(uint seed, string part, int count)
    {
        var half = Blocks(seed, "abx-" + part, count, [0, 1]);
        return [.. half.Select(h => h == 0)];
    }

    /// <summary>The level Part B plays for a mission event (<see cref="MissionEvent"/>), or null when it sets none.</summary>
    public static int? LevelFor(MissionEvent e, uint seed) => e.Kind switch
    {
        "trial" when e.Number is >= 1 and <= CockpitTrials => CockpitOrder(seed)[e.Number - 1],
        "outside" when e.Number is >= 1 and <= OutsideTrials => OutsideOrder(seed)[e.Number - 1],
        "abx" when e.Number is >= 1 and <= AbxTrials => e.Text switch { "A" => 0, "B" => 1, "X" => AbxXIsA(seed, "b", AbxTrials)[e.Number - 1] ? 0 : 1, _ => null },
        _ => null,
    };

    // ---- Part A: offline stimuli -----------------------------------------------------------------------------------------

    /// <summary>One rendered stimulus: a dry signal from a direction at a level, the generic family. Azimuth clockwise
    /// from the nose (right positive), elevation up; the render tool's frame is x forward, y up, z right.</summary>
    public sealed record Stimulus(string Id, string Signal, double AzimuthDeg, double ElevationDeg, double DistanceM, int Level)
    {
        [System.Text.Json.Serialization.JsonIgnore]
        public (double X, double Y, double Z) Position
        {
            get
            {
                double az = AzimuthDeg * Math.PI / 180, el = ElevationDeg * Math.PI / 180;
                return (DistanceM * Math.Cos(el) * Math.Cos(az), DistanceM * Math.Sin(el), DistanceM * Math.Cos(el) * Math.Sin(az));
            }
        }
        /// <summary>Front (true) or back (false); null for a direction at the side.</summary>
        [System.Text.Json.Serialization.JsonIgnore]
        public bool? Front => Math.Abs(Math.Cos(AzimuthDeg * Math.PI / 180)) < 1e-6 ? null : Math.Cos(AzimuthDeg * Math.PI / 180) > 0;
    }
    /// <summary>A1 trial: rate one stimulus (externalisation 0..4, front or back).</summary>
    public sealed record RatingTrial(int Trial, string Stimulus);
    /// <summary>A/B/X trial: A is Off, B is Subtle, X one of them.</summary>
    public sealed record AbxTrial(int Trial, string A, string B, string X);
    /// <summary>What the runner sees (ids only, no level, no direction).</summary>
    public sealed record PartATrials(IReadOnlyList<RatingTrial> A1, IReadOnlyList<AbxTrial> A2);
    /// <summary>The unblinding key: every stimulus and the seed.</summary>
    public sealed record PartAKey(uint Seed, IReadOnlyList<Stimulus> Stimuli);

    /// <summary>The 8 Part A directions: front and back pairs (no side, which has no front/back answer) and two above and
    /// below the front, at cockpit distance (0.5 m). (azimuth, elevation).</summary>
    public static IReadOnlyList<(double Az, double El)> PartADirectionList { get; } = [(0, 0), (45, 0), (135, 0), (180, 0), (-135, 0), (-45, 0), (0, 45), (0, -30)];

    /// <summary>Part A's plan from a seed: 24 stimuli (8 directions x Off, Subtle, Normal), the pink burst train and the
    /// switch click alternating by direction (the same signal at the three levels of one direction, so they pair), A1 in
    /// a shuffled order, A2 eight A/B/X trials on random directions. Ids are opaque ("s01"..).</summary>
    public static (PartATrials Trials, PartAKey Key) PartAPlan(uint seed)
    {
        var rng = For(seed, "part-a");
        var stimuli = new List<Stimulus>();
        for (var d = 0; d < PartADirections; d++)
            for (var level = 0; level <= 2; level++)
                stimuli.Add(new("", d % 2 == 0 ? "pink" : "click", PartADirectionList[d].Az, PartADirectionList[d].El, 0.5, level));
        // Opaque ids in a shuffled order.
        var ids = Enumerable.Range(1, stimuli.Count).ToList(); rng.Shuffle(ids);
        stimuli = [.. stimuli.Select((s, i) => s with { Id = "s" + ids[i].ToString("00", CultureInfo.InvariantCulture) })];
        var a1 = stimuli.Select(s => s.Id).ToList(); rng.Shuffle(a1);
        var xIsA = AbxXIsA(seed, "a", PartAAbx);
        var a2 = new List<AbxTrial>();
        for (var t = 0; t < PartAAbx; t++)
        {
            var d = rng.Below(PartADirections);
            var a = stimuli.First(s => s.AzimuthDeg == PartADirectionList[d].Az && s.ElevationDeg == PartADirectionList[d].El && s.Level == 0).Id;
            var b = stimuli.First(s => s.AzimuthDeg == PartADirectionList[d].Az && s.ElevationDeg == PartADirectionList[d].El && s.Level == 1).Id;
            a2.Add(new(t + 1, a, b, xIsA[t] ? a : b));
        }
        return (new([.. a1.Select((id, i) => new RatingTrial(i + 1, id))], a2), new(seed, [.. stimuli.OrderBy(s => s.Id, StringComparer.Ordinal)]));
    }

    /// <summary>The render tool's arguments for one stimulus (docs/COCKPIT_SPACE.md section 12, binaural_room_render.exe):
    /// the generic family at the stimulus level (0 renders the direct path alone, the Off render), head straight, the
    /// dry signal as input; it writes render.wav into <paramref name="outDir"/>.</summary>
    public static IReadOnlyList<string> RenderArguments(Stimulus s, string dryWav, string outDir, string? hrtf)
    {
        var (x, y, z) = s.Position;
        static string N(double v) => Math.Round(v, 4).ToString("0.####", CultureInfo.InvariantCulture);
        List<string> args = ["--family", "generic", "--strength", s.Level.ToString(CultureInfo.InvariantCulture), "--source", $"{N(x)},{N(y)},{N(z)}", "--yaw", "0", "--input", dryWav, "--out", outDir];
        if (!string.IsNullOrWhiteSpace(hrtf)) { args.Add("--hrtf"); args.Add(hrtf); }
        return args;
    }

    // ---- Signals ------------------------------------------------------------------------------------------------------

    /// <summary>A dry mono stimulus, 1.5 s at 48 kHz, peak -6 dBFS: "pink" (three 250 ms pink-noise bursts with 10 ms
    /// ramps, 150 ms apart) or "click" (three switch clicks 500 ms apart over a 115 Hz hum with harmonics, -20 dB).</summary>
    public static float[] DrySignal(string signal, uint seed)
    {
        var n = SampleRate * 3 / 2; var x = new float[n]; var rng = For(seed, "dry-" + signal);
        double White() => rng.Unit() * 2 - 1;
        if (signal == "pink")
        {
            // Paul Kellet's pink filter (economy version).
            double b0 = 0, b1 = 0, b2 = 0;
            for (var burst = 0; burst < 3; burst++)
            {
                int start = SampleRate / 10 + burst * (SampleRate * 4 / 10), length = SampleRate / 4, ramp = SampleRate / 100;
                for (var i = 0; i < length; i++)
                {
                    var w = White();
                    b0 = 0.99765 * b0 + w * 0.0990460; b1 = 0.96300 * b1 + w * 0.2965164; b2 = 0.57000 * b2 + w * 1.0526913;
                    var pink = b0 + b1 + b2 + w * 0.1848;
                    var g = i < ramp ? 0.5 - 0.5 * Math.Cos(Math.PI * i / ramp) : i >= length - ramp ? 0.5 - 0.5 * Math.Cos(Math.PI * (length - 1 - i) / ramp) : 1;
                    x[start + i] = (float)(pink * g);
                }
            }
        }
        else if (signal == "click")
        {
            for (var i = 0; i < n; i++)
            {
                var t = (double)i / SampleRate;
                x[i] = (float)(0.1 * (Math.Sin(2 * Math.PI * 115 * t) + 0.5 * Math.Sin(2 * Math.PI * 230 * t) + 0.25 * Math.Sin(2 * Math.PI * 345 * t)));
            }
            for (var c = 0; c < 3; c++)
            {
                var start = SampleRate / 5 + c * SampleRate / 2;
                for (var i = 0; i < SampleRate / 50; i++)
                {
                    var t = (double)i / SampleRate;
                    x[start + i] += (float)(Math.Exp(-t / 0.0015) * (White() + Math.Sin(2 * Math.PI * 3200 * t)) + Math.Exp(-t / 0.004) * 0.5 * Math.Sin(2 * Math.PI * 1100 * t));
                }
            }
        }
        else throw new ArgumentException("Unknown signal: " + signal);
        var peak = x.Max(v => Math.Abs(v));
        if (peak > 0) for (var i = 0; i < n; i++) x[i] = (float)(x[i] / peak * 0.5);
        return x;
    }

    /// <summary>The ready tick the mission plays (30 ms, 1 kHz, soft ramps).</summary>
    public static float[] Tick()
    {
        var n = SampleRate * 3 / 100; var x = new float[n];
        for (var i = 0; i < n; i++) x[i] = (float)(0.4 * Math.Sin(2 * Math.PI * 1000 * i / SampleRate) * Math.Sin(Math.PI * i / n));
        return x;
    }

    /// <summary>A mono 16-bit PCM WAV at 48 kHz.</summary>
    public static byte[] Wav(float[] samples)
    {
        using var stream = new MemoryStream(); using var w = new BinaryWriter(stream);
        var data = samples.Length * 2;
        w.Write("RIFF"u8); w.Write(36 + data); w.Write("WAVE"u8); w.Write("fmt "u8); w.Write(16); w.Write((short)1); w.Write((short)1);
        w.Write(SampleRate); w.Write(SampleRate * 2); w.Write((short)2); w.Write((short)16); w.Write("data"u8); w.Write(data);
        foreach (var s in samples) w.Write((short)Math.Clamp(Math.Round(s * 32767.0), -32768, 32767));
        w.Flush(); return stream.ToArray();
    }

    // ---- Part B: the test mission ------------------------------------------------------------------------------------

    /// <summary>The mission script (DCS mission scripting, read statically from MissionEditor\modules\me_trigrules.lua and
    /// the shipped missions: DO SCRIPT FILE at MISSION START; trigger.action.explosion for a world sound at a known point;
    /// missionCommands for the F10 radio menu; env.info to dcs.log). Nothing here names a level: the kit sets it from the
    /// log lines. <paramref name="seed"/> shuffles the outside directions.</summary>
    public static string MissionScript(uint seed) => MissionScriptTemplate.Replace("__SEED__", (seed % 2147483646 + 1).ToString(CultureInfo.InvariantCulture), StringComparison.Ordinal)
        .Replace("__TRIALS__", CockpitTrials.ToString(CultureInfo.InvariantCulture), StringComparison.Ordinal)
        .Replace("__OUTSIDE__", OutsideTrials.ToString(CultureInfo.InvariantCulture), StringComparison.Ordinal)
        .Replace("__ABX__", AbxTrials.ToString(CultureInfo.InvariantCulture), StringComparison.Ordinal)
        .Replace("__TICK__", "l10n/DEFAULT/" + TickFile, StringComparison.Ordinal);

    private const string MissionScriptTemplate = """
        -- Cockpit space listening test (DCS Control, tools/listening-test). Made by DcsVr.Cli audio-test mission.
        -- Everything goes through the F10 radio menu (Other): no key bindings. Each choice is written to dcs.log as a
        -- "CSPACE" line; the kit on the PC reads it and sets the sound for the trial. Levels are never named here.
        local SEED = __SEED__
        local TRIALS, OUTSIDE, ABX = __TRIALS__, __OUTSIDE__, __ABX__
        local TICK = "__TICK__"
        local state = { trial = 0, outside = 0, abx = 0 }
        local function log(text) env.info("CSPACE " .. text) end
        local function rnd(n) SEED = (SEED * 16807) % 2147483647; return (SEED % n) + 1 end
        -- Outside directions: two shuffled passes over 8 bearings (degrees clockwise from the nose).
        local bearings = {}
        for pass = 1, 2 do
          local b = { 0, 45, 90, 135, 180, 225, 270, 315 }
          for i = #b, 2, -1 do local j = rnd(i); b[i], b[j] = b[j], b[i] end
          for i = 1, #b do bearings[#bearings + 1] = b[i] end
        end
        local function ready(delay)
          timer.scheduleFunction(function() trigger.action.outSound(TICK); return nil end, nil, timer.getTime() + delay)
        end
        local function say(text) trigger.action.outText(text, 4, true) end

        local function nextTrial()
          if state.trial >= TRIALS then say("All cockpit trials done."); return end
          state.trial = state.trial + 1
          log("trial " .. state.trial)
          say("Cockpit trial " .. state.trial .. " of " .. TRIALS .. ". Wait for the tick, then flip switches around you and turn your head.")
          ready(3)
        end
        local function rate(k)
          if state.trial == 0 then say("Start a cockpit trial first."); return end
          log("rate " .. state.trial .. " " .. k)
          say("Trial " .. state.trial .. ": " .. k .. ".")
        end
        local function outside()
          if state.outside >= OUTSIDE then say("All outside sounds done."); return end
          local unit = world.getPlayer()
          if not unit then say("No player aircraft."); return end
          state.outside = state.outside + 1
          local n, az = state.outside, bearings[state.outside]
          log("outside " .. n .. " az " .. az)
          say("Outside sound " .. n .. " of " .. OUTSIDE .. " in a few seconds. Look straight ahead.")
          timer.scheduleFunction(function()
            local u = world.getPlayer()
            if not u then return nil end
            local pos = u:getPosition()
            local fx, fz = pos.x.x, pos.x.z
            local len = math.sqrt(fx * fx + fz * fz)
            if len < 1e-6 then fx, fz, len = 1, 0, 1 end
            fx, fz = fx / len, fz / len
            local a = math.rad(az)
            local dx, dz = math.cos(a) * fx - math.sin(a) * fz, math.cos(a) * fz + math.sin(a) * fx
            local px, pz = pos.p.x + dx * 150, pos.p.z + dz * 150
            local py = land.getHeight({ x = px, y = pz }) + 15
            trigger.action.explosion({ x = px, y = py, z = pz }, 3)
            return nil
          end, nil, timer.getTime() + 4)
        end
        local function heard(deg)
          if state.outside == 0 then say("Play an outside sound first."); return end
          log("heard " .. state.outside .. " " .. deg)
          say("Outside sound " .. state.outside .. ": noted.")
        end
        local function abxNext()
          if state.abx >= ABX then say("All A/B/X trials done."); return end
          state.abx = state.abx + 1
          log("abxtrial " .. state.abx)
          say("A/B/X trial " .. state.abx .. " of " .. ABX .. ". Play A, B and X, flip switches with each, then answer.")
        end
        local function abxPlay(which)
          if state.abx == 0 then say("Start an A/B/X trial first."); return end
          log("abx " .. state.abx .. " " .. which)
          say(which .. ": wait for the tick.")
          ready(3)
        end
        local function abxAnswer(which)
          if state.abx == 0 then say("Start an A/B/X trial first."); return end
          log("abxanswer " .. state.abx .. " " .. which)
          say("A/B/X trial " .. state.abx .. ": noted.")
        end

        local root = missionCommands.addSubMenu("Cockpit space test")
        missionCommands.addCommand("Next cockpit trial", root, nextTrial)
        local rateMenu = missionCommands.addSubMenu("Rate the cockpit trial", root)
        local labels = { [0] = "0: inside my head", [1] = "1: at my head", [2] = "2: just outside", [3] = "3: clearly outside", [4] = "4: where it should be" }
        for k = 0, 4 do missionCommands.addCommand(labels[k], rateMenu, rate, k) end
        missionCommands.addCommand("Next outside sound", root, outside)
        local heardMenu = missionCommands.addSubMenu("The outside sound came from", root)
        local names = { { 0, "Front" }, { 45, "Front right" }, { 90, "Right" }, { 135, "Back right" }, { 180, "Back" }, { 225, "Back left" }, { 270, "Left" }, { 315, "Front left" } }
        for _, d in ipairs(names) do missionCommands.addCommand(d[2], heardMenu, heard, d[1]) end
        local abxMenu = missionCommands.addSubMenu("A/B/X", root)
        missionCommands.addCommand("Next A/B/X trial", abxMenu, abxNext)
        for _, w in ipairs({ "A", "B", "X" }) do missionCommands.addCommand("Play " .. w, abxMenu, abxPlay, w) end
        missionCommands.addCommand("X is A", abxMenu, abxAnswer, "A")
        missionCommands.addCommand("X is B", abxMenu, abxAnswer, "B")
        missionCommands.addCommand("End the test", root, function() log("end"); say("Test ended. Thank you.") end)
        log("start " .. TRIALS .. " " .. OUTSIDE .. " " .. ABX)
        say("Cockpit space test ready: F10 radio menu, Other, Cockpit space test.")
        """;

    /// <summary>
    /// The test mission: a copy of <paramref name="sourceMiz"/> (any single-player mission saved by DCS, the pilot's own
    /// aircraft cold on the ramp suits best) with the script and the tick added as l10n/DEFAULT resources and one MISSION
    /// START trigger that runs the script (DO SCRIPT FILE), written into the mission's trig and trigrules tables the way
    /// DCS's own serializer writes them. Refused when the mission already has the test or is not one DCS saved.
    /// </summary>
    public static byte[] BuildMission(byte[] sourceMiz, uint seed)
    {
        using var input = new ZipArchive(new MemoryStream(sourceMiz), ZipArchiveMode.Read);
        var missionEntry = input.GetEntry("mission") ?? throw new InvalidDataException("Not a DCS mission: it has no \"mission\" file.");
        string mission; using (var r = new StreamReader(missionEntry.Open(), Encoding.UTF8)) mission = r.ReadToEnd();
        if (mission.Contains(ScriptKey, StringComparison.Ordinal)) throw new InvalidDataException("This mission already has the cockpit space test.");
        var map = input.GetEntry("l10n/DEFAULT/mapResource") is { } m ? new StreamReader(m.Open(), Encoding.UTF8).ReadToEnd() : "mapResource = \n{\n} -- end of mapResource\n";
        var newMission = InjectTrigger(mission);
        var newMap = InjectResources(map);
        using var output = new MemoryStream();
        using (var zip = new ZipArchive(output, ZipArchiveMode.Create, leaveOpen: true))
        {
            foreach (var e in input.Entries)
            {
                if (e.FullName is "mission" or "l10n/DEFAULT/mapResource") continue;
                var copy = zip.CreateEntry(e.FullName, CompressionLevel.Optimal);
                using var from = e.Open(); using var to = copy.Open(); from.CopyTo(to);
            }
            void Add(string name, byte[] bytes) { using var s = zip.CreateEntry(name, CompressionLevel.Optimal).Open(); s.Write(bytes); }
            Add("mission", new UTF8Encoding(false).GetBytes(newMission));
            Add("l10n/DEFAULT/mapResource", new UTF8Encoding(false).GetBytes(newMap));
            Add("l10n/DEFAULT/" + ScriptFile, new UTF8Encoding(false).GetBytes(MissionScript(seed)));
            Add("l10n/DEFAULT/" + TickFile, Wav(Tick()));
        }
        return output.ToArray();
    }

    /// <summary>The mission text with the MISSION START trigger added at the next free index of trigrules (and the same
    /// index in trig: conditions, actions, funcStartup).</summary>
    public static string InjectTrigger(string mission)
    {
        var nl = mission.Contains("\r\n", StringComparison.Ordinal) ? "\r\n" : "\n";
        var text = mission.ReplaceLineEndings("\n");
        const string emptyRules = "\t[\"trigrules\"] = {},\n";
        var rulesEmpty = text.IndexOf(emptyRules, StringComparison.Ordinal);
        var (rulesStart, rulesEnd) = Block(text, "\t[\"trigrules\"]", 1) ?? (rulesEmpty >= 0 ? (rulesEmpty, rulesEmpty + emptyRules.Length) : throw new InvalidDataException("Not a mission DCS saved: no trigrules table."));
        var used = TopIndexRegex().Matches(text[rulesStart..rulesEnd]).Select(x => int.Parse(x.Groups[1].Value, CultureInfo.InvariantCulture));
        var (trigStart, trigEnd) = Block(text, "\t[\"trig\"]", 1) ?? throw new InvalidDataException("Not a mission DCS saved: no trig table.");
        var trigUsed = TrigIndexRegex().Matches(text[trigStart..trigEnd]).Select(x => int.Parse(x.Groups[1].Value, CultureInfo.InvariantCulture));
        var n = Math.Max(used.DefaultIfEmpty(0).Max(), trigUsed.DefaultIfEmpty(0).Max()) + 1;
        var i = n.ToString(CultureInfo.InvariantCulture);
        string rule = $"\t\t[{i}] = \n\t\t{{\n\t\t\t[\"rules\"] = {{}},\n\t\t\t[\"eventlist\"] = \"\",\n\t\t\t[\"comment\"] = \"Cockpit space test\",\n\t\t\t[\"predicate\"] = \"triggerStart\",\n"
            + $"\t\t\t[\"actions\"] = \n\t\t\t{{\n\t\t\t\t[1] = \n\t\t\t\t{{\n\t\t\t\t\t[\"file\"] = \"{ScriptKey}\",\n\t\t\t\t\t[\"predicate\"] = \"a_do_script_file\",\n\t\t\t\t}}, -- end of [1]\n"
            + $"\t\t\t}}, -- end of [\"actions\"]\n\t\t}}, -- end of [{i}]\n";
        // Trig first (it is before or after trigrules; edit the later one first so the earlier offsets hold).
        var edits = new List<(int At, int Length, string Text)>();
        foreach (var (name, value) in new[] { ("conditions", "return(true)"), ("actions", $"a_do_script_file(getValueResourceByKey(\\\"{ScriptKey}\\\"));"), ("funcStartup", $"if mission.trig.conditions[{i}]() then mission.trig.actions[{i}]() end") })
        {
            var line = $"\t\t\t[{i}] = \"{value}\",\n";
            var inner = Block(text[..trigEnd], "\t\t[\"" + name + "\"]", 2, trigStart);
            if (inner is { } b) edits.Add((b.End - EndLength(text, b.End), 0, line));
            else
            {
                var empty = text.IndexOf("\t\t[\"" + name + "\"] = {},\n", trigStart, StringComparison.Ordinal);
                if (empty < 0 || empty > trigEnd) edits.Add((trigEnd - EndLength(text, trigEnd), 0, $"\t\t[\"{name}\"] = \n\t\t{{\n{line}\t\t}}, -- end of [\"{name}\"]\n"));
                else { var len = ("\t\t[\"" + name + "\"] = {},\n").Length; edits.Add((empty, len, $"\t\t[\"{name}\"] = \n\t\t{{\n{line}\t\t}}, -- end of [\"{name}\"]\n")); }
            }
        }
        if (rulesStart == rulesEmpty) edits.Add((rulesEmpty, emptyRules.Length, "\t[\"trigrules\"] = \n\t{\n" + rule + "\t}, -- end of [\"trigrules\"]\n"));
        else edits.Add((rulesEnd - EndLength(text, rulesEnd), 0, rule));
        foreach (var (at, length, insert) in edits.OrderByDescending(e => e.At)) text = text[..at] + insert + text[(at + length)..];
        return text.Replace("\n", nl, StringComparison.Ordinal);
    }

    /// <summary>The mapResource text with the script and the tick added.</summary>
    public static string InjectResources(string map)
    {
        var nl = map.Contains("\r\n", StringComparison.Ordinal) ? "\r\n" : "\n";
        var text = map.ReplaceLineEndings("\n");
        var lines = $"\t[\"{ScriptKey}\"] = \"{ScriptFile}\",\n\t[\"{TickKey}\"] = \"{TickFile}\",\n";
        var end = text.LastIndexOf("} -- end of mapResource", StringComparison.Ordinal);
        if (end >= 0) text = text[..end] + lines + text[end..];
        else if (MapEmptyRegex().Match(text) is { Success: true } e) text = text[..e.Index] + "mapResource = \n{\n" + lines + "} -- end of mapResource" + text[(e.Index + e.Length)..];
        else throw new InvalidDataException("Not a mission DCS saved: its mapResource is not as DCS writes it.");
        return text.Replace("\n", nl, StringComparison.Ordinal);
    }

    /// <summary>The span of a table "&lt;indent&gt;[name] = \n&lt;indent&gt;{" .. "&lt;indent&gt;}, -- end of [name]" (DCS's
    /// serializer), from the start of its key to just after its closing line; null when not there.</summary>
    private static (int Start, int End)? Block(string text, string key, int depth, int from = 0)
    {
        var indent = new string('\t', depth);
        var open = text.IndexOf(key + " = \n" + indent + "{\n", from, StringComparison.Ordinal);
        if (open < 0) return null;
        var name = key.TrimStart('\t');
        var closing = "\n" + indent + "}, -- end of " + name + "\n";
        var close = text.IndexOf(closing, open, StringComparison.Ordinal);
        if (close < 0) { closing = "\n" + indent + "} -- end of " + name + "\n"; close = text.IndexOf(closing, open, StringComparison.Ordinal); }
        return close < 0 ? null : (open, close + closing.Length);
    }
    /// <summary>The length of the closing line that ends at <paramref name="end"/> (without the newline before it).</summary>
    private static int EndLength(string text, int end) { var start = text.LastIndexOf('\n', end - 2) + 1; return end - start; }

    [GeneratedRegex(@"\n\t\t\[(\d+)\] = \n")] private static partial Regex TopIndexRegex();
    [GeneratedRegex(@"\n\t\t\t\[(\d+)\] = ")] private static partial Regex TrigIndexRegex();
    [GeneratedRegex(@"mapResource\s*=\s*\{\s*\}")] private static partial Regex MapEmptyRegex();

    // ---- The mission's lines in dcs.log -------------------------------------------------------------------------------

    /// <summary>One mission line: kind (start, trial, rate, outside, heard, abxtrial, abx, abxanswer, end), the trial
    /// number, a value (rating, bearing, heard bearing) and a text (A, B, X).</summary>
    public sealed record MissionEvent(string Kind, int Number, int? Value = null, string? Text = null);

    /// <summary>A dcs.log line of the test mission, or null.</summary>
    public static MissionEvent? ParseLine(string line)
    {
        var at = line.IndexOf(LogTag + " ", StringComparison.Ordinal);
        if (at < 0) return null;
        var parts = line[(at + LogTag.Length + 1)..].Trim().Split(' ', StringSplitOptions.RemoveEmptyEntries);
        if (parts.Length == 0) return null;
        int? Int(int i) => i < parts.Length && int.TryParse(parts[i], NumberStyles.AllowLeadingSign, CultureInfo.InvariantCulture, out var v) ? v : null;
        return parts[0] switch
        {
            "start" => new("start", 0),
            "end" => new("end", 0),
            "trial" or "abxtrial" when Int(1) is { } t => new(parts[0], t),
            "rate" when Int(1) is { } t && Int(2) is { } k && k is >= 0 and <= 4 => new("rate", t, k),
            "outside" when Int(1) is { } t && parts.Length > 3 && parts[2] == "az" && Int(3) is { } az => new("outside", t, az),
            "heard" when Int(1) is { } t && Int(2) is { } deg => new("heard", t, deg),
            "abx" or "abxanswer" when Int(1) is { } t && parts.Length > 2 && parts[2] is "A" or "B" or "X" => new(parts[0], t, Text: parts[2]),
            _ => null,
        };
    }

    // ---- Analysis -------------------------------------------------------------------------------------------------------

    /// <summary>Per level: the ratings, their median and the share of 3 or more.</summary>
    public sealed record LevelRatings(string Level, int Count, double? Median, double? ShareOutside, double? Reversals);
    /// <summary>A paired comparison (Wilcoxon signed rank, two-sided): pairs, the median difference (second minus first), p.</summary>
    public sealed record Comparison(string What, int Pairs, double? MedianDifference, double P);
    /// <summary>A/B/X: correct of trials, one-sided binomial p.</summary>
    public sealed record AbxResult(string Part, int Correct, int Trials, double P);
    public sealed record Analysis(uint Seed, IReadOnlyList<LevelRatings> Levels, IReadOnlyList<Comparison> Comparisons, IReadOnlyList<AbxResult> Abx,
        IReadOnlyList<LevelRatings> Outside, string Verdict, IReadOnlyList<string> Notes);

    /// <summary>One answer of Part A's runner (answers.csv): "a1,trial,stimulus,rating,front|back" or "a2,trial,,A|B,".</summary>
    public sealed record PartAAnswer(string Part, int Trial, string? Stimulus, int? Rating, string? Answer);

    public static IReadOnlyList<PartAAnswer> ReadAnswers(IEnumerable<string> csv)
    {
        var list = new List<PartAAnswer>();
        foreach (var raw in csv)
        {
            var f = raw.Trim().Split(',');
            if (f.Length < 4 || f[0] is not ("a1" or "a2") || !int.TryParse(f[1], NumberStyles.None, CultureInfo.InvariantCulture, out var trial)) continue;
            list.Add(f[0] == "a1"
                ? new("a1", trial, f[2], int.TryParse(f[3], NumberStyles.None, CultureInfo.InvariantCulture, out var r) && r is >= 0 and <= 4 ? r : null, f.Length > 4 && f[4] is "front" or "back" ? f[4] : null)
                : new("a2", trial, null, null, f[3] is "A" or "B" ? f[3] : null));
        }
        return list;
    }

    /// <summary>
    /// The analysis (docs/COCKPIT_SPACE.md section 11): per level the median externalisation and the share rated 3 or
    /// more (Part A1 and Part B), front/back reversals (A1, and the outside sounds of Part B), Wilcoxon signed rank Off
    /// against Subtle and Subtle against Normal (paired by direction in A1 and by block in Part B), A/B/X correct with its
    /// binomial p. Subtle is accepted as the default when it rates higher than Off (median difference +1 or more, p below
    /// 0.05 over A1 and B together) and its reversals are not worse than Off's. Either part may be missing.
    /// </summary>
    public static Analysis Analyze(uint seed, IReadOnlyList<PartAAnswer>? partA, IEnumerable<string>? partBLog)
    {
        var (trials, key) = PartAPlan(seed);
        var byId = key.Stimuli.ToDictionary(s => s.Id, StringComparer.Ordinal);
        var ratings = new Dictionary<int, List<double>> { [0] = [], [1] = [], [2] = [] };
        var reversals = new Dictionary<int, (int Bad, int All)> { [0] = (0, 0), [1] = (0, 0), [2] = (0, 0) };
        // Pairs: per direction (A1) and per block (B), the rating at each level.
        var pairs = new Dictionary<string, Dictionary<int, double>>();
        var notes = new List<string>(); var abx = new List<AbxResult>();
        if (partA is not null)
        {
            foreach (var a in partA.Where(a => a.Part == "a1" && a.Stimulus is not null && byId.ContainsKey(a.Stimulus)))
            {
                var s = byId[a.Stimulus!];
                if (a.Rating is { } r)
                {
                    ratings[s.Level].Add(r);
                    var unit = pairs.TryGetValue("a:" + s.AzimuthDeg + "/" + s.ElevationDeg, out var u) ? u : pairs["a:" + s.AzimuthDeg + "/" + s.ElevationDeg] = [];
                    unit[s.Level] = r;
                }
                if (s.Front is { } front && a.Answer is { } fb)
                {
                    var (bad, all) = reversals[s.Level];
                    reversals[s.Level] = (bad + ((fb == "front") != front ? 1 : 0), all + 1);
                }
            }
            var a2 = partA.Where(a => a.Part == "a2" && a.Answer is not null).ToList();
            if (a2.Count > 0)
            {
                var correct = a2.Count(a => trials.A2.FirstOrDefault(t => t.Trial == a.Trial) is { } t && (a.Answer == "A") == (t.X == t.A));
                abx.Add(new("A2", correct, a2.Count, BinomialP(correct, a2.Count)));
            }
        }
        var outside = new Dictionary<int, (int Bad, int All, List<double> Errors)> { [0] = (0, 0, []), [1] = (0, 0, []) };
        if (partBLog is not null)
        {
            var events = partBLog.Select(ParseLine).OfType<MissionEvent>().ToList();
            // Only the last run of the mission counts (a restart begins again with "start").
            var last = events.FindLastIndex(e => e.Kind == "start");
            if (last > 0) events = events[last..];
            var order = CockpitOrder(seed);
            foreach (var rate in events.Where(e => e.Kind == "rate" && e.Number is >= 1 and <= CockpitTrials).GroupBy(e => e.Number).Select(g => g.Last()))
            {
                var level = order[rate.Number - 1];
                ratings[level].Add(rate.Value!.Value);
                var unit = pairs.TryGetValue("b:" + (rate.Number - 1) / 3, out var u) ? u : pairs["b:" + (rate.Number - 1) / 3] = [];
                unit[level] = rate.Value.Value;
            }
            var outsideOrder = OutsideOrder(seed);
            var played = events.Where(e => e.Kind == "outside").GroupBy(e => e.Number).ToDictionary(g => g.Key, g => g.Last().Value!.Value);
            foreach (var h in events.Where(e => e.Kind == "heard" && e.Number is >= 1 and <= OutsideTrials).GroupBy(e => e.Number).Select(g => g.Last()))
            {
                if (!played.TryGetValue(h.Number, out var truth)) continue;
                var level = outsideOrder[h.Number - 1];
                var (bad, all, errors) = outside[level];
                double Cos(int d) => Math.Cos(d * Math.PI / 180);
                var heardDeg = h.Value ?? 0;
                var reversed = Math.Abs(Cos(truth)) > 1e-6 && Math.Abs(Cos(heardDeg)) > 1e-6 && Cos(truth) > 0 != Cos(heardDeg) > 0;
                var error = Math.Abs(((heardDeg - truth) % 360 + 540) % 360 - 180);
                errors.Add(error);
                outside[level] = (bad + (reversed ? 1 : 0), all + 1, errors);
            }
            var xIsA = AbxXIsA(seed, "b", AbxTrials);
            var answers = events.Where(e => e.Kind == "abxanswer" && e.Number is >= 1 and <= AbxTrials).GroupBy(e => e.Number).Select(g => g.Last()).ToList();
            if (answers.Count > 0)
            {
                var correct = answers.Count(a => (a.Text == "A") == xIsA[a.Number - 1]);
                abx.Add(new("B", correct, answers.Count, BinomialP(correct, answers.Count)));
            }
            if (!events.Any(e => e.Kind == "start")) notes.Add("No start line of the test mission in the log.");
        }
        var levels = Enumerable.Range(0, 3).Select(l => new LevelRatings(Binaural.CockpitSpaceName(l), ratings[l].Count, Median(ratings[l]),
            ratings[l].Count == 0 ? null : ratings[l].Count(r => r >= 3) / (double)ratings[l].Count, reversals[l].All == 0 ? null : reversals[l].Bad / (double)reversals[l].All)).ToList();
        Comparison Compare(int first, int second)
        {
            var paired = pairs.Values.Where(p => p.ContainsKey(first) && p.ContainsKey(second)).ToList();
            var diffs = paired.Select(p => p[second] - p[first]).ToList();
            return new($"{Binaural.CockpitSpaceName(first)} → {Binaural.CockpitSpaceName(second)}", paired.Count, Median(diffs),
                WilcoxonSignedRankP(paired.Select(p => p[first]).ToList(), paired.Select(p => p[second]).ToList()));
        }
        var comparisons = new List<Comparison> { Compare(0, 1), Compare(1, 2) };
        var outsideLevels = Enumerable.Range(0, 2).Select(l => new LevelRatings(Binaural.CockpitSpaceName(l), outside[l].All, Median(outside[l].Errors), null,
            outside[l].All == 0 ? null : outside[l].Bad / (double)outside[l].All)).ToList();
        // Reversals of Subtle against Off: A1's and the outside sounds' together.
        int bad0 = reversals[0].Bad + outside[0].Bad, all0 = reversals[0].All + outside[0].All, bad1 = reversals[1].Bad + outside[1].Bad, all1 = reversals[1].All + outside[1].All;
        var notWorse = all0 == 0 || all1 == 0 || bad1 / (double)all1 <= bad0 / (double)all0 + 1e-12;
        var c01 = comparisons[0];
        string verdict;
        if (c01.Pairs == 0) verdict = "Not enough answers for a verdict.";
        else if (c01.MedianDifference >= 1 && c01.P < 0.05 && notWorse)
            verdict = comparisons[1] is { Pairs: > 0, MedianDifference: >= 1, P: < 0.05 } ? "Subtle beats Off, and Normal beats Subtle: Normal could be the default if nobody hears colouration." : "Subtle beats Off: keep Subtle as the default.";
        else if (!notWorse) verdict = "Subtle has more front/back reversals than Off: not accepted as the default.";
        else verdict = $"Subtle does not rate clearly higher than Off (median {Signed(c01.MedianDifference)}, p {c01.P.ToString("0.###", CultureInfo.InvariantCulture)}): not accepted as the default yet.";
        return new(seed, levels, comparisons, abx, outsideLevels, verdict, notes);
        static string Signed(double? v) => v is { } x ? (x >= 0 ? "+" : "") + x.ToString("0.#", CultureInfo.InvariantCulture) : "n/a";
    }

    /// <summary>The analysis as a short Markdown report.</summary>
    public static string Report(Analysis a)
    {
        static string P(double? v, string format = "0.##") => v is { } x ? x.ToString(format, CultureInfo.InvariantCulture) : "–";
        static string Pct(double? v) => v is { } x ? Math.Round(x * 100).ToString(CultureInfo.InvariantCulture) + " %" : "–";
        var b = new StringBuilder();
        b.Append("# Cockpit space listening test\n\nSeed ").Append(a.Seed.ToString(CultureInfo.InvariantCulture)).Append(".\n\n**").Append(a.Verdict).Append("**\n\n");
        b.Append("| Level | Ratings | Median (0-4) | Rated 3 or 4 | Front/back reversals (A1) |\n|---|---|---|---|---|\n");
        foreach (var l in a.Levels) b.Append($"| {l.Level} | {l.Count} | {P(l.Median)} | {Pct(l.ShareOutside)} | {Pct(l.Reversals)} |\n");
        b.Append("\n| Comparison | Pairs | Median difference | Wilcoxon p |\n|---|---|---|---|\n");
        foreach (var c in a.Comparisons) b.Append($"| {c.What} | {c.Pairs} | {P(c.MedianDifference)} | {P(c.P, "0.###")} |\n");
        if (a.Abx.Count > 0)
        {
            b.Append("\n| A/B/X (Off or Subtle) | Correct | p (one-sided) |\n|---|---|---|\n");
            foreach (var x in a.Abx) b.Append($"| {x.Part} | {x.Correct} of {x.Trials} | {P(x.P, "0.###")} |\n");
        }
        if (a.Outside.Any(o => o.Count > 0))
        {
            b.Append("\n| Outside sounds | Answers | Median error (deg) | Front/back reversals |\n|---|---|---|---|\n");
            foreach (var o in a.Outside) b.Append($"| {o.Level} | {o.Count} | {P(o.Median, "0")} | {Pct(o.Reversals)} |\n");
        }
        foreach (var n in a.Notes) b.Append("\n").Append(n).Append('\n');
        return b.ToString();
    }

    public static double? Median(IReadOnlyList<double> values)
    {
        if (values.Count == 0) return null;
        var sorted = values.Order().ToArray();
        return sorted.Length % 2 == 1 ? sorted[sorted.Length / 2] : (sorted[sorted.Length / 2 - 1] + sorted[sorted.Length / 2]) / 2;
    }

    /// <summary>Wilcoxon signed-rank test, two-sided, exact (the permutation distribution of the signed ranks, ties as
    /// average ranks, zero differences dropped). 1 with no non-zero difference.</summary>
    public static double WilcoxonSignedRankP(IReadOnlyList<double> first, IReadOnlyList<double> second)
    {
        var d = first.Zip(second, (a, b) => b - a).Where(x => Math.Abs(x) > 1e-12).ToList();
        var n = d.Count;
        if (n == 0) return 1;
        // Average ranks of |d|, doubled to stay integers.
        var order = d.Select((v, i) => (Abs: Math.Abs(v), i)).OrderBy(x => x.Abs).ToList();
        var rank2 = new int[n];
        for (var k = 0; k < n;)
        {
            var j = k; while (j + 1 < n && Math.Abs(order[j + 1].Abs - order[k].Abs) < 1e-12) j++;
            var avg2 = k + 1 + j + 1; // 2 x average of ranks k+1..j+1
            for (var m = k; m <= j; m++) rank2[order[m].i] = avg2;
            k = j + 1;
        }
        var total = rank2.Sum();
        var wPlus = Enumerable.Range(0, n).Where(i => d[i] > 0).Sum(i => rank2[i]);
        // Counts of every sum of a subset of the ranks.
        var counts = new double[total + 1]; counts[0] = 1;
        foreach (var r in rank2) for (var s = total; s >= r; s--) counts[s] += counts[s - r];
        var all = Math.Pow(2, n);
        double below = 0, above = 0;
        for (var s = 0; s <= total; s++) { if (s <= wPlus) below += counts[s]; if (s >= wPlus) above += counts[s]; }
        return Math.Min(1, 2 * Math.Min(below, above) / all);
    }

    /// <summary>P(at least <paramref name="correct"/> of <paramref name="trials"/> by guessing), p = 0.5.</summary>
    public static double BinomialP(int correct, int trials)
    {
        if (trials <= 0) return 1;
        double sum = 0;
        for (var k = Math.Max(0, correct); k <= trials; k++) sum += Choose(trials, k);
        return Math.Min(1, sum / Math.Pow(2, trials));
        static double Choose(int n, int k) { double c = 1; for (var i = 1; i <= k; i++) c = c * (n - k + i) / i; return c; }
    }
}
