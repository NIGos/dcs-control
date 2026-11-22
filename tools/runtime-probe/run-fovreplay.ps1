# Runs the foveated replay (fovreplay.exe, REPLAY.md) on the Pimax runtime: starts Pimax, keeps the headset awake,
# cycles the submission variants while reading the compositor's mirrors, writes summary.md, and ALWAYS puts back what it
# changed: screen saver timeout, mirror keys (the replay itself), Pimax closed with the PiServiceLauncher service
# stopped (start type left Automatic).
#   tools\runtime-probe\run-fovreplay.ps1 [-Seconds 15] [-Loops 1] [-Only a,b] [-UserSession] [-NoReboot] [-KeepPimax] [-OutRoot <dir>]
# -SubmitOnly: no mirrors, no tracking needed: only checks that every variant submits without runtime errors.
# -UserSession: the headset is worn. Beeps when the pattern starts and when it ends; the variant number is shown in the
# headset under the focus. No DCS, no OFXR, no Quad Views involved.
param([double]$Seconds = 15, [int]$Loops = 1, [string]$Only, [switch]$UserSession, [switch]$SubmitOnly, [switch]$NoReboot, [switch]$KeepPimax, [string]$OutRoot)
$ErrorActionPreference = 'Stop'
$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
$bin = Join-Path $PSScriptRoot 'bin'
if (-not $OutRoot) { $OutRoot = Join-Path $repo 'artifacts\runtime-probe\fovreplay' }
$out = Join-Path $OutRoot (Get-Date -Format 'yyyyMMdd-HHmmss')
New-Item -ItemType Directory -Force $out | Out-Null
$log = Join-Path $out 'runner.log'
function Say([string]$m) { $line = "$(Get-Date -Format 'HH:mm:ss') $m"; Add-Content $log $line; $line }
$ctl = Join-Path $bin 'pimax_ctl.exe'
foreach ($exe in 'fovreplay.exe', 'pimax_ctl.exe') { if (-not (Test-Path (Join-Path $bin $exe))) { throw "missing ${exe}: run tools\runtime-probe\build.bat" } }
if (Get-Process DCS -ErrorAction SilentlyContinue) { throw 'DCS is running: the replay does not run beside it' }
$runtimeProcs = 'PimaxClient', 'pi_server', 'pi_overlay', 'pi_vst', 'PiPlayService', 'PiPlatformService_64'
$svcBefore = (Get-Service PiServiceLauncher -ErrorAction SilentlyContinue).Status
$pimaxWasRunning = [bool](Get-Process pi_server -ErrorAction SilentlyContinue)
Say "before: PiServiceLauncher $svcBefore, pi_server running $pimaxWasRunning"
function Invoke-Elevated([string]$cmd) {
  # The service needs an elevated shell (UAC prompts are off on this machine).
  $p = Start-Process pwsh -Verb RunAs -WindowStyle Hidden -PassThru -ArgumentList @('-NoProfile', '-Command', $cmd)
  $p.WaitForExit(60000) | Out-Null
}
$saverBefore = $null; $code = 99
try {
  if ((Get-Service PiServiceLauncher).Status -ne 'Running') { Say 'starting the PiServiceLauncher service'; Invoke-Elevated 'Start-Service PiServiceLauncher' }
  if (-not (Get-Process PimaxClient -ErrorAction SilentlyContinue)) { Say 'starting Pimax Client'; Start-Process 'C:\Program Files\Pimax\PimaxClient\pimaxui\PimaxClient.exe' }
  $t0 = Get-Date
  while (-not (Get-Process pi_server -ErrorAction SilentlyContinue)) { if (((Get-Date) - $t0).TotalSeconds -gt 120) { throw 'pi_server did not start' }; Start-Sleep 3 }
  Start-Sleep 15
  $saverBefore = [int](& $ctl get runtime_screen_off_timeout_second | Select-Object -Last 1)
  Say "screen saver timeout was $saverBefore s; set to 0 for the run"
  & $ctl set runtime_screen_off_timeout_second 0 | Out-Null
  if (-not $NoReboot) {
    # A sleeping display does not wake for the new timeout: restart the headset (Pimax Play's own "restart headset").
    Say ('headset restart: ' + (& $ctl reboot | Select-Object -Last 1)); Start-Sleep 60
  }
  $args2 = @("`"$out`"", '--seconds', "$Seconds", '--loops', "$Loops")
  if ($Only) { $args2 += @('--only', $Only) }
  if ($UserSession) { $args2 += '--beep' }
  if ($SubmitOnly) { $args2 += @('--allow-untracked', '--submit-only') }
  for ($attempt = 1; $attempt -le 20; $attempt++) {
    Say "replay, attempt $attempt (it waits for tracking by itself)"
    $p = Start-Process -FilePath (Join-Path $bin 'fovreplay.exe') -ArgumentList $args2 -WorkingDirectory $out -PassThru -WindowStyle Hidden
    if (-not $p.WaitForExit(40 * 60 * 1000)) { Say 'replay timed out: stopping it'; $p | Stop-Process -Force; $code = 98; break }
    $code = $p.ExitCode; Say "replay exit code $code"
    if ($code -eq 5) { Say 'the runtime is not ready for a session yet: again in 15 s'; Start-Sleep 15; continue }
    break
  }
  switch ($code) {
    0 { Say "summary: $(Join-Path $out 'summary.md')" }
    3 { Say 'NOT TRACKED: the headset did not track (or the mirror stayed dark). Light the room, give the cameras a clear view; nothing was measured.' }
    4 { Say 'no mirror image from the compositor: nothing or only part was measured.' }
  }
  $srv = Get-ChildItem "$env:LOCALAPPDATA\Pimax\runtime\pvr_srv_log_*.txt" -ErrorAction SilentlyContinue | Sort-Object LastWriteTime -Descending | Select-Object -First 1
  if ($srv) { Copy-Item $srv.FullName (Join-Path $out 'pvr_srv_log.txt') }
}
catch { Say "ERROR: $_"; $code = 97 }
finally {
  if ($null -ne $saverBefore) { $now = & $ctl set runtime_screen_off_timeout_second $saverBefore | Select-Object -Last 1; Say "screen saver timeout back to $now s" }
  if (-not $KeepPimax -and -not $pimaxWasRunning) {
    # The agreed closing state: service stopped, start type Automatic (a reboot brings it back).
    Invoke-Elevated 'Stop-Service PiServiceLauncher -Force'
    for ($i = 0; $i -lt 3; $i++) { Get-Process -Name $runtimeProcs -ErrorAction SilentlyContinue | Stop-Process -Force -Confirm:$false -ErrorAction SilentlyContinue; Start-Sleep 4 }
    Start-Sleep 8
    $left = (Get-Process -Name $runtimeProcs -ErrorAction SilentlyContinue | ForEach-Object Name) -join ', '
    $svc = Get-Service PiServiceLauncher
    Say ("Pimax closed, PiServiceLauncher $($svc.Status) / $($svc.StartType)" + $(if ($left) { " (still running: $left)" } else { '' }))
  } else { Say 'Pimax left running' }
}
exit $code
