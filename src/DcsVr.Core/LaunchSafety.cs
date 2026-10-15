using Microsoft.Win32;

namespace DcsVr.Core;

/// <summary>
/// Conditions under which the private launch environment cannot work. The OpenXR loader ignores
/// XR_RUNTIME_JSON and XR_API_LAYER_PATH in elevated processes but still honours XR_ENABLE_API_LAYERS,
/// so an elevated DCS asks for layers it cannot find and xrCreateInstance fails.
/// </summary>
public static class LaunchSafety
{
    public static bool CurrentProcessElevated => Environment.IsPrivilegedProcess;

    /// <summary>Returns the compatibility flag that forces elevation for this executable, or null.</summary>
    public static string? ForcedElevation(string executable)
    {
        var full = Path.GetFullPath(executable);
        foreach (var hive in new[] { RegistryHive.CurrentUser, RegistryHive.LocalMachine })
        foreach (var view in new[] { RegistryView.Registry64, RegistryView.Registry32 })
        {
            try
            {
                using var root = RegistryKey.OpenBaseKey(hive, view);
                using var layers = root.OpenSubKey(@"SOFTWARE\Microsoft\Windows NT\CurrentVersion\AppCompatFlags\Layers");
                if (layers is null || layers.GetValueNames().FirstOrDefault(n => string.Equals(n, full, StringComparison.OrdinalIgnoreCase)) is not { } name) continue;
                var flags = (layers.GetValue(name) as string ?? "").Split(' ', StringSplitOptions.RemoveEmptyEntries);
                if (flags.FirstOrDefault(f => f is "RUNASADMIN" or "RUNASHIGHEST") is { } flag) return $"{hive}: {flag}";
            }
            catch (Exception e) when (e is IOException or UnauthorizedAccessException or System.Security.SecurityException or ArgumentException or NotSupportedException) { }
        }
        return null;
    }

    /// <summary>Throws when DCS would start elevated and silently lose the profile environment.</summary>
    public static void EnsureNotElevated(string executable)
    {
        if (CurrentProcessElevated)
            throw new InvalidOperationException(UacDisabled()
                ? "User Account Control is off on this PC, so every program, DCS too, runs as administrator, and the OpenXR loader then ignores the profile runtime and layers. Turn User Account Control on (Control Panel → User Accounts → Change User Account Control settings, any level above Never notify) and restart Windows."
                : "DCS Control is running as administrator. The OpenXR loader ignores the profile runtime and layers in elevated processes. Reopen the app without administrator rights.");
        if (ForcedElevation(executable) is { } flag)
            throw new InvalidOperationException($"DCS.exe is set to run as administrator ({flag}). Elevated DCS ignores the profile runtime and layers. Clear 'Run this program as an administrator' in DCS.exe Properties → Compatibility.");
    }

    /// <summary>
    /// The DCS launcher starts the game by running <c>DCS.exe --restarted</c> with CREATE_BREAKAWAY_FROM_JOB. When this
    /// app (and so DCS, which inherits it) runs inside a Windows job object that allows no breakaway, that call fails
    /// with access denied: dcs.log reads "Launching process … failed with error code 5" and the game never opens. This
    /// happens when the app is started by a tool that runs its children in a job (a sandbox, a script runner, some
    /// launchers), never when it is started from the Start menu or Explorer. Replaceable for tests.
    /// </summary>
    public static Func<JobState> CurrentJob { get; set; } = ReadCurrentJob;
    /// <summary>Steam runs: the Steam edition of DCS needs it before Launch DCS. Replaceable for tests (the offline
    /// verification host must not depend on whether Steam happens to run on the build machine).</summary>
    public static Func<bool> SteamRunning { get; set; } = () =>
    {
        var steam = System.Diagnostics.Process.GetProcessesByName("steam");
        try { return steam.Length > 0; } finally { foreach (var process in steam) process.Dispose(); }
    };

    /// <param name="InJob">The process belongs to a job object.</param>
    /// <param name="LimitFlags">JOBOBJECT_BASIC_LIMIT_INFORMATION.LimitFlags of the innermost job.</param>
    /// <param name="Unknown">The process is in a job whose limits could not be read: nothing is blocked, the user is warned.</param>
    public readonly record struct JobState(bool InJob, uint LimitFlags, bool Unknown = false)
    {
        public const uint BreakawayOk = 0x800, SilentBreakawayOk = 0x1000;
        /// <summary>A child that asks to break away from the job (the DCS launcher's restart) is refused.</summary>
        public bool BlocksBreakaway => InJob && !Unknown && (LimitFlags & (BreakawayOk | SilentBreakawayOk)) == 0;
    }

    public const string LauncherUnknownMessage = "DCS Control runs inside a Windows job whose limits could not be read, so it is unknown whether the DCS launcher can start the game here. If DCS does not open after the launcher, close DCS Control and open it from the Start menu or Explorer, or turn off Show the DCS launcher.";

    /// <summary>True when the app runs in a job whose breakaway limit could not be read (a warning, never a block).</summary>
    public static bool LauncherRestartUnknown
    {
        get { try { return CurrentJob() is { InJob: true, Unknown: true }; } catch (Exception e) when (e is System.ComponentModel.Win32Exception or EntryPointNotFoundException or DllNotFoundException) { return true; } }
    }

    public const string LauncherBlockedMessage = "DCS Control was started inside a Windows job (by another program, such as a script runner or sandbox), so the DCS launcher cannot start the game: Windows refuses its restart with access denied (error 5). Close DCS Control and open it from the Start menu or Explorer, or turn off Show the DCS launcher.";

    /// <summary>True when the DCS launcher's restart into the game would be refused (see <see cref="CurrentJob"/>).</summary>
    public static bool LauncherRestartBlocked
    {
        get { try { return CurrentJob().BlocksBreakaway; } catch (Exception e) when (e is System.ComponentModel.Win32Exception or EntryPointNotFoundException or DllNotFoundException) { return false; } }
    }

    private static JobState ReadCurrentJob()
    {
        if (!OperatingSystem.IsWindows()) return default;
        if (!IsProcessInJob(GetCurrentProcess(), IntPtr.Zero, out var inJob) || !inJob) return new(false, 0);
        // A null job handle queries the job of the calling process; 9 = JobObjectExtendedLimitInformation.
        var info = new byte[144];
        return QueryInformationJobObject(IntPtr.Zero, 9, info, info.Length, IntPtr.Zero)
            ? new(true, BitConverter.ToUInt32(info, 16)) : new(true, 0, Unknown: true);
    }
    [System.Runtime.InteropServices.DllImport("kernel32.dll")] private static extern IntPtr GetCurrentProcess();
    [System.Runtime.InteropServices.DllImport("kernel32.dll", SetLastError = true)] private static extern bool IsProcessInJob(IntPtr process, IntPtr job, out bool result);
    [System.Runtime.InteropServices.DllImport("kernel32.dll", SetLastError = true)] private static extern bool QueryInformationJobObject(IntPtr job, int infoClass, byte[] info, int length, IntPtr returnLength);

    /// <summary>What the pilot reads when a game folder's permissions keep this account out (DCS under Program Files, typically).</summary>
    public const string WriteAccessAdvice = "Windows does not let your account write there, which is usual when DCS is installed under Program Files (antivirus exclusions do not change it). Press Grant access to give your account Modify permission on it with one Windows prompt, or set it yourself: folder Properties → Security → Edit → your account → Modify. Do not run DCS Control as administrator instead: DCS would start elevated and ignore the profile's layers.";

    /// <summary>What the pilot reads when the permissions allow writing but something else refuses it.</summary>
    public const string BlockedAdvice = "The folder's permissions allow it, so something else refuses the write: usually an antivirus or Windows Security's Controlled folder access (allow DCS Control there: Windows Security → Virus & threat protection → Ransomware protection → Allow an app through Controlled folder access), or a read-only or network drive.";

    /// <summary>Windows Security's "Controlled folder access" is on: it blocks programs it does not know from writing to
    /// Documents and Saved Games even when the folder permissions allow it.</summary>
    public static bool ControlledFolderAccessOn()
    {
        try
        {
            using var key = Registry.LocalMachine.OpenSubKey(@"SOFTWARE\Microsoft\Windows Defender\Windows Defender Exploit Guard\Controlled Folder Access");
            return key?.GetValue("EnableControlledFolderAccess") is int on && on == 1;
        }
        catch (Exception e) when (e is System.Security.SecurityException or UnauthorizedAccessException or IOException) { return false; }
    }

    /// <summary>User Account Control is off (EnableLUA=0): every program of an administrator runs elevated, so DCS would
    /// too, and the OpenXR loader would ignore the profile's layers.</summary>
    public static bool UacDisabled()
    {
        try
        {
            using var key = Registry.LocalMachine.OpenSubKey(@"SOFTWARE\Microsoft\Windows\CurrentVersion\Policies\System");
            return key?.GetValue("EnableLUA") is int on && on == 0;
        }
        catch (Exception e) when (e is System.Security.SecurityException or UnauthorizedAccessException or IOException) { return false; }
    }

    /// <summary>The advice for a folder that cannot be written: the permission advice when its permissions keep this
    /// account out, otherwise what else blocks writes (antivirus, Controlled folder access, the drive).</summary>
    public static string WriteAdvice(string? folder = null) =>
        folder is not null && !PermissionsDenyWrite(folder) ? BlockedAdvice
        : WriteAccessAdvice + (ControlledFolderAccessOn() ? " Windows Security's Controlled folder access is on: if the folder is already writable, allow DCS Control there (Ransomware protection → Allow an app through Controlled folder access)." : "");

    /// <summary>Every game folder a profile writes to: Saved Games\DCS\Config (options.lua), Saved Games\DCS\Scripts
    /// (the engine optimizations' hook and module, prefetch fix included) and, only for the Cheeky loader (DLSS 5,
    /// Foveated SR), DCS's bin folder. Optimizations only never needs the DCS install folder.</summary>
    public static IReadOnlyList<string> ProfileFolders(VrProfile profile, InventorySnapshot inventory)
    {
        var folders = new List<string>();
        if (inventory.OptionsPath is { } options && Path.GetDirectoryName(options) is { } config)
        {
            folders.Add(config);
            if (profile.UsesEngineModule && Path.GetDirectoryName(config) is { } savedGames) folders.Add(Path.Combine(savedGames, "Scripts"));
        }
        if (profile.ForLaunch().UsesCheeky && inventory.DcsExecutable is { } exe && Path.GetDirectoryName(exe) is { } bin) folders.Add(bin);
        return folders;
    }

    /// <summary>The game folders a profile writes to that the current account cannot write (cached 15 s: the profile
    /// checks ask on every edit).</summary>
    public static IReadOnlyList<string> UnwritableFolders(VrProfile profile, InventorySnapshot inventory) => UnwritableFolders(ProfileFolders(profile, inventory));

    /// <summary>The existing folders of <paramref name="folders"/> the current account cannot write (cached 15 s); a
    /// missing folder is reported by its own check.</summary>
    public static IReadOnlyList<string> UnwritableFolders(IEnumerable<string> folders) =>
        folders.Distinct(StringComparer.OrdinalIgnoreCase).Where(f => Directory.Exists(f) && !CanWriteCached(f)).ToList();

    private static readonly System.Collections.Concurrent.ConcurrentDictionary<string, (bool Writable, long At)> WriteProbes = new(StringComparer.OrdinalIgnoreCase);
    private static bool CanWriteCached(string directory)
    {
        var now = Environment.TickCount64;
        if (WriteProbes.TryGetValue(directory, out var probe) && now - probe.At < 15000) return probe.Writable;
        var writable = CanWrite(directory);
        WriteProbes[directory] = (writable, now);
        return writable;
    }
    /// <summary>Forgets the cached answers (after Grant access).</summary>
    public static void ForgetWriteProbes() => WriteProbes.Clear();

    /// <summary>The folder's permissions give this account (or a group it belongs to) no right to create files, or deny
    /// it. False when the permissions cannot be read: then they are not what blocks the write.</summary>
    public static bool PermissionsDenyWrite(string directory)
    {
        try
        {
            using var identity = System.Security.Principal.WindowsIdentity.GetCurrent();
            var sids = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
            if (identity.User is { } user) sids.Add(user.Value);
            foreach (var group in identity.Groups ?? []) sids.Add(group.Value);
            const System.Security.AccessControl.FileSystemRights create = System.Security.AccessControl.FileSystemRights.CreateFiles | System.Security.AccessControl.FileSystemRights.WriteData;
            bool allowed = false, denied = false;
            var rules = new DirectoryInfo(directory).GetAccessControl().GetAccessRules(true, true, typeof(System.Security.Principal.SecurityIdentifier));
            foreach (System.Security.AccessControl.FileSystemAccessRule rule in rules)
            {
                if (!sids.Contains(rule.IdentityReference.Value) || (rule.FileSystemRights & create) == 0) continue;
                if (rule.AccessControlType == System.Security.AccessControl.AccessControlType.Deny) denied = true; else allowed = true;
            }
            return denied || !allowed;
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException or InvalidOperationException or System.Security.SecurityException or PlatformNotSupportedException) { return false; }
    }

    /// <summary>A local NTFS folder: the only kind whose permissions Grant access can change (a network share, FAT/exFAT
    /// or read-only media cannot be fixed that way).</summary>
    public static bool CanGrant(string directory)
    {
        try
        {
            var drive = new DriveInfo(Path.GetPathRoot(Path.GetFullPath(directory))!);
            return drive.DriveType == DriveType.Fixed && string.Equals(drive.DriveFormat, "NTFS", StringComparison.OrdinalIgnoreCase);
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException or ArgumentException) { return false; }
    }

    /// <summary>
    /// Gives the current account Modify permission on <paramref name="folders"/> (inherited by their files and
    /// subfolders) with icacls, all in one elevated PowerShell: a single Windows prompt. Returns false when the prompt
    /// was declined. The account is named by its SID, taken here (not in the elevated process, which may run as another
    /// administrator) and independent of the Windows language. Only the folders passed change; the caller checks the
    /// result again, since icacls may report files it could not change while the folder itself was granted.
    /// </summary>
    public static bool GrantWriteAccess(IReadOnlyList<string> folders)
    {
        if (folders.Count == 0) return true;
        using var identity = System.Security.Principal.WindowsIdentity.GetCurrent();
        var sid = identity.User?.Value ?? throw new InvalidOperationException("The current Windows account could not be identified.");
        static string Quoted(string s) => "'" + s.Replace("'", "''") + "'";
        var script = "$code = 0\n" + string.Concat(folders.Select(f =>
            $"& icacls.exe {Quoted(f.TrimEnd('\\', '/'))} /grant {Quoted("*" + sid + ":(OI)(CI)M")} /C /Q; if ($LASTEXITCODE -ne 0) {{ $code = $LASTEXITCODE }}\n")) + "exit $code\n";
        var start = new System.Diagnostics.ProcessStartInfo("powershell.exe", "-NoProfile -NonInteractive -ExecutionPolicy Bypass -EncodedCommand " + Convert.ToBase64String(System.Text.Encoding.Unicode.GetBytes(script)))
        { UseShellExecute = true, Verb = "runas", WindowStyle = System.Diagnostics.ProcessWindowStyle.Hidden };
        try
        {
            using var process = System.Diagnostics.Process.Start(start) ?? throw new InvalidOperationException("Windows did not start the permission change.");
            process.WaitForExit();
        }
        catch (System.ComponentModel.Win32Exception e) when (e.NativeErrorCode == 1223) { return false; } // the prompt was declined
        catch (System.ComponentModel.Win32Exception e) { throw new InvalidOperationException("Windows refused to run the permission change (" + e.Message + "). " + WriteAccessAdvice, e); }
        ForgetWriteProbes();
        return true;
    }

    /// <summary>Creates and immediately deletes a probe file, which is the only reliable answer under UAC virtualization and ACLs.</summary>
    public static bool CanWrite(string directory)
    {
        try
        {
            if (!Directory.Exists(directory)) return false;
            using var probe = new FileStream(Path.Combine(directory, ".dcs-vr-probe-" + Guid.NewGuid().ToString("N")), FileMode.CreateNew, FileAccess.Write, FileShare.None, 1, FileOptions.DeleteOnClose);
            return true;
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException) { return false; }
    }
}
