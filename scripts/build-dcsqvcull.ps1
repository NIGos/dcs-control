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
# The tests' inputs from this repository, unless the caller names others: the far-depth test loads this Quad Views
# build, and the shared-memory layout test compares the module's copy of the header with the pupil shift layer's.
# Set for this script only, so a later build in the same session (another checkout, the pupil shift tests' cache) never
# inherits them.
$testEnvironment = [ordered]@{
    DCSVR_QV_DLL = Join-Path $workspaceRoot 'external/quadviews/bin/x64/Release/XR_APILAYER_MBUCCHIA_quad_views_foveated.dll'
    QV_PERCEPTION_SHM_H = Join-Path $workspaceRoot 'native/pupil_shift/perception_shm.h'
    DCSQV_MODULE_SHM_H = Join-Path $workspaceRoot 'native/dcsqvcull/src/dcsvr_perception_shm.h'
}
$callerEnvironment = @{}
foreach ($name in @($testEnvironment.Keys)) {
    $callerEnvironment[$name] = [Environment]::GetEnvironmentVariable($name)
    if (-not $callerEnvironment[$name] -and ($name -ne 'DCSVR_QV_DLL' -or (Test-Path -LiteralPath $testEnvironment[$name]))) { Set-Item "env:$name" $testEnvironment[$name] }
}
try {
    $testSources = @((Join-Path $workspaceRoot 'native/dcsqvcull/test'), (Join-Path $workspaceRoot 'native/dcsqvcull/CMakeLists.txt'))
    foreach ($name in @($testEnvironment.Keys)) { if ([Environment]::GetEnvironmentVariable($name)) { $testSources += [Environment]::GetEnvironmentVariable($name) } }
    if (-not (Test-NativeTestsCurrent $build $testSources)) {
        $testRun = Join-Path $build 'test-run'
        if (Test-Path -LiteralPath $testRun) { Get-ChildItem -LiteralPath $testRun -Force | Remove-Item -Recurse -Force }
        & $wrapper ctest --test-dir $build --output-on-failure
        if ($LASTEXITCODE -ne 0) { throw 'DcsQvCull tests failed.' }
        Save-NativeTestsPassed $build $testSources
    }
} finally {
    foreach ($name in @($testEnvironment.Keys)) { [Environment]::SetEnvironmentVariable($name, $callerEnvironment[$name]) }
}
