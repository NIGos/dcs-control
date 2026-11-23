#Requires -Version 7
# Release flight matrix: one command that flies the FIXED matrix of matrix.json unattended (ATMOSX and stock DCS x a dense
# and a sparse scene; per scene one DCS start running the engine's in-flight test suite with one A/B per engine part)
# and writes results.json + REPORT.md; it then runs the release gate (compare-matrix.ps1, against -Baseline if given).
# TEST-ONLY.
#
#   pwsh -File tools\release-matrix\run-matrix.ps1 -DryRun [-Cli <package>\files\DcsVr.Cli.exe]     checks only, nothing started
#   pwsh -File tools\release-matrix\run-matrix.ps1 -Cli <package>\files\DcsVr.Cli.exe -Label 0.7.0 [-Baseline <results.json>]
#
# Run it from a normal PowerShell 7 window at normal priority (DCS inherits the priority; the Claude sandbox redirects
# %LOCALAPPDATA% and the CLI refuses to run there). README.md has the details. Where things are: parameter, else
# environment variable, else the default resolved at run time (nothing of one machine is written here):
#   -Cli            DCSC_CLI              DcsVr.Cli.exe of the package under test; default the installed app's
#   -OutDir         DCSC_MATRIX_OUT       default <main checkout>\artifacts\release-matrix\runs\<yyyyMMdd-HHmm>[-dry]
#   -MissionDir     DCSC_MATRIX_MISSIONS  the matrix's .miz files (checked by sha256); default <main checkout>\artifacts\release-matrix\missions
#   -TapDir         DCSC_RIG_TAP_DIR      the rig's capture layer (simulated gaze); default <main checkout>\artifacts\rig\capture-layer\bin
#   -SavedGamesDcs  DCSC_SAVED_GAMES_DCS  default %USERPROFILE%\Saved Games\DCS
#   -AtmosxBackup   DCSC_ATMOSX_BACKUP    the ATMOSX installer's own backup folder; default <Saved Games DCS>\ATMOSX Backup
#   -PimaxCtl       DCSC_PIMAX_CTL        pimax_ctl.exe; default tools\runtime-probe\bin of this checkout, else of the main one
#   -PimaxClient    DCSC_PIMAX_CLIENT     default %ProgramFiles%\Pimax\PimaxClient\pimaxui\PimaxClient.exe
#   -QuietLock      DCSC_QUIET_LOCK       default <main checkout>\artifacts\.lane\quiet.lock
#   -Payload        DCSC_MATRIX_PAYLOAD   a DcsQvCullPayload.dll to fly instead of the one the package installs (a build to test)
# The machine is changed only as the live rounds did it, and put back in `finally`: the app's profile (applied with the
# CLI, a test COPY of it flown, the user's profile re-applied), ATMOSX (off for the stock scenes from the mod's own
# backup, back and hash-verified), the Pimax screen-saver key (0 during the run, 180 after), Pimax on then off (service
# procedure). Never: files written by hand under Saved Games\DCS\Scripts, Export.lua, Windows / driver / NVIDIA / Pimax
# Play settings, DCS graphics options (ATMOSX's own visibRange value aside), input or focus to DCS, a headset reboot.
# Exit: 0 done and gate passed; 4 done, gate failed; 1 a scene failed / something to put back by hand; 2 not started
# (checks failed); 3 no quiet machine in time (nothing changed).
param([string]$Matrix, [string[]]$Scenes, [string]$Label, [string]$Baseline, [switch]$DryRun, [int]$QuietWaitMin = 60,
  [string]$Cli = $env:DCSC_CLI, [string]$OutDir = $env:DCSC_MATRIX_OUT, [string]$MissionDir = $env:DCSC_MATRIX_MISSIONS,
  [string]$TapDir = $env:DCSC_RIG_TAP_DIR, [string]$SavedGamesDcs = $env:DCSC_SAVED_GAMES_DCS, [string]$AtmosxBackup = $env:DCSC_ATMOSX_BACKUP,
  [string]$PimaxCtl = $env:DCSC_PIMAX_CTL, [string]$PimaxClient = $env:DCSC_PIMAX_CLIENT, [string]$QuietLock = $env:DCSC_QUIET_LOCK,
  [string]$Payload = $env:DCSC_MATRIX_PAYLOAD)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'matrix-lib.ps1')
if ($Scenes) { $Scenes = @($Scenes | ForEach-Object { $_ -split ',' } | ForEach-Object { $_.Trim() } | Where-Object { $_ }) }

# ---- where things are ---------------------------------------------------------------------------------------------
$Tool = $PSScriptRoot
$Top = [IO.Path]::GetFullPath((Join-Path $Tool '..\..'))
$Main = Get-MainCheckout $Tool
$Runner = Join-Path $Top 'tools\rig\run-scenario.ps1'
if (-not $Matrix) { $Matrix = Join-Path $Tool 'matrix.json' }
$stamp = Get-Date -Format 'yyyyMMdd-HHmm'
if (-not $OutDir) { $OutDir = Join-Path $Main "artifacts\release-matrix\runs\$stamp$(if ($DryRun) { '-dry' })" }
$OutDir = [IO.Path]::GetFullPath($OutDir)
if (-not $MissionDir) { $MissionDir = Join-Path $Main 'artifacts\release-matrix\missions' }
if (-not $TapDir) { $TapDir = Join-Path $Main 'artifacts\rig\capture-layer\bin' }
if (-not $SavedGamesDcs) { $SavedGamesDcs = Join-Path $env:USERPROFILE 'Saved Games\DCS' }
if (-not $AtmosxBackup) { $AtmosxBackup = Join-Path $SavedGamesDcs 'ATMOSX Backup' }
if (-not $PimaxCtl) { $PimaxCtl = @((Join-Path $Top 'tools\runtime-probe\bin\pimax_ctl.exe'), (Join-Path $Main 'tools\runtime-probe\bin\pimax_ctl.exe')) | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1; if (-not $PimaxCtl) { $PimaxCtl = Join-Path $Main 'tools\runtime-probe\bin\pimax_ctl.exe' } }
if (-not $PimaxClient) { $PimaxClient = Join-Path $env:ProgramFiles 'Pimax\PimaxClient\pimaxui\PimaxClient.exe' }
if (-not $QuietLock) { $QuietLock = Join-Path $Main 'artifacts\.lane\quiet.lock' }
if (-not $Cli) { $Cli = Join-Path $env:LOCALAPPDATA 'Programs\DcsControl\files\DcsVr.Cli.exe' }
$Cli = [IO.Path]::GetFullPath($Cli)
$State = Join-Path $env:LOCALAPPDATA 'DcsControl'
$ModDir = Join-Path $SavedGamesDcs 'Scripts\DcsQvCull'
$StateFiles = 'draft.json', 'live.json', 'settings.json', 'presets\active.txt', 'presets\Default.json'
$BuildProcs = 'cl', 'link', 'ninja', 'cmake', 'ctest', 'MSBuild'
New-Item -ItemType Directory -Force $OutDir | Out-Null
$Steps = Join-Path $OutDir 'steps.log'
function Note([string]$s) { $l = "{0:yyyy-MM-dd HH:mm:ss} {1}" -f (Get-Date), $s; Add-Content -LiteralPath $Steps -Value $l; Write-Host $l }
function Sha([string]$p) { if (Test-Path -LiteralPath $p -PathType Leaf) { (Get-FileHash -LiteralPath $p -Algorithm SHA256).Hash.ToLowerInvariant() } else { 'absent' } }
function Write-Utf8([string]$p, [string]$t) { [IO.File]::WriteAllText($p, $t, (New-Object Text.UTF8Encoding($false))) }
function Read-Shared([string]$p) { try { $fs = [IO.File]::Open($p, 'Open', 'Read', 'ReadWrite,Delete'); try { (New-Object IO.StreamReader($fs)).ReadToEnd() } finally { $fs.Dispose() } } catch { '' } }
function Cli-Status { try { & $Cli status 2>&1 | Out-String | ConvertFrom-Json } catch { $null } }
function Cli-Version { try { [string](& $Cli help 2>&1 | Select-Object -First 1) } catch { "(CLI did not run: $_)" } }

# ---- ATMOSX (from the installer's own backup folder; no installer run) -----------------------------------------------
function Get-AtmosxFiles { $j = Join-Path $AtmosxBackup 'atmosx_managed_files.json'; if (Test-Path -LiteralPath $j) { @((Get-Content -LiteralPath $j -Raw | ConvertFrom-Json).Files) } else { $null } }
function Get-AtmosxState($Files, [string]$DcsRoot, [string]$Opt) {
  $mod = 0; $orig = 0
  foreach ($f in $Files) {
    $h = Sha (Join-Path $DcsRoot $f.DestinationRelativePath)
    if ($f.OriginalExisted) { if ($h -eq ([string]$f.OriginalSha256).ToLowerInvariant()) { $orig++ } else { $mod++ } } elseif ($h -ne 'absent') { $mod++ } else { $orig++ }
  }
  $ultra = if (Test-Path -LiteralPath $Opt) { ([regex]::Matches([IO.File]::ReadAllText($Opt), '\["visibRange"\] = "ATMOSXUltra"')).Count } else { -1 }
  [pscustomobject]@{ Installed = ($mod -gt 0); ModFiles = $mod; StockFiles = $orig; UltraLines = $ultra }
}
function Get-AtmosxSnapshot($Files, [string]$DcsRoot, [string]$Opt) { $o = [ordered]@{}; foreach ($f in $Files) { $o[[string]$f.DestinationRelativePath] = Sha (Join-Path $DcsRoot $f.DestinationRelativePath) }; $o['options.lua'] = Sha $Opt; $o }
function Set-AtmosxOff($Files, [string]$DcsRoot, [string]$Opt, [string]$Work) {
  if (Get-Process DCS -ErrorAction SilentlyContinue) { throw 'ATMOSX off: DCS is running' }
  $save = Join-Path $Work 'atmosx-mod'; $bad = 0
  foreach ($f in $Files) {
    $dst = Join-Path $DcsRoot $f.DestinationRelativePath; $keep = Join-Path $save $f.DestinationRelativePath
    New-Item -ItemType Directory -Force (Split-Path $keep) | Out-Null
    if ($f.OriginalExisted) {
      $src = Join-Path $AtmosxBackup $f.BackupRelativePath
      if ((Sha $src) -ne ([string]$f.OriginalSha256).ToLowerInvariant()) { throw "ATMOSX backup hash mismatch: $($f.BackupRelativePath)" }
      if (Test-Path -LiteralPath $dst) { Copy-Item -LiteralPath $dst $keep -Force }
      Copy-Item -LiteralPath $src $dst -Force
      if ((Sha $dst) -ne ([string]$f.OriginalSha256).ToLowerInvariant()) { $bad++ }
    } elseif (Test-Path -LiteralPath $dst) { Move-Item -LiteralPath $dst $keep -Force }
  }
  $t = [IO.File]::ReadAllText($Opt); $n = ([regex]::Matches($t, '\["visibRange"\] = "ATMOSXUltra"')).Count
  if ($n -eq 1) { [IO.File]::WriteAllText($Opt, $t.Replace('["visibRange"] = "ATMOSXUltra"', '["visibRange"] = "Extreme"'), (New-Object Text.UTF8Encoding($false))) }
  elseif ($n -gt 1) { throw "options.lua has $n ATMOSXUltra visibRange lines" }
  "ATMOSX off: $($Files.Count) entries, $bad hash mismatches after the copy; visibRange $(if ($n -eq 1) { 'ATMOSXUltra -> Extreme' } else { 'not ATMOSXUltra, left' })"
}
function Set-AtmosxOn($Files, [string]$DcsRoot, [string]$Opt, [string]$Work, $Record) {
  $save = Join-Path $Work 'atmosx-mod'
  foreach ($f in $Files) { $dst = Join-Path $DcsRoot $f.DestinationRelativePath; $keep = Join-Path $save $f.DestinationRelativePath; if (Test-Path -LiteralPath $keep) { Copy-Item -LiteralPath $keep $dst -Force } }
  Copy-Item -LiteralPath (Join-Path $Work 'options-saved.lua') $Opt -Force
  $now = Get-AtmosxSnapshot $Files $DcsRoot $Opt; $diff = @($now.Keys | Where-Object { $now[$_] -ne $Record[$_] })
  [pscustomobject]@{ Compared = $now.Count; Different = $diff }
}

# ---- machine state: what the run may touch, as group|key -> hash ----------------------------------------------------
function Get-MachineState([string]$DcsRoot, [string]$Opt, $AtxFiles) {
  $o = [ordered]@{}
  $app = Join-Path $env:LOCALAPPDATA 'Programs\DcsControl'
  foreach ($f in @(Get-ChildItem -LiteralPath $app -Recurse -File -Force -ErrorAction SilentlyContinue)) { $o["app|$($f.FullName.Substring($app.Length + 1))"] = Sha $f.FullName }
  foreach ($f in @(Get-ChildItem -LiteralPath $State -Recurse -File -Force -ErrorAction SilentlyContinue | Where-Object { $_.Name -ne 'app.log' -and $_.FullName -notlike '*\webview\*' -and $_.FullName -notlike '*\managed\profiles\*\*.log' })) { $o["state|$($f.FullName.Substring($State.Length + 1))"] = Sha $f.FullName }
  if ($AtxFiles) { foreach ($f in $AtxFiles) { $o["atmosx|$($f.DestinationRelativePath)"] = Sha (Join-Path $DcsRoot $f.DestinationRelativePath) } }
  $o['options|options.lua'] = Sha $Opt
  foreach ($f in @(Get-ChildItem -LiteralPath (Join-Path $SavedGamesDcs 'Config') -File -ErrorAction SilentlyContinue)) { $o["config|$($f.Name)"] = Sha $f.FullName }
  $sc = Join-Path $SavedGamesDcs 'Scripts'
  foreach ($f in @(Get-ChildItem -LiteralPath $sc -Recurse -File -ErrorAction SilentlyContinue | Where-Object { $_.Name -notmatch '\.(log|csv)$' -and $_.Name -notmatch '^report_' })) { $o["scripts|$($f.FullName.Substring($sc.Length + 1))"] = Sha $f.FullName }
  $o
}
function Compare-MachineState($A, $B) {
  $keys = @(@($A.Keys) + @($B.Keys) | Sort-Object -Unique)
  $expected = '^(state\|(originals|boost|engine-shader-cache|managed)\\|config\|(InstantActionGeneratorParams\.lua|steam_authdata\.bin)$)'
  @(foreach ($k in $keys) { if ($A[$k] -ne $B[$k]) { [pscustomobject]@{ Key = $k; Before = $(if ($A.Contains($k)) { $A[$k] } else { 'absent' }); After = $(if ($B.Contains($k)) { $B[$k] } else { 'absent' }); Expected = ($k -match $expected) } } })
}

# ---- Pimax ---------------------------------------------------------------------------------------------------------
function Invoke-Elevated([string]$Body, [string]$Name) {
  $f = Join-Path $OutDir "$Name.ps1"; Set-Content -LiteralPath $f -Value $Body
  Start-Process pwsh -Verb RunAs -ArgumentList '-NoProfile', '-WindowStyle', 'Hidden', '-File', "`"$f`"" -Wait -WindowStyle Hidden
}
function Get-SaverKey { if (-not (Get-Process pi_server -ErrorAction SilentlyContinue)) { return $null }; (& $PimaxCtl get runtime_screen_off_timeout_second 2>&1 | Select-Object -Last 1 | Out-String).Trim() }
function Get-SlamLine {
  $lf = Get-ChildItem (Join-Path $env:LOCALAPPDATA 'Pimax\runtime\pvr_srv_log_*.txt') -ErrorAction SilentlyContinue | Sort-Object LastWriteTime -Descending | Select-Object -First 1
  if (-not $lf) { return $null }
  [pscustomobject]@{ File = $lf; Line = ((Read-Shared $lf.FullName) -split "`n" | Select-String 'slam warning flags' | Select-Object -Last 1).Line }
}
function Start-PimaxTracked {
  if ((Get-Service PiServiceLauncher -ErrorAction Stop).Status -ne 'Running') { Invoke-Elevated 'Start-Service PiServiceLauncher' 'pimax-on' }
  Note "PiServiceLauncher $((Get-Service PiServiceLauncher).Status) $((Get-Service PiServiceLauncher).StartType)"
  $t0 = Get-Date
  if (-not (Get-Process PimaxClient -ErrorAction SilentlyContinue)) { Start-Process -FilePath $PimaxClient -WindowStyle Minimized; Note 'PimaxClient started (minimised)' }
  while (-not (Get-Process pi_server -ErrorAction SilentlyContinue)) { if (((Get-Date) - $t0).TotalSeconds -gt 180) { throw 'pi_server did not start within 180 s' }; Start-Sleep 3 }
  Start-Sleep 10
  $Script:SaverFound = Get-SaverKey; Note "screen-saver key as found: $Script:SaverFound"
  & $PimaxCtl set runtime_screen_off_timeout_second 0 2>&1 | Out-Null; $Script:SaverSet = $true; Note "screen-saver key now: $(Get-SaverKey)"
  while (((Get-Date) - $t0).TotalSeconds -lt 300) {
    $s = Get-SlamLine
    if ($s -and $s.File.LastWriteTime -gt $t0 -and $s.Line -match 'flags: 0\s*$') { Note "TRACKED: $($s.Line.Trim())"; return }
    Start-Sleep 3
  }
  throw "headset not tracked within 300 s of power-on (last: $((Get-SlamLine).Line)); light on, headset on its stand facing the room?"
}
function Stop-Pimax {
  Invoke-Elevated @'
Stop-Service PiServiceLauncher -Force
Get-Process PiPlayService,PiPlatformService_64,pi_server,pi_overlay,pi_vst,PimaxClient -ErrorAction SilentlyContinue | Stop-Process -Force
'@ 'pimax-off'
  Start-Sleep 5
  "Pimax off: service $((Get-Service PiServiceLauncher).Status) $((Get-Service PiServiceLauncher).StartType); left: '$((Get-Process PiPlayService,PiPlatformService_64,pi_server,pi_overlay,pi_vst,PimaxClient -ErrorAction SilentlyContinue | ForEach-Object { $_.Name }) -join ' ')'"
}

# ---- the matrix and the plan --------------------------------------------------------------------------------------
$M = Get-Content -LiteralPath $Matrix -Raw | ConvertFrom-Json
$problems = [Collections.Generic.List[string]]::new(); $checks = [Collections.Generic.List[string]]::new()
foreach ($p in (Test-MatrixDefinition $M)) { $problems.Add("matrix: $p") }
$plan = @($M.scenes | Where-Object { -not $Scenes -or $_.id -in $Scenes })
foreach ($s in @($Scenes | Where-Object { $_ -and $_ -notin @($M.scenes | ForEach-Object { $_.id }) })) { $problems.Add("scene '$s' is not in the matrix (have: $(($M.scenes | ForEach-Object { $_.id }) -join ', '))") }
# ATMOSX scenes first (the mod as the user has it), then the stock scenes: one ATMOSX change, undone in `finally`
$plan = @($plan | Where-Object { $_.atmosx }) + @($plan | Where-Object { -not $_.atmosx })
$atxScenes = @($plan | Where-Object { $_.atmosx }).Count; $stockScenes = $plan.Count - $atxScenes
$ini = $M.suite.ini
$blocks = [int]$ini.Bench.Blocks; $blockSec = [int]$ini.Bench.BlockSec; $nBench = @($M.parts | Where-Object { $_.kind -eq 'bench' }).Count
# time: per bench blocks x BlockSec + ~6 s; W1 pairs x 2 blocks of 90 frames at 45 fps; DCS start to FLY ~2 min (apply,
# copy, Pimax ready, load); close, logs and the stock restore ~1 min (the live rounds' steps logs of 2026-11-23)
$suiteSec = $nBench * ($blocks * $blockSec + 6) + [int]$ini.Suite.WaterSkipTiming * 2 * 2 + 5 + [int]$ini.Suite.PassTimelineSec + 10
$sceneSec = @($plan | ForEach-Object { 125 + [double]$_.suiteAt + $suiteSec + 65 })
$totalMin = [Math]::Round((($sceneSec | Measure-Object -Sum).Sum + 5 * 60) / 60)
Write-Host "release matrix v$($M.version): $($plan.Count) scene(s): $(($plan | ForEach-Object { "$($_.id) (suite at FLY+$($_.suiteAt) s)" }) -join ', ')"
Write-Host ("suite per scene about {0:N1} min ({1} benches x {2} blocks x {3} s + W1 {4} pairs); estimate {5} min for the whole run (with Pimax on/tracking, backups and the restore)" -f ($suiteSec / 60), $nBench, $blocks, $blockSec, $ini.Suite.WaterSkipTiming, $totalMin)

# ---- checks that need nothing running (the dry run stops after them) ----------------------------------------------
if (-not (Test-Path -LiteralPath $Runner)) { $problems.Add("rig runner missing: $Runner") }
else { $tok = $null; $err = $null; [void][Management.Automation.Language.Parser]::ParseFile($Runner, [ref]$tok, [ref]$err); if ($err.Count) { $problems.Add("rig runner does not parse: $($err[0].Message)") } else { $checks.Add('rig runner parses') } }
$missionPath = @{}
foreach ($mp in $M.missions.PSObject.Properties) {
  $p = Join-Path $MissionDir ([string]$mp.Value.file); $missionPath[$mp.Name] = $p
  if (-not (Test-Path -LiteralPath $p)) { $problems.Add("mission $($mp.Name) not found: $p (-MissionDir or DCSC_MATRIX_MISSIONS)"); continue }
  $h = Sha $p
  if (-not $mp.Value.sha256) { $problems.Add("mission $($mp.Name): no sha256 in the matrix (this one is $h)") }
  elseif ($h -ne ([string]$mp.Value.sha256).ToLowerInvariant()) { $problems.Add("mission $($mp.Name): sha256 $h is not the matrix's $($mp.Value.sha256): another mission, results would not compare") }
  else { $checks.Add("mission $($mp.Name) $($mp.Value.file): sha256 matches") }
}
$cliVersion = $null
if (-not (Test-Path -LiteralPath $Cli)) { $problems.Add("CLI not found: $Cli (-Cli or DCSC_CLI)") }
else {
  $cliVersion = Cli-Version
  if ($cliVersion -notmatch 'DCS Control') { $problems.Add("CLI did not answer 'help' as expected: $cliVersion") } else { $checks.Add("CLI: $cliVersion ($Cli)") }
  if (-not (Test-Path -LiteralPath (Join-Path (Split-Path $Cli) 'DcsVr.Core.dll'))) { $problems.Add('DcsVr.Core.dll is not next to the CLI (the suite is started through it)') }
  $st = Cli-Status; if ($st) { $checks.Add("app state: $($st.state)$(if ($st.profileId) { " ($($st.profileId))" })") } else { $problems.Add('CLI status did not answer (run from a normal PowerShell, not a sandbox)') }
}
if ($Payload -and -not (Test-Path -LiteralPath $Payload)) { $problems.Add("payload to fly not found: $Payload") }
$draftPath = Join-Path $State 'draft.json'; $draft = $null
if (-not (Test-Path -LiteralPath $draftPath)) { $problems.Add("the app's draft.json not found: $draftPath") }
else {
  $draft = Get-Content -LiteralPath $draftPath -Raw | ConvertFrom-Json
  if (-not $draft.profile -or -not $draft.dcs -or -not $draft.options) { $problems.Add('draft.json has no profile / dcs / options') }
  elseif (-not (Test-Path -LiteralPath $draft.dcs)) { $problems.Add("DCS.exe of draft.json not found: $($draft.dcs)") }
  else { $checks.Add("profile $($draft.profile.id); DCS $(Split-Path (Split-Path $draft.dcs))") }
  if ($M.foveatedFrameGen -and $draft.profile.frameGen -eq $false) { $problems.Add('the matrix flies foveated frame generation, the user profile has frame generation off') }
}
foreach ($n in $StateFiles) { if (-not (Test-Path -LiteralPath (Join-Path $State $n))) { $problems.Add("user state file missing: $n") } }
$tapDll = Join-Path $TapDir 'XR_APILAYER_RIG_capture.dll'; $tapJson = Join-Path $TapDir 'XR_APILAYER_RIG_capture.json'
if (-not (Test-Path -LiteralPath $tapDll) -or -not (Test-Path -LiteralPath $tapJson)) { $problems.Add("capture layer not in $TapDir (build it with tools\rig\build-capture-layer.ps1, or -TapDir)") }
elseif ([string]((Get-Content -LiteralPath $tapJson -Raw | ConvertFrom-Json).api_layer.library_path) -ne $tapDll) { $problems.Add('capture layer manifest points elsewhere: rebuild it') }
else { $checks.Add("capture layer: $TapDir") }
if (-not (Test-Path -LiteralPath $PimaxCtl)) { $problems.Add("pimax_ctl.exe not found: $PimaxCtl (tools\runtime-probe\build.bat, or -PimaxCtl)") } else { $checks.Add("pimax_ctl: $PimaxCtl") }
if (-not (Test-Path -LiteralPath $PimaxClient)) { $problems.Add("PimaxClient.exe not found: $PimaxClient") }
if (-not (Get-Service PiServiceLauncher -ErrorAction SilentlyContinue)) { $problems.Add('service PiServiceLauncher not found') }
else { $checks.Add("Pimax service: $((Get-Service PiServiceLauncher).Status) / $((Get-Service PiServiceLauncher).StartType); PimaxClient running: $([bool](Get-Process PimaxClient -ErrorAction SilentlyContinue))") }
$atx = Get-AtmosxFiles; $dcsRoot = if ($draft.dcs) { Split-Path (Split-Path $draft.dcs) } else { $null }; $atxState = $null
if (-not $atx) {
  if ($atxScenes) { $problems.Add("ATMOSX scenes need ATMOSX: no atmosx_managed_files.json in $AtmosxBackup") } else { $checks.Add('ATMOSX: no installer backup: the stock scenes fly DCS as it is') }
} elseif ($dcsRoot) {
  $atxState = Get-AtmosxState $atx $dcsRoot $draft.options
  $bk = @($atx | Where-Object { $_.OriginalExisted -and (Sha (Join-Path $AtmosxBackup $_.BackupRelativePath)) -ne ([string]$_.OriginalSha256).ToLowerInvariant() })
  if ($bk.Count -and $stockScenes -and $atxState.Installed) { $problems.Add("ATMOSX backup: $($bk.Count) original file(s) do not match their recorded hash: cannot turn ATMOSX off safely") }
  if ($atxScenes -and -not $atxState.Installed) { $problems.Add('ATMOSX scenes: ATMOSX is not installed now') }
  if ($atxState.UltraLines -gt 1) { $problems.Add("options.lua has $($atxState.UltraLines) ATMOSXUltra visibRange lines") }
  $checks.Add("ATMOSX: $(if ($atxState.Installed) { 'installed' } else { 'not installed' }) ($($atxState.ModFiles) mod / $($atxState.StockFiles) stock of $($atx.Count) managed files; backup originals $(if ($bk.Count) { "$($bk.Count) BAD" } else { 'all match' }); visibRange ATMOSXUltra lines $($atxState.UltraLines))")
}
$insIni = Join-Path $ModDir 'DcsQvCull.ini'; $insPay = Join-Path $ModDir 'payload\DcsQvCullPayload.dll'   # where the app's Apply puts them (DeploymentPlanner)
$checks.Add("engine module installed now: ini $(Test-Path -LiteralPath $insIni), payload $(Test-Path -LiteralPath $insPay) (the run's first apply installs the package's own)")
if (Get-Process DCS -ErrorAction SilentlyContinue) { $problems.Add('DCS is running') }
$busy = @(Get-Process -ErrorAction SilentlyContinue | Where-Object { $_.ProcessName -in $BuildProcs } | ForEach-Object { $_.ProcessName } | Sort-Object -Unique)
$checks.Add("quiet machine now: $(if ($busy) { 'NO, running: ' + ($busy -join ' ') } else { 'yes' }); quiet lock $(if (Test-Path -LiteralPath $QuietLock) { 'HELD: ' + (Get-Content -LiteralPath $QuietLock -TotalCount 1) } else { 'free' }) ($QuietLock)")
$free = (Get-PSDrive -Name $OutDir.Substring(0, 1)).Free; if ($free -lt 6GB) { $problems.Add("less than 6 GB free on the output drive ($([int]($free / 1GB)) GB)") }
if ($Baseline -and -not (Test-Path -LiteralPath $Baseline)) { $problems.Add("baseline not found: $Baseline") }

# per-scene scenario files for the rig runner, and the runner's own dry run on each
function New-SceneFiles($S, [string]$Dir, [string]$DevIni, [string]$DevPay) {
  New-Item -ItemType Directory -Force $Dir | Out-Null
  $sch = @([ordered]@{ at = [double]$S.suiteAt; do = 'suite' }, [ordered]@{ at = [double]$S.suiteAt + 1; do = 'waitsuite'; seconds = [int]$M.suite.waitSeconds })
  $sc = [ordered]@{ name = "rm_$($S.id -replace '-', '_')"; status = 'ready'; measures = "release matrix v$($M.version), scene $($S.id): the engine test suite with one A/B per part"
    mission = $missionPath[[string]$S.mission]; seconds = [double]$S.suiteAt + 5; windowAtOrigin = $false; simGaze = [string]$M.simGaze
    arms = @([ordered]@{ name = 'suite'; options = @([bool]$M.foveatedFrameGen); seconds = [double]$S.suiteAt + 5; schedule = $sch
        profileOverrides = [ordered]@{ developerOptions = $true; engineDevMode = $true; engineDevIniPath = $DevIni; engineDevPayloadPath = $DevPay } })
    profileOverrides = [ordered]@{}; ofxr = [ordered]@{}; diagnostics = [ordered]@{}; quadviews = [ordered]@{}; schedule = @(); verdict = [ordered]@{ crops = @() }
    sideEffects = @('no input, no focus; rig autostart; simulated gaze; the app''s StartSuite; engine developer mode with a developer ini and payload in the run folder (test copy of the profile only)') }
  $p = Join-Path $Dir 'scenario.json'; Write-Utf8 $p ($sc | ConvertTo-Json -Depth 8); $p
}
function Get-RunnerArgs([string]$ScPath, [string]$Runs, [string]$RigState) {
  @('-NoProfile', '-File', $Runner, '-Scenario', $ScPath, '-StateBackup', $RigState, '-RunsDir', $Runs, '-TapDir', $TapDir, '-SavedGamesDcs', $SavedGamesDcs,
    '-PimaxCtl', $PimaxCtl, '-PimaxClient', $PimaxClient, '-Cli', $Cli, '-QuietLock', $QuietLock, '-NoLock', '-KeepPimax', '-StopOnFailure')
}
$rigState = Join-Path $OutDir 'rig-state'
if ($DryRun) {
  foreach ($n in $StateFiles) { $s = Join-Path $State $n; if (Test-Path -LiteralPath $s) { $to = Join-Path $rigState $n; New-Item -ItemType Directory -Force (Split-Path $to) | Out-Null; Copy-Item -LiteralPath $s $to -Force } }
  if (Test-Path -LiteralPath $Runner) {
    foreach ($s in $plan) {
      $d = Join-Path $OutDir "scenes\$($s.id)"; $sp = New-SceneFiles $s $d (Join-Path $d 'dev\DcsQvCull.ini') (Join-Path $d 'dev\DcsQvCullPayload.dll')
      $o = & pwsh @(Get-RunnerArgs $sp (Join-Path $d 'runs') $rigState) -DryRun 2>&1 | Out-String; $rc = $LASTEXITCODE
      Set-Content -LiteralPath (Join-Path $d 'runner-dryrun.txt') -Value $o
      $pr = @($o -split "`r?`n" | Where-Object { $_ -match '^PROBLEM: ' } | ForEach-Object { $_ -replace '^PROBLEM: ', '' })
      if ($rc -ne 0 -and -not $pr.Count) { $pr = @("runner dry run exit $rc`: $(($o.Trim() -split "`n" | Select-Object -Last 2) -join ' | ')") }
      foreach ($x in $pr) { $problems.Add("runner ($($s.id)): $x") }
      if (-not $pr.Count) { $checks.Add("runner dry run $($s.id): ok ($(($o -split "`r?`n" | Where-Object { $_ -match '^(mission|dry run)' }) -join '; '))") }
    }
  }
  $checks | ForEach-Object { Write-Host "ok   $_" }; $problems | ForEach-Object { Write-Host "PROBLEM $_" }
  $txt = @("release matrix dry run $(Get-Date -Format s)", "out: $OutDir", "estimate: $totalMin min") + @($checks | ForEach-Object { "ok   $_" }) + @($problems | ForEach-Object { "PROBLEM $_" })
  [IO.File]::WriteAllLines((Join-Path $OutDir 'dryrun.txt'), $txt)
  Write-Host "dry run: $($problems.Count) problem(s); nothing applied, nothing started ($OutDir\dryrun.txt)"
  exit $(if ($problems.Count) { 1 } else { 0 })
}
if ($problems.Count) { $problems | ForEach-Object { Write-Host "PROBLEM $_" }; Write-Host 'not started: fix the problems above (run -DryRun to check again)'; exit 2 }

# ---- live run -----------------------------------------------------------------------------------------------------
$res = [ordered]@{ schema = 1; matrix = $M.name; matrixVersion = $M.version; label = $(if ($Label) { $Label } else { $cliVersion }); created = (Get-Date).ToString('s'); partial = $false; partialWhy = $null
  package = [ordered]@{ cliVersion = $cliVersion; coreSha256 = (Sha (Join-Path (Split-Path $Cli) 'DcsVr.Core.dll')); payloadSha256 = $null; payloadFrom = $(if ($Payload) { 'given (-Payload)' } else { 'installed by the package' }); iniSha256 = $null }
  profileId = $draft.profile.id; profileSha256 = $null; dcsVersion = $null; gate = $M.gate; scenes = [Collections.Generic.List[object]]::new(); run = [ordered]@{} }
$lockMine = $false; $lockText = "release matrix (tools\release-matrix\run-matrix.ps1, pid $PID), started $(Get-Date -Format HH:mm), expected $totalMin minutes"
$Script:SaverSet = $false; $Script:SaverFound = $null; $atxOff = $false; $atxRecord = $null; $before = $null; $stBefore = $null; $stop = $null; $gateRc = $null
$saveResults = { Write-Utf8 (Join-Path $OutDir 'results.json') ($res | ConvertTo-Json -Depth 12) }
# 1. a quiet machine, then the lock (never another session's: wait while it exists; create it only when absent)
$t0 = Get-Date
while ($true) {
  $busy = @(Get-Process -ErrorAction SilentlyContinue | Where-Object { $_.ProcessName -in $BuildProcs -or $_.ProcessName -like 'xrfg*' } | ForEach-Object { $_.ProcessName } | Sort-Object -Unique)
  $held = Test-Path -LiteralPath $QuietLock
  if (-not $busy -and -not $held) {
    try { [IO.Directory]::CreateDirectory((Split-Path $QuietLock)) | Out-Null; $fs = [IO.File]::Open($QuietLock, 'CreateNew', 'Write'); $w = New-Object IO.StreamWriter($fs); $w.WriteLine($lockText); $w.Dispose(); $lockMine = $true; break }
    catch [IO.IOException] { }   # taken between the look and the create: wait again
  }
  if (((Get-Date) - $t0).TotalMinutes -gt $QuietWaitMin) { Note "not a quiet machine within $QuietWaitMin min (busy: $($busy -join ' '); lock: $(if ($held) { Get-Content -LiteralPath $QuietLock -TotalCount 1 })): nothing changed, not flying"; exit 3 }
  Write-Host "waiting for a quiet machine (busy: $($busy -join ' '); lock: $(if ($held) { Get-Content -LiteralPath $QuietLock -TotalCount 1 } else { 'free' }))"; Start-Sleep 30
}
Note "quiet machine after $([int]((Get-Date) - $t0).TotalSeconds) s; quiet lock taken: $lockText"
try {
  # 2. record the state as found
  robocopy $State (Join-Path $OutDir 'state-backup\DcsControl') /MIR /COPY:DAT /DCOPY:T /R:1 /W:1 /XF app.log /XD webview /NFL /NDL /NJH /NJS /NP | Out-Null; Note "state backup (robocopy exit $LASTEXITCODE)"
  foreach ($n in $StateFiles) { $to = Join-Path $rigState $n; New-Item -ItemType Directory -Force (Split-Path $to) | Out-Null; Copy-Item -LiteralPath (Join-Path $State $n) $to -Force }
  $draft = Get-Content -LiteralPath (Join-Path $rigState 'draft.json') -Raw | ConvertFrom-Json
  $before = Get-MachineState $dcsRoot $draft.options $atx; $before.GetEnumerator() | ForEach-Object { "$($_.Key)`t$($_.Value)" } | Set-Content (Join-Path $OutDir 'state-before.txt')
  $stBefore = Cli-Status
  $res.run.asFound = [ordered]@{ appState = $stBefore.state; profileId = $stBefore.profileId; pimaxClient = [bool](Get-Process PimaxClient -ErrorAction SilentlyContinue); pimaxService = "$((Get-Service PiServiceLauncher).Status)"; atmosx = $(if ($atxState) { $atxState.Installed } else { $false }) }
  Note "as found: app $($stBefore.state) $($stBefore.profileId); Pimax service $($res.run.asFound.pimaxService), client $($res.run.asFound.pimaxClient); ATMOSX $($res.run.asFound.atmosx); $($before.Count) state entries recorded"
  if ($atx -and $atxState.Installed) {
    $atxRecord = Get-AtmosxSnapshot $atx $dcsRoot $draft.options; $atxRecord.GetEnumerator() | ForEach-Object { "$($_.Key)`t$($_.Value)" } | Set-Content (Join-Path $OutDir 'atmosx-before.txt')
    Copy-Item -LiteralPath $draft.options (Join-Path $OutDir 'options-saved.lua') -Force; Note "ATMOSX recorded: $($atxRecord.Count) paths"
  }
  # 3. Pimax on, screen-saver key 0, wait until tracked
  Start-PimaxTracked
  # 4. the developer files: what the package installs for the user's profile (+ the suite keys per scene)
  $pf = Join-Path $OutDir 'user-profile.json'; Write-Utf8 $pf ($draft.profile | ConvertTo-Json -Depth 10)
  $ao = & $Cli apply --profile $pf --dcs $draft.dcs --options $draft.options 2>&1 | Out-String; if ($LASTEXITCODE -ne 0) { throw "apply of the user profile failed: $($ao.Trim())" }
  Note "user profile applied with $cliVersion to read the engine files it installs"
  if (-not (Test-Path -LiteralPath $insIni) -or -not (Test-Path -LiteralPath $insPay)) { throw 'the apply installed no DcsQvCull.ini / DcsQvCullPayload.dll: is the engine on in the profile?' }
  $baseIni = [IO.File]::ReadAllText($insIni)
  # the installed payload is gone after each scene (the runner restores stock): keep a copy of it for every scene
  $payFrom = if ($Payload) { [IO.Path]::GetFullPath($Payload) } else { $pc = Join-Path $OutDir 'package-payload\DcsQvCullPayload.dll'; New-Item -ItemType Directory -Force (Split-Path $pc) | Out-Null; Copy-Item -LiteralPath $insPay $pc -Force; $pc }
  $res.package.iniSha256 = Sha $insIni; $res.package.payloadSha256 = Sha $payFrom
  Note "engine files: ini sha256 $($res.package.iniSha256); payload sha256 $($res.package.payloadSha256) ($($res.package.payloadFrom))"
  # 5. the scenes
  foreach ($s in $plan) {
    $sr = [ordered]@{ id = $s.id; atmosx = [bool]$s.atmosx; mission = [string]$M.missions.([string]$s.mission).file; missionSha256 = Sha $missionPath[[string]$s.mission]; suiteAt = $s.suiteAt; status = 'not flown'; why = $null; fpsCap = $null; runDir = $null; parts = $null; suiteEnded = $null; skipped = @() }
    $res.scenes.Add($sr)
    if ($stop) { $sr.why = "not flown: $stop"; continue }
    if (-not $s.atmosx -and $atx -and $atxState.Installed -and -not $atxOff) { $atxOff = $true; Note (Set-AtmosxOff $atx $dcsRoot $draft.options $OutDir) }
    $d = Join-Path $OutDir "scenes\$($s.id)"; $dev = Join-Path $d 'dev'; New-Item -ItemType Directory -Force $dev | Out-Null
    $t = $baseIni
    foreach ($sec in $ini.PSObject.Properties) { foreach ($k in $sec.Value.PSObject.Properties) { $t = Set-IniText $t $sec.Name $k.Name ([string]$k.Value) } }
    $t = Set-IniText $t 'Suite' 'Label' "release matrix v$($M.version) $($s.id)"
    $devIni = Join-Path $dev 'DcsQvCull.ini'; $devPay = Join-Path $dev 'DcsQvCullPayload.dll'
    Write-Utf8 $devIni $t; Copy-Item -LiteralPath $payFrom $devPay -Force
    $sp = New-SceneFiles $s $d $devIni $devPay
    Note "scene $($s.id): ATMOSX $(if ($s.atmosx) { 'on' } else { 'off' }), mission $($sr.mission), suite at FLY+$($s.suiteAt) s"
    & pwsh @(Get-RunnerArgs $sp (Join-Path $d 'runs') $rigState) *> (Join-Path $d 'runner.txt'); $rc = $LASTEXITCODE
    $fl = Get-ChildItem -LiteralPath (Join-Path $d 'runs') -Directory -Filter 'rm_*' -ErrorAction SilentlyContinue | Sort-Object LastWriteTime -Descending | Select-Object -First 1
    $rj = if ($fl -and (Test-Path -LiteralPath (Join-Path $fl.FullName 'run.json'))) { Get-Content -LiteralPath (Join-Path $fl.FullName 'run.json') -Raw | ConvertFrom-Json } else { $null }
    if ($fl) {
      $sr.runDir = Get-RelPath $OutDir $fl.FullName
      $g = Get-SceneParts $M $fl.FullName $dev; $sr.parts = $g.parts; $sr.suiteEnded = $g.info.ended; $sr.skipped = $g.info.skipped; $sr.fpsCap = $g.info.fpsCap
      if (-not $res.dcsVersion) { $dv = [regex]::Match((Read-Shared (Join-Path $fl.FullName 'dcs.log')), 'DCS/(\d+\.\d+\.\d+\.\d+)'); if ($dv.Success) { $res.dcsVersion = $dv.Groups[1].Value } }   # not $m: PowerShell names ignore case, $m is $M (the matrix)
      if (-not $res.profileSha256 -and (Test-Path -LiteralPath (Join-Path $fl.FullName 'profile.json'))) {
        $pj = Get-Content -LiteralPath (Join-Path $fl.FullName 'profile.json') -Raw | ConvertFrom-Json
        foreach ($k in 'developerOptions', 'engineDevMode', 'engineDevIniPath', 'engineDevPayloadPath') { $pj.PSObject.Properties.Remove($k) }
        $res.profileSha256 = [BitConverter]::ToString([Security.Cryptography.SHA256]::HashData([Text.Encoding]::UTF8.GetBytes(($pj | ConvertTo-Json -Depth 10 -Compress)))).Replace('-', '').ToLowerInvariant()
      }
    }
    $crash = if ($rj -and $rj.crash) { [string]$rj.crash } else { $null }
    if ($rc -eq 0 -and $sr.suiteEnded -eq 'finished' -and -not $crash) { $sr.status = 'ok' }
    else {
      $why = [Collections.Generic.List[string]]::new()
      if ($rc -ne 0) { $why.Add("runner exit $rc$(if ($rj.error) { ": $($rj.error)" })") }
      if ($crash) { $why.Add("DCS crash file $crash") }
      if ($sr.suiteEnded -ne 'finished') { $why.Add("suite $(if ($sr.suiteEnded) { $sr.suiteEnded } else { 'not ended' })") }
      $sr.status = 'failed'; $sr.why = $why -join '; '
    }
    Note "scene $($s.id): $($sr.status)$(if ($sr.why) { " ($($sr.why))" }); parts ok: $(if ($sr.parts) { @($sr.parts.Values | Where-Object { $_.status -eq 'ok' }).Count } else { 0 }) of $(@($M.parts).Count)"
    if ($sr.status -ne 'ok') { $stop = "scene $($s.id) failed ($($sr.why)): a failed or crashed flight stops all flying" }
    & $saveResults
  }
} catch { Note "run FAILED: $_"; $res.run.error = "$_"; if (-not $stop) { $stop = "$_" } }
finally {
  # 6. put everything back (each step on its own: one failure never skips the others)
  $rs = [Collections.Generic.List[string]]::new(); $manual = [Collections.Generic.List[string]]::new()
  $t0 = Get-Date; while ((Get-Process DCS -ErrorAction SilentlyContinue) -and ((Get-Date) - $t0).TotalSeconds -lt 150) { Start-Sleep 5 }
  $dcsUp = [bool](Get-Process DCS -ErrorAction SilentlyContinue)
  if ($dcsUp) { $manual.Add('DCS is still running (the runner could not close it): close it, then restore ATMOSX / the profile as listed here'); Note 'DCS still running: ATMOSX and the profile are NOT restored' }
  if ($atxOff -and -not $dcsUp) {
    try {
      $r = Set-AtmosxOn $atx $dcsRoot $draft.options $OutDir $atxRecord
      $rs.Add("ATMOSX back: $($r.Compared) paths compared with the record, $($r.Different.Count) different $($r.Different -join ', ')")
      if ($r.Different.Count) { $manual.Add("ATMOSX: $($r.Different.Count) path(s) differ from the record (saved copies in $OutDir\atmosx-mod)") }
    } catch { $rs.Add("ATMOSX restore FAILED: $_"); $manual.Add("ATMOSX: restore by hand from $OutDir\atmosx-mod and options-saved.lua") }
  } elseif ($atxOff) { $manual.Add("ATMOSX is OFF: copy $OutDir\atmosx-mod back over the DCS folder and options-saved.lua over options.lua") }
  if ($Script:SaverSet) {
    try {
      if (Get-Process pi_server -ErrorAction SilentlyContinue) {
        & $PimaxCtl set runtime_screen_off_timeout_second 180 2>&1 | Out-Null; $v = Get-SaverKey; $rs.Add("screen-saver key: $v (as found $($Script:SaverFound))")
        if ($v -ne '180') { $manual.Add('Pimax screen-saver key is not 180: set it in Pimax Play') }
      } else { $manual.Add('Pimax screen-saver key NOT restored (pi_server not running): set 180 in Pimax Play') }
    } catch { $manual.Add("Pimax screen-saver key NOT restored: $_") }
  }
  if (-not $dcsUp -and $draft -and $stBefore) {
    $t1 = Get-Date; while ((Get-Process DcsVr.Cli -ErrorAction SilentlyContinue) -and ((Get-Date) - $t1).TotalSeconds -lt 90) { Start-Sleep 2 }
    try {
      if ($stBefore.state -eq 'applied') {
        $pf = Join-Path $OutDir 'user-profile.json'; Write-Utf8 $pf ($draft.profile | ConvertTo-Json -Depth 10)
        $ao = & $Cli apply --profile $pf --dcs $draft.dcs --options $draft.options 2>&1 | Out-String; $c = $LASTEXITCODE
        $rs.Add("user profile re-applied with $cliVersion (exit $c); app state now $((Cli-Status).state)"); if ($c -ne 0) { $manual.Add('the user profile was not re-applied: open DCS Control and Apply') }
        if ($stBefore.appVersion -and $cliVersion -notmatch [regex]::Escape([string]$stBefore.appVersion)) { $rs.Add("note: before the run the profile had been applied by $($stBefore.appVersion); it is now applied by the package under test") }
      } else { $o = & $Cli restore 2>&1 | Out-String; $rs.Add("app state was '$($stBefore.state)' before: restore (exit $LASTEXITCODE), now $((Cli-Status).state)") }
    } catch { $rs.Add("profile restore FAILED: $_"); $manual.Add('open DCS Control: Apply your profile (or Restore)') }
    foreach ($n in $StateFiles) { if ((Sha (Join-Path $State $n)) -ne (Sha (Join-Path $rigState $n))) { $rs.Add("user file ${n}: DIFFERENT from the copy taken at the start (copy in $rigState)"); $manual.Add("user file $n changed during the run") } }
  }
  if ($before) {
    try {
      $after = Get-MachineState $dcsRoot $draft.options $atx; $after.GetEnumerator() | ForEach-Object { "$($_.Key)`t$($_.Value)" } | Set-Content (Join-Path $OutDir 'state-after.txt')
      $dd = Compare-MachineState $before $after; $un = @($dd | Where-Object { -not $_.Expected })
      $rs.Add("state: $($after.Count) entries, $($dd.Count) differ ($($un.Count) not expected$(if ($un.Count) { ': ' + (($un | Select-Object -First 12 | ForEach-Object { $_.Key }) -join ', ') }))")
      $dd | ForEach-Object { "$(if ($_.Expected) { 'expected' } else { 'UNEXPECTED' })`t$($_.Key)`t$($_.Before) -> $($_.After)" } | Set-Content (Join-Path $OutDir 'state-diff.txt')
    } catch { $rs.Add("state comparison failed: $_") }
  }
  if (-not $dcsUp) { try { $rs.Add((Stop-Pimax)) } catch { $manual.Add("Pimax not closed: $_") } } else { $manual.Add('Pimax left on (DCS still running)') }
  if ($lockMine -and (Test-Path -LiteralPath $QuietLock) -and ((Get-Content -LiteralPath $QuietLock -TotalCount 1) -eq $lockText)) { Remove-Item -LiteralPath $QuietLock -Force -Confirm:$false; $rs.Add('quiet lock removed') }
  foreach ($x in $rs) { Note "restore: $x" }; foreach ($x in $manual) { Note "BY HAND: $x" }
  $res.run.restore = @($rs); $res.run.byHand = @($manual); $res.run.ended = (Get-Date).ToString('s'); $res.run.stoppedBecause = $stop
  if (@($res.scenes | Where-Object { $_.status -ne 'ok' }).Count -or $res.scenes.Count -lt $plan.Count) { $res.partial = $true; $res.partialWhy = 'not every scene was measured' }
  & $saveResults
  # 7. the short report and the gate
  $md = [Collections.Generic.List[string]]::new()
  $md.Add("# Release matrix $($res.label) ($($res.created))"); $md.Add('')
  $md.Add("Package $cliVersion, payload $($res.package.payloadSha256) ($($res.package.payloadFrom)), DCS $($res.dcsVersion), profile $($res.profileId). Matrix v$($M.version), $blocks blocks x $blockSec s per A/B; fps capped as noted, so render-thread CPU is the measure. Change = ON vs its OFF arm in the same flight (each ON block against its OFF neighbours); +- = standard error (the module's own report prints a 95 % interval, about 2.4 x larger).")
  $md.Add('')
  foreach ($sr in $res.scenes) {
    $md.Add("## $($sr.id): $($sr.status)$(if ($sr.why) { " ($($sr.why))" })$(if ($sr.fpsCap) { ", capped $($sr.fpsCap) fps" })"); $md.Add('')
    if (-not $sr.parts) { continue }
    $md.Add('| Part (OFF arm) | Render thread ms/frame OFF -> ON | Change +- SE | p95 change +- SE | Process CPU change | Blocks |'); $md.Add('|---|---|---|---|---|---|')
    foreach ($pt in $M.parts) {
      $e = $sr.parts[[string]$pt.id]
      if (-not $e -or $e.status -ne 'ok') { $md.Add("| $($pt.label) | missing: $($e.why) | | | | |"); continue }
      $rt = $e.metrics.render_thread_ms
      $fmt = { param($x) if ($x) { '{0:+0.00;-0.00} % +- {1}' -f $x.changePct, $(if ($null -ne $x.sePct) { '{0:N2}' -f $x.sePct } else { 'n/a' }) } else { 'n/a' } }
      $md.Add("| $($pt.label) ($($pt.offArm)) | $('{0:N3} -> {1:N3}' -f $rt.off, $rt.on) | $(& $fmt $rt) | $(& $fmt $e.metrics.p95_ms) | $(& $fmt $e.metrics.process_cpu_ms) | $($e.nBlocks)$(if ($pt.kind -eq 'water') { ", engaged $($e.engaged)" }) |")
    }
    if ($sr.skipped.Count) { $md.Add(''); $md.Add("Skipped by the module: $($sr.skipped -join '; ')") }
    $md.Add('')
  }
  $md.Add('## Restore'); $md.Add(''); foreach ($x in $rs) { $md.Add("- $x") }; foreach ($x in $manual) { $md.Add("- **BY HAND**: $x") }
  if ($stop) { $md.Add(''); $md.Add("Stopped: $stop") }
  if ($res.scenes.Count) {
    $cmpArgs = @('-NoProfile', '-File', (Join-Path $Tool 'compare-matrix.ps1'), '-Current', (Join-Path $OutDir 'results.json'), '-Markdown', (Join-Path $OutDir 'GATE.md'), '-Quiet')
    if ($Baseline) { $cmpArgs += @('-Baseline', $Baseline) }
    & pwsh @cmpArgs; $gateRc = $LASTEXITCODE
    $md.Add(''); $md.Add("## Gate$(if ($Baseline) { " against $(Split-Path $Baseline -Leaf)" } else { ' (no baseline: cost rule only)' }): $(switch ($gateRc) { 0 { 'PASS' } 1 { 'FAIL' } default { "not judged (exit $gateRc)" } }) (GATE.md)")
  }
  [IO.File]::WriteAllLines((Join-Path $OutDir 'REPORT.md'), $md)
  Note "done: $OutDir\REPORT.md$(if ($null -ne $gateRc) { ", gate exit $gateRc" })"
}
if ($stop -or $res.run.byHand.Count) { exit 1 }
exit $(if ($gateRc -eq 1) { 4 } else { 0 })
