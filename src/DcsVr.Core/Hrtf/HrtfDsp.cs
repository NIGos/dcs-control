using System.Numerics;

namespace DcsVr.Core.Hrtf;

/// <summary>
/// The converter's DSP: FFT, minimum phase from a magnitude (real cepstrum, the same steps as native/binaural/src/dsp.h),
/// onset delays and fractional-octave smoothing. Pure functions; deterministic.
/// </summary>
public static class HrtfDsp
{
    public static int NextPow2(int n) { var p = 1; while (p < n) p <<= 1; return p; }

    /// <summary>In-place iterative radix-2 FFT; the inverse divides by n.</summary>
    public static void Fft(Complex[] a, bool inverse)
    {
        int n = a.Length;
        for (int i = 1, j = 0; i < n; i++)
        {
            var bit = n >> 1;
            for (; (j & bit) != 0; bit >>= 1) j ^= bit;
            j ^= bit;
            if (i < j) (a[i], a[j]) = (a[j], a[i]);
        }
        for (var len = 2; len <= n; len <<= 1)
        {
            var ang = 2 * Math.PI / len * (inverse ? 1 : -1);
            var wl = new Complex(Math.Cos(ang), Math.Sin(ang));
            for (var i = 0; i < n; i += len)
            {
                var w = Complex.One;
                for (var k = 0; k < len / 2; k++)
                {
                    var u = a[i + k];
                    var v = a[i + k + len / 2] * w;
                    a[i + k] = u + v;
                    a[i + k + len / 2] = u - v;
                    w *= wl;
                }
            }
        }
        if (inverse) for (var i = 0; i < n; i++) a[i] /= n;
    }

    /// <summary>Magnitude at bins 0..n/2 of x zero-padded to n.</summary>
    public static double[] Magnitude(ReadOnlySpan<float> x, int n)
    {
        var a = new Complex[n];
        for (var i = 0; i < x.Length && i < n; i++) a[i] = x[i];
        Fft(a, false);
        var m = new double[n / 2 + 1];
        for (var k = 0; k <= n / 2; k++) m[k] = a[k].Magnitude;
        return m;
    }

    /// <summary>
    /// Minimum-phase impulse response from magnitudes at bins 0..n/2 (floored at -140 dB of the peak), truncated to
    /// `taps` with a half-Hann window over the last quarter.
    /// </summary>
    public static float[] MinimumPhase(double[] mag, int n, int taps)
    {
        var peak = 0.0;
        foreach (var m in mag) peak = Math.Max(peak, m);
        var floor = Math.Max(peak, 1e-30) * 1e-7;
        var c = new Complex[n];
        for (var k = 0; k <= n / 2; k++)
        {
            var v = Math.Log(Math.Max(mag[k], floor));
            c[k] = v;
            if (k > 0 && k < n / 2) c[n - k] = v;
        }
        Fft(c, true);
        for (var i = 1; i < n / 2; i++) { c[i] = 2 * c[i].Real; c[n - i] = 0; }
        c[0] = c[0].Real;
        c[n / 2] = c[n / 2].Real;
        Fft(c, false);
        for (var i = 0; i < n; i++) c[i] = Complex.Exp(c[i]);
        Fft(c, true);
        var output = new float[taps];
        var fade = Math.Max(taps / 4, 1);
        for (var i = 0; i < taps && i < n; i++)
        {
            var w = 1.0;
            if (i >= taps - fade)
            {
                var t = (double)(i - (taps - fade) + 1) / (fade + 1);
                w = 0.5 * (1 + Math.Cos(Math.PI * t));
            }
            output[i] = (float)(c[i].Real * w);
        }
        return output;
    }

    /// <summary>
    /// Onset delay of an impulse response in samples (fractional), as the renderer models it: the lag that best aligns
    /// the response with its own minimum-phase version (cross-correlation, 8x band-limited upsampling). For a response
    /// that is exactly minimum phase plus a delay this is that delay; unlike a level threshold it is not pulled early by
    /// the pre-ringing of a band-limited onset nor late by a shadowed (low-passed) ear (Nam, Abel and Smith, "A method
    /// for estimating interaural time difference for binaural synthesis", AES 125th Convention, 2008).
    /// </summary>
    public static double MinimumPhaseDelay(ReadOnlySpan<float> ir)
    {
        var n = NextPow2(Math.Max(ir.Length, 16) * 2);
        var a = new Complex[n];
        for (var i = 0; i < ir.Length; i++) a[i] = ir[i];
        Fft(a, false);
        var mag = new double[n / 2 + 1];
        for (var k = 0; k <= n / 2; k++) mag[k] = a[k].Magnitude;
        var mp = MinimumPhase(mag, n, n);
        var b = new Complex[n];
        for (var i = 0; i < n; i++) b[i] = mp[i];
        Fft(b, false);
        const int up = 8;
        var x = new Complex[n * up];  // cross-spectrum, zero-padded: the cross-correlation upsampled 8x
        for (var k = 0; k <= n / 2; k++)
        {
            var v = a[k] * Complex.Conjugate(b[k]);
            if (k == n / 2) { x[k] = v / 2; x[n * up - k] = Complex.Conjugate(v) / 2; }
            else { x[k] = v; if (k > 0) x[n * up - k] = Complex.Conjugate(v); }
        }
        Fft(x, true);
        int best = 0; var bestValue = double.MinValue;
        for (var i = 0; i < x.Length; i++)
        {
            var lag = i <= x.Length / 2 ? i : i - x.Length;
            if (lag < -up * 4) continue;  // a minimum-phase response leads the measured one: lags are >= 0 (a few samples of slack)
            if (x[i].Real > bestValue) { bestValue = x[i].Real; best = lag; }
        }
        // Parabolic refinement between the upsampled points.
        int Idx(int lag) => ((lag % x.Length) + x.Length) % x.Length;
        double y0 = x[Idx(best - 1)].Real, y1 = x[Idx(best)].Real, y2 = x[Idx(best + 1)].Real, den = y0 - 2 * y1 + y2;
        var frac = Math.Abs(den) > 1e-30 ? 0.5 * (y0 - y2) / den : 0;
        return Math.Max(0, (best + frac) / up);
    }

    /// <summary>Power spectrum smoothed over 1/`fraction` octave (centred, bins 1..n/2; bin 0 kept).</summary>
    public static double[] SmoothFractionalOctave(double[] power, int n, double sampleRate, double fraction)
    {
        var half = Math.Pow(2, 0.5 / fraction);
        var prefix = new double[power.Length + 1];
        for (var k = 0; k < power.Length; k++) prefix[k + 1] = prefix[k] + power[k];
        var output = new double[power.Length];
        output[0] = power[0];
        for (var k = 1; k < power.Length; k++)
        {
            int lo = Math.Max(1, (int)Math.Floor(k / half)), hi = Math.Min(power.Length - 1, (int)Math.Ceiling(k * half));
            output[k] = (prefix[hi + 1] - prefix[lo]) / (hi - lo + 1);
        }
        return output;
    }
}
