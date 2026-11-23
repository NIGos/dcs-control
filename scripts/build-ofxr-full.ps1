$ErrorActionPreference = 'Stop'
$workspaceRoot = Split-Path -Parent $PSScriptRoot
. (Join-Path $PSScriptRoot 'build-jobs.ps1')  # Get-BuildJobs: the compile jobs at once
$sdkRoot = Join-Path $workspaceRoot 'external/ofxr/external/FidelityFX-SDK-v1.1.4'
$wrapper = Join-Path $PSScriptRoot 'msvc.cmd'
if (-not (Test-Path -LiteralPath $sdkRoot)) {
    & git clone --depth 1 --filter=blob:none --sparse --branch v1.1.4 https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK.git $sdkRoot
    if ($LASTEXITCODE -ne 0) { throw 'FidelityFX download failed.' }
    & git -C $sdkRoot sparse-checkout set sdk tools
    if ($LASTEXITCODE -ne 0) { throw 'FidelityFX sparse checkout failed.' }
}
$revision = & git -C $sdkRoot rev-parse HEAD
if ($revision -ne 'c6efa6bf7f2027b3ec94f28578bb5965eabb9e55') { throw 'Unexpected FidelityFX revision.' }
# The pinned SDK forces a Visual Studio platform even with Ninja. Keep its
# target architecture but avoid a generator platform that Ninja rejects.
$toolchainPath = Join-Path $sdkRoot 'sdk/toolchain.cmake'
$toolchainText = [IO.File]::ReadAllText($toolchainPath)
$originalPlatform = 'set(CMAKE_GENERATOR_PLATFORM "x64" CACHE STRING "" FORCE)'
$ninjaPlatform = @'
if(CMAKE_GENERATOR STREQUAL "Ninja")
        set(CMAKE_GENERATOR_PLATFORM "" CACHE STRING "" FORCE)
        set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} /machine:x64")
    else()
        set(CMAKE_GENERATOR_PLATFORM "x64" CACHE STRING "" FORCE)
    endif()
'@
if (-not $toolchainText.Contains('/machine:x64")')) {
    if (-not $toolchainText.Contains($originalPlatform)) { throw 'Pinned SDK toolchain layout changed.' }
    [IO.File]::WriteAllText($toolchainPath, $toolchainText.Replace($originalPlatform, $ninjaPlatform))
}
$pixCmakePath = Join-Path $sdkRoot 'sdk/libs/pix/CMakeLists.txt'
$pixCmake = [IO.File]::ReadAllText($pixCmakePath)
[IO.File]::WriteAllText($pixCmakePath, $pixCmake.Replace('${CMAKE_GENERATOR_PLATFORM}', '${CMAKE_VS_PLATFORM_NAME}'))
$sdkBuild = Join-Path $workspaceRoot 'artifacts/native/fidelityfx'
# SDK 1.1.4 appends output paths to the output-variable name itself. Ninja
# correctly rejects the resulting literal dependency. Preserve the name and
# accumulate paths separately (no shader or runtime behavior changes).
$compilePath = Join-Path $sdkRoot 'sdk/include/FidelityFX/gpu/CMakeCompileShaders.txt'
$compileText = [IO.File]::ReadAllText($compilePath)
$compileText = $compileText.Replace('list(APPEND PERMUTATION_OUTPUTS ', 'list(APPEND LOCAL_PERMUTATION_OUTPUTS ')
$compileText = $compileText.Replace('set(${PERMUTATION_OUTPUTS} ${PERMUTATION_OUTPUTS} PARENT_SCOPE)', 'set(${PERMUTATION_OUTPUTS} ${LOCAL_PERMUTATION_OUTPUTS} PARENT_SCOPE)')
[IO.File]::WriteAllText($compilePath, $compileText)
& $wrapper cmake -S "$sdkRoot/sdk" -B $sdkBuild -G Ninja -DCMAKE_BUILD_TYPE=Release -DFFX_ALL=OFF -DFFX_OF=ON -DFFX_API_BACKEND=DX12_X64 -DFFX_BUILD_AS_DLL=OFF
if ($LASTEXITCODE -ne 0) { throw 'FidelityFX configuration failed.' }
& $wrapper cmake --build $sdkBuild --parallel (Get-BuildJobs)
if ($LASTEXITCODE -ne 0) { throw 'FidelityFX build failed.' }
$ofxrBuild = Join-Path $workspaceRoot 'artifacts/native/ofxr-full'
# Deferred private swapchains for layers that composite their own views (Quad Views). Idempotent.
$ofxrPatch = Join-Path $workspaceRoot 'patches/ofxr/defer-until-submitted.patch'
& git -C "$workspaceRoot/external/ofxr" apply --check --reverse $ofxrPatch 2>$null
if ($LASTEXITCODE -ne 0) {
    & git -C "$workspaceRoot/external/ofxr" apply $ofxrPatch
    if ($LASTEXITCODE -ne 0) { throw 'OFXR deferred-swapchain patch failed.' }
}
& $wrapper cmake -S "$workspaceRoot/external/ofxr" -B $ofxrBuild -G Ninja -DCMAKE_BUILD_TYPE=Release
if ($LASTEXITCODE -ne 0) { throw 'OFXR configuration failed.' }
& $wrapper cmake --build $ofxrBuild --parallel (Get-BuildJobs)
if ($LASTEXITCODE -ne 0) { throw 'OFXR build failed.' }
# Tray lifecycle tests create a notification icon. Exclude them from offline verification.
& $wrapper ctest --test-dir $ofxrBuild --output-on-failure --parallel 2 -E tray_lifecycle
if ($LASTEXITCODE -ne 0) { throw 'OFXR tests failed.' }
