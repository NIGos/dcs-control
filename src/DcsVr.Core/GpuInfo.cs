using System.Text.RegularExpressions;
using Microsoft.Win32;

namespace DcsVr.Core;

/// <summary>A display adapter Windows has a driver for.</summary>
/// <param name="Vendor">NVIDIA, AMD, Intel or Other (from the PCI vendor id).</param>
public sealed record GpuAdapter(string Name, string Vendor, string? Driver);

/// <summary>
/// The PC's graphics adapters, read once from the display class in the registry (no GPU API is loaded). DLSS 5,
/// Foveated Super Resolution and NVIDIA optical-flow frame generation need an NVIDIA RTX card; FidelityFX frame
/// generation runs on any of them. Virtual adapters (remote desktop, TeamViewer, Parsec) are left out.
/// </summary>
public static partial class GpuInfo
{
    private static readonly Lazy<IReadOnlyList<GpuAdapter>> Detected = new(ReadRegistry);
    /// <summary>Replaceable so tests and the offline verification never depend on the build machine's GPU.</summary>
    public static Func<IReadOnlyList<GpuAdapter>> Adapters { get; set; } = () => Detected.Value;

    /// <summary>An NVIDIA GeForce/Quadro/data-center RTX (or TITAN RTX) adapter is present.</summary>
    public static bool HasNvidiaRtx => Adapters().Any(IsNvidiaRtx);
    /// <summary>The adapters could not be read: nothing is blocked on the GPU's account.</summary>
    public static bool Unknown => Adapters().Count == 0;
    public static bool IsNvidiaRtx(GpuAdapter a) => a.Vendor == "NVIDIA" && RtxRegex().IsMatch(a.Name);
    /// <summary>Every NVIDIA RTX adapter is an RTX 50 card (NVIDIA's DLSS 5 runtime runs as released, no patch needed).</summary>
    public static bool OnlyRtx50 => Adapters().Where(IsNvidiaRtx).ToList() is { Count: > 0 } rtx && rtx.All(a => Rtx50Regex().IsMatch(a.Name));
    /// <summary>The DLSS 5 runtime's info line for a compatibility-patched (or unsigned) copy on a PC whose RTX cards are
    /// all RTX 50 (GitHub #33): the patch is only for RTX 20-40. Null otherwise.</summary>
    public static string? PatchedRuntimeNote(bool modified) =>
        modified && OnlyRtx50 ? "Patched copy for RTX 20-40 cards. Your RTX 50 runs NVIDIA's original file." : null;
    /// <summary>Every NVIDIA RTX adapter is a GeForce RTX 20, 30 or 40 card (by name; workstation cards and unknown names
    /// never count), where NVIDIA's original DLSS 5 runtime does not run.</summary>
    public static bool OnlyRtx20To40 => Adapters().Where(IsNvidiaRtx).ToList() is { Count: > 0 } rtx && rtx.All(a => Rtx20To40Regex().IsMatch(a.Name));
    /// <summary>The saved DLSS 5 runtime's info line: the patched-copy note above, or for NVIDIA's original file on a PC
    /// whose RTX cards are all RTX 20-40, that DLSS 5 will not apply there (in flight: "ON but not applied").</summary>
    public static string? RuntimeNote(bool modified) => PatchedRuntimeNote(modified)
        ?? (!modified && OnlyRtx20To40 ? "NVIDIA's original file runs only on RTX 50 cards, so DLSS 5 will not apply on this PC's " + string.Join(", ", Adapters().Where(IsNvidiaRtx).Select(a => a.Name)) + ". See \"RTX 20, 30 and 40 cards\" in the user guide." : null);
    /// <summary>"AMD Radeon RX 7900 XTX" (the names of the real adapters, for messages).</summary>
    public static string Names => string.Join(", ", Adapters().Select(a => a.Name));

    [GeneratedRegex(@"\bRTX\b|\bTITAN RTX\b|\b(?:A|L)\d{2,4}\b|\bH100\b", RegexOptions.IgnoreCase)] private static partial Regex RtxRegex();
    [GeneratedRegex(@"VEN_([0-9A-F]{4})", RegexOptions.IgnoreCase)] private static partial Regex VendorRegex();
    [GeneratedRegex(@"\bRTX\s*50\d0\b", RegexOptions.IgnoreCase)] private static partial Regex Rtx50Regex();
    [GeneratedRegex(@"\bRTX\s*[234]0\d0\b", RegexOptions.IgnoreCase)] private static partial Regex Rtx20To40Regex();

    private static IReadOnlyList<GpuAdapter> ReadRegistry()
    {
        var adapters = new List<GpuAdapter>();
        try
        {
            using var display = Registry.LocalMachine.OpenSubKey(@"SYSTEM\CurrentControlSet\Control\Class\{4d36e968-e325-11ce-bfc1-08002be10318}");
            foreach (var name in display?.GetSubKeyNames().Where(k => k.Length == 4 && k.All(char.IsDigit)) ?? [])
            {
                using var key = display!.OpenSubKey(name);
                if (key?.GetValue("DriverDesc") is not string description) continue;
                var device = key.GetValue("MatchingDeviceId") as string ?? "";
                var vendor = VendorRegex().Match(device) is { Success: true } m ? m.Groups[1].Value.ToUpperInvariant() switch
                {
                    "10DE" => "NVIDIA", "1002" or "1022" => "AMD", "8086" => "Intel", _ => "Other"
                } : "Other";
                // Only real PCI adapters: virtual displays (remote desktop, TeamViewer, Parsec, IddCx) have no PCI vendor.
                if (vendor == "Other" && !device.StartsWith("pci", StringComparison.OrdinalIgnoreCase)) continue;
                adapters.Add(new(description.Trim(), vendor, key.GetValue("DriverVersion") as string));
            }
        }
        catch (Exception e) when (e is System.Security.SecurityException or UnauthorizedAccessException or IOException) { }
        return adapters;
    }
}
