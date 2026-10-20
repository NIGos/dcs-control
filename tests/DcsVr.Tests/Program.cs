using System.Diagnostics;
using System.IO.Compression;
using System.Text;
using DcsVr.Core;

if (args.FirstOrDefault() == "--environment-child") { Console.Write(Environment.GetEnvironmentVariable("XR_RUNTIME_JSON") + "|" + Environment.GetEnvironmentVariable("XR_ENABLE_API_LAYERS")); return 0; }
var workspace = Path.GetFullPath(args.FirstOrDefault() ?? ".");
if (!File.Exists(Path.Combine(workspace, "Directory.Build.props"))) { Console.Error.WriteLine("Run from the repository root, or pass it as the first argument: " + workspace); return 2; }
var root = Path.Combine(workspace, "artifacts/tests/" + Guid.NewGuid().ToString("N")); Directory.CreateDirectory(root);
var tests = new List<(string Name, Action Test)>();
Readiness.ActiveRouteProvider = () => new(null, "Test fixture: no running headset route.", TrackingKind.Unknown);
PimaxFovea.SettingsPath = Path.Combine(root, "no-pimax-play/global.json"); // never the build machine's Pimax Play settings
HeadsetRefresh.PimaxLogFolder = Path.Combine(root, "no-pimax-logs"); HeadsetRefresh.PimaxRuntimeFolder = Path.Combine(root, "no-pimax-runtime"); HeadsetRefresh.SteamVrSettings = Path.Combine(root, "no-steamvr.vrsettings");
GpuInfo.Adapters = () => [new("NVIDIA GeForce RTX 4090", "NVIDIA", "32.0.15.0")]; // the verified pipeline's card, never the build machine's
NvidiaSettings.ThreadedOptimizationForDcs = () => new(ThreadedOptimization.Auto, false); // nor its driver settings
NvidiaSettings.MfaaForDcs = () => new(false, false);
LaunchSafety.CurrentJob = () => default; // never the job object the test runner itself may run in
void Test(string name, Action action) => tests.Add((name, action));
void Require(bool value, string message = "Assertion failed") { if (!value) throw new Exception(message); }
void Throws<T>(Action action) where T : Exception { try { action(); } catch (T) { return; } throw new Exception("Expected " + typeof(T).Name); }
string DeferredOfxr = Path.Combine(workspace, "artifacts/native/ofxr-djules75/XR_APILAYER_XRFrameBridge_diagnostic.dll");
string Make(string relative, string text) { var path = PathPolicy.UnderRoot(root, relative); AtomicFile.WriteText(path, text); return path; }
string Zip(string relative, params (string Name, string Content)[] entries)
{
    var path = PathPolicy.UnderRoot(root, relative); Directory.CreateDirectory(Path.GetDirectoryName(path)!);
    using var zip = ZipFile.Open(path, ZipArchiveMode.Create);
    foreach (var e in entries) { var entry = zip.CreateEntry(e.Name); using var writer = new StreamWriter(entry.Open()); writer.Write(e.Content); }
    return path;
}
const string Lua = "-- preserve comment\r\noptions = { VR = {enable=false, openxr_quadView = true}, graphics = { Upscaling = \"DLSS\", maxFPS=89, }, [\"other\"] = { [1]=\"a\", }, }";
Test("Lua existing replacement preserves unrelated text", () => { var patched = new LuaOptions(Lua).Set(["VR", "openxr_quadView"], false); Require(patched == Lua.Replace("openxr_quadView = true", "openxr_quadView = false")); });
Test("Lua missing field in table without trailing comma", () => { var p = new LuaOptions(Lua).Set(["VR", "openxr_eyeGaze"], true); Require(new LuaOptions(p).Get("VR", "openxr_eyeGaze") == "true"); Require(p.Contains("\r\n")); });
Test("Lua empty table insertion", () => Require(new LuaOptions(new LuaOptions("options={VR={}}").Set(["VR", "enable"], true)).Get("VR", "enable") == "true"));
Test("Lua strings escape quotes and backslashes", () => { var value = "a\\b\"c\n"; var patched = new LuaOptions(Lua).Set(["graphics", "Upscaling"], value); Require(new LuaOptions(patched).Get("graphics", "Upscaling")!.Contains("\\n")); });
Test("Lua rejects execution", () => Throws<InvalidDataException>(() => new LuaOptions("options = os.execute('test')")));
Test("Lua rejects trailing executable code", () => Throws<InvalidDataException>(() => new LuaOptions("options={}\nos.execute('test')")));
Test("Lua rejects duplicate keys", () => Throws<InvalidDataException>(() => new LuaOptions("options={VR={enable=true,enable=false}}")));
Test("Lua refuses replacing a table", () => Throws<InvalidDataException>(() => new LuaOptions(Lua).Set(["VR"], false)));
Test("Lua BOM and long comments", () => Require(new LuaOptions("\uFEFF--[[hello]]\noptions={VR={enable=true}}").Get("VR", "enable") == "true"));
Test("Lua actual DCS parse and patch without writing", () =>
{
    var path = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.UserProfile), "Saved Games/DCS/Config/options.lua");
    if (!File.Exists(path)) return;
    var before = Hashing.FileSha256(path); var text = File.ReadAllText(path); var patch = LuaOptions.Patch(text, new Dictionary<string, object> { ["VR.openxr_quadView"] = false, ["VR.openxr_eyeGaze"] = false, ["VR.enable"] = true });
    Require(new LuaOptions(patch).Get("VR", "openxr_quadView") == "false"); Require(before == Hashing.FileSha256(path));
});
foreach (var unsafePath in new[] { "../outside", "a/../../outside", @"..\outside", @"C:\outside", "file:stream", "CON.txt", "a/NUL", "LPT1.bin", "name.", "name ", "a//b" })
    Test("Reject unsafe path " + unsafePath, () => Throws<InvalidDataException>(() => PathPolicy.UnderRoot(root, unsafePath)));
Test("ZIP extracts ordinary nested files", () => { var z = Zip("normal.zip", ("a/", ""), ("a/test.txt", "hello")); var d = Path.Combine(root, "unzipped"); ComponentCache.ExtractZip(z, d); Require(File.ReadAllText(Path.Combine(d, "a/test.txt")) == "hello"); });
Test("ZIP validates all paths before extraction", () => { var z = Zip("hostile.zip", ("valid.txt", "first"), ("../outside.txt", "bad")); var d = Path.Combine(root, "hostile"); Throws<InvalidDataException>(() => ComponentCache.ExtractZip(z, d)); Require(!Directory.Exists(d)); });
Test("ZIP rejects case-insensitive duplicate paths", () => { var z = Zip("duplicate.zip", ("A.txt", "1"), ("a.txt", "2")); Throws<InvalidDataException>(() => ComponentCache.ExtractZip(z, Path.Combine(root, "duplicate"))); });
Test("ZIP rejects symbolic links", () =>
{
    var z = Zip("symlink.zip", ("link", "target")); using (var a = ZipFile.Open(z, ZipArchiveMode.Update)) a.Entries[0].ExternalAttributes = unchecked((int)0xA0000000);
    Throws<InvalidDataException>(() => ComponentCache.ExtractZip(z, Path.Combine(root, "symlink")));
});
Test("Package digest rejects modified archive", () => { var z = Zip("digest.zip", ("a", "b")); Throws<InvalidDataException>(() => new ComponentCache(Path.Combine(root, "digest-cache")).Import(PackageCatalog.Get("ofxr"), z)); });
foreach (var package in PackageCatalog.All)
    Test("Official package integrity " + package.Id, () => { var archive = Path.Combine(workspace, ".cache/packages", package.ArchiveName); Require(Hashing.FileSha256(archive) == package.Sha256); var d = new ComponentCache(Path.Combine(root, "package-cache")).Import(package, archive); Require(Directory.EnumerateFiles(d, "*", SearchOption.AllDirectories).Any()); });
Test("Profile compatibility rejects Cheeky plus Pimax native Quad Views", () => Require(ProfileValidation.Validate(new() { NeuralRendering = true }).Any(i => i.Code == "quad-cheeky" && i.Severity == IssueSeverity.Error)));
Test("Foveated Super Resolution is stereo only: Quad Views turns it off and never deploys Cheeky for it", () =>
{
    foreach (var provider in new[] { QuadProvider.QuadViewsFoveated, QuadProvider.PimaxNative })
    {
        var p = new VrProfile { QuadViews = provider, FoveatedDlss = true, QuadFocusAdapter = true };
        Require(!p.UsesFoveatedDlss && !p.UsesCheeky && !p.UsesQuadFocus, provider + ": ignored with Quad Views");
        Require(!ProfileValidation.Validate(p).Any(i => i.Code.StartsWith("quad-")), provider + ": no Cheeky error, DLSS is simply off");
        Require(ProfileValidation.ResolveFeatures(p) is { FoveatedDlss: false, QuadFocusAdapter: false } r && r.QuadViews == provider, provider + ": settled to DLSS off, provider kept");
    }
    var dlss5 = ProfileValidation.ResolveFeatures(ProfilePresets.All[5] with { FoveatedDlss = true });
    Require(dlss5 is { FoveatedDlss: false, NeuralRendering: true, UsesQuadFocus: true }, "DLSS 5 with Quad Views keeps the focus adapter without Foveated Super Resolution");
    Require(ConfigurationWriters.Cheeky(ProfilePresets.All[5] with { FoveatedDlss = true }).Split('\n').Any(l => l.Trim() == "Enabled=0"), "A Quad Views profile never writes Enabled=1");
    var stereo = new VrProfile { QuadViews = QuadProvider.None, FoveatedDlss = true };
    Require(stereo.UsesFoveatedDlss && stereo.UsesCheeky && ProfileValidation.ResolveFeatures(stereo).FoveatedDlss && ConfigurationWriters.Cheeky(stereo).Split('\n').Any(l => l.Trim() == "Enabled=1"), "Stereo keeps it");
    var exe = Make("qv-foveated-only/bin/DCS.exe", "fixture"); var options = Make("qv-foveated-only/options.lua", Lua);
    var runtime = Make("qv-foveated-only/runtime.json", "{\"runtime\":{\"library_path\":\"runtime.dll\"}}"); Make("qv-foveated-only/runtime.dll", "fixture");
    var plan = new DeploymentPlanner(new(null, null, null, Path.Combine(workspace, "external/quadviews/bin/x64/Release")))
        .Build(new VrProfile { QuadViews = QuadProvider.QuadViewsFoveated, FoveatedDlss = true, QuadFocusAdapter = true }, new() { DcsExecutable = exe, OptionsPath = options, PimaxRuntime = runtime }, Path.Combine(root, "qv-foveated-only/managed"));
    Require(plan.LaunchEnvironment["DCSVR_QUAD_FOCUS"] == "0" && !plan.Files.Any(f => f.Path.Contains("CheekyFoveatedDLSS", StringComparison.OrdinalIgnoreCase) || f.Path.EndsWith("dxgi.dll", StringComparison.OrdinalIgnoreCase)), "No Cheeky files for Quad Views + Foveated Super Resolution alone");
});
Test("Sboys cannot use native Pimax Quad Views", () => Require(ProfileValidation.Validate(new() { Runtime = RuntimeKind.SboysSteamVr }).Any(i => i.Code == "quad-runtime")));
Test("Unknown profile schema and invalid numbers rejected", () => { var p = new VrProfile { SchemaVersion = 99, FoveaWidth = double.NaN, NeuralIntensity = 2 }; Require(ProfileValidation.Validate(p).Count(i => i.Severity == IssueSeverity.Error) == 3); });
Test("Profile JSON roundtrip", () => Require(JsonData.Deserialize<VrProfile>(JsonData.Serialize(ProfilePresets.All[1])) == ProfilePresets.All[1]));
Test("Advanced DLSS 5 tuning reaches the renderer", () =>
{
    var profile = ProfilePresets.All[5] with { NeuralIntensity = .6, NeuralLocalTone = 1.3, NeuralLocalStructure = .8, NeuralSkinStructure = 1.1, NeuralAutomaticMask = true, NeuralUiCorrection = true,
        NeuralColorStrength = 1.2, NeuralTransferStrength = .9, NeuralPaperWhiteScale = 2, NeuralDepth = NeuralDepthMode.Reversed, NeuralMotionScaleX = -1, NeuralMotionScaleY = 1 };
    var ini = ConfigurationWriters.Cheeky(profile);
    foreach (var expected in new[] { "NrIntensity=0.6", "NrLocalToneStrength=1.3", "NrLocalStructureStrength=0.8", "NrSkinStructureStrength=1.1", "NrAutomaticMask=1", "NrUiCorrection=1", "NrColorStrength=1.2", "NrHdrTransferStrength=0.9", "NrPaperWhiteScale=2", "NrDepthConvention=2", "NrMotionScaleXMultiplier=-1", "NrMotionScaleYMultiplier=1" }) Require(ini.Contains(expected), expected);
    Require(JsonData.Deserialize<VrProfile>(JsonData.Serialize(profile)) == profile);
});
Test("Neural tuning validates all runtime ranges", () =>
{
    var p = new VrProfile { NeuralLocalTone=3, NeuralLocalStructure=-1, NeuralSkinStructure=double.NaN, NeuralColorStrength=3, NeuralTransferStrength=-1, NeuralPaperWhiteScale=0, NeuralMotionScaleX=5, NeuralMotionScaleY=-5, NeuralDepth=(NeuralDepthMode)7 };
    Require(ProfileValidation.Validate(p).Count(i => i.Severity == IssueSeverity.Error) == 9);
});
Test("Neural ABI compatibility is explicit", () =>
{
    Require(NativeBinary.SupportsNeuralContract("310,8,0,0") && NativeBinary.SupportsNeuralContract("310.8.0.0"));
    foreach (var version in new string?[] { null, "310.2.1.0", "310.9.1.0", "junk", "310.80.0.0" }) Require(!NativeBinary.SupportsNeuralContract(version));
});
Test("Unknown JSON settings rejected", () => Throws<System.Text.Json.JsonException>(() => JsonData.Deserialize<VrProfile>("{\"fakeSetting\":1}")));
Test("Cheeky uses real settings schema", () => { var schema = File.ReadAllText(Path.Combine(workspace, "external/cheeky/uevr/settings_fields.inc")); foreach (var key in ConfigurationWriters.Cheeky(new()).Split('\n').Where(l => l.Contains('=')).Select(l => l.Split('=')[0].Trim()).Where(k => k != "SchemaVersion")) Require(schema.Contains(key, StringComparison.Ordinal), key); });
Test("OFXR toggle configuration", () => { var ini = ConfigurationWriters.Ofxr(new() { FrameGen = FrameGeneration.FidelityFx, ShowOverlay = false }); Require(ini.Contains("backend=fidelityfx")); Require(ini.Contains("position=off")); });
Test("OFXR per-frame records only on request, with a larger log", () =>
{
    var off = ConfigurationWriters.Ofxr(new() { FrameGen = FrameGeneration.Nvidia });
    var on = ConfigurationWriters.Ofxr(new() { FrameGen = FrameGeneration.Nvidia, DiagnosticFrameRecords = true });
    Require(!off.Contains("frame_records") && off.Contains("max_file_mb=256\n"), "off: unchanged");
    Require(on.Contains("flush_each_event=0\nframe_records=1\n") && on.Contains("max_file_mb=1024\n"), "on");
    Require(off.Contains("\npipelined_deep_hold=1\nasync_arm=0\n") && ConfigurationWriters.Ofxr(new() { FrameGen = FrameGeneration.Nvidia, FrameGenDeepPipeline = false }).Contains("\npipelined_deep_hold=0\n"), "the deep hold follows the smoothness buffer; async arm off");
    Require(off.Contains("\nhud_symbology=0\n") && ConfigurationWriters.Ofxr(new() { FrameGen = FrameGeneration.Nvidia, FrameGenHudSymbology = true }).Contains("\nhud_symbology=0\n"), "HUD symbology needs Depth boost's dual camera");
    var hud = new VrProfile { FrameGen = FrameGeneration.Nvidia, FrameGenHudSymbology = true, DepthBoost = true, QuadViews = QuadProvider.QuadViewsFoveated, Gaze = GazeMode.EyeTracked };
    Require(hud.UsesHudSymbology && ConfigurationWriters.Ofxr(hud).Contains("\nhud_symbology=1\n"), "HUD symbology on with the dual camera");
    Require(ConfigurationWriters.Ofxr(new() { FrameGen = FrameGeneration.Nvidia, FrameGenPipelinedDeepHold = true, FrameGenAsyncArm = true }).Contains("\npipelined_deep_hold=1\nasync_arm=1\n"));
});
Test("Explicit manifest keeps extension metadata", () =>
{
    var dll = Make("manifest/layer.dll", "fixture"); var path = Make("manifest/manifest.json", "{\"api_layer\":{\"name\":\"layer\",\"library_path\":\"layer.dll\",\"disable_environment\":\"DISABLE\",\"instance_extensions\":[{\"name\":\"XR_VARJO_quad_views\"}]}}");
    var result = ConfigurationWriters.ExplicitManifest(File.ReadAllText(path), path); Require(result.Contains("XR_VARJO_quad_views")); Require(!result.Contains("disable_environment")); Require(result.Contains("layer.dll"));
});
Test("Explicit manifest rejects missing library", () => Throws<InvalidDataException>(() => ConfigurationWriters.ExplicitManifest("{\"api_layer\":{\"library_path\":\"absent.dll\"}}", Path.Combine(root, "x.json"))));
Test("Transaction restores bytes exactly and removes new files", () =>
{
    var old = Make("transaction/original", "old\r\nexact"); var fresh = Path.Combine(root, "transaction/new"); var store = new TransactionStore(Path.Combine(root, "journals/normal"));
    var j = store.Apply(new("fixture", "Test", [new(old, Hashing.FileSha256(old), Encoding.UTF8.GetBytes("new"), "test"), new(fresh, null, [1, 2], "new")], new Dictionary<string, string>(), "fixture"));
    Require(File.ReadAllText(old) == "new"); Require(store.Restore(j.Id).Complete); Require(File.ReadAllText(old) == "old\r\nexact"); Require(!File.Exists(fresh)); Require(store.Restore(j.Id).Complete);
});
Test("Transaction rolls back partial failure", () =>
{
    var first = Make("rollback/first", "old"); var second = Path.Combine(root, "rollback/second"); var store = new TransactionStore(Path.Combine(root, "journals/rollback"));
    Throws<IOException>(() => store.Apply(new("fixture", "Test", [new(first, Hashing.FileSha256(first), [4], "one"), new(second, null, [5], "two")], new Dictionary<string, string>(), "fixture"), i => { if (i == 1) throw new IOException("Injected failure"); }));
    Require(File.ReadAllText(first) == "old"); Require(!File.Exists(second)); Require(store.List()[0].Status == "restored");
});
Test("Transaction protects edits after installation", () =>
{
    var path = Make("conflict/file", "old"); var store = new TransactionStore(Path.Combine(root, "journals/conflict")); var j = store.Apply(new("fixture", "Test", [new(path, Hashing.FileSha256(path), [4], "one")], new Dictionary<string, string>(), "fixture"));
    AtomicFile.WriteText(path, "user-edit"); var r = store.Restore(j.Id); Require(!r.Complete && r.Conflicts.Count == 1); Require(File.ReadAllText(path) == "user-edit");
});
Test("Transaction detects changes since preview", () =>
{
    var path = Make("race/file", "old"); var hash = Hashing.FileSha256(path); AtomicFile.WriteText(path, "edit"); var store = new TransactionStore(Path.Combine(root, "journals/race"));
    Throws<IOException>(() => store.Apply(new("fixture", "Test", [new(path, hash, [4], "one")], new Dictionary<string, string>(), "fixture"))); Require(File.ReadAllText(path) == "edit");
});
Test("Transaction detects altered backup", () =>
{
    var path = Make("corrupt/file", "old"); var store = new TransactionStore(Path.Combine(root, "journals/corrupt")); var j = store.Apply(new("fixture", "Test", [new(path, Hashing.FileSha256(path), [4], "one")], new Dictionary<string, string>(), "fixture"));
    AtomicFile.WriteText(Path.Combine(store.StateDirectory, j.Id, j.Entries[0].BackupFile!), "corrupt"); Throws<InvalidDataException>(() => store.Restore(j.Id)); Require(File.ReadAllBytes(path).SequenceEqual(new byte[] { 4 }));
});
Test("Transaction rejects unsafe restore ID", () => Throws<InvalidDataException>(() => new TransactionStore(root).Restore("../other")));
// ---- Original files: back up once, overwrite afterwards, Back to stock DCS puts everything back ----------------------
ApplyPlan Plan(string profile, params FileMutation[] files) => new(profile, profile, files, new Dictionary<string, string>(), "fixture");
FileMutation Write(string path, string content, string? root = null, IReadOnlyList<string>? logs = null) =>
    new(path, File.Exists(path) ? Hashing.FileSha256(path) : null, Encoding.UTF8.GetBytes(content), "test", RuntimeLogs: logs, Root: root);
Test("Originals: the first write backs up, later applies overwrite anything without conflicts, Back to stock DCS puts back exact bytes", () =>
{
    var replaced = Make("originals/basic/replaced.cfg", "original\r\nexact"); var created = Path.Combine(root, "originals/basic/created.json");
    var store = new OriginalsStore(Path.Combine(root, "originals/basic/state"));
    var first = store.Apply(Plan("one", Write(replaced, "installed"), Write(created, "{}")));
    Require(File.ReadAllText(replaced) == "installed" && File.ReadAllText(created) == "{}" && first.ReplacedForeign.SequenceEqual([replaced]), "Written; the existing file was another program's");
    // A file DCS Control created, edited by the user or left by anything else: the next apply simply overwrites it.
    // (Another program's file changed after DCS Control replaced it has its own test below.)
    AtomicFile.WriteText(created, "edited");
    var second = store.Apply(Plan("two", Write(replaced, "second"), Write(created, "{\"v\":2}")));
    Require(File.ReadAllText(replaced) == "second" && second.ReplacedForeign.Count == 0 && store.Status() is { Count: 2, State: "applied", ProfileId: "two" }, "Overwritten");
    Require(Directory.GetFiles(store.Directory, "*.original").Length == 1, "One backup: the original, taken the first time");
    AtomicFile.WriteText(created, "edited again");
    Require(store.RestoreOriginals().Complete && File.ReadAllText(replaced) == "original\r\nexact" && !File.Exists(created), "Originals back; a created file goes whatever it held");
    Require(store.Status() is { Count: 0, State: "clean", Current: null } && Directory.GetFiles(store.Directory, "*.original").Length == 0, "Nothing left to restore");
    Require(store.RestoreOriginals().Complete, "Restoring twice is harmless");
});
Test("Originals: another program's file is backed up once, replaced, reported and brought back", () =>
{
    var bin = Path.Combine(root, "originals/foreign/bin"); var dxgi = Make("originals/foreign/bin/dxgi.dll", "ReShade");
    var store = new OriginalsStore(Path.Combine(root, "originals/foreign/state"));
    var applied = store.Apply(Plan("cheeky", Write(dxgi, "cheeky loader", bin)));
    Require(applied.ReplacedForeign.SequenceEqual([dxgi]) && store.Status().Files.Single() is { Action: "restore", Foreign: true }, "Reported as another program's file");
    Require(store.Apply(Plan("cheeky", Write(dxgi, "cheeky loader v2", bin))).ReplacedForeign.Count == 0, "Reported once");
    Require(store.RestoreOriginals().Complete && File.ReadAllText(dxgi) == "ReShade");
});
Test("Originals: another program's file updated after DCS Control replaced it is backed up again and never downgraded", () =>
{
    var bin = Path.Combine(root, "originals/foreign-update/bin"); var dxgi = Make("originals/foreign-update/bin/dxgi.dll", "ReShade 6.0");
    var store = new OriginalsStore(Path.Combine(root, "originals/foreign-update/state"));
    store.Apply(Plan("cheeky", Write(dxgi, "cheeky loader", bin)));
    // ReShade's updater puts its newer dxgi.dll back over DCS Control's loader.
    AtomicFile.WriteText(dxgi, "ReShade 6.1");
    var again = store.Apply(Plan("cheeky", Write(dxgi, "cheeky loader", bin)));
    Require(File.ReadAllText(dxgi) == "cheeky loader" && again.ReplacedForeign.SequenceEqual([dxgi]), "The newer file is backed up before it is replaced, and reported");
    Require(store.RestoreOriginals().Complete && File.ReadAllText(dxgi) == "ReShade 6.1", "Back to stock DCS brings back the newer file, not the older original");
    // Updated again after the last apply, then Back to stock DCS: the newer file stays; nothing older is written over it.
    AtomicFile.WriteText(dxgi, "ReShade 6.0");
    store.Apply(Plan("cheeky", Write(dxgi, "cheeky loader", bin)));
    AtomicFile.WriteText(dxgi, "ReShade 6.2");
    Require(store.RestoreOriginals().Complete && File.ReadAllText(dxgi) == "ReShade 6.2", "A newer file found at restore is left as it is");
    Require(store.Status() is { Count: 0, State: "clean" }, "Nothing left to restore");
    // Another DCS Control copy (a dev build, another state folder) writes its own Cheeky loader there: never the pilot's.
    store.Apply(Plan("cheeky", Write(dxgi, "cheeky loader", bin)));
    var backups = Directory.GetFiles(store.Directory, "*.original").Length;
    AtomicFile.WriteText(dxgi, "another copy's CheekyFoveatedDLSS loader");
    Require(store.Apply(Plan("cheeky", Write(dxgi, "cheeky loader", bin))).ReplacedForeign.Count == 0, "A Cheeky loader is not taken as a newer original");
    Require(store.RestoreOriginals().Complete && File.ReadAllText(dxgi) == "ReShade 6.2", "Back to stock DCS still brings back the pilot's file");
    // A newer file taking over leaves no orphaned backup of the older one.
    store.Apply(Plan("cheeky", Write(dxgi, "cheeky loader", bin))); AtomicFile.WriteText(dxgi, "ReShade 6.3");
    store.Apply(Plan("cheeky", Write(dxgi, "cheeky loader", bin)));
    Require(Directory.GetFiles(store.Directory, "*.original").Length == backups, "The superseded backup is deleted");
    Require(store.RestoreOriginals().Complete && File.ReadAllText(dxgi) == "ReShade 6.3");
});
Test("Originals: another program's library the pilot installs where DCS Control's file was (an empty path before) is backed up, and a newer one survives a profile without it", () =>
{
    var bin = Path.GetFullPath(Path.Combine(root, "originals/pilot-install/bin")); System.IO.Directory.CreateDirectory(bin); var dxgi = Path.Combine(bin, "dxgi.dll"); var other = Path.Combine(bin, "other.dll");
    var store = new OriginalsStore(Path.Combine(root, "originals/pilot-install/state"));
    store.Apply(Plan("cheeky", Write(dxgi, "cheeky loader", bin)));
    // ReShade installed while the profile is applied: the next apply backs it up before writing the loader again.
    AtomicFile.WriteText(dxgi, "MZ ReShade 6.0");
    Require(store.Apply(Plan("cheeky", Write(dxgi, "cheeky loader", bin))).ReplacedForeign.SequenceEqual([dxgi]), "Backed up and reported");
    Require(store.RestoreOriginals().Complete && File.ReadAllText(dxgi) == "MZ ReShade 6.0", "Back to stock DCS brings it back");
    // ReShade updated after DCS Control replaced it, then a profile without the loader: the newer file stays.
    store.Apply(Plan("cheeky", Write(dxgi, "cheeky loader", bin)));
    AtomicFile.WriteText(dxgi, "MZ ReShade 6.1");
    store.Apply(Plan("plain", Write(other, "other", bin)));
    Require(File.ReadAllText(dxgi) == "MZ ReShade 6.1", "The older original is not written over the newer file");
    Require(store.RestoreOriginals().Complete && File.ReadAllText(dxgi) == "MZ ReShade 6.1" && !File.Exists(other));
    // A library the pilot installed over DCS Control's on a path that was empty, then a profile without the loader:
    // the pilot's file stays (it was never backed up, so putting the path back would delete it).
    File.Delete(dxgi); store.Apply(Plan("cheeky", Write(dxgi, "cheeky loader", bin)));
    AtomicFile.WriteText(dxgi, "MZ OpenKneeboard");
    store.Apply(Plan("plain", Write(other, "other", bin)));
    Require(File.ReadAllText(dxgi) == "MZ OpenKneeboard", "Kept across a profile switch");
    Require(store.RestoreOriginals().Complete && File.ReadAllText(dxgi) == "MZ OpenKneeboard", "and by Back to stock DCS");
});
Test("Originals: bytes of DCS Control's own components already there count as absent", () =>
{
    var bin = Path.Combine(root, "originals/leftover/bin"); var managed = Path.Combine(root, "originals/leftover/managed");
    var loader = Make("originals/leftover/bin/dxgi.dll", "focus loader"); var host = Make("originals/leftover/bin/Cheeky/Host.dll", "cheeky host");
    var profile = Make("originals/leftover/managed/profiles/p/profile.json", "an older profile");
    var store = new OriginalsStore(Path.Combine(root, "originals/leftover/state"), ownHashes: () => new HashSet<string> { Hashing.BytesSha256(Encoding.UTF8.GetBytes("cheeky host")) });
    var applied = store.Apply(Plan("p",
        new(loader, Hashing.FileSha256(loader), Encoding.UTF8.GetBytes("upstream loader"), "loader", Root: bin, OwnHashes: [Hashing.BytesSha256(Encoding.UTF8.GetBytes("focus loader"))]),
        Write(host, "cheeky host v2", bin), new(profile, Hashing.FileSha256(profile), Encoding.UTF8.GetBytes("{}"), "profile", Root: managed, OwnedLocation: true)));
    Require(applied.ReplacedForeign.Count == 0 && store.Status().Files.All(f => f.Action == "remove" && !f.Foreign), "Leftovers of ours are not another program's files");
    Require(store.RestoreOriginals().Complete && !File.Exists(loader) && !File.Exists(host) && !File.Exists(profile) && !Directory.Exists(Path.Combine(bin, "Cheeky")) && Directory.Exists(bin), "Removed with their empty folder, never the root");
});
Test("Originals: options.lua keeps every user edit; only owned settings go back, and an unreadable file gets the whole original", () =>
{
    var config = Path.Combine(root, "originals/lua/Config"); var path = Make("originals/lua/Config/options.lua", Lua);
    var store = new OriginalsStore(Path.Combine(root, "originals/lua/state"));
    FileMutation Settings(params (string Key, string? Before, string After)[] changes)
    {
        var text = File.ReadAllText(path); foreach (var c in changes) text = new LuaOptions(text).SetLiteral(c.Key.Split('.'), c.After);
        return new(path, Hashing.FileSha256(path), Encoding.UTF8.GetBytes(text), "options", changes.Select(c => new LuaValueChange(c.Key, c.Before, c.After)).ToArray(), Root: config);
    }
    store.Apply(Plan("a", Settings(("VR.enable", "false", "true"), ("VR.openxr_eyeGaze", null, "true"))));
    var lua = new LuaOptions(File.ReadAllText(path)); Require(lua.Get("VR", "enable") == "true" && lua.Get("VR", "openxr_eyeGaze") == "true");
    // The user (or DCS) changes other settings and even an owned one.
    AtomicFile.WriteText(path, File.ReadAllText(path).Replace("maxFPS=89", "maxFPS=120").Replace("\"DLSS\"", "'DLSS'"));
    AtomicFile.WriteText(path, new LuaOptions(File.ReadAllText(path)).SetLiteral(["VR", "enable"], "false"));
    // A second profile owns graphics.maxFPS too; its original is the user's 120, eyeGaze is no longer used and goes back.
    store.Apply(Plan("b", Settings(("VR.enable", "false", "true"), ("graphics.maxFPS", "120", "45"))));
    lua = new LuaOptions(File.ReadAllText(path));
    Require(lua.Get("VR", "enable") == "true" && lua.Get("graphics", "maxFPS") == "45" && lua.Get("VR", "openxr_eyeGaze") is null, "Settings the new profile no longer uses went back");
    Require(store.Status().Files.Single() is { Action: "settings" } item && item.Detail!.Contains("graphics.maxFPS") && !item.Detail.Contains("openxr_eyeGaze"));
    AtomicFile.WriteText(path, File.ReadAllText(path).Replace("-- preserve comment", "-- DCS was here"));
    Require(store.RestoreOriginals().Complete);
    lua = new LuaOptions(File.ReadAllText(path));
    Require(lua.Get("VR", "enable") == "false" && lua.Get("graphics", "maxFPS") == "120" && lua.Get("VR", "openxr_eyeGaze") is null && lua.Get("graphics", "Upscaling") == "'DLSS'" && File.ReadAllText(path).Contains("-- DCS was here"), "Owned settings back, the rest kept");
    // A file that can no longer be parsed (never executed) gets the whole original back.
    store.Apply(Plan("c", Settings(("VR.enable", "false", "true"))));
    AtomicFile.WriteText(path, "options={}\nos.execute('never')");
    Require(store.RestoreOriginals().Complete && new LuaOptions(File.ReadAllText(path)).Get("VR", "enable") == "false");
});
Test("Originals: an interrupted apply keeps every original; the next apply or Back to stock DCS finishes it", () =>
{
    var a = Make("originals/interrupted/a.cfg", "A"); var b = Path.Combine(root, "originals/interrupted/b.cfg");
    var store = new OriginalsStore(Path.Combine(root, "originals/interrupted/state"));
    try { store.Apply(Plan("p", Write(a, "a1"), Write(b, "b1")), i => { if (i == 1) throw new IOException("Injected failure"); }); throw new Exception("No failure"); }
    catch (IOException e) { Require(e.Message.Contains("Back to stock DCS") && e.Message.Contains("Injected failure"), e.Message); }
    Require(File.ReadAllText(a) == "a1" && !File.Exists(b) && store.Status() is { Count: 2, State: "applying", Current: null }, "Both originals recorded before anything was written");
    Require(store.Apply(Plan("p", Write(a, "a2"), Write(b, "b2"))).Current.Status == "applied" && File.ReadAllText(b) == "b2", "Launching again completes it");
    Require(store.RestoreOriginals().Complete && File.ReadAllText(a) == "A" && !File.Exists(b));
});
Test("Originals: a damaged backup is reported and kept listed; every other path is restored", () =>
{
    var a = Make("originals/damaged/a.cfg", "A"); var b = Make("originals/damaged/b.cfg", "B");
    var store = new OriginalsStore(Path.Combine(root, "originals/damaged/state"));
    store.Apply(Plan("p", Write(a, "a1"), Write(b, "b1")));
    File.WriteAllText(Directory.GetFiles(store.Directory, "*.original").Order().First(), "corrupt");
    var result = store.RestoreOriginals();
    Require(!result.Complete && result.Conflicts.Single().StartsWith(a) && File.ReadAllText(a) == "a1" && File.ReadAllText(b) == "B", "Damaged backup never written");
    Require(store.Status() is { Count: 1, State: "restoring" } && store.Apply(Plan("p", Write(a, "a2"))).Current is not null, "Still listed, and never blocks the next apply");
});
Test("Originals: a path a new profile drops, with a damaged backup, stays listed and does not block the apply", () =>
{
    var a = Make("originals/stale-damaged/a.cfg", "A"); var b = Make("originals/stale-damaged/b.cfg", "B");
    var store = new OriginalsStore(Path.Combine(root, "originals/stale-damaged/state"));
    store.Apply(Plan("p", Write(a, "a1"), Write(b, "b1")));
    File.WriteAllText(Directory.GetFiles(store.Directory, "*.original").Order().First(), "corrupt");
    Require(store.Apply(Plan("q", Write(b, "b2"))).Current.Status == "applied" && File.ReadAllText(a) == "a1" && store.Status().Count == 2, "Applied; the damaged path is kept for Back to stock DCS");
    var result = store.RestoreOriginals();
    Require(!result.Complete && result.Conflicts.Single().StartsWith(a) && File.ReadAllText(b) == "B");
});
Test("Originals: options.lua settings a new profile drops keep their originals when the write fails", () =>
{
    var config = Path.Combine(root, "originals/lua-fail/Config"); var path = Make("originals/lua-fail/Config/options.lua", Lua);
    var store = new OriginalsStore(Path.Combine(root, "originals/lua-fail/state"));
    FileMutation Settings(params (string Key, string? Before, string After)[] changes)
    {
        var text = File.ReadAllText(path); foreach (var c in changes) text = new LuaOptions(text).SetLiteral(c.Key.Split('.'), c.After);
        return new(path, Hashing.FileSha256(path), Encoding.UTF8.GetBytes(text), "options", changes.Select(c => new LuaValueChange(c.Key, c.Before, c.After)).ToArray(), Root: config);
    }
    store.Apply(Plan("a", Settings(("VR.enable", "false", "true"), ("VR.openxr_eyeGaze", null, "true"))));
    Throws<IOException>(() => store.Apply(Plan("b", Settings(("VR.enable", "false", "true"))), _ => throw new IOException("Injected failure")));
    Require(store.Status().Files.Single().Detail!.Contains("openxr_eyeGaze"), "The dropped setting's original is still recorded");
    Require(store.RestoreOriginals().Complete && new LuaOptions(File.ReadAllText(path)) is var lua && lua.Get("VR", "openxr_eyeGaze") is null && lua.Get("VR", "enable") == "false", "Back to stock DCS still puts it back");
});
Test("Originals: an in-place update records the new bytes and Back to stock DCS still brings the original back", () =>
{
    var settings = Make("originals/update/settings.cfg", "original"); var lua = Make("originals/update/options.lua", "options={VR={enable=false}}");
    var store = new OriginalsStore(Path.Combine(root, "originals/update/state"));
    Throws<InvalidOperationException>(() => store.UpdateInstalled([(settings, [1])])); // nothing applied
    store.Apply(Plan("p", Write(settings, "v1"), new(lua, Hashing.FileSha256(lua), Encoding.UTF8.GetBytes("options={VR={enable=true}}"), "lua", [new("VR.enable", "false", "true")])));
    AtomicFile.WriteText(settings, "edited");
    store.UpdateInstalled([(settings, Encoding.UTF8.GetBytes("v2"))]);
    Require(File.ReadAllText(settings) == "v2" && store.ReadCurrent()!.Entries.Single(e => e.Path == settings).InstalledSha256 == Hashing.FileSha256(settings), "Overwritten and recorded");
    Throws<InvalidOperationException>(() => store.UpdateInstalled([(lua, [9])]));
    Throws<InvalidOperationException>(() => store.UpdateInstalled([(Path.Combine(root, "originals/update/other"), [9])]));
    Throws<InvalidDataException>(() => store.UpdateInstalled([(settings, [9]), (settings, [8])]));
    Require(store.RestoreOriginals().Complete && File.ReadAllText(settings) == "original" && new LuaOptions(File.ReadAllText(lua)).Get("VR", "enable") == "false");
});
// This PC on 2026-10-05: an app started inside another app's sandbox kept its journals in a private copy of
// %LOCALAPPDATA% (Packages\<package>\LocalCache\Local\DcsControl). Its profile (journal A, applied) owned the DCS files;
// the normally started app could not see A, so its own apply (journal B) found A's files: Cheeky's host, runtime and INI
// and the managed files were gone (removed in between, recorded nowhere), dxgi.dll, dxgi2.dll and nvngx_dlssnr.dll were
// A's bytes and were "backed up" as originals, and options.lua already held A's settings. Both journals say "applied".
Test("Conversion: two applied journals (this PC's state) give the true originals; Back to stock DCS and a new apply work", () =>
{
    var dir = Path.Combine(root, "convert-two-applied"); var bin = Path.Combine(dir, "DCSWorld/bin"); var config = Path.Combine(dir, "Saved Games/DCS/Config");
    var managed = Path.Combine(dir, "state/managed"); var profileDir = Path.Combine(managed, "profiles/pimax-qv-dlss5-fg-boost");
    Directory.CreateDirectory(bin); Directory.CreateDirectory(managed);
    var original = "options = {\n\t[\"graphics\"] = {\n\t\t[\"sync\"] = false,\n\t\t[\"maxFPS\"] = 90,\n\t},\n\t[\"miscellaneous\"] = {\n\t\t[\"launcher\"] = true,\n\t},\n\t[\"VR\"] = {\n\t\t[\"enable\"] = true,\n\t},\n}\n";
    var options = Make("convert-two-applied/Saved Games/DCS/Config/options.lua", original);
    var dxgi = Path.Combine(bin, "dxgi.dll"); var dxgi2 = Path.Combine(bin, "dxgi2.dll"); var cheekyDir = Path.Combine(bin, "CheekyFoveatedDLSS");
    var host = Path.Combine(cheekyDir, "CheekyFoveatedDLSSHost.dll"); var runtimeDll = Path.Combine(cheekyDir, "CheekyFoveatedDLSSRuntime.dll");
    var ini = Path.Combine(cheekyDir, "CheekyFoveatedDLSS.ini"); var nvngx = Path.Combine(cheekyDir, "nvngx_dlssnr.dll");
    var layer = Path.Combine(profileDir, "cheeky/CheekyOpenXRLayer.dll"); var profileJson = Path.Combine(profileDir, "profile.json"); var launchJson = Path.Combine(profileDir, "launch.json");
    var loaderLog = Path.Combine(bin, "CheekyFoveatedDLSS-Loader.log"); var hostLog = Path.Combine(cheekyDir, "CheekyFoveatedDLSS-Host.log");
    var userFile = Make("convert-two-applied/DCSWorld/bin/user-mod.txt", "not ours");
    FileMutation Bin(string path, string content, IReadOnlyList<string>? logs = null) => new(path, File.Exists(path) ? Hashing.FileSha256(path) : null, Encoding.UTF8.GetBytes(content), "fixture", RuntimeLogs: logs, Root: bin);
    FileMutation Managed(string path, string content) => new(path, File.Exists(path) ? Hashing.FileSha256(path) : null, Encoding.UTF8.GetBytes(content), "fixture", Root: managed);
    FileMutation Options(params (string Key, string Before, string After)[] changes)
    {
        var text = File.ReadAllText(options); foreach (var c in changes) text = new LuaOptions(text).SetLiteral(c.Key.Split('.'), c.After);
        return new(options, Hashing.FileSha256(options), Encoding.UTF8.GetBytes(text), "options", changes.Select(c => new LuaValueChange(c.Key, c.Before, c.After)).ToArray(), Root: config);
    }
    var sandboxCopy = new TransactionStore(Path.Combine(dir, "Packages/Claude_fixture/LocalCache/Local/DcsControl/transactions"));
    var realStore = new TransactionStore(Path.Combine(dir, "state/transactions"));
    // Older history in the real store, fully restored: ignored.
    var old = realStore.Apply(Plan("pimax-qv-dlss5-fg-boost", Bin(dxgi, "loader v0"), Managed(profileJson, "{\"v\":0}")));
    Require(realStore.Restore(old.Id).Complete && !File.Exists(dxgi));
    // Journal A (17:37Z, sandboxed app): options.lua MOD (launcher true -> false), everything else NEW.
    var logs = new[] { loaderLog, hostLog };
    var a = sandboxCopy.Apply(Plan("pimax-qv-dlss5-fg-boost", Options(("miscellaneous.launcher", "true", "false")), Bin(dxgi, "focus loader", logs), Bin(dxgi2, "prefetch fix"),
        Bin(host, "host A"), Bin(runtimeDll, "runtime A"), Bin(ini, "[A]\nX=1\n"), Managed(layer, "layer A"), Bin(nvngx, "user runtime 310.8"), Managed(profileJson, "{\"v\":1}"), Managed(launchJson, "{\"v\":1}")));
    File.WriteAllText(loaderLog, "log"); File.WriteAllText(hostLog, "log");
    // In between: the Cheeky files and the managed files disappear without any journal noting it.
    foreach (var gone in new[] { host, runtimeDll, ini, layer, profileJson, launchJson }) File.Delete(gone);
    // Journal B (21:20Z, normally started app): options.lua MOD (graphics.sync false -> false), dxgi.dll, dxgi2.dll and
    // nvngx_dlssnr.dll MOD with A's bytes as "originals", Cheeky files and managed files NEW.
    var b = realStore.Apply(Plan("pimax-qv-dlss5-fg-boost", Options(("graphics.sync", "false", "false")), Bin(dxgi, "focus loader", logs), Bin(dxgi2, "prefetch fix"),
        Bin(host, "host B"), Bin(runtimeDll, "runtime B"), Bin(ini, "[B]\nX=2\n"), Managed(layer, "layer B"), Bin(nvngx, "user runtime 310.8"), Managed(profileJson, "{\"v\":2}"), Managed(launchJson, "{\"v\":2}")));
    Require(a.Status == "applied" && b.Status == "applied" && b.Entries.Count(e => e.PreviousSha256 is not null) == 4, "The fixture reproduces two applied journals with 4 MOD and 6 NEW entries in B");
    var store = new OriginalsStore(Path.Combine(dir, "state/originals"), () => [realStore.StateDirectory, sandboxCopy.StateDirectory]);
    var status = store.Status();
    Require(status.Count == 10 && status.State == "applied" && status.ProfileId == "pimax-qv-dlss5-fg-boost" && status.Current!.Id == b.Id, "Ten paths, the newest applied journal is the applied profile");
    var lua = status.Files.Single(f => f.Path == options);
    Require(lua.Action == "settings" && lua.Detail == "miscellaneous.launcher, graphics.sync", "options.lua: launcher from A (true), sync from B: " + lua.Detail);
    Require(status.Files.Where(f => f.Path != options).All(f => f.Action == "remove" && !f.Foreign), "Every other path was absent before DCS Control: " + string.Join(", ", status.Files.Where(f => f.Action != "remove").Select(f => f.Path)));
    Require(Directory.Exists(realStore.StateDirectory + ".converted") && !Directory.Exists(realStore.StateDirectory) && Directory.Exists(sandboxCopy.StateDirectory), "The old folder is kept renamed; the sandbox copy is left as it is");
    Require(File.ReadAllText(store.LogPath).Contains("2 journals were marked applied at once"), "The conversion is logged");
    // The launch contract reads the applied profile; Back to stock DCS puts back the true originals.
    Require(store.ReadCurrent()!.Entries.Count == 10);
    Require(store.RestoreOriginals().Complete, "Back to stock DCS completes");
    Require(File.ReadAllText(options) == original, "options.lua is the true original");
    Require(!File.Exists(dxgi) && !File.Exists(dxgi2) && !Directory.Exists(cheekyDir) && !File.Exists(loaderLog) && !Directory.Exists(profileDir + "/cheeky") && !File.Exists(profileJson), "DCS Control's files are gone, A's leftovers included");
    Require(File.ReadAllText(userFile) == "not ours" && Directory.Exists(bin), "Nothing else touched");
    // A new apply works on the clean state and is restorable again.
    var again = store.Apply(Plan("next", Options(("miscellaneous.launcher", "true", "false")), Bin(dxgi, "focus loader"), Managed(profileJson, "{\"v\":3}")));
    Require(again.ReplacedForeign.Count == 0 && store.Status().Count == 3 && store.RestoreOriginals().Complete && File.ReadAllText(options) == original && !File.Exists(dxgi));
});
Test("Conversion: unreadable or damaged journals are skipped and logged, never deleted", () =>
{
    var dir = Path.Combine(root, "convert-damaged"); var legacy = Path.Combine(dir, "state/transactions");
    var file = Make("convert-damaged/bin/settings.cfg", "original");
    var journal = new TransactionStore(legacy).Apply(Plan("p", Write(file, "installed")));
    File.WriteAllText(Path.Combine(legacy, journal.Id, journal.Entries[0].BackupFile!), "damaged");
    var broken = Path.Combine(legacy, new string('a', 32)); Directory.CreateDirectory(broken); File.WriteAllText(Path.Combine(broken, "journal.json"), "{\"schemaVersion\":1,\"futureField\":true");
    var store = new OriginalsStore(Path.Combine(dir, "state/originals"), () => [legacy]);
    Require(store.Status() is { Count: 0, State: "applied" }, "The damaged path is left out; the profile is still known");
    var log = File.ReadAllText(store.LogPath);
    Require(log.Contains("Skipped unreadable journal") && log.Contains("backup is missing or damaged"), log);
    Require(File.Exists(Path.Combine(legacy + ".converted", new string('a', 32), "journal.json")) && File.ReadAllText(file) == "installed", "Kept for inspection; the file stays as it is");
});
Test("Cheeky ratios are written inside Cheeky's own range, so it never rewrites them", () =>
{
    var ini = ConfigurationWriters.Cheeky(new VrProfile { PeripheralScale = .19, FoveaWidth = .1, FoveaHeight = .37, QuadViews = QuadProvider.None, FoveatedDlss = true });
    Require(ini.Contains("PeripheralDlaaScale=0.2") && ini.Contains("Width=0.2") && ini.Contains("Height=0.37") && ini.Contains("NrWidth=0.2"));
    Require(ConfigurationWriters.Cheeky(new VrProfile { NeuralStyle = NeuralStyle.Cinematic }).Contains("NrStyle=2"));
});
Test("The DLSS 5 toggle key reaches Cheeky as virtual-key:modifiers and unknown keys are rejected", () =>
{
    Require(NeuralHotkeys.Environment(NeuralHotkeys.Default) == "123:5" && NeuralHotkeys.Environment("Off") == "0:0");
    Require(NeuralHotkeys.Environment("Ctrl+Alt+Shift+F10") == "121:7");
    Require(new VrProfile().NeuralToggleKey == "Ctrl+Shift+F12");
    Require(ProfileValidation.Validate(new VrProfile { NeuralToggleKey = "Scroll" }).Any(i => i.Code == "neural-hotkey" && i.Severity == IssueSeverity.Error));
    Require(ProfileValidation.Validate(new VrProfile()).All(i => i.Code != "neural-hotkey"));
});
Test("Triple frame generation writes the fork's triple_frame_gen and paces DCS at a third of the refresh", () =>
{
    var triple = new VrProfile { FrameGen = FrameGeneration.Nvidia, FrameGenFactor = 3, FpsLimit = FpsLimitMode.MatchRefresh, HeadsetRefreshHz = 90 };
    Require(ConfigurationWriters.Ofxr(triple).Contains("triple_frame_gen=1") && ConfigurationWriters.Ofxr(triple with { FrameGenFactor = 2 }).Contains("triple_frame_gen=0"));
    Require(FramePacing.Multiplier(triple) == 3 && FramePacing.RequestedCap(triple) == 30 && FramePacing.Multiplier(triple with { FrameGen = FrameGeneration.Off }) == 1);
    Require(ProfileValidation.Validate(triple with { FrameGenFactor = 4 }).Any(i => i.Code == "framegen-factor"));
    var auto = triple with { FrameGenFactor = VrProfile.FrameGenAuto };
    Require(new VrProfile().FrameGenFactor == VrProfile.FrameGenAuto && ConfigurationWriters.Ofxr(auto).Contains("adaptive_frame_gen=1") && ConfigurationWriters.Ofxr(auto).Contains("triple_frame_gen=0") && ConfigurationWriters.Ofxr(triple).Contains("adaptive_frame_gen=0"));
    // Moving-edge solving (level 1) only where it measured better: Medium preset, one-way flow (the defaults).
    Require(ConfigurationWriters.Ofxr(new VrProfile { FrameGen = FrameGeneration.Nvidia }).Contains("nvidia_occlusion_aware=1")
        && ConfigurationWriters.Ofxr(new VrProfile { FrameGen = FrameGeneration.Nvidia, FlowPreset = NvidiaFlowPreset.Fast }).Contains("nvidia_occlusion_aware=0")
        && ConfigurationWriters.Ofxr(new VrProfile { FrameGen = FrameGeneration.Nvidia, BidirectionalFlow = true }).Contains("nvidia_occlusion_aware=0"));
    Require(ConfigurationWriters.Ofxr(new VrProfile { FrameGen = FrameGeneration.Nvidia }).Contains("cockpit_mask=1") && ConfigurationWriters.Ofxr(new VrProfile { FrameGen = FrameGeneration.Nvidia }).Contains("cockpit_depth=1"), "The cockpit mask and depth consumers are on with Framegen");
    Require(ConfigurationWriters.Ofxr(new VrProfile { FrameGen = FrameGeneration.Nvidia }).Contains("dcs_motion=1") && ConfigurationWriters.Ofxr(new VrProfile { FrameGen = FrameGeneration.FidelityFx }).Contains("dcs_motion=0"), "DCS motion vectors on the NVIDIA path only");
    Require(FramePacing.Multiplier(auto) == 2 && FramePacing.RequestedCap(auto) == 45 && ProfileValidation.Validate(auto).All(i => i.Code != "framegen-factor"));
    Require(ConfigurationWriters.QuadViews(new VrProfile { QuadTurbo = true }).Contains("turbo_mode=1") && ConfigurationWriters.QuadViews(new VrProfile()).Contains("turbo_mode=0"));
    {
        var tracked = ConfigurationWriters.QuadViews(new VrProfile { FrameGen = FrameGeneration.Nvidia });
        Require(tracked.Contains("dcsvr_saccade_widening=1") && tracked.Contains("dcsvr_saccade_lead_ms=85") && tracked.Contains("dcsvr_gaze_deadzone=0.5") && tracked.Contains("dcsvr_saccade_max_extend=0.35"));
        Require(ConfigurationWriters.QuadViews(new VrProfile { FrameGen = FrameGeneration.Nvidia, FrameGenFactor = 2 }).Contains("dcsvr_saccade_lead_ms=55") && ConfigurationWriters.QuadViews(new VrProfile()).Contains("dcsvr_saccade_lead_ms=25"));
        var fixedGaze = ConfigurationWriters.QuadViews(new VrProfile { Gaze = GazeMode.Fixed });
        bool Line(string text, string line) => text.ReplaceLineEndings("\n").Split('\n').Any(l => l.Trim() == line);
        Require(Line(fixedGaze, "dcsvr_saccade_widening=0") && Line(fixedGaze, "dcsvr_gaze_deadzone=0"), "a fixed focus has no gaze to filter");
        var off = ConfigurationWriters.QuadViews(new VrProfile { QuadSaccadeLead = false, QuadGazeStabilize = false });
        Require(Line(off, "dcsvr_saccade_widening=0") && Line(off, "dcsvr_gaze_deadzone=0") && Line(tracked, "dcsvr_gaze_deadzone=0.5"));
    }
    Require(ConfigurationWriters.QuadViews(new VrProfile()).Contains("focus_view_shape=2") && ConfigurationWriters.QuadViews(new VrProfile { QuadRoundFocus = false }).Contains("focus_view_shape=8"));
    Require(ConfigurationWriters.QuadViews(new VrProfile()).Contains("dcsvr_periphery_sharpen=0.3") && ConfigurationWriters.QuadViews(new VrProfile { QuadPeripheryContrast = 0 }).Split('\n').Any(l => l.Trim() == "dcsvr_periphery_sharpen=0"));
    Require(ProfileValidation.Validate(new VrProfile { QuadPeripheryContrast = 1.5 }).Any(i => i.Code == "quad-periphery-contrast"));
    Require(ConfigurationWriters.QuadViews(new VrProfile()).Contains("dcsvr_sharpen_taper=1") && ConfigurationWriters.QuadViews(new VrProfile { QuadSharpenTaper = false }).Contains("dcsvr_sharpen_taper=0"));
    {
        // Periphery stability: off by default; on only with the bundled Quad Views in a VR flight; strength 0..1 -> 0..80 ms.
        bool Line(string text, string line) => text.ReplaceLineEndings("\n").Split('\n').Any(l => l.Trim() == line);
        var bundled = new VrProfile { QuadViews = QuadProvider.QuadViewsFoveated };
        Require(!new VrProfile().PeripheryStability && new VrProfile().PeripheryStabilityStrength == .5);
        Require(Line(ConfigurationWriters.QuadViews(bundled), "dcsvr_temporal=0") && Line(ConfigurationWriters.QuadViews(bundled), "dcsvr_temporal_tau_ms=40"));
        var on = bundled with { PeripheryStability = true };
        var onText = ConfigurationWriters.QuadViews(on);
        Require(on.UsesPeripheryStability && Line(onText, "dcsvr_temporal=1") && Line(onText, "dcsvr_temporal_ramp_deg=8") && Line(onText, "dcsvr_temporal_clip=1") && Line(onText, "dcsvr_temporal_debug=0"));
        Require(Line(ConfigurationWriters.QuadViews(on with { PeripheryStabilityStrength = 1 }), "dcsvr_temporal_tau_ms=80") && Line(ConfigurationWriters.QuadViews(on with { PeripheryStabilityStrength = 0 }), "dcsvr_temporal_tau_ms=0") && Line(ConfigurationWriters.QuadViews(on with { PeripheryStabilityStrength = .3 }), "dcsvr_temporal_tau_ms=24"));
        Require(Line(ConfigurationWriters.QuadViews(on with { Gaze = GazeMode.Fixed }), "dcsvr_temporal=1"), "a fixed focus is stabilised around its centre");
        foreach (var off in new[] { on with { QuadViews = QuadProvider.PimaxNative }, on with { QuadViews = QuadProvider.None }, on with { Desktop = true } })
            Require(!off.UsesPeripheryStability && Line(ConfigurationWriters.QuadViews(off), "dcsvr_temporal=0"));
        Require(ProfileValidation.Validate(on with { PeripheryStabilityStrength = 1.5 }).Any(i => i.Code == "periphery-stability-strength" && i.Severity == IssueSeverity.Error));
        Require(ProfileValidation.Validate(on with { PeripheryStabilityStrength = double.NaN }).Any(i => i.Code == "periphery-stability-strength"));
        Require(!ProfileValidation.Validate(on).Any(i => i.Code.StartsWith("periphery-stability")));
        Require(ProfileValidation.Validate(on with { QuadViews = QuadProvider.PimaxNative }).Any(i => i.Code == "periphery-stability-provider" && i.Severity == IssueSeverity.Warning));
        Require(!ProfileValidation.Validate(on with { Desktop = true }).Any(i => i.Code == "periphery-stability-provider"), "Optimizations only keeps the VR setting for later without a warning");
        Require(!ProfileValidation.Validate(on with { PeripheryStability = false, QuadViews = QuadProvider.PimaxNative }).Any(i => i.Code.StartsWith("periphery-stability")));
    }
});
Test("The smoothness buffer writes OFXR's deep_pipeline in 2x, 3x and Auto, on unless the profile turns it off", () =>
{
    var p = new VrProfile { FrameGen = FrameGeneration.Nvidia, FrameGenFactor = 2 };
    Require(p.FrameGenDeepPipeline && ConfigurationWriters.Ofxr(p).Contains("deep_pipeline=1"));
    var shallow = p with { FrameGenDeepPipeline = false };
    Require(ConfigurationWriters.Ofxr(shallow).Contains("deep_pipeline=0") && !ConfigurationWriters.Ofxr(shallow).Contains("deep_pipeline=1"));
    // OFXR fork patch 0007 applies deep_pipeline to 3x too, so the setting is written as chosen for every multiplier.
    foreach (var factor in new[] { 3, VrProfile.FrameGenAuto })
        Require(ConfigurationWriters.Ofxr(p with { FrameGenFactor = factor }).Contains("deep_pipeline=1") && ConfigurationWriters.Ofxr(shallow with { FrameGenFactor = factor }).Contains("deep_pipeline=0"), factor.ToString());
    // Profiles saved before the option existed keep the deeper pipeline they always had.
    Require(JsonData.Deserialize<VrProfile>("{\"id\":\"legacy\"}").FrameGenDeepPipeline);
    Require(JsonData.Deserialize<VrProfile>(JsonData.Serialize(shallow)) == shallow);
    // Any combination is valid: the fork runs 3X shallow whatever the setting says.
    Require(ProfileValidation.Validate(shallow with { FrameGenFactor = 3 }).Select(i => i.Code).SequenceEqual(ProfileValidation.Validate(p with { FrameGenFactor = 3 }).Select(i => i.Code)));
});
Test("The diagnostic overlay key reaches OFXR only with framegen and never clashes with the DLSS 5 key", () =>
{
    Require(new VrProfile().DiagnosticOverlayKey == "Alt+Shift+F12" && NeuralHotkeys.Environment("Alt+Shift+F12") == "123:6");
    Require(NeuralHotkeys.Environment(null, NeuralHotkeys.DiagnosticDefault) == "123:6");
    Require(ProfileValidation.Validate(new VrProfile { NeuralRendering = true, FrameGen = FrameGeneration.Nvidia, NeuralToggleKey = "Alt+Shift+F12" }).Any(i => i.Code == "hotkey-conflict"));
    Require(ProfileValidation.Validate(new VrProfile { DiagnosticOverlayKey = "Scroll" }).Any(i => i.Code == "diagnostic-hotkey"));
    Require(ProfileValidation.Validate(new VrProfile()).All(i => i.Code is not ("hotkey-conflict" or "diagnostic-hotkey")));
});
Test("Recorded hotkeys: any key with modifiers as virtual-key:modifiers, legacy labels still work", () =>
{
    Require(NeuralHotkeys.TryParse("123:5", out var k) && k == new Hotkey(0x7B, 5) && NeuralHotkeys.Label(k) == "Ctrl+Shift+F12" && NeuralHotkeys.Environment("123:5") == "123:5");
    Require(NeuralHotkeys.Same("Ctrl+Shift+F12", "123:5") && !NeuralHotkeys.Same("Ctrl+Shift+F12", "123:7"));
    Require(NeuralHotkeys.Label("101:2") == "Alt+Num 5" && NeuralHotkeys.Label("75:3") == "Ctrl+Alt+K" && NeuralHotkeys.Label("Off") == "Off" && NeuralHotkeys.Environment("0:0") == "0:0");
    foreach (var bad in new[] { "255:1", "12:8", "0:4", "a:1", "1:", ":1", "Scroll" }) Require(!NeuralHotkeys.TryParse(bad, out _) && NeuralHotkeys.Environment(bad) == "0:0", bad);
    // A modifier is needed unless the key works alone; modifier keys alone and Windows' own combinations are refused.
    Require(NeuralHotkeys.Problem(new(0x4B, 0)) is { } letter && letter.Contains("needs Ctrl, Alt or Shift"));
    Require(NeuralHotkeys.Problem(new(0x7B, 0)) is null && NeuralHotkeys.Problem(new(0x13, 0)) is null && NeuralHotkeys.Problem(new(0x91, 0)) is null);
    Require(NeuralHotkeys.Problem(new(0x11, 1)) is not null && NeuralHotkeys.Problem(new(0x73, 2)) is { } altF4 && altF4.Contains("Windows"));
    Require(ProfileValidation.Validate(new VrProfile { NeuralToggleKey = "75:0" }).Any(i => i.Code == "neural-hotkey" && i.Severity == IssueSeverity.Error));
    Require(ProfileValidation.Validate(new VrProfile { NeuralToggleKey = "75:7" }).All(i => i.Severity != IssueSeverity.Error || !i.Code.Contains("hotkey")));
    // A DCS default binding is a warning, only while the key is in use; the shipped defaults are free in DCS.
    Require(NeuralHotkeys.IsDcsDefault(new(0x70, 0)) && NeuralHotkeys.IsDcsDefault(new(0x4C, 1)) && !NeuralHotkeys.IsDcsDefault(new(0x7B, 5)) && !NeuralHotkeys.IsDcsDefault(new(0x7B, 6)));
    Require(NeuralHotkeys.All.Values.All(v => !NeuralHotkeys.IsDcsDefault(new(v.VirtualKey, v.Modifiers))), "earlier menu choices are free in DCS");
    // DCS binds physical keys (US names); the warning follows the keyboard layout. On AZERTY the key in DCS's "Z" place
    // types W, "A" types Q, and "M" sits where US has ";": Ctrl+W there is DCS's Ctrl+Z, not Ctrl+W.
    var azerty = new Dictionary<int, int> { [0x10] = 'A', [0x11] = 'Z', [0x1E] = 'Q', [0x2C] = 'W', [0x27] = 'M', [0x32] = 0xBC };
    var us = NeuralHotkeys.DcsDefaultsForLayout(scan => scan switch { 0x10 => 'Q', 0x11 => 'W', 0x1E => 'A', 0x2C => 'Z', 0x27 => 0xBA, 0x32 => 'M', _ => scan + 0x1000 });
    var fr = NeuralHotkeys.DcsDefaultsForLayout(scan => azerty.TryGetValue(scan, out var vk) ? vk : scan + 0x1000);
    Require(NeuralHotkeys.DcsDefaults.Contains("90:1") && NeuralHotkeys.DcsDefaults.Contains("87:1"), "DCS binds Ctrl+Z and Ctrl+W (US)");
    Require(us.Contains("90:1") && fr.Contains("87:1") && NeuralHotkeys.IsDcsDefault(new('W', 1), fr), "DCS's Ctrl+Z is Ctrl+W on AZERTY");
    Require(NeuralHotkeys.DcsDefaults.Contains("77:2") && fr.Contains("188:2") && !fr.Contains("77:2"), "DCS's Alt+M is Alt+, on AZERTY");
    // Layout-independent keys (F-keys, navigation, numpad) stay as they are; unmapped keys are dropped.
    Require(fr.Contains("112:0") && fr.Contains("33:1") && fr.Contains("96:0") && fr.All(c => int.Parse(c.Split(':')[0]) < 0xFF));
    Require(NeuralHotkeys.DcsDefaultsForCurrentLayout().Contains("112:0"), "The current layout's list keeps F1");
    var dcsKey = new VrProfile { NeuralRendering = true, NeuralToggleKey = "112:0" };
    Require(ProfileValidation.Validate(dcsKey).Any(i => i.Code == "neural-hotkey-dcs" && i.Severity == IssueSeverity.Warning && i.Message.StartsWith("F1 ")));
    Require(ProfileValidation.Validate(dcsKey with { NeuralRendering = false }).All(i => i.Code != "neural-hotkey-dcs"));
    // The same key in either spelling clashes; DeploymentPlanner passes recorded keys through unchanged.
    Require(ProfileValidation.Validate(new VrProfile { NeuralRendering = true, FrameGen = FrameGeneration.Nvidia, NeuralToggleKey = "123:6" }).Any(i => i.Code == "hotkey-conflict"));
    Require(ProfileValidation.Validate(new VrProfile { NeuralRendering = true, FrameGen = FrameGeneration.Nvidia, NeuralToggleKey = "Off", DiagnosticOverlayKey = "Off" }).All(i => i.Code != "hotkey-conflict"));
});
Test("The OFXR VRAM counter is off by default and written as diag_vram", () =>
{
    Require(!new VrProfile().DiagnosticVram && ConfigurationWriters.Ofxr(new VrProfile { FrameGen = FrameGeneration.Nvidia }).Contains("\ndiag_vram=0"));
    var on = ConfigurationWriters.Ofxr(new VrProfile { FrameGen = FrameGeneration.Nvidia, DiagnosticVram = true });
    Require(on.Contains("diag_vram=1") && on.IndexOf("diag_vram=1", StringComparison.Ordinal) < on.IndexOf("[diagnostics]", StringComparison.Ordinal), "in the [ofxr] section");
    Require(JsonData.Deserialize<VrProfile>("{\"id\":\"legacy\"}").DiagnosticVram == false);
});
Test("Smooth mouse cursor is off by default, written as smooth_cursor with DCS.exe's folder for the cursor images", () =>
{
    Require(!new VrProfile().SmoothCursor && JsonData.Deserialize<VrProfile>("{\"id\":\"legacy\"}").SmoothCursor == false);
    var off = ConfigurationWriters.Ofxr(new VrProfile { FrameGen = FrameGeneration.Nvidia });
    Require(off.Contains("\nsmooth_cursor=0") && off.Split('\n').Any(l => l.TrimEnd('\r') == "cursor_templates="), "off, no folder");
    var on = ConfigurationWriters.Ofxr(new VrProfile { FrameGen = FrameGeneration.Nvidia, SmoothCursor = true }, @"E:\DCS World\bin");
    Require(on.Contains("smooth_cursor=1") && on.Contains(@"cursor_templates=E:\DCS World\bin")
        && on.IndexOf("smooth_cursor=1", StringComparison.Ordinal) < on.IndexOf("[diagnostics]", StringComparison.Ordinal), "in the [ofxr] section");
    var round = JsonData.Deserialize<VrProfile>(JsonData.Serialize(new VrProfile { SmoothCursor = true }));
    Require(round.SmoothCursor, "kept in the profile");
    // Without DCS's cursor images next to DCS.exe the layer falls back; validation says so, as information only.
    var bin = Path.Combine(root, "smooth-cursor/bin"); Directory.CreateDirectory(bin); var exe = Path.Combine(bin, "DCS.exe"); File.WriteAllText(exe, "fixture");
    var inventory = new InventorySnapshot { DcsExecutable = exe };
    var issues = ProfileValidation.Validate(new VrProfile { FrameGen = FrameGeneration.Nvidia, SmoothCursor = true }, inventory);
    Require(issues.Single(i => i.Code == "smooth-cursor-images").Severity == IssueSeverity.Info);
    File.WriteAllText(Path.Combine(bin, "Visualizer.dll"), "fixture");
    Require(ProfileValidation.Validate(new VrProfile { FrameGen = FrameGeneration.Nvidia, SmoothCursor = true }, inventory).All(i => i.Code != "smooth-cursor-images"));
    Require(ProfileValidation.Validate(new VrProfile { FrameGen = FrameGeneration.Off, SmoothCursor = true }, new InventorySnapshot { DcsExecutable = Path.Combine(root, "smooth-cursor/none/DCS.exe") }).All(i => i.Code != "smooth-cursor-images"));
});
Test("The DCS launcher is refused only inside a job that forbids breakaway, before anything is written", () =>
{
    Require(new LaunchSafety.JobState(true, 0x2000).BlocksBreakaway && !new LaunchSafety.JobState(true, 0x800).BlocksBreakaway
        && !new LaunchSafety.JobState(true, 0x1000).BlocksBreakaway && !new LaunchSafety.JobState(false, 0).BlocksBreakaway);
    var previous = LaunchSafety.CurrentJob;
    var service = new ControlService(workspace, Path.Combine(root, "launcher-job/state"));
    try
    {
        LaunchSafety.CurrentJob = () => new(true, 0x2000);
        Require(LaunchSafety.LauncherRestartBlocked);
        try { service.SyncAndPrepareLaunch(new VrProfile { KeepDcsLauncher = true }, new()); throw new Exception("Not refused"); }
        catch (InvalidOperationException e) { Require(e.Message == LaunchSafety.LauncherBlockedMessage); }
        Require(!Directory.Exists(Path.Combine(root, "launcher-job/state/transactions")) || !Directory.EnumerateDirectories(Path.Combine(root, "launcher-job/state/transactions")).Any());
        // Without the launcher DCS goes straight into the game, which works inside any job: the launch goes on (and here
        // stops later for want of a profile, with another message).
        try { service.SyncAndPrepareLaunch(new VrProfile(), new()); } catch (Exception e) when (e is InvalidOperationException or InvalidDataException or IOException) { Require(e.Message != LaunchSafety.LauncherBlockedMessage); }
        LaunchSafety.CurrentJob = () => new(true, 0x800);
        Require(!LaunchSafety.LauncherRestartBlocked && !LaunchSafety.LauncherRestartUnknown);
        // Limits that cannot be read (QueryInformationJobObject failed): "unknown", a warning in Checks, never a block.
        LaunchSafety.CurrentJob = () => new(true, 0, Unknown: true);
        Require(!LaunchSafety.LauncherRestartBlocked && LaunchSafety.LauncherRestartUnknown && !new LaunchSafety.JobState(true, 0, true).BlocksBreakaway);
        var checks = Readiness.Check(new VrProfile { KeepDcsLauncher = true }, new(), service);
        Require(checks.Checks.Any(c => c.Id == "dcs-launcher" && c.State == CheckState.Warning && c.Detail == LaunchSafety.LauncherUnknownMessage), "Warned in Checks");
        try { service.SyncAndPrepareLaunch(new VrProfile { KeepDcsLauncher = true }, new()); } catch (Exception e) when (e is InvalidOperationException or InvalidDataException or IOException) { Require(e.Message != LaunchSafety.LauncherBlockedMessage, e.Message); }
    }
    finally { LaunchSafety.CurrentJob = previous; }
});
Test("The draft is kept across restarts and a damaged file is ignored", () =>
{
    var store = new DraftStore(Path.Combine(root, "draft-store"));
    Require(store.Load() is null);
    var draft = new VrProfile { Id = "pimax-qv-dlss5", NeuralRendering = true, QuadViews = QuadProvider.PimaxNative, QuadSharpening = .456789123456, NeuralToggleKey = "75:7" };
    store.Save(draft, @"D:\DCS\bin\DCS.exe", null);
    var loaded = store.Load()!;
    Require(loaded.Dcs == @"D:\DCS\bin\DCS.exe" && loaded.Options is null && loaded.Profile.QuadSharpening == .456789123456 && loaded.Profile.NeuralToggleKey == "75:7");
    Require(loaded.Profile.QuadViews == QuadProvider.QuadViewsFoveated && loaded.Profile.QuadFocusAdapter, "settled like the checklist");
    File.WriteAllText(store.Path, "{ not json"); Require(store.Load() is null);
});
Test("Last flight: dcs.log session, Pimax frames, OFXR modes and frame time, DLSS 5, CPU Boost and the engine prefetch fix", () =>
{
    var start = new DateTimeOffset(2026, 10, 5, 17, 37, 28, TimeSpan.Zero);
    var invariant = System.Globalization.CultureInfo.InvariantCulture;
    string L(DateTimeOffset t, string format) => t.ToLocalTime().ToString(format, invariant);
    var session = LastFlight.ParseDcsSession("=== Log opened UTC 2026-10-05 17:37:28\n2026-10-05 17:37:30.358 INFO    APP (Main): Command line: \"DCS.exe\"\n",
        "2026-10-05 17:43:12.879 INFO    DX11BACKEND (31660): total_size: 0\n2026-10-05 17:43:12.892 INFO    VISUALIZER (31660): render thread has stopped\n=== Log closed.\n", start.AddHours(1));
    Require(session is { Closed: true } s1 && s1.Start == start && s1.End == new DateTimeOffset(2026, 10, 5, 17, 43, 12, 892, TimeSpan.Zero));
    Require(LastFlight.ParseDcsSession("no header", "", start) is null);
    var crashed = LastFlight.ParseDcsSession("=== Log opened UTC 2026-10-05 17:37:28\n", "2026-10-05 17:40:00.000 INFO x\n", start.AddHours(1));
    Require(crashed is { Closed: false } c1 && c1.End == start.AddSeconds(152));
    // Pimax: the DCS process's lines inside the session; loading seconds (under 1 FPS) and other processes are skipped.
    string P(DateTimeOffset t, int pid, double a, double c, int missed, int discard) =>
        $"[{L(t, "yy-MM-dd HH:mm:ss.fff")}][26424][info][PSRV] {pid} Warning rendering fps:(a:{a.ToString(invariant)},c:{c.ToString(invariant)}) Missed:(a:{missed},c:0,bl:0) Stale:(a:0) Discard:(a:{discard}) GPU: 65% CPU: 97%";
    var pimax = new[] { P(start.AddSeconds(-30), 0, 90, 90, 0, 0), P(start.AddSeconds(10), 24208, .04437, 88.99, 87, 0), P(start.AddSeconds(60), 24208, 90.013079, 89.995608, 28, 28),
        P(start.AddSeconds(61), 24208, 75.637003, 89.999387, 28, 13), P(start.AddSeconds(62), 999, 30, 90, 60, 0), "[26-10-05 19:57:29.656][32656][info][PSRV] hmd: 493.147220 FPS" };
    var frames = LastFlight.ParsePimax(pimax, start, start.AddMinutes(5), 24208)!;
    Require(frames.Seconds == 2 && Math.Abs(frames.AverageFps - 82.825041) < 1e-4 && frames.Missed == 56 && frames.Discarded == 41 && frames.AtRefreshShare == .5);
    Require(LastFlight.ParsePimax(pimax, start, start.AddMinutes(5), null)!.Seconds == 3, "any non-zero process without a PID");
    // OFXR: once-a-second latency_status (multiplier + 10 adaptive, a = p90 frame cost in µs) and adaptive switches.
    var ofxr = LastFlight.ParseOfxr([
        "seq=14 ms=51876.732 tid=31660 phase=I op=adaptive_switch result=5 dur_us=0 a=0 b=2 c=0",
        "seq=84 ms=100545.118 tid=31660 phase=I op=latency_status result=12 dur_us=0 a=0 b=0 c=0",
        "seq=2321 ms=119052.254 tid=31660 phase=I op=adaptive_switch result=2 dur_us=0 a=2 b=3 c=24581",
        "seq=230013 ms=306904.592 tid=31660 phase=I op=latency_status result=13 dur_us=0 a=27409 b=31 c=44444",
        "seq=232125 ms=307905.616 tid=31660 phase=I op=latency_status result=13 dur_us=0 a=28206 b=30 c=44444",
        "seq=232126 ms=308905.616 tid=31660 phase=I op=latency_status result=12 dur_us=0 a=20000 b=30 c=44444",
        "seq=9345 ms=123974.375 tid=31660 phase=I op=adaptive_switch result=1 dur_us=0 a=3 b=2 c=6752",
        "seq=429 ms=112503.069 tid=26532 phase=I op=submission_timing result=1 dur_us=0 a=196610 b=131072 c=19036828363629029"])!;
    Require(ofxr.Share is { Seconds2x: 2, Seconds3x: 2, Switches: 2, Adaptive: true } && ofxr.Times is { P50Ms: 27.4, P90Ms: 28.2, Seconds: 3 });
    Require(LastFlight.ParseOfxr(["seq=1 ms=0.000 tid=27164 phase=I op=logger result=401 dur_us=0 a=128 b=0 c=24208"]) is null);
    // Adaptive cadence: 11 is 1X (full rate, generation paused); 8 enters 1X and 9-12 leave it; 13 turns cadence on.
    var cadence = LastFlight.ParseOfxr(["seq=1 ms=0 tid=1 phase=I op=adaptive_switch result=13 dur_us=0 a=0 b=2 c=0",
        "seq=2 ms=1000 tid=1 phase=I op=latency_status result=12 dur_us=0 a=20000 b=0 c=0", "seq=3 ms=1500 tid=1 phase=I op=adaptive_switch result=8 dur_us=0 a=2 b=1 c=7000",
        "seq=4 ms=2000 tid=1 phase=I op=latency_status result=11 dur_us=0 a=8000 b=0 c=0", "seq=5 ms=3000 tid=1 phase=I op=latency_status result=11 dur_us=0 a=8100 b=0 c=0",
        "seq=6 ms=3500 tid=1 phase=I op=adaptive_switch result=11 dur_us=0 a=1 b=2 c=0"])!;
    Require(cadence.Share is { Seconds1x: 2, Seconds2x: 1, Seconds3x: 0, Switches: 2, Adaptive: true }, "1X seconds and switches in and out of it");
    // Cheeky: the runtime came up, the hotkey toggled it twice; a failure is reported as such.
    var nr = LastFlight.ParseCheeky(["19:41:40.836 [T31660] DLSS-NR runtime search path=E:\\DCS\\bin\\CheekyFoveatedDLSS\\nvngx_dlssnr.dll error=0", "19:41:40.836 [T31660] DLSS-NR 310.8 feature-18 runtime initialized"],
        ["19:42:56.971 DLSS-NR hotkey toggled enabled=no", "19:43:00.551 DLSS-NR hotkey toggled enabled=yes"]);
    Require(nr is { State: "ran", Version: "310.8", Toggles: 2, OnAtExit: true, Error: null });
    Require(LastFlight.ParseCheeky(["19:41:40.836 [T31660] DLSS-NR feature creation failed error=-1160773628"], []) is { State: "failed", Error: { } e1 } && e1.StartsWith("DLSS-NR"));
    Require(LastFlight.ParseCheeky(["19:41:41.217 [T31660] DLSS-NR history view=1 calls=1 compensationFailures=0"], []).State == "not-started");
    // Both Cheeky logs are appended to by every session with the time of day only (as in DCS's bin\CheekyFoveatedDLSS):
    // only the lines after the last startup marker belong to the last flight.
    var sessions = LastFlight.ParseCheeky([
        "18:02:11.100 [T1200] Cheeky 0.5.3 Standalone initializing; runtime remains resident until game exit",
        "18:02:40.836 [T1200] DLSS-NR 310.8 feature-18 runtime initialized",
        "19:37:24.334 [T26404] Cheeky 0.5.4 Standalone initializing; runtime remains resident until game exit",
        "19:41:40.836 [T31660] DLSS-NR feature creation failed error=-1160773628"],
        ["18:02:11.000 Initializing standalone host", "18:05:00.000 DLSS-NR hotkey toggled enabled=no", "18:06:00.000 DLSS-NR hotkey toggled enabled=yes", "18:07:00.000 DLSS-NR hotkey toggled enabled=no",
         "19:37:24.332 Initializing standalone host", "19:37:25.729 Host ready; waiting for game graphics. F8 opens settings."]);
    Require(sessions is { State: "failed", Version: null, Toggles: 0, OnAtExit: null }, "The earlier session's runtime start and toggles are not this flight's");
    var earlier = LastFlight.ParseCheeky(["18:02:11.100 [T1200] Cheeky 0.5.3 Standalone initializing; runtime remains resident until game exit", "18:02:12.000 [T1200] DLSS-NR feature creation failed error=-1",
        "19:37:24.334 [T26404] Cheeky 0.5.3 Standalone initializing; runtime remains resident until game exit", "19:41:40.836 [T31660] DLSS-NR 310.8 feature-18 runtime initialized"],
        ["18:02:11.000 Initializing standalone host", "18:05:00.000 DLSS-NR hotkey toggled enabled=no", "19:37:24.332 Initializing OptiScaler host", "19:42:56.971 DLSS-NR hotkey toggled enabled=yes"]);
    Require(earlier is { State: "ran", Version: "310.8", Toggles: 1, OnAtExit: true, Error: null }, "An earlier session's failure is not this flight's");
    // Malformed or out-of-range numbers in any log are skipped, never thrown.
    Require(LastFlight.ParsePimax([P(start.AddSeconds(60), 24208, 90, 90, 0, 0).Replace("Missed:(a:0", "Missed:(a:99999999999999999999"), P(start.AddSeconds(61), 24208, 90, 90, 1, 0).Replace("a:90,", "a:9.0.0,")], start, start.AddMinutes(5), null) is null);
    Require(LastFlight.ParseOfxr(["seq=1 ms=0 tid=1 phase=I op=latency_status result=99999999999999999999 dur_us=0 a=1 b=0 c=0", "seq=2 ms=0 tid=1 phase=I op=latency_status result=12 dur_us=0 a=99999999999999999999999 b=0 c=0"]) is null);
    Require(LastFlight.ParseBoostStatus("{\"updatedAt\":\"2026-10-05T17:43:18+00:00\",\"dcsPid\":1.5,\"restored\":1e30,\"state\":\"restored\"}") is { DcsPid: null, Result.Restored: 0 });
    // CPU Boost status.json (an older one still naming the prefetch fix) and the engine prefetch fix's 10 s reports.
    var boost = LastFlight.ParseBoostStatus("{\"schemaVersion\":1,\"state\":\"restored\",\"updatedAt\":\"2026-10-05T17:43:18.0943102+00:00\",\"dcsPid\":24208,\"changed\":[\"DCS (PID 24208): priority AboveNormal\",\"DCS (PID 24208): affinity all CPUs 0xFFFF\",\"PimaxClient (PID 31476): Normal, affinity 0xFC03\",\"steamwebhelper (PID 32368): BelowNormal, affinity 0xC000\"],\"closed\":[],\"restored\":2,\"errors\":[\"Tobii.Service (PID 4616): Access is denied. (it may need the administrator rights option)\"],\"prefetch\":\"skip (loaded by DCS, see bin\\\\DcsVrPrefetchFix.log)\"}")!;
    Require(boost.DcsPid == 24208 && boost.Result is { State: "restored", Moved: 2, Restored: 2, Errors.Count: 1 });
    Require(LastFlight.ParseBoostStatus("{broken") is null && LastFlight.ParseBoostStatus("{\"state\":\"running\"}") is null);
    string F(DateTimeOffset t, string rest) => L(t, "yyyy-MM-dd HH:mm:ss") + " " + rest;
    var prefetch = LastFlight.ParsePrefetch([F(start.AddSeconds(-4), "prefetch fix started mode=skip window_ms=5000"), F(start.AddSeconds(6), "calls/s=0 skipped=0.0% pointers=1 imports=18 lookups=0 faults=0"),
        F(start.AddSeconds(256), "calls/s=22239 skipped=84.2% pointers=1 imports=40 lookups=0 faults=0"), F(start.AddSeconds(266), "calls/s=108385 skipped=98.3% pointers=1 imports=41 lookups=0 faults=0"),
        F(start.AddDays(-1), "calls/s=99999 skipped=0.0% pointers=1 imports=41 lookups=0 faults=0")], start, start.AddMinutes(6))!;
    Require(prefetch.Reports == 3 && prefetch.PeakCallsPerSecond == 108385 && prefetch.AverageCallsPerSecond == 65312 && prefetch.SkippedPercent == 95.9);
    // The engine module's 10 s reports: a frame rate held by the headset runtime (Virtual Desktop at 72 Hz / 2). Only
    // the last DCS start counts; the rate comes from the report or the governor line; malformed lines are skipped.
    var cap = LastFlight.ParseFrameCap(["08:00:00.000 DcsQvCull loader started from x", "08:00:10.000 perf: fps=80.0 ft p50/p95=12.0/13.0 ms render-thread busy=99% gpu util=60% (dcs 55%) verdict=CPU (0 s GPU, 10 CPU, 0 capped, 0 mixed of 10) engine=on",
        "08:07:14.884 DcsQvCull loader started from DcsQvCull.dll",
        "08:12:49.741 perf: fps=45.9 ft p50/p95=15.4/24.4 ms render-thread busy=78% gpu util=67% (dcs 62%) verdict=mixed (1 s GPU, 0 CPU, 1 capped, 4 mixed of 10) engine=on governor=0 views=4 pairs=2",
        "08:13:00.302 perf: fps=41.7 ft p50/p95=22.5/32.1 ms render-thread busy=62% gpu util=80% (dcs 72%) verdict=capped (1 s GPU, 0 CPU, 5 capped, 4 mixed of 10) engine=on governor=0 views=4 pairs=2",
        "08:13:01.357 governor: capped at 36.0 fps (72 Hz / 2): governor holding stage 0",
        "08:13:10.807 perf: fps=35.4 ft p50/p95=27.9/28.5 ms render-thread busy=53% gpu util=70% (dcs 63%) verdict=capped 36.0 fps = 72/2 (0 s GPU, 0 CPU, 10 capped, 0 mixed of 10) engine=on governor=0 views=4 pairs=2",
        "08:13:15.000 perf: fps=60.0 ft p50/p95=16.0/17.0 ms render-thread busy=99% gpu util=60% (dcs 55%) verdict=CPU (0 s GPU, 10 CPU, 0 capped, 0 mixed of 10) engine=on",
        "08:13:20.000 perf: fps=x verdict=", "garbage"])!;
    Require(cap is { Fps: 36, RefreshHz: 72, CappedReports: 2, Reports: 4, Divider: 2 }, "capped share and rate; CPU and GPU reports count");
    Require(LastFlight.ParseFrameCap(["08:13:01.357 governor: capped at 36,0 fps (72 Hz / 2): holding", "08:13:10.807 perf: fps=35,4 ft p50/p95=27,9/28,5 ms verdict=capped 36,0 fps = 72/2 (0 s GPU)"]) is { Fps: 36, RefreshHz: 72, Divider: 2, CappedReports: 1 }, "comma decimals of older payloads");
    Require(LastFlight.ParseFrameCap(["08:13:01.357 governor: capped at 1e999 fps (72 Hz / 2)", "08:13:10.807 perf: fps=35.4 ft p50/p95=27.9/28.5 ms verdict=capped (0 s GPU)"]) is { Fps: null, CappedReports: 1, Reports: 1 }, "no named rate");
    Require(LastFlight.ParseFrameCap(["garbage", "08:00:00.000 DcsQvCull loader started from x"]) is null, "no reports");
    // Quad Views' eye tracking line: the last DCS start in the session counts; one before it or outside it does not.
    string Q(DateTimeOffset t, string text) => t.ToString("yyyy-MM-dd HH:mm:ss zzz", System.Globalization.CultureInfo.InvariantCulture).Remove(t.ToString("yyyy-MM-dd HH:mm:ss zzz", System.Globalization.CultureInfo.InvariantCulture).Length - 3, 1) + ": " + text;
    Require(LastFlight.ParseEyeTracking([Q(start.AddSeconds(5), "Quad-Views-Foveated layer (v1.1.3) is active"), Q(start.AddSeconds(6), "Eye tracking is not supported")], start, start.AddMinutes(6)) is { Supported: false }, "not supported");
    Require(LastFlight.ParseEyeTracking([Q(start.AddSeconds(5), "Quad-Views-Foveated layer (v1.1.3) is active"), Q(start.AddSeconds(6), "Eye tracking is not supported"),
        Q(start.AddSeconds(90), "Quad-Views-Foveated layer (v1.1.3) is active"), Q(start.AddSeconds(91), "Eye tracking is supported")], start, start.AddMinutes(6)) is { Supported: true }, "the last start counts");
    Require(LastFlight.ParseEyeTracking([Q(start.AddDays(-1), "Eye tracking is not supported"), "garbage"], start, start.AddMinutes(6)) is null, "an older flight's line is not this one");
    // Read: every source found on disk, matched to the session by time and the DCS PID.
    var flight = Path.Combine(root, "last-flight");
    var dcsLog = Make("last-flight/Saved Games/DCS/Logs/dcs.log", "=== Log opened UTC 2026-10-05 17:37:28\n2026-10-05 17:37:30.358 INFO    APP (Main): Command line: \"DCS.exe\"\n2026-10-05 17:43:12.892 INFO    VISUALIZER (31660): render thread has stopped\n=== Log closed.\n");
    Require(LastFlight.Read(new(Path.Combine(flight, "missing.log"))) is null);
    Make("last-flight/Pimax/runtime/pvr_srv_log_" + L(start.AddMinutes(-8), "yy-MM-dd-HH-mm-ss") + ".txt", string.Join("\n", pimax));
    Make($"last-flight/managed/profiles/pimax-qv/ofxr/ofxr-bridge-flight-{L(start.AddSeconds(29), "yyyyMMdd-HHmmss")}-pid24208.log",
        "seq=1 ms=0.000 tid=27164 phase=I op=logger result=401 dur_us=0 a=128 b=0 c=24208\nseq=230013 ms=306904.592 tid=31660 phase=I op=latency_status result=12 dur_us=0 a=13000 b=31 c=44444\n");
    // OFXR 0046: the segment rotated out at the size cap (.prev, same process in its header) counts too, never as the log itself.
    Make($"last-flight/managed/profiles/pimax-qv/ofxr/ofxr-bridge-flight-{L(start.AddSeconds(29), "yyyyMMdd-HHmmss")}-pid24208.prev.log",
        "seq=1 ms=0.000 tid=27164 phase=I op=logger result=401 dur_us=0 a=128 b=0 c=24208\r\nseq=1200 ms=120000.000 tid=31660 phase=I op=latency_status result=13 dur_us=0 a=13000 b=31 c=44444\r\n");
    {
        var pruneRoot = Path.Combine(root, "ofxr-prune/profiles");
        for (var i = 0; i < 5; i++) Make($"ofxr-prune/profiles/{(i % 2 == 0 ? "a" : "b")}/ofxr/ofxr-bridge-flight-2026101{i}-120000-pid{100 + i}.log", "x");
        Make("ofxr-prune/profiles/a/ofxr/ofxr-bridge-flight-20261010-120000-pid100.prev.log", "x");
        Make("ofxr-prune/profiles/a/ofxr/other.log", "x");
        Require(LastFlight.PruneOfxrLogs(pruneRoot, keep: 3) == 3, "the two oldest logs and the .prev of one");
        var left = Directory.EnumerateFiles(pruneRoot, "*", SearchOption.AllDirectories).Select(Path.GetFileName).Order().ToArray();
        Require(left.SequenceEqual(["ofxr-bridge-flight-20261012-120000-pid102.log", "ofxr-bridge-flight-20261013-120000-pid103.log", "ofxr-bridge-flight-20261014-120000-pid104.log", "other.log"]), string.Join(",", left));
    }
    {
        // Smooth cursor captures: whole processes go, oldest first; the newest process and other files always stay.
        var captureRoot = Path.Combine(root, "capture-prune/profiles");
        var pids = new[] { 300, 100, 200 };
        for (var i = 0; i < pids.Length; i++)
            foreach (var kind in new[] { "previous.bmp", "current.bmp", "overview.bmp", "info.txt" })
                for (var n = 1; n <= 2; n++)
                    File.SetLastWriteTimeUtc(Make($"capture-prune/profiles/{(i == 2 ? "b" : "a")}/ofxr/cursor-capture-{pids[i]}-{n}-{kind}", "x"), new DateTime(2026, 10, 10 + i, 12, n, 0, DateTimeKind.Utc));
        Make("capture-prune/profiles/a/ofxr/cursor-capture-notes.txt", "x");
        Make("capture-prune/profiles/a/ofxr/ofxr-bridge-flight-20261010-120000-pid300.log", "x");
        Require(LastFlight.PruneCursorCaptures(captureRoot, keep: 2) == 8, "the oldest process's 8 files");
        var left = Directory.EnumerateFiles(captureRoot, "*", SearchOption.AllDirectories).Select(Path.GetFileName).ToArray();
        Require(left.Length == 18 && !left.Any(f => f!.StartsWith("cursor-capture-300-")) && left.Contains("cursor-capture-notes.txt") && left.Contains("ofxr-bridge-flight-20261010-120000-pid300.log"), string.Join(",", left));
        Require(LastFlight.PruneCursorCaptures(captureRoot, keep: 0) == 8 && Directory.EnumerateFiles(captureRoot, "cursor-capture-200-*", SearchOption.AllDirectories).Count() == 8, "keep 0 still keeps the newest process");
        Require(LastFlight.PruneCursorCaptures(Path.Combine(root, "capture-prune/missing")) == 0);
    }
    {
        // Prepared Sboys tools: a week old and not the newest go, unless the tool is running; previews go after the hour.
        var cache = new ComponentCache(Path.Combine(root, "staging-prune"));
        string Staged(string name, double days) { var d = Path.Combine(cache.DirectoryPath, "staging", name); Make(Path.GetRelativePath(root, Path.Combine(d, "tool.exe")), "x"); Directory.SetCreationTimeUtc(d, DateTime.UtcNow.AddDays(-days)); return d; }
        var oldest = Staged("sboys-old", 30); var open = Staged("sboys-open", 20); var recent = Staged("sboys-recent", 2); var newest = Staged("sboys-newest", 1);
        var preview = Staged("ofxr-preview", 1); var fresh = Staged("cheeky-fresh", 0);
        using (new FileStream(Path.Combine(open, "tool.exe"), FileMode.Open, FileAccess.Read, FileShare.Read)) cache.RemoveStaleStaging(TimeSpan.FromHours(1));
        Require(!Directory.Exists(oldest) && Directory.Exists(open) && Directory.Exists(recent) && Directory.Exists(newest) && !Directory.Exists(preview) && Directory.Exists(fresh), "staging prune");
        cache.RemoveStaleStaging(TimeSpan.FromHours(1), TimeSpan.Zero);
        Require(!Directory.Exists(open) && !Directory.Exists(recent) && Directory.Exists(newest), "the newest prepared tool always stays");
    }
    Require(LastFlight.OfxrPreviousSegment(Path.Combine("x", "ofxr-bridge-flight-20261005-193758-pid24208.log")) ==Path.Combine("x", "ofxr-bridge-flight-20261005-193758-pid24208.prev.log"));
    Require(LastFlight.OfxrHeaderPid("seq=1 ms=0.000 tid=1 phase=I op=logger result=401 dur_us=0 a=256 b=0 c=24208\r\nseq=2 ms=0.1 tid=1 phase=I op=latency_status result=12 dur_us=0 a=1 b=0 c=24208\r\n") == 24208
        && LastFlight.OfxrHeaderPid("seq=1 ms=0.000 tid=1 phase=I op=latency_status result=12 dur_us=0 a=1 b=0 c=24208\nseq=2 ms=0.000 tid=1 phase=I op=logger result=401 dur_us=0 a=256 b=0 c=24208\n") is null
        && LastFlight.OfxrHeaderPid("") is null && LastFlight.OfxrHeaderPid("seq=1 phase=I op=logger result=401 dur_us=0 a=256 b=0 c=99999999999\n") is null, "header record pid");
    Make($"last-flight/managed/profiles/older/ofxr/ofxr-bridge-flight-{L(start.AddDays(-1), "yyyyMMdd-HHmmss")}-pid18932.log", "seq=1 ms=0 tid=1 phase=I op=latency_status result=13 dur_us=0 a=50000 b=0 c=0\n");
    var status = Make("last-flight/boost/status.json", "{\"state\":\"restored\",\"updatedAt\":\"2026-10-05T17:43:18+00:00\",\"dcsPid\":24208,\"changed\":[\"PimaxClient (PID 1): Normal, affinity 0xFC03\"],\"restored\":1,\"errors\":[]}");
    var bin = Path.Combine(flight, "DCSWorld/bin");
    // The prefetch fix's log is DcsQvCullPrefetch.log in the engine optimizations module next to dcs.log's Saved Games
    // folder; the 0.5.4 bin\DcsVrPrefetchFix.log is no longer read.
    Make("last-flight/Saved Games/DCS/Scripts/DcsQvCull/DcsQvCullPrefetch.log", F(start.AddSeconds(256), "calls/s=1000 skipped=50.0% pointers=1 imports=40 lookups=0 faults=0"));
    Make("last-flight/DCSWorld/bin/DcsVrPrefetchFix.log", F(start.AddSeconds(256), "calls/s=1000 skipped=10.0% pointers=1 imports=40 lookups=0 faults=0"));
    Require(LastFlight.PrefetchLog(dcsLog) == Path.GetFullPath(Path.Combine(flight, "Saved Games/DCS/Scripts/DcsQvCull/DcsQvCullPrefetch.log")));
    // Two sessions in the same append-only log: the earlier one failed, the last one ran.
    var cheeky = Make("last-flight/DCSWorld/bin/CheekyFoveatedDLSS/CheekyFoveatedDLSS-Standalone.log", "17:02:11.100 [T1200] Cheeky 0.5.3 Standalone initializing; runtime remains resident until game exit\n"
        + "17:02:12.000 [T1200] DLSS-NR feature creation failed error=-1\n19:37:24.334 [T26404] Cheeky 0.5.3 Standalone initializing; runtime remains resident until game exit\n"
        + "19:37:24.334 [T26404] Interception startup begin\n19:41:40.836 [T31660] DLSS-NR 310.8 feature-18 runtime initialized\n");
    File.SetLastWriteTimeUtc(cheeky, start.AddMinutes(5).UtcDateTime); File.SetLastWriteTimeUtc(dcsLog, start.AddMinutes(6).UtcDateTime);
    var read = LastFlight.Read(new(dcsLog, Path.Combine(flight, "Pimax/runtime"), Path.Combine(flight, "managed/profiles"), status, bin))!;
    Require(read.Closed && read.DcsPid == 24208 && read.Duration == TimeSpan.FromSeconds(344.892) && read.Headset!.Seconds == 2 && read.Framegen is { Seconds2x: 1, Seconds3x: 1 }
        && read.FrameTime!.P50Ms == 13 && read.Dlss5 is { State: "ran", Version: "310.8", Error: null } && read.Boost!.Moved == 1 && read.Prefetch!.SkippedPercent == 50);
    // A Boost status or Cheeky log from another session is not this flight's.
    File.SetLastWriteTimeUtc(cheeky, start.AddDays(-1).UtcDateTime);
    File.WriteAllText(status, "{\"state\":\"restored\",\"updatedAt\":\"2026-10-04T10:00:00+00:00\",\"dcsPid\":1,\"changed\":[],\"restored\":0,\"errors\":[]}");
    var other = LastFlight.Read(new(dcsLog, null, Path.Combine(flight, "managed/profiles"), status, bin))!;
    Require(other.Boost is null && other.Dlss5 is null && other.Headset is null && other.DcsPid == 24208);
    // A .prev segment whose header names another process is not this flight's.
    File.WriteAllText(Path.Combine(flight, $"managed/profiles/pimax-qv/ofxr/ofxr-bridge-flight-{L(start.AddSeconds(29), "yyyyMMdd-HHmmss")}-pid24208.prev.log"),
        "seq=1 ms=0.000 tid=27164 phase=I op=logger result=401 dur_us=0 a=128 b=0 c=1\r\nseq=1200 ms=120000.000 tid=31660 phase=I op=latency_status result=13 dur_us=0 a=13000 b=31 c=44444\r\n");
    Require(LastFlight.Read(new(dcsLog, null, Path.Combine(flight, "managed/profiles"), status, bin))!.Framegen is { Seconds2x: 1, Seconds3x: 0 }, "another process's .prev segment");
});
Test("A restore resumed after a conflict removes the empty folder of a created file, never the root", () =>
{
    var bin = Path.Combine(root, "resume-folder/bin"); Directory.CreateDirectory(bin);
    var folder = Path.Combine(bin, "Component"); var file = Path.Combine(folder, "settings.ini");
    var store = new TransactionStore(Path.Combine(root, "journals/resume-folder"));
    var journal = store.Apply(new("p", "p", [new FileMutation(file, null, Encoding.UTF8.GetBytes("[A]\nX=1\n"), "test", Root: bin)], new Dictionary<string, string>(), "fixture"));
    File.WriteAllText(file, "edited by the component");
    Require(!store.Restore(journal.Id).Complete && File.Exists(file));
    File.WriteAllText(file, "[A]\nX=1\n");
    Require(store.Restore(journal.Id).Complete && !Directory.Exists(folder) && Directory.Exists(bin));
});
Test("Transaction rejects duplicate mutations", () => { var f = new FileMutation(Path.Combine(root, "same"), null, [1], "test"); Throws<InvalidDataException>(() => new TransactionStore(Path.Combine(root, "journals/duplicates")).Apply(new("test", "test", [f, f], new Dictionary<string, string>(), "fixture"))); });
Test("Per-process OpenXR environment reaches child, parent unchanged", () =>
{
    var parent = Environment.GetEnvironmentVariable("XR_RUNTIME_JSON"); var launch = new LaunchConfiguration(Environment.ProcessPath!, new() { ["XR_RUNTIME_JSON"] = "fixture-runtime.json", ["XR_ENABLE_API_LAYERS"] = "layer-a;layer-b" });
    var start = ControlService.BuildStartInfo(launch);
    if (Path.GetFileNameWithoutExtension(Environment.ProcessPath!).Equals("dotnet", StringComparison.OrdinalIgnoreCase)) start.ArgumentList.Add(System.Reflection.Assembly.GetExecutingAssembly().Location);
    start.ArgumentList.Add("--environment-child"); start.RedirectStandardOutput = true; start.CreateNoWindow = true;
    using var child = Process.Start(start)!; var output = child.StandardOutput.ReadToEnd(); Require(child.WaitForExit(10000)); Require(child.ExitCode == 0); Require(output == "fixture-runtime.json|layer-a;layer-b", output); Require(Environment.GetEnvironmentVariable("XR_RUNTIME_JSON") == parent);
});
Test("Unsigned malformed DLL rejected", () => { var path = Make("binary/fake.dll", "MZ"); var v = NativeBinary.Inspect(path); Require(!v.IsX64 && !v.TrustedSignature); });
Test("Installed NVIDIA DLSS signature checked offline", () =>
{
    var inv = new WindowsInventory().Capture(); if (inv.DcsExecutable is null) return;
    var v = NativeBinary.Inspect(Path.Combine(Path.GetDirectoryName(inv.DcsExecutable)!, "nvngx_dlss.dll")); Require(v.IsX64); Require(v.TrustedSignature); Require(NativeBinary.IsNvidiaSigner(v.Signer));
});
Test("Baseline and framegen installation in isolated DCS fixture", () =>
{
    var exe = Make("fixture/bin/DCS.exe", "fixture"); var options = Make("fixture/options.lua", Lua); var runtime = Make("fixture/runtime.json", "{\"runtime\":{\"library_path\":\"runtime.dll\"}}"); Make("fixture/runtime.dll", "fixture");
    var inv = new InventorySnapshot { DcsExecutable = exe, OptionsPath = options, PimaxRuntime = runtime }; var before = Hashing.FileSha256(options);
    var ofxr = new ComponentCache(Path.Combine(root, "cache-plan")).Import(PackageCatalog.Get("ofxr"), Path.Combine(workspace, ".cache/packages", PackageCatalog.Get("ofxr").ArchiveName));
    var planner = new DeploymentPlanner(new(ofxr, null, null)); var p = new VrProfile { FrameGen = FrameGeneration.Nvidia };
    var plan = planner.Build(p, inv, Path.Combine(root, "fixture/managed")); Require(Hashing.FileSha256(options) == before); Require(plan.LaunchEnvironment["XR_ENABLE_API_LAYERS"] == "XR_APILAYER_XRFrameBridge_diagnostic");
    Require(plan.LaunchEnvironment[VectorXr.DisableVariable] == "1", "VectorXR stays off while DCS runs with the profile's own layers");
    var store = new OriginalsStore(Path.Combine(root, "originals/full-plan")); store.Apply(plan); Require(new LuaOptions(File.ReadAllText(options)).Get("VR", "enable") == "true"); Require(store.RestoreOriginals().Complete); Require(Hashing.FileSha256(options) == before);
});
var failed = new List<string>(); var timer = Stopwatch.StartNew();
Test("Software Quad Views plus framegen deploys per-process configuration", () =>
{
    var exe = Make("software-quad/bin/DCS.exe", "fixture"); var options = Make("software-quad/options.lua", Lua); var runtime = Make("software-quad/runtime.json", "{\"runtime\":{\"library_path\":\"runtime.dll\"}}"); Make("software-quad/runtime.dll", "fixture");
    var ofxr = new ComponentCache(Path.Combine(root, "software-quad/cache")).Import(PackageCatalog.Get("ofxr"), Path.Combine(workspace, ".cache/packages", PackageCatalog.Get("ofxr").ArchiveName));
    var p = new VrProfile { Runtime = RuntimeKind.SboysSteamVr, Tracking = TrackingKind.Lighthouse, QuadViews = QuadProvider.QuadViewsFoveated, FrameGen = FrameGeneration.Nvidia, Gaze = GazeMode.Fixed };
    var inv = new InventorySnapshot { DcsExecutable = exe, OptionsPath = options, SteamVrRuntime = runtime };
    var quadPlanner = new DeploymentPlanner(new(ofxr, null, null, Path.Combine(workspace, "external/quadviews/bin/x64/Release"), OfxrLayerDll: DeferredOfxr));
    var plan = quadPlanner.Build(p, inv, Path.Combine(root, "software-quad/managed"));
    Require(plan.LaunchEnvironment["XR_ENABLE_API_LAYERS"] == "XR_APILAYER_MBUCCHIA_quad_views_foveated;XR_APILAYER_XRFrameBridge_diagnostic");
    Require(plan.LaunchEnvironment.ContainsKey("DCSVR_QUAD_SETTINGS")); Require(plan.Files.Any(f => f.Purpose == "Software Quad Views provider"));
    // Quad Views composites its four views; the fork build creates private swapchains only for the submitted pair.
    Require(Hashing.BytesSha256(plan.Files.Single(f => f.Path.EndsWith("XR_APILAYER_XRFrameBridge_diagnostic.dll")).Content) == Hashing.FileSha256(DeferredOfxr));
    Require(new StreamReader(new MemoryStream(plan.Files.Single(f => f.Path.EndsWith("ofxr_bridge.ini")).Content), Encoding.UTF8, true).ReadToEnd().Contains("d3d11_bridge=1"));
    // 0010: the smooth cursor's images are read from DCS.exe's own folder when the game has not loaded them.
    var ofxrIni = new StreamReader(new MemoryStream(plan.Files.Single(f => f.Path.EndsWith("ofxr_bridge.ini")).Content), Encoding.UTF8, true).ReadToEnd();
    Require(ofxrIni.Contains("smooth_cursor=0") && ofxrIni.Contains("cursor_templates=" + Path.GetDirectoryName(Path.GetFullPath(exe))), "cursor_templates is DCS.exe's folder");
    // Periphery stability reaches the bundled Quad Views' per-process settings: off by default, on with the profile.
    string QuadSettings(ApplyPlan built) => Encoding.UTF8.GetString(built.Files.Single(f => f.Purpose == "Profile Quad Views settings").Content).ReplaceLineEndings("\n");
    Require(QuadSettings(plan).Contains("\ndcsvr_temporal=0\n"));
    var stable = QuadSettings(quadPlanner.Build(p with { PeripheryStability = true, PeripheryStabilityStrength = .25 }, inv, Path.Combine(root, "software-quad/managed-stable")));
    Require(stable.Contains("\ndcsvr_temporal=1\n") && stable.Contains("\ndcsvr_temporal_tau_ms=20\n"), "strength 0.25 is a 20 ms time constant");
    var store = new OriginalsStore(Path.Combine(root, "originals/quad")); store.Apply(plan); var text = File.ReadAllText(plan.LaunchEnvironment["DCSVR_QUAD_SETTINGS"]); Require(text.Contains("[exe:DCS]")); Require(text.Contains("turbo_mode=0")); Require(text.Contains("stereo_output_multiplier=1")); Require(store.RestoreOriginals().Complete);
});
Test("Cheeky and OFXR stereo stack installs and restores in isolated fixture", () =>
{
    var exe = Make("stereo/bin/DCS.exe", "fixture"); var options = Make("stereo/options.lua", Lua); var runtime = Make("stereo/runtime.json", "{\"runtime\":{\"library_path\":\"runtime.dll\"}}"); Make("stereo/runtime.dll", "fixture");
    var cache = new ComponentCache(Path.Combine(root, "stereo/cache"));
    var ofxr = cache.Import(PackageCatalog.Get("ofxr"), Path.Combine(workspace, ".cache/packages", PackageCatalog.Get("ofxr").ArchiveName));
    var cheeky = cache.Import(PackageCatalog.Get("cheeky"), Path.Combine(workspace, ".cache/packages", PackageCatalog.Get("cheeky").ArchiveName));
    var layer = Path.Combine(workspace, "artifacts/native/cheeky/bin/Release/CheekyOpenXRLayer.dll");
    var p = new VrProfile { QuadViews = QuadProvider.None, FoveatedDlss = true, FrameGen = FrameGeneration.Nvidia };
    var inv = new InventorySnapshot { DcsExecutable = exe, OptionsPath = options, PimaxRuntime = runtime };
    var plan = new DeploymentPlanner(new(ofxr, cheeky, layer)).Build(p, inv, Path.Combine(root, "stereo/managed"));
    Require(plan.LaunchEnvironment["XR_ENABLE_API_LAYERS"] == "XR_APILAYER_CHEEKY_foveated_dlss;XR_APILAYER_XRFrameBridge_diagnostic");
    var store = new OriginalsStore(Path.Combine(root, "originals/stereo")); store.Apply(plan); Require(File.Exists(Path.Combine(Path.GetDirectoryName(exe)!, "dxgi.dll"))); Require(store.RestoreOriginals().Complete); Require(!File.Exists(Path.Combine(Path.GetDirectoryName(exe)!, "dxgi.dll")));
});
Test("Applying a profile over the applied one overwrites its files, Cheeky's dxgi.dll included, with no new backup", () =>
{
    var exe = Make("replace-cheeky/bin/DCS.exe", "fixture"); var options = Make("replace-cheeky/options.lua", Lua);
    var runtime = Make("replace-cheeky/runtime.json", "{\"runtime\":{\"library_path\":\"runtime.dll\"}}"); Make("replace-cheeky/runtime.dll", "fixture");
    var cheeky = new ComponentCache(Path.Combine(root, "replace-cheeky/cache")).Import(PackageCatalog.Get("cheeky"), Path.Combine(workspace, ".cache/packages", PackageCatalog.Get("cheeky").ArchiveName));
    var layer = Path.Combine(workspace, "artifacts/native/cheeky/bin/Release/CheekyOpenXRLayer.dll");
    var p = new VrProfile { QuadViews = QuadProvider.None, FoveatedDlss = true };
    var inv = new InventorySnapshot { DcsExecutable = exe, OptionsPath = options, PimaxRuntime = runtime };
    var planner = new DeploymentPlanner(new(null, cheeky, layer)); var managed = Path.Combine(root, "replace-cheeky/managed");
    var store = new OriginalsStore(Path.Combine(root, "originals/replace-cheeky")); store.Apply(planner.Build(p, inv, managed));
    var count = store.Status().Count; var dxgi = Path.Combine(Path.GetDirectoryName(exe)!, "dxgi.dll");
    var again = planner.Build(p with { PeripheralScale = p.PeripheralScale == .4 ? .5 : .4 }, inv, managed, ownedSettings: store.OwnedSettings(options));
    Require(again.Files.Single(f => f.Path.Equals(dxgi, StringComparison.OrdinalIgnoreCase)).ExpectedSha256 is not null, "Existing files are listed as replaced, never refused");
    Require(store.Apply(again).ReplacedForeign.Count == 0 && store.Status().Count == count && store.Status().Files.Single(f => f.Path.Equals(dxgi, StringComparison.OrdinalIgnoreCase)).Action == "remove", "Same originals: dxgi.dll was absent");
    Require(store.RestoreOriginals().Complete && !File.Exists(dxgi) && File.ReadAllText(options) == Lua);
});
Test("A leftover Cheeky loader identical to ours is not treated as another overlay", () =>
{
    var exe = Make("leftover/bin/DCS.exe", "fixture"); var options = Make("leftover/options.lua", Lua);
    var runtime = Make("leftover/runtime.json", "{\"runtime\":{\"library_path\":\"runtime.dll\"}}"); Make("leftover/runtime.dll", "fixture");
    var cheeky = new ComponentCache(Path.Combine(root, "leftover/cache")).Import(PackageCatalog.Get("cheeky"), Path.Combine(workspace, ".cache/packages", PackageCatalog.Get("cheeky").ArchiveName));
    File.Copy(Path.Combine(cheeky, "dxgi.dll"), Path.Combine(Path.GetDirectoryName(exe)!, "dxgi.dll"));
    var plan = new DeploymentPlanner(new(null, cheeky, Path.Combine(workspace, "artifacts/native/cheeky/bin/Release/CheekyOpenXRLayer.dll")))
        .Build(new VrProfile { QuadViews = QuadProvider.None, FoveatedDlss = true }, new() { DcsExecutable = exe, OptionsPath = options, PimaxRuntime = runtime }, Path.Combine(root, "leftover/managed"));
    Require(plan.Files.Single(f => f.Path.EndsWith("dxgi.dll", StringComparison.OrdinalIgnoreCase)).ExpectedSha256 is not null);
    var store = new OriginalsStore(Path.Combine(root, "originals/leftover"));
    Require(store.Apply(plan).ReplacedForeign.Count == 0 && store.Status().Files.Single(f => f.Path.EndsWith("dxgi.dll", StringComparison.OrdinalIgnoreCase)).Action == "remove", "Our own loader counts as absent");
    Require(store.RestoreOriginals().Complete && !File.Exists(Path.Combine(Path.GetDirectoryName(exe)!, "dxgi.dll")));
});
Test("options.lua is changed only where the pipeline needs a different value", () =>
{
    var exe = Make("minimal-lua/bin/DCS.exe", "fixture"); var runtime = Make("minimal-lua/runtime.json", "{\"runtime\":{\"library_path\":\"runtime.dll\"}}"); Make("minimal-lua/runtime.dll", "fixture");
    var ready = Make("minimal-lua/ready.lua", "options = {\n\t[\"VR\"] = {\n\t\t[\"enable\"] = true,\n\t\t[\"openxr_eyeGaze\"] = false,\n\t},\n\t[\"graphics\"] = {\n\t\t[\"Upscaling\"] = \"DLSS\",\n\t},\n\t[\"miscellaneous\"] = {\n\t\t[\"launcher\"] = false,\n\t},\n}\n");
    var planner = new DeploymentPlanner(new(null, null, null)); var stereo = new VrProfile { QuadViews = QuadProvider.None };
    Require(planner.Build(stereo, new() { DcsExecutable = exe, OptionsPath = ready, PimaxRuntime = runtime }, Path.Combine(root, "minimal-lua/managed")).Files.All(f => f.Path != ready));
    var off = Make("minimal-lua/off.lua", "options = {\n\t[\"VR\"] = {\n\t\t[\"enable\"] = false,\n\t\t[\"openxr_eyeGaze\"] = false,\n\t},\n}\n");
    var changes = planner.Build(stereo, new() { DcsExecutable = exe, OptionsPath = off, PimaxRuntime = runtime }, Path.Combine(root, "minimal-lua/managed2")).Files.Single(f => f.Path == off).LuaChanges!;
    Require(changes.Select(c => c.Path).SequenceEqual(["VR.enable"])); // eye gaze and other options are never touched
});
Test("The refresh rate is read from Pimax Play's newest session report (fps_target), and follows the headset unless set by hand", () =>
{
    var logs = Path.Combine(root, "pimax-logs"); Directory.CreateDirectory(logs);
    var older = Path.Combine(logs, "PiService__2026-10-09-20.log");
    // Pimax Play's real report: fps_target is the headset's rate (72.4 at 72 Hz); refresh_rate is 120 whatever it is.
    File.WriteAllText(older, "x [rformanceSessionData] Performance body to send: {\"game_info\":{\"fps_analysis\":{\"fps_target\":72.40000000000001}},\"refresh_rate\":120}\n");
    File.SetLastWriteTimeUtc(older, DateTime.UtcNow.AddHours(-2));
    Require(HeadsetRefresh.ReadPimax(logs) is { Hz: 72.4 }, "fps_target, not refresh_rate");
    var newer = Path.Combine(logs, "PiService__2026-10-10-20.log");
    File.WriteAllText(newer, "a line without it\nx Performance body to send: {\"fps_analysis\":{\"fps_target\":72.40000000000001},\"refresh_rate\":120}\nx Performance body to send: {\"fps_analysis\":{\"fps_target\":90},\"refresh_rate\":120}\nx other: {\"fps_target\":60}\n");
    Require(HeadsetRefresh.ReadPimax(logs) is { Hz: 90 }, "newest file, last report");
    var previous = HeadsetRefresh.PimaxLogFolder; HeadsetRefresh.PimaxLogFolder = logs;
    try
    {
        Require(HeadsetRefresh.Resolve(new VrProfile { HeadsetRefreshHz = 72 }).HeadsetRefreshHz == 90, "follows the headset");
        Require(HeadsetRefresh.Resolve(new VrProfile { HeadsetRefreshHz = 72, RefreshFromHeadset = false }).HeadsetRefreshHz == 72, "set by hand");
        var read = HeadsetRefresh.Resolve(new VrProfile { FrameGen = FrameGeneration.Nvidia, FrameGenFactor = 2, FpsLimit = FpsLimitMode.MatchRefresh });
        Require(FramePacing.RequestedCap(read) == 45, "the DCS cap follows 90 Hz");
        Require(FramePacing.Checks(read, new InventorySnapshot()).Single(c => c.Id == "refresh-rate").State == CheckState.Pass, "a read rate needs no confirmation");
        Require(FramePacing.Checks(read with { RefreshFromHeadset = false }, new InventorySnapshot()).Single(c => c.Id == "refresh-rate").State == CheckState.Manual, "a rate set by hand is confirmed in the headset software");
    }
    finally { HeadsetRefresh.PimaxLogFolder = previous; }
    Require(HeadsetRefresh.ReadPimax(Path.Combine(root, "no-such-folder")) is null);
});
Test("The live Pimax compositor rate wins over the last session's report", () =>
{
    var runtime = Path.Combine(root, "pimax-runtime"); Directory.CreateDirectory(runtime);
    var older = Path.Combine(runtime, "pvr_srv_log_26-10-09-20-19-29.txt");
    File.WriteAllText(older, "[PSRV] 0 Normal rendering fps:(a:89.9,c:89.996436) Missed:(a:0)\n");
    File.SetLastWriteTimeUtc(older, DateTime.UtcNow.AddHours(-2));
    // Pimax Play switched from 90 to 72 Hz (display_timing_selection 0 to 1) in the newest log; one second missed frames
    // (c:66.1) and the last, partial second reads low too: neither is the panel's rate.
    File.WriteAllText(Path.Combine(runtime, "pvr_srv_log_26-10-10-14-12-32.txt"), new string('x', 300 * 1024)
        + string.Concat(Enumerable.Repeat("\n[PSRV] 0 Normal rendering fps:(a:90.02,c:89.996436) Missed:(a:0)", 4)) + "\n[PSRV] set display_timing_selection:1"
        + string.Concat(Enumerable.Repeat("\n[PSRV] 31556 Normal rendering fps:(a:36.1,c:72.402376) Missed:(a:0)", 6)) + "\n[PSRV] 31556 Warning rendering fps:(a:30.2,c:66.1) Missed:(a:9)"
        + string.Concat(Enumerable.Repeat("\n[PSRV] 31556 Normal rendering fps:(a:36.1,c:72.401) Missed:(a:0)", 4)) + "\n[PSRV] 31556 Normal rendering fps:(a:20.0,c:61.0) Missed:(a:0)\n[PSRV] hmd: 493.15 FPS\n");
    Require(HeadsetRefresh.ReadPimaxRuntime(runtime) is { Hz: 72.4, Source: "Pimax Play" }, "the newest log's steady compositor rate");
    Require(HeadsetRefresh.ReadPimaxRuntime(Path.Combine(root, "no-such-runtime")) is null);
    var logs = Path.Combine(root, "pimax-session-logs"); Directory.CreateDirectory(logs);
    File.WriteAllText(Path.Combine(logs, "PiService__2026-10-10-14.log"), "x Performance body to send: {\"fps_analysis\":{\"fps_target\":90}}\n");
    var (previousRuntime, previousLogs) = (HeadsetRefresh.PimaxRuntimeFolder, HeadsetRefresh.PimaxLogFolder);
    HeadsetRefresh.PimaxRuntimeFolder = runtime; HeadsetRefresh.PimaxLogFolder = logs;
    try
    {
        Require(HeadsetRefresh.Detect(RuntimeKind.Pimax) is { Hz: 72.4 }, "live rate first");
        HeadsetRefresh.PimaxRuntimeFolder = Path.Combine(root, "no-such-runtime");
        Require(HeadsetRefresh.Detect(RuntimeKind.Pimax) is { Hz: 90 }, "session report when the runtime log is missing");
    }
    finally { (HeadsetRefresh.PimaxRuntimeFolder, HeadsetRefresh.PimaxLogFolder) = (previousRuntime, previousLogs); }
});
Test("An installation from before the rename keeps its state folder; a new one uses DcsControl", () =>
{
    var fresh = Path.Combine(root, "rename/fresh"); Directory.CreateDirectory(fresh);
    Require(ControlService.StateRootIn(fresh) == Path.Combine(fresh, "DcsControl"));
    var earlier = Path.Combine(root, "rename/earlier"); Directory.CreateDirectory(Path.Combine(earlier, "DcsVrControl"));
    Require(ControlService.StateRootIn(earlier) == Path.Combine(earlier, "DcsVrControl"), "the backups of an existing install stay where they are");
    Directory.CreateDirectory(Path.Combine(earlier, "DcsControl"));
    Require(ControlService.StateRootIn(earlier) == Path.Combine(earlier, "DcsControl"));
});
Test("Optimizations only leaves DCS, its VR setting, runtime and layers as the user has them, and adds only the optimizations", () =>
{
    var exe = Make("desktop/bin/DCS.exe", "fixture");
    var options = Make("desktop/sg/DCS/Config/options.lua", "options = {\n\t[\"VR\"] = {\n\t\t[\"enable\"] = true,\n\t},\n\t[\"graphics\"] = {\n\t\t[\"maxFPS\"] = 144,\n\t\t[\"width\"] = 2560,\n\t\t[\"height\"] = 1440,\n\t\t[\"fullScreen\"] = true,\n\t},\n}\n");
    var engine = Path.GetDirectoryName(Make("desktop/components/dcsqvcull/DcsQvCull.dll", "loader"))!;
    Make("desktop/components/dcsqvcull/DcsQvCullPayload.dll", "payload"); Make("desktop/components/dcsqvcull/DcsQvCull.lua", "-- hook");
    // No OpenXR runtime on this PC at all.
    var inventory = new InventorySnapshot { DcsExecutable = exe, OptionsPath = options };
    var vr = new VrProfile { QuadViews = QuadProvider.QuadViewsFoveated, NeuralRendering = true, FrameGen = FrameGeneration.Nvidia, CpuBoost = true, EngineOptimizations = true, BoostPrefetch = PrefetchFix.Skip,
        SmallDcsWindow = true, LowerMonitor = true, PauseTobiiDesktop = true, FpsLimit = FpsLimitMode.MatchRefresh, DisableDcsVSync = true };
    var desktop = ProfileValidation.ResolveFeatures(vr with { Desktop = true });
    // The VR settings stay in the profile for the next VR flight (an app restart, Pimax again); none of them runs.
    Require(desktop.QuadViews == QuadProvider.QuadViewsFoveated && desktop.NeuralRendering && desktop.FrameGen == FrameGeneration.Nvidia && desktop.PauseTobiiDesktop, "VR settings kept");
    var launch = desktop.ForLaunch();
    Require(launch.QuadViews == QuadProvider.None && !launch.NeuralRendering && launch.FrameGen == FrameGeneration.Off && !launch.UsesCheeky && !launch.UsesQuadFocus && !desktop.UsesTobiiPause, "VR features off at launch");
    Require(ProfileValidation.ResolveFeatures(desktop with { Desktop = false }) is { QuadViews: QuadProvider.QuadViewsFoveated, NeuralRendering: true, UsesQuadFocus: true }, "Pimax again brings them back");
    Require(desktop.SmallDcsWindow && desktop.LowerMonitor && !desktop.UsesSmallDcsWindow && !desktop.UsesLowerMonitor && desktop.UsesBoostHelper && !desktop.BoostHelperElevated, "VR helpers kept for later but not run");
    var planner = new DeploymentPlanner(new(null, null, null, EngineDirectory: engine));
    var plan = planner.Build(desktop, inventory, Path.Combine(root, "desktop/managed"));
    // No OpenXR variable at all: the PC's own runtime and layers (another headset, its own Quad Views) stay in charge.
    Require(plan.LaunchEnvironment.Keys.All(k => !k.StartsWith("XR", StringComparison.OrdinalIgnoreCase) && !k.StartsWith("DISABLE_", StringComparison.OrdinalIgnoreCase) && !k.StartsWith("DCSVR_PREFETCH", StringComparison.OrdinalIgnoreCase)), string.Join(",", plan.LaunchEnvironment.Keys));
    // options.lua is not touched: VR on or off, frame limit, window and vertical sync stay the user's.
    Require(plan.Files.All(f => f.Path != options));
    // The optimizations (prefetch fix included, in DcsQvCull.ini) go to Saved Games; nothing to DCS's install folder.
    var ini = Encoding.Unicode.GetString(plan.Files.Single(f => f.Path.EndsWith("DcsQvCull.ini", StringComparison.OrdinalIgnoreCase)).Content);
    Require(ini.Contains("Mode=skip") && plan.Files.All(f => !f.Path.StartsWith(Path.GetDirectoryName(Path.GetFullPath(exe))!, StringComparison.OrdinalIgnoreCase)));
    Require(!LaunchSafety.ProfileFolders(desktop, inventory).Contains(Path.GetDirectoryName(exe)!, StringComparer.OrdinalIgnoreCase));
    Require(plan.Files.All(f => !f.Path.Contains("quadviews", StringComparison.OrdinalIgnoreCase) && !f.Path.Contains("ofxr", StringComparison.OrdinalIgnoreCase)));
    // The readiness checks never ask for a runtime or a headset.
    Require(!ProfileValidation.Validate(desktop, inventory).Any(i => i.Severity == IssueSeverity.Error));
    // A former Sboys user without SteamVR, and VR settings that need a runtime file, never block Optimizations only.
    Require(!ProfileValidation.Validate(desktop with { Runtime = RuntimeKind.SboysSteamVr }, inventory).Any(i => i.Severity == IssueSeverity.Error), "Sboys route kept, SteamVR absent");
    // A VR profile on the same PC still needs its runtime.
    try { planner.Build(ProfileValidation.ResolveFeatures(vr with { NeuralRendering = false }), inventory, Path.Combine(root, "desktop/managed-vr")); Require(false, "VR without a runtime must be refused"); }
    catch (InvalidDataException e) { Require(e.Message.Contains("OpenXR runtime")); }
});
Test("Profile launches skip the DCS launcher, whose restart loses the profile process", () =>
{
    var exe = Make("nolauncher/bin/DCS.exe", "fixture"); var options = Make("nolauncher/options.lua", "options = {\n\t[\"miscellaneous\"] = {\n\t\t[\"launcher\"] = true,\n\t},\n\t[\"graphics\"] = {\n\t},\n\t[\"VR\"] = {\n\t},\n}\n");
    var runtime = Make("nolauncher/runtime.json", "{\"runtime\":{\"library_path\":\"runtime.dll\"}}"); Make("nolauncher/runtime.dll", "fixture");
    var plan = new DeploymentPlanner(new(null, null, null)).Build(new VrProfile { QuadViews = QuadProvider.None }, new() { DcsExecutable = exe, OptionsPath = options, PimaxRuntime = runtime }, Path.Combine(root, "nolauncher/managed"));
    var change = plan.Files.Single(f => f.Path == options).LuaChanges!.Single(c => c.Path == "miscellaneous.launcher");
    Require(change.PreviousRaw == "true" && change.InstalledRaw == "false");
    var kept = new DeploymentPlanner(new(null, null, null)).Build(new VrProfile { QuadViews = QuadProvider.None, KeepDcsLauncher = true }, new() { DcsExecutable = exe, OptionsPath = options, PimaxRuntime = runtime }, Path.Combine(root, "nolauncher/managed-kept"));
    Require(kept.Files.Where(f => f.Path == options).SelectMany(f => f.LuaChanges ?? []).All(c => c.Path != "miscellaneous.launcher"));
});
Test("Another mod's dxgi.dll is backed up, replaced and reported, and Back to stock DCS brings it back", () =>
{
    var exe = Make("loaders/bin/DCS.exe", "fixture"); var options = Make("loaders/options.lua", Lua); var other = Make("loaders/bin/dxgi.dll", "other-mod");
    var runtime = Make("loaders/runtime.json", "{\"runtime\":{\"library_path\":\"runtime.dll\"}}"); Make("loaders/runtime.dll", "fixture");
    var cheeky = new ComponentCache(Path.Combine(root, "loaders/cache")).Import(PackageCatalog.Get("cheeky"), Path.Combine(workspace, ".cache/packages", PackageCatalog.Get("cheeky").ArchiveName));
    var p = new VrProfile { QuadViews = QuadProvider.None, FoveatedDlss = true };
    var plan = new DeploymentPlanner(new(null, cheeky, Path.Combine(workspace, "artifacts/native/cheeky/bin/Release/CheekyOpenXRLayer.dll"))).Build(p, new() { DcsExecutable = exe, OptionsPath = options, PimaxRuntime = runtime }, Path.Combine(root, "loaders/managed"));
    var store = new OriginalsStore(Path.Combine(root, "originals/loaders"));
    Require(store.Apply(plan).ReplacedForeign.SequenceEqual([other]) && Hashing.FileSha256(other) == Hashing.FileSha256(Path.Combine(cheeky, "dxgi.dll")));
    Require(store.Status().Files.Single(f => f.Path == other) is { Action: "restore", Foreign: true });
    Require(store.RestoreOriginals().Complete && File.ReadAllText(other) == "other-mod" && File.ReadAllText(options) == Lua);
});
Test("The prefetch fix is an engine optimization: nothing in DCS's bin without DLSS 5, and a 0.5.4 profile's dxgi.dll and dxgi2.dll go in the next apply", () =>
{
    foreach (var (name, userDxgi, desktop) in new[] { ("clean", (string?)null, false), ("reshade", "user's ReShade", true) })
    {
        var dir = "prefetch-move/" + name;
        var exe = Make(dir + "/game/bin/DCS.exe", "fixture"); var options = Make(dir + "/sg/DCS/Config/options.lua", Lua);
        var runtime = Make(dir + "/runtime.json", "{\"runtime\":{\"library_path\":\"runtime.dll\"}}"); Make(dir + "/runtime.dll", "fixture");
        var components = Path.GetDirectoryName(Make(dir + "/components/dcsqvcull/DcsQvCull.dll", "loader"))!;
        Make(dir + "/components/dcsqvcull/DcsQvCullPayload.dll", "payload"); Make(dir + "/components/dcsqvcull/DcsQvCull.lua", "-- hook");
        var bin = Path.GetDirectoryName(Path.GetFullPath(exe))!; var dxgi = Path.Combine(bin, "dxgi.dll"); var dxgi2 = Path.Combine(bin, "dxgi2.dll");
        var oldIni = Path.Combine(bin, "DcsControlPrefetchFix.ini"); var oldLog = Path.Combine(bin, "DcsVrPrefetchFix.log");
        if (userDxgi is not null) File.WriteAllText(dxgi, userDxgi);
        var inventory = new InventorySnapshot { DcsExecutable = exe, OptionsPath = options, PimaxRuntime = runtime };
        var store = new OriginalsStore(Path.Combine(root, "originals/" + dir));
        // What 0.5.4 applied for CPU Boost with the prefetch fix and no Cheeky: the loader alone, the fix and its settings.
        FileMutation Old(string path, string content, IReadOnlyList<string>? logs = null) => new(path, File.Exists(path) ? Hashing.FileSha256(path) : null, Encoding.UTF8.GetBytes(content), "0.5.4", RuntimeLogs: logs, Root: bin);
        store.Apply(new("boost", "0.5.4", [Old(dxgi, "focus loader"), Old(dxgi2, "prefetch fix", [oldLog]), Old(oldIni, "[PrefetchFix]\nMode=skip\n")],
            new Dictionary<string, string> { ["DCSVR_PREFETCH_MODE"] = "skip" }, exe));
        File.WriteAllText(oldLog, "2026-10-10 20:00:00 calls/s=1000 skipped=99.0% pointers=1 imports=40 lookups=0 faults=0");
        Require(File.ReadAllText(dxgi) == "focus loader" && File.Exists(dxgi2));
        // 0.5.5: CPU Boost with Engine Optimizations and the fix on Skip writes nothing in bin and no prefetch variable.
        var profile = new VrProfile { QuadViews = QuadProvider.None, CpuBoost = true, EngineOptimizations = true, BoostPrefetch = PrefetchFix.Skip, Desktop = desktop };
        var planner = new DeploymentPlanner(new(null, null, null, EngineDirectory: components));
        var plan = planner.Build(profile, inventory, Path.Combine(root, dir + "/managed"));
        Require(plan.Files.All(f => !f.Path.StartsWith(bin + Path.DirectorySeparatorChar, StringComparison.OrdinalIgnoreCase)), "Nothing in DCS's bin: " + string.Join(", ", plan.Files.Select(f => f.Path)));
        Require(plan.LaunchEnvironment.Keys.All(k => !k.StartsWith("DCSVR_PREFETCH", StringComparison.OrdinalIgnoreCase)));
        Require(!LaunchSafety.ProfileFolders(profile, inventory).Contains(bin, StringComparer.OrdinalIgnoreCase), "The DCS install folder is not needed");
        // The normal apply takes the old files out (the user's own dxgi.dll comes back) and writes [Prefetch] in the same step.
        var applied = store.Apply(plan);
        Require(!File.Exists(dxgi2) && !File.Exists(oldIni) && !File.Exists(oldLog), "dxgi2.dll, its settings and its log are gone");
        Require(userDxgi is null ? !File.Exists(dxgi) : File.ReadAllText(dxgi) == userDxgi, "dxgi.dll removed, or the user's own put back");
        Require(applied.PutBack.Count >= 3 && applied.ReplacedForeign.Count == 0, "Put back: " + string.Join(", ", applied.PutBack));
        var module = Path.Combine(Path.GetDirectoryName(Path.GetDirectoryName(Path.GetFullPath(options)))!, "Scripts", "DcsQvCull");
        var ini = File.ReadAllText(Path.Combine(module, "DcsQvCull.ini")).ReplaceLineEndings("\n");
        Require(ini.Contains("[Prefetch]\n") && ini.Contains("\nMode=skip\nWindowMs=5000\n"), ini);
        Require(store.Status().Files.All(f => !f.Path.StartsWith(bin + Path.DirectorySeparatorChar, StringComparison.OrdinalIgnoreCase)), "The DCS install is no longer recorded as changed");
        // Off writes Mode=off; CPU Boost without Engine Optimizations has no prefetch fix and writes nothing at all for it.
        Require(ConfigurationWriters.DcsQvCull(profile with { BoostPrefetch = PrefetchFix.Off }).Contains("\nMode=off"));
        Require(planner.Build(profile with { EngineOptimizations = false }, inventory, Path.Combine(root, dir + "/managed-noengine")).Files.All(f => !f.Path.Contains("DcsQvCull") && !f.Path.StartsWith(bin + Path.DirectorySeparatorChar, StringComparison.OrdinalIgnoreCase)));
        Require(store.RestoreOriginals().Complete && !Directory.Exists(module) && (userDxgi is null ? !File.Exists(dxgi) : File.ReadAllText(dxgi) == userDxgi));
    }
});
Test("Pupil shift is deployed first in the layer chain with its ini, only for eye-tracked Quad Views", () =>
{
    var exe = Make("pupil/game/bin/DCS.exe", "fixture"); var options = Make("pupil/sg/DCS/Config/options.lua", Lua);
    var runtime = Make("pupil/runtime.json", "{\"runtime\":{\"library_path\":\"runtime.dll\"}}"); Make("pupil/runtime.dll", "fixture");
    var dll = Make("pupil/components/pupilshift/XR_APILAYER_DCSVR_pupil_shift.dll", "layer");
    var inventory = new InventorySnapshot { DcsExecutable = exe, OptionsPath = options, PimaxRuntime = runtime };
    var planner = new DeploymentPlanner(new(null, null, null, PupilShiftDll: dll));
    var on = new VrProfile { QuadViews = QuadProvider.PimaxNative, Gaze = GazeMode.EyeTracked, PupilShift = true, PupilShiftEyeRadiusMm = 11, PupilShiftVirtualImageM = 1.2 };
    var plan = planner.Build(on, inventory, Path.Combine(root, "pupil/managed"));
    Require(plan.LaunchEnvironment["XR_ENABLE_API_LAYERS"].Split(';')[0] == "XR_APILAYER_DCSVR_pupil_shift");
    var ini = Encoding.UTF8.GetString(plan.Files.Single(f => f.Path.EndsWith("PupilShift.ini")).Content).ReplaceLineEndings("\n");
    Require(ini.Contains("[PupilShift]\nEnabled=1") && ini.Contains("EyeRadiusMm=11") && ini.Contains("VirtualImageM=1.2") && ini.Contains("SimulateGaze=0"));
    var manifest = Encoding.UTF8.GetString(plan.Files.Single(f => f.Path.EndsWith("pupil-shift.json")).Content);
    Require(manifest.Contains("XR_APILAYER_DCSVR_pupil_shift") && manifest.Contains("XR_APILAYER_DCSVR_pupil_shift.dll"));
    // Without a gaze (fixed focus, no Quad Views, desktop) the layer is not installed and the profile says why.
    foreach (var off in new[] { on with { Gaze = GazeMode.Fixed }, on with { QuadViews = QuadProvider.None }, on with { PupilShift = false } })
        Require(!planner.Build(off, inventory, Path.Combine(root, "pupil/managed-off")).Files.Any(f => f.Path.Contains("pupilshift")));
    Require(ProfileValidation.Validate(on with { Gaze = GazeMode.Fixed }).Any(i => i.Code == "pupil-gaze" && i.Severity == IssueSeverity.Warning));
    Require(ProfileValidation.Validate(on with { PupilShiftEyeRadiusMm = 30 }).Any(i => i.Code == "pupil-radius" && i.Severity == IssueSeverity.Error));
    Require(ProfileValidation.Validate(on with { PupilShiftVirtualImageM = 0.1 }).Any(i => i.Code == "pupil-image"));
    Require(!ProfileValidation.Validate(on).Any(i => i.Code.StartsWith("pupil")));
    Throws<InvalidDataException>(() => new DeploymentPlanner(new(null, null, null)).Build(on, inventory, Path.Combine(root, "pupil/managed-missing")));
    // The in-flight switch: written to the ini, named for the diagnostic panel, and kept apart from the other keys.
    Require(ini.Contains("Toggle=121:5"));
    Require(ConfigurationWriters.PupilShift(on with { PupilShiftToggleKey = "Off" }).Contains("Toggle=0:0") && plan.LaunchEnvironment["DCSVR_PUPIL_HOTKEY"] == "0:0");
    Require(ProfileValidation.Validate(on with { EngineOptimizations = true, PupilShiftToggleKey = NeuralHotkeys.EngineDefault }).Any(i => i.Code == "hotkey-conflict"));
    Require(ProfileValidation.Validate(on with { EngineOptimizations = true, PupilShiftToggleKey = "120:3" }).Any(i => i.Code == "pupil-hotkey"));
    Require(!ProfileValidation.Validate(on with { PupilShift = false, EngineOptimizations = true, PupilShiftToggleKey = NeuralHotkeys.EngineDefault }).Any(i => i.Code == "hotkey-conflict"));
});
Test("Other environments: AMD/Intel graphics, OpenBeta Saved Games, 3× at 72 Hz, small CPUs, short profile names", () =>
{
    // NVIDIA-only processing on an AMD or Intel card is refused with what to choose instead; FidelityFX is fine.
    var previous = GpuInfo.Adapters;
    try
    {
        GpuInfo.Adapters = () => [new("AMD Radeon RX 7900 XTX", "AMD", "31.0")];
        var issues = ProfileValidation.Validate(new VrProfile { QuadViews = QuadProvider.QuadViewsFoveated, NeuralRendering = true, NeuralRuntimePath = @"C:\x\nvngx_dlssnr.dll", FrameGen = FrameGeneration.Nvidia });
        Require(issues.Any(i => i.Code == "dlss-gpu" && i.Message.Contains("AMD Radeon")) && issues.Any(i => i.Code == "framegen-gpu" && i.Message.Contains("FidelityFX")));
        Require(!ProfileValidation.Validate(new VrProfile { FrameGen = FrameGeneration.FidelityFx }).Any(i => i.Code.EndsWith("-gpu")), "FidelityFX runs anywhere");
        Require(!ProfileValidation.Validate(new VrProfile { Desktop = true, FrameGen = FrameGeneration.Nvidia }).Any(i => i.Code.EndsWith("-gpu")), "Optimizations only runs none of it");
        GpuInfo.Adapters = () => [];
        Require(!ProfileValidation.Validate(new VrProfile { FrameGen = FrameGeneration.Nvidia }).Any(i => i.Code.EndsWith("-gpu")), "unknown adapters block nothing");
        GpuInfo.Adapters = () => [new("NVIDIA GeForce GTX 1080 Ti", "NVIDIA", "32.0")];
        Require(!GpuInfo.HasNvidiaRtx && GpuInfo.IsNvidiaRtx(new("NVIDIA RTX A4000", "NVIDIA", null)) && !GpuInfo.IsNvidiaRtx(new("Intel Arc A770", "Intel", null)));
    }
    finally { GpuInfo.Adapters = previous; }
    // The Saved Games folder DCS uses comes from dcs_variant.txt.
    Make("variant/openbeta/dcs_variant.txt", "openbeta\r\n"); Make("variant/stable/bin/DCS.exe", "fixture");
    Require(WindowsInventory.DcsVariantFolder(Path.Combine(root, "variant/openbeta")) == "DCS.openbeta" && WindowsInventory.DcsVariantFolder(Path.Combine(root, "variant/stable")) is null && WindowsInventory.DcsVariantFolder(null) is null);
    // 3× at 72 Hz asks DCS for no less than its minimum.
    Require(FramePacing.RequestedCap(new() { FpsLimit = FpsLimitMode.MatchRefresh, HeadsetRefreshHz = 72, FrameGen = FrameGeneration.Nvidia, FrameGenFactor = 3 }) == 30);
    // A small CPU: background apps never land on DCS's render cores, the headset services are not squeezed.
    var small = CpuTopology.LimitToSystem(new CpuTopology(CoreRankingSource.DcsLog, "4 cores", 8, [0, 1, 2, 3], [4, 5], [3, 6, 7]), 1, 0xFF, 8);
    Require(small.HasRanking && !small.BackgroundCores.Contains(3) && small.VrRuntimeCores.Count == 0, "small CPU");
    // An 8.3 short name of the profile folder is removed from the diagnostic package too.
    Require(!Diagnostics.Redactor("Giuseppe", @"C:\Users\Giuseppe", "PC-01")(@"TEMP=C:\Users\GIUSEP~1\AppData\Local\Temp").Contains("GIUSEP"));
});
Test("Another program's dxgi.dll in DCS's bin is reported before Launch sets it aside; ours never is", () =>
{
    var distribution = Path.Combine(root, "dxgi/distribution");
    Make("dxgi/distribution/components/cheeky/dxgi.dll", "our loader");
    var service = new ControlService(distribution, Path.Combine(root, "dxgi/state"));
    var exe = Make("dxgi/game/bin/DCS.exe", "fixture");
    var inventory = new InventorySnapshot { DcsExecutable = exe };
    var dlss = new VrProfile { NeuralRendering = true, QuadViews = QuadProvider.QuadViewsFoveated };
    Require(service.ForeignDxgi(dlss, inventory) is null, "no dxgi.dll");
    Make("dxgi/game/bin/dxgi.dll", "ReShade bytes");
    Require(service.ForeignDxgi(dlss, inventory) is { Name: "another program" } found && found.Path.EndsWith("dxgi.dll"), "a foreign one");
    Require(service.ForeignDxgi(new VrProfile { QuadViews = QuadProvider.PimaxNative }, inventory) is null, "no loader needed, nothing to say");
    // Kept working: the pilot's own file is chained after the DLSS 5 loader as dxgi2.dll; never when off, and never over
    // another program's dxgi2.dll.
    Require(service.ChainedDxgi(dlss, inventory) is { } chained && Encoding.UTF8.GetString(chained) == "ReShade bytes", "chained");
    Require(service.ChainedDxgi(dlss with { KeepOtherDxgi = false }, inventory) is null, "set aside instead");
    Make("dxgi/game/bin/dxgi2.dll", "someone else's chained mod");
    Require(service.ChainedDxgi(dlss, inventory) is null, "another program's dxgi2.dll stays");
    File.Delete(Path.Combine(root, "dxgi/game/bin/dxgi2.dll"));
    Make("dxgi/game/bin/dxgi.dll", "our loader");
    Require(service.ForeignDxgi(dlss, inventory) is null, "our own loader");
});
Test("Writes survive a file held for a moment, keep read-only files read-only, and a protected folder is a warning Launch resolves", () =>
{
    // options.lua made read-only by the pilot: replaced, and still read-only.
    var file = Make("resilience/Config/options.lua", "old");
    File.SetAttributes(file, File.GetAttributes(file) | FileAttributes.ReadOnly);
    AtomicFile.WriteText(file, "new");
    Require(File.ReadAllText(file) == "new" && File.GetAttributes(file).HasFlag(FileAttributes.ReadOnly), "read-only kept");
    File.SetAttributes(file, FileAttributes.Normal);
    // Another program holding the file (an antivirus scan) for a moment: the write waits for it.
    var held = new FileStream(file, FileMode.Open, FileAccess.Read, FileShare.Read);
    var release = Task.Run(async () => { await Task.Delay(300); held.Dispose(); });
    AtomicFile.WriteText(file, "after the scan");
    release.Wait();
    Require(File.ReadAllText(file) == "after the scan", "retried until released");
    var tries = 0;
    Throws<UnauthorizedAccessException>(() => AtomicFile.Retry(() => { tries++; throw new UnauthorizedAccessException(); }));
    Require(tries == 8, "a real permission problem fails after the last try");
    // Folders that do not exist are left to their own checks; writable ones are not reported.
    Require(LaunchSafety.UnwritableFolders([Path.Combine(root, "resilience/Config"), Path.Combine(root, "no-such-folder")]).Count == 0);
    var exe = Make("resilience/game/bin/DCS.exe", "fixture");
    var inventory = new InventorySnapshot { DcsExecutable = exe, OptionsPath = file };
    Require(!ProfileValidation.Validate(new VrProfile(), inventory).Any(i => i.Code == "write-access"), "writable folders raise nothing");
    Require(LaunchSafety.WriteAccessAdvice.Contains("Grant access") && LaunchSafety.WriteAdvice().StartsWith(LaunchSafety.WriteAccessAdvice));
});
Test("Diagnostics explains DLSS 5 on the focus area from the last session of Cheeky's log", () =>
{
    const string start = "22:29:05.544 [T16224] Cheeky 0.5.3 Standalone initializing; runtime remains resident until game exit";
    const string init = "22:29:30.000 [T1] DLSS-NR 310.8 feature-18 runtime initialized";
    const string periphery = "22:30:14.011 [T10000] DCS quad focus: reject handle=0x247330A4030 count=2048 reason=no-proof matches=0 exact=0 copy=0 size_route=output-not-focus-size ngx=(0,0 816x804) resource=0x248068BB780";
    const string detail = "22:30:14.012 [T10000] DCS quad focus: detail handle=0x247330A4030 focusL=(0,0 1520x1480 arr=0 flags=0x7 res=0x1 chain=0x2 size=differ) focusR=(0,0 1520x1480 arr=1 flags=0x7 res=0x1 chain=0x2 size=differ) copy_edges=0 submitted_copies=0";
    const string accepted = "22:30:15.000 [T10000] DCS quad focus: mapped focus accepted handle=0x1 resource=0x2 route=size-bijection output=(0,0 1520x1480) census=focus2/peripheral2/other0";
    var applied = Diagnostics.DiagnoseDlssFocus([start, init, periphery, detail, accepted]);
    Require(applied.State == "applied" && applied.Accepted == 1 && applied.Rejected == 1 && applied.Version == "310.8" && applied.Explanation!.Contains("peripheral"), "peripheral rejections are normal");
    // An earlier session's acceptance does not count for the last one.
    var notApplied = Diagnostics.DiagnoseDlssFocus([start, accepted, start, init, periphery, detail, periphery]);
    Require(notApplied.State == "not-applied" && notApplied.Reason == "no-proof/output-not-focus-size" && notApplied.Rejected == 2 && notApplied.DlssSize == "816x804" && notApplied.FocusSize == "1520x1480");
    Require(notApplied.Explanation!.Contains("816×804") && notApplied.Explanation.Contains("1520×1480"), notApplied.Explanation);
    Require(Diagnostics.DiagnoseDlssFocus([start, init]).State == "no-dlss");
    Require(Diagnostics.DiagnoseDlssFocus(["x DCS quad focus: reject handle=0x1 count=1 reason=snapshot-unavailable layer_module=missing abi_compatible=0"]).Reason == "snapshot-unavailable");
    Require(Diagnostics.DiagnoseDlssFocus(Path.Combine(root, "no-dcs-bin")).State == "no-session");
});
Test("The diagnostic package collects DCS, component and headset logs as text, with the user and PC names removed", () =>
{
    var saved = Path.Combine(root, "diag/Saved Games/DCS"); var bin = Path.Combine(root, "diag/game/bin");
    var options = Make("diag/Saved Games/DCS/Config/options.lua", "options = { graphics = { } }");
    Make("diag/Saved Games/DCS/Logs/dcs.log", "=== Log opened by " + Environment.UserName + " on " + Environment.MachineName + " at " + Environment.GetFolderPath(Environment.SpecialFolder.UserProfile) + "\\Saved Games, contact someone@example.com");
    Make("diag/Saved Games/DCS/Scripts/Hooks/DcsQvCull.lua", "-- hook");
    var exe = Make("diag/game/bin/DCS.exe", "fixture");
    Make("diag/game/bin/CheekyFoveatedDLSS/CheekyFoveatedDLSS-Standalone.log", "Cheeky log");
    Make("diag/game/bin/CheekyFoveatedDLSS/nvngx_dlssnr.dll", "binary");
    var managed = Path.Combine(root, "diag/managed/profiles/p1");
    Make("diag/managed/profiles/p1/ofxr/ofxr-bridge-flight-1.log", "ofxr");
    Make("diag/managed/profiles/p1/ofxr/ofxr-bridge-flight-1.prev.log", "ofxr earlier segment");
    Make("diag/managed/profiles/p1/ofxr/XR_APILAYER_x.dll", "binary");
    var local = Path.Combine(root, "diag/local");
    Make("diag/local/Pimax/runtime/pvr_srv_log_26-10-10-14-12-32.txt", new string('a', 10) + "rendering fps:(a:90.0,c:89.99)");
    Make("diag/local/Pimax/runtime/pvr_srv_log_26-10-09-14-12-32.txt", "older");
    File.SetLastWriteTimeUtc(Path.Combine(local, "Pimax/runtime/pvr_srv_log_26-10-09-14-12-32.txt"), DateTime.UtcNow.AddDays(-1));
    var zip = Path.Combine(root, "diag/package.zip");
    var entries = Diagnostics.CreatePackage(new(null, managed, options, exe, RuntimeKind.Pimax, local, Path.Combine(root, "diag/roaming")), "{\"report\":1}", zip);
    using var archive = System.IO.Compression.ZipFile.OpenRead(zip);
    var names = archive.Entries.Select(e => e.FullName).ToHashSet();
    foreach (var name in new[] { "report.json", "dcs/Logs/dcs.log", "dcs/Config/options.lua", "dcs/Scripts/Hooks.txt", "dcs/bin/CheekyFoveatedDLSS/CheekyFoveatedDLSS-Standalone.log", "dcs/bin/modules.txt",
        "dcs-control/applied-profile/ofxr/ofxr-bridge-flight-1.log", "dcs-control/applied-profile/ofxr/ofxr-bridge-flight-1.prev.log", "pimax/runtime/pvr_srv_log_26-10-10-14-12-32.txt", "openxr.txt", "system.txt", "README.txt" })
        Require(names.Contains(name), "missing " + name);
    Require(!names.Any(n => n.EndsWith(".dll")) && !names.Contains("pimax/runtime/pvr_srv_log_26-10-09-14-12-32.txt"), "text only, newest runtime log");
    string Read(string name) { using var reader = new StreamReader(archive.GetEntry(name)!.Open()); return reader.ReadToEnd(); }
    var log = Read("dcs/Logs/dcs.log");
    Require(log.Contains("<user>") && log.Contains("<pc>") && log.Contains("<e-mail>") && !log.Contains(Environment.UserName, StringComparison.OrdinalIgnoreCase) && !log.Contains("someone@example.com"), log);
    Require(!Read("README.txt").Contains(Environment.UserName, StringComparison.OrdinalIgnoreCase) && entries.Count == archive.Entries.Count);
    // Every spelling of the profile folder, a profile folder named differently from the user, short and non-ASCII
    // names, and what Pimax's service logs carry.
    var redact = Diagnostics.Redactor("Nicolò", @"C:\Users\Al", "RIG-01");
    var text = redact(@"C:\Users\Al\Saved Games | C:\\Users\\Al\\x | C:/Users/Al/y | ""C:\\Users\\Al"" | pilot Nicolò and Nicol\u00F2 on RIG-01 | Algebra stays"
        + "\n[RequestReportSn]   token: ECT:[[[AAAABBBBCCCCDDDDEE]]] | SetAccountInfo: userId=12345, isValid=1 | Pimax Crystal Super, serial number: X999TEST000000000001 | Serial:X999TEST000000000001 | map X999TEST000000000001platMap");
    Require(!text.Contains(@"Users\Al") && !text.Contains(@"Users\\Al") && !text.Contains("Users/Al") && !text.Contains("Nicol") && !text.Contains("RIG-01") && text.Contains("Algebra stays"), text);
    Require(!text.Contains("AAAABBBBCC") && !text.Contains("12345") && !text.Contains("X999TEST") && text.Contains("token: <token>") && text.Contains("userId=<id>") && text.Contains("serial number: <serial>"), text);
});
Test("Pupil shift takes over DCS's custom IPD (same value, applied by the layer) and leaves it alone otherwise", () =>
{
    var exe = Make("pupil-ipd/game/bin/DCS.exe", "fixture"); var runtime = Make("pupil-ipd/runtime.json", "{\"runtime\":{\"library_path\":\"runtime.dll\"}}"); Make("pupil-ipd/runtime.dll", "fixture");
    var dll = Make("pupil-ipd/components/pupilshift/XR_APILAYER_DCSVR_pupil_shift.dll", "layer");
    var planner = new DeploymentPlanner(new(null, null, null, PupilShiftDll: dll));
    var on = new VrProfile { QuadViews = QuadProvider.PimaxNative, Gaze = GazeMode.EyeTracked, PupilShift = true, EyeDistance = EyeDistanceSource.DcsCustom };
    string Ini(ApplyPlan plan) => Encoding.UTF8.GetString(plan.Files.Single(f => f.Path.EndsWith("PupilShift.ini")).Content).ReplaceLineEndings("\n");
    IReadOnlyList<LuaValueChange> Lua(ApplyPlan plan) => plan.Files.Single(f => f.Path.EndsWith("options.lua")).LuaChanges ?? [];
    var custom = Make("pupil-ipd/sg/DCS/Config/options.lua", "options = {\n\t[\"VR\"] = {\n\t\t[\"custom_IPD\"] = \"64\",\n\t\t[\"custom_IPD_enable\"] = true,\n\t\t[\"enable\"] = true,\n\t},\n}\n");
    var plan = planner.Build(on, new InventorySnapshot { DcsExecutable = exe, OptionsPath = custom, PimaxRuntime = runtime }, Path.Combine(root, "pupil-ipd/managed"));
    Require(Ini(plan).Contains("Ipd=64\n") && Lua(plan).Any(c => c.Path == "VR.custom_IPD_enable" && c.InstalledRaw == "false"), "taken over");
    // Without DCS's custom IPD (or without Pupil shift) nothing changes.
    var plain = Make("pupil-ipd/sg2/DCS/Config/options.lua", "options = {\n\t[\"VR\"] = {\n\t\t[\"custom_IPD\"] = \"64\",\n\t\t[\"custom_IPD_enable\"] = false,\n\t\t[\"enable\"] = true,\n\t},\n}\n");
    plan = planner.Build(on, new InventorySnapshot { DcsExecutable = exe, OptionsPath = plain, PimaxRuntime = runtime }, Path.Combine(root, "pupil-ipd/managed2"));
    Require(Ini(plan).Contains("Ipd=0\n") && !Lua(plan).Any(c => c.Path.StartsWith("VR.custom_IPD")), "left alone");
    plan = planner.Build(on with { PupilShift = false }, new InventorySnapshot { DcsExecutable = exe, OptionsPath = custom, PimaxRuntime = runtime }, Path.Combine(root, "pupil-ipd/managed3"));
    Require(!Lua(plan).Any(c => c.Path.StartsWith("VR.custom_IPD")), "no Pupil shift, DCS keeps its IPD");
    // Eye distance: the headset's own (default) turns DCS's custom IPD off, with or without the layer (the layer then
    // uses the runtime's IPD, Ipd=0); Set here goes through the layer when it runs, else through DCS's own option.
    foreach (var layer in new[] { true, false })
    {
        plan = planner.Build(on with { PupilShift = layer, EyeDistance = EyeDistanceSource.Headset }, new InventorySnapshot { DcsExecutable = exe, OptionsPath = custom, PimaxRuntime = runtime }, Path.Combine(root, $"pupil-ipd/headset-{layer}"));
        Require(Lua(plan).Any(c => c.Path == "VR.custom_IPD_enable" && c.InstalledRaw == "false") && (!layer || Ini(plan).Contains("Ipd=0\n")), "headset IPD, layer " + layer);
        plan = planner.Build(on with { PupilShift = layer, EyeDistance = EyeDistanceSource.Manual, EyeDistanceMm = 65.5 }, new InventorySnapshot { DcsExecutable = exe, OptionsPath = plain, PimaxRuntime = runtime }, Path.Combine(root, $"pupil-ipd/manual-{layer}"));
        if (layer) Require(Ini(plan).Contains("Ipd=65.5\n") && !Lua(plan).Any(c => c.Path.StartsWith("VR.custom_IPD")), "manual through the layer");
        else Require(Lua(plan).Any(c => c.Path == "VR.custom_IPD_enable" && c.InstalledRaw == "true") && Lua(plan).Any(c => c.Path == "VR.custom_IPD" && c.InstalledRaw == "\"65.5\""), "manual through DCS's option");
    }
    plan = planner.Build(on with { EyeDistance = EyeDistanceSource.Headset }, new InventorySnapshot { DcsExecutable = exe, OptionsPath = plain, PimaxRuntime = runtime }, Path.Combine(root, "pupil-ipd/headset-plain"));
    Require(!Lua(plan).Any(c => c.Path.StartsWith("VR.custom_IPD")), "headset IPD already in use: nothing to change");
    Require(ProfileValidation.Validate(on with { EyeDistance = EyeDistanceSource.Manual, EyeDistanceMm = 30 }).Any(i => i.Code == "eye-distance"), "manual range");
    Require(JsonData.Deserialize<VrProfile>("{\"schemaVersion\":1,\"id\":\"old\"}").EyeDistance == EyeDistanceSource.Headset, "older profiles use the headset's eye distance");
    // The layer's log: the views' cant and the eye distances.
    var folder = Path.Combine(root, "pupil-ipd/status"); Make("pupil-ipd/status/" + PupilShiftStatus.LayerDll, "layer");
    Make("pupil-ipd/status/" + PupilShiftStatus.LogName, "10:00:02.000 stats: frames=90 shifted=90 restored=360 gaze x[-1.0..1.0] y[0.0..0.0] deg, max shift 0.20 mm; runtime left eye vs head x[-32.77..-32.77] z[0.00..0.00] mm; cant 6.1 deg, runtime IPD 65.5 mm, rendered IPD 64.0 mm\n");
    Require(PupilShiftStatus.Read(folder, false).Last is { CantDeg: 6.1, RuntimeIpdMm: 65.5, RenderedIpdMm: 64.0 });
});
Test("Depth boost deploys the pupil shift layer on its own, with its section, its switch and DCS's custom IPD taken over", () =>
{
    var exe = Make("depth/game/bin/DCS.exe", "fixture"); var runtime = Make("depth/runtime.json", "{\"runtime\":{\"library_path\":\"runtime.dll\"}}"); Make("depth/runtime.dll", "fixture");
    var dll = Make("depth/components/pupilshift/XR_APILAYER_DCSVR_pupil_shift.dll", "layer");
    // Depth boost's dual camera brings the engine module, in its Perception-only mode without Engine Optimizations.
    var depthEngine = Path.GetDirectoryName(Make("depth/components/dcsqvcull/DcsQvCull.dll", "loader"))!;
    Make("depth/components/dcsqvcull/DcsQvCullPayload.dll", "payload"); Make("depth/components/dcsqvcull/DcsQvCull.lua", "-- hook");
    var planner = new DeploymentPlanner(new(null, null, null, PupilShiftDll: dll, EngineDirectory: depthEngine));
    var custom = Make("depth/sg/DCS/Config/options.lua", "options = {\n\t[\"VR\"] = {\n\t\t[\"custom_IPD\"] = \"64\",\n\t\t[\"custom_IPD_enable\"] = true,\n\t\t[\"enable\"] = true,\n\t},\n}\n");
    var inventory = new InventorySnapshot { DcsExecutable = exe, OptionsPath = custom, PimaxRuntime = runtime };
    string Ini(ApplyPlan plan) => Encoding.UTF8.GetString(plan.Files.Single(f => f.Path.EndsWith("PupilShift.ini")).Content).ReplaceLineEndings("\n");
    IReadOnlyList<LuaValueChange> Lua(ApplyPlan plan) => plan.Files.SingleOrDefault(f => f.Path.EndsWith("options.lua"))?.LuaChanges ?? [];
    // Off by default.
    Require(!new VrProfile().DepthBoost && !new VrProfile { QuadViews = QuadProvider.PimaxNative, Gaze = GazeMode.EyeTracked }.UsesPerceptionLayer, "off by default");
    var on = new VrProfile { QuadViews = QuadProvider.PimaxNative, Gaze = GazeMode.EyeTracked, DepthBoost = true, DepthBoostMode = DepthBoostMode.Enhanced, DepthBoostMax = 1.3, DepthBoostNearM = 1.2, EyeDistance = EyeDistanceSource.DcsCustom };
    var plan = planner.Build(on, inventory, Path.Combine(root, "depth/managed"));
    Require(plan.LaunchEnvironment["XR_ENABLE_API_LAYERS"].Split(';')[0] == "XR_APILAYER_DCSVR_pupil_shift", "deployed first without Pupil shift");
    var ini = Ini(plan);
    Require(ini.Contains("[PupilShift]\nEnabled=0\n") && ini.Contains("[DepthBoost]\nEnabled=1\n") && ini.Contains("MaxScale=1.3\n") && ini.Contains("NearM=1.2\n")
        && ini.Contains("Mode=enhanced\n") && ini.Contains("FarStartM=3\n") && ini.Contains("FarFullM=50\n")
        && ini.Contains("Toggle=121:6\n") && ini.Contains("Toggle=121:5\n") && ini.Contains("UpMmPerS=5\n") && ini.Contains("DownMmPerS=40\n"), "both sections, each with its switch");
    Require(ini.Contains("Ipd=64\n") && Lua(plan).Any(c => c.Path == "VR.custom_IPD_enable" && c.InstalledRaw == "false"), "custom IPD taken over for Depth boost alone");
    // The dual camera's engine module comes with Depth boost alone, in its Perception-only mode.
    var moduleIni = plan.Files.SingleOrDefault(f => f.Path.EndsWith("DcsQvCull.ini"));
    Require(moduleIni is not null && plan.Files.Any(f => f.Path.EndsWith(Path.Combine("Hooks", "DcsQvCull.lua"))), "the engine module and its hook for Depth boost alone");
    var moduleText = Encoding.Unicode.GetString(moduleIni!.Content.AsSpan(2)).ReplaceLineEndings("\n"); // UTF-16 with its BOM
    Require(moduleText.Contains("PerceptionOnly=1\n") && moduleText.Contains("CockpitTrueIpd=1\n"), "Perception-only, dual camera on");
    // Developer mode belongs to Engine Optimizations: its paths are not written for Depth boost alone.
    var dev = on with { EngineDevMode = true, EngineDevPayloadPath = @"D:\dev\payload.dll", EngineDevIniPath = @"D:\dev\dev.ini" };
    var devText = ConfigurationWriters.DcsQvCull(dev).ReplaceLineEndings("\n");
    Require(devText.Contains("PayloadPath=\n") && devText.Contains("IniPath=\n"), "no developer paths without Engine Optimizations");
    Require(ConfigurationWriters.DcsQvCull(dev with { EngineOptimizations = true }).Contains(@"PayloadPath=D:\dev\payload.dll"), "developer paths with them");
    Require(plan.LaunchEnvironment["DCSVR_DEPTH_HOTKEY"] == "0:0" && plan.LaunchEnvironment["DCSVR_PUPIL_HOTKEY"] == "0:0", "no panel without frame generation");
    // Both on: one layer, both enabled. Pupil shift alone keeps its old ini (Depth boost off).
    var both = Ini(planner.Build(on with { PupilShift = true }, inventory, Path.Combine(root, "depth/managed-both")));
    Require(both.Contains("[PupilShift]\nEnabled=1\n") && both.Contains("[DepthBoost]\nEnabled=1\n"), "both");
    var pupilOnly = Ini(planner.Build(on with { PupilShift = true, DepthBoost = false }, inventory, Path.Combine(root, "depth/managed-pupil")));
    Require(pupilOnly.Contains("[PupilShift]\nEnabled=1\n") && pupilOnly.Contains("[DepthBoost]\nEnabled=0\n"), "Pupil shift alone");
    // Without an eye-tracked gaze, or off: no layer, DCS keeps its IPD; the profile says why.
    foreach (var off in new[] { on with { Gaze = GazeMode.Fixed }, on with { QuadViews = QuadProvider.None }, on with { DepthBoost = false }, on with { Desktop = true } })
    {
        var offPlan = planner.Build(off, inventory, Path.Combine(root, "depth/managed-off"));
        Require(!offPlan.Files.Any(f => f.Path.Contains("pupilshift")) && !Lua(offPlan).Any(c => c.Path.StartsWith("VR.custom_IPD")), "not deployed");
        Require(!offPlan.Files.Any(f => f.Path.EndsWith("DcsQvCull.ini")), "no engine module without Depth boost (or Engine Optimizations)");
    }
    Require(ProfileValidation.Validate(on with { Gaze = GazeMode.Fixed }).Any(i => i.Code == "depth-gaze" && i.Severity == IssueSeverity.Warning));
    Require(ProfileValidation.Validate(on with { DepthBoostMax = 2.5 }).Any(i => i.Code == "depth-max" && i.Severity == IssueSeverity.Error));
    Require(ProfileValidation.Validate(on with { DepthBoostMax = 0.9 }).Any(i => i.Code == "depth-max"));
    Require(ProfileValidation.Validate(on with { DepthBoostNearM = 0.2 }).Any(i => i.Code == "depth-near" && i.Severity == IssueSeverity.Error));
    Require(!ProfileValidation.Validate(on).Any(i => i.Code.StartsWith("depth") || i.Code == "hotkey-conflict"), "the defaults are valid, and Alt+Shift+F10 is not a DCS default");
    // The switch: its own key, apart from Pupil shift's and the others.
    Require(ProfileValidation.Validate(on with { PupilShift = true, DepthBoostToggleKey = NeuralHotkeys.PupilDefault }).Any(i => i.Code == "hotkey-conflict"));
    Require(!ProfileValidation.Validate(on with { PupilShift = false, DepthBoostToggleKey = NeuralHotkeys.PupilDefault }).Any(i => i.Code == "hotkey-conflict"));
    Require(ProfileValidation.Validate(on with { EngineOptimizations = true, DepthBoostToggleKey = NeuralHotkeys.EngineDefault }).Any(i => i.Code == "hotkey-conflict"));
    Require(ProfileValidation.Validate(on with { EngineOptimizations = true, DepthBoostToggleKey = "120:3" }).Any(i => i.Code == "depth-hotkey"));
    Require(ConfigurationWriters.PupilShift(on with { DepthBoostToggleKey = "Off" }).ReplaceLineEndings("\n").Split("[DepthBoost]")[1].Contains("Toggle=0:0\n"), "switch off");
    // A profile saved before Depth boost existed reads as off.
    Require(System.Text.Json.JsonSerializer.Deserialize<VrProfile>("{\"PupilShift\":true}") is { DepthBoost: false, DepthBoostMax: 1.4, DepthBoostNearM: 1.5, DepthBoostToggleKey: NeuralHotkeys.DepthBoostDefault });
});
Test("Depth boost modes: Realistic by default (x1 to 150 m, x1.25 from 400 m), Enhanced on request; the engine measures the scene depth with Engine Optimizations", () =>
{
    var realistic = new VrProfile { QuadViews = QuadProvider.PimaxNative, Gaze = GazeMode.EyeTracked, DepthBoost = true };
    Require(realistic is { DepthBoostMode: DepthBoostMode.Realistic, DepthBoostRealisticMax: 1.25, DepthBoostFarStartM: 150, DepthBoostFarFullM: 400, DepthBoostActiveMax: 1.25 }, "Realistic is the default");
    var ini = ConfigurationWriters.PupilShift(realistic).ReplaceLineEndings("\n");
    Require(ini.Contains("Mode=realistic\nMaxScale=1.25\nFarStartM=150\nFarFullM=400\n"), "Realistic in the layer's ini");
    var enhanced = realistic with { DepthBoostMode = DepthBoostMode.Enhanced };
    Require(ConfigurationWriters.PupilShift(enhanced).ReplaceLineEndings("\n").Contains("Mode=enhanced\nMaxScale=1.4\nFarStartM=3\nFarFullM=50\n"), "Enhanced: its own strength and the fixed 3-50 m curve");
    Require(ConfigurationWriters.PupilShift(realistic with { DepthBoostFarStartM = 200, DepthBoostFarFullM = 600, DepthBoostRealisticMax = 1.3 }).ReplaceLineEndings("\n")
        .Contains("MaxScale=1.3\nFarStartM=200\nFarFullM=600\n"), "Realistic's own distances and strength");
    // Validation per mode: Realistic up to x1.5, Enhanced up to x2; the other mode's strength is not checked.
    Require(!ProfileValidation.Validate(realistic).Any(i => i.Code.StartsWith("depth-") && i.Severity == IssueSeverity.Error), "the defaults are valid");
    Require(ProfileValidation.Validate(realistic with { DepthBoostRealisticMax = 1.6 }).Any(i => i.Code == "depth-realistic-max" && i.Severity == IssueSeverity.Error), "Realistic above x1.5");
    Require(!ProfileValidation.Validate(realistic with { DepthBoostMax = 2.5 }).Any(i => i.Code == "depth-max"), "Enhanced's strength is not checked in Realistic");
    Require(!ProfileValidation.Validate(enhanced with { DepthBoostMax = 1.8 }).Any(i => i.Code.StartsWith("depth-") && i.Severity == IssueSeverity.Error), "Enhanced x1.8");
    Require(!ProfileValidation.Validate(enhanced with { DepthBoostRealisticMax = 3, DepthBoostFarStartM = 1 }).Any(i => i.Code is "depth-realistic-max" or "depth-far"), "Realistic's values are not checked in Enhanced");
    Require(ProfileValidation.Validate(realistic with { DepthBoostFarStartM = 40 }).Any(i => i.Code == "depth-far"), "Realistic starts at 50 m or more");
    Require(ProfileValidation.Validate(realistic with { DepthBoostFarFullM = 160 }).Any(i => i.Code == "depth-far"), "the full strength at least 1.2 times the start");
    Require(ProfileValidation.Validate(realistic with { DepthBoostFarFullM = 5000 }).Any(i => i.Code == "depth-far"), "at most 3000 m");
    Require(ProfileValidation.Validate(realistic with { DepthBoostMode = (DepthBoostMode)7 }).Any(i => i.Code == "depth-mode"), "an unknown mode");
    // Profiles: a saved Depth boost profile without a mode reads as Realistic; the mode round-trips as text.
    Require(System.Text.Json.JsonSerializer.Deserialize<VrProfile>("{\"DepthBoost\":true,\"DepthBoostMax\":1.6}") is { DepthBoostMode: DepthBoostMode.Realistic, DepthBoostMax: 1.6, DepthBoostActiveMax: 1.25 }, "old profile");
    var options = new System.Text.Json.JsonSerializerOptions { Converters = { new System.Text.Json.Serialization.JsonStringEnumConverter(allowIntegerValues: false) } };
    var json = System.Text.Json.JsonSerializer.Serialize(enhanced, options);
    Require(json.Contains("\"DepthBoostMode\":\"Enhanced\"") && System.Text.Json.JsonSerializer.Deserialize<VrProfile>(json, options)!.DepthBoostMode == DepthBoostMode.Enhanced, "mode as text");
    // The engine module samples the depth buffer only for Depth boost with Engine Optimizations ([Perception] FixationDepth).
    Require(ConfigurationWriters.DcsQvCull(realistic with { EngineOptimizations = true }).ReplaceLineEndings("\n").Contains("[Perception]\n") &&
        ConfigurationWriters.DcsQvCull(realistic with { EngineOptimizations = true }).ReplaceLineEndings("\n").Contains("FixationDepth=1\n"), "FixationDepth=1 with Depth boost");
    Require(ConfigurationWriters.DcsQvCull(realistic with { EngineOptimizations = true, DepthBoost = false }).ReplaceLineEndings("\n").Contains("FixationDepth=0\n"), "off without Depth boost");
    Require(ConfigurationWriters.DcsQvCull(realistic with { EngineOptimizations = true, Gaze = GazeMode.Fixed }).ReplaceLineEndings("\n").Contains("FixationDepth=0\n"), "off when Depth boost cannot run");
    // The dual camera: Depth boost always has the engine module, with Engine Optimizations or in its Perception-only mode.
    Require(realistic.UsesDepthBoost && realistic.UsesFixationDepth && realistic.UsesEngineModule && !(realistic with { DepthBoost = false }).UsesEngineModule, "Depth boost installs the module");
    var perceptionOnly = ConfigurationWriters.DcsQvCull(realistic with { EngineOptimizations = false }).ReplaceLineEndings("\n");
    Require(perceptionOnly.Contains("PerceptionOnly=1\n") && perceptionOnly.Contains("FixationDepth=1\n") && perceptionOnly.Contains("CockpitTrueIpd=1\n")
        && perceptionOnly.Contains("CockpitTrueIpdOverlayPath=cpu\n") && perceptionOnly.Contains("CockpitTrueIpdOwnship=2\n") && perceptionOnly.Contains("CockpitTrueIpdToggle=0:0\n"), "Perception-only with the dual camera");
    var withEngine = ConfigurationWriters.DcsQvCull(realistic with { EngineOptimizations = true, OwnAircraftTrueDepth = false }).ReplaceLineEndings("\n");
    Require(withEngine.Contains("PerceptionOnly=0\n") && withEngine.Contains("CockpitTrueIpd=1\n") && withEngine.Contains("CockpitTrueIpdOwnship=0\n"), "with the engine; own aircraft off");
    Require(ConfigurationWriters.DcsQvCull(realistic with { EngineOptimizations = true, DepthBoost = false }).ReplaceLineEndings("\n").Contains("CockpitTrueIpd=0\n"), "no dual camera without Depth boost");
});
Test("Depth boost with the dual camera: always gradual, no transition switch; the engine's depth grid stays", () =>
{
    var on = new VrProfile { QuadViews = QuadProvider.PimaxNative, Gaze = GazeMode.EyeTracked, DepthBoost = true, EngineOptimizations = true };
    var predictive = on with { DepthBoostTransition = DepthBoostTransition.Predictive, DepthBoostTransitionKey = NeuralHotkeys.DepthBoostDefault };
    // The dual camera only moves the scale smoothly: a profile saved with Predictive and its key writes neither.
    foreach (var profile in new[] { on, predictive })
    {
        var ini = ConfigurationWriters.PupilShift(profile).ReplaceLineEndings("\n");
        Require(ini.Contains("Transition=gradual\n") && ini.Contains("TransitionToggle=0:0\n"), "gradual, no switch");
    }
    Require(!ProfileValidation.Validate(predictive).Any(i => i.Code.StartsWith("depth-transition") || i.Code == "hotkey-conflict"), "the old key is not checked");
    // The grid served only the predictive transition: not captured any more; the scene depth at the gaze stays.
    var engineIni = ConfigurationWriters.DcsQvCull(on with { EngineOptimizations = false }).ReplaceLineEndings("\n");
    Require(engineIni.Contains("DepthGrid=0\n") && engineIni.Contains("FixationDepth=1\n"), "no grid, the fixation depth");
    // Profiles: an older profile still loads; the transition round-trips as text.
    Require(System.Text.Json.JsonSerializer.Deserialize<VrProfile>("{\"DepthBoost\":true}") is { DepthBoostTransition: DepthBoostTransition.Gradual, DepthBoostTransitionKey: "120:6" }, "old profile");
    var options = new System.Text.Json.JsonSerializerOptions { Converters = { new System.Text.Json.Serialization.JsonStringEnumConverter(allowIntegerValues: false) } };
    var json = System.Text.Json.JsonSerializer.Serialize(predictive, options);
    Require(json.Contains("\"DepthBoostTransition\":\"Predictive\"") && System.Text.Json.JsonSerializer.Deserialize<VrProfile>(json, options)!.DepthBoostTransition == DepthBoostTransition.Predictive, "as text");
});
Test("Visual realism: all off writes the ini as before; each mode, the key and [VrFx] write their keys; the module for a visual feature alone", () =>
{
    var vr = new VrProfile { QuadViews = QuadProvider.PimaxNative, Gaze = GazeMode.EyeTracked };
    string Ini(VrProfile p) => ConfigurationWriters.DcsQvCull(p).ReplaceLineEndings("\n");
    // Everything off (or unable to run): [Color] exactly as before Visual realism, no new section, and no value leaks in.
    foreach (var off in new[] { new VrProfile(), vr with { EngineOptimizations = true }, vr with { DepthBoost = true }, vr with { GazeDof = true, Gaze = GazeMode.Fixed },
        vr with { Desktop = true, EngineOptimizations = true, ColorMode = ColorMode.Enhanced, GVision = true, ColorModeKey = "120:6" } })
    {
        var ini = Ini(off);
        Require(ini.Contains("\n\n[Color]\nModeKey=0:0\nToneProbe=0\nSwapProbe=0\n\n[Log]\n") && !ini.Contains("[ColorFix]") && !ini.Contains("[ColorEnhanced]") && !ini.Contains("[VrFx]"), "all off: [Color] as before");
        Require(!off.UsesVisualRealism && ConfigurationWriters.DcsQvCull(off) == ConfigurationWriters.DcsQvCull(off with
        {
            ColorFixExposureEv = 1, ColorFixSaturation = 1.2, ColorEnhancedWarmth = 0.9,
            NightVision = false, NightVisionStrength = 0.1, Glare = false, GlareRadius = 9, GazeDofStrength = 1, GazeDofSettleMs = 300, GVisionStrength = 0.4,
            GVisionMode = GVisionMode.Full, GVisionUseGaze = false
        }), "values of features that are off change nothing");
    }
    Require(!new VrProfile().UsesEngineModule && !(vr with { GazeDof = true, Gaze = GazeMode.Fixed }).UsesEngineModule, "nothing installed while everything is off");
    // Fix: Mode=1 and both modes' values (the module's defaults); the tuning curve, developer probes and [VrFx] are not written.
    var fix = Ini(vr with { ColorMode = ColorMode.Fix });
    Require(fix.Contains("\n[Color]\nMode=1\nModeKey=0:0\nToneProbe=0\nSwapProbe=0\n\n; Color modes"), "Fix: [Color]");
    Require(fix.Contains("\n[ColorFix]\nExposureEV=0\nNightEV=0\nSaturation=1\nWarmth=0\nAdaptBright=0\nAdaptDark=0\n\n[ColorEnhanced]\nExposureEV=0\nNightEV=0\nSaturation=1.15\nWarmth=0.35\nAdaptBright=0.4\nAdaptDark=1.5\n"
        + "NightVision=1\nNightVisionStrength=0.8\nNightVisionBlue=0.4\nNightVisionBlur=0.25\nNightVisionScotopic=0.005\nNightVisionPhotopic=5\nGlare=1\nGlareStrength=0.35\nGlareThresholdEV=3.5\nGlareRadius=5\nGlareStreak=0.3\n\n[Log]\n"), "Fix: each mode's defaults");
    foreach (var key in new[] { "Slope=", "Toe=", "Shoulder=", "WhiteClip=", "HueKeep=", "HueShadow=", "ChromaNorm=", "WhiteStart=", "GamutKeepSat=", "DarkDesat=", "Dither=", "MeterFocus=", "SrgbMatch=", "EqualizeEc=", "NightFloor=", "NightGate=", "GlareDebug=", "\nTiming=", "ToneProbeViews=", "ToneProbeHdr=" })
        Require(!fix.Contains(key), "never written: " + key);
    Require(!fix.Contains("[VrFx]") && fix.Contains("PerceptionOnly=1\n"), "Fix alone: Perception-only module, no [VrFx]");
    // Enhanced with its own values, written with invariant decimals whatever the PC's language.
    var previous = System.Globalization.CultureInfo.CurrentCulture;
    System.Globalization.CultureInfo.CurrentCulture = new System.Globalization.CultureInfo("it-IT");
    try
    {
        var enhanced = Ini(vr with { ColorMode = ColorMode.Enhanced, EngineOptimizations = true, ColorEnhancedExposureEv = -0.5, ColorEnhancedSaturation = 1.25, NightVision = false, GlareStrength = 0.55, NightVisionScotopic = 0.0125 });
        Require(enhanced.Contains("[Color]\nMode=2\n") && enhanced.Contains("PerceptionOnly=0\n") && enhanced.Contains("[ColorEnhanced]\nExposureEV=-0.5\nNightEV=0\nSaturation=1.25\n")
            && enhanced.Contains("NightVision=0\n") && enhanced.Contains("NightVisionScotopic=0.0125\n") && enhanced.Contains("GlareStrength=0.55\n") && !enhanced.Contains(",5"), "Enhanced: its values, invariant decimals");
    }
    finally { System.Globalization.CultureInfo.CurrentCulture = previous; }
    // Off with the key: Mode=0, the key, and both modes' values (the key reaches either in flight).
    var keyed = Ini(vr with { ColorModeKey = "120:6" });
    Require(keyed.Contains("[Color]\nMode=0\nModeKey=120:6\n") && keyed.Contains("[ColorFix]\n") && keyed.Contains("[ColorEnhanced]\n"), "Off with the key");
    Require(Ini(vr with { ColorMode = ColorMode.Fix, ColorModeKey = "Off" }).Contains("ModeKey=0:0\n"), "key off");
    // [VrFx]: Gaze DoF only with Depth boost's gaze; G vision on its own; every key written.
    var dof = Ini(vr with { DepthBoost = true, GazeDof = true, GazeDofStrength = 0.7, GazeDofPupilMm = 4 });
    Require(dof.Contains("\n[VrFx]\nGazeDof=1\nGazeDofStrength=0.7\nGazeDofMaxArcmin=6\nGazeDofSaccadeRelease=1\nGazeDofSettleMs=120\nGazeDofFadeInMs=250\nGazeDofToleranceD=0.3\nGazeDofFoveaGuardDeg=2\nGazeDofPupilMm=4\n"
        + "GVision=0\nGVisionStrength=1\nGVisionUseGaze=1\nGVisionRedout=1\nGVisionTolerance=7.5\nGVisionMode=0\n\n[Log]\n") && !dof.Contains("[ColorFix]") && !dof.Contains("\nMode=0\nModeKey"), "Gaze DoF");
    var g = vr with { GVision = true, GVisionMode = GVisionMode.Complement };
    var gIni = Ini(g);
    Require(g.UsesEngineModule && gIni.Contains("PerceptionOnly=1\n") && gIni.Contains("GazeDof=0\n") && gIni.Contains("GVision=1\n") && gIni.Contains("GVisionMode=1\n")
        && gIni.Contains("\n[Color]\nModeKey=0:0\nToneProbe=0\nSwapProbe=0\n\n; Gaze DoF"), "G vision alone");
    // The module (loader, payload, ini and Lua hook) is deployed for a visual feature alone, in its Perception-only mode.
    var exe = Make("visual/game/bin/DCS.exe", "fixture"); var runtime = Make("visual/runtime.json", "{\"runtime\":{\"library_path\":\"runtime.dll\"}}"); Make("visual/runtime.dll", "fixture");
    var engine = Path.GetDirectoryName(Make("visual/components/dcsqvcull/DcsQvCull.dll", "loader"))!;
    Make("visual/components/dcsqvcull/DcsQvCullPayload.dll", "payload"); Make("visual/components/dcsqvcull/DcsQvCull.lua", "-- hook");
    var layerDll = Make("visual/components/pupilshift/XR_APILAYER_DCSVR_pupil_shift.dll", "layer");
    var planner = new DeploymentPlanner(new(null, null, null, PupilShiftDll: layerDll, EngineDirectory: engine));
    var inventory = new InventorySnapshot { DcsExecutable = exe, OptionsPath = Make("visual/sg/DCS/Config/options.lua", "options = {\n\t[\"VR\"] = {\n\t\t[\"enable\"] = true,\n\t},\n}\n"), PimaxRuntime = runtime };
    foreach (var (on, index) in new[] { vr with { ColorMode = ColorMode.Enhanced }, vr with { ColorModeKey = "120:6" }, g }.Select((p, i) => (p, i)))
    {
        var plan = planner.Build(on, inventory, Path.Combine(root, "visual/managed-" + index));
        var module = plan.Files.SingleOrDefault(f => f.Path.EndsWith("DcsQvCull.ini"));
        Require(module is not null && plan.Files.Any(f => f.Path.EndsWith(Path.Combine("Hooks", "DcsQvCull.lua"))) && plan.Files.Any(f => f.Path.EndsWith("DcsQvCullPayload.dll")), "the module for a visual feature alone");
        Require(Encoding.Unicode.GetString(module!.Content.AsSpan(2)).ReplaceLineEndings("\n").Contains("PerceptionOnly=1\n"), "in its Perception-only mode");
    }
    foreach (var off in new[] { vr, vr with { GazeDof = true, Gaze = GazeMode.Fixed }, vr with { Desktop = true, ColorMode = ColorMode.Fix, GVision = true } })
        Require(!planner.Build(off, inventory, Path.Combine(root, "visual/managed-off")).Files.Any(f => f.Path.EndsWith("DcsQvCull.ini")), "no module while nothing visual runs");
});
Test("Visual realism: the pupil shift layer publishes the gaze for eye-focus blur and G vision, without Depth boost and without taking DCS's IPD", () =>
{
    var vr = new VrProfile { QuadViews = QuadProvider.PimaxNative, Gaze = GazeMode.EyeTracked };
    string Pupil(VrProfile p) => ConfigurationWriters.PupilShift(p).ReplaceLineEndings("\n");
    // No gaze needed: the layer's file exactly as before, for every profile that deploys the layer.
    foreach (var p in new[] { vr with { PupilShift = true }, vr with { DepthBoost = true }, vr with { DepthBoost = true, GVision = true, GVisionUseGaze = false }, vr with { PupilShift = true, GazeDof = true, Gaze = GazeMode.Fixed } })
        Require(!p.UsesGazePublish && !Pupil(p).Contains("PublishGaze") && Pupil(p) == Pupil(p with { GazeDof = false, GVision = false }), "no PublishGaze line while no gaze is needed");
    foreach (var p in new[] { vr with { GazeDof = true }, vr with { GVision = true }, vr with { DepthBoost = true, GazeDof = true }, vr with { PupilShift = true, GVision = true } })
        Require(p.UsesGazePublish && p.UsesPerceptionLayer && Pupil(p).Contains("SimulateGazeYDeg=0\n\n; The gaze for the engine module's eye-focus blur and G vision, also without Depth boost.\nPublishGaze=1\n\n; Depth boost"), "PublishGaze=1");
    Require(!(vr with { GVision = true, GVisionUseGaze = false }).UsesPerceptionLayer && !(vr with { GazeDof = true, Gaze = GazeMode.Fixed }).UsesPerceptionLayer
        && !(vr with { GVision = true, QuadViews = QuadProvider.None }).UsesGazePublish && !(vr with { GazeDof = true, Desktop = true }).UsesPerceptionLayer, "no layer when no gaze is needed or there is none");
    Require(ConfigurationWriters.DcsQvCull(vr with { GazeDof = true }).ReplaceLineEndings("\n").Contains("[VrFx]\nGazeDof=1\n"), "GazeDof=1 without Depth boost");
    // Eye-focus blur alone: the engine module and the layer (Pupil shift and Depth boost off, PublishGaze on); DCS's
    // custom IPD stays DCS's, since the layer moves nothing.
    var exe = Make("publish/game/bin/DCS.exe", "fixture"); var runtime = Make("publish/runtime.json", "{\"runtime\":{\"library_path\":\"runtime.dll\"}}"); Make("publish/runtime.dll", "fixture");
    var engine = Path.GetDirectoryName(Make("publish/components/dcsqvcull/DcsQvCull.dll", "loader"))!;
    Make("publish/components/dcsqvcull/DcsQvCullPayload.dll", "payload"); Make("publish/components/dcsqvcull/DcsQvCull.lua", "-- hook");
    var planner = new DeploymentPlanner(new(null, null, null, PupilShiftDll: Make("publish/components/pupilshift/XR_APILAYER_DCSVR_pupil_shift.dll", "layer"), EngineDirectory: engine));
    var custom = Make("publish/sg/DCS/Config/options.lua", "options = {\n\t[\"VR\"] = {\n\t\t[\"custom_IPD\"] = \"64\",\n\t\t[\"custom_IPD_enable\"] = true,\n\t\t[\"enable\"] = true,\n\t},\n}\n");
    var inventory = new InventorySnapshot { DcsExecutable = exe, OptionsPath = custom, PimaxRuntime = runtime };
    var on = vr with { GazeDof = true, EyeDistance = EyeDistanceSource.DcsCustom };
    var plan = planner.Build(on, inventory, Path.Combine(root, "publish/managed"));
    var layerIni = Encoding.UTF8.GetString(plan.Files.Single(f => f.Path.EndsWith("PupilShift.ini")).Content).ReplaceLineEndings("\n");
    Require(plan.LaunchEnvironment["XR_ENABLE_API_LAYERS"].Split(';')[0] == "XR_APILAYER_DCSVR_pupil_shift" && plan.Files.Any(f => f.Path.EndsWith("DcsQvCull.ini")), "layer and module deployed");
    Require(layerIni.Contains("[PupilShift]\nEnabled=0\n") && layerIni.Contains("PublishGaze=1\n") && layerIni.Contains("[DepthBoost]\nEnabled=0\n") && layerIni.Contains("Ipd=0\n"), "the layer only publishes the gaze");
    Require(!(plan.Files.SingleOrDefault(f => f.Path.EndsWith("options.lua"))?.LuaChanges ?? []).Any(c => c.Path.StartsWith("VR.custom_IPD")), "DCS's custom IPD left alone");
    var withPupil = planner.Build(on with { PupilShift = true }, inventory, Path.Combine(root, "publish/managed-pupil"));
    Require((withPupil.Files.Single(f => f.Path.EndsWith("options.lua")).LuaChanges ?? []).Any(c => c.Path == "VR.custom_IPD_enable" && c.InstalledRaw == "false"), "with Pupil shift the layer still takes over DCS's IPD");
    Require(!planner.Build(vr with { GVision = true, GVisionUseGaze = false }, inventory, Path.Combine(root, "publish/managed-g")).Files.Any(f => f.Path.Contains("pupilshift")), "G vision straight ahead needs no layer");
});
Test("Visual realism: an older profile loads unchanged, the settings round-trip, and out-of-range values are refused", () =>
{
    // A profile saved before Visual realism: everything off, the module's defaults, the same ini.
    var old = JsonData.Deserialize<VrProfile>("{\"engineOptimizations\":true,\"focusDlssPreset\":11}");
    Require(old is { ColorMode: ColorMode.Off, ColorModeKey: "Off", NightVision: true, Glare: true, GazeDof: false, GVision: false, GVisionMode: GVisionMode.Auto, ColorEnhancedSaturation: 1.15, GazeDofSettleMs: 120, GVisionTolerance: 7.5 }, "old profile: off, module defaults");
    Require(ConfigurationWriters.DcsQvCull(old) == ConfigurationWriters.DcsQvCull(new VrProfile { EngineOptimizations = true, FocusDlssPreset = 11 }), "old profile: the same ini");
    var set = new VrProfile { ColorMode = ColorMode.Enhanced, ColorModeKey = "120:6", GVision = true, GVisionMode = GVisionMode.Full, GazeDof = true, GlareRadius = 7 };
    var json = JsonData.Serialize(set);
    Require(json.Contains("\"colorMode\": \"Enhanced\"") && json.Contains("\"gVisionMode\": \"Full\"") && JsonData.Deserialize<VrProfile>(json) == set, "round trip, enums as text");
    var vr = new VrProfile { QuadViews = QuadProvider.PimaxNative, Gaze = GazeMode.EyeTracked };
    bool Refused(VrProfile p, string code) => ProfileValidation.Validate(p).Any(i => i.Code == code && i.Severity == IssueSeverity.Error);
    Require(!ProfileValidation.Validate(vr with { ColorMode = ColorMode.Enhanced, GVision = true, DepthBoost = true, GazeDof = true }).Any(i => i.Code.StartsWith("visual-") || i.Code.StartsWith("color-") || i.Code == "hotkey-conflict"), "the defaults are valid");
    Require(Refused(vr with { ColorFixSaturation = 2 }, "visual-colorFixSaturation") && Refused(vr with { ColorEnhancedExposureEv = -3 }, "visual-colorEnhancedExposureEv")
        && Refused(vr with { ColorEnhancedAdaptDark = 31 }, "visual-colorEnhancedAdaptDark") && Refused(vr with { NightVisionStrength = double.NaN }, "visual-nightVisionStrength")
        && Refused(vr with { GlareRadius = 0.5 }, "visual-glareRadius") && Refused(vr with { GlareThresholdEv = 9 }, "visual-glareThresholdEv")
        && Refused(vr with { GazeDofSettleMs = 600 }, "visual-gazeDofSettleMs") && Refused(vr with { GazeDofFadeInMs = 20 }, "visual-gazeDofFadeInMs")
        && Refused(vr with { GVisionTolerance = 11 }, "visual-gVisionTolerance") && Refused(vr with { ColorMode = (ColorMode)5 }, "visual-colorMode")
        && Refused(vr with { GVisionMode = (GVisionMode)3 }, "visual-gVisionMode"), "out of range");
    Require(Refused(vr with { NightVisionScotopic = 0.1, NightVisionPhotopic = 0.3 }, "visual-nightVisionPhotopic") && !Refused(vr with { NightVisionScotopic = 0.1, NightVisionPhotopic = 0.4 }, "visual-nightVisionPhotopic"), "daylight at least 4 times night");
    Require(Refused(vr with { GazeDofPupilMm = 1 }, "visual-gazeDofPupilMm") && Refused(vr with { GazeDofPupilMm = 9 }, "visual-gazeDofPupilMm")
        && !Refused(vr with { GazeDofPupilMm = 0 }, "visual-gazeDofPupilMm") && !Refused(vr with { GazeDofPupilMm = 6.5 }, "visual-gazeDofPupilMm"), "pupil 0 (automatic) or 2-8 mm");
    // Gaze DoF without Depth boost's gaze: said, not refused.
    Require(ProfileValidation.Validate(vr with { GazeDof = true, Gaze = GazeMode.Fixed }).Any(i => i.Code == "visual-gaze" && i.Severity == IssueSeverity.Warning)
        && ProfileValidation.Validate(vr with { GazeDof = true, QuadViews = QuadProvider.None }).Any(i => i.Code == "visual-gaze")
        && !ProfileValidation.Validate(vr with { GazeDof = true }).Any(i => i.Code == "visual-gaze"), "Gaze DoF needs an eye-tracked focus, not Depth boost");
    // The color mode key: a key of its own, never the engine module's.
    Require(ProfileValidation.Validate(vr with { ColorModeKey = "121:6", DepthBoost = true }).Any(i => i.Code == "hotkey-conflict" && i.Message.StartsWith("The color mode key")), "not Depth boost's key");
    Require(Refused(vr with { ColorModeKey = "123:3", EngineOptimizations = true }, "color-hotkey") && Refused(vr with { ColorModeKey = "nonsense" }, "color-hotkey"), "reserved or unreadable keys");
    Require(!ProfileValidation.Validate(vr with { ColorModeKey = "120:6" }).Any(i => i.Code.StartsWith("color-hotkey") || i.Code == "hotkey-conflict"), "Alt+Shift+F9 is free");
});
Test("The Pupil shift page reads Depth boost's transition: selected, the switch, the predict: line's steps, fallback", () =>
{
    var folder = Path.Combine(root, "depth-transition-status"); Directory.CreateDirectory(folder);
    File.WriteAllText(Path.Combine(folder, PupilShiftStatus.LayerDll), "layer");
    var log = Path.Combine(folder, PupilShiftStatus.LogName);
    const string settings = "10:00:00.100 depth settings: enabled=1 mode=realistic max_scale=1.25 far=150..400m near=1.50m up=5.0mm/s down=40.0mm/s saccade_step=4.0mm recover=800ms hold=500ms pvr=1 key=121:6 transition=gradual transition_key=120:6 predict min=8.0deg max_step=12.0mm disparity=0.40deg lead=0.50frames poll=500Hz";
    const string depth = "10:00:02.000 depth: on source=depth vergence=vergence fixation=far distance 500.00 m [480.00..520.00] scale 1.200 [1.000..1.200] target 1.250; frames=45 near=0 far=45 unknown=0 no_gaze=0 boosted=40 saccades=1 rejected=4; scene mode=realistic fresh=40 stale=2 moved=1 absent=0 shares near 0.02 far 0.97 ring 0.10/0.85 far_median 500.0 m edge=0 ambiguous=3 vetoes=0 depth_over_vergence=0 changes=1 landing_steps=0; pvr valid=45 invalid=0 eye_invalid=0 convergence 0.00 m; IPD base 64.0 mm, render 76.8 mm";
    const string predict = "10:00:02.000 predict: transition=predictive in effect samples=500 rate 500 Hz saccades=3 dropped=0 predicted=3 steps=2 mean_step 8.44 mm confirmed=1 contradicted=1 expired=0 skipped short=1 no_grid=0 edge=0 ambiguous=0 outside=0 late=0 same=0; gain 1.02; grid fresh=45 stale=0";
    File.WriteAllText(log, settings + "\n" + depth + "\n");
    Require(PupilShiftStatus.Read(folder, true).Depth is { Transition: "gradual", TransitionHotkey: "120:6", Predictive: null }, "gradual from the settings line");
    File.WriteAllText(log, settings + "\n10:00:01.000 hotkey: transition predictive\n" + depth + "\n" + predict + "\n" + predict + "\n");
    var status = PupilShiftStatus.Read(folder, true).Depth;
    Require(status is { Transition: "predictive", PredictiveSteps: 4, PredictiveConfirmed: 2, PredictiveContradicted: 2, PredictiveSaccades: 6,
        Predictive: { InEffect: true, Steps: 2, MeanStepMm: 8.44, Short: 1, Confirmed: 1 } }, "the switch selected predictive; its steps counted");
    File.WriteAllText(log, settings.Replace("transition=gradual", "transition=predictive") + "\n" + depth + "\n" +
        "10:00:02.000 predict: transition=predictive fallback gradual: no usable Pimax PVR client (per-eye gaze) samples=0 rate 0 Hz saccades=0 dropped=0 predicted=0 steps=0 mean_step 0.00 mm confirmed=0 contradicted=0 expired=0 skipped short=0 no_grid=0 edge=0 ambiguous=0 outside=0 late=0 same=0; gain 1.00; grid fresh=0 stale=0\n");
    Require(PupilShiftStatus.Read(folder, true).Depth is { Transition: "predictive", Predictive: { InEffect: false, Reason: "no usable Pimax PVR client (per-eye gaze)" } }, "fallback and why");
    // An older log without the transition: none shown.
    File.WriteAllText(log, "10:00:00.100 depth settings: enabled=1 max_scale=1.40 near=1.50m up=5.0mm/s down=40.0mm/s saccade_step=4.0mm recover=800ms hold=500ms key=121:6\n" + depth + "\n");
    Require(PupilShiftStatus.Read(folder, true).Depth is { Transition: null, Hotkey: "121:6" }, "older log");
});
Test("The Pupil shift page reads Depth boost from the layer's log: working, probing, no depth, switched off, Pupil shift off", () =>
{
    var folder = Path.Combine(root, "depth-status"); Directory.CreateDirectory(folder);
    File.WriteAllText(Path.Combine(folder, PupilShiftStatus.LayerDll), "layer");
    var log = Path.Combine(folder, PupilShiftStatus.LogName);
    string Depth(string state, string source, string fixation, double scale, double max, int near, int far, int boosted) => string.Create(System.Globalization.CultureInfo.InvariantCulture,
        $"depth: {state} source={source} fixation={fixation} distance 25.00 m [0.70..999.00] scale {scale:0.000} [1.000..{max:0.000}] target {max:0.000}; frames=90 near={near} far={far} unknown=0 no_gaze=0 boosted={boosted} saccades=2 rejected=9; IPD base 64.0 mm, render {64 * scale:0.0} mm");
    File.WriteAllText(log, string.Join("\n",
        "10:00:00.000 XR_APILAYER_DCSVR_pupil_shift loaded",
        "10:00:00.100 settings: enabled=0 eye_radius=10.5mm virtual_image=1.50m max_gaze=35deg simulate=0 (0.0, 0.0) toggle=121:5 ipd=64.0mm",
        "10:00:00.100 depth settings: enabled=1 max_scale=1.40 near=1.50m up=5.0mm/s down=40.0mm/s saccade_step=4.0mm recover=800ms hold=500ms key=121:6",
        "10:00:01.000 end_frame: restored 4 projection views",
        "10:00:02.000 stats: frames=90 shifted=0 (pupil shift disabled); runtime left eye vs head x[-32.75..-32.75] mm; cant 6.0 deg, runtime IPD 65.5 mm, rendered IPD 64.0 mm",
        Depth("on", "probing", "near", 1, 1, 90, 0, 0),
        "10:00:03.000 stats: frames=90 shifted=0 (pupil shift disabled); runtime left eye vs head x[-32.75..-32.75] mm; cant 6.0 deg, runtime IPD 65.5 mm, rendered IPD 70.0 mm",
        Depth("on", "vergence", "far", 1.25, 1.25, 0, 90, 80)) + "\n");
    var status = PupilShiftStatus.Read(folder, false);
    Require(status.State == "disabled" && status.Hotkey == "121:5", "Pupil shift off, the layer runs for Depth boost");
    Require(status.Depth is { State: "working", SessionMaxScale: 1.25, Seconds: 2, Hotkey: "121:6" } d && Math.Abs(d.SecondsBoosted - 80.0 / 90) < 1e-9 && d.SecondsNear == 1 && d.Last is { RenderIpdMm: 80.0, Source: "vergence", Fixation: "far" }, "working");
    File.AppendAllText(log, Depth("off (switch)", "vergence", "far", 1, 1.25, 0, 90, 0) + "\n");
    Require(PupilShiftStatus.Read(folder, true).Depth?.State == "off", "switched off");
    File.WriteAllText(log, "10:00:00.100 depth settings: enabled=1 max_scale=1.40 near=1.50m up=5.0mm/s down=40.0mm/s saccade_step=4.0mm recover=800ms hold=500ms key=121:6\n" + Depth("on", "constant", "near", 1, 1, 90, 0, 0) + "\n");
    Require(PupilShiftStatus.Read(folder, true).Depth?.State == "no-depth", "one shared gaze: no depth");
    File.WriteAllText(log, "10:00:00.100 depth settings: enabled=1 max_scale=1.40 near=1.50m up=5.0mm/s down=40.0mm/s saccade_step=4.0mm recover=800ms hold=500ms key=121:6\n" + Depth("on", "probing", "far", 1, 1, 0, 90, 0) + "\n");
    Require(PupilShiftStatus.Read(folder, true).Depth?.State == "probing", "waiting for both distances");
    // Pupil shift alone: no Depth boost section on the page.
    File.WriteAllText(log, "10:00:00.100 settings: enabled=1 eye_radius=10.5mm virtual_image=1.50m max_gaze=35deg simulate=0 (0.0, 0.0) toggle=121:5 ipd=0.0mm\n10:00:00.100 depth settings: enabled=0 max_scale=1.40 near=1.50m up=5.0mm/s down=40.0mm/s saccade_step=4.0mm recover=800ms hold=500ms key=121:6\n");
    Require(PupilShiftStatus.Read(folder, true) is { Depth: null, Hotkey: "121:5", State: "waiting" }, "Depth boost not on in the layer: no section");
});
Test("The Pupil shift page reads Depth boost's scene depth line: source, vergence, mode, edges, vetoes; Realistic without the scene depth", () =>
{
    var folder = Path.Combine(root, "depth-scene-status"); Directory.CreateDirectory(folder);
    File.WriteAllText(Path.Combine(folder, PupilShiftStatus.LayerDll), "layer");
    var log = Path.Combine(folder, PupilShiftStatus.LogName);
    const string settings = "10:00:00.100 depth settings: enabled=1 mode=realistic max_scale=1.25 far=150..400m near=1.50m up=5.0mm/s down=40.0mm/s saccade_step=4.0mm recover=800ms hold=500ms pvr=1 key=121:6";
    string Line(string source, string vergence, string mode, double scale, int fresh, int edge, int vetoes) => string.Create(System.Globalization.CultureInfo.InvariantCulture,
        $"10:00:02.000 depth: on source={source} vergence={vergence} fixation=far distance 500.00 m [480.00..520.00] scale {scale:0.000} [1.000..{scale:0.000}] target 1.250; frames=90 near=0 far=90 unknown=0 no_gaze=0 boosted=80 saccades=1 rejected=4; scene mode={mode} fresh={fresh} stale=2 moved=1 absent=0 shares near 0.02 far 0.97 ring 0.10/0.85 far_median 500.0 m edge={edge} ambiguous=3 vetoes={vetoes} depth_over_vergence=0 changes=1 landing_steps=0; pvr valid=88 invalid=2 eye_invalid=1 convergence 0.00 m; IPD base 64.0 mm, render {64 * scale:0.0} mm");
    File.WriteAllText(log, settings + "\n" + Line("depth", "vergence", "realistic", 1.2, 88, 5, 2) + "\n");
    var status = PupilShiftStatus.Read(folder, true);
    Require(status.Depth is { State: "working", Last: { Source: "depth", Vergence: "vergence", Mode: "realistic", SceneFresh: 88, EdgeFrames: 5, Vetoes: 2, RenderIpdMm: 76.8 } } d
        && d.Summary.Contains("scene depth"), status.Depth?.State + " " + status.Depth?.Summary);
    // Realistic on the vergence alone (no engine module): normal depth, and the page says why.
    File.WriteAllText(log, settings + "\n" + Line("vergence", "vergence", "realistic", 1.0, 0, 0, 0) + "\n");
    Require(PupilShiftStatus.Read(folder, true).Depth is { State: "no-scene" } ns && ns.Summary.Contains("Engine Optimizations"), "Realistic without the scene depth");
    // Enhanced on the vergence alone works as before.
    File.WriteAllText(log, settings + "\n" + Line("vergence", "vergence", "enhanced", 1.3, 0, 0, 0) + "\n");
    Require(PupilShiftStatus.Read(folder, true).Depth?.State == "working", "Enhanced on vergence");
    // Neither source: the vergence's state decides (one shared gaze, or still probing).
    File.WriteAllText(log, settings + "\n" + Line("none", "constant", "enhanced", 1.0, 0, 0, 0) + "\n");
    Require(PupilShiftStatus.Read(folder, true).Depth?.State == "no-depth", "no source, one shared gaze");
    File.WriteAllText(log, settings + "\n" + Line("none", "probing", "enhanced", 1.0, 0, 0, 0) + "\n");
    Require(PupilShiftStatus.Read(folder, true).Depth?.State == "probing", "no source, probing");
    // After the flight the whole flight counts: the scene depth boosted, so it worked.
    File.WriteAllText(log, settings + "\n" + Line("depth", "probing", "realistic", 1.25, 90, 0, 0) + "\n" + Line("none", "probing", "realistic", 1.0, 0, 0, 0) + "\n");
    Require(PupilShiftStatus.Read(folder, false).Depth is { State: "working", SessionMaxScale: 1.25 }, "worked after the flight");
});
Test("The Pupil shift page reads the layer's log: working, no gaze, switched off, not installed", () =>
{
    var folder = Path.Combine(root, "pupil-status"); Directory.CreateDirectory(folder);
    Require(PupilShiftStatus.Read(folder, false).State == "not-installed" && PupilShiftStatus.Read(null, true).State == "not-installed");
    File.WriteAllText(Path.Combine(folder, PupilShiftStatus.LayerDll), "layer");
    Require(PupilShiftStatus.Read(folder, false).State == "waiting");
    var log = Path.Combine(folder, PupilShiftStatus.LogName);
    File.WriteAllText(log, string.Join("\n",
        "10:00:00.000 XR_APILAYER_DCSVR_pupil_shift loaded",
        "10:00:00.100 settings: enabled=1 eye_radius=10.5mm virtual_image=1.50m max_gaze=35deg simulate=0 (0.0, 0.0) toggle=121:5",
        "10:00:01.000 end_frame: restored 4 projection views",
        "10:00:02.000 stats: frames=90 shifted=90 restored=360 gaze x[-12.4..18.0] y[-6.0..4.1] deg, max shift 3.21 mm; runtime left eye vs head x[-31.20..-31.20] z[0.00..0.00] mm",
        "10:00:03.000 stats: frames=90 shifted=90 restored=360 gaze x[-20.0..5.0] y[-2.0..9.5] deg, max shift 3.80 mm; runtime left eye vs head x[-31.20..-31.20] z[0.00..0.00] mm") + "\n");
    var working = PupilShiftStatus.Read(folder, false);
    Require(working.State == "working" && working.SessionGazeX == new PupilRange(-20, 18) && working.SessionGazeY == new PupilRange(-6, 9.5) && working.SessionMaxShiftMm == 3.8, "session ranges");
    Require(working.SecondsWithGaze == 2 && working.Seconds == 2 && working.RestoredViews == 360 && working.Hotkey == "121:5" && working.Warnings.Count == 0 && !working.Live);
    File.AppendAllText(log, "10:00:04.000 stats: frames=90 shifted=0 (no gaze source); runtime left eye vs head x[-31.20..-31.20] mm\n");
    Require(PupilShiftStatus.Read(folder, true).State == "no-gaze" && PupilShiftStatus.Read(folder, false) is { State: "working", Live: false }, "live: the last second; after the flight: the whole flight");
    // The bundled Quad Views says there is no real gaze (its fixed focus): no pupil moves, the gaze source ends the line.
    File.AppendAllText(log, "10:00:04.500 stats: frames=90 shifted=0 (no valid gaze); runtime left eye vs head x[-31.20..-31.20] mm; cant 6.0 deg, runtime IPD 65.5 mm, rendered IPD 64.0 mm; gaze source=none (qv 0, focus 0, simulated 0, none 90; qv invalid 90, saccade 0)\n");
    Require(PupilShiftStatus.Read(folder, true) is { State: "no-gaze", Last: { Shifted: 0, RenderedIpdMm: 64.0 } }, "Quad Views without a valid gaze: no gaze, rendered IPD read before the gaze source");
    File.AppendAllText(log, "10:00:05.000 stats: frames=90 shifted=0 (correction off); runtime left eye vs head x[-31.20..-31.20] mm\n");
    Require(PupilShiftStatus.Read(folder, true).State == "off");
    // A runtime that already moves the eye with the gaze would get the correction twice.
    File.AppendAllText(log, "10:00:06.000 stats: frames=90 shifted=90 restored=360 gaze x[-1.0..1.0] y[0.0..0.0] deg, max shift 0.20 mm; runtime left eye vs head x[-33.00..-30.00] z[0.00..0.00] mm\n");
    Require(PupilShiftStatus.Read(folder, false).Warnings.Any(w => w.Contains("twice")));
});
Test("DCS engine optimizations install in Saved Games Scripts with every ini key explicit, and restore removes them and what they wrote", () =>
{
    var exe = Make("engine/game/bin/DCS.exe", "fixture"); var options = Make("engine/sg/DCS/Config/options.lua", Lua);
    var runtime = Make("engine/runtime.json", "{\"runtime\":{\"library_path\":\"runtime.dll\"}}"); Make("engine/runtime.dll", "fixture");
    var components = Path.GetDirectoryName(Make("engine/components/dcsqvcull/DcsQvCull.dll", "loader"))!;
    Make("engine/components/dcsqvcull/DcsQvCullPayload.dll", "payload"); Make("engine/components/dcsqvcull/DcsQvCull.lua", "-- hook");
    var otherHook = Make("engine/sg/DCS/Scripts/Hooks/OtherHook.lua", "-- user's own hook");
    var inventory = new InventorySnapshot { DcsExecutable = exe, OptionsPath = options, PimaxRuntime = runtime };
    var planner = new DeploymentPlanner(new(null, null, null, EngineDirectory: components));
    var scripts = Path.GetFullPath(Path.Combine(root, "engine/sg/DCS/Scripts")); var module = Path.Combine(scripts, "DcsQvCull");
    var off = new VrProfile { QuadViews = QuadProvider.None };
    Require(planner.Build(off, inventory, Path.Combine(root, "engine/managed-off")).Files.All(f => !f.Path.StartsWith(scripts, StringComparison.OrdinalIgnoreCase)));
    var on = off with { EngineOptimizations = true };
    var plan = planner.Build(on, inventory, Path.Combine(root, "engine/managed"));
    var engineFiles = plan.Files.Where(f => f.Path.StartsWith(scripts, StringComparison.OrdinalIgnoreCase)).Select(f => Path.GetRelativePath(scripts, f.Path)).ToArray();
    // The hook is written last, so restore removes it first; nothing goes to the DCS install.
    Require(engineFiles.SequenceEqual([@"DcsQvCull\DcsQvCull.dll", @"DcsQvCull\payload\DcsQvCullPayload.dll", @"DcsQvCull\DcsQvCull.ini", @"Hooks\DcsQvCull.lua"]));
    Require(plan.Files.All(f => !f.Path.StartsWith(Path.GetDirectoryName(exe)!, StringComparison.OrdinalIgnoreCase)));
    var ini = new StreamReader(new MemoryStream(plan.Files.Single(f => f.Path.EndsWith("DcsQvCull.ini")).Content), Encoding.UTF8, true).ReadToEnd();
    foreach (var line in new[] { "[General]\n", "PerceptionOnly=0\nDiagnostics=0", "[Timing]\nShaderTimeCache=1\nTaskQueueClock=1\nCacheUs=1000", "[Scene]\nPartitionBoost=1\nCostWeights=1\nCostWeightsSanity=0\nCollectThreadsMax=0\nFineTimerResolution=0", "[Cull]\nEnabled=0\nDebugHole=0", "[D3D]\nMeter=0\nFilter=0", "BenchTimer=0\nBenchPartition=0\nBenchIsolation=0\nBenchThreads=0\nBenchTimerRes=0\nBenchFilter=0", "SelfTest=0\nProfile=0\nBenchCull=0" })
        Require(ini.ReplaceLineEndings("\n").Contains(line), "DcsQvCull.ini lacks " + line);
    var tuned = ConfigurationWriters.DcsQvCull(on with { EngineShaderTimeCache = false, EnginePartitionBoost = false, EngineTimerRefreshUs = 500, EngineDiagnosticHooks = true });
    Require(tuned.Contains("ShaderTimeCache=0") && tuned.Contains("PartitionBoost=0") && tuned.Contains("CacheUs=500") && tuned.Contains("Diagnostics=1"));
    Require(ProfileValidation.Validate(on with { EngineTimerRefreshUs = 100 }).Any(i => i.Code == "engine-timer") && !ProfileValidation.Validate(on).Any(i => i.Code == "engine-timer"));
    // The in-flight switch: Alt+Shift+F11 by default, Off writes 0:0; the module's own developer keys stay off.
    Require(ini.Contains("Toggle=122:6") && ini.Contains("DeveloperKeys=0") && ConfigurationWriters.DcsQvCull(on with { EngineToggleKey = "Off" }).Contains("Toggle=0:0"));
    Require(!ProfileValidation.Validate(on).Any(i => i.Code is "engine-hotkey" or "hotkey-conflict"));
    Require(ProfileValidation.Validate(on with { EngineToggleKey = "120:3" }).Any(i => i.Code == "engine-hotkey" && i.Severity == IssueSeverity.Error));
    Require(ProfileValidation.Validate(on with { EngineToggleKey = "119:3" }).Any(i => i.Code == "engine-hotkey") && ini.Contains("TightCasters=0") && ini.Contains("BenchShadow=0"));
    foreach (var key in new[] { "AllocSlabs=1", "PlainTriangleCounter=1", "[Texture]", "StreamDedupe=1", "[Effects]", "SkipSameConstantBuffer=1", "CostWeights=1", "CostWeightsSanity=0", "Beeps=1", "BenchCostWeights=0", "Terrain=0", "SkipSameConstantUpload=0", "BenchCbUpload=0", "LowPowerPacer=0", "BenchPacer=0", "MotionSweep=0", "FrameHeapSlabs=1", "TaskQueueClock=1", "BenchFrameHeap=0", "MotionProfile=0", "MotionTaxi=0", "MotionCounters=1", "BigModelPages=1", "BigPageBytes=4194304", "ShadowInstancing=1", "ShadowBatching=1", "ShadowPlanAsync=1", "ShadowTextureSkip=1", "ShadowInstCompile=0", "ShadowInstVerify=0", "BenchShadowInst=0", "BenchBigPages=0", "BenchShadowTex=0", "BenchTexTable=0", "YawScan=0", "HoldYawDeg=-1", "GBufferBatching=0", "GBufferInstCompile=0", "GBufferInstVerify=0", "BenchGBufferInst=0", "ParallelUpload=0", "BenchParallelUpload=0", "BenchShadowPlanAsync=0", "DirectUploadCount=0", "FxApplyCount=0", "GBufferTexCount=0", "BenchAllocSlabs=0", "BenchTexDedupe=0", "BenchCbSkip=0", "BenchTriPlain=0", "BenchEngine=1", "BenchMicro=0", "Quick=1", "SigScan=1", "DirectUpload=0", "DirectUploadVerify=0", "BenchDirectUpload=0", "SplitFilter=1", "SplitFilterOps=0x4ff", "Meter=0", "SplitFilterVerify=0", "BenchSplitFilter=0", "JoinTailCount=0", "SrvSpanCount=0", "ShadowRecorder=1", "ShadowRecorderScope=0x30f", "ShadowRecorderWaitUs=200", "ShadowRecorderPriority=0", "ShadowRecorderSplit=0xf", "ShadowRecorderInstancing=1", "ShadowRecVerify=0", "ShadowRecVerifySec=5", "BenchShadowRecorder=0", "ShadowRecCount=0", "GBufferRecorder=1", "GBufferRecorderScope=0x10055", "GBufferRecorderMaxSegments=12", "GBufferRecorderHelpers=2", "GBufferRecorderWaitUs=300", "GBufferRecorderIsland=30", "GBufferRecorderRedo=1", "GBufferRecorderSwapAhead=1", "GBufferBatching=0", "GBufferRecVerify=0", "GBufferRecVerifySec=6", "GBufferRecVerifyStride=0", "BenchGBufferRecorder=0", "GBufferRecCount=0", "GBufferRecStateDump=0", "GpuPassTiming=0", "YawProfile=0", "RotationProfile=0", $"ShadowRecorderHelpers={(Environment.ProcessorCount <= 8 ? 1 : 3)}", "GBufferRecorderCockpit=0", "GBufferRecorderByOrdinal=1", "SrvTailTrim=0", "PassFlush=0", "FrameStartGap=0", "RunnableThreads=0", "VramCount=0", "GpuPassStats=0", "ForwardRecCount=0", "BenchPassFlush=0", "SrvTailTrimVerify=0", "BenchSrvTailTrim=0", "[Prefetch]", "Mode=skip", "WindowMs=5000", "BenchPrefetch=0", "[Governor]", "Enabled=1", "GpuBusyPct=92", "MinGainPct=2.0", "GpuFullPct=98", "TrialSec=20", "RecheckSec=10", "RevertBackoffSec=300", "[Perception]", "FixationDepth=0", "DepthGrid=0", "SceneCutReset=1", "FarDepthExport=0", "FarDepthExportSlots=2", "RecorderDetail=0", "PerfIntervalSec=10", "VramIntervalSec=60" })
        Require(ini.Contains(key), "DcsQvCull.ini lacks " + key);
    // The terrain prefetch fix is an engine optimization: the profile's mode, off when the option is off.
    Require(ConfigurationWriters.DcsQvCull(on with { BoostPrefetch = PrefetchFix.Observe }).Contains("\nMode=observe") && ConfigurationWriters.DcsQvCull(on with { BoostPrefetch = PrefetchFix.Off }).Contains("\nMode=off")
        && ConfigurationWriters.DcsQvCull(on with { EngineOptimizations = false }).Contains("\nMode=off") && on.UsesPrefetchFix && !(on with { EngineOptimizations = false, CpuBoost = true }).UsesPrefetchFix
        && (on with { Desktop = true }).ForLaunch().UsesPrefetchFix);
    var trimmed = ConfigurationWriters.DcsQvCull(on with { EngineModelAllocator = false, EngineTextureDedupe = false, EngineEffectBufferSkip = false, EnginePlainCounter = false, EngineCostWeights = false, EngineBeeps = false, EngineFrameHeap = false, EngineShadowInstancing = false, EngineStateFilter = false, EngineShadowRecorder = false, EngineGBufferRecorder = false });
    Require(trimmed.Contains("SlabBytes=4096") && trimmed.Contains("SplitFilter=0") && trimmed.Contains("ShadowRecorder=0") && trimmed.Contains("GBufferRecorder=0") && !trimmed.Contains("\nAllocSlabs=1") && trimmed.Contains("PlainTriangleCounter=0") && trimmed.Contains("StreamDedupe=0") && trimmed.Contains("SkipSameConstantBuffer=0") && trimmed.Contains("CostWeights=0") && trimmed.Contains("Beeps=0") && trimmed.Contains("FrameHeapSlabs=0") && trimmed.Contains("BigModelPages=0") && trimmed.Contains("ShadowInstancing=0") && trimmed.Contains("ShadowBatching=0") && trimmed.Contains("ShadowPlanAsync=0") && trimmed.Contains("ShadowTextureSkip=0")
        && ConfigurationWriters.DcsQvCull(on with { EngineShaderTimeCache = false }).Contains("TaskQueueClock=0"));
    // Developer mode: [Dev] paths only while it is on; full paths to existing files, otherwise refused or flagged.
    Require(ini.Contains("PayloadPath=\r\n") || ini.Contains("PayloadPath=\n"));
    var devPayload = Make("engine/dev/build/DcsQvCullPayload.dll", "dev"); var devIni = Make("engine/dev/DcsQvCull.dev.ini", "[Timing]");
    var dev = on with { EngineDevMode = true, EngineDevPayloadPath = devPayload, EngineDevIniPath = devIni };
    var devText = ConfigurationWriters.DcsQvCull(dev);
    Require(devText.Contains("PayloadPath=" + devPayload) && devText.Contains("IniPath=" + devIni) && !ConfigurationWriters.DcsQvCull(dev with { EngineDevMode = false }).Contains(devPayload));
    Require(!ProfileValidation.Validate(dev).Any(i => i.Code.StartsWith("engine-dev")));
    Require(ProfileValidation.Validate(dev with { EngineDevIniPath = null }).Any(i => i.Code == "engine-dev" && i.Severity == IssueSeverity.Error));
    Require(ProfileValidation.Validate(dev with { EngineDevPayloadPath = "relative.dll" }).Any(i => i.Code == "engine-dev"));
    Require(ProfileValidation.Validate(dev with { EngineDevPayloadPath = devPayload + "\nx.dll" }).Any(i => i.Code == "engine-dev"));
    Require(ProfileValidation.Validate(dev with { EngineDevPayloadPath = Path.Combine(root, "engine/none.dll") }).Any(i => i.Code == "engine-dev-missing" && i.Severity == IssueSeverity.Warning));
    Require(ProfileValidation.Validate(on with { NeuralRendering = true, EngineToggleKey = NeuralHotkeys.Default }).Any(i => i.Code == "hotkey-conflict"));
    Require(!ProfileValidation.Validate(on with { EngineOptimizations = false, NeuralRendering = true, EngineToggleKey = NeuralHotkeys.Default }).Any(i => i.Code == "hotkey-conflict"));
    // Without the bundled component the profile cannot be planned.
    Throws<InvalidDataException>(() => new DeploymentPlanner(new(null, null, null)).Build(on, inventory, Path.Combine(root, "engine/managed-missing")));

    var store = new OriginalsStore(Path.Combine(root, "originals/engine"));
    store.Apply(plan);
    Require(File.ReadAllText(Path.Combine(scripts, "Hooks", "DcsQvCull.lua")) == "-- hook" && File.ReadAllText(Path.Combine(module, "payload", "DcsQvCullPayload.dll")) == "payload");
    // Status before DCS ran: installed, not loaded.
    var dcsLog = Make("engine/sg/DCS/Logs/dcs.log", "");
    Require(EngineOptimizations.Read(options, dcsLog, dcsRunning: false) is { State: "installed" });
    // DCS reported a load failure after the install.
    File.WriteAllText(dcsLog, "2026-10-07 21:00:00.000 ERROR   DcsQvCull (Main): failed to load native module: error loading module\n");
    File.SetLastWriteTimeUtc(dcsLog, DateTime.UtcNow.AddMinutes(1));
    Require(EngineOptimizations.Read(options, dcsLog, false) is { State: "failed" } failed && failed.Summary.Contains("failed to load native module"));
    // What the module writes while DCS runs.
    var log = Path.Combine(module, "DcsQvCull.log");
    File.WriteAllText(log, string.Join("\n", "21:00:00.000 loader: starting payload active_1.dll (generation 1)", "21:00:00.100 timer cache: dx11backend!ED_get_time redirected (refresh every 1000 us)",
        "21:00:01.000 scene: DCSScene at 0000", "21:00:02.000 partition boost: unexpected scene layout (value 7); disabled", "21:00:03.000 stats: OFF timer=cache refresh/s=1000",
        @"21:00:03.500 loader: payload source C:\dev\build\DcsQvCullPayload.dll", @"21:00:03.600 config: dev mode, settings from C:\dev\DcsQvCull.dev.ini",
        "21:00:04.000 ==== DcsQvCull test suite 2026-10-07 21:00 ====", "21:00:05.000 stats: OFF timer=cache refresh/s=1000"));
    File.SetLastWriteTimeUtc(log, DateTime.UtcNow.AddMinutes(1));
    var status = EngineOptimizations.Read(options, dcsLog, dcsRunning: true);
    Require(status is { State: "loaded", TimerCacheAvailable: true, TimerCacheActive: true, SceneHooked: true, SuiteRunning: true } && status.Warnings.Count == 1 && status.Warnings[0].Contains("partition boost")
        && status.Summary.Contains(@"developer payload C:\dev\build\DcsQvCullPayload.dll") && status.Summary.Contains(@"developer settings C:\dev\DcsQvCull.dev.ini"));
    File.AppendAllText(log, "\n21:07:00.000 suite: report written to report_20261007_210000.txt\n21:07:01.000 stats: OFF timer=real refresh/s=0");
    Make("engine/sg/DCS/Scripts/DcsQvCull/report_20261007_210000.txt", "==== suite finished ====");
    status = EngineOptimizations.Read(options, dcsLog, dcsRunning: true);
    Require(status is { SuiteRunning: false, TimerCacheActive: false } && status.LastReport == Path.Combine(module, "report_20261007_210000.txt"));
    // The suite needs a running DCS and the installed module; the flag is what the module watches for.
    Throws<InvalidOperationException>(() => EngineOptimizations.StartSuite(options, dcsRunning: false));
    EngineOptimizations.StartSuite(options, dcsRunning: true);
    Require(File.Exists(Path.Combine(module, "run_suite.flag")));
    Make("engine/sg/DCS/Scripts/DcsQvCull/bench_20261007_210000.csv", "block"); Make("engine/sg/DCS/Scripts/DcsQvCull/payload/active_1.dll", "copy");
    Make("engine/sg/DCS/Scripts/DcsQvCull/payload/active_2.dll", "copy");
    // The previous session's logs the module keeps at each start, and its rotated prefetch log.
    Make("engine/sg/DCS/Scripts/DcsQvCull/DcsQvCull.prev.log", "earlier"); Make("engine/sg/DCS/Scripts/DcsQvCull/DcsQvCullPrefetch.prev.log", "earlier");
    // The shadow shader cache, including a temp file left by a write that was cut short.
    Make("engine/sg/DCS/Scripts/DcsQvCull/cache/s_0123456789abcdef.qvc", "entry"); Make("engine/sg/DCS/Scripts/DcsQvCull/cache/g_0123456789abcdef.qvc.12345.tmp", "partial");
    File.AppendAllText(log, "\n21:07:30.000 cockpit ipd: ownship matching off (SceneRenderable vtable not in this NGModel build)");
    Require(EngineOptimizations.Read(options, dcsLog, dcsRunning: true).Warnings.Any(w => w.Contains("recognise your own aircraft", StringComparison.Ordinal)), "Ownship matching off is reported");
    // A later payload that failed to start (a reload): the last loader outcome counts.
    File.AppendAllText(log, "\n21:08:00.000 loader: payload start reported an error (Start returned 0)\n");
    Require(EngineOptimizations.Read(options, dcsLog, dcsRunning: true) is { State: "failed" } startFailed && startFailed.Summary.Contains("payload start reported an error"));
    // A second DCS writes DcsQvCull.<pid>.log while the first holds DcsQvCull.log: the newer one is read.
    var pidLog = Path.Combine(module, "DcsQvCull.4242.log");
    File.WriteAllText(pidLog, "21:09:00.000 loader: starting payload active_3.dll (generation 1)\n"); File.SetLastWriteTimeUtc(pidLog, DateTime.UtcNow.AddMinutes(2));
    Require(EngineOptimizations.ModuleLog(module) == pidLog && EngineOptimizations.Read(options, dcsLog, dcsRunning: true) is { State: "loaded" });

    Require(store.RestoreOriginals().Complete);
    Require(!Directory.Exists(module) && !File.Exists(Path.Combine(scripts, "Hooks", "DcsQvCull.lua")) && File.ReadAllText(otherHook) == "-- user's own hook");
    Require(EngineOptimizations.Read(options, dcsLog, false) is { State: "not-installed" });
});
Test("Runtime files removed on restore are only declared logs, outputs and module copies inside the component's folder", () =>
{
    var owner = Path.GetFullPath(Path.Combine(root, "runtime-files/module"));
    Require(RuntimeFiles.Allowed(Path.Combine(owner, "x.log"), owner) && RuntimeFiles.Allowed(Path.Combine(owner, "report_*.txt"), owner) && RuntimeFiles.Allowed(Path.Combine(owner, "payload", "active_*.dll"), owner));
    Require(!RuntimeFiles.Allowed(Path.Combine(owner, "library.dll"), owner) && !RuntimeFiles.Allowed(Path.Combine(owner, "*.exe"), owner) && !RuntimeFiles.Allowed(Path.Combine(owner, "..", "other", "x.log"), owner)
        && !RuntimeFiles.Allowed(Path.Combine(owner, "*", "x.log"), owner) && !RuntimeFiles.Allowed(Path.Combine(owner, "*.*"), owner) && !RuntimeFiles.Allowed(Path.Combine(owner, "a?.log"), owner) && !RuntimeFiles.Allowed(Path.Combine(owner, "report.txt"), owner));
    Make("runtime-files/module/payload/active_1.dll", "a"); Make("runtime-files/module/payload/active_1.dllx", "keep"); Make("runtime-files/module/payload/other.dll", "keep");
    Require(RuntimeFiles.Matching(Path.Combine(owner, "payload", "active_*.dll")).Select(Path.GetFileName).SequenceEqual(["active_1.dll"]));
});
string ReleaseFixture(string name, string version, string content)
{
    var source = Path.Combine(root, "app-sources/" + name);
    Directory.CreateDirectory(source); AtomicFile.WriteText(Path.Combine(source, "app.bin"), content); AtomicFile.WriteText(Path.Combine(source, "readme.txt"), "fixture");
    var files = new[] { "app.bin", "readme.txt" }.Select(f => new ReleaseFile(f, Hashing.FileSha256(Path.Combine(source, f)), new FileInfo(Path.Combine(source, f)).Length)).ToList();
    AtomicFile.WriteText(Path.Combine(source, "release-manifest.json"), JsonData.Serialize(new ReleaseManifest(1, "DcsControl", version, files))); return source;
}
Test("Application install, upgrade, uninstall preserve unowned files", () =>
{
    var source1 = ReleaseFixture("upgrade-v1", "1", "first"); var source2 = ReleaseFixture("upgrade-v2", "2", "second"); var target = Path.Combine(root, "app-install/upgrade");
    var installer = new ApplicationInstaller(target); installer.Install(source1); Require(File.ReadAllText(Path.Combine(target, "app.bin")) == "first");
    installer.Install(source2); Require(File.ReadAllText(Path.Combine(target, "app.bin")) == "second"); AtomicFile.WriteText(Path.Combine(target, "user-notes.txt"), "user");
    Require(installer.Uninstall().Complete); Require(!File.Exists(Path.Combine(target, "app.bin"))); Require(!File.Exists(Path.Combine(target, "installation.json"))); Require(File.ReadAllText(Path.Combine(target, "user-notes.txt")) == "user");
});
Test("Upgrade removes obsolete owned files, reinstalls them and uninstalls cleanly", () =>
{
    var v1 = ReleaseFixture("obsolete-v1", "1", "first"); var v2 = ReleaseFixture("obsolete-v2", "2", "second");
    var manifestPath = Path.Combine(v2, "release-manifest.json"); var manifest = JsonData.Deserialize<ReleaseManifest>(File.ReadAllText(manifestPath));
    AtomicFile.WriteText(manifestPath, JsonData.Serialize(manifest with { Files = manifest.Files.Where(f => f.Path != "readme.txt").ToList() }));
    var target = Path.Combine(root, "app-install/obsolete"); var installer = new ApplicationInstaller(target); installer.Install(v1);
    var owned = Path.Combine(target, "readme.txt"); var notes = Path.Combine(target, "user-notes.txt"); AtomicFile.WriteText(notes, "user");
    installer.Install(v2); Require(!File.Exists(owned)); installer.Install(v2); Require(!File.Exists(owned));
    installer.Install(v1); Require(File.Exists(owned)); Require(installer.Uninstall().Complete && !File.Exists(owned) && File.ReadAllText(notes) == "user");
});
Test("Failed upgrade recovers an obsolete file removed earlier in the transaction", () =>
{
    var v1 = ReleaseFixture("obsolete-failure-v1", "1", "first"); var v2 = ReleaseFixture("obsolete-failure-v2", "2", "second");
    var manifestPath = Path.Combine(v2, "release-manifest.json"); var manifest = JsonData.Deserialize<ReleaseManifest>(File.ReadAllText(manifestPath));
    AtomicFile.WriteText(manifestPath, JsonData.Serialize(manifest with { Files = manifest.Files.Where(f => f.Path != "readme.txt").ToList() }));
    var target = Path.Combine(root, "app-install/obsolete-failure"); var installer = new ApplicationInstaller(target); installer.Install(v1);
    var path = Path.Combine(target, "readme.txt"); var bytes = File.ReadAllBytes(path);
    Throws<IOException>(() => installer.Install(v2, i => { if (i == 2) throw new IOException("Failure after obsolete deletion"); }));
    Require(File.ReadAllBytes(path).SequenceEqual(bytes) && File.ReadAllText(Path.Combine(target, "app.bin")) == "first" && installer.Uninstall().Complete);
});
Test("Modified obsolete files are retained before any upgrade writes", () =>
{
    var v1 = ReleaseFixture("obsolete-edited-v1", "1", "first"); var v2 = ReleaseFixture("obsolete-edited-v2", "2", "second");
    var manifestPath = Path.Combine(v2, "release-manifest.json"); var manifest = JsonData.Deserialize<ReleaseManifest>(File.ReadAllText(manifestPath));
    AtomicFile.WriteText(manifestPath, JsonData.Serialize(manifest with { Files = manifest.Files.Where(f => f.Path != "readme.txt").ToList() }));
    var target = Path.Combine(root, "app-install/obsolete-edited"); var installer = new ApplicationInstaller(target); installer.Install(v1);
    AtomicFile.WriteText(Path.Combine(target, "readme.txt"), "my changes"); Throws<IOException>(() => installer.Install(v2));
    Require(File.ReadAllText(Path.Combine(target, "readme.txt")) == "my changes" && File.ReadAllText(Path.Combine(target, "app.bin")) == "first");
});
Test("Failed application upgrade rolls back to the old version", () =>
{
    var source1 = ReleaseFixture("failure-v1", "1", "first"); var source2 = ReleaseFixture("failure-v2", "2", "second"); var target = Path.Combine(root, "app-install/failure");
    var installer = new ApplicationInstaller(target); installer.Install(source1);
    Throws<IOException>(() => installer.Install(source2, i => { if (i == 1) throw new IOException("Injected failure"); }));
    Require(File.ReadAllText(Path.Combine(target, "app.bin")) == "first"); Require(installer.Uninstall().Complete);
});
Test("Application uninstall refuses edits before removing any files", () =>
{
    var source = ReleaseFixture("edits", "1", "first"); var target = Path.Combine(root, "app-install/edits"); var installer = new ApplicationInstaller(target); installer.Install(source);
    AtomicFile.WriteText(Path.Combine(target, "app.bin"), "user-edit"); Throws<IOException>(() => installer.Uninstall()); Require(File.Exists(Path.Combine(target, "readme.txt"))); Require(File.ReadAllText(Path.Combine(target, "app.bin")) == "user-edit");
});
Test("Application install refuses modified source payload", () =>
{
    var source = ReleaseFixture("bad-source", "1", "first"); AtomicFile.WriteText(Path.Combine(source, "app.bin"), "tampered");
    Throws<InvalidDataException>(() => new ApplicationInstaller(Path.Combine(root, "app-install/bad-source")).Install(source));
});
Test("Application installer rejects out-of-scope journal paths", () =>
{
    var source = ReleaseFixture("scope", "1", "first"); var target = Path.Combine(root, "app-install/scope"); var installer = new ApplicationInstaller(target); var journal = installer.Install(source);
    var outside = Make("outside-ledger/file", "protected"); journal.Entries[0] = journal.Entries[0] with { Path = outside };
    AtomicFile.WriteText(Path.Combine(installer.Transactions.StateDirectory, journal.Id, "journal.json"), JsonData.Serialize(journal));
    Throws<InvalidDataException>(() => installer.Uninstall()); Require(File.ReadAllText(outside) == "protected");
});
Test("Application installer rejects reserved destination roots", () => Throws<InvalidDataException>(() => new ApplicationInstaller(Path.GetPathRoot(root)!)));
Test("Application installation refuses unknown existing files", () =>
{
    var source = ReleaseFixture("existing", "1", "first"); var target = Path.Combine(root, "app-install/existing"); Directory.CreateDirectory(target); AtomicFile.WriteText(Path.Combine(target, "app.bin"), "mine");
    Throws<IOException>(() => new ApplicationInstaller(target).Install(source)); Require(File.ReadAllText(Path.Combine(target, "app.bin")) == "mine");
});
Test("Back to stock DCS merges owned settings back and keeps later graphics edits", () =>
{
    var path = Make("lua-merge/options.lua", Lua); var patched = LuaOptions.Patch(Lua, new Dictionary<string, object> { ["VR.enable"] = true, ["VR.openxr_eyeGaze"] = true });
    var before = new LuaOptions(Lua); var after = new LuaOptions(patched);
    var changes = new[] { "VR.enable", "VR.openxr_eyeGaze" }.Select(k => new LuaValueChange(k, before.Get(k.Split('.')), after.Get(k.Split('.'))!)).ToArray();
    var store = new OriginalsStore(Path.Combine(root, "originals/lua-merge")); store.Apply(new("lua", "Lua test", [new(path, Hashing.FileSha256(path), Encoding.UTF8.GetBytes(patched), "options", changes)], new Dictionary<string, string>(), "fixture"));
    AtomicFile.WriteText(path, File.ReadAllText(path).Replace("maxFPS=89", "maxFPS=120")); Require(store.RestoreOriginals().Complete);
    var result = new LuaOptions(File.ReadAllText(path)); Require(result.Get("VR", "enable") == "false"); Require(result.Get("VR", "openxr_eyeGaze") is null); Require(result.Get("graphics", "maxFPS") == "120"); Require(File.ReadAllText(path).Contains("preserve comment"));
});
Test("Back to stock DCS sets an edited owned setting back, never refusing", () =>
{
    var path = Make("lua-conflict/options.lua", Lua); var change = new LuaValueChange("graphics.Upscaling", "\"DLSS\"", "\"DLSS\""); var store = new OriginalsStore(Path.Combine(root, "originals/lua-conflict"));
    store.Apply(new("lua", "Lua test", [new(path, Hashing.FileSha256(path), Encoding.UTF8.GetBytes(Lua), "options", [change])], new Dictionary<string, string>(), "fixture"));
    AtomicFile.WriteText(path, Lua.Replace("\"DLSS\"", "\"FSR\"").Replace("maxFPS=89", "maxFPS=120")); Require(store.RestoreOriginals().Complete);
    Require(new LuaOptions(File.ReadAllText(path)).Get("graphics", "Upscaling") == "\"DLSS\"" && new LuaOptions(File.ReadAllText(path)).Get("graphics", "maxFPS") == "120");
});
Test("Back to stock DCS recognizes equivalent Lua string quoting", () =>
{
    var path = Make("lua-quotes/options.lua", Lua); var change = new LuaValueChange("graphics.Upscaling", "\"DLSS\"", "\"DLSS\""); var store = new OriginalsStore(Path.Combine(root, "originals/lua-quotes"));
    store.Apply(new("lua", "Lua test", [new(path, Hashing.FileSha256(path), Encoding.UTF8.GetBytes(Lua), "options", [change])], new Dictionary<string, string>(), "fixture"));
    var quoted = Lua.Replace("\"DLSS\"", "'DLSS'").Replace("maxFPS=89", "maxFPS=120"); AtomicFile.WriteText(path, quoted); Require(store.RestoreOriginals().Complete); Require(File.ReadAllText(path) == quoted, "An equivalent value is left exactly as written");
});
Test("Back to stock DCS puts the whole original back when options.lua can no longer be read, without executing it", () =>
{
    var path = Make("lua-execution/options.lua", Lua); var change = new LuaValueChange("VR.enable", "false", "true"); var store = new OriginalsStore(Path.Combine(root, "originals/lua-execution"));
    store.Apply(new("lua", "Lua test", [new(path, Hashing.FileSha256(path), Encoding.UTF8.GetBytes(Lua.Replace("enable=false", "enable=true")), "options", [change])], new Dictionary<string, string>(), "fixture"));
    AtomicFile.WriteText(path, "options={}\nos.execute('unsupported')"); Require(store.RestoreOriginals().Complete); Require(File.ReadAllText(path) == Lua);
    Require(File.ReadAllText(store.LogPath).Contains("could not be read"));
});
Test("Combined profiles opt into the bounded focus adapter", () =>
{
    foreach (var p in ProfilePresets.All.Where(p => p.QuadFocusAdapter))
    {
        Require(p.UsesQuadFocus && p.NeuralRendering && p.FrameGen == FrameGeneration.Nvidia);
        var issues = ProfileValidation.Validate(p with { NeuralRuntimePath = "nvngx_dlssnr.dll" });
        Require(!issues.Any(i => i.Severity == IssueSeverity.Error));
    }
});
Test("Focus adapter rejects unverified alternative compositors", () => Require(ProfileValidation.Validate(new() { QuadViews = QuadProvider.QuadViewsFoveated, QuadFocusAdapter = true, NeuralRendering = true, QuadViewsLayerDirectory = "alternative" }).Any(i => i.Code == "quad-adapter-provider")));
Test("Focus configuration avoids nested foveation and duplicate peripheral processing", () =>
{
    var ini = ConfigurationWriters.Cheeky(ProfilePresets.All[5] with { QuadNeuralEdgeFade = false });
    foreach (var field in new[] { "PeripheralDlaa=0", "Width=1", "Height=1", "CenterMode=0", "AutoStereoAlignment=0", "NrFoveated=0", "NrUseSrFoveation=0", "D3D11D3D12Transport=1" }) Require(ini.Split('\n').Any(l => l.Trim() == field), field);
});
Test("DLSS 5 area writes a central feathered NR region only with the focus adapter", () =>
{
    bool Has(VrProfile p, params string[] fields) { var lines = ConfigurationWriters.Cheeky(p).Split('\n').Select(l => l.Trim()).ToHashSet(); return fields.All(lines.Contains); }
    var focus = ProfilePresets.All[5] with { NeuralBeforeUpscaling = false, NeuralRuntimePath = "nvngx_dlssnr.dll" };
    Require(focus.NeuralFocusArea == 100 && !focus.UsesCentralNeuralArea, "Whole focus area is the default");
    Require(Has(focus with { QuadNeuralEdgeFade = false }, "NrFoveated=0", "NrUseSrFoveation=0", "NrWidth=1", "NrHeight=1", "Enabled=0", "PeripheralDlaa=0"), "Whole focus area keeps whole-view NR");
    // By default the whole-area DLSS 5 fades out with the Quad Views edge band (round or rectangular), capped at 0.45.
    Require(focus.UsesNeuralEdgeFade && Has(focus with { QuadEdgeBlend = 0.1, QuadRoundFocus = true }, "NrFoveated=1", "NrUseSrFoveation=0", "NrWidth=1", "NrHeight=1", "NrRoundness=1", "NrTransitionWidth=0.2", "Enabled=0"), "Edge fade, round");
    Require(Has(focus with { QuadEdgeBlend = 0.3, QuadRoundFocus = false }, "NrFoveated=1", "NrRoundness=0", "NrTransitionWidth=0.45"), "Edge fade, rectangle, capped");
    Require(Has(focus with { QuadEdgeBlend = 0 }, "NrFoveated=0") && !(focus with { QuadEdgeBlend = 0 }).UsesNeuralEdgeFade, "A sharp Quad Views edge keeps whole-view NR");
    foreach (var area in new[] { 90, 80, 70, 50 })
        Require(Has(focus with { NeuralFocusArea = area }, "NrFoveated=1", "NrUseSrFoveation=0", $"NrWidth=0.{area / 10}", $"NrHeight=0.{area / 10}",
            focus.QuadRoundFocus ? "NrRoundness=1" : "NrRoundness=0", "NrTransitionWidth=0.12", "Enabled=0", "PeripheralDlaa=0", "CenterMode=0", "AutoStereoAlignment=0", "NrProcessingOrder=0"), "Central " + area + "%");
    Require(!(focus with { NeuralRendering = false, NeuralFocusArea = 70 }).UsesCentralNeuralArea, "The area applies only to DLSS 5");
    Require(Has(focus with { FoveatedDlss = true, NeuralFocusArea = 70 }, "Enabled=0", "NrFoveated=1", "NrWidth=0.7"), "Foveated Super Resolution never runs on the focus views");
    var stereo = ProfilePresets.All[3] with { NeuralFocusArea = 50 };
    Require(!stereo.UsesCentralNeuralArea && Has(stereo, "NrFoveated=1", "NrUseSrFoveation=1", "NrWidth=0.57", "NrHeight=0.34"), "Stereo NR coverage is unchanged");
    Require(!ProfileValidation.Validate(focus with { NeuralFocusArea = 70 }).Any(i => i.Code == "neural-area"));
    foreach (var invalidArea in new[] { 0, 60, 101 }) Require(ProfileValidation.Validate(focus with { NeuralFocusArea = invalidArea }).Any(i => i.Code == "neural-area" && i.Severity == IssueSeverity.Error), "Rejects " + invalidArea);
    var saved = focus with { NeuralFocusArea = 70 };
    Require(JsonData.Deserialize<VrProfile>(JsonData.Serialize(saved)) == saved && JsonData.Serialize(saved).Contains("\"neuralFocusArea\": 70"));
    var legacy = System.Text.Json.Nodes.JsonNode.Parse(JsonData.Serialize(focus))!.AsObject(); legacy.Remove("neuralFocusArea");
    Require(JsonData.Deserialize<VrProfile>(legacy.ToJsonString()).NeuralFocusArea == 100, "Profiles saved before the option load as whole focus area");
});
Test("Focus adapter fails closed when adapted binaries are missing", () =>
{
    var exe = Make("missing-adapter/bin/DCS.exe", "fixture"); var options = Make("missing-adapter/options.lua", Lua);
    var runtime = Make("missing-adapter/runtime.json", "{\"runtime\":{\"library_path\":\"runtime.dll\"}}"); Make("missing-adapter/runtime.dll", "fixture");
    var p = new VrProfile { QuadViews = QuadProvider.QuadViewsFoveated, QuadFocusAdapter = true, NeuralRendering = true };
    Throws<InvalidDataException>(() => new DeploymentPlanner(new(null, "unused", "unused")).Build(p, new() { DcsExecutable = exe, OptionsPath = options, PimaxRuntime = runtime }, Path.Combine(root, "missing-adapter/managed")));
});
foreach (var runtimeKind in new[] { RuntimeKind.Pimax, RuntimeKind.SboysSteamVr })
    Test("Combined focus deployment and restoration " + runtimeKind, () =>
    {
        // Cheeky runs with Quad Views only for DLSS 5, whose signed runtime file is never in the repository.
        if (RealNeuralRuntime() is not { } neuralRuntime) { Console.WriteLine("SKIP combined focus deployment: no signed nvngx_dlssnr.dll (set DCSVR_TEST_NEURAL_PATH)"); return; }
        var dir = "combined-" + runtimeKind; var exe = Make(dir + "/bin/DCS.exe", "fixture"); var options = Make(dir + "/options.lua", Lua);
        var runtime = Make(dir + "/runtime.json", "{\"runtime\":{\"library_path\":\"runtime.dll\"}}"); Make(dir + "/runtime.dll", "fixture");
        var ofxr = new ComponentCache(Path.Combine(root, dir + "/cache")).Import(PackageCatalog.Get("ofxr"), Path.Combine(workspace, ".cache/packages", PackageCatalog.Get("ofxr").ArchiveName));
        var upstream = Path.Combine(workspace, ".cache/packages/cheeky");
        var focus = Path.Combine(root, dir + "/focus"); Directory.CreateDirectory(Path.Combine(focus, "CheekyFoveatedDLSS"));
        File.Copy(Path.Combine(upstream, "dxgi.dll"), Path.Combine(focus, "dxgi.dll"));
        foreach (var name in new[] { "CheekyFoveatedDLSSHost.dll", "CheekyFoveatedDLSSRuntime.dll" }) File.Copy(Path.Combine(workspace, "artifacts/native/cheeky/bin/Release/CheekyFoveatedDLSS", name), Path.Combine(focus, "CheekyFoveatedDLSS", name));
        var p = new VrProfile { Runtime = runtimeKind, QuadViews = QuadProvider.QuadViewsFoveated, QuadFocusAdapter = true, NeuralRendering = true, NeuralRuntimePath = neuralRuntime, FrameGen = FrameGeneration.Nvidia, Gaze = runtimeKind == RuntimeKind.Pimax ? GazeMode.EyeTracked : GazeMode.Fixed };
        var inv = new InventorySnapshot { DcsExecutable = exe, OptionsPath = options, PimaxRuntime = runtime, SteamVrRuntime = runtime };
        var plan = new DeploymentPlanner(new(ofxr, null, Path.Combine(workspace, "artifacts/native/cheeky/bin/Release/CheekyOpenXRLayer.dll"), Path.Combine(workspace, "external/quadviews/bin/x64/Release"), focus, DeferredOfxr)).Build(p, inv, Path.Combine(root, dir + "/managed"));
        Require(plan.LaunchEnvironment["DCSVR_QUAD_FOCUS"] == "1");
        Require(plan.LaunchEnvironment["DCSVR_DIAG_HOTKEY"] == "123:6" && plan.LaunchEnvironment["DCSVR_DIAG_OVERLAY"] == "0" && plan.LaunchEnvironment["DCSVR_NR_HOTKEY"] == NeuralHotkeys.Environment(p.NeuralToggleKey));
        Require(plan.LaunchEnvironment["XR_ENABLE_API_LAYERS"] == "XR_APILAYER_CHEEKY_foveated_dlss;XR_APILAYER_MBUCCHIA_quad_views_foveated;XR_APILAYER_XRFrameBridge_diagnostic");
        var runtimeFile = plan.Files.Single(f => f.Path.EndsWith("CheekyFoveatedDLSSRuntime.dll")); Require(Hashing.BytesSha256(runtimeFile.Content) == Hashing.FileSha256(Path.Combine(focus, "CheekyFoveatedDLSS/CheekyFoveatedDLSSRuntime.dll")));
        var store = new OriginalsStore(Path.Combine(root, dir + "/originals")); store.Apply(plan); Require(store.RestoreOriginals().Complete); Require(File.ReadAllText(options) == Lua);
    });
Test("Service validates profile errors before importing packages", () =>
{
    var state = Path.Combine(root, "early-validation/state"); var service = new ControlService(Path.Combine(root, "missing-distribution"), state);
    try { service.Preview(ProfilePresets.All[5], new()); throw new Exception("Invalid profile accepted"); }
    catch (InvalidDataException e) { Require(e.Message.Contains("nvngx_dlssnr.dll") && !e.Message.Contains("experimental mode")); }
    Require(!Directory.Exists(Path.Combine(state, "cache")));
});
(ControlService Service, string Runtime, string Library, string Options, string Exe) LaunchFixture(string name)
{
    var prefix = "launch-check/" + name;
    var exe = Make(prefix + "/bin/DCS.exe", "Never execute this fixture"); var options = Make(prefix + "/options.lua", Lua);
    var library = Make(prefix + "/runtime.dll", "Never load this fixture"); var runtime = Make(prefix + "/runtime.json", "{\"runtime\":{\"library_path\":\"runtime.dll\"}}");
    var service = new ControlService(root, Path.Combine(root, prefix, "state"));
    service.Apply(service.Preview(new(), new() { DcsExecutable = exe, OptionsPath = options, PimaxRuntime = runtime }));
    return (service, runtime, library, options, exe);
}
Test("Applied launch contract validates without executing DCS", () =>
{
    var f = LaunchFixture("valid"); var start = f.Service.PrepareLaunchApplied(); Require(start.FileName == f.Exe);
    Require(start.WorkingDirectory == Path.GetDirectoryName(f.Exe)); Require(start.Environment["XR_RUNTIME_JSON"] == f.Runtime);
    Require(!start.UseShellExecute && start.Environment["DCSVR_QUAD_FOCUS"] == "0");
});
Test("Launch accepts unrelated Lua edits and rejects owned-setting edits", () =>
{
    var f = LaunchFixture("lua-edits"); AtomicFile.WriteText(f.Options, File.ReadAllText(f.Options).Replace("maxFPS=89", "maxFPS=120"));
    Require(f.Service.PrepareLaunchApplied().FileName == f.Exe);
    AtomicFile.WriteText(f.Options, File.ReadAllText(f.Options).Replace("enable=true", "enable=false")); Throws<IOException>(() => f.Service.PrepareLaunchApplied());
});
foreach (var manifest in new[] { true, false })
    Test("Launch rejects changed external " + (manifest ? "runtime manifest" : "runtime library"), () =>
    {
        var f = LaunchFixture(manifest ? "manifest-changed" : "library-changed"); AtomicFile.WriteText(manifest ? f.Runtime : f.Library, "changed");
        Throws<IOException>(() => f.Service.PrepareLaunchApplied());
    });
Test("Launch refuses missing executable", () => { var f = LaunchFixture("missing-exe"); File.Delete(f.Exe); Throws<IOException>(() => f.Service.PrepareLaunchApplied()); });
Test("An interrupted apply leaves no applied profile to launch; Launch DCS applies again and launches", () =>
{
    var f = LaunchFixture("interrupted"); var inventory = new InventorySnapshot { DcsExecutable = f.Exe, OptionsPath = f.Options, PimaxRuntime = f.Runtime };
    try { f.Service.Originals.Apply(f.Service.Preview(new() { FpsLimit = FpsLimitMode.Custom, RenderedFpsCap = 61 }, inventory), i => { if (i == 1) throw new IOException("Injected failure"); }); } catch (IOException) { }
    Throws<InvalidOperationException>(() => f.Service.PrepareLaunchApplied());
    var (sync, start) = f.Service.SyncAndPrepareLaunch(new(), inventory);
    Require(sync.Kind == LaunchSyncKind.Applied && start.FileName == f.Exe && f.Service.RestoreOriginals().Complete && File.ReadAllText(f.Options) == Lua);
});
Test("Legacy launch profiles remain readable", () => Require(JsonData.Deserialize<LaunchConfiguration>("{\"executable\":\"DCS.exe\",\"environment\":{}}").Dependencies is null));
Test("Software coverage cap is explicit and does not change profile values", () =>
{
    var p = new VrProfile { QuadViews = QuadProvider.QuadViewsFoveated, FoveaWidth = 1, FoveaHeight = 1 };
    Require(ProfileValidation.Validate(p).Any(i => i.Code == "quad-coverage-cap")); Require(p.FoveaWidth == 1 && p.FoveaHeight == 1);
});
Test("Profiles without quality fields get the default quality values", () =>
{
    var p = JsonData.Deserialize<VrProfile>("{\"id\":\"legacy\"}");
    Require(p.QuadFocusScale == 1.125 && p.PeripheralScale == .19 && p.FoveaWidth == .57 && p.FoveaHeight == .34 && p.QuadSharpening == .7 && p.QuadEdgeBlend == .2 && p.FlowPreset == NvidiaFlowPreset.Medium && !p.BidirectionalFlow);
});
Test("Custom quality controls target supported provider fields", () =>
{
    var p = new VrProfile { QuadFocusScale = 1.25, QuadSharpening = .45, QuadEdgeBlend = .15, FlowPreset = NvidiaFlowPreset.Slow, BidirectionalFlow = true, NvidiaFlowScale = 100 };
    var quad = ConfigurationWriters.QuadViews(p); var flow = ConfigurationWriters.Ofxr(p);
    Require(quad.Contains("focus_multiplier=1.25") && quad.Contains("sharpen_focus_view=0.45") && quad.Contains("smoothen_focus_view_edges=0.15") && quad.Contains("focus_view_shape=2"));
    Require(flow.Contains("nvidia_preset=slow") && flow.Contains("nvidia_bidirectional=1") && flow.Contains("nvidia_input_scale=100"));
    Require(JsonData.Deserialize<VrProfile>(JsonData.Serialize(p)) == p);
});
Test("Invalid quality controls cannot be deployed", () =>
{
    var invalid = new[] { new VrProfile { QuadFocusScale = double.NaN }, new VrProfile { QuadFocusScale = 2.01 }, new VrProfile { QuadSharpening = -1 },
        new VrProfile { QuadEdgeBlend = .51 }, new VrProfile { FlowPreset = (NvidiaFlowPreset)99 } };
    foreach (var p in invalid) Require(ProfileValidation.Validate(p).Any(i => i.Severity == IssueSeverity.Error));
});
Test("Steam libraries detect escaped paths and ignore app IDs", () =>
{
    var steam = Path.GetFullPath(Path.Combine(root, "steam-discovery")); var library = Path.GetFullPath(Path.Combine(root, "steam-library"));
    Make("steam-discovery/steamapps/libraryfolders.vdf", "\"libraryfolders\" { \"0\" { \"path\" \"" + library.Replace("\\", "\\\\") + "\" \"apps\" { \"250820\" \"1234\" } } }");
    Require(WindowsInventory.SteamLibraries(steam).SequenceEqual(new[] { steam, library }));
});
Test("Driver checks distinguish registration, missing DLL and SteamVR blocking", () =>
{
    var directory = Path.Combine(root, "registered-driver"); Make("registered-driver/driver.vrdrivermanifest", "{\"name\":\"CustomHeadsetOpenVR\"}");
    var config = Path.Combine(root, "driver-config");
    var registration = Make("driver-registration.json", JsonData.Serialize(new { external_drivers = new[] { directory }, config = new[] { config } }));
    var facts = WindowsInventory.ReadDrivers(registration, null, []); Require(facts.Count == 1 && !facts[0].LibraryExists && !facts[0].Blocked);
    var dll = Path.Combine(directory, "bin/win64/driver_CustomHeadsetOpenVR.dll"); Directory.CreateDirectory(Path.GetDirectoryName(dll)!);
    File.Copy(Path.Combine(workspace, "artifacts/native/cheeky/bin/Release/CheekyOpenXRLayer.dll"), dll);
    Make("driver-config/steamvr.vrsettings", "{\"driver_CustomHeadsetOpenVR\":{\"blocked_by_safe_mode\":true}}");
    facts = WindowsInventory.ReadDrivers(registration, null, []); Require(facts[0].LibraryExists && facts[0].Blocked);
});
Test("Readiness rejects absent Sboys registration and gaze instead of claiming success", () =>
{
    var p = new VrProfile { Runtime = RuntimeKind.SboysSteamVr, QuadViews = QuadProvider.QuadViewsFoveated, Tracking = TrackingKind.Lighthouse };
    var report = Readiness.Check(p, new(), new ControlService(root, Path.Combine(root, "readiness-missing/state")));
    Require(!report.CanPrepare && report.Checks.Any(c => c.Id == "sboys-registration" && c.State == CheckState.Error) && report.Checks.Any(c => c.Id == "gaze-bridge" && c.State == CheckState.Error));
});
Test("Readiness accepts registered driver and enabled gaze DLL as file facts only", () =>
{
    var dll = Path.Combine(workspace, "artifacts/native/cheeky/bin/Release/CheekyOpenXRLayer.dll");
    var p = new VrProfile { Runtime = RuntimeKind.SboysSteamVr, QuadViews = QuadProvider.QuadViewsFoveated, Tracking = TrackingKind.Slam };
    var report = Readiness.Check(p, new() { Drivers = [new("CustomHeadsetOpenVR", root, true, false)], Layers = [new("Fixture", Make("readiness-present/gaze.json", "{\"file_format_version\":\"1.0.0\",\"api_layer\":{\"name\":\"XR_APILAYER_MBUCCHIA_eye_trackers\",\"library_path\":\"x.dll\",\"api_version\":\"1.0\",\"disable_environment\":\"DISABLE_XR_APILAYER_MBUCCHIA_eye_trackers\"}}"), true, "XR_APILAYER_MBUCCHIA_eye_trackers", dll, true)] }, new ControlService(root, Path.Combine(root, "readiness-present/state")));
    Require(report.Checks.Single(c => c.Id == "sboys-registration").State == CheckState.Pass && report.Checks.Single(c => c.Id == "gaze-bridge").State == CheckState.Pass);
    Require(report.Checks.All(c => c.Id != "sboys-tracking")); // tracking does not change deployment
});
Test("Readiness rejects a Sboys beta driver when preparing the pinned release", () =>
{
    var p = new VrProfile { Runtime = RuntimeKind.SboysSteamVr, Gaze = GazeMode.Fixed, Tracking = TrackingKind.Lighthouse };
    var report = Readiness.Check(p, new() { Drivers = [new("CustomHeadsetOpenVR", root, true, false, "1.3.0-beta.2")] }, new ControlService(root, Path.Combine(root, "readiness-beta/state")));
    Require(report.Checks.Single(c => c.Id == "sboys-version").State == CheckState.Error && !report.CanPrepare);
});
Test("Fixed focus avoids a gaze dependency without silently switching mode", () =>
{
    var p = new VrProfile { Runtime = RuntimeKind.SboysSteamVr, Gaze = GazeMode.Fixed, Tracking = TrackingKind.Lighthouse };
    var report = Readiness.Check(p, new(), new ControlService(root, Path.Combine(root, "readiness-fixed/state")));
    Require(report.Checks.All(c => c.Id != "gaze-bridge") && p.Gaze == GazeMode.Fixed);
});
Test("Package acquisition verifies an imported official archive and rejects altered input", () =>
{
    var package = PackageCatalog.Get("sboys"); var cache = Path.Combine(root, "acquisition/cache");
    var archive = PackageAcquisition.Acquire(package, cache, Path.Combine(workspace, ".cache/packages", package.ArchiveName)).GetAwaiter().GetResult();
    Require(Hashing.FileSha256(archive) == package.Sha256);
    var bad = Make("acquisition/bad.zip", "bad"); Throws<InvalidDataException>(() => PackageAcquisition.Acquire(package, cache, bad).GetAwaiter().GetResult());
    Require(Hashing.FileSha256(archive) == package.Sha256);
});
Test("Downloaded package checks its digest before committing cache bytes", () =>
{
    var bytes = System.Text.Encoding.UTF8.GetBytes("verified fixture archive");
    var package = new ComponentPackage("fixture", "1", "fixture.zip", "https://example.invalid/fixture.zip", Hashing.BytesSha256(bytes), "MIT", "fixture");
    var cache = Path.Combine(root, "network-fixture/cache");
    using var client = new HttpClient(new FixtureHttpHandler(bytes));
    var path = PackageAcquisition.Acquire(package, cache, client: client).GetAwaiter().GetResult(); Require(File.ReadAllBytes(path).SequenceEqual(bytes));
    AtomicFile.WriteText(path, "corrupt old cache"); using var badClient = new HttpClient(new FixtureHttpHandler([1, 2, 3]));
    Throws<InvalidDataException>(() => PackageAcquisition.Acquire(package, cache, client: badClient).GetAwaiter().GetResult());
    Require(File.ReadAllText(path) == "corrupt old cache");
});
Test("Runtime preflight rejects a non-PE or x86 DLL", () =>
{
    var dll = Make("readiness-bad/runtime.dll", "stub"); var runtime = Make("readiness-bad/runtime.json", JsonData.Serialize(new { runtime = new { library_path = dll } }));
    var report = Readiness.Check(new VrProfile { QuadViews = QuadProvider.None }, new() { PimaxRuntime = runtime }, new ControlService(root, Path.Combine(root, "readiness-bad/state")));
    Require(report.Checks.Single(c => c.Id == "runtime").State == CheckState.Error && !NativeBinary.IsX64(dll));
});
(ControlService Service, string Options, string Autoexec, ApplyPlan Plan) PacingFixture(string name, VrProfile profile, bool missingCap = false, bool legacyCap = false)
{
    var prefix = "pacing/" + name; var state = Path.Combine(root, prefix + "/state");
    var exe = Make(prefix + "/bin/DCS.exe", "Never execute this fixture");
    var options = Make(prefix + "/options.lua", missingCap ? "options={graphics={Upscaling=\"DLSS\"},VR={enable=false}}" : Lua.Replace("maxFPS=89", "maxFPS=89, sync=true"));
    var runtime = Make(prefix + "/runtime.json", "{\"runtime\":{\"library_path\":\"runtime.dll\"}}"); Make(prefix + "/runtime.dll", "Never load this fixture");
    Make("pacing-provider/ofxr/XR_APILAYER_XRFrameBridge_diagnostic.dll", "Never load this fixture");
    var autoexec = Path.Combine(root, prefix + "/autoexec.cfg"); if (legacyCap) AtomicFile.WriteText(autoexec, "-- legacy cap\nmax_fps = 35;");
    var service = new ControlService(root, state);
    var plan = new DeploymentPlanner(new(Path.Combine(root, "pacing-provider"), null, null)).Build(profile,
        new() { DcsExecutable = exe, OptionsPath = options, PimaxRuntime = runtime, SteamVrRuntime = runtime }, service.ManagedRoot);
    return (service, options, autoexec, plan);
}
foreach (var route in new[] { RuntimeKind.Pimax, RuntimeKind.SboysSteamVr })
foreach (var mode in Enum.GetValues<FpsLimitMode>())
    Test($"{route} framegen {mode} cap applies and restores without quality changes", () =>
    {
        var p = new VrProfile { Id = "fps-" + route + "-" + mode, Runtime = route, QuadViews = QuadProvider.None, FrameGen = FrameGeneration.Nvidia,
            FpsLimit = mode, HeadsetRefreshHz = 90, RenderedFpsCap = 47.5, DisableDcsVSync = mode != FpsLimitMode.Preserve, NeuralIntensity = .423456789 };
        var f = PacingFixture(route + "-" + mode, p); var original = File.ReadAllText(f.Options);
        var changes = f.Plan.Files.Single(file => file.Path == f.Options).LuaChanges!;
        Require(changes.Any(c => c.Path == "graphics.maxFPS") == (mode != FpsLimitMode.Preserve));
        var journal = f.Service.Apply(f.Plan); var lua = new LuaOptions(File.ReadAllText(f.Options));
        var expected = mode == FpsLimitMode.Preserve ? 89 : mode == FpsLimitMode.RuntimeHeadroom ? 300 : mode == FpsLimitMode.MatchRefresh ? 45 : 47.5;
        Require(FramePacing.PositiveNumber(lua.Get("graphics", "maxFPS")) == expected);
        Require(lua.Get("graphics", "sync") == (mode == FpsLimitMode.Preserve ? "true" : "false"));
        Require(f.Service.PrepareLaunchApplied().FileName.EndsWith("DCS.exe"));
        var saved = f.Plan.Files.Single(file => file.Path.EndsWith("profile.json")); Require(JsonData.Deserialize<VrProfile>(Encoding.UTF8.GetString(saved.Content)) == p);
        Require(f.Service.RestoreOriginals().Complete && File.ReadAllText(f.Options) == original);
    });
Test("Profiles saved with settings that no longer exist still load", () =>
{
    // Earlier versions saved self-reported limiters, an experimental consent and a Sboys folder; scripts may still send them.
    var profile = ProfilePresets.All[1];
    var obsolete = "\"externalLimiter\": \"Active\", \"externalLimiterFps\": 30, \"runtimeReprojection\": \"Off\", \"experimentalAcknowledged\": true, \"sboysDriverDirectory\": \"C:\\\\Sboys\", \"ExperimentalAcknowledged\": false, ";
    var json = JsonData.Serialize(profile).Insert(1, obsolete);
    Require(JsonData.Deserialize<VrProfile>(json) == profile, "profile.json");
    var draft = new SavedDraft(profile, null, null, DateTimeOffset.UnixEpoch);
    var draftJson = JsonData.Serialize(draft).Replace("\"profile\": {", "\"profile\": {" + obsolete);
    Require(draftJson.Contains("sboysDriverDirectory") && JsonData.Deserialize<SavedDraft>(draftJson).Profile == profile, "draft.json");
    Throws<System.Text.Json.JsonException>(() => JsonData.Deserialize<VrProfile>(JsonData.Serialize(profile).Insert(1, "\"notASetting\": 1, ")));
});
Test("Legacy profiles default to preserving all FPS controls", () =>
{
    var p = JsonData.Deserialize<VrProfile>("{\"schemaVersion\":1,\"id\":\"legacy\",\"name\":\"Legacy\"}");
    Require(p.FpsLimit == FpsLimitMode.Preserve && !p.DisableDcsVSync);
});
Test("Disabling framegen removes the half-refresh cap", () =>
{
    var p = new VrProfile { FpsLimit = FpsLimitMode.MatchRefresh, HeadsetRefreshHz = 90 };
    Require(FramePacing.RequestedCap(p) == 90 && FramePacing.RequestedCap(p with { FrameGen = FrameGeneration.FidelityFx }) == 45);
    var f = PacingFixture("fg-off", p); var journal = f.Service.Apply(f.Plan); Require(new LuaOptions(File.ReadAllText(f.Options)).Get("graphics", "maxFPS") == "90"); Require(f.Service.RestoreOriginals().Complete);
});
Test("Fractional refresh preserves half-rate precision", () => Require(FramePacing.RequestedCap(new() { FpsLimit = FpsLimitMode.MatchRefresh, HeadsetRefreshHz = 89.5, FrameGen = FrameGeneration.Nvidia }) == 44.75));
Test("FPS settings reject nonfinite rates and unrecognized modes", () =>
{
    foreach (var p in new[] { new VrProfile { HeadsetRefreshHz = double.NaN }, new() { HeadsetRefreshHz = 49 }, new() { RenderedFpsCap = double.PositiveInfinity }, new() { RenderedFpsCap = 29 }, new() { FpsLimit = (FpsLimitMode)90 } })
        Require(ProfileValidation.Validate(p).Any(i => i.Severity == IssueSeverity.Error));
});
Test("FPS ceiling distinguishes fresh frames from the selected refresh", () =>
{
    var p = new VrProfile { FrameGen = FrameGeneration.Nvidia, FpsLimit = FpsLimitMode.Custom, RenderedFpsCap = 30, HeadsetRefreshHz = 90 };
    var plan = FramePacing.Describe(p); Require(plan.RequiredRenderedFps == 45 && plan.ConfiguredOutputCeiling == 60);
    Require(FramePacing.Checks(p, new()).Any(c => c.Id == "fps-under-target" && c.State == CheckState.Warning));
    Require(FramePacing.Describe(p with { RenderedFpsCap = 300 }).ConfiguredOutputCeiling == 90);
});
Test("Other limiters and motion smoothing stay manual checks", () =>
{
    var checks = FramePacing.Checks(new() { FrameGen = FrameGeneration.Nvidia }, new());
    Require(checks.Single(c => c.Id == "external-limiters").State == CheckState.Manual && checks.Single(c => c.Id == "runtime-reprojection").State == CheckState.Manual);
    Require(FramePacing.Checks(new(), new()).All(c => c.Id != "runtime-reprojection"));
});
Test("Preflight flags legacy caps and desktop VSync", () =>
{
    var checks = FramePacing.Checks(new() { FrameGen = FrameGeneration.Nvidia },
        new() { AutoexecPath = "autoexec.cfg", AutoexecMaxFps = "35", DcsSettings = new Dictionary<string, string> { ["graphics.sync"] = "true" }, LimiterProcesses = ["RTSS"] });
    foreach (var id in new[] { "autoexec-pacing", "dcs-vsync" }) Require(checks.Single(c => c.Id == id).State == CheckState.Warning);
    Require(checks.Single(c => c.Id == "external-limiters").Detail.Contains("Running: RTSS"));
});
Test("Autoexec inspection reads plain caps and never executes commands", () =>
{
    Require(FramePacing.ReadAutoexec("-- comment\nmax_fps=35;") == ("35", false));
    Require(FramePacing.ReadAutoexec("-- max_fps=35\n") == (null, false));
    Require(FramePacing.ReadAutoexec("os.execute('never execute'); max_fps=35") == (null, true));
    Require(FramePacing.ReadAutoexec("max_fps = calculate_rate()") == (null, true));
    Require(FramePacing.ReadAutoexec("HUD_MFD_after_DLSS = true") == (null, false));
    // The HUD advice line next to a frame cap, also in its dotted form: both read, the cap still found.
    Require(FramePacing.ReadAutoexec("max_fps = 90\nHUD_MFD_after_DLSS = true") == ("90", false) && FramePacing.ReadAutoexec("options.graphics.HUD_MFD_after_DLSS = true\nmax_fps=72") == ("72", false));
    Require(FramePacing.ReadAutoexec("--[[\nmax_fps = 30\n--]]\n") == (null, false) && FramePacing.ReadAutoexec("--[==[ max_fps = 30 ]==] max_fps = 45") == ("45", false), "block comments");
    Require(FramePacing.ReadAutoexec("name = \"a--b\"\nmax_fps = 60") == ("60", false), "-- inside a string is no comment");
    Require(FramePacing.Checks(new(), new() { AutoexecPath = "autoexec.cfg" }).Single(c => c.Id == "autoexec-pacing").State == CheckState.Pass);
});
Test("FPS cap insertion restores absent cap and VSync fields", () =>
{
    var f = PacingFixture("missing-fields", new() { FpsLimit = FpsLimitMode.Custom, DisableDcsVSync = true }, missingCap: true); var before = File.ReadAllText(f.Options);
    var journal = f.Service.Apply(f.Plan); Require(new LuaOptions(File.ReadAllText(f.Options)).Get("graphics", "maxFPS") == "45"); Require(f.Service.RestoreOriginals().Complete && File.ReadAllText(f.Options) == before);
});
Test("Saved launch rejects owned FPS edits; Back to stock DCS sets the owned cap back and keeps the rest", () =>
{
    var f = PacingFixture("edited-cap", new() { FpsLimit = FpsLimitMode.Custom }); f.Service.Apply(f.Plan);
    AtomicFile.WriteText(f.Options, new LuaOptions(File.ReadAllText(f.Options)).Set(["graphics", "maxFPS"], 61).Replace("-- preserve comment", "-- user"));
    Throws<IOException>(() => f.Service.PrepareLaunchApplied()); Require(f.Service.RestoreOriginals().Complete && new LuaOptions(File.ReadAllText(f.Options)).Get("graphics", "maxFPS") == "89" && File.ReadAllText(f.Options).Contains("-- user"));
});
foreach (var exists in new[] { false, true })
    Test("Saved launch rejects " + (exists ? "edited" : "new") + " legacy pacing file", () =>
    {
        var f = PacingFixture("autoexec-" + exists, new(), legacyCap: exists); var journal = f.Service.Apply(f.Plan);
        if (exists) Require(File.ReadAllText(f.Autoexec) == "-- legacy cap\nmax_fps = 35;");
        AtomicFile.WriteText(f.Autoexec, "max_fps=20;"); Throws<IOException>(() => f.Service.PrepareLaunchApplied());
        Require(f.Service.RestoreOriginals().Complete && File.ReadAllText(f.Autoexec) == "max_fps=20;");
    });
Test("Sboys gaze bridge loads explicitly below Quad Views", () =>
{
    var exe = Make("gaze-order/bin/DCS.exe", "fixture"); var options = Make("gaze-order/options.lua", Lua); var runtime = Make("gaze-order/runtime.json", "{\"runtime\":{\"library_path\":\"runtime.dll\"}}"); Make("gaze-order/runtime.dll", "fixture");
    Make("gaze-order/gaze/eye.dll", "fixture");
    var manifest = Make("gaze-order/gaze/eye.json", "{\"file_format_version\":\"1.0.0\",\"api_layer\":{\"name\":\"XR_APILAYER_MBUCCHIA_eye_trackers\",\"library_path\":\"eye.dll\",\"api_version\":\"1.0\",\"implementation_version\":\"1\",\"description\":\"fixture\",\"disable_environment\":\"DISABLE_XR_APILAYER_MBUCCHIA_eye_trackers\"}}");
    var ofxr = new ComponentCache(Path.Combine(root, "gaze-order/cache")).Import(PackageCatalog.Get("ofxr"), Path.Combine(workspace, ".cache/packages", PackageCatalog.Get("ofxr").ArchiveName));
    var p = new VrProfile { Runtime = RuntimeKind.SboysSteamVr, Tracking = TrackingKind.Lighthouse, QuadViews = QuadProvider.QuadViewsFoveated, FrameGen = FrameGeneration.Nvidia, Gaze = GazeMode.EyeTracked };
    var inv = new InventorySnapshot { DcsExecutable = exe, OptionsPath = options, SteamVrRuntime = runtime, Layers = [new("CurrentUser", manifest, true, "XR_APILAYER_MBUCCHIA_eye_trackers", Path.Combine(root, "gaze-order/gaze/eye.dll"), true)] };
    var plan = new DeploymentPlanner(new(ofxr, null, null, Path.Combine(workspace, "external/quadviews/bin/x64/Release"), OfxrLayerDll: DeferredOfxr)).Build(p, inv, Path.Combine(root, "gaze-order/managed"));
    Require(plan.LaunchEnvironment["XR_ENABLE_API_LAYERS"] == "XR_APILAYER_MBUCCHIA_quad_views_foveated;XR_APILAYER_XRFrameBridge_diagnostic;XR_APILAYER_MBUCCHIA_eye_trackers");
    Require(plan.LaunchEnvironment["DISABLE_XR_APILAYER_MBUCCHIA_eye_trackers"] == "1");
    Require(plan.Files.Any(f => f.Path.EndsWith("gaze-bridge.json") && Encoding.UTF8.GetString(f.Content).Contains("eye.dll")));
    var fixedGaze = new DeploymentPlanner(new(ofxr, null, null, Path.Combine(workspace, "external/quadviews/bin/x64/Release"), OfxrLayerDll: DeferredOfxr)).Build(p with { Gaze = GazeMode.Fixed }, inv, Path.Combine(root, "gaze-order/managed-fixed"));
    Require(!fixedGaze.LaunchEnvironment["XR_ENABLE_API_LAYERS"].Contains("eye_trackers"));
});
Test("Readiness rejects a gaze bridge that cannot be reordered", () =>
{
    var manifest = Make("gaze-unorderable/eye.json", "{\"file_format_version\":\"1.0.0\",\"api_layer\":{\"name\":\"XR_APILAYER_MBUCCHIA_eye_trackers\",\"library_path\":\"eye.dll\",\"api_version\":\"1.0\"}}");
    var p = new VrProfile { Runtime = RuntimeKind.SboysSteamVr, QuadViews = QuadProvider.QuadViewsFoveated, Tracking = TrackingKind.Slam };
    var dll = Path.Combine(workspace, "artifacts/native/cheeky/bin/Release/CheekyOpenXRLayer.dll");
    var report = Readiness.Check(p, new() { Layers = [new("Fixture", manifest, true, "XR_APILAYER_MBUCCHIA_eye_trackers", dll, true), new("Fixture", "toolkit.json", true, "XR_APILAYER_NOVENDOR_toolkit", dll, true)] }, new ControlService(root, Path.Combine(root, "gaze-unorderable/state")));
    Require(report.Checks.Single(c => c.Id == "gaze-bridge").State == CheckState.Error);
    var foreign = report.Checks.Single(c => c.Id == "implicit-layers"); Require(foreign.State == CheckState.Warning && foreign.Detail.Contains("toolkit") && !foreign.Detail.Contains("eye_trackers"));
});
Test("Readiness reports elevation and write access", () =>
{
    var report = Readiness.Check(new VrProfile(), new() { DcsExecutable = Make("elevation/bin/DCS.exe", "fixture"), OptionsPath = Make("elevation/Config/options.lua", Lua) }, new ControlService(root, Path.Combine(root, "elevation/state")));
    Require(report.Checks.Single(c => c.Id == "elevation").State == (LaunchSafety.CurrentProcessElevated ? CheckState.Error : CheckState.Pass));
    Require(report.Checks.Single(c => c.Id == "write-access").State == CheckState.Pass);
    Require(LaunchSafety.CanWrite(Path.Combine(root, "elevation/Config")) && !LaunchSafety.CanWrite(Path.Combine(root, "elevation/missing")));
    Require(Directory.GetFiles(Path.Combine(root, "elevation/Config")).Length == 1);
});
Test("Junctions above the authorized root are accepted, links below it are rejected", () =>
{
    var target = Path.GetFullPath(Path.Combine(root, "junction/target")); Directory.CreateDirectory(target);
    var link = Path.GetFullPath(Path.Combine(root, "junction/moved")); var inner = Path.GetFullPath(Path.Combine(root, "junction/real/linked"));
    Directory.CreateDirectory(Path.Combine(root, "junction/real"));
    foreach (var (from, to) in new[] { (link, target), (inner, target) })
    {
        using var mklink = Process.Start(new ProcessStartInfo("cmd.exe", $"/c mklink /J \"{from}\" \"{to}\"") { UseShellExecute = false, RedirectStandardOutput = true, CreateNoWindow = true })!;
        mklink.WaitForExit(); Require(mklink.ExitCode == 0, "mklink /J failed");
    }
    Require(PathPolicy.UnderRoot(link, "Config/options.lua").StartsWith(link, StringComparison.OrdinalIgnoreCase));
    PathPolicy.RejectReparsePoints(Path.Combine(link, "options.lua"));
    Throws<InvalidDataException>(() => PathPolicy.UnderRoot(Path.Combine(root, "junction/real"), "linked/options.lua"));
    Throws<InvalidDataException>(() => PathPolicy.RejectReparsePoints(link));
});
Test("A profile replacing the applied one previews and applies directly over it", () =>
{
    var (fixture, runtime, _, options, exe) = LaunchFixture("replace");
    var service = new ControlService(Path.Combine(workspace, ".cache"), fixture.StateRoot);
    var p = new VrProfile { FrameGen = FrameGeneration.Nvidia, QuadViews = QuadProvider.None };
    var inventory = new InventorySnapshot { DcsExecutable = exe, OptionsPath = options, PimaxRuntime = runtime };
    var plan = service.Preview(p, inventory);
    Require(Readiness.Check(p, inventory, service).Checks.Single(c => c.Id == "active-backup").State == CheckState.Pass, "An applied profile is never a blocker");
    var before = service.Originals.Status();
    Require(service.Apply(plan).Current.Status == "applied" && service.Originals.Status() is { State: "applied" } after && after.Files.Count(f => f.Action == "settings") == 1, "Applied over the first profile");
    Require(service.Originals.Status().Files.Single(f => f.Action == "settings").Path == before.Files.Single(f => f.Action == "settings").Path, "options.lua keeps its first original");
    Require(service.Preview(p, inventory).Files.Count == plan.Files.Count && service.RestoreOriginals().Complete && File.ReadAllText(options) == Lua);
});
Test("Preview removes its verified extractions", () =>
{
    var (fixture, runtime, _, options, exe) = LaunchFixture("staging");
    Require(fixture.RestoreOriginals().Complete);
    var service = new ControlService(Path.Combine(workspace, ".cache"), fixture.StateRoot); // .cache/packages holds the pinned archives.
    service.Preview(new() { FrameGen = FrameGeneration.Nvidia, QuadViews = QuadProvider.None }, new() { DcsExecutable = exe, OptionsPath = options, PimaxRuntime = runtime });
    var staging = Path.Combine(service.StateRoot, "cache/staging");
    Require(!Directory.Exists(staging) || Directory.GetDirectories(staging).Length == 0);
});
Test("Lua decodes numeric, hexadecimal and skip escapes and compares numbers by value", () =>
{
    var lua = new LuaOptions("options={graphics={maxFPS=0x5A, scale=.5, neg=-1e2, s=\"\\65\\x42\\z   C\"}}");
    Require(LuaOptions.NormalizeLiteral(lua.Get("graphics", "maxFPS")) == LuaOptions.NormalizeLiteral("90.0"));
    Require(LuaOptions.NormalizeLiteral(lua.Get("graphics", "scale")) == LuaOptions.NormalizeLiteral("0.5"));
    Require(LuaOptions.NormalizeLiteral(lua.Get("graphics", "neg")) == LuaOptions.NormalizeLiteral("-100"));
    // DCS uses Lua 5.1: \ddd is decimal; \x and \z are not escapes and keep their letter.
    Require(LuaOptions.NormalizeLiteral(lua.Get("graphics", "s")) == LuaOptions.NormalizeLiteral("\"Ax42z   C\""));
    Throws<InvalidDataException>(() => new LuaOptions("options={a=\"\\300\"}"));
    Throws<InvalidDataException>(() => new LuaOptions("options={a=0x}"));
});
Test("NVIDIA signer requires the exact organization", () =>
{
    Require(NativeBinary.IsNvidiaSigner("CN=NVIDIA Corporation, OU=2-J, O=NVIDIA Corporation, L=Santa Clara, S=California, C=US"));
    Require(!NativeBinary.IsNvidiaSigner("CN=NVIDIA Corporation, O=Not NVIDIA Corporation Ltd, C=US"));
    Require(!NativeBinary.IsNvidiaSigner("CN=NVIDIA Corporation"));
    Require(!NativeBinary.IsNvidiaSigner(null));
});
Test("Component-normalized INI keeps launch working and Back to stock DCS removes it either way", () =>
{
    var ini = ConfigurationWriters.Cheeky(new VrProfile { NeuralRendering = true, NeuralIntensity = .35 });
    var target = Path.Combine(root, "ini-normalized/CheekyFoveatedDLSS.ini");
    var store = new OriginalsStore(Path.Combine(root, "ini-normalized/state"));
    var plan = new ApplyPlan("ini", "ini", [new(target, null, Encoding.UTF8.GetBytes(ini), "DLSS and gaze settings", IniValues: IniFile.Parse(ini))], new Dictionary<string, string>(), "DCS.exe");
    var applied = store.Apply(plan).Current;
    // What Cheeky 0.5.x wrote back in a real DCS start: extra defaults and float32 formatting.
    File.WriteAllText(target, ini.Replace("NrIntensity=0.35", "NrIntensity=0.349999994").Replace("TransitionWidth=0.04", "TransitionWidth=0.0399999991") + "CenterPreset=0\nRrCenterPreset=0\n");
    Require(IniFile.Matches(File.ReadAllText(target), applied.Entries[0].IniValues!));
    Require(store.RestoreOriginals().Complete && !File.Exists(target));
    var second = store.Apply(plan).Current;
    File.WriteAllText(target, ini.Replace("NrEnabled=1", "NrEnabled=0"));
    Require(!IniFile.Matches(File.ReadAllText(target), second.Entries[0].IniValues!));
    Require(store.RestoreOriginals().Complete && !File.Exists(target));
});
Test("Steam edition launch keeps the profile environment", () =>
{
    var steamExe = Make("steam-env/steamapps/common/DCSWorld/bin/DCS.exe", "fixture"); var options = Make("steam-env/options.lua", Lua);
    var runtime = Make("steam-env/runtime.json", "{\"runtime\":{\"library_path\":\"runtime.dll\"}}"); Make("steam-env/runtime.dll", "fixture");
    var steam = new DeploymentPlanner(new(null, null, null)).Build(new VrProfile(), new() { DcsExecutable = steamExe, OptionsPath = options, PimaxRuntime = runtime }, Path.Combine(root, "steam-env/managed"));
    Require(steam.LaunchEnvironment["SteamAppId"] == "223750" && steam.LaunchEnvironment["SteamGameId"] == "223750");
    var standaloneExe = Make("standalone-env/DCS World/bin/DCS.exe", "fixture"); var standaloneOptions = Make("standalone-env/options.lua", Lua);
    var standalone = new DeploymentPlanner(new(null, null, null)).Build(new VrProfile(), new() { DcsExecutable = standaloneExe, OptionsPath = standaloneOptions, PimaxRuntime = runtime }, Path.Combine(root, "standalone-env/managed"));
    Require(!standalone.LaunchEnvironment.ContainsKey("SteamAppId"));
});
Test("Restore removes declared component logs only, never other files", () =>
{
    var bin = Path.Combine(root, "runtime-logs/bin"); Directory.CreateDirectory(bin);
    var keep = Make("runtime-logs/bin/user.log", "not ours");
    var loader = Path.Combine(bin, "dxgi.dll"); var host = Path.Combine(bin, "Comp/Host.dll");
    var logs = new[] { Path.Combine(bin, "Comp-Loader.log"), Path.Combine(bin, "Comp/Comp-Host.log"), Path.Combine(bin, "Comp/not-a-log.txt") };
    var store = new OriginalsStore(Path.Combine(root, "runtime-logs/state"));
    store.Apply(new ApplyPlan("logs", "logs", [new(loader, null, [1], "loader", RuntimeLogs: logs), new(host, null, [2], "host")], new Dictionary<string, string>(), "DCS.exe"));
    File.WriteAllText(logs[0], "runtime"); File.WriteAllText(logs[1], "runtime"); File.WriteAllText(logs[2], "not a log");
    Require(store.RestoreOriginals().Complete);
    Require(!File.Exists(loader) && !File.Exists(logs[0]) && !File.Exists(logs[1]) && File.Exists(logs[2]) && File.Exists(keep));
});
Test("Eye-tracked Sboys profile without a gaze bridge is flagged, not blocked", () =>
{
    var p = new VrProfile { Runtime = RuntimeKind.SboysSteamVr, Tracking = TrackingKind.Lighthouse, QuadViews = QuadProvider.QuadViewsFoveated, Gaze = GazeMode.EyeTracked };
    var issues = ProfileValidation.Validate(p, new InventorySnapshot { DcsExecutable = "x", OptionsPath = "y", SteamVrRuntime = "z" });
    Require(issues.Single(i => i.Code == "gaze-bridge-missing").Severity == IssueSeverity.Warning);
    Require(!ProfileValidation.Validate(p with { Gaze = GazeMode.Fixed }, new InventorySnapshot { DcsExecutable = "x", OptionsPath = "y", SteamVrRuntime = "z" }).Any(i => i.Code == "gaze-bridge-missing"));
});
Test("INI comparison is section-aware and ignores trailing comments", () =>
{
    var owned = IniFile.Parse("[A]\nX=1\n");
    Require(IniFile.Matches("[A]\nX=1 ; added by the component\n[B]\nX=2\n", owned));
    Require(!IniFile.Matches("[B]\nX=1\n[A]\nX=2\n", owned));
});
Test("Sboys 1.3 provides eye gaze through SteamVR without a bridge", () =>
{
    var p = new VrProfile { Runtime = RuntimeKind.SboysSteamVr, QuadViews = QuadProvider.QuadViewsFoveated, Tracking = TrackingKind.Slam, Gaze = GazeMode.EyeTracked };
    InventorySnapshot With(string version) => new() { DcsExecutable = "x", OptionsPath = "y", SteamVrRuntime = "z", Drivers = [new("CustomHeadsetOpenVR", root, true, false, version)] };
    Require(GazeBridge.RuntimeProvidesGaze(With("1.3.0")) && GazeBridge.RuntimeProvidesGaze(With("1.4.1")));
    Require(!GazeBridge.RuntimeProvidesGaze(With("1.3.0-beta.2")) && !GazeBridge.RuntimeProvidesGaze(With("1.2.9")));
    Require(!ProfileValidation.Validate(p, With("1.3.0")).Any(i => i.Code == "gaze-bridge-missing"));
    Require(ProfileValidation.Validate(p, With("1.2.9")).Any(i => i.Code == "gaze-bridge-missing"));
    var report = Readiness.Check(p, With("1.3.0"), new ControlService(root, Path.Combine(root, "sboys-gaze/state")));
    Require(report.Checks.Single(c => c.Id == "gaze-bridge").State == CheckState.Manual);
});
// Pimax Play's global.json as stored on a Crystal Super: runtime_quadviews_fine_fov_<eye>_<side> is the share left OUT of the focus.
string PimaxJson(int type, double outer, double nasal, double up, double down, double gaze = .25, double periphery = .1919, int transition = 1) =>
    "{\"piplay_quadviews_fine_type\":" + type + ",\"piplay_quadviews_quick_horizontal_gaze_fov_scale\":0.33,\"piplay_quadviews_quick_vertical_gaze_fov_scale\":0.33,"
    + "\"runtime_quadviews_horizontal_fov_scale\":0.26,\"runtime_quadviews_vertical_fov_scale\":0.37,"
    + FormattableString.Invariant($"\"runtime_quadviews_fine_fov_left_left\":{outer},\"runtime_quadviews_fine_fov_left_right\":{nasal},\"runtime_quadviews_fine_fov_left_up\":{up},\"runtime_quadviews_fine_fov_left_down\":{down},")
    + FormattableString.Invariant($"\"runtime_quadviews_fine_fov_right_left\":{nasal},\"runtime_quadviews_fine_fov_right_right\":{outer},\"runtime_quadviews_fine_fov_right_up\":{up},\"runtime_quadviews_fine_fov_right_down\":{down},")
    + FormattableString.Invariant($"\"runtime_quadviews_gaze_resolution_scale\":{gaze},\"runtime_quadviews_periphery_resolution_scale\":{periphery},\"runtime_quadviews_focus_transition_type\":{transition},\"runtime_quadviews_focus_blend_area\":0.05}}");
Test("Setup detection reads the Sboys route, tracking, gaze, refresh and Pimax Quad View values", () =>
{
    var log = Make("detect/vrserver.txt", "Sat 19:47 [Info] - Active HMD set to null.Null Serial Number\nSat 19:48 [Info] - Active HMD set to CustomHeadsetOpenVR.PimaxSlamCustomHMD\n");
    var vr = Make("detect/steamvr.vrsettings", "{\"steamvr\":{\"preferredRefreshRate\":72}}");
    var pimax = Make("detect/global.json", PimaxJson(1, .66, .20, .66, .66));
    var sources = new DetectionSources { SteamVrServerLog = log, SteamVrSettings = vr, PimaxPlaySettings = pimax, NeuralRuntimeFolders = [], ProcessRunning = name => name is "vrserver" or "pi_server" };
    var inventory = new InventorySnapshot { Drivers = [new("CustomHeadsetOpenVR", root, true, false, "1.3.0")] };
    var result = SetupDetection.Detect(ProfilePresets.All.Single(x => x.Id == "pimax-combined"), inventory, sources);
    var p = result.Profile;
    Require(result.Active.Route == RuntimeKind.SboysSteamVr && p.Runtime == RuntimeKind.SboysSteamVr && p.Tracking == TrackingKind.Slam && p.Gaze == GazeMode.EyeTracked);
    // Fine mode 0.66 outward / 0.20 toward the nose / 0.66 up and down: Pimax logged a 3478×2048 focus, 0.57 × 0.34 of 5424×5356 at 1.125.
    Require(p.HeadsetRefreshHz == 72 && Math.Abs(p.QuadFocusScale - 1.125) < 1e-9 && Math.Abs(p.PeripheralScale - .1919) < 1e-9 && Math.Abs(p.FoveaWidth - .57) < 1e-9 && Math.Abs(p.FoveaHeight - .34) < 1e-9 && p.QuadEdgeBlend == .2);
    Require(p.FoveaSource == FoveaSource.PimaxPlay && p.Id == "pimax-combined" && result.Detected.Count >= 8); // identity is left to the interface's naming
    Require(result.Notes.Any(n => n.StartsWith("Pimax Play · Fine: Center Resolution 125%, Peripheral Resolution 20%, Top → Center 33%")) && result.Notes.Any(n => n.Contains("centres the focus on your gaze")));
    // Detection shows Pimax Play's own labels and values, not converted fractions.
    Require(result.Detected.Single(d => d.Label == "L-Eye Right → Center").Value == "10%" && result.Detected.Single(d => d.Label == "Center Resolution").Value == "125%" && result.Detected.All(d => d.Label != "Focus width"));
    Require(!ProfileValidation.Validate(p).Any(i => i.Code == "peripheral-scale"));
});
Test("Pimax Play's stored values are the share left out on each side, not the focus size", () =>
{
    // Quick 33 %: the runtime stores 0.33 per side and logs a 4088×4038 focus at 5424×5356 × 1.125, i.e. 0.67 × 0.67.
    var quick = PimaxFovea.Read(Make("pimax-quick/global.json", PimaxJson(0, .33, .33, .33, .33)))!;
    Require(quick.Mode == "Quick" && Math.Abs(quick.Converted.Width - .67) < 1e-9 && Math.Abs(quick.Converted.Height - .67) < 1e-9 && quick.Converted.FocusScale == 1.125 && !quick.Converted.Asymmetric && !quick.Converted.Clamped);
    Require(Math.Abs(5424 * 1.125 * quick.Converted.Width - 4088) < 5 && Math.Abs(5356 * 1.125 * quick.Converted.Height - 4038) < 5);
    // Without saved per-side values the Quick sliders are used; the stale runtime_*_fov_scale pair (0.26/0.37) never is.
    var sliders = PimaxFovea.Read(Make("pimax-sliders/global.json", "{\"runtime_quadviews_gaze_resolution_scale\":0.25,\"runtime_quadviews_periphery_resolution_scale\":0.1919,\"runtime_quadviews_horizontal_fov_scale\":0.26,\"runtime_quadviews_vertical_fov_scale\":0.37,\"piplay_quadviews_quick_horizontal_gaze_fov_scale\":0.33,\"piplay_quadviews_quick_vertical_gaze_fov_scale\":0.33}"))!;
    // Without a transition key Pimax Play shows Linear, so the transition is on.
    Require(sliders.FromQuickSliders && Math.Abs(sliders.Converted.Width - .67) < 1e-9 && sliders.Converted.Transition && sliders.Play.TransitionMode == "Linear" && sliders.Mismatch is null);
    // A focus larger than the bundled provider's 90 % cap is capped and reported in Pimax Play's units.
    var wide = PimaxFovea.Read(Make("pimax-wide/global.json", PimaxJson(0, .05, .05, .05, .05)))!;
    Require(wide.Converted.Clamped && wide.Converted.Width == .9 && Math.Abs(wide.Converted.Height - .9) < 1e-9 && Math.Abs(wide.Converted.RequestedWidth - .95) < 1e-9);
    Require(PimaxFovea.CapNote(wide) is { } cap && cap.Contains("Horizontal FOV is 5% and Vertical FOV is 5%") && cap.Contains("Horizontal and Vertical FOV 10%") && PimaxFovea.Notes(wide).Contains(cap) && PimaxFovea.CapNote(quick) is null);
    Require(PimaxFovea.Read(Path.Combine(root, "pimax-missing.json")) is null && PimaxFovea.Read(Make("pimax-bad/global.json", "{\"runtime_quadviews_gaze_resolution_scale\":\"x\"}")) is null);
});
Test("The focus area follows Pimax Play only for bundled Quad Views with Pimax Play as the source, and is never written to Pimax", () =>
{
    var path = Make("pimax-resolve/global.json", PimaxJson(1, .66, .20, .66, .66, transition: -1)); var before = File.ReadAllText(path);
    var p = new VrProfile { QuadViews = QuadProvider.QuadViewsFoveated, FoveaWidth = .5, FoveaHeight = .5, QuadEdgeBlend = .25 };
    var resolved = PimaxFovea.Resolve(p, path);
    Require(resolved.Applies && Math.Abs(resolved.Profile.FoveaWidth - .57) < 1e-9 && Math.Abs(resolved.Profile.FoveaHeight - .34) < 1e-9 && resolved.Profile.QuadFocusScale == 1.125 && resolved.Profile.QuadEdgeBlend == 0, "Pimax transition off means no edge blending");
    var quad = ConfigurationWriters.QuadViews(resolved.Profile);
    Require(quad.Contains("horizontal_focus_section=0.57") && quad.Contains("vertical_focus_section=0.34") && quad.Contains("focus_multiplier=1.125") && quad.Contains("peripheral_multiplier=0.1919"));
    Require(PimaxFovea.Resolve(p with { FoveaSource = FoveaSource.Profile }, path).Profile == p with { FoveaSource = FoveaSource.Profile });
    Require(PimaxFovea.Resolve(p with { QuadViews = QuadProvider.PimaxNative }, path) is { Applies: false } native && native.Profile.FoveaWidth == .5);
    Require(PimaxFovea.Resolve(p with { QuadViewsLayerDirectory = "alternative" }, path).Profile.FoveaWidth == .5);
    var missing = PimaxFovea.Resolve(p, Path.Combine(root, "pimax-none.json"));
    Require(missing.Applies && missing.Pimax is null && missing.Profile == p && missing.Notes.Single().Contains("This profile's own focus values are used"));
    Require(File.ReadAllText(path) == before);
    // Launch reports Pimax values changed after Apply, and nothing when they are unchanged.
    // Profiles applied by earlier versions saved the converted values themselves; newer ones save the draft plus pimax-fovea.json.
    var written = AppliedFovea.From(resolved.Profile, resolved.Pimax!.Stamp);
    Require(PimaxFovea.ChangedSinceApply(resolved.Profile, path: path) is null && PimaxFovea.ChangedSinceApply(p, written, path) is null);
    File.WriteAllText(path, PimaxJson(0, .33, .33, .33, .33, transition: -1));
    Require(PimaxFovea.ChangedSinceApply(resolved.Profile, path: path) is { } changed && changed.Contains("Pimax Play now: Quick 33% × 33% · 125% · 20%"));
    Require(PimaxFovea.ChangedSinceApply(p, written, path) is { } changedDraft && changedDraft.Contains("Pimax Play now: Quick 33% × 33%"));
    Require(PimaxFovea.ChangedSinceApply(p with { FoveaSource = FoveaSource.Profile }, path: path) is null);
});
Test("Pimax Play's values are shown with Pimax Play's own conversion and rounding", () =>
{
    // The rules of Pimax Play's Quad View page (pimaxui quadviewFov store), on the float32 values Pimax stores.
    Require(PimaxPlayUnits.CenterResolution(.25) == 125 && PimaxPlayUnits.CenterResolution(0) == 100 && PimaxPlayUnits.CenterResolution(1) == 200);
    Require(PimaxPlayUnits.PeripheralResolution(.1918999999761581) == 20 && PimaxPlayUnits.PeripheralResolution(.1919) == 20 && PimaxPlayUnits.PeripheralResolution(0) == 1 && PimaxPlayUnits.PeripheralResolution(.0909) == 10);
    Require(PimaxPlayUnits.QuickFov(.3300000131130219) == 33 && PimaxPlayUnits.QuickFov(null) == 33 && PimaxPlayUnits.QuickFov(0) == 33 && PimaxPlayUnits.QuickFov(.01) == 5);
    Require(PimaxPlayUnits.FineFov(.6600000262260437) == 33 && PimaxPlayUnits.FineFov(.2000000029802322, inner: true) == 10 && PimaxPlayUnits.FineFov(.99) == 50 && PimaxPlayUnits.FineFov(.5799999833106995) == 29 && PimaxPlayUnits.FineFov(null, inner: true) == 10);
    Require(PimaxPlayUnits.VerticalOffset(0) == 0 && PimaxPlayUnits.VerticalOffset(null) == -10 && PimaxPlayUnits.VerticalOffset(-.05) == -10 && PimaxPlayUnits.VerticalOffset(.5) == 100);
    Require(PimaxPlayUnits.TransitionRange(.05000000074505806) == 5 && PimaxPlayUnits.TransitionRange(null) == 5 && PimaxPlayUnits.Alpha(.5) == 50 && PimaxPlayUnits.Alpha(null) == 50);
    Require(PimaxPlayUnits.TransitionMode(-1) == "Off" && PimaxPlayUnits.TransitionMode(0) == "Linear" && PimaxPlayUnits.TransitionMode(1) == "Alpha" && PimaxPlayUnits.TransitionMode(null) == "Linear" && PimaxPlayUnits.TransitionCurve(2) == "Sqrt" && PimaxPlayUnits.TransitionCurve(9) == "Linear");
    // Pimax's runtime rounds the focus to an even pixel count: its logged 4088×4038 (Quick 33 %) and 3478×2048 (Fine 33/10 · 33/33).
    Require(PimaxPlayUnits.FocusPixels(5424, .67, 1.125) == 4088 && PimaxPlayUnits.FocusPixels(5356, .67, 1.125) == 4038 && PimaxPlayUnits.FocusPixels(5424, .57, 1.125) == 3478 && PimaxPlayUnits.FocusPixels(5356, .34, 1.125) == 2048);
    // The user's global.json in Fine and in Quick mode: Pimax Play's labels, order and values.
    const string F33 = "0.6600000262260437", F10 = "0.2000000029802322", Q33 = "0.3300000131130219";
    string Saved(int type, string ll, string lr, string rl, string rr, string ud, double? offset = 0) =>
        "{\"piplay_quadviews_fine_type\":" + type + $",\"piplay_quadviews_quick_horizontal_gaze_fov_scale\":{Q33},\"piplay_quadviews_quick_vertical_gaze_fov_scale\":{Q33},"
        + $"\"piplay_quadviews_fine_fov_top_to_center\":{F33},\"piplay_quadviews_fine_fov_bottom_to_center\":{F33},\"piplay_quadviews_fine_fov_left_left_to_center\":{F33},\"piplay_quadviews_fine_fov_left_right_to_center\":{F10},\"piplay_quadviews_fine_fov_right_left_to_center\":{F10},\"piplay_quadviews_fine_fov_right_right_to_center\":{F33},"
        + $"\"runtime_quadviews_fine_fov_left_left\":{ll},\"runtime_quadviews_fine_fov_left_right\":{lr},\"runtime_quadviews_fine_fov_left_up\":{ud},\"runtime_quadviews_fine_fov_left_down\":{ud},"
        + $"\"runtime_quadviews_fine_fov_right_left\":{rl},\"runtime_quadviews_fine_fov_right_right\":{rr},\"runtime_quadviews_fine_fov_right_up\":{ud},\"runtime_quadviews_fine_fov_right_down\":{ud},"
        + "\"runtime_quadviews_gaze_resolution_scale\":0.25,\"runtime_quadviews_periphery_resolution_scale\":0.1918999999761581,\"runtime_quadviews_focus_transition_type\":1,\"runtime_quadviews_focus_blend_mode\":0,"
        + "\"runtime_quadviews_focus_blend_area\":0.05000000074505806,\"runtime_quadviews_focus_transition_opacity_percent\":0.5" + (offset is { } o ? FormattableString.Invariant($",\"runtime_quadviews_vertical_focus_offset\":{o}") : "") + "}";
    string Shows(PimaxQuadViewsSettings s) => string.Join(", ", s.Play.Controls.Select(c => c.Label + " " + c.Value));
    var fine = PimaxFovea.Read(Make("pimax-play-fine/global.json", Saved(1, F33, F10, F10, F33, F33)))!;
    Require(Shows(fine) == "Center Resolution 125%, Peripheral Resolution 20%, Top → Center 33%, Bottom → Center 33%, L-Eye Left → Center 33%, L-Eye Right → Center 10%, R-Eye Left → Center 10%, R-Eye Right → Center 33%, Transition Mode Alpha, Transition Range 5%, Alpha 50%", Shows(fine));
    Require(fine.Play.Short == "Fine 33/10 · 33/33 · 125% · 20%" && fine.Mismatch is null && Math.Abs(fine.Converted.Width - .57) < 1e-6 && Math.Abs(fine.Converted.Height - .34) < 1e-6 && fine.Converted.Transition);
    Require(PimaxFovea.Summary(fine).StartsWith("Pimax Play · Fine: Center Resolution 125%, Peripheral Resolution 20%, Top → Center 33%"));
    var quick = PimaxFovea.Read(Make("pimax-play-quick/global.json", Saved(0, Q33, Q33, Q33, Q33, Q33)))!;
    Require(Shows(quick) == "Center Resolution 125%, Peripheral Resolution 20%, Horizontal FOV 33%, Vertical FOV 33%, Vertical Offset 0, Transition Mode Alpha", Shows(quick));
    Require(quick.Play.Short == "Quick 33% × 33% · 125% · 20%" && quick.Mismatch is null && Math.Abs(quick.Converted.Width - .67) < 1e-6);
    Require(PimaxFovea.Read(Make("pimax-play-offset/global.json", Saved(0, Q33, Q33, Q33, Q33, Q33, offset: null)))!.Play.VerticalOffset == -10, "A missing offset shows Pimax Play's default");
    // Fine mode selected while the runtime still holds the Quick values (or the reverse): both are shown, the runtime's are used.
    var stale = PimaxFovea.Read(Make("pimax-play-stale/global.json", Saved(1, Q33, Q33, Q33, Q33, Q33)))!;
    Require(stale.Mismatch is { } m && m.Contains("L-Eye Right → Center shows 10% but the runtime uses 16%") && m.Contains("runtime renders with its own values") && Math.Abs(stale.Converted.Width - .67) < 1e-6, stale.Mismatch ?? "no mismatch");
    var quickStale = PimaxFovea.Read(Make("pimax-play-stale-quick/global.json", Saved(0, "0.3", "0.3", "0.3", "0.3", Q33)))!;
    Require(quickStale.Mismatch is { } q && q.Contains("Horizontal FOV shows 33% but the runtime uses 30%") && !q.Contains("Vertical FOV"), quickStale.Mismatch ?? "no mismatch");
    // The per-eye resolution comes from DCS's stereo views, not the Quad Views block logged before them.
    var log = Make("pimax-play-log/dcs.log", "x OpenXR:     View [0]: Recommended Width=1074 Height=1060 SampleCount=1\nx OpenXR:     View [1]: Recommended Width=1074 Height=1060 SampleCount=1\nx OpenXR:     View [2]: Recommended Width=3478 Height=2048 SampleCount=1\nx OpenXR:     View [3]: Recommended Width=3478 Height=2048 SampleCount=1\nx OpenXR:     View [0]: Recommended Width=5424 Height=5356 SampleCount=1\nx OpenXR:     View [1]: Recommended Width=5424 Height=5356 SampleCount=1\n");
    Require(PimaxFovea.ReadEyeResolution(log) == (5424, 5356) && PimaxFovea.ReadEyeResolution(Path.Combine(root, "pimax-play-log/none.log")) is null);
    // Only the head of dcs.log is read (the views are logged as the session starts), shared with a DCS still writing it,
    // and a changed file is read again.
    var stereo = "x OpenXR:     View [0]: Recommended Width=4000 Height=3900 SampleCount=1\nx OpenXR:     View [1]: Recommended Width=4000 Height=3900 SampleCount=1\n";
    var big = Make("pimax-play-log/big.log", stereo + new string('.', PimaxFovea.EyeResolutionHead) + "\n" + stereo.Replace("4000", "9999"));
    using (var writer = new FileStream(big, FileMode.Open, FileAccess.ReadWrite, FileShare.ReadWrite))
        Require(PimaxFovea.ReadEyeResolution(big) == (4000, 3900), "Views beyond the head are not read; the file stays shared");
    File.AppendAllText(log, "x OpenXR:     View [0]: Recommended Width=4000 Height=3900 SampleCount=1\nx OpenXR:     View [1]: Recommended Width=4000 Height=3900 SampleCount=1\n");
    Require(PimaxFovea.ReadEyeResolution(log) == (4000, 3900), "Read again when the file changed");
    Require(PimaxFovea.ParseEyeResolution("x OpenXR:     View [0]: Recommended Width=99999999999 Height=1 SampleCount=1\nx OpenXR:     View [1]: Recommended Width=1 Height=1") is null);
});
Test("This profile's focus is edited in Pimax Play's Quick units and maps exactly like Pimax Quick", () =>
{
    // Quick 33 % × 33 % · 125 % · 20 % is the focus Pimax renders for those sliders: 0.67 × 0.67 at 1.125, periphery 0.1919.
    Require(PimaxPlayUnits.ShareFromQuickFov(33) == .67 && PimaxPlayUnits.DensityFromCenter(125) == 1.125 && PimaxPlayUnits.PeripheryFromPercent(20) == .1919);
    // Default profiles keep their stored values (Pimax's Fine 33/10 · 33/33 converted) and show them in Quick units.
    var p = new VrProfile();
    Require(p.FoveaWidth == .57 && p.FoveaHeight == .34 && PimaxPlayUnits.ProfileShort(p) == "43% × 66% · 125% · 20%", PimaxPlayUnits.ProfileShort(p));
    Require(PimaxPlayUnits.ShareFromQuickFov(43) == .57 && PimaxPlayUnits.ShareFromQuickFov(66) == .34);
    for (var q = 5; q <= 90; q++) Require(PimaxPlayUnits.JsRound(PimaxPlayUnits.QuickFovFromShare(PimaxPlayUnits.ShareFromQuickFov(q))) == q, "FOV " + q);
    for (var c = 100; c <= 200; c++) Require(PimaxPlayUnits.JsRound(PimaxPlayUnits.CenterFromDensity(PimaxPlayUnits.DensityFromCenter(c))) == c && PimaxPlayUnits.CenterResolution((PimaxPlayUnits.DensityFromCenter(c) - 1) * 2) == c, "Center " + c);
    for (var r = 16; r <= 100; r++) Require(PimaxPlayUnits.JsRound(PimaxPlayUnits.PeripheralPercent(PimaxPlayUnits.PeripheryFromPercent(r))) == r && PimaxPlayUnits.PeripheralResolution(PimaxPlayUnits.PeripheryFromPercent(r)) == r, "Peripheral " + r);
    // What Pimax Play would store for the same slider gives the same focus through the converter.
    foreach (var q in new[] { 10, 33, 50, 90 })
    {
        var share = PimaxPlayUnits.QuickStored(q);
        var converted = PimaxFovea.Convert(new(share, share, share, share), new(share, share, share, share), .25, .1919, true);
        Require(Math.Abs(converted.RequestedWidth - PimaxPlayUnits.ShareFromQuickFov(q)) < 1e-9, "Quick " + q);
    }
    // Every bundled Quad Views value the editor can produce stays within the profile's validated ranges.
    Require(!ProfileValidation.Validate(p with { QuadViews = QuadProvider.QuadViewsFoveated, FoveaWidth = PimaxPlayUnits.ShareFromQuickFov(90), FoveaHeight = PimaxPlayUnits.ShareFromQuickFov(5), QuadFocusScale = PimaxPlayUnits.DensityFromCenter(200), PeripheralScale = PimaxPlayUnits.PeripheryFromPercent(16) })
        .Any(i => i.Severity == IssueSeverity.Error));
});
Test("CPU Boost lists reject empty names and processes that start DCS", () =>
{
    var p = new VrProfile { CpuBoost = true };
    Require(!ProfileValidation.Validate(p).Any(i => i.Code.StartsWith("boost")), "Default Boost lists are valid");
    Require(ProfileValidation.Validate(p with { FreeVram = true, FreeVramApps = ["notepad", " "] }).Any(i => i.Code == "boost-app-name" && i.Severity == IssueSeverity.Error));
    foreach (var name in new[] { "steam", "Steam.exe", "explorer", "EpicGamesLauncher", "DCS", "DcsControl" })
        Require(ProfileValidation.Validate(p with { FreeVram = true, FreeVramApps = [name] }).Any(i => i.Code == "boost-launcher" && i.Severity == IssueSeverity.Error), name);
    Require(ProfileValidation.Validate(p with { BoostBackgroundApps = ["msedge", "explorer"] }).Any(i => i.Code == "boost-launcher"));
    Require(!ProfileValidation.Validate(p with { BoostBackgroundApps = ["explorer"], BoostMoveBackgroundApps = false }).Any(i => i.Code == "boost-launcher"), "An unused move list is not checked");
    Require(!ProfileValidation.Validate(p with { CpuBoost = false, BoostBackgroundApps = ["steam"] }).Any(i => i.Code.StartsWith("boost")), "Lists are checked only with Boost on");
    Require(ProfileValidation.Validate(p with { BoostDcsPriority = (BoostPriority)9 }).Any(i => i.Code == "boost-choice"));
    Require(ProfileValidation.Validate(new VrProfile { FoveaSource = (FoveaSource)7 }).Any(i => i.Code == "fovea-source"));
});
Test("Setup detection never infers the Pimax route from Pimax Play alone", () =>
{
    // Pimax Play also runs underneath Sboys, so it is not evidence of the route.
    var none = new DetectionSources { SteamVrServerLog = Path.Combine(root, "missing.txt"), PimaxPlaySettings = Path.Combine(root, "missing.json"), NeuralRuntimeFolders = [], ProcessRunning = name => name == "pi_server" };
    Require(SetupDetection.DetectActiveRoute(none).Route is null);
    var steamVrWithoutLine = none with { ProcessRunning = name => name is "vrserver" or "pi_server" };
    Require(SetupDetection.DetectActiveRoute(steamVrWithoutLine).Route is null);
    var idle = none with { ProcessRunning = _ => false };
    var result = SetupDetection.Detect(new VrProfile(), new(), idle);
    Require(result.Active.Route is null && result.Detected.Count == 0 && result.Notes.Count > 0 && result.Profile == new VrProfile());
});
Test("Readiness blocks a profile whose route does not match the running headset", () =>
{
    var previous = Readiness.ActiveRouteProvider;
    try
    {
        Readiness.ActiveRouteProvider = () => new(RuntimeKind.SboysSteamVr, "SteamVR is running the headset through Sboys.", TrackingKind.Slam);
        var pimax = Readiness.Check(new VrProfile(), new(), new ControlService(root, Path.Combine(root, "route-mismatch/state")));
        Require(pimax.Checks.Single(c => c.Id == "route-mismatch").State == CheckState.Error);
        var sboys = Readiness.Check(new VrProfile { Runtime = RuntimeKind.SboysSteamVr }, new(), new ControlService(root, Path.Combine(root, "route-mismatch/state2")));
        Require(sboys.Checks.Single(c => c.Id == "route-mismatch").State == CheckState.Pass);
    }
    finally { Readiness.ActiveRouteProvider = previous; }
});
Test("Cheeky package never extracts the version.dll fallback loader", () =>
{
    Require(PackageCatalog.Get("cheeky").ExcludedEntries!.Contains("version.dll"));
    var z = Zip("excluded.zip", ("dxgi.dll", "a"), ("version.dll", "b"), ("CheekyFoveatedDLSS/x.dll", "c"));
    var d = Path.Combine(root, "excluded"); ComponentCache.ExtractZip(z, d, ["version.dll"]);
    Require(File.Exists(Path.Combine(d, "dxgi.dll")) && !File.Exists(Path.Combine(d, "version.dll")) && File.Exists(Path.Combine(d, "CheekyFoveatedDLSS/x.dll")));
});
Test("SteamVR route is found even when its HMD line is older than the log tail", () =>
{
    var log = Path.Combine(root, "long-vrserver.txt");
    File.WriteAllText(log, "start [Info] - Active HMD set to CustomHeadsetOpenVR.PimaxSlamCustomHMD\n" + string.Concat(Enumerable.Repeat("[Info] - frame timing filler line for a long session\n", 20000)));
    var sources = new DetectionSources { SteamVrServerLog = log, ProcessRunning = name => name == "vrserver" };
    Require(SetupDetection.DetectActiveRoute(sources).Route == RuntimeKind.SboysSteamVr && new FileInfo(log).Length > 600 * 1024);
});
Test("Sboys versions newer than 1.3.0 pass, betas do not", () =>
{
    InventorySnapshot With(string version) => new() { Drivers = [new("CustomHeadsetOpenVR", root, true, false, version)] };
    var service = new ControlService(root, Path.Combine(root, "sboys-versions/state"));
    var p = new VrProfile { Runtime = RuntimeKind.SboysSteamVr, Tracking = TrackingKind.Slam };
    Require(Readiness.Check(p, With("1.4.0"), service).Checks.Single(c => c.Id == "sboys-version").State == CheckState.Pass);
    Require(Readiness.Check(p, With("1.3.0-beta.2"), service).Checks.Single(c => c.Id == "sboys-version").State == CheckState.Error);
});
Test("INI journals with unqualified keys from earlier builds still match", () =>
{
    var text = "[CheekyFoveatedDLSS]\nEnabled=1\nNrIntensity=0.349999994\n";
    Require(IniFile.Matches(text, new Dictionary<string, string> { ["Enabled"] = "1", ["NrIntensity"] = "0.35" }));
    Require(!IniFile.Matches(text, new Dictionary<string, string> { ["Enabled"] = "0" }));
    Require(!IniFile.Matches("[A]\nX=1\n[B]\nX=1\n", new Dictionary<string, string> { ["X"] = "1" })); // ambiguous name
});
Test("Application update and uninstall tolerate an owned file removed by antivirus", () =>
{
    var destination = Path.Combine(root, "av-install");
    var first = ReleaseFixture("av-first", "1.0.0", "first");
    File.WriteAllText(Path.Combine(first, "extra.dll"), "quarantined later");
    var manifest = JsonData.Deserialize<ReleaseManifest>(File.ReadAllText(Path.Combine(first, "release-manifest.json")));
    var files = manifest.Files.Append(new ReleaseFile("extra.dll", Hashing.FileSha256(Path.Combine(first, "extra.dll")), new FileInfo(Path.Combine(first, "extra.dll")).Length)).ToList();
    AtomicFile.WriteText(Path.Combine(first, "release-manifest.json"), JsonData.Serialize(manifest with { Files = files }));
    var installer = new ApplicationInstaller(destination);
    installer.Install(first);
    File.Delete(Path.Combine(destination, "extra.dll")); // what Defender does with a quarantined file
    File.Delete(Path.Combine(destination, "app.bin"));    // a missing file that the next version ships again
    installer.Install(ReleaseFixture("av-second", "1.0.1", "second"));
    Require(File.ReadAllText(Path.Combine(destination, "app.bin")) == "second" && !File.Exists(Path.Combine(destination, "extra.dll")));
    Require(installer.Uninstall().Complete && !File.Exists(Path.Combine(destination, "app.bin")));
});

// --- CPU Boost: topology parsing, plan building, restore bookkeeping and CLI parsing (no real processes) ---
const string Ryzen9800X3DCpuInfo =
    "CPU cores: 8, threads: 16\nCPU: AMD Ryzen 7 9800X3D 8-Core Processor\n" +
    "logical cores with performance class 6: {4, 5, 6, 7}\nlogical cores with performance class 5: {8, 9}\n" +
    "logical cores with performance class 4: {2, 3}\nlogical cores with performance class 3: {10, 11}\n" +
    "logical cores with performance class 2: {0, 1}\nlogical cores with performance class 1: {12, 13}\n" +
    "logical cores with performance class 0: {14, 15}\ncommon cores: {10, 11, 0, 1, 12, 13, 14, 15}\n" +
    "render cores: {4, 5, 6, 7, 8, 9, 2, 3}\nIO cores: {}\nunavailable cores: {}\n";
Test("Topology parses the 9800X3D CPPC block", () =>
{
    var topo = CpuTopology.ParseDcsLog(Ryzen9800X3DCpuInfo);
    Require(topo is not null && topo.Source == CoreRankingSource.DcsLog && topo.HasRanking);
    Require(topo!.LogicalCount == 16);
    Require(topo.RenderCores.SequenceEqual(new[] { 4, 5, 6, 7, 8, 9, 2, 3 }));
    Require(topo.VrRuntimeCores.SequenceEqual(new[] { 10, 11, 0, 1, 12, 13, 14, 15 }));
    Require(topo.BackgroundCores.SequenceEqual(new[] { 14, 15 })); // lowest performance class, at least two, never all
});
Test("Topology yields no ranking for a homogeneous CPU", () =>
{
    Require(CpuTopology.ParseDcsLog("logical cores with performance class 0: {0, 1, 2, 3}\nrender cores: {0, 1, 2, 3}\ncommon cores: {}") is null);
    Require(CpuTopology.ParseDcsLog("no cpu information here") is null);
});
Test("Topology Detect never throws and reports a logical count", () => { var t = CpuTopology.Detect(); Require(t is not null && t.LogicalCount > 0); });
var boostTopo = CpuTopology.ParseDcsLog(Ryzen9800X3DCpuInfo)!;
BoostSnapshot BoostFixture() => new(new List<BoostProcessFact>
{
    new(100, "DCS", 50, true),
    new(50, "msedge", 1, true),      // the only msedge is an ancestor of DCS
    new(1, "explorer", 0, true),
    new(200, "pi_server", 1, false), // a service under another account -> needs administrator
    new(300, "PimaxClient", 1, true),
}, DcsPid: 100);
Test("Boost plan lists Pimax services, DCS and admin need, and excludes an ancestor background app", () =>
{
    var plan = BoostPlanner.Compose(new VrProfile { CpuBoost = true }, boostTopo, BoostFixture());
    Require(plan.Source == CoreRankingSource.DcsLog && plan.RenderCores.SequenceEqual(boostTopo.RenderCores));
    var dcs = plan.Processes.Single(p => p.Category == "DCS");
    Require(dcs is { Running: true, NeedsAdministrator: false } && dcs.Action.Contains("all 16 CPUs"));
    var pi = plan.Processes.Single(p => p.Name == "pi_server");
    Require(pi is { Category: "VR runtime", Running: true, NeedsAdministrator: true });
    Require(plan.Processes.Single(p => p.Name == "PimaxClient").Running);
    Require(plan.Processes.Single(p => p.Name == "msedge") is { Category: "Background", Running: false });
    Require(plan.Notes.Any(n => n.Contains("Skipped background app msedge")));
    // The prefetch fix is an engine optimization now, no longer part of what CPU Boost does.
    Require(plan.Notes.All(n => !n.Contains("refetch", StringComparison.Ordinal)));
});
Test("Boost plan leaves SteamVR compositor untouched on the Sboys route", () =>
{
    var plan = BoostPlanner.Compose(new VrProfile { CpuBoost = true, Runtime = RuntimeKind.SboysSteamVr, QuadViews = QuadProvider.None }, boostTopo, BoostFixture());
    Require(!plan.Processes.Any(p => p.Category == "VR runtime"));
    Require(plan.Notes.Any(n => n.Contains("SteamVR route")));
});
Test("Boost plan with no ranking only touches DCS priority", () =>
{
    var plan = BoostPlanner.Compose(new VrProfile { CpuBoost = true }, CpuTopology.None("homogeneous fixture"), BoostFixture());
    Require(plan.Source == CoreRankingSource.None);
    Require(!plan.Processes.Any(p => p.Category is "VR runtime" or "Background"));
    Require(plan.Processes.Any(p => p.Category == "DCS"));
    Require(plan.Notes.Any(n => n.Contains("No CPU core ranking")));
});
Test("Boost plan ancestor walk collects the DCS parent chain", () =>
{
    var snap = new BoostSnapshot(new List<BoostProcessFact> { new(100, "DCS", 50, true), new(50, "steam", 1, true), new(1, "explorer", 0, true) }, 100);
    var ancestors = BoostPlanner.AncestorPids(snap);
    Require(ancestors.SetEquals(new[] { 50, 1 }) && !ancestors.Contains(100));
});
Test("Boost ledger records once, normalizes inherited masks, and matches by identity", () =>
{
    var ledger = new BoostLedger();
    ulong all = 0xFFFF, vr = 0xFC00, bg = 0xF000; var ours = new[] { vr, bg };
    Require(ledger.TryRecord(10, "msedge", 123, all, "Normal", bg, "BelowNormal", all, ours, out var c1));
    Require(c1.OriginalAffinity == all && c1.OriginalPriority == "Normal" && c1.AppliedAffinity == bg);
    Require(!ledger.TryRecord(10, "msedge", 123, bg, "BelowNormal", bg, "BelowNormal", all, ours, out _));
    Require(ledger.Changes.Count == 1);
    Require(ledger.TryRecord(20, "child", 5, bg, "BelowNormal", bg, "BelowNormal", all, ours, out var c2));
    Require(c2.OriginalAffinity == all && c2.OriginalPriority == "Normal"); // inherited our mask -> real original is all CPUs
    Require(BoostLedger.SameProcess(c1, "msedge", 123) && !BoostLedger.SameProcess(c1, "msedge", 999) && !BoostLedger.SameProcess(c1, "other", 123));
});
Test("Boost CLI arguments parse and reject bad input", () =>
{
    var a = BoostArguments.Parse(new[] { "--profile", "p.json", "--dcs-pid", "42", "--state", "s" });
    Require(a is { ProfilePath: "p.json", DcsPid: 42, StateRoot: "s", Error: null, Help: false });
    Require(BoostArguments.Parse(new[] { "--dcs-pid", "x" }).Error is not null);
    Require(BoostArguments.Parse(new[] { "--dcs-pid", "-3" }).Error is not null);
    Require(BoostArguments.Parse(new[] { "--profile" }).Error is not null);
    Require(BoostArguments.Parse(new[] { "--bogus" }).Error is not null);
    Require(BoostArguments.Parse(new[] { "--help" }).Help);
    Require(BoostArguments.Parse(Array.Empty<string>()) is { Error: null, Help: false, ProfilePath: null, DcsPid: null });
    Require(BoostArguments.Parse(new[] { "--dcs-exe", @"C:\DCS\bin\DCS.exe", "--dcs-log", @"C:\SG\DCS\Logs\dcs.log" }) is { DcsExecutable: @"C:\DCS\bin\DCS.exe", DcsLog: @"C:\SG\DCS\Logs\dcs.log", Error: null });
    Require(BoostArguments.Parse(new[] { "--dcs-exe" }).Error is not null && BoostArguments.Parse(new[] { "--dcs-log" }).Error is not null);
});
Test("Boost name matching honours * and ? anywhere, case-insensitively, with optional .exe", () =>
{
    Require(BoostPlanner.MatchesProcessName("platform_runtime_*_service", "platform_runtime_VR4_service"));
    Require(BoostPlanner.MatchesProcessName("platform_runtime_*_service", "Platform_Runtime_x_Service.exe"));
    Require(!BoostPlanner.MatchesProcessName("platform_runtime_*_service", "platform_runtime_VR4_service_helper"));
    Require(BoostPlanner.MatchesProcessName("pi_?erver", "pi_server") && !BoostPlanner.MatchesProcessName("pi_?erver", "pi_serverx"));
    Require(BoostPlanner.MatchesProcessName("msedge.exe", "MSEDGE") && BoostPlanner.MatchesProcessName("msedge", "msedge.exe"));
    Require(BoostPlanner.MatchesProcessName("*edge*", "msedgewebview2") && BoostPlanner.MatchesProcessName("ms*", "ms"));
    Require(!BoostPlanner.MatchesProcessName("msedge", "msedgewebview2") && !BoostPlanner.MatchesProcessName("chrome", "chrom"));
    Require(BoostPlanner.MatchesProcessName("*", "anything") && BoostPlanner.MatchesProcessName("D*", "DCS"));
});
Test("Boost plan resolves a mid-pattern wildcard and never picks DCS or protected processes", () =>
{
    var snap = new BoostSnapshot(new List<BoostProcessFact>
    {
        new(100, "DCS", 50, true), new(50, "steam", 1, true), new(1, "explorer", 0, true),
        new(400, "platform_runtime_VR4_service", 1, true), new(500, "DcsVr.Cli", 1, true), new(600, "audiodg", 1, true),
    }, 100);
    var plan = BoostPlanner.Compose(new VrProfile { CpuBoost = true, BoostBackgroundApps = ["D*", "a*"] }, boostTopo, snap);
    Require(plan.Processes.Single(p => p.Name == "platform_runtime_*_service").Running, "mid-pattern wildcard matched");
    Require(plan.Processes.Single(p => p.Name == "D*") is { Running: false }, "D* never resolves to DCS or the helper");
    Require(plan.Processes.Single(p => p.Name == "a*") is { Running: false }, "a* never resolves to audiodg");
    Require(plan.Notes.Any(n => n.Contains("Skipped background app D*")));
});
Test("Boost validation refuses patterns that match protected processes or every process, and warns on High", () =>
{
    var p = new VrProfile { CpuBoost = true };
    foreach (var pattern in new[] { "D*", "s*", "*team", "DCS_upd?ter", "audiodg", "DcsVr.Cli", "csrss.exe", "svc*", "*e*" })
        Require(ProfileValidation.Validate(p with { BoostBackgroundApps = [pattern] }).Any(i => i.Code == "boost-launcher" && i.Severity == IssueSeverity.Error), pattern);
    foreach (var pattern in new[] { "*", "**", "*.exe", "?*", "???" })
        Require(ProfileValidation.Validate(p with { FreeVram = true, FreeVramApps = [pattern] }).Any(i => i.Code == "boost-app-wildcard" && i.Severity == IssueSeverity.Error), pattern);
    foreach (var pattern in new[] { "msedge*", "platform_runtime_*_service", "Spot?fy", "ms-teams" })
        Require(!ProfileValidation.Validate(p with { BoostBackgroundApps = [pattern], FreeVram = true, FreeVramApps = [pattern] }).Any(i => i.Severity == IssueSeverity.Error && i.Code.StartsWith("boost")), pattern);
    var high = ProfileValidation.Validate(p with { BoostDcsPriority = BoostPriority.High });
    Require(high.Any(i => i is { Code: "boost-high-priority", Severity: IssueSeverity.Warning } && i.Message.Contains("compositor")));
    Require(!high.Any(i => i.Code.StartsWith("boost") && i.Severity == IssueSeverity.Error));
    Require(!ProfileValidation.Validate(p).Any(i => i.Code == "boost-high-priority"));
});
Test("Boost ledger keys by PID and start time and re-records a recycled PID", () =>
{
    var ledger = new BoostLedger();
    ulong all = 0xFFFF, bg = 0xC000; var ours = new[] { 0x0F00UL, bg };
    Require(ledger.TryRecord(10, "msedge", 100, all, "Normal", bg, "BelowNormal", all, ours, out _) && ledger.Contains(10, 100));
    Require(!ledger.TryRecord(10, "msedge", 100, bg, "BelowNormal", bg, "BelowNormal", all, ours, out _));
    Require(ledger.TryRecord(10, "chrome", 200, 0x00FF, "High", bg, "BelowNormal", all, ours, out var recycled), "a new process with the same PID is recorded");
    Require(recycled is { Name: "chrome", StartTicks: 200, OriginalAffinity: 0x00FF, OriginalPriority: "High" });
    Require(ledger.Changes.Count == 1 && !ledger.Contains(10, 100) && ledger.Contains(10, 200));
});
Test("Boost attach follows the Steam self-restart and never picks a young or foreign DCS", () =>
{
    var now = new DateTime(2026, 10, 4, 12, 0, 0);
    var min = TimeSpan.FromSeconds(20);
    const string exe = @"C:\Games\DCS World\bin\DCS.exe";
    var first = new DcsCandidate(111, now.AddSeconds(-5), exe);
    var restarted = new DcsCandidate(222, now.AddSeconds(-30), exe);
    var foreign = new DcsCandidate(333, now.AddMinutes(-10), @"D:\Other\bin\DCS.exe");
    Require(BoostAttach.Choose([first], 111, exe, min, now) is null, "the hinted DCS is too young");
    Require(BoostAttach.Choose([first with { StartTime = now.AddSeconds(-25) }], 111, exe, min, now) == 111);
    Require(BoostAttach.Choose([restarted], 111, exe, min, now) == 222, "hint exited: the restarted DCS from the same executable");
    Require(BoostAttach.Choose([restarted with { StartTime = now.AddSeconds(-3) }], 111, exe, min, now) is null, "the restarted DCS must also run long enough");
    Require(BoostAttach.Choose([foreign], 111, exe, min, now) is null, "another installation is never chosen");
    Require(BoostAttach.Choose([foreign with { ImagePath = null }], 111, exe, min, now) is null, "an unreadable path is not trusted");
    Require(BoostAttach.Choose([foreign, restarted], 222, exe, min, now) == 222, "a live hint wins");
    Require(BoostAttach.Choose([foreign], null, null, min, now) == 333, "no hint and no executable: any DCS");
    Require(BoostAttach.Choose([], 111, exe, min, now) is null);
});
Test("Boost topology is limited to the system mask and skips affinity with several processor groups", () =>
{
    var multi = CpuTopology.LimitToSystem(boostTopo, processorGroups: 2, systemMask: ulong.MaxValue, processorCount: 128);
    Require(!multi.HasRanking && multi.Source == CoreRankingSource.None && multi.Description.Contains("processor groups") && multi.LogicalCount == 128);
    Require(!CpuTopology.LimitToSystem(boostTopo, 1, 0, 72).HasRanking, "more than 64 logical CPUs");
    var plan = BoostPlanner.Compose(new VrProfile { CpuBoost = true }, multi, BoostFixture());
    Require(plan.Notes.Any(n => n.Contains("processor groups")) && !plan.Processes.Any(p => p.Category is "VR runtime" or "Background"));
    Require(plan.Processes.Single(p => p.Category == "DCS").Action.Contains("affinity unchanged"));
    var exact = CpuTopology.LimitToSystem(boostTopo, 1, 0xFFFF, 16);
    Require(exact.HasRanking && exact.AllCpusMask == 0xFFFF && exact.LogicalCount == 16 && exact.BackgroundCores.SequenceEqual(new[] { 14, 15 }));
    var partial = CpuTopology.LimitToSystem(boostTopo, 1, 0x3FFF, 14); // CPUs 14 and 15 unavailable to this PC
    Require(!partial.HasRanking && partial.AllCpusMask == 0x3FFF, "background cores outside the system mask give no ranking");
    var trimmed = CpuTopology.LimitToSystem(boostTopo, 1, 0xFFF3, 14); // render CPUs 2 and 3 unavailable
    Require(trimmed.HasRanking && !trimmed.RenderCores.Contains(2) && !trimmed.RenderCores.Contains(3) && trimmed.RenderCores.Contains(4) && trimmed.LogicalCount == 14);
});
Test("Boost reads dcs.log while DCS holds it open for writing, and only its head", () =>
{
    var log = PathPolicy.UnderRoot(root, "boost-log/Logs/dcs.log"); Directory.CreateDirectory(Path.GetDirectoryName(log)!);
    using (var writer = new FileStream(log, FileMode.Create, FileAccess.Write, FileShare.Read))
    {
        var bytes = Encoding.UTF8.GetBytes(Ryzen9800X3DCpuInfo + new string('x', 100_000)); writer.Write(bytes); writer.Flush();
        Throws<IOException>(() => File.ReadAllText(log)); // the old read: sharing violation while DCS writes
        var head = CpuTopology.ReadLogHead(log);
        Require(head.Length == CpuTopology.LogHeadBytes && head.StartsWith("CPU cores: 8"));
        var detected = CpuTopology.Detect(log);
        Require(detected.Source == CoreRankingSource.DcsLog || Environment.ProcessorCount < 16 || Environment.ProcessorCount > 64, detected.Description);
    }
    Require(BoostRuntime.DcsLogFromOptions(@"C:\Users\x\Saved Games\DCS.openbeta\Config\options.lua") == @"C:\Users\x\Saved Games\DCS.openbeta\Logs\dcs.log");
    Require(BoostRuntime.DcsLogFromOptions(null) is null);
});
Test("DCS-running guard compares canonical paths through junctions, Saved Games and the managed folder", () =>
{
    var install = PathPolicy.UnderRoot(root, "guard/install/DCS World"); Directory.CreateDirectory(Path.Combine(install, "bin"));
    var link = PathPolicy.UnderRoot(root, "guard/linked-dcs");
    using (var mklink = Process.Start(new ProcessStartInfo("cmd.exe") { ArgumentList = { "/c", "mklink", "/J", link, install }, UseShellExecute = false, CreateNoWindow = true, RedirectStandardOutput = true })!)
    { mklink.StandardOutput.ReadToEnd(); mklink.WaitForExit(); }
    var savedGames = PathPolicy.UnderRoot(root, "guard/Saved Games"); var managed = PathPolicy.UnderRoot(root, "guard/state/managed");
    Directory.CreateDirectory(Path.Combine(savedGames, "DCS.openbeta", "Config")); Directory.CreateDirectory(managed);
    var sg = new[] { savedGames };
    Require(Directory.Exists(Path.Combine(link, "bin")), "junction created");
    Require(ControlService.UsedByRunningDcs(Path.Combine(link, "bin", "dxgi.dll"), install, sg, managed), "a file reached through a junction is inside the running install");
    Require(ControlService.UsedByRunningDcs(Path.Combine(install, "bin", "dxgi.dll"), link, sg, managed), "the running install seen through a junction");
    Require(ControlService.UsedByRunningDcs(Path.Combine(savedGames, "DCS.openbeta", "Config", "options.lua"), install, sg, managed));
    Require(ControlService.UsedByRunningDcs(Path.Combine(managed, "cheeky", "dxgi.dll"), install, sg, managed), "managed components are in use while DCS runs");
    Require(!ControlService.UsedByRunningDcs(Path.Combine(savedGames, "Other", "file.txt"), install, sg, managed));
    Require(!ControlService.UsedByRunningDcs(PathPolicy.UnderRoot(root, "guard/fixture/options.lua"), install, sg, managed), "fixture files elsewhere stay allowed");
    Require(!ControlService.UsedByRunningDcs(install + "-other\\bin\\DCS.exe", install, sg, managed), "a sibling with a common prefix is not inside");
    Directory.Delete(link); // removes the junction only
});
Test("Boost helper command passes the DCS executable and log, hidden when elevated", () =>
{
    foreach (var elevated in new[] { false, true })
    {
        var prefix = "boost-start/" + elevated;
        var exe = Make(prefix + "/install/bin/DCS.exe", "Never execute this fixture"); var options = Make(prefix + "/sg/DCS/Config/options.lua", Lua);
        var runtime = Make(prefix + "/runtime.json", "{\"runtime\":{\"library_path\":\"runtime.dll\"}}"); Make(prefix + "/runtime.dll", "Never load this fixture");
        var cli = Make(prefix + "/cli/DcsVr.Cli.exe", "Never execute this fixture");
        var service = new ControlService(root, Path.Combine(root, prefix, "state"));
        var profile = new VrProfile { CpuBoost = true, BoostPrefetch = PrefetchFix.Off, BoostElevated = elevated };
        service.Apply(service.Preview(profile, new() { DcsExecutable = exe, OptionsPath = options, PimaxRuntime = runtime }));
        var start = service.BuildCpuBoostStart(4242, Path.GetDirectoryName(cli))!;
        var args = start.ArgumentList.ToList();
        string? After(string option) { var i = args.IndexOf(option); return i >= 0 && i + 1 < args.Count ? args[i + 1] : null; }
        Require(start.FileName == cli && args[0] == "boost" && After("--dcs-pid") == "4242");
        Require(After("--dcs-exe") == exe, "launched executable passed");
        Require(After("--dcs-log") == Path.GetFullPath(Path.Combine(root, prefix, "sg", "DCS", "Logs", "dcs.log")), "log next to the applied options.lua");
        Require(elevated ? start is { UseShellExecute: true, Verb: "runas", WindowStyle: ProcessWindowStyle.Hidden } : start is { UseShellExecute: false, CreateNoWindow: true });
        Require(BoostArguments.Parse(args.Skip(1).ToArray()) is { Error: null, DcsPid: 4242 } parsed && parsed.DcsExecutable == exe);
        Require(service.RestoreOriginals().Complete);
    }
});

Test("Desktop Tobii pause: only the desktop tracker's services, never the headset's; the helper starts elevated for it", () =>
{
    // Service names and image paths as installed on the test PC (Tobii Experience 4.72, Eye Tracker 5, Pimax Crystal Super).
    Require(TobiiDesktop.IsDesktopService("Tobii Service", @"""C:\Program Files\Tobii\Tobii EyeX\Tobii.Service.exe"" -WaitForEULA=0"));
    Require(TobiiDesktop.IsDesktopService("TobiiIS5LEYETRACKER5", @"C:\WINDOWS\System32\DriverStore\FileRepository\eyetracker5.inf_amd64_a\platform_runtime_IS5LEYETRACKER5_service.exe"));
    Require(!TobiiDesktop.IsDesktopService("Tobii VR4PIMAXP3B Platform Runtime", @"C:\Program Files\Pimax\Runtime\EyeTrackingServer\platform_runtime\platform_runtime_VR4PIMAXP3B_service.exe"), "the headset's runtime is never paused");
    Require(!TobiiDesktop.IsDesktopService("TobiiXR5", @"C:\Somewhere\platform_runtime_XR5EYECHIP_WIN10_x64.exe"));
    Require(!TobiiDesktop.IsDesktopService("TobiiGeneric", @"C:\WINDOWS\System32\DriverStore\FileRepository\tobii_generic.inf_amd64_8\TobiiVirtualDevice.exe"));
    Require(!TobiiDesktop.IsDesktopService("Tobii Service", @"C:\Program Files\Pimax\Tobii.Service.exe") && !TobiiDesktop.IsDesktopService("Other Service", @"C:\x\platform_runtime_IS5.exe"));
    var on = new VrProfile { PauseTobiiDesktop = true };
    Require(on.UsesBoostHelper && on.BoostHelperElevated && !new VrProfile().BoostHelperElevated && new VrProfile { CpuBoost = true, BoostElevated = true }.BoostHelperElevated);
    var prefix = "boost-start/tobii";
    var exe = Make(prefix + "/install/bin/DCS.exe", "Never execute this fixture"); var options = Make(prefix + "/sg/DCS/Config/options.lua", Lua);
    var runtime = Make(prefix + "/runtime.json", "{\"runtime\":{\"library_path\":\"runtime.dll\"}}"); Make(prefix + "/runtime.dll", "Never load this fixture");
    var cli = Make(prefix + "/cli/DcsVr.Cli.exe", "Never execute this fixture");
    var service = new ControlService(root, Path.Combine(root, prefix, "state"));
    service.Apply(service.Preview(on with { QuadViews = QuadProvider.None }, new() { DcsExecutable = exe, OptionsPath = options, PimaxRuntime = runtime }));
    Require(service.BuildCpuBoostStart(4242, Path.GetDirectoryName(cli)) is { UseShellExecute: true, Verb: "runas" }, "Tobii pause alone starts the helper elevated");
    Require(service.RestoreOriginals().Complete);
});

Test("Profiles saved without foveaSource keep their own focus values; new profiles follow Pimax Play", () =>
{
    Require(new VrProfile().FoveaSource == FoveaSource.PimaxPlay && ProfilePresets.All.All(p => p.FoveaSource == FoveaSource.PimaxPlay), "New profiles and presets follow Pimax Play");
    var node = System.Text.Json.Nodes.JsonNode.Parse(JsonData.Serialize(new VrProfile { QuadViews = QuadProvider.QuadViewsFoveated, FoveaWidth = .44 }))!.AsObject();
    Require(JsonData.Deserialize<VrProfile>(node.ToJsonString()).FoveaSource == FoveaSource.PimaxPlay, "A saved source is kept");
    node.Remove("foveaSource");
    var legacy = JsonData.Deserialize<VrProfile>(node.ToJsonString());
    Require(legacy.FoveaSource == FoveaSource.Profile && legacy.FoveaWidth == .44, "A profile saved before the setting keeps its own values");
    Require(JsonData.Deserialize<VrProfile>("{\"schemaVersion\":1,\"id\":\"legacy\",\"name\":\"Legacy\"}").FoveaSource == FoveaSource.Profile);
    Require(JsonData.Deserialize<VrProfile>(JsonData.Serialize(legacy)) == legacy, "Saving it again writes the source");
    // Import, and the applied profile.json the interface shows at startup, read through the same JSON reader.
    var file = Make("legacy-fovea/profile.json", node.ToJsonString());
    Require(new ControlService(root, Path.Combine(root, "legacy-fovea/state")).LoadProfile(file).FoveaSource == FoveaSource.Profile, "Import");
    Require(JsonData.Deserialize<VrProfile>(File.ReadAllText(file)) is { FoveaSource: FoveaSource.Profile, FoveaWidth: .44 }, "Applied profile.json at startup");
    Throws<System.Text.Json.JsonException>(() => JsonData.Deserialize<VrProfile>("{\"foveaSource\":\"PimaxPlay\",\"fakeSetting\":1}"));
});
Test("Default focus values are Pimax Play's Fine values converted", () =>
{
    var p = new VrProfile();
    Require(p.FoveaWidth == .57 && p.FoveaHeight == .34 && p.QuadFocusScale == 1.125 && p.PeripheralScale == .19);
    var converted = PimaxFovea.Convert(new(.66, .20, .66, .66), new(.20, .66, .66, .66), .25, .1919, true);
    Require(Math.Abs(converted.Width - p.FoveaWidth) < 1e-9 && Math.Abs(converted.Height - p.FoveaHeight) < 1e-9 && converted.FocusScale == p.QuadFocusScale && Math.Abs(converted.PeripheralScale - p.PeripheralScale) < .01);
});
Test("Import settles feature combinations exactly like the interface checklist", () =>
{
    var service = new ControlService(root, Path.Combine(root, "import-settle/state"));
    VrProfile Load(string name, VrProfile p) => service.LoadProfile(Make("import-settle/" + name + ".json", JsonData.Serialize(p)));
    Require(Load("sboys-native", new VrProfile { Runtime = RuntimeKind.SboysSteamVr, QuadViews = QuadProvider.PimaxNative }) is { QuadViews: QuadProvider.QuadViewsFoveated, QuadFocusAdapter: false }, "Sboys + Pimax native");
    Require(Load("cheeky-no-adapter", new VrProfile { QuadViews = QuadProvider.QuadViewsFoveated, NeuralRendering = true, QuadFocusAdapter = false }).UsesQuadFocus, "Bundled QV + Cheeky without the adapter");
    Require(Load("qv-foveated", new VrProfile { QuadViews = QuadProvider.QuadViewsFoveated, FoveatedDlss = true, NeuralRendering = true }) is { FoveatedDlss: false, UsesQuadFocus: true }, "Bundled QV + DLSS 5 + Foveated Super Resolution");
    Require(Load("qv-foveated-only", new VrProfile { QuadViews = QuadProvider.QuadViewsFoveated, FoveatedDlss = true, QuadFocusAdapter = true }) is { FoveatedDlss: false, QuadFocusAdapter: false, UsesCheeky: false }, "Bundled QV + Foveated Super Resolution alone is DLSS off");
    Require(Load("native-foveated", new VrProfile { FoveatedDlss = true }) is { QuadViews: QuadProvider.PimaxNative, FoveatedDlss: false, UsesCheeky: false }, "Pimax native + Foveated Super Resolution keeps Pimax native, DLSS off");
    Require(Load("stereo-foveated", new VrProfile { QuadViews = QuadProvider.None, FoveatedDlss = true }) is { FoveatedDlss: true, UsesCheeky: true }, "Stereo keeps Foveated Super Resolution");
    Require(Load("native-dlss", new VrProfile { QuadViews = QuadProvider.PimaxNative, NeuralRendering = true }) is { QuadViews: QuadProvider.QuadViewsFoveated, UsesQuadFocus: true }, "Pimax native + DLSS 5");
    Require(Load("plain-adapter", new VrProfile { QuadViews = QuadProvider.QuadViewsFoveated, QuadFocusAdapter = true }) is { QuadFocusAdapter: false }, "No adapter without Cheeky");
    Require(Load("native", new VrProfile()) is { QuadViews: QuadProvider.PimaxNative }, "Pimax native stays on the Pimax route without DLSS");
    // Messages name controls that exist: the Quad Views provider and the alternative provider folder.
    var messages = string.Join("\n", new[] { new VrProfile { Runtime = RuntimeKind.SboysSteamVr }, new VrProfile { QuadViews = QuadProvider.QuadViewsFoveated, NeuralRendering = true },
        new VrProfile { QuadViews = QuadProvider.QuadViewsFoveated, NeuralRendering = true, QuadFocusAdapter = true, QuadViewsLayerDirectory = "alt" } }.SelectMany(p => ProfileValidation.Validate(p)).Where(i => i.Code.StartsWith("quad-")).Select(i => i.Message));
    Require(messages.Contains("Bundled Quad Views as the Quad Views provider") && messages.Contains("Alternative Quad Views provider folder") && !messages.Contains("enable the focus adapter"), messages);
});
Test("Pimax notes say when the focus resolution or periphery is clamped", () =>
{
    var clamped = PimaxFovea.Read(Make("pimax-clamped/global.json", PimaxJson(1, .66, .20, .66, .66, gaze: 3, periphery: .1)))!;
    Require(clamped.Converted.FocusScale == 2 && clamped.Converted.PeripheralScale == .15);
    var notes = PimaxFovea.Notes(clamped);
    Require(notes.Any(n => n.Contains("clamped to 2×")) && notes.Any(n => n.Contains("clamped to 0.15")) && !notes.Any(n => n.Contains("used as stored")), string.Join("\n", notes));
    var normal = PimaxFovea.Notes(PimaxFovea.Read(Make("pimax-normal/global.json", PimaxJson(1, .66, .20, .66, .66)))!);
    Require(normal.Any(n => n.Contains("used as stored")) && !normal.Any(n => n.Contains("clamped")));
});
Test("Applied profile.json keeps the user's focus values; Pimax Play's converted values go to the provider and pimax-fovea.json", () =>
{
    var source = Path.Combine(workspace, "external/quadviews/bin/x64/Release");
    var quadDir = Path.Combine(root, "fovea-saved/dist/components/quadviews"); Directory.CreateDirectory(quadDir);
    foreach (var name in new[] { "XR_APILAYER_MBUCCHIA_quad_views_foveated.dll", "openxr-api-layer.json", "settings.cfg" })
    { File.Copy(Path.Combine(source, name), Path.Combine(quadDir, name)); File.WriteAllText(Path.Combine(quadDir, name + ".sha256"), Hashing.FileSha256(Path.Combine(quadDir, name))); }
    var exe = Make("fovea-saved/bin/DCS.exe", "fixture"); var options = Make("fovea-saved/options.lua", Lua);
    var runtime = Make("fovea-saved/runtime.json", "{\"runtime\":{\"library_path\":\"runtime.dll\"}}"); Make("fovea-saved/runtime.dll", "fixture");
    var pimax = Make("fovea-saved/global.json", PimaxJson(1, .66, .20, .66, .66));
    var previous = PimaxFovea.SettingsPath; PimaxFovea.SettingsPath = pimax;
    try
    {
        var service = new ControlService(Path.Combine(root, "fovea-saved/dist"), Path.Combine(root, "fovea-saved/state"));
        var draft = new VrProfile { QuadViews = QuadProvider.QuadViewsFoveated, FoveaWidth = .44, FoveaHeight = .41, QuadFocusScale = 1.3, PeripheralScale = .5 };
        var plan = service.Preview(draft, new() { DcsExecutable = exe, OptionsPath = options, PimaxRuntime = runtime });
        string Text(string file) => Encoding.UTF8.GetString(plan.Files.Single(f => Path.GetFileName(f.Path) == file).Content);
        Require(JsonData.Deserialize<VrProfile>(Text("profile.json")) == draft, "profile.json holds the user's own draft");
        Require(Text("settings.cfg").Contains("horizontal_focus_section=0.57") && Text("settings.cfg").Contains("peripheral_multiplier=0.1919") && Text("settings.cfg").Contains("focus_multiplier=1.125"), "The provider gets Pimax's values");
        var written = JsonData.Deserialize<AppliedFovea>(Text(AppliedFovea.FileName));
        Require(written.Stamp == PimaxFovea.Read(pimax)!.Stamp && Math.Abs(written.FoveaWidth - .57) < 1e-9 && Math.Abs(written.FoveaHeight - .34) < 1e-9);
        var journal = service.Apply(plan).Current;
        var saved = journal.Entries.Single(e => Path.GetFileName(e.Path) == "profile.json").Path;
        VrProfile Applied() => JsonData.Deserialize<VrProfile>(File.ReadAllText(saved));
        Require(Applied() == draft && PimaxFovea.ChangedSinceApply(Applied(), PimaxFovea.ReadApplied(saved)) is null, "Unchanged Pimax values are not reported");
        File.WriteAllText(pimax, PimaxJson(0, .33, .33, .33, .33));
        Require(PimaxFovea.ChangedSinceApply(Applied(), PimaxFovea.ReadApplied(saved)) is { } changed && changed.Contains("Pimax Play now: Quick 33% × 33% · 125% · 20%"), "Launch warns after a Pimax change");
        Require(service.RestoreOriginals().Complete && File.ReadAllText(options) == Lua && !File.Exists(saved) && !File.Exists(Path.Combine(Path.GetDirectoryName(saved)!, AppliedFovea.FileName)));
        // A profile that uses its own values writes no pimax-fovea.json.
        Require(service.Preview(draft with { FoveaSource = FoveaSource.Profile }, new() { DcsExecutable = exe, OptionsPath = options, PimaxRuntime = runtime }).Files.All(f => Path.GetFileName(f.Path) != AppliedFovea.FileName));
    }
    finally { PimaxFovea.SettingsPath = previous; }
});
// ---- Launch DCS: one click brings the installed profile in line with the draft, then launches ---------------------
(ControlService Service, InventorySnapshot Inventory, string Options, string Runtime) SyncFixture(string name)
{
    var prefix = "launch-sync/" + name;
    var exe = Make(prefix + "/bin/DCS.exe", "Never execute this fixture"); var options = Make(prefix + "/options.lua", Lua);
    Make(prefix + "/runtime.dll", "Never load this fixture"); var runtime = Make(prefix + "/runtime.json", "{\"runtime\":{\"library_path\":\"runtime.dll\"}}");
    return (new ControlService(root, Path.Combine(root, prefix, "state")), new() { DcsExecutable = exe, OptionsPath = options, PimaxRuntime = runtime }, options, runtime);
}
Test("Launch applies a draft when nothing is applied, then changes nothing while the draft is unchanged", () =>
{
    var f = SyncFixture("first");
    var draft = new VrProfile { FpsLimit = FpsLimitMode.Custom, RenderedFpsCap = 60 };
    var (first, start) = f.Service.SyncAndPrepareLaunch(draft, f.Inventory);
    Require(first.Kind == LaunchSyncKind.Applied && first.ReplacedProfileId is null && start.FileName == f.Inventory.DcsExecutable, "Applied with a backup, then the launch contract checks out");
    Require(new LuaOptions(File.ReadAllText(f.Options)).Get("graphics", "maxFPS") == "60");
    var firstAt = f.Service.Originals.Status().LastActionAt;
    var again = f.Service.SyncApplied(draft with { Name = "Renamed only" }, f.Inventory);
    Require(again.Kind == LaunchSyncKind.Unchanged && again.Journal.Id == first.Journal.Id && f.Service.Originals.Status().LastActionAt == firstAt, "Unchanged draft: nothing written, no new backup");
    Require(f.Service.SyncApplied(null, f.Inventory) is { Kind: LaunchSyncKind.Unchanged }, "No draft (CLI launch) uses the applied profile");
    Require(f.Service.RestoreOriginals().Complete && File.ReadAllText(f.Options) == Lua);
    Throws<InvalidOperationException>(() => f.Service.SyncApplied(null, f.Inventory)); // the CLI has nothing to launch
});
Test("Launch after a change writes the draft over the applied profile and checks the launch contract", () =>
{
    var f = SyncFixture("changed");
    var before = f.Service.SyncApplied(new VrProfile { FpsLimit = FpsLimitMode.Custom, RenderedFpsCap = 60 }, f.Inventory);
    var originals = f.Service.Originals.Status();
    var (after, start) = f.Service.SyncAndPrepareLaunch(new VrProfile { FpsLimit = FpsLimitMode.Custom, RenderedFpsCap = 72 }, f.Inventory);
    Require(after.Kind == LaunchSyncKind.Applied && after.ReplacedProfileId == before.Journal.ProfileId && after.Journal.Id != before.Journal.Id && start.FileName == f.Inventory.DcsExecutable && after.Message.Contains(" over "), after.Message);
    Require(f.Service.Originals.Status() is { State: "applied" } now && now.Count == originals.Count && now.Current!.Id == after.Journal.Id, "Same originals, the new profile is the applied one");
    // The original options.lua is still the one from before the first apply.
    Require(new LuaOptions(File.ReadAllText(f.Options)).Get("graphics", "maxFPS") == "72" && f.Service.RestoreOriginals().Complete && File.ReadAllText(f.Options) == Lua);
});
Test("Launch never stops on edited, leftover or foreign files: it writes over them; an invalid draft writes nothing", () =>
{
    var f = SyncFixture("overwrite");
    var appliedDraft = new VrProfile { FpsLimit = FpsLimitMode.Custom, RenderedFpsCap = 60 };
    var applied = f.Service.SyncApplied(appliedDraft, f.Inventory);
    // Invalid draft: checked like Preview before anything is written.
    var before = applied.Journal.Entries.ToDictionary(e => e.Path, e => Hashing.FileSha256(e.Path));
    Throws<InvalidDataException>(() => f.Service.SyncApplied(new VrProfile { NeuralIntensity = 2 }, f.Inventory));
    Require(before.All(p => Hashing.FileSha256(p.Key) == p.Value) && f.Service.PrepareLaunchApplied().FileName == f.Inventory.DcsExecutable, "The applied profile is untouched");
    // A file the applied profile created was edited, another deleted: the next launch simply writes the profile again.
    var launch = applied.Journal.Entries.Single(e => Path.GetFileName(e.Path) == "launch.json").Path; AtomicFile.WriteText(launch, File.ReadAllText(launch) + " ");
    var profile = applied.Journal.Entries.Single(e => Path.GetFileName(e.Path) == "profile.json").Path;
    var changed = f.Service.SyncApplied(appliedDraft with { RenderedFpsCap = 50 }, f.Inventory);
    Require(changed.Kind == LaunchSyncKind.Applied && f.Service.PrepareLaunchApplied().FileName == f.Inventory.DcsExecutable, changed.Message);
    File.Delete(profile);
    var again = f.Service.SyncApplied(appliedDraft, f.Inventory);
    Require(again.Kind == LaunchSyncKind.Applied && File.Exists(profile) && f.Service.PrepareLaunchApplied().FileName == f.Inventory.DcsExecutable, again.Message);
    Require(applied.Journal.Entries.Count >= 3 && f.Service.RestoreOriginals().Complete && File.ReadAllText(f.Options) == Lua && !File.Exists(launch));
});
Test("Launch sets the profile's own DCS settings back in place when DCS rewrote them, keeping DCS's other changes", () =>
{
    var f = SyncFixture("lua-drift");
    var draft = new VrProfile { FpsLimit = FpsLimitMode.Custom, RenderedFpsCap = 60 };
    var applied = f.Service.SyncApplied(draft, f.Inventory);
    var others = applied.Journal.Entries.Where(e => e.LuaChanges is null).ToDictionary(e => e.Path, e => Hashing.FileSha256(e.Path));
    // DCS writes options.lua back on exit: Max FPS changed in its settings, and a comment of its own.
    string DcsRewrite(string maxFps) { var text = new LuaOptions(File.ReadAllText(f.Options)).Set(["graphics", "maxFPS"], int.Parse(maxFps)); return text.Replace("-- preserve comment", "-- written by DCS"); }
    AtomicFile.WriteText(f.Options, DcsRewrite("120"));
    Throws<IOException>(() => f.Service.PrepareLaunchApplied());
    var (sync, start) = f.Service.SyncAndPrepareLaunch(draft, f.Inventory);
    Require(sync.Kind == LaunchSyncKind.SettingsRealigned && sync.Message.Contains("graphics.maxFPS") && sync.Journal.Id == applied.Journal.Id && start.FileName == f.Inventory.DcsExecutable, sync.Message);
    Require(new LuaOptions(File.ReadAllText(f.Options)).Get("graphics", "maxFPS") == "60" && File.ReadAllText(f.Options).Contains("-- written by DCS"), "Only the owned key was set back");
    Require(f.Service.Originals.ReadCurrent()!.Id == applied.Journal.Id && others.All(p => Hashing.FileSha256(p.Key) == p.Value), "Same applied profile, no other file written");
    Require(f.Service.SyncApplied(draft, f.Inventory).Kind == LaunchSyncKind.Unchanged, "Up to date afterwards");
    // A changed draft on top of DCS's rewrite: written over it, keeping DCS's own change.
    AtomicFile.WriteText(f.Options, DcsRewrite("144"));
    var replaced = f.Service.SyncApplied(draft with { RenderedFpsCap = 72 }, f.Inventory);
    Require(replaced.Kind == LaunchSyncKind.Applied && replaced.ReplacedProfileId == applied.Journal.ProfileId && replaced.Journal.Id != applied.Journal.Id, replaced.Message);
    Require(new LuaOptions(File.ReadAllText(f.Options)).Get("graphics", "maxFPS") == "72" && File.ReadAllText(f.Options).Contains("-- written by DCS"));
    // Back to stock DCS merges the owned keys back and keeps DCS's own change.
    Require(f.Service.RestoreOriginals().Complete && new LuaOptions(File.ReadAllText(f.Options)).Get("graphics", "maxFPS") == "89" && File.ReadAllText(f.Options).Contains("-- written by DCS"));
});
Test("Launch applies the same profile again when an external dependency changed since Apply", () =>
{
    var f = SyncFixture("dependency");
    var first = f.Service.SyncApplied(new VrProfile(), f.Inventory);
    AtomicFile.WriteText(f.Runtime, "{\"runtime\":{\"library_path\":\"runtime.dll\"},\"updated\":true}");
    Throws<IOException>(() => f.Service.PrepareLaunchApplied());
    var (again, start) = f.Service.SyncAndPrepareLaunch(new VrProfile(), f.Inventory);
    Require(again.Kind == LaunchSyncKind.Applied && again.Message.Contains("again") && again.Journal.Id != first.Journal.Id && start.FileName == f.Inventory.DcsExecutable, again.Message);
    Require(f.Service.RestoreOriginals().Complete && File.ReadAllText(f.Options) == Lua);
});
Test("Launch updates only Pimax Play's focus values in place, and Back to stock DCS stays clean", () =>
{
    var source = Path.Combine(workspace, "external/quadviews/bin/x64/Release");
    var quadDir = Path.Combine(root, "launch-sync/pimax/dist/components/quadviews"); Directory.CreateDirectory(quadDir);
    foreach (var name in new[] { "XR_APILAYER_MBUCCHIA_quad_views_foveated.dll", "openxr-api-layer.json", "settings.cfg" })
    { File.Copy(Path.Combine(source, name), Path.Combine(quadDir, name)); File.WriteAllText(Path.Combine(quadDir, name + ".sha256"), Hashing.FileSha256(Path.Combine(quadDir, name))); }
    var exe = Make("launch-sync/pimax/bin/DCS.exe", "Never execute this fixture"); var options = Make("launch-sync/pimax/options.lua", Lua);
    var runtime = Make("launch-sync/pimax/runtime.json", "{\"runtime\":{\"library_path\":\"runtime.dll\"}}"); Make("launch-sync/pimax/runtime.dll", "Never load this fixture");
    var pimax = Make("launch-sync/pimax/global.json", PimaxJson(1, .66, .20, .66, .66));
    var previous = PimaxFovea.SettingsPath; PimaxFovea.SettingsPath = pimax;
    try
    {
        var service = new ControlService(Path.Combine(root, "launch-sync/pimax/dist"), Path.Combine(root, "launch-sync/pimax/state"));
        var inventory = new InventorySnapshot { DcsExecutable = exe, OptionsPath = options, PimaxRuntime = runtime };
        var draft = new VrProfile { QuadViews = QuadProvider.QuadViewsFoveated, FoveaWidth = .44, FoveaHeight = .41 };
        var applied = service.SyncApplied(draft, inventory); var countBefore = service.Originals.Status().Count;
        string Entry(string file) => applied.Journal.Entries.Single(e => Path.GetFileName(e.Path) == file).Path;
        var settings = Entry("settings.cfg"); var fovea = Entry(AppliedFovea.FileName); var launchJson = File.ReadAllText(Entry("launch.json"));
        Require(File.ReadAllText(settings).Contains("horizontal_focus_section=0.57"), "Applied with Pimax Fine 33/10");
        Require(service.SyncApplied(draft, inventory).Kind == LaunchSyncKind.Unchanged, "Unchanged Pimax values: nothing written");
        File.WriteAllText(pimax, PimaxJson(0, .33, .33, .33, .33));
        var (updated, start) = service.SyncAndPrepareLaunch(draft, inventory);
        Require(updated.Kind == LaunchSyncKind.FoveaUpdated && updated.Message == "Pimax Play changed: focus updated to Quick 33% × 33% · 125% · 20%." && start.FileName == exe, updated.Message);
        Require(updated.Journal.Id == applied.Journal.Id && service.Originals.Status().Count == countBefore, "Same applied profile, no new backup");
        Require(File.ReadAllText(settings).Contains("horizontal_focus_section=0.67") && File.ReadAllText(settings).Contains("vertical_focus_section=0.67"), "settings.cfg holds Quick 33%");
        Require(PimaxFovea.ReadApplied(Entry("profile.json")) is { } written && Math.Abs(written.FoveaWidth - .67) < 1e-9 && written.Stamp == PimaxFovea.Read(pimax)!.Stamp, "pimax-fovea.json updated");
        Require(File.ReadAllText(Entry("launch.json")) == launchJson && JsonData.Deserialize<VrProfile>(File.ReadAllText(Entry("profile.json"))) == draft, "Nothing else changed");
        var journal = service.Originals.ReadCurrent()!;
        Require(journal.Entries.Where(e => e.Path == settings || e.Path == fovea).All(e => e.InstalledSha256 == Hashing.FileSha256(e.Path)), "The applied profile records the new installed hashes");
        Require(PimaxFovea.ChangedSinceApply(draft, PimaxFovea.ReadApplied(Entry("profile.json"))) is null && service.SyncApplied(null, inventory).Kind == LaunchSyncKind.Unchanged, "Up to date afterwards");
        // An edited settings.cfg is not updated in place: the profile is written again in full, over the edit.
        File.WriteAllText(pimax, PimaxJson(1, .66, .20, .66, .66)); AtomicFile.WriteText(settings, File.ReadAllText(settings) + "# user\n");
        Require(service.SyncApplied(draft, inventory).Kind == LaunchSyncKind.Applied && !File.ReadAllText(settings).EndsWith("# user\n") && File.ReadAllText(settings).Contains("horizontal_focus_section=0.57"), "Written again");
        Require(service.RestoreOriginals().Complete && File.ReadAllText(options) == Lua && !File.Exists(settings) && !File.Exists(fovea), "Back to stock DCS after in-place updates completes");
    }
    finally { PimaxFovea.SettingsPath = previous; }
});
// ---- Saved DLSS 5 runtime copy ------------------------------------------------------------------------------------
// The user-supplied NVIDIA runtime is never in the repository: tests that need a genuinely signed file use
// DCSVR_TEST_NEURAL_PATH (as the release and WebView checks do) or the usual download folder, and skip without it.
string? RealNeuralRuntime()
{
    foreach (var candidate in new[] { Environment.GetEnvironmentVariable("DCSVR_TEST_NEURAL_PATH"), Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.UserProfile), "Downloads", "DLSSNR Package", "nvngx_dlssnr.dll") })
        if (!string.IsNullOrWhiteSpace(candidate) && File.Exists(candidate)) return Path.GetFullPath(candidate);
    return null;
}
/// <summary>A stereo DLSS 5 distribution (Cheeky components with their hash files) and DCS fixture for ControlService.Preview.</summary>
(ControlService Service, InventorySnapshot Inventory, string Options) NeuralFixture(string name)
{
    var prefix = "saved-runtime/" + name;
    var distribution = Path.Combine(root, prefix, "dist");
    foreach (var file in new[] { "components/cheeky/dxgi.dll", "components/cheeky/CheekyFoveatedDLSS/CheekyFoveatedDLSSHost.dll", "components/cheeky/CheekyFoveatedDLSS/CheekyFoveatedDLSSRuntime.dll", "components/CheekyOpenXRLayer.dll" })
    {
        var path = Make(prefix + "/dist/" + file, "Never load this fixture: " + file);
        AtomicFile.WriteText(path + ".sha256", Hashing.FileSha256(path));
    }
    var exe = Make(prefix + "/bin/DCS.exe", "Never execute this fixture"); var options = Make(prefix + "/options.lua", Lua);
    Make(prefix + "/runtime.dll", "Never load this fixture"); var runtime = Make(prefix + "/runtime.json", "{\"runtime\":{\"library_path\":\"runtime.dll\"}}");
    return (new ControlService(distribution, Path.Combine(root, prefix, "state")), new() { DcsExecutable = exe, OptionsPath = options, PimaxRuntime = runtime }, options);
}
var stereoNeural = new VrProfile { Id = "stereo-dlss5", QuadViews = QuadProvider.None, FrameGen = FrameGeneration.Off, NeuralRendering = true, NeuralRuntimePath = null };
Test("Saved runtime: an invalid or wrongly named file is refused and nothing is saved", () =>
{
    var service = new ControlService(root, Path.Combine(root, "saved-runtime/invalid/state"));
    var junk = Make("saved-runtime/invalid/source/nvngx_dlssnr.dll", "MZ not a real runtime");
    Throws<InvalidDataException>(() => service.RememberNeuralRuntime(junk));
    // Signed x64 Windows code that is not NVIDIA's runtime.
    var signed = PathPolicy.UnderRoot(root, "saved-runtime/invalid/signed/nvngx_dlssnr.dll"); Directory.CreateDirectory(Path.GetDirectoryName(signed)!);
    File.Copy(Path.Combine(Environment.SystemDirectory, "ntdll.dll"), signed);
    Throws<InvalidDataException>(() => service.RememberNeuralRuntime(signed));
    Throws<InvalidDataException>(() => service.RememberNeuralRuntime(Make("saved-runtime/invalid/source/other.dll", "MZ")));
    Require(service.SavedRuntime.Read() is null && !File.Exists(service.SavedRuntime.DllPath), "Nothing saved");
    Require(service.ResolveNeuralRuntime(stereoNeural).NeuralRuntimePath is null, "Without a saved copy the path stays empty");
    Require(ProfileValidation.Validate(service.ResolveNeuralRuntime(stereoNeural)).Any(i => i.Code == "neural-runtime" && i.Severity == IssueSeverity.Error), "The file is still asked for");
    Require(!service.ForgetNeuralRuntime(), "Forget without a copy is harmless");
});
Test("Saved runtime: a selected signed runtime is saved once and used by drafts without their own file", () =>
{
    if (RealNeuralRuntime() is not { } real) { Console.WriteLine("SKIP saved runtime selection: no signed nvngx_dlssnr.dll (set DCSVR_TEST_NEURAL_PATH)"); return; }
    var service = new ControlService(root, Path.Combine(root, "saved-runtime/select/state"));
    var saved = service.RememberNeuralRuntime(real);
    Require(saved.Path == Path.Combine(service.StateRoot, "runtimes", "nvngx_dlssnr.dll") && saved.OriginalPath == real && NativeBinary.SupportsNeuralContract(saved.Version), "Recorded");
    Require(Hashing.FileSha256(saved.Path) == saved.Sha256 && saved.Sha256 == Hashing.FileSha256(real) && saved.Bytes == new FileInfo(real).Length, "Exact bytes");
    var record = JsonData.Deserialize<SavedNeuralRuntime>(File.ReadAllText(service.SavedRuntime.InfoPath));
    Require(record.Sha256 == saved.Sha256 && record.Version == saved.Version && record.OriginalPath == real && record.SavedAt == saved.SavedAt && !record.Forgotten, "JSON next to the copy");
    var written = File.GetLastWriteTimeUtc(saved.Path);
    Require(service.RememberNeuralRuntime(real).SavedAt == saved.SavedAt && File.GetLastWriteTimeUtc(saved.Path) == written, "The same file again changes nothing");
    Require(Directory.GetFiles(Path.Combine(service.StateRoot, "runtimes")).Length == 2, "Only the copy and its record, no leftovers");
    // A draft without a runtime path, or with one that no longer exists, uses the saved copy; its own file wins.
    Require(service.ResolveNeuralRuntime(stereoNeural).NeuralRuntimePath == saved.Path);
    Require(service.ResolveNeuralRuntime(stereoNeural with { NeuralRuntimePath = Path.Combine(root, "moved-away/nvngx_dlssnr.dll") }).NeuralRuntimePath == saved.Path, "A missing original falls back to the copy");
    Require(service.ResolveNeuralRuntime(stereoNeural with { NeuralRuntimePath = real }).NeuralRuntimePath == real, "An explicit file is kept");
    Require(service.ResolveNeuralRuntime(stereoNeural with { NeuralRendering = false }).NeuralRuntimePath is null, "Untouched without DLSS 5");
    Require(!ProfileValidation.Validate(service.ResolveNeuralRuntime(stereoNeural)).Any(i => i.Code == "neural-runtime"), "Validation accepts an empty path with a saved copy");
    Require(service.ForgetNeuralRuntime() == false && service.SavedRuntime.Read() is null && !File.Exists(saved.Path), "Forget deletes the copy when nothing applied uses it");
    Require(service.ResolveNeuralRuntime(stereoNeural).NeuralRuntimePath is null, "Forgotten copy no longer used");
});
Test("Saved runtime: NVIDIA's runtime changed by a compatibility patch is accepted when selected, never by Detect", () =>
{
    if (RealNeuralRuntime() is not { } real) { Console.WriteLine("SKIP patched runtime: no signed nvngx_dlssnr.dll (set DCSVR_TEST_NEURAL_PATH)"); return; }
    // The RTX 20/30/40 patches rewrite GPU code inside NVIDIA's file: version and certificate stay, the signature breaks.
    var patched = PathPolicy.UnderRoot(root, "saved-runtime/patched/source/nvngx_dlssnr.dll"); Directory.CreateDirectory(Path.GetDirectoryName(patched)!);
    var bytes = File.ReadAllBytes(real); bytes[bytes.Length / 2] ^= 0xFF; File.WriteAllBytes(patched, bytes);
    var inspection = NativeBinary.Inspect(patched);
    Require(!inspection.TrustedSignature && NativeBinary.SupportsNeuralContract(inspection.Version) && NativeBinary.IsModifiedRuntime(inspection), "Fixture: signature broken, version kept");
    Throws<InvalidDataException>(() => NativeBinary.VerifyNeuralRuntime(bytes)); // The strict check refuses it.
    Require(NativeBinary.VerifyNeuralRuntime(bytes, allowModified: true).Sha256 == Hashing.BytesSha256(bytes), "Accepted for a file the user selected");
    Require(!NativeBinary.IsModifiedRuntime(NativeBinary.Inspect(real)), "The signed original is not modified");
    var (service, inventory, _) = NeuralFixture("patched");
    var saved = service.RememberNeuralRuntime(patched);
    Require(saved.Modified && saved.Sha256 == Hashing.BytesSha256(bytes) && NativeBinary.SupportsNeuralContract(saved.Version), "Saved and recorded as modified");
    Require(JsonData.Deserialize<SavedNeuralRuntime>(File.ReadAllText(service.SavedRuntime.InfoPath)).Modified, "Recorded in the JSON");
    var deployed = service.Preview(stereoNeural, inventory).Files.Single(f => f.Path.EndsWith(@"CheekyFoveatedDLSS\nvngx_dlssnr.dll", StringComparison.OrdinalIgnoreCase));
    Require(Hashing.BytesSha256(deployed.Content) == saved.Sha256, "Deployed from the saved copy");
    Require(service.Preview(stereoNeural with { NeuralRuntimePath = patched }, inventory).Files.Any(f => f.Path.EndsWith("nvngx_dlssnr.dll")), "Also as the profile's own file");
    Require(!service.RememberNeuralRuntime(real).Modified, "Selecting the signed file again replaces it, not modified");
    // Detect only ever picks validly signed files, even when a patched one sits in the folder it searches.
    var downloads = PathPolicy.UnderRoot(root, "saved-runtime/patched-detect/downloads"); Directory.CreateDirectory(Path.Combine(downloads, "DLSSNR"));
    File.Copy(patched, Path.Combine(downloads, "DLSSNR", "nvngx_dlssnr.dll"));
    var store = new NeuralRuntimeStore(Path.Combine(root, "saved-runtime/patched-detect/state/runtimes"));
    var sources = new DetectionSources { SteamVrServerLog = Path.Combine(root, "missing.txt"), PimaxPlaySettings = Path.Combine(root, "missing.json"), NeuralRuntimeFolders = [downloads], ProcessRunning = _ => false, SavedRuntimes = store };
    SetupDetection.Detect(stereoNeural, new(), sources);
    Require(store.Current() is null, "Detect ignored the patched file");
});
Test("Saved runtime: deployment copies the hash-checked saved copy; Forget while applied keeps it until restore", () =>
{
    if (RealNeuralRuntime() is not { } real) { Console.WriteLine("SKIP saved runtime deployment: no signed nvngx_dlssnr.dll (set DCSVR_TEST_NEURAL_PATH)"); return; }
    var (service, inventory, options) = NeuralFixture("deploy");
    var original = File.ReadAllText(options);
    Throws<InvalidDataException>(() => service.Preview(stereoNeural, inventory)); // Nothing selected yet.
    var saved = service.RememberNeuralRuntime(real);
    var plan = service.Preview(stereoNeural, inventory);
    var deployed = plan.Files.Single(f => f.Path.EndsWith(@"CheekyFoveatedDLSS\nvngx_dlssnr.dll", StringComparison.OrdinalIgnoreCase));
    Require(Hashing.BytesSha256(deployed.Content) == saved.Sha256, "Deployed from the saved copy");
    var profileJson = JsonData.Deserialize<VrProfile>(Encoding.UTF8.GetString(plan.Files.Single(f => f.Path.EndsWith("profile.json")).Content));
    Require(profileJson.NeuralRuntimePath is null, "profile.json keeps the empty path meaning the saved copy");
    var readiness = Readiness.Check(stereoNeural, inventory, service);
    Require(readiness.Checks.Single(c => c.Id == "deployment").State == CheckState.Pass && !readiness.Checks.Any(c => c.Id == "neural-runtime"), "Readiness uses the saved copy");
    service.Apply(plan);
    Require(service.AppliedUsesSavedRuntime());
    Require(service.ForgetNeuralRuntime(), "Kept while the applied profile was installed from it");
    Require(File.Exists(saved.Path) && service.SavedRuntime.Read()!.Forgotten && service.SavedRuntime.Current() is null, "Kept, but not used for new drafts");
    Require(service.ResolveNeuralRuntime(stereoNeural).NeuralRuntimePath is null);
    Require(service.RestoreOriginals().Complete && File.ReadAllText(options) == original);
    Require(!File.Exists(saved.Path) && service.SavedRuntime.Read() is null, "Deleted once that profile is restored");
    // A saved copy that changed after it was saved is refused, even with the same length.
    saved = service.RememberNeuralRuntime(real);
    using (var stream = new FileStream(saved.Path, FileMode.Open, FileAccess.ReadWrite)) { stream.Position = stream.Length - 1; var last = stream.ReadByte(); stream.Position = stream.Length - 1; stream.WriteByte((byte)(last ^ 0xFF)); }
    try { service.Preview(stereoNeural, inventory); throw new Exception("A changed saved copy was deployed"); }
    catch (InvalidDataException e) { Require(e.Message.Contains("changed since it was saved"), e.Message); }
    Require(service.RememberNeuralRuntime(real).Sha256 == Hashing.FileSha256(saved.Path), "Selecting the file again repairs the copy");
    Require(service.Preview(stereoNeural, inventory).Files.Any(f => f.Path.EndsWith("nvngx_dlssnr.dll")));
});
Test("Saved runtime: Detect saves a found runtime and uses the copy for a profile whose file moved", () =>
{
    if (RealNeuralRuntime() is not { } real) { Console.WriteLine("SKIP saved runtime detection: no signed nvngx_dlssnr.dll (set DCSVR_TEST_NEURAL_PATH)"); return; }
    var folder = PathPolicy.UnderRoot(root, "saved-runtime/detect/downloads/DLSSNR"); Directory.CreateDirectory(folder);
    var found = Path.Combine(folder, "nvngx_dlssnr.dll"); File.Copy(real, found);
    var store = new NeuralRuntimeStore(Path.Combine(root, "saved-runtime/detect/state/runtimes"));
    var sources = new DetectionSources { SteamVrServerLog = Path.Combine(root, "missing.txt"), PimaxPlaySettings = Path.Combine(root, "missing.json"), NeuralRuntimeFolders = [Path.GetDirectoryName(folder)!], ProcessRunning = _ => false, SavedRuntimes = store };
    var first = SetupDetection.Detect(stereoNeural, new(), sources);
    Require(store.Current() is { } copy && copy.OriginalPath == found && first.Profile.NeuralRuntimePath is null, "Found file saved, profile uses the copy");
    Require(first.Detected.Single(d => d.Field == "neuralRuntimePath").Value.StartsWith("Saved copy · version 310.8"));
    File.Delete(found);
    var moved = SetupDetection.Detect(stereoNeural with { NeuralRuntimePath = found }, new(), sources with { NeuralRuntimeFolders = [] });
    Require(moved.Profile.NeuralRuntimePath is null && moved.Detected.Any(d => d.Field == "neuralRuntimePath"), "A profile whose file is gone switches to the copy");
    var again = SetupDetection.Detect(stereoNeural, new(), sources with { NeuralRuntimeFolders = [] });
    Require(again.Profile.NeuralRuntimePath is null && again.Detected.All(d => d.Field != "neuralRuntimePath") && again.Notes.Any(n => n.Contains("saved copy")), "Nothing to change when the copy is already used");
});
Test("Readiness text uses one number format on any Windows language", () =>
{
    var previous = System.Globalization.CultureInfo.CurrentCulture;
    System.Globalization.CultureInfo.CurrentCulture = new System.Globalization.CultureInfo("it-IT");
    try
    {
        var p = new VrProfile { QuadViews = QuadProvider.QuadViewsFoveated, FrameGen = FrameGeneration.Nvidia, NeuralRendering = true, FpsLimit = FpsLimitMode.Custom, RenderedFpsCap = 47.5,
            HeadsetRefreshHz = 90.5, QuadFocusScale = 1.25 };
        var details = string.Join("\n", FramePacing.Checks(p, new InventorySnapshot()).Select(c => c.Detail));
        Require(details.Contains("47.5 FPS") && details.Contains("90.5 Hz") && !details.Contains("47,5") && !details.Contains("90,5"), details);
    }
    finally { System.Globalization.CultureInfo.CurrentCulture = previous; }
});

// --- Flight helpers: Free VRAM before flight, Small DCS window in VR, Lower the monitor while flying (no real processes or displays) ---
BoostSnapshot VramFixture() => new(new List<BoostProcessFact>
{
    new(100, "DCS", 50, true), new(50, "steam", 1, true), new(1, "explorer", 0, true),
    new(10, "NVIDIA Overlay", 9, true), new(11, "NVIDIA Overlay", 10, true), new(9, "nvcontainer", 1, false),
    new(20, "msedge", 1, true), new(21, "msedge", 20, true), new(22, "msedgewebview2", 1, true),
    new(30, "HueSync", 1, true), new(40, "RazerCortex", 41, false), new(60, "dwm", 1, false), new(70, "audiodg", 1, false),
}, DcsPid: 100);
Test("Free VRAM closes only listed programs, never DCS, its ancestors, launchers or protected processes", () =>
{
    var snapshot = VramFixture();
    var excluded = new HashSet<int>(BoostPlanner.AncestorPids(snapshot)) { 100 };
    var pids = FreeVram.Targets(snapshot, new VrProfile().FreeVramApps, excluded).Select(t => t.Pid).Order().ToArray();
    Require(pids.SequenceEqual([10, 11, 20, 21, 30, 40]), string.Join(",", pids)); // msedgewebview2 is not msedge
    var hostile = FreeVram.Targets(snapshot, ["D*", "s*", "*team", "dwm", "audio*", "explorer", "nv*"], excluded).Select(t => t.Name).ToArray();
    Require(hostile.SequenceEqual(["NVIDIA Overlay", "NVIDIA Overlay", "nvcontainer"]), "only unprotected processes match: " + string.Join(",", hostile));
    var steamParent = snapshot with { Processes = [.. snapshot.Processes.Select(p => p.Pid == 100 ? p with { ParentPid = 20 } : p)] };
    var ancestors = new HashSet<int>(BoostPlanner.AncestorPids(steamParent)) { 100 };
    Require(!FreeVram.Targets(steamParent, ["msedge"], ancestors).Any(t => t.Pid == 20), "a listed program that started DCS is never closed");
    Require(new VrProfile().FreeVramApps.SequenceEqual(VrProfile.DefaultFreeVramApps) && !VrProfile.DefaultFreeVramApps.Any(n => n.Contains("Discord", StringComparison.OrdinalIgnoreCase) || n.Contains("obs", StringComparison.OrdinalIgnoreCase)));
    var p = new VrProfile { FreeVram = true };
    Require(!ProfileValidation.Validate(p).Any(i => i.Severity == IssueSeverity.Error), "the default list is valid");
    foreach (var bad in new[] { "DCS", "steam", "explorer", "D*", "dwm" })
        Require(ProfileValidation.Validate(p with { FreeVramApps = [bad] }).Any(i => i.Code == "boost-launcher" && i.Severity == IssueSeverity.Error), bad);
    Require(ProfileValidation.Validate(p with { FreeVramApps = ["*"] }).Any(i => i.Code == "boost-app-wildcard"));
    Require(!ProfileValidation.Validate(new VrProfile { FreeVramApps = ["steam"] }).Any(i => i.Code.StartsWith("boost")), "the list is checked only when the feature is on");
    Require(new VrProfile { FreeVram = true }.UsesBoostHelper && new VrProfile { LowerMonitor = true }.UsesBoostHelper && !new VrProfile { SmallDcsWindow = true }.UsesBoostHelper && !new VrProfile().UsesBoostHelper);
});
Test("What Boost will do shows each program's approximate VRAM and the total it would free", () =>
{
    var memory = new Dictionary<int, long> { [10] = 1200L << 20, [11] = 180L << 20, [20] = 120L << 20, [30] = 250L << 20, [100] = 9000L << 20, [60] = 4000L << 20 };
    var plan = BoostPlanner.Compose(new VrProfile(), boostTopo, VramFixture(), memory, new FakeDisplays());
    var overlay = plan.FreeVram!.Single(v => v.Name == "NVIDIA Overlay");
    Require(overlay is { Processes: 2, DedicatedBytes: 1380L << 20, NeedsAdministrator: false, Skipped: false });
    Require(plan.FreeVram!.Single(v => v.Name == "RazerCortex") is { Processes: 1, NeedsAdministrator: true, DedicatedBytes: 0 });
    Require(plan.FreeVram!.Single(v => v.Name == "ChatGPT") is { Processes: 0 });
    Require(plan.FreeVramBytes == (1380L + 120 + 250) << 20, "DCS and dwm are never counted: " + plan.FreeVramBytes);
    Require(BoostPlanner.Compose(new VrProfile(), boostTopo, VramFixture()).FreeVramBytes is null, "no counters, no total");
    Require(plan.Monitor is { Device: @"\\.\DISPLAY1", Offered: true } && plan.Monitor.Current == new DisplayMode(3840, 2160, 144) && plan.Monitor.Flight == new DisplayMode(1920, 1080, 60));
    Require(plan.SmallWindow!.Contains("graphics.width = 1280") && !plan.Processes.Any(p => p.Category == "Close"));
    var odd = BoostPlanner.Compose(new VrProfile { FlightDisplayWidth = 1366, FlightDisplayHeight = 768, FlightDisplayRefresh = 59 }, boostTopo, VramFixture(), memory, new FakeDisplays());
    Require(odd.Monitor is { Offered: false } && odd.Monitor.Note.Contains("does not report"));
});
Test("Free VRAM reopens each program once, as it was started, and never its helpers or what is running again", () =>
{
    const string edge = @"C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe", overlay = @"C:\Program Files\NVIDIA Corporation\NVIDIA App\CEF\NVIDIA Overlay.exe";
    const string razer = @"C:\Program Files\Razer\RazerAppEngine\app-4.0.827\RazerAppEngine.exe", hue = @"C:\Program Files\Hue Sync\HueSync.exe";
    var closed = new List<ClosedProcess>
    {
        new(20, 1, "msedge", edge, $"\"{edge}\" --profile-directory=Default"),
        new(21, 20, "msedge", edge, $"\"{edge}\" --type=renderer --lang=en-US"),
        new(23, 999, "msedge", edge, $"\"{edge}\" --type=crashpad-handler"), // a helper whose parent was not closed
        new(10, 9, "NVIDIA Overlay", overlay, $"\"{overlay}\""),
        new(30, 1, "HueSync", hue, $"\"{hue}\" "),
        new(31, 1, "HueSync", hue, $"\"{hue}\""), // a second instance with the same command line: started once
        new(50, 77, "RazerAppEngine", razer, " --url-params=apps=synapse,chroma-app --launch-force-hidden=synapse,chroma-app --autoStart=1"),
        new(60, 1, "RazerCortex", null, null), // unreadable (elevated): never started blindly
    };
    var targets = FreeVram.ReopenTargets(closed, runningImages: [overlay]); // NVIDIA App started its overlay again
    Require(targets.Count == 3, string.Join(" | ", targets));
    Require(targets[0] == new ReopenTarget("msedge", edge, "--profile-directory=Default"));
    Require(targets[1] == new ReopenTarget("HueSync", hue, ""));
    Require(targets[2] == new ReopenTarget("RazerAppEngine", razer, "--url-params=apps=synapse,chroma-app --launch-force-hidden=synapse,chroma-app --autoStart=1"), "a command line without the program is all arguments");
    Require(FreeVram.Arguments("msedge.exe --new-window", edge) == "--new-window" && FreeVram.Arguments("msedge --new-window", edge) == "--new-window");
    Require(FreeVram.Arguments($"{hue}", hue) == "" && FreeVram.Arguments(null, hue) == "" && FreeVram.Arguments("\"unterminated", hue) == "\"unterminated");
    Require(FreeVram.IsHelperProcess("--type=gpu-process") && FreeVram.IsHelperProcess("x \"--type=utility\"") && !FreeVram.IsHelperProcess("--typeface=x"));
    Require(FreeVram.ReopenTargets([], []).Count == 0);
    // Edge kept in the background (no visible window) comes back in the background, not as a new browser window.
    var background = FreeVram.ReopenTargets([new(20, 1, "msedge", edge, $"\"{edge}\" --profile-directory=Default", HadWindow: false), new(21, 20, "msedge", edge, $"\"{edge}\" --type=renderer")], []);
    Require(background.Single() == new ReopenTarget("msedge", edge, "--profile-directory=Default --no-startup-window"), string.Join(" | ", background));
    // A windowless program that is not a browser (no helper processes) starts with its own arguments only.
    Require(FreeVram.ReopenTargets([new(30, 1, "HueSync", hue, $"\"{hue}\" -silent", HadWindow: false)], []).Single().Arguments == "-silent");
});
Test("A profile with the old close list keeps closing those apps through Free VRAM", () =>
{
    var json = JsonData.Serialize(new VrProfile { CpuBoost = true }).Replace("\"boostCloseApps\": []", "\"boostCloseApps\": [\"notepad\", \"Spotify\"]");
    var node = System.Text.Json.Nodes.JsonNode.Parse(json)!.AsObject();
    foreach (var key in new[] { "freeVram", "freeVramApps", "freeVramReopen", "freeVramForce" }) node.Remove(key);
    var migrated = JsonData.Deserialize<VrProfile>(node.ToJsonString());
    Require(migrated is { FreeVram: true, FreeVramForce: true, FreeVramReopen: false } && migrated.FreeVramApps.SequenceEqual(["notepad", "Spotify"]) && migrated.BoostCloseApps.Count == 0);
    node["cpuBoost"] = false;
    Require(JsonData.Deserialize<VrProfile>(node.ToJsonString()) is { FreeVram: false }, "the old list did nothing without CPU Boost");
    var current = JsonData.Deserialize<VrProfile>(JsonData.Serialize(new VrProfile { FreeVram = true }));
    Require(current is { FreeVram: true, FreeVramReopen: true, FreeVramForce: false } && current == new VrProfile { FreeVram = true }, "new profiles round-trip unchanged");
});
Test("Small DCS window owns graphics width, height, fullScreen and aspect, and Back to stock DCS puts the user's values back", () =>
{
    var exe = Make("small-window/bin/DCS.exe", "fixture"); var runtime = Make("small-window/runtime.json", "{\"runtime\":{\"library_path\":\"runtime.dll\"}}"); Make("small-window/runtime.dll", "fixture");
    const string original = "options = {\n\t[\"graphics\"] = {\n\t\t[\"aspect\"] = 2.3888888888889,\n\t\t[\"fullScreen\"] = true,\n\t\t[\"height\"] = 1440,\n\t\t[\"sync\"] = false,\n\t\t[\"width\"] = 3440,\n\t},\n\t[\"VR\"] = {\n\t\t[\"enable\"] = true,\n\t},\n}\n";
    var options = Make("small-window/options.lua", original);
    var inventory = new InventorySnapshot { DcsExecutable = exe, OptionsPath = options, PimaxRuntime = runtime };
    var planner = new DeploymentPlanner(new(null, null, null)); var managed = Path.Combine(root, "small-window/managed");
    var p = new VrProfile { QuadViews = QuadProvider.None, KeepDcsLauncher = true, SmallDcsWindow = true };
    var plan = planner.Build(p, inventory, managed);
    var changes = plan.Files.Single(f => f.Path == options).LuaChanges!.ToDictionary(c => c.Path);
    Require(changes.Keys.Order().SequenceEqual(["graphics.aspect", "graphics.fullScreen", "graphics.height", "graphics.width"]), string.Join(",", changes.Keys));
    Require(changes["graphics.width"] is { PreviousRaw: "3440", InstalledRaw: "1280" } && changes["graphics.height"] is { PreviousRaw: "1440", InstalledRaw: "720" } && changes["graphics.fullScreen"] is { PreviousRaw: "true", InstalledRaw: "false" });
    var store = new OriginalsStore(Path.Combine(root, "originals/small-window")); store.Apply(plan);
    var lua = new LuaOptions(File.ReadAllText(options));
    Require(lua.Get("graphics", "width") == "1280" && lua.Get("graphics", "height") == "720" && lua.Get("graphics", "fullScreen") == "false" && lua.Get("graphics", "sync") == "false");
    Require(store.OwnedSettings(options)!.Keys.Order().SequenceEqual(["graphics.aspect", "graphics.fullScreen", "graphics.height", "graphics.width"]), "the four keys are owned");
    // Turned off in the next profile: the user's own values come back with it, without a restore in between.
    var off = planner.Build(p with { SmallDcsWindow = false }, inventory, managed, ownedSettings: store.OwnedSettings(options));
    store.Apply(off);
    Require(File.ReadAllText(options) == original, "turning it off puts the user's values back");
    store.Apply(planner.Build(p, inventory, managed, ownedSettings: store.OwnedSettings(options)));
    Require(store.RestoreOriginals().Complete && File.ReadAllText(options) == original, "Back to stock DCS is byte-exact");
    var already = Make("small-window/already.lua", "options = {\n\t[\"graphics\"] = {\n\t\t[\"fullScreen\"] = false,\n\t\t[\"height\"] = 720,\n\t\t[\"width\"] = 1280,\n\t},\n\t[\"VR\"] = {\n\t\t[\"enable\"] = true,\n\t},\n}\n");
    Require(planner.Build(p, inventory with { OptionsPath = already }, Path.Combine(root, "small-window/managed2")).Files.All(f => f.Path != already), "already small: options.lua is not touched");
});
Test("Monitor mode: set for the flight only, put back exactly, kept when the user changed it, and recovered after a crash", () =>
{
    var marker = Path.Combine(root, "display/boost/display-mode.json");
    var displays = new FakeDisplays();
    var original = displays.Current(@"\\.\DISPLAY1")!;
    var session = new FlightDisplay(displays, marker);
    Require(session.Apply(new(1920, 1080, 60)).Contains("-> 1920×1080 at 60 Hz") && displays.Mode.Mode == new DisplayMode(1920, 1080, 60) && File.Exists(marker));
    Require(displays.Mode with { Width = 3840, Height = 2160, Refresh = 144 } == original, "depth and position are kept");
    Require(session.Apply(new(1280, 720, 60)).Contains("already changed") && displays.Sets == 1, "applied once");
    Require(session.Restore()!.Contains("back to 3840×2160 at 144 Hz") && displays.Mode == original && !File.Exists(marker));
    Require(session.Restore() is null && displays.Sets == 2, "restored once");
    // Unsupported and unchanged modes are never set.
    Require(new FlightDisplay(displays, marker).Apply(new(1366, 768, 59)).Contains("does not report") && displays.Sets == 2 && !File.Exists(marker));
    Require(new FlightDisplay(displays, marker).Apply(new(3840, 2160, 144)).Contains("nothing changed") && displays.Sets == 2);
    // Windows refuses the mode: nothing is recorded.
    displays.Refuse = true; Require(new FlightDisplay(displays, marker).Apply(new(1920, 1080, 60)).Contains("refused") && !File.Exists(marker) && displays.Mode == original); displays.Refuse = false;
    // The user picked another mode during the flight: it is kept.
    var kept = new FlightDisplay(displays, marker); kept.Apply(new(1920, 1080, 60));
    displays.Mode = displays.Mode with { Width = 2560, Height = 1440, Refresh = 120 };
    Require(kept.Restore()!.Contains("left as it is") && displays.Mode.Mode == new DisplayMode(2560, 1440, 120) && !File.Exists(marker));
    // The helper was ended after the change: the marker sets it back at the next start, once.
    displays.Mode = original;
    new FlightDisplay(displays, marker).Apply(new(1920, 1080, 60));
    Require(File.Exists(marker) && JsonData.Deserialize<DisplayMarker>(File.ReadAllText(marker)) is { Device: @"\\.\DISPLAY1", Applied: { Width: 1920 } } m && m.Previous == original);
    Require(FlightDisplay.RestoreLeftover(displays, marker)!.Contains("back to") && displays.Mode == original && !File.Exists(marker));
    Require(FlightDisplay.RestoreLeftover(displays, marker) is null, "no marker, nothing to do");
    // After a restart Windows already shows the saved mode: the marker is only removed.
    new FlightDisplay(displays, marker).Apply(new(1920, 1080, 60)); displays.Mode = original; var sets = displays.Sets;
    Require(FlightDisplay.RestoreLeftover(displays, marker)!.Contains("not the flight mode") && displays.Sets == sets && !File.Exists(marker));
    // Setting the exact mode back fails: the registry mode is used, and the marker stays when that fails too.
    new FlightDisplay(displays, marker).Apply(new(1920, 1080, 60)); displays.Refuse = true;
    Require(FlightDisplay.RestoreLeftover(displays, marker)!.Contains("could not be set back") && File.Exists(marker));
    displays.Refuse = false; displays.RefuseExact = true;
    Require(FlightDisplay.RestoreLeftover(displays, marker)!.Contains("back to") && displays.Resets == 1 && displays.Mode == original && !File.Exists(marker));
    var stateRoot = Path.Combine(root, "display");
    Require(Path.GetFullPath(BoostRuntime.DisplayMarkerPath(stateRoot)) == Path.GetFullPath(marker) && BoostRuntime.RestoreLeftoverDisplay(stateRoot, displays) is null, "the app start restore reads the helper's marker");
    Require(ProfileValidation.Validate(new VrProfile { LowerMonitor = true, FlightDisplayWidth = 100 }).Any(i => i.Code == "monitor-mode") && !ProfileValidation.Validate(new VrProfile { LowerMonitor = true }).Any(i => i.Code == "monitor-mode"));
});

// --- Updates: versions, release selection, the 6-hour throttle, signatures, release notes, app settings (no network) ---
Test("Release versions read tags with or without v and order pre-releases before their release", () =>
{
    Require(ReleaseVersion.Parse("v0.5.6-preview") is { Major: 0, Minor: 5, Patch: 6, Suffix: "preview" } && ReleaseVersion.Parse("0.5.6") is { Suffix: "" });
    Require(ReleaseVersion.IsNewer("v0.5.6-preview", "0.5.5-preview") && ReleaseVersion.IsNewer("0.5.6", "0.5.6-preview") && !ReleaseVersion.IsNewer("0.5.6-preview", "0.5.6"));
    Require(ReleaseVersion.IsNewer("v1.0.0-preview", "0.99.99") && ReleaseVersion.IsNewer("0.10.0", "0.9.9") && !ReleaseVersion.IsNewer("v0.5.5-preview", "0.5.5-preview"));
    Require(ReleaseVersion.IsNewer("0.5.6-preview.10", "0.5.6-preview.9") && ReleaseVersion.IsNewer("0.5.6-rc", "0.5.6-preview") && ReleaseVersion.IsNewer("0.5.6-preview.1", "0.5.6-preview"));
    Require(ReleaseVersion.Parse(ProductInfo.Version) is not null, "the product version is a release version");
    foreach (var bad in new[] { "", "latest", "v1.2", "v1.2.3.4", "1.2.3-", "1.2.3-pre view", "vv1.2.3", "1.2.3+build", "v1.2.3-preview..1", "x1.2.3", "1234567.0.0", "v٠.5.11", "0.٥.1" })
        Require(ReleaseVersion.Parse(bad) is null && !ReleaseVersion.IsNewer(bad, "0.0.1"), "malformed tag accepted: " + bad);
});
Exception? Failure(Task task) { try { task.Wait(); return null; } catch (AggregateException e) { return e.InnerException; } }
const string ReleaseDownload = "https://github.com/NIGos/dcs-control/releases/download/";
string ReleaseList(params object[] releases) => System.Text.Json.JsonSerializer.Serialize(releases);
object GitHubRelease(string tag, bool draft = false, bool assets = true, string? body = null, string? assetRoot = null) => new
{
    tag_name = tag, draft, prerelease = tag.Contains('-'), html_url = Updates.ReleasesPage + "/tag/" + tag, body = body ?? "Notes for " + tag, published_at = "2026-10-11T10:00:00Z",
    assets = assets ? new object[] {
        new { name = $"DcsControl-{tag.TrimStart('v')}-win-x64.zip", browser_download_url = (assetRoot ?? ReleaseDownload + tag + "/") + $"DcsControl-{tag.TrimStart('v')}-win-x64.zip", size = 900 },
        new { name = $"DcsControl-{tag.TrimStart('v')}-Setup.exe", browser_download_url = (assetRoot ?? ReleaseDownload + tag + "/") + $"DcsControl-{tag.TrimStart('v')}-Setup.exe", size = 1000 },
        new { name = $"DcsControl-{tag.TrimStart('v')}-Setup.exe.sig", browser_download_url = (assetRoot ?? ReleaseDownload + tag + "/") + $"DcsControl-{tag.TrimStart('v')}-Setup.exe.sig", size = 88 } } : Array.Empty<object>()
};
Test("Release selection ignores drafts and malformed tags, takes the highest newer release and finds its setup and signature", () =>
{
    var json = ReleaseList(GitHubRelease("v9.0.0", draft: true), GitHubRelease("nightly"), GitHubRelease("v0.5.7-preview"), GitHubRelease("v0.5.8-preview", body: "**New** things"), GitHubRelease("v0.5.4-preview"));
    var update = Updates.Select(json, "0.5.6-preview")!;
    Require(update.Version == "0.5.8-preview" && update.Tag == "v0.5.8-preview" && update.Short == "0.5.8" && update.Notes == "**New** things" && update.PageUrl == Updates.ReleasesPage + "/tag/v0.5.8-preview");
    Require(update.CanInstall && update.SetupName == "DcsControl-0.5.8-preview-Setup.exe" && update.SetupUrl == ReleaseDownload + "v0.5.8-preview/DcsControl-0.5.8-preview-Setup.exe"
        && update.SignatureUrl == update.SetupUrl + ".sig" && update.SetupBytes == 1000 && update.Published == new DateTimeOffset(2026, 10, 11, 10, 0, 0, TimeSpan.Zero));
    Require(Updates.Select(json, "0.5.8-preview") is null && Updates.Select(json, "0.5.8") is null && Updates.Select("[]", "0.5.6-preview") is null, "nothing newer, nothing offered");
    Require(Updates.Select(ReleaseList(GitHubRelease("v0.5.7")), "0.5.7-preview")?.Version == "0.5.7", "the release after its pre-release");
    // A release without a setup (or with assets off this repository's download path) can only be opened on GitHub.
    Require(Updates.Select(ReleaseList(GitHubRelease("v0.5.7-preview", assets: false)), "0.5.6-preview") is { CanInstall: false, SetupUrl: null });
    Require(Updates.Select(ReleaseList(GitHubRelease("v0.5.7-preview", assetRoot: "https://example.com/NIGos/dcs-control/releases/download/v0.5.7-preview/")), "0.5.6-preview") is { CanInstall: false });
    Require(Updates.Select(ReleaseList(new { tag_name = "v0.5.7", draft = false, html_url = "https://evil.example/", body = (string?)null }), "0.5.6") is { PageUrl: Updates.ReleasesPage, Notes: "" });
    Throws<System.Text.Json.JsonException>(() => Updates.Select("<html>", "0.5.6"));
});
Test("Update downloads only from this repository's release assets", () =>
{
    Require(Updates.AllowedAssetUrl(ReleaseDownload + "v0.5.7-preview/DcsControl-0.5.7-preview-Setup.exe"));
    foreach (var url in new[] { "http://github.com/NIGos/dcs-control/releases/download/v1/x.exe", "https://github.com.evil.example/NIGos/dcs-control/releases/download/v1/x.exe",
        "https://github.com/Other/dcs-control/releases/download/v1/x.exe", "https://github.com:8443/NIGos/dcs-control/releases/download/v1/x.exe", "https://github.com/NIGos/dcs-control/releases/download/../../x.exe", "not a url" })
        Require(!Updates.AllowedAssetUrl(url), "allowed: " + url);
});
Test("The diagnostic package keeps a long log's start and end", () =>
{
    var path = Path.Combine(root, "diagnostic-head-tail.log"); Directory.CreateDirectory(root);
    File.WriteAllText(path, "startup line\n" + string.Concat(Enumerable.Range(0, 2000).Select(i => $"block {i}\n")) + "last line\n");
    var cut = Diagnostics.HeadAndTail(path, 100, 100);
    Require(cut.StartsWith("startup line\n", StringComparison.Ordinal) && cut.EndsWith("last line\n", StringComparison.Ordinal) && cut.Contains("bytes left out by the diagnostic package", StringComparison.Ordinal), cut);
    Require(cut.Split('\n').All(l => l.Length == 0 || l.StartsWith("block ", StringComparison.Ordinal) || l.StartsWith("startup", StringComparison.Ordinal) || l.StartsWith("last", StringComparison.Ordinal) || l.StartsWith("[...", StringComparison.Ordinal)), "only whole lines");
    Require(Diagnostics.HeadAndTail(path, 1 << 20, 1 << 20) == File.ReadAllText(path), "a short file is whole");
});
Test("NVIDIA Threaded optimization Off is named before the flight and in the engine status, with where to change it", () =>
{
    var previous = NvidiaSettings.ThreadedOptimizationForDcs;
    try
    {
        var profile = new VrProfile { EngineOptimizations = true };
        NvidiaSettings.ThreadedOptimizationForDcs = () => new(ThreadedOptimization.Off, true);
        var warning = ProfileValidation.Validate(profile).SingleOrDefault(i => i.Code == "nvidia-threaded-optimization");
        Require(warning is { Severity: IssueSeverity.Warning } && warning.Message.Contains("DCS program profile", StringComparison.Ordinal) && warning.Message.Contains("Use global setting", StringComparison.Ordinal), warning?.Message ?? "no warning");
        Require(EngineOptimizations.NoCommandListsAdvice().Contains("Threaded optimization is Off in the DCS program profile", StringComparison.Ordinal));
        Require(!ProfileValidation.Validate(profile with { EngineShadowRecorder = false, EngineGBufferRecorder = false }).Any(i => i.Code == "nvidia-threaded-optimization"), "nothing to say when the recorders are off");
        NvidiaSettings.ThreadedOptimizationForDcs = () => new(ThreadedOptimization.Off, false);
        Require(ProfileValidation.Validate(profile).Single(i => i.Code == "nvidia-threaded-optimization").Message.Contains("Global settings: set Threaded optimization to Auto", StringComparison.Ordinal));
        foreach (var fine in new ThreadedOptimizationSetting?[] { new(ThreadedOptimization.Auto, false), new(ThreadedOptimization.On, true), null })
        {
            NvidiaSettings.ThreadedOptimizationForDcs = () => fine;
            Require(!ProfileValidation.Validate(profile).Any(i => i.Code == "nvidia-threaded-optimization"), "no warning for " + fine);
        }
        // MFAA on: the same effect, named with where to turn it off (GitHub issue #1).
        NvidiaSettings.ThreadedOptimizationForDcs = () => new(ThreadedOptimization.Auto, false);
        var previousMfaa = NvidiaSettings.MfaaForDcs;
        try
        {
            NvidiaSettings.MfaaForDcs = () => new(true, true);
            Require(ProfileValidation.Validate(profile).Single(i => i.Code == "nvidia-mfaa").Message.Contains("MFAA is On in the DCS program profile", StringComparison.Ordinal), "MFAA warning");
            Require(EngineOptimizations.NoCommandListsAdvice().Contains("Antialiasing - MFAA is On", StringComparison.Ordinal), "MFAA advice");
            NvidiaSettings.MfaaForDcs = () => new(false, true);
            Require(!ProfileValidation.Validate(profile).Any(i => i.Code == "nvidia-mfaa"), "MFAA off: nothing");
        }
        finally { NvidiaSettings.MfaaForDcs = previousMfaa; }
        // NVIDIA card, setting fine or unreadable: the other causes; no NVIDIA card: the driver itself.
        Require(EngineOptimizations.NoCommandListsAdvice().Contains("integrated graphics", StringComparison.Ordinal));
        var adapters = GpuInfo.Adapters;
        try { GpuInfo.Adapters = () => [new("AMD Radeon RX 7900 XTX", "AMD", "31.0")]; Require(EngineOptimizations.NoCommandListsAdvice().Contains("common on AMD", StringComparison.Ordinal)); }
        finally { GpuInfo.Adapters = adapters; }
        // The module's own line (0.5.7) says which capability, the creation flags and DCS's adapter.
        const string line = "shadow recorder: the D3D11 device lacks what the recorder needs: DriverCommandLists=0 ConstantBufferOffsetting=1 ID3D11DeviceContext1=1 creation flags 0x0 on {0} (vendor {1}, LUID 0:1) -- hint; unavailable";
        Require(EngineOptimizations.NoCommandListsAdvice(string.Format(System.Globalization.CultureInfo.InvariantCulture, line, "AMD Radeon(TM) Graphics", "0x1002")).Contains("DCS runs on AMD Radeon(TM) Graphics, not on the NVIDIA card", StringComparison.Ordinal), "integrated graphics");
        Require(EngineOptimizations.NoCommandListsAdvice(string.Format(System.Globalization.CultureInfo.InvariantCulture, line, "NVIDIA GeForce RTX 4090", "0x10de").Replace("flags 0x0", "flags 0x8 (PREVENT_INTERNAL_THREADING_OPTIMIZATIONS)", StringComparison.Ordinal)).Contains("hooked into DCS", StringComparison.Ordinal), "injector flag");
        Require(EngineOptimizations.NoCommandListsAdvice("x: DriverCommandLists=1 ConstantBufferOffsetting=0 ID3D11DeviceContext1=1 on Y (vendor 0x10de)").Contains("ConstantBufferOffsetting missing", StringComparison.Ordinal), "another capability");
    }
    finally { NvidiaSettings.ThreadedOptimizationForDcs = previous; }
});
Test("A panel-only profile with bundled Quad Views plans through the service with the fork's OFXR in pass-through", () =>
{
    var dist = Path.Combine(root, "panel-only/dist");
    var quadSource = Path.Combine(workspace, "external/quadviews/bin/x64/Release");
    var quadDir = Path.Combine(dist, "components/quadviews"); Directory.CreateDirectory(quadDir);
    foreach (var name in new[] { "XR_APILAYER_MBUCCHIA_quad_views_foveated.dll", "openxr-api-layer.json", "settings.cfg" })
    { File.Copy(Path.Combine(quadSource, name), Path.Combine(quadDir, name), true); File.WriteAllText(Path.Combine(quadDir, name + ".sha256"), Hashing.FileSha256(Path.Combine(quadDir, name))); }
    var ofxrDir = Path.Combine(dist, "components/ofxr"); Directory.CreateDirectory(ofxrDir);
    var ofxrDll = Path.Combine(ofxrDir, "XR_APILAYER_XRFrameBridge_diagnostic.dll");
    File.Copy(DeferredOfxr, ofxrDll, true); File.WriteAllText(ofxrDll + ".sha256", Hashing.FileSha256(ofxrDll));
    var exe = Make("panel-only/bin/DCS.exe", "fixture"); var options = Make("panel-only/options.lua", Lua);
    var runtime = Make("panel-only/runtime.json", "{\"runtime\":{\"library_path\":\"runtime.dll\"}}"); Make("panel-only/runtime.dll", "fixture");
    var service = new ControlService(dist, Path.Combine(root, "panel-only/state"));
    var draft = new VrProfile { QuadViews = QuadProvider.QuadViewsFoveated, FrameGen = FrameGeneration.Off, HeadsetPanel = true };
    var plan = service.Preview(draft, new() { DcsExecutable = exe, OptionsPath = options, PimaxRuntime = runtime });
    var ini = Encoding.Unicode.GetString(plan.Files.Single(f => f.Path.EndsWith("ofxr_bridge.ini")).Content).ReplaceLineEndings("\n");
    Require(plan.LaunchEnvironment["XR_ENABLE_API_LAYERS"].Split(';').Contains("XR_APILAYER_XRFrameBridge_diagnostic") && ini.Contains("enabled=0\n") && ini.Contains("d3d11_bridge=0\n"), "OFXR deployed in pass-through");
    Require(plan.LaunchEnvironment["DCSVR_DIAG_HOTKEY"] != "0:0", "the panel key works without frame generation");
    // Without the panel, nothing of OFXR.
    plan = service.Preview(draft with { HeadsetPanel = false }, new() { DcsExecutable = exe, OptionsPath = options, PimaxRuntime = runtime });
    Require(!plan.LaunchEnvironment["XR_ENABLE_API_LAYERS"].Contains("XRFrameBridge") && plan.LaunchEnvironment["DCSVR_DIAG_HOTKEY"] == "0:0", "no OFXR without frame generation or the panel");
});
Test("Far depth is off by default and switches the engine's depth export and Quad Views' pass together", () =>
{
    var off = new VrProfile { QuadViews = QuadProvider.QuadViewsFoveated, EngineOptimizations = true };
    Require(!off.FarDepth && !off.UsesFarDepth, "off by default");
    Require(ConfigurationWriters.DcsQvCull(off).ReplaceLineEndings("\n").Contains("FarDepthExport=0\n") && ConfigurationWriters.QuadViews(off).ReplaceLineEndings("\n").Contains("dcsvr_far_depth=0\n"), "both off");
    var on = off with { FarDepth = true, FarDepthPreset = FarDepthPreset.Strong };
    Require(ConfigurationWriters.DcsQvCull(on).ReplaceLineEndings("\n").Contains("FarDepthCoverageMode=hdr\n"), "the HUD found in the HDR image (F-16: HUD after DLSS)");
    Require(ConfigurationWriters.DcsQvCull(on).ReplaceLineEndings("\n").Contains("FarDepthExport=1\n") &&ConfigurationWriters.QuadViews(on).ReplaceLineEndings("\n").Contains("dcsvr_far_depth=1\ndcsvr_far_depth_preset=strong\n") && !ConfigurationWriters.QuadViews(on).Contains("allow_unmasked", StringComparison.Ordinal) && ConfigurationWriters.DcsQvCull(on).ReplaceLineEndings("\n").Contains("FarDepthPreOverlay=1\n"), "both on, with the preset and the HUD kept out");
    Require(!(on with { EngineOptimizations = false }).UsesFarDepth && ProfileValidation.Validate(on with { EngineOptimizations = false }).Any(i => i.Code == "far-depth-needs"), "needs the engine module");
    Require(!(on with { QuadViews = QuadProvider.PimaxNative }).UsesFarDepth, "needs the bundled Quad Views");
    Require(ConfigurationWriters.QuadViews(on).ReplaceLineEndings("\n").Contains("dcsvr_far_depth_toggle=119:6\n") && ConfigurationWriters.QuadViews(off).ReplaceLineEndings("\n").Contains("dcsvr_far_depth_toggle=0:0\n"), "its in-flight switch, Alt+Shift+F8");
    Require(ProfileValidation.Validate(on with { FarDepthToggleKey = NeuralHotkeys.EngineDefault }).Any(i => i.Code == "hotkey-conflict"), "the switch needs a key of its own");
    // The module's test suite starts on Ctrl+Alt+F11 with Shift held too, and from any in-flight key.
    Require(ProfileValidation.Validate(on with { FarDepthToggleKey = "122:7" }).Any(i => i.Code == "far-depth-hotkey" && i.Severity == IssueSeverity.Error), "Ctrl+Alt+Shift+F11 is the suite's");
    Require(ProfileValidation.Validate(on with { NeuralRendering = true, NeuralToggleKey = "122:3" }).Any(i => i.Code == "neural-hotkey" && i.Severity == IssueSeverity.Error), "the DLSS 5 key too");
    Require(!ProfileValidation.Validate(on with { NeuralRendering = true, NeuralToggleKey = "122:3", EngineOptimizations = false }).Any(i => i.Code == "neural-hotkey" && i.Message.Contains("reserved by Engine Optimizations")), "only while the module runs");
    // options.lua outside Saved Games\DCS*\Config: a warning, since DCS never reads it there.
    Require(ProfileValidation.IsSavedGamesOptions(@"C:\Games\Saved Games\DCS.openbeta\Config\options.lua") && !ProfileValidation.IsSavedGamesOptions(@"D:\options.lua") && !ProfileValidation.IsSavedGamesOptions(@"D:\backup\options.lua"), "Saved Games location");
    Require(ProfileValidation.Validate(new VrProfile(), new InventorySnapshot { OptionsPath = @"D:\backup\options.lua" }).Any(i => i.Code == "options-location" && i.Severity == IssueSeverity.Warning), "warned");
});
Test("The in-headset panel without frame generation loads OFXR only to draw it: no generation, no D3D11 bridge", () =>
{
    var panelOnly = new VrProfile { FrameGen = FrameGeneration.Off, HeadsetPanel = true };
    Require(panelOnly.UsesHeadsetPanel && !new VrProfile { FrameGen = FrameGeneration.Off }.UsesHeadsetPanel && new VrProfile { FrameGen = FrameGeneration.Nvidia }.UsesHeadsetPanel, "who gets the panel");
    Require(!(panelOnly with { Desktop = true }).UsesHeadsetPanel, "never on a monitor");
    var ini = ConfigurationWriters.Ofxr(panelOnly with { ShowOverlay = true }).ReplaceLineEndings("\n");
    Require(ini.Contains("enabled=0\n") && ini.Contains("d3d11_bridge=0\n") && ini.Contains("position=off"), "pass-through OFXR: " + ini);
    var generating = ConfigurationWriters.Ofxr(new VrProfile { FrameGen = FrameGeneration.Nvidia }).ReplaceLineEndings("\n");
    Require(generating.Contains("enabled=1\n") && generating.Contains("d3d11_bridge=1\n"), "frame generation unchanged");
    Require(ProfileValidation.Validate(panelOnly with { DiagnosticOverlayKey = "Ctrl+Alt+F11" }).Any(i => i.Code.StartsWith("diagnostic-hotkey", StringComparison.Ordinal)), "the panel key is checked without frame generation too");
    Require(ini.Contains("logging_enabled=0\n") && ini.Contains("visibility_mask=0\n"), "no flight log, no visibility mask for the panel alone");
    Require(generating.Contains("logging_enabled=1\n") && generating.Contains("visibility_mask=1\n"), "frame generation keeps them");
    Require(!(panelOnly with { DiagnosticOverlayKey = "0:0", DiagnosticOverlayAtStart = false }).UsesHeadsetPanel && (panelOnly with { DiagnosticOverlayKey = "0:0", DiagnosticOverlayAtStart = true }).UsesHeadsetPanel, "a panel that can never show loads nothing");
    Require(ProfileValidation.Validate(panelOnly with { NeuralRendering = true, NeuralRuntimePath = "x.dll", NeuralToggleKey = NeuralHotkeys.DiagnosticDefault }).Any(i => i.Code == "hotkey-conflict"), "DLSS 5 and the panel cannot share a key without frame generation either");
});
Test("The update check asks GitHub at most every 6 hours unless forced, and offline keeps what it knew", () =>
{
    var state = Path.Combine(root, "updates/throttle"); var now = new DateTimeOffset(2026, 10, 11, 8, 0, 0, TimeSpan.Zero);
    var handler = new UrlFixtureHandler(new() { [Updates.ReleasesUrl] = Encoding.UTF8.GetBytes(ReleaseList(GitHubRelease("v0.5.7-preview"))) });
    var updates = new Updates(state, handler, () => now, "0.5.6-preview");
    Require(updates.Check().Result is { Error: null, Update.Version: "0.5.7-preview" } && handler.Requests.Count == 1);
    now = now.AddHours(5.9); Require(updates.Check().Result.Update?.Version == "0.5.7-preview" && handler.Requests.Count == 1, "within 6 hours the cached check answers");
    Require(updates.Check(force: true).Result.Update is not null && handler.Requests.Count == 2, "Check now always asks");
    now = now.AddHours(6.1); Require(updates.Check().Result.Update is not null && handler.Requests.Count == 3, "after 6 hours it asks again");
    Require(handler.Requests.All(r => r == Updates.ReleasesUrl) && handler.UserAgents.All(a => a.StartsWith("DcsControl/", StringComparison.Ordinal)), "only the release list, with a User-Agent");
    // Offline: an error, not an exception, and the update found before is kept.
    handler.Offline = true; now = now.AddHours(7);
    var offline = updates.Check().Result;
    Require(offline.Error is { Length: > 0 } && offline.Update?.Version == "0.5.7-preview" && offline.CheckedAt == now, "offline keeps the known update");
    // A failed check is tried again after 30 minutes, not 6 hours.
    handler.Offline = false; var asked = handler.Requests.Count;
    now = now.AddMinutes(20); Require(updates.Check().Result.Error is not null && handler.Requests.Count == asked, "a failed check is kept for a while");
    now = now.AddMinutes(15); Require(updates.Check().Result.Error is null && handler.Requests.Count == asked + 1, "a failed check is retried after 30 minutes");
    handler.Offline = true;
    Require(new Updates(Path.Combine(root, "updates/offline-first"), handler, () => now, "0.5.6-preview").Check().Result is { Update: null, Error: not null });
    // A new installed version starts over: the cache was for another version, and an update that is no longer newer is dropped.
    Require(new Updates(state, handler, () => now, "0.5.7-preview").Cached() is null);
    AtomicFile.WriteText(updates.CachePath, "{ not json"); Require(updates.Cached() is null && updates.Check().Result.Error is not null, "a damaged cache is ignored");
});
Test("Release signatures: ECDSA P-256 over SHA-256 in IEEE P1363 form, a changed file or signature is refused", () =>
{
    using var key = System.Security.Cryptography.ECDsa.Create(System.Security.Cryptography.ECCurve.NamedCurves.nistP256);
    var publicPem = key.ExportSubjectPublicKeyInfoPem(); var data = Encoding.UTF8.GetBytes("setup bytes");
    var signature = key.SignData(data, System.Security.Cryptography.HashAlgorithmName.SHA256, System.Security.Cryptography.DSASignatureFormat.IeeeP1363FixedFieldConcatenation);
    Require(signature.Length == 64 && Updates.Verify(new MemoryStream(data), signature, publicPem));
    var changed = (byte[])data.Clone(); changed[0] ^= 1; Require(!Updates.Verify(new MemoryStream(changed), signature, publicPem), "changed file");
    var forged = (byte[])signature.Clone(); forged[10] ^= 1; Require(!Updates.Verify(new MemoryStream(data), forged, publicPem), "changed signature");
    Require(!Updates.Verify(new MemoryStream(data), signature, Updates.PublicKeyPem), "another key");
    using var p384 = System.Security.Cryptography.ECDsa.Create(System.Security.Cryptography.ECCurve.NamedCurves.nistP384);
    Require(!Updates.Verify(new MemoryStream(data), new byte[96], p384.ExportSubjectPublicKeyInfoPem()), "only P-256");
    Require(Updates.ParseSignature(Encoding.ASCII.GetBytes(Convert.ToBase64String(signature) + "\r\n")).SequenceEqual(signature));
    Throws<InvalidDataException>(() => Updates.ParseSignature(Encoding.ASCII.GetBytes("not base64!")));
    Throws<InvalidDataException>(() => Updates.ParseSignature(Encoding.ASCII.GetBytes(Convert.ToBase64String(new byte[70]))));
});
Test("The embedded release key is an ECDSA P-256 public key (and no private key)", () =>
{
    using var key = System.Security.Cryptography.ECDsa.Create(); key.ImportFromPem(Updates.PublicKeyPem);
    Require(key.KeySize == 256 && (key.ExportParameters(false).Curve.Oid is { } curve && (curve.Value == "1.2.840.10045.3.1.7" || curve.FriendlyName is "nistP256" or "ECDSA_P256")) && !Updates.PublicKeyPem.Contains("PRIVATE", StringComparison.Ordinal));
    Throws<System.Security.Cryptography.CryptographicException>(() => key.ExportParameters(true));
});
Test("A downloaded setup is kept only when its signature verifies; otherwise it is deleted", () =>
{
    using var key = System.Security.Cryptography.ECDsa.Create(System.Security.Cryptography.ECCurve.NamedCurves.nistP256);
    var setup = Encoding.UTF8.GetBytes("MZ setup fixture"); var update = Updates.Select(ReleaseList(GitHubRelease("v0.5.7-preview")), "0.5.6-preview")! with { SetupBytes = setup.Length };
    var signature = Encoding.ASCII.GetBytes(Convert.ToBase64String(key.SignData(setup, System.Security.Cryptography.HashAlgorithmName.SHA256, System.Security.Cryptography.DSASignatureFormat.IeeeP1363FixedFieldConcatenation)));
    var handler = new UrlFixtureHandler(new() { [update.SetupUrl!] = setup, [update.SignatureUrl!] = signature });
    var downloads = Path.Combine(root, "updates/downloads");
    var updates = new Updates(Path.Combine(root, "updates/download-state"), handler, currentVersion: "0.5.6-preview", publicKeyPem: key.ExportSubjectPublicKeyInfoPem(), downloadRoot: downloads);
    var path = updates.Download(update).Result;
    Require(path == Path.GetFullPath(Path.Combine(downloads, "0.5.7-preview", "DcsControl-0.5.7-preview-Setup.exe")) && File.ReadAllBytes(path).SequenceEqual(setup) && updates.VerifyFile(path));
    File.WriteAllBytes(path, [.. setup, 0]); Require(!updates.VerifyFile(path), "a setup changed after the download is refused");
    // Signed with another key (or altered on the way): refused and nothing is left behind.
    var tampered = (byte[])setup.Clone(); tampered[3] ^= 1; handler.Answers[update.SetupUrl!] = tampered;
    var refused = false; try { updates.Download(update).Wait(); } catch (AggregateException e) when (e.InnerException is InvalidDataException) { refused = true; }
    Require(refused && Directory.GetFiles(downloads, "*", SearchOption.AllDirectories).Length == 0, "the refused setup and its signature are deleted");
    Require(Failure(new Updates(Path.Combine(root, "updates/download-state"), handler, downloadRoot: downloads).Download(update)) is InvalidDataException, "the embedded key refuses a setup signed with another key");
    // A release without a setup, or a size that does not match, never reaches the signature check.
    Require(Failure(updates.Download(update with { SetupUrl = null })) is InvalidOperationException);
    handler.Answers[update.SetupUrl!] = setup; Require(Failure(updates.Download(update with { SetupBytes = 5 })) is IOException);
    updates.CleanDownloads(); Require(!Directory.EnumerateFileSystemEntries(downloads).Any());
});
Test("Release notes: the section of the running version, from its heading to the next", () =>
{
    var notes = "# 0.5.7 preview\r\n\r\n**New** in 0.5.7.\r\n- one\r\n\r\n# 0.5.6 preview\n\nSix.\n- a\n  - b\n\n# 0.5.5 preview\n\nFive.\n";
    Require(Updates.ReleaseNotesSection(notes, "0.5.7-preview") == "**New** in 0.5.7.\n- one" && Updates.ReleaseNotesSection(notes, "0.5.6-preview") == "Six.\n- a\n  - b");
    Require(Updates.ReleaseNotesSection(notes, "0.5.5-preview") == "Five." && Updates.ReleaseNotesSection(notes, "0.5.4-preview") is null && Updates.ReleaseNotesSection(notes, "garbage") is null);
    Require(Updates.ReleaseNotesSection("# 1.0.0\n\nFinal.", "1.0.0") == "Final." && Updates.ReleaseNotesSection("# 0.5.6 preview\n\n# 0.5.5 preview\nx", "0.5.6-preview") is null);
    var shipped = File.ReadAllText(Path.Combine(workspace, "docs/RELEASE_NOTES.md"));
    Require(Updates.ReleaseNotesSection(shipped, ProductInfo.Version) is { Length: > 100 }, "docs/RELEASE_NOTES.md has a section for " + ProductInfo.Version);
});
Test("DLSS 5 focus: a central area is round with a round focus, and the focus preset reaches Cheeky", () =>
{
    var focus = new VrProfile { NeuralRendering = true, QuadViews = QuadProvider.QuadViewsFoveated, QuadFocusAdapter = true, NeuralFocusArea = 90, FocusDlssPreset = 11 };
    var ini = ConfigurationWriters.Cheeky(focus);
    Require(focus.UsesCentralNeuralArea && (!focus.QuadRoundFocus || ini.Contains("NrRoundness=1")) && ini.Contains("NrWidth=0.9") && ini.Contains("CenterPreset=11"), ini);
    Require(ConfigurationWriters.Cheeky(focus with { FocusDlssPreset = 0 }).Contains("CenterPreset=0"));
    Require(ProfileValidation.Validate(focus with { FocusDlssPreset = 10 }).Any(i => i.Code == "focus-preset"), "J is not offered");
});
Test("autoexec.cfg: HUD_MFD_after_DLSS is found only on a live line", () =>
{
    Require(FramePacing.HudAfterDlss("max_fps = 90\nHUD_MFD_after_DLSS = true\n") && FramePacing.HudAfterDlss("options.graphics.HUD_MFD_after_DLSS=true"));
    Require(!FramePacing.HudAfterDlss("-- HUD_MFD_after_DLSS = true") && !FramePacing.HudAfterDlss("HUD_MFD_after_DLSS = false") && !FramePacing.HudAfterDlss(""));
    Require(!FramePacing.HudAfterDlss("--[[\nHUD_MFD_after_DLSS = true\n--]]"), "inside a block comment");
    Require(!FramePacing.HudAfterDlss("HUD_MFD_after_DLSS = true\nHUD_MFD_after_DLSS = false"), "the last assignment wins");
});
Test("Far depth and Periphery stability run only with the bundled Quad Views provider, never an alternative one", () =>
{
    var bundled = new VrProfile { QuadViews = QuadProvider.QuadViewsFoveated, FarDepth = true, PeripheryStability = true, EngineOptimizations = true };
    Require(bundled.UsesBundledQuadViews && bundled.UsesFarDepth && bundled.UsesPeripheryStability);
    var other = bundled with { QuadViewsLayerDirectory = "D:/Other Quad Views" };
    Require(!other.UsesBundledQuadViews && !other.UsesFarDepth && !other.UsesPeripheryStability, "An alternative provider has neither pass");
    Require(ProfileValidation.Validate(other).Any(i => i.Message.Contains("Far depth", StringComparison.Ordinal)), "and the checks say so");
});
Test("Engine settings: Cache distant shadows is off by default and written explicitly either way", () =>
{
    // Framegen's cockpit mask: exported only with frame generation (the module still waits for the layer's heartbeat).
    Require(ConfigurationWriters.DcsQvCull(new VrProfile { EngineOptimizations = true, FrameGen = FrameGeneration.Nvidia }).Contains("CockpitMaskExport=1")
        && ConfigurationWriters.DcsQvCull(new VrProfile { EngineOptimizations = true }).Contains("CockpitMaskExport=0"), "Cockpit mask follows Framegen");
    Require(ConfigurationWriters.DcsQvCull(new VrProfile { EngineOptimizations = true, FrameGen = FrameGeneration.Nvidia }) is var fg && fg.Contains("CockpitDepthExport=1") && fg.Contains("CockpitMotionExport=1")
        && ConfigurationWriters.DcsQvCull(new VrProfile { EngineOptimizations = true }).Contains("CockpitMotionExport=0"), "Depth and motion follow Framegen");
    var stock = ConfigurationWriters.DcsQvCull(new VrProfile { EngineOptimizations = true }).ReplaceLineEndings("\n");
    Require(stock.Contains("[Shadows]\nCascadeCache=0\n"), "Off by default, written");
    var on = ConfigurationWriters.DcsQvCull(new VrProfile { EngineOptimizations = true, EngineShadowCache = true }).ReplaceLineEndings("\n");
    Require(on.Contains("[Shadows]\nCascadeCache=1\n") && !on.Contains("CascadeCacheMask") && !on.Contains("CascadeCacheMaxAge"), "On when chosen; the sub-keys keep the module's defaults");
});
Test("CPU Boost: processes changed by a helper that was ended are restored at the next start, only the same process", () =>
{
    var state = PathPolicy.UnderRoot(root, "boost-ledger/state"); Directory.CreateDirectory(Path.Combine(state, "boost"));
    using var self = Process.GetCurrentProcess();
    var before = self.PriorityClass;
    try
    {
        self.PriorityClass = ProcessPriorityClass.BelowNormal;
        var start = self.StartTime.Ticks; var affinity = (ulong)(long)self.ProcessorAffinity;
        // What a helper recorded before it was ended: this process, and one whose PID came back as another process.
        var ledger = new[] { new BoostChange(self.Id, self.ProcessName, start, affinity, "Normal", affinity, "BelowNormal"),
                             new BoostChange(self.Id, self.ProcessName, start - 1, affinity, "High", affinity, "BelowNormal") };
        AtomicFile.WriteText(BoostRuntime.LedgerPath(state), JsonData.Serialize(ledger));
        Require(BoostRuntime.RestoreLeftoverProcesses(state) == 1, "Only the same process (PID and start time) is restored");
        self.Refresh();
        Require(self.PriorityClass == ProcessPriorityClass.Normal, "Its original priority is back");
        Require(!File.Exists(BoostRuntime.LedgerPath(state)) && BoostRuntime.RestoreLeftoverProcesses(state) is null, "The record is gone; a second start does nothing");
    }
    finally { self.PriorityClass = before; }
});
Test("Import: a JSON file that is not a DCS Control profile is refused with a plain message", () =>
{
    var service = new ControlService(root, Path.Combine(root, "import-json/state"));
    var report = Make("import-json/dcs-control-report.json", "{\"schemaVersion\":1,\"version\":\"0.5.10-preview\",\"inventory\":{}}");
    try { service.LoadProfile(report); throw new Exception("A report was imported as a profile"); }
    catch (InvalidDataException e) { Require(e.Message.Contains("not a DCS Control profile"), e.Message); }
});
Test("App settings: update checks on by default, saved, unknown keys tolerated; What's new once per version", () =>
{
    var state = Path.Combine(root, "app-settings"); var store = new AppSettingsStore(state);
    // A settings file that cannot be read now is never saved over (the pilot's choices survive a damaged or locked file).
    Directory.CreateDirectory(state); File.WriteAllText(store.Path, "{ damaged");
    Require(store.Read() is null && store.Load() == new AppSettings() && !store.TakeWhatsNew("0.5.6-preview", true) && File.ReadAllText(store.Path) == "{ damaged", "a damaged file is left alone");
    File.Delete(store.Path);
    Require(store.Load() == new AppSettings(true, null));
    store.Save(new(false, "0.5.5-preview")); Require(new AppSettingsStore(state).Load() == new AppSettings(false, "0.5.5-preview"));
    AtomicFile.WriteText(store.Path, "{\"checkForUpdates\":false,\"aLaterSetting\":3}"); Require(store.Load() == new AppSettings(false, null));
    AtomicFile.WriteText(store.Path, "not json"); Require(store.Load() == new AppSettings(true, null));
    // A new installation records the version without showing; an update from a version that did not record it shows once.
    var fresh = new AppSettingsStore(Path.Combine(root, "app-settings-fresh"));
    Require(!fresh.TakeWhatsNew("0.5.6-preview", usedBefore: false) && fresh.Load().LastSeenVersion == "0.5.6-preview" && !fresh.TakeWhatsNew("0.5.6-preview", usedBefore: true));
    var updated = new AppSettingsStore(Path.Combine(root, "app-settings-updated"));
    Require(updated.TakeWhatsNew("0.5.6-preview", usedBefore: true) && !updated.TakeWhatsNew("0.5.6-preview", usedBefore: true));
    Require(updated.TakeWhatsNew("0.5.7-preview", usedBefore: false) && updated.Load() is { LastSeenVersion: "0.5.7-preview", CheckForUpdates: true }, "a later version shows again; the check setting is kept");
});
Test("Pimax eye tracking is found by its service's prefix (the name carries the headset model)", () =>
{
    // The default probe's matcher over a fixed process list.
    Func<string, bool> Running(params string[] names) => pattern => DetectionSources.AnyMatches(pattern, names);
    var sources = new DetectionSources { SteamVrServerLog = Path.Combine(root, "missing.txt"), PimaxPlaySettings = Path.Combine(root, "missing.json"), NeuralRuntimeFolders = [] };
    var p = new VrProfile { Runtime = RuntimeKind.Pimax, QuadViews = QuadProvider.QuadViewsFoveated, Gaze = GazeMode.Fixed };
    var eyes = SetupDetection.Detect(p, new InventorySnapshot(), sources with { ProcessRunning = Running("pi_server", "PiService", "platform_runtime_VR4PIMAXP3B_service") });
    Require(eyes.Profile.Gaze == GazeMode.EyeTracked, "Crystal: platform_runtime_VR4PIMAXP3B_service");
    Require(SetupDetection.Detect(p, new InventorySnapshot(), sources with { ProcessRunning = Running("pi_server", "PiService", "platform_runtime_XR5EYECHIP_WIN10_x64") }).Profile.Gaze == GazeMode.EyeTracked);
    Require(SetupDetection.Detect(p with { Gaze = GazeMode.EyeTracked }, new InventorySnapshot(), sources with { ProcessRunning = Running("pi_server", "PiService", "platform_runtime_IS5LEYETRACKER5_service") }).Profile.Gaze == GazeMode.Fixed, "a desktop Tobii is not the headset's");
    Require(!Running("vrserver_helper")("vrserver") && Running("PiService")("PiService"), "names without a wildcard stay exact");
});
Test("Flight helpers never close or move another signed-in user's programs; services are confined only", () =>
{
    var snapshot = new BoostSnapshot([new(10, "msedge", 1, true, 1), new(11, "msedge", 1, true, 2), new(12, "msedge", 1, true, 0), new(13, "msedge", 1, true, -1)], null, OwnSession: 1);
    Require(FreeVram.Targets(snapshot, ["msedge"], new HashSet<int>()).Select(t => t.Pid).SequenceEqual([10]), "only this session's programs are closed");
    Require(FreeVram.Describe(snapshot, ["msedge"], new HashSet<int>(), null).Single().Processes == 1);
    Require(snapshot.Processes.Where(p => !snapshot.OtherSession(p, services: true)).Select(p => p.Pid).SequenceEqual([10, 12]), "a service may be confined");
    Require(FreeVram.Targets(snapshot with { OwnSession = null }, ["msedge"], new HashSet<int>()).Count == 4, "a fixture without sessions filters nothing");
});
Test("Boost ledger records processes a confined program started, so the restore gives them all CPUs", () =>
{
    var ledger = new BoostLedger();
    ulong all = 0xFFFF, bg = 0xF000; var ours = new[] { 0xFC00UL, bg };
    Require(ledger.TryRecord(10, "msedge", 100, all, "Normal", bg, "BelowNormal", all, ours, out _));
    Require(ledger.TryRecordInherited(11, "msedge", 150, 10, bg, "BelowNormal", all));
    Require(ledger.Changes.Single(c => c.Pid == 11) is { OriginalAffinity: 0xFFFF, OriginalPriority: "Normal", AppliedAffinity: 0xF000, AppliedPriority: "BelowNormal" });
    Require(ledger.TryRecordInherited(12, "helper", 160, 10, 0x00FF, "BelowNormal", all), "priority alone was inherited");
    Require(ledger.Changes.Single(c => c.Pid == 12) is { OriginalAffinity: 0x00FF, OriginalPriority: "Normal" });
    Require(ledger.TryRecordInherited(13, "grandchild", 170, 11, bg, "Normal", all), "a child's own child");
    Require(!ledger.TryRecordInherited(11, "msedge", 150, 10, bg, "BelowNormal", all), "recorded once");
    Require(!ledger.TryRecordInherited(14, "own", 180, 10, 0x00FF, "High", all), "set by the program itself: left alone");
    Require(!ledger.TryRecordInherited(15, "older", 50, 10, bg, "BelowNormal", all), "started before the recorded parent: a recycled PID");
    Require(!ledger.TryRecordInherited(16, "orphan", 200, 99, bg, "BelowNormal", all), "parent not recorded");
});
Test("Programs Free VRAM closed are reopened once after a helper that was ended", () =>
{
    var app = Make("free-vram-leftover/Fixture Reopen App.exe", "");
    var path = Path.Combine(root, "free-vram-leftover/boost/closed.json"); Directory.CreateDirectory(Path.GetDirectoryName(path)!);
    AtomicFile.WriteText(path, JsonData.Serialize(new[] { new ClosedProcess(500, 1, "Fixture Reopen App", app, "\"" + app + "\" --minimized", true), new ClosedProcess(501, 500, "Fixture Reopen App", app, "\"" + app + "\" --type=renderer", false) }));
    var started = new List<ReopenTarget>();
    var done = BoostRuntime.ReopenClosedFile(path, target => { started.Add(target); return null; });
    Require(started.Count == 1 && started[0].Arguments == "--minimized" && started[0].ImagePath == app, "the program, with its own arguments; never its helper");
    Require(done is { Count: 1 } && !done[0].Contains("--minimized") && !File.Exists(path), "logged without arguments; the record is gone");
    Require(BoostRuntime.ReopenClosedFile(path, _ => throw new Exception("started twice")) is null, "once only");
    Require(BoostRuntime.ClosedPath(root) == Path.Combine(root, "boost", "closed.json"));
});
Test("A desktop Tobii left stopped is offered to the pilot, with an elevated CLI resume", () =>
{
    var state = Path.Combine(root, "tobii-left"); Make("tobii-left/boost/" + TobiiDesktop.MarkerFile, "[\"Fixture Tobii Absent\"]");
    Require(BoostRuntime.TobiiLeftPaused(state) == !BoostRuntime.HelperRunning(), "marker present and no helper");
    Require(!BoostRuntime.TobiiLeftPaused(Path.Combine(root, "tobii-none")));
    var cli = Make("tobii-cli/DcsVr.Cli.exe", "");
    var start = BoostRuntime.TobiiResumeStart(state, Path.GetDirectoryName(cli));
    Require(start is { Verb: "runas", UseShellExecute: true } && start.ArgumentList.SequenceEqual(["tobii-resume", "--state", state]));
    Require(BoostRuntime.TobiiResumeStart(state, Path.Combine(root, "no-cli")) is null);
});
Test("A Pimax server log from a past session is not the live refresh rate", () =>
{
    var runtime = Path.Combine(root, "pimax-stale-runtime"); Directory.CreateDirectory(runtime);
    var log = Path.Combine(runtime, "pvr_srv_log_26-10-01-10-00-00.txt");
    File.WriteAllText(log, string.Concat(Enumerable.Repeat("\n[PSRV] 0 Normal rendering fps:(a:90.02,c:89.996436) Missed:(a:0)", 6)));
    var running = HeadsetRefresh.PimaxRuntimeRunning;
    try
    {
        HeadsetRefresh.PimaxRuntimeRunning = () => false;
        Require(HeadsetRefresh.ReadPimaxRuntime(runtime) is { Hz: 90, Source: "Pimax Play" }, "a log written now is live");
        File.SetLastWriteTimeUtc(log, DateTime.UtcNow.AddHours(-3));
        Require(HeadsetRefresh.ReadPimaxRuntime(runtime) is null, "stale and no Pimax server: the session report is used instead");
        HeadsetRefresh.PimaxRuntimeRunning = () => true;
        Require(HeadsetRefresh.ReadPimaxRuntime(runtime) is { Hz: 90, Source: "Pimax Play, last session" }, "stale while the server runs: labelled as the last session");
    }
    finally { HeadsetRefresh.PimaxRuntimeRunning = running; }
});
Test("Small fixes: DST-safe DCS lifetime, Sboys \"1.3\", the SteamVR log read once per change", () =>
{
    // Process.StartTime is local; the helper passes UTC now. An hour of offset (or a DST change) must not matter.
    var startLocal = DateTime.Now.AddSeconds(-25);
    Require(BoostAttach.Choose([new DcsCandidate(7, startLocal, null)], null, null, TimeSpan.FromSeconds(20), DateTime.UtcNow) == 7);
    Require(BoostAttach.Choose([new DcsCandidate(7, DateTime.Now.AddSeconds(-5), null)], null, null, TimeSpan.FromSeconds(20), DateTime.UtcNow) is null);
    InventorySnapshot With(string version) => new() { Drivers = [new("CustomHeadsetOpenVR", root, true, false, version)] };
    Require(GazeBridge.RuntimeProvidesGaze(With("1.3")) && GazeBridge.RuntimeProvidesGaze(With("2.0")) && !GazeBridge.RuntimeProvidesGaze(With("1.2")));
    var log = Make("hmd-cache/vrserver.txt", "x [Info] - Active HMD set to CustomHeadsetOpenVR.AAAA\n");
    var stamp = DateTime.UtcNow.AddMinutes(-1); File.SetLastWriteTimeUtc(log, stamp);
    Require(SetupDetection.LastActiveHmd(log) == "CustomHeadsetOpenVR.AAAA");
    File.WriteAllText(log, "x [Info] - Active HMD set to CustomHeadsetOpenVR.BBBB\n"); File.SetLastWriteTimeUtc(log, stamp);
    Require(SetupDetection.LastActiveHmd(log) == "CustomHeadsetOpenVR.AAAA", "same length and time: not read again");
    File.AppendAllText(log, "y\n");
    Require(SetupDetection.LastActiveHmd(log) == "CustomHeadsetOpenVR.BBBB", "a changed log is read again");
});

// ---- End-to-end scenarios: a whole fake PC per test (DCS install, Saved Games, OpenXR runtimes, distribution, state) ----
// Only fixture folders under the test root are written; the guard against a running DCS compares these paths only.
const string E2eLua = "options = {\n\t[\"graphics\"] = {\n\t\t[\"maxFPS\"] = 120,\n\t\t[\"sync\"] = true,\n\t\t[\"Upscaling\"] = \"TAA\",\n\t\t[\"width\"] = 2560,\n\t\t[\"height\"] = 1440,\n\t\t[\"fullScreen\"] = true,\n\t\t[\"aspect\"] = 1.7777777777778,\n\t},\n\t[\"miscellaneous\"] = {\n\t\t[\"launcher\"] = true,\n\t},\n\t[\"VR\"] = {\n\t\t[\"enable\"] = false,\n\t\t[\"openxr_quadView\"] = false,\n\t\t[\"custom_IPD_enable\"] = true,\n\t\t[\"custom_IPD\"] = 63.5,\n\t},\n}\n";
E2eFixture E2e(string name)
{
    var prefix = "e2e/" + name; var dist = Path.Combine(root, prefix, "dist");
    void Component(string relative, string content) { var path = Make(prefix + "/dist/components/" + relative, content); AtomicFile.WriteText(path + ".sha256", Hashing.FileSha256(path)); }
    foreach (var (folder, kind) in new[] { ("cheeky", "upstream"), ("cheeky-focus", "focus") })
    {
        Component(folder + "/dxgi.dll", "MZ CheekyFoveatedDLSS " + kind + " loader (fixture, never loaded)");
        Component(folder + "/CheekyFoveatedDLSS/CheekyFoveatedDLSSHost.dll", "MZ " + kind + " host (fixture)");
        Component(folder + "/CheekyFoveatedDLSS/CheekyFoveatedDLSSRuntime.dll", "MZ " + kind + " runtime (fixture)");
    }
    Component("CheekyOpenXRLayer.dll", "MZ cheeky layer (fixture)"); Component("ofxr/XR_APILAYER_XRFrameBridge_diagnostic.dll", "MZ ofxr fork (fixture)");
    Component("dcsqvcull/DcsQvCull.dll", "MZ engine loader (fixture)"); Component("dcsqvcull/DcsQvCullPayload.dll", "MZ engine payload (fixture)"); Component("dcsqvcull/DcsQvCull.lua", "-- engine hook (fixture)");
    Component("pupilshift/XR_APILAYER_DCSVR_pupil_shift.dll", "MZ pupil shift (fixture)");
    var quad = Path.Combine(dist, "components/quadviews"); Directory.CreateDirectory(quad);
    foreach (var file in new[] { "XR_APILAYER_MBUCCHIA_quad_views_foveated.dll", "openxr-api-layer.json", "settings.cfg" })
    { File.Copy(Path.Combine(workspace, "external/quadviews/bin/x64/Release", file), Path.Combine(quad, file)); File.WriteAllText(Path.Combine(quad, file + ".sha256"), Hashing.FileSha256(Path.Combine(quad, file))); }
    var exe = Make(prefix + "/game/bin/DCS.exe", "Never execute this fixture"); Make(prefix + "/game/bin/edCore.dll", "MZ DCS's own library (fixture)");
    var options = Make(prefix + "/sg/DCS/Config/options.lua", E2eLua); Make(prefix + "/sg/DCS/Scripts/Hooks/UserHook.lua", "-- the pilot's own hook");
    var pimax = Make(prefix + "/pimax/runtime.json", "{\"runtime\":{\"library_path\":\"runtime.dll\"}}"); Make(prefix + "/pimax/runtime.dll", "Never load this fixture");
    var steam = Make(prefix + "/steamvr/steamxr_win64.json", "{\"runtime\":{\"library_path\":\"runtime.dll\"}}"); Make(prefix + "/steamvr/runtime.dll", "Never load this fixture");
    return new(new ControlService(dist, Path.Combine(root, prefix, "state")), Path.GetFullPath(Path.Combine(root, prefix)), Path.GetFullPath(exe), Path.GetFullPath(options), pimax, steam);
}
// The inventory the app captures: the selected DCS files and the DCS settings it reads from options.lua.
InventorySnapshot E2eInventory(E2eFixture f)
{
    var lua = new LuaOptions(File.ReadAllText(f.Options)); var settings = new Dictionary<string, string>();
    foreach (var key in new[] { "VR.openxr_quadView", "VR.openxr_eyeGaze", "graphics.Upscaling", "graphics.maxFPS", "graphics.sync", "VR.custom_IPD_enable", "VR.custom_IPD" })
        if (lua.Get(key.Split('.')) is { } raw) settings[key] = raw;
    return new() { DcsExecutable = f.Exe, OptionsPath = f.Options, PimaxRuntime = f.Pimax, SteamVrRuntime = f.SteamVr, DcsSettings = settings };
}
// Every file (with its hash) and folder below the given folders of the fixture.
SortedDictionary<string, string> E2eTree(E2eFixture f, params string[] folders)
{
    var tree = new SortedDictionary<string, string>(StringComparer.OrdinalIgnoreCase);
    foreach (var folder in folders.Select(x => Path.Combine(f.Base, x)).Where(Directory.Exists))
    {
        foreach (var dir in Directory.GetDirectories(folder, "*", SearchOption.AllDirectories)) tree[Path.GetRelativePath(f.Base, dir) + "\\"] = "dir";
        foreach (var file in Directory.GetFiles(folder, "*", SearchOption.AllDirectories)) tree[Path.GetRelativePath(f.Base, file)] = Hashing.FileSha256(file);
    }
    return tree;
}
string E2eDiff(SortedDictionary<string, string> expected, SortedDictionary<string, string> actual) =>
    string.Join("; ", expected.Keys.Union(actual.Keys).Where(k => expected.GetValueOrDefault(k) != actual.GetValueOrDefault(k)).Select(k => $"{k}: {expected.GetValueOrDefault(k) ?? "absent"} -> {actual.GetValueOrDefault(k) ?? "absent"}"));
// After an apply: the launch contract holds, every installed file is what a fresh preview of the profile plans, nothing of an
// earlier profile is left in DCS's folders or the managed folder, and options.lua holds exactly the owned settings (the
// settings an earlier profile owned are back to the given originals).
void E2eCheckApplied(E2eFixture f, VrProfile profile, string step, SortedDictionary<string, string> baseTree, HashSet<string> ownedEver, LuaOptions? originals = null)
{
    Require(f.Service.Originals.Status() is { State: "applied" }, step + ": not applied");
    f.Service.PrepareLaunchApplied();
    var journal = f.Service.Originals.ReadCurrent()!; var installed = journal.Entries.Select(e => e.Path).ToHashSet(StringComparer.OrdinalIgnoreCase);
    var plan = f.Service.Preview(profile, E2eInventory(f));
    Require(plan.Files.Select(p => Path.GetFullPath(p.Path)).ToHashSet(StringComparer.OrdinalIgnoreCase).SetEquals(installed), step + ": the applied files are not the plan's");
    foreach (var file in plan.Files.Where(p => p.LuaChanges is null))
        Require(File.ReadAllBytes(file.Path).AsSpan().SequenceEqual(file.Content), step + ": installed file differs from the plan: " + file.Path);
    var managed = f.Service.ManagedRoot;
    var strayManaged = Directory.Exists(managed) ? Directory.GetFiles(managed, "*", SearchOption.AllDirectories).Where(p => !installed.Contains(p)).ToArray() : [];
    Require(strayManaged.Length == 0, step + ": files of an earlier profile left in the managed folder: " + string.Join(", ", strayManaged));
    var strayGame = E2eTree(f, "game", "sg").Where(p => !p.Key.EndsWith('\\') && !installed.Contains(Path.Combine(f.Base, p.Key)) && baseTree.GetValueOrDefault(p.Key) != p.Value && !p.Key.EndsWith("options.lua")).Select(p => p.Key).ToArray();
    Require(strayGame.Length == 0, step + ": files of an earlier profile left in DCS's folders: " + string.Join(", ", strayGame));
    var lua = new LuaOptions(File.ReadAllText(f.Options)); originals ??= new LuaOptions(E2eLua);
    var owned = journal.Entries.Where(e => e.LuaChanges is not null).SelectMany(e => e.LuaChanges!).ToArray();
    foreach (var change in owned)
        Require(LuaOptions.NormalizeLiteral(lua.Get(change.Path.Split('.'))) == LuaOptions.NormalizeLiteral(change.InstalledRaw), $"{step}: {change.Path} is {lua.Get(change.Path.Split('.'))}, not {change.InstalledRaw}");
    foreach (var key in ownedEver.Except(owned.Select(c => c.Path)))
        Require(LuaOptions.NormalizeLiteral(lua.Get(key.Split('.'))) == LuaOptions.NormalizeLiteral(originals.Get(key.Split('.'))), $"{step}: {key}, owned by an earlier profile only, is {lua.Get(key.Split('.'))}, not the original {originals.Get(key.Split('.'))}");
    ownedEver.UnionWith(owned.Select(c => c.Path));
}
// After Back to stock DCS: nothing recorded, no backup or kept copy left, and nothing (not even an empty folder) in the
// managed folder.
void E2eCheckClean(E2eFixture f, string step)
{
    Require(f.Service.Originals.Status() is { State: "clean", Count: 0, Current: null }, step + ": originals still recorded");
    var leftovers = Directory.GetFiles(f.Service.Originals.Directory).Where(p => Path.GetExtension(p) is ".original" or ".replaced").ToArray();
    Require(leftovers.Length == 0, step + ": backups left: " + string.Join(", ", leftovers));
    var managed = f.Service.ManagedRoot;
    var left = Directory.Exists(managed) ? Directory.GetFileSystemEntries(managed, "*", SearchOption.AllDirectories) : [];
    Require(left.Length == 0, step + ": left in the managed folder: " + string.Join(", ", left));
}
VrProfile E2eA() => ProfileValidation.ResolveFeatures(new VrProfile { Id = "e2e-a", Name = "A · stereo, Foveated SR", QuadViews = QuadProvider.None, FoveatedDlss = true, FoveaSource = FoveaSource.Profile, RefreshFromHeadset = false });
VrProfile E2eB(string? neural) => ProfileValidation.ResolveFeatures(new VrProfile
{
    Id = "e2e-b", Name = "B · Quad Views, DLSS 5, frame generation", QuadViews = QuadProvider.QuadViewsFoveated, NeuralRendering = neural is not null, NeuralRuntimePath = neural, FocusDlssPreset = neural is null ? 0 : 11,
    FrameGen = FrameGeneration.Nvidia, FrameGenFactor = 2, FpsLimit = FpsLimitMode.MatchRefresh, HeadsetRefreshHz = 90, RefreshFromHeadset = false, DisableDcsVSync = true, EngineOptimizations = true, FoveaSource = FoveaSource.Profile
});
VrProfile E2eC(string? neural) => ProfileValidation.ResolveFeatures(E2eB(neural) with { Id = "e2e-c", Name = "C · optimizations only", Desktop = true });
VrProfile E2eD() => ProfileValidation.ResolveFeatures(new VrProfile
{
    Id = "e2e-d", Name = "D · Sboys", Runtime = RuntimeKind.SboysSteamVr, Tracking = TrackingKind.Lighthouse, QuadViews = QuadProvider.QuadViewsFoveated, Gaze = GazeMode.Fixed,
    FrameGen = FrameGeneration.FidelityFx, SmallDcsWindow = true, FpsLimit = FpsLimitMode.Custom, RenderedFpsCap = 40, FoveaSource = FoveaSource.Profile, RefreshFromHeadset = false
});
Test("E2E: switching A, B, C, D, B and Back to stock DCS installs each plan exactly and leaves no trace", () =>
{
    var neural = RealNeuralRuntime(); if (neural is null) Console.WriteLine("NOTE e2e switching: no nvngx_dlssnr.dll, B runs without DLSS 5");
    var f = E2e("switching"); var baseTree = E2eTree(f, "game", "sg"); var lua = File.ReadAllBytes(f.Options); var owned = new HashSet<string>();
    foreach (var (step, profile) in new[] { ("A", E2eA()), ("B", E2eB(neural)), ("C", E2eC(neural)), ("D", E2eD()), ("B again", E2eB(neural)) })
    {
        var sync = f.Service.SyncApplied(profile, E2eInventory(f));
        Require(sync.Kind == LaunchSyncKind.Applied && sync.Notes is not { Count: > 0 }, step + ": " + sync.Message);
        E2eCheckApplied(f, profile, step, baseTree, owned);
        Require(f.Service.SyncApplied(profile, E2eInventory(f)).Kind == LaunchSyncKind.Unchanged, step + ": a second launch rewrites the profile");
        // Optimizations only leaves options.lua as it was but for the DCS launcher, which a launch from the app always skips.
        if (step == "C") Require(File.ReadAllText(f.Options) == E2eLua.Replace("[\"launcher\"] = true", "[\"launcher\"] = false"), "Optimizations only changes nothing but the launcher: " + File.ReadAllText(f.Options));
        if (step == "D") Require(new LuaOptions(File.ReadAllText(f.Options)) is var d && d.Get("graphics", "width") == "1280" && d.Get("graphics", "fullScreen") == "false" && d.Get("graphics", "maxFPS") == "40", "D's window and cap");
    }
    Require(f.Service.RestoreOriginals().Complete);
    Require(File.ReadAllBytes(f.Options).AsSpan().SequenceEqual(lua), "options.lua is byte for byte the original");
    var after = E2eTree(f, "game", "sg"); Require(after.SequenceEqual(baseTree), "DCS's folders differ: " + E2eDiff(baseTree, after));
    E2eCheckClean(f, "Back to stock DCS");
});
Test("E2E: an apply or a Back to stock DCS cut short is finished by the next launch or restore", () =>
{
    var neural = RealNeuralRuntime();
    var f = E2e("interrupted"); var baseTree = E2eTree(f, "game", "sg"); var lua = File.ReadAllBytes(f.Options); var owned = new HashSet<string>();
    f.Service.SyncApplied(E2eA(), E2eInventory(f));
    E2eCheckApplied(f, E2eA(), "A", baseTree, owned);
    // Cut short half-way through writing B over A.
    var plan = f.Service.Preview(E2eB(neural), E2eInventory(f));
    try { f.Service.Originals.Apply(plan, i => { if (i == plan.Files.Count / 2) throw new IOException("Injected failure"); }); throw new Exception("No failure"); }
    catch (IOException e) when (e.Message.Contains("Injected failure")) { }
    Require(f.Service.Originals.Status() is { State: "applying", Current: null } && f.Service.ReadAppliedProfile() is null, "Nothing counts as applied");
    Throws<InvalidOperationException>(() => f.Service.PrepareLaunchApplied());
    var (sync, start) = f.Service.SyncAndPrepareLaunch(E2eB(neural), E2eInventory(f));
    Require(sync.Kind == LaunchSyncKind.Applied && start.FileName == f.Exe, sync.Message);
    E2eCheckApplied(f, E2eB(neural), "B after the interrupted apply", baseTree, owned);
    // Back to stock DCS cut short: a file held open by another program cannot be removed.
    RestoreResult HeldRestore(string held) { using (new FileStream(held, FileMode.Open, FileAccess.Read, FileShare.Read)) return f.Service.RestoreOriginals(); }
    var launch = f.Service.Originals.ReadCurrent()!.Entries.Single(e => Path.GetFileName(e.Path) == "launch.json").Path;
    var partial = HeldRestore(launch);
    Require(!partial.Complete && partial.Conflicts.Single().StartsWith(launch) && f.Service.Originals.Status() is { State: "restoring", Count: 1 }, "Only the held file is left: " + string.Join("; ", partial.Conflicts));
    Throws<InvalidOperationException>(() => f.Service.PrepareLaunchApplied());
    // Launch after the half restore: A is written over what is left.
    var again = f.Service.SyncAndPrepareLaunch(E2eA(), E2eInventory(f)).Sync;
    Require(again.Kind == LaunchSyncKind.Applied && !File.Exists(launch), again.Message);
    owned.Clear(); E2eCheckApplied(f, E2eA(), "A after the interrupted restore", baseTree, owned);
    // Cut short again, then Back to stock DCS once more finishes it.
    var aLaunch = f.Service.Originals.ReadCurrent()!.Entries.Single(e => Path.GetFileName(e.Path) == "launch.json").Path;
    Require(!HeldRestore(aLaunch).Complete);
    Require(f.Service.RestoreOriginals().Complete);
    Require(File.ReadAllBytes(f.Options).AsSpan().SequenceEqual(lua), "options.lua is byte for byte the original");
    var after = E2eTree(f, "game", "sg"); Require(after.SequenceEqual(baseTree), "DCS's folders differ: " + E2eDiff(baseTree, after));
    E2eCheckClean(f, "Back to stock DCS after an interrupted restore");
});
Test("E2E: the pilot's edits between launches (options.lua, another dxgi.dll, a deleted file, Pimax Play, DCS's custom IPD) are handled", () =>
{
    var neural = RealNeuralRuntime();
    var f = E2e("edits"); var baseTree = E2eTree(f, "game", "sg");
    var pimax = Make("e2e/edits/pimax-play/global.json", PimaxJson(1, .66, .20, .66, .66));
    var previous = PimaxFovea.SettingsPath; PimaxFovea.SettingsPath = pimax;
    try
    {
        var profile = E2eB(neural) with { FoveaSource = FoveaSource.PimaxPlay, PupilShift = true, Gaze = GazeMode.EyeTracked, EyeDistance = EyeDistanceSource.DcsCustom };
        Require(f.Service.SyncApplied(profile, E2eInventory(f)).Kind == LaunchSyncKind.Applied);
        var journal = f.Service.Originals.ReadCurrent()!;
        string Installed(string name) => f.Service.Originals.ReadCurrent()!.Entries.Single(e => Path.GetFileName(e.Path) == name).Path;
        Require(File.ReadAllText(Installed("PupilShift.ini")).Contains("Ipd=63.5") && new LuaOptions(File.ReadAllText(f.Options)).Get("VR", "custom_IPD_enable") == "false", "The layer takes over DCS's custom IPD");
        // DCS rewrote Max FPS (owned) and the pilot changed the resolution (not owned).
        AtomicFile.WriteText(f.Options, new LuaOptions(new LuaOptions(File.ReadAllText(f.Options)).SetLiteral(["graphics", "maxFPS"], "144")).SetLiteral(["graphics", "width"], "1920"));
        var realigned = f.Service.SyncApplied(profile, E2eInventory(f));
        Require(realigned.Kind == LaunchSyncKind.SettingsRealigned && new LuaOptions(File.ReadAllText(f.Options)) is var l1 && l1.Get("graphics", "maxFPS") == "45" && l1.Get("graphics", "width") == "1920", realigned.Message);
        // The pilot set another IPD in DCS (DCS turns its custom IPD on again).
        AtomicFile.WriteText(f.Options, new LuaOptions(new LuaOptions(File.ReadAllText(f.Options)).SetLiteral(["VR", "custom_IPD_enable"], "true")).SetLiteral(["VR", "custom_IPD"], "66"));
        var ipd = f.Service.SyncApplied(profile, E2eInventory(f));
        Require(ipd.Kind == LaunchSyncKind.Applied && ipd.Message.Contains("custom IPD") && File.ReadAllText(Installed("PupilShift.ini")).Contains("Ipd=66")
            && new LuaOptions(File.ReadAllText(f.Options)).Get("VR", "custom_IPD_enable") == "false", ipd.Message);
        E2eCheckApplied(f, profile, "after the IPD change", baseTree, [], new LuaOptions(E2eLua.Replace("63.5", "66")));
        // Another program's dxgi.dll (ReShade) installed over the DLSS 5 loader.
        var dxgi = Path.Combine(Path.GetDirectoryName(f.Exe)!, "dxgi.dll"); var dxgi2 = Path.Combine(Path.GetDirectoryName(f.Exe)!, "dxgi2.dll");
        if (neural is not null)
        {
            File.WriteAllText(dxgi, "MZ ReShade 6.0 (fixture)");
            var reshade = f.Service.SyncApplied(profile, E2eInventory(f));
            Require(reshade.Kind == LaunchSyncKind.Applied && reshade.Notes!.Any(n => n.Contains("dxgi.dll")) && File.ReadAllText(dxgi2) == "MZ ReShade 6.0 (fixture)" && File.ReadAllText(dxgi).Contains("focus loader"), reshade.Message);
        }
        // A managed file deleted (an antivirus): the next launch writes it again.
        var manifest = Installed("ofxr.json"); File.Delete(manifest);
        Require(f.Service.SyncApplied(profile, E2eInventory(f)).Kind == LaunchSyncKind.Applied && File.Exists(manifest));
        // Pimax Play's focus changed.
        File.WriteAllText(pimax, PimaxJson(0, .33, .33, .33, .33));
        var pimaxSync = f.Service.SyncApplied(profile, E2eInventory(f));
        Require(pimaxSync.Kind is LaunchSyncKind.FoveaUpdated or LaunchSyncKind.Applied && File.ReadAllText(Installed("settings.cfg")).Contains("horizontal_focus_section=0.67"), pimaxSync.Message);
        f.Service.PrepareLaunchApplied();
        // Another profile, then Back to stock DCS: the pilot's edits and ReShade stay, owned settings go back.
        Require(f.Service.SyncApplied(E2eA(), E2eInventory(f)).Kind == LaunchSyncKind.Applied);
        Require(f.Service.RestoreOriginals().Complete);
        var lua = new LuaOptions(File.ReadAllText(f.Options));
        Require(lua.Get("graphics", "maxFPS") == "120" && lua.Get("graphics", "width") == "1920" && lua.Get("VR", "custom_IPD_enable") == "true" && lua.Get("VR", "custom_IPD") == "66"
            && lua.Get("graphics", "Upscaling") == "\"TAA\"" && lua.Get("VR", "enable") == "false" && lua.Get("miscellaneous", "launcher") == "true", File.ReadAllText(f.Options));
        Require(neural is null ? !File.Exists(dxgi) : File.ReadAllText(dxgi) == "MZ ReShade 6.0 (fixture)", "ReShade is the pilot's dxgi.dll now");
        var expected = new SortedDictionary<string, string>(baseTree, StringComparer.OrdinalIgnoreCase);
        if (neural is not null) expected[Path.GetRelativePath(f.Base, dxgi)] = Hashing.FileSha256(dxgi);
        var after = E2eTree(f, "game", "sg"); after.Remove(Path.GetRelativePath(f.Base, f.Options)); expected.Remove(Path.GetRelativePath(f.Base, f.Options));
        Require(after.SequenceEqual(expected), "DCS's folders differ: " + E2eDiff(expected, after));
        E2eCheckClean(f, "Back to stock DCS after edits");
    }
    finally { PimaxFovea.SettingsPath = previous; }
});
Test("E2E: state written by 0.5.10 (no lastInstalled, an older profile.json) keeps launching, takes a new profile and restores", () =>
{
    var neural = RealNeuralRuntime();
    var f = E2e("upgrade"); var baseTree = E2eTree(f, "game", "sg"); var lua = File.ReadAllBytes(f.Options);
    var old = E2eA() with { Id = "pimax-old", Name = "0.5.10 profile" };
    f.Service.SyncApplied(old, E2eInventory(f));
    // Shape the state as 0.5.10 left it: profile.json without the fields added since, baseline.json without lastInstalled.
    void MakeOld()
    {
        var baselinePath = Path.Combine(f.Service.Originals.Directory, "baseline.json");
        var baseline = System.Text.Json.Nodes.JsonNode.Parse(File.ReadAllText(baselinePath))!.AsObject(); baseline.Remove("lastInstalled");
        if (baseline["current"] is System.Text.Json.Nodes.JsonObject current)
            foreach (var entry in current["entries"]!.AsArray().Select(e => e!.AsObject()).Where(e => Path.GetFileName((string)e["path"]!) == "profile.json"))
            {
                var path = (string)entry["path"]!; var saved = System.Text.Json.Nodes.JsonNode.Parse(File.ReadAllText(path))!.AsObject();
                foreach (var key in new[] { "focusDlssPreset", "engineShadowCache" }) Require(saved.Remove(key), "profile.json has " + key);
                AtomicFile.WriteText(path, saved.ToJsonString(JsonData.Options)); entry["installedSha256"] = Hashing.FileSha256(path);
            }
        AtomicFile.WriteText(baselinePath, baseline.ToJsonString(JsonData.Options));
    }
    MakeOld();
    Require(f.Service.ReadAppliedProfile() is { Profile.FocusDlssPreset: 0 } applied && applied.Profile.Id == "pimax-old", "The older profile.json reads");
    f.Service.PrepareLaunchApplied();
    Require(f.Service.SyncApplied(null, E2eInventory(f)).Kind == LaunchSyncKind.Unchanged, "The CLI launch of the applied profile writes nothing");
    Require(f.Service.SyncApplied(f.Service.ReadAppliedProfile()!.Profile, E2eInventory(f)).Kind == LaunchSyncKind.Unchanged, "The app's draft loaded from it writes nothing");
    var owned = new HashSet<string>();
    var b = f.Service.SyncApplied(E2eB(neural), E2eInventory(f));
    Require(b.Kind == LaunchSyncKind.Applied && b.ReplacedProfileId == "pimax-old", b.Message);
    E2eCheckApplied(f, E2eB(neural), "B over the 0.5.10 profile", baseTree, owned);
    // 0.5.10 interrupted an apply: "applying", no applied profile, no lastInstalled.
    var plan = f.Service.Preview(E2eA(), E2eInventory(f));
    try { f.Service.Originals.Apply(plan, i => { if (i == 2) throw new IOException("Injected failure"); }); } catch (IOException) { }
    MakeOld();
    Require(f.Service.Originals.Status() is { State: "applying" });
    Require(f.Service.RestoreOriginals().Complete);
    Require(File.ReadAllBytes(f.Options).AsSpan().SequenceEqual(lua), "options.lua is byte for byte the original");
    var after = E2eTree(f, "game", "sg"); Require(after.SequenceEqual(baseTree), "DCS's folders differ: " + E2eDiff(baseTree, after));
    E2eCheckClean(f, "Back to stock DCS from 0.5.10 state");
});
Test("E2E: the CLI's apply, launch-check and restore write exactly what the app writes", () =>
{
    var cli = Path.Combine(workspace, "src/DcsVr.Cli/bin/Release/net10.0-windows/DcsVr.Cli.exe");
    if (!File.Exists(cli)) { Console.WriteLine("SKIP CLI parity: build src/DcsVr.Cli in Release first"); return; }
    var neural = RealNeuralRuntime();
    var f = E2e("cli"); var baseTree = E2eTree(f, "game", "sg"); var lua = File.ReadAllBytes(f.Options);
    (int Code, string Output) Cli(params string[] arguments)
    {
        var start = new ProcessStartInfo(cli) { UseShellExecute = false, RedirectStandardOutput = true, RedirectStandardError = true, CreateNoWindow = true };
        foreach (var argument in arguments.Concat(["--state", f.Service.StateRoot, "--distribution", f.Service.DistributionRoot])) start.ArgumentList.Add(argument);
        using var process = Process.Start(start)!;
        var output = process.StandardOutput.ReadToEndAsync(); var error = process.StandardError.ReadToEndAsync();
        if (!process.WaitForExit(120000)) { process.Kill(true); throw new Exception("CLI timed out"); }
        return (process.ExitCode, output.Result + error.Result);
    }
    foreach (var (step, draft) in new[] { ("A", E2eA() with { RuntimeManifestPath = f.Pimax }), ("B", E2eB(neural) with { RuntimeManifestPath = f.Pimax }), ("D", E2eD() with { RuntimeManifestPath = f.SteamVr }) })
    {
        // The app: Apply without launching.
        Require(f.Service.SyncApplied(draft, E2eInventory(f)).Kind == LaunchSyncKind.Applied);
        var app = E2eTree(f, "game", "sg", "state/managed"); var appStatus = f.Service.Originals.Status();
        Require(f.Service.RestoreOriginals().Complete);
        // The CLI, with the same profile file.
        var file = Path.Combine(f.Base, step + ".json"); f.Service.SaveProfile(draft, file);
        var applied = Cli("apply", "--profile", file, "--dcs", f.Exe, "--options", f.Options);
        Require(applied.Code == 0, step + ": " + applied.Output);
        var cliTree = E2eTree(f, "game", "sg", "state/managed");
        Require(cliTree.SequenceEqual(app), step + ": the CLI wrote other files than the app: " + E2eDiff(app, cliTree));
        var cliStatus = f.Service.Originals.Status();
        Require(cliStatus.Count == appStatus.Count && cliStatus.Files.Select(i => i.Path + i.Action + i.Foreign).SequenceEqual(appStatus.Files.Select(i => i.Path + i.Action + i.Foreign)), step + ": other originals");
        var check = Cli("launch-check"); Require(check.Code == 0 && check.Output.Contains(f.Exe), step + ": " + check.Output);
        f.Service.PrepareLaunchApplied();
        Require(f.Service.SyncApplied(draft, E2eInventory(f)).Kind == LaunchSyncKind.Unchanged, step + ": the app sees the CLI's apply as its own");
        var restored = Cli("restore"); Require(restored.Code == 0, step + ": " + restored.Output);
        Require(File.ReadAllBytes(f.Options).AsSpan().SequenceEqual(lua) && E2eTree(f, "game", "sg").SequenceEqual(baseTree), step + ": the CLI's restore");
        E2eCheckClean(f, step + " CLI restore");
    }
});
Test("E2E: CPU Boost and Free VRAM records left by an ended helper survive a moment when the file cannot be read", () =>
{
    // Another program (an antivirus) holds the record open for reading but allows deletion: the record must not be lost.
    var state = PathPolicy.UnderRoot(root, "e2e/boost-left/state"); Directory.CreateDirectory(Path.Combine(state, "boost"));
    using var self = Process.GetCurrentProcess();
    var affinity = (ulong)(long)self.ProcessorAffinity; var priority = self.PriorityClass.ToString();
    // This process, recorded with the values it has now: restoring it changes nothing.
    AtomicFile.WriteText(BoostRuntime.LedgerPath(state), JsonData.Serialize(new[] { new BoostChange(self.Id, self.ProcessName, self.StartTime.Ticks, affinity, priority, affinity, priority) }));
    using (new FileStream(BoostRuntime.LedgerPath(state), FileMode.Open, FileAccess.Read, FileShare.Delete)) BoostRuntime.RestoreLedgerFile(BoostRuntime.LedgerPath(state));
    Require(File.Exists(BoostRuntime.LedgerPath(state)), "The ledger is kept for the next start");
    Require(BoostRuntime.RestoreLedgerFile(BoostRuntime.LedgerPath(state)) == 1 && !File.Exists(BoostRuntime.LedgerPath(state)), "and restored then");
    var app = Make("e2e/boost-left/Fixture Reopen App.exe", "");
    AtomicFile.WriteText(BoostRuntime.ClosedPath(state), JsonData.Serialize(new[] { new ClosedProcess(500, 1, "Fixture Reopen App", app, "\"" + app + "\"", true) }));
    var started = new List<ReopenTarget>();
    using (new FileStream(BoostRuntime.ClosedPath(state), FileMode.Open, FileAccess.Read, FileShare.Delete)) BoostRuntime.ReopenClosedFile(BoostRuntime.ClosedPath(state), t => { started.Add(t); return null; });
    Require(started.Count == 0 && File.Exists(BoostRuntime.ClosedPath(state)), "The closed programs are kept for the next start");
    Require(BoostRuntime.ReopenClosedFile(BoostRuntime.ClosedPath(state), t => { started.Add(t); return null; }) is { Count: 1 } && started.Count == 1 && !File.Exists(BoostRuntime.ClosedPath(state)));
    // A damaged record cannot be used: it goes, nothing is touched.
    File.WriteAllText(BoostRuntime.LedgerPath(state), "{ damaged");
    Require(BoostRuntime.RestoreLedgerFile(BoostRuntime.LedgerPath(state)) == 0 && !File.Exists(BoostRuntime.LedgerPath(state)));
});

Test("E2E: a DCS install removed from its folder drops its entries at Back to stock DCS; a root missing down to the drive is kept and reported", () =>
{
    // (a) The install's bin folder deleted while its parent still exists (DCS uninstalled or moved).
    var game = Path.GetFullPath(Path.Combine(root, "e2e/root-gone/game")); var bin = Path.Combine(game, "bin"); Directory.CreateDirectory(bin);
    var config = Path.GetFullPath(Path.Combine(root, "e2e/root-gone/sg/Config")); var kept = Make("e2e/root-gone/sg/Config/settings.cfg", "original");
    var store = new OriginalsStore(Path.Combine(root, "e2e/root-gone/state"));
    store.Apply(Plan("p", Write(Path.Combine(bin, "dxgi.dll"), "loader", bin), Write(Path.Combine(bin, "Sub/host.dll"), "host", bin), Write(kept, "changed", config)));
    Directory.Delete(bin, true);
    Require(store.RestoreOriginals().Complete && store.Status() is { Count: 0, State: "clean" } && !Directory.Exists(bin) && File.ReadAllText(kept) == "original", "Dropped, nothing rebuilt; the rest restored");
    // (b) A root whose whole path is missing down to the drive (a volume mounted elsewhere, a removed top folder): kept and
    // reported, never dropped. Recorded by hand in the baseline: nothing is ever written outside the fixture.
    var drive = Path.GetPathRoot(Path.GetFullPath(root))!; var top = Path.Combine(drive, "NoSuchTop-" + Guid.NewGuid().ToString("N")); var missingBin = Path.Combine(top, "bin");
    Require(!Directory.Exists(top));
    var other = new OriginalsStore(Path.Combine(root, "e2e/root-missing/state")); var local = Make("e2e/root-missing/bin/a.cfg", "A");
    other.Apply(Plan("p", Write(local, "a1", Path.GetDirectoryName(local))));
    var baselinePath = Path.Combine(other.Directory, "baseline.json");
    var baseline = System.Text.Json.Nodes.JsonNode.Parse(File.ReadAllText(baselinePath))!.AsObject();
    var files = baseline["files"]!.AsArray();
    files.Add(System.Text.Json.Nodes.JsonNode.Parse(JsonData.Serialize(new OriginalFile { Path = Path.Combine(missingBin, "dxgi.dll"), Root = missingBin })));
    AtomicFile.WriteText(baselinePath, baseline.ToJsonString(JsonData.Options));
    var result = other.RestoreOriginals();
    Require(!Directory.Exists(top), "Nothing created at the drive's root");
    Require(!result.Complete && result.Conflicts.Single().StartsWith(Path.Combine(missingBin, "dxgi.dll")) && other.Status() is { Count: 1, State: "restoring" } && File.ReadAllText(local) == "A", "Kept and reported: " + string.Join("; ", result.Conflicts));
});
foreach (var (name, action) in tests)
{
    try { action(); Console.WriteLine("PASS " + name); }
    catch (Exception e) { failed.Add(name); Console.WriteLine("FAIL " + name + "\n" + e); }
}
var report = new { total = tests.Count, passed = tests.Count - failed.Count, failed, durationSeconds = timer.Elapsed.TotalSeconds, fixtureDirectory = root };
Console.WriteLine(JsonData.Serialize(report));
// A fully passing run leaves nothing behind (each run used to add a fixture folder); a failing one keeps it to look at.
if (failed.Count == 0) try { Directory.Delete(root, true); } catch (Exception e) when (e is IOException or UnauthorizedAccessException) { }
else AtomicFile.WriteText(Path.Combine(root, "result.json"), JsonData.Serialize(report));
return failed.Count == 0 ? 0 : 1;

/// <summary>One end-to-end fixture: the service on its own distribution and state, and the fake PC's DCS and runtime files.</summary>
sealed record E2eFixture(ControlService Service, string Base, string Exe, string Options, string Pimax, string SteamVr);

/// <summary>A primary 3840×2160 144 Hz display for the monitor mode tests; records every call.</summary>
sealed class FakeDisplays : IDisplayModes
{
    public SavedDisplayMode Mode = new(3840, 2160, 144, 32, 0, 0);
    public bool Refuse, RefuseExact;
    public int Sets, Resets;
    private readonly SavedDisplayMode _saved = new(3840, 2160, 144, 32, 0, 0);
    public string? PrimaryDevice() => @"\\.\DISPLAY1";
    public string Name(string device) => "Fixture monitor (" + device + ")";
    public SavedDisplayMode? Current(string device) => device == @"\\.\DISPLAY1" ? Mode : null;
    public IReadOnlyList<DisplayMode> Modes(string device) => [new(3840, 2160, 144), new(3840, 2160, 60), new(2560, 1440, 120), new(1920, 1080, 144), new(1920, 1080, 60), new(1280, 720, 60)];
    public bool Set(string device, SavedDisplayMode mode)
    {
        if (Refuse || RefuseExact && mode == _saved) return false;
        Sets++; Mode = mode; return true;
    }
    public bool Reset(string device) { if (Refuse) return false; Resets++; Mode = _saved; return true; }
}

sealed class FixtureHttpHandler(byte[] bytes) : HttpMessageHandler
{
    protected override Task<HttpResponseMessage> SendAsync(HttpRequestMessage request, CancellationToken cancellationToken)
        => Task.FromResult(new HttpResponseMessage(System.Net.HttpStatusCode.OK) { Content = new ByteArrayContent(bytes) });
}

/// <summary>Fixed answers by URL for the update tests; records each request and its User-Agent. Offline throws like a
/// PC without network.</summary>
sealed class UrlFixtureHandler(Dictionary<string, byte[]> answers) : HttpMessageHandler
{
    public Dictionary<string, byte[]> Answers { get; } = answers;
    public List<string> Requests { get; } = [];
    public List<string> UserAgents { get; } = [];
    public bool Offline;
    protected override Task<HttpResponseMessage> SendAsync(HttpRequestMessage request, CancellationToken cancellationToken)
    {
        if (Offline) throw new HttpRequestException("No network (test fixture).");
        var url = request.RequestUri!.AbsoluteUri; Requests.Add(url); UserAgents.Add(request.Headers.UserAgent.ToString());
        return Task.FromResult(Answers.TryGetValue(url, out var bytes) ? new HttpResponseMessage(System.Net.HttpStatusCode.OK) { Content = new ByteArrayContent(bytes) } : new HttpResponseMessage(System.Net.HttpStatusCode.NotFound));
    }
}
