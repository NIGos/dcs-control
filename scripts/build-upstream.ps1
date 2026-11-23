#Requires -Version 7
# -Force: patch-cheeky.py discards local edits in external/cheeky instead of refusing to reset them.
param([ValidateSet('ofxr-core','cheeky')][string]$Component = 'ofxr-core', [switch]$Force)
# Every key the app writes for this component must be read by it (scripts/written-keys.txt).
& (Join-Path $PSScriptRoot 'check-written-keys.ps1')
if ($LASTEXITCODE -ne 0) { throw 'The app writes a key this component never reads (check-written-keys.ps1).' }
$ErrorActionPreference = 'Stop'
$workspaceRoot = Split-Path -Parent $PSScriptRoot
. (Join-Path $PSScriptRoot 'build-jobs.ps1')  # Get-BuildJobs: the compile jobs at once
$nativeWrapper = Join-Path $PSScriptRoot 'msvc.cmd'
$nativeSource = Join-Path $workspaceRoot ('external/' + $(if ($Component -eq 'ofxr-core') {'ofxr'} else {'cheeky'}))
$nativeBuild = Join-Path $workspaceRoot ('artifacts/native/' + $Component)
$nativeOptions = @('-DCMAKE_BUILD_TYPE=Release')
if ($Component -eq 'cheeky') {
    $patchOptions = @(if ($Force) { '--force' })
    # The Python launcher, not plain "python" (on a PC without Python on PATH that is the Store stub): scripts/python.ps1.
    . (Join-Path $PSScriptRoot 'python.ps1')
    Invoke-Python (Join-Path $PSScriptRoot 'patch-cheeky.py') @patchOptions
    if ($LASTEXITCODE -ne 0) { throw 'Cheeky adaptation failed.' }
}
if ($Component -eq 'ofxr-core') { $nativeOptions += @('-DXRFG_BUILD_LAYER=OFF','-DXRFG_BUILD_STANDALONE=OFF') }
& $nativeWrapper cmake -S $nativeSource -B $nativeBuild -G Ninja @nativeOptions
if ($LASTEXITCODE -ne 0) { throw 'Native CMake configuration failed.' }
& $nativeWrapper cmake --build $nativeBuild --parallel (Get-BuildJobs)
if ($LASTEXITCODE -ne 0) { throw 'Native build failed.' }
$testOptions = @('--test-dir',$nativeBuild,'--output-on-failure','--parallel','2')
# These upstream tests show windows and exercise input capture. The user has
# explicitly requested verification without desktop interaction.
if ($Component -eq 'cheeky') {
    # Mixed WARP calibration is slow under concurrent GPU tests on this host.
    $testOptions += @('-E','([Oo]verlay|MixedCalibration)')
    $testFile = Join-Path $nativeBuild 'CTestTestfile.cmake'
    # patch-cheeky.py already raises it to 300 in CMakeLists.txt; this pins it whatever the generated value is, and
    # fails loudly when CMake's output no longer has the anchor (the old silent Replace never matched).
    # CMake quotes the test name as [=[...]=] or "..." depending on its version: both are taken.
    $timeoutAnchor = [regex]'(CheekyMixedCalibrationTests(?:\]=\]|")\s+PROPERTIES\s+TIMEOUT )"\d+"'
    $testText = [IO.File]::ReadAllText($testFile)
    if ($timeoutAnchor.Matches($testText).Count -ne 1) { throw "CTestTestfile.cmake: the MixedCalibration timeout anchor was not found exactly once ($timeoutAnchor)." }
    [IO.File]::WriteAllText($testFile, $timeoutAnchor.Replace($testText, '$1"300"'))
    # Each run's test data (captures, host-test output, per-process core discovery folders) is never read by a later
    # run and grew to gigabytes: start every test run from empty folders.
    $testBin = Join-Path $nativeBuild 'bin/Release'
    foreach ($folder in 'uevr-test-data', 'standalone-host-tests', 'runtime-host-test-data') {
        $path = Join-Path $testBin $folder
        if (Test-Path -LiteralPath $path) { Get-ChildItem -LiteralPath $path -Force | Remove-Item -Recurse -Force }
    }
    Get-ChildItem -LiteralPath $testBin -Directory -Filter 'core-discovery-*' -ErrorAction SilentlyContinue | Remove-Item -Recurse -Force
}
# The tests are skipped when nothing they read changed since they last all passed (scripts/test-cache.ps1): every
# program and library of the build (Ninja relinks nothing when no source changed; the test data folders above are empty
# by now), the checkout's tests and test assets, the test list ctest runs, this script. DCSC_FORCE_TESTS=1 runs them.
. (Join-Path $PSScriptRoot 'test-cache.ps1')
# Cheeky's tests leave copies of DLLs in per-process folders (<name>-<pid>-<ticks>) and in their data folders: not built,
# so not part of the fingerprint.
$testOutput = '(?i)\\(uevr-test-data|standalone-host-tests|runtime-host-test-data|core-discovery-[^\\]*|[^\\]*-\d+-\d+)\\'
$testFingerprint = Get-NativeTestFingerprint $nativeBuild @((Join-Path $nativeSource 'tests'), (Join-Path $nativeSource 'assets'), (Join-Path $nativeBuild 'CTestTestfile.cmake')) $testOutput
if (Test-NativeTestsCurrent $nativeBuild $testFingerprint) {
    if ($Component -eq 'cheeky') { . (Join-Path $PSScriptRoot 'build-stamp.ps1'); Save-BuildStamp 'cheeky' }
    return
}
# ctest runs directly, not through msvc.cmd: cmd would split the exclusion pattern at its '|'.
$ctest = (& $nativeWrapper where ctest | Select-Object -First 1)
if (-not $ctest) { throw 'ctest not found.' }
& $ctest @testOptions
if ($LASTEXITCODE -ne 0) { throw 'Native tests failed.' }
if ($Component -eq 'cheeky') {
    & $nativeWrapper ctest --test-dir $nativeBuild --output-on-failure --parallel 1 -R MixedCalibration
    if ($LASTEXITCODE -ne 0) { throw 'Mixed calibration tests failed.' }
}
Save-NativeTestsPassed $nativeBuild $testFingerprint
# Built and tested: build-release.ps1 ships the Cheeky DLLs only while their sources and patches match this stamp.
if ($Component -eq 'cheeky') { . (Join-Path $PSScriptRoot 'build-stamp.ps1'); Save-BuildStamp 'cheeky' }
