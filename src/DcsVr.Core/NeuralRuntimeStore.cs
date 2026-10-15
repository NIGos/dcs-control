using System.Text.Json;

namespace DcsVr.Core;

/// <summary>What is recorded next to the saved copy (nvngx_dlssnr.json).</summary>
/// <param name="Path">The saved copy itself.</param>
/// <param name="OriginalPath">The file the user selected or Detect found.</param>
/// <param name="Forgotten">The user chose Forget while an applied profile was installed from this copy: new drafts no
/// longer use it, and the file is deleted when that profile is restored.</param>
/// <param name="Modified">NVIDIA's runtime changed by a compatibility patch (RTX 20, 30, 40): no valid NVIDIA signature.</param>
public sealed record SavedNeuralRuntime(string Path, string? Version, string Sha256, long Bytes, string OriginalPath, DateTimeOffset SavedAt, bool Forgotten = false, bool Modified = false);

/// <summary>
/// The app's own copy of the user-supplied DLSS 5 runtime (state root\runtimes\nvngx_dlssnr.dll), so the file is selected
/// once. Only a file that passes the deployment checks (x64, DLSS-NR 310.8, NVIDIA Corporation signature or NVIDIA's file
/// changed by a compatibility patch) is saved; only the latest is kept. A profile without a runtime path, or with a path that no longer exists, uses this copy.
/// </summary>
public sealed class NeuralRuntimeStore(string directory)
{
    public const string FileName = "nvngx_dlssnr.dll";
    public string Directory { get; } = System.IO.Path.GetFullPath(directory);
    public string DllPath => System.IO.Path.Combine(Directory, FileName);
    public string InfoPath => System.IO.Path.Combine(Directory, "nvngx_dlssnr.json");

    /// <summary>The record as saved, also when forgotten; null when there is no complete saved copy. Cheap: the file
    /// length is compared, the hash is checked when the copy is deployed.</summary>
    public SavedNeuralRuntime? Read()
    {
        try
        {
            if (!File.Exists(InfoPath) || !File.Exists(DllPath)) return null;
            var info = JsonData.Deserialize<SavedNeuralRuntime>(File.ReadAllText(InfoPath));
            return new FileInfo(DllPath).Length == info.Bytes && info.Sha256.Length == 64 ? info with { Path = DllPath } : null;
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException or JsonException or InvalidDataException or NotSupportedException) { return null; }
    }

    /// <summary>The saved copy new drafts use: present and not forgotten.</summary>
    public SavedNeuralRuntime? Current() => Read() is { Forgotten: false } saved ? saved : null;

    /// <summary>Verifies <paramref name="source"/> exactly like deployment and saves it as the copy, replacing an older one.
    /// The same file already saved is left as it is (a forgotten copy is used again).</summary>
    public SavedNeuralRuntime Save(string source)
    {
        var original = System.IO.Path.GetFullPath(source);
        if (!System.IO.Path.GetFileName(original).Equals(FileName, StringComparison.OrdinalIgnoreCase))
            throw new InvalidDataException("The DLSS 5 runtime must be named nvngx_dlssnr.dll.");
        var bytes = File.ReadAllBytes(original);
        var sha = Hashing.BytesSha256(bytes);
        // The same file already saved (and the copy still intact): nothing is written.
        if (Read() is { } existing && existing.Sha256 == sha && Hashing.FileSha256(DllPath) == sha)
        {
            if (existing.Forgotten) { existing = existing with { Forgotten = false }; AtomicFile.WriteText(InfoPath, JsonData.Serialize(existing)); }
            return existing;
        }
        // The user selected this file: NVIDIA's runtime patched for RTX 20, 30 and 40 cards is accepted and recorded as such.
        var verification = NativeBinary.VerifyNeuralRuntime(bytes, allowModified: true);
        // NVIDIA writes the version as "310,8,0,0"; it is recorded and shown as 310.8.0.0.
        var version = verification.Version?.Replace(" ", "").Replace(',', '.');
        var saved = new SavedNeuralRuntime(DllPath, version, sha, bytes.LongLength, original, DateTimeOffset.UtcNow, Modified: NativeBinary.IsModifiedRuntime(verification));
        // Each file is replaced atomically; the record is written last, so a copy without a matching record is never used.
        try { File.Delete(InfoPath); } catch (DirectoryNotFoundException) { }
        AtomicFile.Write(DllPath, bytes);
        AtomicFile.WriteText(InfoPath, JsonData.Serialize(saved));
        return saved;
    }

    /// <summary>Deletes the saved copy, or with <paramref name="keepFile"/> only stops new drafts from using it.</summary>
    public void Forget(bool keepFile)
    {
        if (Read() is not { } saved) { Delete(); return; }
        if (keepFile) AtomicFile.WriteText(InfoPath, JsonData.Serialize(saved with { Forgotten = true }));
        else Delete();
    }

    /// <summary>Deletes a forgotten copy once nothing applied was installed from it.</summary>
    public void RemoveForgotten() { if (Read() is { Forgotten: true }) Delete(); }

    private void Delete()
    {
        foreach (var path in new[] { InfoPath, DllPath })
            try { File.Delete(path); } catch (DirectoryNotFoundException) { }
    }
}
