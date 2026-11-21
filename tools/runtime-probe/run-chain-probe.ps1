# Runs the runtime probe in chain mode (tools\runtime-probe\CHAIN.md): the probe plays the game, loads the Quad Views
# layer (and optionally the frame-generation layer under it) itself, submits four views and reads the Pimax mirrors;
# then the analysis. Output: artifacts\runtime-probe\<timestamp>-<label>\.
#
# This script does NOT start, stop or configure Pimax and does not touch the headset's settings: the caller owns that
# (Pimax running, headset awake on its stand in a lit room, screen saver off). It refuses to run while DCS.exe runs.
# Nothing is installed or registered, and the layers' own folders are not written to: the settings and the
# frame-generation layer are staged in the output folder, and the environment is set for the probe process only.
#
#   tools\runtime-probe\run-chain-probe.ps1 -QuadViews <folder> -Settings <file> -Label <text> [-FrameGen <folder>] [-TwoLayers]
#       [-Motion | -MotionOnly] [-MotionPhases 8] [-MotionFrames 24] [-Quick] [-Format 29]
#       [-MotionPattern lattice|scene] [-MotionDiag] [-MotionSeed <n>] [-Triple] [-AsyncArm]
#       [-QuadSet key=value,...] [-FrameGenSet key=value,...] [-OutRoot <dir>]
param(
  [Parameter(Mandatory)][string]$QuadViews,   # folder with XR_APILAYER_MBUCCHIA_quad_views_foveated.dll (and its openxr-api-layer.json)
  [Parameter(Mandatory)][string]$Settings,    # Quad Views settings file; a profile file written by DCS Control works as it is
  [Parameter(Mandatory)][string]$Label,
  [string]$FrameGen,                          # folder with XR_APILAYER_XRFrameBridge_diagnostic.dll and ofxr_bridge.ini
  [switch]$TwoLayers,                         # dcsvr_two_layers=1 in Quad Views, and foveated_layers=1 in the frame-generation layer
  [switch]$Motion, [switch]$MotionOnly, [switch]$Quick,
  [int]$MotionPhases = 8, [int]$MotionFrames = 24, [int]$Format = 29,
  # scene: a textured world that does not repeat, with markers, swept there and back (CHAIN.md). With it -MotionPhases
  # is the length of the sweep and defaults to the probe's 32; -MotionDiag adds an upward component to the motion.
  [ValidateSet('lattice', 'scene')][string]$MotionPattern = 'lattice', [switch]$MotionDiag, [uint32]$MotionSeed = 0,
  [switch]$Triple,                            # [ofxr] triple_frame_gen=1: 3X (two generated frames per application frame)
  [switch]$AsyncArm,                          # [ofxr] async_arm=1: frame generation armed on a thread of its own
  [switch]$AllowUntracked,                    # probe_run --allow-untracked: the new Pimax runtime (2.0.5) composites the probe untracked
  # Appended to the staged Quad Views settings, after everything else (last wins).
  [string[]]$QuadSet = @('turbo_mode=0', 'force_no_eye_tracking=1', 'dcsvr_far_depth=0'),
  # Written into [ofxr] of the staged ofxr_bridge.ini, for example 'triple_frame_gen=1'.
  [string[]]$FrameGenSet = @(),
  [string]$OutRoot)
$ErrorActionPreference = 'Stop'
# pwsh -File hands a list over as one string: split it.
$QuadSet = @($QuadSet | ForEach-Object { $_ -split ',' } | Where-Object { $_ }); $FrameGenSet = @($FrameGenSet | ForEach-Object { $_ -split ',' } | Where-Object { $_ })
$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
$bin = Join-Path $repo 'tools\runtime-probe\bin'
foreach ($exe in 'probe_run.exe', 'probe_analyze.exe') { if (-not (Test-Path (Join-Path $bin $exe))) { throw "missing ${exe}: run tools\runtime-probe\build.bat" } }
if (Get-Process DCS -ErrorAction SilentlyContinue) { throw 'DCS.exe is running: the chain probe does not run beside it' }
if (-not (Get-Process pi_server -ErrorAction SilentlyContinue)) { throw 'Pimax is not running (no pi_server). Start it and wake the headset first: this script leaves Pimax alone.' }
if ($Label -notmatch '^[A-Za-z0-9_.-]+$') { throw 'Label: letters, digits, dot, dash and underscore only' }
if (($Triple -or $AsyncArm) -and -not $FrameGen) { throw '-Triple and -AsyncArm are keys of the frame-generation layer: they need -FrameGen' }
if ($MotionPattern -eq 'scene' -and -not ($Motion -or $MotionOnly)) { throw '-MotionPattern scene needs -Motion or -MotionOnly' }
if ($MotionDiag -and $MotionPattern -ne 'scene') { throw '-MotionDiag needs -MotionPattern scene' }

$qvDir = (Resolve-Path $QuadViews).Path
$qvManifest = Join-Path $qvDir 'openxr-api-layer.json'
$qvDll = Join-Path $qvDir 'XR_APILAYER_MBUCCHIA_quad_views_foveated.dll'
if (-not (Test-Path $qvDll)) { throw "no Quad Views DLL in $qvDir" }
$qvLayer = if (Test-Path $qvManifest) { $qvManifest } else { $qvDll }
$settingsPath = (Resolve-Path $Settings).Path
$fgDir = $null
if ($FrameGen) {
  $fgDir = (Resolve-Path $FrameGen).Path
  foreach ($f in 'XR_APILAYER_XRFrameBridge_diagnostic.dll', 'ofxr_bridge.ini') { if (-not (Test-Path (Join-Path $fgDir $f))) { throw "no $f in $fgDir" } }
}

if (-not $OutRoot) { $OutRoot = Join-Path $repo 'artifacts\runtime-probe' }
$out = Join-Path $OutRoot ("{0}-{1}" -f (Get-Date -Format 'yyyyMMdd-HHmmss'), $Label)
New-Item -ItemType Directory -Force $out | Out-Null
$log = Join-Path $out 'runner.log'
function Say([string]$m) { $line = "$(Get-Date -Format 'HH:mm:ss') $m"; Add-Content $log $line; $line }

# Quad Views settings, staged. The layer reads settings.cfg beside its DLL first, then this file (DCSVR_QUAD_SETTINGS).
# A DCS Control profile keeps its keys under [exe:DCS], which would not apply to probe_run.exe: those headers are
# renamed. The closing section holds the mode and the keys the probe needs.
$staged = Join-Path $out 'quad-settings.cfg'
$lines = @(Get-Content $settingsPath) | ForEach-Object { if ($_ -match '^\[exe:DCS[^\]]*\]\s*$') { '[exe:probe_run]' } else { $_ } }
$lines += '', '# added by run-chain-probe.ps1', '[exe:probe_run]'
$lines += "dcsvr_two_layers=$(if ($TwoLayers) { 1 } else { 0 })"
# QuadSet last, so it can override the mode (dcsvr_two_layers=2, the array swapchains DCS Control uses).
$lines += $QuadSet
Set-Content -Path $staged -Value $lines -Encoding ascii
Say "Quad Views: $qvLayer"
Say "settings: $settingsPath -> $staged (two layers: $([bool]$TwoLayers); added: $($QuadSet -join ', '))"

# Frame-generation layer, staged: the layer reads ofxr_bridge.ini beside its own DLL and writes its log there.
$fgLayer = $null
if ($fgDir) {
  $fgStage = Join-Path $out 'framegen'
  New-Item -ItemType Directory -Force $fgStage | Out-Null
  Copy-Item (Join-Path $fgDir 'XR_APILAYER_XRFrameBridge_diagnostic.dll') $fgStage
  $ini = [System.Collections.Generic.List[string]]@(Get-Content (Join-Path $fgDir 'ofxr_bridge.ini'))
  function Set-Ini([string]$section, [string]$key, [string]$value) {
    $start = -1; $end = $ini.Count
    for ($i = 0; $i -lt $ini.Count; $i++) {
      if ($ini[$i] -match '^\s*\[(.+)\]\s*$') { if ($start -ge 0) { $end = $i; break }; if ($Matches[1] -eq $section) { $start = $i } }
    }
    if ($start -lt 0) { $ini.Add("[$section]"); $ini.Add("$key=$value"); return }
    for ($i = $start + 1; $i -lt $end; $i++) { if ($ini[$i] -match "^\s*$([regex]::Escape($key))\s*=") { $ini[$i] = "$key=$value"; return } }
    $ini.Insert($start + 1, "$key=$value")
  }
  # A tray-managed install carries control_event: without the tray's event the layer would stay off.
  for ($i = $ini.Count - 1; $i -ge 0; $i--) { if ($ini[$i] -match '^\s*control_event\s*=') { $ini.RemoveAt($i) } }
  Set-Ini 'ofxr' 'enabled' '1'
  Set-Ini 'ofxr' 'd3d11_bridge' '1'
  Set-Ini 'ofxr' 'foveated_layers' $(if ($TwoLayers) { '1' } else { '0' })
  Set-Ini 'diagnostics' 'logging_enabled' '1'
  # The shortcuts first, so that an explicit -FrameGenSet for the same key still wins.
  if ($Triple) { Set-Ini 'ofxr' 'triple_frame_gen' '1' }
  if ($AsyncArm) { Set-Ini 'ofxr' 'async_arm' '1' }
  foreach ($kv in $FrameGenSet) { $k, $v = $kv -split '=', 2; if (-not $k -or $null -eq $v) { throw "FrameGenSet: '$kv' is not key=value" }; Set-Ini 'ofxr' $k.Trim() $v.Trim() }
  Set-Content -Path (Join-Path $fgStage 'ofxr_bridge.ini') -Value $ini -Encoding ascii
  $fgLayer = Join-Path $fgStage 'XR_APILAYER_XRFrameBridge_diagnostic.dll'
  Say "frame generation: $fgDir staged in $fgStage (foveated_layers=$(if ($TwoLayers) { 1 } else { 0 }); triple: $([bool]$Triple); async arm: $([bool]$AsyncArm); set: $($FrameGenSet -join ', '))"
}

# Application -> Quad Views -> frame generation -> runtime: the order of DCS Control's own chain.
$probeArgs = @("`"$out`"", '--quad', '--layer', "`"$qvLayer`"")
if ($fgLayer) { $probeArgs += '--layer', "`"$fgLayer`"" }
if ($Quick) { $probeArgs += '--quick' }
if ($MotionOnly) { $probeArgs += '--motion-only' } elseif ($Motion) { $probeArgs += '--motion' }
if ($MotionPattern -eq 'scene') {
  # Frames shown per application frame, for the analysis: 1 without frame generation, 2 (2X) or 3 (-Triple) with it.
  # If -FrameGenSet changes the mode (triple_frame_gen, adaptive_frame_gen) the staged ini decides, not this number.
  $fgFactor = if (-not $fgLayer) { 1 } elseif ($Triple -or ($FrameGenSet -match '^\s*triple_frame_gen\s*=\s*1\s*$')) { 3 } else { 2 }
  $probeArgs += '--motion-pattern', 'scene', '--motion-frames', $MotionFrames, '--motion-fg', $fgFactor
  if ($PSBoundParameters.ContainsKey('MotionPhases')) { $probeArgs += '--motion-phases', $MotionPhases }
  if ($MotionDiag) { $probeArgs += '--motion-diag' }
  if ($MotionSeed) { $probeArgs += '--motion-seed', $MotionSeed }
} elseif ($Motion -or $MotionOnly) { $probeArgs += '--motion-phases', $MotionPhases, '--motion-frames', $MotionFrames }
$probeArgs += '--format', $Format
if ($AllowUntracked) { $probeArgs += '--allow-untracked' }
Say ("probe_run " + ($probeArgs -join ' '))

$qvLogDir = Join-Path $env:LOCALAPPDATA 'Quad-Views-Foveated'
$code = 99
# After a headset restart the runtime refuses sessions for a while (exit 5): a few more tries, 15 s apart.
for ($attempt = 1; $attempt -le 8; $attempt++) {
  $psi = [System.Diagnostics.ProcessStartInfo]::new((Join-Path $bin 'probe_run.exe'), ($probeArgs -join ' '))
  $psi.UseShellExecute = $false; $psi.CreateNoWindow = $true; $psi.WorkingDirectory = $out
  $psi.EnvironmentVariables['DCSVR_QUAD_SETTINGS'] = $staged   # this process only
  $p = [System.Diagnostics.Process]::Start($psi)
  if (-not $p.WaitForExit(30 * 60 * 1000)) { Say 'probe timed out after 30 min: stopping it'; $p.Kill(); $code = 98; break }
  $code = $p.ExitCode
  Say "probe exit code $code (attempt $attempt)"
  if ($code -ne 5) { break }
  Say 'the runtime is not ready for a session yet: again in 15 s'; Start-Sleep 15
}
switch ($code) {
  2 { Say 'session or chain failure: see probe_run.log (and the layers'' logs below).' }
  3 { Say 'NOT TRACKED, or the layer gave no focus view: nothing was measured. Light the room; check the Quad Views log.' }
  4 { Say 'no mirror image from the compositor: nothing or only part was measured.' }
}
# The layers' logs and the compositor's, copied for the record (read only).
$qvLog = Join-Path $qvLogDir 'Quad-Views-Foveated.log'
if (Test-Path $qvLog) { Copy-Item $qvLog (Join-Path $out 'quad-views.log') }
$srv = Get-ChildItem "$env:LOCALAPPDATA\Pimax\runtime\pvr_srv_log_*.txt" -ErrorAction SilentlyContinue | Sort-Object LastWriteTime -Descending | Select-Object -First 1
if ($srv) { Copy-Item $srv.FullName (Join-Path $out 'pvr_srv_log.txt') }
& (Join-Path $bin 'probe_analyze.exe') $out | Add-Content $log
Say "report: $(Join-Path $out 'report.md')"
exit $code
