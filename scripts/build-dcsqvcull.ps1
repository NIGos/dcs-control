#Requires -Version 7
# Builds the DCS engine optimizations (native/dcsqvcull: loader and payload DLLs) and runs their offline tests.
$ErrorActionPreference = 'Stop'
$workspaceRoot = Split-Path -Parent $PSScriptRoot
$wrapper = Join-Path $PSScriptRoot 'msvc.cmd'
$build = Join-Path $workspaceRoot 'artifacts/native/dcsqvcull'
& $wrapper cmake -S (Join-Path $workspaceRoot 'native/dcsqvcull') -B $build -G Ninja -DCMAKE_BUILD_TYPE=Release
if ($LASTEXITCODE -ne 0) { throw 'DcsQvCull configuration failed.' }
& $wrapper cmake --build $build --parallel 4
if ($LASTEXITCODE -ne 0) { throw 'DcsQvCull build failed.' }
# The tests write reports and bench files into their working folder and read back the newest: they start from an
# empty one, so a report left by an earlier run is never read instead of this run's.
. (Join-Path $PSScriptRoot 'test-cache.ps1')
# The far-depth test loads this repository's Quad Views build, unless DCSVR_QV_DLL names another one.
$quadViewsDll = Join-Path $workspaceRoot 'external/quadviews/bin/x64/Release/XR_APILAYER_MBUCCHIA_quad_views_foveated.dll'
if (-not $env:DCSVR_QV_DLL -and (Test-Path -LiteralPath $quadViewsDll)) { $env:DCSVR_QV_DLL = $quadViewsDll }
$testSources = @((Join-Path $workspaceRoot 'native/dcsqvcull/test'), (Join-Path $workspaceRoot 'native/dcsqvcull/CMakeLists.txt'))
if ($env:DCSVR_QV_DLL) { $testSources += $env:DCSVR_QV_DLL }
# The shared-memory layout test compares the module's copy with the pupil shift layer's own header.
if (-not $env:QV_PERCEPTION_SHM_H) { $env:QV_PERCEPTION_SHM_H = Join-Path $workspaceRoot 'native/pupil_shift/perception_shm.h' }
$testSources += $env:QV_PERCEPTION_SHM_H
if (-not $env:DCSQV_MODULE_SHM_H) { $env:DCSQV_MODULE_SHM_H = Join-Path $workspaceRoot 'native/dcsqvcull/src/dcsvr_perception_shm.h' }
if (-not (Test-NativeTestsCurrent $build $testSources)) {
    $testRun = Join-Path $build 'test-run'
    if (Test-Path -LiteralPath $testRun) { Get-ChildItem -LiteralPath $testRun -Force | Remove-Item -Recurse -Force }
    & $wrapper ctest --test-dir $build --output-on-failure
    if ($LASTEXITCODE -ne 0) { throw 'DcsQvCull tests failed.' }
    Save-NativeTestsPassed $build $testSources
}
