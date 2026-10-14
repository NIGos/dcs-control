#Requires -Version 7
# Builds the pupil shift OpenXR layer (native/pupil_shift) and runs its tests: the geometry and Depth boost unit tests
# (pupil_shift_tests) and the end-to-end harness (pupil_shift_harness: the built DLL under a fake runtime, synthetic
# gaze traces; CSV and SVG traces in artifacts/native/pupil-shift/harness_out).
# -OpenXrInclude: the OpenXR headers when external/cheeky is not checked out next to this script (a separate worktree).
param([string]$OpenXrInclude)
$ErrorActionPreference = 'Stop'
$workspaceRoot = Split-Path -Parent $PSScriptRoot
$wrapper = Join-Path $PSScriptRoot 'msvc.cmd'
$build = Join-Path $workspaceRoot 'artifacts/native/pupil-shift'
$configure = @('-S', (Join-Path $workspaceRoot 'native/pupil_shift'), '-B', $build, '-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Release')
if ($OpenXrInclude) { $configure += "-DOPENXR_INCLUDE=$($OpenXrInclude -replace '\\', '/')" }
& $wrapper cmake @configure
if ($LASTEXITCODE -ne 0) { throw 'Pupil shift configuration failed.' }
& $wrapper cmake --build $build --parallel 4
if ($LASTEXITCODE -ne 0) { throw 'Pupil shift build failed.' }
. (Join-Path $PSScriptRoot 'test-cache.ps1')
$testSources = @((Join-Path $workspaceRoot 'native/pupil_shift'))  # its CMakeLists.txt included
if (-not (Test-NativeTestsCurrent $build $testSources)) {
    & $wrapper ctest --test-dir $build --output-on-failure
    if ($LASTEXITCODE -ne 0) { throw 'Pupil shift tests failed.' }
    Save-NativeTestsPassed $build $testSources
}
