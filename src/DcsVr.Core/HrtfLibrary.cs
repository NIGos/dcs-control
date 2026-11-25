using System.Diagnostics;
using DcsVr.Core.Hrtf;

namespace DcsVr.Core;

/// <summary>
/// The .dcshrtf files as the app needs them: the facts the page shows, from <see cref="DcsHrtfFile"/> (the format's one
/// C# reader and writer, check for check hrtf_format.h's Validate(), so a file the module would refuse is never offered
/// or deployed), and a small test set. The pilot's sets are written by the converter (<see cref="IHrtfConverter"/>).
/// </summary>
public static class HrtfFile
{
    /// <summary>A small valid set from a rigid spherical head (Woodworth delays, a level difference per side): the stand-in
    /// for the converter's output in the tests and the offline interface verification, never offered to a pilot as an
    /// HRTF. <paramref name="stepDeg"/> sets the grid (20° gives 145 directions from −60° to +90°).</summary>
    public static byte[] Synthetic(string title, double headRadiusM = 0.0875, int stepDeg = 20, bool personal = true, ushort taps = 32, uint sampleRate = 48000)
    {
        var dirs = new List<(float X, float Y, float Z)>();
        for (var el = -60; el < 90; el += stepDeg)
            for (var az = 0; az < 360; az += stepDeg)
            {
                double e = el * Math.PI / 180, a = az * Math.PI / 180;
                dirs.Add(((float)(Math.Cos(e) * Math.Cos(a)), (float)Math.Sin(e), (float)(-Math.Cos(e) * Math.Sin(a))));
            }
        dirs.Add((0, 1, 0));
        int n = dirs.Count, t = n - 2;
        var directions = new float[n * 3]; var delays = new float[n * 2]; var triangles = new uint[t * 3]; var ir = new float[n * 2 * taps];
        for (var i = 0; i < n; i++)
        {
            var d = dirs[i];
            directions[i * 3] = d.X; directions[i * 3 + 1] = d.Y; directions[i * 3 + 2] = d.Z;
            // Lateral angle from the median plane; +z is right, so the right ear leads for z > 0.
            var lateral = Math.Asin(Math.Clamp(d.Z, -1f, 1f));
            var itd = headRadiusM / 343.0 * (Math.Abs(lateral) + Math.Sin(Math.Abs(lateral))) * sampleRate;
            float lead = 8, lag = (float)(8 + itd);
            delays[i * 2] = lateral > 0 ? lag : lead; delays[i * 2 + 1] = lateral > 0 ? lead : lag;
            for (var ear = 0; ear < 2; ear++)
            {
                var side = ear == 0 ? -d.Z : d.Z; // towards this ear: louder
                var gain = (float)Math.Pow(10, 6 * side / 20);
                ir[(i * 2 + ear) * taps] = gain; ir[(i * 2 + ear) * taps + 1] = -0.25f * gain;
            }
        }
        for (var i = 1; i + 1 < n; i++) { triangles[(i - 1) * 3] = 0; triangles[(i - 1) * 3 + 1] = (uint)i; triangles[(i - 1) * 3 + 2] = (uint)(i + 1); }
        var flags = DcsHrtfFile.FileFlags.MinimumPhase | DcsHrtfFile.FileFlags.FrontNormalised | (personal ? DcsHrtfFile.FileFlags.Personal : 0);
        return DcsHrtfFile.Write(new(flags, sampleRate, taps, (float)headRadiusM, 1.2f, 1f, title, "Synthetic spherical head (test fixture)", directions, delays, triangles, ir));
    }

    /// <summary>The facts of a valid file (<see cref="DcsHrtfFile.Validate"/>, hrtf_format.h's checks), else null with the
    /// first problem in <paramref name="why"/> (the header's own wording).</summary>
    public static HrtfFacts? Validate(byte[] data, out string? why)
    {
        why = DcsHrtfFile.Validate(data);
        if (why is not null) return null;
        var d = DcsHrtfFile.ReadValid(data);
        int horizontal = 0; bool up = false, down = false;
        for (var i = 0; i < d.DirectionCount; i++)
        {
            var y = d.Directions[i * 3 + 1];
            if (MathF.Abs(y) < 0.05f) horizontal++;
            up |= y > 0.7f; down |= y < -0.5f;
        }
        return new(d.Name.Trim(), d.Source.Trim(), (int)d.SampleRate, d.DirectionCount, d.TriangleCount, d.Taps, d.DatasetHeadRadiusM * 1000.0, d.MeasurementDistanceM,
            d.Flags.HasFlag(DcsHrtfFile.FileFlags.Personal), d.Flags.HasFlag(DcsHrtfFile.FileFlags.DiffuseFieldEq), d.Flags.HasFlag(DcsHrtfFile.FileFlags.FrontNormalised), horizontal, up, down);
    }
}

/// <summary>What a valid .dcshrtf holds: its own name and source text, the sample rate, the measurement grid, the IR
/// length, the head radius fitted to its delays (mm) and the measurement distance (m). <paramref name="Horizontal"/>:
/// directions within about 3° of the horizontal plane; <paramref name="Above"/> / <paramref name="Below"/>: the grid
/// reaches well above (45°) and below (30°).</summary>
public sealed record HrtfFacts(string Title, string Source, int SampleRate, int Directions, int Triangles, int Taps, double HeadRadiusMm, double DistanceM,
    bool Personal, bool DiffuseFieldEq, bool FrontNormalised, int Horizontal, bool Above, bool Below)
{
    /// <summary>The largest time difference between the ears the fitted radius stands for (Woodworth, 90 degrees), ms.</summary>
    public double MaxItdMs => ItdMs(HeadRadiusMm);
    /// <summary>Woodworth's largest time difference between the ears for a head of <paramref name="headRadiusMm"/>, ms.</summary>
    public static double ItdMs(double headRadiusMm) => headRadiusMm / 1000 / 343 * (Math.PI / 2 + 1) * 1000;

    /// <summary>The plain warnings the page shows with the conversion report.</summary>
    public IReadOnlyList<string> Warnings()
    {
        var list = new List<string>();
        if (!Above || !Below) list.Add(!Above && !Below ? "The measurements cover little above or below you: height will sound vague." : !Above ? "The measurements cover little above you: sounds overhead will sound vague." : "The measurements cover little below you.");
        if (Directions < 100) list.Add("Few measured directions: directions between them are guessed.");
        if (Horizontal < 12) list.Add("Few measurements around you at ear height: left, right and behind may be less exact.");
        // The radius is a Woodworth fit of the onsets, close to the head's own size (SADIE II's KU100: 94 mm, about
        // 0.71 ms between the ears): unusual below 70 mm or above 140 mm (about 0.52 and 1.05 ms).
        if (HeadRadiusMm is < 70 or > 140) list.Add($"The time difference between the ears in the file (up to {MaxItdMs:0.00} ms) is unusual. Check that it is your own measurement.");
        return list;
    }
}

/// <summary>One HRTF set in the app's library: the file name (the [Audio] Hrtf value) and its facts, where it came from
/// (the SOFA file's name only, never its folder), when it was imported and what the converter warned about.</summary>
public sealed record HrtfSet(string Name, string Title, string Source, int SampleRate, int Directions, int Taps, double HeadRadiusMm, string? FromFile,
    DateTimeOffset? Imported, IReadOnlyList<string> Warnings, long Bytes, double? Coverage = null)
{
    /// <summary>The largest time difference between the ears its head radius stands for, ms (the page shows it).</summary>
    public double MaxItdMs => HrtfFacts.ItdMs(HeadRadiusMm);
}

/// <summary>A set the plan deploys: Scripts\DcsQvCull\hrtf\&lt;Name&gt;.dcshrtf.</summary>
public sealed record HrtfDeployment(string Name, byte[] Content);

/// <summary>The SOFA converter (docs/BINAURAL_AUDIO.md section 4.2: src/DcsVr.Core/Hrtf and the CLI command
/// <c>hrtf-convert &lt;in.sofa&gt; &lt;out.dcshrtf&gt; [--name]</c>). It writes a .dcshrtf at <paramref name="outputPath"/>,
/// or throws <see cref="HrtfConversionException"/> with a plain reason (wrong convention, NaN data, fewer than 100
/// directions, an unsupported rate). The app validates the result again before keeping it.</summary>
public interface IHrtfConverter
{
    HrtfConversion Convert(string sofaPath, string outputPath, string name, CancellationToken cancel = default);
}
/// <summary>The converter's report beyond what the file holds: its warnings and the share of the sphere the grid covers
/// (0 to 1), when it says.</summary>
public sealed record HrtfConversion(IReadOnlyList<string> Warnings, double? Coverage = null);
public sealed class HrtfConversionException(string message) : Exception(message);

/// <summary>
/// The converter of this installation through its contract interface: <c>DcsVr.Cli hrtf-convert in out --name name</c>
/// next to the app (every build ships it). Exit 0 with the file written is a conversion; lines starting with
/// "warning:" are its warnings and "coverage: N%" its grid coverage. Any other exit is a refusal, whose reason is the
/// last line it printed on stderr. Runs hidden, at below-normal priority, for at most two minutes.
/// </summary>
public sealed class CliHrtfConverter(string? directory = null) : IHrtfConverter
{
    public static readonly TimeSpan Timeout = TimeSpan.FromMinutes(2);
    private static readonly System.Collections.Concurrent.ConcurrentDictionary<string, bool> Probed = new(StringComparer.OrdinalIgnoreCase);
    /// <summary>This installation's CLI has the converter: <c>DcsVr.Cli hrtf-convert --check</c> exits 0 (the contract's
    /// probe, docs/BINAURAL_AUDIO.md section 10). Asked once per folder and app run (hidden, below-normal priority, 10 s).
    /// False while the CLI has no such command, so the Audio page greys out Import SOFA instead of failing.</summary>
    public static bool IsAvailable(string? directory = null)
    {
        var folder = directory ?? AppContext.BaseDirectory;
        return Probed.GetOrAdd(folder, static f =>
        {
            var cli = new[] { "DcsVr.Cli.exe", "DcsVr.Cli" }.Select(n => Path.Combine(f, n)).FirstOrDefault(File.Exists);
            if (cli is null) return false;
            try
            {
                var start = new ProcessStartInfo(cli) { UseShellExecute = false, CreateNoWindow = true, RedirectStandardOutput = true, RedirectStandardError = true, WorkingDirectory = Path.GetDirectoryName(cli)! };
                start.ArgumentList.Add("hrtf-convert"); start.ArgumentList.Add("--check");
                using var process = Process.Start(start);
                if (process is null) return false;
                try { process.PriorityClass = ProcessPriorityClass.BelowNormal; } catch (Exception e) when (e is InvalidOperationException or System.ComponentModel.Win32Exception) { }
                _ = process.StandardOutput.ReadToEndAsync(); _ = process.StandardError.ReadToEndAsync();
                if (!process.WaitForExit(TimeSpan.FromSeconds(10))) { try { process.Kill(true); } catch (InvalidOperationException) { } return false; }
                return process.ExitCode == 0;
            }
            catch (Exception e) when (e is IOException or InvalidOperationException or System.ComponentModel.Win32Exception) { return false; }
        });
    }
    public HrtfConversion Convert(string sofaPath, string outputPath, string name, CancellationToken cancel = default)
    {
        var folder = directory ?? AppContext.BaseDirectory;
        var cli = new[] { "DcsVr.Cli.exe", "DcsVr.Cli" }.Select(n => Path.Combine(folder, n)).FirstOrDefault(File.Exists)
            ?? throw new HrtfConversionException("The SOFA converter (DcsVr.Cli) was not found next to DCS Control. Reinstall DCS Control.");
        var start = new ProcessStartInfo(cli) { UseShellExecute = false, CreateNoWindow = true, RedirectStandardOutput = true, RedirectStandardError = true, WorkingDirectory = Path.GetDirectoryName(cli)! };
        foreach (var a in new[] { "hrtf-convert", sofaPath, outputPath, "--name", name }) start.ArgumentList.Add(a);
        using var process = Process.Start(start) ?? throw new HrtfConversionException("The SOFA converter could not be started.");
        try { process.PriorityClass = ProcessPriorityClass.BelowNormal; } catch (Exception e) when (e is InvalidOperationException or System.ComponentModel.Win32Exception) { }
        var output = process.StandardOutput.ReadToEndAsync(cancel); var error = process.StandardError.ReadToEndAsync(cancel);
        if (!process.WaitForExit(Timeout)) { try { process.Kill(true); } catch (InvalidOperationException) { } throw new HrtfConversionException("The conversion took more than two minutes and was stopped."); }
        process.WaitForExit();
        var lines = (output.Result + "\n" + error.Result).Split('\n').Select(l => l.Trim()).Where(l => l.Length > 0).ToArray();
        if (process.ExitCode != 0 || !File.Exists(outputPath))
        {
            var reason = error.Result.Split('\n').Select(l => l.Trim()).LastOrDefault(l => l.Length > 0) ?? lines.LastOrDefault() ?? "no reason was given";
            if (reason.Contains("Unknown command", StringComparison.OrdinalIgnoreCase) || reason.StartsWith("Usage", StringComparison.OrdinalIgnoreCase))
                reason = "this DCS Control has no SOFA converter yet";
            throw new HrtfConversionException("The SOFA file was not converted: " + MidSentence(reason).TrimEnd('.') + ".");
        }
        var warnings = lines.Where(l => l.StartsWith("warning:", StringComparison.OrdinalIgnoreCase)).Select(l => l[8..].Trim()).Where(l => l.Length > 0).ToArray();
        double? coverage = lines.Select(l => l.StartsWith("coverage:", StringComparison.OrdinalIgnoreCase) && double.TryParse(l[9..].Trim().TrimEnd('%'), System.Globalization.NumberStyles.Float, System.Globalization.CultureInfo.InvariantCulture, out var c) ? c / 100 : (double?)null).LastOrDefault(c => c is not null);
        return new(warnings, coverage is { } v ? Math.Clamp(v, 0, 1) : null);
    }

    /// <summary>The converter's reason continues a sentence: its first word in lower case ("This is not a SOFA file" ->
    /// "this is not a SOFA file"), unless that word is a name or an abbreviation ("SOFA", "Data.Delay").</summary>
    internal static string MidSentence(string reason)
    {
        var end = reason.IndexOf(' ');
        var word = end < 0 ? reason : reason[..end];
        return word.Length > 1 && char.IsUpper(word[0]) && word.Skip(1).All(char.IsLower) ? char.ToLowerInvariant(word[0]) + reason[1..] : reason;
    }
}

/// <summary>The converter's test double (the tests and the offline interface verification): a "SOFA" file whose text
/// starts with "reject:" is refused with the rest of its first line as the reason; "invalid" writes a damaged file; any
/// other file becomes <see cref="HrtfFile.Synthetic"/> named after the set. <see cref="Calls"/> counts conversions.</summary>
public sealed class FixtureHrtfConverter : IHrtfConverter
{
    public int Calls { get; private set; }
    public HrtfConversion Convert(string sofaPath, string outputPath, string name, CancellationToken cancel = default)
    {
        Calls++;
        var text = File.ReadAllText(sofaPath);
        if (text.StartsWith("reject:", StringComparison.Ordinal)) throw new HrtfConversionException("The SOFA file was not converted: " + text[7..].Split('\n')[0].Trim() + ".");
        var bytes = HrtfFile.Synthetic(name);
        if (text.StartsWith("invalid", StringComparison.Ordinal)) bytes[DcsHrtfFile.HeaderBytes + 3] ^= 0x5a;
        File.WriteAllBytes(outputPath, bytes);
        return new(text.StartsWith("warn", StringComparison.Ordinal) ? ["Fixture warning: the measurement distance was assumed."] : [], 0.93);
    }
}

/// <summary>
/// The pilot's own HRTF sets, kept in the app's state folder (hrtf\&lt;name&gt;.dcshrtf with &lt;name&gt;.json beside it).
/// Imported once from a SOFA file through the converter; the SOFA file itself, or its folder, is never kept or sent
/// anywhere. Launch DCS copies every valid set into Scripts\DcsQvCull\hrtf while 3D audio is on (so another set can be
/// picked in flight), and Back to stock DCS removes them there; the library itself stays.
/// </summary>
public sealed class HrtfLibrary(string stateRoot)
{
    public const int MaxSets = 16;
    /// <summary>What a set's name may be (Binaural.IsValidHrtfName), in the page's words.</summary>
    public const string NameRule = "Use a name of 1 to 64 letters, digits, spaces, dashes, dots or underscores, not \"default\".";
    public string Folder { get; } = Path.Combine(Path.GetFullPath(stateRoot), "hrtf");
    /// <summary>Sets already read, by path: kept while the file and its sidecar have the same size and time (List runs with
    /// every state reply of the page; reading and checking a 9 MB set each time is not needed).</summary>
    private static readonly System.Collections.Concurrent.ConcurrentDictionary<string, (long Length, DateTime Written, long SideLength, DateTime SideWritten, HrtfSet Set)> Cache = new(StringComparer.OrdinalIgnoreCase);
    private sealed record Sidecar(string? FromFile, DateTimeOffset? Imported, IReadOnlyList<string>? Warnings, double? Coverage);
    private string FileOf(string name) => Path.Combine(Folder, name + Binaural.HrtfExtension);
    private string SidecarOf(string name) => Path.Combine(Folder, name + ".json");

    /// <summary>The valid sets, by name. A file the module would refuse (damaged, edited) is left out.</summary>
    public IReadOnlyList<HrtfSet> List()
    {
        if (!Directory.Exists(Folder)) return [];
        var sets = new List<HrtfSet>();
        try
        {
            foreach (var file in new DirectoryInfo(Folder).EnumerateFiles("*" + Binaural.HrtfExtension).OrderBy(f => f.Name, StringComparer.OrdinalIgnoreCase))
            {
                var name = Path.GetFileNameWithoutExtension(file.Name);
                if (!file.Name.EndsWith(Binaural.HrtfExtension, StringComparison.OrdinalIgnoreCase) || !Binaural.IsValidHrtfName(name) || name.Equals(Binaural.DefaultHrtf, StringComparison.OrdinalIgnoreCase)) continue;
                if (Read(name) is { } set) sets.Add(set);
            }
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException) { }
        return sets;
    }

    /// <summary>The set with this name, or null when it is missing or not valid.</summary>
    public HrtfSet? Read(string name)
    {
        if (!Binaural.IsValidHrtfName(name)) return null;
        try
        {
            var path = FileOf(name); var info = new FileInfo(path);
            if (!info.Exists || info.Length > DcsHrtfFile.MaxFileBytes) { Cache.TryRemove(info.FullName, out _); return null; }
            var sideInfo = new FileInfo(SidecarOf(name));
            long sideLength = sideInfo.Exists ? sideInfo.Length : -1; var sideWritten = sideInfo.Exists ? sideInfo.LastWriteTimeUtc : default;
            if (Cache.TryGetValue(info.FullName, out var cached) && cached.Length == info.Length && cached.Written == info.LastWriteTimeUtc
                && cached.SideLength == sideLength && cached.SideWritten == sideWritten && cached.Set.Name == name) return cached.Set;
            var facts = HrtfFile.Validate(File.ReadAllBytes(path), out _);
            if (facts is null) { Cache.TryRemove(info.FullName, out _); return null; }
            Sidecar? side = null;
            try { if (File.Exists(SidecarOf(name))) side = JsonData.Deserialize<Sidecar>(File.ReadAllText(SidecarOf(name))); }
            catch (Exception e) when (e is System.Text.Json.JsonException or InvalidDataException or NotSupportedException) { }
            var warnings = (side?.Warnings ?? []).Concat(facts.Warnings()).Distinct(StringComparer.Ordinal).ToArray();
            var set = new HrtfSet(name, facts.Title.Length > 0 ? facts.Title : name, facts.Source, facts.SampleRate, facts.Directions, facts.Taps, Math.Round(facts.HeadRadiusMm, 1),
                side?.FromFile, side?.Imported, warnings, info.Length, side?.Coverage);
            Cache[info.FullName] = (info.Length, info.LastWriteTimeUtc, sideLength, sideWritten, set);
            return set;
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException) { return null; }
    }

    public bool Contains(string? name) => name is not null && Read(name) is not null;

    /// <summary>The sets Launch DCS copies next to the module: every valid one, read once (the bytes deployed are the
    /// bytes validated).</summary>
    public IReadOnlyList<HrtfDeployment> ForDeployment()
    {
        var list = new List<HrtfDeployment>();
        foreach (var set in List())
        {
            try
            {
                var bytes = File.ReadAllBytes(FileOf(set.Name));
                if (HrtfFile.Validate(bytes, out _) is not null) list.Add(new(set.Name, bytes));
            }
            catch (Exception e) when (e is IOException or UnauthorizedAccessException) { }
        }
        return list;
    }

    /// <summary>A name free in the library: the wanted one, else with " 2", " 3" … added.</summary>
    public string FreeName(string wanted)
    {
        var name = Binaural.NameFrom(wanted);
        if (!Exists(name)) return name;
        for (var i = 2; i < 1000; i++)
        {
            var suffix = " " + i; var candidate = (name.Length + suffix.Length > Binaural.MaxNameLength ? name[..(Binaural.MaxNameLength - suffix.Length)].TrimEnd() : name) + suffix;
            if (!Exists(candidate)) return candidate;
        }
        throw new InvalidOperationException("Too many sets with this name.");
    }
    private bool Exists(string name) => File.Exists(FileOf(name)) || name.Equals(Binaural.DefaultHrtf, StringComparison.OrdinalIgnoreCase);

    /// <summary>Converts <paramref name="sofaPath"/> into the set <paramref name="name"/>. Refused, nothing is kept: a
    /// name taken (<see cref="HrtfNameTakenException"/>, the page asks for another), a library already full, a
    /// conversion the converter refused, or a result the module would refuse.</summary>
    public HrtfSet Import(string sofaPath, string name, IHrtfConverter converter, CancellationToken cancel = default)
    {
        if (!Binaural.IsValidHrtfName(name) || name.Equals(Binaural.DefaultHrtf, StringComparison.OrdinalIgnoreCase))
            throw new InvalidDataException(NameRule);
        if (Exists(name)) throw new HrtfNameTakenException(name, FreeName(name));
        if (List().Count >= MaxSets) throw new InvalidOperationException($"You have {MaxSets} HRTF sets. Remove one first.");
        var info = new FileInfo(sofaPath);
        if (!info.Exists) throw new FileNotFoundException("The SOFA file was not found.");
        if (!info.Extension.Equals(".sofa", StringComparison.OrdinalIgnoreCase)) throw new InvalidDataException("Choose a .sofa file.");
        if (info.Length > 512L * 1024 * 1024) throw new InvalidDataException("The SOFA file is larger than 512 MB.");
        Directory.CreateDirectory(Folder);
        var temp = Path.Combine(Folder, ".import-" + Guid.NewGuid().ToString("N") + ".tmp");
        try
        {
            var conversion = converter.Convert(info.FullName, temp, name, cancel);
            var length = File.Exists(temp) ? new FileInfo(temp).Length : 0;
            if (length == 0 || length > DcsHrtfFile.MaxFileBytes) throw new HrtfConversionException("The converter wrote no usable file.");
            var bytes = File.ReadAllBytes(temp);
            if (HrtfFile.Validate(bytes, out var why) is null) throw new HrtfConversionException($"The converted file is not valid ({why}), so it is not used.");
            AtomicFile.WriteText(SidecarOf(name), JsonData.Serialize(new Sidecar(info.Name, DateTimeOffset.Now, conversion.Warnings, conversion.Coverage)));
            File.Move(temp, FileOf(name), false);
            return Read(name) ?? throw new HrtfConversionException("The converted file could not be read back.");
        }
        catch
        {
            if (!File.Exists(FileOf(name))) try { File.Delete(SidecarOf(name)); } catch (Exception e) when (e is IOException or UnauthorizedAccessException) { }
            throw;
        }
        finally { try { File.Delete(temp); } catch (Exception e) when (e is IOException or UnauthorizedAccessException) { } }
    }

    /// <summary>Removes a set from the library (a copy Launch DCS installed goes at the next launch or Back to stock).</summary>
    public void Delete(string name)
    {
        if (!Binaural.IsValidHrtfName(name) || name.Equals(Binaural.DefaultHrtf, StringComparison.OrdinalIgnoreCase)) throw new InvalidDataException("The generic set cannot be removed.");
        File.Delete(FileOf(name)); File.Delete(SidecarOf(name));
    }
}

/// <summary>The name is already a set in the library: <see cref="Suggested"/> is a free one.</summary>
public sealed class HrtfNameTakenException(string name, string suggested) : Exception($"You already have an HRTF set named \"{name}\".")
{
    public string Name { get; } = name;
    public string Suggested { get; } = suggested;
}
