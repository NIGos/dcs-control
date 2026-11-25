using System.Numerics;
using DcsVr.Core.Hrtf;

namespace DcsVr.Tests;

/// <summary>
/// How faithfully a converted set keeps the measured time difference between the ears: the interaural phase delay
/// (right relative to left, microseconds, averaged over 200-1500 Hz where hearing uses the phase) of the measured HRIRs
/// against the rendered ones (the stored minimum-phase IR followed by the stored onset delay of each ear).
/// </summary>
static class HrtfItdMetrics
{
    public const double BandLowHz = 200, BandHighHz = 1500;

    /// <summary>Interaural phase delay in microseconds (positive: the right ear later, a source on the left).</summary>
    public static double PhaseDelayUs(ReadOnlySpan<float> left, ReadOnlySpan<float> right, double rate, double delayLeft = 0, double delayRight = 0)
    {
        var n = HrtfDsp.NextPow2(Math.Max(Math.Max(left.Length, right.Length) * 2, (int)Math.Ceiling(rate / 12)));
        var l = new Complex[n];
        var r = new Complex[n];
        for (var i = 0; i < left.Length; i++) l[i] = left[i];
        for (var i = 0; i < right.Length; i++) r[i] = right[i];
        HrtfDsp.Fft(l, false);
        HrtfDsp.Fft(r, false);
        var hi = (int)Math.Floor(BandHighHz * n / rate);
        double previous = 0, unwrapped = 0, sum = 0;
        var count = 0;
        for (var k = 0; k <= hi; k++)
        {
            var f = k * rate / n;
            var x = r[k] * Complex.Conjugate(l[k]) * Complex.FromPolarCoordinates(1, -2 * Math.PI * f * (delayRight - delayLeft) / rate);
            var p = x.Phase;
            if (k == 0) unwrapped = p;
            else
            {
                var step = p - previous;
                unwrapped += step - 2 * Math.PI * Math.Round(step / (2 * Math.PI));
            }
            previous = p;
            if (f < BandLowHz) continue;
            sum += -unwrapped / (2 * Math.PI * f);
            count++;
        }
        return sum / count * 1e6;
    }

    /// <summary>For each direction of the converted set: the measured and the rendered interaural phase delay (us).
    /// `measured[i]` are the measured [left, right] IRs of direction i of the converted set (repeats averaged).</summary>
    public static (double[] Measured, double[] Rendered) PhaseDelays(DcsHrtfFile.Data d, IReadOnlyList<float[][]> measured)
    {
        var a = new double[d.DirectionCount];
        var b = new double[d.DirectionCount];
        Parallel.For(0, d.DirectionCount, i =>
        {
            a[i] = PhaseDelayUs(measured[i][0], measured[i][1], d.SampleRate);
            b[i] = PhaseDelayUs(d.Ir.AsSpan(i * 2 * d.Taps, d.Taps), d.Ir.AsSpan((i * 2 + 1) * d.Taps, d.Taps), d.SampleRate,
                d.Delays[i * 2], d.Delays[i * 2 + 1]);
        });
        return (a, b);
    }

    /// <summary>The measured IRs of each direction of a converted set: every measurement within 0.05 degrees of it
    /// averaged (the converter merges such repeats the same way).</summary>
    public static List<float[][]> MeasuredFor(DcsHrtfFile.Data d, HrirSet set)
    {
        var cos = Math.Cos(0.05 * Math.PI / 180);
        var unit = set.Directions.Select(v => { var l = Math.Sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); return new[] { v[0] / l, v[1] / l, v[2] / l }; }).ToArray();
        var result = new float[d.DirectionCount][][];
        Parallel.For(0, d.DirectionCount, i =>
        {
            double[] u = [d.Directions[i * 3], d.Directions[i * 3 + 1], d.Directions[i * 3 + 2]];
            var group = new List<int>();
            var best = 0; var bestDot = -2.0;
            for (var j = 0; j < unit.Length; j++)
            {
                var dot = u[0] * unit[j][0] + u[1] * unit[j][1] + u[2] * unit[j][2];
                if (dot > cos) group.Add(j);
                if (dot > bestDot) { bestDot = dot; best = j; }
            }
            if (group.Count == 0) group.Add(best);
            var len = set.Ir[0][0].Length;
            result[i] = new float[2][];
            for (var e = 0; e < 2; e++)
            {
                var s = new double[len];
                foreach (var g in group) for (var k = 0; k < len; k++) s[k] += set.Ir[g][e][k];
                result[i][e] = s.Select(x => (float)(x / group.Count)).ToArray();
            }
        });
        return result.ToList();
    }

    /// <summary>The directions of the horizontal plane (elevation within 0.5 degrees), sorted by azimuth (degrees,
    /// counter-clockwise from the front).</summary>
    public static int[] HorizontalRing(DcsHrtfFile.Data d)
    {
        var ring = new List<(double Az, int I)>();
        for (var i = 0; i < d.DirectionCount; i++)
        {
            double x = d.Directions[i * 3], y = d.Directions[i * 3 + 1], z = d.Directions[i * 3 + 2];
            if (Math.Abs(Math.Asin(Math.Clamp(y, -1, 1))) * 180 / Math.PI > 0.5) continue;
            var az = Math.Atan2(-z, x) * 180 / Math.PI;
            ring.Add((az < 0 ? az + 360 : az, i));
        }
        return ring.OrderBy(p => p.Az).Select(p => p.I).ToArray();
    }

    /// <summary>The stored ITD (right onset - left onset) in microseconds.</summary>
    public static double StoredItdUs(DcsHrtfFile.Data d, int i) => (d.Delays[i * 2 + 1] - d.Delays[i * 2]) / d.SampleRate * 1e6;

    public static int Front(DcsHrtfFile.Data d)
    {
        var f = 0;
        for (var i = 0; i < d.DirectionCount; i++) if (d.Directions[i * 3] > d.Directions[f * 3]) f = i;
        return f;
    }
}
