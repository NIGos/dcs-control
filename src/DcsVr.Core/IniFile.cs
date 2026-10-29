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
            if (line[0] == '[')
            {
                var close = line.IndexOf(']');
                section = (close > 0 ? line[1..close] : line.Trim('[', ']')).Trim();
                continue;
            }
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

    /// <summary>Text in the encoding <paramref name="like"/> uses (the bytes of the file it replaces): UTF-16 or UTF-8 with
    /// their BOM, otherwise UTF-8 without one.</summary>
    public static byte[] Encode(string text, byte[] like)
    {
        if (like is [0xFF, 0xFE, ..]) return [0xFF, 0xFE, .. System.Text.Encoding.Unicode.GetBytes(text)];
        if (like is [0xEF, 0xBB, 0xBF, ..]) return [0xEF, 0xBB, 0xBF, .. System.Text.Encoding.UTF8.GetBytes(text)];
        return System.Text.Encoding.UTF8.GetBytes(text);
    }

    /// <summary>
    /// <paramref name="text"/> with the section-qualified keys of <paramref name="values"/> set (every occurrence), each
    /// other line, comment and the line endings kept. A key the text lacks is added at the end of its section (the section
    /// is added when missing).
    /// </summary>
    public static string SetValues(string text, IReadOnlyDictionary<string, string> values)
    {
        var newline = text.Contains("\r\n", StringComparison.Ordinal) ? "\r\n" : "\n";
        var lines = text.Replace("\r\n", "\n", StringComparison.Ordinal).Split('\n').ToList();
        var found = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        var sectionEnd = new Dictionary<string, int>(StringComparer.OrdinalIgnoreCase);
        var section = "";
        for (var i = 0; i < lines.Count; i++)
        {
            var line = lines[i].Trim();
            if (line.Length == 0 || line[0] is ';' or '#') continue;
            if (line[0] == '[') { section = line.Trim('[', ']').Trim(); sectionEnd[section] = i + 1; continue; }
            var separator = lines[i].IndexOf('=');
            if (separator <= 0) continue;
            var key = section + "." + lines[i][..separator].Trim();
            sectionEnd[section] = i + 1;
            if (!values.TryGetValue(key, out var value)) continue;
            lines[i] = lines[i][..(separator + 1)] + value;
            found.Add(key);
        }
        foreach (var group in values.Where(p => !found.Contains(p.Key)).GroupBy(p => p.Key.Contains('.') ? p.Key[..p.Key.LastIndexOf('.')] : "", StringComparer.OrdinalIgnoreCase)
            .OrderByDescending(g => sectionEnd.GetValueOrDefault(g.Key, int.MaxValue)))
        {
            var added = group.Select(p => p.Key[(p.Key.LastIndexOf('.') + 1)..] + "=" + p.Value).ToList();
            if (sectionEnd.TryGetValue(group.Key, out var at)) { lines.InsertRange(at, added); continue; }
            if (lines.Count > 0 && lines[^1].Length == 0) lines.RemoveAt(lines.Count - 1);
            if (group.Key.Length > 0) lines.Add("[" + group.Key + "]");
            lines.AddRange(added);
            lines.Add("");
        }
        return string.Join(newline, lines);
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
