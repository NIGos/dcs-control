#Requires -Version 7
# -Force: patch-cheeky.py discards local edits in external/cheeky instead of refusing to reset them.
param([ValidateSet('ofxr-core','cheeky')][string]$Component = 'ofxr-core', [switch]$Force)
$ErrorActionPreference = 'Stop'
$workspaceRoot = Split-Path -Parent $PSScriptRoot
$nativeWrapper = Join-Path $PSScriptRoot 'msvc.cmd'
$nativeSource = Join-Path $workspaceRoot ('external/' + $(if ($Component -eq 'ofxr-core') {'ofxr'} else {'cheeky'}))
$nativeBuild = Join-Path $workspaceRoot ('artifacts/native/' + $Component)
$nativeOptions = @('-DCMAKE_BUILD_TYPE=Release')
if ($Component -eq 'cheeky') {
    $patchOptions = @(if ($Force) { '--force' })
    & python (Join-Path $PSScriptRoot 'patch-cheeky.py') @patchOptions
    if ($LASTEXITCODE -ne 0) { throw 'Cheeky adaptation failed.' }
}
if ($Component -eq 'ofxr-core') { $nativeOptions += @('-DXRFG_BUILD_LAYER=OFF','-DXRFG_BUILD_STANDALONE=OFF') }
& $nativeWrapper cmake -S $nativeSource -B $nativeBuild -G Ninja @nativeOptions
if ($LASTEXITCODE -ne 0) { throw 'Native CMake configuration failed.' }
& $nativeWrapper cmake --build $nativeBuild --parallel 4
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
    $timeoutAnchor = [regex]'(CheekyMixedCalibrationTests\]=\] PROPERTIES  TIMEOUT )"\d+"'
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
# ctest runs directly, not through msvc.cmd: cmd would split the exclusion pattern at its '|'.
$ctest = (& $nativeWrapper where ctest | Select-Object -First 1)
if (-not $ctest) { throw 'ctest not found.' }
& $ctest @testOptions
if ($LASTEXITCODE -ne 0) { throw 'Native tests failed.' }
if ($Component -eq 'cheeky') {
    & $nativeWrapper ctest --test-dir $nativeBuild --output-on-failure --parallel 1 -R MixedCalibration
    if ($LASTEXITCODE -ne 0) { throw 'Mixed calibration tests failed.' }
}
# Built and tested: build-release.ps1 ships the Cheeky DLLs only while their sources and patches match this stamp.
if ($Component -eq 'cheeky') { . (Join-Path $PSScriptRoot 'build-stamp.ps1'); Save-BuildStamp 'cheeky' }
