using System.Buffers.Binary;
using System.Text;

namespace DcsVr.Core.Hrtf;

/// <summary>
/// The compact HRTF file format (".dcshrtf", version 1) the DCS audio module reads: the C# twin of
/// native/binaural/include/hrtf_format.h (layout, checksums and validation rules are identical; the native harness
/// loads a file written here, tests/DcsVr.Tests regenerates it byte for byte).
/// </summary>
public static class DcsHrtfFile
{
    public const uint Magic = 0x54524844u; // "DHRT"
    public const ushort Version = 1;
    public const int HeaderBytes = 256;
    public const string Extension = ".dcshrtf";
    public const uint MinSampleRate = 22050, MaxSampleRate = 192000;
    public const int MinDirections = 6, MaxDirections = 16384, MinTaps = 16, MaxTaps = 512;
    public const long MaxFileBytes = 64L << 20;

    [Flags]
    public enum FileFlags : uint { MinimumPhase = 1, DiffuseFieldEq = 2, FrontNormalised = 4, Personal = 8 }
    const uint KnownFlags = 15;

    /// <summary>The content of one file.</summary>
    public sealed record Data(
        FileFlags Flags, uint SampleRate, int Taps, float DatasetHeadRadiusM, float MeasurementDistanceM, float FrontGain,
        string Name, string Source,
        float[] Directions,   // n * 3 (x forward, y up, z right; unit)
        float[] Delays,       // n * 2 (left, right), samples at SampleRate
        uint[] Triangles,     // t * 3, counter-clockwise seen from outside
        float[] Ir)           // n * 2 * taps: [direction][ear][tap]
    {
        public int DirectionCount => Directions.Length / 3;
        public int TriangleCount => Triangles.Length / 3;
    }

    public static ulong Fnv1a(ReadOnlySpan<byte> data, ulong h = 0xcbf29ce484222325ul)
    {
        foreach (var b in data) { h ^= b; h *= 0x100000001b3ul; }
        return h;
    }

    public static long PayloadBytes(int directions, int triangles, int taps) =>
        (long)directions * (16 + 8) + (long)triangles * 12 + (long)directions * 2 * taps * 4;

    public static byte[] Write(Data d)
    {
        int n = d.DirectionCount, t = d.TriangleCount;
        if (d.Directions.Length != n * 3 || d.Delays.Length != n * 2 || d.Triangles.Length != t * 3 || d.Ir.Length != n * 2 * d.Taps)
            throw new ArgumentException("Inconsistent HRTF arrays.");
        var payload = PayloadBytes(n, t, d.Taps);
        var bytes = new byte[HeaderBytes + payload];
        var p = bytes.AsSpan(HeaderBytes);
        for (int i = 0; i < n; i++)
        {
            BinaryPrimitives.WriteSingleLittleEndian(p[(i * 16)..], d.Directions[i * 3]);
            BinaryPrimitives.WriteSingleLittleEndian(p[(i * 16 + 4)..], d.Directions[i * 3 + 1]);
            BinaryPrimitives.WriteSingleLittleEndian(p[(i * 16 + 8)..], d.Directions[i * 3 + 2]);
        }
        var o = n * 16;
        for (int i = 0; i < n * 2; i++) BinaryPrimitives.WriteSingleLittleEndian(p[(o + i * 4)..], d.Delays[i]);
        o += n * 8;
        for (int i = 0; i < t * 3; i++) BinaryPrimitives.WriteUInt32LittleEndian(p[(o + i * 4)..], d.Triangles[i]);
        o += t * 12;
        for (int i = 0; i < d.Ir.Length; i++) BinaryPrimitives.WriteSingleLittleEndian(p[(o + i * 4)..], d.Ir[i]);

        var h = bytes.AsSpan(0, HeaderBytes);
        BinaryPrimitives.WriteUInt32LittleEndian(h, Magic);
        BinaryPrimitives.WriteUInt16LittleEndian(h[4..], Version);
        BinaryPrimitives.WriteUInt16LittleEndian(h[6..], HeaderBytes);
        BinaryPrimitives.WriteUInt32LittleEndian(h[8..], (uint)d.Flags);
        BinaryPrimitives.WriteUInt32LittleEndian(h[12..], d.SampleRate);
        BinaryPrimitives.WriteUInt32LittleEndian(h[16..], (uint)n);
        BinaryPrimitives.WriteUInt32LittleEndian(h[20..], (uint)t);
        BinaryPrimitives.WriteUInt16LittleEndian(h[24..], (ushort)d.Taps);
        BinaryPrimitives.WriteSingleLittleEndian(h[28..], d.DatasetHeadRadiusM);
        BinaryPrimitives.WriteSingleLittleEndian(h[32..], d.MeasurementDistanceM);
        BinaryPrimitives.WriteSingleLittleEndian(h[36..], d.FrontGain);
        BinaryPrimitives.WriteUInt64LittleEndian(h[40..], (ulong)payload);
        BinaryPrimitives.WriteUInt64LittleEndian(h[48..], Fnv1a(bytes.AsSpan(HeaderBytes)));
        WriteString(h.Slice(64, 64), d.Name);
        WriteString(h.Slice(128, 96), d.Source);
        BinaryPrimitives.WriteUInt64LittleEndian(h[56..], Fnv1a(h));  // with the field still zero
        return bytes;
    }

    /// <summary>UTF-8, cut at a character boundary so it always fits with its terminating NUL.</summary>
    static void WriteString(Span<byte> field, string text)
    {
        var encoded = Encoding.UTF8.GetBytes(text);
        var length = Math.Min(encoded.Length, field.Length - 1);
        while (length > 0 && length < encoded.Length && (encoded[length] & 0xC0) == 0x80) length--;
        encoded.AsSpan(0, length).CopyTo(field);
    }

    static string ReadString(ReadOnlySpan<byte> field)
    {
        var end = field.IndexOf((byte)0);
        return Encoding.UTF8.GetString(end < 0 ? field : field[..end]);
    }

    /// <summary>The same checks as hrtf_format.h Validate, in the same order; null when valid, else the reason.</summary>
    public static string? Validate(ReadOnlySpan<byte> bytes)
    {
        if (bytes.Length < HeaderBytes) return "too small";
        if (bytes.Length > MaxFileBytes) return "too large";
        var h = bytes[..HeaderBytes];
        if (BinaryPrimitives.ReadUInt32LittleEndian(h) != Magic) return "not an HRTF file (magic)";
        if (BinaryPrimitives.ReadUInt16LittleEndian(h[4..]) != Version) return "unsupported version";
        if (BinaryPrimitives.ReadUInt16LittleEndian(h[6..]) != HeaderBytes) return "header size";
        Span<byte> copy = stackalloc byte[HeaderBytes];
        h.CopyTo(copy);
        copy.Slice(56, 8).Clear();
        if (Fnv1a(copy) != BinaryPrimitives.ReadUInt64LittleEndian(h[56..])) return "header checksum";
        var flags = BinaryPrimitives.ReadUInt32LittleEndian(h[8..]);
        if ((flags & ~KnownFlags) != 0) return "unknown flags";
        if ((flags & (uint)FileFlags.MinimumPhase) == 0) return "not minimum phase";
        var rate = BinaryPrimitives.ReadUInt32LittleEndian(h[12..]);
        if (rate < MinSampleRate || rate > MaxSampleRate) return "sample rate";
        var n = BinaryPrimitives.ReadUInt32LittleEndian(h[16..]);
        if (n < MinDirections || n > MaxDirections) return "direction count";
        var t = BinaryPrimitives.ReadUInt32LittleEndian(h[20..]);
        if (t < 4 || t > 2 * n) return "triangle count";
        var taps = BinaryPrimitives.ReadUInt16LittleEndian(h[24..]);
        if (taps < MinTaps || taps > MaxTaps) return "tap count";
        var radius = BinaryPrimitives.ReadSingleLittleEndian(h[28..]);
        if (!(radius >= 0.04f && radius <= 0.15f)) return "head radius";
        var distance = BinaryPrimitives.ReadSingleLittleEndian(h[32..]);
        if (!(distance >= 0.1f && distance <= 10.0f)) return "measurement distance";
        var frontGain = BinaryPrimitives.ReadSingleLittleEndian(h[36..]);
        if (!(frontGain > 0.0f) || !float.IsFinite(frontGain)) return "front gain";
        if (h.Slice(64, 64).IndexOf((byte)0) < 0 || h.Slice(128, 96).IndexOf((byte)0) < 0) return "strings";
        if (BinaryPrimitives.ReadUInt16LittleEndian(h[26..]) != 0) return "reserved field";
        var payload = BinaryPrimitives.ReadUInt64LittleEndian(h[40..]);
        if ((long)payload != PayloadBytes((int)n, (int)t, taps)) return "payload size field";
        if (bytes.Length != HeaderBytes + (long)payload) return "file size";
        if (Fnv1a(bytes[HeaderBytes..]) != BinaryPrimitives.ReadUInt64LittleEndian(h[48..])) return "payload checksum";
        var p = bytes[HeaderBytes..];
        var maxDelay = 0.01f * rate;
        for (int i = 0; i < n; i++)
        {
            float x = BinaryPrimitives.ReadSingleLittleEndian(p[(i * 16)..]), y = BinaryPrimitives.ReadSingleLittleEndian(p[(i * 16 + 4)..]),
                  z = BinaryPrimitives.ReadSingleLittleEndian(p[(i * 16 + 8)..]);
            if (!float.IsFinite(x) || !float.IsFinite(y) || !float.IsFinite(z)) return "direction not finite";
            if (MathF.Abs(x * x + y * y + z * z - 1.0f) > 1e-3f) return "direction not unit";
            float l = BinaryPrimitives.ReadSingleLittleEndian(p[(int)(n * 16 + i * 8)..]), r = BinaryPrimitives.ReadSingleLittleEndian(p[(int)(n * 16 + i * 8 + 4)..]);
            if (!(l >= 0 && l <= maxDelay && r >= 0 && r <= maxDelay)) return "delay";
        }
        var to = (int)(n * 24);
        for (int i = 0; i < t; i++)
        {
            uint a = BinaryPrimitives.ReadUInt32LittleEndian(p[(to + i * 12)..]), b = BinaryPrimitives.ReadUInt32LittleEndian(p[(to + i * 12 + 4)..]),
                 c = BinaryPrimitives.ReadUInt32LittleEndian(p[(to + i * 12 + 8)..]);
            if (a >= n || b >= n || c >= n || a == b || b == c || a == c) return "triangle index";
        }
        var io = to + (int)t * 12;
        for (long i = 0; i < (long)n * 2 * taps; i++)
        {
            var v = BinaryPrimitives.ReadSingleLittleEndian(p[(int)(io + i * 4)..]);
            if (!float.IsFinite(v) || MathF.Abs(v) > 64.0f) return "impulse response value";
        }
        return null;
    }

    /// <summary>Reads a valid file (throws InvalidDataException with the reason otherwise).</summary>
    public static Data Read(ReadOnlySpan<byte> bytes)
    {
        if (Validate(bytes) is { } why) throw new InvalidDataException("Not a valid HRTF file: " + why + ".");
        var h = bytes[..HeaderBytes];
        int n = (int)BinaryPrimitives.ReadUInt32LittleEndian(h[16..]), t = (int)BinaryPrimitives.ReadUInt32LittleEndian(h[20..]);
        int taps = BinaryPrimitives.ReadUInt16LittleEndian(h[24..]);
        var p = bytes[HeaderBytes..];
        var dirs = new float[n * 3];
        for (int i = 0; i < n; i++)
            for (int k = 0; k < 3; k++) dirs[i * 3 + k] = BinaryPrimitives.ReadSingleLittleEndian(p[(i * 16 + k * 4)..]);
        var delays = new float[n * 2];
        for (int i = 0; i < n * 2; i++) delays[i] = BinaryPrimitives.ReadSingleLittleEndian(p[(n * 16 + i * 4)..]);
        var tris = new uint[t * 3];
        for (int i = 0; i < t * 3; i++) tris[i] = BinaryPrimitives.ReadUInt32LittleEndian(p[(n * 24 + i * 4)..]);
        var ir = new float[n * 2 * taps];
        var io = n * 24 + t * 12;
        for (int i = 0; i < ir.Length; i++) ir[i] = BinaryPrimitives.ReadSingleLittleEndian(p[(io + i * 4)..]);
        return new Data((FileFlags)BinaryPrimitives.ReadUInt32LittleEndian(h[8..]), BinaryPrimitives.ReadUInt32LittleEndian(h[12..]), taps,
            BinaryPrimitives.ReadSingleLittleEndian(h[28..]), BinaryPrimitives.ReadSingleLittleEndian(h[32..]), BinaryPrimitives.ReadSingleLittleEndian(h[36..]),
            ReadString(h.Slice(64, 64)), ReadString(h.Slice(128, 96)), dirs, delays, tris, ir);
    }
}
