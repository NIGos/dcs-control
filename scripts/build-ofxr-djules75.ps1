#Requires -Version 7
$ErrorActionPreference = 'Stop'
$workspaceRoot = Split-Path -Parent $PSScriptRoot
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
# A string patch to a pinned SDK file: the anchor is replaced, or the replacement is already there (an earlier run).
# Neither means the file is not the one pinned, and the build stops instead of building it unpatched.
function Update-PinnedText([string]$text, [string]$anchor, [string]$replacement, [string]$file) {
    if ($text.Contains($anchor)) { return $text.Replace($anchor, $replacement) }
    if ($text.Contains($replacement)) { return $text }
    throw "Pinned SDK file $file changed: '$anchor' not found."
}
$pixCmakePath = Join-Path $sdkRoot 'sdk/libs/pix/CMakeLists.txt'
$pixCmake = [IO.File]::ReadAllText($pixCmakePath)
[IO.File]::WriteAllText($pixCmakePath, (Update-PinnedText $pixCmake '${CMAKE_GENERATOR_PLATFORM}' '${CMAKE_VS_PLATFORM_NAME}' $pixCmakePath))
$sdkBuild = Join-Path $workspaceRoot 'artifacts/native/fidelityfx'
# SDK 1.1.4 appends output paths to the output-variable name itself. Ninja
# correctly rejects the resulting literal dependency. Preserve the name and
# accumulate paths separately (no shader or runtime behavior changes).
$compilePath = Join-Path $sdkRoot 'sdk/include/FidelityFX/gpu/CMakeCompileShaders.txt'
$compileText = [IO.File]::ReadAllText($compilePath)
$compileText = Update-PinnedText $compileText 'list(APPEND PERMUTATION_OUTPUTS ' 'list(APPEND LOCAL_PERMUTATION_OUTPUTS ' $compilePath
$compileText = Update-PinnedText $compileText 'set(${PERMUTATION_OUTPUTS} ${PERMUTATION_OUTPUTS} PARENT_SCOPE)' 'set(${PERMUTATION_OUTPUTS} ${LOCAL_PERMUTATION_OUTPUTS} PARENT_SCOPE)' $compilePath
[IO.File]::WriteAllText($compilePath, $compileText)
& $wrapper cmake -S "$sdkRoot/sdk" -B $sdkBuild -G Ninja -DCMAKE_BUILD_TYPE=Release -DFFX_ALL=OFF -DFFX_OF=ON -DFFX_API_BACKEND=DX12_X64 -DFFX_BUILD_AS_DLL=OFF
if ($LASTEXITCODE -ne 0) { throw 'FidelityFX configuration failed.' }
& $wrapper cmake --build $sdkBuild --parallel 4
if ($LASTEXITCODE -ne 0) { throw 'FidelityFX build failed.' }
# --- djules75 fork of OFXR Bridge (DCS-capable since 0.2.6), pinned to tag 0.2.9.1 ---
$forkRoot = Join-Path $workspaceRoot 'external/ofxr-djules75'
$forkTag = '0.2.9.1'
$forkCommit = '8cdee48ec97afc41cff0a48c68a44566643c9943'
if (-not (Test-Path -LiteralPath $forkRoot)) {
    & git clone --branch $forkTag --depth 1 https://github.com/djules75/OFXR-Bridge.git $forkRoot
    if ($LASTEXITCODE -ne 0) { throw 'OFXR fork download failed.' }
}
$forkRevision = & git -C $forkRoot rev-parse HEAD
if ($forkRevision -ne $forkCommit) { throw "Unexpected OFXR fork revision $forkRevision (expected $forkCommit for $forkTag)." }
# The fork's layer needs openvr.h (FPS overlay reads SteamVR delivery stats). Same pin as its CI.
$openvrHeader = Join-Path $forkRoot 'external/openvr/openvr.h'
$openvrSha256 = '94E5545370159C85F87CD6E15DD3739F7C919FC7A6E869F5E4ED463533A07ED0'
if (-not (Test-Path -LiteralPath $openvrHeader)) {
    New-Item -ItemType Directory -Force (Split-Path -Parent $openvrHeader) | Out-Null
    & curl.exe -sSL -o $openvrHeader 'https://raw.githubusercontent.com/ValveSoftware/openvr/v2.5.1/headers/openvr.h'
    if ($LASTEXITCODE -ne 0) { throw 'openvr.h download failed.' }
}
if ((Get-FileHash -LiteralPath $openvrHeader -Algorithm SHA256).Hash -ne $openvrSha256) { throw 'openvr.h hash mismatch.' }
# The fork fixes the DCS second-thread xrWaitFrame deadlock itself, and defer_until_submitted is not
# ported. DCS Control's own changes (the in-headset diagnostic panel) are kept as source patches in
# patches/ofxr-djules75 (LGPL: the corresponding-source package carries them) and applied here in name
# order to the pinned checkout. A patch that is already applied is left as it is, so re-running is safe;
# a checkout that matches neither state stops the build. Restore a clean checkout with
# `git -C external/ofxr-djules75 checkout -- .` plus `git -C external/ofxr-djules75 clean -n` (review, then -f).
$forkPatches = @(Get-ChildItem -LiteralPath (Join-Path $workspaceRoot 'patches/ofxr-djules75') -Filter '*.patch' -File -ErrorAction SilentlyContinue | Sort-Object Name)
if ($forkPatches.Count -eq 0) { throw 'No OFXR fork patches found in patches/ofxr-djules75: the build would ship the fork without DCS Control''s changes.' }
function Test-ForkPatch([string]$patchFile, [switch]$Reverse) {
    $savedPreference = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $arguments = @('-C', $forkRoot, 'apply', '--check', '--whitespace=nowarn')
        if ($Reverse) { $arguments += '--reverse' }
        & git @arguments $patchFile 2>&1 | Out-Null
        return $LASTEXITCODE -eq 0
    } finally { $ErrorActionPreference = $savedPreference }
}
# Patches are LF; a checkout with core.autocrlf may have turned them into CRLF.
$normalizedPatches = @($forkPatches | ForEach-Object {
    $temp = [IO.Path]::GetTempFileName()
    [IO.File]::WriteAllText($temp, [IO.File]::ReadAllText($_.FullName).Replace("`r`n", "`n"))
    [pscustomobject]@{ Name = $_.Name; FullName = $temp }
})
# The patch set applied last is recorded in the fork's .git folder. When any patch changed since (an earlier one edited,
# for example), the checkout goes back to the pinned sources and every patch is applied again: finding only the newest
# applied patch would take an edited earlier one as applied and build stale code. Ignored files (external/openvr) stay.
$patchSha = [Security.Cryptography.SHA256]::Create()
$patchSet = [Convert]::ToHexString($patchSha.ComputeHash([Text.Encoding]::UTF8.GetBytes((($normalizedPatches | ForEach-Object { $_.Name + "`n" + [IO.File]::ReadAllText($_.FullName) }) -join "`n"))))
$patchMarker = Join-Path (& git -C $forkRoot rev-parse --absolute-git-dir) 'dcs-control-patches'
$recordedSet = if (Test-Path -LiteralPath $patchMarker) { (Get-Content -LiteralPath $patchMarker -Raw).Trim() } else { $null }
if ($recordedSet -and $recordedSet -ne $patchSet) {
    Write-Host 'OFXR fork patches changed since they were applied: starting again from the pinned sources.'
    & git -C $forkRoot checkout -- .
    if ($LASTEXITCODE -ne 0) { throw 'OFXR fork source reset failed.' }
    & git -C $forkRoot clean -fdq
    if ($LASTEXITCODE -ne 0) { throw 'OFXR fork source reset failed.' }
}
try {
    # A later patch may edit files an earlier one created, so earlier patches no longer reverse-apply on a fully
    # patched tree. Find the newest applied patch first; everything before it counts as applied.
    $appliedThrough = -1
    for ($i = $normalizedPatches.Count - 1; $i -ge 0; $i--) {
        if (Test-ForkPatch $normalizedPatches[$i].FullName -Reverse) { $appliedThrough = $i; break }
    }
    for ($i = 0; $i -lt $normalizedPatches.Count; $i++) {
        $forkPatch = $normalizedPatches[$i]
        if ($i -le $appliedThrough) { Write-Host "OFXR fork patch $($forkPatch.Name) already applied."; continue }
        if (-not (Test-ForkPatch $forkPatch.FullName)) { throw "OFXR fork patch $($forkPatch.Name) neither applies to nor is applied in $forkRoot; restore a clean checkout." }
        & git -C $forkRoot apply --whitespace=nowarn $forkPatch.FullName
        if ($LASTEXITCODE -ne 0) { throw "OFXR fork patch $($forkPatch.Name) failed to apply." }
        Write-Host "Applied OFXR fork patch $($forkPatch.Name)."
    }
    Set-Content -LiteralPath $patchMarker -Value $patchSet -Encoding ascii
} finally { $normalizedPatches | ForEach-Object { Remove-Item -LiteralPath $_.FullName -Force -ErrorAction SilentlyContinue } }
# The shared-memory contract headers the patches put in the fork must match their canonical copies: a drifted copy
# stops the build here rather than shipping a layer that reads the engine module's blocks differently.
& (Join-Path $PSScriptRoot 'check-contracts.ps1')
$forkBuild = Join-Path $workspaceRoot 'artifacts/native/ofxr-djules75'
& $wrapper cmake -S $forkRoot -B $forkBuild -G Ninja -DCMAKE_BUILD_TYPE=Release "-DXRFG_FIDELITYFX_SDK_ROOT=$sdkRoot/sdk"
if ($LASTEXITCODE -ne 0) { throw 'OFXR fork configuration failed.' }
& $wrapper cmake --build $forkBuild --parallel 4
if ($LASTEXITCODE -ne 0) { throw 'OFXR fork build failed.' }
# Tray lifecycle tests create a notification icon. Exclude them from offline verification.
# Every test once, except the pacing tests (real frame timing), the tray tests (they create a notification icon) and
# the benches (label bench: GPU times printed, nothing required; `ctest -L bench` runs them).
# The quotes keep the '|' from cmd: msvc.cmd passes its arguments on through %*.
$pacingTests = 'triple_deep|scene_change|adaptive|deep_hold|async_arm|pipelined_wait'
& $wrapper ctest --test-dir $forkBuild --output-on-failure --parallel 2 --timeout 300 -LE bench -E "`"tray_lifecycle|$pacingTests`""
if ($LASTEXITCODE -ne 0) { throw 'OFXR fork tests failed.' }
. (Join-Path $PSScriptRoot 'build-stamp.ps1')
if ($env:DCSC_SKIP_TIMING_TESTS -eq '1') {
    # An intermediate build while a game holds the GPU: no stamp, so build-release.ps1 refuses this DLL until a build
    # with the pacing tests has passed.
    Write-Host 'Pacing tests skipped (DCSC_SKIP_TIMING_TESTS=1): this build is not stamped for a release.'
    Remove-Item -LiteralPath (Get-BuildStampPath 'ofxr') -Force -ErrorAction SilentlyContinue
    return
}
# The pacing tests: on a busy GPU one misses a deadline now and then, so a failing one runs again, up to 3 times in
# all; a real regression fails every time. Each retry is logged (date, test, attempt, result) in
# artifacts/native/ofxr-djules75-retries.log, so a test that keeps needing them shows up.
& $wrapper ctest --test-dir $forkBuild --output-on-failure --parallel 1 --timeout 300 -LE bench -R "`"$pacingTests`""
if ($LASTEXITCODE -ne 0) {
    $failedList = Join-Path $forkBuild 'Testing/Temporary/LastTestsFailed.log'
    $failedTests = @(if (Test-Path -LiteralPath $failedList) {
        Get-Content -LiteralPath $failedList | ForEach-Object { ($_ -split ':', 2)[-1].Trim() } | Where-Object { $_ }
    })
    if ($failedTests.Count -eq 0) { throw 'OFXR fork pacing tests failed (no failed test listed).' }
    $retryLog = Join-Path $workspaceRoot 'artifacts/native/ofxr-djules75-retries.log'
    foreach ($failedTest in $failedTests) {
        $passed = $false
        for ($attempt = 2; $attempt -le 3 -and -not $passed; $attempt++) {
            & $wrapper ctest --test-dir $forkBuild --output-on-failure --timeout 300 -R "`"^$failedTest$`""
            $passed = $LASTEXITCODE -eq 0
            Add-Content -LiteralPath $retryLog -Encoding utf8 -Value ('{0} {1} attempt {2} {3}' -f
                (Get-Date -Format 'yyyy-MM-dd HH:mm:ss'), $failedTest, $attempt, $(if ($passed) { 'passed' } else { 'failed' }))
        }
        if (-not $passed) { throw "OFXR fork pacing test $failedTest failed 3 times." }
    }
}
# Built and tested: build-release.ps1 ships this DLL only while its sources and patches match this stamp.
Save-BuildStamp 'ofxr'
