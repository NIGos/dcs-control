using System.Globalization;
using System.Text.Json;
using System.Text.RegularExpressions;

namespace DcsVr.Core;

/// <summary>Pimax Play's per-side focus values for one eye (runtime_quadviews_fine_fov_*). This app sizes the focus at the
/// percentage Pimax Play shows: Quick 25% is a focus 25% of the view, Fine Left + Right → Center 33% + 10% is 43%.</summary>
public sealed record PimaxEyeFov(double Left, double Right, double Up, double Down)
{
    public double Width => (Left + Right) / 2;
    public double Height => (Up + Down) / 2;
}

/// <summary>Bundled Quad Views values converted from Pimax Play's settings.</summary>
/// <param name="Transition">Pimax blends the focus edge (Transition Mode Linear or Alpha, not Off).</param>
/// <param name="RequestedWidth">Width before the bundled provider's 0.9 cap.</param>
public sealed record PimaxConversion(double Width, double Height, double FocusScale, double PeripheralScale, bool Transition,
    double RequestedWidth, double RequestedHeight, bool Clamped, bool Asymmetric);

/// <summary>One control of Pimax Play's Quad View page: Pimax Play's own label and the value exactly as it displays it.</summary>
public sealed record PimaxControl(string Label, string Value);

/// <summary>
/// Pimax Play's Quad View page as Pimax Play itself shows it. The rules are Pimax Play's own (pimaxui app.asar,
/// quadviewFov store): Center Resolution = round(50 + gaze × 250) (0.28 → 120); Peripheral Resolution = round(1 + periphery × 99)
/// (0.1414 → 15); Quick FOV = round(stored × 100); Fine "→ Center" = floor(stored × 50) (0.66 → 33, 0.20 → 10; 0.99 → 50);
/// Vertical Offset = round(−100 + (stored + 0.5) × 200); Transition Range and Alpha = round(stored × 100);
/// Transition Mode −1 Off, 1 Alpha, anything else Linear. Missing values show Pimax Play's defaults.
/// </summary>
public sealed record PimaxPlayView(string Mode, int CenterResolution, int PeripheralResolution, int HorizontalFov, int VerticalFov, int VerticalOffset,
    int Top, int Bottom, int LeftEyeLeft, int LeftEyeRight, int RightEyeLeft, int RightEyeRight,
    string TransitionMode, int TransitionRange, string TransitionCurve, int Alpha)
{
    public bool Fine => Mode == "Fine";

    /// <summary>The controls in Pimax Play's order for the selected mode. Like Pimax Play, Transition Range and the
    /// curve or alpha slider appear only in Fine mode with the transition on.</summary>
    public IReadOnlyList<PimaxControl> Controls
    {
        get
        {
            static string P(int v) => v.ToString(CultureInfo.InvariantCulture) + "%";
            var list = new List<PimaxControl> { new("Center Resolution", P(CenterResolution)), new("Peripheral Resolution", P(PeripheralResolution)) };
            if (Fine)
                list.AddRange([new("Top → Center", P(Top)), new("Bottom → Center", P(Bottom)), new("L-Eye Left → Center", P(LeftEyeLeft)),
                    new("L-Eye Right → Center", P(LeftEyeRight)), new("R-Eye Left → Center", P(RightEyeLeft)), new("R-Eye Right → Center", P(RightEyeRight))]);
            else
                list.AddRange([new("Horizontal FOV", P(HorizontalFov)), new("Vertical FOV", P(VerticalFov)), new("Vertical Offset", VerticalOffset.ToString(CultureInfo.InvariantCulture))]);
            list.Add(new("Transition Mode", TransitionMode));
            if (Fine && TransitionMode != "Off")
            {
                list.Add(new("Transition Range", P(TransitionRange)));
                list.Add(TransitionMode == "Linear" ? new("Transition Curve", TransitionCurve) : new("Alpha", P(Alpha)));
            }
            return list;
        }
    }

    /// <summary>"Quick 33% × 33% · 125% · 20%" or "Fine 33/10 · 33/33 · 125% · 20%" (L-eye left/right · top/bottom).</summary>
    public string Short => Fine
        ? FormattableString.Invariant($"Fine {LeftEyeLeft}/{LeftEyeRight}{(RightEyeLeft == LeftEyeRight && RightEyeRight == LeftEyeLeft ? "" : $" (R {RightEyeLeft}/{RightEyeRight})")} · {Top}/{Bottom} · {CenterResolution}% · {PeripheralResolution}%")
        : FormattableString.Invariant($"Quick {HorizontalFov}% × {VerticalFov}% · {CenterResolution}% · {PeripheralResolution}%");
}

/// <summary>Pimax Play's Quad View settings as read from %APPDATA%\Pimax\AppConfig\global.json. Never written.</summary>
/// <param name="FineType">piplay_quadviews_fine_type: 0 Quick, 1 Fine (the tab selected in Pimax Play).</param>
/// <param name="Play">The page as Pimax Play shows it: sliders from the piplay_* keys, shared values from runtime_*.</param>
/// <param name="Mismatch">Set when Pimax Play's sliders and the runtime_* values its runtime uses disagree.</param>
public sealed record PimaxQuadViewsSettings(
    string Path, DateTimeOffset Modified, int? FineType, double? QuickHorizontal, double? QuickVertical,
    PimaxEyeFov LeftEye, PimaxEyeFov RightEye, bool FromQuickSliders, double GazeResolutionScale, double PeripheryResolutionScale,
    int TransitionType, double? BlendArea, PimaxConversion Converted, PimaxPlayView Play, string? Mismatch)
{
    public string Mode => FineType == 1 ? "Fine" : "Quick";
    /// <summary>Changes whenever a value that reaches the bundled provider changes.</summary>
    public string Stamp => string.Join('|', new[] { Converted.Width, Converted.Height, Converted.FocusScale, Converted.PeripheralScale, Converted.Transition ? 1 : 0 }
        .Select(v => v.ToString("G9", CultureInfo.InvariantCulture)));
}

/// <summary>The profile that will be written, after taking the focus area from Pimax Play when the profile asks for it.</summary>
/// <param name="Applies">The profile takes its focus area from Pimax Play (bundled Quad Views, source Pimax Play).</param>
public sealed record FoveaResolution(VrProfile Profile, bool Applies, PimaxQuadViewsSettings? Pimax, IReadOnlyList<string> Notes);

/// <summary>The Pimax Play values converted and written to bundled Quad Views at Apply (pimax-fovea.json in the profile
/// folder). profile.json keeps the user's own values; this keeps what DCS actually uses, for "changed since Apply".</summary>
public sealed record AppliedFovea(string? Stamp, double FoveaWidth, double FoveaHeight, double QuadFocusScale, double PeripheralScale, double QuadEdgeBlend)
{
    public const string FileName = "pimax-fovea.json";
    public static AppliedFovea From(VrProfile written, string? stamp) =>
        new(stamp, written.FoveaWidth, written.FoveaHeight, written.QuadFocusScale, written.PeripheralScale, written.QuadEdgeBlend);
}

/// <summary>
/// Pimax Play's units, with Pimax Play's own conversion and rounding, and the Quick-style units a profile's own focus
/// values are edited in. The focus is the percentage Pimax Play shows: Quick Horizontal FOV q is a focus q % of the view
/// wide; Fine "Left → Center" p and "Right → Center" p' add up to a focus p + p' % wide.
/// </summary>
public static class PimaxPlayUnits
{
    /// <summary>JavaScript's Math.round (half up), which Pimax Play uses.</summary>
    public static double JsRound(double value) => Math.Floor(value + .5);
    public static int CenterResolution(double gaze) => (int)JsRound(50 + Math.Clamp(gaze, 0, 1) * 250);
    public static int PeripheralResolution(double periphery) => (int)JsRound(1 + Math.Clamp(periphery, 0, 1) * 99);
    public static int QuickFov(double? stored) => (int)Math.Clamp(JsRound((stored is > 0 ? stored.Value : .33) * 100), 5, 90);
    /// <summary>Pimax Play floors stored × 50. The stored value is a float32 (0.58 is kept as 0.5799999833), so a 1e-4
    /// tolerance shows the value that was set rather than one less.</summary>
    public static int FineFov(double? stored, bool inner = false)
    {
        var raw = stored is > 0 ? stored.Value : inner ? .2 : .66;
        return raw >= .99 ? 50 : (int)Math.Clamp(Math.Floor(raw * 50 + 1e-4), 5, 50);
    }
    public static int VerticalOffset(double? stored) => stored is not { } raw ? -10 : (int)JsRound(-100 + (Math.Clamp(raw, -.5, .5) + .5) * 200);
    public static int TransitionRange(double? stored) => stored is > 0 ? (int)Math.Clamp(JsRound(stored.Value * 100), 1, 50) : 5;
    public static int Alpha(double? stored) => stored is not { } raw ? 50 : (int)Math.Clamp(JsRound(raw * 100), 5, 95);
    public static string TransitionMode(int? type) => type == -1 ? "Off" : type == 1 ? "Alpha" : "Linear";
    public static string TransitionCurve(int? mode) => mode is >= 0 and <= 3 ? new[] { "Linear", "Power2", "Sqrt", "Sigmoid" }[mode.Value] : "Linear";
    /// <summary>What Pimax Play writes to runtime_quadviews_fine_fov_* for a slider value.</summary>
    public static double QuickStored(int value) => Math.Round(value * .01, 4);
    public static double FineStored(int value) => value >= 50 ? .99 : Math.Round(value / 50.0, 4);

    // A profile's own focus values in Pimax Play's Quick units, converted exactly like Pimax Quick (stored to 4 decimals as Pimax does).
    public static double QuickFovFromShare(double share) => share * 100;
    public static double ShareFromQuickFov(double quickFov) => Math.Round(quickFov * .01, 4);
    // Pimax's runtime turns the stored gaze and periphery values into pixel densities (pvr_client log, Pimax Play 2026-10:
    // gaze 0.28 → m_focusPixelDensity 1.2; periphery 0.1414 → m_peripheralPixelDensity 0.148572, 0.1919 → 0.198).
    public static double FocusDensity(double gaze) => .5 + gaze * 2.5;
    public static double PeripheryDensity(double periphery) => .01 + periphery * .98;
    /// <summary>Center Resolution is the focus pixel density in %, so 120% is 1.2×.</summary>
    public static double CenterFromDensity(double density) => density * 100;
    public static double DensityFromCenter(double center) => Math.Round(center / 100, 4);
    public static double PeripheralPercent(double density) => 1 + (density - .01) / .98 * 99;
    public static double PeripheryFromPercent(double percent) => Math.Round(PeripheryDensity(Math.Round((percent - 1) / 99, 4)), 4);
    /// <summary>A profile's own focus values in Pimax Play's Quick units, e.g. "57% × 34% · 113% · 19%" for 0.57 × 0.34, 1.125, 0.19.</summary>
    public static string ProfileShort(VrProfile p) =>
        FormattableString.Invariant($"{JsRound(QuickFovFromShare(p.FoveaWidth))}% × {JsRound(QuickFovFromShare(p.FoveaHeight))}% · {JsRound(CenterFromDensity(p.QuadFocusScale))}% · {JsRound(PeripheralPercent(p.PeripheralScale))}%");
    /// <summary>Focus pixels per eye along one axis, rounded to an even count as Pimax's runtime does (matches its logged
    /// 4088×4038 and 3478×2048 at 5424×5356 × 1.125).</summary>
    public static int FocusPixels(int eyePixels, double share, double density) => (int)(JsRound(eyePixels * share * density / 2) * 2);
}

/// <summary>
/// Converts Pimax Play's Quad View settings to bundled Quad-Views-Foveated values. The focus is the percentage Pimax Play
/// shows (product rule): width (left + right) / 2 and height (up + down) / 2 of the stored values, so Quick 25 % is 0.25 × 0.25
/// and Fine 0.66/0.20 (33/10 → Center) is 0.43. Pimax's own runtime renders 1 − that instead ("fine fov left eye: L=0.75" for
/// Quick 25 %, 4882×4820 at 5424×5356 × 1.2).
/// The focus pixel density is 0.5 + gaze_resolution_scale × 2.5, i.e. Center Resolution / 100 (0.28 → logged
/// m_focusPixelDensity 1.2 with Pimax Play 2026-10); the periphery is 0.01 + periphery_resolution_scale × 0.98 (0.1414 → 0.148572).
/// </summary>
public static class PimaxFovea
{
    /// <summary>Pimax Play's settings file; tests and the offline verification point this at fixtures.</summary>
    public static string SettingsPath { get; set; } = System.IO.Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData), "Pimax", "AppConfig", "global.json");
    /// <summary>Quad-Views-Foveated caps each focus axis at 90 %.</summary>
    public const double ProviderCap = .9;
    public const string SameLabelNote = "The focus is the percentage shown in Pimax Play: Quick Horizontal FOV 25% is a focus 25% of the view wide; Fine Left → Center 33% and Right → Center 10% add up to 43%.";
    public const string Explanation = "The focus is the percentage shown in Pimax Play: Quick, Horizontal and Vertical FOV per axis; Fine, Left → Center + Right → Center (and Top + Bottom). Center Resolution 120% is a 1.2× pixel density, as in Pimax's runtime log, and Peripheral Resolution 15% is 0.149.";

    public static bool UsesPimaxPlay(VrProfile profile) =>
        profile.FoveaSource == FoveaSource.PimaxPlay && profile.QuadViews == QuadProvider.QuadViewsFoveated && string.IsNullOrWhiteSpace(profile.QuadViewsLayerDirectory);

    public static PimaxQuadViewsSettings? Read(string? path = null)
    {
        path ??= SettingsPath;
        try
        {
            if (!File.Exists(path)) return null;
            using var document = JsonDocument.Parse(File.ReadAllText(path));
            var root = document.RootElement;
            if (root.ValueKind != JsonValueKind.Object) return null;
            double? Read(string name) => root.TryGetProperty(name, out var value) && value.ValueKind == JsonValueKind.Number && value.TryGetDouble(out var number) && double.IsFinite(number) ? number : null;
            if (Read("runtime_quadviews_gaze_resolution_scale") is not { } gaze || Read("runtime_quadviews_periphery_resolution_scale") is not { } periphery) return null;
            double? quickH = Read("piplay_quadviews_quick_horizontal_gaze_fov_scale"), quickV = Read("piplay_quadviews_quick_vertical_gaze_fov_scale");
            // The runtime_* fine values are what the runtime uses in both modes; the Quick sliders only fill gaps.
            PimaxEyeFov? Eye(string eye)
            {
                var values = new[] { "left", "right", "up", "down" }.Select(side => Read($"runtime_quadviews_fine_fov_{eye}_{side}")).ToArray();
                if (values.All(v => v is not null)) return new(values[0]!.Value, values[1]!.Value, values[2]!.Value, values[3]!.Value);
                return quickH is { } h && quickV is { } v ? new(h, h, v, v) : null;
            }
            var fromQuick = Read("runtime_quadviews_fine_fov_left_left") is null;
            if (Eye("left") is not { } left || Eye("right") is not { } right) return null;
            var type = Read("piplay_quadviews_fine_type") is { } t ? (int)t : (int?)null;
            int? transitionType = Read("runtime_quadviews_focus_transition_type") is { } tt ? (int)tt : null;
            var play = new PimaxPlayView(type == 1 ? "Fine" : "Quick", PimaxPlayUnits.CenterResolution(gaze), PimaxPlayUnits.PeripheralResolution(periphery),
                PimaxPlayUnits.QuickFov(quickH), PimaxPlayUnits.QuickFov(quickV), PimaxPlayUnits.VerticalOffset(Read("runtime_quadviews_vertical_focus_offset")),
                PimaxPlayUnits.FineFov(Read("piplay_quadviews_fine_fov_top_to_center")), PimaxPlayUnits.FineFov(Read("piplay_quadviews_fine_fov_bottom_to_center")),
                PimaxPlayUnits.FineFov(Read("piplay_quadviews_fine_fov_left_left_to_center")), PimaxPlayUnits.FineFov(Read("piplay_quadviews_fine_fov_left_right_to_center"), inner: true),
                PimaxPlayUnits.FineFov(Read("piplay_quadviews_fine_fov_right_left_to_center"), inner: true), PimaxPlayUnits.FineFov(Read("piplay_quadviews_fine_fov_right_right_to_center")),
                PimaxPlayUnits.TransitionMode(transitionType), PimaxPlayUnits.TransitionRange(Read("runtime_quadviews_focus_blend_area")),
                PimaxPlayUnits.TransitionCurve(Read("runtime_quadviews_focus_blend_mode") is { } m ? (int)m : null), PimaxPlayUnits.Alpha(Read("runtime_quadviews_focus_transition_opacity_percent")));
            return new(Path.GetFullPath(path), File.GetLastWriteTimeUtc(path), type, quickH, quickV, left, right, fromQuick, gaze, periphery,
                transitionType ?? 0, Read("runtime_quadviews_focus_blend_area"), Convert(left, right, gaze, periphery, play.TransitionMode != "Off"), play,
                fromQuick ? null : Mismatch(play, left, right));
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException or JsonException or InvalidOperationException or ArgumentException) { return null; }
    }

    /// <summary>Compares Pimax Play's sliders for the selected mode with the runtime_* values Pimax's runtime renders with.</summary>
    private static string? Mismatch(PimaxPlayView play, PimaxEyeFov left, PimaxEyeFov right)
    {
        var differences = new List<string>();
        void Compare(string label, int shown, double expected, params double[] runtime)
        {
            if (runtime.All(r => Math.Abs(r - expected) < .006)) return;
            var used = runtime.Select(r => play.Fine ? PimaxPlayUnits.FineFov(r) : PimaxPlayUnits.QuickFov(r)).Distinct().Select(v => v + "%");
            differences.Add(FormattableString.Invariant($"{label} shows {shown}% but the runtime uses {string.Join(" / ", used)}"));
        }
        if (play.Fine)
        {
            Compare("Top → Center", play.Top, PimaxPlayUnits.FineStored(play.Top), left.Up, right.Up);
            Compare("Bottom → Center", play.Bottom, PimaxPlayUnits.FineStored(play.Bottom), left.Down, right.Down);
            Compare("L-Eye Left → Center", play.LeftEyeLeft, PimaxPlayUnits.FineStored(play.LeftEyeLeft), left.Left);
            Compare("L-Eye Right → Center", play.LeftEyeRight, PimaxPlayUnits.FineStored(play.LeftEyeRight), left.Right);
            Compare("R-Eye Left → Center", play.RightEyeLeft, PimaxPlayUnits.FineStored(play.RightEyeLeft), right.Left);
            Compare("R-Eye Right → Center", play.RightEyeRight, PimaxPlayUnits.FineStored(play.RightEyeRight), right.Right);
        }
        else
        {
            Compare("Horizontal FOV", play.HorizontalFov, PimaxPlayUnits.QuickStored(play.HorizontalFov), left.Left, left.Right, right.Left, right.Right);
            Compare("Vertical FOV", play.VerticalFov, PimaxPlayUnits.QuickStored(play.VerticalFov), left.Up, left.Down, right.Up, right.Down);
        }
        return differences.Count == 0 ? null
            : $"Pimax Play's {play.Mode} sliders and the values its runtime uses differ: {string.Join("; ", differences)}. Pimax's runtime renders with its own values, and so does bundled Quad Views. Open Pimax Play's Quad View page, or restart the Pimax service, to bring them back in line.";
    }

    public static PimaxConversion Convert(PimaxEyeFov left, PimaxEyeFov right, double gazeScale, double peripheryScale, bool transition)
    {
        // Both eyes normally mirror each other; bundled Quad Views uses one size for both.
        var width = (left.Width + right.Width) / 2; var height = (left.Height + right.Height) / 2;
        double Axis(double value) => Math.Clamp(value, .1, ProviderCap);
        var asymmetric = Math.Abs(left.Left - left.Right) > .005 || Math.Abs(right.Left - right.Right) > .005 || Math.Abs(left.Up - left.Down) > .005 || Math.Abs(right.Up - right.Down) > .005;
        return new(Axis(width), Axis(height), Math.Clamp(PimaxPlayUnits.FocusDensity(gazeScale), .5, 2), Math.Clamp(PimaxPlayUnits.PeripheryDensity(peripheryScale), .15, 1), transition,
            width, height, width > ProviderCap || height > ProviderCap, asymmetric);
    }

    /// <summary>The profile with Pimax Play's converted values when it takes its focus area from Pimax Play.</summary>
    public static FoveaResolution Resolve(VrProfile profile, string? path = null)
    {
        if (!UsesPimaxPlay(profile)) return new(profile, false, null, []);
        if (Read(path) is not { } pimax)
            return new(profile, true, null, ["Pimax Play's Quad View settings were not found or could not be read (" + (path ?? SettingsPath) + "). This profile's own focus values are used."]);
        var c = pimax.Converted;
        var effective = profile with { FoveaWidth = c.Width, FoveaHeight = c.Height, QuadFocusScale = c.FocusScale, PeripheralScale = c.PeripheralScale, QuadEdgeBlend = c.Transition ? profile.QuadEdgeBlend : 0 };
        return new(effective, true, pimax, Notes(pimax));
    }

    /// <summary>The 90 % cap in Pimax Play's units, or null when Pimax's focus fits.</summary>
    public static string? CapNote(PimaxQuadViewsSettings pimax)
    {
        var c = pimax.Converted; if (!c.Clamped) return null;
        // In Pimax Play's units the focus is Quick's FOV value, or Fine's Left + Right (Top + Bottom) → Center.
        var p = pimax.Play; var parts = new List<string>();
        int Shown(double share) => (int)PimaxPlayUnits.JsRound(share * 100);
        if (c.RequestedWidth > ProviderCap)
            parts.Add(p.Fine ? FormattableString.Invariant($"Left + Right → Center add up to {Shown(c.RequestedWidth)}%") : FormattableString.Invariant($"Horizontal FOV is {Shown(c.RequestedWidth)}%"));
        if (c.RequestedHeight > ProviderCap)
            parts.Add(p.Fine ? FormattableString.Invariant($"Top + Bottom → Center add up to {Shown(c.RequestedHeight)}%") : FormattableString.Invariant($"Vertical FOV is {Shown(c.RequestedHeight)}%"));
        return $"In Pimax Play {string.Join(" and ", parts)}. Bundled Quad Views renders a focus of at most 90% of the view, so it renders 90%.";
    }

    /// <summary>Plain facts about the Pimax values, for the interface, Preview and readiness.</summary>
    public static IReadOnlyList<string> Notes(PimaxQuadViewsSettings pimax)
    {
        var c = pimax.Converted; var p = pimax.Play; var notes = new List<string> { Summary(pimax) };
        if (pimax.Mismatch is { } mismatch) notes.Add(mismatch);
        if (CapNote(pimax) is { } cap) notes.Add(cap);
        if (c.Asymmetric)
            notes.Add(p.Fine
                ? FormattableString.Invariant($"Pimax places the focus off-centre (L-Eye Left → Center {p.LeftEyeLeft}%, L-Eye Right → Center {p.LeftEyeRight}%). Bundled Quad Views centres the focus on your gaze, with the same total size.")
                : "Pimax's runtime values place the focus off-centre. Bundled Quad Views centres the focus on your gaze, with the same total size.");
        if (pimax.FromQuickSliders) notes.Add("Pimax Play has no per-side values saved yet, so the Quick sliders are used.");
        if (!c.Transition) notes.Add("Pimax Play's Transition Mode is Off, so edge blending is 0.");
        var requestedFocus = PimaxPlayUnits.FocusDensity(pimax.GazeResolutionScale);
        if (Math.Abs(requestedFocus - c.FocusScale) > 1e-9)
            notes.Add(FormattableString.Invariant($"Pimax's stored Center Resolution (gaze scale {Number(pimax.GazeResolutionScale)}, a {Number(requestedFocus)}× pixel density) is outside what bundled Quad Views renders (0.5× to 2×), so it is clamped to {Number(c.FocusScale)}×."));
        var requestedPeriphery = PimaxPlayUnits.PeripheryDensity(pimax.PeripheryResolutionScale);
        if (Math.Abs(requestedPeriphery - c.PeripheralScale) > 1e-9)
            notes.Add(FormattableString.Invariant($"Peripheral Resolution {p.PeripheralResolution}% (a {Number(requestedPeriphery)}× pixel density) is below what bundled Quad Views renders, so it is clamped to 0.15× (Peripheral Resolution 16%)."));
        return notes;
    }

    /// <summary>Pimax Play's page in its own words, e.g. "Pimax Play · Quick: Center Resolution 125%, Peripheral Resolution 20%, …".</summary>
    public static string Summary(PimaxQuadViewsSettings pimax) =>
        $"Pimax Play · {pimax.Play.Mode}: {string.Join(", ", pimax.Play.Controls.Select(x => x.Label + " " + x.Value))}.";

    /// <summary>Message when Pimax Play's values now differ from those written at Apply; null when unchanged or not used.</summary>
    /// <param name="applied">The applied profile.json: the user's own draft (or, for profiles applied by earlier
    /// versions, the converted values themselves).</param>
    /// <param name="written">The converted values written at Apply (pimax-fovea.json next to profile.json); when absent the
    /// focus values of <paramref name="applied"/> are the written ones.</param>
    public static string? ChangedSinceApply(VrProfile applied, AppliedFovea? written = null, string? path = null)
    {
        var resolved = Resolve(applied, path);
        if (resolved.Pimax is null) return null;
        if (SameFovea(resolved.Profile, written ?? AppliedFovea.From(applied, null))) return null;
        return $"Pimax Play's Quad View settings changed since Apply (Pimax Play now: {resolved.Pimax.Play.Short}). Launch DCS updates the focus area to these values.";
    }

    /// <summary>The focus values of <paramref name="now"/> are those written at Apply (within float noise).</summary>
    public static bool SameFovea(VrProfile now, AppliedFovea written)
    {
        static bool Same(double a, double b) => Math.Abs(a - b) < 1e-6;
        return Same(now.FoveaWidth, written.FoveaWidth) && Same(now.FoveaHeight, written.FoveaHeight) && Same(now.QuadFocusScale, written.QuadFocusScale)
            && Same(now.PeripheralScale, written.PeripheralScale) && Same(now.QuadEdgeBlend, written.QuadEdgeBlend);
    }

    /// <summary>Reads pimax-fovea.json saved next to an applied profile.json; null when absent or unreadable.</summary>
    public static AppliedFovea? ReadApplied(string profileJsonPath)
    {
        try
        {
            var path = System.IO.Path.Combine(System.IO.Path.GetDirectoryName(profileJsonPath)!, AppliedFovea.FileName);
            return File.Exists(path) ? JsonData.Deserialize<AppliedFovea>(File.ReadAllText(path)) : null;
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException or JsonException or InvalidDataException or ArgumentException) { return null; }
    }

    /// <summary>The per-eye resolution the headset runtime recommended in DCS's last session (dcs.log, stereo views
    /// "View [0]: Recommended Width=5424 Height=5356"; the 4-view Quad Views block before it is skipped). Null when unknown.
    /// DCS writes these lines while the session starts, so only the first <see cref="EyeResolutionHead"/> bytes are read
    /// (shared, so a running DCS keeps writing), and the answer is cached by the file's length and time.</summary>
    public static (int Width, int Height)? ReadEyeResolution(string? dcsLog)
    {
        try
        {
            if (dcsLog is null) return null;
            var info = new FileInfo(dcsLog);
            if (!info.Exists) return null;
            var key = (info.FullName.ToUpperInvariant(), info.Length, info.LastWriteTimeUtc);
            lock (EyeCacheLock) if (_eyeCache is { } hit && hit.Key == key) return hit.Value;
            using var stream = new FileStream(dcsLog, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete);
            var buffer = new byte[(int)Math.Min(EyeResolutionHead, stream.Length)];
            stream.ReadExactly(buffer);
            var value = ParseEyeResolution(System.Text.Encoding.UTF8.GetString(buffer));
            lock (EyeCacheLock) _eyeCache = (key, value);
            return value;
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException or ArgumentException or NotSupportedException) { return null; }
    }
    public const int EyeResolutionHead = 256 * 1024;
    private static readonly Lock EyeCacheLock = new();
    private static ((string Path, long Length, DateTime Written) Key, (int Width, int Height)? Value)? _eyeCache;

    /// <summary>The last stereo view block (views 0 and 1 only) in dcs.log text; a cut-off last line never matches.</summary>
    public static (int Width, int Height)? ParseEyeResolution(string text)
    {
        var views = new List<(int View, int Width, int Height)>();
        (int, int)? found = null;
        void Close() { if (views.Count == 2 && views[0].View == 0 && views[1].View == 1) found = (views[0].Width, views[0].Height); views.Clear(); }
        foreach (var line in text.Split('\n'))
        {
            var match = ViewLine.Match(line);
            if (!match.Success || !int.TryParse(match.Groups[1].Value, NumberStyles.None, CultureInfo.InvariantCulture, out var view)
                || !int.TryParse(match.Groups[2].Value, NumberStyles.None, CultureInfo.InvariantCulture, out var width)
                || !int.TryParse(match.Groups[3].Value, NumberStyles.None, CultureInfo.InvariantCulture, out var height)) { if (views.Count > 0) Close(); continue; }
            if (view == 0 && views.Count > 0) Close();
            views.Add((view, width, height));
        }
        Close();
        return found;
    }
    private static readonly Regex ViewLine = new(@"OpenXR:\s+View \[(\d)\]: Recommended Width=(\d+) Height=(\d+)", RegexOptions.CultureInvariant);

    private static string Number(double value) => value.ToString("0.####", CultureInfo.InvariantCulture);
}
