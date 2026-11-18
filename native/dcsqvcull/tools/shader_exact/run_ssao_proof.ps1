#Requires -Version 7
# The offline proof of the 0.7 SSAO water skip (early_shaders.h): the module's own builds of DCS's deferred\SSAO.fx
# (written by the native test: QV_EARLY_ONLY=1 QV_EARLY_OUT=<Blobs> test_main.exe) against DCS's archive blobs, bit for
# bit, on WARP and the GPU, plus negative controls (variants broken on purpose must be reported as different).
#   run_ssao_proof.ps1 -Blobs <folder> -Archive <folder with 4f6d6b19....dxbc, 72edc447....dxbc> -Out <folder>
#                      [-Controls <folder with ctl_<name>_<64|128>.dxbc>] [-Cases 40] [-Seeds 3] [-Dev both]
# Check artifacts\.lane\quiet.lock before a GPU run (-Dev warp avoids the GPU).
param(
    [Parameter(Mandatory)][string]$Blobs,
    [Parameter(Mandatory)][string]$Archive,
    [Parameter(Mandatory)][string]$Out,
    [string]$Controls = '',
    [int]$Cases = 40,
    [int]$Seeds = 3,
    [string]$Dev = 'both'
)
$ErrorActionPreference = 'Stop'
& (Join-Path $PSScriptRoot 'build.ps1') -Out (Join-Path $Out 'build')
$exe = Join-Path $Out 'build\sx_equiv.exe'
$rules = Join-Path $PSScriptRoot 'rules\ssao.txt'
$csv = Join-Path $Out 'ssao_proof.csv'
Remove-Item $csv -ErrorAction SilentlyContinue
$stock = @{ '64' = '4f6d6b190537b2e5200a51605776f007.dxbc'; '128' = '72edc4474b4ed0f536bcfe89939c914d.dxbc' }
$fail = 0
foreach ($k in '64', '128') {
    $a = Join-Path $Archive $stock[$k]
    $runs = @(@{ id = "module_stock_$k"; blob = Join-Path $Blobs "ssao${k}_stock.dxbc"; expect = 'same'; seeds = 1 },
              @{ id = "module_variant_$k"; blob = Join-Path $Blobs "ssao${k}_variant.dxbc"; expect = 'same'; seeds = $Seeds })
    if ($Controls) {
        foreach ($c in Get-ChildItem $Controls -Filter "ctl_*_$k.dxbc") {
            $runs += @{ id = $c.BaseName; blob = $c.FullName; expect = 'differ'; seeds = 1 }
        }
    }
    foreach ($r in $runs) {
        for ($s = 1; $s -le $r.seeds; $s++) {
            & $exe pair $a $r.blob $rules -dev $Dev -cases ($r.expect -eq 'same' ? $Cases : 10) -seed $s -texsize 192x160 `
                -expect $r.expect -id $r.id -csv $csv
            if ($LASTEXITCODE -ne 0) { $fail++ }
        }
    }
}
"proof: $fail failing runs (csv: $csv)"
exit ($fail ? 1 : 0)
