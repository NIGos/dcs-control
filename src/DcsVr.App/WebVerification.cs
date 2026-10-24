using System.IO;
using System.Net.Http;
using System.Text.Json;
using System.Windows;
using System.Windows.Interop;
using DcsVr.Core;
using Microsoft.Web.WebView2.Core;

namespace DcsVr.App;

/// <summary>Real WebView2 Chromium + .NET bridge, hosted in an HWND that is never shown.</summary>
internal static class WebVerification
{
    /// <summary>Logs of one flight in the formats DCS Control reads (see LastFlight): 2× most of the time, 3× for a
    /// stretch, DLSS 5 on, CPU Boost restored with one service it could not move, the engine optimizations' prefetch fix
    /// skipping repeats (in <paramref name="savedGames"/>\Scripts\DcsQvCull).</summary>
    private static FlightSources WriteFlightFixture(string root, string savedGames, DateTimeOffset start, DateTimeOffset end)
    {
        var invariant = System.Globalization.CultureInfo.InvariantCulture;
        string Local(DateTimeOffset t, string format) => t.ToLocalTime().ToString(format, invariant);
        var pimax = Directory.CreateDirectory(Path.Combine(root, "pimax-runtime")).FullName;
        File.WriteAllLines(Path.Combine(pimax, "pvr_srv_log_" + Local(start.AddMinutes(-6), "yy-MM-dd-HH-mm-ss") + ".txt"), Enumerable.Range(0, 2400).Select(i =>
        {
            var t = start.AddSeconds(30 + i); var slow = i % 9 == 0; var loading = i < 20;
            var a = loading ? 0.04 : slow ? 79.84 : 89.97; var missed = loading ? 87 : slow ? 9 : 0;
            return $"[{Local(t, "yy-MM-dd HH:mm:ss.fff")}][26424][info][PSRV] 4242 {(missed > 0 ? "Warning" : "Normal")} rendering fps:(a:{a.ToString(invariant)},c:89.996) Missed:(a:{missed},c:0,bl:0) Stale:(a:0) Discard:(a:{(slow ? 3 : 0)}) GPU: 64% CPU: 71%";
        }));
        var ofxr = Directory.CreateDirectory(Path.Combine(root, "profiles", "pimax-qv-dlss5-fg-boost", "ofxr")).FullName;
        File.WriteAllLines(Path.Combine(ofxr, $"ofxr-bridge-flight-{Local(start.AddSeconds(40), "yyyyMMdd-HHmmss")}-pid4242.log"),
            ["seq=1 ms=0.000 tid=27164 phase=I op=logger result=401 dur_us=0 a=128 b=0 c=4242", "seq=14 ms=51876.732 tid=31660 phase=I op=adaptive_switch result=5 dur_us=0 a=0 b=2 c=0",
             .. Enumerable.Range(0, 2200).Select(i => $"seq={100 + i} ms={60000 + i * 1000}.000 tid=31660 phase=I op=latency_status result={(i is >= 900 and < 1300 ? 13 : 12)} dur_us=0 a={(i is >= 900 and < 1300 ? 24800 + i % 7 * 300 : 15200 + i % 11 * 250)} b=90 c=11111"),
             "seq=9000 ms=960000.000 tid=31660 phase=I op=adaptive_switch result=2 dur_us=0 a=2 b=3 c=24581", "seq=9400 ms=1360000.000 tid=31660 phase=I op=adaptive_switch result=1 dur_us=0 a=3 b=2 c=16752"]);
        var boost = Path.Combine(Directory.CreateDirectory(Path.Combine(root, "boost")).FullName, "status.json");
        File.WriteAllText(boost, JsonData.Serialize(new { schemaVersion = 1, state = "restored", updatedAt = end.AddSeconds(5), dcsPid = 4242,
            changed = new[] { "DCS (PID 4242): priority AboveNormal", "DCS (PID 4242): affinity all CPUs 0xFFFF", "PimaxClient (PID 31476): Normal, affinity 0xFC03", "pi_server (PID 8736): High, affinity 0xFC03", "steamwebhelper (PID 32368): BelowNormal, affinity 0xC000" },
            closed = Array.Empty<string>(), restored = 3, errors = new[] { "platform_runtime_VR4PIMAXP3B_service (PID 32536): Access is denied. (it may need the administrator rights option)" } }));
        var bin = Directory.CreateDirectory(Path.Combine(root, "bin")).FullName;
        // The engine optimizations module's prefetch fix, in Saved Games\DCS\Scripts\DcsQvCull next to the Logs folder.
        var module = Directory.CreateDirectory(Path.Combine(savedGames, "Scripts", "DcsQvCull")).FullName;
        File.WriteAllLines(Path.Combine(module, "DcsQvCullPrefetch.log"), [Local(start.AddSeconds(4), "yyyy-MM-dd HH:mm:ss") + " prefetch fix started mode=skip window_ms=1000",
            .. Enumerable.Range(1, 250).Select(i => Local(start.AddSeconds(4 + i * 10), "yyyy-MM-dd HH:mm:ss") + $" calls/s={(i < 20 ? 0 : 150000 + i % 13 * 9000)} skipped={(i < 20 ? "0.0" : "99.4")}% pointers=1 imports=41 lookups=0 faults=0")]);
        var cheeky = Directory.CreateDirectory(Path.Combine(bin, "CheekyFoveatedDLSS")).FullName;
        var standalone = Path.Combine(cheeky, "CheekyFoveatedDLSS-Standalone.log"); var host = Path.Combine(cheeky, "CheekyFoveatedDLSS-Host.log");
        // Cheeky appends every session to the same logs, with the time of day only: an earlier session that failed and
        // toggled DLSS 5 off comes first and must not count.
        File.WriteAllLines(standalone, ["11:02:11.100 [T1200] Cheeky 0.5.3 Standalone initializing; runtime remains resident until game exit",
            "11:02:40.836 [T1200] DLSS-NR feature creation failed error=-1160773628",
            "14:31:24.334 [T26404] Cheeky 0.5.3 Standalone initializing; runtime remains resident until game exit",
            "14:35:40.836 [T31660] DLSS-NR runtime search path=bin\\CheekyFoveatedDLSS\\nvngx_dlssnr.dll error=0", "14:35:40.836 [T31660] DLSS-NR 310.8 feature-18 runtime initialized"]);
        File.WriteAllLines(host, ["11:02:11.000 Initializing standalone host", "11:20:00.000 DLSS-NR hotkey toggled enabled=no",
            "14:31:24.332 Initializing standalone host", "14:31:25.729 Host ready; waiting for game graphics. F8 opens settings.", "14:52:56.971 DLSS-NR hotkey toggled enabled=no", "14:53:10.551 DLSS-NR hotkey toggled enabled=yes"]);
        foreach (var file in new[] { standalone, host }) File.SetLastWriteTimeUtc(file, end.AddSeconds(-30).UtcDateTime);
        return new(null, pimax, Path.Combine(root, "profiles"), boost, bin);
    }

    /// <summary>The documentation views (docs/screenshots) at 1320 × 920, 100 %: a new profile's default values with every
    /// feature checked on the Pimax route and Pimax Play running, then Launch DCS (offline: DCS is never started) for the
    /// applied Overview and Recovery, then Back to stock DCS. Written next to <paramref name="output"/> under the names
    /// the documentation uses.</summary>
    private static async Task DocumentationViews(CoreWebView2 core, CoreWebView2Controller controller, ControlService service, string output, string? neural, Action<string> log)
    {
        var folder = Path.GetDirectoryName(output)!;
        async Task Step(string script)
        {
            await core.ExecuteScriptAsync("window.docStep='pending';Promise.resolve().then(()=>"+script+").then(()=>window.docStep='done',e=>window.docStep='error:'+e.message)");
            for (var wait = 0; wait < 1000 && await core.ExecuteScriptAsync("window.docStep") == "\"pending\""; wait++) await Task.Delay(20);
            var result = JsonSerializer.Deserialize<string>(await core.ExecuteScriptAsync("window.docStep"));
            if (result != "done") throw new IOException("Documentation step failed: " + script + " → " + result);
        }
        async Task Shot(string name, string? page, string prepare = "")
        {
            if (page is not null) await core.ExecuteScriptAsync("window.dcsUi.navigate("+JsonSerializer.Serialize(page)+");");
            await core.ExecuteScriptAsync("document.getElementById('toast').hidden=true;" + prepare);
            await Task.Delay(250);
            var overflow = await core.ExecuteScriptAsync("document.documentElement.scrollWidth>innerWidth");
            if (overflow != "false") throw new IOException("Documentation view overflows: " + name);
            var path = Path.Combine(folder, name + ".png"); await using (var stream = File.Create(path)) await core.CapturePreviewAsync(CoreWebView2CapturePreviewImageFormat.Png, stream);
            log("Documentation view: " + path);
        }
        controller.Bounds = new System.Drawing.Rectangle(0, 0, 1320, 920); controller.RasterizationScale = 1;
        // Pimax Play running; a new profile's default values with every feature checked on the Pimax route.
        Readiness.ActiveRouteProvider = () => new(null, "Pimax Play is running and SteamVR is not. Use the Pimax route, or start SteamVR first for Sboys.", TrackingKind.Unknown);
        if (neural is not null) await Step("window.dcsUi.saveRuntime("+JsonSerializer.Serialize(neural)+")");
        await Step("(()=>{const ui=window.dcsUi;ui.adoptDraft({profile:ui.withFeatures("+JsonData.Serialize(new VrProfile())+",{route:'Pimax',features:{quad:true,dlss:true,framegen:true,boost:true}})});"
            + "return ui.run('refresh',ui.draft());})()");
        await Step("window.dcsUi.refreshBoostPlan ? window.dcsUi.refreshBoostPlan() : null");
        await Shot("overview-changes-apply", "overview");
        await Shot("quad-views-pimax-play", "foveation", "(b=>{if(b)b.scrollIntoView({block:'center'});})(document.getElementById('pimaxFovea'));");
        await Shot("dlss5-runtime-file", "dlss");
        await Shot("framegen-auto-2x-3x", "framegen");
        await Shot("cpu-boost-plan", "boost");
        await Step("window.dcsUi.run('checkReadiness')");
        await Shot("checks", "diagnostics");
        await Shot("framegen-hotkey-capture", "osd", "(()=>{const key=document.getElementById('input-diagnosticOverlayKey');key.click();window.dispatchEvent(new KeyboardEvent('keydown',{keyCode:17,ctrlKey:true,shiftKey:true,bubbles:true,cancelable:true}));document.getElementById('setting-diagnosticOverlayKey').scrollIntoView({block:'center'});})()");
        await core.ExecuteScriptAsync("window.dispatchEvent(new KeyboardEvent('keydown',{keyCode:27,bubbles:true,cancelable:true}));");
        await core.ExecuteScriptAsync("window.setupWizard.open(); window.setupWizard.choose({route:'Pimax',features:{quad:true,dlss:true,framegen:true,boost:true}});");
        await Shot("guided-setup-choose", null);
        await Step("window.setupWizard.next()");
        await Shot("guided-setup-check", null);
        await core.ExecuteScriptAsync("document.getElementById('setupClose').click()");
        // Launch DCS (the offline host stops after the launch contract check), then the applied state.
        await Step("window.dcsUi.run('launch')");
        await Step("window.dcsUi.run('refresh',window.dcsUi.draft())");
        await Shot("overview-last-flight", "overview");
        await Shot("recovery-original-files", "recovery", "(f=>{if(f)f.open=true;})(document.getElementById('originalFiles'));");
        await Step("window.dcsUi.run('restore')");
        if (service.Originals.Status() is not { Count: 0, Current: null }) throw new IOException("The documentation fixture was not restored.");
    }

    /// <summary>The feature video's stills: the neutral demo layout at 1920 × 1080 (1600 × 900 at 120 %), every feature
    /// on (Depth boost and Far depth included), each page as a pilot sees it, then Launch DCS and Back to stock DCS.</summary>
    private static async Task VideoViews(CoreWebView2 core, CoreWebView2Controller controller, ControlService service, string output, string? neural, Action<string> log)
    {
        var folder = Path.GetDirectoryName(output)!;
        async Task Step(string script)
        {
            await core.ExecuteScriptAsync("window.docStep='pending';Promise.resolve().then(()=>"+script+").then(()=>window.docStep='done',e=>window.docStep='error:'+e.message)");
            for (var wait = 0; wait < 1000 && await core.ExecuteScriptAsync("window.docStep") == "\"pending\""; wait++) await Task.Delay(20);
            var result = JsonSerializer.Deserialize<string>(await core.ExecuteScriptAsync("window.docStep"));
            if (result != "done") throw new IOException("Video step failed: " + script + " → " + result);
        }
        async Task Shot(string name, string? page, string prepare = "")
        {
            if (page is not null) await core.ExecuteScriptAsync("window.dcsUi.navigate("+JsonSerializer.Serialize(page)+");");
            await core.ExecuteScriptAsync("document.getElementById('toast').hidden=true;document.getElementById('pages')?.scrollTo(0,0);" + prepare);
            await Task.Delay(300);
            var path = Path.Combine(folder, name + ".png"); await using (var stream = File.Create(path)) await core.CapturePreviewAsync(CoreWebView2CapturePreviewImageFormat.Png, stream);
            log("Video view: " + path);
        }
        const string Edit = "const edit=(path,value)=>{const f=window.dcsUi.fields.find(x=>x.path===path);if(f.type==='toggle')f.input.checked=value;else f.input.value=value;f.input.dispatchEvent(new Event(f.type==='toggle'||f.type==='select'?'change':'input',{bubbles:true}));};";
        controller.Bounds = new System.Drawing.Rectangle(0, 0, 1600, 900);
        var scaled = controller as CoreWebView2Controller; scaled.RasterizationScale = 1.2; scaled.ShouldDetectMonitorScaleChanges = false;
        Readiness.ActiveRouteProvider = () => new(null, "Pimax Play is running and SteamVR is not. Use the Pimax route, or start SteamVR first for Sboys.", TrackingKind.Unknown);
        if (neural is not null) await Step("window.dcsUi.saveRuntime("+JsonSerializer.Serialize(neural)+")");
        // Stock first: a new profile with nothing on, as at the first start.
        await Step("(()=>{const ui=window.dcsUi;ui.adoptDraft({profile:ui.withFeatures("+JsonData.Serialize(new VrProfile())+",{route:'Pimax',features:{}})});return ui.run('refresh',ui.draft());})()");
        await Shot("v-overview-stock", "overview");
        // The features ticked one at a time in the right panel, for the video's click-through.
        var steps = new[] { "engine:true", "engine:true,boost:true", "engine:true,boost:true,quad:true", "engine:true,boost:true,quad:true,dlss:true", "engine:true,boost:true,quad:true,dlss:true,framegen:true" };
        for (var step = 0; step < steps.Length; step++)
        {
            await Step("(()=>{const ui=window.dcsUi;ui.adoptDraft({profile:ui.withFeatures("+JsonData.Serialize(new VrProfile())+",{route:'Pimax',features:{"+steps[step]+"}})});return ui.run('refresh',ui.draft());})()");
            await Shot($"v-step{step + 1}", "overview");
        }
        // Depth boost off, then on, on the Perception page.
        await Shot("v-depth-off", "pupil", "(c=>{if(c)c.scrollIntoView({block:'start'});})(document.getElementById('depthControls'));");
        // Then every feature, Depth boost and Far depth included.
        await Step("(()=>{const ui=window.dcsUi;ui.adoptDraft({profile:ui.withFeatures("+JsonData.Serialize(new VrProfile())+",{route:'Pimax',features:{engine:true,boost:true,quad:true,dlss:true,framegen:true}})});return ui.run('refresh',ui.draft());})()");
        await Step("(()=>{"+Edit+"edit('depthBoost',true);edit('farDepth',true);edit('peripheryStability',true);})()");
        await Step("window.dcsUi.refreshBoostPlan ? window.dcsUi.refreshBoostPlan() : null");
        await Shot("v-overview", "overview");
        await Shot("v-guide", "guide");
        await Shot("v-guide-perception", "guide", "(s=>{if(s)s.scrollIntoView({block:'start'});})([...document.querySelectorAll('.guide-section')][2]);");
        await Shot("v-engine", "engine");
        await Shot("v-boost", "boost");
        await Shot("v-foveation", "foveation");
        await Shot("v-framegen", "framegen");
        await Shot("v-dlss", "dlss");
        await Shot("v-perception", "pupil");
        await Shot("v-perception-depth", "pupil", "(c=>{if(c)c.scrollIntoView({block:'start'});})(document.getElementById('depthControls'));");
        await Shot("v-osd", "osd");
        await Step("window.dcsUi.run('checkReadiness')");
        await Shot("v-checks", "diagnostics");
        await Step("window.dcsUi.run('launch')");
        await Step("window.dcsUi.run('refresh',window.dcsUi.draft())");
        await Shot("v-overview-applied", "overview");
        await Shot("v-recovery", "recovery", "(f=>{if(f)f.open=true;})(document.getElementById('originalFiles'));");
        await Step("window.dcsUi.run('restore')");
        await Shot("v-recovery-restored", "recovery");
        if (service.Originals.Status() is not { Count: 0, Current: null }) throw new IOException("The video fixture was not restored.");
    }

    /// <summary>GitHub's release list as the update check sees it, served without network: a newer pre-release (this
    /// version's patch + 1) with its setup and signature, a higher draft and a malformed tag that must be ignored, an older
    /// release. The setup is signed with a throwaway key that replaces the release key for this run only, and its first
    /// download arrives altered (the signature must refuse it). The release notes carry markup that must stay text. This
    /// version's release notes come from a fixture RELEASE_NOTES.md, and the settings record an earlier version, so What's
    /// new opens at start like after an update.</summary>
    private static (Updates Updates, string NotesPath, string Version, UpdateFixtureHandler Handler) UpdateFixture(string root, string stateRoot)
    {
        var current = ReleaseVersion.Parse(ProductInfo.Version) ?? throw new InvalidOperationException("The product version is not a release version.");
        var next = (current with { Patch = current.Patch + 1 }).ToString();
        using var key = System.Security.Cryptography.ECDsa.Create(System.Security.Cryptography.ECCurve.NamedCurves.nistP256);
        var setup = System.Text.Encoding.UTF8.GetBytes("Offline update fixture. Never executed.");
        var signature = Convert.ToBase64String(key.SignData(setup, System.Security.Cryptography.HashAlgorithmName.SHA256, System.Security.Cryptography.DSASignatureFormat.IeeeP1363FixedFieldConcatenation));
        var name = $"DcsControl-{next}-Setup.exe"; var download = $"https://github.com/NIGos/dcs-control/releases/download/v{next}/";
        const string notes = "**Hostile notes check** <img src=x onerror=\"window.notesInjected=true\"> <script>window.notesInjected=true</script>\n\n- First bullet with `code`\n  - Nested bullet\n- Second bullet\n\nA [link](https://example.invalid/) in a paragraph.";
        object Release(string tag, bool draft, string body, object[] assets) => new { tag_name = tag, draft, prerelease = true, html_url = Updates.ReleasesPage + "/tag/" + tag, body, published_at = "2026-10-11T12:00:00Z", assets };
        var releases = JsonSerializer.Serialize(new[] {
            Release("v99.0.0", true, "A draft is never offered.", []), Release("nightly", false, "Not a version.", []),
            Release("v" + next, false, notes, [new { name, browser_download_url = download + name, size = setup.Length }, new { name = name + ".sig", browser_download_url = download + name + ".sig", size = signature.Length }]),
            Release("v0.4.0-preview", false, "Older.", []) });
        var handler = new UpdateFixtureHandler(new Dictionary<string, byte[]> { [Updates.ReleasesUrl] = System.Text.Encoding.UTF8.GetBytes(releases),
            [download + name] = setup, [download + name + ".sig"] = System.Text.Encoding.ASCII.GetBytes(signature) }, download + name);
        var notesPath = Path.Combine(Directory.CreateDirectory(Path.Combine(root, "release-notes")).FullName, "RELEASE_NOTES.md");
        File.WriteAllText(notesPath, $"# {current.Short}{(current.Suffix.Length > 0 ? " " + current.Suffix : "")}\n\n**Fixture notes** for this version, <b>not markup</b>.\n- One\n- Two\n\n# 0.4.0 preview\n\nOlder notes that must not show.\n");
        new AppSettingsStore(stateRoot).Save(new(true, "0.4.0-preview"));
        return (new Updates(stateRoot, handler, publicKeyPem: key.ExportSubjectPublicKeyInfoPem(), downloadRoot: Path.Combine(root, "update-downloads")), notesPath, next, handler);
    }

    public static async Task Run(string output)
    {
        output = Path.GetFullPath(output); Directory.CreateDirectory(Path.GetDirectoryName(output)!);
        // Documentation screenshots (DCSVR_SMOKE_DEMO_ROOT): the same fixture, laid out like a real PC under a neutral
        // folder (Steam library, Saved Games, AppData), so no path on screen names the machine or its user. The smoke
        // runs unchanged; then the documentation views are captured instead of the verification matrix.
        var demoRoot = Environment.GetEnvironmentVariable("DCSVR_SMOKE_DEMO_ROOT") is { Length: > 0 } demoSetting ? Path.GetFullPath(demoSetting) : null;
        var fixture = demoRoot ?? output + "-fixture";
        string At(string standard, string demo) { var path = Path.Combine(fixture, demoRoot is null ? standard : demo); Directory.CreateDirectory(Path.GetDirectoryName(path)!); return path; }
        void Log(string value) => File.AppendAllText(output+"-trace.txt",DateTimeOffset.Now.ToString("O")+" "+value+"\n");
        var exe = At("game/bin/DCS.exe", @"SteamLibrary\steamapps\common\DCSWorld\bin\DCS.exe"); var options = At("game/options.lua", @"Saved Games\DCS\Config\options.lua");
        File.WriteAllText(exe,"Offline placeholder. Never executed."); File.WriteAllText(options,"options={graphics={Upscaling=\"DLSS\"},VR={enable=false}}");
        var runtimeDll = At("runtime.dll", @"Pimax\Runtime\PiOpenXR_64.dll"); File.Copy(Path.Combine(AppContext.BaseDirectory,"WebView2Loader.dll"),runtimeDll,true); // PE fixture, never loaded as a runtime.
        var runtime = At("runtime.json", @"Pimax\Runtime\PiOpenXR_64.json"); File.WriteAllText(runtime,JsonData.Serialize(new { runtime = new { library_path = runtimeDll } }));
        var inventory = new InventorySnapshot { DcsExecutable = exe, OptionsPath = options, PimaxRuntime = runtime, SteamVrRuntime = runtime, PimaxVersion = "1.0.1.103", Tracking = TrackingKind.Lighthouse, Observations = ["Offline fixture. No DCS process or headset session."] };
        var service = new ControlService(AppContext.BaseDirectory,Path.Combine(fixture,demoRoot is null ? "state" : @"AppData\Local\DcsControl"));
        Readiness.ActiveRouteProvider = () => new(null, "Offline verification: no headset route.", TrackingKind.Unknown);
        // Pimax Play as saved on a Crystal Super (float32 values as Pimax writes them), never the user's own file: Quick 33 %,
        // or with DCSVR_TEST_PIMAX_MODE=fine the Fine tab at 33/10 · 33/33. runtime_* is what Pimax's runtime renders.
        var fine = string.Equals(Environment.GetEnvironmentVariable("DCSVR_TEST_PIMAX_MODE"), "fine", StringComparison.OrdinalIgnoreCase);
        const string F33 = "0.6600000262260437", F10 = "0.2000000029802322", Q33 = "0.3300000131130219";
        string Sides(string eye, string left, string right, string up, string down) =>
            $"\"runtime_quadviews_fine_fov_{eye}_left\":{left},\"runtime_quadviews_fine_fov_{eye}_right\":{right},\"runtime_quadviews_fine_fov_{eye}_up\":{up},\"runtime_quadviews_fine_fov_{eye}_down\":{down},";
        var pimax = At("pimax-global.json", @"AppData\Roaming\Pimax\AppConfig\global.json");
        File.WriteAllText(pimax, "{\"piplay_quadviews_fine_type\":" + (fine ? 1 : 0) + $",\"piplay_quadviews_quick_horizontal_gaze_fov_scale\":{Q33},\"piplay_quadviews_quick_vertical_gaze_fov_scale\":{Q33},"
            + $"\"piplay_quadviews_fine_fov_top_to_center\":{F33},\"piplay_quadviews_fine_fov_bottom_to_center\":{F33},\"piplay_quadviews_fine_fov_left_left_to_center\":{F33},\"piplay_quadviews_fine_fov_left_right_to_center\":{F10},\"piplay_quadviews_fine_fov_right_left_to_center\":{F10},\"piplay_quadviews_fine_fov_right_right_to_center\":{F33},"
            + (fine ? Sides("left", F33, F10, F33, F33) + Sides("right", F10, F33, F33, F33) : Sides("left", Q33, Q33, Q33, Q33) + Sides("right", Q33, Q33, Q33, Q33))
            + "\"runtime_quadviews_gaze_resolution_scale\":0.25,\"runtime_quadviews_periphery_resolution_scale\":0.1918999999761581,\"runtime_quadviews_focus_transition_type\":1,\"runtime_quadviews_focus_blend_mode\":0,"
            + "\"runtime_quadviews_focus_blend_area\":0.05000000074505806,\"runtime_quadviews_focus_transition_opacity_percent\":0.5,\"runtime_quadviews_vertical_focus_offset\":0}");
        // DCS's log of the headset runtime's views (Quad Views block, then stereo) gives the per-eye resolution, 5424×5356.
        var logs = Path.GetDirectoryName(At(@"Logs\dcs.log", @"Saved Games\DCS\Logs\dcs.log"))!;
        var views = new[] { (0, 1074, 1060), (1, 1074, 1060), (2, 3478, 2048), (3, 3478, 2048), (0, 5424, 5356), (1, 5424, 5356) }
            .Select(v => $"2026-10-04 14:38:35.814 INFO    VISUALIZER (Main): [16:38:35.949][Info   ] OpenXR:     View [{v.Item1}]: Recommended Width={v.Item2} Height={v.Item3} SampleCount=1");
        // The last flight, as DCS, the Pimax runtime, OFXR, Cheeky, CPU Boost and the engine optimizations' prefetch fix log it (formats copied from
        // real logs; never the user's own): 2026-10-04 14:30:00-15:12:40 UTC, DCS PID 4242.
        var sessionStart = new DateTimeOffset(2026, 10, 4, 14, 30, 0, TimeSpan.Zero); var sessionEnd = sessionStart.AddSeconds(2560.5);
        // The log also carries DCS's CPU classification (a Ryzen 7 9800X3D), so What Boost will do is read from it and
        // never from this PC's own dcs.log.
        string[] cpu = ["CPU cores: 8, threads: 16", "logical cores with performance class 6: {4, 5, 6, 7}", "logical cores with performance class 5: {8, 9}",
            "logical cores with performance class 4: {2, 3}", "logical cores with performance class 3: {10, 11}", "logical cores with performance class 2: {0, 1}",
            "logical cores with performance class 1: {12, 13}", "logical cores with performance class 0: {14, 15}", "common cores: {10, 11, 0, 1, 12, 13, 14, 15}",
            "render cores: {4, 5, 6, 7, 8, 9, 2, 3}", "IO cores: {}", "unavailable cores: {}"];
        File.WriteAllLines(Path.Combine(logs, "dcs.log"), ["=== Log opened UTC 2026-10-04 14:30:00", .. cpu.Select(line => "2026-10-04 14:30:01.120 INFO    EDCORE (Main): " + line), .. views, "2026-10-04 14:38:35.900 INFO    VISUALIZER (Main): done",
            "2026-10-04 15:12:40.500 INFO    VISUALIZER (31660): render thread has stopped", "=== Log closed."]);
        var flight = WriteFlightFixture(Path.Combine(fixture, "flight"), Path.GetDirectoryName(logs)!, sessionStart, sessionEnd);
        PimaxFovea.SettingsPath = pimax;
        HeadsetRefresh.PimaxLogFolder = Path.Combine(fixture, "no-pimax-logs"); HeadsetRefresh.PimaxRuntimeFolder = Path.Combine(fixture, "no-pimax-runtime"); HeadsetRefresh.SteamVrSettings = Path.Combine(fixture, "no-steamvr.vrsettings");
        GpuInfo.Adapters = () => [new("NVIDIA GeForce RTX 4090", "NVIDIA", "32.0.15.0")];
        NvidiaSettings.ThreadedOptimizationForDcs = () => new(ThreadedOptimization.Auto, false);
        NvidiaSettings.MfaaForDcs = () => new(false, false);
        NvidiaSettings.DlssPresetForDcs = () => null;
        LaunchSafety.SteamRunning = () => true; // the fixture is the Steam edition; the build machine's Steam does not matter
        var update = UpdateFixture(Path.Combine(fixture, "updates"), service.StateRoot);
        var bridge = new WebBridge(service,capture: (_,_) => inventory,offline:true,flightSources:(_,_) => flight with { DcsLog = Path.Combine(logs, "dcs.log") },updates:update.Updates,releaseNotesPath:update.NotesPath);
        using var source = new HwndSource(new HwndSourceParameters("DCS Control offline web verification") { Width = 1320, Height = 920, WindowStyle = unchecked((int)0x80000000), PositionX = -32000, PositionY = -32000 });
        var env = await CoreWebView2Environment.CreateAsync(null,Path.Combine(fixture,"browser"),new CoreWebView2EnvironmentOptions("--disable-background-networking --disable-component-update --no-first-run --disable-renderer-backgrounding --disable-background-timer-throttling --disable-features=CalculateNativeWinOcclusion"));
        Log("Environment ready");
        var controller = await env.CreateCoreWebView2ControllerAsync(source.Handle);
        try
        {
            // Keep the initial CSS viewport deterministic, independent of the desktop's display scaling.
            controller.ShouldDetectMonitorScaleChanges = false;
            controller.RasterizationScale = 1;
            controller.Bounds = new System.Drawing.Rectangle(0,0,1320,920); controller.IsVisible = true;
            var core = controller.CoreWebView2; WebHost.Configure(core,bridge);
            var neural = Environment.GetEnvironmentVariable("DCSVR_TEST_NEURAL_PATH");
            // A signed x64 DLL that is not NVIDIA's, named like the runtime: it must never become the saved copy.
            var invalidRuntime = Path.Combine(fixture,"not-nvidia/nvngx_dlssnr.dll"); Directory.CreateDirectory(Path.GetDirectoryName(invalidRuntime)!);
            File.Copy(Path.Combine(AppContext.BaseDirectory,"WebView2Loader.dll"),invalidRuntime,true);
            await core.AddScriptToExecuteOnDocumentCreatedAsync("window.offlineFixture="+JsonSerializer.Serialize(new { neuralRuntimePath = neural, invalidRuntimePath = invalidRuntime, pimaxMode = fine ? "Fine" : "Quick", updateVersion = update.Version })+";");
            Log("Controller ready");
            core.NavigationStarting += (_,e) => Log("Navigation: "+e.Uri+" cancelled="+e.Cancel);
            core.ProcessFailed += (_,e) => Log("Process failed: "+e.ProcessFailedKind+" "+e.Reason);
            core.WebResourceResponseReceived += (_,e) => Log("Resource: "+e.Request.Uri+" "+e.Response.StatusCode);
            core.WebMessageReceived += (_,e) => Log("Web message: "+e.WebMessageAsJson[..Math.Min(150,e.WebMessageAsJson.Length)]);
            var ready = new TaskCompletionSource<bool>();
            core.NavigationCompleted += (_,e) => { if (e.IsSuccess) ready.TrySetResult(true); else ready.TrySetException(new IOException("Web navigation failed: "+e.WebErrorStatus)); };
            // Motion follows Windows' setting; the verification renders as with animations off.
            await core.CallDevToolsProtocolMethodAsync("Emulation.setEmulatedMedia", "{\"features\":[{\"name\":\"prefers-reduced-motion\",\"value\":\"reduce\"}]}");
            core.Navigate(WebHost.Address); await ready.Task.WaitAsync(TimeSpan.FromSeconds(30));
            Log("Navigation completed");
            for (var attempt = 0; attempt < 500 && await core.ExecuteScriptAsync("Boolean(window.uiReady)") != "true"; attempt++) await Task.Delay(20);
            if (await core.ExecuteScriptAsync("Boolean(window.uiReady)") != "true") throw new IOException("The web interface did not connect to the .NET bridge.");
            Log("UI connected");
            var completed = new TaskCompletionSource<JsonElement>();
            core.WebMessageReceived += (_,e) => { using var doc = JsonDocument.Parse(e.WebMessageAsJson); if (doc.RootElement.TryGetProperty("smokeResult",out var result)) completed.TrySetResult(result.Clone()); };
            Log("Smoke injection: "+await core.ExecuteScriptAsync(File.ReadAllText(Path.Combine(AppContext.BaseDirectory,"ui/smoke.js"))));
            Log("Smoke run: "+await core.ExecuteScriptAsync("window.runWebSmoke()"));
            var checks = await completed.Task.WaitAsync(TimeSpan.FromSeconds(100));
            File.WriteAllText(output+"-checks.json",checks.GetRawText());
            if (!checks.GetProperty("passed").GetBoolean()) throw new IOException("Web interface verification failed: "+checks.GetRawText());
            if (new DraftStore(service.StateRoot).Load() is null) throw new IOException("The draft was not kept for the next start.");
            // The update: the altered first download was deleted, the second kept with its signature; nothing else was fetched.
            var downloads = Directory.Exists(update.Updates.DownloadRoot) ? Directory.GetFiles(update.Updates.DownloadRoot, "*", SearchOption.AllDirectories).Select(Path.GetFileName).Order(StringComparer.Ordinal).ToArray() : [];
            var setupName = $"DcsControl-{update.Version}-Setup.exe";
            if (!downloads.SequenceEqual([setupName, setupName + ".sig"]) || !update.Updates.VerifyFile(Path.Combine(update.Updates.DownloadRoot, update.Version, setupName)))
                throw new IOException("The update download did not end with only the verified setup: " + string.Join(", ", downloads));
            if (update.Handler.Requests.Any(r => !r.StartsWith("https://api.github.com/", StringComparison.Ordinal) && !r.StartsWith("https://github.com/NIGos/dcs-control/releases/download/", StringComparison.Ordinal)))
                throw new IOException("The update asked another address: " + string.Join(", ", update.Handler.Requests));
            if (new AppSettingsStore(service.StateRoot).Load().LastSeenVersion != ProductInfo.Version) throw new IOException("What's new was not recorded as seen.");
            // The neutral demo layout alone keeps the regular renders (release previews); DCSVR_SMOKE_DOC_VIEWS=1 makes the
            // documentation screenshots instead.
            if (demoRoot is not null && Environment.GetEnvironmentVariable("DCSVR_SMOKE_DOC_VIEWS") == "1") { await DocumentationViews(core, controller, service, output, neural, Log); return; }
            // DCSVR_SMOKE_VIDEO_VIEWS=1: full-HD stills of the real interface for the feature video (artifacts/video).
            if (demoRoot is not null && Environment.GetEnvironmentVariable("DCSVR_SMOKE_VIDEO_VIEWS") == "1") { await VideoViews(core, controller, service, output, neural, Log); return; }
            var screenshots = new List<object>(); var index = 0;
            await core.ExecuteScriptAsync("window.screenshotDraft=window.dcsUi.draft();");
            foreach (var (width,height,scale) in new[] { (1320,920,1d),(940,660,1d),(1320,920,1.5),(1700,1240,2d) })
            {
                await core.ExecuteScriptAsync("window.renderRefreshPending=true;window.dcsUi.run('refresh',window.screenshotDraft).finally(()=>window.renderRefreshPending=false)");
                for (var wait = 0; wait < 300 && await core.ExecuteScriptAsync("window.renderRefreshPending") == "true"; wait++) await Task.Delay(20);
                if (await core.ExecuteScriptAsync("window.renderRefreshPending") == "true") throw new IOException("Screenshot draft refresh timed out.");
                controller.Bounds = new System.Drawing.Rectangle(0,0,width,height);
                var controller3 = controller as CoreWebView2Controller; controller3.RasterizationScale = scale; controller3.ShouldDetectMonitorScaleChanges = false;
                foreach (var page in new[] { "overview","guide","foveation","dlss","framegen","boost","engine","setup","diagnostics","recovery" })
                {
                    if (page == "diagnostics")
                    {
                        // Checks as after Check this PC: Must fix, Check yourself and the folded OK group.
                        await core.ExecuteScriptAsync("window.renderRefreshPending=true;window.dcsUi.run('checkReadiness').finally(()=>window.renderRefreshPending=false)");
                        for (var wait = 0; wait < 300 && await core.ExecuteScriptAsync("window.renderRefreshPending") == "true"; wait++) await Task.Delay(20);
                    }
                    await core.ExecuteScriptAsync("window.dcsUi.navigate("+JsonSerializer.Serialize(page)+");");
                    await Task.Delay(80);
                    var layout = await core.ExecuteScriptAsync("JSON.stringify({overflow:document.documentElement.scrollWidth>innerWidth,footerVisible:document.getElementById('launch').getBoundingClientRect().bottom<=innerHeight,headerVisible:document.getElementById('pageTitle').getBoundingClientRect().top>=0,panelFits:(t=>t.scrollHeight<=t.clientHeight)(document.querySelector('.panel-top')),viewport:[innerWidth,innerHeight],page:"+JsonSerializer.Serialize(page)+"})");
                    var parsed = JsonDocument.Parse(JsonSerializer.Deserialize<string>(layout)!);
                    if (parsed.RootElement.GetProperty("overflow").GetBoolean()) throw new IOException("Web page overflows: "+page);
                    if (!parsed.RootElement.GetProperty("footerVisible").GetBoolean() || !parsed.RootElement.GetProperty("headerVisible").GetBoolean()) throw new IOException("Web workflow leaves the viewport: "+page);
                    // At the default window size the whole right panel (checklist, profile name, setup buttons) fits without scrolling.
                    if ((width,height,scale) == (1320,920,1d) && !parsed.RootElement.GetProperty("panelFits").GetBoolean()) throw new IOException("The right panel scrolls at 1320 × 920: "+page);
                    var path = output+$"-{index++}.png"; await using var stream = File.Create(path);
                    await core.CapturePreviewAsync(CoreWebView2CapturePreviewImageFormat.Png,stream);
                    screenshots.Add(new { page,width,height,scale,path,layout=parsed.RootElement.Clone() });
                }
                {
                    // The diagnostic panel key recording, with Ctrl+Shift held.
                    await core.ExecuteScriptAsync("(()=>{const ui=window.dcsUi;ui.navigate('osd');const key=document.getElementById('input-diagnosticOverlayKey');key.click();window.dispatchEvent(new KeyboardEvent('keydown',{keyCode:17,ctrlKey:true,shiftKey:true,bubbles:true,cancelable:true}));document.getElementById('setting-diagnosticOverlayKey').scrollIntoView({block:'center'});})()");
                    await Task.Delay(80);
                    var raw = await core.ExecuteScriptAsync("JSON.stringify({overflow:document.documentElement.scrollWidth>innerWidth,footerVisible:document.getElementById('launch').getBoundingClientRect().bottom<=innerHeight,headerVisible:document.getElementById('pageTitle').getBoundingClientRect().top>=0,capturing:document.getElementById('input-diagnosticOverlayKey').classList.contains('capturing'),viewport:[innerWidth,innerHeight]})");
                    using var parsed = JsonDocument.Parse(JsonSerializer.Deserialize<string>(raw)!);
                    if (parsed.RootElement.GetProperty("overflow").GetBoolean() || !parsed.RootElement.GetProperty("capturing").GetBoolean() || !parsed.RootElement.GetProperty("footerVisible").GetBoolean()) throw new IOException("Key capture layout failed.");
                    var path = output+$"-{index++}.png"; await using (var stream = File.Create(path)) await core.CapturePreviewAsync(CoreWebView2CapturePreviewImageFormat.Png,stream);
                    screenshots.Add(new { page="framegen-key-capture",width,height,scale,path,layout=parsed.RootElement.Clone() });
                    await core.ExecuteScriptAsync("window.dispatchEvent(new KeyboardEvent('keydown',{keyCode:27,bubbles:true,cancelable:true}));");
                }
                await core.ExecuteScriptAsync("window.setupWizard.open(); window.setupWizard.choose({route:'Pimax',features:{quad:false,dlss:false,framegen:false,boost:false}});");
                for (var stage = 0; stage < 2; stage++)
                {
                    if (stage > 0)
                    {
                        await core.ExecuteScriptAsync("window.setupRenderPending=true;window.setupWizard.next().finally(()=>window.setupRenderPending=false)");
                        for (var wait = 0; wait < 300 && await core.ExecuteScriptAsync("window.setupRenderPending") == "true"; wait++) await Task.Delay(30);
                    }
                    var raw = await core.ExecuteScriptAsync("JSON.stringify({overflow:document.documentElement.scrollWidth>innerWidth,footerVisible:document.getElementById('setupNext').getBoundingClientRect().bottom<=innerHeight,headerVisible:document.getElementById('setupTitle').getBoundingClientRect().top>=0,step:window.setupWizard.step,viewport:[innerWidth,innerHeight]})");
                    using var parsed = JsonDocument.Parse(JsonSerializer.Deserialize<string>(raw)!);
                    var layout = parsed.RootElement;
                    if (layout.GetProperty("step").GetInt32() != stage || layout.GetProperty("overflow").GetBoolean() || !layout.GetProperty("footerVisible").GetBoolean() || !layout.GetProperty("headerVisible").GetBoolean()) throw new IOException("Wizard layout or step failed: " + stage);
                    var path = output+$"-{index++}.png"; await using var stream = File.Create(path); await core.CapturePreviewAsync(CoreWebView2CapturePreviewImageFormat.Png,stream);
                    screenshots.Add(new { page="setup-"+stage,width,height,scale,path,layout=layout.Clone() });
                }
                await core.ExecuteScriptAsync("document.getElementById('setupClose').click()");
            }
            // Launch DCS after Pimax Play's Quad View values changed: only the focus values are updated in place (same
            // applied profile, new installed hash recorded), and Back to stock DCS then completes.
            async Task<string> Await(string script)
            {
                await core.ExecuteScriptAsync("window.verifyStep='pending';Promise.resolve().then(()=>"+script+").then(()=>window.verifyStep='done',e=>window.verifyStep='error:'+e.message)");
                for (var wait = 0; wait < 500 && await core.ExecuteScriptAsync("window.verifyStep") == "\"pending\""; wait++) await Task.Delay(20);
                return JsonSerializer.Deserialize<string>(await core.ExecuteScriptAsync("window.verifyStep"))!;
            }
            async Task<string> Text(string script) => JsonSerializer.Deserialize<string>(await core.ExecuteScriptAsync(script))!;
            async Task Capture(string name, string page = "overview")
            {
                controller.Bounds = new System.Drawing.Rectangle(0,0,1320,920); ((CoreWebView2Controller)controller).RasterizationScale = 1;
                await core.ExecuteScriptAsync("window.dcsUi.navigate("+JsonSerializer.Serialize(page)+");document.getElementById('toast').hidden=true;(f=>{if(f)f.open=true;})(document.getElementById('originalFiles'));"); await Task.Delay(120);
                var layout = await core.ExecuteScriptAsync("JSON.stringify({overflow:document.documentElement.scrollWidth>innerWidth,footerVisible:document.getElementById('launch').getBoundingClientRect().bottom<=innerHeight,headerVisible:document.getElementById('pageTitle').getBoundingClientRect().top>=0,panelFits:(t=>t.scrollHeight<=t.clientHeight)(document.querySelector('.panel-top')),viewport:[innerWidth,innerHeight],page:"+JsonSerializer.Serialize(name)+"})");
                var parsed = JsonDocument.Parse(JsonSerializer.Deserialize<string>(layout)!).RootElement.Clone();
                if (parsed.GetProperty("overflow").GetBoolean() || !parsed.GetProperty("footerVisible").GetBoolean() || !parsed.GetProperty("panelFits").GetBoolean()) throw new IOException("Right panel layout failed: " + name);
                var path = output+$"-{index++}.png"; await using (var stream = File.Create(path)) await core.CapturePreviewAsync(CoreWebView2CapturePreviewImageFormat.Png,stream);
                screenshots.Add(new { page=name,width=1320,height=920,scale=1d,path,layout=parsed });
            }
            var pimaxBefore = File.ReadAllText(pimax);
            await core.ExecuteScriptAsync("(()=>{const p=window.dcsUi.profile;p.quadViews='QuadViewsFoveated';p.foveaSource='PimaxPlay';p.neuralRendering=false;p.foveatedDlss=false;p.quadViewsLayerDirectory=null;window.dcsUi.invalidate();})()");
            if (await Await("window.dcsUi.run('launch')") is var first && first != "done") throw new IOException("Pimax-sourced launch failed: " + first);
            var appliedJournal = service.Originals.ReadCurrent() ?? throw new IOException("No applied profile after launch."); var originalCount = service.Originals.Status().Count;
            var settingsBefore = appliedJournal.Entries.Single(e => e.Path.EndsWith(@"quadviews\settings.cfg", StringComparison.OrdinalIgnoreCase));
            if (await Text("document.getElementById('statusTitle').textContent") != "Ready to fly") throw new IOException("The applied profile does not read Ready to fly.");
            await Capture("ready-to-fly");
            // Recovery with a profile applied: one Original files card, its file list open.
            if (!(await Text("document.getElementById('originals').textContent")).Contains("files changed by DCS Control", StringComparison.Ordinal)) throw new IOException("Recovery does not list the original files.");
            await Capture("recovery-applied", "recovery");
            File.WriteAllText(pimax, pimaxBefore.Replace("\"runtime_quadviews_gaze_resolution_scale\":0.25", "\"runtime_quadviews_gaze_resolution_scale\":0.5"));
            try
            {
                if (await Await("window.dcsUi.run('refresh')") != "done") throw new IOException("Refresh failed.");
                var shown = await Text("document.getElementById('statusTitle').textContent+'|'+document.getElementById('status').textContent");
                if (!shown.StartsWith("Ready to fly|", StringComparison.Ordinal) || !shown.Contains("Pimax Play changed: focus will be updated to", StringComparison.Ordinal)) throw new IOException("The Pimax change is not announced: " + shown);
                await Capture("pimax-changed");
                if (await Await("window.dcsUi.run('launch')") is var second && second != "done") throw new IOException("Launch after a Pimax change failed: " + second);
                var expected = "Pimax Play changed: focus updated to " + (fine ? "Fine 33/10 · 33/33 · 175% · 20%" : "Quick 33% × 33% · 175% · 20%") + ".";
                var status = await Text("window.dcsUi.state.status");
                if (!status.StartsWith(expected, StringComparison.Ordinal) || !status.Contains("DCS was not started", StringComparison.Ordinal)) throw new IOException("Unexpected launch status: " + status);
                var journal = service.Originals.ReadCurrent() ?? throw new IOException("The applied profile disappeared.");
                if (journal.Id != appliedJournal.Id || service.Originals.Status().Count != originalCount) throw new IOException("The Pimax change was not applied in place.");
                var settings = journal.Entries.Single(e => e.Path == settingsBefore.Path);
                if (settings.InstalledSha256 == settingsBefore.InstalledSha256 || settings.InstalledSha256 != Hashing.FileSha256(settings.Path)
                    || !File.ReadAllText(settings.Path).Contains("focus_multiplier=1.75", StringComparison.Ordinal)) throw new IOException("The applied profile does not record the updated settings.");
                if (journal.Entries.Where(e => e.Path != settings.Path && !e.Path.EndsWith(AppliedFovea.FileName, StringComparison.OrdinalIgnoreCase)).Any(e => e.InstalledSha256 != appliedJournal.Entries.Single(x => x.Path == e.Path).InstalledSha256))
                    throw new IOException("Files other than the Pimax-derived settings changed.");
                if (await Text("document.getElementById('statusTitle').textContent") != "Ready to fly") throw new IOException("Not Ready to fly after the update.");
                await Capture("pimax-updated");
                if (await Await("window.dcsUi.run('restore')") != "done") throw new IOException("Restore failed.");
                if (service.Originals.Status() is not { Count: 0, Current: null } || File.Exists(settings.Path)) throw new IOException("Restore after the in-place update did not complete cleanly.");
            }
            finally { File.WriteAllText(pimax, pimaxBefore); }
            // The update banner on the Overview (the smoke hid it), then the update dialog with the fixture's release notes.
            await core.ExecuteScriptAsync("document.getElementById('updateBanner').hidden=false;");
            await Capture("update-available");
            await core.ExecuteScriptAsync("document.getElementById('updateNotesOpen').click();");
            await Capture("update-dialog");
            await core.ExecuteScriptAsync("document.getElementById('updateLater').click();document.getElementById('updateBanner').hidden=true;");
            File.WriteAllText(output+"-renders.json",JsonData.Serialize(screenshots));
            var after = File.ReadAllText(options);
            if (after != "options={graphics={Upscaling=\"DLSS\"},VR={enable=false}}") throw new IOException("Fixture restoration was incomplete.");
        }
        finally { controller.Close(); }
    }
}

/// <summary>The update check's network in the offline verification: fixed answers by URL, every request recorded. The
/// setup's first download arrives with one byte changed.</summary>
internal sealed class UpdateFixtureHandler(IReadOnlyDictionary<string, byte[]> answers, string alteredOnce) : HttpMessageHandler
{
    private int _altered;
    public System.Collections.Concurrent.ConcurrentQueue<string> Requests { get; } = new();
    protected override Task<HttpResponseMessage> SendAsync(HttpRequestMessage request, CancellationToken cancellationToken)
    {
        var url = request.RequestUri!.AbsoluteUri; Requests.Enqueue(url);
        if (!answers.TryGetValue(url, out var bytes)) return Task.FromResult(new HttpResponseMessage(System.Net.HttpStatusCode.NotFound));
        if (url == alteredOnce && Interlocked.Exchange(ref _altered, 1) == 0) { bytes = (byte[])bytes.Clone(); bytes[0] ^= 1; }
        return Task.FromResult(new HttpResponseMessage(System.Net.HttpStatusCode.OK) { Content = new ByteArrayContent(bytes) });
    }
}
