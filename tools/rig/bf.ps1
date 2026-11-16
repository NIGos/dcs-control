# Builds targets and/or runs ctest tests of an OFXR build folder, under the quiet lock (artifacts\.lane\quiet.lock).
#   bf.ps1 -What "<line for the lock>" -BuildDir <name> [-Targets a,b] [-Tests '<ctest -R regex>'] [-Repeat N]
#          [-Tag x] [-Minutes 10] [-WaitMinutes 30] [-All] [-Extra { script }] [-WorkRoot <folder>] [-DcsBin <folder>]
# -BuildDir is a build folder under -WorkRoot (default <repo>\artifacts\rig\builds); logs go to -WorkRoot too.
# -All: the whole suite (ctest -LE bench), output to <WorkRoot>\bugfix-ctest-<BuildDir>-<Tag>.txt.
# -DcsBin: DCS's bin folder for the tests that read DCS's own files (XRFG_TEST_DCS_BIN); without it the variable is
# left as the caller has it.
param([string]$What = 'run', [string]$BuildDir = 'split-a-build', [string[]]$Targets = @(), [string]$Tests = '',
      [int]$Repeat = 1, [string]$Tag = 'x', [int]$Minutes = 10, [int]$WaitMinutes = 30, [switch]$All,
      [scriptblock]$Extra = $null, [string]$WorkRoot = $env:DCSC_RIG_BUILDS, [string]$DcsBin)
$r = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..')); $lock = "$r\artifacts\.lane\quiet.lock"
$port = if ($WorkRoot) { [IO.Path]::GetFullPath($WorkRoot) } else { "$r\artifacts\rig\builds" }
$b = "$port\$BuildDir"
for ($i = 0; $i -lt ($WaitMinutes * 3) -and (Test-Path $lock); $i++) { Start-Sleep -Seconds 20 }
if (Test-Path $lock) { "STILL LOCKED: " + (Get-Content $lock); exit 3 }
for ($i = 0; $i -lt 30 -and (Get-Process cl, link, ninja, ctest, MSBuild -ErrorAction SilentlyContinue); $i++) { Start-Sleep -Seconds 10 }
if (Get-Process DCS, cl, link, ninja, ctest, MSBuild -ErrorAction SilentlyContinue) { "machine busy (DCS or a build): not starting"; exit 4 }
if (Test-Path $lock) { "LOCKED meanwhile: " + (Get-Content $lock); exit 3 }
"rig bf.ps1, $What, started $(Get-Date -Format HH:mm), $Minutes min" | Set-Content $lock
try {
  "lock taken $(Get-Date -Format HH:mm:ss)"
  if ($Targets.Count -gt 0) {
    & "$r\scripts\msvc.cmd" cmake --build $b --parallel 6 --target $Targets *> "$port\bugfix-build-$Tag.log"
    "build exit $LASTEXITCODE"
    if ($LASTEXITCODE -ne 0) { Select-String -Path "$port\bugfix-build-$Tag.log" -Pattern 'error|FAILED' | Select-Object -First 25 | ForEach-Object { $_.Line.Substring(0, [Math]::Min(400, $_.Line.Length)) }; return }
  }
  if ($DcsBin) { $env:XRFG_TEST_DCS_BIN = $DcsBin }
  if ($Tests) {
    $pass = 0; $fail = 0
    for ($n = 1; $n -le $Repeat; $n++) {
      $out = "$port\bugfix-test-$Tag-$n.txt"
      & "$r\scripts\msvc.cmd" ctest --test-dir $b -R "`"$Tests`"" --output-on-failure --timeout 300 *> $out
      if ($LASTEXITCODE -eq 0) { $pass++ } else { $fail++ }
    }
    "tests '$Tests' x $Repeat : pass $pass fail $fail (last output: $port\bugfix-test-$Tag-$Repeat.txt)"
  }
  if ($All) {
    $sw = [Diagnostics.Stopwatch]::StartNew()
    & "$r\scripts\msvc.cmd" ctest --test-dir $b -LE bench --output-on-failure *> "$port\bugfix-ctest-$BuildDir-$Tag.txt"
    "$BuildDir whole ctest exit $LASTEXITCODE in $([int]$sw.Elapsed.TotalSeconds) s"
    Get-Content "$port\bugfix-ctest-$BuildDir-$Tag.txt" -Tail 14
  }
  if ($Extra) { & $Extra }
} finally { Remove-Item $lock -Force -ErrorAction SilentlyContinue; "lock released $(Get-Date -Format HH:mm:ss)" }
