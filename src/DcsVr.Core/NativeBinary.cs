using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Security.Cryptography.X509Certificates;

namespace DcsVr.Core;

public sealed record BinaryInspection(bool IsX64, bool TrustedSignature, string? Signer, string? Version, string Sha256);

public static class NativeBinary
{
    public static bool IsX64(string path)
    {
        try
        {
            using var reader = new BinaryReader(File.OpenRead(path));
            if (reader.BaseStream.Length < 64 || reader.ReadUInt16() != 0x5a4d) return false;
            reader.BaseStream.Position = 60; var offset = reader.ReadInt32();
            if (offset < 64 || offset > reader.BaseStream.Length - 24) return false;
            reader.BaseStream.Position = offset;
            return reader.ReadUInt32() == 0x4550 && reader.ReadUInt16() == 0x8664;
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException) { return false; }
    }
    public static bool SupportsNeuralContract(string? version)
    {
        var parts = version?.Replace(',', '.').Replace(" ", "").Split('.');
        // 310.9 (NVIDIA's faster, smaller model) keeps 310.8's exports and feature contract.
        return parts is { Length: >= 2 } && parts[0] == "310" && parts[1] is "8" or "9";
    }
    /// <summary>True only when the certificate organization is exactly NVIDIA Corporation, not any subject containing the word.</summary>
    public static bool IsNvidiaSigner(string? subject)
    {
        if (string.IsNullOrWhiteSpace(subject)) return false;
        try
        {
            return new X500DistinguishedName(subject).EnumerateRelativeDistinguishedNames()
                .Any(rdn => !rdn.HasMultipleElements && rdn.GetSingleElementType().Value == "2.5.4.10"
                    && string.Equals(rdn.GetSingleElementValue(), "NVIDIA Corporation", StringComparison.Ordinal));
        }
        catch (CryptographicException) { return false; }
    }
    /// <summary>The DLSS 5 runtime checks used for deployment and for the saved copy: the bytes are written to a private
    /// file, so the checked file cannot be swapped between the checks and their use, then they must be an x64 PE with the
    /// DLSS-NR 310.8 contract (310.8 or 310.9) and a verifiable signature from organisation NVIDIA Corporation. With
    /// <paramref name="allowModified"/> (a file the user selected) NVIDIA's runtime changed by a compatibility patch is
    /// accepted too: the patches that run it on RTX 20, 30 and 40 cards rewrite its GPU code, which breaks NVIDIA's
    /// signature. Such a file must carry no signature or NVIDIA's (now invalid) one; a file validly signed by anyone, or
    /// signed by anyone but NVIDIA, is not NVIDIA's runtime and is refused.</summary>
    public static BinaryInspection VerifyNeuralRuntime(byte[] bytes, bool allowModified = false)
    {
        var privateCopy = Path.Combine(Path.GetTempPath(), "dcsvr-" + Guid.NewGuid().ToString("N"), "nvngx_dlssnr.dll");
        BinaryInspection verification;
        try
        {
            AtomicFile.Write(privateCopy, bytes);
            verification = Inspect(privateCopy);
        }
        finally { try { Directory.Delete(Path.GetDirectoryName(privateCopy)!, recursive: true); } catch (IOException) { } catch (UnauthorizedAccessException) { } }
        if (verification.Sha256 != Hashing.BytesSha256(bytes)) throw new InvalidDataException("The DLSS 5 runtime changed during verification. Select it again.");
        if (!verification.IsX64) throw new InvalidDataException("The DLSS 5 runtime must be the x64 nvngx_dlssnr.dll.");
        if (!SupportsNeuralContract(verification.Version))
            throw new InvalidDataException($"DLSS 5 supports nvngx_dlssnr.dll version 310.8 or 310.9; the selected file is version {verification.Version ?? "unknown"}.");
        if (IsModifiedRuntime(verification))
        {
            var foreign = verification.TrustedSignature || (verification.Signer is not null && !IsNvidiaSigner(verification.Signer));
            if (!allowModified || foreign)
                throw new InvalidDataException(allowModified
                    ? "This is not NVIDIA's nvngx_dlssnr.dll: it is signed by someone else."
                    : "The DLSS 5 runtime must be x64 with a verifiable NVIDIA signature.");
        }
        return verification;
    }

    /// <summary>True for a runtime without a valid NVIDIA signature: NVIDIA's file changed by a compatibility patch.</summary>
    public static bool IsModifiedRuntime(BinaryInspection verification) => !(verification.TrustedSignature && IsNvidiaSigner(verification.Signer));
    public static BinaryInspection Inspect(string path)
    {
        var bytes = File.ReadAllBytes(path);
        var x64 = false;
        if (bytes.Length >= 64 && bytes[0] == 'M' && bytes[1] == 'Z')
        {
            var offset = BitConverter.ToInt32(bytes, 60);
            x64 = offset >= 64 && offset <= bytes.Length - 24 && BitConverter.ToUInt32(bytes, offset) == 0x4550 && BitConverter.ToUInt16(bytes, offset + 4) == 0x8664;
        }
        string? signer = null;
        try
        {
#pragma warning disable SYSLIB0057 // Signed-file extraction has no equivalent in X509CertificateLoader.
            using var certificate = new X509Certificate2(X509Certificate.CreateFromSignedFile(path));
#pragma warning restore SYSLIB0057
            signer = certificate.Subject;
        }
        catch (CryptographicException) { }
        var trusted = x64 && VerifySignatureOffline(path);
        return new(x64, trusted, signer, FileVersionInfo.GetVersionInfo(path).FileVersion, Hashing.BytesSha256(bytes));
    }

    private static bool VerifySignatureOffline(string path)
    {
        var file = new TrustFile { Size = (uint)Marshal.SizeOf<TrustFile>(), Path = Path.GetFullPath(path) };
        var pointer = Marshal.AllocHGlobal(Marshal.SizeOf<TrustFile>());
        Marshal.StructureToPtr(file, pointer, false);
        var data = new TrustData { Size = (uint)Marshal.SizeOf<TrustData>(), UiChoice = 2, UnionChoice = 1, File = pointer, StateAction = 1, ProviderFlags = 0x1010 };
        var action = new Guid("00AAC56B-CD44-11d0-8CC2-00C04FC295EE");
        try { return WinVerifyTrust(new IntPtr(-1), ref action, ref data) == 0; }
        finally
        {
            data.StateAction = 2; _ = WinVerifyTrust(new IntPtr(-1), ref action, ref data);
            Marshal.DestroyStructure<TrustFile>(pointer); Marshal.FreeHGlobal(pointer);
        }
    }
    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    private struct TrustFile { public uint Size; [MarshalAs(UnmanagedType.LPWStr)] public string Path; public IntPtr FileHandle; public IntPtr KnownSubject; }
    [StructLayout(LayoutKind.Sequential)]
    private struct TrustData
    {
        public uint Size; public IntPtr PolicyCallback; public IntPtr SipClient;
        public uint UiChoice; public uint RevocationChecks; public uint UnionChoice; public IntPtr File;
        public uint StateAction; public IntPtr StateData; public IntPtr UrlReference;
        public uint ProviderFlags; public uint UiContext; public IntPtr SignatureSettings;
    }
    [DllImport("wintrust.dll", ExactSpelling = true)]
    private static extern int WinVerifyTrust(IntPtr window, ref Guid action, ref TrustData data);
}
