using System.Diagnostics;
using Microsoft.Win32;
using System.Text.Json;
using System.Text.RegularExpressions;

namespace DcsVr.Core;

public sealed class WindowsInventory
{
    /// <summary>"DCS" or "DCS.&lt;variant&gt;" from &lt;install&gt;\dcs_variant.txt, the Saved Games folder that install uses;
    /// null when there is no install or the file is missing (then DCS uses Saved Games\DCS, but older installs may differ).</summary>
    public static string? DcsVariantFolder(string? dcsDirectory)
    {
        if (dcsDirectory is null) return null;
        try
        {
            var file = Path.Combine(dcsDirectory, "dcs_variant.txt");
            if (!File.Exists(file)) return null;
            var variant = File.ReadAllText(file).Trim();
            return variant.Length == 0 ? "DCS" : variant.IndexOfAny(Path.GetInvalidFileNameChars()) >= 0 ? null : "DCS." + variant;
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException) { return null; }
    }

    /// <summary>The Pimax OpenXR runtime manifest: under Program Files, else wherever the registered runtimes say
    /// (Pimax Play installed on another drive).</summary>
    public static string PimaxRuntimeManifest(string? activeRuntime)
    {
        var standard = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ProgramFiles), @"Pimax\Runtime\PiOpenXR_64.json");
        if (File.Exists(standard)) return standard;
        static bool Pimax(string? path) => path is not null && Path.GetFileName(path).StartsWith("PiOpenXR", StringComparison.OrdinalIgnoreCase) && File.Exists(path);
        if (Pimax(activeRuntime)) return Path.GetFullPath(activeRuntime!);
        try
        {
            using var runtimes = Registry.LocalMachine.OpenSubKey(@"SOFTWARE\Khronos\OpenXR\1\AvailableRuntimes");
            if (runtimes?.GetValueNames().FirstOrDefault(Pimax) is { } found) return Path.GetFullPath(found);
        }
        catch (Exception e) when (e is System.Security.SecurityException or UnauthorizedAccessException or IOException) { }
        return standard;
    }

    public InventorySnapshot Capture(string? executableOverride = null, string? optionsOverride = null)
    {
        var observations = new List<string>();
        var installed = InstalledPrograms(observations);
        // Exact product names only; other uninstall entries (mods, tools) can contain "DCS World". Empty locations are ignored.
        var dcsDirectory = installed.Where(p => p.Name.Equals("DCS World", StringComparison.OrdinalIgnoreCase) || p.Name.StartsWith("DCS World ", StringComparison.OrdinalIgnoreCase) || p.Name.StartsWith("DCS World_", StringComparison.OrdinalIgnoreCase))
            .Select(p => p.Directory).FirstOrDefault(d => !string.IsNullOrWhiteSpace(d) && HasDcsExecutable(d));
        // Eagle Dynamics' own key: what most DCS tools read, also present when the install was moved or its uninstall
        // entry has no location.
        dcsDirectory ??= new[] { @"SOFTWARE\Eagle Dynamics\DCS World", @"SOFTWARE\Eagle Dynamics\DCS World OpenBeta" }
            .Select(key => ReadString(RegistryHive.CurrentUser, key, "Path", observations)).FirstOrDefault(d => !string.IsNullOrWhiteSpace(d) && HasDcsExecutable(d!));
        var steamRoot = ReadString(RegistryHive.CurrentUser, @"SOFTWARE\Valve\Steam", "SteamPath", observations)
            ?? Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ProgramFilesX86), "Steam");
        var libraries = SteamLibraries(steamRoot);
        dcsDirectory ??= libraries.Select(p => Path.Combine(p, "steamapps/common/DCSWorld")).FirstOrDefault(HasDcsExecutable);
        string? executable = executableOverride is not null ? Path.GetFullPath(executableOverride) : null;
        if (dcsDirectory is not null) dcsDirectory = Path.GetFullPath(dcsDirectory);
        if (executable is null && dcsDirectory is not null)
            // Current releases keep identical copies in bin and bin-mt; bin is what Steam and the launcher start.
            executable = new[] { @"bin\DCS.exe", @"bin-mt\DCS.exe" }.Select(p => Path.Combine(dcsDirectory, p)).FirstOrDefault(File.Exists);
        if (executable is not null && !File.Exists(executable)) { observations.Add("The selected DCS executable does not exist."); executable = null; }
        // A DCS.exe the pilot chose decides the install (and with it dcs_variant.txt, so the Saved Games folder): with a
        // release and an open beta installed, the one found automatically may be the other.
        if (executable is not null && (executableOverride is not null || dcsDirectory is null)) dcsDirectory = Path.GetDirectoryName(Path.GetDirectoryName(executable));
        var savedGames = new[] { SystemPaths.SavedGames(), Path.Combine(Environment.GetEnvironmentVariable("USERPROFILE") ?? Environment.GetFolderPath(Environment.SpecialFolder.UserProfile), "Saved Games") }
            .Distinct(StringComparer.OrdinalIgnoreCase).ToArray();
        // DCS picks its Saved Games folder from dcs_variant.txt in its install ("openbeta" → Saved Games\DCS.openbeta);
        // without it, Saved Games\DCS. With both folders present, the fixed order alone could pick the one DCS ignores.
        var variant = DcsVariantFolder(dcsDirectory);
        var folders = variant is null ? new[] { "DCS", "DCS.openbeta" } : new[] { variant };
        string? options = optionsOverride is not null ? Path.GetFullPath(optionsOverride) : savedGames.SelectMany(folder => folders
            .Select(p => Path.Combine(folder, p, @"Config\options.lua"))).FirstOrDefault(File.Exists);
        if (options is null)
            observations.Add(savedGames.Any(folder => Directory.Exists(Path.Combine(folder, variant ?? "DCS")))
                ? "DCS has not saved its settings yet (no options.lua in " + Path.Combine(savedGames[0], variant ?? "DCS", "Config") + "). Start DCS once, to the main menu, and close it; then detect again."
                : "Saved Games settings not detected. Searched: " + string.Join("; ", savedGames) + ". If DCS was never started, start it once to the main menu and close it; otherwise select the matching options.lua.");
        else if (optionsOverride is null && variant is null && savedGames.Any(folder => File.Exists(Path.Combine(folder, "DCS", @"Config\options.lua")) && File.Exists(Path.Combine(folder, "DCS.openbeta", @"Config\options.lua"))))
            observations.Add("Both Saved Games\\DCS and Saved Games\\DCS.openbeta exist and the DCS install does not say which it uses; " + options + " was chosen. Select the other options.lua if DCS uses it.");
        if (options is not null && !File.Exists(options)) { observations.Add("The selected options.lua file does not exist."); options = null; }
        var activeRuntime = ReadString(RegistryHive.LocalMachine, @"SOFTWARE\Khronos\OpenXR\1", "ActiveRuntime", observations);
        var pimaxManifest = PimaxRuntimeManifest(activeRuntime);
        var steamDirectory = installed.FirstOrDefault(p => p.Name.Equals("SteamVR", StringComparison.OrdinalIgnoreCase)).Directory;
        steamDirectory ??= libraries.Select(p => Path.Combine(p, "steamapps/common/SteamVR")).FirstOrDefault(p => File.Exists(Path.Combine(p, "steamxr_win64.json")));
        var steamManifest = steamDirectory is not null ? Path.Combine(steamDirectory, "steamxr_win64.json") : null;
        var appData = Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData);
        var sboysConfig = Path.Combine(appData, "CustomHeadset/settings.json");
        var files = new List<FileFact>();
        if (executable is not null)
        {
            files.Add(Fact(executable));
            var bin = Path.GetDirectoryName(executable)!;
            foreach (var file in new[] { "nvngx_dlss.dll", "nvngx_dlssnr.dll", "dxgi.dll", "version.dll", "openxr_loader.dll" }) files.Add(Fact(Path.Combine(bin, file)));
        }
        var settings = new Dictionary<string, string>();
        if (options is not null)
        {
            try
            {
                var lua = new LuaOptions(File.ReadAllText(options));
                foreach (var path in new[] { "VR.openxr_quadView", "VR.openxr_eyeGaze", "VR.pixel_density", "graphics.Upscaling", "graphics.DLSS_PerfQuality", "graphics.maxFPS", "graphics.sync", "VR.custom_IPD_enable", "VR.custom_IPD" })
                    if (lua.Get(path.Split('.')) is { } raw) settings[path] = raw;
            }
            catch (Exception e) when (e is IOException or InvalidDataException or UnauthorizedAccessException) { observations.Add(e.Message); }
        }
        string? autoexecPath = options is null ? null : Path.Combine(Path.GetDirectoryName(options)!, "autoexec.cfg");
        string? autoexecFps = null; var autoexecUnknown = false; var hudAfterDlss = false;
        if (File.Exists(autoexecPath))
        {
            try
            {
                var autoexecText = File.ReadAllText(autoexecPath);
                (autoexecFps, autoexecUnknown) = FramePacing.ReadAutoexec(autoexecText); files.Add(Fact(autoexecPath!));
                hudAfterDlss = FramePacing.HudAfterDlss(autoexecText);
            }
            catch (Exception e) when (e is IOException or UnauthorizedAccessException) { autoexecUnknown = true; observations.Add("Cannot inspect autoexec.cfg: " + e.Message); }
        }
        else autoexecPath = null;
        var limiterProcesses = new List<string>();
        foreach (var name in new[] { "RTSS", "RTSSHooksLoader", "RTSSHooksLoader64" })
        {
            var processes = Process.GetProcessesByName(name);
            if (processes.Length > 0) limiterProcesses.Add(name);
            foreach (var process in processes) process.Dispose();
        }
        var pimax = installed.FirstOrDefault(p => p.Name.Contains("Pimax", StringComparison.OrdinalIgnoreCase));
        var layers = ReadLayers(observations);
        var drivers = ReadDrivers(Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "openvr/openvrpaths.vrpath"), steamDirectory, observations);
        var running = Process.GetProcessesByName("DCS");
        var dcsRunning = running.Length > 0;
        foreach (var process in running) process.Dispose();
        return new()
        {
            DcsDirectory = dcsDirectory, DcsExecutable = executable, OptionsPath = options,
            ActiveRuntime = activeRuntime, PimaxRuntime = File.Exists(pimaxManifest) ? pimaxManifest : null,
            SteamVrRuntime = steamManifest is not null && File.Exists(steamManifest) ? steamManifest : null,
            PimaxVersion = pimax.Version, SboysConfigPath = File.Exists(sboysConfig) ? sboysConfig : null,
            SboysDirectory = drivers.FirstOrDefault(d => d.Name.Contains("customheadset", StringComparison.OrdinalIgnoreCase))?.Directory, DcsRunning = dcsRunning,
            Files = files, Layers = layers, Drivers = drivers, DcsSettings = settings, Observations = observations,
            AutoexecPath = autoexecPath, AutoexecMaxFps = autoexecFps, AutoexecPacingUnknown = autoexecUnknown, AutoexecHudAfterDlss = autoexecPath is null ? null : hudAfterDlss,LimiterProcesses = limiterProcesses
        };
    }

    private static FileFact Fact(string path)
    {
        if (!File.Exists(path)) return new(path, false, null, null);
        try { return new(path, true, FileVersionInfo.GetVersionInfo(path).FileVersion, Hashing.FileSha256(path)); }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException) { return new(path, true, null, null); }
    }

    private static bool HasDcsExecutable(string directory) =>
        File.Exists(Path.Combine(directory, "bin/DCS.exe")) || File.Exists(Path.Combine(directory, "bin-mt/DCS.exe"));

    private static List<(string Name, string? Directory, string? Version)> InstalledPrograms(List<string> observations)
    {
        var result = new List<(string, string?, string?)>();
        foreach (var hive in new[] { RegistryHive.LocalMachine, RegistryHive.CurrentUser })
        foreach (var view in new[] { RegistryView.Registry64, RegistryView.Registry32 })
        {
            try
            {
                using var root = RegistryKey.OpenBaseKey(hive, view);
                using var uninstall = root.OpenSubKey(@"SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall");
                if (uninstall is null) continue;
                foreach (var key in uninstall.GetSubKeyNames())
                {
                    using var app = uninstall.OpenSubKey(key);
                    if (app?.GetValue("DisplayName") is string name)
                        result.Add((name, app.GetValue("InstallLocation") as string, app.GetValue("DisplayVersion") as string));
                }
            }
            catch (Exception e) when (e is UnauthorizedAccessException or System.Security.SecurityException or IOException) { observations.Add($"Registry inventory: {e.Message}"); }
        }
        return result;
    }

    private static string? ReadString(RegistryHive hive, string key, string name, List<string> observations)
    {
        try
        {
            using var root = RegistryKey.OpenBaseKey(hive, RegistryView.Registry64);
            using var value = root.OpenSubKey(key);
            return value?.GetValue(name) as string;
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException or System.Security.SecurityException) { observations.Add(e.Message); return null; }
    }

    private static List<LayerFact> ReadLayers(List<string> observations)
    {
        var result = new List<LayerFact>();
        foreach (var hive in new[] { RegistryHive.LocalMachine, RegistryHive.CurrentUser })
        {
            try
            {
                using var root = RegistryKey.OpenBaseKey(hive, RegistryView.Registry64);
                using var layers = root.OpenSubKey(@"SOFTWARE\Khronos\OpenXR\1\ApiLayers\Implicit");
                if (layers is null) continue;
                foreach (var path in layers.GetValueNames())
                {
                    string? name = null, dll = null;
                    try
                    {
                        using var manifest = System.Text.Json.JsonDocument.Parse(File.ReadAllText(path));
                        var api = manifest.RootElement.GetProperty("api_layer");
                        name = api.GetProperty("name").GetString();
                        if (api.TryGetProperty("library_path", out var library) && library.GetString() is { } relative)
                            dll = Path.GetFullPath(relative, Path.GetDirectoryName(path)!);
                    }
                    catch (Exception e) when (e is IOException or UnauthorizedAccessException or System.Text.Json.JsonException or KeyNotFoundException or ArgumentException or InvalidOperationException)
                    { observations.Add($"Cannot read layer manifest: {Path.GetFileName(path)}."); }
                    result.Add(new(hive.ToString(), path, layers.GetValue(path) is int v && v == 0, name, dll, dll is not null && File.Exists(dll)));
                }
            }
            catch (Exception e) when (e is IOException or UnauthorizedAccessException or System.Security.SecurityException) { observations.Add(e.Message); }
        }
        return result;
    }

    public static IReadOnlyList<string> SteamLibraries(string root)
    {
        var paths = new List<string> { Path.GetFullPath(root) };
        try
        {
            var vdf = File.ReadAllText(Path.Combine(root, "steamapps/libraryfolders.vdf"));
            foreach (Match match in Regex.Matches(vdf, "\"(?:path|[0-9]+)\"\\s+\"([^\"]+)\""))
            {
                var value = match.Groups[1].Value.Replace("\\\\", "\\");
                if (Path.IsPathFullyQualified(value)) paths.Add(Path.GetFullPath(value));
            }
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException or ArgumentException) { }
        return paths.Distinct(StringComparer.OrdinalIgnoreCase).ToArray();
    }

    public static IReadOnlyList<DriverFact> ReadDrivers(string registration, string? steamDirectory, List<string> observations)
    {
        var paths = new List<string>(); var configPaths = new List<string>();
        try
        {
            using var json = JsonDocument.Parse(File.ReadAllText(registration));
            if (json.RootElement.TryGetProperty("external_drivers", out var drivers))
                paths.AddRange(drivers.EnumerateArray().Select(d => d.GetString()).OfType<string>());
            if (json.RootElement.TryGetProperty("config", out var configs))
                configPaths.AddRange(configs.EnumerateArray().Select(d => d.GetString()).OfType<string>());
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException or JsonException or InvalidOperationException) { observations.Add("SteamVR driver registration could not be read: " + e.Message); }
        if (steamDirectory is not null && Directory.Exists(Path.Combine(steamDirectory, "drivers")))
            paths.AddRange(Directory.EnumerateDirectories(Path.Combine(steamDirectory, "drivers")));
        var blocked = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        foreach (var config in configPaths)
        {
            try
            {
                using var settings = JsonDocument.Parse(File.ReadAllText(Path.Combine(config, "steamvr.vrsettings")));
                foreach (var item in settings.RootElement.EnumerateObject())
                    if (item.Name.StartsWith("driver_", StringComparison.Ordinal) &&
                        ((item.Value.TryGetProperty("blocked_by_safe_mode", out var block) && block.ValueKind == JsonValueKind.True) ||
                         (item.Value.TryGetProperty("enable", out var enabled) && enabled.ValueKind == JsonValueKind.False))) blocked.Add(item.Name[7..]);
            }
            catch (Exception e) when (e is IOException or UnauthorizedAccessException or JsonException or InvalidOperationException) { }
        }
        var result = new List<DriverFact>();
        foreach (var path in paths.Distinct(StringComparer.OrdinalIgnoreCase))
        {
            try
            {
                using var manifest = JsonDocument.Parse(File.ReadAllText(Path.Combine(path, "driver.vrdrivermanifest")));
                var name = manifest.RootElement.GetProperty("name").GetString()!;
                result.Add(new(name, Path.GetFullPath(path), NativeBinary.IsX64(Path.Combine(path, "bin/win64/driver_" + name + ".dll")), blocked.Contains(name),
                    manifest.RootElement.TryGetProperty("version", out var version) ? version.GetString() : null));
            }
            catch (Exception e) when (e is IOException or UnauthorizedAccessException or JsonException or ArgumentException or KeyNotFoundException or InvalidOperationException) { observations.Add("SteamVR driver manifest could not be read: " + path); }
        }
        return result;
    }
}
