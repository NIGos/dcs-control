# DCS Control: builds and runs the offline Far depth checks (tests/depthwarp/depthwarp_test.cpp) against the shaders in
# native/quadviews and the Quad Views composition shaders (external/quadviews, patched by build-quadviews.ps1). Nothing
# is installed. After build-quadviews.ps1 has built Far depth into the layer, the test checks the layer's ProjectionPS is
# exactly its own injection into the shader the build kept from before (ProjectionPS.nofardepth.hlsl).
#
#   scripts/build-depthwarp-test.ps1 [-Warp] [-Quick] [-Perf] [-NoRun]
#
# -Quick halves the test images. -Perf asserts the GPU cost limits; without it they are only reported, since timings
# on a busy GPU (DCS running) say nothing about the code.
param(
    [switch]$Warp,
    [switch]$Quick,
    [switch]$Perf,
    [switch]$NoRun
)
$ErrorActionPreference = 'Stop'
$workspaceRoot = Split-Path -Parent $PSScriptRoot
$native = Join-Path $workspaceRoot 'native/quadviews'
$layer = Join-Path $workspaceRoot 'external/quadviews/openxr-api-layer'
if (-not (Test-Path (Join-Path $layer 'ProjectionPS.hlsl'))) {
    throw "Quad Views sources not found at ${layer}: run scripts/build-quadviews.ps1 first."
}
$outDir = Join-Path $workspaceRoot 'artifacts/native/depthwarp-test'
New-Item -ItemType Directory -Force $outDir | Out-Null
$exe = Join-Path $outDir 'depthwarp_test.exe'
& (Join-Path $PSScriptRoot 'msvc.cmd') cl /nologo /std:c++20 /EHsc /O2 /W4 "/Fe:$exe" "/Fo:$outDir\" (Join-Path $workspaceRoot 'tests/depthwarp/depthwarp_test.cpp') "/I$native" d3d11.lib d3dcompiler.lib dxgi.lib
if ($LASTEXITCODE -ne 0) { throw "depthwarp_test build failed ($LASTEXITCODE)" }
if ($NoRun) { return }
$testArgs = @($native, $layer)
if ($Warp) { $testArgs += '--warp' }
if ($Quick) { $testArgs += '--quick' }
if ($Perf) { $testArgs += '--perf' }
& $exe @testArgs
if ($LASTEXITCODE -ne 0) { throw "depthwarp_test failed ($LASTEXITCODE)" }
