#Requires -Version 7
param([ValidateSet('ofxr-core','cheeky')][string]$Component = 'ofxr-core')
$ErrorActionPreference = 'Stop'
$workspaceRoot = Split-Path -Parent $PSScriptRoot
$nativeWrapper = Join-Path $PSScriptRoot 'msvc.cmd'
$nativeSource = Join-Path $workspaceRoot ('external/' + $(if ($Component -eq 'ofxr-core') {'ofxr'} else {'cheeky'}))
$nativeBuild = Join-Path $workspaceRoot ('artifacts/native/' + $Component)
$nativeOptions = @('-DCMAKE_BUILD_TYPE=Release')
if ($Component -eq 'cheeky') {
    & python (Join-Path $PSScriptRoot 'patch-cheeky.py')
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
    $testText = [IO.File]::ReadAllText($testFile).Replace('CheekyMixedCalibrationTests]=] PROPERTIES  TIMEOUT "60"', 'CheekyMixedCalibrationTests]=] PROPERTIES  TIMEOUT "300"')
    [IO.File]::WriteAllText($testFile, $testText)
}
& $nativeWrapper ctest @testOptions
if ($LASTEXITCODE -ne 0) { throw 'Native tests failed.' }
if ($Component -eq 'cheeky') {
    & $nativeWrapper ctest --test-dir $nativeBuild --output-on-failure --parallel 1 -R MixedCalibration
    if ($LASTEXITCODE -ne 0) { throw 'Mixed calibration tests failed.' }
}
# Built and tested: build-release.ps1 ships the Cheeky DLLs only while their sources and patches match this stamp.
if ($Component -eq 'cheeky') { . (Join-Path $PSScriptRoot 'build-stamp.ps1'); Save-BuildStamp 'cheeky' }
