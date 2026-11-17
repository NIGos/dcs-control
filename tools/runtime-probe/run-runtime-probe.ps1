# Runs the runtime probe unattended on the Pimax OpenXR runtime: wakes the headset, waits for tracking, captures every
# case with each graphics API, analyses them, and always puts back what it changed and closes Pimax.
# Needs: the headset on its stand in a LIT room (the Pimax compositor shows no application while the headset is not
# tracked), DCS closed, tools\runtime-probe built (build.bat).
#   tools\runtime-probe\run-runtime-probe.ps1 [-Quick] [-Api d3d11,d3d12] [-KeepPimax] [-OutRoot <dir>]
param([switch]$Quick, [string[]]$Api = @('d3d11', 'd3d12'), [switch]$KeepPimax, [string]$OutRoot)
$ErrorActionPreference = 'Stop'
$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
$bin = Join-Path $repo 'tools\runtime-probe\bin'
if (-not $OutRoot) { $OutRoot = Join-Path $repo 'artifacts\runtime-probe' }
$out = Join-Path $OutRoot (Get-Date -Format 'yyyyMMdd-HHmmss')
New-Item -ItemType Directory -Force $out | Out-Null
$log = Join-Path $out 'runner.log'
function Say([string]$m) { $line = "$(Get-Date -Format 'HH:mm:ss') $m"; Add-Content $log $line; $line }
$ctl = Join-Path $bin 'pimax_ctl.exe'
foreach ($exe in 'probe_run.exe', 'probe_analyze.exe', 'pimax_ctl.exe') { if (-not (Test-Path (Join-Path $bin $exe))) { throw "missing ${exe}: run tools\runtime-probe\build.bat" } }
if (Get-Process DCS -ErrorAction SilentlyContinue) { throw 'DCS is running: the probe does not run beside it' }

$runtimeProcs = 'PimaxClient', 'pi_server', 'pi_overlay', 'pi_vst', 'PiPlayService', 'PiPlatformService_64'
$pimaxWasRunning = [bool](Get-Process pi_server -ErrorAction SilentlyContinue)
$saverBefore = $null
$worst = 0
try {
  $svc = Get-Service PiServiceLauncher -ErrorAction Stop
  if ($svc.Status -ne 'Running') { Say 'starting the PiServiceLauncher service'; Start-Service PiServiceLauncher }
  if (-not (Get-Process PimaxClient -ErrorAction SilentlyContinue)) { Say 'starting Pimax Client'; Start-Process 'C:\Program Files\Pimax\PimaxClient\pimaxui\PimaxClient.exe' }
  $t0 = Get-Date
  while (-not (Get-Process pi_server -ErrorAction SilentlyContinue)) { if (((Get-Date) - $t0).TotalSeconds -gt 90) { throw 'pi_server did not start' }; Start-Sleep 3 }
  Start-Sleep 15
  $saverBefore = [int](& $ctl get runtime_screen_off_timeout_second | Select-Object -Last 1)
  Say "screen saver timeout was $saverBefore s; set to 0 for the run"
  & $ctl set runtime_screen_off_timeout_second 0 | Out-Null
  # A sleeping display does not wake for the new timeout: restart the headset (Pimax Play's own "restart headset").
  Say ('headset restart: ' + (& $ctl reboot | Select-Object -Last 1))
  Start-Sleep 60
  foreach ($a in $Api) {
    $dir = Join-Path $out $a
    New-Item -ItemType Directory -Force $dir | Out-Null
    $probeArgs = @("`"$dir`""); if ($Quick) { $probeArgs += '--quick' }; if ($a -eq 'd3d12') { $probeArgs += '--d3d12' }
    $code = 99
    # After a headset restart the runtime refuses sessions for a while (137 s seen): up to 20 tries, 15 s apart.
    for ($attempt = 1; $attempt -le 20; $attempt++) {
      Say "$a probe run, attempt $attempt (it waits for tracking by itself)"
      $p = Start-Process -FilePath (Join-Path $bin 'probe_run.exe') -ArgumentList $probeArgs -WorkingDirectory $dir -PassThru -WindowStyle Hidden
      if (-not $p.WaitForExit(30 * 60 * 1000)) { Say 'probe timed out after 30 min: stopping it'; $p | Stop-Process -Force; $code = 98; break }
      $code = $p.ExitCode
      Say "$a probe exit code $code"
      if ($code -eq 5 -and $attempt -lt 20) { Say 'the runtime is not ready for a session yet: again in 15 s'; Start-Sleep 15; continue }
      if ($code -ne 2 -or $attempt -ge 2) { break }
      Say ('session failed: one more headset restart: ' + (& $ctl reboot | Select-Object -Last 1)); Start-Sleep 90
    }
    switch ($code) {
      3 { Say 'NOT TRACKED: the headset did not track within the wait. Light the room and leave the cameras a clear view; nothing was measured.' }
      4 { Say 'no mirror image from the compositor: nothing or only part was measured.' }
    }
    $srv = Get-ChildItem "$env:LOCALAPPDATA\Pimax\runtime\pvr_srv_log_*.txt" -ErrorAction SilentlyContinue | Sort-Object LastWriteTime -Descending | Select-Object -First 1
    if ($srv) { Copy-Item $srv.FullName (Join-Path $dir 'pvr_srv_log.txt') }
    & (Join-Path $bin 'probe_analyze.exe') $dir | Add-Content $log
    Say "$a report: $(Join-Path $dir 'report.md')"
    if ($code -gt $worst) { $worst = $code }
    if ($code -eq 3) { break }
  }
}
catch { Say "ERROR: $_"; $worst = 97 }
finally {
  # Always: screen saver back (the probe restores the mirror keys itself and records them in run.txt), Pimax closed unless it was open.
  if ($null -ne $saverBefore) { $now = & $ctl set runtime_screen_off_timeout_second $saverBefore | Select-Object -Last 1; Say "screen saver timeout back to $now s" }
  if (-not $KeepPimax -and -not $pimaxWasRunning) {
    for ($i = 0; $i -lt 2; $i++) { Get-Process -Name $runtimeProcs -ErrorAction SilentlyContinue | Stop-Process -Force -Confirm:$false -ErrorAction SilentlyContinue; Start-Sleep 4 }
    Start-Sleep 12
    $left = (Get-Process -Name $runtimeProcs -ErrorAction SilentlyContinue | ForEach-Object Name) -join ', '
    Say ("Pimax closed" + $(if ($left) { " (still running: $left)" } else { '' }))
  } else { Say 'Pimax left running' }
}
exit $worst
