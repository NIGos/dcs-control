using System.Globalization;

namespace DcsVr.Core;

/// <summary>A key plus exactly these modifiers (Ctrl 1, Alt 2, Shift 4; either side). (0, 0) is off.</summary>
public readonly record struct Hotkey(int VirtualKey, int Modifiers)
{
    public bool IsOff => VirtualKey == 0;
    /// <summary>"virtual-key:modifiers" in decimal: the format of DCSVR_NR_HOTKEY and DCSVR_DIAG_HOTKEY and of the
    /// profile values recorded by the key capture field.</summary>
    public string Code => $"{VirtualKey.ToString(CultureInfo.InvariantCulture)}:{Modifiers.ToString(CultureInfo.InvariantCulture)}";
    public override string ToString() => NeuralHotkeys.Label(this);
}

/// <summary>
/// In-flight keys: the DLSS 5 toggle (Cheeky, DCSVR_NR_HOTKEY) and the diagnostic panel (OFXR, DCSVR_DIAG_HOTKEY).
/// Both native parsers take "virtual-key:modifiers" in decimal (key 1-254, modifiers 0-7, "0:0" off) and fire only on
/// the key with exactly those modifiers held. A profile value is "Off", "virtual-key:modifiers" as recorded by the key
/// capture field, or one of the labels earlier versions offered in a menu (<see cref="All"/>), which keep working.
/// </summary>
public static class NeuralHotkeys
{
    public const string Default = "Ctrl+Shift+F12";
    public const string DiagnosticDefault = "Alt+Shift+F12";
    /// <summary>The DCS engine optimizations' in-flight switch: Alt+Shift+F11, free in DCS's default bindings.</summary>
    public const string EngineDefault = "122:6";
    /// <summary>The pupil shift layer's in-flight switch: Ctrl+Shift+F10.</summary>
    public const string PupilDefault = "121:5";
    /// <summary>Depth boost's in-flight switch: Alt+Shift+F10, free in DCS's default bindings (not in
    /// <see cref="DcsDefaultCodes"/>) and outside the engine optimizations' Ctrl+Alt keys.</summary>
    public const string DepthBoostDefault = "121:6";
    /// <summary>Far depth's in-flight switch: Alt+Shift+F8.</summary>
    public const string FarDepthDefault = "119:6";
    /// <summary>The color mode hotkey of the engine module ([Color] ModeKey): off unless chosen.</summary>
    public const string ColorModeDefault = "Off";
    /// <summary>Ctrl+Alt+F8 to F12 and Ctrl+Alt+Page Up/Down, with or without Shift: the engine optimizations module's
    /// own keys (its test suite, for one, starts on Ctrl+Alt+F11 whatever else is held).</summary>
    public static bool EngineModuleKey(Hotkey key) => (key.Modifiers & 3) == 3 && key.VirtualKey is >= 0x77 and <= 0x7B or 0x21 or 0x22;
    /// <summary>The fixed choices of earlier versions, checked free in DCS's default keyboard bindings and the user's own
    /// bindings on 2026-10-04. Profiles may still hold these labels.</summary>
    public static IReadOnlyDictionary<string, (int VirtualKey, int Modifiers)> All { get; } = new Dictionary<string, (int, int)>(StringComparer.OrdinalIgnoreCase)
    {
        ["Off"] = (0, 0),
        ["Ctrl+Shift+F12"] = (0x7B, 1 | 4),
        ["Alt+Shift+F12"] = (0x7B, 2 | 4),
        ["Ctrl+Alt+Shift+F10"] = (0x79, 1 | 2 | 4),
        ["Ctrl+Alt+Shift+F12"] = (0x7B, 1 | 2 | 4),
        ["Ctrl+Alt+Shift+Home"] = (0x24, 1 | 2 | 4),
        ["Ctrl+Alt+Shift+End"] = (0x23, 1 | 2 | 4),
        ["Ctrl+Alt+Shift+Insert"] = (0x2D, 1 | 2 | 4),
    };

    /// <summary>Reads a profile value: "Off", "virtual-key:modifiers" or a legacy label. False for anything else.</summary>
    public static bool TryParse(string? value, out Hotkey key)
    {
        key = default;
        if (string.IsNullOrWhiteSpace(value)) return false;
        value = value.Trim();
        if (All.TryGetValue(value, out var legacy)) { key = new(legacy.VirtualKey, legacy.Modifiers); return true; }
        var colon = value.IndexOf(':');
        if (colon <= 0 || colon == value.Length - 1) return false;
        if (!int.TryParse(value.AsSpan(0, colon), NumberStyles.None, CultureInfo.InvariantCulture, out var vk)
            || !int.TryParse(value.AsSpan(colon + 1), NumberStyles.None, CultureInfo.InvariantCulture, out var modifiers)) return false;
        if (vk is < 0 or > 0xFE || modifiers is < 0 or > 7 || (vk == 0 && modifiers != 0)) return false;
        key = new(vk, modifiers);
        return true;
    }

    /// <summary>The environment value for the native side; "0:0" (off) for anything that does not parse.</summary>
    public static string Environment(string? name, string fallback = Default) => TryParse(name ?? fallback, out var key) ? key.Code : "0:0";

    /// <summary>The key as it reads on a keyboard: "Ctrl+Shift+F12", "Alt+Num 5", "Off".</summary>
    public static string Label(Hotkey key)
    {
        if (key.IsOff) return "Off";
        var parts = new List<string>(4);
        if ((key.Modifiers & 1) != 0) parts.Add("Ctrl");
        if ((key.Modifiers & 2) != 0) parts.Add("Alt");
        if ((key.Modifiers & 4) != 0) parts.Add("Shift");
        parts.Add(KeyName(key.VirtualKey));
        return string.Join("+", parts);
    }

    public static string Label(string? value) => TryParse(value, out var key) ? Label(key) : value ?? "";

    public static string KeyName(int vk) => vk switch
    {
        >= 0x70 and <= 0x87 => "F" + (vk - 0x6F).ToString(CultureInfo.InvariantCulture),
        >= 0x30 and <= 0x39 or >= 0x41 and <= 0x5A => ((char)vk).ToString(),
        >= 0x60 and <= 0x69 => "Num " + (vk - 0x60).ToString(CultureInfo.InvariantCulture),
        _ => Names.TryGetValue(vk, out var name) ? name : "Key " + vk.ToString(CultureInfo.InvariantCulture)
    };

    private static readonly Dictionary<int, string> Names = new()
    {
        [0x08] = "Backspace", [0x09] = "Tab", [0x0D] = "Enter", [0x13] = "Pause", [0x14] = "Caps Lock", [0x1B] = "Esc", [0x20] = "Space",
        [0x21] = "Page Up", [0x22] = "Page Down", [0x23] = "End", [0x24] = "Home", [0x25] = "Left", [0x26] = "Up", [0x27] = "Right", [0x28] = "Down",
        [0x2C] = "Print Screen", [0x2D] = "Insert", [0x2E] = "Delete", [0x6A] = "Num *", [0x6B] = "Num +", [0x6D] = "Num -", [0x6E] = "Num .", [0x6F] = "Num /",
        [0x90] = "Num Lock", [0x91] = "Scroll Lock", [0xBA] = ";", [0xBB] = "=", [0xBC] = ",", [0xBD] = "-", [0xBE] = ".", [0xBF] = "/", [0xC0] = "`",
        [0xDB] = "[", [0xDC] = "\\", [0xDD] = "]", [0xDE] = "'", [0xE2] = "\\ (102nd key)",
    };

    /// <summary>Keys that work on their own; every other key needs Ctrl, Alt or Shift so typing never triggers it.</summary>
    public static bool WorksAlone(int vk) => vk is >= 0x70 and <= 0x87 or 0x13 or 0x91 or 0x2D or 0x24 or 0x23 or 0x21 or 0x22;

    /// <summary>Why the key cannot be used, or null when it can. Off is always allowed.</summary>
    public static string? Problem(Hotkey key)
    {
        if (key.IsOff) return null;
        if (key.VirtualKey is 0x10 or 0x11 or 0x12 or >= 0xA0 and <= 0xA5 or 0x5B or 0x5C or 0x5D or 0x14)
            return "Ctrl, Alt, Shift, Windows and Caps Lock are not keys on their own. Hold them with another key.";
        if (key.VirtualKey is >= 0x01 and <= 0x06) return "Mouse buttons cannot be used.";
        if (key.VirtualKey is 0xE5) return "This key has no usable key code. Choose another key.";
        if (Reserved.Contains((key.VirtualKey, key.Modifiers))) return Label(key) + " is used by Windows.";
        if (key.Modifiers == 0 && !WorksAlone(key.VirtualKey))
            return Label(key) + " needs Ctrl, Alt or Shift. F-keys, Pause, Scroll Lock, Insert, Home, End, Page Up and Page Down also work alone.";
        return null;
    }

    private static readonly HashSet<(int, int)> Reserved = [(0x73, 2), (0x09, 2), (0x1B, 1), (0x1B, 2), (0x1B, 5), (0x20, 2), (0x2E, 3)];

    /// <summary>Key combinations bound by default in DCS's own keyboard layers (Config\Input: UI layer, common and
    /// default aircraft bindings, cameras, command menu, voice chat, Supercarrier and the other global layers), read
    /// from DCS 2.9.30 on 2026-10-05 with left and right modifiers merged, as the native parsers do. Aircraft modules
    /// add their own bindings, which are not listed.</summary>
    private const string DcsDefaultCodes =
        "8:0 9:1 9:4 9:5 13:0 13:1 19:0 19:1 27:0 32:0 33:0 33:1 33:2 33:4 34:0 34:1 34:2 34:4 35:0 35:1 35:2 35:4 35:5 36:1 36:2 36:4 36:5 45:0 45:2 46:0 46:1 46:2 "
        + "49:0 49:2 50:0 50:2 51:0 51:2 52:2 53:2 65:0 65:1 65:4 66:0 66:1 66:2 66:4 67:0 67:1 67:2 67:4 68:0 68:1 68:4 69:0 69:1 69:4 70:0 70:2 71:1 71:2 71:4 "
        + "72:1 72:2 72:5 72:6 72:7 73:0 74:2 74:4 75:0 75:1 75:2 75:4 76:0 76:1 76:2 76:4 77:0 77:2 77:4 77:7 78:0 78:2 78:4 79:0 79:7 80:0 80:4 80:7 81:0 81:1 "
        + "81:4 82:0 82:4 82:7 83:0 83:1 83:4 83:7 84:0 84:7 85:0 86:2 86:4 87:0 87:1 87:2 87:4 88:1 89:0 89:1 89:2 89:5 90:0 90:1 90:2 90:4 90:6 96:0 96:1 96:2 "
        + "97:0 97:1 97:2 97:3 97:4 97:7 98:0 98:1 98:2 98:3 98:4 98:5 98:7 99:0 99:1 99:2 99:3 99:4 99:7 100:0 100:1 100:2 100:3 100:4 100:5 100:7 101:0 101:1 "
        + "101:2 101:3 101:4 101:5 102:0 102:1 102:2 102:3 102:4 102:5 102:7 103:0 103:1 103:2 103:3 103:4 103:7 104:0 104:1 104:2 104:3 104:4 104:5 104:7 105:0 "
        + "105:1 105:2 105:3 105:4 105:7 106:0 106:1 106:2 106:3 106:4 106:5 106:7 107:1 107:2 107:4 109:2 109:4 109:5 110:0 110:1 110:2 110:4 111:0 111:1 111:2 "
        + "111:3 111:4 111:5 111:7 112:0 112:1 112:2 112:4 113:0 113:1 113:2 113:4 113:5 114:0 114:1 115:0 115:1 115:2 115:4 116:0 116:1 116:2 117:0 117:1 117:2 "
        + "117:4 118:0 118:1 118:2 119:0 119:2 120:0 120:1 120:2 120:3 120:4 120:5 120:7 121:0 121:1 121:2 121:4 122:0 122:1 122:2 122:5 122:7 123:0 123:1 123:2 "
        + "123:4 144:0 187:0 187:4 187:7 188:0 188:3 189:4 189:7 190:0 190:3 192:1 192:2 192:4 219:0 219:1 219:2 219:4 220:1 220:4 221:0 221:1 221:2 221:4 222:0 222:2 222:4";

    /// <summary>The "virtual-key:modifiers" codes DCS binds by default (see <see cref="DcsDefaultCodes"/>), as virtual keys
    /// of the US layout.</summary>
    public static IReadOnlySet<string> DcsDefaults { get; } = DcsDefaultCodes.Split(' ', StringSplitOptions.RemoveEmptyEntries).ToHashSet(StringComparer.Ordinal);

    /// <summary>
    /// <see cref="DcsDefaults"/> as virtual keys of the keyboard layout in use. DCS binds physical keys (DirectInput scan
    /// codes, named after the US layout), while the in-flight keys are virtual keys of the user's layout: on AZERTY, DCS's
    /// "Z" binding is the key that types W. Letters, digits and punctuation are taken to their US scan code and back to
    /// the virtual key that scan code produces in the current layout; F-keys, navigation keys and the numeric keypad are
    /// the same in every layout and are kept as they are. A key the layout does not map is dropped (no warning).
    /// </summary>
    public static IReadOnlySet<string> DcsDefaultsForLayout(Func<int, int?> virtualKeyOfScanCode)
    {
        var result = new HashSet<string>(StringComparer.Ordinal);
        foreach (var code in DcsDefaults)
        {
            var colon = code.IndexOf(':');
            var vk = int.Parse(code.AsSpan(0, colon), CultureInfo.InvariantCulture);
            if (!UsScanCodes.TryGetValue(vk, out var scan)) { result.Add(code); continue; }
            if (virtualKeyOfScanCode(scan) is { } local and > 0 and < 0xFF) result.Add(local.ToString(CultureInfo.InvariantCulture) + code[colon..]);
        }
        return result;
    }

    /// <summary><see cref="DcsDefaultsForLayout(Func{int, int?})"/> for the keyboard layout of the calling thread
    /// (MapVirtualKeyEx), cached per layout. Falls back to the US list when the layout cannot be read.</summary>
    public static IReadOnlySet<string> DcsDefaultsForCurrentLayout()
    {
        try
        {
            var layout = GetKeyboardLayout(0);
            if (layout == IntPtr.Zero) return DcsDefaults;
            if (_layoutDefaults is { } cached && cached.Layout == layout) return cached.Keys;
            var keys = DcsDefaultsForLayout(scan => MapVirtualKeyEx((uint)scan, 1 /* MAPVK_VSC_TO_VK */, layout) is var vk and > 0 ? (int)vk : null);
            _layoutDefaults = (layout, keys);
            return keys;
        }
        catch (Exception e) when (e is DllNotFoundException or EntryPointNotFoundException) { return DcsDefaults; }
    }
    private static (IntPtr Layout, IReadOnlySet<string> Keys)? _layoutDefaults;
    [System.Runtime.InteropServices.DllImport("user32.dll")] private static extern IntPtr GetKeyboardLayout(uint thread);
    [System.Runtime.InteropServices.DllImport("user32.dll")] private static extern uint MapVirtualKeyEx(uint code, uint mapType, IntPtr layout);

    /// <summary>Scan codes of the layout-dependent keys in the US layout (letters, digits, punctuation, the 102nd key).</summary>
    private static readonly Dictionary<int, int> UsScanCodes = BuildUsScanCodes();
    private static Dictionary<int, int> BuildUsScanCodes()
    {
        var map = new Dictionary<int, int> { [0x30] = 0x0B };
        for (var digit = 1; digit <= 9; digit++) map[0x30 + digit] = 0x01 + digit;
        foreach (var (row, first) in new[] { ("QWERTYUIOP", 0x10), ("ASDFGHJKL", 0x1E), ("ZXCVBNM", 0x2C) })
            for (var i = 0; i < row.Length; i++) map[row[i]] = first + i;
        foreach (var (vk, scan) in new[] { (0xBA, 0x27), (0xBB, 0x0D), (0xBC, 0x33), (0xBD, 0x0C), (0xBE, 0x34), (0xBF, 0x35), (0xC0, 0x29), (0xDB, 0x1A), (0xDC, 0x2B), (0xDD, 0x1B), (0xDE, 0x28), (0xE2, 0x56) })
            map[vk] = scan;
        return map;
    }

    /// <summary>True when DCS binds this combination by default (on the same physical key, in the keyboard layout in use,
    /// or in <paramref name="defaults"/>): it still works, but DCS reacts to it as well.</summary>
    public static bool IsDcsDefault(Hotkey key, IReadOnlySet<string>? defaults = null) => !key.IsOff && (defaults ?? DcsDefaultsForCurrentLayout()).Contains(key.Code);

    /// <summary>Same key and modifiers, whatever the spelling (legacy label or code).</summary>
    public static bool Same(string? a, string? b) => TryParse(a, out var x) && TryParse(b, out var y) && x == y;
}
