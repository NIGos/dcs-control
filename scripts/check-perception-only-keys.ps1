#Requires -Version 7
# Static check: with Engine Optimizations off the engine module runs in its Perception-only mode
# ([General] PerceptionOnly=1), and the configuration it applies is PerceptionOnlyConfig(c) (config.inc): a fresh Config
# with only the listed fields copied from the ini. A [Perception] key read into a Config field that the list does not
# copy is silently dead with the engine off; that was Sharp HUD's SymbologyExport until 6420669.
# This script reads native/dcsqvcull/src/main/config_load.inc (every L"Perception" read and the c.<field> it fills) and
# config.inc (PerceptionOnlyConfig's p.<field> = c.<field> lines) and fails when a field is not copied, unless it is in
# $deliberate below with a reason. It also fails when the app writes a [Perception] key (src/DcsVr.Core/
# ConfigurationWriters.cs) that the module never reads (a typo or a removed key).
# Exit 0 = consistent; 1 = a problem (listed).
param([string]$Root = (Split-Path -Parent $PSScriptRoot))
$ErrorActionPreference = 'Stop'
$load = Get-Content -LiteralPath (Join-Path $Root 'native/dcsqvcull/src/main/config_load.inc')
$cfg = [IO.File]::ReadAllText((Join-Path $Root 'native/dcsqvcull/src/main/config.inc'))
$writer = [IO.File]::ReadAllText((Join-Path $Root 'src/DcsVr.Core/ConfigurationWriters.cs'))
# Perception keys that are read but deliberately not applied in Perception-only mode (field -> reason).
$deliberate = @{}

# PerceptionOnlyConfig's body.
$start = $cfg.IndexOf('Config PerceptionOnlyConfig(const Config& c) {'); if ($start -lt 0) { throw 'PerceptionOnlyConfig not found in config.inc' }
$end = $cfg.IndexOf("`n}", $start); $body = $cfg.Substring($start, $end - $start)
$copied = [Collections.Generic.HashSet[string]]::new()
foreach ($m in [regex]::Matches($body, 'p\.(\w+)\s*=\s*c\.(\w+)')) { if ($m.Groups[1].Value -eq $m.Groups[2].Value) { [void]$copied.Add($m.Groups[1].Value) } }

$reads = [ordered]@{}   # key -> field ('' when the value goes elsewhere than a Config field)
for ($i = 0; $i -lt $load.Count; $i++) {
  foreach ($m in [regex]::Matches($load[$i], 'L"Perception",\s*L"(\w+)"')) {
    $key = $m.Groups[1].Value; $field = ''
    # The statement's target: c.<field> = on this line or up to three lines before (a wrapped expression) ...
    for ($j = $i; $j -ge [Math]::Max(0, $i - 3) -and -not $field; $j--) {
      $a = [regex]::Match($load[$j], '\bc\.(\w+)\s*=(?!=)'); if ($a.Success) { $field = $a.Groups[1].Value }
      if ($j -lt $i -and $load[$j].TrimEnd().EndsWith(';')) { break }
    }
    # ... or, for a string read into a buffer, the first c.<field> used within the next six lines.
    if (-not $field -and $load[$i] -match 'GetPrivateProfileStringW') {
      for ($j = $i + 1; $j -le [Math]::Min($load.Count - 1, $i + 6) -and -not $field; $j++) { $a = [regex]::Match($load[$j], '\bc\.(\w+)'); if ($a.Success) { $field = $a.Groups[1].Value } }
    }
    if (-not $reads.Contains($key)) { $reads[$key] = $field }
  }
}
$problems = [Collections.Generic.List[string]]::new(); $direct = [Collections.Generic.List[string]]::new()
foreach ($k in $reads.Keys) {
  $f = $reads[$k]
  if (-not $f) { $direct.Add($k); continue }
  if ($copied.Contains($f) -or $deliberate.ContainsKey($f)) { continue }
  $problems.Add("[Perception] $k -> Config.$f is read but PerceptionOnlyConfig does not copy it: dead with Engine Optimizations off")
}
# Keys the app writes under [Perception] in the engine module's ini.
$sec = ''; $written = [Collections.Generic.HashSet[string]]::new()
foreach ($line in ($writer -split "`n")) {
  $s = [regex]::Match($line, '^\s*\[(\w+)\]\s*$'); if ($s.Success) { $sec = $s.Groups[1].Value; continue }
  if ($sec -eq 'Perception') { $k = [regex]::Match($line, '^\s*(\w+)='); if ($k.Success) { [void]$written.Add($k.Groups[1].Value) } }
}
foreach ($k in $written) { if (-not $reads.Contains($k)) { $problems.Add("[Perception] $k is written by ConfigurationWriters.cs but never read by config_load.inc") } }
Write-Host "perception keys read: $($reads.Count) ($($direct.Count) applied outside Config: $($direct -join ', ')); copied in PerceptionOnlyConfig: $($copied.Count); written by the app: $($written.Count)"
if ($problems.Count) { $problems | ForEach-Object { Write-Host "PROBLEM: $_" }; exit 1 }
Write-Host 'perception-only keys: consistent'
exit 0
