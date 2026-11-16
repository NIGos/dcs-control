#Requires -Version 7
# -LocalOnly '<why>': a build for this machine only. When the second test phase does not pass (a pacing test fails 3
# times, or DCSC_SKIP_TIMING_TESTS=1 skips the phase) the build ends with a local-only stamp carrying <why> and the
# failed tests, instead of stopping without one. build-release.ps1 refuses a local-only stamp (scripts/build-stamp.ps1),
# so nothing built this way can be published, and no stamp has to be written by hand. The first phase must still pass.
# When everything passes the stamp is a full one, -LocalOnly or not.
param([string]$LocalOnly)
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
# Every test once, except the timing group below (it runs alone in a second phase), the tray tests (they create a
# notification icon) and the benches (label bench: GPU times printed, nothing required; `ctest -L bench` runs them).
# The quotes keep the '|' from cmd: msvc.cmd passes its arguments on through %*.
# $pacingTests only says which tests run alone, one at a time, in the second phase. It is a name match and it is wide
# on purpose: correctness tests were named into it so they run without a neighbour (two_layers_deep_hold_*,
# async_arm_warm_private_images*). Being in this group does NOT make a test retryable: see $retryablePacingTests.
$pacingTests = 'triple_deep|scene_change|adaptive|deep_hold|async_arm|pipelined_wait'
& $wrapper ctest --test-dir $forkBuild --output-on-failure --parallel 2 --timeout 300 -LE bench -E "`"tray_lifecycle|$pacingTests`""
if ($LASTEXITCODE -ne 0) { throw 'OFXR fork tests failed.' }
. (Join-Path $PSScriptRoot 'build-stamp.ps1')
if ($env:DCSC_SKIP_TIMING_TESTS -eq '1') {
    # An intermediate build while a game is running (the pacing tests need a quiet CPU): no stamp, so
    # build-release.ps1 refuses this DLL until a build with the pacing tests has passed.
    Write-Host 'Pacing tests skipped (DCSC_SKIP_TIMING_TESTS=1): this build is not stamped for a release.'
    if ($LocalOnly) { Save-BuildStamp 'ofxr' -LocalOnly "$LocalOnly (pacing tests skipped)" }
    else { Remove-Item -LiteralPath (Get-BuildStampPath 'ofxr') -Force -ErrorAction SilentlyContinue }
    return
}
# The second phase. These tests run the layer on the WARP software device against a fake runtime with a 10 ms frame
# period (tests/layer_call_chain.cpp), so what makes one miss a deadline is CPU contention on this machine (another
# build, a game's own threads), not GPU load. A test of the retry set that fails runs again, up to 3 times in all; a
# real regression fails every time.
# The retry set is explicit and narrow: only tests whose verdict is a pacing measure (deadlines, missed slots, the
# time of a 408) and that the retry log has shown to be load-sensitive. Whole names, anchored. Everything else that
# runs in this phase fails the build at its first failure.
$retryablePacingTests = @(
    '^xrfg_layer_adaptive_cadence(_[a-z0-9_]+)?$'      # adaptive cadence: exit, jitter, stall, idle, reidle, pause, close, the #23 cases
    '^xrfg_layer_pipelined_deep_hold(_off)?$'          # the 2X hold on a pipelined presenter
    '^xrfg_layer_scene_change_(triple|adaptive)$'      # cadence across a scene change
)
# Never retried, whatever the list above becomes: the two-layer tests check which image, role and alpha a layer names
# (two_layers_deep_hold_arming_single is the 0085 double arming, _roles is 0089, every two_layers_* run has the alpha
# checks) and warm_private_images checks a start-up record. An intermittent failure there is a race in the product,
# and a retry would pass it one run in three.
$neverRetriedTests = 'two_layers|warm_private_images'
function Test-PacingRetryAllowed([string]$Name) {
    if ($Name -match $neverRetriedTests) { return $false }
    foreach ($pattern in $retryablePacingTests) { if ($Name -match $pattern) { return $true } }
    return $false
}
# The line that says why a test failed, from ctest's output (--output-on-failure): the message of the first
# "CMake Error" after the test's result line, else the first line of its output, else the result line itself.
function Get-CtestFailureLine([string[]]$Output, [string]$Name) {
    $escaped = [regex]::Escape($Name)
    $resultAt = -1
    for ($i = 0; $i -lt $Output.Count; $i++) {
        if ($Output[$i] -match "Test\s+#\d+:\s+$escaped\s" -and $Output[$i] -notmatch '\sPassed\s') { $resultAt = $i; break }
    }
    if ($resultAt -lt 0) { return 'no result line in the ctest output' }
    $own = @(for ($i = $resultAt + 1; $i -lt $Output.Count; $i++) {
        if ($Output[$i] -match '^\s*(Start\s+\d+:|\d+/\d+\s+Test\s+#\d+:|\d+% tests passed)') { break }
        $Output[$i]
    })
    for ($i = 0; $i -lt $own.Count - 1; $i++) {
        if ($own[$i] -match '^CMake Error') {
            $message = @($own[($i + 1)..($own.Count - 1)] | Where-Object { $_ -and $_.Trim() }) | Select-Object -First 1
            if ($message) { return $message.Trim() }
        }
    }
    $first = @($own | Where-Object { $_ -and $_.Trim() }) | Select-Object -First 1
    if ($first) { return $first.Trim() }
    return ($Output[$resultAt] -replace '^\s*\d+/\d+\s+', '').Trim()
}
# Attempts 2 and 3 for the tests that failed attempt 1. Every attempt, the first included, is logged (date, test,
# attempt, result, the failure line) in $LogPath. $RunOne runs one test by name and returns ExitCode and Output.
# Returns one record per failed test: Name, Retryable, PassedAt (0 = never), Attempts.
function Invoke-PacingRetries([string[]]$FailedTests, [string[]]$FirstOutput, [scriptblock]$RunOne, [string]$LogPath) {
    $log = {
        param([string]$Test, [string]$Text)
        Add-Content -LiteralPath $LogPath -Encoding utf8 -Value ('{0} {1} {2}' -f (Get-Date -Format 'yyyy-MM-dd HH:mm:ss'), $Test, $Text)
    }
    foreach ($failedTest in $FailedTests) {
        $reason = Get-CtestFailureLine $FirstOutput $failedTest
        if (-not (Test-PacingRetryAllowed $failedTest)) {
            & $log $failedTest "attempt 1 failed, not retried (outside the retry set): $reason"
            [pscustomobject]@{ Name = $failedTest; Retryable = $false; PassedAt = 0; Attempts = 1 }
            continue
        }
        & $log $failedTest "attempt 1 failed: $reason"
        $passedAt = 0
        $attempt = 1
        while ($attempt -lt 3 -and -not $passedAt) {
            $attempt++
            $run = & $RunOne $failedTest
            if ($run.ExitCode -eq 0) {
                $passedAt = $attempt
                & $log $failedTest "attempt $attempt passed"
            } else {
                & $log $failedTest "attempt $attempt failed: $(Get-CtestFailureLine @($run.Output) $failedTest)"
            }
        }
        [pscustomobject]@{ Name = $failedTest; Retryable = $true; PassedAt = $passedAt; Attempts = $attempt }
    }
}
$firstOutput = @(& $wrapper ctest --test-dir $forkBuild --output-on-failure --parallel 1 --timeout 300 -LE bench -R "`"$pacingTests`"" | ForEach-Object { Write-Host $_; $_ })
if ($LASTEXITCODE -ne 0) {
    $failedList = Join-Path $forkBuild 'Testing/Temporary/LastTestsFailed.log'
    $failedTests = @(if (Test-Path -LiteralPath $failedList) {
        Get-Content -LiteralPath $failedList | ForEach-Object { ($_ -split ':', 2)[-1].Trim() } | Where-Object { $_ }
    })
    if ($failedTests.Count -eq 0) { throw 'OFXR fork pacing tests failed (no failed test listed).' }
    $retryLog = Join-Path $workspaceRoot 'artifacts/native/ofxr-djules75-retries.log'
    # --no-tests=error: a name that matches no test is a failure, not an empty run that exits 0.
    $runOne = {
        param([string]$name)
        $lines = @(& $wrapper ctest --test-dir $forkBuild --output-on-failure --timeout 300 --no-tests=error -R "`"^$name$`"" | ForEach-Object { Write-Host $_; $_ })
        [pscustomobject]@{ ExitCode = $LASTEXITCODE; Output = $lines }
    }
    $retried = @(Invoke-PacingRetries $failedTests $firstOutput $runOne $retryLog)
    Write-Host ''
    Write-Host "OFXR fork pacing tests, second phase: $($retried.Count) failed their first attempt (log: artifacts/native/ofxr-djules75-retries.log)."
    foreach ($entry in $retried) {
        $outcome = if (-not $entry.Retryable) { 'NOT RETRIED (outside the retry set): failed' }
            elseif ($entry.PassedAt) { "needed a retry: passed at attempt $($entry.PassedAt) of 3" }
            else { 'FAILED 3 times' }
        Write-Host "  $($entry.Name): $outcome"
    }
    $broken = @($retried | Where-Object { -not $_.PassedAt })
    if ($broken.Count) {
        $brokenNames = ($broken | ForEach-Object Name) -join ', '
        if (-not $LocalOnly) { throw "OFXR fork tests failed in the second phase: $brokenNames." }
        Add-Content -LiteralPath $retryLog -Encoding utf8 -Value ('{0} local-only stamp written by the build script: {1} (failed: {2}); not for publishing' -f
            (Get-Date -Format 'yyyy-MM-dd HH:mm:ss'), $LocalOnly, $brokenNames)
        Save-BuildStamp 'ofxr' -LocalOnly "$LocalOnly (failed: $brokenNames)"
        return
    }
} else {
    Write-Host 'OFXR fork pacing tests, second phase: all passed at the first attempt, nothing retried.'
}
# Built and tested: build-release.ps1 ships this DLL only while its sources and patches match this stamp.
Save-BuildStamp 'ofxr'
