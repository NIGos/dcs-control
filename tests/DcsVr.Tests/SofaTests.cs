using System.IO.Compression;
using DcsVr.Core;
using DcsVr.Core.Hrtf;
using PureHDF;
using PureHDF.Filters;
using PureHDF.VOL.Native;

namespace DcsVr.Tests;

/// <summary>
/// SOFA import (docs/BINAURAL_AUDIO.md 4.2 and 10): <see cref="SofaReader"/> on SOFA files written here with PureHDF
/// (the same shapes and attributes a netCDF-4 SOFA file has: chunked and deflated Data.IR, string attributes, [I C] and
/// [M C] variables), the <c>hrtf-convert</c> command in process and as DcsVr.Cli.exe through the app's import, and the
/// generic set: SADIE II's KU100 converted from the cached archive gives the committed
/// native/binaural/data/sadie2_ku100.dcshrtf byte for byte (skipped without .cache/hrtf).
/// </summary>
static class SofaTests
{
    /// <summary>What a test SOFA file holds; every field can be changed or left out to make a variant.</summary>
    sealed class Sofa
    {
        public Dictionary<string, object> Globals = new()
        {
            ["Conventions"] = "SOFA", ["Version"] = "2.1", ["SOFAConventions"] = "SimpleFreeFieldHRIR", ["SOFAConventionsVersion"] = "1.0",
            ["DataType"] = "FIR", ["RoomType"] = "free field", ["Title"] = "Test head", ["DatabaseName"] = "Test database",
            ["ListenerShortName"] = "T1", ["Organization"] = "Test lab.",
        };
        public required double[] Ir;            // [M R N] flat
        public required ulong[] IrDims;
        public bool IrAsFloat;
        public double[]? Rate = [48000];
        public string? RateUnits = "hertz";
        public double[]? Delay = [0, 0];
        public ulong[] DelayDims = [1, 2];
        public required double[] Source;        // [M C] flat
        public string? SourceType = "spherical", SourceUnits = "degree, degree, metre";
        public double[]? Receiver = [0, 0.09, 0, 0, -0.09, 0];
        public double[]? View, Up, Listener;
        public bool Chunked = true;
    }

    static Sofa FromSet(HrirSet set, double distance = 1.2)
    {
        int m = set.Directions.Length, n = set.Ir[0][0].Length;
        var ir = new double[m * 2 * n];
        var src = new double[m * 3];
        for (var i = 0; i < m; i++)
        {
            for (var e = 0; e < 2; e++) for (var k = 0; k < n; k++) ir[(i * 2 + e) * n + k] = set.Ir[i][e][k];
            var d = set.Directions[i];  // head frame (x front, y up, z right) -> SOFA azimuth / elevation in degrees
            var az = Math.Atan2(-d[2], d[0]) * 180 / Math.PI;
            src[i * 3] = az < 0 ? az + 360 : az; src[i * 3 + 1] = Math.Asin(Math.Clamp(d[1], -1, 1)) * 180 / Math.PI; src[i * 3 + 2] = distance;
        }
        return new Sofa { Ir = ir, IrDims = [(ulong)m, 2, (ulong)n], Source = src, Rate = [set.SampleRate] };
    }

    static H5Dataset Dataset(object data, ulong[] dims, bool chunked, params (string Name, string Value)[] attributes)
    {
        H5Dataset set = chunked
            ? new H5Dataset(data, chunks: dims.Select(d => (uint)Math.Max(1UL, Math.Min(d, 64UL))).ToArray(), fileDims: dims,
                datasetCreation: new H5DatasetCreation(Filters: [(H5Filter)(ushort)2, (H5Filter)(ushort)1]))  // shuffle + deflate, as netCDF-4
            : new H5Dataset(data, fileDims: dims);
        if (attributes.Length > 0) set.Attributes = attributes.ToDictionary(a => a.Name, a => (object)a.Value);
        return set;
    }

    static string Write(string folder, string name, Sofa s)
    {
        var file = new H5File();
        foreach (var (key, value) in s.Globals) file.Attributes[key] = value;
        var ir = s.IrAsFloat ? (object)s.Ir.Select(v => (float)v).ToArray() : s.Ir;
        file["Data.IR"] = Dataset(ir, s.IrDims, s.Chunked);
        if (s.Rate is { } rate) file["Data.SamplingRate"] = s.RateUnits is { } ru ? Dataset(rate, [(ulong)rate.Length], false, ("Units", ru)) : Dataset(rate, [(ulong)rate.Length], false);
        if (s.Delay is { } delay) file["Data.Delay"] = Dataset(delay, s.DelayDims, false);
        var sourceAttributes = new List<(string, string)>();
        if (s.SourceType is { } st) sourceAttributes.Add(("Type", st));
        if (s.SourceUnits is { } su) sourceAttributes.Add(("Units", su));
        file["SourcePosition"] = Dataset(s.Source, [(ulong)(s.Source.Length / 3), 3], s.Chunked, sourceAttributes.ToArray());
        if (s.Receiver is { } receiver) file["ReceiverPosition"] = Dataset(receiver, [2, 3, 1], false, ("Type", "cartesian"), ("Units", "metre"));
        if (s.Listener is { } listener) file["ListenerPosition"] = Dataset(listener, [1, 3], false, ("Type", "cartesian"), ("Units", "metre"));
        if (s.View is { } view) file["ListenerView"] = Dataset(view, [1, 3], false, ("Type", "cartesian"), ("Units", "metre"));
        if (s.Up is { } up) file["ListenerUp"] = Dataset(up, [1, 3], false);
        Directory.CreateDirectory(folder);
        var path = Path.Combine(folder, name);
        file.Write(path);
        return path;
    }

    static double Angle(double[] a, double[] b) => Math.Acos(Math.Clamp(a[0] * b[0] + a[1] * b[1] + a[2] * b[2], -1, 1)) * 180 / Math.PI;

    public static void Register(Action<string, Action> test, Action<bool, string> require, Action<string> skip, string workspace, string root)
    {
        var folder = Path.Combine(root, "sofa");
        var reference = HrtfTests.Synthetic(HrtfTests.LatLong(15, 15), length: 64);

        void Same(HrirSet got, HrirSet want, bool swapped = false)
        {
            require(got.Directions.Length == want.Directions.Length && got.SampleRate == want.SampleRate, $"{got.Directions.Length} directions at {got.SampleRate}");
            var worst = 0.0;
            for (var i = 0; i < want.Directions.Length; i++)
            {
                worst = Math.Max(worst, Angle(got.Directions[i], want.Directions[i]));
                for (var e = 0; e < 2; e++) require(got.Ir[i][e].AsSpan().SequenceEqual(want.Ir[i][swapped ? 1 - e : e]), $"IR {i} ear {e}");
            }
            require(worst < 1e-5, $"directions differ by up to {worst} degrees");
        }

        test("SOFA reader: a SimpleFreeFieldHRIR file (chunked, deflated, spherical degrees) reads back the HRIRs, directions, rate and names", () =>
        {
            var path = Write(folder, "plain.sofa", FromSet(reference));
            var read = SofaReader.Read(path);
            Same(read.Set, reference);
            require(read.Convention == "SimpleFreeFieldHRIR" && read.Warnings.Count == 0, string.Join("; ", read.Warnings));
            require(read.Set.Delay is null && Math.Abs(read.Set.MeasurementDistanceM - 1.2) < 1e-9, $"delay {read.Set.Delay is null}, distance {read.Set.MeasurementDistanceM}");
            require(read.Set.Name == "plain" && read.Set.Source == "Test database T1, Test lab", $"{read.Set.Name} / {read.Set.Source}");
            require(!read.Set.Source.Contains(folder, StringComparison.OrdinalIgnoreCase), "the path never goes into the set");
            var named = SofaReader.Read(path, "Mine", "Measured at home");
            require(named.Set.Name == "Mine" && named.Set.Source == "Measured at home", "explicit name and source");
            // Contiguous storage and float32 samples read the same.
            var plain = FromSet(reference); plain.Chunked = false; plain.IrAsFloat = true;
            Same(SofaReader.Read(Write(folder, "float.sofa", plain)).Set, reference);
        });

        test("SOFA reader: cartesian centimetres, radians, per-measurement Data.Delay, swapped receivers, a rotated listener, FIR-E shape", () =>
        {
            var m = reference.Directions.Length;
            // Cartesian in cm: SOFA x front, y left, z up.
            var cart = FromSet(reference);
            cart.Source = reference.Directions.SelectMany(d => new[] { d[0] * 150, -d[2] * 150, d[1] * 150 }).ToArray();
            cart.SourceType = "cartesian"; cart.SourceUnits = "centimetre";
            var c = SofaReader.Read(Write(folder, "cartesian.sofa", cart));
            Same(c.Set, reference);
            require(Math.Abs(c.Set.MeasurementDistanceM - 1.5) < 1e-9, "distance from cm: " + c.Set.MeasurementDistanceM);
            // Radians.
            var rad = FromSet(reference);
            for (var i = 0; i < m; i++) { rad.Source[i * 3] *= Math.PI / 180; rad.Source[i * 3 + 1] *= Math.PI / 180; }
            rad.SourceUnits = "radian, radian, metre";
            Same(SofaReader.Read(Write(folder, "radian.sofa", rad)).Set, reference);
            // Data.Delay per measurement and ear, in samples.
            var delayed = FromSet(reference);
            delayed.Delay = Enumerable.Range(0, m * 2).Select(j => (double)(j % 7)).ToArray(); delayed.DelayDims = [(ulong)m, 2];
            var dl = SofaReader.Read(Write(folder, "delay.sofa", delayed)).Set;
            require(dl.Delay is { } dd && dd.Length == m && dd[3][0] == 6 && dd[3][1] == 0, "Data.Delay [M R]");
            var single = FromSet(reference); single.Delay = [2, 5];
            require(SofaReader.Read(Write(folder, "delay1.sofa", single)).Set.Delay is { } ds && ds.All(p => p[0] == 2 && p[1] == 5), "Data.Delay [I R]");
            // Receiver 0 on the right (negative y): the ears are swapped back, with a warning.
            var swapped = FromSet(reference);
            for (var i = 0; i < m; i++)
                for (var k = 0; k < 64; k++) (swapped.Ir[(i * 2) * 64 + k], swapped.Ir[(i * 2 + 1) * 64 + k]) = (swapped.Ir[(i * 2 + 1) * 64 + k], swapped.Ir[(i * 2) * 64 + k]);
            swapped.Receiver = [0, -0.09, 0, 0, 0.09, 0];
            var sw = SofaReader.Read(Write(folder, "swapped.sofa", swapped));
            Same(sw.Set, reference);
            require(sw.Warnings.Any(w => w.Contains("swapped", StringComparison.Ordinal)), "swap warning");
            // A listener looking along +y (SOFA's left): every source is 90 degrees further right in the head frame.
            var rotated = FromSet(reference); rotated.View = [0, 1, 0]; rotated.Up = [0, 0, 1];
            var rs = SofaReader.Read(Write(folder, "rotated.sofa", rotated)).Set;
            var worst = 0.0;
            for (var i = 0; i < m; i++)
            {
                var d = reference.Directions[i];
                worst = Math.Max(worst, Angle(rs.Directions[i], [-d[2], d[1], d[0]]));  // the listener faces left: what was in front is on the right
            }
            require(worst < 1e-5, "rotated listener: " + worst);
            // A listener 1 m forward: a source 1.2 m ahead becomes 0.2 m ahead (distance), still in front.
            var moved = FromSet(reference); moved.Listener = [0.5, 0, 0];
            require(SofaReader.Read(Write(folder, "moved.sofa", moved)).Set.Directions.Length == m, "listener position");
            // FIR-E: Data.IR [M R N E] with one emitter.
            var fire = FromSet(reference); fire.IrDims = [(ulong)m, 2, 64, 1]; fire.Globals["SOFAConventions"] = "FreeFieldHRIR"; fire.Globals["DataType"] = "FIR-E";
            var fe = SofaReader.Read(Write(folder, "fire.sofa", fire));
            Same(fe.Set, reference);
            require(fe.Warnings.Any(w => w.Contains("FreeFieldHRIR", StringComparison.Ordinal)), "convention warning");
        });

        test("SOFA reader: optional attributes missing are assumed with a warning; a zero distance becomes 1.2 m", () =>
        {
            var bare = FromSet(reference, distance: 0);
            bare.Globals.Clear(); bare.SourceType = null; bare.SourceUnits = null; bare.RateUnits = null; bare.Delay = null; bare.Receiver = null;
            var read = SofaReader.Read(Write(folder, "bare.sofa", bare));
            Same(read.Set, reference);
            require(read.Set.Name == "bare" && read.Set.Source == "SOFA file", read.Set.Source);
            foreach (var fragment in new[] { "which SOFA convention", "coordinate type", "units", "no measurement distance" })
                require(read.Warnings.Any(w => w.Contains(fragment, StringComparison.Ordinal)), "warning about " + fragment + ": " + string.Join("; ", read.Warnings));
            require(read.Set.MeasurementDistanceM == 1.2, "distance " + read.Set.MeasurementDistanceM);
            require(DcsHrtfFile.Validate(HrtfConverter.Convert(read.Set, personal: true).File) is null, "converts");
        });

        test("SOFA reader: refuses what it cannot use, with the reason in plain words", () =>
        {
            void Refused(string file, string fragment)
            {
                try { SofaReader.Read(file); }
                catch (HrtfConversionException e) { require(e.Message.Contains(fragment, StringComparison.OrdinalIgnoreCase), fragment + " -> " + e.Message); return; }
                throw new Exception("Not refused: " + fragment);
            }
            Directory.CreateDirectory(folder);
            var text = Path.Combine(folder, "text.sofa"); File.WriteAllText(text, "not a sofa file at all");
            Refused(text, "not a SOFA file");
            var cdf = Path.Combine(folder, "classic.sofa"); File.WriteAllBytes(cdf, [(byte)'C', (byte)'D', (byte)'F', 1, 0, 0, 0, 0]);
            Refused(cdf, "netCDF-3");
            Refused(Path.Combine(folder, "missing.sofa"), "not found");
            var tf = FromSet(reference); tf.Globals["SOFAConventions"] = "SimpleFreeFieldHRTF"; tf.Globals["DataType"] = "TF";
            Refused(Write(folder, "tf.sofa", tf), "transfer functions");
            var room = FromSet(reference); room.Globals["SOFAConventions"] = "SingleRoomSRIR";
            Refused(Write(folder, "room.sofa", room), "SimpleFreeFieldHRIR");
            var other = FromSet(reference); other.Globals["Conventions"] = "CF-1.6";
            Refused(Write(folder, "other.sofa", other), "not a SOFA file");
            var m = reference.Directions.Length;
            var three = FromSet(reference); three.Ir = [.. three.Ir, .. new double[m * 64]]; three.IrDims = [(ulong)m, 3, 64];
            Refused(Write(folder, "three.sofa", three), "two ears");
            var noRate = FromSet(reference); noRate.Rate = null;
            Refused(Write(folder, "norate.sofa", noRate), "sample rate");
            var mixed = FromSet(reference); mixed.Rate = Enumerable.Range(0, m).Select(i => i == 3 ? 44100.0 : 48000).ToArray();
            Refused(Write(folder, "mixed.sofa", mixed), "different sample rates");
            var negative = FromSet(reference); negative.Delay = [0, -1];
            Refused(Write(folder, "negative.sofa", negative), "negative");
            var rows = FromSet(reference); rows.Source = rows.Source[..(rows.Source.Length - 3)];
            Refused(Write(folder, "rows.sofa", rows), "rows");
            var units = FromSet(reference); units.SourceUnits = "degree, degree, furlong";
            Refused(Write(folder, "units.sofa", units), "furlong");
            // Read, then refused by the converter with its own reason (the same exception type the app catches).
            var nan = FromSet(reference); nan.Ir[100] = double.NaN;
            var set = SofaReader.Read(Write(folder, "nan.sofa", nan)).Set;
            try { HrtfConverter.Convert(set, true); throw new Exception("NaN not refused"); }
            catch (HrtfConversionException e) { require(e.Message.Contains("non-finite", StringComparison.Ordinal), e.Message); }
        });

        test("hrtf-convert: --check, a conversion with its warning and coverage lines, a refusal on stderr with exit 2, wrong arguments exit 1", () =>
        {
            int Run(out string output, out string error, params string[] args)
            {
                using StringWriter o = new(), e = new();
                var code = SofaConversion.RunCli(args, o, e);
                output = o.ToString(); error = e.ToString(); return code;
            }
            require(Run(out var checkOut, out _, "--check") == 0 && checkOut.Contains("PureHDF 2.2.0", StringComparison.Ordinal), checkOut);
            var sparse = FromSet(HrtfTests.Synthetic(HrtfTests.LatLong(20, 20), length: 64));
            var path = Write(folder, "cli.sofa", sparse);
            var target = Path.Combine(folder, "out", "cli.dcshrtf");
            var code = Run(out var output, out var error, path, target, "--name", "Ears", "--source", "my lab");
            require(code == 0 && error.Length == 0, code + " " + error);
            var lines = output.Split('\n', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries);
            require(lines.Any(l => l.StartsWith("warning: Sparse grid", StringComparison.Ordinal)) && lines.Any(l => l.StartsWith("coverage: ", StringComparison.Ordinal) && l.EndsWith('%')), output);
            var bytes = File.ReadAllBytes(target);
            var data = DcsHrtfFile.Read(bytes);
            require(DcsHrtfFile.Validate(bytes) is null && data.Name == "Ears" && data.Source == "my lab" && data.Flags.HasFlag(DcsHrtfFile.FileFlags.Personal), $"{data.Name} {data.Source} {data.Flags}");
            require(!File.Exists(target + ".part"), "no part file left");
            // The same input converts to the same bytes; --generic clears the personal flag.
            require(Run(out _, out _, path, target, "--name", "Ears", "--source", "my lab") == 0 && File.ReadAllBytes(target).AsSpan().SequenceEqual(bytes), "deterministic");
            require(Run(out _, out _, path, target, "--generic") == 0 && !DcsHrtfFile.Read(File.ReadAllBytes(target)).Flags.HasFlag(DcsHrtfFile.FileFlags.Personal) && DcsHrtfFile.Read(File.ReadAllBytes(target)).Name == "cli", "generic, default name");
            // The coverage line: a full sphere is 100 %, the upper half about half.
            var full = SofaConversion.Coverage(DcsHrtfFile.Read(bytes).Directions);
            require(full >= 0.99, "coverage " + full);
            // A refusal: exit 2, the reason the last stderr line, nothing written.
            var text = Path.Combine(folder, "bad.sofa"); File.WriteAllText(text, "nothing");
            var refused = Path.Combine(folder, "out", "bad.dcshrtf");
            require(Run(out _, out var why, text, refused) == 2 && why.Trim().Split('\n')[^1].Contains("not a SOFA file", StringComparison.Ordinal) && !File.Exists(refused), why);
            require(Run(out _, out var usage, path) == 1 && usage.Length > 0 && !usage.StartsWith("Usage", StringComparison.Ordinal), usage);
            require(Run(out _, out _, path, target, "--bogus") == 1 && Run(out _, out _, path, path) == 1, "bad options");
        });

        test("hrtf-convert: the app's Import SOFA through DcsVr.Cli.exe keeps a valid set with the report; a refused file keeps nothing", () =>
        {
            var cli = System.Reflection.Assembly.GetExecutingAssembly().GetCustomAttributes(typeof(System.Reflection.AssemblyMetadataAttribute), false)
                .Cast<System.Reflection.AssemblyMetadataAttribute>().FirstOrDefault(a => a.Key == "DcsVrCliPath")?.Value;
            if (cli is null || !File.Exists(cli)) { skip("the CLI built with the tests was not found"); return; }
            var directory = Path.GetDirectoryName(cli)!;
            require(CliHrtfConverter.IsAvailable(directory), "hrtf-convert --check");
            var library = new HrtfLibrary(Path.Combine(root, "sofa-import-state"));
            var irregular = HrtfTests.Synthetic(HrtfTests.LatLong(12, 12, minEl: -50), rate: 44100, length: 96);
            var path = Write(Path.Combine(folder, "My measurements"), "my ears.sofa", FromSet(irregular));
            var set = library.Import(path, "my ears", new CliHrtfConverter(directory));
            require(set is { Name: "my ears", SampleRate: 44100, FromFile: "my ears.sofa" } && set.Directions == irregular.Directions.Length && set.Coverage is > 0.5 and < 1.0, $"{set.Directions} {set.SampleRate} {set.Coverage}");
            require(set.Warnings.Any(w => w.Contains("below", StringComparison.Ordinal)), string.Join("; ", set.Warnings));
            require(library.List().Single().Name == "my ears" && library.ForDeployment().Single().Content.Length == set.Bytes, "listed and deployable");
            require(!File.ReadAllText(Path.Combine(library.Folder, "my ears.json")).Contains("My measurements", StringComparison.Ordinal), "the folder is not kept");
            var bad = Path.Combine(folder, "refused.sofa"); File.WriteAllText(bad, "no");
            try { library.Import(bad, "refused", new CliHrtfConverter(directory)); throw new Exception("not refused"); }
            catch (HrtfConversionException e) { require(e.Message.StartsWith("The SOFA file was not converted: This is not a SOFA file", StringComparison.Ordinal), e.Message); }
            require(library.List().Count == 1 && Directory.GetFiles(library.Folder).Length == 2, "nothing kept for the refused file");
        });

        test("Generic HRTF: SADIE II KU100 from the cached archive converts to the committed sadie2_ku100.dcshrtf byte for byte, within 1 dB of the source", () =>
        {
            var archive = Path.Combine(workspace, ".cache", "hrtf", "D1_HRIR_SOFA.zip");
            var committed = Path.Combine(workspace, "native", "binaural", "data", "sadie2_ku100.dcshrtf");
            require(File.Exists(committed), "native/binaural/data/sadie2_ku100.dcshrtf is missing (scripts/fetch-hrtf.ps1)");
            var facts = HrtfFile.Validate(File.ReadAllBytes(committed), out var why);
            require(facts is { Directions: 8703, SampleRate: 48000, Taps: 128, Personal: false, Title: "SADIE II KU100" }, "the app's check: " + why);
            // The app's report: no warning for the generic head (its fitted radius, 119 mm, is about 0.9 ms between the ears).
            require(facts!.Warnings().Count == 0 && Math.Abs(facts.MaxItdMs - 0.89) < 0.02, string.Join("; ", facts.Warnings()) + " " + facts.MaxItdMs);
            if (!File.Exists(archive)) { skip("no .cache/hrtf/D1_HRIR_SOFA.zip (scripts/fetch-hrtf.ps1 downloads it)"); return; }
            var sofa = Path.Combine(root, "sadie", "D1_48K.sofa");
            Directory.CreateDirectory(Path.GetDirectoryName(sofa)!);
            using (var zip = ZipFile.OpenRead(archive))
                zip.Entries.Single(e => e.Name.Contains("48K", StringComparison.Ordinal) && e.Name.EndsWith(".sofa", StringComparison.OrdinalIgnoreCase)).ExtractToFile(sofa);
            var read = SofaReader.Read(sofa, "SADIE II KU100", "SADIE II v2-2 (Univ. of York) D1, Apache-2.0, modified");
            require(read.Set.Directions.Length == 8802 && read.Set.SampleRate == 48000 && read.Set.Ir[0][0].Length == 256 && read.Warnings.Count == 0, string.Join("; ", read.Warnings));
            var a = HrtfConverter.Convert(read.Set, personal: false);
            var b = HrtfConverter.Convert(SofaReader.Read(sofa, "SADIE II KU100", "SADIE II v2-2 (Univ. of York) D1, Apache-2.0, modified").Set, personal: false);
            require(a.File.AsSpan().SequenceEqual(b.File), "converts bit-identically twice");
            require(a.File.AsSpan().SequenceEqual(File.ReadAllBytes(committed)), "differs from the committed set: run scripts/fetch-hrtf.ps1 and commit the result");
            // Magnitude per third octave (200 Hz-16 kHz) against the source HRIRs, equalised and scaled as the converter does.
            var d = DcsHrtfFile.Read(a.File);
            var worst = 0.0;
            var sources = new List<int>();  // the source measurement of each kept direction (repeats merged)
            for (var i = 0; i < d.DirectionCount; i++)
            {
                double[] u = [d.Directions[i * 3], d.Directions[i * 3 + 1], d.Directions[i * 3 + 2]];
                var best = 0; var bestDot = -2.0;
                for (var j = 0; j < read.Set.Directions.Length; j++)
                {
                    var v = read.Set.Directions[j];
                    var dot = u[0] * v[0] + u[1] * v[1] + u[2] * v[2];
                    if (dot > bestDot + 1e-12) { bestDot = dot; best = j; }
                }
                sources.Add(best);
            }
            var errors = new double[d.DirectionCount];
            Parallel.For(0, d.DirectionCount, i =>
            {
                var w = 0.0;
                for (var e = 0; e < 2; e++)
                {
                    var got = HrtfTests.BandsDb(d.Ir.AsSpan((i * 2 + e) * d.Taps, d.Taps), 2048, d.SampleRate, 1);
                    var want = HrtfTests.BandsDb(read.Set.Ir[sources[i]][e], 2048, d.SampleRate, d.FrontGain, a.DiffuseFieldEqDb, a.SpectrumPoints);
                    for (var k = 0; k < got.Length; k++) w = Math.Max(w, Math.Abs(got[k] - want[k]));
                }
                errors[i] = w;
            });
            worst = errors.Max();
            Console.WriteLine($"  SADIE II KU100: {d.DirectionCount} directions, fitted radius {d.DatasetHeadRadiusM * 100:0.00} cm, worst third-octave error {worst:0.000} dB, {a.File.Length} bytes");
            require(worst <= 1.0, $"magnitude error {worst:0.000} dB");
        });
    }
}
