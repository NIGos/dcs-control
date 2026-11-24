using DcsVr.Core;
using DcsVr.Core.Hrtf;

namespace DcsVr.Tests;

/// <summary>
/// The SOFA -> .dcshrtf converter (docs/BINAURAL_AUDIO.md 8.2, "C# converter tests"), on synthetic spherical-head HRIRs
/// with known magnitudes and onsets. The fixture native/binaural/test/data/csharp_converter_fixture.dcshrtf is this
/// converter's output for a small grid; the native harness loads it (the two sides agree on the format). Regenerate it
/// after a deliberate converter change with DCSVR_UPDATE_HRTF_FIXTURE=1.
/// </summary>
static class HrtfTests
{
    // ---- synthetic HRIRs ------------------------------------------------------------------------------------------
    const double HeadRadius = 0.0875;

    /// <summary>Brown & Duda head shadow per ear plus a back shelf (as native/binaural/test/synthetic_hrtf.h).</summary>
    static double EarMagnitude(double[] d, int ear, double f)
    {
        double w0 = HrtfConverter.SpeedOfSound / HeadRadius, w = 2 * Math.PI * f;
        var axis = ear == 0 ? -d[2] : d[2];
        var t = Math.Acos(Math.Clamp(axis, -1, 1)) * 180 / Math.PI;
        var alpha = 1.05 + 0.95 * Math.Cos(Math.PI * t / 180);
        var mag = Math.Sqrt(1 + Math.Pow(alpha * w / (2 * w0), 2)) / Math.Sqrt(1 + Math.Pow(w / (2 * w0), 2));
        if (d[0] < 0) mag *= 1 + (Math.Pow(10, -8 * -d[0] / 20) - 1) * (f * f / (f * f + 9e6));
        // A mild elevation cue (a pinna-like notch moving with elevation) so the spectra differ in every direction.
        var notch = 7000 + 3000 * d[1];
        mag *= 1 - 0.5 * Math.Exp(-Math.Pow((f - notch) / 800, 2));
        return mag;
    }

    /// <summary>Woodworth onset per ear, seconds (>= 0).</summary>
    static double EarDelay(double[] d, int ear)
    {
        var axis = ear == 0 ? -d[2] : d[2];
        var t = Math.Acos(Math.Clamp(axis, -1, 1));
        var rc = HeadRadius / HrtfConverter.SpeedOfSound;
        return (t < Math.PI / 2 ? -rc * Math.Cos(t) : rc * (t - Math.PI / 2)) + rc;
    }

    /// <summary>Minimum-phase IRs of `length` samples delayed (fractionally) by `pre` + the ear's onset.</summary>
    internal static HrirSet Synthetic(double[][] dirs, double rate = 48000, int length = 256, double pre = 10)
    {
        var n = HrtfDsp.NextPow2(length * 4);
        var ir = new float[dirs.Length][][];
        Parallel.For(0, dirs.Length, i =>
        {
            ir[i] = new float[2][];
            for (var e = 0; e < 2; e++)
            {
                var mag = new double[n / 2 + 1];
                for (var k = 0; k <= n / 2; k++) mag[k] = EarMagnitude(dirs[i], e, k * rate / n);
                var h = HrtfDsp.MinimumPhase(mag, n, n);
                var a = new System.Numerics.Complex[n];
                for (var k = 0; k < n; k++) a[k] = h[k];
                HrtfDsp.Fft(a, false);
                var delay = pre + EarDelay(dirs[i], e) * rate;
                for (var k = 0; k < n; k++)
                {
                    var kk = k <= n / 2 ? k : k - n;
                    a[k] *= System.Numerics.Complex.FromPolarCoordinates(1, -2 * Math.PI * kk * delay / n);
                }
                HrtfDsp.Fft(a, true);
                ir[i][e] = Enumerable.Range(0, length).Select(k => (float)a[k].Real).ToArray();
            }
        });
        return new HrirSet { SampleRate = rate, Directions = dirs, Ir = ir, MeasurementDistanceM = 1.2, Name = "Synthetic sphere", Source = "test" };
    }

    internal static double[][] LatLong(double elStep, double azStep, double minEl = -90 + 1e-9)
    {
        var dirs = new List<double[]> { HrirSet.DirectionFromSofa(0, 90) };
        for (var el = 90 - elStep; el > minEl; el -= elStep)
            for (var az = 0.0; az < 360 - 1e-9; az += azStep) dirs.Add(HrirSet.DirectionFromSofa(az, el));
        if (minEl < -89) dirs.Add(HrirSet.DirectionFromSofa(0, -90));
        return dirs.ToArray();
    }

    internal static double[] BandsDb(ReadOnlySpan<float> ir, int n, double rate, double scale, double[]? eqDb = null, int eqN = 0)
    {
        var mag = HrtfDsp.Magnitude(ir, n);
        var result = new List<double>();
        for (var c = 200.0; c <= 16000; c *= Math.Pow(2, 1.0 / 3))
        {
            double lo = c / Math.Pow(2, 1.0 / 6), hi = c * Math.Pow(2, 1.0 / 6), e = 0;
            for (var k = 1; k <= n / 2; k++)
            {
                var f = k * rate / n;
                if (f < lo || f >= hi) continue;
                var g = scale;
                if (eqDb != null) g *= Math.Pow(10, eqDb[(int)Math.Round(f / rate * eqN)] / 20);
                e += Math.Pow(mag[k] * g, 2);
            }
            result.Add(10 * Math.Log10(Math.Max(e, 1e-30)));
        }
        return result.ToArray();
    }

    public static void Register(Action<string, Action> test, Action<bool, string> require, string workspace)
    {
        test("HRTF converter: a measured-style grid converts the same twice, validates, closed counter-clockwise triangulation", () =>
        {
            var set = Synthetic(LatLong(10, 10));
            var a = HrtfConverter.Convert(set, personal: false);
            var b = HrtfConverter.Convert(set, personal: false);
            require(a.File.AsSpan().SequenceEqual(b.File), "bit-identical");
            require(DcsHrtfFile.Validate(a.File) is null, DcsHrtfFile.Validate(a.File) ?? "");
            var d = DcsHrtfFile.Read(a.File);
            require(d.DirectionCount == set.Directions.Length && d.TriangleCount == 2 * d.DirectionCount - 4, $"{d.DirectionCount} {d.TriangleCount}");
            for (var f = 0; f < d.TriangleCount; f++)
            {
                double[] A = Dir(d, d.Triangles[f * 3]), B = Dir(d, d.Triangles[f * 3 + 1]), C = Dir(d, d.Triangles[f * 3 + 2]);
                var det = A[0] * (B[1] * C[2] - B[2] * C[1]) - A[1] * (B[0] * C[2] - B[2] * C[0]) + A[2] * (B[0] * C[1] - B[1] * C[0]);
                require(det > 0, "triangle " + f + " is clockwise");
            }
            // Every edge shared by exactly two triangles, once in each direction.
            var edges = new HashSet<(uint, uint)>();
            for (var f = 0; f < d.TriangleCount; f++)
                for (var k = 0; k < 3; k++) require(edges.Add((d.Triangles[f * 3 + k], d.Triangles[f * 3 + (k + 1) % 3])), "edge twice");
            foreach (var (u, v) in edges) require(edges.Contains((v, u)), "open edge");
            require(DcsHrtfFile.Write(d).AsSpan().SequenceEqual(a.File), "read + write round trip");
            require(d.Flags == (DcsHrtfFile.FileFlags.MinimumPhase | DcsHrtfFile.FileFlags.DiffuseFieldEq | DcsHrtfFile.FileFlags.FrontNormalised), d.Flags.ToString());
            require(a.Report.Warnings.Count == 0, string.Join("; ", a.Report.Warnings));
        });

        test("HRTF converter: magnitude within 1 dB per third octave (200 Hz-16 kHz) and ITD within 10 us of the source, head radius fitted", () =>
        {
            var set = Synthetic(LatLong(10, 10));
            var conv = HrtfConverter.Convert(set, personal: false);
            var d = DcsHrtfFile.Read(conv.File);
            var n = 2048;
            double worstDb = 0, worstUs = 0;
            for (var i = 0; i < d.DirectionCount; i++)
            {
                for (var e = 0; e < 2; e++)
                {
                    var got = BandsDb(d.Ir.AsSpan((i * 2 + e) * d.Taps, d.Taps), n, d.SampleRate, 1);
                    var want = BandsDb(set.Ir[i][e], n, d.SampleRate, d.FrontGain, conv.DiffuseFieldEqDb, conv.SpectrumPoints);
                    for (var k = 0; k < got.Length; k++) worstDb = Math.Max(worstDb, Math.Abs(got[k] - want[k]));
                }
                var itd = (d.Delays[i * 2 + 1] - d.Delays[i * 2]) / d.SampleRate;
                var truth = EarDelay(set.Directions[i], 1) - EarDelay(set.Directions[i], 0);
                worstUs = Math.Max(worstUs, Math.Abs(itd - truth) * 1e6);
            }
            Console.WriteLine($"  HRTF converter: worst third-octave magnitude error {worstDb:0.000} dB, worst ITD error {worstUs:0.00} us, fitted radius {conv.Report.FittedHeadRadiusM * 100:0.00} cm");
            require(worstDb <= 1.0, $"magnitude error {worstDb:0.000} dB");
            require(worstUs <= 10.0, $"ITD error {worstUs:0.00} us");
            require(Math.Abs(conv.Report.FittedHeadRadiusM - HeadRadius) <= 0.002, $"radius {conv.Report.FittedHeadRadiusM}");
            // Front normalised: the interpolated front IRs carry unit energy per ear (on average).
            var dirs = Enumerable.Range(0, d.DirectionCount).Select(i => Dir(d, (uint)i)).ToArray();
            var (v, w) = HrtfConverter.Locate(dirs, d.Triangles.Select(x => (int)x).ToArray(), [1, 0, 0]);
            var energy = 0.0;
            for (var e = 0; e < 2; e++)
                for (var k = 0; k < d.Taps; k++)
                {
                    var s = 0.0;
                    for (var j = 0; j < 3; j++) s += w[j] * d.Ir[(v[j] * 2 + e) * d.Taps + k];
                    energy += 0.5 * s * s;
                }
            require(Math.Abs(10 * Math.Log10(energy)) < 0.01, $"front energy {10 * Math.Log10(energy):0.000} dB");
            // Diffuse-field EQ: limited to +-15 dB.
            require(conv.DiffuseFieldEqDb.All(x => x >= -15 && x <= 15), "EQ limit");
        });

        test("HRTF converter: rejects bad input with a clear reason", () =>
        {
            void Rejects(HrirSet set, string fragment)
            {
                try { HrtfConverter.Convert(set, true); }
                catch (HrtfConversionException e) { require(e.Message.Contains(fragment, StringComparison.OrdinalIgnoreCase), e.Message); return; }
                throw new Exception("Not rejected: " + fragment);
            }
            var good = Synthetic(LatLong(15, 15), length: 64);
            Rejects(new HrirSet { SampleRate = 22050, Directions = good.Directions, Ir = good.Ir }, "sample rate");
            Rejects(new HrirSet { SampleRate = 48000, Directions = good.Directions[..99], Ir = good.Ir[..99] }, "at least 100");
            var nan = good.Ir.Select(m => m.Select(e => e.ToArray()).ToArray()).ToArray();
            nan[5][1][3] = float.NaN;
            Rejects(new HrirSet { SampleRate = 48000, Directions = good.Directions, Ir = nan }, "non-finite");
            var upper = Synthetic(LatLong(10, 10, minEl: -1), length: 64);
            Rejects(upper, "surround the head");
            var ragged = good.Ir.Select(m => m.Select(e => e.ToArray()).ToArray()).ToArray();
            ragged[7][0] = new float[63];
            Rejects(new HrirSet { SampleRate = 48000, Directions = good.Directions, Ir = ragged }, "differ in length");
            Rejects(new HrirSet { SampleRate = 48000, Directions = good.Directions, Ir = good.Ir, Delay = good.Directions.Select(_ => new[] { 0.0, double.NaN }).ToArray() }, "Data.Delay");
            // A valid file corrupted is refused by the validator (the same rules as the DCS side).
            var ok = HrtfConverter.Convert(good, true).File;
            var payload = (byte[])ok.Clone(); payload[400] ^= 1;
            require(DcsHrtfFile.Validate(payload) == "payload checksum", DcsHrtfFile.Validate(payload) ?? "valid");
            var header = (byte[])ok.Clone(); header[70] ^= 1;
            require(DcsHrtfFile.Validate(header) == "header checksum", DcsHrtfFile.Validate(header) ?? "valid");
            require(DcsHrtfFile.Validate(ok.AsSpan(0, ok.Length - 4)) == "file size", "truncated");
        });

        test("HRTF converter: a personal irregular grid with repeats converts and validates", () =>
        {
            var rng = new Random(5);
            var dirs = new List<double[]>();
            const int count = 400;
            for (var i = 0; i < count; i++)
            {
                // Fibonacci sphere, jittered by up to ~3 degrees, elevation above -60 degrees (a typical chair-mounted rig).
                var y = 1 - 2 * (i + 0.5) / count;
                if (y < -0.87) continue;
                var r = Math.Sqrt(1 - y * y);
                var phi = i * Math.PI * (3 - Math.Sqrt(5)) + (rng.NextDouble() - 0.5) * 0.1;
                dirs.Add([r * Math.Cos(phi), y + (rng.NextDouble() - 0.5) * 0.05, r * Math.Sin(phi)]);
            }
            for (var i = 0; i < 5; i++) dirs.Add((double[])dirs[i * 17].Clone());
            var set = Synthetic(dirs.ToArray(), rate: 44100, length: 200);
            var conv = HrtfConverter.Convert(set, personal: true);
            require(DcsHrtfFile.Validate(conv.File) is null, "validates");
            var d = DcsHrtfFile.Read(conv.File);
            require(conv.Report.MergedDuplicates == 5 && d.DirectionCount == dirs.Count - 5 && d.TriangleCount == 2 * d.DirectionCount - 4, $"{conv.Report.MergedDuplicates} {d.DirectionCount}");
            require(d.Flags.HasFlag(DcsHrtfFile.FileFlags.Personal) && d.SampleRate == 44100 && d.Taps == 120, $"{d.Flags} {d.SampleRate} {d.Taps}");
            require(conv.Report.Warnings.Any(w => w.Contains("Sparse", StringComparison.Ordinal)) && conv.Report.Warnings.Any(w => w.Contains("merged", StringComparison.Ordinal)), string.Join("; ", conv.Report.Warnings));
            require(conv.Report.MinElevationDeg < -55 && conv.Report.MaxElevationDeg > 85, $"{conv.Report.MinElevationDeg} {conv.Report.MaxElevationDeg}");
            require(Math.Abs(conv.Report.FittedHeadRadiusM - HeadRadius) <= 0.003, $"radius {conv.Report.FittedHeadRadiusM}");
        });

        test("HRTF converter: output matches the native harness fixture byte for byte", () =>
        {
            var path = Path.Combine(workspace, "native", "binaural", "test", "data", "csharp_converter_fixture.dcshrtf");
            var set = Synthetic(LatLong(15, 30), length: 128);
            var bytes = HrtfConverter.Convert(new HrirSet { SampleRate = set.SampleRate, Directions = set.Directions, Ir = set.Ir, Name = "Converter fixture", Source = "synthetic, tests/DcsVr.Tests" }, personal: false, taps: 32).File;
            if (Environment.GetEnvironmentVariable("DCSVR_UPDATE_HRTF_FIXTURE") == "1")
            {
                Directory.CreateDirectory(Path.GetDirectoryName(path)!);
                File.WriteAllBytes(path, bytes);
            }
            require(File.Exists(path), "missing fixture " + path + " (DCSVR_UPDATE_HRTF_FIXTURE=1 writes it)");
            require(File.ReadAllBytes(path).AsSpan().SequenceEqual(bytes), "the converter's output changed: regenerate the fixture deliberately (DCSVR_UPDATE_HRTF_FIXTURE=1) and rerun the native harness");
        });
    }

    static double[] Dir(DcsHrtfFile.Data d, uint i) => [d.Directions[i * 3], d.Directions[i * 3 + 1], d.Directions[i * 3 + 2]];
}
