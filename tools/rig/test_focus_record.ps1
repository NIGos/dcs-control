# Offline test of the runner's passive focus record (run-scenario.ps1: $FgSampler, Get-HookPress, Get-FocusAt, Get-FlyLine).
#   pwsh -File tools\rig\test_focus_record.ps1        exit 0 = every check holds. Starts nothing, sends no input.
$ErrorActionPreference = 'Stop'
$runner = Join-Path $PSScriptRoot 'run-scenario.ps1'
$ast = [System.Management.Automation.Language.Parser]::ParseFile($runner, [ref]$null, [ref]$null)
foreach ($f in $ast.FindAll({ $args[0] -is [System.Management.Automation.Language.FunctionDefinitionAst] -and $args[0].Name -in 'Get-HookPress', 'Get-FocusAt', 'Get-FlyLine', 'Read-Shared' }, $true)) { Invoke-Expression $f.Extent.Text }
$sampler = $ast.FindAll({ $args[0] -is [System.Management.Automation.Language.AssignmentStatementAst] -and $args[0].Left.Extent.Text -eq '$FgSampler' }, $true) | Select-Object -First 1
$fails = 0; $checks = 0
function Check([bool]$ok, [string]$what) { $script:checks++; if (-not $ok) { $script:fails++ }; Write-Host "$(if ($ok) { 'PASS' } else { 'FAIL' }) $what" }
$tmp = Join-Path ([IO.Path]::GetTempPath()) ("focusrec-" + [guid]::NewGuid().ToString('N')); New-Item -ItemType Directory $tmp | Out-Null
try {
  # dcs.log writes UTC; the press is the first FLY / spawn line of the hook.
  $log = @"
2026-11-22 11:36:59.000 INFO    DcsQvCull (Main): rig autostart: armed, waiting for the briefing
2026-11-22 11:37:09.800 INFO    DcsQvCull (Main): rig autostart: no briefing after 10 s, paused: spawn and unpause (try 1): true nil
2026-11-22 11:37:09.955 INFO    WWT (Main): F-16C_50
2026-11-22 11:37:12.810 INFO    DcsQvCull (Main): rig autostart: flying (model time 3.0 s, +3.0 s in 3.0 s real; player unit 441)
"@
  $p = Get-HookPress $log
  $want = [DateTime]::SpecifyKind([datetime]'2026-11-22 11:37:09.800', 'Utc').ToLocalTime()
  Check ($p -and $p.At -eq $want -and $p.What -like 'no briefing after 10 s, paused: spawn and unpause (try 1)') 'hook press: spawn line, UTC to local'
  $p2 = Get-HookPress "2026-11-22 11:37:09.800 INFO    DcsQvCull (Main): rig autostart: FLY pressed (try 2): true nil"
  Check ($p2 -and $p2.What -eq 'FLY pressed (try 2)') 'hook press: FLY line'
  Check ($null -eq (Get-HookPress 'nothing here')) 'hook press: none'
  # Nearest sample within 1.5 s.
  $tsv = Join-Path $tmp 'foreground.tsv'
  $rows = @("time`tforeground`tforegroundTitle`tdcsForeground`tdcsCentre`tdcsIconic")
  foreach ($o in @(-3.0, -0.4, 0.3, 2.0)) { $rows += "{0:o}`t{1}`tT{2}`tFalse`tClaude`tFalse" -f $want.AddSeconds($o), $(if ($o -eq 0.3) { 'Claude' } else { 'other' }), $o }
  Set-Content -LiteralPath $tsv -Value $rows
  $s = Get-FocusAt $tsv $want
  Check ($s -and $s.foreground -eq 'Claude' -and $s.offsetSec -eq 0.3 -and $s.dcsForeground -eq 'False' -and $s.dcsCentre -eq 'Claude') 'focus sample: nearest one, fields read'
  Check ($null -eq (Get-FocusAt $tsv $want.AddSeconds(10))) 'focus sample: none within 1.5 s'
  Check ($null -eq (Get-FocusAt (Join-Path $tmp 'missing.tsv') $want)) 'focus sample: no file'
  # The start proof: aircraft line first, else the hook's model-time line (start = its time minus the measured run).
  $DcsLog = Join-Path $tmp 'dcs.log'; Set-Content -LiteralPath $DcsLog -Value $log
  $fl = Get-FlyLine; Check ($fl -and $fl.By -eq 'aircraft line' -and $fl.Aircraft -eq 'F-16C_50') 'start proof: aircraft line'
  Set-Content -LiteralPath $DcsLog -Value ($log -split "`r?`n" | Where-Object { $_ -notmatch 'WWT' })
  $fl = Get-FlyLine; Check ($fl -and $fl.By -eq 'model time (rig autostart)' -and $fl.At -eq [DateTime]::SpecifyKind([datetime]'2026-11-22 11:37:09.810', 'Utc').ToLocalTime()) 'start proof: model-time line'
  # The sampler itself, for 1.2 s (reads the foreground only; no DCS needed).
  if (-not ('Rig.W' -as [type]) -or -not ('Rig.P' -as [type])) {
    foreach ($a in $ast.FindAll({ $args[0] -is [System.Management.Automation.Language.CommandAst] -and $args[0].GetCommandName() -eq 'Add-Type' -and $args[0].Extent.Text -match '-Name (W|P) ' }, $true)) { Invoke-Expression $a.Extent.Text }
  }
  $fg = Invoke-Expression $sampler.Right.Extent.Text
  $stop = Join-Path $tmp 'stop'; $out = Join-Path $tmp 'sampled.tsv'
  $job = Start-ThreadJob -ScriptBlock $fg -ArgumentList $out, $stop; Start-Sleep -Milliseconds 1200; Set-Content $stop 'stop'; $job | Wait-Job -Timeout 5 | Out-Null; $job | Remove-Job -Force
  $got = @(Import-Csv -LiteralPath $out -Delimiter "`t")
  Check ($got.Count -ge 2 -and $got[0].foreground) "sampler: $($got.Count) samples, foreground '$($got[0].foreground)'"
} finally { Remove-Item -LiteralPath $tmp -Recurse -Force -ErrorAction SilentlyContinue }
Write-Host "$checks checks, $fails failed"
if ($fails) { exit 1 }
