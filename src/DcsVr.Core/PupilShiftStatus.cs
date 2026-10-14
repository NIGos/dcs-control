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
/// <param name="CantDeg">Angle between the two eyes' view axes (the headset's views turned outwards).</param>
/// <param name="RuntimeIpdMm">The eye distance the headset reports.</param>
/// <param name="RenderedIpdMm">The eye distance DCS renders with (DCS's custom IPD when the layer applies it).</param>
public sealed record PupilShiftSecond(int Frames, int Shifted, int? Restored, PupilRange? GazeX, PupilRange? GazeY, double MaxShiftMm, PupilRange RuntimeEyeX, bool CorrectionOff,
    double? CantDeg = null, double? RuntimeIpdMm = null, double? RenderedIpdMm = null);

/// <summary>One second of Depth boost (a "depth:" line of PupilShift.log).</summary>
/// <param name="On">Enabled and not switched off in flight.</param>
/// <param name="Source">Where the fixation depth came from in most frames: depth (the scene depth from the engine
/// module), vergence, none. Layers before the scene depth wrote the vergence's state here instead: probing, vergence,
/// constant.</param>
/// <param name="Fixation">near (cockpit), far or unknown.</param>
/// <param name="DistanceM">The estimated fixation distance, metres (999 = infinity, -1 = none).</param>
/// <param name="Scale">The render IPD scale at the end of the second.</param>
/// <param name="Boosted">Frames with a scale above 1.</param>
/// <param name="Vergence">probing (no proof yet that the gaze carries each eye's own direction), vergence, constant
/// (both eyes share one gaze: no vergence); null in older logs.</param>
/// <param name="Mode">realistic or enhanced; null in older logs.</param>
/// <param name="SceneFresh">Frames with a fresh scene-depth summary from the engine module.</param>
/// <param name="EdgeFrames">Frames with the gaze on an edge (the state held).</param>
/// <param name="Vetoes">Times the eyes' vergence (near) overruled a far scene depth.</param>
public sealed record DepthBoostSecond(bool On, string Source, string Fixation, double DistanceM, double Scale, double ScaleMax, double Target,
    int Frames, int Near, int Far, int NoGaze, int Boosted, double BaseIpdMm, double RenderIpdMm,
    string? Vergence = null, string? Mode = null, int SceneFresh = 0, int EdgeFrames = 0, int Vetoes = 0)
{
    /// <summary>The vergence's state, also from older logs (their Source).</summary>
    public string VergenceState => Vergence ?? (Source is "probing" or "vergence" or "constant" ? Source : "probing");
}

/// <summary>One second of Depth boost's predictive transition (a "predict:" line of PupilShift.log).</summary>
/// <param name="InEffect">Predictive in effect; false: selected, but gradual in effect (<paramref name="Reason"/>).</param>
/// <param name="Saccades">Saccades seen in the high-rate gaze.</param>
/// <param name="Predicted">Saccades whose landing was predicted.</param>
/// <param name="Steps">Steps made during a saccade; <paramref name="MeanStepMm"/> their mean size (eye distance).</param>
/// <param name="Confirmed">Steps the landing's scene depth confirmed; <paramref name="Contradicted"/>: reverted at the
/// normal rates; <paramref name="Expired"/>: neither in time.</param>
/// <param name="Short">Predictions without a step: too short a saccade, no depth grid, an edge (or mixed cells), too
/// little of the saccade left.</param>
public sealed record PredictiveSecond(bool InEffect, string? Reason, int Saccades, int Predicted, int Steps, double MeanStepMm, int Confirmed, int Contradicted,
    int Expired, int Short, int NoGrid, int Edge, int Late);

/// <summary>Depth boost's part of PupilShift.log.</summary>
/// <param name="State">disabled (not on in the running layer), waiting, working, probing, no-depth, no-scene (Realistic
/// without the scene depth: normal depth), no-gaze, off.</param>
/// <param name="Transition">gradual or predictive as selected (the settings line, then the in-flight switch); null in
/// older logs.</param>
/// <param name="Predictive">The last "predict:" second while predictive was selected.</param>
public sealed record DepthBoostStatus(string State, string Summary, DepthBoostSecond? Last, double SessionMaxScale, double SecondsBoosted,
    double SecondsNear, double SecondsFar, int Seconds, string? Hotkey, string? Transition = null, string? TransitionHotkey = null,
    PredictiveSecond? Predictive = null, int PredictiveSteps = 0, int PredictiveConfirmed = 0, int PredictiveContradicted = 0, int PredictiveSaccades = 0);

/// <summary>What the pupil shift layer reports in PupilShift.log, for the Pupil shift page.</summary>
/// <param name="State">not-installed, waiting (installed, DCS has not loaded it yet), working, no-gaze, off, not-restored,
/// disabled (Pupil shift is off; the layer runs for Depth boost only).</param>
/// <param name="Live">The log is being written now (DCS runs); otherwise it is the last flight's.</param>
/// <param name="Depth">Depth boost, when the layer's log has its settings line.</param>
public sealed record PupilShiftStatus(string State, string Summary, bool Live, string? LogPath, DateTime? LogTime, PupilShiftSecond? Last,
    PupilRange? SessionGazeX, PupilRange? SessionGazeY, double SessionMaxShiftMm, int SecondsWithGaze, int Seconds,
    int? RestoredViews, string? Hotkey, IReadOnlyList<string> Warnings, DepthBoostStatus? Depth = null)
{
    public const string FolderName = "pupilshift";
    public const string LogName = "PupilShift.log";
    public const string LayerDll = "XR_APILAYER_DCSVR_pupil_shift.dll";

    private static readonly Regex Stats = new(@"stats: frames=(\d+) shifted=(\d+)(?: restored=(\d+))?(?: gaze x\[(-?[\d.]+)\.\.(-?[\d.]+)\] y\[(-?[\d.]+)\.\.(-?[\d.]+)\] deg, max shift ([\d.]+) mm)?(?:(?<off> \(correction off\))|(?<disabled> \(pupil shift disabled\)))?.*?runtime left eye vs head x\[(-?[\d.]+)\.\.(-?[\d.]+)\](?:.*?; cant (?<cant>[\d.]+) deg, runtime IPD (?<ripd>[\d.]+) mm, rendered IPD (?<ipd>[\d.]+) mm)?", RegexOptions.CultureInvariant);
    private static readonly Regex EndFrame = new(@"end_frame: restored (\d+) projection views", RegexOptions.CultureInvariant);
    private static readonly Regex Toggle = new(@"settings: enabled=(\d) .*?toggle=(\d+:\d)", RegexOptions.CultureInvariant);
    private static readonly Regex DepthSettings = new(@"depth settings: enabled=(\d).*? key=(\d+:\d)(?: transition=(?<transition>\w+) transition_key=(?<tkey>\d+:\d))?", RegexOptions.CultureInvariant);
    private static readonly Regex TransitionKey = new(@"hotkey: transition (?<transition>gradual|predictive)", RegexOptions.CultureInvariant);
    private static readonly Regex PredictLine = new(@"predict: transition=predictive (?<effect>in effect|fallback gradual)(?:: (?<why>[^;]*?))? samples=", RegexOptions.CultureInvariant);
    private static readonly Regex Counter = new(@"(?<k>saccades|predicted|steps|confirmed|contradicted|expired|short|no_grid|edge|ambiguous|outside|late)=(?<v>\d+)|mean_step (?<mean>[\d.]+) mm", RegexOptions.CultureInvariant);
    private static readonly Regex DepthLine = new(@"depth: (?<on>on|off \(switch\)) source=(?<source>\w+)(?: vergence=(?<verg>\w+))? fixation=(?<fix>\w+) distance (?<d>-?[\d.]+) m \[[^\]]*\] scale (?<s>[\d.]+) \[(?<smin>[\d.]+)\.\.(?<smax>[\d.]+)\] target (?<t>[\d.]+); frames=(?<f>\d+) near=(?<n>\d+) far=(?<far>\d+) unknown=\d+ no_gaze=(?<ng>\d+) boosted=(?<b>\d+)[^;]*;(?: scene mode=(?<mode>\w+) fresh=(?<fresh>\d+)[^;]*? edge=(?<edge>\d+)[^;]*? vetoes=(?<veto>\d+)[^;]*;)?(?: pvr[^;]*;)? IPD base (?<base>[\d.]+) mm, render (?<render>[\d.]+) mm", RegexOptions.CultureInvariant);

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
        bool pupilEnabled = true;
        // Depth boost.
        string? depthKey = null, transition = null, transitionKey = null;
        PredictiveSecond? predictive = null;
        int predictiveSteps = 0, predictiveConfirmed = 0, predictiveContradicted = 0, predictiveSaccades = 0;
        bool depthEnabled = false, depthProven = false, sceneSeen = false;
        DepthBoostSecond? depthLast = null;
        double depthMax = 1, boostedSeconds = 0, nearSeconds = 0, farSeconds = 0;
        int depthSeconds = 0;
        foreach (var line in lines)
        {
            if (DepthSettings.Match(line) is { Success: true } ds)
            {
                depthEnabled = ds.Groups[1].Value == "1"; depthKey = ds.Groups[2].Value;
                if (ds.Groups["transition"].Success) { transition = ds.Groups["transition"].Value; transitionKey = ds.Groups["tkey"].Value; }
                continue;
            }
            if (TransitionKey.Match(line) is { Success: true } tk) { transition = tk.Groups["transition"].Value; continue; }
            if (PredictLine.Match(line) is { Success: true } pl)
            {
                var c = new Dictionary<string, int>();
                double mean = 0;
                foreach (Match cm in Counter.Matches(line))
                    if (cm.Groups["mean"].Success) mean = Num(cm.Groups["mean"].Value);
                    else c[cm.Groups["k"].Value] = c.GetValueOrDefault(cm.Groups["k"].Value) + Int(cm.Groups["v"].Value);
                int C(string k) => c.GetValueOrDefault(k);
                predictive = new(pl.Groups["effect"].Value == "in effect", pl.Groups["why"].Success ? pl.Groups["why"].Value.Trim() : null, C("saccades"), C("predicted"),
                    C("steps"), mean, C("confirmed"), C("contradicted"), C("expired"), C("short"), C("no_grid"), C("edge") + C("ambiguous") + C("outside"), C("late"));
                predictiveSteps += predictive.Steps; predictiveConfirmed += predictive.Confirmed; predictiveContradicted += predictive.Contradicted; predictiveSaccades += predictive.Saccades;
                continue;
            }
            if (Toggle.Match(line) is { Success: true } t) { pupilEnabled = t.Groups[1].Value != "0"; hotkey = t.Groups[2].Value; }
            if (DepthLine.Match(line) is { Success: true } d)
            {
                var depthSecond = new DepthBoostSecond(d.Groups["on"].Value == "on", d.Groups["source"].Value, d.Groups["fix"].Value, Num(d.Groups["d"].Value), Num(d.Groups["s"].Value),
                    Num(d.Groups["smax"].Value), Num(d.Groups["t"].Value), Int(d.Groups["f"].Value), Int(d.Groups["n"].Value), Int(d.Groups["far"].Value),
                    Int(d.Groups["ng"].Value), Int(d.Groups["b"].Value), Num(d.Groups["base"].Value), Num(d.Groups["render"].Value),
                    d.Groups["verg"].Success ? d.Groups["verg"].Value : null, d.Groups["mode"].Success ? d.Groups["mode"].Value : null,
                    d.Groups["fresh"].Success ? Int(d.Groups["fresh"].Value) : 0, d.Groups["edge"].Success ? Int(d.Groups["edge"].Value) : 0,
                    d.Groups["veto"].Success ? Int(d.Groups["veto"].Value) : 0);
                depthLast = depthSecond; depthSeconds++;
                depthMax = Math.Max(depthMax, depthSecond.ScaleMax);
                if (depthSecond.Frames > 0)
                {
                    boostedSeconds += (double)depthSecond.Boosted / depthSecond.Frames;
                    nearSeconds += (double)depthSecond.Near / depthSecond.Frames;
                    farSeconds += (double)depthSecond.Far / depthSecond.Frames;
                }
                depthProven |= depthSecond.Source is "vergence" or "depth" || depthSecond.VergenceState == "vergence";
                sceneSeen |= depthSecond.SceneFresh > 0;
                continue;
            }
            if (EndFrame.Match(line) is { Success: true } e) restored = Math.Max(restored ?? 0, Int(e.Groups[1].Value));
            if (Stats.Match(line) is not { Success: true } m) continue;
            var second = new PupilShiftSecond(Int(m.Groups[1].Value), Int(m.Groups[2].Value), m.Groups[3].Success ? Int(m.Groups[3].Value) : null,
                m.Groups[4].Success ? new PupilRange(Num(m.Groups[4].Value), Num(m.Groups[5].Value)) : null, m.Groups[6].Success ? new PupilRange(Num(m.Groups[6].Value), Num(m.Groups[7].Value)) : null,
                m.Groups[8].Success ? Num(m.Groups[8].Value) : 0, new PupilRange(Num(m.Groups[9].Value), Num(m.Groups[10].Value)), m.Groups["off"].Success,
                m.Groups["cant"].Success ? Num(m.Groups["cant"].Value) : null, m.Groups["ripd"].Success ? Num(m.Groups["ripd"].Value) : null, m.Groups["ipd"].Success ? Num(m.Groups["ipd"].Value) : null);
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
            _ when !pupilEnabled => ("disabled", "Pupil shift is off in this profile; the layer runs for Depth boost."),
            null => ("waiting", "Loaded by DCS; no statistics yet."),
            // After the flight, the whole flight counts: its last second (leaving VR, the switch pressed) says little.
            _ when !live && withGaze > 0 && restored != 0 => ("working", string.Create(CultureInfo.InvariantCulture, $"Worked: each eye followed its pupil for {withGaze} of {seconds} seconds, up to {maxShift:0.0} mm.")),
            { CorrectionOff: true } => ("off", "Loaded, switched off in flight."),
            { Shifted: 0 } => ("no-gaze", "Loaded, but no gaze arrives: the profile needs eye-tracked Quad Views, and the headset's eye tracking must be on."),
            _ when restored == 0 => ("not-restored", "Shifting, but no projection view was handed back to the runtime unchanged: report it."),
            _ => ("working", string.Create(CultureInfo.InvariantCulture, $"Working: each eye follows its pupil, up to {maxShift:0.0} mm this flight.")),
        };
        var depth = depthEnabled || depthLast is not null ? ReadDepth(depthEnabled, depthLast, live, depthProven, sceneSeen, depthMax, boostedSeconds, nearSeconds, farSeconds, depthSeconds, depthKey) with
        {
            Transition = transition, TransitionHotkey = transitionKey, Predictive = transition == "predictive" ? predictive : null,
            PredictiveSteps = predictiveSteps, PredictiveConfirmed = predictiveConfirmed, PredictiveContradicted = predictiveContradicted, PredictiveSaccades = predictiveSaccades
        } : null;
        return new(state, (live ? "" : "Last flight · ") + summary, live, log, time, last, gazeX, gazeY, maxShift, withGaze, seconds, restored, hotkey, warnings, depth);
    }

    private static DepthBoostStatus ReadDepth(bool enabled, DepthBoostSecond? last, bool live, bool proven, bool sceneSeen, double max, double boosted, double near, double far, int seconds, string? hotkey)
    {
        // The source: the scene depth (engine module), the vergence, or neither ("none" in a layer with the scene depth;
        // older layers wrote the vergence's state as the source).
        var sceneLess = last is { Source: not "depth" } && !sceneSeen;
        var (state, summary) = last switch
        {
            _ when !enabled => ("disabled", "Not on in the running layer. Launch DCS installs it with a profile that uses it."),
            null => ("waiting", "Loaded; no statistics yet."),
            // After the flight, the whole flight counts.
            _ when !live && proven && max > 1.0005 => ("working", string.Create(CultureInfo.InvariantCulture, $"Worked: up to ×{max:0.00}, boosted for {boosted:0} of {seconds} seconds.")),
            { On: false } => ("off", "Loaded, switched off in flight."),
            { Frames: > 0 } l when l.NoGaze == l.Frames => ("no-gaze", "No gaze arrives: the headset's eye tracking must be on. The scale stays normal."),
            { Mode: "realistic" } when sceneLess => ("no-scene", "Realistic needs the distance you look at from the scene, which Engine Optimizations measure. With the eyes' vergence alone it keeps normal depth."),
            { Source: "constant" or "none" } l when l.VergenceState == "constant" => ("no-depth", "Both eyes' focus areas follow one shared gaze and no scene depth arrives, so the distance you look at cannot be measured. The scale stays normal."),
            { Source: "probing" or "none" } => ("probing", "Waiting to see you look at the cockpit and outside once; until then the scale stays normal."),
            { Source: "depth" } => ("working", string.Create(CultureInfo.InvariantCulture, $"Working from the scene depth: ×{last.Scale:0.00} now ({last.Fixation}), up to ×{max:0.00} this flight.")),
            _ => ("working", string.Create(CultureInfo.InvariantCulture, $"Working: ×{last.Scale:0.00} now ({last.Fixation}), up to ×{max:0.00} this flight.")),
        };
        return new(state, (live ? "" : "Last flight · ") + summary, last, max, boosted, near, far, seconds, hotkey);
    }

    // Log text is never trusted: an unreadable number counts as 0.
    private static int Int(string s) => int.TryParse(s, NumberStyles.Integer, CultureInfo.InvariantCulture, out var n) ? n : 0;
    private static double Num(string s) => double.TryParse(s, NumberStyles.Float, CultureInfo.InvariantCulture, out var n) ? n : 0;
}
