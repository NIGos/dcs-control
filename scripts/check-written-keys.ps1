#Requires -Version 7
# Static check: every ini key the app writes is read by the component it is written for. scripts/written-keys.txt
# lists them (writer, section, key), made by DcsVr.Tests from ConfigurationWriters.cs over profiles that turn on every
# optional block; that test fails when the list is out of date. This script looks for each key in the component's
# sources (and the added lines of its patches):
#   DcsQvCull  DcsQvCull.ini     native/dcsqvcull/src: the L"<section>", L"<key>" pair, else L"<key>" ([Dev] included)
#   PupilShift PupilShift.ini    native/pupil_shift: L"<key>" (ini_float/ini_int name the section after the key)
#   Ofxr       ofxr_bridge.ini   external/ofxr-djules75/src + patches/ofxr-djules75, patches/ofxr: the pair, else L"<key>"
#   QuadViews  settings.cfg      external/quadviews/openxr-api-layer + patches/quadviews + native/quadviews: "<key>"
#   Cheeky     Cheeky's ini      external/cheeky/src and the sources in patches/cheeky: "<key>"
# A component whose sources are not here (Ofxr, QuadViews, Cheeky: its external checkout missing) is reported and skipped.
# Test sources (test, harness, fake) do not count. Exit 0 = every key is read; 1 = a problem (listed).
param([string]$Root = (Split-Path -Parent $PSScriptRoot))
$ErrorActionPreference = 'Stop'
$list = Join-Path $Root 'scripts/written-keys.txt'
if (-not (Test-Path -LiteralPath $list)) { Write-Host "PROBLEM: $list is missing"; exit 1 }
$extensions = @('.h', '.hpp', '.inc', '.cpp', '.c')
# $checkout: the external checkout the component is built from. Without it the component is skipped, whatever else is
# here (its patches and own sources alone are not what it reads).
function Sources([string[]]$folders, [string[]]$patchFolders, [string]$checkout) {
  if ($checkout -and -not (Test-Path -LiteralPath (Join-Path $Root $checkout))) { return $null }
  $text = [Text.StringBuilder]::new(); $found = $false
  foreach ($f in $folders) {
    $dir = Join-Path $Root $f
    if (-not (Test-Path -LiteralPath $dir)) { continue }
    $found = $true
    # Filtered on the path below the folder (the worktree's own path may hold any of these words).
    foreach ($file in Get-ChildItem -LiteralPath $dir -Recurse -File | Where-Object { $extensions -contains $_.Extension -and $_.FullName.Substring($dir.Length) -notmatch '(?i)[\\/](tests?|third_party|external)[\\/]|test|harness|fake' }) {
      [void]$text.AppendLine([IO.File]::ReadAllText($file.FullName))
    }
  }
  foreach ($f in $patchFolders) {
    $dir = Join-Path $Root $f
    if (-not (Test-Path -LiteralPath $dir)) { continue }
    foreach ($file in Get-ChildItem -LiteralPath $dir -File -Filter '*.patch') {
      foreach ($line in [IO.File]::ReadAllLines($file.FullName)) { if ($line.StartsWith('+') -and -not $line.StartsWith('+++')) { [void]$text.AppendLine($line.Substring(1)) } }
    }
  }
  if (-not $found) { return $null }
  return $text.ToString()
}
$readers = [ordered]@{
  DcsQvCull  = @{ Text = Sources @('native/dcsqvcull/src') @(); Pair = $true }
  PupilShift = @{ Text = Sources @('native/pupil_shift') @(); Pair = $false }
  Ofxr       = @{ Text = Sources @('external/ofxr-djules75/src') @('patches/ofxr-djules75', 'patches/ofxr') 'external/ofxr-djules75'; Pair = $true }
  QuadViews  = @{ Text = Sources @('external/quadviews/openxr-api-layer', 'native/quadviews') @('patches/quadviews') 'external/quadviews'; Pair = $false }
  Cheeky     = @{ Text = Sources @('external/cheeky/src', 'patches/cheeky') @() 'external/cheeky'; Pair = $false }
}
$problems = [Collections.Generic.List[string]]::new(); $counts = [ordered]@{}; $skipped = [Collections.Generic.HashSet[string]]::new()
foreach ($line in [IO.File]::ReadAllLines($list)) {
  if (-not $line -or $line.StartsWith('#')) { continue }
  $writer, $section, $key = $line.Split("`t")
  $reader = $readers[$writer]
  if ($null -eq $reader) { $problems.Add("unknown writer '$writer' in written-keys.txt"); continue }
  if ($null -eq $reader.Text) { [void]$skipped.Add($writer); continue }
  $counts[$writer] = 1 + ($counts[$writer] ?? 0)
  $k = [regex]::Escape($key)
  # The section and key pair where the reader names both; else the key as a wide literal (a helper given the section
  # as a variable, such as color_grade.h's ColorFix / ColorEnhanced or OFXR's read_ofxr_switch).
  $read = if ($reader.Pair -and $section) { $reader.Text -match ('L"' + [regex]::Escape($section) + '",\s*L"' + $k + '"') -or $reader.Text -match ('L"' + $k + '"') } else { $reader.Text -match ('"' + $k + '"') }
  if (-not $read) { $problems.Add("$writer [$section] $key is written by ConfigurationWriters.cs but never read by the component") }
}
# The other direction for the engine module: every key native/dcsqvcull/src/main/config_load.inc reads in a section the
# app writes for it is written (the list above) or named in scripts/module-only-keys.txt with a reason; a stale line
# there (written now, or not read any more) is a problem too. Sections the app never writes (Night, Perf, Profile,
# Views) are the module's own.
$loadPath = Join-Path $Root 'native/dcsqvcull/src/main/config_load.inc'
$onlyPath = Join-Path $Root 'scripts/module-only-keys.txt'
if ((Test-Path -LiteralPath $loadPath) -and (Test-Path -LiteralPath $onlyPath)) {
  $writtenQv = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
  foreach ($line in [IO.File]::ReadAllLines($list)) { if ($line.StartsWith("DcsQvCull`t")) { [void]$writtenQv.Add($line.Substring(10)) } }
  $sections = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
  foreach ($k in $writtenQv) { [void]$sections.Add($k.Split("`t")[0]) }
  $readQv = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
  foreach ($m in [regex]::Matches([IO.File]::ReadAllText($loadPath), 'L"(\w+)",\s*L"(\w+)"')) { [void]$readQv.Add($m.Groups[1].Value + "`t" + $m.Groups[2].Value) }
  $only = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
  foreach ($line in [IO.File]::ReadAllLines($onlyPath)) {
    if (-not $line.Trim() -or $line.StartsWith('#')) { continue }
    $f = $line.Split("`t")
    if ($f.Count -lt 3 -or -not $f[2].Trim()) { $problems.Add("module-only-keys.txt: '$line' needs section, key and a reason (tab separated)"); continue }
    $k = $f[0] + "`t" + $f[1]; [void]$only.Add($k)
    if ($writtenQv.Contains($k)) { $problems.Add("module-only-keys.txt: [$($f[0])] $($f[1]) is written by the app now; remove the line") }
    elseif (-not $readQv.Contains($k)) { $problems.Add("module-only-keys.txt: [$($f[0])] $($f[1]) is not read by config_load.inc any more; remove the line") }
  }
  $moduleOnly = 0
  foreach ($k in $readQv) {
    if (-not $sections.Contains($k.Split("`t")[0]) -or $writtenQv.Contains($k)) { continue }
    if ($only.Contains($k)) { ++$moduleOnly; continue }
    $s, $n = $k.Split("`t")
    $problems.Add("DcsQvCull [$s] $n is read by config_load.inc but never written by the app: write it, or list it in scripts/module-only-keys.txt with the reason its module default is what ships")
  }
  Write-Host "engine module keys read in the app's sections: $($readQv.Count) read in config_load.inc, $moduleOnly module-only (listed)"
}
Write-Host ("written keys checked: " + (($counts.GetEnumerator() | ForEach-Object { "$($_.Key) $($_.Value)" }) -join ', ') + $(if ($skipped.Count) { "; sources not here (external checkout missing), skipped: $($skipped -join ', ')" } else { '' }))
if ($problems.Count) { $problems | ForEach-Object { Write-Host "PROBLEM: $_" }; exit 1 }
Write-Host 'written keys: every one is read'
exit 0
