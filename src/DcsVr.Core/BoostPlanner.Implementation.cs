using System.Runtime.InteropServices;

namespace DcsVr.Core;

/// <summary>One running process as CPU Boost sees it. <paramref name="AccessibleForSetInfo"/> is false when the
/// process is owned by another account or session and cannot be opened for PROCESS_SET_INFORMATION without
/// administrator rights.</summary>
public sealed record BoostProcessFact(int Pid, string Name, int ParentPid, bool AccessibleForSetInfo);

/// <summary>A snapshot of running processes plus the real DCS PID, if any. Pure input to plan building, so plans can be
/// tested without touching real processes.</summary>
public sealed record BoostSnapshot(IReadOnlyList<BoostProcessFact> Processes, int? DcsPid);

public static partial class BoostPlanner
{
    public const string DcsProcessName = "DCS";

    /// <summary>True for a process name CPU Boost never moves or closes, whatever pattern matched it.</summary>
    internal static bool IsProtectedName(string name)
    {
        var trimmed = TrimExe(name.Trim());
        return ProfileValidation.BoostProtectedProcesses.Contains(trimmed);
    }

    /// <summary>The one process-name matcher CPU Boost uses (plan, runtime and validation): <c>*</c> matches any run
    /// of characters and <c>?</c> one character, anywhere in the pattern; case-insensitive; a trailing ".exe" on
    /// either side is ignored.</summary>
    public static bool MatchesProcessName(string pattern, string name)
    {
        var p = TrimExe(pattern.Trim());
        var n = TrimExe(name.Trim());
        int pi = 0, ni = 0, star = -1, mark = 0;
        while (ni < n.Length)
        {
            if (pi < p.Length && (p[pi] == '?' || char.ToUpperInvariant(p[pi]) == char.ToUpperInvariant(n[ni]))) { pi++; ni++; }
            else if (pi < p.Length && p[pi] == '*') { star = pi++; mark = ni; }
            else if (star >= 0) { pi = star + 1; ni = ++mark; }
            else return false;
        }
        while (pi < p.Length && p[pi] == '*') pi++;
        return pi == p.Length;
    }

    /// <summary>VR runtime and headset services confined to the common cores, by headset route. Affinity only; their
    /// priority is left alone because the compositor must keep up. SteamVR's own compositor/vrserver are deliberately
    /// absent (see the plan notes).</summary>
    internal static IReadOnlyList<string> VrRuntimeServices(VrProfile profile) => profile.Runtime switch
    {
        RuntimeKind.Pimax =>
        [
            "pi_server", "pi_overlay", "PiPlayService", "pi_vst", "PimaxClient",
            "platform_runtime_*_service", "Tobii.Service"
        ],
        _ => []
    };

    /// <summary>Builds the plan from a profile, a CPU ranking and a process snapshot, plus the video memory per process
    /// and the displays for the flight helpers. Pure: changes nothing.</summary>
    internal static BoostPlan Compose(VrProfile profile, CpuTopology topology, BoostSnapshot snapshot, IReadOnlyDictionary<int, long>? videoMemory = null, IDisplayModes? displays = null)
    {
        var cpu = ComposeCpu(profile, topology, snapshot);
        var excluded = new HashSet<int>(AncestorPids(snapshot));
        if (snapshot.DcsPid is { } dcsPid) excluded.Add(dcsPid);
        var vram = FreeVram.Describe(snapshot, profile.FreeVramApps, excluded, videoMemory);
        var monitor = displays is null ? null : FlightDisplay.Describe(displays, FlightDisplay.Target(profile));
        var small = $"graphics.width = {VrProfile.SmallWindowWidth}, graphics.height = {VrProfile.SmallWindowHeight}, graphics.fullScreen = false";
        return cpu with { FreeVram = vram, FreeVramBytes = videoMemory is null ? null : vram.Sum(v => v.DedicatedBytes ?? 0), Monitor = monitor, SmallWindow = small, TobiiServices = TobiiDesktop.InstalledServices() };
    }

    private static BoostPlan ComposeCpu(VrProfile profile, CpuTopology topology, BoostSnapshot snapshot)
    {
        var notes = new List<string>();
        var processes = new List<BoostProcess>();
        var ancestors = AncestorPids(snapshot);

        static bool Matches(string pattern, string name) => MatchesProcessName(pattern, name);

        // Returns the first running, confinable instance of a configured name, or null, and records why it was skipped.
        BoostProcessFact? Resolve(string pattern, out bool skippedAsLauncher)
        {
            skippedAsLauncher = false;
            BoostProcessFact? first = null;
            foreach (var p in snapshot.Processes.Where(p => Matches(pattern, p.Name)))
            {
                if (p.Pid == snapshot.DcsPid || ancestors.Contains(p.Pid) || IsProtectedName(p.Name)) { skippedAsLauncher = true; continue; }
                first ??= p;
            }
            return first;
        }

        // DCS always keeps every CPU; only its priority changes.
        var dcs = snapshot.Processes.FirstOrDefault(p => string.Equals(TrimExe(p.Name), DcsProcessName, StringComparison.OrdinalIgnoreCase));
        processes.Add(new BoostProcess(DcsProcessName, "DCS",
            $"Priority {Describe(profile.BoostDcsPriority)}" + (topology.HasRanking ? $", all {topology.LogicalCount} CPUs" : ", CPU affinity unchanged"),
            Running: dcs is not null, NeedsAdministrator: dcs is { AccessibleForSetInfo: false }));

        var canMove = topology.HasRanking;
        if (!canMove)
            notes.Add($"No CPU core ranking available ({topology.Description}). Only DCS priority is applied; no process is moved between cores.");

        // VR runtime services.
        if (profile.Runtime == RuntimeKind.SboysSteamVr && !profile.Desktop)
            notes.Add("SteamVR route: the OpenXR compositor (vrcompositor) and vrserver are left untouched. Confining them has not been shown to be safe and can stall presentation.");
        if (profile.Desktop)
            notes.Add("Optimizations only: no VR runtime runs, so none is moved.");
        else if (!profile.BoostMoveVrRuntime)
            notes.Add("VR runtime confinement is turned off in this profile.");
        else if (canMove)
            foreach (var pattern in VrRuntimeServices(profile))
            {
                var match = Resolve(pattern, out var skipped);
                if (skipped) notes.Add($"Skipped {pattern} where it is DCS, an ancestor of DCS or a protected process; confining it could confine DCS.");
                processes.Add(new BoostProcess(pattern, "VR runtime",
                    $"Common cores ({Format(topology.VrRuntimeCores)}), priority unchanged",
                    Running: match is not null, NeedsAdministrator: match is { AccessibleForSetInfo: false }));
            }

        // Background apps.
        if (!profile.BoostMoveBackgroundApps)
            notes.Add("Background apps are not moved in this profile.");
        else if (canMove)
            foreach (var name in profile.BoostBackgroundApps.Where(n => !string.IsNullOrWhiteSpace(n)))
            {
                var match = Resolve(name, out var skipped);
                if (skipped) notes.Add($"Skipped background app {name} where it is DCS, an ancestor of DCS or a protected process.");
                processes.Add(new BoostProcess(name, "Background",
                    $"Slowest cores ({Format(topology.BackgroundCores)}), below normal priority",
                    Running: match is not null, NeedsAdministrator: match is { AccessibleForSetInfo: false }));
            }

        notes.Add(profile.BoostPrefetch switch
        {
            PrefetchFix.Skip => @"Prefetch fix: DCS loads it at start (bin\dxgi2.dll); repeated terrain prefetches within 5 s are skipped. Statistics in bin\DcsVrPrefetchFix.log.",
            PrefetchFix.Observe => @"Prefetch fix: DCS loads it at start (bin\dxgi2.dll) and only counts prefetch calls in bin\DcsVrPrefetchFix.log.",
            _ => "Prefetch fix: off."
        });
        if (profile.BoostElevated && processes.Any(p => p.NeedsAdministrator))
            notes.Add("Some targets run under another account; they can be changed only with the administrator rights this profile requests.");

        return new BoostPlan(profile.CpuBoost, topology.Source, topology.Description,
            topology.RenderCores, topology.VrRuntimeCores, topology.BackgroundCores, processes, notes);
    }

    static string TrimExe(string name) =>
        name.EndsWith(".exe", StringComparison.OrdinalIgnoreCase) ? name[..^4] : name;
    static string Describe(BoostPriority priority) => priority switch
    {
        BoostPriority.AboveNormal => "Above normal",
        BoostPriority.High => "High",
        _ => "Normal"
    };
    static string Format(IReadOnlyList<int> cores) => cores.Count == 0 ? "none" : string.Join(", ", cores);

    /// <summary>PIDs on the parent chain from DCS up to the root. DCS itself is not included.</summary>
    internal static IReadOnlySet<int> AncestorPids(BoostSnapshot snapshot)
    {
        var result = new HashSet<int>();
        if (snapshot.DcsPid is not { } pid) return result;
        var byPid = new Dictionary<int, BoostProcessFact>();
        foreach (var p in snapshot.Processes) byPid.TryAdd(p.Pid, p);
        var current = byPid.TryGetValue(pid, out var start) ? start.ParentPid : 0;
        var guard = 0;
        while (current != 0 && byPid.TryGetValue(current, out var parent) && result.Add(current) && guard++ < 128)
            current = parent.ParentPid;
        return result;
    }

    // --- Real process capture (toolhelp snapshot: PID, parent PID and name; access tested read-only) ---

    /// <summary>One toolhelp snapshot of all processes with their real parent PIDs. Names are returned without
    /// ".exe". <paramref name="trackedDcsPid"/> roots the ancestor walk at the DCS actually being boosted; without it
    /// the first DCS found is used. <paramref name="testAccess"/> additionally opens each process once (read-only
    /// check) to report whether it needs administrator rights; the runtime skips that.</summary>
    internal static BoostSnapshot CaptureProcesses(bool testAccess = true, int? trackedDcsPid = null)
    {
        var facts = new List<BoostProcessFact>();
        int? dcsPid = null;
        var snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snapshot == new IntPtr(-1)) return new BoostSnapshot(facts, trackedDcsPid);
        try
        {
            var entry = new PROCESSENTRY32 { dwSize = (uint)Marshal.SizeOf<PROCESSENTRY32>() };
            for (var ok = Process32First(snapshot, ref entry); ok; ok = Process32Next(snapshot, ref entry))
            {
                var name = TrimExe(entry.szExeFile ?? "");
                var pid = (int)entry.th32ProcessID;
                var accessible = !testAccess || IsAccessibleForSetInfo(pid);
                facts.Add(new BoostProcessFact(pid, name, (int)entry.th32ParentProcessID, accessible));
                if (string.Equals(name, DcsProcessName, StringComparison.OrdinalIgnoreCase)) dcsPid ??= pid;
            }
        }
        finally { CloseHandle(snapshot); }
        return new BoostSnapshot(facts, trackedDcsPid ?? dcsPid);
    }

    static bool IsAccessibleForSetInfo(int pid)
    {
        var handle = OpenProcess(PROCESS_SET_INFORMATION, false, (uint)pid);
        if (handle == IntPtr.Zero) return false;
        CloseHandle(handle);
        return true;
    }

    const uint TH32CS_SNAPPROCESS = 0x2;
    const uint PROCESS_SET_INFORMATION = 0x0200;

    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    private struct PROCESSENTRY32
    {
        public uint dwSize;
        public uint cntUsage;
        public uint th32ProcessID;
        public IntPtr th32DefaultHeapID;
        public uint th32ModuleID;
        public uint cntThreads;
        public uint th32ParentProcessID;
        public int pcPriClassBase;
        public uint dwFlags;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 260)] public string szExeFile;
    }

    [DllImport("kernel32.dll", SetLastError = true)] private static extern IntPtr CreateToolhelp32Snapshot(uint flags, uint processId);
    [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)] private static extern bool Process32First(IntPtr snapshot, ref PROCESSENTRY32 entry);
    [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)] private static extern bool Process32Next(IntPtr snapshot, ref PROCESSENTRY32 entry);
    [DllImport("kernel32.dll", SetLastError = true)] private static extern IntPtr OpenProcess(uint access, bool inherit, uint processId);
    [DllImport("kernel32.dll", SetLastError = true)] private static extern bool CloseHandle(IntPtr handle);
}
