namespace DcsVr.Core.Hrtf;

/// <summary>
/// Head-related impulse responses as measured: what a SOFA SimpleFreeFieldHRIR file holds, already in DCS's head frame.
/// </summary>
public sealed class HrirSet
{
    public required double SampleRate { get; init; }
    /// <summary>Unit vectors, x forward, y up, z right (DirectionFromSofa converts SOFA's spherical coordinates).</summary>
    public required double[][] Directions { get; init; }
    /// <summary>[measurement][ear 0 = left, 1 = right][sample].</summary>
    public required float[][][] Ir { get; init; }
    /// <summary>SOFA Data.Delay in samples per measurement and ear, or null (the IRs carry their onsets).</summary>
    public double[][]? Delay { get; init; }
    public double MeasurementDistanceM { get; init; } = 1.2;
    public string Name { get; init; } = "";
    public string Source { get; init; } = "";

    /// <summary>SOFA spherical (azimuth counter-clockwise from the front, elevation up; degrees) to the head frame.</summary>
    public static double[] DirectionFromSofa(double azimuthDeg, double elevationDeg)
    {
        double az = azimuthDeg * Math.PI / 180, el = elevationDeg * Math.PI / 180;
        return [Math.Cos(el) * Math.Cos(az), Math.Sin(el), -Math.Cos(el) * Math.Sin(az)];
    }
}

public sealed record HrtfConversionReport(
    int Directions, int MergedDuplicates, int UnusedDirections, double SampleRate, int Taps, double FittedHeadRadiusM,
    double MinElevationDeg, double MaxElevationDeg, double LargestGapDeg, double FrontGain, IReadOnlyList<string> Warnings);

/// <summary>The converted set: the .dcshrtf bytes and the converter's report. Refusals throw
/// <see cref="DcsVr.Core.HrtfConversionException"/>, the same type the import path (HrtfLibrary, the Audio page) catches.</summary>
public sealed record HrtfConvertedSet(byte[] File, HrtfConversionReport Report, double[] DiffuseFieldEqDb, int SpectrumPoints);

/// <summary>
/// SOFA HRIRs -> .dcshrtf (native/binaural/include/hrtf_format.h). Steps, in order:
///   1. checks: rate 44.1-96 kHz, at least 100 directions, finite data, IRs of 16 samples or more;
///   2. directions closer than 0.05 degrees merged (their IRs and Data.Delay averaged, the first direction kept), the
///      convex hull triangulated (SphereHull); the directions must surround the head;
///   3. diffuse-field equalisation: the power average over directions (weighted by their share of the sphere), smoothed
///      to 1/3 octave, inverted, centred on its 200 Hz-10 kHz mean, limited to +-15 dB;
///   4. minimum phase (real cepstrum) of each equalised magnitude, truncated to `taps` (128 at 48 kHz, scaled with the rate)
///      with a half-Hann tail over the last quarter;
///   5. onsets: per ear, the delay that makes the stored minimum-phase IR match the measured phase over 200-1500 Hz
///      (HrtfDsp.ExcessPhaseDelay), plus Data.Delay; the common minimum removed. Each ear keeps its own measured delay
///      (not a mean held constant with the difference split around it): with the near field on, the renderer looks each
///      ear up from that ear's own direction, which needs each ear's own delay curve;
///   6. front normalisation: one gain so that noise from straight ahead (the interpolated front IRs) comes out at 0 dB on
///      the mean of the two ears, measured over 200 Hz-8 kHz (where the sound of DCS is; the 12-24 kHz half of a white
///      spectrum would otherwise decide the level);
///   7. head radius: least-squares fit of Woodworth's ITD = (r/c)(phi + sin phi), phi the lateral angle, over all
///      directions (c = 343 m/s).
/// Deterministic: the same input gives the same bytes.
/// </summary>
public static class HrtfConverter
{
    public const double SpeedOfSound = 343.0;
    const double MergeDegrees = 0.05;
    /// <summary>Band of the front normalisation (hertz).</summary>
    public const double FrontBandLowHz = 200, FrontBandHighHz = 8000;
    /// <summary>A set measured closer than this has the near field in its responses; the renderer's near-field model
    /// assumes a far-field measurement (docs/BINAURAL_AUDIO.md 4.2).</summary>
    public const double NearMeasurementM = 1.0;
    /// <summary>The closest measurement distance the renderer's near field takes from a set (binaural_core.h
    /// kNearFieldFloorM, just above the largest ear radius): a set measured closer is used as if measured here.</summary>
    public const double NearFieldFloorM = 0.15;

    public static HrtfConvertedSet Convert(HrirSet set, bool personal, int? taps = null)
    {
        // 1. Checks.
        if (!(set.SampleRate >= 44100 && set.SampleRate <= 96000)) throw new HrtfConversionException($"Unsupported sample rate {set.SampleRate:0} Hz (44.1 to 96 kHz).");
        var m = set.Directions.Length;
        if (set.Ir.Length != m || (set.Delay is { } dl && dl.Length != m)) throw new HrtfConversionException("The measurement counts do not match.");
        if (m < 100) throw new HrtfConversionException($"Only {m} directions (at least 100 are needed).");
        var length = set.Ir[0][0].Length;
        if (length < 16) throw new HrtfConversionException("The impulse responses are shorter than 16 samples.");
        for (var i = 0; i < m; i++)
        {
            if (set.Directions[i].Length != 3 || set.Directions[i].Any(v => !double.IsFinite(v))) throw new HrtfConversionException("A direction is not finite.");
            if (set.Ir[i].Length != 2 || set.Ir[i][0].Length != length || set.Ir[i][1].Length != length) throw new HrtfConversionException("The impulse responses differ in length.");
            foreach (var ear in set.Ir[i])
                foreach (var v in ear)
                    if (!float.IsFinite(v)) throw new HrtfConversionException("The impulse responses contain non-finite values.");
            if (set.Delay is { } d && (d[i].Length != 2 || d[i].Any(v => !double.IsFinite(v) || v < 0))) throw new HrtfConversionException("Data.Delay is not finite.");
        }
        if (!(set.MeasurementDistanceM >= 0.1 && set.MeasurementDistanceM <= 10)) throw new HrtfConversionException("The measurement distance is out of range (0.1 to 10 m).");
        var outTaps = taps ?? Math.Clamp((int)Math.Round(128 * set.SampleRate / 48000 / 8) * 8, 16, 512);
        if (outTaps < DcsHrtfFile.MinTaps || outTaps > DcsHrtfFile.MaxTaps) throw new HrtfConversionException("Tap count out of range.");
        var warnings = new List<string>();
        if (set.MeasurementDistanceM < NearFieldFloorM)
            warnings.Add(string.Create(System.Globalization.CultureInfo.InvariantCulture, $"Measured at {set.MeasurementDistanceM:0.0#} m, closer than 3D audio uses: it is treated as measured at {NearFieldFloorM:0.0#} m (close sounds in the cockpit start from there)."));
        else if (set.MeasurementDistanceM < NearMeasurementM)
            warnings.Add(string.Create(System.Globalization.CultureInfo.InvariantCulture, $"Measured at {set.MeasurementDistanceM:0.0#} m, closer than most sets: close sounds (in the cockpit) start from the set's own near field."));

        // 2. Directions: unit, merged (repeats averaged), triangulated.
        var unit = new List<double[]>();
        var members = new List<List<int>>();
        var cosMerge = Math.Cos(MergeDegrees * Math.PI / 180);
        var merged = 0;
        for (var i = 0; i < m; i++)
        {
            var v = set.Directions[i];
            var len = Math.Sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
            if (len < 1e-9) throw new HrtfConversionException("A direction has zero length.");
            double[] u = [v[0] / len, v[1] / len, v[2] / len];
            var duplicate = -1;
            for (var j = 0; j < unit.Count; j++)
            {
                var w = unit[j];
                if (u[0] * w[0] + u[1] * w[1] + u[2] * w[2] > cosMerge) { duplicate = j; break; }
            }
            if (duplicate >= 0) { members[duplicate].Add(i); merged++; continue; }
            unit.Add(u);
            members.Add([i]);
        }
        if (merged > 0) warnings.Add($"{merged} repeated directions merged.");
        var flat = new double[unit.Count * 3];
        for (var i = 0; i < unit.Count; i++) for (var k = 0; k < 3; k++) flat[i * 3 + k] = unit[i][k];
        int[] hull;
        try { hull = SphereHull.Build(flat); }
        catch (ArgumentException e) { throw new HrtfConversionException("The directions cannot be triangulated: " + e.Message); }
        // Keep only hull vertices (a direction inside the hull would never be used), remapped in order.
        var map = Enumerable.Repeat(-1, unit.Count).ToArray();
        foreach (var v in hull) map[v] = 0;
        var keep = new List<int>();
        for (var i = 0; i < unit.Count; i++) if (map[i] == 0) { map[i] = keep.Count; keep.Add(i); }
        var unused = unit.Count - keep.Count;
        if (unused > 0) warnings.Add($"{unused} directions left out (inside the measurement sphere).");
        var n = keep.Count;
        var tris = hull.Select(v => map[v]).ToArray();
        var t = tris.Length / 3;
        if (t != 2 * n - 4) throw new HrtfConversionException("The triangulation is not closed.");
        var dirs = new double[n][];
        for (var i = 0; i < n; i++) dirs[i] = unit[keep[i]];
        // The measurement of each kept direction: a repeat's IRs and Data.Delay averaged sample by sample.
        var raw = new float[n][][];
        var rawDelay = new double[n][];
        for (var i = 0; i < n; i++)
        {
            var group = members[keep[i]];
            raw[i] = new float[2][];
            rawDelay[i] = new double[2];
            for (var e = 0; e < 2; e++)
            {
                if (group.Count == 1) raw[i][e] = set.Ir[group[0]][e];
                else
                {
                    var avg = new double[length];
                    foreach (var g in group) for (var k = 0; k < length; k++) avg[k] += set.Ir[g][e][k];
                    raw[i][e] = avg.Select(x => (float)(x / group.Count)).ToArray();
                }
                if (set.Delay is { } sd) rawDelay[i][e] = group.Average(g => sd[g][e]);
            }
        }
        var minPlane = double.MaxValue;
        var largestGap = 0.0;
        var vertexArea = new double[n];
        for (var f = 0; f < t; f++)
        {
            double[] a = dirs[tris[f * 3]], b = dirs[tris[f * 3 + 1]], c = dirs[tris[f * 3 + 2]];
            var nrm = Cross(Sub(b, a), Sub(c, a));
            var area2 = Math.Sqrt(Dot(nrm, nrm));
            minPlane = Math.Min(minPlane, Dot(nrm, a) / area2);
            for (var k = 0; k < 3; k++) vertexArea[tris[f * 3 + k]] += area2 / 6;
            largestGap = Math.Max(largestGap, Math.Max(Angle(a, b), Math.Max(Angle(b, c), Angle(c, a))));
        }
        if (minPlane < 1e-3) throw new HrtfConversionException("The directions do not surround the head (a whole side has no measurements).");
        var minEl = dirs.Min(d => Math.Asin(Math.Clamp(d[1], -1, 1))) * 180 / Math.PI;
        var maxEl = dirs.Max(d => Math.Asin(Math.Clamp(d[1], -1, 1))) * 180 / Math.PI;
        if (minEl > -45) warnings.Add($"No directions below {minEl:0} degrees elevation: sounds from below use the nearest ones.");
        if (largestGap > 15) warnings.Add($"Sparse grid: up to {largestGap:0} degrees between neighbouring directions.");

        // 3. Diffuse-field equalisation.
        var N = HrtfDsp.NextPow2(Math.Max(length, outTaps) * 4);
        var bins = N / 2 + 1;
        var mags = new double[n * 2][];
        Parallel.For(0, n * 2, j => mags[j] = HrtfDsp.Magnitude(raw[j / 2][j % 2], N));
        var power = new double[bins];
        var totalArea = vertexArea.Sum();
        for (var i = 0; i < n; i++)
            for (var k = 0; k < bins; k++)
                power[k] += vertexArea[i] / totalArea * 0.5 * (mags[i * 2][k] * mags[i * 2][k] + mags[i * 2 + 1][k] * mags[i * 2 + 1][k]);
        var smooth = HrtfDsp.SmoothFractionalOctave(power, N, set.SampleRate, 3);
        var eqDb = new double[bins];
        for (var k = 0; k < bins; k++) eqDb[k] = -10 * Math.Log10(Math.Max(smooth[k], 1e-30));
        double sum = 0; var count = 0;
        for (var k = 1; k < bins; k++)
        {
            var f = k * set.SampleRate / N;
            if (f >= 200 && f <= 10000) { sum += eqDb[k]; count++; }
        }
        var centre = count > 0 ? sum / count : 0;
        for (var k = 0; k < bins; k++) eqDb[k] = Math.Clamp(eqDb[k] - centre, -15, 15);

        // 4. Minimum phase.
        var ir = new float[n * 2 * outTaps];
        Parallel.For(0, n * 2, j =>
        {
            var eq = new double[bins];
            for (var k = 0; k < bins; k++) eq[k] = mags[j][k] * Math.Pow(10, eqDb[k] / 20);
            HrtfDsp.MinimumPhase(eq, N, outTaps).CopyTo(ir, j * outTaps);
        });

        // 5. Onsets (samples at the file rate): the stored IR against the measured one. The equaliser's own phase is the
        // same in every IR, so it only shifts every onset alike and is removed with the common minimum.
        var onset = new double[n * 2];
        Parallel.For(0, n * 2, j =>
            onset[j] = HrtfDsp.ExcessPhaseDelay(raw[j / 2][j % 2], ir.AsSpan(j * outTaps, outTaps), set.SampleRate) + rawDelay[j / 2][j % 2]);
        var minOnset = onset.Min();
        for (var i = 0; i < onset.Length; i++) onset[i] -= minOnset;
        if (onset.Max() > 0.005 * set.SampleRate) throw new HrtfConversionException("Onset delays differ by more than 5 ms: the impulse responses are not aligned.");

        // 6. Front normalisation on the interpolated front IRs, over 200 Hz-8 kHz.
        var (fi, fw) = Locate(dirs, tris, [1, 0, 0]);
        double bandPower = 0;
        var bandBins = 0;
        for (var e = 0; e < 2; e++)
        {
            var front = new float[outTaps];
            for (var k = 0; k < outTaps; k++)
            {
                double v = 0;
                for (var j = 0; j < 3; j++) v += fw[j] * ir[(fi[j] * 2 + e) * outTaps + k];
                front[k] = (float)v;
            }
            var mag = HrtfDsp.Magnitude(front, N);
            for (var k = 1; k < bins; k++)
            {
                var f = k * set.SampleRate / N;
                if (f < FrontBandLowHz || f > FrontBandHighHz) continue;
                bandPower += mag[k] * mag[k];
                bandBins++;
            }
        }
        var energy = bandBins > 0 ? bandPower / bandBins : 0;
        if (!(energy > 1e-12)) throw new HrtfConversionException("The front impulse responses are silent.");
        var gain = 1 / Math.Sqrt(energy);
        for (var i = 0; i < ir.Length; i++) ir[i] = (float)(ir[i] * gain);
        if (ir.Any(v => Math.Abs(v) > 64)) throw new HrtfConversionException("The impulse responses are out of range after normalisation.");

        // 7. Head radius (Woodworth).
        double num = 0, den = 0;
        for (var i = 0; i < n; i++)
        {
            var itd = (onset[i * 2] - onset[i * 2 + 1]) / set.SampleRate;  // left later = source on the right = positive
            var phi = Math.Asin(Math.Clamp(dirs[i][2], -1, 1));
            var g = (phi + Math.Sin(phi)) / SpeedOfSound;
            num += itd * g; den += g * g;
        }
        var radius = den > 0 ? num / den : 0;
        if (!(radius >= 0.04 && radius <= 0.15)) throw new HrtfConversionException($"The interaural delays do not fit a head (fitted radius {radius * 100:0.0} cm).");

        var data = new DcsHrtfFile.Data(
            DcsHrtfFile.FileFlags.MinimumPhase | DcsHrtfFile.FileFlags.DiffuseFieldEq | DcsHrtfFile.FileFlags.FrontNormalised | (personal ? DcsHrtfFile.FileFlags.Personal : 0),
            (uint)Math.Round(set.SampleRate), outTaps, (float)radius, (float)set.MeasurementDistanceM, (float)gain, set.Name, set.Source,
            dirs.SelectMany(d => d.Select(v => (float)v)).ToArray(), onset.Select(v => (float)v).ToArray(), tris.Select(v => (uint)v).ToArray(), ir);
        var bytes = DcsHrtfFile.Write(data);
        if (DcsHrtfFile.Validate(bytes) is { } why) throw new HrtfConversionException("The converted file does not validate: " + why + ".");
        var report = new HrtfConversionReport(n, merged, unused, set.SampleRate, outTaps, radius, minEl, maxEl, largestGap, gain, warnings);
        return new HrtfConvertedSet(bytes, report, eqDb, N);
    }

    /// <summary>The triangle whose cone holds `d` and its barycentric weights (>= 0, sum 1).</summary>
    public static (int[] Vertices, double[] Weights) Locate(double[][] dirs, int[] tris, double[] d)
    {
        int[] best = [tris[0], tris[1], tris[2]];
        double[] bw = [1, 0, 0];
        var bestMin = double.MinValue;
        for (var f = 0; f < tris.Length / 3; f++)
        {
            double[] a = dirs[tris[f * 3]], b = dirs[tris[f * 3 + 1]], c = dirs[tris[f * 3 + 2]];
            var det = Dot(a, Cross(b, c));
            if (Math.Abs(det) < 1e-15) continue;
            double wa = Dot(d, Cross(b, c)) / det, wb = Dot(d, Cross(c, a)) / det, wc = Dot(d, Cross(a, b)) / det;
            var min = Math.Min(wa, Math.Min(wb, wc));
            if (min > bestMin) { bestMin = min; best = [tris[f * 3], tris[f * 3 + 1], tris[f * 3 + 2]]; bw = [wa, wb, wc]; }
        }
        for (var i = 0; i < 3; i++) bw[i] = Math.Max(bw[i], 0);
        var s = bw.Sum();
        return (best, bw.Select(w => w / s).ToArray());
    }

    static double[] Sub(double[] a, double[] b) => [a[0] - b[0], a[1] - b[1], a[2] - b[2]];
    static double[] Cross(double[] a, double[] b) => [a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]];
    static double Dot(double[] a, double[] b) => a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
    static double Angle(double[] a, double[] b) => Math.Acos(Math.Clamp(Dot(a, b), -1, 1)) * 180 / Math.PI;
}
