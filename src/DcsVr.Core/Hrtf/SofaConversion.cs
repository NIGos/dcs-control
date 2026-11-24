using System.Globalization;

namespace DcsVr.Core.Hrtf;

/// <summary>A SOFA file converted: the .dcshrtf, the reader's and the converter's warnings together, and the share of
/// the sphere the grid covers (0 to 1).</summary>
public sealed record SofaConversionResult(HrtfConvertedSet Converted, string Convention, IReadOnlyList<string> Warnings, double Coverage);

/// <summary>
/// SOFA -> .dcshrtf in one call (<see cref="SofaReader"/>, then <see cref="HrtfConverter"/>), and the CLI command behind
/// it, <c>DcsVr.Cli hrtf-convert &lt;in.sofa&gt; &lt;out.dcshrtf&gt; [--name N] [--source S] [--generic]</c>, whose
/// contract the app (<see cref="DcsVr.Core.CliHrtfConverter"/>) and scripts/fetch-hrtf.ps1 rely on
/// (docs/BINAURAL_AUDIO.md section 10):
///   - exit 0: the file is written (whole: written beside the target, then moved over it); stdout has one
///     "warning: &lt;text&gt;" line per warning, "coverage: &lt;percent&gt;%" and a summary line;
///   - exit 2: the file was refused; the last stderr line is the reason, in plain words; nothing is written;
///   - exit 1: wrong arguments or a file that cannot be read or written (the reason on stderr);
///   - <c>hrtf-convert --check</c>: exit 0 when the converter and its HDF5 reader are present.
/// Without --generic the set is marked personal. --name defaults to the SOFA file's name, --source to the database and
/// subject the file names (never its path).
/// </summary>
public static class SofaConversion
{
    /// <summary>A direction of the sphere counts as covered when a measured one lies within this angle (the converter
    /// calls a grid sparse above the same spacing).</summary>
    public const double CoverageDegrees = 15;

    public static SofaConversionResult Convert(string sofaPath, string? name = null, string? source = null, bool generic = false)
    {
        var read = SofaReader.Read(sofaPath, name, source);
        var converted = HrtfConverter.Convert(read.Set, personal: !generic);
        var warnings = read.Warnings.Concat(converted.Report.Warnings).Distinct(StringComparer.Ordinal).ToArray();
        return new(converted, read.Convention, warnings, Coverage(DcsHrtfFile.Read(converted.File).Directions));
    }

    /// <summary>The share of 4,000 evenly spread directions (a Fibonacci sphere) that have a measured direction within
    /// <see cref="CoverageDegrees"/>. Directions: x, y, z per direction, unit length.</summary>
    public static double Coverage(float[] directions)
    {
        const int probes = 4000;
        var n = directions.Length / 3;
        if (n == 0) return 0;
        var cos = Math.Cos(CoverageDegrees * Math.PI / 180);
        var covered = 0;
        for (var i = 0; i < probes; i++)
        {
            var y = 1 - 2 * (i + 0.5) / probes;
            var r = Math.Sqrt(1 - y * y);
            var phi = i * Math.PI * (3 - Math.Sqrt(5));
            double px = r * Math.Cos(phi), pz = r * Math.Sin(phi);
            for (var j = 0; j < n; j++)
                if (px * directions[j * 3] + y * directions[j * 3 + 1] + pz * directions[j * 3 + 2] >= cos) { covered++; break; }
        }
        return (double)covered / probes;
    }

    // Its own method, not inlined: a missing PureHDF.dll fails here, inside the caller's try, not when RunCli is compiled.
    [System.Runtime.CompilerServices.MethodImpl(System.Runtime.CompilerServices.MethodImplOptions.NoInlining)]
    static string? ReaderVersion() =>
        System.Reflection.CustomAttributeExtensions.GetCustomAttribute<System.Reflection.AssemblyInformationalVersionAttribute>(typeof(PureHDF.H5File).Assembly)?.InformationalVersion.Split('+')[0]
        ?? typeof(PureHDF.H5File).Assembly.GetName().Version?.ToString();

    /// <summary>The CLI command; <paramref name="args"/> are the arguments after "hrtf-convert".</summary>
    public static int RunCli(IReadOnlyList<string> args, TextWriter output, TextWriter error)
    {
        if (args.Contains("--check"))
        {
            // The HDF5 reader is a separate assembly: a CLI without it must say no here, not fail on the first import.
            try { output.WriteLine("hrtf-convert: ready (PureHDF " + ReaderVersion() + ")"); return 0; }
            catch (Exception e) when (e is FileNotFoundException or FileLoadException or TypeLoadException) { error.WriteLine("The SOFA reader (PureHDF) is missing: " + e.Message); return 1; }
        }
        string? name = null, source = null;
        var generic = false;
        var positional = new List<string>();
        for (var i = 0; i < args.Count; i++)
        {
            switch (args[i])
            {
                case "--name" when i + 1 < args.Count: name = args[++i]; break;
                case "--source" when i + 1 < args.Count: source = args[++i]; break;
                case "--generic": generic = true; break;
                default:
                    if (args[i].StartsWith("--", StringComparison.Ordinal)) { error.WriteLine("hrtf-convert does not know the option " + args[i] + "."); return 1; }
                    positional.Add(args[i]); break;
            }
        }
        if (positional.Count != 2) { error.WriteLine("hrtf-convert needs a SOFA file and an output file: hrtf-convert <in.sofa> <out.dcshrtf> [--name N] [--source S] [--generic]"); return 1; }
        string input = positional[0], target = positional[1];
        if (name is not null && name.Trim().Length == 0) { error.WriteLine("The set needs a name."); return 1; }
        try
        {
            if (!File.Exists(input)) { error.WriteLine("The SOFA file was not found."); return 1; }
            if (string.Equals(Path.GetFullPath(input), Path.GetFullPath(target), StringComparison.OrdinalIgnoreCase)) { error.WriteLine("The output would overwrite the SOFA file."); return 1; }
            var result = Convert(input, name, source, generic);
            var full = Path.GetFullPath(target);
            Directory.CreateDirectory(Path.GetDirectoryName(full)!);
            var part = full + ".part";
            File.WriteAllBytes(part, result.Converted.File);
            File.Move(part, full, overwrite: true);
            foreach (var w in result.Warnings) output.WriteLine("warning: " + w);
            var r = result.Converted.Report;
            output.WriteLine(string.Format(CultureInfo.InvariantCulture, "coverage: {0:0.0}%", result.Coverage * 100));
            output.WriteLine(string.Format(CultureInfo.InvariantCulture,
                "converted: {0} directions, {1:0} Hz, {2} taps, fitted head radius {3:0.00} cm, elevation {4:0} to {5:0} degrees, {6} bytes ({7})",
                r.Directions, r.SampleRate, r.Taps, r.FittedHeadRadiusM * 100, r.MinElevationDeg, r.MaxElevationDeg, result.Converted.File.Length, result.Convention));
            return 0;
        }
        catch (HrtfConversionException e) { error.WriteLine(e.Message); return 2; }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException or ArgumentException or NotSupportedException)
        {
            try { File.Delete(Path.GetFullPath(target) + ".part"); } catch (Exception cleanup) when (cleanup is IOException or UnauthorizedAccessException or ArgumentException or NotSupportedException) { }
            error.WriteLine(e.Message.Split('\n')[0].Trim()); return 1;
        }
    }
}
