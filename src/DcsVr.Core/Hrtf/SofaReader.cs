using PureHDF;
using PureHDF.VOL.Native;

namespace DcsVr.Core.Hrtf;

/// <summary>What <see cref="SofaReader"/> read: the HRIRs in DCS's head frame, the SOFA convention it found and what it
/// assumed or changed on the way (shown to the pilot with the converter's own warnings).</summary>
public sealed record SofaReadResult(HrirSet Set, string Convention, IReadOnlyList<string> Warnings);

/// <summary>
/// AES69 SOFA (netCDF-4 / HDF5) -> <see cref="HrirSet"/>, the converter's input. Read with PureHDF (MIT, no native code;
/// docs/THIRD_PARTY.md). Accepts what a SimpleFreeFieldHRIR file may hold:
///   - Data.IR [M R N] (or [M R N 1] of the FIR-E files), float or double, R = 2 ears;
///   - Data.SamplingRate [I] or [M] (all equal), in hertz (kHz understood);
///   - Data.Delay [I R], [M R] or [R], in samples, optional;
///   - SourcePosition [I C] or [M C], spherical (azimuth counter-clockwise, elevation; degree or radian; distance in
///     metre, cm or mm) or cartesian (x front, y left, z up), relative to ListenerPosition, ListenerView and ListenerUp
///     (all optional: origin, +x, +z);
///   - ReceiverPosition [R C ...] tells which receiver is the left ear (the one with the larger y); without it receiver
///     0 is the left ear, as the convention says.
/// GeneralFIR and FreeFieldHRIR files with the same shape are read too (a warning says so). Every global attribute is
/// optional. Anything else is refused with a <see cref="HrtfConversionException"/> whose message says what is wrong.
/// The file's path never goes into the result.
/// </summary>
public static class SofaReader
{
    /// <summary>Above this many IR samples (M x 2 x N) a file is refused instead of read (256 MB as double).</summary>
    public const long MaxSamples = 32L << 20;

    public static SofaReadResult Read(string path, string? name = null, string? source = null)
    {
        if (!File.Exists(path)) throw new HrtfConversionException("The SOFA file was not found.");
        using var stream = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.Read);
        return Read(stream, name ?? Path.GetFileNameWithoutExtension(path), source);
    }

    public static SofaReadResult Read(Stream stream, string? name = null, string? source = null)
    {
        var head = new byte[8];
        var got = stream.Read(head, 0, head.Length);
        stream.Position = 0;
        if (got >= 4 && head[0] == 'C' && head[1] == 'D' && head[2] == 'F' && head[3] is 1 or 2 or 5)
            throw new HrtfConversionException("This is a netCDF-3 file; SOFA files are netCDF-4 (HDF5). Save it again as SOFA from the tool that made it.");
        NativeFile file;
        try { file = H5File.Open(stream, leaveOpen: true); }
        catch (Exception e) when (e is not HrtfConversionException) { throw new HrtfConversionException("This is not a SOFA file (it cannot be read as HDF5: " + Short(e) + ")."); }
        using (file)
        {
            try { return ReadFile(file, name, source); }
            catch (HrtfConversionException) { throw; }
            catch (Exception e) { throw new HrtfConversionException("The SOFA file could not be read: " + Short(e) + "."); }
        }
    }

    static SofaReadResult ReadFile(NativeFile file, string? name, string? source)
    {
        var warnings = new List<string>();
        var conventions = Text(file, "Conventions");
        var convention = Text(file, "SOFAConventions") ?? "";
        var dataType = Text(file, "DataType") ?? "";
        if (conventions is not null && !conventions.Equals("SOFA", StringComparison.OrdinalIgnoreCase))
            throw new HrtfConversionException($"This is not a SOFA file (Conventions is \"{conventions}\").");
        if (dataType.Length > 0 && !dataType.StartsWith("FIR", StringComparison.OrdinalIgnoreCase))
            throw new HrtfConversionException(dataType.StartsWith("TF", StringComparison.OrdinalIgnoreCase)
                ? $"The file holds transfer functions ({convention}, {dataType}), not impulse responses. A SimpleFreeFieldHRIR file is needed."
                : $"The file holds {dataType} data ({convention}), not impulse responses. A SimpleFreeFieldHRIR file is needed.");
        if (convention.Length > 0 && !convention.Equals("SimpleFreeFieldHRIR", StringComparison.OrdinalIgnoreCase))
        {
            if (convention.Equals("GeneralFIR", StringComparison.OrdinalIgnoreCase) || convention.Equals("FreeFieldHRIR", StringComparison.OrdinalIgnoreCase) || convention.Equals("GeneralFIR-E", StringComparison.OrdinalIgnoreCase))
                warnings.Add($"The file is {convention}, read as SimpleFreeFieldHRIR (head-related impulse responses).");
            else
                throw new HrtfConversionException($"The file is a SOFA {convention} file. DCS Control reads SimpleFreeFieldHRIR files (head-related impulse responses).");
        }
        if (conventions is null || convention.Length == 0) warnings.Add("The file does not say which SOFA convention it follows; read as SimpleFreeFieldHRIR.");

        // Data.IR.
        if (!file.LinkExists("Data.IR")) throw new HrtfConversionException("The file has no impulse responses (Data.IR).");
        var irSet = file.Dataset("Data.IR");
        var dims = irSet.Space.Dimensions;
        if (dims.Length == 4 && dims[3] == 1) dims = dims[..3];
        if (dims.Length != 3) throw new HrtfConversionException($"Data.IR has {irSet.Space.Dimensions.Length} dimensions; [measurements, ears, samples] is needed.");
        if (dims[1] != 2) throw new HrtfConversionException($"The file has {dims[1]} receivers; two ears are needed.");
        if (dims[0] == 0 || dims[2] == 0) throw new HrtfConversionException("Data.IR is empty.");
        if ((double)dims[0] * dims[1] * dims[2] > MaxSamples) throw new HrtfConversionException($"The file is too large ({dims[0]} directions of {dims[2]} samples).");
        int m = (int)dims[0], n = (int)dims[2];
        var raw = Doubles(irSet, "Data.IR");
        if (raw.Length != m * 2 * n) throw new HrtfConversionException("Data.IR could not be read whole.");

        // Sampling rate.
        if (!file.LinkExists("Data.SamplingRate")) throw new HrtfConversionException("The file does not give its sample rate (Data.SamplingRate).");
        var rateSet = file.Dataset("Data.SamplingRate");
        var rates = Doubles(rateSet, "Data.SamplingRate");
        if (rates.Length != 1 && rates.Length != m) throw new HrtfConversionException("Data.SamplingRate has an unexpected shape.");
        if (rates.Any(r => !double.IsFinite(r) || r <= 0)) throw new HrtfConversionException("Data.SamplingRate is not a valid rate.");
        if (rates.Any(r => r != rates[0])) throw new HrtfConversionException("The measurements use different sample rates.");
        var rate = rates[0];
        var rateUnit = (Text(rateSet, "Units") ?? "hertz").Trim().ToLowerInvariant();
        if (rateUnit is "kilohertz" or "khz") rate *= 1000;
        else if (rateUnit is not ("hertz" or "hz" or "")) throw new HrtfConversionException($"Data.SamplingRate is in \"{rateUnit}\"; hertz is needed.");

        // Data.Delay (optional; in samples).
        double[][]? delay = null;
        if (file.LinkExists("Data.Delay"))
        {
            var d = Doubles(file.Dataset("Data.Delay"), "Data.Delay");
            if (d.Length != 2 && d.Length != 2 * m) throw new HrtfConversionException("Data.Delay has an unexpected shape (one delay per ear, or per measurement and ear, is needed).");
            if (d.Any(v => !double.IsFinite(v))) throw new HrtfConversionException("Data.Delay has values that are not numbers.");
            if (d.Any(v => v < 0)) throw new HrtfConversionException("Data.Delay has negative delays.");
            if (d.Any(v => v != 0))
                delay = Enumerable.Range(0, m).Select(i => d.Length == 2 ? new[] { d[0], d[1] } : new[] { d[i * 2], d[i * 2 + 1] }).ToArray();
        }

        // Positions.
        if (!file.LinkExists("SourcePosition")) throw new HrtfConversionException("The file has no source positions (SourcePosition).");
        var sourcePositions = Positions(file, "SourcePosition", m, warnings, spherical: true, required: true)!;
        if (m > 1 && sourcePositions.Length == 1) throw new HrtfConversionException("Every measurement has the same source position.");
        var listener = Positions(file, "ListenerPosition", m, warnings, spherical: false, required: false);
        var view = Positions(file, "ListenerView", m, warnings, spherical: false, required: false, direction: true);
        var up = Positions(file, "ListenerUp", m, warnings, spherical: false, required: false, direction: true);

        var directions = new double[m][];
        var distances = new double[m];
        var zeroDistance = 0;
        for (var i = 0; i < m; i++)
        {
            var (s, hasDistance) = sourcePositions[sourcePositions.Length == 1 ? 0 : i];
            if (!hasDistance) zeroDistance++;
            double[] r = [s[0], s[1], s[2]];
            if (listener is not null && hasDistance) { var l = listener[listener.Length == 1 ? 0 : i].Value; for (var k = 0; k < 3; k++) r[k] -= l[k]; }
            var (fwd, left, top) = Frame(view?[view.Length == 1 ? 0 : i].Value, up?[up.Length == 1 ? 0 : i].Value);
            double x = Dot(r, fwd), y = Dot(r, left), z = Dot(r, top);
            var length = Math.Sqrt(x * x + y * y + z * z);
            if (!(length > 1e-9) || !double.IsFinite(length)) throw new HrtfConversionException($"Measurement {i + 1} has no direction (its source is at the listener).");
            distances[i] = hasDistance ? length : 0;
            directions[i] = [x / length, z / length, -y / length];  // SOFA x front, y left, z up -> head frame x front, y up, z right
        }
        var known = distances.Where(v => v > 0).Order().ToArray();
        var distance = known.Length > 0 ? known[known.Length / 2] : 0;
        if (zeroDistance > 0 || !(distance >= 0.1 && distance <= 10))
        {
            warnings.Add(known.Length == 0 || zeroDistance > 0 ? "The file gives no measurement distance; 1.2 m is assumed." : $"The measurement distance ({distance:0.###} m) is unusual; 1.2 m is assumed.");
            distance = 1.2;
        }

        // Which receiver is the left ear: the one further along +y (SOFA's left) in the listener's frame.
        var swap = false;
        if (file.LinkExists("ReceiverPosition"))
        {
            var rp = file.Dataset("ReceiverPosition");
            var rd = rp.Space.Dimensions;
            var values = Doubles(rp, "ReceiverPosition");
            if (rd.Length >= 2 && rd[0] == 2 && rd[1] == 3 && values.Length >= 6)
            {
                // [R C I] / [R C M] / [R C]: the first measurement's two receivers.
                var stride = values.Length / 6;
                double[] r0 = [values[0 * stride], values[1 * stride], values[2 * stride]], r1 = [values[3 * stride], values[4 * stride], values[5 * stride]];
                if (IsSpherical(rp)) { r0 = Cartesian(r0, Units(rp)); r1 = Cartesian(r1, Units(rp)); }
                if (r0[1] < r1[1] - 1e-6) { swap = true; warnings.Add("Receiver 1 is the right ear in this file; the ears were swapped."); }
            }
        }

        var ir = new float[m][][];
        for (var i = 0; i < m; i++)
        {
            ir[i] = new float[2][];
            for (var e = 0; e < 2; e++)
            {
                var from = (i * 2 + (swap ? 1 - e : e)) * n;
                var a = new float[n];
                for (var k = 0; k < n; k++) a[k] = (float)raw[from + k];
                ir[i][e] = a;
            }
            if (swap && delay is not null) delay[i] = [delay[i][1], delay[i][0]];
        }

        var title = Text(file, "Title");
        var database = Text(file, "DatabaseName");
        var subject = Text(file, "ListenerShortName");
        var defaultSource = string.Join(" ", new[] { database, subject }.Where(s => !string.IsNullOrWhiteSpace(s)).Select(s => s!.Trim()));
        if (defaultSource.Length == 0) defaultSource = title?.Trim() ?? "";
        if (Text(file, "Organization") is { } organization && organization.Trim().Length > 0)
            defaultSource = (defaultSource.Length > 0 ? defaultSource + ", " : "") + organization.Trim().TrimEnd('.');
        var set = new HrirSet
        {
            SampleRate = rate, Directions = directions, Ir = ir, Delay = delay, MeasurementDistanceM = distance,
            Name = Clean(name ?? title ?? subject ?? "SOFA"), Source = Clean(source ?? (defaultSource.Length > 0 ? defaultSource : "SOFA file")),
        };
        return new(set, convention.Length > 0 ? convention : "SimpleFreeFieldHRIR", warnings);
    }

    /// <summary>A position variable as cartesian metres in SOFA's frame (x front, y left, z up), one row per I or M, and
    /// whether the row carries a distance (a spherical position with radius 0 gives only a direction). Null when the
    /// variable is absent and not required.</summary>
    static (double[] Value, bool HasDistance)[]? Positions(NativeFile file, string variable, int m, List<string> warnings, bool spherical, bool required, bool direction = false)
    {
        if (!file.LinkExists(variable)) return required ? throw new HrtfConversionException($"The file has no {variable}.") : null;
        var set = file.Dataset(variable);
        var dims = set.Space.Dimensions;
        var values = Doubles(set, variable);
        if (dims.Length is < 1 or > 3 || values.Length % 3 != 0 || (dims.Length >= 2 && dims[1] != 3 && dims[^1] != 3))
            throw new HrtfConversionException($"{variable} has an unexpected shape.");
        var rows = values.Length / 3;
        if (rows != 1 && rows != m) throw new HrtfConversionException($"{variable} has {rows} rows for {m} measurements.");
        var type = Text(set, "Type")?.Trim().ToLowerInvariant();
        var units = Units(set);
        bool isSpherical;
        if (type is null)
        {
            isSpherical = spherical && (units.Length == 0 || units.Any(u => u.StartsWith("deg") || u.StartsWith("rad")));
            if (variable == "SourcePosition") warnings.Add($"SourcePosition does not say its coordinate type; read as {(isSpherical ? "spherical" : "cartesian")}.");
        }
        else if (type == "spherical") isSpherical = true;
        else if (type == "cartesian") isSpherical = false;
        else throw new HrtfConversionException($"{variable} uses \"{type}\" coordinates; spherical or cartesian are needed.");
        if (isSpherical && units.Length == 0 && variable == "SourcePosition") warnings.Add("SourcePosition does not give its units; degrees and metres are assumed.");
        var result = new (double[], bool)[rows];
        for (var r = 0; r < rows; r++)
        {
            double[] v = [values[r * 3], values[r * 3 + 1], values[r * 3 + 2]];
            if (v.Any(x => !double.IsFinite(x))) throw new HrtfConversionException($"{variable} has values that are not numbers.");
            if (isSpherical)
            {
                var hasDistance = Math.Abs(v[2]) > 1e-12;
                if (!hasDistance) v[2] = 1;
                result[r] = (Cartesian(v, units), hasDistance && !direction);
            }
            else
            {
                var scale = direction ? 1 : DistanceScale(units.LastOrDefault() ?? "metre", variable);
                result[r] = ([v[0] * scale, v[1] * scale, v[2] * scale], !direction);
            }
        }
        return result;
    }

    static bool IsSpherical(IH5Dataset set) => Text(set, "Type")?.Trim().Equals("spherical", StringComparison.OrdinalIgnoreCase) == true;

    /// <summary>The Units attribute split at commas ("degree, degree, metre"), lower case.</summary>
    static string[] Units(IH5Dataset set) =>
        (Text(set, "Units") ?? "").Split([',', ' '], StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries).Select(u => u.ToLowerInvariant()).ToArray();

    /// <summary>Spherical (azimuth, elevation, radius) to SOFA cartesian, the angle unit from the first unit token, the
    /// distance unit from the last.</summary>
    static double[] Cartesian(double[] v, string[] units)
    {
        var angle = units.FirstOrDefault() ?? "degree";
        var k = angle.StartsWith("rad") ? 1.0 : angle.StartsWith("deg") ? Math.PI / 180 : throw new HrtfConversionException($"Angles in \"{angle}\" are not understood (degree or radian).");
        var r = v[2] * (units.Length >= 3 ? DistanceScale(units[^1], "SourcePosition") : 1);
        double az = v[0] * k, el = v[1] * k;
        return [r * Math.Cos(el) * Math.Cos(az), r * Math.Cos(el) * Math.Sin(az), r * Math.Sin(el)];
    }

    static double DistanceScale(string unit, string variable) => unit switch
    {
        "metre" or "meter" or "metres" or "meters" or "m" => 1,
        "centimetre" or "centimeter" or "centimetres" or "centimeters" or "cm" => 0.01,
        "millimetre" or "millimeter" or "millimetres" or "millimeters" or "mm" => 0.001,
        _ => throw new HrtfConversionException($"{variable} gives distances in \"{unit}\"; metres are needed."),
    };

    /// <summary>The listener's orthonormal frame (front, left, up) from ListenerView and ListenerUp (+x and +z by default).</summary>
    static (double[] Front, double[] Left, double[] Up) Frame(double[]? view, double[]? up)
    {
        var f = Normal(view ?? [1, 0, 0]) ?? throw new HrtfConversionException("ListenerView has zero length.");
        var u0 = Normal(up ?? [0, 0, 1]) ?? throw new HrtfConversionException("ListenerUp has zero length.");
        var d = Dot(u0, f);
        var u = Normal([u0[0] - d * f[0], u0[1] - d * f[1], u0[2] - d * f[2]]) ?? throw new HrtfConversionException("ListenerUp points along ListenerView.");
        double[] l = [u[1] * f[2] - u[2] * f[1], u[2] * f[0] - u[0] * f[2], u[0] * f[1] - u[1] * f[0]];  // up x front
        return (f, l, u);
    }

    static double[]? Normal(double[] v)
    {
        var len = Math.Sqrt(Dot(v, v));
        return len > 1e-9 && double.IsFinite(len) ? [v[0] / len, v[1] / len, v[2] / len] : null;
    }

    static double Dot(double[] a, double[] b) => a[0] * b[0] + a[1] * b[1] + a[2] * b[2];

    /// <summary>A numeric dataset as doubles, whatever its stored type (float, double, signed or unsigned integers).</summary>
    static double[] Doubles(IH5Dataset set, string variable)
    {
        var type = set.Type;
        return (type.Class, type.Size) switch
        {
            (H5DataTypeClass.FloatingPoint, 8) => All<double>(set),
            (H5DataTypeClass.FloatingPoint, 4) => Array.ConvertAll(All<float>(set), v => (double)v),
            (H5DataTypeClass.FixedPoint, 1) => type.FixedPoint.IsSigned ? Array.ConvertAll(All<sbyte>(set), v => (double)v) : Array.ConvertAll(All<byte>(set), v => (double)v),
            (H5DataTypeClass.FixedPoint, 2) => type.FixedPoint.IsSigned ? Array.ConvertAll(All<short>(set), v => (double)v) : Array.ConvertAll(All<ushort>(set), v => (double)v),
            (H5DataTypeClass.FixedPoint, 4) => type.FixedPoint.IsSigned ? Array.ConvertAll(All<int>(set), v => (double)v) : Array.ConvertAll(All<uint>(set), v => (double)v),
            (H5DataTypeClass.FixedPoint, 8) => type.FixedPoint.IsSigned ? Array.ConvertAll(All<long>(set), v => (double)v) : Array.ConvertAll(All<ulong>(set), v => (double)v),
            _ => throw new HrtfConversionException($"{variable} is stored as {type.Class} ({type.Size} bytes), not as numbers."),
        };
    }

    /// <summary>The whole dataset. Each chunk is decoded once: PureHDF's default chunk cache is small, and a netCDF-4
    /// file's chunks rarely match the read order (SADIE II's 48 kHz file took a minute with it, 0.1 s with this).</summary>
    static T[] All<T>(IH5Dataset set) where T : unmanaged =>
        set is NativeDataset native ? native.Read<T[]>(new H5DatasetAccess(ChunkCache: new WholeChunkCache())) : set.Read<T[]>();

    sealed class WholeChunkCache : IReadingChunkCache
    {
        readonly Dictionary<ulong, Memory<byte>> chunks = [];
        public Memory<byte> GetChunk(ulong index, Func<Memory<byte>> chunkReader)
        {
            if (!chunks.TryGetValue(index, out var chunk)) chunks[index] = chunk = chunkReader();
            return chunk;
        }
    }

    /// <summary>A string attribute (fixed or variable length, scalar or a one-element array), or null when absent or not text.</summary>
    static string? Text(IH5Object o, string attribute)
    {
        try
        {
            if (!o.AttributeExists(attribute)) return null;
            var a = o.Attribute(attribute);
            // Fixed-length strings (netCDF-4) are class String; variable-length ones (h5py, PureHDF) VariableLength.
            if (a.Type.Class is not (H5DataTypeClass.String or H5DataTypeClass.VariableLength)) return null;
            var parts = a.Read<string[]>();
            var text = string.Concat(parts ?? []).TrimEnd('\0').Trim();
            return text;
        }
        catch (Exception e) when (e is not OutOfMemoryException) { return null; }
    }

    /// <summary>One line of printable text (the .dcshrtf header keeps UTF-8 up to its field size).</summary>
    static string Clean(string text) => new string(text.Select(c => char.IsControl(c) ? ' ' : c).ToArray()).Trim();

    static string Short(Exception e)
    {
        var message = e.Message.Split('\n')[0].Trim().TrimEnd('.');
        return message.Length > 160 ? message[..160] : message.Length > 0 ? message : e.GetType().Name;
    }
}
