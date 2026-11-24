namespace DcsVr.Core.Hrtf;

/// <summary>
/// Convex hull of measurement directions (unit vectors): the triangulation the renderer interpolates on. Incremental
/// hull with a deterministic radial perturbation (1e-9) so that the many co-planar quadrilaterals of a latitude-longitude
/// grid get one consistent split; the split is valid for the unperturbed points too (both halves lie in the same plane).
/// Triangles are counter-clockwise seen from outside. Deterministic: the same input gives the same triangles.
/// </summary>
public static class SphereHull
{
    sealed class Face
    {
        public int A, B, C;
        public double Nx, Ny, Nz, D;
        public bool Alive = true;
    }

    /// <summary>Triangles (t * 3 indices) of the hull of `points` (n * 3). Throws when the points are degenerate.</summary>
    public static int[] Build(double[] points)
    {
        var n = points.Length / 3;
        if (n < 4) throw new ArgumentException("At least 4 directions are needed.");
        // Perturbed copy: radius 1 + 1e-9 * u_i, u_i from a fixed integer hash of i.
        var p = new double[n * 3];
        for (var i = 0; i < n; i++)
        {
            var hash = (uint)i * 2654435761u;
            hash ^= hash >> 16;
            var scale = 1.0 + 1e-9 * (hash / (double)uint.MaxValue);
            for (var k = 0; k < 3; k++) p[i * 3 + k] = points[i * 3 + k] * scale;
        }
        double Dot(int i, double x, double y, double z) => p[i * 3] * x + p[i * 3 + 1] * y + p[i * 3 + 2] * z;
        double Dist2(int i, int j) { double dx = p[i * 3] - p[j * 3], dy = p[i * 3 + 1] - p[j * 3 + 1], dz = p[i * 3 + 2] - p[j * 3 + 2]; return dx * dx + dy * dy + dz * dz; }

        // Initial tetrahedron from extreme points.
        var i0 = 0;
        var i1 = Enumerable.Range(0, n).MaxBy(i => Dist2(i0, i));
        int i2 = -1; double best = -1;
        for (var i = 0; i < n; i++)
        {
            var c = Cross(Sub(p, i1, i0), Sub(p, i, i0));
            var len = c.x * c.x + c.y * c.y + c.z * c.z;
            if (len > best) { best = len; i2 = i; }
        }
        var nrm = Cross(Sub(p, i1, i0), Sub(p, i2, i0));
        int i3 = -1; best = -1;
        for (var i = 0; i < n; i++)
        {
            var d = Math.Abs((p[i * 3] - p[i0 * 3]) * nrm.x + (p[i * 3 + 1] - p[i0 * 3 + 1]) * nrm.y + (p[i * 3 + 2] - p[i0 * 3 + 2]) * nrm.z);
            if (d > best) { best = d; i3 = i; }
        }
        if (best < 1e-9) throw new ArgumentException("The directions are co-planar.");

        var faces = new List<Face>();
        var edges = new Dictionary<long, Face>();  // directed edge (u,v) -> its face
        static long Key(int u, int v) => ((long)u << 32) | (uint)v;
        double cx = (p[i0 * 3] + p[i1 * 3] + p[i2 * 3] + p[i3 * 3]) / 4, cy = (p[i0 * 3 + 1] + p[i1 * 3 + 1] + p[i2 * 3 + 1] + p[i3 * 3 + 1]) / 4,
               cz = (p[i0 * 3 + 2] + p[i1 * 3 + 2] + p[i2 * 3 + 2] + p[i3 * 3 + 2]) / 4;
        Face Make(int a, int b, int c)
        {
            var nn = Cross(Sub(p, b, a), Sub(p, c, a));
            var len = Math.Sqrt(nn.x * nn.x + nn.y * nn.y + nn.z * nn.z);
            var f = new Face { A = a, B = b, C = c, Nx = nn.x / len, Ny = nn.y / len, Nz = nn.z / len };
            f.D = Dot(a, f.Nx, f.Ny, f.Nz);
            return f;
        }
        void Add(Face f)
        {
            faces.Add(f);
            edges[Key(f.A, f.B)] = f; edges[Key(f.B, f.C)] = f; edges[Key(f.C, f.A)] = f;
        }
        foreach (var (a, b, c) in new[] { (i0, i1, i2), (i0, i3, i1), (i1, i3, i2), (i2, i3, i0) })
        {
            var f = Make(a, b, c);
            if (f.Nx * cx + f.Ny * cy + f.Nz * cz - f.D > 0) f = Make(a, c, b);  // outward
            Add(f);
        }

        var used = new bool[n];
        used[i0] = used[i1] = used[i2] = used[i3] = true;
        var visible = new List<Face>();
        var horizon = new List<(int u, int v)>();
        var alive = 4;
        for (var i = 0; i < n; i++)
        {
            if (used[i]) continue;
            visible.Clear();
            foreach (var f in faces)
                if (f.Alive && Dot(i, f.Nx, f.Ny, f.Nz) - f.D > 1e-13) visible.Add(f);
            if (visible.Count == 0) continue;  // inside (a near-duplicate): left out, reported by the caller
            used[i] = true;
            foreach (var f in visible) f.Alive = false;
            horizon.Clear();
            foreach (var f in visible)
                foreach (var (u, v) in new[] { (f.A, f.B), (f.B, f.C), (f.C, f.A) })
                    if (edges.TryGetValue(Key(v, u), out var twin) && twin.Alive) horizon.Add((u, v));
            foreach (var f in visible) { edges.Remove(Key(f.A, f.B)); edges.Remove(Key(f.B, f.C)); edges.Remove(Key(f.C, f.A)); }
            foreach (var (u, v) in horizon) Add(Make(u, v, i));
            alive += horizon.Count - visible.Count;
            if (faces.Count > 4 * alive + 64) faces.RemoveAll(f => !f.Alive);
        }
        var result = new List<int>();
        foreach (var f in faces)
            if (f.Alive) { result.Add(f.A); result.Add(f.B); result.Add(f.C); }
        return result.ToArray();
    }

    static (double x, double y, double z) Sub(double[] p, int a, int b) => (p[a * 3] - p[b * 3], p[a * 3 + 1] - p[b * 3 + 1], p[a * 3 + 2] - p[b * 3 + 2]);
    static (double x, double y, double z) Cross((double x, double y, double z) a, (double x, double y, double z) b) =>
        (a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x);
}
