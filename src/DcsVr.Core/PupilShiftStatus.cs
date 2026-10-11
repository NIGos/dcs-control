using System.Globalization;
using System.Text.RegularExpressions;

namespace DcsVr.Core;

/// <summary>A range of values, minimum to maximum.</summary>
public sealed record PupilRange(double Min, double Max);

/// <summary>One second of the pupil shift layer's statistics (a "stats:" line of PupilShift.log).</summary>
/// <param name="Frames">Frames DCS rendered in that second.</param>
/// <param name="Shifted">Frames rendered from the pupil position.</param>
/// <param name="Restored">Projection views given back to the runtime with its own poses (null in logs without the count).</param>
/// <param name="GazeX">Horizontal gaze range, degrees (null without a gaze).</param>
/// <param name="GazeY">Vertical gaze range, degrees.</param>
/// <param name="MaxShiftMm">The largest pupil shift, millimetres.</param>
/// <param name="RuntimeEyeX">Where the runtime itself puts the left eye sideways, relative to the head centre, millimetres.</param>
public sealed record PupilShiftSecond(int Frames, int Shifted, int? Restored, PupilRange? GazeX, PupilRange? GazeY, double MaxShiftMm, PupilRange RuntimeEyeX, bool CorrectionOff);

/// <summary>What the pupil shift layer reports in PupilShift.log, for the Pupil shift page.</summary>
/// <param name="State">not-installed, waiting (installed, DCS has not loaded it yet), working, no-gaze, off, not-restored.</param>
/// <param name="Live">The log is being written now (DCS runs); otherwise it is the last flight's.</param>
public sealed record PupilShiftStatus(string State, string Summary, bool Live, string? LogPath, DateTime? LogTime, PupilShiftSecond? Last,
    PupilRange? SessionGazeX, PupilRange? SessionGazeY, double SessionMaxShiftMm, int SecondsWithGaze, int Seconds,
    int? RestoredViews, string? Hotkey, IReadOnlyList<string> Warnings)
{
    public const string FolderName = "pupilshift";
    public const string LogName = "PupilShift.log";
    public const string LayerDll = "XR_APILAYER_DCSVR_pupil_shift.dll";

    private static readonly Regex Stats = new(@"stats: frames=(\d+) shifted=(\d+)(?: restored=(\d+))?(?: gaze x\[(-?[\d.]+)\.\.(-?[\d.]+)\] y\[(-?[\d.]+)\.\.(-?[\d.]+)\] deg, max shift ([\d.]+) mm)?(?<off> \(correction off\))?.*?runtime left eye vs head x\[(-?[\d.]+)\.\.(-?[\d.]+)\]", RegexOptions.CultureInvariant);
    private static readonly Regex EndFrame = new(@"end_frame: restored (\d+) projection views", RegexOptions.CultureInvariant);
    private static readonly Regex Toggle = new(@"toggle=(\d+:\d)", RegexOptions.CultureInvariant);

    /// <summary>The deployed layer's folder of an applied profile.</summary>
    public static string Folder(string managedRoot, string profileId) => PathPolicy.UnderRoot(managedRoot, "profiles/" + profileId + "/" + FolderName);

    /// <summary>Reads the layer's log in <paramref name="folder"/> (null: no applied profile with pupil shift).</summary>
    public static PupilShiftStatus Read(string? folder, bool dcsRunning)
    {
        if (folder is null || !File.Exists(Path.Combine(folder, LayerDll)))
            return new("not-installed", "Not installed. Launch DCS installs it with a profile that uses it.", false, null, null, null, null, null, 0, 0, 0, null, null, []);
        var log = Path.Combine(folder, LogName);
        if (!File.Exists(log))
            return new("waiting", dcsRunning ? "Installed. DCS has not started VR yet." : "Installed. It reports here once DCS starts VR.", false, null, null, null, null, null, 0, 0, 0, null, null, []);
        string[] lines;
        try
        {
            using var stream = new FileStream(log, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete);
            using var reader = new StreamReader(stream);
            lines = reader.ReadToEnd().Split('\n');
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException) { lines = []; }
        var time = File.GetLastWriteTime(log);
        // The layer writes once a second while DCS renders in VR.
        var live = dcsRunning && DateTime.Now - time < TimeSpan.FromSeconds(10);
        PupilShiftSecond? last = null;
        PupilRange? gazeX = null, gazeY = null;
        double maxShift = 0;
        int seconds = 0, withGaze = 0;
        int? restored = null;
        string? hotkey = null;
        foreach (var line in lines)
        {
            if (Toggle.Match(line) is { Success: true } t) hotkey = t.Groups[1].Value;
            if (EndFrame.Match(line) is { Success: true } e) restored = Math.Max(restored ?? 0, Int(e.Groups[1].Value));
            if (Stats.Match(line) is not { Success: true } m) continue;
            var second = new PupilShiftSecond(Int(m.Groups[1].Value), Int(m.Groups[2].Value), m.Groups[3].Success ? Int(m.Groups[3].Value) : null,
                m.Groups[4].Success ? new PupilRange(Num(m.Groups[4].Value), Num(m.Groups[5].Value)) : null, m.Groups[6].Success ? new PupilRange(Num(m.Groups[6].Value), Num(m.Groups[7].Value)) : null,
                m.Groups[8].Success ? Num(m.Groups[8].Value) : 0, new PupilRange(Num(m.Groups[9].Value), Num(m.Groups[10].Value)), m.Groups["off"].Success);
            last = second; seconds++;
            if (second.Shifted == 0 || second.GazeX is not { } x || second.GazeY is not { } y) continue;
            withGaze++;
            gazeX = gazeX is { } gx ? new PupilRange(Math.Min(gx.Min, x.Min), Math.Max(gx.Max, x.Max)) : x;
            gazeY = gazeY is { } gy ? new PupilRange(Math.Min(gy.Min, y.Min), Math.Max(gy.Max, y.Max)) : y;
            maxShift = Math.Max(maxShift, second.MaxShiftMm);
            if (second.Restored is { } r) restored = Math.Max(restored ?? 0, r);
        }
        var warnings = new List<string>();
        var span = gazeX is { } sx ? sx.Max - sx.Min : 0;
        if (withGaze >= 30 && span < 5)
            warnings.Add("The gaze barely moved during the flight (less than 5 degrees). The focus area may not follow your eyes: check eye tracking in Pimax Play.");
        var runtimeEyeMoves = last is { } l && l.RuntimeEyeX.Max - l.RuntimeEyeX.Min > 0.5;
        if (runtimeEyeMoves)
            warnings.Add("The runtime moves the eye position itself with the gaze; the correction would be applied twice. Turn Pupil shift off and report it.");
        var (state, summary) = last switch
        {
            null => ("waiting", "Loaded by DCS; no statistics yet."),
            // After the flight, the whole flight counts: its last second (leaving VR, the switch pressed) says little.
            _ when !live && withGaze > 0 && restored != 0 => ("working", string.Create(CultureInfo.InvariantCulture, $"Worked: each eye followed its pupil for {withGaze} of {seconds} seconds, up to {maxShift:0.0} mm.")),
            { CorrectionOff: true } => ("off", "Loaded, switched off in flight."),
            { Shifted: 0 } => ("no-gaze", "Loaded, but no gaze arrives: the profile needs eye-tracked Quad Views, and the headset's eye tracking must be on."),
            _ when restored == 0 => ("not-restored", "Shifting, but no projection view was handed back to the runtime unchanged: report it."),
            _ => ("working", string.Create(CultureInfo.InvariantCulture, $"Working: each eye follows its pupil, up to {maxShift:0.0} mm this flight.")),
        };
        return new(state, (live ? "" : "Last flight · ") + summary, live, log, time, last, gazeX, gazeY, maxShift, withGaze, seconds, restored, hotkey, warnings);
    }

    private static int Int(string s) => int.Parse(s, NumberStyles.Integer, CultureInfo.InvariantCulture);
    private static double Num(string s) => double.Parse(s, NumberStyles.Float, CultureInfo.InvariantCulture);
}
