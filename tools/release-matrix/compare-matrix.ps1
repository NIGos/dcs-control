#Requires -Version 7
# Release gate: compares a matrix run with an earlier one (both results.json of run-matrix.ps1).
#
#   pwsh -File tools\release-matrix\compare-matrix.ps1 -Baseline <results.json> -Current <results.json> [-Matrix <matrix.json>] [-Markdown <out.md>] [-Json <out.json>]
#   pwsh -File tools\release-matrix\compare-matrix.ps1 -Current <results.json>        (cost rule only, no baseline)
#
# Flags (exit code 1 when any is raised; 2 on unusable input; 0 = pass):
#  regression  a part's render-thread ms/frame or p95 frame time effect (change % ON vs its OFF arm, measured in the same
#              flight) grew by more than max(2 x combined SE, 3 %) percentage points against the baseline;
#  cost        a part's render-thread change vs its OFF arm (stock; observe for the prefetch fix) is above +2 % and
#              above 2 SE in the current run;
#  missing     a scene or part of the baseline (or of the current matrix) was not measured.
# The limits come from the matrix's "gate" (the current results carry a copy; -Matrix overrides). A partial baseline
# compares only what it holds; the rest is listed as notes. Differences of package, profile or matrix version are
# printed as warnings: they do not flag by themselves.
param([string]$Baseline, [Parameter(Mandatory)][string]$Current, [string]$Matrix, [string]$Markdown, [string]$Json, [switch]$Quiet)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'matrix-lib.ps1')
try {
  $cur = Get-Content -LiteralPath $Current -Raw | ConvertFrom-Json
  $base = if ($Baseline) { Get-Content -LiteralPath $Baseline -Raw | ConvertFrom-Json } else { $null }
  $gate = if ($Matrix) { (Get-Content -LiteralPath $Matrix -Raw | ConvertFrom-Json).gate } elseif ($cur.gate) { $cur.gate } else { $null }
  if ($cur.schema -ne 1 -or ($base -and $base.schema -ne 1)) { throw 'results schema must be 1' }
  if (-not $cur.scenes) { throw 'current results have no scenes' }
} catch { Write-Host "compare-matrix: unusable input: $_"; exit 2 }

$warn = @()
if ($base) {
  if ($base.matrixVersion -ne $cur.matrixVersion) { $warn += "matrix version differs (baseline $($base.matrixVersion), current $($cur.matrixVersion)): scenes or suite keys changed" }
  if ($base.profileSha256 -and $cur.profileSha256 -and $base.profileSha256 -ne $cur.profileSha256) { $warn += 'the flown profile differs from the baseline''s (profile.json without the developer keys)' }
  foreach ($s in @($cur.scenes)) {
    $b = @($base.scenes | Where-Object { $_.id -eq $s.id }) | Select-Object -First 1
    if ($b -and $b.fpsCap -and $s.fpsCap -and [double]$b.fpsCap -ne [double]$s.fpsCap) { $warn += "scene $($s.id): fps cap $($b.fpsCap) in the baseline, $($s.fpsCap) now" }
  }
  if ($base.partial) { $warn += "the baseline is PARTIAL: $($base.partialWhy)" }
}
$f = @(Compare-MatrixResults $base $cur $gate)
$flags = @($f | Where-Object { $_.flag })
$pay = [string]$cur.package.payloadSha256
$lines = [Collections.Generic.List[string]]::new()
$lines.Add('# Release matrix comparison')
$lines.Add('')
$lines.Add("Current: $($cur.label) ($($cur.package.cliVersion), payload $($pay.Substring(0, [Math]::Min(12, $pay.Length))), $($cur.created))")
$lines.Add("Baseline: $(if ($base) { "$($base.label) ($($base.package.cliVersion), $($base.created))$(if ($base.partial) { ', PARTIAL' })" } else { 'none (cost rule only)' })")
$lines.Add('')
$lines.Add("**Verdict: $(if ($flags.Count) { "FAIL, $($flags.Count) flag(s)" } else { 'PASS' })**")
$lines.Add('')
foreach ($w in $warn) { $lines.Add("- warning: $w") }
if ($warn.Count) { $lines.Add('') }
$lines.Add('| Scene | Part | Rule | Flag | Detail |')
$lines.Add('|---|---|---|---|---|')
foreach ($x in ($f | Sort-Object { -[int]$_.flag }, { $_.scene }, { $_.part }, { $_.rule })) { $lines.Add("| $($x.scene) | $($x.part) | $($x.rule) | $(if ($x.flag) { '**FLAG**' } else { '' }) | $($x.text) |") }
if (-not $Quiet) { $lines | ForEach-Object { Write-Host $_ } }
if ($Markdown) { [IO.File]::WriteAllLines($Markdown, $lines) }
if ($Json) { [IO.File]::WriteAllText($Json, ([ordered]@{ verdict = $(if ($flags.Count) { 'fail' } else { 'pass' }); flags = $flags.Count; warnings = $warn; findings = $f } | ConvertTo-Json -Depth 6)) }
exit $(if ($flags.Count) { 1 } else { 0 })
