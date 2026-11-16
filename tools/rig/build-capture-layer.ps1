# Builds the TEST-ONLY rig capture layer, its headset-free harness and the chain shim, and writes the layer's manifest
# for XR_API_LAYER_PATH (absolute library path). Nothing built here is shipped or installed.
#
#   pwsh -File tools\rig\build-capture-layer.ps1 [-Test] [-NoWait]
#
#   -Test    runs the offline tests afterwards: the harness on WARP with a byte check, and OFXR's own call-chain test
#            with the capture layer below OFXR (needs a built OFXR fork, see -OfxrBuild; skipped with a line if absent)
#   -NoWait  does not wait for the quiet lock (the caller holds it); the lock is <repo>\artifacts\.lane\quiet.lock, or the
#            file DCSC_QUIET_LOCK names (one lock for every checkout of a machine)
#
# Nothing of this machine is written in the script. Where things are comes from a parameter, else from an environment
# variable, else from the default given here:
#   -OutDir        DCSC_RIG_TAP_DIR      where the binaries and the manifest go; default <repo>\artifacts\rig\capture-layer\bin
#                                        (git-ignored; the default -TapDir of run-scenario.ps1)
#   -OpenXrInclude DCSC_OPENXR_INCLUDE   the folder that holds openxr\openxr.h; default the OFXR fork's copy,
#                                        <repo>\external\ofxr-djules75\external\OpenXR-SDK\include
#   -OfxrBuild     DCSC_OFXR_BUILD       a built OFXR fork (xrfg_layer_call_chain.exe and the layer DLL), for -Test;
#                                        default <repo>\artifacts\native\ofxr-djules75
#   -TestOut       DCSC_RIG_TEST_OUT     where -Test writes; default <repo>\artifacts\rig\capture-layer\test-out
# The compiler is found by scripts\msvc.cmd, as for every other native build of this repository.
param([switch]$Test, [switch]$NoWait,
  [string]$OutDir = $env:DCSC_RIG_TAP_DIR, [string]$OpenXrInclude = $env:DCSC_OPENXR_INCLUDE,
  [string]$OfxrBuild = $env:DCSC_OFXR_BUILD, [string]$TestOut = $env:DCSC_RIG_TEST_OUT)
$ErrorActionPreference = 'Stop'
$Rig  = [IO.Path]::GetFullPath($PSScriptRoot)
$Repo = [IO.Path]::GetFullPath((Join-Path $Rig '..\..'))
$Src  = Join-Path $Rig 'capture-layer'
$Bin  = if ($OutDir) { [IO.Path]::GetFullPath($OutDir) } else { Join-Path $Repo 'artifacts\rig\capture-layer\bin' }
$Lock = if ($env:DCSC_QUIET_LOCK) { [IO.Path]::GetFullPath($env:DCSC_QUIET_LOCK) } else { Join-Path $Repo 'artifacts\.lane\quiet.lock' }
$Inc  = if ($OpenXrInclude) { [IO.Path]::GetFullPath($OpenXrInclude) } else { Join-Path $Repo 'external\ofxr-djules75\external\OpenXR-SDK\include' }
$Msvc = Join-Path $Repo 'scripts\msvc.cmd'
function Wait-Quiet { if ($NoWait) { return }; $told = $false; while (Test-Path -LiteralPath $Lock) { if (-not $told) { Write-Host "quiet.lock held ($((Get-Content -LiteralPath $Lock -ErrorAction SilentlyContinue | Select-Object -First 1))): waiting"; $told = $true }; Start-Sleep 20 } }
if (-not (Test-Path -LiteralPath (Join-Path $Inc 'openxr\openxr.h'))) { throw "OpenXR headers not found in ${Inc}: the OFXR fork's checkout has them (scripts\build-ofxr-djules75.ps1 clones it), or give -OpenXrInclude." }
New-Item -ItemType Directory -Force $Bin | Out-Null
Wait-Quiet
# Two small command files in the output folder (they hold this machine's paths and are never tracked): cmd takes one
# quoted path, the quoted paths of the compiler lines are inside the file.
$inner = Join-Path $Bin '_build.cmd'
$cl = 'cl /nologo /O2 /EHsc /W3 /MT /D_CRT_SECURE_NO_WARNINGS /I"' + $Inc + '"'
Set-Content -LiteralPath $inner -Encoding ascii -Value @(
  '@echo off', ('cd /d "' + $Bin + '"'),
  ($cl + ' /LD "' + (Join-Path $Src 'rigtap.cpp') + '" /Fe:XR_APILAYER_RIG_capture.dll d3d12.lib d3dcompiler.lib dxguid.lib psapi.lib || exit /b 1'),
  ($cl + ' /LD "' + (Join-Path $Src 'rig_chain_shim.cpp') + '" /Fe:rig_chain_shim.dll || exit /b 1'),
  ($cl + ' "' + (Join-Path $Src 'rigtap_harness.cpp') + '" /Fe:rigtap_harness.exe d3d12.lib dxgi.lib dxguid.lib || exit /b 1'),
  'echo built')
$outer = Join-Path $Bin '_run.cmd'
Set-Content -LiteralPath $outer -Encoding ascii -Value ('@call "' + $Msvc + '" "' + $inner + '"')
$o = & $env:ComSpec /d /c $outer 2>&1 | Out-String
Write-Host $o
if ($LASTEXITCODE -ne 0) { throw "build failed ($LASTEXITCODE)" }
$dll = Join-Path $Bin 'XR_APILAYER_RIG_capture.dll'
$manifest = [ordered]@{ file_format_version = '1.0.0'; api_layer = [ordered]@{ name = 'XR_APILAYER_RIG_capture'; library_path = $dll; api_version = '1.0'; implementation_version = '1'; description = 'TEST-ONLY rig capture layer (pass-through; records what reaches the runtime in xrEndFrame when armed)' } }
[IO.File]::WriteAllText((Join-Path $Bin 'XR_APILAYER_RIG_capture.json'), ($manifest | ConvertTo-Json -Depth 4), (New-Object Text.UTF8Encoding($false)))
Write-Host "layer: $dll  sha256 $((Get-FileHash -LiteralPath $dll).Hash.Substring(0,16))"
Write-Host "manifest folder for XR_API_LAYER_PATH: $Bin   name for XR_ENABLE_API_LAYERS (LAST): XR_APILAYER_RIG_capture"
if (-not $Test) { return }

$TestRoot = if ($TestOut) { [IO.Path]::GetFullPath($TestOut) } else { Join-Path $Repo 'artifacts\rig\capture-layer\test-out' }
# ---- offline test 1: the harness (fake loader + fake runtime + OFXR's part), WARP, byte comparison ----------------
Wait-Quiet
$t1 = Join-Path $TestRoot 'harness'
if (Test-Path -LiteralPath $t1) { Remove-Item -LiteralPath $t1 -Recurse -Force -Confirm:$false }
New-Item -ItemType Directory -Force $t1 | Out-Null
& (Join-Path $Bin 'rigtap_harness.exe') $dll $t1; $h = $LASTEXITCODE
& py (Join-Path $Src 'check_harness.py') $t1; $c = $LASTEXITCODE
Write-Host "harness exit $h, byte check exit $c"

# ---- offline test 2: OFXR's own call-chain test (its fake runtime, WARP) with the capture layer BELOW OFXR --------
# The test program, the layer and the test's ini are read from a built OFXR fork; the layer is copied into the test's
# folder, as OFXR's own ctest does, so the built component is never written to.
$ofxrDir = if ($OfxrBuild) { [IO.Path]::GetFullPath($OfxrBuild) } else { Join-Path $Repo 'artifacts\native\ofxr-djules75' }
$chain = Join-Path $ofxrDir 'xrfg_layer_call_chain.exe'; $ofxr = Join-Path $ofxrDir 'XR_APILAYER_XRFrameBridge_diagnostic.dll'
$baseIni = Join-Path $Repo 'external\ofxr-djules75\tests\ofxr_bridge_shallow_pipeline.ini'
$c2 = @{}
if ((Test-Path -LiteralPath $chain) -and (Test-Path -LiteralPath $ofxr) -and (Test-Path -LiteralPath $baseIni)) {
  foreach ($arm in 'idle', 'armed') {
    Wait-Quiet
    $w = Join-Path $TestRoot "chain-$arm"
    if (Test-Path -LiteralPath $w) { Remove-Item -LiteralPath $w -Recurse -Force -Confirm:$false }
    New-Item -ItemType Directory -Force $w | Out-Null
    Copy-Item -LiteralPath $ofxr -Destination $w
    $ini = [IO.File]::ReadAllText($baseIni).Replace('[ofxr]', "[ofxr]`nfoveated_layers=1`n").Replace('logging_enabled=0', 'logging_enabled=1')
    [IO.File]::WriteAllText((Join-Path $w 'ofxr_bridge.ini'), $ini)
    $env:RIGSHIM_UPPER = Join-Path $w 'XR_APILAYER_XRFrameBridge_diagnostic.dll'; $env:RIGSHIM_LOWER = $dll
    $env:RIGTAP_DIR = Join-Path $w 'tap'; $env:RIGTAP_FRAMES = $(if ($arm -eq 'armed') { '40' } else { $null }); $env:RIGTAP_AFTER_S = $(if ($arm -eq 'armed') { '0.25' } else { $null })
    $out = & $chain (Join-Path $Bin 'rig_chain_shim.dll') (Join-Path $w 'fake-runtime.log') 'two:dcs' 2>&1 | Out-String; $c2[$arm] = $LASTEXITCODE
    Set-Content -LiteralPath (Join-Path $w 'call-chain-output.txt') -Value $out
    Write-Host "call chain two:dcs with the capture layer below OFXR ($arm): exit $($c2[$arm])"; Write-Host ($out.Trim())
    Remove-Item Env:RIGSHIM_UPPER, Env:RIGSHIM_LOWER, Env:RIGTAP_DIR -ErrorAction SilentlyContinue; Remove-Item Env:RIGTAP_FRAMES, Env:RIGTAP_AFTER_S -ErrorAction SilentlyContinue
    if (Test-Path -LiteralPath (Join-Path $w 'tap\rigtap.log')) { Get-Content -LiteralPath (Join-Path $w 'tap\rigtap.log') | Select-Object -First 30 | Write-Host }
    $capDir = Join-Path $w 'tap\cap00_env'; $fl = Get-ChildItem -LiteralPath $w -Filter 'ofxr-bridge-flight-*.log' | Select-Object -First 1
    if ($arm -eq 'armed') { if ((Test-Path -LiteralPath (Join-Path $capDir 'done.txt')) -and $fl) { & py (Join-Path $Rig 'verdict.py') $capDir --flight-log $fl.FullName --quiet; & py (Join-Path $Src 'check_chain.py') $capDir; $c2['check'] = $LASTEXITCODE } else { Write-Host 'armed run: no complete capture or no flight log'; $c2['check'] = 1 } }
  }
} else { Write-Host "call-chain test skipped: $chain or $ofxr or $baseIni missing (build the OFXR fork, or give -OfxrBuild)" }
if ($h -ne 0 -or $c -ne 0) { throw 'offline test 1 failed' }
if ($c2.Count -and ($c2['idle'] -ne 0 -or $c2['armed'] -ne 0 -or $c2['check'] -ne 0)) { throw 'offline test 2 failed' }
