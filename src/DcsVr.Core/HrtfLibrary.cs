using System.Buffers.Binary;
using System.Diagnostics;
using System.Text;

namespace DcsVr.Core;

/// <summary>
/// The .dcshrtf v1 format (native/binaural/include/hrtf_format.h) as far as the app needs it: every check of the
/// header's Validate(), so a file the module would refuse is never offered or deployed, and the facts the page shows.
/// The app never writes the format itself: the converter does (task A, <see cref="IHrtfConverter"/>).
/// </summary>
public static class HrtfFile
{
    public const uint Magic = 0x54524844; // "DHRT"
    public const ushort Version = 1, HeaderBytes = 256;
    public const uint MinSampleRate = 22050, MaxSampleRate = 192000, MinDirections = 6, MaxDirections = 16384;
    public const ushort MinTaps = 16, MaxTaps = 512;
    public const long MaxFileBytes = 64L << 20;
    public const uint FlagMinimumPhase = 1, FlagDiffuseFieldEq = 2, FlagFrontNormalised = 4, FlagPersonal = 8, KnownFlags = 15;

    public static ulong Fnv1a(ReadOnlySpan<byte> data, ulong h = 0xcbf29ce484222325)
    {
        foreach (var b in data) { h ^= b; h *= 0x100000001b3; }
        return h;
    }
    public static ulong PayloadBytes(uint directions, uint triangles, uint taps) =>
        (ulong)directions * (16 + 8) + (ulong)triangles * 12 + (ulong)directions * 2 * taps * 4;
    /// <summary>The header checksum: FNV-1a 64 of the 256 header bytes with the checksum field (offset 56) zero.</summary>
    public static ulong HeaderChecksum(ReadOnlySpan<byte> header)
    {
        Span<byte> copy = stackalloc byte[HeaderBytes];
        header[..HeaderBytes].CopyTo(copy);
        copy.Slice(56, 8).Clear();
        return Fnv1a(copy);
    }

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
        uint n = (uint)dirs.Count, t = n - 2;
        var payload = PayloadBytes(n, t, taps);
        var bytes = new byte[HeaderBytes + (long)payload];
        var span = bytes.AsSpan();
        int o = HeaderBytes;
        foreach (var d in dirs) { BinaryPrimitives.WriteSingleLittleEndian(span[o..], d.X); BinaryPrimitives.WriteSingleLittleEndian(span[(o + 4)..], d.Y); BinaryPrimitives.WriteSingleLittleEndian(span[(o + 8)..], d.Z); o += 16; }
        foreach (var d in dirs)
        {
            // Lateral angle from the median plane; +z is right, so the right ear leads for z > 0.
            var lateral = Math.Asin(Math.Clamp(d.Z, -1f, 1f));
            var itd = headRadiusM / 343.0 * (Math.Abs(lateral) + Math.Sin(Math.Abs(lateral))) * sampleRate;
            float lead = 8, lag = (float)(8 + itd);
            BinaryPrimitives.WriteSingleLittleEndian(span[o..], lateral > 0 ? lag : lead); BinaryPrimitives.WriteSingleLittleEndian(span[(o + 4)..], lateral > 0 ? lead : lag); o += 8;
        }
        for (uint i = 1; i + 1 < n; i++) { BinaryPrimitives.WriteUInt32LittleEndian(span[o..], 0); BinaryPrimitives.WriteUInt32LittleEndian(span[(o + 4)..], i); BinaryPrimitives.WriteUInt32LittleEndian(span[(o + 8)..], i + 1); o += 12; }
        foreach (var d in dirs)
            for (var ear = 0; ear < 2; ear++)
            {
                var side = ear == 0 ? -d.Z : d.Z; // towards this ear: louder
                var gain = (float)Math.Pow(10, 6 * side / 20);
                for (var k = 0; k < taps; k++) { BinaryPrimitives.WriteSingleLittleEndian(span[o..], k == 0 ? gain : k == 1 ? -0.25f * gain : 0); o += 4; }
            }
        BinaryPrimitives.WriteUInt32LittleEndian(span, Magic); BinaryPrimitives.WriteUInt16LittleEndian(span[4..], Version); BinaryPrimitives.WriteUInt16LittleEndian(span[6..], HeaderBytes);
        BinaryPrimitives.WriteUInt32LittleEndian(span[8..], FlagMinimumPhase | FlagFrontNormalised | (personal ? FlagPersonal : 0));
        BinaryPrimitives.WriteUInt32LittleEndian(span[12..], sampleRate); BinaryPrimitives.WriteUInt32LittleEndian(span[16..], n); BinaryPrimitives.WriteUInt32LittleEndian(span[20..], t);
        BinaryPrimitives.WriteUInt16LittleEndian(span[24..], taps); BinaryPrimitives.WriteSingleLittleEndian(span[28..], (float)headRadiusM);
        BinaryPrimitives.WriteSingleLittleEndian(span[32..], 1.2f); BinaryPrimitives.WriteSingleLittleEndian(span[36..], 1f);
        BinaryPrimitives.WriteUInt64LittleEndian(span[40..], payload); BinaryPrimitives.WriteUInt64LittleEndian(span[48..], Fnv1a(span[HeaderBytes..]));
        var name = Encoding.UTF8.GetBytes(title); name.AsSpan(0, Math.Min(name.Length, 63)).CopyTo(span[64..]);
        Encoding.UTF8.GetBytes("Synthetic spherical head (test fixture)").CopyTo(span[128..]);
        BinaryPrimitives.WriteUInt64LittleEndian(span[56..], HeaderChecksum(span));
        return bytes;
    }

    /// <summary>hrtf_format.h Validate(), byte for byte the same checks in the same order: null and the facts when the
    /// file is valid, else the first problem in <paramref name="why"/> (the header's own wording).</summary>
    public static HrtfFacts? Validate(byte[] data, out string? why)
    {
        why = null;
        if (data.Length < HeaderBytes) { why = "too small"; return null; }
        if (data.Length > MaxFileBytes) { why = "too large"; return null; }
        uint U32(int o) => BinaryPrimitives.ReadUInt32LittleEndian(data.AsSpan(o));
        ushort U16(int o) => BinaryPrimitives.ReadUInt16LittleEndian(data.AsSpan(o));
        ulong U64(int o) => BinaryPrimitives.ReadUInt64LittleEndian(data.AsSpan(o));
        float F32(int o) => BinaryPrimitives.ReadSingleLittleEndian(data.AsSpan(o));
        if (U32(0) != Magic) { why = "not an HRTF file (magic)"; return null; }
        if (U16(4) != Version) { why = "unsupported version"; return null; }
        if (U16(6) != HeaderBytes) { why = "header size"; return null; }
        if (HeaderChecksum(data) != U64(56)) { why = "header checksum"; return null; }
        uint flags = U32(8), rate = U32(12), n = U32(16), t = U32(20); ushort taps = U16(24);
        float radius = F32(28), distance = F32(32), frontGain = F32(36);
        if ((flags & ~KnownFlags) != 0) { why = "unknown flags"; return null; }
        if ((flags & FlagMinimumPhase) == 0) { why = "not minimum phase"; return null; }
        if (rate is < MinSampleRate or > MaxSampleRate) { why = "sample rate"; return null; }
        if (n is < MinDirections or > MaxDirections) { why = "direction count"; return null; }
        if (t < 4 || t > 2 * n) { why = "triangle count"; return null; }
        if (taps is < MinTaps or > MaxTaps) { why = "tap count"; return null; }
        if (!(radius >= 0.04f && radius <= 0.15f)) { why = "head radius"; return null; }
        if (!(distance >= 0.1f && distance <= 10.0f)) { why = "measurement distance"; return null; }
        if (!(frontGain > 0) || !float.IsFinite(frontGain)) { why = "front gain"; return null; }
        if (data.AsSpan(64, 64).IndexOf((byte)0) < 0 || data.AsSpan(128, 96).IndexOf((byte)0) < 0) { why = "strings"; return null; }
        if (U16(26) != 0) { why = "reserved field"; return null; }
        var payload = PayloadBytes(n, t, taps);
        if (U64(40) != payload) { why = "payload size field"; return null; }
        if ((ulong)data.Length != HeaderBytes + payload) { why = "file size"; return null; }
        if (Fnv1a(data.AsSpan(HeaderBytes)) != U64(48)) { why = "payload checksum"; return null; }
        int directions = HeaderBytes, delays = directions + (int)n * 16, triangles = delays + (int)n * 8, ir = triangles + (int)t * 12;
        var maxDelay = 0.01f * rate;
        var horizontal = 0; var up = false; var down = false;
        for (var i = 0; i < n; i++)
        {
            float x = F32(directions + i * 16), y = F32(directions + i * 16 + 4), z = F32(directions + i * 16 + 8);
            if (!float.IsFinite(x) || !float.IsFinite(y) || !float.IsFinite(z)) { why = "direction not finite"; return null; }
            if (MathF.Abs(x * x + y * y + z * z - 1.0f) > 1e-3f) { why = "direction not unit"; return null; }
            float left = F32(delays + i * 8), right = F32(delays + i * 8 + 4);
            if (!(left >= 0 && left <= maxDelay && right >= 0 && right <= maxDelay)) { why = "delay"; return null; }
            if (MathF.Abs(y) < 0.05f) horizontal++;
            up |= y > 0.7f; down |= y < -0.5f;
        }
        for (var i = 0; i < t; i++)
        {
            uint a = U32(triangles + i * 12), b = U32(triangles + i * 12 + 4), c = U32(triangles + i * 12 + 8);
            if (a >= n || b >= n || c >= n || a == b || b == c || a == c) { why = "triangle index"; return null; }
        }
        var irCount = (long)n * 2 * taps;
        for (long i = 0; i < irCount; i++)
        {
            var v = F32(ir + (int)(i * 4));
            if (!float.IsFinite(v) || MathF.Abs(v) > 64.0f) { why = "impulse response value"; return null; }
        }
        static string Text(ReadOnlySpan<byte> field) { var end = field.IndexOf((byte)0); return Encoding.UTF8.GetString(field[..(end < 0 ? field.Length : end)]).Trim(); }
        return new(Text(data.AsSpan(64, 64)), Text(data.AsSpan(128, 96)), (int)rate, (int)n, (int)t, taps, radius * 1000.0, distance, (flags & FlagPersonal) != 0,
            (flags & FlagDiffuseFieldEq) != 0, (flags & FlagFrontNormalised) != 0, horizontal, up, down);
    }
}

/// <summary>What a valid .dcshrtf holds: its own name and source text, the sample rate, the measurement grid, the IR
/// length, the head radius fitted to its delays (mm) and the measurement distance (m). <paramref name="Horizontal"/>:
/// directions within about 3° of the horizontal plane; <paramref name="Above"/> / <paramref name="Below"/>: the grid
/// reaches well above (45°) and below (30°).</summary>
public sealed record HrtfFacts(string Title, string Source, int SampleRate, int Directions, int Triangles, int Taps, double HeadRadiusMm, double DistanceM,
    bool Personal, bool DiffuseFieldEq, bool FrontNormalised, int Horizontal, bool Above, bool Below)
{
    /// <summary>The plain warnings the page shows with the conversion report.</summary>
    public IReadOnlyList<string> Warnings()
    {
        var list = new List<string>();
        if (!Above || !Below) list.Add(!Above && !Below ? "The measurements cover little above or below you: height will sound vague." : !Above ? "The measurements cover little above you: sounds overhead will sound vague." : "The measurements cover little below you.");
        if (Directions < 100) list.Add("Few measured directions: directions between them are guessed.");
        if (Horizontal < 12) list.Add("Few measurements around you at ear height: left, right and behind may be less exact.");
        if (HeadRadiusMm is < 70 or > 110) list.Add($"The head size in the file ({2 * Math.PI * HeadRadiusMm / 10:0} cm around) is unusual. Check that it is your own measurement.");
        return list;
    }
}

/// <summary>One HRTF set in the app's library: the file name (the [Audio] Hrtf value) and its facts, where it came from
/// (the SOFA file's name only, never its folder), when it was imported and what the converter warned about.</summary>
public sealed record HrtfSet(string Name, string Title, string Source, int SampleRate, int Directions, int Taps, double HeadRadiusMm, string? FromFile,
    DateTimeOffset? Imported, IReadOnlyList<string> Warnings, long Bytes, double? Coverage = null);

/// <summary>A set the plan deploys: Scripts\DcsQvCull\hrtf\&lt;Name&gt;.dcshrtf.</summary>
public sealed record HrtfDeployment(string Name, byte[] Content);

/// <summary>The SOFA converter (docs/BINAURAL_AUDIO.md section 4.2, task A: src/DcsVr.Core/Hrtf and the CLI command
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
/// next to the app (task A adds the command). Exit 0 with the file written is a conversion; lines starting with
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
            var reason = error.Result.Split('\n').Select(l => l.Trim()).LastOrDefault(l => l.Length > 0) ?? lines.LastOrDefault() ?? "the converter gave no reason";
            if (reason.Contains("Unknown command", StringComparison.OrdinalIgnoreCase) || reason.StartsWith("Usage", StringComparison.OrdinalIgnoreCase))
                reason = "this DCS Control has no SOFA converter yet";
            throw new HrtfConversionException("The SOFA file was not converted: " + reason.TrimEnd('.') + ".");
        }
        var warnings = lines.Where(l => l.StartsWith("warning:", StringComparison.OrdinalIgnoreCase)).Select(l => l[8..].Trim()).Where(l => l.Length > 0).ToArray();
        double? coverage = lines.Select(l => l.StartsWith("coverage:", StringComparison.OrdinalIgnoreCase) && double.TryParse(l[9..].Trim().TrimEnd('%'), System.Globalization.NumberStyles.Float, System.Globalization.CultureInfo.InvariantCulture, out var c) ? c / 100 : (double?)null).LastOrDefault(c => c is not null);
        return new(warnings, coverage is { } v ? Math.Clamp(v, 0, 1) : null);
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
        if (text.StartsWith("invalid", StringComparison.Ordinal)) bytes[HrtfFile.HeaderBytes + 3] ^= 0x5a;
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
    public string Folder { get; } = Path.Combine(Path.GetFullPath(stateRoot), "hrtf");
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
            if (!info.Exists || info.Length > HrtfFile.MaxFileBytes) return null;
            var facts = HrtfFile.Validate(File.ReadAllBytes(path), out _);
            if (facts is null) return null;
            Sidecar? side = null;
            try { if (File.Exists(SidecarOf(name))) side = JsonData.Deserialize<Sidecar>(File.ReadAllText(SidecarOf(name))); }
            catch (Exception e) when (e is System.Text.Json.JsonException or InvalidDataException or NotSupportedException) { }
            var warnings = (side?.Warnings ?? []).Concat(facts.Warnings()).Distinct(StringComparer.Ordinal).ToArray();
            return new(name, facts.Title.Length > 0 ? facts.Title : name, facts.Source, facts.SampleRate, facts.Directions, facts.Taps, Math.Round(facts.HeadRadiusMm, 1),
                side?.FromFile, side?.Imported, warnings, info.Length, side?.Coverage);
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
            throw new InvalidDataException("Use a name of 1 to 64 letters, digits, spaces, dashes, dots or underscores, not \"default\".");
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
            if (length == 0 || length > HrtfFile.MaxFileBytes) throw new HrtfConversionException("The converter wrote no usable file.");
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
