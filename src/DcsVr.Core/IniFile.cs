using System.Globalization;

namespace DcsVr.Core;

/// <summary>Reads simple key=value INI files and compares owned values the way their consumers interpret them.</summary>
public static class IniFile
{
    public static Dictionary<string, string> Parse(string text)
    {
        // Keys are section-qualified ("Section.Key") so a duplicate key in another section never matches.
        var values = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
        var section = "";
        foreach (var raw in text.Split('\n'))
        {
            var line = raw.Trim();
            if (line.Length == 0 || line[0] is ';' or '#') continue;
            if (line[0] == '[') { section = line.Trim('[', ']').Trim(); continue; }
            var separator = line.IndexOf('=');
            if (separator <= 0) continue;
            var value = line[(separator + 1)..];
            var comment = value.IndexOf(" ;", StringComparison.Ordinal);
            if (comment >= 0) value = value[..comment];
            values[section + "." + line[..separator].Trim()] = value.Trim();
        }
        return values;
    }

    /// <summary>An INI file's text as its reader takes it: UTF-16 with a BOM (DcsQvCull.ini, ofxr_bridge.ini), otherwise
    /// UTF-8 with or without a BOM (PupilShift.ini, Cheeky's).</summary>
    public static string Decode(byte[] bytes)
    {
        if (bytes is [0xFF, 0xFE, ..]) return System.Text.Encoding.Unicode.GetString(bytes, 2, bytes.Length - 2);
        if (bytes is [0xEF, 0xBB, 0xBF, ..]) return System.Text.Encoding.UTF8.GetString(bytes, 3, bytes.Length - 3);
        return System.Text.Encoding.UTF8.GetString(bytes);
    }

    /// <summary>
    /// True when every owned key still has its value. Numbers compare with float32 tolerance because components
    /// such as Cheeky store settings as float and write 0.35 back as 0.349999994.
    /// </summary>
    public static bool Matches(string text, IReadOnlyDictionary<string, string> owned)
    {
        var current = Parse(text);
        return owned.All(pair => Lookup(current, pair.Key) is { } actual && Same(actual, pair.Value));
    }

    /// <summary>Exact section-qualified key, or an unqualified key from an older journal matched by its name.</summary>
    private static string? Lookup(Dictionary<string, string> current, string key)
    {
        if (current.TryGetValue(key, out var exact)) return exact;
        if (key.Contains('.')) return null;
        var matches = current.Where(pair => pair.Key.EndsWith("." + key, StringComparison.OrdinalIgnoreCase)).Select(pair => pair.Value).ToArray();
        return matches.Length == 1 ? matches[0] : null;
    }

    private static bool Same(string actual, string expected)
    {
        if (string.Equals(actual, expected, StringComparison.OrdinalIgnoreCase)) return true;
        return double.TryParse(actual, NumberStyles.Float, CultureInfo.InvariantCulture, out var a)
            && double.TryParse(expected, NumberStyles.Float, CultureInfo.InvariantCulture, out var b)
            && Math.Abs(a - b) <= 1e-6 * Math.Max(1, Math.Abs(b));
    }
}
