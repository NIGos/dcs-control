#Requires -Version 7
# Parses the files of one live measurement session (native/dcsqvcull/LIVE_MEASUREMENT_PLAN.md: the perf audit's
# questions a-f) into a summary: one table per question and the verdict of each decision rule. Reads only; writes
# the summary to the console and, with -OutFile, to a Markdown file.
#
# Input: a folder with the session's report_*.txt (one per suite run; "suite label: <step>" names the step),
# bench_*.csv, passtl_*.csv and DcsQvCull.log (used only when no report is found).
#
#   pwsh scripts/parse-live-measurement.ps1 -LogDir <folder> [-OutFile <folder>\summary.md]
#   pwsh scripts/parse-live-measurement.ps1 -SelfTest      (offline check of the parsers on built-in lines)
param(
    [string]$LogDir = '',
    [string]$OutFile = '',
    [switch]$SelfTest
)
$ErrorActionPreference = 'Stop'
$inv = [Globalization.CultureInfo]::InvariantCulture
function Num([string]$s) { return [double]::Parse($s, $inv) }

# ---------------------------------------------------------------------------
# Parsers (one report's lines -> objects)
# ---------------------------------------------------------------------------
function Get-Label([string[]]$lines) {
    foreach ($l in $lines) { if ($l -match 'suite label: (.+)$') { return $Matches[1].Trim() } }
    return ''
}

function Get-Timeline([string[]]$lines) {
    $t = [ordered]@{ Frames = $null; QuadFrames = $null; NoBoundary = $false; Mean = $null; P50 = $null; P95 = $null; RtCpu = $null; RtBusy = $null; Kinds = @(); Culling = @() }
    $inCull = $false
    foreach ($l in $lines) {
        if ($l -match 'pass timeline: (\d+) frames \((\d+) quad-view frames.*pass execution mean ([\d.]+) ms, p50 ([\d.]+), p95 ([\d.]+)') {
            $t.Frames = [int]$Matches[1]; $t.QuadFrames = [int64]$Matches[2]
            $t.Mean = Num $Matches[3]; $t.P50 = Num $Matches[4]; $t.P95 = Num $Matches[5]
            # Before the 2026-11-11 fix the frame boundary never fired live (1 timeline frame for thousands of
            # quad-view frames), so the per-frame figures were window totals: reported as n/a, never as ms/frame.
            $t.NoBoundary = ($t.Frames * 2 -lt $t.QuadFrames)
        } elseif ($l -match 'pass timeline: WARNING frame boundary not detected') {
            $t.NoBoundary = $true
        } elseif ($l -match 'pass timeline: render thread CPU ([\d.]+) ms/frame \(busy ([\d.]+)%') {
            $t.RtCpu = Num $Matches[1]; $t.RtBusy = Num $Matches[2]
        } elseif ($l -match '^\s*pass timeline (.+?)\s+([\d.]+)\s+([\d.]+)\s+([\d.]+)\s+([\d.]+)\s*$') {
            $t.Kinds += [pscustomobject]@{ Kind = $Matches[1].Trim(); Mean = Num $Matches[2]; P50 = Num $Matches[3]; P95 = Num $Matches[4]; Calls = Num $Matches[5] }
        } elseif ($l -match 'pass timeline: culling timeline') {
            $inCull = $true
        } elseif ($inCull -and $l -match '^\s+(aggregated \(render thread\)|provider collectRenderables|collect objects|sort\+batch)\s+([\d.]+) ms/frame') {
            $t.Culling += [pscustomobject]@{ Phase = $Matches[1]; Ms = Num $Matches[2] }
        } elseif ($inCull -and $l -match '^--') { $inCull = $false }
    }
    if ($null -eq $t.Frames) { return $null }
    if ($t.NoBoundary) { $t.Mean = $null; $t.P50 = $null; $t.P95 = $null; $t.Kinds = @() }  # window totals, not per frame
    return [pscustomobject]$t
}

# Every "==== BENCH RESULT: <what> (...)" block with its field rows.
function Get-Benches([string[]]$lines) {
    $out = @(); $cur = $null
    foreach ($l in $lines) {
        if ($l -match '==== BENCH RESULT: (.+?) \(\d+ blocks') {
            $cur = [pscustomobject]@{ What = $Matches[1]; Locked = $false; Fields = @{} }
            $out += $cur
        } elseif ($cur -and $l -match '^NOTE: frame rate was locked') {
            $cur.Locked = $true
        } elseif ($cur -and $l -match '^\s+(\S+)\s+OFF\s+(-?[\d.]+)\s+ON\s+(-?[\d.]+)\s+change\s+([-+]?[\d.]+)% \+/- ([\d.]+)% \((significant|within noise)\)') {
            $cur.Fields[$Matches[1]] = [pscustomobject]@{ Off = Num $Matches[2]; On = Num $Matches[3]; ChangePct = Num $Matches[4]; CiPct = Num $Matches[5]; Significant = $Matches[6] -eq 'significant' }
        } elseif ($l -match '==== end of bench') { $cur = $null }
    }
    return $out
}

function Get-FxApply([string[]]$lines) {
    $f = [ordered]@{ SamePerFrame = $null; SamePct = $null; Estimate = $null; Upper = $null }
    foreach ($l in $lines) {
        if ($l -match 'fx apply: same pass as the previous Apply ([\d.]+)/frame \(([\d.]+)%\)') { $f.SamePerFrame = Num $Matches[1]; $f.SamePct = Num $Matches[2] }
        elseif ($l -match 'fx apply estimate: same-pass fast path saves about ([\d.]+) ms/frame \(upper bound ([\d.]+)') { $f.Estimate = Num $Matches[1]; $f.Upper = Num $Matches[2] }
    }
    if ($null -eq $f.Estimate) { return $null }
    return [pscustomobject]$f
}

function Get-Census([string[]]$lines) {
    $c = [ordered]@{ Frames = $null; Chains = $null; ChainRows = @(); Pairs = @(); TerrainCalls = @() }
    foreach ($l in $lines) {
        if ($l -match 'chain census: (\d+) frames compared \((-?\d+) calccounters chains') { $c.Frames = [int]$Matches[1]; $c.Chains = [int]$Matches[2] }
        elseif ($l -match 'chain census #(\d+) (.+) call #(\d+): objects (\d+), instances ([\d.]+)/frame') {
            $c.ChainRows += [pscustomobject]@{ Index = [int]$Matches[1]; Pass = $Matches[2]; Call = [int]$Matches[3]; Objects = [int]$Matches[4]; Instances = Num $Matches[5] }
        } elseif ($l -match 'chain census pair #(\d+) >= #(\d+): counts contained in (\d+)/(\d+) frames .*objects over max (\d+)(.*)$') {
            $c.Pairs += [pscustomobject]@{ A = [int]$Matches[1]; B = [int]$Matches[2]; Contained = [int]$Matches[3]; Frames = [int]$Matches[4]; MaxOver = [int]$Matches[5]; SamePass = $Matches[6] -match 'same pass' }
        } elseif ($l -match '^\s+(TerrainModule\S*)\s+((?:#\d+ [\d.]+\*?\s*)+)$') {
            foreach ($m in [regex]::Matches($Matches[2], '#(\d+) ([\d.]+)')) { $c.TerrainCalls += [pscustomobject]@{ Call = [int]$m.Groups[1].Value; Ms = Num $m.Groups[2].Value } }
        }
    }
    if ($null -eq $c.Frames) { return $null }
    return [pscustomobject]$c
}

function Get-Bundle([string[]]$lines) {
    $blocks = @(); $on = $null; $built = $true
    foreach ($l in $lines) {
        if ($l -match 'gpu bundle: no requested item is built') { $built = $false }
        elseif ($l -match 'gpu bundle block (\d+)/(\d+): (ON|OFF)') { $on = $Matches[3] -eq 'ON' }
        elseif ($null -ne $on -and $l -match 'gpu passes: ([\d.]+) ms/frame exclusive GPU time in passes') {
            $blocks += [pscustomobject]@{ On = $on; Ms = Num $Matches[1] }; $on = $null
        }
    }
    if (-not $blocks) { return $null }
    # ABBA pairs: blocks (0,1), (2,3), ...: ON - OFF per pair.
    $d = @()
    for ($i = 0; $i + 1 -lt $blocks.Count; $i += 2) {
        $a = $blocks[$i]; $b = $blocks[$i + 1]
        if ($a.On -ne $b.On) { $d += $(if ($a.On) { $a.Ms - $b.Ms } else { $b.Ms - $a.Ms }) }
    }
    $mean = 0; $sd = 0
    if ($d.Count) { $mean = ($d | Measure-Object -Average).Average }
    if ($d.Count -gt 1) { $sd = [math]::Sqrt((($d | ForEach-Object { ($_ - $mean) * ($_ - $mean) }) | Measure-Object -Sum).Sum / ($d.Count - 1)) }
    $t = @(0, 12.71, 4.30, 3.18, 2.78, 2.57, 2.45, 2.36, 2.31, 2.26, 2.23, 2.20, 2.18)
    $dof = [math]::Max(0, $d.Count - 1)
    $tv = if ($dof -lt $t.Count) { $t[$dof] } else { 2.10 }
    $ci = if ($d.Count -gt 1) { $tv * $sd / [math]::Sqrt($d.Count) } else { [double]::NaN }
    return [pscustomobject]@{ Built = $built; Blocks = $blocks.Count; Pairs = $d.Count; MeanDiffMs = $mean; Ci95Ms = $ci }
}

function Get-Verify([string[]]$lines) {
    $v = @()
    foreach ($l in $lines) { if ($l -match 'verify: (differences found|0 mismatches|.*NOT SAFE.*)') { $v += $l.Trim() } }
    return $v
}

# ---------------------------------------------------------------------------
# Decision rules (LIVE_MEASUREMENT_PLAN.md)
# ---------------------------------------------------------------------------
function Judge-Cockpit($bench, [string[]]$verify) {
    if (-not $bench) { return 'NOT RUN' }
    $rt = $bench.Fields['render_thread_cpu_ms/frame']; $busy = $bench.Fields['render_thread_busy_%']
    $gpu = $bench.Fields['gpu_util_%']; $p95 = $bench.Fields['p95_frame_ms']
    $why = @()
    if ($verify | Where-Object { $_ -match 'NOT SAFE|differences found' }) { $why += 'verify found differences' }
    if (-not $rt -or -not $busy) { $why += 'no render-thread columns' }
    elseif ($busy.Off -lt 90) { $why += ('render thread busy {0:N0}% < 90% (not render-thread-bound)' -f $busy.Off) }
    if ($gpu -and $gpu.Off -ge 90) { $why += ('GPU {0:N0}% >= 90%' -f $gpu.Off) }
    if ($bench.Locked) { $why += 'frame rate locked' }
    if ($why) { return 'INVALID: ' + ($why -join '; ') }
    $delta = $rt.On - $rt.Off
    $p95Worse = $p95 -and $p95.Significant -and $p95.ChangePct -gt 0
    if ($rt.Significant -and $delta -le -0.3 -and -not $p95Worse) { return ('GO: render thread {0:+0.000;-0.000} ms/frame (significant), p95 not worse' -f $delta) }
    return ('NO-GO: render thread {0:+0.000;-0.000} ms/frame ({1}){2}' -f $delta, $(if ($rt.Significant) { 'significant' } else { 'within noise' }), $(if ($p95Worse) { ', p95 worse' } else { '' }))
}

function Judge-TableBits($bench) {
    if (-not $bench) { return 'NOT RUN' }
    $p95 = $bench.Fields['p95_frame_ms']; $rt = $bench.Fields['render_thread_cpu_ms/frame']
    if (-not $p95) { return 'INVALID: no p95 row' }
    $rtWorse = $rt -and $rt.Significant -and $rt.ChangePct -gt 0
    if ($p95.Significant -and $p95.ChangePct -gt 1) { return ('GO BACK TO 16: p95 {0:+0.00;-0.00}% (significant)' -f $p95.ChangePct) }
    if ($rtWorse) { return ('KEEP 20?: p95 fine ({0:+0.00;-0.00}%), but render thread CPU {1:+0.00;-0.00}% (significant): look at the blocks' -f $p95.ChangePct, $rt.ChangePct) }
    return ('KEEP 20: p95 {0:+0.00;-0.00}% ({1})' -f $p95.ChangePct, $(if ($p95.Significant) { 'significant' } else { 'within noise' }))
}

function Judge-FxApply($fx) {
    if (-not $fx) { return 'NOT RUN' }
    if ($fx.Estimate -ge 0.3) { return ('GO: same-pass fast path about {0:N3} ms/frame' -f $fx.Estimate) }
    if ($fx.Estimate -lt 0.15) { return ('NO-GO: {0:N3} ms/frame' -f $fx.Estimate) }
    return ('GRAY ZONE: {0:N3} ms/frame (build only together with another render-thread item)' -f $fx.Estimate)
}

# The view chains: the chains run by one pass object with the largest object count; pairs 1st>=3rd, 2nd>=4th.
function Judge-Census($c) {
    if (-not $c) { return 'NOT RUN' }
    if ($c.Frames -lt 100) { return ('INVALID: {0} frames compared (< 100)' -f $c.Frames) }
    $groups = $c.ChainRows | Group-Object Pass, Objects | Where-Object Count -ge 4 | Sort-Object { ($_.Group | Select-Object -First 1).Objects } -Descending
    if (-not $groups) { return 'INVALID: no pass ran 4 chains (view chains not found)' }
    $v = @($groups[0].Group | Sort-Object Index | Select-Object -First 4)
    $pairs = @(@($v[0].Index, $v[2].Index), @($v[1].Index, $v[3].Index))
    $res = @()
    foreach ($p in $pairs) {
        $row = $c.Pairs | Where-Object { $_.A -eq $p[0] -and $_.B -eq $p[1] } | Select-Object -First 1
        if (-not $row) { return ('NO-GO: pair #{0} >= #{1} not listed (contained in under half of the frames)' -f $p[0], $p[1]) }
        $res += $row
    }
    $bad = $res | Where-Object { $_.Contained -ne $_.Frames }
    $focusMs = ($c.TerrainCalls | Where-Object { $_.Call -in 3, 4 } | Measure-Object Ms -Sum).Sum
    $note = if ($c.TerrainCalls) { ' focus chains (TerrainModule #3+#4) {0:N3} ms/frame' -f $focusMs } else { ' focus chain GPU ms not found' }
    if ($bad) { return ('NO-GO: ' + (($bad | ForEach-Object { '#{0}>=#{1} {2}/{3}, up to {4} objects over' -f $_.A, $_.B, $_.Contained, $_.Frames, $_.MaxOver }) -join '; ') + ';' + $note) }
    if ($c.TerrainCalls -and $focusMs -lt 0.25) { return ('NO-GO (small): counts contained in every frame, but' + $note + ' < 0.25') }
    return ('GO (counts): #{0}>=#{1} and #{2}>=#{3} contained in all {4} frames;{5}' -f $pairs[0][0], $pairs[0][1], $pairs[1][0], $pairs[1][1], $c.Frames, $note)
}

function Judge-Bundle($b) {
    if (-not $b) { return 'NOT RUN' }
    if (-not $b.Built) { return ('A/A NOISE FLOOR: ON-OFF {0:+0.000;-0.000} ms, 95% interval +/- {1:N3} ms over {2} pairs (later GO: <= -0.3 ms and outside 2x this)' -f $b.MeanDiffMs, $b.Ci95Ms, $b.Pairs) }
    if ($b.MeanDiffMs -le -0.3 -and [math]::Abs($b.MeanDiffMs) -gt $b.Ci95Ms) { return ('GO: {0:+0.000;-0.000} ms +/- {1:N3}' -f $b.MeanDiffMs, $b.Ci95Ms) }
    return ('NO-GO: {0:+0.000;-0.000} ms +/- {1:N3}' -f $b.MeanDiffMs, $b.Ci95Ms)
}

# ---------------------------------------------------------------------------
# Self test
# ---------------------------------------------------------------------------
if ($SelfTest) {
    $fail = 0
    function T([bool]$ok, [string]$what) { "{0} {1}" -f $(if ($ok) { 'PASS' } else { 'FAIL' }), $what; if (-not $ok) { $script:fail++ } }
    $r = @(
        'suite label: A5 cockpit recorder',
        '  pass timeline: 2700 frames (2702 quad-view frames, 60.0 s, 900000 events); render-thread pass execution mean 18.250 ms, p50 18.100, p95 21.300, max 30.000 ms/frame',
        '  pass timeline: render thread CPU 20.100 ms/frame (busy 96.5% of wall time)',
        '    pass timeline GBufferPass                          4.930    4.900    5.600     4.0',
        '  pass timeline: culling timeline over the same window:',
        '    aggregated (render thread)       3.10 ms/frame (14.0% of frame)  calls/frame 1.00  thread changes 0%',
        '==== BENCH RESULT: g-buffer recorder cockpit part (R24) (16 blocks, file bench_x.csv) ====',
        '  p95_frame_ms                 OFF    40.000  ON    39.800  change  -0.50% +/- 0.80% (within noise)',
        '  gpu_util_%                   OFF    70.000  ON    69.000  change  -1.40% +/- 2.00% (within noise)',
        '  render_thread_cpu_ms/frame   OFF    20.000  ON    19.500  change  -2.50% +/- 0.70% (significant)',
        '  render_thread_busy_%         OFF    97.000  ON    96.000  change  -1.00% +/- 0.50% (significant)',
        '==== end of bench, optimisation restored to ON ====',
        '  fx apply: same pass as the previous Apply 2400/frame (41.0%); of those, identical ...',
        '  fx apply estimate: same-pass fast path saves about 0.412 ms/frame (upper bound 0.900 ms/frame); R14 gate 0.4 ms: PASS',
        '  chain census: 150 frames compared (9 calccounters chains per frame; 0 frames with another chain count, 0 over 32 chains); sampled 160, read back 152',
        '  chain census #5 TerrainModule call #0: objects 19527, instances 168000/frame, non-empty objects 900/frame',
        '  chain census #6 TerrainModule call #1: objects 19527, instances 167000/frame, non-empty objects 900/frame',
        '  chain census #7 TerrainModule call #2: objects 19527, instances 30000/frame, non-empty objects 400/frame',
        '  chain census #8 TerrainModule call #3: objects 19527, instances 29000/frame, non-empty objects 400/frame',
        '  chain census pair #5 >= #7: counts contained in 150/150 frames (100.0%), objects over max 0, excess instances 0.0/frame (same pass)',
        '  chain census pair #6 >= #8: counts contained in 150/150 frames (100.0%), objects over max 0, excess instances 0.0/frame (same pass)',
        '    TerrainModule #0 0.300 #1 0.300 #2 0.150 #3 0.160 #4 0.150',
        '  gpu bundle: no requested item is built: this run is an A/A',
        '  gpu bundle block 1/4: OFF (items 0x0)', '  gpu passes: 15.20 ms/frame exclusive GPU time in passes, 40 kinds',
        '  gpu bundle block 2/4: ON (items 0x0)', '  gpu passes: 15.30 ms/frame exclusive GPU time in passes, 40 kinds',
        '  gpu bundle block 3/4: ON (items 0x0)', '  gpu passes: 15.10 ms/frame exclusive GPU time in passes, 40 kinds',
        '  gpu bundle block 4/4: OFF (items 0x0)', '  gpu passes: 15.20 ms/frame exclusive GPU time in passes, 40 kinds')
    T ((Get-Label $r) -eq 'A5 cockpit recorder') 'label'
    $tl = Get-Timeline $r
    T ($tl.Frames -eq 2700 -and $tl.P95 -eq 21.3 -and $tl.RtBusy -eq 96.5 -and $tl.Kinds[0].Kind -eq 'GBufferPass' -and $tl.Culling.Count -eq 1) 'timeline'
    $old = Get-Timeline @('  pass timeline: 1 frames (3712 quad-view frames, 66.0 s, 519694 events); render-thread pass execution mean 50318.275 ms, p50 50318.275, p95 50318.275, max 50318.275 ms/frame', '  pass timeline: render thread CPU 16.716 ms/frame (busy 94.0% of wall time)', '    pass timeline GBufferPassData                    20893.810 20893.810 20893.810  29640.0')
    T ($old.NoBoundary -and $null -eq $old.Mean -and $old.Kinds.Count -eq 0 -and $old.RtCpu -eq 16.716) 'timeline without frame boundary (pre-fix live report): per-frame figures n/a, render thread CPU kept'
    T (-not $tl.NoBoundary -and $tl.QuadFrames -eq 2702) 'timeline quad-view frames'
    $b = Get-Benches $r
    T ($b.Count -eq 1 -and $b[0].Fields['render_thread_cpu_ms/frame'].Significant) 'bench rows'
    T ((Judge-Cockpit $b[0] @()) -like 'GO:*') 'cockpit GO'
    T ((Judge-FxApply (Get-FxApply $r)) -like 'GO:*') 'fx apply GO'
    $c = Get-Census $r
    T ($c.Frames -eq 150 -and $c.ChainRows.Count -eq 4 -and $c.Pairs.Count -eq 2 -and $c.TerrainCalls.Count -eq 5) 'census parse'
    T ((Judge-Census $c) -like 'GO (counts)*') 'census GO'
    $bu = Get-Bundle $r
    T ($bu.Pairs -eq 2 -and -not $bu.Built -and [math]::Abs($bu.MeanDiffMs - 0.0) -lt 1e-9) 'bundle A/A pairs'
    "$fail failure(s)"
    exit $(if ($fail) { 1 } else { 0 })
}

# ---------------------------------------------------------------------------
# Session
# ---------------------------------------------------------------------------
if (-not $LogDir -or -not (Test-Path -LiteralPath $LogDir)) { throw 'Give -LogDir <folder with report_*.txt, bench_*.csv, passtl_*.csv>.' }
$reports = @(Get-ChildItem -LiteralPath $LogDir -Filter 'report_*.txt' | Sort-Object Name)
$sources = @()
foreach ($f in $reports) { $sources += [pscustomobject]@{ Name = $f.Name; Lines = [IO.File]::ReadAllLines($f.FullName) } }
if (-not $sources) {
    $log = Join-Path $LogDir 'DcsQvCull.log'
    if (-not (Test-Path -LiteralPath $log)) { throw "No report_*.txt and no DcsQvCull.log in $LogDir." }
    # Split the log at every suite header.
    $all = [IO.File]::ReadAllLines($log); $cur = $null
    foreach ($l in $all) {
        if ($l -match '==== DcsQvCull test suite') { $cur = [pscustomobject]@{ Name = 'DcsQvCull.log suite ' + ($sources.Count + 1); Lines = [Collections.Generic.List[string]]::new() }; $sources += $cur }
        if ($cur) { $cur.Lines.Add($l) }
    }
}

$md = [Text.StringBuilder]::new()
function Out([string]$s = '') { [void]$md.AppendLine($s); $s }

Out '# Live measurement summary (perf audit questions a-f)'
Out ''
Out ('Folder: {0}; {1} suite runs.' -f (Resolve-Path -LiteralPath $LogDir), $sources.Count)
Out ''
Out '| Run | Label |'
Out '|---|---|'
foreach ($s in $sources) { Out ('| {0} | {1} |' -f $s.Name, (Get-Label $s.Lines)) }

Out ''
Out '## (a) Render thread: pass timeline'
Out ''
Out '| Step | Frames | Pass ms mean | p50 | p95 | Render thread CPU ms/frame | Busy % | Culling (render thread) ms |'
Out '|---|---|---|---|---|---|---|---|'
$timelines = @()
foreach ($s in $sources) {
    $t = Get-Timeline $s.Lines
    if (-not $t) { continue }
    $timelines += [pscustomobject]@{ Label = (Get-Label $s.Lines); T = $t }
    $agg = ($t.Culling | Where-Object Phase -like 'aggregated*' | Select-Object -First 1).Ms
    $na = 'n/a (no frame boundary)'
    Out ('| {0} | {1} | {2} | {3} | {4} | {5} | {6} | {7} |' -f (Get-Label $s.Lines), $t.Frames,
        $(if ($t.NoBoundary) { $na } else { '{0:N3}' -f $t.Mean }), $(if ($t.NoBoundary) { $na } else { '{0:N3}' -f $t.P50 }), $(if ($t.NoBoundary) { $na } else { '{0:N3}' -f $t.P95 }),
        $(if ($null -ne $t.RtCpu) { '{0:N3}' -f $t.RtCpu } else { 'n/a' }), $(if ($null -ne $t.RtBusy) { '{0:N1}' -f $t.RtBusy } else { 'n/a' }),
        $(if ($null -ne $agg) { '{0:N2}' -f $agg } else { 'n/a' }))
}
foreach ($x in $timelines) {
    Out ''
    Out ('Top passes, {0} (exclusive render-thread ms per frame):' -f $x.Label)
    Out ''
    Out '| Pass kind | mean | p50 | p95 | calls/frame |'
    Out '|---|---|---|---|---|'
    foreach ($k in ($x.T.Kinds | Select-Object -First 12)) { Out ('| {0} | {1:N3} | {2:N3} | {3:N3} | {4:N1} |' -f $k.Kind, $k.Mean, $k.P50, $k.P95, $k.Calls) }
}
$paused = $timelines | Where-Object Label -match 'paused' | Where-Object Label -notmatch 'unpaused' | Select-Object -First 1
$unpaused = $timelines | Where-Object Label -match 'unpaused' | Select-Object -First 1
if ($paused -and $unpaused -and $null -ne $unpaused.T.RtBusy) {
    Out ''
    Out ('Main-thread check: render thread busy {0:N1}% paused, {1:N1}% unpaused -> {2}' -f $paused.T.RtBusy, $unpaused.T.RtBusy,
        $(if ($unpaused.T.RtBusy -lt 85) { 'unpaused is limited elsewhere (check GPU % and the cap before calling it Main-bound)' } else { 'still render-thread bound' }))
}
if ($unpaused -and $unpaused.T.Kinds.Count) {
    $terr = ($unpaused.T.Kinds | Where-Object Kind -match 'Terrain' | Measure-Object Mean -Sum).Sum
    Out ('B5 terrain: terrain pass kinds {0:N3} ms/frame unpaused -> {1}' -f $terr, $(if ($terr -ge 1.5) { 'reopen the terrain study' } else { 'stays closed' }))
}

Out ''
Out '## (b) Cockpit G-buffer recorder, render-thread ms'
Out ''
$cockpit = $null; $cockpitVerify = @()
foreach ($s in $sources) {
    $bb = Get-Benches $s.Lines | Where-Object What -match 'cockpit part'
    if ($bb) { $cockpit = $bb | Select-Object -First 1; $cockpitVerify = Get-Verify $s.Lines }
}
if ($cockpit) {
    Out '| Field | OFF | ON | change % | +/- % | |'
    Out '|---|---|---|---|---|---|'
    foreach ($k in 'render_thread_cpu_ms/frame', 'render_thread_busy_%', 'gbuffer_pass_ms/frame', 'pass_ms/frame', 'frame_ms', 'p95_frame_ms', 'gpu_util_%') {
        $f = $cockpit.Fields[$k]
        if ($f) { Out ('| {0} | {1:N3} | {2:N3} | {3:+0.00;-0.00} | {4:N2} | {5} |' -f $k, $f.Off, $f.On, $f.ChangePct, $f.CiPct, $(if ($f.Significant) { 'significant' } else { '' })) }
    }
    foreach ($v in $cockpitVerify) { Out ('Verify: ' + $v) }
}
Out ('Verdict (b): ' + (Judge-Cockpit $cockpit $cockpitVerify))

Out ''
Out '## (c) Effect Applies (same pass again)'
Out ''
foreach ($s in $sources) {
    $fx = Get-FxApply $s.Lines
    if ($fx) { Out ('{0}: same-pass Applies {1:N0}/frame ({2:N1}%), estimate {3:N3} ms/frame (upper {4:N3}) -> {5}' -f (Get-Label $s.Lines), $fx.SamePerFrame, $fx.SamePct, $fx.Estimate, $fx.Upper, (Judge-FxApply $fx)) }
}
$dense = $sources | Where-Object { (Get-Label $_.Lines) -match 'germany|dense|B1' } | ForEach-Object { Get-FxApply $_.Lines } | Where-Object { $_ } | Select-Object -First 1
Out ('Verdict (c), dense map: ' + (Judge-FxApply $dense))

Out ''
Out '## (d) Focus chain from the periphery chain (counts census)'
Out ''
foreach ($s in $sources) {
    $c = Get-Census $s.Lines
    if (-not $c) { continue }
    Out ('{0}: {1} frames, {2} chains per frame' -f (Get-Label $s.Lines), $c.Frames, $c.Chains)
    Out ''
    Out '| Chain | Pass | Call | Objects | Instances/frame |'
    Out '|---|---|---|---|---|'
    foreach ($r in $c.ChainRows) { Out ('| #{0} | {1} | {2} | {3} | {4:N0} |' -f $r.Index, $r.Pass, $r.Call, $r.Objects, $r.Instances) }
    Out ''
    Out ('Verdict (d), {0}: {1}' -f (Get-Label $s.Lines), (Judge-Census $c))
    Out ''
}

Out '## (e) Small exact GPU items as one bundle'
Out ''
foreach ($s in $sources) {
    $bu = Get-Bundle $s.Lines
    if ($bu) { Out ('{0}: {1} blocks -> {2}' -f (Get-Label $s.Lines), $bu.Blocks, (Judge-Bundle $bu)) }
}

Out ''
Out '## (f) Prefetch TableBits=20, p95 in a CPU-bound scene'
Out ''
$tb = $null
foreach ($s in $sources) { $x = Get-Benches $s.Lines | Where-Object What -match 'repeat table'; if ($x) { $tb = $x | Select-Object -First 1 } }
if ($tb) {
    foreach ($k in 'p95_frame_ms', 'frame_ms', 'render_thread_cpu_ms/frame') {
        $f = $tb.Fields[$k]
        if ($f) { Out ('{0}: OFF (2^16) {1:N3}, ON {2:N3}, change {3:+0.00;-0.00}% +/- {4:N2}%' -f $k, $f.Off, $f.On, $f.ChangePct, $f.CiPct) }
    }
}
Out ('Verdict (f): ' + (Judge-TableBits $tb))

$csv = @(Get-ChildItem -LiteralPath $LogDir -Filter 'passtl_*.csv' -ErrorAction SilentlyContinue)
if ($csv) { Out ''; Out ('Per-frame pass tables: ' + (($csv | ForEach-Object Name) -join ', ')) }

if ($OutFile) { [IO.File]::WriteAllText($OutFile, $md.ToString()); "Summary written to $OutFile" }
