# Runs one ctest test until it fails (at most -Max times) and keeps the failing run's work folder: the tool for an
# intermittent test. Under the quiet lock (artifacts\.lane\quiet.lock).
#   bfloop.ps1 -BuildDir <name> -Test xrfg_layer_two_layers_deep_hold_off -WorkDir xrfg_layer_two_layers_deep_hold_off -Tag b2 [-Max 150] [-KeepPass]
#              [-WorkRoot <folder>] [-DcsBin <folder>]
# -BuildDir is a build folder under -WorkRoot (default <repo>\artifacts\rig\builds); what is kept goes to -WorkRoot.
param([string]$BuildDir = 'split-b-build', [string]$Test = '', [string]$WorkDir = '', [string]$Tag = 'x', [int]$Max = 150,
      [int]$Minutes = 6, [int]$WaitMinutes = 30, [switch]$KeepPass, [int]$Fails = 1, [string]$WorkRoot = $env:DCSC_RIG_BUILDS, [string]$DcsBin)
$r = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..')); $lock = "$r\artifacts\.lane\quiet.lock"
$port = if ($WorkRoot) { [IO.Path]::GetFullPath($WorkRoot) } else { "$r\artifacts\rig\builds" }
$b = "$port\$BuildDir"
for ($i = 0; $i -lt ($WaitMinutes * 3) -and (Test-Path $lock); $i++) { Start-Sleep -Seconds 20 }
if (Test-Path $lock) { "STILL LOCKED: " + (Get-Content $lock); exit 3 }
if (Get-Process DCS, cl, link, ninja, ctest, MSBuild -ErrorAction SilentlyContinue) { "machine busy (DCS or a build): not starting"; exit 4 }
"rig bfloop.ps1, loop $Test until it fails, started $(Get-Date -Format HH:mm), $Minutes min" | Set-Content $lock
try {
  if ($DcsBin) { $env:XRFG_TEST_DCS_BIN = $DcsBin }
  $pass = 0; $fail = 0
  for ($n = 1; $n -le $Max -and $fail -lt $Fails; $n++) {
    $out = "$port\bugfix-loop-$Tag.txt"
    & "$r\scripts\msvc.cmd" ctest --test-dir $b -R "`"^$Test`$`"" --output-on-failure --timeout 300 *> $out
    if ($LASTEXITCODE -eq 0) {
      $pass++
      if ($KeepPass -and $pass -eq 1) { Remove-Item "$port\bugfix-keep-$Tag-pass" -Recurse -Force -ErrorAction SilentlyContinue; Copy-Item "$b\$WorkDir" "$port\bugfix-keep-$Tag-pass" -Recurse }
    } else {
      $fail++
      Remove-Item "$port\bugfix-keep-$Tag-fail$fail" -Recurse -Force -ErrorAction SilentlyContinue
      Copy-Item "$b\$WorkDir" "$port\bugfix-keep-$Tag-fail$fail" -Recurse
      Copy-Item $out "$port\bugfix-keep-$Tag-fail$fail\ctest-output.txt"
    }
  }
  "loop '$Test': pass $pass fail $fail"
} finally { Remove-Item $lock -Force -ErrorAction SilentlyContinue; "lock released $(Get-Date -Format HH:mm:ss)" }
