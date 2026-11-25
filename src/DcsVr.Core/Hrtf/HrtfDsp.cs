using System.Numerics;

namespace DcsVr.Core.Hrtf;

/// <summary>
/// The converter's DSP: FFT, minimum phase from a magnitude (real cepstrum, the same steps as native/binaural/src/dsp.h),
/// onset delays (excess-phase slope) and fractional-octave smoothing. Pure functions; deterministic.
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

    /// <summary>Low edge of the band the onset delays are fitted in (hertz).</summary>
    public const double DelayBandLowHz = 200;
    /// <summary>High edge of that band: below it the phase difference between the ears carries the time cue (hearing
    /// follows fine-structure timing up to about 1.5 kHz); above it the pinna's lobes and notches make the phase useless.</summary>
    public const double DelayBandHighHz = 1500;

    /// <summary>
    /// Onset delay of `ir` in samples (fractional), as the renderer models it: `reference` (the minimum-phase IR that is
    /// played) followed by a pure delay. It is the slope of the excess phase, phase(ir) - phase(reference), fitted by least
    /// squares (with an offset) over 200-1500 Hz, so that the rendered response keeps the measured phase difference between
    /// the ears where hearing uses it. A full-band cross-correlation (the earlier method) locks onto the high-frequency
    /// lobes of a shadowed ear: 120 us RMS off the measured low-frequency ITD of SADIE II's KU100, the front 150 us to the
    /// left, and jumps of 200 us between neighbouring directions. In the low band the measured response is close to
    /// minimum phase plus a delay, so its excess phase is almost linear and smooth across directions.
    /// </summary>
    public static double ExcessPhaseDelay(ReadOnlySpan<float> ir, ReadOnlySpan<float> reference, double sampleRate)
    {
        // Bins at most ~12 Hz apart: about a hundred points in the band at any rate.
        var n = NextPow2(Math.Max(Math.Max(ir.Length, reference.Length) * 2, (int)Math.Ceiling(sampleRate / 12)));
        var a = new Complex[n];
        var b = new Complex[n];
        for (var i = 0; i < ir.Length; i++) a[i] = ir[i];
        for (var i = 0; i < reference.Length; i++) b[i] = reference[i];
        Fft(a, false);
        Fft(b, false);
        var hi = (int)Math.Floor(DelayBandHighHz * n / sampleRate);
        // Unwrapped from DC upwards (between bins the excess phase moves by far less than pi: 4 ms at 12 Hz is 0.3 rad).
        double previous = 0, unwrapped = 0, sx = 0, sy = 0, sxx = 0, sxy = 0;
        var count = 0;
        for (var k = 0; k <= hi; k++)
        {
            var p = (a[k] * Complex.Conjugate(b[k])).Phase;
            if (k == 0) unwrapped = p;
            else
            {
                var step = p - previous;
                unwrapped += step - 2 * Math.PI * Math.Round(step / (2 * Math.PI));
            }
            previous = p;
            var f = k * sampleRate / n;
            if (f < DelayBandLowHz) continue;
            var w = 2 * Math.PI * f;
            sx += w; sy += unwrapped; sxx += w * w; sxy += w * unwrapped; count++;
        }
        var den = count * sxx - sx * sx;
        var slope = den > 0 ? (count * sxy - sx * sy) / den : 0;  // phase per angular frequency: minus the delay in seconds
        return -slope * sampleRate;
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
