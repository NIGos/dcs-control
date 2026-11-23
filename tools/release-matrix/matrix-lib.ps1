#Requires -Version 7
# Shared code of the release flight matrix: matrix validation, paired block statistics, suite report parsing, the
# results of a scene, the release gate. Dot-sourced by run-matrix.ps1, compare-matrix.ps1 and the tests. No side effects.

# ---- paths --------------------------------------------------------------------------------------------------------
# The main checkout of this repository (a worktree's artifacts\ and tool binaries live there; the quiet lock is the
# machine's one and only lock).
function Get-MainCheckout([string]$From) {
  $common = @(& git -C $From rev-parse --path-format=absolute --git-common-dir 2>$null)   # all of it: Select-Object -First would stop git early
  if ($LASTEXITCODE -eq 0 -and $common.Count -and $common[0]) { return [IO.Path]::GetFullPath((Split-Path ([string]$common[0]).Trim() -Parent)) }
  [IO.Path]::GetFullPath((Join-Path $From '..\..'))
}
# A path made relative to $Root for anything written into results (no user paths in results or baselines).
function Get-RelPath([string]$Root, [string]$Path) {
  if (-not $Path) { return $null }
  $r = $null
  if ([IO.Path]::GetPathRoot($Root) -ieq [IO.Path]::GetPathRoot($Path)) { try { $r = [IO.Path]::GetRelativePath($Root, $Path) } catch { $r = $null } }
  if (-not $r -or $r.StartsWith('..') -or [IO.Path]::IsPathRooted($r)) { return (Split-Path $Path -Leaf) }
  $r.Replace('\', '/')
}

# ---- ini text ----------------------------------------------------------------------------------------------------
# key=value in the LAST [Section] of that name (added at its end, or a new section at the file's end).
function Set-IniText([string]$Text, [string]$Section, [string]$Key, [string]$Value) {
  $nl = if ($Text.Contains("`r`n")) { "`r`n" } else { "`n" }
  $lines = [Collections.Generic.List[string]]($Text -split "`r?`n"); $start = -1
  for ($i = 0; $i -lt $lines.Count; $i++) { if ($lines[$i].Trim() -ieq "[$Section]") { $start = $i } }
  if ($start -lt 0) { while ($lines.Count -and $lines[$lines.Count - 1].Trim() -eq '') { $lines.RemoveAt($lines.Count - 1) }; $lines.Add(''); $lines.Add("[$Section]"); $lines.Add("$Key=$Value"); $lines.Add(''); return ($lines -join $nl) }
  $end = $lines.Count
  for ($i = $start + 1; $i -lt $lines.Count; $i++) { if ($lines[$i].TrimStart().StartsWith('[')) { $end = $i; break } }
  $rx = '^\s*' + [regex]::Escape($Key) + '\s*='; $found = -1
  for ($i = $start + 1; $i -lt $end; $i++) { if ($lines[$i] -match $rx) { $found = $i } }
  if ($found -ge 0) { $lines[$found] = "$Key=$Value" } else { $at = $end; while ($at -gt $start + 1 -and $lines[$at - 1].Trim() -eq '') { $at-- }; $lines.Insert($at, "$Key=$Value") }
  $lines -join $nl
}

# ---- matrix definition ---------------------------------------------------------------------------------------------
function Test-MatrixDefinition($M) {
  $p = [Collections.Generic.List[string]]::new()
  if ($M.schema -ne 1) { $p.Add("schema must be 1 (is '$($M.schema)')") }
  if (-not $M.version) { $p.Add('version missing') }
  $scenes = @($M.scenes | Where-Object { $_ })
  if (-not $scenes.Count) { $p.Add('no scenes') }
  $ids = @($scenes | ForEach-Object { [string]$_.id })
  foreach ($d in @($ids | Group-Object | Where-Object { $_.Count -gt 1 })) { $p.Add("scene id '$($d.Name)' is not unique") }
  foreach ($s in $scenes) {
    if (-not $s.id -or [string]$s.id -notmatch '^[a-z0-9-]+$') { $p.Add("scene id '$($s.id)': lower case letters, digits and '-' only") }
    if ($s.atmosx -isnot [bool]) { $p.Add("scene $($s.id): atmosx must be true or false") }
    if (-not $s.mission -or -not $M.missions.([string]$s.mission)) { $p.Add("scene $($s.id): mission '$($s.mission)' is not in missions") }
    if ($null -eq $s.suiteAt -or [double]$s.suiteAt -lt 5 -or [double]$s.suiteAt -gt 300) { $p.Add("scene $($s.id): suiteAt must be 5-300 s after FLY") }
  }
  foreach ($mp in @($M.missions.PSObject.Properties)) {
    $file = [string]$mp.Value.file
    if (-not $file -or $file -notmatch '\.miz$' -or $file.Contains('\') -or $file.Contains('/')) { $p.Add("mission $($mp.Name): 'file' must be a bare .miz file name") }
    if ($mp.Value.sha256 -and [string]$mp.Value.sha256 -notmatch '^[0-9a-fA-F]{64}$') { $p.Add("mission $($mp.Name): sha256 is not 64 hex digits") }
  }
  $parts = @($M.parts | Where-Object { $_ })
  if (-not $parts.Count) { $p.Add('no parts') }
  foreach ($d in @($parts | ForEach-Object { [string]$_.id } | Group-Object | Where-Object { $_.Count -gt 1 })) { $p.Add("part id '$($d.Name)' is not unique") }
  foreach ($pt in $parts) {
    if ($pt.kind -notin 'bench', 'water') { $p.Add("part $($pt.id): kind must be bench or water") }
    if ($pt.kind -eq 'bench') {
      if (-not $pt.bench) { $p.Add("part $($pt.id): bench (the report's BENCH RESULT name, a regex) missing") }
      else { try { [void][regex]::new([string]$pt.bench) } catch { $p.Add("part $($pt.id): bench is not a valid regex") } }
    }
  }
  if (@($parts | Where-Object { $_.kind -eq 'water' }).Count -gt 1) { $p.Add('only one water part') }
  $ini = $M.suite.ini
  if (-not $ini -or -not $ini.Suite -or -not $ini.Bench) { $p.Add('suite.ini needs [Suite] and [Bench]') }
  else {
    foreach ($pt in $parts) {
      $need = switch ([string]$pt.id) { 'splitFilter' { 'BenchSplitFilter' } 'prefetch' { 'BenchPrefetch' } 'shadowRecorder' { 'BenchShadowRecorder' } 'gbufferRecorder' { 'BenchGBufferRecorder' } 'engine' { 'BenchEngine' } 'water' { 'WaterSkipTiming' } default { $null } }
      if ($need -and -not ([int]$ini.Suite.$need -gt 0)) { $p.Add("part $($pt.id) needs [Suite] $need set (> 0)") }
    }
    if ([int]$ini.Bench.Blocks -lt 8) { $p.Add('[Bench] Blocks should be at least 8 (4 pairs)') }
    if ([string]$ini.Bench.PassMs -ne '1') { $p.Add('[Bench] PassMs must be 1 (render-thread pass ms per block)') }
  }
  foreach ($k in 'render_thread_ms', 'p95_ms') { if (-not $M.metrics.$k) { $p.Add("metrics.$k missing") } }
  $g = $M.gate
  foreach ($k in 'regressionSeFactor', 'regressionFloorPct', 'costMaxPct', 'costSeFactor') { if ($null -eq $g.$k) { $p.Add("gate.$k missing") } }
  foreach ($k in @($g.regressionMetrics)) { if (-not $M.metrics.$k) { $p.Add("gate.regressionMetrics: '$k' is not in metrics") } }
  , $p
}

# ---- statistics ---------------------------------------------------------------------------------------------------
# Student's t (95 %, two-sided) by degrees of freedom, as the module's bench.h uses it.
$Script:T95 = @(0, 12.71, 4.30, 3.18, 2.78, 2.57, 2.45, 2.36, 2.31, 2.26, 2.23, 2.20, 2.18)
# The module's own A/B statistic (bench.h): each ON block against the mean of its OFF neighbours, in % of that mean
# (cancels slow drift). Returns OFF / ON means, mean change %, its standard error (sd / sqrt n) and the module's 95 %
# interval (t * SE), n pairs, n blocks, and the ON - OFF difference of the means in the metric's unit. $Rows: objects
# with an 'on' (0/1) property and the column; values < 0 (= not measured) and blocks with 0 frames are left out.
function Get-PairedStats($Rows, [string]$Column) {
  $rows = @($Rows | Where-Object { $_ })
  $v = @(foreach ($r in $rows) {
      $x = $r.$Column; $ok = $null -ne $x -and "$x" -ne '' -and [double]$x -ge 0
      if ($null -ne $r.frames -and "$($r.frames)" -ne '' -and [double]$r.frames -le 0) { $ok = $false }
      [pscustomobject]@{ on = ([int]$r.on -ne 0); val = $(if ($ok) { [double]$x } else { $null }) }
    })
  $on = @($v | Where-Object { $_.on -and $null -ne $_.val } | ForEach-Object { $_.val })
  $off = @($v | Where-Object { -not $_.on -and $null -ne $_.val } | ForEach-Object { $_.val })
  if (-not $on.Count -or -not $off.Count) { return $null }
  $d = [Collections.Generic.List[double]]::new()
  for ($i = 0; $i -lt $v.Count; $i++) {
    if (-not $v[$i].on -or $null -eq $v[$i].val) { continue }
    $b = 0.0; $nb = 0
    if ($i -gt 0 -and -not $v[$i - 1].on -and $null -ne $v[$i - 1].val) { $b += $v[$i - 1].val; $nb++ }
    if ($i + 1 -lt $v.Count -and -not $v[$i + 1].on -and $null -ne $v[$i + 1].val) { $b += $v[$i + 1].val; $nb++ }
    if (-not $nb) { continue }; $b /= $nb
    if ($b -ne 0) { $d.Add(($v[$i].val - $b) / $b * 100.0) }
  }
  $n = $d.Count; $mean = 0.0; foreach ($x in $d) { $mean += $x }; if ($n) { $mean /= $n }
  $sd = 0.0; foreach ($x in $d) { $sd += ($x - $mean) * ($x - $mean) }; if ($n -gt 1) { $sd = [Math]::Sqrt($sd / ($n - 1)) } else { $sd = 0.0 }
  $se = if ($n -gt 1) { $sd / [Math]::Sqrt($n) } else { $null }
  $dof = [Math]::Max(0, $n - 1); $t = if ($dof -lt $Script:T95.Count) { $Script:T95[$dof] } else { 2.10 }
  $offMean = ($off | Measure-Object -Average).Average; $onMean = ($on | Measure-Object -Average).Average
  [ordered]@{
    off = [Math]::Round($offMean, 4); on = [Math]::Round($onMean, 4); deltaAbs = [Math]::Round($onMean - $offMean, 4)
    changePct = [Math]::Round($mean, 3); sePct = $(if ($null -ne $se) { [Math]::Round($se, 3) } else { $null })
    ci95Pct = $(if ($null -ne $se) { [Math]::Round($t * $se, 3) } else { $null }); nPairs = $n; nBlocks = $rows.Count
  }
}
function Import-BlockCsv([string]$Path) { @(Import-Csv -LiteralPath $Path) }

# ---- suite report ---------------------------------------------------------------------------------------------------
# The module's report_*.txt: its bench sections (name, csv, frame-rate cap, the module's own lines), the W1 water
# section (csv), notes of benches skipped, and how the suite ended.
function Read-SuiteReport([string]$Text) {
  $lines = $Text -split "`r?`n"
  $r = [ordered]@{ label = $null; ended = $null; benches = [Collections.Generic.List[object]]::new(); water = $null; skipped = [Collections.Generic.List[string]]::new() }
  $cur = $null; $section = ''
  foreach ($l in $lines) {
    if ($l -match '^suite label: (.+)$') { $r.label = $Matches[1].Trim() }
    if ($l -match '^==== suite (finished|aborted)') { $r.ended = $Matches[1] }
    if ($l -match '^-- (.+?) --\s*$') { $section = $Matches[1] }
    if ($l -match '^\s+(.*(bench skipped|A/B skipped|; skipped).*)$') { $r.skipped.Add("$section`: $($Matches[1].Trim())") }
    if ($l -match '^==== BENCH RESULT: (.+) \((\d+) blocks, file (\S+?)\) ====') {
      $cur = [ordered]@{ name = $Matches[1].Trim(); blocks = [int]$Matches[2]; csv = $Matches[3]; section = $section; capFps = $null; capText = $null; module = [ordered]@{} }
      $r.benches.Add($cur); continue
    }
    if ($cur) {
      if ($l -match '^==== end of bench') { $cur = $null; continue }
      if ($l -match 'locked at ([\d.]+) fps \((refresh [^)]*)\)') { $cur.capFps = [double]$Matches[1]; $cur.capText = $Matches[2] }
      elseif ($l -match 'locked in both states \(OFF ([\d.]+) fps[^,]*, ON ([\d.]+) fps') { $cur.capFps = [double]$Matches[2]; $cur.capText = "OFF $($Matches[1]) / ON $($Matches[2]) fps" }
      if ($l -match '^\s+(\S+)\s+OFF\s+(-?[\d.]+)\s+ON\s+(-?[\d.]+)\s+change\s+([+-]?[\d.]+)%\s+\+/-\s+([\d.]+)%') {
        $cur.module[$Matches[1]] = [ordered]@{ off = [double]$Matches[2]; on = [double]$Matches[3]; changePct = [double]$Matches[4]; ci95Pct = [double]$Matches[5] }
      }
    }
    if ($l -match 'water skip timing: .*blocks in (water_timing_\S+?\.csv)') { $r.water = [ordered]@{ csv = $Matches[1]; section = $section } }
  }
  $r
}

# ---- results of one scene -----------------------------------------------------------------------------------------
# $RunDir: the runner's flight folder (module\report_*.txt, module\bench_*.csv); $DevDir: the developer ini folder (the
# module writes water_timing_*.csv there). Each part of the matrix: status ok / missing, metrics from the block CSV.
function Get-SceneParts($M, [string]$RunDir, [string]$DevDir) {
  $out = [ordered]@{}; $mod = Join-Path $RunDir 'module'
  $rep = @(Get-ChildItem -LiteralPath $mod -Filter 'report_*.txt' -File -ErrorAction SilentlyContinue | Sort-Object Name)
  $info = [ordered]@{ report = $null; ended = $null; skipped = @(); fpsCap = $null }
  if (-not $rep.Count) { foreach ($pt in $M.parts) { $out[[string]$pt.id] = [ordered]@{ status = 'missing'; why = 'no suite report in the flight folder' } }; return @{ parts = $out; info = $info } }
  $rp = Read-SuiteReport ([IO.File]::ReadAllText($rep[-1].FullName))
  $info.report = $rep[-1].Name; $info.ended = $rp.ended; $info.skipped = @($rp.skipped)
  foreach ($pt in $M.parts) {
    $e = [ordered]@{ status = 'missing'; label = $pt.label; offArm = $pt.offArm; benchMode = $pt.benchMode }
    if ($pt.kind -eq 'water') {
      if (-not $rp.water) { $e.why = 'no W1 water timing in the report' }
      else {
        $csv = @((Join-Path $DevDir $rp.water.csv), (Join-Path $mod $rp.water.csv)) | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
        if (-not $csv) { $e.why = "water csv $($rp.water.csv) not found" }
        else {
          $rows = Import-BlockCsv $csv; $e.csv = $rp.water.csv; $e.metrics = [ordered]@{}
          foreach ($mp in $M.metrics.PSObject.Properties) { if ($mp.Value.waterCsv) { $s = Get-PairedStats $rows ([string]$mp.Value.waterCsv); if ($s) { $e.metrics[$mp.Name] = $s } } }
          $e.status = $(if ($e.metrics.Count) { 'ok' } else { 'missing' }); if (-not $e.metrics.Count) { $e.why = 'water csv without usable blocks' }
          $e.nBlocks = $rows.Count; $e.engaged = [bool](@($rows | Where-Object { [int]$_.on -ne 0 -and [int]$_.mode -ne 0 }).Count)
        }
      }
    } else {
      $b = @($rp.benches | Where-Object { $_.name -match [string]$pt.bench }) | Select-Object -Last 1
      if (-not $b) { $e.why = "no '$($pt.bench)' bench result in the report$(if ($rp.skipped.Count) { ' (' + ($rp.skipped -join '; ') + ')' })" }
      else {
        $csv = Join-Path $mod $b.csv
        $e.bench = $b.name; $e.csv = $b.csv; $e.fpsCap = $b.capFps; $e.capText = $b.capText; $e.module = $b.module
        if (-not (Test-Path -LiteralPath $csv)) { $e.why = "bench csv $($b.csv) not kept" }
        else {
          $rows = Import-BlockCsv $csv; $e.metrics = [ordered]@{}
          foreach ($mp in $M.metrics.PSObject.Properties) { if ($mp.Value.csv) { $s = Get-PairedStats $rows ([string]$mp.Value.csv); if ($s) { $e.metrics[$mp.Name] = $s } } }
          $e.nBlocks = $rows.Count
          $e.status = $(if ($e.metrics.render_thread_ms) { 'ok' } else { 'missing' }); if (-not $e.metrics.render_thread_ms) { $e.why = 'render-thread CPU not measured in the blocks' }
          if ($b.capFps -and -not $info.fpsCap) { $info.fpsCap = $b.capFps }
        }
      }
    }
    $out[[string]$pt.id] = $e
  }
  @{ parts = $out; info = $info }
}

# ---- release gate --------------------------------------------------------------------------------------------------
# Findings of Current against Baseline (both results.json objects) under the matrix gate:
#  regression: a part's measured effect (change %, ON vs its OFF arm, same flight) of a regression metric grew by more
#              than max(SeFactor * sqrt(SE_b^2 + SE_c^2), FloorPct) percentage points;
#  cost:       in Current, a part's render-thread change vs its OFF arm is above +CostMaxPct and above CostSeFactor * SE;
#  missing:    a scene or part the baseline has, or the current run lists, is not measured in Current.
# Each finding: scene, part, metric, rule, flag (bool), text. Notes (flag false) say what could not be compared.
function Compare-MatrixResults($Baseline, $Current, $Gate) {
  $found = [Collections.Generic.List[object]]::new()
  $add = { param($s, $p, $m, $rule, [bool]$flag, $t) $found.Add([pscustomobject]@{ scene = $s; part = $p; metric = $m; rule = $rule; flag = $flag; text = $t }) }
  $num = { param($x) if ($null -eq $x) { $null } else { [double]$x } }
  $g = [ordered]@{ regressionMetrics = @('render_thread_ms', 'p95_ms'); regressionSeFactor = 2.0; regressionFloorPct = 3.0; costMetric = 'render_thread_ms'; costMaxPct = 2.0; costSeFactor = 2.0 }
  if ($Gate) { foreach ($gp in $Gate.PSObject.Properties) { $g[$gp.Name] = $gp.Value } }
  $bScenes = @{}; if ($Baseline) { foreach ($s in @($Baseline.scenes)) { $bScenes[[string]$s.id] = $s } }
  $cScenes = @{}; foreach ($s in @($Current.scenes)) { $cScenes[[string]$s.id] = $s }
  foreach ($id in @($bScenes.Keys | Where-Object { -not $cScenes.ContainsKey($_) })) { & $add $id '*' '' 'missing' $true 'scene in the baseline, not in the current results' }
  foreach ($cs in @($Current.scenes)) {
    $id = [string]$cs.id; $bs = $bScenes[$id]
    if ($cs.status -ne 'ok') { & $add $id '*' '' 'missing' $true "scene not measured: $($cs.status)$(if ($cs.why) { " ($($cs.why))" })" }
    if (-not $bs -and $Baseline) { & $add $id '*' '' 'note' $false 'no baseline for this scene' }
    $parts = @(); if ($cs.parts) { $parts = @($cs.parts.PSObject.Properties.Name) }
    if ($bs -and $bs.parts) { $parts = @($parts + @($bs.parts.PSObject.Properties.Name) | Select-Object -Unique) }
    foreach ($pn in $parts) {
      $cp = if ($cs.parts) { $cs.parts.$pn } else { $null }; $bp = if ($bs -and $bs.parts) { $bs.parts.$pn } else { $null }
      $cOk = $cp -and $cp.status -eq 'ok'; $bOk = $bp -and $bp.status -eq 'ok'
      if (-not $cOk) { if ($cs.status -eq 'ok' -or $bOk) { & $add $id $pn '' 'missing' $true "part not measured$(if ($cp.why) { ": $($cp.why)" })" }; continue }
      # cost vs the OFF arm (stock, or observe for the prefetch fix), current alone
      $cm = $cp.metrics.([string]$g.costMetric)
      if ($cm) {
        $ch = & $num $cm.changePct; $se = & $num $cm.sePct
        $over = $ch -gt [double]$g.costMaxPct -and ($null -eq $se -or $ch -gt [double]$g.costSeFactor * $se)
        $seT = if ($null -ne $se) { '{0:N2}' -f $se } else { 'n/a' }
        & $add $id $pn $g.costMetric 'cost' $over ("{0} vs {1}: {2:+0.00;-0.00} % +- {3} (SE), limit +{4} % and {5} SE" -f $g.costMetric, $(if ($cp.offArm) { $cp.offArm } else { 'OFF' }), $ch, $seT, $g.costMaxPct, $g.costSeFactor)
      }
      if (-not $Baseline) { continue }
      if (-not $bOk) { & $add $id $pn '' 'note' $false "no baseline for this part$(if ($bp.why) { " ($($bp.why))" })"; continue }
      foreach ($mk in @($g.regressionMetrics)) {
        $c = $cp.metrics.$mk; $b = $bp.metrics.$mk
        if (-not $c -or -not $b) { & $add $id $pn $mk 'note' $false "$mk not in $(if (-not $c) { 'current' } else { 'baseline' })"; continue }
        $cc = & $num $c.changePct; $bc = & $num $b.changePct; $cse = & $num $c.sePct; $bse = & $num $b.sePct
        $comb = if ($null -ne $cse -and $null -ne $bse) { [Math]::Sqrt($cse * $cse + $bse * $bse) } else { $null }
        $thr = [Math]::Max($(if ($null -ne $comb) { [double]$g.regressionSeFactor * $comb } else { 0 }), [double]$g.regressionFloorPct)
        $d = $cc - $bc; $bad = $d -gt $thr
        & $add $id $pn $mk 'regression' $bad ("{0} change {1:+0.00;-0.00} % -> {2:+0.00;-0.00} % ({3:+0.00;-0.00} pp; limit {4:N2} pp = max({5} x combined SE {6}, {7} %)); ON {8:N3} -> {9:N3}" -f $mk, $bc, $cc, $d, $thr, $g.regressionSeFactor, $(if ($null -ne $comb) { '{0:N2}' -f $comb } else { 'n/a' }), $g.regressionFloorPct, [double]$b.on, [double]$c.on)
      }
    }
  }
  $found.ToArray()   # one finding per pipeline object (callers wrap the call in @())
}
