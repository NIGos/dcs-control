#Requires -Version 7
# Offline tests of the release matrix (no DCS, no headset): paired statistics, report parsing, the gate rules,
# compare-matrix.ps1's exit codes, the matrix definition, ini edits. Synthetic inputs only.
#   pwsh -File tools\release-matrix\tests\test-matrix.ps1        exit 0 = all passed
$ErrorActionPreference = 'Stop'
$T = Split-Path $PSScriptRoot -Parent
. (Join-Path $T 'matrix-lib.ps1')
$Script:pass = 0; $Script:fail = [Collections.Generic.List[string]]::new()
function Check([string]$Name, [bool]$Ok, [string]$Got = '') { if ($Ok) { $Script:pass++ } else { $Script:fail.Add("$Name$(if ($Got) { " (got: $Got)" })") } }
function Near($a, $b, $eps = 0.001) { $null -ne $a -and [Math]::Abs([double]$a - [double]$b) -le $eps }
$tmp = Join-Path ([IO.Path]::GetTempPath()) "release-matrix-test-$PID"; New-Item -ItemType Directory -Force $tmp | Out-Null

# ---- paired statistics (the module's bench.h method) ---------------------------------------------------------------
$rows = @(
  [pscustomobject]@{ on = 0; frames = 180; rt = 10.0; p = 23.0 }, [pscustomobject]@{ on = 1; frames = 180; rt = 11.0; p = 23.1 },
  [pscustomobject]@{ on = 0; frames = 180; rt = 10.0; p = 23.0 }, [pscustomobject]@{ on = 1; frames = 180; rt = 11.2; p = 23.0 },
  [pscustomobject]@{ on = 0; frames = 180; rt = 10.0; p = 23.0 }, [pscustomobject]@{ on = 1; frames = 180; rt = 10.8; p = 22.9 })
$st = Get-PairedStats $rows 'rt'
# ON blocks vs their OFF neighbours: +10 %, +12 %, +8 % (the last ON block has one neighbour) -> mean 10, sd 2, SE 2/sqrt 3
Check 'stats: change % = mean of the paired differences' (Near $st.changePct 10.0) $st.changePct
Check 'stats: SE = sd / sqrt n' (Near $st.sePct (2 / [Math]::Sqrt(3))) $st.sePct
Check 'stats: 95 % interval with t(2) = 4.30' (Near $st.ci95Pct (4.30 * 2 / [Math]::Sqrt(3)) 0.002) $st.ci95Pct
Check 'stats: means and n' ((Near $st.off 10) -and (Near $st.on 11) -and $st.nPairs -eq 3 -and $st.nBlocks -eq 6 -and (Near $st.deltaAbs 1)) "$($st.off) $($st.on) $($st.nPairs) $($st.nBlocks)"
$rows2 = @($rows | ForEach-Object { $_.PSObject.Copy() }); $rows2[2].rt = -1      # not measured: left out, its neighbours pair with one side
$st2 = Get-PairedStats $rows2 'rt'
Check 'stats: -1 values are left out' ($st2.nPairs -eq 3 -and (Near $st2.off 10)) "$($st2.nPairs) $($st2.off)"
$rows3 = @($rows | ForEach-Object { $_.PSObject.Copy() }); $rows3[1].frames = 0
Check 'stats: blocks with 0 frames are left out' ((Get-PairedStats $rows3 'rt').nPairs -eq 2)
Check 'stats: no ON blocks -> null' ($null -eq (Get-PairedStats @($rows | Where-Object { $_.on -eq 0 }) 'rt'))

# ---- report parsing -------------------------------------------------------------------------------------------------
$rep = @'
==== DcsQvCull test suite 2026-11-23 02:17 ====
suite label: release matrix v1 stock-sparse
-- W1 water skip: GPU frame period and render-thread time, OFF (key 0) / ON ([Water] SkipHiddenWater=3) blocks (w1_water.h, 8 pairs, 90 frames per block) --
  water skip timing: 8 pairs (ON = mode 3), 90 frames per block; ON minus neighbouring OFF (+- standard error): GPU frame period -0.001 +- 0.004 ms; blocks in water_timing_20261123_021812.csv
-- A/B: terrain prefetch fix (OFF = observe, every call to the kernel; ON = repeats skipped) --
bench: start, 16 blocks x 5 s (settle 1.0 s), alternating OFF/ON
==== BENCH RESULT: terrain prefetch fix (observe vs skip) (16 blocks, file bench_20261123_022054.csv) ====
NOTE: frame rate was locked at 45.0 fps (refresh 90 Hz / 2) in both OFF and ON blocks; fps cannot show CPU gains in this run.
  p95_frame_ms                 OFF    23.207  ON    23.178  change  -0.14% +/- 0.35% (within noise)
  render_thread_cpu_ms/frame   OFF     9.920  ON    11.221  change +13.37% +/- 1.71% (significant)
==== end of bench, optimisation restored to OFF ====
-- A/B: shadow recorder (OFF = DCS draws every caster) --
  prefetch fix unavailable this session: bench skipped
==== BENCH RESULT: all engine optimizations vs stock DCS (16 blocks, file bench_20261123_023000.csv) ====
  render_thread_cpu_ms/frame   OFF    10.000  ON    11.000  change +10.00% +/- 1.00% (significant)
==== end of bench, optimisation restored to OFF ====
==== suite finished ====
'@
$r = Read-SuiteReport $rep
Check 'report: label and end' ($r.label -eq 'release matrix v1 stock-sparse' -and $r.ended -eq 'finished') "$($r.label) / $($r.ended)"
Check 'report: two bench sections' ($r.benches.Count -eq 2) $r.benches.Count
Check 'report: bench name, blocks, csv, cap' ($r.benches[0].name -eq 'terrain prefetch fix (observe vs skip)' -and $r.benches[0].blocks -eq 16 -and $r.benches[0].csv -eq 'bench_20261123_022054.csv' -and $r.benches[0].capFps -eq 45.0) ($r.benches[0] | ConvertTo-Json -Compress -Depth 4)
Check 'report: module lines' ((Near $r.benches[0].module['render_thread_cpu_ms/frame'].changePct 13.37) -and (Near $r.benches[0].module['p95_frame_ms'].ci95Pct 0.35))
Check 'report: no cap note -> null' ($null -eq $r.benches[1].capFps)
Check 'report: water csv' ($r.water.csv -eq 'water_timing_20261123_021812.csv')
Check 'report: skipped bench noted' (@($r.skipped | Where-Object { $_ -match 'bench skipped' }).Count -eq 1) ($r.skipped -join '|')
Check 'report: aborted suite' ((Read-SuiteReport '==== suite aborted ====').ended -eq 'aborted')

# ---- scene parts from a flight folder (report + csv files) --------------------------------------------------------
$M = Get-Content -LiteralPath (Join-Path $T 'matrix.json') -Raw | ConvertFrom-Json
$fd = Join-Path $tmp 'flight'; $dev = Join-Path $tmp 'dev'; New-Item -ItemType Directory -Force (Join-Path $fd 'module'), $dev | Out-Null
Set-Content -LiteralPath (Join-Path $fd 'module\report_20261123_021711.txt') -Value $rep
$hdr = 'block,on,seconds,frames,fps,frame_ms,p95_ms,process_cpu_ms_per_frame,collect_thread_cpu_ms_per_frame,collect_ms,periph_renderables,focus_renderables,gpu_util,render_thread_cpu_ms_per_frame,render_thread_busy_pct,pass_ms_per_frame,gbuffer_pass_ms_per_frame'
$csv = @($hdr) + @(0..7 | ForEach-Object { $on = $_ % 2; "$_,$on,4.04,182,45.0,22.22,$(23.0 + 0.1 * $on),$(80 - 10 * $on),0.4,0.4,75,68,60,$(10 + $on),50,6,0.4" })
Set-Content -LiteralPath (Join-Path $fd 'module\bench_20261123_022054.csv') -Value $csv
Set-Content -LiteralPath (Join-Path $dev 'water_timing_20261123_021812.csv') -Value (@('block,on,mode,frames,gpu_period_ms,gpu_busy_ms,render_thread_cpu_ms,water_pass_ms,top_pass_ms') + @(0..4 | ForEach-Object { "$_,$($_ % 2),$(3 * ($_ % 2)),90,22.2,11.5,12.0,0.34,7.1" }))
$g = Get-SceneParts $M $fd $dev
Check 'scene: prefetch ok from the csv' ($g.parts.prefetch.status -eq 'ok' -and (Near $g.parts.prefetch.metrics.render_thread_ms.changePct 10.0) -and $g.parts.prefetch.nBlocks -eq 8) ($g.parts.prefetch | ConvertTo-Json -Compress -Depth 5)
Check 'scene: p95 from the csv' ($g.parts.prefetch.metrics.p95_ms -and $g.parts.prefetch.metrics.p95_ms.changePct -gt 0)
Check 'scene: water ok from the dev folder, engaged' ($g.parts.water.status -eq 'ok' -and $g.parts.water.engaged -and (Near $g.parts.water.metrics.render_thread_ms.changePct 0))
Check 'scene: engine csv not kept -> missing' ($g.parts.engine.status -eq 'missing' -and $g.parts.engine.why -match 'not kept') $g.parts.engine.why
Check 'scene: no split filter bench -> missing' ($g.parts.splitFilter.status -eq 'missing')
Check 'scene: fps cap' ($g.info.fpsCap -eq 45.0)

# ---- the gate ------------------------------------------------------------------------------------------------------
function New-Part([double]$Rt, [double]$RtSe, [double]$P95 = 0, [double]$P95Se = 0.2) {
  [ordered]@{ status = 'ok'; offArm = 'stock'; metrics = [ordered]@{ render_thread_ms = [ordered]@{ off = 10; on = 10 * (1 + $Rt / 100); changePct = $Rt; sePct = $RtSe }; p95_ms = [ordered]@{ off = 23; on = 23; changePct = $P95; sePct = $P95Se } } }
}
function New-Res($Parts, [string]$Id = 'stock-sparse', [string]$Status = 'ok') {
  [ordered]@{ schema = 1; matrixVersion = 1; label = 't'; package = @{ cliVersion = 'x'; payloadSha256 = 'ab' }; scenes = @([ordered]@{ id = $Id; status = $Status; fpsCap = 45; parts = $Parts }) } | ConvertTo-Json -Depth 8 | ConvertFrom-Json
}
$gate = $M.gate
function Get-Flags($Old, $New) { @(Compare-MatrixResults $Old $New $gate | Where-Object { $_.flag }) }
function Get-Texts($Items) { (@($Items) | ForEach-Object { $_.text }) -join ' | ' }
$base = New-Res ([ordered]@{ engine = (New-Part -10 0.5); prefetch = (New-Part 0.5 0.7) })
Check 'gate: identical -> no flag' ((Get-Flags $base $base).Count -eq 0) (Get-Texts (Get-Flags $base $base))
$cur = New-Res ([ordered]@{ engine = (New-Part -6.5 0.5); prefetch = (New-Part 0.5 0.7) })      # +3.5 pp, limit max(2 x 0.71, 3) = 3
$fl = Get-Flags $base $cur
Check 'gate: engine effect 3.5 pp worse -> regression flag' ($fl.Count -eq 1 -and $fl[0].rule -eq 'regression' -and $fl[0].part -eq 'engine' -and $fl[0].metric -eq 'render_thread_ms') (Get-Texts $fl)
$cur = New-Res ([ordered]@{ engine = (New-Part -7.1 0.5); prefetch = (New-Part 0.5 0.7) })      # +2.9 pp: under the 3 % floor
Check 'gate: 2.9 pp worse with a small SE -> no flag (3 % floor)' ((Get-Flags $base $cur).Count -eq 0)
$bw = New-Res ([ordered]@{ engine = (New-Part -10 2.0) }); $cur = New-Res ([ordered]@{ engine = (New-Part -5.5 2.0) })   # +4.5 pp, limit 2 x sqrt 8 = 5.66
Check 'gate: 4.5 pp worse with SE 2 each -> no flag (2 x combined SE)' ((Get-Flags $bw $cur).Count -eq 0)
$cur = New-Res ([ordered]@{ engine = (New-Part -4 2.0) })                                       # +6 pp > 5.66
Check 'gate: 6 pp worse with SE 2 each -> flag' ((Get-Flags $bw $cur).Count -eq 1)
$cur = New-Res ([ordered]@{ engine = (New-Part -10 0.5 4.0 0.3); prefetch = (New-Part 0.5 0.7) }) # p95 +4 pp
$fl = Get-Flags $base $cur
Check 'gate: p95 regression -> flag' ($fl.Count -eq 1 -and $fl[0].metric -eq 'p95_ms') (Get-Texts $fl)
$cur = New-Res ([ordered]@{ engine = (New-Part -12 0.5); prefetch = (New-Part 0.5 0.7) })
Check 'gate: improvement -> no flag' ((Get-Flags $base $cur).Count -eq 0)
# cost vs stock in the current run alone
$fl = Get-Flags $null (New-Res ([ordered]@{ prefetch = (New-Part 13.4 0.7) }))
Check 'gate: cost +13.4 % +- 0.7 -> flag without a baseline' ($fl.Count -eq 1 -and $fl[0].rule -eq 'cost') (Get-Texts $fl)
Check 'gate: cost +2.5 % +- 0.5 -> flag' ((Get-Flags $null (New-Res ([ordered]@{ x = (New-Part 2.5 0.5) }))).Count -eq 1)
Check 'gate: cost +2.5 % +- 2.0 -> no flag (within 2 SE)' ((Get-Flags $null (New-Res ([ordered]@{ x = (New-Part 2.5 2.0) }))).Count -eq 0)
Check 'gate: cost +1.9 % +- 0.1 -> no flag (under +2 %)' ((Get-Flags $null (New-Res ([ordered]@{ x = (New-Part 1.9 0.1) }))).Count -eq 0)
Check 'gate: gain -12 % -> no flag' ((Get-Flags $null (New-Res ([ordered]@{ x = (New-Part -12 0.5) }))).Count -eq 0)
# missing things
$fl = Get-Flags $base (New-Res ([ordered]@{ engine = (New-Part -10 0.5) }))
Check 'gate: part in the baseline missing now -> flag' ($fl.Count -eq 1 -and $fl[0].rule -eq 'missing' -and $fl[0].part -eq 'prefetch') (Get-Texts $fl)
$cur = New-Res ([ordered]@{ engine = (New-Part -10 0.5); prefetch = [ordered]@{ status = 'missing'; why = 'bench skipped' } })
Check 'gate: part measured as missing -> flag' ((Get-Flags $base $cur).Count -eq 1)
$fl = Get-Flags $base (New-Res ([ordered]@{ engine = (New-Part -10 0.5); prefetch = (New-Part 0.5 0.7) }) 'stock-dense')
Check 'gate: baseline scene not run -> flag' (@($fl | Where-Object { $_.scene -eq 'stock-sparse' -and $_.rule -eq 'missing' }).Count -eq 1) (Get-Texts $fl)
$fl = Get-Flags $base (New-Res ([ordered]@{ engine = (New-Part -10 0.5) }) 'stock-sparse' 'failed')
Check 'gate: failed scene -> flag' (@($fl | Where-Object { $_.part -eq '*' }).Count -eq 1) (Get-Texts $fl)
# a partial baseline: what it lacks is a note, not a flag
$pb = New-Res ([ordered]@{ engine = (New-Part -10 0.5) })
$all = Compare-MatrixResults $pb (New-Res ([ordered]@{ engine = (New-Part -10 0.5); water = (New-Part 0.1 0.2) })) $gate
Check 'gate: part absent from a partial baseline -> note only' (@($all | Where-Object { $_.flag }).Count -eq 0 -and @($all | Where-Object { $_.part -eq 'water' -and $_.rule -eq 'note' }).Count -eq 1)
$two = [ordered]@{ schema = 1; matrixVersion = 1; scenes = @((New-Res ([ordered]@{ engine = (New-Part -10 0.5) })).scenes[0], (New-Res ([ordered]@{ engine = (New-Part -10 0.5) }) 'atmosx-dense').scenes[0]) } | ConvertTo-Json -Depth 9 | ConvertFrom-Json
$all = Compare-MatrixResults $pb $two $gate
Check 'gate: scene absent from a partial baseline -> note only' (@($all | Where-Object { $_.flag }).Count -eq 0 -and @($all | Where-Object { $_.scene -eq 'atmosx-dense' -and $_.rule -eq 'note' }).Count -ge 1)

# ---- compare-matrix.ps1 exit codes ----------------------------------------------------------------------------------
$cmp = Join-Path $T 'compare-matrix.ps1'
$bf = Join-Path $tmp 'b.json'; $cf = Join-Path $tmp 'c.json'; $xf = Join-Path $tmp 'bad.json'
$base | ConvertTo-Json -Depth 9 | Set-Content $bf
New-Res ([ordered]@{ engine = (New-Part -10 0.5); prefetch = (New-Part 0.5 0.7) }) | ConvertTo-Json -Depth 9 | Set-Content $cf
& pwsh -NoProfile -File $cmp -Baseline $bf -Current $cf -Quiet; Check 'compare-matrix: pass -> exit 0' ($LASTEXITCODE -eq 0) $LASTEXITCODE
New-Res ([ordered]@{ engine = (New-Part -5 0.5); prefetch = (New-Part 0.5 0.7) }) | ConvertTo-Json -Depth 9 | Set-Content $cf
& pwsh -NoProfile -File $cmp -Baseline $bf -Current $cf -Quiet -Markdown (Join-Path $tmp 'g.md') -Json (Join-Path $tmp 'g.json'); Check 'compare-matrix: regression -> exit 1' ($LASTEXITCODE -eq 1) $LASTEXITCODE
Check 'compare-matrix: markdown and json written' ((Get-Content (Join-Path $tmp 'g.md') -Raw) -match 'FAIL, 1 flag' -and (Get-Content (Join-Path $tmp 'g.json') -Raw | ConvertFrom-Json).verdict -eq 'fail')
Set-Content $xf '{ not json'
& pwsh -NoProfile -File $cmp -Baseline $bf -Current $xf -Quiet *> $null; Check 'compare-matrix: unusable input -> exit 2' ($LASTEXITCODE -eq 2) $LASTEXITCODE
$pbf = Join-Path $T 'baselines\0.7.0-preview-cdf337d-partial.json'
if (Test-Path -LiteralPath $pbf) {
  & pwsh -NoProfile -File $cmp -Baseline $pbf -Current $pbf -Quiet -Json (Join-Path $tmp 'self.json')
  $j = Get-Content (Join-Path $tmp 'self.json') -Raw | ConvertFrom-Json
  $costParts = @($j.findings | Where-Object { $_.flag } | ForEach-Object { "$($_.rule):$($_.part)" } | Sort-Object)
  Check 'stored baseline against itself: only the known costs flag (prefetch skip, engine in sparse stock)' (($costParts -join ',') -eq 'cost:engine,cost:prefetch') ($costParts -join ',')
}

# ---- matrix definition and ini text ---------------------------------------------------------------------------------
Check 'matrix.json is valid' ((Test-MatrixDefinition $M).Count -eq 0) ((Test-MatrixDefinition $M) -join '; ')
$bad = $M | ConvertTo-Json -Depth 9 | ConvertFrom-Json; $bad.scenes[1].id = $bad.scenes[0].id; $bad.suite.ini.Suite.BenchEngine = '0'; $bad.missions.dense.file = 'x\y.miz'
$bp = Test-MatrixDefinition $bad
Check 'matrix: duplicate id, part without its bench key, mission path -> 3 problems' ($bp.Count -eq 3) ($bp -join '; ')
$t = "[Suite]`r`nQuick=0`r`n`r`n[Bench]`r`nBlocks=4`r`n"
$t = Set-IniText $t 'Suite' 'Quick' '1'; $t = Set-IniText $t 'Suite' 'BenchEngine' '1'; $t = Set-IniText $t 'Dev' 'IniPath' 'x'
Check 'ini: key replaced, key added in its section, new section at the end' ($t -match '(?s)\[Suite\]\r\nQuick=1\r\nBenchEngine=1\r\n\r\n\[Bench\]\r\nBlocks=4.*\[Dev\]\r\nIniPath=x') $t
Check 'relative paths never leave the root' ((Get-RelPath 'C:\a\b' 'C:\a\b\c\d.txt') -eq 'c/d.txt' -and (Get-RelPath 'C:\a\b' 'D:\x\y.txt') -eq 'y.txt' -and (Get-RelPath 'C:\a\b' 'C:\z\y.txt') -eq 'y.txt')

Remove-Item -LiteralPath $tmp -Recurse -Force -ErrorAction SilentlyContinue
Write-Host "release-matrix tests: $Script:pass passed, $($Script:fail.Count) failed"
$Script:fail | ForEach-Object { Write-Host "  FAIL $_" }
exit $(if ($Script:fail.Count) { 1 } else { 0 })
