using System.Runtime.InteropServices;

namespace DcsVr.Core;

/// <summary>A monitor mode: resolution and refresh rate.</summary>
public readonly record struct DisplayMode(int Width, int Height, int Refresh)
{
    public override string ToString() => $"{Width}×{Height} at {Refresh} Hz";
}

/// <summary>A display's complete current mode, as read before the flight, so exactly this can be set back.</summary>
public sealed record SavedDisplayMode(int Width, int Height, int Refresh, int BitsPerPixel, int PositionX, int PositionY)
{
    public DisplayMode Mode => new(Width, Height, Refresh);
}

/// <summary>The Win32 display calls the monitor mode feature needs, so its bookkeeping is testable without a monitor.</summary>
public interface IDisplayModes
{
    /// <summary>The primary display's device name (e.g. \\.\DISPLAY1), or null.</summary>
    string? PrimaryDevice();
    /// <summary>A readable name for the display (its monitor), or the device name.</summary>
    string Name(string device);
    SavedDisplayMode? Current(string device);
    /// <summary>The modes the monitor reports (progressive, the current colour depth), largest first.</summary>
    IReadOnlyList<DisplayMode> Modes(string device);
    /// <summary>Sets a mode for this session only: never written to the registry, so a restart brings back the saved mode.</summary>
    bool Set(string device, SavedDisplayMode mode);
    /// <summary>Returns the display to the mode Windows has saved for it.</summary>
    bool Reset(string device);
}

/// <summary>What Lower the monitor while flying would do, as shown before launch.</summary>
/// <param name="Offered">The monitor reports the flight mode.</param>
public sealed record MonitorPlan(string Device, string Name, DisplayMode Current, DisplayMode Flight, bool Offered, IReadOnlyList<DisplayMode> Modes, string Note);

/// <summary>Written before the monitor mode changes and removed once it is set back: a leftover file means the change
/// was not undone (the helper was ended), so the next app or helper start puts the mode back.</summary>
public sealed record DisplayMarker(string Device, SavedDisplayMode Previous, DisplayMode Applied, DateTimeOffset At, int HelperPid);

/// <summary>
/// Lower the monitor while flying. <see cref="Apply"/> records the display's exact mode and a marker file, then sets the
/// flight mode for this session only (ChangeDisplaySettingsEx without CDS_UPDATEREGISTRY, so Windows never saves it);
/// <see cref="Restore"/> sets the recorded mode back, but only while the display still shows the flight mode, so a mode
/// the user chose in between is kept. HDR is never changed. Pure over <see cref="IDisplayModes"/>.
/// </summary>
public sealed class FlightDisplay(IDisplayModes displays, string markerPath)
{
    private readonly object _lock = new();
    public string? Device { get; private set; }
    public SavedDisplayMode? Previous { get; private set; }
    public DisplayMode? Applied { get; private set; }

    public static DisplayMode Target(VrProfile profile) => new(profile.FlightDisplayWidth, profile.FlightDisplayHeight, profile.FlightDisplayRefresh);

    /// <summary>The plan for the primary display, or null when no display can be read.</summary>
    public static MonitorPlan? Describe(IDisplayModes displays, DisplayMode flight)
    {
        if (displays.PrimaryDevice() is not { } device || displays.Current(device) is not { } current) return null;
        var modes = displays.Modes(device);
        var offered = modes.Contains(flight);
        var note = !offered ? $"The monitor does not report {flight}; the monitor is left as it is. Select one of its modes."
            : current.Mode == flight ? "The monitor already runs this mode; nothing changes."
            : "Set when DCS starts, back to the current mode when DCS exits. Never saved as a Windows setting; HDR is left as it is.";
        return new(device, displays.Name(device), current.Mode, flight, offered, modes, note);
    }

    /// <summary>Switches the primary display to <paramref name="target"/>. Returns what happened, for the log.</summary>
    public string Apply(DisplayMode target)
    {
        lock (_lock)
        {
            if (Applied is not null) return "The monitor mode is already changed.";
            if (displays.PrimaryDevice() is not { } device) return "No primary display found; the monitor is left as it is.";
            if (displays.Current(device) is not { } current) return $"The mode of {device} could not be read; the monitor is left as it is.";
            if (current.Mode == target) return $"{device} already runs {target}; nothing changed.";
            if (!displays.Modes(device).Contains(target)) return $"{device} does not report {target}; the monitor is left as it is.";
            // The marker is written first: whatever ends this process after the change, the next start sets it back.
            WriteMarker(new DisplayMarker(device, current, target, DateTimeOffset.UtcNow, Environment.ProcessId));
            if (!displays.Set(device, current with { Width = target.Width, Height = target.Height, Refresh = target.Refresh }))
            {
                DeleteMarker();
                return $"Windows refused {target} for {device}; the monitor is left as it is.";
            }
            Device = device; Previous = current; Applied = target;
            return $"{device}: {current.Mode} -> {target} for the flight.";
        }
    }

    /// <summary>Sets the recorded mode back, once. Null when nothing was changed.</summary>
    public string? Restore()
    {
        lock (_lock)
        {
            if (Device is not { } device || Previous is not { } previous || Applied is not { } applied) return null;
            Device = null; Previous = null; Applied = null;
            return PutBack(displays, device, previous, applied, DeleteMarker);
        }
    }

    /// <summary>A marker left by a helper that did not restore: the recorded mode is set back if the display still
    /// shows the flight mode, and the marker is removed either way (a restart already brought the saved mode back).
    /// Null when there is no marker.</summary>
    public static string? RestoreLeftover(IDisplayModes displays, string markerPath)
    {
        DisplayMarker? marker;
        try { marker = File.Exists(markerPath) ? JsonData.Deserialize<DisplayMarker>(File.ReadAllText(markerPath)) : null; }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException or System.Text.Json.JsonException or InvalidDataException) { marker = null; TryDelete(markerPath); return "An unreadable monitor mode record was removed."; }
        if (marker is null) return null;
        return PutBack(displays, marker.Device, marker.Previous, marker.Applied, () => TryDelete(markerPath));
    }

    private static string PutBack(IDisplayModes displays, string device, SavedDisplayMode previous, DisplayMode applied, Action done)
    {
        var now = displays.Current(device);
        if (now is null || now.Mode != applied)
        {
            done();
            return now is null ? $"{device} is not connected; nothing to set back." : $"{device} runs {now.Mode}, not the flight mode; left as it is.";
        }
        if (displays.Set(device, previous) || displays.Reset(device)) { done(); return $"{device}: back to {previous.Mode}."; }
        return $"{device} could not be set back to {previous.Mode}; it returns at the next restart or from Windows display settings.";
    }

    private void WriteMarker(DisplayMarker marker)
    {
        try { AtomicFile.WriteText(markerPath, JsonData.Serialize(marker)); }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException) { }
    }
    private void DeleteMarker() => TryDelete(markerPath);
    private static void TryDelete(string path) { try { File.Delete(path); } catch (Exception e) when (e is IOException or UnauthorizedAccessException) { } }
}

/// <summary>The real display calls (user32). Reading never changes anything; <see cref="Set"/> and
/// <see cref="Reset"/> are used only by the boost helper and the leftover restore.</summary>
public sealed class WindowsDisplayModes : IDisplayModes
{
    public string? PrimaryDevice()
    {
        var device = NewDevice();
        for (uint i = 0; EnumDisplayDevicesW(null, i, ref device, 0); i++, device = NewDevice())
            if ((device.StateFlags & (DISPLAY_DEVICE_PRIMARY_DEVICE | DISPLAY_DEVICE_ATTACHED_TO_DESKTOP)) == (DISPLAY_DEVICE_PRIMARY_DEVICE | DISPLAY_DEVICE_ATTACHED_TO_DESKTOP))
                return device.DeviceName;
        return null;
    }

    public string Name(string device)
    {
        var monitor = NewDevice();
        return EnumDisplayDevicesW(device, 0, ref monitor, 0) && !string.IsNullOrWhiteSpace(monitor.DeviceString) ? $"{monitor.DeviceString} ({device})" : device;
    }

    public SavedDisplayMode? Current(string device)
    {
        var mode = NewMode();
        if (!EnumDisplaySettingsW(device, ENUM_CURRENT_SETTINGS, ref mode)) return null;
        return new((int)mode.dmPelsWidth, (int)mode.dmPelsHeight, (int)mode.dmDisplayFrequency, (int)mode.dmBitsPerPel, mode.dmPositionX, mode.dmPositionY);
    }

    public IReadOnlyList<DisplayMode> Modes(string device)
    {
        var depth = Current(device)?.BitsPerPixel ?? 32;
        var result = new HashSet<DisplayMode>();
        var mode = NewMode();
        for (var i = 0; i < 4096 && EnumDisplaySettingsW(device, i, ref mode); i++, mode = NewMode())
            // Below 1024×576 a desktop is impractical even for a flight; such modes only lengthen the list.
            if (mode.dmBitsPerPel == depth && (mode.dmDisplayFlags & DM_INTERLACED) == 0 && mode.dmDisplayFrequency > 1 && mode.dmPelsWidth >= 1024 && mode.dmPelsHeight >= 576)
                result.Add(new((int)mode.dmPelsWidth, (int)mode.dmPelsHeight, (int)mode.dmDisplayFrequency));
        return result.OrderByDescending(m => m.Width * (long)m.Height).ThenByDescending(m => m.Width).ThenByDescending(m => m.Refresh).ToArray();
    }

    public bool Set(string device, SavedDisplayMode target)
    {
        var mode = NewMode();
        if (!EnumDisplaySettingsW(device, ENUM_CURRENT_SETTINGS, ref mode)) return false;
        mode.dmPelsWidth = (uint)target.Width; mode.dmPelsHeight = (uint)target.Height; mode.dmDisplayFrequency = (uint)target.Refresh;
        mode.dmBitsPerPel = (uint)target.BitsPerPixel; mode.dmPositionX = target.PositionX; mode.dmPositionY = target.PositionY;
        mode.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_DISPLAYFREQUENCY | DM_BITSPERPEL | DM_POSITION;
        // No CDS_UPDATEREGISTRY: the mode is for this session only and is never saved by Windows.
        return ChangeDisplaySettingsExW(device, ref mode, IntPtr.Zero, 0, IntPtr.Zero) == DISP_CHANGE_SUCCESSFUL;
    }

    public bool Reset(string device) => ChangeDisplaySettingsExW(device, IntPtr.Zero, IntPtr.Zero, 0, IntPtr.Zero) == DISP_CHANGE_SUCCESSFUL;

    static DEVMODE NewMode() => new() { dmSize = (ushort)Marshal.SizeOf<DEVMODE>() };
    static DISPLAY_DEVICE NewDevice() => new() { cb = Marshal.SizeOf<DISPLAY_DEVICE>() };

    const int ENUM_CURRENT_SETTINGS = -1, DISP_CHANGE_SUCCESSFUL = 0;
    const uint DISPLAY_DEVICE_ATTACHED_TO_DESKTOP = 0x1, DISPLAY_DEVICE_PRIMARY_DEVICE = 0x4, DM_INTERLACED = 0x2;
    const uint DM_POSITION = 0x20, DM_BITSPERPEL = 0x40000, DM_PELSWIDTH = 0x80000, DM_PELSHEIGHT = 0x100000, DM_DISPLAYFREQUENCY = 0x400000;

    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    private struct DEVMODE
    {
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)] public string dmDeviceName;
        public ushort dmSpecVersion, dmDriverVersion, dmSize, dmDriverExtra;
        public uint dmFields;
        public int dmPositionX, dmPositionY;
        public uint dmDisplayOrientation, dmDisplayFixedOutput;
        public short dmColor, dmDuplex, dmYResolution, dmTTOption, dmCollate;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)] public string dmFormName;
        public ushort dmLogPixels;
        public uint dmBitsPerPel, dmPelsWidth, dmPelsHeight, dmDisplayFlags, dmDisplayFrequency;
        public uint dmICMMethod, dmICMIntent, dmMediaType, dmDitherType, dmReserved1, dmReserved2, dmPanningWidth, dmPanningHeight;
    }

    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    private struct DISPLAY_DEVICE
    {
        public int cb;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)] public string DeviceName;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 128)] public string DeviceString;
        public uint StateFlags;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 128)] public string DeviceID;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 128)] public string DeviceKey;
    }

    [DllImport("user32.dll", CharSet = CharSet.Unicode)] private static extern bool EnumDisplayDevicesW(string? device, uint index, ref DISPLAY_DEVICE info, uint flags);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] private static extern bool EnumDisplaySettingsW(string device, int mode, ref DEVMODE info);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] private static extern int ChangeDisplaySettingsExW(string device, ref DEVMODE mode, IntPtr hwnd, uint flags, IntPtr parameter);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] private static extern int ChangeDisplaySettingsExW(string device, IntPtr mode, IntPtr hwnd, uint flags, IntPtr parameter);
}
