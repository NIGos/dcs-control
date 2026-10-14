using System.Runtime.InteropServices;

namespace DcsVr.Core;

/// <summary>NVIDIA Control Panel's "Threaded optimization" as DCS gets it.</summary>
public enum ThreadedOptimization { Unknown, Auto, On, Off }

/// <param name="Value">The value DCS gets: its own program profile's when set there, otherwise the global one.</param>
/// <param name="FromDcsProfile">The value comes from the DCS program profile (otherwise from Global settings).</param>
public sealed record ThreadedOptimizationSetting(ThreadedOptimization Value, bool FromDcsProfile)
{
    public string Where => FromDcsProfile ? "the DCS program profile" : "Global settings";
    /// <summary>The steps that turn it back to Auto.</summary>
    public string Fix => FromDcsProfile
        ? "NVIDIA Control Panel → Manage 3D settings → Program settings → DCS World: set Threaded optimization to Use global setting (Auto)"
        : "NVIDIA Control Panel → Manage 3D settings → Global settings: set Threaded optimization to Auto";
}

/// <summary>NVIDIA Control Panel's "Antialiasing - MFAA" (sample interleaving) as DCS gets it. On, the driver reports no
/// D3D11 command lists to DCS, like Threaded optimization Off (found by a pilot, GitHub issue #1).</summary>
public sealed record MfaaSetting(bool On, bool FromDcsProfile)
{
    public string Where => FromDcsProfile ? "the DCS program profile" : "Global settings";
    public string Fix => FromDcsProfile
        ? "NVIDIA Control Panel → Manage 3D settings → Program settings → DCS World: set Antialiasing - MFAA to Off (or Use global setting)"
        : "NVIDIA Control Panel → Manage 3D settings → Global settings: set Antialiasing - MFAA to Off";
}

/// <summary>
/// Reads (never writes) NVIDIA driver settings through NVAPI's driver settings (DRS) interface in nvapi64.dll, which
/// every NVIDIA driver installs. "Threaded optimization" Off makes the driver report no D3D11 command lists to DCS, so
/// the engine optimizations' multi-threaded shadows and G-buffer cannot run; the app says so before the flight.
/// Without an NVIDIA driver, or when NVAPI answers anything unexpected, the result is null.
/// </summary>
public static class NvidiaSettings
{
    /// <summary>OGL_THREAD_CONTROL_ID: the Control Panel's "Threaded optimization" (it applies to Direct3D too).</summary>
    private const uint ThreadControlId = 0x20C1221E;
    /// <summary>MFAA_ENABLE_ID: the Control Panel's "Antialiasing - MFAA" (Profile Inspector: Enable sample interleaving).</summary>
    private const uint MfaaId = 0x0098C1AC;
    private const int SettingNotFound = -160;

    // Read at most every 20 s: the checks run often, and the pilot may change it in the Control Panel meanwhile.
    private static (DateTime At, (ThreadedOptimizationSetting? Threaded, MfaaSetting? Mfaa) Value)? _last;
    private static readonly Lock CacheLock = new();
    private static (ThreadedOptimizationSetting? Threaded, MfaaSetting? Mfaa) CachedRead()
    {
        lock (CacheLock)
        {
            if (_last is { } last && DateTime.UtcNow - last.At < TimeSpan.FromSeconds(20)) return last.Value;
            var value = Read(); _last = (DateTime.UtcNow, value); return value;
        }
    }
    /// <summary>Replaceable so tests and the offline verification never depend on the build machine's driver.</summary>
    public static Func<ThreadedOptimizationSetting?> ThreadedOptimizationForDcs { get; set; } = () => CachedRead().Threaded;
    /// <summary>Replaceable like <see cref="ThreadedOptimizationForDcs"/>.</summary>
    public static Func<MfaaSetting?> MfaaForDcs { get; set; } = () => CachedRead().Mfaa;

    /// <summary>Read again (after the pilot changed it in the Control Panel); the next call reads the driver.</summary>
    public static (ThreadedOptimizationSetting? Threaded, MfaaSetting? Mfaa) Fresh() => Read();

    private static (ThreadedOptimizationSetting? Threaded, MfaaSetting? Mfaa) Read()
    {
        if (!OperatingSystem.IsWindows() || !Environment.Is64BitProcess) return (null, null);
        IntPtr library = IntPtr.Zero, session = IntPtr.Zero;
        try
        {
            if (!NativeLibrary.TryLoad("nvapi64.dll", out library) || !NativeLibrary.TryGetExport(library, "nvapi_QueryInterface", out var query)) return (null, null);
            var queryInterface = Marshal.GetDelegateForFunctionPointer<QueryInterface>(query);
            T? Function<T>(uint id) where T : Delegate => queryInterface(id) is var p && p != IntPtr.Zero ? Marshal.GetDelegateForFunctionPointer<T>(p) : null;
            var initialize = Function<Initialize>(0x0150E828);
            var createSession = Function<CreateSession>(0x0694D52E);
            var loadSettings = Function<LoadSettings>(0x375DBD6B);
            var baseProfile = Function<GetBaseProfile>(0xDA8466A0);
            var findApplication = Function<FindApplicationByName>(0xEEE566B2);
            var getSetting = Function<GetSetting>(0x73BF8338);
            var destroySession = Function<DestroySession>(0xDAD9CFF8);
            if (initialize is null || createSession is null || loadSettings is null || baseProfile is null || findApplication is null || getSetting is null || destroySession is null) return (null, null);
            if (initialize() != 0 || createSession(out session) != 0 || loadSettings(session) != 0) return (null, null);
            try
            {
                // DCS's own profile first (NVIDIA ships one for DCS World), then Global settings, then the default (Auto).
                var application = new byte[ApplicationV3Size];
                BitConverter.GetBytes(ApplicationV3Size | (3u << 16)).CopyTo(application, 0);
                var name = new char[2048]; "dcs.exe".CopyTo(0, name, 0, 7);  // NvAPI_UnicodeString: a full 2048-character buffer
                var hasDcs = findApplication(session, name, out var dcsProfile, application) == 0;
                var hasGlobal = baseProfile(session, out var global) == 0;
                (uint Value, bool FromDcs)? Effective(uint id) =>
                    hasDcs && Value(getSetting, session, dcsProfile, id) is { } own ? (own, true)
                    : hasGlobal && Value(getSetting, session, global, id) is { } shared ? (shared, false) : null;
                ThreadedOptimizationSetting? threaded = !hasDcs && !hasGlobal ? null
                    : Effective(ThreadControlId) is { } t ? new(t.Value switch { 0 => ThreadedOptimization.Auto, 1 => ThreadedOptimization.On, 2 => ThreadedOptimization.Off, _ => ThreadedOptimization.Unknown }, t.FromDcs)
                    : new(ThreadedOptimization.Auto, false);
                MfaaSetting? mfaa = !hasDcs && !hasGlobal ? null : Effective(MfaaId) is { } m ? new(m.Value != 0, m.FromDcs) : new(false, false);
                return (threaded, mfaa);
            }
            finally { destroySession(session); }
        }
        catch (Exception e) when (e is DllNotFoundException or EntryPointNotFoundException or BadImageFormatException or SEHException or MarshalDirectiveException) { return (null, null); }
        finally { if (library != IntPtr.Zero) NativeLibrary.Free(library); }
    }

    /// <summary>The setting in one profile; null when that profile does not set it.</summary>
    private static uint? Value(GetSetting getSetting, IntPtr session, IntPtr profile, uint id)
    {
        var setting = new byte[SettingV1Size];
        BitConverter.GetBytes(SettingV1Size | (1u << 16)).CopyTo(setting, 0);
        var status = getSetting(session, profile, id, setting);
        if (status == SettingNotFound) return null;
        if (status != 0) return null;
        // NVDRS_SETTING_V1: version, settingName[2048] (wide), settingId, settingType, settingLocation,
        // isCurrentPredefined, isPredefinedValid, predefinedValue (4100 bytes), currentValue (4100 bytes).
        return BitConverter.ToUInt32(setting, CurrentValueOffset);
    }

    private const uint SettingV1Size = 12320;
    private const int CurrentValueOffset = 4 + 4096 + 5 * 4 + 4100;
    private const uint ApplicationV3Size = 4 + 4 + 4 * 4096 + 4;

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate IntPtr QueryInterface(uint id);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate int Initialize();
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate int CreateSession(out IntPtr session);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate int DestroySession(IntPtr session);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate int LoadSettings(IntPtr session);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate int GetBaseProfile(IntPtr session, out IntPtr profile);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl, CharSet = CharSet.Unicode)]
    private delegate int FindApplicationByName(IntPtr session, [In] char[] appName, out IntPtr profile, [In, Out] byte[] application);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate int GetSetting(IntPtr session, IntPtr profile, uint settingId, [In, Out] byte[] setting);
}
